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
   `0x90128c62 (j .)`. That is LiteOS kernel bring-up (task creation / scheduler /
   memory pools, then the BLE/SLE stack) — the broader connectivity work; each init
   level needs its subsystem satisfied. (WS63 5/5 qtests + BS21 M1 unaffected by the
   decoder change.)

The infrastructure (CPU + xlinx [now incl. prefd] + memory map + UART/GPIO + SFC +
flash1 + the disjoint-range ROM dispatch + bs21_rom_call + the mask-ROM signature +
the TCXO fix + the GigaDevice flash ID) is in place; **flashboot loads + jumps to the
app, and the LiteOS app executes its startup and reaches its init-call table** — the
BS2X vendor boot chain (loaderboot → flashboot → app) runs end-to-end on `-M bs21`
into the application's RTOS init.
