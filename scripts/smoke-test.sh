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
        -d int,unimp,guest_errors,trace:ws63_gpio_set,trace:ws63_gpio_clr \
        -D "$TMP/blinky.log" \
        -kernel "$BLINKY_ELF" >/dev/null 2>&1
    traps=$(grep -c illegal_instruction "$TMP/blinky.log" 2>/dev/null)
    # GPIO is a real device; output changes now emit proper trace events
    # (ws63_gpio_set / ws63_gpio_clr), enabled above via -d trace:.
    gpio=$(grep -cE 'ws63_gpio_(set|clr) ' "$TMP/blinky.log" 2>/dev/null)
    toggled=$(grep -c 'ws63_gpio_set GPIO DATA_SET out=0x00000001' "$TMP/blinky.log" 2>/dev/null)
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

# ---- wifi_blob_link: link the vendor Wi-Fi ROM blob + resolve externals ----
BLOB_ELF="$TARGET_DIR/wifi_blob_link"
if [ -f "$BLOB_ELF" ]; then
    echo "==> wifi_blob_link: expecting libwifi_rom_data.a to link + relocate"
    timeout 8 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$BLOB_ELF" </dev/null >"$TMP/blob.out" 2>/dev/null || true
    if grep -q "BLOB LINK SPIKE: PASS" "$TMP/blob.out"; then
        echo "    PASS: $(grep -m1 g_mem_start "$TMP/blob.out" | sed 's/^[[:space:]]*//')"
    else
        echo "    FAIL: blob link/reloc not confirmed. Got:"; tail -5 "$TMP/blob.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> wifi_blob_link: SKIP (build it: cargo build -p wifi_blob_link --release)"
fi

# ---- rf_port_demo: ws63-rf-rs porting layer + blob linked through it ----
RFP_ELF="$TARGET_DIR/rf_port_demo"
if [ -f "$RFP_ELF" ]; then
    echo "==> rf_port_demo: expecting ws63-rf-rs porting fns + blob link to pass"
    timeout 8 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$RFP_ELF" </dev/null >"$TMP/rfp.out" 2>/dev/null || true
    if grep -q "RF PORT DEMO: PASS" "$TMP/rfp.out"; then
        echo "    PASS: $(grep -m1 'log sink' "$TMP/rfp.out" | sed 's/^[[:space:]]*//')"
    else
        echo "    FAIL: porting layer not confirmed. Got:"; tail -6 "$TMP/rfp.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> rf_port_demo: SKIP (build it: cargo build -p rf_port_demo --release)"
fi

# ---- ws63-rf-rs scheduler self-test (internal example: context switch + sem) ----
SCHED_ELF="$TARGET_DIR/examples/sched_selftest"
if [ -f "$SCHED_ELF" ]; then
    echo "==> sched_selftest: expecting multitask context switch + semaphore to pass"
    timeout 8 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$SCHED_ELF" </dev/null >"$TMP/sched.out" 2>/dev/null || true
    if grep -q "SCHED SELFTEST: PASS" "$TMP/sched.out"; then
        echo "    PASS: $(grep -m1 'semaphore items' "$TMP/sched.out" | sed 's/^[[:space:]]*//')"
    else
        echo "    FAIL: scheduler not confirmed. Got:"; tail -6 "$TMP/sched.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> sched_selftest: SKIP (build it: cargo build -p ws63-rf-rs --example sched_selftest --release)"
fi

# ---- semihost_selftest: pass/fail via the QEMU exit code (no UART scraping) ----
SEMI_ELF="$TARGET_DIR/semihost_selftest"
if [ -f "$SEMI_ELF" ]; then
    echo "==> semihost_selftest: expecting QEMU to exit 0 via RISC-V semihosting SYS_EXIT"
    timeout 5 "$QEMU_BIN" -M ws63 -nographic -semihosting \
        -kernel "$SEMI_ELF" </dev/null >"$TMP/semi.out" 2>&1
    ec=$?
    if [ "$ec" -eq 0 ]; then
        echo "    PASS: exit code 0 ($(grep -m1 semihost_selftest "$TMP/semi.out" 2>/dev/null))"
    else
        echo "    FAIL: exit code $ec. Got:"; head -4 "$TMP/semi.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> semihost_selftest: SKIP (build it: cargo build -p semihost_selftest --release)"
fi

# ---- custom_memory: per-example memory.x override (ws63-rt bundled one disabled) ----
CUSTMEM_ELF="$TARGET_DIR/custom_memory"
if [ -f "$CUSTMEM_ELF" ]; then
    echo "==> custom_memory: expecting its OWN memory.x in effect (marker 0x00c0ffee)"
    timeout 4 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$CUSTMEM_ELF" </dev/null >"$TMP/custmem.out" 2>/dev/null || true
    if grep -q "OK (per-example memory.x in effect)" "$TMP/custmem.out"; then
        echo "    PASS: $(grep -m1 marker= "$TMP/custmem.out" | sed 's/^[[:space:]]*//')"
    else
        echo "    FAIL: per-example memory.x not confirmed. Got:"; head -4 "$TMP/custmem.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> custom_memory: SKIP (build it: cargo build -p custom_memory --release)"
fi

# ---- async_delay: embedded-hal-async DelayNs (TIMER IRQ -> waker -> block_on) ----
ASYNC_ELF="$TARGET_DIR/async_delay"
if [ -f "$ASYNC_ELF" ]; then
    echo "==> async_delay: expecting embedded-hal-async DelayNs to drive 5 timer-IRQ ticks"
    timeout 6 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$ASYNC_ELF" </dev/null >"$TMP/asyncd.out" 2>/dev/null || true
    if grep -q "ASYNC DELAY: PASS" "$TMP/asyncd.out"; then
        echo "    PASS: $(grep -c 'async tick #' "$TMP/asyncd.out") async ticks (IRQ->waker->block_on)"
    else
        echo "    FAIL: async delay not confirmed. Got:"; tail -5 "$TMP/asyncd.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> async_delay: SKIP (build it: cargo build -p async_delay --release)"
fi

# ---- embassy_multitask: embassy-executor + embassy-time on the WS63 time-driver ----
EMB_ELF="$TARGET_DIR/embassy_multitask"
if [ -f "$EMB_ELF" ]; then
    echo "==> embassy_multitask: expecting 2 embassy-time tasks (TCXO now() + TIMER alarm)"
    timeout 8 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$EMB_ELF" </dev/null >"$TMP/emb.out" 2>/dev/null || true
    if grep -q "EMBASSY MULTITASK: PASS" "$TMP/emb.out"; then
        echo "    PASS: $(grep -c '\[fast\] tick' "$TMP/emb.out") fast + $(grep -c '\[slow\] tick' "$TMP/emb.out") slow ticks (embassy-time scheduled)"
    else
        echo "    FAIL: embassy multitask not confirmed. Got:"; tail -5 "$TMP/emb.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> embassy_multitask: SKIP (build it: cargo build -p embassy_multitask --release)"
fi

# ---- embassy_async_io: GPIO embedded-hal-async Wait + async UART under embassy ----
EMBIO_ELF="$TARGET_DIR/embassy_async_io"
if [ -f "$EMBIO_ELF" ]; then
    echo "==> embassy_async_io: expecting GPIO Wait + async UART under embassy"
    timeout 8 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$EMBIO_ELF" </dev/null >"$TMP/embio.out" 2>/dev/null || true
    if grep -q "EMBASSY ASYNC IO: PASS" "$TMP/embio.out"; then
        echo "    PASS: $(grep -c 'async rising edge' "$TMP/embio.out") async GPIO edges (Wait->IRQ->waker) + async UART"
    else
        echo "    FAIL: embassy async-IO not confirmed. Got:"; tail -5 "$TMP/embio.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> embassy_async_io: SKIP (build it: cargo build -p embassy_async_io --release)"
fi

# ---- async_bus: embedded-hal-async SpiBus + I2c (loopback under block_on) ----
ABUS_ELF="$TARGET_DIR/async_bus"
if [ -f "$ABUS_ELF" ]; then
    echo "==> async_bus: expecting async SPI/I2c loopback (embedded-hal-async)"
    timeout 5 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$ABUS_ELF" </dev/null >"$TMP/abus.out" 2>/dev/null || true
    if grep -q "ASYNC BUS: PASS" "$TMP/abus.out"; then
        echo "    PASS: $(grep -m1 'spi.transfer' "$TMP/abus.out" | sed 's/^[[:space:]]*//')"
    else
        echo "    FAIL: async bus not confirmed. Got:"; tail -4 "$TMP/abus.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> async_bus: SKIP (build it: cargo build -p async_bus --release)"
fi

# ---- spi_loopback: blocking SPI0 full-duplex round-trip (two-stage clock) ----
SPIL_ELF="$TARGET_DIR/spi_loopback"
if [ -f "$SPIL_ELF" ]; then
    echo "==> spi_loopback: expecting blocking SPI0 transfer() loopback to round-trip"
    timeout 8 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$SPIL_ELF" </dev/null >"$TMP/spil.out" 2>/dev/null || true
    if grep -q "SPI loopback OK" "$TMP/spil.out"; then
        echo "    PASS: SPI0 full-duplex loopback round-trips (CLDO_CRG clock writes absorbed)"
    else
        echo "    FAIL: loopback not confirmed. Got:"; tail -4 "$TMP/spil.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> spi_loopback: SKIP (build it: cargo build -p spi_loopback --release)"
fi

# ---- i2c_scan: I2C0 bus scan (driver + NACK/timeout path) ----
I2CS_ELF="$TARGET_DIR/i2c_scan"
if [ -f "$I2CS_ELF" ]; then
    echo "==> i2c_scan: expecting the I2C0 address scan to run to completion"
    timeout 8 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
        -kernel "$I2CS_ELF" </dev/null >"$TMP/i2cs.out" 2>/dev/null || true
    if grep -qE "scan done|no devices acked" "$TMP/i2cs.out"; then
        echo "    PASS: scan completed ($(grep -c 'found device' "$TMP/i2cs.out") addrs acked by the QEMU model)"
    else
        echo "    FAIL: scan did not complete. Got:"; tail -4 "$TMP/i2cs.out" | sed 's/^/      /'
        fail=1
    fi
else
    echo "==> i2c_scan: SKIP (build it: cargo build -p i2c_scan --release)"
fi

[ "$fail" -eq 0 ] && echo "SMOKE TEST: PASS" || echo "SMOKE TEST: FAIL"
exit "$fail"
