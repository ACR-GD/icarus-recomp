#!/bin/sh
# Build dist/Icarus-macOS.zip: a universal (Apple Silicon + Intel) Icarus.app, macOS 11+.
# Needs: gen/ (make gen), a universal SDL3 install (SDL_PREFIX, default build/dist-src/sdl-mac-inst,
# see NOTES.md "Releases"), dist/assets/Icarus.icns (tools/mkicon.py).
# The app contains the recompiled program and SDL3 only; no game files.
set -e
cd "$(dirname "$0")/.."
SDL_PREFIX=${SDL_PREFIX:-build/dist-src/sdl-mac-inst}
VERSION=${VERSION:-1.0}
OUT=build/rel-mac
APP=dist/Icarus.app
ARCH="-arch arm64 -arch x86_64 -mmacosx-version-min=11.0"
echo "compiling (universal)..."
make -j"$(sysctl -n hw.ncpu)" BUILD="$OUT" BIN="$OUT/icarus" OPT=-O2 EXTRA="$ARCH" \
  SDL_CFLAGS="-I$SDL_PREFIX/include" SDL_LIBS="-L$SDL_PREFIX/lib -lSDL3" \
  LDEXTRA="-Wl,-rpath,@executable_path/../Frameworks" "$OUT/icarus" > "$OUT.log" 2>&1 || { tail -20 "$OUT.log"; exit 1; }

rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"
cp "$OUT/icarus" "$APP/Contents/MacOS/Icarus"
strip -x "$APP/Contents/MacOS/Icarus"      # drop local symbols and build-path debug references
cp "$SDL_PREFIX/lib/libSDL3.0.dylib" "$APP/Contents/Frameworks/"
cp dist/assets/Icarus.icns "$APP/Contents/Resources/"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Icarus</string>
  <key>CFBundleDisplayName</key><string>Icarus</string>
  <key>CFBundleIdentifier</key><string>com.secondsave.icarus</string>
  <key>CFBundleExecutable</key><string>Icarus</string>
  <key>CFBundleIconFile</key><string>Icarus</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>Recomp by Second Save. Requires your own copy of Icarus: Sanctuary of the Gods.</string>
</dict></plist>
PLIST
codesign --force --deep -s - "$APP"

STAGE=build/rel-mac/stage; rm -rf "$STAGE"; mkdir -p "$STAGE/Icarus"
cp -R "$APP" "$STAGE/Icarus/"
cp packaging/HOW-TO-PLAY.txt "$STAGE/Icarus/"
cp tools/fonts/CrimsonPro-OFL.txt "$STAGE/Icarus/OFL.txt"
cp build/dist-src/SDL3-*/LICENSE.txt "$STAGE/Icarus/SDL-LICENSE.txt"
rm -f dist/Icarus-macOS.zip
(cd "$STAGE" && ditto -c -k --norsrc --noextattr --noqtn --keepParent Icarus ../../../dist/Icarus-macOS.zip)
lipo -info "$APP/Contents/MacOS/Icarus"
ls -la dist/Icarus-macOS.zip
