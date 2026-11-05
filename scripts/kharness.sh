exec >/dev/null 2>&1
BB=/system/bin/busybox
U=http://192.168.8.208:8000
R=/mnt/ram0

# survival: vendor init marked every daemon oom_score_adj=-1000 (unkillable),
# so the first OOM panics the kernel. Make daemons killable, wfd dies FIRST.
for P in $(ls /proc | grep -E '^[0-9]+$'); do
  N=$(cat /proc/$P/comm 2>/dev/null)
  case "$N" in
    wfd) echo 1000 > /proc/$P/oom_score_adj 2>/dev/null ;;
    init|kthreadd|ueventd) : ;;
    *) echo 0 > /proc/$P/oom_score_adj 2>/dev/null ;;
  esac
done

# wfd spawns at stream start with vendor defaults again; enforce continuously
( while :; do
    for P in $(pidof wfd 2>/dev/null); do
      echo 1000 > /proc/$P/oom_score_adj 2>/dev/null
    done
    sleep 1
  done ) &

# free baseline: DLNA renderer + MediaDaemon are dead weight during Miracast
$BB pkill rk_dlna_dmr 2>/dev/null
$BB pkill MediaDaemon 2>/dev/null
sync
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null

# kernel stream: poll ring buffer, append deltas to ram disk, expose via boa
$BB rm -f $R/k.txt
( while :; do
    $BB dmesg -c >> $R/k.txt 2>/dev/null
    sleep 1
  done ) &
$BB ln -sf $R/k.txt /data/boa/www/k.txt

# ram snapshot: uptime + MemFree + wfd RSS, every 2s
( while :; do
    {
      cat /proc/uptime
      grep -E 'MemFree|MemAvailable' /proc/meminfo
      for P in $(pidof wfd 2>/dev/null); do
        grep VmRSS /proc/$P/status
        grep VmSize /proc/$P/status
      done
    } > $R/m.txt 2>/dev/null
    sleep 2
  done ) &
$BB ln -sf $R/m.txt /data/boa/www/m.txt

$BB wget -q -O /dev/null $U/e/KHARNESSED
