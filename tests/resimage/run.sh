#!/usr/bin/env bash
# Builds the resource library natively and checks, for each art file the game
# holds in memory (src/GameSrc/resimage.c), that every resource read from the
# file's copy in memory is byte for byte what is read from the file.
# Usage: tests/resimage/run.sh <the game's DATA folder>
set -euo pipefail

DATA="$(cd "${1:?usage: tests/resimage/run.sh <the game\'s DATA folder>}" && pwd)"
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
{ sources 2D_SRC; sources LG_SRC; sources FIX_SRC; sources DSTRUCT_SRC; sources RES_SRC; } |
    grep '\.c$' |
    xargs -P "$(nproc)" -I{} sh -c \
        'gcc -O2 -fcommon -w -pthread -DLG_SLOT_PTHREADS '"$INCLUDES"' -c "'"$L"'/{}" -o "'"$OUT"'/obj/$(echo {} | tr / _).o"'
ar rcs "$OUT/libs.a" "$OUT"/obj/*.o
# shellcheck disable=SC2086
gcc -O2 -g -fcommon -pthread $INCLUDES tests/resimage/resimage_test.c "$OUT/libs.a" -lm -o "$OUT/resimage_test"

# the names of resimage.c's list
files=()
for name in $(grep -o 'res/data/[a-z0-9]*\.res' src/GameSrc/resimage.c | sed 's|res/data/||'); do
    found="$(find "$DATA" -maxdepth 1 -iname "$name" | head -1)"
    if [[ -z "$found" ]]; then
        echo "no $name in $DATA"
        exit 1
    fi
    files+=("$(basename "$found")")
done

# from the folder, by short names as the game's are: a copy goes by its name
if (cd "$DATA" && "$OUT/resimage_test" "${files[@]}"); then
    echo "OK"
else
    echo "FAILED"
    exit 1
fi
