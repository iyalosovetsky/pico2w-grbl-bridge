#!/usr/bin/env bash
# Configure, build, and (optionally) flash pico2w-grbl-bridge.
#
# Examples:
#   tools/build.sh                               # build for pico2_w (RP2350), default pins
#   tools/build.sh --board pico_w                 # build for a plain Pico W (RP2040)
#   tools/build.sh --dp-pin 16 --pio 0 --flash    # custom PIO-USB pins, then flash
#   tools/build.sh --clean --flash                # wipe the build dir first, then build+flash
#
# Flashing uses `picotool`, so the board must already be in BOOTSEL mode (hold BOOTSEL
# while plugging in its native USB port) — this firmware doesn't expose picotool's
# USB reset interface, so it can't be rebooted into BOOTSEL remotely.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BOARD="pico2_w"
DP_PIN=0
PIO_INDEX=1
SDK_PATH="${PICO_SDK_PATH:-}"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
CLEAN=0
FLASH=0

usage() {
    cat <<EOF
Configure, build, and (optionally) flash pico2w-grbl-bridge.

Examples:
  tools/build.sh                               # build for pico2_w (RP2350), default pins
  tools/build.sh --board pico_w                 # build for a plain Pico W (RP2040)
  tools/build.sh --dp-pin 16 --pio 0 --flash    # custom PIO-USB pins, then flash
  tools/build.sh --clean --flash                # wipe the build dir first, then build+flash

Flashing uses picotool, so the board must already be in BOOTSEL mode (hold BOOTSEL
while plugging in its native USB port) — this firmware doesn't expose picotool's
USB reset interface, so it can't be rebooted into BOOTSEL remotely.

Options:
  --board <pico_w|pico2_w>   Target board (default: pico2_w)
  --sdk <path>                pico-sdk path (default: \$PICO_SDK_PATH)
  --dp-pin <gpio>              PIO-USB host D+ pin; D- is this + 1 (default: 0)
  --pio <0|1|2>                 PIO block claimed by the PIO-USB host (default: 1)
  -j, --jobs <n>                 Parallel build jobs (default: nproc)
  -c, --clean                    Remove the build directory first
  -f, --flash                    Flash via picotool after a successful build
  -h, --help                      Show this help
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --board) BOARD="$2"; shift 2 ;;
        --sdk) SDK_PATH="$2"; shift 2 ;;
        --dp-pin) DP_PIN="$2"; shift 2 ;;
        --pio) PIO_INDEX="$2"; shift 2 ;;
        -j|--jobs) JOBS="$2"; shift 2 ;;
        -c|--clean) CLEAN=1; shift ;;
        -f|--flash) FLASH=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

if [ "$BOARD" != "pico_w" ] && [ "$BOARD" != "pico2_w" ]; then
    echo "error: --board must be 'pico_w' (RP2040) or 'pico2_w' (RP2350), got '$BOARD'" >&2
    exit 1
fi

if [ -z "$SDK_PATH" ]; then
    echo "error: pico-sdk path not set. Pass --sdk <path> or export PICO_SDK_PATH." >&2
    exit 1
fi
if [ ! -f "$SDK_PATH/pico_sdk_init.cmake" ]; then
    echo "error: '$SDK_PATH' doesn't look like a pico-sdk checkout (no pico_sdk_init.cmake)." >&2
    exit 1
fi

BUILD_DIR="$REPO_ROOT/build-$BOARD"

if [ "$CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
    echo "Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"

echo "Configuring: board=$BOARD dp-pin=$DP_PIN pio=$PIO_INDEX"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DPICO_SDK_PATH="$SDK_PATH" \
    -DPICO_BOARD="$BOARD" \
    -DPIO_USB_HOST_DP_PIN="$DP_PIN" \
    -DPIO_USB_HOST_PIO_INDEX="$PIO_INDEX"

echo "Building ($JOBS jobs)"
cmake --build "$BUILD_DIR" -j "$JOBS"

UF2="$BUILD_DIR/bridge.uf2"
echo "Built: $UF2"

if [ "$FLASH" -eq 1 ]; then
    if ! command -v picotool >/dev/null 2>&1; then
        echo "error: picotool not found on PATH — can't flash. Copy $UF2 to the RPI-RP2 drive by hand instead." >&2
        exit 1
    fi
    echo "Flashing via picotool (make sure the board is in BOOTSEL mode: hold BOOTSEL while plugging in its native USB port)"
    picotool load -u -v -x "$UF2"
fi
