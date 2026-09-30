#!/bin/bash
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty; printf "theme = Dracula\n" > ~/.config/ghostty/config.ghostty
/src/host/seance-host-sym --size 1000x560 > /tmp/seq.log 2>&1 &
HP=$!
CTL=/src/host/seancectl
chk() { kill -0 $HP 2>/dev/null && echo "  ok after: $1" || { echo "  DIED at/after: $1"; grep -a "fatal signal" /tmp/seq.log | cut -c1-200; pkill -TERM seance; exit; }; }
sleep 12; chk startup
T2=$($CTL newtab); sleep 3; chk "newtab ($T2)"
$CTL list >/dev/null; chk "list"
$CTL split $T2 right; sleep 3; chk "split"
T3=$($CTL newtab); sleep 3; chk "newtab2 ($T3)"
xdotool mousemove 40 13 click 1; sleep 2; chk "click tab1"
xdotool mousemove 988 13 click 1; sleep 3; chk "click +"
xdotool mousemove 452 13 click 1; sleep 3; chk "click close x"
'
pkill -TERM seance-host
