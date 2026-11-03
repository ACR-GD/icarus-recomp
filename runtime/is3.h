/* InstallShield 3 .Z archive extractor (PKWARE DCL "implode" streams), shared by the launcher's
 * installer (runtime/installer.c) and the command-line tool (tools/is3unpack.c).
 * Decompressor follows the algorithm of Mark Adler's blast.c (zlib contrib). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <setjmp.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#endif

#define MAXBITS 13
typedef struct { const uint8_t *in; size_t left; int bitbuf, bitcnt; jmp_buf env;
                 uint8_t *out; size_t outlen, outcap; } state;
static int bits(state *s, int need) {
    int val = s->bitbuf;
    while (s->bitcnt < need) {
        if (!s->left) longjmp(s->env, 1);
        val |= (int)(*s->in++) << s->bitcnt; s->left--; s->bitcnt += 8;
    }
    s->bitbuf = val >> need; s->bitcnt -= need;
    return val & ((1 << need) - 1);
}
typedef struct { short *count, *symbol; } huffman;
static int decode(state *s, huffman *h) {
    int len = 1, code = 0, first = 0, count, index = 0;
    int bitbuf = s->bitbuf, left = s->bitcnt;
    short *next = h->count + 1;
    for (;;) {
        while (left--) {
            code |= (bitbuf & 1) ^ 1; bitbuf >>= 1;
            count = *next++;
            if (code < first + count) { s->bitbuf = bitbuf; s->bitcnt = (s->bitcnt - len) & 7; return h->symbol[index + (code - first)]; }
            index += count; first += count; first <<= 1; code <<= 1; len++;
        }
        left = (MAXBITS + 1) - len;
        if (left == 0) break;
        if (!s->left) longjmp(s->env, 1);
        bitbuf = *s->in++; s->left--;
        if (left > 8) left = 8;
    }
    return -9;
}
static void construct(huffman *h, const uint8_t *rep, int n) {
    int symbol = 0, len, left; short offs[MAXBITS + 1], length[256];
    do { len = *rep++; left = (len >> 4) + 1; len &= 15; do length[symbol++] = len; while (--left); } while (--n);
    n = symbol;
    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
    for (symbol = 0; symbol < n; symbol++) if (length[symbol]) h->symbol[offs[length[symbol]]++] = symbol;
}
static void put(state *s, uint8_t c) {
    if (s->outlen == s->outcap) { s->outcap = s->outcap ? s->outcap * 2 : 65536; s->out = realloc(s->out, s->outcap); }
    s->out[s->outlen++] = c;
}
static int blast(state *s) {
    static const uint8_t litlen[] = {11,124,8,7,28,7,188,13,76,4,10,8,12,10,12,10,8,23,8,9,7,6,7,8,7,6,55,8,23,24,12,11,7,9,11,12,6,7,22,5,7,24,6,11,9,6,7,22,7,11,38,7,9,8,25,11,8,11,9,12,8,12,5,38,5,38,5,11,7,5,6,21,6,10,53,8,7,24,10,27,44,253,253,253,252,252,252,13,12,45,12,45,12,61,12,45,44,173};
    static const uint8_t lenlen[] = {2,35,36,53,38,23};
    static const uint8_t distlen[] = {2,20,53,230,247,151,248};
    static const short base[16] = {3,2,4,5,6,7,8,9,10,12,16,24,40,72,136,264};
    static const char extra[16] = {0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8};
    static int virgin = 1;
    static short litcnt[MAXBITS+1], litsym[256], lencnt[MAXBITS+1], lensym[16], distcnt[MAXBITS+1], distsym[64];
    static huffman litcode = {litcnt, litsym}, lencode = {lencnt, lensym}, distcode = {distcnt, distsym};
    if (virgin) { construct(&litcode, litlen, sizeof litlen); construct(&lencode, lenlen, sizeof lenlen); construct(&distcode, distlen, sizeof distlen); virgin = 0; }
    int lit = bits(s, 8); if (lit > 1) return -1;
    int dict = bits(s, 8); if (dict < 4 || dict > 6) return -2;
    for (;;) {
        if (bits(s, 1)) {
            int symbol = decode(s, &lencode);
            int len = base[symbol] + bits(s, extra[symbol]);
            if (len == 519) break;
            symbol = len == 2 ? 2 : dict;
            int dist = decode(s, &distcode) << symbol;
            dist += bits(s, symbol); dist++;
            if ((size_t)dist > s->outlen) return -3;
            while (len--) put(s, s->out[s->outlen - dist]);
        } else {
            put(s, lit ? decode(s, &litcode) : bits(s, 8));
        }
    }
    return 0;
}
static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void mkdirs(char *path) {
    for (char *p = path + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
}
/* Extract (or list, when list != 0) an IS3 archive held in memory into outdir.
 * Returns the number of files that failed (0 = success), -1 if it is not an IS3 archive. */
static int is3_extract_mem(const uint8_t *buf, size_t sz, const char *outdir, int list) {
    if (sz < 0x40 || rd32(buf) != 0x8C655D13) return -1;
    unsigned nfiles = rd16(buf + 0x0C), ndirs = rd16(buf + 0x31);
    uint32_t toc = rd32(buf + 0x29);
    if (toc >= sz) return -1;
    char **dirs = calloc(ndirs ? ndirs : 1, sizeof *dirs);
    const uint8_t *p = buf + toc;
    for (unsigned i = 0; i < ndirs; i++) {
        unsigned esz = rd16(p + 2), nl = rd16(p + 4);
        dirs[i] = calloc(nl + 1, 1); memcpy(dirs[i], p + 6, nl);
        for (char *c = dirs[i]; *c; c++) if (*c == '\\') *c = '/';
        p += esz;
    }
    int errors = 0;
    for (unsigned i = 0; i < nfiles; i++) {
        unsigned dir = rd16(p + 1);
        uint32_t usize = rd32(p + 3), csize = rd32(p + 7), off = rd32(p + 11);
        unsigned esz = rd16(p + 0x17), nl = p[0x1D];
        char name[1024];
        snprintf(name, sizeof name, "%s/%s%s%.*s", outdir, dir < ndirs ? dirs[dir] : "", (dir < ndirs && dirs[dir][0]) ? "/" : "", nl, (const char *)p + 0x1E);
        if (list) printf("%10u %10u  %s\n", usize, csize, name);
        else if ((size_t)off + csize > sz) errors++;
        else {
            state s = {0}; s.in = buf + off; s.left = csize;
            int r = setjmp(s.env) ? -10 : blast(&s);
            if (r || s.outlen != usize) { fprintf(stderr, "%s: error %d (%zu/%u)\n", name, r, s.outlen, usize); errors++; }
            mkdirs(name);
            FILE *o = fopen(name, "wb"); if (o) { fwrite(s.out, 1, s.outlen, o); fclose(o); } else errors++;
            free(s.out);
        }
        p += esz;
    }
    for (unsigned i = 0; i < ndirs; i++) free(dirs[i]);
    free(dirs);
    return errors;
}
