// Game core: Ocarina of Time (Ship of Harkinian) driven one frame at a time.
// The counterpart of port/game/game_main.c for Super Mario 64: the Swift side
// calls watch_port_run_frame() at watch_port_frame_rate() Hz.
//
// Ship of Harkinian's main() is SohWatch_Start() in the watch build, and its
// frame loop is replaced by Graph_RunFrame() (patches/oot).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <unistd.h>

#include "watch_lus.h"
#include "watch_port.h"

extern "C" int SohWatch_Start(int argc, char* argv[]);
extern "C" void Graph_RunFrame(void);
extern "C" void SDL_SetMainReady(void);

// How long frames take (game and drawing), averaged over 100 frames and
// appended to frame-times.log in the data folder, which can be copied off
// the watch with xcrun devicectl device copy from.
static FILE* sFrameLog;
static double sFrameSeconds;
static int sFrames;

extern "C" void watch_port_init(const char* data_dir) {
    // Configuration and save files go to Application Support/OcarinaOfTime:
    // given SHIP_HOME, libultraship also creates the Application Support
    // folder named after its last component. The game's archives (oot.o2r,
    // soh.o2r) are read from the app bundle.
    std::error_code error;
    std::filesystem::path home = std::filesystem::path(getenv("HOME")) / "Library/Application Support/OcarinaOfTime";
    std::filesystem::create_directories(home, error);
    setenv("SHIP_HOME", home.c_str(), 1);
    chdir(home.c_str());

    sFrameLog = fopen("frame-times.log", "a");

    // SDL takes the watch for iOS, where the app must say it owns main().
    SDL_SetMainReady();

    static char name[] = "soh";
    static char* argv[] = { name, nullptr };
    SohWatch_Start(1, argv);
}

extern "C" void watch_port_run_frame(void) {
    auto started = std::chrono::steady_clock::now();
    Graph_RunFrame();
    sFrameSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (++sFrames == 100) {
        size_t underrun, dropped, largest_pull, buffered;
        WatchTakeAudioStats(&underrun, &dropped, &largest_pull, &buffered);
        if (sFrameLog != nullptr) {
            fprintf(sFrameLog,
                    "%ld: %.1f ms per frame, %.1f ms of it drawing; audio: %zu missing, %zu dropped, "
                    "largest pull %zu, %zu queued\n",
                    (long)time(nullptr), sFrameSeconds * 10.0, Fast::WatchTakeDrawSeconds() * 10.0, underrun,
                    dropped, largest_pull, buffered);
            fflush(sFrameLog);
        }
        sFrameSeconds = 0.0;
        sFrames = 0;
    }
}

// The game picks its own rate: 60 frames per second on the title screen and
// in menus, 20 in play (every version runs at NTSC speed in Ship of
// Harkinian). Each frame produces that frame's share of the audio.
extern "C" int watch_port_frame_rate(void) {
    return Fast::WatchTargetFps();
}

// Ocarina of Time has no free-moving menu cursor.
extern "C" int watch_port_menu_pointer_active(void) {
    return 0;
}
