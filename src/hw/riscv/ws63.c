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
#include "net/net.h"   /* synthetic Wi-Fi/Ethernet MAC (ws63-netmac) */
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "target/riscv/cpu.h"
#include "hw/core/cpu.h"
#include "system/system.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "exec/address-spaces.h"
#include "elf.h"
#include "trace.h"     /* generated from hw/riscv/trace-events (ws63_* events) */
#include "hisi_riscv31.h" /* shared device-model type names + helpers (reused by bs21.c) */

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
/* riscv31 core private peripheral bus: FlashPatch (0xE0000000) + SCS (0xE000E000) */
#define WS63_PPB_BASE       0xE0000000
#define WS63_PPB_SIZE       0x00010000

/* IRQ numbers (chip_core_irq.h). 26-31 use standard mie bits; >=32 are custom. */
#define WS63_IRQ_TIMER0     26
#define WS63_IRQ_RTC        29
#define WS63_IRQ_I2C0       31
#define WS63_IRQ_I2C1       32
#define WS63_IRQ_GPIO0      33
#define WS63_IRQ_SPI0       43
#define WS63_IRQ_WLMAC      45
#define WS63_IRQ_I2S        51
#define WS63_IRQ_SPI1       52
#define WS63_IRQ_UART0      53
#define WS63_IRQ_DMA        59
#define WS63_IRQ_LSADC      72
#define WS63_IRQ_MAX        73
/* Lines 0..31 are delivered on the CPU's mip (standard vectored mtvec); >=32 are
 * the custom LOCI interrupts. WS63 only wires peripherals at 26..31 (+ LOCI), but
 * BS21's LiteOS tick uses the standard machine-timer interrupt MTIP = mip bit 7
 * (TIMER_3 routed to it), so the mip range must include the low bits too. WS63
 * uses none of lines 0..25, so widening the range is a no-op for it. */
#define WS63_MIE_IRQ_LO     0
#define WS63_MIE_IRQ_HI     31

/* Custom CSRs (arch_encoding.h): LOCIPRI 0xBC0.., LOCIEN 0xBE0.., LOCIPCLR 0xBF0. */
#define WS63_LOCI_CSR_BASE  0xBC0
#define WS63_LOCI_CSR_END   0xBFF

/* 24 MHz TCXO crystal reference. The WS63 timers (and WDT) count at this crystal
 * rate, NOT the PLL — see ws63_periph_clk_hz(). 40 MHz-crystal boards set 40 MHz
 * via g_ws63_tcxo_hz (HW_CTL bit0). */
#define WS63_TCXO_HZ        24000000ULL

/* ============================================================================
 * Forward decls
 * ========================================================================= */
/* TYPE_WS63_INTC defined in hisi_riscv31.h (shared with bs21.c). */
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

/* Exposed via hisi_riscv31.h: let a machine point the intc at its hart after the
 * CPU is realized (env is consulted only at IRQ-delivery time). Used by bs21.c;
 * ws63.c sets s->intc.env directly since it owns the struct. */
void ws63_intc_set_cpu_env(DeviceState *intc, CPURISCVState *env)
{
    WS63_INTC(intc)->env = env;
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
        /* Mirror priority config into the CPU so ws63_local_irq_pending() can
         * enforce it: LOCIPRI0-15 (0xBC0-0xBCF) + PRITHD (0xBFE). */
        if (csrno >= 0xBC0 && csrno <= 0xBCF) {
            env->ws63_locipri[csrno - 0xBC0] = v;
            riscv_cpu_interrupt(env);
        } else if (csrno == 0xBFE) {
            env->ws63_prithd = v & 0xF;
            riscv_cpu_interrupt(env);
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

/* Exposed via hisi_riscv31.h so bs21.c reuses the identical LOCI/vendor CSRs. */
void ws63_register_custom_csrs(void)
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
/* TYPE_WS63_UART defined in hisi_riscv31.h (shared with bs21.c). */
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
        qemu_set_irq(s->irq, 0);        /* RX consumed -> de-assert */
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
        /* raise RX IRQ if the firmware enabled UART interrupts (INTR_EN @ 0x18);
         * polled firmware leaves INTR_EN=0 and is unaffected. */
        if (s->shadow[0x18 / 4]) {
            qemu_set_irq(s->irq, 1);
        }
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

static const Property ws63_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", WS63UartState, chr),
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
/* TYPE_WS63_TIMER defined in hisi_riscv31.h (shared with bs21.c). */
OBJECT_DECLARE_SIMPLE_TYPE(WS63TimerState, WS63_TIMER)

#define TMR_CONTROL_EN      (1u << 0)
#define TMR_CONTROL_ONESHOT (1u << 1)   /* mode[2:1]==01 */
#define TMR_CONTROL_MASK    (1u << 3)

/* dw_apb timer: WS63 uses 3 channels (TIMER_0..2); BS21 has a 4th (TIMER_3 @
 * +0x400) that drives the LiteOS tick. Model 4 — the extra channel is inert on
 * WS63 (never enabled, raw_intr stays 0, so the global status bit 3 reads 0). */
#define WS63_TIMER_CHANNELS 4
struct WS63TimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[WS63_TIMER_CHANNELS];
    QEMUTimer *qt[WS63_TIMER_CHANNELS];
    uint32_t load[WS63_TIMER_CHANNELS];
    uint32_t control[WS63_TIMER_CHANNELS];
    uint32_t raw_intr[WS63_TIMER_CHANNELS];
    int64_t  start_ns[WS63_TIMER_CHANNELS];
};

/*
 * Clock tree. PCLK = the PLL (240 MHz) once locked, else the TCXO (24/40 MHz per
 * SYS_CTL0 HW_CTL); the SoC locks the PLL early in boot so PCLK is normally
 * 240 MHz (not the old fixed 24 MHz). Each peripheral domain is then (a) gated by
 * a CLDO_CRG_CKEN bit and (b) for UART/SPI/I2C, sourced from TCXO or a PLL-derived
 * clock by a CLDO_CRG_CLK_SEL bit. Gates default ON: the rs timer HAL never sets
 * the timer gate and relies on it being clocked, so defaulting off would wrongly
 * freeze it. Clearing a gate freezes that domain's clock. The timer has no source
 * select (always PCLK); UART/SPI rates are not behaviorally observable (QEMU's
 * chardev links aren't rate-limited), so CLK_SEL is tracked but only the timer
 * gate is observable.
 */
#define WS63_UART_PLL_HZ   160000000ULL          /* UART/SPI PLL-derived clock */
#define WS63_CKEN0_TIMER   21                    /* CLDO_CRG_CKEN_CTL0 bit (per rs HAL) */
static uint32_t g_ws63_tcxo_hz    = WS63_TCXO_HZ;/* HW_CTL bit0: 0=24 MHz, 1=40 MHz */
static uint32_t g_ws63_cken0      = 0xFFFFFFFFu; /* CLDO_CRG_CKEN_CTL0 @0x44001100 */
static uint32_t g_ws63_cken1      = 0xFFFFFFFFu; /* CLDO_CRG_CKEN_CTL1 @0x44001104 */
static uint32_t g_ws63_clk_sel    = 0;           /* CLDO_CRG_CLK_SEL   @0x44001134 */
static WS63TimerState *g_ws63_timer;             /* for re-arm on gate toggle */

/*
 * Effective clock for a peripheral domain: 0 if its CKEN gate is off, else its
 * source rate. cken_reg: 0=CKEN_CTL0, 1=CKEN_CTL1. sel_bit < 0 => the TCXO crystal
 * (the timers count at the crystal, NOT the PLL — verified against fbb_ws63
 * clock_init.c: timer_porting_clock_value_set(REQ_24M)); else the CLK_SEL bit picks
 * the PLL-derived (1) vs TCXO (0) source.
 */
static uint64_t ws63_periph_clk_hz(int cken_reg, int cken_bit, int sel_bit)
{
    uint32_t cken = cken_reg ? g_ws63_cken1 : g_ws63_cken0;
    if (!(cken & (1u << cken_bit))) {
        return 0;                                /* clock gated off */
    }
    if (sel_bit < 0) {
        return g_ws63_tcxo_hz;                   /* timer: TCXO crystal (24/40 MHz) */
    }
    return (g_ws63_clk_sel & (1u << sel_bit)) ? WS63_UART_PLL_HZ : g_ws63_tcxo_hz;
}

static uint64_t ws63_timer_clk_hz(void)
{
    return ws63_periph_clk_hz(0, WS63_CKEN0_TIMER, -1);
}

static bool ws63_timer_clock_gated(void)
{
    return ws63_timer_clk_hz() == 0;
}

static int64_t ws63_timer_period_ns(uint32_t load)
{
    uint64_t hz = ws63_timer_clk_hz();
    if (hz == 0) {
        return -1;                               /* gated off -> not running */
    }
    if (load == 0) {
        load = 1;
    }
    return (int64_t)((load * 1000000000ULL) / hz);
}

static void ws63_timer_update_irq(WS63TimerState *s, unsigned i)
{
    int level = (s->raw_intr[i] && !(s->control[i] & TMR_CONTROL_MASK)) ? 1 : 0;
    qemu_set_irq(s->irq[i], level);
}

static void ws63_timer_arm(WS63TimerState *s, unsigned i)
{
    int64_t period = ws63_timer_period_ns(s->load[i]);
    if (period < 0) {
        timer_del(s->qt[i]);            /* clock gated off -> stay stopped */
        return;
    }
    s->start_ns[i] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(s->qt[i], s->start_ns[i] + period);
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
static void *ws63_timer_chan_ctx[WS63_TIMER_CHANNELS][2];

static uint32_t ws63_timer_current(WS63TimerState *s, unsigned i)
{
    if (!(s->control[i] & TMR_CONTROL_EN)) {
        return 0;
    }
    uint64_t hz = ws63_timer_clk_hz();
    if (hz == 0) {
        return s->load[i];              /* clock gated off -> frozen at reload */
    }
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->start_ns[i];
    uint64_t ticks = (uint64_t)(elapsed) * hz / 1000000000ULL;
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
        for (unsigned i = 0; i < WS63_TIMER_CHANNELS; i++) {
            uint32_t bit = (off == 0x7C) ? s->raw_intr[i]
                : (s->raw_intr[i] && !(s->control[i] & TMR_CONTROL_MASK));
            v |= (bit ? 1u : 0u) << i;
        }
        return v;
    }
    if (off == 0x78) { /* EOI_REN: read clears all */
        for (unsigned i = 0; i < WS63_TIMER_CHANNELS; i++) {
            s->raw_intr[i] = 0;
            ws63_timer_update_irq(s, i);
        }
        return 0;
    }
    if (off >= 0x100 && off < 0x500) {
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
        for (unsigned i = 0; i < WS63_TIMER_CHANNELS; i++) {
            s->raw_intr[i] = 0;
            ws63_timer_update_irq(s, i);
        }
        return;
    }
    if (off >= 0x100 && off < 0x500) {
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
    g_ws63_timer = s;                   /* for clock-gate re-arm from CLDO_CRG */
    for (unsigned i = 0; i < WS63_TIMER_CHANNELS; i++) {
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
    for (unsigned i = 0; i < WS63_TIMER_CHANNELS; i++) {
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
/* TYPE_WS63_GPIO defined in hisi_riscv31.h (shared with bs21.c). */
OBJECT_DECLARE_SIMPLE_TYPE(WS63GpioState, WS63_GPIO)

#define WS63_GPIO_PINS 8        /* pins exposed as external signal nets */

struct WS63GpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq out_pin[WS63_GPIO_PINS]; /* output pin nets (drive other pins/devices) */
    uint32_t out;
    uint32_t ext_in;        /* level driven onto pins from outside the bank */
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
    case 0x00: return s->out | s->ext_in; /* pin level: our output | external drive */
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
static void ws63_gpio_eval(WS63GpioState *s, uint32_t old_eff)
{
    uint32_t eff = s->out | s->ext_in;  /* pin level = our output | external drive */
    uint32_t rose = ~old_eff & eff;
    uint32_t fell = old_eff & ~eff;
    uint32_t edge = (s->int_dedge & (rose | fell))
                  | (~s->int_dedge & s->int_pol & rose)
                  | (~s->int_dedge & ~s->int_pol & fell);
    /* edge-type pins latch on a matching edge */
    s->int_raw |= s->int_en & s->int_type & edge;
    /* level-type pins track the active level */
    uint32_t level_active = (s->int_pol & eff) | (~s->int_pol & ~eff);
    uint32_t level_pins = s->int_en & ~s->int_type;
    s->int_raw = (s->int_raw & ~level_pins) | (level_pins & level_active);

    qemu_set_irq(s->irq, (s->int_raw & ~s->int_mask & s->int_en) ? 1 : 0);

    /* drive our output pins onto their nets (board-level wiring to other pins) */
    for (int i = 0; i < WS63_GPIO_PINS; i++) {
        qemu_set_irq(s->out_pin[i], (s->out >> i) & 1);
    }
}

/* external source drives one of our input pins (board wiring / another device) */
static void ws63_gpio_set_in(void *opaque, int n, int level)
{
    WS63GpioState *s = opaque;
    uint32_t old_eff = s->out | s->ext_in;
    if (level) {
        s->ext_in |= (1u << n);
    } else {
        s->ext_in &= ~(1u << n);
    }
    ws63_gpio_eval(s, old_eff);
}

static void ws63_gpio_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    WS63GpioState *s = opaque;
    uint32_t old_eff = s->out | s->ext_in;
    switch (off) {
    case 0x00: s->out = (uint32_t)val; ws63_gpio_eval(s, old_eff); break;
    case 0x04: s->oen = (uint32_t)val; break;
    case 0x0C: s->int_en = (uint32_t)val; ws63_gpio_eval(s, old_eff); break;
    case 0x10: s->int_mask = (uint32_t)val; ws63_gpio_eval(s, old_eff); break;
    case 0x14: s->int_type = (uint32_t)val; break;
    case 0x18: s->int_pol = (uint32_t)val; break;
    case 0x1C: s->int_dedge = (uint32_t)val; break;
    case 0x2C: /* INT_EOI (w1c) */
        s->int_raw &= ~(uint32_t)val;
        ws63_gpio_eval(s, old_eff);
        break;
    case 0x30: /* DATA_SET (w1s) */
        s->out |= (uint32_t)val;
        trace_ws63_gpio_set(s->out);
        ws63_gpio_eval(s, old_eff);
        break;
    case 0x34: /* DATA_CLR (w1c) */
        s->out &= ~(uint32_t)val;
        trace_ws63_gpio_clr(s->out);
        ws63_gpio_eval(s, old_eff);
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
    /* pin signal nets: inputs driven from outside (board wiring / monitor),
     * outputs that can drive other pins/devices. */
    qdev_init_gpio_in(DEVICE(obj), ws63_gpio_set_in, WS63_GPIO_PINS);
    qdev_init_gpio_out(DEVICE(obj), s->out_pin, WS63_GPIO_PINS);
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

/*
 * Reset model (fbb_ws63 porting/reboot/reboot_porting.c). GLB_CTL_M lives in
 * the SYS_CTL0 window, so its chip-reset trigger and the reset-reason history
 * record are handled here:
 *   0x2110 bit 2  -> chip reset  (HAL_CHIP_RESET_REG, reg32_setbit(...,2))
 *   0x00A0        -> SYS_RST_RECORD_0  (reset-reason history, read)
 *   0x00A4        -> SYS_DIAG_CLR_1    (write 1s to clear matched history bits)
 * The record is a host-side static so it survives qemu_system_reset_request (a
 * guest reset re-inits device state but not this), letting the firmware read
 * back "software reset" (bit 1) on the next boot.
 */
#define WS63_GLB_CHIP_RESET_OFF  0x2110
#define WS63_GLB_CHIP_RESET_BIT  (1u << 2)
#define WS63_SYS_RST_RECORD_OFF  0x00A0
#define WS63_SYS_DIAG_CLR_OFF    0x00A4
#define WS63_SYS_SOFT_RST_HIS    0x2
static uint32_t g_ws63_rst_record;

static uint64_t ws63_sysctl0_read(void *opaque, hwaddr off, unsigned size)
{
    WS63SysCtl0State *s = opaque;
    switch (off) {
    case 0x0014: return 0;            /* HW_CTL: TCXO 24 MHz */
    case 0x319C: return 1u << 12;     /* REG_EXCEP_RO_RG: PLL locked */
    case WS63_SYS_RST_RECORD_OFF: return g_ws63_rst_record;
    default: return s->shadow[(off / 4) % (WS63_SYSCTL0_SIZE / 4)];
    }
}

static void ws63_sysctl0_write(void *opaque, hwaddr off, uint64_t val,
                               unsigned size)
{
    WS63SysCtl0State *s = opaque;
    if (off == WS63_GLB_CHIP_RESET_OFF && (val & WS63_GLB_CHIP_RESET_BIT)) {
        /* software_reset(): record the cause, then reboot the machine. */
        g_ws63_rst_record |= WS63_SYS_SOFT_RST_HIS;
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }
    if (off == WS63_SYS_DIAG_CLR_OFF) {
        g_ws63_rst_record &= ~(uint32_t)val;
        return;
    }
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
 * TCXO clock/counter — the bootloaders (flashboot/loaderboot) call hal_tcxo_init/
 * hal_tcxo_get during early clock bring-up: they enable the TCXO via TCXO_COUNT
 * (0x440004C0) and then poll bit4 ("count valid") and read a free-running counter
 * for us-resolution timekeeping. Model bit4 as always-set and back the count with
 * the QEMU virtual clock at the nominal 24 MHz so delays/timeouts terminate.
 * ========================================================================= */
/* TYPE_WS63_TCXO defined in hisi_riscv31.h (shared with bs21.c). */
OBJECT_DECLARE_SIMPLE_TYPE(WS63TcxoState, WS63_TCXO)

#define WS63_TCXO_BASE      0x44000000
#define WS63_TCXO_SIZE      0x00001000
#define WS63_TCXO_COUNT_OFF 0x04C0      /* TCXO_COUNT_BASE_ADDR - base */
#define WS63_TCXO_HZ        24000000ULL

struct WS63TcxoState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint64_t count;     /* 64-bit free-running TCXO tick counter */
    uint32_t count_off; /* offset of the TCXO_COUNT block within the region:
                         * 0x4C0 on WS63 (TCXO @0x44000000); 0x000 on BS21 where
                         * TCXO_COUNT_BASE_ADDR is the region base (0x57000200).
                         * Set via ws63_tcxo_set_count_off() before realize. */
    uint32_t chunked16; /* count register layout. 0 (WS63): count[31:0] @+4,
                         * count[63:32] @+8. 1 (BS21 TCXO v150): the count is
                         * split into four 16-bit chunks — count0[15:0] @+4,
                         * count1[31:16] @+8, count2[47:32] @+0C, count3[63:48]
                         * @+10 (hal_tcxo_v150_regs_def.h). Set via
                         * ws63_tcxo_set_chunked16() before realize. */
    uint64_t latched;   /* chunked16: count snapshot taken on the refresh write
                         * (status bit0). The v150 read is refresh -> poll valid ->
                         * read count0..3, and the firmware reads the chunks
                         * high-to-low, so all four must come from one snapshot;
                         * advancing per-chunk would tear the value and stall every
                         * tcxo delay (count >= target never holds). */
    uint32_t shadow[WS63_TCXO_SIZE / 4];
};

/* Override the TCXO_COUNT block offset (default WS63's 0x4C0). bs21.c calls this
 * so the count status/lo/hi sit at the region base, matching TCXO_COUNT_BASE_ADDR. */
void ws63_tcxo_set_count_off(DeviceState *dev, uint32_t off)
{
    WS63_TCXO(dev)->count_off = off;
}

void ws63_tcxo_set_chunked16(DeviceState *dev, bool chunked16)
{
    WS63_TCXO(dev)->chunked16 = chunked16;
}

/*
 * Advance the 64-bit counter. Track the QEMU virtual clock at the nominal 24 MHz
 * for rough realism, but ALWAYS advance by at least a step on each sample: in TCG
 * without icount the virtual clock barely moves inside a tight MMIO-poll loop, so
 * a purely clock-derived counter would freeze and TCXO delay loops would never
 * terminate. The guaranteed step makes delays elapse after a bounded # of reads.
 */
static uint64_t ws63_tcxo_tick(WS63TcxoState *s)
{
    uint64_t clk = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) * WS63_TCXO_HZ
                 / 1000000000ULL;
    s->count = (clk > s->count + 64) ? clk : s->count + 64;
    return s->count;
}

static uint64_t ws63_tcxo_read(void *opaque, hwaddr off, unsigned size)
{
    WS63TcxoState *s = opaque;
    if (off == s->count_off) {              /* +0x00: status, bit4 = count valid */
        return 0x10;
    }
    if (s->chunked16) {
        /* BS21 TCXO v150: count split into four 16-bit chunks, all read from the
         * snapshot latched by the most recent refresh write (ws63_tcxo_write). */
        if (off == s->count_off + 4) {       /* count0: count[15:0]  */
            return (uint32_t)(s->latched & 0xffff);
        } else if (off == s->count_off + 8) { /* count1: count[31:16] */
            return (uint32_t)((s->latched >> 16) & 0xffff);
        } else if (off == s->count_off + 0xc) { /* count2: count[47:32] */
            return (uint32_t)((s->latched >> 32) & 0xffff);
        } else if (off == s->count_off + 0x10) { /* count3: count[63:48] */
            return (uint32_t)((s->latched >> 48) & 0xffff);
        }
    } else if (off == s->count_off + 4) {   /* +0x04: count[31:0] (advance here) */
        return (uint32_t)ws63_tcxo_tick(s);
    } else if (off == s->count_off + 8) {   /* +0x08: count[63:32] */
        return (uint32_t)(s->count >> 32);
    }
    return s->shadow[(off / 4) % (WS63_TCXO_SIZE / 4)];
}

static void ws63_tcxo_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    WS63TcxoState *s = opaque;
    /* chunked16 (BS21 v150): a write to the status register with the refresh bit
     * (bit0) set takes a fresh count snapshot for the count0..3 reads. */
    if (s->chunked16 && off == s->count_off && (val & 0x1)) {
        s->latched = ws63_tcxo_tick(s);
    }
    s->shadow[(off / 4) % (WS63_TCXO_SIZE / 4)] = (uint32_t)val;
}

static const MemoryRegionOps ws63_tcxo_ops = {
    .read = ws63_tcxo_read,
    .write = ws63_tcxo_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ws63_tcxo_instance_init(Object *obj)
{
    WS63TcxoState *s = WS63_TCXO(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    s->count_off = WS63_TCXO_COUNT_OFF;     /* WS63 default; bs21.c overrides to 0 */
    memory_region_init_io(&s->iomem, obj, &ws63_tcxo_ops, s,
                          TYPE_WS63_TCXO, WS63_TCXO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const TypeInfo ws63_tcxo_typeinfo = {
    .name          = TYPE_WS63_TCXO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WS63TcxoState),
    .instance_init = ws63_tcxo_instance_init,
};

/* ============================================================================
 * SFC (Serial Flash Controller, 0x48000000) — the bootloaders and app probe the
 * SPI flash through the command interface (cmd_ins=opcode @0x308, cmd_config
 * start bit @0x300, data in cmd_databuf @0x400). We model just enough of the SPI
 * command protocol for flash identification/status so flash init succeeds:
 * RDID(0x9F) -> a supported JEDEC ID (W25Q16, 0x1560EF), RDSR(0x05/35/15) -> 0
 * (ready, unprotected); the start bit auto-clears (command "completes").
 * ========================================================================= */
#define TYPE_WS63_SFC "ws63-sfc"
OBJECT_DECLARE_SIMPLE_TYPE(WS63SfcState, WS63_SFC)

#define WS63_SFC_BASE       0x48000000
#define WS63_SFC_SIZE       0x00001000
#define SFC_CMD_CONFIG      0x300   /* bit0 = start (auto-clears when done) */
#define SFC_CMD_INS         0x308   /* bits[7:0] = SPI opcode */
#define SFC_CMD_DATABUF0    0x400
#define SFC_FLASH_ID_W25Q16 0x001560EF   /* Winbond (WS63 default) */
#define SFC_FLASH_ID_GD25Q80C 0x001440C8 /* GigaDevice (BS21: FLASH_MANUFACTURER_GIGADEVICE_GD25Q80C) */

struct WS63SfcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t flash_id;  /* JEDEC ID returned for RDID (0x9F): packed cap<<16|type<<8|mfr.
                         * WS63 = W25Q16; BS21 = GD25Q80C. Set via ws63_sfc_set_flash_id(). */
    uint32_t shadow[WS63_SFC_SIZE / 4];
};

/* Override the JEDEC ID the SFC reports for RDID (default WS63's W25Q16). bs21.c
 * sets the GigaDevice ID the BS2X flashboot's flash detection expects. */
void ws63_sfc_set_flash_id(DeviceState *dev, uint32_t id)
{
    WS63_SFC(dev)->flash_id = id;
}

static uint64_t ws63_sfc_read(void *opaque, hwaddr off, unsigned size)
{
    WS63SfcState *s = opaque;
    return s->shadow[(off / 4) % (WS63_SFC_SIZE / 4)];
}

static void ws63_sfc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    WS63SfcState *s = opaque;

    if (off == SFC_CMD_CONFIG && (val & 0x1)) {
        uint8_t op = s->shadow[SFC_CMD_INS / 4] & 0xff;
        switch (op) {
        case 0x9f: /* RDID */
            s->shadow[SFC_CMD_DATABUF0 / 4] = s->flash_id;
            break;
        case 0x05: /* RDSR */
        case 0x35: /* RDSR2 */
        case 0x15: /* RDSR3 */
            s->shadow[SFC_CMD_DATABUF0 / 4] = 0; /* not busy, unprotected */
            break;
        default:
            break;
        }
        s->shadow[off / 4] = (uint32_t)val & ~0x1u; /* transfer complete */
        return;
    }
    s->shadow[(off / 4) % (WS63_SFC_SIZE / 4)] = (uint32_t)val;
}

static const MemoryRegionOps ws63_sfc_ops = {
    .read = ws63_sfc_read,
    .write = ws63_sfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ws63_sfc_instance_init(Object *obj)
{
    WS63SfcState *s = WS63_SFC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    s->flash_id = SFC_FLASH_ID_W25Q16;  /* WS63 default; bs21.c overrides to GD */
    memory_region_init_io(&s->iomem, obj, &ws63_sfc_ops, s,
                          TYPE_WS63_SFC, WS63_SFC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const TypeInfo ws63_sfc_typeinfo = {
    .name          = TYPE_WS63_SFC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WS63SfcState),
    .instance_init = ws63_sfc_instance_init,
};

/* ============================================================================
 * Generic peripheral models — one device type with per-kind behavior, covering
 * the rest of the WS63 peripherals so the C SDK / rs HAL drivers can poll/use
 * them without hanging (no real hardware). Bases from WS63.svd; register layout
 * + the status/ready/done bits to force are from the SVD + C SDK HAL (researched).
 * Default behavior is a read/write register shadow (drivers read back what they
 * wrote); per-kind overrides force the bits drivers poll on.
 * ========================================================================= */
#define TYPE_WS63_PERIPH "ws63-periph"
OBJECT_DECLARE_SIMPLE_TYPE(WS63PeriphState, WS63_PERIPH)

typedef enum {
    PK_GENERIC, PK_WDT, PK_DMA, PK_TRNG, PK_I2C,
    PK_RTC, PK_LSADC, PK_SPI, PK_EFUSE, PK_I2S, PK_PWM, PK_TSENSOR,
} WS63PeriphKind;

#define WS63_PERIPH_MAXSIZE 0x1000

struct WS63PeriphState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;       /* IRQ output line (DMA/RTC/...) */
    uint32_t kind;
    uint32_t size;
    hwaddr   base;      /* MMIO base (identifies cldo_crg for the clock tree) */
    uint32_t rng;       /* TRNG LFSR / generic entropy */
    uint32_t dma_done;  /* DMA: per-channel transfer-complete mask */
    uint64_t counter;   /* RTC free-running counter */
    QEMUTimer *qtimer;  /* RTC periodic / WDT timeout */
    uint32_t int_status;/* RTC interrupt pending */
    uint32_t fifo[32];  /* SPI/I2C loopback FIFO */
    int fhead, ftail, fcnt;
    uint32_t shadow[WS63_PERIPH_MAXSIZE / 4];
};

#define WS63_RTC_HZ 32768ULL

/* RTC periodic tick / WDT timeout */
static void ws63_periph_timer(void *opaque)
{
    WS63PeriphState *s = opaque;

    if (s->kind == PK_RTC) {
        uint32_t load = s->shadow[0x00 / 4];        /* RTC_LOAD_COUNT */
        if (!(s->shadow[0x08 / 4] & 0x4)) {          /* CONTROL.int_mask == 0 */
            s->int_status = 1;
            qemu_set_irq(s->irq, 1);
        }
        if (load == 0) {
            load = WS63_RTC_HZ;
        }
        timer_mod(s->qtimer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (uint64_t)load * 1000000000ULL / WS63_RTC_HZ);
    } else if (s->kind == PK_WDT) {
        /* watchdog bit: counter expired without a kick -> reset the SoC */
        if (s->shadow[0x10 / 4] & 0x1) {             /* WDT_CR.wdt_en */
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
    }
}

static void ws63_wdt_arm(WS63PeriphState *s)
{
    uint32_t cnt = s->shadow[0x04 / 4] >> 8;          /* WDT_LOAD bits[31:8] */
    if (cnt == 0) {
        cnt = s->shadow[0x04 / 4] ? s->shadow[0x04 / 4] : WS63_RTC_HZ;
    }
    timer_mod(s->qtimer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (uint64_t)cnt * 1000000000ULL / WS63_RTC_HZ);
}

/*
 * Real DMA channel transfer: when a channel's cfg.ch_enable is written, copy the
 * data from src to dst (honoring widths + address increment), mark the channel
 * transfer-complete, and (if the channel's TC interrupt is enabled+unmasked)
 * raise the DMA IRQ. Channel regs: base 0x100 + ch*0x20; dest@+0x04, cfg@+0x08,
 * src@+0x10, ctrl@+0x14 (transfersize[11:0], swsize[20:18], dwsize[23:21],
 * src_inc[26], dest_inc[27], tc_int_en[31]). The channel cfg adds the
 * peripheral flow-control fields decoded below: src_per[1:4], dest_per[5:8],
 * fc_tt[9:11], int_err_mask[12], tc_int_mask[13], active[15].
 */
static void ws63_dma_run(WS63PeriphState *s, int ch)
{
    uint32_t base = 0x100 + (uint32_t)ch * 0x20;
    uint32_t dst  = s->shadow[(base + 0x04) / 4];
    uint32_t cfg  = s->shadow[(base + 0x08) / 4];
    uint32_t src  = s->shadow[(base + 0x10) / 4];
    uint32_t ctrl = s->shadow[(base + 0x14) / 4];
    /* v151 ctrl: transfersize[0:11], swsize[18:20], dwsize[21:23],
     * src_inc bit26, dest_inc bit27, tc_int_en bit31. */
    uint32_t count = ctrl & 0xfff;
    uint32_t sw = 1u << ((ctrl >> 18) & 0x7);
    uint32_t dw = 1u << ((ctrl >> 21) & 0x7);
    bool sinc = (ctrl >> 26) & 0x1;
    bool dinc = (ctrl >> 27) & 0x1;
    /* v151 cfg flow-control / handshaking fields. */
    uint32_t fc   = (cfg >> 9) & 0x7;   /* fc_tt: transfer type / flow control */
    uint32_t sper = (cfg >> 1) & 0xf;   /* src_per:  source handshaking ID      */
    uint32_t dper = (cfg >> 5) & 0xf;   /* dest_per: destination handshaking ID */
    uint32_t i, w = sw < dw ? sw : dw;

    if (count > 0x10000) count = 0x10000;       /* safety bound */
    if (w == 0 || w > 8) w = 4;

    /*
     * Flow control (fc_tt): 0 = mem->mem, 1 = mem->periph, 2 = periph->mem,
     * 3 = periph->periph. For the peripheral side the driver holds the address
     * fixed (src_inc/dest_inc = 0) at a peripheral data register and names the
     * request line via src_per/dest_per. Because the copy below uses the
     * MMIO-aware cpu_physical_memory_{read,write}, a mem->periph beat lands in
     * the destination peripheral's register handler (e.g. the SPI loopback
     * FIFO at SPI_DR) and a periph->mem beat reads it back -- hardware
     * handshaking is modelled as "every queued beat is serviced" (functional,
     * not cycle-accurate request pacing).
     */
    trace_ws63_dma_xfer(ch, fc, sper, dper, src, dst, count, w);

    for (i = 0; i < count; i++) {
        uint8_t b[8] = {0};
        cpu_physical_memory_read(sinc ? src + i * sw : src, b, w);
        cpu_physical_memory_write(dinc ? dst + i * dw : dst, b, w);
    }
    s->dma_done |= (1u << ch);
    /* Completion: hardware auto-clears ch_enable (bit0) and active (bit15). */
    s->shadow[(base + 0x08) / 4] &= ~((1u << 0) | (1u << 15));

    /*
     * Masked TC interrupt = ctrl.tc_int_en (bit31) AND cfg.tc_int_mask (bit13).
     * The v151/PL080 cfg "mask enable" bit is active-high (1 = interrupt
     * enabled/unmasked): the C SDK sets both ctrl bit31 and cfg bit13 when a
     * completion callback is registered (fbb_ws63
     * hal_dma_v151_config_single_block). The raw status (dma_done, read via
     * ORI_INT_STATUS) is set regardless so polling always observes completion.
     */
    if ((ctrl & (1u << 31)) && (cfg & (1u << 13))) {
        qemu_set_irq(s->irq, 1);
    }
}

static uint32_t ws63_xorshift(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x1234abcdu;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

/* small loopback FIFO for I2C/SPI transfer engines */
static void ws63_fifo_push(WS63PeriphState *s, uint32_t v)
{
    if (s->fcnt < (int)ARRAY_SIZE(s->fifo)) {
        s->fifo[s->ftail] = v;
        s->ftail = (s->ftail + 1) % ARRAY_SIZE(s->fifo);
        s->fcnt++;
    }
}

static uint32_t ws63_fifo_pop(WS63PeriphState *s)
{
    uint32_t v = 0;
    if (s->fcnt > 0) {
        v = s->fifo[s->fhead];
        s->fhead = (s->fhead + 1) % ARRAY_SIZE(s->fifo);
        s->fcnt--;
    }
    return v;
}

static uint64_t ws63_periph_read(void *opaque, hwaddr off, unsigned size)
{
    WS63PeriphState *s = opaque;
    uint32_t v = s->shadow[(off / 4) % (WS63_PERIPH_MAXSIZE / 4)];

    switch (s->kind) {
    case PK_WDT:
        if (off == 0x0C) return 0;          /* EOI: read clears int */
        if (off == 0x14) return s->shadow[0x04 / 4]; /* CNT -> load value */
        break;
    case PK_DMA:
        /* INT_ST (0x04): int_st[0:7] + int_trans_st[8:15]; ORI_INT_ST (0x0C) raw.
         * The v151 TC ISR reads this to find the channel (it writes no clear reg);
         * the local-IRQ line auto-clears on delivery, so no read-to-clear here. */
        if (off == 0x04 || off == 0x0C) {
            return s->dma_done | (s->dma_done << 8);
        }
        if (off == 0x10) {              /* EN_CHNS: enabled-channel mask */
            uint32_t m = 0;
            for (int c = 0; c < 8; c++) {
                if (s->shadow[(0x100 + c * 0x20 + 0x08) / 4] & 1) m |= 1u << c;
            }
            return m;
        }
        break;
    case PK_TRNG:
        if (off == 0x100) return ws63_xorshift(&s->rng); /* FIFO_DATA */
        if (off == 0x104) return 0x3;       /* FIFO_READY: data_ready|done */
        break;
    case PK_I2C:
        if (off == 0x0C) return 0x39;                 /* SR: done|rx|tx|stop */
        if (off == 0x1C) return ws63_fifo_pop(s) & 0xff; /* RXR: pop loopback FIFO */
        if (off == 0x20) {                            /* FIFOSTATUS */
            return (s->fcnt == 0 ? 0x08 : 0x00) | 0x02; /* rxfe if empty, txfe */
        }
        if (off == 0x28) return s->fcnt;              /* RXCOUNT */
        break;
    case PK_RTC:
        if (off == 0x04) { s->counter += 0x1000; return (uint32_t)s->counter; } /* CURRENT_VALUE */
        if (off == 0x0C) { s->int_status = 0; qemu_set_irq(s->irq, 0); return 0; } /* EOI */
        if (off == 0x10) return s->int_status; /* INT_STATUS */
        break;
    case PK_LSADC:
        /* v154 ADC: the init calibration sequence polls these "done" status bits
         * before any conversion — report calibration complete so init proceeds.
         * The auto-scan read path is synchronous (no conversion IRQ), so we just
         * model the calibration-done bits + the RX-FIFO level/data. */
        if (off == 0x044) return 0x1;       /* offset_cali_finish_sts.offset_cali_finish */
        if (off == 0x0A0) return 0x1;       /* rpt_cap_cali_sts_0.finish */
        /* 0x080 gain.intr_gain_uint reads back 0 (shadow) -> gain unit 0, as required */
        if (off == 0x04) {                  /* CTRL_1: rne(bit3) tracks the RX FIFO level */
            return (s->fcnt > 0) ? 0x08 : 0x00;
        }
        if (off == 0x20) {                  /* CTRL_9: pop a sample; de-assert IRQ when drained */
            if (s->fcnt > 0) {
                s->fcnt--;
                if (s->fcnt == 0) {
                    qemu_set_irq(s->irq, 0);
                }
                return 0x1F40;              /* ~mid-scale 14-bit code, channel 0 -> sane voltage */
            }
            return 0;
        }
        break;
    case PK_SPI:
        if (off == 0x60) return ws63_fifo_pop(s);   /* DR: pop RX (loopback) FIFO */
        if (off == 0xE4) {                          /* WSR */
            return 0x06 | (s->fcnt > 0 ? 0x08 : 0); /* txfnf|txfe + rxfne if data */
        }
        if (off == 0xD0) return 0;                  /* TLR (TX FIFO empty) */
        if (off == 0xDC) return s->fcnt;            /* RLR (RX FIFO level) */
        if (off == 0xC0) return 0;                  /* INSR */
        break;
    case PK_I2S:
        if (off == 0x54) return s->shadow[0x4C / 4]; /* LEFT_RX  <- LEFT_TX  */
        if (off == 0x58) return s->shadow[0x50 / 4]; /* RIGHT_RX <- RIGHT_TX */
        break;
    case PK_EFUSE:
        if (off == 0x2C) return 0x0c;       /* STS: boot0_done|boot1_done */
        break;                              /* data window 0x800.. -> shadow (OTP) */
    case PK_TSENSOR:
        /* sts @0x308: rdy(bit1)=1, data(bits[11:2]) = temp code. ~25C -> code 422
         * via temp=(code-114)/(896-114)*165-40 ; report a steady 25C. */
        if (off == 0x308) return (1u << 1) | (422u << 2);
        break;
    case PK_PWM:
        /* PERIODLOAD_FLAG @ 0x124 + 0x40*ch -> 1 (period loaded) */
        if (off >= 0x124 && ((off - 0x124) % 0x40) == 0) return 1;
        break;
    default:
        break;
    }
    return v;
}

static void ws63_periph_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    WS63PeriphState *s = opaque;
    uint32_t v = (uint32_t)val;

    switch (s->kind) {
    case PK_I2C:
        if (off == 0x04) { v &= ~0xfu; }                 /* COM: cmd bits auto-clear */
        else if (off == 0x18) { ws63_fifo_push(s, v & 0xff); } /* TXR -> loopback RX */
        break;
    case PK_SPI:
        if (off == 0x60) { ws63_fifo_push(s, v); }       /* DR -> loopback RX FIFO */
        break;
    case PK_LSADC:
        if (off == 0x1C && (v & 0x1)) {                  /* CTRL_8 lsadc_start: scan */
            s->fcnt = 2;                                 /* FIFO_DATA_LENS samples ready */
        }
        break;
    case PK_EFUSE:
        if (off >= 0x800 && off < 0xA00) {               /* OTP data window: program = OR */
            s->shadow[(off / 4) % (WS63_PERIPH_MAXSIZE / 4)] |= v;
            return;
        }
        break;
    case PK_PWM:
        if (off == 0x08 || off == 0x18 || off == 0x28 || off == 0x38) {
            v = 0;                          /* group START self-clears */
        }
        break;
    case PK_RTC:
        if (off == 0x08) {                  /* RTC_CONTROL */
            s->shadow[off / 4] = v;
            if (v & 0x1) {                  /* enable -> arm periodic tick */
                uint32_t load = s->shadow[0x00 / 4] ? s->shadow[0x00 / 4] : WS63_RTC_HZ;
                timer_mod(s->qtimer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          (uint64_t)load * 1000000000ULL / WS63_RTC_HZ);
            } else {
                timer_del(s->qtimer);
            }
            return;
        }
        break;
    case PK_WDT:
        if (off == 0x10) {                  /* WDT_CR */
            s->shadow[off / 4] = v;
            if (v & 0x1) { ws63_wdt_arm(s); } else { timer_del(s->qtimer); }
            return;
        }
        if (off == 0x08) {                  /* WDT_RESTART (kick) */
            if (s->shadow[0x10 / 4] & 0x1) { ws63_wdt_arm(s); }
            return;
        }
        break;
    case PK_DMA:
        if (off == 0x08) {                  /* DMAC_INT_CLR: int_trans_clr[0:7] / int_err_clr[8:15] */
            s->dma_done &= ~((v | (v >> 8)) & 0xFF); /* clear the channel on either field */
            if (s->dma_done == 0) {
                qemu_set_irq(s->irq, 0);
            }
            s->shadow[off / 4] = 0;
            return;
        }
        if (off >= 0x100 && off < 0x100 + 8 * 0x20 &&
            ((off - 0x100) % 0x20) == 0x08 && (v & 1)) {
            /* channel cfg write with ch_enable -> run the transfer */
            s->shadow[(off / 4) % (WS63_PERIPH_MAXSIZE / 4)] = v;
            ws63_dma_run(s, (off - 0x100) / 0x20);
            return;
        }
        break;
    default:
        break;
    }
    /*
     * CLDO_CRG clock gates / source select drive the clock tree: off 0x00 =
     * CKEN_CTL0, 0x04 = CKEN_CTL1, 0x34 = CLK_SEL. Clearing the timer gate
     * (CKEN_CTL0 bit21) freezes the timer; setting it resumes (re-armed below).
     * The shadow store still runs after, so register read-back is consistent.
     */
    if (s->kind == PK_GENERIC && s->base == 0x44001100 &&
        (off == 0x00 || off == 0x04 || off == 0x34)) {
        bool timer_was_gated = ws63_timer_clock_gated();
        if (off == 0x00) {
            g_ws63_cken0 = v;
        } else if (off == 0x04) {
            g_ws63_cken1 = v;
        } else {
            g_ws63_clk_sel = v;
        }
        if (off == 0x00 && g_ws63_timer &&
            timer_was_gated != ws63_timer_clock_gated()) {
            for (unsigned i = 0; i < 3; i++) {
                if (g_ws63_timer->control[i] & TMR_CONTROL_EN) {
                    ws63_timer_arm(g_ws63_timer, i); /* arm re-checks the gate */
                }
            }
        }
    }
    s->shadow[(off / 4) % (WS63_PERIPH_MAXSIZE / 4)] = v;
}

static const MemoryRegionOps ws63_periph_ops = {
    .read = ws63_periph_read,
    .write = ws63_periph_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ws63_periph_realize(DeviceState *dev, Error **errp)
{
    WS63PeriphState *s = WS63_PERIPH(dev);
    s->rng = 0x2545f491u ^ (s->kind << 8);
    if (s->kind == PK_RTC || s->kind == PK_WDT) {
        s->qtimer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ws63_periph_timer, s);
    }
    if (s->base == 0x44001100) {            /* cldo_crg: CKEN gates default all-on */
        s->shadow[0x00 / 4] = 0xFFFFFFFFu;  /* CKEN_CTL0 (matches g_ws63_cken0) */
        s->shadow[0x04 / 4] = 0xFFFFFFFFu;  /* CKEN_CTL1 (matches g_ws63_cken1) */
    }
    memory_region_init_io(&s->iomem, OBJECT(dev), &ws63_periph_ops, s,
                          TYPE_WS63_PERIPH, s->size ? s->size : WS63_PERIPH_MAXSIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void ws63_periph_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = ws63_periph_realize;
}

static const TypeInfo ws63_periph_typeinfo = {
    .name          = TYPE_WS63_PERIPH,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WS63PeriphState),
    .class_init    = ws63_periph_class_init,
};

/* Shared helper (declared in hisi_riscv31.h): a PK_SPI loopback controller at
 * @base, for the bs2x machines to functionally exercise the Rust SPI driver. */
DeviceState *ws63_create_spi_loopback(hwaddr base)
{
    DeviceState *dev = qdev_new(TYPE_WS63_PERIPH);
    WS63PeriphState *s = WS63_PERIPH(dev);
    s->kind = PK_SPI;
    s->size = 0x1000;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return dev;
}

/* ============================================================================
 * BS2X GADC (13-bit ADC, v153) digital block @0x57036000 — minimal model so the
 * chip-bs21 Rust `gadc` driver completes a conversion. The driver's blocking path
 * polls done = rpt_gadc_data_3 (0x070) bit0 and reads the result from
 * rpt_gadc_data_2 (0x06C); the ANA/LDO writes (>=0x3D0, within this window) are
 * shadowed, and the power/enable handshake lives in the PMU block @0x57008700 +
 * the AON iso bit, which fall to the bs2x GLB absorber (the driver never polls
 * those). So this stateless model only needs to report "done" + a fixed sample.
 * ========================================================================== */
#define WS63_GADC_RESULT     0x00012345u /* test sample (18-bit, positive) */
#define WS63_GADC_DATA2_OFF  0x06C        /* rpt_gadc_data_2 (result) */
#define WS63_GADC_DATA3_OFF  0x070        /* rpt_gadc_data_3 (bit0 = sample done) */

static uint64_t ws63_gadc_read(void *opaque, hwaddr off, unsigned size)
{
    switch (off) {
    case WS63_GADC_DATA3_OFF:
        return 0x1;                 /* sample-done */
    case WS63_GADC_DATA2_OFF:
        return WS63_GADC_RESULT;    /* the accumulated 18-bit sample */
    default:
        return 0;
    }
}

static void ws63_gadc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    /* bring-up / channel-select / config writes: accept + ignore. */
}

static const MemoryRegionOps ws63_gadc_ops = {
    .read = ws63_gadc_read,
    .write = ws63_gadc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* Shared helper (declared in hisi_riscv31.h): map the GADC model at @base. */
void ws63_create_gadc(hwaddr base)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr, NULL, &ws63_gadc_ops, NULL, "bs2x.gadc", 0x1000);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

/* ============================================================================
 * BS2X I2C (DesignWare SSI, v151) master model with ONE slave on the bus, so the
 * chip-bs21 Rust `i2c` driver's bus scan finds exactly one device. A transfer
 * (write to IC_DATA_CMD 0x1C) targeting IC_TAR == WS63_I2C_SLAVE_ADDR completes
 * (STOP_DET, and a read returns WS63_I2C_SLAVE_DATA); any other address aborts
 * (TX_ABRT + addr_7b_noack), which is how the driver tells present from absent.
 * ========================================================================== */
#define WS63_I2C_SLAVE_ADDR  0x50
#define WS63_I2C_SLAVE_DATA  0xA5
#define I2C_IC_TAR_OFF       0x10
#define I2C_IC_DATA_CMD_OFF  0x1C
#define I2C_IC_STATUS_OFF    0x60
#define I2C_IC_RXFLR_OFF     0x68
#define I2C_IC_ABRT_SLV_OFF  0x7C
#define I2C_IC_RAW_INTR_OFF  0xB8
#define I2C_IC_CLR_INTR_OFF  0xC0
#define I2C_IC_CLR_INT_OFF   0xC4
#define I2C_RAW_TX_ABRT      (1u << 6)
#define I2C_RAW_STOP_DET     (1u << 9)

typedef struct {
    uint32_t ic_tar;
    uint32_t raw_intr;   /* bit6 tx_abrt, bit9 stop_det */
    uint32_t abrt_slv;   /* bit0 addr_7b_noack */
    uint8_t  rx_byte;
    bool     rx_valid;
} WS63I2cState;

static uint64_t ws63_i2c_read(void *opaque, hwaddr off, unsigned size)
{
    WS63I2cState *s = opaque;
    switch (off) {
    case I2C_IC_STATUS_OFF:
        /* tfnf(1)|tfe(2) always ready; rfne(3) when a byte is waiting. */
        return 0x6 | (s->rx_valid ? 0x8 : 0);
    case I2C_IC_RAW_INTR_OFF:
        return s->raw_intr;
    case I2C_IC_ABRT_SLV_OFF:
        return s->abrt_slv;
    case I2C_IC_RXFLR_OFF:
        return s->rx_valid ? 1 : 0;
    case I2C_IC_DATA_CMD_OFF: {
        uint8_t b = s->rx_byte;
        s->rx_valid = false;
        return b;
    }
    case I2C_IC_CLR_INTR_OFF:
    case I2C_IC_CLR_INT_OFF:
        /* read-to-clear all interrupts + the abort source. */
        s->raw_intr = 0;
        s->abrt_slv = 0;
        return 0;
    default:
        return 0;
    }
}

static void ws63_i2c_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    WS63I2cState *s = opaque;
    switch (off) {
    case I2C_IC_TAR_OFF:
        s->ic_tar = val & 0x3FF;
        break;
    case I2C_IC_DATA_CMD_OFF:
        if ((s->ic_tar & 0x7F) == WS63_I2C_SLAVE_ADDR) {
            s->raw_intr |= I2C_RAW_STOP_DET;        /* device ACKed */
            if (val & (1u << 8)) {                  /* cmd bit = read */
                s->rx_byte = WS63_I2C_SLAVE_DATA;
                s->rx_valid = true;
            }
        } else {
            s->raw_intr |= I2C_RAW_TX_ABRT;         /* address NACK */
            s->abrt_slv |= 1u;                      /* addr_7b_noack */
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps ws63_i2c_ops = {
    .read = ws63_i2c_read,
    .write = ws63_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* Shared helper (declared in hisi_riscv31.h): map a DesignWare I2C master model
 * (with one slave @0x50) at @base, for the bs2x machines to exercise the Rust I2C
 * driver's bus scan. */
void ws63_create_i2c(hwaddr base)
{
    WS63I2cState *s = g_new0(WS63I2cState, 1);
    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr, NULL, &ws63_i2c_ops, s, "bs2x.i2c", 0x1000);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

/* ============================================================================
 * BS2X KEYSCAN (key-matrix scanner, v150) @0x5208D000 — reports one fixed key so
 * the chip-bs21 Rust `keyscan` driver decodes a known (row,col,pressed). Once the
 * scan task is started, EVENT_STS reads ready and KEY_VALUE_FIFO yields the key
 * once (then the empty marker 0x0FF).
 * ========================================================================== */
#define KEYSCAN_START_OFF      0x10
#define KEYSCAN_EVENT_STS_OFF  0x28
#define KEYSCAN_FIFO_OFF       0x94
#define KEYSCAN_KEY_VALUE      0x111u /* key[8]=pressed, key[7:3]=row=2, key[2:0]=col=1 */
#define KEYSCAN_FIFO_EMPTY     0x0FFu

typedef struct { bool key_pending; } WS63KeyscanState;

static uint64_t ws63_keyscan_read(void *opaque, hwaddr off, unsigned size)
{
    WS63KeyscanState *s = opaque;
    switch (off) {
    case KEYSCAN_EVENT_STS_OFF:
        return s->key_pending ? 0x0A : 0; /* key_press(1) | key_value_rdy(3) */
    case KEYSCAN_FIFO_OFF:
        if (s->key_pending) {
            s->key_pending = false;
            return KEYSCAN_KEY_VALUE;
        }
        return KEYSCAN_FIFO_EMPTY;
    default:
        return 0;
    }
}

static void ws63_keyscan_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    WS63KeyscanState *s = opaque;
    if (off == KEYSCAN_START_OFF && (val & 0x1)) {
        s->key_pending = true; /* a scan started -> a key becomes available */
    }
}

static const MemoryRegionOps ws63_keyscan_ops = {
    .read = ws63_keyscan_read,
    .write = ws63_keyscan_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void ws63_create_keyscan(hwaddr base)
{
    WS63KeyscanState *s = g_new0(WS63KeyscanState, 1);
    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr, NULL, &ws63_keyscan_ops, s, "bs2x.keyscan", 0x1000);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

/* ============================================================================
 * BS2X QDEC (quadrature decoder, v150) @0x52000200 — returns a fixed SIGNED count
 * so the chip-bs21 Rust `qdec` driver reads a known value. ACC=0xFFFB so the
 * driver's `as i16` yields -5 (exercises the signed decode); ACCDBL=2.
 * ========================================================================== */
#define QDEC_ACC_DATA_OFF     0x54
#define QDEC_ACCDBL_DATA_OFF  0x58

static uint64_t ws63_qdec_read(void *opaque, hwaddr off, unsigned size)
{
    switch (off) {
    case QDEC_ACC_DATA_OFF:
        return 0xFFFBu;   /* (i16)0xFFFB = -5 */
    case QDEC_ACCDBL_DATA_OFF:
        return 0x2u;
    default:
        return 0;
    }
}

static void ws63_qdec_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    /* enable / task-start / config writes: accept + ignore. */
}

static const MemoryRegionOps ws63_qdec_ops = {
    .read = ws63_qdec_read,
    .write = ws63_qdec_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void ws63_create_qdec(hwaddr base)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    memory_region_init_io(mr, NULL, &ws63_qdec_ops, NULL, "bs2x.qdec", 0x1000);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

/* peripheral instance table (base, kind, window size, name, irq) — from WS63.svd.
 * irq != 0 is connected to the intc (the device raises it via qemu_set_irq). */
static const struct {
    hwaddr base; uint32_t kind; uint32_t size; const char *name; int irq;
} ws63_periph_table[] = {
    /* GLB_CTL_M (0x40002000) is within the SYS_CTL0 window and handled there;
     * SYS_CTL1 (0x44000000) is within the TCXO device window. */
    { 0x44001100, PK_GENERIC, 0x1000, "cldo_crg",  0 },
    { 0x44004000, PK_GENERIC, 0x1000, "rf_wb_ctl", 0 }, /* RF/PHY radio NOT simulated; config shadow */
    { 0x44006C00, PK_GENERIC, 0x400,  "share_mem", 0 },
    { 0x44007800, PK_GENERIC, 0x400,  "fama_remap",0 },
    { 0x57030000, PK_GENERIC, 0x1000, "ulp_gpio",  0 },
    { 0x40006000, PK_WDT,     0x1000, "wdt",       0 },
    { 0x44008000, PK_EFUSE,   0x1000, "efuse",     0 },
    { 0x4400C000, PK_LSADC,   0x1000, "lsadc",     WS63_IRQ_LSADC },
    /* IO_CONFIG (0x4400D000) is the ws63-pinmux device (pin-mux fabric). */
    { 0x4400E000, PK_TSENSOR, 0x1000, "tsensor",   0 },
    { 0x44018000, PK_I2C,     0x100,  "i2c0",      0 },
    { 0x44018100, PK_I2C,     0x100,  "i2c1",      0 },
    { 0x44020000, PK_SPI,     0x1000, "spi0",      0 },
    { 0x44021000, PK_SPI,     0x1000, "spi1",      0 },
    { 0x44024000, PK_PWM,     0x1000, "pwm",       0 },
    { 0x44025000, PK_I2S,     0x1000, "i2s",       0 },
    { 0x44100000, PK_GENERIC, 0x1000, "spacc",     0 },
    { 0x44110000, PK_GENERIC, 0x1000, "pke",       0 },
    { 0x44112000, PK_GENERIC, 0x1000, "km",        0 },
    { 0x44114000, PK_TRNG,    0x1000, "trng",      0 },
    { 0x4A000000, PK_DMA,     0x1000, "dma",       WS63_IRQ_DMA },
    { 0x520A0000, PK_DMA,     0x1000, "sdma",      0 },
    { 0x57024000, PK_RTC,     0x1000, "rtc",       WS63_IRQ_RTC },
};
#define WS63_NUM_PERIPH ARRAY_SIZE(ws63_periph_table)

/* ============================================================================
 * IO_CONFIG pin-mux fabric (0x4400D000). GPIO_xx_SEL[2:0] selects each pin's
 * function (0 = GPIO, 1-7 = UART/SPI/I2C/PWM/...). We route the GPIO pin net
 * through here: a source pin reaches the board net only while it is muxed to
 * GPIO; mux it to another function and the GPIO signal is gated (the pin then
 * carries that peripheral instead — those signals are covered by each
 * peripheral's own TX/RX/loopback rather than re-routed onto this net).
 * ========================================================================= */
#define TYPE_WS63_PINMUX "ws63-pinmux"
OBJECT_DECLARE_SIMPLE_TYPE(WS63PinmuxState, WS63_PINMUX)

#define WS63_PINMUX_BASE 0x4400D000
#define WS63_PINMUX_SIZE 0x1000

struct WS63PinmuxState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq out[WS63_GPIO_PINS];       /* routed pin -> destination */
    uint8_t in_level[WS63_GPIO_PINS];   /* source level (from GPIO output) */
    uint32_t sel[WS63_PINMUX_SIZE / 4]; /* GPIO_xx_SEL @ 0x4*pin, func in [2:0] */
};

static void ws63_pinmux_route(WS63PinmuxState *s, int pin)
{
    uint32_t func = s->sel[pin] & 0x7;  /* 0 = GPIO function */
    qemu_set_irq(s->out[pin], func == 0 ? s->in_level[pin] : 0);
}

static void ws63_pinmux_set_in(void *opaque, int n, int level)
{
    WS63PinmuxState *s = opaque;
    if (n < WS63_GPIO_PINS) {
        s->in_level[n] = level ? 1 : 0;
        ws63_pinmux_route(s, n);
    }
}

static uint64_t ws63_pinmux_read(void *opaque, hwaddr off, unsigned size)
{
    WS63PinmuxState *s = opaque;
    return s->sel[(off / 4) % (WS63_PINMUX_SIZE / 4)];
}

static void ws63_pinmux_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    WS63PinmuxState *s = opaque;
    s->sel[(off / 4) % (WS63_PINMUX_SIZE / 4)] = (uint32_t)val;
    if ((off / 4) < WS63_GPIO_PINS) {   /* GPIO_pin_SEL changed -> re-route pin */
        ws63_pinmux_route(s, off / 4);
    }
}

static const MemoryRegionOps ws63_pinmux_ops = {
    .read = ws63_pinmux_read,
    .write = ws63_pinmux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ws63_pinmux_instance_init(Object *obj)
{
    WS63PinmuxState *s = WS63_PINMUX(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    memory_region_init_io(&s->iomem, obj, &ws63_pinmux_ops, s,
                          TYPE_WS63_PINMUX, WS63_PINMUX_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_in(DEVICE(obj), ws63_pinmux_set_in, WS63_GPIO_PINS);
    qdev_init_gpio_out(DEVICE(obj), s->out, WS63_GPIO_PINS);
}

static const TypeInfo ws63_pinmux_typeinfo = {
    .name          = TYPE_WS63_PINMUX,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WS63PinmuxState),
    .instance_init = ws63_pinmux_instance_init,
};

/* ============================================================================
 * Synthetic Wi-Fi/Ethernet MAC (ws63-netmac, 0x44210000) — the software-in-the-
 * loop connectivity base (ROADMAP phase 5). The real WS63 Wi-Fi datapath is a
 * closed RF/PHY blob (hardware-in-the-loop only); we DO NOT model the radio.
 * Instead we expose a minimal Ethernet-frame MAC at the ws63-rf-rs netif seam:
 * firmware writes a frame into TX_BUF + TX_LEN and pulses TX_GO, and we hand the
 * frame to the QEMU netdev (SLIRP/-nic user NAT). Host-side frames arrive via the
 * .receive callback into RX_BUF and raise IRQ 45 (WLMAC_INT). This lets ws63-rs's
 * smoltcp stack do ARP/ICMP/UDP over the user-mode NAT without any radio model.
 * Pairs with `-nic user`. NOT a register-faithful model of the vendor WLMAC.
 * ========================================================================= */
#define TYPE_WS63_NETMAC "ws63-netmac"
OBJECT_DECLARE_SIMPLE_TYPE(WS63NetMacState, WS63_NETMAC)

#define WS63_NETMAC_BASE      0x44210000   /* WIFI_SUB region */
#define WS63_NETMAC_SIZE      0x00004000
#define WS63_NETMAC_BUF_MAX   2048         /* >= max Ethernet frame */

/* Register map (offsets within the 16 KiB window). */
#define NETMAC_CTRL           0x000   /* RW  bit0 enable, bit1 rx_irq_en */
#define NETMAC_INT_STATUS     0x004   /* RO  bit0 rx_ready */
#define NETMAC_INT_CLEAR      0x008   /* W1C bit0 rx_ready */
#define NETMAC_TX_LEN         0x00C   /* RW  bytes staged in TX_BUF */
#define NETMAC_TX_GO          0x010   /* W1  send the staged frame */
#define NETMAC_RX_LEN         0x014   /* RO  bytes available in RX_BUF */
#define NETMAC_RX_ACK         0x018   /* W1  release RX_BUF for the next frame */
#define NETMAC_MAC_LO         0x020   /* RO  mac[0..3] (little-endian) */
#define NETMAC_MAC_HI         0x024   /* RO  mac[4..5] */
#define NETMAC_TX_BUF         0x1000  /* TX frame staging buffer */
#define NETMAC_RX_BUF         0x2000  /* RX frame buffer */

#define NETMAC_CTRL_ENABLE    0x1
#define NETMAC_CTRL_RX_IRQ_EN 0x2
#define NETMAC_INT_RX_READY   0x1

struct WS63NetMacState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    NICState *nic;
    NICConf conf;
    uint32_t ctrl;
    uint32_t tx_len;
    uint32_t rx_len;
    bool rx_ready;
    uint8_t txbuf[WS63_NETMAC_BUF_MAX];
    uint8_t rxbuf[WS63_NETMAC_BUF_MAX];
};

static void ws63_netmac_update_irq(WS63NetMacState *s)
{
    qemu_set_irq(s->irq, s->rx_ready && (s->ctrl & NETMAC_CTRL_RX_IRQ_EN));
}

/* Net core asks before delivering: accept only when enabled and RX_BUF is free. */
static bool ws63_netmac_can_receive(NetClientState *nc)
{
    WS63NetMacState *s = qemu_get_nic_opaque(nc);
    return (s->ctrl & NETMAC_CTRL_ENABLE) && !s->rx_ready;
}

static ssize_t ws63_netmac_receive(NetClientState *nc, const uint8_t *buf,
                                   size_t size)
{
    WS63NetMacState *s = qemu_get_nic_opaque(nc);

    if (!(s->ctrl & NETMAC_CTRL_ENABLE) || s->rx_ready) {
        return 0;   /* not ready: net core re-queues and retries later */
    }
    if (size > WS63_NETMAC_BUF_MAX) {
        size = WS63_NETMAC_BUF_MAX;   /* truncate oversized frames */
    }
    memcpy(s->rxbuf, buf, size);
    s->rx_len = size;
    s->rx_ready = true;
    ws63_netmac_update_irq(s);
    return size;
}

static uint64_t ws63_netmac_read(void *opaque, hwaddr off, unsigned sz)
{
    WS63NetMacState *s = opaque;

    if (off >= NETMAC_RX_BUF && off < NETMAC_RX_BUF + WS63_NETMAC_BUF_MAX) {
        return ldl_le_p(&s->rxbuf[off - NETMAC_RX_BUF]);
    }
    if (off >= NETMAC_TX_BUF && off < NETMAC_TX_BUF + WS63_NETMAC_BUF_MAX) {
        return ldl_le_p(&s->txbuf[off - NETMAC_TX_BUF]);
    }
    switch (off) {
    case NETMAC_CTRL:
        return s->ctrl;
    case NETMAC_INT_STATUS:
        return s->rx_ready ? NETMAC_INT_RX_READY : 0;
    case NETMAC_TX_LEN:
        return s->tx_len;
    case NETMAC_RX_LEN:
        return s->rx_len;
    case NETMAC_MAC_LO:
        return ldl_le_p(&s->conf.macaddr.a[0]);
    case NETMAC_MAC_HI:
        return s->conf.macaddr.a[4] | (s->conf.macaddr.a[5] << 8);
    default:
        return 0;
    }
}

static void ws63_netmac_write(void *opaque, hwaddr off, uint64_t val, unsigned sz)
{
    WS63NetMacState *s = opaque;

    if (off >= NETMAC_TX_BUF && off < NETMAC_TX_BUF + WS63_NETMAC_BUF_MAX) {
        stl_le_p(&s->txbuf[off - NETMAC_TX_BUF], (uint32_t)val);
        return;
    }
    switch (off) {
    case NETMAC_CTRL:
        s->ctrl = (uint32_t)val;
        ws63_netmac_update_irq(s);
        return;
    case NETMAC_INT_CLEAR:
        if (val & NETMAC_INT_RX_READY) {
            s->rx_ready = false;
            ws63_netmac_update_irq(s);
        }
        return;
    case NETMAC_TX_LEN:
        s->tx_len = (uint32_t)val;
        if (s->tx_len > WS63_NETMAC_BUF_MAX) {
            s->tx_len = WS63_NETMAC_BUF_MAX;
        }
        return;
    case NETMAC_TX_GO:
        if ((s->ctrl & NETMAC_CTRL_ENABLE) && s->tx_len) {
            qemu_send_packet(qemu_get_queue(s->nic), s->txbuf, s->tx_len);
        }
        return;
    case NETMAC_RX_ACK:
        s->rx_ready = false;
        ws63_netmac_update_irq(s);
        return;
    default:
        return;   /* RO / unknown registers: absorb */
    }
}

static const MemoryRegionOps ws63_netmac_ops = {
    .read = ws63_netmac_read,
    .write = ws63_netmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static NetClientInfo ws63_netmac_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = ws63_netmac_can_receive,
    .receive = ws63_netmac_receive,
};

static void ws63_netmac_realize(DeviceState *dev, Error **errp)
{
    WS63NetMacState *s = WS63_NETMAC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &ws63_netmac_ops, s,
                          TYPE_WS63_NETMAC, WS63_NETMAC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&ws63_netmac_net_info, &s->conf,
                          TYPE_WS63_NETMAC, dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static const Property ws63_netmac_props[] = {
    DEFINE_NIC_PROPERTIES(WS63NetMacState, conf),
};

static void ws63_netmac_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ws63_netmac_realize;
    device_class_set_props(dc, ws63_netmac_props);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo ws63_netmac_typeinfo = {
    .name          = TYPE_WS63_NETMAC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(WS63NetMacState),
    .class_init    = ws63_netmac_class_init,
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
    WS63TcxoState tcxo;
    WS63SfcState sfc;
    WS63PinmuxState pinmux;
    WS63NetMacState netmac;
    WS63PeriphState periph[WS63_NUM_PERIPH];
    MemoryRegion bootrom;
    MemoryRegion rom;
    MemoryRegion itcm;
    MemoryRegion dtcm;
    MemoryRegion flash;
    MemoryRegion ppb;
};

/* Exposed via hisi_riscv31.h so bs21.c builds its own memory map. */
void ws63_make_ram(MemoryRegion *sys, MemoryRegion *mr,
                   const char *name, hwaddr base, uint64_t size)
{
    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(sys, base, mr);
}

static void ws63_cpu_reset(void *opaque)
{
    RISCVCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
    /*
     * Emulate the boot-stage hand-off ABI: on real silicon each boot stage is
     * entered with a0 = pointer to a boot-parameter block set up by the previous
     * stage (mask ROM -> loaderboot -> flashboot -> app). A standalone "-kernel"
     * boot has no previous stage, so a0 would be 0; the WS63 bootloaders
     * dereference a0 in their reset path (lw t3,0(a0)) before mtvec is even set,
     * which would load-fault and then double-fault to pc=0. Point a0 at a
     * readable, zeroed SRAM word so the boot-reason load reads 0 ("normal boot")
     * and startup proceeds. Harmless to firmwares that ignore a0 (rt/app).
     */
    cpu->env.gpr[10] = WS63_SRAM_BASE; /* a0 */
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

    /*
     * Core private peripheral bus (riscv31 core-local). 0xE0000000 holds the
     * Flash-Patch unit (FLPCTRL + 192 comparators, riscv_patch_init) and a
     * Cortex-M-style System Control Space at 0xE000E000 (CPUID/SCB-like).
     * We load the fully-patched firmware image, so the patch unit is inert;
     * back it with RAM so read-modify-write of these control regs works and
     * the accesses are absorbed rather than faulting. */
    ws63_make_ram(sys, &s->ppb, "ws63.ppb", WS63_PPB_BASE, WS63_PPB_SIZE);

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

    /* TCXO clock/counter (over the absorber) — early bootloader clock bring-up. */
    object_initialize_child(OBJECT(machine), "tcxo", &s->tcxo, TYPE_WS63_TCXO);
    sysbus_realize(SYS_BUS_DEVICE(&s->tcxo), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tcxo), 0, WS63_TCXO_BASE);

    /* SFC serial-flash controller (over the absorber) — flash identification. */
    object_initialize_child(OBJECT(machine), "sfc", &s->sfc, TYPE_WS63_SFC);
    sysbus_realize(SYS_BUS_DEVICE(&s->sfc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sfc), 0, WS63_SFC_BASE);

    /* All remaining modelable peripherals (I2C/SPI/PWM/I2S/LSADC/EFUSE/WDT/RTC/
     * DMA/SDMA/TRNG/CLDO_CRG/IO_CONFIG/...): register shadow + per-kind status
     * bits so HAL drivers run. Mapped over the catch-all absorber. */
    for (int i = 0; i < (int)WS63_NUM_PERIPH; i++) {
        object_initialize_child(OBJECT(machine), ws63_periph_table[i].name,
                                &s->periph[i], TYPE_WS63_PERIPH);
        s->periph[i].kind = ws63_periph_table[i].kind;
        s->periph[i].size = ws63_periph_table[i].size;
        s->periph[i].base = ws63_periph_table[i].base;
        sysbus_realize(SYS_BUS_DEVICE(&s->periph[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->periph[i]), 0, ws63_periph_table[i].base);
        if (ws63_periph_table[i].irq) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->periph[i]), 0,
                qdev_get_gpio_in(DEVICE(&s->intc), ws63_periph_table[i].irq));
        }
    }

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
    /* IO_CONFIG pin-mux fabric (0x4400D000). */
    object_initialize_child(OBJECT(machine), "pinmux", &s->pinmux, TYPE_WS63_PINMUX);
    sysbus_realize(SYS_BUS_DEVICE(&s->pinmux), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pinmux), 0, WS63_PINMUX_BASE);

    /* Board-level pin net, ROUTED THROUGH THE PIN MUX: GPIO0 output pins ->
     * pinmux -> GPIO1 input pins. A pin reaches GPIO1 only while IO_CONFIG muxes
     * it to GPIO; mux it elsewhere and the pinmux gates the GPIO signal. The
     * pinmux input lines can also be driven externally (monitor / another dev). */
    for (int i = 0; i < WS63_GPIO_PINS; i++) {
        qdev_connect_gpio_out(DEVICE(&s->gpio[0]), i,
                              qdev_get_gpio_in(DEVICE(&s->pinmux), i));
        qdev_connect_gpio_out(DEVICE(&s->pinmux), i,
                              qdev_get_gpio_in(DEVICE(&s->gpio[1]), i));
    }

    /* UART0/1/2 (custom device on top of the absorber). */
    const hwaddr uart_base[3] = { WS63_UART0_BASE, WS63_UART1_BASE, WS63_UART2_BASE };
    for (int i = 0; i < 3; i++) {
        DeviceState *uart = qdev_new(TYPE_WS63_UART);
        qdev_prop_set_chr(uart, "chardev", serial_hd(i));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, uart_base[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0,
                           qdev_get_gpio_in(DEVICE(&s->intc), WS63_IRQ_UART0 + i));
    }

    /* Synthetic Wi-Fi/Ethernet MAC (0x44210000, IRQ 45). Binds the default
     * `-nic user` netdev so smoltcp can reach the SLIRP NAT. Configure the NIC
     * BEFORE realize so qemu_new_nic() picks up the user-supplied netdev. */
    object_initialize_child(OBJECT(machine), "netmac", &s->netmac, TYPE_WS63_NETMAC);
    qemu_configure_nic_device(DEVICE(&s->netmac), true, NULL);
    sysbus_realize(SYS_BUS_DEVICE(&s->netmac), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->netmac), 0, WS63_NETMAC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->netmac), 0,
                       qdev_get_gpio_in(DEVICE(&s->intc), WS63_IRQ_WLMAC));

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

    /*
     * Single RV32IMFC_Zicsr hart. The ISA is baked into the named `ws63` CPU
     * type (target/riscv/cpu.c:rv32_ws63_cpu_init, the default_cpu_type below):
     * I/M/F/C + Zicsr/Zifencei/Zicntr + Zcf, no A/D, no MMU, and deliberately no
     * Zca/Zcb/Zcmp/Zcmt (the HiSilicon "xlinx" custom code-size instructions own
     * that compressed encoding space; see trans_xlinx.c.inc). Using the named
     * type instead of poking the configurable base CPU's per-letter properties
     * keeps machine init working under -accel qtest, which doesn't expose them.
     */
    object_initialize_child(OBJECT(machine), "cpu", &s->cpu, machine->cpu_type);
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
    mc->default_cpu_type = TYPE_RISCV_CPU_WS63;
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
    type_register_static(&ws63_tcxo_typeinfo);
    type_register_static(&ws63_sfc_typeinfo);
    type_register_static(&ws63_pinmux_typeinfo);
    type_register_static(&ws63_netmac_typeinfo);
    type_register_static(&ws63_periph_typeinfo);
    type_register_static(&ws63_machine_typeinfo);
}

type_init(ws63_register_types)
