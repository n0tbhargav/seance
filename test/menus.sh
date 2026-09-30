#!/bin/bash
# Right-click context menu, hamburger menu, window-show-tab-bar=always, window icon, --install-desktop.
zypper -q --non-interactive install --no-recommends xprop >/dev/null 2>&1
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su -s /bin/bash tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty; printf "theme = Dracula\n$CFG\n" > ~/.config/ghostty/config.ghostty
CTL=/src/host/seancectl
ok() { [ "$1" = 0 ] && echo "PASS  $2" || echo "FAIL  $2"; }
n() { $CTL list | grep -c pane=; }
/src/host/seance-host > /out/menus.log 2>&1 &
sleep 12
HAS_ICON=0
for w in $(xdotool search --class seance); do
  xprop -id $w _NET_WM_ICON 2>/dev/null | head -c 40 | grep -q "_NET_WM_ICON(CARDINAL)" && HAS_ICON=1
done
[ "$HAS_ICON" = 1 ]; ok $? "a seance window advertises an icon (_NET_WM_ICON)"
xprop -id $(xdotool search --class seance | tail -1) WM_CLASS | grep -q "\"seance\""; ok $? "WM_CLASS is seance (matches the .desktop StartupWMClass)"
echo "panes: $(n)"
# context menu: right-click in the terminal; Down x3 lands on "New Tab" (Copy is disabled, separators skipped)
xdotool mousemove 500 300 click 3; sleep 2
xdotool key Down Down Down Return; sleep 4
[ "$(n)" = 2 ]; ok $? "right-click menu -> New Tab ($(n) panes)"
# hamburger button is in the tab strip (now visible with 2 tabs)
xdotool mousemove 12 13 click 1; sleep 2
xdotool key Down Return; sleep 4
[ "$(n)" = 3 ]; ok $? "hamburger menu -> New Tab ($(n) panes)"
DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 500x60+0+0 +repage /out/menus_strip.png
'
pkill -TERM seance-host
