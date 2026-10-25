# RetroPad bridge (ESP32)

A small ESP32 board that lets you play the watch port with a real game controller.

```
Xbox controller ──Bluetooth LE (HID)──▶ ESP32 bridge ──Bluetooth LE (RetroPad service)──▶ Apple Watch
```

## Why a bridge is needed

An Apple Watch can't use a game controller directly:

- **watchOS has no GameController framework.** The iPhone, iPad, Mac and Apple TV have one; the watch doesn't.
- **The watch can't pair one.** Its Bluetooth settings only list headphones, speakers and health accessories.
- **Apps can't read controllers through Core Bluetooth either.** Controllers send their buttons through the standard Bluetooth HID service (`0x1812`), and Apple hides that service from apps on every platform. Probing a controller from an app only shows *Device Information* and *Battery*.

What a watch app *can* do is connect to any Bluetooth LE device that offers a **custom** service. The bridge is that device: it talks HID to the controller and offers the controller's state to the watch as a custom service.

## What it does

The firmware uses ESP-IDF's NimBLE Bluetooth stack in two roles at once:

- **Central, towards the controller.** It scans for a gamepad (Bluetooth HID with the gamepad appearance, or "Xbox" in its name), connects, bonds with "Just Works" pairing, finds the HID service and subscribes to its input reports. The bond is stored in flash, so the controller reconnects by itself after a power cycle.
- **Peripheral, towards the watch.** It advertises as `RetroPad` with the RetroPad service and sends a small state packet to the watch whenever the controller's state changes. The watch side needs no pairing.

## Hardware

Tested on an **ESP32-C6** mini board (chip ESP32-C6FH4, 4 MB embedded flash, Bluetooth 5 LE), the thumb-sized kind with a USB-C port and two buttons, BOOT and RESET.

Other ESP32 chips with Bluetooth LE (ESP32, ESP32-S3, ESP32-C3) should work after changing `board` in `platformio.ini`; they haven't been tried. Chips without Bluetooth LE (ESP32-S2) can't work.

## Build and flash

Requires [PlatformIO](https://platformio.org). It downloads ESP-IDF (5.5 was used) the first time.

```sh
cd bridge/esp32-pad
pio run -t upload                                       # build and flash over USB
pio run -t upload --upload-port /dev/cu.usbmodem1101    # if several serial ports exist
pio device monitor                                      # serial log, 115200 baud
```

Close any serial monitor before flashing: only one program can hold the port. The C6's USB port is built into the chip, so it disappears briefly whenever the board resets. A monitor that doesn't reconnect by itself will miss the startup lines.

## Pairing a controller

1. **Power the bridge.** It starts advertising for the watch and looking for a controller.
2. **Put the controller in pairing mode:** turn it on with the Xbox button, then hold the small **pair button** (on top, next to the USB-C port) until the Xbox logo **flashes quickly**.
3. **Wait a few seconds.** The logo stops flashing and stays lit once the bridge has bonded.

After that, turning the controller on is enough: it reconnects to the bridge by itself.

If the controller is also paired with a Mac, iPhone or iPad, that device may take it first. Turn its Bluetooth off during pairing, or remove the controller there.

**To pair a different controller, hold BOOT for 3 seconds.** The bridge forgets all bonded controllers and disconnects the current one. (RESET just restarts the board.)

## Connecting the watch

Open the game on the watch. The first time, watchOS asks for **Bluetooth** permission; allow it. The app then finds the bridge and connects whenever the game is open. If the permission was declined, turn it on under *Settings → Privacy & Security → Bluetooth* on the watch.

The status at the top-left of the watch screen shows the state of both links:

| Watch shows | Meaning |
|---|---|
| **Controller** (green) | Watch, bridge and controller all connected |
| **No controller** (orange) | Watch connected to the bridge; no controller on the bridge |
| **Searching…** (grey) | Watch is looking for the bridge |
| **Bluetooth off** (red) | Bluetooth is off on the watch, or the app isn't allowed to use it |

## Serial log

The bridge logs over USB at 115200 baud:

- A **status line every 10 seconds:** `status: controller connected, watch subscribed, advertising off`. Advertising is off while a watch is connected; that's normal.
- **Raw input reports** in hex whenever a button byte changes, followed by the decoded state: `buttons 0x0001  L(-1,2) R(-9,2) LT 0 RT 0`. This is how the report layout in [`docs/xbox-controller.md`](../../docs/xbox-controller.md) was checked, and it's the starting point for supporting another controller.
- Connection events: controller found, paired, ready, disconnected; watch connected, subscribed, disconnected.

## Protocol

Service `6d3c0001-5f8e-4b7a-9c1e-2a7b52504144`, characteristic `6d3c0002-5f8e-4b7a-9c1e-2a7b52504144` (read and notify). The value is an 11-byte `struct pad_state`, defined in [`src/pad_state.h`](src/pad_state.h):

| Bytes | Field |
|---|---|
| 0 | Version (2) |
| 1 | Sequence number; increases with every change |
| 2–3 | Buttons (`PAD_BTN_*` bits), little-endian |
| 4–7 | Left X, left Y, right X, right Y: −127…127, +Y is up |
| 8–9 | Left and right trigger: 0…255 |
| 10 | Flags: bit 0 = a controller is connected and sending reports |

The packet describes a generic controller, not an N64 one. The mapping to N64 buttons happens on the watch, in `app/RetroWatch/PadBridge.swift`. Version 1 packets (10 bytes, no flags byte) are still accepted by the watch app.

## Power

The bridge needs its own power while you play:

- **Not from the controller's USB-C port.** That port is an input, for charging and for wired play; it doesn't supply power.
- **A small LiPo cell is the simplest option.** Many mini ESP32 boards have a LiPo charger and battery pads. A 150–400 mAh cell fits in a 3D-printed holder clipped to the controller, and charges through the board's USB-C port.
- **The controller's battery is possible but unproven.** Rechargeable packs often expose two charging contacts through the battery door. Whether they carry the battery's voltage when the pack isn't on its charger depends on the pack; measure with a multimeter first, and don't short them. Two AA cells give about 2–3 V, below the 3.3 V the ESP32 needs, so either way a small step-up converter to 5 V (into the board's 5 V pin) would be needed.

Power draw and battery life haven't been measured yet. Expect tens of milliamps while connected.

## Troubleshooting

- **The watch shows "Searching…" forever.** Check the serial status line. `advertising off` with `watch none` shouldn't last more than 2 seconds: a watchdog restarts advertising, which the C6 stops silently while it sets up the controller link. Also check the watch's Bluetooth permission for the app.
- **The controller never connects.** Make sure it's really in pairing mode (fast-flashing logo) and that no Mac, iPhone or iPad nearby takes it first. Hold BOOT for 3 seconds to clear old bonds, then pair again.
- **Buttons are wrong or missing.** Read the raw reports in the serial log and compare them with the layout in `src/pad_state.c`. Only the Xbox report format is decoded.
- **Flashing fails because the port is busy.** Close `pio device monitor` or any other program reading the serial port.

## Known limitations

- One controller and one watch at a time.
- Only the Xbox Wireless report format is decoded (see [`docs/xbox-controller.md`](../../docs/xbox-controller.md)). Other Bluetooth LE gamepads may connect but need a parser in `pad_state.c`.
- Latency hasn't been measured. In play it feels immediate, but there's no number yet.
- There's no status LED; the serial log and the watch's status label are the only feedback.
- The watch screen still dims when it isn't touched, controller or not. See [`docs/limitations.md`](../../docs/limitations.md#the-screen-dims-and-apps-cant-stop-it).
