#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

IMAGE_TAG="systemshock-vita-vitasdk"

BUILD_DIR="build"
VPK_NAME="systemshock"
PROFILE_FLAG="OFF"
CHECKS_FLAG="OFF"

# "profile": the game with its timers, for measuring.
# "profile checks": the same with the self-checks and the frame dumps, which
# hitch and so are kept out of the build that measures.
# Each is a package and an app of its own (vita/vita.cmake).
if [[ "${1:-}" == "profile" ]]; then
    BUILD_DIR="build-profile"
    VPK_NAME="systemshock-profile"
    PROFILE_FLAG="ON"
    shift
    if [[ "${1:-}" == "checks" ]]; then
        BUILD_DIR="build-profile-checks"
        VPK_NAME="systemshock-profile-checks"
        CHECKS_FLAG="ON"
        shift
    fi
fi

if [[ "${1:-}" == "clean" ]]; then
    rm -rf "$BUILD_DIR"
fi

docker build -t "$IMAGE_TAG" -f vita/Dockerfile .

docker run --rm \
    -v "$PWD:/workspace" \
    -w /workspace \
    --user "$(id -u):0" \
    -e BUILD_DIR="$BUILD_DIR" \
    -e PROFILE_FLAG="$PROFILE_FLAG" \
    -e CHECKS_FLAG="$CHECKS_FLAG" \
    "$IMAGE_TAG" \
    bash -c '
        set -euo pipefail
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        cmake .. \
            -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
            -DENABLE_VITA_PROFILE=$PROFILE_FLAG \
            -DENABLE_VITA_PROFILE_CHECKS=$CHECKS_FLAG \
            -DCMAKE_BUILD_TYPE=None
        make -j"$(nproc)"
    '

echo "Build complete: $BUILD_DIR/$VPK_NAME.vpk"
