# Limitations and findings

What the watch can and can't do for this port, with the evidence behind each point. Hardware used: Apple Watch Ultra 3 (watchOS 26), Xcode 26.6, the European Super Mario 64 ROM.

## Input

### The touchscreen tracks one finger

watchOS reports a single touch at a time, so the stick and a button can't be held together. The touch controls work around this by sliding: drag on the game to run, then slide onto a button below it, and the stick keeps its last direction. On the way down, though, the finger passes over the game area and pulls the stick backwards; that's still open (see the README's *Help wanted*).

### The double-tap pinch is too slow for jumping

watchOS lets one button in an app react to the system's double-tap gesture (finger and thumb, twice, on the hand wearing the watch). Apps can't detect a single pinch. In testing:

- it took about **two seconds** to register, with the screen turning grey first;
- it **didn't fire while the screen was being touched**, which is exactly when a jump is needed.

It was removed from the controls.

### No game controllers without help

- watchOS has **no GameController framework**; the module doesn't exist in the watchOS SDK.
- The watch's Bluetooth settings **can't pair a controller**.
- **Core Bluetooth hides the HID service** (`0x1812`) that controllers send their buttons over. Probing a controller from an app shows only *Device Information* and *Battery*.

The [ESP32 bridge](../bridge/esp32-pad) gets around all three.

Relaying a controller paired with the iPhone to the watch would also be possible, but wasn't built: if the phone is at hand, playing on the phone makes more sense.

### Controllers that don't work

Cheap phone controllers with an "iOS / Android" switch were tested with an **HZ-2746** (Bluetooth LE, vendor ID `0x248A`), using Apple's frameworks and raw HID on a Mac:

| | iOS mode | Android mode |
|---|---|---|
| Recognised as a game controller | No | No |
| Stick | Mouse pointer movement | Nothing |
| Two buttons | Left / right mouse click | Left / right mouse click |
| Other buttons | Media keys: volume, play/pause, next / previous track | The same media keys |
| Some buttons | Nothing | Nothing |

Neither mode offers anything a relay could use. These controllers emulate a mouse and media keys because iOS doesn't support generic gamepads.

## The screen dims, and apps can't stop it

The watch dims its screen a few seconds after the last touch, Digital Crown turn or wrist raise. Controller input doesn't count. When it dims, the app becomes inactive and the game pauses until the screen wakes.

- **There's no public API to keep the screen on.** iOS's `isIdleTimerDisabled` doesn't exist on watchOS, and Apple engineers have said on the developer forums that it can't be done.
- **The private API exists but refuses.** IdleTimerServices (`ITIdleTimerState`, `ITIdleTimerConfiguration.configurationToDisableIdleTimer`) is reachable from a watch app and reports the service as available. Asking it to disable the idle timer fails with:

  > Error Domain=ITIdleTimerConfigurationErrorDomain Code=2 "Missing <com.apple.idle-timer-services> Entitlement"

  That entitlement is Apple-only. A developer provisioning profile can't include it, and the watch won't launch an app that claims an entitlement its profile doesn't allow.
- **Faking a touch doesn't help.** An app can only create touches inside itself; the system's idle timer only reacts to real input.

Workarounds: set **Settings → Display & Brightness → Wake Duration → Wake for 70 seconds** on the watch, and nudge the Digital Crown now and then while playing.

## Performance

- **Apple Watch Ultra 3: 28–31 ms per frame** in front of the castle. The European version runs at 25 frames per second, so it has 40 ms per frame: full speed, with some room left.
- A Mac takes about 8 ms per frame for the same scenes, so Mac timings say little about the watch.
- Everything is drawn on the CPU by the software renderer (watchOS has no OpenGL or Vulkan), at the N64's 320×240.
- **Older watches are untested.** The library is built for `arm64_32` too (Series 4–8, SE), but those chips are slower, and the US version needs 33 ms per frame (30 fps).

## Audio

- The audio session must use the `.playback` category. With `.ambient` the engine ran but the watch stayed silent.
- PAL (European) ROMs must run at 25 frames per second. At 30 the game produces 20% too much audio, so sound comes out rushed and the game runs fast. The core reports its rate (`watch_port_frame_rate()`) and the app paces itself with it.

## Not tested yet

- **Battery drain** of the watch while playing, and of the bridge.
- **Save files.** The game writes its EEPROM save into the app's Documents folder, but saving and reloading hasn't been verified.
- **Latency** of the controller path (controller → bridge → watch). It feels immediate but hasn't been measured.
- **The US and Japanese ROMs.** The build supports them; only the European one was played.

## Development quirks

- **Installing on the watch often times out** ("tunnel connection failed"). Keep the watch awake and unlocked, close to the Mac and iPhone, and retry; usually the second attempt works.
- **macOS ships GNU make 3.81**, which the decomp's Makefile doesn't support. Install GNU make 4 (`brew install make`, used as `gmake`).
- **One sound file needs GNU `as`** (`brew install aarch64-elf-binutils`); clang's assembler rejects its macros.
- The watchOS SDK's **fortify macros clash with libultra's `os_libc.h`**; the compiler wrapper turns fortify off.

## Distribution

- The repository contains **no Nintendo code or assets**. Each person builds the game from their own ROM; the build checks its SHA-1.
- There are no screenshots of the game in the repository, only of the test scene.
- The app isn't meant for the App Store.
