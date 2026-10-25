// Controller state shared between the Swift input code and the game thread,
// packed into one atomic word so a read never sees a half-updated state.

#include <stdatomic.h>
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
