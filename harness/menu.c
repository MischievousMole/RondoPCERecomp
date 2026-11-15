/* Launcher + pause menu for the player. Draws into a 640x360 RGBA overlay (straight alpha). */
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include "menu.h"
#include "font_gen.h"

enum { P_LAUNCH, P_PAUSE, P_VIDEO, P_AUDIO, P_CONTROLS };
enum { I_ACTION, I_CHOICE, I_SLIDER, I_PAGE, I_BIND };

typedef struct item {
    const char *label; int type;
    int *ival; const char *const *names; int n;        /* choice */
    float *fval; float lo, hi, step; int pct;          /* slider */
    int page; int action; int bind;                    /* page link / action id / binding index */
} item_t;

static int open_, page, sel[5], waiting = -1, launcher;
static menu_ctx *C;

static const char *const filters[] = { "Pixel", "Sharp", "Scanlines", "CRT" };
static const char *const scales[] = { "Fill height", "Integer" };
static const char *const aspects[] = { "Original", "Square pixels", "Stretch 4:3" };
static const char *const syncs[] = { "Auto", "Locked to refresh", "VRR (original speed)" };
static const char *const motions[] = { "60 Hz", "120 Hz repeat", "120 Hz black frame" };
static const char *const onoff[] = { "Off", "On" };
static const char *const ssmins[] = { "Off", "1 min", "3 min", "5 min", "10 min" };
const char *const menu_bind_names[MENU_NBIND] = { "Up", "Down", "Left", "Right", "Jump", "Attack", "Run (start)", "Select" };

static int build(item_t *it) {
    int n = 0;
#define ACT(l, a)  it[n++] = (item_t){ .label = l, .type = I_ACTION, .action = a }
#define PG(l, p)   it[n++] = (item_t){ .label = l, .type = I_PAGE, .page = p }
#define CH(l, v, a) it[n++] = (item_t){ .label = l, .type = I_CHOICE, .ival = v, .names = a, .n = (int)(sizeof a / sizeof a[0]) }
#define SL(l, v, a, b, s, p) it[n++] = (item_t){ .label = l, .type = I_SLIDER, .fval = v, .lo = a, .hi = b, .step = s, .pct = p }
    switch (page) {
    case P_LAUNCH: ACT("Start game", MR_START); PG("Video", P_VIDEO); PG("Audio", P_AUDIO); PG("Controls", P_CONTROLS); ACT("Quit", MR_QUIT); break;
    case P_PAUSE: ACT("Resume", MR_RESUME); PG("Video", P_VIDEO); PG("Audio", P_AUDIO); PG("Controls", P_CONTROLS);
                  ACT("Reset game", MR_RESET); ACT("Quit to desktop", MR_QUIT); break;
    case P_VIDEO:
        CH("Filter", &C->v->filter, filters); CH("Scale", &C->v->integer, scales); CH("Aspect", &C->v->aspect, aspects);
        CH("Sync", C->sync_mode, syncs); CH("Motion", C->present_mode, motions);
        SL("Scanline strength", &C->v->scan_strength, 0, 1, 0.05f, 1); SL("CRT mask", &C->v->mask_strength, 0, 1, 0.05f, 1);
        SL("Sprite brightness", &C->v->bri[1], 0.5f, 1.5f, 0.05f, 1); SL("Sprite contrast", &C->v->con[1], 0.5f, 1.5f, 0.05f, 1);
        SL("BG brightness", &C->v->bri[0], 0.5f, 1.5f, 0.05f, 1); SL("BG contrast", &C->v->con[0], 0.5f, 1.5f, 0.05f, 1);
        CH("Fullscreen", C->fullscreen, onoff);
        if (C->ss_choice) CH("Screensaver", C->ss_choice, ssmins);
        ACT("Reset video", MR_RESET_VIDEO); ACT("Back", MR_BACK); break;
    case P_AUDIO:
        SL("Master", C->vol[0], 0, 1, 0.05f, 1); SL("Music (CD)", C->vol[1], 0, 1, 0.05f, 1);
        SL("Sound effects", C->vol[2], 0, 1, 0.05f, 1); SL("Voices", C->vol[3], 0, 1, 0.05f, 1); ACT("Back", MR_BACK); break;
    case P_CONTROLS:
        for (int i = 0; i < MENU_NBIND; i++) it[n++] = (item_t){ .label = menu_bind_names[i], .type = I_BIND, .bind = i };
        ACT("Reset controls", MR_RESET_CONTROLS); ACT("Back", MR_BACK); break;
    }
    return n;
}

void menu_open(menu_ctx *c, int is_launcher) { C = c; open_ = 1; launcher = is_launcher; page = is_launcher ? P_LAUNCH : P_PAUSE; sel[page] = 0; waiting = -1; }
int menu_is_open(void) { return open_; }
#define PREVIEW_ROWS 11   /* Filter .. BG contrast on the video page */
int menu_preview(void) { return open_ && page == P_VIDEO && sel[P_VIDEO] < PREVIEW_ROWS; }

static int back(void) {
    if (page == P_VIDEO || page == P_AUDIO || page == P_CONTROLS) { page = launcher ? P_LAUNCH : P_PAUSE; return MR_NONE; }
    if (page == P_PAUSE) { open_ = 0; return MR_RESUME; }
    return MR_NONE;
}

int menu_event(const SDL_Event *e, menu_ctx *c) {
    C = c;
    item_t it[24]; int n = build(it);
    if (waiting >= 0) {                                  /* capturing a binding */
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            if (e->key.keysym.scancode != SDL_SCANCODE_ESCAPE) C->keymap[waiting] = e->key.keysym.scancode;
            waiting = -1; return MR_CHANGED;
        }
        if (e->type == SDL_CONTROLLERBUTTONDOWN) { C->padmap[waiting] = e->cbutton.button; waiting = -1; return MR_CHANGED; }
        return MR_NONE;
    }
    int up = 0, down = 0, left = 0, right = 0, ok = 0, cancel = 0;
    if (e->type == SDL_KEYDOWN) {
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_UP: up = 1; break; case SDL_SCANCODE_DOWN: down = 1; break;
        case SDL_SCANCODE_LEFT: left = 1; break; case SDL_SCANCODE_RIGHT: right = 1; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_SPACE: case SDL_SCANCODE_KP_ENTER: if (!e->key.repeat) ok = 1; break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: if (!e->key.repeat) cancel = 1; break;
        default: break;
        }
    } else if (e->type == SDL_CONTROLLERBUTTONDOWN) {
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: up = 1; break; case SDL_CONTROLLER_BUTTON_DPAD_DOWN: down = 1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: left = 1; break; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: right = 1; break;
        case SDL_CONTROLLER_BUTTON_A: ok = 1; break; case SDL_CONTROLLER_BUTTON_B: cancel = 1; break;
        case SDL_CONTROLLER_BUTTON_START: if (page == P_PAUSE) { open_ = 0; return MR_RESUME; } break;
        }
    } else return MR_NONE;
    int *s = &sel[page];
    if (up) *s = (*s + n - 1) % n;
    if (down) *s = (*s + 1) % n;
    if (cancel) return back();
    item_t *t = &it[*s];
    if ((left || right) && (t->type == I_CHOICE || t->type == I_SLIDER)) {
        if (t->type == I_CHOICE) *t->ival = (*t->ival + (right ? 1 : t->n - 1)) % t->n;
        else { *t->fval += right ? t->step : -t->step; if (*t->fval < t->lo) *t->fval = t->lo; if (*t->fval > t->hi) *t->fval = t->hi; }
        return t->ival == C->sync_mode ? MR_SYNC : MR_CHANGED;
    }
    if (ok) {
        switch (t->type) {
        case I_PAGE: page = t->page; sel[page] = 0; return MR_NONE;
        case I_CHOICE: *t->ival = (*t->ival + 1) % t->n; return t->ival == C->sync_mode ? MR_SYNC : MR_CHANGED;
        case I_BIND: waiting = t->bind; return MR_NONE;
        case I_ACTION:
            if (t->action == MR_BACK) return back();
            if (t->action == MR_START || t->action == MR_RESUME || t->action == MR_RESET) open_ = 0;
            return t->action;
        }
    }
    return MR_NONE;
}

/* ---- drawing ---- */
static uint32_t *B; static int BW, BH, G = 1;   /* G = physical pixels per logical pixel */
static void fill(int x, int y, int w, int h, uint32_t argb) {
    x *= G; y *= G; w *= G; h *= G;
    for (int j = y < 0 ? 0 : y; j < y + h && j < BH; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < BW; i++) B[j * BW + i] = argb;
}
static void text(int x, int y, const char *s, uint32_t rgb, int scale) {
    int k = G * scale;
    for (; *s; s++, x += FONT_W * scale) {
        int c = (unsigned char)*s; if (c < 32 || c > 126) c = '?';
        const unsigned char *g = FONT[c - 32];
        for (int j = 0; j < FONT_H; j++)
            for (int i = 0; i < FONT_W; i++) {
                if (!g[j * FONT_W + i]) continue;
                for (int b = 0; b < k; b++)
                    for (int a = 0; a < k; a++) {
                        int px = x * G + i * k + a, py = y * G + j * k + b;
                        if (px >= 0 && py >= 0 && px < BW && py < BH) B[py * BW + px] = 0xFF000000u | rgb;
                    }
            }
    }
}
static void textr(int xr, int y, const char *s, uint32_t rgb) { text(xr - (int)strlen(s) * FONT_W, y, s, rgb, 1); }

static const char *keyname(int sc) { const char *n = SDL_GetScancodeName((SDL_Scancode)sc); return n && *n ? n : "-"; }
static const char *padname(int b) {
    static const char *const nm[] = { "A", "B", "X", "Y", "Back", "Guide", "Start", "L-stick", "R-stick", "LB", "RB", "D-up", "D-down", "D-left", "D-right" };
    return b >= 0 && b < 15 ? nm[b] : "-";
}

void menu_draw(uint32_t *buf, int pw_, int ph_) {
    B = buf; BW = pw_; BH = ph_; G = pw_ / 640; if (G < 1) G = 1;
    memset(buf, 0, (size_t)pw_ * ph_ * 4);
    int w = 640, h = 360;    /* logical layout */
    if (!open_) return;
    item_t it[24]; int n = build(it);
    const uint32_t WHITE = 0xF0F0F0, GREY = 0x9A9A9A, GOLD = 0xE8C060, RED = 0xC03030;
    if (menu_preview()) {
        /* live preview: game undimmed, one-line strip at the bottom */
        item_t *t = &it[sel[page]];
        char v[96];
        if (t->type == I_CHOICE) snprintf(v, sizeof v, "%s   < %s >", t->label, t->names[*t->ival]);
        else snprintf(v, sizeof v, "%s   < %d%% >", t->label, (int)(*t->fval * 100 + 0.5f));
        fill(0, h - 24, w, 24, 0xCC0C0C0C);
        text((w - (int)strlen(v) * FONT_W) / 2, h - 16, v, GOLD, 1);
        text(8, h - 16, "VIDEO", GREY, 1);
        textr(w - 8, h - 16, "up/down: more", 0x707070);
        return;
    }
    int rowh = page == P_VIDEO ? 14 : 16;
    int px = (w - 380) / 2, pw = 380;
    if (launcher) fill(0, 0, w, h, 0xFF000000); else fill(0, 0, w, h, 0x99000000);
    static const char *const titles[] = { "", "PAUSED", "VIDEO", "AUDIO", "CONTROLS" };
    int has_t = titles[page][0] != 0;
    int ph = n * rowh + (has_t ? 22 : 0) + 16;
    int top = launcher && page == P_LAUNCH ? 160 : (h - ph) / 2 + 8;
    if (top < 14) top = 14;
    if (launcher && page == P_LAUNCH) {
        text((w - 18 * FONT_W * 3) / 2, 60, "AKUMAJOU DRACULA X", 0xD8D0C0, 3);
        text((w - 12 * FONT_W) / 2, 100, "CHI NO RONDO", RED, 1);
    }
    fill(px - 10, top - 8, pw + 20, ph, 0xF5101010);
    if (has_t) text(px, top, titles[page], GOLD, 1);
    int y = top + (has_t ? 22 : 4);
    for (int i = 0; i < n; i++, y += rowh) {
        item_t *t = &it[i];
        int on = i == sel[page];
        if (on) fill(px - 6, y - 3, pw + 12, rowh - 2, 0xFF5A1A1A);
        text(px, y, t->label, on ? WHITE : GREY, 1);
        char v[64] = "";
        if (t->type == I_CHOICE) snprintf(v, sizeof v, "< %s >", t->names[*t->ival]);
        else if (t->type == I_SLIDER) snprintf(v, sizeof v, "< %d%% >", (int)(*t->fval * 100 + 0.5f));
        else if (t->type == I_BIND) {
            if (waiting == t->bind) snprintf(v, sizeof v, "press a key or button");
            else snprintf(v, sizeof v, "%s / %s", keyname(C->keymap[t->bind]), padname(C->padmap[t->bind]));
        }
        if (v[0]) textr(px + pw, y, v, on ? GOLD : GREY);
    }
    if (top + ph + 4 > h - 18) return;   /* no room for the hint line */
    const char *hint = page == P_CONTROLS ? "Enter/A: rebind   Esc/B: back" : "Arrows/D-pad: move, change   Enter/A: select   Esc/B: back";
    text((w - (int)strlen(hint) * FONT_W) / 2, h - 16, hint, 0x808080, 1);
}
