// The controller state the bridge sends to the watch, and the parser that
// fills it from an Xbox Wireless controller's Bluetooth LE input report.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAD_STATE_VERSION 2

// pad_state.flags
#define PAD_FLAG_CONTROLLER 0x01 // a controller is connected and sending reports

enum {
    PAD_BTN_A = 1 << 0,
    PAD_BTN_B = 1 << 1,
    PAD_BTN_X = 1 << 2,
    PAD_BTN_Y = 1 << 3,
    PAD_BTN_LB = 1 << 4,
    PAD_BTN_RB = 1 << 5,
    PAD_BTN_VIEW = 1 << 6,
    PAD_BTN_MENU = 1 << 7,
    PAD_BTN_LS = 1 << 8,
    PAD_BTN_RS = 1 << 9,
    PAD_BTN_DUP = 1 << 10,
    PAD_BTN_DDOWN = 1 << 11,
    PAD_BTN_DLEFT = 1 << 12,
    PAD_BTN_DRIGHT = 1 << 13,
    PAD_BTN_GUIDE = 1 << 14,
    PAD_BTN_SHARE = 1 << 15,
};

// Sent as-is (little-endian, 11 bytes) in the bridge's state characteristic.
struct __attribute__((packed)) pad_state {
    uint8_t version; // PAD_STATE_VERSION
    uint8_t seq;     // increases with every change
    uint16_t buttons; // PAD_BTN_* bits
    int8_t lx, ly, rx, ry; // -127..127, +y is up
    uint8_t lt, rt;  // 0..255
    uint8_t flags;   // PAD_FLAG_* (added in version 2)
};

// Parses an Xbox Wireless controller input report (the 16-byte gamepad
// report). Returns false if the report has another shape.
bool pad_parse_xbox_report(const uint8_t *data, size_t len, struct pad_state *out);
