#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

IMAGE_TAG="systemshock-vita-vitasdk"

if [[ "${1:-}" == "clean" ]]; then
    rm -rf build
fi

docker build -t "$IMAGE_TAG" -f vita/Dockerfile .

docker run --rm \
    -v "$PWD:/workspace" \
    -w /workspace \
    --user "$(id -u):0" \
    "$IMAGE_TAG" \
    bash -c '
        set -euo pipefail
        mkdir -p build
        cd build
        cmake .. \
            -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
            -DENABLE_OPENGL=OFF \
            -DVITA=true \
            -DENABLE_FLUIDSYNTH=OFF \
            -DENABLE_SDL2=ON \
            -DCMAKE_BUILD_TYPE=None
        make -j"$(nproc)"
    '

echo "Build complete: build/systemshock.vpk"
