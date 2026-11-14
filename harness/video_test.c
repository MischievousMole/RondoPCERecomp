/* video_test SHOT.raw OUTPREFIX [W H]: renders a captured frame through every filter. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
#include "video.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); int hdr[3];
    if (!f || fread(hdr, sizeof hdr, 1, f) != 1) return 1;
    int sw = hdr[0], sh = hdr[1], dot = hdr[2];
    uint32_t *src = malloc(4 * sw * sh); uint8_t *lay = malloc(sw * sh);
    if (fread(src, 4, sw * sh, f) != (size_t)(sw * sh) || fread(lay, 1, sw * sh, f) != (size_t)(sw * sh)) return 1;
    fclose(f);
    int dw = argc > 4 ? atoi(argv[3]) : 2560, dh = argc > 4 ? atoi(argv[4]) : 1440;
    uint32_t *dst = malloc(4 * dw * dh);
    const char *names[] = { "pixel", "sharp", "scanlines", "crt" };
    for (int mode = 0; mode < 5; mode++) {
        video_cfg c; video_default_cfg(&c);
        c.filter = mode < 4 ? mode : VF_SHARP;
        if (mode == 0) c.integer = 1;
        if (mode == 4) { c.bri[1] = 1.25f; c.con[1] = 1.15f; c.bri[0] = 0.85f; }   /* per-layer demo */
        double t0 = now();
        for (int i = 0; i < 10; i++) video_present(src, lay, sw, sw, sh, dot, dst, dw, dh, &c);
        double ms = (now() - t0) / 10;
        int ox, oy, ow, oh; video_geometry(&c, sw, sh, dot, dw, dh, &ox, &oy, &ow, &oh);
        char p[512]; snprintf(p, sizeof p, "%s_%s.ppm", argv[2], mode < 4 ? names[mode] : "layers");
        FILE *o = fopen(p, "wb"); fprintf(o, "P6\n%d %d\n255\n", dw, dh);
        for (int i = 0; i < dw * dh; i++) { fputc(dst[i] >> 16, o); fputc(dst[i] >> 8, o); fputc(dst[i], o); }
        fclose(o);
        printf("%-10s %dx%d image at (%d,%d) size %dx%d  %.1f ms/frame\n", mode < 4 ? names[mode] : "layers", sw, sh, ox, oy, ow, oh, ms);
    }
    return 0;
}
