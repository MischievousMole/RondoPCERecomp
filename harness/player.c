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
#include "pce.h"
#include "video.h"
#include "shaders_gen.h"

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
static void load_ini(void) {
    FILE *f = fopen(ini_path, "r"); if (!f) return;
    char k[64]; float v;
    while (fscanf(f, " %63[^=]=%f", k, &v) == 2) {
        if (!strcmp(k, "filter")) V.filter = (int)v; else if (!strcmp(k, "integer")) V.integer = (int)v;
        else if (!strcmp(k, "present")) present_mode = (int)v;
        else if (!strcmp(k, "bg_brightness")) V.bri[0] = v; else if (!strcmp(k, "bg_contrast")) V.con[0] = v;
        else if (!strcmp(k, "sprite_brightness")) V.bri[1] = v; else if (!strcmp(k, "sprite_contrast")) V.con[1] = v;
        else if (!strcmp(k, "scanlines")) V.scan_strength = v; else if (!strcmp(k, "mask")) V.mask_strength = v;
    }
    fclose(f);
}
static void save_ini(void) {
    FILE *f = fopen(ini_path, "w"); if (!f) return;
    fprintf(f, "filter=%d\ninteger=%d\npresent=%d\nbg_brightness=%.2f\nbg_contrast=%.2f\nsprite_brightness=%.2f\nsprite_contrast=%.2f\nscanlines=%.2f\nmask=%.2f\n",
            V.filter, V.integer, present_mode, V.bri[0], V.con[0], V.bri[1], V.con[1], V.scan_strength, V.mask_strength);
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
static void audio_cb(pce_t *m, const int16_t *buf, int n) { (void)m; SDL_QueueAudio(dev, buf, n * 4); }

static uint8_t pad_from_input(const Uint8 *k, SDL_GameController *gc) {
    uint8_t b = 0;
    if (k[SDL_SCANCODE_X]) b |= 0x02; if (k[SDL_SCANCODE_Z]) b |= 0x01;   /* X = jump (II), Z = attack (I) */
    if (k[SDL_SCANCODE_RSHIFT]) b |= 0x04; if (k[SDL_SCANCODE_RETURN]) b |= 0x08;
    if (k[SDL_SCANCODE_UP]) b |= 0x10; if (k[SDL_SCANCODE_RIGHT]) b |= 0x20;
    if (k[SDL_SCANCODE_DOWN]) b |= 0x40; if (k[SDL_SCANCODE_LEFT]) b |= 0x80;
    if (gc) {
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A)) b |= 0x02;   /* jump */
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X) || SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B)) b |= 0x01;   /* attack */
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK)) b |= 0x04;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START)) b |= 0x08;
        int ax = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX), ay = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -16000) b |= 0x10;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 16000) b |= 0x20;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 16000) b |= 0x40;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -16000) b |= 0x80;
    }
    return b;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) return 1;
    char *bp = SDL_GetBasePath(); snprintf(base, sizeof base, "%s", bp ? bp : "./"); SDL_free(bp);
    snprintf(ini_path, sizeof ini_path, "%ssettings.ini", base);
    video_default_cfg(&V); load_ini();

    char cue[1024], bios[1024], msg[1400];
    if (!find_file(base, ".cue", cue, sizeof cue, 3)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rondo", "Put your Akumajou Dracula X disc (.cue + .img/.bin) next to this program.", NULL); return 1;
    }
    int have_bios = find_file(base, "syscard3.pce", bios, sizeof bios, 3);
    if (pce_init(&PM, cue, have_bios ? bios : NULL)) {
        snprintf(msg, sizeof msg, "Could not load the disc:\n%s", PM.error); SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rondo", msg, NULL); return 1;
    }
    PM.native_on = 1; PM.log = stderr;
    { extern const char *bram_dir; static char bd[1100]; snprintf(bd, sizeof bd, "%ssaves", base); bram_dir = bd; }

    SDL_AudioSpec want = {0}, have; want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    PM.audio_cb = audio_cb; SDL_PauseAudioDevice(dev, 0);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_DisplayMode dm; SDL_GetDesktopDisplayMode(0, &dm);
    int windowed = getenv("RONDO_TEST_FRAMES") != NULL;   /* test mode: plain window at desktop size */
    SDL_Window *win = SDL_CreateWindow("Akumajou Dracula X: Chi no Rondo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       windowed ? dm.w : 1280, windowed ? dm.h : 720,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (windowed ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP));
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Rondo", "OpenGL 3.3 is required.", win); return 1; }
    load_gl();
    SDL_GL_SetSwapInterval(-1) == 0 || SDL_GL_SetSwapInterval(1);    /* adaptive vsync if available */
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

    SDL_GameController *gc = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { gc = SDL_GameControllerOpen(i); break; }

    cpu_reset(&PM.cpu);
    long test_frames = getenv("RONDO_TEST_FRAMES") ? atol(getenv("RONDO_TEST_FRAMES")) : 0;
    const char *test_input = getenv("RONDO_TEST_INPUT");
    static uint32_t upload[512 * 242];
    int sw = 256, sh = 224, dot = 0, fresh = 0, running = 1;
    const Uint32 target_q = 48000 * 4 * 3 / 60;     /* keep ~3 frames of audio queued */
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !gc) gc = SDL_GameControllerOpen(e.cdevice.which);
            else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_F2: V.filter = (V.filter + 1) % 4; break;
                case SDLK_F3: present_mode = (present_mode + 1) % 3; break;
                case SDLK_F4: V.integer ^= 1; break;
                case SDLK_F5: V.bri[1] -= 0.05f; break; case SDLK_F6: V.bri[1] += 0.05f; break;
                case SDLK_F7: V.bri[0] -= 0.05f; break; case SDLK_F8: V.bri[0] += 0.05f; break;
                case SDLK_F11: SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP); break;
                }
                save_ini();
            }
        }
        /* emulate when the audio queue needs more (audio clock paces the game at 59.83 Hz) */
        int ran = 0;
        while (SDL_GetQueuedAudioSize(dev) < target_q && ran < 3) {
            PM.joy_buttons = pad_from_input(SDL_GetKeyboardState(NULL), gc);
            if (test_input) {   /* "frame:mask,..." as in pcerun */
                const char *p = test_input; uint8_t mk = 0; long fr; unsigned v; int nn;
                while (sscanf(p, "%ld:%x%n", &fr, &v, &nn) == 2 && fr <= PM.frame) { mk = v; p += nn; if (*p == ',') p++; }
                PM.joy_buttons = mk;
            }
            pce_run_frame(&PM); ran++;
        }
        if (ran) {
            sw = g_fb_w; sh = g_fb_h; dot = g_dotclk;
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++) {
                    uint32_t p = g_fb[y * FB_STRIDE + x];
                    /* RGBA8 in memory: R, G, B, A(sprite layer) */
                    upload[y * sw + x] = ((p >> 16) & 255) | (p & 0xFF00) | ((p & 255) << 16) | (g_layer[y * FB_STRIDE + x] == 2 ? 0xFF000000u : 0);
                }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, upload);
            fresh = 1;
        }
        int fw, fh; SDL_GL_GetDrawableSize(win, &fw, &fh);
        glViewport(0, 0, fw, fh);
        if (present_mode == VP_120_BFI && !fresh) {      /* second refresh of this frame: black */
            glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        } else {
            int ox, oy, ow, oh; video_geometry(&V, sw, sh, dot, fw, fh, &ox, &oy, &ow, &oh);
            pglUniform2f(uSrcSize, sw, sh); pglUniform2f(uPicOrigin, ox, oy); pglUniform2f(uPicSize, ow, oh);
            pglUniform1f(uFbH, fh); pglUniform1i(uFilter, V.filter);
            pglUniform2f(uBri, V.bri[0], V.bri[1]); pglUniform2f(uCon, V.con[0], V.con[1]);
            pglUniform1f(uScan, V.scan_strength); pglUniform1f(uMask, V.mask_strength);
            glDrawArrays(GL_TRIANGLES, 0, 3);
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
        SDL_GL_SwapWindow(win);
        fresh = 0;
        if (present_mode == VP_60 && !ran) SDL_Delay(1);
    }
    save_ini();
    SDL_Quit();
    return 0;
}
