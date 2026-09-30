#!/bin/bash
# Persistence via tmux: `command = tmux new-session -A -s main` -> kill -9 the GUI -> restart -> session and its output survive.
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty
echo "command = tmux -f /dev/null new-session -A -s main" > ~/.config/ghostty/config.ghostty
CTL=/src/host/seancectl
/src/host/seance-host > /out/persist-host1.log 2>&1 &
H1=$!
sleep 12
$CTL exec 1 "echo BEFORE_GUI_DEATH_$((6*7))"
$CTL wait 1 "^BEFORE_GUI_DEATH_42" 20 && echo "session 1: marker visible"
$CTL exec 1 "sleep 300 &"   # a background job that must outlive the GUI
sleep 1
echo "--- killing the GUI with SIGKILL"
kill -9 $H1; sleep 2
pgrep -u tester -x tmux >/dev/null && echo "tmux server still alive: yes" || echo "tmux server still alive: NO"
/src/host/seance-host > /out/persist-host2.log 2>&1 &
sleep 12
$CTL list | head -2
$CTL read focused | grep -q "BEFORE_GUI_DEATH_42" && echo "PASS  restarted GUI reattached to the same session (output preserved)" || echo "FAIL  session content missing after restart"
pgrep -u tester -f "sleep 300" >/dev/null && echo "PASS  background job survived GUI death" || echo "FAIL  background job died"
'
pkill -TERM seance-host
