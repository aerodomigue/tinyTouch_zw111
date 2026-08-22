# PIV prompt feedback on macOS

## Scope

In PIV mode the device has no way of knowing that macOS is asking for the PIV
PIN. The ZW111 stays on its blue idle breathing effect, and nothing tells the
user that a touch is expected. The yellow prompt effect (`FP_LED_STATE_PROMPT`)
currently only fires for CLI-driven operations that call
`fingerprint_authorize_once()` (`CONFIG_UNLOCK`, `PAIRING_MODE`, `ENROLL`).

This document records which macOS dialogs can request the PIV PIN, what each one
emits, and the resulting design for prompt feedback.

## Method

Measurements were taken on macOS 26.5.2 (build 25F84) with the unified firmware
in PIV mode and the identity paired through `sc_auth`. Each test case was
delimited in the unified log with `logger -t TINYTOUCH_TEST "BEGIN <case>"`, then
extracted afterwards:

```sh
/usr/bin/log show --info --start "<begin>" --end "<end>" --style compact \
  --predicate 'process == "ctkahp" OR process == "loginwindow" OR process == "SecurityAgent"'
```

Two traps cost time and are worth recording:

- `log` is a zsh builtin. The absolute path `/usr/bin/log` is required.
- `log show` hides Info-level messages unless `--info` is passed, and Debug
  unless `--debug` is passed. Several of the markers below are Info level and are
  simply invisible without it.

## Which dialogs use PIV

macOS selects a card-specific PAM stack when a CryptoTokenKit token is present:

| stack | line |
| --- | --- |
| `/etc/pam.d/sudo` | `auth sufficient pam_smartcard.so` |
| `/etc/pam.d/screensaver_ctk` | `auth required pam_smartcard.so use_first_pass` |
| `/etc/pam.d/screensaver_new_ctk` | `auth required pam_smartcard.so use_first_pass pkinit` |
| `/etc/pam.d/authorization_ctk` | `auth required pam_smartcard.so use_first_pass pkinit` |

The difference is `use_first_pass`. Only `sudo` lets `pam_smartcard.so` prompt
for itself. Everywhere else SecurityAgent collects the PIN in its own UI and
hands it to PAM afterwards.

## Measured results

Cases exercised: `sudo` in a terminal (A1), an admin authorization dialog raised
by `osascript ... with administrator privileges` (A2), lock screen via ⌃⌘Q (A4),
screensaver via hot corner (A5), and sleep followed by wake (A6). The System
Settings padlock (A3), the `.pkg` installer (A7) and PKCS#11 / mTLS key use (A8)
were not exercised; A3 and A7 are expected to behave like A2 because they share
`authorization_ctk`, A8 is unknown and is the one remaining gap.

### Prompt-open markers

| case | marker at the moment the prompt appears | level | lead time before the touch |
| --- | --- | --- | --- |
| A1 `sudo` | `ctkahp`: `[AHP] Invoking SmartCard agent for uid <uid>` | Info | 3.46 s |
| A2 admin dialog | `loginwindow`: `-[ApplicationManager checkInAppContext:eventData:] \| ApplicationManager: Checked in app : SecurityAgent` | Default | 4.06 s |
| A4 lock screen | `loginwindow`: `_handleAppleEvent \| Received a kAELockScreenEvent`, then `-[LWScreenLock startScreenLock:]` | Default | 7.96 s |
| A5 screensaver | `loginwindow`: `-[ScreenSaverDaemon _screenSaverStart:]`, then the same `LWScreenLock` path | Default | 13.8 s |
| A6 sleep → wake | `loginwindow`: `-[LWScreenLock startScreenLock:] \| entered kLWLockFromDisplayDim (5)`, plus `ctkahp`: `SmartCards inserted:` on wake | Default | 33 s |

`-[LWScreenLock startScreenLock:]` is common to A4, A5 and A6.

That marker alone is not enough, because it fires when the screen *locks*, not
when someone walks back to it. Lock the Mac, come back ten minutes later, and
the LED is long back to blue at the exact moment the hint is wanted. A fourth
marker covers that: `loginwindow` logs

```
-[LWScreenLock startUnlock:] | entered newValue: kLWUnlockFromUserActive (9),
                                       oldValue: kLWLockFromDisplayDim (5)
```

when the user becomes active again on a locked screen, including on wake from
sleep. Beware of a decoy: a `startUnlock` also fires roughly 200 ms after every
lock, carrying a `kLWLockFrom...` reason instead. Matching on
`kLWUnlockFromUserActive` discriminates them; over 90 minutes it produced five
occurrences, all real.

In the end `startScreenLock` is not what the watcher matches on. `loginwindow`
posts a distributed notification alongside it, and logs the fact:

```
-[SessionAgentNotificationCenter sendDistributedNotification:object:]
  | sendDistributedNotification: com.apple.screenIsLocked, with object:501
```

`com.apple.screenIsLocked` is a public notification name that plenty of software
already depends on, so it is far less likely to be renamed than a private
`-[LWScreenLock ...]` method. Over three hours the two matched one for one, nine
occurrences each, so the public name wins.

Four markers therefore raise the prompt: `com.apple.screenIsLocked` when the
screen locks, `kLWUnlockFromUserActive` when the user returns to a locked screen,
`Checked in app : SecurityAgent` for authorization dialogs, and
`Invoking SmartCard agent` for `sudo`.

### Clearing the prompt when something else authenticates

`Token login result` only exists when the PIV card did the work. Unlock the Mac
with an Apple Watch, Touch ID or a typed password and CryptoTokenKit is never
involved, so nothing told the watcher the prompt was over and the LED stayed
yellow until the firmware hold ran out.

The counterpart notification fixes it for every method at once:

```
sendDistributedNotification: com.apple.screenIsUnlocked, with object:501
```

It is posted whatever unlocked the screen, exactly once per unlock, and pairs
with `com.apple.screenIsLocked`. The card path emits both it and
`Token login result`; the result cooldown keeps that from bouncing the LED.

### Cancelling a prompt

The 30 s hold was originally the only answer to a prompt that goes away without
authenticating, on the assumption — inherited from the upstream README — that
macOS exposes nothing when its dialog is dismissed. That turned out to be wrong,
and two markers cover it, both at a level cheap enough to stream:

| cancellation | marker | level |
| --- | --- | --- |
| Ctrl-C on a `sudo` PIN prompt | `sudo`: `SmartCard - Unable to get interactive PIN` | Default |
| Cancel or Escape on an authorization dialog | `SecurityAgent`: `<SFAuthenticationWindow: …> finishing close` | Info |

The `sudo` line repeats as pam retries, which the LED dedup absorbs.

The obvious alternative for the dialog, `loginwindow`'s appDeath notification for
SecurityAgent, is a trap: it was measured **11 seconds** after the window
actually closed, because the process lingers. The window's own close is
immediate.

Neither event sets a cooldown. A dialog that closes on success is followed by its
own result, and one that reopens for a retry has to be free to light the LED
again straight away.

One more marker closes the loop in the other direction. `loginwindow` logs
SkyLight's `[ Display:Power ] Event: Did Sleep` when the display goes dark, which
means nobody is standing there any more. Acting on it returns the LED to idle
immediately instead of waiting out the firmware hold, and lets the LED follow the
screen: locked with the display on is yellow, display off is blue, display on is
yellow again. SkyLight is a client-side framework and dozens of processes log
that line — over two hours, 36 occurrences across all processes but only 5 from
`loginwindow`, which are the real display transitions.

Order matters on wake. The card is de-powered during sleep, and the wake marker
arrives about a second *before* the card finishes re-enumerating:

```
22:00:42.960  loginwindow  startUnlock ... kLWUnlockFromUserActive
22:00:43.831  ctkahp       SmartCards inserted
```

A naive "ignore prompts while the card is absent" rule therefore drops precisely
the prompt that matters most. The watcher remembers such a prompt and replays it
when the card returns, within a short grace period so that merely plugging the
device in later does not light it.

### Submission and result markers

These are identical across all cases:

| event | marker | level |
| --- | --- | --- |
| PIN submitted | `ctkahp`: `[AHP] Invoking SmartCard agent for uid <uid>` (fires again here) | Info |
| PIN submitted | `ctkahp`: `TKGetSmartcardSetting checkCertificateTrust` | Info |
| key operation | `ctkahp`: `evaluateAccessControl:<SecAccessControlRef: tkid(com.apple.pivtoken:<GUID>);osgn(PIN)>` | Default |
| directory bind (screen cases only) | same, with `od(PIN)` instead of `osgn(PIN)` | Default |
| result | `ctkahp`: `[AHP] Token login result <n>` (`0` = success) | Info |
| keychain | `ctkahp`: `[AHP] Keychain unlock result <n>` | Info |

The token GUID from `sc_auth identities` appears in the `evaluateAccessControl`
line and can be used to ignore other smart cards.

### Device presence across sleep

Sleep de-powers the device. In A6 the card disappeared and came back:

```
22:00:20.279  ctkahp  [AHP] Smartcards removed:  com.apple.pivtoken:<GUID>
22:00:43.831  ctkahp  [AHP] SmartCards inserted: com.apple.pivtoken:<GUID>
```

23 s elapsed between the two. During that window the lock screen is already
displayed but the card is not usable. This is a real feedback case of its own:
the LED should not claim readiness before the card is back.

### What the firmware can see

No APDU reaches the card between a prompt appearing and the PIN being submitted.
In A1 the card is first touched at `evaluate token access request`, 3.4 s after
the prompt, which is after the fingerprint. The CCID traffic during that window
is host polling only (`GetSlotStatus`, message type `0x65`), indistinguishable
from idle. **A firmware-only solution is not possible.** See "Validation" for the
device-side confirmation of this, which is the one assumption not yet checked
from the device.

### Cost of streaming the log

Measured over 25 s of wall time, one filtered `log stream` process:

| level | CPU of the `log` client |
| --- | --- |
| `--level debug` | ~11 % |
| `--level info` | ~0 % |
| default | ~0 % |

Debug level is unusable for a permanently running daemon. Every marker above is
Info or Default, so the daemon must never pass `--level debug`.

## Design

A host daemon translates macOS authentication events into LED state commands,
sent over the CDC console the device already exposes. The authentication path
itself is untouched: no new PAM module, no change to how PIV authorizes key use,
no new secret.

```
loginwindow / ctkahp  ──log stream──>  tinytouch-prompt-watcher  ──CDC──>  device LED
```

### Firmware changes (implemented)

`fingerprint.c` already had the state machine and the yellow effect. What was
missing was a way to drive it from the host and an automatic return to idle:

- `fingerprint_led_prompt()` applies `FP_LED_STATE_PROMPT` and arms a deadline
  `FP_LED_PROMPT_HOLD_MS` (30 s) ahead.
- `fingerprint_led_tick()` expires that deadline and returns to
  `FP_LED_STATE_IDLE`. It is called from the existing `touch_hid_task` loop in
  `touch_pin_hid.c`, which already runs every 250 ms, rather than adding a task
  or an `esp_timer` callback that would block on the sensor mutex.
- `show_result()` re-arms the deadline with the result effect's own duration
  instead of blocking on it. It used to `vTaskDelay` for the full second the two
  flashes take, and since it sits on the authentication path, the PIV PIN was
  typed a second later than it needed to be. The ZW111 plays the effect itself,
  so there was never anything to wait for. A result also supersedes the prompt
  that asked for it.
- Two console commands in `config_console.c`: `LED PROMPT` and `LED IDLE`.

The hold lives in the firmware and only there. The host does not duplicate the
timeout; it raises the prompt and clears it early on a result, and otherwise
lets the device fall back on its own.

Both commands are **LED-only**. They do not call `piv_note_user_presence()`, do
not touch `pin_verified_until`, and cannot extend any authorization window. A
process able to open the CDC port gains the ability to blink the LED and nothing
else, so they do not require `CONFIG_UNLOCK`. They are rate limited to one state
change per 250 ms so they cannot hammer the sensor UART, which is shared with
fingerprint polling through the existing mutex.

Two implementation details worth remembering:

- The deadline comparison casts the tick difference to `int32_t` so it stays
  correct across a tick-counter wrap.
- `fingerprint_led_tick()` takes the mutex with a zero timeout and keeps the
  deadline armed until idle is really applied, so a sensor busy with a long
  operation such as enrollment is retried on the next tick rather than leaving
  the LED lit.

### Host daemon

New file `software/macos-helper/tinytouch_prompt_watcher.py`, one
responsibility: watch the log, drive the LED.

- Spawn `/usr/bin/log stream --level info --style ndjson` with a predicate
  restricted to `ctkahp` and `loginwindow`, and parse the JSON lines.
- State machine `idle → prompt → (result | timeout) → idle`, driven by the three
  prompt-open markers, `Token login result` and a bounded hold.
- Write `LED PROMPT` / `LED IDLE` to the device's `/dev/cu.usbmodem*`, reusing
  `port_identity()` from `tinytouch_helper.py`. Extract that helper into a shared
  module rather than duplicating it.
- Restart the `log` child if it exits, reconnect the serial port on
  `SerialException`, mirroring the reconnect loop in `tinytouch_helper.py`.
- Cooperate with the CLI: `tinytouch` already unloads the HID helper before
  taking the port (`HELPER_SUSPENDED`, `unload_helper()`). The watcher needs the
  same treatment or it will hold the port during `enroll` and `provision`.

Installed as a `LaunchAgent`, `com.tinytouch.promptwatcher`, managed by
`tinytouch daemon install` / `uninstall` / `status`.

A `LaunchDaemon` was the initial choice, because an agent does not exist at the
login window before sign-in. It was dropped for two reasons. Installing one
requires `sudo`, which nothing else in the CLI needs. More importantly, the
serial-port arbitration is user-level: the CLI stops the HID helper and the
watcher with `launchctl bootout gui/$UID` before every device command, and it
cannot do the equivalent for a system daemon without root. A root daemon holding
the port would break `tinytouch enroll`.

The cost is stated plainly to the user at install time: everything after sign-in
is covered, the login window is not. Should login-window feedback ever be
wanted, it needs a separate root service with its own port arbitration, not a
change of domain for this one.

### Open design questions

- **The lock screen is a persistent prompt.** Yellow breathing for the whole time
  the machine is locked would be wrong: it wastes power, wears the LED, and
  advertises an armed device in an empty room. The hold is bounded at 30 s,
  re-armed by `kLWUnlockFromUserActive` when someone returns to the screen, and
  cleared by `Event: Did Sleep` when the display goes dark. The remaining gap is
  small: if you sit in front of the lock screen for more than 30 s without
  touching, the LED is blue again by then.
- **`Checked in app : SecurityAgent` is not PIV-specific.** SecurityAgent also
  raises pure-password dialogs, which would light the LED for nothing. A touch
  during such a dialog is harmless, so the false positive is acceptable, but it
  should be measured before deciding whether to filter it.
- **A8 (PKCS#11 / mTLS) is unmeasured.** Key use outside a login goes through
  none of the three markers. It probably surfaces only as
  `evaluateAccessControl ... osgn(PIN)` at submission time, i.e. too late. If
  that path matters, it needs its own investigation.

## Prior art and documented alternatives

### There is no notification API, and nobody has found one

Every existing tool that answers "is the token waiting for me?" is heuristic:

| tool | platform | mechanism |
| --- | --- | --- |
| [yknotify](https://github.com/noperator/yknotify) | macOS | `log stream`, watches for `usbsmartcardreaderd: [com.apple.CryptoTokenKit:ccid] Time extension received` (OpenPGP) and an `IOHIDFamily ... startQueue` line (FIDO2) being the newest entry in their category |
| [gpg-tap-notifier-macos](https://github.com/palantir/gpg-tap-notifier-macos) | macOS | proxies the `gpg-agent` ↔ `scdaemon` channel and assumes any response slower than 1 s means the card is waiting for a human |
| [yubikey-touch-detector](https://github.com/max-baz/yubikey-touch-detector) | Linux | watches a `/dev/hidraw*` node disappear while the key waits |

Palantir state outright that `gpg-agent` and `scdaemon` have no builtin mechanism
to alert external processes when the card is waiting for human input. The
log-stream design in this document is the same family as `yknotify`, with better
markers, so it is a well-trodden path rather than an exotic one.

### Why a YubiKey blinks on its own and tinyTouch cannot

`yknotify`'s OpenPGP marker is `Time extension received`. That is the CCID
mechanism by which a card tells the host "still working, keep waiting". A YubiKey
receives the key-operation APDU **first**, stalls it with time extensions while
waiting for the touch, and blinks meanwhile. The card knows because the operation
already reached it.

tinyTouch's order is inverted: the fingerprint gates the typing of the dummy PIN,
which is what causes macOS to send any APDU at all. Nothing reaches the card
before the touch, so the firmware has nothing to react to. This is the root cause
of the whole problem, and it is structural, not a missing feature.

The consequence is worth keeping: for **app-initiated key use** (ssh via PKCS#11,
mTLS in Safari, code signing — the unmeasured A8 case), the operation *does*
reach the card first. There, stalling the APDU with CCID time extensions while
waiting for the fingerprint would be a firmware-only solution, with no daemon
involved, and it would light the LED at exactly the right moment. A8 and the
login prompts therefore want opposite mechanisms.

### Apple's documented smart card logging

Apple documents a logging switch for smart card activity:

```sh
sudo defaults write /Library/Preferences/com.apple.security.smartcard Logging -bool true
```

It has not been tested here. If it produces explicit, stable messages around the
PIN prompt, it should replace the reverse-engineered strings in this document —
an Apple-documented log line is far less likely to be renamed than
`-[LWScreenLock startScreenLock:]`. This is cheap to check and should be done
before writing the watcher.

The same preference domain documents `DisabledTokens`, which exists precisely to
turn off the built-in PIV token in favour of third-party middleware:

```sh
sudo defaults write /Library/Preferences/com.apple.security.smartcard \
  DisabledTokens -array com.apple.CryptoTokenKit.pivtoken
```

### The documented API path: ship a CryptoTokenKit token extension

The supported way to be told that macOS wants the PIN is to *be* the token
driver. A CryptoTokenKit token extension implements
`TKTokenSession.tokenSession(_:beginAuthFor:constraint:)`, which the system calls
when an operation needs authentication, and returns a `TKTokenAuthOperation`
(typically a `TKTokenSmartCardPINAuthOperation`) whose `finish()` receives the
PIN. Throwing `TKError.authenticationNeeded` from a key operation is what makes
the system raise the PIN dialog. A known gotcha: the callback only fires if the
key's constraint is set to something other than a plain boolean.

[OpenSCToken](https://github.com/frankmorgner/OpenSCToken) is a production
precedent — a third-party CTK extension used for macOS smart card login — so the
path is real, not theoretical.

Trade-offs:

- **For:** documented, stable across releases, no log parsing, and the extension
  learns about the prompt before it is displayed.
- **Against:** it replaces Apple's `pivtoken` with a signed app plus appex, which
  changes install, pairing and update flows entirely. It is exactly the "big
  refactor" this investigation set out to avoid.
- **Unverified:** whether `beginAuthFor` fires at *prompt* time for the login
  window and lock screen. The measurements above show that for Apple's `pivtoken`
  the PIN is collected by SecurityAgent before the token is involved at all. If
  a custom token is treated the same way, the refactor would buy nothing for the
  cases that matter most. This must be tested with a throwaway extension before
  anyone commits to it.

## Alternatives considered

| option | sudo | auth dialogs | lock screen | login window | app key use (A8) | cost |
| --- | --- | --- | --- | --- | --- | --- |
| firmware-only, from CCID traffic | no | no | no | no | no | none, but does not work |
| PAM module in `sudo_local` | yes | no (`use_first_pass`) | no (`use_first_pass`) | no | no | C module, risk of breaking `sudo` |
| log-stream agent (chosen) | yes | yes | yes | no, see below | no | one agent, ~0 % CPU |
| `com.apple.screenIsLocked` notification | no | no | yes | no | no | trivial, but partial |
| CCID time extension in firmware | no | no | no | no | **yes** | firmware only; complements the daemon |
| custom CryptoTokenKit extension | probably | probably | unverified | unverified | yes | full rewrite of the token layer |

A PAM module remains a reasonable *addition* for `sudo` later — precise and free
at runtime — but must be declared `auth optional`, never `required`, so a failure
can never lock the user out of `sudo`. The `_ctk` stacks have no `_local` include,
so covering the other dialogs that way would mean editing Apple-owned files that
macOS updates overwrite.

## Validation

1. Add a temporary `ESP_LOGI` of `CLA/INS/P1/P2` for every APDU in
   `piv_handle_apdu()`, flash, and run `sudo -k; sudo -v`. Confirm no APDU
   arrives between the prompt appearing and the touch. This is the last
   assumption of this design not verified from the device side.
2. Replay the case matrix (A1, A2, A4, A5, A6) with the watcher running: the LED
   must turn yellow within ~100 ms of each prompt.
3. Exercise the untested cases: A3 System Settings padlock, A7 `.pkg` installer,
   A9 login window after logout.
4. Cancel a prompt (⌃C on `sudo`, Escape on a dialog) — the LED must return to
   blue after the hold timeout.
5. Sleep and wake — no yellow while the card is absent, yellow once
   `SmartCards inserted` arrives.
6. Unplug and replug the device while the watcher runs — it must reconnect.
7. Run `tinytouch enroll` while the watcher is loaded — enrollment must not fail
   on a busy serial port.

## Stability risk

These log messages are not API. Apple can rename `Invoking SmartCard agent`,
`startScreenLock` or `Token login result` in any release. The watcher must treat
an unmatched log as "no prompt" and silently stop providing feedback, never block
or misreport. A startup self-test that greps the last hour of logs for the
expected markers is enough to warn the user that feedback went inactive after a
macOS upgrade.
