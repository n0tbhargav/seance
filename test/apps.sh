#!/bin/bash
# M1 test: real applications (vim w/ syntax + tmux split) render sanely. Screenshots -> /out/vim.png /out/tmux.png
Xvfb :9 -screen 0 1024x768x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 TERM=xterm-256color LANG=C.UTF-8 LC_ALL=C.UTF-8
cat > /tmp/demo.c <<'C'
#include <stdio.h>
// demo file
int main(void) {
    printf("héllo wörld — 日本語\n");
    return 0;
}
C
./seance vim -u NONE -c 'syntax on' -c 'set number' /tmp/demo.c &
P=$!; sleep 4
xwd -root -silent | convert xwd:- /out/vim.png
kill $P 2>/dev/null; sleep 1
./seance tmux -f /dev/null new-session 'echo left pane; bash' \; split-window -h 'echo right pane; bash' &
sleep 5
xwd -root -silent | convert xwd:- /out/tmux.png
