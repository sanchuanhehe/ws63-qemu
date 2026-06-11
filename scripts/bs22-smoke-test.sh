#!/usr/bin/env bash
# Smoke-test the minimal BS22 QEMU machine (-M bs22) against ws63-rs BS2X firmware.
#   - uart_hello: assert the banner prints over UART0 (custom UART @ 0x52081000)
#   - blinky:     assert the firmware reaches the GPIO0 toggle loop (MMIO trace)
#
# This is the milestone-1 acceptance for BS22: blinky + uart_hello boot end-to-end
# on the bs22 machine. Exit 0 = both pass.
#
# BS22 is register- and memory-identical to BS21E at the M1 level (the whole BS2X
# family shares platform_core.h peripheral bases + chip_core_irq.h IRQ numbers, and
# BS22's M1 memory map matches BS21E's 160K L2RAM layout). So the SAME bare-metal
# bs2x firmware drives this test — the examples/bs21 binaries built with the
# `chip-bs21` (= bs2x family) HAL. There is deliberately no separate `chip-bs22`
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
for sub in examples/bs21 bs21-examples; do
    for prof in release debug; do
        d="$WS63_RS/$sub/target/riscv32imfc-unknown-none-elf/$prof"
        [ -f "$d/bs21_uart_hello" ] && { TARGET_DIR="$d"; break 2; }
    done
done
: "${TARGET_DIR:=$WS63_RS/examples/bs21/target/riscv32imfc-unknown-none-elf/release}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
[ -x "$QEMU_BIN" ] || { echo "FATAL: QEMU not built ($QEMU_BIN)"; exit 2; }
"$QEMU_BIN" -M help 2>/dev/null | grep -q '^bs22 ' || { echo "FATAL: this QEMU has no -M bs22 (rebuild: scripts/build.sh)"; exit 2; }

# ---- uart_hello: serial output ----
UART_ELF="$TARGET_DIR/bs21_uart_hello"
if [ -f "$UART_ELF" ]; then
    echo "==> bs2x uart_hello on -M bs22: expecting UART banner over UART0 @ 0x52081000"
    timeout 5 "$QEMU_BIN" -M bs22 -nographic -serial mon:stdio \
        -kernel "$UART_ELF" >"$TMP/uart.out" 2>/dev/null
    if grep -q "Hello from BS21 on QEMU" "$TMP/uart.out"; then
        echo "    PASS: $(grep -m1 Hello "$TMP/uart.out")"
    else
        echo "    FAIL: banner not found. Got:"; head -5 "$TMP/uart.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> bs2x uart_hello: SKIP (build it: cargo build --manifest-path examples/bs21/Cargo.toml --release)"
fi

# ---- blinky: GPIO toggle loop ----
BLINKY_ELF="$TARGET_DIR/bs21_blinky"
if [ -f "$BLINKY_ELF" ]; then
    echo "==> bs2x blinky on -M bs22: expecting GPIO0 (0x57010000) writes + no illegal-instruction traps"
    timeout 3 "$QEMU_BIN" -M bs22 -nographic -serial mon:stdio \
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


# ---- spi_loopback: DesignWare-SSI v151 driver round-trip (loopback model @0x52087000) ----
SPI_ELF="$TARGET_DIR/bs21_spi_loopback"
if [ -f "$SPI_ELF" ]; then
    echo "==> bs21 spi_loopback on -M bs22: expecting SPI0 TX->RX loopback (chip-bs21 SPI driver)"
    timeout 5 "$QEMU_BIN" -M bs22 -nographic -serial mon:stdio \
        -kernel "$SPI_ELF" >"$TMP/spi.out" 2>/dev/null
    if grep -q "SPI loopback OK" "$TMP/spi.out"; then
        echo "    PASS: $(grep -m1 'SPI loopback' "$TMP/spi.out")"
    else
        echo "    FAIL: SPI loopback not OK. Got:"; grep -a SPI "$TMP/spi.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> bs21 spi_loopback: SKIP (build examples/bs21/Cargo.toml --release)"
fi


# ---- gadc_read: BS2X 13-bit ADC (v153) driver conversion (GADC model @0x57036000) ----
GADC_ELF="$TARGET_DIR/bs21_gadc_read"
if [ -f "$GADC_ELF" ]; then
    echo "==> bs21 gadc_read on -M bs22: expecting a GADC conversion (chip-bs21 gadc driver)"
    timeout 5 "$QEMU_BIN" -M bs22 -nographic -serial mon:stdio \
        -kernel "$GADC_ELF" >"$TMP/gadc.out" 2>/dev/null
    if grep -q "GADC read OK" "$TMP/gadc.out"; then
        echo "    PASS: $(grep -m1 'AIN0 raw' "$TMP/gadc.out")"
    else
        echo "    FAIL: GADC read not OK. Got:"; grep -a GADC "$TMP/gadc.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> bs21 gadc_read: SKIP"
fi

[ "$fail" -eq 0 ] && echo "BS22 SMOKE TEST: PASS" || echo "BS22 SMOKE TEST: FAIL"
exit "$fail"
