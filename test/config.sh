#!/bin/bash
# M1 test: config file + Ghostty-format theme; verify colors from screenshot pixels; verify zoom changes cell size.
Xvfb :9 -screen 0 1024x768x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 XDG_CONFIG_HOME=/tmp/cfg
mkdir -p /tmp/cfg/seance/themes
cat > /tmp/cfg/seance/themes/testtheme <<'T'
# ghostty-format theme
background = #1e1e2e
foreground = #cdd6f4
palette = 1=#f38ba8
T
cat > /tmp/cfg/seance/config <<'C'
theme = testtheme
font-size = 14
cursor-color = #ff00ff
C
./seance sh -c 'printf "\033[31mRED\033[0m plain\n"; sleep 30' &
sleep 3
xwd -root -silent | convert xwd:- /out/config1.png
echo "bg pixel: $(convert /out/config1.png -crop 1x1+700+500 txt:- | tail -1 | grep -o '#[0-9A-F]\{6\}')  (want #1E1E2E)"
echo "has f38ba8 (palette red): $(convert /out/config1.png -format %c histogram:info:- | grep -ci f38ba8)"
echo "has ff00ff (cursor): $(convert /out/config1.png -format %c histogram:info:- | grep -ci ff00ff)"
# zoom: measure cursor width before/after ctrl+plus
w1=$(convert /out/config1.png -fill black +opaque "#ff00ff" -trim -format %w info:-)
xdotool key ctrl+plus; sleep 1
xwd -root -silent | convert xwd:- /out/config2.png
w2=$(convert /out/config2.png -fill black +opaque "#ff00ff" -trim -format %w info:-)
echo "cursor width before/after zoom: $w1 -> $w2 (want increase)"
