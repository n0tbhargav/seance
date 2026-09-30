#!/bin/bash
# Does the given host binary survive 20s and answer on its control socket? (as non-root)
useradd -m tester 2>/dev/null
cp /src/host/libubsan.so.0* /usr/lib64/ 2>/dev/null
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
for bin in "$@"; do
  su tester -c "
  export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 SHELL=/bin/bash UBSAN_OPTIONS=print_stacktrace=1
  $bin > /tmp/alive.log 2>&1 &
  HP=\$!
  sleep 15
  if kill -0 \$HP 2>/dev/null; then echo \"$bin: ALIVE\"; else echo \"$bin: DIED\"; fi
  grep -a -B1 -A22 'fatal signal' /tmp/alive.log | cut -c1-170 | head -30
  kill -TERM \$HP 2>/dev/null; sleep 1
  "
done
