/*
 * QEMU machine model for the HiSilicon BS21E / BS2X RISC-V SoC (minimal / M1).
 *
 * BS21E is another member of the HiSilicon BS2X family (BLE + SLE / NearLink, NO
 * Wi-Fi), built on the same "HimiDeer" riscv31 core as WS63/BS21 — a single
 * RV32IMFC_Zicsr hart (hardware single-precision float, no atomics). The whole
 * BS2X family shares one set of peripheral base addresses (platform_core.h) and
 * IRQ numbers (chip_core_irq.h), and BS21E's M1 memory map (160K L2RAM, ITCM/DTCM/
 * flash origins) is identical to BS22's and to our BS21 machine — so at the
 * milestone-1 level BS21E, BS21 and BS22 are register- and memory-identical, and
 * this machine reuses the shared device models from hw/riscv/ws63.c (declared in
 * hisi_riscv31.h), exactly like bs21.c/bs22.c. (BS21E is the SDK SKU our generic
 * "bs21" maps to; `-M bs21e` is the explicit SKU name, the same clean M1 + full
 * functional-peripheral machine as `-M bs22`, without bs21.c's vendor-C-SDK boot.
 * The family diverges only where M1 doesn't reach: BS20's smaller 128K RAM, and
 * each chip's vendor-firmware ROM/eFUSE/SFC bring-up.)
 *
 * Milestone-1 scope: boot bare-metal ws63-rs firmware built for the BS2X family
 * (examples/bs21/{blinky,uart_hello}, `--features chip-bs21`, which selects the
 * shared bs2x-pac at these addresses). That Rust firmware emits only standard
 * RV32IMFC and never calls the mask ROM, so this machine needs neither the
 * HiSilicon custom-ISA decoder nor ROM interception (both carried by bs21.c for
 * the vendor C-SDK boot and deferred here with connectivity). Modeled: the CPU,
 * the BS21E memory map, the custom UART (x3), one GPIO bank, the TIMER, the TCXO,
 * the LOCI interrupt controller + custom CSRs, and unimplemented-device absorbers
 * for the MMIO windows. NOT modeled (vendor-C-firmware only, add when BS21E vendor
 * boot is pursued): eFUSE, SFC, the 32K-clock-detect status, the mask-ROM
 * signature, and the flash1 XIP window.
 *
 * Facts from the fbb_bs2x SDK (platform_core.h / chip_core_irq.h /
 * chips/bs21e/board/memory_config); see docs/bs21-recon.md.
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
 * Memory map — from the fbb_bs2x SDK (platform_core.h + chips/bs21e memory_config),
 * matching the BS2X examples/bs21 memory.x (BS21E == BS21E at M1). Code runs XIP
 * from NOR flash @0x10000000; data/bss/stack live in L2RAM @0x00100000 (the -m
 * bank, 160K on BS21E/BS21E).
 * ------------------------------------------------------------------------- */
#define BS21E_BOOTROM_BASE   0x00000000
#define BS21E_BOOTROM_SIZE   0x00008000   /* 32K mask-ROM MPU window */
#define BS21E_ROM_BASE       0x00008000
#define BS21E_ROM_SIZE       0x00078000   /* ROM symbols extend to ~0x80000 */
#define BS21E_ITCM_BASE      0x00080000
#define BS21E_ITCM_SIZE      0x00080000   /* I-TCM MPU window 0x80000..0x100000 */
#define BS21E_DTCM_BASE      0x000F0000   /* M1 (Rust memory.x) DTCM: top of TCM */
#define BS21E_DTCM_SIZE      0x00010000   /* 64K */
#define BS21E_FLASH_BASE     0x10000000   /* XIP NOR flash (QSPI) */
#define BS21E_FLASH_SIZE     0x00100000   /* 1M */
#define BS21E_SRAM_BASE      0x00100000   /* L2RAM (160K on BS21E/BS21E) */
#define BS21E_SRAM_SIZE      0x00028000
#define BS21E_RESET_PC       0x10000000   /* flash XIP entry */

/* Peripheral MMIO windows (low-priority catch-all absorbers; real devices map
 * on top). M_CTL fabric @0x52000000 (UART/I2C/SPI/PWM/DMA/TIMER/WDT/TRNG),
 * GLB/PMU fabric @0x57000000 (GLB_CTL/PMU/GPIO/RTC/TCXO/FUSE). */
#define BS21E_MMIO_MCTL_BASE 0x52000000
#define BS21E_MMIO_MCTL_SIZE 0x01000000
#define BS21E_MMIO_GLB_BASE  0x57000000
#define BS21E_MMIO_GLB_SIZE  0x01000000
/* USB 2.0 OTG controller (Synopsys DWC OTG) @0x58000000 — absorb the window. */
#define BS21E_MMIO_USB_BASE  0x58000000
#define BS21E_MMIO_USB_SIZE  0x00040000
/* riscv31 core private peripheral bus (FlashPatch + SCS), same as WS63/BS21. */
#define BS21E_PPB_BASE       0xE0000000
#define BS21E_PPB_SIZE       0x00010000

/* Peripheral bases (fbb_bs2x platform_core.h, shared across BS2X). UARTs are
 * tightly packed at 0x1000 spacing: UART1(H0) 0x52080000, UART0(L0) 0x52081000,
 * UART2(L1) 0x52082000. */
#define BS21E_UART0_BASE     0x52081000
#define BS21E_UART1_BASE     0x52080000
#define BS21E_UART2_BASE     0x52082000
#define BS21E_TIMER_BASE     0x52002000
#define BS21E_GPIO0_BASE     0x57010000
#define BS21E_TCXO_BASE      0x57000200
#define BS21E_TCXO_SIZE      0x00000200   /* TCXO_COUNT block; GLB_CTL_A @0x57000400 */
#define BS21E_SPI0_BASE      0x52087000   /* SPI_M0 (DesignWare SSI v151) */
#define BS21E_GADC_BASE      0x57036000   /* GADC digital block (13-bit ADC v153) */
#define BS21E_I2C0_BASE      0x52083000   /* I2C0 (DesignWare SSI v151) */
#define BS21E_KEYSCAN_BASE   0x5208D000   /* KEYSCAN (key-matrix v150) */
#define BS21E_QDEC_BASE      0x52000200   /* QDEC (quadrature decoder v150) */
#define BS21E_RTC_BASE       0x57024100   /* RTC0 (rtc_unified v150) */
#define BS21E_TRNG_BASE      0x52009000   /* TRNG (v1) */
#define BS21E_WDT_BASE       0x52003000   /* WDT (watchdog v151) */
#define BS21E_DMA_BASE       0x52070000   /* MDMA (v151) */
#define BS21E_PDM_BASE       0x5208E000   /* PDM (audio mic v150) */

/* IRQ numbers (chip_core_irq.h, shared across BS2X). 26-31 use standard mie bits;
 * >=32 are LOCI. */
#define BS21E_IRQ_GPIO0      34   /* GPIO_0 (LOCAL_INTERRUPT0 26 + 8) */
#define BS21E_IRQ_UART0      39   /* UART_0 */
#define BS21E_IRQ_UART1      41   /* UART_1 */
#define BS21E_IRQ_UART2      42   /* UART_2 */
#define BS21E_IRQ_TIMER0     53   /* TIMER_0..3 = 53..56 (general-purpose) */
/* The LiteOS tick drives TIMER_3 but routes its interrupt to the standard RISC-V
 * machine-timer interrupt MTIP = mip bit 7 (OS_TICK_INT_NUM 7). */
#define BS21E_IRQ_TICK       7    /* MTIP (mip bit 7) — TIMER_3 -> LiteOS tick */

#define TYPE_BS21E_MACHINE MACHINE_TYPE_NAME("bs21e")
OBJECT_DECLARE_SIMPLE_TYPE(BS21EMachineState, BS21E_MACHINE)

struct BS21EMachineState {
    MachineState parent_obj;
    RISCVCPU cpu;
    MemoryRegion bootrom;
    MemoryRegion rom;
    MemoryRegion itcm;
    MemoryRegion dtcm;
    MemoryRegion flash;
    MemoryRegion ppb;
    MemoryRegion tcxo_win;
};

static void bs21e_cpu_reset(void *opaque)
{
    RISCVCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
    /*
     * Same boot-stage hand-off ABI as WS63/BS21: ws63-rt's startup dereferences
     * a0 (the boot-parameter-block pointer set by the previous stage) before
     * mtvec is configured. A standalone "-kernel" boot has no previous stage, so
     * point a0 at zeroed L2RAM so the boot-reason word reads 0 ("normal boot").
     */
    cpu->env.gpr[10] = BS21E_SRAM_BASE; /* a0 */
}

static void bs21e_machine_init(MachineState *machine)
{
    BS21EMachineState *s = BS21E_MACHINE(machine);
    MemoryRegion *sys = get_system_memory();
    uint64_t entry = BS21E_RESET_PC;

    /* LOCI local-interrupt CSRs (identical to WS63 — same riscv31 core). */
    ws63_register_custom_csrs();

    /* RAM-backed memory map (L2RAM is the -m bank). */
    ws63_make_ram(sys, &s->bootrom, "bs21e.bootrom", BS21E_BOOTROM_BASE, BS21E_BOOTROM_SIZE);
    ws63_make_ram(sys, &s->rom, "bs21e.rom", BS21E_ROM_BASE, BS21E_ROM_SIZE);
    ws63_make_ram(sys, &s->itcm, "bs21e.itcm", BS21E_ITCM_BASE, BS21E_ITCM_SIZE);
    ws63_make_ram(sys, &s->dtcm, "bs21e.dtcm", BS21E_DTCM_BASE, BS21E_DTCM_SIZE);
    ws63_make_ram(sys, &s->flash, "bs21e.flash", BS21E_FLASH_BASE, BS21E_FLASH_SIZE);
    memory_region_add_subregion(sys, BS21E_SRAM_BASE, machine->ram);

    /* Core private peripheral bus (FlashPatch + SCS) — back with RAM so control
     * read-modify-writes are absorbed rather than faulting (see ws63.c). */
    ws63_make_ram(sys, &s->ppb, "bs21e.ppb", BS21E_PPB_BASE, BS21E_PPB_SIZE);

    /* Catch-all absorbers for un-modeled peripheral MMIO (real devices on top). */
    create_unimplemented_device("bs21e.mmio.mctl", BS21E_MMIO_MCTL_BASE, BS21E_MMIO_MCTL_SIZE);
    create_unimplemented_device("bs21e.mmio.glb", BS21E_MMIO_GLB_BASE, BS21E_MMIO_GLB_SIZE);
    create_unimplemented_device("bs21e.mmio.usb", BS21E_MMIO_USB_BASE, BS21E_MMIO_USB_SIZE);

    /* LOCI interrupt controller (shared model). */
    DeviceState *intc = qdev_new(TYPE_WS63_INTC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);

    /* TCXO clock/counter (shared model; over the absorber). The TCXO_COUNT block
     * is 0x57000200..0x57000400 (GLB_CTL_A starts right after at 0x57000400), so
     * alias only the 0x200 that is really TCXO — the rest of GLB falls through to
     * the absorber. BS2X TCXO v150 uses 16-bit count chunks at the region base. */
    DeviceState *tcxo = qdev_new(TYPE_WS63_TCXO);
    ws63_tcxo_set_count_off(tcxo, 0);    /* BS2X TCXO_COUNT is at the region base */
    ws63_tcxo_set_chunked16(tcxo, true); /* BS2X TCXO v150: 16-bit count chunks */
    sysbus_realize_and_unref(SYS_BUS_DEVICE(tcxo), &error_fatal);
    MemoryRegion *tcxo_mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(tcxo), 0);
    memory_region_init_alias(&s->tcxo_win, OBJECT(tcxo), "bs21e.tcxo", tcxo_mr,
                             0, BS21E_TCXO_SIZE);
    memory_region_add_subregion(sys, BS21E_TCXO_BASE, &s->tcxo_win);

    /* TIMER0..3 (shared model, 4 channels). TIMER_0..2 -> general LOCI IRQs
     * 53..55; TIMER_3 (the LiteOS tick) -> MTIP (mip bit 7). */
    DeviceState *timer = qdev_new(TYPE_WS63_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer), 0, BS21E_TIMER_BASE);
    for (int i = 0; i < 3; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(timer), i,
                           qdev_get_gpio_in(intc, BS21E_IRQ_TIMER0 + i));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(timer), 3,
                       qdev_get_gpio_in(intc, BS21E_IRQ_TICK));

    /* One GPIO bank (GPIO0 @ 0x57010000, IRQ 34). M1 blinky uses GPIO0 pin 0. */
    DeviceState *gpio = qdev_new(TYPE_WS63_GPIO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, BS21E_GPIO0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 0,
                       qdev_get_gpio_in(intc, BS21E_IRQ_GPIO0));

    /* SPI0 (DesignWare SSI v151) TX->RX loopback — functionally exercises the
     * chip-bs21 Rust SPI driver (spi_loopback example). */
    ws63_create_spi_loopback(BS21E_SPI0_BASE);

    /* GADC (13-bit ADC v153) digital block — functionally exercises the chip-bs21
     * Rust gadc driver (gadc_read example). */
    ws63_create_gadc(BS21E_GADC_BASE);

    /* I2C0 (DesignWare SSI v151) with one slave @0x50 — exercises the chip-bs21
     * Rust i2c driver's bus scan (i2c_scan example). */
    ws63_create_i2c(BS21E_I2C0_BASE);

    /* KEYSCAN + QDEC (BS2X HID) — exercise the chip-bs21 keyscan/qdec drivers. */
    ws63_create_keyscan(BS21E_KEYSCAN_BASE);
    ws63_create_qdec(BS21E_QDEC_BASE);

    /* RTC0 + TRNG — exercise the chip-bs21 rtc/trng drivers. */
    ws63_create_rtc(BS21E_RTC_BASE);
    ws63_create_trng(BS21E_TRNG_BASE);

    /* WDT (watchdog v151) — exercises the chip-bs21 wdt driver. */
    ws63_create_wdt(BS21E_WDT_BASE);

    /* MDMA (v151) — mem-to-mem copy, exercises the chip-bs21 dma driver. */
    ws63_create_dma(BS21E_DMA_BASE);

    /* PDM (audio mic v150) — config-level, exercises the chip-bs21 pdm driver. */
    ws63_create_pdm(BS21E_PDM_BASE);

    /* USB (DWC OTG) core-ID model over the USB absorber — exercises the usb driver. */
    ws63_create_usb(BS21E_MMIO_USB_BASE);

    /* UART0/1/2 (custom device on top of the absorber). */
    const hwaddr uart_base[3] = { BS21E_UART0_BASE, BS21E_UART1_BASE, BS21E_UART2_BASE };
    const int uart_irq[3] = { BS21E_IRQ_UART0, BS21E_IRQ_UART1, BS21E_IRQ_UART2 };
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
            error_report("bs21e: could not load kernel ELF '%s'",
                         machine->kernel_filename);
            exit(1);
        }
        entry = elf_entry;
    }

    /*
     * Single RV32IMFC_Zicsr hart. M1 reuses the named `ws63` CPU type, which is
     * the same riscv31 ISA BS21E wants (I/M/F/C + Zicsr/Zifencei/Zicntr/Zcf, no
     * A/D, no MMU). A `bs21e`/`hisi-riscv31` CPU alias is deferred to the CPU
     * rename.
     */
    object_initialize_child(OBJECT(machine), "cpu", &s->cpu, machine->cpu_type);
    qdev_prop_set_uint64(DEVICE(&s->cpu), "resetvec", entry);
    s->cpu.env.mhartid = 0;
    qemu_register_reset(bs21e_cpu_reset, &s->cpu);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_fatal);

    /* Let the interrupt controller drive this hart. */
    ws63_intc_set_cpu_env(intc, &s->cpu.env);
}

static void bs21e_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "HiSilicon BS21E (RV32IMFC BLE / SLE NearLink SoC, minimal)";
    mc->init = bs21e_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = TYPE_RISCV_CPU_WS63;
    mc->default_ram_id = "bs21e.sram";
    mc->default_ram_size = BS21E_SRAM_SIZE;
}

static const TypeInfo bs21e_machine_typeinfo = {
    .name          = TYPE_BS21E_MACHINE,
    .parent        = TYPE_MACHINE,
    .class_init    = bs21e_machine_class_init,
    .instance_size = sizeof(BS21EMachineState),
};

static void bs21e_register_types(void)
{
    type_register_static(&bs21e_machine_typeinfo);
}

type_init(bs21e_register_types)
