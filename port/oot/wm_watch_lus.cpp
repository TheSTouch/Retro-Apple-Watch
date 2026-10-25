// libultraship window backend for the watch: a fixed WATCH_FB_WIDTH x
// WATCH_FB_HEIGHT surface with no events, keyboard or mouse. The Swift side
// paces the frames and shows what gfx_soft_lus.cpp publishes.
//
// For checks on a Mac, the backend can also run the game headless:
//   WATCH_PREVIEW_DIR     write every WATCH_PREVIEW_EVERY-th frame there as a .ppm
//   WATCH_PREVIEW_FRAMES  stop after this many frames
//   WATCH_PREVIEW_INPUT   buttons to hold from a frame on, "frame:hexmask,..."
//                         (WATCH_BTN_* bits), e.g. "300:1000,305:0" taps Start

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fast/backends/gfx_window_manager_api.h"
#include "fast/Fast3dGui.h"
#include "ship/Context.h"

#include "watch_lus.h"
#include "watch_port.h"

namespace Fast {

namespace {

// The frame rate the game asks for: 60 on the title screen and in menus, 20
// in play (Ship of Harkinian sets it from the game's R_UPDATE_RATE).
std::atomic<int> sTargetFps{ 20 };

struct PreviewInput {
    long frame;
    uint16_t buttons;
};

class GfxWindowBackendWatch final : public GfxWindowBackend {
  public:
    void Init(const char* gameName, const char* apiName, bool startFullScreen, uint32_t width, uint32_t height,
              int32_t posX, int32_t posY) override {
        mStart = std::chrono::steady_clock::now();
        mFullScreen = true;

        // ImGui still gets a context (the game's code calls into it), but no
        // platform or renderer backend: nothing is ever drawn with it.
        GuiWindowInitData impl = {};
        impl.Gx2 = { WATCH_FB_WIDTH, WATCH_FB_HEIGHT };
        impl.Backend = (WindowBackend)WATCH_WINDOW_BACKEND;
        std::dynamic_pointer_cast<Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui())->Init(impl);

        if (const char* dir = getenv("WATCH_PREVIEW_DIR")) {
            mPreviewDir = dir;
        }
        if (const char* frames = getenv("WATCH_PREVIEW_FRAMES")) {
            mPreviewFrames = atol(frames);
        }
        if (const char* every = getenv("WATCH_PREVIEW_EVERY")) {
            mPreviewEvery = std::max(1L, atol(every));
        }
        if (const char* input = getenv("WATCH_PREVIEW_INPUT")) {
            for (const char* p = input; *p;) {
                char* end;
                long frame = strtol(p, &end, 10);
                if (*end != ':') {
                    break;
                }
                uint16_t buttons = (uint16_t)strtoul(end + 1, &end, 16);
                mPreviewInput.push_back({ frame, buttons });
                p = *end == ',' ? end + 1 : end;
            }
        }
    }

    void Close() override {
        mIsRunning = false;
    }

    void SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                              void (*onAllKeysUp)()) override {
    }

    void SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) override {
    }

    void SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) override {
    }

    void SetFullscreen(bool fullscreen) override {
    }

    void GetActiveWindowRefreshRate(uint32_t* refreshRate) override {
        *refreshRate = 60;
    }

    void SetCursorVisibility(bool visibility) override {
    }

    void SetMousePos(int32_t posX, int32_t posY) override {
    }

    void GetMousePos(int32_t* x, int32_t* y) override {
        *x = *y = 0;
    }

    void GetMouseDelta(int32_t* x, int32_t* y) override {
        *x = *y = 0;
    }

    void GetMouseWheel(float* x, float* y) override {
        *x = *y = 0.0f;
    }

    bool GetMouseState(uint32_t btn) override {
        return false;
    }

    void SetMouseCapture(bool capture) override {
    }

    bool IsMouseCaptured() override {
        return false;
    }

    void GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) override {
        *width = WATCH_FB_WIDTH;
        *height = WATCH_FB_HEIGHT;
        *posX = *posY = 0;
    }

    void SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) override {
    }

    Ship::WindowRect GetPrimaryMonitorRect() override {
        return { 0, 0, WATCH_FB_WIDTH, WATCH_FB_HEIGHT };
    }

    void HandleEvents() override {
    }

    bool IsFrameReady() override {
        return true;
    }

    void SwapBuffersBegin() override {
    }

    void SwapBuffersEnd() override {
        mFrame++;
        if (!mPreviewDir.empty() || mPreviewFrames > 0) {
            Preview();
        }
    }

    double GetTime() override {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count();
    }

    int GetTargetFps() override {
        return mTargetFps;
    }

    void SetTargetFps(int fps) override {
        mTargetFps = fps;
        sTargetFps = fps;
    }

    void SetMaxFrameLatency(int latency) override {
    }

    const char* GetKeyName(int scancode) override {
        return "";
    }

    bool CanDisableVsync() override {
        return false;
    }

    bool IsRunning() override {
        return mIsRunning;
    }

    void Destroy() override {
    }

    bool IsFullscreen() override {
        return true;
    }

  private:
    void Preview() {
        for (const PreviewInput& in : mPreviewInput) {
            if (in.frame == mFrame) {
                watch_input_set(in.buttons, 0, 0);
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (mFrame > 1) {
            mFrameTime += std::chrono::duration<double, std::milli>(now - mLastSwap).count();
        }
        mLastSwap = now;
        if (mFrame % 100 == 0) {
            fprintf(stderr, "watch preview: frame %ld, %.1f ms per frame, %.1f ms drawing\n", mFrame,
                    mFrameTime / 100.0, WatchTakeDrawSeconds() * 1000.0 / 100.0);
            mFrameTime = 0.0;
        }

        if (!mPreviewDir.empty() && mFrame % mPreviewEvery == 0) {
            std::vector<uint8_t> rgbx(WATCH_FB_WIDTH * WATCH_FB_HEIGHT * 4);
            watch_port_copy_frame(rgbx.data());
            std::string path = mPreviewDir + "/frame_" + std::to_string(mFrame) + ".ppm";
            if (FILE* f = fopen(path.c_str(), "wb")) {
                fprintf(f, "P6\n%d %d\n255\n", WATCH_FB_WIDTH, WATCH_FB_HEIGHT);
                for (size_t i = 0; i < rgbx.size(); i += 4) {
                    fwrite(&rgbx[i], 1, 3, f);
                }
                fclose(f);
            }
        }

        if (mPreviewFrames > 0 && mFrame >= mPreviewFrames) {
            mIsRunning = false;
        }
    }

    std::chrono::steady_clock::time_point mStart, mLastSwap;
    long mFrame = 0;
    std::string mPreviewDir;
    long mPreviewFrames = 0;
    long mPreviewEvery = 20;
    std::vector<PreviewInput> mPreviewInput;
    double mFrameTime = 0.0;
};

} // namespace

int WatchTargetFps() {
    return sTargetFps;
}

GfxWindowBackend* WatchCreateWindowBackend() {
    return new GfxWindowBackendWatch();
}

} // namespace Fast
