#!/usr/bin/env bash
# Install the system packages needed to build qemu-system-riscv32.
# Debian/Ubuntu. Run once before scripts/build.sh.
set -euo pipefail

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

$SUDO apt-get update
$SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y \
    git build-essential pkg-config \
    ninja-build meson \
    libglib2.0-dev libpixman-1-dev libslirp-dev \
    flex bison python3 python3-venv zlib1g-dev

echo "deps installed:"
echo "  ninja  $(ninja --version 2>/dev/null)"
echo "  meson  $(meson --version 2>/dev/null)"
echo "  glib   $(pkg-config --modversion glib-2.0 2>/dev/null)"
echo "  pixman $(pkg-config --modversion pixman-1 2>/dev/null)"
echo "  slirp  $(pkg-config --modversion slirp 2>/dev/null)  (--enable-slirp -> -nic user)"
