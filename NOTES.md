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
* Save / load from the in-game menu. Saves go to `save/`; copy-on-write keeps `data/` pristine.
* Listening to the audio by ear. `ICARUS_AUDIO_DUMP` shows the title CD track and the movie
  soundtrack reach the mixer at normal levels. Battle sound effects use the same DirectSound path.
* ENDING.AVI, later chapters, every spell or effect path.
* A Windows build (the code avoids POSIX-only APIs except `dirent.h` and `mkdir`, which MinGW-w64
  provides).

## Debugging tips
* `ICARUS_TRACE=1 ICARUS_TRACE_QUIET=PeekMessageA,GetLocalTime,GetSystemTime,SetEntries,SetPalette`
  gives a readable API log.
* `tools/disasm.py data/ICARUS.EXE ADDR [N]` disassembles with import names.
* A FATAL prints guest registers, the last 256 dispatch targets and the host backtrace
  (`f_XXXXXXXX` = guest function at that address).
* The guest's own state in `save/droiyan.bac` (written at New Game) persists between runs, like on
  the original install.
