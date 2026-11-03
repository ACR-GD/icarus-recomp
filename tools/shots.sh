#!/bin/sh
# shots.sh T1 T2 ... : convert the frames nearest to the given times (seconds) to build/shots/at_T.png
for t in "$@"; do
  want=$(echo "$t" | awk '{printf "%d", $1*10}')
  f=$(ls build/shots/*.ppm | awk -F'build/shots/s|.ppm' -v w="$want" '{d=$2-w; if (d<0) d=-d; if (best=="" || d<bd) {best=$0; bd=d}} END {print best}')
  sips -s format png "$f" --out "build/shots/at_$t.png" >/dev/null 2>&1 || convert "$f" "build/shots/at_$t.png"
  echo "build/shots/at_$t.png <- $f"
done
