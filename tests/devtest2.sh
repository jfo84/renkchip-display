#!/system/bin/sh
BB=/system/bin/busybox
U=http://192.168.8.208:8000
W=/data/boa/www
T=/data/tz
export PATH=/system/bin:/sbin:/bin
$BB rm -f /data/tz_probe
$BB mkdir -p $T
{
  $BB wget -q -O $T/test_demux_dev $U/test_demux_dev
  $BB wget -q -O $T/demux_seedrun $U/demux_seedrun
  $BB wget -q -O $T/fuzz_regress_pmt.seed $U/fuzz_regress_pmt.seed
  $BB chmod 755 $T/test_demux_dev $T/demux_seedrun
  echo "=== files ==="
  $BB ls -la $T
  echo "=== checks ==="
  $T/test_demux_dev 2>/dev/null
  echo "CHECKS_EXIT=$?"
  echo "=== fuzz 50000 ==="
  $T/test_demux_dev --fuzz 50000 2>/dev/null
  echo "FUZZ_EXIT=$?"
  echo "=== regression seed ==="
  $T/demux_seedrun $T/fuzz_regress_pmt.seed
  echo "SEED_EXIT=$?"
  echo "=== df after ==="
  /system/bin/toolbox df /data
  echo DEVTEST_DONE
} > $W/devtest.txt 2>&1
