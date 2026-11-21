/* System Card 3.0 high-level emulation for the trace harness.
 * The jump table at $E000 holds `FB nn 60` stubs (trap, index, RTS); interrupt vectors
 * point at `FB F0+n 40` stubs. Calls that are not implemented yet are logged. */
#include <string.h>
#include <stdlib.h>
#include "pce.h"
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

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
static uint16_t g_hookret = STUB_BASE + 0x80;
static void call_hook(pce_t *m, cpu6280 *c, uint16_t target) {
    cpu_push(c, c->a); cpu_push(c, c->x); cpu_push(c, c->y);
    uint16_t ret = g_hookret - 1;
    cpu_push(c, ret >> 8); cpu_push(c, ret & 0xFF);
    c->pc = target;
}

/* backup RAM: one host file per BRAM entry under bram_dir, named by the 12-byte entry name */
const char *bram_dir = "build/bram";
static void bram_path(pce_t *m, char *out, size_t n) {
    uint16_t np = ZP(m, AL) | (ZP(m, AH) << 8);
    char name[40]; int k = 0;
    for (int i = 0; i < 12; i++) k += sprintf(name + k, "%02X", cpu_read(&m->cpu, np + i));
    snprintf(out, n, "%s/%s.sav", bram_dir, name);
}
static int bram_io(pce_t *m, cpu6280 *c, int write) {
    char p[256]; bram_path(m, p, sizeof p);
    uint16_t buf = ZP(m, BL) | (ZP(m, BH) << 8);
    unsigned len = ZP(m, CL) | (ZP(m, CH) << 8), off = ZP(m, DL) | (ZP(m, DH) << 8);
    uint8_t data[2048] = {0};
    FILE *f = fopen(p, "rb"); size_t have = 0;
    if (f) { have = fread(data, 1, sizeof data, f); fclose(f); }
    if (!write) {
        if (!f) return 1;
        for (unsigned i = 0; i < len && off + i < sizeof data; i++) cpu_write(c, buf + i, data[off + i]);
        return 0;
    }
    for (unsigned i = 0; i < len && off + i < sizeof data; i++) data[off + i] = cpu_read(c, buf + i);
    if (off + len > have) have = off + len;
#ifdef _WIN32
    _mkdir(bram_dir);
#else
    mkdir(bram_dir, 0755);
#endif
    f = fopen(p, "wb"); if (!f) return 2;
    fwrite(data, 1, have, f); fclose(f);
    return 0;
}

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
static long msf_frame(uint8_t mm, uint8_t ss, uint8_t ff) { return ((long)bcd(mm) * 60 + bcd(ss)) * 75 + bcd(ff) - 150; }
static long track_frame(pce_t *m, int track, int end) {
    for (int i = 0; i < m->ntoc; i++) if (m->toc[i].track == track) return m->toc[i].start;
    if (end && m->ntoc) return m->toc[m->ntoc - 1].start + m->toc[m->ntoc - 1].frames;
    return -1;
}
static uint8_t tobcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void cd_play(pce_t *m, cpu6280 *c) {
    uint8_t st = ZP(m, BH), et = ZP(m, DH);
    long s, e;
    switch (st & 0xC0) {
    case 0x00: s = (ZP(m, AL) << 16) | (ZP(m, AH) << 8) | ZP(m, BL); break;
    case 0x40: s = msf_frame(ZP(m, AL), ZP(m, AH), ZP(m, BL)); break;
    default:   s = track_frame(m, bcd(ZP(m, AL)), 0); break;
    }
    switch (et & 0xC0) {
    case 0x00: e = (ZP(m, CL) << 16) | (ZP(m, CH) << 8) | ZP(m, DL); break;
    case 0x40: e = msf_frame(ZP(m, CL), ZP(m, CH), ZP(m, DL)); break;
    case 0x80: e = track_frame(m, bcd(ZP(m, CL)), 1); break;
    default:   e = track_frame(m, 999, 1); break;
    }
    int mode = et & 0x0F;
    int track = 0;
    for (int i = 0; i < m->ntoc; i++) if (s >= m->toc[i].start && s < m->toc[i].start + m->toc[i].frames) track = m->toc[i].track;
    if (m->verbose) fprintf(m->log, "  cd_play frames %ld-%ld (track %d) mode %d\n", s, e, track, mode);
    if (s >= 0 && e > s && mode != 0) cdda_play(&m->audio, s, e, mode == 1);
    m->cd_track = track;
    c->a = 0;
}

static void cd_subq(pce_t *m, cpu6280 *c) {
    uint16_t a = ZP(m, BL) | (ZP(m, BH) << 8);
    audio_t *au = &m->audio;
    long fr = (long)(au->cd_pos / 588);
    int track = 0; long tstart = 0;
    for (int i = 0; i < m->ntoc; i++) if (fr >= m->toc[i].start && fr < m->toc[i].start + m->toc[i].frames) { track = m->toc[i].track; tstart = m->toc[i].start; }
    long rel = fr - tstart, ab = fr + 150;
    uint8_t q[10] = { au->cd_on ? 0 : 3, 0x01, tobcd(track), 0x01,
                      tobcd(rel / 4500), tobcd(rel / 75 % 60), tobcd(rel % 75),
                      tobcd(ab / 4500), tobcd(ab / 75 % 60), tobcd(ab % 75) };
    for (int i = 0; i < 10; i++) cpu_write(c, a + i, q[i]);
    c->a = 0;
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
        for (unsigned i = 0; i < bytes; i++) { cpu_write(c, addr + i, src[i]); if ((i & 0x1FFF) == 0 || i == bytes - 1) pce_mark_dirty(m, cpu_phys(c, addr + i) >> 13); }
    } else if (mode >= 2 && mode <= 6) {
        /* map bank BL into MPR[mode] window and fill consecutive banks */
        uint8_t bank = ZP(m, BL);
        for (unsigned i = 0; i < bytes; i++) {
            uint32_t ph = ((uint32_t)(bank + (i >> 13)) << 13) | (i & 0x1FFF);
            pce_write(m, ph, src[i]);
            pce_mark_dirty(m, ph >> 13);
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
    pce_mark_dirty(m, 0xF8);
    c->s = 0xFF; c->p = 0; c->speed = 1;
    c->irq_mask = 0;
    ZP(m, CRL_M) = 0x08; ZP(m, CRH_M) = 0;
    m->vdc.reg[5] = 0x0008;   /* BIOS leaves vblank IRQ enabled */
    m->vdc.reg[9] = 0x0010;   /* 64x32 BAT */
    m->vdc.reg[0x0A] = 0x0202; m->vdc.reg[0x0B] = 0x041F; m->vdc.reg[0x0C] = 0x0F02; m->vdc.reg[0x0D] = 0x00EF; m->vdc.reg[0x0E] = 0x0003;
    if (m->verbose) fprintf(m->log, "boot: IPL blk=%u n=%u -> $%04X jmp $%04X mpr2-6=%02X..\n", blk, bln, sta, jmp, c->mpr[2]);
    c->pc = jmp;
}

static int bios_hle_exec(pce_t *m, cpu6280 *c, uint8_t id);

int bios_hle_trap(void *ctx, cpu6280 *c) {
    pce_t *m = ctx;
    uint8_t id = cpu_read(c, c->pc);
    c->pc++;
    return bios_hle_exec(m, c, id);
}

/* Real System Card mode: calls listed here run the ROM's own code; everything else is intercepted. */
static const uint8_t passthrough[] = { 32, 60, 61, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 75 };
#define MAGIC_PC 0xFFF2
int16_t g_bios_icept[0x2000];
#define icept g_bios_icept

void bios_real_install(pce_t *m) {
    for (int i = 0; i < 0x2000; i++) icept[i] = -1;
    for (int i = 0; i < NCALLS; i++) {
        int pass = 0;
        for (unsigned k = 0; k < sizeof passthrough; k++) if (passthrough[k] == i) pass = 1;
        if (!pass) icept[i * 3] = i;
    }
    static const int vec_ids[5] = { STUB_IRQ2, STUB_IRQ1, STUB_TIMER, STUB_NMI, STUB_RESET };
    for (int i = 0; i < 5; i++) {
        uint16_t v = m->rom[0x1FF6 + i * 2] | (m->rom[0x1FF7 + i * 2] << 8);
        icept[v & 0x1FFF] = vec_ids[i];
    }
    g_hookret = 0xFFF0;
    icept[0x1FF0] = STUB_HOOKRET;
    H.rnd = 0x1234567;
}

/* called before every instruction in real-BIOS mode; returns 1 if it handled the PC */
int bios_real_intercept(pce_t *m, cpu6280 *c) {
    if (c->mpr[c->pc >> 13] != 0) return 0;
    int id = icept[c->pc & 0x1FFF];
    if (id < 0) return 0;
    uint16_t entry = c->pc;
    if (id == STUB_HOOKRET) {        /* PLY PLX PLA RTI */
        c->y = cpu_pull(c); c->x = cpu_pull(c); c->a = cpu_pull(c);
        c->p = cpu_pull(c); uint16_t l = cpu_pull(c); c->pc = l | (cpu_pull(c) << 8);
        return 1;
    }
    c->pc = MAGIC_PC;
    bios_hle_exec(m, c, id);
    if (c->pc == MAGIC_PC) {
        if (id >= STUB_IRQ2) { c->p = cpu_pull(c); uint16_t l = cpu_pull(c); c->pc = l | (cpu_pull(c) << 8); }
        else { uint16_t l = cpu_pull(c); c->pc = (l | (cpu_pull(c) << 8)) + 1; }
    } else if (c->pc == MAGIC_PC - 2) {
        c->pc = entry;               /* ex_vsync: retry after the next interrupt */
    }
    return 1;
}

static int bios_hle_exec(pce_t *m, cpu6280 *c, uint8_t id) {
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
    m->bulk_load = 1;
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
    case 8: m->audio.cd_on = 0; c->a = 0; break;                    /* cd_pause */
    case 15:                                                        /* cd_fade: A = 0 restore, 8/C fade out (6 s / 2.5 s) */
        if (c->a == 0) { m->audio.cd_vol = 1.0f; m->audio.cd_fade = 0; m->audio.ad_vol = 1.0f; }
        else m->audio.cd_fade = -1.0f / (48000.0f * ((c->a & 4) ? 2.5f : 6.0f));
        c->a = 0; break;
    case 1: case 2: case 4: case 9: c->a = 0; break;
    case 6:  cd_play(m, c); break;                                  /* cd_play */
    case 10: cd_subq(m, c); break;
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
    case 26: c->a = bram_io(m, c, 0); break;                           /* bm_read */
    case 27: c->a = bram_io(m, c, 1); break;                           /* bm_write */
    case 28: { char p[256]; bram_path(m, p, sizeof p); remove(p); c->a = 0; break; } /* bm_delete */
    case 25: ZP(m, CL) = 0x00; ZP(m, CH) = 0x07; c->a = 0; break;       /* bm_free: 1792 bytes */
    case 24: c->a = 0; break;                                          /* bm_format */
    case 33: {                                                         /* ex_joysns */
        uint8_t now = m->joy_buttons, old = RAMB(m, 0x2228);
        RAMB(m, 0x2232) = old; RAMB(m, 0x2228) = now;
        RAMB(m, 0x222D) = now & ~old;
        break;
    }
    case 16: case 22: adpcm_stop(&m->audio); c->a = 0; break;          /* ad_reset / ad_stop */
    case 23: c->a = m->audio.ad_on ? 1 : 0; c->x = c->a; break;        /* ad_stat */
    case 17: {                                                          /* ad_trans: CD -> ADPCM RAM */
        uint32_t lba = (ZP(m, CL) << 16) | (ZP(m, CH) << 8) | ZP(m, DL);
        uint16_t dst = ZP(m, DH) ? m->adpcm_wr : (ZP(m, BL) | (ZP(m, BH) << 8));
        unsigned n = ZP(m, AL) * 2048;
        for (unsigned i = 0; i < n && lba * 2048 + i < (unsigned long)m->disc_sectors * 2048; i++)
            m->adpcm[(dst + i) & 0xFFFF] = m->disc[lba * 2048 + i];
        m->adpcm_wr = dst + n;
        if (m->verbose > 1) fprintf(m->log, "  ad_trans lba=%06X -> adpcm %04X len=%u\n", lba, dst, n);
        c->a = 0; break;
    }
    case 18: case 19: {                                                 /* ad_read: ADPCM -> mem, ad_write: mem -> ADPCM */
        uint16_t ad = ZP(m, CL) | (ZP(m, CH) << 8);
        uint8_t mode = ZP(m, DH);
        uint16_t addr = ZP(m, BL) | (ZP(m, BH) << 8);
        unsigned n = ZP(m, AL) | (ZP(m, AH) << 8);
        if (!n) n = 0x10000;
        for (unsigned i = 0; i < n; i++) {
            uint16_t a = ad + i;
            if (mode == 0) {
                if (id == 18) { cpu_write(c, addr + i, m->adpcm[a]); pce_mark_dirty(m, cpu_phys(c, addr + i) >> 13); } else m->adpcm[a] = cpu_read(c, addr + i);
            } else if (mode >= 2 && mode <= 6) {
                uint32_t ph = ((uint32_t)(ZP(m, BL) + (i >> 13)) << 13) | (i & 0x1FFF);
                if (id == 18) { pce_write(m, ph, m->adpcm[a]); pce_mark_dirty(m, ph >> 13); } else m->adpcm[a] = pce_read(m, ph);
            } else {                                                    /* VRAM, word address in bx */
                uint16_t *w = &m->vdc.vram[(addr + i / 2) & 0x7FFF];
                if (id == 18) *w = (i & 1) ? ((*w & 0xFF) | (m->adpcm[a] << 8)) : ((*w & 0xFF00) | m->adpcm[a]);
                else m->adpcm[a] = (i & 1) ? (*w >> 8) : (*w & 0xFF);
            }
        }
        if (mode >= 2 && mode <= 6) c->mpr[mode] = ZP(m, BL);
        if (m->verbose > 1) fprintf(m->log, "  %s adpcm %04X %s mode=%02X addr=%04X len=%u\n", names[id], ad, id == 18 ? "->" : "<-", mode, addr, n);
        c->a = 0; break;
    }
    case 7: case 11: case 12: case 13: c->a = 0; break;                 /* cd_search, cd_dinfo, cd_contnts, cd_subrd */
    case 20: case 21:                                                   /* ad_play / ad_cplay */
        if (id == 20) adpcm_play(&m->audio, m->adpcm, ZP(m, BL) | ZP(m, BH) << 8, ZP(m, AL) | ZP(m, AH) << 8, ZP(m, DH));
        if (m->verbose) fprintf(m->log, "  %s addr=%04X len=%04X rate=%02X mode=%02X\n", names[id],
            ZP(m, BL) | ZP(m, BH) << 8, ZP(m, AL) | ZP(m, AH) << 8, ZP(m, DH), ZP(m, DL));
        c->a = 0; break;
    default:
        if (m->verbose && first) fprintf(m->log, "  (not implemented: %s)\n", names[id]);
        break;
    }
    m->bulk_load = 0;
    return 0;
}
