# HiSilicon WS63 "xlinx" 自定义 RISC-V 指令扩展

WS63 的 **riscv31** 核在标准 RV32IMFC 之上实现了一组 HiSilicon 私有指令（厂商工具链
`riscv32-linux-musl-gcc -march=rv32imfcxlinxma_xlinxmb_xlinxmc` 默认发射）。**真实 C SDK
固件（`fbb_ws63`）通篇使用它们**，因此要在 QEMU 上跑厂商编译的固件，必须实现这些指令。

它们落在 RV32 保留 / RV64-only（`0x1b`/`0x3b`）与 custom-N（`0x0b`/`0x5b`/`0x7b`）的 32 位
opcode 空间、一种 48 位（marker `0x1f`）形态、以及被复用的压缩编码空隙（funct3=100 的
push/pop/uxt，funct3=001/101 的 D-float 槽位）。在我们的 RV32 核上标准解码器都会落空，转交
[`src/target/riscv/insn_trans/trans_xlinx.c.inc`](../src/target/riscv/insn_trans/trans_xlinx.c.inc)。

> **编码为实测真值**：用厂商汇编器逐操作数扫描（汇编 → 读原始字节 → 对每个字段做差分）得到；
> 语义经厂商代码生成（`gcc -S`）与 13 条单元自检（`scripts/`/会话记录）核对。

## 32 位指令

| 助记符 | opcode[6:0] | 语义 | 字段 |
|--------|-------------|------|------|
| `l.li rd, imm32` | 48 位，halfword0 `& 0x3f == 0x1f`，bits[15:12]=0 | `rd = imm32` | rd=bits[11:7]；其后 4 字节小端 = imm32 |
| `{add,sub,or,xor,and}shf rd,rs1,rs2,mode,sh` | `0x1b` | `rd = rs1 OP (rs2 移位 sh)` | funct3: 0 add,1 sub,2 or,3 xor,4 and；rd[11:7] rs1[19:15] rs2[24:20]；sh=bits[29:25]；mode=bits[31:30]: 0 sll,1 srl,2 sra,3 ror |
| `{beq,bne,blt,bge,bltu,bgeu}i rs1,imm8,off` | `0x3b` | 比较 `rs1` 与 8 位有符号 `imm8`，成立则跳转 | funct3: 0 eq,1 ne,4 lt,5 ge,6 ltu,7 geu；rs1[19:15]；imm8=bits[31:24]（符号）；off: imm[1]=bit7, imm[5:2]=bits[11:8], imm[9:6]=bits[23:20]（10 位有符号，±1KB）|
| `muliadd rd,rs1,rs2,imm6` | `0x5b`（bits[13:12]=01）| `rd = rs1 + rs2 * imm6` | rd[11:7] rs1[19:15] rs2[24:20]；imm6 = bit14 \| (bits[29:25]<<1) |
| `jal16 / j16 target` | `0x7b` | `pc += off`；`jal16`(bit7=0) 置 `ra=pc+4`，`j16`(bit7=1) 置 `rd=x0` | **25 位有符号 off（±16MB）**：标准 J 型给 imm[20:1]（imm[10:1]=bits[30:21], imm[11]=bit20, imm[19:12]=bits[19:12], imm[20]=bit31）**外加** imm[24:21]=bits[11:8]，符号位 imm[24]=bit11 |
| `ldmia/stmia {ra,s0-s11},(base)` | `0x0b` | 多寄存器 load/store，地址递增，base 不回写 | bit12: 0 ld / 1 st；base=bits[19:15]；寄存器存在位图：ra=bit7, s0=bit9, s1=bit10, s2..s11=bits21..30 |

> `j16 a0e46e <memcpy>` 这类调用偏移可达 ~9MB，远超标准 J 型 ±1MB —— 这正是 25 位立即数
> （bits[11:8] 承载高位）存在的原因；曾因漏掉 imm[23] 导致目标地址丢 bit23（`0xa0e46e`→`0x20e46e`）。

## 16 位指令（复用压缩编码空隙）

| 助记符 | 匹配 | 语义 | 字段 |
|--------|------|------|------|
| `push/pop/popret {ra,s0-sN}, spimm` | funct3=100,Q0：`insn & 0xE003 == 0x8000` | Zcmp 式多寄存器入/出栈（popret 末尾 `ret`）| bits[3:0]=op（8 push,0 pop,4 popret）；bits[7:4]=rlist（1..13 ⇒ {ra,s0..s(N-2)}）；bits[12:8]=extra；`stack_adj = round_up(n*4,16)+extra*16` |
| `uxtb/uxth rd'` | `insn & 0xFC5F == 0x9C01` | `rd' = zext8/zext16(rd')` 原地 | rd'=bits[9:7]（x8+）；bit5: 0 字节 / 1 半字 |
| 压缩 `lbu/lhu rd',imm(rs1')` | funct3=001,bit0=0：`insn & 0xE001 == 0x2000` | 无符号字节/半字 load（占用 C.FLD/C.FLDSP 空槽）| rd'=bits[4:2], rs1'=bits[9:7]（均 x8+）；bit1: 0 字节 / 1 半字；imm[4:0]={bit11,bit10,bit6,bit5,bit12}（字节偏移，不缩放）|
| 压缩 `sb/sh rs2',imm(rs1')` | funct3=101,bit0=0：`insn & 0xE001 == 0xA000` | 字节/半字 store（占用 C.FSD/C.FSDSP 空槽）| rs2'=bits[4:2], rs1'=bits[9:7]；其余同上 |

## 在 QEMU 中的接入

- 新文件 `target/riscv/insn_trans/trans_xlinx.c.inc`：`decode_xlinx16/32/48` + 各 `trans_*`。
- `target/riscv/translate.c`（见 `patches/ws63-target-riscv.patch`）最小 hook：
  - `insn_len()`：`(first_word & 0x3f) == 0x1f` ⇒ 6 字节（48 位 `l.li`）；`MAX_INSN_LEN` 4→6。
  - `decode_opc()`：16/32 位标准解码落空后转 `decode_xlinx16/32`；新增 6 字节分支取 `l.li`。
- **必须关闭 Zcb/Zcmp/Zcmt**（见 `hw/riscv/ws63.c`）：它们与 xlinx 的 funct3=100 push/pop/uxt
  编码空间冲突，会误解码（曾把 `push {ra},-16`(0x8018) 误当 Zcb load → load fault）。保留 Zcf
  （压缩浮点 load/store，编码空间不冲突，trap 向量里 `c.fsw` 会用到）。

## 验证现状

- **13/13 指令自检通过**（l.li 保 bit23、shf 全族、muliadd、branch-imm、push/pop/popret、ldmia/stmia）。
- 真实 `ws63-liteos-app.elf` 在 QEMU 上完成 relocation / dyn_mem_cfg / riscv_patch_init / 驱动初始化，
  正确执行**数百万条**指令，直至命中**掩膜 ROM 边界**（见下）。
- **真实 C SDK `flashboot` 已在 QEMU 上跑出 UART 输出**（厂商 gcc 编译、自包含、零 ROM 依赖）：
  ```
  Flash Init Fail! ret = 0x80001341
  SFC fix SR ret =0x80000002
  ```
  即 xlinx ISA + 内存映射 + 启动参数(a0) + TCXO 时钟 + UART 全链路打通；后续受阻于未建模的 SFC
  Flash 控制器（0x08000300 等），但 bootloader 不挂死、优雅报错继续。rs SDK 三例无回归。

## 已知边界（非 ISA 问题）

- **掩膜 ROM 依赖**：`ws63-liteos-app` 调用 53 个固化在硅片掩膜 ROM（0x109000–0x14C000）里的函数
  （`vsnprintf_s`/`memcpy_s`/SFC/pin/watchdog/timer/systick…，ABI 见 `rom_config/acore/acore.sym`）。
  SDK 不提供 ROM 二进制（`_rom.bin` 为空），故整应用无法在无 ROM dump 时跑到底。
- **bootloader 启动参数 ABI**：`flashboot`/`loaderboot` 自包含（**零 ROM 依赖**），但入口即
  `lw t3,0(a0)` —— 期望上一级（loaderboot/ROM）经 `a0` 传入启动参数结构；直接 `-kernel` 引导时
  `a0=0` 会 load fault。需伪造启动参数（后续工作）。
- **PPB / FlashPatch**：`0xE0000000` 为核内私有外设总线（FlashPatch 重映射单元 + Cortex-M 式 SCS
  `0xE000E000`）。我们加载完整已打补丁镜像，补丁单元无意义，用 RAM 吸收即可（见 `ws63.c` 的 `ppb`）。
