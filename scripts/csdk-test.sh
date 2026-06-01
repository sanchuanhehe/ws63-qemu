#!/usr/bin/env bash
# Boot prebuilt fbb_ws63 C SDK peripheral-sample ELFs on ws63-qemu and assert each
# sample's UART output marker. This validates the WS63 peripheral models against
# REAL vendor-compiled firmware (complementing scripts/smoke-test.sh, which runs
# the ws63-rs Rust firmware).
#
# Hermetic + CI-friendly: needs only the built qemu and the committed fixtures in
# tests/csdk/ — no fbb_ws63 checkout or vendor toolchain. Regenerate fixtures with
# scripts/build-csdk-samples.sh.
#
# Env:
#   QEMU_DIR  (default <repo>/qemu)   location of the built QEMU
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_DIR="${QEMU_DIR:-$HERE/qemu}"
QEMU_BIN="${QEMU_BIN:-$QEMU_DIR/build/qemu-system-riscv32}"
FIX="$HERE/tests/csdk"
MANIFEST="$FIX/manifest.txt"

[ -x "$QEMU_BIN" ] || { echo "QEMU not built: $QEMU_BIN (run scripts/build.sh)" >&2; exit 1; }
[ -f "$MANIFEST" ] || { echo "manifest not found: $MANIFEST" >&2; exit 1; }

pass=0; fail=0; skip=0
# Read the manifest on FD 3 (and run QEMU with stdin from /dev/null) so QEMU's
# `-serial mon:stdio` cannot consume the manifest off the loop's stdin.
while IFS='|' read -r elf to marker <&3; do
    case "${elf// /}" in ''|\#*) continue;; esac
    elf="$(echo "$elf"   | xargs)"
    to="$(echo "$to"     | xargs)"
    marker="$(echo "$marker" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    path="$FIX/$elf"
    if [ ! -f "$path" ]; then
        echo "==> $elf: SKIP (fixture missing)"; skip=$((skip + 1)); continue
    fi
    echo "==> $elf: expecting /$marker/"
    out="$(timeout "$to" "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
           -kernel "$path" </dev/null 2>/dev/null || true)"
    if printf '%s' "$out" | grep -qaE "$marker"; then
        echo "    PASS"; pass=$((pass + 1))
    else
        echo "    FAIL — marker not seen (last lines:)"
        printf '%s\n' "$out" | grep -aE '[[:print:]]' | tail -3 | sed 's/^/      /'
        fail=$((fail + 1))
    fi
done 3< "$MANIFEST"

# --- NV / partition overlay check ----------------------------------------------
# Boot a sample with the flash overlay (tests/csdk/flash/) and assert the C SDK's
# partition + NV reads now succeed: the partition table resolves (so the
# "[UPG] ...flash_start_addr fail" seen on an empty flash is GONE) and the firmware
# still boots to the scheduler.
FLASHMAN="$FIX/flash/manifest.txt"
nvelf="$FIX/tcxo.elf"
if [ -f "$FLASHMAN" ] && [ -f "$nvelf" ]; then
    overlay=()
    while IFS='|' read -r f a; do
        case "${f// /}" in ''|\#*) continue;; esac
        f="$(echo "$f" | xargs)"; a="$(echo "$a" | xargs)"
        [ -f "$FIX/flash/$f" ] && overlay+=(-device "loader,file=$FIX/flash/$f,addr=$a")
    done < "$FLASHMAN"
    echo "==> NV overlay: expecting partition table to resolve (no UPG flash-addr fail)"
    out="$(timeout 16 "$QEMU_BIN" -M ws63 -nographic -serial mon:stdio \
           "${overlay[@]}" -kernel "$nvelf" </dev/null 2>/dev/null || true)"
    if printf '%s' "$out" | grep -qa 'entering scheduler' \
       && ! printf '%s' "$out" | grep -qa 'upg_get_upgrade_flag_flash_start_addr fail'; then
        echo "    PASS"; pass=$((pass + 1))
    else
        echo "    FAIL — partition/NV overlay not effective"; fail=$((fail + 1))
    fi
fi

echo "C SDK SAMPLE TESTS: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
