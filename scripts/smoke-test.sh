#!/usr/bin/env bash
# Smoke-test the WS63 QEMU machine against ws63-rs firmware.
#   - uart_hello: assert the banner prints over UART0 (custom UART device)
#   - blinky:     assert the firmware reaches the GPIO0 toggle loop (MMIO trace)
# Exit 0 = both pass.
#
# Env: QEMU_DIR (default <repo>/qemu), WS63_RS (default ../ws63-rs)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_DIR="${QEMU_DIR:-$HERE/qemu}"
QEMU_BIN="${QEMU_BIN:-$QEMU_DIR/build/qemu-system-riscv32}"
WS63_RS="${WS63_RS:-$HERE/../ws63-rs}"
TARGET_DIR="$WS63_RS/target/riscv32imfc-unknown-none-elf/release"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
[ -x "$QEMU_BIN" ] || { echo "FATAL: QEMU not built ($QEMU_BIN)"; exit 2; }

# ---- timer_irq: interrupt delivery ----
TIMER_ELF="$TARGET_DIR/timer_irq"
if [ -f "$TIMER_ELF" ]; then
    echo "==> timer_irq: expecting TIMER_0 interrupts (IRQ 26) to be delivered"
    timeout 5 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$TIMER_ELF" >"$TMP/timer.out" 2>/dev/null
    if grep -q "OK: timer interrupts delivered" "$TMP/timer.out"; then
        echo "    PASS: $(grep -c 'timer irq #' "$TMP/timer.out") interrupts seen"
    else
        echo "    FAIL: interrupts not delivered. Got:"; head -4 "$TMP/timer.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> timer_irq: SKIP (build it: cargo build -p timer_irq --release)"
fi

# ---- gpio_irq: custom local interrupt (IRQ >= 32) delivery ----
GPIO_ELF="$TARGET_DIR/gpio_irq"
if [ -f "$GPIO_ELF" ]; then
    echo "==> gpio_irq: expecting GPIO0 IRQ 33 (custom local, >=32) to be delivered"
    timeout 5 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$GPIO_ELF" >"$TMP/gpio.out" 2>/dev/null
    if grep -q "custom local IRQ (>=32) delivered" "$TMP/gpio.out"; then
        echo "    PASS: $(grep -c 'gpio irq #' "$TMP/gpio.out") interrupts seen"
    else
        echo "    FAIL: >=32 IRQ not delivered. Got:"; head -4 "$TMP/gpio.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> gpio_irq: SKIP (build it: cargo build -p gpio_irq --release)"
fi

# ---- reset_demo: software_reset + reset_reason round-trip ----
RESET_ELF="$TARGET_DIR/reset_demo"
if [ -f "$RESET_ELF" ]; then
    echo "==> reset_demo: expecting software_reset() to reboot + reset_reason()=Software"
    timeout 8 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$RESET_ELF" </dev/null >"$TMP/reset.out" 2>/dev/null || true
    boots=$(grep -c "WS63 system-reset test" "$TMP/reset.out" 2>/dev/null)
    boots=${boots:-0}
    if grep -q "OK: software reset observed" "$TMP/reset.out" && [ "$boots" -ge 2 ]; then
        echo "    PASS: rebooted ($boots boots), reset_reason=Software"
    else
        echo "    FAIL: reset round-trip not observed. Got:"; tail -4 "$TMP/reset.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> reset_demo: SKIP (build it: cargo build -p reset_demo --release)"
fi

# ---- uart_hello: serial output ----
UART_ELF="$TARGET_DIR/uart_hello"
if [ -f "$UART_ELF" ]; then
    echo "==> uart_hello: expecting UART banner"
    timeout 5 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$UART_ELF" >"$TMP/uart.out" 2>/dev/null
    if grep -q "Hello from WS63 on QEMU" "$TMP/uart.out"; then
        echo "    PASS: $(grep -m1 Hello "$TMP/uart.out")"
    else
        echo "    FAIL: banner not found. Got:"; head -5 "$TMP/uart.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> uart_hello: SKIP (build it: cargo build -p uart_hello --release)"
fi

# ---- blinky: GPIO toggle loop ----
BLINKY_ELF="$TARGET_DIR/blinky"
if [ -f "$BLINKY_ELF" ]; then
    echo "==> blinky: expecting GPIO0 (0x44028xxx) writes + no illegal-instruction traps"
    timeout 3 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -d int,unimp,guest_errors -D "$TMP/blinky.log" \
        -kernel "$BLINKY_ELF" >/dev/null 2>&1
    traps=$(grep -c illegal_instruction "$TMP/blinky.log" 2>/dev/null)
    # GPIO is a real device now; it logs output changes via qemu_log (-d on).
    gpio=$(grep -c 'ws63-gpio: \(SET\|CLR\)' "$TMP/blinky.log" 2>/dev/null)
    toggled=$(grep -c 'ws63-gpio: SET -> out=0x00000001' "$TMP/blinky.log" 2>/dev/null)
    traps=${traps:-0}
    gpio=${gpio:-0}
    toggled=${toggled:-0}
    if [ "$traps" -eq 0 ] && [ "$gpio" -gt 0 ] && [ "$toggled" -gt 0 ]; then
        echo "    PASS: $gpio GPIO toggles (pin0 high seen), 0 illegal-instruction traps"
    else
        echo "    FAIL: gpio_writes=$gpio illegal_traps=$traps"
        fail=1
    fi
else
    echo "==> blinky: SKIP (build it: cargo build -p blinky --release)"
fi

# ---- dma_loopback: peripheral DMA (mem<->SPI handshaking) + SDMA channel ----
DMA_ELF="$TARGET_DIR/dma_loopback"
if [ -f "$DMA_ELF" ]; then
    echo "==> dma_loopback: expecting mem<->SPI0 peripheral DMA + SDMA ch8 to pass"
    timeout 8 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$DMA_ELF" </dev/null >"$TMP/dma.out" 2>/dev/null || true
    if grep -q "DMA LOOPBACK TEST: PASS" "$TMP/dma.out"; then
        echo "    PASS: $(grep -m1 part1 "$TMP/dma.out" | sed 's/^[[:space:]]*//')"
    else
        echo "    FAIL: loopback not confirmed. Got:"; tail -5 "$TMP/dma.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> dma_loopback: SKIP (build it: cargo build -p dma_loopback --release)"
fi

[ "$fail" -eq 0 ] && echo "SMOKE TEST: PASS" || echo "SMOKE TEST: FAIL"
exit "$fail"
