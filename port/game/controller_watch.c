// libultra controller calls, answered from the watch's input state. Takes the
// place of the PC port's controller_entry_point.c and its desktop drivers.

#include <ultra64.h>

#include "macros.h"

#include "watch_port.h"
#include "../src/port_internal.h"

s32 osContInit(UNUSED OSMesgQueue *mq, u8 *controllerBits, UNUSED OSContStatus *status) {
    *controllerBits = 1; // one controller, in port 1
    return 0;
}

s32 osContStartReadData(UNUSED OSMesgQueue *mq) {
    return 0;
}

void osContGetReadData(OSContPad *pad) {
    uint16_t buttons;
    int8_t stick_x, stick_y;
    watch_input_get(&buttons, &stick_x, &stick_y);
    pad->button = buttons;
    pad->stick_x = stick_x;
    pad->stick_y = stick_y;
    pad->errnum = 0;
}
