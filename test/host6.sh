#!/bin/bash
# Resize only (no keybinds): does the prompt redraw cleanly?
Xvfb :9 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash XDG_CONFIG_HOME=/tmp/cfg6
mkdir -p /tmp/cfg6
./host/seance-host > /out/host6.log 2>&1 &
P=$!
sleep 12
xdotool type --delay 40 'echo A'; xdotool key Return; sleep 2
WID=$(xdotool search --name seance | head -1)
xdotool windowsize $WID 700 400; sleep 4
xdotool windowsize $WID 900 500; sleep 4
xdotool windowsize $WID 600 300; sleep 4
xwd -root -silent | convert xwd:- /out/host6.png
kill -TERM $P 2>/dev/null
