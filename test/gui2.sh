#!/bin/bash
# Tab interactions with real mouse events: switch, +, close x, middle-click close, drag reorder, right-click menu.
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty; printf "theme = Dracula\nfont-size = 12\n" > ~/.config/ghostty/config.ghostty
CTL=/src/host/seancectl
ok() { [ "$1" = 0 ] && echo "PASS  $2" || echo "FAIL  $2"; }
layout() { $CTL list | sed -n "s/pane=\([0-9]*\) tab=\([0-9]*\).*/\1@\2/p" | tr "\n" " "; }
ntabs() { $CTL list | sed -n "s/.* tab=\([0-9]*\) .*/\1/p" | sort -u | wc -l; }
/src/host/seance-host --size 1000x560 > /out/gui2.log 2>&1 &
sleep 12
T2=$($CTL newtab); sleep 3; $CTL split $T2 right; sleep 3; T3=$($CTL newtab); sleep 3
echo "layout: $(layout)   (panes@tab)"
[ "$(ntabs)" = 3 ]; ok $? "3 tabs created"

# tab geometry: ☰ button on the left, then tabs ~116px wide starting at x~31; label centre 75+116*i, close x at 132+116*i, y=13
xdotool mousemove 75 13 click 1; sleep 2
$CTL exec focused "echo CLICK_SWITCHED"; $CTL wait 1 "CLICK_SWITCHED" 15; ok $? "clicking a tab moves keyboard focus to its pane"

xdotool mousemove 988 13 click 1; sleep 4
[ "$(ntabs)" = 4 ]; ok $? "+ button adds a tab ($(ntabs) tabs)"

xdotool mousemove 480 13 click 1; sleep 3        # x on the 4th tab
[ "$(ntabs)" = 3 ]; ok $? "close button closes a tab ($(ntabs) tabs)"

xdotool mousemove 191 13 click 2; sleep 3         # middle-click the 2nd tab (2 panes)
[ "$(ntabs)" = 2 ]; ok $? "middle-click closes a tab (and its split panes) ($(ntabs) tabs; layout: $(layout))"

BEFORE=$(layout)
xdotool mousemove 191 13 mousedown 1; sleep 0.3
xdotool mousemove 165 13; sleep 0.2; xdotool mousemove 110 13; sleep 0.2; xdotool mousemove 50 13; sleep 0.3; xdotool mouseup 1; sleep 2
AFTER=$(layout)
echo "drag reorder: before [$BEFORE] after [$AFTER]"
[ "$BEFORE" != "$AFTER" ]; ok $? "dragging a tab reorders it"

xdotool mousemove 75 13 click 3; sleep 2
DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 420x300+0+0 +repage /out/gui2_menu.png
xdotool key Escape; sleep 1
'
pkill -TERM seance-host
