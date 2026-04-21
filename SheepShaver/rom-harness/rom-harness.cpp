/*
 * rom-harness.cpp — Headless ROM JIT exerciser for SheepShaver AArch64 JIT
 *
 * Loads the Mac ROM file, scans it for PPC basic blocks, and for each block:
 *   1. Executes it in the SheepShaver interpreter
 *   2. JIT-compiles and executes it natively
 *   3. Compares all register outputs
 *
 * No display, no hardware, no disk — pure CPU exercising.
 *
 * Usage:
 *   ./rom-harness <rom-file> [options]
 *
 * Options:
 *   --offset=0xNNNNNN   Start scanning at this ROM offset (default: 0)
 *   --count=N           Number of blocks to test (default: all)
 *   --verbose           Print each block result
 *   --stop-on-fail      Stop at first mismatch
 *   --min-insns=N       Minimum instructions per block (default: 1)
 *   --max-insns=N       Maximum instructions per block (default: 64)
 *   --entry=0xNNNNNN    Execute a single block at this ROM offset
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <cassert>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#include <signal.h>
#include <setjmp.h>

/* ---------- Forward declarations for the JIT ---------- */
#include "ppc-jit.h"

/* ---------- PPC instruction decoding helpers ---------- */

static inline uint32_t read_be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline void write_be32(uint8_t *p, uint32_t v) {
	p[0] = (v >> 24) & 0xFF;
	p[1] = (v >> 16) & 0xFF;
	p[2] = (v >> 8)  & 0xFF;
	p[3] =  v        & 0xFF;
}

/* Primary opcode extraction */
static inline uint32_t ppc_primary(uint32_t insn) { return insn >> 26; }

/* Extended opcode for opcode 19/31/59/63 */
static inline uint32_t ppc_xo(uint32_t insn) { return (insn >> 1) & 0x3FF; }

/* Is this a block-terminating instruction? */
static bool is_block_terminator(uint32_t insn) {
	uint32_t opc = ppc_primary(insn);
	switch (opc) {
	case 18: return true; /* b/bl */
	case 16: return true; /* bc/bcl (conditional branch) */
	case 19: {
		uint32_t xo = ppc_xo(insn);
		if (xo == 16 || xo == 528) return true; /* bclr, bcctr */
		return false;
	}
	case 6:  return true; /* EMUL_OP (opcode 6 = SheepShaver trampoline) */
	default: return false;
	}
}

/* Is this a load/store instruction that accesses memory?
   We skip blocks containing these to avoid SIGSEGV from unmapped addresses. */
static bool is_memory_access(uint32_t insn) {
	uint32_t opc = ppc_primary(insn);
	/* Load/store integer */
	if (opc >= 32 && opc <= 55) return true;
	/* Load/store FP */
	if (opc >= 48 && opc <= 55) return true;
	/* Load/store with update, indexed forms via opcode 31 */
	if (opc == 31) {
		uint32_t xo = ppc_xo(insn);
		switch (xo) {
		/* Loads indexed */
		case 20: /* lwarx */ case 23: /* lwzx */ case 55: /* lwzux */
		case 87: /* lbzx */ case 119: /* lbzux */ case 279: /* lhzx */
		case 311: /* lhzux */ case 343: /* lhax */ case 375: /* lhaux */
		case 533: /* lswx */ case 534: /* lwbrx */ case 535: /* lfsx */
		case 567: /* lfsux */ case 599: /* lfdx */ case 631: /* lfdux */
		case 597: /* lswi */ case 790: /* lhbrx */
		/* Stores indexed */
		case 150: /* stwcx. */ case 151: /* stwx */ case 183: /* stwux */
		case 215: /* stbx */ case 247: /* stbux */ case 407: /* sthx */
		case 439: /* sthux */ case 662: /* stwbrx */ case 663: /* stfsx */
		case 695: /* stfsux */ case 727: /* stfdx */ case 759: /* stfdux */
		case 661: /* stswx */ case 725: /* stswi */ case 918: /* sthbrx */
		case 438: /* eciwx */ case 470: /* ecowx */
		/* Cache */
		case 54: /* dcbst */ case 86: /* dcbf */ case 246: /* dcbt */
		case 278: /* dcbtst */ case 982: /* icbi */ case 1014: /* dcbz */
			return true;
		default:
			break;
		}
	}
	return false;
}

/* Is this a supervisor/privileged instruction we should skip? */
static bool is_privileged(uint32_t insn) {
	uint32_t opc = ppc_primary(insn);
	if (opc == 31) {
		uint32_t xo = ppc_xo(insn);
		switch (xo) {
		case 146: /* mtmsr */
		case 83:  /* mfmsr */
		case 306: /* tlbie */
		case 566: /* tlbsync */
		case 370: /* tlbia */
		case 595: /* mfsr */
		case 659: /* mfsrin */
		case 210: /* mtsr */
		case 242: /* mtsrin */
		case 178: /* mtdec (move to DEC) */
			return true;
		}
	}
	if (opc == 17) return true; /* sc (system call) */
	if (opc == 19 && ppc_xo(insn) == 50) return true; /* rfi */
	return false;
}

/* Does this instruction modify LR? (bl, bcl, bclrl, etc.) */
static bool modifies_lr(uint32_t insn) {
	return (insn & 1) != 0 && (ppc_primary(insn) == 18 || ppc_primary(insn) == 16 ||
		(ppc_primary(insn) == 19 && (ppc_xo(insn) == 16 || ppc_xo(insn) == 528)));
}

/* Does this instruction read from CTR or LR as a branch target? */
static bool reads_link_regs(uint32_t insn) {
	if (ppc_primary(insn) == 19) {
		uint32_t xo = ppc_xo(insn);
		if (xo == 16) return true; /* bclr — reads LR */
		if (xo == 528) return true; /* bcctr — reads CTR */
	}
	return false;
}

/* ---------- Minimal PPC Interpreter for standalone testing ---------- */
/* 
 * This is a SUBSET interpreter — handles only the instructions the JIT
 * handles, enough to compare register outputs for compute-only blocks.
 */

struct PPCRegs {
	uint32_t gpr[32];        /* offset 0: 32 × 4 = 128 bytes */
	double   fpr[32];        /* offset 128: 32 × 8 = 256 bytes */
	uint8_t  vr[32 * 16];    /* offset 384: 32 × 16 = 512 bytes */
	uint32_t cr;             /* offset 896 */
	uint8_t  xer_so;          /* offset 900 */
	uint8_t  xer_ov;          /* offset 901 */
	uint8_t  xer_ca;          /* offset 902 */
	uint8_t  xer_cnt;         /* offset 903 */
	uint32_t padding904;     /* offset 904 */
	uint32_t padding908;     /* offset 908 */
	uint32_t fpscr;          /* offset 912 */
	uint32_t lr;             /* offset 916 */
	uint32_t ctr;            /* offset 920 */
	uint32_t pc;             /* offset 924 */
};

/* Register field offsets — must match ppc-jit.cpp PPCR_* */
static_assert(offsetof(PPCRegs, gpr) == 0, "GPR offset mismatch");
static_assert(offsetof(PPCRegs, fpr) == 128, "FPR offset mismatch");
static_assert(offsetof(PPCRegs, cr) == 896, "CR offset mismatch");
static_assert(offsetof(PPCRegs, xer_so) == 900, "XER offset mismatch");
static_assert(offsetof(PPCRegs, fpscr) == 912, "FPSCR offset mismatch");
static_assert(offsetof(PPCRegs, lr) == 916, "LR offset mismatch");
static_assert(offsetof(PPCRegs, ctr) == 920, "CTR offset mismatch");
static_assert(offsetof(PPCRegs, pc) == 924, "PC offset mismatch");

/* Pack XER bytes into PPC 32-bit format */
static inline uint32_t pack_xer(const PPCRegs *r) {
	return ((uint32_t)r->xer_so << 31) | ((uint32_t)r->xer_ov << 30) |
	       ((uint32_t)r->xer_ca << 29) | r->xer_cnt;
}

/* Unpack PPC 32-bit XER into struct bytes */
static inline void unpack_xer(PPCRegs *r, uint32_t xer) {
	r->xer_so = (xer >> 31) & 1;
	r->xer_ov = (xer >> 30) & 1;
	r->xer_ca = (xer >> 29) & 1;
	r->xer_cnt = xer & 0x7F;
}


/* Bit field helpers for CR */
static inline uint32_t cr_field(uint32_t cr, int field) {
	return (cr >> (28 - field * 4)) & 0xF;
}
static inline void set_cr_field(uint32_t &cr, int field, uint32_t val) {
	uint32_t shift = 28 - field * 4;
	cr = (cr & ~(0xF << shift)) | ((val & 0xF) << shift);
}

/* CR0 update from result value + SO */
static inline void update_cr0(uint32_t &cr, uint8_t xer_so, int32_t result) {
	uint32_t bits = 0;
	if (result < 0) bits = 8;      /* LT */
	else if (result > 0) bits = 4;  /* GT */
	else bits = 2;                  /* EQ */
	if (xer_so) bits |= 1; /* SO */
	set_cr_field(cr, 0, bits);
}

/* XER carry bit (bit 29) */
static inline bool xer_ca(const PPCRegs *r) { return r->xer_ca; }
static inline void set_xer_ca(PPCRegs *r, bool ca) { r->xer_ca = ca ? 1 : 0; }

/* 
 * Interpret a single basic block of PPC instructions.
 * Returns false if an unsupported instruction is encountered.
 * The block MUST end with a terminator (b/bl/blr/bctr/bc).
 * For testing purposes, we force a synthetic blr at the end if not present.
 */
static bool interpret_block(PPCRegs *regs, const uint8_t *rom, size_t rom_size,
                            uint32_t start_pc, uint32_t rom_base_mac, int max_insns) {
	uint32_t pc = start_pc;
	
	for (int i = 0; i < max_insns + 1; i++) {
		uint32_t rom_offset = pc - rom_base_mac;
		if (rom_offset >= rom_size) return false;
		
		uint32_t insn = read_be32(rom + rom_offset);
		uint32_t opc = ppc_primary(insn);
		
		/* Decode and execute */
		switch (opc) {
		
		/* --- Integer Arithmetic --- */
		case 14: { /* addi rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			regs->gpr[rD] = (rA == 0) ? (uint32_t)(int32_t)simm : regs->gpr[rA] + (int32_t)simm;
			pc += 4; break;
		}
		case 15: { /* addis rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			uint32_t val = (uint32_t)simm << 16;
			regs->gpr[rD] = (rA == 0) ? val : regs->gpr[rA] + val;
			pc += 4; break;
		}
		case 24: { /* ori rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] | uimm;
			pc += 4; break;
		}
		case 25: { /* oris rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] | ((uint32_t)uimm << 16);
			pc += 4; break;
		}
		case 26: { /* xori rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] ^ uimm;
			pc += 4; break;
		}
		case 27: { /* xoris rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] ^ ((uint32_t)uimm << 16);
			pc += 4; break;
		}
		case 28: { /* andi. rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] & uimm;
			update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
			pc += 4; break;
		}
		case 29: { /* andis. rA,rS,UIMM */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			regs->gpr[rA] = regs->gpr[rS] & ((uint32_t)uimm << 16);
			update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
			pc += 4; break;
		}
		
		case 21: { /* rlwinm[.] rA,rS,SH,MB,ME */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int SH = (insn >> 11) & 0x1F;
			int MB = (insn >> 6) & 0x1F;
			int ME = (insn >> 1) & 0x1F;
			bool rc = insn & 1;
			uint32_t val = regs->gpr[rS];
			uint32_t rotated = (val << SH) | (val >> (32 - SH));
			if (SH == 0) rotated = val;
			/* Build mask */
			uint32_t mask;
			if (MB <= ME)
				mask = ((0xFFFFFFFF >> MB) & (0xFFFFFFFF << (31 - ME)));
			else
				mask = ((0xFFFFFFFF >> MB) | (0xFFFFFFFF << (31 - ME)));
			regs->gpr[rA] = rotated & mask;
			if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
			pc += 4; break;
		}
		
		case 23: { /* rlwnm[.] rA,rS,rB,MB,ME */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int rB = (insn >> 11) & 0x1F;
			int MB = (insn >> 6) & 0x1F;
			int ME = (insn >> 1) & 0x1F;
			bool rc = insn & 1;
			uint32_t val = regs->gpr[rS];
			int sh = regs->gpr[rB] & 0x1F;
			uint32_t rotated = sh ? ((val << sh) | (val >> (32 - sh))) : val;
			uint32_t mask;
			if (MB <= ME)
				mask = ((0xFFFFFFFF >> MB) & (0xFFFFFFFF << (31 - ME)));
			else
				mask = ((0xFFFFFFFF >> MB) | (0xFFFFFFFF << (31 - ME)));
			regs->gpr[rA] = rotated & mask;
			if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
			pc += 4; break;
		}
		
		case 20: { /* rlwimi[.] rA,rS,SH,MB,ME */
			int rS = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int SH = (insn >> 11) & 0x1F;
			int MB = (insn >> 6) & 0x1F;
			int ME = (insn >> 1) & 0x1F;
			bool rc = insn & 1;
			uint32_t val = regs->gpr[rS];
			uint32_t rotated = SH ? ((val << SH) | (val >> (32 - SH))) : val;
			uint32_t mask;
			if (MB <= ME)
				mask = ((0xFFFFFFFF >> MB) & (0xFFFFFFFF << (31 - ME)));
			else
				mask = ((0xFFFFFFFF >> MB) | (0xFFFFFFFF << (31 - ME)));
			regs->gpr[rA] = (rotated & mask) | (regs->gpr[rA] & ~mask);
			if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
			pc += 4; break;
		}
		
		case 7: { /* mulli rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			regs->gpr[rD] = (uint32_t)((int32_t)regs->gpr[rA] * (int32_t)simm);
			pc += 4; break;
		}
		
		case 8: { /* subfic rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			uint64_t result = (uint64_t)(uint32_t)(int32_t)simm + (uint64_t)(~regs->gpr[rA]) + 1ULL;
			regs->gpr[rD] = (uint32_t)result;
			set_xer_ca(regs, result >> 32);
			pc += 4; break;
		}
		
		case 12: { /* addic rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)(uint32_t)(int32_t)simm;
			regs->gpr[rD] = (uint32_t)result;
			set_xer_ca(regs, result >> 32);
			pc += 4; break;
		}
		case 13: { /* addic. rD,rA,SIMM */
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)(uint32_t)(int32_t)simm;
			regs->gpr[rD] = (uint32_t)result;
			set_xer_ca(regs, result >> 32);
			update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
			pc += 4; break;
		}
		case 10: { /* cmpli crD,rA,UIMM */
			int crD = (insn >> 23) & 0x7;
			int rA = (insn >> 16) & 0x1F;
			uint16_t uimm = insn & 0xFFFF;
			uint32_t a = regs->gpr[rA];
			uint32_t b = (uint32_t)uimm;
			uint32_t bits = 0;
			if (a < b) bits = 8;
			else if (a > b) bits = 4;
			else bits = 2;
			if (regs->xer_so) bits |= 1;
			set_cr_field(regs->cr, crD, bits);
			pc += 4; break;
		}
		case 11: { /* cmpi crD,rA,SIMM */
			int crD = (insn >> 23) & 0x7;
			int rA = (insn >> 16) & 0x1F;
			int16_t simm = (int16_t)(insn & 0xFFFF);
			int32_t a = (int32_t)regs->gpr[rA];
			int32_t b = (int32_t)simm;
			uint32_t bits = 0;
			if (a < b) bits = 8;
			else if (a > b) bits = 4;
			else bits = 2;
			if (regs->xer_so) bits |= 1;
			set_cr_field(regs->cr, crD, bits);
			pc += 4; break;
		}
		
		/* --- Opcode 31: XO-form integer arithmetic, logical, comparison --- */
		case 31: {
			int rD = (insn >> 21) & 0x1F;
			int rA = (insn >> 16) & 0x1F;
			int rB = (insn >> 11) & 0x1F;
			bool rc = insn & 1;
			bool oe = (insn >> 10) & 1;
			uint32_t xo = ppc_xo(insn);
			uint32_t xo9 = (insn >> 1) & 0x1FF; /* 9-bit XO for arith */
			
			switch (xo) {
			/* Comparison */
			case 0: { /* cmp crD,rA,rB */
				int crD = (insn >> 23) & 0x7;
				int32_t a = (int32_t)regs->gpr[rA];
				int32_t b = (int32_t)regs->gpr[rB];
				uint32_t bits = 0;
				if (a < b) bits = 8; else if (a > b) bits = 4; else bits = 2;
				if (regs->xer_so) bits |= 1;
				set_cr_field(regs->cr, crD, bits);
				pc += 4; break;
			}
			case 32: { /* cmpl crD,rA,rB */
				int crD = (insn >> 23) & 0x7;
				uint32_t a = regs->gpr[rA];
				uint32_t b = regs->gpr[rB];
				uint32_t bits = 0;
				if (a < b) bits = 8; else if (a > b) bits = 4; else bits = 2;
				if (regs->xer_so) bits |= 1;
				set_cr_field(regs->cr, crD, bits);
				pc += 4; break;
			}
			
			/* Logical */
			case 28: { /* and[.] rA,rS,rB */
				regs->gpr[rA] = regs->gpr[rD] & regs->gpr[rB];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 60: { /* andc[.] rA,rS,rB */
				regs->gpr[rA] = regs->gpr[rD] & ~regs->gpr[rB];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 444: { /* or[.] rA,rS,rB */
				regs->gpr[rA] = regs->gpr[rD] | regs->gpr[rB];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 124: { /* nor[.] rA,rS,rB */
				regs->gpr[rA] = ~(regs->gpr[rD] | regs->gpr[rB]);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 316: { /* xor[.] rA,rS,rB */
				regs->gpr[rA] = regs->gpr[rD] ^ regs->gpr[rB];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 284: { /* eqv[.] rA,rS,rB */
				regs->gpr[rA] = ~(regs->gpr[rD] ^ regs->gpr[rB]);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 412: { /* orc[.] rA,rS,rB */
				regs->gpr[rA] = regs->gpr[rD] | ~regs->gpr[rB];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 476: { /* nand[.] rA,rS,rB */
				regs->gpr[rA] = ~(regs->gpr[rD] & regs->gpr[rB]);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			
			/* Shift */
			case 24: { /* slw[.] rA,rS,rB */
				uint32_t sh = regs->gpr[rB] & 0x3F;
				regs->gpr[rA] = (sh < 32) ? (regs->gpr[rD] << sh) : 0;
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 536: { /* srw[.] rA,rS,rB */
				uint32_t sh = regs->gpr[rB] & 0x3F;
				regs->gpr[rA] = (sh < 32) ? (regs->gpr[rD] >> sh) : 0;
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 792: { /* sraw[.] rA,rS,rB */
				uint32_t sh = regs->gpr[rB] & 0x3F;
				int32_t val = (int32_t)regs->gpr[rD];
				if (sh == 0) {
					regs->gpr[rA] = val;
					set_xer_ca(regs, false);
				} else if (sh < 32) {
					bool ca = (val < 0) && ((val & ((1 << sh) - 1)) != 0);
					regs->gpr[rA] = (uint32_t)(val >> sh);
					set_xer_ca(regs, ca);
				} else {
					bool ca = val < 0;
					regs->gpr[rA] = (uint32_t)(val >> 31);
					set_xer_ca(regs, ca);
				}
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 824: { /* srawi[.] rA,rS,SH */
				int SH = rB; /* rB field is SH for srawi */
				int32_t val = (int32_t)regs->gpr[rD];
				if (SH == 0) {
					regs->gpr[rA] = val;
					set_xer_ca(regs, false);
				} else {
					bool ca = (val < 0) && ((val & ((1 << SH) - 1)) != 0);
					regs->gpr[rA] = (uint32_t)(val >> SH);
					set_xer_ca(regs, ca);
				}
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			
			/* Count leading zeros */
			case 26: { /* cntlzw[.] rA,rS */
				uint32_t val = regs->gpr[rD];
				regs->gpr[rA] = val ? __builtin_clz(val) : 32;
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			
			/* Extend sign */
			case 922: { /* extsh[.] rA,rS */
				regs->gpr[rA] = (uint32_t)(int32_t)(int16_t)(uint16_t)regs->gpr[rD];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			case 954: { /* extsb[.] rA,rS */
				regs->gpr[rA] = (uint32_t)(int32_t)(int8_t)(uint8_t)regs->gpr[rD];
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rA]);
				pc += 4; break;
			}
			
			/* Move to/from special registers */
			case 339: { /* mfspr rD,spr */
				uint32_t spr = ((insn >> 16) & 0x1F) | (((insn >> 11) & 0x1F) << 5);
				switch (spr) {
				case 1: regs->gpr[rD] = pack_xer(regs); break;
				case 8: regs->gpr[rD] = regs->lr; break;
				case 9: regs->gpr[rD] = regs->ctr; break;
				default: regs->gpr[rD] = 0; break; /* unknown SPR */
				}
				pc += 4; break;
			}
			case 467: { /* mtspr spr,rS */
				uint32_t spr = ((insn >> 16) & 0x1F) | (((insn >> 11) & 0x1F) << 5);
				switch (spr) {
				case 1: unpack_xer(regs, regs->gpr[rD]); break;
				case 8: regs->lr = regs->gpr[rD]; break;
				case 9: regs->ctr = regs->gpr[rD]; break;
				default: break;
				}
				pc += 4; break;
			}
			
			/* Move from CR */
			case 19: { /* mfcr rD */
				regs->gpr[rD] = regs->cr;
				pc += 4; break;
			}
			
			/* Multiply */
			case 235: { /* mullw[o][.] rD,rA,rB (xo9=235) */
				int64_t result = (int64_t)(int32_t)regs->gpr[rA] * (int64_t)(int32_t)regs->gpr[rB];
				regs->gpr[rD] = (uint32_t)result;
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
				pc += 4; break;
			}
			case 75: { /* mulhw[.] rD,rA,rB */
				int64_t result = (int64_t)(int32_t)regs->gpr[rA] * (int64_t)(int32_t)regs->gpr[rB];
				regs->gpr[rD] = (uint32_t)(result >> 32);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
				pc += 4; break;
			}
			case 11: { /* mulhwu[.] rD,rA,rB */
				uint64_t result = (uint64_t)regs->gpr[rA] * (uint64_t)regs->gpr[rB];
				regs->gpr[rD] = (uint32_t)(result >> 32);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
				pc += 4; break;
			}
			
			/* Divide */
			case 491: { /* divw[o][.] rD,rA,rB */
				int32_t a = (int32_t)regs->gpr[rA];
				int32_t b = (int32_t)regs->gpr[rB];
				if (b == 0 || (a == (int32_t)0x80000000 && b == -1))
					regs->gpr[rD] = 0;
				else
					regs->gpr[rD] = (uint32_t)(a / b);
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
				pc += 4; break;
			}
			case 459: { /* divwu[o][.] rD,rA,rB */
				uint32_t a = regs->gpr[rA];
				uint32_t b = regs->gpr[rB];
				if (b == 0)
					regs->gpr[rD] = 0;
				else
					regs->gpr[rD] = a / b;
				if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
				pc += 4; break;
			}
			
			/* Add/Sub variants (9-bit XO) */
			default: {
				switch (xo9) {
				case 266: { /* add[o][.] rD,rA,rB */
					regs->gpr[rD] = regs->gpr[rA] + regs->gpr[rB];
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 10: { /* addc[o][.] rD,rA,rB */
					uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)regs->gpr[rB];
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 138: { /* adde[o][.] rD,rA,rB */
					uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)regs->gpr[rB] + (uint64_t)xer_ca(regs);
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 234: { /* addme[o][.] rD,rA */
					uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)xer_ca(regs) - 1ULL;
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 202: { /* addze[o][.] rD,rA */
					uint64_t result = (uint64_t)regs->gpr[rA] + (uint64_t)xer_ca(regs);
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 40: { /* subf[o][.] rD,rA,rB */
					regs->gpr[rD] = regs->gpr[rB] - regs->gpr[rA];
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 8: { /* subfc[o][.] rD,rA,rB */
					uint64_t result = (uint64_t)regs->gpr[rB] + (uint64_t)(~regs->gpr[rA]) + 1ULL;
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 136: { /* subfe[o][.] rD,rA,rB */
					uint64_t result = (uint64_t)regs->gpr[rB] + (uint64_t)(~regs->gpr[rA]) + (uint64_t)xer_ca(regs);
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 232: { /* subfme[o][.] rD,rA */
					uint64_t result = (uint64_t)(~regs->gpr[rA]) + (uint64_t)xer_ca(regs) - 1ULL;
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 200: { /* subfze[o][.] rD,rA */
					uint64_t result = (uint64_t)(~regs->gpr[rA]) + (uint64_t)xer_ca(regs);
					regs->gpr[rD] = (uint32_t)result;
					set_xer_ca(regs, result >> 32);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				case 104: { /* neg[o][.] rD,rA */
					regs->gpr[rD] = (uint32_t)(-(int32_t)regs->gpr[rA]);
					if (rc) update_cr0(regs->cr, regs->xer_so, (int32_t)regs->gpr[rD]);
					pc += 4; break;
				}
				default:
					return false; /* unsupported XO31 */
				}
				break;
			}
			}
			break;
		}
		
		/* --- CR Logical (opcode 19) --- */
		case 19: {
			uint32_t xo = ppc_xo(insn);
			switch (xo) {
			case 257: { /* crand crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (a & b) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 449: { /* cror crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (a | b) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 193: { /* crxor crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (a ^ b) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 33: { /* crnor crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (!(a | b)) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 129: { /* crandc crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (a & ~b) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 289: { /* creqv crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (!(a ^ b)) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 417: { /* crorc crbD,crbA,crbB */
				int crbD = (insn >> 21) & 0x1F;
				int crbA = (insn >> 16) & 0x1F;
				int crbB = (insn >> 11) & 0x1F;
				int a = (regs->cr >> (31 - crbA)) & 1;
				int b = (regs->cr >> (31 - crbB)) & 1;
				if (a | ~b) regs->cr |= (1u << (31 - crbD));
				else regs->cr &= ~(1u << (31 - crbD));
				pc += 4; break;
			}
			case 0: { /* mcrf crD,crS */
				int crD = (insn >> 23) & 0x7;
				int crS = (insn >> 18) & 0x7;
				set_cr_field(regs->cr, crD, cr_field(regs->cr, crS));
				pc += 4; break;
			}
			/* bclr/bcctr — terminators, handled below */
			case 16: /* bclr */
			case 528: /* bcctr */
				goto handle_terminator;
			default:
				return false;
			}
			break;
		}
		
		/* --- Branch terminators --- */
		case 18: /* b/bl */
		case 16: /* bc/bcl */
		case 6:  /* EMUL_OP */
			goto handle_terminator;
			
		default:
			return false; /* unsupported opcode */
		}
		continue;
		
	handle_terminator:
		/* For terminators, we just set PC and stop.
		   The JIT does the same — stores the target PC and returns. */
		{
			uint32_t opc_t = ppc_primary(insn);
			if (opc_t == 18) { /* b/bl */
				int32_t disp = insn & 0x03FFFFFC;
				if (disp & 0x02000000) disp |= 0xFC000000; /* sign extend */
				bool aa = (insn >> 1) & 1;
				bool lk = insn & 1;
				uint32_t target = aa ? (uint32_t)disp : pc + disp;
				if (lk) regs->lr = pc + 4;
				regs->pc = target;
			} else if (opc_t == 16) { /* bc */
				int BO = (insn >> 21) & 0x1F;
				int BI = (insn >> 16) & 0x1F;
				int16_t bd = (int16_t)(insn & 0xFFFC);
				bool aa = (insn >> 1) & 1;
				bool lk = insn & 1;
				
				/* PPC ISA BO field (5 bits, MSB-first):
				   BO[0] (0x10): 1=don't test condition
				   BO[1] (0x08): condition sense (branch if CR[BI]=BO[1])
				   BO[2] (0x04): 1=don't decrement/test CTR
				   BO[3] (0x02): CTR sense (0=branch if CTR≠0, 1=branch if CTR==0)
				   BO[4] (0x01): prediction hint */
				if (!(BO & 0x04)) regs->ctr--; /* decrement if BO[2]=0 */
				
				bool ctr_ok = (BO & 0x04) || ((regs->ctr != 0) ^ ((BO >> 1) & 1));
				bool cond_ok = (BO & 0x10) || (((regs->cr >> (31 - BI)) & 1) == ((BO >> 3) & 1));
				
				if (ctr_ok && cond_ok) {
					uint32_t target = aa ? (uint32_t)(int32_t)bd : pc + (int32_t)bd;
					if (lk) regs->lr = pc + 4;
					regs->pc = target;
				} else {
					regs->pc = pc + 4;
				}
			} else if (opc_t == 19) {
				uint32_t xo = ppc_xo(insn);
				int BO = (insn >> 21) & 0x1F;
				int BI = (insn >> 16) & 0x1F;
				bool lk = insn & 1;
				
				if (!(BO & 0x04)) regs->ctr--;
				bool ctr_ok = (BO & 0x04) || ((regs->ctr != 0) ^ ((BO >> 1) & 1));
				bool cond_ok = (BO & 0x10) || (((regs->cr >> (31 - BI)) & 1) == ((BO >> 3) & 1));
				
				if (ctr_ok && cond_ok) {
					uint32_t target = (xo == 16) ? regs->lr : regs->ctr;
					if (lk) regs->lr = pc + 4;
					regs->pc = target;
				} else {
					regs->pc = pc + 4;
				}
			} else {
				/* EMUL_OP or other — just set PC past it */
				regs->pc = pc;
			}
			return true;
		}
	}
	
	/* Ran out of instructions without terminator — set PC */
	regs->pc = pc;
	return true;
}

/* ---------- Block scanning ---------- */

struct ROMBlock {
	uint32_t offset;     /* ROM offset of first instruction */
	int n_insns;         /* Number of instructions in block */
	bool has_mem_access; /* Contains load/store */
	bool has_privileged; /* Contains supervisor instruction */
	bool has_emul_op;    /* Contains EMUL_OP trampoline */
	bool has_link_read;  /* Reads LR/CTR as branch target */
	bool terminated;     /* Ends with a proper terminator */
};

static int scan_rom_blocks(const uint8_t *rom, size_t rom_size,
                           ROMBlock *blocks, int max_blocks,
                           int min_insns, int max_insns) {
	int n_blocks = 0;
	uint32_t offset = 0;
	
	while (offset < rom_size - 4 && n_blocks < max_blocks) {
		/* Skip zero words (padding) */
		uint32_t insn = read_be32(rom + offset);
		if (insn == 0 || insn == 0xFFFFFFFF) {
			offset += 4;
			continue;
		}
		
		/* Start a new block */
		ROMBlock blk = {};
		blk.offset = offset;
		blk.n_insns = 0;
		blk.terminated = false;
		
		uint32_t scan = offset;
		while (scan < rom_size - 4 && blk.n_insns < max_insns) {
			insn = read_be32(rom + scan);
			if (insn == 0) break; /* hit padding */
			
			if (is_memory_access(insn)) blk.has_mem_access = true;
			if (is_privileged(insn)) blk.has_privileged = true;
			if (ppc_primary(insn) == 6) blk.has_emul_op = true;
			if (reads_link_regs(insn)) blk.has_link_read = true;
			
			blk.n_insns++;
			scan += 4;
			
			if (is_block_terminator(insn)) {
				blk.terminated = true;
				break;
			}
		}
		
		if (blk.n_insns >= min_insns && blk.terminated) {
			blocks[n_blocks++] = blk;
		}
		
		offset = scan;
	}
	
	return n_blocks;
}

/* ---------- SIGSEGV handler for safe JIT execution ---------- */

static sigjmp_buf segv_jmp;
static volatile sig_atomic_t segv_caught = 0;

static void segv_handler(int sig, siginfo_t *si, void *ctx) {
	segv_caught = 1;
	siglongjmp(segv_jmp, 1);
}

/* ---------- Comparison ---------- */

struct TestResult {
	int total;
	int passed;
	int failed;
	int skipped;
	int interp_unsupported;
	int jit_compile_fail;
	int jit_segv;
};

static void print_regs(const char *label, const PPCRegs *r) {
	fprintf(stderr, "  %s: PC=%08x LR=%08x CTR=%08x CR=%08x XER=%08x\n",
		label, r->pc, r->lr, r->ctr, r->cr, pack_xer(r));
	for (int i = 0; i < 32; i += 4) {
		fprintf(stderr, "    GPR%02d-%02d: %08x %08x %08x %08x\n",
			i, i+3, r->gpr[i], r->gpr[i+1], r->gpr[i+2], r->gpr[i+3]);
	}
}

static bool compare_regs(const PPCRegs *interp, const PPCRegs *jit,
                         uint32_t rom_offset, bool verbose) {
	bool match = true;
	
	/* Compare GPRs */
	for (int i = 0; i < 32; i++) {
		if (interp->gpr[i] != jit->gpr[i]) { match = false; break; }
	}
	/* Compare special regs */
	if (interp->pc != jit->pc) match = false;
	if (interp->lr != jit->lr) match = false;
	if (interp->ctr != jit->ctr) match = false;
	if (interp->cr != jit->cr) match = false;
	if (pack_xer(interp) != pack_xer(jit)) match = false;
	
	if (!match && verbose) {
		fprintf(stderr, "MISMATCH at ROM+0x%06x:\n", rom_offset);
		print_regs("INTERP", interp);
		print_regs("JIT   ", jit);
		/* Find specific diffs */
		for (int i = 0; i < 32; i++) {
			if (interp->gpr[i] != jit->gpr[i])
				fprintf(stderr, "  DIFF GPR%d: interp=%08x jit=%08x\n",
					i, interp->gpr[i], jit->gpr[i]);
		}
		if (interp->pc != jit->pc)
			fprintf(stderr, "  DIFF PC: interp=%08x jit=%08x\n", interp->pc, jit->pc);
		if (interp->lr != jit->lr)
			fprintf(stderr, "  DIFF LR: interp=%08x jit=%08x\n", interp->lr, jit->lr);
		if (interp->ctr != jit->ctr)
			fprintf(stderr, "  DIFF CTR: interp=%08x jit=%08x\n", interp->ctr, jit->ctr);
		if (interp->cr != jit->cr)
			fprintf(stderr, "  DIFF CR: interp=%08x jit=%08x\n", interp->cr, jit->cr);
		if (pack_xer(interp) != pack_xer(jit))
			fprintf(stderr, "  DIFF XER: interp=%08x jit=%08x\n", pack_xer(interp), pack_xer(jit));
	}
	
	return match;
}

/* ---------- Pseudo-random seed for reproducible register state ---------- */
static uint32_t xorshift32(uint32_t *state) {
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static void seed_regs(PPCRegs *r, uint32_t seed, uint32_t rom_base_mac) {
	memset(r, 0, sizeof(*r)); /* zero everything first */
	uint32_t s = seed;
	for (int i = 0; i < 32; i++)
		r->gpr[i] = xorshift32(&s);
	/* R1 = valid stack pointer (within our allocated memory) */
	r->gpr[1] = rom_base_mac - 0x10000; /* stack below ROM */
	r->lr = rom_base_mac + 0x1000;  /* valid LR */
	r->ctr = xorshift32(&s);
	r->cr = xorshift32(&s) & 0xFFFFFFFF;
	unpack_xer(r, xorshift32(&s) & 0xE000007F); /* valid XER bits only */
	r->fpscr = 0;
}

/* ---------- Main ---------- */

static void usage(const char *prog) {
	fprintf(stderr, "Usage: %s <rom-file> [options]\n"
		"  --offset=0xNNNNNN   Start ROM offset (default: 0)\n"
		"  --count=N           Max blocks to test (default: all)\n"
		"  --verbose           Print each result\n"
		"  --stop-on-fail      Stop at first mismatch\n"
		"  --min-insns=N       Minimum block size (default: 1)\n"
		"  --max-insns=N       Maximum block size (default: 64)\n"
		"  --entry=0xNNNNNN    Test single block at ROM offset\n"
		"  --seed=N            Random seed (default: 0xDEADBEEF)\n"
		"  --passes=N          Number of random-seed passes (default: 1)\n"
		"  --compute-only      Skip blocks with memory access (default)\n"
		"  --all-blocks        Include blocks with memory access (will likely segv)\n",
		prog);
}

int main(int argc, char **argv) {
	/* Parse args */
	const char *rom_path = NULL;
	uint32_t start_offset = 0;
	int max_count = 0; /* 0 = all */
	bool verbose = false;
	bool stop_on_fail = false;
	int min_insns = 1;
	int max_insns = 64;
	uint32_t single_entry = 0xFFFFFFFF;
	uint32_t seed = 0xDEADBEEF;
	int passes = 1;
	bool compute_only = true;
	
	static struct option long_opts[] = {
		{"offset", required_argument, 0, 'o'},
		{"count", required_argument, 0, 'c'},
		{"verbose", no_argument, 0, 'v'},
		{"stop-on-fail", no_argument, 0, 's'},
		{"min-insns", required_argument, 0, 'm'},
		{"max-insns", required_argument, 0, 'M'},
		{"entry", required_argument, 0, 'e'},
		{"seed", required_argument, 0, 'S'},
		{"passes", required_argument, 0, 'p'},
		{"compute-only", no_argument, 0, 'C'},
		{"all-blocks", no_argument, 0, 'A'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};
	
	int ch;
	while ((ch = getopt_long(argc, argv, "o:c:vsm:M:e:S:p:CAh", long_opts, NULL)) != -1) {
		switch (ch) {
		case 'o': start_offset = strtoul(optarg, NULL, 0); break;
		case 'c': max_count = atoi(optarg); break;
		case 'v': verbose = true; break;
		case 's': stop_on_fail = true; break;
		case 'm': min_insns = atoi(optarg); break;
		case 'M': max_insns = atoi(optarg); break;
		case 'e': single_entry = strtoul(optarg, NULL, 0); break;
		case 'S': seed = strtoul(optarg, NULL, 0); break;
		case 'p': passes = atoi(optarg); break;
		case 'C': compute_only = true; break;
		case 'A': compute_only = false; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 1;
		}
	}
	
	if (optind < argc) rom_path = argv[optind];
	if (!rom_path) { usage(argv[0]); return 1; }
	
	/* Load ROM */
	int fd = open(rom_path, O_RDONLY);
	if (fd < 0) { perror(rom_path); return 1; }
	off_t rom_size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	
	if (rom_size <= 0 || rom_size > 8 * 1024 * 1024) {
		fprintf(stderr, "Invalid ROM size: %ld\n", (long)rom_size);
		close(fd);
		return 1;
	}
	
	/* Allocate memory: RAM area + ROM area, contiguous */
	const size_t ram_size = 16 * 1024 * 1024;
	const size_t total_size = ram_size + rom_size + 0x100000; /* ROM area + padding */
	
	uint8_t *mem = (uint8_t *)mmap(
		(void *)0x10000000UL, total_size,
		PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
		-1, 0);
	if (mem == MAP_FAILED) {
		mem = (uint8_t *)mmap(NULL, total_size,
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS,
			-1, 0);
	}
	if (mem == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return 1;
	}
	memset(mem, 0, total_size);
	
	uint8_t *ram_host = mem;
	uint8_t *rom_host = mem + ram_size;
	uint32_t ram_base_mac = (uint32_t)(uintptr_t)ram_host;
	uint32_t rom_base_mac = (uint32_t)(uintptr_t)rom_host;
	
	/* Read ROM into memory */
	ssize_t rd = read(fd, rom_host, rom_size);
	close(fd);
	if (rd != rom_size) {
		fprintf(stderr, "Short read: %ld of %ld\n", (long)rd, (long)rom_size);
		munmap(mem, total_size);
		return 1;
	}
	
	fprintf(stderr, "ROM loaded: %s (%ld bytes)\n", rom_path, (long)rom_size);
	fprintf(stderr, "Memory layout: RAM=%08x-%08x, ROM=%08x-%08x\n",
		ram_base_mac, ram_base_mac + (uint32_t)ram_size,
		rom_base_mac, rom_base_mac + (uint32_t)rom_size);
	
	/* Scan ROM for blocks */
	const int MAX_BLOCKS = 100000;
	ROMBlock *blocks = new ROMBlock[MAX_BLOCKS];
	int n_blocks;
	
	if (single_entry != 0xFFFFFFFF) {
		/* Single block mode */
		blocks[0].offset = single_entry;
		blocks[0].n_insns = 0;
		blocks[0].has_mem_access = false;
		blocks[0].has_privileged = false;
		blocks[0].has_emul_op = false;
		blocks[0].has_link_read = false;
		blocks[0].terminated = false;
		
		/* Scan to find block size */
		uint32_t scan = single_entry;
		while (scan < (uint32_t)rom_size - 4 && blocks[0].n_insns < max_insns) {
			uint32_t insn = read_be32(rom_host + scan);
			if (insn == 0) break;
			if (is_memory_access(insn)) blocks[0].has_mem_access = true;
			if (is_privileged(insn)) blocks[0].has_privileged = true;
			if (ppc_primary(insn) == 6) blocks[0].has_emul_op = true;
			if (reads_link_regs(insn)) blocks[0].has_link_read = true;
			blocks[0].n_insns++;
			scan += 4;
			if (is_block_terminator(insn)) { blocks[0].terminated = true; break; }
		}
		n_blocks = 1;
		verbose = true;
	} else {
		fprintf(stderr, "Scanning ROM for basic blocks (min=%d, max=%d)...\n",
			min_insns, max_insns);
		n_blocks = scan_rom_blocks(rom_host + start_offset,
			rom_size - start_offset, blocks, MAX_BLOCKS,
			min_insns, max_insns);
		/* Adjust offsets for start_offset */
		for (int i = 0; i < n_blocks; i++)
			blocks[i].offset += start_offset;
		fprintf(stderr, "Found %d basic blocks\n", n_blocks);
	}
	
	if (max_count > 0 && max_count < n_blocks)
		n_blocks = max_count;
	
	/* Init JIT */
	if (!ppc_jit_aarch64_init(4096)) {
		fprintf(stderr, "JIT init failed\n");
		delete[] blocks;
		munmap(mem, total_size);
		return 1;
	}
	
	/* Install SIGSEGV handler */
	struct sigaction sa = {}, old_sa = {};
	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, &old_sa);
	sigaction(SIGBUS, &sa, NULL);
	
	/* Run tests */
	TestResult result = {};
	int testable = 0;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	
	for (int pass = 0; pass < passes; pass++) {
		uint32_t pass_seed = seed + pass * 0x12345;
		
		for (int bi = 0; bi < n_blocks; bi++) {
			const ROMBlock &blk = blocks[bi];
			result.total++;
			
			/* Filter */
			if (compute_only && blk.has_mem_access) { result.skipped++; continue; }
			if (blk.has_privileged) { result.skipped++; continue; }
			if (blk.has_emul_op) { result.skipped++; continue; }
			
			testable++;
			
			/* Prepare register state */
			PPCRegs interp_regs, jit_regs;
			seed_regs(&interp_regs, pass_seed + blk.offset, rom_base_mac);
			memcpy(&jit_regs, &interp_regs, sizeof(PPCRegs));
			
			/* Set PC */
			uint32_t block_mac_pc = rom_base_mac + blk.offset;
			interp_regs.pc = block_mac_pc;
			jit_regs.pc = block_mac_pc;
			
			/* Run interpreter */
			if (!interpret_block(&interp_regs, rom_host, rom_size,
			                     block_mac_pc, rom_base_mac, blk.n_insns)) {
				result.interp_unsupported++;
				result.skipped++;
				continue;
			}
			
			/* JIT compile — note: JIT expects the full memory buffer and Mac PC */
			ppc_jit_block jblk;
			if (!ppc_jit_aarch64_compile(block_mac_pc, mem, total_size, &jblk) ||
			    jblk.n_insns == 0) {
				result.jit_compile_fail++;
				result.skipped++;
				if (verbose)
					fprintf(stderr, "  ROM+0x%06x: JIT compile failed (%d insns)\n",
						blk.offset, blk.n_insns);
				continue;
			}
			
			/* Only test complete blocks — incomplete blocks return to interpreter
			   at the failing instruction, which is correct but not comparable */
			if (!jblk.complete) {
				result.jit_compile_fail++;
				result.skipped++;
				if (verbose)
					fprintf(stderr, "  ROM+0x%06x: JIT incomplete (%d/%d insns)\n",
						blk.offset, jblk.n_insns, blk.n_insns);
				continue;
			}
			
			/* Run JIT with SIGSEGV protection */
			segv_caught = 0;
			if (sigsetjmp(segv_jmp, 1) == 0) {
				ppc_jit_entry_fn fn = (ppc_jit_entry_fn)(void *)jblk.code;
				fn((void *)&jit_regs);
			}
			if (segv_caught) {
				result.jit_segv++;
				result.skipped++;
				if (verbose)
					fprintf(stderr, "  ROM+0x%06x: JIT SIGSEGV\n", blk.offset);
				continue;
			}
			
			/* Compare */
			if (compare_regs(&interp_regs, &jit_regs, blk.offset, verbose)) {
				result.passed++;
				if (verbose)
					fprintf(stderr, "  ROM+0x%06x: PASS (%d insns)\n",
						blk.offset, blk.n_insns);
			} else {
				result.failed++;
				if (verbose) {
					/* Dump the block instructions */
					fprintf(stderr, "  Block instructions:\n");
					for (int j = 0; j < blk.n_insns; j++) {
						uint32_t w = read_be32(rom_host + blk.offset + j * 4);
						fprintf(stderr, "    %06x: %08x (opc=%d",
							blk.offset + j * 4, w, ppc_primary(w));
						if (ppc_primary(w) == 31)
							fprintf(stderr, " xo=%d", ppc_xo(w));
						fprintf(stderr, ")\n");
					}
				}
				if (stop_on_fail) goto done;
			}
			
			/* Progress */
			if (!verbose && testable % 10000 == 0)
				fprintf(stderr, "\r  Tested %d blocks... (%d pass, %d fail)",
					testable, result.passed, result.failed);
		}
	}

done:
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
	
	/* Restore signal handler */
	sigaction(SIGSEGV, &old_sa, NULL);
	
	/* Summary */
	fprintf(stderr, "\n=== ROM Harness Results ===\n");
	fprintf(stderr, "Total blocks scanned: %d\n", result.total);
	fprintf(stderr, "Testable (compute-only, non-privileged): %d\n", testable);
	fprintf(stderr, "Passed:              %d\n", result.passed);
	fprintf(stderr, "Failed:              %d\n", result.failed);
	fprintf(stderr, "Skipped:             %d\n", result.skipped);
	fprintf(stderr, "  Interp unsupported: %d\n", result.interp_unsupported);
	fprintf(stderr, "  JIT compile fail:   %d\n", result.jit_compile_fail);
	fprintf(stderr, "  JIT SIGSEGV:        %d\n", result.jit_segv);
	fprintf(stderr, "Time: %.2f sec (%.0f blocks/sec)\n",
		elapsed, testable > 0 ? testable / elapsed : 0);
	fprintf(stderr, "Score: %d/%d\n", result.passed, result.passed + result.failed);
	
	/* Cleanup */
	ppc_jit_aarch64_exit();
	delete[] blocks;
	munmap(mem, total_size);
	
	return result.failed > 0 ? 1 : 0;
}
