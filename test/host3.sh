#!/bin/bash
# Ghostty-core host: paste (Ctrl+Shift+V), copy (Ctrl+Shift+C), wheel scrollback.
Xvfb :9 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash XDG_CONFIG_HOME=/tmp/cfg3
mkdir -p /tmp/cfg3
rm -f /tmp/pasted_ok
./host/seance-host > /out/host3.log 2>&1 &
P=$!
sleep 12
# 1. paste
printf 'touch /tmp/pasted_ok\n' | xclip -selection clipboard -i
xdotool key ctrl+shift+v; sleep 3
[ -f /tmp/pasted_ok ] && echo "PASTE: ok" || echo "PASTE: FAILED (file missing)"
# 2. output + copy: print a distinctive word, double-click it, ctrl+shift+c
xdotool type --delay 40 'echo copyme_token_42'; xdotool key Return; sleep 3
xdotool mousemove 60 45 click --repeat 2 --delay 60 1; sleep 1
xdotool key ctrl+shift+c; sleep 1
echo "CLIPBOARD after copy: [$(xclip -o -selection clipboard 2>&1)]"
# 3. scrollback: 200 lines then wheel up
xdotool type --delay 30 'seq 1 200'; xdotool key Return; sleep 4
xwd -root -silent | convert xwd:- /out/host3_bottom.png
for i in 1 2 3 4 5 6; do xdotool click 4; sleep 0.2; done; sleep 2
xwd -root -silent | convert xwd:- /out/host3_scrolled.png
kill -TERM $P; sleep 1
