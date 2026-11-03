// Launcher: pick the disc images, install the game data, change settings, play.
// Drawn with SDL3's renderer at a logical 800x600 with 2x text (crisp on HiDPI displays).
#include "launcher.h"
#include "installer.h"
#include <stdint.h>
#include "font_ui.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LW 800
#define LH 600

// ---------------------------------------------------------------- settings model
struct opt { const char *key, *label; int n; const char *val[5]; const char *txt[5]; int cur; };
static struct opt OPTS[] = {
    { "SCALE", "Window size", 4, { "1", "2", "3", "4" }, { "640 x 480", "1280 x 960", "1920 x 1440", "2560 x 1920" }, 1 },
    { "FULLSCREEN", "Display", 2, { "0", "1" }, { "Window", "Fullscreen" }, 0 },
    { "SCALE_MODE", "Scaling", 4, { "sharp", "nearest", "smooth", "integer" }, { "Sharp", "Pixel", "Smooth", "Integer" }, 0 },
    { "FPS", "Frame rate", 2, { "", "60" }, { "Match display", "60 fps" }, 0 },
    { "HWCURSOR", "Mouse cursor", 2, { "1", "0" }, { "System (no lag)", "In-game" }, 0 },
    { "TEXT_SPEED", "Dialogue speed", 3, { "1", "2", "0" }, { "Original", "Fast", "Instant" }, 1 },
    { "FONT", "Dialogue font", 2, { "replacement", "original" }, { "Modern", "Original (1998)" }, 0 },
    { "MUTE", "Sound", 2, { "0", "1" }, { "On", "Off" }, 0 },
    { "LAUNCHER", "Launcher at startup", 2, { "1", "0" }, { "Show", "Skip" }, 0 },
};
#define NOPT (int)(sizeof OPTS / sizeof OPTS[0])

static void load_opts(void) {
    for (int k = 0; k < NOPT; k++) {
        char name[64]; snprintf(name, sizeof name, "ICARUS_%s", OPTS[k].key);
        const char *v = getenv(name);
        if (!v) continue;
        for (int i = 0; i < OPTS[k].n; i++) if (!strcmp(v, OPTS[k].val[i])) OPTS[k].cur = i;
    }
}

// ---------------------------------------------------------------- drawing helpers
static SDL_Window *win; static SDL_Renderer *ren;
static float mx, my; static int mdown, mclick, rclick;
struct ttex { uint64_t key; SDL_Texture *t; float w, h; int age; };
static struct ttex cache[192];
static uint64_t hash_str(const char *s, const void *f) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)(uintptr_t)f;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}
static float text_width(const struct uifont *f, const char *s) {
    int w = 0; for (; *s; s++) { int c = (uint8_t)*s; if (c < 32 || c > 126) c = '?'; w += f->g[c - 32].adv; }
    return w / 2.0f;
}
static struct ttex *text(const struct uifont *f, const char *s) {
    uint64_t key = hash_str(s, f);
    int free_slot = -1, old = 0;
    for (int k = 0; k < 192; k++) {
        if (cache[k].t && cache[k].key == key) { cache[k].age = 0; return &cache[k]; }
        if (!cache[k].t && free_slot < 0) free_slot = k;
        if (cache[k].age > cache[old].age) old = k;
    }
    if (free_slot < 0) { SDL_DestroyTexture(cache[old].t); cache[old].t = 0; free_slot = old; }
    int w = 2, h = f->ascent + f->descent + 2;
    for (const char *p = s; *p; p++) { int c = (uint8_t)*p; if (c < 32 || c > 126) c = '?'; w += f->g[c - 32].adv; }
    SDL_Surface *sf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ARGB8888);
    memset(sf->pixels, 0, (size_t)sf->pitch * h);
    int pen = 1;
    for (const char *p = s; *p; p++) {
        int c = (uint8_t)*p; if (c < 32 || c > 126) c = '?';
        const struct uiglyph *g = &f->g[c - 32];
        for (int y = 0; y < g->h; y++) for (int x = 0; x < g->w; x++) {
            int px = pen + g->x + x, py = g->y + y;
            if (px < 0 || py < 0 || px >= w || py >= h) continue;
            uint8_t a = f->a[g->off + y * g->w + x];
            if (a) ((uint32_t *)((uint8_t *)sf->pixels + py * sf->pitch))[px] = (uint32_t)a << 24 | 0xffffff;
        }
        pen += g->adv;
    }
    struct ttex *t = &cache[free_slot];
    t->t = SDL_CreateTextureFromSurface(ren, sf); t->w = w / 2.0f; t->h = h / 2.0f; t->key = key; t->age = 0;
    SDL_SetTextureScaleMode(t->t, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(sf);
    return t;
}
enum { AL_LEFT, AL_CENTER, AL_RIGHT };
static void draw_text(const struct uifont *f, const char *s, float x, float y, uint32_t rgb, int align, float alpha) {
    if (!*s) return;
    struct ttex *t = text(f, s);
    float dx = align == AL_CENTER ? x - t->w / 2 : align == AL_RIGHT ? x - t->w : x;
    SDL_SetTextureColorMod(t->t, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    SDL_SetTextureAlphaModFloat(t->t, alpha);
    SDL_FRect d = { dx, y, t->w, t->h };
    SDL_RenderTexture(ren, t->t, 0, &d);
}
static void fill(float x, float y, float w, float h, uint32_t rgb, float a) {
    SDL_SetRenderDrawColor(ren, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (Uint8)(a * 255));
    SDL_FRect r = { x, y, w, h }; SDL_RenderFillRect(ren, &r);
}
static void frame(float x, float y, float w, float h, uint32_t rgb, float a) {
    SDL_SetRenderDrawColor(ren, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (Uint8)(a * 255));
    SDL_FRect r = { x, y, w, h }; SDL_RenderRect(ren, &r);
}
static void gradient(void) {
    SDL_Vertex v[4] = {
        { { 0, 0 }, { 0.09f, 0.07f, 0.06f, 1 }, { 0, 0 } }, { { LW, 0 }, { 0.09f, 0.07f, 0.06f, 1 }, { 0, 0 } },
        { { LW, LH }, { 0.03f, 0.03f, 0.04f, 1 }, { 0, 0 } }, { { 0, LH }, { 0.03f, 0.03f, 0.04f, 1 }, { 0, 0 } },
    };
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(ren, 0, v, 4, idx, 6);
}
static int hover(float x, float y, float w, float h) { return mx >= x && mx < x + w && my >= y && my < y + h; }
#define GOLD 0xd4af5au
#define GOLD_DIM 0x8a7440u
#define TEXT 0xe8e2d6u
#define MUTED 0x9a938au
#define PANEL 0x15120fu
// button: returns 1 when clicked
static int button(const char *label, float x, float y, float w, float h, int primary, int enabled) {
    int hv = enabled && hover(x, y, w, h);
    fill(x, y, w, h, primary ? (hv ? 0xe2c06au : 0xc9a24bu) : (hv ? 0x3a322bu : 0x2a241fu), enabled ? 1 : 0.4f);
    frame(x, y, w, h, primary ? 0xf0d78au : GOLD_DIM, enabled ? 0.9f : 0.3f);
    draw_text(&UI_BODY, label, x + w / 2, y + (h - (UI_BODY.ascent + UI_BODY.descent) / 2.0f) / 2, primary ? 0x1a140cu : TEXT, AL_CENTER, enabled ? 1 : 0.45f);
    return hv && mclick;
}
static void elide(const char *path, char *out, size_t n, float maxw) {
    if (!path || !*path) { snprintf(out, n, "(not selected)"); return; }
    if (text_width(&UI_SMALL, path) <= maxw) { snprintf(out, n, "%s", path); return; }
    const char *s = path;
    while (*s && text_width(&UI_SMALL, s) + text_width(&UI_SMALL, "...") > maxw) s++;
    snprintf(out, n, "...%s", s);
}

// ---------------------------------------------------------------- file dialog
static char picked[2][1024]; static volatile int picked_ready[2];
static void SDLCALL on_pick(void *ud, const char *const *files, int filter) {
    (void)filter; int which = (int)(intptr_t)ud;
    if (files && files[0]) { snprintf(picked[which], sizeof picked[which], "%s", files[0]); picked_ready[which] = 1; }
}
static const SDL_DialogFileFilter FILTERS[] = { { "Disc images (cue, bin, iso)", "cue;bin;iso" }, { "All files", "*" } };

// ---------------------------------------------------------------- install thread
static struct install_progress prog; static volatile int installing;
static char cd_path[2][1024]; static const char *inst_dir;
static int SDLCALL install_thread(void *arg) {
    (void)arg;
    install_game(cd_path[0], cd_path[1], inst_dir, &prog);
    installing = 0;
    return 0;
}

// ---------------------------------------------------------------- config file
static void write_config(const char *cfg) {
    FILE *f = fopen(cfg, "w");
    if (!f) return;
    fprintf(f, "# Icarus settings (written by the launcher). Environment variables ICARUS_<NAME> override them.\n\n");
    fprintf(f, "CD1 = %s\nCD2 = %s\n", cd_path[0], cd_path[1]);
    for (int k = 0; k < NOPT; k++) {
        if (!OPTS[k].val[OPTS[k].cur][0]) fprintf(f, "#%s =\n", OPTS[k].key);
        else fprintf(f, "%s = %s\n", OPTS[k].key, OPTS[k].val[OPTS[k].cur]);
    }
    fclose(f);
}
static void apply_env(void) {
    for (int k = 0; k < NOPT; k++) {
        char name[64]; snprintf(name, sizeof name, "ICARUS_%s", OPTS[k].key);
        const char *v = OPTS[k].val[OPTS[k].cur];
        if (v[0]) SDL_setenv_unsafe(name, v, 1); else SDL_unsetenv_unsafe(name);
    }
}

// ---------------------------------------------------------------- main loop
int launcher_run(const char *data_dir, const char *save_dir, const char *cfg_path, int (*data_ready)(void)) {
    inst_dir = data_dir;
    load_opts();
    snprintf(cd_path[0], sizeof cd_path[0], "%s", getenv("ICARUS_CD1") ? getenv("ICARUS_CD1") : "");
    snprintf(cd_path[1], sizeof cd_path[1], "%s", getenv("ICARUS_CD2") ? getenv("ICARUS_CD2") : "");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { fprintf(stderr, "launcher: %s\n", SDL_GetError()); return LAUNCH_PLAY; }
    if (!SDL_CreateWindowAndRenderer("Icarus - Recomp by Second Save", LW, LH, SDL_WINDOW_HIGH_PIXEL_DENSITY, &win, &ren)) {
        fprintf(stderr, "launcher: %s\n", SDL_GetError()); return LAUNCH_PLAY;
    }
    SDL_SetRenderLogicalPresentation(ren, LW, LH, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderVSync(ren, 1);
    int result = -1, ready = data_ready();
    char status[600] = "";
    while (result < 0) {
        mclick = rclick = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) result = LAUNCH_QUIT;
            else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                SDL_ConvertEventToRenderCoordinates(ren, &e);
                if (e.type == SDL_EVENT_MOUSE_MOTION) { mx = e.motion.x; my = e.motion.y; }
                else { mx = e.button.x; my = e.button.y; }
                if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) { if (e.button.button == SDL_BUTTON_LEFT) mclick = 1; if (e.button.button == SDL_BUTTON_RIGHT) rclick = 1; }
            } else if (e.type == SDL_EVENT_KEY_DOWN && !installing) {
                if (e.key.key == SDLK_ESCAPE) result = LAUNCH_QUIT;
                if ((e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) && ready) result = LAUNCH_PLAY;
            }
        }
        for (int k = 0; k < 2; k++) if (picked_ready[k]) { picked_ready[k] = 0; snprintf(cd_path[k], sizeof cd_path[k], "%s", picked[k]); }
        if (prog.done) {
            prog.done = 0; ready = data_ready();
            if (prog.ok) snprintf(status, sizeof status, "%s", prog.warn[0] ? prog.warn : "Game installed. Ready to play.");
            else snprintf(status, sizeof status, "Install failed: %s", prog.err);
            write_config(cfg_path);
        }
        for (int k = 0; k < 192; k++) if (cache[k].t) cache[k].age++;

        gradient();
        // title
        draw_text(&UI_TITLE, "ICARUS", LW / 2.0f, 22, GOLD, AL_CENTER, 1);
        draw_text(&UI_SUB, "Sanctuary of the Gods", LW / 2.0f, 74, TEXT, AL_CENTER, 0.85f);
        fill(LW / 2.0f - 120, 104, 240, 1, GOLD_DIM, 0.8f);
        draw_text(&UI_SMALL, "Recomp by Second Save", LW / 2.0f, 110, GOLD, AL_CENTER, 1);

        // discs panel
        float px = 40, py = 146, pw = LW - 80;
        fill(px, py, pw, 150, PANEL, 0.85f); frame(px, py, pw, 150, GOLD_DIM, 0.6f);
        draw_text(&UI_HEAD, "Game discs", px + 16, py + 8, GOLD, AL_LEFT, 1);
        for (int k = 0; k < 2; k++) {
            float ry = py + 42 + 34 * k;
            draw_text(&UI_BODY, k ? "Disc 2" : "Disc 1", px + 16, ry + 3, TEXT, AL_LEFT, 1);
            fill(px + 90, ry, pw - 230, 26, 0x0c0a09u, 1); frame(px + 90, ry, pw - 230, 26, 0x3d352cu, 1);
            char el[1100]; elide(cd_path[k], el, sizeof el, pw - 250);
            draw_text(&UI_SMALL, el, px + 98, ry + 5, cd_path[k][0] ? TEXT : MUTED, AL_LEFT, 1);
            if (button("Browse...", px + pw - 130, ry, 114, 26, 0, !installing))
                SDL_ShowOpenFileDialog(on_pick, (void *)(intptr_t)k, win, FILTERS, 2, cd_path[k][0] ? cd_path[k] : 0, false);
        }
        float sy = py + 114;
        if (installing) {
            fill(px + 16, sy + 6, pw - 180, 14, 0x0c0a09u, 1);
            fill(px + 16, sy + 6, (pw - 180) * prog.frac, 14, GOLD, 1);
            draw_text(&UI_SMALL, prog.msg, px + 16, sy - 14, MUTED, AL_LEFT, 1);
        } else {
            const char *st = status[0] ? status : ready ? "Game data installed." : "Select your Icarus discs (cue/bin or iso), then Install.";
            draw_text(&UI_SMALL, st, px + 16, sy + 4, status[0] && !prog.ok ? 0xe07a5fu : (ready ? 0x9fc58au : MUTED), AL_LEFT, 1);
        }
        if (button(ready ? "Reinstall" : "Install", px + pw - 130, sy, 114, 26, !ready, !installing && cd_path[0][0])) {
            memset(&prog, 0, sizeof prog); installing = 1; status[0] = 0;
            SDL_DetachThread(SDL_CreateThread(install_thread, "install", 0));
        }

        // settings panel
        py = 316; float ph = 210;
        fill(px, py, pw, ph, PANEL, 0.85f); frame(px, py, pw, ph, GOLD_DIM, 0.6f);
        draw_text(&UI_HEAD, "Settings", px + 16, py + 8, GOLD, AL_LEFT, 1);
        for (int k = 0; k < NOPT; k++) {
            int col = k < 5 ? 0 : 1, row = k < 5 ? k : k - 5;
            float ox = px + 16 + col * (pw / 2), oy = py + 46 + row * 32;
            draw_text(&UI_SMALL, OPTS[k].label, ox, oy + 5, MUTED, AL_LEFT, 1);
            float bx = ox + 150, bw = pw / 2 - 182;
            int hv = hover(bx, oy, bw, 26);
            fill(bx, oy, bw, 26, hv ? 0x3a322bu : 0x221d19u, 1); frame(bx, oy, bw, 26, hv ? GOLD : 0x3d352cu, 1);
            draw_text(&UI_SMALL, OPTS[k].txt[OPTS[k].cur], bx + bw / 2, oy + 5, TEXT, AL_CENTER, 1);
            draw_text(&UI_SMALL, "<", bx + 10, oy + 4, GOLD_DIM, AL_LEFT, 1);
            draw_text(&UI_SMALL, ">", bx + bw - 10, oy + 4, GOLD_DIM, AL_RIGHT, 1);
            if (hv && (mclick || rclick)) {
                int back = rclick || mx < bx + bw / 3;
                OPTS[k].cur = (OPTS[k].cur + (back ? OPTS[k].n - 1 : 1)) % OPTS[k].n;
            }
        }

        // footer
        if (button("Quit", px, LH - 58, 120, 36, 0, !installing)) result = LAUNCH_QUIT;
        if (button("Play", LW - px - 180, LH - 58, 180, 36, 1, ready && !installing)) result = LAUNCH_PLAY;
        int ho = hover(px + 140, LH - 50, 160, 22);
        draw_text(&UI_SMALL, "Open save folder", px + 140, LH - 50, ho ? GOLD : MUTED, AL_LEFT, 1);
        if (ho && mclick) {
            char url[1200]; snprintf(url, sizeof url, "file://%s%s", save_dir[0] == '/' ? "" : "/", save_dir);
            SDL_CreateDirectory(save_dir); SDL_OpenURL(url);
        }
        // ICARUS_LAUNCHER_SHOT=file.bmp (testing): save the window after ~1 s, then act on
        // ICARUS_LAUNCHER_AUTO (play / quit / install)
        static int frames; const char *shot = getenv("ICARUS_LAUNCHER_SHOT");
        if (shot && ++frames == 60) {
            SDL_Surface *sf = SDL_RenderReadPixels(ren, 0);
            if (sf) { SDL_SaveBMP(sf, shot); SDL_DestroySurface(sf); }
            const char *aa = getenv("ICARUS_LAUNCHER_AUTO");
            if (aa && !strcmp(aa, "play") && ready) result = LAUNCH_PLAY;
            else if (aa && !strcmp(aa, "install") && cd_path[0][0]) { memset(&prog, 0, sizeof prog); installing = 1; SDL_DetachThread(SDL_CreateThread(install_thread, "install", 0)); }
            else if (!aa || !strcmp(aa, "quit")) result = LAUNCH_QUIT;
        }
        if (shot && getenv("ICARUS_LAUNCHER_AUTO") && !strcmp(getenv("ICARUS_LAUNCHER_AUTO"), "install") && frames > 60 && !installing && ready) {
            SDL_Surface *sf = SDL_RenderReadPixels(ren, 0); char p2[1100]; snprintf(p2, sizeof p2, "%s.after.bmp", shot);
            if (sf) { SDL_SaveBMP(sf, p2); SDL_DestroySurface(sf); }
            result = LAUNCH_QUIT;
        }
        SDL_RenderPresent(ren);
    }
    write_config(cfg_path);
    if (result == LAUNCH_PLAY) apply_env();
    for (int k = 0; k < 192; k++) if (cache[k].t) { SDL_DestroyTexture(cache[k].t); cache[k].t = 0; }
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); ren = 0; win = 0;
    return result;
}
