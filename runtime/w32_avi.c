// AVIFIL32 (stream reading) and MSVFW32 (ICDecompress with a built-in Cinepak decoder), enough for
// the game's own movie player (OPEN.AVI / ENDING.AVI: Cinepak 320x240 + 8-bit PCM).
#include "win32.h"

#define AVIERR_FILEOPEN 0x80044068u
#define AVIERR_NODATA 0x80044071u
#define AVIERR_BUFFERTOOSMALL 0x80044072u
#define ICERR_OK 0
#define ICERR_BADFORMAT 0xfffffffeu

// ---------------------------------------------------------------- AVI file parsing
struct chunk { uint32_t off, size, start; };    // start = first sample in the chunk (audio)
struct stream {
    uint32_t type, handler, scale, rate, start, length, sugg, samplesize, quality;
    uint8_t fmt[1024]; uint32_t fmtsize;
    struct chunk *ch; int nch, cap;
    uint32_t total;          // samples
    uint32_t blockalign;
};
struct avifile { FILE *f; struct stream st[4]; int nst; uint32_t movi; int refs; };
#define NAVI 16
static struct { struct avifile *a; int stream; uint32_t obj; } HS[64];   // stream handles
static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
#define FCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static void add_chunk(struct stream *s, uint32_t off, uint32_t size) {
    if (s->nch == s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->ch = realloc(s->ch, sizeof *s->ch * s->cap); }
    s->ch[s->nch++] = (struct chunk){ off, size, 0 };
}
static void parse_list(struct avifile *a, long start, long end, int *cur_stream) {
    FILE *f = a->f;
    uint8_t h[12];
    for (long p = start; p + 8 <= end;) {
        fseek(f, p, SEEK_SET);
        if (fread(h, 1, 8, f) != 8) break;
        uint32_t id = rd32le(h), size = rd32le(h + 4);
        long data = p + 8;
        if (id == FCC('L','I','S','T')) {
            if (fread(h + 8, 1, 4, f) != 4) break;
            uint32_t lt = rd32le(h + 8);
            if (lt == FCC('m','o','v','i')) {
                a->movi = (uint32_t)(data);   // position of the 'movi' fourcc
            } else {
                if (lt == FCC('s','t','r','l')) { *cur_stream = a->nst++; }
                parse_list(a, data + 4, data + size, cur_stream);
            }
        } else if (id == FCC('s','t','r','h') && *cur_stream >= 0 && *cur_stream < 4) {
            uint8_t b[56] = { 0 }; fread(b, 1, size < 56 ? size : 56, f);
            struct stream *s = &a->st[*cur_stream];
            s->type = rd32le(b); s->handler = rd32le(b + 4); s->scale = rd32le(b + 20); s->rate = rd32le(b + 24);
            s->start = rd32le(b + 28); s->length = rd32le(b + 32); s->sugg = rd32le(b + 36); s->quality = rd32le(b + 40);
            s->samplesize = rd32le(b + 44);
        } else if (id == FCC('s','t','r','f') && *cur_stream >= 0 && *cur_stream < 4) {
            struct stream *s = &a->st[*cur_stream];
            s->fmtsize = size < sizeof s->fmt ? size : sizeof s->fmt;
            fread(s->fmt, 1, s->fmtsize, f);
        } else if (id == FCC('i','d','x','1')) {
            uint8_t *ix = malloc(size); fread(ix, 1, size, f);
            uint32_t first_off = size >= 16 ? rd32le(ix + 8) : 0;
            // offsets are relative to 'movi' (usual) or absolute (some writers)
            uint32_t base = first_off >= a->movi ? 0 : a->movi;
            for (uint32_t k = 0; k + 16 <= size; k += 16) {
                uint32_t cid = rd32le(ix + k);
                int sn = ((cid & 0xff) - '0') * 10 + (((cid >> 8) & 0xff) - '0');
                if (sn < 0 || sn >= a->nst) continue;
                add_chunk(&a->st[sn], base + rd32le(ix + k + 8) + 8, rd32le(ix + k + 12));
            }
            free(ix);
        }
        p = data + size + (size & 1);
    }
}
static struct avifile *avi_open(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    struct avifile *a = calloc(1, sizeof *a); a->f = f;
    uint8_t h[12]; fread(h, 1, 12, f);
    if (rd32le(h) != FCC('R','I','F','F') || rd32le(h + 8) != FCC('A','V','I',' ')) { fclose(f); free(a); return 0; }
    int cur = -1;
    parse_list(a, 12, 12 + rd32le(h + 4) - 4, &cur);
    for (int k = 0; k < a->nst; k++) {
        struct stream *s = &a->st[k];
        if (s->type == FCC('a','u','d','s')) {
            s->blockalign = s->fmtsize >= 14 ? (s->fmt[12] | s->fmt[13] << 8) : 1;
            if (!s->blockalign) s->blockalign = 1;
            uint32_t t = 0;
            for (int c = 0; c < s->nch; c++) { s->ch[c].start = t; t += s->ch[c].size / s->blockalign; }
            s->total = t;
        } else s->total = (uint32_t)s->nch;
    }
    LOG("AVI %s: %d streams, movi at %x\n", path, a->nst, a->movi);
    return a;
}

// ---------------------------------------------------------------- AVIFIL32
static uint32_t vt_stream;
static uint32_t a_AVIFileInit(void) { return 0; }
static uint32_t a_zero(void) { return 0; }
static int hs_of(uint32_t obj) { for (int k = 0; k < 64; k++) if (HS[k].a && HS[k].obj == obj) return k; return -1; }
static uint32_t a_AVIStreamOpenFromFileA(void) {
    uint32_t out = ARG(0); char name[512], hp[1024]; gstr_get(ARG(1), name, sizeof name);
    uint32_t type = ARG(2); int32_t idx = (int32_t)ARG(3);
    WR32(out, 0);
    if (!host_path(name, hp, sizeof hp, 0)) { LOG("AVIStreamOpenFromFile %s: not found\n", name); return AVIERR_FILEOPEN; }
    struct avifile *a = avi_open(hp);
    if (!a) return AVIERR_FILEOPEN;
    int n = 0;
    for (int k = 0; k < a->nst; k++) if (!type || a->st[k].type == type) {
        if (n++ != idx) continue;
        for (int h = 0; h < 64; h++) if (!HS[h].a) {
            if (!vt_stream) { static const struct api m[] = { { "QueryInterface", 0, 12 }, { "AddRef", 0, 4 }, { "Release", 0, 4 } }; vt_stream = w32_make_vtable("IAVIStream", m, 3); }
            HS[h].a = a; HS[h].stream = k; HS[h].obj = w32_new_object(vt_stream, 8);
            WR32(out, HS[h].obj);
            LOG("AVIStreamOpenFromFile %s type %.4s -> stream %d (%d chunks)\n", name, (char *)&type, k, a->st[k].nch);
            return 0;
        }
    }
    fclose(a->f); free(a);
    return AVIERR_NODATA;
}
static uint32_t a_AVIStreamRelease(void) {
    int h = hs_of(ARG(0)); if (h < 0) return 0;
    struct avifile *a = HS[h].a; HS[h].a = 0;
    int still = 0; for (int k = 0; k < 64; k++) if (HS[k].a == a) still = 1;
    if (!still) { fclose(a->f); for (int k = 0; k < a->nst; k++) free(a->st[k].ch); free(a); }
    return 0;
}
static struct stream *STR(uint32_t obj) { int h = hs_of(obj); if (h < 0) rt_fatal("bad AVI stream %08x", obj); return &HS[h].a->st[HS[h].stream]; }
static FILE *SFILE(uint32_t obj) { return HS[hs_of(obj)].a->f; }
static uint32_t a_AVIStreamInfoA(void) {
    struct stream *s = STR(ARG(0)); uint32_t p = ARG(1), n = ARG(2);
    uint8_t b[140] = { 0 };
    #define P32(o, v) do { uint32_t v_ = (v); memcpy(b + (o), &v_, 4); } while (0)
    P32(0, s->type); P32(4, s->handler); P32(20, s->scale); P32(24, s->rate); P32(28, s->start); P32(32, s->total);
    P32(40, s->sugg); P32(44, s->quality); P32(48, s->type == FCC('a','u','d','s') ? s->blockalign : s->samplesize);
    if (s->type == FCC('v','i','d','s') && s->fmtsize >= 12) { P32(60, rd32le(s->fmt + 4)); P32(64, rd32le(s->fmt + 8)); }
    memcpy(GP(p), b, n < 140 ? n : 140);
    return 0;
}
static uint32_t a_AVIStreamReadFormat(void) {
    struct stream *s = STR(ARG(0)); uint32_t fmt = ARG(2), pn = ARG(3);
    if (fmt) { uint32_t n = RD32(pn); memcpy(GP(fmt), s->fmt, n < s->fmtsize ? n : s->fmtsize); }
    WR32(pn, s->fmtsize);
    return 0;
}
static uint32_t a_AVIStreamLength(void) { return STR(ARG(0))->total; }
static uint32_t a_AVIStreamRead(void) {
    struct stream *s = STR(ARG(0)); FILE *f = SFILE(ARG(0));
    int32_t start = (int32_t)ARG(1), n = (int32_t)ARG(2); uint32_t buf = ARG(3), cap = ARG(4), pbytes = ARG(5), psamples = ARG(6);
    if (start < 0) start = 0;
    if (s->type != FCC('a','u','d','s')) {
        if (start >= s->nch) { if (pbytes) WR32(pbytes, 0); if (psamples) WR32(psamples, 0); return AVIERR_NODATA; }
        struct chunk *c = &s->ch[start];
        if (pbytes) WR32(pbytes, c->size);
        if (psamples) WR32(psamples, 1);
        if (!buf) return 0;
        if (cap < c->size) return AVIERR_BUFFERTOOSMALL;
        fseek(f, c->off, SEEK_SET); fread(GP(buf), 1, c->size, f);
        return 0;
    }
    // audio: samples (blocks) across chunks
    if (n == -1 /*AVISTREAMREAD_CONVENIENT*/) n = (int32_t)(cap / s->blockalign);
    uint32_t want = (uint32_t)n * s->blockalign, got = 0;
    if (!buf) { if (pbytes) WR32(pbytes, want); if (psamples) WR32(psamples, (uint32_t)n); return 0; }
    if (want > cap) want = cap - cap % s->blockalign;
    int lo = 0, hi = s->nch - 1, ci = 0;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (s->ch[mid].start <= (uint32_t)start) { ci = mid; lo = mid + 1; } else hi = mid - 1; }
    uint32_t skip = ((uint32_t)start - (s->nch ? s->ch[ci].start : 0)) * s->blockalign;
    for (; ci < s->nch && got < want; ci++, skip = 0) {
        struct chunk *c = &s->ch[ci];
        if (skip >= c->size) continue;
        uint32_t take = c->size - skip; if (take > want - got) take = want - got;
        fseek(f, c->off + skip, SEEK_SET); fread(GP(buf + got), 1, take, f);
        got += take;
    }
    if (pbytes) WR32(pbytes, got);
    if (psamples) WR32(psamples, got / s->blockalign);
    return got ? 0 : AVIERR_NODATA;
}

#define A(n, f, b) { #n, f, b }
const struct api API_AVIFIL32[] = {
    A(AVIStreamRelease, a_AVIStreamRelease, 4), A(AVIStreamOpenFromFileA, a_AVIStreamOpenFromFileA, 24),
    A(AVIFileInit, a_AVIFileInit, 0), A(AVIStreamRead, a_AVIStreamRead, 28), A(AVIFileExit, a_zero, 0),
    A(AVIStreamInfoA, a_AVIStreamInfoA, 12), A(AVIStreamReadFormat, a_AVIStreamReadFormat, 16),
    A(AVIStreamLength, a_AVIStreamLength, 4),
};
const int N_AVIFIL32 = sizeof API_AVIFIL32 / sizeof API_AVIFIL32[0];

// ---------------------------------------------------------------- Cinepak
struct cvid_cb { uint8_t y[4]; int8_t u, v; };
struct cvid_strip { struct cvid_cb v1[256], v4[256]; };
struct cvid {
    int used, w, h;
    struct cvid_strip strips[32];
    uint32_t *rgb;             // persistent XRGB frame (inter frames update it)
};
static struct cvid DEC[8];
static inline uint32_t be16(const uint8_t *p) { return (uint32_t)(p[0] << 8 | p[1]); }
static inline uint32_t be24(const uint8_t *p) { return (uint32_t)(p[0] << 16 | p[1] << 8 | p[2]); }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)(p[1] << 16 | p[2] << 8 | p[3]); }
static void cvid_codebook(struct cvid_cb *cb, int id, const uint8_t *d, const uint8_t *end) {
    int entsz = (id & 4) ? 4 : 6;
    int selective = id & 1;
    uint32_t flag = 0, mask = 0;
    for (int i = 0; i < 256; i++) {
        if (selective) {
            if (!(mask >>= 1)) { if (d + 4 > end) return; flag = be32(d); d += 4; mask = 0x80000000u; }
            if (!(flag & mask)) continue;
        }
        if (d + entsz > end) return;
        for (int k = 0; k < 4; k++) cb[i].y[k] = d[k];
        if (entsz == 6) { cb[i].u = (int8_t)d[4]; cb[i].v = (int8_t)d[5]; } else { cb[i].u = 0; cb[i].v = 0; }
        d += entsz;
    }
}
static inline uint32_t yuv(int y, int u, int v) {
    int r = y + 2 * v, g = y - u / 2 - v, b = y + 2 * u;
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint32_t)(r << 16 | g << 8 | b);
}
static void cvid_vectors(struct cvid *c, struct cvid_strip *s, int id, const uint8_t *d, const uint8_t *end, int y0, int y1) {
    uint32_t flag = 0, mask = 0;
    for (int y = y0; y < y1; y += 4) for (int x = 0; x < c->w; x += 4) {
        if ((id & 1) && !(mask >>= 1)) { if (d + 4 > end) return; flag = be32(d); d += 4; mask = 0x80000000u; }
        if ((id & 1) && !(flag & mask)) continue;                      // skipped block (inter)
        if (!(id & 2) && !(mask >>= 1)) { if (d + 4 > end) return; flag = be32(d); d += 4; mask = 0x80000000u; }
        uint32_t px[16];
        if ((id & 2) || !(flag & mask)) {                               // V1: one entry, 2x2 upscaled
            if (d >= end) return;
            struct cvid_cb *e = &s->v1[*d++];
            for (int k = 0; k < 4; k++) {
                uint32_t p = yuv(e->y[k], e->u, e->v);
                int bx = (k & 1) * 2, by = (k >> 1) * 2;
                px[by * 4 + bx] = px[by * 4 + bx + 1] = px[(by + 1) * 4 + bx] = px[(by + 1) * 4 + bx + 1] = p;
            }
        } else {                                                        // V4: four entries, one per quadrant
            if (d + 4 > end) return;
            for (int q = 0; q < 4; q++) {
                struct cvid_cb *e = &s->v4[*d++];
                int bx = (q & 1) * 2, by = (q >> 1) * 2;
                px[by * 4 + bx] = yuv(e->y[0], e->u, e->v); px[by * 4 + bx + 1] = yuv(e->y[1], e->u, e->v);
                px[(by + 1) * 4 + bx] = yuv(e->y[2], e->u, e->v); px[(by + 1) * 4 + bx + 1] = yuv(e->y[3], e->u, e->v);
            }
        }
        for (int r = 0; r < 4; r++) {
            if (y + r >= c->h) break;
            for (int k = 0; k < 4 && x + k < c->w; k++) c->rgb[(y + r) * c->w + x + k] = px[r * 4 + k];
        }
    }
}
static int cvid_decode(struct cvid *c, const uint8_t *d, uint32_t size) {
    if (size < 10) return 0;
    const uint8_t *end = d + size;
    int flags = d[0];
    int nstrips = (int)be16(d + 8);
    d += 10;
    int y0 = 0;
    for (int i = 0; i < nstrips && i < 32 && d + 12 <= end; i++) {
        uint32_t ssize = be24(d + 1);
        const uint8_t *se = d + ssize; if (se > end || ssize < 12) se = end;
        int top = (int)be16(d + 4), bot = (int)be16(d + 8);
        int h = (top && bot > top) ? bot - top : bot;
        if (i > 0 && !(flags & 1)) c->strips[i] = c->strips[i - 1];
        const uint8_t *p = d + 12;
        while (p + 4 <= se) {
            int cid = p[0]; uint32_t csz = be24(p + 1);
            if (csz < 4) break;
            const uint8_t *ce = p + csz; if (ce > se) ce = se;
            switch (cid) {
            case 0x20: case 0x21: case 0x24: case 0x25: cvid_codebook(c->strips[i].v4, cid, p + 4, ce); break;
            case 0x22: case 0x23: case 0x26: case 0x27: cvid_codebook(c->strips[i].v1, cid, p + 4, ce); break;
            case 0x30: case 0x31: case 0x32: cvid_vectors(c, &c->strips[i], cid, p + 4, ce, y0, y0 + h > c->h ? c->h : y0 + h); break;
            default: break;
            }
            p += csz;
        }
        y0 += h;
        d = se;
    }
    return 1;
}

// ---------------------------------------------------------------- MSVFW32
static uint32_t i_ICLocate(void) {
    uint32_t handler = ARG(1), bi = ARG(2);
    uint32_t comp = bi ? RD32(bi + 16) : 0;
    uint32_t h = handler | 0x20202020u, cc = comp | 0x20202020u;
    if (h != FCC('c','v','i','d') && cc != FCC('c','v','i','d')) { LOG("ICLocate: no codec for %.4s/%.4s\n", (char *)&handler, (char *)&comp); return 0; }
    for (int k = 1; k < 8; k++) if (!DEC[k].used) {
        memset(&DEC[k], 0, sizeof DEC[k]);
        DEC[k].used = 1; DEC[k].w = (int)RD32(bi + 4); DEC[k].h = (int)RD32(bi + 8); if (DEC[k].h < 0) DEC[k].h = -DEC[k].h;
        DEC[k].rgb = calloc((size_t)(DEC[k].w * DEC[k].h), 4);
        LOG("ICLocate cvid %dx%d -> %d\n", DEC[k].w, DEC[k].h, k);
        return 0x1c000000u | (uint32_t)k;
    }
    return 0;
}
static struct cvid *DC(uint32_t hic) { uint32_t k = hic & 0xff; if ((hic & 0xff000000u) != 0x1c000000u || k >= 8 || !DEC[k].used) return 0; return &DEC[k]; }
static uint32_t i_ICClose(void) { struct cvid *c = DC(ARG(0)); if (c) { free(c->rgb); c->used = 0; } return 0; }
static uint32_t i_ICSendMessage(void) {
    uint32_t msg = ARG(1), d2 = ARG(3);
    switch (msg) {
    case 0x4000 + 10:   // ICM_DECOMPRESS_GET_FORMAT
        if (!d2) return 40;
        { uint32_t in = ARG(2); memcpy(GP(d2), GP(in), 40); WR16(d2 + 14, 24); WR32(d2 + 16, 0); WR32(d2 + 20, RD32(in + 4) * 3 * (RD32(in + 8))); }
        return ICERR_OK;
    default: return ICERR_OK;   // BEGIN / END / QUERY ...
    }
}
static uint32_t i_ICDecompress(void) {
    struct cvid *c = DC(ARG(0));
    uint32_t inbi = ARG(2), data = ARG(3), outbi = ARG(4), bits = ARG(5);
    if (!c) return ICERR_BADFORMAT;
    uint32_t size = RD32(inbi + 20);
    if (size) cvid_decode(c, GP(data), size);
    int ow = (int)RD32(outbi + 4), oh = (int32_t)RD32(outbi + 8), bpp = RD16(outbi + 14);
    uint32_t comp = RD32(outbi + 16);
    int bottom_up = oh > 0; if (oh < 0) oh = -oh;
    int pitch = ((ow * bpp / 8) + 3) & ~3;
    int is565 = comp == 3 && RD32(outbi + 40) == 0xf800;
    static int logged; if (!logged++) LOG("ICDecompress -> %dx%d %dbpp comp=%u%s\n", ow, oh, bpp, comp, is565 ? " (565)" : "");
    for (int y = 0; y < oh && y < c->h; y++) {
        uint32_t row = bits + (uint32_t)((bottom_up ? oh - 1 - y : y) * pitch);
        const uint32_t *src = c->rgb + y * c->w;
        for (int x = 0; x < ow && x < c->w; x++) {
            uint32_t p = src[x], r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (bpp == 16) WR16(row + 2 * x, is565 ? ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)) : ((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)));
            else if (bpp == 24) { WR8(row + 3 * x, b); WR8(row + 3 * x + 1, g); WR8(row + 3 * x + 2, r); }
            else if (bpp == 32) WR32(row + 4 * x, p);
            else WR8(row + x, (uint8_t)((r * 3 + g * 6 + b) / 10));
        }
    }
    return ICERR_OK;
}
const struct api API_MSVFW32[] = {
    A(ICClose, i_ICClose, 4), A(ICDecompress, i_ICDecompress, -1)   /* VFWAPIV = cdecl */, A(ICLocate, i_ICLocate, 20), A(ICSendMessage, i_ICSendMessage, 16),
};
const int N_MSVFW32 = sizeof API_MSVFW32 / sizeof API_MSVFW32[0];
