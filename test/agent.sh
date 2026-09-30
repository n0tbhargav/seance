#!/bin/bash
# Agent control: drive a pane through seancectl (list/exec/wait/read/key/events/split), as a NON-ROOT user.
useradd -m tester 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
mkdir -p /out
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
${HOSTBIN:-/src/host/seance-host} > /out/agent-host.log 2>&1 &
HP=$!
sleep 12
kill -0 $HP 2>/dev/null && echo "host alive" || echo "HOST DIED"
CTL=/src/host/seancectl
echo "--- list"; $CTL list
$CTL events > /out/agent-events.log &
sleep 1
echo "--- exec + wait"
$CTL exec 1 "echo AGENT_MARK_\$((6*7))"
$CTL wait 1 "^AGENT_MARK_42" 15 && echo "wait: matched" || echo "wait: TIMEOUT"
echo "--- read (last non-empty lines)"; $CTL read 1 | grep -v "^\s*$" | tail -3
echo "--- key ctrl-c interrupts a running command"
$CTL exec 1 "sleep 60"; sleep 1; $CTL key 1 ctrl-c
$CTL exec 1 "echo AFTER_INTERRUPT"
$CTL wait 1 "^AFTER_INTERRUPT" 15 && echo "interrupt: ok" || echo "interrupt: TIMEOUT"
echo "--- command_finished event (needs shell integration)"
$CTL exec 1 "false"; sleep 3
grep -a command_finished /out/agent-events.log | tail -2
echo "--- split via ctl, send to new pane"
$CTL split 1 right; sleep 4
$CTL list
NEWP=$($CTL list | grep -o "pane=[0-9]*" | tail -1 | cut -d= -f2)
$CTL exec $NEWP "echo IN_PANE_$NEWP"
$CTL wait $NEWP "^IN_PANE_$NEWP" 15 && echo "pane $NEWP: ok" || echo "pane $NEWP: TIMEOUT"
echo "--- env inside pane"; $CTL exec 1 "echo ENV=\$SEANCE_PANE:\${SEANCE_SOCKET##*/}"; sleep 2; $CTL read 1 | grep "^ENV=" | tail -1
echo "--- events seen"; sort -u /out/agent-events.log | cut -c1-90 | head -12
'
xwd -root -silent | convert xwd:- /out/agent.png
pkill -TERM seance-host
