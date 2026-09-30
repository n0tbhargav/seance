# Séance

A GTK3 terminal for **SLES 15 SP4** (x86-64, glibc 2.31, GTK 3.24, **no GPU required**) built on the real
[Ghostty](https://github.com/ghostty-org/ghostty) terminal core (libghostty), with a scripting interface for agentic coding.

Séance is *not* a reimplementation: the Ghostty core (terminal emulation, fonts + shaping, config, themes, keybinds,
renderer) runs unmodified apart from a few small patches (`patches/`). It renders offscreen through Mesa's software
OpenGL (llvmpipe, GL 4.5 core on SP4's Mesa 21.2.4) and a thin GTK3 host paints the resulting frames. Ghostty's own
GTK4/libadwaita app can't run on SP4; this host can.

## What you get
- Everything Ghostty's core does: JetBrains Mono (bundled) with real ligatures, font fallback, Nerd Font glyphs, 617 bundled
  themes, truecolor, Kitty keyboard/graphics protocols, reflow, shell integration, `~/.config/ghostty/config.ghostty`
- Themed GTK chrome that follows your Ghostty theme (dark or light): tab strip with close buttons and a "+" button, middle-click to close,
  drag to reorder, right-click menu (New / Rename / Move / Close / Close Others), draggable themed split dividers, overlay scrollbar
- Tabs and splits (Ghostty keybinds: new_tab, new_split, goto_split, goto_tab, move_tab, resize_split, equalize_splits, toggle_split_zoom,
  reload_config, new_window ...), copy/paste, primary selection, IME/compose, HiDPI
- Translucent windows via `background-opacity` (needs a compositing window manager; falls back to opaque automatically without one)
- **Agent control**: a per-instance Unix socket and the `seancectl` CLI (below)
- Desktop notifications, clickable links, mouse cursor shapes, bell

## Install (SLES 15 SP4)
Prebuilt x86-64 binaries are on the [Releases page](https://github.com/n0tbhargav/seance/releases) (glibc 2.31 baseline).

**Tarball (no root needed; relocatable — extract anywhere):**
```sh
curl -LO https://github.com/n0tbhargav/seance/releases/download/v0.1.0/seance-0.1.0-linux-x86_64.tar.gz
mkdir -p ~/apps && tar xzf seance-0.1.0-linux-x86_64.tar.gz -C ~/apps
~/apps/seance/bin/seance
```
**RPM (system-wide: /opt/seance, /usr/bin/seance, /usr/bin/seancectl, menu entry):**
```sh
curl -LO https://github.com/n0tbhargav/seance/releases/download/v0.1.0/seance-0.1.0-1.x86_64.rpm
sudo rpm -Uvh seance-0.1.0-1.x86_64.rpm
```
Verify downloads against `SHA256SUMS` on the release page.

**Runtime dependencies** (from your distribution):
```sh
sudo zypper in libgtk-3-0 Mesa-libEGL1 Mesa-dri fontconfig
scripts/check-vm.sh        # preflight: glibc, GTK3, EGL, llvmpipe GL >= 4.3, DISPLAY
```
Software rendering is automatic on machines without a GPU; set `LIBGL_ALWAYS_SOFTWARE=1` to force it.
Needs an X11 display (or XWayland); EGL itself is headless.

> **Status: pre-release.** Tested in an SP4-equivalent container (openSUSE Leap 15.4, Xvfb) but **not yet on a real SLES 15 SP4 VM** —
> run `scripts/check-vm.sh` and please open an issue with what you find.

## Configuration
Ghostty's own config: `~/.config/ghostty/config.ghostty` (see `ghostty +show-config` docs upstream), e.g.
```
theme = Dracula
font-size = 13
font-family = "JetBrains Mono"
keybind = ctrl+shift+t=new_tab
keybind = ctrl+shift+o=new_split:right
keybind = ctrl+shift+e=new_split:down
keybind = ctrl+alt+h=goto_split:left
```
CLI: `seance [-e COMMAND...] [--size WxH] [--version]`. Live reload: bind `reload_config` (e.g. `keybind = ctrl+shift+r=reload_config`) and
edits to the config/theme apply without restarting; `new_window` spawns another instance; fullscreen/maximize actions work.
Env overrides: `SEANCE_SIZE=1200x800` (initial window), `SEANCE_CMD=...` (run a command instead of the shell),
`SEANCE_RESOURCES=...`, `SEANCE_STATS=1`.

## Agent control (`seancectl`)
Every instance exposes `$SEANCE_SOCKET` (mode 0600), and every program inside a pane gets `SEANCE_SOCKET` and `SEANCE_PANE`.
```sh
seancectl list                              # panes: id, tab, focus, size, title
seancectl exec  "make -j8 && echo BUILD_OK" # type a command and press Enter in the focused/own pane
seancectl wait  "BUILD_OK|Error" 300        # block until the screen matches a regex (exit 1 on timeout)
seancectl read                              # visible screen as text (add `scrollback` for everything)
seancectl key   ctrl-c                      # real key events: enter esc tab up down f5 alt-x ...
seancectl send  1 'text\n'                  # paste-style insert (no Enter), C escapes
seancectl split 1 right ; seancectl newtab ; seancectl focus 2 ; seancectl close 2
seancectl events                            # stream: command_finished (exit code + duration), bell, notification, title, focus, pane_*
```
`PANE` is an id or `focused` (default `$SEANCE_PANE`, else the focused pane). `command_finished` needs Ghostty's shell integration
(on by default for bash/zsh/fish).

## Persistent sessions (survive GUI / X / SSH loss)
Use Ghostty's `command` option with tmux — no extra daemon needed (verified: `test/persist.sh`; `kill -9` the GUI, restart, the session
and its output and background jobs are intact):
```
# ~/.config/ghostty/config.ghostty
command = tmux new-session -A -s main
```
Agents inside keep running; `seancectl` still works (it talks to the GUI; use tmux's own CLI for detached control).

## Build from source
Requires Zig 0.16 (builds the core), git, and Docker (SLES 15.4 build environment).
```sh
scripts/fetch-ghostty.sh        # clones the pinned Ghostty commit into ./ghostty and applies patches/
cd ghostty && zig build -Dtarget=x86_64-linux-gnu.2.31 -Dapp-runtime=none -Doptimize=ReleaseFast \
    -Demit-themes=true -Demit-terminfo=true --prefix ../full ; cd ..
ln -sf ghostty-internal.so full/lib/libghostty.so
# compile terminfo with a Linux tic (ncurses-devel, e.g. inside the Leap container):
#   rm -rf full/share/terminfo/{67,78}; tic -x -o full/share/terminfo full/share/terminfo/ghostty.terminfo
docker build --platform linux/amd64 -t seance-build:15.4 .
docker run --rm --platform linux/amd64 -v $PWD:/src -w /src/host seance-build:15.4 make install DESTDIR=/src/stage PREFIX=/opt/seance
# RPM: packaging/seance.spec (see test/rpm.sh); tarball: tar -C stage/opt -czf seance-VERSION-linux-x86_64.tar.gz seance
```
On a SLES 15 SP4 host, `make -C host` works directly with `gtk3-devel`, `gcc` and `glibc-devel` installed.

## Tests (Xvfb, Leap 15.4 = SP4 userland)
`test/*.sh` run inside `seance-test:15.4` / `seance-bench:15.4`: `host*.sh` (render, input, clipboard, config, themes, tabs/splits),
`agent.sh` (seancectl end to end), `alive.sh` (startup regression), `rpm.sh` (clean install as non-root).
Run shell-related tests as a non-root user (SUSE's ncurses ignores `$TERMINFO` for root).

## Layout
- `host/` GTK3 host (`host.c`) and `seancectl.c`; `packaging/` RPM spec; `patches/` Ghostty patches; `scripts/check-vm.sh`
- `src/main.c` — the earlier libghostty-vt + Cairo/Pango prototype, kept as a fallback / reference
- `PLAN.md` roadmap, `PROGRESS.md` engineering log (findings, gotchas, decisions)

## Status
Working and tested in an SP4-equivalent container (amd64 emulation). **Not yet validated on a real SLES 15 SP4 VM** — run
`scripts/check-vm.sh` first. Known gaps: native (non-tmux) session persistence, split zoom/resize actions, clipboard
permission prompts, multiple windows.

## License
Séance's code is MIT (`LICENSE`). Binary releases bundle Ghostty (MIT), JetBrains Mono (OFL 1.1), Symbols Nerd Font (MIT), the
iTerm2 color-scheme themes and other open-source libraries — see `NOTICE.md` and `licenses/`. Séance is an independent project,
not affiliated with the Ghostty project.
