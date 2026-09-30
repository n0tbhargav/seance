#!/bin/bash
# Preflight for running seance on a SLES 15 SP4 (or similar) VM. Run as the user who will run seance, with DISPLAY set.
ok=0; bad=0
pass() { printf '  [ok]   %s\n' "$1"; ok=$((ok+1)); }
fail() { printf '  [FAIL] %s\n' "$1"; bad=$((bad+1)); }
echo "== OS"; . /etc/os-release 2>/dev/null; echo "  $PRETTY_NAME"
glibc=$(ldd --version 2>&1 | head -1 | grep -o '[0-9]\+\.[0-9]\+$'); echo "  glibc $glibc"
[ "$(printf '%s\n2.29\n' "$glibc" | sort -V | head -1)" = "2.29" ] && pass "glibc >= 2.29" || fail "glibc < 2.29 (seance core needs 2.29+)"
echo "== Libraries"
for lib in libgtk-3.so.0 libEGL.so.1 libGL.so.1 libfontconfig.so.1; do
  ldconfig -p | grep -q "$lib" && pass "$lib" || fail "$lib missing (zypper in: libgtk-3-0 Mesa-libEGL1 Mesa-libGL1 fontconfig)"
done
echo "== GL (software rendering is enough)"
if command -v eglinfo >/dev/null 2>&1; then
  LIBGL_ALWAYS_SOFTWARE=1 eglinfo 2>&1 | grep -iE "vendor|renderer|version" | head -3
elif command -v glxinfo >/dev/null 2>&1; then
  out=$(LIBGL_ALWAYS_SOFTWARE=1 glxinfo 2>&1)
  echo "$out" | grep -E "OpenGL (core profile )?(version|renderer)" | head -3 | sed 's/^/  /'
  ver=$(echo "$out" | grep "core profile version string" | grep -o '[0-9]\+\.[0-9]\+' | head -1)
  if [ -z "$ver" ]; then
    echo "  [warn] could not query GL (needs a running X server on \$DISPLAY); re-run inside your desktop session"
  elif [ "$(printf '%s\n4.3\n' "$ver" | sort -V | head -1)" = "4.3" ]; then
    pass "OpenGL core $ver >= 4.3 (Ghostty needs 4.3)"
  else
    fail "OpenGL core $ver < 4.3 (need Mesa llvmpipe: zypper in Mesa-dri)"
  fi
else
  echo "  (install Mesa-demo-x for glxinfo to check the GL version)"
fi
[ -d /usr/lib64/dri ] && ls /usr/lib64/dri | grep -q "swrast\|kms_swrast\|llvmpipe" && pass "Mesa software DRI driver present" || fail "Mesa-dri (llvmpipe/swrast) missing"
echo "== Display"
[ -n "$DISPLAY" ] && pass "DISPLAY=$DISPLAY" || fail "DISPLAY not set (seance needs X11 or XWayland; EGL itself is headless)"
echo "== seance"
if [ -x /opt/seance/bin/seance ]; then
  pass "installed at /opt/seance"
  missing=$(ldd /opt/seance/bin/seance 2>&1 | grep "not found"); [ -z "$missing" ] && pass "all shared libs resolve" || fail "unresolved libs: $missing"
  [ -f /opt/seance/share/terminfo/x/xterm-ghostty ] && pass "terminfo bundled" || fail "terminfo missing"
else
  fail "seance not installed (rpm -Uvh seance-*.rpm)"
fi
echo; echo "$ok ok, $bad failed"; exit $((bad>0))
