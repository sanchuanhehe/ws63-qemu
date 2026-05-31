# ws63-qemu

在 **QEMU** 上仿真 HiSilicon **WS63**（RISC-V RV32IMFC，Wi-Fi 6 + BLE + SLE/星闪 SoC），
用于无硬件运行 [`ws63-rs`](https://github.com/sanchuanhehe/ws63-rs) 裸机固件。

仿照 [esp-qemu](https://github.com/espressif/qemu) 的做法：fork 一个固定版本的 QEMU，加入一个
in-tree 板卡文件 `hw/riscv/ws63.c`（WS63 机器模型 + 自定义 UART），只构建 `riscv32-softmmu`。

> 状态：**MVP**。可运行 blinky（GPIO 忙等）与 uart_hello（串口打印）。未建模中断控制器、
> 大多数外设作 MMIO 吸收。详见 [docs/design.md](docs/design.md)。

## 快速开始

```bash
# 1. 安装构建依赖（Debian/Ubuntu，需 sudo）
bash scripts/setup-deps.sh

# 2. 克隆固定版 QEMU、注入 WS63 板卡、构建 qemu-system-riscv32（~10–20 分钟）
bash scripts/build.sh
#   产物：./qemu/build/qemu-system-riscv32，且 `-M help` 含 "ws63"

# 3. 用 ws63 工具链构建固件（在 ws63-rs 仓库中）
cd ../ws63-rs && cargo build -p blinky --release && cd -

# 4. 运行
bash scripts/run.sh                 # 默认跑 ws63-rs 的 blinky
bash scripts/run.sh ../ws63-rs/target/riscv32imfc-unknown-none-elf/release/uart_hello
#   退出 QEMU：Ctrl-A 然后 X
```

直接调用：

```bash
./qemu/build/qemu-system-riscv32 -M ws63 -nographic -serial mon:stdio \
    -kernel <firmware.elf>
```

调试 / 追踪（看到未建模外设的访问、guest 错误）：

```bash
DEBUG=1 bash scripts/run.sh <firmware.elf>   # 写 qemu.log
# 或： -d int,unimp,guest_errors -D qemu.log
```

## 它做了什么

| 方面 | 说明 |
|------|------|
| 机器 | `-M ws63`：单 **RV32IMFC** hart（由可配置 rv32 核精确设为 I/M/F/C，关 A/D），WS63 内存映射，复位到 ELF entry |
| UART0 | `0x4401_0000` 的**自定义 HiSilicon UART**（非 16550）；TX 直接输出到 `-serial` |
| 内存 | BOOTROM/ROM/ITCM/DTCM/FLASH/SRAM，按 `ws63-rt/memory.x` 布局（见 [docs/memory-map.md](docs/memory-map.md)） |
| 其它外设 | `create_unimplemented_device` 吸收（GPIO/Timer/SPI/I2C/…），`-d unimp` 可追踪 |
| 中断 | 未建模（WS63 自定义 SYS_CTL1，非 CLINT/PLIC）——blinky 与轮询 UART 无需中断 |

## 目录结构

```
ws63-qemu/
├── src/hw/riscv/ws63.c     # 机器模型 + 自定义 UART 设备（注入 QEMU 树）
├── scripts/
│   ├── setup-deps.sh        # 安装构建依赖
│   ├── build.sh             # 克隆 QEMU@tag + 注入 + 构建（幂等）
│   └── run.sh               # 运行固件 ELF 的包装脚本
├── docs/
│   ├── design.md            # 设计、UART 寄存器、简化项
│   └── memory-map.md        # 内存映射 + 外设基址（真值来源）
└── qemu/                    # 由 build.sh 克隆的 QEMU 树（.gitignore）
```

## 真值来源

- 内存布局：`ws63-rs/ws63-rt/{memory.x,layout.ld}`（固件链接地址）。
- 外设基址 / 寄存器：`ws63-rs/ws63-svd/WS63.svd`。
- UART 行为：`ws63-rs/ws63-hal/src/uart.rs` + fbb_ws63 `hal_uart_v151_regs_def.h`。

## 许可

`src/hw/riscv/ws63.c` 派生自 QEMU 代码风格，按 **GPL-2.0-or-later** 授权（与 QEMU 一致）。
