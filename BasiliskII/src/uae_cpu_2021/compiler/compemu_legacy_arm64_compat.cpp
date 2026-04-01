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

static inline int legacy_addr_with_offset(int base, uae_s32 offset)
{
	if (offset == 0)
		return base;
	if (offset > 0 && offset <= 4095) {
		const int tmp = (base == REG_WORK1) ? REG_WORK2 : REG_WORK1;
		ADD_xxi(tmp, base, offset);
		return tmp;
	}
	if (offset < 0 && offset >= -4095) {
		const int tmp = (base == REG_WORK1) ? REG_WORK2 : REG_WORK1;
		SUB_xxi(tmp, base, -offset);
		return tmp;
	}
	jit_abort("unsupported legacy host-pointer offset %d", offset);
	return base;
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
void add_l(RW4 d, RR4 s) { if (legacy_needflags_enabled()) jff_ADD_l(d, s); else jnf_ADD_l(d, s); }
void add_l_ri(RW4 d, uae_s32 i) { if (legacy_needflags_enabled()) jff_ADD_l_imm(d, i); else jnf_ADD_l_imm(d, i); }
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

void adc_b(RW1 d, RR1 s) { legacy_copy_carry_to_flagx(); if (legacy_needflags_enabled()) jff_ADDX_b(d, s); else jnf_ADDX_b(d, s); }
void adc_w(RW2 d, RR2 s) { legacy_copy_carry_to_flagx(); if (legacy_needflags_enabled()) jff_ADDX_w(d, s); else jnf_ADDX_w(d, s); }
void adc_l(RW4 d, RR4 s) { legacy_copy_carry_to_flagx(); if (legacy_needflags_enabled()) jff_ADDX_l(d, s); else jnf_ADDX_l(d, s); }
void sbb_b(RW1 d, RR1 s) { legacy_copy_carry_to_flagx(); if (legacy_needflags_enabled()) jff_SUBX_b(d, s); else jnf_SUBX_b(d, s); }
void sbb_w(RW2 d, RR2 s) { legacy_copy_carry_to_flagx(); if (legacy_needflags_enabled()) jff_SUBX_w(d, s); else jnf_SUBX_w(d, s); }
void sbb_l(RW4 d, RR4 s) { legacy_copy_carry_to_flagx(); if (legacy_needflags_enabled()) jff_SUBX_l(d, s); else jnf_SUBX_l(d, s); }

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
		const int addr = legacy_addr_with_offset(base, offset);
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
		const int addr = legacy_addr_with_offset(base, offset);
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
	jff_TST_l(r);
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
#endif
	/* Intentionally empty. */
}

void exec_nostats(void)
{
#if defined(CPU_AARCH64)
	jit_diag_exec_nostats_calls++;
	jit_diag_dispatch_count++;
	jit_diag_maybe_print();
#endif
	for (;;) {
		uae_u32 opcode = GET_OPCODE;
		(*cpufunctbl[opcode])(opcode);
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
#endif
	if (!check_for_cache_miss()) {
		cpu_history pc_hist[MAXRUN];
		memset(pc_hist, 0, sizeof(pc_hist));
		int blocklen = 0;
		int total_cycles = 0;
		start_pc_p = regs.pc_oldp;
		start_pc = regs.pc;
#if defined(CPU_AARCH64)
		/* Inhibit one_tick() during block tracing. The tick thread's
		   one_tick() has side effects (incrementing Ticks, SDL events)
		   that happen during interpreter tracing but NOT during native
		   block execution. This asymmetry causes different execution paths.
		   Inhibiting during tracing (typically <64 instructions, ~microseconds)
		   has negligible impact on 60Hz timing accuracy. */
		extern bool tick_inhibit;
		tick_inhibit = true;
#endif
		for (;;) {
			pc_hist[blocklen++].location = (uae_u16 *)regs.pc_p;
			uae_u32 opcode = GET_OPCODE;
			(*cpufunctbl[opcode])(opcode);
			cpu_check_ticks();
			total_cycles += 4 * CYCLE_UNIT;
			if (end_block(opcode) || SPCFLAGS_TEST(SPCFLAG_ALL) || blocklen >= MAXRUN) {
#if defined(CPU_AARCH64)
				tick_inhibit = false;
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
