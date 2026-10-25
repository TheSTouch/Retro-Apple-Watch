// Audio and controller glue between libultraship and the watch app.
//
// Audio: an AudioPlayer backed by a single-producer / single-consumer ring
// buffer, as port/src/audio_watch.c does for Super Mario 64. The game's audio
// thread pushes samples; the AVAudioEngine render callback on the Swift side
// drains them with watch_audio_pull().
//
// Controller: the state the Swift side sets with watch_input_set() (kept by
// port/src/input_watch.c) is merged into the N64 pad the game reads.

#include <atomic>
#include <cstring>

#include "ship/audio/AudioPlayer.h"

#include "watch_lus.h"
#include "watch_port.h"

extern "C" void watch_input_get(uint16_t* buttons, int8_t* stick_x, int8_t* stick_y);

namespace {

constexpr int SAMPLE_RATE = 32000;
constexpr size_t RING_FRAMES = 8192; // power of two, about 250 ms
// The game makes a frame's worth (about 1600 sample frames) in one go, 20
// times a second, and aims to keep this many queued. Ship of Harkinian's
// own 1680 is too little for the watch's audio engine, which pulls large
// chunks.
constexpr int DESIRED_BUFFERED = 3200;

int16_t sRing[RING_FRAMES * 2];
std::atomic<size_t> sWritePos; // in frames, only grows
std::atomic<size_t> sReadPos;

// For the frame log: sample frames the output asked for but did not get,
// sample frames dropped because the ring was full, and the largest request.
std::atomic<size_t> sUnderrunFrames;
std::atomic<size_t> sDroppedFrames;
std::atomic<size_t> sLargestPull;

class WatchAudioPlayer final : public Ship::AudioPlayer {
  public:
    WatchAudioPlayer(Ship::AudioSettings settings) : AudioPlayer(settings) {
    }

    int32_t Buffered() override {
        return (int32_t)(sWritePos.load() - sReadPos.load());
    }

  protected:
    bool DoInit() override {
        return true;
    }

    void DoClose() override {
    }

    void DoPlay(const uint8_t* buf, size_t len) override {
        const int16_t* samples = (const int16_t*)buf;
        size_t frames = len / 4;
        size_t w = sWritePos.load();
        size_t free_frames = RING_FRAMES - (w - sReadPos.load());
        if (frames > free_frames) {
            sDroppedFrames += frames - free_frames;
            frames = free_frames; // the consumer stalled (app paused): drop the overflow
        }
        for (size_t i = 0; i < frames; i++) {
            size_t slot = (w + i) & (RING_FRAMES - 1);
            sRing[slot * 2] = samples[i * 2];
            sRing[slot * 2 + 1] = samples[i * 2 + 1];
        }
        sWritePos.store(w + frames);
    }
};

} // namespace

namespace Ship {
std::shared_ptr<AudioPlayer> WatchCreateAudioPlayer(AudioSettings settings) {
    settings.SampleRate = SAMPLE_RATE;
    settings.DesiredBuffered = DESIRED_BUFFERED;
    return std::make_shared<WatchAudioPlayer>(settings);
}
} // namespace Ship

void WatchApplyInput(uint32_t* button, int8_t* stick_x, int8_t* stick_y) {
    uint16_t buttons;
    int8_t x, y;
    watch_input_get(&buttons, &x, &y);
    *button |= buttons;
    if (x != 0 || y != 0) {
        *stick_x = x;
        *stick_y = y;
    }
}

extern "C" int watch_audio_sample_rate(void) {
    return SAMPLE_RATE;
}

void WatchTakeAudioStats(size_t* underrun, size_t* dropped, size_t* largest_pull, size_t* buffered) {
    *underrun = sUnderrunFrames.exchange(0);
    *dropped = sDroppedFrames.exchange(0);
    *largest_pull = sLargestPull.exchange(0);
    *buffered = sWritePos.load() - sReadPos.load();
}

extern "C" size_t watch_audio_pull(int16_t* dst, size_t frames) {
    size_t r = sReadPos.load();
    size_t available = sWritePos.load() - r;
    if (frames > sLargestPull.load()) {
        sLargestPull.store(frames);
    }
    if (frames > available) {
        sUnderrunFrames += frames - available;
        frames = available;
    }
    for (size_t i = 0; i < frames; i++) {
        size_t slot = (r + i) & (RING_FRAMES - 1);
        dst[i * 2] = sRing[slot * 2];
        dst[i * 2 + 1] = sRing[slot * 2 + 1];
    }
    sReadPos.store(r + frames);
    return frames;
}
