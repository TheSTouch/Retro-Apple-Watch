// Glue shared by the port's C files. Not visible to Swift.

#ifndef PORT_INTERNAL_H
#define PORT_INTERNAL_H

#include <stdint.h>

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;
struct AudioAPI;

extern struct GfxRenderingAPI gfx_soft_api;
extern struct GfxWindowManagerAPI gfx_watch_wm_api;
extern struct AudioAPI audio_watch_api;

// Latest controller state set by watch_input_set().
void watch_input_get(uint16_t *buttons, int8_t *stick_x, int8_t *stick_y);

#endif
