// DirectSound on an SDL3 audio stream: software mixing of guest-memory sound buffers, plus the
// CD-DA music tracks (read from WAV files extracted from the game disc).
#include "win32.h"
#include <SDL3/SDL.h>
#include <math.h>

#define OUT_RATE 44100
#define DS_OK 0
#define DSERR_GENERIC 0x80004005u

struct dsbuf {
    int used, primary;
    uint32_t data, size;            // guest memory
    int rate, ch, bits;
    int32_t vol, pan;               // hundredths of dB
    double pos;                     // byte position (fractional frames)
    int playing, loop;
    uint32_t obj;
    float gl, gr;
    // Streaming: a looping buffer the game keeps writing while it plays (the movie player).
    // Each write's whole frames are queued in a host FIFO, which is played with a ~90 ms cushion
    // and a slight speed adjustment to follow the writer. (The game's movie player writes 918
    // bytes of 8-bit stereo into 919-byte slots, so playing the ring directly swaps the channels
    // and replays a stale byte every 42 ms: audible crackle.)
    int writes, streaming, primed;
    uint8_t *fifo; uint32_t fr, fw;  // byte FIFO (FIFO_CAP), read / write counters
    double ffrac;                    // fractional frame position inside the FIFO
};
#define FIFO_CAP (1u << 18)
#define NB 256
static struct dsbuf B[NB];
static SDL_AudioStream *stream;
static uint32_t vt_ds, vt_buf, ds_obj;
static int audio_ok;
static float master = 1.0f;

// ---------------------------------------------------------------- CD-DA
static FILE *cd_f; static int cd_track, cd_from, cd_to; static long cd_end;
static int cd_playing_flag;
static int open_track(int t) {
    char p[1100];
    if (cd_f) { fclose(cd_f); cd_f = 0; }
    snprintf(p, sizeof p, "%s/audio/track%02d.wav", cd_dir, t);
    cd_f = fopen(p, "rb");
    if (!cd_f) { LOG("cd: no %s\n", p); return 0; }
    fseek(cd_f, 0, SEEK_END); cd_end = ftell(cd_f); fseek(cd_f, 44, SEEK_SET);
    // ICARUS_CDDA_MAXSEC=n cuts every track to n seconds (testing track-end handling)
    if (getenv("ICARUS_CDDA_MAXSEC")) { long lim = 44 + atol(getenv("ICARUS_CDDA_MAXSEC")) * 176400L; if (lim < cd_end) cd_end = lim; }
    cd_track = t;
    return 1;
}
void video_on_title(void);
void cdda_play(int from, int to) {
    video_on_title();   // the first music is the title screen: boot logos are over
    if (stream) SDL_LockAudioStream(stream);
    cd_from = from; cd_to = to < from ? from : to;
    cd_playing_flag = open_track(from);
    if (stream) SDL_UnlockAudioStream(stream);
    LOG("cdda play %d..%d -> %d\n", from, to, cd_playing_flag);
}
void cdda_stop(void) {
    if (stream) SDL_LockAudioStream(stream);
    cd_playing_flag = 0; if (cd_f) { fclose(cd_f); cd_f = 0; }
    if (stream) SDL_UnlockAudioStream(stream);
}
int cdda_playing(void) { return cd_playing_flag; }
extern void mci_cd_finished(void);
static volatile int cd_done_event;

// ---------------------------------------------------------------- mixer
static float vol_gain(int32_t v) { if (v <= -10000) return 0; return powf(10.0f, (float)v / 2000.0f); }
static void update_gain(struct dsbuf *b) {
    float g = vol_gain(b->vol);
    float l = 1, r = 1;
    if (b->pan < 0) r = vol_gain(b->pan); else if (b->pan > 0) l = vol_gain(-b->pan);
    b->gl = g * l; b->gr = g * r;
}
static float mixbuf[8192 * 2];
static FILE *dump;           // ICARUS_AUDIO_DUMP=file.raw: copy of the mixed output (s16 stereo 44.1 kHz)
static void SDLCALL mix_cb(void *ud, SDL_AudioStream *st, int additional, int total) {
    (void)ud; (void)total;
    int frames = additional / 4;
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        memset(mixbuf, 0, sizeof(float) * 2 * n);
        for (int k = 0; k < NB; k++) {
            struct dsbuf *b = &B[k];
            if (!b->used || b->primary || !b->playing || !b->size) continue;
            int bpf = b->ch * b->bits / 8; if (!bpf) continue;
            uint32_t nfr = b->size / bpf;
            double step = (double)b->rate / OUT_RATE;
            double fpos = b->pos / bpf;
            if (b->streaming) {
                double bps = (double)b->rate * bpf, target = bps * 0.09;
                uint32_t avail = b->fw - b->fr;
                if (!b->primed && avail >= target) b->primed = 1;
                if (!b->primed) continue;               // wait for the cushion (silence)
                double adj = 1 + ((double)avail - target) / target * 0.05;
                if (adj < 0.97) adj = 0.97; if (adj > 1.03) adj = 1.03;
                double st = step * adj;
                for (int i = 0; i < n; i++) {
                    if (b->fw - b->fr < 2u * (uint32_t)bpf) { b->primed = 0; LOG("snd #%d: underrun, re-buffering\n", k); break; }
                    uint32_t a1 = b->fr & (FIFO_CAP - 1), a2 = (b->fr + (uint32_t)bpf) & (FIFO_CAP - 1);
                    float l, r, l2, r2, t = (float)b->ffrac;
                    if (b->bits == 8) {
                        l = (b->fifo[a1] - 128) / 128.0f; r = b->ch == 2 ? (b->fifo[(a1 + 1) & (FIFO_CAP - 1)] - 128) / 128.0f : l;
                        l2 = (b->fifo[a2] - 128) / 128.0f; r2 = b->ch == 2 ? (b->fifo[(a2 + 1) & (FIFO_CAP - 1)] - 128) / 128.0f : l2;
                    } else {
                        int16_t v[4];
                        for (int q = 0; q < 2 * b->ch; q++) v[q] = (int16_t)(b->fifo[(a1 + 2 * q) & (FIFO_CAP - 1)] | b->fifo[(a1 + 2 * q + 1) & (FIFO_CAP - 1)] << 8);
                        int16_t w2[4];
                        for (int q = 0; q < 2 * b->ch; q++) w2[q] = (int16_t)(b->fifo[(a2 + 2 * q) & (FIFO_CAP - 1)] | b->fifo[(a2 + 2 * q + 1) & (FIFO_CAP - 1)] << 8);
                        l = v[0] / 32768.0f; r = b->ch == 2 ? v[1] / 32768.0f : l;
                        l2 = w2[0] / 32768.0f; r2 = b->ch == 2 ? w2[1] / 32768.0f : l2;
                    }
                    l += (l2 - l) * t; r += (r2 - r) * t;
                    mixbuf[2 * i] += l * b->gl; mixbuf[2 * i + 1] += r * b->gr;
                    b->ffrac += st;
                    while (b->ffrac >= 1) { b->ffrac -= 1; b->fr += (uint32_t)bpf; }
                    fpos += st; if (fpos >= nfr) fpos -= nfr;   // keep the reported play cursor moving
                }
                b->pos = fpos * bpf;
                continue;
            }
            for (int i = 0; i < n; i++) {
                uint32_t f = (uint32_t)fpos;
                if (f >= nfr) {
                    if (b->loop) { fpos -= nfr; f = (uint32_t)fpos; if (f >= nfr) { fpos = 0; f = 0; } }
                    else { b->playing = 0; fpos = 0; break; }
                }
                uint32_t f2 = f + 1 < nfr ? f + 1 : (b->loop ? 0 : f);
                float t = (float)(fpos - f);
                uint32_t a = b->data + f * bpf, a2 = b->data + f2 * bpf;
                float l, r, l2, r2;
                if (b->bits == 8) {
                    l = (RD8(a) - 128) / 128.0f; r = b->ch == 2 ? (RD8(a + 1) - 128) / 128.0f : l;
                    l2 = (RD8(a2) - 128) / 128.0f; r2 = b->ch == 2 ? (RD8(a2 + 1) - 128) / 128.0f : l2;
                } else {
                    l = (int16_t)RD16(a) / 32768.0f; r = b->ch == 2 ? (int16_t)RD16(a + 2) / 32768.0f : l;
                    l2 = (int16_t)RD16(a2) / 32768.0f; r2 = b->ch == 2 ? (int16_t)RD16(a2 + 2) / 32768.0f : l2;
                }
                l += (l2 - l) * t; r += (r2 - r) * t;        // linear interpolation
                mixbuf[2 * i] += l * b->gl; mixbuf[2 * i + 1] += r * b->gr;
                fpos += step;
            }
            if (b->playing) b->pos = fpos * bpf;
        }
        if (cd_playing_flag && cd_f) {
            int16_t tmp[4096 * 2]; int got = 0;
            while (got < n) {
                long left = (cd_end - ftell(cd_f)) / 4; if (left < 0) left = 0;
                size_t want = (size_t)(n - got) < (size_t)left ? (size_t)(n - got) : (size_t)left;
                size_t rd = fread(tmp + 2 * got, 4, want, cd_f);
                got += (int)rd;
                if (got < n) {
                    if (cd_track < cd_to && open_track(cd_track + 1)) continue;
                    cd_playing_flag = 0; cd_done_event = 1; break;
                }
            }
            for (int i = 0; i < got; i++) { mixbuf[2 * i] += tmp[2 * i] / 32768.0f * 0.8f; mixbuf[2 * i + 1] += tmp[2 * i + 1] / 32768.0f * 0.8f; }
        }
        int16_t out[4096 * 2];
        for (int i = 0; i < 2 * n; i++) {
            float v = mixbuf[i] * master;
            if (v > 1) v = 1; if (v < -1) v = -1;
            out[i] = (int16_t)(v * 32767);
        }
        SDL_PutAudioStreamData(st, out, n * 4);
        if (dump) fwrite(out, 4, (size_t)n, dump);
        frames -= n;
    }
}
void audio_init(void) {
    if (getenv("ICARUS_MUTE") && atoi(getenv("ICARUS_MUTE"))) master = 0;
    if (getenv("ICARUS_AUDIO_DUMP")) dump = fopen(getenv("ICARUS_AUDIO_DUMP"), "wb");
    // headless (test) runs never reach the speakers: SDL's dummy device still drives the mixer
    // (so ICARUS_AUDIO_DUMP works) but plays nothing. ICARUS_MUTE=0 opts back in.
    if (getenv("ICARUS_HEADLESS") && atoi(getenv("ICARUS_HEADLESS")) && !(getenv("ICARUS_MUTE") && !atoi(getenv("ICARUS_MUTE"))))
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");   // ~12 ms: sound effects follow the action
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "SDL audio: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, OUT_RATE };
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, mix_cb, 0);
    if (!stream) { fprintf(stderr, "SDL audio stream: %s\n", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(stream);
    audio_ok = 1;
}
void audio_shutdown(void) { if (stream) SDL_DestroyAudioStream(stream); stream = 0; }
// polled from the guest thread: report CD track end to the MCI layer (MM_MCINOTIFY)
void audio_poll(void) { if (cd_done_event) { cd_done_event = 0; mci_cd_finished(); } }

static void lock(void) { if (stream) SDL_LockAudioStream(stream); }
static void unlock(void) { if (stream) SDL_UnlockAudioStream(stream); }

// ---------------------------------------------------------------- IDirectSound
#define THIS ARG(0)
static struct dsbuf *BF(uint32_t o) { uint32_t k = RD32(o + 4); if (k >= NB || !B[k].used) rt_fatal("bad sound buffer %08x", o); return &B[k]; }
static uint32_t c_AddRef(void) { return 2; }
static uint32_t c_Release(void) { return 0; }
static uint32_t ds_ok(void) { return DS_OK; }
static uint32_t ds_CreateSoundBuffer(void) {
    uint32_t d = ARG(1), out = ARG(2);
    uint32_t flags = RD32(d + 4), size = RD32(d + 8), fmt = RD32(d + 16);
    int k; for (k = 1; k < NB && B[k].used; k++) {}
    if (k == NB) return DSERR_GENERIC;
    struct dsbuf *b = &B[k]; memset(b, 0, sizeof *b);
    b->used = 1; b->primary = (flags & 1) != 0;
    b->rate = 22050; b->ch = 1; b->bits = 8;
    if (fmt) { b->ch = RD16(fmt + 2); b->rate = (int)RD32(fmt + 4); b->bits = RD16(fmt + 14); }
    if (!b->primary) { b->size = size; b->data = gmem_alloc(size ? size : 16); }
    update_gain(b);
    b->obj = w32_new_object(vt_buf, 8); WR32(b->obj + 4, (uint32_t)k);
    WR32(out, b->obj);
    LOG("CreateSoundBuffer #%d flags=%x size=%u %dHz %dch %dbit\n", k, flags, size, b->rate, b->ch, b->bits);
    return DS_OK;
}
static uint32_t ds_DuplicateSoundBuffer(void) {
    struct dsbuf *s = BF(ARG(1));
    int k; for (k = 1; k < NB && B[k].used; k++) {}
    if (k == NB) return DSERR_GENERIC;
    struct dsbuf *b = &B[k]; *b = *s; b->pos = 0; b->playing = 0;
    b->fifo = 0; b->fr = b->fw = 0; b->writes = 0; b->streaming = 0; b->primed = 0;
    b->obj = w32_new_object(vt_buf, 8); WR32(b->obj + 4, (uint32_t)k);
    WR32(ARG(2), b->obj);
    return DS_OK;
}
static uint32_t ds_GetCaps(void) { memset(GP(ARG(1) + 4), 0, 92); WR32(ARG(1) + 4, 0xf0f); return DS_OK; }

// ---------------------------------------------------------------- IDirectSoundBuffer
static uint32_t b_Release(void) {
    struct dsbuf *b = BF(THIS);
    LOG("snd release #%u\n", RD32(THIS + 4));
    lock(); b->playing = 0; b->used = 0; free(b->fifo); b->fifo = 0; unlock();
    // shared data of duplicated buffers is left to the heap (never freed while another copy uses it)
    return 0;
}
static uint32_t b_GetCaps(void) { struct dsbuf *b = BF(THIS); WR32(ARG(1) + 4, 0); WR32(ARG(1) + 8, b->size); return DS_OK; }
static uint32_t b_GetCurrentPosition(void) {
    struct dsbuf *b = BF(THIS);
    lock(); uint32_t p = (uint32_t)b->pos; unlock();
    if (b->size) p %= b->size;
    if (ARG(1)) WR32(ARG(1), p);
    if (ARG(2)) WR32(ARG(2), b->size ? (p + (uint32_t)(b->rate * b->ch * b->bits / 8 / 50)) % b->size : 0);
    return DS_OK;
}
static uint32_t b_GetFormat(void) {
    struct dsbuf *b = BF(THIS); uint32_t f = ARG(1);
    if (f) { WR16(f, 1); WR16(f + 2, b->ch); WR32(f + 4, b->rate); WR32(f + 8, b->rate * b->ch * b->bits / 8); WR16(f + 12, b->ch * b->bits / 8); WR16(f + 14, b->bits); WR16(f + 16, 0); }
    if (ARG(3)) WR32(ARG(3), 18);
    return DS_OK;
}
static uint32_t b_GetVolume(void) { WR32(ARG(1), (uint32_t)BF(THIS)->vol); return DS_OK; }
static uint32_t b_GetPan(void) { WR32(ARG(1), (uint32_t)BF(THIS)->pan); return DS_OK; }
static uint32_t b_GetFrequency(void) { WR32(ARG(1), (uint32_t)BF(THIS)->rate); return DS_OK; }
static uint32_t b_GetStatus(void) { struct dsbuf *b = BF(THIS); WR32(ARG(1), b->playing ? (1 | (b->loop ? 4 : 0)) : 0); return DS_OK; }
static uint32_t b_Lock(void) {
    struct dsbuf *b = BF(THIS);
    uint32_t off = ARG(1), n = ARG(2), flags = ARG(7);
    if (flags & 2 /*ENTIREBUFFER*/) { off = 0; n = b->size; }
    if (flags & 1 /*FROMWRITECURSOR*/) off = (uint32_t)b->pos;
    if (!b->size) { WR32(ARG(3), 0); WR32(ARG(4), 0); if (ARG(5)) WR32(ARG(5), 0); if (ARG(6)) WR32(ARG(6), 0); return DS_OK; }
    off %= b->size; if (n > b->size) n = b->size;
    uint32_t n1 = n, n2 = 0;
    if (off + n > b->size) { n1 = b->size - off; n2 = n - n1; }
    WR32(ARG(3), b->data + off); WR32(ARG(4), n1);
    if (ARG(5)) WR32(ARG(5), n2 ? b->data : 0);
    if (ARG(6)) WR32(ARG(6), n2);
    return DS_OK;
}
// A write while a looping buffer plays: queue its whole frames in the FIFO. Two such writes mark
// the buffer as streamed (from then on it plays from the FIFO).
static void fifo_put(struct dsbuf *b, uint32_t src, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) b->fifo[(b->fw + k) & (FIFO_CAP - 1)] = RD8(src + k);
    b->fw += n;
}
static uint32_t b_Unlock(void) {
    struct dsbuf *b = BF(THIS);
    uint32_t p1 = ARG(1), n1 = ARG(2), p2 = ARG(3), n2 = ARG(4);
    if (!b->playing || !b->loop || !b->size || p1 < b->data || p1 >= b->data + b->size || !n1) return DS_OK;
    uint32_t bpf = (uint32_t)(b->ch * b->bits / 8); if (!bpf) return DS_OK;
    uint32_t whole = (n1 + n2) / bpf * bpf;           // only complete frames
    lock();
    if (!b->fifo) b->fifo = malloc(FIFO_CAP);
    if (b->fw - b->fr + whole <= FIFO_CAP) {
        uint32_t a = whole < n1 ? whole : n1;
        fifo_put(b, p1, a);
        if (whole > a && p2) fifo_put(b, p2, whole - a);
    }
    if (++b->writes >= 2 && !b->streaming) { b->streaming = 1; LOG("snd #%u: streaming\n", RD32(THIS + 4)); }
    unlock();
    return DS_OK;
}
static uint32_t b_Play(void) { struct dsbuf *b = BF(THIS);
    LOG("snd play #%u size=%u %dHz vol=%d pan=%d loop=%d pos=%u (from %08x)\n", RD32(THIS + 4), b->size, b->rate, b->vol, b->pan, (ARG(3) & 1) != 0, (uint32_t)b->pos, RD32(ESP)); lock();
    if (!b->playing) { b->fr = b->fw = 0; b->ffrac = 0; b->writes = 0; b->streaming = 0; b->primed = 0; }
    b->playing = 1; b->loop = (ARG(3) & 1) != 0; unlock(); return DS_OK; }
static uint32_t b_Stop(void) { struct dsbuf *b = BF(THIS); lock(); b->playing = 0; unlock(); return DS_OK; }
static uint32_t b_SetCurrentPosition(void) {
    struct dsbuf *b = BF(THIS);
    lock(); b->pos = ARG(1); b->fr = b->fw = 0; b->ffrac = 0; b->writes = 0; b->streaming = 0; b->primed = 0; unlock();
    return DS_OK;
}
static uint32_t b_SetFormat(void) {
    struct dsbuf *b = BF(THIS); uint32_t f = ARG(1);
    lock(); b->ch = RD16(f + 2); b->rate = (int)RD32(f + 4); b->bits = RD16(f + 14); unlock();
    return DS_OK;
}
static uint32_t b_SetVolume(void) { struct dsbuf *b = BF(THIS); lock(); b->vol = (int32_t)ARG(1); update_gain(b); unlock(); return DS_OK; }
static uint32_t b_SetPan(void) { struct dsbuf *b = BF(THIS); lock(); b->pan = (int32_t)ARG(1); update_gain(b); unlock(); return DS_OK; }
static uint32_t b_SetFrequency(void) { struct dsbuf *b = BF(THIS); lock(); if (ARG(1)) b->rate = (int)ARG(1); unlock(); return DS_OK; }

#define M(n, f, b) { #n, f, b }
static const struct api DS_METHODS[] = {
    M(QueryInterface, 0, 12), M(AddRef, c_AddRef, 4), M(Release, c_Release, 4), M(CreateSoundBuffer, ds_CreateSoundBuffer, 16),
    M(GetCaps, ds_GetCaps, 8), M(DuplicateSoundBuffer, ds_DuplicateSoundBuffer, 12), M(SetCooperativeLevel, ds_ok, 12),
    M(Compact, ds_ok, 4), M(GetSpeakerConfig, 0, 8), M(SetSpeakerConfig, ds_ok, 8), M(Initialize, ds_ok, 8),
};
static const struct api BUF_METHODS[] = {
    M(QueryInterface, 0, 12), M(AddRef, c_AddRef, 4), M(Release, b_Release, 4), M(GetCaps, b_GetCaps, 8),
    M(GetCurrentPosition, b_GetCurrentPosition, 12), M(GetFormat, b_GetFormat, 16), M(GetVolume, b_GetVolume, 8),
    M(GetPan, b_GetPan, 8), M(GetFrequency, b_GetFrequency, 8), M(GetStatus, b_GetStatus, 8), M(Initialize, ds_ok, 12),
    M(Lock, b_Lock, 32), M(Play, b_Play, 16), M(SetCurrentPosition, b_SetCurrentPosition, 8), M(SetFormat, b_SetFormat, 8),
    M(SetVolume, b_SetVolume, 8), M(SetPan, b_SetPan, 8), M(SetFrequency, b_SetFrequency, 8), M(Stop, b_Stop, 4),
    M(Unlock, b_Unlock, 20), M(Restore, ds_ok, 4),
};
static uint32_t api_DirectSoundCreate(void) {
    if (!vt_ds) {
        vt_ds = w32_make_vtable("IDirectSound", DS_METHODS, sizeof DS_METHODS / sizeof DS_METHODS[0]);
        vt_buf = w32_make_vtable("IDirectSoundBuffer", BUF_METHODS, sizeof BUF_METHODS / sizeof BUF_METHODS[0]);
        ds_obj = w32_new_object(vt_ds, 8);
    }
    WR32(ARG(1), ds_obj);
    return DS_OK;
}
const struct api API_DSOUND[] = { M(DirectSoundCreate, api_DirectSoundCreate, 12) };
const int N_DSOUND = 1;
