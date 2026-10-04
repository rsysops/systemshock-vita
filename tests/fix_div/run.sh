#!/usr/bin/env bash
# Builds and runs the fix_div bit-exactness test with gcc and clang, at -O2
# and -Ofast (the Vita build uses -Ofast). Usage: tests/fix_div/run.sh [cases]
set -euo pipefail

cd "$(dirname "$0")/../.."

CASES="${1:-100000000}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

FIX=src/Libraries/FIX/Source
INCLUDES=(-I"$FIX" -Isrc/Libraries/H -Isrc/Libraries/LG/Source -Isrc/Libraries/LG/Source/LOG/src)
SOURCES=("$FIX/fix.c" "$FIX/fix_sqrt.c" "$FIX/MakeTables.c" src/Libraries/LG/Source/LOG/src/log.c
         tests/fix_div/fix_div_test.c)

status=0
for cc in gcc clang; do
    for opt in -O2 -Ofast; do
        echo "$cc $opt:"
        "$cc" "$opt" -fcommon -DFIX_DIV_FPU "${INCLUDES[@]}" -c tests/fix_div/fix_div_ref.c -o "$OUT/ref.o"
        "$cc" "$opt" -fcommon -DFIX_DIV_FPU "${INCLUDES[@]}" "${SOURCES[@]}" "$OUT/ref.o" -lm -o "$OUT/fix_div_test"
        "$OUT/fix_div_test" "$CASES" || status=1
    done
done

if [[ $status -ne 0 ]]; then
    echo "FAILED"
else
    echo "OK"
fi
exit $status
