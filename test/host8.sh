#!/bin/bash
# Split + resize with the REAL shell as a NON-ROOT user (so xterm-ghostty terminfo is honored).
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty
printf "keybind = ctrl+shift+o=new_split:right\nkeybind = ctrl+shift+e=new_split:down\n" > ~/.config/ghostty/config.ghostty
/src/host/seance-host > /out/host8.log 2>&1 &
sleep 12
' 
xdotool type --delay 40 'echo A'; xdotool key Return; sleep 2
xdotool key ctrl+shift+o; sleep 5
xdotool key ctrl+shift+e; sleep 5
xdotool type --delay 40 'echo B'; xdotool key Return; sleep 2
xwd -root -silent | convert xwd:- /out/host8.png
pkill -TERM seance-host
