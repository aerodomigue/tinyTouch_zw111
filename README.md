# tinyTouch

Fingerprint authentication for macOS built from an ESP32-S3 and a UART
fingerprint sensor. It unlocks the screen, answers `sudo`, and satisfies
authorization dialogs without typing a password.

Fork of [ZimengXiong/tinyTouch](https://github.com/ZimengXiong/tinyTouch).

## Table of contents

- [Modes](#modes)
- [Security trade-offs](#security-trade-offs)
- [LED feedback](#led-feedback)
- [PIV prompt feedback](#piv-prompt-feedback)
- [Install](#install)
- [Fingerprint enrollment](#fingerprint-enrollment)
- [Hardware](#hardware)
- [Wiring](#wiring)
- [Notes](#notes)

## Modes

The device works in one of two modes, selected with `tinytouch mode`.

### HID mode

The ESP32 acts as a USB keyboard. Your real password stays on the Mac,
encrypted, and the ESP32 keeps only a shared pairing key. After a fingerprint
match the ESP32 sends a signed request to the host helper, which checks it,
encrypts the password for that one request, and sends it back. The ESP32
decrypts it in RAM, types it, then wipes it.

This works almost everywhere a password is accepted. It is also the riskier
mode: the final step is your real password being typed into whatever has focus.
Requests carry a nonce and a MAC so old ones cannot be replayed, and the reply is
a one-time encrypted response, but that does not change where the password ends
up.

### PIV mode

The ESP32 acts as a USB smart card. macOS sends normal PIV commands over CCID
and asks the card to use the PIV private key; the ESP32 only allows that key
operation right after a fingerprint match.

macOS also expects a PIV PIN, so the firmware has a small HID path that types the
dummy PIN `000000`. That PIN is not your Mac password. It only gets past the
macOS PIN dialog — the real authorization is the fingerprint gate around the PIV
key. This avoids typing your real password, but only works where macOS accepts
smart cards.

### What each mode covers

| | HID | PIV |
| -- | -- | -- |
| Keyboardless login | yes | yes |
| `sudo` prompts | yes | yes |
| Apple TCC (Privacy & Security) | yes | yes |
| General settings | yes | no |
| Keychain / Apple Passwords | yes | no |
| Anywhere a password is accepted (remote SSH, etc.) | yes | usually not |

## Security trade-offs

| | HID | PIV |
| -- | -- | -- |
| Sensor to ESP32 | unauthenticated UART | unauthenticated UART |
| ESP32 to computer | shared-key MAC and encryption | plain USB CCID/APDU |
| Authentication | password typed over HID | PIV challenge/response |

| Attack | HID | PIV |
| -- | -- | -- |
| Sensor UART spoofing | possible | possible |
| Wrong focused field | possible | no |
| Malicious password field | possible | no |
| USB traffic sniffing | low impact, channel is encrypted and MAC'd | APDUs observable, PIV private key is not |
| USB keylogger | can reveal the password | cannot reveal the key |
| USB command injection | bad MACs and replays rejected | APDUs accepted, but key use still needs a fingerprint |
| Flash dump, no secure boot or flash encryption | shared key exposable | PIV key exposable |
| Flash dump, secure boot and flash encryption on | shared key not exportable | PIV key not exportable |
| Flash dump with a secure element | shared key not exportable | PIV key not exportable |

**Sensor UART spoofing is the significant weakness.** All authentication happens
inside the fingerprint sensor, which talks to the ESP32 over an unauthenticated
UART, so it can be spoofed by someone with physical access to the device.
Filling the enclosure with black epoxy raises the bar; a properly authenticated
sensor would fix it.

Every attack listed here needs physical access to both the device and your Mac.
Enable secure boot and flash encryption if that assumption does not hold for you.

## LED feedback

tinyTouch drives the Hi-Link ZW111 LED with its multi-function `0x3C` command.
Each state transition is sent once and the sensor runs the effect itself, so
there is no host-side animation loop.

| State | Colour and effect |
| --- | --- |
| Idle / sleep | Blue breathing, 10-second period |
| Touch requested | Yellow breathing, 1-second period |
| Fingerprint rejected | Two red flashes, 0.5 s each |
| Fingerprint accepted | Two green flashes, 0.5 s each |

After a result the LED returns to idle. A request triggered by the CLI also
returns to idle after seven seconds without an image.

On the first boot with this firmware, tinyTouch provisions the ZW111's
persistent manual LED mode. Disconnect and reconnect USB once after the log
message asks for it, making sure the ZW111's VCC actually falls to 0 V — an
ESP32 reset alone is not enough. This changes only the sensor LED mode; it does
not erase fingerprint templates, PIV keys, or any other configuration.

## PIV prompt feedback

In PIV mode the device cannot tell on its own that macOS is asking for the PIN:
no APDU reaches the card between the prompt appearing and the PIN being
submitted. Without help, the LED stays blue while macOS waits for a touch.

The `LED PROMPT` and `LED IDLE` console commands let a host say so, and
`software/macos-helper/tinytouch_prompt_watcher.py` sends them by watching the
unified log for macOS authentication events. It covers `sudo`, authorization
dialogs, the lock screen, the screensaver and wake from sleep.

On a locked screen the LED follows the display: yellow while the screen is on and
waiting, blue once the display goes dark, yellow again when you come back. Wake
from sleep is included, even though the card needs about a second to
re-enumerate.

`LED PROMPT` shows the yellow effect and returns to idle by itself after 30
seconds, so an abandoned prompt cannot leave the LED lit. Both commands are LED
only: they grant no authorization, do not affect PIV key use, and are rate
limited to one state change per 250 ms because the effect shares the sensor UART
with fingerprint matching. They deliberately do not require `CONFIG_UNLOCK`.

### Install the service

```sh
tinytouch daemon install
tinytouch daemon status
tinytouch daemon uninstall
```

`install` is idempotent: run it again to reinstall over an existing copy.
`status` reports whether the service is installed and running, and is also part
of `tinytouch status`.

The service runs as a LaunchAgent in your login session, so it does not cover
the login window before you sign in. Everything after that is covered.

### Run it by hand

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r software/macos-helper/requirements.txt

# check the log markers exist on this macOS version
.venv/bin/python software/macos-helper/tinytouch_prompt_watcher.py --self-test

# watch the events without touching the device
.venv/bin/python software/macos-helper/tinytouch_prompt_watcher.py --dry-run --verbose

# run it for real
.venv/bin/python software/macos-helper/tinytouch_prompt_watcher.py
```

To start it at login without the CLI, write a LaunchAgent to
`~/Library/LaunchAgents/com.tinytouch.promptwatcher.plist` with `RunAtLoad` and
`KeepAlive` set and `ProgramArguments` pointing at that interpreter and script,
then load it with `launchctl bootstrap gui/$UID <plist>`. The CLI logs to
`/tmp/tinytouch-prompt-watcher.log`, with errors in the matching `.err` file.

Only one process may hold the serial port. The CLI stops the service for the
duration of any command that talks to the device and restarts it afterwards. If
you run the watcher by hand, stop it before `tinytouch enroll` and similar
commands.

Rerun `--self-test` after a macOS upgrade. These log messages are not API and
Apple can rename them; if they disappear the watcher goes quiet rather than
misbehaving. See [PIV prompt feedback](docs/piv-prompt-feedback.md) for the
measurements behind the markers and the alternatives that were rejected.

## Install

### HID mode

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r software/macos-helper/requirements.txt

pairing_key="$(openssl rand -hex 32)"
.venv/bin/python software/macos-helper/tinytouch_helper.py --set-pairing-key "$pairing_key"
.venv/bin/python software/macos-helper/tinytouch_helper.py --set-password 'your-password-here'
```

Run the helper with:

```sh
.venv/bin/python software/macos-helper/tinytouch_helper.py
```

For launchd, edit the paths in
`software/macos-helper/launchd/com.tinytouch.helper.plist` and copy it to
`~/Library/LaunchAgents/`.

### PIV mode

`main/secrets.h` needs the PIV certificates and private keys for slots `9a` and
`9d`. Generate test keys with:

```sh
cd firmware/tiny_touch_smartcard
openssl req -newkey rsa:2048 -nodes -keyout piv_key_9a.pem -x509 -days 3650 -out piv_cert_9a.pem -subj "/CN=tinytouch piv auth/"
openssl req -newkey rsa:2048 -nodes -keyout piv_key_9d.pem -x509 -days 3650 -out piv_cert_9d.pem -subj "/CN=tinytouch piv key management/"
cp main/secrets.example.h main/secrets.h
```

Then paste `piv_cert_9a.pem` into `PIV_CERT_9A_PEM`, `piv_key_9a.pem` into
`PIV_PRIVATE_KEY_9A_PEM`, `piv_cert_9d.pem` into `PIV_CERT_9D_PEM`, and
`piv_key_9d.pem` into `PIV_PRIVATE_KEY_9D_PEM`.

Build and flash:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```

Then pair the identity with your macOS account:

```sh
system_profiler SPSmartCardsDataType
sc_auth identities
sudo sc_auth pair -u "$USER" -h <auth-cert-hash>
```

Test it:

```sh
sudo -k
sudo -v
```

Touch the sensor when macOS asks for the PIN.

For the ESP32-S3 USB sleep/wake failure and its TinyUSB recovery path, see
[USB suspend and resume recovery](docs/usb-suspend-resume.md).

## Fingerprint enrollment

Enrollment captures the centre and four edges of the same finger, with a full
lift between each capture. The ZW111 merges the five samples into a single
template in the selected slot. Run `tinytouch enroll --slot 1` and follow the
terminal instructions. Existing templates are not upgraded automatically.

See [guided fingerprint enrollment](docs/fingerprint-enrollment.md) for the
capture sequence, timeouts, replacement behaviour, and protocol details.

## Hardware

| Part | Used here | Notes |
| -- | -- | -- |
| Microcontroller | Seeed Studio ESP32-S3 | Needs native USB and a hardware UART. Secure boot and flash encryption strongly recommended |
| Fingerprint sensor | ZW101-style UART sensor | Uses the common `0xEF01` packet protocol |
| Computer | macOS | HID mode needs the helper, PIV mode needs macOS smart card support |
| Case | Printed top and bottom | `hardware/case/case_top.stl`, `hardware/case/case_bottom.stl` |

Other ESP32-S3 boards work if the USB and UART pins are available. Other
fingerprint sensors may work if they speak the same UART protocol. Other
microcontroller families are not supported.

Build guide: <https://www.youtube.com/watch?v=YsP1hRg28Gw>

[CAD](https://cad.onshape.com/documents/d0e6bb7977e6171d4e4a5086/w/1ded27ad6c634fd1fdaf26d0/e/aca67210e400490a08d0b29a?renderMode=0&uiState=6a4c1df32e292f12144a65fe).
If you change it, please keep your changes open source too.

## Wiring

The fingerprint sensor connects over UART to pins 6 and 7 for TX and RX. The
interrupt pin can go anywhere; the firmware uses pin 1.

## Notes

Do not commit:

- `firmware/tiny_touch_keyboard/secrets.h`
- `firmware/tiny_touch_smartcard/main/secrets.h`
