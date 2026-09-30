#!/bin/bash
# GUI look: tabs (themed strip, close buttons, + button), split, given theme. Screenshot -> /out/gui_${THEME}.png
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty
printf "theme = %s\nfont-size = 12\n%s\n" "${THEME:-Dracula}" "${EXTRA_CFG}" > ~/.config/ghostty/config.ghostty
CTL=/src/host/seancectl
/src/host/seance-host --size 1000x560 > /out/gui.log 2>&1 &
sleep 12
$CTL exec 1 "echo tab one: build logs"
T2=$($CTL newtab); sleep 3; $CTL exec $T2 "echo tab two: server"
$CTL split $T2 right; sleep 3
T3=$($CTL newtab); sleep 3; $CTL exec $T3 "echo tab three: notes"
sleep 2
DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 1000x330+0+0 +repage /out/gui_${THEME_TAG:-dracula}.png
'
pkill -TERM seance-host
