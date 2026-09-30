#!/bin/bash
# Tabs + splits driven through Ghostty keybinds -> host actions.
Xvfb :9 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash XDG_CONFIG_HOME=/tmp/cfg5
mkdir -p /tmp/cfg5/ghostty
cat > /tmp/cfg5/ghostty/config.ghostty <<'C'
theme = Dracula
font-size = 11
keybind = ctrl+shift+t=new_tab
keybind = ctrl+shift+o=new_split:right
keybind = ctrl+shift+e=new_split:down
keybind = ctrl+alt+h=goto_split:left
keybind = ctrl+alt+l=goto_split:right
keybind = ctrl+shift+1=goto_tab:1
keybind = ctrl+shift+2=goto_tab:2
C
[ -n "$EXTRA_CFG" ] && echo "$EXTRA_CFG" >> /tmp/cfg5/ghostty/config.ghostty
./host/seance-host > /out/host5.log 2>&1 &
P=$!
sleep 12
t() { xdotool type --delay 40 "$1"; xdotool key Return; sleep 2; }
t 'echo PANE_ONE'
xdotool key ctrl+shift+o; sleep 4
t 'echo PANE_TWO'
xdotool key ctrl+shift+e; sleep 4
t 'echo PANE_THREE'
xwd -root -silent | convert xwd:- /out/host5_splits.png
xdotool key ctrl+alt+h; sleep 2
t 'echo BACK_IN_ONE'
xwd -root -silent | convert xwd:- /out/host5_nav.png
xdotool key ctrl+shift+t; sleep 5
t 'echo TAB_TWO'
xwd -root -silent | convert xwd:- /out/host5_tab2.png
xdotool key ctrl+shift+1; sleep 2
t 'echo TAB_ONE_AGAIN'
xdotool key ctrl+shift+2; sleep 2
t 'exit'
sleep 3
xwd -root -silent | convert xwd:- /out/host5_after_exit.png
kill -TERM $P 2>/dev/null; sleep 1
grep -a "error\|warn.*surface\|crash\|assert" /out/host5.log | head -5
