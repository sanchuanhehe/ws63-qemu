# Rust 工具链是否需要实现 xlinx 扩展？（含 HiSpark riscv-llvm 调研）

> 结论先行：**不必要**。ws63-rs 用标准 `rv32imfc` 即可正确运行（已在 ws63-qemu 上验证）；
> 在 Rust 工具链里实现 xlinx 仅是「代码密度优化」，代价是把 Rust 重挂到 HiSilicon 的 LLVM fork，
> 收益有限且只能拿到一个**很小的子集**。

## 调研：[gitee.com/HiSpark/riscv-llvm](https://gitee.com/HiSpark/riscv-llvm)（LLVM 15.0.4 fork）

该 fork 在 `llvm/lib/Target/RISCV/` 里**确有** xlinx 扩展，但只是一个名为 **"xlinxma GROUP1"** 的小集合：

- `RISCV.td`：`SubtargetFeature<"xlinxma", "HasExtLinxma", "true", "LINXM GROUP1 Instructions">`
- 指令定义在 `HiMCUGenInstrInfo.td` / `HiMCUGenImmOperands.td` / `HiMCUGenCompressPat.td`，**仅 5 条 16 位指令**：
  `c.neg`、`c.sbz`/`c.shz`/`c.swz`（store-zero，把 x0 存到 byte/half/word）、`c.xori`。

## 关键发现：GCC 与 LLVM 的自定义 ISA **不一致**

| 工具链 | 自定义集合 | C SDK / ws63-rs 用哪个 |
|--------|------------|------------------------|
| **GCC**（`riscv32-linux-musl-gcc`，`-march=rv32imfcxlinxma_xlinxmb_xlinxmc`）| **完整集**：`l.li`、`{add,sub,or,xor,and}shf`、`b*i`、`muliadd`、`jal16`/`j16`、`ldmia`/`stmia`、`push`/`pop`/`popret`、`uxtb`/`uxth`、压缩 `lbu`/`lhu`/`sb`/`sh`（见 [`xlinx-isa.md`](xlinx-isa.md)）| **C SDK（ws63-liteos-app/flashboot）用 GCC 全集** |
| **LLVM**（HiSpark fork 15.0.4，`xlinxma`）| **GROUP1 小集**：`c.neg`、`c.sbz`/`shz`/`swz`、`c.xori`（5 条）| ws63-rs **未用**（标准 rust = 标准 LLVM = 无 xlinx）|

即：QEMU 里我实现的是 **GCC 全集**（C SDK 固件所需）；LLVM fork 的 GROUP1 是另一套、且是早期/部分移植，
两者编码空间与指令都不同（如 LLVM `c.sbz` 在 funct3=0001/Q2，GCC 的 `sb` 在 funct3=101/Q0）。

## ws63-rs 是否需要 xlinx？

- **正确性：不需要**。ws63-rs 由自定义 rust 工具链以 `riscv32imfc`（硬浮点、无原子）构建，
  blinky/uart_hello/timer_irq/gpio_irq 均在 ws63-qemu 上正确运行 —— 标准指令集足够。
- **代码密度/性能：有边际收益**。xlinx（尤其 GCC 的 `l.li`/`push`-`pop`/`*shf`）能压缩固件体积、减少指令数；
  对 flash 受限的 MCU 是 nice-to-have，但非必需。
- **代价很高**：要让 rustc 发射 xlinx，必须把 Rust 的 LLVM 后端换成 HiSilicon 的 fork（15.0.4，相对当前 rustc 偏旧），
  自行构建 rustc —— 工程量大；且即便如此，**Rust 只能拿到 LLVM 的 GROUP1 小集**（5 条），拿不到 GCC 的完整密度优化。

## 建议

1. **ws63-rs 维持标准 `rv32imfc`**（现状），不引入 xlinx —— 正确、可用、工具链可维护。
2. 若未来确需极致代码密度：评估把 GCC 的 `xlinxma_mb_mc` 通过 inline asm/intrinsic 局部使用（汇编器支持），
   或推动 HiSilicon LLVM 把 GCC 全集补齐后再考虑 rustc 重挂 —— 优先级低。
3. **对 ws63-qemu 的影响**：当前 QEMU 实现 GCC 全集，足以跑 C SDK 与（未来若用 inline-asm-xlinx 的）rs。
   若有人用 HiSpark LLVM 构建固件，需再补这 5 条 GROUP1 指令（编码已在 `HiMCUGen*.td`，工作量小）——目前无此需求。
