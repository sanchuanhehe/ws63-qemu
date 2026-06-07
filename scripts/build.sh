#!/usr/bin/env bash
# Build qemu-system-riscv32 with the WS63 machine model.
#
# Clones a pinned QEMU release, copies the WS63 source files (ws63.c, the xlinx
# decoder .inc, the qtest) and applies the per-version WS63 patch-series
# (patches/<QEMU_TAG>/[0-9]*.patch — the edits to existing QEMU files), then builds
# ONLY the riscv32-softmmu target. A QEMU version is supported iff patches/<tag>/
# exists. Idempotent: re-running re-copies the sources and re-builds incrementally.
#
# Env overrides:
#   QEMU_TAG   (default v10.0.0)  QEMU release tag to pin
#   QEMU_DIR   (default <repo>/qemu)
#   QEMU_REPO  (default gitlab.com/qemu-project/qemu)
#   JOBS       (default nproc)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_TAG="${QEMU_TAG:-v10.0.0}"
QEMU_REPO="${QEMU_REPO:-https://gitlab.com/qemu-project/qemu.git}"
QEMU_DIR="${QEMU_DIR:-$HERE/qemu}"
JOBS="${JOBS:-$(nproc)}"

# The patch-series is maintained per QEMU version under patches/<QEMU_TAG>/ (the
# edits to existing QEMU files drift between releases). Pick the dir for this tag;
# a missing dir means "this QEMU version isn't ported yet" — fail loudly with the
# set that IS supported.
PATCH_DIR="$HERE/patches/$QEMU_TAG"
if [ ! -d "$PATCH_DIR" ]; then
    echo "FATAL: no WS63 patch-series for $QEMU_TAG (patches/$QEMU_TAG/ missing)." >&2
    echo "       supported QEMU versions: $(cd "$HERE/patches" && echo v*)" >&2
    echo "       to port: build against $QEMU_TAG, rebase the series, add patches/$QEMU_TAG/." >&2
    exit 2
fi

# 1. clone (shallow) if absent
if [ ! -d "$QEMU_DIR/.git" ]; then
    echo "==> cloning QEMU $QEMU_TAG into $QEMU_DIR"
    git clone --depth 1 --branch "$QEMU_TAG" "$QEMU_REPO" "$QEMU_DIR"
fi

# 2. copy the WS63 source files that are NEW to the QEMU tree. These don't exist
#    upstream, so a copy never conflicts; they stay editable under src/ (the
#    authoring copy) rather than being carried as patches. ws63.c is the machine,
#    trans_xlinx.c.inc the custom-ISA decoder (#included by translate.c via the
#    series), ws63-test.c the boot-free register-level qtest.
echo "==> copying WS63 + BS21 sources (machines, shared models, xlinx decoder, qtest)"
cp "$HERE/src/hw/riscv/ws63.c"                            "$QEMU_DIR/hw/riscv/ws63.c"
# Shared HiSilicon riscv31 device-model declarations + the BS21 machine. bs21.c
# reuses ws63.c's device models via hisi_riscv31.h (CONFIG_BS21 selects CONFIG_WS63).
cp "$HERE/src/hw/riscv/hisi_riscv31.h"                    "$QEMU_DIR/hw/riscv/hisi_riscv31.h"
cp "$HERE/src/hw/riscv/bs21.c"                            "$QEMU_DIR/hw/riscv/bs21.c"
cp "$HERE/src/target/riscv/insn_trans/trans_xlinx.c.inc" "$QEMU_DIR/target/riscv/insn_trans/trans_xlinx.c.inc"
cp "$HERE/src/tests/qtest/ws63-test.c"                    "$QEMU_DIR/tests/qtest/ws63-test.c"

# 3. apply the WS63 patch-series for this QEMU version — the EDITS to existing
#    files, as a proper git-am-able series (replaces the old single patch + the
#    sed/cat injections):
#      0001 target/riscv: CPU type, local interrupts, xlinx decode, ROM interception
#      0002 hw/riscv: register the machine (meson source set, Kconfig, trace-events)
#      0003 tests/qtest: register ws63-test (qtests_riscv32)
#      000N (version-specific) e.g. adapt the copied ws63.c to an older QEMU API
#    Idempotent: skip if already applied (cached/incremental qemu/ tree).
if ! grep -q "ws63_locipd" "$QEMU_DIR/target/riscv/cpu_helper.c"; then
    echo "==> applying WS63 patch-series (patches/$QEMU_TAG/[0-9]*.patch)"
    for p in "$PATCH_DIR"/[0-9]*.patch; do
        echo "    git apply $(basename "$p")"
        git -C "$QEMU_DIR" apply "$p"
    done
fi

# 5. configure (once). --enable-slirp gives `-netdev user` (SLIRP NAT) for the
#    Wi-Fi/Ethernet MAC model (needs libslirp-dev installed).
if [ ! -f "$QEMU_DIR/build/build.ninja" ]; then
    echo "==> configuring (riscv32-softmmu only, slirp on)"
    (cd "$QEMU_DIR" && ./configure --target-list=riscv32-softmmu \
        --disable-werror --disable-docs --enable-slirp)
fi

# 6. build the emulator, then the WS63 qtest binary (ninja picks up the
#    meson.build edit above and regenerates before building the target).
echo "==> building qemu-system-riscv32 (-j$JOBS)"
(cd "$QEMU_DIR" && make -j"$JOBS" qemu-system-riscv32)
echo "==> building tests/qtest/ws63-test (-j$JOBS)"
(cd "$QEMU_DIR/build" && ninja -j"$JOBS" tests/qtest/ws63-test)

BIN="$QEMU_DIR/build/qemu-system-riscv32"
echo "==> done: $BIN"
"$BIN" -M help 2>/dev/null | grep -i ws63 || true
