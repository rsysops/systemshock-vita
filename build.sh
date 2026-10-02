#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

IMAGE_TAG="systemshock-vita-vitasdk"

BUILD_DIR="build"
PROFILE_FLAG="OFF"

if [[ "${1:-}" == "profile" ]]; then
    BUILD_DIR="build-profile"
    PROFILE_FLAG="ON"
    shift
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
    "$IMAGE_TAG" \
    bash -c '
        set -euo pipefail
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        cmake .. \
            -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
            -DENABLE_OPENGL=OFF \
            -DVITA=true \
            -DENABLE_FLUIDSYNTH=OFF \
            -DENABLE_SDL2=ON \
            -DENABLE_VITA_PROFILE=$PROFILE_FLAG \
            -DCMAKE_BUILD_TYPE=None
        make -j"$(nproc)"
    '

echo "Build complete: $BUILD_DIR/systemshock.vpk"
