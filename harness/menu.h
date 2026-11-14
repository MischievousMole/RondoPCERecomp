#ifndef MENU_H
#define MENU_H
#include <SDL.h>
#include "video.h"

#define MENU_NBIND 8   /* up, down, left, right, jump, attack, run, select */
enum { MR_NONE, MR_START, MR_RESUME, MR_RESET, MR_QUIT, MR_CHANGED, MR_SYNC, MR_BACK, MR_RESET_VIDEO, MR_RESET_CONTROLS };

typedef struct menu_ctx {
    video_cfg *v;
    int *present_mode, *sync_mode, *fullscreen;
    float *vol[4];            /* master, music, sfx, voices */
    int *keymap, *padmap;     /* SDL scancodes / SDL controller buttons, MENU_NBIND each */
} menu_ctx;

extern const char *const menu_bind_names[MENU_NBIND];
void menu_open(menu_ctx *c, int is_launcher);
int  menu_is_open(void);
int  menu_preview(void);     /* video page: show the game undimmed beside the panel */
int  menu_event(const SDL_Event *e, menu_ctx *c);
void menu_draw(uint32_t *buf, int w, int h);
#endif
