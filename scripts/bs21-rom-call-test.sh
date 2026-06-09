#!/usr/bin/env bash
# Validate bs21_rom_call (patches/v10.0.0/0005): a guest call to a BS21 mask-ROM
# function is intercepted by the illegal-instruction hook, emulated in host C, and
# returns to ra. The test program calls memset_s @0x3d80c to fill a buffer with
# 'A', reads it back, and prints it over UART0 — so a correct run prints "XA"
# (X = the UART works, A = the byte bs21_rom_call's memset_s wrote).
#
# Needs riscv64-unknown-elf-{as,ld,objcopy}. Usage: bs21-rom-call-test.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_BIN="${QEMU_BIN:-$HERE/qemu/build/qemu-system-riscv32}"
AS=riscv64-unknown-elf-as; LD=riscv64-unknown-elf-ld
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

command -v "$AS" >/dev/null || { echo "SKIP: $AS not installed"; exit 0; }
[ -x "$QEMU_BIN" ] || { echo "FATAL: QEMU not built"; exit 2; }

cat > "$TMP/t.s" <<'ASM'
.section .text
.global _start
_start:
    li   t3, 0x52081004     /* UART0 DATA (offset 0x04) */
    li   t5, 0x58           /* 'X' — UART works */
    sb   t5, 0(t3)
    li   a0, 0x00100800     /* dst */
    li   a1, 16             /* dmax */
    li   a2, 0x41           /* 'A' */
    li   a3, 16             /* n */
    li   t0, 0x0003d80c     /* memset_s ROM addr -> bs21_rom_call */
    jalr ra, 0(t0)
    li   t1, 0x00100800
    lbu  t2, 0(t1)          /* 'A' iff memset_s ran */
    sb   t2, 0(t3)
    li   t4, 0x0a
    sb   t4, 0(t3)
1:  j    1b
ASM
"$AS" -march=rv32imac -mabi=ilp32 "$TMP/t.s" -o "$TMP/t.o"
printf 'SECTIONS { . = 0x100000; .text : { *(.text*) } }\nENTRY(_start)\n' > "$TMP/t.ld"
"$LD" -m elf32lriscv -T "$TMP/t.ld" "$TMP/t.o" -o "$TMP/t.elf"

timeout 3 "$QEMU_BIN" -M bs21 -nographic -serial file:"$TMP/out.txt" \
    -kernel "$TMP/t.elf" </dev/null >/dev/null 2>&1 || true

got="$(head -c 2 "$TMP/out.txt" 2>/dev/null)"
if [ "$got" = "XA" ]; then
    echo "BS21 ROM-CALL TEST: PASS (memset_s emulated via bs21_rom_call)"
    exit 0
fi
echo "BS21 ROM-CALL TEST: FAIL (serial='$(od -c "$TMP/out.txt" 2>/dev/null | head -1)')"
exit 1
