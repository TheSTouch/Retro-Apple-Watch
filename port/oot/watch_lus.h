// The watch's backends for libultraship (Ship of Harkinian's platform layer).
// patches/oot/ hooks these into libultraship's window and audio setup when
// it is built with LUS_WATCH_PORT_DIR pointing here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace Fast {
class GfxRenderingAPI;
class GfxWindowBackend;

// The window backend id the watch registers. libultraship's own are 1 to 3.
constexpr int WATCH_WINDOW_BACKEND = 4;

// Software rasterizer drawing into the watch's framebuffer (gfx_soft_lus.cpp).
GfxRenderingAPI* WatchCreateRenderingApi();
// Seconds spent drawing triangles since the last call (for measurements).
double WatchTakeDrawSeconds();
// A fixed 320x240 "window" without events (wm_watch_lus.cpp).
GfxWindowBackend* WatchCreateWindowBackend();
// The frame rate the game last asked the window for.
int WatchTargetFps();
} // namespace Fast

namespace Ship {
class AudioPlayer;
struct AudioSettings;

// Feeds the ring buffer the Swift side drains with watch_audio_pull().
std::shared_ptr<AudioPlayer> WatchCreateAudioPlayer(AudioSettings settings);
} // namespace Ship

// Merges the watch's controller state into an N64 pad read (the button
// field is libultraship's CONTROLLERBUTTONS_T, 32 bits in Ship of Harkinian).
void WatchApplyInput(uint32_t* button, int8_t* stick_x, int8_t* stick_y);

// Audio counters since the last call (for measurements): sample frames the
// output went without, frames dropped on a full buffer, the largest single
// request from the output, and what is queued now.
void WatchTakeAudioStats(size_t* underrun, size_t* dropped, size_t* largest_pull, size_t* buffered);
