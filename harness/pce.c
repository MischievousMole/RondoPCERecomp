/* PC Engine CD machine: memory map, VDC, VCE, timer, joypad, PSG capture. */
#include <stdlib.h>
#include <string.h>
#include "pce.h"
#include "recomp_rt.h"

uint32_t g_ovl_epoch;
#ifndef NO_NATIVE_TABLE
#else
const native_ovl_t g_native_table[1];
const int g_native_count = 0;
#endif

static const int vram_inc[4] = { 1, 32, 64, 128 };

/* ---------------- VDC ---------------- */
static void vdc_irq_update(pce_t *m) {
    vdc_t *v = &m->vdc;
    uint16_t cr = v->reg[5], dcr = v->reg[0x0F];
    int on = ((v->status & 0x20) && (cr & 0x08)) ||   /* vblank */
             ((v->status & 0x04) && (cr & 0x04)) ||   /* raster */
             ((v->status & 0x08) && (dcr & 0x01)) ||  /* SATB DMA end */
             ((v->status & 0x10) && (dcr & 0x02)) ||  /* VRAM DMA end */
             ((v->status & 0x01) && (cr & 0x01)) ||
             ((v->status & 0x02) && (cr & 0x02));
    if (on) m->cpu.irq_pending |= IRQ_IRQ1; else m->cpu.irq_pending &= ~IRQ_IRQ1;
}

static void vdc_dma(pce_t *m) {
    vdc_t *v = &m->vdc;
    uint16_t src = v->reg[0x10], dst = v->reg[0x11];
    unsigned len = v->reg[0x12] + 1;
    int sd = (v->reg[0x0F] & 4) ? -1 : 1, dd = (v->reg[0x0F] & 8) ? -1 : 1;
    for (unsigned i = 0; i < len; i++) {
        v->vram[dst & 0x7FFF] = v->vram[src & 0x7FFF];
        src += sd; dst += dd;
    }
    v->reg[0x10] = src; v->reg[0x11] = dst;
    if (v->reg[0x0F] & 0x02) v->status |= 0x10;
    vdc_irq_update(m);
}

void vdc_write(pce_t *m, int port, uint8_t val) {
    vdc_t *v = &m->vdc;
    if (port == 0) { v->ar = val & 0x1F; return; }
    if (port == 1) return;
    uint16_t *r = &v->reg[v->ar];
    if (port == 2) *r = (*r & 0xFF00) | val;
    else           *r = (*r & 0x00FF) | (val << 8);
    switch (v->ar) {
    case 0x01: if (port == 3) { v->read_latch = v->vram[v->reg[1] & 0x7FFF]; } break;
    case 0x02:
        if (port == 3) {
            if (v->reg[0] < 0x8000) v->vram[v->reg[0]] = v->reg[2];
            v->reg[0] += vram_inc[(v->reg[5] >> 11) & 3];
        }
        break;
    case 0x05: vdc_irq_update(m); break;
    case 0x08: v->bg_y = v->reg[8]; break;
    case 0x12: if (port == 3) vdc_dma(m); break;
    case 0x13: if (port == 3) v->satb_pending = 1; break;
    }
}

uint8_t vdc_read(pce_t *m, int port) {
    vdc_t *v = &m->vdc;
    if (port == 0) {
        uint8_t s = v->status;
        v->status = 0;
        vdc_irq_update(m);
        return s;
    }
    if (port == 2) return v->ar == 2 ? (v->read_latch & 0xFF) : 0;
    if (port == 3) {
        if (v->ar != 2) return 0;
        uint8_t r = v->read_latch >> 8;
        v->reg[1] += vram_inc[(v->reg[5] >> 11) & 3];
        v->read_latch = v->vram[v->reg[1] & 0x7FFF];
        return r;
    }
    return 0;
}

/* ---------------- memory map ---------------- */
uint8_t pce_read(void *ctx, uint32_t a) {
    pce_t *m = ctx;
    uint8_t bank = a >> 13; uint16_t off = a & 0x1FFF;
    if (bank < 0x20) return m->rom[a];
    if (bank >= 0x68 && bank < 0x80) return m->scdram[a - 0x68 * 0x2000];
    if (bank >= 0x80 && bank < 0x88) return m->cdram[a - 0x80 * 0x2000];
    if (bank >= 0xF8 && bank < 0xFC) return m->ram[off];
    if (bank == 0xFF) {
        switch (off & 0x1C00) {
        case 0x0000: return vdc_read(m, off & 3);
        case 0x0400:
            switch (off & 7) {
            case 4: return m->cram[m->vce_addr & 0x1FF] & 0xFF;
            case 5: { uint8_t r = 0xFE | (m->cram[m->vce_addr & 0x1FF] >> 8); m->vce_addr++; return r; }
            }
            return 0xFF;
        case 0x0C00: return m->tm_count & 0x7F;
        case 0x1000: {
            /* bit 7 = 0: CD unit attached; bit 6 = 1: Japanese machine */
            uint8_t r = 0x40;
            if (m->joy_clr) r |= 0;
            else if (m->joy_sel) r |= (~m->joy_buttons >> 4) & 0x0F;
            else r |= (~m->joy_buttons) & 0x0F;
            return r | 0x30;
        }
        case 0x1400:
            if ((off & 3) == 2) return m->cpu.irq_mask;
            if ((off & 3) == 3) return m->cpu.irq_pending;
            return 0;
        case 0x1800:
            if (off == 0x18C1) return 0xAA;   /* Super System Card signature */
            if (off == 0x18C2) return 0x55;
            if (off == 0x18C3) return 0x03;
            if ((off & 0xFF) < 16) {
                if (m->verbose > 1) fprintf(m->log, "CD rd %04X\n", off);
                return m->cd_regs[off & 15];
            }
            return 0xFF;
        }
        return 0xFF;
    }
    return 0xFF;
}

void pce_write(void *ctx, uint32_t a, uint8_t v) {
    pce_t *m = ctx;
    uint8_t bank = a >> 13; uint16_t off = a & 0x1FFF;
    if (bank < 0x20) { if (m->verbose) fprintf(m->log, "ROM write %06X=%02X pc=%04X\n", a, v, m->cpu.pc); return; }
    if (!m->bulk_load && m->cur_ovl[bank] >= 0 && !m->bank_dirty[bank] && !m->volatile_map[bank][off]) {
        int o = m->cur_ovl[bank];
        int code = (m->ovl[o].bits[off >> 3] >> (off & 7) & 1) ||
                   (m->native_cm[bank] && (m->native_cm[bank][off >> 3] >> (off & 7) & 1));
        if (code) {
            /* CPU wrote over executed code: overlay copy or self-modification -> re-identify the bank */
            m->smc_writes++;
            if (m->verbose && m->smc_writes < 50)
                fprintf(m->log, "code write %06X=%02X (ovl %02X_%08X) pc=%04X frame %ld\n", a, v, m->ovl[o].bank, m->ovl[o].hash, m->cpu.pc, m->frame);
            if (m->outdir) {
                char p[512]; snprintf(p, sizeof p, "%s/ovl/codewrites.txt", m->outdir);
                FILE *f = fopen(p, "a");
                if (f) { fprintf(f, "%02X_%08X %04X %04X\n", m->ovl[o].bank, m->ovl[o].hash, off, m->cpu.pc); fclose(f); }
            }
            pce_mark_dirty(m, bank);
        }
    }
    if (bank >= 0x68 && bank < 0x80) { m->scdram[a - 0x68 * 0x2000] = v; return; }
    if (bank >= 0x80 && bank < 0x88) { m->cdram[a - 0x80 * 0x2000] = v; return; }
    if (bank >= 0xF8 && bank < 0xFC) { m->ram[off] = v; return; }
    if (bank == 0xFF) {
        switch (off & 0x1C00) {
        case 0x0000: vdc_write(m, off & 3, v); return;
        case 0x0400:
            switch (off & 7) {
            case 0: m->vce_ctrl = v; break;
            case 2: m->vce_addr = (m->vce_addr & 0x100) | v; break;
            case 3: m->vce_addr = (m->vce_addr & 0xFF) | ((v & 1) << 8); break;
            case 4: m->cram[m->vce_addr & 0x1FF] = (m->cram[m->vce_addr & 0x1FF] & 0x100) | v; break;
            case 5: m->cram[m->vce_addr & 0x1FF] = (m->cram[m->vce_addr & 0x1FF] & 0xFF) | ((v & 1) << 8); m->vce_addr++; break;
            }
            return;
        case 0x0800: {
            int r = off & 0x0F;
            if (m->psg_log) fprintf(m->psg_log, "%ld %d %X %02X\n", m->frame, m->line, r, v);
            if (r == 0) m->psg_ch = v & 7;
            else if (r == 1) m->psg_global[0] = v;
            else if (r == 8 || r == 9) m->psg_global[r - 7] = v;
            else if (m->psg_ch < 6) m->psg_reg[m->psg_ch][r] = v;
            return;
        }
        case 0x0C00:
            if ((off & 1) == 0) m->tm_reload = v & 0x7F;
            else {
                if ((v & 1) && !m->tm_on) { m->tm_count = m->tm_reload; m->tm_frac = 0; }
                m->tm_on = v & 1;
            }
            return;
        case 0x1000:
            m->joy_sel = v & 1; m->joy_clr = (v >> 1) & 1;
            return;
        case 0x1400:
            if ((off & 3) == 2) m->cpu.irq_mask = v & 7;
            if ((off & 3) == 3) m->cpu.irq_pending &= ~IRQ_TIMER;
            return;
        case 0x1800:
            if (m->verbose > 1) fprintf(m->log, "CD wr %04X=%02X\n", off, v);
            if ((off & 0xFF) < 16) m->cd_regs[off & 15] = v;
            return;
        }
        return;
    }
}

/* ---------------- frame loop ---------------- */
static void timer_tick(pce_t *m, int cyc) {
    if (!m->tm_on) return;
    /* timer clock = 7.16 MHz / 1024; CPU fast cycle = 7.16 MHz */
    m->tm_frac += cyc;
    while (m->tm_frac >= 1024) {
        m->tm_frac -= 1024;
        if (m->tm_count == 0) { m->tm_count = m->tm_reload; m->cpu.irq_pending |= IRQ_TIMER; }
        else m->tm_count--;
    }
}

static uint32_t fnv(const uint8_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

void pce_mark_dirty(pce_t *m, uint8_t bank) { m->bank_dirty[bank] = 1; m->native_cm[bank] = NULL; g_ovl_epoch++; }

static int ovl_lookup(pce_t *m, uint8_t bank) {
    uint8_t buf[0x2000];
    for (int i = 0; i < 0x2000; i++) buf[i] = pce_read(m, ((uint32_t)bank << 13) | i);
    uint8_t hb[0x2000];
    for (int i = 0; i < 0x2000; i++) hb[i] = m->volatile_map[bank][i] ? 0 : buf[i];
    uint32_t h = fnv(hb, 0x2000) ^ bank;
    for (int i = 0; i < m->novl; i++) if (m->ovl[i].hash == h && m->ovl[i].bank == bank) return i;
    if (m->novl == m->ovl_cap) { m->ovl_cap = m->ovl_cap ? m->ovl_cap * 2 : 64; m->ovl = realloc(m->ovl, m->ovl_cap * sizeof *m->ovl); }
    struct ovl *o = &m->ovl[m->novl];
    o->hash = h; o->bank = bank; memset(o->bits, 0, sizeof o->bits);
    if (m->outdir) {
        char p[512]; snprintf(p, sizeof p, "%s/ovl/bank_%02X_%08X.bin", m->outdir, bank, h);
        FILE *f = fopen(p, "wb"); if (f) { fwrite(buf, 1, 0x2000, f); fclose(f); }
    }
    return m->novl++;
}

void pce_save_coverage(pce_t *m) {
    if (!m->outdir) return;
    char p[512];
    for (int i = 0; i < m->novl; i++) {
        snprintf(p, sizeof p, "%s/ovl/cov_%02X_%08X.bin", m->outdir, m->ovl[i].bank, m->ovl[i].hash);
        FILE *f = fopen(p, "wb"); if (f) { fwrite(m->ovl[i].bits, 1, 1024, f); fclose(f); }
    }
}

static void run_cycles(pce_t *m, int budget) {
    cpu6280 *c = &m->cpu;
    while (budget > 0 && !m->stop) {
        uint32_t ph = cpu_phys(c, c->pc);
        if (m->cov) m->cov[ph >> 3] |= 1 << (ph & 7);
        {
            uint8_t bank = ph >> 13;
            if (m->bank_dirty[bank] || m->cur_ovl[bank] < 0) { m->cur_ovl[bank] = ovl_lookup(m, bank); m->bank_dirty[bank] = 0; }
            uint16_t off = ph & 0x1FFF;
            m->ovl[m->cur_ovl[bank]].bits[off >> 3] |= 1 << (off & 7);
        }
        if (m->native_on && !m->trace_left) {
            int oi = m->cur_ovl[ph >> 13];
            int ni = m->ovl_native[oi];
            if (ni == -2) {
                ni = -1;
                for (int k = 0; k < g_native_count; k++)
                    if (g_native_table[k].bank == m->ovl[oi].bank && g_native_table[k].hash == m->ovl[oi].hash) { ni = k; break; }
                m->ovl_native[oi] = ni;
            }
            if (ni >= 0 && !m->ovl_native_off[oi]) {
                m->native_cm[ph >> 13] = g_native_table[ni].codemap;
                m->native_cm_ovl[ph >> 13] = oi;
                static pce_t snap, after;
                m->native_calls++;
                int check = m->difftest_every && (m->native_calls % m->difftest_every) == 0;
                if (check) { snap = *m; snap.psg_log = NULL; }
                uint32_t n0 = c->ninstr; uint16_t pc0 = c->pc;
                FILE *pl = m->psg_log; if (check) m->psg_log = NULL;
                int cyc = g_native_table[ni].fn(c, budget);
                m->psg_log = pl;
                uint32_t k = c->ninstr - n0;
                if (k == 0 && c->pc == pc0) goto interp;   /* no progress: let the interpreter step */
                if (check) {
                    after = *m;
                    *m = snap; m->psg_log = pl;
                    for (uint32_t i = 0; i < k; i++) {
                        /* interpreter replay without taking interrupts the native code would not take */
                        uint8_t pend = c->irq_pending; c->irq_pending = 0;
                        cpu_step(c);
                        c->irq_pending |= pend;
                    }
                    m->diff_checks++;
                    int bad = 0; char what[64] = "";
                    if (c->a != after.cpu.a || c->x != after.cpu.x || c->y != after.cpu.y || c->s != after.cpu.s ||
                        (c->p & ~F_T) != (after.cpu.p & ~F_T) || c->pc != after.cpu.pc || memcmp(c->mpr, after.cpu.mpr, 8)) { bad = 1; strcpy(what, "regs"); }
                    else if (memcmp(m->ram, after.ram, sizeof m->ram)) { bad = 1; strcpy(what, "ram"); }
                    else if (memcmp(m->cdram, after.cdram, sizeof m->cdram)) { bad = 1; strcpy(what, "cdram"); }
                    else if (memcmp(m->scdram, after.scdram, sizeof m->scdram)) { bad = 1; strcpy(what, "scdram"); }
                    else if (memcmp(m->vdc.vram, after.vdc.vram, sizeof m->vdc.vram) || memcmp(m->vdc.reg, after.vdc.reg, sizeof m->vdc.reg)) { bad = 1; strcpy(what, "vdc"); }
                    else if (memcmp(m->cram, after.cram, sizeof m->cram)) { bad = 1; strcpy(what, "cram"); }
                    if (bad) {
                        m->diff_fails++;
                        if (m->diff_fails <= 20)
                            fprintf(m->log, "DIFF %s: start %02X:%04X n=%u native pc=%04X A=%02X X=%02X Y=%02X P=%02X S=%02X | interp pc=%04X A=%02X X=%02X Y=%02X P=%02X S=%02X\n",
                                what, snap.cpu.mpr[pc0 >> 13], pc0, k, after.cpu.pc, after.cpu.a, after.cpu.x, after.cpu.y, after.cpu.p, after.cpu.s,
                                c->pc, c->a, c->x, c->y, c->p, c->s);
                    }
                    /* keep the interpreter's result */
                }
                m->native_instr += k;
                int fast = c->speed ? cyc : cyc * 4;
                budget -= fast;
                timer_tick(m, fast);
                continue;
            }
        }
    interp:
        if (m->trace_left > 0) {
            m->trace_left--;
            fprintf(m->log, "T %02X:%04X %02X %02X %02X  A=%02X X=%02X Y=%02X S=%02X P=%02X\n", c->mpr[c->pc >> 13], c->pc,
                    cpu_read(c, c->pc), cpu_read(c, c->pc + 1), cpu_read(c, c->pc + 2), c->a, c->x, c->y, c->s, c->p);
        }
        uint32_t n0i = c->ninstr;
        int cyc = cpu_step(c);
        m->interp_instr += c->ninstr - n0i;
        if (cyc < 0) { m->stop = 1; break; }
        int fast = c->speed ? cyc : cyc * 4;
        budget -= fast;
        timer_tick(m, fast);
    }
}

void pce_run_frame(pce_t *m) {
    vdc_t *v = &m->vdc;
    /* display window from VPR/VDW */
    int vsw = v->reg[0x0C] & 0x1F, vds = v->reg[0x0C] >> 8, vdw = (v->reg[0x0D] & 0x1FF) + 1;
    if (!v->reg[0x0D]) vdw = 224;
    v->disp_start = vsw + vds + 2;
    if (v->disp_start < 3) v->disp_start = 14;
    v->disp_end = v->disp_start + vdw;
    g_fb_h = vdw > 242 ? 242 : vdw;
    if (v->disp_end > 261) v->disp_end = 261;
    for (m->line = 0; m->line < LINES_PER_FRAME && !m->stop; m->line++) {
        if (m->line == v->disp_start) { v->line_ctr = 64; v->bg_y = v->reg[8]; }
        /* raster compare */
        if (m->line >= v->disp_start && m->line < v->disp_end) {
            if ((v->reg[6] & 0x3FF) == v->line_ctr && (v->reg[5] & 0x04)) { v->status |= 0x04; vdc_irq_update(m); }
        }
        if (m->line >= v->disp_start && m->line < v->disp_end)
            render_line(m, m->line - v->disp_start, v->bg_y);
        run_cycles(m, CPU_CYC_PER_LINE);
        v->line_ctr++;
        v->bg_y++;
        if (m->line == v->disp_end) {
            if (v->reg[5] & 0x08) v->status |= 0x20;
            if (v->satb_pending || (v->reg[0x0F] & 0x10)) {
                uint16_t s = v->reg[0x13];
                for (int i = 0; i < 256; i++) v->satb[i] = v->vram[(s + i) & 0x7FFF];
                v->satb_pending = 0;
                if (v->reg[0x0F] & 0x01) v->status |= 0x08;
            }
            vdc_irq_update(m);
        }
    }
    m->frame++;
}

void pce_load_volatile(pce_t *m, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    unsigned b, o, n = 1;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        n = 1;
        if (sscanf(line, "%x:%x+%u", &b, &o, &n) >= 2 && b < 256)
            for (unsigned i = 0; i < n && o + i < 0x2000; i++) m->volatile_map[b][o + i] = 1;
    }
    fclose(f);
}

int pce_init(pce_t *m, const char *disc_iso, const char *bios_rom) {
    memset(m, 0, sizeof *m);
    m->log = stderr;
    FILE *f = fopen(disc_iso, "rb");
    if (!f) { perror(disc_iso); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    m->disc = malloc(n); if (fread(m->disc, 1, n, f) != (size_t)n) { fclose(f); return -1; }
    fclose(f);
    m->disc_sectors = n / 2048;
    m->cov = calloc(1, 0x200000 / 8);
    m->volatile_map = calloc(256, 0x2000);
    for (int i = 0; i < 256; i++) { m->cur_ovl[i] = -1; m->bank_dirty[i] = 1; }
    for (int i = 0; i < 4096; i++) m->ovl_native[i] = -2;
    m->cpu.read = pce_read; m->cpu.write = pce_write; m->cpu.ctx = m;
    m->cpu.trap = bios_hle_trap;
    if (bios_rom) {
        f = fopen(bios_rom, "rb");
        if (!f) { perror(bios_rom); return -1; }
        size_t r = fread(m->rom, 1, sizeof m->rom, f); fclose(f);
        if (r == 0x40000 + 512 || (r & 0x3FF) == 512) memmove(m->rom, m->rom + 512, r - 512);
        m->real_bios = 1;
    } else {
        bios_hle_install(m);
    }
    m->cpu.mpr[7] = 0;
    return 0;
}
