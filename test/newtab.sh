#!/bin/bash
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
/src/host/seance-host > /tmp/nt.log 2>&1 &
HP=$!
sleep 12
kill -0 $HP && echo "alive before newtab"
/src/host/seancectl newtab; echo "newtab rc=$?"
sleep 3
kill -0 $HP && echo "alive after newtab" || echo "DIED after newtab"
/src/host/seancectl list
'
