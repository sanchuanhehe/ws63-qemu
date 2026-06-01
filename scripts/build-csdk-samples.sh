#!/usr/bin/env bash
# Regenerate the fbb_ws63 C SDK peripheral-sample test fixtures (tests/csdk/*.elf).
#
# Each fixture is a ws63-liteos-app image built with exactly one peripheral sample
# selected (CONFIG_ENABLE_PERIPHERAL_SAMPLE=y + CONFIG_SAMPLE_SUPPORT_<X>=y). The
# resulting ELF boots LiteOS and runs that sample's task, which prints a known
# marker over UART0 — that is what scripts/csdk-test.sh asserts on ws63-qemu.
#
# This is a LOCAL / manual tool (needs the fbb_ws63-qemu checkout + the vendor
# riscv32 toolchain); it is NOT run in CI. CI consumes the committed ELFs instead.
#
# Usage:
#   FBB=/path/to/fbb_ws63-qemu scripts/build-csdk-samples.sh [sample...]
#   (no args => the default sample set below)
#
# Env:
#   FBB     fbb_ws63(-qemu) checkout (must contain src/build.py)   [required]
#   STRIP   riscv32 strip binary (default: auto-detect in the SDK toolchain)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIX="$HERE/tests/csdk"
FBB="${FBB:?set FBB to the fbb_ws63(-qemu) checkout}"
SRC="$FBB/src"
APP_CFG="$SRC/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config"
ELF_OUT="$SRC/output/ws63/acore/ws63-liteos-app/ws63-liteos-app.elf"

# sample name -> CONFIG_SAMPLE_SUPPORT_<X> token (watchdog => WDT)
declare -A KCONF=(
  [tcxo]=TCXO [timer]=TIMER [dma]=DMA [systick]=SYSTICK [watchdog]=WDT [pwm]=PWM
  [adc]=ADC
)
SAMPLES=("${@:-tcxo systick adc}")
# shellcheck disable=SC2206
SAMPLES=(${SAMPLES[@]})

[ -f "$APP_CFG" ] || { echo "app config not found: $APP_CFG" >&2; exit 1; }
mkdir -p "$FIX"

# auto-detect strip if not given
STRIP="${STRIP:-$(find "$SRC/tools/bin/compiler/riscv" -name '*-strip' 2>/dev/null | head -1)}"

# every SAMPLE_SUPPORT token we might toggle (so each build has exactly one on)
ALL_TOKENS=(TCXO TIMER DMA SYSTICK WDT PWM I2C SPI ADC PINCTRL BLINKY UART)

set_sample() {
  local want="$1" t
  # base: peripheral framework on, BLE mesh off (BT is cut)
  sed -i 's/^# CONFIG_ENABLE_PERIPHERAL_SAMPLE is not set/CONFIG_ENABLE_PERIPHERAL_SAMPLE=y/' "$APP_CFG"
  sed -i 's/^CONFIG_SAMPLE_SUPPORT_BLE_MESH=y/# CONFIG_SAMPLE_SUPPORT_BLE_MESH is not set/' "$APP_CFG"
  # clear every sample token, then enable the wanted one
  for t in "${ALL_TOKENS[@]}"; do
    sed -i "/^CONFIG_SAMPLE_SUPPORT_${t}=y/d" "$APP_CFG"
  done
  # ensure ENABLE line present (in case the first sed didn't match an already-on file)
  grep -q '^CONFIG_ENABLE_PERIPHERAL_SAMPLE=y' "$APP_CFG" || \
    echo 'CONFIG_ENABLE_PERIPHERAL_SAMPLE=y' >> "$APP_CFG"
  echo "CONFIG_SAMPLE_SUPPORT_${want}=y" >> "$APP_CFG"
}

for s in "${SAMPLES[@]}"; do
  tok="${KCONF[$s]:-}"
  [ -n "$tok" ] || { echo "!! unknown sample '$s' (no Kconfig token)"; continue; }
  echo "==> building $s sample (CONFIG_SAMPLE_SUPPORT_$tok)"
  set_sample "$tok"
  ( cd "$SRC" && python3 build.py ws63-liteos-app -c -ninja ) >/tmp/csdk-$s.log 2>&1 || {
    echo "!! build failed for $s (see /tmp/csdk-$s.log)"; continue; }
  [ -f "$ELF_OUT" ] || { echo "!! no ELF produced for $s"; continue; }
  cp "$ELF_OUT" "$FIX/$s.elf"
  # full strip (QEMU -kernel needs only PT_LOAD segments + entry, not symbols) ~400 KB
  [ -n "$STRIP" ] && "$STRIP" "$FIX/$s.elf" 2>/dev/null || true
  echo "   -> $FIX/$s.elf ($(du -h "$FIX/$s.elf" | cut -f1))"
done

# Refresh the NV / partition flash overlay (tests/csdk/flash/) from this build.
FLDIR="$FIX/flash"
mkdir -p "$FLDIR"
cp_overlay() { [ -f "$1" ] && { cp "$1" "$FLDIR/$2"; echo "   overlay: $2 ($(du -h "$FLDIR/$2" | cut -f1))"; }; }
cp_overlay "$SRC/output/ws63/acore/param_bin/root_params_sign.bin" partition_params.bin
cp_overlay "$SRC/output/ws63/pktbin/ws63_all_nv.bin"               nv.bin
cp_overlay "$SRC/output/ws63/pktbin/ws63_all_nv_factory.bin"       nv_factory.bin

echo "done. Review tests/csdk/manifest.txt markers, then run scripts/csdk-test.sh."
