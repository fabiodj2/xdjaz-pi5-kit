#!/bin/sh
# midi.sh NAME...: press FLX4 controls through azbridge -i (test helper)
F=/opt/xdjaz/state/azbridge.in
for k in "$@"; do
  case $k in
  source) m="96 01";; browse) m="96 7D";;
  push) m="96 41";; back) m="96 42";; load1) m="96 46";; load2) m="96 47";;
  down) echo "B6 40 01" > $F; sleep 0.3; continue;; up) echo "B6 40 7F" > $F; sleep 0.3; continue;;
  play1) m="90 0B";; play2) m="91 0B";;
  wait*) sleep ${k#wait}; continue;;
  *) echo "unknown $k" >&2; continue;;
  esac
  echo "$m 7F" > $F; sleep 0.15; echo "$m 00" > $F; sleep 0.6
done
