#!/bin/bash
# Real-application + protocol checks in the Ghostty-core host, asserted via seancectl as a non-root user.
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
/src/host/seance-host > /out/apps2-host.log 2>&1 &
sleep 12
bash /src/test/apps2_inner.sh
'
pkill -TERM seance-host
