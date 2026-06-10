#!/usr/bin/env bash
# Smoke-test the minimal BS20 QEMU machine (-M bs20) against ws63-rs BS2X firmware.
#   - uart_hello: assert the banner prints over UART0 (custom UART @ 0x52081000)
#   - blinky:     assert the firmware reaches the GPIO0 toggle loop (MMIO trace)
#
# This is the milestone-1 acceptance for BS20: blinky + uart_hello boot end-to-end
# on the bs20 machine. Exit 0 = both pass.
#
# BS20 shares the BS2X peripheral bases + IRQ numbers with BS21E/BS22 but has a
# family shares platform_core.h peripheral bases + chip_core_irq.h IRQ numbers, and
# BS20's M1 memory map matches BS21E's 160K L2RAM layout). So the SAME bare-metal
# bs2x firmware drives this test — the examples/bs20 binaries (128K firmware) built with the
# `chip-bs21` (= bs2x family) HAL. There is deliberately no separate `chip-bs20`
# Rust build: it would be a byte-identical duplicate. A distinct firmware only
# becomes meaningful where the chips actually diverge (BS20's smaller 128K RAM, or
# each chip's vendor-C-SDK ROM/eFUSE/SFC bring-up), which is beyond M1.
#
# Build the firmware first (its own workspace, chip-bs21 HAL + BS2X memory.x):
#   cargo build --manifest-path examples/bs21/Cargo.toml --release
#
# Env: QEMU_DIR (default <repo>/qemu), WS63_RS (default ../ws63-rs)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_DIR="${QEMU_DIR:-$HERE/qemu}"
QEMU_BIN="${QEMU_BIN:-$QEMU_DIR/build/qemu-system-riscv32}"
# Umbrella checkout (renamed repo, or the older on-disk name).
WS63_RS="${WS63_RS:-}"
if [ -z "$WS63_RS" ]; then
    for c in "$HERE/../ws63-rs" "$HERE/../hisi-riscv-rs"; do
        [ -d "$c" ] && { WS63_RS="$c"; break; }
    done
    WS63_RS="${WS63_RS:-$HERE/../ws63-rs}"
fi
# Find the bs2x example target dir across both layouts (examples/bs21 | bs21-examples),
# preferring release over debug.
TARGET_DIR=""
for sub in examples/bs20 bs20-examples; do
    for prof in release debug; do
        d="$WS63_RS/$sub/target/riscv32imfc-unknown-none-elf/$prof"
        [ -f "$d/bs20_uart_hello" ] && { TARGET_DIR="$d"; break 2; }
    done
done
: "${TARGET_DIR:=$WS63_RS/examples/bs21/target/riscv32imfc-unknown-none-elf/release}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
[ -x "$QEMU_BIN" ] || { echo "FATAL: QEMU not built ($QEMU_BIN)"; exit 2; }
"$QEMU_BIN" -M help 2>/dev/null | grep -q '^bs20 ' || { echo "FATAL: this QEMU has no -M bs20 (rebuild: scripts/build.sh)"; exit 2; }

# ---- uart_hello: serial output ----
UART_ELF="$TARGET_DIR/bs20_uart_hello"
if [ -f "$UART_ELF" ]; then
    echo "==> bs2x uart_hello on -M bs20: expecting UART banner over UART0 @ 0x52081000"
    timeout 5 "$QEMU_BIN" -M bs20 -nographic -serial mon:stdio \
        -kernel "$UART_ELF" >"$TMP/uart.out" 2>/dev/null
    if grep -q "Hello from BS20 on QEMU" "$TMP/uart.out"; then
        echo "    PASS: $(grep -m1 Hello "$TMP/uart.out")"
    else
        echo "    FAIL: banner not found. Got:"; head -5 "$TMP/uart.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> bs2x uart_hello: SKIP (build it: cargo build --manifest-path examples/bs21/Cargo.toml --release)"
fi

# ---- blinky: GPIO toggle loop ----
BLINKY_ELF="$TARGET_DIR/bs20_blinky"
if [ -f "$BLINKY_ELF" ]; then
    echo "==> bs2x blinky on -M bs20: expecting GPIO0 (0x57010000) writes + no illegal-instruction traps"
    timeout 3 "$QEMU_BIN" -M bs20 -nographic -serial mon:stdio \
        -d int,unimp,guest_errors,trace:ws63_gpio_set,trace:ws63_gpio_clr \
        -D "$TMP/blinky.log" \
        -kernel "$BLINKY_ELF" >/dev/null 2>&1
    traps=$(grep -c illegal_instruction "$TMP/blinky.log" 2>/dev/null)
    gpio=$(grep -cE 'ws63_gpio_(set|clr) ' "$TMP/blinky.log" 2>/dev/null)
    toggled=$(grep -c 'ws63_gpio_set GPIO DATA_SET out=0x00000001' "$TMP/blinky.log" 2>/dev/null)
    traps=${traps:-0}; gpio=${gpio:-0}; toggled=${toggled:-0}
    if [ "$traps" -eq 0 ] && [ "$gpio" -gt 0 ] && [ "$toggled" -gt 0 ]; then
        echo "    PASS: $gpio GPIO toggles (pin0 high seen), 0 illegal-instruction traps"
    else
        echo "    FAIL: gpio_writes=$gpio illegal_traps=$traps"
        fail=1
    fi
else
    echo "==> bs2x blinky: SKIP (build it: cargo build --manifest-path examples/bs21/Cargo.toml --release)"
fi

[ "$fail" -eq 0 ] && echo "BS20 SMOKE TEST: PASS" || echo "BS20 SMOKE TEST: FAIL"
exit "$fail"
