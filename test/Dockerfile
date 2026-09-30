# GUI test image: Leap 15.4 (same userland lineage as SLES 15 SP4) + Xvfb + input/screenshot tools.
FROM opensuse/leap:15.4
RUN zypper -q --non-interactive install --no-recommends xvfb-run xorg-x11-server-extra xdotool ImageMagick xwd ncurses-devel \
      gtk3-tools dejavu-fonts xclip vim less tmux python3 gcc make && zypper clean -a
# Runtime for the Ghostty-core host: Mesa EGL + llvmpipe (software GL 4.5 core on Mesa 21.2.4)
RUN zypper -q --non-interactive install --no-recommends Mesa-libEGL1 Mesa-libGL1 Mesa-dri Mesa-libglapi0 libgbm1 Mesa-demo-x fontconfig && zypper clean -a
