#!/bin/bash
# Enable the 40-pin header I2C2 bus (pins 3/5) at runtime.  Idempotent.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
OVERLAY=i2c2

if [ "$(id -u)" -ne 0 ]; then
    echo "enable-i2c2: must run as root" >&2
    exit 1
fi
if [ -e /dev/i2c-2 ]; then
    echo "enable-i2c2: /dev/i2c-2 already present"
    exit 0
fi
if ! grep -q ' /sys/kernel/config ' /proc/mounts; then
    mount -t configfs none /sys/kernel/config
fi
DIR="/sys/kernel/config/device-tree/overlays/$OVERLAY"
mkdir -p "$DIR"
if [ "$(cat "$DIR/status" 2>/dev/null || true)" != "applied" ]; then
    cat "$HERE/i2c2.dtbo" >"$DIR/dtbo"
fi
for _ in $(seq 1 50); do
    if [ -e /dev/i2c-2 ]; then
        echo "enable-i2c2: /dev/i2c-2 ready"
        exit 0
    fi
    sleep 0.1
done
echo "enable-i2c2: /dev/i2c-2 did not appear; check dmesg" >&2
exit 1
