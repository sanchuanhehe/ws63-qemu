/*
 * Shared device models for the HiSilicon "riscv31" core family (WS63, BS21/BS2X).
 *
 * WS63 and BS21 are both HiSilicon RV32IMFC SoCs built on the same "HimiDeer"
 * riscv31 core — identical LOCI local-interrupt architecture + custom CSRs — and
 * the same versioned IP blocks (UART v151, TIMER v150, GPIO v150; register
 * identical, see docs/bs21-recon.md). Only the memory map, peripheral base
 * addresses, IRQ numbers and instance counts differ per chip.
 *
 * This header exposes the shared device-model type names + machine helpers so a
 * per-chip machine file (ws63.c, bs21.c) can instantiate them at its own
 * addresses. The implementations currently live in hw/riscv/ws63.c; the full
 * family split (moving them into hw/riscv/hisi_riscv31.c behind a
 * CONFIG_HISI_RISCV31 source set, plus the ROM-ABI machine callback and the
 * `hisi-riscv31` CPU rename) is deferred to the connectivity-generalization pass.
 * For milestone-1 (BS21 blinky + uart_hello boot) bs21.c reuses them in place —
 * CONFIG_BS21 selects CONFIG_WS63 so ws63.c (and these models) is compiled in.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RISCV_HISI_RISCV31_H
#define HW_RISCV_HISI_RISCV31_H

#include "exec/hwaddr.h"
#include "exec/memory.h"
#include "hw/qdev-core.h"
#include "target/riscv/cpu.h"

/* Shared HiSilicon riscv31 device-model type names (implemented in ws63.c). */
#define TYPE_WS63_INTC   "ws63-intc"
#define TYPE_WS63_UART   "ws63-uart"
#define TYPE_WS63_TIMER  "ws63-timer"
#define TYPE_WS63_GPIO   "ws63-gpio"
#define TYPE_WS63_TCXO   "ws63-tcxo"
/* SFC (serial-flash controller) v150 — models the SPI command interface enough
 * for flash identification/status (RDID -> JEDEC ID, RDSR -> 0). BS21's SFC is
 * the same v150 IP as WS63's, at a different base (0x90000000 vs 0x48000000). */
#define TYPE_WS63_SFC    "ws63-sfc"

/* Map a fresh RAM-backed region @size bytes into @sys at @base. */
void ws63_make_ram(MemoryRegion *sys, MemoryRegion *mr, const char *name,
                   hwaddr base, uint64_t size);

/* Register the HiSilicon custom CSRs (LOCI local-interrupt CSRs + the RAZ/WI
 * vendor CSRs) on the global RISC-V CSR table. Identical for WS63 and BS21;
 * call once from machine init before the CPU runs. */
void ws63_register_custom_csrs(void);

/* Point the shared LOCI interrupt controller at the hart it drives. Call after
 * the intc is realized and the CPU object exists (env is used only at runtime
 * IRQ-delivery time, so post-realize wiring is fine). */
void ws63_intc_set_cpu_env(DeviceState *intc, CPURISCVState *env);

/* Override the shared TCXO model's TCXO_COUNT block offset (default WS63 0x4C0).
 * BS21 places TCXO_COUNT at the region base (TCXO_COUNT_BASE_ADDR 0x57000200), so
 * bs21.c sets this to 0. Call after qdev_new, before realize. */
void ws63_tcxo_set_count_off(DeviceState *dev, uint32_t off);

/* Select the TCXO count-register layout. Default (WS63) returns count[31:0] @+4
 * and count[63:32] @+8. BS21's TCXO v150 splits the count into four 16-bit
 * chunks (count0..3 @+4/+8/+0C/+10); bs21.c enables that. Call before realize. */
void ws63_tcxo_set_chunked16(DeviceState *dev, bool chunked16);

/* Override the JEDEC ID the shared SFC model reports for RDID (default WS63's
 * W25Q16). bs21.c sets the GigaDevice ID the BS2X flashboot expects. */
void ws63_sfc_set_flash_id(DeviceState *dev, uint32_t id);

/* Create + map a DesignWare-SSI SPI controller model with TX->RX loopback at
 * @base (its TX FIFO is looped back to RX, so a blocking transfer reads back what
 * it wrote — same model WS63 uses for spi0/spi1). BS2X's SPI is the same v151 IP
 * as WS63's, so this lets the bs2x machines functionally exercise the (now
 * chip-bs21-enabled) Rust SPI driver. Realizes the device and maps it into system
 * memory; returns it. */
DeviceState *ws63_create_spi_loopback(hwaddr base);

/* Map a minimal BS2X GADC (13-bit ADC v153) model at @base (its digital block,
 * 0x57036000): reports sample-done + a fixed result so the chip-bs21 Rust `gadc`
 * driver completes a conversion. The power/enable handshake (PMU @0x57008700 +
 * AON iso) falls to the GLB absorber — the driver never polls it. */
void ws63_create_gadc(hwaddr base);

/* Map a DesignWare I2C (v151) master model with one slave (@0x50) at @base, so the
 * chip-bs21 Rust I2C driver's bus scan finds exactly one device (present = ACK /
 * STOP_DET; absent = TX_ABRT + addr_7b_noack). */
void ws63_create_i2c(hwaddr base);

/* Map a BS2X KEYSCAN (key-matrix scanner v150) model at @base: once the scan task
 * starts it reports one fixed key (row 2, col 1, pressed) so the chip-bs21 Rust
 * keyscan driver decodes a known event. */
void ws63_create_keyscan(hwaddr base);

/* Map a BS2X QDEC (quadrature decoder v150) model at @base: returns a fixed signed
 * count (-5) + error count (2) so the chip-bs21 Rust qdec driver reads them back. */
void ws63_create_qdec(hwaddr base);

/* Map a BS2X RTC0 (rtc_unified v150) model at @base: a free-running counter with
 * the cnt_req/cnt_lock coherent-read handshake, advancing per read. */
void ws63_create_rtc(hwaddr base);

/* Map a BS2X TRNG (v1) model at @base: FIFO always ready, data varies per read. */
void ws63_create_trng(hwaddr base);

/* Map a BS2X WDT (watchdog v151) shadow model at @base: WDT_CNT reads back the
 * loaded WDT_LOAD so the chip-bs21 wdt driver's counter_value() returns the
 * configured timeout. */
void ws63_create_wdt(hwaddr base);

#endif /* HW_RISCV_HISI_RISCV31_H */
