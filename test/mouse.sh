#!/bin/bash
# M1 test: drag-select text, copy with Ctrl+Shift+C, verify clipboard; screenshot.
set -e
Xvfb :9 -screen 0 1024x768x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1
./seance sh -c 'printf "alpha bravo charlie\nsecond line here\n"; sleep 30' &
sleep 3
# cells are ~9x18px; drag across "bravo" on row 0 (x 54..99)
xdotool mousemove 56 8 mousedown 1 mousemove 80 8 mousemove 96 8 mouseup 1
sleep 1
xdotool key ctrl+shift+c
sleep 1
echo "PRIMARY:   [$(xclip -o -selection primary 2>&1)]"
echo "CLIPBOARD: [$(xclip -o -selection clipboard 2>&1)]"
xwd -root -silent | convert xwd:- /out/mouse.png
