/*
 * QEMU machine model for the HiSilicon WS63 RISC-V SoC.
 *
 * WS63 is a Wi-Fi 6 + BLE + SLE (SparkLink) combo SoC built around a single
 * RV32IMFC_Zicsr hart (hardware single-precision float, NO atomic extension),
 * 240 MHz. This board models enough of the SoC to run bare-metal ws63-rs
 * firmware (https://github.com/sanchuanhehe/ws63-rs): the memory map from the
 * ws63-rt linker scripts, a custom HiSilicon UART, and MMIO absorbers for the
 * remaining peripherals. Interrupts (the custom SYS_CTL1 controller) are NOT
 * modeled — blinky (GPIO busy-loop) and polled UART output need no IRQ delivery.
 *
 * CPU: a single RV32IMFC_Zicsr hart, built from QEMU's configurable "rv32" core
 * with exactly I/M/F/C enabled and A (atomics) + D (double float) disabled — a
 * faithful match for the WS63 ISA.
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
 * Memory map — from ws63-rt/memory.x (the addresses the firmware is linked at).
 * These are the *Rust* runtime's view; the C SDK's platform_core.h uses a
 * different/older layout which does NOT match the linked firmware.
 * ------------------------------------------------------------------------- */
#define WS63_BOOTROM_BASE   0x00100000
#define WS63_BOOTROM_SIZE   0x00009000   /* 36 KiB */
#define WS63_ROM_BASE       0x00109000
#define WS63_ROM_SIZE       0x00043000   /* 268 KiB */
#define WS63_ITCM_BASE      0x0014C000
#define WS63_ITCM_SIZE      0x00004000   /* 16 KiB */
#define WS63_DTCM_BASE      0x00180000
#define WS63_DTCM_SIZE      0x00004000   /* 16 KiB */
#define WS63_FLASH_BASE     0x00200000
#define WS63_FLASH_SIZE     0x00800000   /* 8 MiB XIP flash */
#define WS63_SRAM_BASE      0x00A00000
#define WS63_SRAM_SIZE      0x00090000   /* 576 KiB */

/* Reset PC: ORIGIN(PROGRAM) in layout.ld — .startup/.text.entry is linked here
 * and is the ELF entry point. Overridden by the actual ELF entry when -kernel
 * is given. */
#define WS63_RESET_PC       0x00230300

/* MMIO peripheral windows (absorbed unless explicitly modeled). */
#define WS63_MMIO_LOW_BASE  0x40000000   /* SYS_CTL/TIMER/UART/GPIO/SPACC/SFC/DMA */
#define WS63_MMIO_LOW_SIZE  0x10000000   /* .. up to 0x50000000 */
#define WS63_MMIO_SDMA_BASE 0x52000000
#define WS63_MMIO_SDMA_SIZE 0x01000000
#define WS63_MMIO_RTC_BASE  0x57000000   /* RTC + ULP_GPIO */
#define WS63_MMIO_RTC_SIZE  0x01000000

#define WS63_UART0_BASE     0x44010000
#define WS63_UART_MMIO_SIZE 0x1000

/* ============================================================================
 * WS63 UART — HiSilicon custom register layout (NOT 16550-compatible).
 * Modeled from ws63-svd/WS63.svd UART0 + ws63-hal/src/uart.rs + the C SDK
 * (hal_uart_v151_regs_def.h). For TX-only "hello world" + simple echo we need:
 *   - write DATA (0x04)         -> emit the byte to the chardev
 *   - read FIFO_STATUS (0x44)   -> tx never full, tx empty, rx empty unless RX
 *   - read DATA (0x04)          -> pop the received byte
 * Everything else accepts writes and reads back a shadow / zero.
 * ========================================================================= */
#define TYPE_WS63_UART "ws63-uart"
OBJECT_DECLARE_SIMPLE_TYPE(WS63UartState, WS63_UART)

#define UART_DATA           0x04
#define UART_LINE_STATUS    0x34
#define UART_FIFO_STATUS    0x44

/* FIFO_STATUS bit fields (WS63.svd UART0 FIFO_STATUS). */
#define FIFO_TX_FULL        (1u << 0)
#define FIFO_TX_EMPTY       (1u << 1)
#define FIFO_RX_FULL        (1u << 2)
#define FIFO_RX_EMPTY       (1u << 3)

/* LINE_STATUS convenience bits (data_available[0], thre_s, tx_empty_s). */
#define LSR_DATA_AVAIL      (1u << 0)
#define LSR_TX_READY        (3u << 5)   /* thre_s | tx_empty_s, always ready */

struct WS63UartState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    CharBackend chr;
    qemu_irq irq;

    uint8_t rx_byte;
    bool rx_valid;
    /* shadow of config registers so reads return what was written */
    uint32_t shadow[WS63_UART_MMIO_SIZE / 4];
};

static uint64_t ws63_uart_read(void *opaque, hwaddr off, unsigned size)
{
    WS63UartState *s = opaque;

    switch (off) {
    case UART_FIFO_STATUS: {
        uint32_t v = FIFO_TX_EMPTY;            /* TX FIFO drains instantly */
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
        /* Best-effort blocking write to the connected chardev (stdio/file). */
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
 * HiSilicon custom CSRs (RAZ/WI)
 *
 * WS63 firmware touches vendor-custom CSRs that a generic RISC-V core does not
 * implement, which would otherwise raise illegal-instruction traps:
 *   - 0x7C0 / 0x7C1  I-cache / D-cache enable (ws63-rt startup cpu_cache_init)
 *   - 0xFC2 ...       read by the trap handler / vendor code
 * Make the three architecturally-custom CSR ranges read-as-zero / write-ignore.
 * These ranges contain no standard CSRs, so this clobbers nothing.
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

static riscv_csr_operations ws63_csr_razwi = {
    .name = "ws63-custom",
    .predicate = ws63_csr_any,
    .read = ws63_csr_read_zero,
    .write = ws63_csr_write_ignore,
};

static void ws63_register_custom_csrs(void)
{
    int n;
    for (n = 0x7c0; n <= 0x7ff; n++) {
        riscv_set_csr_ops(n, &ws63_csr_razwi);
    }
    for (n = 0xbc0; n <= 0xbff; n++) {
        riscv_set_csr_ops(n, &ws63_csr_razwi);
    }
    for (n = 0xfc0; n <= 0xfff; n++) {
        riscv_set_csr_ops(n, &ws63_csr_razwi);
    }
}

/* ============================================================================
 * WS63 machine
 * ========================================================================= */
#define TYPE_WS63_MACHINE MACHINE_TYPE_NAME("ws63")
OBJECT_DECLARE_SIMPLE_TYPE(WS63MachineState, WS63_MACHINE)

struct WS63MachineState {
    MachineState parent_obj;

    RISCVCPU cpu;
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

/* Reset the hart to its resetvec at machine reset (CPUs created outside a
 * hart-array are not otherwise in the reset path). */
static void ws63_cpu_reset(void *opaque)
{
    cpu_reset(CPU(opaque));
}

static void ws63_machine_init(MachineState *machine)
{
    WS63MachineState *s = WS63_MACHINE(machine);
    MemoryRegion *sys = get_system_memory();
    uint64_t entry = WS63_RESET_PC;

    /* Tolerate WS63's vendor-custom CSRs (cache enable, etc.). */
    ws63_register_custom_csrs();

    /* RAM-backed memory map (flash modeled as RAM so XIP reads + the data
     * relocation the startup performs all just work). SRAM is the -m bank. */
    ws63_make_ram(sys, &s->bootrom, "ws63.bootrom", WS63_BOOTROM_BASE, WS63_BOOTROM_SIZE);
    ws63_make_ram(sys, &s->rom, "ws63.rom", WS63_ROM_BASE, WS63_ROM_SIZE);
    ws63_make_ram(sys, &s->itcm, "ws63.itcm", WS63_ITCM_BASE, WS63_ITCM_SIZE);
    ws63_make_ram(sys, &s->dtcm, "ws63.dtcm", WS63_DTCM_BASE, WS63_DTCM_SIZE);
    ws63_make_ram(sys, &s->flash, "ws63.flash", WS63_FLASH_BASE, WS63_FLASH_SIZE);
    memory_region_add_subregion(sys, WS63_SRAM_BASE, machine->ram);

    /* Absorb all other peripheral MMIO so stray accesses log instead of
     * aborting the VM (low priority — real devices map on top). */
    create_unimplemented_device("ws63.mmio.periph",
                                WS63_MMIO_LOW_BASE, WS63_MMIO_LOW_SIZE);
    create_unimplemented_device("ws63.mmio.sdma",
                                WS63_MMIO_SDMA_BASE, WS63_MMIO_SDMA_SIZE);
    create_unimplemented_device("ws63.mmio.rtc",
                                WS63_MMIO_RTC_BASE, WS63_MMIO_RTC_SIZE);

    /* UART0 — real device on top of the absorber. */
    DeviceState *uart = qdev_new(TYPE_WS63_UART);
    qdev_prop_set_chr(uart, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, WS63_UART0_BASE);

    /* Firmware ELF (-kernel). Entry point overrides the default reset PC. */
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

    /* Single RV32IMFC_Zicsr hart — faithful to WS63: enable I/M/F/C, leave A
     * (atomics) and D (double float) OFF. The configurable "rv32" base CPU
     * starts with no MISA extensions, so we set exactly what the chip has.
     * Reset directly to the firmware entry (no OpenSBI, no FDT — bare-metal). */
    object_initialize_child(OBJECT(machine), "cpu", &s->cpu, machine->cpu_type);
    object_property_set_bool(OBJECT(&s->cpu), "i", true, &error_abort);
    object_property_set_bool(OBJECT(&s->cpu), "m", true, &error_abort);
    object_property_set_bool(OBJECT(&s->cpu), "f", true, &error_abort);
    object_property_set_bool(OBJECT(&s->cpu), "c", true, &error_abort);
    object_property_set_bool(OBJECT(&s->cpu), "a", false, &error_abort);
    object_property_set_bool(OBJECT(&s->cpu), "d", false, &error_abort);
    /* zawrs is on by default for the base rv32 core but requires A; the WS63
     * target (rv32imfc) has neither, so turn it off to keep A disabled. */
    object_property_set_bool(OBJECT(&s->cpu), "zawrs", false, &error_abort);
    qdev_prop_set_uint64(DEVICE(&s->cpu), "resetvec", entry);
    s->cpu.env.mhartid = 0;
    qemu_register_reset(ws63_cpu_reset, &s->cpu);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_fatal);
}

static void ws63_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "HiSilicon WS63 (RV32IMFC Wi-Fi6/BLE/SLE SoC)";
    mc->init = ws63_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE32; /* configured to rv32imfc in init */
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
    type_register_static(&ws63_uart_typeinfo);
    type_register_static(&ws63_machine_typeinfo);
}

type_init(ws63_register_types)
