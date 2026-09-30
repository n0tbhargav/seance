#!/bin/bash
# Throughput baseline: time `cat bigfile` inside each terminal (same Xvfb, same 100x30 grid).
# Numbers are relative (amd64 emulation on an arm64 host), compare terminals to each other only.
MB=${MB:-20}
Xvfb :9 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:9 NO_AT_BRIDGE=1 LANG=C.UTF-8 LC_ALL=C.UTF-8 TERM=xterm-256color
mkdir -p /tmp/bench
python3 - <<PY
import random, os
mb=int(os.environ.get("MB","20"))
random.seed(1)
words=[ "alpha","bravo","charlie","delta","echo","foxtrot","golf","hotel","india","juliet","kilo","lima","mike"]
def gen(fn, f):
    n=0; target=mb*1024*1024
    with open(fn,"w") as o:
        while n<target:
            s=f()
            o.write(s); n+=len(s.encode())
gen("/tmp/bench/ascii", lambda: " ".join(random.choice(words) for _ in range(14))+"\n")
gen("/tmp/bench/sgr",   lambda: "".join("\x1b[38;5;%dm%s\x1b[0m "%(random.randint(16,231),random.choice(words)) for _ in range(10))+"\n")
gen("/tmp/bench/unicode", lambda: " ".join(random.choice(["héllo","wörld","日本語","→","✓","naïve","Ω≈ç√"]) for _ in range(14))+"\n")
PY
run() { # name file cmd...
  local name=$1 file=$2; shift 2
  cat > /tmp/job.sh <<J
#!/bin/bash
S=\$(date +%s.%N); cat /tmp/bench/$file; E=\$(date +%s.%N)
python3 -c "print('%-12s %-8s %6.2fs  %6.1f MB/s' % ('$name','$file',\$E-\$S,$MB/(\$E-\$S)))" >> /out/bench.txt
pkill -f seance-host 2>/dev/null; true
J
  chmod +x /tmp/job.sh
  timeout 240 "$@" 2>>/out/host-stats.txt; sleep 1
}
: > /out/bench.txt; : > /out/host-stats.txt
REPS=${REPS:-3}
for rep in $(seq $REPS); do
  for f in sgr unicode ascii; do   # rotate order vs. first version to expose warmup effects
    run seance  $f ./seance /tmp/job.sh
    [ -x ./host/seance-host ] && SEANCE_CMD=/tmp/job.sh LIBGL_ALWAYS_SOFTWARE=1 SEANCE_STATS=1 run gh-core $f ./host/seance-host
    run xterm   $f xterm -fa Monospace -fs 11 -geometry 100x30 -e /tmp/job.sh
    run lxterm  $f lxterminal --geometry=100x30 -e /tmp/job.sh
  done
done
echo "--- all runs"; cat /out/bench.txt
echo "--- median MB/s"
python3 - <<'PY'
import re, statistics, collections
d=collections.defaultdict(list)
for l in open("/out/bench.txt"):
    m=re.match(r"(\S+)\s+(\S+)\s+[\d.]+s\s+([\d.]+) MB/s", l)
    if m: d[(m[1],m[2])].append(float(m[3]))
for k in sorted(d, key=lambda k:(k[1],k[0])): print("%-8s %-8s median %6.1f  runs %s" % (k[0],k[1],statistics.median(d[k]),d[k]))
PY
