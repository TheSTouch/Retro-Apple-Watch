// GfxWindowManagerAPI for the watch. There is no window: the Swift side pulls
// finished frames with watch_port_copy_frame() and drives the frame loop, so
// most of these are no-ops.

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "gfx/gfx_window_manager_api.h"

#include "watch_port.h"
#include "port_internal.h"

static void wm_init(const char *game_name, bool start_in_fullscreen) {
    (void)game_name;
    (void)start_in_fullscreen;
}

static void wm_set_keyboard_callbacks(bool (*on_key_down)(int scancode), bool (*on_key_up)(int scancode),
                                      void (*on_all_keys_up)(void)) {
    (void)on_key_down;
    (void)on_key_up;
    (void)on_all_keys_up;
}

static void wm_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
    (void)on_fullscreen_changed;
}

static void wm_set_fullscreen(bool enable) {
    (void)enable;
}

static void wm_main_loop(void (*run_one_game_iter)(void)) {
    run_one_game_iter();
}

static void wm_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = WATCH_FB_WIDTH;
    *height = WATCH_FB_HEIGHT;
}

static void wm_handle_events(void) {
}

static bool wm_start_frame(void) {
    return true;
}

static void wm_swap_buffers_begin(void) {
}

static void wm_swap_buffers_end(void) {
}

static double wm_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

struct GfxWindowManagerAPI gfx_watch_wm_api = {
    wm_init,
    wm_set_keyboard_callbacks,
    wm_set_fullscreen_changed_callback,
    wm_set_fullscreen,
    wm_main_loop,
    wm_get_dimensions,
    wm_handle_events,
    wm_start_frame,
    wm_swap_buffers_begin,
    wm_swap_buffers_end,
    wm_get_time,
};
