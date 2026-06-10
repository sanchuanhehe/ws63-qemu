# Running BS21/BS2X vendor firmware on `-M bs21`

Status: **loaderboot executes** (the first vendor boot stage runs its full init on
`-M bs21`). This documents the signed-image format, the BS21 ROM table, and the
remaining boundaries — the BS21 analog of the WS63 C-SDK-on-QEMU work.

## What works

`scripts/bs21-vendor-boot.sh <loaderboot_sign.bin>` boots the fbb_bs2x prebuilt
loaderboot (`src/interim_binary/bs21e/bin/boot_bin/loaderboot-bs21e-1100e/`):
it runs **~480 instructions** of real vendor code — relocates the boot-param block
to the DTCM, brings up PMU (`0x57004600`), and reaches its interrupt-driven
download-mode idle spin (`j .` @0x4298e) — **all standard RV32 + xlinx, zero
illegal-instruction traps**. The xlinx decoder (active for `-M bs21` via the `ws63`
CPU type) handles the vendor compiler's custom instructions transparently.

## Signed-image format (loaderboot)

Multi-section HiSilicon signed image:

| offset | section |
|--------|---------|
| `0x000` | image header — magic `0x4bd2f01e`, header-len `0x40` (@0x0c), `KeyAlg 0x2a13c812` (@0x18, = the sign cfg) + zero pad |
| `0x100` | code-info header — magic `0x4bd2f02d`; **code size** = u32 @ (code-info + `0x24`); then a hash block |
| tail   | the code itself (`code_size` bytes, ending at EOF) → for loaderboot: 0x5c20 bytes @ file `0x300` |

The mask ROM copies the code to ITCM and jumps to it. We instead load the
extracted code at **`0x40000`** (`APP_ITCM_ORIGIN`, "use itcm start addr load
loaderboot") and set the reset PC there.

**flashboot** uses the SAME two-header structure, only with different magics
(image header `0x4b1e3c1e` = its ImageId, code-info header `0x4b1e3c2d` @0x100,
size @ code-info+0x24 = 0x8ab0, code at file 0x300). `bs21-vendor-boot.sh` accepts
both magics, so it loads flashboot too. flashboot also links to **`0x40000`**: it
**runs** ~206 instrs (reset → ULP_AON clock config @0x5702c*** → a BSS/relocate
clear loop → its main code @0x40552), then reaches an early halt (`j .` @0x40120)
when it touches the **SFC** (Serial Flash Controller @ `0x90000000`: it writes
`0x509` to `0x90000210` and calls an SFC routine @0x4097a) — the SFC/flash is not
modelled, so the flash read returns garbage and flashboot stops. flashboot's source
(`flashboot_init` + `usb_download` + `upgrade_version_check`) confirms it reads
flash early. **SFC modelling is the next boundary** for flashboot to load the app
and print its banner.

## Memory-map fix (found by running firmware)

The loaderboot reset code copies the boot-param block to **`0x20000000`** — the
real BS21 DTCM (`APP_DTCM_ORIGIN`, len `0x10000`). `bs21.c` had wrongly placed
DTCM at `0xF0000`; loaderboot faulted writing to `0x20002d50`. Fixed: DTCM →
`0x20000000`, ITCM window → `0x80000..0x100000`. (M1 + WS63 qtests unaffected.)

## `bs21_rom_call` — IMPLEMENTED (patches/v10.0.0/0005)

The ROM-call interceptor is now in place (mirrors `ws63_rom_call`, dispatched by
the disjoint PC range). It emulates the secure-libc set the BS2X boot stages call:
`memset_s 0x3d80c`, `memcpy_s 0x3e07e`, `memmove_s 0x3e95c`, `sprintf_s 0x3ef18`,
`snprintf_s 0x3ef60`, `vsnprintf_s 0x3ef92` (the printf family reuses the
chip-neutral `ws63_vformat()`). **Validated**: a synthetic test that `jalr`s to
`0x3d80c` (memset_s) on `-M bs21` is intercepted, the buffer is correctly filled,
and execution resumes at `ra` — serial prints `XA` (X = UART, A = the memset'd
byte). WS63 unchanged (5/5 qtests + M1 still pass). systick/tcxo/SFC/watchdog ROM
APIs are not mapped yet (their BS21 addresses differ) — they fall through to the
success stub. `cpu_helper.c` is version-volatile, so 0005 is on v10.0.0 (the
build.sh default); other QEMU versions need it rebased.

> **Note on loaderboot**: loaderboot is self-contained and makes **zero** ROM
> calls (it never executes below 0x40000), so `bs21_rom_call` does not change its
> behaviour — it reaches its download-mode idle spin either way. `bs21_rom_call`
> serves the later stages (flashboot/app), which call secure-libc heavily.

## BS21 ROM table (source for `bs21_rom_call`)

Source: `fbb_bs2x/src/drivers/chips/bs2x/rom/rom_config/acore/{acore_rom_n1200,
romboot,remote_lib_boot}.sym` (the BS21 `acore.sym` equivalents) + `librom_callback.a`.
The ROM region is **`~0x10000..0x42000`** — **disjoint from WS63's `0x109000..0x14C000`**,
so the interceptor can dispatch by PC range without a machine flag.

Secure-libc (the most-called by later stages), from `acore_rom_n1200.sym`:

| function | BS21 addr | function | BS21 addr |
|----------|-----------|----------|-----------|
| `memset_s`   | `0x3d80c` | `sprintf_s`   | `0x3ef18` |
| `memcpy_s`   | `0x3e07e` | `snprintf_s`  | `0x3ef60` |
| `memmove_s`  | `0x3e95c` | `vsnprintf_s` | `0x3ef92` |

SFC / boot (from `remote_lib_boot.sym`): `uapi_sfc_init=0x1037a`,
`uapi_sfc_init_rom=0x1044a`, `hal_sfc_init=0x1097c`, … (romboot text `0x102ec..0x11e90`).

To intercept, mirror WS63's `ws63_rom_call` (patches/<tag>/0001): when
`env->pc` is in the BS21 ROM range, emulate the function in host C (args in
a0–a3, result in a0) and resume at `ra`.

## Remaining boundaries (the deferred connectivity work)

1. ~~BS21 ROM-call table~~ — DONE (`bs21_rom_call`, patches 0005; see above).
2. ~~flashboot image format~~ — DONE (cracked; `bs21-vendor-boot.sh` runs it).
3. ~~SFC reg model~~ — DONE. The `ws63-sfc` v150 model (RDID→JEDEC ID) is mapped at
   `0x90000000` in bs21.c; flashboot's flash-ID init now succeeds (it runs 9 instrs
   further). M1 + WS63 unaffected.
4. ~~Flash CONTENT — partition table~~ — DONE. flashboot reads the partition table
   via **XIP at flash1 `0x90100000`** (its helper @0x406b4 just does `lui a0,0x90100;
   ret`) and checks the first word against magic **`0x4b87a52d`**. bs21.c now maps a
   `bs21.flash1` RAM region at 0x90100000; loading the prebuilt `partition.bin`
   (`src/interim_binary/bs21e/bin/partition/.../partition.bin`, magic at offset 0)
   there gets flashboot **past the magic** and **~940 instrs** (4x) deep into its
   main path (partition parsing) before a new idle spin @0x4293a. Reproduce:
   `bs21-vendor-boot.sh flashboot_sign_a.bin 5 0x40000 partition.bin`.
5. ~~Full flash image~~ — DONE. `bs21-build-flash.sh` unpacks the fbb_bs2x
   `bs21e_all.fwpkg` (loaderboot/partition/flashboot_a+b/**application**/nv) and lays
   each out at its partition flash offset (app @0x15000 → XIP 0x90115000); the boot
   script chunk-loads it at 0x90100000 (the generic loader caps a single raw load at
   ~0x10000). flashboot loads with the full flash present.
6. **The 0x4293a halt is a CRASH, not a boot-mode decision — it is the absent
   BS21 mask-ROM** (cracked 2026-06-09). Earlier notes guessed a "boot-reason check"
   at 0x41730; that was wrong — it came from objdump **misdecoding xlinx**. The real
   mechanism (decoded with the vendor xlinx-aware objdump, see below):

   - The spin @0x4293a is flashboot's **panic tail**: `irq_lock()` → record reason
     `0xdeadbeaf` into DTCM @0x2000ffe8 (`0x44f1c`) → clear bit0 of
     `BOOT_PORTING_RESET_REG` 0x57004600 (`0x42916`) → `j .`. It is reached from
     flashboot's **exception handler** (`mtvec = 0x47bbc`), which printf-dumps
     `exception:/uwExcType=/mepc=/mstatus=/mtval=/mcause=/ra=/sp=…` (strings @0x4871c+)
     via the log fn `0x43b40`. So flashboot **trapped**, then panicked.
   - **First trap: `mcause=0x2` (illegal instr), `mepc=0x0`.** flashboot validates the
     mask-ROM signature: `0x43c8a: lui a5,0x10; lw a4,32(a5)` reads `*(0x10020)` and
     `bne a4, 0xd4818193, 0x4403a`. The ROM region (0x10000–0x40000) is **zeroed RAM**
     in `-M bs21` (no mask-ROM dump exists), so `*(0x10020)=0 ≠ 0xd4818193` → it tail-
     `ret`s at 0x4403a with **ra=0** → jumps to PC 0 → illegal-instruction trap.
   - **Proof:** inject the magic — `-device loader,file=<0xd4818193>,addr=0x10020` —
     and flashboot **stops branching to the crash** (0 hits on 0x4403a), reaches the
     magic-OK path 0x43c98, and runs further (1260 vs 723 insns) before a **second**
     crash (`mcause=0x5` load-access-fault @ `0x570004a0`). So flashboot has **many**
     mask-ROM/peripheral dependencies; the magic is just the first.
   - **Conclusion:** like WS63, flashboot is tightly coupled to the silicon mask-ROM
     (signature + ROM data tables + ROM functions it tail-calls and registers
     callbacks into, e.g. ROM addr 0x1c200 at 0x43c9a). The SDK ships only
     `librom_callback.a` + `.sym` (no ROM image), so the path forward is **emulating
     the BS21 mask-ROM** (synthesize the signature/tables + extend `bs21_rom_call` for
     the functions it invokes) — the same scale as the WS63 ROM-on-QEMU work, i.e. the
     deferred connectivity-scale effort. This is NOT a single register to model.

   **Tooling note (reusable):** objdump misdecodes xlinx as `fld/fsd/.insn/illegal`.
   The fbb_ws63 vendor toolchain decodes it correctly — use
   `…/cc_riscv32_musl_105/cc_riscv32_musl_fp/bin/riscv32-linux-musl-objdump
   -b binary -m riscv:rv32 -D --adjust-vma=0x40000` on the extracted code. It shows the
   real `popret/push/pop/l.li/uxth/divu/...` (BS2X linx131 == WS63 xlinx, same ISA).
   For a true linear trace + per-insn registers use QEMU
   `-accel tcg,one-insn-per-tb=on -d in_asm,cpu`.

7. **mask-ROM signature MODELLED → flashboot RUNS its init + prints its banner
   (2026-06-09).** Two `bs21.c`-local fixes got flashboot from the 0x4293a panic to a
   clean, fully-diagnosed app-load attempt:
   - **ROM signature**: `bs21.c` pokes `*(uint32_t*)0x10020 = 0xd4818193` into the
     RAM-backed ROM region at machine init (a small extensible `rom_data[]` table) —
     so the signature check passes natively (no `-device loader` injection).
   - **TCXO collision fix**: the shared `ws63-tcxo` model is mapped at `0x57000200`
     with a 0x1000 region (WS63 layout, where TCXO sits alone at 0x44000000) — on BS21
     that swallowed the whole GLB_CTL_A/D block and **faulted on flashboot's 2-byte
     read of `0x570004a0`** (TCXO ops require 4-byte). Per the SDK
     (`TCXO_COUNT_BASE_ADDR 0x57000200`, `GLB_CTL_A 0x57000400`) the TCXO_COUNT block
     is only 0x200, so `bs21.c` aliases just `BS21_TCXO_SIZE 0x200` of the device —
     GLB_CTL_A/D fall through to the absorber. And BS21's TCXO_COUNT sits at the region
     **base** (offset 0), not WS63's +0x4C0, so `ws63_tcxo_set_count_off(tcxo, 0)`
     (new setter in `hisi_riscv31.h`) points the status/lo/hi there — flashboot's
     `*(0x57000200)` bit-4 (count-valid) poll now passes (and BS21 `uart_hello`'s tick
     counter reads correctly too). WS63's TCXO keeps its 0x4C0 default → 5/5 qtests
     unaffected.

   With these, flashboot runs **2953 insns** (was 723) through its full init and prints:
   ```
   Flashboot Init! id = 0x0 / Power On / Reboot cause:0xF0F0 / Reboot count:0x0
   Flash Init ret = 0x80001341 / Load App Failed!
   ```
8. **SFC flash ID fixed → flashboot LOADS the app and JUMPS to it (2026-06-09).** The
   `Flash Init ret=0x80001341` was a **flash-ID mismatch**: BS21's flashboot flash table
   is **GigaDevice** (`sfc_config_info_porting.c`: GD25LE80), and at `0x42122` it
   compares the read JEDEC ID against **`0x1460C8`** (mfr 0xC8 / type 0x60 / cap 0x14) —
   but the shared `ws63-sfc` model answered RDID with WS63's Winbond W25Q16 (0x1560EF),
   so detection failed → error flag → 0x80001341. Made the SFC's RDID ID per-machine
   (`ws63_sfc_set_flash_id()`, default W25Q16; bs21.c sets `0x1460C8`). Now:
   ```
   Flashboot Init! / Power On / Reboot cause:0xF0F0 / Reboot count:0x0
   Flash Init ret = 0x0 / No need to upgrade / Jump to addr = 0x90115300
   ```
   With the **full flash image** present (`bs21-build-flash.sh`, app @ flash 0x15000 →
   XIP 0x90115000), flashboot's flash detect ✓, upgrade-version check ✓, and it **loads
   + jumps to the app at 0x90115300**. The app's RTOS startup then executes from XIP
   (sets `mtvec`, clears `mstatus`/`mie`, programs the LOCI custom CSRs `0x7c2`/`0x7c3`,
   sets `gp` to DTCM `0x2000369c`). **flashboot's job is complete.** Reproduce:
   `bs21-vendor-boot.sh flashboot_sign_a.bin 8 0x40000 <full-flash.bin>` (the 4th arg is
   the `bs21-build-flash.sh` image). Running the full LiteOS BLE/SLE app from here is the
   broader connectivity work (full memory map / RAM init / all peripherals).

9. **App (LiteOS) startup runs — xlinx `prefd` decoded; app reaches its init-call
   table (2026-06-09).** The app's RTOS startup ran only 21 insns then trapped
   (mcause=2) on `prefd` (`0x0003300b`) in its `.data` copy loop — opcode 0x0b funct3=3,
   a cache-prefetch hint the xlinx decoder didn't handle (it routed all 0x0b to
   ldmia/stmia, which matched no register bits → illegal). Added funct3 dispatch:
   0x0b f3 0/1 = ldmia/stmia (already handled), **f3 2/3 = pref/prefd → NOP** (a
   prefetch is architecturally a no-op). With that, the app's `ldmia`/`stmia` block
   copy runs and the app executes **934 insns** of real LiteOS startup (copies `.data`
   flash→DTCM, sets up gp/sp, runs early init), then walks its **init-call table**
   (11 fn-ptrs @ 0x90115a10..0x90115a3c). **Next blocker:** init `table[9]` (0x90118a3a,
   a LiteOS task/thread registration — entry 0x90118cfe, stack 1536, prio 31, via the
   creator at 0x90118a2a) returns error **0x2000209** → the app logs + halts at
   `0x90128c62 (j .)`.

10. **xlinx `muliadd` immediate bug fixed → app passes LiteOS task creation, runs the
    full kernel init (2026-06-09).** The init-call `table[9]` failure was a **decoder
    bug**, not a missing subsystem: `LOS_TaskResume` indexes the TCB array as
    `base + id*92` via xlinx `muliadd s0,s0,a0,92`, but the decoder masked the
    immediate's funct7 field to 5 bits (`& 0x1f`) instead of 7 (`& 0x7f`) — so any
    immediate > 63 was truncated (92 → 28). `TCB[1]` resolved to `base+28` instead of
    `base+92` → resume read the wrong TCB (status 0, not SUSPENDED) → 0x2000209.
    Verified against 82 `muliadd` instances with imm 64–192: the 5-bit mask got all 82
    wrong, the 7-bit mask gets all 82 right (immediate = `(funct7<<1) | bit14`, 8 bits).
    With the fix the app passes task creation and runs the **whole LiteOS kernel init**
    (1663 app insns, was 934), reaching the app's own reboot/idle path
    (`APP|Reboot core:%d cause 0x%x`, halts @0x90126206). **Next boundary:** that path
    reads a ULP_AON register (`*0x5702c0f0` vs 0x10000, our absorber returns 0) and
    looks app/multi-core specific (`core:2` — likely the BT slave core) — the deeper
    app/connectivity layer. (WS63 5/5 qtests + BS21 M1 unaffected by the decoder fix.)

11. **xlinx `ldmia`/`stmia` bank-selector bug fixed → the `core:2` "multi-core" halt
    was an app CRASH, now gone (2026-06-10).** Verified first whether multi-core was
    real: a BT slave core *does* exist (`slave_cpu_t{SLAVE_CPU_BT, SLAVE_CPU_MAX_NUM}`),
    but the `APP|Reboot core:2` halt at 0x90126206 was **not** a multi-core boundary —
    `core:2` is a hardcoded reboot-source code, and the path was reached via a
    **NULL-pointer `memcpy`** whose source pointer had been clobbered. Root cause: a
    third xlinx **decoder bug**. `ldmia/stmia` (opcode 0x0b) carries a 16-slot register
    presence bitmap, and **bit31 selects which of two register banks** each slot maps to;
    the old decoder only knew **bank 0** (`{ra,sp,s0,s1,a0,a1,s2..s11}`). LiteOS's
    8-word `memcpy` block copy uses a **bank-1** list (`{ra,t0..t2,a0..a7,t3..t6}`), so
    its `ldmia`/`stmia` decoded as bank-0 s-registers and **clobbered the caller's
    `s0..s11`** → a later `memcpy(dst, s0=NULL, n)` → crash → the reboot path. Added the
    bit31 bank selector + a `[2][16]` slot→GPR table (slot bits `7,8,9,10,11,20..30`),
    derived and **validated 100% against 164 real `ldmia`/`stmia` from the firmware**
    (96 bank-0 + 68 bank-1). With the fix the crash is gone (0 traps, mcause=0) and the
    app advances past the reboot path into further init. **Next blocker:** the app now
    spins on a **32K-clock calibration poll** at 0x901286b2 — `while (!(*(u16*)0x57008488
    & 1))`, i.e. bit0 of `HAL_CALIBRATION_32K_CLOCK_DET_STS` (PMU2_CMU `0x57008000` +
    `0x488`, `hal_32k_clock.c:18`); our generic absorber returns 0, so the wait never
    completes. Modeling that status bit as "calibration done" (bit0=1) is the next step.
    (WS63 5/5 qtests ×4 QEMU versions + BS21 M1 unaffected by the decoder fix.)

12. **32K-clock calibration status modelled → app passes the poll, runs further bring-up
    (2026-06-10).** Added a small `bs21.clk32k` MMIO region over the GLB absorber at the
    PMU2_CMU 32K-clock-detect block (`0x57008480`, size `0x20`): `DET_STS` (`+0x08` =
    `0x57008488`) reads bit0=1 (DONE) / bit1=0 (not DOING), so `hal_32k_clock_get_detect_
    result()`'s `while (!(STS & 1))` completes immediately; CFG/VAL enable writes are
    no-ops. The detect result reads 0 — which is **safe**: `calibration_get_clock_frq()`
    (clock_calibration.c:55) guards `if (result != 0)` before dividing, so on 0 it keeps
    the default `g_clock_32k = 32768 Hz`. Verified the exact access sequence (WR VAL=0x40,
    DOING-check, clr/set ENABLE, DONE-poll=1, read RES=0) — 9 accesses, no loop. With this
    the app passes 0x901286b2 and runs **far more** init (memory pool, **LOS task
    creation** at 0x90118xxx, registration tables, 0x90122/0x90126/0x90128xxx) before the
    next blocker. **Next blocker:** the app enters a tight 20-PC ITCM loop (resident
    boot-stage service code at 0x41/0x42/0x45xxx) that **polls a 2-bit field of
    `0x570007a0`** (GLB_CTL_A region, via a descriptor-table register accessor) with TCXO
    delays between reads, interleaved with ULP_AON config writes (`0x5702c934 = 0xc5`,
    reads of `0x5702c330..340`). Our generic absorber returns 0, so the field never
    reaches its ready value → infinite wait. This is the deeper **clock/power-domain
    bring-up** (a PLL/clock-switch or LDO "ready" status); modeling `0x570007a0`'s ready
    field correctly needs the BS2X GLB_CTL_A register map. (Also fixed `bs21-smoke-test.sh`
    to find the examples under the post-regroup `examples/bs21` path — it had silently
    SKIPped, a false PASS. WS63 5/5 qtests + BS21 M1 — uart_hello banner + 13 GPIO toggles,
    0 illegal traps — both green.)

13. **Stuck loop traced to eFUSE (v151) + ULP_AON PMU bring-up (2026-06-10).** Dug
    `0x570007a0` to its root. The app is **truly stuck** (identical 3723 app PCs / max PC
    `0x901662bc` / 9 UART lines at both 8s and 25s), in a **20-PC ITCM loop** with **no
    app PCs** — an init/handler-table iteration (table of `{fn_ptr@0x9019Fxxx, id, 0,
    fn_ptr@0x90166xxx}` at DTCM `0x20002ae4`) whose one handler runs **eFUSE + ULP_AON
    PMU register operations** that never satisfy their exit condition. Decoded from live
    DTCM dumps: `0x425b6` = `hal_efuse_switch_en_set(0xa5a5)` (writes the eFUSE unlock
    magic `0xa5a5` to `g_efuse_switch_en_addr = 0x5702C258`); the per-byte loop `0x4269e`
    iterates eFUSE bytes via `g_efuse_base = 0x57028030`, read-data `0x57028800+`,
    boot-done status `g_efuse_boot_done_addr = 0x5702802C` (bit2 = `efuse_boot2_done`,
    `EFUSE_BOOT_DONE_MASK 0x4`). The eFUSE IP is **v151** (full source in
    `fbb_ws63/.../hal/efuse/v151/`; `HAL_EFUSE_SWITCH_EN 0xa5a5`). Steady-state MMIO is a
    *varied* sequence (not one poll): eFUSE `0x57028030`/`0x5702887c` + ULP_AON
    `0x5702c204`/`0x5702c520`/`0x5702c930`/`0x5702c938`/`0x5702c974` writes(`0xa5a5`/
    `0x5a5a`/configs)+reads. **Root cause:** neither the eFUSE controller (`0x57028000`)
    nor the ULP_AON PMU status block (`0x5702c000`) is modelled — the absorber returns 0,
    so the calibration/PMU "ready"/trim-data the handler waits for never appears. **This
    is a distinct, larger phase:** model the **eFUSE v151 controller** (boot-done bit2
    set; switch-en accepts `0xa5a5`; read-data returns blank/efuse image) **+ the ULP_AON
    PMU status registers**. The exact ready-bit/expected-value needs the BS2X ULP_AON +
    eFUSE register maps, which are **not in the open SDK source** (precompiled hal libs /
    generated headers) — so it isn't a quick single-register model like the 32K-clock one;
    it wants the register spec first (per "C SDK as sole ground truth", not guessed bits).
    Reusable probes used: live DTCM dumps via QMP `human-monitor-command "xp/Nxw addr"`;
    `-d in_asm` app-PC-coverage diff across run lengths to prove stuck-vs-slow; `-d unimp`
    tail to read the steady-state MMIO footprint.

14. **Real root cause = TCXO v150 count layout (16-bit chunks); eFUSE v151 modelled →
    app runs all clock/PMU/calibration init to its stage-1 reboot (2026-06-10).** §13's
    "eFUSE/ULP_AON status" guess was wrong. **Key discovery:** the app **overwrites ITCM**
    (0x40000) with its own copied code, so the earlier flashboot disasm of the loop was of
    the wrong bytes — dumping the *live* ITCM (QMP `pmemsave`) revealed the truth. The 20-PC
    loop is a `uapi_tcxo_delay_us` busy-wait: a counter-read (`0x42816`) latches the TCXO
    and assembles a 64-bit count as `(*0x57000204 & 0xffff) | (*0x57000208 << 16)` (low) and
    `(*0x5700020c & 0xffff) | (*0x57000210 << 16)` (high). Per the SDK (`hal_tcxo_v150_regs_
    def.h`, **ground truth**) the BS21 TCXO has **four 16-bit count registers** (count0[15:0]
    @+4, count1[31:16] @+8, count2[47:32] @+0C, count3[63:48] @+10; status `valid` = bit4).
    Our shared TCXO model used the **WS63 2-register layout** (count[31:0] @+4, count[63:32]
    @+8), so the app assembled only `count[15:0]` (wrapping every ~2 ms) with high = 0 → the
    delay's `now < target` never held → infinite wait. **Fix:** `ws63_tcxo_set_chunked16()`
    — a BS21-only mode that splits the count into the four 16-bit chunks (WS63 path untouched,
    default off → byte-identical, 5/5 qtests green). Also modelled the **eFUSE v151
    controller** (`hal_efuse_v151`, ground truth): boot-done sts asserted (0x2c → 0x1c),
    ctl/avdd/clock_period readback, a blank 128-byte (1024-bit) fuse array (trims read 0 →
    calibration uses defaults), and OR-on-write programming — faithful supporting
    infrastructure (it isn't the gate, but it stops the absorber-returns-0 eFUSE boot-done
    from costing the bounded `check_efuse_boot_done` its 50×1 ms each call). With both, the
    app passes the delay loop and runs **all** its clock/PMU/eFUSE/calibration init (new code
    at 0x9012d5xx/0x9012fxxx), then **deliberately reboots**: `reboot(core=2, cause=0x2003)`
    via `0x9012d59c` (`"APP|Reboot core:%d cause 0x%x"`, reason stored at DTCM 0x2000ffd8,
    magic 0xdeadbeaf @0x2000ffe8) — core 2 = APPS_CORE; it clears bit0 of BOOT_PORTING_RESET_
    REG `0x57004600` and spins `j .` waiting for the chip reset. This is the **original
    "core:2" halt, now reached cleanly** (not via a crash) after a full init pass — a stage-1
    → stage-2 reboot. WS63 5/5 qtests + BS21 M1 (uart_hello banner + 13 GPIO toggles, 0
    illegal traps) both green. **Key probe:** the app overwrites ITCM — always disassemble
    *live* memory (QMP `pmemsave`), not the loaded image, once past the app handoff.

15. **The `cause=0x2003` reboot is a PANIC from an event-wait timeout = the connectivity
    boundary (2026-06-10).** Classified the reboot (per §14's open question): it is **not** a
    clean stage-1→2 reboot, it's a **software panic** (no CPU trap — mtvec `0x44330` is never
    entered; mcause/mepc = 0). The panic handler `0x9012f8ec` prints `"APP|[panic]id:%d,
    code:0x%x,call:0x%x"` then `0x9012f9a0` dumps context and calls `reboot(core=2,
    cause=0x2003)`. The trigger is a **queue-consume + timed-wait loop** at `0x9013062e`: it
    polls a software message queue (`0x90130368` reads list entries at `0x4bed0`/`0x4bec4`),
    checks a pending state (`0x90122cd4` → 2 = pending), runs a timed wait (`0x90122d64`),
    and on timeout calls `panic(id=9)` (cause 0x2003 is *not* a watchdog — those are
    0x2002/0x4002/0x8002 in reboot_porting.h). **Root cause:** the queue/event the task waits
    on is never produced — the producer needs the **event/message-driven connectivity stack**
    (interrupts/IPC from the BLE/SLE/radio + protocol/BT core), which is the deferred
    connectivity work, not modelled. So **modelling the chip-reset trigger would NOT advance
    the boot — it would reboot-loop into the same panic** (the missing event is structural,
    not first-boot state). This is the expected end of the *single-core stage-1* bring-up:
    the app completes all of kernel init + clock/PMU/eFUSE/calibration, then blocks on the
    connectivity/event layer. Crossing it is the deferred BLE/SLE blob + multi-core/IPC +
    full-peripheral-event work (project north star), a multi-week effort — not a single
    register/decoder fix. WS63 5/5 qtests + BS21 M1 green (this entry is analysis only, no
    code change).

The infrastructure (CPU + xlinx [now incl. prefd + the muliadd imm fix + the ldmia/stmia
bank selector] + memory map + UART/GPIO + SFC + flash1 + the disjoint-range ROM dispatch
+ bs21_rom_call + the mask-ROM signature + the TCXO fix + the GigaDevice flash ID + the
32K-clock-detect status + the eFUSE v151 controller + the TCXO v150 16-bit count layout)
is in place; **flashboot loads + jumps to the app, and the LiteOS app runs its full kernel
init AND all clock/PMU/eFUSE/calibration bring-up crash-free**, then deliberately reboots
the apps core — the BS2X vendor boot chain (loaderboot → flashboot → app) runs end-to-end on
`-M bs21` through the application's **entire single-core stage-1 bring-up** (kernel init +
clock/PMU/eFUSE/calibration), then blocks on the **event/connectivity layer**: a task waits
on a message queue that is never produced, times out, and panics (`panic(id=9)` →
`reboot(core=2, cause=0x2003)`; see §15). That boundary is the deferred BLE/SLE + multi-core
IPC + full-peripheral-event work (the project north star), not a single fix — modelling the
chip reset alone would only reboot-loop into the same panic.
