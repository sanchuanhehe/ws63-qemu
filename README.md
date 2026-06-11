# ws63-qemu

在 **QEMU** 上仿真 HiSilicon **WS63**（RISC-V RV32IMFC，Wi-Fi 6 + BLE + SLE/星闪 SoC），
用于无硬件运行 [`ws63-rs`](https://github.com/hispark-rs/ws63-rs) 裸机固件。

仿照 [esp-qemu](https://github.com/espressif/qemu) 的做法：fork 一个固定版本的 QEMU，加入一个
in-tree 板卡文件 `hw/riscv/ws63.c`（WS63 机器模型 + 自定义 UART），只构建 `riscv32-softmmu`。

> 状态：单核 SoC 模型。CPU + xlinx 自定义 ISA、内存、中断（IRQ 26–31 + 自定义本地 ≥32，含 LOCIPRI/PRITHD
> 优先级阈值）、**全部 35 个 SVD 外设**（DMA/RTC/WDT/I2C/SPI/I2S/LSADC/UART/TSENSOR/EFUSE/TRNG/SFC +
> GPIO 引脚网/pinmux + CLDO_CRG 时钟门控）均已建模；跑通 ws63-rs（blinky/uart_hello/timer_irq/gpio_irq）与
> fbb_ws63 C SDK（flashboot + ws63-liteos-app 启动到调度器）。可选 `-icount` 确定性指令计时。
> 开发/测试基建齐备:GDB(`-s -S`)、qtest 寄存器级回归、semihosting 退出码、GPIO/DMA trace 事件、命名 `-cpu ws63`。
> **用户手册见 [docs/user-manual.md](docs/user-manual.md)**（安装 / 使用 / 注意事项 / 验证覆盖范围）；
> 设计见 [docs/design.md](docs/design.md)，掩膜 ROM 桩见 [docs/rom-stubs.md](docs/rom-stubs.md)，
> 变更见 [CHANGELOG.md](CHANGELOG.md)，规划见 [ROADMAP.md](ROADMAP.md)。

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
bash scripts/run.sh ../ws63-rs/target/riscv32imfc-unknown-none-elf/release/timer_irq  # 定时器中断
#   退出 QEMU：Ctrl-A 然后 X

# 5. 一键冒烟（启动真实固件：ws63-rs 各示例 + C SDK 样例）
WS63_RS=../ws63-rs bash scripts/smoke-test.sh

# 6. 寄存器级 qtest（免启动，直接驱动 GPIO/UART/timer/INTC/DMA 模型）
bash scripts/qtest.sh

# semihosting：固件用 SYS_EXIT 设置 QEMU 退出码（CI 免解析 UART 即得 pass/fail）
SEMIHOST=1 bash scripts/run.sh ../ws63-rs/target/riscv32imfc-unknown-none-elf/release/semihost_selftest
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
| 机器 | `-M ws63`：单 **RV32IMFC** hart（命名 CPU `-cpu ws63` = I/M/F/C + Zicsr/Zcf，关 A/D、无 MMU；为默认 CPU 型号），WS63 内存映射，复位到 ELF entry |
| UART0/1/2 | **自定义 HiSilicon UART**（非 16550）；TX 直接输出到 `-serial` |
| TIMER ×3 | 下数计数器 + 中断（IRQ 26/27/28），周期重载 |
| GPIO0/1/2 | 输出 set/clr、输入、中断寄存器 |
| SYS_CTL0 | 时钟状态（TCXO/PLL 锁），使 `init_clocks()` 跑通 |
| 中断控制器 | 自定义 `LOCIxx` CSR + IRQ 路由；**IRQ 26–31（mie 类）+ ≥32（自定义本地中断，target/riscv 补丁）均完整投递**（timer_irq / gpio_irq 已验证；见 [design](docs/design.md)） |
| 内存 | BOOTROM/ROM/ITCM/DTCM/FLASH/SRAM，按 `ws63-rt/memory.x` 布局（见 [docs/memory-map.md](docs/memory-map.md)） |
| 其它外设 | `create_unimplemented_device` 吸收（I2C/SPI/PWM/…），`-d unimp` 按地址可追踪 |

## 目录结构

```
ws63-qemu/
├── src/
│   ├── hw/riscv/ws63.c                            # 机器模型 + 自定义 UART 设备（注入 QEMU 树）
│   ├── target/riscv/insn_trans/trans_xlinx.c.inc  # xlinx 自定义 ISA 解码（注入）
│   └── tests/qtest/ws63-test.c                    # 寄存器级 qtest（注入 QEMU 树）
├── patches/                          # 对既有 QEMU 文件改动的 patch-series，**按版本分目录**
│   ├── v10.0.0/                       #   当前默认基线（git format-patch 生成）
│   │   ├── 0001-target-riscv-*.patch  #     CPU 型号 + 本地中断(≥32) + xlinx 解码 hooks + ROM 拦截
│   │   ├── 0002-hw-riscv-*.patch      #     注册机器（meson 源集 + Kconfig + trace-events）
│   │   └── 0003-tests-qtest-*.patch   #     注册 ws63-test（qtests_riscv32）
│   ├── v11.0.1/                       #   最新稳定主版（+ 0004 hw/→hw/core/ + 0005 修上游 PMU 回归）
│   ├── v10.2.3/                       #   最新 10.2.x，已移植（同结构 + 0004 适配 ws63.c）
│   └── v9.2.4/                        #   上一基线，仍维护（同结构 + 0004 适配 ws63.c 的旧 API）
├── scripts/
│   ├── setup-deps.sh        # 安装构建依赖（含 libslirp-dev）
│   ├── build.sh             # 克隆 QEMU@$QEMU_TAG + 拷源文件 + apply patches/$QEMU_TAG/ + 构建（幂等）
│   ├── run.sh               # 运行固件 ELF（DEBUG/ICOUNT/NV/SEMIHOST 开关）
│   ├── smoke-test.sh        # 启动真实固件冒烟（ws63-rs 各示例 + C SDK）
│   ├── csdk-test.sh         # C SDK 外设样例回归
│   └── qtest.sh             # 寄存器级 qtest 运行器
├── tests/csdk/              # 预构建 C SDK 样例 ELF + flash/NV 镜像（测试夹具）
├── docs/                    # design.md / memory-map.md / user-manual.md / xlinx-isa.md …
└── qemu/                    # 由 build.sh 克隆的 QEMU 树（.gitignore）
```

## 真值来源

- 内存布局：`ws63-rs/ws63-rt/{memory.x,layout.ld}`（固件链接地址）。
- 外设基址 / 寄存器：`ws63-rs/ws63-pac/ws63-svd/WS63.svd`（svd 现为 ws63-pac 的嵌套子模块）。
- UART 行为：`ws63-rs/ws63-hal/src/uart.rs` + fbb_ws63 `hal_uart_v151_regs_def.h`。

## 许可

`src/hw/riscv/ws63.c` 派生自 QEMU 代码风格，按 **GPL-2.0-or-later** 授权（与 QEMU 一致）。
