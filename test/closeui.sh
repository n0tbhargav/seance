#!/bin/bash
# Is the tab removed from the screen immediately, even while the (stubborn) child is still being killed?
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su -s /bin/bash tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash SEANCE_STATS=1
mkdir -p ~/.config/ghostty; printf "theme = Dracula\n" > ~/.config/ghostty/config.ghostty
/src/host/seance-host > /out/closeui.log 2>&1 &
sleep 12
CTL=/src/host/seancectl
DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 500x40+0+0 +repage /out/closeui_1tab.png
T=$($CTL newtab); sleep 3
$CTL exec $T "trap \"\" HUP TERM INT; sleep 300"; sleep 1
DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 500x40+0+0 +repage /out/closeui_2tabs.png
$CTL close $T; sleep 0.25
DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 500x40+0+0 +repage /out/closeui_after.png
sleep 2
'
pkill -TERM seance-host
cd /out && convert closeui_1tab.png closeui_2tabs.png closeui_after.png -append -scale 200% closeui_all.png
