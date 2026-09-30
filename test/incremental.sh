#!/bin/bash
# M1 test: partial (dirty-row) redraw must be pixel-identical to full redraw after a scrolling session.
export DISPLAY=:9 NO_AT_BRIDGE=1 TERM=xterm-256color LANG=C.UTF-8 LC_ALL=C.UTF-8
Xvfb :9 -screen 0 1024x768x24 >/dev/null 2>&1 &
sleep 2
run() { # tag [env]
  ( [ "$2" ] && export SEANCE_FULL_REDRAW=1
    ./seance sh -c 'i=1; while [ $i -le 300 ]; do printf "line %d \033[1;3%dmcolor\033[0m and some text\n" $i $((i%7+1)); i=$((i+1)); done | less -R; ' &
    P=$!; sleep 4
    for k in space space b Down Down Down Up; do xdotool key $k; sleep 0.4; done
    sleep 1; xwd -root -silent | convert xwd:- /out/inc_$1.png; kill $P 2>/dev/null; sleep 1 )
}
run partial
run full 1
echo "differing pixels (partial vs full): $(compare -metric AE /out/inc_partial.png /out/inc_full.png null: 2>&1)"
