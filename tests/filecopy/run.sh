#!/usr/bin/env bash
# Builds the game's file copy natively (src/GameSrc/filecopy.c) and checks
# that a copy is byte for byte its source, for files of many sizes and for
# the game's own archive, over whatever the destination held before.
# Usage: tests/filecopy/run.sh [the game's DATA folder]
set -euo pipefail

DATA="${1:-}"
cd "$(dirname "$0")/../.."

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

gcc -O2 -g -Wall -Wextra -Isrc/GameSrc/Headers tests/filecopy/filecopy_test.c src/GameSrc/filecopy.c \
    src/Libraries/RES/Source/caseless.c -o "$OUT/filecopy_test"

archive=""
if [[ -n "$DATA" ]]; then
    archive="$(find "$DATA" -maxdepth 1 -iname archive.dat | head -1)"
fi

mkdir "$OUT/work"
# shellcheck disable=SC2086
if "$OUT/filecopy_test" "$OUT/work" $archive; then
    echo "OK"
else
    echo "FAILED"
    exit 1
fi
