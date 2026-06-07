# Changelog

All notable changes to **ws63-qemu** (the `qemu-system-riscv32 -M ws63` machine
model + the HiSilicon xlinx ISA patch) are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **`-M bs21` — HiSilicon BS21 / BS2X machine (multi-chip family, milestone 1).**
  BS21 is a BLE 5.4 + SLE (NearLink) SoC, no Wi-Fi, on the *same* HiSilicon
  "HimiDeer" riscv31 core as WS63 (identical LOCI interrupt architecture + custom
  CSRs, identical UART v151 / TIMER v150 / GPIO v150 IP). So the new `bs21.c`
  machine **reuses ws63.c's device models in place** — exposed via a new shared
  header `hw/riscv/hisi_riscv31.h` (the WS63 INTC/UART/TIMER/GPIO/TCXO type names
  + `ws63_make_ram` / `ws63_register_custom_csrs` / `ws63_intc_set_cpu_env`) — at
  BS21's own memory map + base addresses + IRQ numbers (docs/bs21-recon.md, from
  the fbb_bs2x SDK). `CONFIG_BS21` (Kconfig + meson, patch `000N-...bs21...`)
  `select`s `CONFIG_WS63` so the shared models compile in; `build.sh` copies
  `bs21.c` + `hisi_riscv31.h`. M1 firmware (ws63-rs `bs21-examples/{blinky,
  uart_hello}`, `--features chip-bs21`) emits only standard RV32IMFC and never
  calls the mask ROM, so this machine needs **neither** the HiSilicon custom-ISA
  decoder **nor** ROM interception (both deferred with connectivity). Validated
  on v10.0.0: `scripts/bs21-smoke-test.sh` boots both examples (UART0 @ 0x52081000
  banner; GPIO0 @ 0x57010000 toggle, 0 illegal-instruction traps). WS63 unchanged
  — `-M ws63` boots + all 5 qtests still pass. The full family split (shared models
  → `hisi_riscv31.c` under `CONFIG_HISI_RISCV31`, ROM-ABI machine callback, and the
  `hisi-riscv31` CPU rename) is deferred to the connectivity-generalization pass.
- **Phase 1 test/dev infrastructure complete** (ROADMAP §1):
  - **qtest** (`tests/qtest/ws63-test.c`, run via `scripts/qtest.sh`, in CI) — a
    register-level, boot-free regression that drives the GPIO/UART/timer/INTC/DMA
    models directly over MMIO via libqtest. 4 cases: GPIO DATA set/clr/OEN/INT-EN
    read-back; UART FIFO/line-status reset values; timer load/enable/fire with
    delivery into the INTC (IRQ 26, observed via `qtest_irq_intercept_in`); DMA
    channel-0 mem→mem block copy + raw completion status. Injected + meson-registered
    by `scripts/build.sh`.
  - **`-cpu ws63`** named CPU (`target/riscv` patch): RV32IMFC_Zicsr + Zcf, no A/D,
    no MMU, Zcb/Zcmp deliberately disabled (the xlinx custom code-size insns own that
    compressed encoding). It is now the machine's `default_cpu_type`, so machine init
    no longer pokes the configurable CPU's per-letter properties — which also makes the
    machine initialise under `-accel qtest` (that accel doesn't expose those properties).
  - **Semihosting pass/fail** — `scripts/run.sh SEMIHOST=1` adds `-semihosting`; the
    ws63-rs `semihost_selftest` example reports its result via the RISC-V semihosting
    `SYS_EXIT_EXTENDED` exit code (and `SYS_WRITE0` console), so CI gets pass/fail
    without scraping UART (`scripts/smoke-test.sh` asserts exit code 0).
  - **GPIO/DMA trace events** — the temporary GPIO `qemu_log` calls became proper
    `hw/riscv/trace-events` events (`ws63_gpio_set` / `ws63_gpio_clr` / `ws63_dma_xfer`),
    enabled selectively with `-d trace:ws63_gpio_*`. `build.sh` appends them to the
    upstream `hw/riscv/trace-events`; `smoke-test.sh`'s blinky check uses them.
- **System-reset model** (SYS_CTL0 handler). GLB_CTL_M's chip-reset trigger
  (`0x40002110` bit 2) now issues `qemu_system_reset_request`, and the reset-reason
  history record (`SYS_RST_RECORD_0` @ `0x400000A0`, cleared via `SYS_DIAG_CLR_1` @
  `0x400000A4`) is modelled with a host-side record that survives the guest reset —
  so firmware reads back "software reset" on the next boot. Mirrors fbb_ws63
  `reboot_porting.c`. Validates the ws63-rs `System::software_reset` / `reset_reason`
  rewrite: the new `reset_demo` example round-trips cold-boot → reset → Software, and
  `scripts/smoke-test.sh` asserts it (CI now builds all default-member examples).
- **Connectivity base** (ROADMAP §5): synthetic Wi-Fi/Ethernet MAC (`ws63-netmac`
  @ 0x44210000, IRQ 45) bridging the ws63-rf-rs netif seam to a host netdev via
  SLIRP (`--enable-slirp`, `libslirp-dev`); no RF/PHY. qtest `/ws63/netmac` does a
  real frame round-trip; the ws63-rs `net_ping` example ARP/ICMP/UDPs over the
  SLIRP NAT; `scripts/smoke-test.sh` asserts `NET PING: PASS`.
- **qtest matrix** (`.github/workflows/qtest-matrix.yml`): runs the register-level
  qtest across every supported QEMU version (required) plus the newest stable
  (experimental forward-compat radar). Weekly cron.

### Changed
- **Rebased to QEMU v10.0.0** (default; up from v9.2.4) and **migrated the
  injection to a per-version `git format-patch` series** under `patches/<QEMU_TAG>/`,
  replacing the single `ws63-target-riscv.patch` + the sed/cat-append hooks in
  `scripts/build.sh`. New files (`ws63.c`, the xlinx decoder, the qtest) are copied
  from `src/`; edits to existing QEMU files are the series (`0001` target/riscv,
  `0002` machine registration, `0003` qtest registration; older versions add a
  `0004` compat patch for the copied `ws63.c`). **v9.2.4, v10.0.0, v10.2.3, and
  v11.0.1 are maintained** — each verified to apply, build, and pass qtest 5/5
  (v10.x + v11 additionally pass the C-SDK suite). API drift handled per version:
  v10.0 `sysemu/`→`system/` headers and `const` `Property[]` (no terminator); v10.2
  `insn_len`→`internals.h`, declarative `DEFINE_RISCV_CPU`, table-driven
  `decode_opc`, `CharBackend`→`CharFrontend`, `exec/`→`system/address-spaces.h`;
  v11 six `hw/*.h`→`hw/core/*.h`. The `qtest-matrix` now has all four versions
  green; the forward radar will point at the next QEMU release. See
  `patches/README.md`.

### Fixed
- **QEMU v11.0 upstream regression in RV32 `mcycleh`/`minstreth`**
  (`patches/v11.0.1/0005`): v11's `riscv_pmu_read_ctr()` slices the high/low half
  of `ctr_prev`/`ctr_val` for an RV32 fixed counter but reads the fixed-counter
  value (`cpu_get_host_ticks()`) un-sliced, so the upper-half read returns the
  *low* 32 bits — which change every read. Firmware doing the standard atomic
  64-bit cycle read (`csrr hi,mcycleh; csrr lo,mcycle; csrr hi2,mcycleh; bne`)
  never converges and spins forever (ws63-rs `rf_port_demo` hung after
  `osal_kmalloc` on v11; fine on v9.2.4/v10.0.0/v10.2.3, which sliced internally).
  Fix slices the fixed-counter value with the same `extract64`. v11 firmware
  smoke-test 16/17 → 17/17. A generic upstream fix, worth reporting to QEMU.

## [0.3.0] - 2026-06-01

Validation against real vendor firmware: a C SDK peripheral-sample test harness
(now in CI alongside the ws63-rs smoke test), an NV / partition-table flash overlay,
and the model fixes those tests turned up — the LSADC, the DMA engine, the local-IRQ
delivery path, and the watchdog ROM API — plus a catalogue of every mask-ROM stub.

### Added
- **C SDK peripheral-sample tests** (`scripts/csdk-test.sh` + `tests/csdk/`). Boots
  prebuilt fbb_ws63 C SDK peripheral-sample ELFs on the WS63 machine and asserts each
  sample's UART success marker — validating the peripheral models against real vendor
  firmware (complements the ws63-rs `scripts/smoke-test.sh`). Shipping fixtures, all
  green in CI: `tcxo` (TCXO ms/us counter), `systick` (SysTick counter), `adc` (LSADC
  conversion) and `dma` (memory copy), plus an NV-overlay assertion — **5/5**.
  `scripts/build-csdk-samples.sh` regenerates fixtures from a fbb_ws63 checkout
  (selects one `CONFIG_SAMPLE_SUPPORT_*`, clean-builds, strips to ~400 KB). Remaining
  documented gap (`tests/csdk/manifest.txt`): `timer` — not a peripheral-model gap
  (the HW timer fires fine), but the LiteOS software-timer task layer never reaches
  `uapi_timer_start`. The `watchdog` sample runs healthy (it kicks — see below) but
  its interrupt-mode "kick timeout!" marker is not asserted.
- **NV / partition flash overlay** (`scripts/run.sh NV=1` + `tests/csdk/flash/`).
  A `-kernel` boot skips flashboot, so the flash XIP window is empty and the C SDK's
  partition-table + NV reads fail. `NV=1` loads the partition table (`params`, table
  magic `0x4b87a54b` @ XIP 0x200380) and the software/factory NV stores into flash —
  outside the app's own XIP region — so `uapi_partition_get_info()` and NV reads
  succeed (the "[UPG] ...flash_start_addr fail" messages go away). csdk-test.sh
  asserts this. The per-chip factory-calibration keys (e.g. `xo_trim`) are written at
  production and absent from any build NV, so that one read still reports — by design.
- **`docs/rom-stubs.md`** — a dedicated document cataloguing every mask-ROM stub /
  interception: the `ws63_rom_call` mechanism (illegal-inst trap on 0x109000-0x14C000
  → host-C emulation) and all emulated ROM functions (`mem*_s`, `*printf_s`,
  systick/tcxo time, the timer/SFC vtable getters, and the watchdog API), the
  ROM/boot-adjacent device stubs (TCXO counter, PPB, SYS_CTL0, SFC, flash XIP + NV
  overlay, low-MMIO absorber), the boot-param hand-off, and the "ROM data wall"
  limits (BT/WiFi, factory-calibration NV like `xo_trim`, crypto).

### Fixed
- **Local-IRQ storm on C SDK completion interrupts.** Local IRQs ≥32 now auto-clear
  their LOCIPD bit when the CPU takes them (edge/one-shot). The C SDK delivers local
  IRQs ≥32 by per-IRQ `mcause` through a shared `default_local_interrupt_handler` with
  no LOCIPCLR step, so the hardware auto-clears LOCIPD on delivery — which this now
  models. Previously a device that held its line asserted (e.g. the C SDK DMA/ADC done
  interrupts) re-delivered ~1M times/s and starved the scheduler. No regression: the
  rs `gpio_irq` (IRQ 33) still delivers once per edge.
- **C SDK `adc` sample now passes** (`tests/csdk/adc.elf`). The LSADC model reports
  the v154 offset/cap-calibration-done status (so `uapi_adc_init` finishes) and
  tracks the RX-FIFO level/data; the poll-based `adc_port_read` returns and prints
  `voltage: N mv`.
- **C SDK `dma` sample now passes** (`tests/csdk/dma.elf`). Three v151 DMA model bugs
  fixed: the channel int-status read now includes the `int_trans_st[8:15]` terminal-
  count bits, the descriptor control word's source/dest address-increment flags are
  read from the correct bits (26/27 — previously 27/28, so the destination never
  advanced), and the completion IRQ auto-clears (see above). The poll-based transfer
  completes and the copied-buffer `memcmp` matches → `dma memory copy test succ`.
- **Watchdog ROM API emulated** (`ws63_rom_call`). The whole watchdog stack is
  mask-ROM-resident, so the calls were no-op-stubbed and the WDT never ran. Now
  `uapi_watchdog_init/enable/kick/disable/deinit` are emulated: enable arms a virtual
  one-shot timer, kick re-arms it, and on expiry the SoC reset is requested — the
  watchdog's actual function. A healthy (kicking) firmware is never reset (the C SDK
  watchdog sample runs cleanly to the scheduler). The interrupt-mode timeout callback
  ("watchdog kick timeout!") is not modelled (would need vCPU-synchronised injection).

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

[Unreleased]: https://github.com/sanchuanhehe/ws63-qemu/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/sanchuanhehe/ws63-qemu/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/sanchuanhehe/ws63-qemu/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/sanchuanhehe/ws63-qemu/releases/tag/v0.1.0
