#!/bin/bash
# Translucency with a real compositor: xcompmgr + red root background; background-opacity = 0.5 must blend with red.
zypper -q --non-interactive install --no-recommends xcompmgr xsetroot >/dev/null 2>&1
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x${DEPTH:-24} -ac +extension Composite +extension RENDER >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9
xsetroot -solid "#ff0000"
xcompmgr -c >/tmp/xcomp.log 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash SEANCE_STATS=1
mkdir -p ~/.config/ghostty
printf "theme = Dracula\nbackground-opacity = ${OPACITY:-0.5}\n" > ~/.config/ghostty/config.ghostty
/src/host/seance-host --size 700x400 > /out/trans.log 2>&1 &
sleep 14
/src/host/seancectl exec 1 "echo TRANSLUCENT_TEST"
sleep 3
DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 800x450+0+0 +repage /out/trans.png
echo "pixel inside terminal: $(DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 1x1+400+300 txt:- | tail -1 | grep -o "#[0-9A-F]\{6\}")   (opaque Dracula would be #282A36)"
grep -a "translucency" /out/trans.log
'
pkill -TERM seance-host
