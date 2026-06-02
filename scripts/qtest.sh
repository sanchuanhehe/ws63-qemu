#!/usr/bin/env bash
# Run the WS63 register-level qtest (tests/qtest/ws63-test).
#
# Boot-free regression: drives the GPIO/UART/timer/INTC/DMA models directly over
# MMIO, no firmware. Builds the test binary first (idempotent) then runs it.
# Complements scripts/smoke-test.sh (which boots real firmware end-to-end).
#
# Env: QEMU_DIR (default <repo>/qemu)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_DIR="${QEMU_DIR:-$HERE/qemu}"
QEMU_BIN="${QEMU_BIN:-$QEMU_DIR/build/qemu-system-riscv32}"
TEST_BIN="$QEMU_DIR/build/tests/qtest/ws63-test"

[ -x "$QEMU_BIN" ] || { echo "FATAL: QEMU not built ($QEMU_BIN) — run scripts/build.sh" >&2; exit 2; }

# Build the qtest binary if absent or stale (ninja is a no-op when up to date).
echo "==> building tests/qtest/ws63-test"
(cd "$QEMU_DIR/build" && ninja tests/qtest/ws63-test)

echo "==> running ws63-test (boot-free register-level regression)"
QTEST_QEMU_BINARY="$QEMU_BIN" "$TEST_BIN" --tap -k
