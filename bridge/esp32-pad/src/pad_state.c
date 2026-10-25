#include "pad_state.h"

// Xbox Wireless (Bluetooth LE) gamepad report, 16 bytes:
//   0-7   left X, left Y, right X, right Y: uint16 LE, centre 0x8000, Y grows downwards
//   8-11  left / right trigger: uint16 LE, 0..1023
//   12    d-pad as a hat: 0 = released, 1 = up, then clockwise to 8 = up-left
//   13    A=0x01 B=0x02 X=0x08 Y=0x10 LB=0x40 RB=0x80
//   14    View=0x04 Menu=0x08 Guide=0x10 LS=0x20 RS=0x40
//   15    Share=0x01
// The serial log prints raw reports, so the layout can be checked against a
// real controller (see the README in this folder).

static int8_t axis(const uint8_t *p, bool invert) {
    int v = (int)(p[0] | (p[1] << 8)) - 0x8000;
    v /= 258; // -32768..32767 -> -127..127
    if (v > 127) v = 127;
    if (v < -127) v = -127;
    return (int8_t)(invert ? -v : v);
}

static uint8_t trigger(const uint8_t *p) {
    int v = p[0] | (p[1] << 8);
    return (uint8_t)(v > 1023 ? 255 : v >> 2);
}

bool pad_parse_xbox_report(const uint8_t *data, size_t len, struct pad_state *out) {
    if (len < 16) {
        return false;
    }
    out->lx = axis(&data[0], false);
    out->ly = axis(&data[2], true);
    out->rx = axis(&data[4], false);
    out->ry = axis(&data[6], true);
    out->lt = trigger(&data[8]);
    out->rt = trigger(&data[10]);

    uint16_t b = 0;
    static const uint16_t hat[9] = {
        0,
        PAD_BTN_DUP,
        PAD_BTN_DUP | PAD_BTN_DRIGHT,
        PAD_BTN_DRIGHT,
        PAD_BTN_DDOWN | PAD_BTN_DRIGHT,
        PAD_BTN_DDOWN,
        PAD_BTN_DDOWN | PAD_BTN_DLEFT,
        PAD_BTN_DLEFT,
        PAD_BTN_DUP | PAD_BTN_DLEFT,
    };
    if (data[12] <= 8) {
        b |= hat[data[12]];
    }
    if (data[13] & 0x01) b |= PAD_BTN_A;
    if (data[13] & 0x02) b |= PAD_BTN_B;
    if (data[13] & 0x08) b |= PAD_BTN_X;
    if (data[13] & 0x10) b |= PAD_BTN_Y;
    if (data[13] & 0x40) b |= PAD_BTN_LB;
    if (data[13] & 0x80) b |= PAD_BTN_RB;
    if (data[14] & 0x04) b |= PAD_BTN_VIEW;
    if (data[14] & 0x08) b |= PAD_BTN_MENU;
    if (data[14] & 0x10) b |= PAD_BTN_GUIDE;
    if (data[14] & 0x20) b |= PAD_BTN_LS;
    if (data[14] & 0x40) b |= PAD_BTN_RS;
    if (data[15] & 0x01) b |= PAD_BTN_SHARE;
    out->buttons = b;
    return true;
}
