# Xbox Wireless controller

Notes on the controller used with the [RetroPad bridge](../bridge/esp32-pad): what was tested, what the controller sends, and how it maps to the N64 controller.

## Tested model

| | |
|---|---|
| Controller | Xbox Wireless Controller (Xbox Series X\|S generation, model 1914) |
| USB IDs | vendor `0x045E` (Microsoft), product `0x0B13` |
| Firmware | 5.9.2709.0 |
| Bluetooth | Bluetooth LE, standard HID over GATT |

Controllers from the Xbox One generation (model 1708) use the same report format once their firmware is updated for Bluetooth LE, but haven't been tried. Xbox One controllers without Bluetooth (model 1697 and older) can't work.

## Pairing

- **Pairing mode:** turn the controller on, then hold the **pair button** (top edge, next to the USB-C port) until the Xbox logo flashes quickly.
- **Paired:** the logo stays lit.
- **Reconnecting:** once bonded, the controller reconnects to the bridge when turned on, without pairing mode.
- **Other hosts:** in testing, a Mac the controller was also paired with didn't take it back while the bridge was running. Turn off Bluetooth on other paired devices during pairing to be safe.

## Input report

The controller sends one 16-byte input report on its HID report characteristic. Values are little-endian.

| Bytes | Field | Values | Confirmed on hardware |
|---|---|---|---|
| 0–1 | Left stick X | 0…65535, centre `0x8000` | Yes |
| 2–3 | Left stick Y | 0…65535, centre `0x8000`, grows downwards | Yes |
| 4–5 | Right stick X | as left X | Yes |
| 6–7 | Right stick Y | as left Y | Yes |
| 8–9 | Left trigger | 0…1023 | Assumed |
| 10–11 | Right trigger | 0…1023 | Assumed |
| 12 | D-pad as a hat | 0 = released, 1 = up, then clockwise: 3 = right, 5 = down, 7 = left; even values are diagonals | Up, right, down, left: yes. Diagonals: assumed |
| 13 | Buttons | A `0x01`, B `0x02`, X `0x08`, Y `0x10`, LB `0x40`, RB `0x80` | Yes |
| 14 | Buttons | View `0x04`, Menu `0x08`, Xbox `0x10`, left stick click `0x20`, right stick click `0x40` | Assumed |
| 15 | Buttons | Share `0x01` | Assumed |

"Confirmed" means the bit was seen in the bridge's serial log while that button was pressed. "Assumed" fields follow the layout commonly documented for this controller. They're decoded by the bridge but weren't checked one by one; if one turns out wrong, the raw log shows the real bit.

The decoder is in [`bridge/esp32-pad/src/pad_state.c`](../bridge/esp32-pad/src/pad_state.c).

## Mapping to the N64 controller

Done on the watch, in [`app/RetroWatch/PadBridge.swift`](../app/RetroWatch/PadBridge.swift):

| Xbox | N64 | Notes |
|---|---|---|
| Left stick | Stick | Scaled to the N64 range (±80) |
| A | A | Jump |
| X or B | B | Punch; both work, so either thumb position is fine |
| LT | Z | Pressed beyond about 40% |
| LB / RB | L / R | |
| Right stick | C buttons | Pushed beyond half way; turns the camera |
| Menu | Start | Pause |
| D-pad | D-pad | |
| View, Xbox, Y, stick clicks, Share | Not used | |

## Other controllers

The bridge connects to any Bluetooth LE gamepad that advertises the HID service with the gamepad appearance, or has "Xbox" in its name. Only the Xbox report format is decoded, though. Supporting another controller means adding a parser for its report in `pad_state.c`; the raw reports in the serial log show its layout.

Classic-Bluetooth-only controllers, such as older PlayStation or Switch Pro controllers, would need an ESP32 chip with classic Bluetooth (the original ESP32) and different firmware.

Phone controllers with an "iOS / Android" mode switch, like the HZ-2746 that was tried, don't help: in both modes they show up as a mouse plus media keys (volume, play/pause, track skip), not as a gamepad. See [limitations](limitations.md#controllers-that-dont-work).
