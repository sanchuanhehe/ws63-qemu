/*
 * HiSilicon WS63 "xlinx" custom RISC-V instruction extensions.
 *
 * The WS63 "riscv31" core (and its vendor toolchain,
 *   riscv32-linux-musl-gcc -march=rv32imfcxlinxma_xlinxmb_xlinxmc)
 * emits a family of non-standard instructions used throughout the
 * fbb_ws63 C SDK firmware.  They live in the RV32-reserved / RV64-only
 * (0x1b/0x3b) and custom-N (0x0b/0x5b/0x7b) 32-bit opcode space, in a
 * 48-bit (marker 0x1f) form, and in the reserved compressed space
 * (funct3=100), so on our RV32 core the standard decoder always falls
 * through to the dispatchers here.
 *
 * Encodings are ground truth, reverse-engineered via the vendor
 * assembler round-trip (assemble -> read raw bytes -> sweep each
 * operand field); semantics confirmed against vendor codegen.
 * See ws63-qemu/docs/xlinx-isa.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

static inline int xlinx_rd(uint32_t i)  { return (i >> 7)  & 0x1f; }
static inline int xlinx_rs1(uint32_t i) { return (i >> 15) & 0x1f; }
static inline int xlinx_rs2(uint32_t i) { return (i >> 20) & 0x1f; }
static inline int xlinx_f3(uint32_t i)  { return (i >> 12) & 0x7;  }

/* return shift(rs2, shamt) for the *shf family. mode: 0 sll,1 srl,2 sra,3 ror */
static TCGv xlinx_shifted(DisasContext *ctx, int rs2, int mode, int shamt)
{
    TCGv s = get_gpr(ctx, rs2, EXT_NONE);
    TCGv out = tcg_temp_new();

    if (shamt == 0) {
        tcg_gen_mov_tl(out, s);
        return out;
    }
    switch (mode) {
    case 0: tcg_gen_shli_tl(out, s, shamt); break;  /* sll */
    case 1: tcg_gen_shri_tl(out, s, shamt); break;  /* srl */
    case 2: tcg_gen_sari_tl(out, s, shamt); break;  /* sra */
    case 3: tcg_gen_rotri_tl(out, s, shamt); break; /* ror */
    }
    return out;
}

/* {add,sub,or,xor,and}shf rd, rs1, rs2, mode, shamt : rd = rs1 OP (rs2 sh shamt) */
static bool trans_xlinx_shf(DisasContext *ctx, uint32_t insn)
{
    int f3 = xlinx_f3(insn);
    int mode = (insn >> 30) & 0x3;
    int shamt = (insn >> 25) & 0x1f;
    TCGv rs1 = get_gpr(ctx, xlinx_rs1(insn), EXT_NONE);
    TCGv shifted = xlinx_shifted(ctx, xlinx_rs2(insn), mode, shamt);
    TCGv dest = dest_gpr(ctx, xlinx_rd(insn));

    switch (f3) {
    case 0: tcg_gen_add_tl(dest, rs1, shifted); break;
    case 1: tcg_gen_sub_tl(dest, rs1, shifted); break;
    case 2: tcg_gen_or_tl(dest, rs1, shifted);  break;
    case 3: tcg_gen_xor_tl(dest, rs1, shifted); break;
    case 4: tcg_gen_and_tl(dest, rs1, shifted); break;
    default: return false;
    }
    gen_set_gpr(ctx, xlinx_rd(insn), dest);
    return true;
}

/* muliadd rd, rs1, rs2, imm6 : rd = rs1 + rs2 * imm6 */
static bool trans_xlinx_muliadd(DisasContext *ctx, uint32_t insn)
{
    int imm6;
    TCGv rs1, rs2, prod, dest;

    if (((insn >> 12) & 0x3) != 0x1) {
        return false;
    }
    imm6 = ((insn >> 14) & 0x1) | (((insn >> 25) & 0x1f) << 1);
    rs1 = get_gpr(ctx, xlinx_rs1(insn), EXT_NONE);
    rs2 = get_gpr(ctx, xlinx_rs2(insn), EXT_NONE);
    prod = tcg_temp_new();
    dest = dest_gpr(ctx, xlinx_rd(insn));
    tcg_gen_muli_tl(prod, rs2, imm6);
    tcg_gen_add_tl(dest, rs1, prod);
    gen_set_gpr(ctx, xlinx_rd(insn), dest);
    return true;
}

/* {beq,bne,blt,bge,bltu,bgeu}i rs1, imm8, offset : compare rs1 with imm8 */
static bool trans_xlinx_branchi(DisasContext *ctx, uint32_t insn)
{
    int f3 = xlinx_f3(insn);
    int32_t cmp = (int32_t)(int8_t)(insn >> 24);
    int32_t off = 0;
    TCGCond cond;
    DisasExtend ext;
    TCGLabel *l;
    TCGv src1;
    target_ulong orig_pc_save;

    /* branch offset: imm[1]=bit7, imm[5:2]=bits[11:8], imm[9:6]=bits[23:20] */
    off |= ((insn >> 7) & 0x1) << 1;
    off |= ((insn >> 8) & 0xf) << 2;
    off |= ((insn >> 20) & 0xf) << 6;
    off = (off << 22) >> 22; /* sign-extend from imm[9] */

    switch (f3) {
    case 0: cond = TCG_COND_EQ;  ext = EXT_SIGN; break;
    case 1: cond = TCG_COND_NE;  ext = EXT_SIGN; break;
    case 4: cond = TCG_COND_LT;  ext = EXT_SIGN; break;
    case 5: cond = TCG_COND_GE;  ext = EXT_SIGN; break;
    case 6: cond = TCG_COND_LTU; ext = EXT_ZERO; break;
    case 7: cond = TCG_COND_GEU; ext = EXT_ZERO; break;
    default: return false;
    }

    l = gen_new_label();
    src1 = get_gpr(ctx, xlinx_rs1(insn), ext);
    orig_pc_save = ctx->pc_save;
    tcg_gen_brcondi_tl(cond, src1, cmp, l);
    gen_goto_tb(ctx, 1, ctx->cur_insn_len); /* not taken */
    ctx->pc_save = orig_pc_save;
    gen_set_label(l);                       /* taken */
    gen_goto_tb(ctx, 0, off);
    ctx->pc_save = -1;
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}

/*
 * jal16/j16, opcode 0x7b; bit7=0 -> rd=ra (jal16), bit7=1 -> rd=x0 (j16).
 * 25-bit signed PC-relative immediate (range +/-16 MiB): the standard J-type
 * scramble for imm[20:1] PLUS imm[24:21] in bits[11:8], sign at imm[24]=bit11.
 */
static bool trans_xlinx_jal(DisasContext *ctx, uint32_t insn)
{
    int rd = (insn & 0x80) ? 0 : xRA;
    int32_t imm = 0;

    imm |= ((insn >> 21) & 0x3ff) << 1;  /* imm[10:1]  = bits[30:21] */
    imm |= ((insn >> 20) & 0x1)  << 11;  /* imm[11]    = bit20       */
    imm |= ((insn >> 12) & 0xff) << 12;  /* imm[19:12] = bits[19:12] */
    imm |= ((insn >> 31) & 0x1)  << 20;  /* imm[20]    = bit31       */
    imm |= ((insn >> 8)  & 0xf)  << 21;  /* imm[24:21] = bits[11:8]  */
    imm = (imm << 7) >> 7;               /* sign-extend from imm[24] */

    gen_jal(ctx, rd, imm);
    return true;
}

/* uxtb/uxth rd' : zero-extend byte/half in place (compressed reg x8..x15) */
static bool trans_xlinx_uxt(DisasContext *ctx, uint16_t insn)
{
    int rd = 8 + ((insn >> 7) & 0x7);
    TCGv src = get_gpr(ctx, rd, EXT_NONE);
    TCGv dest = dest_gpr(ctx, rd);

    if (insn & 0x20) {
        tcg_gen_ext16u_tl(dest, src); /* uxth */
    } else {
        tcg_gen_ext8u_tl(dest, src);  /* uxtb */
    }
    gen_set_gpr(ctx, rd, dest);
    return true;
}

/*
 * push/pop/popret {ra, s0-sN}, spimm : Zcmp-style multi-register save/restore.
 * 16-bit, funct3=100/quadrant0. bits[7:4]=rlist code (1..13 => {ra,s0..s(code-2)}),
 * bits[3:0]=op (8 push, 0 pop, 4 popret), bits[12:8]=extra (stack adj += extra*16).
 */
static bool trans_xlinx_pushpop(DisasContext *ctx, uint16_t insn)
{
    int rlist_code = (insn >> 4) & 0xf;
    int op = insn & 0xf;
    int extra = (insn >> 8) & 0x1f;
    uint32_t bitmap;
    MemOp memop = MO_TEUL;
    int reg_size = 4;
    target_ulong stack_adj;
    TCGv sp, addr;
    int i;
    bool is_push = (op == 8);
    bool is_popret = (op == 4);
    bool is_pop = (op == 0);

    if (rlist_code < 1 || rlist_code > 13) {
        return false;
    }
    if (!is_push && !is_pop && !is_popret) {
        return false;
    }
    bitmap = decode_push_pop_list(ctx, rlist_code + 3); /* -> zcmp rlist 4..15 */
    if (bitmap == 0) {
        return false;
    }
    stack_adj = ROUND_UP(ctpop32(bitmap) * reg_size, 16) + extra * 16;
    sp = dest_gpr(ctx, xSP);
    addr = tcg_temp_new();

    if (is_push) {
        tcg_gen_subi_tl(addr, sp, reg_size);
        for (i = 31; i >= 0; i--) {
            if (bitmap & (1u << i)) {
                TCGv val = get_gpr(ctx, i, EXT_NONE);
                tcg_gen_qemu_st_tl(val, addr, ctx->mem_idx, memop);
                tcg_gen_subi_tl(addr, addr, reg_size);
            }
        }
        tcg_gen_subi_tl(sp, sp, stack_adj);
        gen_set_gpr(ctx, xSP, sp);
        return true;
    }

    tcg_gen_addi_tl(addr, sp, stack_adj - reg_size);
    for (i = 31; i >= 0; i--) {
        if (bitmap & (1u << i)) {
            TCGv dest = dest_gpr(ctx, i);
            tcg_gen_qemu_ld_tl(dest, addr, ctx->mem_idx, memop);
            gen_set_gpr(ctx, i, dest);
            tcg_gen_subi_tl(addr, addr, reg_size);
        }
    }
    tcg_gen_addi_tl(sp, sp, stack_adj);
    gen_set_gpr(ctx, xSP, sp);

    if (is_popret) {
        TCGv ret_addr = get_gpr(ctx, xRA, EXT_SIGN);
        tcg_gen_mov_tl(cpu_pc, ret_addr);
        tcg_gen_lookup_and_goto_ptr();
        ctx->base.is_jmp = DISAS_NORETURN;
    }
    return true;
}

/*
 * ldmia/stmia {ra,s0-s11},(base) : multi-register load/store, ascending
 * addresses, no base writeback. opcode 0x0b, bit12: 0 load / 1 store.
 * Register-presence bitmask (ascending address order below).
 */
static bool trans_xlinx_ldmstm(DisasContext *ctx, uint32_t insn)
{
    static const struct { int ibit; int reg; } map[] = {
        {  7,  1 },                         /* ra      */
        {  9,  8 }, { 10,  9 },             /* s0, s1  */
        { 21, 18 }, { 22, 19 }, { 23, 20 }, /* s2..s4  */
        { 24, 21 }, { 25, 22 }, { 26, 23 }, /* s5..s7  */
        { 27, 24 }, { 28, 25 }, { 29, 26 }, /* s8..s10 */
        { 30, 27 },                         /* s11     */
    };
    bool is_store = (insn >> 12) & 0x1;
    MemOp memop = MO_TEUL;
    TCGv addr = tcg_temp_new();
    int i, count = 0;

    tcg_gen_mov_tl(addr, get_gpr(ctx, xlinx_rs1(insn), EXT_NONE));
    for (i = 0; i < (int)ARRAY_SIZE(map); i++) {
        if (!(insn & (1u << map[i].ibit))) {
            continue;
        }
        if (is_store) {
            TCGv val = get_gpr(ctx, map[i].reg, EXT_NONE);
            tcg_gen_qemu_st_tl(val, addr, ctx->mem_idx, memop);
        } else {
            TCGv dest = dest_gpr(ctx, map[i].reg);
            tcg_gen_qemu_ld_tl(dest, addr, ctx->mem_idx, memop);
            gen_set_gpr(ctx, map[i].reg, dest);
        }
        tcg_gen_addi_tl(addr, addr, 4);
        count++;
    }
    return count != 0;
}

/* l.li rd, imm32 : 48-bit load 32-bit immediate */
static bool trans_xlinx_lli(DisasContext *ctx, int rd, uint32_t imm32)
{
    gen_set_gpri(ctx, rd, (int32_t)imm32);
    return true;
}

/*
 * Compressed byte/half load (lbu/lhu, funct3=001) and store (sb/sh, funct3=101),
 * occupying the unused C.FLD/C.FSD (D-float) compressed slots. bit1: 0 byte / 1 half.
 * rd'/rs2' = bits[4:2], rs1' = bits[9:7] (both x8..x15), unsigned, imm in bytes.
 * imm[4:0] = {bit11, bit10, bit6, bit5, bit12}.
 */
static bool trans_xlinx_cls(DisasContext *ctx, uint16_t insn, bool is_store)
{
    int rd = 8 + ((insn >> 2) & 0x7);
    int rs1 = 8 + ((insn >> 7) & 0x7);
    int imm = ((insn >> 12) & 0x1)
            | (((insn >> 5) & 0x1) << 1)
            | (((insn >> 6) & 0x1) << 2)
            | (((insn >> 10) & 0x1) << 3)
            | (((insn >> 11) & 0x1) << 4);
    bool half = (insn >> 1) & 0x1;
    MemOp memop = half ? MO_TEUW : MO_UB;
    TCGv addr = get_address(ctx, rs1, imm);

    if (is_store) {
        tcg_gen_qemu_st_tl(get_gpr(ctx, rd, EXT_NONE), addr, ctx->mem_idx, memop);
    } else {
        TCGv dest = dest_gpr(ctx, rd);
        tcg_gen_qemu_ld_tl(dest, addr, ctx->mem_idx, memop);
        gen_set_gpr(ctx, rd, dest);
    }
    return true;
}

/* ---- dispatchers ---- */

static bool decode_xlinx16(DisasContext *ctx, uint16_t insn)
{
    /* push/pop/popret: funct3=100, quadrant 0 */
    if ((insn & 0xe003) == 0x8000) {
        return trans_xlinx_pushpop(ctx, insn);
    }
    /* uxtb/uxth: 100111 rd'(9:7) 0 t(5) 000 01 ; mask out rd' and t */
    if ((insn & 0xfc5f) == 0x9c01) {
        return trans_xlinx_uxt(ctx, insn);
    }
    /* compressed lbu/lhu (funct3=001) and sb/sh (funct3=101), bit0=0 */
    if ((insn & 0xe001) == 0x2000) {
        return trans_xlinx_cls(ctx, insn, false);
    }
    if ((insn & 0xe001) == 0xa000) {
        return trans_xlinx_cls(ctx, insn, true);
    }
    return false;
}

static bool decode_xlinx32(DisasContext *ctx, uint32_t insn)
{
    switch (insn & 0x7f) {
    case 0x0b:
        /* opcode 0x0b, funct3: 0 = ldmia (load-multiple), 1 = stmia
         * (store-multiple); 2/3 = pref/prefd, cache prefetch hints with no
         * architectural effect -> NOP (the LiteOS app's .data copy loop issues
         * `prefd` ahead of its ldmia/stmia block copy). */
        if (((insn >> 12) & 0x7) >= 2) {
            return true;
        }
        return trans_xlinx_ldmstm(ctx, insn);
    case 0x1b:
        return trans_xlinx_shf(ctx, insn);
    case 0x3b:
        return trans_xlinx_branchi(ctx, insn);
    case 0x5b:
        return trans_xlinx_muliadd(ctx, insn);
    case 0x7b:
        return trans_xlinx_jal(ctx, insn);
    }
    return false;
}

/* hw0 = first halfword (opcode+rd), imm32 = following 32 bits */
static bool decode_xlinx48(DisasContext *ctx, uint16_t hw0, uint32_t imm32)
{
    /* l.li: marker 0x1f in bits[5:0], bits[15:12]==0 */
    if ((hw0 & 0x3f) == 0x1f && (hw0 & 0xf000) == 0) {
        return trans_xlinx_lli(ctx, (hw0 >> 7) & 0x1f, imm32);
    }
    return false;
}
