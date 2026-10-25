import CoreGraphics
import Foundation

/// Runs the C core on its own thread at 30 Hz and publishes finished frames.
final class GameCore: ObservableObject {
    @Published private(set) var frame: CGImage?
    @Published private(set) var frameTimeMs: Double = 0

    let input = InputState()
    private let audio = AudioOutput()

    private let width = Int(WATCH_FB_WIDTH)
    private let height = Int(WATCH_FB_HEIGHT)
    private let lock = NSLock()
    private var running = false
    private var thread: Thread?

    init() {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        watch_port_init(documents.path)
    }

    func setActive(_ active: Bool) {
        lock.lock()
        defer { lock.unlock() }
        guard active != running else { return }
        running = active
        if active {
            audio.start()
            let thread = Thread { [weak self] in self?.loop() }
            thread.qualityOfService = .userInteractive
            thread.name = "game"
            self.thread = thread
            thread.start()
        } else {
            audio.stop()
            thread = nil
        }
    }

    private var isRunning: Bool {
        lock.lock()
        defer { lock.unlock() }
        return running
    }

    private func loop() {
        let framePeriod = 1.0 / 30.0
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        var lastSeq: UInt32 = 0
        var nextTick = Date.timeIntervalSinceReferenceDate

        while isRunning {
            input.apply()
            let start = Date.timeIntervalSinceReferenceDate
            watch_port_run_frame()
            let elapsed = Date.timeIntervalSinceReferenceDate - start

            let seq = watch_port_copy_frame(&pixels)
            if seq != lastSeq, let image = makeImage(pixels) {
                lastSeq = seq
                DispatchQueue.main.async {
                    self.frame = image
                    self.frameTimeMs = elapsed * 1000
                }
            }

            nextTick += framePeriod
            let now = Date.timeIntervalSinceReferenceDate
            if nextTick > now {
                Thread.sleep(forTimeInterval: nextTick - now)
            } else {
                nextTick = now // running slow: don't try to catch up
            }
        }
    }

    private func makeImage(_ pixels: [UInt8]) -> CGImage? {
        guard let provider = CGDataProvider(data: Data(pixels) as CFData) else { return nil }
        return CGImage(
            width: width, height: height,
            bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
            provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}
