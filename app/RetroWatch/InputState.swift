import Foundation
#if canImport(GameController)
import GameController
#endif

/// Merges touch and Digital Crown input (plus gamepads on platforms with the
/// GameController framework, which watchOS lacks) into one N64 controller
/// state, which the game thread hands to the core every frame.
///
/// First-pass touch scheme (the watch screen only tracks one finger):
/// drag = analog stick, tap = A, long press = Start, Digital Crown = C-left /
/// C-right.
final class InputState {
    private let lock = NSLock()
    private var touchStick = CGVector.zero // -1...1 on each axis
    private var tapFramesLeft = 0
    private var startFramesLeft = 0
    private var crownFramesLeft = 0
    private var crownButton: UInt16 = 0

    // Called from the main thread.

    func setTouchStick(_ v: CGVector) {
        lock.withLock { touchStick = v }
    }

    func tap() {
        lock.withLock { tapFramesLeft = 4 }
    }

    func pressStart() {
        lock.withLock { startFramesLeft = 4 }
    }

    func crownTurned(by delta: Double) {
        guard abs(delta) > 0.01 else { return }
        lock.withLock {
            crownButton = UInt16(delta > 0 ? WATCH_BTN_C_RIGHT : WATCH_BTN_C_LEFT)
            crownFramesLeft = 3
        }
    }

    // Called from the game thread, once per frame.

    func apply() {
        var buttons: UInt16 = 0
        var stick = CGVector.zero
        lock.withLock {
            stick = touchStick
            if tapFramesLeft > 0 {
                buttons |= UInt16(WATCH_BTN_A)
                tapFramesLeft -= 1
            }
            if startFramesLeft > 0 {
                buttons |= UInt16(WATCH_BTN_START)
                startFramesLeft -= 1
            }
            if crownFramesLeft > 0 {
                buttons |= crownButton
                crownFramesLeft -= 1
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
