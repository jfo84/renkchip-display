#!/bin/sh
# rsh.sh 'command' — queue a command for the dongle, wait for DONE, print output
BASE=$(cd "$(dirname "$0")" && pwd)
cd "$BASE"
if [ -z "$1" ]; then echo "usage: rsh.sh 'command'" >&2; exit 1; fi
N=$(wc -l < exfil.log | tr -d ' ')
echo "$1" > cmd
i=0
while [ "$i" -lt 100 ]; do
  if tail -n "+$((N + 1))" exfil.log | grep -q '/o/DONE'; then break; fi
  sleep 2
  i=$((i + 1))
done
tail -n "+$((N + 1))" exfil.log | sed -n 's/.*EXFIL \/o\///p' | awk '
  $1 == "DONE" { next }
  $1 == "LINE" { print ""; next }
  { printf "%s ", $1 }
  END { print "" }'
if [ "$i" -ge 100 ]; then echo "[rsh: timed out waiting for DONE]" >&2; fi
