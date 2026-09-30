#!/bin/bash
# Relocatable tarball: extract somewhere arbitrary, run as a normal user.
useradd -m tester 2>/dev/null
mkdir -p /home/tester/apps && python3 -c "import tarfile; tarfile.open(\"/src/dist/seance-0.1.1-linux-x86_64.tar.gz\").extractall(\"/home/tester/apps\")" && chown -R tester /home/tester/apps
Xvfb :9 -screen 0 1280x800x24 -ac >/dev/null 2>&1 &
sleep 2
su tester -c '
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SHELL=/bin/bash
/home/tester/apps/seance/bin/seance --version
/home/tester/apps/seance/bin/seance > /tmp/t.log 2>&1 &
sleep 12
/home/tester/apps/seance/bin/seancectl exec 1 "echo TERM=\$TERM RES=\$GHOSTTY_RESOURCES_DIR"
sleep 3
/home/tester/apps/seance/bin/seancectl read 1 | grep "^TERM="
'
pkill -TERM seance
