import Foundation
#if canImport(GameController)
import GameController
#endif

/// Merges touch and Digital Crown input (plus gamepads on platforms with the
/// GameController framework, which watchOS lacks) into one N64 controller
/// state, which the game thread hands to the core every frame.
///
/// GameView decides what touches mean; this class only holds the result:
/// the stick, buttons held while a finger is on them, and timed presses.
final class InputState {
    private let lock = NSLock()
    private var touchStick = CGVector.zero // -1...1 on each axis
    private var timed: [UInt16: Int] = [:] // button -> frames left to hold it
    private var heldButtons: UInt16 = 0     // held as long as the finger stays
    private var gamepadButtons: UInt16 = 0  // from a controller via the RetroPad bridge
    private var gamepadStick = CGVector.zero

    // Called from the main thread.

    func setTouchStick(_ v: CGVector) {
        lock.withLock { touchStick = v }
    }

    /// Holds a WATCH_BTN_* button down for the next `frames` game frames.
    func press(_ button: Int, frames: Int = 4) {
        lock.withLock { timed[UInt16(button)] = frames }
    }

    /// Buttons that stay down until the next call (0 releases them).
    func setHeld(_ buttons: UInt16) {
        lock.withLock { heldButtons = buttons }
    }

    /// A game controller's state, already in N64 terms (any thread).
    func setGamepad(buttons: UInt16, stick: CGVector) {
        lock.withLock {
            gamepadButtons = buttons
            gamepadStick = stick
        }
    }

    func crownTurned(by delta: Double) {
        guard abs(delta) > 0.01 else { return }
        press(delta > 0 ? WATCH_BTN_C_RIGHT : WATCH_BTN_C_LEFT, frames: 3)
    }

    // Called from the game thread, once per frame.

    func apply() {
        var buttons: UInt16 = 0
        var stick = CGVector.zero
        lock.withLock {
            stick = touchStick
            if abs(gamepadStick.dx) + abs(gamepadStick.dy) > abs(stick.dx) + abs(stick.dy) {
                stick = gamepadStick
            }
            buttons |= heldButtons | gamepadButtons
            for (button, frames) in timed {
                buttons |= button
                timed[button] = frames > 1 ? frames - 1 : nil
            }
        }

        #if canImport(GameController)
        if let pad = GCController.current?.extendedGamepad {
            buttons |= Self.buttons(from: pad)
            let x = pad.leftThumbstick.xAxis.value, y = pad.leftThumbstick.yAxis.value
            if abs(x) > abs(stick.dx) { stick.dx = CGFloat(x) }
            if abs(y) > abs(stick.dy) { stick.dy = CGFloat(y) }
        }
        #endif

        watch_input_set(buttons, Self.axis(stick.dx), Self.axis(stick.dy))
    }

    private static func axis(_ v: CGFloat) -> Int8 {
        Int8(max(-80, min(80, (v * 80).rounded())))
    }

    #if canImport(GameController)
    private static func buttons(from pad: GCExtendedGamepad) -> UInt16 {
        var b: UInt16 = 0
        func map(_ input: GCControllerButtonInput, _ bit: Int) {
            if input.isPressed { b |= UInt16(bit) }
        }
        map(pad.buttonA, WATCH_BTN_A)
        map(pad.buttonX, WATCH_BTN_B)
        map(pad.leftTrigger, WATCH_BTN_Z)
        map(pad.buttonMenu, WATCH_BTN_START)
        map(pad.leftShoulder, WATCH_BTN_L)
        map(pad.rightShoulder, WATCH_BTN_R)
        map(pad.dpad.up, WATCH_BTN_D_UP)
        map(pad.dpad.down, WATCH_BTN_D_DOWN)
        map(pad.dpad.left, WATCH_BTN_D_LEFT)
        map(pad.dpad.right, WATCH_BTN_D_RIGHT)
        // Right stick acts as the four C buttons.
        let rx = pad.rightThumbstick.xAxis.value, ry = pad.rightThumbstick.yAxis.value
        if rx > 0.5 { b |= UInt16(WATCH_BTN_C_RIGHT) }
        if rx < -0.5 { b |= UInt16(WATCH_BTN_C_LEFT) }
        if ry > 0.5 { b |= UInt16(WATCH_BTN_C_UP) }
        if ry < -0.5 { b |= UInt16(WATCH_BTN_C_DOWN) }
        return b
    }
    #endif
}
