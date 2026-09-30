# Séance — plan and milestones

GTK3 terminal on libghostty for SLES 15 SP4 x86-64 (glibc 2.31, GTK 3.24, no GPU on VMs),
built for agentic coding. Built to migrate cleanly to the official libghostty release.

## Status (updated 2026-09-30)
| Milestone | State |
|---|---|
| M0 prototype (libghostty-vt + Cairo) | done (src/main.c, kept as fallback) |
| M1 daily-driver features + benchmark | done |
| M2 architecture / font spike | **superseded**: real Ghostty core (fonts, shaping, themes) is used as-is |
| M3 Ghostty-grade text | **done via core** (ligatures, fallback, Nerd Font, themes verified) |
| M4 Ghostty-core track gate | **GO, done**: full core + Linux embedded platform patch + GTK3 host, software GL verified |
| M5 tabs / splits | done (zoom / resize-split / multi-window not yet) |
| M6 agent features | **partly done**: control socket + seancectl + events (command_finished, notification, bell, title). **Open: session persistence** |
| M7 packaging | RPM done + verified in clean container; needs validation on a real SP4 VM (scripts/check-vm.sh) |
| M8 migrate to official libghostty | pending upstream release; patches/ + host/ isolate the coupling |

### Next up (suggested order)
1. Validate on a real SP4 VM (check-vm.sh -> rpm -> seance); fix whatever real Mesa/X11 differences show.
2. Native session persistence (tmux-based persistence already works via `command = tmux new-session -A -s main`, verified in test/persist.sh). Optional seanced: PTY-owning daemon so shells/agents survive GUI or SSH loss; GUI reattaches. Options: (a) embed libghostty-vt in
   the daemon and stream snapshots (see snapshot.h), (b) simpler: run panes under a dtach/abduco-style PTY holder and let the core attach.
3. Host features: split zoom/resize/equalize, multiple windows, config-reload UI, clipboard permission prompt, search UI, scrollbar.
4. Perf: measure native llvmpipe frame time; consider dirty-rect/partial frame export if needed; optional zero-copy via shared memory.
5. Upstream-friendliness: propose the Linux embedded platform + memory-frame API upstream; keep patches small and rebased.

## Principles
1. **Pin, don't track.** One Ghostty commit as a submodule; upgrades are deliberate.
2. **Public API first.** Touch Ghostty internals only where no public API exists (fonts, renderer),
   and only behind our own interfaces (`vt_*` shim, `font_*`, `render_*`).
3. **Every milestone ships something runnable** in the SP4 container, with a test.
4. **CPU rendering is a first-class target**, GL is optional.
5. **Measure before optimizing:** vtebench parse-only vs total; compare with MATE Terminal / VTE.

## Test environments
- Build: `seance-build:15.4` (SUSE BCI 15.4, gcc 7, gtk3-devel).
- Runtime/GUI tests: `opensuse/leap:15.4` + Xvfb (BCI lacks Xvfb). Same userland lineage as SP4.
- Final acceptance: a real SP4 VM.

## Milestones

### M0 — Working prototype (current)
C app: GTK3 + Cairo/Pango + libghostty-vt static lib.
- Done: cross-built `libghostty-vt.a` (x86_64, glibc 2.31), app compiles clean, glibc symbols <= 2.10.
- Exit: Xvfb smoke test renders colored text and screenshots it (`test/smoke.sh`).

### M1 — Usable daily driver (C prototype hardened)
- Mouse reporting, selection + copy (Ctrl+Shift+C), primary selection, bracketed paste (done).
- Dirty-row rendering; scrollback; window resize; title; bell; TERM/COLORTERM.
- Config file (font, size, theme). Ghostty theme-format loader.
- Exit: runs vim, tmux, htop, less correctly; `vtebench` baseline recorded.

### M2 — Architecture split (de-risk the fork)
- Introduce shims: `vt_*` (libghostty), `font_*`, `render_*`, `ui_*`.
- Spike: build Ghostty `src/font` as a module for `x86_64-linux-gnu.2.31`; shape ligatures into an atlas.
- Decision gate: (a) import Ghostty font module, (b) C rewrite on FreeType+HarfBuzz+Fontconfig.
- Exit: atlas-rendered text through Cairo matches Pango output on a golden-image test.

### M3 — Ghostty-grade text
- Ligatures/OpenType features, font fallback, Nerd Font, sprite box-drawing/powerline, emoji,
  variable fonts, per-range font mapping. Static FreeType/HarfBuzz/Fontconfig (newer than SLES).
- Exit: golden-image tests for a text corpus; visual parity with Ghostty on same corpus.

### M4 — Ghostty-core track (decision gate: go/no-go)
Revised after M2 findings: the embedded C API (host draws UI, core owns terminal/fonts/renderer) is the shortest path to
"real Ghostty on GTK3"; only MACOS/IOS platforms exist today, so we add a Linux GL platform.
- Cross-build full libghostty (`-Dapp-runtime=none`) for glibc 2.31; verify it links in the SP4 container.
- Extend `apprt/embedded.zig` + OpenGL renderer with a Linux platform (GL make-current/present callbacks).
- GTK3 host: GtkGLArea (core 4.3+), input -> `ghostty_surface_key/mouse/text`, clipboard/actions callbacks, tabs/splits in host.
- Software GL (Mesa llvmpipe, GL 4.5 core verified on Mesa 21.2.4) is the no-GPU path; measure fps/latency on 1080p.
- Fallback if llvmpipe is too slow or the embedded extension is too invasive: P1 (Cairo + own font layer).
- Exit: fork renders a shell in GTK3 on llvmpipe; benchmark vs the P1 front-end; choose the mainline.

### M5 — Workspace features
- Tabs and splits (GtkNotebook/GtkPaned), keybindings, config reload, search, command palette.
- Exit: multi-pane sessions, keyboard-driven.

### M6 — Agent features (the differentiator)
- **seanced**: daemon that owns PTYs; GUI attach/detach; survives GUI/SSH loss.
- Control socket: send-keys, read-screen (via libghostty formatter), wait-for-output, list-panes.
- Shell-integration markers (OSC 133): command boundaries, exit status, "command finished" notifications.
- Clickable URLs and file:line, OSC 52 clipboard, Shift+Enter via Kitty keyboard protocol.
- Exit: an agent can drive a pane end to end via the socket, unattended.

### M7 — Packaging and release
- Static libghostty + bundled font libs; RPM for SLES 15 SP4 (+ SP3/SP5 smoke); desktop file, terminfo fallback.
- CI: container build + Xvfb tests + benchmarks.
- Exit: `zypper in ./seance.rpm` on a clean SP4 VM works.

### M8 — Migrate to official libghostty
- When the official release lands: bump pin, adapt the `vt_*` shim, drop any fork-only patches.

## Risks
| Risk | Mitigation |
|---|---|
| libghostty API churn | Pinned commit + shim; one file to adapt |
| Ghostty font module isn't standalone | M2 spike; fallback to C on FreeType/HarfBuzz |
| No GL 3.3 on VMs | CPU renderer is the default path |
| CPU renderer too slow on dense/Unicode output | Glyph atlas, dirty rows, benchmark early (M1) |
| Fork drifts from upstream | M4 gate; prefer module import over fork |
| GPL/LGPL contamination | Read VTE/Tilix for design only; Ghostty is MIT |
| Name collision (other "seance" projects) | Accepted; distinct binary/package naming in M7 |
