#!/usr/bin/env bash
# Builds the 2D and resource libraries and the game's text drawing natively
# and checks, with the game's own fonts in each of its screen modes, that an
# outlined text copied from its kept picture is pixel for pixel the nine
# draws it stands for.
# Usage: tests/hudkeep/run.sh <the game's DATA folder> [texts per font and mode] [seed]
set -euo pipefail

DATA="$(cd "${1:?usage: tests/hudkeep/run.sh <the game\'s DATA folder> [texts per font and mode] [seed]}" && pwd)"
shift
cd "$(dirname "$0")/../.."

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

L=src/Libraries
INCLUDES="-I$L/2D/Source -I$L/2D/Source/GR -I$L/2D/Source/Clip -I$L/2D/Source/Flat8 -I$L/2D/Source/Gen \
-I$L/2D/Source/RSD -I$L/3D/Source -I$L/H -I$L/FIX/Source -I$L/LG/Source -I$L/LG/Source/LOG/src \
-I$L/DSTRUCT/Source -I$L/RES/Source -Isrc/GameSrc/Headers"
# The game's sources get these from a header given to every file (precompiled.h),
# which also brings in SDL: only what the two files here need of it.
GAME="-include stdbool.h -include lg_types.h -include 2d.h -include fix.h -include rect.h -include res.h \
-include 2dres.h"

# The source files of a list in the libraries' CMakeLists.txt
sources() { # list name
    awk -v name="$1" '$0 == "set(" name {on = 1; next} on && /^\)/ {on = 0} on {sub(/\)$/, "", $1); print $1}
                      on && /\)$/ {on = 0}' "$L/CMakeLists.txt"
}

mkdir "$OUT/obj"
# fix_pow.c duplicates a function of fix.c
{ sources 2D_SRC; sources LG_SRC; sources FIX_SRC | grep -v fix_pow.c; sources DSTRUCT_SRC; sources RES_SRC; } |
    grep '\.c$' |
    xargs -P "$(nproc)" -I{} sh -c \
        'gcc -O2 -fcommon -w -pthread -DLG_SLOT_PTHREADS '"$INCLUDES"' -c "'"$L"'/{}" -o "'"$OUT"'/obj/$(echo {} | tr / _).o"'
ar rcs "$OUT/libs.a" "$OUT"/obj/*.o
# hudkeep.c is part of the test's own file, which looks into it
# shellcheck disable=SC2086
gcc -O2 -g -fcommon -fsigned-char -fno-strict-aliasing -w -pthread $GAME $INCLUDES \
    tests/hudkeep/hudkeep_test.c src/GameSrc/gr2ss.c "$OUT/libs.a" -lm -o "$OUT/hudkeep_test"

FONTS="$(find "$DATA" -maxdepth 1 -iname gamescr.res | head -1)"
if [[ -z "$FONTS" ]]; then
    echo "no gamescr.res in $DATA"
    exit 1
fi

if "$OUT/hudkeep_test" "$FONTS" "$@"; then
    echo "OK"
else
    echo "FAILED"
    exit 1
fi
