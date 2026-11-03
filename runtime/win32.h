// Win32 high-level emulation shared declarations.
#pragma once
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------- guest memory layout
#define TIB_ADDR    0x00010000u    // thread information block (FS base)
#define HOSTSTR     0x00011000u    // command line, environment, module name
#define STACK_LO    0x00100000u
#define STACK_HI    0x003F0000u
#define HEAP_LO     0x01000000u
#define HEAP_HI     MEM_SIZE

uint32_t gmem_alloc(uint32_t size);          // zeroed, 16-byte aligned, 0 on failure
void gmem_free(uint32_t addr);
uint32_t gmem_size(uint32_t addr);
uint32_t gmem_realloc(uint32_t addr, uint32_t size);
static inline void *GP(uint32_t a) { return MEM + (a & MEM_MASK); }
void gstr_get(uint32_t a, char *out, int n);
void gstr_put(uint32_t a, const char *s);

// ---------------------------------------------------------------- host functions
// Host implementations of imported functions and COM methods. They read their arguments with
// ARG(n) (the return address is at ESP), return the EAX value, and argbytes says how much the
// callee pops (stdcall); -1 marks cdecl.
#define ARG(n) RD32(ESP + 4 + 4 * (n))
typedef uint32_t (*apifn)(void);
struct api { const char *name; apifn fn; int argbytes; };
uint32_t w32_register(const char *name, apifn fn, int argbytes);   // returns magic address
void w32_call_host(uint32_t magic);
// builds a COM vtable in guest memory from a method list (NULL fn = logged stub)
uint32_t w32_make_vtable(const char *iface, const struct api *methods, int n);
uint32_t w32_new_object(uint32_t vtable, uint32_t size);

extern const struct api API_KERNEL32[], API_USER32[], API_GDI32[], API_WINMM[], API_DDRAW[], API_DSOUND[],
    API_AVIFIL32[], API_MSVFW32[];
extern const int N_KERNEL32, N_USER32, N_GDI32, N_WINMM, N_DDRAW, N_DSOUND, N_AVIFIL32, N_MSVFW32;

// ---------------------------------------------------------------- subsystems
void w32_init(int argc, char **argv);
void w32_shutdown(void);
void w32_async_callbacks(void);      // deliver due multimedia timer callbacks (from rt_irq)
void w32_pump_events(void);          // SDL events -> window messages / input state
uint32_t w32_tick_ms(void);          // GetTickCount/timeGetTime clock
extern int w32_log_api;              // ICARUS_TRACE=1: log every host call

// paths: guest "C:\..." / relative / "D:\..." -> host files
int host_path(const char *guest, char *out, size_t n, int for_write);
extern char game_dir[1024], cd_dir[1024], save_dir[1024];

// display (w32_ddraw.c / w32_user.c)
void video_init(void);
void video_present(uint32_t pixels, int pitch, int w, int h, int bpp, const uint32_t *pal);
void video_shutdown(void);
extern uint32_t main_hwnd, main_wndproc;
uint32_t ddraw_primary_pixels(int *pitch, int *w, int *h);
void gdi_draw_text(uint32_t pixels, int pitch, int w, int h, int bpp, int x, int y, const char *s, int n,
                   uint32_t color, int height);

// audio (w32_dsound.c)
void audio_init(void);
void audio_shutdown(void);
void cdda_play(int from_track, int to_track);
void cdda_stop(void);
int cdda_playing(void);
void movie_play(const char *host_path);

// portability (macOS / Linux / Windows-MinGW)
void host_sleep_ms(uint32_t ms);
void host_frame_pace(void);
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define FSEEK64 _fseeki64
#define FTELL64 _ftelli64
#else
#define MKDIR(p) mkdir((p), 0755)
#define FSEEK64 fseeko
#define FTELL64 ftello
#endif

// misc
#define LOG(...) do { if (rt_verbose) fprintf(stderr, __VA_ARGS__); } while (0)
extern int rt_verbose;
