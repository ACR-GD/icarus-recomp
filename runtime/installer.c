// Game installer used by the launcher: rebuilds the data folder from the player's own disc images.
// Same steps as tools/prepare_data.py, without needing Python:
//   CD 1: ISO9660 -> EXE.Z ROOT.Z EDATA.Z TDATA.Z (InstallShield 3) -> install folder,
//         ICARUS.EXE checked against the build this binary was recompiled from, mapped into
//         icarus.img (the guest memory image), droiyan.ini (CD drive "D:\").
//   CD 2: every file -> cd/, CD-DA tracks -> cd/audio/trackNN.wav.
// Accepts .cue (with .bin), raw .bin (MODE1/2352) and .iso (2048-byte sectors) images.
#include "installer.h"
#include "is3.h"
#include "../gen/image_info.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

static void fail(struct install_progress *p, const char *fmt, const char *arg) {
    snprintf(p->err, sizeof p->err, fmt, arg ? arg : "");
}
static void status(struct install_progress *p, float frac, const char *msg) {
    p->frac = frac; snprintf(p->msg, sizeof p->msg, "%s", msg);
}
static void mkdirs_path(const char *path) {
    char t[1100]; snprintf(t, sizeof t, "%s", path);
    for (char *c = t + 1; *c; c++) if (*c == '/' || *c == '\\') { char s = *c; *c = 0; MKDIR(t); *c = s; }
    MKDIR(t);
}

// ---------------------------------------------------------------- disc images
struct track { int no, audio; long start, end; };   // sectors
struct disc {
    FILE *f; int raw;                 // raw = 2352-byte sectors (data at +16), else 2048
    struct track t[100]; int nt;
};
static void disc_close(struct disc *d) { if (d->f) fclose(d->f); memset(d, 0, sizeof *d); }
static int ends_with(const char *s, const char *ext) { size_t a = strlen(s), b = strlen(ext); return a >= b && !strcasecmp(s + a - b, ext); }
static int disc_open(struct disc *d, const char *path, char *err, size_t errn) {
    memset(d, 0, sizeof *d);
    char bin[1100] = "";
    if (ends_with(path, ".cue")) {
        FILE *c = fopen(path, "r");
        if (!c) { snprintf(err, errn, "cannot open %s", path); return 0; }
        char line[512], dir[1100]; snprintf(dir, sizeof dir, "%s", path);
        char *sl = strrchr(dir, '/'); char *bs = strrchr(dir, '\\'); if (bs > sl) sl = bs;
        if (sl) sl[1] = 0; else dir[0] = 0;
        while (fgets(line, sizeof line, c)) {
            char *s = line; while (isspace((unsigned char)*s)) s++;
            if (!strncasecmp(s, "FILE", 4)) {
                char *q1 = strchr(s, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : 0;
                if (q1 && q2) { *q2 = 0; snprintf(bin, sizeof bin, "%s%s", dir, q1 + 1); }
                else { char name[512]; if (sscanf(s + 4, "%511s", name) == 1) snprintf(bin, sizeof bin, "%s%s", dir, name); }
            } else if (!strncasecmp(s, "TRACK", 5) && d->nt < 99) {
                int no = 0; char mode[64] = ""; sscanf(s + 5, "%d %63s", &no, mode);
                d->t[d->nt].no = no; d->t[d->nt].audio = !strncasecmp(mode, "AUDIO", 5); d->t[d->nt].start = -1; d->nt++;
                if (!strncasecmp(mode, "MODE1/2048", 10)) d->raw = 0; else d->raw = 1;
            } else if (!strncasecmp(s, "INDEX 01", 8) && d->nt) {
                int m, sec, fr; if (sscanf(s + 8, "%d:%d:%d", &m, &sec, &fr) == 3) d->t[d->nt - 1].start = (m * 60 + sec) * 75 + fr;
            }
        }
        fclose(c);
        if (!bin[0] || !d->nt) { snprintf(err, errn, "%s is not a usable cue sheet", path); return 0; }
        d->raw = 1;   // multi-track cue sheets of this game are raw 2352-byte images
    } else {
        snprintf(bin, sizeof bin, "%s", path);
        d->raw = !ends_with(path, ".iso");
        d->nt = 1; d->t[0].no = 1; d->t[0].start = 0;
    }
    d->f = fopen(bin, "rb");
    if (!d->f) { snprintf(err, errn, "cannot open the disc image %s", bin); return 0; }
    fseek(d->f, 0, SEEK_END);
    long total = ftell(d->f) / (d->raw ? 2352 : 2048);
    for (int k = 0; k < d->nt; k++) d->t[k].end = k + 1 < d->nt ? d->t[k + 1].start : total;
    return 1;
}
static int read_sectors(struct disc *d, long lba, int n, uint8_t *out) {
    long base = d->t[0].start;
    for (int k = 0; k < n; k++) {
        long s = base + lba + k;
        if (d->raw) { fseek(d->f, s * 2352 + 16, SEEK_SET); if (fread(out + 2048 * k, 1, 2048, d->f) != 2048) return 0; }
        else { fseek(d->f, s * 2048, SEEK_SET); if (fread(out + 2048 * k, 1, 2048, d->f) != 2048) return 0; }
    }
    return 1;
}

// ---------------------------------------------------------------- ISO9660
struct isofile { char path[512]; uint32_t lba, size; };
struct isolist { struct isofile *f; int n, cap; };
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int iso_walk(struct disc *d, uint32_t lba, uint32_t size, const char *prefix, struct isolist *out, int depth) {
    if (depth > 8 || size > (4u << 20)) return 0;
    uint32_t ns = (size + 2047) / 2048;
    uint8_t *buf = malloc(ns * 2048);
    if (!read_sectors(d, lba, (int)ns, buf)) { free(buf); return 0; }
    for (uint32_t p = 0; p < size;) {
        uint8_t len = buf[p];
        if (!len) { p = (p / 2048 + 1) * 2048; continue; }
        uint8_t nl = buf[p + 32]; const char *nm = (const char *)buf + p + 33;
        if (!(nl == 1 && (nm[0] == 0 || nm[0] == 1))) {
            char name[256]; int k = 0;
            for (; k < nl && k < 255 && nm[k] != ';'; k++) name[k] = nm[k];
            name[k] = 0; if (k && name[k - 1] == '.') name[k - 1] = 0;
            char path[512]; snprintf(path, sizeof path, "%s%s", prefix, name);
            uint32_t elba = le32(buf + p + 2), esz = le32(buf + p + 10);
            if (buf[p + 25] & 2) {
                char pre[520]; snprintf(pre, sizeof pre, "%s/", path);
                iso_walk(d, elba, esz, pre, out, depth + 1);
            } else {
                if (out->n == out->cap) { out->cap = out->cap ? out->cap * 2 : 64; out->f = realloc(out->f, sizeof *out->f * out->cap); }
                snprintf(out->f[out->n].path, sizeof out->f[0].path, "%s", path);
                out->f[out->n].lba = elba; out->f[out->n].size = esz; out->n++;
            }
        }
        p += len;
    }
    free(buf);
    return 1;
}
static int iso_list(struct disc *d, struct isolist *out) {
    uint8_t pvd[2048];
    if (!read_sectors(d, 16, 1, pvd) || memcmp(pvd + 1, "CD001", 5)) return 0;
    return iso_walk(d, le32(pvd + 156 + 2), le32(pvd + 156 + 10), "", out, 0);
}
static struct isofile *iso_find(struct isolist *l, const char *name) {
    for (int k = 0; k < l->n; k++) if (!strcasecmp(l->f[k].path, name)) return &l->f[k];
    return 0;
}
static uint8_t *iso_read(struct disc *d, struct isofile *f) {
    uint32_t ns = (f->size + 2047) / 2048;
    uint8_t *b = malloc((size_t)ns * 2048 + 1);
    if (!b || !read_sectors(d, f->lba, (int)ns, b)) { free(b); return 0; }
    return b;
}
static int iso_copy(struct disc *d, struct isofile *f, const char *dst) {
    FILE *o = fopen(dst, "wb"); if (!o) return 0;
    uint8_t buf[2048 * 64];
    uint32_t left = f->size, lba = f->lba;
    while (left) {
        uint32_t n = left > sizeof buf ? (uint32_t)sizeof buf : left;
        if (!read_sectors(d, lba, (int)((n + 2047) / 2048), buf)) { fclose(o); return 0; }
        fwrite(buf, 1, n, o); left -= n; lba += (n + 2047) / 2048;
    }
    fclose(o);
    return 1;
}

// ---------------------------------------------------------------- ICARUS.EXE -> memory image
static int make_image(const char *exe, const char *img, char *err, size_t errn) {
    FILE *f = fopen(exe, "rb"); if (!f) { snprintf(err, errn, "ICARUS.EXE was not extracted"); return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz); if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return 0; } fclose(f);
    uint64_t h = 0xcbf29ce484222325ull;
    for (long k = 0; k < sz; k++) h = (h ^ b[k]) * 0x100000001b3ull;
    if (sz != EXE_SIZE || h != EXE_FNV64) {
        snprintf(err, errn, "This disc has a different version of ICARUS.EXE than the one this build supports "
                 "(English release, 1998). Size %ld, expected %d.", sz, EXE_SIZE);
        free(b); return 0;
    }
    uint32_t pe = le32(b + 0x3c);
    unsigned nsec = b[pe + 6] | b[pe + 7] << 8, opt = b[pe + 20] | b[pe + 21] << 8;
    uint32_t base = le32(b + pe + 24 + 28), size = le32(b + pe + 24 + 56), hdrs = le32(b + pe + 24 + 60);
    uint8_t *m = calloc(1, size);
    memcpy(m, b, hdrs < (uint32_t)sz ? hdrs : (uint32_t)sz);
    for (unsigned k = 0; k < nsec; k++) {
        const uint8_t *s = b + pe + 24 + opt + 40 * k;
        uint32_t va = le32(s + 12), rawsz = le32(s + 16), rawp = le32(s + 20);
        if (va + rawsz > size) rawsz = size - va;
        if (rawp + rawsz <= (uint32_t)sz) memcpy(m + va, b + rawp, rawsz);
    }
    FILE *o = fopen(img, "wb");
    if (!o) { snprintf(err, errn, "cannot write %s", img); free(b); free(m); return 0; }
    uint32_t hdr[2] = { base, size };
    fwrite(hdr, 4, 2, o); fwrite(m, 1, size, o); fclose(o);
    free(b); free(m);
    return 1;
}

// ---------------------------------------------------------------- CD audio
static int write_track(struct disc *d, struct track *t, const char *dst) {
    long frames = t->end - t->start; if (frames <= 0) return 0;
    uint32_t n = (uint32_t)(frames * 2352);
    FILE *o = fopen(dst, "wb"); if (!o) return 0;
    uint8_t h[44]; memcpy(h, "RIFF", 4);
    uint32_t v = 36 + n; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4); uint16_t s = 1; memcpy(h + 20, &s, 2); s = 2; memcpy(h + 22, &s, 2);
    v = 44100; memcpy(h + 24, &v, 4); v = 176400; memcpy(h + 28, &v, 4); s = 4; memcpy(h + 32, &s, 2); s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &n, 4);
    fwrite(h, 1, 44, o);
    static uint8_t buf[2352 * 256];
    fseek(d->f, t->start * 2352, SEEK_SET);
    for (uint32_t left = n; left;) {
        uint32_t k = left > sizeof buf ? (uint32_t)sizeof buf : left;
        if (fread(buf, 1, k, d->f) != k) break;
        fwrite(buf, 1, k, o); left -= k;
    }
    fclose(o);
    return 1;
}

// ---------------------------------------------------------------- install
int install_is_complete(const char *datadir) {
    char p[1100]; snprintf(p, sizeof p, "%s/icarus.img", datadir);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    fclose(f);
    snprintf(p, sizeof p, "%s/install.ok", datadir);
    f = fopen(p, "rb"); if (!f) return 0;
    fclose(f);
    return 1;
}

int install_game(const char *cd1, const char *cd2, const char *datadir, struct install_progress *p) {
    struct disc d1 = { 0 }, d2 = { 0 };
    struct isolist l1 = { 0 }, l2 = { 0 };
    char path[1200];
    int ok = 0;
    p->warn[0] = 0;
    status(p, 0.01f, "Reading disc 1...");
    if (!disc_open(&d1, cd1, p->err, sizeof p->err)) goto out;
    if (!iso_list(&d1, &l1)) { fail(p, "Disc 1 (%s) has no readable data track.", cd1); goto out; }
    if (!iso_find(&l1, "ROOT.Z")) { fail(p, "%s is not Icarus disc 1 (ROOT.Z is missing).", cd1); goto out; }
    mkdirs_path(datadir);
    snprintf(path, sizeof path, "%s/install.ok", datadir); remove(path);
    static const struct { const char *z, *sub; float a, b; } Z[] = {
        { "EXE.Z", "", 0.02f, 0.05f }, { "ROOT.Z", "", 0.05f, 0.30f }, { "EDATA.Z", "edata", 0.30f, 0.38f }, { "TDATA.Z", "tdata", 0.38f, 0.48f },
    };
    for (int k = 0; k < 4; k++) {
        char msg[128]; snprintf(msg, sizeof msg, "Unpacking %s...", Z[k].z);
        status(p, Z[k].a, msg);
        struct isofile *f = iso_find(&l1, Z[k].z);
        if (!f) { fail(p, "Disc 1 is missing %s.", Z[k].z); goto out; }
        uint8_t *buf = iso_read(&d1, f);
        if (!buf) { fail(p, "Could not read %s from disc 1.", Z[k].z); goto out; }
        char dst[1100]; snprintf(dst, sizeof dst, "%s%s%s", datadir, Z[k].sub[0] ? "/" : "", Z[k].sub);
        mkdirs_path(dst);
        int r = is3_extract_mem(buf, f->size, dst, 0);
        free(buf);
        if (r != 0) { fail(p, "%s on disc 1 is damaged.", Z[k].z); goto out; }
    }
    status(p, 0.49f, "Checking the game program...");
    snprintf(path, sizeof path, "%s/ICARUS.EXE", datadir);
    char img[1100]; snprintf(img, sizeof img, "%s/icarus.img", datadir);
    if (!make_image(path, img, p->err, sizeof p->err)) goto out;
    snprintf(path, sizeof path, "%s/droiyan.ini", datadir);
    { FILE *f = fopen(path, "wb"); if (f) { fputs("D:\\", f); fclose(f); } }

    // run-time CD: disc 2 (has the music); disc 1 has the same files but no audio
    struct disc *cd = &d1; struct isolist *cl = &l1;
    if (cd2 && cd2[0]) {
        status(p, 0.50f, "Reading disc 2...");
        if (!disc_open(&d2, cd2, p->err, sizeof p->err)) goto out;
        if (!iso_list(&d2, &l2) || !iso_find(&l2, "OPEN.AVI")) { fail(p, "%s is not Icarus disc 2.", cd2); goto out; }
        cd = &d2; cl = &l2;
    }
    uint64_t total = 0, done = 0;
    for (int k = 0; k < cl->n; k++) if (!ends_with(cl->f[k].path, ".Z") || cd == &d2) total += cl->f[k].size;
    for (int k = 0; k < cd->nt; k++) if (cd->t[k].audio) total += (uint64_t)(cd->t[k].end - cd->t[k].start) * 2352;
    for (int k = 0; k < cl->n; k++) {
        struct isofile *f = &cl->f[k];
        if (cd == &d1 && (ends_with(f->path, ".Z") || !(strncasecmp(f->path, "MAP/", 4) == 0 || ends_with(f->path, ".AVI")))) continue;
        char msg[600]; snprintf(msg, sizeof msg, "Copying %s...", f->path);
        status(p, 0.5f + 0.5f * (float)done / (float)(total ? total : 1), msg);
        snprintf(path, sizeof path, "%s/cd/%s", datadir, f->path);
        char dir[1200]; snprintf(dir, sizeof dir, "%s", path); char *sl = strrchr(dir, '/'); if (sl) { *sl = 0; mkdirs_path(dir); }
        if (!iso_copy(cd, f, path)) { fail(p, "Could not copy %s from the disc.", f->path); goto out; }
        done += f->size;
    }
    int ntracks = 0;
    snprintf(path, sizeof path, "%s/cd/audio", datadir); mkdirs_path(path);
    for (int k = 0; k < cd->nt; k++) if (cd->t[k].audio) {
        char msg[128]; snprintf(msg, sizeof msg, "Saving music track %d...", cd->t[k].no);
        status(p, 0.5f + 0.5f * (float)done / (float)(total ? total : 1), msg);
        snprintf(path, sizeof path, "%s/cd/audio/track%02d.wav", datadir, cd->t[k].no);
        if (!write_track(cd, &cd->t[k], path)) { fail(p, "Could not save music track %s.", ""); goto out; }
        done += (uint64_t)(cd->t[k].end - cd->t[k].start) * 2352; ntracks++;
    }
    if (!ntracks) snprintf(p->warn, sizeof p->warn, "No CD music found: pick disc 2 as a .cue file to get the soundtrack.");
    snprintf(path, sizeof path, "%s/install.ok", datadir);
    { FILE *f = fopen(path, "wb"); if (f) { fprintf(f, "icarus-recomp data v1\nmusic tracks %d\n", ntracks); fclose(f); } }
    status(p, 1.0f, ntracks ? "Installed." : "Installed (without music).");
    ok = 1;
out:
    free(l1.f); free(l2.f);
    disc_close(&d1); disc_close(&d2);
    p->ok = ok;
    p->done = 1;
    return ok;
}
