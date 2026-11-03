// KERNEL32 emulation: host-function registry, guest heap, files, process/environment queries.
#include "win32.h"
#include <time.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>

int w32_log_api;
char game_dir[1024] = "data", cd_dir[1024] = "data/cd", save_dir[1024] = "save";

// ---------------------------------------------------------------- host function registry
#define MAX_HOST 2048
static struct api host[MAX_HOST];
static int nhost;
uint32_t w32_register(const char *name, apifn fn, int argbytes) {
    if (nhost >= MAX_HOST) rt_fatal("too many host functions");
    host[nhost] = (struct api){ name, fn, argbytes };
    return MAGIC_BASE + 16u * nhost++;
}
static uint32_t api_missing(void) { return 0; }
void w32_call_host(uint32_t magic) {
    uint32_t k = (magic - MAGIC_BASE) / 16;
    if (k >= (uint32_t)nhost) rt_fatal("call to bad host address %08x", magic);
    struct api *a = &host[k];
    if (a->fn == api_missing) rt_fatal("unimplemented host function %s (called from %08x)", a->name, RD32(ESP));
    if (w32_log_api) {
        static const char *quiet; static int qi;
        if (!qi) { qi = 1; quiet = getenv("ICARUS_TRACE_QUIET"); }
        if (!quiet || !strstr(quiet, a->name))
            fprintf(stderr, "[api] %s(%x, %x, %x, %x) from %08x\n", a->name, ARG(0), ARG(1), ARG(2), ARG(3), RD32(ESP));
    }
    uint32_t r = a->fn();
    EAX = r;
    ESP += 4 + (a->argbytes > 0 ? a->argbytes : 0);
    IRQCHK();
}
static uint32_t com_stub_magic(const char *iface, const char *method) {
    char *nm = malloc(strlen(iface) + strlen(method) + 3);
    sprintf(nm, "%s::%s", iface, method);
    return w32_register(nm, api_missing, 0);
}
uint32_t w32_make_vtable(const char *iface, const struct api *m, int n) {
    uint32_t vt = gmem_alloc(4 * n);
    for (int k = 0; k < n; k++)
        WR32(vt + 4 * k, m[k].fn ? w32_register(m[k].name, m[k].fn, m[k].argbytes) : com_stub_magic(iface, m[k].name));
    return vt;
}
uint32_t w32_new_object(uint32_t vtable, uint32_t size) {
    uint32_t o = gmem_alloc(size < 8 ? 8 : size);
    WR32(o, vtable);
    return o;
}

// ---------------------------------------------------------------- guest heap
// First-fit allocator over [HEAP_LO, HEAP_HI); bookkeeping lives on the host side so guest
// buffer overruns cannot corrupt it.
struct range { uint32_t a, n; };
static struct range *fr; static int nfr, cfr;           // free ranges, sorted by address
static uint32_t *ha, *hs; static uint32_t hcap;          // open-addressing map addr -> size
static void hput(uint32_t a, uint32_t s) {
    if (!hcap || 0) {}
    uint32_t i = (a >> 4) * 2654435761u & (hcap - 1);
    while (ha[i] && ha[i] != a && ha[i] != 1) i = (i + 1) & (hcap - 1);
    ha[i] = a; hs[i] = s;
}
static int hfind(uint32_t a) {
    uint32_t i = (a >> 4) * 2654435761u & (hcap - 1);
    while (ha[i]) { if (ha[i] == a) return (int)i; i = (i + 1) & (hcap - 1); }
    return -1;
}
static void heap_init(void) {
    cfr = 1024; fr = malloc(sizeof *fr * cfr); nfr = 1;
    fr[0] = (struct range){ HEAP_LO, HEAP_HI - HEAP_LO };
    hcap = 1u << 20; ha = calloc(hcap, 4); hs = calloc(hcap, 4);
}
static uint32_t heap_used, heap_count;
uint32_t gmem_alloc(uint32_t size) {
    uint32_t n = (size + 15) & ~15u; if (!n) n = 16;
    for (int k = 0; k < nfr; k++) if (fr[k].n >= n) {
        uint32_t a = fr[k].a;
        fr[k].a += n; fr[k].n -= n;
        if (!fr[k].n) { memmove(fr + k, fr + k + 1, sizeof *fr * (nfr - k - 1)); nfr--; }
        if (heap_count * 2 > hcap) rt_fatal("too many heap blocks");
        hput(a, n); heap_used += n; heap_count++;
        memset(MEM + a, 0, n);
        return a;
    }
    fprintf(stderr, "guest heap exhausted (%u bytes requested, %u in use)\n", size, heap_used);
    return 0;
}
uint32_t gmem_size(uint32_t a) { int i = hfind(a); return i < 0 ? 0 : hs[i]; }
void gmem_free(uint32_t a) {
    if (!a) return;
    int i = hfind(a);
    if (i < 0) { LOG("free of unknown block %08x\n", a); return; }
    uint32_t n = hs[i]; ha[i] = 1; hs[i] = 0;   // tombstone
    heap_used -= n; heap_count--;
    int k = 0; while (k < nfr && fr[k].a < a) k++;
    if (k > 0 && fr[k - 1].a + fr[k - 1].n == a) {
        fr[k - 1].n += n;
        if (k < nfr && fr[k - 1].a + fr[k - 1].n == fr[k].a) { fr[k - 1].n += fr[k].n; memmove(fr + k, fr + k + 1, sizeof *fr * (nfr - k - 1)); nfr--; }
        return;
    }
    if (k < nfr && a + n == fr[k].a) { fr[k].a = a; fr[k].n += n; return; }
    if (nfr == cfr) { cfr *= 2; fr = realloc(fr, sizeof *fr * cfr); }
    memmove(fr + k + 1, fr + k, sizeof *fr * (nfr - k)); nfr++;
    fr[k] = (struct range){ a, n };
}
uint32_t gmem_realloc(uint32_t a, uint32_t size) {
    if (!a) return gmem_alloc(size);
    uint32_t old = gmem_size(a);
    if (old >= size && old - size < 4096) return a;
    uint32_t b = gmem_alloc(size);
    if (!b) return 0;
    memcpy(MEM + b, MEM + a, old < size ? old : size);
    gmem_free(a);
    return b;
}
void gstr_get(uint32_t a, char *out, int n) {
    int k = 0;
    if (a) for (; k < n - 1; k++) { out[k] = (char)RD8(a + k); if (!out[k]) break; }
    out[k] = 0;
}
void gstr_put(uint32_t a, const char *s) { size_t n = strlen(s) + 1; memcpy(GP(a), s, n); }

// ---------------------------------------------------------------- paths
// Guest view: the game is installed in C:\ICARUS (current directory), the CD is D:\.
// Files the game writes go to save_dir; reads look there first so saves round-trip.
static int find_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d) return 0;
    struct dirent *e; int ok = 0;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}
static int resolve_ci(const char *root, const char *rel, char *out, size_t n, int for_write) {
    char cur[1024]; snprintf(cur, sizeof cur, "%s", root);
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", rel);
    char *comp[64]; int nc = 0, k = 0;
    for (char *s = tmp; *s && nc < 64;) {
        while (*s == '/') *s++ = 0;
        if (!*s) break;
        comp[nc++] = s;
        while (*s && *s != '/') s++;
    }
    for (k = 0; k < nc; k++) {
        if (!strcmp(comp[k], ".")) continue;
        char nx[1024];
        if (!find_ci(cur, comp[k], nx, sizeof nx)) {
            if (k < nc - 1 || !for_write) return 0;
            snprintf(nx, sizeof nx, "%s/%s", cur, comp[k]);
        }
        snprintf(cur, sizeof cur, "%s", nx);
    }
    snprintf(out, n, "%s", cur);
    return 1;
}
int host_path(const char *guest, char *out, size_t n, int for_write) {
    char p[1024]; int k = 0;
    for (const char *s = guest; *s && k < 1023; s++) p[k++] = *s == '\\' ? '/' : *s;
    p[k] = 0;
    const char *root = game_dir; const char *rel = p;
    if (p[0] && p[1] == ':') {
        char drv = (char)(p[0] | 0x20);
        rel = p + 2;
        if (drv == 'd') root = cd_dir;
        else if (!strncasecmp(rel, "/icarus/", 8)) rel += 8;
        else if (!strncasecmp(rel, "/windows/fonts/", 15)) { snprintf(out, n, "%s/fonts/%s", game_dir, rel + 15); return 1; }
    }
    while (*rel == '/') rel++;
    if (root == game_dir) {
        const char *base = strrchr(rel, '/'); base = base ? base + 1 : rel;
        if (for_write) { MKDIR(save_dir); if (!resolve_ci(save_dir, base, out, n, 0)) snprintf(out, n, "%s/%s", save_dir, base); return 1; }
        if (resolve_ci(save_dir, base, out, n, 0)) return 1;
    }
    return resolve_ci(root, rel, out, n, for_write);
}

// ---------------------------------------------------------------- file handles
#define NFH 256
// Files opened for writing that exist only in the read-only game data are opened read-only and
// copied into save_dir the first time the game actually writes to them (copy-on-write).
static FILE *fh[NFH];
static char *fh_guest[NFH]; static int fh_cow[NFH];
static uint32_t last_error;
#define FH_BASE 0x100u
#define STD_IN 0x7001u
#define STD_OUT 0x7002u
#define STD_ERR 0x7003u
static FILE *fget(uint32_t h) { return (h >= FH_BASE && h < FH_BASE + NFH) ? fh[h - FH_BASE] : 0; }
static int save_path(const char *guest, char *out, size_t n) {
    char p[1024]; snprintf(p, sizeof p, "%s", guest);
    for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
    const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
    if (p[0] && p[1] == ':' && (p[0] | 0x20) == 'd') return 0;
    MKDIR(save_dir);
    if (!resolve_ci(save_dir, base, out, n, 0)) snprintf(out, n, "%s/%s", save_dir, base);
    return 1;
}
// disp: 1 CREATE_NEW 2 CREATE_ALWAYS 3 OPEN_EXISTING 4 OPEN_ALWAYS 5 TRUNCATE_EXISTING
static uint32_t open_guest(const char *name, int wr, int disp) {
    char hp[1024]; FILE *f = 0; int cow = 0;
    int truncate = disp == 1 || disp == 2 || disp == 5;
    if (!wr) {
        if (host_path(name, hp, sizeof hp, 0)) f = fopen(hp, "rb");
    } else if (truncate) {
        if (save_path(name, hp, sizeof hp)) f = fopen(hp, "w+b");
    } else {
        char sp[1024];
        if (save_path(name, sp, sizeof sp) && (f = fopen(sp, "r+b"))) snprintf(hp, sizeof hp, "%s", sp);
        else if (host_path(name, hp, sizeof hp, 0) && (f = fopen(hp, "rb"))) cow = 1;
        else if (disp == 4 && save_path(name, hp, sizeof hp)) f = fopen(hp, "w+b");
    }
    if (!f) { LOG("open %s: not found\n", name); last_error = 2; return 0xffffffffu; }
    for (int k = 3; k < NFH; k++) if (!fh[k]) {
        fh[k] = f; fh_cow[k] = cow; free(fh_guest[k]); fh_guest[k] = strdup(name);
        LOG("open %s -> %s%s = %x\n", name, hp, cow ? " (cow)" : "", FH_BASE + k);
        return FH_BASE + k;
    }
    fclose(f); return 0xffffffffu;
}
static FILE *fget_w(uint32_t h) {
    if (h < FH_BASE || h >= FH_BASE + NFH) return 0;
    int k = (int)(h - FH_BASE);
    if (!fh[k] || !fh_cow[k]) return fh[k];
    char sp[1024];
    if (!save_path(fh_guest[k], sp, sizeof sp)) return 0;
    long pos = ftell(fh[k]);
    FILE *o = fopen(sp, "w+b"); if (!o) return 0;
    fseek(fh[k], 0, SEEK_SET);
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, fh[k])) > 0) fwrite(buf, 1, n, o);
    fclose(fh[k]); fh[k] = o; fh_cow[k] = 0;
    fseek(o, pos, SEEK_SET);
    LOG("copy-on-write %s -> %s\n", fh_guest[k], sp);
    return o;
}

static uint32_t k_CreateFileA(void) {
    char name[512]; gstr_get(ARG(0), name, sizeof name);
    return open_guest(name, (ARG(1) & 0x40000000u) != 0, (int)ARG(4));
}
static uint32_t k__lopen(void) {
    char name[512]; gstr_get(ARG(0), name, sizeof name);
    return open_guest(name, (ARG(1) & 3) != 0, 3);
}
static uint32_t k_CloseHandle(void) { FILE *f = fget(ARG(0)); if (f) { fclose(f); fh[ARG(0) - FH_BASE] = 0; } return 1; }
static uint32_t k__lclose(void) { k_CloseHandle(); return 0; }
static uint32_t k_ReadFile(void) {
    FILE *f = fget(ARG(0)); uint32_t n = 0;
    if (f) n = (uint32_t)fread(GP(ARG(1)), 1, ARG(2), f);
    if (ARG(3)) WR32(ARG(3), n);
    return f != 0;
}
static uint32_t k__lread(void) { FILE *f = fget(ARG(0)); return f ? (uint32_t)fread(GP(ARG(1)), 1, ARG(2), f) : 0xffffffffu; }
static uint32_t k_WriteFile(void) {
    uint32_t h = ARG(0), n = ARG(2);
    if (h == STD_OUT || h == STD_ERR) { fwrite(GP(ARG(1)), 1, n, stderr); }
    else { FILE *f = fget_w(h); if (!f) return 0; n = (uint32_t)fwrite(GP(ARG(1)), 1, n, f); fflush(f); }
    if (ARG(3)) WR32(ARG(3), n);
    return 1;
}
static uint32_t k_SetFilePointer(void) {
    FILE *f = fget(ARG(0)); if (!f) return 0xffffffffu;
    int64_t d = (int32_t)ARG(1);
    if (ARG(2)) d = (int64_t)(((uint64_t)RD32(ARG(2)) << 32) | ARG(1));
    FSEEK64(f, d, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    int64_t pos = FTELL64(f);
    if (ARG(2)) WR32(ARG(2), (uint32_t)(pos >> 32));
    return (uint32_t)pos;
}
static uint32_t k__llseek(void) {
    FILE *f = fget(ARG(0)); if (!f) return 0xffffffffu;
    fseek(f, (int32_t)ARG(1), ARG(2) == 0 ? SEEK_SET : ARG(2) == 1 ? SEEK_CUR : SEEK_END);
    return (uint32_t)ftell(f);
}
static uint32_t k_GetFileSize(void) {
    FILE *f = fget(ARG(0)); if (!f) return 0xffffffffu;
    long p = ftell(f); fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, p, SEEK_SET);
    if (ARG(1)) WR32(ARG(1), 0);
    return (uint32_t)s;
}
static uint32_t k_FlushFileBuffers(void) { FILE *f = fget(ARG(0)); if (f) fflush(f); return 1; }
static uint32_t k_GetFileType(void) { uint32_t h = ARG(0); if (h >= STD_IN && h <= STD_ERR) return 2; return fget(h) ? 1 : 0; }
static uint32_t k_GetStdHandle(void) { int32_t n = (int32_t)ARG(0); return n == -10 ? STD_IN : n == -11 ? STD_OUT : n == -12 ? STD_ERR : 0xffffffffu; }

// ---------------------------------------------------------------- memory APIs
static uint32_t k_GlobalAlloc(void) { return gmem_alloc(ARG(1)); }
static uint32_t k_GlobalLock(void) { return ARG(0); }
static uint32_t k_GlobalUnlock(void) { return 1; }
static uint32_t k_GlobalHandle(void) { return ARG(0); }
static uint32_t k_GlobalFree(void) { gmem_free(ARG(0)); return 0; }
static uint32_t k_HeapCreate(void) { return 0x00ee0000u; }
static uint32_t k_HeapDestroy(void) { return 1; }
static uint32_t k_HeapAlloc(void) { return gmem_alloc(ARG(2)); }
static uint32_t k_HeapFree(void) { gmem_free(ARG(2)); return 1; }
static uint32_t k_HeapReAlloc(void) {
    uint32_t flags = ARG(1), p = ARG(2), n = ARG(3);
    if (flags & 0x10 /*HEAP_REALLOC_IN_PLACE_ONLY*/) return gmem_size(p) >= n ? p : 0;
    uint32_t old = gmem_size(p), q = gmem_realloc(p, n);
    if (q && n > old) memset(GP(q + old), 0, n - old);
    return q;
}
static uint32_t k_HeapCompact(void) { return 0x100000; }
static uint32_t k_VirtualAlloc(void) {
    uint32_t a = ARG(0), n = ARG(1);
    if (a) {   // commit inside a reservation we handed out earlier
        return a;
    }
    return gmem_alloc((n + 0xfff) & ~0xfffu);
}
static uint32_t k_VirtualFree(void) {
    uint32_t a = ARG(0), n = ARG(1), type = ARG(2);
    if (type & 0x8000 /*MEM_RELEASE*/) gmem_free(a);
    else if (type & 0x4000 /*MEM_DECOMMIT*/) memset(GP(a), 0, n);
    return 1;
}
static uint32_t k_IsBad(void) { return 0; }

// ---------------------------------------------------------------- strings / locale
static uint32_t k_lstrlenA(void) { return ARG(0) ? (uint32_t)strlen((char *)GP(ARG(0))) : 0; }
static uint32_t k_GetACP(void) { return 1252; }
static uint32_t k_GetOEMCP(void) { return 437; }
static uint32_t k_GetCPInfo(void) {
    uint32_t p = ARG(1); memset(GP(p), 0, 20);
    WR32(p, 1); WR8(p + 4, '?');
    return 1;
}
static uint32_t k_MultiByteToWideChar(void) {
    uint32_t src = ARG(2), dst = ARG(4); int32_t n = (int32_t)ARG(3); uint32_t cap = ARG(5);
    if (n < 0) n = (int32_t)strlen((char *)GP(src)) + 1;
    if (!cap) return (uint32_t)n;
    uint32_t k = 0;
    for (; k < (uint32_t)n && k < cap; k++) WR16(dst + 2 * k, RD8(src + k));
    return k;
}
static uint32_t k_WideCharToMultiByte(void) {
    uint32_t src = ARG(2), dst = ARG(4); int32_t n = (int32_t)ARG(3); uint32_t cap = ARG(5);
    if (n < 0) { n = 0; while (RD16(src + 2 * n)) n++; n++; }
    if (!cap) return (uint32_t)n;
    uint32_t k = 0;
    for (; k < (uint32_t)n && k < cap; k++) { uint16_t c = RD16(src + 2 * k); WR8(dst + k, c < 256 ? c : '?'); }
    return k;
}
static uint32_t k_GetStringTypeW(void) { last_error = 120; return 0; }   // not implemented on Win95
static uint32_t k_GetStringTypeA(void) {
    // (lcid, type, src, n, out)
    uint32_t src = ARG(2), out = ARG(4); int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen((char *)GP(src)) + 1;
    for (int k = 0; k < n; k++) {
        unsigned c = RD8(src + k); uint16_t t = 0;
        if (c >= 'A' && c <= 'Z') t |= 0x101; if (c >= 'a' && c <= 'z') t |= 0x102;
        if (c >= '0' && c <= '9') t |= 0x84; if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x8;
        if (c == ' ' || c == 9) t |= 0x40; if (c < 32 || c == 127) t |= 0x20;
        if (c > 32 && c < 127 && !(t & 0x103) && !(t & 4)) t |= 0x10;
        if ((t & 0x103) || (t & 4)) t |= 0x80 * 0;
        WR16(out + 2 * k, t);
    }
    return 1;
}
static uint32_t k_CompareStringA(void) {
    uint32_t a = ARG(2), b = ARG(4); int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    if (na < 0) na = (int32_t)strlen((char *)GP(a)); if (nb < 0) nb = (int32_t)strlen((char *)GP(b));
    int n = na < nb ? na : nb;
    int r = (ARG(1) & 1) ? strncasecmp((char *)GP(a), (char *)GP(b), n) : memcmp(GP(a), GP(b), n);
    if (!r) r = na - nb;
    return r < 0 ? 1 : r > 0 ? 3 : 2;
}
static uint32_t k_CompareStringW(void) { last_error = 120; return 0; }

// ---------------------------------------------------------------- process / environment
static uint32_t k_GetVersion(void) { return 0xC0000004u; }   // Windows 95
static uint32_t k_GetCommandLineA(void) { return HOSTSTR; }
static uint32_t k_GetEnvironmentStrings(void) { return HOSTSTR + 0x100; }
static uint32_t k_GetEnvironmentStringsW(void) { return 0; }
static uint32_t k_FreeEnv(void) { return 1; }
static uint32_t k_GetStartupInfoA(void) { memset(GP(ARG(0)), 0, 68); WR32(ARG(0), 68); return 0; }
static uint32_t k_GetModuleHandleA(void) { return ARG(0) ? 0 : 0x400000u; }
static uint32_t k_GetModuleFileNameA(void) {
    const char *s = "C:\\ICARUS\\ICARUS.EXE"; uint32_t n = ARG(2);
    strncpy((char *)GP(ARG(1)), s, n); return (uint32_t)strlen(s);
}
static uint32_t k_GetCurrentDirectoryA(void) { const char *s = "C:\\ICARUS"; if (ARG(0) > strlen(s)) gstr_put(ARG(1), s); return (uint32_t)strlen(s); }
static uint32_t k_GetFullPathNameA(void) {
    char in[512], out[600]; gstr_get(ARG(0), in, sizeof in);
    if (in[0] && in[1] == ':') snprintf(out, sizeof out, "%s", in); else snprintf(out, sizeof out, "C:\\ICARUS\\%s", in);
    uint32_t n = (uint32_t)strlen(out);
    if (ARG(1) > n) { gstr_put(ARG(2), out); if (ARG(3)) { char *b = strrchr(out, '\\'); WR32(ARG(3), ARG(2) + (uint32_t)(b ? b - out + 1 : 0)); } }
    return n;
}
static uint32_t k_GetDriveTypeA(void) {
    char r[16]; gstr_get(ARG(0), r, sizeof r);
    char d = (char)(r[0] | 0x20);
    return d == 'd' ? 5 : d == 'c' ? 3 : 1;
}
static uint32_t k_SetHandleCount(void) { return ARG(0); }
static uint32_t k_one(void) { return 1; }
static uint32_t k_zero(void) { return 0; }
static uint32_t k_GetLastError(void) { return last_error; }
static uint32_t k_GetCurrentProcess(void) { return 0xffffffffu; }
static uint32_t k_ExitProcess(void) { fprintf(stderr, "ExitProcess(%d)\n", ARG(0)); w32_shutdown(); exit((int)ARG(0)); }
static uint32_t k_TerminateProcess(void) { fprintf(stderr, "TerminateProcess(%d)\n", ARG(1)); w32_shutdown(); exit((int)ARG(1)); }
static uint32_t k_RaiseException(void) { rt_fatal("RaiseException(%08x)", ARG(0)); }
static uint32_t k_RtlUnwind(void) { rt_fatal("RtlUnwind (structured exception unwinding is not supported)"); }
static uint32_t k_LoadLibraryA(void) { char n[256]; gstr_get(ARG(0), n, sizeof n); LOG("LoadLibraryA(%s) -> fail\n", n); return 0; }
static void put_systime(uint32_t p, struct tm *t, int ms) {
    WR16(p, t->tm_year + 1900); WR16(p + 2, t->tm_mon + 1); WR16(p + 4, t->tm_wday); WR16(p + 6, t->tm_mday);
    WR16(p + 8, t->tm_hour); WR16(p + 10, t->tm_min); WR16(p + 12, t->tm_sec); WR16(p + 14, ms);
}
int64_t w32_time_skew;   // seconds added to the guest clock (boot-logo skipping)
static void now_tm(struct tm *t, int *ms, int local) {
    struct timespec ts; timespec_get(&ts, TIME_UTC);
    extern int logo_skip, title_reached;
    if (logo_skip && !title_reached) w32_time_skew += 2;   // waits on time() end quickly while skipping
    time_t s = ts.tv_sec + (time_t)w32_time_skew;
#ifdef _WIN32
    if (local) localtime_s(t, &s); else gmtime_s(t, &s);
#else
    if (local) localtime_r(&s, t); else gmtime_r(&s, t);
#endif
    *ms = (int)(ts.tv_nsec / 1000000);
}
static uint32_t k_GetLocalTime(void) { struct tm t; int ms; now_tm(&t, &ms, 1); put_systime(ARG(0), &t, ms); return 0; }
static uint32_t k_GetSystemTime(void) { struct tm t; int ms; now_tm(&t, &ms, 0); put_systime(ARG(0), &t, ms); return 0; }
static uint32_t k_GetTimeZoneInformation(void) { memset(GP(ARG(0)), 0, 172); return 0; }
static uint32_t k_Sleep(void) {
    uint32_t ms = ARG(0);
    w32_pump_events();
    host_sleep_ms(ms ? ms : 1);
    return 0;
}

#define A(n, f, b) { #n, f, b }
const struct api API_KERNEL32[] = {
    A(GlobalUnlock, k_GlobalUnlock, 4), A(GlobalFree, k_GlobalFree, 4), A(GlobalHandle, k_GlobalHandle, 4),
    A(GlobalAlloc, k_GlobalAlloc, 8), A(GlobalLock, k_GlobalLock, 4), A(_lread, k__lread, 12), A(_lclose, k__lclose, 4),
    A(GetFileSize, k_GetFileSize, 8), A(lstrlenA, k_lstrlenA, 4), A(CreateFileA, k_CreateFileA, 28),
    A(CloseHandle, k_CloseHandle, 4), A(SetFilePointer, k_SetFilePointer, 16), A(WriteFile, k_WriteFile, 20),
    A(ReadFile, k_ReadFile, 20), A(_lopen, k__lopen, 8), A(_llseek, k__llseek, 12), A(HeapReAlloc, k_HeapReAlloc, 16),
    A(VirtualAlloc, k_VirtualAlloc, 16), A(CompareStringW, k_CompareStringW, 24), A(CompareStringA, k_CompareStringA, 24),
    A(FlushFileBuffers, k_FlushFileBuffers, 4), A(SetStdHandle, k_one, 8), A(SetConsoleCtrlHandler, k_one, 8),
    A(LoadLibraryA, k_LoadLibraryA, 4), A(GetProcAddress, k_zero, 8), A(IsBadCodePtr, k_IsBad, 4),
    A(IsBadWritePtr, k_IsBad, 8), A(IsBadReadPtr, k_IsBad, 8), A(GetStringTypeW, k_GetStringTypeW, 16),
    A(GetStringTypeA, k_GetStringTypeA, 20), A(GetFileType, k_GetFileType, 4), A(GetStdHandle, k_GetStdHandle, 4),
    A(SetHandleCount, k_SetHandleCount, 4), A(GetOEMCP, k_GetOEMCP, 0), A(GetACP, k_GetACP, 0), A(GetCPInfo, k_GetCPInfo, 8),
    A(WideCharToMultiByte, k_WideCharToMultiByte, 32), A(GetEnvironmentStringsW, k_GetEnvironmentStringsW, 0),
    A(GetEnvironmentStrings, k_GetEnvironmentStrings, 0), A(FreeEnvironmentStringsW, k_FreeEnv, 4),
    A(MultiByteToWideChar, k_MultiByteToWideChar, 24), A(FreeEnvironmentStringsA, k_FreeEnv, 4),
    A(GetModuleFileNameA, k_GetModuleFileNameA, 12), A(UnhandledExceptionFilter, k_zero, 4),
    A(SetUnhandledExceptionFilter, k_zero, 4), A(RaiseException, k_RaiseException, 16), A(VirtualFree, k_VirtualFree, 12),
    A(SetEnvironmentVariableA, k_one, 8), A(Sleep, k_Sleep, 4), A(HeapCreate, k_HeapCreate, 12), A(HeapDestroy, k_HeapDestroy, 4),
    A(GetVersion, k_GetVersion, 0), A(GetCommandLineA, k_GetCommandLineA, 0), A(GetStartupInfoA, k_GetStartupInfoA, 4),
    A(GetModuleHandleA, k_GetModuleHandleA, 4), A(GetLocalTime, k_GetLocalTime, 4), A(GetSystemTime, k_GetSystemTime, 4),
    A(HeapCompact, k_HeapCompact, 8), A(TerminateProcess, k_TerminateProcess, 8),
    A(GetTimeZoneInformation, k_GetTimeZoneInformation, 4), A(GetDriveTypeA, k_GetDriveTypeA, 4),
    A(GetFullPathNameA, k_GetFullPathNameA, 16), A(GetCurrentDirectoryA, k_GetCurrentDirectoryA, 8),
    A(ExitProcess, k_ExitProcess, 4), A(GetCurrentProcess, k_GetCurrentProcess, 0), A(GetLastError, k_GetLastError, 0),
    A(RtlUnwind, k_RtlUnwind, 16), A(HeapFree, k_HeapFree, 12), A(HeapAlloc, k_HeapAlloc, 12),
};
const int N_KERNEL32 = sizeof API_KERNEL32 / sizeof API_KERNEL32[0];

// ---------------------------------------------------------------- process setup
static const struct { const char *dll; const struct api *t; const int *n; } MODS[] = {
    { "KERNEL32.DLL", API_KERNEL32, &N_KERNEL32 }, { "USER32.DLL", API_USER32, &N_USER32 },
    { "GDI32.DLL", API_GDI32, &N_GDI32 }, { "WINMM.DLL", API_WINMM, &N_WINMM }, { "DDRAW.DLL", API_DDRAW, &N_DDRAW },
    { "DSOUND.DLL", API_DSOUND, &N_DSOUND }, { "AVIFIL32.DLL", API_AVIFIL32, &N_AVIFIL32 }, { "MSVFW32.DLL", API_MSVFW32, &N_MSVFW32 },
};
void w32_init(int argc, char **argv) {
    (void)argc; (void)argv;
    heap_init();
    nhost = 1;     // keep magic index 0 unused
    // imports
    int missing = 0;
    for (int k = 0; k < NIMPORTS; k++) {
        const struct api *found = 0;
        for (size_t m = 0; m < sizeof MODS / sizeof MODS[0]; m++) {
            if (strcasecmp(MODS[m].dll, IMPORTS[k].dll)) continue;
            for (int j = 0; j < *MODS[m].n; j++) if (!strcmp(MODS[m].t[j].name, IMPORTS[k].name)) found = &MODS[m].t[j];
        }
        uint32_t magic;
        if (found) magic = w32_register(found->name, found->fn, found->argbytes);
        else { magic = w32_register(IMPORTS[k].name, api_missing, 0); missing++; LOG("import %s!%s not implemented\n", IMPORTS[k].dll, IMPORTS[k].name); }
        WR32(IMPORTS[k].iat, magic);
    }
    if (missing) LOG("%d imports unimplemented\n", missing);
    // TIB: no SEH frames, stack bounds, self pointer
    WR32(TIB_ADDR + 0x00, 0xffffffffu); WR32(TIB_ADDR + 0x04, STACK_HI); WR32(TIB_ADDR + 0x08, STACK_LO);
    WR32(TIB_ADDR + 0x18, TIB_ADDR); WR32(TIB_ADDR + 0x30, TIB_ADDR + 0x800);
    FS_BASE = TIB_ADDR;
    gstr_put(HOSTSTR, "C:\\ICARUS\\ICARUS.EXE");
    gstr_put(HOSTSTR + 0x100, "PATH=C:\\WINDOWS"); WR8(HOSTSTR + 0x110, 0);
}
