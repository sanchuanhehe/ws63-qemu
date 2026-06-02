# ws63-qemu 用户手册

本手册面向 **使用** ws63-qemu 的人,覆盖三部分:**安装**、**使用与注意事项**、**当前验证覆盖范围**。
适用版本:**v0.3.0**(`qemu-system-riscv32 -M ws63`,QEMU v9.2.4 fork + HiSilicon xlinx 自定义 ISA)。

> 它是什么:在无 WS63 硬件(EVB)的情况下,运行**未经修改的、厂商/工具链编译的 WS63 固件**——既能跑
> [`ws63-rs`](https://github.com/sanchuanhehe/ws63-rs)(Rust 裸机),也能跑 fbb_ws63 **C SDK**(flashboot +
> LiteOS app)。它是 **软件在环(SIL)** 的驱动验证底座,**不是**周期精确的微架构模拟器。
>
> 想了解内部设计而非如何使用,请读 [`design.md`](design.md);本手册只讲怎么装、怎么用、验了什么。

---

## 目录

- [一、安装](#一安装)
  - [1.1 系统要求](#11-系统要求)
  - [1.2 方式 A:下载预编译 Release(最快)](#12-方式-a下载预编译-release最快)
  - [1.3 方式 B:从源码构建(可移植)](#13-方式-b从源码构建可移植)
  - [1.4 准备固件](#14-准备固件)
- [二、使用](#二使用)
  - [2.1 基本运行](#21-基本运行)
  - [2.2 `run.sh` 选项](#22-runsh-选项)
  - [2.3 GDB 调试](#23-gdb-调试)
  - [2.4 追踪与日志](#24-追踪与日志)
- [三、使用注意事项(重要)](#三使用注意事项重要)
- [四、当前验证覆盖范围](#四当前验证覆盖范围)
  - [4.1 持续验证(CI)](#41-持续验证ci)
  - [4.2 ws63-rs(Rust)冒烟](#42-ws63-rsrust冒烟)
  - [4.3 C SDK 外设样例](#43-c-sdk-外设样例)
  - [4.4 外设建模覆盖](#44-外设建模覆盖)
  - [4.5 明确不在覆盖范围内](#45-明确不在覆盖范围内)
- [五、故障排查(FAQ)](#五故障排查faq)
- [附:文档导航](#附文档导航)

---

## 一、安装

### 1.1 系统要求

- **OS**:Linux(主要在 Ubuntu/Debian x86_64 上验证);其他平台请走「从源码构建」。
- **磁盘**:QEMU 源码树 + 构建产物约 **2 GB**。
- **首次构建耗时**:约 **10–20 分钟**(取决于核数;`build.sh` 只构建 `riscv32-softmmu` 单目标)。
- **运行时**:仅需 `qemu-system-riscv32` 二进制(动态依赖 glib/pixman)。

有两种安装方式,任选其一。

### 1.2 方式 A:下载预编译 Release(最快)

从 [Releases](https://github.com/sanchuanhehe/ws63-qemu/releases) 下载对应版本资产:

| 资产 | 说明 |
|------|------|
| `qemu-system-riscv32-ws63-<ver>` | Ubuntu 构建的仿真器二进制(动态链接 glibc/glib/pixman)|
| `ws63-qemu-src-<ver>.tar.gz` | 源码包(`src/` `patches/` `scripts/` `tests/` `docs/` 等)——其他平台用它重建 |
| `SHA256SUMS` | 校验和 |

```bash
# 校验后赋可执行权限
sha256sum -c SHA256SUMS
chmod +x qemu-system-riscv32-ws63-<ver>
./qemu-system-riscv32-ws63-<ver> -M help | grep ws63   # 确认机器已注册
```

> 预编译二进制是在 **Ubuntu** 上链接的。若你的发行版 glibc/glib/pixman 版本不兼容(`error while loading
> shared libraries`),请改用方式 B 从源码构建。

### 1.3 方式 B:从源码构建(可移植)

```bash
# 1. 安装构建依赖(Debian/Ubuntu;需 sudo)
bash scripts/setup-deps.sh
#   等价手动:apt-get install git build-essential pkg-config ninja-build meson \
#             libglib2.0-dev libpixman-1-dev flex bison python3 python3-venv zlib1g-dev

# 2. 克隆固定版 QEMU、注入 WS63 板卡 + xlinx ISA、构建
bash scripts/build.sh
#   产物:./qemu/build/qemu-system-riscv32
```

`build.sh` 做的事(**幂等**,可反复运行做增量构建):

1. 浅克隆 QEMU `v9.2.4`(`QEMU_TAG` 可改)到 `./qemu/`;
2. 注入板卡源 `src/hw/riscv/ws63.c` 和 xlinx 解码器 `trans_xlinx.c.inc`;
3. 应用 `patches/ws63-target-riscv.patch`(自定义本地中断 IRQ≥32 投递 + xlinx ISA 钩子);
4. 在 meson/Kconfig 注册 `CONFIG_WS63`;
5. `./configure --target-list=riscv32-softmmu` 后 `make`。

构建相关环境变量:`QEMU_TAG`(默认 `v9.2.4`)、`QEMU_DIR`(默认 `<repo>/qemu`)、`QEMU_REPO`、`JOBS`(默认 `nproc`)。

**验证安装**:

```bash
./qemu/build/qemu-system-riscv32 -M help | grep ws63   # 应输出含 "ws63" 的一行
```

### 1.4 准备固件

仿真器本身不含固件,你需要一个 **ELF** 来跑。两条路径:

**(a) ws63-rs(Rust 裸机)** —— 需要 `ws63` 自定义 Rust 工具链(rv32imfc 硬浮点、无原子,内置为 builtin target):

```bash
curl -fLO https://github.com/sanchuanhehe/ws63-rust-toolchain/releases/download/v1.96.0/ws63-rust-1.96.0-x86_64-unknown-linux-gnu.tar.gz
tar xzf ws63-rust-1.96.0-*.tar.gz && rustup toolchain link ws63 "$PWD/stage2"
# 在 ws63-rs 仓库中:
cargo build -p blinky --release
#   产物:target/riscv32imfc-unknown-none-elf/release/blinky
```

**(b) fbb_ws63 C SDK(厂商 gcc)** —— 用 SDK 内置工具链:

```bash
cd fbb_ws63/src && python3 build.py ws63-liteos-app -ninja
```

> 跑 C SDK app **不需要**自己装工具链来用仓库自带的测试 fixture——`tests/csdk/` 里已有预编译样例
> ELF,见[§4.3](#43-c-sdk-外设样例)。

---

## 二、使用

### 2.1 基本运行

```bash
# 用包装脚本(推荐):不带参数默认跑 ws63-rs 的 blinky
bash scripts/run.sh
bash scripts/run.sh path/to/firmware.elf

# 直接调用 QEMU
./qemu/build/qemu-system-riscv32 -M ws63 -nographic -serial mon:stdio -kernel firmware.elf
```

**退出 QEMU**:`Ctrl-A` 然后 `X`。

串口约定:`-serial mon:stdio` 把 **UART0** 的 TX 输出到你的终端,终端输入送入 UART0 的 RX(中断使能时触发
IRQ 53)。`mon:` 表示同一通道复用 QEMU monitor(`Ctrl-A C` 切换)。

### 2.2 `run.sh` 选项

通过环境变量开启(`run.sh <elf> [额外 qemu 参数...]`,额外参数原样透传给 QEMU):

| 变量 | 作用 |
|------|------|
| `DEBUG=1` | 加 `-d int,guest_errors,unimp -D qemu.log`,把中断/guest 错误/未建模访问写入 `qemu.log` |
| `ICOUNT=1` | **确定性指令计时**(`-icount shift=2`,约 250 MHz、IPC=1):同一固件每次运行计时**完全一致**。**非**周期精确 |
| `ICOUNT_SHIFT=N` | 改 icount shift(默认 2→4 ns/insn≈250 MHz;3→125 MHz)|
| `NV=1` | 用分区表 + NV 镜像(`tests/csdk/flash/`)回填 flash XIP 窗口,使 C SDK 的分区/NV 读取成功(见[§三](#三使用注意事项重要))|
| `SEMIHOST=1` | 加 `-semihosting`:固件可用 RISC-V semihosting `SYS_EXIT` 设置 QEMU **进程退出码**(CI 免解析 UART 即得 pass/fail)、`SYS_WRITE0` 打印到控制台。见 ws63-rs `ws63-examples/semihost_selftest` |

路径相关变量:`QEMU_DIR` / `QEMU_BIN`(仿真器位置)、`WS63_RS`(取默认 blinky ELF 用的 ws63-rs 路径,默认 `../ws63-rs`)、
`FLASH_DIR`(NV overlay 目录)。

示例:

```bash
ICOUNT=1 bash scripts/run.sh fw.elf          # 可复现计时
NV=1 bash scripts/run.sh ws63-liteos-app.elf # C SDK app + 分区表/NV
DEBUG=1 bash scripts/run.sh fw.elf           # 写 qemu.log 追踪
```

### 2.3 GDB 调试

QEMU 内置 gdbstub 对本机器开箱即用。透传 `-s -S`(= 在 `:1234` 起 gdbstub 并冻结在复位):

```bash
bash scripts/run.sh fw.elf -s -S
# 另一终端:
riscv32-unknown-elf-gdb fw.elf   # 或 gdb-multiarch
(gdb) target remote :1234
(gdb) break main
(gdb) continue
```

> 注意:GDB 的反汇编器**不认识 xlinx 自定义指令**,落在 ROM/xlinx 区的指令会显示为未知。源码级
> 断点/单步/查看变量在标准指令上正常工作。

### 2.4 追踪与日志

- `DEBUG=1`(见上)或手动 `-d int,unimp,guest_errors -D qemu.log`。
- `-d unimp`:打印对**未建模/兜底**外设的访问(按地址定位)。
- `-d int`:打印每次中断投递(确认 IRQ 号/向量)。
- `-d trace:ws63_gpio_*`:WS63 模型的正规 trace 事件——`ws63_gpio_set` / `ws63_gpio_clr`
  (GPIO 输出变化)、`ws63_dma_xfer`(DMA 搬运)。也可 `-trace ws63_dma_xfer` 等单独开启。

### 2.5 寄存器级 qtest(免启动回归)

`scripts/qtest.sh` 用 libqtest **直接驱动外设寄存器**(测试进程扮演 CPU 角色,**不启动固件**),
毫秒级验证 GPIO/UART/timer/INTC/DMA 模型的寄存器语义。与 `smoke-test.sh`(整机启动真实固件)互补。

```bash
bash scripts/qtest.sh          # 构建 tests/qtest/ws63-test 并运行(4 例,TAP 输出)
```

覆盖:GPIO 数据 set/clr/OEN/INT-EN 读写;UART FIFO/行状态复位值;timer 装载/使能/触发 +
经 INTC 投递 IRQ 26(`qtest_irq_intercept_in`);DMA 通道 0 mem→mem 搬运 + 完成位。

---

## 三、使用注意事项(重要)

这些是用仿真器时**必须知道的语义边界**,不理解会误判结果:

1. **默认非周期精确,虚拟时间自由运行。** TCG 不模拟流水线/cache/逐指令周期。需要可复现计时就用
   `ICOUNT=1`,但那是 **IPC=1 近似**(≈250 MHz),**不是**真实微架构周期。真周期级请用 gem5 等,非本仿真器目标。

2. **掩膜 ROM 是桩的。** WS63 应用核有一块片上掩膜 ROM(`0x109000..0x14C000`,厂商不发布二进制)。仿真器在
   `ws63_rom_call` 里**仿真**用到的 ROM 函数(`mem*_s`/`*printf_s`/计时/看门狗 API/vtable getter 等),
   **未识别的 ROM 地址返回 0(成功)**让固件不崩。代价:依赖 **ROM 数据**(非代码)的功能无法重建——见下第 5 点。
   细节见 [`rom-stubs.md`](rom-stubs.md)。

3. **`-kernel` 启动跳过 bootloader。** 直接把 app 装入 RAM,**不经 flashboot**,所以 flash XIP 窗口默认是空的,
   C SDK 的分区表 / NV 读取会失败(`[UPG] ...flash_start_addr fail`、`nv read sw fail`)。**解法:`NV=1`** 回填
   分区表 + NV。但**逐芯片出厂标定键**(如 `xo_trim` 晶振温补)在生产时烧录,任何构建产物的 NV 都没有,
   故 `xo_trim ... nv read sw fail` 一行**固有残留,非缺陷**。

4. **配置类外设是「影子」。** 寄存器可读回但无副作用——典型是 **RF / PHY / 晶体**等本质模拟量或物理硬件
   (`RF_WB_CTL`、`SHARE_MEM`、`SPACC/PKE/KM` 等)。行为可内部计算的外设是真实的(见[§4.4](#44-外设建模覆盖))。

5. **不仿真的东西(ROM 数据墙 / 物理边界):**
   - **Wi-Fi / BT / SLE 的射频(PHY/RF)** —— 物理边界,不仿;C SDK 含 BT/WiFi 的 app 会在子系统深层初始化崩于
     ROM 数据墙(vtable/NV/efuse/RF 标定无 dump),需裁剪这些任务(`config.py` 注释 `BGLE/BTH/WIFI_TASK_EXIST`)。
   - **真实密码学**(AES/SHA/RSA 的 SPACC v2 描述符协议)——未在启动路径,按需扩展。
   - **看门狗中断模式回调**("kick timeout!"):WDT 超时→复位**已建模**,但超时→中断回调→再复位**未建模**
     (需与 vCPU 同步的 PC 注入)。

6. **目标核 = 单核 RV32IMFC。** 单浮点(`F`)、压缩(`C`)、**无原子(`A`关)**、**无 `D`**;WS63 是**单核**(无第二 hart)。

7. **QEMU 固定 v9.2.4。** 升级到 v10.x 需注意上游 API 变化(`class_init` 签名、`sysemu/`→`system/` 头改名等)。

---

## 四、当前验证覆盖范围

「覆盖」分三层:**持续 CI 验证**(每次提交都跑)、**外设建模覆盖**(35/35 建模)、**明确的边界**(已知不覆盖)。

### 4.1 持续验证(CI)

`ci.yml` 在**每次 push / PR**上跑(Ubuntu),门禁全绿才算通过:

1. 构建 `qemu-system-riscv32`(带 WS63 机器);
2. **机器注册** sanity(`-M help` 含 ws63);
3. **ws63-rs 冒烟**(`smoke-test.sh`,见§4.2);
4. **C SDK 外设样例**(`csdk-test.sh`,见§4.3,**5/5**);
5. **寄存器级 qtest**(`qtest.sh`,免启动驱动 GPIO/UART/timer/INTC/DMA,**4/4**,见§2.5)。

`release.yml`(打 `v*` tag 时)额外做全新构建 + 冒烟,然后发布二进制。v0.3.0 的三个 workflow(ci×2 + release)均 ✅。

### 4.2 ws63-rs(Rust)冒烟

`scripts/smoke-test.sh`(真值见脚本);每条都是端到端、断言串口/MMIO 标志:

| 固件 | 验证什么 | 成功判据 |
|------|----------|----------|
| `blinky` | GPIO0 输出翻转 + xlinx/启动正确 | 0 非法指令陷阱 + 观察到 GPIO 翻转(pin0 拉高)|
| `uart_hello` | 自定义 UART0 TX | 串口打印 `Hello from WS63 on QEMU!` |
| `timer_irq` | TIMER_0→**IRQ 26**→ISR(mie 类中断)| 串口 `timer irq #N` 递增 + `OK: timer interrupts delivered` |
| `gpio_irq` | GPIO0→**IRQ 33**→ISR(**≥32 自定义本地中断**)| 串口 `gpio irq #N` + `OK: custom local IRQ (>=32) delivered` |
| `reset_demo` | `software_reset()` + `reset_reason()` 往返 | 重启 ≥2 次 + `OK: software reset observed`(reset_reason=Software)|
| `dma_loopback` | mem↔SPI0 外设 DMA + SDMA 通道 | `DMA LOOPBACK TEST: PASS` |
| `wifi_blob_link` | 链接 `libwifi_rom_data.a` + 重定位 | `BLOB LINK SPIKE: PASS` |
| `rf_port_demo` | ws63-rf-rs porting 层 + blob 经其链接 | `RF PORT DEMO: PASS` |
| `sched_selftest` | ws63-rf-rs 协作调度器(上下文切换 + 信号量)| `SCHED SELFTEST: PASS` |
| `semihost_selftest` | semihosting 退出码(M/F/Zicsr 自检)| **QEMU 退出码 0**(免解析 UART;见§2.2 `SEMIHOST`)|

### 4.3 C SDK 外设样例

`scripts/csdk-test.sh` 启动 `tests/csdk/` 里**预编译的 fbb_ws63 C SDK 厂商固件**并断言各自的 UART 成功标志——
用**真实厂商固件**交叉验证外设模型。纯净 + 免 SDK/工具链(用已提交的 fixture)。**当前 5/5 绿**:

| 样例 | 验证的外设路径 | 成功标志 |
|------|----------------|----------|
| `tcxo.elf` | TCXO ms/us 计数器 | `tcxo get ms work normall` |
| `systick.elf` | SysTick 计数器 | `systick get ms work normall` |
| `adc.elf` | LSADC 标定 + RX-FIFO 转换读 | `voltage: N mv` |
| `dma.elf` | DMA v151 内存搬运 + 完成 | `dma memory copy test succ` |
| (NV overlay) | 分区表解析 + NV 读 | 启动到调度器且无 `upg ...flash_start_addr fail` |

**已记录但未断言的样例**(`tests/csdk/manifest.txt`):

- **`timer`** —— **非外设模型问题**。硬件定时器模型正确(channel 0 / IRQ 26 即 LiteOS systick 正常触发),
  但 LiteOS **软件定时器任务层**没走到 `uapi_timer_start`,属上层任务问题。
- **`watchdog`** —— 看门狗 API **功能可用**(喂狗→重装、超时→复位;健康样例干净跑到调度器),但样例唯一的成功
  标志是**中断模式**的 `"watchdog kick timeout!"` 回调,而该回调**未建模**(见[§三](#三使用注意事项重要)第 5 点),
  故无 UART marker 可断言。

> 重新生成 fixture:`scripts/build-csdk-samples.sh`(从 fbb_ws63 checkout 选一个 `CONFIG_SAMPLE_SUPPORT_*`、
> 干净构建、strip 到 ~400 KB)。

### 4.4 外设建模覆盖

`WS63.svd` 的**全部 35 个外设均已建模**(无裸 catch-all 黑洞)。分两档,完整矩阵见 [`design.md` §外设建模矩阵](design.md):

- **行为完整(真实数据/计时/中断/回环)**:CPU+内存、xlinx ISA、UART0/1/2(TX+RX)、TIMER×3、GPIO+pinmux
  (真实引脚信号网)、中断控制器(26–31 + ≥32,含 LOCIPRI/PRITHD)、DMA/SDMA、RTC、WDT、I2C0/1、SPI0/1、I2S、
  LSADC、TSENSOR、EFUSE(OTP 按位或)、TRNG、TCXO、SFC、CLDO_CRG 时钟门控。
- **配置影子(可读回、无副作用)**:RF_WB_CTL、SHARE_MEM、SPACC/PKE/KM、部分 SYS_CTL1/IO_CONFIG 位等——
  本质模拟量 / 物理硬件 / 未被固件触发,见 design.md 的说明。

### 4.5 明确不在覆盖范围内

| 项 | 状态 | 原因 |
|----|------|------|
| Wi-Fi / BT / SLE 射频(PHY/RF)| ❌ 不仿 | 物理边界 |
| C SDK 含 BT/WiFi app 的深层初始化 | ❌ 崩于 ROM 数据墙 | vtable/NV/efuse/RF 标定无 dump,需裁剪任务 |
| 逐芯片出厂标定 NV(`xo_trim` 等)| ❌ 固有缺失 | 生产时烧录,任何构建产物都不含 |
| 真实 AES/SHA/RSA(SPACC v2)| ❌ 影子 | 描述符协议复杂且未被触发 |
| 看门狗中断模式超时回调 | ❌ 未建模 | 需 vCPU 同步的 PC 注入 |
| 周期精确时序 | ❌ 近似 | TCG 非微架构;`ICOUNT` 仅 IPC=1 近似 |
| 双核 / 第二 hart | ❌ 不适用 | WS63 单核(已核实)|
| snapshot / migration | ❌ 未投入 | 按需 |

---

## 五、故障排查(FAQ)

| 现象 | 原因 / 解法 |
|------|-------------|
| `QEMU not built: .../qemu-system-riscv32` | 先 `bash scripts/build.sh` |
| `firmware ELF not found` | `run.sh` 第一个参数给对 ELF 路径,或先构建固件([§1.4](#14-准备固件))|
| `-M help` 不含 ws63 | 构建未注入成功;删 `qemu/` 重跑 `build.sh`(它会重新克隆+注入)|
| 预编译二进制 `error while loading shared libraries` | glibc/glib/pixman 版本不匹配;改用[源码构建](#13-方式-b从源码构建可移植)|
| 固件一上来就 **illegal instruction** 大量陷阱 | 多为 xlinx ISA 或 ROM 调用问题;`DEBUG=1` 看 `qemu.log` 的 pc 落点,ROM 区(0x109xxx)是预期被拦截的 |
| C SDK app 反复打印 `[UPG] ...flash_start_addr fail` / `nv read sw fail` | 用 **`NV=1`** 回填分区表/NV;残留的 `xo_trim` 一行是固有的 |
| C SDK BT/WiFi 任务崩溃 | 预期(ROM 数据墙);裁剪 `BGLE/BTH/WIFI_TASK_EXIST` 任务 |
| 计时每次运行都不同 | 默认虚拟时间自由运行;要可复现用 **`ICOUNT=1`** |
| 看门狗样例"没看到 timeout 打印" | 中断模式回调未建模(见[§三](#三使用注意事项重要)第 5 点);健康喂狗→不复位是正确行为 |

---

## 附:文档导航

- [`README.md`](../README.md) —— 项目概览 + 快速开始
- [`design.md`](design.md) —— 设计、UART 寄存器、**完整外设矩阵**、已知简化
- [`memory-map.md`](memory-map.md) —— 内存映射 + 外设基址(真值来源)
- [`rom-stubs.md`](rom-stubs.md) —— 掩膜 ROM 桩与拦截的完整目录
- [`xlinx-isa.md`](xlinx-isa.md) —— HiSilicon xlinx 自定义指令
- [`tests/csdk/manifest.txt`](../tests/csdk/manifest.txt) —— C SDK 样例断言 + gap 诊断
- [`CHANGELOG.md`](../CHANGELOG.md) —— 版本变更 · [`ROADMAP.md`](../ROADMAP.md) —— 规划

真值来源:内存布局 = `ws63-rs/ws63-rt/{memory.x,layout.ld}`;外设基址/寄存器 = `ws63-rs/ws63-svd/WS63.svd`;
寄存器行为 = fbb_ws63 C SDK `hal_*_regs_def.h`。
