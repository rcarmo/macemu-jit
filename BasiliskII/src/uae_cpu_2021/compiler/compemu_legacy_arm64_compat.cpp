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
	LSL_wwi(x, x, 29);
	unlock2(x);
}

static inline int legacy_x86_cc_to_native(int cc)
{
	switch (cc) {
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
	if (d == PC_P) { arm_ADD_l_ri(d, (uintptr)(uae_s64)i); return; }
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

void adc_b(RW1 d, RR1 s)
{
	legacy_fix_inverted_carry();
	INIT_REGS_b(d, s);
	MOV_xi(REG_WORK1, 0);
	BFI_xxii(REG_WORK1, s, 24, 8);
	LSL_wwi(REG_WORK3, d, 24);
	ADCS_www(REG_WORK1, REG_WORK1, REG_WORK3);
	BFXIL_xxii(d, REG_WORK1, 24, 8);
	flags_carry_inverted = false;
	EXIT_REGS(d, s);
}

void adc_w(RW2 d, RR2 s)
{
	legacy_fix_inverted_carry();
	INIT_REGS_w(d, s);
	MOV_xi(REG_WORK1, 0);
	BFI_xxii(REG_WORK1, s, 16, 16);
	LSL_wwi(REG_WORK3, d, 16);
	ADCS_www(REG_WORK1, REG_WORK1, REG_WORK3);
	BFXIL_xxii(d, REG_WORK1, 16, 16);
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

void cmov_l_rr(RW4 d, RR4 s, uae_s32 cc)
{
	if (d == s)
		return;
	d = rmw(d);
	if (isconst(s)) {
		LOAD_U32(REG_WORK1, live.state[s].val);
		CSEL_xxxc(d, REG_WORK1, d, legacy_x86_cc_to_native(cc));
	} else {
		s = readreg(s);
		CSEL_xxxc(d, s, d, legacy_x86_cc_to_native(cc));
		unlock2(s);
	}
	unlock2(d);
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
void imul_64_32(RW4 d, RW4 s) { if (legacy_needflags_enabled()) jff_MULS64(d, s); else jnf_MULS64(d, s); }
void mul_64_32(RW4 d, RW4 s) { if (legacy_needflags_enabled()) jff_MULU64(d, s); else jnf_MULU64(d, s); }

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
	flags_carry_inverted = false;
}

int kill_rodent(int r)
{
	(void)r;
	return 0;
}

void do_nothing(void)
{
#if defined(CPU_AARCH64)
	jit_diag_do_nothing_calls++;
	jit_diag_dispatch_count++;
	jit_diag_maybe_print();
	/* Call cpu_check_ticks on every countdown expiry so that one_tick()
	   runs at the proper cadence even for tight compiled loops. Without
	   this, ROM hardware-polling loops that run natively never give the
	   60Hz timer a chance to update emulated hardware registers. */
	/* one_tick() runs via the tick thread at 60Hz — no cpu_check_ticks needed */
#endif
	/* Intentionally empty otherwise. */
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

void exec_nostats(void)
{
#if defined(CPU_AARCH64)
	jit_diag_exec_nostats_calls++;
	jit_diag_dispatch_count++;
	jit_diag_maybe_print();
	{
		uintptr pcp = (uintptr)regs.pc_p;
		uintptr base = (uintptr)RAMBaseHost;
		uintptr limit = base + RAMSize + ROMSize + 0x1000000;
		if (pcp < base || pcp >= limit || (pcp & 1)) {
			static int bad_count = 0;
			uae_u32 safe_pc = regs.pc & ~1u;
			if (bad_count++ < 50)
				fprintf(stderr, "JIT: exec_nostats bad pc_p=%p regs.pc=%08x d0=%08x d1=%08x a0=%08x a1=%08x a2=%08x a7=%08x sr=%04x spc=%08x oldp=%p last_setpc=%p last_kind=%u last_seq=%lu\n",
					(void*)regs.pc_p, regs.pc,
					regs.regs[0], regs.regs[1], regs.regs[8], regs.regs[9], regs.regs[10], regs.regs[15],
					(unsigned)regs.sr, (unsigned)regs.spcflags, (void*)regs.pc_oldp,
					(void*)jit_last_setpc_value, (unsigned)jit_last_setpc_kind, jit_last_setpc_seq);
			/* Re-derive pc_p from guest PC */
			regs.pc = safe_pc;
			regs.pc_p = get_real_address(safe_pc, 0, sz_word);
			regs.pc_oldp = regs.pc_p - safe_pc;
		}
	}
#endif
	static unsigned long trace_count = 0;
	for (;;) {
		uae_u32 before_pc = m68k_getpc();
		uae_u32 opcode = GET_OPCODE;
		bool trace_this = trace_count < jit_tracewin_limit() && jit_tracewin_match(before_pc);
		if (trace_this) {
			fprintf(stderr,
				"TRACEWINJ BEFORE step=%lu pc=%08x op=%04x regs.pc=%08x pc_p=%p oldp=%p d0=%08x d1=%08x a0=%08x a1=%08x a2=%08x a7=%08x sr=%04x nzcv=%08x x=%08x\n",
				trace_count + 1,
				(unsigned)before_pc,
				(unsigned)opcode,
				(unsigned)regs.pc,
				(void*)regs.pc_p,
				(void*)regs.pc_oldp,
				(unsigned)regs.regs[0],
				(unsigned)regs.regs[1],
				(unsigned)regs.regs[8],
				(unsigned)regs.regs[9],
				(unsigned)regs.regs[10],
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
				"TRACEWINJ AFTER step=%lu pc=%08x op=%04x regs.pc=%08x pc_p=%p oldp=%p d0=%08x d1=%08x a0=%08x a1=%08x a2=%08x a7=%08x sr=%04x nzcv=%08x x=%08x\n",
				trace_count,
				(unsigned)after_pc,
				(unsigned)opcode,
				(unsigned)regs.pc,
				(void*)regs.pc_p,
				(void*)regs.pc_oldp,
				(unsigned)regs.regs[0],
				(unsigned)regs.regs[1],
				(unsigned)regs.regs[8],
				(unsigned)regs.regs[9],
				(unsigned)regs.regs[10],
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

void execute_normal(void)
{
#if defined(CPU_AARCH64)
	jit_diag_execute_normal_calls++;
	jit_diag_dispatch_count++;
	jit_diag_maybe_print();
	/* If pc_p is outside valid Mac memory range (corrupt), re-derive it. */
	{
		uintptr pcp = (uintptr)regs.pc_p;
		uintptr base = (uintptr)RAMBaseHost;
		uintptr limit = base + RAMSize + ROMSize + 0x1000000; /* allocation limit */
		if (pcp < base || pcp >= limit || (pcp & 1)) {
			static int fix_count = 0;
			uae_u32 safe_pc = regs.pc & ~1u;
			if (fix_count++ < 50)
				fprintf(stderr, "JIT: exec_normal bad pc_p=%p regs.pc=%08x safe=%08x "
					"d0=%08x d1=%08x a0=%08x a7=%08x sr=%04x spc=%08x oldp=%p "
					"isp=%08x msp=%08x s=%d m=%d\n",
					(void*)regs.pc_p, regs.pc, safe_pc,
					regs.regs[0], regs.regs[1], regs.regs[8], regs.regs[15],
					(unsigned)regs.sr, (unsigned)regs.spcflags, (void*)regs.pc_oldp,
					(unsigned)regs.isp, (unsigned)regs.msp, regs.s, regs.m);
			/* Check if the guest Mac address is in valid executable memory:
			   - RAM: 0 <= pc < RAMSize
			   - ROM: ROMBaseMac <= pc < ROMBaseMac + ROMSize
			   Anything else (NuBus space, frame buffer, unmapped) is a bus error. */
			bool valid_mac_pc = (safe_pc < (uae_u32)RAMSize) ||
				(safe_pc >= (uae_u32)ROMBaseMac && safe_pc < (uae_u32)(ROMBaseMac + ROMSize));
			if (!valid_mac_pc) {
				/* Guest PC points to unmapped memory (e.g. NuBus slot probe).
				   Generate a bus error exception to let the ROM's handler
				   deal with it, just like real hardware would. */
				static int buserr_count = 0;
				if (buserr_count++ < 10)
					fprintf(stderr, "JIT: bus error for unmapped PC=%08x a7=%08x isp=%08x (triggering Exception 2)\n",
						safe_pc, m68k_areg(regs, 7), regs.isp);
				/* Restore a7 from ISP/MSP — the JIT may have desynchronized them */
				if (regs.s && !regs.m && regs.isp >= 0x1000)
					m68k_areg(regs, 7) = regs.isp;
				else if (regs.s && regs.m && regs.msp >= 0x1000)
					m68k_areg(regs, 7) = regs.msp;
				Exception(2, safe_pc);
				return;
			}
			/* Valid Mac address — re-derive pc_p from the guest PC */
			regs.pc = safe_pc;
			regs.pc_p = get_real_address(safe_pc, 0, sz_word);
			regs.pc_oldp = regs.pc_p - safe_pc;
		}
	}
#endif
	if (!check_for_cache_miss()) {
		cpu_history pc_hist[MAXRUN];
		memset(pc_hist, 0, sizeof(pc_hist));
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
			uae_u32 trace_a1 = regs.regs[9];
			if (trace_a1 >= 0x1e00 && trace_a1 < 0x1e40)
				basilisk_trace_after_table_ready = true;
			static unsigned long pctrace_count = 0;
			static unsigned long pctrace_limit = 0;
			static bool pctrace_init = false;
			if (!pctrace_init) {
				const char *env = getenv("B2_JIT_PCTRACE");
				pctrace_limit = env ? strtoul(env, NULL, 10) : 0;
				pctrace_init = true;
			}
			uae_u32 pc = m68k_getpc();
			if (pctrace_limit && pctrace_count < pctrace_limit && jit_pctrace_match(pc)) {
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
				if (pctrace_stack) {
					uaecptr sp = m68k_areg(regs, 7);
					fprintf(stderr,
						"PCTSTACK %08x sm4=%08x s0=%08x s4=%08x s8=%08x\n",
						pc,
						(unsigned)get_long(sp - 4),
						(unsigned)get_long(sp + 0),
						(unsigned)get_long(sp + 4),
						(unsigned)get_long(sp + 8));
				}
				if (pctrace_mem) {
					uaecptr a0v = m68k_areg(regs, 0);
					uaecptr a3v = m68k_areg(regs, 3);
					fprintf(stderr,
						"PCTMEM %08x m1e4=%08x m1e8=%08x m20c=%08x ma0m4=%08x ma3=%08x ma3p4=%08x\n",
						pc,
						(unsigned)get_long(0x1e4),
						(unsigned)get_long(0x1e8),
						(unsigned)get_long(0x20c),
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
		for (;;) {
			pc_hist[blocklen++].location = (uae_u16 *)regs.pc_p;
			uae_u32 opcode = GET_OPCODE;
			(*cpufunctbl[opcode])(opcode);
			cpu_check_ticks();
			total_cycles += 4 * CYCLE_UNIT;
			int maxrun_limit = MAXRUN;
			{
				static int env_maxrun = -1;
				if (env_maxrun < 0) {
					const char *env = getenv("B2_JIT_MAXRUN");
					env_maxrun = (env && *env) ? atoi(env) : MAXRUN;
				}
				maxrun_limit = env_maxrun;
			}
			bool must_end = SPCFLAGS_TEST(SPCFLAG_ALL) || blocklen >= maxrun_limit;
			if (!must_end && end_block(opcode)) {
				uintptr new_pcp = (uintptr)regs.pc_p;
				uintptr blk_start = (uintptr)pc_hist[0].location;
				/* Continue past internal branches to build bigger blocks.
				   Cap at next multiple of 16 below MAXRUN for alignment. */
				int blk_limit = (maxrun_limit < 48 ? maxrun_limit : 48) & ~15;
				if (blk_limit < 16) blk_limit = 16;
				if (new_pcp >= blk_start && new_pcp < blk_start + 512
				    && blocklen < blk_limit)
					continue;
				must_end = true;
			}
			if (must_end) {
#if defined(CPU_AARCH64)
				tick_inhibit = false;
				uae_u32 block_pc = get_virtual_address((uae_u8*)pc_hist[0].location);
				if (verify_this_block) {
					jit_block_verify_run(pc_hist, blocklen, total_cycles, block_pc);
					return;
				}
#endif
				compile_block(pc_hist, blocklen, total_cycles);
				return;
			}
		}
	}
}

void execute_exception(uae_u32 cycles)
{
	countdown -= cycles;
	Exception(regs.jit_exception, 0);
	regs.jit_exception = 0;
}
