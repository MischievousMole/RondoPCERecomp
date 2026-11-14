/* Output scaler: PC Engine frame -> any resolution (target 2560x1440).
 *  - per-layer brightness/contrast (backdrop+BG vs sprites), applied in gamma space
 *  - geometry: real pixel aspect from the VCE dot clock (256 px mode = 8:7), fill-height or integer scale
 *  - filters: PIXEL (nearest), SHARP (sharp bilinear), SCANLINES, CRT (scanlines + aperture grille)
 *  - scanline beams are computed in linear light; beam width grows with brightness like a real CRT spot
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "video.h"
#include <pthread.h>

#ifndef VIDEO_THREADS
#define VIDEO_THREADS 8
#endif

static float to_lin[256];
static uint8_t to_srgb[4096];
static int lut_ready;

static void init_luts(void) {
    for (int i = 0; i < 256; i++) to_lin[i] = powf(i / 255.0f, 2.2f);
    for (int i = 0; i < 4096; i++) { float v = powf(i / 4095.0f, 1.0f / 2.2f); to_srgb[i] = (uint8_t)(v * 255.0f + 0.5f); }
    lut_ready = 1;
}

void video_default_cfg(video_cfg *c) {
    memset(c, 0, sizeof *c);
    c->filter = VF_SHARP; c->integer = 0; c->aspect = VA_AUTHENTIC;
    c->bri[0] = c->bri[1] = 1.0f; c->con[0] = c->con[1] = 1.0f;
    c->scan_strength = 0.75f; c->mask_strength = 0.5f;
}

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

void video_geometry(const video_cfg *c, int sw, int sh, int dotclk, int dw, int dh, int *ox, int *oy, int *ow, int *oh) {
    static const float par_tab[4] = { 8.0f / 7.0f, 6.0f / 7.0f, 4.0f / 7.0f, 4.0f / 7.0f };
    float par = c->aspect == VA_SQUARE ? 1.0f : par_tab[dotclk & 3];
    float sy = (float)dh / sh;
    if (c->integer) sy = floorf(sy);
    float w = sw * par * sy;
    if (c->aspect == VA_STRETCH) w = dh * 4.0f / 3.0f;
    if (w > dw) { sy *= dw / w; w = dw; }
    *ow = (int)(w + 0.5f); *oh = (int)(sh * sy + 0.5f);
    *ox = (dw - *ow) / 2; *oy = (dh - *oh) / 2;
}

typedef struct job { const video_cfg *c; int sw, sh, ow, oh, ox, oy, dw; const float *lin; float *hres; uint8_t *hlev; uint32_t *dst; } job_t;
typedef struct job_part { const job_t *j; int stage, t; } job_part_t;
static void stage_h(const job_t *J, int x0, int x1);
static void stage_v(const job_t *J, int y0, int y1);
static void *worker(void *arg) {
    job_part_t *p = arg; const job_t *J = p->j;
    if (p->stage == 0) stage_h(J, J->ow * p->t / VIDEO_THREADS, J->ow * (p->t + 1) / VIDEO_THREADS);
    else stage_v(J, J->oh * p->t / VIDEO_THREADS, J->oh * (p->t + 1) / VIDEO_THREADS);
    return NULL;
}

void video_present(const uint32_t *src, const uint8_t *lay, int stride, int sw, int sh, int dotclk,
                   uint32_t *dst, int dw, int dh, const video_cfg *c) {
    if (!lut_ready) init_luts();
    int ox, oy, ow, oh;
    video_geometry(c, sw, sh, dotclk, dw, dh, &ox, &oy, &ow, &oh);
    for (int i = 0; i < dw * dh; i++) dst[i] = 0;
    if (ow <= 0 || oh <= 0) return;

    /* 1. per-layer adjust + linearize */
    static float *lin; static int lin_n;
    if (lin_n < sw * sh * 3) { free(lin); lin = malloc(sizeof(float) * sw * sh * 3); lin_n = sw * sh * 3; }
    uint8_t adj[2][256];
    for (int l = 0; l < 2; l++)
        for (int v = 0; v < 256; v++) {
            float x = ((v / 255.0f - 0.5f) * c->con[l] + 0.5f) * c->bri[l];
            adj[l][v] = (uint8_t)(clampf(x, 0, 1) * 255.0f + 0.5f);
        }
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            uint32_t p = src[y * stride + x];
            int l = lay[y * stride + x] == 2 ? 1 : 0;
            float *o = &lin[(y * sw + x) * 3];
            o[0] = to_lin[adj[l][(p >> 16) & 255]]; o[1] = to_lin[adj[l][(p >> 8) & 255]]; o[2] = to_lin[adj[l][p & 255]];
        }

    /* 2. horizontal resample of every source line to the output width */
    static float *hres; static uint8_t *hlev; static int hres_n;
    if (hres_n < ow * sh * 3) { free(hres); free(hlev); hres = malloc(sizeof(float) * ow * sh * 3); hlev = malloc(ow * sh * 3); hres_n = ow * sh * 3; }
    job_t J = { c, sw, sh, ow, oh, ox, oy, dw, lin, hres, hlev, dst };
    pthread_t th[VIDEO_THREADS]; job_part_t parts[VIDEO_THREADS];
    for (int stage = 0; stage < 2; stage++) {
        for (int t = 0; t < VIDEO_THREADS; t++) {
            parts[t].j = &J; parts[t].stage = stage; parts[t].t = t;
            pthread_create(&th[t], NULL, worker, &parts[t]);
        }
        for (int t = 0; t < VIDEO_THREADS; t++) pthread_join(th[t], NULL);
    }
}

static void stage_h(const job_t *J, int x0, int x1) {
    const video_cfg *c = J->c; int sw = J->sw, sh = J->sh, ow = J->ow;
    const float *lin = J->lin; float *hres = J->hres;
    float sx = (float)ow / sw;
    float hsharp = c->filter == VF_CRT ? sx * 0.45f : sx;    /* CRT: softer horizontal edges */
    for (int x = x0; x < x1; x++) {
        float u = (x + 0.5f) / sx - 0.5f;
        int i0 = (int)floorf(u); float f = u - i0;
        if (c->filter == VF_PIXEL) { i0 = (int)((x + 0.5f) / sx); f = 0; }
        else f = clampf((f - 0.5f) * hsharp + 0.5f, 0, 1);
        int a = i0 < 0 ? 0 : i0 >= sw ? sw - 1 : i0, b = i0 + 1 >= sw ? sw - 1 : i0 + 1 < 0 ? 0 : i0 + 1;
        for (int y = 0; y < sh; y++) {
            const float *pa = &lin[(y * sw + a) * 3], *pb = &lin[(y * sw + b) * 3];
            float *o = &hres[(y * ow + x) * 3];
            o[0] = pa[0] + (pb[0] - pa[0]) * f; o[1] = pa[1] + (pb[1] - pa[1]) * f; o[2] = pa[2] + (pb[2] - pa[2]) * f;
            uint8_t *lv = &J->hlev[(y * ow + x) * 3];
            lv[0] = (uint8_t)(clampf(o[0], 0, 1) * 32.0f); lv[1] = (uint8_t)(clampf(o[1], 0, 1) * 32.0f); lv[2] = (uint8_t)(clampf(o[2], 0, 1) * 32.0f);
        }
    }

}

/* 3. vertical: per output row, either sharp-bilinear or scanline beams (3 taps, intensity-dependent width) */
static void stage_v(const job_t *J, int y0, int y1) {
    const video_cfg *c = J->c; int sh = J->sh, ow = J->ow, oh = J->oh, ox = J->ox, oy = J->oy, dw = J->dw;
    const float *hres = J->hres; uint32_t *dst = J->dst;
    float syf = (float)oh / sh;
    int scan = c->filter == VF_SCANLINES || c->filter == VF_CRT;
    float mask_lo = 1.0f - 0.6f * c->mask_strength;
    float mask_gain = 3.0f / (1.0f + 2.0f * mask_lo);
    for (int y = y0; y < y1; y++) {
        float v = (y + 0.5f) / syf;
        int j = (int)floorf(v);
        uint32_t *out = &dst[(oy + y) * dw + ox];
        if (!scan) {
            int j0, j1; float f;
            if (c->filter == VF_PIXEL) { j0 = j1 = j < sh ? j : sh - 1; f = 0; }
            else {
                float t = v - 0.5f; j0 = (int)floorf(t); f = clampf((t - j0 - 0.5f) * syf + 0.5f, 0, 1);
                j1 = j0 + 1; if (j0 < 0) j0 = 0; if (j1 >= sh) j1 = sh - 1; if (j0 >= sh) j0 = sh - 1;
            }
            const float *r0 = &hres[j0 * ow * 3], *r1 = &hres[j1 * ow * 3];
            for (int x = 0; x < ow; x++) {
                float r = r0[x*3] + (r1[x*3] - r0[x*3]) * f, g = r0[x*3+1] + (r1[x*3+1] - r0[x*3+1]) * f, b = r0[x*3+2] + (r1[x*3+2] - r0[x*3+2]) * f;
                out[x] = 0xFF000000u | to_srgb[(int)(clampf(r,0,1) * 4095)] << 16 | to_srgb[(int)(clampf(g,0,1) * 4095)] << 8 | to_srgb[(int)(clampf(b,0,1) * 4095)];
            }
            continue;
        }
        /* beam weights for taps j-1, j, j+1 at 33 intensity levels */
        float w[3][33];
        for (int k = 0; k < 3; k++) {
            float d = v - (j - 1 + k + 0.5f);
            for (int L = 0; L <= 32; L++) {
                float sigma = 0.20f + 0.16f * (L / 32.0f);
                float beam = expf(-0.5f * (d / sigma) * (d / sigma)) / (sigma * 2.5066f);
                float plain = k == 1 ? 1.0f : 0.0f;
                w[k][L] = plain + (beam - plain) * c->scan_strength;
            }
        }
        const float *rows[3]; const uint8_t *lvs[3];
        for (int k = 0; k < 3; k++) { int jj = j - 1 + k; jj = jj < 0 ? 0 : jj >= sh ? sh - 1 : jj; rows[k] = &hres[jj * ow * 3]; lvs[k] = &J->hlev[jj * ow * 3]; }
        for (int x = 0; x < ow; x++) {
            float o[3] = {0, 0, 0};
            for (int k = 0; k < 3; k++) {
                const float *s = &rows[k][x * 3]; const uint8_t *lv = &lvs[k][x * 3];
                o[0] += s[0] * w[k][lv[0]]; o[1] += s[1] * w[k][lv[1]]; o[2] += s[2] * w[k][lv[2]];
            }
            if (c->filter == VF_CRT) {
                int ph = (ox + x) % 3;
                for (int ch = 0; ch < 3; ch++) o[ch] *= (ch == ph ? 1.0f : mask_lo) * mask_gain;
            }
            out[x] = 0xFF000000u | to_srgb[(int)(clampf(o[0],0,1) * 4095)] << 16 | to_srgb[(int)(clampf(o[1],0,1) * 4095)] << 8 | to_srgb[(int)(clampf(o[2],0,1) * 4095)];
        }
    }
}
