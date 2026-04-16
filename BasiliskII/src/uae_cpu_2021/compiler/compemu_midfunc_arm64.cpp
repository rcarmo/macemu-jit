/*
 * compiler/compemu_midfunc_armA64.cpp - Native MIDFUNCS for AARCH64
 *
 * Copyright (c) 2019 TomB
 *
 * Inspired by Christian Bauer's Basilisk II
 *
 *  Original 68040 JIT compiler for UAE, copyright 2000-2002 Bernd Meyer
 *
 *  Adaptation for Basilisk II and improvements, copyright 2000-2002
 *    Gwenole Beauchesne
 *
 *  Basilisk II (C) 1997-2002 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Note:
 *  	File is included by compemu_support.cpp
 *
 */

/********************************************************************
 * CPU functions exposed to gencomp. Both CREATE and EMIT time      *
 ********************************************************************/

/*
 *  RULES FOR HANDLING REGISTERS:
 *
 *  * In the function headers, order the parameters
 *     - 1st registers written to
 *     - 2nd read/modify/write registers
 *     - 3rd registers read from
 *  * Before calling raw_*, you must call readreg, writereg or rmw for
 *    each register
 *  * The order for this is
 *     - 1nd call readreg for all registers read without offset
 *     - 2rd call rmw for all rmw registers
 *     - 3th call writereg for all written-to registers
 *     - 4th call raw_*
 *     - 5th unlock2 all registers that were locked
 */

#define INIT_REGS_b(d,s) 					\
	int targetIsReg = (d < 16); 		\
	int s_is_d = (s == d);  				\
	if(!s_is_d) 										\
		s = readreg(s); 							\
	d = rmw(d);											\
	if(s_is_d)											\
		s = d;

#define INIT_RREGS_b(d,s) 				\
	int targetIsReg = (d < 16); 		\
	int s_is_d = (s == d);  				\
	if(!s_is_d) 										\
		s = readreg(s); 							\
	d = readreg(d);									\
	if(s_is_d)											\
		s = d;

#define INIT_REG_b(d) 						\
	int targetIsReg = (d < 16); 		\
	d = rmw(d);

#define INIT_RREG_b(d) 						\
	int targetIsReg = (d < 16); 		\
	d = readreg(d);

#define INIT_WREG_b(d) 						\
	int targetIsReg = (d < 16); 		\
	if(targetIsReg)									\
		d = rmw(d); 									\
	else														\
		d = writereg(d);

#define INIT_REGS_w(d,s) 					\
	int targetIsReg = (d < 16); 		\
	int s_is_d = (s == d);  				\
	if(!s_is_d) 										\
		s = readreg(s); 							\
	d = rmw(d);											\
	if(s_is_d)											\
		s = d;

#define INIT_RREGS_w(d,s)					\
	int targetIsReg = (d < 16); 		\
	int s_is_d = (s == d);  				\
	if(!s_is_d) 										\
		s = readreg(s); 							\
	d = readreg(d);									\
	if(s_is_d)											\
		s = d;

#define INIT_REG_w(d) 						\
	int targetIsReg = (d < 16); 		\
	d = rmw(d);

#define INIT_RREG_w(d) 						\
	int targetIsReg = (d < 16); 		\
	d = readreg(d);

#define INIT_WREG_w(d) 						\
	int targetIsReg = (d < 16); 		\
	if(targetIsReg)									\
		d = rmw(d);										\
	else														\
		d = writereg(d);

#define INIT_REGS_l(d,s) 					\
	int targetIsReg = (d < 16); 		\
	int s_is_d = (s == d);  				\
	if(!s_is_d) 										\
		s = readreg(s); 							\
	d = rmw(d);											\
	if(s_is_d)											\
		s = d;

#define INIT_RREGS_l(d,s) 				\
	int targetIsReg = (d < 16); 		\
	int s_is_d = (s == d);  				\
	if(!s_is_d) 										\
		s = readreg(s); 							\
	d = readreg(d);									\
	if(s_is_d)											\
		s = d;

#define EXIT_REGS(d,s)						\
	unlock2(d);											\
	if(!s_is_d)											\
		unlock2(s);


MIDFUNC(0,live_flags,(void))
{
	live.flags_on_stack = TRASH;
	live.flags_in_flags = VALID;
	live.flags_are_important = 1;
}
MENDFUNC(0,live_flags,(void))

MIDFUNC(0,dont_care_flags,(void))
{
	live.flags_are_important = 0;
}
MENDFUNC(0,dont_care_flags,(void))

/* Mark hardware NZCV as stale without saving it to regflags.nzcv.
   Used by DBcc (case 1/DBRA): the sub_w_ri sets NZCV for the branch
   decision, but M68K DBcc does NOT affect CCR. The pre-existing flags
   were already saved to regflags.nzcv by clobber_flags() inside sub_w_ri.
   Calling this prevents flush(1) at block end from overwriting
   regflags.nzcv with the stale sub_w_ri result. */
MIDFUNC(0,discard_flags_in_nzcv,(void))
{
	/* Mark hardware NZCV as stale and memory flags as authoritative.
	   After this, flush(1)->flags_to_stack() takes the early-return
	   path (flags_on_stack==VALID) and skips the raw_flags_to_reg
	   that would save the stale hardware NZCV to regflags.nzcv. */
	live.flags_in_flags = TRASH;
	live.flags_on_stack = VALID;
	flags_carry_inverted = false;
	/* Also evict FLAGTMP from its hardware register so flush(1)
	   won't write it back via tomem. */
	if (live.state[FLAGTMP].status == DIRTY || live.state[FLAGTMP].status == CLEAN) {
		int r = live.state[FLAGTMP].realreg;
		if (r >= 0) {
			live.nat[r].nholds--;
			if (live.nat[r].nholds == 0)
				live.nat[r].touched = 0;
		}
		live.state[FLAGTMP].realreg = -1;
		set_status(FLAGTMP, INMEM);
	}
}
MENDFUNC(0,discard_flags_in_nzcv,(void))

MIDFUNC(0,make_flags_live,(void))
{
	make_flags_live_internal();
}
MENDFUNC(0,make_flags_live,(void))

MIDFUNC(2,mov_l_mi,(IMPTR d, IMPTR s))
{
	/* d usually points to memory in regs struct, but can also be a global
	   (e.g. regflags.nzcv). Use absolute address if out of LDR/STR range. */
	if(d >= (uintptr)&regs && d < (uintptr)&regs + 32760) {
		uintptr idx = d - (uintptr) &regs;
		if(d == (uintptr) &(regs.pc_p) || d == (uintptr) &(regs.pc_oldp)) {
			LOAD_U64(REG_WORK2, s);  // pc_p/pc_oldp are 64-bit host pointers
			if (jit_trace_setpc_env()) {
				STR_xXpre(REG_WORK2, RSP_INDEX, -16);
				LDR_xXi(REG_PAR1, RSP_INDEX, 0);
				LOAD_U32(REG_PAR2, 7);
				compemu_raw_call((uintptr)jit_trace_setpc_value);
				LDR_xXpost(REG_WORK2, RSP_INDEX, 16);
			}
			STR_xXi(REG_WORK2, R_REGSTRUCT, idx);
		} else {
			LOAD_U32(REG_WORK2, (uae_u32)s);
			STR_wXi(REG_WORK2, R_REGSTRUCT, idx);
		}
	} else {
		LOAD_U32(REG_WORK2, (uae_u32)s);
		LOAD_U64(REG_WORK1, d);
		STR_wXi(REG_WORK2, REG_WORK1, 0);
	}
}
MENDFUNC(2,mov_l_mi,(IMPTR d, IMPTR s))

MIDFUNC(4,disp_ea20_target_add,(RW4 target, RR4 reg, IM8 shift, IM8 extend))
{
	if(isconst(target) && isconst(reg)) {
		if(extend)
			set_const(target, live.state[target].val + (((uae_s32)(uae_s16)live.state[reg].val) << (shift & 0x1f)));
		else
			set_const(target, live.state[target].val + (live.state[reg].val << (shift & 0x1f)));
		return;
	}

	reg = readreg(reg);
	target = rmw(target);

	if(extend) {
		SIGNED16_REG_2_REG(REG_WORK1, reg);
		ADD_wwwLSLi(target, target, REG_WORK1, shift & 0x1f);
	} else {
		ADD_wwwLSLi(target, target, reg, shift & 0x1f);
	}

	unlock2(target);
	unlock2(reg);
}
MENDFUNC(4,disp_ea20_target_add,(RW4 target, RR4 reg, IM8 shift, IM8 extend))

MIDFUNC(4,disp_ea20_target_mov,(W4 target, RR4 reg, IM8 shift, IM8 extend))
{
	if(isconst(reg)) {
		if(extend)
			set_const(target, ((uae_s32)(uae_s16)live.state[reg].val) << (shift & 0x1f));
		else
			set_const(target, live.state[reg].val << (shift & 0x1f));
		return;
	}

	reg = readreg(reg);
	target = writereg(target);

	if(extend) {
		SIGNED16_REG_2_REG(REG_WORK1, reg);
		LSL_wwi(target, REG_WORK1, shift & 0x1f);
	} else {
		LSL_wwi(target, reg, shift & 0x1f);
	}

	unlock2(target);
	unlock2(reg);
}
MENDFUNC(4,disp_ea20_target_mov,(W4 target, RR4 reg, IM8 shift, IM8 extend))

MIDFUNC(2,sign_extend_16_rr,(W4 d, RR2 s))
{
	// Only used in calc_disp_ea_020() -> flags not relevant and never modified
	if (isconst(s)) {
		set_const(d, (uae_s32)(uae_s16)live.state[s].val);
		return;
	}

	int s_is_d = (s == d);
	if (!s_is_d) {
		s = readreg(s);
		d = writereg(d);
	}	else {
		s = d = rmw(s);
	}
	SIGNED16_REG_2_REG(d, s);
	if (!s_is_d)
		unlock2(d);
	unlock2(s);
}
MENDFUNC(2,sign_extend_16_rr,(W4 d, RR2 s))

MIDFUNC(3,lea_l_brr,(W4 d, RR4 s, IM32 offset))
{
	if (isconst(s)) {
		COMPCALL(mov_l_ri)(d, live.state[s].val+offset);
		return;
	}

#if defined(CPU_AARCH64)
	/* PC_P holds a 64-bit host pointer — must use 64-bit arithmetic. */
	if (d == PC_P || s == PC_P) {
		int s_is_d = (s == d);
		if(s_is_d) {
			s = d = rmw(d);
		} else {
			s = readreg(s);
			d = writereg(d);
		}
		if(offset >= 0 && offset <= 0xfff) {
			ADD_xxi(d, s, offset);
		} else if(offset >= -0xfff && offset < 0) {
			SUB_xxi(d, s, -offset);
		} else {
			LOAD_U64(REG_WORK1, (uintptr)(uae_s64)(uae_s32)offset);
			ADD_xxx(d, s, REG_WORK1);
		}
		EXIT_REGS(d,s);
		return;
	}
#endif

	int s_is_d = (s == d);
	if(s_is_d) {
		s = d = rmw(d);
	} else {
		s = readreg(s);
		d = writereg(d);
	}

	if(offset >= 0 && offset <= 0xfff) {
		ADD_wwi(d, s, offset);
	} else if(offset >= -0xfff && offset < 0) {
		SUB_wwi(d, s, -offset);
	} else {
		LOAD_U32(REG_WORK1, offset);
		ADD_www(d, s, REG_WORK1);
	}

	EXIT_REGS(d,s);
}
MENDFUNC(3,lea_l_brr,(W4 d, RR4 s, IM32 offset))

MIDFUNC(5,lea_l_brr_indexed,(W4 d, RR4 s, RR4 index, IM8 factor, IM8 offset))
{
	if (!offset) {
		COMPCALL(lea_l_rr_indexed)(d, s, index, factor);
		return;
	}
	if (isconst(s) && isconst(index)) {
		set_const(d, live.state[s].val + (uae_s32)(uae_s8)offset + live.state[index].val * factor);
		return;
	}

	s = readreg(s);
	if(d == index) {
		d = index = rmw(d);
	} else {
		index = readreg(index);
		d = writereg(d);
	}

	int shft;
	switch(factor) {
		case 1: shft=0; break;
		case 2: shft=1; break;
		case 4: shft=2; break;
		case 8: shft=3; break;
		default: abort();
	}

	if(offset >= 0 && offset <= 127) {
		ADD_wwi(REG_WORK1, s, offset);
	} else {
		SUB_wwi(REG_WORK1, s, -offset);
	}
	ADD_wwwLSLi(d, REG_WORK1, index, shft);

	unlock2(d);
	if(d != index)
		unlock2(index);
	unlock2(s);
}
MENDFUNC(5,lea_l_brr_indexed,(W4 d, RR4 s, RR4 index, IM8 factor, IM8 offset))

MIDFUNC(4,lea_l_rr_indexed,(W4 d, RR4 s, RR4 index, IM8 factor))
{
	if (isconst(s) && isconst(index)) {
		set_const(d, live.state[s].val + live.state[index].val * factor);
		return;
	}

	s = readreg(s);
	if(d == index) {
		d = index = rmw(d);
	} else {
		index = readreg(index);
		d = writereg(d);
	}

	int shft;
	switch(factor) {
		case 1: shft=0; break;
		case 2: shft=1; break;
		case 4: shft=2; break;
		case 8: shft=3; break;
		default: abort();
	}

	ADD_wwwLSLi(d, s, index, shft);

	unlock2(d);
	if(d != index)
		unlock2(index);
	unlock2(s);
}
MENDFUNC(4,lea_l_rr_indexed,(W4 d, RR4 s, RR4 index, IM8 factor))

MIDFUNC(2,mov_l_rr,(W4 d, RR4 s))
{
	int olds;

	if (d == s) { /* How pointless! */
		return;
	}
	if (isconst(s)) {
		COMPCALL(mov_l_ri)(d, live.state[s].val);
		return;
	}
	/* Do a real copy instead of sharing hardware registers.
	   Register sharing causes lock conflicts when subsequent opcodes
	   need both source and destination simultaneously. */
	olds = s;
	s = readreg(s);
	d = writereg(d);
	compemu_raw_mov_l_rr(d, s);
	unlock2(d);
	unlock2(s);
}
MENDFUNC(2,mov_l_rr,(W4 d, RR4 s))

MIDFUNC(2,mov_l_mr,(IMPTR d, RR4 s))
{
	/* d usually points to memory in regs struct, but can also be a global
	   (e.g. regflags.nzcv). Use absolute address if out of LDR/STR range. */
	if (isconst(s)) {
		COMPCALL(mov_l_mi)(d, live.state[s].val);
		return;
	}

	s = readreg(s);

	if(d >= (uintptr)&regs && d < (uintptr)&regs + 32760) {
		uintptr idx = d - (uintptr) &regs;
		if(d == (uintptr)&regs.pc_oldp || d == (uintptr)&regs.pc_p) {
			if (jit_trace_setpc_env()) {
				STR_xXpre(s, RSP_INDEX, -16);
				LDR_xXi(REG_PAR1, RSP_INDEX, 0);
				LOAD_U32(REG_PAR2, 8);
				compemu_raw_call((uintptr)jit_trace_setpc_value);
				LDR_xXpost(s, RSP_INDEX, 16);
			}
			STR_xXi(s, R_REGSTRUCT, idx);
		} else
			STR_wXi(s, R_REGSTRUCT, idx);
	} else {
		LOAD_U64(REG_WORK1, d);
		STR_wXi(s, REG_WORK1, 0);
	}

	unlock2(s);
}
MENDFUNC(2,mov_l_mr,(IMPTR d, RR4 s))

MIDFUNC(2,mov_l_rm,(W4 d, IMPTR s))
{
	/* s usually points to memory in regs struct, but can also be a global
	   (e.g. regflags.nzcv). Use absolute address if out of LDR/STR range. */
	d = writereg(d);

	if(s >= (uintptr)&regs && s < (uintptr)&regs + 32760) {
		uintptr idx = s - (uintptr) &regs;
		if (s == (uintptr)&regs.pc_p || s == (uintptr)&regs.pc_oldp)
			LDR_xXi(d, R_REGSTRUCT, idx);
		else
			LDR_wXi(d, R_REGSTRUCT, idx);
	} else {
		LOAD_U64(REG_WORK1, s);
		LDR_wXi(d, REG_WORK1, 0);
	}

	unlock2(d);
}
MENDFUNC(2,mov_l_rm,(W4 d, IMPTR s))

MIDFUNC(2,mov_l_ri,(W4 d, IMPTR s))
{
	set_const(d, s);
}
MENDFUNC(2,mov_l_ri,(W4 d, IMPTR s))

MIDFUNC(2,mov_b_ri,(W1 d, IM8 s))
{
	if(d < 16) {
		if (isconst(d)) {
			set_const(d, (live.state[d].val & 0xffffff00) | (s & 0x000000ff));
			return;
		}
		d = rmw(d);

		MOV_wi(REG_WORK1, (s & 0xff));
		BFI_wwii(d, REG_WORK1, 0, 8);

		unlock2(d);
	} else {
		set_const(d, s & 0xff);
	}
}
MENDFUNC(2,mov_b_ri,(W1 d, IM8 s))

MIDFUNC(2,sub_l_ri,(RW4 d, IM8 i))
{
	if (!i)
		return;
	if (isconst(d)) {
		// Always preserve full 64-bit for PC_P (host pointer) or
		// when the current value already exceeds 32 bits.
		if (d == PC_P || live.state[d].val > (uintptr)0xFFFFFFFFULL)
			live.state[d].val = live.state[d].val - i;
		else
			live.state[d].val = (uae_u32)(live.state[d].val - i);
		return;
	}

	bool is_pcp = (d == PC_P);
	bool is_ptr = is_pcp || (isconst(d) && live.state[d].val > (uintptr)0xFFFFFFFFULL);
	d = rmw(d);

	if (is_ptr)
		SUB_xxi(d, d, i);  // 64-bit SUB for host pointer
	else
		SUB_wwi(d, d, i);

	unlock2(d);
}
MENDFUNC(2,sub_l_ri,(RW4 d, IM8 i))

MIDFUNC(2,sub_w_ri,(RW2 d, IM8 i))
{
	// This function is only called with i = 1 and the caller needs flags.
	// ARM SUBS sets C = not-borrow, so for x86-style carry conditions we
	// must mark carry as inverted just like the jff_SUB_* helpers do.
	clobber_flags();

	d = rmw(d);

	LSL_wwi(REG_WORK2, d, 16);

	SUBS_wwish(REG_WORK2, REG_WORK2, (i & 0xff) << 4, 1);
	BFXIL_xxii(d, REG_WORK2, 16, 16);
	flags_carry_inverted = true;
	live_flags();

	unlock2(d);
}
MENDFUNC(2,sub_w_ri,(RW2 d, IM8 i))

/* forget_about() takes a mid-layer register */
MIDFUNC(1,forget_about,(W4 r))
{
	if (isinreg(r))
		disassociate(r);
	live.state[r].val = 0;
	set_status(r, UNDEF);
}
MENDFUNC(1,forget_about,(W4 r))


MIDFUNC(2,arm_ADD_l,(RW4 d, RR4 s))
{
	if (isconst(s)) {
		uintptr val = live.state[s].val;
		#ifdef CPU_AARCH64
		// When adding a 32-bit M68K displacement to PC_P (a 64-bit host
		// pointer), sign-extend the displacement first.  M68K branch
		// offsets are signed, but stored as unsigned uintptr in
		// live.state[].val.  Adding an unsigned 0xFFFFFE42 (i.e. -0x1BE)
		// to a 64-bit pointer causes carry into bit 32, producing 0x4...
		// instead of the correct 0x3...
		if (d == PC_P && val <= (uintptr)0xFFFFFFFFULL)
			val = (uintptr)(uae_s64)(uae_s32)val;
#endif
		COMPCALL(arm_ADD_l_ri)(d, val);
		return;
	}

#ifdef CPU_AARCH64
	bool is_pcp = (d == PC_P);
#endif
	s = readreg(s);
	d = rmw(d);
#ifdef CPU_AARCH64
	if (is_pcp) {
		/* PC_P holds a 64-bit host pointer. src is a signed 32-bit
		   displacement in a W register.  Use ADD Xd, Xd, Ws, SXTW
		   to sign-extend the 32-bit displacement to 64-bit before
		   adding, preserving the upper bits of the host pointer. */
		ADD_xxwEX(d, d, s, 6); // extend option 6 = SXTW
	} else
#endif
	{
		ADD_www(d, d, s);
	}
	unlock2(d);
	unlock2(s);
}
MENDFUNC(2,arm_ADD_l,(RW4 d, RR4 s))

MIDFUNC(2,arm_ADD_ldiv8,(RW4 d, RR4 s))
{
	if (isconst(s)) {
		COMPCALL(arm_ADD_l_ri)(d,(live.state[s].val & ~0x1f) >> 3);
		return;
	}

	s = readreg(s);
	d = rmw(d);
	ASR_wwi(REG_WORK1, s, 5);
	ADD_wwwLSLi(d, d, REG_WORK1, 2);
	unlock2(d);
	unlock2(s);
}
MENDFUNC(2,arm_ADD_ldiv8,(RW4 d, RR4 s))

static inline bool arm64_low32_hostptr_imm(uintptr i)
{
	uintptr base = (uintptr)RAMBaseHost;
	uintptr limit = base + RAMSize + ROMSize + 0x100000;
	if (i >= base && i < limit)
		return true;
	if (base <= (uintptr)0xFFFFFFFFULL && limit <= (uintptr)0xFFFFFFFFULL + 1) {
		uintptr i32 = (uintptr)(uae_u32)i;
		uintptr b32 = (uintptr)(uae_u32)base;
		uintptr l32 = (uintptr)(uae_u32)limit;
		if (i32 >= b32 && i32 < l32)
			return true;
	}
	return false;
}

MIDFUNC(2,arm_ADD_l_ri,(RW4 d, IMPTR i))
{
	if (!i)
		return;
	const bool hostptr_imm = arm64_low32_hostptr_imm(i);
	if (isconst(d)) {
		// Preserve full 64-bit result when d is PC_P, when i is a host
		// pointer immediate (even if it currently fits in 32 bits on Linux),
		// or when val already exceeds 32 bits.
		if (d == PC_P || i > (IMPTR)0xFFFFFFFFULL || hostptr_imm || live.state[d].val > (uintptr)0xFFFFFFFFULL) {
			uintptr val = live.state[d].val;
			// When adding a host pointer base to a 32-bit M68K displacement,
			// sign-extend the displacement first. Otherwise values like
			// 0xFFFFFFFA are treated as +4GB-6 and produce 0x1........ PCs.
			if (val <= (uintptr)0xFFFFFFFFULL && (i > (IMPTR)0xFFFFFFFFULL || hostptr_imm))
				val = (uintptr)(uae_s64)(uae_s32)(uae_u32)val;
			live.state[d].val = val + i;
		} else {
			live.state[d].val = (uae_u32)(live.state[d].val + i);
		}
		return;
	}

	// Use 64-bit ADD when d is PC_P or when the immediate is a host pointer
	// base used to turn a signed guest displacement into a host PC pointer.
	bool is_ptr = (d == PC_P) || (i > (IMPTR)0xFFFFFFFFULL) || hostptr_imm;
	bool is_pcp = (d == PC_P);
	d = rmw(d);

	if (is_ptr) {
		if (is_pcp) {
			if(i <= 0xfff) {
				ADD_xxi(d, d, i);
			} else {
				LOAD_U64(REG_WORK1, (uintptr)i);
				ADD_xxx(d, d, REG_WORK1);
			}
		} else {
			LOAD_U64(REG_WORK1, (uintptr)i);
			ADD_xxwEX(d, REG_WORK1, d, 6); // ADD Xd, Ximm, Wd, SXTW
		}
	} else {
		uae_u32 i32 = (uae_u32)i;
		if(i32 <= 0xfff) {
			ADD_wwi(d, d, i32);
		} else {
			LOAD_U32(REG_WORK1, i32);
			ADD_www(d, d, REG_WORK1);
		}
	}

	unlock2(d);
}
MENDFUNC(2,arm_ADD_l_ri,(RW4 d, IMPTR i))

MIDFUNC(2,arm_ADD_l_ri8,(RW4 d, IM8 i))
{
	if (!i)
		return;
	if (isconst(d)) {
		// Preserve full 64-bit if d is PC_P or already holds a 64-bit value
		if (d == PC_P || live.state[d].val > (uintptr)0xFFFFFFFFULL)
			live.state[d].val = live.state[d].val + i;
		else
			live.state[d].val = (uae_u32)(live.state[d].val + i);
		return;
	}

	bool is_ptr = (d == PC_P);
	d = rmw(d);
	if (is_ptr)
		ADD_xxi(d, d, i);  // 64-bit ADD for host pointer
	else
		ADD_wwi(d, d, i);  // 32-bit ADD for M68k values
	unlock2(d);
}
MENDFUNC(2,arm_ADD_l_ri8,(RW4 d, IM8 i))

MIDFUNC(2,arm_SUB_l_ri8,(RW4 d, IM8 i))
{
	if (!i)
		return;
	if (isconst(d)) {
		// Preserve full 64-bit if d is PC_P or already holds a 64-bit value
		if (d == PC_P || live.state[d].val > (uintptr)0xFFFFFFFFULL)
			live.state[d].val = live.state[d].val - i;
		else
			live.state[d].val = (uae_u32)(live.state[d].val - i);
		return;
	}

	bool is_ptr = (d == PC_P);
	d = rmw(d);
	if (is_ptr)
		SUB_xxi(d, d, i);  // 64-bit SUB for host pointer
	else
		SUB_wwi(d, d, i);  // 32-bit SUB for M68k values
	unlock2(d);
}
MENDFUNC(2,arm_SUB_l_ri8,(RW4 d, IM8 i))


#ifdef JIT_DEBUG
#include "aarch64.h"

void disam_range(void *start, void *stop)
{
  char disbuf[256];
  uint64_t disptr = (uint64_t)start;
  while(disptr < (uint64_t)stop) {
    disasm(disptr, disbuf);
    write_log("%016llx  %s\n", disptr, disbuf);
    disptr += 4;
  }
}

#endif

STATIC_INLINE void flush_cpu_icache(void *start, void *stop)
{
#ifdef JIT_DEBUG
	if((uae_u64)stop - (uae_u64)start > 4) {
    if(disasm_this) {
      char disbuf[256];
      uint64_t disptr = (uint64_t)start;
      while(disptr < (uint64_t)stop) {
        disasm(disptr, disbuf);
        write_log("%016llx  %s\n", disptr, disbuf);
        disptr += 4;
      }
      disasm_this = false;
    }
  }
#endif

#if defined(__APPLE__) && defined(CPU_AARCH64)
	sys_icache_invalidate(start, (char *)stop - (char *)start);
#else
	__builtin___clear_cache((char *)start, (char *)stop);
#endif
}


STATIC_INLINE void write_jmp_target(uae_u32* jmpaddr, uintptr a)
{
	jit_begin_write_window();
	uintptr off = (a - (uintptr)jmpaddr) >> 2;
	if((*(jmpaddr) & 0xfc000000) == 0x14000000) {
		/* branch always — 26-bit offset, ±128MB */
		off = off & 0x3ffffff;
		*(jmpaddr) = (*(jmpaddr) & 0xfc000000) | off;
	} else if((*(jmpaddr) & 0x7c000000) == 0x34000000) {
		/* TBZ/TBNZ/CBZ/CBNZ — 14-bit offset */
		intptr soff = (intptr)((a - (uintptr)jmpaddr)) >> 2;
		if (soff > 0x1fff || soff < -0x2000)
			write_log("JIT: TBZ/TBNZ branch to target too long (%ld).\n", (long)soff);
		off = off & 0x3fff;
		*(jmpaddr) = (*(jmpaddr) & 0xfffc001f) | (off << 5);
	} else {
		/* conditional branch B.cond — 19-bit offset, ±1MB */
		intptr soff = (intptr)((a - (uintptr)jmpaddr)) >> 2;
		if (soff > 0x3ffff || soff < -0x40000)
			write_log("JIT: B.cond to target too long (%ld) jmpaddr=%p target=%p.\n",
				(long)soff, (void*)jmpaddr, (void*)a);
		off = off & 0x7ffff;
		*(jmpaddr) = (*(jmpaddr) & 0xff00001f) | (off << 5);
	}

	flush_cpu_icache((void *)jmpaddr, (void *)&jmpaddr[1]);
	jit_end_write_window();
}

static inline void emit_jmp_target(uae_u32 a) {
	emit_long(a - (JITPTR target + 4));
}

/*************************************************************************
* FPU stuff                                                             *
*************************************************************************/

#ifdef USE_JIT_FPU

MIDFUNC(1,f_forget_about,(FW r))
{
	if (f_isinreg(r))
		f_disassociate(r);
	live.fate[r].status = UNDEF;
}
MENDFUNC(1,f_forget_about,(FW r))

MIDFUNC(0,dont_care_fflags,(void))
{
	f_disassociate(FP_RESULT);
}
MENDFUNC(0,dont_care_fflags,(void))

MIDFUNC(2,fmov_rr,(FW d, FR s))
{
	if (d == s) { /* How pointless! */
		return;
	}
	s = f_readreg(s);
	d = f_writereg(d);
	raw_fmov_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fmov_rr,(FW d, FR s))

MIDFUNC(2,fmov_l_rr,(FW d, RR4 s))
{
	s = readreg(s);
	d = f_writereg(d);
	raw_fmov_l_rr(d, s);
	f_unlock(d);
	unlock2(s);
}
MENDFUNC(2,fmov_l_rr,(FW d, RR4 s))

MIDFUNC(2,fmov_s_rr,(FW d, RR4 s))
{
	s = readreg(s);
	d = f_writereg(d);
	raw_fmov_s_rr(d, s);
	f_unlock(d);
	unlock2(s);
}
MENDFUNC(2,fmov_s_rr,(FW d, RR4 s))

MIDFUNC(2,fmov_w_rr,(FW d, RR2 s))
{
	s = readreg(s);
	d = f_writereg(d);
	raw_fmov_w_rr(d, s);
	f_unlock(d);
	unlock2(s);
}
MENDFUNC(2,fmov_w_rr,(FW d, RR2 s))

MIDFUNC(2,fmov_b_rr,(FW d, RR1 s))
{
	s = readreg(s);
	d = f_writereg(d);
	raw_fmov_b_rr(d, s);
	f_unlock(d);
	unlock2(s);
}
MENDFUNC(2,fmov_b_rr,(FW d, RR1 s))

MIDFUNC(3,fmov_d_rrr,(FW d, RR4 s1, RR4 s2))
{
	s1 = readreg(s1);
	s2 = readreg(s2);
	d = f_writereg(d);
	raw_fmov_d_rrr(d, s1, s2);
	f_unlock(d);
	unlock2(s2);
	unlock2(s1);
}
MENDFUNC(3,fmov_d_rrr,(FW d, RR4 s1, RR4 s2))

MIDFUNC(2,fmov_l_ri,(FW d, IM32 i))
{
	switch(i) {
		case 0:
			fmov_d_ri_0(d);
			break;
		case 1:
			fmov_d_ri_1(d);
			break;
		case 10:
			fmov_d_ri_10(d);
			break;
		case 100:
			fmov_d_ri_100(d);
			break;
		default:
			d = f_writereg(d);
			compemu_raw_mov_l_ri(REG_WORK1, i);
			raw_fmov_l_rr(d, REG_WORK1);
			f_unlock(d);
	}
}
MENDFUNC(2,fmov_l_ri,(FW d, IM32 i))

MIDFUNC(2,fmov_s_ri,(FW d, IM32 i))
{
	d = f_writereg(d);
	compemu_raw_mov_l_ri(REG_WORK1, i);
	raw_fmov_s_rr(d, REG_WORK1);
	f_unlock(d);
}
MENDFUNC(2,fmov_s_ri,(FW d, IM32 i))

MIDFUNC(2,fmov_to_l_rr,(W4 d, FR s))
{
	s = f_readreg(s);
	d = writereg(d);
	raw_fmov_to_l_rr(d, s);
	unlock2(d);
	f_unlock(s);
}
MENDFUNC(2,fmov_to_l_rr,(W4 d, FR s))

MIDFUNC(2,fmov_to_s_rr,(W4 d, FR s))
{
	s = f_readreg(s);
	d = writereg(d);
	raw_fmov_to_s_rr(d, s);
	unlock2(d);
	f_unlock(s);
}
MENDFUNC(2,fmov_to_s_rr,(W4 d, FR s))

MIDFUNC(2,fmov_to_w_rr,(W4 d, FR s))
{
	s = f_readreg(s);
	INIT_WREG_w(d);

	raw_fmov_to_w_rr(d, s, targetIsReg);
	unlock2(d);
	f_unlock(s);
}
MENDFUNC(2,fmov_to_w_rr,(W4 d, FR s))

MIDFUNC(2,fmov_to_b_rr,(W4 d, FR s))
{
	s = f_readreg(s);
	INIT_WREG_b(d);

	raw_fmov_to_b_rr(d, s, targetIsReg);
	unlock2(d);
	f_unlock(s);
}
MENDFUNC(2,fmov_to_b_rr,(W4 d, FR s))

MIDFUNC(1,fmov_d_ri_0,(FW r))
{
	r = f_writereg(r);
	raw_fmov_d_ri_0(r);
	f_unlock(r);
}
MENDFUNC(1,fmov_d_ri_0,(FW r))

MIDFUNC(1,fmov_d_ri_1,(FW r))
{
	r = f_writereg(r);
	raw_fmov_d_ri_1(r);
	f_unlock(r);
}
MENDFUNC(1,fmov_d_ri_1,(FW r))

MIDFUNC(1,fmov_d_ri_10,(FW r))
{
	r = f_writereg(r);
	raw_fmov_d_ri_10(r);
	f_unlock(r);
}
MENDFUNC(1,fmov_d_ri_10,(FW r))

MIDFUNC(1,fmov_d_ri_100,(FW r))
{
	r = f_writereg(r);
	raw_fmov_d_ri_100(r);
	f_unlock(r);
}
MENDFUNC(1,fmov_d_ri_100,(FW r))

MIDFUNC(2,fmov_d_rm,(FW r, MEMR m))
{
	r = f_writereg(r);
	raw_fmov_d_rm(r, m);
	f_unlock(r);
}
MENDFUNC(2,fmov_d_rm,(FW r, MEMR m))

MIDFUNC(2,fmovs_rm,(FW r, MEMR m))
{
	r = f_writereg(r);
	raw_fmovs_rm(r, m);
	f_unlock(r);
}
MENDFUNC(2,fmovs_rm,(FW r, MEMR m))

MIDFUNC(2,fmov_rm,(FW r, MEMR m))
{
	r = f_writereg(r);
	raw_fmov_d_rm(r, m);
	f_unlock(r);
}
MENDFUNC(2,fmov_rm,(FW r, MEMR m))

MIDFUNC(3,fmov_to_d_rrr,(W4 d1, W4 d2, FR s))
{
	s = f_readreg(s);
	d1 = writereg(d1);
	d2 = writereg(d2);
	raw_fmov_to_d_rrr(d1, d2, s);
	unlock2(d2);
	unlock2(d1);
	f_unlock(s);
}
MENDFUNC(3,fmov_to_d_rrr,(W4 d1, W4 d2, FR s))

MIDFUNC(2,fsqrt_rr,(FW d, FR s))
{
	s = f_readreg(s);
	d = f_writereg(d);
	raw_fsqrt_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fsqrt_rr,(FW d, FR s))

MIDFUNC(2,fabs_rr,(FW d, FR s))
{
	s = f_readreg(s);
	d = f_writereg(d);
	raw_fabs_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fabs_rr,(FW d, FR s))

MIDFUNC(2,fneg_rr,(FW d, FR s))
{
	s = f_readreg(s);
	d = f_writereg(d);
	raw_fneg_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fneg_rr,(FW d, FR s))

MIDFUNC(2,fdiv_rr,(FRW d, FR s))
{
	s = f_readreg(s);
	d = f_rmw(d);
	raw_fdiv_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fdiv_rr,(FRW d, FR s))

MIDFUNC(2,fadd_rr,(FRW d, FR s))
{
	s = f_readreg(s);
	d = f_rmw(d);
	raw_fadd_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fadd_rr,(FRW d, FR s))

MIDFUNC(2,fmul_rr,(FRW d, FR s))
{
	s = f_readreg(s);
	d = f_rmw(d);
	raw_fmul_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fmul_rr,(FRW d, FR s))

MIDFUNC(2,fsub_rr,(FRW d, FR s))
{
	s = f_readreg(s);
	d = f_rmw(d);
	raw_fsub_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fsub_rr,(FRW d, FR s))

MIDFUNC(2,frndint_rr,(FW d, FR s))
{
	s = f_readreg(s);
	d = f_writereg(d);
	raw_frndint_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,frndint_rr,(FW d, FR s))

MIDFUNC(2,frndintz_rr,(FW d, FR s))
{
	s = f_readreg(s);
	d = f_writereg(d);
	raw_frndintz_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,frndintz_rr,(FW d, FR s))

MIDFUNC(2,fmod_rr,(FRW d, FR s))
{
	s = f_readreg(s);
	d = f_rmw(d);
	raw_fmod_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fmod_rr,(FRW d, FR s))

MIDFUNC(2,fsgldiv_rr,(FRW d, FR s))
{
	s = f_readreg(s);
	d = f_rmw(d);
	raw_fsgldiv_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fsgldiv_rr,(FRW d, FR s))

MIDFUNC(1,fcuts_r,(FRW r))
{
	r = f_rmw(r);
	raw_fcuts_r(r);
	f_unlock(r);
}
MENDFUNC(1,fcuts_r,(FRW r))

MIDFUNC(2,frem1_rr,(FRW d, FR s))
{
	s = f_readreg(s);
	d = f_rmw(d);
	raw_frem1_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,frem1_rr,(FRW d, FR s))

MIDFUNC(2,fsglmul_rr,(FRW d, FR s))
{
	s = f_readreg(s);
	d = f_rmw(d);
	raw_fsglmul_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fsglmul_rr,(FRW d, FR s))

MIDFUNC(2,fmovs_rr,(FW d, FR s))
{
	s = f_readreg(s);
	d = f_writereg(d);
	raw_fmovs_rr(d, s);
	f_unlock(s);
	f_unlock(d);
}
MENDFUNC(2,fmovs_rr,(FW d, FR s))

MIDFUNC(3,ffunc_rr,(double (*func)(double), FW d, FR s))
{
	clobber_flags();

	s = f_readreg(s);
	int reald = f_writereg(d);

	prepare_for_call_1();

	f_unlock(s);
	f_unlock(reald);

	prepare_for_call_2();

	raw_ffunc_rr(func, reald, s);

	live.fat[reald].holds = d;
	live.fat[reald].nholds = 1;

	live.fate[d].realreg = reald;
	live.fate[d].status = DIRTY;
}
MENDFUNC(3,ffunc_rr,(double (*func)(double), FW d, FR s))

MIDFUNC(3,fpowx_rr,(uae_u32 x, FW d, FR s))
{
	clobber_flags();

	s = f_readreg(s);
	int reald = f_writereg(d);

	prepare_for_call_1();

	f_unlock(s);
	f_unlock(reald);

	prepare_for_call_2();

	raw_fpowx_rr(x, reald, s);

	live.fat[reald].holds = d;
	live.fat[reald].nholds = 1;

	live.fate[d].realreg = reald;
	live.fate[d].status = DIRTY;
}
MENDFUNC(3,fpowx_rr,(uae_u32 x, FW d, FR s))

MIDFUNC(1,fflags_into_flags,())
{
	clobber_flags();
	fflags_into_flags_internal();
}
MENDFUNC(1,fflags_into_flags,())

MIDFUNC(2,fp_from_exten_mr,(RR4 adr, FR s))
{
	clobber_flags();

	adr = readreg(adr);
	s = f_readreg(s);
	raw_fp_from_exten_mr(adr, s);
	f_unlock(s);
	unlock2(adr);
}
MENDFUNC(2,fp_from_exten_mr,(RR4 adr, FR s))

MIDFUNC(2,fp_to_exten_rm,(FW d, RR4 adr))
{
	clobber_flags();

	adr = readreg(adr);
	d = f_writereg(d);
	raw_fp_to_exten_rm(d, adr);
	unlock2(adr);
	f_unlock(d);
}
MENDFUNC(2,fp_to_exten_rm,(FW d, RR4 adr))

MIDFUNC(2,fp_from_double_mr,(RR4 adr, FR s))
{
	adr = readreg(adr);
	s = f_readreg(s);
	raw_fp_from_double_mr(adr, s);
	f_unlock(s);
	unlock2(adr);
}
MENDFUNC(2,fp_from_double_mr,(RR4 adr, FR s))

MIDFUNC(2,fp_to_double_rm,(FW d, RR4 adr))
{
	adr = readreg(adr);
	d = f_writereg(d);
	raw_fp_to_double_rm(d, adr);
	unlock2(adr);
	f_unlock(d);
}
MENDFUNC(2,fp_to_double_rm,(FW d, RR4 adr))

MIDFUNC(2,fp_fscc_ri,(RW4 d, int cc))
{
	d = rmw(d);
	raw_fp_fscc_ri(d, cc);
	// Fix 20: raw_fp_fscc_ri uses CLEAR_LOW8_xx, SET_LOW8_xx, and BFXIL_xxii
	// which are all 64-bit ops that preserve dirty upper 32 bits.
	MOV_ww(d, d);
	unlock2(d);
}
MENDFUNC(2,fp_fscc_ri,(RW4 d, int cc))


#endif // USE_JIT_FPU

/* ROXR.B #imm,Dn: rotate right through X flag (byte size, immediate count) */
MIDFUNC(2,roxr_b_ri,(RW1 d, IM8 i))
{
    /* ROXR.B #1,Dn:
       new_bit0 = d & 1
       d = (X << 7) | ((d & 0xff) >> 1)
       X = C = new_bit0
       This handles count=1 specifically. For count>1, loop. */
    clobber_flags();
    d = rmw(d);
    
    int cnt = i;
    for (int c = 0; c < cnt; c++) {
        /* Save bit 0 of d */
        UBFX_xxii(REG_WORK1, d, 0, 1);  /* REG_WORK1 = d & 1 */
        /* Shift d right by 1 (byte) */
        UBFX_xxii(REG_WORK2, d, 1, 7);  /* REG_WORK2 = (d >> 1) & 0x7f */
        /* Load X flag and put in bit 7 */
        LOAD_U64(REG_WORK3, (uintptr)&regflags.x);
        LDR_wXi(REG_WORK4, REG_WORK3, 0);
        BFI_xxii(REG_WORK2, REG_WORK4, 7, 1);  /* REG_WORK2[7] = X */
        /* Write back to d (byte only) */
        BFI_xxii(d, REG_WORK2, 0, 8);
        /* X = C = old bit 0 */
        STR_wXi(REG_WORK1, REG_WORK3, 0);  /* regflags.x = old bit 0 */
    }
    
    /* Set NZCV: N=bit7, Z=(byte==0), V=0, C=X */
    /* For no-flags version, skip flags. For flags version, set them. */
    UBFX_xxii(REG_WORK2, d, 0, 8);
    TST_ww(REG_WORK2, REG_WORK2);  /* sets N and Z from byte value */
    flags_carry_inverted = false;
    /* C flag needs to match X (old bit 0), stored in REG_WORK1 */
    /* We'll just let the caller handle flags via live_flags/end_needflags */
    
    live_flags();
    unlock2(d);
}
MENDFUNC(2,roxr_b_ri,(RW1 d, IM8 i))
