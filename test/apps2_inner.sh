#!/bin/bash
# Runs as the non-root user inside apps2.sh. Each stage gets its own tab; screens are dumped to /out/apps2-<stage>.txt.
CTL=/src/host/seancectl
pass=0; fail=0
ok() { if [ "$1" = 0 ]; then echo "PASS  $2"; pass=$((pass+1)); else echo "FAIL  $2"; fail=$((fail+1)); fi; }
newtab() { P=$($CTL newtab | tr -d '\n'); sleep 2; }
dump() { $CTL read "$P" > "/out/apps2-$1.txt"; }
endtab() { $CTL close "$P"; sleep 1; }

# --- vim
newtab
printf 'int main(void) {\n  return 0;\n}\n' > /tmp/demo.c
$CTL exec $P "vim -u NONE -c 'syntax on' /tmp/demo.c"
$CTL wait $P "int main" 20; ok $? "vim shows file"
dump vim
grep -q '^~' /out/apps2-vim.txt; ok $? "vim filler lines"
$CTL key $P esc; $CTL key $P : q enter
$CTL exec $P 'echo VIM_GONE'; $CTL wait $P '^VIM_GONE' 15; ok $? "vim exits to shell"
endtab

# --- tmux
newtab
$CTL exec $P "tmux -f /dev/null new-session"
sleep 4; dump tmux1
grep -q '\[0\] 0:' /out/apps2-tmux1.txt; ok $? "tmux status bar"
$CTL key $P ctrl-b; $CTL key $P %; sleep 2      # ctrl-b %  (split)
$CTL exec $P 'echo TMUX_SPLIT_OK'; $CTL wait $P 'TMUX_SPLIT_OK' 15; ok $? "tmux split pane works"
dump tmux2
$CTL exec $P exit; sleep 1; $CTL exec $P exit; sleep 1
$CTL exec $P 'echo TMUX_GONE'; $CTL wait $P '^TMUX_GONE' 15; ok $? "tmux exits cleanly"
endtab

# --- less
newtab
$CTL exec $P 'seq 1 300 | less'
$CTL wait $P '^:' 15; $CTL key $P space; sleep 1
dump less
grep -q '^4[0-9]$' /out/apps2-less.txt; ok $? "less pages forward"
$CTL key $P q
endtab

# --- Kitty keyboard protocol
newtab
cat > /tmp/kitty.py <<'PY'
import sys, os, tty, termios, select
fd = sys.stdin.fileno(); old = termios.tcgetattr(fd); tty.setraw(fd)
sys.stdout.write("\x1b[>1u"); sys.stdout.flush()
sys.stdout.write("READY\r\n"); sys.stdout.flush()
select.select([fd], [], [], 20)
data = os.read(fd, 64)
sys.stdout.write("\x1b[<u"); termios.tcsetattr(fd, termios.TCSADRAIN, old)
sys.stdout.write("GOT=%r\r\n" % data); sys.stdout.flush()
PY
$CTL exec $P 'python3 /tmp/kitty.py'; $CTL wait $P 'READY' 15
$CTL key $P shift-enter; $CTL wait $P 'GOT=' 15
dump kitty
grep 'GOT=' /out/apps2-kitty.txt | tail -1
grep -q 'GOT=b.\\x1b\[13;2u' /out/apps2-kitty.txt; ok $? "Kitty keyboard: Shift+Enter -> CSI 13;2u"
endtab

# --- bracketed paste (an app that enables mode 2004 must see the paste wrapped in CSI 200~ ... CSI 201~)
newtab
$CTL exec $P "printf '\033[?2004h'; cat -v"
sleep 2
$CTL send $P 'first\nsecond'; sleep 2
dump paste
grep -q '\^\[\[200~first' /out/apps2-paste.txt; ok $? "paste is wrapped in bracketed-paste start marker"
grep -q 'second\^\[\[201~' /out/apps2-paste.txt; ok $? "paste ends with bracketed-paste end marker"
$CTL key $P ctrl-c
endtab

# --- OSC 52 clipboard write
newtab
$CTL exec $P 'printf "\033]52;c;%s\a" $(echo -n OSC52_HELLO | base64)'; sleep 3
CB=$(DISPLAY=:9 xclip -o -selection clipboard 2>&1)
echo "clipboard after OSC52: [$CB]"
[ "$CB" = "OSC52_HELLO" ]; ok $? "OSC 52 writes the system clipboard"
endtab

echo "== $pass passed, $fail failed"
