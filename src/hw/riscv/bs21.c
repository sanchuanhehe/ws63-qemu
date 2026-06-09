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
#define BS21_ITCM_SIZE      0x00070000   /* 512K I-TCM window (DTCM carved off top) */
#define BS21_DTCM_BASE      0x000F0000
#define BS21_DTCM_SIZE      0x00010000
#define BS21_FLASH_BASE     0x10000000   /* XIP NOR flash (QSPI) */
#define BS21_FLASH_SIZE     0x00100000   /* 1M */
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

/* IRQ numbers (chip_core_irq.h). 26-31 use standard mie bits; >=32 are LOCI.
 * (BS21's 26-29 are BT/ADC, unlike WS63 where they are TIMER.) */
#define BS21_IRQ_GPIO0      34   /* GPIO_0 (33 = ULP_GPIO) */
#define BS21_IRQ_UART0      39   /* UART_0 */
#define BS21_IRQ_UART1      41   /* UART_1 */
#define BS21_IRQ_UART2      42   /* UART_2 */
#define BS21_IRQ_TIMER0     53   /* TIMER_0..3 = 53..56 */

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
    MemoryRegion ppb;
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
    memory_region_add_subregion(sys, BS21_SRAM_BASE, machine->ram);

    /* Core private peripheral bus (FlashPatch + SCS) — back with RAM so control
     * read-modify-writes are absorbed rather than faulting (see ws63.c). */
    ws63_make_ram(sys, &s->ppb, "bs21.ppb", BS21_PPB_BASE, BS21_PPB_SIZE);

    /* Catch-all absorbers for un-modeled peripheral MMIO (real devices on top). */
    create_unimplemented_device("bs21.mmio.mctl", BS21_MMIO_MCTL_BASE, BS21_MMIO_MCTL_SIZE);
    create_unimplemented_device("bs21.mmio.glb", BS21_MMIO_GLB_BASE, BS21_MMIO_GLB_SIZE);
    create_unimplemented_device("bs21.mmio.usb", BS21_MMIO_USB_BASE, BS21_MMIO_USB_SIZE);

    /* LOCI interrupt controller (shared model). */
    DeviceState *intc = qdev_new(TYPE_WS63_INTC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);

    /* TCXO clock/counter (shared model; over the absorber). */
    DeviceState *tcxo = qdev_new(TYPE_WS63_TCXO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(tcxo), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(tcxo), 0, BS21_TCXO_BASE);

    /* TIMER0..3 (shared model; IRQ 53..). The model exposes 3 channels. */
    DeviceState *timer = qdev_new(TYPE_WS63_TIMER);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer), 0, BS21_TIMER_BASE);
    for (int i = 0; i < 3; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(timer), i,
                           qdev_get_gpio_in(intc, BS21_IRQ_TIMER0 + i));
    }

    /* One GPIO bank (GPIO0 @ 0x57010000, IRQ 34). BS21 has 5 banks; M1 blinky
     * uses GPIO0 pin 0, so one bank suffices. */
    DeviceState *gpio = qdev_new(TYPE_WS63_GPIO);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, BS21_GPIO0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 0,
                       qdev_get_gpio_in(intc, BS21_IRQ_GPIO0));

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
