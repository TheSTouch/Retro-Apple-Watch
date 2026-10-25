#ifndef MENU_WATCH_H
#define MENU_WATCH_H

#include <stdint.h>

// Called once per game frame, before the game runs.
void watch_menu_frame_tick(void);

// Buttons to add to the controller read (A for the frame after a menu tap).
uint16_t watch_menu_extra_buttons(void);

#endif
