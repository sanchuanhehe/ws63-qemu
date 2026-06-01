# 多角度对齐分析：ws63-rs HAL ↔ C SDK ↔ SVD（QEMU 为交叉验证台）

> 目标：用 **ws63-qemu** 同时运行 **ws63-rs**（Rust 裸机）与 **fbb_ws63 C SDK**（厂商 gcc）两侧固件，
> 在「外设寄存器布局/时序」「内存映射」「启动」「中断」多个维度交叉验证 ws63-rs HAL 的正确性。

## 方法（四方对齐）

| 来源 | 角色 |
|------|------|
| `ws63-svd/WS63.svd` | 寄存器布局的「名义真值」（厂商 SVD）|
| `ws63-hal`（Rust）| 被验证对象：ws63-rs 的 HAL 驱动 |
| `fbb_ws63`（C SDK）| 权威参照：厂商在产硬件上验证过的驱动 |
| **ws63-qemu 运行时** | **交叉验证台**：QEMU 外设模型按 SVD + rs HAL 建模；**若 C SDK 固件在同一模型上也能正确工作，则证明 rs HAL 的寄存器模型与 C SDK 一致** |

运行时交叉验证是最强证据：QEMU 的 UART/timer/中断/内存模型源自 **rs HAL + SVD**；而厂商 gcc 编译的
**C SDK 固件**（flashboot、ws63-liteos-app）在同一模型上跑出正确 UART 输出、定时器中断、启动流程
——等于用厂商实现反向核验了 rs HAL。

## UART（最强对齐，两侧均经此打印）

| 寄存器 | SVD / rs HAL (`uart.rs`) | C SDK (`hal_uart_v151_regs_def.h`) | QEMU 模型 (`ws63.c`) |
|--------|---|---|---|
| `data`（RX 读/TX 写）| **0x04** | **0x04**（`intr_id@0x00, data@0x04`）| `UART_DATA 0x04` |
| `line_status` | 0x34 | **0x34** | `UART_LINE_STATUS 0x34` |
| `fifo_status` | 0x44（`tx_full[0]/tx_empty[1]/rx_full[2]/rx_empty[3]`）| **0x44** | `UART_FIFO_STATUS 0x44`，同位定义 |

- **TX 时序一致**：rs HAL `write_byte` 轮询 `fifo_status.tx_fifo_full`(0x44 bit0) 为 0 → 写 `data`(0x04)；
  C SDK `hal_uart_v151` 经 `hal_uart_is_tx_fifo_full`（读 `fifo_status`）+ `hal_uart_get_data_register`(`&->data`) 同序。
- **关键结论**：WS63 UART **不是 16550**（`data` 在 0x04 而非 0x00；自定义 `fifo_status@0x44`）——
  rs HAL、C SDK、SVD 三方一致，且两侧固件在 QEMU 同一模型上打印成功 → **rs HAL 的 UART 寄存器模型正确**。

## 时钟 / TCXO

- TCXO 计数器 `0x440004C0`（bit4 count-valid + 64 位计数 +0x04/+0x08）：C SDK `hal_tcxo`（flashboot）
  实际使用，QEMU 据此建模后 bootloader 时钟 bring-up 通过。rs HAL 当前以名义 24 MHz 工作，未依赖该计数器
  ——**对齐发现**：若 rs 要做 us 级延时/时间戳，应对接 `0x440004C0`（与 C SDK 一致），这是一条 rs HAL 待补齐项。
- SYS_CTL0 时钟状态（TCXO/PLL 锁，`0x40000000`）：两侧 `init_clocks` 均经此，QEMU 模型同时满足。

## 定时器 / 中断

- **TIMER_0 → IRQ 26**：rs 示例 `timer_irq` 与 C SDK `radar_timer_init`（`[RADAR_LOG] radar_timer_init succ`）
  **都用 IRQ 26**（标准 `mie` 位 + 向量化 mtvec）——两侧在 QEMU 同一中断模型上闭环，**IRQ 号与投递路径对齐**。
- IRQ≥32（自定义 `LOCIxx` 本地中断）：rs 示例 `gpio_irq`（GPIO0→IRQ 33）验证；C SDK 同走该通路。

## 内存映射 / 启动

- BOOTROM/ROM/ITCM/DTCM/FLASH/SRAM 布局（`ws63-rt/memory.x` ↔ C SDK `memory_config_common.h`）一致：
  两侧固件在同一 QEMU 内存映射上正确 relocation/启动（rs blinky 等；C SDK app 完成 relocation/dyn_mem_cfg）。
- 外设基址（UART `0x4401_x000`、TIMER `0x4400_2000`、GPIO `0x4402_8000`、SFC `0x4800_0000`、EFUSE `0x4400_8000`）
  ：SVD ↔ C SDK 头文件 ↔ rs HAL/PAC 三方一致。

## 发现与差异

1. ✅ **UART/timer/中断/内存映射/外设基址**：rs HAL 与 C SDK 完全对齐（同寄存器、同位、同时序），运行时双向验证。
2. 🟡 **TCXO 计数器**：C SDK 用 `0x440004C0` 做时间基准；rs HAL 暂用名义频率，未对接——建议补齐以对齐时序语义。
3. 🟡 **指令集**：C SDK 用 HiSilicon **xlinx 自定义指令**（GCC），rs 用标准 rv32imfc——功能等价，密度不同
   （见 [`rust-toolchain-xlinx.md`](rust-toolchain-xlinx.md)）。
4. ⬜ **flash/NV/efuse 内容**：需真实分区/标定数据（无 dump），属数据墙而非寄存器布局差异。

> 总评：在已建模的外设维度上，**ws63-rs HAL 的寄存器布局与时序与厂商 C SDK 一致**，并经「两侧固件跑在
> 同一 QEMU 模型」运行时交叉验证。唯一明确的 rs 待补齐项是 TCXO 时间基准对接。
