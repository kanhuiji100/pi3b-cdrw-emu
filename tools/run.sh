#!/bin/bash
# Convenience launcher: enable peripheral mode, then run the emulator.
#
#   sudo tools/run.sh                       # demo ISO
#   sudo tools/run.sh /path/to/disc.iso
#   sudo tools/run.sh /path/to/DISC01.CDR
#
# For an external PC, run tools/otg-vbus-off.sh first (and keep the board on
# its Type-C supply).
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE"

if [ "$(id -u)" -ne 0 ]; then
    echo "run: must run as root" >&2
    exit 1
fi

"$HERE/tools/enable-otg.sh"
"$HERE/tools/enable-i2c2.sh" || true
exec "$HERE/build/pi3b-cdrw-emu" \
    --root "${CDRW_ROOT:-$HERE}" \
    --udc fcc00000.usb \
    --image "${1:-$HERE/examples/demo.iso}"
