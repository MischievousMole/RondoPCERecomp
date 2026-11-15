/* Rondo recomp player: SDL2 + OpenGL 3.3.
 * - finds the game's .cue (next to the exe or in a subfolder) and syscard3.pce
 * - recompiled code + interpreter fallback, audio-paced at 59.83 Hz
 * - GPU shader output (sharp / pixel / scanlines / CRT), original aspect with side bars
 * - 60 Hz, 120 Hz repeat, 120 Hz black frame insertion; VRR-friendly (vsync on)
 * Keys: arrows, X = I (jump), Z = II (attack), Enter = RUN, RShift = SELECT,
 *       F2 filter, F3 refresh mode, F4 integer scale, F5/F6 sprite brightness, F7/F8 BG brightness,
 *       F11 fullscreen, Esc quit. Gamepad: A = I, X/B = II, Start = RUN, Back = SELECT. */
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pce.h"
#include "video.h"
#include "shaders_gen.h"
#include "menu.h"
#ifdef SANTABLE
#include "santable_gen.h"   /* SANTABLE_FRAG + WORDMARK (generated from extras/santable) */
#endif

static pce_t PM;
static video_cfg V;
static char base[1024], ini_path[1100];

/* --- GL entry points (core 3.3) --- */
#define GLF(t, n) static t n;
GLF(PFNGLCREATESHADERPROC, pglCreateShader) GLF(PFNGLSHADERSOURCEPROC, pglShaderSource) GLF(PFNGLCOMPILESHADERPROC, pglCompileShader)
GLF(PFNGLGETSHADERIVPROC, pglGetShaderiv) GLF(PFNGLGETSHADERINFOLOGPROC, pglGetShaderInfoLog) GLF(PFNGLCREATEPROGRAMPROC, pglCreateProgram)
GLF(PFNGLATTACHSHADERPROC, pglAttachShader) GLF(PFNGLLINKPROGRAMPROC, pglLinkProgram) GLF(PFNGLUSEPROGRAMPROC, pglUseProgram)
GLF(PFNGLGETUNIFORMLOCATIONPROC, pglGetUniformLocation) GLF(PFNGLUNIFORM1IPROC, pglUniform1i) GLF(PFNGLUNIFORM1FPROC, pglUniform1f)
GLF(PFNGLUNIFORM2FPROC, pglUniform2f) GLF(PFNGLGENVERTEXARRAYSPROC, pglGenVertexArrays) GLF(PFNGLBINDVERTEXARRAYPROC, pglBindVertexArray)
GLF(PFNGLACTIVETEXTUREPROC, pglActiveTexture)
static void load_gl(void) {
#define L(n, s) n = SDL_GL_GetProcAddress(s)
    L(pglCreateShader, "glCreateShader"); L(pglShaderSource, "glShaderSource"); L(pglCompileShader, "glCompileShader");
    L(pglGetShaderiv, "glGetShaderiv"); L(pglGetShaderInfoLog, "glGetShaderInfoLog"); L(pglCreateProgram, "glCreateProgram");
    L(pglAttachShader, "glAttachShader"); L(pglLinkProgram, "glLinkProgram"); L(pglUseProgram, "glUseProgram");
    L(pglGetUniformLocation, "glGetUniformLocation"); L(pglUniform1i, "glUniform1i"); L(pglUniform1f, "glUniform1f");
    L(pglUniform2f, "glUniform2f"); L(pglGenVertexArrays, "glGenVertexArrays"); L(pglBindVertexArray, "glBindVertexArray");
    L(pglActiveTexture, "glActiveTexture");
}
static GLuint compile(GLenum type, const char *src) {
    GLuint s = pglCreateShader(type); pglShaderSource(s, 1, &src, NULL); pglCompileShader(s);
    GLint ok; pglGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; pglGetShaderInfoLog(s, sizeof log, NULL, log); SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Shader error", log, NULL); exit(1); }
    return s;
}

/* --- settings --- */
static int present_mode = VP_120_REPEAT;
static int sync_mode = 2;   /* 0 auto, 1 vblank-locked, 2 VRR/clock (default: original speed) */
static int fullscreen = 1;
static const char *menu_shot;   /* test hook */
static float vol[4] = { 1, 1, 1, 1 };     /* master, music, sfx, voices */
static const int def_keys[MENU_NBIND] = { SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                                          SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_RETURN, SDL_SCANCODE_RSHIFT };
static const int def_pads[MENU_NBIND] = { SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                                          SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_A,
                                          SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK };
static int keymap[MENU_NBIND], padmap[MENU_NBIND];
static int binds_v = 0;   /* 1 = jump/attack rows in true PCE order */
static int had_binds = 0;
static int ss_choice = 2;   /* screensaver delay: off, 1, 3, 5, 10 min */
static void load_ini(void) {
    FILE *f = fopen(ini_path, "r"); if (!f) return;
    char k[64]; float v;
    while (fscanf(f, " %63[^=]=%f", k, &v) == 2) {
        if (!strcmp(k, "filter")) V.filter = (int)v; else if (!strcmp(k, "integer")) V.integer = (int)v;
        else if (!strcmp(k, "present")) present_mode = (int)v;
        else if (!strcmp(k, "sync")) sync_mode = (int)v;
        else if (!strcmp(k, "aspect")) V.aspect = (int)v; else if (!strcmp(k, "fullscreen")) fullscreen = (int)v;
        else if (!strcmp(k, "vol_master")) vol[0] = v; else if (!strcmp(k, "vol_music")) vol[1] = v;
        else if (!strcmp(k, "vol_sfx")) vol[2] = v; else if (!strcmp(k, "vol_voices")) vol[3] = v;
        else if (!strcmp(k, "binds_v")) binds_v = (int)v;
        else if (!strcmp(k, "screensaver")) ss_choice = (int)v;
        else if (!strncmp(k, "key", 3) && k[3] >= '0' && k[3] < '0' + MENU_NBIND) { keymap[k[3] - '0'] = (int)v; had_binds = 1; }
        else if (!strncmp(k, "pad", 3) && k[3] >= '0' && k[3] < '0' + MENU_NBIND) padmap[k[3] - '0'] = (int)v;
        else if (!strcmp(k, "bg_brightness")) V.bri[0] = v; else if (!strcmp(k, "bg_contrast")) V.con[0] = v;
        else if (!strcmp(k, "sprite_brightness")) V.bri[1] = v; else if (!strcmp(k, "sprite_contrast")) V.con[1] = v;
        else if (!strcmp(k, "scanlines")) V.scan_strength = v; else if (!strcmp(k, "mask")) V.mask_strength = v;
    }
    fclose(f);
}
static void save_ini(void) {
    FILE *f = fopen(ini_path, "w"); if (!f) return;
    fprintf(f, "sync=%d\nfilter=%d\ninteger=%d\npresent=%d\nbg_brightness=%.2f\nbg_contrast=%.2f\nsprite_brightness=%.2f\nsprite_contrast=%.2f\nscanlines=%.2f\nmask=%.2f\n",
            sync_mode, V.filter, V.integer, present_mode, V.bri[0], V.con[0], V.bri[1], V.con[1], V.scan_strength, V.mask_strength);
    fprintf(f, "aspect=%d\nfullscreen=%d\nvol_master=%.2f\nvol_music=%.2f\nvol_sfx=%.2f\nvol_voices=%.2f\n", V.aspect, fullscreen, vol[0], vol[1], vol[2], vol[3]);
    fprintf(f, "binds_v=1\nscreensaver=%d\n", ss_choice);
    for (int i = 0; i < MENU_NBIND; i++) fprintf(f, "key%d=%d\npad%d=%d\n", i, keymap[i], i, padmap[i]);
    fclose(f);
}

/* --- find files --- */
#ifdef _WIN32
#include <windows.h>
static int find_file(const char *dir, const char *suffix, char *out, size_t n, int depth) {
    char pat[MAX_PATH * 2]; WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0; size_t sl = strlen(suffix);
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        size_t l = strlen(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) { char sub[MAX_PATH * 2]; snprintf(sub, sizeof sub, "%s%s\\", dir, fd.cFileName); found = find_file(sub, suffix, out, n, depth - 1); }
        } else if (l >= sl && !_stricmp(fd.cFileName + l - sl, suffix)) { snprintf(out, n, "%s%s", dir, fd.cFileName); found = 1; }
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}
#else
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
static int find_file(const char *dir, const char *suffix, char *out, size_t n, int depth) {
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int found = 0; size_t sl = strlen(suffix);
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[2048]; snprintf(p, sizeof p, "%s%s", dir, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        size_t l = strlen(e->d_name);
        if (S_ISDIR(st.st_mode)) { if (depth > 0) { char sub[2100]; snprintf(sub, sizeof sub, "%s/", p); found = find_file(sub, suffix, out, n, depth - 1); } }
        else if (l >= sl && !strcasecmp(e->d_name + l - sl, suffix)) { snprintf(out, n, "%s", p); found = 1; }
    }
    closedir(d);
    return found;
}
#endif

/* --- audio --- */
static SDL_AudioDeviceID dev;
/* audio is collected per game frame, then resampled into the device queue (dynamic rate control) */
static int16_t abuf[8192 * 2]; static int an;
static void audio_cb(pce_t *m, const int16_t *buf, int n) {
    (void)m;
    if (an + n > 8192) n = 8192 - an;
    memcpy(abuf + an * 2, buf, n * 4); an += n;
}
static double drc_pos;                       /* fractional read position carried between frames */
static int16_t alast[2];
static void audio_flush(double scale) {
    /* keep ~50 ms queued: nudge the rate by up to +-0.5 % */
    const double target = 48000 * 0.050;
    double fill = SDL_GetQueuedAudioSize(dev) / 4.0;
    double err = (target - fill) / target; if (err > 1) err = 1; if (err < -1) err = -1;
    double step = 1.0 / (scale * (1.0 + 0.005 * err));   /* input samples per output sample */
    static int16_t out[16384 * 2]; int no = 0;
    double pos = drc_pos;
    while (pos < an && no < 16384) {
        int i = (int)pos; double f = pos - i;
        const int16_t *a = i == 0 ? alast : &abuf[(i - 1) * 2], *b = &abuf[i * 2];
        out[no * 2] = (int16_t)(a[0] + (b[0] - a[0]) * f); out[no * 2 + 1] = (int16_t)(a[1] + (b[1] - a[1]) * f);
        no++; pos += step;
    }
    drc_pos = pos - an;
    if (an) { alast[0] = abuf[(an - 1) * 2]; alast[1] = abuf[(an - 1) * 2 + 1]; }
    an = 0;
    SDL_QueueAudio(dev, out, no * 4);
}
static double now_s(void) { return SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

static uint8_t pad_from_input(const Uint8 *k, SDL_GameController *gc) {
    static const uint8_t bit[MENU_NBIND] = { 0x10, 0x40, 0x80, 0x20, 0x01, 0x02, 0x08, 0x04 };  /* PCE: I = jump, II = attack */
    uint8_t b = 0;
    for (int i = 0; i < MENU_NBIND; i++) {
        if (keymap[i] > 0 && keymap[i] < SDL_NUM_SCANCODES && k[keymap[i]]) b |= bit[i];
        if (gc && padmap[i] >= 0 && SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)padmap[i])) b |= bit[i];
    }
    if (gc) {
        int ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        if (ay < -16000) b |= 0x10; if (ax > 16000) b |= 0x20; if (ay > 16000) b |= 0x40; if (ax < -16000) b |= 0x80;
    }
    return b;
}

static char g_cue[1024], g_bios[1024]; static int g_have_bios;
static void apply_audio(void) { PM.audio.g_master = vol[0]; PM.audio.g_cd = vol[1]; PM.audio.g_psg = vol[2]; PM.audio.g_ad = vol[3]; }
static int boot_machine(void) {
    FILE *lf = PM.log;
    void (*cb)(pce_t *, const int16_t *, int) = PM.audio_cb;
    if (pce_init(&PM, g_cue, g_have_bios ? g_bios : NULL)) return -1;
    PM.native_on = 1; PM.log = lf ? lf : stderr; PM.verbose = 1; PM.audio_cb = cb;
    apply_audio();
    cpu_reset(&PM.cpu);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) return 1;
    char *bp = SDL_GetBasePath(); snprintf(base, sizeof base, "%s", bp ? bp : "./"); SDL_free(bp);
    snprintf(ini_path, sizeof ini_path, "%ssettings.ini", base);
    video_default_cfg(&V);
    memcpy(keymap, def_keys, sizeof keymap); memcpy(padmap, def_pads, sizeof padmap);
    load_ini();
    if (binds_v < 1 && had_binds) {   /* v6 settings: rows 4/5 held each other's buttons */
        int t = keymap[4]; keymap[4] = keymap[5]; keymap[5] = t; t = padmap[4]; padmap[4] = padmap[5]; padmap[5] = t; binds_v = 1;
    }

    char cue[1024], bios[1024], msg[1400];
    if (!find_file(base, ".cue", cue, sizeof cue, 3)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rondo", "Put your Akumajou Dracula X disc (.cue + .img/.bin) next to this program.", NULL); return 1;
    }
    int have_bios = find_file(base, "syscard3.pce", bios, sizeof bios, 3);
    snprintf(g_cue, sizeof g_cue, "%s", cue); snprintf(g_bios, sizeof g_bios, "%s", bios); g_have_bios = have_bios;
    if (pce_init(&PM, cue, have_bios ? bios : NULL)) {
        snprintf(msg, sizeof msg, "Could not load the disc:\n%s", PM.error); SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rondo", msg, NULL); return 1;
    }
    PM.native_on = 1;
    { char lp[1200]; snprintf(lp, sizeof lp, "%srondo.log", base); FILE *lf = fopen(lp, "w"); PM.log = lf ? lf : stderr; if (lf) setvbuf(lf, NULL, _IOLBF, 0); }
    PM.verbose = 1;
    fprintf(PM.log, "cue: %s\nbios: %s\ndata sectors: %ld\n", cue, have_bios ? bios : "(none)", PM.disc_sectors);
    for (int i = 0; i < PM.ntoc; i++) fprintf(PM.log, "track %d %s start %ld frames %ld\n", PM.toc[i].track, PM.toc[i].audio ? "AUDIO" : "DATA", PM.toc[i].start, PM.toc[i].frames);
    { extern const char *bram_dir; static char bd[1100]; snprintf(bd, sizeof bd, "%ssaves", base); bram_dir = bd; }

    SDL_AudioSpec want = {0}, have; want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    fprintf(PM.log, "audio device %u: %d Hz, %d ch, driver %s\n", dev, have.freq, have.channels, SDL_GetCurrentAudioDriver());
    PM.audio_cb = audio_cb; SDL_PauseAudioDevice(dev, 0);
    apply_audio();

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_DisplayMode dm; SDL_GetDesktopDisplayMode(0, &dm);
    int windowed = getenv("RONDO_TEST_FRAMES") != NULL;   /* test mode: plain window at desktop size */
    SDL_Window *win = SDL_CreateWindow("Akumajou Dracula X: Chi no Rondo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       windowed ? dm.w : 1280, windowed ? dm.h : 720,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (windowed || !fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP));
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rondo", "OpenGL 3.3 is required.", win); return 1; }
    load_gl();

    SDL_ShowCursor(SDL_DISABLE);

    GLuint prog = pglCreateProgram();
    pglAttachShader(prog, compile(GL_VERTEX_SHADER, SHADER_VERT)); pglAttachShader(prog, compile(GL_FRAGMENT_SHADER, SHADER_FRAG));
    pglLinkProgram(prog); pglUseProgram(prog);
    GLuint vao; pglGenVertexArrays(1, &vao); pglBindVertexArray(vao);
    GLuint tex; glGenTextures(1, &tex); pglActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    pglUniform1i(pglGetUniformLocation(prog, "uSrc"), 0);
#define U(n) pglGetUniformLocation(prog, n)
    GLint uSrcSize = U("uSrcSize"), uPicOrigin = U("uPicOrigin"), uPicSize = U("uPicSize"), uFbH = U("uFbHeight"), uFilter = U("uFilter"),
          uBri = U("uBri"), uCon = U("uCon"), uScan = U("uScan"), uMask = U("uMask");

    /* menu overlay: 640x360 ARGB, nearest-scaled over the whole screen */
    static const char *ov_frag = "#version 330 core\nuniform sampler2D uOv; uniform vec2 uFb; out vec4 o;\n"
        "void main(){ vec2 uv = vec2(gl_FragCoord.x / uFb.x, 1.0 - gl_FragCoord.y / uFb.y); o = texture(uOv, uv); }\n";
    GLuint ovprog = pglCreateProgram();
    pglAttachShader(ovprog, compile(GL_VERTEX_SHADER, SHADER_VERT)); pglAttachShader(ovprog, compile(GL_FRAGMENT_SHADER, ov_frag));
    pglLinkProgram(ovprog);
    GLuint ovtex; glGenTextures(1, &ovtex); pglActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, ovtex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    pglActiveTexture(GL_TEXTURE0);
    pglUseProgram(ovprog); pglUniform1i(pglGetUniformLocation(ovprog, "uOv"), 1);
    GLint uOvFb = pglGetUniformLocation(ovprog, "uFb");
    pglUseProgram(prog);
    enum { OVW = 1280, OVH = 720 };
    static uint32_t overlay[OVW * OVH];
    menu_ctx mc = { &V, &present_mode, &sync_mode, &fullscreen, { &vol[0], &vol[1], &vol[2], &vol[3] }, keymap, padmap, NULL };
#ifdef SANTABLE
    mc.ss_choice = &ss_choice;
    GLuint ssprog = pglCreateProgram();
    pglAttachShader(ssprog, compile(GL_VERTEX_SHADER, SHADER_VERT)); pglAttachShader(ssprog, compile(GL_FRAGMENT_SHADER, SANTABLE_FRAG));
    pglLinkProgram(ssprog);
    GLuint wtex; glGenTextures(1, &wtex); pglActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, wtex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, WORD_W, WORD_H, 0, GL_RED, GL_UNSIGNED_BYTE, WORDMARK);
    pglActiveTexture(GL_TEXTURE0);
    pglUseProgram(ssprog); pglUniform1i(pglGetUniformLocation(ssprog, "uWord"), 2);
    pglUniform2f(pglGetUniformLocation(ssprog, "uWordSize"), WORD_W, WORD_H);
    GLint uSsRes = pglGetUniformLocation(ssprog, "uRes"), uSsTime = pglGetUniformLocation(ssprog, "uTime");
    pglUseProgram(prog);
#endif
    static const int ss_minutes[5] = { 0, 1, 3, 5, 10 };
    double last_input = 0; int ss_active = 0; double ss_start = 0;
    int started = 0;

    SDL_GameController *gc = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { gc = SDL_GameControllerOpen(i); break; }

    cpu_reset(&PM.cpu);
    long test_frames = getenv("RONDO_TEST_FRAMES") ? atol(getenv("RONDO_TEST_FRAMES")) : 0;
    const char *test_input = getenv("RONDO_TEST_INPUT");
    static uint32_t upload[512 * 242];
    int sw = 256, sh = 224, dot = 0, fresh = 0, running = 1;
    /* frame pacing */
    const double GAME_HZ = 7159090.9 / 455.0 / 263.0;     /* 59.826 Hz */
    SDL_DisplayMode cur; int refresh = 60;
    if (!SDL_GetWindowDisplayMode(win, &cur) && cur.refresh_rate > 0) refresh = cur.refresh_rate;
    int per = (int)(refresh / 59.94 + 0.5); if (per < 1) per = 1;
    int locked = sync_mode == 1 || (sync_mode == 0 && fabs(refresh - per * 59.94) / refresh < 0.012);
    fprintf(PM.log, "display %d Hz -> %s, %d refresh(es) per game frame\n", refresh, locked ? "vblank-locked" : "VRR/clock", locked ? per : 1);
    SDL_GL_SetSwapInterval(1);
    long vbl = 0; double next = now_s();
    while (running) {
        SDL_Event e;
        if (!started && !menu_is_open() && !test_frames) menu_open(&mc, 1);
        if (getenv("RONDO_MENU_SCRIPT")) {   /* test hook: "iteration:KeyName,..." */
            static long iter; iter++;
            const char *p = getenv("RONDO_MENU_SCRIPT"); long it; char kn[32]; int nn;
            while (sscanf(p, "%ld:%31[^,]%n", &it, kn, &nn) == 2) {
                if (it == iter) { SDL_Event ev = {0}; ev.type = SDL_KEYDOWN; ev.key.keysym.scancode = SDL_GetScancodeFromName(kn);
                                     ev.key.keysym.sym = SDL_GetKeyFromScancode(ev.key.keysym.scancode); SDL_PushEvent(&ev); }
                p += nn; if (*p == ',') p++; else break;
            }
            static long shot_at = -1; if (shot_at < 0) shot_at = getenv("RONDO_SHOT_AT") ? atol(getenv("RONDO_SHOT_AT")) : 0;
            if (iter == shot_at) menu_shot = getenv("RONDO_SHOT_FILE");
        }
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { running = 0; break; }
            if (e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_MOUSEBUTTONDOWN ||
                (e.type == SDL_CONTROLLERAXISMOTION && (e.caxis.value > 16000 || e.caxis.value < -16000))) {
                last_input = now_s();
                if (ss_active) { ss_active = 0; next = now_s(); vbl = 0; continue; }   /* wake: swallow the press */
            }
            if (e.type == SDL_CONTROLLERDEVICEADDED && !gc) { gc = SDL_GameControllerOpen(e.cdevice.which); continue; }
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F11 && !e.key.repeat) {
                fullscreen ^= 1; SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); save_ini(); continue;
            }
            if (menu_is_open()) {
                int r = menu_event(&e, &mc);
                switch (r) {
                case MR_START: started = 1; next = now_s(); vbl = 0; break;
                case MR_RESUME: next = now_s(); vbl = 0; break;
                case MR_RESET: boot_machine(); started = 1; next = now_s(); vbl = 0; break;
                case MR_QUIT: running = 0; break;
                case MR_RESET_VIDEO: { int a = V.aspect; video_default_cfg(&V); V.aspect = a; present_mode = VP_120_REPEAT; sync_mode = 2; } /* fallthrough */
                case MR_SYNC:
                    locked = sync_mode == 1 || (sync_mode == 0 && fabs(refresh - per * 59.94) / refresh < 0.012);
                    next = now_s(); vbl = 0;
                    fprintf(PM.log, "sync mode %d -> %s\n", sync_mode, locked ? "vblank-locked" : "VRR/clock");
                    save_ini(); break;
                case MR_RESET_CONTROLS: memcpy(keymap, def_keys, sizeof keymap); memcpy(padmap, def_pads, sizeof padmap); save_ini(); break;
                case MR_CHANGED:
                    apply_audio();
                    if (!!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != fullscreen)
                        SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    save_ini(); break;
                }
                continue;
            }
            /* in game: Esc, Guide, or Start+Back opens the pause menu */
            if ((e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !e.key.repeat) ||
                (e.type == SDL_CONTROLLERBUTTONDOWN && (e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE ||
                 (gc && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START) && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK)))))
                menu_open(&mc, 0);
        }
        if (last_input == 0) last_input = now_s();
#ifdef SANTABLE
        if (!ss_active && ss_minutes[ss_choice] && !test_frames && now_s() - last_input > ss_minutes[ss_choice] * 60.0) {
            if (started && !menu_is_open()) menu_open(&mc, 0);     /* idle in game: pause first */
            ss_active = 1; ss_start = now_s();
        }
        if (getenv("RONDO_SS_TEST")) { ss_active = 1; if (ss_start == 0) ss_start = now_s() - atof(getenv("RONDO_SS_TEST")); }
#endif
        int paused = menu_is_open() || !started && !test_frames || ss_active;
        /* clock mode with repeat/BFI: two presents per game frame (119.65 Hz) */
        int sub = (!locked && present_mode != VP_60) ? 2 : 1;
        int run = locked ? (vbl % per == 0) : (vbl % sub == 0);
        if (run && !paused) {
            PM.joy_buttons = pad_from_input(SDL_GetKeyboardState(NULL), gc);
            if (test_input) {   /* "frame:mask,..." as in pcerun */
                const char *p = test_input; uint8_t mk = 0; long fr; unsigned v; int nn;
                while (sscanf(p, "%ld:%x%n", &fr, &v, &nn) == 2 && fr <= PM.frame) { mk = v; p += nn; if (*p == ',') p++; }
                PM.joy_buttons = mk;
            }
            pce_run_frame(&PM);
            /* locked: the game runs at refresh/per Hz, so stretch its audio to match; clock mode: exact */
            audio_flush(locked ? GAME_HZ / ((double)refresh / per) : 1.0);
            sw = g_fb_w; sh = g_fb_h; dot = g_dotclk;
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++) {
                    uint32_t p = g_fb[y * FB_STRIDE + x];
                    upload[y * sw + x] = ((p >> 16) & 255) | (p & 0xFF00) | ((p & 255) << 16) | (g_layer[y * FB_STRIDE + x] == 2 ? 0xFF000000u : 0);
                }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, upload);
            fresh = 1;
        }
        int fw, fh; SDL_GL_GetDrawableSize(win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        if (present_mode == VP_120_BFI && ((locked && per >= 2) || !locked) && !fresh && !paused) {   /* black on the in-between refreshes */
            glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        } else {
            int ox, oy, ow, oh; video_geometry(&V, sw, sh, dot, fw, fh, &ox, &oy, &ow, &oh);
            pglUniform2f(uSrcSize, sw, sh); pglUniform2f(uPicOrigin, ox, oy); pglUniform2f(uPicSize, ow, oh);
            pglUniform1f(uFbH, fh); pglUniform1i(uFilter, V.filter);
            pglUniform2f(uBri, V.bri[0], V.bri[1]); pglUniform2f(uCon, V.con[0], V.con[1]);
            pglUniform1f(uScan, V.scan_strength); pglUniform1f(uMask, V.mask_strength);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
#ifdef SANTABLE
        if (ss_active) {
            pglUseProgram(ssprog); pglUniform2f(uSsRes, fw, fh); pglUniform1f(uSsTime, (float)(now_s() - ss_start));
            glDrawArrays(GL_TRIANGLES, 0, 3); pglUseProgram(prog);
            if (menu_shot) {
                static uint8_t px3[3840 * 2160 * 3];
                glReadPixels(0, 0, fw, fh, GL_RGB, GL_UNSIGNED_BYTE, px3);
                FILE *o = fopen(menu_shot, "wb"); fprintf(o, "P6\n%d %d\n255\n", fw, fh);
                for (int y = fh - 1; y >= 0; y--) fwrite(px3 + y * fw * 3, 3, fw, o);
                fclose(o); running = 0;
            }
        } else
#endif
        if (menu_is_open()) {
            menu_draw(overlay, OVW, OVH);
            if (!started) { glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); }
            pglActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, ovtex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, OVW, OVH, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, overlay);
            pglActiveTexture(GL_TEXTURE0);
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            pglUseProgram(ovprog); pglUniform2f(uOvFb, fw, fh);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            pglUseProgram(prog); glDisable(GL_BLEND);
            if (menu_shot) {   /* test hook: capture the menu once and exit */
                static uint8_t px2[3840 * 2160 * 3];
                glReadPixels(0, 0, fw, fh, GL_RGB, GL_UNSIGNED_BYTE, px2);
                FILE *o = fopen(menu_shot, "wb"); fprintf(o, "P6\n%d %d\n255\n", fw, fh);
                for (int y = fh - 1; y >= 0; y--) fwrite(px2 + y * fw * 3, 3, fw, o);
                fclose(o); running = 0;
            }
        }
        if (test_frames && PM.frame >= test_frames && fresh) {
            static uint8_t px[3840 * 2160 * 3];
            glReadPixels(0, 0, fw, fh, GL_RGB, GL_UNSIGNED_BYTE, px);
            FILE *o = fopen(getenv("RONDO_TEST_OUT") ? getenv("RONDO_TEST_OUT") : "test.ppm", "wb");
            fprintf(o, "P6\n%d %d\n255\n", fw, fh);
            for (int y = fh - 1; y >= 0; y--) fwrite(px + y * fw * 3, 3, fw, o);
            fclose(o);
            running = 0;
        }
        if (!locked) {
            /* VRR / clock mode: present each game frame at its exact time */
            next += 1.0 / (GAME_HZ * sub);
            double wait = next - now_s();
            if (wait > 0.002) SDL_Delay((Uint32)((wait - 0.0015) * 1000));
            while (now_s() < next) {}
            if (now_s() - next > 0.1) next = now_s();      /* fell behind (window drag etc.) */
        }
        SDL_GL_SwapWindow(win);
        fresh = 0; vbl++;
        if (!started || menu_is_open()) {}
        if (test_frames && !locked) {}
    }
    save_ini();
    SDL_Quit();
    return 0;
}
