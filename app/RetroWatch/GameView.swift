import SwiftUI

struct GameView: View {
    @StateObject private var core = GameCore()
    @Environment(\.scenePhase) private var scenePhase
    @State private var crown = 0.0
    @State private var lastCrown = 0.0
    @State private var touchStart: Date?

    var body: some View {
        GeometryReader { geo in
            ZStack {
                Color.black
                if let frame = core.frame {
                    Image(decorative: frame, scale: 1)
                        .resizable()
                        .interpolation(.none)
                        .aspectRatio(CGFloat(WATCH_FB_WIDTH) / CGFloat(WATCH_FB_HEIGHT), contentMode: .fit)
                }
                VStack {
                    Spacer()
                    Text(String(format: "%.1f ms", core.frameTimeMs))
                        .font(.system(size: 10).monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }
            .contentShape(Rectangle())
            .gesture(touchStick(radius: min(geo.size.width, geo.size.height) * 0.25))
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

    /// Drag from wherever the finger lands: the offset is the stick. A short,
    /// still touch is a tap (A button); a still touch held for half a second
    /// is Start, sent when the finger lifts.
    private func touchStick(radius: CGFloat) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                if touchStart == nil { touchStart = Date() }
                let dx = value.translation.width / radius
                let dy = -value.translation.height / radius
                let length = max(1, (dx * dx + dy * dy).squareRoot())
                core.input.setTouchStick(CGVector(dx: dx / length, dy: dy / length))
            }
            .onEnded { value in
                let moved = hypot(value.translation.width, value.translation.height)
                if let start = touchStart, moved < 8 {
                    let held = Date().timeIntervalSince(start)
                    if held < 0.3 {
                        core.input.tap()
                    } else if held >= 0.5 {
                        core.input.pressStart()
                    }
                }
                touchStart = nil
                core.input.setTouchStick(.zero)
            }
    }
}
