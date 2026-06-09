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
4. **Flash CONTENT** — the next gate: after flash-ID, flashboot reads the partition
   table from flash (via the SFC command interface, into DTCM `0x20000c34`) and
   checks its first word against the magic **`0x4b87a52d`** (loaded by an xlinx
   `l.li`). Empty flash → 0 ≠ magic → it halts. To proceed it needs the SFC's READ
   command backed by a flash image containing a valid partition table (magic
   `0x4b87a52d`) + the application — i.e. loading the real firmware. That (+ the app
   image) is the remaining "boot the vendor app" chain.

The infrastructure (CPU + xlinx + memory map + UART/GPIO + the disjoint-range ROM
dispatch + bs21_rom_call) is in place; both loaderboot and flashboot *run* — the
remaining work is the SFC/flash so flashboot can load and start the application.
