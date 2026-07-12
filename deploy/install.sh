#!/bin/sh
# Deploy rk3588-evb7-v11 runtime bits on the OFFICIAL BSP Debian rootfs.
# Run as root ON THE BOARD, from this deploy/ directory.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)

# WiFi kernel module must already be at /lib/modules/bcmdhd.ko (from boot.img
# rebuild step). Warn if missing.
[ -f /lib/modules/bcmdhd.ko ] || echo "WARN: /lib/modules/bcmdhd.ko not found -> WiFi will not load"

install -m644 "$DIR/bcmdhd.service"       /etc/systemd/system/bcmdhd.service
install -m644 "$DIR/brcm-bt.service"      /etc/systemd/system/brcm-bt.service
install -d /etc/NetworkManager/conf.d
install -m644 "$DIR/wifi-no-randmac.conf" /etc/NetworkManager/conf.d/wifi-no-randmac.conf
install -m755 "$DIR/selftest.sh"          /usr/local/bin/rk3588-selftest

systemctl daemon-reload
systemctl enable bcmdhd.service brcm-bt.service
systemctl restart NetworkManager || true

echo "Installed. WiFi(bcmdhd)+BT(brcm-bt) enabled, NM MAC-randomization disabled."
echo "Reboot, then run: rk3588-selftest"
