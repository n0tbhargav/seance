# Séance — progress checkpoint

Read this first when resuming. Plan: PLAN.md. Memory: seance-terminal-project.

## Now
- **M1 DONE** (usable daily-driver prototype; all tests in test/ pass). Baseline (bench/run.sh, 10MB `cat`, median of 3, amd64 emulation,
  relative only): seance ascii 60.8 / sgr 50.0 / unicode 48.8 MB/s; lxterminal(VTE) 10.7 / 12.9 / 10.7; xterm 2.8 / 0.3 / 3.3.
  Caveat: partly reflects throttled drawing during floods (VTE does this too).
- Starting **M2**: architecture split (vt_/font_/render_ shims) + Ghostty font-module spike (decision gate C vs Zig vs C-on-FreeType/HarfBuzz).

## Status log
- Mouse support written in src/main.c (drag select, Ctrl+Shift+C copy, PRIMARY on release, middle-click paste,
  mouse reporting via libghostty encoder, wheel). Compiles clean. NOT yet verified: run `test/mouse.sh` in
  `seance-test:15.4` once the image build (bnj1utmwc) finishes; expected PRIMARY/CLIPBOARD = "bravo".
- Language: C for M0-M1 (Zig only as libghostty build tool). Revisit at M2 gate: Zig if importing Ghostty font
  module / forking; Rust not planned.

- **Mouse VERIFIED** (test/mouse.sh): drag-select + Ctrl+Shift+C -> PRIMARY and CLIPBOARD both "bravo".
- **Language decision (user-approved direction):** Zig for the terminal front-end/core, Rust for `seanced`
  (daemon + control socket + `seance ctl` CLI), C only for the M0-M1 prototype. Daemon should embed libghostty-vt
  (screen state, snapshot.h for GUI reattach). gtk3-rs is officially unmaintained -> no Rust GTK.

- **Mouse reporting VERIFIED** (test/report.sh; SGR click at cell (5,3) -> ESC[<0;6;4M/m). Bug fixed: encoder size wasn't set on first allocation.
- **Config + themes + zoom VERIFIED** (test/config.sh): ~/.config/seance/config (Ghostty key=value; keys: font-family,
  font-size, theme, foreground, background, cursor-color, palette=N=#hex, scrollback-lines); themes searched in
  ~/.config/{seance,ghostty}/themes, /usr/share/{seance,ghostty}/themes; Ctrl+plus/minus/0 zoom; opaque block cursor.

- **M1 word/line select VERIFIED** (test/dblclick.sh). Real apps checked visually: vim + tmux split render fine; UTF-8 ok;
  CJK shows tofu (DejaVu only) -> needs M3 font fallback.
- **M2 research:** Ghostty `src/font` is NOT a standalone module: it imports ../config.zig, ../terminal, ../renderer,
  ../global, ../quirks, ../unicode, ../os, ../datastruct via relative paths; build.zig only exports `ghostty-vt`.
  Feasible route = vendored/patched Ghostty tree + a tiny `src/seance_font.zig` root that re-exports `font` (+ C ABI),
  relying on Zig lazy analysis to skip unused code. Cost: dependence on internals (churn) -> keep behind `font_*` shim.

- **Dirty-row rendering DONE + VERIFIED** (persistent back buffer; test/incremental.sh: partial vs full redraw = 0 differing
  pixels after a `less` scrolling session; SEANCE_FULL_REDRAW=1 debug switch). All earlier tests still pass.
- Benchmark running in background (task bn06v4w5h, `bench/run.sh`, seance vs xterm vs lxterminal/VTE, `cat` of 10MB ascii/sgr/unicode);
  results -> out/bench.txt. Relative numbers only (amd64 emulation).

- First benchmark run was NOT trustworthy (seance sgr faster than ascii; xterm lacked fonts). Reran with 3 reps, interleaved,
  xterm font fixed (task bdqwwu7ru, medians in out/bench.txt). Don't run other Docker/Zig jobs while it runs (skews timing).

- **Perf finding (SEANCE_STATS=1, bench/stats.sh):** libghostty parse is NOT the bottleneck (10MB ASCII in 0.03s vt_write even
  under emulation); drawing was (20ms/frame). Fixed with: adaptive draw scheduling (>=3x last frame cost), 1MB drain per
  wake-up, batched same-style ASCII text runs (one Pango call per run; hint-metrics ON so advances are integer px).
  10MB ascii wall 2.05s -> 0.78s; draw time 1.25s -> 0.03s. Alignment verified (test/align.sh: '|' centered after 99 cols).
  Earlier benchmark (pre-fix): seance beat xterm everywhere, beat VTE on unicode, lost to VTE on ascii (4.7 vs 10.7 MB/s).
  Final post-fix benchmark running: task btnuvytav -> out/bench.txt.

- **M2/M4 findings (important):**
  1. Embedded C API (`include/ghostty.h`, what the macOS Swift app uses) = host does UI (tabs/splits/windows), core owns
     terminal+fonts+renderer+config+keybinds. But platforms are only MACOS/IOS and the OpenGL renderer has no embedded-runtime
     path on Linux (OpenGL.zig threadEnter takes apprt.Surface). => Candidate route **P2**: extend embedded apprt with a Linux GL
     platform (host supplies GL context via GtkGLArea callbacks) + GTK3 host app. Far smaller than porting the ~24k-line GTK4 apprt.
  2. Software GL is viable: Leap 15.4 (SP4-era) Mesa 21.2.4 llvmpipe reports OpenGL **4.5 core** (Ghostty needs 4.3). Speed unmeasured.
  3. Trying: full libghostty build `zig build -Dtarget=x86_64-linux-gnu.2.31 -Dapp-runtime=none` -> ../full (task bzifr0c7w).
  Routes now: P1 = keep Cairo/Pango app + own FreeType/HarfBuzz font layer (C); P2 = Ghostty core via embedded+GL, GTK3 host.

- **KEY ARCHITECTURE FINDING (route P2 is much cheaper than thought):** current Ghostty renders OFFSCREEN in the core via its own
  EGL device (EGL_PLATFORM_SURFACELESS_MESA, llvmpipe when no GPU): `renderer/opengl/Device.zig`. Finished frames = `ExportedFrame`
  = dmabuf OR `.memory` (premultiplied RGBA8, tightly packed) pushed to a single-slot latest-wins queue (`renderer.pushFrame`) and
  the render thread posts `.redraw` to the surface mailbox; apprt then calls `core.renderer.takeFrame()` on the main thread
  (GTK4 example: apprt/gtk/class/render_surface.zig:157). apprt sends `.presentation_health` to the renderer; "unhealthy" => CPU
  memory frames instead of dmabuf. => GTK3 host needs NO GL: take memory frame -> cairo ARGB32 surface -> paint.
  Plan P2: extend apprt/embedded.zig with a Linux platform + C API `ghostty_surface_take_frame()/release`, redraw callback; GTK3 host
  (C or Zig) does UI (tabs/splits/menus/input) like the macOS Swift app.
- Full libghostty (`-Dapp-runtime=none`) cross-built 119/125 steps for glibc 2.31 (FreeType/HarfBuzz/Fontconfig/glslang/
  spirv-cross/simdutf/highway/wuffs all fine). Only failure: pkg/opengl/egl.zig `eglGetPlatformDisplay` id type (c_int without X11
  headers). Patch: `id: ?*anyopaque` in egl.zig + `null` in Device.zig (kept as git diff in ghostty/ clone). Rebuild task be67ljfw4.

- **Full core builds for target (with 2-line EGL patch):** ghostty/../full/lib/ghostty-internal.{a,so} (160MB .a w/ debug; .so 29MB).
  `.so` NEEDED only libm/libc/ld-linux/libpthread/libdl/librt; max glibc symbol 2.29 (< SP4's 2.31). EGL/GL is dlopen'd at
  runtime (needs Mesa EGL + llvmpipe on the VM); fontconfig is static but needs /etc/fonts at runtime.

- **M4 SPIKE SUCCESS (2026-09-30):** the REAL Ghostty core (1.3.2-main) runs inside a GTK3 host on the SP4-era userland (Leap 15.4,
  Mesa 21.2.4 llvmpipe, OpenGL 4.5 loaded): default JetBrains Mono font + Ghostty default theme rendered offscreen via surfaceless EGL,
  CPU frames -> `ghostty_surface_take_frame` -> Cairo blit; keyboard input + shell work (test/host.sh, out/host.png).
  Ghostty patches (reproducible): `patches/0001-linux-embedded-gl-frames.patch` on base commit in `patches/ghostty-base-commit.txt`:
  egl.zig/Device.zig (EGL display type), embedded.zig (Linux platform, take/release frame, unhealthy presentation), ghostty.h,
  SharedDeps.zig (glad compiled into lib for Linux/none). Host: host/host.c (build: gcc ... -Lfull/lib -l:ghostty-internal.so; needs
  symlink full/lib/libghostty.so -> ghostty-internal.so). Runtime deps on the VM: Mesa EGL + dri (llvmpipe), fontconfig conf.
  TODO: perf on llvmpipe (frame time @1000x640 & 1080p, flood throughput), mouse/scroll/selection/clipboard tests, ligatures/CJK,
  tabs/splits in host, static-ish packaging, cursor visibility check.

- **Ghostty-core host perf (llvmpipe, amd64 emulation, relative):** flood `cat` 10MB: ascii 47.9 / sgr 51.1 / unicode 50.6 MB/s
  (C app: 60/50/49; VTE 11-13). Host-side per-frame copy+swizzle+flip ~1.2-1.8 ms. Bug fixed: default-priority wakeup ticks starved GTK
  redraws (0 frames during flood) -> ticks now g_idle_add_full(G_PRIORITY_DEFAULT_IDLE) + coalesced; now 4-5 frames presented during a
  0.2s flood. Tests: test/host.sh, test/hostframes.sh, test/hostflood.sh.

- **Ghostty-native features VERIFIED through the GTK3 host (test/host2.sh, out/host2.png):** ~/.config/ghostty/config.ghostty + themes dir
  (custom theme colors + palette applied), font-size, REAL JetBrains Mono ligatures (=> != -> <= >= === www), double-click word select ->
  PRIMARY ("bravo") via write_clipboard_cb. => **M4 gate: GO** with P2 (Ghostty core + GTK3 host). C Cairo app stays as fallback.
  Remaining risks: real SLES SP4 VM validation (Mesa EGL/llvmpipe pkgs, native llvmpipe speed), resources dir (terminfo/shell integration/
  themes), packaging (static link), tabs/splits in host, GUI-less/ssh-X-forward cases (EGL surfaceless works without X but host needs X).

- Static link of core into host FAILS with gcc (archive needs Zig's bundled libc++: std::__1::*; also needs explicit -lEGL for eglQueryString).
  Decision: ship host + shared core (`libghostty.so`, libc++ inside) with rpath; link host with -lEGL. (Could revisit via `zig cc`.)

- **Host upgraded & VERIFIED (test/host.sh host2.sh host3.sh):** HiDPI scale, IME/compose (GtkIMMulticontext), mouse cursor shapes, open URL,
  bell, desktop notification (notify-send), quit action; paste (Ctrl+Shift+V via read_clipboard_cb), copy (Ctrl+Shift+C), wheel scrollback
  (direction correct). Host links with -lEGL.

- **Resources VERIFIED (test/host4.sh):** build.zig patched to emit resources on Linux lib build (`-Demit-themes=true -Demit-terminfo=true`);
  host finds them (GHOSTTY_RESOURCES_DIR = <exe>/../share/ghostty or dev ../full/share/ghostty); Dracula theme applied exactly (#282A36);
  shell integration active (cursor,path,title); TERM=xterm-ghostty. Terminfo must be compiled with Linux `tic` (in `ncurses-devel` on SUSE;
  macOS tic writes hex dirs 78/xterm-ghostty which Linux ncurses ignores). NOTE: SUSE ncurses ignores $TERMINFO for ROOT (container artifact):
  as a normal user `TERMINFO=... tput colors` = 256. Run GUI tests as non-root where it matters.

- **RPM DONE + VERIFIED (test/rpm.sh):** `dist/x86_64/seance-0.1.0-1.x86_64.rpm` (packaging/seance.spec, host/Makefile `make install`):
  /opt/seance/{bin/seance, lib/libghostty.so, share/{ghostty(themes+shell-integration), terminfo}}, /usr/bin/seance symlink; runs as non-root:
  TERM=xterm-ghostty, tput colors=256, resources found via exe path. Bug fixed: stage files were mode 600 (spec now normalizes modes).
  `scripts/check-vm.sh` = preflight for the user's real SP4 VM (glibc, GTK3, EGL, Mesa llvmpipe GL>=4.3, DISPLAY, libs resolve).

- **Tabs + splits DONE + VERIFIED (host rewritten per-pane; test/host5.sh, host8.sh):** Ghostty keybinds -> actions: new_tab, new_split
  (right/down/left/up, GtkPaned), goto_split (prev/next/directional), goto_tab, close (child exit closes pane/tab; last tab quits),
  per-pane titles/cursors/clipboard/IME, explicit focus tracking (works without a WM). Tab bar auto-hides with one tab.
  **Prompt-stacking after resize was a ROOT-IN-CONTAINER artifact** (SUSE ncurses ignores $TERMINFO for root => bash saw TERM=xterm-ghostty
  with no terminfo => dumb redraw). As a non-root user (test/host8.sh) resize/splits are perfectly clean. RULE: run shell-related GUI
  tests as a non-root user. (A winch-reprinting program also redraws fine in-place, confirming core/host resize handling is correct.)

- **Agent control layer DONE + VERIFIED (test/agent.sh):** host runs a Unix control socket ($SEANCE_SOCKET; default in $XDG_RUNTIME_DIR or ~/.cache,
  mode 0600) + `seancectl` CLI (host/seancectl.c): list, read [scrollback], send (paste-style), exec (send+Enter), key (enter/esc/ctrl-c/...),
  wait <regex> [secs], focus, split, newtab, close, events (stream). Events: command_finished (exit code + duration; needs shell integration),
  bell, notification, title, focus, pane_opened/closed. Verified end-to-end as non-root: exec+wait, read, ctrl-c interrupt (exit=130 event),
  split + exec in new pane. PANE arg = id or `focused` (default $SEANCE_PANE else focused).
- **CRASH ROOT-CAUSED + FIXED:** `ghostty_init` captures a slice of the process `environ` block (pointer+len, main_c.zig); any later
  `setenv()` (I called g_setenv("SEANCE_SOCKET") after init) can realloc environ -> dangling slice -> SIGSEGV in
  `Surface.defaultTermioEnv -> global.environMap -> Environ.putPosixBlock` at ghostty_surface_new (size-of-environment dependent, hence
  "flaky"; hid under -O0/Debug core). RULE: do ALL setenv before ghostty_init and none after. Found via a `-Dstrip=false` core build +
  the host's SIGSEGV backtrace handler (+ addr2line). Per-pane env vars (SEANCE_PANE/SEANCE_SOCKET) work again and are default-on.
  test/alive.sh (host survives 15s as non-root) is the regression test for this.

- **Software-GL perf at full HD (test/hostflood.sh, SEANCE_SIZE=1900x1000, amd64 emulation):** 10MB flood ascii/sgr/unicode = 49/40/42 MB/s,
  4-5 frames presented per 0.2s flood, host copy+swizzle 3.0-4.6 ms/frame (1000x640: 1.3-2.0 ms). README/PLAN refreshed; RPM rebuilt with seancectl.

- **Real-app/protocol suite PASSES 11/11 (test/apps2.sh, via seancectl, non-root):** vim, tmux (incl. split), less, Kitty keyboard protocol
  (Shift+Enter -> CSI 13;2u), bracketed paste markers, OSC 52 clipboard write. Fixes made on the way: `seancectl key` now supports shifted
  chars/punctuation (`:` `%` `A`...), `exec` types text verbatim (only `send` interprets escapes).
- **Persistence works today via tmux** (test/persist.sh): `command = tmux new-session -A -s main`; GUI kill -9 + restart reattaches, jobs survive.
  (Fragile-as-root note: host2/host5 screenshots taken as root show bash readline redraw artifacts; not host bugs.)

- **Polish DONE + VERIFIED (test/host9.sh):** CLI `seance [-e CMD...] [--size WxH] [--version|--help]`; live `reload_config` (bind a key; theme swap
  verified); `new_window` (spawns another instance); fullscreen/maximize actions; .desktop launcher + `seancectl` symlink in the RPM.
  **Lesson (regression caught by apps2):** the core sends *soft* `reload_config` whenever an app queries the color scheme (vim did); swapping
  configs inside the action callback crashed. Soft reloads are declined; hard reloads run from an idle callback. Rule: never do heavy/
  re-entrant core calls (config swap, surface free) synchronously inside action_cb — defer to an idle handler.
- **Final regression (all pass):** alive, agent, apps2 (11/11), host9, persist, rpm.

- **GUI pass (in progress):** theme-driven GTK chrome from the Ghostty config (bg/fg/palette[4] accent; light+dark verified via test/gui1.sh):
  custom tab labels (title + × close button), "+" new-tab button, middle-click close, right-click menu (New/Rename/Move/Close/Close Others),
  drag-reorder (notebook reorderable), keyboard focus follows tab switches (`switch-page` -> idle set_focus), min tab width, wide split handle
  with themed 1px divider, overlay scrollbar thumb (ACTION_SCROLLBAR), split resize/equalize/zoom + MOVE_TAB/SET_TAB_TITLE/PROMPT_TITLE actions,
  translucency (RGBA visual + `background-opacity`; OPERATOR_SOURCE paint when a compositor exists, flatten over theme bg otherwise; UNTESTED
  under a real compositor). test/gui2.sh clicks the real widgets (switch, +, ×, middle-click, drag) — all PASS before the crash below.
- **CRASH #3 root-caused (intermittent SIGSEGV on 2nd+ surface creation):** NULL call inside libEGL_mesa. Race: new surface's renderer starts with
  presentation_health=healthy and may try a DMABUF export (unsupported by llvmpipe -> NULL hook) before the host's reportPresentationHealth(.unhealthy)
  lands. Fix = core patch: default presentation_health to .unhealthy for Linux+embedded (renderer/generic.zig; in patches/0001). Found via NULL-rip
  handler that prints the return address from the stack (crash handler now uses sigaltstack + siginfo/ucontext). Also fixed: shutdown crash
  (free surfaces before ghostty_app_free), pane_of_surface() no longer dereferences possibly-freed surfaces, callbacks validate Pane* (live_pane).
  NOTE: the black popup menu in Xvfb screenshots is an xwd/ARGB artifact (menu works; host survives); verify menus with a WM.

- **GUI pass DONE + VERIFIED on the release build:** gui2 (real mouse: switch/+/×/middle-click/drag) 6/6, gui3 (resize/equalize/zoom/move_tab) 5/5, apps2 11/11,
  host9, persist, agent, alive, rpm all pass; RPM rebuilt (patched core). No-compositor fallback renders exactly the theme color (config loaded with
  `background-opacity = 1` override when no compositor is detected). Translucency with a compositor: code path engages (rgba visual + composited detected
  under xcompmgr) but Xvfb's 32-bit visual has no usable alpha for xcompmgr (window shown as premultiplied-over-black), so real-compositor blending is
  UNVERIFIED — check on a real desktop with `background-opacity = 0.85`.
  Menus: xwd shows popup menus as black boxes (ARGB artifact); functionally fine.
- **Core patch #2 (in patches/0001):** presentation_health defaults to .unhealthy on Linux+embedded (fixes NULL-call crash in Mesa EGL on 2nd+ surface).
  Build note: rebuilding the core with -Demit-terminfo overwrites full/share/terminfo with macOS hex dirs -> re-run Linux `tic` (see README/PROGRESS).

- **v0.1.1 pass (user Q&A driven; committed locally, NOT yet pushed/released):**
  glyph `⏵` (U+23F5) = missing-font issue (no default SUSE font has it; `noto-sans-symbols2-fonts` fixes it, Séance's fontconfig fallback picks it up
  automatically — verified); default keybinds work (Ctrl+Shift+T/O/E verified; an earlier "not working" was my test window being shorter than the Xvfb pointer);
  app icon (original SVG + 48/64/128/256 PNGs; window icon list, WM_CLASS=seance, RPM symlinks into hicolor + icon-cache scriptlets, desktop Icon=seance);
  `seance --install-desktop/--uninstall-desktop` (custom paths, spaces OK, passes desktop-file-validate); `--shell-hook tcsh`;
  tcsh integration hook (packaging/shell/seance.tcsh: OSC 133 A/B/C/D, OSC 7 cwd, OSC 2 title, preserves SUSE's precmd via save+source) verified with real tcsh
  (exit codes, durations, titles, new tab in same cwd); right-click context menu + ☰ hamburger menu (About/Open Config/Reload/Fullscreen/Quit) verified via keyboard nav;
  `window-show-tab-bar` honored; docs ("Using Séance" in README: keys, menus, icon, custom path, fonts, tcsh, SSH/TERM). gui2 coordinates updated for the ☰ button.
  Tests added: glyph.sh keys.sh menus.sh tcsh.sh custompath.sh. Ghostty has NO tcsh integration upstream (bash/zsh/fish/elvish/nushell only).

## Next
1. Real-app tests: vim, tmux, less, htop-like (test/apps.sh); double/triple-click word/line select.
2. Redraw efficiency; vtebench baseline vs. xterm/VTE if available.
4. Then M2 spike (Ghostty src/font as module).

## Autonomy rules
- Before each step check `~/.claude/usage-state.json`; at >=95% 5h usage, update this file and run
  `~/.claude/wait-for-reset.sh` in background, then resume here.
- Long builds/tests: `run_in_background`, no polling.

## Environment notes
- Zig 0.16 (brew). Static lib: `cd ghostty && zig build -Dtarget=x86_64-linux-gnu.2.31 -Doptimize=ReleaseFast --prefix ../vt`
  (ignore the GTK4/blueprint failure for the full app; `vt/lib/libghostty-vt.a` is still produced).
- Build app: `docker run --rm --platform linux/amd64 -v $PWD:/src seance-build:15.4 make`
- SUSE BCI has no Xvfb; use `opensuse/leap:15.4` for GUI tests.
