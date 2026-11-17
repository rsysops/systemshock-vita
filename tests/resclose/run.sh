#!/usr/bin/env bash
# Builds the resource library natively and checks that closing a resource
# file says when writing it failed, as on a full memory card (played by the
# file size limit), and not when it didn't.
# Usage: tests/resclose/run.sh
set -euo pipefail

cd "$(dirname "$0")/../.."

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

L=src/Libraries
INCLUDES="-I$L/2D/Source -I$L/2D/Source/GR -I$L/2D/Source/Clip -I$L/2D/Source/Flat8 -I$L/2D/Source/Gen \
-I$L/2D/Source/RSD -I$L/3D/Source -I$L/H -I$L/FIX/Source -I$L/LG/Source -I$L/LG/Source/LOG/src \
-I$L/DSTRUCT/Source -I$L/RES/Source"

# The source files of a list in the libraries' CMakeLists.txt
sources() { # list name
    awk -v name="$1" '$0 == "set(" name {on = 1; next} on && /^\)/ {on = 0} on {sub(/\)$/, "", $1); print $1}
                      on && /\)$/ {on = 0}' "$L/CMakeLists.txt"
}

mkdir "$OUT/obj"
{ sources 2D_SRC; sources LG_SRC; sources FIX_SRC; sources DSTRUCT_SRC; sources RES_SRC; } | grep '\.c$' |
    xargs -P "$(nproc)" -I{} sh -c \
        'gcc -O2 -fcommon -w -pthread -DLG_SLOT_PTHREADS '"$INCLUDES"' -c "'"$L"'/{}" -o "'"$OUT"'/obj/$(echo {} | tr / _).o"'
ar rcs "$OUT/libs.a" "$OUT"/obj/*.o
# shellcheck disable=SC2086
gcc -O2 -g -Wall -fcommon -pthread $INCLUDES tests/resclose/resclose_test.c "$OUT/libs.a" -lm -o "$OUT/resclose_test"

mkdir "$OUT/work"
if "$OUT/resclose_test" "$OUT/work"; then
    echo "OK"
else
    echo "FAILED"
    exit 1
fi
