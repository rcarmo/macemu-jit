/*
 *  ppc-jit-aarch64.cpp — PPC → AArch64 direct codegen JIT
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
#include "ppc-jit-aarch64.h"
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

/* Emit: store next_pc to regs->pc, epilogue, ret */
static void emit_epilogue_with_pc(uint32_t next_pc) {
	emit_load_imm32(RTMP0, (int32_t)next_pc);
	a64_str_w_imm(RTMP0, RSTATE, PPCR_PC);
	/* Restore callee-saved regs and return */
	a64_ldp_post(RSTATE, 21, A64_SP, 16);
	a64_ldp_post(A64_FP, A64_LR, A64_SP, 16);
	a64_ret();
}

/* ---- Compile one PPC instruction ---- */
static bool compile_one(uint32_t op) {
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
		case 266: /* add */
			emit_load_gpr(RTMP0, ra);
			emit_load_gpr(RTMP1, rb);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, rd);
			return true;
		case 40: /* subf (rD = rB - rA) */
			emit_load_gpr(RTMP0, rb);
			emit_load_gpr(RTMP1, ra);
			emit32(0x4B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* SUB */
			emit_store_gpr(RTMP0, rd);
			return true;
		case 28: /* and */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x0A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			return true;
		case 444: /* or (also mr) */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x2A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			return true;
		case 316: /* xor */
			emit_load_gpr(RTMP0, PPC_RS(op));
			emit_load_gpr(RTMP1, rb);
			emit32(0x4A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0);
			emit_store_gpr(RTMP0, ra);
			return true;
		case 104: /* neg */
			emit_load_gpr(RTMP0, ra);
			emit32(0x4B0003E0 | (RTMP0 << 16) | RTMP0); /* SUB Wd, WZR, Wn = NEG */
			emit_store_gpr(RTMP0, rd);
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
			return false;
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
			return false;
		}
		default:
			return false;
		}
	}

	case 32: /* lwz rD,d(rA) */
		rd = PPC_RD(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		if (ra == 0) {
			/* lwz rD, d(0) = load from absolute address d — rare in test code */
			return false;
		}
		emit_load_gpr(RTMP0, ra);                /* base address */
		if (simm) {
			emit_load_imm32(RTMP1, (int32_t)simm);
			emit32(0x0B000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADD */
		}
		/* LDR W(RTMP1), [X(RTMP0)] — load 32-bit from host address */
		emit32(0xB9400000 | (RTMP0 << 5) | RTMP1); /* LDR Wt, [Xn] */
		/* Byte-swap: PPC is big-endian, ARM64 is little-endian */
		emit32(0x5AC00800 | (RTMP1 << 5) | RTMP1); /* REV Wd, Wn */
		emit_store_gpr(RTMP1, rd);
		return true;

	case 36: /* stw rS,d(rA) */
		rd = PPC_RS(op); ra = PPC_RA(op); simm = PPC_SIMM(op);
		if (ra == 0) return false;
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

	default:
		return false;
	}
}

/* ---- Public API ---- */

bool ppc_jit_aarch64_init(size_t cache_size_kb)
{
	jit_cache_size = cache_size_kb * 1024;
	jit_cache_base = (uint8_t *)jit_cache_alloc(jit_cache_size);
	if (!jit_cache_base) {
		fprintf(stderr, "PPC-JIT-A64: failed to allocate %zu KB code cache\n", cache_size_kb);
		return false;
	}
	jit_cache_wp = (uint32_t *)jit_cache_base;
	jit_cache_end = (uint32_t *)(jit_cache_base + jit_cache_size);
	fprintf(stderr, "PPC-JIT-A64: code cache %zu KB at %p\n", cache_size_kb, jit_cache_base);
	return true;
}

void ppc_jit_aarch64_exit(void)
{
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
		return false;

	uint32_t *code_start = jit_cache_wp;
	jit_code_ptr = jit_cache_wp;

	/* Prologue: save callee-saved regs, set x20 = regs ptr from x0 */
	a64_stp_pre(A64_FP, A64_LR, A64_SP, -16);
	a64_stp_pre(RSTATE, 21, A64_SP, -16);
	a64_mov_reg(RSTATE, A64_X0);

	uint32_t cur_pc = pc;
	int n_compiled = 0;
	bool complete = true;

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

		if (op == 0x00000000) { /* illegal — end of test code */
			emit_epilogue_with_pc(cur_pc);
			n_compiled++;
			cur_pc += 4;
			break;
		}

		if (!compile_one(op)) {
			emit_epilogue_with_pc(cur_pc);
			complete = false;
			break;
		}

		n_compiled++;
		cur_pc += 4;
	}

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
	out->complete = complete;

	return n_compiled > 0;
}

#endif /* __aarch64__ */
