#!/bin/bash
# M1 test: mouse reporting (SGR 1006) reaches the app with correct cell coords.
Xvfb :9 -screen 0 1024x768x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1
./seance sh -c 'printf "\033[?1000h\033[?1006h"; stty raw -echo min 0 time 40; cat | od -c > /out/report.txt' &
sleep 3
# cell (col 5,row 3) 0-based -> pixel center ~ (5*9+4, 3*18+9)
xdotool mousemove 49 63 click 1
sleep 6
cat /out/report.txt
