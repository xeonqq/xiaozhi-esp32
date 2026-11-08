#!/usr/bin/env bash
# Build a board variant and flash it over serial.
# Usage: scripts/build_and_flash.sh [board] [variant] [port]
# Defaults: bread-compact-esp32 bread-compact-esp32-sh1106 /dev/ttyUSB0
set -euo pipefail

BOARD="${1:-bread-compact-esp32}"
VARIANT="${2:-bread-compact-esp32-sh1106}"
PORT="${3:-/dev/ttyUSB0}"

export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/opt/esp32}"
IDF_EXPORT="${IDF_PATH:-/opt/esp-idf-6.1}/export.sh"
# shellcheck disable=SC1090
source "$IDF_EXPORT"

cd "$(dirname "$0")/.."
python3 scripts/build.py "$BOARD" --name "$VARIANT"

cd build
TARGET=$(python3 -c "import json; print(json.load(open('project_description.json'))['target'])")
# esptool directly: idf.py flash rejects the venv's python/python3 symlink mismatch
python3 -m esptool --chip "$TARGET" -p "$PORT" -b 460800 write_flash @flash_args