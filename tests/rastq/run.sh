#!/usr/bin/env bash
# Builds the 2D, LG and FIX libraries and the rasterizer queue natively and
# checks that record + replay, on one thread or split into row bands across
# several, draws the same pixels as drawing directly, and that direct drawing
# still gives the pixels of the libraries as they were before row bands.
# Usage: tests/rastq/run.sh [frames per canvas size] [seed]
set -euo pipefail

cd "$(dirname "$0")/../.."

FRAMES="${1:-1000}"
SEED="${2:-}"
# The last commit whose 2D and LG libraries know nothing about row bands
REFERENCE=0ca600bf
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

includes() { # source root
    local l="$1/src/Libraries"
    echo "-I$l/2D/Source -I$l/2D/Source/GR -I$l/2D/Source/Clip -I$l/2D/Source/Flat8 -I$l/2D/Source/Gen" \
        "-I$l/2D/Source/RSD -I$l/3D/Source -I$l/H -I$l/FIX/Source -I$l/LG/Source -I$l/LG/Source/LOG/src" \
        "-I$l/DSTRUCT/Source -I$l/RES/Source"
}

# The .c files of a source list in the libraries' CMakeLists.txt
sources() { # source root, list name
    awk -v name="$2" '$0 == "set(" name {on = 1; next} on && /^\)/ {on = 0} on {print $1}' \
        "$1/src/Libraries/CMakeLists.txt" | grep '\.c$'
}

build_lib() { # library name, source root, extra compiler flags
    local name="$1" root="$2"
    shift 2
    mkdir "$OUT/$name"
    # fix_pow.c duplicates a function of fix.c
    { sources "$root" 2D_SRC; sources "$root" LG_SRC; sources "$root" FIX_SRC | grep -v fix_pow.c; } |
        xargs -P "$(nproc)" -I{} sh -c \
            'gcc -O2 -fcommon -w -pthread -DLG_SLOT_PTHREADS '"$*"' '"$(includes "$root")"' -c "'"$root"'/src/Libraries/{}" -o "'"$OUT/$name"'/$(echo {} | tr / _).o"'
    ar rcs "$OUT/$name.a" "$OUT/$name"/*.o
}

build() { # output name, library name, source root, extra compiler flags
    local name="$1" lib="$2" root="$3"
    shift 3
    local queue="$root/src/Libraries/3D/Source/rastq.c"
    [[ -f "$root/src/Libraries/3D/Source/rastqthr.c" ]] && queue="$queue $root/src/Libraries/3D/Source/rastqthr.c"
    # shellcheck disable=SC2046
    gcc -O2 -g -fcommon -pthread -DLG_SLOT_PTHREADS -DRASTQ_SELFCHECK "$@" $(includes "$root") \
        $queue tests/rastq/rastq_test.c "$OUT/$lib.a" -lm -o "$OUT/$name"
}

status=0

build_lib lib .
mkdir "$OUT/ref"
git archive "$REFERENCE" src/Libraries | tar -x -C "$OUT/ref"
# One deliberate change since then is given to the reference too: the row
# table of textures whose sides aren't powers of two no longer has leftover
# memory on either side of it (see vtab.c), which decided a few pixels.
cp src/Libraries/2D/Source/vtab.c src/Libraries/2D/Source/vtab.h "$OUT/ref/src/Libraries/2D/Source/"
sed -i 's/gr_free_temp(info.vtab)/gr_free_vtab(info.vtab)/; s/#include "tmapint.h"/#include "tmapint.h"\n#include "vtab.h"/' \
    "$OUT/ref/src/Libraries/2D/Source/Gen/gentm.c"
build_lib reflib "$OUT/ref"

echo "direct drawing against the libraries of $REFERENCE (with today's row tables):"
build plain lib .
build reference reflib "$OUT/ref" -DRASTQ_REFERENCE
RASTQ_HASH=1 "$OUT/plain" "$FRAMES" $SEED >"$OUT/hash.new"
RASTQ_HASH=1 "$OUT/reference" "$FRAMES" $SEED >"$OUT/hash.ref"
if cmp -s "$OUT/hash.new" "$OUT/hash.ref"; then
    echo "  $(wc -l <"$OUT/hash.new") frames identical"
else
    echo "  frames differ, the first ones:"
    diff "$OUT/hash.ref" "$OUT/hash.new" | head -6 | sed 's/^/    /'
    status=1
fi

echo "default arena:"
"$OUT/plain" "$FRAMES" $SEED || status=1

# A small arena makes scenes flush several times.
echo "small arena:"
build small lib . "-DRASTQ_ARENA_BYTES=(320*1024)"
"$OUT/small" "$FRAMES" $SEED || status=1

# The sanitizers are slow, so they get a tenth of the frames.
echo "address and undefined-behaviour sanitizers on the recorder and the test:"
build sanitized lib . -fsanitize=address,undefined -fno-sanitize-recover=undefined
ASAN_OPTIONS=detect_leaks=0 "$OUT/sanitized" "$((FRAMES / 10 + 1))" $SEED || status=1

echo "thread sanitizer on the libraries, the recorder and the test:"
build_lib tsanlib . -fsanitize=thread -g
build tsan tsanlib . -fsanitize=thread
# gOVResult is the one global the mappers write: every fixed-point division
# stores its overflow status there. Only the 3D library reads it, on the main
# thread, right after a division of its own.
echo "race:gOVResult" >"$OUT/tsan.supp"
# without address randomization, which this sanitizer can't always cope with
TSAN_OPTIONS="suppressions=$OUT/tsan.supp halt_on_error=0 exitcode=66" \
    setarch "$(uname -m)" -R "$OUT/tsan" "$((FRAMES / 10 + 1))" $SEED 2>"$OUT/tsan.log" || status=1
if grep -q "^SUMMARY" "$OUT/tsan.log"; then
    grep "^SUMMARY" "$OUT/tsan.log" | sort | uniq -c | sed 's/^/  /'
fi

if [[ $status -ne 0 ]]; then
    echo "FAILED"
else
    echo "OK"
fi
exit $status
