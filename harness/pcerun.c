/* pcerun DISC.iso OUTDIR FRAMES [options]
 *   -b bios.rom      use a real System Card image instead of HLE
 *   -d from:to:step  dump frames as PPM
 *   -i "f:btn,..."   scripted input: frame:hexmask (hold until next entry)
 *   -v / -vv         log BIOS calls / CD register access
 * Writes OUTDIR/cov.bin (executed-opcode bitmap over the 2 MB physical space). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pce.h"

static pce_t M;

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: pcerun DISC.iso OUTDIR FRAMES [-b bios] [-d a:b:s] [-i script] [-v]\n"); return 1; }
    const char *disc = argv[1], *out = argv[2]; long frames = atol(argv[3]);
    unsigned rseed = 0; long rstart = -1; uint32_t rs = 0; uint8_t rmask = 0; int rhold = 0;
    const char *bios = NULL, *script = NULL, *tracks = NULL; long d0 = -1, d1 = -1, ds = 1; int verbose = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) bios = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) sscanf(argv[++i], "%ld:%ld:%ld", &d0, &d1, &ds);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) sscanf(argv[++i], "%u:%ld", &rseed, &rstart);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) tracks = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-vv")) verbose = 2;
    }
    if (pce_init(&M, disc, bios)) return 1;
    M.verbose = verbose;
    M.outdir = out;
    { extern const char *bram_dir; static char bd[512]; snprintf(bd, sizeof bd, "%s/bram", out); bram_dir = bd; }
    if (getenv("PCE_NATIVE")) M.native_on = 1;
    if (tracks && pce_load_toc(&M, tracks) < 0) fprintf(stderr, "no toc in %s\n", tracks);
    pce_load_volatile(&M, getenv("PCE_VOLATILE") ? getenv("PCE_VOLATILE") : "tools/volatile.txt");
    if (getenv("PCE_DIFFTEST")) M.difftest_every = atoi(getenv("PCE_DIFFTEST"));
    { char cmd[600]; snprintf(cmd, sizeof cmd, "mkdir -p '%s/ovl'", out); if (system(cmd)) {} }
    if (getenv("PCE_TRACE")) M.trace_left = atol(getenv("PCE_TRACE"));
    char path[512];
    snprintf(path, sizeof path, "%s/psg.log", out);
    M.psg_log = fopen(path, "w");
    if (getenv("PCE_WAV")) { M.wav = fopen(getenv("PCE_WAV"), "wb"); char hdr[44] = {0}; fwrite(hdr, 1, 44, M.wav); }
    cpu_reset(&M.cpu);
    for (long f = 0; f < frames && !M.stop; f++) {
        if (script) {
            /* entries "frame:mask" in ascending order */
            const char *p = script; uint8_t mask = 0;
            while (*p) {
                long fr; unsigned mk; int n;
                if (sscanf(p, "%ld:%x%n", &fr, &mk, &n) != 2) break;
                if (fr <= f) mask = mk; else break;
                p += n; if (*p == ',') p++;
            }
            M.joy_buttons = mask;
        }
        if (rstart >= 0 && f >= rstart) {
            /* random play: hold a random combination for 4-40 frames; mostly right/left + attack/jump */
            if (!rs) rs = rseed * 2654435761u + 1;
            if (rhold-- <= 0) {
                rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
                static const uint8_t dirs[] = { 0x20, 0x20, 0x20, 0x80, 0x00, 0x40, 0x10, 0x60, 0xA0 };
                rmask = dirs[rs % 9] | ((rs >> 8) & 3);
                if (((rs >> 12) & 31) == 0) rmask |= 0x08;   /* occasional RUN (pause / menus) */
                rhold = 4 + (rs >> 20) % 36;
            }
            /* tap buttons: release every other frame so presses register */
            M.joy_buttons = (f & 1) ? (rmask & 0xF0) : rmask;
        }
        if (getenv("PCE_TRACE_AT") && f == atol(getenv("PCE_TRACE_AT"))) M.trace_left = getenv("PCE_TRACE_N") ? atol(getenv("PCE_TRACE_N")) : 200000;
        if (getenv("PCE_POKE")) {
            /* "from-to:addr=val,..." logical RAM addresses ($2000-$3FFF) */
            const char *p = getenv("PCE_POKE");
            long a0, a1; unsigned ad, val; int n;
            while (sscanf(p, "%ld-%ld:%x=%x%n", &a0, &a1, &ad, &val, &n) == 4) {
                if (f >= a0 && f <= a1 && ad >= 0x2000 && ad < 0x4000) M.ram[ad - 0x2000] = val;
                p += n; if (*p == ',') p++; else break;
            }
        }
        pce_run_frame(&M);
        if (getenv("PCE_RAMLOG") && f % atol(getenv("PCE_RAMLOG")) == 0) {
            static FILE *rl; if (!rl) { snprintf(path, sizeof path, "%s/ramlog.bin", out); rl = fopen(path, "wb"); }
            fwrite(&f, sizeof f, 1, rl); fwrite(M.ram, 1, 0x2000, rl);
        }
        if (getenv("PCE_DUMP") && f == atol(getenv("PCE_DUMP"))) {
            snprintf(path, sizeof path, "%s/mem_%06ld.bin", out, f);
            FILE *fd = fopen(path, "wb");
            for (uint32_t a = 0; a < 0x200000; a++) fputc(pce_read(&M, a < 0x1FE000 ? a : 0), fd);
            fclose(fd);
            snprintf(path, sizeof path, "%s/vram_%06ld.bin", out, f);
            fd = fopen(path, "wb"); fwrite(M.vdc.vram, 2, 0x8000, fd); fclose(fd);
            snprintf(path, sizeof path, "%s/adpcm_%06ld.bin", out, f);
            fd = fopen(path, "wb"); fwrite(M.adpcm, 1, 0x10000, fd); fclose(fd);
        }
        if (d0 >= 0 && f >= d0 && f <= d1 && (f - d0) % ds == 0) {
            snprintf(path, sizeof path, "%s/frame_%06ld.ppm", out, f);
            render_save_ppm(path, g_fb_h);
        }
    }
    pce_save_coverage(&M);
    snprintf(path, sizeof path, "%s/cov.bin", out);
    FILE *fc = fopen(path, "wb"); fwrite(M.cov, 1, 0x200000 / 8, fc); fclose(fc);
    long n = 0; for (int i = 0; i < 0x200000 / 8; i++) n += __builtin_popcount(M.cov[i]);
    fprintf(stderr, "frames=%ld pc=%04X mpr=%02X %02X %02X %02X %02X %02X %02X %02X executed=%ld halted=%d irqmask=%X pend=%X P=%02X\n",
            M.frame, M.cpu.pc, M.cpu.mpr[0], M.cpu.mpr[1], M.cpu.mpr[2], M.cpu.mpr[3], M.cpu.mpr[4], M.cpu.mpr[5],
            M.cpu.mpr[6], M.cpu.mpr[7], n, M.cpu.halted, M.cpu.irq_mask, M.cpu.irq_pending, M.cpu.p);
    fprintf(stderr, "overlays=%d native: calls=%ld instr=%ld interp instr=%ld (%.1f%% native) difftest %ld checks %ld fails smc=%ld\n",
            M.novl, M.native_calls, M.native_instr, M.interp_instr,
            100.0 * M.native_instr / (double)(M.native_instr + M.interp_instr + 1), M.diff_checks, M.diff_fails, M.smc_writes);
    fprintf(stderr, "VDC:");
    for (int i = 0; i < 20; i++) fprintf(stderr, " %02X=%04X", i, M.vdc.reg[i]);
    fprintf(stderr, " st=%02X irq_m=%02X\n", M.vdc.status, M.ram[0xF5]);
    if (M.psg_log) fclose(M.psg_log);
    if (M.wav) {
        uint32_t data = M.wav_samples * 4;
        uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
                          0x80,0xBB,0,0, 0x00,0xEE,0x02,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
        uint32_t riff = data + 36;
        memcpy(h + 4, &riff, 4); memcpy(h + 40, &data, 4);
        fseek(M.wav, 0, SEEK_SET); fwrite(h, 1, 44, M.wav); fclose(M.wav);
    }
    return 0;
}
