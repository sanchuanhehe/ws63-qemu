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

# 2b. apply the target/riscv patch (custom WS63 local-interrupt delivery: the
#     LOCIEN/LOCIPD CSRs + mcause 32-72 vectored delivery for IRQ>=32). Idempotent.
if ! grep -q "ws63_locipd" "$QEMU_DIR/target/riscv/cpu_helper.c"; then
    echo "==> applying patches/ws63-target-riscv.patch"
    git -C "$QEMU_DIR" apply "$HERE/patches/ws63-target-riscv.patch"
fi

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

# 5. configure (once)
if [ ! -f "$QEMU_DIR/build/build.ninja" ]; then
    echo "==> configuring (riscv32-softmmu only)"
    (cd "$QEMU_DIR" && ./configure --target-list=riscv32-softmmu \
        --disable-werror --disable-docs)
fi

# 6. build
echo "==> building qemu-system-riscv32 (-j$JOBS)"
(cd "$QEMU_DIR" && make -j"$JOBS" qemu-system-riscv32)

BIN="$QEMU_DIR/build/qemu-system-riscv32"
echo "==> done: $BIN"
"$BIN" -M help 2>/dev/null | grep -i ws63 || true
