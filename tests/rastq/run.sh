#!/usr/bin/env bash
# Builds the 2D, LG and FIX libraries and the rasterizer queue natively and
# checks that record + replay draws the same pixels as drawing directly.
# Usage: tests/rastq/run.sh [frames per canvas size] [seed]
set -euo pipefail

cd "$(dirname "$0")/../.."

FRAMES="${1:-1000}"
SEED="${2:-}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

L=src/Libraries
INCLUDES=(-I"$L/2D/Source" -I"$L/2D/Source/GR" -I"$L/2D/Source/Clip" -I"$L/2D/Source/Flat8"
          -I"$L/2D/Source/Gen" -I"$L/2D/Source/RSD" -I"$L/3D/Source" -I"$L/H" -I"$L/FIX/Source"
          -I"$L/LG/Source" -I"$L/LG/Source/LOG/src" -I"$L/DSTRUCT/Source" -I"$L/RES/Source")

# The .c files of a source list in the libraries' CMakeLists.txt
sources() {
    awk -v name="$1" '$0 == "set(" name {on = 1; next} on && /^\)/ {on = 0} on {print $1}' "$L/CMakeLists.txt" |
        grep '\.c$'
}

# fix_pow.c duplicates a function of fix.c
LIB_SOURCES="$(sources 2D_SRC) $(sources LG_SRC) $(sources FIX_SRC | grep -v fix_pow.c)"

mkdir "$OUT/lib"
for f in $LIB_SOURCES; do
    echo "$f"
done | xargs -P "$(nproc)" -I{} sh -c \
    'gcc -O2 -fcommon -w "$@" -c "'"$L"'/{}" -o "'"$OUT"'/lib/$(echo {} | tr / _).o"' sh "${INCLUDES[@]}"
ar rcs "$OUT/lib2d.a" "$OUT"/lib/*.o

status=0
build() { # output name, extra compiler flags
    gcc -O2 -g -fcommon -DRASTQ_SELFCHECK "${@:2}" "${INCLUDES[@]}" \
        "$L/3D/Source/rastq.c" tests/rastq/rastq_test.c "$OUT/lib2d.a" -lm -o "$OUT/$1"
}

echo "default arena:"
build plain
"$OUT/plain" "$FRAMES" $SEED || status=1

# A small arena makes scenes flush several times.
echo "small arena:"
build small "-DRASTQ_ARENA_BYTES=(320*1024)"
"$OUT/small" "$FRAMES" $SEED || status=1

# The sanitizers are slow, so they get a tenth of the frames.
echo "address and undefined-behaviour sanitizers on the recorder and the test:"
build sanitized -fsanitize=address,undefined -fno-sanitize-recover=undefined
ASAN_OPTIONS=detect_leaks=0 "$OUT/sanitized" "$((FRAMES / 10 + 1))" $SEED || status=1

if [[ $status -ne 0 ]]; then
    echo "FAILED"
else
    echo "OK"
fi
exit $status
