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
loaderboot") and set the reset PC there. **flashboot uses a different format**
(magic = ImageId `0x4b1e3c1e`, code-info as a *trailer* `0x4b1e3c2d` at the tail) —
not yet cracked.

## Memory-map fix (found by running firmware)

The loaderboot reset code copies the boot-param block to **`0x20000000`** — the
real BS21 DTCM (`APP_DTCM_ORIGIN`, len `0x10000`). `bs21.c` had wrongly placed
DTCM at `0xF0000`; loaderboot faulted writing to `0x20002d50`. Fixed: DTCM →
`0x20000000`, ITCM window → `0x80000..0x100000`. (M1 + WS63 qtests unaffected.)

## BS21 ROM table (for a future `bs21_rom_call`)

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

1. **BS21 ROM-call table** — implement `bs21_rom_call` (addresses above) so the
   later stages' secure-libc / SFC calls are emulated.
2. **flashboot image format** — the trailer-based variant (above).
3. **SFC + mask-ROM stubs** — flash controller + the boot-stage hand-off.

The infrastructure (CPU + xlinx + memory map + UART/GPIO + the disjoint-range ROM
dispatch) is in place; vendor firmware already *runs*.
