import SwiftUI

/// The game image at the top, a bar of buttons under it.
///
/// The watch screen tracks one finger, so the whole screen is a single touch
/// surface. Dragging on the game image is the stick. Sliding the finger down
/// onto a button presses it while the stick keeps the direction it last had,
/// so "drag to run, slide onto A" is a running jump.
///
/// On the game image, a tap is A (or, in the file select, points and clicks
/// right where it lands) and a still half-second press is Start.
struct GameView: View {
    @StateObject private var core = GameCore()
    @Environment(\.scenePhase) private var scenePhase
    @State private var crown = 0.0
    @State private var lastCrown = 0.0
    @State private var touch: Touch?
    @State private var pressedButton: BarButton?

    private struct Touch {
        let start: Date
        let startedOnGame: Bool
    }

    private enum BarButton: CaseIterable {
        case a, b, z

        var label: String {
            switch self {
            case .a: "A"
            case .b: "B"
            case .z: "Z"
            }
        }

        var bit: Int {
            switch self {
            case .a: WATCH_BTN_A
            case .b: WATCH_BTN_B
            case .z: WATCH_BTN_Z
            }
        }

        var color: Color {
            switch self {
            case .a: .blue
            case .b: .green
            case .z: .gray
            }
        }
    }

    var body: some View {
        GeometryReader { geo in
            let layout = Layout(size: geo.size)
            ZStack(alignment: .topLeading) {
                Color.black
                if let frame = core.frame {
                    Image(decorative: frame, scale: 1)
                        .resizable()
                        .interpolation(.none)
                        .frame(width: layout.game.width, height: layout.game.height)
                        .offset(x: layout.game.minX, y: layout.game.minY)
                }
                ForEach(BarButton.allCases, id: \.self) { button in
                    let rect = layout.rect(for: button)
                    RoundedRectangle(cornerRadius: 14)
                        .fill(button.color.opacity(pressedButton == button ? 0.9 : 0.45))
                        .overlay(Text(button.label).font(.system(size: 22, weight: .bold)))
                        .frame(width: rect.width, height: rect.height)
                        .offset(x: rect.minX, y: rect.minY)
                }
                Text(String(format: "%.1f ms", core.frameTimeMs))
                    .font(.system(size: 9).monospacedDigit())
                    .foregroundStyle(.secondary)
                    .frame(width: geo.size.width)
                    .offset(y: geo.size.height - 14)
            }
            .contentShape(Rectangle())
            .gesture(touchGesture(layout))
        }
        .ignoresSafeArea()
        .focusable()
        .digitalCrownRotation($crown, from: -1_000_000, through: 1_000_000, by: 0.1,
                              sensitivity: .medium, isContinuous: true, isHapticFeedbackEnabled: false)
        .onChange(of: crown) { _, value in
            core.input.crownTurned(by: value - lastCrown)
            lastCrown = value
        }
        .onChange(of: scenePhase, initial: true) { _, phase in
            core.setActive(phase == .active)
        }
    }

    private struct Layout {
        let game: CGRect
        let bar: CGRect

        init(size: CGSize) {
            let gameHeight = size.width * CGFloat(WATCH_FB_HEIGHT) / CGFloat(WATCH_FB_WIDTH)
            game = CGRect(x: 0, y: 0, width: size.width, height: gameHeight)
            bar = CGRect(x: 0, y: gameHeight, width: size.width, height: size.height - gameHeight)
        }

        /// Buttons sit right under the game, so the slide from the stick is short.
        func rect(for button: BarButton) -> CGRect {
            let index = CGFloat(BarButton.allCases.firstIndex(of: button)!)
            let count = CGFloat(BarButton.allCases.count)
            let gap: CGFloat = 6
            let width = (bar.width - gap * (count + 1)) / count
            let height = min(bar.height - 24, 80)
            return CGRect(x: gap + index * (width + gap), y: bar.minY + gap, width: width, height: height)
        }

        func button(at point: CGPoint) -> BarButton? {
            guard point.y >= bar.minY else { return nil }
            // Whole columns count, so a finger slightly off a button still hits it.
            let count = BarButton.allCases.count
            let column = Int(point.x / (bar.width / CGFloat(count)))
            return BarButton.allCases[max(0, min(count - 1, column))]
        }

        /// A point on the game image in framebuffer pixels.
        func framebufferPoint(_ p: CGPoint) -> CGPoint {
            CGPoint(x: (p.x - game.minX) / game.width * CGFloat(WATCH_FB_WIDTH),
                    y: (p.y - game.minY) / game.height * CGFloat(WATCH_FB_HEIGHT))
        }
    }

    private func touchGesture(_ layout: Layout) -> some Gesture {
        let radius = layout.game.width * 0.25
        return DragGesture(minimumDistance: 0)
            .onChanged { value in
                if touch == nil {
                    touch = Touch(start: Date(), startedOnGame: layout.game.contains(value.startLocation))
                }
                if let button = layout.button(at: value.location) {
                    // On the bar: press the button under the finger, keep the stick.
                    if button != pressedButton {
                        pressedButton = button
                        core.input.setHeld(UInt16(button.bit))
                        core.input.press(button.bit, frames: 2) // even a brief touch registers
                    }
                    return
                }
                if pressedButton != nil {
                    pressedButton = nil
                    core.input.setHeld(0)
                }
                guard touch?.startedOnGame == true else { return }
                let dx = value.translation.width / radius
                let dy = -value.translation.height / radius
                let length = max(1, (dx * dx + dy * dy).squareRoot())
                core.input.setTouchStick(CGVector(dx: dx / length, dy: dy / length))
            }
            .onEnded { value in
                defer {
                    touch = nil
                    pressedButton = nil
                    core.input.setHeld(0)
                    core.input.setTouchStick(.zero)
                }
                guard let touch, touch.startedOnGame, layout.game.contains(value.location),
                      hypot(value.translation.width, value.translation.height) < 8 else { return }
                let held = Date().timeIntervalSince(touch.start)
                if held < 0.3 {
                    if watch_port_menu_pointer_active() != 0 {
                        let p = layout.framebufferPoint(value.location)
                        watch_input_pointer_tap(Float(p.x), Float(p.y))
                    } else {
                        core.input.press(WATCH_BTN_A)
                    }
                } else if held >= 0.5 {
                    core.input.press(WATCH_BTN_START)
                }
            }
    }
}
