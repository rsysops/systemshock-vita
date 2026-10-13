#!/usr/bin/env bash
# Builds the resource and movie libraries natively and checks, for every audio
# log and bark of the game's data, that its sound read a block at a time from
# the file (how a log is played) is byte for byte its sound read whole from
# memory (how it was played before).
# Usage: tests/alog/run.sh <the game's DATA folder>
set -euo pipefail

DATA="$(cd "${1:?usage: tests/alog/run.sh <the game\'s DATA folder>}" && pwd)"
cd "$(dirname "$0")/../.."

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

L=src/Libraries
INCLUDES="-I$L/2D/Source -I$L/2D/Source/GR -I$L/2D/Source/Clip -I$L/2D/Source/Flat8 -I$L/2D/Source/Gen \
-I$L/2D/Source/RSD -I$L/3D/Source -I$L/H -I$L/FIX/Source -I$L/LG/Source -I$L/LG/Source/LOG/src \
-I$L/DSTRUCT/Source -I$L/RES/Source -I$L/AFILE/Source"

# The source files of a list in the libraries' CMakeLists.txt
sources() { # list name
    awk -v name="$1" '$0 == "set(" name {on = 1; next} on && /^\)/ {on = 0} on {sub(/\)$/, "", $1); print $1}
                      on && /\)$/ {on = 0}' "$L/CMakeLists.txt"
}

mkdir "$OUT/obj"
# fix_pow.c duplicates a function of fix.c
# The movie library's picture decoders (the .cpp files) are 32-bit code and a
# log has no picture: the test stands in for the two functions they export.
{ sources 2D_SRC; sources LG_SRC; sources FIX_SRC | grep -v fix_pow.c; sources DSTRUCT_SRC; sources RES_SRC
  sources AFILE_SRC; } | grep '\.c$' |
    xargs -P "$(nproc)" -I{} sh -c \
        'gcc -O2 -fcommon -w -pthread -DLG_SLOT_PTHREADS '"$INCLUDES"' -c "'"$L"'/{}" -o "'"$OUT"'/obj/$(echo {} | tr / _).o"'
ar rcs "$OUT/libs.a" "$OUT"/obj/*.o
# shellcheck disable=SC2086
gcc -O2 -g -fcommon -pthread $INCLUDES tests/alog/alog_test.c "$OUT/libs.a" -lm -o "$OUT/alog_test"

files=()
for name in citalog frnalog geralog citbark frnbark gerbark; do
    found="$(find "$DATA" -maxdepth 1 -iname "$name.res" | head -1)"
    [[ -n "$found" ]] && files+=("$found")
done
if [[ ${#files[@]} -eq 0 ]]; then
    echo "no audio log file in $DATA"
    exit 1
fi

if "$OUT/alog_test" "${files[@]}"; then
    echo "OK"
else
    echo "FAILED"
    exit 1
fi
