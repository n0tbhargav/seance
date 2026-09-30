#!/bin/sh
# Runtime smoke test: run seance under Xvfb on Leap 15.4 (same userland as SLES 15 SP4),
# have it print colored text, screenshot the X root window.
set -e
zypper -q --non-interactive install --no-recommends xvfb-run xorg-x11-server-extra gtk3-tools dejavu-fonts ImageMagick xwd >/dev/null
Xvfb :9 -screen 0 1024x768x24 >/dev/null 2>&1 &
sleep 3
export DISPLAY=:9 NO_AT_BRIDGE=1
./seance sh -c 'printf "\033[1;32mhello \033[38;2;255;128;0morange\033[0m\n"; sleep 6' &
sleep 4
xwd -root -silent | convert xwd:- /out/shot.png
wait || true
