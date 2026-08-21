# Guided fingerprint enrollment

## Overview

The unified firmware enrolls one finger from five distinct captures instead of
building a template from only two similar placements. The goal is to cover the
center and edges of the finger while keeping a single fingerprint template in
the selected sensor slot.

The flow uses the ZW111 native `PS_AutoEnroll` command (`0x31`). The sensor owns
image capture, feature extraction, template merging, and storage. tinyTouch
keeps the fingerprint UART mutex for the operation and translates the sensor's
progress packets into CLI instructions.

## Capture sequence

Follow the terminal prompts and fully lift the finger between captures:

1. place the center of the finger on the sensor;
2. place the left edge of the same finger on the sensor;
3. place the right edge of the same finger on the sensor;
4. place the fingertip on the sensor;
5. place the lower edge of the same finger on the sensor.

The ZW111 combines these samples into one template. This improves coverage but
does not reproduce Apple's Touch ID enrollment UI: the ZW111 does not expose a
coverage map or per-region completion percentage.

## Enroll a finger

Run:

```sh
tinytouch enroll --slot 1
```

Slots 1 through 5 are available through the tinyTouch CLI. Configuration
changes must first be authorized with an already enrolled finger when the
sensor is not empty. If the target slot already contains a template, the native
enrollment command is allowed to replace that slot.

Installing the firmware or CLI does not modify existing templates. Existing
fingers keep their previous two-capture templates until they are explicitly
enrolled again.

## Timeouts and errors

The firmware allows up to 90 seconds for the complete five-capture operation.
The CLI waits 105 seconds so the device can report its final success or failure
before the host-side timeout expires.

The sensor requires a finger lift between successful captures. If feature
extraction fails, the CLI asks the user to lift, adjust, and repeat the current
area. A merge failure, invalid slot, unsupported capture count, sensor timeout,
or storage failure aborts the operation and returns the LED to idle after the
failure indication.

The protocol behavior is defined by the official
[Hi-Link fingerprint module manual](https://r0.hlktech.com/download/HLK-ZW111/1/%E6%8C%87%E7%BA%B9%E6%A8%A1%E7%BB%84%E4%BA%A7%E5%93%81%E7%94%A8%E6%88%B7%E6%89%8B%E5%86%8C_V1.5.1.pdf),
section 3.3.2.2. The manual recommends three to six captures for an 88 by 112
small-area sensor such as the ZW111.
