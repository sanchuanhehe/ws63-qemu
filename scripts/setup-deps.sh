#!/usr/bin/env bash
# Install the system packages needed to build qemu-system-riscv32 (the WS63/BS2X
# fork). Detects the host OS and uses the matching package manager:
#   Linux        -> apt-get   (Debian/Ubuntu; x86_64 + aarch64 — apt resolves arch)
#   macOS        -> brew      (Homebrew)
#   MSYS2/MINGW  -> pacman    (when run outside the setup-msys2 action)
# Run once before scripts/build.sh.
set -euo pipefail

OS="$(uname -s 2>/dev/null || echo unknown)"

case "$OS" in
  Linux)
    SUDO=""
    [ "$(id -u)" -ne 0 ] && SUDO="sudo"
    $SUDO apt-get update
    $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y \
        git build-essential pkg-config \
        ninja-build meson \
        libglib2.0-dev libpixman-1-dev libslirp-dev \
        flex bison python3 python3-venv zlib1g-dev \
        patchelf
    MGR="linux/apt"
    ;;

  Darwin)
    # ninja/pkgconf/python3 ship on the runner; meson/glib/pixman/libslirp do not.
    # dylibbundler is used by the release packaging step. (flex/bison/zlib: Apple's
    # system versions suffice — QEMU's macOS build doesn't need brew ones.)
    if ! command -v brew >/dev/null 2>&1; then
        echo "FATAL: Homebrew not found. Install from https://brew.sh first." >&2
        exit 1
    fi
    brew update
    brew install meson ninja pkgconf glib pixman libslirp dylibbundler
    MGR="macos/brew"
    ;;

  MINGW*|MSYS*|CYGWIN*)
    # Windows under MSYS2/MINGW64. In CI the msys2/setup-msys2 action already
    # installs these via pacboy/install; this branch covers a manual MSYS2 shell.
    if ! command -v pacman >/dev/null 2>&1; then
        echo "FATAL: pacman not found — run this inside an MSYS2 shell." >&2
        exit 1
    fi
    pacman -S --needed --noconfirm \
        base-devel git bison flex make diffutils zip \
        mingw-w64-x86_64-toolchain \
        mingw-w64-x86_64-ninja mingw-w64-x86_64-meson \
        mingw-w64-x86_64-python mingw-w64-x86_64-python-setuptools \
        mingw-w64-x86_64-pkgconf \
        mingw-w64-x86_64-glib2 mingw-w64-x86_64-pixman \
        mingw-w64-x86_64-libslirp mingw-w64-x86_64-zlib mingw-w64-x86_64-zstd
    MGR="windows/msys2-pacman"
    ;;

  *)
    echo "FATAL: unsupported OS '$OS'." >&2
    echo "       Supported: Linux (apt), macOS (brew), MSYS2/MINGW (pacman)." >&2
    echo "       Install equivalents of: git, a C toolchain, pkg-config, ninja," >&2
    echo "       meson, glib, pixman, libslirp, zlib (+flex/bison/python on Linux)." >&2
    exit 2
    ;;
esac

echo "deps installed ($MGR):"
echo "  ninja  $(ninja --version 2>/dev/null)"
echo "  meson  $(meson --version 2>/dev/null)"
echo "  glib   $(pkg-config --modversion glib-2.0 2>/dev/null)"
echo "  pixman $(pkg-config --modversion pixman-1 2>/dev/null)"
echo "  slirp  $(pkg-config --modversion slirp 2>/dev/null)  (--enable-slirp -> -netdev user)"
