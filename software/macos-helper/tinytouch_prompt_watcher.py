#!/usr/bin/env python3
"""Light the tinyTouch LED when macOS asks for the PIV PIN.

In PIV mode nothing reaches the card between the moment macOS displays its PIN
prompt and the moment the PIN is submitted, so the firmware cannot know that a
touch is expected. This watcher observes the unified log instead and drives the
LED over the CDC console.

The design follows yknotify (https://github.com/noperator/yknotify), which
solves the equivalent problem for YubiKey touches: stream `log` as NDJSON,
filter with an NSPredicate, keep a small state machine, debounce the output.
One difference matters: yknotify needs `--level debug` because its IOHIDFamily
markers are debug level, which costs around 11% CPU. Every marker used here is
Info or Default level, so `--level info` is enough and costs approximately
nothing.

See docs/piv-prompt-feedback.md for the measurements behind the markers.
"""

from __future__ import annotations

import argparse
import json
import logging
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from enum import Enum
from typing import Iterator

import serial
import serial.tools.list_ports

LOGGER = logging.getLogger("tinytouch.prompt_watcher")

# Not derived from __doc__: the frozen CLI is built with --optimize 2, which
# strips docstrings, leaving __doc__ as None.
DESCRIPTION = "Light the tinyTouch LED when macOS asks for the PIV PIN."

LOG_BINARY = "/usr/bin/log"
LOG_LEVEL_ARGUMENT = "info"

SERIAL_BAUD_RATE = 115200
SERIAL_WRITE_TIMEOUT_SECONDS = 2.0
SERIAL_PORT_PREFIX = "/dev/cu.usbmodem"

LED_DEBOUNCE_SECONDS = 0.25
RECONNECT_DELAY_SECONDS = 1.0
SELF_TEST_WINDOW = "1h"

# A prompt marker is acted on only if nothing contradicts it within this delay.
# The same `Invoking SmartCard agent` line is emitted both when the prompt opens
# and when the PIN is submitted, so acting immediately produces a yellow flash a
# few hundred milliseconds before the result effect.
PROMPT_DEFER_SECONDS = 0.3
TICK_SECONDS = 0.05

# A successful unlock is followed by further token operations (the directory
# bind, the keychain unlock), each re-emitting the prompt marker. They must not
# relight the LED once the user is already through.
RESULT_COOLDOWN_SECONDS = 3.0

PROMPT_COMMAND = "LED PROMPT"
IDLE_COMMAND = "LED IDLE"

CTK_PROCESS = "ctkahp"
LOGIN_PROCESS = "loginwindow"

# Marker emitted by pam_smartcard's own prompt. It fires a second time when the
# PIN is submitted, in every case; the state machine tolerates the repeat.
SUDO_PROMPT_MARKER = "Invoking SmartCard agent for uid"

# Screen lock, screensaver and display-dim-to-lock all funnel through
# LWScreenLock. SecurityAgent check-in covers authorization dialogs.
# `startScreenLock:` alone logs about fifteen progress lines per lock; the
# `| entered` suffix isolates the single line that opens the sequence.
SCREEN_LOCK_MARKER = "startScreenLock:] | entered"
SECURITY_AGENT_MARKER = "Checked in app : SecurityAgent"

RESULT_MARKER = "Token login result"

# Fires one to two milliseconds after the submit-time repeat of the prompt
# marker, which is what makes the two occurrences distinguishable.
SUBMIT_MARKER = "TKGetSmartcardSetting checkCertificateTrust"

# Apple spells these two inconsistently: lowercase "c" on removal, uppercase on
# insertion. Both are matched verbatim rather than case-folded, so that a future
# rename is noticed as a lost marker instead of being silently absorbed.
CARD_REMOVED_MARKER = "Smartcards removed"
CARD_INSERTED_MARKER = "SmartCards inserted"

PREDICATE = (
    f'(process == "{CTK_PROCESS}" AND ('
    f'eventMessage CONTAINS "{SUDO_PROMPT_MARKER}" OR '
    f'eventMessage CONTAINS "{RESULT_MARKER}" OR '
    f'eventMessage CONTAINS "{SUBMIT_MARKER}" OR '
    f'eventMessage CONTAINS "{CARD_REMOVED_MARKER}" OR '
    f'eventMessage CONTAINS "{CARD_INSERTED_MARKER}")) OR '
    f'(process == "{LOGIN_PROCESS}" AND ('
    f'eventMessage CONTAINS "{SCREEN_LOCK_MARKER}" OR '
    f'eventMessage CONTAINS "{SECURITY_AGENT_MARKER}"))'
)


class EventKind(Enum):
    """Kinds of authentication event recognised in the unified log."""

    PROMPT_OPENED = "prompt_opened"
    PIN_SUBMITTED = "pin_submitted"
    RESULT = "result"
    CARD_REMOVED = "card_removed"
    CARD_INSERTED = "card_inserted"


@dataclass(frozen=True)
class PromptEvent:
    """A single recognised authentication event.

    Attributes:
        kind: What happened.
        source: Short label describing which marker matched.
        result_code: Token login result, present only for `EventKind.RESULT`.
    """

    kind: EventKind
    source: str
    result_code: int | None = None


class LogEventSource:
    """Streams the unified log and yields recognised authentication events."""

    def __init__(self, predicate: str = PREDICATE) -> None:
        """Initialise the source.

        Args:
            predicate: NSPredicate passed to `log stream`.
        """
        self._predicate = predicate

    def _spawn(self) -> subprocess.Popen[str]:
        """Start a `log stream` child process.

        Returns:
            The running process, with stdout piped as text.

        Raises:
            OSError: If the `log` binary cannot be executed.
        """
        command = [
            LOG_BINARY,
            "stream",
            "--level",
            LOG_LEVEL_ARGUMENT,
            "--style",
            "ndjson",
            "--predicate",
            self._predicate,
        ]
        LOGGER.debug("spawning: %s", " ".join(command))
        return subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )

    def events(self) -> Iterator[PromptEvent]:
        """Yield events forever, restarting the log stream if it dies.

        Yields:
            Recognised `PromptEvent` values.
        """
        while True:
            try:
                process = self._spawn()
            except OSError as error:
                LOGGER.error("could not start %s: %s", LOG_BINARY, error)
                time.sleep(RECONNECT_DELAY_SECONDS)
                continue

            try:
                if process.stdout is None:
                    raise RuntimeError("log stream produced no stdout pipe")
                for line in process.stdout:
                    event = parse_log_line(line)
                    if event is not None:
                        yield event
            except (OSError, RuntimeError) as error:
                LOGGER.error("log stream failed: %s", error)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=RECONNECT_DELAY_SECONDS)
                except subprocess.TimeoutExpired:
                    process.kill()

            LOGGER.warning("log stream ended, restarting")
            time.sleep(RECONNECT_DELAY_SECONDS)


def parse_log_line(line: str) -> PromptEvent | None:
    """Turn one NDJSON log line into an event.

    Args:
        line: Raw NDJSON emitted by `log stream` or `log show`.

    Returns:
        The recognised event, or None when the line matches nothing. An
        unrecognised line is never an error: if Apple renames a marker the
        watcher must go quiet, not misreport.
    """
    stripped = line.strip()
    if not stripped or stripped.startswith("["):
        return None
    try:
        entry = json.loads(stripped.rstrip(","))
    except json.JSONDecodeError:
        return None

    message = entry.get("eventMessage", "")
    # NDJSON output carries `processImagePath`, never the `process` key that the
    # predicate language uses, so the name has to come from the path's basename.
    process = entry.get("processImagePath", "").rsplit("/", 1)[-1]

    if RESULT_MARKER in message:
        return PromptEvent(EventKind.RESULT, "token_login", _result_code(message))
    if SUBMIT_MARKER in message:
        return PromptEvent(EventKind.PIN_SUBMITTED, "check_certificate_trust")
    if CARD_REMOVED_MARKER in message:
        return PromptEvent(EventKind.CARD_REMOVED, "card")
    if CARD_INSERTED_MARKER in message:
        return PromptEvent(EventKind.CARD_INSERTED, "card")
    if process == CTK_PROCESS and SUDO_PROMPT_MARKER in message:
        return PromptEvent(EventKind.PROMPT_OPENED, "pam_smartcard")
    if process == LOGIN_PROCESS and SCREEN_LOCK_MARKER in message:
        return PromptEvent(EventKind.PROMPT_OPENED, "screen_lock")
    if process == LOGIN_PROCESS and SECURITY_AGENT_MARKER in message:
        return PromptEvent(EventKind.PROMPT_OPENED, "security_agent")
    return None


def _result_code(message: str) -> int | None:
    """Extract the trailing integer of a `Token login result N` message.

    Args:
        message: The full log message.

    Returns:
        The parsed code, or None when the message does not end with an integer.
    """
    try:
        return int(message.rsplit(" ", 1)[1])
    except (IndexError, ValueError):
        return None


class LedController:
    """Sends LED state commands to the device over its CDC console."""

    def __init__(self, port: str | None = None, dry_run: bool = False) -> None:
        """Initialise the controller.

        Args:
            port: Serial device path, or None to auto-detect.
            dry_run: When true, log the commands instead of sending them.
        """
        self._requested_port = port
        self._dry_run = dry_run
        self._serial: serial.Serial | None = None
        self._lock = threading.Lock()
        self._last_command: str | None = None
        self._last_sent_at = 0.0

    def _resolve_port(self) -> str | None:
        """Find the device's serial port.

        Returns:
            The port path, or None when no candidate is present.
        """
        if self._requested_port:
            return self._requested_port
        candidates = sorted(
            port.device
            for port in serial.tools.list_ports.comports()
            if port.device.startswith(SERIAL_PORT_PREFIX)
        )
        return candidates[0] if candidates else None

    def _connection(self) -> serial.Serial | None:
        """Return an open serial connection, opening one if needed.

        Returns:
            The connection, or None when the device is absent or busy.
        """
        if self._serial is not None and self._serial.is_open:
            return self._serial
        port = self._resolve_port()
        if port is None:
            return None
        try:
            self._serial = serial.Serial(
                port,
                SERIAL_BAUD_RATE,
                timeout=0,
                write_timeout=SERIAL_WRITE_TIMEOUT_SECONDS,
            )
        except (OSError, serial.SerialException) as error:
            LOGGER.warning("could not open %s: %s", port, error)
            self._serial = None
            return None
        LOGGER.info("connected to %s", port)
        return self._serial

    def send(self, command: str) -> None:
        """Send a command, deduplicated and rate limited.

        Repeating the current state is skipped. A state change arriving too soon
        after the previous one waits for the rate limit rather than being
        dropped: losing a transition would strand the LED in the wrong colour.

        Args:
            command: Console command to send, without its terminator.
        """
        with self._lock:
            if command == self._last_command:
                return
            remaining = LED_DEBOUNCE_SECONDS - (time.monotonic() - self._last_sent_at)
            if remaining > 0:
                time.sleep(remaining)
            self._last_command = command
            self._last_sent_at = time.monotonic()

            if self._dry_run:
                LOGGER.info("[dry-run] %s", command)
                return

            connection = self._connection()
            if connection is None:
                return
            try:
                connection.write(f"{command}\n".encode("ascii"))
                connection.flush()
            except (OSError, serial.SerialException) as error:
                LOGGER.warning("write failed, dropping connection: %s", error)
                self.close()
                return
            LOGGER.debug("sent %s", command)

    def close(self) -> None:
        """Close the serial connection if one is open."""
        if self._serial is not None:
            try:
                self._serial.close()
            except (OSError, serial.SerialException):
                LOGGER.debug("ignoring error while closing the serial port")
            self._serial = None


class PromptWatcher:
    """Maps authentication events onto LED states.

    The prompt hold is owned by the firmware, which reverts to idle on its own
    after a fixed delay. This class therefore only decides when to raise the
    prompt and when to clear it early.
    """

    def __init__(self, leds: LedController) -> None:
        """Initialise the watcher.

        Args:
            leds: Controller used to drive the device.
        """
        self._leds = leds
        self._card_present = True
        self._pending_prompt_at = 0.0
        self._pending_source = ""
        self._cooldown_until = 0.0

    def handle(self, event: PromptEvent) -> None:
        """React to one event.

        Args:
            event: The event to process.
        """
        if event.kind is EventKind.CARD_REMOVED:
            self._card_present = False
            self._cancel_pending()
            LOGGER.info("card removed")
            return

        if event.kind is EventKind.CARD_INSERTED:
            self._card_present = True
            LOGGER.info("card inserted")
            return

        if event.kind is EventKind.PIN_SUBMITTED:
            # Proof that the repeat of the prompt marker just seen was a
            # submission, not a new prompt.
            LOGGER.debug("PIN submitted")
            self._cancel_pending()
            return

        if event.kind is EventKind.RESULT:
            LOGGER.info("authentication result %s", event.result_code)
            self._cancel_pending()
            self._cooldown_until = time.monotonic() + RESULT_COOLDOWN_SECONDS
            self._leds.send(IDLE_COMMAND)
            return

        now = time.monotonic()
        if not self._card_present:
            # The device is still re-enumerating after sleep. Claiming readiness
            # here would be a lie: no touch can succeed yet.
            LOGGER.debug("prompt from %s ignored, card absent", event.source)
            return
        if now < self._cooldown_until:
            LOGGER.debug("prompt from %s ignored, result cooldown", event.source)
            return

        self._pending_prompt_at = now + PROMPT_DEFER_SECONDS
        self._pending_source = event.source

    def tick(self) -> None:
        """Fire a deferred prompt once nothing has contradicted it.

        Called on a timer so that state advances without new log traffic.
        """
        if not self._pending_prompt_at:
            return
        if time.monotonic() < self._pending_prompt_at:
            return
        source = self._pending_source
        self._cancel_pending()
        LOGGER.info("prompt opened (%s)", source)
        self._leds.send(PROMPT_COMMAND)

    def _cancel_pending(self) -> None:
        """Drop a deferred prompt that has not fired yet."""
        self._pending_prompt_at = 0.0
        self._pending_source = ""


def run_self_test() -> int:
    """Check that the expected markers still appear in the recent log.

    macOS log messages are not API. After a system upgrade a marker may vanish,
    and the watcher would go silently inert. This reports which markers were
    seen over the recent past so the loss is visible.

    Returns:
        Process exit status: 0 when at least one marker was found.
    """
    command = [
        LOG_BINARY,
        "show",
        "--info",
        "--last",
        SELF_TEST_WINDOW,
        "--style",
        "ndjson",
        "--predicate",
        PREDICATE,
    ]
    try:
        completed = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, check=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        LOGGER.error("self-test could not read the log: %s", error)
        return 1

    seen: dict[str, int] = {}
    for line in completed.stdout.splitlines():
        event = parse_log_line(line)
        if event is not None:
            key = f"{event.kind.value}:{event.source}"
            seen[key] = seen.get(key, 0) + 1

    if not seen:
        LOGGER.error(
            "no marker found in the last %s; either nothing authenticated during "
            "that window, or macOS renamed the log messages",
            SELF_TEST_WINDOW,
        )
        return 1
    for key, count in sorted(seen.items()):
        LOGGER.info("%-32s %d", key, count)
    return 0


def main() -> int:
    """Parse arguments and run the watcher.

    Returns:
        Process exit status.
    """
    parser = argparse.ArgumentParser(description=DESCRIPTION)
    parser.add_argument("--port", help="serial port, default: first usbmodem found")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="log the LED commands instead of sending them to the device",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="report which markers appear in the recent log, then exit",
    )
    parser.add_argument("--verbose", action="store_true", help="enable debug logging")
    arguments = parser.parse_args()

    # Explicitly stdout: logging defaults to stderr, which under launchd would
    # send every ordinary line to StandardErrorPath and leave StandardOutPath
    # empty. Crashes still reach stderr on their own.
    logging.basicConfig(
        level=logging.DEBUG if arguments.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        stream=sys.stdout,
    )

    if arguments.self_test:
        return run_self_test()

    leds = LedController(port=arguments.port, dry_run=arguments.dry_run)
    watcher = PromptWatcher(leds)
    source = LogEventSource()

    ticker = threading.Thread(
        target=_tick_loop, args=(watcher,), daemon=True, name="prompt-tick"
    )
    ticker.start()

    LOGGER.info("watching for PIV prompts%s", " (dry run)" if arguments.dry_run else "")
    try:
        for event in source.events():
            watcher.handle(event)
    except KeyboardInterrupt:
        LOGGER.info("stopping")
    finally:
        leds.close()
    return 0


def _tick_loop(watcher: PromptWatcher) -> None:
    """Advance the watcher's timers without waiting for new log traffic.

    Args:
        watcher: The watcher to tick.
    """
    while True:
        time.sleep(TICK_SECONDS)
        watcher.tick()


if __name__ == "__main__":
    sys.exit(main())
