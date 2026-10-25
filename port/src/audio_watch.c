// AudioAPI backed by a single-producer / single-consumer ring buffer.
// The game thread pushes samples with play(); the AVAudioEngine render
// callback on the Swift side drains them with watch_audio_pull().

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio/audio_api.h"

#include "watch_port.h"
#include "port_internal.h"

#define SAMPLE_RATE 32000
#define RING_FRAMES 8192 // power of two, about 250 ms
// The PC port aims to keep this many frames queued (same value as its SDL backend).
#define DESIRED_BUFFERED 1100

static int16_t ring[RING_FRAMES * 2];
static _Atomic size_t write_pos; // in frames, only grows
static _Atomic size_t read_pos;

static bool audio_init(void) {
    return true;
}

static int audio_buffered(void) {
    return (int)(atomic_load(&write_pos) - atomic_load(&read_pos));
}

static int audio_get_desired_buffered(void) {
    return DESIRED_BUFFERED;
}

static void audio_play(const uint8_t *buf, size_t len) {
    const int16_t *samples = (const int16_t *)buf;
    size_t frames = len / 4;
    size_t w = atomic_load(&write_pos);
    size_t free_frames = RING_FRAMES - (w - atomic_load(&read_pos));
    if (frames > free_frames) {
        frames = free_frames; // the consumer stalled (app paused): drop the overflow
    }
    for (size_t i = 0; i < frames; i++) {
        size_t slot = (w + i) & (RING_FRAMES - 1);
        ring[slot * 2] = samples[i * 2];
        ring[slot * 2 + 1] = samples[i * 2 + 1];
    }
    atomic_store(&write_pos, w + frames);
}

int watch_audio_sample_rate(void) {
    return SAMPLE_RATE;
}

size_t watch_audio_pull(int16_t *dst, size_t frames) {
    size_t r = atomic_load(&read_pos);
    size_t available = atomic_load(&write_pos) - r;
    if (frames > available) {
        frames = available;
    }
    for (size_t i = 0; i < frames; i++) {
        size_t slot = (r + i) & (RING_FRAMES - 1);
        dst[i * 2] = ring[slot * 2];
        dst[i * 2 + 1] = ring[slot * 2 + 1];
    }
    atomic_store(&read_pos, r + frames);
    return frames;
}

struct AudioAPI audio_watch_api = {
    audio_init,
    audio_buffered,
    audio_get_desired_buffered,
    audio_play,
};
