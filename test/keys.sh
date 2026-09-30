#!/bin/bash
# Default vs explicit keybinds, same key. CFG env = extra config lines.
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty; printf "theme = Dracula\n$CFG\n" > ~/.config/ghostty/config.ghostty
CTL=/src/host/seancectl
/src/host/seance-host > /out/keys.log 2>&1 &
sleep 12
$CTL events > /out/keys-events.log &
sleep 1
n() { $CTL list | grep -c pane=; }
echo "[$LABEL] start: $(n) panes"
xdotool key ctrl+shift+t; sleep 4; echo "[$LABEL] after ctrl+shift+t: $(n) panes"
xdotool key ctrl+shift+o; sleep 4; echo "[$LABEL] after ctrl+shift+o: $(n) panes"
xdotool key ctrl+shift+v; sleep 1
'
pkill -TERM seance-host
