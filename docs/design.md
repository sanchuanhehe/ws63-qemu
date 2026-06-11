# ws63-qemu 设计说明

## 目标

在 QEMU 上仿真 HiSilicon WS63（RISC-V）SoC，让 [`ws63-rs`](https://github.com/hispark-rs/ws63-rs)
裸机固件（blinky、UART 打印）无需真实硬件即可运行——为 ROADMAP 阶段 1「硬件在环 bring-up」
提供一个**软件在环**替代信号：证明内存布局、startup（PMP/FPU/cache/栈/数据重定位）、链接脚本
在一个 WS63 地址空间模型上能正确跑起来。

## 方法：fork 一个固定版本的 QEMU（仿 esp-qemu）

QEMU 没有稳定的「树外板卡插件」ABI，因此自定义 SoC 的标准做法是 fork 并加一个 in-tree 板卡文件
——Espressif 的 esp-qemu 即如此（`hw/riscv/esp32c3.c` + 自定义 CPU/外设）。本项目同样：

- 默认基线 **QEMU v10.0.0**（`scripts/build.sh` 克隆该 tag；从 v9.2.4 升级而来）。**同时维护 v10.2.3、v11.0.1 与 v9.2.4**。
- 新增单文件 `hw/riscv/ws63.c`（机器模型 + 外设设备）+ `insn_trans/trans_xlinx.c.inc`（xlinx 解码器)+ `tests/qtest/ws63-test.c`
  作为**新文件直接拷入**（不冲突，保留在 `src/` 便于编辑，跟随默认基线 v10）；对**既有 QEMU 文件的改动**走
  **按版本分目录的 patch-series** `patches/<QEMU_TAG>/0001..*.patch`（`git format-patch` 生成):`0001` target/riscv 核
  (CPU 型号 + 本地中断 + xlinx hooks + ROM 拦截)、`0002` 注册机器(meson/Kconfig/trace-events)、`0003` 注册 qtest;
  旧版本另带 `0004` 适配 ws63.c 的旧 API。`build.sh` 选 `patches/$QEMU_TAG/` 拷入 + `git apply`,幂等。见
  [`patches/README.md`](../patches/README.md)。
- 只构建 `riscv32-softmmu` 一个目标，控制构建时间（~10–20 分钟）。

> 为什么不用树外补丁：board 没有稳定插件 ABI；为什么不用 `-M virt`：固件按 WS63 地址链接，
> 首次访问 WS63 外设就会在 virt 上 fault。

## 机器模型（`hw/riscv/ws63.c`）

| 组件 | 实现 |
|------|------|
| 机器类型 | `MACHINE_TYPE_NAME("ws63")` → `-M ws63` |
| CPU | 单 hart，命名 CPU `-cpu ws63`（默认型号）= **rv32imfc**（I/M/F/C + Zicsr/Zcf，关 A/D，无 MMU，禁 Zcb/Zcmp 让位 xlinx）|
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
  实现核内投递**（见 patch-series `patches/<QEMU_TAG>/0001-target-riscv-*.patch`）：`CPUArchState` 加 `ws63_locien/locipd`；
  `riscv_cpu_local_irq_pending()` 在标准 mip/mie 检查之后增查 `locipd & locien`（受 `mstatus.MIE` 门控），
  返回 IRQ 号；`riscv_cpu_do_interrupt()` 既有逻辑即以 `mcause = irq` 投递（向量化时 `mtvec + 4*irq`）。
  设备经新导出的 `riscv_cpu_set_local_irq(env, irq, level)` 置/清 `locipd`；`LOCIEN`/`LOCIPCLR` CSR 写
  更新 enable/清 pending。**完整投递、已实测**（见 `gpio_irq` 示例，GPIO0 pin0→IRQ 33）。
  - **优先级 / 阈值已强制**：`ws63_local_irq_pending()` 读 `LOCIPRI`（每 IRQ 4 位，8 个/寄存器）取优先级，
    仅当 `优先级 > PRITHD`（严格大于）才投递，多个候选取最高优先级、同级取最小 IRQ 号；复位默认每 IRQ 优先级 1、
    阈值 0（即退化为按号投递，兼容旧行为）。CSR 写经 `ws63_loci_write` 镜像入 `env->ws63_locipri[]/ws63_prithd`。
    已用 LOCIPRI/PRITHD 探针实测（屏蔽 / 抢占 / 严格 `>` 边界 5/5 通过）。

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
| CPU (rv32imfc) / 内存 / 复位 / ELF 载入 | ✅ 真实 | 命名 CPU `-cpu ws63`（默认型号）:I/M/F/C + Zicsr/Zcf,关 A/D、无 MMU,禁用 Zcb/Zcmp(让位 xlinx 压缩编码);见 `target/riscv` 补丁 |
| **xlinx 自定义 ISA** | ✅ 真实 | HiSilicon riscv31 私有指令（l.li/\*shf/b\*i/muliadd/jal16/ldmia/push-pop/压缩 lbu-sb…），**厂商 gcc 固件必需**；见 [xlinx-isa.md](xlinx-isa.md) |
| UART0/1/2 | ✅ 真实 | 自定义 HiSilicon 寄存器；TX→chardev，RX←chardev（中断使能时触发 IRQ 53/54/55）|
| TIMER ×3 | ✅ 真实 | 下数计数器 + 中断（26/27/28），周期重载 |
| GPIO0/1/2 | ✅ **真实(行为完整)** | 输出 set/clr、输入读、边沿/电平中断；引脚为**真实信号网**：bank 内输出→输入回环 + **跨 bank 板级连线**（GPIO0 输出脚驱动 GPIO1 输入脚，可观测+中断，裸机验证）+ 可由 monitor/外部设备驱动 |
| SYS_CTL0 | ✅ 真实(部分) | 仅时钟状态（TCXO/PLL 锁）；其余读 0 |
| **TCXO 时钟/计数器** | ✅ 真实(部分) | `0x440004C0`：bit4 count-valid + 64 位单调计数（+0x04/+0x08），供 bootloader us 级延时 |
| **PPB（核内私有外设总线）** | 🟡 RAM 吸收 | `0xE0000000` FlashPatch 单元 + Cortex-M 式 SCS（`0xE000E000`）；加载已打补丁镜像故补丁单元无意义 |
| 中断控制器 | ✅ 真实 | IRQ 26–31（mie 类）+ ≥32（自定义 LOCIxx，target/riscv 补丁）均完整投递；`LOCIPRI` 优先级 + `PRITHD` 阈值已强制（严格 `>`、最高优先级优先、同级取小号）|
| **SFC（Flash 控制器）** | ✅ 真实(部分) | SPI 命令接口（RDID→W25Q16、RDSR→ready、命令完成）；flash XIP 窗口（0x200000）为 RAM 背靠，默认空——可用 `run.sh NV=1` 回填分区表+NV（见下「NV 仿真」）|
| **I2C0/1**（0x44018000/0x44018100）| ✅ **真实(行为完整)** | 真实回环 FIFO：TXR→RXR 多字节顺序回读、SR 完成位、COM 命令位自动清、IRQ 31/32 |
| **SPI0/1**（0x44020000/0x44021000）| ✅ **真实(行为完整)** | 真实回环 FIFO：DR 写入→顺序读回、WSR 反映 FIFO（rxfne/txfe）、RLR 深度、IRQ 43/52 |
| **PWM**（0x44024000）| ✅ 真实(部分) | 寄存器影子 + PERIODLOAD_FLAG=1 + START 自清 |
| **I2S**（0x44025000）| ✅ 真实(行为完整) | LEFT/RIGHT TX→RX 回环 |
| **LSADC**（0x4400C000）| ✅ **真实(行为完整)** | CTRL_8 触发转换 → rne=1/bsy=0、CTRL_9 弹出 14-bit 采样 + 完成 IRQ 72（读清）|
| **EFUSE**（0x44008000）| ✅ **真实(行为完整)** | 真实 OTP：STS boot-done + 数据窗读回，**写=按位或**（一次性熔丝只能置位，不可清零，裸机验证）；标定内容无 dump 故为空白熔丝 |
| **TSENSOR**（0x4400E000）| ✅ **真实(行为完整)** | start→sts rdy=1 + 10-bit 温度码（合成 ~25°C，按 HAL 转换公式）|
| **WDT**（0x40006000）| ✅ **真实(行为完整)** | QEMU 定时器倒计时；超时未喂狗则真复位 SoC（裸机测试验证）|
| **RTC**（0x57024000）| ✅ **真实(行为完整)** | QEMU 定时器周期触发 **IRQ 29** + INT_STATUS/EOI；CURRENT_VALUE 计数 |
| **DMA/SDMA**（0x4A000000/0x520A0000）| ✅ **真实(行为完整)** | 通道使能即**真正搬运内存**（src→dst，按宽度/地址自增），置传输完成位，按 tc_int_en 触发 **IRQ 59**；INT_CLR 清除（裸机测试验证）|
| **TRNG**（0x44114000）| ✅ 真实 | FIFO_READY=ready、FIFO_DATA 伪随机（xorshift）|
| **合成 Wi-Fi/以太 MAC**（`ws63-netmac` @ 0x44210000）| ✅ **真实(行为完整,合成)** | 软件在环连接性底座（路线图阶段 5）：**不仿 RF/PHY**，在 ws63-rf-rs netif 缝合点暴露最小以太帧 MAC——TX_BUF+TX_GO→`qemu_send_packet`（接 `-nic user` SLIRP NAT），主机帧→`.receive`→RX_BUF + **IRQ 45**（WLMAC_INT）；qtest 整帧收发回环验证。非厂商 WLMAC 寄存器级复刻 |
| **SPACC / PKE / KM**（密码学）| 🟡 影子 | 寄存器影子（**未在启动路径**：mbedtls 用 ROM 表软件 AES）。真实 AES/SHA/RSA 可经 QEMU crypto 库实现，但 SPACC v2 多通道描述符协议复杂且无固件触发，列为按需扩展 |
| **CLDO_CRG**（时钟与复位生成）| ✅ 真实(部分) | 时钟门控生效：清 CKEN_CTL0 bit21 冻结定时器、置位恢复（已实测）；CLK_SEL 源路由建模为状态；其余位影子 |
| **IO_CONFIG / SYS_CTL1 / PWM / RF_WB_CTL / SHARE_MEM / FAMA_REMAP / ULP_GPIO**（影子）| 🟡 影子 | 见下「配置类为何是影子」 |

> **覆盖度**：`WS63.svd` 的全部 **35 个外设**现均有模型（无裸 catch-all 黑洞）。
> 「行为完整」= 真实数据搬运/计时/中断/转换（DMA/RTC/WDT/Timer/I2C/SPI/I2S/LSADC/GPIO/UART/**TSENSOR/EFUSE**）。
>
> **配置类为何是影子**：配置寄存器本身没有"行为"，其行为是对*别处*的*作用*。作用可内部计算的已做成真实
> （TSENSOR 出温度、EFUSE 走 OTP、LSADC 出采样）；作用是**物理/外部**的则在仿真器里无可观测行为：
> - **引脚行为本身可仿真**：QEMU 用信号网（`qemu_irq`）建模引脚——GPIO 引脚是真实信号网
>   （bank 内回环 + 跨 bank 板级连线 + 可外部驱动）。真正"不可观测"的只是**悬空引脚**（输出无连接对象）。
> - **IO_CONFIG（引脚复用）= `ws63-pinmux` 设备（已做路由）**：`GPIO_xx_SEL[2:0]` 选功能（0=GPIO，
>   1-7=UART/SPI/I2C/PWM…）。GPIO0→GPIO1 的板级引脚网**经 pinmux 路由**：引脚复用为 GPIO 时信号导通、
>   复用为其他功能时被门控（引脚改由该外设承载）。裸机验证：复用 GPIO→读到 `1`，复用走→`0`，复用回→`1`。
>   非 GPIO 功能（UART TX/SPI CLK）的数据本身由各外设的 TX/RX/回环覆盖；pinmux 负责的是"哪根物理引脚承载它"。
> - **RF_WB_CTL / WiFi / BT**：射频 PHY，物理边界，不仿。
> - **CLDO_CRG 时钟门控（已生效）**：定时器门（CKEN_CTL0 bit21）清零会冻结定时器、置位恢复（默认开，
>   故不显式开门的固件不受影响）；CLK_SEL 源路由建模为状态。其*复位位*理论可复位目标外设，但位→外设映射
>   复杂且无固件触发，暂留影子。
> - **SHARE_MEM_CTL**：核间共享内存控制，单核下无意义。
> - **SPACC/PKE/KM**：见加密行，真实 AES/SHA/RSA 需复杂且未被触发的描述符协议，按需扩展。

## 运行厂商 C SDK 固件（多角度对齐）

除了 ws63-rs（Rust，标准 rv32imfc），本仿真器现在也能运行 **fbb_ws63 C SDK** 厂商 gcc 编译的固件
——这需要实现 HiSilicon 的 **xlinx 自定义 ISA**（[xlinx-isa.md](xlinx-isa.md)）。两侧固件对照可交叉验证
内存映射、启动、外设寄存器时序与驱动逻辑。

| C SDK 镜像 | 现状 | 边界 |
|------------|------|------|
| `flashboot` / `loaderboot`（bootloader，**零掩膜 ROM 依赖**）| ✅ 跑出 UART 输出（时钟 bring-up→flash init）| 下一阻塞 = SFC Flash 控制器（未建模，flash init 优雅失败不挂死）|
| `ws63-liteos-app`（主应用，**裁剪 BT+WiFi**）| ✅ **稳定启动到调度器并空转运行**（`cpu 0 entering scheduler` + `radar_timer_init succ`，timer IRQ 26 周期触发，无崩溃）| 加 `run.sh NV=1` 后分区表/NV 读取成功（UPG `flash_start_addr fail` 消除）；仅剩 `xo_trim` 出厂标定键（按芯片生产时烧录，任何构建 NV 都没有，固有）；BT/WiFi 任务需子系统 ROM 数据/RF 硬件，已裁剪（`config.py` 注释 `BGLE/BTH/WIFI_TASK_EXIST`）|
| `ws63-liteos-app`（含 BT/WiFi）| ✅ 启动 LiteOS、创建全部 14 子系统任务、进入调度器 | `bt`/`wifi` 任务深层初始化崩于**ROM 数据墙**（vtable/NV/efuse/RF 标定无 dump）|

构建 C SDK：`cd fbb_ws63/src && python3 build.py ws63-liteos-app -ninja`（厂商工具链已内置）。
运行：`qemu-system-riscv32 -M ws63 -nographic -serial mon:stdio -kernel <image>.elf`。

### NV 仿真（分区表 + NV 回填）
`-kernel` 启动直接把 app 装入 RAM、**跳过 flashboot**（真机里 flashboot 负责把分区表/NV 写进 flash），
所以 flash XIP 窗口（0x200000，RAM 背靠）默认是空的，C SDK 的 `uapi_partition_get_info()` / NV 读取失败
（`[UPG] ...flash_start_addr fail`、`nv read sw fail`）。`scripts/run.sh NV=1` 用 `-device loader` 把
**分区表 + NV** 回填进 flash（`tests/csdk/flash/`，见其 `manifest.txt`）：

| 镜像 | XIP 地址 | 内容 |
|------|----------|------|
| `partition_params.bin` | 0x200000 | params 区，分区表在 0x200380（magic `0x4b87a54b`）|
| `nv.bin` | 0x5FC000 | 软件 NV 键值区 |
| `nv_factory.bin` | 0x20C000 | 出厂 NV 键值区 |

三者都落在 app 自身 XIP 区（~0x230300–0x294000）**之外**，故不冲突。回填后分区表解析成功、NV 读取成功，
UPG 的 `flash_start_addr fail` 消除（`scripts/csdk-test.sh` 断言此点）。**唯一残留**：`xo_trim`
（晶振温补标定）等**逐芯片出厂标定键**——生产时烧录，任何构建产物的 NV 都不含，固有缺失、非缺陷。

## 可观察的验证目标（均已实测 PASS，见 `scripts/smoke-test.sh`）

| 固件 | 触及 | 在 QEMU 中如何观察成功 |
|------|------|------------------------|
| `blinky` | GPIO0 输出翻转 | 0 非法指令陷阱；`ws63-gpio` 经 `-d` 打印 `out=...` 翻转 |
| `uart_hello` | UART0（跳过 `init_clocks`） | `-serial mon:stdio` 打印 `Hello from WS63 on QEMU!` |
| `timer_irq` | TIMER_0 → IRQ 26 → ISR | 串口打印 `timer irq #N` 递增 + `OK: timer interrupts delivered`（mie 类中断端到端） |
| `gpio_irq` | GPIO0 pin0 → IRQ 33 → ISR | 串口打印 `gpio irq #N` + `OK: custom local IRQ (>=32) delivered`（**≥32 自定义本地中断端到端**） |

## 已知简化（未来工作）

1. **中断优先级 / 阈值**：✅ 已强制（`ws63_local_irq_pending()` 按 `LOCIPRI` 优先级 + `PRITHD` 阈值投递，
   严格 `>`、最高优先级优先、同级取小号）。**剩余细节**：被阈值屏蔽的「已挂起」IRQ 在仅写 CSR 降低阈值
   （无新边沿）时不会自动重投——需要一次新的中断源边沿。固件一般「先配优先级/阈值、后开中断源」故不触及；
   已实测该常规顺序 5/5 通过。
2. **所有 35 个 SVD 外设均已建模**（见矩阵）：DMA/RTC/WDT/I2C/SPI/I2S/LSADC/UART-RX/GPIO+pinmux 等为完整行为
   （真实数据/时间/IRQ/回环/引脚），少数为配置回读影子（晶体/RF/PHY 等本质模拟量或不可建模硬件）。
3. **时钟树（门控 + 源路由）**：定时器以 `ws63_periph_clk_hz()` 取 PCLK——PLL 锁定时 240 MHz、未锁回退 TCXO
   （24/40 MHz，依 `HW_CTL`）。**时钟门控已生效**：`CLDO_CRG_CKEN_CTL0/1`（@0x44001100/04）建模为时钟门，
   **默认开**（rs 定时器 HAL 不显式开门、依赖默认开）；固件显式清除定时器门（CKEN_CTL0 bit21）会**冻结**定时器
   （`ws63_timer_arm` 复检门控→`timer_del`），重新置位则**恢复**——已实测 3/3（开→跑、关→冻、再开→恢复）。门控
   寄存器影子默认 0xFFFFFFFF 与全局一致，故固件的读-改-写不会误清他位。**源路由**：`CLDO_CRG_CLK_SEL@0x44001134`
   的 TCXO/PLL 选择按 `ws63_periph_clk_hz(reg,bit,sel_bit)` 建模为状态；定时器无源选择（恒 PCLK），UART/SPI 的
   源/分频在 QEMU chardev 不限速下不可观测，故仅记录状态。
4. **非周期精确，但可选确定性指令计时**：TCG 不模拟流水线/cache/逐指令周期，默认虚拟时间自由运行。
   `scripts/run.sh ICOUNT=1`（= `-icount shift=2`，约 250 MHz、IPC=1）开启**确定性指令计时**：虚拟时间绑定指令数，
   **同一固件每次运行结果完全一致**（实测 1e6 循环 3 次均 = 2,880,003 ticks；不开 icount 则随宿主时钟漂移）。
   这是 IPC=1 近似，**不是**真实微架构周期精确（真周期级需 gem5 等，非本仿真器目标）。`ICOUNT_SHIFT` 可调频率。
5. 默认 **v10.0.0**(已从 v9.2.4 升级),**两版本各一套 `patches/<tag>/` 序列并行维护**。升级处理的 API 变化:`sysemu/`→`system/`
   头文件改名、`Property[]` 去 `DEFINE_PROP_END_OF_LIST` 终止符并改 `const`(经 `device_class_set_props_n`/`ARRAY_SIZE`)。
   v10.2.3 也已移植(其 target/riscv 漂移更大:`insn_len`→`internals.h`、声明式 `DEFINE_RISCV_CPU`、表驱动 `decode_opc`、
   `CharBackend`→`CharFrontend`、`exec/`→`system/address-spaces.h`)。再往后由 `qtest-matrix.yml` 的 **v11.0.1** 实验性 cell
   作漂移雷达;届时按 [`patches/README.md`](../patches/README.md) 的流程补 `patches/v11.0.x/`。
