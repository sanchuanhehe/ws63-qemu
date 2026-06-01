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
- 新增单文件 `hw/riscv/ws63.c`（机器模型 + 外设设备），经 `meson.build` / `Kconfig` 两处极小注入接入构建；
  另加一个 `patches/ws63-target-riscv.patch`（CPU 核的自定义本地中断投递，见「中断控制器」）。均由
  `scripts/build.sh` 自动注入/应用，幂等。
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
| UART0/1/2 | 自定义 `ws63-uart` SysBusDevice @ `0x4401_0000/1000/2000` |
| TIMER | `ws63-timer`（3 个下数计数器 @ `0x4400_2000`，到点产生中断 26/27/28） |
| GPIO0/1/2 | `ws63-gpio`（输出 set/clr、输入、中断寄存器 @ `0x4402_8000/9000/A000`） |
| SYS_CTL0 | `ws63-sysctl0`（时钟状态：TCXO + PLL 已锁，使 `init_clocks()` 不空转） |
| 中断控制器 | `ws63-intc`：自定义 `LOCIxx` CSR 状态 + IRQ 路由（见下） |
| 其余外设 | `create_unimplemented_device` 吸收（三窗口），`-d unimp` 按地址可追踪 |

## 中断控制器（ws63-intc）

WS63 用 HiSilicon 自定义的「riscv31」核内 CLIC 式方案，不是 CLINT/PLIC。设备 IRQ 分两类：

- **IRQ 26–31（TIMER_0/1/2、RTC、I2C0）**：用**标准 `mie` 位**。固件经真实 `mie` CSR 使能；
  `ws63-intc` 收到外设 IRQ 线后用 `riscv_cpu_update_mip(env, 1<<n)` 拉高 `mip[n]`，QEMU 经
  **向量化 mtvec**（mode 1）派发到 `mtvec + 4*n`。**完整保真、已实测**（见 `timer_irq` 示例）。
- **IRQ ≥32（GPIO=33、UART=53…LSADC=72）**：用核内自定义 CSR `LOCIEN0-2`(0xBE0)/`LOCIPD0-2`(0xBE8)/
  `LOCIPCLR`(0xBF0)，且 mcause 取值 32–72 **放不进 RV32 的 32 位 mip/mie**。**已通过 `target/riscv` 补丁
  实现核内投递**（见 `patches/ws63-target-riscv.patch`）：`CPUArchState` 加 `ws63_locien/locipd`；
  `riscv_cpu_local_irq_pending()` 在标准 mip/mie 检查之后增查 `locipd & locien`（受 `mstatus.MIE` 门控），
  返回 IRQ 号；`riscv_cpu_do_interrupt()` 既有逻辑即以 `mcause = irq` 投递（向量化时 `mtvec + 4*irq`）。
  设备经新导出的 `riscv_cpu_set_local_irq(env, irq, level)` 置/清 `locipd`；`LOCIEN`/`LOCIPCLR` CSR 写
  更新 enable/清 pending。**完整投递、已实测**（见 `gpio_irq` 示例，GPIO0 pin0→IRQ 33）。
  - 简化：优先级 `LOCIPRI`/阈值 `PRITHD` 暂不强制（按 IRQ 号从小到大投递）；可后续细化。

> 即：mie 类（26–31）与自定义本地类（≥32）两条中断线现均已端到端验证。

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

## 外设建模矩阵

| 外设 | 状态 | 说明 |
|------|------|------|
| CPU (rv32imfc) / 内存 / 复位 / ELF 载入 | ✅ 真实 | — |
| **xlinx 自定义 ISA** | ✅ 真实 | HiSilicon riscv31 私有指令（l.li/\*shf/b\*i/muliadd/jal16/ldmia/push-pop/压缩 lbu-sb…），**厂商 gcc 固件必需**；见 [xlinx-isa.md](xlinx-isa.md) |
| UART0/1/2 | ✅ 真实 | 自定义 HiSilicon 寄存器；TX→chardev，RX←chardev（中断使能时触发 IRQ 53/54/55）|
| TIMER ×3 | ✅ 真实 | 下数计数器 + 中断（26/27/28），周期重载 |
| GPIO0/1/2 | ✅ 真实 | 输出 set/clr、输入读、边沿/电平中断 + 输出→输入回环（可自触发 IRQ） |
| SYS_CTL0 | ✅ 真实(部分) | 仅时钟状态（TCXO/PLL 锁）；其余读 0 |
| **TCXO 时钟/计数器** | ✅ 真实(部分) | `0x440004C0`：bit4 count-valid + 64 位单调计数（+0x04/+0x08），供 bootloader us 级延时 |
| **PPB（核内私有外设总线）** | 🟡 RAM 吸收 | `0xE0000000` FlashPatch 单元 + Cortex-M 式 SCS（`0xE000E000`）；加载已打补丁镜像故补丁单元无意义 |
| 中断控制器 | ✅ 真实 | IRQ 26–31（mie 类）+ ≥32（自定义 LOCIxx，target/riscv 补丁）均完整投递；优先级阈值未强制 |
| **SFC（Flash 控制器）** | ✅ 真实(部分) | SPI 命令接口（RDID→W25Q16、RDSR→ready、命令完成）；flash XIP 内容未回填 |
| **I2C0/1**（0x44018000/0x44018100）| ✅ **真实(行为完整)** | 真实回环 FIFO：TXR→RXR 多字节顺序回读、SR 完成位、COM 命令位自动清、IRQ 31/32 |
| **SPI0/1**（0x44020000/0x44021000）| ✅ **真实(行为完整)** | 真实回环 FIFO：DR 写入→顺序读回、WSR 反映 FIFO（rxfne/txfe）、RLR 深度、IRQ 43/52 |
| **PWM**（0x44024000）| ✅ 真实(部分) | 寄存器影子 + PERIODLOAD_FLAG=1 + START 自清 |
| **I2S**（0x44025000）| ✅ 真实(行为完整) | LEFT/RIGHT TX→RX 回环 |
| **LSADC**（0x4400C000）| ✅ **真实(行为完整)** | CTRL_8 触发转换 → rne=1/bsy=0、CTRL_9 弹出 14-bit 采样 + 完成 IRQ 72（读清）|
| **EFUSE**（0x44008000）| ✅ 真实(部分) | STS boot-done 位 + 数据窗影子（标定内容无 dump）|
| **WDT**（0x40006000）| ✅ **真实(行为完整)** | QEMU 定时器倒计时；超时未喂狗则真复位 SoC（裸机测试验证）|
| **RTC**（0x57024000）| ✅ **真实(行为完整)** | QEMU 定时器周期触发 **IRQ 29** + INT_STATUS/EOI；CURRENT_VALUE 计数 |
| **DMA/SDMA**（0x4A000000/0x520A0000）| ✅ **真实(行为完整)** | 通道使能即**真正搬运内存**（src→dst，按宽度/地址自增），置传输完成位，按 tc_int_en 触发 **IRQ 59**；INT_CLR 清除（裸机测试验证）|
| **TRNG**（0x44114000）| ✅ 真实 | FIFO_READY=ready、FIFO_DATA 伪随机（xorshift）|
| **SPACC / PKE / KM**（密码学）| 🟡 影子 | 寄存器影子（**未在启动路径**：mbedtls 用 ROM 表软件 AES）。真实 AES/SHA/RSA 可经 QEMU crypto 库实现，但 SPACC v2 多通道描述符协议复杂且无固件触发，列为按需扩展 |
| **CLDO_CRG / IO_CONFIG / TSENSOR / RF_WB_CTL / SHARE_MEM / FAMA_REMAP / ULP_GPIO**（影子）| 🟡 影子 | 读写寄存器影子（驱动写后可读回）；RF_WB_CTL 的无线电/PHY 不仿真，仅寄存器配置影子 |

> **覆盖度**：`WS63.svd` 的全部 **35 个外设**现均有模型（无裸 catch-all 黑洞）。
> 「行为完整」= 真实数据搬运/计时/中断（DMA/RTC/WDT/Timer/I2C/SPI/I2S/LSADC/GPIO/UART）；
> 「影子」= 寄存器读写一致（配置类，驱动不会因读回 0 而挂死）。**无法真实仿真**的只有模拟量
> （ADC 电压、TSENSOR 温度——只能合成）与 **RF/PHY 射频**（RF_WB_CTL/WiFi/BT，物理边界）。

## 运行厂商 C SDK 固件（多角度对齐）

除了 ws63-rs（Rust，标准 rv32imfc），本仿真器现在也能运行 **fbb_ws63 C SDK** 厂商 gcc 编译的固件
——这需要实现 HiSilicon 的 **xlinx 自定义 ISA**（[xlinx-isa.md](xlinx-isa.md)）。两侧固件对照可交叉验证
内存映射、启动、外设寄存器时序与驱动逻辑。

| C SDK 镜像 | 现状 | 边界 |
|------------|------|------|
| `flashboot` / `loaderboot`（bootloader，**零掩膜 ROM 依赖**）| ✅ 跑出 UART 输出（时钟 bring-up→flash init）| 下一阻塞 = SFC Flash 控制器（未建模，flash init 优雅失败不挂死）|
| `ws63-liteos-app`（主应用，**裁剪 BT+WiFi**）| ✅ **稳定启动到调度器并空转运行**（`cpu 0 entering scheduler` + `radar_timer_init succ`，timer IRQ 26 周期触发，无崩溃）| 仅剩 flash/NV/efuse 内容相关的非致命 `fail` 打印（UPG/NV）；BT/WiFi 任务需子系统 ROM 数据/RF 硬件，已裁剪（`config.py` 注释 `BGLE/BTH/WIFI_TASK_EXIST`）|
| `ws63-liteos-app`（含 BT/WiFi）| ✅ 启动 LiteOS、创建全部 14 子系统任务、进入调度器 | `bt`/`wifi` 任务深层初始化崩于**ROM 数据墙**（vtable/NV/efuse/RF 标定无 dump）|

构建 C SDK：`cd fbb_ws63/src && python3 build.py ws63-liteos-app -ninja`（厂商工具链已内置）。
运行：`qemu-system-riscv32 -M ws63 -nographic -serial mon:stdio -kernel <image>.elf`。

## 可观察的验证目标（均已实测 PASS，见 `scripts/smoke-test.sh`）

| 固件 | 触及 | 在 QEMU 中如何观察成功 |
|------|------|------------------------|
| `blinky` | GPIO0 输出翻转 | 0 非法指令陷阱；`ws63-gpio` 经 `-d` 打印 `out=...` 翻转 |
| `uart_hello` | UART0（跳过 `init_clocks`） | `-serial mon:stdio` 打印 `Hello from WS63 on QEMU!` |
| `timer_irq` | TIMER_0 → IRQ 26 → ISR | 串口打印 `timer irq #N` 递增 + `OK: timer interrupts delivered`（mie 类中断端到端） |
| `gpio_irq` | GPIO0 pin0 → IRQ 33 → ISR | 串口打印 `gpio irq #N` + `OK: custom local IRQ (>=32) delivered`（**≥32 自定义本地中断端到端**） |

## 已知简化（未来工作）

1. **中断优先级 / 阈值未强制**：`LOCIPRI`(2 位优先级) / `PRITHD`(阈值) 仅读回存储，投递按 IRQ 号从小到大
   而非按优先级抢占。多数固件不依赖；可后续在 `ws63_local_irq_pending()` 中加优先级比较。
2. **多数外设仅吸收**（见矩阵）。按 `ws63-timer`/`ws63-gpio` 的模式可逐个增量建模。
3. **CPU/时钟非周期精确**：定时器以名义 24 MHz 计时；无真实 PLL/时钟树。时序不保真。
4. 固定 v9.2.4；升级到 v10.x LTS 需注意 API 变化（如 `class_init` 的 `const void *data`、`sysemu/`→`system/` 头文件改名）。
