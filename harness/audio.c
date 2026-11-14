/* HuC6280 PSG (6 wavetable channels, noise on 4/5) + MSM5205 ADPCM, rendered per scanline at 48 kHz. */
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "pce.h"
#include "audio.h"

#define PSG_CLOCK 3579545.0
#define RATE 48000

static float vol_tab[32];      /* 5-bit channel volume, 1.5 dB steps */
static float lr_tab[16];       /* 4-bit main/balance volume, 3 dB steps */
static int tabs_ready;

static void init_tabs(void) {
    for (int i = 0; i < 32; i++) vol_tab[i] = i ? powf(10.0f, -1.5f * (31 - i) / 20.0f) : 0.0f;
    for (int i = 0; i < 16; i++) lr_tab[i] = i ? powf(10.0f, -3.0f * (15 - i) / 20.0f) : 0.0f;
    tabs_ready = 1;
}

void audio_reset(audio_t *a) {
    memset(a, 0, sizeof *a);
    if (!tabs_ready) init_tabs();
    for (int i = 0; i < 6; i++) a->ch[i].lfsr = 1;
}

void psg_write(audio_t *a, int r, uint8_t v) {
    psg_ch_t *c = (a->sel < 6) ? &a->ch[a->sel] : NULL;
    switch (r) {
    case 0: a->sel = v & 7; break;
    case 1: a->main_lr = v; break;
    case 2: if (c) c->freq = (c->freq & 0xF00) | v; break;
    case 3: if (c) c->freq = (c->freq & 0x0FF) | ((v & 0x0F) << 8); break;
    case 4:
        if (!c) break;
        if ((c->ctrl & 0x40) && !(v & 0x40)) c->wave_idx = 0;   /* DDA off -> reset index */
        if (!(v & 0x80) && (v & 0x40)) c->wave_idx = 0;
        c->ctrl = v;
        break;
    case 5: if (c) c->bal = v; break;
    case 6:
        if (!c) break;
        if (c->ctrl & 0x40) c->dda = v & 0x1F;
        else if (!(c->ctrl & 0x80)) { c->wave[c->wave_idx] = v & 0x1F; c->wave_idx = (c->wave_idx + 1) & 31; }
        break;
    case 7: if (c && a->sel >= 4) c->noise = v; break;
    case 8: a->lfo_freq = v; break;
    case 9: a->lfo_ctrl = v; break;
    }
}

/* MSM5205 */
static const int step_tab[49] = {
    16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
    1411, 1552 };
static const int idx_adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

void adpcm_play(audio_t *a, const uint8_t *ram, uint16_t start, uint16_t len, uint8_t rate) {
    a->ad_ram = ram; a->ad_pos = (uint32_t)start * 2; a->ad_end = ((uint32_t)start + len) * 2;
    a->ad_rate = 32000.0 / (16 - (rate & 15));
    a->ad_sig = 0; a->ad_step = 0; a->ad_frac = 0; a->ad_on = 1;
}
void adpcm_stop(audio_t *a) { a->ad_on = 0; }

static int adpcm_next(audio_t *a) {
    uint8_t b = a->ad_ram[(a->ad_pos >> 1) & 0xFFFF];
    int n = (a->ad_pos & 1) ? (b >> 4) : (b & 0x0F);   /* low nibble first */
    a->ad_pos++;
    int ss = step_tab[a->ad_step];
    int d = ss >> 3;
    if (n & 1) d += ss >> 2;
    if (n & 2) d += ss >> 1;
    if (n & 4) d += ss;
    a->ad_sig += (n & 8) ? -d : d;
    if (a->ad_sig > 2047) a->ad_sig = 2047;
    if (a->ad_sig < -2048) a->ad_sig = -2048;
    a->ad_step += idx_adj[n & 7];
    if (a->ad_step < 0) a->ad_step = 0;
    if (a->ad_step > 48) a->ad_step = 48;
    return a->ad_sig;
}

void cdda_play(audio_t *a, long start_frame, long end_frame, int repeat) {
    a->cd_start = start_frame * 588; a->cd_end = end_frame * 588;
    a->cd_pos = a->cd_start; a->cd_repeat = repeat; a->cd_on = 1;
    if (a->cd_vol <= 0 || a->cd_fade < 0) { a->cd_vol = 1.0f; a->cd_fade = 0; }
}

/* render `n` stereo samples into out (interleaved int16) */
void audio_render(audio_t *a, int16_t *out, int n) {
    float ml = lr_tab[a->main_lr >> 4], mr = lr_tab[a->main_lr & 15];
    for (int s = 0; s < n; s++) {
        float L = 0, R = 0;
        for (int i = 0; i < 6; i++) {
            psg_ch_t *c = &a->ch[i];
            if (!(c->ctrl & 0x80)) continue;
            float v;
            if (c->ctrl & 0x40) v = c->dda;
            else if (i >= 4 && (c->noise & 0x80)) {
                double nf = PSG_CLOCK / 64.0 / ((~c->noise & 0x1F) + 1) * 4;
                c->phase += nf / RATE;
                while (c->phase >= 1.0) {
                    c->phase -= 1.0;
                    c->lfsr = (c->lfsr >> 1) | ((((c->lfsr >> 0) ^ (c->lfsr >> 1) ^ (c->lfsr >> 11) ^ (c->lfsr >> 12) ^ (c->lfsr >> 17)) & 1) << 17);
                }
                v = (c->lfsr & 1) ? 31 : 0;
            } else {
                unsigned p = c->freq ? c->freq : 4096;
                double f = PSG_CLOCK / (32.0 * p);
                if (p < 8) v = 16;          /* ultrasonic: silence-ish */
                else {
                    c->phase += f * 32.0 / RATE;
                    while (c->phase >= 32.0) c->phase -= 32.0;
                    v = c->wave[(int)c->phase];
                }
            }
            float amp = (v - 16.0f) / 16.0f * vol_tab[c->ctrl & 0x1F];
            L += amp * lr_tab[c->bal >> 4] * ml;
            R += amp * lr_tab[c->bal & 15] * mr;
        }
        L *= 0.18f; R *= 0.18f;
        if (a->ad_on) {
            a->ad_frac += a->ad_rate / RATE;
            while (a->ad_frac >= 1.0 && a->ad_on) {
                a->ad_frac -= 1.0;
                a->ad_last = adpcm_next(a) / 2048.0f;
                if (a->ad_pos >= a->ad_end) a->ad_on = 0;
            }
            L += a->ad_last * 0.5f * a->ad_vol; R += a->ad_last * 0.5f * a->ad_vol;
        }
        if (a->cd_on && a->cd_fetch) {
            int16_t lr[2] = {0, 0};
            if (a->cd_fetch(a->cd_ctx, (long)a->cd_pos, lr)) {
                L += lr[0] / 32768.0f * 0.8f * a->cd_vol;
                R += lr[1] / 32768.0f * 0.8f * a->cd_vol;
            }
            a->cd_pos += 44100.0 / RATE;
            if (a->cd_pos >= a->cd_end) {
                if (a->cd_repeat) a->cd_pos = a->cd_start; else a->cd_on = 0;
            }
            if (a->cd_fade != 0) {
                a->cd_vol += a->cd_fade;
                if (a->cd_vol <= 0) { a->cd_vol = 0; a->cd_fade = 0; }
            }
        }
        int il = (int)(L * 32767), ir = (int)(R * 32767);
        out[s * 2] = il > 32767 ? 32767 : il < -32768 ? -32768 : il;
        out[s * 2 + 1] = ir > 32767 ? 32767 : ir < -32768 ? -32768 : ir;
    }
}
