# ws63-qemu 路线图（ROADMAP）

> 本路线图记录 `ws63-qemu` 的目标、已交付内容与剩余工作。
> **现状(v0.3.0)**:已是一个**单核 SoC 行为保真、能在无硬件下验证 [ws63-rs](https://github.com/sanchuanhehe/ws63-rs)
> 驱动并运行 fbb_ws63 C SDK 厂商固件**的仿真器——最初规划的「从 MVP 成长为单核 SoC 保真器」目标(阶段 0–4)
> **已基本达成**。连接性底座(合成 MAC + SLIRP 的 ping/UDP 软件路径)已完成;剩余的是测试基建打磨、
> 可维护性/上游,以及真实 Wi-Fi 栈(blob)这一可选远期尾部。
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
| 1 | 开发与测试基建(GDB / 真实固件回归 / qtest / semihosting / trace / 命名 CPU) | ✅ **已完成**(GDB + 双固件回归 + qtest 寄存器级回归 + semihosting 退出码 + GPIO/DMA trace 事件 + `-cpu ws63`)|
| 2 | **中断控制器保真**(IRQ≥32 投递 + LOCIPRI/PRITHD,`target/riscv` 补丁)| ✅ **已完成**(gpio_irq 端到端验证)|
| 3 | 外设深度(GPIO 输入/中断、I2C/SPI、reset、WDT、RTC、DMA…)| ✅ **已完成**(全 35 外设,详见 design.md 矩阵)|
| 4 | 时钟树 + 真实度(门控/源路由;eFuse/LSADC)| ✅ **已完成**(门控+源路由+eFuse/LSADC;完整 PLL→波特时序本质不可观测)|
| 5 | 连接性底座(**可选/研究级**:Wi-Fi MAC 外设 + SLIRP,单核,不仿 RF)| ✅ 已完成(M1 SLIRP + M2 MAC + M3 net_ping + M4 冒烟入口) |
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

## 阶段 1 — 开发与测试基建 ✅ 已完成

全部六项已实现并进 CI:

- **GDB** ✅ — QEMU 内置 gdbstub 对本机器开箱即用;`run.sh` 透传 `-s -S` 调试,文档见 [`docs/user-manual.md` §2.3](docs/user-manual.md)。
- **真实固件回归** ✅ — CI 每次 push/PR 跑 `smoke-test.sh`(ws63-rs:blinky/uart_hello/timer_irq/gpio_irq/reset_demo/
  dma_loopback/wifi_blob_link/rf_port_demo/sched_selftest/semihost_selftest)+ `csdk-test.sh`(C SDK 样例
  tcxo/systick/adc/dma + NV,5/5)。比原计划只验 ws63-rs 更强:**用真实厂商固件**交叉验证外设模型。
- **qtest** ✅ — `tests/qtest/ws63-test.c`(libqtest)对 GPIO/UART/timer/INTC/DMA 做**寄存器级、免启动**回归
  (4 例:GPIO 数据/OEN/中断使能读写、UART 复位值、timer 装载/使能/触发 + 经 INTC 投递 IRQ 26、DMA 通道 0 mem→mem
  搬运 + 完成位)。由 `scripts/build.sh` 注入 + meson 注册,`scripts/qtest.sh` 运行,接入 CI。补足「整机启动断言」之外
  的细粒度快速回归层。
- **semihosting** ✅ — `run.sh SEMIHOST=1` 加 `-semihosting`;ws63-rs `ws63-examples/semihost_selftest` 用极小
  RISC-V semihosting 助手(`SYS_EXIT_EXTENDED`/`SYS_WRITE0`)以**退出码**报告 pass/fail,CI 不靠解析 UART 即得结果
  (`smoke-test.sh` 断言退出码 0)。
- **trace-events** ✅ — GPIO 的临时 `qemu_log` 换成 `hw/riscv/trace-events` 正规 trace 事件
  (`ws63_gpio_set` / `ws63_gpio_clr` / `ws63_dma_xfer`);`-d trace:ws63_gpio_*` 选择性开启(`build.sh` 追加事件,
  `smoke-test.sh` 据此断言 blinky)。
- **命名 CPU** ✅ — `target/riscv` 补丁加 `ws63` CPU 类型(`-cpu ws63` = RV32IMFC_Zicsr + Zcf,无 A/D、无 MMU,
  禁用 Zcb/Zcmp 以让 xlinx 自定义压缩编码空间)。machine 默认即此型,不再逐位设扩展属性——这也让机器在
  `-accel qtest` 下可初始化(qtest 不暴露可配置 CPU 的逐字母属性)。

**验收**:✅ qtest 在 CI 跑过(4/4);两套固件冒烟 + C SDK 样例仍全绿(`-cpu ws63` 下)。

---

## 阶段 2 — 中断控制器保真 ✅ 已完成

IRQ≥32(GPIO/UART/DMA…)已真正投递。`target/riscv` 补丁(patch-series `patches/0001-target-riscv-*.patch`)实现:

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
- **源路由**:`CLDO_CRG_CLK_SEL@0x44001134` TCXO/PLL 选择建模为状态。**定时器/WDT 计数时钟已校正为 TCXO 晶体
  (24/40 MHz)**——非 PLL PCLK(2026-06 对照 fbb_ws63 `clock_init.c`:`timer_porting_clock_value_set(REQ_24M)`,
  删去 `ws63_pclk_hz`/`WS63_PLL_HZ`);与 ws63-rs HAL 的 timer/WDT 时钟修复一致,smoke-test 全绿。
- **eFuse / LSADC** 已最小建模并**行为完整**——把原先静态对照 SDK 的修复变成可执行验证(LSADC `ctrl_8/9` 转换、EFUSE 读窗口)。
- **本质受限**(非缺陷):UART 波特、SPI 分频等时序在 QEMU chardev 不限速下**不可观测**,故源/分频仅记录状态。
  完整时钟树(FNPLL 2880 MHz + CLDO_CRG)已在 [ws63-guide ch8](https://github.com/sanchuanhehe/ws63-guide) 实证落盘。

---

## 阶段 5 — 连接性底座(可选 / 研究级,单核,不仿 RF)✅ 已完成

**不是双核**(WS63 单核已核实)、**不仿无线电 / PHY**(同 esp-qemu)。思路:把 **Wi-Fi MAC 建模为外设**,主机侧用
**SLIRP / 假以太**在 ws63-rf-rs 的 netif 缝合点收发帧——而非仿真射频。分里程碑推进:

| 里程碑 | 内容 | 状态 |
|--------|------|------|
| M1 | 构建打开 `--enable-slirp`,`-nic user` 提供 SLIRP NAT 后端 | ✅ 已完成 |
| M2 | `ws63.c` 加**合成 MAC 设备**(`ws63-netmac` @ 0x44210000,IRQ 45):TX 寄存器→`qemu_send_packet`、netdev RX→RX 缓冲 + IRQ;qtest 整帧收发回环 | ✅ 已完成 |
| M3 | ws63-rs `net_ping` 示例:`set_tx_sink`→写 MAC TX、MAC RX IRQ(45)→`rx_push`、smoltcp 静态 IP(10.0.2.15/24,网关 10.0.2.2)走 ARP/ICMP/UDP | ✅ 已完成 |
| M4 | QEMU 冒烟测试入口:`smoke-test.sh` 加 net_ping(`-nic user`),grep `NET PING: PASS`;`setup-deps.sh` 补 `libslirp-dev`;CI/release 构建 net_ping | ✅ 已完成 |

> M3 实证(`-M ws63 -nic user`):ARP 解析网关 → ICMP echo 在 seq=0 收到应答(SLIRP 本地应答,无需外网)→ `rx irq hits=2`
> 证明 WLMAC 中断投递了 ARP 应答 + echo 应答 → `NET PING: PASS`,多次运行稳定。**注意**:MTU 级 RX 暂存缓冲必须放在
> 被调用的 `drain_rx()` 里、而非内联进汇编陷阱处理器的手写栈帧——2 KB 局部会溢出并静默破坏 RX 投递。

- **合成 MAC 不是厂商 WLMAC 的寄存器级复刻**:它是驱动验证用的最小以太帧接口,绕开闭源 RF/PHY blob(后者仅硬件在环)。
- 远期 blob 尖刺(ws63-rs 阶段 3):让 `libwifi_rom_data.a` 在正确内存布局下链接、外部符号解析(QEMU 内存布局已就绪)——
  独立于本合成 MAC,属真实 Wi-Fi 栈路线,低置信。

---

## 阶段 6 — 可维护性 / 上游(持续)🔄

- **默认基线已升到 QEMU v10.0.0**(原 v9.2.4):处理的 API 变化 = 头文件 `sysemu/`→`system/`、`Property[]` 去掉
  `DEFINE_PROP_END_OF_LIST` 终止符并改 `const`(经 `device_class_set_props_n`/`ARRAY_SIZE`);v10 上 qtest 5/5 + 双固件冒烟
  + C SDK 样例全绿。`src/` 跟随默认基线(v10);v9.2.4 的旧 API 差异由其序列的 `0004` 补丁吸收。自定义代码隔离在
  `hw/riscv/ws63.c` + `insn_trans/trans_xlinx.c.inc` + 按版本分目录的 patch-series。
- **「拷文件 + sed 注入」已被正式 patch-series 取代,且按 QEMU 版本分目录维护**(`patches/<tag>/`,`git format-patch`
  生成、`git apply` 应用):`0001` target/riscv 核(CPU 型号 + 本地中断 + xlinx 解码 + ROM 拦截)、`0002` 注册机器
  (meson/Kconfig/trace-events)、`0003` 注册 qtest;旧版本另带 `0004` 适配 ws63.c 的旧 API。`build.sh` 选 `patches/$QEMU_TAG/`
  应用,仅剩「拷 3 个新文件 + apply 该版本序列」,无 sed/cat 追加;版本未建目录则报错列出已支持版本。详见 [`patches/README.md`](patches/README.md)。
- **同时维护 v10.0.0(默认)、v10.2.3、v9.2.4 三条序列**,均实测「序列可应用 + 构建 + qtest 5/5」(v10.x 另过双固件冒烟 + C SDK)。
  10.0→10.2 的漂移很能说明问题:`insn_len` 挪进 `internals.h`、CPU 定义改声明式 `DEFINE_RISCV_CPU`、`decode_opc` 改表驱动、
  `CharBackend`→`CharFrontend`、`exec/`→`system/address-spaces.h`——正因如此才按版本分目录。
- **CI 已就位**(build + 双固件冒烟 + C SDK 样例 + QEMU 构建缓存),默认 tag 升至 v10.0.0。
- **qtest 矩阵已就位**(`qtest-matrix.yml`):寄存器级 qtest 在**每个有 `patches/<tag>/` 的版本上必过**(v10.0.0 + v10.2.3 + v9.2.4),
  外加**最新主版 v11.0.1 实验性**(`continue-on-error`,当前红:尚无 `patches/v11.0.1/`,是「给该版本补一套序列」的信号)。
  无需 Rust 工具链/不启动固件,是 out-of-tree overlay 漂移雷达。周一定时跑一次以捕获新发布的 QEMU。
- 可能向上游提交基础机器(`hw/riscv/ws63.c`)+ qtests(HiSilicon WS63 board)——patch-series 已是上游友好格式。

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
| 1 硬件在环 bring-up | 软件在环替代(阶段 0)+ GDB + 双固件回归 + qtest 寄存器级回归 + semihosting 退出码 | ✅ 已做(阶段 1 完成)|
| 2 正确性修复(中断重写、I2C/SPI 超时、reset、GPIO pull) | 中断保真(阶段 2)+ 外设深度(阶段 3)无硬件验证 | ✅ 可验证 |
| 3 链接/blob 尖刺 | blob 在正确内存布局下链接(阶段 5 子项) | ⏸ 待连接性推进 |
| 4 porting 层 + HCC IPC | HCC 是 host↔device 软件链路(非双核);按需在 MAC 边界建模 | ⏸ 可选 |
| 5 连接性示例(scan→connect→ping) | 合成 MAC + SLIRP,不仿 RF(阶段 5)| ✅ ping/UDP 软件路径已做(net_ping over SLIRP);真实 scan/connect 需 blob,属阶段 3 尾部 |
| 6 async(embassy) | 依赖阶段 2 的中断投递(已完成) | ✅ 底座就绪 |
