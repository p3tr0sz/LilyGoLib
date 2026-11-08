#!/usr/bin/env bash
#
# restore_firmware.sh — write a full-flash image back onto a board, e.g. the
# file tools/backup_firmware.sh produced, to return the unit to exactly how it
# shipped. Writes the whole image at offset 0x0.
#
# Usage:
#   tools/restore_firmware.sh --in FILE [--port PORT] [--chip CHIP]
#                             [--baud BAUD] [--yes]
#
#   --in     the backup image to write (required).
#   --port   serial port. Auto-detected when exactly one ESP device is attached.
#   --chip   esp32s3 (default). esp32 for T-Watch 2020.
#   --baud   write baud rate (default 921600).
#   --yes    skip the confirmation prompt.
#
# This overwrites the entire flash — the current firmware and all of its NVS are
# replaced by the image. Enter download mode first if the port is unstable
# (hold BOOT, tap RST, release BOOT); tap RST again when done.
set -euo pipefail

IN=""
PORT=""
CHIP="esp32s3"
BAUD="921600"
ASSUME_YES=0

while [ $# -gt 0 ]; do
    case "$1" in
        --in)   IN="$2";   shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --chip) CHIP="$2"; shift 2 ;;
        --baud) BAUD="$2"; shift 2 ;;
        --yes)  ASSUME_YES=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$IN" ]; then
    echo "give --in <backup.bin>" >&2
    exit 2
fi
if [ ! -f "$IN" ]; then
    echo "no such file: $IN" >&2
    exit 1
fi

# Verify against a sibling .sha256 if the backup wrote one.
if [ -f "${IN}.sha256" ]; then
    echo "Verifying ${IN} against ${IN}.sha256 ..."
    if command -v shasum >/dev/null 2>&1; then
        ( cd "$(dirname "$IN")" && shasum -a 256 -c "$(basename "$IN").sha256" )
    elif command -v sha256sum >/dev/null 2>&1; then
        ( cd "$(dirname "$IN")" && sha256sum -c "$(basename "$IN").sha256" )
    fi
fi

if command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL=(esptool.py)
elif command -v esptool >/dev/null 2>&1; then
    ESPTOOL=(esptool)
elif python3 -c 'import esptool' >/dev/null 2>&1; then
    ESPTOOL=(python3 -m esptool)
else
    echo "esptool not found. Install it with:  python3 -m pip install --upgrade esptool" >&2
    exit 1
fi

PORT_ARGS=()
[ -n "$PORT" ] && PORT_ARGS=(--port "$PORT")

echo "About to WRITE the entire flash of the ${CHIP} from:"
echo "    ${IN}"
echo "This replaces the current firmware and all stored settings."
if [ "$ASSUME_YES" != 1 ]; then
    printf "Type 'restore' to continue: "
    read -r reply
    [ "$reply" = "restore" ] || { echo "aborted."; exit 1; }
fi

"${ESPTOOL[@]}" --chip "$CHIP" "${PORT_ARGS[@]}" --baud "$BAUD" \
    write_flash --flash_size detect 0x0 "$IN"

echo
echo "Restore complete. Tap RST to boot (and to leave download mode)."
