/* Disc image reader for the player: CUE + single BIN/IMG (raw 2352-byte sectors).
 * Provides the first data track as 2048-byte sectors and CD-DA samples by absolute frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pce.h"
#include "disc.h"

static FILE *g_img;
static long g_img_frames;

static void dirname_of(const char *p, char *out, size_t n) {
    snprintf(out, n, "%s", p);
    char *s = strrchr(out, '/'), *b = strrchr(out, '\\');
    if (b > s) s = b;
    if (s) s[1] = 0; else out[0] = 0;
}

/* CD-DA fetch: loads each audio track into memory on first use */
static int disc_fetch(void *ctx, long sample, int16_t *lr) {
    pce_t *m = ctx;
    long frame = sample / 588;
    for (int i = 0; i < m->ntoc; i++) {
        struct toc_ent *t = &m->toc[i];
        if (frame < t->start || frame >= t->start + t->frames) continue;
        if (!t->audio) return 0;
        if (!t->data) {
            t->data = calloc(t->frames, 2352);
            int ok = fseek(g_img, t->start * 2352, SEEK_SET) == 0;
            size_t got = ok ? fread(t->data, 2352, t->frames, g_img) : 0;
            fprintf(m->log, "cd-da: load track %d (%ld frames from %ld): seek %s, read %zu frames\n", t->track, t->frames, t->start, ok ? "ok" : "FAILED", got);
        }
        long off = (sample - t->start * 588) * 4;
        lr[0] = (int16_t)(t->data[off] | t->data[off + 1] << 8);
        lr[1] = (int16_t)(t->data[off + 2] | t->data[off + 3] << 8);
        return 1;
    }
    return 0;
}

int disc_open(pce_t *m, const char *cue_path, char *err, size_t errn) {
    FILE *f = fopen(cue_path, "r");
    if (!f) { snprintf(err, errn, "cannot open %s", cue_path); return -1; }
    char dir[1024], line[1024], bin[1024] = "";
    dirname_of(cue_path, dir, sizeof dir);
    int cur = -1;
    m->ntoc = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "FILE", 4)) {
            char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2 && !bin[0]) { *q2 = 0; snprintf(bin, sizeof bin, "%s%s", dir, q1 + 1); }
        } else if (!strncmp(p, "TRACK", 5) && m->ntoc < 100) {
            int n; char type[32];
            if (sscanf(p, "TRACK %d %31s", &n, type) == 2) {
                cur = m->ntoc++;
                m->toc[cur].track = n; m->toc[cur].audio = !strcmp(type, "AUDIO");
                m->toc[cur].start = -1; m->toc[cur].data = NULL;
            }
        } else if (!strncmp(p, "INDEX 01", 8) && cur >= 0) {
            int mm, ss, ff;
            if (sscanf(p, "INDEX 01 %d:%d:%d", &mm, &ss, &ff) == 3) m->toc[cur].start = (mm * 60 + ss) * 75 + ff;
        }
    }
    fclose(f);
    if (!bin[0] || !m->ntoc) { snprintf(err, errn, "%s: no FILE/TRACK entries", cue_path); return -1; }
    g_img = fopen(bin, "rb");
    if (!g_img) { snprintf(err, errn, "cannot open %s", bin); return -1; }
    fseek(g_img, 0, SEEK_END); g_img_frames = ftell(g_img) / 2352;
    for (int i = 0; i < m->ntoc; i++)
        m->toc[i].frames = (i + 1 < m->ntoc ? m->toc[i + 1].start : g_img_frames) - m->toc[i].start;
    /* first data track -> 2048-byte sectors */
    for (int i = 0; i < m->ntoc; i++) {
        if (m->toc[i].audio) continue;
        long n = m->toc[i].frames;
        m->disc = malloc(n * 2048);
        uint8_t raw[2352];
        fseek(g_img, m->toc[i].start * 2352, SEEK_SET);
        for (long k = 0; k < n; k++) {
            if (fread(raw, 1, 2352, g_img) != 2352) { n = k; break; }
            memcpy(m->disc + k * 2048, raw + 16, 2048);
        }
        m->disc_sectors = n;
        break;
    }
    if (!m->disc) { snprintf(err, errn, "no data track"); return -1; }
    m->audio.cd_fetch = disc_fetch; m->audio.cd_ctx = m;
    return 0;
}
