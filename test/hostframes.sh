#!/bin/bash
# Do frames flow when a command is given via SEANCE_CMD? (debug for bench 0-frame stats)
Xvfb :9 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SEANCE_STATS=1
SEANCE_CMD='seq 1 40; sleep 8' ./host/seance-host > /out/hf.log 2>&1 &
P=$!
sleep 12
xwd -root -silent | convert xwd:- /out/hf.png
kill -TERM $P; sleep 2
grep -a "host stats\|error\|warn" /out/hf.log | head
