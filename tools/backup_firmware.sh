#!/usr/bin/env bash
#
# backup_firmware.sh — dump the full flash image of a LilyGo board before you
# flash anything new onto it. Do this once on a brand-new watch: it is the only
# copy of that unit's factory NVS (Wi-Fi/BLE calibration, pairing, serial data)
# that LilyGo's generic factory .bin in ../firmware/ cannot restore for you.
#
# Usage:
#   tools/backup_firmware.sh [--port PORT] [--chip CHIP] [--size BYTES]
#                            [--baud BAUD] [--out FILE] [--efuse]
#
#   --port   serial port (e.g. /dev/cu.usbmodem1101, /dev/ttyACM0, COM5).
#            Auto-detected when exactly one ESP device is attached.
#   --chip   esp32s3 (default; the T-Watch Ultra / S3). esp32 for T-Watch 2020.
#   --size   bytes to read (default 0x1000000 = 16 MB, the Ultra's full flash).
#   --baud   read baud rate (default 921600).
#   --out    output file (default firmware-backup-<chip>-<date>.bin here).
#   --efuse  also write an espefuse summary alongside (record only; not restorable).
#
# Put the watch in DOWNLOAD MODE first if the port keeps dropping:
#   hold BOOT, tap RST, release BOOT  (docs/lilygo-t-watch-ultra.md#t-watch-ultra-enter-download-mode)
# After a successful backup, tap RST to leave download mode.
#
# Restore with: tools/restore_firmware.sh --in <this file>
set -euo pipefail

PORT=""
CHIP="esp32s3"
SIZE="0x1000000"   # 16 MB
BAUD="921600"
OUT=""
EFUSE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --port)  PORT="$2";  shift 2 ;;
        --chip)  CHIP="$2";  shift 2 ;;
        --size)  SIZE="$2";  shift 2 ;;
        --baud)  BAUD="$2";  shift 2 ;;
        --out)   OUT="$2";   shift 2 ;;
        --efuse) EFUSE=1;    shift ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# esptool may be a script on PATH or a Python module.
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

ESPEFUSE=()
if [ "$EFUSE" = 1 ]; then
    if command -v espefuse.py >/dev/null 2>&1; then ESPEFUSE=(espefuse.py)
    elif command -v espefuse >/dev/null 2>&1; then ESPEFUSE=(espefuse)
    elif python3 -c 'import espefuse' >/dev/null 2>&1; then ESPEFUSE=(python3 -m espefuse)
    else echo "--efuse requested but espefuse not found; skipping efuse dump." >&2; EFUSE=0; fi
fi

PORT_ARGS=()
[ -n "$PORT" ] && PORT_ARGS=(--port "$PORT")

if [ -z "$OUT" ]; then
    OUT="firmware-backup-${CHIP}-$(date +%Y%m%d-%H%M%S).bin"
fi

if [ -e "$OUT" ]; then
    echo "refusing to overwrite existing $OUT — pass a different --out" >&2
    exit 1
fi

echo "Reading ${SIZE} bytes of flash from the ${CHIP} -> ${OUT}"
echo "(if the port keeps dropping, enter download mode: hold BOOT, tap RST, release BOOT)"
"${ESPTOOL[@]}" --chip "$CHIP" "${PORT_ARGS[@]}" --baud "$BAUD" \
    read_flash 0 "$SIZE" "$OUT"

# Record a sha256 next to the image so a later restore can be verified.
if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$OUT" | tee "${OUT}.sha256"
elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$OUT" | tee "${OUT}.sha256"
fi

if [ "$EFUSE" = 1 ]; then
    EFUSE_OUT="${OUT%.bin}-efuse.txt"
    echo "Writing efuse summary -> ${EFUSE_OUT}"
    "${ESPEFUSE[@]}" --chip "$CHIP" "${PORT_ARGS[@]}" summary | tee "$EFUSE_OUT" || \
        echo "efuse summary failed (non-fatal)" >&2
fi

echo
echo "Backup complete: ${OUT}"
echo "Keep it somewhere safe. Restore with:  tools/restore_firmware.sh --in \"${OUT}\""
