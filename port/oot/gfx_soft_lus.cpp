// Software rasterizer implementing libultraship's GfxRenderingAPI.
//
// The Ship of Harkinian counterpart of port/src/gfx_soft.c. watchOS has no
// GPU API for apps, so libultraship's display list interpreter hands its
// triangles here and they are drawn on the CPU. It follows the semantics of
// libultraship's OpenGL backend (gfx_opengl.cpp and its default.shader.glsl):
// clip-space positions with GL depth and depth clamping, LESS / LEQUAL depth
// tests with a polygon offset for decals, SRC_ALPHA / ONE_MINUS_SRC_ALPHA
// blending, and the two-cycle color combiner described by CCFeatures.
//
// Framebuffers are stored the way GL stores them: row 0 is the bottom row
// in window coordinates. libultraship renders its game framebuffer with
// invertY, which puts the top of the picture in row 0.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fast/backends/gfx_rendering_api.h"
#include "fast/interpreter.h"

#include "watch_lus.h"
#include "watch_port.h"

namespace Fast {

namespace {

constexpr int MAX_INPUTS = 7;
// Everything after the position: 2 x (uv + clamp s/t), fog, grayscale, inputs.
constexpr int MAX_ATTRS = 2 * 4 + 4 + 4 + MAX_INPUTS * 4;
// A triangle clipped by the 5 planes below has at most 3 + 5 vertices.
constexpr int MAX_CLIP_VERTS = 8;
// Triangles are clipped against x and y only outside this multiple of the
// viewport, to keep edge functions precise without clipping most triangles.
constexpr float GUARD_BAND = 4.0f;
// The interpreter allocates max^2 * 4 bytes for texture uploads.
constexpr int MAX_TEXTURE_SIZE = 1024;
// Texture ids handed out for framebuffers bound as textures.
constexpr uint32_t FB_TEXTURE_BIT = 0x80000000u;

// Combiner operand slots: each points at four floats for the current pixel.
enum Slot : uint8_t {
    S_ZERO,
    S_ONE,
    S_INPUT1, // .. S_INPUT1 + 6
    S_TEX0 = S_INPUT1 + MAX_INPUTS,
    S_TEX0A,
    S_TEX1,
    S_TEX1A,
    S_COMBINED,
    S_NOISE,
    S_COUNT
};

enum FormulaMode : uint8_t { F_FULL, F_SINGLE, F_MULTIPLY, F_MIX };

struct Formula {
    FormulaMode mode;
    uint8_t a, b, c, d;
};

struct Cycle {
    Formula rgb, alpha;
    bool separate_alpha; // alpha has its own formula
    uint8_t wrap_rgb, wrap_alpha; // 0: none, 1: +-1.01, 2: -0.51..1.51
};

// A compiled combiner. The API only passes ShaderProgram pointers around,
// and the GL and Metal backends define that type themselves, so ours is
// private and cast.
struct SoftShader {
    CCFeatures cc;
    int num_floats;
    int uv_off[2], clamp_s_off[2], clamp_t_off[2];
    int fog_off, gray_off, input_off, input_size;
    int num_cycles;
    Cycle cycles[2];
    bool uses_noise;
};

struct SoftTexture {
    std::vector<uint32_t> pixels; // RGBA8, red in the lowest byte
    int width = 0, height = 0;
    bool linear = false;
    uint32_t cms = 0, cmt = 0;
};

struct SoftFramebuffer {
    int width = 0, height = 0;
    bool invert_y = false;
    bool has_depth = false;
    std::vector<uint32_t> color;
    std::vector<float> depth;
    // Sampler state for when the framebuffer is bound as a texture.
    bool linear = false;
    uint32_t cms = 0, cmt = 0;
};

// What a texture unit samples from, resolved once per draw call.
struct TexView {
    const uint32_t* pixels = nullptr;
    int width = 0, height = 0;
    int wmask = -1, hmask = -1; // size - 1 for power-of-two sizes, else -1
    bool linear = false;
    uint32_t cms = 0, cmt = 0;
};

struct ClipVertex {
    float pos[4];
    float attr[MAX_ATTRS];
};

struct ScreenVertex {
    float x, y, z, inv_w;
    float attr[MAX_ATTRS]; // already divided by w
};

const float kZero4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
const float kOne4[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

inline float Clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// GLSL's WRAP(x, low, high): mod(x - low, high - low) + low.
inline float Wrap(float x, float low, float high) {
    if (x >= low && x < high) {
        return x;
    }
    float range = high - low, t = x - low;
    return t - range * floorf(t / range) + low;
}

inline void Unpack(uint32_t p, float out[4]) {
    const float k = 1.0f / 255.0f;
    out[0] = (float)(p & 0xff) * k;
    out[1] = (float)((p >> 8) & 0xff) * k;
    out[2] = (float)((p >> 16) & 0xff) * k;
    out[3] = (float)(p >> 24) * k;
}

inline uint32_t Pack(float r, float g, float b) {
    return (uint32_t)(Clamp01(r) * 255.0f + 0.5f) | ((uint32_t)(Clamp01(g) * 255.0f + 0.5f) << 8) |
           ((uint32_t)(Clamp01(b) * 255.0f + 0.5f) << 16) | 0xff000000u;
}

// GL wrap modes, from the N64 tile's cms/cmt bits. mask is size - 1 when
// size is a power of two (nearly always on the N64), else -1.
inline int WrapCoord(int i, int size, int mask, uint32_t mode) {
    if (mode & G_TX_CLAMP) {
        if (mode & G_TX_MIRROR) { // GL_MIRROR_CLAMP_TO_EDGE
            i = i < 0 ? -1 - i : i;
        }
        return i < 0 ? 0 : (i >= size ? size - 1 : i);
    }
    if (mode & G_TX_MIRROR) { // GL_MIRRORED_REPEAT
        int period = size * 2;
        if (mask >= 0) {
            i &= period - 1;
        } else {
            i %= period;
            if (i < 0) {
                i += period;
            }
        }
        return i < size ? i : period - 1 - i;
    }
    if (mask >= 0) { // GL_REPEAT
        return i & mask;
    }
    i %= size;
    return i < 0 ? i + size : i;
}

// Weighted sums of packed RGBA8 colors, two channels per 32-bit lane pair.
// Weights are in 1/256 and add up to at most 256.
inline uint32_t Mix3(uint32_t c0, uint32_t c1, uint32_t c2, uint32_t w1, uint32_t w2) {
    uint32_t w0 = 256 - w1 - w2;
    uint32_t rb = ((c0 & 0x00ff00ffu) * w0 + (c1 & 0x00ff00ffu) * w1 + (c2 & 0x00ff00ffu) * w2) >> 8;
    uint32_t ga = (((c0 >> 8) & 0x00ff00ffu) * w0 + ((c1 >> 8) & 0x00ff00ffu) * w1 + ((c2 >> 8) & 0x00ff00ffu) * w2) >> 8;
    return (rb & 0x00ff00ffu) | ((ga & 0x00ff00ffu) << 8);
}

inline uint32_t Mix2(uint32_t c0, uint32_t c1, uint32_t w1) {
    return Mix3(c0, c1, 0, w1, 0);
}

inline bool IsPowerOfTwo(int v) {
    return v > 0 && (v & (v - 1)) == 0;
}

// A random value in [0, 1) per pixel and frame, standing in for the shader's
// sin-based random().
inline float Random01(int x, int y, uint32_t frame) {
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ frame * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return (float)(h >> 8) * (1.0f / 16777216.0f);
}

inline float Edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// Narrows [xs, xe] (pixel offsets in a row, 0..last) to where the edge
// function e + a * k may be >= 0, keeping one pixel of slack on each side for
// the exact per-pixel test. False when no pixel of the row can be inside.
inline bool SpanLimit(float e, float a, int last, int& xs, int& xe) {
    if (a > 0.0f) {
        float k = std::min(std::max(-e / a, -1.0f), (float)last + 1.0f);
        xs = std::max(xs, (int)ceilf(k) - 1);
    } else if (a < 0.0f) {
        float k = std::min(std::max(e / -a, -1.0f), (float)last + 1.0f);
        xe = std::min(xe, (int)floorf(k) + 1);
    } else if (e < 0.0f) {
        return false;
    }
    return xs <= xe;
}

// Of two triangles sharing an edge (traversed in opposite directions),
// exactly one owns pixels lying on it.
inline bool OwnsEdge(float ax, float ay, float bx, float by) {
    float dy = by - ay, dx = bx - ax;
    return dy > 0.0f || (dy == 0.0f && dx < 0.0f);
}

// Sutherland-Hodgman against plane dot(plane.xyzw, pos) + plane[4] >= 0.
int ClipPolygon(const float plane[5], int nattrs, const ClipVertex* in, int n, ClipVertex* out) {
    int count = 0;
    for (int i = 0; i < n; i++) {
        const ClipVertex* p = &in[i];
        const ClipVertex* q = &in[(i + 1) % n];
        float dp = plane[0] * p->pos[0] + plane[1] * p->pos[1] + plane[2] * p->pos[2] + plane[3] * p->pos[3] + plane[4];
        float dq = plane[0] * q->pos[0] + plane[1] * q->pos[1] + plane[2] * q->pos[2] + plane[3] * q->pos[3] + plane[4];
        if (dp >= 0.0f) {
            out[count++] = *p;
        }
        if ((dp >= 0.0f) != (dq >= 0.0f)) {
            float t = dp / (dp - dq);
            ClipVertex* v = &out[count++];
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

// GL clamps depth instead of clipping at the near and far planes, so only
// w > 0 and the guard band are clipped.
const float kClipPlanes[5][5] = {
    { 0.0f, 0.0f, 0.0f, 1.0f, -1e-5f },      // w > 0
    { 1.0f, 0.0f, 0.0f, GUARD_BAND, 0.0f },  // x >= -g*w
    { -1.0f, 0.0f, 0.0f, GUARD_BAND, 0.0f }, // x <= g*w
    { 0.0f, 1.0f, 0.0f, GUARD_BAND, 0.0f },  // y >= -g*w
    { 0.0f, -1.0f, 0.0f, GUARD_BAND, 0.0f }, // y <= g*w
};

uint8_t SlotFor(int item, bool first_cycle) {
    switch (item) {
        case SHADER_0:
            return S_ZERO;
        case SHADER_1:
            return S_ONE;
        case SHADER_INPUT_1:
        case SHADER_INPUT_2:
        case SHADER_INPUT_3:
        case SHADER_INPUT_4:
        case SHADER_INPUT_5:
        case SHADER_INPUT_6:
        case SHADER_INPUT_7:
            return S_INPUT1 + (item - SHADER_INPUT_1);
        // In the second cycle, TEXEL0 and TEXEL1 trade places (as in the shader).
        case SHADER_TEXEL0:
            return first_cycle ? S_TEX0 : S_TEX1;
        case SHADER_TEXEL0A:
            return first_cycle ? S_TEX0A : S_TEX1A;
        case SHADER_TEXEL1:
            return first_cycle ? S_TEX1 : S_TEX0;
        case SHADER_TEXEL1A:
            return first_cycle ? S_TEX1A : S_TEX0A;
        case SHADER_COMBINED:
            return S_COMBINED;
        case SHADER_NOISE:
            return S_NOISE;
    }
    return S_ZERO;
}

Formula MakeFormula(const int c[4], bool single, bool multiply, bool mix, bool first_cycle) {
    Formula f;
    f.mode = single ? F_SINGLE : multiply ? F_MULTIPLY : mix ? F_MIX : F_FULL;
    f.a = SlotFor(c[0], first_cycle);
    f.b = SlotFor(c[1], first_cycle);
    f.c = SlotFor(c[2], first_cycle);
    f.d = SlotFor(c[3], first_cycle);
    return f;
}

// Evaluates a formula for channels [first, end) into out.
inline void Eval(const Formula& f, const float* const src[S_COUNT], int first, int end, float out[4]) {
    const float *a = src[f.a], *b = src[f.b], *c = src[f.c], *d = src[f.d];
    switch (f.mode) {
        case F_SINGLE:
            for (int ch = first; ch < end; ch++) {
                out[ch] = d[ch];
            }
            break;
        case F_MULTIPLY:
            for (int ch = first; ch < end; ch++) {
                out[ch] = a[ch] * c[ch];
            }
            break;
        case F_MIX:
            for (int ch = first; ch < end; ch++) {
                out[ch] = b[ch] + (a[ch] - b[ch]) * c[ch];
            }
            break;
        default:
            for (int ch = first; ch < end; ch++) {
                out[ch] = (a[ch] - b[ch]) * c[ch] + d[ch];
            }
            break;
    }
}

bool FormulaUses(const Formula& f, uint8_t slot) {
    switch (f.mode) {
        case F_SINGLE:
            return f.d == slot;
        case F_MULTIPLY:
            return f.a == slot || f.c == slot;
        case F_MIX:
            return f.a == slot || f.b == slot || f.c == slot;
        default:
            return f.a == slot || f.b == slot || f.c == slot || f.d == slot;
    }
}

class GfxRenderingAPISoft final : public GfxRenderingAPI {
  public:
    const char* GetName() override {
        return "Software";
    }

    int GetMaxTextureSize() override {
        return MAX_TEXTURE_SIZE;
    }

    GfxClipParameters GetClipParameters() override {
        return { false, mFramebuffers[mCurrentFb].invert_y };
    }

    // ---------------------------------------------------------------------
    // Shaders

    void UnloadShader(ShaderProgram* oldPrg) override {
    }

    void LoadShader(ShaderProgram* newPrg) override {
        mShader = reinterpret_cast<SoftShader*>(newPrg);
    }

    void ClearShaderCache() override {
        mShaders.clear();
        mShader = nullptr;
    }

    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override {
        SoftShader* prg = &mShaders[std::make_pair(shaderId0, shaderId1)];
        CCFeatures& cc = prg->cc;
        gfx_cc_get_features(shaderId0, shaderId1, &cc);

        // The vertex layout the interpreter writes, as in gfx_opengl.cpp.
        int a = 0;
        for (int t = 0; t < 2; t++) {
            prg->uv_off[t] = prg->clamp_s_off[t] = prg->clamp_t_off[t] = -1;
            if (cc.usedTextures[t]) {
                prg->uv_off[t] = a;
                a += 2;
                if (cc.clamp[t][0]) {
                    prg->clamp_s_off[t] = a++;
                }
                if (cc.clamp[t][1]) {
                    prg->clamp_t_off[t] = a++;
                }
            }
        }
        prg->fog_off = cc.opt_fog ? a : -1;
        a += cc.opt_fog ? 4 : 0;
        prg->gray_off = cc.opt_grayscale ? a : -1;
        a += cc.opt_grayscale ? 4 : 0;
        prg->input_off = a;
        prg->input_size = cc.opt_alpha ? 4 : 3;
        a += std::min(cc.numInputs, MAX_INPUTS) * prg->input_size;
        prg->num_floats = 4 + a;

        prg->num_cycles = cc.opt_2cyc ? 2 : 1;
        prg->uses_noise = false;
        for (int c = 0; c < prg->num_cycles; c++) {
            Cycle& cy = prg->cycles[c];
            bool first = c == 0;
            cy.rgb = MakeFormula(cc.c[c][0], cc.do_single[c][0], cc.do_multiply[c][0], cc.do_mix[c][0], first);
            cy.alpha = MakeFormula(cc.c[c][1], cc.do_single[c][1], cc.do_multiply[c][1], cc.do_mix[c][1], first);
            cy.separate_alpha = !cc.color_alpha_same[c] && cc.opt_alpha;
            cy.wrap_rgb = c == 1 ? (cc.c[1][0][2] == SHADER_COMBINED ? 1 : 2) : 0;
            cy.wrap_alpha = c == 1 && cc.opt_alpha ? (cc.c[1][1][2] == SHADER_COMBINED ? 1 : 2) : 0;
            prg->uses_noise |= FormulaUses(cy.rgb, S_NOISE) || (cy.separate_alpha && FormulaUses(cy.alpha, S_NOISE));
        }

        mShader = prg;
        return reinterpret_cast<ShaderProgram*>(prg);
    }

    ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override {
        auto it = mShaders.find(std::make_pair(shaderId0, shaderId1));
        return it == mShaders.end() ? nullptr : reinterpret_cast<ShaderProgram*>(&it->second);
    }

    void ShaderGetInfo(ShaderProgram* program, uint8_t* numInputs, bool usedTextures[2]) override {
        const SoftShader* prg = reinterpret_cast<const SoftShader*>(program);
        *numInputs = prg->cc.numInputs;
        usedTextures[0] = prg->cc.usedTextures[0];
        usedTextures[1] = prg->cc.usedTextures[1];
    }

    // ---------------------------------------------------------------------
    // Textures

    uint32_t NewTexture() override {
        if (!mFreeTextures.empty()) {
            uint32_t id = mFreeTextures.back();
            mFreeTextures.pop_back();
            return id;
        }
        if (mTextures.empty()) {
            mTextures.resize(1); // id 0 is never handed out
        }
        mTextures.emplace_back();
        return (uint32_t)mTextures.size() - 1;
    }

    void DeleteTexture(uint32_t texId) override {
        if (texId == 0 || texId >= mTextures.size()) {
            return;
        }
        mTextures[texId] = SoftTexture();
        mFreeTextures.push_back(texId);
    }

    void SelectTexture(int tile, uint32_t textureId) override {
        mCurrentTile = tile;
        mBound[tile] = textureId;
    }

    void SelectTextureFb(int fbId) override {
        // Like the GL backend, framebuffers are always bound to the first unit.
        mCurrentTile = 0;
        mBound[0] = FB_TEXTURE_BIT | (uint32_t)fbId;
    }

    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override {
        uint32_t id = mBound[mCurrentTile];
        if (width == 0 || height == 0 || (id & FB_TEXTURE_BIT) || id >= mTextures.size()) {
            return;
        }
        SoftTexture& tex = mTextures[id];
        tex.pixels.resize((size_t)width * height);
        memcpy(tex.pixels.data(), rgba32Buf, (size_t)width * height * 4);
        tex.width = (int)width;
        tex.height = (int)height;
    }

    void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) override {
        mCurrentTile = sampler;
        uint32_t id = mBound[sampler];
        if (id & FB_TEXTURE_BIT) {
            SoftFramebuffer& fb = mFramebuffers[id & ~FB_TEXTURE_BIT];
            fb.linear = linear_filter;
            fb.cms = cms;
            fb.cmt = cmt;
        } else if (id < mTextures.size()) {
            SoftTexture& tex = mTextures[id];
            tex.linear = linear_filter;
            tex.cms = cms;
            tex.cmt = cmt;
        }
    }

    void SetTextureFilter(FilteringMode mode) override {
        mFilterMode = mode;
    }

    FilteringMode GetTextureFilter() override {
        return mFilterMode;
    }

    void SetSrgbMode() override {
    }

    ImTextureID GetTextureById(int id) override {
        return (ImTextureID)(uintptr_t)id;
    }

    // ---------------------------------------------------------------------
    // Render state

    void SetDepthTestAndMask(bool depth_test, bool z_upd) override {
        mCurrentDepthTest = depth_test;
        mCurrentDepthMask = z_upd;
    }

    void SetZmodeDecal(bool decal) override {
        mCurrentZmodeDecal = decal;
    }

    void SetCurrentPrimDepth(float depth) override {
        mCurrentPrimDepth = depth;
    }

    void SetViewport(int x, int y, int width, int height) override {
        mVpX = x;
        mVpY = y;
        mVpW = width;
        mVpH = height;
    }

    void SetScissor(int x, int y, int width, int height) override {
        mScX = x;
        mScY = y;
        mScW = width;
        mScH = height;
    }

    void SetUseAlpha(bool useAlpha) override {
        mUseAlpha = useAlpha;
    }

    // ---------------------------------------------------------------------
    // Drawing

    void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) override {
        const SoftShader* prg = mShader;
        if (prg == nullptr) {
            return;
        }
        SoftFramebuffer& fb = mFramebuffers[mCurrentFb];
        if (fb.width == 0 || fb.height == 0) {
            return;
        }
        for (int t = 0; t < SHADER_MAX_TEXTURES; t++) {
            mViews[t] = ResolveTexture(mBound[t]);
        }

        // Pixels the triangles may touch: viewport, scissor and framebuffer.
        mRectX0 = std::max({ 0, mVpX, mScX });
        mRectY0 = std::max({ 0, mVpY, mScY });
        mRectX1 = std::min({ fb.width, mVpX + mVpW, mScX + mScW });
        mRectY1 = std::min({ fb.height, mVpY + mVpH, mScY + mScH });
        if (mRectX0 >= mRectX1 || mRectY0 >= mRectY1) {
            return;
        }

        auto started = std::chrono::steady_clock::now();
        int stride = prg->num_floats;
        int nattrs = stride - 4;
        for (size_t t = 0; t < buf_vbo_num_tris; t++) {
            ClipVertex poly[2][MAX_CLIP_VERTS];
            int n = 3;
            int outside_mask = 0;
            for (int i = 0; i < 3; i++) {
                const float* v = &buf_vbo[(t * 3 + i) * stride];
                memcpy(poly[0][i].pos, v, 4 * sizeof(float));
                memcpy(poly[0][i].attr, v + 4, nattrs * sizeof(float));
                for (int p = 0; p < 5; p++) {
                    const float* pl = kClipPlanes[p];
                    if (pl[0] * v[0] + pl[1] * v[1] + pl[2] * v[2] + pl[3] * v[3] + pl[4] < 0.0f) {
                        outside_mask |= 1 << p;
                    }
                }
            }
            int cur = 0;
            for (int p = 0; p < 5 && n > 0; p++) {
                if (outside_mask & (1 << p)) {
                    n = ClipPolygon(kClipPlanes[p], nattrs, poly[cur], n, poly[cur ^ 1]);
                    cur ^= 1;
                }
            }
            if (n < 3) {
                continue;
            }
            ScreenVertex sv[MAX_CLIP_VERTS];
            for (int i = 0; i < n; i++) {
                ToScreen(poly[cur][i], nattrs, sv[i]);
            }
            for (int i = 1; i + 1 < n; i++) {
                RasterTriangle(*prg, fb, nattrs, &sv[0], &sv[i], &sv[i + 1]);
            }
        }
        sDrawSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }

    // ---------------------------------------------------------------------
    // Frame lifecycle

    void Init() override {
        mFramebuffers.resize(1); // the window
    }

    void OnResize() override {
    }

    void StartFrame() override {
        mFrameCount++;
        mPresentFb = -1;
    }

    void EndFrame() override {
    }

    void FinishRender() override {
        int id = mPresentFb >= 0 ? mPresentFb : 0;
        const SoftFramebuffer& fb = mFramebuffers[id];
        if (fb.width == 0 || fb.height == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(sFrontLock);
        for (int y = 0; y < WATCH_FB_HEIGHT; y++) {
            // Picture rows from the top; GL rows count from the bottom unless
            // the framebuffer was drawn upside down (invertY).
            int sy = y * fb.height / WATCH_FB_HEIGHT;
            const uint32_t* src = &fb.color[(size_t)(fb.invert_y ? sy : fb.height - 1 - sy) * fb.width];
            uint32_t* dst = &sFront[(size_t)y * WATCH_FB_WIDTH];
            if (fb.width == WATCH_FB_WIDTH) {
                memcpy(dst, src, WATCH_FB_WIDTH * sizeof(uint32_t));
            } else {
                for (int x = 0; x < WATCH_FB_WIDTH; x++) {
                    dst[x] = src[x * fb.width / WATCH_FB_WIDTH];
                }
            }
        }
        sFrontSeq++;
    }

    // ---------------------------------------------------------------------
    // Framebuffers

    int CreateFramebuffer() override {
        mFramebuffers.emplace_back();
        return (int)mFramebuffers.size() - 1;
    }

    void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                     bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                     bool can_extract_depth) override {
        SoftFramebuffer& fb = mFramebuffers[fb_id];
        width = std::max(width, 1u);
        height = std::max(height, 1u);
        if (fb.width != (int)width || fb.height != (int)height) {
            fb.width = (int)width;
            fb.height = (int)height;
            fb.color.assign((size_t)width * height, 0xff000000u);
            fb.depth.assign((size_t)width * height, 1.0f);
        }
        // The window always has a depth buffer.
        fb.has_depth = fb_id == 0 || has_depth_buffer;
        fb.invert_y = opengl_invertY;
    }

    void StartDrawToFramebuffer(int fbId, float noiseScale) override {
        if (noiseScale != 0.0f) {
            mNoiseScale = 1.0f / noiseScale;
        }
        mCurrentFb = fbId;
    }

    void ClearFramebuffer(bool color, bool depth) override {
        SoftFramebuffer& fb = mFramebuffers[mCurrentFb];
        if (color) {
            std::fill(fb.color.begin(), fb.color.end(), 0xff000000u);
        }
        if (depth) {
            std::fill(fb.depth.begin(), fb.depth.end(), 1.0f);
        }
    }

    void ClearDepthRegion(int x, int y, int w, int h) override {
        SoftFramebuffer& fb = mFramebuffers[mCurrentFb];
        int x0 = std::max(x, 0), y0 = std::max(y, 0);
        int x1 = std::min(x + w, fb.width), y1 = std::min(y + h, fb.height);
        for (int row = y0; row < y1; row++) {
            std::fill(&fb.depth[(size_t)row * fb.width + x0], &fb.depth[(size_t)row * fb.width + x1], 1.0f);
        }
    }

    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override {
        if (fbDstId >= (int)mFramebuffers.size() || fbSrcId >= (int)mFramebuffers.size()) {
            return;
        }
        const SoftFramebuffer& src = mFramebuffers[fbSrcId];
        SoftFramebuffer& dst = mFramebuffers[fbDstId];

        // The same adjustments as the GL backend, then a nearest-filtered blit.
        if (!src.invert_y) {
            int extent = srcY1 - srcY0;
            srcY1 = src.height - srcY0;
            srcY0 = srcY1 - extent;
        }
        if (src.invert_y != dst.invert_y) {
            std::swap(srcY0, srcY1);
        }
        Blit(src, dst, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1);
    }

    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override {
        if (fbId >= (int)mFramebuffers.size()) {
            return;
        }
        const SoftFramebuffer& fb = mFramebuffers[fbId];
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t p = (int)x < fb.width && (int)y < fb.height ? fb.color[(size_t)y * fb.width + x] : 0;
                uint32_t r = (p & 0xff) >> 3, g = ((p >> 8) & 0xff) >> 3, b = ((p >> 16) & 0xff) >> 3;
                rgba16Buf[(size_t)y * width + x] = (uint16_t)((r << 11) | (g << 6) | (b << 1) | 1);
            }
        }
    }

    void ResolveMSAAColorBuffer(int fbIdTarget, int fbIdSrc) override {
        const SoftFramebuffer& src = mFramebuffers[fbIdSrc];
        SoftFramebuffer& dst = mFramebuffers[fbIdTarget];
        Blit(src, dst, 0, 0, src.width, src.height, 0, 0, dst.width, dst.height);
    }

    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) override {
        std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;
        const SoftFramebuffer& fb = mFramebuffers[fb_id];
        for (const auto& coord : coordinates) {
            int x = (int)coord.first, y = (int)coord.second;
            if (fb.invert_y) {
                y = fb.height - y;
            }
            x = std::clamp(x, 0, fb.width - 1);
            y = std::clamp(y, 0, fb.height - 1);
            // As read back from a 24-bit GL depth buffer.
            uint32_t d24 = (uint32_t)(Clamp01(fb.depth[(size_t)y * fb.width + x]) * 16777215.0f);
            res.emplace(coord, (uint16_t)((d24 >> 10) << 2));
        }
        return res;
    }

    void* GetFramebufferTextureId(int fbId) override {
        // Asked for the game framebuffer once it is finished: that is the
        // picture to show.
        mPresentFb = fbId;
        return (void*)(uintptr_t)(FB_TEXTURE_BIT | (uint32_t)fbId);
    }

    // ---------------------------------------------------------------------

    static double sDrawSeconds; // time spent drawing triangles, for measurements
    static std::mutex sFrontLock;
    static uint32_t sFront[WATCH_FB_WIDTH * WATCH_FB_HEIGHT];
    static uint32_t sFrontSeq;

  private:
    TexView ResolveTexture(uint32_t id) const {
        TexView v;
        if (id & FB_TEXTURE_BIT) {
            const SoftFramebuffer& fb = mFramebuffers[id & ~FB_TEXTURE_BIT];
            v.pixels = fb.color.data();
            v.width = fb.width;
            v.height = fb.height;
            v.linear = fb.linear;
            v.cms = fb.cms;
            v.cmt = fb.cmt;
        } else if (id != 0 && id < mTextures.size() && !mTextures[id].pixels.empty()) {
            const SoftTexture& tex = mTextures[id];
            v.pixels = tex.pixels.data();
            v.width = tex.width;
            v.height = tex.height;
            v.linear = tex.linear;
            v.cms = tex.cms;
            v.cmt = tex.cmt;
        }
        v.wmask = IsPowerOfTwo(v.width) ? v.width - 1 : -1;
        v.hmask = IsPowerOfTwo(v.height) ? v.height - 1 : -1;
        return v;
    }

    void Sample(const TexView& t, float u, float v, float out[4]) const {
        if (t.pixels == nullptr) {
            memcpy(out, kOne4, sizeof(kOne4));
            return;
        }
        int w = t.width, h = t.height;
        if (!t.linear || mFilterMode == FILTER_NONE) {
            int x = WrapCoord((int)floorf(u * w), w, t.wmask, t.cms);
            int y = WrapCoord((int)floorf(v * h), h, t.hmask, t.cmt);
            Unpack(t.pixels[y * w + x], out);
            return;
        }
        float fx = u * w - 0.5f, fy = v * h - 0.5f;
        float x0f = floorf(fx), y0f = floorf(fy);
        uint32_t wx = (uint32_t)((fx - x0f) * 256.0f), wy = (uint32_t)((fy - y0f) * 256.0f);
        // The two columns and rows the filter reads, wrapped once each.
        int x0 = WrapCoord((int)x0f, w, t.wmask, t.cms), x1 = WrapCoord((int)x0f + 1, w, t.wmask, t.cms);
        const uint32_t* row0 = &t.pixels[WrapCoord((int)y0f, h, t.hmask, t.cmt) * w];
        const uint32_t* row1 = &t.pixels[WrapCoord((int)y0f + 1, h, t.hmask, t.cmt) * w];
        uint32_t c;
        if (mFilterMode == FILTER_THREE_POINT) {
            // The N64's three-point filter, as filter3point() in the shader.
            if (wx + wy < 256) {
                c = Mix3(row0[x0], row0[x1], row1[x0], wx, wy);
            } else {
                c = Mix3(row1[x1], row1[x0], row0[x1], 256 - wx, 256 - wy);
            }
        } else {
            c = Mix2(Mix2(row0[x0], row0[x1], wx), Mix2(row1[x0], row1[x1], wx), wy);
        }
        Unpack(c, out);
    }

    void ToScreen(const ClipVertex& cv, int nattrs, ScreenVertex& sv) const {
        float inv_w = 1.0f / cv.pos[3];
        sv.x = mVpX + (cv.pos[0] * inv_w + 1.0f) * 0.5f * mVpW;
        sv.y = mVpY + (cv.pos[1] * inv_w + 1.0f) * 0.5f * mVpH;
        sv.z = cv.pos[2] * inv_w * 0.5f + 0.5f;
        sv.inv_w = inv_w;
        for (int i = 0; i < nattrs; i++) {
            sv.attr[i] = cv.attr[i] * inv_w;
        }
    }

    void RasterTriangle(const SoftShader& prg, SoftFramebuffer& fb, int nattrs, const ScreenVertex* a,
                        const ScreenVertex* b, const ScreenVertex* c) {
        float area = Edge(a->x, a->y, b->x, b->y, c->x, c->y);
        if (area == 0.0f || std::isnan(area)) {
            return;
        }
        if (area < 0.0f) {
            std::swap(b, c);
            area = -area;
        }

        int min_x = std::max((int)floorf(std::min({ a->x, b->x, c->x })), mRectX0);
        int max_x = std::min((int)ceilf(std::max({ a->x, b->x, c->x })), mRectX1);
        int min_y = std::max((int)floorf(std::min({ a->y, b->y, c->y })), mRectY0);
        int max_y = std::min((int)ceilf(std::max({ a->y, b->y, c->y })), mRectY1);
        if (min_x >= max_x || min_y >= max_y) {
            return;
        }

        // Edge functions as A*x + B*y + C, stepped incrementally.
        float a0 = b->y - c->y, b0 = c->x - b->x;
        float a1 = c->y - a->y, b1 = a->x - c->x;
        float a2 = a->y - b->y, b2 = b->x - a->x;
        bool own0 = OwnsEdge(b->x, b->y, c->x, c->y);
        bool own1 = OwnsEdge(c->x, c->y, a->x, a->y);
        bool own2 = OwnsEdge(a->x, a->y, b->x, b->y);
        float inv_area = 1.0f / area;

        const CCFeatures& cc = prg.cc;
        bool depth_enabled = (mCurrentDepthTest || mCurrentDepthMask) && fb.has_depth;
        bool depth_test = depth_enabled && mCurrentDepthTest;
        bool depth_write = depth_enabled && mCurrentDepthMask;
        bool decal = mCurrentZmodeDecal;

        // glPolygonOffset(-2, -2) for decals.
        float z_bias = 0.0f;
        if (decal) {
            float dzdx = (a->z * a0 + b->z * a1 + c->z * a2) * inv_area;
            float dzdy = (a->z * b0 + b->z * b1 + c->z * b2) * inv_area;
            z_bias = -(2.0f * std::max(fabsf(dzdx), fabsf(dzdy)) + 2.0f / 16777216.0f);
        }

        float tex[2][4], texa[2][4], combined[4], noise[4];
        const float* src[S_COUNT];
        src[S_ZERO] = kZero4;
        src[S_ONE] = kOne4;
        // Without alpha, inputs are three floats and their fourth is never read.
        float attr[MAX_ATTRS + 1];
        for (int i = 0; i < MAX_INPUTS; i++) {
            src[S_INPUT1 + i] = i < cc.numInputs ? &attr[prg.input_off + i * prg.input_size] : kZero4;
        }
        src[S_TEX0] = tex[0];
        src[S_TEX0A] = texa[0];
        src[S_TEX1] = tex[1];
        src[S_TEX1A] = texa[1];
        src[S_COMBINED] = combined;
        src[S_NOISE] = noise;
        int channels = cc.opt_alpha ? 4 : 3;

        float px0 = min_x + 0.5f, py0 = min_y + 0.5f;
        float row_w0 = Edge(b->x, b->y, c->x, c->y, px0, py0);
        float row_w1 = Edge(c->x, c->y, a->x, a->y, px0, py0);
        float row_w2 = Edge(a->x, a->y, b->x, b->y, px0, py0);

        // z, 1/w and attribute/w are linear in screen space: their value at
        // the row's first pixel, and their steps per pixel and per row.
        auto plane = [&](float va, float vb, float vc, float& row, float& dx, float& dy) {
            row = (row_w0 * va + row_w1 * vb + row_w2 * vc) * inv_area;
            dx = (a0 * va + a1 * vb + a2 * vc) * inv_area;
            dy = (b0 * va + b1 * vb + b2 * vc) * inv_area;
        };
        float z_row, z_dx, z_dy, q_row, q_dx, q_dy;
        plane(a->z, b->z, c->z, z_row, z_dx, z_dy);
        plane(a->inv_w, b->inv_w, c->inv_w, q_row, q_dx, q_dy);
        z_row += z_bias;
        float attr_row[MAX_ATTRS], attr_dx[MAX_ATTRS], attr_dy[MAX_ATTRS];
        for (int i = 0; i < nattrs; i++) {
            plane(a->attr[i], b->attr[i], c->attr[i], attr_row[i], attr_dx[i], attr_dy[i]);
        }
        int last = max_x - min_x - 1;

        for (int y = min_y; y < max_y; y++) {
            // The pixels of this row inside all three edges, give or take one.
            int xs = 0, xe = last;
            if (SpanLimit(row_w0, a0, last, xs, xe) && SpanLimit(row_w1, a1, last, xs, xe) &&
                SpanLimit(row_w2, a2, last, xs, xe)) {
                float w0 = row_w0 + a0 * xs, w1 = row_w1 + a1 * xs, w2 = row_w2 + a2 * xs;
                for (int k = xs; k <= xe; k++, w0 += a0, w1 += a1, w2 += a2) {
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                        continue;
                    }
                    if ((w0 == 0.0f && !own0) || (w1 == 0.0f && !own1) || (w2 == 0.0f && !own2)) {
                        continue;
                    }

                    int x = min_x + k;
                    float fk = (float)k;
                    float z = cc.opt_prim_depth ? mCurrentPrimDepth : Clamp01(z_row + z_dx * fk);
                    size_t idx = (size_t)y * fb.width + x;
                    if (depth_test && (decal ? z > fb.depth[idx] : z >= fb.depth[idx])) {
                        continue;
                    }

                    // Perspective-correct attributes.
                    float w = 1.0f / (q_row + q_dx * fk);
                    for (int i = 0; i < nattrs; i++) {
                        attr[i] = (attr_row[i] + attr_dx[i] * fk) * w;
                    }

                    for (int t = 0; t < 2; t++) {
                        if (!cc.usedTextures[t]) {
                            memcpy(tex[t], kOne4, sizeof(kOne4));
                            texa[t][0] = texa[t][1] = texa[t][2] = texa[t][3] = 1.0f;
                            continue;
                        }
                        const TexView& view = mViews[t];
                        float u = attr[prg.uv_off[t]], v = attr[prg.uv_off[t] + 1];
                        if (prg.clamp_s_off[t] >= 0 && view.width > 0) {
                            u = std::clamp(u, 0.5f / view.width, attr[prg.clamp_s_off[t]]);
                        }
                        if (prg.clamp_t_off[t] >= 0 && view.height > 0) {
                            v = std::clamp(v, 0.5f / view.height, attr[prg.clamp_t_off[t]]);
                        }
                        Sample(view, u, v, tex[t]);
                        if (cc.used_masks[t]) {
                            float mask[4], blend[4];
                            Sample(mViews[SHADER_FIRST_MASK_TEXTURE + t], u, v, mask);
                            if (cc.used_blend[t]) {
                                Sample(mViews[SHADER_FIRST_REPLACEMENT_TEXTURE + t], u, v, blend);
                            } else {
                                memcpy(blend, kZero4, sizeof(kZero4));
                            }
                            for (int i = 0; i < 4; i++) {
                                tex[t][i] += (blend[i] - tex[t][i]) * mask[3];
                            }
                        }
                        texa[t][0] = texa[t][1] = texa[t][2] = texa[t][3] = tex[t][3];
                    }

                    if (prg.uses_noise) {
                        float n = (Random01((int)((x + 0.5f) * mNoiseScale), (int)((y + 0.5f) * mNoiseScale),
                                            mFrameCount) +
                                   1.0f) *
                                  0.5f;
                        noise[0] = noise[1] = noise[2] = noise[3] = n;
                    }

                    // The color combiner, one or two cycles.
                    float out[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    for (int cy = 0; cy < prg.num_cycles; cy++) {
                        const Cycle& cycle = prg.cycles[cy];
                        if (cycle.wrap_rgb) {
                            float lo = cycle.wrap_rgb == 1 ? -1.01f : -0.51f, hi = cycle.wrap_rgb == 1 ? 1.01f : 1.51f;
                            for (int i = 0; i < 3; i++) {
                                combined[i] = Wrap(combined[i], lo, hi);
                            }
                        }
                        if (cycle.wrap_alpha) {
                            float lo = cycle.wrap_alpha == 1 ? -1.01f : -0.51f, hi = cycle.wrap_alpha == 1 ? 1.01f : 1.51f;
                            combined[3] = Wrap(combined[3], lo, hi);
                        }
                        if (cycle.separate_alpha) {
                            Eval(cycle.rgb, src, 0, 3, out);
                            Eval(cycle.alpha, src, 3, 4, out);
                        } else {
                            Eval(cycle.rgb, src, 0, channels, out);
                        }
                        memcpy(combined, out, sizeof(out));
                    }

                    for (int i = 0; i < channels; i++) {
                        out[i] = Clamp01(Wrap(out[i], -0.51f, 1.51f));
                    }
                    if (cc.opt_fog) {
                        const float* fog = &attr[prg.fog_off];
                        for (int i = 0; i < 3; i++) {
                            out[i] += (fog[i] - out[i]) * fog[3];
                        }
                    }
                    if (cc.opt_alpha) {
                        if (cc.opt_texture_edge) {
                            if (out[3] <= 0.19f) {
                                continue;
                            }
                            out[3] = 1.0f;
                        }
                        if (cc.opt_noise) {
                            float r = Random01((int)((x + 0.5f) * mNoiseScale), (int)((y + 0.5f) * mNoiseScale),
                                               mFrameCount);
                            out[3] *= floorf(Clamp01(r + out[3]));
                        }
                    }
                    if (cc.opt_grayscale) {
                        const float* gray = &attr[prg.gray_off];
                        float intensity = (out[0] + out[1] + out[2]) / 3.0f;
                        for (int i = 0; i < 3; i++) {
                            out[i] += (gray[i] * intensity - out[i]) * gray[3];
                        }
                    }
                    if (cc.opt_alpha) {
                        if (cc.opt_alpha_threshold && out[3] < 8.0f / 256.0f) {
                            continue;
                        }
                        if (cc.opt_invisible) {
                            out[3] = 0.0f;
                        }
                    }

                    if (mUseAlpha) {
                        float dst[4];
                        Unpack(fb.color[idx], dst);
                        float al = Clamp01(out[3]);
                        for (int i = 0; i < 3; i++) {
                            out[i] = out[i] * al + dst[i] * (1.0f - al);
                        }
                    }
                    fb.color[idx] = Pack(out[0], out[1], out[2]);
                    if (depth_write) {
                        fb.depth[idx] = z;
                    }
                }
            }
            row_w0 += b0;
            row_w1 += b1;
            row_w2 += b2;
            z_row += z_dy;
            q_row += q_dy;
            for (int i = 0; i < nattrs; i++) {
                attr_row[i] += attr_dy[i];
            }
        }
    }

    // glBlitFramebuffer with GL_NEAREST. A rectangle given with its corners
    // swapped mirrors the picture along that axis.
    static void Blit(const SoftFramebuffer& src, SoftFramebuffer& dst, int sx0, int sy0, int sx1, int sy1, int dx0,
                     int dy0, int dx1, int dy1) {
        if (&src == &dst || dx0 == dx1 || dy0 == dy1 || src.width == 0 || dst.width == 0) {
            return;
        }
        float scale_x = (float)(sx1 - sx0) / (float)(dx1 - dx0);
        float scale_y = (float)(sy1 - sy0) / (float)(dy1 - dy0);
        int x_lo = std::max(std::min(dx0, dx1), 0), x_hi = std::min(std::max(dx0, dx1), dst.width);
        int y_lo = std::max(std::min(dy0, dy1), 0), y_hi = std::min(std::max(dy0, dy1), dst.height);
        for (int y = y_lo; y < y_hi; y++) {
            int sy = (int)floorf(sy0 + (y + 0.5f - dy0) * scale_y);
            if (sy < 0 || sy >= src.height) {
                continue;
            }
            for (int x = x_lo; x < x_hi; x++) {
                int sx = (int)floorf(sx0 + (x + 0.5f - dx0) * scale_x);
                if (sx < 0 || sx >= src.width) {
                    continue;
                }
                dst.color[(size_t)y * dst.width + x] = src.color[(size_t)sy * src.width + sx];
            }
        }
    }

    std::map<std::pair<uint64_t, uint64_t>, SoftShader> mShaders;
    SoftShader* mShader = nullptr;

    std::vector<SoftTexture> mTextures;
    std::vector<uint32_t> mFreeTextures;
    uint32_t mBound[SHADER_MAX_TEXTURES] = {};
    TexView mViews[SHADER_MAX_TEXTURES];
    int mCurrentTile = 0;
    FilteringMode mFilterMode = FILTER_THREE_POINT;

    std::vector<SoftFramebuffer> mFramebuffers;
    int mCurrentFb = 0;
    int mPresentFb = -1;

    int mVpX = 0, mVpY = 0, mVpW = 0, mVpH = 0;
    int mScX = 0, mScY = 0, mScW = 0, mScH = 0;
    int mRectX0 = 0, mRectY0 = 0, mRectX1 = 0, mRectY1 = 0;
    bool mUseAlpha = false;
    float mNoiseScale = 1.0f;
    uint32_t mFrameCount = 0;
};

} // namespace

double GfxRenderingAPISoft::sDrawSeconds;
std::mutex GfxRenderingAPISoft::sFrontLock;
uint32_t GfxRenderingAPISoft::sFront[WATCH_FB_WIDTH * WATCH_FB_HEIGHT];
uint32_t GfxRenderingAPISoft::sFrontSeq;

double WatchTakeDrawSeconds() {
    double seconds = GfxRenderingAPISoft::sDrawSeconds;
    GfxRenderingAPISoft::sDrawSeconds = 0.0;
    return seconds;
}

GfxRenderingAPI* WatchCreateRenderingApi() {
    return new GfxRenderingAPISoft();
}

} // namespace Fast

extern "C" uint32_t watch_port_copy_frame(uint8_t* dst) {
    std::lock_guard<std::mutex> lock(Fast::GfxRenderingAPISoft::sFrontLock);
    memcpy(dst, Fast::GfxRenderingAPISoft::sFront, sizeof(Fast::GfxRenderingAPISoft::sFront));
    return Fast::GfxRenderingAPISoft::sFrontSeq;
}
