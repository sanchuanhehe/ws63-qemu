/*
 * QEMU machine model for the HiSilicon BS21 / BS2X RISC-V SoC.
 *
 * BS21 is a BLE 5.4 + SLE (SparkLink / NearLink) combo SoC (NO Wi-Fi) built on
 * the same HiSilicon "HimiDeer" riscv31 core as WS63 — a single RV32IMFC_Zicsr
 * hart (hardware single-precision float, no atomics), app core at 64 MHz. It
 * shares WS63's LOCI local-interrupt architecture + custom CSRs and the same
 * versioned IP blocks (UART v151 / TIMER v150 / GPIO v150), so this machine
 * reuses the shared device models from hw/riscv/ws63.c (declared in
 * hisi_riscv31.h) at BS21's own base addresses + IRQ numbers.
 *
 * Milestone-1 scope: boot bare-metal ws63-rs firmware built with
 * `--features chip-bs21` (bs21-examples/{blinky,uart_hello}). That Rust firmware
 * emits only standard RV32IMFC and never calls the mask ROM, so this machine
 * needs neither the HiSilicon custom-ISA decoder nor ROM interception (both
 * deferred with connectivity). Modeled: the CPU, the BS21 memory map, the custom
 * UART (x3), one GPIO bank, the TIMER, the TCXO, the LOCI interrupt controller +
 * custom CSRs, and unimplemented-device absorbers for the two MMIO windows.
 *
 * Facts from the fbb_bs2x SDK; see docs/bs21-recon.md.
 *
 * Copyright (c) 2026 ws63-rs contributors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
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
#include "target/riscv/cpu.h"
#include "hw/core/cpu.h"
#include "system/system.h"
#include "system/reset.h"
#include "exec/address-spaces.h"
#include "elf.h"
#include "hisi_riscv31.h" /* shared HiSilicon riscv31 device models (impl: ws63.c) */

/* ----------------------------------------------------------------------------
 * Memory map — from the fbb_bs2x SDK (platform_core.h), matching the BS21
 * bs21-examples memory.x. Code runs XIP from NOR flash @0x10000000; data/bss/
 * stack live in L2RAM @0x00100000 (the -m bank).
 * ------------------------------------------------------------------------- */
#define BS21_BOOTROM_BASE   0x00000000
#define BS21_BOOTROM_SIZE   0x00008000   /* 32K mask-ROM MPU window */
#define BS21_ROM_BASE       0x00008000
#define BS21_ROM_SIZE       0x00078000   /* ROM symbols extend to ~0x80000 */
#define BS21_ITCM_BASE      0x00080000
#define BS21_ITCM_SIZE      0x00080000   /* I-TCM MPU window 0x80000..0x100000 */
/* DTCM lives at 0x20000000 (APP_DTCM_ORIGIN), NOT carved from the ITCM window —
 * the loaderboot reset code relocates the boot-param block to ~0x20002d50. */
#define BS21_DTCM_BASE      0x20000000
#define BS21_DTCM_SIZE      0x00010000   /* 64K (APP_DTCM_LENGTH) */
#define BS21_FLASH_BASE     0x10000000   /* XIP NOR flash (QSPI) */
#define BS21_FLASH_SIZE     0x00100000   /* 1M */
/* flash1 XIP window (the SFC-mapped QSPI flash): the partition table + firmware
 * live here. flashboot reads the partition table from 0x90100000 (it returns this
 * pointer directly) and checks the magic 0x4b87a52d. */
#define BS21_FLASH1_BASE    0x90100000
#define BS21_FLASH1_SIZE    0x00100000   /* 1M */
#define BS21_SRAM_BASE      0x00100000   /* L2RAM (160K on BS21E/BS22) */
#define BS21_SRAM_SIZE      0x00028000
#define BS21_RESET_PC       0x10000000   /* flash XIP entry */

/* Peripheral MMIO windows (low-priority catch-all absorbers; real devices map
 * on top). M_CTL fabric @0x52000000 (UART/I2C/SPI/PWM/DMA/TIMER/WDT/TRNG),
 * GLB/PMU fabric @0x57000000 (GLB_CTL/PMU/GPIO/RTC/TCXO/FUSE). */
#define BS21_MMIO_MCTL_BASE 0x52000000
#define BS21_MMIO_MCTL_SIZE 0x01000000
#define BS21_MMIO_GLB_BASE  0x57000000
#define BS21_MMIO_GLB_SIZE  0x01000000
/* USB 2.0 OTG controller (Synopsys DWC OTG) @0x58000000 — bs2x-pac models it but
 * no firmware drives it yet; absorb the window so register access doesn't fault. */
#define BS21_MMIO_USB_BASE  0x58000000
#define BS21_MMIO_USB_SIZE  0x00040000
/* riscv31 core private peripheral bus (FlashPatch + SCS), same as WS63. */
#define BS21_PPB_BASE       0xE0000000
#define BS21_PPB_SIZE       0x00010000

/* Peripheral bases (fbb_bs2x). UARTs are tightly packed at 0x1000 spacing:
 * UART1(H0) 0x52080000, UART0(L0) 0x52081000, UART2(L1) 0x52082000. */
#define BS21_UART0_BASE     0x52081000
#define BS21_UART1_BASE     0x52080000
#define BS21_UART2_BASE     0x52082000
#define BS21_TIMER_BASE     0x52002000
#define BS21_GPIO0_BASE     0x57010000
#define BS21_TCXO_BASE      0x57000200
#define BS21_TCXO_SIZE      0x00000200   /* TCXO_COUNT block; GLB_CTL_A @0x57000400 */
#define BS21_SFC_BASE       0x90000000   /* serial-flash controller regs (v150) */
#define BS21_SPI0_BASE      0x52087000   /* SPI_M0 (DesignWare SSI v151) */
#define BS21_GADC_BASE      0x57036000   /* GADC digital block (13-bit ADC v153) */
#define BS21_I2C0_BASE      0x52083000   /* I2C0 (DesignWare SSI v151) */
#define BS21_KEYSCAN_BASE   0x5208D000   /* KEYSCAN (key-matrix v150) */
#define BS21_QDEC_BASE      0x52000200   /* QDEC (quadrature decoder v150) */
#define BS21_RTC_BASE       0x57024100   /* RTC0 (rtc_unified v150) */
#define BS21_TRNG_BASE      0x52009000   /* TRNG (v1) */
#define BS21_WDT_BASE       0x52003000   /* WDT (watchdog v151) */
#define BS21_DMA_BASE       0x52070000   /* MDMA (v151) */

/* IRQ numbers (chip_core_irq.h). 26-31 use standard mie bits; >=32 are LOCI.
 * (BS21's 26-29 are BT/ADC, unlike WS63 where they are TIMER.) */
#define BS21_IRQ_GPIO0      34   /* GPIO_0 (33 = ULP_GPIO) */
#define BS21_IRQ_UART0      39   /* UART_0 */
#define BS21_IRQ_UART1      41   /* UART_1 */
#define BS21_IRQ_UART2      42   /* UART_2 */
#define BS21_IRQ_TIMER0     53   /* TIMER_0..3 = 53..56 (general-purpose) */
/* The LiteOS tick (tick_timer.c) drives TIMER_3 but routes its interrupt to the
 * standard RISC-V machine-timer interrupt MTIP = mip bit 7 (OS_TICK_INT_NUM 7,
 * which is < LOCAL_INTERRUPT0 26 so it is a core, not LOCI, interrupt). */
#define BS21_IRQ_TICK       7    /* MTIP (mip bit 7) — TIMER_3 -> LiteOS tick */

#define TYPE_BS21_MACHINE MACHINE_TYPE_NAME("bs21")
OBJECT_DECLARE_SIMPLE_TYPE(BS21MachineState, BS21_MACHINE)

struct BS21MachineState {
    MachineState parent_obj;
    RISCVCPU cpu;
    MemoryRegion bootrom;
    MemoryRegion rom;
    MemoryRegion itcm;
    MemoryRegion dtcm;
    MemoryRegion flash;
    MemoryRegion flash1;
    MemoryRegion ppb;
    MemoryRegion tcxo_win;
    MemoryRegion clk32k;
    MemoryRegion efuse;
    uint8_t  efuse_data[128];   /* EFUSE_REGION_MAX_BITS 1024 / 8 */
    uint32_t efuse_ctl;         /* 0x030 efuse_wr_rd */
    uint32_t efuse_clk;         /* 0x034 clock_period */
    uint32_t efuse_resv;        /* 0x038 reserved */
    uint32_t efuse_avdd;        /* 0x03c efuse_avdd_sw */
};

/*
 * PMU2_CMU 32K-clock calibration status (PMU2_CMU_CTL_RB_BASE 0x57008000).
 * The vendor app's hal_32k_clock_get_detect_result() (hal_32k_clock.c) starts a
 * calibration then spins:  while (!(*(u16*)0x57008488 & 1)) {}  — waiting for
 * bit0 of HAL_CALIBRATION_32K_CLOCK_DET_STS (base + 0x488) to read 1 ("DONE").
 * The generic GLB absorber returns 0, so the wait never completes. Model the
 * calibration as instantaneously done: DET_STS reads DONE (bit0=1) and not
 * in-progress (bit1 DOING=0); CFG/VAL enable writes are no-ops. The region is
 * mapped at the DET block (0x57008480) so the rest of PMU2_CMU still falls to
 * the absorber. (DET_RES_L/H read 0 for now — extend if a consumer needs them.)
 */
#define BS21_CLK32K_DET_BASE  0x57008480   /* PMU2_CMU + 0x480 (CFG/VAL/STS/RES) */
#define BS21_CLK32K_DET_SIZE  0x00000020
#define BS21_CLK32K_DET_STS   0x08         /* relative to BS21_CLK32K_DET_BASE */

static uint64_t bs21_clk32k_read(void *opaque, hwaddr off, unsigned size)
{
    switch (off) {
    case BS21_CLK32K_DET_STS:
        return 0x1;   /* bit0 DET_DONE=1, bit1 DET_DOING=0 */
    default:
        return 0;
    }
}

static void bs21_clk32k_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    /* calibration enable/cfg/cycle writes are no-ops — calibration is "done". */
}

static const MemoryRegionOps bs21_clk32k_ops = {
    .read = bs21_clk32k_read,
    .write = bs21_clk32k_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * eFUSE v151 controller (EFUSE0 @ FUSE_CTL_RB_ADDR 0x57028000). The vendor app's
 * clock/PMU calibration reads trim values out of eFUSE (hal_efuse_v151, full source
 * in fbb_ws63 .../drivers/hal/efuse/v151). The generic absorber returns 0 for every
 * register — including the boot-done status and the ctl-register readback — which
 * stalls the eFUSE-driven calibration handler the app iterates from its init table.
 * Model the controller per the SDK (register map from hal_efuse_v151_reg_def.h, base
 * offsets from bs2x efuse_porting.c):
 *   0x02c efuse_sts  : [1:0] man_sts, [2] boot0_done, [3] boot1_done, [4] boot2_done
 *   0x030 efuse_ctl  : [15:0] efuse_wr_rd (0x5a5a read / 0xa5a5 write mode + byte addr)
 *   0x034 clk_period : [7:0]
 *   0x038 reserved
 *   0x03c avdd_ctl   : [0] efuse_avdd_sw
 *   0x800+ data      : reg i covers bytes 2i,2i+1 -> efuse[2i] | (efuse[2i+1] << 8)
 * boot-done is reported asserted; the fuse array is blank (all 0), so trim/ctrim reads
 * return 0 and the calibration code takes its documented "use default" path (e.g.
 * calibration_xo_core_ctrim_init). eFUSE programming (write mode) ORs bits into the
 * array (fuses are one-way 0->1). The switch-enable register (0x5702c258) lives in the
 * ULP_AON block and its read-modify-write works through the absorber, so it needs no
 * model here.
 */
#define BS21_EFUSE_BASE      0x57028000
#define BS21_EFUSE_SIZE      0x00001000
#define BS21_EFUSE_STS_OFF   0x02c
#define BS21_EFUSE_CTL_OFF   0x030
#define BS21_EFUSE_CLK_OFF   0x034
#define BS21_EFUSE_RESV_OFF  0x038
#define BS21_EFUSE_AVDD_OFF  0x03c
#define BS21_EFUSE_DATA_OFF  0x800
#define BS21_EFUSE_MAX_BYTES 128

static uint64_t bs21_efuse_read(void *opaque, hwaddr off, unsigned size)
{
    BS21MachineState *s = opaque;
    switch (off) {
    case BS21_EFUSE_STS_OFF:
        return 0x1c;   /* boot0/1/2_done = 1, man_sts = 0 (idle) */
    case BS21_EFUSE_CTL_OFF:
        return s->efuse_ctl;
    case BS21_EFUSE_CLK_OFF:
        return s->efuse_clk;
    case BS21_EFUSE_RESV_OFF:
        return s->efuse_resv;
    case BS21_EFUSE_AVDD_OFF:
        return s->efuse_avdd;
    default:
        break;
    }
    if (off >= BS21_EFUSE_DATA_OFF &&
        off < BS21_EFUSE_DATA_OFF + BS21_EFUSE_MAX_BYTES * 2) {
        unsigned b = ((off - BS21_EFUSE_DATA_OFF) / 4) * 2; /* low byte index */
        return s->efuse_data[b] | ((uint32_t)s->efuse_data[b + 1] << 8);
    }
    return 0;
}

static void bs21_efuse_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    BS21MachineState *s = opaque;
    switch (off) {
    case BS21_EFUSE_CTL_OFF:
        s->efuse_ctl = val & 0xffff;
        return;
    case BS21_EFUSE_CLK_OFF:
        s->efuse_clk = val & 0xff;
        return;
    case BS21_EFUSE_RESV_OFF:
        s->efuse_resv = (uint32_t)val;
        return;
    case BS21_EFUSE_AVDD_OFF:
        s->efuse_avdd = val & 0x1;
        return;
    default:
        break;
    }
    if (off >= BS21_EFUSE_DATA_OFF &&
        off < BS21_EFUSE_DATA_OFF + BS21_EFUSE_MAX_BYTES * 2) {
        /* eFUSE programming: a reg write carries the low byte (even addr, val&0xff)
         * or the high byte (odd addr, (val>>8)&0xff); fuses are one-way -> OR. */
        unsigned b = ((off - BS21_EFUSE_DATA_OFF) / 4) * 2;
        s->efuse_data[b]     |= val & 0xff;
        s->efuse_data[b + 1] |= (val >> 8) & 0xff;
    }
    /* efuse_sts (0x02c) and anything else: read-only / ignored. */
}

static const MemoryRegionOps bs21_efuse_ops = {
    .read = bs21_efuse_read,
    .write = bs21_efuse_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 1, .max_access_size = 4 },
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void bs21_cpu_reset(void *opaque)
{
    RISCVCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
    /*
     * Same boot-stage hand-off ABI as WS63: ws63-rt's startup dereferences a0
     * (the boot-parameter-block pointer set by the previous stage) before mtvec
     * is configured. A standalone "-kernel" boot has no previous stage, so a0
     * would be 0 and the load would fault then double-fault to pc=0. Point a0 at
     * zeroed L2RAM so the boot-reason word reads 0 ("normal boot"). Harmless to
     * firmware that ignores a0.
     */
    cpu->env.gpr[10] = BS21_SRAM_BASE; /* a0 */
}

static void bs21_machine_init(MachineState *machine)
{
    BS21MachineState *s = BS21_MACHINE(machine);
    MemoryRegion *sys = get_system_memory();
    uint64_t entry = BS21_RESET_PC;

    /* LOCI local-interrupt CSRs (identical to WS63 — same riscv31 core). */
    ws63_register_custom_csrs();

    /* RAM-backed memory map (L2RAM is the -m bank). */
    ws63_make_ram(sys, &s->bootrom, "bs21.bootrom", BS21_BOOTROM_BASE, BS21_BOOTROM_SIZE);
    ws63_make_ram(sys, &s->rom, "bs21.rom", BS21_ROM_BASE, BS21_ROM_SIZE);
    ws63_make_ram(sys, &s->itcm, "bs21.itcm", BS21_ITCM_BASE, BS21_ITCM_SIZE);
    ws63_make_ram(sys, &s->dtcm, "bs21.dtcm", BS21_DTCM_BASE, BS21_DTCM_SIZE);
    ws63_make_ram(sys, &s->flash, "bs21.flash", BS21_FLASH_BASE, BS21_FLASH_SIZE);
    ws63_make_ram(sys, &s->flash1, "bs21.flash1", BS21_FLASH1_BASE, BS21_FLASH1_SIZE);
    memory_region_add_subregion(sys, BS21_SRAM_BASE, machine->ram);

    /* Core private peripheral bus (FlashPatch + SCS) — back with RAM so control
     * read-modify-writes are absorbed rather than faulting (see ws63.c). */
    ws63_make_ram(sys, &s->ppb, "bs21.ppb", BS21_PPB_BASE, BS21_PPB_SIZE);

    /* BS21 mask-ROM emulation. The silicon mask ROM (0x10000..0x40000) is not in
     * the SDK; the boot stages validate ROM *data* (a signature/version word) and
     * tail-call ROM *functions* (handled by bs21_rom_call on illegal-instruction
     * traps, patches/<tag>/0005). Here we synthesize the ROM data words the vendor
     * flashboot reads: it checks `*(uint32_t*)0x10020 == 0xd4818193` at its 0x43c8a
     * and, on mismatch, ret's with ra=0 -> jumps to PC 0 -> illegal-instruction
     * panic. Poke the known words into the RAM-backed ROM region (extend as more
     * ROM-data dependencies are found). */
    {
        static const struct { hwaddr addr; uint32_t val; } rom_data[] = {
            { 0x10020, 0xd4818193 },  /* flashboot mask-ROM signature (0x43c8a) */
        };
        uint32_t *rom = memory_region_get_ram_ptr(&s->rom);
        for (int i = 0; i < (int)ARRAY_SIZE(rom_data); i++) {
            rom[(rom_data[i].addr - BS21_ROM_BASE) / 4] = rom_data[i].val;
        }
    }

    /* Catch-all absorbers for un-modeled peripheral MMIO (real devices on top). */
    create_unimplemented_device("bs21.mmio.mctl", BS21_MMIO_MCTL_BASE, BS21_MMIO_MCTL_SIZE);
    create_unimplemented_device("bs21.mmio.glb", BS21_MMIO_GLB_BASE, BS21_MMIO_GLB_SIZE);
    create_unimplemented_device("bs21.mmio.usb", BS21_MMIO_USB_BASE, BS21_MMIO_USB_SIZE);

    /* LOCI interrupt controller (shared model). */
    DeviceState *intc = qdev_new(TYPE_WS63_INTC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);

    /* TCXO clock/counter (shared model; over the absorber). On BS21 the
     * TCXO_COUNT block is only 0x57000200..0x57000400 (TCXO_COUNT_BASE_ADDR;
     * GLB_CTL_A starts right after at 0x57000400) — unlike WS63 where the TCXO
     * sits alone at 0x44000000 with a full 0x1000 region. Alias only the 0x200
     * that is really TCXO here, so the GLB_CTL_A/D registers above it fall through
     * to the glb absorber instead of hitting the TCXO's strict 4-byte handler
     * (the vendor flashboot does sub-word reads of GLB_CTL_A, e.g. 0x570004a0). */
    DeviceState *tcxo = qdev_new(TYPE_WS63_TCXO);
    ws63_tcxo_set_count_off(tcxo, 0);   /* BS21 TCXO_COUNT is at the region base */
    ws63_tcxo_set_chunked16(tcxo, true); /* BS21 TCXO v150: 16-bit count chunks */
    sysbus_realize_and_unref(SYS_BUS_DEVICE(tcxo), &error_fatal);
    MemoryRegion *tcxo_mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(tcxo), 0);
    memory_region_init_alias(&s->tcxo_win, OBJECT(tcxo), "bs21.tcxo", tcxo_mr,
                             0, BS21_TCXO_SIZE);
    memory_region_add_subregion(sys, BS21_TCXO_BASE, &s->tcxo_win);

    /* PMU2_CMU 32K-clock calibration status (over the GLB absorber) — reports the
     * calibration as instantly done so the app's hal_32k_clock detect poll
     * (while (!(*(u16*)0x57008488 & 1))) completes. See bs21_clk32k_ops above. */
    memory_region_init_io(&s->clk32k, OBJECT(machine), &bs21_clk32k_ops, s,
                          "bs21.clk32k", BS21_CLK32K_DET_SIZE);
    memory_region_add_subregion(sys, BS21_CLK32K_DET_BASE, &s->clk32k);

    /* eFUSE v151 controller (over the GLB absorber) — boot-done asserted + a blank
     * fuse array so the app's eFUSE-driven clock/PMU calibration reads its trims (all
     * 0 -> default path) instead of stalling on the unmodelled controller. See
     * bs21_efuse_ops above. */
    memory_region_init_io(&s->efuse, OBJECT(machine), &bs21_efuse_ops, s,
                          "bs21.efuse", BS21_EFUSE_SIZE);
    memory_region_add_subregion(sys, BS21_EFUSE_BASE, &s->efuse);

    /* SFC serial-flash controller (shared v150 model) — models the SPI command
     * interface enough for flash identification (RDID -> JEDEC ID), so the vendor
     * flashboot's early flash init succeeds. BS21's flashboot's flash table is
     * GigaDevice GD25LE80 (sfc_config_info_porting.c) — JEDEC 0xC8/0x60/0x14 =
     * 0x1460C8 (mfr/type/cap packed) — not WS63's Winbond W25Q16; flashboot's
     * detect (0x42122) compares the read ID against 0x1460C8, so report that. */
    DeviceState *sfc = qdev_new(TYPE_WS63_SFC);
    ws63_sfc_set_flash_id(sfc, 0x001460C8);   /* GD25LE80 (GigaDevice, 1MB) */
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sfc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(sfc), 0, BS21_SFC_BASE);

    /* TIMER0..3 (shared model, 4 channels). TIMER_0..2 -> general LOCI IRQs
     * 53..55; TIMER_3 (the LiteOS tick) -> MTIP (mip bit 7). */
    DeviceState *timer = qdev_new(TYPE_WS63_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer), 0, BS21_TIMER_BASE);
    for (int i = 0; i < 3; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(timer), i,
                           qdev_get_gpio_in(intc, BS21_IRQ_TIMER0 + i));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(timer), 3,
                       qdev_get_gpio_in(intc, BS21_IRQ_TICK));

    /* One GPIO bank (GPIO0 @ 0x57010000, IRQ 34). BS21 has 5 banks; M1 blinky
     * uses GPIO0 pin 0, so one bank suffices. */
    DeviceState *gpio = qdev_new(TYPE_WS63_GPIO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, BS21_GPIO0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 0,
                       qdev_get_gpio_in(intc, BS21_IRQ_GPIO0));

    /* SPI0 (DesignWare SSI v151) with TX->RX loopback — lets the chip-bs21 Rust
     * SPI driver be functionally exercised (spi_loopback example). Maps on top of
     * the M_CTL absorber. */
    ws63_create_spi_loopback(BS21_SPI0_BASE);

    /* GADC (13-bit ADC v153) digital block — functionally exercises the chip-bs21
     * Rust gadc driver (gadc_read example). */
    ws63_create_gadc(BS21_GADC_BASE);

    /* I2C0 (DesignWare SSI v151) with one slave @0x50 — exercises the chip-bs21
     * Rust i2c driver's bus scan (i2c_scan example). */
    ws63_create_i2c(BS21_I2C0_BASE);

    /* KEYSCAN + QDEC (BS2X HID) — exercise the chip-bs21 keyscan/qdec drivers. */
    ws63_create_keyscan(BS21_KEYSCAN_BASE);
    ws63_create_qdec(BS21_QDEC_BASE);

    /* RTC0 + TRNG — exercise the chip-bs21 rtc/trng drivers. */
    ws63_create_rtc(BS21_RTC_BASE);
    ws63_create_trng(BS21_TRNG_BASE);

    /* WDT (watchdog v151) — exercises the chip-bs21 wdt driver. */
    ws63_create_wdt(BS21_WDT_BASE);

    /* MDMA (v151) — mem-to-mem copy, exercises the chip-bs21 dma driver. */
    ws63_create_dma(BS21_DMA_BASE);

    /* UART0/1/2 (custom device on top of the absorber). */
    const hwaddr uart_base[3] = { BS21_UART0_BASE, BS21_UART1_BASE, BS21_UART2_BASE };
    const int uart_irq[3] = { BS21_IRQ_UART0, BS21_IRQ_UART1, BS21_IRQ_UART2 };
    for (int i = 0; i < 3; i++) {
        DeviceState *uart = qdev_new(TYPE_WS63_UART);
        qdev_prop_set_chr(uart, "chardev", serial_hd(i));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, uart_base[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0,
                           qdev_get_gpio_in(intc, uart_irq[i]));
    }

    /* Firmware ELF (-kernel). Entry overrides the default reset PC. */
    if (machine->kernel_filename) {
        uint64_t elf_entry;
        if (load_elf(machine->kernel_filename, NULL, NULL, NULL, &elf_entry,
                     NULL, NULL, NULL, 0, EM_RISCV, 0, 0) <= 0) {
            error_report("bs21: could not load kernel ELF '%s'",
                         machine->kernel_filename);
            exit(1);
        }
        entry = elf_entry;
    }

    /*
     * Single RV32IMFC_Zicsr hart. M1 reuses the named `ws63` CPU type, which is
     * the same riscv31 ISA BS21 wants (I/M/F/C + Zicsr/Zifencei/Zicntr/Zcf, no
     * A/D, no MMU; the compressed space reserved for the HiSilicon custom code-
     * size ops). A `bs21`/`hisi-riscv31` CPU alias is deferred to the CPU rename.
     */
    object_initialize_child(OBJECT(machine), "cpu", &s->cpu, machine->cpu_type);
    qdev_prop_set_uint64(DEVICE(&s->cpu), "resetvec", entry);
    s->cpu.env.mhartid = 0;
    qemu_register_reset(bs21_cpu_reset, &s->cpu);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_fatal);

    /* Let the interrupt controller drive this hart. */
    ws63_intc_set_cpu_env(intc, &s->cpu.env);
}

static void bs21_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "HiSilicon BS21 (RV32IMFC BLE 5.4 / SLE NearLink SoC)";
    mc->init = bs21_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = TYPE_RISCV_CPU_WS63;
    mc->default_ram_id = "bs21.sram";
    mc->default_ram_size = BS21_SRAM_SIZE;
}

static const TypeInfo bs21_machine_typeinfo = {
    .name          = TYPE_BS21_MACHINE,
    .parent        = TYPE_MACHINE,
    .class_init    = bs21_machine_class_init,
    .instance_size = sizeof(BS21MachineState),
};

static void bs21_register_types(void)
{
    type_register_static(&bs21_machine_typeinfo);
}

type_init(bs21_register_types)
