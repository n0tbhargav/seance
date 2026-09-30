# SLES 15 SP4 build environment (SUSE BCI base). Compiles against the same
# glibc/GTK3 the target VMs have.
FROM registry.suse.com/bci/bci-base:15.4
RUN zypper --non-interactive install --no-recommends gcc make pkg-config gtk3-devel glibc-devel && zypper clean -a
WORKDIR /src
