/* System Card 3.0 high-level emulation for the trace harness.
 * The jump table at $E000 holds `FB nn 60` stubs (trap, index, RTS); interrupt vectors
 * point at `FB F0+n 40` stubs. Calls that are not implemented yet are logged. */
#include <string.h>
#include <stdlib.h>
#include "pce.h"

static const char *names[] = {
 "cd_boot","cd_reset","cd_base","cd_read","cd_seek","cd_exec","cd_play","cd_search","cd_pause","cd_stat",
 "cd_subq","cd_dinfo","cd_contnts","cd_subrd","cd_pcmrd","cd_fade","ad_reset","ad_trans","ad_read","ad_write",
 "ad_play","ad_cplay","ad_stop","ad_stat","bm_format","bm_free","bm_read","bm_write","bm_delete","bm_files",
 "ex_getver","ex_setvec","ex_getfnt","ex_joysns","ex_joyrep","ex_scrsiz","ex_dotmod","ex_scrmod","ex_imode","ex_vmode",
 "ex_hmode","ex_vsync","ex_rcron","ex_rcroff","ex_irqon","ex_irqoff","ex_bgon","ex_bgoff","ex_spron","ex_sproff",
 "ex_dspon","ex_dspoff","ex_dmamod","ex_sprdma","ex_satclr","ex_sprput","ex_setrcr","ex_setred","ex_setwrt","ex_setdma",
 "ex_binbcd","ex_bcdbin","ex_rnd","ma_mul8u","ma_mul8s","ma_mul16u","ma_div16s","ma_div16u","ma_sqrt","ma_sin",
 "ma_cos","ma_atni","psg_bios","grp_bios","ex_memopen","psg_driver","ex_colorcmd" };
#define NCALLS (int)(sizeof names / sizeof names[0])

/* BIOS RAM (logical $2000 page, physical bank $F8) */
#define ZP(m, a)    ((m)->ram[(a) & 0xFF])
#define RAMB(m, a)  ((m)->ram[(a) - 0x2000])
#define AL 0xF8
#define AH 0xF9
#define BL 0xFA
#define BH 0xFB
#define CL 0xFC
#define CH 0xFD
#define DL 0xFE
#define DH 0xFF
#define IRQ_M   0xF5
#define CRL_M   0xF3
#define CRH_M   0xF4

#define STUB_BASE 0xF000
enum { STUB_IRQ2 = 0xF0, STUB_IRQ1, STUB_TIMER, STUB_NMI, STUB_RESET, STUB_HOOKRET };

typedef struct { long vsync_count, vsync_wait; int in_wait; uint32_t rnd; } hle_t;
static hle_t H;

static void put16(pce_t *m, uint16_t a, uint16_t v) { m->rom[a - 0xE000] = v; m->rom[a - 0xE000 + 1] = v >> 8; }

void bios_hle_install(pce_t *m) {
    memset(m->rom, 0xFF, sizeof m->rom);
    for (int i = 0; i < NCALLS; i++) {
        m->rom[i * 3 + 0] = 0xFB; m->rom[i * 3 + 1] = i; m->rom[i * 3 + 2] = 0x60;
    }
    /* interrupt stubs: FB id 40 */
    for (int i = 0; i < 5; i++) {
        uint16_t a = STUB_BASE + i * 16 - 0xE000;
        m->rom[a] = 0xFB; m->rom[a + 1] = STUB_IRQ2 + i; m->rom[a + 2] = 0x40;
    }
    /* hook return: PLY PLX PLA RTI */
    { uint16_t a = STUB_BASE + 0x80 - 0xE000; m->rom[a] = 0x7A; m->rom[a+1] = 0xFA; m->rom[a+2] = 0x68; m->rom[a+3] = 0x40; }
    put16(m, 0xFFF6, STUB_BASE + 0x00);
    put16(m, 0xFFF8, STUB_BASE + 0x10);
    put16(m, 0xFFFA, STUB_BASE + 0x20);
    put16(m, 0xFFFC, STUB_BASE + 0x30);
    put16(m, 0xFFFE, STUB_BASE + 0x40);
    m->rom[0x1FF5] = 0x68;  /* read by the loader / main program */
    H.rnd = 0x1234567;
}

static uint16_t ram16(pce_t *m, uint16_t a) { return RAMB(m, a) | (RAMB(m, a + 1) << 8); }

/* call a user routine (ending in RTS) from interrupt context, then PLY/PLX/PLA/RTI */
static void call_hook(pce_t *m, cpu6280 *c, uint16_t target) {
    cpu_push(c, c->a); cpu_push(c, c->x); cpu_push(c, c->y);
    uint16_t ret = STUB_BASE + 0x80 - 1;
    cpu_push(c, ret >> 8); cpu_push(c, ret & 0xFF);
    c->pc = target;
}

static void cd_read(pce_t *m, cpu6280 *c) {
    uint32_t lba = (ZP(m, CL) << 16) | (ZP(m, CH) << 8) | ZP(m, DL);
    uint8_t mode = ZP(m, DH);
    uint16_t addr = ZP(m, BL) | (ZP(m, BH) << 8);
    unsigned bytes;
    if (mode == 0 || mode == 0xFE) bytes = ZP(m, AL) | (ZP(m, AH) << 8);
    else bytes = ZP(m, AL) * 2048;
    if (m->verbose > 1) fprintf(m->log, "  cd_read lba=%06X mode=%02X addr/bank=%04X len=%u\n", lba, mode, addr, bytes);
    if (lba * 2048 + bytes > (unsigned long)m->disc_sectors * 2048) { c->a = 1; return; }
    const uint8_t *src = m->disc + lba * 2048;
    if (mode == 0 || mode == 1) {
        for (unsigned i = 0; i < bytes; i++) cpu_write(c, addr + i, src[i]);
    } else if (mode >= 2 && mode <= 6) {
        /* map bank BL into MPR[mode] window and fill consecutive banks */
        uint8_t bank = ZP(m, BL);
        for (unsigned i = 0; i < bytes; i++) {
            uint32_t ph = ((uint32_t)(bank + (i >> 13)) << 13) | (i & 0x1FFF);
            pce_write(m, ph, src[i]);
        }
        c->mpr[mode] = bank;
    } else if (mode == 0xFE || mode == 0xFF) {
        for (unsigned i = 0; i + 1 < bytes; i += 2)
            m->vdc.vram[(addr + i / 2) & 0x7FFF] = src[i] | (src[i + 1] << 8);
    }
    c->a = 0;
}

static void vsync_work(pce_t *m) {
    vdc_t *v = &m->vdc;
    H.vsync_count++;
    if (ZP(m, IRQ_M) & 0x40) return;   /* BIOS vsync processing disabled */
    v->reg[5] = ZP(m, CRL_M) | (ZP(m, CRH_M) << 8);
    v->reg[7] = ram16(m, 0x220C);
    v->reg[8] = ram16(m, 0x2210);
    /* joypad 0 */
    uint8_t now = m->joy_buttons, old = RAMB(m, 0x2228);
    RAMB(m, 0x2232) = old;
    RAMB(m, 0x2228) = now;
    RAMB(m, 0x222D) |= now & ~old;
    RAMB(m, 0x2241)++;
}

static void colorcmd(pce_t *m) {
    uint16_t bp = ram16(m, 0x2220), sp = ram16(m, 0x2223);
    uint8_t bl = RAMB(m, 0x2222), sl = RAMB(m, 0x2225), cmd = RAMB(m, 0x221F);
    if (m->verbose) fprintf(m->log, "  colorcmd cmd=%d bg=%04X/%d spr=%04X/%d\n", cmd, bp, bl, sp, sl);
    for (int i = 0; i < bl * 16; i++) m->cram[i & 0x1FF] = (cpu_read(&m->cpu, bp + i*2) | (cpu_read(&m->cpu, bp + i*2 + 1) << 8)) & 0x1FF;
    for (int i = 0; i < sl * 16; i++) m->cram[(256 + i) & 0x1FF] = (cpu_read(&m->cpu, sp + i*2) | (cpu_read(&m->cpu, sp + i*2 + 1) << 8)) & 0x1FF;
    RAMB(m, 0x221F) = 0;
}

static void boot(pce_t *m, cpu6280 *c) {
    /* System Card boot: read IPL header (sector 1), load the IPL program, jump. */
    const uint8_t *h = m->disc + 2048;
    uint32_t blk = (h[0] << 16) | (h[1] << 8) | h[2];
    uint8_t bln = h[3];
    uint16_t sta = h[4] | (h[5] << 8), jmp = h[6] | (h[7] << 8);
    c->mpr[0] = 0xFF; c->mpr[1] = 0xF8; c->mpr[7] = 0x00;
    for (int i = 0; i < 5; i++) c->mpr[2 + i] = 0x80 + h[8 + i];
    memset(m->ram, 0, sizeof m->ram);
    for (unsigned i = 0; i < bln * 2048u; i++) cpu_write(c, sta + i, m->disc[blk * 2048 + i]);
    c->s = 0xFF; c->p = 0; c->speed = 1;
    c->irq_mask = 0;
    ZP(m, CRL_M) = 0x08; ZP(m, CRH_M) = 0;
    m->vdc.reg[5] = 0x0008;   /* BIOS leaves vblank IRQ enabled */
    m->vdc.reg[9] = 0x0010;   /* 64x32 BAT */
    m->vdc.reg[0x0A] = 0x0202; m->vdc.reg[0x0B] = 0x041F; m->vdc.reg[0x0C] = 0x0F02; m->vdc.reg[0x0D] = 0x00EF; m->vdc.reg[0x0E] = 0x0003;
    if (m->verbose) fprintf(m->log, "boot: IPL blk=%u n=%u -> $%04X jmp $%04X mpr2-6=%02X..\n", blk, bln, sta, jmp, c->mpr[2]);
    c->pc = jmp;
}

int bios_hle_trap(void *ctx, cpu6280 *c) {
    pce_t *m = ctx;
    uint8_t id = cpu_read(c, c->pc);
    c->pc++;
    if (id >= STUB_IRQ2) {
        switch (id) {
        case STUB_RESET: boot(m, c); return 0;
        case STUB_IRQ1: {
            if (ZP(m, IRQ_M) & 0x02) { c->pc = ram16(m, 0x2202); return 0; }
            uint8_t st = vdc_read(m, 0);
            if (st & 0x20) {
                vsync_work(m);
                if (ZP(m, IRQ_M) & 0x10) { call_hook(m, c, ram16(m, 0x2208)); return 0; }
            }
            if (st & 0x04) {
                if (ZP(m, IRQ_M) & 0x20) { call_hook(m, c, ram16(m, 0x220A)); return 0; }
            }
            return 0;  /* falls through to RTI */
        }
        case STUB_IRQ2:  if (ZP(m, IRQ_M) & 0x01) c->pc = ram16(m, 0x2200); return 0;
        case STUB_TIMER:
            if (ZP(m, IRQ_M) & 0x04) { c->pc = ram16(m, 0x2204); return 0; }
            c->irq_pending &= ~IRQ_TIMER; return 0;
        case STUB_NMI:   if (ZP(m, IRQ_M) & 0x08) c->pc = ram16(m, 0x2206); return 0;
        }
        return 0;
    }
    if (id >= NCALLS) { fprintf(m->log, "bad trap %02X\n", id); return 1; }
    m->bios_calls[id]++;
    /* return address of the JSR */
    uint16_t ret = (cpu_read(c, 0x2100 + (uint8_t)(c->s + 1)) | (cpu_read(c, 0x2100 + (uint8_t)(c->s + 2)) << 8)) + 1;
    static uint8_t seen[0x200000 / 8];
    uint32_t site = cpu_phys(c, ret - 3);
    int first = !(seen[site >> 3] & (1 << (site & 7)));
    seen[site >> 3] |= 1 << (site & 7);
    if (m->verbose && (first || m->verbose > 1) && strcmp(names[id], "ex_vsync"))
        fprintf(m->log, "[f%ld l%d] %s from %04X (bank %02X) A=%02X X=%02X Y=%02X al=%02X bl=%02X bh=%02X cl=%02X ch=%02X dl=%02X dh=%02X\n",
                m->frame, m->line, names[id], ret - 3, c->mpr[(ret - 3) >> 13], c->a, c->x, c->y,
                ZP(m, AL), ZP(m, BL), ZP(m, BH), ZP(m, CL), ZP(m, CH), ZP(m, DL), ZP(m, DH));
    vdc_t *v = &m->vdc;
    switch (id) {
    case 3:  cd_read(m, c); break;                                  /* cd_read */
    case 1: case 2: case 4: case 8: case 9: case 15: c->a = 0; break;
    case 6:  c->a = 0; break;                                       /* cd_play (logged) */
    case 10: { uint16_t a = ZP(m, BL) | (ZP(m, BH) << 8); for (int i = 0; i < 10; i++) cpu_write(c, a + i, 0); cpu_write(c, a, 3); c->a = 0; break; }
    case 30: c->x = 3; c->y = 0; c->a = 0; break;                  /* ex_getver */
    case 31: { uint8_t n = c->a & 7; RAMB(m, 0x2200 + n * 2) = c->x; RAMB(m, 0x2201 + n * 2) = c->y; break; } /* ex_setvec */
    case 74: c->a = 0x68; c->x = 0x18; c->p &= ~F_C; break;        /* ex_memopen */
    case 41:                                                        /* ex_vsync */
        if (!H.in_wait) { H.in_wait = 1; H.vsync_wait = H.vsync_count; }
        if (H.vsync_count == H.vsync_wait) { c->pc -= 2; c->halted = 1; }
        else H.in_wait = 0;
        break;
    case 42: ZP(m, CRL_M) |= 0x04; v->reg[5] |= 0x04; break;       /* rcron */
    case 43: ZP(m, CRL_M) &= ~0x04; v->reg[5] &= ~0x04; break;     /* rcroff */
    case 44: ZP(m, CRL_M) |= 0x08; v->reg[5] |= 0x08; break;       /* irqon */
    case 45: ZP(m, CRL_M) &= ~0x08; v->reg[5] &= ~0x08; break;
    case 46: ZP(m, CRL_M) |= 0x80; break;
    case 47: ZP(m, CRL_M) &= ~0x80; break;
    case 48: ZP(m, CRL_M) |= 0x40; break;
    case 49: ZP(m, CRL_M) &= ~0x40; break;
    case 50: ZP(m, CRL_M) |= 0xC0; break;
    case 51: ZP(m, CRL_M) &= ~0xC0; break;
    case 35: v->reg[9] = (v->reg[9] & ~0x70) | ((c->a & 7) << 4); break; /* ex_scrsiz */
    case 37:                                                        /* ex_scrmod */
        v->reg[0x0B] = (v->reg[0x0B] & 0xFF00) | ((c->x - 1) & 0x7F);
        v->reg[0x0D] = c->y * 8 - 1;
        v->reg[0x0C] = 0x0F02;
        break;
    case 53: v->reg[0x13] = ram16(m, 0x2214); v->satb_pending = 1; break; /* ex_sprdma */
    case 56: v->reg[6] = c->x | (c->y << 8); break;                /* ex_setrcr (to verify) */
    case 62: H.rnd = H.rnd * 1103515245 + 12345; c->a = H.rnd >> 16; break;
    case 63: { unsigned r = ZP(m, AL) * ZP(m, BL); ZP(m, CL) = r; ZP(m, CH) = r >> 8; break; }
    case 64: { int r = (int8_t)ZP(m, AL) * (int8_t)ZP(m, BL); ZP(m, CL) = r; ZP(m, CH) = r >> 8; break; }
    case 65: { uint32_t r = (uint32_t)(ZP(m, AL) | ZP(m, AH) << 8) * (ZP(m, BL) | ZP(m, BH) << 8);
               ZP(m, CL) = r; ZP(m, CH) = r >> 8; ZP(m, DL) = r >> 16; ZP(m, DH) = r >> 24; break; }
    case 76: colorcmd(m); break;
    case 16: case 22: m->adpcm_playing = 0; c->a = 0; break;            /* ad_reset / ad_stop */
    case 23: c->a = m->adpcm_playing ? 1 : 0; c->x = c->a; break;      /* ad_stat */
    case 17:                                                            /* ad_trans: CD -> ADPCM RAM */
    case 18: {                                                          /* ad_read: CD -> ADPCM RAM */
        uint32_t lba = (ZP(m, CL) << 16) | (ZP(m, CH) << 8) | ZP(m, DL);
        uint16_t dst = ZP(m, BL) | (ZP(m, BH) << 8);
        unsigned n = ZP(m, AL) | (ZP(m, AH) << 8);
        if (ZP(m, DH) != 0) n = ZP(m, AL) * 2048;
        for (unsigned i = 0; i < n && lba * 2048 + i < (unsigned long)m->disc_sectors * 2048; i++)
            m->adpcm[(dst + i) & 0xFFFF] = m->disc[lba * 2048 + i];
        if (m->verbose) fprintf(m->log, "  ad_read lba=%06X -> adpcm %04X len=%u type=%02X\n", lba, dst, n, ZP(m, DH));
        c->a = 0; break;
    }
    case 20: case 21:                                                   /* ad_play / ad_cplay */
        if (m->verbose) fprintf(m->log, "  %s addr=%04X len=%04X rate=%02X mode=%02X\n", names[id],
            ZP(m, BL) | ZP(m, BH) << 8, ZP(m, AL) | ZP(m, AH) << 8, ZP(m, DH), ZP(m, DL));
        c->a = 0; break;
    default:
        if (m->verbose && first) fprintf(m->log, "  (not implemented: %s)\n", names[id]);
        break;
    }
    return 0;
}
