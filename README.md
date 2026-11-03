# Icarus: Sanctuary of the Gods (Droiyan), static recompilation

A native port of the 1998 Windows 95 RPG *Icarus: Sanctuary of the Gods* (Korean title *Droiyan*,
KRG Soft / JC Research) for macOS, Linux and Windows. The game's x86 code (`ICARUS.EXE`, MSVC 5) is
translated to C ahead of time, and a small runtime replaces the Win32 / DirectX 5 APIs it used with
SDL3.

Nothing is emulated at run time: no Windows, no Wine, no CPU interpreter.

## How it works

| Piece | What it does |
|---|---|
| `tools/recomp.py` | Disassembles `ICARUS.EXE` (Capstone), finds every function (call targets, relocation table, incremental-link thunks, switch tables) and writes one C function per guest function into `gen/`. |
| `runtime/rt.h`, `rt_core.c` | CPU state, lazy flags, x87, string ops, dispatcher for indirect calls, host-to-guest callbacks. |
| `runtime/w32_kernel.c` | Imports and COM vtables point at "magic" addresses served by host functions. Also the guest heap, files (case-insensitive, copy-on-write saves) and process queries. |
| `runtime/w32_user.c` | Window, message queue, keyboard/mouse (SDL3), GDI text, `wsprintf`. |
| `runtime/w32_ddraw.c` | DirectDraw surfaces in guest memory (8- and 16-bit modes), Blt/BltFast/Flip, palettes, presentation. |
| `runtime/w32_dsound.c` | DirectSound buffers mixed in software, plus the CD audio music tracks. |
| `runtime/w32_mm.c` | Multimedia timers (delivered on the game thread), MCI `cdaudio`, the `mmio` RIFF reader. |
| `runtime/w32_avi.c` | AVIFile stream reading and a Cinepak decoder for the game's own movie player. |
| `tools/prepare_data.py`, `tools/is3unpack.c` | Rebuild the installed game from the original discs: ISO9660 extraction, the InstallShield 3 `.Z` archives (PKWARE DCL), and the CD audio tracks. |

## You need

* The two original discs as BIN/CUE images (`ICAR_CD1.cue/.bin`, `ICAR_CD2.cue/.bin`).
* A C compiler (clang or gcc; MinGW-w64 on Windows) and SDL3 (`brew install sdl3`, `apt install libsdl3-dev`, MSYS2 `mingw-w64-x86_64-SDL3`).
* Python 3 with `capstone` and `pefile` to regenerate `gen/` (`pip install capstone pefile`).

## Build

```sh
make build/is3unpack
make data CD1=path/to/ICAR_CD1.cue CD2=path/to/ICAR_CD2.cue   # builds data/ (about 870 MB, 530 MB of it CD music)
make gen PY=python3                                            # translate data/ICARUS.EXE -> gen/
make -j8                                                       # -> ./icarus
./icarus
```

`data/` holds the installed game files, `droiyan.ini` (the CD drive the game reads from: `D:\`),
and `data/cd/` (the run-time CD contents: maps, movies, music as WAV). Saved games and options go to
`save/`. The original files in `data/` are never modified.

## Controls

The game's own controls: mouse for everything, Space or click to skip the intro movie, Esc to skip
the prologue. Alt+Enter toggles fullscreen.

## Environment variables

| Variable | Effect |
|---|---|
| `ICARUS_DATA=dir` | Game data folder (default `./data`, or `data/` next to the binary). |
| `ICARUS_SAVE=dir` | Save folder (default `./save`). |
| `ICARUS_SCALE=n` | Initial window scale (default 2). `ICARUS_FULLSCREEN=1` starts fullscreen. |
| `ICARUS_SMOOTH=1` | Bilinear scaling instead of sharp pixels. |
| `ICARUS_MUTE=1` | No sound. |
| `ICARUS_WARP=1` | Let the game move the host mouse pointer (`SetCursorPos`). |
| `ICARUS_VERBOSE=1` | Log files, surfaces, sound buffers and MCI calls. |
| `ICARUS_TRACE=1` | Log every Win32/COM call (`ICARUS_TRACE_QUIET=Name1,Name2` hides noisy ones). |
| `ICARUS_HEADLESS=1` | No window (testing). With `ICARUS_SHOT=file%04d.ppm` and `ICARUS_SHOT_EVERY=n` it saves frames. |
| `ICARUS_AUDIO_DUMP=file.raw` | Also write the mixed audio (s16 stereo 44.1 kHz) to a file. |
| `ICARUS_FPS=n` | Vertical-blank rate that Flip and on-screen palette changes are paced to (default 60). |
| `ICARUS_KEYS=t:vk,...`, `ICARUS_CLICKS=t:x:y[:right],...` | Scripted input at t seconds (testing). |

`tools/run_headless.sh SECONDS` and `tools/shots.sh T...` wrap the testing knobs.

## Sharing it

The recompiler, runtime and tools are original code. `gen/` (the translated game code) and `data/`
are derived from the copyrighted game, so share the source tree and let players run
`make data` / `make gen` against their own discs, rather than publishing `gen/`, `data/` or a
prebuilt binary that bundles them.

## Status

Working: boot logos, title menu, intro movie (Cinepak + audio), prologue, CD music, sound effects,
the opening scene and dialogue. See `NOTES.md` for what has and has not been tested yet.

## Continuous integration

`.github/workflows/build.yml` builds the runtime and tools on Linux (Debian trixie), macOS
(Homebrew) and Windows (MSYS2 UCRT64, clang) against `ci/stub-gen`, a stand-in for `gen/` with no
game code, and runs `ci/smoke.sh`. The playable game is only ever built locally from your own discs.
