#!/bin/bash
# Switch the Orange Pi 3B USB 2.0 device port to peripheral mode at runtime.
#
# The vendor DTB describes the RK3566 USB2 OTG controller as a host.  A live
# device-tree overlay flips dr_mode, then the dwc3 driver is rebound so the
# controller re-probes and registers a UDC (/sys/class/udc/fcc00000.usb).
# No reboot is required.  Idempotent.
set -euo pipefail

UDC=fcc00000.usb
OVERLAY=otg-peripheral
HERE=$(cd "$(dirname "$0")" && pwd)
DTBO="$HERE/otg-peripheral.dtbo"

if [ "$(id -u)" -ne 0 ]; then
    echo "enable-otg: must run as root" >&2
    exit 1
fi

if [ -e "/sys/class/udc/$UDC" ]; then
    echo "enable-otg: $UDC already available"
    exit 0
fi

if ! grep -q ' /sys/kernel/config ' /proc/mounts; then
    mount -t configfs none /sys/kernel/config
fi

OVERLAY_DIR="/sys/kernel/config/device-tree/overlays/$OVERLAY"
mkdir -p "$OVERLAY_DIR"
if [ "$(cat "$OVERLAY_DIR/status" 2>/dev/null || true)" != "applied" ]; then
    cat "$DTBO" >"$OVERLAY_DIR/dtbo"
fi

if [ -e "/sys/bus/platform/devices/$UDC" ]; then
    echo "$UDC" >/sys/bus/platform/drivers/dwc3/unbind 2>/dev/null || true
    sleep 0.3
    echo "$UDC" >/sys/bus/platform/drivers/dwc3/bind
fi

for _ in $(seq 1 50); do
    if [ -e "/sys/class/udc/$UDC" ]; then
        echo "enable-otg: $UDC ready ($(cat "/sys/class/udc/$UDC/state" 2>/dev/null || echo unknown))"
        exit 0
    fi
    sleep 0.1
done

echo "enable-otg: $UDC did not appear; check dmesg" >&2
exit 1
