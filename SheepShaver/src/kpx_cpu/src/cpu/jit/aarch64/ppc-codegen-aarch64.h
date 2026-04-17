/*
 *  ppc-codegen-aarch64.h — ARM64 instruction encoding for PPC JIT
 *
 *  Reuses BasiliskII's codegen_arm64.h patterns adapted for PPC register model.
 *  This file provides the low-level ARM64 instruction emission macros.
 *
 *  Phase 2+ only — Phase 1 uses the optimized interpreter.
 */

#ifndef PPC_CODEGEN_AARCH64_H
#define PPC_CODEGEN_AARCH64_H

#include <stdint.h>

/* ---- Code emission pointer ---- */
static uint32_t *jit_code_ptr;

static inline void emit32(uint32_t insn) {
    *jit_code_ptr++ = insn;
}

/* ---- ARM64 register names ---- */
#define A64_X0   0
#define A64_X1   1
#define A64_X2   2
#define A64_X3   3
#define A64_X4   4
#define A64_X5   5
#define A64_X6   6
#define A64_X7   7
#define A64_X8   8
#define A64_X9   9
#define A64_X10  10
#define A64_X11  11
#define A64_X12  12
#define A64_X13  13
#define A64_X14  14
#define A64_X15  15
#define A64_X16  16
#define A64_X17  17
#define A64_X18  18
#define A64_X19  19
#define A64_X20  20
#define A64_X21  21
#define A64_X22  22
#define A64_X23  23
#define A64_X24  24
#define A64_X25  25
#define A64_X26  26
#define A64_X27  27
#define A64_X28  28
#define A64_FP   29
#define A64_LR   30
#define A64_SP   31
#define A64_XZR  31

/* ---- PPC → ARM64 register mapping ---- */
/* Hot PPC GPRs pinned to callee-saved ARM64 registers */
#define PPC_GPR_BASE  A64_X19   /* GPR[0-7] → x19-x26 */
#define PPC_LR_REG    A64_X27   /* PPC LR */
#define PPC_CTR_REG   A64_X28   /* PPC CTR */
#define PPC_STATE_REG A64_X20   /* CPU state struct pointer */

/* GPR[8-31] and CR/XER/FPSCR live in memory at known offsets from state ptr */

/* ---- Basic ARM64 instruction encoding ---- */

/* MOV (register) Xd = Xn */
static inline void a64_mov_reg(int rd, int rn) {
    emit32(0xAA0003E0 | (rn << 16) | rd);  /* ORR Xd, XZR, Xn */
}

/* MOV immediate (16-bit, no shift) */
static inline void a64_movz(int rd, uint16_t imm16, int shift) {
    emit32(0xD2800000 | (shift << 21) | ((uint32_t)imm16 << 5) | rd);
}

/* MOVK (keep other bits) */
static inline void a64_movk(int rd, uint16_t imm16, int shift) {
    emit32(0xF2800000 | (shift << 21) | ((uint32_t)imm16 << 5) | rd);
}

/* ADD Xd, Xn, Xm */
static inline void a64_add_reg(int rd, int rn, int rm) {
    emit32(0x8B000000 | (rm << 16) | (rn << 5) | rd);
}

/* ADDS Xd, Xn, Xm (sets flags) */
static inline void a64_adds_reg(int rd, int rn, int rm) {
    emit32(0xAB000000 | (rm << 16) | (rn << 5) | rd);
}

/* SUB Xd, Xn, Xm */
static inline void a64_sub_reg(int rd, int rn, int rm) {
    emit32(0xCB000000 | (rm << 16) | (rn << 5) | rd);
}

/* SUBS Xd, Xn, Xm (sets flags) */
static inline void a64_subs_reg(int rd, int rn, int rm) {
    emit32(0xEB000000 | (rm << 16) | (rn << 5) | rd);
}

/* AND Xd, Xn, Xm */
static inline void a64_and_reg(int rd, int rn, int rm) {
    emit32(0x8A000000 | (rm << 16) | (rn << 5) | rd);
}

/* ORR Xd, Xn, Xm */
static inline void a64_orr_reg(int rd, int rn, int rm) {
    emit32(0xAA000000 | (rm << 16) | (rn << 5) | rd);
}

/* EOR Xd, Xn, Xm */
static inline void a64_eor_reg(int rd, int rn, int rm) {
    emit32(0xCA000000 | (rm << 16) | (rn << 5) | rd);
}

/* LDR Xt, [Xn, #imm12] (unsigned offset, scaled by 8 for 64-bit) */
static inline void a64_ldr_imm(int rt, int rn, uint32_t offset) {
    emit32(0xF9400000 | ((offset / 8) << 10) | (rn << 5) | rt);
}

/* STR Xt, [Xn, #imm12] */
static inline void a64_str_imm(int rt, int rn, uint32_t offset) {
    emit32(0xF9000000 | ((offset / 8) << 10) | (rn << 5) | rt);
}

/* LDR Wt, [Xn, #imm12] (32-bit load, unsigned offset scaled by 4) */
static inline void a64_ldr_w_imm(int rt, int rn, uint32_t offset) {
    emit32(0xB9400000 | ((offset / 4) << 10) | (rn << 5) | rt);
}

/* STR Wt, [Xn, #imm12] (32-bit store) */
static inline void a64_str_w_imm(int rt, int rn, uint32_t offset) {
    emit32(0xB9000000 | ((offset / 4) << 10) | (rn << 5) | rt);
}

/* B (unconditional branch, PC-relative) */
static inline void a64_b(int32_t offset) {
    emit32(0x14000000 | ((offset >> 2) & 0x03FFFFFF));
}

/* B.cond (conditional branch, PC-relative) */
#define A64_CC_EQ  0x0
#define A64_CC_NE  0x1
#define A64_CC_CS  0x2
#define A64_CC_CC  0x3
#define A64_CC_MI  0x4
#define A64_CC_PL  0x5
#define A64_CC_VS  0x6
#define A64_CC_VC  0x7
#define A64_CC_HI  0x8
#define A64_CC_LS  0x9
#define A64_CC_GE  0xA
#define A64_CC_LT  0xB
#define A64_CC_GT  0xC
#define A64_CC_LE  0xD
#define A64_CC_AL  0xE

static inline void a64_b_cond(int cond, int32_t offset) {
    emit32(0x54000000 | (((offset >> 2) & 0x7FFFF) << 5) | cond);
}

/* BLR Xn (branch with link to register) */
static inline void a64_blr(int rn) {
    emit32(0xD63F0000 | (rn << 5));
}

/* BR Xn (branch to register) */
static inline void a64_br(int rn) {
    emit32(0xD61F0000 | (rn << 5));
}

/* RET (branch to x30) */
static inline void a64_ret(void) {
    emit32(0xD65F03C0);
}

/* NOP */
static inline void a64_nop(void) {
    emit32(0xD503201F);
}

/* STP Xt1, Xt2, [Xn, #imm7]! (pre-index, for push) */
static inline void a64_stp_pre(int rt1, int rt2, int rn, int imm7) {
    emit32(0xA9800000 | (((imm7 / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

/* LDP Xt1, Xt2, [Xn], #imm7 (post-index, for pop) */
static inline void a64_ldp_post(int rt1, int rt2, int rn, int imm7) {
    emit32(0xA8C00000 | (((imm7 / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt1);
}

/* IC IVAU, Xt (instruction cache invalidate by VA to PoU) */
static inline void a64_ic_ivau(int rt) {
    emit32(0xD50B7520 | rt);
}

/* DC CVAU, Xt (data cache clean by VA to PoU) */
static inline void a64_dc_cvau(int rt) {
    emit32(0xD50B7B20 | rt);
}

/* DSB ISH */
static inline void a64_dsb_ish(void) {
    emit32(0xD5033B9F);
}

/* ISB */
static inline void a64_isb(void) {
    emit32(0xD5033FDF);
}

#endif /* PPC_CODEGEN_AARCH64_H */
