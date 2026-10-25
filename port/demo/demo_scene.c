// Demo core used until a game is built in: a spinning, textured cube sent
// through the real N64 display list interpreter (gfx_pc.c) and the software
// renderer, so the whole graphics path can be tested without any game data.
// The stick spins the cube, A switches between texture and plain shading.

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/mbi.h>

#include "gfx/gfx_pc.h"

#include "watch_port.h"
#include "../src/port_internal.h"

#define TEX_SIZE 32

static uint8_t checker_tex[TEX_SIZE * TEX_SIZE * 2]; // RGBA5551, big-endian like on the N64
static Vtx cube_verts[24];
static Mtx proj_mtx, view_mtx;
static Vp viewport = { { { WATCH_FB_WIDTH * 2, WATCH_FB_HEIGHT * 2, G_MAXZ / 2, 0 },
                         { WATCH_FB_WIDTH * 2, WATCH_FB_HEIGHT * 2, G_MAXZ / 2, 0 } } };
static Gfx display_list[128];

static float yaw, pitch = 0.5f;
static uint16_t prev_buttons;
static int textured = 1;

static uint16_t rgba5551(int r, int g, int b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1);
}

static void build_texture(void) {
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            int light = ((x / 8) + (y / 8)) & 1;
            int border = x == 0 || y == 0 || x == TEX_SIZE - 1 || y == TEX_SIZE - 1;
            uint16_t c = border ? rgba5551(40, 40, 60) : (light ? rgba5551(250, 210, 90) : rgba5551(230, 90, 60));
            checker_tex[(y * TEX_SIZE + x) * 2] = c >> 8;
            checker_tex[(y * TEX_SIZE + x) * 2 + 1] = c & 0xff;
        }
    }
}

static void build_cube(void) {
    // Each face: normal axis, sign, and a tint so faces read differently.
    static const struct { int axis; float sign; uint8_t tint[3]; } faces[6] = {
        { 0, 1, { 255, 255, 255 } }, { 0, -1, { 170, 170, 190 } },
        { 1, 1, { 230, 230, 255 } }, { 1, -1, { 120, 120, 150 } },
        { 2, 1, { 210, 210, 230 } }, { 2, -1, { 150, 150, 170 } },
    };
    static const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int f = 0; f < 6; f++) {
        int u_axis = (faces[f].axis + 1) % 3, v_axis = (faces[f].axis + 2) % 3;
        for (int i = 0; i < 4; i++) {
            Vtx_t *v = &cube_verts[f * 4 + i].v;
            float c0 = corners[i][0], c1 = corners[i][1];
            // Flip one axis on negative faces so every face winds counter-clockwise.
            if (faces[f].sign < 0) {
                c0 = -c0;
            }
            v->ob[faces[f].axis] = faces[f].sign;
            v->ob[u_axis] = c0;
            v->ob[v_axis] = c1;
            v->flag = 0;
            v->tc[0] = (short)((corners[i][0] > 0 ? TEX_SIZE : 0) << 5);
            v->tc[1] = (short)((corners[i][1] > 0 ? TEX_SIZE : 0) << 5);
            v->cn[0] = faces[f].tint[0];
            v->cn[1] = faces[f].tint[1];
            v->cn[2] = faces[f].tint[2];
            v->cn[3] = 255;
        }
    }
}

// N64 matrices use row vectors: clip = v * M.
static void mtx_identity(float m[4][4]) {
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

static void mtx_mul(float out[4][4], float a[4][4], float b[4][4]) {
    float r[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(out, r, sizeof(r));
}

static void build_matrices(void) {
    float fovy = 45.0f * (float)M_PI / 180.0f, aspect = (float)WATCH_FB_WIDTH / WATCH_FB_HEIGHT;
    float near = 1.0f, far = 100.0f, f = 1.0f / tanf(fovy / 2.0f);
    memset(proj_mtx.m, 0, sizeof(proj_mtx.m));
    proj_mtx.m[0][0] = f / aspect;
    proj_mtx.m[1][1] = f;
    proj_mtx.m[2][2] = (near + far) / (near - far);
    proj_mtx.m[2][3] = -1.0f;
    proj_mtx.m[3][2] = 2.0f * near * far / (near - far);

    float ry[4][4], rx[4][4], t[4][4];
    mtx_identity(ry);
    ry[0][0] = cosf(yaw);
    ry[0][2] = -sinf(yaw);
    ry[2][0] = sinf(yaw);
    ry[2][2] = cosf(yaw);
    mtx_identity(rx);
    rx[1][1] = cosf(pitch);
    rx[1][2] = sinf(pitch);
    rx[2][1] = -sinf(pitch);
    rx[2][2] = cosf(pitch);
    mtx_identity(t);
    t[3][2] = -5.0f;
    mtx_mul(view_mtx.m, ry, rx);
    mtx_mul(view_mtx.m, view_mtx.m, t);
}

static Gfx *build_display_list(void) {
    // gfx_pc skips fills whose color image is the depth image (it treats them
    // as z-buffer clears), so point the two at different placeholder addresses.
    static uint16_t color_image_tag, depth_image_tag;
    Gfx *p = display_list;
    gDPPipeSync(p++);
    gDPSetColorImage(p++, G_IM_FMT_RGBA, G_IM_SIZ_16b, WATCH_FB_WIDTH, &color_image_tag);
    gDPSetDepthImage(p++, &depth_image_tag);
    gDPSetScissor(p++, G_SC_NON_INTERLACE, 0, 0, WATCH_FB_WIDTH, WATCH_FB_HEIGHT);
    gSPViewport(p++, &viewport);

    // Background
    gDPSetCycleType(p++, G_CYC_FILL);
    gDPSetRenderMode(p++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetFillColor(p++, (rgba5551(24, 28, 64) << 16) | rgba5551(24, 28, 64));
    gDPFillRectangle(p++, 0, 0, WATCH_FB_WIDTH - 1, WATCH_FB_HEIGHT - 1);
    gDPPipeSync(p++);

    // Cube
    gDPSetCycleType(p++, G_CYC_1CYCLE);
    gDPSetRenderMode(p++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
    gSPClearGeometryMode(p++, 0xffffffff);
    gSPSetGeometryMode(p++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH | G_CULL_BACK);
    gSPMatrix(p++, &proj_mtx, G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPMatrix(p++, &view_mtx, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    if (textured) {
        gDPSetCombineMode(p++, G_CC_MODULATERGB, G_CC_MODULATERGB);
        gSPTexture(p++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
        gDPSetTextureFilter(p++, G_TF_BILERP);
        gDPLoadTextureBlock(p++, checker_tex, G_IM_FMT_RGBA, G_IM_SIZ_16b, TEX_SIZE, TEX_SIZE, 0,
                            G_TX_WRAP, G_TX_WRAP, 5, 5, G_TX_NOLOD, G_TX_NOLOD);
    } else {
        gDPSetCombineMode(p++, G_CC_SHADE, G_CC_SHADE);
        gSPTexture(p++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
    }
    gSPVertex(p++, cube_verts, 24, 0);
    for (int f = 0; f < 6; f++) {
        int b = f * 4;
        gSP2Triangles(p++, b, b + 1, b + 2, 0, b, b + 2, b + 3, 0);
    }
    gSPTexture(p++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_OFF);
    gSPEndDisplayList(p++);
    return display_list;
}

void watch_port_init(const char *data_dir) {
    (void)data_dir;
    build_texture();
    build_cube();
    gfx_init(&gfx_watch_wm_api, &gfx_soft_api, "RetroWatch demo", false);
}

int watch_port_frame_rate(void) {
    return 30;
}

void watch_port_run_frame(void) {
    uint16_t buttons;
    int8_t stick_x, stick_y;
    watch_input_get(&buttons, &stick_x, &stick_y);
    if ((buttons & WATCH_BTN_A) && !(prev_buttons & WATCH_BTN_A)) {
        textured = !textured;
    }
    prev_buttons = buttons;
    yaw += 0.03f + stick_x / 80.0f * 0.12f;
    pitch += stick_y / 80.0f * 0.08f;

    build_matrices();
    gfx_start_frame();
    gfx_run(build_display_list());
    gfx_end_frame();
}
