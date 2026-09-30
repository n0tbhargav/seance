#!/bin/bash
# tcsh integration: command_finished events (exit code + duration), cwd tracking for new tabs, title events.
zypper -q --non-interactive install --no-recommends tcsh >/dev/null 2>&1
useradd -m -s /usr/bin/tcsh tester 2>/dev/null
usermod -s /usr/bin/tcsh tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su -s /bin/bash tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8
mkdir -p ~/.config/ghostty; printf "theme = Dracula\n" > ~/.config/ghostty/config.ghostty
cat > ~/.tcshrc <<T
set prompt = "%n@%m:%~%# "
T
/src/host/seance-host --shell-hook tcsh >> ~/.tcshrc
echo "--- hook line added to ~/.tcshrc:"; tail -1 ~/.tcshrc
CTL=/src/host/seancectl
ok() { [ "$1" = 0 ] && echo "PASS  $2" || echo "FAIL  $2"; }
SHELL=/usr/bin/tcsh /src/host/seance-host > /out/tcsh.log 2>&1 &
sleep 12
$CTL events > /out/tcsh-events.log &
sleep 1
$CTL exec 1 "false"; sleep 3
$CTL exec 1 "sleep 1"; sleep 4
$CTL exec 1 "true"; sleep 3
grep -a "command_finished" /out/tcsh-events.log | tail -4
grep -aq "exit=1 " /out/tcsh-events.log; ok $? "tcsh: exit status 1 reported for a failing command"
grep -aq "exit=0 duration_ms=[0-9]\{4,\}" /out/tcsh-events.log; ok $? "tcsh: duration measured (sleep 1 >= 1000 ms)"
grep -aq "event\|title pane=1 title=\"tester@" /out/tcsh-events.log; ok $? "tcsh: title event from the prompt"
$CTL exec 1 "cd /tmp"; sleep 3
NEWTAB=$($CTL newtab); sleep 4
$CTL exec $NEWTAB "pwd"; sleep 3
$CTL read $NEWTAB | grep -q "^/tmp"; ok $? "tcsh: a new tab starts in the same directory (/tmp) via OSC 7"
DISPLAY=:9 xwd -root -silent | convert xwd:- -crop 800x200+0+0 +repage /out/tcsh.png
'
pkill -TERM seance-host
