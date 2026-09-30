#!/bin/bash
# Ghostty-core host: native Ghostty config + theme file, font-size, ligatures, double-click select -> PRIMARY.
Xvfb :9 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash XDG_CONFIG_HOME=/tmp/cfg
mkdir -p /tmp/cfg/ghostty/themes
cat > /tmp/cfg/ghostty/themes/testtheme <<'T'
background = #1e1e2e
foreground = #cdd6f4
palette = 1=#f38ba8
palette = 2=#a6e3a1
T
cat > /tmp/cfg/ghostty/config.ghostty <<'C'
theme = testtheme
font-size = 15
cursor-style = bar
C
./host/seance-host > /out/host2.log 2>&1 &
P=$!
sleep 12
xdotool type --delay 50 'printf "\033[31mred\033[32m green\033[0m  => != -> <= >= === www alpha bravo\n"'; xdotool key Return
sleep 4
xwd -root -silent | convert xwd:- /out/host2.png
# double-click on "bravo" (find by scanning: last word of row 2). rows are ~24px at 15pt; row 1 is the printf echo, row 2 output.
xdotool mousemove 510 67 click --repeat 2 --delay 60 1; sleep 2
echo "PRIMARY: [$(xclip -o -selection primary 2>&1)]"
xwd -root -silent | convert xwd:- /out/host2_sel.png
kill -TERM $P; sleep 1
grep -a "theme\|font\|error" /out/host2.log | head -8
