#!/bin/bash
# Which glyphs render? Default fonts only. Also: do the DEFAULT keybinds create tabs/splits (no user config)?
useradd -m tester 2>/dev/null
[ -n "$FONT_PKG" ] && zypper -q --non-interactive install --no-recommends $FONT_PKG >/dev/null 2>&1
echo "fonts with U+23F5 (⏵): $(fc-list ':charset=23f5' family | sort -u | tr '\n' ';')"
echo "fonts with U+276F (❯): $(fc-list ':charset=276f' family | sort -u | head -3 | tr '\n' ';')"
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty; printf "theme = Dracula\nfont-size = 14\n" > ~/.config/ghostty/config.ghostty
CTL=/src/host/seancectl
/src/host/seance-host > /out/glyph.log 2>&1 &
sleep 12
$CTL exec 1 "printf \"arrows: ⏵⏵ ▶ ❯ → ✓ ✗ ● ⚡ ★ • — …  end\\n\""
sleep 3
DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 900x110+0+0 +repage /out/glyph.png
n() { $CTL list | grep -c pane=; }
echo "panes at start: $(n)"
xdotool key ctrl+shift+t; sleep 4; echo "after Ctrl+Shift+T (default new tab): $(n) panes"
xdotool key ctrl+shift+o; sleep 4; echo "after Ctrl+Shift+O (default split right): $(n) panes"
xdotool key ctrl+shift+e; sleep 4; echo "after Ctrl+Shift+E (default split down): $(n) panes"
'
pkill -TERM seance-host
