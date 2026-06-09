#!/usr/bin/env bash
# Boot a HiSilicon BS21/BS2X *vendor* signed boot image on `-M bs21`.
#
# The fbb_bs2x prebuilt loaderboot (src/interim_binary/bs21e/bin/boot_bin/
# loaderboot-bs21e-1100e/loaderboot_sign.bin) is a multi-section signed image:
#
#   0x000  image header     (magic 0x4bd2f01e, len 0x40) + pad
#   0x100  code-info header (magic 0x4bd2f02d, len 0x40) + hash block
#          code size (u32) at file offset 0x120
#   tail   the actual code  (size bytes, ending at EOF)
#
# So code starts at (filesize - code_size). The mask ROM normally copies this to
# the ITCM and jumps to it; here we load the extracted code at the loaderboot run
# address (0x40000 = APP_ITCM_ORIGIN, "use itcm start addr load loaderboot") and
# set the reset PC there. loaderboot runs its init (~480 insns: relocates the
# boot-param block to the DTCM @0x20000000, brings up PMU @0x57004600) and reaches
# its interrupt-driven download-mode idle spin — all standard RV32 + xlinx, no
# illegal-instruction traps, proving BS21 vendor firmware executes on -M bs21.
#
# Remaining to go further (flashboot banner / full chain): a BS21 ROM-call table
# (the secure-libc + SFC functions the later stages call live in the ROM region;
# see docs/bs21-vendor-firmware.md), the flashboot image format (trailer-based,
# different from loaderboot), and SFC/mask-ROM stubs.
#
# Usage: bs21-vendor-boot.sh <loaderboot_sign.bin> [seconds] [load_addr]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_BIN="${QEMU_BIN:-$HERE/qemu/build/qemu-system-riscv32}"
IMG="${1:?usage: bs21-vendor-boot.sh <loaderboot_sign.bin> [seconds] [load_addr]}"
SECS="${2:-4}"
ADDR="${3:-0x40000}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

[ -x "$QEMU_BIN" ] || { echo "FATAL: QEMU not built ($QEMU_BIN)"; exit 2; }
"$QEMU_BIN" -M help 2>/dev/null | grep -q '^bs21 ' || { echo "FATAL: no -M bs21"; exit 2; }

# Extract the code section: locate the code-info header (magic 0x4bd2f02d); the
# code size is the u32 at code-info + 0x24, and the code sits at the file tail.
python3 - "$IMG" "$TMP/code.bin" <<'PY'
import sys, struct
d = open(sys.argv[1], 'rb').read()
ci = next((i for i in range(0, len(d) - 3, 4)
           if struct.unpack_from('<I', d, i)[0] == 0x4bd2f02d), None)
if ci is None:
    sys.exit("no code-info header (magic 0x4bd2f02d) — flashboot uses a "
             "different, trailer-based format (see docs/bs21-vendor-firmware.md)")
csize = struct.unpack_from('<I', d, ci + 0x24)[0]
if not (0 < csize <= len(d)):
    sys.exit(f"bad code size 0x{csize:x}")
open(sys.argv[2], 'wb').write(d[len(d) - csize:])
print(f"code-info @0x{ci:x}, code 0x{csize:x} bytes @ file 0x{len(d)-csize:x}")
PY
[ -s "$TMP/code.bin" ] || exit 1

echo "==> booting $(basename "$IMG") at $ADDR on -M bs21 (${SECS}s)"
timeout "$SECS" "$QEMU_BIN" -M bs21 -nographic -serial mon:stdio \
    -d in_asm,unimp,guest_errors -D "$TMP/trace.log" \
    -device loader,file="$TMP/code.bin",addr="$ADDR" \
    -device loader,addr="$ADDR",cpu-num=0 </dev/null >"$TMP/uart.out" 2>&1 || true

insns=$(grep -cE '^0x' "$TMP/trace.log" 2>/dev/null); insns=${insns:-0}
illegal=$(grep -c 'illegal' "$TMP/trace.log" 2>/dev/null); illegal=${illegal:-0}
echo "    UART:  $(grep -v terminating "$TMP/uart.out" | head -3 | tr '\n' ' ')"
echo "    insns executed: $insns   illegal-at-pc!=0: see trace"
echo "    last PC: $(grep -E '^0x' "$TMP/trace.log" | tail -1)"
if [ "$insns" -gt 100 ]; then
    echo "    => RUNS (vendor firmware executes on -M bs21)"
else
    echo "    => stalled early (insns=$insns) — check load addr / image format"
fi
