/*
 * QEMU machine model for the HiSilicon BS22 / BS2X RISC-V SoC (minimal / M1).
 *
 * BS22 is another member of the HiSilicon BS2X family (BLE + SLE / NearLink, NO
 * Wi-Fi), built on the same "HimiDeer" riscv31 core as WS63/BS21 — a single
 * RV32IMFC_Zicsr hart (hardware single-precision float, no atomics). The whole
 * BS2X family shares one set of peripheral base addresses (platform_core.h) and
 * IRQ numbers (chip_core_irq.h), and BS22's M1 memory map (160K L2RAM, ITCM/DTCM/
 * flash origins) is identical to BS21E's — so at the milestone-1 level BS22 and
 * BS21 are register- and memory-identical, and this machine reuses the shared
 * device models from hw/riscv/ws63.c (declared in hisi_riscv31.h), exactly like
 * bs21.c. (The chips diverge only where M1 doesn't reach: BS20's smaller 128K
 * RAM, and each chip's vendor-firmware ROM/eFUSE/SFC bring-up.)
 *
 * Milestone-1 scope: boot bare-metal ws63-rs firmware built for the BS2X family
 * (examples/bs21/{blinky,uart_hello}, `--features chip-bs21`, which selects the
 * shared bs2x-pac at these addresses). That Rust firmware emits only standard
 * RV32IMFC and never calls the mask ROM, so this machine needs neither the
 * HiSilicon custom-ISA decoder nor ROM interception (both carried by bs21.c for
 * the vendor C-SDK boot and deferred here with connectivity). Modeled: the CPU,
 * the BS22 memory map, the custom UART (x3), one GPIO bank, the TIMER, the TCXO,
 * the LOCI interrupt controller + custom CSRs, and unimplemented-device absorbers
 * for the MMIO windows. NOT modeled (vendor-C-firmware only, add when BS22 vendor
 * boot is pursued): eFUSE, SFC, the 32K-clock-detect status, the mask-ROM
 * signature, and the flash1 XIP window.
 *
 * Facts from the fbb_bs2x SDK (platform_core.h / chip_core_irq.h /
 * chips/bs22/board/memory_config); see docs/bs21-recon.md.
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
 * Memory map — from the fbb_bs2x SDK (platform_core.h + chips/bs22 memory_config),
 * matching the BS2X examples/bs21 memory.x (BS22 == BS21E at M1). Code runs XIP
 * from NOR flash @0x10000000; data/bss/stack live in L2RAM @0x00100000 (the -m
 * bank, 160K on BS21E/BS22).
 * ------------------------------------------------------------------------- */
#define BS22_BOOTROM_BASE   0x00000000
#define BS22_BOOTROM_SIZE   0x00008000   /* 32K mask-ROM MPU window */
#define BS22_ROM_BASE       0x00008000
#define BS22_ROM_SIZE       0x00078000   /* ROM symbols extend to ~0x80000 */
#define BS22_ITCM_BASE      0x00080000
#define BS22_ITCM_SIZE      0x00080000   /* I-TCM MPU window 0x80000..0x100000 */
#define BS22_DTCM_BASE      0x000F0000   /* M1 (Rust memory.x) DTCM: top of TCM */
#define BS22_DTCM_SIZE      0x00010000   /* 64K */
#define BS22_FLASH_BASE     0x10000000   /* XIP NOR flash (QSPI) */
#define BS22_FLASH_SIZE     0x00100000   /* 1M */
#define BS22_SRAM_BASE      0x00100000   /* L2RAM (160K on BS21E/BS22) */
#define BS22_SRAM_SIZE      0x00028000
#define BS22_RESET_PC       0x10000000   /* flash XIP entry */

/* Peripheral MMIO windows (low-priority catch-all absorbers; real devices map
 * on top). M_CTL fabric @0x52000000 (UART/I2C/SPI/PWM/DMA/TIMER/WDT/TRNG),
 * GLB/PMU fabric @0x57000000 (GLB_CTL/PMU/GPIO/RTC/TCXO/FUSE). */
#define BS22_MMIO_MCTL_BASE 0x52000000
#define BS22_MMIO_MCTL_SIZE 0x01000000
#define BS22_MMIO_GLB_BASE  0x57000000
#define BS22_MMIO_GLB_SIZE  0x01000000
/* USB 2.0 OTG controller (Synopsys DWC OTG) @0x58000000 — absorb the window. */
#define BS22_MMIO_USB_BASE  0x58000000
#define BS22_MMIO_USB_SIZE  0x00040000
/* riscv31 core private peripheral bus (FlashPatch + SCS), same as WS63/BS21. */
#define BS22_PPB_BASE       0xE0000000
#define BS22_PPB_SIZE       0x00010000

/* Peripheral bases (fbb_bs2x platform_core.h, shared across BS2X). UARTs are
 * tightly packed at 0x1000 spacing: UART1(H0) 0x52080000, UART0(L0) 0x52081000,
 * UART2(L1) 0x52082000. */
#define BS22_UART0_BASE     0x52081000
#define BS22_UART1_BASE     0x52080000
#define BS22_UART2_BASE     0x52082000
#define BS22_TIMER_BASE     0x52002000
#define BS22_GPIO0_BASE     0x57010000
#define BS22_TCXO_BASE      0x57000200
#define BS22_TCXO_SIZE      0x00000200   /* TCXO_COUNT block; GLB_CTL_A @0x57000400 */
#define BS22_SPI0_BASE      0x52087000   /* SPI_M0 (DesignWare SSI v151) */
#define BS22_GADC_BASE      0x57036000   /* GADC digital block (13-bit ADC v153) */
#define BS22_I2C0_BASE      0x52083000   /* I2C0 (DesignWare SSI v151) */
#define BS22_KEYSCAN_BASE   0x5208D000   /* KEYSCAN (key-matrix v150) */
#define BS22_QDEC_BASE      0x52000200   /* QDEC (quadrature decoder v150) */
#define BS22_RTC_BASE       0x57024100   /* RTC0 (rtc_unified v150) */
#define BS22_TRNG_BASE      0x52009000   /* TRNG (v1) */
#define BS22_WDT_BASE       0x52003000   /* WDT (watchdog v151) */

/* IRQ numbers (chip_core_irq.h, shared across BS2X). 26-31 use standard mie bits;
 * >=32 are LOCI. */
#define BS22_IRQ_GPIO0      34   /* GPIO_0 (LOCAL_INTERRUPT0 26 + 8) */
#define BS22_IRQ_UART0      39   /* UART_0 */
#define BS22_IRQ_UART1      41   /* UART_1 */
#define BS22_IRQ_UART2      42   /* UART_2 */
#define BS22_IRQ_TIMER0     53   /* TIMER_0..3 = 53..56 (general-purpose) */
/* The LiteOS tick drives TIMER_3 but routes its interrupt to the standard RISC-V
 * machine-timer interrupt MTIP = mip bit 7 (OS_TICK_INT_NUM 7). */
#define BS22_IRQ_TICK       7    /* MTIP (mip bit 7) — TIMER_3 -> LiteOS tick */

#define TYPE_BS22_MACHINE MACHINE_TYPE_NAME("bs22")
OBJECT_DECLARE_SIMPLE_TYPE(BS22MachineState, BS22_MACHINE)

struct BS22MachineState {
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

static void bs22_cpu_reset(void *opaque)
{
    RISCVCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
    /*
     * Same boot-stage hand-off ABI as WS63/BS21: ws63-rt's startup dereferences
     * a0 (the boot-parameter-block pointer set by the previous stage) before
     * mtvec is configured. A standalone "-kernel" boot has no previous stage, so
     * point a0 at zeroed L2RAM so the boot-reason word reads 0 ("normal boot").
     */
    cpu->env.gpr[10] = BS22_SRAM_BASE; /* a0 */
}

static void bs22_machine_init(MachineState *machine)
{
    BS22MachineState *s = BS22_MACHINE(machine);
    MemoryRegion *sys = get_system_memory();
    uint64_t entry = BS22_RESET_PC;

    /* LOCI local-interrupt CSRs (identical to WS63 — same riscv31 core). */
    ws63_register_custom_csrs();

    /* RAM-backed memory map (L2RAM is the -m bank). */
    ws63_make_ram(sys, &s->bootrom, "bs22.bootrom", BS22_BOOTROM_BASE, BS22_BOOTROM_SIZE);
    ws63_make_ram(sys, &s->rom, "bs22.rom", BS22_ROM_BASE, BS22_ROM_SIZE);
    ws63_make_ram(sys, &s->itcm, "bs22.itcm", BS22_ITCM_BASE, BS22_ITCM_SIZE);
    ws63_make_ram(sys, &s->dtcm, "bs22.dtcm", BS22_DTCM_BASE, BS22_DTCM_SIZE);
    ws63_make_ram(sys, &s->flash, "bs22.flash", BS22_FLASH_BASE, BS22_FLASH_SIZE);
    memory_region_add_subregion(sys, BS22_SRAM_BASE, machine->ram);

    /* Core private peripheral bus (FlashPatch + SCS) — back with RAM so control
     * read-modify-writes are absorbed rather than faulting (see ws63.c). */
    ws63_make_ram(sys, &s->ppb, "bs22.ppb", BS22_PPB_BASE, BS22_PPB_SIZE);

    /* Catch-all absorbers for un-modeled peripheral MMIO (real devices on top). */
    create_unimplemented_device("bs22.mmio.mctl", BS22_MMIO_MCTL_BASE, BS22_MMIO_MCTL_SIZE);
    create_unimplemented_device("bs22.mmio.glb", BS22_MMIO_GLB_BASE, BS22_MMIO_GLB_SIZE);
    create_unimplemented_device("bs22.mmio.usb", BS22_MMIO_USB_BASE, BS22_MMIO_USB_SIZE);

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
    memory_region_init_alias(&s->tcxo_win, OBJECT(tcxo), "bs22.tcxo", tcxo_mr,
                             0, BS22_TCXO_SIZE);
    memory_region_add_subregion(sys, BS22_TCXO_BASE, &s->tcxo_win);

    /* TIMER0..3 (shared model, 4 channels). TIMER_0..2 -> general LOCI IRQs
     * 53..55; TIMER_3 (the LiteOS tick) -> MTIP (mip bit 7). */
    DeviceState *timer = qdev_new(TYPE_WS63_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer), 0, BS22_TIMER_BASE);
    for (int i = 0; i < 3; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(timer), i,
                           qdev_get_gpio_in(intc, BS22_IRQ_TIMER0 + i));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(timer), 3,
                       qdev_get_gpio_in(intc, BS22_IRQ_TICK));

    /* One GPIO bank (GPIO0 @ 0x57010000, IRQ 34). M1 blinky uses GPIO0 pin 0. */
    DeviceState *gpio = qdev_new(TYPE_WS63_GPIO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, BS22_GPIO0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 0,
                       qdev_get_gpio_in(intc, BS22_IRQ_GPIO0));

    /* SPI0 (DesignWare SSI v151) TX->RX loopback — functionally exercises the
     * chip-bs21 Rust SPI driver (spi_loopback example). */
    ws63_create_spi_loopback(BS22_SPI0_BASE);

    /* GADC (13-bit ADC v153) digital block — functionally exercises the chip-bs21
     * Rust gadc driver (gadc_read example). */
    ws63_create_gadc(BS22_GADC_BASE);

    /* I2C0 (DesignWare SSI v151) with one slave @0x50 — exercises the chip-bs21
     * Rust i2c driver's bus scan (i2c_scan example). */
    ws63_create_i2c(BS22_I2C0_BASE);

    /* KEYSCAN + QDEC (BS2X HID) — exercise the chip-bs21 keyscan/qdec drivers. */
    ws63_create_keyscan(BS22_KEYSCAN_BASE);
    ws63_create_qdec(BS22_QDEC_BASE);

    /* RTC0 + TRNG — exercise the chip-bs21 rtc/trng drivers. */
    ws63_create_rtc(BS22_RTC_BASE);
    ws63_create_trng(BS22_TRNG_BASE);

    /* WDT (watchdog v151) — exercises the chip-bs21 wdt driver. */
    ws63_create_wdt(BS22_WDT_BASE);

    /* UART0/1/2 (custom device on top of the absorber). */
    const hwaddr uart_base[3] = { BS22_UART0_BASE, BS22_UART1_BASE, BS22_UART2_BASE };
    const int uart_irq[3] = { BS22_IRQ_UART0, BS22_IRQ_UART1, BS22_IRQ_UART2 };
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
            error_report("bs22: could not load kernel ELF '%s'",
                         machine->kernel_filename);
            exit(1);
        }
        entry = elf_entry;
    }

    /*
     * Single RV32IMFC_Zicsr hart. M1 reuses the named `ws63` CPU type, which is
     * the same riscv31 ISA BS22 wants (I/M/F/C + Zicsr/Zifencei/Zicntr/Zcf, no
     * A/D, no MMU). A `bs22`/`hisi-riscv31` CPU alias is deferred to the CPU
     * rename.
     */
    object_initialize_child(OBJECT(machine), "cpu", &s->cpu, machine->cpu_type);
    qdev_prop_set_uint64(DEVICE(&s->cpu), "resetvec", entry);
    s->cpu.env.mhartid = 0;
    qemu_register_reset(bs22_cpu_reset, &s->cpu);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_fatal);

    /* Let the interrupt controller drive this hart. */
    ws63_intc_set_cpu_env(intc, &s->cpu.env);
}

static void bs22_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "HiSilicon BS22 (RV32IMFC BLE / SLE NearLink SoC, minimal)";
    mc->init = bs22_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = TYPE_RISCV_CPU_WS63;
    mc->default_ram_id = "bs22.sram";
    mc->default_ram_size = BS22_SRAM_SIZE;
}

static const TypeInfo bs22_machine_typeinfo = {
    .name          = TYPE_BS22_MACHINE,
    .parent        = TYPE_MACHINE,
    .class_init    = bs22_machine_class_init,
    .instance_size = sizeof(BS22MachineState),
};

static void bs22_register_types(void)
{
    type_register_static(&bs22_machine_typeinfo);
}

type_init(bs22_register_types)
