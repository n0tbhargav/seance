#!/bin/bash
# Flood test for the Ghostty-core host: wall time, frames actually presented to the window during the flood.
Xvfb :9 -screen 0 ${XSCREEN:-1280x800x24} >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 LIBGL_ALWAYS_SOFTWARE=1 LANG=C.UTF-8 SEANCE_STATS=1 MB=${MB:-10}
sed -n "/^mkdir -p/,/^PY/p" bench/run.sh > /tmp/gen.sh; bash /tmp/gen.sh >/dev/null 2>&1
for f in ascii sgr unicode; do
  cat > /tmp/job.sh <<J
#!/bin/bash
S=\$(date +%s.%N); cat /tmp/bench/$f; E=\$(date +%s.%N)
python3 -c "print('$f: cat took %.2fs (%.1f MB/s)' % (\$E-\$S, $MB/(\$E-\$S)))" >> /out/flood.txt
sleep 2; pkill -f seance-host; true
J
  chmod +x /tmp/job.sh
  SEANCE_CMD=/tmp/job.sh ./host/seance-host 2>&1 | grep -a "host stats" | sed "s/^/$f: /" >> /out/flood.txt
done
cat /out/flood.txt
