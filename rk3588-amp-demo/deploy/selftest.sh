#!/bin/bash
# rk3588-evb7-v11 boot self-test: AMP + desktop + WiFi + BT
# Run as root: rk3588-selftest   (installed to /usr/local/bin by install.sh)
PASS=0; FAIL=0
ok(){  echo -e "  [\e[32mOK\e[0m]   $1"; PASS=$((PASS+1)); }
bad(){ echo -e "  [\e[31mFAIL\e[0m] $1"; FAIL=$((FAIL+1)); }
inf(){ echo -e "  [info] $1"; }

echo "== rk3588-evb7-v11 self-test =="

# 1. CPU count: AMP took cpu_l3 -> Linux should see 7 cores
n=$(nproc)
[ "$n" = "7" ] && ok "nproc=7 (cpu_l3 handed to AMP core)" || bad "nproc=$n (expected 7)"

# 2. AMP heartbeat in the 0x30000000 no-map reserved region
if command -v busybox >/dev/null 2>&1; then
  magic=$(busybox devmem 0x30000800 32 2>/dev/null)
  c1=$(busybox devmem 0x30000804 32 2>/dev/null); sleep 1
  c2=$(busybox devmem 0x30000804 32 2>/dev/null)
  if [ "$magic" = "0x414D5033" ]; then
    if [ "$c1" != "$c2" ]; then ok "AMP heartbeat: magic OK, counter $c1 -> $c2"
    else bad "AMP magic OK but counter stuck at $c1 (cpu3 not running?)"; fi
  else bad "AMP magic=$magic (expected 0x414D5033)"; fi
else inf "busybox not found -> skip AMP heartbeat check"; fi

# 3. rpmsg host (Linux AMP side)
dmesg | grep -q "rpmsg host is online" && ok "rpmsg host online" \
  || inf "no rpmsg host line (bare-metal demo implements no rpmsg)"

# 4. Mali G610 GPU
if dmesg | grep -qiE "mali .*GPU identified"; then
  ok "Mali GPU: $(dmesg | grep -i 'GPU identified' | tail -1 | sed 's/.*GPU/GPU/')"
else bad "Mali GPU not identified (check rk3588_g610.config)"; fi

# 5. Desktop / display manager
if systemctl is-active --quiet gdm3 || systemctl is-active --quiet gdm; then ok "desktop: gdm active"
elif systemctl is-active --quiet S49weston; then ok "desktop: weston active"
else bad "no display manager active"; fi

# 6. WiFi
if ip link show wlan0 >/dev/null 2>&1; then
  st=$(nmcli -t -f DEVICE,STATE dev 2>/dev/null | awk -F: '$1=="wlan0"{print $2}')
  if [ "$st" = "connected" ]; then
    ok "wlan0 connected ($(ip -4 addr show wlan0 | awk '/inet /{print $2}'))"
  else bad "wlan0 present but state=${st:-unknown}"; fi
else bad "wlan0 missing (bcmdhd.ko not loaded?)"; fi

# 7. Bluetooth
if command -v hciconfig >/dev/null 2>&1 && hciconfig hci0 >/dev/null 2>&1; then
  if hciconfig hci0 | grep -q "UP RUNNING"; then ok "hci0 UP RUNNING"
  else hciconfig hci0 up 2>/dev/null
       hciconfig hci0 | grep -q "UP RUNNING" && ok "hci0 brought UP" || bad "hci0 present but not UP"; fi
else bad "hci0 missing (brcm-bt.service not attached?)"; fi

echo "== result: $PASS OK, $FAIL FAIL =="
[ "$FAIL" = "0" ]
