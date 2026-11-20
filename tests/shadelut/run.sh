#!/usr/bin/env bash
# Builds and runs the smooth shadows' colour table test (src/MacSrc/ShadeLut.c)
# with gcc and clang, at -O2 and -Ofast (the Vita build uses -Ofast).
# Usage: tests/shadelut/run.sh [the game's DATA folder]
# Without a DATA folder, only the made-up tables are checked.
set -euo pipefail

cd "$(dirname "$0")/../.."

DATA="${1:-}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

status=0
for cc in gcc clang; do
    for opt in -O2 -Ofast; do
        echo "$cc $opt:"
        "$cc" "$opt" -Wall -Wextra -Isrc/MacSrc src/MacSrc/ShadeLut.c tests/shadelut/shadelut_test.c -lm \
            -o "$OUT/shadelut_test"
        "$OUT/shadelut_test" ${DATA:+"$DATA"} || status=1
    done
done

if [[ $status -ne 0 ]]; then
    echo "FAILED"
else
    echo "OK"
fi
exit $status
