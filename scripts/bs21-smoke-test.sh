#!/usr/bin/env bash
# Smoke-test the BS21 QEMU machine (-M bs21) against hisi-riscv-rs BS21 firmware.
#   - uart_hello: assert the banner prints over UART0 (custom UART @ 0x52081000)
#   - blinky:     assert the firmware reaches the GPIO0 toggle loop (MMIO trace)
# This is the milestone-1 acceptance: BS21 blinky + uart_hello boot end-to-end on
# the bs21 machine. Exit 0 = both pass.
#
# Build the firmware first (its own workspace, chip-bs21 HAL + BS21 memory.x):
#   cargo build --manifest-path hisi-riscv-rs/bs21-examples/Cargo.toml --release
#
# Env: QEMU_DIR (default <repo>/qemu), WS63_RS (default ../hisi-riscv-rs)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_DIR="${QEMU_DIR:-$HERE/qemu}"
QEMU_BIN="${QEMU_BIN:-$QEMU_DIR/build/qemu-system-riscv32}"
# Umbrella checkout (renamed repo, or the older on-disk name). The BS21 examples
# are their own (chip-bs21) workspace, grouped under examples/bs21 after the tree
# regroup (older layout had them at the top level as bs21-examples).
WS63_RS="${WS63_RS:-}"
if [ -z "$WS63_RS" ]; then
    for c in "$HERE/../hisi-riscv-rs" "$HERE/../ws63-rs"; do
        [ -d "$c" ] && { WS63_RS="$c"; break; }
    done
    WS63_RS="${WS63_RS:-$HERE/../hisi-riscv-rs}"
fi
# Find the example target dir across both layouts (examples/bs21 | bs21-examples),
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


# ---- spi_loopback: DesignWare-SSI v151 driver round-trip (loopback model @0x52087000) ----
SPI_ELF="$TARGET_DIR/bs21_spi_loopback"
if [ -f "$SPI_ELF" ]; then
    echo "==> bs21 spi_loopback on -M bs21: expecting SPI0 TX->RX loopback (chip-bs21 SPI driver)"
    timeout 5 "$QEMU_BIN" -M bs21 -nographic -serial mon:stdio \
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
    echo "==> bs21 gadc_read on -M bs21: expecting a GADC conversion (chip-bs21 gadc driver)"
    timeout 5 "$QEMU_BIN" -M bs21 -nographic -serial mon:stdio \
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


# ---- i2c_scan: BS2X DesignWare I2C (v151) driver bus scan (one slave @0x50) ----
I2C_ELF="$TARGET_DIR/bs21_i2c_scan"
if [ -f "$I2C_ELF" ]; then
    echo "==> bs21 i2c_scan on -M bs21: expecting a bus scan to find I2C0 slave 0x50"
    timeout 6 "$QEMU_BIN" -M bs21 -nographic -serial mon:stdio \
        -kernel "$I2C_ELF" >"$TMP/i2c.out" 2>/dev/null
    if grep -q "I2C scan OK" "$TMP/i2c.out"; then
        echo "    PASS: $(grep -m1 'found device' "$TMP/i2c.out")"
    else
        echo "    FAIL: I2C scan not OK. Got:"; grep -a I2C "$TMP/i2c.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> bs21 i2c_scan: SKIP"
fi


# ---- hid_demo: BS2X KEYSCAN + QDEC (v150) drivers ----
HID_ELF="$TARGET_DIR/bs21_hid_demo"
if [ -f "$HID_ELF" ]; then
    echo "==> bs21 hid_demo on -M bs21: expecting KEYSCAN key + QDEC count (chip-bs21 drivers)"
    timeout 5 "$QEMU_BIN" -M bs21 -nographic -serial mon:stdio \
        -kernel "$HID_ELF" >"$TMP/hid.out" 2>/dev/null
    if grep -q "HID demo OK" "$TMP/hid.out"; then
        echo "    PASS: $(grep -m1 'key:' "$TMP/hid.out") / $(grep -m1 'qdec count' "$TMP/hid.out")"
    else
        echo "    FAIL: HID demo not OK. Got:"; grep -aE 'key:|qdec|HID' "$TMP/hid.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> bs21 hid_demo: SKIP"
fi


# ---- clock_rng: BS2X RTC (v150) + TRNG (v1) drivers ----
CRNG_ELF="$TARGET_DIR/bs21_clock_rng"
if [ -f "$CRNG_ELF" ]; then
    echo "==> bs21 clock_rng on -M bs21: expecting RTC counter advance + TRNG randoms (chip-bs21)"
    timeout 5 "$QEMU_BIN" -M bs21 -nographic -serial mon:stdio \
        -kernel "$CRNG_ELF" >"$TMP/crng.out" 2>/dev/null
    if grep -q "RTC+TRNG OK" "$TMP/crng.out"; then
        echo "    PASS: $(grep -m1 'rtc c1' "$TMP/crng.out") / $(grep -m1 'trng r1' "$TMP/crng.out")"
    else
        echo "    FAIL: RTC+TRNG not OK. Got:"; grep -aE 'rtc|trng|RTC' "$TMP/crng.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> bs21 clock_rng: SKIP"
fi

[ "$fail" -eq 0 ] && echo "BS21 SMOKE TEST: PASS" || echo "BS21 SMOKE TEST: FAIL"
exit "$fail"
