#!/bin/bash
# Install/update the boot service.  Run from the source tree:
#   sudo deploy/install.sh
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=/opt/pi3b-cdrw
STATE=/var/lib/pi3b-cdrw

if [ "$(id -u)" -ne 0 ]; then
    echo "install: must run as root (sudo deploy/install.sh)" >&2
    exit 1
fi
if [ "$HERE" = "$PREFIX" ]; then
    echo "install: run from the source tree, not from /opt/pi3b-cdrw" >&2
    exit 1
fi
if [ ! -x "$HERE/build/pi3b-cdrw-emu" ]; then
    echo "install: $HERE/build/pi3b-cdrw-emu missing; run make first" >&2
    exit 1
fi

# Stop both the service and a manually started daemon.
systemctl stop pi3b-cdrw.service 2>/dev/null || true
pkill -x pi3b-cdrw-emu 2>/dev/null || true
sleep 1

install -d -m 0755 "$PREFIX" "$PREFIX/build" "$STATE/discs"
install -m 0755 "$HERE/build/pi3b-cdrw-emu" "$PREFIX/build/pi3b-cdrw-emu"

for d in tools docs examples src tests deploy; do
    rm -rf "$PREFIX/$d"
    cp -a "$HERE/$d" "$PREFIX/$d"
done
install -m 0644 "$HERE/README.md" "$HERE/LICENSE" "$HERE/Makefile" "$PREFIX/"

# Preserve discs already in the state directory on reinstall; copy new ones.
cp -a --sparse=always -n "$HERE/discs/." "$STATE/discs/"

install -m 0644 "$HERE/deploy/pi3b-cdrw.service" /etc/systemd/system/pi3b-cdrw.service
systemctl daemon-reload
systemctl enable pi3b-cdrw.service
systemctl restart pi3b-cdrw.service || true

echo "--- service status"
systemctl --no-pager --full status pi3b-cdrw.service || true
echo "--- recent log"
journalctl -u pi3b-cdrw.service -n 20 --no-pager || true
