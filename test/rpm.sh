#!/bin/bash
# Install the RPM into a clean container and run it as a NON-ROOT user from /usr/bin/seance.
rpm -Uvh --nodeps /src/dist/x86_64/seance-0.1.1-1.x86_64.rpm >/dev/null 2>&1 && echo "rpm installed" || { echo "rpm install failed"; exit 1; }
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
mkdir -p ~/.config/ghostty && echo "theme = Dracula" > ~/.config/ghostty/config.ghostty
/usr/bin/seance > /tmp/rpm-host.log 2>&1 &
sleep 12
xdotool type --delay 40 "echo TERM=\$TERM; tput colors; echo res=\$GHOSTTY_RESOURCES_DIR"; xdotool key Return
sleep 4
xwd -root -silent | convert xwd:- /out/rpm.png
pkill -TERM seance; cp /tmp/rpm-host.log /out/rpm-host.log
'
grep -a "error\|failed" /tmp/rpm-host.log | head -3
