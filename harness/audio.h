#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>

typedef struct psg_ch {
    uint16_t freq; uint8_t ctrl, bal, noise, dda;
    uint8_t wave[32]; uint8_t wave_idx;
    double phase; uint32_t lfsr;
} psg_ch_t;

typedef struct audio {
    psg_ch_t ch[6];
    uint8_t sel, main_lr, lfo_freq, lfo_ctrl;
    /* ADPCM */
    const uint8_t *ad_ram; uint32_t ad_pos, ad_end; double ad_rate, ad_frac;
    int ad_sig, ad_step, ad_on; float ad_last, ad_vol;
    double sample_frac;
    /* CD-DA (positions in 44.1 kHz stereo samples from the start of the disc image) */
    int cd_on, cd_repeat;
    double cd_pos; long cd_start, cd_end;
    float cd_vol, cd_fade;               /* fade: volume change per output sample */
    int (*cd_fetch)(void *ctx, long sample, int16_t *lr);
    void *cd_ctx;
} audio_t;

void audio_reset(audio_t *a);
void psg_write(audio_t *a, int r, uint8_t v);
void adpcm_play(audio_t *a, const uint8_t *ram, uint16_t start, uint16_t len, uint8_t rate);
void adpcm_stop(audio_t *a);
void audio_render(audio_t *a, int16_t *out, int n);
void cdda_play(audio_t *a, long start_frame, long end_frame, int repeat);

#endif
