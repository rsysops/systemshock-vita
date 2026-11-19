#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

IMAGE_TAG="systemshock-vita-vitasdk"

BUILD_DIR="build"
VPK_NAME="systemshock"
PROFILE_FLAG="OFF"

# "profile": the game with its timers, for measuring. Its Profiler page picks
# what it profiles: the timings alone, or with the self-checks and the frame
# dumps (docs/PROFILING.md). A package and an app of its own (vita/vita.cmake).
if [[ "${1:-}" == "profile" ]]; then
    BUILD_DIR="build-profile"
    VPK_NAME="systemshock-profile"
    PROFILE_FLAG="ON"
    shift
    if [[ "${1:-}" == "checks" ]]; then
        echo "The checks have no build of their own any more: build \"profile\" and pick" >&2
        echo "Profile: Checks on its Profiler page (in-game menu)." >&2
        exit 1
    fi
fi

CLEAN=0
if [[ "${1:-}" == "clean" ]]; then
    CLEAN=1
    shift
fi

# anything else (a misspelling, "clean" first)
# would otherwise build the plain game without a word
if [[ $# -gt 0 ]]; then
    echo "usage: ./build.sh [profile] [clean]" >&2
    exit 1
fi

if [[ $CLEAN -eq 1 ]]; then
    rm -rf "$BUILD_DIR"
fi

docker build -t "$IMAGE_TAG" -f vita/Dockerfile .

docker run --rm \
    -v "$PWD:/workspace" \
    -w /workspace \
    --user "$(id -u):0" \
    -e BUILD_DIR="$BUILD_DIR" \
    -e PROFILE_FLAG="$PROFILE_FLAG" \
    "$IMAGE_TAG" \
    bash -c '
        set -euo pipefail
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        cmake .. \
            -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
            -DENABLE_VITA_PROFILE=$PROFILE_FLAG \
            -DCMAKE_BUILD_TYPE=None
        make -j"$(nproc)"
    '

echo "Build complete: $BUILD_DIR/$VPK_NAME.vpk"
