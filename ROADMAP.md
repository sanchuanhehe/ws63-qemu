# ws63-qemu 路线图（ROADMAP）

> 本路线图规划 `ws63-qemu` 从当前的「MVP + 中断(26–31) + Timer/GPIO/SYS_CTL0/UART」成长为一个
> **单核 SoC 保真、能在无硬件下验证 [ws63-rs](https://github.com/sanchuanhehe/ws63-rs) 驱动**的仿真器。
> 设计与现状见 [`docs/design.md`](docs/design.md)，内存映射见 [`docs/memory-map.md`](docs/memory-map.md)。

## 北极星

**在没有 EVB 的情况下，把 ws63-rs 固件「跑得足够真」以验证驱动正确性**——内存布局 / 启动 / 链接脚本、
外设寄存器与时序、中断驱动逻辑，乃至（远期、单核、不仿 RF）连接性 IPC 边界。

QEMU 是手段，是 ws63-rs ROADMAP **阶段 1「硬件在环 bring-up」的软件在环替代**，不是终点。它能在真机到位前
验证大量正确性，但**不等于真机**（不建模无线电、时钟树不完整、时序不保真）。

## 关于 WS63 核数（更正记录）

**WS63 是单核**——经 fbb_ws63 C SDK 核实：`ch2_system.md` 明确「系统提供**一个**自研 RISC-V 处理器作为主控 CPU」；
`platform_core.h` 标题为 *Application Core*；`rom_config/` 只有 `acore`；全 SDK 无 `dcore`。Wi-Fi/BT 是
**链接进同一个应用镜像的库**（`libwifi_driver_hmac.a` / `libwifi_driver_dmac.a` / `libwpa_supplicant.a`）——
HMAC/DMAC 是 Wi-Fi **软件分层**（host-MAC / device-MAC），跑在同一颗核上，**不是两颗物理核**。

→ 本路线图**不含双核 / 第二 hart**。连接性若推进，是把 Wi-Fi MAC 建模为**外设** + 主机网络后端，而非第二个 RISC-V 核。

---

## 阶段总览

| 阶段 | 主题 | 状态 |
|------|------|------|
| 0 | MVP + 中断控制器(IRQ 26–31) + Timer/GPIO/SYS_CTL0/UART + 三例 + CI | ✅ 已完成 |
| 1 | 开发与测试基建（GDB / semihosting / qtest / trace / 命名 CPU） | 计划（近期、低成本高杠杆）|
| 2 | **中断控制器保真**（IRQ≥32 投递，`target/riscv` 补丁）| ✅ 已完成（gpio_irq 验证）|
| 3 | 外设深度（GPIO 输入/中断、I2C/SPI、reset、WDT、RTC）| 计划（对齐 ws63-rs 阶段 2）|
| 4 | 时钟树 + 真实度（PLL/门控；eFuse/LSADC 可选）| 计划 |
| 5 | 连接性底座（**可选/研究级**：Wi-Fi MAC 外设 + SLIRP，单核，不仿 RF）| 暂不投入 |
| 6 | 可维护性 / 上游（跟版 rebase、代码隔离、qtest CI、可能上游）| 持续 |

---

## 阶段 0 — 当前已完成

单核 **RV32IMFC** hart（关 A/D/zawrs）、按 `ws63-rt/memory.x` 的内存映射、`-kernel` ELF 载入、复位到 entry；
自定义 **HiSilicon UART ×3**；**Timer ×3**（下数计数器 + 中断 26/27/28）；**GPIO ×3**（输出 set/clr、输入、中断寄存器）；
**SYS_CTL0** 时钟状态（TCXO/PLL 锁）；**ws63-intc**（自定义 `LOCIxx` CSR 状态 + IRQ 路由）；其余外设 catch-all 吸收。

**已端到端验证**（`scripts/smoke-test.sh` 全绿、CI 绿）：`blinky`（驱动真实 GPIO）、`uart_hello`（串口打印）、
`timer_irq`（TIMER_0 → IRQ 26 → ISR，**中断投递闭环**）。

**中断保真边界**：IRQ 26–31（TIMER/RTC/I2C0，标准 `mie` 位）经 `riscv_cpu_update_mip` + 向量化 mtvec **完整投递**；
IRQ ≥32（GPIO/UART…，mcause 32–72 放不进 RV32 的 32 位 mip/mie）目前**只建模 CSR 状态、未投递**——这正是阶段 2 要解决的。

---

## 阶段 1 — 开发与测试基建（近期、低成本高杠杆）

- **GDB**：QEMU 内置 gdbstub 对自定义机器开箱即用。`run.sh` 加 `--gdb`（`-s -S`）+ 调试文档
  （`target remote :1234`、断点/单步/查 CSR）。零核改动。
- **semihosting**：`-semihosting`。给 ws63-rs 加一个极小 semihosting `exit()/print` 助手，让 CI 不靠解析 UART
  即得 pass/fail 退出码（更稳的回归信号）。
- **qtest**：`tests/qtest/ws63-test.c`（libqtest），对 timer/gpio/uart/intc 做**寄存器级、免启动**回归，接入 build + CI。
- **trace-events**：把 GPIO 现在的临时 `qemu_log` 换成 `hw/riscv/trace-events` 正规 trace 事件（`-trace` 友好、可选择性开启）。
- **命名 CPU**（可与阶段 2 合并）：`target/riscv` 加 `ws63` CPU 类型（`-cpu ws63` = rv32imfc），免去 machine init 里逐位设扩展。

**验收**：qtest 在 CI 中跑过；GDB 能连上断点；三例冒烟仍绿。

---

## 阶段 2 — 中断控制器保真（核心解锁项）

让 IRQ≥32（GPIO/UART/DMA…）真正投递。需要改 QEMU CPU 核，仿 Andes/Nuclei 的 **vendor-CSR 注册法**。

- 新增 `target/riscv/csr_ws63.c`：把 `LOCIEN0-2`(0xBE0)、`LOCIPRI0-15`(0xBC0)、`LOCIPCLR`(0xBF0)、`LOCIPD`
  做成**带 per-CPU 状态的真实 CSR**（替换现在 0xBC0–0xBFF 的 backed storage）。
- **本地中断投递路径**（`target/riscv` 补丁的关键）：当设备 IRQ≥32 pending 且 `LOCIEN` 使能、优先级达标时，
  让 CPU 以 `mcause = irq(32–72)` 取中断 + 向量化 mtvec → `mtvec + 4*irq` → ws63-rt 的 `local_interrupt_handler`。
  自定义 pending 通路，绕开 RV32 32 位 mip/mie 容不下 ≥32 的限制。
- 把现有外设 IRQ 线（GPIO 33–35、UART 53–55…）经 `ws63-intc` 接到此投递。
- **可维护性**：全部落在新文件 + 对 `csr.c` / `cpu_helper.c` 的最小 hook，便于跟版 rebase；`build.sh` 追加注入。

**验收**：新增中断式示例（如 `gpio_irq` / `uart_rx_irq`），端到端证明 ≥32 投递。
**解锁**：ws63-rs 阶段 2 中断子系统重写的验证 + 一切中断式驱动。

---

## 阶段 3 — 外设深度（对齐 ws63-rs 阶段 2 正确性修复）

- **GPIO 输入 + 边沿/电平中断**：经 monitor/QOM 属性驱动引脚输入 → 边沿检测 → IRQ 33–35（依赖阶段 2）；
  验证 GPIO 中断驱动 + pull（IO_CONFIG）。
- **I2C0/1**：建模 `I2C_SR` 状态 + dummy slave/loopback，使 HAL I2C 驱动可跑 + 超时路径可测（ws63-rs 阶段 2 I2C 超时修复）。
- **SPI0/1**：建模 `SPI_WSR` 状态 + loopback，验证已修的 SPI 驱动（trsm/SCKDV/超时）。
- **system reset**：`GLB_CTL_M`(0x40002110) + `SYS_RST_RECORD`(0x400000A0)，使 `software_reset()`/`reset_reason()`
  触发 QEMU system_reset。
- **WDT**：倒计时 → 复位（看门狗超时可测）。**RTC**：计数 + RTC 中断（29，属 mie 类，已可投递）。
- 每个：sysbus device + qtest，模式同 `ws63-timer` / `ws63-gpio`。

---

## 阶段 4 — 时钟树 + 真实度（可选）

- 更完整的 SYS_CTL0/CLDO_CRG/TCXO：时钟门控、PLL 配置、频率上报，使 `init_clocks` 做真实工作，且时钟相关时序
  （UART 波特、timer 速率，当前名义 24 MHz）反映配置。
- **eFuse / LSADC 最小建模**（顺带价值）：可用来**验证上周修复的 eFuse/LSADC 寄存器序列**（读窗口 base+0x800、
  LSADC `ctrl_8/9/11` 映射等）——把静态对照 SDK 的修复变成可执行验证。

---

## 阶段 5 — 连接性底座（可选 / 研究级，单核，不仿 RF）

- **不是双核**（WS63 单核已核实）。若推进：把 **Wi-Fi MAC 建模为外设**，主机侧用 **SLIRP / 假以太**在 MAC/驱动边界
  收发帧（仿 esp-qemu——**不仿无线电 / PHY**）；Wi-Fi 驱动库跑在单核上，QEMU 给 MAC 寄存器接口 + 注入/取出帧。
- 含 ws63-rs 阶段 3 的 blob 尖刺：让 `libwifi_rom_data.a` 在正确内存布局下链接、外部符号解析（QEMU 内存布局已基本就绪）。
- 研究级、低置信、依赖 ws63-rs 走到连接性。**明确标注为远期可选尾部**。

---

## 阶段 6 — 可维护性 / 上游（持续）

- 跟 QEMU release（v9.2 → v10.x LTS）rebase；自定义代码隔离到新文件（`hw/riscv/ws63*.c`、`target/riscv/csr_ws63.c`）
  + 最小 hook；考虑用正式 patch-series 取代「拷文件 + sed 注入」。
- CI 矩阵：build + qtest + 功能冒烟（blinky/uart_hello/timer_irq + 新中断/外设示例）；QEMU 构建缓存。
- 可能向上游提交基础机器（`hw/riscv/ws63.c`）+ qtests（HiSilicon WS63 board）。

---

## 冻结 / 非目标

- **RF / PHY 无线电仿真**——不做；连接性若推进只到 MAC / SLIRP 边界（同 esp-qemu）。
- **双核 / 第二 hart**——WS63 单核（已核实），不做。
- **周期精确时序**、**SPACC/PKE 真实密码学**、**snapshot / migration**——按需再说，默认不投入。

---

## 与 ws63-rs ROADMAP 的对应

| ws63-rs 阶段 | ws63-qemu 如何加速/验证 |
|---|---|
| 1 硬件在环 bring-up | ✅ 已做软件在环替代（阶段 0）；阶段 1 加 GDB/qtest 让验证更扎实 |
| 2 正确性修复（中断重写、I2C/SPI 超时、reset、GPIO pull） | 阶段 2（中断保真）+ 阶段 3（外设深度）在无硬件下验证 |
| 3 链接/blob 尖刺 | 阶段 5 子项：blob 在正确内存布局下链接 |
| 4 porting 层 + HCC IPC | HCC 是 host↔device 软件链路（非双核）；按需在 MAC 边界建模 |
| 5 连接性示例（scan→connect→ping） | 阶段 5（可选）：Wi-Fi MAC + SLIRP，不仿 RF |
| 6 async（embassy） | 依赖阶段 2 的中断投递（中断驱动 I/O） |
