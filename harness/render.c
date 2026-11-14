/* VDC/VCE scanline renderer (BG + 64 sprites, 16 sprites/line limit not enforced). */
#include <string.h>
#include "pce.h"

#define FB_W 512
#define FB_H 242
uint32_t g_fb[FB_W * FB_H];
int g_fb_w = 256, g_fb_h = 224;

static uint32_t rgb(uint16_t c) {
    uint32_t b = c & 7, r = (c >> 3) & 7, g = (c >> 6) & 7;
    return (r * 255 / 7) << 16 | (g * 255 / 7) << 8 | (b * 255 / 7);
}

/* render one displayed line; y = 0.. within the active area; bg_y = BG row */
void render_line(pce_t *m, int y, int bg_y) {
    vdc_t *v = &m->vdc;
    if (y < 0 || y >= FB_H) return;
    int w = ((v->reg[0x0B] & 0x7F) + 1) * 8;
    if (w > FB_W) w = FB_W;
    if (w < 64) w = 256;
    g_fb_w = w;
    uint8_t pix[FB_W]; uint8_t pal[FB_W]; uint8_t spr[FB_W]; uint8_t sprpri[FB_W];
    memset(pix, 0, w); memset(pal, 0, w); memset(spr, 0, w);
    uint16_t cr = v->reg[5];
    if (cr & 0x80) {
        int mw = v->reg[9];
        int wc = (mw >> 4) & 3, tw = wc == 0 ? 32 : wc == 1 ? 64 : 128;
        int th = (mw & 0x40) ? 64 : 32;
        int sx = v->reg[7] & 0x3FF;
        int row = (bg_y >> 3) & (th - 1), ty = bg_y & 7;
        for (int x = 0; x < w; x++) {
            int px = (sx + x) & (tw * 8 - 1);
            uint16_t e = v->vram[(row * tw + (px >> 3)) & 0x7FFF];
            uint16_t ta = (e & 0x7FF) * 16 + ty;
            uint16_t p01 = v->vram[ta & 0x7FFF], p23 = v->vram[(ta + 8) & 0x7FFF];
            int b = 7 - (px & 7);
            int c = ((p01 >> b) & 1) | (((p01 >> (b + 8)) & 1) << 1) | (((p23 >> b) & 1) << 2) | (((p23 >> (b + 8)) & 1) << 3);
            pix[x] = c; pal[x] = e >> 12;
        }
    }
    if (cr & 0x40) {
        for (int i = 63; i >= 0; i--) {
            const uint16_t *s = &v->satb[i * 4];
            int sy = (s[0] & 0x3FF) - 64, sxp = (s[1] & 0x3FF) - 32;
            uint16_t at = s[3];
            int cw = (at & 0x100) ? 2 : 1, chh = ((at >> 12) & 3) == 0 ? 1 : ((at >> 12) & 3) == 1 ? 2 : 4;
            int hgt = chh * 16;
            if (y < sy || y >= sy + hgt) continue;
            int ly = y - sy;
            if (at & 0x8000) ly = hgt - 1 - ly;
            uint16_t pat = (s[2] >> 1) & 0x3FF;
            if (cw == 2) pat &= ~1;
            if (chh == 2) pat &= ~2; else if (chh == 4) pat &= ~6;
            int cy = ly >> 4, ry = ly & 15;
            for (int cx = 0; cx < cw; cx++) {
                int ccx = (at & 0x800) ? (cw - 1 - cx) : cx;
                uint16_t base = ((pat + ccx + cy * 2) & 0x3FF) * 64 + ry;
                uint16_t p0 = v->vram[base & 0x7FFF], p1 = v->vram[(base + 16) & 0x7FFF];
                uint16_t p2 = v->vram[(base + 32) & 0x7FFF], p3 = v->vram[(base + 48) & 0x7FFF];
                for (int k = 0; k < 16; k++) {
                    int bit = (at & 0x800) ? k : 15 - k;
                    int c = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
                    int x = sxp + cx * 16 + k;
                    if (!c || x < 0 || x >= w) continue;
                    spr[x] = c | ((at & 0xF) << 4); sprpri[x] = (at >> 7) & 1;
                }
            }
        }
    }
    uint32_t *out = &g_fb[y * FB_W];
    for (int x = 0; x < w; x++) {
        uint16_t col;
        if (spr[x] && (sprpri[x] || !pix[x])) col = m->cram[256 + spr[x]];
        else if (pix[x]) col = m->cram[pal[x] * 16 + pix[x]];
        else col = m->cram[0];
        out[x] = rgb(col);
    }
}

void render_blank(pce_t *m, int y) {
    if (y < 0 || y >= FB_H) return;
    uint32_t c = rgb(m->cram[0]);
    for (int x = 0; x < FB_W; x++) g_fb[y * FB_W + x] = c;
}

int render_save_ppm(const char *path, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", g_fb_w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < g_fb_w; x++) {
            uint32_t p = g_fb[y * FB_W + x];
            fputc(p >> 16, f); fputc(p >> 8, f); fputc(p, f);
        }
    fclose(f);
    return 0;
}
