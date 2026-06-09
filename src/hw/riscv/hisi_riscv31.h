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

#endif /* HW_RISCV_HISI_RISCV31_H */
