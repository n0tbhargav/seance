#!/bin/bash
# How long does closing a tab/pane take? (surface free time + wall time from `seancectl close` to the pane disappearing)
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su -s /bin/bash tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash SEANCE_STATS=1
/src/host/seance-host > /out/closetime.log 2>&1 &
sleep 12
CTL=/src/host/seancectl
for i in 1 2 3; do
  T=$($CTL newtab); sleep 3
  case $i in
    2) $CTL exec $T "sleep 300"; sleep 1; echo "(close #2 has a running foreground sleep)";;
    3) $CTL exec $T "trap \"\" HUP TERM INT; sleep 300"; sleep 1; echo "(close #3 has a child that ignores HUP/TERM/INT)";;
  esac
  S=$(date +%s.%N); $CTL close $T
  while $CTL list | grep -q "pane=$T "; do sleep 0.05; done
  E=$(date +%s.%N)
  python3 -c "print(\"close #$i: pane gone after %.2fs\" % ($E-$S))"
done
grep -a "surface free" /out/closetime.log
'
pkill -TERM seance-host
