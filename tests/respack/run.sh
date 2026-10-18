#!/usr/bin/env bash
# Builds the resource library natively twice, as it was at a reference
# commit and as it is in the tree, and gives each the same life of a
# resource file: created, then opened for writing round after round with
# some resources rewritten, which leaves dead ones for the next opening to
# pack away (ResPack). After every round the two files must be the same
# bytes, and every resource must read back what was last written to it.
# Usage: tests/respack/run.sh [seed]
set -euo pipefail

cd "$(dirname "$0")/../.."

SEED="${1:-}"
# The last commit whose ResPack moved resources one at a time in the file
REFERENCE=63678953
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

includes() { # source root
    local l="$1/src/Libraries"
    echo "-I$l/2D/Source -I$l/2D/Source/GR -I$l/2D/Source/Clip -I$l/2D/Source/Flat8 -I$l/2D/Source/Gen" \
        "-I$l/2D/Source/RSD -I$l/3D/Source -I$l/H -I$l/FIX/Source -I$l/LG/Source -I$l/LG/Source/LOG/src" \
        "-I$l/DSTRUCT/Source -I$l/RES/Source"
}

# The source files of a list in the libraries' CMakeLists.txt
sources() { # source root, list name
    awk -v name="$2" '$0 == "set(" name {on = 1; next} on && /^\)/ {on = 0} on {sub(/\)$/, "", $1); print $1}
                      on && /\)$/ {on = 0}' "$1/src/Libraries/CMakeLists.txt"
}

build() { # name, source root
    local name="$1" root="$2"
    mkdir "$OUT/$name"
    { sources "$root" 2D_SRC; sources "$root" LG_SRC; sources "$root" FIX_SRC
      sources "$root" DSTRUCT_SRC; sources "$root" RES_SRC; } | grep '\.c$' |
        xargs -P "$(nproc)" -I{} sh -c \
            'gcc -O2 -fcommon -w -pthread -DLG_SLOT_PTHREADS '"$(includes "$root")"' -c "'"$root"'/src/Libraries/{}" -o "'"$OUT/$name"'/$(echo {} | tr / _).o"'
    ar rcs "$OUT/$name.a" "$OUT/$name"/*.o
    # shellcheck disable=SC2046
    gcc -O2 -g -fcommon -w -pthread $(includes "$root") tests/respack/respack_test.c "$OUT/$name.a" -lm \
        -o "$OUT/$name-test"
    mkdir "$OUT/$name-files"
}

mkdir "$OUT/ref"
git archive "$REFERENCE" src/Libraries | tar -x -C "$OUT/ref"
build was "$OUT/ref"
build now .

status=0
echo "as it was:"
# shellcheck disable=SC2086
"$OUT/was-test" "$OUT/was-files" $SEED || status=1
echo "as it is:"
# shellcheck disable=SC2086
"$OUT/now-test" "$OUT/now-files" $SEED || status=1

differ=0
rounds=0
for f in "$OUT"/was-files/round-*.res; do
    rounds=$((rounds + 1))
    if ! cmp -s "$f" "$OUT/now-files/$(basename "$f")"; then
        echo "  $(basename "$f") differs between the two"
        differ=$((differ + 1))
    fi
done
echo "the file after each of $rounds rounds: $differ differ ($(du -k "$OUT/now-files/round-00.res" | cut -f1) KB at the start)"
if [[ $differ -ne 0 || $rounds -eq 0 || $status -ne 0 ]]; then
    echo "FAILED"
    exit 1
fi
echo "OK"
