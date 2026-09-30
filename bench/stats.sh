#!/bin/bash
# Where does the time go? Run seance on each data file with SEANCE_STATS=1 (parse vs draw split).
Xvfb :9 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 LANG=C.UTF-8 SEANCE_STATS=1
ls /tmp/bench/ascii >/dev/null 2>&1 || { echo "run bench/run.sh first to generate data (or generate here)"; }
for f in ascii sgr unicode; do
  S=$(date +%s.%N)
  ./seance sh -c "cat /tmp/bench/$f" 2>&1 | grep stats | sed "s/^/$f: /"
  echo "$f wall: $(python3 -c "print(round($(date +%s.%N)-$S,2))")s"
done
