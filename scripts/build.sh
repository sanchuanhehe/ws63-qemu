#!/usr/bin/env bash
# Build qemu-system-riscv32 with the WS63 machine model.
#
# Clones a pinned QEMU release, injects hw/riscv/ws63.c plus the minimal
# meson.build / Kconfig hooks, and builds ONLY the riscv32-softmmu target.
# Idempotent: re-running re-copies ws63.c and re-builds incrementally.
#
# Env overrides:
#   QEMU_TAG   (default v9.2.4)   QEMU release tag to pin
#   QEMU_DIR   (default <repo>/qemu)
#   QEMU_REPO  (default gitlab.com/qemu-project/qemu)
#   JOBS       (default nproc)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_TAG="${QEMU_TAG:-v9.2.4}"
QEMU_REPO="${QEMU_REPO:-https://gitlab.com/qemu-project/qemu.git}"
QEMU_DIR="${QEMU_DIR:-$HERE/qemu}"
JOBS="${JOBS:-$(nproc)}"

# 1. clone (shallow) if absent
if [ ! -d "$QEMU_DIR/.git" ]; then
    echo "==> cloning QEMU $QEMU_TAG into $QEMU_DIR"
    git clone --depth 1 --branch "$QEMU_TAG" "$QEMU_REPO" "$QEMU_DIR"
fi

# 2. inject the board source
echo "==> injecting hw/riscv/ws63.c"
cp "$HERE/src/hw/riscv/ws63.c" "$QEMU_DIR/hw/riscv/ws63.c"

# 2a. inject the HiSilicon "xlinx" custom-ISA decoder (included by translate.c via
#     the target/riscv patch below). Must be present before the build.
echo "==> injecting target/riscv/insn_trans/trans_xlinx.c.inc"
cp "$HERE/src/target/riscv/insn_trans/trans_xlinx.c.inc" \
   "$QEMU_DIR/target/riscv/insn_trans/trans_xlinx.c.inc"

# 2b. apply the target/riscv patch: custom WS63 local-interrupt delivery (LOCIEN/
#     LOCIPD CSRs + mcause 32-72 vectored delivery for IRQ>=32) AND the xlinx
#     custom-ISA decoder hooks in translate.c (insn_len/decode_opc). Idempotent.
if ! grep -q "ws63_locipd" "$QEMU_DIR/target/riscv/cpu_helper.c"; then
    echo "==> applying patches/ws63-target-riscv.patch"
    git -C "$QEMU_DIR" apply "$HERE/patches/ws63-target-riscv.patch"
fi

# 2c. inject the qtest (register-level, boot-free regression of the WS63 models)
echo "==> injecting tests/qtest/ws63-test.c"
cp "$HERE/src/tests/qtest/ws63-test.c" "$QEMU_DIR/tests/qtest/ws63-test.c"

# 3. register in the meson source set (before the 'hw_arch +=' line)
MB="$QEMU_DIR/hw/riscv/meson.build"
if ! grep -q "CONFIG_WS63" "$MB"; then
    echo "==> patching $MB"
    sed -i "/^hw_arch += {'riscv': riscv_ss}/i riscv_ss.add(when: 'CONFIG_WS63', if_true: files('ws63.c'))" "$MB"
fi

# 4. declare the Kconfig symbol (auto-enabled for riscv32)
KC="$QEMU_DIR/hw/riscv/Kconfig"
if ! grep -q "^config WS63" "$KC"; then
    echo "==> patching $KC"
    cat >> "$KC" <<'EOF'

config WS63
    bool
    default y
    depends on RISCV32
    select UNIMP
EOF
fi

# 4a. append the WS63 trace events to hw/riscv/trace-events (the dir already has
#     one for riscv-iommu; APPEND, never overwrite, or other riscv machines lose
#     their events). meson regenerates hw/riscv/trace.h from this. Idempotent.
TE="$QEMU_DIR/hw/riscv/trace-events"
if ! grep -q "ws63_gpio_set" "$TE"; then
    echo "==> appending WS63 trace events to $TE"
    cat >> "$TE" <<'EOF'

# --- HiSilicon WS63 SoC model (ws63.c) ---
ws63_gpio_set(uint32_t out) "GPIO DATA_SET out=0x%08x"
ws63_gpio_clr(uint32_t out) "GPIO DATA_CLR out=0x%08x"
ws63_dma_xfer(int ch, unsigned fc, unsigned sper, unsigned dper, uint32_t src, uint32_t dst, unsigned items, unsigned width) "DMA ch%d fc=%u sper=%u dper=%u src=0x%08x dst=0x%08x items=%u width=%u"
EOF
fi

# 4b. register the qtest in qtests_riscv32 (idempotent). Prepends 'ws63-test'
#     to the list so meson builds tests/qtest/ws63-test from the injected source.
QM="$QEMU_DIR/tests/qtest/meson.build"
if ! grep -q "ws63-test" "$QM"; then
    echo "==> registering ws63-test in $QM"
    sed -i "s#^qtests_riscv32 = \\\\#qtests_riscv32 = ['ws63-test'] + \\\\#" "$QM"
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
