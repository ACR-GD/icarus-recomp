# Working notes

## Source material
* `Icarus_CD1/ICAR_CD1.bin/.cue`: data only. InstallShield 3 setup with `EXE.Z` (ICARUS.EXE),
  `ROOT.Z` (installed root files), `EDATA.Z` and `TDATA.Z` (edata\, tdata\), plus MAP\, OPEN.AVI and
  ENDING.AVI.
* `Icarus_CD2/ICAR_CD2.bin/.cue`: track 1 is data (MAP\, AVIs, identical to CD1's), tracks 2–19 are
  the CD-DA soundtrack. The game needs CD2 in the drive while playing.
* The installer writes `droiyan.ini` (3 bytes: the CD drive, e.g. `D:\`). The game reads it at start-up
  and prefixes `map\` and `*.avi` with it.

## ICARUS.EXE
* MSVC 5, debug PDB path `C:\DROY-WIN\DROYUSA\Debug\Icarus.pdb`, image base 0x400000, has relocations.
* Incremental-link thunk table at 0x401000 (`jmp func`).
* WndProc 0x43af80 (thunk 0x4017d0). Mouse position goes to 0x52a4c4/0x52a4c8, buttons to 0x5281cc.
* Display modes: 640x480x8 for the game, 640x480x16 for the movie player.
* Movie player: AVIStreamOpenFromFile('vids') at 0x40ff80, audio stream at 0x4108e0, timer proc
  0x410680 (calls `this->vtbl[4]`), key polling loop 0x43cc8d (Space / left / right button skip).
* CD music: MCI `cdaudio` through the wrapper at 0x42f810 (`mciSendCommand` OPEN/SET TMSF/STATUS/PLAY with
  NOTIFY). MM_MCINOTIFY (0x3b9) goes back to the WndProc.
* Errors are C++ exceptions (`RaiseException 0xE06D7363`). The runtime treats them as fatal, which
  is fine because they only fire on missing files or failed API calls.

## Verified (macOS arm64, headless and windowed)
* Boot: ST Entertainment and KRG Soft logos, palette fades, title menu with palette animation and
  music (track 15).
* NEW GAME: intro movie OPEN.AVI (Cinepak 320x240, 16-bit mode, synced to the stream timer), Space skips it.
* Prologue slideshow (8-bit, track 14), Esc skips it.
* First map: dialogue with portraits and the game's bitmap font, CD music track 2, scene change to the
  forest, the first tactical battle (movement grid, HP/MP bars, command bar).
* A 4.5-minute click-through soak: no crash, music changed through tracks 6, 7 and 10.
* CPU: 3–8 % headless and about 14 % windowed once Flip and on-screen palette changes are paced to
  60 Hz (`ICARUS_FPS`).
* Linux (Debian trixie arm64 in Docker, clang, SDL3 from apt): builds with no warnings and reaches the
  title menu headless.

## Not verified yet
* Which skill uses DROYSND.SNF entry 51 (Power Wave is the suspect). Entry numbers come from
  data tables, so `ICARUS_SNDTEST` was used to check that entry 51 now loads and plays.
* Save / load from the in-game menu. Saves go to `save/`; copy-on-write keeps `data/` pristine.
* Listening to the audio by ear. `ICARUS_AUDIO_DUMP` shows the title CD track and the movie
  soundtrack reach the mixer at normal levels. Battle sound effects use the same DirectSound path.
* ENDING.AVI, later chapters, every spell or effect path.
* A Windows build (the code avoids POSIX-only APIs except `dirent.h` and `mkdir`, which MinGW-w64
  provides).

## Text
* Dialogue lives in `edata/EVENTnn.DAT` as plain ASCII: a space-padded speaker name, then
  NUL-terminated lines (the start of each file still has Korean leftovers).
* Font: `ENGLISH.FNT`, 256 glyphs of 8x16 one-bit cells in CP437 layout, loaded to 0x5eb030
  (`HANGUL.FNT` for two-byte Korean text, unused in this release).
* Every character goes through 0x449f60(char, colour). It copies the glyph and plots its set bits in
  one palette colour into the 8-bit buffer [0x5ecb30] (pitch [0x531374]) at pen
  ([0x5eedbc], [0x5eedc0]), then advances the pen 9 px (4 px for a space). 0x44a000 draws a
  string; 0x44a0d0 is the typewriter used by dialogue boxes, which redraws the text every frame.
* `runtime/text.c` hooks 0x449f60: anti-aliased Crimson Pro SemiBold 16 px on the same baseline
  (row 11), proportional advance, shadow at +1,+1, coverage blended through the screen palette.

## Sound
* UI sounds: `DROYSNDT.SNF`. Game and battle sounds: `DROYSND.SNF`, 81 entries (u16 count, u32
  offsets, then WAV files). They are loaded on demand through 0x444120(slot, entry): fopen to read
  the offset, `mmioOpen` + `mmioSeek`, then the RIFF parser at 0x450cf0, then a DirectSound buffer
  in one of the slots. Playback goes through 0x4485b0 → 0x432290 (SetCurrentPosition(0), Play).
* The parser only closes the mmio handle on success. Entry 51 is a Sun `.snd` (mu-law, 22 kHz),
  not a WAV, so every attempt to load it leaked a handle and failed. With 32 emulated handles the
  leaks eventually disabled all sound loading. The runtime now reclaims handles (LRU) and presents
  AU entries as WAV, so entry 51 plays. On the original release it was most likely silent.
* CD music: on PLAY over a playing track the game sets a flag to ignore the next MM_MCINOTIFY
  (expecting MCI_NOTIFY_SUPERSEDED). Without SUPERSEDED/ABORTED notifications the real
  end-of-track notice was eaten and music never looped. Notifications are now delivered from the
  event pump.

## Presentation and input (quality of life)
* Sprites: RLE, 0 = skip N, 1-3/0xff = transparent pixel, 0xfe = next row, 0xfd = end, other
  bytes = palette index. Data pointers u32[0x531f70], widths u16[0x526300], heights u16[0x52c290].
  0x44a7a0(x, y, sprite, mode) draws one. The mouse cursor and dragged items are drawn with it at
  exactly (mouse_x, mouse_y) = ([0x52a4c4], [0x52a4c8]). `runtime/qol.c` turns those into the OS
  cursor at each Flip.
* 0x43c9b0 is the game's per-frame "show": SetCursor(NULL), then Flip.
* Dialogue typewriter state: shown 0x5c6768, tick 0x5c676c, active 0x5c6770, total 0x5c6774. The
  line ends when shown == total, so the speed hook never goes past total - 1.
* Boot logos wait on time() (GetLocalTime/GetSystemTime). Logo skip advances the guest clock and
  drops pacing until the first CD track (the title music).
* Measured on a 75 Hz display: 60 fps host pacing showed 3 of every 4 frames for one refresh and
  the fourth for two (judder). Vsync at the display rate gives a steady 75 fps.

## Debugging tips
* `ICARUS_TRACE=1 ICARUS_TRACE_QUIET=PeekMessageA,GetLocalTime,GetSystemTime,SetEntries,SetPalette`
  gives a readable API log.
* `tools/disasm.py data/ICARUS.EXE ADDR [N]` disassembles with import names.
* A FATAL prints guest registers, the last 256 dispatch targets and the host backtrace
  (`f_XXXXXXXX` = guest function at that address).
* The guest's own state in `save/droiyan.bac` (written at New Game) persists between runs, like on
  the original install.
