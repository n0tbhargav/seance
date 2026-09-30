#!/bin/bash
# Split zoom / resize / equalize and move_tab via Ghostty keybinds; verified through pane sizes in `seancectl list`.
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty
cat > ~/.config/ghostty/config.ghostty <<C
theme = Dracula
font-size = 12
keybind = ctrl+shift+z=toggle_split_zoom
keybind = ctrl+shift+q=equalize_splits
keybind = ctrl+alt+shift+right=resize_split:right,120
keybind = ctrl+shift+m=move_tab:1
C
CTL=/src/host/seancectl
ok() { [ "$1" = 0 ] && echo "PASS  $2" || echo "FAIL  $2"; }
cols() { $CTL list | sed -n "s/.*pane=$1 .*cols=\([0-9]*\) .*/\1/p"; }
tabof() { $CTL list | sed -n "s/.*pane=$1 tab=\([0-9]*\) .*/\1/p"; }
/src/host/seance-host --size 1000x560 > /out/gui3.log 2>&1 &
HP=$!
sleep 12
$CTL split 1 right; sleep 4              # pane 2 on the right, focused
W_HALF=$(cols 1); echo "after split: pane1 cols=$W_HALF pane2 cols=$(cols 2)"
xdotool key ctrl+alt+shift+Right; sleep 2
W_AFTER=$(cols 1); echo "after resize_split:right: pane1 cols=$W_AFTER"
[ "$W_AFTER" -gt "$W_HALF" ]; ok $? "resize_split changes the divider (pane1 $W_HALF -> $W_AFTER cols)"
xdotool key ctrl+shift+q; sleep 2
W_EQ=$(cols 1); echo "after equalize: pane1 cols=$W_EQ pane2 cols=$(cols 2)"
D=$(( W_EQ - $(cols 2) )); D=${D#-}
[ "$D" -le 3 ]; ok $? "equalize_splits makes panes equal (diff $D cols)"
xdotool key ctrl+shift+z; sleep 3
Z=$(cols 2); echo "zoomed: pane2 cols=$Z"
[ "$Z" -gt 90 ]; ok $? "toggle_split_zoom expands the focused pane ($Z cols)"
xdotool key ctrl+shift+z; sleep 3
U=$(cols 2); echo "unzoomed: pane2 cols=$U"
[ "$U" -lt 70 ]; ok $? "second toggle restores the split ($U cols)"
T2=$($CTL newtab); sleep 3
echo "tabs before move: pane1@$(tabof 1) pane$T2@$(tabof $T2)"
xdotool key ctrl+shift+m; sleep 2
echo "tabs after move_tab:1: pane1@$(tabof 1) pane$T2@$(tabof $T2)"
[ "$(tabof $T2)" = 0 ]; ok $? "move_tab reorders (new tab wrapped to position 0)"
kill -0 $HP && echo "host alive" || echo "HOST DIED"
'
pkill -TERM seance-host
