# USB suspend and resume recovery

## Scope

This document describes a suspend/resume failure observed with the unified PIV
firmware on a Seeed Studio XIAO ESP32-S3 connected to macOS. It applies to the
firmware that exposes CCID, HID, and CDC through the same TinyUSB device.

## Symptoms

After macOS woke from sleep, the device remained enumerated but could no longer
complete authentication:

- the ZW111 kept its blue idle breathing effect;
- the HID interface remained listed by macOS but sent no keyboard report;
- the CDC serial device remained present but did not answer `PING`;
- the CCID reader remained usable and a PIV `SELECT` APDU still returned
  `0x9000`.

The last observation is important. A successful CCID transfer proves that the
USB bus and the ESP32-S3 controller are active. The failure was therefore not a
disconnected device, a stopped CPU, or an unresponsive fingerprint sensor.

## Cause

TinyUSB uses `tud_ready()` to guard its built-in HID and CDC drivers.
`tud_ready()` is true only when the device is mounted and TinyUSB does not
consider it suspended.

On the affected suspend/resume cycle, host traffic resumed but TinyUSB's
software state remained suspended. Consequently:

1. `tud_hid_ready()` stayed false;
2. the fingerprint polling path did not run because it is intentionally gated
   by HID readiness;
3. the dummy PIV PIN could not be delivered as a HID keyboard report;
4. CDC writes were also rejected by their `tud_ready()` guard.

The custom CCID driver uses TinyUSB's endpoint API directly and does not use the
built-in HID or CDC readiness guard. This explains why PIV APDUs continued to
work while HID and CDC appeared frozen.

The firmware also called `tud_task()` from `app_main()` even though
`tinyusb_driver_install()` already starts the esp_tinyusb task. Two consumers
were therefore racing the same TinyUSB event queue and global device state.
This was an independent correctness defect and could make resume processing
unreliable, although removing it alone was not sufficient to fix the observed
sleep failure.

## Recovery mechanism

The custom CCID transfer callback now reconciles TinyUSB's state when both of
the following conditions are true:

- a CCID transfer completed successfully, proving that host traffic is active;
- `tud_suspended()` still reports that TinyUSB is suspended.

In that case, the callback submits TinyUSB's native `DCD_EVENT_RESUME` event
from task context. TinyUSB clears its suspended state through its normal event
handler, after which the built-in HID and CDC drivers become ready again. The
firmware does not reboot, disconnect, or re-enumerate the USB device.

Only the task created by `tinyusb_driver_install()` now calls `tud_task()`.
`app_main()` returns after starting the independent console and fingerprint/HID
tasks.

## Safety properties

- Resume is synthesized only after a successful USB transfer and only while
  TinyUSB still reports a suspended state.
- Normal CCID traffic is unchanged when TinyUSB is already resumed.
- No watchdog, periodic recovery task, forced USB reset, or host-visible
  re-enumeration is used.
- Fingerprint templates, ZW111 LED control, PIV keys, and NVS data are not
  modified by the recovery path.
- The mechanism does not wake a sleeping host. It repairs device state after
  the host has resumed and sent CCID traffic.
- This is a recovery path for the unified CCID/PIV firmware, not the separate
  keyboard-only firmware.

## Validation

The change was validated with ESP-IDF 5.3.2 on a Seeed Studio XIAO ESP32-S3 and
a Hi-Link ZW111:

- the firmware built without errors;
- macOS sleep/wake no longer reproduced the frozen authentication state;
- CDC status commands responded after wake;
- the fingerprint sensor remained ready with the enrolled template intact;
- the PIV reader remained available and `SELECT PIV` returned `0x9000`;
- fingerprint-gated HID delivery completed PIV authentication after wake.

Related upstream reports provide useful context:

- [tinyTouch issue #28](https://github.com/ZimengXiong/tinyTouch/issues/28)
  describes the same macOS post-sleep symptom.
- [tinyTouch pull request #11](https://github.com/ZimengXiong/tinyTouch/pull/11)
  identifies the duplicate `tud_task()` consumers and notes that removing the
  duplicate alone does not solve suspend recovery.
- [TinyUSB issue #3445](https://github.com/hathach/tinyusb/issues/3445)
  reports an ESP32-S3 host-resume window where traffic has resumed while
  `tud_ready()` still returns false.
