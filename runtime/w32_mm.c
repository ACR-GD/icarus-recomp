// WINMM (multimedia timers, MCI cdaudio/movie, mmio RIFF reader), AVIFIL32 and MSVFW32.
#include "win32.h"
#include <SDL3/SDL.h>
#include <strings.h>
#include <ctype.h>

// ---------------------------------------------------------------- multimedia timers
// Callbacks are delivered on the guest thread from rt_irq(); a host thread only raises irq_pending.
struct mmtimer { int used; uint32_t delay, proc, user, flags, id; uint32_t next; };
static struct mmtimer T[16];
static SDL_Mutex *tmu; static int tthr_on;
static void tlock(void) { if (!tmu) tmu = SDL_CreateMutex(); SDL_LockMutex(tmu); }
static void tunlock(void) { SDL_UnlockMutex(tmu); }
static int SDLCALL timer_thread(void *arg) {
    (void)arg;
    for (;;) {
        SDL_Delay(1);
        uint32_t now = w32_tick_ms();
        tlock();
        for (int k = 0; k < 16; k++) if (T[k].used && (int32_t)(now - T[k].next) >= 0) irq_pending = 1;
        tunlock();
    }
    return 0;
}
void audio_poll(void);
void w32_async_callbacks(void) {
    audio_poll();
    uint32_t now = w32_tick_ms();
    for (int k = 0; k < 16; k++) {
        tlock();
        int due = T[k].used && (int32_t)(now - T[k].next) >= 0;
        struct mmtimer t = T[k];
        if (due) {
            if (t.flags & 1) { T[k].next += t.delay ? t.delay : 1; if ((int32_t)(now - T[k].next) > 200) T[k].next = now + t.delay; }
            else T[k].used = 0;
        }
        tunlock();
        if (!due) continue;
        uint32_t a[5] = { t.id, 0, t.user, 0, 0 };
        guest_call(t.proc, 5, a);
    }
}
static uint32_t m_timeSetEvent(void) {
    tlock();
    int k; for (k = 0; k < 16 && T[k].used; k++);
    if (k == 16) { tunlock(); return 0; }
    T[k] = (struct mmtimer){ 1, ARG(0), ARG(2), ARG(3), ARG(4), (uint32_t)k + 1, w32_tick_ms() + ARG(0) };
    tunlock();
    if (!tthr_on) { tthr_on = 1; SDL_DetachThread(SDL_CreateThread(timer_thread, "mmtimer", 0)); }
    LOG("timeSetEvent(%u ms, proc %08x, user %08x, flags %x) -> %d\n", ARG(0), ARG(2), ARG(3), ARG(4), k + 1);
    return (uint32_t)k + 1;
}
static uint32_t m_timeKillEvent(void) {
    uint32_t id = ARG(0);
    tlock();
    if (id >= 1 && id <= 16) T[id - 1].used = 0;
    tunlock();
    return 0;
}

// ---------------------------------------------------------------- MCI
#define CD_DEVICE 1
#define MOVIE_DEVICE 2
static uint32_t cd_notify_hwnd; static int cd_notify;
static uint32_t cd_timefmt = 10;    // TMSF
static int ntracks = 19;
void mci_cd_finished(void) {
    if (cd_notify && cd_notify_hwnd) {
        cd_notify = 0;
        extern void w32_post_message(uint32_t hwnd, uint32_t m, uint32_t w, uint32_t l);
        w32_post_message(cd_notify_hwnd, 0x3b9 /*MM_MCINOTIFY*/, 1 /*SUCCESSFUL*/, CD_DEVICE);
    }
}
static int track_secs(int t) {
    char p[1100]; snprintf(p, sizeof p, "%s/audio/track%02d.wav", cd_dir, t);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f);
    return (int)((n - 44) / 176400);
}
static uint32_t m_mciSendCommandA(void) {
    uint32_t dev = ARG(0), msg = ARG(1), flags = ARG(2), p = ARG(3);
    LOG("mciSendCommand dev=%u msg=%x flags=%x\n", dev, msg, flags);
    switch (msg) {
    case 0x803: {   // MCI_OPEN
        char type[64] = ""; if (flags & 0x2000) gstr_get(RD32(p + 8), type, sizeof type);
        LOG("MCI_OPEN %s\n", type);
        WR32(p + 4, CD_DEVICE);
        return 0;
    }
    case 0x804: cdda_stop(); return 0;                      // MCI_CLOSE
    case 0x80d: if (flags & 0x400) cd_timefmt = RD32(p + 4); return 0;   // MCI_SET
    case 0x814: {   // MCI_STATUS
        uint32_t item = RD32(p + 8), trk = RD32(p + 12), r = 0;
        switch (item) {
        case 3: r = (uint32_t)ntracks; break;               // NUMBER_OF_TRACKS
        case 4: r = cdda_playing() ? 526 : 525; break;      // MODE: playing / stopped
        case 5: case 7: r = 1; break;                       // MEDIA_PRESENT / READY
        case 1: { int s = track_secs((flags & 0x10) ? (int)trk : 2); r = (uint32_t)((s / 60) | ((s % 60) << 8)); break; }  // LENGTH (MSF)
        case 0x4001: r = ((flags & 0x10) && trk == 1) ? 1 /*data*/ : 0 /*audio*/; break;   // CDA_STATUS_TYPE_TRACK
        case 8: r = 2; break;
        default: r = 0;
        }
        if (item == 0x4001) r = ((flags & 0x10) && trk == 1) ? 0x441 /*MCI_CDA_TRACK_OTHER*/ : 0x440 /*AUDIO*/;
        WR32(p + 4, r);
        return 0;
    }
    case 0x806: {   // MCI_PLAY
        int from = (flags & 4) ? (int)(RD32(p + 4) & 0xff) : 2, to = ntracks;
        if (flags & 8) {   // TMSF: "to track N at 0:00:00" stops where track N begins
            uint32_t t = RD32(p + 8); to = (int)(t & 0xff);
            if (!(t >> 8) && to > from) to--;
        }
        cd_notify = (flags & 1) != 0; cd_notify_hwnd = RD32(p);
        LOG("MCI_PLAY tracks %d..%d notify=%d\n", from, to, cd_notify);
        cdda_play(from, to);
        return 0;
    }
    case 0x808: case 0x809: cdda_stop(); return 0;           // MCI_STOP / PAUSE
    default:
        LOG("mciSendCommand dev=%u msg=%x flags=%x\n", dev, msg, flags);
        return 0;
    }
}
static char movie_file[1024];
static uint32_t m_mciSendStringA(void) {
    char cmd[1024]; gstr_get(ARG(0), cmd, sizeof cmd);
    uint32_t ret = ARG(1), retlen = ARG(2);
    LOG("mciSendString \"%s\"\n", cmd);
    if (ret && retlen) WR8(ret, 0);
    if (!strncasecmp(cmd, "open ", 5)) {
        char f[512]; sscanf(cmd + 5, "%511s", f);
        if (!host_path(f, movie_file, sizeof movie_file, 0)) { movie_file[0] = 0; return 275; /*MCIERR_FILE_NOT_FOUND*/ }
        return 0;
    }
    if (!strncasecmp(cmd, "play movie", 10)) { if (movie_file[0]) movie_play(movie_file); return 0; }
    if (!strncasecmp(cmd, "status movie length", 19) || !strncasecmp(cmd, "status movie position", 21)) {
        if (ret && retlen) gstr_put(ret, "100");
        return 0;
    }
    if (!strncasecmp(cmd, "status movie window handle", 26)) { if (ret && retlen) gstr_put(ret, "0"); return 0; }
    if (!strncasecmp(cmd, "close", 5)) { movie_file[0] = 0; return 0; }
    return 0;
}
static uint32_t m_mciGetErrorStringA(void) { if (ARG(1) && ARG(2)) gstr_put(ARG(1), "MCI error"); return 1; }

// ---------------------------------------------------------------- mmio (RIFF files)
#define NMM 32
static FILE *mmf[NMM];
static FILE *MMF(uint32_t h) { return (h >= 0x500 && h < 0x500 + NMM) ? mmf[h - 0x500] : 0; }
static uint32_t m_mmioOpenA(void) {
    char name[512], hp[1024]; gstr_get(ARG(0), name, sizeof name);
    if (!host_path(name, hp, sizeof hp, 0)) { LOG("mmioOpen %s: not found\n", name); return 0; }
    FILE *f = fopen(hp, "rb"); if (!f) return 0;
    for (int k = 0; k < NMM; k++) if (!mmf[k]) { mmf[k] = f; return 0x500u + k; }
    fclose(f); return 0;
}
static uint32_t m_mmioClose(void) { FILE *f = MMF(ARG(0)); if (f) { fclose(f); mmf[ARG(0) - 0x500] = 0; } return 0; }
static uint32_t m_mmioRead(void) { FILE *f = MMF(ARG(0)); if (!f) return 0xffffffffu; return (uint32_t)fread(GP(ARG(1)), 1, ARG(2), f); }
static uint32_t m_mmioSeek(void) {
    FILE *f = MMF(ARG(0)); if (!f) return 0xffffffffu;
    fseek(f, (int32_t)ARG(1), ARG(2) == 0 ? SEEK_SET : ARG(2) == 1 ? SEEK_CUR : SEEK_END);
    return (uint32_t)ftell(f);
}
static uint32_t rd_le32(FILE *f) { uint8_t b[4]; if (fread(b, 1, 4, f) != 4) return 0xffffffffu; return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint32_t m_mmioDescend(void) {
    FILE *f = MMF(ARG(0)); uint32_t ck = ARG(1), parent = ARG(2), flags = ARG(3);
    if (!f) return 265;
    long end = parent ? (long)(RD32(parent + 12) + RD32(parent + 4)) : 0x7fffffff;
    uint32_t want_id = RD32(ck), want_type = RD32(ck + 8);
    for (;;) {
        long at = ftell(f);
        if (at + 8 > end) return 265;   // MMIOERR_CHUNKNOTFOUND
        uint32_t id = rd_le32(f), size = rd_le32(f);
        if (id == 0xffffffffu) return 265;
        uint32_t type = 0;
        int is_list = id == 0x46464952u /*RIFF*/ || id == 0x5453494cu /*LIST*/;
        if (is_list) type = rd_le32(f);
        int match = 1;
        if (flags & 0x10) match = id == want_id;                         // FINDCHUNK
        if (flags & 0x20) match = id == 0x46464952u && type == want_type; // FINDRIFF
        if (flags & 0x40) match = id == 0x5453494cu && type == want_type; // FINDLIST
        if (match) {
            WR32(ck, id); WR32(ck + 4, size); WR32(ck + 8, type);
            WR32(ck + 12, (uint32_t)(at + 8)); WR32(ck + 16, 0);
            return 0;
        }
        fseek(f, at + 8 + size + (size & 1), SEEK_SET);
    }
}
static uint32_t m_mmioAscend(void) {
    FILE *f = MMF(ARG(0)); uint32_t ck = ARG(1); if (!f) return 0;
    uint32_t size = RD32(ck + 4);
    fseek(f, (long)(RD32(ck + 12) + size + (size & 1)), SEEK_SET);
    return 0;
}

void w32_post_message(uint32_t hwnd, uint32_t m, uint32_t w, uint32_t l);
#define A(n, f, b) { #n, f, b }
const struct api API_WINMM[] = {
    A(mciSendStringA, m_mciSendStringA, 16), A(timeSetEvent, m_timeSetEvent, 20), A(timeKillEvent, m_timeKillEvent, 4),
    A(mciGetErrorStringA, m_mciGetErrorStringA, 12), A(mciSendCommandA, m_mciSendCommandA, 16), A(mmioClose, m_mmioClose, 8),
    A(mmioAscend, m_mmioAscend, 12), A(mmioRead, m_mmioRead, 12), A(mmioDescend, m_mmioDescend, 16), A(mmioSeek, m_mmioSeek, 12),
    A(mmioOpenA, m_mmioOpenA, 12),
};
const int N_WINMM = sizeof API_WINMM / sizeof API_WINMM[0];

