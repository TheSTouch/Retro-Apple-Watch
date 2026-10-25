// Software rasterizer implementing the PC port's GfxRenderingAPI.
//
// watchOS offers neither OpenGL nor Vulkan, so gfx_pc.c (the N64 display list
// interpreter) hands its triangles to this renderer, which draws them on the
// CPU into a WATCH_FB_WIDTH x WATCH_FB_HEIGHT framebuffer. It follows the
// semantics of gfx_opengl.c: clip-space positions with GL depth (-w..w),
// LEQUAL depth test, SRC_ALPHA / ONE_MINUS_SRC_ALPHA blending, and the color
// combiner described by gfx_cc.h.

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx/gfx_cc.h"
#include "gfx/gfx_rendering_api.h"

#include "watch_port.h"
#include "port_internal.h"

#define FB_W WATCH_FB_WIDTH
#define FB_H WATCH_FB_HEIGHT

#define MAX_SHADERS 64
#define MAX_INPUTS 4
// Everything after the position: uv (2) + fog (4) + inputs (4 x 4).
#define MAX_ATTRS (2 + 4 + MAX_INPUTS * 4)
// A triangle clipped by the 5 planes below has at most 3 + 5 vertices.
#define MAX_CLIP_VERTS 8
// Triangles are clipped against x and y only outside this multiple of the
// viewport, to keep edge functions precise without clipping most triangles.
#define GUARD_BAND 4.0f

struct ShaderProgram {
    uint32_t shader_id;
    struct CCFeatures cc;
    uint8_t num_floats;
    uint8_t input_size;
    bool uses_uv;
};

struct SoftTexture {
    uint32_t *pixels; // RGBA8, red in the lowest byte
    int width, height;
    bool linear;
    uint32_t cms, cmt;
};

struct ClipVertex {
    float pos[4];
    float attr[MAX_ATTRS];
};

struct ScreenVertex {
    float x, y, z, inv_w;
    float attr[MAX_ATTRS]; // already divided by w
};

static uint32_t color_buf[FB_W * FB_H];
static float depth_buf[FB_W * FB_H];

static uint32_t front_buf[FB_W * FB_H];
static uint32_t front_seq;
static pthread_mutex_t front_lock = PTHREAD_MUTEX_INITIALIZER;

static struct ShaderProgram shader_pool[MAX_SHADERS];
static int shader_pool_size;
static struct ShaderProgram *cur_shader;

static struct SoftTexture *textures; // index 0 is never handed out
static uint32_t texture_count, texture_cap;
static int active_tile;
static uint32_t bound_texture[2];

static bool depth_test, depth_mask, zmode_decal, use_alpha;
static int vp_x, vp_y, vp_w = FB_W, vp_h = FB_H; // GL convention: y from the bottom
static int clip_x0, clip_y0, clip_x1 = FB_W, clip_y1 = FB_H; // scissor in rows from the top
static uint32_t frame_count;

static const float zero4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static const float white4[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

// ---------------------------------------------------------------------------
// Shaders and textures

static bool soft_z_is_from_0_to_1(void) {
    return false;
}

static void soft_unload_shader(struct ShaderProgram *old_prg) {
    (void)old_prg;
}

static void soft_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
}

static struct ShaderProgram *soft_create_and_load_new_shader(uint32_t shader_id) {
    if (shader_pool_size == MAX_SHADERS) {
        abort();
    }
    struct ShaderProgram *prg = &shader_pool[shader_pool_size++];
    prg->shader_id = shader_id;
    gfx_cc_get_features(shader_id, &prg->cc);
    prg->uses_uv = prg->cc.used_textures[0] || prg->cc.used_textures[1];
    prg->input_size = prg->cc.opt_alpha ? 4 : 3;
    prg->num_floats = 4 + (prg->uses_uv ? 2 : 0) + (prg->cc.opt_fog ? 4 : 0)
                      + prg->cc.num_inputs * prg->input_size;
    cur_shader = prg;
    return prg;
}

static struct ShaderProgram *soft_lookup_shader(uint32_t shader_id) {
    for (int i = 0; i < shader_pool_size; i++) {
        if (shader_pool[i].shader_id == shader_id) {
            return &shader_pool[i];
        }
    }
    return NULL;
}

static void soft_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->cc.num_inputs;
    used_textures[0] = prg->cc.used_textures[0];
    used_textures[1] = prg->cc.used_textures[1];
}

static uint32_t soft_new_texture(void) {
    if (texture_count + 1 >= texture_cap) {
        uint32_t new_cap = texture_cap ? texture_cap * 2 : 256;
        textures = realloc(textures, new_cap * sizeof(struct SoftTexture));
        memset(textures + texture_cap, 0, (new_cap - texture_cap) * sizeof(struct SoftTexture));
        texture_cap = new_cap;
    }
    return ++texture_count;
}

static void soft_select_texture(int tile, uint32_t texture_id) {
    active_tile = tile;
    bound_texture[tile] = texture_id;
}

static void soft_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    struct SoftTexture *tex = &textures[bound_texture[active_tile]];
    if (tex->width * tex->height != width * height) {
        tex->pixels = realloc(tex->pixels, (size_t)width * height * 4);
    }
    memcpy(tex->pixels, rgba32_buf, (size_t)width * height * 4);
    tex->width = width;
    tex->height = height;
}

static void soft_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    active_tile = tile;
    struct SoftTexture *tex = &textures[bound_texture[tile]];
    tex->linear = linear_filter;
    tex->cms = cms;
    tex->cmt = cmt;
}

// ---------------------------------------------------------------------------
// Render state

static void soft_set_depth_test(bool enable) {
    depth_test = enable;
}

static void soft_set_depth_mask(bool z_upd) {
    depth_mask = z_upd;
}

static void soft_set_zmode_decal(bool decal) {
    zmode_decal = decal;
}

static void soft_set_viewport(int x, int y, int width, int height) {
    vp_x = x;
    vp_y = y;
    vp_w = width;
    vp_h = height;
}

static void soft_set_scissor(int x, int y, int width, int height) {
    clip_x0 = x < 0 ? 0 : x;
    clip_x1 = x + width > FB_W ? FB_W : x + width;
    clip_y0 = FB_H - (y + height) < 0 ? 0 : FB_H - (y + height);
    clip_y1 = FB_H - y > FB_H ? FB_H : FB_H - y;
}

static void soft_set_use_alpha(bool enable) {
    use_alpha = enable;
}

// ---------------------------------------------------------------------------
// Texture sampling

static inline int wrap_coord(int i, int size, uint32_t mode) {
    if (mode & G_TX_CLAMP) {
        return i < 0 ? 0 : (i >= size ? size - 1 : i);
    }
    if (mode & G_TX_MIRROR) {
        int period = size * 2;
        i %= period;
        if (i < 0) {
            i += period;
        }
        return i < size ? i : period - 1 - i;
    }
    i %= size;
    return i < 0 ? i + size : i;
}

static inline void unpack(uint32_t p, float out[4]) {
    const float k = 1.0f / 255.0f;
    out[0] = (float)(p & 0xff) * k;
    out[1] = (float)((p >> 8) & 0xff) * k;
    out[2] = (float)((p >> 16) & 0xff) * k;
    out[3] = (float)(p >> 24) * k;
}

static void sample_texture(uint32_t id, float u, float v, float out[4]) {
    const struct SoftTexture *tex = &textures[id];
    if (id == 0 || tex->pixels == NULL) {
        memcpy(out, white4, sizeof(white4));
        return;
    }
    int w = tex->width, h = tex->height;
    if (!tex->linear) {
        int x = wrap_coord((int)floorf(u * w), w, tex->cms);
        int y = wrap_coord((int)floorf(v * h), h, tex->cmt);
        unpack(tex->pixels[y * w + x], out);
        return;
    }
    float fx = u * w - 0.5f, fy = v * h - 0.5f;
    float x0f = floorf(fx), y0f = floorf(fy);
    float tx = fx - x0f, ty = fy - y0f;
    int x0 = wrap_coord((int)x0f, w, tex->cms), x1 = wrap_coord((int)x0f + 1, w, tex->cms);
    int y0 = wrap_coord((int)y0f, h, tex->cmt), y1 = wrap_coord((int)y0f + 1, h, tex->cmt);
    float c00[4], c10[4], c01[4], c11[4];
    unpack(tex->pixels[y0 * w + x0], c00);
    unpack(tex->pixels[y0 * w + x1], c10);
    unpack(tex->pixels[y1 * w + x0], c01);
    unpack(tex->pixels[y1 * w + x1], c11);
    for (int i = 0; i < 4; i++) {
        float top = c00[i] + (c10[i] - c00[i]) * tx;
        float bottom = c01[i] + (c11[i] - c01[i]) * tx;
        out[i] = top + (bottom - top) * ty;
    }
}

// ---------------------------------------------------------------------------
// Color combiner: (a - b) * c + d, with the shortcuts gfx_cc_get_features found

static inline float combine(const float *const src[8], const uint8_t c[4], int ch,
                            bool single, bool multiply, bool mix) {
    if (single) {
        return src[c[3]][ch];
    }
    if (multiply) {
        return src[c[0]][ch] * src[c[2]][ch];
    }
    if (mix) {
        float b = src[c[1]][ch];
        return b + (src[c[0]][ch] - b) * src[c[2]][ch];
    }
    return (src[c[0]][ch] - src[c[1]][ch]) * src[c[2]][ch] + src[c[3]][ch];
}

static inline float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Cheap per-pixel hash for the N64 "noise" alpha used by vanish caps.
static inline bool noise_keep(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + frame_count * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (h ^ (h >> 16)) & 1;
}

// ---------------------------------------------------------------------------
// Rasterization

static inline float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// Top-left style fill rule: of two triangles sharing an edge (traversed in
// opposite directions), exactly one owns pixels lying on it.
static inline bool owns_edge(float ax, float ay, float bx, float by) {
    float dy = by - ay, dx = bx - ax;
    return dy > 0.0f || (dy == 0.0f && dx < 0.0f);
}

static void raster_triangle(const struct ShaderProgram *prg, int nattrs, const struct ScreenVertex *a,
                            const struct ScreenVertex *b, const struct ScreenVertex *c) {
    float area = edge(a->x, a->y, b->x, b->y, c->x, c->y);
    if (area == 0.0f) {
        return;
    }
    if (area < 0.0f) {
        const struct ScreenVertex *t = b;
        b = c;
        c = t;
        area = -area;
    }

    int min_x = (int)floorf(fminf(a->x, fminf(b->x, c->x)));
    int max_x = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int min_y = (int)floorf(fminf(a->y, fminf(b->y, c->y)));
    int max_y = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    int vp_top = FB_H - (vp_y + vp_h);
    if (min_x < clip_x0) min_x = clip_x0;
    if (min_x < vp_x) min_x = vp_x;
    if (max_x > clip_x1) max_x = clip_x1;
    if (max_x > vp_x + vp_w) max_x = vp_x + vp_w;
    if (min_y < clip_y0) min_y = clip_y0;
    if (min_y < vp_top) min_y = vp_top;
    if (max_y > clip_y1) max_y = clip_y1;
    if (max_y > vp_top + vp_h) max_y = vp_top + vp_h;
    if (min_x >= max_x || min_y >= max_y) {
        return;
    }

    // Edge functions as A*x + B*y + C, stepped incrementally.
    float a0 = b->y - c->y, b0 = c->x - b->x;
    float a1 = c->y - a->y, b1 = a->x - c->x;
    float a2 = a->y - b->y, b2 = b->x - a->x;
    bool own0 = owns_edge(b->x, b->y, c->x, c->y);
    bool own1 = owns_edge(c->x, c->y, a->x, a->y);
    bool own2 = owns_edge(a->x, a->y, b->x, b->y);
    float inv_area = 1.0f / area;

    float z_bias = 0.0f;
    if (zmode_decal) {
        float dzdx = (a->z * a0 + b->z * a1 + c->z * a2) * inv_area;
        float dzdy = (a->z * b0 + b->z * b1 + c->z * b2) * inv_area;
        z_bias = -(2.0f * fmaxf(fabsf(dzdx), fabsf(dzdy)) + 1e-5f);
    }

    const struct CCFeatures *cc = &prg->cc;
    int uv_off = 0;
    int fog_off = prg->uses_uv ? 2 : 0;
    int in_off = fog_off + (cc->opt_fog ? 4 : 0);
    bool alpha_same = cc->color_alpha_same;

    float px0 = min_x + 0.5f, py0 = min_y + 0.5f;
    float row_w0 = edge(b->x, b->y, c->x, c->y, px0, py0);
    float row_w1 = edge(c->x, c->y, a->x, a->y, px0, py0);
    float row_w2 = edge(a->x, a->y, b->x, b->y, px0, py0);

    for (int y = min_y; y < max_y; y++) {
        float w0 = row_w0, w1 = row_w1, w2 = row_w2;
        for (int x = min_x; x < max_x; x++, w0 += a0, w1 += a1, w2 += a2) {
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            if ((w0 == 0.0f && !own0) || (w1 == 0.0f && !own1) || (w2 == 0.0f && !own2)) continue;

            float l0 = w0 * inv_area, l1 = w1 * inv_area, l2 = w2 * inv_area;
            float z = l0 * a->z + l1 * b->z + l2 * c->z + z_bias;
            if (z > 1.0f) continue;
            int idx = y * FB_W + x;
            if (depth_test && z > depth_buf[idx]) continue;

            // Perspective-correct attributes.
            float attr[MAX_ATTRS];
            float w = 1.0f / (l0 * a->inv_w + l1 * b->inv_w + l2 * c->inv_w);
            for (int i = 0; i < nattrs; i++) {
                attr[i] = (l0 * a->attr[i] + l1 * b->attr[i] + l2 * c->attr[i]) * w;
            }

            float tex0[4], tex1[4], tex0a[4];
            if (cc->used_textures[0]) {
                sample_texture(bound_texture[0], attr[uv_off], attr[uv_off + 1], tex0);
            } else {
                memcpy(tex0, white4, sizeof(tex0));
            }
            if (cc->used_textures[1]) {
                sample_texture(bound_texture[1], attr[uv_off], attr[uv_off + 1], tex1);
            } else {
                memcpy(tex1, white4, sizeof(tex1));
            }
            tex0a[0] = tex0a[1] = tex0a[2] = tex0a[3] = tex0[3];

            float inputs[MAX_INPUTS][4];
            for (int i = 0; i < cc->num_inputs; i++) {
                const float *in = &attr[in_off + i * prg->input_size];
                inputs[i][0] = in[0];
                inputs[i][1] = in[1];
                inputs[i][2] = in[2];
                inputs[i][3] = prg->input_size == 4 ? in[3] : 1.0f;
            }
            const float *src[8] = {
                [SHADER_0] = zero4,
                [SHADER_INPUT_1] = inputs[0],
                [SHADER_INPUT_2] = inputs[1],
                [SHADER_INPUT_3] = inputs[2],
                [SHADER_INPUT_4] = inputs[3],
                [SHADER_TEXEL0] = tex0,
                [SHADER_TEXEL0A] = tex0a,
                [SHADER_TEXEL1] = tex1,
            };

            float r = combine(src, cc->c[0], 0, cc->do_single[0], cc->do_multiply[0], cc->do_mix[0]);
            float g = combine(src, cc->c[0], 1, cc->do_single[0], cc->do_multiply[0], cc->do_mix[0]);
            float bl = combine(src, cc->c[0], 2, cc->do_single[0], cc->do_multiply[0], cc->do_mix[0]);
            float al = 1.0f;
            if (cc->opt_alpha) {
                int k = alpha_same ? 0 : 1;
                al = combine(src, cc->c[k], 3, cc->do_single[k], cc->do_multiply[k], cc->do_mix[k]);
                if (cc->opt_texture_edge) {
                    if (al <= 0.3f) continue;
                    al = 1.0f;
                }
                if (cc->opt_noise && !noise_keep(x * 240 / vp_h, y * 240 / vp_h)) {
                    al = 0.0f;
                }
            }
            if (cc->opt_fog) {
                const float *fog = &attr[fog_off];
                r += (fog[0] - r) * fog[3];
                g += (fog[1] - g) * fog[3];
                bl += (fog[2] - bl) * fog[3];
            }

            if (use_alpha) {
                float dst[4];
                unpack(color_buf[idx], dst);
                al = clamp01(al);
                r = r * al + dst[0] * (1.0f - al);
                g = g * al + dst[1] * (1.0f - al);
                bl = bl * al + dst[2] * (1.0f - al);
            }
            color_buf[idx] = (uint32_t)(clamp01(r) * 255.0f + 0.5f)
                             | ((uint32_t)(clamp01(g) * 255.0f + 0.5f) << 8)
                             | ((uint32_t)(clamp01(bl) * 255.0f + 0.5f) << 16)
                             | 0xff000000u;
            if (depth_test && depth_mask) {
                depth_buf[idx] = z;
            }
        }
        row_w0 += b0;
        row_w1 += b1;
        row_w2 += b2;
    }
}

// Sutherland-Hodgman against plane dot(plane, pos) >= 0.
static int clip_polygon(const float plane[4], int nattrs, const struct ClipVertex *in, int n,
                        struct ClipVertex *out) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        const struct ClipVertex *p = &in[i], *q = &in[(i + 1) % n];
        float dp = plane[0] * p->pos[0] + plane[1] * p->pos[1] + plane[2] * p->pos[2] + plane[3] * p->pos[3];
        float dq = plane[0] * q->pos[0] + plane[1] * q->pos[1] + plane[2] * q->pos[2] + plane[3] * q->pos[3];
        if (dp >= 0.0f) {
            out[count++] = *p;
        }
        if ((dp >= 0.0f) != (dq >= 0.0f)) {
            float t = dp / (dp - dq);
            struct ClipVertex *v = &out[count++];
            for (int k = 0; k < 4; k++) {
                v->pos[k] = p->pos[k] + (q->pos[k] - p->pos[k]) * t;
            }
            for (int k = 0; k < nattrs; k++) {
                v->attr[k] = p->attr[k] + (q->attr[k] - p->attr[k]) * t;
            }
        }
    }
    return count;
}

static const float clip_planes[5][4] = {
    { 0.0f, 0.0f, 1.0f, 1.0f },        // near: z >= -w
    { 1.0f, 0.0f, 0.0f, GUARD_BAND },  // x >= -g*w
    { -1.0f, 0.0f, 0.0f, GUARD_BAND }, // x <= g*w
    { 0.0f, 1.0f, 0.0f, GUARD_BAND },  // y >= -g*w
    { 0.0f, -1.0f, 0.0f, GUARD_BAND }, // y <= g*w
};

static void to_screen(const struct ClipVertex *cv, int nattrs, struct ScreenVertex *sv) {
    float inv_w = 1.0f / cv->pos[3];
    sv->x = vp_x + (cv->pos[0] * inv_w + 1.0f) * 0.5f * vp_w;
    sv->y = FB_H - (vp_y + (cv->pos[1] * inv_w + 1.0f) * 0.5f * vp_h);
    sv->z = cv->pos[2] * inv_w * 0.5f + 0.5f;
    sv->inv_w = inv_w;
    for (int i = 0; i < nattrs; i++) {
        sv->attr[i] = cv->attr[i] * inv_w;
    }
}

static void soft_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    (void)buf_vbo_len;
    const struct ShaderProgram *prg = cur_shader;
    int stride = prg->num_floats;
    int nattrs = stride - 4;

    for (size_t t = 0; t < buf_vbo_num_tris; t++) {
        struct ClipVertex poly[2][MAX_CLIP_VERTS];
        int n = 3;
        int outside_mask = 0;
        for (int i = 0; i < 3; i++) {
            const float *v = &buf_vbo[(t * 3 + i) * stride];
            memcpy(poly[0][i].pos, v, 4 * sizeof(float));
            memcpy(poly[0][i].attr, v + 4, nattrs * sizeof(float));
            for (int p = 0; p < 5; p++) {
                const float *pl = clip_planes[p];
                if (pl[0] * v[0] + pl[1] * v[1] + pl[2] * v[2] + pl[3] * v[3] < 0.0f) {
                    outside_mask |= 1 << p;
                }
            }
        }
        int cur = 0;
        for (int p = 0; p < 5 && n > 0; p++) {
            if (outside_mask & (1 << p)) {
                n = clip_polygon(clip_planes[p], nattrs, poly[cur], n, poly[cur ^ 1]);
                cur ^= 1;
            }
        }
        if (n < 3) {
            continue;
        }
        struct ScreenVertex sv[MAX_CLIP_VERTS];
        for (int i = 0; i < n; i++) {
            to_screen(&poly[cur][i], nattrs, &sv[i]);
        }
        for (int i = 1; i + 1 < n; i++) {
            raster_triangle(prg, nattrs, &sv[0], &sv[i], &sv[i + 1]);
        }
    }
}

// ---------------------------------------------------------------------------
// Frame lifecycle

static void soft_init(void) {
}

static void soft_on_resize(void) {
}

static void soft_start_frame(void) {
    frame_count++;
    for (int i = 0; i < FB_W * FB_H; i++) {
        color_buf[i] = 0xff000000u;
        depth_buf[i] = 1.0f;
    }
}

static void soft_end_frame(void) {
}

static void soft_finish_render(void) {
    pthread_mutex_lock(&front_lock);
    memcpy(front_buf, color_buf, sizeof(front_buf));
    front_seq++;
    pthread_mutex_unlock(&front_lock);
}

uint32_t watch_port_copy_frame(uint8_t *dst) {
    pthread_mutex_lock(&front_lock);
    memcpy(dst, front_buf, sizeof(front_buf));
    uint32_t seq = front_seq;
    pthread_mutex_unlock(&front_lock);
    return seq;
}

struct GfxRenderingAPI gfx_soft_api = {
    soft_z_is_from_0_to_1,
    soft_unload_shader,
    soft_load_shader,
    soft_create_and_load_new_shader,
    soft_lookup_shader,
    soft_shader_get_info,
    soft_new_texture,
    soft_select_texture,
    soft_upload_texture,
    soft_set_sampler_parameters,
    soft_set_depth_test,
    soft_set_depth_mask,
    soft_set_zmode_decal,
    soft_set_viewport,
    soft_set_scissor,
    soft_set_use_alpha,
    soft_draw_triangles,
    soft_init,
    soft_on_resize,
    soft_start_frame,
    soft_end_frame,
    soft_finish_render,
};
