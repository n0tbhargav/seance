#!/bin/bash
# M4 spike: Ghostty core (full libghostty, Linux platform patch) hosted in GTK3, software GL (llvmpipe), CPU frame -> Cairo.
Xvfb :9 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
export GHOSTTY_LOG=stderr
./host/seance-host > /out/host.log 2>&1 &
P=$!
sleep 12
xdotool type --delay 60 'echo hello from ghostty core'; xdotool key Return
sleep 4
xwd -root -silent | convert xwd:- /out/host.png
kill $P 2>/dev/null
echo "--- host log (tail)"; tail -25 /out/host.log
