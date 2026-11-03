#!/bin/sh
# Build dist/Icarus-Linux-x86_64.tar.gz in an Ubuntu 22.04 (glibc 2.35) x86-64 container, with
# SDL3 built from source and bundled (lib/, found through an $ORIGIN rpath).
# Needs: gen/ (make gen), SDL3 source unpacked in build/dist-src (SDL3-<version>/).
# Contains the recompiled program and SDL3 only. On an Apple Silicon Mac this runs emulated: slow.
set -e
cd "$(dirname "$0")/.."
SDLSRC=$(ls -d build/dist-src/SDL3-*/ | grep -v mingw | head -1)
[ -f "$SDLSRC/CMakeLists.txt" ] || { echo "SDL3 source not found in build/dist-src"; exit 1; }
docker run --rm --platform linux/amd64 -v "$PWD":/src -w /src ubuntu:22.04 sh -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq build-essential cmake ninja-build pkg-config libasound2-dev libpulse-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libegl1-mesa-dev libdbus-1-dev libudev-dev \
  libwayland-dev wayland-protocols libdecor-0-dev patchelf >/dev/null 2>&1
OUT=build/rel-linux; mkdir -p $OUT
if [ ! -f $OUT/sdl-inst/lib/libSDL3.so ]; then
  cmake -S '"$SDLSRC"' -B $OUT/sdl -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX=/src/$OUT/sdl-inst >/dev/null
  cmake --build $OUT/sdl >/dev/null && cmake --install $OUT/sdl >/dev/null
fi
make -j$(nproc) CC=gcc BUILD=$OUT/obj BIN=$OUT/icarus OPT=-O2 \
  SDL_CFLAGS="-I$OUT/sdl-inst/include" SDL_LIBS="-L$OUT/sdl-inst/lib -lSDL3" \
  $OUT/icarus > $OUT.log 2>&1 || { grep -E "error" $OUT.log | head -30; exit 1; }
strip $OUT/icarus
patchelf --set-rpath "\$ORIGIN/lib" $OUT/icarus   # bundled SDL3 in lib/ next to the program
readelf -d $OUT/icarus | grep -E "RPATH|RUNPATH"
ldd $OUT/icarus | sed "s/ (0x.*//"
objdump -T $OUT/icarus | grep -o "GLIBC_[0-9.]*" | sort -V | tail -1
'
STAGE=build/rel-linux/stage; rm -rf "$STAGE"; mkdir -p "$STAGE/Icarus/lib"
cp build/rel-linux/icarus "$STAGE/Icarus/"
cp -P build/rel-linux/sdl-inst/lib/libSDL3.so.0* "$STAGE/Icarus/lib/"
cp packaging/HOW-TO-PLAY.txt "$STAGE/Icarus/"
cp tools/fonts/CrimsonPro-OFL.txt "$STAGE/Icarus/OFL.txt"
cp "$SDLSRC/LICENSE.txt" "$STAGE/Icarus/SDL-LICENSE.txt"
cp dist/assets/icon_1024.png "$STAGE/Icarus/icarus.png"
rm -f dist/Icarus-Linux-x86_64.tar.gz
# neutral owner in the archive (no local user name)
tar -C "$STAGE" --uid 0 --gid 0 --uname root --gname root -czf dist/Icarus-Linux-x86_64.tar.gz Icarus
ls -la dist/Icarus-Linux-x86_64.tar.gz
