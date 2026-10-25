# RetroPad bridge (ESP32)

watchOS apps can't use game controllers: there's no GameController framework on the watch, and Apple hides the Bluetooth HID service that controllers send their buttons over. Watch apps *can* read any custom Bluetooth LE service, though.

This firmware turns a small ESP32 into a bridge:

```
Xbox controller ──BLE HID──▶ ESP32 (this firmware) ──custom BLE service──▶ watch app
```

- **Central role:** it finds a controller in pairing mode, bonds with it, and subscribes to its input reports. Bonds are stored in flash, so the controller reconnects by itself.
- **Peripheral role:** it advertises as `RetroPad` with its own service. The watch app connects whenever the game is open; there's no pairing on the watch side.

Tested with an **ESP32-C6** and an **Xbox Wireless Controller** (model 1914, Bluetooth LE firmware). Other ESP32 chips with Bluetooth LE should work with a different `board` in `platformio.ini`.

## Build and flash

Requires [PlatformIO](https://platformio.org).

```sh
cd bridge/esp32-pad
pio run -t upload        # build and flash over USB
pio device monitor       # serial log, 115200
```

## Pairing

1. Power the ESP32. It starts looking for a controller.
2. Turn on the controller and hold its **pair button** until the Xbox logo flashes quickly.
3. The logo stays lit once they're connected. From then on, turning the controller on reconnects it.

To pair a different controller, **hold BOOT for 3 seconds**: the bridge forgets its bonded controllers.

The serial log prints raw input reports and a status line every 10 seconds (`controller connected, watch subscribed, …`), which helps with debugging.

## Protocol

Service `6d3c0001-5f8e-4b7a-9c1e-2a7b52504144`, characteristic `6d3c0002-…` (read and notify): an 11-byte `struct pad_state`, defined in [`src/pad_state.h`](src/pad_state.h).

| Bytes | Field |
|---|---|
| 0 | version (2) |
| 1 | sequence number, increases with every change |
| 2–3 | buttons (`PAD_BTN_*`), little-endian |
| 4–7 | left X, left Y, right X, right Y: −127…127, +Y is up |
| 8–9 | left and right trigger: 0…255 |
| 10 | flags: bit 0 = a controller is connected |

The watch app maps this to the N64 controller in `app/RetroWatch/PadBridge.swift`.

## Power

The ESP32 needs its own power. The Xbox controller's USB-C port is an input only. A small LiPo cell on a board with a built-in charger, in a 3D-printed holder clipped to the controller, works well.
