#!/bin/bash
# Resources: TERM/terminfo, shell integration, named theme from bundled themes.
Xvfb :9 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash XDG_CONFIG_HOME=/tmp/cfg4
mkdir -p /tmp/cfg4/ghostty
cat > /tmp/cfg4/ghostty/config.ghostty <<'C'
theme = Dracula
C
./host/seance-host > /out/host4.log 2>&1 &
P=$!
sleep 12
xdotool type --delay 40 'echo "TERMINFO=$TERMINFO"; ls $TERMINFO/x; infocmp xterm-ghostty | head -2; tput colors'; xdotool key Return
sleep 4
xwd -root -silent | convert xwd:- /out/host4.png
echo "bg pixel: $(convert /out/host4.png -crop 1x1+600+300 txt:- | tail -1 | grep -o '#[0-9A-F]\{6\}')  (Dracula bg = #282A36)"
kill -TERM $P; sleep 1
grep -a "resources\|theme" /out/host4.log | head -5
