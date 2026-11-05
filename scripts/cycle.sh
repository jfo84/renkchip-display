#!/bin/sh
# cycle.sh — one command after a dongle reboot: wait for network, re-arm C2,
# redeploy the RAM-disk bundle, verify, and report state.
BASE=$(cd "$(dirname "$0")" && pwd)
cd "$BASE/work" || exit 1

echo "[1/4] waiting for the dongle to answer on main (.160)..."
i=0
while [ $i -lt 60 ]; do
    ping -c 1 -t 2 192.168.8.160 >/dev/null 2>&1 && break
    sleep 5; i=$((i + 1))
done
[ $i -ge 60 ] && { echo "dongle never came back"; exit 1; }
echo "      up (waited $((i * 5))s)"

echo "[2/4] re-arming poller + harness..."
sh rearm.sh

echo "[3/4] deploying RAM-disk bundle..."
./rsh.sh 'busybox wget -q -O /mnt/ram0/bump http://192.168.8.208:8000/bump; chmod 755 /mnt/ram0/bump; busybox wget -q -O /mnt/ram0/wfdcap8.h264 http://192.168.8.208:8000/wfdcap8.h264; ls -la /mnt/ram0/bump /mnt/ram0/wfdcap8.h264'

echo "[4/4] state:"
./rsh.sh 'cat /proc/uptime; getprop init.svc.surfaceflinger; /mnt/ram0/bump x regs >/mnt/ram0/o.txt 2>&1; cat /mnt/ram0/o.txt'
echo "done. TV should show the setup screen; if black: setprop ctl.start surfaceflinger"
