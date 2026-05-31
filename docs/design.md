# ws63-qemu 设计说明

## 目标

在 QEMU 上仿真 HiSilicon WS63（RISC-V）SoC，让 [`ws63-rs`](https://github.com/sanchuanhehe/ws63-rs)
裸机固件（blinky、UART 打印）无需真实硬件即可运行——为 ROADMAP 阶段 1「硬件在环 bring-up」
提供一个**软件在环**替代信号：证明内存布局、startup（PMP/FPU/cache/栈/数据重定位）、链接脚本
在一个 WS63 地址空间模型上能正确跑起来。

## 方法：fork 一个固定版本的 QEMU（仿 esp-qemu）

QEMU 没有稳定的「树外板卡插件」ABI，因此自定义 SoC 的标准做法是 fork 并加一个 in-tree 板卡文件
——Espressif 的 esp-qemu 即如此（`hw/riscv/esp32c3.c` + 自定义 CPU/外设）。本项目同样：

- 固定 **QEMU v9.2.4**（稳定线；`scripts/build.sh` 克隆该 tag）。
- 新增单文件 `hw/riscv/ws63.c`（机器模型 + 自定义 UART 设备），经 `meson.build` / `Kconfig` 两处
  极小注入接入构建（`scripts/build.sh` 自动完成，幂等）。
- 只构建 `riscv32-softmmu` 一个目标，控制构建时间（~10–20 分钟）。

> 为什么不用树外补丁：board 没有稳定插件 ABI；为什么不用 `-M virt`：固件按 WS63 地址链接，
> 首次访问 WS63 外设就会在 virt 上 fault。

## 机器模型（`hw/riscv/ws63.c`）

| 组件 | 实现 |
|------|------|
| 机器类型 | `MACHINE_TYPE_NAME("ws63")` → `-M ws63` |
| CPU | 单 hart，由可配置 `rv32` 核精确设为 **rv32imfc**（开 I/M/F/C，关 A/D/zawrs）|
| 复位 | `resetvec` = `-kernel` ELF 的 entry（缺省 `0x230300`）；无 OpenSBI/FDT |
| 内存 | BOOTROM/ROM/ITCM/DTCM/FLASH 作 RAM，SRAM 作 `-m` bank（见 [memory-map](memory-map.md)） |
| 固件载入 | `load_elf(-kernel, …, EM_RISCV, …)`，按 ELF 物理地址落段 |
| UART0 | 自定义 `ws63-uart` SysBusDevice @ `0x4401_0000` |
| 其余外设 | `create_unimplemented_device` 吸收（三个窗口），`-d unimp` 可追踪 |
| 中断 | **不建模**（WS63 是自定义 SYS_CTL1 控制器，非 CLINT/PLIC）；blinky/轮询 UART 无需中断 |

## 自定义 UART 设备

WS63 UART **不是** 16550（这是关键，QEMU 自带 `serial_mm` 不可用）。它是 HiSilicon 定制布局，
经 `WS63.svd` UART0 + `ws63-hal/src/uart.rs` + SDK `hal_uart_v151_regs_def.h` 核实：

| 偏移 | 寄存器 | 16550? |
|------|--------|--------|
| `0x00` | INTR_ID | ✗（16550 是 RBR/THR） |
| `0x04` | **DATA**（读=RX，写=TX） | ✗（16550 DATA 在 0x00） |
| `0x08` | UART_CTL | ✗ |
| `0x0C/0x10/0x14` | DIV_H / DIV_L / DIV_FRA（16 位整数 + 6 位小数分频） | ✗ |
| `0x34` | LINE_STATUS | 位定义不同于 16550 LSR |
| `0x44` | **FIFO_STATUS**：tx_full[0]/tx_empty[1]/rx_full[2]/rx_empty[3] | ✗ |

HAL 的 TX 路径（`uart.rs` `write_byte`）：轮询 `FIFO_STATUS.tx_fifo_full`（0x44 bit0）为 0 →
写 `DATA`（0x04）。模型据此：

- 读 `FIFO_STATUS` → TX 永远「空且不满」（瞬时排空），RX 视收到的字节而定。
- 写 `DATA` → `qemu_chr_fe_write_all` 输出到 chardev（`-serial mon:stdio`）。
- 读 `DATA` → 弹出收到的字节（最小 RX，支持回显）。
- 其余寄存器：接受写、读回 shadow / 0。

## 可观察的验证目标

| 固件 | 触及 | 在 QEMU 中如何观察成功 |
|------|------|------------------------|
| `blinky`（已存在于 ws63-examples） | 仅 GPIO0（`0x4402_8000`）+ 栈 | 不崩溃即证明 CPU/内存布局/startup 正确；`-d unimp` 可见对 GPIO0 的写 |
| `uart_hello`（本项目新增到 ws63-examples） | UART0（跳过 `init_clocks`） | `-serial mon:stdio` 上打印出字符串 |

## 已知简化（未来工作）

1. **无中断控制器**。要驱动中断式驱动 / 连接性，需实现 `ws63-intc`（SYS_CTL1 窗口 + 局部中断线接 CPU）。
2. **SYS_CTL0 / CLDO_CRG 仅吸收**。要让调用 `init_clocks()` 的固件完整跑通时钟初始化，需把 TCXO 检测、
   PLL 锁定位建模为返回「已锁定」。
3. **外设多为吸收**。GPIO/Timer/SPI/I2C 等可按 SVD 增量建模（带状态、可追踪引脚电平）。
4. 固定 v9.2.4；升级到 v10.x LTS 需注意 API 变化（如 `class_init` 的 `const void *data`、`sysemu/`→`system/` 头文件改名）。
