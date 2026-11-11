#!/bin/bash
# Restore the on-board 5 V source on the USB 2.0 device port (host mode).
# Inverse of otg-vbus-off.sh.
set -euo pipefail

REG_DEV=vcc5v0-otg-regulator
REG_DIR=/sys/bus/platform/drivers/reg-fixed-voltage
GPIO=5
GPIO_DIR=/sys/class/gpio/gpio$GPIO

if [ "$(id -u)" -ne 0 ]; then
    echo "otg-vbus-on: must run as root" >&2
    exit 1
fi

if [ -e "$GPIO_DIR" ]; then
    echo 1 >"$GPIO_DIR/value"
    echo "$GPIO" >/sys/class/gpio/unexport
fi

if [ ! -e "$REG_DIR/$REG_DEV" ]; then
    echo "$REG_DEV" >"$REG_DIR/bind"
fi
echo "otg-vbus-on: $REG_DEV state=$(cat "/sys/bus/platform/devices/$REG_DEV/regulator/state" 2>/dev/null || echo unknown)"
