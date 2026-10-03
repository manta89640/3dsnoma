#!/bin/sh
# Fetch m2c and apply this project's changes (guest-memory C++ output, the PowerPC register
# ABI, and fixes for decompiling a program that has to run, not just match).
#
#   tools/m2c/setup.sh [DIR]     (default: ~/devkitpro-3ds/decomp-tools/m2c)
#
# tools/decomp/decompile.py finds m2c through M2C_DIR (default: the directory above).
set -e
DIR=${1:-$HOME/devkitpro-3ds/decomp-tools/m2c}
BASE=708d2d2   # upstream commit the patch applies to
HERE=$(cd "$(dirname "$0")" && pwd)
if [ ! -d "$DIR/.git" ]; then
    git clone https://github.com/matt-kempster/m2c.git "$DIR"
fi
cd "$DIR"
git checkout -q -B noma "$BASE"
git apply "$HERE/noma.patch"
echo "m2c with the noma patch: $DIR"
