#!/bin/bash
# Right-click tab menu: rendered? host survives?
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty; printf "theme = Dracula\n" > ~/.config/ghostty/config.ghostty
/src/host/seance-host-sym --size 1000x560 > /tmp/menu.log 2>&1 &
HP=$!
sleep 12
/src/host/seancectl newtab >/dev/null; sleep 3
xdotool mousemove 40 13 click 3; sleep 2
DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 420x300+0+0 +repage /out/menu_${TAG}.png
xdotool key Escape; sleep 3
kill -0 $HP 2>/dev/null && echo "$TAG: host alive after menu" || echo "$TAG: HOST DIED after menu"
'
pkill -TERM seance-host
