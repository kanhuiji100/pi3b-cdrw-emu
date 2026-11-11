#!/bin/bash
# Turn off the Orange Pi 3B's own 5 V source on the USB 2.0 device port.
#
# The vendor DTB marks the OTG VBUS switch (vcc5v0_otg, GPIO0_A5) as
# always-on.  When the device port is connected to an external PC with an
# A-to-A cable, both ends would then drive 5 V onto the same VBUS wire.
#
# This script unbinds the fixed-regulator driver and drives GPIO0_A5 low, so
# the port only *senses* the PC's VBUS.  The board must still be powered from
# its own Type-C supply.  Verified on the OPi 3B with the host port feeding
# VBUS through the cable; the emulated drive kept enumerating and reading.
set -euo pipefail

REG_DEV=vcc5v0-otg-regulator
REG_DIR=/sys/bus/platform/drivers/reg-fixed-voltage
GPIO=5
GPIO_DIR=/sys/class/gpio/gpio$GPIO

if [ "$(id -u)" -ne 0 ]; then
    echo "otg-vbus-off: must run as root" >&2
    exit 1
fi

if [ -e "$REG_DIR/$REG_DEV" ]; then
    echo "$REG_DEV" >"$REG_DIR/unbind"
    echo "otg-vbus-off: unbound $REG_DEV"
fi

if [ ! -e "$GPIO_DIR" ]; then
    echo "$GPIO" >/sys/class/gpio/export
fi
echo out >"$GPIO_DIR/direction"
echo 0 >"$GPIO_DIR/value"

echo "otg-vbus-off: GPIO0_A5=$(cat "$GPIO_DIR/value"); the host must supply VBUS"
echo "otg-vbus-off: board power must come from the Type-C connector"
