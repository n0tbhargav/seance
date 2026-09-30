#!/bin/bash
# Custom install path (with a space!) + symlink launch + --install-desktop + --shell-hook.
zypper -q --non-interactive install --no-recommends desktop-file-utils >/dev/null 2>&1
useradd -m tester 2>/dev/null
D="/home/tester/My Apps"; mkdir -p "$D" && python3 -c "import tarfile,sys; tarfile.open('/src/dist/seance-0.1.2-linux-x86_64.tar.gz').extractall(sys.argv[1])" "$D" && chown -R tester /home/tester
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su -s /bin/bash tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
ok() { [ "$1" = 0 ] && echo "PASS  $2" || echo "FAIL  $2"; }
B="/home/tester/My Apps/seance/bin"
mkdir -p ~/bin && ln -sf "$B/seance" ~/bin/seance && ln -sf "$B/seancectl" ~/bin/seancectl
~/bin/seance --version | grep -q "seance 0.1.2"; ok $? "runs through a symlink from a custom path with a space"
~/bin/seance --install-desktop
grep -q "^Exec=\"/home/tester/My Apps/seance/bin/seance\"" ~/.local/share/applications/seance.desktop; ok $? "desktop entry Exec is the quoted real path"
[ -f ~/.local/share/icons/hicolor/256x256/apps/seance.png ]; ok $? "icon copied for this install"
desktop-file-validate ~/.local/share/applications/seance.desktop; ok $? "desktop entry passes desktop-file-validate"
~/bin/seance --shell-hook tcsh | grep -q "My Apps/seance/share/seance/shell/seance.tcsh"; ok $? "shell hook line points at this install (quoted path)"
~/bin/seance --uninstall-desktop >/dev/null
[ ! -e ~/.local/share/applications/seance.desktop ]; ok $? "--uninstall-desktop removes the launcher"
mkdir -p ~/.config/ghostty; printf "theme = Dracula\nwindow-show-tab-bar = always\n" > ~/.config/ghostty/config.ghostty
~/bin/seance > /tmp/cp.log 2>&1 &
sleep 12
~/bin/seancectl exec 1 "echo RUNS_FROM_CUSTOM_PATH; echo RES=\$GHOSTTY_RESOURCES_DIR"; sleep 3
~/bin/seancectl read 1 | grep -q "RES=/home/tester/My Apps/seance/share/ghostty"; ok $? "resources resolve inside the custom path"
DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 700x120+0+0 +repage /out/custom_alwaystabs.png
'
pkill -TERM seance
