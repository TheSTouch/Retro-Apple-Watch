// Runs the core on the Mac (no watch, no simulator) and writes one frame as a
// PPM image. Handy for checking renderer changes quickly.
//
//   tools/render-preview/build.sh && build/render-preview out.ppm 60

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "watch_port.h"

int main(int argc, char *argv[]) {
    const char *out_path = argc > 1 ? argv[1] : "preview.ppm";
    int frames = argc > 2 ? atoi(argv[2]) : 30;

    watch_port_init(".");
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < frames; i++) {
        watch_port_run_frame();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / frames;
    fprintf(stderr, "%d frames, %.3f ms per frame\n", frames, ms);

    static uint8_t pixels[WATCH_FB_WIDTH * WATCH_FB_HEIGHT * 4];
    watch_port_copy_frame(pixels);
    FILE *f = fopen(out_path, "wb");
    if (f == NULL) {
        perror(out_path);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", WATCH_FB_WIDTH, WATCH_FB_HEIGHT);
    for (int i = 0; i < WATCH_FB_WIDTH * WATCH_FB_HEIGHT; i++) {
        fwrite(&pixels[i * 4], 1, 3, f);
    }
    fclose(f);
    return 0;
}
