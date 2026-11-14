#ifndef VIDEO_H
#define VIDEO_H
#include <stdint.h>

enum { VF_PIXEL, VF_SHARP, VF_SCANLINES, VF_CRT };
enum { VA_AUTHENTIC, VA_SQUARE, VA_STRETCH };   /* 8:7-style PAR from the dot clock / 1:1 / fill 4:3 */
/* 120 Hz presentation modes (used by the player) */
enum { VP_60, VP_120_REPEAT, VP_120_BFI };

typedef struct video_cfg {
    int filter, integer, aspect, present;
    float bri[2], con[2];          /* [0] backdrop + BG tiles, [1] sprites */
    float scan_strength, mask_strength;
} video_cfg;

void video_default_cfg(video_cfg *c);
void video_geometry(const video_cfg *c, int sw, int sh, int dotclk, int dw, int dh, int *ox, int *oy, int *ow, int *oh);
void video_present(const uint32_t *src, const uint8_t *lay, int stride, int sw, int sh, int dotclk,
                   uint32_t *dst, int dw, int dh, const video_cfg *c);
#endif
