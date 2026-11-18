/*
 * compiler/codegen_arm.cpp - AARCH64 code generator
 *
 * Copyright (c) 2019 TomB
 * 
 * This file is part of the UAE4ARM project.
 *
 * JIT compiler m68k -> ARMv8.0
 *
 * Original 68040 JIT compiler for UAE, copyright 2000-2002 Bernd Meyer
 * This file is derived from CCG, copyright 1999-2003 Ian Piumarta
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Note: JIT for aarch64 only works if memory for Amiga part (regs.natmen_offset
 *       to additional_mem + ADDITIONAL_MEMSIZE + BARRIER) is allocated in lower
 *       32 bit address space. See alloc_AmigaMem() in generic_mem.cpp.
 */

#include "flags_arm.h"
#include <cmath>

/*************************************************************************
 * Some basic information about the the target CPU                       *
 *************************************************************************/

#define R0_INDEX 0
#define R1_INDEX 1
#define R2_INDEX 2
#define R3_INDEX 3
#define R4_INDEX 4
#define R5_INDEX 5
#define R6_INDEX 6
#define R7_INDEX 7
#define R8_INDEX  8
#define R9_INDEX  9
#define R10_INDEX 10
#define R11_INDEX 11
#define R12_INDEX 12
#define R13_INDEX 13
#define R14_INDEX 14
#define R15_INDEX 15
#define R16_INDEX 16
#define R17_INDEX 17
#define R18_INDEX 18
#define R27_INDEX 27
#define R28_INDEX 28

#define RSP_INDEX 31
#define RLR_INDEX 30
#define RFP_INDEX 29

/* The register in which subroutines return an integer return value */
#define REG_RESULT R0_INDEX

/* The registers subroutines take their first and second argument in */
#define REG_PAR1 R0_INDEX
#define REG_PAR2 R1_INDEX

#define REG_WORK1 R2_INDEX
#define REG_WORK2 R3_INDEX
#define REG_WORK3 R4_INDEX
#define REG_WORK4 R5_INDEX

#define REG_PC_TMP R1_INDEX /* Another register that is not the above */

#define R_MEMSTART 27
#define R_REGSTRUCT 28
uae_s8 always_used[] = {2,3,4,5,18,R_MEMSTART,R_REGSTRUCT,-1}; // r2-r5 are work register in emitted code, r18 special use reg

uae_u8 call_saved[] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1, 1,1,1,1, 1,1,1,1, 1,0,0,0};

/* This *should* be the same as call_saved. But:
   - We might not really know which registers are saved, and which aren't,
	 so we need to preserve some, but don't want to rely on everyone else
	 also saving those registers
   - Special registers (such like the stack pointer) should not be "preserved"
	 by pushing, even though they are "saved" across function calls
   - r19 - r26 not in use, so no need to preserve
   - if you change need_to_preserve, modify raw_push_regs_to_preserve() and raw_pop_preserved_regs()
*/
static const uae_u8 need_to_preserve[] = {0,0,0,0, 0,0,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,0, 0,0,0,0, 0,0,0,1, 1,0,0,0};

#include "codegen_arm64.h"

#define FIX_INVERTED_CARRY              \
  if(flags_carry_inverted) {            \
	MRS_NZCV_x(REG_WORK1);              \
	EOR_xxCflag(REG_WORK1, REG_WORK1);  \
	MSR_NZCV_x(REG_WORK1);              \
	flags_carry_inverted = false;       \
  }


STATIC_INLINE void SIGNED8_IMM_2_REG(W4 r, IM8 v) {
	uae_s16 v16 = (uae_s16)(uae_s8)v;
	if (v16 & 0x8000) {
		// Use 32-bit MOVN to keep upper 32 bits clean.
		// MOVN_xi produces a 64-bit result with dirty upper bits for negative values
		// (e.g., MOVN_xi for byte -1 → 0xFFFFFFFFFFFFFFFF).
		MOVN_wi(r, (uae_u16) ~v16);
	} else {
		MOV_wi(r, (uae_u16) v16);
	}
}

STATIC_INLINE void UNSIGNED16_IMM_2_REG(W4 r, IM16 v) {
	// Use 32-bit MOV to keep upper 32 bits clean.
	MOV_wi(r, v);
}

STATIC_INLINE void SIGNED16_IMM_2_REG(W4 r, IM16 v) {
	if (v & 0x8000) {
		// Use 32-bit MOVN to keep upper 32 bits clean.
		MOVN_wi(r, (uae_u16) ~v);
	} else {
		MOV_wi(r, (uae_u16) v);
	}
}

STATIC_INLINE void UNSIGNED8_REG_2_REG(W4 d, RR4 s) {
	UXTB_xx(d, s);
}

STATIC_INLINE void SIGNED8_REG_2_REG(W4 d, RR4 s) {
	// Use 32-bit sign extension to keep upper 32 bits clean.
	// SXTB_xx sign-extends to 64 bits, leaving dirty upper 32 bits
	// for negative values (e.g., SXTB_xx of 0xFF → 0xFFFFFFFFFFFFFFFF).
	SXTB_ww(d, s);
}

STATIC_INLINE void UNSIGNED16_REG_2_REG(W4 d, RR4 s) {
	UXTH_xx(d, s);
}

STATIC_INLINE void SIGNED16_REG_2_REG(W4 d, RR4 s) {
	// Use 32-bit sign extension to keep upper 32 bits clean.
	// SXTH_xx sign-extends to 64 bits, leaving dirty upper 32 bits
	// for negative values (e.g., SXTH_xx of 0xFFFF → 0xFFFFFFFFFFFFFFFF).
	SXTH_ww(d, s);
}

STATIC_INLINE void LOAD_U32(int r, uae_u32 val)
{
	// Use 32-bit W-register instructions so upper 32 bits are always zeroed.
	// The 64-bit variants (MOVN_xi, MOV_xi) would sign-extend values like
	// 0xFFFFxxxx to 0xFFFFFFFFFFFFxxxx, corrupting Amiga addresses used
	// in register-indexed addressing [Xn, X27].
	if((val & 0xffff0000) == 0xffff0000) {
		MOVN_wi(r, ~val);
	} else {
		MOV_wi(r, val);
		if(val >> 16)
			MOVK_wish(r, val >> 16, 16);
	}
}

STATIC_INLINE void LOAD_U64(int r, uae_u64 val)
{
	MOV_xi(r, val);
	if((val >> 16) & 0xffff)
		MOVK_xish(r, val >> 16, 16);
	if((val >> 32) & 0xffff)
		MOVK_xish(r, val >> 32, 32);
	if(val >> 48)
		MOVK_xish(r, val >> 48, 48);
}


#define NUM_PUSH_CMDS 1
#define NUM_POP_CMDS 1
STATIC_INLINE void raw_push_regs_to_preserve(void) {
	STP_xxXpre(27, 28, RSP_INDEX, -16);
}

STATIC_INLINE void raw_pop_preserved_regs(void) {
	LDP_xxXpost(27, 28, RSP_INDEX, 16);
}

STATIC_INLINE void raw_flags_to_reg(int r)
{
	MRS_NZCV_x(r);
	if(flags_carry_inverted) {
		EOR_xxCflag(r, r);
		MSR_NZCV_x(r);
		flags_carry_inverted = false;
	}
	// Use absolute address for regflags.nzcv instead of offset from regs,
	// because LDR/STR unsigned immediate truncates offsets >32760 bytes.
	LOAD_U64(REG_WORK3, (uintptr)&(regflags.nzcv));
	STR_wXi(r, REG_WORK3, 0);

	live.state[FLAGTMP].status = INMEM;
	live.state[FLAGTMP].realreg = -1;
	/* We just "evicted" FLAGTMP. */
	live.nat[r].nholds = 0;
}

STATIC_INLINE void raw_reg_to_flags(int r)
{
	MSR_NZCV_x(r);
}


//
// compuemu_support used raw calls
//
LOWFUNC(WRITE,RMW,2,compemu_raw_inc_opcount,(IM16 op))
{
  uintptr idx = (uintptr) &(regs.raw_cputbl_count) - (uintptr) &regs;
  LDR_xXi(REG_WORK2, R_REGSTRUCT, idx);
  MOV_xi(REG_WORK3, op);
  LDR_wXxLSLi(REG_WORK1, REG_WORK2, REG_WORK3, 1);
  ADD_wwi(REG_WORK1, REG_WORK1, 1);
  STR_wXxLSLi(REG_WORK1, REG_WORK2, REG_WORK3, 1);
}
LENDFUNC(WRITE,RMW,1,compemu_raw_inc_opcount,(IM16 op))

STATIC_INLINE void compemu_raw_call(uintptr t);

/* Runtime diagnostics are observers, not allocator boundaries.  Preserve the
   complete AAPCS64 caller-saved state they can destroy so enabling a trace
   cannot alter guest execution.  Guest FP0-FP7 use callee-saved d8-d15; d0-d7
   cover the allocator's caller-saved FP_RESULT/FS1 and emitter scratch values. */
static constexpr int JIT_OBSERVER_SAVE_SIZE = 240;
static constexpr int JIT_OBSERVER_X18_OFF = 144;
static constexpr int JIT_OBSERVER_NZCV_OFF = 152;
static constexpr int JIT_OBSERVER_FPCR_OFF = 160;
static constexpr int JIT_OBSERVER_FPSR_OFF = 168;
static constexpr int JIT_OBSERVER_D0_OFF = 176;

STATIC_INLINE void compemu_raw_observer_save(void)
{
	SUB_xxi(RSP_INDEX, RSP_INDEX, JIT_OBSERVER_SAVE_SIZE);
	for (int r = 0; r < 18; r += 2)
		STP_xxXi(r, r + 1, RSP_INDEX, r * 8);
	STR_xXi(R18_INDEX, RSP_INDEX, JIT_OBSERVER_X18_OFF);
	MRS_NZCV_x(R18_INDEX);
	STR_xXi(R18_INDEX, RSP_INDEX, JIT_OBSERVER_NZCV_OFF);
	MRS_FPCR_x(R18_INDEX);
	STR_xXi(R18_INDEX, RSP_INDEX, JIT_OBSERVER_FPCR_OFF);
	MRS_FPSR_x(R18_INDEX);
	STR_xXi(R18_INDEX, RSP_INDEX, JIT_OBSERVER_FPSR_OFF);
	for (int r = 0; r < 8; ++r)
		STR_dXi(r, RSP_INDEX, JIT_OBSERVER_D0_OFF + r * 8);
}

STATIC_INLINE void compemu_raw_observer_restore(void)
{
	for (int r = 0; r < 8; ++r)
		LDR_dXi(r, RSP_INDEX, JIT_OBSERVER_D0_OFF + r * 8);
	LDR_xXi(R18_INDEX, RSP_INDEX, JIT_OBSERVER_FPSR_OFF);
	MSR_FPSR_x(R18_INDEX);
	LDR_xXi(R18_INDEX, RSP_INDEX, JIT_OBSERVER_FPCR_OFF);
	MSR_FPCR_x(R18_INDEX);
	LDR_xXi(R18_INDEX, RSP_INDEX, JIT_OBSERVER_NZCV_OFF);
	MSR_NZCV_x(R18_INDEX);
	for (int r = 0; r < 18; r += 2)
		LDP_xxXi(r, r + 1, RSP_INDEX, r * 8);
	LDR_xXi(R18_INDEX, RSP_INDEX, JIT_OBSERVER_X18_OFF);
	ADD_xxi(RSP_INDEX, RSP_INDEX, JIT_OBSERVER_SAVE_SIZE);
}

STATIC_INLINE void compemu_raw_call_observer_i(uintptr target, uintptr arg1)
{
	compemu_raw_observer_save();
	LOAD_U64(REG_PAR1, arg1);
	compemu_raw_call(target);
	compemu_raw_observer_restore();
}

/* Strict native-entry accounting is evidence, not guest work.  Keep the hot
   path to one load/increment/store plus a power-of-two test, and cross the
   expensive observer/C ABI boundary only for the logarithmically sparse
   summaries.  This is emitted before incoming guest NZCV is restored: ANDS
   sets host NZCV transiently; the normal prologue then restores guest flags. */
STATIC_INLINE void compemu_raw_strict_entry_counter(uintptr counter, uintptr reporter, uintptr pc)
{
	LOAD_U64(REG_WORK4, counter);
	LDR_xXi(REG_WORK1, REG_WORK4, 0);
	ADD_xxi(REG_WORK1, REG_WORK1, 1);
	STR_xXi(REG_WORK1, REG_WORK4, 0);
	SUB_xxi(REG_WORK2, REG_WORK1, 1);
	ANDS_xxx(REG_WORK2, REG_WORK1, REG_WORK2);
	uae_u32 *skip_report = (uae_u32 *)get_target();
	CBNZ_xi(REG_WORK2, 0);
	compemu_raw_call_observer_i(reporter, pc);
	write_jmp_target(skip_report, (uintptr)get_target());
}

STATIC_INLINE void compemu_raw_call_observer_ii(uintptr target, uintptr arg1, uintptr arg2)
{
	compemu_raw_observer_save();
	LOAD_U64(REG_PAR1, arg1);
	LOAD_U64(REG_PAR2, arg2);
	compemu_raw_call(target);
	compemu_raw_observer_restore();
}

/* Call an observational/runtime-coherency service with one value held in a
   native register plus an immediate.  Reload the value from the observer save
   frame so this also works when value_reg is one of the ABI argument regs. */
STATIC_INLINE void compemu_raw_call_observer_ri(uintptr target, int value_reg, uintptr arg2)
{
	compemu_raw_observer_save();
	if (value_reg <= R18_INDEX) {
		const int value_off = value_reg == R18_INDEX ? JIT_OBSERVER_X18_OFF : value_reg * 8;
		LDR_xXi(REG_PAR1, RSP_INDEX, value_off);
	} else {
		/* X19-X28 are callee-saved and therefore deliberately absent from the
		   observer frame.  Allocated guest addresses may live in X19-X26; copy
		   their still-live value directly instead of indexing beyond the frame. */
		MOV_xx(REG_PAR1, value_reg);
	}
	LOAD_U64(REG_PAR2, arg2);
	compemu_raw_call(target);
	compemu_raw_observer_restore();
}

/* In strict mode the 68k cache-control model keeps translation enabled while
   guest instruction caching is disabled.  Every generated direct guest store,
   including FPU stores, must therefore invalidate overlapping translated RAM
   before the compiled block can dispatch again. */
STATIC_INLINE void emit_strict_cache_disabled_write_barrier(int address_reg, uae_u32 size)
{
	if (jit_strict_cache_disabled_coherence()) {
		/* Invalidation protects later dispatches.  Also end the current block
		   after this instruction: an FPU store can overwrite a later opcode
		   which has already been folded into the block being emitted. */
		jit_emitted_guest_memory_write = true;
		compemu_raw_call_observer_ri((uintptr)jit_notify_guest_memory_write,
			address_reg, size);
	}
}

LOWFUNC(WRITE,READ,1,compemu_raw_cmp_pc,(IMPTR s))
{
	/* s is always >= NATMEM_OFFSET and < NATMEM_OFFSET + max. Amiga mem */
	clobber_flags();

	uintptr idx = (uintptr) &(regs.pc_p) - (uintptr) &regs;
	LDR_xXi(REG_WORK1, R_REGSTRUCT, idx); // regs.pc_p is 64 bit

	LOAD_U64(REG_WORK2, s);
	CMP_xx(REG_WORK1, REG_WORK2);
}
LENDFUNC(WRITE,READ,1,compemu_raw_cmp_pc,(IMPTR s))

/* Publish one self-consistent architectural PC snapshot.  Any emitted path
   which can leave compiled code must do this before testing/branching to a C
   dispatcher.  Publishing only pc_p leaves m68k_getpc() dependent on the
   previous block's pc/pc_oldp base and can re-enter at an already-retired PC. */
STATIC_INLINE void compemu_raw_set_pc_full_from_reg(RR4 rr_pc)
{
	const uintptr idx_pcp = (uintptr)&regs.pc_p - (uintptr)&regs;
	const uintptr idx_pc = (uintptr)&regs.pc - (uintptr)&regs;
	const uintptr idx_oldp = (uintptr)&regs.pc_oldp - (uintptr)&regs;
	STR_xXi(rr_pc, R_REGSTRUCT, idx_pcp);
	STR_xXi(rr_pc, R_REGSTRUCT, idx_oldp);
	LOAD_U64(REG_WORK3, (uintptr)&MEMBaseDiff);
	LDR_xXi(REG_WORK3, REG_WORK3, 0);
	SUB_xxx(REG_WORK3, rr_pc, REG_WORK3);
	STR_wXi(REG_WORK3, R_REGSTRUCT, idx_pc);
}

STATIC_INLINE void compemu_raw_set_pc_full_const(IMPTR host_pc)
{
	LOAD_U64(REG_WORK2, host_pc);
	compemu_raw_set_pc_full_from_reg(REG_WORK2);
}

LOWFUNC(NONE,WRITE,1,compemu_raw_set_pc_i,(IMPTR s))
{
	LOAD_U64(REG_WORK1, s);
	uintptr idx = (uintptr) &(regs.pc_p) - (uintptr) &regs;
	STR_xXi(REG_WORK1, R_REGSTRUCT, idx);
}
LENDFUNC(NONE,WRITE,1,compemu_raw_set_pc_i,(IMPTR s))

LOWFUNC(NONE,WRITE,1,compemu_raw_set_pc_from_reg,(RR4 rr_pc))
{
	const uintptr idx_pcp = (uintptr)&(regs.pc_p) - (uintptr)&regs;
	const uintptr idx_pc = (uintptr)&(regs.pc) - (uintptr)&regs;
	const uintptr idx_oldp = (uintptr)&(regs.pc_oldp) - (uintptr)&regs;
	STR_xXi(rr_pc, R_REGSTRUCT, idx_pcp);
	STR_xXi(rr_pc, R_REGSTRUCT, idx_oldp);
	LOAD_U64(REG_WORK2, (uintptr)&MEMBaseDiff);
	LDR_xXi(REG_WORK2, REG_WORK2, 0);
	SUB_xxx(REG_WORK3, rr_pc, REG_WORK2);
	STR_wXi(REG_WORK3, R_REGSTRUCT, idx_pc);
}
LENDFUNC(NONE,WRITE,1,compemu_raw_set_pc_from_reg,(RR4 rr_pc))

LOWFUNC(NONE,WRITE,2,compemu_raw_set_pc_full_i,(IM32 guest_pc, IMPTR host_pc))
{
	LOAD_U64(REG_WORK1, host_pc);
	const uintptr idx_pcp = (uintptr)&(regs.pc_p) - (uintptr)&regs;
	const uintptr idx_pc = (uintptr)&(regs.pc) - (uintptr)&regs;
	const uintptr idx_oldp = (uintptr)&(regs.pc_oldp) - (uintptr)&regs;
	STR_xXi(REG_WORK1, R_REGSTRUCT, idx_pcp);
	STR_xXi(REG_WORK1, R_REGSTRUCT, idx_oldp);
	LOAD_U32(REG_WORK2, (uae_u32)guest_pc);
	STR_wXi(REG_WORK2, R_REGSTRUCT, idx_pc);
}
LENDFUNC(NONE,WRITE,2,compemu_raw_set_pc_full_i,(IM32 guest_pc, IMPTR host_pc))

LOWFUNC(NONE,WRITE,2,compemu_raw_mov_l_mi,(MEMW d, IMPTR s))
{
	uintptr idx = d - (uintptr) &regs;
	if(d == (uintptr) &(regs.pc_p) || d == (uintptr) &(regs.pc_oldp)) {
		LOAD_U64(REG_WORK2, s);  // pc_p/pc_oldp are 64-bit host pointers
		STR_xXi(REG_WORK2, R_REGSTRUCT, idx);
	} else if(idx <= 16380 && (idx & 3) == 0) {
		// Within R_REGSTRUCT unsigned-offset range (covers regs + nearby
		// globals like regflags that the linker places close by).
		LOAD_U32(REG_WORK2, (uae_u32)s);
		STR_wXi(REG_WORK2, R_REGSTRUCT, idx);
	} else {
		// Address too far from R_REGSTRUCT — use absolute address.
		LOAD_U64(REG_WORK1, d);
		LOAD_U32(REG_WORK2, (uae_u32)s);
		STR_wXi(REG_WORK2, REG_WORK1, 0);
	}
}
LENDFUNC(NONE,WRITE,2,compemu_raw_mov_l_mi,(MEMW d, IMPTR s))

LOWFUNC(NONE,WRITE,2,compemu_raw_mov_l_mr,(MEMW d, RR4 s))
{
	uintptr idx = d - (uintptr) &regs;
	if(d == (uintptr) &(regs.pc_p) || d == (uintptr) &(regs.pc_oldp)) {
		STR_xXi(s, R_REGSTRUCT, idx);
	} else if(idx <= 16380 && (idx & 3) == 0) {
		// Within R_REGSTRUCT unsigned-offset range (covers regs + nearby
		// globals like regflags that the linker places close by).
		STR_wXi(s, R_REGSTRUCT, idx);
	} else {
		// Absolute address: alternate scratch preserves a REG_WORK1 source.
		const int addr_reg = s == REG_WORK1 ? REG_WORK2 : REG_WORK1; LOAD_U64(addr_reg, d);
		STR_wXi(s, addr_reg, 0);
	}
}
LENDFUNC(NONE,WRITE,2,compemu_raw_mov_l_mr,(MEMW d, RR4 s))

LOWFUNC(NONE,NONE,2,compemu_raw_mov_l_ri,(W4 d, IM32 s))
{
	LOAD_U32(d, s);
}
LENDFUNC(NONE,NONE,2,compemu_raw_mov_l_ri,(W4 d, IM32 s))

LOWFUNC(NONE,READ,2,compemu_raw_mov_l_rm,(W4 d, MEMR s))
{
	uintptr idx = s - (uintptr) &regs;
	if(s == (uintptr) &(regs.pc_p) || s == (uintptr) &(regs.pc_oldp)) {
		LDR_xXi(d, R_REGSTRUCT, idx);
	} else if(idx <= 16380 && (idx & 3) == 0) {
		// Within R_REGSTRUCT unsigned-offset range.
		LDR_wXi(d, R_REGSTRUCT, idx);
	} else {
		// Address too far from R_REGSTRUCT — use absolute address.
		LOAD_U64(REG_WORK1, s);
		LDR_wXi(d, REG_WORK1, 0);
	}
}
LENDFUNC(NONE,READ,2,compemu_raw_mov_l_rm,(W4 d, MEMR s))

LOWFUNC(NONE,NONE,2,compemu_raw_mov_l_rr,(W4 d, RR4 s))
{
	// Use 64-bit MOV to preserve full register width.  PC_P holds a
	// 64-bit host pointer; all other virtual registers hold 32-bit M68k
	// values whose upper 32 bits are already zeroed by W-register ops
	// upstream, so MOV_xx is safe for both cases.
	MOV_xx(d, s);
}
LENDFUNC(NONE,NONE,2,compemu_raw_mov_l_rr,(W4 d, RR4 s))

LOWFUNC(WRITE,RMW,1,compemu_raw_dec_m,(MEMRW d))
{
	clobber_flags();

	LOAD_U64(REG_WORK1, d);
	LDR_wXi(REG_WORK2, REG_WORK1, 0);
	SUBS_wwi(REG_WORK2, REG_WORK2, 1);
	STR_wXi(REG_WORK2, REG_WORK1, 0);
}
LENDFUNC(WRITE,RMW,1,compemu_raw_dec_m,(MEMRW ds))

LOWFUNC(WRITE,RMW,1,compemu_raw_inc_m,(MEMRW d))
{
	/* Profiling helper: keep this flag-neutral so it can be inserted on
	   already-decided control-flow paths without changing condition codes. */
	LOAD_U64(REG_WORK1, d);
	LDR_wXi(REG_WORK2, REG_WORK1, 0);
	ADD_wwi(REG_WORK2, REG_WORK2, 1);
	STR_wXi(REG_WORK2, REG_WORK1, 0);
}
LENDFUNC(WRITE,RMW,1,compemu_raw_inc_m,(MEMRW d))

STATIC_INLINE void compemu_raw_call(uintptr t)
{
	/* x0-x7 carry AAPCS64 arguments.  In particular REG_WORK1 is x2, so
	   using it for the call target destroys argument 3 before BLR.  x18 is
	   permanently reserved from the JIT allocator; use it as the call-only
	   target scratch regardless of the current helper's arity. */
#if defined(__APPLE__)
	/* x18 is the Darwin platform register: the kernel may zero it on any
	   context switch, so a preemption between the load and BLR would jump to
	   0. LR is saved first anyway, so use x30 itself as the call target
	   (BLR reads the target before it writes the return address). */
	STR_xXpre(RLR_INDEX, RSP_INDEX, -16);
	LOAD_U64(RLR_INDEX, t);
	BLR_x(RLR_INDEX);
	LDR_xXpost(RLR_INDEX, RSP_INDEX, 16);
#else
	LOAD_U64(R18_INDEX, t);

	STR_xXpre(RLR_INDEX, RSP_INDEX, -16);
	BLR_x(R18_INDEX);
	LDR_xXpost(RLR_INDEX, RSP_INDEX, 16);
#endif
}

STATIC_INLINE void compemu_raw_call_r(RR4 r)
{
	STR_xXpre(RLR_INDEX, RSP_INDEX, -16);
	BLR_x(r);
	LDR_xXpost(RLR_INDEX, RSP_INDEX, 16);
}

/* Runtime state synchronisers are architectural bookkeeping, not guest
 * arithmetic. Preserve NZCV explicitly across their AAPCS64 C calls so a
 * serviced FPU opcode cannot alter the integer CCR carried by host flags. */
STATIC_INLINE void compemu_raw_call_preserve_nzcv(uintptr t)
{
#if defined(__APPLE__)
	/* See compemu_raw_call: never hold a call target in x18 on Darwin. */
	MRS_NZCV_x(REG_WORK4);
	STP_xxXpre(RLR_INDEX, REG_WORK4, RSP_INDEX, -16);
	LOAD_U64(RLR_INDEX, t);
	BLR_x(RLR_INDEX);
#else
	LOAD_U64(R18_INDEX, t);
	MRS_NZCV_x(REG_WORK4);
	STP_xxXpre(RLR_INDEX, REG_WORK4, RSP_INDEX, -16);
	BLR_x(R18_INDEX);
#endif
	LDP_xxXpost(RLR_INDEX, REG_WORK4, RSP_INDEX, 16);
	MSR_NZCV_x(REG_WORK4);
}

STATIC_INLINE void compemu_raw_jcc_l_oponly(int cc)
{
	FIX_INVERTED_CARRY

	switch (cc) {
		case NATIVE_CC_F_F: // Never
			B_i(2);          // skip the caller-patched taken branch
			B_i(0);
			break;

		case NATIVE_CC_F_EQ: // Equal
			BEQ_i(0);
			break;

		case NATIVE_CC_HI: // HI
			BEQ_i(2);										// beq no jump
			BCC_i(0);										// bcc jump
			break;

		case NATIVE_CC_LS: // LS
			BEQ_i(2);										// beq jump
			BCC_i(2);										// bcc no jump
			// jump
			B_i(0);
			// no jump
			break;

		case NATIVE_CC_F_OGT: // Jump if valid and greater than
			BVS_i(2);		// do not jump if NaN
			BGT_i(0);		// jump if greater than
			break;

		case NATIVE_CC_F_OGE: // Jump if valid and greater or equal
			BVS_i(2);		// do not jump if NaN
			BCS_i(0);		// jump if carry set
			break;

		case NATIVE_CC_F_OLT: // Jump if vaild and less than
			BVS_i(2);		// do not jump if NaN
			BCC_i(0);		// jump if carry cleared
			break;

		case NATIVE_CC_F_OLE: // Jump if valid and less or equal
			BVS_i(2);		// do not jump if NaN
			BLE_i(0);		// jump if less or equal
			break;

		case NATIVE_CC_F_OGL: // Jump if valid and greator or less
			BVS_i(2);		// do not jump if NaN
			BNE_i(0);		// jump if not equal
			break;

		case NATIVE_CC_F_OR: // Jump if valid
			BVC_i(0);
			break;

		case NATIVE_CC_F_UN: // Jump if NAN
			BVS_i(0);
			break;

		case NATIVE_CC_F_UEQ: // Jump if NAN or equal
			BVS_i(2); 	// jump if NaN
			BNE_i(2);		// do not jump if greater or less
			// jump
			B_i(0);
			break;

		case NATIVE_CC_F_UGT: // Jump if NAN or greater than
			BVS_i(2); 	// jump if NaN
			BLS_i(2);		// do not jump if lower or same
			// jump
			B_i(0);
			break;

		case NATIVE_CC_F_UGE: // Jump if NAN or greater or equal
			BVS_i(2); 	// jump if NaN
			BMI_i(2);		// do not jump if lower
			// jump
			B_i(0);
			break;

		case NATIVE_CC_F_ULT: // Jump if NAN or less than
			BVS_i(2); 	// jump if NaN
			BGE_i(2);		// do not jump if greater or equal
			// jump
			B_i(0);
			break;

		case NATIVE_CC_F_ULE: // Jump if NAN or less or equal
			BVS_i(2); 	// jump if NaN
			BGT_i(2);		// do not jump if greater
			// jump
			B_i(0);
			break;

		case NATIVE_CC_F_NE: // Not equal
			BNE_i(0);
			break;

		case NATIVE_CC_F_T: // Always
			B_i(0);
			break;

		default:
			CC_B_i(cc, 0);
			break;
	}
	// emit of target into last branch will be done by caller
}

STATIC_INLINE void compemu_raw_handle_except(IM32 cycles)
{
	uae_u32* branchadd;

	clobber_flags();

	uintptr idx = (uintptr)(&regs.jit_exception) - (uintptr)(&regs);
	LDR_wXi(REG_WORK1, R_REGSTRUCT, idx);
	if (jit_test_dispatch_summary_enabled()) {
		LOAD_U64(REG_WORK4, (uintptr)&jit_test_handle_except_checks);
		LDR_xXi(R18_INDEX, REG_WORK4, 0);
		ADD_xxi(R18_INDEX, R18_INDEX, 1);
		STR_xXi(R18_INDEX, REG_WORK4, 0);
	}
	branchadd = (uae_u32*)get_target();
	CBZ_wi(REG_WORK1, 0);  // no exception, jump to next instruction

	if (jit_test_dispatch_summary_enabled()) {
		LOAD_U64(REG_WORK4, (uintptr)&jit_test_handle_except_taken);
		LDR_xXi(R18_INDEX, REG_WORK4, 0);
		ADD_xxi(R18_INDEX, R18_INDEX, 1);
		STR_xXi(R18_INDEX, REG_WORK4, 0);
		LOAD_U64(REG_WORK4, (uintptr)&jit_test_handle_except_cycles);
		LOAD_U32(R18_INDEX, cycles);
		STR_xXi(R18_INDEX, REG_WORK4, 0);
	}
	LOAD_U32(REG_PAR1, cycles);
	uae_u32* branchadd2 = (uae_u32*)get_target();
	B_i(0); // <exec_nostats>
	write_jmp_target(branchadd2, (uintptr)popall_execute_exception);

	// Write target of next instruction
	write_jmp_target(branchadd, (uintptr)get_target());
}

LOWFUNC(NONE,WRITE,1,compemu_raw_execute_normal,(MEMR s))
{
	LOAD_U64(REG_WORK1, s);
	LDR_xXi(REG_WORK1, REG_WORK1, 0);
	uae_u32* branchadd = (uae_u32*)get_target();
	B_i(0); // <exec_nostats>
	write_jmp_target(branchadd, (uintptr)popall_execute_normal_setpc);
}
LENDFUNC(NONE,WRITE,1,compemu_raw_execute_normal,(MEMR s))

STATIC_INLINE void compemu_raw_execute_normal_cycles(MEMR s, IM32 cycles)
{
	LOAD_U64(REG_WORK3, (uintptr)&countdown);
	LDR_wXi(REG_WORK2, REG_WORK3, 0);
	if (jit_test_dispatch_summary_enabled()) {
		LOAD_U64(REG_WORK4, (uintptr)&jit_test_execute_normal_cycles_before);
		STR_wXi(REG_WORK2, REG_WORK4, 0);
	}
	if(cycles >= 0 && cycles <= 0xfff) {
		SUB_wwi(REG_WORK2, REG_WORK2, cycles);
	} else {
		LOAD_U32(REG_WORK1, cycles);
		SUB_www(REG_WORK2, REG_WORK2, REG_WORK1);
	}
	STR_wXi(REG_WORK2, REG_WORK3, 0);
	if (jit_test_dispatch_summary_enabled()) {
		LOAD_U64(REG_WORK4, (uintptr)&jit_test_execute_normal_cycles_after);
		STR_wXi(REG_WORK2, REG_WORK4, 0);
		LOAD_U64(REG_WORK4, (uintptr)&jit_test_execute_normal_cycles_entries);
		LDR_xXi(R18_INDEX, REG_WORK4, 0);
		ADD_xxi(R18_INDEX, R18_INDEX, 1);
		STR_xXi(R18_INDEX, REG_WORK4, 0);
	}

	LOAD_U64(REG_WORK1, s);
	LDR_xXi(REG_WORK1, REG_WORK1, 0);
	uae_u32* branchadd = (uae_u32*)get_target();
	B_i(0);
	write_jmp_target(branchadd, (uintptr)popall_execute_normal_setpc);
}

LOWFUNC(NONE,WRITE,1,compemu_raw_check_checksum,(MEMR s))
{
	LOAD_U64(REG_WORK1, s);
	LDR_xXi(REG_WORK1, REG_WORK1, 0);
	uae_u32* branchadd = (uae_u32*)get_target();
	B_i(0); // <exec_nostats>
	write_jmp_target(branchadd, (uintptr)popall_check_checksum_setpc);
}
LENDFUNC(NONE,WRITE,1,compemu_raw_check_checksum,(MEMR s))

LOWFUNC(NONE,WRITE,1,compemu_raw_exec_nostats,(IMPTR s))
{
	LOAD_U64(REG_WORK1, s);
	uae_u32* branchadd = (uae_u32*)get_target();
	B_i(0); // <exec_nostats>
	write_jmp_target(branchadd, (uintptr)popall_exec_nostats_setpc);
}
LENDFUNC(NONE,WRITE,1,compemu_raw_exec_nostats,(IMPTR s))

STATIC_INLINE void compemu_raw_maybe_recompile(void)
{
	BGE_i(2);
	uae_u32* branchadd = (uae_u32*)get_target();
	B_i(0);
	write_jmp_target(branchadd, (uintptr)popall_recompile_block);
}

STATIC_INLINE void compemu_raw_jmp(uintptr t)
{
	uintptr loc = (uintptr)get_target();
	if(t > loc - 127 * 1024 * 1024 && t < loc + 127 * 1024 * 1024) {
		B_i(0);
		write_jmp_target((uae_u32*)loc, t);
	} else {
		LDR_xPCi(REG_WORK1, 8);
		BR_x(REG_WORK1);
		emit_quad(t);
	}
}

STATIC_INLINE void compemu_raw_jmp_pc_tag(void)
{
	uintptr idx = (uintptr)&regs.pc_p - (uintptr)&regs;
	/* Load enough of pc_p to cover the full cacheline index. cacheline(x) =
	   ((x>>1) & (TAGMASK>>1)) << 1; with TAGMASK=0x3ffff that is bits [1:17]
	   (17 bits), so a 16-bit LDRH is NOT enough -- load 32 bits. Must match
	   the C-side cacheline() in compemu.h or chained dispatch goes to the
	   wrong cache_tags slot. */
	LDR_wXi(REG_WORK1, R_REGSTRUCT, idx);
	/* Extract cacheline = ((pc_p>>1) & (TAGMASK>>1)) << 1. TAGMASK=0x3ffff ->
	   TAGMASK>>1 = 0x1ffff (17 bits). Clear bit 0 (handler slot, not bi slot). */
	UBFX_xxii(REG_WORK1, REG_WORK1, 1, 17);
	LSL_xxi(REG_WORK1, REG_WORK1, 1);
	idx = (uintptr)&regs.cache_tags - (uintptr)&regs;
	LDR_xXi(REG_WORK2, R_REGSTRUCT, idx);
	LDR_xXxLSLi(REG_WORK1, REG_WORK2, REG_WORK1, 3);
	BR_x(REG_WORK1);
}

STATIC_INLINE void compemu_raw_maybe_cachemiss(void)
{
	BEQ_i(2);
	uae_u32* branchadd = (uae_u32*)get_target();
	B_i(0);
	write_jmp_target(branchadd, (uintptr)popall_cache_miss);
}

STATIC_INLINE void compemu_raw_maybe_do_nothing(IM32 cycles)
{
	uintptr idx = (uintptr)&regs.spcflags - (uintptr) &regs;
	LDR_wXi(REG_WORK1, R_REGSTRUCT, idx);
	uae_s8 *branchadd = (uae_s8 *)get_target();
	CBZ_wi(REG_WORK1, 0);  // <end>

	// Use absolute address for countdown (global variable, may be >32KB from regs)
	LOAD_U64(REG_WORK3, (uintptr)&countdown);
	LDR_wXi(REG_WORK2, REG_WORK3, 0);
	if(cycles >= 0 && cycles <= 0xfff) {
		SUB_wwi(REG_WORK2, REG_WORK2, cycles);
	} else {
		LOAD_U32(REG_WORK1, cycles);
		SUB_www(REG_WORK2, REG_WORK2, REG_WORK1);
	}
	STR_wXi(REG_WORK2, REG_WORK3, 0);

	uae_u32* branchadd2 = (uae_u32*)get_target();
	B_i(0);
	write_jmp_target(branchadd2, (uintptr)popall_do_nothing);

	// <end>
	write_jmp_target((uae_u32 *)branchadd, (uintptr)get_target());
}

// Optimize access to struct regstruct with and memory with fixed registers

LOWFUNC(NONE,NONE,1,compemu_raw_init_r_regstruct,(IMPTR s))
{
	LOAD_U64(R_REGSTRUCT, s);
	// Load NATMEM_OFFSET via its absolute address instead of offset from regs.
	// The old approach used LDR_xXi with offset = &NATMEM_OFFSET - &regs, but
	// LDR_xXi silently truncates offsets >32760 bytes (12-bit field), which
	// corrupts R27 when the global natmem_offset is far from regs in memory.
	LOAD_U64(R_MEMSTART, (uintptr)&NATMEM_OFFSET);
	LDR_xXi(R_MEMSTART, R_MEMSTART, 0);
}
LENDFUNC(NONE,NONE,1,compemu_raw_init_r_regstruct,(IMPTR s))

// Handle end of compiled block
LOWFUNC(NONE,NONE,2,compemu_raw_endblock_pc_inreg,(RR4 rr_pc, IM32 cycles))
{
	// countdown -= scaled_cycles(totcycles);
	LOAD_U64(REG_WORK3, (uintptr)&countdown);
	LDR_wXi(REG_WORK1, REG_WORK3, 0);
	if(cycles >= 0 && cycles <= 0xfff) {
		SUB_wwi(REG_WORK1, REG_WORK1, cycles);
	} else {
		LOAD_U32(REG_WORK2, cycles);
		SUB_www(REG_WORK1, REG_WORK1, REG_WORK2);
	}
	STR_wXi(REG_WORK1, REG_WORK3, 0);

	/* Commit the retired successor before either the countdown or spcflags
	   slow exit can return to C.  This is also the canonical state observed by
	   a directly chained successor. */
	compemu_raw_set_pc_full_from_reg(rr_pc);

	uae_u32* branch_hot = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 31, 0); // non-negative countdown continues on the hot chain path

	uae_u32* branchadd = (uae_u32*)get_target();
	B_i(0);
	write_jmp_target(branchadd, (uintptr)popall_do_nothing);

	write_jmp_target(branch_hot, (uintptr)get_target());
	/* Check spcflags on hot path */
	{
		uintptr idx_spc_hot = (uintptr)&regs.spcflags - (uintptr)&regs;
		{ _W(0xd5033bbf); } LDR_wXi(REG_WORK4, R_REGSTRUCT, idx_spc_hot);
		CBZ_wi(REG_WORK4, 2);
		uae_u32* br_dn_hot = (uae_u32*)get_target();
		B_i(0);
		write_jmp_target(br_dn_hot, (uintptr)popall_do_nothing);
	}
	UBFIZ_xxii(rr_pc, rr_pc, 0, 18);  // mask to TAGMASK width (0x3ffff = 18 bits)
	/* Clear bit 0 to ensure even cacheline index (handler slot, not bi slot).
	   cacheline(x)=((x>>1)&(TAGMASK>>1))<<1; TAGMASK>>1=0x1ffff -> 17 bits.
	   MUST match the C-side cacheline() in compemu.h (TAGMASK 0x3ffff). */
	UBFX_xxii(rr_pc, rr_pc, 1, 17);
	LSL_xxi(rr_pc, rr_pc, 1);
	uintptr offs = (uintptr)(&regs.cache_tags) - (uintptr)&regs;
	LDR_xXi(REG_WORK1, R_REGSTRUCT, offs);
	LDR_xXxLSLi(REG_WORK1, REG_WORK1, rr_pc, 3);
	BR_x(REG_WORK1);
}
LENDFUNC(NONE,NONE,2,compemu_raw_endblock_pc_inreg,(RR4 rr_pc, IM32 cycles))

STATIC_INLINE uae_u32* compemu_raw_endblock_pc_isconst(IM32 cycles, IMPTR v)
{
	/* v is always >= NATMEM_OFFSET and < NATMEM_OFFSET + max. Amiga mem */
	uae_u32* tba;

	// countdown -= scaled_cycles(totcycles);
	// Use absolute address for countdown (global variable, may be >32KB from regs)
	LOAD_U64(REG_WORK3, (uintptr)&countdown);
	LDR_wXi(REG_WORK1, REG_WORK3, 0);
	if(cycles >= 0 && cycles <= 0xfff) {
		SUB_wwi(REG_WORK1, REG_WORK1, cycles);
	} else {
		LOAD_U32(REG_WORK2, cycles);
		SUB_www(REG_WORK1, REG_WORK1, REG_WORK2);
	}
	STR_wXi(REG_WORK1, REG_WORK3, 0);

	/* Commit the same complete successor snapshot for every exit path. */
	compemu_raw_set_pc_full_const(v);

	uae_u32* branch_hot = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 31, 0); // non-negative countdown continues on the hot chain path

	uae_u32* branchadd = (uae_u32*)get_target();
	B_i(0);
	write_jmp_target(branchadd, (uintptr)popall_do_nothing);

	write_jmp_target(branch_hot, (uintptr)get_target());
	/* Check spcflags on hot path */
	{
		uintptr idx_spc_hot2 = (uintptr)&regs.spcflags - (uintptr)&regs;
		{ _W(0xd5033bbf); } LDR_wXi(REG_WORK4, R_REGSTRUCT, idx_spc_hot2);
		CBZ_wi(REG_WORK4, 2);
		uae_u32* br_dn_hot2 = (uae_u32*)get_target();
		B_i(0);
		write_jmp_target(br_dn_hot2, (uintptr)popall_do_nothing);
	}
	tba = (uae_u32*)get_target();
	B_i(0); // <target set by caller>

	return tba;
}

/*************************************************************************
* FPU stuff                                                             *
*************************************************************************/

LOWFUNC(NONE,NONE,2,raw_fmov_rr,(FW d, FR s))
{
	FMOV_dd(d, s);
}
LENDFUNC(NONE,NONE,2,raw_fmov_rr,(FW d, FR s))

LOWFUNC(NONE,WRITE,2,compemu_raw_fmov_mr_drop,(MEMW mem, FR s))
{
	if(mem >= (uintptr) &regs && mem < (uintptr) &regs + 32760 && ((mem - (uintptr) &regs) & 0x7) == 0) {
		STR_dXi(s, R_REGSTRUCT, (mem - (uintptr) &regs));
	} else {
		LOAD_U64(REG_WORK1, mem);
		STR_dXi(s, REG_WORK1, 0);
	}
}
LENDFUNC(NONE,WRITE,2,compemu_raw_fmov_mr_drop,(MEMW mem, FR s))

LOWFUNC(NONE,READ,2,compemu_raw_fmov_rm,(FW d, MEMR mem))
{
	if(mem >= (uintptr) &regs && mem < (uintptr) &regs + 32760 && ((mem - (uintptr) &regs) & 0x7) == 0) {
		LDR_dXi(d, R_REGSTRUCT, (mem - (uintptr) &regs));
	} else {
		LOAD_U64(REG_WORK1, mem);
		LDR_dXi(d, REG_WORK1, 0);
	}
}
LENDFUNC(NONE,READ,2,compemu_raw_fmov_rm,(FW d, MEMW mem))

LOWFUNC(NONE,NONE,2,raw_fmov_l_rr,(FW d, RR4 s))
{
	SCVTF_dw(d, s);
}
LENDFUNC(NONE,NONE,2,raw_fmov_l_rr,(FW d, RR4 s))

LOWFUNC(NONE,NONE,2,raw_fmov_s_rr,(FW d, RR4 s))
{
	FMOV_sw(SCRATCH_F64_1, s);
	FCVT_ds(d, SCRATCH_F64_1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_s_rr,(FW d, RR4 s))

LOWFUNC(NONE,NONE,2,raw_fmov_w_rr,(FW d, RR2 s))
{
	SIGNED16_REG_2_REG(REG_WORK1, s);
	SCVTF_dw(d, REG_WORK1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_w_rr,(FW d, RR2 s))

LOWFUNC(NONE,NONE,2,raw_fmov_b_rr,(FW d, RR1 s))
{
	SIGNED8_REG_2_REG(REG_WORK1, s);
	SCVTF_dw(d, REG_WORK1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_b_rr,(FW d, RR1 s))

LOWFUNC(NONE,NONE,2,raw_fmov_d_rrr,(FW d, RR4 s1, RR4 s2))
{
	BFI_xxii(s1, s2, 32, 32);
	FMOV_dx(d, s1);
}
LENDFUNC(NONE,NONE,2,raw_fmov_d_rrr,(FW d, RR4 s1, RR4 s2))

/* Match fpu_mpfr.cpp:extract_to_integer() for the ordinary integer FMOVE
 * destinations.  AArch64 FCVTAS returns zero for NaN and does not publish the
 * 68k OPERR/accrued-IOP contract.  Round under FPCR first, saturate by the
 * source sign, then compare the final signed integer back with that rounded
 * value.  The comparison also catches narrower byte/word saturation.
 *
 * Integer NZCV is guest state in this JIT, so preserve it around all private
 * classification branches.  Native FP registers and FP_RESULT remain owned by
 * the allocator; only MPFR's architectural exception fields are updated. */
STATIC_INLINE void fmov_to_int_emit(W4 d, FR s, int width)
{
	const uae_u32 min_value = width == 8 ? 0xffffff80U :
		width == 16 ? 0xffff8000U : 0x80000000U;
	const uae_u32 max_value = width == 8 ? 0x0000007fU :
		width == 16 ? 0x00007fffU : 0x7fffffffU;

	MRS_NZCV_x(REG_WORK4);

	FRINTI_dd(SCRATCH_F64_1, s);
	FCVTAS_wd(REG_WORK1, SCRATCH_F64_1);

	/* Repair FCVTAS' NaN/out-of-range result using the architectural sign. */
	SCVTF_dw(SCRATCH_F64_2, REG_WORK1);
	FCMP_dd(SCRATCH_F64_1, SCRATCH_F64_2);
	uae_u32* converted_in_range = (uae_u32*)get_target();
	BEQ_i(0);
	FMOV_xd(REG_WORK3, s);
	uae_u32* invalid_positive = (uae_u32*)get_target();
	TBZ_xii(REG_WORK3, 63, 0);
	LOAD_U32(REG_WORK1, min_value);
	uae_u32* final_candidate_from_negative = (uae_u32*)get_target();
	B_i(0);
	write_jmp_target(invalid_positive, (uintptr)get_target());
	LOAD_U32(REG_WORK1, max_value);
	uae_u32* final_candidate_from_positive = (uae_u32*)get_target();
	B_i(0);

	write_jmp_target(converted_in_range, (uintptr)get_target());
	if (width < 32) {
		LOAD_U32(REG_WORK2, max_value);
		CMP_ww(REG_WORK1, REG_WORK2);
		uae_u32* not_above_max = (uae_u32*)get_target();
		BLE_i(0);
		MOV_ww(REG_WORK1, REG_WORK2);
		uae_u32* final_candidate_from_max = (uae_u32*)get_target();
		B_i(0);
		write_jmp_target(not_above_max, (uintptr)get_target());
		LOAD_U32(REG_WORK2, min_value);
		CMP_ww(REG_WORK1, REG_WORK2);
		uae_u32* not_below_min = (uae_u32*)get_target();
		BGE_i(0);
		MOV_ww(REG_WORK1, REG_WORK2);
		write_jmp_target(not_below_min, (uintptr)get_target());
		write_jmp_target(final_candidate_from_max, (uintptr)get_target());
	}

	write_jmp_target(final_candidate_from_negative, (uintptr)get_target());
	write_jmp_target(final_candidate_from_positive, (uintptr)get_target());
	if (width == 32)
		MOV_ww(d, REG_WORK1);
	else
		BFI_wwii(d, REG_WORK1, 0, width);

	/* Equality means the rounded value was representable at the destination
	 * width.  Unordered (NaN), infinity, and either overflow direction reach
	 * the architectural OPERR path. */
	SCVTF_dw(SCRATCH_F64_2, REG_WORK1);
	FCMP_dd(SCRATCH_F64_1, SCRATCH_F64_2);
	uae_u32* no_operr = (uae_u32*)get_target();
	BEQ_i(0);
	LOAD_U64(REG_WORK2, (uintptr)&fpu.fpsr.exception_status);
	LOAD_U32(REG_WORK3, FPSR_EXCEPTION_OPERR);
	STR_wXi(REG_WORK3, REG_WORK2, 0);
	LOAD_U64(REG_WORK2, (uintptr)&fpu.fpsr.accrued_exception);
	LDR_wXi(REG_WORK3, REG_WORK2, 0);
	LOAD_U32(REG_WORK1, FPSR_ACCR_IOP);
	ORR_www(REG_WORK3, REG_WORK3, REG_WORK1);
	STR_wXi(REG_WORK3, REG_WORK2, 0);

	/* A finite fractional value can be both invalid for the destination range
	 * and inexact under the selected rounding mode.  Infinity is exact here;
	 * NaN is unordered and must not acquire INEX2. */
	FCMP_dd(s, SCRATCH_F64_1);
	uae_u32* exception_done_operr_unordered = (uae_u32*)get_target();
	BVS_i(0);
	uae_u32* exception_done_operr_exact = (uae_u32*)get_target();
	BEQ_i(0);
	LOAD_U64(REG_WORK2, (uintptr)&fpu.fpsr.exception_status);
	LDR_wXi(REG_WORK3, REG_WORK2, 0);
	LOAD_U32(REG_WORK1, FPSR_EXCEPTION_INEX2);
	ORR_www(REG_WORK3, REG_WORK3, REG_WORK1);
	STR_wXi(REG_WORK3, REG_WORK2, 0);
	LOAD_U64(REG_WORK2, (uintptr)&fpu.fpsr.accrued_exception);
	LDR_wXi(REG_WORK3, REG_WORK2, 0);
	LOAD_U32(REG_WORK1, FPSR_ACCR_INEX);
	ORR_www(REG_WORK3, REG_WORK3, REG_WORK1);
	STR_wXi(REG_WORK3, REG_WORK2, 0);
	uae_u32* exception_done_from_operr = (uae_u32*)get_target();
	B_i(0);

	write_jmp_target(exception_done_operr_unordered, (uintptr)get_target());
	write_jmp_target(exception_done_operr_exact, (uintptr)get_target());
	uae_u32* exception_done_from_plain_operr = (uae_u32*)get_target();
	B_i(0);

	write_jmp_target(no_operr, (uintptr)get_target());
	/* A representable fractional source raises INEX2 and accrued INEX. */
	FCMP_dd(s, SCRATCH_F64_1);
	uae_u32* exception_done_exact = (uae_u32*)get_target();
	BEQ_i(0);
	LOAD_U64(REG_WORK2, (uintptr)&fpu.fpsr.exception_status);
	LOAD_U32(REG_WORK3, FPSR_EXCEPTION_INEX2);
	STR_wXi(REG_WORK3, REG_WORK2, 0);
	LOAD_U64(REG_WORK2, (uintptr)&fpu.fpsr.accrued_exception);
	LDR_wXi(REG_WORK3, REG_WORK2, 0);
	LOAD_U32(REG_WORK1, FPSR_ACCR_INEX);
	ORR_www(REG_WORK3, REG_WORK3, REG_WORK1);
	STR_wXi(REG_WORK3, REG_WORK2, 0);

	write_jmp_target(exception_done_from_operr, (uintptr)get_target());
	write_jmp_target(exception_done_from_plain_operr, (uintptr)get_target());
	write_jmp_target(exception_done_exact, (uintptr)get_target());
	MSR_NZCV_x(REG_WORK4);
}

LOWFUNC(NONE,NONE,2,raw_fmov_to_l_rr,(W4 d, FR s))
{
	fmov_to_int_emit(d, s, 32);
}
LENDFUNC(NONE,NONE,2,raw_fmov_to_l_rr,(W4 d, FR s))

LOWFUNC(NONE,NONE,2,raw_fmov_to_s_rr,(W4 d, FR s))
{
	/* FCVT supplies the correctly rounded IEEE-single payload under FPCR.  Its
	 * FPSR flags are host state, so sample them in an isolated window, restore
	 * the caller's FPSR, and publish only the equivalent 68k exception fields. */
	MRS_NZCV_x(REG_WORK4);
	MRS_FPSR_x(REG_WORK3);
	MOV_wi(REG_WORK1, 0);
	MSR_FPSR_x(REG_WORK1);
	FCVT_sd(SCRATCH_F64_1, s);
	FMOV_ws(d, SCRATCH_F64_1);
	MRS_FPSR_x(REG_WORK1);
	MSR_FPSR_x(REG_WORK3);

	MOV_wi(REG_WORK2, 0); /* replacement exception-status byte */
	uae_u32* no_ioc = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 0, 0); /* IOC: signalling NaN */
	LOAD_U32(REG_WORK3, FPSR_EXCEPTION_SNAN);
	ORR_www(REG_WORK2, REG_WORK2, REG_WORK3);
	write_jmp_target(no_ioc, (uintptr)get_target());
	uae_u32* no_ofc = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 2, 0); /* OFC */
	LOAD_U32(REG_WORK3, FPSR_EXCEPTION_OVFL);
	ORR_www(REG_WORK2, REG_WORK2, REG_WORK3);
	write_jmp_target(no_ofc, (uintptr)get_target());
	uae_u32* no_ufc = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 3, 0); /* UFC */
	LOAD_U32(REG_WORK3, FPSR_EXCEPTION_UNFL);
	ORR_www(REG_WORK2, REG_WORK2, REG_WORK3);
	write_jmp_target(no_ufc, (uintptr)get_target());
	uae_u32* no_ixc = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 4, 0); /* IXC */
	LOAD_U32(REG_WORK3, FPSR_EXCEPTION_INEX2);
	ORR_www(REG_WORK2, REG_WORK2, REG_WORK3);
	write_jmp_target(no_ixc, (uintptr)get_target());
	LOAD_U64(REG_WORK3, (uintptr)&fpu.fpsr.exception_status);
	STR_wXi(REG_WORK2, REG_WORK3, 0);

	/* Accrued.INEX includes overflow even if a host omits IXC.  Accrued.UNFL
	 * requires both underflow and inexact, matching update_exceptions(). */
	MOV_wi(REG_WORK2, 0);
	uae_u32* no_iop = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 0, 0);
	LOAD_U32(REG_WORK3, FPSR_ACCR_IOP);
	ORR_www(REG_WORK2, REG_WORK2, REG_WORK3);
	write_jmp_target(no_iop, (uintptr)get_target());
	uae_u32* no_accr_ovfl = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 2, 0);
	LOAD_U32(REG_WORK3, FPSR_ACCR_OVFL | FPSR_ACCR_INEX);
	ORR_www(REG_WORK2, REG_WORK2, REG_WORK3);
	write_jmp_target(no_accr_ovfl, (uintptr)get_target());
	uae_u32* no_accr_unfl = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 3, 0);
	uae_u32* no_accr_unfl_inexact = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 4, 0);
	LOAD_U32(REG_WORK3, FPSR_ACCR_UNFL);
	ORR_www(REG_WORK2, REG_WORK2, REG_WORK3);
	write_jmp_target(no_accr_unfl, (uintptr)get_target());
	write_jmp_target(no_accr_unfl_inexact, (uintptr)get_target());
	uae_u32* no_accr_inex = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 4, 0);
	LOAD_U32(REG_WORK3, FPSR_ACCR_INEX);
	ORR_www(REG_WORK2, REG_WORK2, REG_WORK3);
	write_jmp_target(no_accr_inex, (uintptr)get_target());
	LOAD_U64(REG_WORK3, (uintptr)&fpu.fpsr.accrued_exception);
	LDR_wXi(REG_WORK1, REG_WORK3, 0);
	ORR_www(REG_WORK1, REG_WORK1, REG_WORK2);
	STR_wXi(REG_WORK1, REG_WORK3, 0);
	MSR_NZCV_x(REG_WORK4);
}
LENDFUNC(NONE,NONE,2,raw_fmov_to_s_rr,(W4 d, FR s))

LOWFUNC(NONE,NONE,2,raw_fmov_to_w_rr,(W4 d, FR s, int targetIsReg))
{
	fmov_to_int_emit(d, s, 16);
}
LENDFUNC(NONE,NONE,2,raw_fmov_to_w_rr,(W4 d, FR s, int targetIsReg))

LOWFUNC(NONE,NONE,3,raw_fmov_to_b_rr,(W4 d, FR s, int targetIsReg))
{
	fmov_to_int_emit(d, s, 8);
}
LENDFUNC(NONE,NONE,3,raw_fmov_to_b_rr,(W4 d, FR s, int targetIsReg))

LOWFUNC(NONE,NONE,1,raw_fmov_d_ri_0,(FW r))
{
	MOVI_di(r, 0);
}
LENDFUNC(NONE,NONE,1,raw_fmov_d_ri_0,(FW r))

LOWFUNC(NONE,NONE,1,raw_fmov_d_ri_1,(FW r))
{
	FMOV_di(r, 0b01110000);
}
LENDFUNC(NONE,NONE,1,raw_fmov_d_ri_1,(FW r))

LOWFUNC(NONE,NONE,1,raw_fmov_d_ri_10,(FW r))
{
	FMOV_di(r, 0b00100100);
}
LENDFUNC(NONE,NONE,1,raw_fmov_d_ri_10,(FW r))

LOWFUNC(NONE,NONE,1,raw_fmov_d_ri_100,(FW r))
{
	MOV_wi(REG_WORK1, 100);
	SCVTF_dw(r, REG_WORK1);
}
LENDFUNC(NONE,NONE,1,raw_fmov_d_ri_100,(FW r))

LOWFUNC(NONE,READ,2,raw_fmov_d_rm,(FW r, MEMR m))
{
	LOAD_U64(REG_WORK1, m);
	LDR_dXi(r, REG_WORK1, 0);
}
LENDFUNC(NONE,READ,2,raw_fmov_d_rm,(FW r, MEMR m))

LOWFUNC(NONE,READ,2,raw_fmovs_rm,(FW r, MEMR m))
{
	LOAD_U64(REG_WORK1, m);
	LDR_sXi(r, REG_WORK1, 0);
	FCVT_ds(r, r);
}
LENDFUNC(NONE,READ,2,raw_fmovs_rm,(FW r, MEMR m))

LOWFUNC(NONE,NONE,3,raw_fmov_to_d_rrr,(W4 d1, W4 d2, FR s))
{
	FMOV_xd(d1, s);
	LSR_xxi(d2, d1, 32);
}
LENDFUNC(NONE,NONE,3,raw_fmov_to_d_rrr,(W4 d1, W4 d2, FR s))

LOWFUNC(NONE,NONE,2,raw_fsqrt_rr,(FW d, FR s))
{
	FSQRT_dd(d, s);
}
LENDFUNC(NONE,NONE,2,raw_fsqrt_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fabs_rr,(FW d, FR s))
{
	FABS_dd(d, s);
}
LENDFUNC(NONE,NONE,2,raw_fabs_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fneg_rr,(FW d, FR s))
{
	FNEG_dd(d, s);
}
LENDFUNC(NONE,NONE,2,raw_fneg_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fdiv_rr,(FRW d, FR s))
{
	FDIV_ddd(d, d, s);
}
LENDFUNC(NONE,NONE,2,raw_fdiv_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fadd_rr,(FRW d, FR s))
{
	FADD_ddd(d, d, s);
}
LENDFUNC(NONE,NONE,2,raw_fadd_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fmul_rr,(FRW d, FR s))
{
	FMUL_ddd(d, d, s);
}
LENDFUNC(NONE,NONE,2,raw_fmul_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fsub_rr,(FRW d, FR s))
{
	FSUB_ddd(d, d, s);
}
LENDFUNC(NONE,NONE,2,raw_fsub_rr,(FRW d, FR s))

/* FCMP sets NZCV to less=1000, equal=0110, greater=0010, unordered=0011.
   Convert that relation to the lazy FP_RESULT convention consumed by
   fflags_into_flags(): -1.0, +0.0, +1.0, or quiet NaN.  This deliberately
   does not subtract, so equal infinities compare equal and unequal infinite
   operands never publish the FPSR Infinity class. */
LOWFUNC(NONE,NONE,3,fcompare_result_emit,(FW result, FR d, FR s))
{
	assert(result != d && result != s);
	FCMP_dd(d, s);
	uae_u32* unordered = (uae_u32*)get_target();
	BVS_i(0);
	uae_u32* equal = (uae_u32*)get_target();
	BEQ_i(0);
	FMOV_di(result, 0b01110000); /* +1.0 */
	uae_u32* greater = (uae_u32*)get_target();
	BGT_i(0);
	LOAD_U64(REG_WORK1, 0xbff0000000000000ULL); /* -1.0 */
	FMOV_dx(result, REG_WORK1);
	uae_u32* end_negative = (uae_u32*)get_target();
	B_i(0);

	write_jmp_target(greater, (uintptr)get_target());
	uae_u32* end_positive = (uae_u32*)get_target();
	B_i(0);

	write_jmp_target(equal, (uintptr)get_target());
	/* The architectural compare result is N|Z for equal -0 and equal -Inf,
	   but only Z for equal finite negative values. Preserve that distinction
	   with signed zero in the lazy result carrier. */
	FMOV_xd(REG_WORK1, d);
	uae_u32* equal_positive = (uae_u32*)get_target();
	TBZ_xii(REG_WORK1, 63, 0);
	FCMP_d0(d);
	uae_u32* equal_negative_zero = (uae_u32*)get_target();
	BEQ_i(0);
	UBFX_xxii(REG_WORK2, REG_WORK1, 52, 11);
	CMP_xi(REG_WORK2, 2047);
	uae_u32* equal_negative_inf = (uae_u32*)get_target();
	BEQ_i(0);
	write_jmp_target(equal_positive, (uintptr)get_target());
	MOVI_di(result, 0); /* canonical +0.0 */
	uae_u32* end_equal = (uae_u32*)get_target();
	B_i(0);
	write_jmp_target(equal_negative_zero, (uintptr)get_target());
	write_jmp_target(equal_negative_inf, (uintptr)get_target());
	LOAD_U64(REG_WORK1, 0x8000000000000000ULL); /* canonical -0.0 */
	FMOV_dx(result, REG_WORK1);
	uae_u32* end_equal_negative = (uae_u32*)get_target();
	B_i(0);

	write_jmp_target(unordered, (uintptr)get_target());
	LOAD_U64(REG_WORK1, 0x7ff8000000000000ULL); /* canonical quiet NaN */
	FMOV_dx(result, REG_WORK1);

	write_jmp_target(end_negative, (uintptr)get_target());
	write_jmp_target(end_positive, (uintptr)get_target());
	write_jmp_target(end_equal, (uintptr)get_target());
	write_jmp_target(end_equal_negative, (uintptr)get_target());
}
LENDFUNC(NONE,NONE,3,fcompare_result_emit,(FW result, FR d, FR s))

LOWFUNC(NONE,NONE,2,raw_frndint_rr,(FW d, FR s))
{
	FRINTI_dd(d, s);
}
LENDFUNC(NONE,NONE,2,raw_frndint_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,2,raw_frndintz_rr,(FW d, FR s))
{
	FRINTZ_dd(d, s);
}
LENDFUNC(NONE,NONE,2,raw_frndintz_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fmod_rr,(FRW d, FR s))
{
	FDIV_ddd(SCRATCH_F64_1, d, s);
	FRINTZ_dd(SCRATCH_F64_1, SCRATCH_F64_1);
	FMSUB_dddd(d, SCRATCH_F64_1, s, d);
}
LENDFUNC(NONE,NONE,2,raw_fmod_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fsgldiv_rr,(FRW d, FR s))
{
	FCVT_sd(SCRATCH_F64_1, d);
	FCVT_sd(SCRATCH_F64_2, s);
	FDIV_sss(SCRATCH_F64_1, SCRATCH_F64_1, SCRATCH_F64_2);
	FCVT_ds(d, SCRATCH_F64_1);
}
LENDFUNC(NONE,NONE,2,raw_fsgldiv_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,1,raw_fcuts_r,(FRW r))
{
	FCVT_sd(SCRATCH_F64_1, r);
	FCVT_ds(r, SCRATCH_F64_1);
}
LENDFUNC(NONE,NONE,1,raw_fcuts_r,(FRW r))

LOWFUNC(NONE,NONE,2,raw_frem1_rr,(FRW d, FR s))
{
	FDIV_ddd(SCRATCH_F64_2, d, s);
	FRINTA_dd(SCRATCH_F64_2, SCRATCH_F64_2);
	FMSUB_dddd(d, SCRATCH_F64_2, s, d);
}
LENDFUNC(NONE,NONE,2,raw_frem1_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fsglmul_rr,(FRW d, FR s))
{
	FCVT_sd(SCRATCH_F64_1, d);
	FCVT_sd(SCRATCH_F64_2, s);
	FMUL_sss(SCRATCH_F64_1, SCRATCH_F64_1, SCRATCH_F64_2);
	FCVT_ds(d, SCRATCH_F64_1);
}
LENDFUNC(NONE,NONE,2,raw_fsglmul_rr,(FRW d, FR s))

LOWFUNC(NONE,NONE,2,raw_fmovs_rr,(FW d, FR s))
{
	FCVT_sd(SCRATCH_F64_1, s);
	FCVT_ds(d, SCRATCH_F64_1);
}
LENDFUNC(NONE,NONE,2,raw_fmovs_rr,(FW d, FR s))

LOWFUNC(NONE,NONE,3,raw_ffunc_rr,(double (*func)(double), FW d, FR s))
{
	FMOV_dd(0, s);

	LOAD_U64(REG_WORK1, (uintptr)func);

	STR_xXpre(RLR_INDEX, RSP_INDEX, -16);
	BLR_x(REG_WORK1);
	LDR_xXpost(RLR_INDEX, RSP_INDEX, 16);

	FMOV_dd(d, 0);
}
LENDFUNC(NONE,NONE,3,raw_ffunc_rr,(double (*func)(double), FW d, FR s))

LOWFUNC(NONE,NONE,3,raw_fpowx_rr,(uae_u32 x, FW d, FR s))
{
	double (*func)(double,double) = pow;

	if(x == 2) {
		FMOV_di(0, 0b00000000); // load imm #2 into first reg
	} else {
		FMOV_di(0, 0b00100100); // load imm #10 into first reg
	}

	FMOV_dd(1, s);

	LOAD_U64(REG_WORK1, (uintptr)func);

	STR_xXpre(RLR_INDEX, RSP_INDEX, -16);
	BLR_x(REG_WORK1);
	LDR_xXpost(RLR_INDEX, RSP_INDEX, 16);

	FMOV_dd(d, 0);
}
LENDFUNC(NONE,NONE,3,raw_fpowx_rr,(uae_u32 x, FW d, FR s))

LOWFUNC(NONE,WRITE,2,raw_fp_from_exten_mr,(RR4 adr, FR s))
{
	FMOV_xd(REG_WORK1, s);
	FCMP_d0(s);
	ADD_xxwEX(REG_WORK4, R_MEMSTART, adr, EX_UXTW);

	uae_u32* branchadd_iszero = (uae_u32*)get_target();
	BEQ_i(0); // iszero

	UBFX_xxii(REG_WORK2, REG_WORK1, 52, 11); // get exponent
	CMP_xi(REG_WORK2, 2047);

	uae_u32* branchadd_isnan = (uae_u32*)get_target();
	BEQ_i(0); 				// isnan

	MOV_xi(REG_WORK3, 15360);              	    // diff of bias between double and long double
	ADD_xxx(REG_WORK2, REG_WORK2, REG_WORK3); 	// exponent done
	UBFX_xxii(REG_WORK3, REG_WORK1, 63, 1);     // extract sign
	LSL_xxi(REG_WORK3, REG_WORK3, 31);
	ORR_xxxLSLi(REG_WORK2, REG_WORK3, REG_WORK2, 16); // merge sign and exponent

	REV32_xx(REG_WORK2, REG_WORK2);
	STRH_wXi(REG_WORK2, REG_WORK4, 0);         	// write exponent
	ADD_xxi(REG_WORK4, REG_WORK4, 4);

	LSL_xxi(REG_WORK1, REG_WORK1, 11);          // shift mantissa to correct position
	REV_xx(REG_WORK1, REG_WORK1);
	SET_xxbit(REG_WORK1, REG_WORK1, 7);        // insert explicit 1
	STR_xXi(REG_WORK1, REG_WORK4, 0);
	uae_u32* branchadd_end = (uae_u32*)get_target();
	B_i(0);            // end_of_op

	// isnan
	write_jmp_target(branchadd_isnan, (uintptr)get_target());
	MOV_xish(REG_WORK1, 0x7fff, 16);
	MOVN_xi(REG_WORK2, 0);
	B_i(4);

	// iszero
	write_jmp_target(branchadd_iszero, (uintptr)get_target());
	UBFX_xxii(REG_WORK1, REG_WORK1, 63, 1);     // extract sign
	LSL_xxi(REG_WORK1, REG_WORK1, 31);
	MOV_xi(REG_WORK2, 0);

	REV32_xx(REG_WORK1, REG_WORK1);
	STR_wXi(REG_WORK1, REG_WORK4, 0);
	STP_wwXi(REG_WORK2, REG_WORK2, REG_WORK4, 4);

	// end_of_op
	write_jmp_target(branchadd_end, (uintptr)get_target());
}
LENDFUNC(NONE,WRITE,2,raw_fp_from_exten_mr,(RR4 adr, FR s))

LOWFUNC(NONE,READ,2,raw_fp_to_exten_rm,(FW d, RR4 adr))
{
	ADD_xxwEX(REG_WORK3, R_MEMSTART, adr, EX_UXTW);

	ADD_xxi(REG_WORK1, REG_WORK3, 4);
	LDR_xXi(REG_WORK1, REG_WORK1, 0);
	CLEAR_xxbit(REG_WORK1, REG_WORK1, 7); 	// clear explicit 1
	REV_xx(REG_WORK1, REG_WORK1);

	LDRH_wXi(REG_WORK4, REG_WORK3, 0);
	REV16_xx(REG_WORK4, REG_WORK4);				// exponent now in lower half

	ANDS_xx7fff(REG_WORK2, REG_WORK4);
	uae_u32* branchadd_notzero = (uae_u32*)get_target();
	BNE_i(0);				// not_zero

	uae_u32* branchadd_notzero2 = (uae_u32*)get_target();
	CBNZ_xi(REG_WORK1, 0);          // not zero

	// zero
	MOVI_di(d, 0);
	uae_u32* branchadd_end = (uae_u32*)get_target();
	TBZ_xii(REG_WORK4, 15, 0); // end_of_op
	MOV_xish(REG_WORK1, 0x8000, 48);
	FMOV_dx(d, REG_WORK1);
	uae_u32* branchadd_end2 = (uae_u32*)get_target();
	B_i(0);					// end_of_op

	// not_zero
	write_jmp_target(branchadd_notzero, (uintptr)get_target());
	write_jmp_target(branchadd_notzero2, (uintptr)get_target());
	MOV_xi(REG_WORK3, 15360);                 // diff of bias between double and long double
	SUB_xxx(REG_WORK2, REG_WORK2, REG_WORK3);	// exponent done, ToDo: check for carry -> result gets Inf in double
	UBFX_xxii(REG_WORK4, REG_WORK4, 15, 1);		// extract sign
	BFI_xxii(REG_WORK2, REG_WORK4, 11, 1);		// insert sign
	LSR_xxi(REG_WORK1, REG_WORK1, 11);				// shift mantissa to correct position
	LSL_xxi(REG_WORK2, REG_WORK2, 52);
	ORR_xxx(REG_WORK1, REG_WORK1, REG_WORK2);
	FMOV_dx(d, REG_WORK1);

	// end_of_op
	write_jmp_target(branchadd_end, (uintptr)get_target());
	write_jmp_target(branchadd_end2, (uintptr)get_target());
}
LENDFUNC(NONE,READ,2,raw_fp_to_exten_rm,(FW d, RR4 adr))

LOWFUNC(NONE,WRITE,2,raw_fp_from_double_mr,(RR4 adr, FR s))
{
	REV64_dd(SCRATCH_F64_1, s);
	STR_dXx(SCRATCH_F64_1, adr, R_MEMSTART);
}
LENDFUNC(NONE,WRITE,2,raw_fp_from_double_mr,(RR4 adr, FR s))

LOWFUNC(NONE,READ,2,raw_fp_to_double_rm,(FW d, RR4 adr))
{
	LDR_dXx(d, adr, R_MEMSTART);
	REV64_dd(d, d);
}
LENDFUNC(NONE,READ,2,raw_fp_to_double_rm,(FW d, RR4 adr))

STATIC_INLINE void raw_fflags_into_flags(int r)
{
	FCMP_d0(r);
}

LOWFUNC(NONE,NONE,2,raw_fp_fscc_ri,(RW4 d, int cc))
{
	switch (cc) {
		case NATIVE_CC_F_F: // Never
			CLEAR_LOW8_xx(d, d);
			break;

		case NATIVE_CC_F_EQ: // Equal
			CSETM_wc(REG_WORK1, NATIVE_CC_EQ);
			BFXIL_xxii(d, REG_WORK1, 0, 8);
			break;

		case NATIVE_CC_F_OGT: // Set if valid and greater than
			BVS_i(4);		// do not set if NaN
			BLE_i(3);		// do not set if less or equal
			SET_LOW8_xx(d, d);
			B_i(2);
			CLEAR_LOW8_xx(d, d);
			break;

		case NATIVE_CC_F_OGE: // Set if valid and greater or equal
			BVS_i(4);		// do not set if NaN
			BCC_i(3);		// do not set if carry cleared
			SET_LOW8_xx(d, d);
			B_i(2);
			CLEAR_LOW8_xx(d, d);
			break;

		case NATIVE_CC_F_OLT: // Set if vaild and less than
			BVS_i(4);		// do not set if NaN
			BCS_i(3);		// do not set if carry set
			SET_LOW8_xx(d, d);
			B_i(2);
			CLEAR_LOW8_xx(d, d);
			break;

		case NATIVE_CC_F_OLE: // Set if valid and less or equal
			BVS_i(4);		// do not set if NaN
			BGT_i(3);		// do not set if greater than
			SET_LOW8_xx(d, d);
			B_i(2);
			CLEAR_LOW8_xx(d, d);
			break;

		case NATIVE_CC_F_OGL: // Set if valid and greator or less
			BVS_i(4);		// do not set if NaN
			BEQ_i(3);		// do not set if equal
			SET_LOW8_xx(d, d);
			B_i(2);
			CLEAR_LOW8_xx(d, d);
			break;

		case NATIVE_CC_F_OR: // Set if valid
			CSETM_wc(REG_WORK1, NATIVE_CC_VC);    // do not set if NaN
			BFXIL_xxii(d, REG_WORK1, 0, 8);
			break;

		case NATIVE_CC_F_UN: // Set if NAN
			CSETM_wc(REG_WORK1, NATIVE_CC_VS);    // do not set if valid
			BFXIL_xxii(d, REG_WORK1, 0, 8);
			break;

		case NATIVE_CC_F_UEQ: // Set if NAN or equal
			BVS_i(2); 	// set if NaN
			BNE_i(3);		// do not set if greater or less
			SET_LOW8_xx(d, d);
			B_i(2);
			CLEAR_LOW8_xx(d, d);
			break;

		case NATIVE_CC_F_UGT: // Set if NAN or greater than
			BVS_i(2); 	// set if NaN
			BLS_i(3);		// do not set if lower or same
			SET_LOW8_xx(d, d);
			B_i(2);
			CLEAR_LOW8_xx(d, d);
			break;

		case NATIVE_CC_F_UGE: // Set if NAN or greater or equal
			BVS_i(2); 	// set if NaN
			BMI_i(3);		// do not set if lower
			SET_LOW8_xx(d, d);
			B_i(2);
			CLEAR_LOW8_xx(d, d);
			break;

		case NATIVE_CC_F_ULT: // Set if NAN or less than
			BVS_i(2); 	// set if NaN
			BGE_i(3);		// do not set if greater or equal
			SET_LOW8_xx(d, d);
			B_i(2);
			CLEAR_LOW8_xx(d, d);
			break;

		case NATIVE_CC_F_ULE: // Set if NAN or less or equal
			BVS_i(2); 	// set if NaN
			BGT_i(3);		// do not set if greater
			SET_LOW8_xx(d, d);
			B_i(2);
			CLEAR_LOW8_xx(d, d);
			break;

		case NATIVE_CC_F_NE: // Not equal, including unordered
			CSETM_wc(REG_WORK1, NATIVE_CC_NE);
			BFXIL_xxii(d, REG_WORK1, 0, 8);
			break;

		case NATIVE_CC_F_T: // Always
			SET_LOW8_xx(d, d);
			break;

		default:
			jit_abort("invalid AArch64 FScc condition");
	}
}
LENDFUNC(NONE,NONE,2,raw_fp_fscc_ri,(RW4 d, int cc))
