// Tap-to-point for the file select menu. patches/0001 makes the menu's cursor
// code call watch_menu_cursor_hook() every frame; a pending screen tap moves
// the cursor there, and the next controller read clicks with A.

#include <stdatomic.h>

#include <ultra64.h>

#include "watch_port.h"
#include "../src/port_internal.h"

#include "menu_watch.h"

// The menu works in screen pixels centred on the 320x240 image, y up.
#define CURSOR_ORIGIN_X 160.0f
#define CURSOR_ORIGIN_Y 120.0f

static _Atomic uint32_t frame_count;
static _Atomic uint32_t hook_frame; // frame_count when the hook last ran, plus one

// Game thread only.
static enum { CLICK_NONE, CLICK_PRESS, CLICK_RELEASE } click_state;
static float tap_x, tap_y;

void watch_menu_frame_tick(void) {
    atomic_fetch_add(&frame_count, 1);
}

int watch_port_menu_pointer_active(void) {
    uint32_t seen = atomic_load(&hook_frame);
    return seen != 0 && atomic_load(&frame_count) + 1 - seen <= 2;
}

void watch_menu_cursor_hook(f32 *cursor_pos) {
    atomic_store(&hook_frame, atomic_load(&frame_count) + 1);
    float x, y;
    if (watch_input_take_pointer(&x, &y)) {
        tap_x = x - CURSOR_ORIGIN_X;
        tap_y = CURSOR_ORIGIN_Y - y;
        click_state = CLICK_PRESS;
    }
    if (click_state != CLICK_NONE) {
        cursor_pos[0] = tap_x;
        cursor_pos[1] = tap_y;
    }
}

uint16_t watch_menu_extra_buttons(void) {
    switch (click_state) {
        case CLICK_PRESS:
            click_state = CLICK_RELEASE;
            return A_BUTTON;
        case CLICK_RELEASE:
            click_state = CLICK_NONE;
            return 0;
        default:
            return 0;
    }
}
