#!/bin/bash
# Fetch the pinned Ghostty commit into ./ghostty and apply Séance's patches.
set -e
cd "$(dirname "$0")/.."
BASE=$(cat patches/ghostty-base-commit.txt)
if [ ! -d ghostty/.git ]; then git clone --filter=blob:none https://github.com/ghostty-org/ghostty.git ghostty; fi
git -C ghostty fetch --quiet origin "$BASE" 2>/dev/null || git -C ghostty fetch --quiet origin
git -C ghostty checkout --quiet "$BASE"
git -C ghostty checkout --quiet -- .          # drop any previous patch state
git -C ghostty apply --whitespace=nowarn ../patches/0001-linux-embedded-gl-frames.patch
echo "Ghostty $BASE patched. Build the core with:"
echo "  cd ghostty && zig build -Dtarget=x86_64-linux-gnu.2.31 -Dapp-runtime=none -Doptimize=ReleaseFast \\"
echo "      -Demit-themes=true -Demit-terminfo=true --prefix ../full"
echo "Then recompile terminfo with a Linux tic (ncurses-devel): tic -x -o full/share/terminfo full/share/terminfo/ghostty.terminfo"
