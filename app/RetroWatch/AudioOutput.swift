import AVFoundation

/// Plays the core's 16-bit stereo output through AVAudioEngine.
final class AudioOutput {
    private let engine = AVAudioEngine()
    private var source: AVAudioSourceNode?
    private var scratch = [Int16](repeating: 0, count: 4096 * 2)

    func start() {
        if source == nil {
            let format = AVAudioFormat(standardFormatWithSampleRate: Double(watch_audio_sample_rate()), channels: 2)!
            let node = AVAudioSourceNode(format: format) { [unowned self] _, _, frameCount, bufferList in
                self.render(frames: Int(frameCount), into: UnsafeMutableAudioBufferListPointer(bufferList))
                return noErr
            }
            engine.attach(node)
            engine.connect(node, to: engine.mainMixerNode, format: format)
            source = node
        }
        // .playback (not .ambient) so the watch speaker plays it, silent mode or not.
        let session = AVAudioSession.sharedInstance()
        do {
            try session.setCategory(.playback, mode: .default, policy: .default)
            try session.setActive(true)
            try engine.start()
        } catch {
            print("audio: \(error)")
        }
        print("audio: running=\(engine.isRunning) outputs=\(session.currentRoute.outputs.map(\.portType.rawValue))")
    }

    func stop() {
        engine.pause()
    }

    // Runs on the audio render thread.
    private func render(frames: Int, into buffers: UnsafeMutableAudioBufferListPointer) {
        let frames = min(frames, scratch.count / 2)
        let got = scratch.withUnsafeMutableBufferPointer { watch_audio_pull($0.baseAddress, frames) }
        let left = buffers[0].mData!.assumingMemoryBound(to: Float.self)
        let right = buffers[1].mData!.assumingMemoryBound(to: Float.self)
        for i in 0..<frames {
            if i < got {
                left[i] = Float(scratch[i * 2]) / 32768
                right[i] = Float(scratch[i * 2 + 1]) / 32768
            } else {
                left[i] = 0
                right[i] = 0
            }
        }
    }
}
