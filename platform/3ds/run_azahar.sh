#!/usr/bin/env bash
# Run the 3DS build in the project's private Azahar instance (~/devkitpro-3ds/emu-noma, with its
# own sdmc and nand) for a while, then collect the logs. Kills only the emulator it started.
#
#   platform/3ds/run_azahar.sh [SECONDS] [FILE]     (default: 60 s, build-3ds/noma.cxi)
#   SHOTS="30 60 90" also captures the window at those seconds (runs/azahar/shot_N.png)
set -uo pipefail
SECS="${1:-60}"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FILE="${2:-$ROOT/build-3ds/noma.cxi}"
D="$HOME/devkitpro-3ds/emu-noma"
APP="$D/Azahar.app"
AZ_LOG="$D/user/log/azahar_log.txt"
SD="$D/user/sdmc/3ds/noma"
OUT="$ROOT/runs/azahar"
mkdir -p "$OUT"
rm -f "$SD/noma.log"
mkdir -p "$(dirname "$AZ_LOG")"; : > "$AZ_LOG"

before="$(pgrep -f "$APP/Contents/MacOS/azahar" | sort || true)"
open -n -a "$APP" --args -w "$FILE"
sleep 3
PID=""
for p in $(pgrep -f "$APP/Contents/MacOS/azahar" | sort); do
  echo "$before" | grep -qx "$p" || PID="$p"
done
[ -n "$PID" ] || { echo "Azahar did not start" >&2; exit 1; }
echo "azahar pid $PID, running ${SECS}s"
# the window's CGWindowID for screencapture (a small CoreGraphics helper, built on first use)
WINID="$ROOT/build/winid"
[ -x "$WINID" ] || clang -O2 -o "$WINID" "$ROOT/tools/mac/winid.c" -framework CoreGraphics \
    -framework CoreFoundation 2>/dev/null || true
shot() { local id; id=$("$WINID" "$PID" 2>/dev/null) && screencapture -x -o -l "$id" "$1" 2>/dev/null || true; }
rm -f "$OUT"/shot_*.png "$OUT/window.png"
start=$SECONDS
end=$((SECONDS + SECS))
while [ $SECONDS -lt $end ] && kill -0 "$PID" 2>/dev/null; do
  sleep 1
  for t in ${SHOTS:-}; do [ $((SECONDS - start)) -eq "$t" ] && shot "$OUT/shot_$t.png"; done
done
if kill -0 "$PID" 2>/dev/null; then
  shot "$OUT/window.png"
  kill -9 "$PID"; echo "stopped Azahar after ${SECS}s"
else
  echo "Azahar exited by itself"
fi
sleep 1
cp "$AZ_LOG" "$OUT/azahar.log" 2>/dev/null || : > "$OUT/azahar.log"
cp "$SD/noma.log" "$OUT/noma.log" 2>/dev/null || : > "$OUT/noma.log"
echo "logs in $OUT"
