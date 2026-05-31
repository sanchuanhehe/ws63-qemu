/*
 * QEMU machine model for the HiSilicon WS63 RISC-V SoC.
 *
 * WS63 is a Wi-Fi 6 + BLE + SLE (SparkLink) combo SoC built around a single
 * RV32IMFC_Zicsr hart (hardware single-precision float, NO atomic extension),
 * 240 MHz. This board models enough of the SoC to run bare-metal ws63-rs
 * firmware (https://github.com/sanchuanhehe/ws63-rs).
 *
 * Modeled (real): RV32IMFC CPU, memory map (from ws63-rt/memory.x), custom
 * HiSilicon UART (x3), TIMER (x3, with interrupt), GPIO (x3, output + int regs),
 * SYS_CTL0 clock-status (so clock_init completes), and the interrupt controller
 * (custom LOCIxx CSRs + IRQ routing).
 *
 * Interrupt fidelity: device IRQs 26-31 (TIMER/RTC/I2C0) are standard RISC-V
 * `mie` bits — delivered faithfully via QEMU's mip + vectored mtvec. IRQs >=32
 * (GPIO=33, UART=53, ...) use HiSilicon's in-core CLIC-style vectoring (custom
 * LOCIEN/LOCIPRI CSRs + a 32..72 mcause that does not fit RV32's 32-bit
 * mip/mie); their CSR state is modeled but exact in-core delivery would require
 * a target/riscv patch (documented in docs/design.md). Other peripherals are
 * absorbed (named in the trace by address; see docs/memory-map.md).
 *
 * CPU: a single RV32IMFC_Zicsr hart, built from QEMU's configurable "rv32" core
 * with exactly I/M/F/C enabled and A (atomics) + D (double float) disabled.
 *
 * Copyright (c) 2026 ws63-rs contributors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/misc/unimp.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "target/riscv/cpu.h"
#include "hw/core/cpu.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "exec/address-spaces.h"
#include "elf.h"

/* ----------------------------------------------------------------------------
 * Memory map — from ws63-rt/memory.x, which is a faithful transcription of the
 * C SDK board config (memory_config_common.h). NOT the platform_core.h MPU
 * windows (those are protection regions, a different concept).
 * ------------------------------------------------------------------------- */
#define WS63_BOOTROM_BASE   0x00100000
#define WS63_BOOTROM_SIZE   0x00009000
#define WS63_ROM_BASE       0x00109000
#define WS63_ROM_SIZE       0x00043000
#define WS63_ITCM_BASE      0x0014C000
#define WS63_ITCM_SIZE      0x00004000
#define WS63_DTCM_BASE      0x00180000
#define WS63_DTCM_SIZE      0x00004000
#define WS63_FLASH_BASE     0x00200000
#define WS63_FLASH_SIZE     0x00800000
#define WS63_SRAM_BASE      0x00A00000
#define WS63_SRAM_SIZE      0x00090000
#define WS63_RESET_PC       0x00230300

/* Peripheral bases (WS63.svd). */
#define WS63_SYSCTL0_BASE   0x40000000
#define WS63_SYSCTL0_SIZE   0x00004000
#define WS63_TIMER_BASE     0x44002000
#define WS63_TIMER_SIZE     0x00001000
#define WS63_UART0_BASE     0x44010000
#define WS63_UART1_BASE     0x44011000
#define WS63_UART2_BASE     0x44012000
#define WS63_UART_MMIO_SIZE 0x00001000
#define WS63_GPIO0_BASE     0x44028000
#define WS63_GPIO1_BASE     0x44029000
#define WS63_GPIO2_BASE     0x4402A000
#define WS63_GPIO_SIZE      0x00001000

/* MMIO catch-all windows (low priority — real devices map on top). */
#define WS63_MMIO_LOW_BASE  0x40000000
#define WS63_MMIO_LOW_SIZE  0x10000000
#define WS63_MMIO_SDMA_BASE 0x52000000
#define WS63_MMIO_SDMA_SIZE 0x01000000
#define WS63_MMIO_RTC_BASE  0x57000000
#define WS63_MMIO_RTC_SIZE  0x01000000

/* IRQ numbers (chip_core_irq.h). 26-31 use standard mie bits; >=32 are custom. */
#define WS63_IRQ_TIMER0     26
#define WS63_IRQ_GPIO0      33
#define WS63_IRQ_MAX        73
#define WS63_MIE_IRQ_LO     26
#define WS63_MIE_IRQ_HI     31

/* Custom CSRs (arch_encoding.h): LOCIPRI 0xBC0.., LOCIEN 0xBE0.., LOCIPCLR 0xBF0. */
#define WS63_LOCI_CSR_BASE  0xBC0
#define WS63_LOCI_CSR_END   0xBFF

/* Timer ticks at the 24 MHz reference (functional, not cycle-accurate). */
#define WS63_TIMER_HZ       24000000ULL

/* ============================================================================
 * Forward decls
 * ========================================================================= */
#define TYPE_WS63_INTC "ws63-intc"
OBJECT_DECLARE_SIMPLE_TYPE(WS63IntcState, WS63_INTC)

struct WS63IntcState {
    SysBusDevice parent_obj;
    CPURISCVState *env;            /* target hart (set by the machine) */
    uint32_t loci[0x40];           /* LOCIPRI/PRITHD read-back (0xBC0..0xBFF) */
};

/* Single global so the (context-free) custom-CSR ops can reach the intc. */
static WS63IntcState *g_ws63_intc;

/* ============================================================================
 * Interrupt controller
 *
 * Collects peripheral IRQ lines and delivers them. IRQs 26-31 are raised on the
 * CPU's mip (standard, vectored mtvec delivery). IRQs >=32 are recorded in the
 * custom-CSR pending state; faithful in-core vectored delivery for those needs
 * a target/riscv patch (see file header / docs).
 * ========================================================================= */
static void ws63_intc_set_irq(void *opaque, int n, int level)
{
    WS63IntcState *s = opaque;

    if (n < 0 || n >= WS63_IRQ_MAX) {
        return;
    }
    if (!s->env) {
        return;
    }
    if (n >= WS63_MIE_IRQ_LO && n <= WS63_MIE_IRQ_HI) {
        /* Standard mie bits — QEMU delivers via mip + vectored mtvec. */
        riscv_cpu_update_mip(s->env, 1ull << n, level ? (1ull << n) : 0);
    } else {
        /* Custom local interrupt (>=32): deliver via the target/riscv hook,
         * which sets mcause=irq + vectored mtvec, gated by LOCIEN + mstatus.MIE. */
        riscv_cpu_set_local_irq(s->env, n, level);
    }
}

static void ws63_intc_realize(DeviceState *dev, Error **errp)
{
    WS63IntcState *s = WS63_INTC(dev);

    qdev_init_gpio_in(dev, ws63_intc_set_irq, WS63_IRQ_MAX);
    g_ws63_intc = s;
}

static void ws63_intc_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = ws63_intc_realize;
}

static const TypeInfo ws63_intc_typeinfo = {
    .name          = TYPE_WS63_INTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WS63IntcState),
    .class_init    = ws63_intc_class_init,
};

/* ============================================================================
 * Custom CSRs
 *  - 0xBC0..0xBFF (LOCIPRI/LOCIEN/LOCIPCLR): backed storage (read-back works).
 *  - 0x7C0..0x7FF and 0xFC0..0xFFF (cache-enable etc.): RAZ/WI.
 * Generic RISC-V cores trap on these vendor-custom CSRs; modeling them avoids
 * illegal-instruction faults from WS63 firmware.
 * ========================================================================= */
static RISCVException ws63_csr_any(CPURISCVState *env, int csrno)
{
    return RISCV_EXCP_NONE;
}

static RISCVException ws63_csr_read_zero(CPURISCVState *env, int csrno,
                                         target_ulong *val)
{
    *val = 0;
    return RISCV_EXCP_NONE;
}

static RISCVException ws63_csr_write_ignore(CPURISCVState *env, int csrno,
                                            target_ulong val)
{
    return RISCV_EXCP_NONE;
}

/*
 * Custom local-interrupt CSRs. Enable (LOCIEN0-2 @ 0xBE0-0xBE2) and pending
 * (LOCIPD0-2 @ 0xBE8-0xBEA) map to env->ws63_locien/locipd (bit n = IRQ n):
 *   LOCIEN0/LOCIPD0 -> IRQ 32-63  (word0 bits 32-63)
 *   LOCIEN1/LOCIPD1 -> IRQ 64-95  (word1 bits 0-31)
 *   LOCIEN2/LOCIPD2 -> IRQ 96-127 (word1 bits 32-63)
 * LOCIPCLR (0xBF0): write irq_id to clear its pending bit. LOCIPRI/PRITHD and
 * the rest keep simple read-back storage (priority threshold not yet enforced).
 */
#define WS63_CSR_LOCIEN0  0xBE0
#define WS63_CSR_LOCIEN1  0xBE1
#define WS63_CSR_LOCIEN2  0xBE2
#define WS63_CSR_LOCIPD0  0xBE8
#define WS63_CSR_LOCIPD1  0xBE9
#define WS63_CSR_LOCIPD2  0xBEA
#define WS63_CSR_LOCIPCLR 0xBF0

static RISCVException ws63_loci_read(CPURISCVState *env, int csrno,
                                     target_ulong *val)
{
    switch (csrno) {
    case WS63_CSR_LOCIEN0: *val = (uint32_t)(env->ws63_locien[0] >> 32); break;
    case WS63_CSR_LOCIEN1: *val = (uint32_t)env->ws63_locien[1]; break;
    case WS63_CSR_LOCIEN2: *val = (uint32_t)(env->ws63_locien[1] >> 32); break;
    case WS63_CSR_LOCIPD0: *val = (uint32_t)(env->ws63_locipd[0] >> 32); break;
    case WS63_CSR_LOCIPD1: *val = (uint32_t)env->ws63_locipd[1]; break;
    case WS63_CSR_LOCIPD2: *val = (uint32_t)(env->ws63_locipd[1] >> 32); break;
    default:
        *val = g_ws63_intc ? g_ws63_intc->loci[csrno - WS63_LOCI_CSR_BASE] : 0;
        break;
    }
    return RISCV_EXCP_NONE;
}

static RISCVException ws63_loci_write(CPURISCVState *env, int csrno,
                                      target_ulong val)
{
    uint32_t v = (uint32_t)val;
    switch (csrno) {
    case WS63_CSR_LOCIEN0:
        env->ws63_locien[0] = (env->ws63_locien[0] & 0xFFFFFFFFULL) | ((uint64_t)v << 32);
        riscv_cpu_interrupt(env);
        break;
    case WS63_CSR_LOCIEN1:
        env->ws63_locien[1] = (env->ws63_locien[1] & ~0xFFFFFFFFULL) | v;
        riscv_cpu_interrupt(env);
        break;
    case WS63_CSR_LOCIEN2:
        env->ws63_locien[1] = (env->ws63_locien[1] & 0xFFFFFFFFULL) | ((uint64_t)v << 32);
        riscv_cpu_interrupt(env);
        break;
    case WS63_CSR_LOCIPD0: case WS63_CSR_LOCIPD1: case WS63_CSR_LOCIPD2:
        break; /* pending is read-only; clear via LOCIPCLR */
    case WS63_CSR_LOCIPCLR: {
        int irq = v & 0xFFF;
        if (irq >= 32 && irq < 128) {
            env->ws63_locipd[irq >> 6] &= ~(1ULL << (irq & 63));
            riscv_cpu_interrupt(env);
        }
        break;
    }
    default:
        if (g_ws63_intc) {
            g_ws63_intc->loci[csrno - WS63_LOCI_CSR_BASE] = v;
        }
        break;
    }
    return RISCV_EXCP_NONE;
}

static riscv_csr_operations ws63_csr_razwi = {
    .name = "ws63-custom", .predicate = ws63_csr_any,
    .read = ws63_csr_read_zero, .write = ws63_csr_write_ignore,
};
static riscv_csr_operations ws63_csr_loci = {
    .name = "ws63-loci", .predicate = ws63_csr_any,
    .read = ws63_loci_read, .write = ws63_loci_write,
};

static void ws63_register_custom_csrs(void)
{
    int n;
    for (n = 0x7c0; n <= 0x7ff; n++) {
        riscv_set_csr_ops(n, &ws63_csr_razwi);
    }
    for (n = WS63_LOCI_CSR_BASE; n <= WS63_LOCI_CSR_END; n++) {
        riscv_set_csr_ops(n, &ws63_csr_loci);
    }
    for (n = 0xfc0; n <= 0xfff; n++) {
        riscv_set_csr_ops(n, &ws63_csr_razwi);
    }
}

/* ============================================================================
 * WS63 UART — HiSilicon custom register layout (NOT 16550-compatible).
 * ========================================================================= */
#define TYPE_WS63_UART "ws63-uart"
OBJECT_DECLARE_SIMPLE_TYPE(WS63UartState, WS63_UART)

#define UART_DATA           0x04
#define UART_LINE_STATUS    0x34
#define UART_FIFO_STATUS    0x44
#define FIFO_TX_FULL        (1u << 0)
#define FIFO_TX_EMPTY       (1u << 1)
#define FIFO_RX_FULL        (1u << 2)
#define FIFO_RX_EMPTY       (1u << 3)
#define LSR_DATA_AVAIL      (1u << 0)
#define LSR_TX_READY        (3u << 5)

struct WS63UartState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    CharBackend chr;
    qemu_irq irq;
    uint8_t rx_byte;
    bool rx_valid;
    uint32_t shadow[WS63_UART_MMIO_SIZE / 4];
};

static uint64_t ws63_uart_read(void *opaque, hwaddr off, unsigned size)
{
    WS63UartState *s = opaque;
    switch (off) {
    case UART_FIFO_STATUS: {
        uint32_t v = FIFO_TX_EMPTY;
        v |= s->rx_valid ? FIFO_RX_FULL : FIFO_RX_EMPTY;
        return v;
    }
    case UART_DATA: {
        uint8_t b = s->rx_byte;
        s->rx_valid = false;
        return b;
    }
    case UART_LINE_STATUS:
        return LSR_TX_READY | (s->rx_valid ? LSR_DATA_AVAIL : 0);
    default:
        return s->shadow[(off / 4) % (WS63_UART_MMIO_SIZE / 4)];
    }
}

static void ws63_uart_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    WS63UartState *s = opaque;
    if (off == UART_DATA) {
        uint8_t ch = val & 0xff;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        return;
    }
    s->shadow[(off / 4) % (WS63_UART_MMIO_SIZE / 4)] = (uint32_t)val;
}

static const MemoryRegionOps ws63_uart_ops = {
    .read = ws63_uart_read,
    .write = ws63_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static int ws63_uart_can_rx(void *opaque)
{
    WS63UartState *s = opaque;
    return s->rx_valid ? 0 : 1;
}

static void ws63_uart_rx(void *opaque, const uint8_t *buf, int size)
{
    WS63UartState *s = opaque;
    if (size > 0) {
        s->rx_byte = buf[0];
        s->rx_valid = true;
    }
}

static void ws63_uart_event(void *opaque, QEMUChrEvent event)
{
}

static void ws63_uart_realize(DeviceState *dev, Error **errp)
{
    WS63UartState *s = WS63_UART(dev);
    qemu_chr_fe_set_handlers(&s->chr, ws63_uart_can_rx, ws63_uart_rx,
                             ws63_uart_event, NULL, s, NULL, true);
}

static Property ws63_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", WS63UartState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void ws63_uart_instance_init(Object *obj)
{
    WS63UartState *s = WS63_UART(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->iomem, obj, &ws63_uart_ops, s,
                          TYPE_WS63_UART, WS63_UART_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void ws63_uart_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = ws63_uart_realize;
    device_class_set_props(dc, ws63_uart_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo ws63_uart_typeinfo = {
    .name          = TYPE_WS63_UART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WS63UartState),
    .instance_init = ws63_uart_instance_init,
    .class_init    = ws63_uart_class_init,
};

/* ============================================================================
 * TIMER — 3x DesignWare-style down-counters (dw_apb_timer v150).
 * Per-timer regs at base + 0x100*(i+1): LOAD +0x00, CURRENT +0x08, CONTROL
 * +0x10 (en[0], mode[2:1], int_mask[3]), EOI +0x14, RAW_INTR +0x18.
 * Globals: EOI_REN 0x78, RAW_STAT 0x7C, INTR_STAT 0x80.
 * ========================================================================= */
#define TYPE_WS63_TIMER "ws63-timer"
OBJECT_DECLARE_SIMPLE_TYPE(WS63TimerState, WS63_TIMER)

#define TMR_CONTROL_EN      (1u << 0)
#define TMR_CONTROL_ONESHOT (1u << 1)   /* mode[2:1]==01 */
#define TMR_CONTROL_MASK    (1u << 3)

struct WS63TimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[3];
    QEMUTimer *qt[3];
    uint32_t load[3];
    uint32_t control[3];
    uint32_t raw_intr[3];
    int64_t  start_ns[3];
};

static int64_t ws63_timer_period_ns(uint32_t load)
{
    if (load == 0) {
        load = 1;
    }
    return (int64_t)((load * 1000000000ULL) / WS63_TIMER_HZ);
}

static void ws63_timer_update_irq(WS63TimerState *s, unsigned i)
{
    int level = (s->raw_intr[i] && !(s->control[i] & TMR_CONTROL_MASK)) ? 1 : 0;
    qemu_set_irq(s->irq[i], level);
}

static void ws63_timer_arm(WS63TimerState *s, unsigned i)
{
    s->start_ns[i] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(s->qt[i], s->start_ns[i] + ws63_timer_period_ns(s->load[i]));
}

static void ws63_timer_tick(void *opaque)
{
    WS63TimerState *s = ((WS63TimerState **)opaque)[0];
    unsigned i = (unsigned)(uintptr_t)((void **)opaque)[1];

    s->raw_intr[i] = 1;
    ws63_timer_update_irq(s, i);
    if (s->control[i] & TMR_CONTROL_EN) {
        /* periodic-style reload; one-shot firmware stops it in the ISR */
        ws63_timer_arm(s, i);
    }
}

/* per-channel opaque: a small 2-pointer array {state, index} */
static void *ws63_timer_chan_ctx[3][2];

static uint32_t ws63_timer_current(WS63TimerState *s, unsigned i)
{
    if (!(s->control[i] & TMR_CONTROL_EN)) {
        return 0;
    }
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->start_ns[i];
    uint64_t ticks = (uint64_t)(elapsed) * WS63_TIMER_HZ / 1000000000ULL;
    if (ticks >= s->load[i]) {
        return 0;
    }
    return s->load[i] - (uint32_t)ticks;
}

static uint64_t ws63_timer_read(void *opaque, hwaddr off, unsigned size)
{
    WS63TimerState *s = opaque;

    if (off == 0x7C || off == 0x80) { /* global raw / masked status */
        uint32_t v = 0;
        for (unsigned i = 0; i < 3; i++) {
            uint32_t bit = (off == 0x7C) ? s->raw_intr[i]
                : (s->raw_intr[i] && !(s->control[i] & TMR_CONTROL_MASK));
            v |= (bit ? 1u : 0u) << i;
        }
        return v;
    }
    if (off == 0x78) { /* EOI_REN: read clears all */
        for (unsigned i = 0; i < 3; i++) {
            s->raw_intr[i] = 0;
            ws63_timer_update_irq(s, i);
        }
        return 0;
    }
    if (off >= 0x100 && off < 0x400) {
        unsigned i = (off >> 8) - 1;
        unsigned r = off & 0xFF;
        switch (r) {
        case 0x00: return s->load[i];
        case 0x08: return ws63_timer_current(s, i);
        case 0x10: return s->control[i];
        case 0x14: /* EOI: read clears */
            s->raw_intr[i] = 0;
            ws63_timer_update_irq(s, i);
            return 0;
        case 0x18: return s->raw_intr[i];
        case 0x1C: return (s->raw_intr[i] && !(s->control[i] & TMR_CONTROL_MASK)) ? 1 : 0;
        default: return 0;
        }
    }
    return 0;
}

static void ws63_timer_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    WS63TimerState *s = opaque;

    if (off == 0x78) { /* EOI_REN: write clears all */
        for (unsigned i = 0; i < 3; i++) {
            s->raw_intr[i] = 0;
            ws63_timer_update_irq(s, i);
        }
        return;
    }
    if (off >= 0x100 && off < 0x400) {
        unsigned i = (off >> 8) - 1;
        unsigned r = off & 0xFF;
        switch (r) {
        case 0x00:
            s->load[i] = (uint32_t)val;
            break;
        case 0x10: {
            uint32_t old = s->control[i];
            s->control[i] = (uint32_t)val;
            if (!(old & TMR_CONTROL_EN) && (val & TMR_CONTROL_EN)) {
                ws63_timer_arm(s, i);          /* disabled -> enabled */
            } else if ((old & TMR_CONTROL_EN) && !(val & TMR_CONTROL_EN)) {
                timer_del(s->qt[i]);           /* enabled -> disabled */
            }
            ws63_timer_update_irq(s, i);
            break;
        }
        case 0x14: /* EOI: write clears */
            s->raw_intr[i] = 0;
            ws63_timer_update_irq(s, i);
            break;
        default:
            break;
        }
    }
}

static const MemoryRegionOps ws63_timer_ops = {
    .read = ws63_timer_read,
    .write = ws63_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ws63_timer_realize(DeviceState *dev, Error **errp)
{
    WS63TimerState *s = WS63_TIMER(dev);
    for (unsigned i = 0; i < 3; i++) {
        ws63_timer_chan_ctx[i][0] = (void *)s;
        ws63_timer_chan_ctx[i][1] = (void *)(uintptr_t)i;
        s->qt[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL, ws63_timer_tick,
                                ws63_timer_chan_ctx[i]);
    }
}

static void ws63_timer_instance_init(Object *obj)
{
    WS63TimerState *s = WS63_TIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->iomem, obj, &ws63_timer_ops, s,
                          TYPE_WS63_TIMER, WS63_TIMER_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    for (unsigned i = 0; i < 3; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static void ws63_timer_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = ws63_timer_realize;
}

static const TypeInfo ws63_timer_typeinfo = {
    .name          = TYPE_WS63_TIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WS63TimerState),
    .instance_init = ws63_timer_instance_init,
    .class_init    = ws63_timer_class_init,
};

/* ============================================================================
 * GPIO — output (data set/clear) + input + interrupt registers.
 * 0x00 OUT, 0x04 OEN, 0x0C INT_EN, 0x10 INT_MASK, 0x14 INT_TYPE,
 * 0x18 INT_POL, 0x1C INT_DEDGE, 0x24 INT_RAW(ro), 0x28 INTR(ro),
 * 0x2C INT_EOI(wo, w1c), 0x30 DATA_SET(wo, w1s), 0x34 DATA_CLR(wo, w1c).
 * ========================================================================= */
#define TYPE_WS63_GPIO "ws63-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(WS63GpioState, WS63_GPIO)

struct WS63GpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t out;
    uint32_t oen;
    uint32_t int_en;
    uint32_t int_mask;
    uint32_t int_type;
    uint32_t int_pol;
    uint32_t int_dedge;
    uint32_t int_raw;
};

static uint64_t ws63_gpio_read(void *opaque, hwaddr off, unsigned size)
{
    WS63GpioState *s = opaque;
    switch (off) {
    case 0x00: return s->out;            /* output value (no external input) */
    case 0x04: return s->oen;
    case 0x0C: return s->int_en;
    case 0x10: return s->int_mask;
    case 0x14: return s->int_type;
    case 0x18: return s->int_pol;
    case 0x1C: return s->int_dedge;
    case 0x24: return s->int_raw;
    case 0x28: return s->int_raw & ~s->int_mask;
    default: return 0;
    }
}

/* Re-evaluate the GPIO IRQ line. `old_out` is the pin state before a write; for
 * edge-triggered pins we latch INT_RAW on the matching transition, for
 * level-triggered pins INT_RAW tracks the active level. Output drives input
 * (loopback), so writing an output pin can raise its own interrupt — a
 * self-contained interrupt source (and a reasonable GPIO loopback model). */
static void ws63_gpio_eval(WS63GpioState *s, uint32_t old_out)
{
    uint32_t rose = ~old_out & s->out;
    uint32_t fell = old_out & ~s->out;
    uint32_t edge = (s->int_dedge & (rose | fell))
                  | (~s->int_dedge & s->int_pol & rose)
                  | (~s->int_dedge & ~s->int_pol & fell);
    /* edge-type pins latch on a matching edge */
    s->int_raw |= s->int_en & s->int_type & edge;
    /* level-type pins track the active level */
    uint32_t level_active = (s->int_pol & s->out) | (~s->int_pol & ~s->out);
    uint32_t level_pins = s->int_en & ~s->int_type;
    s->int_raw = (s->int_raw & ~level_pins) | (level_pins & level_active);

    qemu_set_irq(s->irq, (s->int_raw & ~s->int_mask & s->int_en) ? 1 : 0);
}

static void ws63_gpio_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    WS63GpioState *s = opaque;
    uint32_t old_out = s->out;
    switch (off) {
    case 0x00: s->out = (uint32_t)val; ws63_gpio_eval(s, old_out); break;
    case 0x04: s->oen = (uint32_t)val; break;
    case 0x0C: s->int_en = (uint32_t)val; ws63_gpio_eval(s, s->out); break;
    case 0x10: s->int_mask = (uint32_t)val; ws63_gpio_eval(s, s->out); break;
    case 0x14: s->int_type = (uint32_t)val; break;
    case 0x18: s->int_pol = (uint32_t)val; break;
    case 0x1C: s->int_dedge = (uint32_t)val; break;
    case 0x2C: /* INT_EOI (w1c) */
        s->int_raw &= ~(uint32_t)val;
        ws63_gpio_eval(s, s->out);
        break;
    case 0x30: /* DATA_SET (w1s) */
        s->out |= (uint32_t)val;
        qemu_log("ws63-gpio: SET -> out=0x%08x\n", s->out);
        ws63_gpio_eval(s, old_out);
        break;
    case 0x34: /* DATA_CLR (w1c) */
        s->out &= ~(uint32_t)val;
        qemu_log("ws63-gpio: CLR -> out=0x%08x\n", s->out);
        ws63_gpio_eval(s, old_out);
        break;
    default: break;
    }
}

static const MemoryRegionOps ws63_gpio_ops = {
    .read = ws63_gpio_read,
    .write = ws63_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ws63_gpio_instance_init(Object *obj)
{
    WS63GpioState *s = WS63_GPIO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->iomem, obj, &ws63_gpio_ops, s,
                          TYPE_WS63_GPIO, WS63_GPIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->oen = 0xFF; /* reset: all inputs */
}

static const TypeInfo ws63_gpio_typeinfo = {
    .name          = TYPE_WS63_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WS63GpioState),
    .instance_init = ws63_gpio_instance_init,
};

/* ============================================================================
 * SYS_CTL0 — minimal clock-status model so clock_init() completes:
 *   HW_CTL (0x14)        -> TCXO freq code (0 = 24 MHz)
 *   REG_EXCEP_RO_RG(0x319C) bit12 -> PLL locked
 * ========================================================================= */
#define TYPE_WS63_SYSCTL0 "ws63-sysctl0"
OBJECT_DECLARE_SIMPLE_TYPE(WS63SysCtl0State, WS63_SYSCTL0)

struct WS63SysCtl0State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t shadow[WS63_SYSCTL0_SIZE / 4];
};

static uint64_t ws63_sysctl0_read(void *opaque, hwaddr off, unsigned size)
{
    WS63SysCtl0State *s = opaque;
    switch (off) {
    case 0x0014: return 0;            /* HW_CTL: TCXO 24 MHz */
    case 0x319C: return 1u << 12;     /* REG_EXCEP_RO_RG: PLL locked */
    default: return s->shadow[(off / 4) % (WS63_SYSCTL0_SIZE / 4)];
    }
}

static void ws63_sysctl0_write(void *opaque, hwaddr off, uint64_t val,
                               unsigned size)
{
    WS63SysCtl0State *s = opaque;
    s->shadow[(off / 4) % (WS63_SYSCTL0_SIZE / 4)] = (uint32_t)val;
}

static const MemoryRegionOps ws63_sysctl0_ops = {
    .read = ws63_sysctl0_read,
    .write = ws63_sysctl0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ws63_sysctl0_instance_init(Object *obj)
{
    WS63SysCtl0State *s = WS63_SYSCTL0(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->iomem, obj, &ws63_sysctl0_ops, s,
                          TYPE_WS63_SYSCTL0, WS63_SYSCTL0_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const TypeInfo ws63_sysctl0_typeinfo = {
    .name          = TYPE_WS63_SYSCTL0,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WS63SysCtl0State),
    .instance_init = ws63_sysctl0_instance_init,
};

/* ============================================================================
 * WS63 machine
 * ========================================================================= */
#define TYPE_WS63_MACHINE MACHINE_TYPE_NAME("ws63")
OBJECT_DECLARE_SIMPLE_TYPE(WS63MachineState, WS63_MACHINE)

struct WS63MachineState {
    MachineState parent_obj;
    RISCVCPU cpu;
    WS63IntcState intc;
    WS63TimerState timer;
    WS63GpioState gpio[3];
    WS63SysCtl0State sysctl0;
    MemoryRegion bootrom;
    MemoryRegion rom;
    MemoryRegion itcm;
    MemoryRegion dtcm;
    MemoryRegion flash;
};

static void ws63_make_ram(MemoryRegion *sys, MemoryRegion *mr,
                          const char *name, hwaddr base, uint64_t size)
{
    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(sys, base, mr);
}

static void ws63_cpu_reset(void *opaque)
{
    cpu_reset(CPU(opaque));
}

static void ws63_machine_init(MachineState *machine)
{
    WS63MachineState *s = WS63_MACHINE(machine);
    MemoryRegion *sys = get_system_memory();
    uint64_t entry = WS63_RESET_PC;

    ws63_register_custom_csrs();

    /* RAM-backed memory map (SRAM is the -m bank). */
    ws63_make_ram(sys, &s->bootrom, "ws63.bootrom", WS63_BOOTROM_BASE, WS63_BOOTROM_SIZE);
    ws63_make_ram(sys, &s->rom, "ws63.rom", WS63_ROM_BASE, WS63_ROM_SIZE);
    ws63_make_ram(sys, &s->itcm, "ws63.itcm", WS63_ITCM_BASE, WS63_ITCM_SIZE);
    ws63_make_ram(sys, &s->dtcm, "ws63.dtcm", WS63_DTCM_BASE, WS63_DTCM_SIZE);
    ws63_make_ram(sys, &s->flash, "ws63.flash", WS63_FLASH_BASE, WS63_FLASH_SIZE);
    memory_region_add_subregion(sys, WS63_SRAM_BASE, machine->ram);

    /* Catch-all absorbers (low priority) for un-modeled peripheral MMIO. */
    create_unimplemented_device("ws63.mmio.periph", WS63_MMIO_LOW_BASE, WS63_MMIO_LOW_SIZE);
    create_unimplemented_device("ws63.mmio.sdma", WS63_MMIO_SDMA_BASE, WS63_MMIO_SDMA_SIZE);
    create_unimplemented_device("ws63.mmio.rtc", WS63_MMIO_RTC_BASE, WS63_MMIO_RTC_SIZE);

    /* Interrupt controller. */
    object_initialize_child(OBJECT(machine), "intc", &s->intc, TYPE_WS63_INTC);
    sysbus_realize(SYS_BUS_DEVICE(&s->intc), &error_fatal);

    /* SYS_CTL0 clock status (over the absorber). */
    object_initialize_child(OBJECT(machine), "sysctl0", &s->sysctl0, TYPE_WS63_SYSCTL0);
    sysbus_realize(SYS_BUS_DEVICE(&s->sysctl0), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysctl0), 0, WS63_SYSCTL0_BASE);

    /* TIMER (IRQ 26/27/28). */
    object_initialize_child(OBJECT(machine), "timer", &s->timer, TYPE_WS63_TIMER);
    sysbus_realize(SYS_BUS_DEVICE(&s->timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->timer), 0, WS63_TIMER_BASE);
    for (int i = 0; i < 3; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->timer), i,
                           qdev_get_gpio_in(DEVICE(&s->intc), WS63_IRQ_TIMER0 + i));
    }

    /* GPIO0/1/2 (IRQ 33/34/35). */
    const hwaddr gpio_base[3] = { WS63_GPIO0_BASE, WS63_GPIO1_BASE, WS63_GPIO2_BASE };
    for (int i = 0; i < 3; i++) {
        char name[16];
        snprintf(name, sizeof(name), "gpio%d", i);
        object_initialize_child(OBJECT(machine), name, &s->gpio[i], TYPE_WS63_GPIO);
        sysbus_realize(SYS_BUS_DEVICE(&s->gpio[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio[i]), 0, gpio_base[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpio[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->intc), WS63_IRQ_GPIO0 + i));
    }

    /* UART0/1/2 (custom device on top of the absorber). */
    const hwaddr uart_base[3] = { WS63_UART0_BASE, WS63_UART1_BASE, WS63_UART2_BASE };
    for (int i = 0; i < 3; i++) {
        DeviceState *uart = qdev_new(TYPE_WS63_UART);
        qdev_prop_set_chr(uart, "chardev", serial_hd(i));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, uart_base[i]);
    }

    /* Firmware ELF (-kernel). Entry overrides the default reset PC. */
    if (machine->kernel_filename) {
        uint64_t elf_entry;
        if (load_elf(machine->kernel_filename, NULL, NULL, NULL, &elf_entry,
                     NULL, NULL, NULL, 0, EM_RISCV, 0, 0) <= 0) {
            error_report("ws63: could not load kernel ELF '%s'",
                         machine->kernel_filename);
            exit(1);
        }
        entry = elf_entry;
    }

    /* Single RV32IMFC_Zicsr hart (I/M/F/C; A/D/zawrs off). */
    object_initialize_child(OBJECT(machine), "cpu", &s->cpu, machine->cpu_type);
    object_property_set_bool(OBJECT(&s->cpu), "i", true, &error_abort);
    object_property_set_bool(OBJECT(&s->cpu), "m", true, &error_abort);
    object_property_set_bool(OBJECT(&s->cpu), "f", true, &error_abort);
    object_property_set_bool(OBJECT(&s->cpu), "c", true, &error_abort);
    object_property_set_bool(OBJECT(&s->cpu), "a", false, &error_abort);
    object_property_set_bool(OBJECT(&s->cpu), "d", false, &error_abort);
    object_property_set_bool(OBJECT(&s->cpu), "zawrs", false, &error_abort);
    qdev_prop_set_uint64(DEVICE(&s->cpu), "resetvec", entry);
    s->cpu.env.mhartid = 0;
    qemu_register_reset(ws63_cpu_reset, &s->cpu);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_fatal);

    /* Let the interrupt controller drive this hart. */
    s->intc.env = &s->cpu.env;
}

static void ws63_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "HiSilicon WS63 (RV32IMFC Wi-Fi6/BLE/SLE SoC)";
    mc->init = ws63_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE32;
    mc->default_ram_id = "ws63.sram";
    mc->default_ram_size = WS63_SRAM_SIZE;
}

static const TypeInfo ws63_machine_typeinfo = {
    .name          = TYPE_WS63_MACHINE,
    .parent        = TYPE_MACHINE,
    .class_init    = ws63_machine_class_init,
    .instance_size = sizeof(WS63MachineState),
};

static void ws63_register_types(void)
{
    type_register_static(&ws63_intc_typeinfo);
    type_register_static(&ws63_uart_typeinfo);
    type_register_static(&ws63_timer_typeinfo);
    type_register_static(&ws63_gpio_typeinfo);
    type_register_static(&ws63_sysctl0_typeinfo);
    type_register_static(&ws63_machine_typeinfo);
}

type_init(ws63_register_types)
