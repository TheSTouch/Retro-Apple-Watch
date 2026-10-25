// Controller state shared between the Swift input code and the game thread,
// packed into one atomic word so a read never sees a half-updated state.

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "watch_port.h"
#include "port_internal.h"

static _Atomic uint32_t input_state;

void watch_input_set(uint16_t buttons, int8_t stick_x, int8_t stick_y) {
    atomic_store(&input_state, (uint32_t)buttons | ((uint32_t)(uint8_t)stick_x << 16)
                               | ((uint32_t)(uint8_t)stick_y << 24));
}

void watch_input_get(uint16_t *buttons, int8_t *stick_x, int8_t *stick_y) {
    uint32_t s = atomic_load(&input_state);
    *buttons = (uint16_t)s;
    *stick_x = (int8_t)(s >> 16);
    *stick_y = (int8_t)(s >> 24);
}

// A pending menu tap: bit 31 set when present, x and y in 1/8 pixels.
static _Atomic uint32_t pointer_tap;

void watch_input_pointer_tap(float x, float y) {
    uint32_t px = (uint32_t)(x < 0 ? 0 : x * 8) & 0xfff, py = (uint32_t)(y < 0 ? 0 : y * 8) & 0xfff;
    atomic_store(&pointer_tap, 0x80000000u | px | (py << 12));
}

bool watch_input_take_pointer(float *x, float *y) {
    uint32_t t = atomic_exchange(&pointer_tap, 0);
    if (!(t & 0x80000000u)) {
        return false;
    }
    *x = (t & 0xfff) / 8.0f;
    *y = ((t >> 12) & 0xfff) / 8.0f;
    return true;
}
