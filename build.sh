#!/usr/bin/env bash
# Build passport-os (Linux, macOS or WSL).
#
# Usage:
#   bash build.sh          build the local official Jollybot edition
#   bash build.sh clean    clear the CMake cache, then do a full build (only needed after changing IDF version or path)
#   bash build.sh release  build the official Jollybot edition with the public release config and verify it
#   bash build.sh diag     memory diagnostics build (heap task tracking + allocation peaks), output to build-diag, not for release
#
# Note: ESP-IDF is expected at /opt/esp-idf (override with IDF_DIR). On Windows, build inside WSL.
# WSL cannot see Windows serial ports (e.g. COM3) by default: bind with usbipd, or flash from Windows with esptool.
set -u

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
IDF_DIR="${IDF_DIR:-/opt/esp-idf}"

cd "$PROJECT_DIR" || { echo "Project directory not found: $PROJECT_DIR"; exit 1; }

if [ ! -f "$IDF_DIR/export.sh" ]; then
    echo "ESP-IDF not found: $IDF_DIR/export.sh"
    echo "If it is installed elsewhere, run: IDF_DIR=/path/to/esp-idf bash build.sh"
    exit 1
fi

echo "==> Loading ESP-IDF environment ($IDF_DIR)"
set +e
. "$IDF_DIR/export.sh" >/dev/null 2>&1
rc=$?
set -e
if [ $rc -ne 0 ]; then
    echo "export.sh failed, check the IDF installation"
    exit 1
fi

if [ "${1:-}" = "release" ]; then
    # Seed from tracked defaults every time. Local sdkconfig may silently retain
    # obsolete TLS/flash options even after sdkconfig.defaults has been updated.
    mkdir -p build-public
    cp -- sdkconfig.defaults build-public/sdkconfig
    idf.py -B build-public -DSDKCONFIG="$PROJECT_DIR/build-public/sdkconfig" \
        -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=ON \
        -DPASSPORT_MUSE_USE_JOLLYBOT=ON build \
        merge-bin -o FoloToy-AI-Passport-full.bin
    python tools/verify_firmware.py build-public --public
    exit 0
fi

if [ "${1:-}" = "diag" ]; then
    mkdir -p build-diag
    cat -- sdkconfig.defaults sdkconfig.diag > build-diag/sdkconfig
    idf.py -B build-diag -DSDKCONFIG="$PROJECT_DIR/build-diag/sdkconfig" \
        -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=OFF \
        -DPASSPORT_MUSE_USE_JOLLYBOT=ON build
    exit 0
fi

if [ "${1:-}" = "clean" ]; then
    echo "==> Cleaning build directory"
    rm -rf build
fi

echo "==> Building (local official Jollybot edition)"
# Never reuse the ignored root sdkconfig: it may preserve obsolete options even
# after defaults change. Keep the expanded generated config inside the build
# directory and seed it from the tracked memory budget on every invocation.
mkdir -p build
cp -- sdkconfig.defaults build/sdkconfig
idf.py -B build -DSDKCONFIG="$PROJECT_DIR/build/sdkconfig" \
    -DIDF_TARGET=esp32c3 -DPASSPORT_PUBLIC_RELEASE=OFF \
    -DPASSPORT_MUSE_USE_JOLLYBOT=ON build
