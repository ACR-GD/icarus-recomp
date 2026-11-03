#!/bin/sh
# Build dist/Icarus-Windows.zip (64-bit Windows 10+) by cross-compiling with MinGW-w64 in Docker.
# Needs: gen/ (make gen), the SDL3 MinGW development package unpacked in build/dist-src
# (SDL3-devel-<version>-mingw.tar.gz from https://github.com/libsdl-org/SDL/releases),
# dist/assets/icarus.ico (tools/mkicon.py). Contains the recompiled program and SDL3 only.
set -e
cd "$(dirname "$0")/.."
SDLW=$(ls -d build/dist-src/SDL3-*/x86_64-w64-mingw32 | head -1)
[ -d "$SDLW" ] || { echo "SDL3 MinGW package not found in build/dist-src"; exit 1; }
docker run --rm -v "$PWD":/src -w /src debian:trixie sh -c '
set -e
apt-get update -qq >/dev/null && apt-get install -y -qq make gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64 >/dev/null 2>&1
OUT=build/rel-win; mkdir -p $OUT
printf "1 ICON \"dist/assets/icarus.ico\"\n" > $OUT/icon.rc
x86_64-w64-mingw32-windres $OUT/icon.rc -O coff -o $OUT/icon.res
make -j$(nproc) CC=x86_64-w64-mingw32-gcc BUILD=$OUT BIN=$OUT/Icarus.exe OPT=-O2 \
  SDL_CFLAGS="-I'"$SDLW"'/include" SDL_LIBS="-L'"$SDLW"'/lib -l:libSDL3.dll.a" \
  LDEXTRA="$OUT/icon.res -mwindows -static -Wl,--stack,33554432" $OUT/Icarus.exe > $OUT.log 2>&1 || { grep -E "error" $OUT.log | head -30; exit 1; }
x86_64-w64-mingw32-strip $OUT/Icarus.exe
x86_64-w64-mingw32-objdump -p $OUT/Icarus.exe | grep "DLL Name" | sort -u
'
STAGE=build/rel-win/stage; rm -rf "$STAGE"; mkdir -p "$STAGE/Icarus"
cp build/rel-win/Icarus.exe "$STAGE/Icarus/"
cp "$SDLW/bin/SDL3.dll" "$STAGE/Icarus/"
# Windows readers expect CRLF line endings
sed 's/$/\r/' packaging/HOW-TO-PLAY.txt > "$STAGE/Icarus/HOW-TO-PLAY.txt"
sed 's/$/\r/' tools/fonts/CrimsonPro-OFL.txt > "$STAGE/Icarus/OFL.txt"
sed 's/$/\r/' "$(dirname "$SDLW")/LICENSE.txt" > "$STAGE/Icarus/SDL-LICENSE.txt"
rm -f dist/Icarus-Windows.zip
(cd "$STAGE" && zip -qr ../../../dist/Icarus-Windows.zip Icarus)
ls -la dist/Icarus-Windows.zip
