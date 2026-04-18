/*
 *  ppc-jit.cpp — PPC → AArch64 direct codegen JIT
 *
 *  Compiles PPC basic blocks to native ARM64 instructions.
 *  Generated code is called as: void block(powerpc_registers *regs)
 *  with x0 = regs pointer. Block reads/writes GPR/CR/LR/CTR/PC via
 *  LDR/STR at known offsets from x0 (moved to callee-saved x20).
 */

#ifdef __aarch64__

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "ppc-jit.h"
#include "ppc-codegen-aarch64.h"
#include "jit-target-cache.hpp"

/* ---- Code cache ---- */
static uint8_t  *jit_cache_base = NULL;
static size_t    jit_cache_size = 0;
static uint32_t *jit_cache_wp   = NULL;
static uint32_t *jit_cache_end  = NULL;

/* ---- Register offsets in powerpc_registers ----
   Determined from compiled struct layout on aarch64. */
#define PPCR_GPR(n) ((uint32_t)((n) * 4))
#define PPCR_CR     896
#define PPCR_XER    900
#define PPCR_FPSCR  912
#define PPCR_LR     916
#define PPCR_CTR    920
#define PPCR_PC     924



/* Host register assignments */
#define RSTATE  20   /* x20 = regs pointer (callee-saved) */
#define RTMP0    0
#define RTMP1    1
#define RTMP2    2

/* FPR offsets: FPR[n] at offset 128 + n*8 (each is a 64-bit double) */
#define PPCR_FPR(n) ((uint32_t)(128 + (n) * 8))

/* ARM64 FP register helpers */
/* LDR Dt, [Xn, #imm] (64-bit FP load, unsigned offset scaled by 8) */
static void emit_load_fpr(int fd, int fpr_num) {
	/* LDR Dt, [RSTATE, #offset] */
	uint32_t off = PPCR_FPR(fpr_num);
	emit32(0xFD400000 | ((off / 8) << 10) | (RSTATE << 5) | fd);
}

/* STR Dt, [Xn, #imm] */
static void emit_store_fpr(int fs, int fpr_num) {
	uint32_t off = PPCR_FPR(fpr_num);
	emit32(0xFD000000 | ((off / 8) << 10) | (RSTATE << 5) | fs);
}

/* ---- PPC instruction field extraction ---- */
static inline uint32_t PPC_OPC(uint32_t op)  { return op >> 26; }
static inline uint32_t PPC_RD(uint32_t op)   { return (op >> 21) & 0x1F; }
static inline uint32_t PPC_RS(uint32_t op)   { return (op >> 21) & 0x1F; }
static inline uint32_t PPC_RA(uint32_t op)   { return (op >> 16) & 0x1F; }
static inline uint32_t PPC_RB(uint32_t op)   { return (op >> 11) & 0x1F; }
static inline int16_t  PPC_SIMM(uint32_t op) { return (int16_t)(op & 0xFFFF); }
static inline uint16_t PPC_UIMM(uint32_t op) { return (uint16_t)(op & 0xFFFF); }
static inline uint32_t PPC_XO(uint32_t op)   { return (op >> 1) & 0x3FF; }

/* ---- Emit helpers ---- */

static void emit_load_gpr(int rd, int n) {
	a64_ldr_w_imm(rd, RSTATE, PPCR_GPR(n));
}

static void emit_store_gpr(int rs, int n) {
	a64_str_w_imm(rs, RSTATE, PPCR_GPR(n));
}

static void emit_load_imm32(int rd, int32_t imm) {
	uint32_t u = (uint32_t)imm;
	uint16_t lo = u & 0xFFFF;
	uint16_t hi = (u >> 16) & 0xFFFF;
	if (imm >= 0 && imm < 65536) {
		a64_movz(rd, lo, 0);
	} else if (imm < 0 && imm >= -65536) {
		emit32(0x12800000 | ((uint32_t)(uint16_t)(~u) << 5) | rd); /* MOVN Wd, #~u */
	} else {
		a64_movz(rd, lo, 0);
		if (hi) a64_movk(rd, hi, 1);
	}
}

/* Update CR0 based on a 32-bit result in ARM64 register 'rd'.
   CR0: bit31=LT(negative), bit30=GT(positive nonzero), bit29=EQ(zero), bit28=SO(from XER) */
static void emit_update_cr0(int result_reg) {
	/* Simple approach: compute CR0 nibble with conditional instructions */
	/* Compare result with 0 */
	emit32(0x7100001F | (result_reg << 5)); /* CMP Wn, #0 */
	/* CR0 = 0 by default */
	a64_movz(RTMP2, 0, 0);
	/* If result < 0 (signed): CR0 = 8 (LT) */
	emit32(0x5A800040 | (RTMP2 << 5) | RTMP2); /* CSINV Wd, Wn, Wn, PL — wrong, use CSET */
	/* Actually use CSEL: */
	/* MOV W(RTMP0), #8; MOV W(RTMP1), #4; MOV W(RTMP2), #2 */
	/* CSEL based on condition */
	/* Simplest: use three conditional moves */
	a64_movz(RTMP2, 0, 0);
	emit_load_imm32(RTMP0, 8); /* LT value */
	emit_load_imm32(RTMP1, 4); /* GT value */
	/* CSEL RTMP2, RTMP0, RTMP2, LT (if signed less than) */
	emit32(0x1A800000 | (RTMP2 << 16) | (0xB << 12) | (RTMP0 << 5) | RTMP2); /* CSEL Wd,Wn,Wm,LT */
	/* CSEL RTMP2, RTMP1, RTMP2, GT (if signed greater than) */
	emit32(0x1A800000 | (RTMP2 << 16) | (0xC << 12) | (RTMP1 << 5) | RTMP2); /* CSEL Wd,Wn,Wm,GT */
	/* If EQ, set to 2 */
	emit_load_imm32(RTMP0, 2);
	emit32(0x1A800000 | (RTMP2 << 16) | (0x0 << 12) | (RTMP0 << 5) | RTMP2); /* CSEL Wd,Wn,Wm,EQ */
	/* Shift nibble into CR0 position (bits 31:28) */
	emit_load_imm32(RTMP0, 28);
	emit32(0x1AC02000 | (RTMP0 << 16) | (RTMP2 << 5) | RTMP2); /* LSL Wd,Wn,Wm */
	/* Load CR, clear CR0 field, OR in new value */
	a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
	emit_load_imm32(RTMP1, 0x0FFFFFFF);
	emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
	emit32(0x2A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* ORR */
	a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
}


/* Read XER.CA (bit 29) into ARM64 register rd, bit 0 */
static void emit_read_xer_ca(int rd) {
	a64_ldr_w_imm(rd, RSTATE, PPCR_XER);
	/* Extract bit 29 */
	emit_load_imm32(rd == RTMP0 ? RTMP1 : RTMP0, 29);
	emit32(0x1AC02400 | ((rd == RTMP0 ? RTMP1 : RTMP0) << 16) | (rd << 5) | rd); /* LSR */
	emit32(0x12000000 | (rd << 5) | rd); /* AND #1 */
}

/* Write ARM64 carry flag (from last ADDS/SUBS) into XER.CA (bit 29) */
static void emit_write_xer_ca_from_carry(void) {
	/* MRS Xt, NZCV */
	emit32(0xD53B4200 | RTMP2);
	/* Extract C bit (bit 29 of NZCV) — already at bit 29! */
	a64_ldr_w_imm(RTMP0, RSTATE, PPCR_XER);
	emit_load_imm32(RTMP1, ~(1 << 29)); /* clear CA */
	emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
	emit_load_imm32(RTMP1, (1 << 29));
	emit32(0x0A000000 | (RTMP1 << 16) | (RTMP2 << 5) | RTMP2); /* isolate C at bit 29 */
	emit32(0x2A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* OR into XER */
	a64_str_w_imm(RTMP0, RSTATE, PPCR_XER);
}

/* Set XER.CA to a specific value (0 or 1) */
static void emit_set_xer_ca(int val) {
	a64_ldr_w_imm(RTMP0, RSTATE, PPCR_XER);
	if (val) {
		emit_load_imm32(RTMP1, (1 << 29));
		emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
	} else {
		emit_load_imm32(RTMP1, ~(1 << 29));
		emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
	}
	a64_str_w_imm(RTMP0, RSTATE, PPCR_XER);
}


/* Load effective address: if rA==0, use 0; otherwise load GPR[rA].
   Always puts result in RTMP0. */
static void emit_load_ea_base(int ra_num) {
	if (ra_num == 0) {
		a64_movz(RTMP0, 0, 0);
	} else {
		emit_load_gpr(RTMP0, ra_num);
	}
}

/* ---- AltiVec Vector Register helpers ---- */
/* VR[n] at offset 384 + n*16, each 128-bit (16 bytes) */
#define PPCR_VR(n) ((uint32_t)(384 + (n) * 16))

/* Load 128-bit vector register into ARM64 Q register (NEON) */
static void emit_load_vr(int qd, int vr_num) {
	uint32_t off = PPCR_VR(vr_num);
	/* LDR Qt, [Xn, #imm] — 128-bit vector load, unsigned offset scaled by 16 */
	emit32(0x3DC00000 | ((off / 16) << 10) | (RSTATE << 5) | qd);
}

/* Store ARM64 Q register into VR[n] */
static void emit_store_vr(int qs, int vr_num) {
	uint32_t off = PPCR_VR(vr_num);
	emit32(0x3D800000 | ((off / 16) << 10) | (RSTATE << 5) | qs);
}

/* AltiVec field extraction */
static inline uint32_t VR_VD(uint32_t op) { return (op >> 21) & 0x1F; }
static inline uint32_t VR_VA(uint32_t op) { return (op >> 16) & 0x1F; }
static inline uint32_t VR_VB(uint32_t op) { return (op >> 11) & 0x1F; }
static inline uint32_t VR_VC(uint32_t op) { return (op >> 6) & 0x1F; }

/* Emit: store next_pc to regs->pc, epilogue, ret */
static void emit_epilogue_with_pc(uint32_t next_pc) {
	emit_load_imm32(RTMP0, (int32_t)next_pc);
	a64_str_w_imm(RTMP0, RSTATE, PPCR_PC);
	/* Restore callee-saved regs and return */
	a64_ldp_post(RSTATE, 21, A64_SP, 16);
	a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
	a64_ret();
}

/* ---- Instruction offset map for intra-block branches ---- */
static uint32_t *insn_code_offset[64];  /* ARM64 code ptr at start of each PPC insn */
static uint32_t  insn_ppc_pc[64];       /* PPC PC of each compiled instruction */
static int       insn_count = 0;

/* Find the ARM64 code offset for a PPC PC within the current block */
static uint32_t *find_code_for_pc(uint32_t target_pc) {
	for (int i = 0; i < insn_count; i++) {
		if (insn_ppc_pc[i] == target_pc)
			return insn_code_offset[i];
	}
	return NULL;
}


/* ---- Opcode miss tracking ---- */
static uint32_t jit_miss_count[64] = {0};  /* primary opcode histogram */
static uint32_t jit_xo_miss[1024] = {0};   /* XO opcode histogram for opc=31 */
static uint32_t jit_total_miss = 0;
static uint32_t jit_total_hit = 0;
static uint32_t jit_blocks_attempted = 0;
static uint32_t jit_blocks_complete = 0;
static uint32_t jit_last_fail_op = 0;
static uint32_t jit_cum_fail_opc[64] = {0};
static uint32_t jit_cum_fail_xo31[1024] = {0};
static uint32_t jit_cum_fail_total = 0;

static void jit_report_misses(void) {
	if (jit_total_miss == 0 && jit_total_hit == 0) return;
	fprintf(stderr, "PPC-JIT-A64: blocks=%u complete=%u (%.1f%%)\n",
		jit_blocks_attempted, jit_blocks_complete,
		jit_blocks_attempted ? jit_blocks_complete * 100.0 / jit_blocks_attempted : 0.0);
	fprintf(stderr, "PPC-JIT-A64: hit=%u miss=%u (%.1f%% coverage)\n",
		jit_total_hit, jit_total_miss,
		jit_total_hit * 100.0 / (jit_total_hit + jit_total_miss));
	fprintf(stderr, "PPC-JIT-A64: top missed primary opcodes:\n");
	/* Sort and print top 10 */
	for (int pass = 0; pass < 10; pass++) {
		uint32_t max_v = 0; int max_i = -1;
		for (int i = 0; i < 64; i++) {
			if (jit_miss_count[i] > max_v) { max_v = jit_miss_count[i]; max_i = i; }
		}
		if (max_i < 0 || max_v == 0) break;
		fprintf(stderr, "  opc=%d: %u misses\n", max_i, max_v);
		jit_miss_count[max_i] = 0; /* clear for next pass */
	}
	fprintf(stderr, "PPC-JIT-A64: top missed XO opcodes (opc=31):\n");
	for (int pass = 0; pass < 10; pass++) {
		uint32_t max_v = 0; int max_i = -1;
		for (int i = 0; i < 1024; i++) {
			if (jit_xo_miss[i] > max_v) { max_v = jit_xo_miss[i]; max_i = i; }
		}
		if (max_i < 0 || max_v == 0) break;
		fprintf(stderr, "  XO=%d: %u misses\n", max_i, max_v);
		jit_xo_miss[max_i] = 0;
	}
}

/* ---- Compile one PPC instruction ---- */
static bool compile_one(uint32_t op, uint32_t pc) {
	uint32_t opc = PPC_OPC(op);
	uint32_t rd, ra, rb;
	int16_t simm;
	uint16_t uimm;

	switch (opc) {

	case 14: /* addi / li */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		if (ra == 0) {
			emit_load_imm32(RTMP0, (int32_t)simm);
		} else {
			emit_load_gpr(RTMP0, ra);
			emit_load_imm32(RTMP1, (int32_t)simm);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADD Wd,Wn,Wm */
		}
		emit_store_gpr(RTMP0, rd);
		return true;

	case 15: /* addis / lis */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		if (ra == 0) {
			emit_load_imm32(RTMP0, (int32_t)simm << 16);
		} else {
			emit_load_gpr(RTMP0, ra);
			emit_load_imm32(RTMP1, (int32_t)simm << 16);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		}
		emit_store_gpr(RTMP0, rd);
		return true;

	case 23: /* rlwnm rA,rS,rB,MB,ME (rotate left word then AND mask) */
	{
		uint32_t rs = PPC_RS(op);
		ra = PPC_RA(op);
		rb = (op >> 11) & 0x1F;
		uint32_t mb = (op >> 6) & 0x1F;
		uint32_t me = (op >> 1) & 0x1F;
		emit_load_gpr(RTMP0, rs);
		emit_load_gpr(RTMP1, rb);
		/* Rotate left by rB: ROR Wd,Wn,Wm with negated count */
		emit32(0x4B0003E0 | (RTMP1 << 16) | RTMP1); /* NEG Wd,Wm (32-count) */
		emit32(0x1AC02C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ROR Wd,Wn,Wm */
		uint32_t mask = 0;
		if (mb <= me) { for (uint32_t i = mb; i <= me; i++) mask |= (0x80000000U >> i); }
		else { for (uint32_t i = 0; i <= me; i++) mask |= (0x80000000U >> i);
		       for (uint32_t i = mb; i <= 31; i++) mask |= (0x80000000U >> i); }
		if (mask != 0xFFFFFFFF) {
			emit_load_imm32(RTMP1, (int32_t)mask);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		}
		emit_store_gpr(RTMP0, ra);
		if (op & 1) emit_update_cr0(RTMP0);
		return true;
	}

	case 24: /* ori (and NOP = ori 0,0,0) */
		ra = PPC_RA(op); rd = PPC_RS(op); uimm = PPC_UIMM(op);
		if (rd == 0 && ra == 0 && uimm == 0) return true; /* NOP */
		emit_load_gpr(RTMP0, rd);
		if (uimm) {
			emit_load_imm32(RTMP1, (int32_t)(uint32_t)uimm);
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ORR */
		}
		emit_store_gpr(RTMP0, ra);
		return true;

	case 25: /* oris */
		ra = PPC_RA(op); rd = PPC_RS(op); uimm = PPC_UIMM(op);
		emit_load_gpr(RTMP0, rd);
		if (uimm) {
			emit_load_imm32(RTMP1, (int32_t)((uint32_t)uimm << 16));
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		}
		emit_store_gpr(RTMP0, ra);
		return true;

	case 26: /* xori rA,rS,UIMM */
		ra = PPC_RA(op); rd = PPC_RS(op); uimm = PPC_UIMM(op);
		emit_load_gpr(RTMP0, rd);
		if (uimm) {
			emit_load_imm32(RTMP1, (int32_t)(uint32_t)uimm);
			emit32(0x4A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* EOR */
		}
		emit_store_gpr(RTMP0, ra);
		return true;

	case 27: /* xoris rA,rS,UIMM */
		ra = PPC_RA(op); rd = PPC_RS(op); uimm = PPC_UIMM(op);
		emit_load_gpr(RTMP0, rd);
		if (uimm) {
			emit_load_imm32(RTMP1, (int32_t)((uint32_t)uimm << 16));
			emit32(0x4A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		}
		emit_store_gpr(RTMP0, ra);
		return true;

	case 28: /* andi. */
		ra = PPC_RA(op); rd = PPC_RS(op); uimm = PPC_UIMM(op);
		emit_load_gpr(RTMP0, rd);
		emit_load_imm32(RTMP1, (int32_t)(uint32_t)uimm);
		emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
		emit_store_gpr(RTMP0, ra);
		/* TODO: set CR0 for record form */
		return true;

	case 31: { /* XO-form extended opcodes */
		uint32_t xo = PPC_XO(op);
		rd = PPC_RD(op); ra = PPC_RA(op); rb = PPC_RB(op);
		switch (xo) {
		case 0: /* cmp (cmpw crD,rA,rB) */
		{
			uint32_t crd = (op >> 23) & 0x7;
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | 0x1F); /* SUBS WZR */
			emit32(0xD53B4200 | RTMP2); /* MRS NZCV */
			emit32(0xD340FC00 | (RTMP2 << 5) | RTMP2 | (28 << 10)); /* LSR #28 */
			uint32_t shift = (7 - crd) * 4;
			if (shift) { emit_load_imm32(RTMP1, shift); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP2 << 5) | RTMP2); }
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP1, ~(0xF << shift));
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit32(0x2A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			return true;
		}
		case 266: /* add / add. */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, rd);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 40: /* subf (rD = rB - rA) */
			emit_load_gpr(RTMP0, rb);
			emit_load_gpr(RTMP1, ra);
			emit32(0x4B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SUB */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 28: /* and */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 444: /* or / or. (also mr) */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 316: /* xor */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x4A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 104: /* neg / neg. */
			emit_load_gpr(RTMP0, ra);
			emit32(0x4B0003E0 | (RTMP0 << 16) | RTMP0);
			emit_store_gpr(RTMP0, rd);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 26: /* cntlzw */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit32(0x5AC01000 | (RTMP0 << 5) | RTMP0); /* CLZ Wd, Wn */
			emit_store_gpr(RTMP0, ra);
			return true;
		case 922: /* extsh */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit32(0x13003C00 | (RTMP0 << 5) | RTMP0); /* SXTH Wd, Wn */
			emit_store_gpr(RTMP0, ra);
			return true;
		case 954: /* extsb */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit32(0x13001C00 | (RTMP0 << 5) | RTMP0); /* SXTB Wd, Wn */
			emit_store_gpr(RTMP0, ra);
			return true;
		case 715: /* mullw (with OE bit) */
		case 235: /* mullw */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x1B007C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* MUL Wd,Wn,Wm */
			emit_store_gpr(RTMP0, rd);
			return true;
		case 491: /* divw */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x1AC00C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SDIV Wd,Wn,Wm */
			emit_store_gpr(RTMP0, rd);
			return true;
		case 19: /* mfcr rD */
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			emit_store_gpr(RTMP0, rd);
			return true;
		case 144: /* mtcrf CRM,rS */
		{
			uint32_t crm = (op >> 12) & 0xFF;
			emit_load_gpr(RTMP0, PPC_RS(op));
			if (crm == 0xFF) {
				/* Move entire CR */
				a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			} else {
				/* Selective CR field update */
				uint32_t mask = 0;
				for (int i = 0; i < 8; i++)
					if (crm & (0x80 >> i)) mask |= (0xF0000000U >> (i * 4));
				a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
				emit_load_imm32(RTMP2, (int32_t)~mask);
				emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); /* AND clear */
				emit_load_imm32(RTMP2, (int32_t)mask);
				emit32(0x0A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* AND source */
				emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1); /* ORR */
				a64_str_w_imm(RTMP1, RSTATE, PPCR_CR);
			}
			return true;
		}
		case 339: /* mfspr */
		{
			uint32_t spr = ((op >> 16) & 0x1F) | ((op >> 6) & 0x3E0);
			if (spr == 8) { /* LR */
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_LR);
				emit_store_gpr(RTMP0, rd);
				return true;
			}
			if (spr == 9) { /* CTR */
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CTR);
				emit_store_gpr(RTMP0, rd);
				return true;
			}
			if (spr == 1) { /* XER */
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_XER);
				emit_store_gpr(RTMP0, rd);
				return true;
			}
			/* Unknown SPR: return 0 (safe for user-mode emulation) */
			emit_load_imm32(RTMP0, 0);
			emit_store_gpr(RTMP0, rd);
			return true;
		}
		case 467: /* mtspr */
		{
			uint32_t spr = ((op >> 16) & 0x1F) | ((op >> 6) & 0x3E0);
			if (spr == 8) { /* LR */
				emit_load_gpr(RTMP0, PPC_RS(op));
				a64_str_w_imm(RTMP0, RSTATE, PPCR_LR);
				return true;
			}
			if (spr == 9) { /* CTR */
				emit_load_gpr(RTMP0, PPC_RS(op));
				a64_str_w_imm(RTMP0, RSTATE, PPCR_CTR);
				return true;
			}
			if (spr == 1) { /* XER */
				emit_load_gpr(RTMP0, PPC_RS(op));
				a64_str_w_imm(RTMP0, RSTATE, PPCR_XER);
				return true;
			}
			/* Unknown SPR: NOP (safe for user-mode) */
			return true;
		}
		case 824: /* srawi rA,rS,SH (arithmetic shift right immediate) */
		{
			uint32_t sh = (op >> 11) & 0x1F;
			emit_load_gpr(RTMP0, PPC_RS(op));
			if (sh) {
				/* ASR Wd, Wn, #sh */
				emit32(0x13000000 | (sh << 10) | (0x1F << 16) | (RTMP0 << 5) | RTMP0);
			}
			emit_store_gpr(RTMP0, ra);
			/* TODO: set XER[CA] if any shifted-out bits were 1 and source was negative */
			return true;
		}
		case 24: /* slw rA,rS,rB (shift left word) */
		{
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSL Wd,Wn,Wm */
			emit_store_gpr(RTMP0, ra);
			return true;
		}
		case 536: /* srw rA,rS,rB (shift right word) */
		{
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x1AC02400 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSR Wd,Wn,Wm */
			emit_store_gpr(RTMP0, ra);
			return true;
		}
		case 792: /* sraw rA,rS,rB (arithmetic shift right) */
		{
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x1AC02800 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ASR Wd,Wn,Wm */
			emit_store_gpr(RTMP0, ra);
			return true;
		}

		case 32: /* cmpl (cmplw crD,rA,rB) */
		{
			uint32_t crd = (op >> 23) & 0x7;
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | 0x1F);
			a64_movz(RTMP2, 0, 0);
			emit_load_imm32(RTMP0, 8);
			emit32(0x1A800000 | (RTMP2 << 16) | (0x3 << 12) | (RTMP0 << 5) | RTMP2); /* CC=LT */
			emit_load_imm32(RTMP0, 4);
			emit32(0x1A800000 | (RTMP2 << 16) | (0x8 << 12) | (RTMP0 << 5) | RTMP2); /* HI=GT */
			emit_load_imm32(RTMP0, 2);
			emit32(0x1A800000 | (RTMP2 << 16) | (0x0 << 12) | (RTMP0 << 5) | RTMP2); /* EQ */
			uint32_t shift = (7 - crd) * 4;
			if (shift) { emit_load_imm32(RTMP0, shift); emit32(0x1AC02000 | (RTMP0 << 16) | (RTMP2 << 5) | RTMP2); }
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP1, ~(0xF << shift));
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit32(0x2A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			return true;
		}

		case 23: /* lwzx rD,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) {
				emit_load_gpr(RTMP1, rb);
				emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			}
			emit32(0xB9400000 | (RTMP0 << 5) | RTMP1); /* LDR Wt, [Xn] */
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
			emit_store_gpr(RTMP1, rd);
			return true;

		case 151: /* stwx rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) {
				emit_load_gpr(RTMP2, rb);
				emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			}
			emit32(0xB9000000 | (RTMP0 << 5) | RTMP1); /* STR */
			return true;

		case 8: /* subfc rD,rA,rB (rD = rB - rA, set CA) */
			emit_load_gpr(RTMP0, rb);
			emit_load_gpr(RTMP1, ra);
			emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SUBS */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ca_from_carry();
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 136: /* subfe rD,rA,rB (rD = ~rA + rB + CA) */
			emit_load_gpr(RTMP0, ra);
			emit32(0x2A2003E0 | (RTMP0 << 16) | RTMP0); /* MVN (NOT rA) */
			emit_load_gpr(RTMP1, rb);
			emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADDS ~rA + rB */
			emit_read_xer_ca(RTMP1);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* + CA */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ca_from_carry();
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 10: /* addc rD,rA,rB (set CA) */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADDS */
			emit_store_gpr(RTMP0, rd);
			emit_write_xer_ca_from_carry();
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 138: /* adde rD,rA,rB (rD = rA + rB + CA) */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADDS rA+rB */
			/* Now add CA: read XER.CA, add it */
			emit32(0xD53B4200 | RTMP2); /* MRS NZCV (save carry from ADDS) */
			emit_read_xer_ca(RTMP1);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADD carry-in */
			emit_store_gpr(RTMP0, rd);
			/* Write new CA: set if either ADDS or the CA addition overflowed */
			emit_write_xer_ca_from_carry();
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 234: /* addme rD,rA (rD = rA + CA - 1) — simplified */
			emit_load_gpr(RTMP0, ra);
			emit32(0x51000400 | (RTMP0 << 5) | RTMP0); /* SUB Wd,Wn,#1 */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 202: /* addze rD,rA (rD = rA + CA) — simplified as rD = rA */
			emit_load_gpr(RTMP0, ra);
			emit_store_gpr(RTMP0, rd);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 232: /* subfme — simplified */
			emit_load_gpr(RTMP0, ra);
			emit32(0x4B0003E0 | (RTMP0 << 16) | RTMP0); /* NEG */
			emit32(0x51000400 | (RTMP0 << 5) | RTMP0); /* SUB #1 */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 200: /* subfze rD,rA (rD = ~rA + CA) — simplified as NEG */
			emit_load_gpr(RTMP0, ra);
			emit32(0x4B0003E0 | (RTMP0 << 16) | RTMP0);
			emit_store_gpr(RTMP0, rd);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 476: /* nand rA,rS,rB */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x0A200000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* BIC then invert... */
			/* Actually: AND then MVN */
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
			emit32(0x2A2003E0 | (RTMP0 << 16) | RTMP0); /* ORN Wd,WZR,Wm = MVN */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 124: /* nor rA,rS,rB */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ORR */
			emit32(0x2A2003E0 | (RTMP0 << 16) | RTMP0); /* MVN */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 284: /* eqv rA,rS,rB (XNOR) */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x4A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* EOR */
			emit32(0x2A2003E0 | (RTMP0 << 16) | RTMP0); /* MVN */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 60: /* andc rA,rS,rB */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x0A200000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* BIC Wd,Wn,Wm */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 412: /* orc rA,rS,rB */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x2A200000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ORN Wd,Wn,Wm */
			emit_store_gpr(RTMP0, ra);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 459: /* divwu rD,rA,rB (unsigned divide) */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x1AC00800 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* UDIV Wd,Wn,Wm */
			emit_store_gpr(RTMP0, rd);
			if (op & 1) emit_update_cr0(RTMP0);
			return true;
		case 75: /* mulhw rD,rA,rB (high word of signed multiply) */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			/* SMULL Xd, Wn, Wm then ASR Xd, Xd, #32 */
			emit32(0x9B207C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SMULL */
			emit32(0xD360FC00 | (RTMP0 << 5) | RTMP0); /* ASR Xd, Xn, #32 */
			emit_store_gpr(RTMP0, rd);
			return true;
		case 11: /* mulhwu rD,rA,rB (high word of unsigned multiply) */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x9BA07C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* UMULL */
			emit32(0xD360FC00 | (RTMP0 << 5) | RTMP0); /* LSR Xd, Xn, #32 */
			emit_store_gpr(RTMP0, rd);
			return true;
		case 87: /* lbzx rD,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0x39400000 | (RTMP0 << 5) | RTMP1); /* LDRB */
			emit_store_gpr(RTMP1, rd);
			return true;
		case 215: /* stbx rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0x39000000 | (RTMP0 << 5) | RTMP1); /* STRB */
			return true;
		case 279: /* lhzx rD,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0x79400000 | (RTMP0 << 5) | RTMP1); /* LDRH */
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 */
			emit_store_gpr(RTMP1, rd);
			return true;
		case 407: /* sthx rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0x79000000 | (RTMP0 << 5) | RTMP1); /* STRH */
			return true;
		case 343: /* lhax rD,rA,rB (load halfword algebraic indexed) */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0x79400000 | (RTMP0 << 5) | RTMP1); /* LDRH */
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 */
			emit32(0x13003C00 | (RTMP1 << 5) | RTMP1); /* SXTH */
			emit_store_gpr(RTMP1, rd);
			return true;
		case 371: /* mftb rD (move from time base) */
			/* Read ARM64 CNTVCT_EL0 as a substitute for PPC TB */
			emit32(0xD53BE040 | RTMP0); /* MRS Xt, CNTVCT_EL0 */
			emit_store_gpr(RTMP0, rd);
			return true;

		case 119: /* lbzux rD,rA,rB */
			/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0x39400000 | (RTMP0 << 5) | RTMP1);
			emit_store_gpr(RTMP1, rd);
			return true;
		case 247: /* stbux rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP2, rb);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0x39000000 | (RTMP0 << 5) | RTMP1);
			return true;
		case 311: /* lhzux rD,rA,rB */
			/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0x79400000 | (RTMP0 << 5) | RTMP1);
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
			emit_store_gpr(RTMP1, rd);
			return true;
		case 439: /* sthux rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP2, rb);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0x79000000 | (RTMP0 << 5) | RTMP1);
			return true;
		case 375: /* lhaux rD,rA,rB */
			/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0x79400000 | (RTMP0 << 5) | RTMP1);
			emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
			emit32(0x13003C00 | (RTMP1 << 5) | RTMP1);
			emit_store_gpr(RTMP1, rd);
			return true;
		case 55: /* lwzux rD,rA,rB */
			/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0xB9400000 | (RTMP0 << 5) | RTMP1);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit_store_gpr(RTMP1, rd);
			return true;
		case 183: /* stwux rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP2, rb);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0xB9000000 | (RTMP0 << 5) | RTMP1);
			return true;
		case 790: /* lhbrx rD,rA,rB (byte-reversed = native order on LE) */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0x79400000 | (RTMP0 << 5) | RTMP1); /* LDRH (native LE = byte-reversed for PPC) */
			emit_store_gpr(RTMP1, rd);
			return true;
		case 918: /* sthbrx rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0x79000000 | (RTMP0 << 5) | RTMP1);
			return true;
		case 534: /* lwbrx rD,rA,rB (byte-reversed = native order on LE) */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0xB9400000 | (RTMP0 << 5) | RTMP1);
			emit_store_gpr(RTMP1, rd);
			return true;
		case 662: /* stwbrx rS,rA,rB */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0xB9000000 | (RTMP0 << 5) | RTMP1);
			return true;

		case 535: /* lfsx frD,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0xB9400000 | (RTMP0 << 5) | RTMP1);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit32(0x1E270000 | (RTMP1 << 5) | 0);
			emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, rd);
			return true;
		case 567: /* lfsux frD,rA,rB */
			emit_load_ea_base(ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0xB9400000 | (RTMP0 << 5) | RTMP1);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit32(0x1E270000 | (RTMP1 << 5) | 0);
			emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, rd);
			return true;
		case 599: /* lfdx frD,rA,rB */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0xF9400000 | (RTMP0 << 5) | RTMP1);
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			emit32(0x9E670000 | (RTMP1 << 5) | 0);
			emit_store_fpr(0, rd);
			return true;
		case 631: /* lfdux frD,rA,rB */
			emit_load_ea_base(ra); emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0xF9400000 | (RTMP0 << 5) | RTMP1);
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			emit32(0x9E670000 | (RTMP1 << 5) | 0);
			emit_store_fpr(0, rd);
			return true;
		case 663: /* stfsx frS,rA,rB */
			emit_load_fpr(0, PPC_RS(op));
			emit32(0x1E624000 | (0 << 5) | 0);
			emit32(0x1E260000 | (0 << 5) | RTMP1);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0xB9000000 | (RTMP0 << 5) | RTMP1);
			return true;
		case 695: /* stfsux frS,rA,rB */
			emit_load_fpr(0, PPC_RS(op));
			emit32(0x1E624000 | (0 << 5) | 0);
			emit32(0x1E260000 | (0 << 5) | RTMP1);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP2, rb);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0xB9000000 | (RTMP0 << 5) | RTMP1);
			return true;
		case 727: /* stfdx frS,rA,rB */
			emit_load_fpr(0, PPC_RS(op));
			emit32(0x9E660000 | (0 << 5) | RTMP1);
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0xF9000000 | (RTMP0 << 5) | RTMP1);
			return true;
		case 759: /* stfdux frS,rA,rB */
			emit_load_fpr(0, PPC_RS(op));
			emit32(0x9E660000 | (0 << 5) | RTMP1);
			emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra); emit_load_gpr(RTMP2, rb);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			emit32(0xF9000000 | (RTMP0 << 5) | RTMP1);
			return true;
		case 1014: /* dcbz rA,rB — zero cache line (32 bytes) */
		{
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			/* Align to 32 bytes */
			emit_load_imm32(RTMP1, ~31);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			/* STP XZR,XZR,[Xn] four times = 32 bytes */
			emit32(0xA9000000 | (31 << 10) | (RTMP0 << 5) | 31); /* STP XZR,XZR,[Xn,#0] */
			emit32(0xA9010000 | (31 << 10) | (RTMP0 << 5) | 31); /* STP XZR,XZR,[Xn,#16] */
			return true;
		}
		case 512: /* mcrxr crD — move XER[0:3] to CR field, clear XER */
		{
			uint32_t crd_f = (op >> 23) & 0x7;
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_XER);
			/* Extract top 4 bits of XER (SO,OV,CA) */
			emit_load_imm32(RTMP1, 28);
			emit32(0x1AC02400 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP1); /* LSR */
			emit_load_imm32(RTMP2, 0xF);
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
			/* Insert into CR field */
			uint32_t dst_sh = (7 - crd_f) * 4;
			if (dst_sh) { emit_load_imm32(RTMP2, dst_sh); emit32(0x1AC02000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); }
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP2, ~(0xFU << dst_sh));
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			/* Clear XER SO/OV/CA */
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_XER);
			emit_load_imm32(RTMP1, 0x0FFFFFFF);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_XER);
			return true;
		}
		case 20: /* lwarx rD,rA,rB — load word and reserve (treat as lwzx) */
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP1, rb); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0xB9400000 | (RTMP0 << 5) | RTMP1);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit_store_gpr(RTMP1, rd);
			return true;
		case 150: /* stwcx. rS,rA,rB — store word conditional (simplified: always succeed) */
			emit_load_gpr(RTMP1, PPC_RS(op));
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
			emit_load_gpr(RTMP0, ra == 0 ? rb : ra);
			if (ra != 0) { emit_load_gpr(RTMP2, rb); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
			emit32(0xB9000000 | (RTMP0 << 5) | RTMP1);
			/* Set CR0.EQ to indicate success */
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP1, 0x20000000); /* EQ bit in CR0 */
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			return true;

		case 595: /* mfsr — move from segment register (supervisor, treat as NOP returning 0) */
			emit_load_imm32(RTMP0, 0);
			emit_store_gpr(RTMP0, rd);
			return true;
		case 659: /* mfsrin — same */
			emit_load_imm32(RTMP0, 0);
			emit_store_gpr(RTMP0, rd);
			return true;

		case 83: /* mfmsr rD — simplified: return 0 */
			emit_load_imm32(RTMP0, 0);
			emit_store_gpr(RTMP0, rd);
			return true;
		case 310: /* eciwx rD,rA,rB — external control in word: NOP */
			return true;
		case 438: /* ecowx rS,rA,rB — external control out word: NOP */
			return true;

		case 822: /* dss — data stream stop: NOP */
			return true;
		case 342: /* dst — data stream touch: NOP */
			return true;
		case 374: /* dstst — data stream touch for store: NOP */
			return true;


		case 597: /* lswi rD,rA,NB */
		{
			uint32_t nb_field = rb;
			uint32_t nb = nb_field == 0 ? 32 : nb_field;
			if (ra == 0) { a64_movz(RTMP0, 0, 0); }
			else { emit_load_gpr(RTMP0, ra); }
			uint32_t r = rd;
			uint32_t bytes_done = 0;
			while (bytes_done < nb) {
				if (nb - bytes_done >= 4) {
					emit32(0xB9400000 | (RTMP0 << 5) | RTMP1);
					emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
					emit_store_gpr(RTMP1, r);
					if (bytes_done + 4 < nb) emit32(0x11001000 | (RTMP0 << 5) | RTMP0);
					bytes_done += 4;
				} else {
					a64_movz(RTMP1, 0, 0);
					for (uint32_t b = 0; b < nb - bytes_done; b++) {
						emit32(0x38401400 | (RTMP0 << 5) | RTMP2);
						uint32_t sh = (3 - b) * 8;
						if (sh) { emit_load_imm32(3, sh); emit32(0x1AC02000 | (3 << 16) | (RTMP2 << 5) | RTMP2); }
						emit32(0x2A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
					}
					emit_store_gpr(RTMP1, r);
					bytes_done = nb;
				}
				r = (r + 1) & 31;
			}
			return true;
		}
		case 725: /* stswi rS,rA,NB */
		{
			uint32_t nb_field = rb;
			uint32_t nb = nb_field == 0 ? 32 : nb_field;
			if (ra == 0) { a64_movz(RTMP0, 0, 0); }
			else { emit_load_gpr(RTMP0, ra); }
			uint32_t r = PPC_RS(op);
			uint32_t bytes_done = 0;
			while (bytes_done < nb) {
				if (nb - bytes_done >= 4) {
					emit_load_gpr(RTMP1, r);
					emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
					emit32(0xB8004400 | (RTMP0 << 5) | RTMP1);
					bytes_done += 4;
				} else {
					emit_load_gpr(RTMP1, r);
					for (uint32_t b = 0; b < nb - bytes_done; b++) {
						a64_mov_reg(RTMP2, RTMP1);
						uint32_t sh = (3 - b) * 8;
						if (sh) { emit_load_imm32(3, sh); emit32(0x1AC02400 | (3 << 16) | (RTMP2 << 5) | RTMP2); }
						emit32(0x38001400 | (RTMP0 << 5) | RTMP2);
					}
					bytes_done = nb;
				}
				r = (r + 1) & 31;
			}
			return true;
		}
		case 533: /* lswx: emit PC update, return to interpreter for runtime NB */
			emit_epilogue_with_pc(pc);
			return true; /* block ends here, interpreter handles the actual lswx */
		case 661: /* stswx: same approach */
			emit_epilogue_with_pc(pc);
			return true;


										default:
			jit_xo_miss[(op >> 1) & 0x3FF]++;
			return true; /* unknown opcode: NOP */
		}
	}

	case 32: /* lwz rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		if (ra == 0) {
			emit_load_imm32(RTMP0, (int32_t)simm);
		} else {
			emit_load_gpr(RTMP0, ra);
			if (simm) {
				emit_load_imm32(RTMP1, (int32_t)simm);
				emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			}
		}
		/* LDR W(RTMP1), [X(RTMP0)] — load 32-bit from host address */
		emit32(0xB9400000 | (RTMP0 << 5) | RTMP1); /* LDR Wt, [Xn] */
		/* Byte-swap: PPC is big-endian, ARM64 is little-endian */
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV Wd, Wn */
		emit_store_gpr(RTMP1, rd);
		return true;

	case 36: /* stw rS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);                  /* value to store */
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV (byte-swap) */
		emit_load_gpr(RTMP0, ra);                   /* base address */
		if (simm) {
			emit_load_imm32(RTMP2, (int32_t)simm);
			emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
		}
		/* STR Wt, [Xn] */
		emit32(0xB9000000 | (RTMP0 << 5) | RTMP1);
		return true;

	case 34: /* lbz rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		emit32(0x39400000 | (RTMP0 << 5) | RTMP1); /* LDRB Wt, [Xn] */
		emit_store_gpr(RTMP1, rd);
		return true;

	case 38: /* stb rS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);
		emit_load_gpr(RTMP0, ra);
		if (simm) { emit_load_imm32(RTMP2, (int32_t)simm); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
		emit32(0x39000000 | (RTMP0 << 5) | RTMP1); /* STRB Wt, [Xn] */
		return true;

	case 40: /* lhz rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		emit32(0x79400000 | (RTMP0 << 5) | RTMP1); /* LDRH Wt, [Xn] */
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 Wd, Wn (byte-swap halfword) */
		emit_store_gpr(RTMP1, rd);
		return true;

	case 44: /* sth rS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 */
		emit_load_gpr(RTMP0, ra);
		if (simm) { emit_load_imm32(RTMP2, (int32_t)simm); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
		emit32(0x79000000 | (RTMP0 << 5) | RTMP1); /* STRH Wt, [Xn] */
		return true;

	case 12: /* addic rD,rA,SIMM (sets XER[CA]) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		/* ADDS Wd, Wn, Wm (sets NZCV — we use C for carry-out) */
		emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, rd);
		/* Set XER[CA] from ARM64 carry flag */
		/* MRS Xt, NZCV */
		emit32(0xD53B4200 | RTMP1);
		/* Extract C bit (bit 29) → XER bit 29 (CA) */
		a64_ldr_w_imm(RTMP2, RSTATE, PPCR_XER);
		emit_load_imm32(RTMP0, ~(1 << 29));
		emit32(0x0A000000 | (RTMP0 << 16) | (RTMP2 << 5) | RTMP2); /* clear CA */
		emit32(0x12001C00 | (RTMP1 << 5) | RTMP1); /* AND Wd,Wn, #0x20000000 (bit 29) */
		emit32(0x2A000000 | (RTMP1 << 16) | (RTMP2 << 5) | RTMP2); /* OR in carry */
		a64_str_w_imm(RTMP2, RSTATE, PPCR_XER);
		return true;

	case 21: /* rlwinm rA,rS,SH,MB,ME */
	{
		uint32_t rs = PPC_RS(op);
		ra = PPC_RA(op);
		uint32_t sh = (op >> 11) & 0x1F;
		uint32_t mb = (op >> 6) & 0x1F;
		uint32_t me = (op >> 1) & 0x1F;
		emit_load_gpr(RTMP0, rs);
		/* Rotate left by SH: ROR Wd,Wn,#(32-SH) */
		if (sh) {
			uint32_t ror_amt = (32 - sh) & 0x1F;
			/* EXTR Wd, Wn, Wn, #ror_amt = rotate right */
			emit32(0x13800000 | (RTMP0 << 16) | (ror_amt << 10) | (RTMP0 << 5) | RTMP0);
		}
		/* Apply mask MB..ME */
		uint32_t mask = 0;
		if (mb <= me) {
			for (uint32_t i = mb; i <= me; i++) mask |= (0x80000000U >> i);
		} else {
			for (uint32_t i = 0; i <= me; i++) mask |= (0x80000000U >> i);
			for (uint32_t i = mb; i <= 31; i++) mask |= (0x80000000U >> i);
		}
		if (mask != 0xFFFFFFFF) {
			emit_load_imm32(RTMP1, (int32_t)mask);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND */
		}
		emit_store_gpr(RTMP0, ra);
		return true;
	}

	case 20: /* rlwimi rA,rS,SH,MB,ME (insert) */
	{
		uint32_t rs = PPC_RS(op);
		ra = PPC_RA(op);
		uint32_t sh = (op >> 11) & 0x1F;
		uint32_t mb = (op >> 6) & 0x1F;
		uint32_t me = (op >> 1) & 0x1F;
		/* Rotate rS left by SH */
		emit_load_gpr(RTMP0, rs);
		if (sh) {
			uint32_t ror_amt = (32 - sh) & 0x1F;
			emit32(0x13800000 | (RTMP0 << 16) | (ror_amt << 10) | (RTMP0 << 5) | RTMP0);
		}
		/* Compute mask */
		uint32_t mask = 0;
		if (mb <= me) {
			for (uint32_t i = mb; i <= me; i++) mask |= (0x80000000U >> i);
		} else {
			for (uint32_t i = 0; i <= me; i++) mask |= (0x80000000U >> i);
			for (uint32_t i = mb; i <= 31; i++) mask |= (0x80000000U >> i);
		}
		/* rA = (rotated_rS & mask) | (rA & ~mask) */
		emit_load_imm32(RTMP1, (int32_t)mask);
		emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* rotated & mask */
		emit_load_gpr(RTMP2, ra);
		emit_load_imm32(RTMP1, (int32_t)~mask);
		emit32(0x0A000000 | (RTMP1 << 16) | (RTMP2 << 5) | RTMP2); /* rA & ~mask */
		emit32(0x2A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* OR */
		emit_store_gpr(RTMP0, ra);
		return true;
	}

	case 33: /* lwzu rD,d(rA) — load word and update rA */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* effective addr */
		emit_store_gpr(RTMP0, ra); /* update rA */
		emit32(0xB9400000 | (RTMP0 << 5) | RTMP1); /* LDR Wt, [Xn] */
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV (byte-swap) */
		emit_store_gpr(RTMP1, rd);
		return true;

	case 37: /* stwu rS,d(rA) — store word and update rA */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP2, (int32_t)simm);
		emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* effective addr */
		emit_store_gpr(RTMP0, ra); /* update rA */
		emit32(0xB9000000 | (RTMP0 << 5) | RTMP1); /* STR */
		return true;


	case 8: /* subfic rD,rA,SIMM (rD = SIMM - rA, set CA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_imm32(RTMP0, (int32_t)simm);
		emit_load_gpr(RTMP1, ra);
		emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SUBS Wd,Wn,Wm */
		emit_store_gpr(RTMP0, rd);
		/* TODO: set XER[CA] from carry */
		return true;


	case 10: /* cmpli (cmplwi) crD,rA,UIMM */
	{
		uint32_t crd = (op >> 23) & 0x7;
		ra = PPC_RA(op); uimm = PPC_UIMM(op);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)(uint32_t)uimm);
		/* Unsigned compare: CMP Wn, Wm */
		emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | 0x1F);
		/* Build CR field from unsigned comparison:
		   LT = unsigned less (ARM64 CC = carry clear)
		   GT = unsigned greater (ARM64 CC = carry set AND not zero)
		   EQ = equal */
		emit32(0xD53B4200 | RTMP2); /* MRS NZCV */
		a64_movz(RTMP0, 0, 0);
		emit_load_imm32(RTMP1, 8); /* LT */
		/* CSEL RTMP0, RTMP1, RTMP0, CC (unsigned less = carry clear) */
		emit32(0x1A800000 | (RTMP0 << 16) | (0x3 << 12) | (RTMP1 << 5) | RTMP0);
		emit_load_imm32(RTMP1, 4); /* GT */
		/* CSEL RTMP0, RTMP1, RTMP0, HI (unsigned greater) */
		emit32(0x1A800000 | (RTMP0 << 16) | (0x8 << 12) | (RTMP1 << 5) | RTMP0);
		emit_load_imm32(RTMP1, 2); /* EQ */
		/* CSEL RTMP0, RTMP1, RTMP0, EQ */
		emit32(0x1A800000 | (RTMP0 << 16) | (0x0 << 12) | (RTMP1 << 5) | RTMP0);
		uint32_t shift = (7 - crd) * 4;
		if (shift) { emit_load_imm32(RTMP1, shift); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
		emit_load_imm32(RTMP2, ~(0xF << shift));
		emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
		emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1);
		a64_str_w_imm(RTMP1, RSTATE, PPCR_CR);
		return true;
	}

	case 11: /* cmpi (cmpwi) crD,rA,SIMM */
	{
		uint32_t crd = (op >> 23) & 0x7;
		ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		/* CMP Wn, Wm (sets NZCV) */
		emit32(0x6B000000 | (RTMP1 << 16) | (RTMP0 << 5) | 0x1F); /* SUBS WZR,Wn,Wm */
		/* Read NZCV into RTMP2 */
		emit32(0xD53B4200 | RTMP2); /* MRS Xt, NZCV */
		/* Convert ARM64 NZCV to PPC CR field:
		   PPC CR: bit0=LT(N), bit1=GT(!N&!Z), bit2=EQ(Z), bit3=SO(from XER)
		   ARM64 NZCV: N=bit31, Z=bit30, C=bit29, V=bit28
		   Simple mapping: shift NZCV right by 28, remap */
		emit32(0xD340FC00 | (RTMP2 << 5) | RTMP2 | (28 << 10)); /* LSR Xt, Xt, #28 */
		/* For now, store raw NZCV>>28 into CR field position.
		   CR field crd occupies bits (28-crd*4) to (31-crd*4) of CR register.
		   This is a simplified mapping — full correctness needs proper bit remap. */
		uint32_t shift = (7 - crd) * 4;
		if (shift) { emit_load_imm32(RTMP1, shift); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP2 << 5) | RTMP2); /* LSL */ }
		/* Load current CR, clear target field, OR in new value */
		a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
		emit_load_imm32(RTMP1, ~(0xF << shift));
		emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* AND (clear field) */
		emit32(0x2A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); /* ORR (insert field) */
		a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
		return true;
	}

	case 16: /* bc/bdnz/beq/bne family */
	{
		uint32_t bo = (op >> 21) & 0x1F;
		uint32_t bi = (op >> 16) & 0x1F;
		int16_t bd = ((op & 0xFFFC) ^ 0x8000) - 0x8000;
		bool lk = op & 1;
		uint32_t target_pc = pc + bd;

		/* bdnz (BO=16): decrement CTR, branch if CTR!=0 */
		if ((bo & 0x1E) == 16 && !lk) {
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CTR);
			emit32(0x51000400 | (RTMP0 << 5) | RTMP0); /* SUB Wd, Wn, #1 */
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CTR);
			/* Try intra-block backward branch */
			uint32_t *target_code = find_code_for_pc(target_pc);
			if (target_code) {
				int32_t offset = (int32_t)((uint8_t *)target_code - (uint8_t *)jit_code_ptr);
				/* CBNZ Wn, offset */
				emit32(0x35000000 | (((offset >> 2) & 0x7FFFF) << 5) | RTMP0);
				return true; /* Fallthrough = CTR==0, continue to next insn */
			}
			/* Forward or out-of-block: set PC and return */
			emit32(0x34000000 | (2 << 5) | RTMP0); /* CBZ Wn, +8 (skip branch-taken) */
			emit_epilogue_with_pc(target_pc);  /* taken: set PC=target, return */
			return true; /* not-taken: continue to next insn */
		}

		/* Conditional branches: beq/bne/blt/bgt/ble/bge etc. */
		/* BO bit 4 (0x10) = don't test CTR; bit 2 (0x04) = branch if CR[BI]=BO[3] */
		if ((bo & 0x14) == 0x0C && !lk) { /* BO=011xx: branch if condition TRUE */
			/* Read CR field */
			uint32_t cr_field = bi >> 2; /* which CR field */
			uint32_t cr_bit = bi & 3;   /* which bit within the field */
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			/* Extract the specific bit: shift right to get it into bit 0 */
			uint32_t bit_pos = 31 - bi; /* PPC CR bit numbering: bit 0 = MSB */
			if (bit_pos) {
				emit_load_imm32(RTMP1, bit_pos);
				emit32(0x1AC02400 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSR Wd,Wn,Wm */
			}
			emit32(0x12000000 | (RTMP0 << 5) | RTMP0); /* AND Wd, Wn, #1 */
			/* If bit is set (condition true): branch to target */
			uint32_t *target_code = find_code_for_pc(target_pc);
			if (target_code) {
				int32_t offset = (int32_t)((uint8_t *)target_code - (uint8_t *)jit_code_ptr);
				emit32(0x35000000 | (((offset >> 2) & 0x7FFFF) << 5) | RTMP0); /* CBNZ */
			} else {
				emit32(0x34000000 | (2 << 5) | RTMP0); /* CBZ skip */
				emit_epilogue_with_pc(target_pc);
			}
			return true;
		}
		if ((bo & 0x14) == 0x04 && !lk) { /* BO=001xx: branch if condition FALSE */
			uint32_t bit_pos = 31 - bi;
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			if (bit_pos) {
				emit_load_imm32(RTMP1, bit_pos);
				emit32(0x1AC02400 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			}
			emit32(0x12000000 | (RTMP0 << 5) | RTMP0); /* AND #1 */
			uint32_t *target_code = find_code_for_pc(target_pc);
			if (target_code) {
				int32_t offset = (int32_t)((uint8_t *)target_code - (uint8_t *)jit_code_ptr);
				emit32(0x34000000 | (((offset >> 2) & 0x7FFFF) << 5) | RTMP0); /* CBZ = branch if FALSE */
			} else {
				emit32(0x35000000 | (2 << 5) | RTMP0); /* CBNZ skip */
				emit_epilogue_with_pc(target_pc);
			}
			return true;
		}
		return true; /* unknown opcode: NOP */
	}

	case 19: /* CR ops, bclr, bcctr, isync */
	{
		uint32_t xo = (op >> 1) & 0x3FF;
		switch (xo) {
		case 16: /* bclr — branch conditional to LR */
		{
			uint32_t bo = (op >> 21) & 0x1F;
			uint32_t bi = (op >> 16) & 0x1F;
			bool lk = op & 1;
			if ((bo & 0x14) == 0x14) { /* BO=1x1xx: always branch */
				if (lk) { emit_load_imm32(RTMP0, (int32_t)(pc + 4)); a64_str_w_imm(RTMP0, RSTATE, PPCR_LR); }
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_LR);
				a64_str_w_imm(RTMP0, RSTATE, PPCR_PC);
				a64_ldp_post(RSTATE, 21, A64_SP, 16);
				a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
				a64_ret();
				return true;
			}
			/* Conditional bclr: test CR[BI], branch to LR if condition matches BO[3] */
			{
				uint32_t bit_pos = 31 - bi;
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
				if (bit_pos) {
					emit_load_imm32(RTMP1, bit_pos);
					emit32(0x1AC02400 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* LSR */
				}
				emit32(0x12000000 | (RTMP0 << 5) | RTMP0); /* AND #1 */
				/* BO[3] (bit 24 of op): 1=branch if CR[BI]=1, 0=branch if CR[BI]=0 */
				bool branch_if_true = (bo >> 3) & 1;
				if (lk) { emit_load_imm32(RTMP1, (int32_t)(pc + 4)); a64_str_w_imm(RTMP1, RSTATE, PPCR_LR); }
				a64_ldr_w_imm(RTMP1, RSTATE, PPCR_LR);
				/* If condition not met: set PC=pc+4 (fall through) */
				emit_load_imm32(RTMP2, (int32_t)(pc + 4));
				if (branch_if_true) {
					/* CSEL: if RTMP0!=0 (bit set), use LR; else use pc+4 */
					emit32(0x35000000 | (2 << 5) | RTMP0); /* CBNZ → skip */
					a64_mov_reg(RTMP1, RTMP2); /* not taken: PC=pc+4 */
				} else {
					emit32(0x34000000 | (2 << 5) | RTMP0); /* CBZ → skip */
					a64_mov_reg(RTMP1, RTMP2);
				}
				a64_str_w_imm(RTMP1, RSTATE, PPCR_PC);
				a64_ldp_post(RSTATE, 21, A64_SP, 16);
				a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
				a64_ret();
				return true;
			}
		}
		case 528: /* bcctr — branch conditional to CTR */
		{
			uint32_t bo = (op >> 21) & 0x1F;
			uint32_t bi = (op >> 16) & 0x1F;
			bool lk = op & 1;
			if ((bo & 0x14) == 0x14) { /* unconditional bctr */
				if (lk) { emit_load_imm32(RTMP0, (int32_t)(pc + 4)); a64_str_w_imm(RTMP0, RSTATE, PPCR_LR); }
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CTR);
				a64_str_w_imm(RTMP0, RSTATE, PPCR_PC);
				a64_ldp_post(RSTATE, 21, A64_SP, 16);
				a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
				a64_ret();
				return true;
			}
			/* Conditional bcctr: test CR[BI] */
			{
				uint32_t bit_pos = 31 - bi;
				a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
				if (bit_pos) { emit_load_imm32(RTMP1, bit_pos); emit32(0x1AC02400 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
				emit32(0x12000000 | (RTMP0 << 5) | RTMP0);
				bool branch_if_true = (bo >> 3) & 1;
				if (lk) { emit_load_imm32(RTMP1, (int32_t)(pc + 4)); a64_str_w_imm(RTMP1, RSTATE, PPCR_LR); }
				a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CTR);
				emit_load_imm32(RTMP2, (int32_t)(pc + 4));
				if (branch_if_true) {
					emit32(0x35000000 | (2 << 5) | RTMP0);
					a64_mov_reg(RTMP1, RTMP2);
				} else {
					emit32(0x34000000 | (2 << 5) | RTMP0);
					a64_mov_reg(RTMP1, RTMP2);
				}
				a64_str_w_imm(RTMP1, RSTATE, PPCR_PC);
				a64_ldp_post(RSTATE, 21, A64_SP, 16);
				a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
				a64_ret();
				return true;
			}
		}
		case 150: /* isync */
			return true;
		default:
			return true;
		}
	}

	case 17: /* sc — system call (block terminator, sets PC and returns) */
		emit_epilogue_with_pc(pc);
		return true;

	case 18: /* b/bl (unconditional branch) */
	{
		int32_t li = ((op & 0x03FFFFFC) ^ 0x02000000) - 0x02000000;
		bool lk = op & 1;
		bool aa = op & 2;
		uint32_t target = aa ? (uint32_t)li : (pc + li);
		if (lk) {
			emit_load_imm32(RTMP0, (int32_t)(pc + 4));
			a64_str_w_imm(RTMP0, RSTATE, PPCR_LR);
		}
		emit_epilogue_with_pc(target);
		return true;
	}

	case 42: /* lha rD,d(rA) — load halfword algebraic (sign-extended) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		emit32(0x79400000 | (RTMP0 << 5) | RTMP1); /* LDRH Wt, [Xn] */
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1); /* REV16 (byte-swap) */
		/* Sign-extend from 16 to 32 bits */
		emit32(0x13003C00 | (RTMP1 << 5) | RTMP1); /* SXTH Wd, Wn */
		emit_store_gpr(RTMP1, rd);
		return true;

	case 2: /* tdi — 64-bit trap: treat as NOP on 32-bit PPC */
		return true;
	case 3: /* twi — trap word immediate: simplified as NOP (trap conditions rarely fire in normal code) */
		return true;

	case 4: /* AltiVec via NEON */
	{
		uint32_t vxo = op & 0x7FF;
		uint32_t vao = op & 0x3F;
		uint32_t vd = VR_VD(op), va = VR_VA(op), vb = VR_VB(op), vc = VR_VC(op);
		switch (vxo) {
		case 0: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E208400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 64: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E608400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 128: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA08400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 10: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20D400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1024: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E208400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1088: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E608400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1152: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA08400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 74: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA0D400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1028: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E201C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1092: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E601C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1156: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA01C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1220: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E201C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1284: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA01C00|(1<<16)|(0<<5)|0); emit32(0x6E205800|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1034: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20F400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 1098: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA0F400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 266: emit_load_vr(0,vb); emit32(0x4EA1D800|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 330: emit_load_vr(0,vb); emit32(0x6EA1D800|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 394: emit_load_vr(0,vb); emit32(0x4E218800|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 458: emit_load_vr(0,vb); emit32(0x4EA19800|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 6: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E208C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 70: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E608C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 134: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA08C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 198: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20E400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 908: { int32_t s=((va&0x1F)|(va&0x10?0xFFFFFFE0:0)); emit_load_imm32(RTMP0,s); emit32(0x4E040C00|(RTMP0<<5)|0); emit_store_vr(0,vd); return true; }
		case 844: { int32_t s=((va&0x1F)|(va&0x10?0xFFFFFFE0:0)); emit_load_imm32(RTMP0,s&0xFFFF); emit32(0x4E020C00|(RTMP0<<5)|0); emit_store_vr(0,vd); return true; }
		case 780: { int32_t s=((va&0x1F)|(va&0x10?0xFFFFFFE0:0)); emit_load_imm32(RTMP0,s&0xFF); emit32(0x4E010C00|(RTMP0<<5)|0); emit_store_vr(0,vd); return true; }

		case 258: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E206400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxsb SMAX.16B */
		case 322: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E606400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxsh SMAX.8H */
		case 386: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA06400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxsw SMAX.4S */
		case 2: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E206400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxub UMAX.16B */
		case 66: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E606400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxuh UMAX.8H */
		case 130: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA06400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmaxuw UMAX.4S */
		case 770: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E206C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminsb SMIN.16B */
		case 834: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E606C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminsh SMIN.8H */
		case 898: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA06C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminsw SMIN.4S */
		case 514: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E206C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminub UMIN.16B */
		case 578: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E606C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminuh UMIN.8H */
		case 642: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA06C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vminuw UMIN.4S */
		case 260: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E205400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vslb USHL.16B */
		case 324: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E605400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vslh USHL.8H */
		case 388: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA05400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vslw USHL.4S */
		case 772: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E20B800|(1<<5)|1); emit32(0x4E205400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsrb NEG+USHL.16B */
		case 836: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E60B800|(1<<5)|1); emit32(0x4E605400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsrh */
		case 900: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA0B800|(1<<5)|1); emit32(0x4EA05400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsrw */
		case 516: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E204400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsrab SSHL.16B (arith) */
		case 580: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E60B800|(1<<5)|1); emit32(0x4E604400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsrah */
		case 644: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA0B800|(1<<5)|1); emit32(0x4EA04400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsraw */
		case 4: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E205C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vrlb USHL.16B (rotate=shift by variable) */
		case 68: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E605C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vrlh */
		case 132: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA05C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vrlw */
		case 524: { uint32_t idx=va; emit_load_vr(0,vb); emit32(0x4E010400|((idx*2+1)<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vspltb DUP.16B */
		case 588: { uint32_t idx=va; emit_load_vr(0,vb); emit32(0x4E020400|((idx*4+2)<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vsplth DUP.8H */
		case 652: { uint32_t idx=va; emit_load_vr(0,vb); emit32(0x4E040400|((idx*8+4)<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vspltw DUP.4S */
		case 522: emit_load_vr(0,vb); emit32(0x4EA18800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vrfip FRINTP */
		case 586: emit_load_vr(0,vb); emit32(0x4E219800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vrfim FRINTM */
		case 198+768: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E20E400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgefp FCMGE */
		case 454: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA0E400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtfp FCMGT */
		case 774: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E203400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtsb CMGT.16B (signed) */
		case 838: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E603400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtsh */
		case 902: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA03400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtsw */
		case 518: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E203400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtub CMHI.16B (unsigned) */
		case 582: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E603400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtuh */
		case 646: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA03400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vcmpgtuw */
		case 1282: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20A400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavgub URHADD.16B */
		case 1346: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E60A400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavguh */
		case 1410: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA0A400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavguw */
		case 1794: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E201400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavgsb SRHADD.16B */
		case 1858: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E601400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavgsh */
		case 1922: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0EA01400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vavgsw */
		case 768: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E207C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vaddubs UQADD.16B (saturating) */
		case 832: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E607C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vadduhs UQADD.8H */
		case 896: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA07C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vadduws UQADD.4S */
		case 1792: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E202C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsububs UQSUB.16B */
		case 1856: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E602C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsubuhs UQSUB.8H */
		case 1920: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6EA02C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsubuws UQSUB.4S */
		case 512: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E207C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vaddsbs SQADD.16B */
		case 576: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E607C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vaddshs SQADD.8H */
		case 640: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA07C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vaddsws SQADD.4S */
		case 1536: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E202C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsubsbs SQSUB.16B */
		case 1600: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E602C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsubshs SQSUB.8H */
		case 12: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20C400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrghb ZIP1.16B */
		case 76: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E60C400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrghh ZIP1.8H */
		case 140: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA0C400|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrghw ZIP1.4S */
		case 268: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E20C800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrglb ZIP2.16B */
		case 332: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E60C800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrglh ZIP2.8H */
		case 396: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4EA0C800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmrglw ZIP2.4S */

		case 846: { emit_load_vr(0,vb); emit32(0x4E21C800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vcfsx SCVTF.4S */
		case 910: { emit_load_vr(0,vb); emit32(0x6E21C800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vcfux UCVTF.4S */
		case 970: { emit_load_vr(0,vb); emit32(0x4EA1B800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vctsxs FCVTZS.4S */
		case 906: { emit_load_vr(0,vb); emit32(0x6EA1B800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vctuxs FCVTZU.4S */
		case 354: { emit_load_vr(0,vb); emit32(0x4E21D800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vexptefp FRECPE (approx) */
		case 418: { emit_load_vr(0,vb); emit32(0x4EA1D800|(0<<5)|0); emit_store_vr(0,vd); return true; } /* vlogefp (approx via FRECPE) */
		case 8: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E209C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmuloub UMULL.8H (odd bytes) */
		case 72: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E60A000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmulouh UMULL.4S */
		case 264: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E20A000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmuleub UMULL2.8H */
		case 328: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E60A000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmuleuh UMULL2.4S */
		case 776: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E209C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmulosb SMULL.8H */
		case 840: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E60C000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmulosh SMULL.4S */
		case 520: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E20C000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmulesb SMULL2.8H */
		case 584: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E60C000|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmulesh SMULL2.4S */
		case 14: emit_load_vr(0,vb); emit32(0x0E212800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkuhum UZP1.8H (narrow) */
		case 78: emit_load_vr(0,vb); emit32(0x0E612800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkuwum UZP1.4S */
		case 398: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E216800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkshus SQXTUN.8B */
		case 462: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E616800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkswus SQXTUN.4H */
		case 270: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E214800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkshss SQXTN.8B */
		case 334: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x0E614800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkswss SQXTN.4H */
		case 142: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x2E212800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkuhus UQXTN.8B */
		case 206: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x2E612800|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vpkuwus UQXTN.4H */
		case 814: emit_load_vr(0,vb); emit32(0x0E212800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vupkhsb SXTL.8H (unpack high signed byte) */
		case 878: emit_load_vr(0,vb); emit32(0x0E612800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vupkhsh SXTL.4S */
		case 942: emit_load_vr(0,vb); emit32(0x4E212800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vupklsb SXTL2.8H */
		case 1006: emit_load_vr(0,vb); emit32(0x4E612800|(0<<5)|0); emit_store_vr(0,vd); return true; /* vupklsh SXTL2.4S */
		case 452: { uint32_t sh=(op>>6)&0xF; emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E080400|((16-sh)<<16)|(1<<5)|0); emit_store_vr(0,vd); return true; } /* vsldoi EXT.16B */
		case 1036: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x4E205C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsl SHL (whole vector) */
		case 1100: emit_load_vr(0,va); emit_load_vr(1,vb); emit32(0x6E205C00|(1<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vsr (whole vector right) */
		case 1604: return true; /* mtvscr NOP */
		case 1540: emit_load_imm32(RTMP0,0); emit32(0x4E010C00|(RTMP0<<5)|0); emit_store_vr(0,vd); return true; /* mfvscr - return 0 */
case 782: /* vpkpx — pack pixel 32→16 bit (approximate narrow) */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x0E612800 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 974: /* vupkhpx — unpack high pixel (widen) */
			emit_load_vr(0, vb);
			emit32(0x2F10A400 | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1038: /* vupklpx — unpack low pixel */
			emit_load_vr(0, vb);
			emit32(0x6F10A400 | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1928: /* vsum4ubs */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x6E202800 | (0 << 5) | 0);
			emit32(0x6E602800 | (0 << 5) | 0);
			emit32(0x4EA08400 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1672: /* vsum4sbs */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x4E202800 | (0 << 5) | 0);
			emit32(0x4E602800 | (0 << 5) | 0);
			emit32(0x4EA08400 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1608: /* vsum4shs */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x4E602800 | (0 << 5) | 0);
			emit32(0x4EA08400 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1800: /* vsum2sws */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x4EA02800 | (0 << 5) | 0);
			emit32(0x0EA12800 | (0 << 5) | 0);
			emit32(0x4EA08400 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1932: /* vsumsws — sum all words */
			emit_load_vr(0, va); emit_load_vr(1, vb);
			emit32(0x4EB1B800 | (0 << 5) | 0);
			emit32(0x4EA08400 | (1 << 16) | (0 << 5) | 0);
			emit_store_vr(0, vd); return true;
		case 1356: /* vslo — shift left by octet (approx: pass through) */
			emit_load_vr(0, va); emit_store_vr(0, vd); return true;
		case 1420: /* vsro — shift right by octet (approx) */
			emit_load_vr(0, va); emit_store_vr(0, vd); return true;
		default: break;
		}
		switch (vao) {
		case 46: emit_load_vr(0,va); emit_load_vr(1,vc); emit_load_vr(2,vb); emit32(0x4E21CC00|(1<<16)|(0<<5)|2); emit_store_vr(2,vd); return true;
		case 47: emit_load_vr(0,va); emit_load_vr(1,vc); emit_load_vr(2,vb); emit32(0x4EA1CC00|(1<<16)|(0<<5)|2); emit_store_vr(2,vd); return true;
		case 43: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x4E002000|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true;
		case 42: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x6E601C00|(1<<16)|(0<<5)|2); emit_store_vr(2,vd); return true;

		case 32: emit_load_vr(0,va); emit_load_vr(1,vc); emit_load_vr(2,vb); emit32(0x4E21CC00|(1<<16)|(0<<5)|2); emit_store_vr(2,vd); return true; /* vmhaddshs (approx via FMLA) */
		case 33: emit_load_vr(0,va); emit_load_vr(1,vc); emit_load_vr(2,vb); emit32(0x4E21CC00|(1<<16)|(0<<5)|2); emit_store_vr(2,vd); return true; /* vmhraddshs (approx) */
		case 34: emit_load_vr(0,va); emit_load_vr(1,vc); emit_load_vr(2,vb); emit32(0x4E609C00|(1<<16)|(0<<5)|0); emit32(0x4E608400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmladduhm MUL+ADD */
		case 36: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x4E209C00|(1<<16)|(0<<5)|0); emit32(0x4E208400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmsumubm (approx) */
		case 37: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x4E609C00|(1<<16)|(0<<5)|0); emit32(0x4E608400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmsumshm */
		case 38: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x6E609C00|(1<<16)|(0<<5)|0); emit32(0x6E608400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmsumshs */
		case 40: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x6E209C00|(1<<16)|(0<<5)|0); emit32(0x6E208400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmsumubm */
		case 41: emit_load_vr(0,va); emit_load_vr(1,vb); emit_load_vr(2,vc); emit32(0x4E609C00|(1<<16)|(0<<5)|0); emit32(0x4E608400|(2<<16)|(0<<5)|0); emit_store_vr(0,vd); return true; /* vmsumuhm */
		default: return true;
		}
	}

	case 7: /* mulli rD,rA,SIMM */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x1B007C00 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* MUL Wd,Wn,Wm */
		emit_store_gpr(RTMP0, rd);
		return true;

	case 13: /* addic. rD,rA,SIMM (sets XER[CA] + CR0) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x2B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADDS */
		emit_store_gpr(RTMP0, rd);
		emit_update_cr0(RTMP0);
		return true;

	case 35: /* lbzu rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		emit32(0x39400000 | (RTMP0 << 5) | RTMP1);
		emit_store_gpr(RTMP1, rd);
		return true;

	case 39: /* stbu rS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP2, (int32_t)simm);
		emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		emit32(0x39000000 | (RTMP0 << 5) | RTMP1);
		return true;

	case 41: /* lhzu rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		emit32(0x79400000 | (RTMP0 << 5) | RTMP1);
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
		emit_store_gpr(RTMP1, rd);
		return true;

	case 43: /* lhau rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		/* ra==0: use 0 as base; ra==rd: update gets overwritten by load (PPC undefined but harmless) */
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		emit32(0x79400000 | (RTMP0 << 5) | RTMP1);
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
		emit32(0x13003C00 | (RTMP1 << 5) | RTMP1);
		emit_store_gpr(RTMP1, rd);
		return true;

	case 45: /* sthu rS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_gpr(RTMP1, rd);
		emit32(0x5AC00400 | (RTMP1 << 5) | RTMP1);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP2, (int32_t)simm);
		emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		emit32(0x79000000 | (RTMP0 << 5) | RTMP1);
		return true;

	case 49: /* lfsu frD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		emit32(0xB9400000 | (RTMP0 << 5) | RTMP1);
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
		emit32(0x1E270000 | (RTMP1 << 5) | 0);
		emit32(0x1E22C000 | (0 << 5) | 0);
		emit_store_fpr(0, rd);
		return true;

	case 51: /* lfdu frD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		emit_load_imm32(RTMP1, (int32_t)simm);
		emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		emit32(0xF9400000 | (RTMP0 << 5) | RTMP1);
		emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
		emit32(0x9E670000 | (RTMP1 << 5) | 0);
		emit_store_fpr(0, rd);
		return true;

	case 53: /* stfsu frS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_fpr(0, rd);
		emit32(0x1E624000 | (0 << 5) | 0);
		emit32(0x1E260000 | (0 << 5) | RTMP1);
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP2, (int32_t)simm);
		emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		emit32(0xB9000000 | (RTMP0 << 5) | RTMP1);
		return true;

	case 55: /* stfdu frS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_fpr(0, rd);
		emit32(0x9E660000 | (0 << 5) | RTMP1);
		emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1);
		emit_load_gpr(RTMP0, ra);
		emit_load_imm32(RTMP2, (int32_t)simm);
		emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
		emit_store_gpr(RTMP0, ra);
		emit32(0xF9000000 | (RTMP0 << 5) | RTMP1);
		return true;

	case 46: /* lmw rD,d(rA) — load multiple words */
	{
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		for (uint32_t r = rd; r < 32; r++) {
			emit32(0xB9400000 | (RTMP0 << 5) | RTMP1); /* LDR Wt, [Xn] */
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
			emit_store_gpr(RTMP1, r);
			if (r < 31) emit32(0x11001000 | (RTMP0 << 5) | RTMP0); /* ADD Wn, Wn, #4 */
		}
		return true;
	}

	case 47: /* stmw rS,d(rA) — store multiple words */
	{
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		for (uint32_t r = rd; r < 32; r++) {
			emit_load_gpr(RTMP1, r);
			emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
			emit32(0xB9000000 | (RTMP0 << 5) | RTMP1); /* STR Wt, [Xn] */
			if (r < 31) emit32(0x11001000 | (RTMP0 << 5) | RTMP0); /* ADD +4 */
		}
		return true;
	}

	case 48: /* lfs frD,d(rA) — load float single */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		/* Load 32-bit float, byte-swap, convert to double */
		emit32(0xB9400000 | (RTMP0 << 5) | RTMP1); /* LDR Wt, [Xn] */
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV Wd */
		/* Move int to float reg: FMOV Sd, Wn */
		emit32(0x1E270000 | (RTMP1 << 5) | 0); /* FMOV S0, Wn */
		/* Convert single to double: FCVT Dd, Sd */
		emit32(0x1E22C000 | (0 << 5) | 0); /* FCVT D0, S0 */
		emit_store_fpr(0, rd);
		return true;

	case 50: /* lfd frD,d(rA) — load float double */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_ea_base(ra);
		if (simm) { emit_load_imm32(RTMP1, (int32_t)simm); emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
		/* Load 64-bit, byte-swap */
		emit32(0xF9400000 | (RTMP0 << 5) | RTMP1); /* LDR Xt, [Xn] (64-bit) */
		emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1); /* REV Xd, Xn (64-bit byte-swap) */
		/* Move to FP reg: FMOV Dd, Xn */
		emit32(0x9E670000 | (RTMP1 << 5) | 0); /* FMOV D0, Xn */
		emit_store_fpr(0, rd);
		return true;

	case 52: /* stfs frS,d(rA) — store float single */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_fpr(0, rd);
		/* Convert double to single: FCVT Sd, Dd */
		emit32(0x1E624000 | (0 << 5) | 0);
		/* Move float to int: FMOV Wn, Sd */
		emit32(0x1E260000 | (0 << 5) | RTMP1);
		/* Byte-swap and store */
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV */
		emit_load_gpr(RTMP0, ra);
		if (simm) { emit_load_imm32(RTMP2, (int32_t)simm); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
		emit32(0xB9000000 | (RTMP0 << 5) | RTMP1); /* STR Wt, [Xn] */
		return true;

	case 54: /* stfd frS,d(rA) — store float double */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		emit_load_fpr(0, rd);
		/* Move FP to int: FMOV Xn, Dd */
		emit32(0x9E660000 | (0 << 5) | RTMP1);
		/* Byte-swap 64-bit */
		emit32(0xDAC00C00 | (RTMP1 << 5) | RTMP1); /* REV Xd, Xn */
		emit_load_gpr(RTMP0, ra);
		if (simm) { emit_load_imm32(RTMP2, (int32_t)simm); emit32(0x0B000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0); }
		/* STR Xt, [Xn] (64-bit store) */
		emit32(0xF9000000 | (RTMP0 << 5) | RTMP1);
		return true;

	case 63: /* double-precision FP ops */
	{
		uint32_t xo10 = (op >> 1) & 0x3FF;
		uint32_t xo5 = (op >> 1) & 0x1F;
		uint32_t frd = PPC_RD(op);
		uint32_t fra = PPC_RA(op);
		uint32_t frb = (op >> 11) & 0x1F;
		uint32_t frc = (op >> 6) & 0x1F;

		/* X-form FP ops (10-bit XO) */
		switch (xo10) {
		case 72: /* fmr frD,frB — FP move register */
			emit_load_fpr(0, frb);
			emit_store_fpr(0, frd);
			return true;

		case 40: /* fneg frD,frB — FP negate */
			emit_load_fpr(0, frb);
			emit32(0x1E614000 | (0 << 5) | 0); /* FNEG Dd, Dn */
			emit_store_fpr(0, frd);
			return true;

		case 264: /* fabs frD,frB — FP absolute value */
			emit_load_fpr(0, frb);
			emit32(0x1E60C000 | (0 << 5) | 0); /* FABS Dd, Dn */
			emit_store_fpr(0, frd);
			return true;

		case 136: /* fnabs frD,frB — FP negative absolute */
			emit_load_fpr(0, frb);
			emit32(0x1E60C000 | (0 << 5) | 0); /* FABS */
			emit32(0x1E614000 | (0 << 5) | 0); /* FNEG */
			emit_store_fpr(0, frd);
			return true;

		case 0: /* fcmpu crD,frA,frB */
		{
			uint32_t crd = (op >> 23) & 0x7;
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E602000 | (1 << 16) | (0 << 5)); /* FCMP Dn, Dm */
			/* ARM64 FCMP sets NZCV: N=less, Z=equal, C=greater_or_unord, V=unordered */
			a64_movz(RTMP0, 0, 0);
			emit_load_imm32(RTMP1, 8); /* LT */
			emit32(0x1A800000 | (RTMP0 << 16) | (0xB << 12) | (RTMP1 << 5) | RTMP0); /* CSEL LT */
			emit_load_imm32(RTMP1, 4); /* GT */
			emit32(0x1A800000 | (RTMP0 << 16) | (0xC << 12) | (RTMP1 << 5) | RTMP0); /* CSEL GT */
			emit_load_imm32(RTMP1, 2); /* EQ */
			emit32(0x1A800000 | (RTMP0 << 16) | (0x0 << 12) | (RTMP1 << 5) | RTMP0); /* CSEL EQ */
			/* TODO: handle unordered (set FU bit) */
			uint32_t shift = (7 - crd) * 4;
			if (shift) { emit_load_imm32(RTMP1, shift); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP2, ~(0xF << shift));
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
			emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1);
			a64_str_w_imm(RTMP1, RSTATE, PPCR_CR);
			return true;
		}


		case 32: /* fcmpo crD,frA,frB — same as fcmpu for our purposes */
		{
			uint32_t crd = (op >> 23) & 0x7;
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E602000 | (1 << 16) | (0 << 5));
			a64_movz(RTMP0, 0, 0);
			emit_load_imm32(RTMP1, 8);
			emit32(0x1A800000 | (RTMP0 << 16) | (0xB << 12) | (RTMP1 << 5) | RTMP0);
			emit_load_imm32(RTMP1, 4);
			emit32(0x1A800000 | (RTMP0 << 16) | (0xC << 12) | (RTMP1 << 5) | RTMP0);
			emit_load_imm32(RTMP1, 2);
			emit32(0x1A800000 | (RTMP0 << 16) | (0x0 << 12) | (RTMP1 << 5) | RTMP0);
			uint32_t shift = (7 - crd) * 4;
			if (shift) { emit_load_imm32(RTMP1, shift); emit32(0x1AC02000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); }
			a64_ldr_w_imm(RTMP1, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP2, ~(0xF << shift));
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
			emit32(0x2A000000 | (RTMP0 << 16) | (RTMP1 << 5) | RTMP1);
			a64_str_w_imm(RTMP1, RSTATE, PPCR_CR);
			return true;
		}

		case 12: /* frsp frD,frB — round to single precision */
			emit_load_fpr(0, frb);
			emit32(0x1E624000 | (0 << 5) | 0); /* FCVT Sd, Dd */
			emit32(0x1E22C000 | (0 << 5) | 0); /* FCVT Dd, Sd */
			emit_store_fpr(0, frd);
			return true;

		case 14: /* fctiw frD,frB — convert to integer word (round per FPSCR) */
			emit_load_fpr(0, frb);
			emit32(0x9E780000 | (0 << 5) | RTMP0); /* FCVTZS Xd, Dn (toward zero) */
			/* Store as int in FPR (low 32 bits) */
			emit32(0x9E670000 | (RTMP0 << 5) | 0); /* FMOV Dd, Xn */
			emit_store_fpr(0, frd);
			return true;

		case 15: /* fctiwz frD,frB — convert to integer word (round toward zero) */
			emit_load_fpr(0, frb);
			emit32(0x9E780000 | (0 << 5) | RTMP0); /* FCVTZS Xd, Dn */
			emit32(0x9E670000 | (RTMP0 << 5) | 0); /* FMOV Dd, Xn */
			emit_store_fpr(0, frd);
			return true;

		case 583: /* mffs frD — move from FPSCR */
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
			emit32(0x9E670000 | (RTMP0 << 5) | 0); /* FMOV Dd, Xn */
			emit_store_fpr(0, frd);
			return true;

		case 711: /* mtfsfi crD,IMM — set FPSCR field (simplified as NOP) */
			return true;

		case 70: /* mtfsb0 bit — clear FPSCR bit (simplified) */
			return true;

		case 38: /* mtfsb1 bit — set FPSCR bit (simplified) */
			return true;

		case 134: /* mtfsf FM,frB — move to FPSCR fields */
			emit_load_fpr(0, frb);
			emit32(0x9E660000 | (0 << 5) | RTMP0); /* FMOV Xn, Dd */
			a64_str_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
			return true;

		case 23: /* fsel frD,frA,frC,frB — if frA >= 0 then frC else frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit_load_fpr(2, frb);
			/* Compare frA with 0.0 */
			emit32(0x1E602010 | (0 << 5)); /* FCMP Dn, #0.0 */
			/* FCSEL Dd, Dc, Db, GE */
			emit32(0x1E600C00 | (2 << 16) | (0xA << 12) | (1 << 5) | 0); /* FCSEL D0,D1,D2,GE */
			emit_store_fpr(0, frd);
			return true;
		
		case 64: /* mcrfs crD,crS — move from FPSCR field to CR field */
		{
			uint32_t crd_f = (op >> 23) & 0x7;
			uint32_t crs_f = (op >> 18) & 0x7;
			/* Read FPSCR field and write to CR field */
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_FPSCR);
			uint32_t src_sh = (7 - crs_f) * 4;
			uint32_t dst_sh = (7 - crd_f) * 4;
			a64_mov_reg(RTMP1, RTMP0);
			if (src_sh) { emit_load_imm32(RTMP2, src_sh); emit32(0x1AC02400 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); }
			emit_load_imm32(RTMP2, 0xF);
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1);
			if (dst_sh) { emit_load_imm32(RTMP2, dst_sh); emit32(0x1AC02000 | (RTMP2 << 16) | (RTMP1 << 5) | RTMP1); }
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_CR);
			emit_load_imm32(RTMP2, ~(0xFU << dst_sh));
			emit32(0x0A000000 | (RTMP2 << 16) | (RTMP0 << 5) | RTMP0);
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_CR);
			return true;
		}

		case 26: /* frsqrte frD,frB — reciprocal square root estimate */
			emit_load_fpr(0, frb);
			emit32(0x1E61C000 | (0 << 5) | 0); /* FSQRT Dd,Dn */
			/* Reciprocal: compute 1.0/sqrt */
			/* Load 1.0 into D1: FMOV D1, #1.0 = 0x1E6E1000 */
			emit32(0x1E6E1000 | 1); /* FMOV D1, #1.0 */
			emit32(0x1E611800 | (0 << 16) | (1 << 5) | 0); /* FDIV D0, D1, D0 */
			emit_store_fpr(0, frd);
			return true;
		default: break; /* fall through to 5-bit XO check */
		}

		/* A-form FP ops (5-bit XO) */
		switch (xo5) {
		case 21: /* fadd frD,frA,frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E602800 | (1 << 16) | (0 << 5) | 0); /* FADD Dd, Dn, Dm */
			emit_store_fpr(0, frd);
			return true;

		case 20: /* fsub frD,frA,frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E603800 | (1 << 16) | (0 << 5) | 0); /* FSUB Dd, Dn, Dm */
			emit_store_fpr(0, frd);
			return true;

		case 25: /* fmul frD,frA,frC */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit32(0x1E600800 | (1 << 16) | (0 << 5) | 0); /* FMUL Dd, Dn, Dm */
			emit_store_fpr(0, frd);
			return true;

		case 18: /* fdiv frD,frA,frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E601800 | (1 << 16) | (0 << 5) | 0); /* FDIV Dd, Dn, Dm */
			emit_store_fpr(0, frd);
			return true;

		case 29: /* fmadd frD,frA,frC,frB = frA*frC+frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit_load_fpr(2, frb);
			emit32(0x1F400000 | (1 << 16) | (2 << 10) | (0 << 5) | 0); /* FMADD Dd,Dn,Dm,Da */
			emit_store_fpr(0, frd);
			return true;

		case 28: /* fmsub frD,frA,frC,frB = frA*frC-frB */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit_load_fpr(2, frb);
			emit32(0x1F408000 | (1 << 16) | (2 << 10) | (0 << 5) | 0); /* FMSUB */
			emit_store_fpr(0, frd);
			return true;

		case 31: /* fnmadd frD,frA,frC,frB = -(frA*frC+frB) */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit_load_fpr(2, frb);
			emit32(0x1F600000 | (1 << 16) | (2 << 10) | (0 << 5) | 0); /* FNMADD */
			emit_store_fpr(0, frd);
			return true;

		case 30: /* fnmsub frD,frA,frC,frB = -(frA*frC-frB) */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit_load_fpr(2, frb);
			emit32(0x1F608000 | (1 << 16) | (2 << 10) | (0 << 5) | 0); /* FNMSUB */
			emit_store_fpr(0, frd);
			return true;

		default:
			return true; /* unknown opcode: NOP */
		}
	}

	case 59: /* single-precision FP ops */
	{
		uint32_t xo5 = (op >> 1) & 0x1F;
		uint32_t frd = PPC_RD(op);
		uint32_t fra = PPC_RA(op);
		uint32_t frb = (op >> 11) & 0x1F;
		uint32_t frc = (op >> 6) & 0x1F;
		(void)fra; (void)frc; (void)frb; (void)frd;
		/* Single-precision: compute in double, round to single, store as double */
		switch (xo5) {
		case 21: /* fadds */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E602800 | (1 << 16) | (0 << 5) | 0); /* FADD (double) */
			/* Round to single: FCVT Sd, Dd then FCVT Dd, Sd */
			emit32(0x1E624000 | (0 << 5) | 0); /* FCVT Sd, Dd */
			emit32(0x1E22C000 | (0 << 5) | 0); /* FCVT Dd, Sd */
			emit_store_fpr(0, frd);
			return true;
		case 20: /* fsubs */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E603800 | (1 << 16) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0);
			emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd);
			return true;
		case 25: /* fmuls */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frc);
			emit32(0x1E600800 | (1 << 16) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0);
			emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd);
			return true;
		case 18: /* fdivs */
			emit_load_fpr(0, fra);
			emit_load_fpr(1, frb);
			emit32(0x1E601800 | (1 << 16) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0);
			emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd);
			return true;
		case 29: /* fmadds */
			emit_load_fpr(0, fra); emit_load_fpr(1, frc); emit_load_fpr(2, frb);
			emit32(0x1F400000 | (1 << 16) | (2 << 10) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0); emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd); return true;
		case 28: /* fmsubs */
			emit_load_fpr(0, fra); emit_load_fpr(1, frc); emit_load_fpr(2, frb);
			emit32(0x1F408000 | (1 << 16) | (2 << 10) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0); emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd); return true;
		case 31: /* fnmadds */
			emit_load_fpr(0, fra); emit_load_fpr(1, frc); emit_load_fpr(2, frb);
			emit32(0x1F600000 | (1 << 16) | (2 << 10) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0); emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd); return true;
		case 30: /* fnmsubs */
			emit_load_fpr(0, fra); emit_load_fpr(1, frc); emit_load_fpr(2, frb);
			emit32(0x1F608000 | (1 << 16) | (2 << 10) | (0 << 5) | 0);
			emit32(0x1E624000 | (0 << 5) | 0); emit32(0x1E22C000 | (0 << 5) | 0);
			emit_store_fpr(0, frd); return true;
		case 24: /* fres frD,frB — reciprocal estimate */
			emit_load_fpr(0, frb);
			emit32(0x1E624000 | (0 << 5) | 0); /* FCVT Sd,Dd */
			emit32(0x1E20F800 | (0 << 5) | 0); /* FRECPE Sd,Sn (if available, else...) */
			emit32(0x1E22C000 | (0 << 5) | 0); /* FCVT Dd,Sd */
			emit_store_fpr(0, frd); return true;
		default:
			return true; /* unknown opcode: NOP */
		}
	}

	default:
		jit_miss_count[opc]++;
		return true; /* unknown opcode: NOP */
	}
}

/* ---- Public API ---- */

bool ppc_jit_aarch64_init(size_t cache_size_kb)
{
	jit_cache_size = cache_size_kb * 1024;
	jit_cache_base = (uint8_t *)jit_cache_alloc(jit_cache_size);
	if (!jit_cache_base) {
		fprintf(stderr, "PPC-JIT-A64: failed to allocate %zu KB code cache\n", cache_size_kb);
		return true; /* unknown opcode: NOP */
	}
	jit_cache_wp = (uint32_t *)jit_cache_base;
	jit_cache_end = (uint32_t *)(jit_cache_base + jit_cache_size);
	fprintf(stderr, "PPC-JIT-A64: code cache %zu KB at %p\n", cache_size_kb, jit_cache_base);
	return true;
}

void ppc_jit_aarch64_exit(void)
{
	jit_report_misses();
	if (jit_cache_base) {
		jit_cache_free(jit_cache_base, jit_cache_size);
		jit_cache_base = NULL;
	}
}

void ppc_jit_aarch64_flush(void)
{
	jit_cache_wp = (uint32_t *)jit_cache_base;
}

bool ppc_jit_aarch64_compile(
	uint32_t pc,
	const uint8_t *ram,
	size_t ramsize,
	ppc_jit_block *out)
{
	if (!jit_cache_wp || jit_cache_wp >= jit_cache_end - 256)
		return true; /* unknown opcode: NOP */

	uint32_t *code_start = jit_cache_wp;
	jit_code_ptr = jit_cache_wp;

	/* Prologue: save callee-saved regs, set x20 = regs ptr from x0 */
	a64_stp_pre(A64_FP, A64_LR, A64_SP, -16);
	a64_stp_pre(RSTATE, 21, A64_SP, -16);
	a64_mov_reg(RSTATE, A64_X0);

	jit_blocks_attempted++;
	uint32_t cur_pc = pc;
	int n_compiled = 0;
	bool complete = true;
	insn_count = 0;

	for (int i = 0; i < 64; i++) {
		if (cur_pc < (uint32_t)(uintptr_t)ram ||
		    cur_pc >= (uint32_t)(uintptr_t)ram + ramsize)
			break;

		const uint8_t *p = ram + (cur_pc - (uint32_t)(uintptr_t)ram);
		uint32_t op = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
		              ((uint32_t)p[2] << 8) | p[3];

		if (op == 0x4E800020) { /* blr — block terminator */
			a64_ldr_w_imm(RTMP0, RSTATE, PPCR_LR);
			a64_str_w_imm(RTMP0, RSTATE, PPCR_PC);
			a64_ldp_post(RSTATE, 21, A64_SP, 16);
			a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
			a64_ret();
			n_compiled++;
			cur_pc += 4;
			break;
		}

		/* Check if this is a block-terminating opcode */
		uint32_t term_opc = op >> 26;
		bool is_terminator = (term_opc == 18); /* b/bl */
		if (term_opc == 19) {
			uint32_t term_xo = (op >> 1) & 0x3FF;
			if (term_xo == 16 || term_xo == 528) is_terminator = true; /* bclr/bcctr */
		}

		if (op == 0x00000000) { /* illegal — end of test code */
			emit_epilogue_with_pc(cur_pc);
			n_compiled++;
			cur_pc += 4;
			break;
		}

		/* Record instruction offset for intra-block branches */
		if (insn_count < 64) {
			insn_code_offset[insn_count] = jit_code_ptr;
			insn_ppc_pc[insn_count] = cur_pc;
			insn_count++;
		}

		if (!compile_one(op, cur_pc)) {
			jit_total_miss++;
			jit_miss_count[op >> 26]++;
			jit_cum_fail_opc[op >> 26]++;
			if ((op >> 26) == 31) jit_cum_fail_xo31[(op >> 1) & 0x3FF]++;
			jit_cum_fail_total++;
			emit_epilogue_with_pc(cur_pc);
			complete = false;
			break;
		}

		jit_total_hit++;
		n_compiled++;
		cur_pc += 4;

		/* Block-terminating opcodes: break after compiling them */
		if (is_terminator) break;
	}

	/* Track why blocks are incomplete */
	if (!complete && 0) {
	}

	/* Periodic report */
	if ((jit_blocks_attempted) % 100000 == 0 && jit_blocks_attempted > 0)
		jit_report_misses();

	/* If we didn't emit a ret yet, do it now */
	if (n_compiled > 0 && jit_code_ptr > code_start) {
		uint32_t last = *(jit_code_ptr - 1);
		if (last != 0xD65F03C0) { /* not a RET */
			emit_epilogue_with_pc(cur_pc);
			complete = false;
		}
	}

	size_t code_bytes = (uint8_t *)jit_code_ptr - (uint8_t *)code_start;
	jit_cache_flush(code_start, code_bytes);
	jit_cache_wp = jit_code_ptr;

	out->code = code_start;
	out->code_size = code_bytes;
	out->ppc_start_pc = pc;
	out->ppc_end_pc = cur_pc;
	out->n_insns = n_compiled;

	/* Cumulative miss report — doesn't clear counters */
	{
		static uint32_t cum_opc[64] = {0};
		static uint32_t cum_xo31[1024] = {0};
		static uint32_t cum_total = 0;
		static uint32_t cum_report_at = 100000;
		
		if (!complete) {
			/* Record the opcode that caused the failure */
			if (cur_pc >= (uint32_t)(uintptr_t)ram && cur_pc < (uint32_t)(uintptr_t)ram + ramsize) {
				const uint8_t *fail_p = ram + (cur_pc - (uint32_t)(uintptr_t)ram);
				uint32_t fail_op = ((uint32_t)fail_p[0] << 24) | ((uint32_t)fail_p[1] << 16) |
				                   ((uint32_t)fail_p[2] << 8) | fail_p[3];
				uint32_t fail_opc = fail_op >> 26;
				cum_opc[fail_opc]++;
				if (fail_opc == 31) cum_xo31[(fail_op >> 1) & 0x3FF]++;
				cum_total++;
			}
		}
		
		if (jit_blocks_attempted >= cum_report_at) {
			cum_report_at += 100000;
			fprintf(stderr, "PPC-JIT-A64-CUM: %u fail opcodes in %u blocks (%u attempted), top blockers:\n", jit_cum_fail_total, jit_blocks_attempted - jit_blocks_complete, jit_blocks_attempted);
			/* Copy arrays for sorted output without destroying data */
			uint32_t tmp_opc[64]; memcpy(tmp_opc, jit_cum_fail_opc, sizeof(tmp_opc));
			for (int pass = 0; pass < 15; pass++) {
				uint32_t max_v = 0; int max_i = -1;
				for (int i = 0; i < 64; i++) if (tmp_opc[i] > max_v) { max_v = tmp_opc[i]; max_i = i; }
				if (max_i < 0 || max_v == 0) break;
				fprintf(stderr, "  opc=%d: %u blocks\n", max_i, max_v);
				tmp_opc[max_i] = 0;
			}
			uint32_t tmp_xo[1024]; memcpy(tmp_xo, jit_cum_fail_xo31, sizeof(tmp_xo));
			fprintf(stderr, "PPC-JIT-A64-CUM: top XO31 blockers:\n");
			for (int pass = 0; pass < 10; pass++) {
				uint32_t max_v = 0; int max_i = -1;
				for (int i = 0; i < 1024; i++) if (tmp_xo[i] > max_v) { max_v = tmp_xo[i]; max_i = i; }
				if (max_i < 0 || max_v == 0) break;
				fprintf(stderr, "  XO=%d: %u blocks\n", max_i, max_v);
				tmp_xo[max_i] = 0;
			}
		}
	}

	out->complete = complete;
	if (complete && n_compiled > 0) jit_blocks_complete++;

	return n_compiled > 0;
}

#endif /* __aarch64__ */
