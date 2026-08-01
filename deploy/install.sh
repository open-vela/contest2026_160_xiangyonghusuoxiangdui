#!/bin/sh
# Deploy rk3588-evb7-v11 runtime bits on the OFFICIAL BSP Debian rootfs.
# Run as root ON THE BOARD, from this deploy/ directory.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)

# WiFi kernel module must already be at /lib/modules/bcmdhd.ko (from boot.img
# rebuild step). Warn if missing.
[ -f /lib/modules/bcmdhd.ko ] || echo "WARN: /lib/modules/bcmdhd.ko not found -> WiFi will not load"

# AMP rpmsg client modules. Not fatal if absent - WiFi/BT below do not need them -
# but say so, because their absence is what makes amp_fb_show report that touch
# will not be forwarded, and that message sends the reader to the wrong place.
for m in rpmsg_char rpmsg_nsh_tty; do
	[ -f "/userdata/$m.ko" ] || echo "WARN: /userdata/$m.ko not found -> amp-rpmsg.service will fail"
done

install -m644 "$DIR/bcmdhd.service"       /etc/systemd/system/bcmdhd.service
install -m644 "$DIR/brcm-bt.service"      /etc/systemd/system/brcm-bt.service
install -m644 "$DIR/amp-rpmsg.service"    /etc/systemd/system/amp-rpmsg.service
install -d /etc/NetworkManager/conf.d
install -m644 "$DIR/wifi-no-randmac.conf" /etc/NetworkManager/conf.d/wifi-no-randmac.conf
install -m755 "$DIR/selftest.sh"          /usr/local/bin/rk3588-selftest

systemctl daemon-reload
systemctl enable bcmdhd.service brcm-bt.service amp-rpmsg.service
systemctl restart NetworkManager || true

# No graphical session, because the AMP core owns this panel.
#
# amp_fb_show has to become DRM master, and a desktop holds it - the program
# exits with "cannot become DRM master" and the reader has no reason to connect
# that to gnome-shell. set-default rather than a service that isolates at boot:
# isolating while systemd is still bringing up the default target is fighting the
# boot transaction, and the correct statement is "graphical never starts", not
# "leave graphical immediately after entering it".
PREV=$(systemctl get-default)
if [ "$PREV" != "multi-user.target" ]; then
	systemctl set-default multi-user.target
	echo "Default target: $PREV -> multi-user.target (no desktop)."
	echo "  To put the desktop back: systemctl set-default graphical.target && reboot"
fi

echo "Installed. WiFi(bcmdhd)+BT(brcm-bt)+AMP rpmsg enabled, NM MAC-randomization disabled."
echo "Reboot, then run: rk3588-selftest"
