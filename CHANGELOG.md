# Changelog

All notable changes to **ws63-qemu** (the `qemu-system-riscv32 -M ws63` machine
model + the HiSilicon xlinx ISA patch) are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **C SDK peripheral-sample tests** (`scripts/csdk-test.sh` + `tests/csdk/`). Boots
  prebuilt fbb_ws63 C SDK peripheral-sample ELFs on the WS63 machine and asserts
  each sample's UART success marker — validating the peripheral models against real
  vendor firmware (complements the ws63-rs `scripts/smoke-test.sh`). Shipping
  fixtures: `tcxo` (TCXO ms/us counter) and `systick` (SysTick counter), both green
  in CI. `scripts/build-csdk-samples.sh` regenerates fixtures from a fbb_ws63
  checkout (selects one `CONFIG_SAMPLE_SUPPORT_*`, clean-builds, strips to ~400 KB).
  Known gaps (sample builds but blocks on an unmodelled completion path, documented
  in `tests/csdk/manifest.txt`): `timer` (timer-completion IRQ), `dma` (LLI
  descriptor mode), `watchdog` (kick/feed timing), `adc` (conversion-done read).
- **NV / partition flash overlay** (`scripts/run.sh NV=1` + `tests/csdk/flash/`).
  A `-kernel` boot skips flashboot, so the flash XIP window is empty and the C SDK's
  partition-table + NV reads fail. `NV=1` loads the partition table (`params`, table
  magic `0x4b87a54b` @ XIP 0x200380) and the software/factory NV stores into flash —
  outside the app's own XIP region — so `uapi_partition_get_info()` and NV reads
  succeed (the "[UPG] ...flash_start_addr fail" messages go away). csdk-test.sh
  asserts this. The per-chip factory-calibration keys (e.g. `xo_trim`) are written at
  production and absent from any build NV, so that one read still reports — by design.

## [0.2.0] - 2026-06-01

### Added
- **Local-interrupt priority/threshold enforcement (LOCIPRI/PRITHD).**
  `ws63_local_irq_pending()` now reads each custom local IRQ's 4-bit `LOCIPRI`
  priority (8 per register) and delivers only the highest-priority pending+enabled
  IRQ whose priority is **strictly greater than `PRITHD`**, breaking ties by lowest
  IRQ number. Reset defaults every IRQ to priority 1 and the threshold to 0, so
  firmware that never touches the priority CSRs keeps the previous deliver-by-number
  behaviour. Validated by a probe (mask / preempt / strict-`>` boundary), 5/5.
- **Clock tree — PCLK-derived timer + clock gating + source routing.**
  The timer now derives its rate from `ws63_periph_clk_hz()`: the PLL (240 MHz)
  when locked, else the TCXO (24/40 MHz per `HW_CTL`). `CLDO_CRG_CKEN_CTL0/1`
  (0x44001100/04) are modelled as clock gates — clearing the timer gate
  (`CKEN_CTL0` bit 21) freezes the timer and restoring it resumes counting; gates
  default **on** so firmware that relies on the default (e.g. the ws63-rs timer
  HAL) is unaffected. `CLDO_CRG_CLK_SEL` (0x44001134) TCXO/PLL source select is
  tracked as state. Validated by a clock-gate probe, 3/3 (on → runs, off → frozen,
  on → resumes).
- **Deterministic instruction-counted timing via `-icount`.**
  `scripts/run.sh ICOUNT=1` adds `-icount shift=2` (~250 MHz, IPC=1); `ICOUNT_SHIFT`
  overrides the shift. Virtual time is then bound to the instruction count, so a
  given firmware produces **identical timing every run** (measured: a 1e6-iteration
  loop reads 2,880,003 timer ticks on every run, vs. wall-clock drift without it).
  This is an IPC=1 approximation, **not** microarchitectural cycle accuracy.
- `CHANGELOG.md` (this file).

### Changed
- Timer base rate corrected from a fixed nominal **24 MHz to 240 MHz PCLK**. This
  matches the ws63-rs timer HAL, which already documents `PCLK = 240 MHz`
  (1 tick ≈ 4.17 ns) — the old 24 MHz was a silent 10× timing mismatch.
- GitHub Actions upgraded to the Node 24 generation (`checkout@v5`, `cache@v5`,
  `upload-artifact@v6`, `gh-release@v3`, …); `cargo audit` via `taiki-e/install-action`.

### Fixed
- The `CLDO_CRG` `CKEN` register shadow now defaults to `0xFFFFFFFF` (matching the
  internal gate state), so a firmware read-modify-write that enables some *other*
  peripheral's gate can no longer accidentally clear the timer gate.

## [0.1.0] - 2026-06-01

First public release — `qemu-system-riscv32` (fork of QEMU v9.2.4) with the
HiSilicon **WS63** machine and the **xlinx** custom RISC-V ISA, running unmodified
vendor-compiled firmware without hardware.

### Added
- **WS63 machine** (`-M ws63`): single RV32IMFC core, full memory map, SRAM/flash.
- **HiSilicon xlinx custom ISA** (`xlinxma/mb/mc`): `l.li`, barrel-shift ALU,
  branch-immediate, `muliadd`, 25-bit `jal16`/`j16`, `ldmia`/`stmia`,
  `push`/`pop`/`popret`, `uxtb`/`uxth`, compressed `lbu`/`lhu`/`sb`/`sh`
  (13/13 decoder unit tests).
- **Interrupt controller**: standard `mie` IRQ 26–31 **and** custom local IRQ ≥32
  (`LOCIEN`/`LOCIPD`/`LOCIPCLR` CSRs + vectored `mtvec`), delivered via the
  `target/riscv` patch (mcause = IRQ number, mip/mie can't hold ≥32).
- **Boot-param hand-off + TCXO clock/counter model** → the fbb_ws63 C SDK
  `flashboot` prints on UART; **mask-ROM call interception** → `ws63-liteos-app`
  boots LiteOS to "cpu 0 entering scheduler".
- **All 35 SVD peripherals modelled.** Behaviorally complete: DMA/SDMA (real memory
  transfer + IRQ), RTC/WDT (timers + SoC reset), Timer, I2C/SPI/I2S (FIFO loopback),
  LSADC, UART (TX + RX-from-chardev), TSENSOR (synthetic), EFUSE (OTP), TRNG, SFC;
  GPIO as a real pin signal-net with **IO_CONFIG pinmux routing**; CLDO_CRG / RF /
  crypto kept as config shadow.
- **Tooling**: `scripts/{build,run,smoke-test,setup-deps}.sh`, a tag-triggered
  release workflow, and the `ROADMAP.md` / `docs/design.md` documentation set.

[Unreleased]: https://github.com/sanchuanhehe/ws63-qemu/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/sanchuanhehe/ws63-qemu/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/sanchuanhehe/ws63-qemu/releases/tag/v0.1.0
