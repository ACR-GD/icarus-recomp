// USER32 / GDI32 emulation on SDL3: one game window, message queue, keyboard/mouse, text output.
#include "win32.h"
#include <SDL3/SDL.h>
#include <time.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>

uint32_t main_hwnd, main_wndproc;
static uint32_t wnd_long[64];
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_w, tex_h;
static int headless;
static int cursor_visible = 1;
static int mouse_x, mouse_y;
static uint8_t keys[256];             // VK state
static uint32_t t0_ms;

void host_sleep_ms(uint32_t ms) { SDL_Delay(ms); }
// Vertical-blank pacing for Flip(DDFLIP_WAIT): the original ran at the monitor refresh rate.
extern int ff_active, logo_skip, title_reached;
void host_frame_pace(void) {
    static uint64_t next; static uint64_t period;
    if (!period) { int fps = getenv("ICARUS_FPS") ? atoi(getenv("ICARUS_FPS")) : 60; period = fps > 0 ? 1000000000ull / (uint64_t)fps : 1; }
    if (logo_skip && !title_reached) { next = 0; return; }
    if (ff_active) {   // fast-forward: 4x the normal rate
        static uint64_t fnext; uint64_t now = SDL_GetTicksNS(), p = period / 4;
        if (fnext > now) SDL_DelayPrecise(fnext - now);
        fnext = (fnext && SDL_GetTicksNS() - fnext < p) ? fnext + p : SDL_GetTicksNS() + p;
        next = 0; return;
    }
    uint64_t now = SDL_GetTicksNS();
    if (next > now) { SDL_DelayPrecise(next - now); now = next; }
    next = (next && now - next < period) ? next + period : now + period;
}
uint32_t w32_tick_ms(void) { return (uint32_t)SDL_GetTicks() - t0_ms + 1000; }

// ---------------------------------------------------------------- message queue
struct msg { uint32_t hwnd, msg, w, l; };
#define MQ 512
static struct msg mq[MQ]; static int mq_r, mq_w;
static void post(uint32_t hwnd, uint32_t m, uint32_t w, uint32_t l) {
    if (((mq_w + 1) & (MQ - 1)) == mq_r) return;
    mq[mq_w] = (struct msg){ hwnd, m, w, l }; mq_w = (mq_w + 1) & (MQ - 1);
}
void w32_post_message(uint32_t hwnd, uint32_t m, uint32_t w, uint32_t l) { post(hwnd, m, w, l); }
static uint32_t send(uint32_t m, uint32_t w, uint32_t l) {
    if (!main_wndproc) return 0;
    uint32_t a[4] = { main_hwnd, m, w, l };
    return guest_call(main_wndproc, 4, a);
}

// ---------------------------------------------------------------- keyboard mapping
static int vk_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + (s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + (s - SDL_SCANCODE_1);
    if (s == SDL_SCANCODE_0) return '0';
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return 0x70 + (s - SDL_SCANCODE_F1);
    switch (s) {
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0d;
    case SDL_SCANCODE_ESCAPE: return 0x1b; case SDL_SCANCODE_BACKSPACE: return 0x08; case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20; case SDL_SCANCODE_LEFT: return 0x25; case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27; case SDL_SCANCODE_DOWN: return 0x28; case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11; case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_PAGEUP: return 0x21; case SDL_SCANCODE_PAGEDOWN: return 0x22; case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_HOME: return 0x24; case SDL_SCANCODE_INSERT: return 0x2d; case SDL_SCANCODE_DELETE: return 0x2e;
    case SDL_SCANCODE_KP_1: return 0x61; case SDL_SCANCODE_KP_2: return 0x62; case SDL_SCANCODE_KP_3: return 0x63;
    case SDL_SCANCODE_KP_4: return 0x64; case SDL_SCANCODE_KP_5: return 0x65; case SDL_SCANCODE_KP_6: return 0x66;
    case SDL_SCANCODE_KP_7: return 0x67; case SDL_SCANCODE_KP_8: return 0x68; case SDL_SCANCODE_KP_9: return 0x69;
    case SDL_SCANCODE_KP_0: return 0x60; case SDL_SCANCODE_PAUSE: return 0x13;
    default: return 0;
    }
}

// ---------------------------------------------------------------- scripted input (headless testing)
// ICARUS_KEYS="t:vk[:ms],..." presses VK at t seconds; ICARUS_CLICKS="t:x:y[:r],..." clicks.
struct sev { double t; int kind, a, b, c; int done; };
static struct sev sevs[256]; static int nsev;
static void parse_script(void) {
    const char *k = getenv("ICARUS_KEYS");
    while (k && *k && nsev < 250) {
        double t; int vk, ms = 150, n = 0;
        if (sscanf(k, "%lf:%i:%i%n", &t, &vk, &ms, &n) < 3) { ms = 150; if (sscanf(k, "%lf:%i%n", &t, &vk, &n) < 2) break; }
        sevs[nsev++] = (struct sev){ t, 0, vk, 1, 0, 0 };
        sevs[nsev++] = (struct sev){ t + ms / 1000.0, 0, vk, 0, 0, 0 };
        k += n; if (*k == ',') k++;
    }
    const char *c = getenv("ICARUS_CLICKS");
    while (c && *c && nsev < 250) {
        double t; int x, y, r = 0, n = 0;
        if (sscanf(c, "%lf:%d:%d:%d%n", &t, &x, &y, &r, &n) < 4) { r = 0; if (sscanf(c, "%lf:%d:%d%n", &t, &x, &y, &n) < 3) break; }
        sevs[nsev++] = (struct sev){ t, 1, x, y, r, 0 };
        sevs[nsev++] = (struct sev){ t + 0.12, 2, x, y, r, 0 };
        c += n; if (*c == ',') c++;
    }
}
static uint32_t mk_mouse(void) { return (uint32_t)(mouse_x & 0xffff) | ((uint32_t)(mouse_y & 0xffff) << 16); }
static uint32_t mk_mbuttons(void) { return (keys[1] ? 1 : 0) | (keys[2] ? 2 : 0) | (keys[0x10] ? 4 : 0) | (keys[0x11] ? 8 : 0); }
static void key_event(int vk, int down) {
    if (!vk) return;
    int was = keys[vk];
    keys[vk] = down ? 0x80 : 0;
    if (down) post(main_hwnd, 0x100, vk, (was ? 0x40000000u : 0) | 1);
    else post(main_hwnd, 0x101, vk, 0xC0000001u);
}
static void mouse_button(int right, int down) {
    keys[right ? 2 : 1] = down ? 0x80 : 0;
    post(main_hwnd, right ? (down ? 0x204 : 0x205) : (down ? 0x201 : 0x202), mk_mbuttons(), mk_mouse());
}
static int skip_logos(void);
static void set_ff(int on);
static void run_script(void) {
    double now = (SDL_GetTicks() - t0_ms) / 1000.0;
    for (int k = 0; k < nsev; k++) {
        struct sev *e = &sevs[k];
        if (e->done || now < e->t) continue;
        e->done = 1;
        if (e->kind == 0 && e->a == 9) { set_ff(e->b); continue; }   // Tab: fast-forward
        if ((e->kind == 0 && e->b) || e->kind == 1) { if (skip_logos()) continue; }
        if (e->kind == 0) key_event(e->a, e->b);
        else { mouse_x = e->a; mouse_y = e->b; post(main_hwnd, 0x200, mk_mbuttons(), mk_mouse()); mouse_button(e->c, e->kind == 1); }
    }
}

static void pump_throttled(void);
// ---------------------------------------------------------------- SDL video
// Presentation: the guest frame is converted to XRGB, uploaded, and drawn letterboxed. Scaling:
//   sharp (default): integer nearest-neighbour upscale into an intermediate texture, then a linear
//                    resize to the window, so pixels stay crisp and evenly sized at any size;
//   nearest / smooth / integer (whole multiples only, black borders) via ICARUS_SCALE_MODE.
// VSync paces Flip when the display refresh is a multiple of 60 Hz; other presents (palette
// changes, movie blits) are throttled to ~60/s so they never stall the game on the GPU.
static int screenshot_every; static int frame_no; static const char *screenshot_path;
enum { SC_SHARP, SC_NEAREST, SC_SMOOTH, SC_INTEGER };
static int scale_mode = SC_SHARP;
static SDL_Texture *mid; static int mid_w, mid_h;
static SDL_FRect dst; static float out_scale = 1, density = 1;
static int vsync_interval;          // 0 = host pacing only
int video_vsync_paces;              // Flip pacing is done by vsync
int video_is_flip;                  // set by DirectDraw around a Flip present
int ff_active;                      // fast-forward (Tab held)
int logo_skip;                      // a key/click during the boot logos
int title_reached;                  // set when the title music starts
static uint64_t last_gpu_ns; static int gpu_pending;
static int cur_w = 640, cur_h = 480;

SDL_Window *video_window(void) { return win; }
float video_points_per_pixel(void) { return out_scale / density; }

void video_init(void) {
    headless = getenv("ICARUS_HEADLESS") && atoi(getenv("ICARUS_HEADLESS"));
    if (getenv("ICARUS_SHOT")) { screenshot_path = getenv("ICARUS_SHOT"); screenshot_every = getenv("ICARUS_SHOT_EVERY") ? atoi(getenv("ICARUS_SHOT_EVERY")) : 60; }
    t0_ms = (uint32_t)SDL_GetTicks();
    parse_script();
    if (headless) return;
    const char *m = getenv("ICARUS_SCALE_MODE");
    if (getenv("ICARUS_SMOOTH")) scale_mode = SC_SMOOTH;
    if (m) scale_mode = !strcmp(m, "nearest") ? SC_NEAREST : !strcmp(m, "smooth") ? SC_SMOOTH : !strcmp(m, "integer") ? SC_INTEGER : SC_SHARP;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) rt_fatal("SDL video init: %s", SDL_GetError());
    int scale = getenv("ICARUS_SCALE") ? atoi(getenv("ICARUS_SCALE")) : 2;
    win = SDL_CreateWindow("Icarus: Sanctuary of the Gods - Recomp by Second Save", 640 * scale, 480 * scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) rt_fatal("SDL window: %s", SDL_GetError());
    SDL_SetWindowMinimumSize(win, 640, 480);
    ren = SDL_CreateRenderer(win, 0);
    // Like on 1998 hardware (Flip waited for the monitor's vertical blank), the game runs at the
    // display refresh: vsync with the interval that puts the frame rate closest to 60-75 fps
    // (60/75/85 Hz -> every refresh, 120/144 Hz -> every second one, 240 Hz -> every fourth).
    // Displays with no such interval fall back to host pacing at 60 fps. ICARUS_FPS forces a rate.
    const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
    float hz = dm && dm->refresh_rate > 0 ? dm->refresh_rate : 60;
    int k = (int)(hz / 67.0f + 0.5f); if (k < 1) k = 1;
    float fps = hz / k;
    if (!getenv("ICARUS_FPS") && !(getenv("ICARUS_VSYNC") && !atoi(getenv("ICARUS_VSYNC"))) && fps >= 55 && fps <= 90 && SDL_SetRenderVSync(ren, k)) {
        vsync_interval = k; video_vsync_paces = 1;
    } else SDL_SetRenderVSync(ren, 0);
    LOG("display %.2f Hz, vsync interval %d\n", hz, vsync_interval);
    if (getenv("ICARUS_FULLSCREEN") && atoi(getenv("ICARUS_FULLSCREEN"))) SDL_SetWindowFullscreen(win, true);
}
void video_shutdown(void) {
    if (mid) SDL_DestroyTexture(mid);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    mid = tex = 0; ren = 0; win = 0;
}
static uint32_t rgba_buf[1024 * 768];
static void write_ppm(const char *path, int w, int h) {
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    static uint8_t rgb[1024 * 768 * 3];
    for (int k = 0; k < w * h; k++) { uint32_t p = rgba_buf[k]; rgb[3 * k] = (uint8_t)(p >> 16); rgb[3 * k + 1] = (uint8_t)(p >> 8); rgb[3 * k + 2] = (uint8_t)p; }
    fwrite(rgb, 3, (size_t)(w * h), f);
    fclose(f);
    rename(tmp, path);
}
// F12: save the current frame as a BMP in screenshots/
static void save_screenshot(void) {
    MKDIR("screenshots");
    char name[128]; time_t t = time(0); struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(name, sizeof name, "screenshots/icarus-%Y%m%d-%H%M%S.bmp", &tmv);
    SDL_Surface *sf = SDL_CreateSurfaceFrom(cur_w, cur_h, SDL_PIXELFORMAT_XRGB8888, rgba_buf, cur_w * 4);
    if (sf && SDL_SaveBMP(sf, name)) fprintf(stderr, "screenshot: %s\n", name);
    SDL_DestroySurface(sf);
}
// ICARUS_STATS=1: once a second print presents/s, flips/s and the longest gap between presents
int stats_flips;
static void stats_tick(void) {
    static int on = -1; static uint64_t t0, last; static int presents; static double worst;
    if (on < 0) on = getenv("ICARUS_STATS") != 0;
    if (!on) return;
    uint64_t now = SDL_GetTicksNS();
    if (last) { double gap = (now - last) / 1e6; if (gap > worst) worst = gap; }
    last = now; presents++;
    if (!t0) t0 = now;
    if (now - t0 >= 1000000000ull) {
        fprintf(stderr, "[stats] %3d presents/s %3d flips/s, worst gap %5.1f ms\n", presents, stats_flips, worst);
        t0 = now; presents = 0; stats_flips = 0; worst = 0;
    }
}
static void layout(int w, int h) {
    int ow, oh; SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    float s = fminf((float)ow / w, (float)oh / h);
    if (scale_mode == SC_INTEGER && s >= 1) s = floorf(s);
    out_scale = s; density = SDL_GetWindowPixelDensity(win); if (density <= 0) density = 1;
    dst.w = w * s; dst.h = h * s; dst.x = floorf((ow - dst.w) / 2); dst.y = floorf((oh - dst.h) / 2);
}
static void gpu_present(void) {
    int w = cur_w, h = cur_h;
    if (!tex || tex_w != w || tex_h != h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(tex, scale_mode == SC_SMOOTH ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        tex_w = w; tex_h = h;
    }
    layout(w, h);
    SDL_UpdateTexture(tex, 0, rgba_buf, w * 4);
    SDL_SetRenderTarget(ren, 0);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    int k = (int)floorf(out_scale);
    if (scale_mode == SC_SHARP && k >= 1 && out_scale - k > 0.01f) {
        if (!mid || mid_w != w * k || mid_h != h * k) {
            if (mid) SDL_DestroyTexture(mid);
            mid = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_TARGET, w * k, h * k);
            SDL_SetTextureScaleMode(mid, SDL_SCALEMODE_LINEAR);
            mid_w = w * k; mid_h = h * k;
        }
        SDL_SetRenderTarget(ren, mid);
        SDL_RenderTexture(ren, tex, 0, 0);
        SDL_SetRenderTarget(ren, 0);
        SDL_RenderTexture(ren, mid, 0, &dst);
    } else SDL_RenderTexture(ren, tex, 0, &dst);
    // ICARUS_DUMP_OUTPUT=file.bmp (testing): save the window contents once, ICARUS_DUMP_AT s in
    static const char *dump = (const char *)1;
    if (dump == (const char *)1) dump = getenv("ICARUS_DUMP_OUTPUT");
    if (dump && SDL_GetTicks() - t0_ms > (uint64_t)(getenv("ICARUS_DUMP_AT") ? atoi(getenv("ICARUS_DUMP_AT")) : 10) * 1000) {
        SDL_Surface *sf = SDL_RenderReadPixels(ren, 0);
        if (sf) { SDL_SaveBMP(sf, dump); SDL_DestroySurface(sf); fprintf(stderr, "dumped output to %s\n", dump); }
        dump = 0;
    }
    SDL_RenderPresent(ren);
    last_gpu_ns = SDL_GetTicksNS(); gpu_pending = 0;
}
void video_present(uint32_t pixels, int pitch, int w, int h, int bpp, const uint32_t *pal) {
    if (w > 1024 || h > 768) return;
    stats_tick();
    for (int y = 0; y < h; y++) {
        const uint8_t *row = GP(pixels + (uint32_t)(y * pitch));
        uint32_t *o = rgba_buf + y * w;
        if (bpp == 8) for (int x = 0; x < w; x++) o[x] = pal[row[x]];
        else if (bpp == 16) for (int x = 0; x < w; x++) {
            uint16_t p = (uint16_t)(row[2 * x] | row[2 * x + 1] << 8);
            uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            o[x] = 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        } else for (int x = 0; x < w; x++) o[x] = 0xff000000u | (row[4 * x + 2] << 16) | (row[4 * x + 1] << 8) | row[4 * x];
    }
    cur_w = w; cur_h = h;
    frame_no++;
    pump_throttled();
    if (screenshot_path && screenshot_every && frame_no % screenshot_every == 0) {
        // a %d in ICARUS_SHOT numbers the files by elapsed tenths of a second
        char p[1100]; snprintf(p, sizeof p, screenshot_path, (int)((SDL_GetTicks() - t0_ms) / 100));
        write_ppm(p, w, h);
    }
    if (headless || !ren) return;
    uint64_t now = SDL_GetTicksNS();
    // flips are frames: show each one (vsync paces them) unless fast-forwarding; anything else is
    // shown at most ~60 times a second
    if ((video_is_flip && !ff_active) || now - last_gpu_ns >= 15000000ull) gpu_present();
    else gpu_pending = 1;
}

// ---------------------------------------------------------------- event pump
static int quit_posted;
void audio_poll(void);
static void to_game(float wx, float wy) {   // window points -> game pixels
    if (!ren) return;
    float px = wx * density, py = wy * density;
    int x = (int)((px - dst.x) / out_scale), y = (int)((py - dst.y) / out_scale);
    mouse_x = x < 0 ? 0 : x >= cur_w ? cur_w - 1 : x;
    mouse_y = y < 0 ? 0 : y >= cur_h ? cur_h - 1 : y;
}
static void set_ff(int on);
void video_on_title(void) { if (title_reached) return; title_reached = 1; if (logo_skip) set_ff(0); }
static void set_ff(int on) {
    if (on == ff_active) return;
    ff_active = on;
    if (ren && vsync_interval) SDL_SetRenderVSync(ren, on ? 0 : vsync_interval);
}
// boot logos: a key or click jumps the clock the logos wait on and drops frame pacing until the
// title screen (its music) starts; the input itself is not passed to the game
static int skip_logos(void) {
    if (title_reached) return 0;
    extern int64_t w32_time_skew; w32_time_skew += 10;
    logo_skip = 1; set_ff(1);
    return 1;
}
void w32_pump_events(void) {
    run_script();
    audio_poll();      // CD track finished -> MM_MCINOTIFY
    SDL_Event e;
    if (headless) {   // still honour SIGINT/SIGTERM (SDL turns them into a quit event)
        while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) { w32_shutdown(); exit(0); }
        return;
    }
    if (gpu_pending && SDL_GetTicksNS() - last_gpu_ns >= 15000000ull) gpu_present();
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            if (!quit_posted) { quit_posted = 1; post(main_hwnd, 0x10 /*WM_CLOSE*/, 0, 0); }
            else { w32_shutdown(); exit(0); }
            break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            int down = e.type == SDL_EVENT_KEY_DOWN;
            SDL_Scancode sc = e.key.scancode;
            if (down && ((sc == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) || sc == SDL_SCANCODE_F11)) {
                if (!e.key.repeat) SDL_SetWindowFullscreen(win, !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                break;
            }
            if (sc == SDL_SCANCODE_F12) { if (down && !e.key.repeat) save_screenshot(); break; }
            if (sc == SDL_SCANCODE_TAB) { set_ff(down); break; }       // hold to fast-forward
            if (down && !e.key.repeat && skip_logos()) break;
            int vk = vk_of(sc);
            if (down && e.key.repeat) { post(main_hwnd, 0x100, vk, 0x40000001u); break; }
            key_event(vk, down);
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST: set_ff(0); break;
        case SDL_EVENT_MOUSE_MOTION: {
            to_game(e.motion.x, e.motion.y);
            post(main_hwnd, 0x200, mk_mbuttons(), mk_mouse());
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
            to_game(e.button.x, e.button.y);
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && skip_logos()) break;
            if (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT)
                mouse_button(e.button.button == SDL_BUTTON_RIGHT, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_GAINED: post(main_hwnd, 0x1c, 1, 0); break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------- USER32
static uint32_t u_RegisterClassA(void) { main_wndproc = RD32(ARG(0) + 4); LOG("RegisterClass wndproc=%08x\n", main_wndproc); return 0xc001; }
static uint32_t u_CreateWindowExA(void) {
    main_hwnd = 0x00010010u;
    video_init();
    uint32_t cs = gmem_alloc(48);
    WR32(cs + 0x0c, ARG(10)); WR32(cs + 0x10, ARG(7)); WR32(cs + 0x14, ARG(6)); WR32(cs + 0x18, ARG(5)); WR32(cs + 0x1c, ARG(4));
    WR32(cs + 0x20, ARG(3)); WR32(cs + 0x24, ARG(2)); WR32(cs + 0x28, ARG(1)); WR32(cs + 0x2c, ARG(0));
    send(0x81 /*WM_NCCREATE*/, 0, cs);
    send(0x01 /*WM_CREATE*/, 0, cs);
    return main_hwnd;
}
static uint32_t u_ShowWindow(void) {
    send(0x18 /*WM_SHOWWINDOW*/, 1, 0);
    send(0x1c /*WM_ACTIVATEAPP*/, 1, 0);
    send(0x06 /*WM_ACTIVATE*/, 1, 0);
    send(0x07 /*WM_SETFOCUS*/, 0, 0);
    return 0;
}
static uint32_t u_UpdateWindow(void) { return 1; }
static uint32_t u_DestroyWindow(void) {
    static int destroying;
    if (!destroying) { destroying = 1; send(0x02 /*WM_DESTROY*/, 0, 0); }
    return 1;
}
static uint32_t u_DefWindowProcA(void) {
    uint32_t m = ARG(1);
    if (m == 0x10) { u_DestroyWindow(); return 0; }
    if (m == 0x81) return 1;
    return 0;
}
static uint32_t u_PostQuitMessage(void) { post(0, 0x12, ARG(0), 0); return 0; }
static uint32_t u_PostMessageA(void) { post(ARG(0), ARG(1), ARG(2), ARG(3)); return 1; }
static void put_msg(uint32_t p, struct msg *m) {
    WR32(p, m->hwnd); WR32(p + 4, m->msg); WR32(p + 8, m->w); WR32(p + 12, m->l); WR32(p + 16, w32_tick_ms());
    WR32(p + 20, (uint32_t)mouse_x); WR32(p + 24, (uint32_t)mouse_y);
}
// ICARUS_SNDTEST="slot:entry,..." (testing): 20 s after start, load DROYSND.SNF entries through
// the game's own on-demand loader (0x444120) and play them (0x4485b0), one per second.
static void sound_test(void) {
    static const char *spec; static int init, idx; static uint32_t next;
    if (!init) { init = 1; spec = getenv("ICARUS_SNDTEST"); next = 20000; }
    if (!spec || !*spec || SDL_GetTicks() - t0_ms < next) return;
    int slot, entry, n = 0;
    if (sscanf(spec, "%d:%d%n", &slot, &entry, &n) != 2) { spec = 0; return; }
    spec += n; if (*spec == ',') spec++;
    next += 1000; idx++;
    uint32_t a1[2] = { (uint32_t)slot, (uint32_t)entry };
    guest_call(0x444120, 2, a1);
    uint32_t a2[3] = { (uint32_t)slot, 0, 0x2b11 };
    uint32_t r = guest_call(0x4485b0, 3, a2);
    fprintf(stderr, "sndtest: slot %d entry %d -> play %u\n", slot, entry, r);
}
static uint32_t u_PeekMessageA(void) {
    static uint32_t last_pump;
    sound_test();
    uint32_t now = w32_tick_ms();
    if (now != last_pump || mq_r == mq_w) { w32_pump_events(); last_pump = now; }
    if (mq_r == mq_w) {
        // a game loop that spins on PeekMessage gets a short breather when nothing happens
        static int spins; if (++spins > 64) { spins = 0; SDL_Delay(1); }
        return 0;
    }
    put_msg(ARG(0), &mq[mq_r]);
    if (ARG(4) & 1) mq_r = (mq_r + 1) & (MQ - 1);
    return 1;
}
static uint32_t u_GetMessageA(void) {
    while (mq_r == mq_w) { w32_pump_events(); if (mq_r == mq_w) SDL_Delay(2); IRQCHK(); }
    struct msg m = mq[mq_r]; mq_r = (mq_r + 1) & (MQ - 1);
    put_msg(ARG(0), &m);
    return m.msg == 0x12 ? 0 : 1;
}
static uint32_t u_TranslateMessage(void) {
    uint32_t p = ARG(0), m = RD32(p + 4), vk = RD32(p + 8);
    if (m == 0x100) {
        int ch = 0;
        if (vk >= 'A' && vk <= 'Z') ch = (keys[0x10] ? vk : vk + 32);
        else if ((vk >= '0' && vk <= '9') || vk == 0x20 || vk == 0x0d || vk == 0x08 || vk == 0x1b) ch = (int)vk;
        if (ch) post(RD32(p), 0x102 /*WM_CHAR*/, (uint32_t)ch, RD32(p + 12));
    }
    return 1;
}
static uint32_t u_DispatchMessageA(void) {
    uint32_t p = ARG(0);
    if (!RD32(p + 4) || RD32(p + 4) == 0x12) return 0;
    uint32_t a[4] = { RD32(p) ? RD32(p) : main_hwnd, RD32(p + 4), RD32(p + 8), RD32(p + 12) };
    return guest_call(main_wndproc, 4, a);
}
// After a display mode change, buttons/keys that were already held read as up until released:
// on 1998 hardware the switch took long enough that the click which caused it was over, and
// otherwise e.g. the NEW GAME click would also skip the intro movie.
static uint8_t latched[256];
void input_latch_held(void) { for (int k = 0; k < 256; k++) if (keys[k]) latched[k] = 1; }
// Pumps host events if the last pump was more than 4 ms ago. Loops that only poll the keyboard
// or only present frames (the movie player) would otherwise never see input or window events.
static void pump_throttled(void) {
    static uint32_t last;
    uint32_t now = (uint32_t)SDL_GetTicks();
    if (now - last >= 4) { last = now; w32_pump_events(); }
}
static uint32_t u_GetAsyncKeyState(void) {
    uint32_t vk = ARG(0) & 0xff;
    pump_throttled();
    if (latched[vk]) { if (!keys[vk]) latched[vk] = 0; return 0; }
    return keys[vk] ? 0x8000 : 0;
}
static uint32_t u_SetCursorPos(void) {
    mouse_x = (int)ARG(0); mouse_y = (int)ARG(1);
    if (win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) && getenv("ICARUS_WARP")) {
        SDL_WarpMouseInWindow(win, (dst.x + (mouse_x + 0.5f) * out_scale) / density, (dst.y + (mouse_y + 0.5f) * out_scale) / density);
    }
    return 1;
}
int hwcursor_active(void);
static uint32_t u_SetCursor(void) {
    if (hwcursor_active()) return 0;     // the host cursor follows the game's cursor sprite instead
    int vis = ARG(0) != 0;
    if (vis != cursor_visible && !headless) { if (vis) SDL_ShowCursor(); else SDL_HideCursor(); }
    cursor_visible = vis;
    return 0;
}
static uint32_t u_GetClientRect(void) { uint32_t r = ARG(1); WR32(r, 0); WR32(r + 4, 0); WR32(r + 8, 640); WR32(r + 12, 480); return 1; }
static uint32_t u_ClientToScreen(void) { return 1; }
static uint32_t u_one(void) { return 1; }
static uint32_t u_zero(void) { return 0; }
static uint32_t u_GetWindowLongA(void) { int32_t i = (int32_t)ARG(1); if (i == -4) return main_wndproc; return (i >= -64 && i < 0) ? wnd_long[-i] : 0; }
static uint32_t u_SetWindowLongA(void) {
    int32_t i = (int32_t)ARG(1); uint32_t old = u_GetWindowLongA();
    if (i == -4) main_wndproc = ARG(2); else if (i >= -64 && i < 0) wnd_long[-i] = ARG(2);
    return old;
}
static uint32_t u_SetRect(void) { uint32_t r = ARG(0); WR32(r, ARG(1)); WR32(r + 4, ARG(2)); WR32(r + 8, ARG(3)); WR32(r + 12, ARG(4)); return 1; }
static uint32_t u_LoadIconA(void) { return 0x7701; }
static uint32_t u_LoadCursorA(void) { return 0x7702; }
static uint32_t u_MessageBoxA(void) {
    char t[1024], c[256]; gstr_get(ARG(1), t, sizeof t); gstr_get(ARG(2), c, sizeof c);
    fprintf(stderr, "MessageBox [%s]: %s\n", c, t);
    if (win) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, c, t, win);
    return 1;
}
static uint32_t u_GetDC(void) { return 0xdc000000u; }   // screen DC
static uint32_t u_ReleaseDC(void) { return 1; }

// wsprintf: Windows format subset (%d %u %x %X %s %c %ld %lu %lx, flags/width/precision)
static int w_format(char *out, int cap, uint32_t fmt, uint32_t args) {
    int o = 0;
    for (uint32_t p = fmt;; p++) {
        char c = (char)RD8(p);
        if (!c) break;
        if (c != '%') { if (o < cap - 1) out[o++] = c; continue; }
        char spec[32]; int s = 0; spec[s++] = '%';
        c = (char)RD8(++p);
        while (c && strchr("-+ #0123456789.", c) && s < 20) { spec[s++] = c; c = (char)RD8(++p); }
        while (c == 'l' || c == 'h') c = (char)RD8(++p);
        if (!c) break;
        char buf[1024];
        if (c == 's') { char str[1024]; gstr_get(RD32(args), str, sizeof str); args += 4; spec[s++] = 's'; spec[s] = 0; snprintf(buf, sizeof buf, spec, str); }
        else if (c == 'c') { spec[s++] = 'c'; spec[s] = 0; snprintf(buf, sizeof buf, spec, (int)RD32(args)); args += 4; }
        else if (c == 'd' || c == 'i') { spec[s++] = 'd'; spec[s] = 0; snprintf(buf, sizeof buf, spec, (int32_t)RD32(args)); args += 4; }
        else if (c == 'u' || c == 'x' || c == 'X') { spec[s++] = c; spec[s] = 0; snprintf(buf, sizeof buf, spec, RD32(args)); args += 4; }
        else { buf[0] = c; buf[1] = 0; }
        for (char *b = buf; *b && o < cap - 1; b++) out[o++] = *b;
    }
    out[o] = 0;
    return o;
}
static uint32_t u_wsprintfA(void) { char b[1025]; int n = w_format(b, 1025, ARG(1), ESP + 12); gstr_put(ARG(0), b); return (uint32_t)n; }
static uint32_t u_wvsprintfA(void) { char b[1025]; int n = w_format(b, 1025, ARG(1), ARG(2)); gstr_put(ARG(0), b); return (uint32_t)n; }

#define A(n, f, b) { #n, f, b }
const struct api API_USER32[] = {
    A(AdjustWindowRectEx, u_one, 16), A(MoveWindow, u_one, 24), A(wsprintfA, u_wsprintfA, -1), A(DestroyWindow, u_DestroyWindow, 4),
    A(MessageBoxA, u_MessageBoxA, 16), A(SetCursorPos, u_SetCursorPos, 8), A(wvsprintfA, u_wvsprintfA, 12),
    A(ClientToScreen, u_ClientToScreen, 8), A(GetClientRect, u_GetClientRect, 8), A(GetWindowRect, u_GetClientRect, 8),
    A(ReleaseDC, u_ReleaseDC, 8), A(GetWindowLongA, u_GetWindowLongA, 8), A(SetCursor, u_SetCursor, 4),
    A(PostMessageA, u_PostMessageA, 16), A(DefWindowProcA, u_DefWindowProcA, 16), A(PostQuitMessage, u_PostQuitMessage, 4),
    A(GetAsyncKeyState, u_GetAsyncKeyState, 4), A(GetMessageA, u_GetMessageA, 16), A(DispatchMessageA, u_DispatchMessageA, 4),
    A(TranslateMessage, u_TranslateMessage, 4), A(SystemParametersInfoA, u_one, 16), A(SetWindowPos, u_one, 28),
    A(LoadIconA, u_LoadIconA, 8), A(GetMenu, u_zero, 4), A(SetRect, u_SetRect, 20), A(SetWindowLongA, u_SetWindowLongA, 12),
    A(PeekMessageA, u_PeekMessageA, 20), A(GetDC, u_GetDC, 4), A(UpdateWindow, u_UpdateWindow, 4), A(ShowWindow, u_ShowWindow, 8),
    A(CreateWindowExA, u_CreateWindowExA, 48), A(RegisterClassA, u_RegisterClassA, 4), A(LoadCursorA, u_LoadCursorA, 8),
};
const int N_USER32 = sizeof API_USER32 / sizeof API_USER32[0];

// ---------------------------------------------------------------- GDI32
// Device contexts: 0xdc000000 = screen, 0xdc000000 | surface id = DirectDraw surface DC.
struct font { int height; int used; };
static struct font fonts[64];
static struct { uint32_t font, color, bkmode; } dcs[256];
uint32_t gdi_dc_surface(uint32_t hdc);   // w32_ddraw.c: surface pixels for a DC
int gdi_dc_info(uint32_t hdc, uint32_t *pixels, int *pitch, int *w, int *h, int *bpp);
static int dci(uint32_t hdc) { return (int)(hdc & 0xff); }
static uint32_t g_CreateFontA(void) {
    for (int k = 1; k < 64; k++) if (!fonts[k].used) { fonts[k].used = 1; fonts[k].height = (int)ARG(0); return 0x6f000000u | k; }
    return 0;
}
static uint32_t g_DeleteObject(void) { uint32_t h = ARG(0); if ((h & 0xff000000u) == 0x6f000000u) fonts[h & 63].used = 0; return 1; }
static uint32_t g_SelectObject(void) { int d = dci(ARG(0)); uint32_t old = dcs[d].font; if ((ARG(1) & 0xff000000u) == 0x6f000000u) dcs[d].font = ARG(1); return old ? old : 0x6f000000u; }
static uint32_t g_SetBkMode(void) { int d = dci(ARG(0)); uint32_t o = dcs[d].bkmode; dcs[d].bkmode = ARG(1); return o ? o : 2; }
static uint32_t g_SetTextColor(void) { int d = dci(ARG(0)); uint32_t o = dcs[d].color; dcs[d].color = ARG(1); return o; }
static uint32_t g_TextOutA(void) {
    uint32_t hdc = ARG(0); int x = (int)ARG(1), y = (int)ARG(2); int n = (int)ARG(4);
    char s[512]; if (n > 511) n = 511; memcpy(s, GP(ARG(3)), n); s[n] = 0;
    uint32_t px; int pitch, w, h, bpp;
    if (!gdi_dc_info(hdc, &px, &pitch, &w, &h, &bpp)) return 1;
    int d = dci(hdc);
    int fh = dcs[d].font ? fonts[dcs[d].font & 63].height : 16;
    gdi_draw_text(px, pitch, w, h, bpp, x, y, s, n, dcs[d].color, fh < 0 ? -fh : fh);
    return 1;
}
static uint32_t g_GetDeviceCaps(void) {
    switch (ARG(1)) {
    case 8: return 640; case 10: return 480; case 12: return 8; case 14: return 1;
    case 24: return 256; case 38: return 0x100 | 0x7e99; case 104: return 256; case 106: return 20; case 108: return 18;
    default: return 0;
    }
}
static uint32_t g_GetSystemPaletteEntries(void) {
    uint32_t start = ARG(1), n = ARG(2), p = ARG(3);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = start + k; uint8_t r = 0, g = 0, b = 0;
        if (i < 10 || i >= 246) { static const uint8_t sys[20][3] = { {0,0,0},{128,0,0},{0,128,0},{128,128,0},{0,0,128},{128,0,128},{0,128,128},{192,192,192},{192,220,192},{166,202,240},
                                                                     {255,251,240},{160,160,164},{128,128,128},{255,0,0},{0,255,0},{255,255,0},{0,0,255},{255,0,255},{0,255,255},{255,255,255} };
                                  int j = i < 10 ? (int)i : (int)i - 236; r = sys[j][0]; g = sys[j][1]; b = sys[j][2]; }
        if (p) { WR8(p + 4 * k, r); WR8(p + 4 * k + 1, g); WR8(p + 4 * k + 2, b); WR8(p + 4 * k + 3, 0); }
    }
    return n;
}
static uint32_t g_GetStockObject(void) { return 0x5f000000u | ARG(0); }
const struct api API_GDI32[] = {
    A(CreateFontA, g_CreateFontA, 56), A(DeleteObject, g_DeleteObject, 4), A(TextOutA, g_TextOutA, 20), A(SetBkMode, g_SetBkMode, 8),
    A(SetTextColor, g_SetTextColor, 8), A(SelectObject, g_SelectObject, 8), A(GetDeviceCaps, g_GetDeviceCaps, 8),
    A(GetStockObject, g_GetStockObject, 4), A(GetSystemPaletteEntries, g_GetSystemPaletteEntries, 16),
};
const int N_GDI32 = sizeof API_GDI32 / sizeof API_GDI32[0];
