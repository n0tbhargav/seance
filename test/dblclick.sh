#!/bin/bash
# M1 test: double-click selects word, triple-click selects line.
Xvfb :9 -screen 0 1024x768x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1
./seance sh -c 'printf "alpha bravo charlie\nsecond line here\n"; sleep 30' &
sleep 3
xdotool mousemove 70 8 click --repeat 2 --delay 50 1; sleep 1
echo "DOUBLE: [$(xclip -o -selection primary)]"
xdotool mousemove 30 26 click --repeat 3 --delay 50 1; sleep 1
echo "TRIPLE: [$(xclip -o -selection primary)]"
