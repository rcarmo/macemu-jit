/* ARM64 compatibility layer for legacy gencomp helper names.
 * Included only from compemu_support.cpp after compemu_support_arm.cpp.
 */

static inline bool legacy_needflags_enabled(void)
{
	return needflags != 0;
}

static inline void legacy_copy_carry_to_flagx(void)
{
	int x = writereg(FLAGX);
	if (flags_carry_inverted)
		CSET_xc(x, NATIVE_CC_CC);
	else
		CSET_xc(x, NATIVE_CC_CS);
	unlock2(x);
}

static inline int legacy_x86_cc_to_native(int cc)
{
	switch (cc) {
	case 0: return NATIVE_CC_VS;
	case 1: return NATIVE_CC_VC;
	case 2: return NATIVE_CC_CS;
	case 3: return NATIVE_CC_CC;
	case 4: return NATIVE_CC_EQ;
	case 5: return NATIVE_CC_NE;
	case 6: return NATIVE_CC_LS;
	case 7: return NATIVE_CC_HI;
	case 8: return NATIVE_CC_MI;
	case 9: return NATIVE_CC_PL;
	case 12: return NATIVE_CC_LT;
	case 13: return NATIVE_CC_GE;
	case 14: return NATIVE_CC_LE;
	case 15: return NATIVE_CC_GT;
	default:
		jit_abort("unsupported legacy x86 condition code %d", cc);
		return NATIVE_CC_EQ;
	}
}

static inline int legacy_addr_with_offset_avoid(int base, uae_s32 offset, int avoid)
{
	if (offset == 0)
		return base;
	int tmp = REG_WORK1;
	if (tmp == base || tmp == avoid)
		tmp = REG_WORK2;
	if (tmp == base || tmp == avoid)
		tmp = REG_WORK3;
	if (tmp == base || tmp == avoid)
		tmp = REG_WORK4;
	if (tmp == base || tmp == avoid)
		jit_abort("no temporary register for legacy host-pointer offset base=%d avoid=%d", base, avoid);
	if (offset > 0 && offset <= 4095) {
		ADD_xxi(tmp, base, offset);
		return tmp;
	}
	if (offset < 0 && offset >= -4095) {
		SUB_xxi(tmp, base, -offset);
		return tmp;
	}
	jit_abort("unsupported legacy host-pointer offset %d", offset);
	return base;
}

static inline int legacy_addr_with_offset(int base, uae_s32 offset)
{
	return legacy_addr_with_offset_avoid(base, offset, -1);
}

void start_needflags(void) { needflags = 1; }
void end_needflags(void) { needflags = 0; }

void duplicate_carry(void)
{
	if (!(needed_flags & FLAG_X))
		return;
	legacy_copy_carry_to_flagx();
}

void restore_carry(void)
{
	int x = readreg(FLAGX);
	SUBS_wwi(REG_WORK3, x, 1);
	unlock2(x);
	flags_carry_inverted = false;
}

void add_b(RW1 d, RR1 s) { if (legacy_needflags_enabled()) jff_ADD_b(d, s); else jnf_ADD_b(d, s); }
void add_w(RW2 d, RR2 s) { if (legacy_needflags_enabled()) jff_ADD_w(d, s); else jnf_ADD_w(d, s); }
void add_l(RW4 d, RR4 s) {
#ifdef CPU_AARCH64
	if (d == PC_P) { arm_ADD_l(d, s); return; }
#endif
	if (legacy_needflags_enabled()) jff_ADD_l(d, s); else jnf_ADD_l(d, s);
}
void add_l_ri(RW4 d, uae_s32 i) {
#ifdef CPU_AARCH64
	if (d == PC_P) { arm_ADD_ptr_ri(d, i); return; }
#endif
	if (legacy_needflags_enabled()) jff_ADD_l_imm(d, i); else jnf_ADD_l_imm(d, i);
}
void sub_b(RW1 d, RR1 s) { if (legacy_needflags_enabled()) jff_SUB_b(d, s); else jnf_SUB_b(d, s); }
void sub_w(RW2 d, RR2 s) { if (legacy_needflags_enabled()) jff_SUB_w(d, s); else jnf_SUB_w(d, s); }
void sub_l(RW4 d, RR4 s) { if (legacy_needflags_enabled()) jff_SUB_l(d, s); else jnf_SUB_l(d, s); }
void sub_b_ri(RW1 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_SUB_b_imm(d, i); else jnf_SUB_b_imm(d, i); }
void and_b(RW1 d, RR1 s) { if (legacy_needflags_enabled()) jff_AND_b(d, s); else jnf_AND_b(d, s); }
void and_w(RW2 d, RR2 s) { if (legacy_needflags_enabled()) jff_AND_w(d, s); else jnf_AND_w(d, s); }
void and_l(RW4 d, RR4 s) { if (legacy_needflags_enabled()) jff_AND_l(d, s); else jnf_AND_l(d, s); }
void and_l_ri(RW4 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_AND_l_imm(d, i); else jnf_AND_l_imm(d, i); }
void or_b(RW1 d, RR1 s) { if (legacy_needflags_enabled()) jff_OR_b(d, s); else jnf_OR_b(d, s); }
void or_w(RW2 d, RR2 s) { if (legacy_needflags_enabled()) jff_OR_w(d, s); else jnf_OR_w(d, s); }
void or_l(RW4 d, RR4 s) { if (legacy_needflags_enabled()) jff_OR_l(d, s); else jnf_OR_l(d, s); }
void or_l_ri(RW4 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_OR_l_imm(d, i); else jnf_OR_l_imm(d, i); }
void xor_b(RW1 d, RR1 s) { if (legacy_needflags_enabled()) jff_EOR_b(d, s); else jnf_EOR_b(d, s); }
void xor_w(RW2 d, RR2 s) { if (legacy_needflags_enabled()) jff_EOR_w(d, s); else jnf_EOR_w(d, s); }
void xor_l(RW4 d, RR4 s) { if (legacy_needflags_enabled()) jff_EOR_l(d, s); else jnf_EOR_l(d, s); }
void cmp_b(RR1 d, RR1 s) { jff_CMP_b(d, s); }
void cmp_w(RR2 d, RR2 s) { jff_CMP_w(d, s); }
void cmp_l(RR4 d, RR4 s) { jff_CMP_l(d, s); }
void mov_b_rr(W1 d, RR1 s) { if (legacy_needflags_enabled()) jff_MOVE_b(d, s); else jnf_MOVE_b(d, s); }
void mov_w_rr(W2 d, RR2 s) { if (legacy_needflags_enabled()) jff_MOVE_w(d, s); else jnf_MOVE_w(d, s); }
void mov_w_ri(W2 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_MOVE_w_imm(d, i); else jnf_MOVE_w_imm(d, i); }
/* cont90j: no-flags in-place low-16 decrement for the DBcc cc>=2 counter.
   Always jnf (never touches CCR) so the live compare flags the DBcc condition
   reads are preserved (the existing sub_w_ri MIDFUNC SETS flags, so it can't
   be used here); in-place via BFI (high word kept) with no scratch destination
   = aliasing-immune, unlike the old lea_l_brr(scratchie,src,-1)+mov_w_rr. */
void dbcc_dec_w(W2 d) { jnf_SUB_w_imm(d, 1); }

void zero_extend_8_rr(W4 d, RR1 s)
{
	if (isconst(s)) {
		set_const(d, (uae_u8)live.state[s].val);
		return;
	}
	const int s_is_d = (s == d);
	if (!s_is_d) {
		s = readreg(s);
		d = writereg(d);
	} else {
		s = d = rmw(s);
	}
	UNSIGNED8_REG_2_REG(d, s);
	if (!s_is_d)
		unlock2(d);
	unlock2(s);
}

void zero_extend_16_rr(W4 d, RR2 s)
{
	if (isconst(s)) {
		set_const(d, (uae_u16)live.state[s].val);
		return;
	}
	const int s_is_d = (s == d);
	if (!s_is_d) {
		s = readreg(s);
		d = writereg(d);
	} else {
		s = d = rmw(s);
	}
	UNSIGNED16_REG_2_REG(d, s);
	if (!s_is_d)
		unlock2(d);
	unlock2(s);
}

void sign_extend_8_rr(W4 d, RR1 s)
{
	if (isconst(s)) {
		set_const(d, (uae_s32)(uae_s8)live.state[s].val);
		return;
	}
	const int s_is_d = (s == d);
	if (!s_is_d) {
		s = readreg(s);
		d = writereg(d);
	} else {
		s = d = rmw(s);
	}
	SIGNED8_REG_2_REG(d, s);
	if (!s_is_d)
		unlock2(d);
	unlock2(s);
}

void test_b_rr(RR1 d, RR1 s)
{
	if (d == s) {
		jff_TST_b(d);
		return;
	}
	if (isconst(d))
		LOAD_U32(REG_WORK1, (uae_u8)live.state[d].val);
	else {
		int rd = readreg(d);
		SIGNED8_REG_2_REG(REG_WORK1, rd);
		unlock2(rd);
	}
	if (isconst(s))
		LOAD_U32(REG_WORK2, (uae_u8)live.state[s].val);
	else {
		int rs = readreg(s);
		SIGNED8_REG_2_REG(REG_WORK2, rs);
		unlock2(rs);
	}
	TST_ww(REG_WORK1, REG_WORK2);
	flags_carry_inverted = false;
}

void test_w_rr(RR2 d, RR2 s)
{
	if (d == s) {
		jff_TST_w(d);
		return;
	}
	if (isconst(d))
		LOAD_U32(REG_WORK1, (uae_u16)live.state[d].val);
	else {
		int rd = readreg(d);
		SIGNED16_REG_2_REG(REG_WORK1, rd);
		unlock2(rd);
	}
	if (isconst(s))
		LOAD_U32(REG_WORK2, (uae_u16)live.state[s].val);
	else {
		int rs = readreg(s);
		SIGNED16_REG_2_REG(REG_WORK2, rs);
		unlock2(rs);
	}
	TST_ww(REG_WORK1, REG_WORK2);
	flags_carry_inverted = false;
}

void test_l_rr(RR4 d, RR4 s)
{
	if (d == s) {
		jff_TST_l(d);
		return;
	}
	if (isconst(d))
		LOAD_U32(REG_WORK1, live.state[d].val);
	else {
		int rd = readreg(d);
		MOV_ww(REG_WORK1, rd);
		unlock2(rd);
	}
	if (isconst(s))
		LOAD_U32(REG_WORK2, live.state[s].val);
	else {
		int rs = readreg(s);
		MOV_ww(REG_WORK2, rs);
		unlock2(rs);
	}
	TST_ww(REG_WORK1, REG_WORK2);
	flags_carry_inverted = false;
}

void test_l_ri(RR4 d, uae_s32 i)
{
	if (isconst(d))
		LOAD_U32(REG_WORK1, live.state[d].val);
	else {
		int rd = readreg(d);
		MOV_ww(REG_WORK1, rd);
		unlock2(rd);
	}
	LOAD_U32(REG_WORK2, (uae_u32)i);
	TST_ww(REG_WORK1, REG_WORK2);
	flags_carry_inverted = false;
}

static inline void legacy_fix_inverted_carry(void)
{
	if (flags_carry_inverted) {
		MRS_NZCV_x(REG_WORK4);
		EOR_xxCflag(REG_WORK4, REG_WORK4);
		MSR_NZCV_x(REG_WORK4);
		flags_carry_inverted = false;
	}
}

static inline void legacy_invert_carry_in_pstate(void)
{
	MRS_NZCV_x(REG_WORK4);
	EOR_xxCflag(REG_WORK4, REG_WORK4);
	MSR_NZCV_x(REG_WORK4);
}

/* Shifted byte/word ADC/SBC arithmetic uses the low padding bits to carry the
   incoming X/borrow into the operand lane.  Rebuild Z from the architectural
   narrow result while preserving the arithmetic N/C/V bits and the current
   carry-polarity contract. */
static inline void legacy_set_z_from_narrow_result(int d, int width)
{
	MRS_NZCV_x(REG_WORK4);
	if (width == 8)
		UBFX_wwii(REG_WORK3, d, 0, 8);
	else
		UBFX_wwii(REG_WORK3, d, 0, 16);
	CMP_wi(REG_WORK3, 0);
	CSET_xc(REG_WORK3, NATIVE_CC_EQ);
	BFI_xxii(REG_WORK4, REG_WORK3, 30, 1);
	MSR_NZCV_x(REG_WORK4);
}

void adc_b(RW1 d, RR1 s)
{
	legacy_fix_inverted_carry();
	INIT_REGS_b(d, s);
	/* Ones below bit 24 propagate carry-in into the byte lane. */
	MOVN_xi(REG_WORK1, 0);
	BFI_xxii(REG_WORK1, s, 24, 8);
	LSL_wwi(REG_WORK3, d, 24);
	ADCS_www(REG_WORK1, REG_WORK1, REG_WORK3);
	BFXIL_xxii(d, REG_WORK1, 24, 8);
	legacy_set_z_from_narrow_result(d, 8);
	flags_carry_inverted = false;
	EXIT_REGS(d, s);
}

void adc_w(RW2 d, RR2 s)
{
	legacy_fix_inverted_carry();
	INIT_REGS_w(d, s);
	/* Ones below bit 16 propagate carry-in into the word lane. */
	MOVN_xi(REG_WORK1, 0);
	BFI_xxii(REG_WORK1, s, 16, 16);
	LSL_wwi(REG_WORK3, d, 16);
	ADCS_www(REG_WORK1, REG_WORK1, REG_WORK3);
	BFXIL_xxii(d, REG_WORK1, 16, 16);
	legacy_set_z_from_narrow_result(d, 16);
	flags_carry_inverted = false;
	EXIT_REGS(d, s);
}

void adc_l(RW4 d, RR4 s)
{
	legacy_fix_inverted_carry();
	INIT_REGS_l(d, s);
	ADCS_www(d, d, s);
	flags_carry_inverted = false;
	EXIT_REGS(d, s);
}

void sbb_b(RW1 d, RR1 s)
{
	legacy_fix_inverted_carry();
	legacy_invert_carry_in_pstate();
	INIT_REGS_b(d, s);
	LSL_wwi(REG_WORK1, d, 24);
	LSL_wwi(REG_WORK3, s, 24);
	SBCS_www(REG_WORK1, REG_WORK1, REG_WORK3);
	BFXIL_xxii(d, REG_WORK1, 24, 8);
	legacy_set_z_from_narrow_result(d, 8);
	flags_carry_inverted = true;
	EXIT_REGS(d, s);
}

void sbb_w(RW2 d, RR2 s)
{
	legacy_fix_inverted_carry();
	legacy_invert_carry_in_pstate();
	INIT_REGS_w(d, s);
	LSL_wwi(REG_WORK1, d, 16);
	LSL_wwi(REG_WORK3, s, 16);
	SBCS_www(REG_WORK1, REG_WORK1, REG_WORK3);
	BFXIL_xxii(d, REG_WORK1, 16, 16);
	legacy_set_z_from_narrow_result(d, 16);
	flags_carry_inverted = true;
	EXIT_REGS(d, s);
}

void sbb_l(RW4 d, RR4 s)
{
	legacy_fix_inverted_carry();
	legacy_invert_carry_in_pstate();
	INIT_REGS_l(d, s);
	SBCS_www(d, d, s);
	flags_carry_inverted = true;
	EXIT_REGS(d, s);
}

static inline void legacy_load_rr4_to_work(int work_reg, RR4 r)
{
	if (isconst(r)) {
		LOAD_U32(work_reg, live.state[r].val);
	} else {
		int rr = readreg(r);
		MOV_ww(work_reg, rr);
		unlock2(rr);
	}
}

static inline void legacy_set_c_preserve_nzv_from_work1_work2(void)
{
	MRS_NZCV_x(REG_WORK4);
	TST_ww(REG_WORK1, REG_WORK2);
	CSET_xc(REG_WORK3, NATIVE_CC_NE);
	BFI_xxii(REG_WORK4, REG_WORK3, 29, 1);
	MSR_NZCV_x(REG_WORK4);
	flags_carry_inverted = false;
}

void bt_l_rr(RR4 d, RR4 s)
{
	legacy_load_rr4_to_work(REG_WORK1, d);
	legacy_load_rr4_to_work(REG_WORK2, s);
	UBFIZ_xxii(REG_WORK2, REG_WORK2, 0, 5);
	MOV_xi(REG_WORK3, 1);
	LSL_www(REG_WORK2, REG_WORK3, REG_WORK2);
	legacy_set_c_preserve_nzv_from_work1_work2();
}

void bt_l_ri(RR4 d, uae_s32 i)
{
	legacy_load_rr4_to_work(REG_WORK1, d);
	LOAD_U32(REG_WORK2, 1u << (i & 31));
	legacy_set_c_preserve_nzv_from_work1_work2();
}

void btc_l_rr(RW4 d, RR4 s)
{
	legacy_load_rr4_to_work(REG_WORK1, d);
	legacy_load_rr4_to_work(REG_WORK2, s);
	UBFIZ_xxii(REG_WORK2, REG_WORK2, 0, 5);
	MOV_xi(REG_WORK3, 1);
	LSL_www(REG_WORK2, REG_WORK3, REG_WORK2);
	legacy_set_c_preserve_nzv_from_work1_work2();
	jnf_BCHG_l(d, s);
}

void btr_l_rr(RW4 d, RR4 s)
{
	legacy_load_rr4_to_work(REG_WORK1, d);
	legacy_load_rr4_to_work(REG_WORK2, s);
	UBFIZ_xxii(REG_WORK2, REG_WORK2, 0, 5);
	MOV_xi(REG_WORK3, 1);
	LSL_www(REG_WORK2, REG_WORK3, REG_WORK2);
	legacy_set_c_preserve_nzv_from_work1_work2();
	jnf_BCLR_l(d, s);
}

void bts_l_rr(RW4 d, RR4 s)
{
	legacy_load_rr4_to_work(REG_WORK1, d);
	legacy_load_rr4_to_work(REG_WORK2, s);
	UBFIZ_xxii(REG_WORK2, REG_WORK2, 0, 5);
	MOV_xi(REG_WORK3, 1);
	LSL_www(REG_WORK2, REG_WORK3, REG_WORK2);
	legacy_set_c_preserve_nzv_from_work1_work2();
	jnf_BSET_l(d, s);
}

void setcc(W1 d, uae_s32 cc)
{
	d = writereg(d);
	CSET_xc(d, legacy_x86_cc_to_native(cc));
	unlock2(d);
}

/* FPP-local classifier: keep this outside the MIDFUNC census because it is
   not a separately reachable API or closure row. */
void fcompare_result_rr(int result, int d, int s)
{
	d = f_readreg(d);
	s = f_readreg(s);
	result = f_writereg(result);
	fcompare_result_emit(result, d, s);
	f_unlock(result);
	f_unlock(s);
	f_unlock(d);
}

void cmov_l_rr(RW4 d, RR4 s, uae_s32 cc)
{
	if (d == s)
		return;
	FIX_INVERTED_CARRY
	d = rmw(d);
	const bool s_is_const = isconst(s);
	int src = s;
	if (s_is_const) {
		/* DBcc selects between PC_P and a host-pointer branch target, so a
		   constant source can be a 64-bit host address (always the case on
		   macOS, where the Mac address space sits above 4 GB). */
		if (s == PC_P || s >= S1)
			LOAD_U64(REG_WORK1, live.state[s].val);
		else
			LOAD_U32(REG_WORK1, live.state[s].val);
		src = REG_WORK1;
	} else {
		src = readreg(s);
	}

	/* gencomp passes flags_x86.h condition numbers. M68K HI/LS use C as
	   borrow, so they are not ARM HI/LS after the JIT's carry normalization:
	   HI is !C&&!Z, LS is C||Z. Preserve the original destination in a work
	   register and compose both predicates without modifying NZCV. */
	if (cc == 7) { /* x86 HI */
		MOV_xx(REG_WORK2, d);
		CSEL_xxxc(REG_WORK3, src, REG_WORK2, NATIVE_CC_CC);
		CSEL_xxxc(d, REG_WORK2, REG_WORK3, NATIVE_CC_EQ);
	} else if (cc == 6) { /* x86 LS */
		MOV_xx(REG_WORK2, d);
		CSEL_xxxc(REG_WORK3, src, REG_WORK2, NATIVE_CC_CS);
		CSEL_xxxc(d, src, REG_WORK3, NATIVE_CC_EQ);
	} else {
		CSEL_xxxc(d, src, d, legacy_x86_cc_to_native(cc));
	}
	if (!s_is_const)
		unlock2(src);
	unlock2(d);
}

/* jit_value_lock / jit_value_unlock: pin a just-fetched source value's host
   register across a following destination-EA computation so that the dst-EA
   allocation cannot clobber the live source value. Used by i_MOVE-to-memory:
   under opt2 register pressure the destination EA (mov_l_rr(dsta,An)+lea) could
   otherwise be allocated into the SAME host register holding the source value,
   making the inline store write the destination ADDRESS instead of the value
   (the self-address table that spun the 0403c19c resource-map compaction).
   g_jvlock_reg/active are read by writereg() to also defeat its isinreg()
   fast-path reuse of the pinned register (which would bypass alloc_reg_hinted's
   lock check via make_exclusive eviction). */
int g_jvlock_reg=-1;
int g_jvlock_active=0;
int jit_value_lock(int r)
{
	g_jvlock_active = 1;
	if (isinreg(r)) {
		int hr = live.state[r].realreg;
		setlock(hr);
		g_jvlock_reg = hr;
		return hr;
	}
	/* Not yet in a register (defensive): materialize and lock it. */
	int hr = readreg(r);
	g_jvlock_reg = hr;
	return hr;
}
void jit_value_unlock(int hr)
{
	g_jvlock_active = 0;
	g_jvlock_reg = -1;
	if (hr >= 0) unlock2(hr);
}

void mov_l_rR(W4 d, RR4 s, uae_s32 offset)
{
	d = writereg(d);
	if (isconst(s)) {
		LOAD_U64(REG_WORK1, (uintptr)(live.state[s].val + (uae_s32)offset));
		LDR_wXi(d, REG_WORK1, 0);
		unlock2(d);
		return;
	}
	const int base = readreg(s);
	const int addr = legacy_addr_with_offset(base, offset);
	LDR_wXi(d, addr, 0);
	unlock2(base);
	unlock2(d);
}

void mov_w_rR(W2 d, RR4 s, uae_s32 offset)
{
	d = writereg(d);
	if (isconst(s)) {
		LOAD_U64(REG_WORK1, (uintptr)(live.state[s].val + (uae_s32)offset));
		LDRH_wXi(d, REG_WORK1, 0);
		unlock2(d);
		return;
	}
	const int base = readreg(s);
	const int addr = legacy_addr_with_offset(base, offset);
	LDRH_wXi(d, addr, 0);
	unlock2(base);
	unlock2(d);
}

void mov_l_Rr(RR4 d, RR4 s, uae_s32 offset)
{
	int src = isconst(s) ? REG_WORK1 : readreg(s);
	if (isconst(s))
		LOAD_U32(src, live.state[s].val);
	if (isconst(d)) {
		LOAD_U64(REG_WORK2, (uintptr)(live.state[d].val + (uae_s32)offset));
		STR_wXi(src, REG_WORK2, 0);
	} else {
		const int base = readreg(d);
		const int addr = legacy_addr_with_offset_avoid(base, offset, src);
		STR_wXi(src, addr, 0);
		unlock2(base);
	}
	if (!isconst(s))
		unlock2(src);
}

void mov_w_Rr(RR4 d, RR2 s, uae_s32 offset)
{
	int src = isconst(s) ? REG_WORK1 : readreg(s);
	if (isconst(s))
		LOAD_U32(src, (uae_u16)live.state[s].val);
	if (isconst(d)) {
		LOAD_U64(REG_WORK2, (uintptr)(live.state[d].val + (uae_s32)offset));
		STRH_wXi(src, REG_WORK2, 0);
	} else {
		const int base = readreg(d);
		const int addr = legacy_addr_with_offset_avoid(base, offset, src);
		STRH_wXi(src, addr, 0);
		unlock2(base);
	}
	if (!isconst(s))
		unlock2(src);
}

void mid_bswap_16(RW2 r)
{
	if (isconst(r)) {
		set_const(r, (uae_u16)do_byteswap_16((uae_u16)live.state[r].val));
		return;
	}
	r = rmw(r);
	REV16_ww(r, r);
	unlock2(r);
}

void mid_bswap_32(RW4 r)
{
	if (isconst(r)) {
		set_const(r, do_byteswap_32((uae_u32)live.state[r].val));
		return;
	}
	r = rmw(r);
	REV32_xx(r, r);
	MOV_ww(r, r);
	unlock2(r);
}

void imul_32_32(RW4 d, RR4 s) { if (legacy_needflags_enabled()) jff_MULS32(d, s); else jnf_MULS32(d, s); }
/* Legacy callers use s as both the multiplicand and returned high half.  Keep
   that old two-operand ABI on top of the AArch64 three-operand ownership
   contract; current gencomp MULL calls the MIDFUNC directly with explicit Dh. */
void imul_64_32(RW4 d, RW4 s) { if (legacy_needflags_enabled()) jff_MULS64(d, s, s); else jnf_MULS64(d, s, s); }
void mul_64_32(RW4 d, RW4 s) { if (legacy_needflags_enabled()) jff_MULU64(d, s, s); else jnf_MULU64(d, s, s); }

void shra_b_ri(RW1 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_ASR_b_imm(d, i); else jnf_ASR_b_imm(d, i); }
void shra_w_ri(RW2 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_ASR_w_imm(d, i); else jnf_ASR_w_imm(d, i); }
void shra_l_ri(RW4 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_ASR_l_imm(d, i); else jnf_ASR_l_imm(d, i); }
void shra_b_rr(RW1 d, RR1 r) { if (legacy_needflags_enabled()) jff_ASR_b_reg(d, r); else jnf_ASR_b_reg(d, r); }
void shra_w_rr(RW2 d, RR1 r) { if (legacy_needflags_enabled()) jff_ASR_w_reg(d, r); else jnf_ASR_w_reg(d, r); }
void shra_l_rr(RW4 d, RR1 r) { if (legacy_needflags_enabled()) jff_ASR_l_reg(d, r); else jnf_ASR_l_reg(d, r); }
void shrl_b_ri(RW1 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_LSR_b_imm(d, i); else jnf_LSR_b_imm(d, i); }
void shrl_w_ri(RW2 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_LSR_w_imm(d, i); else jnf_LSR_w_imm(d, i); }
void shrl_l_ri(RW4 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_LSR_l_imm(d, i); else jnf_LSR_l_imm(d, i); }
void shrl_b_rr(RW1 d, RR1 r) { if (legacy_needflags_enabled()) jff_LSR_b_reg(d, r); else jnf_LSR_b_reg(d, r); }
void shrl_w_rr(RW2 d, RR1 r) { if (legacy_needflags_enabled()) jff_LSR_w_reg(d, r); else jnf_LSR_w_reg(d, r); }
void shrl_l_rr(RW4 d, RR1 r) { if (legacy_needflags_enabled()) jff_LSR_l_reg(d, r); else jnf_LSR_l_reg(d, r); }
void shll_b_ri(RW1 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_LSL_b_imm(d, i); else jnf_LSL_b_imm(d, i); }
void shll_w_ri(RW2 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_LSL_w_imm(d, i); else jnf_LSL_w_imm(d, i); }
void shll_l_ri(RW4 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_LSL_l_imm(d, i); else jnf_LSL_l_imm(d, i); }
void shll_b_rr(RW1 d, RR1 r) { if (legacy_needflags_enabled()) jff_LSL_b_reg(d, r); else jnf_LSL_b_reg(d, r); }
void shll_w_rr(RW2 d, RR1 r) { if (legacy_needflags_enabled()) jff_LSL_w_reg(d, r); else jnf_LSL_w_reg(d, r); }
void shll_l_rr(RW4 d, RR1 r) { if (legacy_needflags_enabled()) jff_LSL_l_reg(d, r); else jnf_LSL_l_reg(d, r); }
void rol_b_rr(RW1 d, RR1 r) { if (legacy_needflags_enabled()) jff_ROL_b(d, r); else jnf_ROL_b(d, r); }
void rol_w_rr(RW2 d, RR1 r) { if (legacy_needflags_enabled()) jff_ROL_w(d, r); else jnf_ROL_w(d, r); }
void rol_l_rr(RW4 d, RR1 r) { if (legacy_needflags_enabled()) jff_ROL_l(d, r); else jnf_ROL_l(d, r); }
void rol_l_ri(RW4 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_ROL_l_imm(d, i); else jnf_ROL_l_imm(d, i); }
void ror_b_rr(RW1 d, RR1 r) { if (legacy_needflags_enabled()) jff_ROR_b(d, r); else jnf_ROR_b(d, r); }
void ror_w_rr(RW2 d, RR1 r) { if (legacy_needflags_enabled()) jff_ROR_w(d, r); else jnf_ROR_w(d, r); }
void ror_l_rr(RW4 d, RR1 r) { if (legacy_needflags_enabled()) jff_ROR_l(d, r); else jnf_ROR_l(d, r); }

void setcc_for_cntzero(RR4 cnt, RR4 data, int size)
{
	if (isconst(cnt) && live.state[cnt].val == 0) {
		switch (size) {
		case 1: jff_TST_b(data); break;
		case 2: jff_TST_w(data); break;
		default: jff_TST_l(data); break;
		}
	}
}

void set_zero(int r, int tmp)
{
	(void)tmp;
	MRS_NZCV_x(REG_WORK1);
	CLEAR_xxZflag(REG_WORK1, REG_WORK1);
	if (isconst(r)) {
		if ((uae_u32)live.state[r].val == 0)
			SET_xxZflag(REG_WORK1, REG_WORK1);
	} else {
		int rr = readreg(r);
		CBNZ_wi(rr, 2); /* skip next if non-zero */
		SET_xxZflag(REG_WORK1, REG_WORK1);
		unlock2(rr);
	}
	MSR_NZCV_x(REG_WORK1);
	/* Only Z changed; preserve the caller's physical-C polarity contract. */
}

int kill_rodent(int r)
{
	(void)r;
	return 0;
}

void do_nothing(void)
{
#if defined(CPU_AARCH64)
	if (jit_diag_enabled()) {
		jit_diag_do_nothing_calls++;
		jit_diag_dispatch_count++;
	}
	countdown = 10000000;
	if (quit_program > 0)
		return;
	if (jit_diag_enabled()) {
		static unsigned long dn_count = 0;
		dn_count++;
		if (dn_count <= 20 || dn_count % 50000 == 0) {
			uaecptr sp = regs.regs[15];
			fprintf(stderr, "DN[%lu] pc=%08x im=%u spc=%x d0=%08x d1=%08x d2=%08x d3=%08x a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a7=%08x s0=%08x s4=%08x t490=%08x t554=%08x t574=%08x\n",
				dn_count, m68k_getpc(), (unsigned)regs.intmask,
				(unsigned)regs.spcflags, regs.regs[0], regs.regs[1], regs.regs[2], regs.regs[3], regs.regs[8], regs.regs[9], regs.regs[10], regs.regs[11], regs.regs[12], regs.regs[15],
				(unsigned)get_long(sp), (unsigned)get_long(sp + 4),
				(unsigned)get_long(0x490), (unsigned)get_long(0x554), (unsigned)get_long(0x574));
		}
	}
	MakeSR();
	m68k_do_specialties();
#endif
}

static bool jit_tracewin_enabled()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_PC_START") && *getenv("B2_TRACE_PC_START")) ? 1 : 0;
	return cached != 0;
}

static uae_u32 jit_tracewin_start()
{
	static uae_u32 value = 0;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_PC_START");
		value = env && *env ? (uae_u32)strtoul(env, NULL, 0) : 0;
		init = true;
	}
	return value;
}

static uae_u32 jit_tracewin_end()
{
	static uae_u32 value = 0xffffffffu;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_PC_END");
		value = env && *env ? (uae_u32)strtoul(env, NULL, 0) : 0xffffffffu;
		init = true;
	}
	return value;
}

static unsigned long jit_tracewin_limit()
{
	static unsigned long value = 200;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_LIMIT");
		value = env && *env ? strtoul(env, NULL, 0) : 200;
		init = true;
	}
	return value;
}

static bool jit_trace_after_table_env()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_AFTER_TABLE") && *getenv("B2_TRACE_AFTER_TABLE") && strcmp(getenv("B2_TRACE_AFTER_TABLE"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

static bool jit_tracewin_match(uae_u32 pc)
{
	if (!jit_tracewin_enabled() || pc < jit_tracewin_start() || pc > jit_tracewin_end())
		return false;
	if (jit_trace_after_table_env() && !basilisk_trace_after_table_ready)
		return false;
	return true;
}

static bool jit_pctrace_match(uae_u32 pc)
{
	if (jit_tracewin_enabled() && (pc < jit_tracewin_start() || pc > jit_tracewin_end()))
		return false;
	if (jit_trace_after_table_env() && !basilisk_trace_after_table_ready)
		return false;
	return true;
}

static bool jit_trace_table_enabled()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_TABLE") && *getenv("B2_TRACE_TABLE") && strcmp(getenv("B2_TRACE_TABLE"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

static void jit_trace_table_maybe_dump_complete(const char *tag, unsigned long step, uae_u32 pc)
{
	static int dumped = 0;
	static int cfg_init = 0;
	static char dump_path[512];
	if (!cfg_init) {
		const char *env = getenv("B2_TRACE_TABLE_DUMP_PATH");
		dump_path[0] = 0;
		if (env && *env) {
			strncpy(dump_path, env, sizeof(dump_path) - 1);
			dump_path[sizeof(dump_path) - 1] = 0;
		}
		cfg_init = 1;
	}
	if (dumped || !dump_path[0])
		return;
	unsigned a1 = (unsigned)regs.regs[9];
	if (a1 >= 0x1e00 && a1 < 0x1e40)
		basilisk_trace_after_table_ready = true;
	if (a1 < 0x1e00 || a1 >= 0x1e40)
		return;
	FILE *f = fopen(dump_path, "wb");
	if (!f)
		return;
	for (uaecptr addr = 0x0e00; addr < 0x1e00; addr++)
		fputc((int)get_byte(addr), f);
	fclose(f);
	dumped = 1;
	fprintf(stderr, "%s_DUMP step=%lu pc=%08x a1=%08x path=%s\n", tag, step, (unsigned)pc, (unsigned)regs.regs[9], dump_path);
}

static void jit_trace_lowmem400_maybe_dump(unsigned long step, uae_u32 pc)
{
	static int dumped = 0;
	static int cfg_init = 0;
	static char dump_path[512];
	if (!cfg_init) {
		const char *env = getenv("B2_TRACE_LOWMEM400_DUMP_PATH");
		dump_path[0] = 0;
		if (env && *env) {
			strncpy(dump_path, env, sizeof(dump_path) - 1);
			dump_path[sizeof(dump_path) - 1] = 0;
		}
		cfg_init = 1;
	}
	if (dumped || !dump_path[0])
		return;
	if (pc < 0x040099f0 || pc > 0x04009a30)
		return;
	FILE *f = fopen(dump_path, "wb");
	if (!f)
		return;
	for (uaecptr addr = 0x0400; addr < 0x0800; addr++)
		fputc((int)get_byte(addr), f);
	fclose(f);
	dumped = 1;
	fprintf(stderr, "LOWMEM400_DUMP step=%lu pc=%08x d1=%08x path=%s\n", step, (unsigned)pc, (unsigned)regs.regs[1], dump_path);
}

/* CONT.110 cont90e: one-shot dump of the QuickDraw pixel-LUT region built by the
   0403bf00 generator, captured at the consumer entry 0403b0e0 (table complete,
   runs once). L1-vs-L2 diff of the dump is the arbiter for whether the generator
   (dbf d4) writes a divergent table (real producer) or dbf d4 is a flush artifact.
   Gated by B2_LUT_DUMP_PATH; dark otherwise. */
static void jit_trace_lut_maybe_dump(unsigned long step, uae_u32 pc)
{
	static int dumped = 0;
	static int cfg_init = 0;
	static char dump_path[512];
	static uae_u32 trig_pc = 0x0403b0e0;
	if (!cfg_init) {
		const char *env = getenv("B2_LUT_DUMP_PATH");
		dump_path[0] = 0;
		if (env && *env) {
			strncpy(dump_path, env, sizeof(dump_path) - 1);
			dump_path[sizeof(dump_path) - 1] = 0;
		}
		const char *penv = getenv("B2_LUT_DUMP_TRIG_PC");
		if (penv && *penv)
			trig_pc = (uae_u32)strtoul(penv, NULL, 0);
		cfg_init = 1;
	}
	if (dumped || !dump_path[0])
		return;
	if (pc != trig_pc)
		return;
	uae_u32 lo = 0xe000, hi = 0x14000;
	const char *le = getenv("B2_LUT_DUMP_LO"); if (le && *le) lo = (uae_u32)strtoul(le, NULL, 0);
	const char *he = getenv("B2_LUT_DUMP_HI"); if (he && *he) hi = (uae_u32)strtoul(he, NULL, 0);
	FILE *f = fopen(dump_path, "wb");
	if (!f)
		return;
	for (uaecptr addr = lo; addr < hi; addr++)
		fputc((int)get_byte(addr), f);
	fclose(f);
	dumped = 1;
	fprintf(stderr, "LUT_DUMP step=%lu pc=%08x lo=%08x hi=%08x path=%s\n",
		step, (unsigned)pc, (unsigned)lo, (unsigned)hi, dump_path);
}

static void jit_trace_table_log(const char *tag, unsigned long step, uae_u32 pc)
{
	if (!jit_trace_table_enabled())
		return;
	fprintf(stderr,
		"%s step=%lu pc=%08x a1=%08x e00=%08x e04=%08x e08=%08x e0c=%08x e10=%08x e14=%08x e18=%08x e1c=%08x e20=%08x e24=%08x e28=%08x e2c=%08x e30=%08x e34=%08x e38=%08x e3c=%08x\n",
		tag,
		step,
		(unsigned)pc,
		(unsigned)regs.regs[9],
		(unsigned)get_long(0x0e00),
		(unsigned)get_long(0x0e04),
		(unsigned)get_long(0x0e08),
		(unsigned)get_long(0x0e0c),
		(unsigned)get_long(0x0e10),
		(unsigned)get_long(0x0e14),
		(unsigned)get_long(0x0e18),
		(unsigned)get_long(0x0e1c),
		(unsigned)get_long(0x0e20),
		(unsigned)get_long(0x0e24),
		(unsigned)get_long(0x0e28),
		(unsigned)get_long(0x0e2c),
		(unsigned)get_long(0x0e30),
		(unsigned)get_long(0x0e34),
		(unsigned)get_long(0x0e38),
		(unsigned)get_long(0x0e3c));
	jit_trace_table_maybe_dump_complete(tag, step, pc);
}

#if defined(CPU_AARCH64)
static cpuop_func *jit_orig_op_0_0_ff = NULL;

static inline void jit_execute_ori_b_d0_exact(void)
{
	uae_u8 *p = regs.pc_p;
	uae_u8 imm = p[3];
	uae_u32 d0 = m68k_dreg(regs, 0);
	uae_u8 result = (uae_u8)(d0 | imm);
	regflags.nzcv = (result == 0 ? (uae_u32)FLAGVAL_Z : 0) |
		((result & 0x80) ? (uae_u32)FLAGVAL_N : 0);
	if (imm)
		m68k_dreg(regs, 0) = (d0 & ~0xff) | result;
	regs.pc_p = p + 4;
}

static void REGPARAM2 jit_fast_op_0_0_ff(uae_u32 opcode)
{
	(void)opcode;
	jit_execute_ori_b_d0_exact();
}

static void jit_install_fast_interpreter_overrides(void)
{
	static bool installed = false;
	if (installed)
		return;
	jit_orig_op_0_0_ff = cpufunctbl[0];
	cpufunctbl[0] = jit_fast_op_0_0_ff;
	installed = true;
}
#endif

void exec_nostats(void)
{
#if defined(CPU_AARCH64)
	if (jit_strict_full_jit_env())
		jit_abort("strict full-JIT: exec_nostats runtime entry pc=%08x", m68k_getpc());
	if (jit_diag_enabled()) {
		jit_diag_exec_nostats_calls++;
		jit_diag_dispatch_count++;
		jit_diag_maybe_print();
	}
#endif
	static unsigned long trace_count = 0;
	const bool trace_enabled = jit_tracewin_enabled();
	for (;;) {
		uae_u32 before_pc = m68k_getpc();
		uae_u32 opcode = GET_OPCODE;
		jit_current_interp_pc = before_pc;
		jit_current_interp_opcode = opcode;
		bool trace_this = false;
		if (trace_enabled && trace_count < jit_tracewin_limit()) {
			trace_this = jit_tracewin_match(before_pc);
		}
		if (jit_guest_path_enabled())
			jit_guest_path_record_reference(before_pc);
		if (jit_trace_target_pc(before_pc))
			jit_trace_pc_hit(before_pc, (2u << 16) | (opcode & 0xffff));
		if (trace_this) {
			fprintf(stderr,
				"TRACEWINJ BEFORE step=%lu pc=%08x op=%04x regs.pc=%08x pc_p=%p oldp=%p d0=%08x d1=%08x d2=%08x d3=%08x a0=%08x a1=%08x a2=%08x a3=%08x a7=%08x sr=%04x nzcv=%08x x=%08x\n",
				trace_count + 1,
				(unsigned)before_pc,
				(unsigned)opcode,
				(unsigned)regs.pc,
				(void*)regs.pc_p,
				(void*)regs.pc_oldp,
				(unsigned)regs.regs[0],
				(unsigned)regs.regs[1],
				(unsigned)regs.regs[2],
				(unsigned)regs.regs[3],
				(unsigned)regs.regs[8],
				(unsigned)regs.regs[9],
				(unsigned)regs.regs[10],
				(unsigned)regs.regs[11],
				(unsigned)regs.regs[15],
				(unsigned)regs.sr,
				(unsigned)regflags.nzcv,
				(unsigned)regflags.x);
			jit_trace_table_log("TRACEWINJTAB", trace_count + 1, before_pc);
		}
		(*cpufunctbl[opcode])(opcode);
		if (trace_this) {
			uae_u32 after_pc = m68k_getpc();
			trace_count++;
			fprintf(stderr,
				"TRACEWINJ AFTER step=%lu pc=%08x op=%04x regs.pc=%08x pc_p=%p oldp=%p d0=%08x d1=%08x d2=%08x d3=%08x a0=%08x a1=%08x a2=%08x a3=%08x a7=%08x sr=%04x nzcv=%08x x=%08x\n",
				trace_count,
				(unsigned)after_pc,
				(unsigned)opcode,
				(unsigned)regs.pc,
				(void*)regs.pc_p,
				(void*)regs.pc_oldp,
				(unsigned)regs.regs[0],
				(unsigned)regs.regs[1],
				(unsigned)regs.regs[2],
				(unsigned)regs.regs[3],
				(unsigned)regs.regs[8],
				(unsigned)regs.regs[9],
				(unsigned)regs.regs[10],
				(unsigned)regs.regs[11],
				(unsigned)regs.regs[15],
				(unsigned)regs.sr,
				(unsigned)regflags.nzcv,
				(unsigned)regflags.x);
			jit_trace_table_log("TRACEWINJTAB", trace_count, after_pc);
		}
		cpu_check_ticks();
		if (end_block(opcode) || SPCFLAGS_TEST(SPCFLAG_ALL))
			return;
	}
}

#if defined(CPU_AARCH64)
extern "C" {
uae_u32 jit_current_interp_pc = 0;
uae_u32 jit_current_interp_opcode = 0;
}

static void exec_nostats_limited(int maxrun_limit)
{
	if (jit_strict_full_jit_env())
		jit_abort("strict full-JIT: exec_nostats_limited runtime entry pc=%08x", m68k_getpc());
	int run_count = 0;
	if (maxrun_limit <= 0 || maxrun_limit > MAXRUN)
		maxrun_limit = MAXRUN;
	for (;;) {
		uae_u32 opcode = GET_OPCODE;
		const uae_u32 pc = m68k_getpc();
		if (jit_guest_instruction_observer_enabled())
			jit_guest_path_record_nostats(pc);
		if (jit_trace_target_pc(pc))
			jit_trace_pc_hit(pc, (2u << 16) | (opcode & 0xffff));
		(*cpufunctbl[opcode])(opcode);
		cpu_check_ticks();
		if (end_block(opcode) || SPCFLAGS_TEST(SPCFLAG_ALL) || ++run_count >= maxrun_limit)
			return;
	}
}

#endif

static inline bool jit_interpop_assert_target(uae_u32 *target)
{
#if defined(CPU_AARCH64)
	static int initialized = 0;
	static bool enabled = false;
	static uae_u32 value = 0;
	if (!initialized) {
		const char *env = getenv("B2_INTERPOP_PC");
		if (env && *env) {
			char *end = NULL;
			const unsigned long parsed = strtoul(env, &end, 0);
			enabled = end != env;
			value = (uae_u32)parsed;
		}
		initialized = 1;
	}
	if (target)
		*target = value;
	return enabled;
#else
	(void)target;
	return false;
#endif
}

void execute_normal(void)
{
#if defined(CPU_AARCH64)
	if (jit_diag_enabled()) {
		jit_diag_execute_normal_calls++;
		jit_diag_dispatch_count++;
	}
	/* If quit_program is set (e.g. M68K_EXEC_RETURN), skip everything
	   and return immediately. Running further instructions would
	   execute random memory past the test code boundary. */
	if (quit_program > 0)
		return;
	/* Handle pending interrupts on every execute_normal entry. */
	if (__atomic_load_n(&regs.spcflags, __ATOMIC_ACQUIRE) & SPCFLAG_ALL) {
		MakeSR();
		m68k_do_specialties();
	}

	if (jit_diag_enabled())
		jit_diag_maybe_print();
	/* A corrupt fetch pointer is an internal JIT invariant failure, not an
	   architectural bus error and not permission to reconstruct registers from
	   stale metadata.  Strict mode checks the canonical direct-address mapping
	   and fails closed; ordinary execution retains the established fetch path. */
	if (jit_strict_full_jit_env()) {
		const uaecptr guest_pc = m68k_getpc();
		const uae_u8 *expected_pc_p = get_real_address(guest_pc, 0, sz_word);
		if (regs.pc_p != expected_pc_p)
			jit_abort("strict full-JIT: noncanonical fetch pointer pc=%08x pc_p=%p expected=%p oldp=%p",
				guest_pc, (void*)regs.pc_p, (const void*)expected_pc_p,
				(void*)regs.pc_oldp);
	}
	/* Ordinary mode avoids caching transient zero-filled RAM probes. Strict mode
	   must not substitute a C semantic loop for translated execution: let the
	   first-seen tracer observe the code and compile an L2 block normally. */
	if (!jit_strict_full_jit_env()) {
		uae_u32 fast_pc = get_virtual_address((uae_u8*)regs.pc_p);
		if (fast_pc < (uae_u32)ROMBaseMac && *((const uae_u16*)regs.pc_p) == 0) {
			exec_nostats_limited(MAXRUN);
			return;
		}
	}
#endif
	if (!check_for_cache_miss()) {
		cpu_history pc_hist[MAXRUN];
#ifdef UAE
		memset(pc_hist, 0, sizeof(pc_hist));
#endif
		int blocklen = 0;
		int total_cycles = 0;
		/* Use the actual current fetch PC as the base for this traced block.
		   On ARM64, stale regs.pc/regs.pc_oldp metadata can survive across
		   mixed-mode transitions even when regs.pc_p is correct. PC-relative
		   codegen (LEA/JMP d16,PC) should anchor to the current host PC. */
		start_pc_p = regs.pc_p;
		start_pc = get_virtual_address((uae_u8*)regs.pc_p);
#if defined(CPU_AARCH64)
		{
			static unsigned long pctrace_count = 0;
			static unsigned long pctrace_limit = 0;
			static bool pctrace_init = false;
			if (!pctrace_init) {
				const char *env = getenv("B2_JIT_PCTRACE");
				pctrace_limit = env ? strtoul(env, NULL, 10) : 0;
				pctrace_init = true;
			}
			if (pctrace_limit && pctrace_count < pctrace_limit) {
				uae_u32 pc = m68k_getpc();
				if (!jit_pctrace_match(pc))
					goto jit_pctrace_done;
				static unsigned long pctrace_words = 0;
				static bool pctrace_words_init = false;
				if (!pctrace_words_init) {
					const char *env = getenv("B2_JIT_PCTRACE_WORDS");
					pctrace_words = env ? strtoul(env, NULL, 10) : 0;
					pctrace_words_init = true;
				}
				static int pctrace_stack = -1;
				static int pctrace_mem = -1;
				if (pctrace_stack < 0) {
					const char *env = getenv("B2_JIT_PCTRACE_STACK");
					pctrace_stack = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
				}
				if (pctrace_mem < 0) {
					const char *env = getenv("B2_JIT_PCTRACE_MEM");
					pctrace_mem = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
				}
				unsigned long current_step = pctrace_count++;
				fprintf(stderr, "PCTRACE %lu %08x d0=%08x d1=%08x d2=%08x d3=%08x d4=%08x d5=%08x d6=%08x d7=%08x a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x sr=%04x nzcv=%08x x=%08x\n",
					current_step, pc,
					regs.regs[0], regs.regs[1], regs.regs[2], regs.regs[3],
					regs.regs[4], regs.regs[5], regs.regs[6], regs.regs[7],
					regs.regs[8], regs.regs[9], regs.regs[10], regs.regs[11],
					regs.regs[12], regs.regs[13], regs.regs[14], regs.regs[15],
					(unsigned)regs.sr, regflags.nzcv, regflags.x);
				jit_trace_table_log("PCTTABLE", current_step, pc);
				jit_trace_lowmem400_maybe_dump(current_step, pc);
				jit_trace_lut_maybe_dump(current_step, pc);
				if (pctrace_stack) {
					uaecptr sp = m68k_areg(regs, 7);
					fprintf(stderr,
						"PCTSTACK %08x sm4=%08x s0=%08x s4=%08x s8=%08x s12=%08x s16=%08x s20=%08x s24=%08x\n",
						pc,
						(unsigned)get_long(sp - 4),
						(unsigned)get_long(sp + 0),
						(unsigned)get_long(sp + 4),
						(unsigned)get_long(sp + 8),
						(unsigned)get_long(sp + 12),
						(unsigned)get_long(sp + 16),
						(unsigned)get_long(sp + 20),
						(unsigned)get_long(sp + 24));
				}
				if (pctrace_mem) {
					uaecptr a0v = m68k_areg(regs, 0);
					uaecptr a3v = m68k_areg(regs, 3);
					fprintf(stderr,
						"PCTMEM %08x m1e4=%08x m1e8=%08x m20c=%08x m0c30=%08x m0c64=%08x m0c68=%08x m0c6c=%08x m0c70=%08x m0c74=%08x ma0m4=%08x ma3=%08x ma3p4=%08x\n",
						pc,
						(unsigned)get_long(0x1e4),
						(unsigned)get_long(0x1e8),
						(unsigned)get_long(0x20c),
						(unsigned)get_long(0x0c30),
						(unsigned)get_long(0x0c64),
						(unsigned)get_long(0x0c68),
						(unsigned)get_long(0x0c6c),
						(unsigned)get_long(0x0c70),
						(unsigned)get_long(0x0c74),
						(unsigned)get_long(a0v >= 4 ? a0v - 4 : a0v),
						(unsigned)get_long(a3v),
						(unsigned)get_long(a3v + 4));
				}
				if (pctrace_words) {
					if (pctrace_words > 12)
						pctrace_words = 12;
					fprintf(stderr, "PCTOPS %08x", pc);
					for (unsigned long wi = 0; wi < pctrace_words; wi++) {
						uae_u16 w = get_iword((int)(wi * 2));
						fprintf(stderr, " w%lu=%04x", wi, (unsigned)w);
					}
					fprintf(stderr, "\n");
				}
			}
jit_pctrace_done:
			;
		}
#endif
#if defined(CPU_AARCH64)
		/* Inhibit one_tick() during block tracing. The tick thread's
		   one_tick() has side effects (incrementing Ticks, SDL events)
		   that happen during interpreter tracing but NOT during native
		   block execution. This asymmetry causes different execution paths.
		   Inhibiting during tracing (typically <64 instructions, ~microseconds)
		   has negligible impact on 60Hz timing accuracy. */
		/* ARM64: tick_inhibit was previously set during block tracing to
		   prevent one_tick() side effects from causing non-deterministic
		   paths. However, with B2_JIT_MAXRUN=1, every instruction traces
		   individually, and the inhibit starves the 60Hz timer, preventing
		   boot progress. Allow one_tick() during tracing. */
		extern bool tick_inhibit;
		/* Do NOT inhibit ticks during tracing. With MAXRUN=1 every dispatch
		   traces one instruction; inhibiting here starves the 60Hz timer
		   and prevents the Mac OS Device Manager from completing async I/O. */
		/* tick_inhibit = false; — already false from compile_block */
		uae_u32 verify_block_pc = get_virtual_address((uae_u8*)regs.pc_p);
		const bool verify_this_block = !jit_block_verify_reentrant && jit_verify_block_target_pc(verify_block_pc);
		if (verify_this_block)
			jit_block_verify_entry_capture(verify_block_pc);
#endif
		int maxrun_limit = MAXRUN;
		{
			static int env_maxrun = -1;
			if (env_maxrun < 0) {
				const char *env = getenv("B2_JIT_MAXRUN");
				env_maxrun = (env && *env) ? atoi(env) : MAXRUN;
			}
			maxrun_limit = env_maxrun;
		}
		for (;;) {
			/* Optional verifier block-split probe. When
			   B2_FORCE_BLOCK_BREAK_BEFORE=<guest pc> matches after at least one
			   retired instruction, end the trace so the next dispatch starts at the
			   requested architectural boundary. */
			{
				static int bb_init = -1;
				static uae_u32 bb_target = 0;
				if (bb_init < 0) {
					const char* e = getenv("B2_FORCE_BLOCK_BREAK_BEFORE");
					bb_target = (e && *e) ? (uae_u32)strtoul(e, 0, 0) : 0;
					bb_init = 0;
				}
				if (bb_target && blocklen > 0 && m68k_getpc() == bb_target) {
#if defined(CPU_AARCH64)
					/* Route to the verifier when armed so a break-before can
					   bisect a target block to the first diverging op. */
					if (verify_this_block) {
						uae_u32 _bpc = get_virtual_address((uae_u8*)pc_hist[0].location);
						jit_block_verify_run(pc_hist, blocklen, total_cycles, _bpc);
						return;
					}
#endif
					if (!jit_strict_defer_cold_ram_trace(pc_hist, blocklen))
						compile_block(pc_hist, blocklen, total_cycles);
					return;
				}
			}
			cpu_history *hist = &pc_hist[blocklen++];
			hist->location = (uae_u16 *)regs.pc_p;
			uae_u32 opcode = GET_OPCODE;
			/* GET_OPCODE is host-table ordered under direct addressing. Retain
			   both the logical opcode and the complete maximum architectural
			   encoding window; later instructions in this same trace may rewrite
			   opcode or extension words before compile_block starts. */
			hist->opcode = (uae_u16)do_get_mem_word(hist->location);
			memcpy(hist->source, hist->location, JIT_TRACE_SOURCE_BYTES);
#if defined(CPU_AARCH64)
			{
				/* Assertion-only first-seen state capture. Cache the environment
				   contract once so ordinary tracing does not call getenv/strtoul for
				   every guest instruction. */
				static unsigned long interpop_count = 0;
				uae_u32 want = 0;
				if (jit_interpop_assert_target(&want)) {
					const uae_u32 pc_now = m68k_getpc();
					if (want == pc_now && interpop_count < 32) {
						interpop_count++;
						fprintf(stderr, "INTERPOP pc=%08x count=%lu op=%04x d0=%08x d1=%08x d2=%08x d3=%08x d4=%08x d5=%08x d7=%08x a0=%08x a1=%08x a2=%08x a3=%08x a6=%08x a7=%08x sr=%04x\n",
							pc_now, interpop_count, opcode,
							regs.regs[0], regs.regs[1], regs.regs[2], regs.regs[3],
							regs.regs[4], regs.regs[5], regs.regs[7], regs.regs[8],
							regs.regs[9], regs.regs[10], regs.regs[11], regs.regs[14],
							regs.regs[15], regs.sr);
					}
				}
			}
#endif
			jit_current_interp_pc = m68k_getpc();
			jit_current_interp_opcode = opcode;
			if (jit_guest_instruction_observer_enabled())
				jit_guest_path_record_trace(jit_current_interp_pc);
			if (jit_trace_target_pc(jit_current_interp_pc))
				jit_trace_pc_hit(jit_current_interp_pc, (2u << 16) | (opcode & 0xffff));
			jit_strict_note_trace_op(jit_current_interp_pc, opcode);
			(*cpufunctbl[opcode])(opcode);
			cpu_check_ticks();
			total_cycles += 4 * CYCLE_UNIT;
			bool must_end = __atomic_load_n(&regs.spcflags, __ATOMIC_ACQUIRE) || blocklen >= maxrun_limit;
			/* A compiled block is a basic block: once the interpreter tracer
			   executes an instruction classified as control flow, stop tracing
			   and compile exactly the instructions retired so far.  Following a
			   sampled forward outcome turns data-dependent control flow into a
			   reusable trace and requires every later stage to reconstruct side
			   exits, runtime PCs, flags, and register state perfectly.  The old
			   policy accumulated opcode- and guest-PC-specific exceptions when
			   those assumptions failed. */
			if (!must_end && end_block(opcode))
				must_end = true;
			if (must_end) {
#if defined(CPU_AARCH64)
				tick_inhibit = false;
				uae_u32 block_pc = get_virtual_address((uae_u8*)pc_hist[0].location);
				{
					static int trace_log = 0;
					if (0 && trace_log++ < 50)
						fprintf(stderr, "TRACE_END blk=%08x len=%d\n", block_pc, blocklen);
				}
				if (verify_this_block) {
					jit_block_verify_run(pc_hist, blocklen, total_cycles, block_pc);
					return;
				}
#endif
				if (!jit_strict_defer_cold_ram_trace(pc_hist, blocklen))
					compile_block(pc_hist, blocklen, total_cycles);
				return;
			}
		}
	}
}

void execute_exception(uae_u32 cycles)
{
	if (jit_test_dispatch_summary_enabled())
		jit_test_handle_except_received_cycles = cycles;
	countdown -= cycles;
	const uae_u32 request = regs.jit_exception;
	const bool has_oldpc = (request & JIT_EXCEPTION_OLDPC_VALID) != 0;
	const bool is_chk = (request & JIT_EXCEPTION_CHK_N_VALID) != 0;
	if (is_chk)
		SET_NFLG((request & JIT_EXCEPTION_CHK_N_SET) != 0);
	Exception(request & JIT_EXCEPTION_VECTOR_MASK,
		has_oldpc ? regs.jit_exception_oldpc : 0);
	regs.jit_exception = 0;
	regs.jit_exception_oldpc = 0;
}

/* --- JIT native-call helpers for SR/CCR opcodes --- */

extern "C" void jit_op_MakeFromSR(void)
{
    MakeFromSR();
}

extern "C" void jit_op_MakeSR(void)
{
    MakeSR();
}

/* ORI/ANDI/EORI to SR/CCR helpers.
   regs.jit_exception encoding:
   - bits 0-15: immediate value
   - bit 16: 1=word (full SR), 0=byte (CCR only) */

extern "C" void jit_op_orsr(void)
{
    uae_u32 encoded = regs.jit_exception;
    uae_u16 val = (uae_u16)(encoded & 0xFFFF);
    int is_word = (encoded >> 16) & 1;
    MakeSR();
    if (is_word) {
        regs.sr |= val;
    } else {
        regs.sr = (regs.sr & 0xFF00) | ((regs.sr | val) & 0xFF);
    }
    MakeFromSR();
}

extern "C" void jit_op_andsr(void)
{
    uae_u32 encoded = regs.jit_exception;
    uae_u16 val = (uae_u16)(encoded & 0xFFFF);
    int is_word = (encoded >> 16) & 1;
    MakeSR();
    if (is_word) {
        regs.sr &= val;
    } else {
        regs.sr = (regs.sr & 0xFF00) | ((regs.sr & val) & 0xFF);
    }
    MakeFromSR();
}

extern "C" void jit_op_eorsr(void)
{
    uae_u32 encoded = regs.jit_exception;
    uae_u16 val = (uae_u16)(encoded & 0xFFFF);
    int is_word = (encoded >> 16) & 1;
    MakeSR();
    if (is_word) {
        regs.sr ^= val;
    } else {
        regs.sr = (regs.sr & 0xFF00) | ((regs.sr ^ val) & 0xFF);
    }
    MakeFromSR();
}

/* ================================================================
 * JIT native-call helpers for opcodes that were interpreter-only.
 * Called via compemu_raw_call from compiled JIT blocks.
 * All helpers operate on regs struct directly after flush.
 * ================================================================ */

/* --- Divide helpers ---
 * These are called after src and dst are flushed to regs.
 * regs.jit_exception encodes: bits 0-2 = dst reg, bits 3-5 = src value location.
 * Actually, simpler: we store both values to scratch regs.
 */

/* Helper for DIVU.W Dn (16-bit unsigned divide)
 * jit_exception = (dst_reg << 16) | src_value(16 bit)
 * But src could be from memory... 
 * 
 * Actually, the simplest approach for DIV:
 * The gencomp genamode already loads src into a virtual register.
 * We store src to scratchregs[0], and the dst reg number to jit_exception.
 * The dst value is in regs.regs[dst_reg].
 */

extern "C" void jit_op_divu_w(void)
{
    int dst_reg = regs.jit_exception & 0xF;
    uae_u32 src = regs.scratchregs[0];
    uae_u32 dst = regs.regs[dst_reg];
    
    if (src == 0) {
        /* Division by zero — trigger exception */
        Exception(5, 0);
        return;
    }
    
    uae_u32 quot = dst / (uae_u16)src;
    uae_u32 rem = dst % (uae_u16)src;
    
    /* Check overflow: quotient must fit in 16 bits */
    if (quot > 0xFFFF) {
        /* Overflow: set V flag, leave dest unchanged */
        SET_VFLG(1);
        SET_NFLG(1); /* N is set on overflow per M68K spec */
        SET_CFLG(0);
        return;
    }
    
    SET_VFLG(0);
    SET_CFLG(0);
    SET_ZFLG(quot == 0);
    SET_NFLG((uae_s16)quot < 0);
    regs.regs[dst_reg] = (rem << 16) | (quot & 0xFFFF);
}

extern "C" void jit_op_divs_w(void)
{
    int dst_reg = regs.jit_exception & 0xF;
    uae_s32 src = (uae_s16)(regs.scratchregs[0] & 0xFFFF);
    uae_s32 dst = (uae_s32)regs.regs[dst_reg];
    
    if (src == 0) {
        Exception(5, 0);
        return;
    }
    
    uae_s32 quot = dst / src;
    uae_s32 rem = dst % src;
    
    /* Check overflow: quotient must fit in signed 16 bits */
    if (quot > 32767 || quot < -32768) {
        SET_VFLG(1);
        SET_NFLG(1);
        SET_CFLG(0);
        return;
    }
    
    SET_VFLG(0);
    SET_CFLG(0);
    SET_ZFLG(quot == 0);
    SET_NFLG((uae_s16)quot < 0);
    regs.regs[dst_reg] = ((uae_u16)rem << 16) | ((uae_u16)quot & 0xFFFF);
}

/* --- Long multiply/divide (32×32→64, 64/32→32:32) ---
 * These use two extension words encoding Dl, Dh registers.
 * jit_exception = extension word
 * scratchregs[0] = source value
 */

extern "C" void jit_op_mull(void)
{
    uae_u32 ext = regs.jit_exception;
    uae_u32 src = regs.scratchregs[0];
    int dl = (ext >> 12) & 7;
    int dh = ext & 7;
    int is_signed = ext & 0x800;
    int is_64bit = ext & 0x400;
    
    if (is_signed) {
        uae_s64 result = (uae_s64)(uae_s32)src * (uae_s64)(uae_s32)regs.regs[dl];
        SET_VFLG(0);
        SET_CFLG(0);
        if (is_64bit) {
            regs.regs[dh] = (uae_u32)(result >> 32);
            regs.regs[dl] = (uae_u32)result;
            SET_ZFLG(result == 0);
            SET_NFLG(result < 0);
        } else {
            regs.regs[dl] = (uae_u32)result;
            SET_ZFLG((uae_u32)result == 0);
            SET_NFLG((uae_s32)result < 0);
            if (result > 0x7FFFFFFFLL || result < -0x80000000LL)
                SET_VFLG(1);
        }
    } else {
        uae_u64 result = (uae_u64)src * (uae_u64)regs.regs[dl];
        SET_VFLG(0);
        SET_CFLG(0);
        if (is_64bit) {
            regs.regs[dh] = (uae_u32)(result >> 32);
            regs.regs[dl] = (uae_u32)result;
            SET_ZFLG(result == 0);
            SET_NFLG((uae_s64)result < 0);
        } else {
            regs.regs[dl] = (uae_u32)result;
            SET_ZFLG((uae_u32)result == 0);
            SET_NFLG((uae_s32)(uae_u32)result < 0);
            if (result > 0xFFFFFFFFULL)
                SET_VFLG(1);
        }
    }
}

extern "C" void jit_op_divl(void)
{
    uae_u32 ext = regs.jit_exception;
    uae_u32 src = regs.scratchregs[0];
    int dq = (ext >> 12) & 7;
    int dr = ext & 7;
    int is_signed = ext & 0x800;
    int is_64bit = ext & 0x400;
    
    if (src == 0) {
        Exception(5, 0);
        return;
    }
    
    if (is_signed) {
        if (is_64bit) {
            uae_s64 dividend = ((uae_s64)(uae_s32)regs.regs[dr] << 32) | regs.regs[dq];
            uae_s64 quot = dividend / (uae_s32)src;
            uae_s32 rem = dividend % (uae_s32)src;
            if (quot > 0x7FFFFFFFLL || quot < -0x80000000LL) {
                SET_VFLG(1);
                SET_NFLG(1);
                SET_CFLG(0);
                return;
            }
            SET_VFLG(0); SET_CFLG(0);
            SET_ZFLG((uae_s32)quot == 0);
            SET_NFLG((uae_s32)quot < 0);
            regs.regs[dr] = (uae_u32)rem;
            regs.regs[dq] = (uae_u32)quot;
        } else {
            uae_s32 quot = (uae_s32)regs.regs[dq] / (uae_s32)src;
            uae_s32 rem = (uae_s32)regs.regs[dq] % (uae_s32)src;
            SET_VFLG(0); SET_CFLG(0);
            SET_ZFLG(quot == 0);
            SET_NFLG(quot < 0);
            if (dr != dq) regs.regs[dr] = (uae_u32)rem;
            regs.regs[dq] = (uae_u32)quot;
        }
    } else {
        if (is_64bit) {
            uae_u64 dividend = ((uae_u64)regs.regs[dr] << 32) | regs.regs[dq];
            uae_u64 quot = dividend / src;
            uae_u32 rem = dividend % src;
            if (quot > 0xFFFFFFFFULL) {
                SET_VFLG(1);
                SET_NFLG(1);
                SET_CFLG(0);
                return;
            }
            SET_VFLG(0); SET_CFLG(0);
            SET_ZFLG((uae_u32)quot == 0);
            SET_NFLG((uae_s32)(uae_u32)quot < 0);
            regs.regs[dr] = rem;
            regs.regs[dq] = (uae_u32)quot;
        } else {
            uae_u32 quot = regs.regs[dq] / src;
            uae_u32 rem = regs.regs[dq] % src;
            SET_VFLG(0); SET_CFLG(0);
            SET_ZFLG(quot == 0);
            SET_NFLG((uae_s32)quot < 0);
            if (dr != dq) regs.regs[dr] = rem;
            regs.regs[dq] = quot;
        }
    }
}

/* --- BCD helpers --- */

extern "C" void jit_op_abcd(void)
{
    /* jit_exception: bits 0-2 = dst reg, bits 3-5 = src reg, bit 6 = predec mode */
    int dst_reg = regs.jit_exception & 7;
    int src_reg = (regs.jit_exception >> 3) & 7;
    int predec = (regs.jit_exception >> 6) & 1;
    
    uae_u8 src_val, dst_val;
    uae_u32 src_addr, dst_addr;
    
    if (predec) {
        /* -(An),-(An) mode */
        src_addr = regs.regs[8 + src_reg] -= 1;
        dst_addr = regs.regs[8 + dst_reg] -= 1;
        src_val = get_byte(src_addr);
        dst_val = get_byte(dst_addr);
    } else {
        /* Dn,Dn mode */
        src_val = (uae_u8)regs.regs[src_reg];
        dst_val = (uae_u8)regs.regs[dst_reg];
    }
    
    int x = GET_XFLG();
    int lo = (src_val & 0xF) + (dst_val & 0xF) + x;
    int carry = 0;
    if (lo > 9) { lo -= 10; carry = 1; }
    int hi = ((src_val >> 4) & 0xF) + ((dst_val >> 4) & 0xF) + carry;
    carry = 0;
    if (hi > 9) { hi -= 10; carry = 1; }
    
    uae_u8 result = (uae_u8)((hi << 4) | (lo & 0xF));
    
    SET_XFLG(carry);
    SET_CFLG(carry);
    if (result != 0) SET_ZFLG(0);
    /* N and V are undefined for BCD */
    
    if (predec) {
        put_byte(dst_addr, result);
    } else {
        regs.regs[dst_reg] = (regs.regs[dst_reg] & 0xFFFFFF00) | result;
    }
}

extern "C" void jit_op_sbcd(void)
{
    int dst_reg = regs.jit_exception & 7;
    int src_reg = (regs.jit_exception >> 3) & 7;
    int predec = (regs.jit_exception >> 6) & 1;
    
    uae_u8 src_val, dst_val;
    uae_u32 src_addr, dst_addr;
    
    if (predec) {
        src_addr = regs.regs[8 + src_reg] -= 1;
        dst_addr = regs.regs[8 + dst_reg] -= 1;
        src_val = get_byte(src_addr);
        dst_val = get_byte(dst_addr);
    } else {
        src_val = (uae_u8)regs.regs[src_reg];
        dst_val = (uae_u8)regs.regs[dst_reg];
    }
    
    int x = GET_XFLG();
    int lo = (dst_val & 0xF) - (src_val & 0xF) - x;
    int borrow = 0;
    if (lo < 0) { lo += 10; borrow = 1; }
    int hi = ((dst_val >> 4) & 0xF) - ((src_val >> 4) & 0xF) - borrow;
    borrow = 0;
    if (hi < 0) { hi += 10; borrow = 1; }
    
    uae_u8 result = (uae_u8)((hi << 4) | (lo & 0xF));
    
    SET_XFLG(borrow);
    SET_CFLG(borrow);
    if (result != 0) SET_ZFLG(0);
    
    if (predec) {
        put_byte(dst_addr, result);
    } else {
        regs.regs[dst_reg] = (regs.regs[dst_reg] & 0xFFFFFF00) | result;
    }
}

extern "C" void jit_op_nbcd(void)
{
    /* jit_exception: bits 0-2 = reg (for Dn mode), bit 3 = 1 if memory (addr in scratchregs[0]) */
    int reg = regs.jit_exception & 7;
    int is_mem = (regs.jit_exception >> 3) & 1;
    
    uae_u8 val;
    uae_u32 addr = 0;
    
    if (is_mem) {
        addr = regs.scratchregs[0];
        val = get_byte(addr);
    } else {
        val = (uae_u8)regs.regs[reg];
    }
    
    int x = GET_XFLG();
    int lo = 0 - (val & 0xF) - x;
    int borrow = 0;
    if (lo < 0) { lo += 10; borrow = 1; }
    int hi = 0 - ((val >> 4) & 0xF) - borrow;
    borrow = 0;
    if (hi < 0) { hi += 10; borrow = 1; }
    
    uae_u8 result = (uae_u8)((hi << 4) | (lo & 0xF));
    
    SET_XFLG(borrow);
    SET_CFLG(borrow);
    if (result != 0) SET_ZFLG(0);
    
    if (is_mem) {
        put_byte(addr, result);
    } else {
        regs.regs[reg] = (regs.regs[reg] & 0xFFFFFF00) | result;
    }
}

/* --- MOVEP helpers --- */

extern "C" void jit_op_mvprm(uae_u32 next_pc)
{
    /* Move register to peripheral (byte-interleaved write).
     * jit_exception: bits 0-2 = An, bits 3-5 = Dn, bit 6 = long mode.
     *
     * The ordered-helper ABI enters with the exact opcode PC.  Every lane is
     * independently faultable there; the successor becomes architectural only
     * after the final successful write, matching the interpreter. */
    int an = regs.jit_exception & 7;
    int dn = (regs.jit_exception >> 3) & 7;
    int is_long = (regs.jit_exception >> 6) & 1;
    uae_s16 disp = (uae_s16)(regs.jit_exception >> 16);
    uae_u32 addr = regs.regs[8 + an] + disp;
    uae_u32 val = regs.regs[dn];

    if (is_long) {
        put_byte(addr, (val >> 24) & 0xFF); addr += 2;
        put_byte(addr, (val >> 16) & 0xFF); addr += 2;
    }
    put_byte(addr, (val >> 8) & 0xFF); addr += 2;
    put_byte(addr, val & 0xFF);
    m68k_setpc(next_pc);
}

extern "C" void jit_op_mvpmr(uae_u32 next_pc)
{
    /* All interleaved source reads fault at the opcode PC.  Commit the complete
       register value before publishing the explicit successor. */
    int an = regs.jit_exception & 7;
    int dn = (regs.jit_exception >> 3) & 7;
    int is_long = (regs.jit_exception >> 6) & 1;
    uae_s16 disp = (uae_s16)(regs.jit_exception >> 16);
    uae_u32 addr = regs.regs[8 + an] + disp;
    uae_u32 val = 0;

    if (is_long) {
        val = (get_byte(addr) << 24); addr += 2;
        val |= (get_byte(addr) << 16); addr += 2;
    }
    val |= (get_byte(addr) << 8); addr += 2;
    val |= get_byte(addr);

    if (is_long) {
        regs.regs[dn] = val;
    } else {
        regs.regs[dn] = (regs.regs[dn] & 0xFFFF0000) | (val & 0xFFFF);
    }
    m68k_setpc(next_pc);
}

/* --- Flow control helpers --- */

extern "C" void jit_op_rtr(void)
{
    /* RTR: pop CCR (word) and PC from stack */
    uae_u32 sp = m68k_areg(regs, 7);
    MakeSR();
    uae_u16 ccr = get_word(sp); sp += 2;
    regs.sr = (regs.sr & 0xFF00) | (ccr & 0xFF);
    MakeFromSR();
    regs.pc = get_long(sp); sp += 4;
    m68k_areg(regs, 7) = sp;
    fill_prefetch_0();
}

extern "C" void jit_op_trap(void)
{
    /* TRAP #vector */
    int vector = regs.jit_exception & 0xF;
    Exception(vector + 32, 0);
}

extern "C" void jit_op_trapv(void)
{
    if (GET_VFLG()) {
        Exception(7, 0);
    }
}

extern "C" void jit_op_moves(void)
{
    /* MOVES: In user/supervisor mode without real MMU,
     * this is effectively a normal move.
     * jit_exception = extension word, scratchregs[0] = EA value.
     * The extension word encodes: bit 11 = direction (0=EA→Rn, 1=Rn→EA),
     * bits 15-12 = register.
     * For now: treat as a no-op since we don't have an MMU. */
    /* Actually, the interpreter implementation does real memory accesses.
     * Let's do the same. For simplicity, fall through to interpreter. */
}

extern "C" void jit_op_chk(void)
{
    /* CHK Dn,<ea> — trap if Dn < 0 or Dn > src */
    int dst_reg = regs.jit_exception & 7;
    uae_s16 src = (uae_s16)(regs.scratchregs[0] & 0xFFFF);
    uae_s16 dst = (uae_s16)(regs.regs[dst_reg] & 0xFFFF);
    
    SET_ZFLG(dst == 0);
    SET_VFLG(0);
    SET_CFLG(0);
    
    if (dst < 0) {
        SET_NFLG(1);
        Exception(6, 0);
    } else if (dst > src) {
        SET_NFLG(0);
        Exception(6, 0);
    }
}

extern "C" void jit_op_chk2(void)
{
    /* scratchregs: [0] bounds EA, [1] extension word, [2] element size.
       CHK2 performs two separately faultable reads in ascending address order.
       It changes only Z and C; N, V, and X survive exactly as on the 68020+
       interpreter path.  Trap delivery is deferred through jit_exception so
       the JIT dispatcher retains the precise current-instruction PC. */
    const uaecptr ea = regs.scratchregs[0];
    const uae_u16 extra = (uae_u16)regs.scratchregs[1];
    const unsigned size = regs.scratchregs[2];
    uae_s32 lower;
    uae_s32 upper;

    regs.jit_exception = 0;
    switch (size) {
    case 1:
        lower = (uae_s32)(uae_s8)get_byte(ea);
        upper = (uae_s32)(uae_s8)get_byte(ea + 1);
        break;
    case 2:
        lower = (uae_s32)(uae_s16)get_word(ea);
        upper = (uae_s32)(uae_s16)get_word(ea + 2);
        break;
    case 4:
        lower = (uae_s32)get_long(ea);
        upper = (uae_s32)get_long(ea + 4);
        break;
    default:
        /* Generator/helper ABI corruption must fail closed, never become an
           implicit guest result or interpreter dispatch. */
        abort();
    }

    uae_s32 value = (uae_s32)regs.regs[(extra >> 12) & 15];
    if ((extra & 0x8000) == 0) {
        if (size == 1)
            value = (uae_s32)(uae_s8)value;
        else if (size == 2)
            value = (uae_s32)(uae_s16)value;
    }

    const bool equal_bound = value == lower || value == upper;
    const bool outside = lower <= upper
        ? value < lower || value > upper
        : value > upper || value < lower;
    SET_ZFLG(equal_bound);
    SET_CFLG(outside);
    if ((extra & 0x0800) != 0 && outside)
        regs.jit_exception = 6;
}

/* --- TAS helper --- */
extern "C" void jit_op_tas(void)
{
    /* jit_exception: bit 3 = memory mode, bits 0-2 = Dn reg (if !memory)
     * scratchregs[0] = EA address (if memory) */
    int is_mem = (regs.jit_exception >> 3) & 1;
    int reg = regs.jit_exception & 7;
    
    uae_u8 val;
    uae_u32 addr = 0;
    
    if (is_mem) {
        addr = regs.scratchregs[0];
        val = get_byte(addr);
    } else {
        val = (uae_u8)regs.regs[reg];
    }
    
    SET_ZFLG(val == 0);
    SET_NFLG(val & 0x80);
    SET_VFLG(0);
    SET_CFLG(0);
    
    val |= 0x80; /* Set bit 7 */
    
    if (is_mem) {
        put_byte(addr, val);
    } else {
        regs.regs[reg] = (regs.regs[reg] & 0xFFFFFF00) | val;
    }
}

/* --- PACK/UNPK helpers --- */
extern "C" void jit_op_pack(uae_u32 next_pc)
{
    /* The ordered-helper ABI enters at the opcode PC.  Source reads retain that
       PC; the predecrement destination write observes the explicit successor,
       exactly where the interpreter performs m68k_incpc(). */
    int dst_reg = regs.jit_exception & 7;
    int src_reg = (regs.jit_exception >> 3) & 7;
    int predec = (regs.jit_exception >> 6) & 1;
    uae_s16 adj = (uae_s16)(regs.jit_exception >> 16);

    uae_u16 val;
    if (predec) {
        /* Canonical 68040 ordering is deliberately not a get_word(): the two
         source bytes are independently predecrement-addressed, which matters
         for A7 and for a source/destination register alias.  The architectural
         source update itself is two bytes for every An. */
        const uae_u32 source = regs.regs[8 + src_reg];
        val = (uae_u16)get_byte(source - areg_byteinc[src_reg]);
        val |= (uae_u16)get_byte(source - 2 * areg_byteinc[src_reg]) << 8;
        regs.regs[8 + src_reg] -= 2;
    } else {
        val = (uae_u16)regs.regs[src_reg];
    }

    val += adj;
    const uae_u8 result = ((val >> 4) & 0xF0) | (val & 0x0F);

    if (predec) {
        regs.regs[8 + dst_reg] -= areg_byteinc[dst_reg];
        m68k_setpc(next_pc);
        put_byte(regs.regs[8 + dst_reg], result);
    } else {
        regs.regs[dst_reg] = (regs.regs[dst_reg] & 0xFFFFFF00) | result;
        m68k_setpc(next_pc);
    }
}

extern "C" void jit_op_unpk(uae_u32 next_pc)
{
    int dst_reg = regs.jit_exception & 7;
    int src_reg = (regs.jit_exception >> 3) & 7;
    int predec = (regs.jit_exception >> 6) & 1;
    uae_s16 adj = (uae_s16)(regs.jit_exception >> 16);

    uae_u8 val;
    if (predec) {
        regs.regs[8 + src_reg] -= areg_byteinc[src_reg];
        val = get_byte(regs.regs[8 + src_reg]);
    } else {
        val = (uae_u8)regs.regs[src_reg];
    }

    uae_u16 result = ((val & 0xF0) << 4) | (val & 0x0F);
    result += adj;

    if (predec) {
        regs.regs[8 + dst_reg] -= 2;
        m68k_setpc(next_pc);
        put_word(regs.regs[8 + dst_reg], result);
    } else {
        regs.regs[dst_reg] = (regs.regs[dst_reg] & 0xFFFF0000) | result;
        m68k_setpc(next_pc);
    }
}

/* --- BFFFO helper --- */
extern "C" void jit_op_bfffo(void)
{
    /* Bit Field Find First One.
     * jit_exception = extension word
     * scratchregs[0] = effective address or Dn number; scratchregs[1]
     *                  distinguishes Dn from the full 32-bit memory address.
     */
    const uae_u32 ext = regs.jit_exception;
    const uae_u32 ea_info = regs.scratchregs[0];
    uae_s32 offset = (ext & 0x800) ? (uae_s32)regs.regs[(ext >> 6) & 7] : (uae_s32)((ext >> 6) & 0x1f);
    const int width = ((((ext & 0x20) ? regs.regs[ext & 7] : ext) - 1) & 0x1f) + 1;
    uae_u32 tmp;

    if (regs.scratchregs[1]) {
        const int src_reg = ea_info & 7;
        offset &= 0x1f;
        const uae_u32 src = regs.regs[src_reg];
        tmp = offset ? ((src << offset) | (src >> (32 - offset))) : src;
    } else {
        uae_u32 bdata[2];
        uae_u32 addr = ea_info + (offset >> 3);
        tmp = get_bitfield(addr, bdata, offset, width);
    }

    SET_NFLG(((uae_s32)tmp) < 0 ? 1 : 0);
    tmp >>= (32 - width);
    SET_ZFLG(tmp == 0);
    SET_VFLG(0);
    SET_CFLG(0);

    uae_u32 mask = 1u << (width - 1);
    while (mask) {
        if (tmp & mask)
            break;
        mask >>= 1;
        offset++;
    }
    regs.regs[(ext >> 12) & 7] = (uae_u32)offset;
}

/* --- BFEXTU helper --- */
extern "C" void jit_op_bfextu(void)
{
    /* Bit Field Extract Unsigned. Mirrors the interpreter (gencpu.c i_BFEXTU).
     * jit_exception = extension word; scratchregs[0] = memory EA or Dn;
     * scratchregs[1] distinguishes the two. Result -> Dn=(ext>>12)&7. */
    const uae_u32 ext = regs.jit_exception;
    const uae_u32 ea_info = regs.scratchregs[0];
    uae_s32 offset = (ext & 0x800) ? (uae_s32)regs.regs[(ext >> 6) & 7] : (uae_s32)((ext >> 6) & 0x1f);
    const int width = ((((ext & 0x20) ? regs.regs[ext & 7] : ext) - 1) & 0x1f) + 1;
    uae_u32 tmp;

    if (regs.scratchregs[1]) {
        const int src_reg = ea_info & 7;
        offset &= 0x1f;
        const uae_u32 src = regs.regs[src_reg];
        tmp = offset ? ((src << offset) | (src >> (32 - offset))) : src;
    } else {
        uae_u32 bdata[2];
        uae_u32 addr = ea_info + (offset >> 3);
        tmp = get_bitfield(addr, bdata, offset, width);
    }

    SET_NFLG(((uae_s32)tmp) < 0 ? 1 : 0);
    tmp >>= (32 - width);
    SET_ZFLG(tmp == 0);
    SET_VFLG(0);
    SET_CFLG(0);
    regs.regs[(ext >> 12) & 7] = tmp;
}

/* --- BFEXTS helper --- */
extern "C" void jit_op_bfexts(void)
{
    /* Bit Field Extract Signed. As BFEXTU but the result is sign-extended
     * (arithmetic shift). N flag is the field MSB (before the shift), Z is on
     * the final sign-extended result, matching the interpreter. */
    const uae_u32 ext = regs.jit_exception;
    const uae_u32 ea_info = regs.scratchregs[0];
    uae_s32 offset = (ext & 0x800) ? (uae_s32)regs.regs[(ext >> 6) & 7] : (uae_s32)((ext >> 6) & 0x1f);
    const int width = ((((ext & 0x20) ? regs.regs[ext & 7] : ext) - 1) & 0x1f) + 1;
    uae_u32 tmp;

    if (regs.scratchregs[1]) {
        const int src_reg = ea_info & 7;
        offset &= 0x1f;
        const uae_u32 src = regs.regs[src_reg];
        tmp = offset ? ((src << offset) | (src >> (32 - offset))) : src;
    } else {
        uae_u32 bdata[2];
        uae_u32 addr = ea_info + (offset >> 3);
        tmp = get_bitfield(addr, bdata, offset, width);
    }

    SET_NFLG(((uae_s32)tmp) < 0 ? 1 : 0);
    uae_s32 res = (uae_s32)tmp >> (32 - width);
    SET_ZFLG(res == 0);
    SET_VFLG(0);
    SET_CFLG(0);
    regs.regs[(ext >> 12) & 7] = (uae_u32)res;
}

/* --- BFTST/BFCHG/BFCLR/BFSET shared helper ---
 * Byte-exact mirror of the interpreter (gencpu.c i_BFTST/BFCHG/BFCLR/BFSET):
 * same get_bitfield/put_bitfield + bdata[] idiom and the same Dn rotate.
 * op: 0=TST (no write), 1=CHG, 2=CLR, 3=SET.
 * jit_exception = extension word; scratchregs[0] = memory EA or Dn number;
 * scratchregs[1] distinguishes the two without stealing an address bit. */
static inline void jit_bf_rmw(int op)
{
    const uae_u32 ext = regs.jit_exception;
    const uae_u32 ea_info = regs.scratchregs[0];
    uae_s32 offset = (ext & 0x800) ? (uae_s32)regs.regs[(ext >> 6) & 7] : (uae_s32)((ext >> 6) & 0x1f);
    const int width = ((((ext & 0x20) ? regs.regs[ext & 7] : ext) - 1) & 0x1f) + 1;
    const int is_dreg = regs.scratchregs[1] != 0;
    const int dreg = ea_info & 7;
    uae_u32 bdata[2];
    uae_u32 dsta = 0;
    uae_u32 tmp;

    if (is_dreg) {
        uae_u32 dv = regs.regs[dreg];
        offset &= 0x1f;
        tmp = (dv << offset) | (offset ? (dv >> (32 - offset)) : 0);
        bdata[0] = tmp & ((1u << (32 - width)) - 1);
    } else {
        dsta = ea_info + (offset >> 3);
        tmp = get_bitfield(dsta, bdata, offset, width);
    }

    SET_NFLG(((uae_s32)tmp) < 0 ? 1 : 0);
    tmp >>= (32 - width);
    SET_ZFLG(tmp == 0);
    SET_VFLG(0);
    SET_CFLG(0);

    switch (op) {
    case 1: tmp = tmp ^ (0xffffffffu >> (32 - width)); break; /* BFCHG */
    case 2: tmp = 0; break;                                   /* BFCLR */
    case 3: tmp = 0xffffffffu >> (32 - width); break;         /* BFSET */
    default: return;                                          /* BFTST: flags only */
    }

    if (is_dreg) {
        tmp = bdata[0] | (tmp << (32 - width));
        regs.regs[dreg] = (tmp >> offset) | (offset ? (tmp << (32 - offset)) : 0);
    } else {
        put_bitfield(dsta, bdata, tmp, offset, width);
    }
}

extern "C" void jit_op_bftst(void) { jit_bf_rmw(0); }
extern "C" void jit_op_bfchg(void) { jit_bf_rmw(1); }
extern "C" void jit_op_bfclr(void) { jit_bf_rmw(2); }
extern "C" void jit_op_bfset(void) { jit_bf_rmw(3); }

/* --- BFINS helper --- */
extern "C" void jit_op_bfins(void)
{
    /* Mirror the interpreter's get_bitfield()/put_bitfield() contract.
     * jit_exception is the extension word; scratchregs[0] is either the
     * memory EA or Dn number, with scratchregs[1] selecting a register.  A dynamic
     * memory offset is a signed 32-bit value and must not be reduced modulo
     * 32 until after distinguishing the register-destination case. */
    const uae_u32 ext = regs.jit_exception;
    const uae_u32 ea_info = regs.scratchregs[0];
    uae_s32 offset = (ext & 0x800)
        ? (uae_s32)regs.regs[(ext >> 6) & 7]
        : (uae_s32)((ext >> 6) & 0x1f);
    const int width = ((((ext & 0x20) ? regs.regs[ext & 7] : ext) - 1) & 0x1f) + 1;
    const uae_u32 field_mask = width == 32 ? 0xffffffffu : ((1u << width) - 1);
    const uae_u32 field = regs.regs[(ext >> 12) & 7] & field_mask;

    if (regs.scratchregs[1]) {
        const int dreg = ea_info & 7;
        const unsigned roff = (unsigned)offset & 0x1f;
        const uae_u32 old = regs.regs[dreg];
        const uae_u32 rotated = roff ? (old << roff) | (old >> (32 - roff)) : old;
        const uae_u32 keep_mask = width == 32 ? 0 : ((1u << (32 - width)) - 1);
        const uae_u32 merged = (rotated & keep_mask) | (field << (32 - width));
        regs.regs[dreg] = roff ? (merged >> roff) | (merged << (32 - roff)) : merged;
    } else {
        uae_u32 bdata[2];
        const uae_u32 dsta = ea_info + (offset >> 3);
        (void)get_bitfield(dsta, bdata, offset, width);
        put_bitfield(dsta, bdata, field, offset, width);
    }

    SET_NFLG((field >> (width - 1)) & 1);
    SET_ZFLG(field == 0);
    SET_VFLG(0);
    SET_CFLG(0);
}

/* --- ROXL/ROXR register helpers --- */

extern "C" void jit_op_roxl(void)
{
    /* jit_exception: bits 0-2 = dst reg, bits 3-4 = size (0=B,1=W,2=L),
     * scratchregs[0] = count */
    int dst_reg = regs.jit_exception & 7;
    int size = (regs.jit_exception >> 3) & 3;
    int count = regs.scratchregs[0] & 63;
    
    int x = GET_XFLG();
    uae_u32 val = regs.regs[dst_reg];
    int bits;
    uae_u32 mask;
    
    switch (size) {
        case 0: bits = 8; mask = 0xFF; break;
        case 1: bits = 16; mask = 0xFFFF; break;
        default: bits = 32; mask = 0xFFFFFFFF; break;
    }
    
    val &= mask;
    int modcount = count % (bits + 1); /* ROXL modulo is bits+1 (includes X) */
    
    for (int i = 0; i < modcount; i++) {
        int msb = (val >> (bits - 1)) & 1;
        val = ((val << 1) | x) & mask;
        x = msb;
    }
    
    SET_XFLG(x);
    SET_CFLG(count ? x : GET_XFLG());
    SET_ZFLG(val == 0);
    SET_NFLG((val >> (bits - 1)) & 1);
    SET_VFLG(0);
    
    regs.regs[dst_reg] = (regs.regs[dst_reg] & ~mask) | val;
}

extern "C" void jit_op_roxr(void)
{
    int dst_reg = regs.jit_exception & 7;
    int size = (regs.jit_exception >> 3) & 3;
    int count = regs.scratchregs[0] & 63;
    
    int x = GET_XFLG();
    uae_u32 val = regs.regs[dst_reg];
    int bits;
    uae_u32 mask;
    
    switch (size) {
        case 0: bits = 8; mask = 0xFF; break;
        case 1: bits = 16; mask = 0xFFFF; break;
        default: bits = 32; mask = 0xFFFFFFFF; break;
    }
    
    val &= mask;
    int modcount = count % (bits + 1);
    
    for (int i = 0; i < modcount; i++) {
        int lsb = val & 1;
        val = ((val >> 1) | ((uae_u32)x << (bits - 1))) & mask;
        x = lsb;
    }
    
    SET_XFLG(x);
    SET_CFLG(count ? x : GET_XFLG());
    SET_ZFLG(val == 0);
    SET_NFLG((val >> (bits - 1)) & 1);
    SET_VFLG(0);
    
    regs.regs[dst_reg] = (regs.regs[dst_reg] & ~mask) | val;
}

/* --- Memory shift/rotate helpers ---
 * These operate on a word at an EA (shift by 1 bit only).
 * scratchregs[0] = effective address */

extern "C" void jit_op_asrw(void)
{
    uae_u32 addr = regs.scratchregs[0];
    uae_s16 val = (uae_s16)get_word(addr);
    int lsb = val & 1;
    val >>= 1; /* Arithmetic shift preserves sign */
    put_word(addr, (uae_u16)val);
    SET_XFLG(lsb); SET_CFLG(lsb);
    SET_ZFLG(val == 0);
    SET_NFLG(val < 0);
    SET_VFLG(0);
}

extern "C" void jit_op_aslw(void)
{
    uae_u32 addr = regs.scratchregs[0];
    uae_u16 val = get_word(addr);
    int msb = (val >> 15) & 1;
    uae_u16 result = val << 1;
    int new_msb = (result >> 15) & 1;
    put_word(addr, result);
    SET_XFLG(msb); SET_CFLG(msb);
    SET_ZFLG(result == 0);
    SET_NFLG(new_msb);
    SET_VFLG(msb != new_msb); /* V set if sign changed */
}

extern "C" void jit_op_lsrw(void)
{
    uae_u32 addr = regs.scratchregs[0];
    uae_u16 val = get_word(addr);
    int lsb = val & 1;
    val >>= 1;
    put_word(addr, val);
    SET_XFLG(lsb); SET_CFLG(lsb);
    SET_ZFLG(val == 0);
    SET_NFLG(0);
    SET_VFLG(0);
}

extern "C" void jit_op_lslw(void)
{
    uae_u32 addr = regs.scratchregs[0];
    uae_u16 val = get_word(addr);
    int msb = (val >> 15) & 1;
    val <<= 1;
    put_word(addr, val);
    SET_XFLG(msb); SET_CFLG(msb);
    SET_ZFLG(val == 0);
    SET_NFLG((val >> 15) & 1);
    SET_VFLG(0);
}

extern "C" void jit_op_rolw(void)
{
    uae_u32 addr = regs.scratchregs[0];
    uae_u16 val = get_word(addr);
    int msb = (val >> 15) & 1;
    val = (val << 1) | msb;
    put_word(addr, val);
    SET_CFLG(msb);
    SET_ZFLG(val == 0);
    SET_NFLG((val >> 15) & 1);
    SET_VFLG(0);
}

extern "C" void jit_op_rorw(void)
{
    uae_u32 addr = regs.scratchregs[0];
    uae_u16 val = get_word(addr);
    int lsb = val & 1;
    val = (val >> 1) | (lsb << 15);
    put_word(addr, val);
    SET_CFLG(lsb);
    SET_ZFLG(val == 0);
    SET_NFLG((val >> 15) & 1);
    SET_VFLG(0);
}

extern "C" void jit_op_roxlw(void)
{
    uae_u32 addr = regs.scratchregs[0];
    uae_u16 val = get_word(addr);
    int msb = (val >> 15) & 1;
    int x = GET_XFLG();
    val = (val << 1) | x;
    put_word(addr, val);
    SET_XFLG(msb); SET_CFLG(msb);
    SET_ZFLG(val == 0);
    SET_NFLG((val >> 15) & 1);
    SET_VFLG(0);
}

extern "C" void jit_op_roxrw(void)
{
    uae_u32 addr = regs.scratchregs[0];
    uae_u16 val = get_word(addr);
    int lsb = val & 1;
    int x = GET_XFLG();
    val = (val >> 1) | (x << 15);
    put_word(addr, val);
    SET_XFLG(lsb); SET_CFLG(lsb);
    SET_ZFLG(val == 0);
    SET_NFLG((val >> 15) & 1);
    SET_VFLG(0);
}

/* --- TRAPcc helper --- */
extern "C" void jit_op_trapcc(void)
{
    /* The condition was already evaluated; if we get here, trap. */
    int cond = regs.jit_exception & 1;
    if (cond) {
        Exception(7, 0);
    }
}


/* ================================================================
 * JIT FPU shadow register sync (MPFR ↔ double)
 * Called at JIT block boundaries when USE_JIT_FPU is enabled.
 * ================================================================ */
#ifdef USE_JIT_FPU
#include "fpu/fpu.h"
#include <cmath>
#include <cstring>
#include <limits>

/* Sync architectural FP registers and FPSR condition state to the native
   double shadows whenever execution enters the JIT from C/interpreter code.
   FP_RESULT is the JIT's lazy condition-code carrier: FBcc compares it with
   zero, so materialise one canonical representative of each architectural
   FP class rather than depending on a preceding native FPP instruction. */
extern "C" void jit_fpu_sync_to_shadow(void)
{
#if defined(CPU_aarch64) || defined(CPU_AARCH64)
    /* AArch64 FPCR RMode uses 00 nearest, 01 +Inf, 10 -Inf, 11 zero;
       the 68k field uses nearest, zero, -Inf, +Inf.  Import only RMode and
       preserve every unrelated host control bit for the return to C. */
    uae_u64 host_fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(host_fpcr));
    regs.jit_host_fpcr = host_fpcr;
    const unsigned guest_round = (fpu_get_fpcr() & FPCR_ROUNDING_MODE) >> 4;
    static const unsigned arm_round[4] = { 0, 3, 2, 1 };
    host_fpcr = (host_fpcr & ~(3ULL << 22)) | ((uae_u64)arm_round[guest_round] << 22);
    __asm__ __volatile__("msr fpcr, %0" : : "r"(host_fpcr) : "memory");
#endif
#ifdef FPU_MPFR
    for (int i = 0; i < 8; i++) {
        regs.jit_fpregs[i] = mpfr_get_d(fpu.registers[i].f, MPFR_RNDN);
        /* MPFR keeps NaN payload/sign outside mpfr_t.  Reconstruct the native
           double shadow with the same high-order payload mapping used by
           extract_to_double(), so a later IEEE-single destination can quiet
           signalling NaNs without losing their architectural payload. */
        if (mpfr_nan_p(fpu.registers[i].f)) {
            uae_u64 bits = 0x7ff0000000000000ULL |
                ((fpu.registers[i].nan_bits >> 11) & 0x000fffffffffffffULL);
            if (fpu.registers[i].nan_sign)
                bits |= 0x8000000000000000ULL;
            std::memcpy(&regs.jit_fpregs[i], &bits, sizeof(bits));
        }
    }
#else
    for (int i = 0; i < 8; i++) {
        regs.jit_fpregs[i] = (double)fpu.registers[i];
    }
#endif
    const uae_u32 fpcc = fpu_get_fpsr() & FPSR_CCB;
    const bool negative = (fpcc & FPSR_CCB_NEGATIVE) != 0;
    if (fpcc & FPSR_CCB_NAN)
        regs.jit_fp_result = std::copysign(
            std::numeric_limits<double>::quiet_NaN(), negative ? -1.0 : 1.0);
    else if (fpcc & FPSR_CCB_INFINITY)
        regs.jit_fp_result = negative
            ? -std::numeric_limits<double>::infinity()
            : std::numeric_limits<double>::infinity();
    else if (fpcc & FPSR_CCB_ZERO)
        regs.jit_fp_result = negative ? -0.0 : 0.0;
    else
        regs.jit_fp_result = negative ? -1.0 : 1.0;
    /* Imported shadows are clean. Native emitters set ownership bits at the
       exact runtime point where an architectural FP value is written. */
    regs.jit_fp_dirty_mask = 0;
}

/* Publish only native-dirty FP values before returning to C or entering an
   interpreter fallback. Untouched binary64 shadows must never narrow wider
   architectural MPFR registers, and a clean lazy result must not overwrite an
   FPSR condition code produced by serviced execution. */
extern "C" void jit_fpu_sync_from_shadow(void)
{
#if defined(CPU_aarch64) || defined(CPU_AARCH64)
    __asm__ __volatile__("msr fpcr, %0" : : "r"(regs.jit_host_fpcr) : "memory");
#endif
    const uae_u32 dirty = regs.jit_fp_dirty_mask;
#ifdef FPU_MPFR
    for (int i = 0; i < 8; i++) {
        if ((dirty & (1u << i)) == 0)
            continue;
        const double shadow = regs.jit_fpregs[i];
        mpfr_set_d(fpu.registers[i].f, shadow, MPFR_RNDN);
        if (std::isnan(shadow)) {
            uae_u64 bits = 0;
            std::memcpy(&bits, &shadow, sizeof(bits));
            fpu.registers[i].nan_bits = (bits & 0x000fffffffffffffULL) << 11;
            fpu.registers[i].nan_sign = (bits >> 63) != 0;
        } else {
            fpu.registers[i].nan_bits = 0xffffffffffffffffULL;
            fpu.registers[i].nan_sign = 0;
        }
    }
#else
    for (int i = 0; i < 8; i++) {
        if (dirty & (1u << i))
            fpu.registers[i] = (fpu_register)regs.jit_fpregs[i];
    }
#endif
    if (dirty & (1u << FP_RESULT)) {
        const double result = regs.jit_fp_result;
        uae_u32 fpcc = 0;
        if (std::isnan(result)) {
            fpcc |= FPSR_CCB_NAN;
            if (std::signbit(result))
                fpcc |= FPSR_CCB_NEGATIVE;
        } else {
            if (std::signbit(result))
                fpcc |= FPSR_CCB_NEGATIVE;
            if (result == 0.0)
                fpcc |= FPSR_CCB_ZERO;
            else if (std::isinf(result))
                fpcc |= FPSR_CCB_INFINITY;
        }
        fpu_set_fpsr((fpu_get_fpsr() & ~FPSR_CCB) | fpcc);
    }
    regs.jit_fp_dirty_mask = 0;
}

#endif /* USE_JIT_FPU */
