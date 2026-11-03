// DirectDraw (DX3/DX5 interfaces) on guest memory surfaces; the primary surface is shown through SDL.
#include "win32.h"

#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define DDERR_NOTFOUND 0x887600ffu
#define E_NOINTERFACE 0x80004002u

struct surf {
    int used, w, h, bpp, pitch;
    uint32_t pixels;           // guest address
    uint32_t caps;
    uint32_t ck_lo, ck_hi; int has_ck;
    int pal;                   // palette index or -1
    int back;                  // attached back buffer surface index or -1
    uint32_t obj;              // guest COM object
    int refs;
};
#define NS 1024
static struct surf S_[NS];
struct pal { int used; uint32_t rgb[256]; uint32_t obj; };
static struct pal P_[64];
static uint32_t vt_dd, vt_dd2, vt_surf, vt_surf2, vt_surf3, vt_pal, vt_clip;
static int mode_w = 640, mode_h = 480, mode_bpp = 8;
static int primary = -1;
static uint32_t dd_obj, dd2_obj;
static int dirty;

#define THIS ARG(0)
static int sidx(uint32_t obj) { return (int)RD32(obj + 4); }
static struct surf *SF(uint32_t obj) { int i = sidx(obj); if (i < 0 || i >= NS || !S_[i].used) rt_fatal("bad surface object %08x", obj); return &S_[i]; }

static int new_surface(int w, int h, int bpp, uint32_t caps) {
    for (int k = 1; k < NS; k++) if (!S_[k].used) {
        struct surf *s = &S_[k];
        memset(s, 0, sizeof *s);
        s->used = 1; s->w = w; s->h = h; s->bpp = bpp; s->pitch = ((w * bpp / 8) + 3) & ~3; s->caps = caps;
        s->pixels = gmem_alloc((uint32_t)(s->pitch * h + 64));
        if (!s->pixels) rt_fatal("out of guest memory for a %dx%d surface", w, h);
        s->pal = -1; s->back = -1; s->refs = 1;
        s->obj = w32_new_object(vt_surf, 16); WR32(s->obj + 4, (uint32_t)k);
        return k;
    }
    rt_fatal("too many surfaces");
}

// ---------------------------------------------------------------- presentation
uint32_t ddraw_primary_pixels(int *pitch, int *w, int *h) {
    if (primary < 0) return 0;
    *pitch = S_[primary].pitch; *w = S_[primary].w; *h = S_[primary].h;
    return S_[primary].pixels;
}
static uint32_t default_pal[256];
static void present(void) {
    if (primary < 0) return;
    struct surf *p = &S_[primary];
    const uint32_t *pal = p->pal >= 0 ? P_[p->pal].rgb : default_pal;
    video_present(p->pixels, p->pitch, p->w, p->h, p->bpp, pal);
    dirty = 0;
}
// palette the screen is shown with, and a counter that changes whenever its colours change
static uint32_t pal_gen;
const uint32_t *ddraw_screen_palette(uint32_t *gen) {
    if (primary < 0) return 0;
    *gen = pal_gen ^ ((uint32_t)S_[primary].pal << 24);
    return S_[primary].pal >= 0 ? P_[S_[primary].pal].rgb : default_pal;
}
// surface whose pixel memory contains addr (for clipping text drawn into it)
int ddraw_find_surface(uint32_t addr, uint32_t *base, uint32_t *end) {
    for (int k = 1; k < NS; k++) if (S_[k].used) {
        uint32_t b = S_[k].pixels, e = b + (uint32_t)(S_[k].pitch * S_[k].h);
        if (addr >= b && addr < e) { *base = b; *end = e; return 1; }
    }
    return 0;
}
// called from PeekMessage-level code paths too, so that games drawing straight into the
// primary surface without Flip still get shown
void ddraw_present_if_dirty(void) { if (dirty) present(); }

// ---------------------------------------------------------------- IUnknown
static uint32_t c_AddRef(void) { return 2; }
static uint32_t c_Release(void) { return 0; }

// ---------------------------------------------------------------- IDirectDraw
static const uint8_t IID_IDirectDraw2[16] = { 0x80,0xa5,0xdb,0xb3,0x45,0x2d,0xcf,0x11,0xa2,0xc3,0x00,0x20,0xaf,0x0f,0x41,0x41 };
static uint32_t dd_QueryInterface(void) {
    uint32_t iid = ARG(1);
    if (!memcmp(GP(iid), IID_IDirectDraw2, 16)) { WR32(ARG(2), dd2_obj); return DD_OK; }
    LOG("IDirectDraw::QueryInterface unknown iid %08x\n", RD32(iid));
    WR32(ARG(2), dd_obj); return DD_OK;
}
static uint32_t dd_SetCooperativeLevel(void) { return DD_OK; }
void input_latch_held(void);
static uint32_t dd_SetDisplayMode(void) {
    input_latch_held();
    mode_w = (int)ARG(1); mode_h = (int)ARG(2); mode_bpp = (int)ARG(3);
    fprintf(stderr, "SetDisplayMode %dx%dx%d\n", mode_w, mode_h, mode_bpp);
    return DD_OK;
}
static uint32_t dd_RestoreDisplayMode(void) { return DD_OK; }
static void put_pixfmt(uint32_t p, int bpp) {
    memset(GP(p), 0, 32);
    WR32(p, 32);
    if (bpp == 8) { WR32(p + 4, 0x60 /*RGB|PALETTEINDEXED8*/); WR32(p + 12, 8); }
    else if (bpp == 16) { WR32(p + 4, 0x40); WR32(p + 12, 16); WR32(p + 16, 0xf800); WR32(p + 20, 0x07e0); WR32(p + 24, 0x001f); }
    else { WR32(p + 4, 0x40); WR32(p + 12, (uint32_t)bpp); WR32(p + 16, 0xff0000); WR32(p + 20, 0xff00); WR32(p + 24, 0xff); }
}
static void put_desc(uint32_t d, struct surf *s) {
    memset(GP(d + 4), 0, 104);
    WR32(d, 108);
    WR32(d + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (s->pixels ? 0x800 : 0));
    WR32(d + 8, (uint32_t)s->h); WR32(d + 12, (uint32_t)s->w); WR32(d + 16, (uint32_t)s->pitch);
    WR32(d + 36, s->pixels);
    if (s->has_ck) { WR32(d + 64, s->ck_lo); WR32(d + 68, s->ck_hi); WR32(d + 4, RD32(d + 4) | 0x10000); }
    put_pixfmt(d + 72, s->bpp);
    WR32(d + 104, s->caps);
}
static uint32_t dd_CreateSurface(void) {
    uint32_t d = ARG(1), out = ARG(2);
    uint32_t flags = RD32(d + 4), caps = RD32(d + 104);
    int w = (flags & 4) ? (int)RD32(d + 12) : mode_w, h = (flags & 2) ? (int)RD32(d + 8) : mode_h;
    int bpp = (flags & 0x1000) ? (int)RD32(d + 72 + 12) : mode_bpp;
    if (caps & 0x200 /*PRIMARY*/) { w = mode_w; h = mode_h; bpp = mode_bpp; }
    int k = new_surface(w, h, bpp, caps | ((caps & 0x200) ? 0x4000 : 0));
    if (flags & 0x10000) { S_[k].has_ck = 1; S_[k].ck_lo = RD32(d + 64); S_[k].ck_hi = RD32(d + 68); }
    if (caps & 0x200) {
        primary = k;
        S_[k].caps |= 0x2000 /*VISIBLE*/ | 0x8000 /*FRONTBUFFER*/;
        if ((flags & 0x20) && RD32(d + 20) > 0) {
            int b = new_surface(w, h, bpp, 0x4 | 0x10 | 0x4000 | 0x8);
            S_[k].back = b;
        }
    }
    LOG("CreateSurface %dx%d %dbpp caps=%x -> #%d\n", w, h, bpp, caps, k);
    WR32(out, S_[k].obj);
    return DD_OK;
}
static uint32_t dd_CreatePalette(void) {
    uint32_t ents = ARG(2), out = ARG(3);
    for (int k = 1; k < 64; k++) if (!P_[k].used) {
        P_[k].used = 1;
        for (int i = 0; i < 256; i++) P_[k].rgb[i] = 0xff000000u | (RD8(ents + 4 * i) << 16) | (RD8(ents + 4 * i + 1) << 8) | RD8(ents + 4 * i + 2);
        P_[k].obj = w32_new_object(vt_pal, 8); WR32(P_[k].obj + 4, (uint32_t)k);
        WR32(out, P_[k].obj);
        return DD_OK;
    }
    return DDERR_GENERIC;
}
static uint32_t dd_CreateClipper(void) { WR32(ARG(2), w32_new_object(vt_clip, 8)); return DD_OK; }
static void put_caps(uint32_t c) {
    uint32_t n = RD32(c); if (n < 8 || n > 400) n = 316;
    memset(GP(c + 4), 0, n - 4);
    WR32(c + 4, 0x40 | 0x1 /*3D|BLT*/ | 0x200 /*BLTCOLORFILL*/ | 0x80000 /*COLORKEY*/ | 0x10000000 /*BLTSTRETCH*/);
    WR32(c + 0x3c, 32u << 20); WR32(c + 0x40, 32u << 20);   // video memory total/free
}
static uint32_t dd_GetCaps(void) { if (ARG(1)) put_caps(ARG(1)); if (ARG(2)) put_caps(ARG(2)); return DD_OK; }
static uint32_t dd_GetDisplayMode(void) {
    struct surf t = { 0 }; t.w = mode_w; t.h = mode_h; t.bpp = mode_bpp; t.pitch = mode_w * mode_bpp / 8;
    put_desc(ARG(1), &t); return DD_OK;
}
static uint32_t dd_EnumDisplayModes(void) {
    uint32_t cb = ARG(4), ctx = ARG(3);
    static const int modes[][3] = { {640,480,8}, {640,480,16}, {800,600,8}, {800,600,16} };
    uint32_t d = gmem_alloc(108);
    for (int k = 0; k < 4; k++) {
        struct surf t = { 0 }; t.w = modes[k][0]; t.h = modes[k][1]; t.bpp = modes[k][2]; t.pitch = t.w * t.bpp / 8;
        put_desc(d, &t);
        uint32_t a[2] = { d, ctx };
        if (!guest_call(cb, 2, a)) break;
    }
    gmem_free(d);
    return DD_OK;
}
static uint32_t dd_WaitForVerticalBlank(void) { return DD_OK; }
static uint32_t dd_GetMonitorFrequency(void) { WR32(ARG(1), 60); return DD_OK; }
static uint32_t dd_GetAvailableVidMem(void) { if (ARG(2)) WR32(ARG(2), 32u << 20); if (ARG(3)) WR32(ARG(3), 32u << 20); return DD_OK; }
static uint32_t dd_ok(void) { return DD_OK; }

// ---------------------------------------------------------------- IDirectDrawSurface
static uint32_t s_QueryInterface(void) { WR32(ARG(2), THIS); return DD_OK; }
static uint32_t s_Release(void) {
    struct surf *s = SF(THIS);
    if (--s->refs > 0) return (uint32_t)s->refs;
    int k = sidx(THIS);
    if (k == primary) return 0;    // keep the screen alive for the presenter
    gmem_free(s->pixels); s->used = 0;
    return 0;
}
static uint32_t s_AddRef(void) { return (uint32_t)++SF(THIS)->refs; }
static uint32_t s_GetAttachedSurface(void) {
    struct surf *s = SF(THIS);
    if (s->back < 0) return DDERR_NOTFOUND;
    WR32(ARG(2), S_[s->back].obj);
    return DD_OK;
}
extern int stats_flips, video_vsync_paces, video_is_flip, ff_active;
void cursor_frame_end(void);
static uint32_t s_Flip(void) {
    struct surf *s = SF(THIS);
    stats_flips++;
    if (!video_vsync_paces || ff_active) host_frame_pace();   // otherwise the vsync'd present paces
    cursor_frame_end();
    if (s->back >= 0) { struct surf *b = &S_[s->back]; uint32_t t = s->pixels; s->pixels = b->pixels; b->pixels = t; }
    video_is_flip = 1; present(); video_is_flip = 0;
    return DD_OK;
}
static uint32_t s_GetSurfaceDesc(void) { put_desc(ARG(1), SF(THIS)); return DD_OK; }
static uint32_t s_GetPixelFormat(void) { put_pixfmt(ARG(1), SF(THIS)->bpp); return DD_OK; }
static uint32_t s_GetCaps(void) { WR32(ARG(1), SF(THIS)->caps); return DD_OK; }
static uint32_t s_Lock(void) {
    struct surf *s = SF(THIS);
    uint32_t r = ARG(1), d = ARG(2);
    put_desc(d, s);
    if (r) WR32(d + 36, s->pixels + RD32(r + 4) * (uint32_t)s->pitch + RD32(r) * (uint32_t)(s->bpp / 8));
    return DD_OK;
}
static uint32_t s_Unlock(void) { if (sidx(THIS) == primary) { dirty = 1; present(); } return DD_OK; }
static uint32_t s_IsLost(void) { return DD_OK; }
static uint32_t s_Restore(void) { return DD_OK; }
static uint32_t s_SetPalette(void) {
    struct surf *s = SF(THIS);
    s->pal = ARG(1) ? (int)RD32(ARG(1) + 4) : -1;
    if (sidx(THIS) == primary) dirty = 1;
    return DD_OK;
}
static uint32_t s_GetPalette(void) { struct surf *s = SF(THIS); if (s->pal < 0) return 0x887600ff; WR32(ARG(1), P_[s->pal].obj); return DD_OK; }
static uint32_t s_SetColorKey(void) {
    struct surf *s = SF(THIS);
    if (ARG(1) & 8 /*SRCBLT*/) { if (ARG(2)) { s->has_ck = 1; s->ck_lo = RD32(ARG(2)); s->ck_hi = RD32(ARG(2) + 4); } else s->has_ck = 0; }
    return DD_OK;
}
static uint32_t s_GetColorKey(void) { struct surf *s = SF(THIS); WR32(ARG(2), s->ck_lo); WR32(ARG(2) + 4, s->ck_hi); return s->has_ck ? DD_OK : 0x88760104u; }
static uint32_t s_SetClipper(void) { return DD_OK; }
static uint32_t s_GetBltStatus(void) { return DD_OK; }
static uint32_t s_GetFlipStatus(void) { return DD_OK; }

struct rect { int l, t, r, b; };
static struct rect get_rect(uint32_t p, struct surf *s) {
    if (!p) return (struct rect){ 0, 0, s->w, s->h };
    return (struct rect){ (int32_t)RD32(p), (int32_t)RD32(p + 4), (int32_t)RD32(p + 8), (int32_t)RD32(p + 12) };
}
static inline uint32_t px_get(struct surf *s, int x, int y) {
    uint32_t a = s->pixels + (uint32_t)(y * s->pitch);
    switch (s->bpp) { case 8: return RD8(a + x); case 16: return RD16(a + 2 * x); default: return RD32(a + 4 * x) & 0xffffff; }
}
static inline void px_put(struct surf *s, int x, int y, uint32_t v) {
    uint32_t a = s->pixels + (uint32_t)(y * s->pitch);
    switch (s->bpp) { case 8: WR8(a + x, v); break; case 16: WR16(a + 2 * x, v); break; default: WR32(a + 4 * x, v); }
}
// generic blit with clipping, optional stretch and source color key
static void blit(struct surf *d, struct rect dr, struct surf *s, struct rect sr, int key, uint32_t klo, uint32_t khi) {
    int dw = dr.r - dr.l, dh = dr.b - dr.t, sw = sr.r - sr.l, shh = sr.b - sr.t;
    if (dw <= 0 || dh <= 0 || sw <= 0 || shh <= 0) return;
    int bytes = d->bpp / 8;
    if (dw == sw && dh == shh && !key && s->bpp == d->bpp) {
        // clip
        int x0 = dr.l, y0 = dr.t, sx0 = sr.l, sy0 = sr.t, w = dw, h = dh;
        if (x0 < 0) { sx0 -= x0; w += x0; x0 = 0; } if (y0 < 0) { sy0 -= y0; h += y0; y0 = 0; }
        if (sx0 < 0) { x0 -= sx0; w += sx0; sx0 = 0; } if (sy0 < 0) { y0 -= sy0; h += sy0; sy0 = 0; }
        if (x0 + w > d->w) w = d->w - x0; if (y0 + h > d->h) h = d->h - y0;
        if (sx0 + w > s->w) w = s->w - sx0; if (sy0 + h > s->h) h = s->h - sy0;
        if (w <= 0 || h <= 0) return;
        for (int y = 0; y < h; y++)
            memmove(GP(d->pixels + (uint32_t)((y0 + y) * d->pitch + x0 * bytes)), GP(s->pixels + (uint32_t)((sy0 + y) * s->pitch + sx0 * bytes)), (size_t)(w * bytes));
        return;
    }
    for (int y = 0; y < dh; y++) {
        int dy = dr.t + y; if (dy < 0 || dy >= d->h) continue;
        int sy = sr.t + (int)((int64_t)y * shh / dh); if (sy < 0 || sy >= s->h) continue;
        for (int x = 0; x < dw; x++) {
            int dx = dr.l + x; if (dx < 0 || dx >= d->w) continue;
            int sx = sr.l + (int)((int64_t)x * sw / dw); if (sx < 0 || sx >= s->w) continue;
            uint32_t v = px_get(s, sx, sy);
            if (key && v >= klo && v <= khi) continue;
            px_put(d, dx, dy, v);
        }
    }
}
static void fill(struct surf *d, struct rect r, uint32_t c) {
    if (r.l < 0) r.l = 0; if (r.t < 0) r.t = 0; if (r.r > d->w) r.r = d->w; if (r.b > d->h) r.b = d->h;
    for (int y = r.t; y < r.b; y++) {
        if (d->bpp == 8) memset(GP(d->pixels + (uint32_t)(y * d->pitch + r.l)), (int)(c & 0xff), (size_t)(r.r - r.l));
        else for (int x = r.l; x < r.r; x++) px_put(d, x, y, c);
    }
}
static uint32_t s_Blt(void) {
    struct surf *d = SF(THIS);
    uint32_t flags = ARG(4), fx = ARG(5);
    struct rect dr = get_rect(ARG(1), d);
    if (flags & 0x400 /*COLORFILL*/) fill(d, dr, RD32(fx + 80));
    else if (ARG(2)) {
        struct surf *s = SF(ARG(2));
        struct rect sr = get_rect(ARG(3), s);
        int key = 0; uint32_t lo = 0, hi = 0;
        if ((flags & 0x8000 /*KEYSRC*/) && s->has_ck) { key = 1; lo = s->ck_lo; hi = s->ck_hi; }
        if (flags & 0x10000 /*KEYSRCOVERRIDE*/) { key = 1; lo = RD32(fx + 92); hi = RD32(fx + 96); }
        blit(d, dr, s, sr, key, lo, hi);
    }
    if (sidx(THIS) == primary) { dirty = 1; present(); }
    return DD_OK;
}
static uint32_t s_BltFast(void) {
    struct surf *d = SF(THIS), *s = SF(ARG(3));
    int x = (int)ARG(1), y = (int)ARG(2);
    struct rect sr = get_rect(ARG(4), s);
    struct rect dr = { x, y, x + sr.r - sr.l, y + sr.b - sr.t };
    int key = (ARG(5) & 1) && s->has_ck;
    blit(d, dr, s, sr, key, s->ck_lo, s->ck_hi);
    if (sidx(THIS) == primary) { dirty = 1; present(); }
    return DD_OK;
}
static uint32_t s_GetDC(void) { WR32(ARG(1), 0xdc000000u | (uint32_t)sidx(THIS) << 8 | 0x80); return DD_OK; }
static uint32_t s_ReleaseDC(void) { if (sidx(THIS) == primary) { dirty = 1; present(); } return DD_OK; }
int gdi_dc_info(uint32_t hdc, uint32_t *pixels, int *pitch, int *w, int *h, int *bpp) {
    int k = (int)((hdc >> 8) & 0xffff);
    if (hdc == 0xdc000000u) k = primary;
    if (k <= 0 || k >= NS || !S_[k].used) return 0;
    *pixels = S_[k].pixels; *pitch = S_[k].pitch; *w = S_[k].w; *h = S_[k].h; *bpp = S_[k].bpp;
    return 1;
}

// ---------------------------------------------------------------- palette
static struct pal *PL(uint32_t obj) { return &P_[RD32(obj + 4) & 63]; }
static uint32_t p_SetEntries(void) {
    struct pal *p = PL(THIS); uint32_t start = ARG(2), n = ARG(3), e = ARG(4);
    for (uint32_t k = 0; k < n && start + k < 256; k++)
        p->rgb[start + k] = 0xff000000u | (RD8(e + 4 * k) << 16) | (RD8(e + 4 * k + 1) << 8) | RD8(e + 4 * k + 2);
    pal_gen++;
    // a palette change on screen is a displayed frame: pace it like a flip (title screen loop)
    if (primary >= 0 && S_[primary].pal == (int)(p - P_)) { host_frame_pace(); dirty = 1; present(); }
    return DD_OK;
}
static uint32_t p_GetEntries(void) {
    struct pal *p = PL(THIS); uint32_t start = ARG(2), n = ARG(3), e = ARG(4);
    for (uint32_t k = 0; k < n && start + k < 256; k++) {
        uint32_t c = p->rgb[start + k];
        WR8(e + 4 * k, c >> 16); WR8(e + 4 * k + 1, c >> 8); WR8(e + 4 * k + 2, c); WR8(e + 4 * k + 3, 0);
    }
    return DD_OK;
}
static uint32_t p_GetCaps(void) { WR32(ARG(1), 0x4 | 0x40 /*8BIT|ALLOW256*/); return DD_OK; }

// ---------------------------------------------------------------- vtables
#define M(n, f, b) { #n, f, b }
static const struct api DD_METHODS[] = {
    M(QueryInterface, dd_QueryInterface, 12), M(AddRef, c_AddRef, 4), M(Release, c_Release, 4), M(Compact, dd_ok, 4),
    M(CreateClipper, dd_CreateClipper, 16), M(CreatePalette, dd_CreatePalette, 20), M(CreateSurface, dd_CreateSurface, 16),
    M(DuplicateSurface, 0, 12), M(EnumDisplayModes, dd_EnumDisplayModes, 20), M(EnumSurfaces, 0, 20),
    M(FlipToGDISurface, dd_ok, 4), M(GetCaps, dd_GetCaps, 12), M(GetDisplayMode, dd_GetDisplayMode, 8),
    M(GetFourCCCodes, 0, 12), M(GetGDISurface, 0, 8), M(GetMonitorFrequency, dd_GetMonitorFrequency, 8),
    M(GetScanLine, 0, 8), M(GetVerticalBlankStatus, 0, 8), M(Initialize, dd_ok, 8), M(RestoreDisplayMode, dd_RestoreDisplayMode, 4),
    M(SetCooperativeLevel, dd_SetCooperativeLevel, 12), M(SetDisplayMode, dd_SetDisplayMode, 16),
    M(WaitForVerticalBlank, dd_WaitForVerticalBlank, 12),
};
static uint32_t dd2_SetDisplayMode(void) { return dd_SetDisplayMode(); }
static const struct api DD2_METHODS[] = {
    M(QueryInterface, dd_QueryInterface, 12), M(AddRef, c_AddRef, 4), M(Release, c_Release, 4), M(Compact, dd_ok, 4),
    M(CreateClipper, dd_CreateClipper, 16), M(CreatePalette, dd_CreatePalette, 20), M(CreateSurface, dd_CreateSurface, 16),
    M(DuplicateSurface, 0, 12), M(EnumDisplayModes, dd_EnumDisplayModes, 20), M(EnumSurfaces, 0, 20),
    M(FlipToGDISurface, dd_ok, 4), M(GetCaps, dd_GetCaps, 12), M(GetDisplayMode, dd_GetDisplayMode, 8),
    M(GetFourCCCodes, 0, 12), M(GetGDISurface, 0, 8), M(GetMonitorFrequency, dd_GetMonitorFrequency, 8),
    M(GetScanLine, 0, 8), M(GetVerticalBlankStatus, 0, 8), M(Initialize, dd_ok, 8), M(RestoreDisplayMode, dd_RestoreDisplayMode, 4),
    M(SetCooperativeLevel, dd_SetCooperativeLevel, 12), M(SetDisplayMode, dd2_SetDisplayMode, 24),
    M(WaitForVerticalBlank, dd_WaitForVerticalBlank, 12), M(GetAvailableVidMem, dd_GetAvailableVidMem, 16),
};
static const struct api SURF_METHODS[] = {
    M(QueryInterface, s_QueryInterface, 12), M(AddRef, s_AddRef, 4), M(Release, s_Release, 4), M(AddAttachedSurface, 0, 8),
    M(AddOverlayDirtyRect, 0, 8), M(Blt, s_Blt, 24), M(BltBatch, 0, 16), M(BltFast, s_BltFast, 24),
    M(DeleteAttachedSurface, 0, 12), M(EnumAttachedSurfaces, 0, 12), M(EnumOverlayZOrders, 0, 16), M(Flip, s_Flip, 12),
    M(GetAttachedSurface, s_GetAttachedSurface, 12), M(GetBltStatus, s_GetBltStatus, 8), M(GetCaps, s_GetCaps, 8),
    M(GetClipper, 0, 8), M(GetColorKey, s_GetColorKey, 12), M(GetDC, s_GetDC, 8), M(GetFlipStatus, s_GetFlipStatus, 8),
    M(GetOverlayPosition, 0, 12), M(GetPalette, s_GetPalette, 8), M(GetPixelFormat, s_GetPixelFormat, 8),
    M(GetSurfaceDesc, s_GetSurfaceDesc, 8), M(Initialize, dd_ok, 12), M(IsLost, s_IsLost, 4), M(Lock, s_Lock, 20),
    M(ReleaseDC, s_ReleaseDC, 8), M(Restore, s_Restore, 4), M(SetClipper, s_SetClipper, 8), M(SetColorKey, s_SetColorKey, 12),
    M(SetOverlayPosition, 0, 12), M(SetPalette, s_SetPalette, 8), M(Unlock, s_Unlock, 8), M(UpdateOverlay, 0, 24),
    M(UpdateOverlayDisplay, 0, 8), M(UpdateOverlayZOrder, 0, 12),
    M(GetDDInterface, 0, 8), M(PageLock, dd_ok, 8), M(PageUnlock, dd_ok, 8),
};
static const struct api PAL_METHODS[] = {
    M(QueryInterface, 0, 12), M(AddRef, c_AddRef, 4), M(Release, c_Release, 4), M(GetCaps, p_GetCaps, 8),
    M(GetEntries, p_GetEntries, 20), M(Initialize, dd_ok, 16), M(SetEntries, p_SetEntries, 20),
};
static const struct api CLIP_METHODS[] = {
    M(QueryInterface, 0, 12), M(AddRef, c_AddRef, 4), M(Release, c_Release, 4), M(GetClipList, 0, 16),
    M(GetHWnd, 0, 8), M(Initialize, dd_ok, 12), M(IsClipListChanged, 0, 8), M(SetClipList, dd_ok, 12), M(SetHWnd, dd_ok, 12),
};
static void ddraw_setup(void) {
    if (vt_dd) return;
    vt_dd = w32_make_vtable("IDirectDraw", DD_METHODS, sizeof DD_METHODS / sizeof DD_METHODS[0]);
    vt_dd2 = w32_make_vtable("IDirectDraw2", DD2_METHODS, sizeof DD2_METHODS / sizeof DD2_METHODS[0]);
    vt_surf = w32_make_vtable("IDirectDrawSurface", SURF_METHODS, sizeof SURF_METHODS / sizeof SURF_METHODS[0]);
    vt_pal = w32_make_vtable("IDirectDrawPalette", PAL_METHODS, sizeof PAL_METHODS / sizeof PAL_METHODS[0]);
    vt_clip = w32_make_vtable("IDirectDrawClipper", CLIP_METHODS, sizeof CLIP_METHODS / sizeof CLIP_METHODS[0]);
    dd_obj = w32_new_object(vt_dd, 8);
    dd2_obj = w32_new_object(vt_dd2, 8);
    for (int i = 0; i < 256; i++) default_pal[i] = 0xff000000u | (uint32_t)(i * 0x010101);
}
static uint32_t api_DirectDrawCreate(void) { ddraw_setup(); WR32(ARG(1), dd_obj); return DD_OK; }
const struct api API_DDRAW[] = { M(DirectDrawCreate, api_DirectDrawCreate, 12) };
const int N_DDRAW = 1;

// ---------------------------------------------------------------- text (GDI TextOut)
// Minimal built-in font: the game's dialogue uses its own bitmap fonts, GDI text is only used for
// a few labels. Characters are drawn from a 6x8 table, doubled for large font heights.
#include "font6x8.h"
void gdi_draw_text(uint32_t pixels, int pitch, int w, int h, int bpp, int x, int y, const char *s, int n, uint32_t color, int height) {
    int scale = height >= 14 ? 2 : 1;
    uint32_t c8 = 255;   // 8-bit: closest to white in most palettes; 16-bit: convert COLORREF
    uint32_t c16 = ((color & 0xf8) << 8) | (((color >> 8) & 0xfc) << 3) | ((color >> 19) & 0x1f);
    for (int k = 0; k < n; k++) {
        unsigned ch = (unsigned char)s[k];
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = FONT6X8[ch - 32];
        for (int gy = 0; gy < 8 * scale; gy++) for (int gx = 0; gx < 6 * scale; gx++) {
            if (!(g[gy / scale] & (0x20 >> (gx / scale)))) continue;
            int px = x + k * 7 * scale + gx, py = y + gy + scale;
            if (px < 0 || py < 0 || px >= w || py >= h) continue;
            uint32_t a = pixels + (uint32_t)(py * pitch);
            if (bpp == 8) WR8(a + px, c8); else if (bpp == 16) WR16(a + 2 * px, c16); else WR32(a + 4 * px, color);
        }
    }
}
