// Quality-of-life features layered on the recompiled game (see README "Quality of life").
//
// Hardware cursor: the game draws its mouse cursor (and any item being dragged) with the sprite
// blitter 0x44a7a0(x, y, sprite, mode) at exactly the mouse position, once per frame, so the
// pointer lags the hand by the frame time. The hook skips those draws, and at each Flip the
// sprites drawn at the mouse position are decoded (the game's run-length sprite format), coloured
// with the screen palette, scaled to the window, and handed to SDL as the OS cursor, which the
// system moves at full rate. ICARUS_HWCURSOR=0 turns it off.
//
// Dialogue speed: 0x44a0d0 types dialogue one character every second frame; ICARUS_TEXT_SPEED
// (default 2) multiplies that, 0 shows whole lines at once, 1 is the original speed.
#include "win32.h"
#include <SDL3/SDL.h>
#include <math.h>

SDL_Window *video_window(void);
float video_points_per_pixel(void);
const uint32_t *ddraw_screen_palette(uint32_t *gen);

#define G_MOUSE_X   0x52a4c4u
#define G_MOUSE_Y   0x52a4c8u
#define G_SPR_DATA  0x531f70u     // u32[] sprite data pointers
#define G_SPR_W     0x526300u     // u16[] widths
#define G_SPR_H     0x52c290u     // u16[] heights

// ---------------------------------------------------------------- hardware cursor
static int hw = -1;
int hwcursor_active(void) {
    if (hw < 0) {
        const char *e = getenv("ICARUS_HWCURSOR");
        hw = video_window() != 0 && !(e && !atoi(e));
    }
    return hw;
}
#define MAXC 4
static uint32_t frame_spr[MAXC]; static int frame_n;

int sprite_cursor_hook(void) {
    if (!hwcursor_active()) return 0;
    int x = (int16_t)ARG(0), y = (int16_t)ARG(1);
    if (x != (int16_t)RD16(G_MOUSE_X) || y != (int16_t)RD16(G_MOUSE_Y) || ARG(3) != 0) return 0;
    if (frame_n < MAXC) frame_spr[frame_n++] = ARG(2) & 0xffff;
    return 1;
}

// decode sprite into an 8-bit index image + mask (w*h)
static int decode_sprite(uint32_t idx, uint8_t *pix, uint8_t *mask, int w, int h) {
    uint32_t p = RD32(G_SPR_DATA + 4 * idx);
    if (!p) return 0;
    int x = 0, y = 0, guard = 0;
    while (y < h && guard++ < 1 << 20) {
        uint8_t b = RD8(p++);
        if (b == 0xfd) break;
        if (b == 0xfe) { y++; x = 0; continue; }
        if (b == 0) { x += RD8(p++); continue; }
        if (b <= 3 || b == 0xff) { x++; continue; }
        if (x >= 0 && x < w) { pix[y * w + x] = b; mask[y * w + x] = 1; }
        x++;
    }
    return 1;
}

static uint64_t cur_key; static SDL_Cursor *cur_cursor; static int cur_hidden = -1;
static void show(int vis) {
    if (vis == cur_hidden) return;
    cur_hidden = vis;
    if (vis) SDL_ShowCursor(); else SDL_HideCursor();
}
static SDL_Surface *scaled(const uint32_t *rgba, int w, int h, int s) {
    SDL_Surface *sf = SDL_CreateSurface(w * s, h * s, SDL_PIXELFORMAT_ARGB8888);
    if (!sf) return 0;
    for (int y = 0; y < h * s; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)sf->pixels + y * sf->pitch);
        for (int x = 0; x < w * s; x++) row[x] = rgba[(y / s) * w + x / s];
    }
    return sf;
}
// called by DirectDraw on every Flip of the primary surface
void cursor_frame_end(void) {
    if (!hwcursor_active()) return;
    int n = frame_n; frame_n = 0;
    if (!n) { show(0); return; }
    uint32_t gen; const uint32_t *pal = ddraw_screen_palette(&gen);
    if (!pal) return;
    // composite all sprites drawn at the mouse position, top-left aligned (the hot spot)
    int W = 0, H = 0;
    for (int k = 0; k < n; k++) { int w = RD16(G_SPR_W + 2 * frame_spr[k]), h = RD16(G_SPR_H + 2 * frame_spr[k]); if (w > W) W = w; if (h > H) H = h; }
    if (W <= 0 || H <= 0 || W > 256 || H > 256) { show(0); return; }
    static uint32_t rgba[256 * 256]; static uint8_t pix[256 * 256], mask[256 * 256];
    memset(rgba, 0, sizeof(uint32_t) * W * H);
    for (int k = 0; k < n; k++) {
        int w = RD16(G_SPR_W + 2 * frame_spr[k]), h = RD16(G_SPR_H + 2 * frame_spr[k]);
        memset(mask, 0, (size_t)(w * h));
        if (!decode_sprite(frame_spr[k], pix, mask, w, h)) continue;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
            if (mask[y * w + x]) rgba[y * W + x] = 0xff000000u | (pal[pix[y * w + x]] & 0xffffff);
    }
    // cursor size follows the window scale; HiDPI displays get a full-resolution alternate image
    float pts = video_points_per_pixel();
    SDL_Window *win = video_window();
    float dens = SDL_GetWindowPixelDensity(win); if (dens <= 0) dens = 1;
    int s1 = (int)(pts + 0.5f); if (s1 < 1) s1 = 1;
    int s2 = (int)(pts * dens + 0.5f); if (s2 < 1) s2 = 1;
    uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < W * H; i++) key = (key ^ rgba[i]) * 1099511628211ull;
    key ^= ((uint64_t)W << 48) ^ ((uint64_t)H << 40) ^ ((uint64_t)s1 << 32) ^ ((uint64_t)s2 << 24);
    if (key != cur_key || !cur_cursor) {
        SDL_Surface *base = scaled(rgba, W, H, s1);
        if (base && s2 != s1) { SDL_Surface *hi = scaled(rgba, W, H, s2); if (hi) { SDL_AddSurfaceAlternateImage(base, hi); SDL_DestroySurface(hi); } }
        if (base && getenv("ICARUS_DUMP_CURSOR")) SDL_SaveBMP(base, getenv("ICARUS_DUMP_CURSOR"));
        SDL_Cursor *c = base ? SDL_CreateColorCursor(base, 0, 0) : 0;
        if (base) SDL_DestroySurface(base);
        if (c) { SDL_SetCursor(c); if (cur_cursor) SDL_DestroyCursor(cur_cursor); cur_cursor = c; cur_key = key; }
    }
    show(1);
}

// ---------------------------------------------------------------- dialogue speed
#define G_TW_SHOWN  0x5c6768u    // characters shown so far
#define G_TW_TICK   0x5c676cu    // frame counter (one character every second frame)
#define G_TW_ACTIVE 0x5c6770u
#define G_TW_TOTAL  0x5c6774u
void text_speed_hook(void) {
    static int speed = -1; static float acc;
    if (speed < 0) speed = getenv("ICARUS_TEXT_SPEED") ? atoi(getenv("ICARUS_TEXT_SPEED")) : 2;
    if (speed == 1 || !RD16(G_TW_ACTIVE)) return;
    uint16_t shown = RD16(G_TW_SHOWN), total = RD16(G_TW_TOTAL);
    if (total < 2 || shown + 1 >= total) return;
    // the routine itself still adds one character and ends the line when shown == total,
    // so never go past total - 1
    if (speed == 0) { WR16(G_TW_SHOWN, (uint16_t)(total - 1)); return; }
    acc += (speed - 1) * 0.5f;
    while (acc >= 1 && shown + 1 < total) { shown++; acc -= 1; }
    WR16(G_TW_SHOWN, shown);
}
