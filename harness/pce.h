/* PC Engine CD-ROM2 / Super CD-ROM2 machine for the trace harness. */
#ifndef PCE_H
#define PCE_H
#include <stdint.h>
#include <stdio.h>
#include "cpu6280.h"
#include "audio.h"

#define LINES_PER_FRAME 263
#define CPU_CYC_PER_LINE 455   /* 1365 master clocks / 3 */

typedef struct vdc {
    uint16_t vram[0x8000];
    uint16_t reg[32];
    uint8_t  ar;
    uint8_t  status;
    uint16_t read_latch;
    uint16_t satb[256];
    int satb_pending;
    int line_ctr;          /* raster counter (64 = first displayed line) */
    int bg_y;              /* BG row counter (reloaded on BYR writes) */
    int disp_start, disp_end;
} vdc_t;

typedef struct pce {
    cpu6280 cpu;
    uint8_t rom[0x40000];     /* banks $00-$1F (System Card) */
    uint8_t ram[0x2000];      /* bank $F8 */
    uint8_t cdram[0x10000];   /* banks $80-$87 */
    uint8_t scdram[0x30000];  /* banks $68-$7F */
    int real_bios;
    vdc_t vdc;
    uint16_t cram[512];
    uint16_t vce_addr;
    uint8_t vce_ctrl;
    /* timer */
    uint8_t tm_reload, tm_on, tm_count; int tm_frac;
    /* joypad */
    uint8_t joy_sel, joy_clr, joy_buttons;  /* buttons: bit0 I,1 II,2 SEL,3 RUN,4 U,5 R,6 D,7 L (1=pressed) */
    /* psg */
    uint8_t psg_ch; uint8_t psg_reg[6][10]; uint8_t psg_global[2];
    FILE *psg_log;
    /* CD */
    uint8_t *disc; long disc_sectors;  /* data track, 2048-byte sectors, LBA 0 = track start */
    uint8_t cd_regs[16];
    uint8_t adpcm[0x10000];
    int adpcm_playing;
    uint16_t adpcm_wr;
    /* run state */
    long frame; int line;
    int64_t line_cycles;
    /* tracing */
    uint8_t *cov;             /* executed-opcode bitmap per physical address (2 MB) */
    FILE *log;
    int verbose;
    long bios_calls[64];
    int stop;
    long trace_left;
    audio_t audio; FILE *wav; long wav_samples;
    /* overlay-aware coverage: banks are re-identified after bulk loads */
    uint32_t bank_hash[256];
    uint8_t  bank_dirty[256];
    struct ovl { uint32_t hash; uint8_t bank; uint8_t bits[1024]; } *ovl;
    int novl, ovl_cap;
    int cur_ovl[256];
    const char *outdir;
    long smc_writes;
    /* native code */
    int native_on, difftest_every;
    int bulk_load;                     /* BIOS loader writing: not self-modifying code */
    uint8_t (*volatile_map)[0x2000];   /* bytes patched at runtime: excluded from overlay hashes */
    int16_t ovl_native[4096];          /* per overlay index: native table index, -1 none, -2 unresolved */
    uint8_t ovl_native_off[4096];      /* native disabled for this overlay (self-modifying code) */
    const uint8_t *native_cm[256];     /* codemap of the native overlay active in each bank */
    int native_cm_ovl[256];
    long native_calls, native_instr, interp_instr, diff_checks, diff_fails;
} pce_t;
void pce_mark_dirty(pce_t *m, uint8_t bank);
void pce_save_coverage(pce_t *m);
void pce_load_volatile(pce_t *m, const char *path);

int  pce_init(pce_t *m, const char *disc_iso, const char *bios_rom);
void pce_run_frame(pce_t *m);
uint8_t pce_read(void *ctx, uint32_t a);
void pce_write(void *ctx, uint32_t a, uint8_t v);
void vdc_write(pce_t *m, int port, uint8_t v);
uint8_t vdc_read(pce_t *m, int port);
void render_line(pce_t *m, int y, int bg_y);
void render_blank(pce_t *m, int y);
int render_save_ppm(const char *path, int h);
extern uint32_t g_fb[];
extern int g_fb_w, g_fb_h;

/* bios_hle.c */
void bios_hle_install(pce_t *m);
int  bios_hle_trap(void *ctx, cpu6280 *c);
void bios_vblank(pce_t *m);

#endif
