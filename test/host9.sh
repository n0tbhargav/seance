#!/bin/bash
# CLI options (-e, --size), live config reload (theme change without restart), new window.
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty
printf "theme = Dracula\nkeybind = ctrl+shift+r=reload_config\nkeybind = ctrl+shift+n=new_window\n" > ~/.config/ghostty/config.ghostty
CTL=/src/host/seancectl
/src/host/seance-host --size 800x500 -e bash -c "echo CLI_E_WORKS; exec bash" > /out/host9.log 2>&1 &
sleep 12
$CTL wait 1 "CLI_E_WORKS" 15 && echo "PASS  -e runs the given command" || echo "FAIL  -e"
echo "bg before: $(convert 2>/dev/null; DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 1x1+400+300 txt:- | tail -1 | grep -o "#[0-9A-F]\{6\}")  (Dracula #282A36)"
sed -i "s/^theme = .*/theme = iTerm2 Solarized Dark/" ~/.config/ghostty/config.ghostty
xdotool key ctrl+shift+r; sleep 5
NEWBG=$(DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 1x1+400+300 txt:- | tail -1 | grep -o "#[0-9A-F]\{6\}")
echo "bg after reload: $NEWBG  (iTerm2 Solarized Dark)"
[ "$NEWBG" = "#002B36" ] && echo "PASS  live config reload applied the new theme" || echo "FAIL  reload"
xdotool key ctrl+shift+n; sleep 10
echo "seance processes: $(pgrep -u tester -c seance-host)  (2 expected after new_window)"
'
pkill -TERM seance-host
