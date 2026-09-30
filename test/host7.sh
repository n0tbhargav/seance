#!/bin/bash
# Isolate the prompt-stacking: split only, no further typing.
Xvfb :9 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash XDG_CONFIG_HOME=/tmp/cfg7
mkdir -p /tmp/cfg7/ghostty
printf 'shell-integration = none\nkeybind = ctrl+shift+o=new_split:right\n' > /tmp/cfg7/ghostty/config.ghostty
SEANCE_CMD="python3 /src/test/winch_prog.py" ./host/seance-host > /out/host7.log 2>&1 &
P=$!
sleep 12
sleep 2
xwd -root -silent | convert xwd:- /out/host7_a.png
xdotool key ctrl+shift+o; sleep 6
xwd -root -silent | convert xwd:- /out/host7_b.png
kill -TERM $P 2>/dev/null
