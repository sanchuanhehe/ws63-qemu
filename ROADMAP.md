# ws63-qemu 路线图（ROADMAP）

> 本路线图记录 `ws63-qemu` 的目标、已交付内容与剩余工作。
> **现状(v0.3.0)**:已是一个**单核 SoC 行为保真、能在无硬件下验证 [ws63-rs](https://github.com/sanchuanhehe/ws63-rs)
> 驱动并运行 fbb_ws63 C SDK 厂商固件**的仿真器——最初规划的「从 MVP 成长为单核 SoC 保真器」目标(阶段 0–4)
> **已基本达成**。剩余的是测试基建打磨、可维护性/上游,以及可选的连接性尾部。
> 设计与现状见 [`docs/design.md`](docs/design.md),使用见 [`docs/user-manual.md`](docs/user-manual.md),
> 内存映射见 [`docs/memory-map.md`](docs/memory-map.md),变更见 [`CHANGELOG.md`](CHANGELOG.md)。

## 北极星

**在没有 EVB 的情况下,把 WS63 固件「跑得足够真」以验证驱动正确性**——内存布局 / 启动 / 链接脚本、
外设寄存器与时序、中断驱动逻辑,乃至(远期、单核、不仿 RF)连接性 IPC 边界。

QEMU 是手段,是 ws63-rs ROADMAP **阶段 1「硬件在环 bring-up」的软件在环替代**,不是终点。它现在已能在真机到位前
验证大量正确性(全部 35 外设行为模型 + 两侧固件交叉验证),但**仍不等于真机**:不建模无线电(RF/PHY)、
时序非周期精确(`-icount` 仅 IPC=1 近似)、依赖掩膜 ROM **数据**的功能受限(见[非目标](#冻结--非目标))。

## 关于 WS63 核数(更正记录)

**WS63 是单核**——经 fbb_ws63 C SDK 核实:`ch2_system.md` 明确「系统提供**一个**自研 RISC-V 处理器作为主控 CPU」;
`platform_core.h` 标题为 *Application Core*;`rom_config/` 只有 `acore`;全 SDK 无 `dcore`。Wi-Fi/BT 是
**链接进同一个应用镜像的库**(`libwifi_driver_hmac.a` / `libwifi_driver_dmac.a` / `libwpa_supplicant.a`)——
HMAC/DMAC 是 Wi-Fi **软件分层**(host-MAC / device-MAC),跑在同一颗核上,**不是两颗物理核**。

→ 本路线图**不含双核 / 第二 hart**。连接性若推进,是把 Wi-Fi MAC 建模为**外设** + 主机网络后端,而非第二个 RISC-V 核。

---

## 阶段总览

| 阶段 | 主题 | 状态 |
|------|------|------|
| 0 | MVP + 中断控制器(IRQ 26–31) + Timer/GPIO/SYS_CTL0/UART + 冒烟 + CI | ✅ 已完成 |
| 1 | 开发与测试基建(GDB / 真实固件回归 / qtest / semihosting / trace / 命名 CPU) | 🟡 **部分完成**(GDB ✅ + 真实固件回归 ✅;qtest/semihosting/trace/命名 CPU 待做)|
| 2 | **中断控制器保真**(IRQ≥32 投递 + LOCIPRI/PRITHD,`target/riscv` 补丁)| ✅ **已完成**(gpio_irq 端到端验证)|
| 3 | 外设深度(GPIO 输入/中断、I2C/SPI、reset、WDT、RTC、DMA…)| ✅ **已完成**(全 35 外设,详见 design.md 矩阵)|
| 4 | 时钟树 + 真实度(门控/源路由;eFuse/LSADC)| ✅ **已完成**(门控+源路由+eFuse/LSADC;完整 PLL→波特时序本质不可观测)|
| 5 | 连接性底座(**可选/研究级**:Wi-Fi MAC 外设 + SLIRP,单核,不仿 RF)| ⏸ 暂不投入 |
| 6 | 可维护性 / 上游(跟版 rebase、代码隔离、qtest CI、可能上游)| 🔄 持续 |

---

## 已交付(v0.1.0 → v0.3.0)

最初规划的核心目标已落地。按版本:

- **v0.1.0** — WS63 机器(`-M ws63`,单核 RV32IMFC + 全内存映射)、**HiSilicon xlinx 自定义 ISA**(厂商 gcc 固件必需)、
  **IRQ≥32 自定义本地中断投递**(阶段 2)、**掩膜 ROM 调用拦截**(`ws63_rom_call`)→ `ws63-liteos-app` 启动到调度器、
  **全部 35 个 SVD 外设建模**(阶段 3,DMA/RTC/WDT/I2C/SPI/I2S/LSADC/UART-RX/SFC/TSENSOR/EFUSE/TRNG +
  GPIO 引脚网/pinmux)、tag 触发的 release 工作流。
- **v0.2.0** — **LOCIPRI/PRITHD 中断优先级/阈值强制**、**时钟树门控 + 源路由**(阶段 4)、`-icount` 确定性指令计时、
  Node-24 代 Actions。
- **v0.3.0** — **C SDK 外设样例测试台**(`csdk-test.sh` + `tests/csdk/`,5/5 进 CI)、**NV / 分区表 flash overlay**、
  **看门狗 ROM API 仿真**(喂狗→重装、超时→复位)、LSADC/DMA/本地中断投递的模型修复、`docs/rom-stubs.md` +
  `docs/user-manual.md`。

→ 结论:**阶段 0、2、3、4 已完成**;阶段 1 部分完成(见下)。

---

## 里程碑(已达成):运行厂商 C SDK 固件(xlinx 自定义 ISA)

> 原为「扩展计划」,**现已达成**。仿真器既跑 ws63-rs(Rust,标准 rv32imfc),也跑 fbb_ws63 C SDK
> 厂商 gcc 编译的固件,实现「两侧 SDK 多角度交叉对齐」。

- **HiSilicon riscv31 `xlinx` 自定义指令集已实现**(`l.li`/`*shf`/`b*i`/`muliadd`/`jal16`/`j16`/
  `ldmia`/`stmia`/`push`/`pop`/`popret`/`uxtb`/`uxth`/ 压缩 `lbu`/`lhu`/`sb`/`sh`),13/13 自检通过。
  详见 [`docs/xlinx-isa.md`](docs/xlinx-isa.md)。
- **`flashboot` 跑出 UART 输出**(时钟 bring-up → flash init)。
- **`ws63-liteos-app` 稳定启动 LiteOS 到 `cpu 0 entering scheduler` 并空转运行**(timer IRQ 26 周期触发,无崩溃)。
- **原先的两条「硬边界」均已解除**:
  1. **掩膜 ROM** → 不再是阻塞:`ws63_rom_call` 在宿主 C 里**仿真**用到的 ROM 函数,未识别地址优雅降级返回 0。
     详见 [`docs/rom-stubs.md`](docs/rom-stubs.md)。
  2. **SFC Flash 控制器** → 已建模(RDID/RDSR/命令完成);配 `run.sh NV=1` 回填分区表 + NV,分区/NV 读取成功。
- **仅剩 ROM 数据墙**(非 ISA / 非建模能解):BT/WiFi 子系统、逐芯片出厂标定(`xo_trim`)、真实密码学——见[非目标](#冻结--非目标)。

---

## 阶段 1 — 开发与测试基建(部分完成;剩余为近期、低成本高杠杆)

**已做**:
- **GDB** ✅ — QEMU 内置 gdbstub 对本机器开箱即用;透传 `-s -S` 调试,文档见 [`docs/user-manual.md` §2.3](docs/user-manual.md)。
- **真实固件回归** ✅ — CI 每次 push/PR 跑 `smoke-test.sh`(ws63-rs:blinky/uart_hello/timer_irq/gpio_irq)
  + `csdk-test.sh`(C SDK 样例 tcxo/systick/adc/dma + NV,5/5)。这比原计划只验 ws63-rs 更强:**用真实厂商固件**
  交叉验证外设模型。

**待做**:
- **qtest**:`tests/qtest/ws63-test.c`(libqtest),对 timer/gpio/uart/intc/dma 做**寄存器级、免启动**回归,接入 CI。
  补足当前「整机启动断言」之外的细粒度、快速回归层。
- **semihosting**:`-semihosting` + ws63-rs 一个极小 `exit()/print` 助手,让 CI 不靠解析 UART 即得 pass/fail 退出码。
- **trace-events**:把 GPIO 现在的临时 `qemu_log` 换成 `hw/riscv/trace-events` 正规 trace 事件(`-trace` 可选择性开启)。
- **命名 CPU**(可与上游 rebase 合并):`target/riscv` 加 `ws63` CPU 类型(`-cpu ws63` = rv32imfc),免去 machine init 逐位设扩展。

**验收**:qtest 在 CI 跑过;两套固件冒烟仍绿。

---

## 阶段 2 — 中断控制器保真 ✅ 已完成

IRQ≥32(GPIO/UART/DMA…)已真正投递。`target/riscv` 补丁(`patches/ws63-target-riscv.patch`)实现:

- `LOCIEN0-2`(CSR 0xBE0)、`LOCIPD`、`LOCIPRI0-15`(0xBC0)、`PRITHD`(0xBFE)作为**带 per-CPU 状态的真实 CSR**。
- **本地中断投递**:设备 IRQ≥32 pending 且 `LOCIEN` 使能、优先级 **严格 > `PRITHD`** 时,CPU 以 `mcause = irq(32–72)`
  取中断 + 向量化 mtvec → ws63-rt 的 `local_interrupt_handler`(自定义 pending 通路,绕开 RV32 32 位 mip/mie)。
  同级取小号;投递时**自动清 LOCIPD**(边沿/一次性,匹配 C SDK 的 `default_local_interrupt_handler` 无 LOCIPCLR 通路)。
- **已验证**:`gpio_irq`(IRQ 33)端到端、LOCIPRI/PRITHD 探针 5/5。
- **剩余细节**(已知简化):被阈值屏蔽的「已挂起」IRQ 在仅写 CSR 降阈值(无新边沿)时不自动重投——需新边沿;
  固件常规「先配优先级、后开源」顺序不触及。

---

## 阶段 3 — 外设深度 ✅ 已完成

`WS63.svd` 全部 **35 个外设均已建模**(无裸 catch-all 黑洞)。**行为完整**(真实数据搬运/计时/中断/回环/引脚):
DMA/SDMA(真实内存搬运 + 外设 DMA:解析 `fc_tt`/`src_per`/`dest_per` 流控与握手字段,MMIO-aware 拷贝把 mem↔外设 DR
搬运路由到设备 handler——经 mem↔SPI0 环回端到端验证;TC 中断门控 `tc_int_en(ctrl.31)&&tc_int_mask(cfg.13)` 对齐 PL080;IRQ 59)、
RTC(IRQ 29)、WDT(倒计时→真复位)、Timer×3、I2C0/1(回环 FIFO)、SPI0/1(回环 FIFO)、
I2S、LSADC(转换 + IRQ)、UART-RX、GPIO(真实引脚信号网 + 边沿/电平中断)、TSENSOR、EFUSE(OTP 按位或)、TRNG、TCXO、SFC。
system reset、IO_CONFIG pinmux 路由亦已建模。完整矩阵见 [`docs/design.md` §外设建模矩阵](docs/design.md)。
**配置影子档**(可读回、无副作用):RF/SHARE_MEM/SPACC-PKE-KM 等本质模拟量或物理硬件。

---

## 阶段 4 — 时钟树 + 真实度 ✅ 已完成

- **时钟门控已生效**:`CLDO_CRG_CKEN_CTL0/1`(@0x44001100/04)建模为时钟门,**默认开**;清定时器门(CKEN_CTL0 bit21)
  **冻结**定时器、置位**恢复**(实测 3/3)。
- **源路由**:`CLDO_CRG_CLK_SEL@0x44001134` TCXO/PLL 选择建模为状态;定时器以 `ws63_periph_clk_hz()` 取 PCLK
  (PLL 锁定 240 MHz / 未锁回退 TCXO 24/40 MHz)。
- **eFuse / LSADC** 已最小建模并**行为完整**——把原先静态对照 SDK 的修复变成可执行验证(LSADC `ctrl_8/9` 转换、EFUSE 读窗口)。
- **本质受限**(非缺陷):UART 波特、SPI 分频等时序在 QEMU chardev 不限速下**不可观测**,故源/分频仅记录状态。

---

## 阶段 5 — 连接性底座(可选 / 研究级,单核,不仿 RF)⏸ 暂不投入

- **不是双核**(WS63 单核已核实)。若推进:把 **Wi-Fi MAC 建模为外设**,主机侧用 **SLIRP / 假以太**在 MAC/驱动边界
  收发帧(仿 esp-qemu——**不仿无线电 / PHY**);Wi-Fi 驱动库跑在单核上,QEMU 给 MAC 寄存器接口 + 注入/取出帧。
- 含 ws63-rs 阶段 3 的 blob 尖刺:让 `libwifi_rom_data.a` 在正确内存布局下链接、外部符号解析(QEMU 内存布局已就绪)。
- 研究级、低置信、依赖 ws63-rs 走到连接性。**明确标注为远期可选尾部**。

---

## 阶段 6 — 可维护性 / 上游(持续)🔄

- 跟 QEMU release(v9.2 → v10.x LTS)rebase;自定义代码已隔离到 `hw/riscv/ws63.c` + `target/riscv` 补丁 +
  `insn_trans/trans_xlinx.c.inc`,便于跟版;考虑用正式 patch-series 取代「拷文件 + sed 注入」。
- **CI 已就位**(build + 双固件冒烟 + C SDK 样例 + QEMU 构建缓存);后续加 qtest 矩阵。
- 可能向上游提交基础机器(`hw/riscv/ws63.c`)+ qtests(HiSilicon WS63 board)。

---

## 冻结 / 非目标

均**非建模能解**或非本仿真器目标:

- **RF / PHY 无线电仿真**——不做;连接性若推进只到 MAC / SLIRP 边界(同 esp-qemu)。
- **双核 / 第二 hart**——WS63 单核(已核实),不做。
- **周期精确时序**——TCG 非微架构;`-icount` 仅 IPC=1 近似。真周期级请用 gem5 等。
- **ROM 数据墙**(依赖掩膜 ROM 中**数据**而非代码,无 dump 不可重建):
  - **BT / WiFi 深层初始化**——子系统 ROM 数据 / RF 标定无 dump;C SDK 需裁剪这些任务。
  - **逐芯片出厂标定 NV**(`xo_trim` 晶振温补等)——生产时烧录,任何构建产物都不含,固有残留。
  - **真实密码学**(SPACC v2 AES/SHA/RSA 描述符协议)——未在启动路径,按需扩展。
  - **看门狗中断模式超时回调**——超时→复位已建模;超时→中断回调需与 vCPU 同步的 PC 注入,未做。
- **snapshot / migration**——按需,默认不投入。

---

## 与 ws63-rs ROADMAP 的对应

| ws63-rs 阶段 | ws63-qemu 如何加速/验证 | 现状 |
|---|---|---|
| 1 硬件在环 bring-up | 软件在环替代(阶段 0)+ GDB + 双固件回归 | ✅ 已做(阶段 1 待加 qtest) |
| 2 正确性修复(中断重写、I2C/SPI 超时、reset、GPIO pull) | 中断保真(阶段 2)+ 外设深度(阶段 3)无硬件验证 | ✅ 可验证 |
| 3 链接/blob 尖刺 | blob 在正确内存布局下链接(阶段 5 子项) | ⏸ 待连接性推进 |
| 4 porting 层 + HCC IPC | HCC 是 host↔device 软件链路(非双核);按需在 MAC 边界建模 | ⏸ 可选 |
| 5 连接性示例(scan→connect→ping) | Wi-Fi MAC + SLIRP,不仿 RF(阶段 5,可选) | ⏸ 暂不投入 |
| 6 async(embassy) | 依赖阶段 2 的中断投递(已完成) | ✅ 底座就绪 |
