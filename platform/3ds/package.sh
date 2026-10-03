#!/bin/sh
# Package the 3DS build: noma.3dsx (Homebrew Launcher) and noma.cia (installable title).
#
#   platform/3ds/package.sh [BUILD_DIR]      (default: build-3ds)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$ROOT/build-3ds}
TOOLS=${DEVKITPRO:-/opt/devkitpro}/tools/bin
[ -x "$TOOLS/makerom" ] || TOOLS=$HOME/devkitpro-3ds/tools/bin
META=$ROOT/platform/3ds/meta
ELF=$BUILD/noma
OUT=$BUILD

"$TOOLS/bannertool" makesmdh -s "Wii no Ma" -l "Wii no Ma (Wii Room) for 3DS" -p "noma" \
    -i "$META/icon.png" -o "$OUT/noma.smdh"
"$TOOLS/bannertool" makebanner -i "$META/banner.png" -a "$META/banner.wav" -o "$OUT/noma.bnr"
# the ELF carries debug info; the packages don't need it
/opt/devkitpro/devkitARM/bin/arm-none-eabi-strip --strip-debug -o "$OUT/noma.stripped.elf" "$ELF"
"$TOOLS/3dsxtool" "$OUT/noma.stripped.elf" "$OUT/noma.3dsx" --smdh="$OUT/noma.smdh"
"$TOOLS/makerom" -f cia -o "$OUT/noma.cia" -elf "$OUT/noma.stripped.elf" \
    -rsf "$ROOT/platform/3ds/cia/noma.rsf" -icon "$OUT/noma.smdh" -banner "$OUT/noma.bnr" -target t
ls -la "$OUT/noma.3dsx" "$OUT/noma.cia"
# a .cxi too: emulators boot it directly, with the exheader's memory mode
"$TOOLS/makerom" -f cxi -o "$OUT/noma.cxi" -elf "$OUT/noma.stripped.elf" \
    -rsf "$ROOT/platform/3ds/cia/noma.rsf" -icon "$OUT/noma.smdh" -banner "$OUT/noma.bnr" -target t
