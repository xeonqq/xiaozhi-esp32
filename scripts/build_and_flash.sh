#!/usr/bin/env bash
# Build a XiaoZhi board variant and flash it over serial.
#
# Usage:
#   scripts/build_and_flash.sh [options] [board] [variant] [port]
#
# Positional arguments are kept for compatibility:
#   board     Board directory under main/boards (default: bread-compact-esp32)
#   variant   Build variant name from config.json (default: bread-compact-esp32-sh1106)
#   port      Serial port (default: /dev/ttyUSB0)
#
# Options:
#   -h, --help            Show this help and exit
#   -l, --list-boards     List available boards and variants, then exit
#   -L, --list-ports      List detected serial ports, then exit
#   -b, --board BOARD     Board directory (alternative to the positional board)
#   -n, --name VARIANT    Build variant name (alternative to the positional variant)
#   -p, --port PORT       Serial port (alternative to the positional port)
#   -B, --baud RATE       Flash baud rate (default: 460800)
#
# Environment:
#   IDF_PATH              ESP-IDF installation (default: /opt/esp-idf-6.1)
#   IDF_TOOLS_PATH        ESP-IDF tools directory (default: /opt/esp32)
#
# If a board other than the default is given without a variant, build.py shows
# that board's variants and asks you to choose one (interactive terminals) or
# fails with a hint to pass --name.
#
# Examples:
#   scripts/build_and_flash.sh --list-boards
#   scripts/build_and_flash.sh --list-ports
#   scripts/build_and_flash.sh
#   scripts/build_and_flash.sh phoniebox-esp32 esp32-phoniebox-sh1106 /dev/ttyUSB0
#   scripts/build_and_flash.sh -b phoniebox-esp32 -n esp32-phoniebox-8mb -p /dev/ttyACM0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

DEFAULT_BOARD="bread-compact-esp32"
DEFAULT_VARIANT="bread-compact-esp32-sh1106"
DEFAULT_PORT="/dev/ttyUSB0"
DEFAULT_BAUD="460800"

BOARD="$DEFAULT_BOARD"
VARIANT="$DEFAULT_VARIANT"
PORT="$DEFAULT_PORT"
BAUD="$DEFAULT_BAUD"
BOARD_GIVEN=0
VARIANT_GIVEN=0
LIST_BOARDS=0
LIST_PORTS=0

usage() {
    # Print the comment block at the top of this file as the help text.
    local script="$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")"
    sed -n '2,/^set -euo pipefail$/p' "$script" | sed '$d' | sed 's/^# \{0,1\}//'
}

list_ports() {
    local ports=()
    shopt -s nullglob
    ports=(/dev/ttyUSB* /dev/ttyACM* /dev/serial/by-id/*)
    shopt -u nullglob
    if ((${#ports[@]} == 0)); then
        echo "No serial ports found (/dev/ttyUSB*, /dev/ttyACM*, /dev/serial/by-id/*)." >&2
        return 1
    fi
    printf '%s\n' "${ports[@]}"
}

positional=()
while (($# > 0)); do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        -l|--list|--list-boards)
            LIST_BOARDS=1
            shift
            ;;
        -L|--list-ports)
            LIST_PORTS=1
            shift
            ;;
        -b|--board)
            if (($# < 2)); then
                echo "error: $1 requires a value" >&2
                exit 2
            fi
            BOARD="$2"
            BOARD_GIVEN=1
            shift 2
            ;;
        --board=*)
            BOARD="${1#*=}"
            BOARD_GIVEN=1
            shift
            ;;
        -n|--name|--variant)
            if (($# < 2)); then
                echo "error: $1 requires a value" >&2
                exit 2
            fi
            VARIANT="$2"
            VARIANT_GIVEN=1
            shift 2
            ;;
        --name=*|--variant=*)
            VARIANT="${1#*=}"
            VARIANT_GIVEN=1
            shift
            ;;
        -p|--port)
            if (($# < 2)); then
                echo "error: $1 requires a value" >&2
                exit 2
            fi
            PORT="$2"
            shift 2
            ;;
        --port=*)
            PORT="${1#*=}"
            shift
            ;;
        -B|--baud)
            if (($# < 2)); then
                echo "error: $1 requires a value" >&2
                exit 2
            fi
            BAUD="$2"
            shift 2
            ;;
        --baud=*)
            BAUD="${1#*=}"
            shift
            ;;
        --)
            shift
            positional+=("$@")
            break
            ;;
        -*)
            echo "error: unknown option: $1" >&2
            echo "Try '$0 --help' for usage." >&2
            exit 2
            ;;
        *)
            positional+=("$1")
            shift
            ;;
    esac
done

if ((${#positional[@]} > 3)); then
    echo "error: too many positional arguments: ${positional[*]}" >&2
    echo "Try '$0 --help' for usage." >&2
    exit 2
fi
if ((${#positional[@]} >= 1)); then
    BOARD="${positional[0]}"
    BOARD_GIVEN=1
fi
if ((${#positional[@]} >= 2)); then
    VARIANT="${positional[1]}"
    VARIANT_GIVEN=1
fi
if ((${#positional[@]} >= 3)); then
    PORT="${positional[2]}"
fi

# A non-default board without an explicit variant should let build.py show that
# board's variants instead of applying the bread-compact default.
if ((BOARD_GIVEN == 1 && VARIANT_GIVEN == 0)); then
    VARIANT=""
fi

IDF_EXPORT="${IDF_PATH:-/opt/esp-idf-6.1}/export.sh"

if ((LIST_PORTS || LIST_BOARDS)); then
    status=0
    if ((LIST_PORTS)); then
        list_ports || status=1
    fi
    if ((LIST_BOARDS)); then
        if ((LIST_PORTS)); then
            echo
        fi
        # Listing works without an activated toolchain, but prefer the real
        # ESP-IDF version when it is available so version-gated variants match.
        if [[ -f "$IDF_EXPORT" ]]; then
            # shellcheck disable=SC1090
            source "$IDF_EXPORT" >/dev/null 2>&1 || true
        fi
        cd "$REPO_ROOT"
        python3 scripts/build.py --list-boards
    fi
    exit "$status"
fi

export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/opt/esp32}"
if [[ ! -f "$IDF_EXPORT" ]]; then
    echo "error: ESP-IDF export script not found: $IDF_EXPORT" >&2
    echo "Set IDF_PATH to your ESP-IDF installation (v6.0.1 or newer)." >&2
    exit 1
fi
# shellcheck disable=SC1090
source "$IDF_EXPORT"

if [[ ! -e "$PORT" ]]; then
    echo "error: serial port '$PORT' not found" >&2
    echo "Detected ports:" >&2
    list_ports >&2 || true
    echo "Use --port PORT (or the third positional argument) to choose one." >&2
    exit 1
fi

cd "$REPO_ROOT"

echo "==> Building board '$BOARD' (variant: ${VARIANT:-<choose>})"
if [[ -n "$VARIANT" ]]; then
    python3 scripts/build.py "$BOARD" --name "$VARIANT"
else
    python3 scripts/build.py "$BOARD"
fi

cd build
TARGET=$(python3 -c "import json; print(json.load(open('project_description.json'))['target'])")
echo "==> Flashing $TARGET to $PORT at $BAUD baud"
# esptool directly: idf.py flash rejects the venv's python/python3 symlink mismatch
python3 -m esptool --chip "$TARGET" -p "$PORT" -b "$BAUD" write_flash @flash_args

if [[ -n "$VARIANT" ]]; then
    echo "==> Done: $BOARD ($VARIANT) flashed to $PORT"
else
    echo "==> Done: $BOARD flashed to $PORT"
fi
