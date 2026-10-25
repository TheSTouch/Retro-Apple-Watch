// Game core: Super Mario 64 driven one frame at a time. Takes the place of
// the PC port's pc_main.c, which owns a blocking main loop and picks desktop
// backends; on the watch the Swift side calls watch_port_run_frame() at
// watch_port_frame_rate() Hz.
//
// Built by tools/build-game.sh with the decomp's own flags, into the same
// static library as the game.

#include <unistd.h>

#include "sm64.h"
#include "macros.h"

#include "game/game_init.h"
#include "game/memory.h"
#include "audio/external.h"

#include "gfx/gfx_pc.h"
#include "audio/audio_api.h"

#include "watch_port.h"
#include "../src/port_internal.h"

#include "menu_watch.h"

// Globals the game expects the platform layer (pc_main.c) to define.
OSMesg gMainReceivedMesg;
OSMesgQueue gSIEventMesgQueue;
s8 gResetTimer;
s8 gNmiResetBarsTimer;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

extern void thread5_game_loop(void *arg);
extern void create_next_audio_buffer(s16 *samples, u32 num_samples);
void game_loop_one_iteration(void);

static int started;

// The game hands finished display lists to the "RSP"; we interpret them.
void exec_display_list(struct SPTask *spTask) {
    if (started) {
        gfx_run((Gfx *)spTask->task.t.data_ptr);
    }
}

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {
}

void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler, UNUSED OSMesgQueue *queue,
                        UNUSED OSMesg *msg) {
}

// The game produces audio in two chunks per frame. Asking for slightly
// more or fewer samples keeps the output buffer near its target fill level.
#ifdef VERSION_EU
#define SAMPLES_HIGH 656
#define SAMPLES_LOW 640
#else
#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528
#endif

void watch_port_init(const char *data_dir) {
    // Save files (EEPROM) are written to the working directory.
    chdir(data_dir);

    main_pool_init();
    gGfxAllocOnlyPool = alloc_only_pool_init();
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

    gfx_init(&gfx_watch_wm_api, &gfx_soft_api, "Super Mario 64", false);
    audio_watch_api.init();
    audio_init();
    sound_init();

    thread5_game_loop(NULL);
    started = 1;
}

// PAL consoles ran the game at 25 frames per second, and the EU version's
// audio produces 25 frames' worth of samples per second.
int watch_port_frame_rate(void) {
#ifdef VERSION_EU
    return 25;
#else
    return 30;
#endif
}

void watch_port_run_frame(void) {
    watch_menu_frame_tick();
    gfx_start_frame();
    game_loop_one_iteration();

    u32 num_samples = audio_watch_api.buffered() < audio_watch_api.get_desired_buffered() ? SAMPLES_HIGH : SAMPLES_LOW;
    s16 audio_buffer[SAMPLES_HIGH * 2 * 2];
    for (int i = 0; i < 2; i++) {
        create_next_audio_buffer(audio_buffer + i * (num_samples * 2), num_samples);
    }
    audio_watch_api.play((u8 *)audio_buffer, 2 * num_samples * 4);

    gfx_end_frame();
}
