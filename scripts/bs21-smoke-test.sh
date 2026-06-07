#!/usr/bin/env bash
# Smoke-test the BS21 QEMU machine (-M bs21) against ws63-rs BS21 firmware.
#   - uart_hello: assert the banner prints over UART0 (custom UART @ 0x52081000)
#   - blinky:     assert the firmware reaches the GPIO0 toggle loop (MMIO trace)
# This is the milestone-1 acceptance: BS21 blinky + uart_hello boot end-to-end on
# the bs21 machine. Exit 0 = both pass.
#
# Build the firmware first (its own workspace, chip-bs21 HAL + BS21 memory.x):
#   cargo build --manifest-path ws63-rs/bs21-examples/Cargo.toml --release
#
# Env: QEMU_DIR (default <repo>/qemu), WS63_RS (default ../ws63-rs)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_DIR="${QEMU_DIR:-$HERE/qemu}"
QEMU_BIN="${QEMU_BIN:-$QEMU_DIR/build/qemu-system-riscv32}"
WS63_RS="${WS63_RS:-$HERE/../ws63-rs}"
# BS21 examples are their own (chip-bs21) workspace; prefer release, fall back to debug.
BASE="$WS63_RS/bs21-examples/target/riscv32imfc-unknown-none-elf"
TARGET_DIR="$BASE/release"
[ -d "$TARGET_DIR" ] || TARGET_DIR="$BASE/debug"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
[ -x "$QEMU_BIN" ] || { echo "FATAL: QEMU not built ($QEMU_BIN)"; exit 2; }
"$QEMU_BIN" -M help 2>/dev/null | grep -q '^bs21 ' || { echo "FATAL: this QEMU has no -M bs21 (rebuild: scripts/build.sh)"; exit 2; }

# ---- uart_hello: serial output ----
UART_ELF="$TARGET_DIR/bs21_uart_hello"
if [ -f "$UART_ELF" ]; then
    echo "==> bs21_uart_hello: expecting UART banner over UART0 @ 0x52081000"
    timeout 5 "$QEMU_BIN" -M bs21 -nographic -serial mon:stdio \
        -kernel "$UART_ELF" >"$TMP/uart.out" 2>/dev/null
    if grep -q "Hello from BS21 on QEMU" "$TMP/uart.out"; then
        echo "    PASS: $(grep -m1 Hello "$TMP/uart.out")"
    else
        echo "    FAIL: banner not found. Got:"; head -5 "$TMP/uart.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> bs21_uart_hello: SKIP (build it: cargo build --manifest-path bs21-examples/Cargo.toml --release)"
fi

# ---- blinky: GPIO toggle loop ----
BLINKY_ELF="$TARGET_DIR/bs21_blinky"
if [ -f "$BLINKY_ELF" ]; then
    echo "==> bs21_blinky: expecting GPIO0 (0x57010000) writes + no illegal-instruction traps"
    timeout 3 "$QEMU_BIN" -M bs21 -nographic -serial mon:stdio \
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
    echo "==> bs21_blinky: SKIP (build it: cargo build --manifest-path bs21-examples/Cargo.toml --release)"
fi

[ "$fail" -eq 0 ] && echo "BS21 SMOKE TEST: PASS" || echo "BS21 SMOKE TEST: FAIL"
exit "$fail"
