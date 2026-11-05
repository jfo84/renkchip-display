exec >/dev/null 2>&1
BB=/system/bin/busybox
U=http://192.168.8.208:8000
PATH=/data/bb:/system/bin:/sbin:/bin
export PATH

# restore applet symlinks
mkdir -p /data/bb
for A in cat grep egrep fgrep wc ls ln base64 pidof pgrep pkill cp mv rm date dmesg env printenv printf readlink realpath uname sleep xargs mkdir rmdir touch; do
  ln -sf $BB /data/bb/$A
done

# re-apply fixes: patched wfd sink + valid 1080p EDID
toolbox mount -o bind /data/wfdp /system/bin/wfd
toolbox mount -o bind /data/edid1080 /sys/class/display/HDMI/data

# persistent logcat to /data (survives mode switches and rotation)
ln -sf /data/log4.txt /data/boa/www/log4.txt
logcat -c
logcat -v time -f /data/log4.txt -r 64 -n 4 &

$BB wget -q -O /dev/null $U/e/POLLER4
while :
do
  : > /data/c
  $BB wget -q -O /data/c $U/cmd
  read C < /data/c
  if [ -n "$C" ]
  then
    eval "$C" > /data/o 2>&1
    while read L
    do
      for W in $L
      do
        $BB wget -q -O /dev/null "$U/o/$W"
      done
      $BB wget -q -O /dev/null "$U/o/LINE"
    done < /data/o
    $BB wget -q -O /dev/null "$U/o/DONE"
  fi
  sleep 3
done
