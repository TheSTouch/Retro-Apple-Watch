// C interface between the watchOS app (Swift) and the game core.
//
// The game core runs on one dedicated thread that calls watch_port_run_frame()
// at watch_port_frame_rate() Hz. Every other function here is safe to call
// from any thread.

#ifndef WATCH_PORT_H
#define WATCH_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Internal render resolution. The N64 drew 320x240; the watch scales it up.
#define WATCH_FB_WIDTH 320
#define WATCH_FB_HEIGHT 240

// N64 controller button bits, as the game reads them from OSContPad.button.
enum {
    WATCH_BTN_A       = 0x8000,
    WATCH_BTN_B       = 0x4000,
    WATCH_BTN_Z       = 0x2000,
    WATCH_BTN_START   = 0x1000,
    WATCH_BTN_D_UP    = 0x0800,
    WATCH_BTN_D_DOWN  = 0x0400,
    WATCH_BTN_D_LEFT  = 0x0200,
    WATCH_BTN_D_RIGHT = 0x0100,
    WATCH_BTN_L       = 0x0020,
    WATCH_BTN_R       = 0x0010,
    WATCH_BTN_C_UP    = 0x0008,
    WATCH_BTN_C_DOWN  = 0x0004,
    WATCH_BTN_C_LEFT  = 0x0002,
    WATCH_BTN_C_RIGHT = 0x0001,
};

// Starts the core. data_dir is a writable directory for save files.
void watch_port_init(const char *data_dir);

// Runs one game frame (game logic, rendering and audio generation).
void watch_port_run_frame(void);

// How often to call watch_port_run_frame(), in Hz. Super Mario 64: 25 for PAL
// (European) builds, 30 otherwise. Ocarina of Time changes it as it goes (60
// in menus, 20 in play), so callers read it before every frame. Game speed
// and audio depend on getting it right.
int watch_port_frame_rate(void);

// Copies the latest finished frame into dst (WATCH_FB_WIDTH * WATCH_FB_HEIGHT
// pixels, 4 bytes each, RGBX order). Returns a counter that increases with
// every finished frame, so callers can skip frames they already have.
uint32_t watch_port_copy_frame(uint8_t *dst);

// Controller state: a WATCH_BTN_* bitmask and the analog stick (-80..80 is
// the range a real N64 stick reaches).
void watch_input_set(uint16_t buttons, int8_t stick_x, int8_t stick_y);

// Nonzero while the game shows a menu with a free-moving cursor (the file
// select). A tap should then point and click rather than act as a button.
int watch_port_menu_pointer_active(void);

// Moves that menu cursor to a framebuffer position (pixels from the top-left
// of the WATCH_FB_WIDTH x WATCH_FB_HEIGHT image) and clicks there.
void watch_input_pointer_tap(float x, float y);

// Audio produced by the core: signed 16-bit stereo, interleaved.
int watch_audio_sample_rate(void);
// Fills up to `frames` stereo frames and returns how many were available;
// the caller pads the rest with silence.
size_t watch_audio_pull(int16_t *dst, size_t frames);

#ifdef __cplusplus
}
#endif

#endif
