// Replacement text renderer for the game's dialogue and menu font.
//
// The game draws every character through one routine (0x449f60, see NOTES.md): it copies an 8x16
// one-bit glyph from ENGLISH.FNT, plots it in one palette colour into the 8-bit buffer at
// [0x5ecb30] (pitch [0x531374]) at pen ([0x5eedbc], [0x5eedc0]) and advances the pen by 9 pixels.
// This hook draws an anti-aliased, proportionally spaced glyph instead (runtime/font_text.h,
// Crimson Pro), with a soft shadow. Coverage is blended against the pixel underneath and mapped
// back to the nearest colour of the current screen palette. ICARUS_FONT=original turns it off.
#include "win32.h"
#include "font_text.h"

#define G_PEN_X  0x5eedbcu
#define G_PEN_Y  0x5eedc0u
#define G_BUF    0x5ecb30u
#define G_PITCH  0x531374u

const uint32_t *ddraw_screen_palette(uint32_t *gen);
int ddraw_find_surface(uint32_t addr, uint32_t *base, uint32_t *end);

static int mode = -1;          // 0 original, 1 replacement
static uint16_t cache[256 * 256 * 16];   // (dst, colour, alpha/16) -> palette index | valid stamp << 8
static uint8_t stamp = 1;
static uint32_t cache_gen = 0xffffffffu;
static const uint32_t *pal;

static int nearest(int r, int g, int b) {
    int best = 0; long bd = 1L << 40;
    for (int i = 0; i < 256; i++) {
        uint32_t c = pal[i];
        int dr = (int)((c >> 16) & 255) - r, dg = (int)((c >> 8) & 255) - g, db = (int)(c & 255) - b;
        long d = 3L * dr * dr + 4L * dg * dg + 2L * db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static uint8_t blend(uint8_t dst, uint8_t col, int a) {   // a: 0..255
    int q = a >> 4;
    if (q >= 15) return col;
    uint16_t *e = &cache[(dst << 12) | (col << 4) | q];
    if ((*e >> 8) == stamp) return (uint8_t)*e;
    int af = q * 17;
    uint32_t d = pal[dst], c = pal[col];
    int r = (int)(((c >> 16) & 255) * af + ((d >> 16) & 255) * (255 - af)) / 255;
    int g = (int)(((c >> 8) & 255) * af + ((d >> 8) & 255) * (255 - af)) / 255;
    int b = (int)((c & 255) * af + (d & 255) * (255 - af)) / 255;
    uint8_t n = (uint8_t)nearest(r, g, b);
    *e = (uint16_t)(n | (stamp << 8));
    return n;
}

int text_glyph_hook(void) {
    if (mode < 0) { const char *f = getenv("ICARUS_FONT"); mode = !(f && !strcmp(f, "original")); }
    if (!mode) return 0;
    uint32_t ch = ARG(0) & 0xff, col = ARG(1) & 0xff;
    if (ch < 32) return 0;
    uint32_t gen; pal = ddraw_screen_palette(&gen);
    if (!pal) return 0;
    if (gen != cache_gen) { cache_gen = gen; if (++stamp == 0) { memset(cache, 0, sizeof cache); stamp = 1; } }
    int x = (int16_t)RD16(G_PEN_X), y = (int16_t)RD16(G_PEN_Y);
    uint32_t buf = RD32(G_BUF), pitch = RD16(G_PITCH);
    uint32_t sbase, send;
    if (!pitch || !ddraw_find_surface(buf, &sbase, &send)) { static int n; if (n++ < 5) LOG("text: buffer %08x is not a surface, using the original font\n", buf); return 0; }
    const struct tglyph *g = &TGLYPH[ch - 32];
    const uint8_t *a = TALPHA + g->off;
    static uint8_t shadow; static uint32_t shadow_gen = 0xffffffffu;
    if (shadow_gen != gen) { shadow_gen = gen; shadow = (uint8_t)nearest(0, 0, 0); }
    for (int pass = 0; pass < 2; pass++) {
        int ox = pass ? 0 : 1, oy = pass ? 0 : 1;
        for (int r = 0; r < g->h; r++) {
            int py = y + g->y + r + oy;
            if (py < 0) continue;
            for (int c = 0; c < g->w; c++) {
                int px = x + g->x + c + ox;
                if (px < 0 || (uint32_t)px >= pitch) continue;
                int al = a[r * g->w + c];
                if (!al) continue;
                uint32_t addr = buf + (uint32_t)py * pitch + (uint32_t)px;
                if (addr < sbase || addr >= send) continue;
                uint8_t dst = RD8(addr);
                WR8(addr, pass ? blend(dst, (uint8_t)col, al) : blend(dst, shadow, al * 3 / 5));
            }
        }
    }
    WR16(G_PEN_X, (uint16_t)(x + g->adv));
    return 1;
}
