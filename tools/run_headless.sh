#!/bin/sh
# run_headless.sh SECONDS [log] : run the game without a window for N seconds.
# Frames are saved every $EVERY presents (default 10) as build/shots/sTTTT.ppm (TTTT = tenths of a
# second since start); the last one is also converted to build/shot.png (needs sips or ImageMagick).
# FRESH=1 starts from an empty test save folder. Extra env (ICARUS_KEYS, ICARUS_CLICKS, ICARUS_VERBOSE, ...) is passed through.
T=${1:-10}; LOG=${2:-build/run.log}
rm -rf build/shots; mkdir -p build/shots
# tests never touch the player's save/ folder: they use build/testsave (wiped when FRESH=1)
[ "${FRESH:-0}" = 1 ] && rm -rf build/testsave
ICARUS_SAVE=${ICARUS_SAVE:-build/testsave} ICARUS_HEADLESS=1 ICARUS_MUTE=1 ICARUS_SHOT=build/shots/s%04d.ppm ICARUS_SHOT_EVERY=${EVERY:-10} ./icarus > "$LOG" 2>&1 &
PID=$!
i=0; while [ $i -lt "$T" ] && kill -0 $PID 2>/dev/null; do sleep 1; i=$((i+1)); done
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null
LAST=$(ls build/shots/*.ppm 2>/dev/null | tail -1)
[ -n "$LAST" ] && (sips -s format png "$LAST" --out build/shot.png >/dev/null 2>&1 || convert "$LAST" build/shot.png 2>/dev/null)
echo "last frame: $LAST"
tail -3 "$LOG"
