#!/bin/bash
# BasiliskII AArch64 JIT Opcode Correctness Test
# Autoresearch harness: compare interpreter vs JIT register state for each opcode class
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UNIX_DIR="$(cd "$SCRIPT_DIR/../BasiliskII/src/Unix" && pwd)"
ROM="${B2_TEST_ROM:-/workspace/projects/rpi-basilisk2-sdl2-nox/Quadra800.ROM}"
DISK="${B2_TEST_DISK:-/workspace/fixtures/basilisk/images/HD200MB}"
RUN_DIR="$(mktemp -d /tmp/ar-jit-opcodes-XXXXXX)"

cleanup() {
    rm -rf "$RUN_DIR"
}
trap cleanup EXIT

emit_failure_metrics() {
    local build_ok="$1"
    local reason="$2"
    local infra_fail="${3:-$(( build_ok == 1 ? 0 : 1 ))}"
    echo "METRIC build_ok=$build_ok"
    echo "METRIC pass=0"
    echo "METRIC fail=0"
    echo "METRIC total=0"
    echo "METRIC infra_fail=$infra_fail"
    echo "METRIC fail_equiv=0"
    echo "METRIC infra_timeout=0"
    echo "METRIC infra_emu_exit=0"
    echo "METRIC infra_no_regdump=0"
    echo "METRIC infra_multi_regdump=0"
    echo "METRIC infra_sentinel=0"
    echo "METRIC infra_other=0"
    echo "METRIC risky_total=0"
    echo "METRIC risky_pass=0"
    echo "METRIC risky_fail=0"
    echo "METRIC risky_fail_equiv=0"
    echo "METRIC risky_infra_fail=0"
    echo "METRIC score=0"
    echo "$reason" >&2
    exit 0
}

# ---- Build -------------------------------------------------------------------
if ! cd "$UNIX_DIR"; then
    emit_failure_metrics 0 "missing Unix build directory: $UNIX_DIR"
fi
if [ ! -r "$ROM" ]; then
    emit_failure_metrics 0 "missing ROM: $ROM"
fi
if [ ! -r "$DISK" ]; then
    emit_failure_metrics 0 "missing disk image: $DISK"
fi

# Fresh git worktrees may contain a stale Makefile without config.h, or may be
# missing ./configure entirely until autogen.sh is run. Normalize that first.
if [ ! -x ./configure ] && [ -x ./autogen.sh ]; then
    NO_CONFIGURE=1 ./autogen.sh >"$RUN_DIR/autogen.log" 2>&1 || true
fi

if [ ! -f config.h ] || [ ! -f Makefile ]; then
    if [ ! -x ./configure ]; then
        tail -20 "$RUN_DIR/autogen.log" >&2 || true
        emit_failure_metrics 0 "missing ./configure after autogen"
    fi
    if ! ac_cv_have_asm_extended_signals=yes \
      ./configure --with-uae-core=2021 --enable-aarch64-jit-experimental --disable-vosf \
      >"$RUN_DIR/configure.log" 2>&1; then
        tail -20 "$RUN_DIR/configure.log" >&2 || true
        emit_failure_metrics 0 "configure failed"
    fi
fi

if ! make -j12 >"$RUN_DIR/build.log" 2>&1; then
    tail -20 "$RUN_DIR/build.log" >&2 || true
    emit_failure_metrics 0 "build failed"
fi
echo "METRIC build_ok=1"

# ---- Test harness ------------------------------------------------------------
# Each test case is a hex sequence of M68K instructions.
# The emulator runs until it hits STOP #0x2700 (4e72 2700), then dumps registers.
# We run in both interpreter (jit false) and JIT (jit true) and compare.

# Register dump is extracted from stderr: lines matching "REGDUMP D0=..."
# We inject EMUL_OP_DUMP_REGS (if present) or parse from DC log at STOP.

run_test() {
    local name="$1"
    local hex_code="$2"      # hex M68K bytecode, space-separated words
    local use_jit="$3"       # "true" or "false"
    local sentinel_a6="$4"   # 8-hex-digit value expected in A6
    local outfile="$5"
    local init_regs="${6:-}"  # optional: D0-D7 A0-A7 [SR] space-separated hex

    local td="$RUN_DIR/test-${name}-jit${use_jit}"
    local reason_file="${outfile}.reason"
    mkdir -p "$td"
    echo "ok" > "$reason_file"

    # Append a non-CCR-clobbering sentinel write (MOVEA.L #imm, A6).
    # Opcode: 2C7C <imm_hi16> <imm_lo16>
    local full_hex="$hex_code 2C7C ${sentinel_a6:0:4} ${sentinel_a6:4:4}"

    # Write prefs
    cat > "$td/prefs" <<EOF
rom $ROM
disk $DISK
ramsize 8388608
modelid 14
cpu 4
fpu false
jit $use_jit
jitfpu false
jitcachesize 8192
screen win/640/480
nosound true
nocdrom true
ignoresegv true
EOF

    # Ensure stale emulator processes from prior tests do not survive.
    pkill -f '/tmp/ar-jit-opcodes-.*BasiliskII' 2>/dev/null || true
    pkill -f "$UNIX_DIR/BasiliskII --config $td/prefs" 2>/dev/null || true

    # Hard timeout: terminate after 30s, kill after 5s grace.
    # Some BasiliskII runs ignore TERM or survive in a separate process group,
    # so follow with a targeted pkill sweep.
    local emu_rc=0
    local -a env_vars=(
        SDL_VIDEODRIVER=x11
        DISPLAY=:99
        HOME="$td"
        B2_TEST_HEX="$full_hex"
        B2_TEST_DUMP=1
    )
    if [ "$use_jit" = "true" ]; then
        env_vars+=(B2_JIT_FORCE_TRANSLATE=1)
    fi
    if [ -n "$init_regs" ]; then
        env_vars+=(B2_TEST_INIT="$init_regs")
    fi
    if ! env "${env_vars[@]}" \
      setarch $(uname -m) -R \
      timeout -k 5s 30s "$UNIX_DIR/BasiliskII" --config "$td/prefs" \
      > "$td/emu.log" 2>&1; then
        emu_rc=$?
    fi

    pkill -f "$UNIX_DIR/BasiliskII --config $td/prefs" 2>/dev/null || true
    sleep 0.2

    if [ "$emu_rc" -eq 124 ] || [ "$emu_rc" -eq 137 ]; then
        echo "timeout" > "$reason_file"
        echo "INFRA $name jit=$use_jit: timeout (rc=$emu_rc)" >&2
        return 1
    fi
    if [ "$emu_rc" -ne 0 ]; then
        echo "emu_exit_$emu_rc" > "$reason_file"
        echo "INFRA $name jit=$use_jit: emulator exited non-zero (rc=$emu_rc)" >&2
        return 1
    fi

    local dump_count
    dump_count=$(grep -c "^REGDUMP:" "$td/emu.log" 2>/dev/null || true)
    if [ "$dump_count" -eq 0 ]; then
        echo "no_regdump" > "$reason_file"
        echo "INFRA $name jit=$use_jit: missing REGDUMP" >&2
        return 1
    fi
    if [ "$dump_count" -gt 1 ]; then
        echo "multi_regdump" > "$reason_file"
        echo "INFRA $name jit=$use_jit: expected 1 REGDUMP, got $dump_count" >&2
        return 1
    fi

    grep "^REGDUMP:" "$td/emu.log" > "$outfile"
    if ! grep -qi "A6=$sentinel_a6" "$outfile"; then
        echo "sentinel_mismatch" > "$reason_file"
        echo "INFRA $name jit=$use_jit: sentinel A6 mismatch (expected $sentinel_a6)" >&2
        return 1
    fi

    return 0
}

# Start Xvfb if needed
if ! pgrep -x Xvfb >/dev/null 2>&1; then
    Xvfb :99 -screen 0 640x480x24 &>/dev/null &
    sleep 1
fi

# ---- Define test cases -------------------------------------------------------
# Format: name|hex_words (M68K big-endian, STOP #0x2700 appended automatically)
# Each test sets up known state and exercises one opcode class.

declare -a TEST_ORDER=(nop move moveq_signext alu alu_overflow addi_subi_long addi_subi_long_wrap addi_subi_word addi_subi_word_wrap addi_subi_byte addi_subi_byte_wrap shift bitops bitops_chg bitops_highbit bitops_chg_highbit branch branch_chain compare compare_negative cmpi_sizes cmpi_sizes_zero cmpi_byte_negative cmpi_word_negative cmpi_long_negative cmpi_beq_taken muldiv movem misc clr_sizes clr_byte_preserve_upper clr_word_preserve_upper neg_sizes neg_zero_sizes swap_roundtrip flags flags_eori_ccr exg exg_roundtrip imm_logic imm_logic_alt imm_logic_byte_highbit imm_logic_word imm_logic_long imm_logic_long_alt tst_sizes tst_zero tst_positive bra_taken bra_w_taken bne_not_taken bne_taken bne_w_not_taken bne_w_taken beq_taken beq_not_taken beq_w_taken beq_w_not_taken bpl_taken bpl_not_taken bpl_w_taken bpl_w_not_taken bmi_taken bmi_not_taken bmi_w_taken bmi_w_not_taken bvc_taken bvc_not_taken_overflow bvc_w_taken bvc_w_not_taken_overflow bvs_taken_overflow bvs_not_taken bvs_w_taken_overflow bvs_w_not_taken bge_taken bge_not_taken bge_w_taken bge_w_not_taken blt_taken blt_not_taken blt_w_taken blt_w_not_taken bgt_taken bgt_not_taken bgt_w_taken bgt_w_not_taken ble_taken ble_not_taken ble_w_taken ble_w_not_taken bcc_taken bcc_not_taken bcc_w_taken bcc_w_not_taken bcs_taken bcs_not_taken bcs_w_taken bcs_w_not_taken bhi_taken bhi_not_taken bhi_w_taken bhi_w_not_taken bls_taken bls_not_taken bls_w_taken bls_w_not_taken scc_basic scc_eq_ne scc_carry scc_hi_ls scc_hi_ls_z scc_vc_vs scc_pl_mi scc_ge_lt scc_gt_le scc_ccr_preserve_blt scc_ccr_preserve_bcs scc_ccr_preserve_bne_not_taken scc_ccr_preserve_beq_taken quick_ops quick_ops_long_neg_roundtrip quick_ops_word quick_ops_word_wrap quick_ops_long_wrap quick_ops_byte quick_ops_byte_wrap quick_ops_addr dbra dbra_not_taken dbra_start_minus1_branch dbra_start_8000_branch dbt_true_not_taken dbra_three_iter dbcc_loop_c_set dbcs_not_taken_c_set dbpl_loop_n_set dbmi_not_taken_n_set dbhi_not_taken_hi_set dbls_not_taken_ls_set dbge_not_taken_n_eq_v dblt_not_taken_n_ne_v dbgt_not_taken_gt_set dble_not_taken_le_set dbhi_false_dec_terminal_ls_set dbls_false_dec_terminal_hi_set dbge_false_dec_terminal_n_ne_v dblt_false_dec_terminal_n_eq_v dbgt_false_dec_terminal_z_set dble_false_dec_terminal_gt_set dbcc_ccr_preserve_beq_taken dbcc_ccr_preserve_bne_taken dbcc_ccr_preserve_bcs_taken dbcc_ccr_preserve_bvc_taken dbcc_ccr_preserve_bvs_taken dbcc_ccr_preserve_bhi_taken dbcc_ccr_preserve_bls_taken dbcc_ccr_preserve_bge_taken dbcc_ccr_preserve_blt_taken dbcc_ccr_preserve_bgt_taken dbcc_ccr_preserve_ble_taken dbvc_loop_v_set dbvs_loop_v_clear dbvc_not_taken_v_clear dbvs_not_taken_v_set dbne_loop_z_set dbeq_loop_z_clear moveq_edges alu_negative_roundtrip imm_logic_word_highbit branch_chain_z_clear branch_chain_carry_set branch_chain_overflow_set scc_ccr_preserve_bvs_taken dbra_four_iter scc_ccr_preserve_bvc_taken scc_ccr_preserve_bhi_taken scc_ccr_preserve_bls_taken dbra_five_iter branch_chain_eq_then_ne branch_chain_carry_clear imm_logic_long_highbit dbra_six_iter not_sizes not_word_preserve_upper not_byte_preserve_upper scc_ccr_preserve_bpl_taken scc_ccr_preserve_bmi_taken scc_ccr_preserve_bge_taken scc_ccr_preserve_bgt_taken scc_ccr_preserve_ble_taken nop_triplet roxl_x_propagation roxr_x_propagation roxl_count_2 asl_overflow lsr_count_32 asr_count_0 ror_word rol_word btst_reg_high_bit muls_neg_neg muls_zero divs_neg_neg divs_overflow abcd_basic sbcd_basic negx_with_x negx_zero addx_basic subx_basic ext_word ext_long move_to_mem_and_back movem_predec_postinc movem_predec_mixed_order addx_chain flag_chain_xzn shift_chain roxl_reg_count_32 roxl_reg_count_33 roxr_reg_count_33 roxr_reg_count_32 roxr_reg_count_0 roxl_reg_count_63 roxr_reg_count_63 roxr_roxl_chain_x roxl_lsr_chain_x mulu_large divu_remainder abcd_with_carry nbcd_basic bsr_rts link_unlk indexed_addr_mode byte_postinc cmpm_equal move_sr_roundtrip dbra_loop_100 dbne_loop_cmpi bsr_in_dbra_loop table_lookup dbra_loop_1000 swap_pack lea_scaled_index multi_branch andi_l_dn eor_self asl_w_vflag asl_b_overflow lsr_w_regcount asr_w_preserve movem_w_signext cmpm_l_equal cmpm_b_unequal addx_64bit subx_64bit muls_boundary divu_max_quotient move_b_preserve_flags byte_logic_chain bchg_imm_high neg_w_partial clr_b_tst all_regs_alive scaled_index_word byte_indexed_load indexed_store_load addq_subq_sizes x_flag_chain sub_w_subx_chain exg_dn_an push_pop_a0 dbeq_loop_50 dbmi_loop_neg lsl_l_count0 asr_l_8_neg rol_l_16 lsl_b_7 asr_b_1_sign move_b_flags move_w_zero add_l_an_dn sub_w_dn_an cmp_b cmp_w ori_w_mem andi_b_mem link_neg16 mulu_max divs_neg_rem negx_64bit cmpi_l_abs_short_eq cmpi_l_abs_short_ne cmpi_bne_w_not_taken cmpi_bne_w_taken cmpi_b_abs_short_blt movem_save_modify_restore bsr_l_long jmp_d8_pc_dn_w pea_movem_stack subq_sp_movea_write tst_bne_after_bsr_rts tst_bne_after_jsr_an save_clear_slot_restore_tst movec_cacr_roundtrip cache_init_sequence move_l_neg_disp_a5 sr_barrier_cache_init divs_word_hardfail divu_word_hardfail mull_32_hardfail divl_32_hardfail aslw_mem_hardfail lsrw_mem_hardfail rolw_mem_hardfail ori_sr_hardfail andi_sr_hardfail eori_sr_hardfail move_from_sr_hardfail move_to_sr_hardfail divs_neg_by_neg_edge divs_by_minus_one_edge divs_zero_dividend_edge divs_overflow_edge divu_exact_edge divu_with_remainder_edge divu_overflow_edge mull_unsigned_32 mull_signed_32 divl_unsigned_32 divl_signed_32 asrw_mem_edge roxlw_mem_edge roxrw_mem_edge abcd_99_plus_01_edge sbcd_with_x_edge nbcd_99_edge bfextu_reg_edge bfexts_reg_edge bfffo_reg_edge bfset_reg_edge bfclr_reg_edge bfchg_reg_edge bftst_reg_edge bfins_reg_edge pack_dn_edge unpk_dn_edge movep_l_roundtrip sr_ops_combo moves_write_read adda_w_cov adda_l_cov adda_w_neg_cov eori_ccr_cov rtr_cov mvr2usp_cov move_b_d16_an_cov move_w_d16_an_cov move_l_d16_an_cov move_b_idx_cov move_l_idx_scale_cov move_l_pc_rel_cov move_l_abs_w_cov move_l_abs_l_cov predec_postinc_cov imm_to_mem_b_cov imm_to_mem_w_cov imm_to_mem_l_cov add_b_overflow_cov sub_w_borrow_cov cmp_l_equal_cov and_l_zero_cov or_l_allones_cov eor_self_cov neg_b_overflow_cov not_b_cov odd_addr_cov a7_byte_postinc_cov fuzz_alu_0 fuzz_shift_0 fuzz_bitops_0 fuzz_muldiv_0 fuzz_extswap_0 fuzz_addxsubx_0 fuzz_memrt_0 fuzz_exg_0 fuzz_mixed_0 fuzz_flags_0 fuzz_alu_1 fuzz_shift_1 fuzz_bitops_1 fuzz_muldiv_1 fuzz_extswap_1 fuzz_addxsubx_1 fuzz_memrt_1 fuzz_exg_1 fuzz_mixed_1 fuzz_flags_1 fuzz_alu_2 fuzz_shift_2 fuzz_bitops_2 fuzz_muldiv_2 fuzz_extswap_2 fuzz_addxsubx_2 fuzz_memrt_2 fuzz_exg_2 fuzz_mixed_2 fuzz_flags_2 fuzz_alu_3 fuzz_shift_3 fuzz_bitops_3 fuzz_muldiv_3 fuzz_extswap_3 fuzz_addxsubx_3 fuzz_memrt_3 fuzz_exg_3 fuzz_mixed_3 fuzz_flags_3 fuzz_alu_4 fuzz_shift_4 fuzz_bitops_4 fuzz_muldiv_4 fuzz_extswap_4 fuzz_addxsubx_4 fuzz_memrt_4 fuzz_exg_4 fuzz_mixed_4 fuzz_flags_4 chk_w_in_range chk_w_zero chk_w_equal sbcd_borrow_chain sbcd_zero_zero nbcd_zero_no_x nbcd_with_x bfins_low8 bfins_mid8 movec_vbr_roundtrip movec_sfc_roundtrip movec_dfc_roundtrip mull_u64 mull_s32_neg divl_u32_rem divl_s32_neg divl_u32_max divl_s32_neg_divisor mull_s64_neg divl_same_dq_dr divl_u64 divl_s64 bfins_dreg_imm bfins_dreg_narrow)
declare -A TESTS
# NOP: trivial decode/execute path sanity check
TESTS[nop]="4E71 4E71"
# NOP_TRIPLET: additional decode/dispatch stream-length sanity for repeated NOPs
TESTS[nop_triplet]="4E71 4E71 4E71"
# --- HIGH-RISK OPCODE VECTORS ---
# ROXL_X_PROPAGATION: ORI #0x10,CCR (set X); MOVEQ #1,D0; ROXL.L #1,D0
# X=1 rotates into bit 0, so D0 should become 3, and X/C reflect bit 31 (was 0)
# ORI.B #imm,CCR = 003C 0010; MOVEQ #1,D0 = 7001; ROXL.L #1,D0 = E390
TESTS[roxl_x_propagation]="003C 0010 7001 E390"
# ROXR_X_PROPAGATION: ORI #0x10,CCR (set X); MOVEQ #2,D0; ROXR.L #1,D0
# X=1 rotates into bit 31, so D0=0x80000001, X/C reflect old bit 0 (was 0)
# ORI.B #imm,CCR = 003C 0010; MOVEQ #2,D0 = 7002; ROXR.L #1,D0 = E290
TESTS[roxr_x_propagation]="003C 0010 7002 E290"
# ROXL_COUNT_2: ORI #0x10,CCR (set X); MOVEQ #3,D0; ROXL.L #2,D0
# Rotate left by 2 through X: bit pattern exercise
# ROXL.L #2,D0 = E590
TESTS[roxl_count_2]="003C 0010 7003 E590"
# ASL_OVERFLOW: MOVEQ #0x40,D0; SWAP D0 (D0=0x00400000...wait)
# Actually: MOVE.L #0x40000000,D0; ASL.L #1,D0 → should set V=1
# MOVE.L #imm,D0 = 203C 4000 0000; ASL.L #1,D0 = E380
TESTS[asl_overflow]="203C 4000 0000 E380"
# LSR_COUNT_32: MOVEQ #-1,D0 (0xFFFFFFFF); MOVEQ #32,D1 (0x20); LSR.L D1,D0
# Shift count=32 for .L → D0 should become 0, C=MSB of original
# MOVEQ #-1,D0 = 70FF; MOVEQ #32,D1 = 7220; LSR.L D1,D0 = E2A8
TESTS[lsr_count_32]="70FF 7220 E2A8"
# ASR_COUNT_0: MOVEQ #-1,D0; MOVEQ #0,D1; ASR.L D1,D0
# Shift count=0 → D0 unchanged, C cleared
# MOVEQ #-1,D0 = 70FF; MOVEQ #0,D1 = 7200; ASR.L D1,D0 = E2A0
TESTS[asr_count_0]="70FF 7200 E2A0"
# ROR_WORD: MOVE.L #0x00010000,D0; ROR.W #1,D0
# ROR.W operates on low word only; upper word preserved
# MOVE.L #0x00010000,D0 = 203C 0001 0000; ROR.W #1,D0 = E258
TESTS[ror_word]="203C 0001 0000 E258"
# ROL_WORD: MOVE.L #0xFFFF8001,D0; ROL.W #1,D0
# ROL.W on low word 0x8001 → 0x0003, upper word 0xFFFF preserved
# MOVE.L #0xFFFF8001,D0 = 203C FFFF 8001; ROL.W #1,D0 = E358
TESTS[rol_word]="203C FFFF 8001 E358"
# BTST_REG_HIGH_BIT: MOVEQ #31,D1; MOVE.L #0x80000000,D0; BTST D1,D0
# Register BTST uses bit mod 32, so bit 31 should test set → Z=0
# MOVEQ #31,D1 = 721F; MOVE.L #0x80000000,D0 = 203C 8000 0000; BTST D1,D0 = 0300
TESTS[btst_reg_high_bit]="721F 203C 8000 0000 0300"
# MULS_NEG_NEG: MOVEQ #-3,D0 (0xFFFFFFFD); MOVEQ #-5,D1 (0xFFFFFFFB); MULS D1,D0
# (-3)*(-5) = 15, result in D0.L
# MOVEQ #-3,D0 = 70FD; MOVEQ #-5,D1 = 72FB; MULS D1,D0 = C1C1
TESTS[muls_neg_neg]="70FD 72FB C1C1"
# MULS_ZERO: MOVEQ #0,D0; MOVEQ #-1,D1; MULS D1,D0
# 0 * anything = 0, Z=1, N=0
TESTS[muls_zero]="7000 72FF C1C1"
# DIVS_NEG_NEG: MOVE.L #0xFFFFFFF1,D0 (-15); MOVEQ #-3,D1; DIVS D1,D0
# -15 / -3 = quotient 5, remainder 0
# MOVE.L #0xFFFFFFF1,D0 = 203C FFFF FFF1; MOVEQ #-3,D1 = 72FD; DIVS D1,D0 = 81C1
TESTS[divs_neg_neg]="203C FFFF FFF1 72FD 81C1"
# DIVS_OVERFLOW: MOVE.L #0x00010000,D0 (65536); MOVEQ #1,D1; DIVS D1,D0
# 65536/1 = 65536 which doesn't fit in 16-bit quotient → V=1, operands unchanged
# MOVE.L #0x00010000,D0 = 203C 0001 0000; MOVEQ #1,D1 = 7201; DIVS D1,D0 = 81C1
TESTS[divs_overflow]="203C 0001 0000 7201 81C1"
# ABCD_BASIC: MOVEQ #0x09,D0; MOVEQ #0x09,D1; ABCD D1,D0
# BCD: 09+09=18 → D0.B=0x18
# MOVEQ #9,D0 = 7009; MOVEQ #9,D1 = 7209; ABCD D1,D0 = C101
TESTS[abcd_basic]="7009 7209 C101"
# SBCD_BASIC: MOVEQ #0x18,D0; MOVEQ #0x09,D1; SBCD D1,D0
# BCD: 18-09=09 → D0.B=0x09
# MOVEQ #0x18,D0 = 7018; MOVEQ #9,D1 = 7209; SBCD D1,D0 = 8101
TESTS[sbcd_basic]="7018 7209 8101"
# NEGX_WITH_X: ORI #0x10,CCR (set X); MOVEQ #5,D0; NEGX.L D0
# NEGX = 0 - D0 - X = 0 - 5 - 1 = -6 = 0xFFFFFFFA
# ORI.B #imm,CCR = 003C 0010; MOVEQ #5,D0 = 7005; NEGX.L D0 = 4080
TESTS[negx_with_x]="003C 0010 7005 4080"
# NEGX_ZERO: MOVEQ #0,D0; NEGX.L D0 (with X clear)
# NEGX of 0 with X=0 → result 0, but Z is only cleared if result≠0 (unchanged here)
# ANDI #0xEF,CCR clears X; MOVEQ #0,D0; NEGX.L D0
# ANDI.B #imm,CCR = 023C 00EF; MOVEQ #0,D0 = 7000; NEGX.L D0 = 4080
TESTS[negx_zero]="023C 00EF 7000 4080"
# ADDX_BASIC: ORI #0x10,CCR (set X); MOVEQ #5,D0; MOVEQ #3,D1; ADDX.L D1,D0
# 5 + 3 + X(1) = 9
# ORI.B #0x10,CCR = 003C 0010; MOVEQ #5,D0 = 7005; MOVEQ #3,D1 = 7203; ADDX.L D1,D0 = D181
TESTS[addx_basic]="003C 0010 7005 7203 D181"
# SUBX_BASIC: ORI #0x10,CCR (set X); MOVEQ #10,D0; MOVEQ #3,D1; SUBX.L D1,D0
# 10 - 3 - X(1) = 6
# ORI.B #0x10,CCR = 003C 0010; MOVEQ #10,D0 = 700A; MOVEQ #3,D1 = 7203; SUBX.L D1,D0 = 9181
TESTS[subx_basic]="003C 0010 700A 7203 9181"
# EXT_WORD: MOVEQ #-1,D0 (0xFF in low byte); EXT.W D0
# EXT.W sign-extends byte to word: 0xFF → 0xFFFF in low word, upper word cleared by MOVEQ
# MOVEQ #-1,D0 = 70FF; EXT.W D0 = 4880
TESTS[ext_word]="70FF 4880"
# EXT_LONG: MOVE.L #0x0000FF80,D0; EXT.W D0; EXT.L D0
# EXT.W: byte 0x80 → word 0xFF80; EXT.L: word 0xFF80 → long 0xFFFFFF80
# MOVE.L #0x0000FF80,D0 = 203C 0000 FF80; EXT.W D0 = 4880; EXT.L D0 = 48C0
TESTS[ext_long]="203C 0000 FF80 4880 48C0"
# --- MEMORY-INDIRECT AND REGISTER-PRESSURE VECTORS ---
# MOVE_TO_MEM_AND_BACK: LEA $2000,A0; MOVE.L #$DEADBEEF,D0; MOVE.L D0,(A0); CLR.L D0; MOVE.L (A0),D1
# Tests basic memory store/load via register indirect
# LEA $2000,A0 = 41F9 0000 2000; MOVE.L #$DEADBEEF,D0 = 203C DEAD BEEF;
# MOVE.L D0,(A0) = 2080; CLR.L D0 = 4280; MOVE.L (A0),D1 = 2210
TESTS[move_to_mem_and_back]="41F9 0000 2000 203C DEAD BEEF 2080 4280 2210"
# MOVEM_PREDEC_POSTINC: set D0-D3, MOVEM.L D0-D3,-(A0), clear regs, MOVEM.L (A0)+,D4-D7
# LEA $3000,A0; MOVEQ #1,D0; MOVEQ #2,D1; MOVEQ #3,D2; MOVEQ #4,D3;
# MOVEM.L D0-D3,-(A0); MOVEQ #0,D0; MOVEQ #0,D1; MOVEQ #0,D2; MOVEQ #0,D3;
# MOVEM.L (A0)+,D4-D7
# LEA $3000,A0 = 41F9 0000 3000
# MOVEM.L D0-D3,-(A0) = 48E0 F000 (mask: D0-D3 reversed for predec = bits 15-12)
# Wait - MOVEM predecrement reverses the register mask. D0-D3 = bits 0-3 in normal,
# but predecrement uses reversed bit ordering: bit 15=D0, bit 14=D1, etc.
# Actually: MOVEM.L reg-list,-(An): register mask is normal (D0=bit0..A7=bit15),
# but registers are stored in reverse order (A7 first, D0 last). The mask itself
# for D0-D3 is 0x000F. But wait, for -(An) the mask encoding reverses:
# bit 0=A7, bit 1=A6, ..., bit 8=D7, ..., bit 15=D0
# So D0-D3 in predec mask: D0=bit15, D1=bit14, D2=bit13, D3=bit12 = 0xF000
# MOVEM.L (A0)+,D4-D7: normal mask, D4=bit4..D7=bit7 = 0x00F0
# 48E0 F000 = MOVEM.L D0-D3,-(A0)
# 4CD8 00F0 = MOVEM.L (A0)+,D4-D7
TESTS[movem_predec_postinc]="41F9 0000 3000 7001 7202 7403 7604 48E0 F000 7000 7200 7400 7600 4CD8 00F0"
# MOVEM_PREDEC_MIXED_ORDER: mixed D/A mask through predecrement + postincrement restore path
# LEA $3000,A0; D0=0x11111111; D1=0x22222222; A1=$3333;
# MOVEM.L D0/D1/A1,-(A0) with reversed predec mask 0xC040;
# clear D2/D3/A2; MOVEM.L (A0)+,D2/D3/A2 (mask 0x040C)
TESTS[movem_predec_mixed_order]="41F9 0000 3000 203C 1111 1111 223C 2222 2222 43F9 0000 3333 48E0 C040 243C 0000 0000 263C 0000 0000 247C 0000 0000 4CD8 040C"
# ADDX_CHAIN: multi-precision add: set X, then chain ADDX across D0+D2, D1+D3
# ORI #$10,CCR; MOVE.L #$FFFFFFFF,D0; MOVEQ #1,D2; ADDX.L D2,D0;
# MOVE.L #$00000000,D1; MOVEQ #0,D3; ADDX.L D3,D1
# This tests X propagation through a chain: D0 overflows, X should propagate to D1 add
# ORI.B #$10,CCR = 003C 0010
# MOVE.L #$FFFFFFFF,D0 = 203C FFFF FFFF; MOVEQ #1,D2 = 7401; ADDX.L D2,D0 = D182
# MOVE.L #0,D1 = 223C 0000 0000; MOVEQ #0,D3 = 7600; ADDX.L D3,D1 = D383
TESTS[addx_chain]="003C 0010 203C FFFF FFFF 7401 D182 223C 0000 0000 7600 D383"
# FLAG_CHAIN_XZN: exercise X/Z/N flag interaction across a sequence
# MOVEQ #-1,D0; ADD.L D0,D0 (should set X=1,C=1,N=1,Z=0,V=0 for 0xFFFFFFFE+carry)
# Wait: ADD.L D0,D0 = D0 + D0 = 0xFFFFFFFF + 0xFFFFFFFF = 0xFFFFFFFE, C=1, X=1
# Then NEGX.L D0: -(0xFFFFFFFE) - X(1) = 0x00000001
# Then ADDX.L D0,D0 with X from NEGX
# MOVEQ #-1,D0 = 70FF; ADD.L D0,D0 = D080; NEGX.L D0 = 4080; ADDX.L D0,D0 = D180
TESTS[flag_chain_xzn]="70FF D080 4080 D180"
# SHIFT_CHAIN: LSL then ROL with count from register, exercising C/X propagation
# MOVEQ #1,D0; MOVEQ #31,D1; LSL.L D1,D0 (D0=0x80000000, C=0, X=0);
# MOVEQ #1,D2; ROL.L D2,D0 (D0=0x00000001, C=1)
# MOVEQ #1,D0 = 7001; MOVEQ #31,D1 = 721F; LSL.L D1,D0 = E3A8
# MOVEQ #1,D2 = 7401; ROL.L D2,D0 = E5B8
TESTS[shift_chain]="7001 721F E3A8 7401 E5B8"
# ROXL_REG_COUNT_32: ORI #$10,CCR (set X); MOVEQ #1,D0; MOVEQ #32,D1; ROXL.L D1,D0
# Explicitly stress the 32-edge behavior for rotate-left-through-extend.
# ORI.B #$10,CCR = 003C 0010; MOVEQ #1,D0 = 7001; MOVEQ #32,D1 = 7220; ROXL.L D1,D0 = E3B0
TESTS[roxl_reg_count_32]="003C 0010 7001 7220 E3B0"
# ROXL_REG_COUNT_33: ORI #$10,CCR (set X); MOVE.L #$80000001,D0; MOVEQ #33,D1; ROXL.L D1,D0
# Exercises 33-bit ring wrap with both endpoint bits set plus X carry-in.
# ORI.B #$10,CCR = 003C 0010; MOVE.L #$80000001,D0 = 203C 8000 0001; MOVEQ #33,D1 = 7221; ROXL.L D1,D0 = E3B0
TESTS[roxl_reg_count_33]="003C 0010 203C 8000 0001 7221 E3B0"
# ROXR_REG_COUNT_33: ORI #$10,CCR (set X); MOVEQ #1,D0; MOVEQ #33,D1; ROXR.L D1,D0
# Exercises register-count masking/modulo behavior across 32+ edge with X/C propagation.
# ORI.B #$10,CCR = 003C 0010; MOVEQ #1,D0 = 7001; MOVEQ #33,D1 = 7221; ROXR.L D1,D0 = E2B0
TESTS[roxr_reg_count_33]="003C 0010 7001 7221 E2B0"
# ROXR_REG_COUNT_32: ORI #$10,CCR (set X); MOVEQ #1,D0; MOVEQ #32,D1; ROXR.L D1,D0
# Explicitly stress the 32-edge behavior in the 33-bit rotate-through-extend ring.
# ORI.B #$10,CCR = 003C 0010; MOVEQ #1,D0 = 7001; MOVEQ #32,D1 = 7220; ROXR.L D1,D0 = E2B0
TESTS[roxr_reg_count_32]="003C 0010 7001 7220 E2B0"
# ROXR_REG_COUNT_0: ORI #$10,CCR (set X); MOVE.L #$12345678,D0; MOVEQ #0,D1; ROXR.L D1,D0
# Count=0 semantics are special (no data rotation, flag handling edge).
# ORI.B #$10,CCR = 003C 0010; MOVE.L #$12345678,D0 = 203C 1234 5678; MOVEQ #0,D1 = 7200; ROXR.L D1,D0 = E2B0
TESTS[roxr_reg_count_0]="003C 0010 203C 1234 5678 7200 E2B0"
# ROXL_REG_COUNT_63: ORI #$10,CCR (set X); MOVE.L #$A5A55A5A,D0; MOVEQ #63,D1; ROXL.L D1,D0
# Stresses masked high register-count behavior near the 6-bit limit.
# ORI.B #$10,CCR = 003C 0010; MOVE.L #$A5A55A5A,D0 = 203C A5A5 5A5A; MOVEQ #63,D1 = 723F; ROXL.L D1,D0 = E3B0
TESTS[roxl_reg_count_63]="003C 0010 203C A5A5 5A5A 723F E3B0"
# ROXR_REG_COUNT_63: ORI #$10,CCR (set X); MOVE.L #$5A5AA5A5,D0; MOVEQ #63,D1; ROXR.L D1,D0
# Companion masked-high-count stress for right rotate-through-extend.
# ORI.B #$10,CCR = 003C 0010; MOVE.L #$5A5AA5A5,D0 = 203C 5A5A A5A5; MOVEQ #63,D1 = 723F; ROXR.L D1,D0 = E2B0
TESTS[roxr_reg_count_63]="003C 0010 203C 5A5A A5A5 723F E2B0"
# ROXR_ROXL_CHAIN_X: ORI #$10,CCR; MOVE.L #1,D0; MOVEQ #1,D1; ROXR.L D1,D0; ROXL.L D1,D0
# Chained opposite-direction rotate-through-X operations stress carry/extend handoff.
# ORI.B #$10,CCR = 003C 0010; MOVE.L #1,D0 = 203C 0000 0001; MOVEQ #1,D1 = 7201; ROXR.L D1,D0 = E2B0; ROXL.L D1,D0 = E3B0
TESTS[roxr_roxl_chain_x]="003C 0010 203C 0000 0001 7201 E2B0 E3B0"
# ROXL_LSR_CHAIN_X: ORI #$10,CCR; MOVE.L #$80000001,D0; MOVEQ #1,D1; ROXL.L D1,D0; LSR.L D1,D0
# Mixed rotate+shift chain stresses X/C handoff into logical shifts.
# ORI.B #$10,CCR = 003C 0010; MOVE.L #$80000001,D0 = 203C 8000 0001; MOVEQ #1,D1 = 7201; ROXL.L D1,D0 = E3B0; LSR.L D1,D0 = E2A8
TESTS[roxl_lsr_chain_x]="003C 0010 203C 8000 0001 7201 E3B0 E2A8"
# MULU_LARGE: MOVE.L #$FFFF,D0; MOVE.L #$FFFF,D1; MULU D1,D0
# 0xFFFF * 0xFFFF = 0xFFFE0001 — tests large unsigned multiply result
# MOVE.L #$FFFF,D0 = 203C 0000 FFFF; MOVE.L #$FFFF,D1 = 223C 0000 FFFF; MULU D1,D0 = C0C1
TESTS[mulu_large]="203C 0000 FFFF 223C 0000 FFFF C0C1"
# DIVU_REMAINDER: MOVE.L #$00070005,D0; MOVEQ #3,D1; DIVU D1,D0
# 0x70005 = 458757; 458757/3 = quotient 152919 (too large for 16 bits? No: 152919 > 65535 → overflow)
# Let me use a smaller dividend: MOVE.L #$00030005,D0; MOVEQ #2,D1; DIVU D1,D0
# 0x30005 = 196613; 196613/2 = 98306 > 65535 → overflow. Let me think...
# MOVE.L #$0001FFFF,D0; MOVEQ #2,D1; DIVU D1,D0
# 0x1FFFF = 131071; 131071/2 = quotient 65535 remainder 1
# Result: D0 = (rem << 16) | quot = 0x0001FFFF
# MOVE.L #$0001FFFF,D0 = 203C 0001 FFFF; MOVEQ #2,D1 = 7202; DIVU D1,D0 = 80C1
TESTS[divu_remainder]="203C 0001 FFFF 7202 80C1"
# ABCD_WITH_CARRY: test ABCD with X flag set
# ORI #$10,CCR; MOVEQ #$99,D0; MOVEQ #$01,D1; ABCD D1,D0
# BCD: 99+01+X(1) = 01 with carry (X=1, C=1 after)
# Wait: 0x99 via MOVEQ is sign-extended: MOVEQ #$99 doesn't work (>127 signed).
# Use: MOVE.L #$99,D0 = 203C 0000 0099; but MOVEQ #-103 = 0x99...no.
# MOVEQ range is -128 to 127, so 0x99 = 153 is out of range.
# Use MOVE.B #$99,D0 — but that's not a single simple encoding. 
# Better: MOVEQ #0,D0; ORI.B #$99,D0
# ORI.B #$99,D0 = 0000 0099
# Full: ORI #$10,CCR; MOVEQ #0,D0; ORI.B #$99,D0; MOVEQ #1,D1; ABCD D1,D0
# 003C 0010 7000 0000 0099 7201 C101
TESTS[abcd_with_carry]="003C 0010 7000 0000 0099 7201 C101"
# NBCD_BASIC: MOVEQ #0,D0; ORI.B #$42,D0; NBCD D0
# NBCD: 0 - D0 - X(0) in BCD = 0 - 0x42 = 0x58 (BCD complement)
# Wait: NBCD with X=0: result = (0x9A - D0) if D0 != 0, or 0 if D0 == 0
# Actually NBCD = 0 - src - X in BCD
# With X=0: 0 - 0x42 in BCD: borrow from tens: 10-2=8 for units, 9-4=5 for tens -> 0x58
# But the real M68K behavior: if zero result with no borrow, Z unchanged; else Z cleared
# MOVEQ #0,D0 = 7000; ORI.B #$42,D0 = 0000 0042; NBCD D0 = 4800
TESTS[nbcd_basic]="7000 0000 0042 4800"
# BSR_RTS: BSR to subroutine that sets D0=#$55, then RTS back
# MOVEQ #1,D0; BSR.B +4; MOVEQ #2,D1; BRA.B +4; MOVEQ #$55,D0; RTS
TESTS[bsr_rts]="7001 6104 7202 6004 7055 4E75"
# LINK_UNLK: LINK A5,#-8 / UNLK A5 frame pointer test
# LEA $4000,A0; MOVEA.L A0,A7; LINK A5,#-8; MOVEQ #$42,D0; MOVE.L D0,(A5);
# CLR.L D0; MOVE.L (A5),D1; UNLK A5
TESTS[link_unlk]="41F9 0000 4000 2E48 4E55 FFF8 7042 2A80 4280 2215 4E5D"
# INDEXED_ADDR_MODE: MOVE.L #$DEADBEEF to (4,A0), read back via (0,A0,D1.L)
# LEA $5000,A0; MOVE.L #$DEADBEEF,(4,A0); CLR.L D0; MOVEQ #4,D1; MOVE.L (0,A0,D1.L),D0
TESTS[indexed_addr_mode]="41F9 0000 5000 217C DEAD BEEF 0004 4280 7204 2030 1800"
# BYTE_POSTINC: write 4 bytes via (A0)+, read back via (A1)+
# LEA $6000,A0; LEA $6000,A1; MOVEQ #$11,D4; MOVE.B D4,(A0)+;
# MOVEQ #$22,D5; MOVE.B D5,(A0)+; MOVEQ #$33,D6; MOVE.B D6,(A0)+;
# MOVEQ #$44,D7; MOVE.B D7,(A0)+; MOVE.B (A1)+,D0; MOVE.B (A1)+,D1;
# MOVE.B (A1)+,D2; MOVE.B (A1)+,D3
TESTS[byte_postinc]="41F9 0000 6000 43F9 0000 6000 7811 10C4 7A22 10C5 7C33 10C6 7E44 10C7 1019 1219 1419 1619"
# CMPM_EQUAL: CMPM.B (A0)+,(A1)+ on equal bytes should set Z=1
# LEA $7000,A0; LEA $7010,A1; MOVEQ #-85,D0 ($AB); MOVE.B D0,(A0); MOVE.B D0,(A1); CMPM.B (A0)+,(A1)+
TESTS[cmpm_equal]="41F9 0000 7000 43F9 0000 7010 70AB 1080 1280 B308"
# MOVE_SR_ROUNDTRIP: MOVE.W #$2710,SR then MOVE.W SR,D0 — SR read/write roundtrip
TESTS[move_sr_roundtrip]="46FC 2710 40C0 7242"
# DBRA_LOOP_100: MOVEQ #99,D0; MOVEQ #0,D1; ADDQ.L #1,D1; DBRA D0,-4
# After 100 iterations: D0.W=$FFFF, D1=100=0x64
# This is a multi-block loop that exercises JIT block re-execution and DBRA compilation
TESTS[dbra_loop_100]="7063 7200 5281 51C8 FFFC"
# MOVE: MOVEQ #0x42,D0; MOVE.L D0,D1; MOVEQ #-1,D2; MOVE.W D2,D3
TESTS[move]="7042 2200 74FF 3602"
# MOVEQ_SIGNEXT: verify MOVEQ sign-extension with CMPI.L and CMPI.W checks
TESTS[moveq_signext]="70FF 0C80 FFFF FFFF 0C40 FFFF 2200"
# ALU: MOVEQ #5,D0; MOVEQ #3,D1; ADD.L D1,D0; SUB.L D1,D0; AND.L D1,D0
TESTS[alu]="7005 7203 D081 9081 C081"
# ALU_OVERFLOW: MOVEQ #0x7f,D0; ADDQ.L #1,D0; SUBQ.L #1,D0
TESTS[alu_overflow]="707F 5280 5180"
# ADDI_SUBI_LONG: MOVEQ #5,D0; ADDI.L #3,D0; SUBI.L #1,D0
TESTS[addi_subi_long]="7005 0680 0000 0003 0480 0000 0001"
# ADDI_SUBI_LONG_WRAP: long arithmetic around 0x7fffffff/0x80000000 boundary with explicit CMPI.L check
TESTS[addi_subi_long_wrap]="203C 7FFF FFFF 0680 0000 0001 0480 0000 0001 0C80 7FFF FFFF"
# ADDI_SUBI_WORD: MOVEQ #0,D0; ADDI.W #0x1234,D0; SUBI.W #0x20,D0
TESTS[addi_subi_word]="7000 0640 1234 0440 0020"
# ADDI_SUBI_WORD_WRAP: word arithmetic around 0x7fff/0x8000 boundary with explicit CMPI.W check
TESTS[addi_subi_word_wrap]="7000 0640 7FFF 0640 0001 0440 0001 0C40 7FFF"
# ADDI_SUBI_BYTE: byte-sized immediate arithmetic with explicit CMPI.B verification
TESTS[addi_subi_byte]="7000 0600 007F 0400 0001 0C00 007E"
# ADDI_SUBI_BYTE_WRAP: byte arithmetic around 0x7f/0x80 boundary with explicit CMPI.B check
TESTS[addi_subi_byte_wrap]="7000 0600 007F 0600 0001 0400 0001 0C00 007F"
# SHIFT: MOVEQ #8,D0; LSL.L #1,D0; LSR.L #2,D0; ASR.L #1,D0; ROL.L #1,D0
TESTS[shift]="7008 E388 E888 E080 E398"
# BITOPS: MOVEQ #0,D0; BSET #3,D0; BTST #3,D0; BCLR #3,D0; BTST #3,D0
TESTS[bitops]="7000 08C0 0003 0800 0003 0880 0003 0800 0003"
# BITOPS_CHG: toggle bit 0 twice with BCHG and verify BTST executes
TESTS[bitops_chg]="7000 0840 0000 0840 0000 0800 0000"
# BITOPS_HIGHBIT: exercise immediate bit operations on bit 31 (long-width boundary)
TESTS[bitops_highbit]="7000 08C0 001F 0800 001F 0880 001F 0800 001F"
# BITOPS_CHG_HIGHBIT: toggle bit 31 twice with BCHG immediate and verify BTST executes
TESTS[bitops_chg_highbit]="7000 0840 001F 0840 001F 0800 001F"
# BRANCH: MOVEQ #0,D0; CMP.L D0,D0; BEQ.S +2; MOVEQ #1,D0 (should skip); MOVEQ #2,D1
TESTS[branch]="7000 B080 6702 7001 7202"
# BRANCH_CHAIN: BEQ taken then BNE not-taken under same flags (Z remains set)
TESTS[branch_chain]="7001 B080 6702 7207 6602 7408"
# COMPARE: MOVEQ #5,D0; MOVEQ #3,D1; CMP.L D1,D0; TST.L D0; CMPI.L #5,D0
TESTS[compare]="7005 7203 B081 4A80 0C80 0000 0005"
# COMPARE_NEGATIVE: compare against -1 and verify BNE not-taken path
TESTS[compare_negative]="70FF 0C80 FFFF FFFF 6602 7207"
# CMPI_SIZES: run CMPI.B/W/L forms against D0 to exercise immediate size decoding
TESTS[cmpi_sizes]="7001 0C00 0001 0C40 0001 0C80 0000 0001"
# CMPI_SIZES_ZERO: run CMPI.B/W/L zero-immediate forms against zeroed D0
TESTS[cmpi_sizes_zero]="7000 0C00 0000 0C40 0000 0C80 0000 0000"
# CMPI_BYTE_NEGATIVE: verify CMPI.B sign/boundary behavior against 0xff and BEQ taken path
TESTS[cmpi_byte_negative]="70FF 0C00 00FF 6702 7207 7408"
# CMPI_WORD_NEGATIVE: verify CMPI.W sign/boundary behavior against 0xffff and BEQ taken path
TESTS[cmpi_word_negative]="70FF 0C40 FFFF 6702 7207 7408"
# CMPI_LONG_NEGATIVE: verify CMPI.L sign/boundary behavior against 0xffffffff and BEQ taken path
TESTS[cmpi_long_negative]="70FF 0C80 FFFF FFFF 6702 7207 7408"
# CMPI_BEQ_TAKEN: compare equal immediate then take BEQ short path
TESTS[cmpi_beq_taken]="7000 0C80 0000 0000 6702 7207 7408"
# MULDIV: MOVEQ #7,D0; MULU.W #3,D0; MOVEQ #21,D1; DIVU.W #3,D1
TESTS[muldiv]="7007 C0FC 0003 7215 82FC 0003"
# MOVEM: setup stack; MOVEM.L D0-D3,-(SP); MOVEM.L (SP)+,D4-D7
TESTS[movem]="7011 7213 7415 7617 48E7 F000 4CDF 000F"
# MISC: MOVEQ #0x5A,D0; SWAP D0; EXT.L D0; CLR.W D1; NEG.L D0
TESTS[misc]="705A 4840 4880 4241 4480"
# CLR_SIZES: verify CLR.B/W/L execution paths and immediate compares on resulting zeros
TESTS[clr_sizes]="203C FFFF FFFF 4200 0C00 0000 4240 0C40 0000 4280 0C80 0000 0000"
# CLR_BYTE_PRESERVE_UPPER: CLR.B should clear low byte while preserving upper 24 bits
TESTS[clr_byte_preserve_upper]="203C 1234 5678 4200 0C80 1234 5600"
# CLR_WORD_PRESERVE_UPPER: CLR.W should clear low word while preserving upper 16 bits
TESTS[clr_word_preserve_upper]="203C 89AB CDEF 4240 0C80 89AB 0000"
# NEG_SIZES: verify NEG.B/W/L execution paths against -1 results in each size domain
TESTS[neg_sizes]="7001 4400 0C00 00FF 7201 4441 0C41 FFFF 7401 4482 0C82 FFFF FFFF"
# NEG_ZERO_SIZES: verify NEG.B/W/L on zero value keeps result at zero across size forms
TESTS[neg_zero_sizes]="7000 4400 4440 4480 0C80 0000 0000"
# SWAP_ROUNDTRIP: SWAP applied twice should restore original long value
TESTS[swap_roundtrip]="7012 4840 4840 0C80 0000 0012"
# FLAGS: MOVE #0x2700,SR; ORI #0x10,CCR; ANDI #0xEF,CCR; MOVE SR,D0
TESTS[flags]="46FC 2700 003C 0010 023C 00EF 40C0"
# FLAGS_EORI_CCR: verify EORI to CCR path under supervisor SR setup
TESTS[flags_eori_ccr]="46FC 2700 003C 0011 0A3C 0010 40C0"
# EXG: MOVEQ #1,D0; MOVEQ #2,D1; EXG D0,D1
TESTS[exg]="7001 7202 C141"
# EXG_ROUNDTRIP: two EXG operations should restore original D0/D1 values
TESTS[exg_roundtrip]="7001 7202 C141 C141"
# IMM_LOGIC: MOVEQ #0,D0; ORI.B #0x0f,D0; EORI.B #0xf0,D0; ANDI.B #0x3c,D0
TESTS[imm_logic]="7000 0000 000F 0A00 00F0 0200 003C"
# IMM_LOGIC_ALT: alternate immediate-byte logic sequence for edge mask patterns
TESTS[imm_logic_alt]="7000 0000 00AA 0200 000F 0A00 0005"
# IMM_LOGIC_BYTE_HIGHBIT: byte-width OR/EOR around 0x80 edge, then normalize with ANDI.B/CMPI.B
TESTS[imm_logic_byte_highbit]="7000 0000 0080 0A00 0080 0200 00FF 0C00 0000"
# IMM_LOGIC_WORD: immediate word-width OR/EOR/AND sequence to cover non-byte forms
TESTS[imm_logic_word]="7000 0040 00FF 0A40 0F0F 0240 00F0"
# IMM_LOGIC_LONG: immediate long-width OR/EOR/AND sequence to cover .L forms
TESTS[imm_logic_long]="7000 0080 00FF 00FF 0A80 0F0F 0F0F 0280 00F0 00F0"
# IMM_LOGIC_LONG_ALT: alternate immediate long masks to exercise non-trivial bit patterns
TESTS[imm_logic_long_alt]="203C F0F0 F0F0 0080 0F0F 0F0F 0A80 00FF 00FF 0280 0F0F 0F0F"
# TST_SIZES: exercise TST.B/W/L decode+flag paths on a negative value
TESTS[tst_sizes]="70FF 4A00 4A40 4A80"
# TST_ZERO: exercise TST.B/W/L decode+flag paths on zero value
TESTS[tst_zero]="7000 4A00 4A40 4A80"
# TST_POSITIVE: exercise TST.B/W/L decode+flag paths on positive non-zero value
TESTS[tst_positive]="7001 4A00 4A40 4A80"
# BRA_TAKEN: unconditional branch should skip MOVEQ #9,D1
TESTS[bra_taken]="7001 6002 7209 7402"
# BRA_W_TAKEN: unconditional word-displacement branch should skip MOVEQ #9,D1
TESTS[bra_w_taken]="7001 6000 0002 7209 7402"
# BNE_NOT_TAKEN: CMP.L D0,D0 sets Z=1; BNE should not branch
TESTS[bne_not_taken]="7001 B080 6602 7207"
# BNE_TAKEN: CMPI.L #2,D0 sets Z=0; BNE should branch and skip MOVEQ #7,D1
TESTS[bne_taken]="7001 0C80 0000 0002 6602 7207 7408"
# BNE_W_NOT_TAKEN: CMP.L D0,D0 sets Z=1; BNE.W should not branch
TESTS[bne_w_not_taken]="7001 B080 6600 0002 7207"
# BNE_W_TAKEN: CMPI.L #2,D0 sets Z=0; BNE.W should branch and skip MOVEQ #7,D1
TESTS[bne_w_taken]="7001 0C80 0000 0002 6600 0002 7207 7408"
# BEQ_TAKEN: CMP.L D0,D0 sets Z=1; BEQ should branch and skip MOVEQ #7,D1
TESTS[beq_taken]="7001 B080 6702 7207 7408"
# BEQ_NOT_TAKEN: CMPI.L #2,D0 sets Z=0; BEQ should not branch
TESTS[beq_not_taken]="7001 0C80 0000 0002 6702 7207"
# BEQ_W_TAKEN: CMP.L D0,D0 sets Z=1; BEQ.W should branch and skip MOVEQ #7,D1
TESTS[beq_w_taken]="7001 B080 6700 0002 7207 7408"
# BEQ_W_NOT_TAKEN: CMPI.L #2,D0 sets Z=0; BEQ.W should not branch
TESTS[beq_w_not_taken]="7001 0C80 0000 0002 6700 0002 7207"
# BPL_TAKEN: N=0 so BPL should branch and skip MOVEQ #9,D1
TESTS[bpl_taken]="7001 6A02 7209 7402"
# BPL_NOT_TAKEN: N=1 (MOVEQ #-1) so BPL should not branch
TESTS[bpl_not_taken]="70FF 6A02 7203"
# BPL_W_TAKEN: word-displacement BPL with N=0 should branch and skip MOVEQ #9,D1
TESTS[bpl_w_taken]="7001 6A00 0002 7209 7402"
# BPL_W_NOT_TAKEN: word-displacement BPL with N=1 should not branch
TESTS[bpl_w_not_taken]="70FF 6A00 0002 7203"
# BMI_TAKEN: N=1 (MOVEQ #-1) so BMI should branch and skip MOVEQ #9,D1
TESTS[bmi_taken]="70FF 6B02 7209 7402"
# BMI_NOT_TAKEN: N=0 so BMI should not branch
TESTS[bmi_not_taken]="7001 6B02 7203"
# BMI_W_TAKEN: word-displacement BMI with N=1 should branch and skip MOVEQ #9,D1
TESTS[bmi_w_taken]="70FF 6B00 0002 7209 7402"
# BMI_W_NOT_TAKEN: word-displacement BMI with N=0 should not branch
TESTS[bmi_w_not_taken]="7001 6B00 0002 7203"
# BVC_TAKEN: V=0 in this sequence, so BVC should branch and skip MOVEQ #9,D1
TESTS[bvc_taken]="7001 6802 7209 7402"
# BVC_NOT_TAKEN_OVERFLOW: 0x7fffffff + 1 sets V=1; BVC should not branch
TESTS[bvc_not_taken_overflow]="203C 7FFF FFFF 5280 6802 7207"
# BVC_W_TAKEN: word-displacement BVC with V=0 should branch and skip MOVEQ #9,D1
TESTS[bvc_w_taken]="7001 6800 0002 7209 7402"
# BVC_W_NOT_TAKEN_OVERFLOW: word-displacement BVC with V=1 should not branch
TESTS[bvc_w_not_taken_overflow]="203C 7FFF FFFF 5280 6800 0002 7207"
# BVS_TAKEN_OVERFLOW: 0x7fffffff + 1 sets V=1; BVS should branch and skip MOVEQ #7,D1
TESTS[bvs_taken_overflow]="203C 7FFF FFFF 5280 6902 7207 7408"
# BVS_NOT_TAKEN: V=0 in this sequence, so BVS should not branch
TESTS[bvs_not_taken]="7001 6902 7203"
# BVS_W_TAKEN_OVERFLOW: word-displacement BVS with V=1 should branch and skip MOVEQ #7,D1
TESTS[bvs_w_taken_overflow]="203C 7FFF FFFF 5280 6900 0002 7207 7408"
# BVS_W_NOT_TAKEN: word-displacement BVS with V=0 should not branch
TESTS[bvs_w_not_taken]="7001 6900 0002 7203"
# BGE_TAKEN: N==V==0, so BGE should branch and skip MOVEQ #9,D1
TESTS[bge_taken]="7001 6C02 7209 7402"
# BGE_NOT_TAKEN: CMPI.L #2,D0 yields N=1,V=0; BGE should not branch
TESTS[bge_not_taken]="7001 0C80 0000 0002 6C02 7207"
# BGE_W_TAKEN: word-displacement BGE with N==V==0 should branch and skip MOVEQ #9,D1
TESTS[bge_w_taken]="7001 6C00 0002 7209 7402"
# BGE_W_NOT_TAKEN: word-displacement BGE with N!=V should not branch
TESTS[bge_w_not_taken]="7001 0C80 0000 0002 6C00 0002 7207"
# BLT_TAKEN: CMPI.L #2,D0 yields N=1,V=0; BLT should branch and skip MOVEQ #7,D1
TESTS[blt_taken]="7001 0C80 0000 0002 6D02 7207 7408"
# BLT_NOT_TAKEN: N==V==0, so BLT should not branch
TESTS[blt_not_taken]="7001 6D02 7203"
# BLT_W_TAKEN: word-displacement BLT with N!=V should branch and skip MOVEQ #7,D1
TESTS[blt_w_taken]="7001 0C80 0000 0002 6D00 0002 7207 7408"
# BLT_W_NOT_TAKEN: word-displacement BLT with N==V==0 should not branch
TESTS[blt_w_not_taken]="7001 6D00 0002 7203"
# BGT_TAKEN: Z==0 and N==V==0, so BGT should branch and skip MOVEQ #9,D1
TESTS[bgt_taken]="7001 6E02 7209 7402"
# BGT_NOT_TAKEN: CMP.L D0,D0 sets Z=1; BGT should not branch
TESTS[bgt_not_taken]="7001 B080 6E02 7207"
# BGT_W_TAKEN: word-displacement BGT with Z==0,N==V==0 should branch and skip MOVEQ #9,D1
TESTS[bgt_w_taken]="7001 6E00 0002 7209 7402"
# BGT_W_NOT_TAKEN: word-displacement BGT with Z=1 should not branch
TESTS[bgt_w_not_taken]="7001 B080 6E00 0002 7207"
# BLE_TAKEN: CMP.L D0,D0 sets Z=1; BLE should branch and skip MOVEQ #7,D1
TESTS[ble_taken]="7001 B080 6F02 7207 7408"
# BLE_NOT_TAKEN: Z==0 and N==V==0, so BLE should not branch
TESTS[ble_not_taken]="7001 6F02 7203"
# BLE_W_TAKEN: word-displacement BLE with Z=1 should branch and skip MOVEQ #7,D1
TESTS[ble_w_taken]="7001 B080 6F00 0002 7207 7408"
# BLE_W_NOT_TAKEN: word-displacement BLE with Z==0,N==V==0 should not branch
TESTS[ble_w_not_taken]="7001 6F00 0002 7203"
# BCC_TAKEN: CMPI.L #0,D0 sets C=0; BCC should branch and skip MOVEQ #7,D1
TESTS[bcc_taken]="7001 0C80 0000 0000 6402 7207 7408"
# BCC_NOT_TAKEN: CMPI.L #2,D0 sets C=1; BCC should not branch
TESTS[bcc_not_taken]="7001 0C80 0000 0002 6402 7207"
# BCC_W_TAKEN: word-displacement BCC with C=0 should branch and skip MOVEQ #7,D1
TESTS[bcc_w_taken]="7001 0C80 0000 0000 6400 0002 7207 7408"
# BCC_W_NOT_TAKEN: word-displacement BCC with C=1 should not branch
TESTS[bcc_w_not_taken]="7001 0C80 0000 0002 6400 0002 7207"
# BCS_TAKEN: CMPI.L #2,D0 sets C=1; BCS should branch and skip MOVEQ #7,D1
TESTS[bcs_taken]="7001 0C80 0000 0002 6502 7207 7408"
# BCS_NOT_TAKEN: CMPI.L #0,D0 sets C=0; BCS should not branch
TESTS[bcs_not_taken]="7001 0C80 0000 0000 6502 7207"
# BCS_W_TAKEN: word-displacement BCS with C=1 should branch and skip MOVEQ #7,D1
TESTS[bcs_w_taken]="7001 0C80 0000 0002 6500 0002 7207 7408"
# BCS_W_NOT_TAKEN: word-displacement BCS with C=0 should not branch
TESTS[bcs_w_not_taken]="7001 0C80 0000 0000 6500 0002 7207"
# BHI_TAKEN: CMPI.L #0,D0 sets C=0,Z=0; BHI should branch and skip MOVEQ #7,D1
TESTS[bhi_taken]="7001 0C80 0000 0000 6202 7207 7408"
# BHI_NOT_TAKEN: CMP.L D0,D0 sets Z=1; BHI should not branch
TESTS[bhi_not_taken]="7001 B080 6202 7207"
# BHI_W_TAKEN: word-displacement BHI with C=0,Z=0 should branch and skip MOVEQ #7,D1
TESTS[bhi_w_taken]="7001 0C80 0000 0000 6200 0002 7207 7408"
# BHI_W_NOT_TAKEN: word-displacement BHI with Z=1 should not branch
TESTS[bhi_w_not_taken]="7001 B080 6200 0002 7207"
# BLS_TAKEN: CMP.L D0,D0 sets Z=1; BLS should branch and skip MOVEQ #7,D1
TESTS[bls_taken]="7001 B080 6302 7207 7408"
# BLS_NOT_TAKEN: CMPI.L #0,D0 sets C=0,Z=0; BLS should not branch
TESTS[bls_not_taken]="7001 0C80 0000 0000 6302 7207"
# BLS_W_TAKEN: word-displacement BLS with Z=1 should branch and skip MOVEQ #7,D1
TESTS[bls_w_taken]="7001 B080 6300 0002 7207 7408"
# BLS_W_NOT_TAKEN: word-displacement BLS with C=0,Z=0 should not branch
TESTS[bls_w_not_taken]="7001 0C80 0000 0000 6300 0002 7207"
# SCC_BASIC: MOVEQ #0,D0/D1; ST D0 (set true); SF D1 (set false)
TESTS[scc_basic]="7000 7200 50C0 51C1"
# SCC_EQ_NE: set Z=1, then SNE should be false and SEQ should be true
TESTS[scc_eq_ne]="7001 7200 7400 B080 56C1 57C2"
# SCC_CARRY: set C=1, then SCC should be false and SCS should be true
TESTS[scc_carry]="7001 7200 7400 0C80 0000 0002 54C1 55C2"
# SCC_HI_LS: set C=0,Z=0; SHI should be true and SLS should be false
TESTS[scc_hi_ls]="7001 7200 7400 0C80 0000 0000 52C1 53C2"
# SCC_HI_LS_Z: set Z=1; SHI should be false and SLS should be true
TESTS[scc_hi_ls_z]="7001 7200 7400 B080 52C1 53C2"
# SCC_VC_VS: force V=1 via overflow; SVC should be false and SVS should be true
TESTS[scc_vc_vs]="203C 7FFF FFFF 5280 58C1 59C2"
# SCC_PL_MI: set N=1 via MOVEQ #-1; SPL should be false and SMI should be true
TESTS[scc_pl_mi]="70FF 5AC1 5BC2"
# SCC_GE_LT: CMPI.L #2,D0 with D0=1 gives N=1,V=0; SGE false, SLT true
TESTS[scc_ge_lt]="7001 0C80 0000 0002 5CC1 5DC2"
# SCC_GT_LE: CMP.L D0,D0 gives Z=1; SGT false, SLE true
TESTS[scc_gt_le]="7001 B080 5EC1 5FC2"
# SCC_CCR_PRESERVE_BLT: SLT should not clobber CCR; BLT must still branch from prior CMPI flags
TESTS[scc_ccr_preserve_blt]="7001 0C80 0000 0002 5DC1 6D02 7407 7608"
# SCC_CCR_PRESERVE_BCS: SCS should not clobber CCR; BCS must still branch from prior CMPI carry flag
TESTS[scc_ccr_preserve_bcs]="7001 0C80 0000 0002 55C1 6502 7407 7608"
# SCC_CCR_PRESERVE_BNE_NOT_TAKEN: SNE should not clobber CCR; BNE should remain not-taken when Z=1
TESTS[scc_ccr_preserve_bne_not_taken]="7001 B080 56C1 6602 7407 7608"
# SCC_CCR_PRESERVE_BEQ_TAKEN: SEQ should not clobber CCR; BEQ should remain taken when Z=1
TESTS[scc_ccr_preserve_beq_taken]="7001 B080 57C1 6702 7407 7608"
# QUICK_OPS: MOVEQ #5,D0; ADDQ.L #1,D0; SUBQ.L #1,D0; MOVE.L D0,D1
TESTS[quick_ops]="7005 5280 5180 2200"
# QUICK_OPS_LONG_NEG_ROUNDTRIP: start at -1, addq/subq roundtrip through zero and verify long result
TESTS[quick_ops_long_neg_roundtrip]="70FF 5280 5180 0C80 FFFF FFFF"
# QUICK_OPS_WORD: word-sized add/sub quick on D0 low word, then move.w to D1
TESTS[quick_ops_word]="70FF 5240 5140 3200"
# QUICK_OPS_WORD_WRAP: ADDQ/SUBQ word across 0x7fff/0x8000 boundary with explicit CMPI.W check
TESTS[quick_ops_word_wrap]="7000 0640 7FFF 5240 5140 0C40 7FFF"
# QUICK_OPS_LONG_WRAP: ADDQ/SUBQ long across 0x7fffffff/0x80000000 boundary with explicit CMPI.L check
TESTS[quick_ops_long_wrap]="203C 7FFF FFFF 5280 5180 0C80 7FFF FFFF"
# QUICK_OPS_BYTE: byte-sized add/sub quick on D0 low byte with explicit CMPI.B validation
TESTS[quick_ops_byte]="7000 5200 5100 0C00 0000"
# QUICK_OPS_BYTE_WRAP: ADDQ/SUBQ byte across 0x7f/0x80 boundary with explicit CMPI.B check
TESTS[quick_ops_byte_wrap]="707F 5200 5100 0C00 007F"
# QUICK_OPS_ADDR: addq/subq on A0 uses address-register execution path
TESTS[quick_ops_addr]="207C 0000 0100 5288 5188"
# DBRA: MOVEQ #1,D0; DBRA D0,+2 (taken once, skips MOVEQ #9,D1); NOP
TESTS[dbra]="7001 51C8 0002 7209 4E71"
# DBRA_NOT_TAKEN: MOVEQ #0,D0; DBRA D0,+2 should not branch (counter reaches -1)
TESTS[dbra_not_taken]="7000 51C8 0002 7207"
# DBRA_START_MINUS1_BRANCH: start at D0=-1 (0xFFFF low word) and verify DBRA still decrements and branches
# MOVEQ #-1,D0; DBRA D0,+2; MOVEQ #7,D1 (skipped if branch); MOVEQ #8,D2
TESTS[dbra_start_minus1_branch]="70FF 51C8 0002 7207 7408"
# DBRA_START_8000_BRANCH: start at D0=0x8000 and verify DBRA decrements to 0x7fff and branches
# MOVE.L #0x00008000,D0; DBRA D0,+2; MOVEQ #7,D1 (skipped if branch); MOVEQ #8,D2
TESTS[dbra_start_8000_branch]="203C 0000 8000 51C8 0002 7207 7408"
# DBT_TRUE_NOT_TAKEN: condition true should never decrement or branch in DBcc form
TESTS[dbt_true_not_taken]="7001 7400 50C8 0002 7207"
# DBRA_THREE_ITER: D0 starts at 2; loop body ADDQ runs three times before fallthrough
TESTS[dbra_three_iter]="7002 7200 5281 51C8 FFFA"
# DBCC_LOOP_C_SET: set C=1 so DBCC condition is false; bounded loop executes exactly twice for D0=1
TESTS[dbcc_loop_c_set]="7001 7201 0C81 0000 0002 4E71 54C8 FFFA"
# DBCS_NOT_TAKEN_C_SET: set C=1 so DBCS condition is true (no decrement/branch)
TESTS[dbcs_not_taken_c_set]="7001 7201 0C81 0000 0002 55C8 0002 7407"
# DBPL_LOOP_N_SET: set N=1 so DBPL condition is false; bounded loop executes exactly twice for D0=1
TESTS[dbpl_loop_n_set]="7001 74FF 4E71 5AC8 FFFA"
# DBMI_NOT_TAKEN_N_SET: set N=1 so DBMI condition is true (no decrement/branch)
TESTS[dbmi_not_taken_n_set]="7001 74FF 5BC8 0002 7608"
# DBHI_NOT_TAKEN_HI_SET: set C=0,Z=0 so DBHI condition is true (no decrement/branch)
TESTS[dbhi_not_taken_hi_set]="7001 7201 0C81 0000 0000 52C8 0002 7407"
# DBLS_NOT_TAKEN_LS_SET: set Z=1 so DBLS condition is true (no decrement/branch)
TESTS[dbls_not_taken_ls_set]="7001 B080 53C8 0002 7407"
# DBGE_NOT_TAKEN_N_EQ_V: set N==V so DBGE condition is true (no decrement/branch)
TESTS[dbge_not_taken_n_eq_v]="7001 7201 0C81 0000 0000 5CC8 0002 7407"
# DBLT_NOT_TAKEN_N_NE_V: set N!=V so DBLT condition is true (no decrement/branch)
TESTS[dblt_not_taken_n_ne_v]="7001 7201 0C81 0000 0002 5DC8 0002 7407"
# DBGT_NOT_TAKEN_GT_SET: set Z=0,N==V so DBGT condition is true (no decrement/branch)
TESTS[dbgt_not_taken_gt_set]="7001 7201 0C81 0000 0000 5EC8 0002 7407"
# DBLE_NOT_TAKEN_LE_SET: set Z=1 so DBLE condition is true (no decrement/branch)
TESTS[dble_not_taken_le_set]="7001 B080 5FC8 0002 7407"
# DBHI_FALSE_DEC_TERMINAL_LS_SET: set LS=true so DBHI is false; D0=0 forces one decrement-to-terminal path
TESTS[dbhi_false_dec_terminal_ls_set]="7000 B080 52C8 0002 7407"
# DBLS_FALSE_DEC_TERMINAL_HI_SET: set HI=true so DBLS is false; D0=0 forces one decrement-to-terminal path
TESTS[dbls_false_dec_terminal_hi_set]="7000 7201 0C81 0000 0000 53C8 0002 7407"
# DBGE_FALSE_DEC_TERMINAL_N_NE_V: set N!=V so DBGE is false; D0=0 forces one decrement-to-terminal path
TESTS[dbge_false_dec_terminal_n_ne_v]="7000 7201 0C81 0000 0002 5CC8 0002 7407"
# DBLT_FALSE_DEC_TERMINAL_N_EQ_V: set N==V so DBLT is false; D0=0 forces one decrement-to-terminal path
TESTS[dblt_false_dec_terminal_n_eq_v]="7000 7201 0C81 0000 0000 5DC8 0002 7407"
# DBGT_FALSE_DEC_TERMINAL_Z_SET: set Z=1 so DBGT is false; D0=0 forces one decrement-to-terminal path
TESTS[dbgt_false_dec_terminal_z_set]="7000 B080 5EC8 0002 7407"
# DBLE_FALSE_DEC_TERMINAL_GT_SET: set Z=0,N==V so DBLE is false; D0=0 forces one decrement-to-terminal path
TESTS[dble_false_dec_terminal_gt_set]="7000 7201 0C81 0000 0000 5FC8 0002 7407"
# DBCC_CCR_PRESERVE_BEQ_TAKEN: DBEQ (condition true) should not clobber Z; subsequent BEQ must remain taken
TESTS[dbcc_ccr_preserve_beq_taken]="7001 B080 57C8 0002 6702 7207 7408"
# DBCC_CCR_PRESERVE_BNE_TAKEN: DBNE (condition true) should not clobber Z=0; subsequent BNE must remain taken
TESTS[dbcc_ccr_preserve_bne_taken]="7001 7201 0C81 0000 0002 56C8 0002 6602 7407 7608"
# DBCC_CCR_PRESERVE_BCS_TAKEN: DBCS (condition true) should not clobber C=1; subsequent BCS must remain taken
TESTS[dbcc_ccr_preserve_bcs_taken]="7001 7201 0C81 0000 0002 55C8 0002 6502 7407 7608"
# DBCC_CCR_PRESERVE_BVC_TAKEN: DBVC (condition true) should not clobber V=0; subsequent BVC must remain taken
TESTS[dbcc_ccr_preserve_bvc_taken]="7001 7201 0C81 0000 0000 58C8 0002 6802 7407 7608"
# DBCC_CCR_PRESERVE_BVS_TAKEN: DBVS (condition true) should not clobber V=1; subsequent BVS must remain taken
TESTS[dbcc_ccr_preserve_bvs_taken]="7001 243C 7FFF FFFF 5282 59C8 0002 6902 7407 7608"
# DBCC_CCR_PRESERVE_BHI_TAKEN: DBHI (condition true) should not clobber C/Z; subsequent BHI must remain taken
TESTS[dbcc_ccr_preserve_bhi_taken]="7001 7201 0C81 0000 0000 52C8 0002 6202 7407 7608"
# DBCC_CCR_PRESERVE_BLS_TAKEN: DBLS (condition true) should not clobber C/Z; subsequent BLS must remain taken
TESTS[dbcc_ccr_preserve_bls_taken]="7001 B080 53C8 0002 6302 7407 7608"
# DBCC_CCR_PRESERVE_BGE_TAKEN: DBGE (condition true) should not clobber N/V; subsequent BGE must remain taken
TESTS[dbcc_ccr_preserve_bge_taken]="7001 7201 0C81 0000 0000 5CC8 0002 6C02 7407 7608"
# DBCC_CCR_PRESERVE_BLT_TAKEN: DBLT (condition true) should not clobber N/V; subsequent BLT must remain taken
TESTS[dbcc_ccr_preserve_blt_taken]="7001 7201 0C81 0000 0002 5DC8 0002 6D02 7407 7608"
# DBCC_CCR_PRESERVE_BGT_TAKEN: DBGT (condition true) should not clobber Z/N/V; subsequent BGT must remain taken
TESTS[dbcc_ccr_preserve_bgt_taken]="7001 7201 0C81 0000 0000 5EC8 0002 6E02 7407 7608"
# DBCC_CCR_PRESERVE_BLE_TAKEN: DBLE (condition true) should not clobber Z/N/V; subsequent BLE must remain taken
TESTS[dbcc_ccr_preserve_ble_taken]="7001 B080 5FC8 0002 6F02 7407 7608"
# DBVC_LOOP_V_SET: force V=1; DBVC condition is false so bounded DBcc loop should execute twice for D0=1
TESTS[dbvc_loop_v_set]="7001 243C 7FFF FFFF 5282 4E71 58C8 FFFA"
# DBVS_LOOP_V_CLEAR: force V=0; DBVS condition is false so bounded DBcc loop should execute twice for D0=1
TESTS[dbvs_loop_v_clear]="7001 7400 4E71 59C8 FFFA"
# DBVC_NOT_TAKEN_V_CLEAR: V=0 makes DBVC condition true (no decrement/branch)
TESTS[dbvc_not_taken_v_clear]="7001 7400 58C8 0002 7207"
# DBVS_NOT_TAKEN_V_SET: V=1 makes DBVS condition true (no decrement/branch)
TESTS[dbvs_not_taken_v_set]="7001 243C 7FFF FFFF 5282 59C8 0002 7207"
# DBNE_LOOP_Z_SET: with Z=1, DBNE decrements and loops exactly twice for D0=1
TESTS[dbne_loop_z_set]="7001 B080 4E71 56C8 FFFA"
# DBEQ_LOOP_Z_CLEAR: with Z=0, DBEQ decrements and loops exactly twice for D0=1
TESTS[dbeq_loop_z_clear]="7001 0C80 0000 0002 4E71 57C8 FFFA"
# MOVEQ_EDGES: verify MOVEQ sign-extension for -128 and positive edge 127
TESTS[moveq_edges]="7080 0C80 FFFF FF80 707F 0C80 0000 007F"
# ALU_NEGATIVE_ROUNDTRIP: D0=-1, add/sub 1 roundtrip should restore -1
TESTS[alu_negative_roundtrip]="70FF 7201 D081 9081 0C80 FFFF FFFF"
# IMM_LOGIC_WORD_HIGHBIT: ORI/EORI/ANDI.W around 0x8000 high-bit edge should normalize back to zero
TESTS[imm_logic_word_highbit]="7000 0040 8000 0A40 8000 0240 FFFF 0C40 0000"
# BRANCH_CHAIN_Z_CLEAR: BNE taken then BEQ not-taken under persistent Z=0
TESTS[branch_chain_z_clear]="7001 0C80 0000 0002 6602 7207 6702 7408"
# BRANCH_CHAIN_CARRY_SET: BCS taken then BCC not-taken while carry remains set
TESTS[branch_chain_carry_set]="7001 0C80 0000 0002 6502 7207 6402 7408"
# BRANCH_CHAIN_OVERFLOW_SET: BVS taken then BVC not-taken while overflow remains set
TESTS[branch_chain_overflow_set]="203C 7FFF FFFF 5280 6902 7207 6802 7408"
# SCC_CCR_PRESERVE_BVS_TAKEN: SVS should not clobber CCR; BVS must remain taken from prior overflow
TESTS[scc_ccr_preserve_bvs_taken]="203C 7FFF FFFF 5280 59C1 6902 7407 7608"
# DBRA_FOUR_ITER: D0 starts at 3; loop body ADDQ runs four times before fallthrough
TESTS[dbra_four_iter]="7003 7200 5281 51C8 FFFA"
# SCC_CCR_PRESERVE_BVC_TAKEN: SVC should not clobber CCR; BVC must remain taken when V=0
TESTS[scc_ccr_preserve_bvc_taken]="7001 58C1 6802 7407 7608"
# SCC_CCR_PRESERVE_BHI_TAKEN: SHI should not clobber CCR; BHI must remain taken when C=0,Z=0
TESTS[scc_ccr_preserve_bhi_taken]="7001 0C80 0000 0000 52C1 6202 7407 7608"
# SCC_CCR_PRESERVE_BLS_TAKEN: SLS should not clobber CCR; BLS must remain taken when Z=1
TESTS[scc_ccr_preserve_bls_taken]="7001 B080 53C1 6302 7407 7608"
# DBRA_FIVE_ITER: D0 starts at 4; loop body ADDQ runs five times before fallthrough
TESTS[dbra_five_iter]="7004 7200 5281 51C8 FFFA"
# BRANCH_CHAIN_EQ_THEN_NE: BEQ taken then BNE not-taken under persistent Z=1
TESTS[branch_chain_eq_then_ne]="70FF 0C80 FFFF FFFF 6702 7207 6602 7408"
# BRANCH_CHAIN_CARRY_CLEAR: BCC taken then BCS not-taken while carry remains clear
TESTS[branch_chain_carry_clear]="7001 0C80 0000 0000 6402 7207 6502 7408"
# IMM_LOGIC_LONG_HIGHBIT: ORI/EORI/ANDI.L around 0x80000000 high-bit edge should normalize back to zero
TESTS[imm_logic_long_highbit]="7000 0080 8000 0000 0A80 8000 0000 0280 FFFF FFFF 0C80 0000 0000"
# DBRA_SIX_ITER: D0 starts at 5; loop body ADDQ runs six times before fallthrough
TESTS[dbra_six_iter]="7005 7200 5281 51C8 FFFA"
# NOT_SIZES: verify NOT.B/W/L transitions on D0 across byte/word/long domains
TESTS[not_sizes]="7000 4600 0C80 0000 00FF 4640 0C80 0000 FF00 4680 0C80 FFFF 00FF"
# NOT_WORD_PRESERVE_UPPER: NOT.W should affect only low word and preserve upper 16 bits
TESTS[not_word_preserve_upper]="203C 1234 5678 4640 0C80 1234 A987"
# NOT_BYTE_PRESERVE_UPPER: NOT.B should affect only low byte and preserve upper 24 bits
TESTS[not_byte_preserve_upper]="203C 1234 5678 4600 0C80 1234 5687"
# SCC_CCR_PRESERVE_BPL_TAKEN: SPL should not clobber CCR; BPL must remain taken when N=0
TESTS[scc_ccr_preserve_bpl_taken]="7001 5AC1 6A02 7407 7608"
# SCC_CCR_PRESERVE_BMI_TAKEN: SMI should not clobber CCR; BMI must remain taken when N=1
TESTS[scc_ccr_preserve_bmi_taken]="70FF 5BC1 6B02 7407 7608"
# SCC_CCR_PRESERVE_BGE_TAKEN: SGE should not clobber CCR; BGE must remain taken when N==V
TESTS[scc_ccr_preserve_bge_taken]="7001 5CC1 6C02 7407 7608"
# SCC_CCR_PRESERVE_BGT_TAKEN: SGT should not clobber CCR; BGT must remain taken when Z=0 and N==V
TESTS[scc_ccr_preserve_bgt_taken]="7001 5EC1 6E02 7407 7608"
# SCC_CCR_PRESERVE_BLE_TAKEN: SLE should not clobber CCR; BLE must remain taken when Z=1
TESTS[scc_ccr_preserve_ble_taken]="7001 B080 5FC1 6F02 7407 7608"
# --- Multi-block and ROM-like pattern vectors ---
# DBNE_LOOP_CMPI: DBNE with CMPI condition, exits when D1==3
TESTS[dbne_loop_cmpi]="7005 7200 5281 0C81 0000 0003 56C8 FFF6"
# BSR_IN_DBRA_LOOP: BSR to subroutine inside DBRA loop, 4 iterations
TESTS[bsr_in_dbra_loop]="7003 7200 6108 51C8 FFFC 6006 4E71 5281 4E75"
# TABLE_LOOKUP: PC-relative table read via scaled index
TESTS[table_lookup]="41F9 0000 9000 20BC 1111 1111 217C 2222 2222 0004 217C 3333 3333 0008 7202 E589 2430 1800"
# DBRA_LOOP_1000: 1000-iteration loop
TESTS[dbra_loop_1000]="203C 0000 03E7 7200 5281 51C8 FFFC"
# SWAP_PACK: pack two words into a long via SWAP+MOVE.W+SWAP
TESTS[swap_pack]="203C 0000 AABB 4840 303C CCDD 4840"
# LEA_SCALED_INDEX: LEA (0,A0,D1.L*4) scaled indexed addressing
TESTS[lea_scaled_index]="41F9 0000 7000 7203 43F0 1C00 2009"
# MULTI_BRANCH: sequential BEQ+BNE with flag propagation
TESTS[multi_branch]="7005 0C80 0000 0005 6702 72FF 7403 6602 76FF"
# ANDI_L_DN: AND.L immediate with register
TESTS[andi_l_dn]="203C DEAD BEEF 0280 FF00 FF00"
# EOR_SELF: EOR.L Dn,Dn (self-XOR = clear, Z=1)
TESTS[eor_self]="203C DEAD BEEF B180"
TESTS[asl_w_vflag]="203C 0000 6000 E540"
TESTS[asl_b_overflow]="203C 0000 0060 E700"
TESTS[lsr_w_regcount]="203C FFFF 8001 720F E368"
TESTS[asr_w_preserve]="203C FFFF 8000 E240"
TESTS[movem_w_signext]="41F9 0000 A000 30FC FF80 317C 0042 0002 41F9 0000 A000 4C98 0003"
TESTS[cmpm_l_equal]="41F9 0000 B000 43F9 0000 B010 20BC DEAD BEEF 22BC DEAD BEEF 45F9 0000 B000 47F9 0000 B010 B78A"
TESTS[cmpm_b_unequal]="41F9 0000 C000 43F9 0000 C010 10FC 00AA 12FC 00BB 45F9 0000 C000 47F9 0000 C010 B70A"
TESTS[addx_64bit]="70FF 72FF 7401 7600 D482 D383"
TESTS[subx_64bit]="203C 0000 0000 223C 0000 0001 7401 7600 9482 9383"
TESTS[muls_boundary]="203C 0000 8000 223C 0000 8000 C1C1"
TESTS[divu_max_quotient]="203C 0000 FFFE 7202 80C1"
TESTS[move_b_preserve_flags]="203C AABB CCDD 103C 0011 4A00"
TESTS[byte_logic_chain]="203C AABB CCDD 0000 000F 0200 00F0 0A00 00FF"
TESTS[bchg_imm_high]="4280 0840 001F"
TESTS[neg_w_partial]="203C AABB 0005 4440"
TESTS[clr_b_tst]="203C DEAD BEEF 4200 4A00"
TESTS[all_regs_alive]="7001 7202 7403 7604 7805 7A06 7C07 7E08 41F9 0000 0100 43F9 0000 0200 45F9 0000 0300 47F9 0000 0400 49F9 0000 0500 4BF9 0000 0600 D081"
TESTS[scaled_index_word]="41F9 0000 D000 20BC 1111 1111 217C 2222 2222 0004 217C 3333 3333 0008 7202 2430 1200"
TESTS[byte_indexed_load]="41F9 0000 E000 10FC 00AA 117C 00BB 0001 117C 00CC 0002 117C 00DD 0003 7203 1030 1800"
TESTS[indexed_store_load]="41F9 0000 E100 7042 7204 2180 1800 4280 2430 1800"
TESTS[addq_subq_sizes]="203C AABB CCDD 5600 5340 5E80"
TESTS[x_flag_chain]="70FF 0680 0000 0001 7200 D181 E391"
TESTS[sub_w_subx_chain]="7001 7200 7402 7600 9442 9381"
TESTS[exg_dn_an]="7011 41F9 0000 2222 C148"
TESTS[push_pop_a0]="41F9 0000 F100 70FF 2100 4280 2218"
TESTS[dbeq_loop_50]="7031 7200 5281 0C81 0000 001E 57C8 FFF6"
TESTS[dbmi_loop_neg]="700A 223C 0000 5000 0441 2000 5BC8 FFF8"
TESTS[lsl_l_count0]="203C DEAD BEEF E188"
TESTS[asr_l_8_neg]="203C 8000 0001 E080"
TESTS[rol_l_16]="203C AABB CCDD 7210 E3B8"
TESTS[lsl_b_7]="203C FF00 FF01 EF00"
TESTS[asr_b_1_sign]="203C 0000 0080 E200"
TESTS[move_b_flags]="203C AABB CC80 1200"
TESTS[move_w_zero]="203C DEAD BEEF 303C 0000"
TESTS[add_l_an_dn]="41F9 0000 A000 20BC 0000 0005 7003 D090"
TESTS[sub_w_dn_an]="41F9 0000 A100 30FC 0010 7005 9150 3010"
TESTS[cmp_b]="203C 0000 00FF 223C 0000 0001 B001"
TESTS[cmp_w]="203C 0000 8000 223C 0000 7FFF B041"
TESTS[ori_w_mem]="41F9 0000 A200 30FC 0F0F 0050 F0F0 3010"
TESTS[andi_b_mem]="41F9 0000 A300 10FC 00AB 0210 000F 1010"
TESTS[link_neg16]="41F9 0000 B000 2E48 4E55 FFF0 7042 2B40 FFF4 4280 222D FFF4 4E5D"
TESTS[mulu_max]="203C 0000 FFFF 223C 0000 FFFF C0C1"
TESTS[divs_neg_rem]="203C FFFF FFF9 7202 81C1"
TESTS[negx_64bit]="7000 7201 4480 4081"
TESTS[cmpi_l_abs_short_eq]="41F9 0000 0DB0 20BC 5A93 2BC7 0CB8 5A93 2BC7 0DB0"
TESTS[cmpi_l_abs_short_ne]="41F9 0000 0DB0 20BC DEAD BEEF 0CB8 5A93 2BC7 0DB0"
TESTS[cmpi_bne_w_not_taken]="41F9 0000 0DB0 20BC 5A93 2BC7 0CB8 5A93 2BC7 0DB0 6600 0004 7277"
TESTS[cmpi_bne_w_taken]="41F9 0000 0DB0 20BC DEAD BEEF 0CB8 5A93 2BC7 0DB0 6600 0004 7277"
TESTS[cmpi_b_abs_short_blt]="41F9 0000 012F 10FC 0003 0C38 0004 012F 6D02 72FF"
TESTS[movem_save_modify_restore]="7042 7201 7402 7603 7804 7A05 41F9 0000 0100 43F9 0000 0200 45F9 0000 0300 47F9 0000 0400 49F9 0000 0500 4BF9 0000 0600 48F8 3FFF 0C30 70FF 4CF8 3FFF 0C30"
TESTS[bsr_l_long]="61FF 0000 0008 7222 6004 7055 4E75"
TESTS[jmp_d8_pc_dn_w]="7004 4EFB 0002 70FF 70FE 7242"
TESTS[pea_movem_stack]="41F9 0000 E000 2E48 7001 7202 7403 7604 4879 0000 C000 48E7 F000 7000 7200 7400 7600 4CDF 000F 205F"
TESTS[subq_sp_movea_write]="41F9 0000 E000 2E48 554F 204F 30BC 1234 3017"
TESTS[tst_bne_after_bsr_rts]="6108 4A40 6602 7277 6004 7000 4E75"
TESTS[tst_bne_after_jsr_an]="43FA 000C 4E91 4A40 6602 7277 6004 7000 4E75"
TESTS[save_clear_slot_restore_tst]="700C 48F8 0001 0C30 42B8 0C30 4CF8 0001 0C30 4A40"
TESTS[movec_cacr_roundtrip]="9080 08C0 001F 4E7B 0002 4E7A 0002 0800 001F"
TESTS[cache_init_sequence]="9080 08C0 001F 4E7B 0002 4E7A 0002 0800 001F F4D8 9080 4E7B 0002 4E7B 0003 203C 807F C040 4E7B 0006 203C 500F C040 4E7B 0007"
TESTS[move_l_neg_disp_a5]="4BF9 0000 F000 203C DEAD BEEF 2B40 FF40 2238 EF40"
TESTS[sr_barrier_cache_init]="46FC 2700 9080 08C0 001F 4E7B 0002 4E7A 0002 0800 001F"
TESTS[divs_word_hardfail]="203C 0000 002A 223C 0000 0005 81C1"
TESTS[divu_word_hardfail]="203C 0000 002A 223C 0000 0005 80C1"
TESTS[mull_32_hardfail]="203C 0000 0064 223C 0000 0003 4C01 0000"
TESTS[divl_32_hardfail]="203C 0000 012C 223C 0000 000A 4C41 0000"
TESTS[aslw_mem_hardfail]="41F9 0000 A000 30FC 4000 E1D0 3010"
TESTS[lsrw_mem_hardfail]="41F9 0000 A000 30FC 8001 E2D0 3010"
TESTS[rolw_mem_hardfail]="41F9 0000 A000 30FC 8001 E7D0 3010"
TESTS[ori_sr_hardfail]="007C 0700"
TESTS[andi_sr_hardfail]="027C 27FF"
TESTS[eori_sr_hardfail]="0A7C 0010"
TESTS[move_from_sr_hardfail]="40C0"
TESTS[move_to_sr_hardfail]="46FC 2500 40C0"
TESTS[divs_neg_by_neg_edge]="203C FFFF FFF1 72FD 81C1"
TESTS[divs_by_minus_one_edge]="203C FFFF FFFE 72FF 81C1"
TESTS[divs_zero_dividend_edge]="7000 7205 81C1"
TESTS[divs_overflow_edge]="203C 0001 0000 7201 81C1"
TESTS[divu_exact_edge]="203C 0000 000C 7203 80C1"
TESTS[divu_with_remainder_edge]="203C 0000 000D 7205 80C1"
TESTS[divu_overflow_edge]="203C 0001 0000 7201 80C1"
TESTS[mull_unsigned_32]="203C 0001 0000 223C 0001 0000 4C01 0000"
TESTS[mull_signed_32]="203C FFFF FFFF 223C 0000 0002 4C01 0800"
TESTS[divl_unsigned_32]="203C 0000 012C 223C 0000 000A 4C41 0000"
TESTS[divl_signed_32]="203C FFFF FFF6 223C 0000 0003 4C41 0800"
TESTS[asrw_mem_edge]="41F9 0000 A000 30FC 8001 E0D0 3010"
TESTS[roxlw_mem_edge]="41F9 0000 A000 30FC 0001 003C 0010 E5D0 3010"
TESTS[roxrw_mem_edge]="41F9 0000 A000 30FC 8000 003C 0010 E4D0 3010"
TESTS[abcd_99_plus_01_edge]="7000 0000 0099 7201 C101"
TESTS[sbcd_with_x_edge]="003C 0010 7000 7201 8101"
TESTS[nbcd_99_edge]="7000 0000 0099 4800"
TESTS[bfextu_reg_edge]="203C ABCD EF01 E9C0 0200"
TESTS[bfexts_reg_edge]="203C ABCD EF01 EBC0 0200"
TESTS[bfffo_reg_edge]="203C 0000 0100 EDC0 0200"
TESTS[bfset_reg_edge]="203C FF00 00FF EEC0 0208"
TESTS[bfclr_reg_edge]="203C FFFF FFFF ECC0 0208"
TESTS[bfchg_reg_edge]="203C FF00 FF00 EAC0 0208"
TESTS[bftst_reg_edge]="203C 8000 0000 E8C0 0008"
TESTS[bfins_reg_edge]="7042 203C FFFF 0000 EFC0 0200"
TESTS[pack_dn_edge]="203C 0000 1234 8140 0000"
TESTS[unpk_dn_edge]="203C 0000 0012 8180 0000"
TESTS[movep_l_roundtrip]="41F9 0000 9100 203C 1234 5678 01C8 0000 1210 1428 0002 1628 0004 1828 0006"
TESTS[sr_ops_combo]="46FC 2700 007C 0010 027C F7FF 0A7C 0004 40C0"
TESTS[moves_write_read]="41F9 0000 A000 203C DEAD BEEF 0E90 0800 2010"
TESTS[adda_w_cov]="41F9 0000 1000 D0FC 0500"
TESTS[adda_l_cov]="41F9 0000 1000 D1FC 0000 0500"
TESTS[adda_w_neg_cov]="41F9 0000 1000 D0FC FF00"
TESTS[eori_ccr_cov]="003C 001F 0A3C 0010"
TESTS[rtr_cov]="41F9 0000 E000 2E48 610C 40C1 6008 4E71 4E71 4E71 3F3C 0010 4E77"
TESTS[mvr2usp_cov]="41F9 0000 1234 4E60 4E69 2009"
TESTS[move_b_d16_an_cov]="41F9 0000 A000 117C 0042 0010 1028 0010"
TESTS[move_w_d16_an_cov]="41F9 0000 A000 317C 1234 0010 3028 0010"
TESTS[move_l_d16_an_cov]="41F9 0000 A000 217C DEAD BEEF 0010 2028 0010"
TESTS[move_b_idx_cov]="41F9 0000 A000 117C 0042 0004 7204 1030 1000"
TESTS[move_l_idx_scale_cov]="41F9 0000 A000 217C DEAD BEEF 0008 7202 2030 1400"
TESTS[move_l_pc_rel_cov]="203A 0002 4E71 4E71"
TESTS[move_l_abs_w_cov]="21FC CAFE BABE 0A00 2038 0A00"
TESTS[move_l_abs_l_cov]="23FC DEAD BEEF 0000 A100 2039 0000 A100"
TESTS[predec_postinc_cov]="41F9 0000 A010 3F3C 1234 3F3C 5678 43F9 0000 A00C 3219 3419"
TESTS[imm_to_mem_b_cov]="41F9 0000 A000 10FC 00AB 1010"
TESTS[imm_to_mem_w_cov]="41F9 0000 A000 30FC CAFE 3010"
TESTS[imm_to_mem_l_cov]="41F9 0000 A000 20BC DEAD BEEF 2010"
TESTS[add_b_overflow_cov]="103C 007F 0600 0001"
TESTS[sub_w_borrow_cov]="303C 0000 0440 0001"
TESTS[cmp_l_equal_cov]="203C 1234 5678 0C80 1234 5678"
TESTS[and_l_zero_cov]="203C FFFF FFFF 0280 0000 0000"
TESTS[or_l_allones_cov]="7000 0080 FFFF FFFF"
TESTS[eor_self_cov]="203C ABCD EF01 B180"
TESTS[neg_b_overflow_cov]="103C 0080 4400"
TESTS[not_b_cov]="103C 00AA 4600"
TESTS[odd_addr_cov]="41F9 0000 A001 20BC CAFE BABE 41F9 0000 A001 2010"
TESTS[a7_byte_postinc_cov]="41F9 0000 E000 2E48 3F3C ABCD 101F"

# --- ADDITIONAL OPCODE COVERAGE VECTORS ---
# CHK.W: check register against upper bound (in-range = no trap)
# MOVEQ #10,D0; MOVEQ #20,D1; CHK.W D1,D0
TESTS[chk_w_in_range]="7008 7214 4181"
# CHK.W zero: D0=0 against D1=100
TESTS[chk_w_zero]="7000 7264 4181"
# CHK.W equal: D0=D1=50
TESTS[chk_w_equal]="7032 7232 4181"

# SBCD borrow chain: 0x00 - 0x01 with X=0 → 0x99, borrow
# ANDI #$EF,CCR; MOVEQ #0,D0; MOVEQ #1,D1; SBCD D1,D0
TESTS[sbcd_borrow_chain]="023C 00EF 7000 7201 8101"
# SBCD zero: 0-0 with X=0 → 0
TESTS[sbcd_zero_zero]="023C 00EF 7000 7200 8101"

# NBCD zero with X=0: NBCD of 0 → 0, no borrow
TESTS[nbcd_zero_no_x]="023C 00EF 7000 4800"
# NBCD with X=1: NBCD of 0 → 0x99, borrow
TESTS[nbcd_with_x]="003C 0010 7000 4800"

# BFINS: insert D0 low 8 bits into D1{0:8}
# MOVE.L #$AB,D0; CLR.L D1; BFINS D0,D1{0:8}
TESTS[bfins_low8]="203C 0000 00AB 4281 EFC1 0008"
# BFINS: insert D0 into D1{16:8} (mid-field)
TESTS[bfins_mid8]="203C 0000 00CD 4281 EFC1 0410"

# MOVEC VBR roundtrip: write then read VBR
# MOVE.L #$12340000,D0; MOVEC D0,VBR; MOVEC VBR,D1
TESTS[movec_vbr_roundtrip]="203C 1234 0000 4E7B 0801 4E7A 1801"
# MOVEC SFC: write SFC=5, read back
# MOVEQ #5,D0; MOVEC D0,SFC; MOVEC SFC,D1
TESTS[movec_sfc_roundtrip]="7005 4E7B 0000 4E7A 1000"
# MOVEC DFC: write DFC=3, read back
TESTS[movec_dfc_roundtrip]="7003 4E7B 0001 4E7A 1001"

# MULL unsigned 64-bit: D0 * D1 → D2:D3 (64-bit result)
# MOVE.L #$FFFFFFFF,D0; MOVE.L #2,D1; MULL.L D0,D2:D3
# MULL encoding: 4C00 + EA(D0) + ext_word (D3=Dl bits15-12, D2=Dh bits2-0, unsigned=0, 64=0x400)
# ext word: 0011_0_0_00000_010 = 0x3402
TESTS[mull_u64]="203C FFFF FFFF 223C 0000 0002 4C01 3402"
# MULL signed 32-bit: -1 * -1 → 1
# MOVE.L #$FFFFFFFF,D0; MOVE.L #$FFFFFFFF,D1; MULL.L D0,D1 (signed 32)
# ext word: 0001_1_0_00000_000 = 0x1800 — wait, let me recalc
# ext word: bit11=signed(1), bit10=64(0), bits15-12=Dl(1), bits2-0=Dh(x)
# = 0001_1_0_00000_000 = 0x1800
TESTS[mull_s32_neg]="203C FFFF FFFF 223C FFFF FFFF 4C00 1800"

# DIVL unsigned 32-bit: 100 / 7 → quot=14, rem=2
# MOVE.L #100,D0; MOVE.L #7,D1; DIVL.L D1,D2:D0
# DIVL encoding: 4C41 (EA=D1) + ext word
# ext word: bits15-12=Dq(0), bit11=signed(0), bit10=32bit(0), bits2-0=Dr(2)
# = 0000_0_0_00000_010 = 0x0002
TESTS[divl_u32_rem]="203C 0000 0064 223C 0000 0007 4C41 0002"
# DIVL signed: -100 / 7 → quot=-14, rem=-2
TESTS[divl_s32_neg]="203C FFFF FF9C 223C 0000 0007 4C41 0802"
# DIVUL.L D1,D2:D0 — max unsigned: 0xFFFFFFFF / 16 = 0x0FFFFFFF rem 15
TESTS[divl_u32_max]="203C FFFF FFFF 223C 0000 0010 4C41 0002"
# DIVSL.L D1,D2:D0 — negative divisor: 100 / -7 = -14 rem 2
TESTS[divl_s32_neg_divisor]="203C 0000 0064 223C FFFF FFF9 4C41 0802"
# MULSL.L D1,D3:D2 — 64-bit signed negative: -100 * 1000 = -100000
TESTS[mull_s64_neg]="243C FFFF FF9C 223C 0000 03E8 4C01 2C03"
# DIVUL.L D1,D0:D0 — same Dq and Dr (remainder discarded): 100/7=14
TESTS[divl_same_dq_dr]="203C 0000 0064 223C 0000 0007 4C41 0000"
# DIVUL.L D1,D3:D2 — 64-bit unsigned: 0x100000064 / 7 = 0x24924932 rem 6
TESTS[divl_u64]="243C 0000 0064 263C 0000 0001 223C 0000 0007 4C41 2403"
# DIVSL.L D1,D3:D2 — 64-bit signed: -100 / 7 = -14 rem -2
TESTS[divl_s64]="243C FFFF FF9C 263C FFFF FFFF 223C 0000 0007 4C41 2C03"
# BFINS D0,D1{4:8} — insert 0xA5 at offset 4 width 8 into cleared D1
TESTS[bfins_dreg_imm]="203C 0000 00A5 4281 EFC1 0108"
# BFINS D0,D1{8:4} — insert 0xF at offset 8 width 4
TESTS[bfins_dreg_narrow]="203C 0000 000F 2200 EFC1 0204"

# RTR: pop CCR + PC from stack — test via BSR/RTR pair
# Setup flags: ORI #$1F,CCR (set all flags)
# BSR.W +4 (push PC); RTR pops CCR (from stack) + PC (from stack)
# This is complex — RTR needs a proper stack frame. Let me use a simpler pattern:
# Push known CCR value + return address onto stack, then RTR
# MOVE.L #<return_addr>,-(SP); MOVE.W #$001F,-(SP); RTR
# But we don't know the return address... skip RTR for now.

# STOP: can't easily test since it halts. The harness USES STOP #$2700 to end.
# We implicitly test STOP in every vector.

# ---- FUZZ VECTORS (auto-generated, seed=0xDEADBEEF) ----
# ALU chain: and.l d4,d3; sub.l d4,d3
TESTS[fuzz_alu_0]="C684 9684"
# Shift chain: asr.l #8,d3; asr.l #3,d3; rol.l #7,d3
TESTS[fuzz_shift_0]="E083 E683 EF9B"
# Bit ops: bchg #17,d4; bchg #25,d4; bclr #0,d4; bchg #31,d4
TESTS[fuzz_bitops_0]="0844 0011 0844 0019 0884 0000 0844 001F"
# Mul/Div: muls.w d5,d3; divs.w d5,d3
TESTS[fuzz_muldiv_0]="C7C5 87C5"
# Ext/Swap: tst.l d0; ext.l d0; not.l d0
TESTS[fuzz_extswap_0]="4A80 48C0 4680"
# Addx/Subx: ori #$10,ccr; subx.l d5,d2; negx.l d2
TESTS[fuzz_addxsubx_0]="003C 0010 9585 4082"
# Mem roundtrip: move.l d3,(60,a1); not.l d3; move.l (60,a1),d0; cmp.l d3,d0
TESTS[fuzz_memrt_0]="2343 003C 4683 2029 003C B083"
# Exg chain: exg d4,d1; exg d1,d5; exg d2,d3; tst.l d1
TESTS[fuzz_exg_0]="C941 C345 C543 4A81"
# Mixed ALU+Shift: or.l d1,d0; swap d0; sub.l d3,d0
TESTS[fuzz_mixed_0]="8081 4840 9083"
# Flag stress: ori #$0,ccr; tst.l d0
TESTS[fuzz_flags_0]="003C 0000 4A80"
# ALU chain: and.l d5,d5; add.l d4,d5
TESTS[fuzz_alu_1]="CA85 DA84"
# Shift chain: lsr.l #3,d1; asr.l #5,d1
TESTS[fuzz_shift_1]="E689 EA81"
# Bit ops: bset #5,d5; bchg #8,d5
TESTS[fuzz_bitops_1]="08C5 0005 0845 0008"
# Mul/Div: mulu.w d5,d1; divu.w d5,d1
TESTS[fuzz_muldiv_1]="C2C5 82C5"
# Ext/Swap: swap d2; neg.l d2; ext.w d2; tst.l d2
TESTS[fuzz_extswap_1]="4842 4482 4882 4A82"
# Addx/Subx: andi #$EF,ccr; subx.l d5,d2; negx.l d2
TESTS[fuzz_addxsubx_1]="023C 00EF 9585 4082"
# Mem roundtrip: move.l d3,(212,a1); not.l d3; move.l (212,a1),d0; cmp.l d3,d0
TESTS[fuzz_memrt_1]="2343 00D4 4683 2029 00D4 B083"
# Exg chain: exg d5,d2; exg d4,d3; tst.l d4
TESTS[fuzz_exg_1]="CB42 C943 4A84"
# Mixed ALU+Shift: lsl.l #1,d2; and.l d0,d2; lsl.l #2,d2
TESTS[fuzz_mixed_1]="E38A C480 E58A"
# Flag stress: ori #$11,ccr; addx.l d4,d0; tst.l d0
TESTS[fuzz_flags_1]="003C 0011 D184 4A80"
# ALU chain: or.l d1,d3; eor.l d1,d3
TESTS[fuzz_alu_2]="8681 B383"
# Shift chain: ror.l #6,d3; asr.l #1,d3; asl.l #8,d3
TESTS[fuzz_shift_2]="EC9B E283 E183"
# Bit ops: bset #7,d1; bchg #21,d1; bset #1,d1
TESTS[fuzz_bitops_2]="08C1 0007 0841 0015 08C1 0001"
# Mul/Div: muls.w d4,d2; divu.w d4,d2
TESTS[fuzz_muldiv_2]="C5C4 84C4"
# Ext/Swap: tst.l d0; ext.l d0; swap d0; not.l d0
TESTS[fuzz_extswap_2]="4A80 48C0 4840 4680"
# Addx/Subx: andi #$EF,ccr; addx.l d5,d0; negx.l d0
TESTS[fuzz_addxsubx_2]="023C 00EF D185 4080"
# Mem roundtrip: move.l d2,(60,a2); not.l d2; move.l (60,a2),d3; cmp.l d2,d3
TESTS[fuzz_memrt_2]="2542 003C 4682 262A 003C B682"
# Exg chain: exg d1,d3; exg d0,d2; tst.l d5
TESTS[fuzz_exg_2]="C343 C142 4A85"
# Mixed ALU+Shift: lsr.l #7,d3; swap d3; or.l d5,d3; neg.l d3; or.l d5,d3
TESTS[fuzz_mixed_2]="EE8B 4843 8685 4483 8685"
# Flag stress: ori #$f,ccr; addx.l d4,d1; tst.l d1
TESTS[fuzz_flags_2]="003C 000F D384 4A81"
# ALU chain: and.l d3,d1; sub.l d5,d1
TESTS[fuzz_alu_3]="C283 9285"
# Shift chain: asl.l #8,d5; ror.l #8,d5
TESTS[fuzz_shift_3]="E185 E09D"
# Bit ops: bset #31,d1; bset #0,d1; bclr #24,d1; bset #8,d1
TESTS[fuzz_bitops_3]="08C1 001F 08C1 0000 0881 0018 08C1 0008"
# Mul/Div: muls.w d4,d2; divu.w d4,d2
TESTS[fuzz_muldiv_3]="C5C4 84C4"
# Ext/Swap: ext.l d3; swap d3; tst.l d3
TESTS[fuzz_extswap_3]="48C3 4843 4A83"
# Addx/Subx: ori #$10,ccr; subx.l d4,d0; negx.l d0
TESTS[fuzz_addxsubx_3]="003C 0010 9184 4080"
# Mem roundtrip: move.l d2,(244,a1); not.l d2; move.l (244,a1),d3; cmp.l d2,d3
TESTS[fuzz_memrt_3]="2342 00F4 4682 2629 00F4 B682"
# Exg chain: exg d1,d3; exg d5,d4; tst.l d2
TESTS[fuzz_exg_3]="C343 CB44 4A82"
# Mixed ALU+Shift: or.l d4,d3; sub.l d3,d3; lsl.l #6,d3; swap d3; sub.l d3,d3; add.l d2,d3
TESTS[fuzz_mixed_3]="8684 9683 ED8B 4843 9683 D682"
# Flag stress: ori #$13,ccr; addx.l d4,d0; tst.l d0
TESTS[fuzz_flags_3]="003C 0013 D184 4A80"
# ALU chain: add.l d2,d2; sub.l d5,d2; and.l d5,d2
TESTS[fuzz_alu_4]="D482 9485 C485"
# Shift chain: rol.l #7,d2; asr.l #1,d2; asl.l #7,d2
TESTS[fuzz_shift_4]="EF9A E282 EF82"
# Bit ops: bclr #13,d3; bset #28,d3; bchg #10,d3
TESTS[fuzz_bitops_4]="0883 000D 08C3 001C 0843 000A"
# Mul/Div: muls.w d5,d2; divs.w d5,d2
TESTS[fuzz_muldiv_4]="C5C5 85C5"
# Ext/Swap: ext.w d5; tst.l d5; ext.l d5; tst.l d5
TESTS[fuzz_extswap_4]="4885 4A85 48C5 4A85"
# Addx/Subx: ori #$10,ccr; subx.l d4,d1
TESTS[fuzz_addxsubx_4]="003C 0010 9384"
# Mem roundtrip: move.l d0,(8,a0); not.l d0; move.l (8,a0),d1; cmp.l d0,d1
TESTS[fuzz_memrt_4]="2140 0008 4680 2228 0008 B280"
# Exg chain: exg d4,d0; tst.l d1
TESTS[fuzz_exg_4]="C940 4A81"
# Mixed ALU+Shift: swap d1; or.l d1,d1; neg.l d1; lsr.l #3,d1; neg.l d1
TESTS[fuzz_mixed_4]="4841 8281 4481 E689 4481"
# Flag stress: ori #$1a,ccr; addx.l d4,d2; tst.l d2
TESTS[fuzz_flags_4]="003C 001A D584 4A82"


declare -A SENTINEL_A6
declare -A INIT_REGS   # optional initial register state (D0-D7 A0-A7 [SR])
# Fuzz vector initial register states
INIT_REGS[fuzz_alu_0]="8878FDF6 80000000 00000000 637A51D3 7FFFFFFF 00000000 000000FF FFFFFFFF 0038D748 007BBF88 003C4A38 0023044C 003974BC 00072334 00000000 007EFF00"
INIT_REGS[fuzz_shift_0]="7FFFFFFF FFFFFFFF 0000FFFF 000000FF FFFFFFFF 80000000 0000FFFF 6F01C50E 00124FD8 005EB90C 0032C4F4 006E747C 005771AC 002B43C0 00000000 007EFF00"
INIT_REGS[fuzz_bitops_0]="FFFFFFFF F567E951 3D5E6FD4 000000FF 10F5BF4D 7FFFFFFF 11D9AF43 75616BFD 001AAB38 00330250 0075C460 005CAF44 00439394 000B9E84 00000000 007EFF00"
INIT_REGS[fuzz_muldiv_0]="000000FF 00000000 FFFFFFFF 80000000 0000008E 0000760C CC333AE3 5CB9710E 0044EBD0 005ABBFC 00695CC8 007CE2A4 006C5B90 00733658 00000000 007EFF00"
INIT_REGS[fuzz_extswap_0]="7FFFFFFF A0635EFF 000000FF 80000000 00000000 00000000 7FFFFFFF 000000C5 006FADBC 006CCE54 00631828 00753CB8 000B9958 00570EEC 00000000 007EFF00"
INIT_REGS[fuzz_addxsubx_0]="7FFFFFFF 7FFFFFFF 00000000 000000F3 00000000 00000000 0000FFFF 80000000 0020F168 00580528 001E44E8 002F4F34 002C2B74 002D03EC 00000000 007EFF00"
INIT_REGS[fuzz_memrt_0]="7FFFFFFF BD92BE4B 00000000 FFFFFFFF A287EB05 55E7D610 000000FF 0000FFFF 00414E60 0051A0B8 007394F8 00694E60 0034DD04 0035BE6C 00000000 007EFF00"
INIT_REGS[fuzz_exg_0]="7FFFFFFF 00000000 000000FF 475A6474 0000008F FFFFFFFF AA6BA628 032BD4ED 002651D4 003F6728 003EFB14 0007632C 0014D140 005B2EBC 00000000 007EFF00"
INIT_REGS[fuzz_mixed_0]="00000000 0000FFFF 00000004 00000027 80000000 000000B8 DE82A945 0000FFFF 00341FB8 0002FB2C 001CBAC4 0056F5D0 003C7BDC 003F7804 00000000 007EFF00"
INIT_REGS[fuzz_flags_0]="80000000 80000000 000000C0 F311B6E1 0000FFFF 7FFFFFFF C91E5274 FFFFFFFF 0029D134 0063A530 006C413C 001FD270 0012EA80 0070F5E0 00000000 007EFF00"
INIT_REGS[fuzz_alu_1]="E8EE138F FFFFFFFF 80000000 0000FFFF 0000FFFF 80000000 E3BC7C50 59D6AAA6 00154C7C 0004F11C 002CAFE4 005FE0A8 000C3530 006C2ED8 00000000 007EFF00"
INIT_REGS[fuzz_shift_1]="00000000 00000000 FFFFFFFF 5448D078 0000FFFF FFFFFFFF FFFFFFFF B3497EB3 00590C00 0015C96C 00316E30 00378A68 003B0BF4 0026E3A0 00000000 007EFF00"
INIT_REGS[fuzz_bitops_1]="FFFFFFFF 00000000 00000000 0000007E 0000007D 4AB3775A FB60C0C3 0000FFFF 0009F9BC 003737C0 0044E830 0024A9C0 00339F64 00233F90 00000000 007EFF00"
INIT_REGS[fuzz_muldiv_1]="AEECBF29 80000000 80000000 FFFFFFFF 1BE0D930 0000B5C7 7FFFFFFF 00000000 003F05F0 0057A43C 00459DBC 000BB2C8 007ADE84 003AA810 00000000 007EFF00"
INIT_REGS[fuzz_extswap_1]="3EFDD522 00000036 80000000 6AF18701 80000000 FFFFFFFF 000000FF 0000FFFF 003AFDF8 00507248 0049E580 005FC27C 0015E3F0 00301E1C 00000000 007EFF00"
INIT_REGS[fuzz_addxsubx_1]="7FFFFFFF 80000000 00000062 0000004B 7FFFFFFF 87040427 7FFFFFFF A35154CE 00366F60 001A16F8 00724F4C 003DF7AC 004B7B40 0010FA88 00000000 007EFF00"
INIT_REGS[fuzz_memrt_1]="C245E710 3B4DA9EF 241620CC 7FFFFFFF FFFFFFFF 00000019 9DD3E198 00000000 001FD5F8 00142300 0079C99C 001DADC4 00585FB0 007A0C68 00000000 007EFF00"
INIT_REGS[fuzz_exg_1]="00000000 0000FFFF 7FFFFFFF 00000000 0000FFFF 0000FFFF 00000000 000000BC 0031A1B8 003EF580 00459FE4 0006BD90 002F6B80 0009D460 00000000 007EFF00"
INIT_REGS[fuzz_mixed_1]="00000000 80000000 7FFFFFFF 00000000 0000FFFF 00000000 000000FF 80000000 0078F5DC 001F065C 0010F264 0032D7D0 005F0B0C 003E697C 00000000 007EFF00"
INIT_REGS[fuzz_flags_1]="C04533B9 00000000 4689409F 00000005 00000000 0B795496 CEF18F0E FACF15E9 00124C74 00566848 0062A114 002740D0 005BC32C 002C2150 00000000 007EFF00"
INIT_REGS[fuzz_alu_2]="2BB84DD1 7FFFFFFF 0000FFFF B88D9738 00000000 FFFFFFFF 00000041 80000000 00361470 001D1ACC 007E2F9C 003AE218 0040B090 00585EB0 00000000 007EFF00"
INIT_REGS[fuzz_shift_2]="0000FFFF BE83F4AB 00000000 80000000 00000000 80000000 80000000 00000000 0077A1E4 00247BD8 004BED8C 00286964 002BADC4 007D41D8 00000000 007EFF00"
INIT_REGS[fuzz_bitops_2]="80000000 FFFFFFFF FFFFFFFF 00000023 80000000 7FFFFFFF 0000FFFF 00000084 005B7F3C 0005EE58 00781E7C 0024174C 000AA384 007B0B00 00000000 007EFF00"
INIT_REGS[fuzz_muldiv_2]="0000FFFF 000000D4 595124DA 7FFFFFFF 0000D55A 0000007C 2E24CEC1 4C0F0F27 00007C30 003F3944 00351CB0 003656C0 003F1824 005E60B0 00000000 007EFF00"
INIT_REGS[fuzz_extswap_2]="80000000 000000FF 4FC1F43B F26435A8 0000FFFF 7FFFFFFF 00000000 000000FF 0014FBA8 005800A0 0008C620 00080578 006D2B98 007422E8 00000000 007EFF00"
INIT_REGS[fuzz_addxsubx_2]="02C481F3 6A4AE5AD 80000000 95EAD6BA 7FFFFFFF 0000FFFF 677BE43B 9A6E70E5 000479F0 006C80F8 00104B8C 0028EC7C 006CE61C 0061BA50 00000000 007EFF00"
INIT_REGS[fuzz_memrt_2]="7FFFFFFF 000000FF FFFFFFFF 000000BC 7FFFFFFF 00000000 A08A2385 AD0C4765 0020B96C 00555408 00196114 004B94E4 006E5368 006ACC94 00000000 007EFF00"
INIT_REGS[fuzz_exg_2]="00000000 00000000 80000000 07465B1C 00000000 0000FFFF 00000085 DAA4134D 002AF0EC 00639414 0024E28C 0076C624 00540F70 0025AF44 00000000 007EFF00"
INIT_REGS[fuzz_mixed_2]="00000000 000000FF FFFFFFFF 0000FFFF 7FFFFFFF 03465513 221C64EA 80000000 007B34B4 0039DAA4 004F6F20 00114818 005A2644 00797148 00000000 007EFF00"
INIT_REGS[fuzz_flags_2]="0000FFFF 00000000 C459EA3A 80000000 00000000 1136C00B A6B7D7DE 00000000 004935D0 007DC188 00458580 002328E4 003E2864 003309CC 00000000 007EFF00"
INIT_REGS[fuzz_alu_3]="00000067 000000B7 7FFFFFFF 9B0B8017 0000FFFF 80000000 7FFFFFFF D03FF5DA 0044C65C 0072C5E0 0048C79C 006E0518 0008DCA0 0070FEDC 00000000 007EFF00"
INIT_REGS[fuzz_shift_3]="0BDBFE50 FFFFFFFF 00000000 7FFFFFFF 47D43365 E08347E3 7FFFFFFF 927EE333 00689DB8 000FAA94 0067413C 000B56DC 0016EE04 0040835C 00000000 007EFF00"
INIT_REGS[fuzz_bitops_3]="88596DE9 FFFFFFFF 7FFFFFFF FFFFFFFF 7FFFFFFF F86E8FA7 BA114E62 7FFFFFFF 006B6D30 000C73C4 003BE184 0002C488 004BAF8C 000E54E0 00000000 007EFF00"
INIT_REGS[fuzz_muldiv_3]="80000000 000000FF 00000000 7FFFFFFF 00005110 7AB04BDC 3ECFA952 BC405280 00276380 003D79B4 002DF3C0 006257D4 004A3988 00261688 00000000 007EFF00"
INIT_REGS[fuzz_extswap_3]="0000FFFF 00000036 FFFFFFFF 7FFFFFFF 00000000 0000FFFF 000000FF 00000055 004335E0 0002A388 007287C8 00584D40 00339710 00520748 00000000 007EFF00"
INIT_REGS[fuzz_addxsubx_3]="FFFFFFFF FFFFFFFF 5AB0C8C7 0000FFFF FFFFFFFF 0000007B 00000000 FFFFFFFF 00043CDC 000A0C4C 00050074 00132CCC 00135EA4 00761FA4 00000000 007EFF00"
INIT_REGS[fuzz_memrt_3]="000000FF 092D6826 00000000 1B613295 FFFFFFFF 000000FF 39374372 00000000 00216324 007097F8 006063B8 007D5844 00112DB4 000BD0E8 00000000 007EFF00"
INIT_REGS[fuzz_exg_3]="897A1A19 80000000 80000000 7FFFFFFF 00000000 7FFFFFFF 58FD46B7 80000000 0017F2C0 00627BF8 005773EC 0005FFBC 001F4DAC 005CF8E8 00000000 007EFF00"
INIT_REGS[fuzz_mixed_3]="FFFFFFFF 00000000 000000FF 00000000 7FFFFFFF 0000FFFF 000000D5 7FFFFFFF 00031B6C 007706C4 000EB344 0011D03C 0004937C 0064B398 00000000 007EFF00"
INIT_REGS[fuzz_flags_3]="93FDE8D8 7FFFFFFF 7FFFFFFF 000000FF FFFFFFFF 00000000 0000FFFF E3976C1E 005F2C8C 001AA328 00402EB0 0079B354 003C55A4 004E5DC4 00000000 007EFF00"
INIT_REGS[fuzz_alu_4]="E814971D 00000022 FFFFFFFF 00000000 7FFFFFFF 00000001 0B8E2A96 D15F0551 004CE140 005C15AC 002152BC 00078ADC 00138CE0 001222EC 00000000 007EFF00"
INIT_REGS[fuzz_shift_4]="80000000 00000000 FFFFFFFF 000000E9 DCFDF7CF 00000000 CDD2AB32 0000FFFF 00439B04 002F7E14 006D7A70 0061AE70 0077845C 00673AB8 00000000 007EFF00"
INIT_REGS[fuzz_bitops_4]="80000000 4C01E224 00000000 00000000 000000FF 1ABCB699 00000000 000000FF 006DEDC8 00458F14 005E8554 00404918 00393DDC 0030DC8C 00000000 007EFF00"
INIT_REGS[fuzz_muldiv_4]="000000FF 000000FF 7FFFFFFF 000000B0 FFFFFFFF 0000B35A 00000000 E8863BAD 000BB0E8 003FDE68 0056BB90 003151B8 001B5728 0070AC64 00000000 007EFF00"
INIT_REGS[fuzz_extswap_4]="80000000 6361AA64 000000FF 0000FFFF 12047320 FFFFFFFF 80000000 7FFFFFFF 0044A034 0065CB9C 004A336C 0079B13C 0068E7C0 00074BD0 00000000 007EFF00"
INIT_REGS[fuzz_addxsubx_4]="23D01E2E 00000000 F7F440AC 7FFFFFFF 00000000 0000FFFF 000000FF CE808892 00214B28 004B1844 00144DC0 000F502C 002972A8 002DF22C 00000000 007EFF00"
INIT_REGS[fuzz_memrt_4]="000000FF 6B78D8FC 0000FFFF 0000FFFF 80000000 FA7FE2E8 000000FF FFFFFFFF 006F9C80 00347EAC 00527498 00467C38 003C6564 0014C494 00000000 007EFF00"
INIT_REGS[fuzz_exg_4]="000000FF 80000000 000000FF 68651AA6 80000000 000000FF FFFFFFFF 7FFFFFFF 0028E76C 001B17E4 003806E0 004FF650 005A19BC 00313940 00000000 007EFF00"
INIT_REGS[fuzz_mixed_4]="80000000 0000FFFF 80000000 9BA96951 7FFFFFFF 80000000 FFFFFFFF 7FFFFFFF 0005ADF8 0043FF50 0048CF98 00464810 0025684C 00195E8C 00000000 007EFF00"
INIT_REGS[fuzz_flags_4]="FFFFFFFF 00000000 000000FF 0000002F 00000000 35FDF202 FFFFFFFF 00000000 0025B20C 003BEC14 00482078 00628CFC 005D71F0 0057BB88 00000000 007EFF00"
SENTINEL_A6[nop]="a601005a"
SENTINEL_A6[nop_triplet]="a60100c2"
SENTINEL_A6[roxl_x_propagation]="a60100c3"
SENTINEL_A6[roxr_x_propagation]="a60100c4"
SENTINEL_A6[roxl_count_2]="a60100c5"
SENTINEL_A6[asl_overflow]="a60100c6"
SENTINEL_A6[lsr_count_32]="a60100c7"
SENTINEL_A6[asr_count_0]="a60100c8"
SENTINEL_A6[ror_word]="a60100c9"
SENTINEL_A6[rol_word]="a60100ca"
SENTINEL_A6[btst_reg_high_bit]="a60100cb"
SENTINEL_A6[muls_neg_neg]="a60100cc"
SENTINEL_A6[muls_zero]="a60100cd"
SENTINEL_A6[divs_neg_neg]="a60100ce"
SENTINEL_A6[divs_overflow]="a60100cf"
SENTINEL_A6[abcd_basic]="a60100d0"
SENTINEL_A6[sbcd_basic]="a60100d1"
SENTINEL_A6[negx_with_x]="a60100d2"
SENTINEL_A6[negx_zero]="a60100d3"
SENTINEL_A6[addx_basic]="a60100d4"
SENTINEL_A6[subx_basic]="a60100d5"
SENTINEL_A6[ext_word]="a60100d6"
SENTINEL_A6[ext_long]="a60100d7"
SENTINEL_A6[move_to_mem_and_back]="a60100d8"
SENTINEL_A6[movem_predec_postinc]="a60100d9"
SENTINEL_A6[movem_predec_mixed_order]="a60100e8"
SENTINEL_A6[addx_chain]="a60100da"
SENTINEL_A6[flag_chain_xzn]="a60100db"
SENTINEL_A6[shift_chain]="a60100dc"
SENTINEL_A6[roxl_reg_count_32]="a60100ec"
SENTINEL_A6[roxl_reg_count_33]="a60100ed"
SENTINEL_A6[roxr_reg_count_33]="a60100ea"
SENTINEL_A6[roxr_reg_count_32]="a60100eb"
SENTINEL_A6[roxr_reg_count_0]="a60100ee"
SENTINEL_A6[roxl_reg_count_63]="a60100ef"
SENTINEL_A6[roxr_reg_count_63]="a60100f0"
SENTINEL_A6[roxr_roxl_chain_x]="a60100f1"
SENTINEL_A6[roxl_lsr_chain_x]="a60100f2"
SENTINEL_A6[mulu_large]="a60100dd"
SENTINEL_A6[divu_remainder]="a60100de"
SENTINEL_A6[abcd_with_carry]="a60100df"
SENTINEL_A6[nbcd_basic]="a60100e0"
SENTINEL_A6[bsr_rts]="a60100e1"
SENTINEL_A6[link_unlk]="a60100e2"
SENTINEL_A6[indexed_addr_mode]="a60100e3"
SENTINEL_A6[byte_postinc]="a60100e4"
SENTINEL_A6[cmpm_equal]="a60100e5"
SENTINEL_A6[move_sr_roundtrip]="a60100e6"
SENTINEL_A6[dbra_loop_100]="a6010100"
SENTINEL_A6[move]="a6010001"
SENTINEL_A6[moveq_signext]="a601007a"
SENTINEL_A6[alu]="a6010002"
SENTINEL_A6[alu_overflow]="a6010031"
SENTINEL_A6[addi_subi_long]="a6010043"
SENTINEL_A6[addi_subi_long_wrap]="a60100a2"
SENTINEL_A6[addi_subi_word]="a6010044"
SENTINEL_A6[addi_subi_word_wrap]="a6010073"
SENTINEL_A6[addi_subi_byte]="a6010056"
SENTINEL_A6[addi_subi_byte_wrap]="a601006f"
SENTINEL_A6[shift]="a6010003"
SENTINEL_A6[bitops]="a6010004"
SENTINEL_A6[bitops_chg]="a6010032"
SENTINEL_A6[bitops_highbit]="a601006d"
SENTINEL_A6[bitops_chg_highbit]="a6010077"
SENTINEL_A6[branch]="a6010005"
SENTINEL_A6[branch_chain]="a6010057"
SENTINEL_A6[compare]="a6010006"
SENTINEL_A6[compare_negative]="a6010033"
SENTINEL_A6[cmpi_sizes]="a6010045"
SENTINEL_A6[cmpi_sizes_zero]="a60100a4"
SENTINEL_A6[cmpi_byte_negative]="a6010071"
SENTINEL_A6[cmpi_word_negative]="a6010072"
SENTINEL_A6[cmpi_long_negative]="a6010078"
SENTINEL_A6[cmpi_beq_taken]="a601005b"
SENTINEL_A6[muldiv]="a6010007"
SENTINEL_A6[movem]="a6010008"
SENTINEL_A6[misc]="a6010009"
SENTINEL_A6[clr_sizes]="a601007b"
SENTINEL_A6[clr_byte_preserve_upper]="a60100a8"
SENTINEL_A6[clr_word_preserve_upper]="a60100a9"
SENTINEL_A6[neg_sizes]="a601007c"
SENTINEL_A6[neg_zero_sizes]="a60100a6"
SENTINEL_A6[swap_roundtrip]="a601007d"
SENTINEL_A6[flags]="a601000a"
SENTINEL_A6[flags_eori_ccr]="a601006e"
SENTINEL_A6[exg]="a601000b"
SENTINEL_A6[exg_roundtrip]="a6010034"
SENTINEL_A6[imm_logic]="a601000c"
SENTINEL_A6[imm_logic_alt]="a6010035"
SENTINEL_A6[imm_logic_byte_highbit]="a60100a3"
SENTINEL_A6[imm_logic_word]="a6010068"
SENTINEL_A6[imm_logic_long]="a601006a"
SENTINEL_A6[imm_logic_long_alt]="a6010075"
SENTINEL_A6[tst_sizes]="a601006b"
SENTINEL_A6[tst_zero]="a6010076"
SENTINEL_A6[tst_positive]="a60100a7"
SENTINEL_A6[bra_taken]="a601000d"
SENTINEL_A6[bra_w_taken]="a601003e"
SENTINEL_A6[bne_not_taken]="a601000e"
SENTINEL_A6[bne_taken]="a601000f"
SENTINEL_A6[bne_w_not_taken]="a601003f"
SENTINEL_A6[bne_w_taken]="a6010040"
SENTINEL_A6[beq_taken]="a6010010"
SENTINEL_A6[beq_not_taken]="a6010011"
SENTINEL_A6[beq_w_taken]="a6010041"
SENTINEL_A6[beq_w_not_taken]="a6010042"
SENTINEL_A6[bpl_taken]="a6010012"
SENTINEL_A6[bpl_not_taken]="a6010029"
SENTINEL_A6[bpl_w_taken]="a6010046"
SENTINEL_A6[bpl_w_not_taken]="a601005c"
SENTINEL_A6[bmi_taken]="a601002a"
SENTINEL_A6[bmi_not_taken]="a6010013"
SENTINEL_A6[bmi_w_taken]="a6010047"
SENTINEL_A6[bmi_w_not_taken]="a601005d"
SENTINEL_A6[bvc_taken]="a6010014"
SENTINEL_A6[bvc_not_taken_overflow]="a601002b"
SENTINEL_A6[bvc_w_taken]="a6010048"
SENTINEL_A6[bvc_w_not_taken_overflow]="a601005e"
SENTINEL_A6[bvs_taken_overflow]="a601002c"
SENTINEL_A6[bvs_not_taken]="a6010015"
SENTINEL_A6[bvs_w_taken_overflow]="a6010049"
SENTINEL_A6[bvs_w_not_taken]="a601005f"
SENTINEL_A6[bge_taken]="a6010016"
SENTINEL_A6[bge_not_taken]="a6010025"
SENTINEL_A6[bge_w_taken]="a601004a"
SENTINEL_A6[bge_w_not_taken]="a6010060"
SENTINEL_A6[blt_taken]="a6010026"
SENTINEL_A6[blt_not_taken]="a6010017"
SENTINEL_A6[blt_w_taken]="a601004b"
SENTINEL_A6[blt_w_not_taken]="a6010061"
SENTINEL_A6[bgt_taken]="a6010018"
SENTINEL_A6[bgt_not_taken]="a6010027"
SENTINEL_A6[bgt_w_taken]="a601004c"
SENTINEL_A6[bgt_w_not_taken]="a6010062"
SENTINEL_A6[ble_taken]="a6010028"
SENTINEL_A6[ble_not_taken]="a6010019"
SENTINEL_A6[ble_w_taken]="a601004d"
SENTINEL_A6[ble_w_not_taken]="a6010063"
SENTINEL_A6[bcc_taken]="a601001a"
SENTINEL_A6[bcc_not_taken]="a601001b"
SENTINEL_A6[bcc_w_taken]="a601004e"
SENTINEL_A6[bcc_w_not_taken]="a6010064"
SENTINEL_A6[bcs_taken]="a601001c"
SENTINEL_A6[bcs_not_taken]="a601001d"
SENTINEL_A6[bcs_w_taken]="a601004f"
SENTINEL_A6[bcs_w_not_taken]="a6010065"
SENTINEL_A6[bhi_taken]="a6010022"
SENTINEL_A6[bhi_not_taken]="a6010023"
SENTINEL_A6[bhi_w_taken]="a6010050"
SENTINEL_A6[bhi_w_not_taken]="a6010066"
SENTINEL_A6[bls_taken]="a6010024"
SENTINEL_A6[bls_not_taken]="a601002d"
SENTINEL_A6[bls_w_taken]="a6010051"
SENTINEL_A6[bls_w_not_taken]="a6010067"
SENTINEL_A6[scc_basic]="a601001e"
SENTINEL_A6[scc_eq_ne]="a601002e"
SENTINEL_A6[scc_carry]="a601002f"
SENTINEL_A6[scc_hi_ls]="a601003a"
SENTINEL_A6[scc_hi_ls_z]="a601003b"
SENTINEL_A6[scc_vc_vs]="a6010036"
SENTINEL_A6[scc_pl_mi]="a6010037"
SENTINEL_A6[scc_ge_lt]="a6010038"
SENTINEL_A6[scc_gt_le]="a6010039"
SENTINEL_A6[scc_ccr_preserve_blt]="a601007e"
SENTINEL_A6[scc_ccr_preserve_bcs]="a601007f"
SENTINEL_A6[scc_ccr_preserve_bne_not_taken]="a6010080"
SENTINEL_A6[scc_ccr_preserve_beq_taken]="a6010081"
SENTINEL_A6[quick_ops]="a601001f"
SENTINEL_A6[quick_ops_long_neg_roundtrip]="a60100a5"
SENTINEL_A6[quick_ops_word]="a6010058"
SENTINEL_A6[quick_ops_word_wrap]="a6010074"
SENTINEL_A6[quick_ops_long_wrap]="a6010079"
SENTINEL_A6[quick_ops_byte]="a6010069"
SENTINEL_A6[quick_ops_byte_wrap]="a6010070"
SENTINEL_A6[quick_ops_addr]="a601006c"
SENTINEL_A6[dbra]="a6010020"
SENTINEL_A6[dbra_not_taken]="a6010021"
SENTINEL_A6[dbra_start_minus1_branch]="a60100e7"
SENTINEL_A6[dbra_start_8000_branch]="a60100e9"
SENTINEL_A6[dbt_true_not_taken]="a6010059"
SENTINEL_A6[dbra_three_iter]="a6010030"
SENTINEL_A6[dbcc_loop_c_set]="a6010082"
SENTINEL_A6[dbcs_not_taken_c_set]="a6010083"
SENTINEL_A6[dbpl_loop_n_set]="a6010084"
SENTINEL_A6[dbmi_not_taken_n_set]="a6010085"
SENTINEL_A6[dbhi_not_taken_hi_set]="a6010086"
SENTINEL_A6[dbls_not_taken_ls_set]="a6010087"
SENTINEL_A6[dbge_not_taken_n_eq_v]="a6010088"
SENTINEL_A6[dblt_not_taken_n_ne_v]="a6010089"
SENTINEL_A6[dbgt_not_taken_gt_set]="a601008a"
SENTINEL_A6[dble_not_taken_le_set]="a601008b"
SENTINEL_A6[dbhi_false_dec_terminal_ls_set]="a601008c"
SENTINEL_A6[dbls_false_dec_terminal_hi_set]="a601008d"
SENTINEL_A6[dbge_false_dec_terminal_n_ne_v]="a601008e"
SENTINEL_A6[dblt_false_dec_terminal_n_eq_v]="a601008f"
SENTINEL_A6[dbgt_false_dec_terminal_z_set]="a6010090"
SENTINEL_A6[dble_false_dec_terminal_gt_set]="a6010091"
SENTINEL_A6[dbcc_ccr_preserve_beq_taken]="a6010092"
SENTINEL_A6[dbcc_ccr_preserve_bne_taken]="a6010093"
SENTINEL_A6[dbcc_ccr_preserve_bcs_taken]="a6010094"
SENTINEL_A6[dbcc_ccr_preserve_bvc_taken]="a6010095"
SENTINEL_A6[dbcc_ccr_preserve_bvs_taken]="a6010096"
SENTINEL_A6[dbcc_ccr_preserve_bhi_taken]="a6010097"
SENTINEL_A6[dbcc_ccr_preserve_bls_taken]="a6010098"
SENTINEL_A6[dbcc_ccr_preserve_bge_taken]="a6010099"
SENTINEL_A6[dbcc_ccr_preserve_blt_taken]="a601009a"
SENTINEL_A6[dbcc_ccr_preserve_bgt_taken]="a601009b"
SENTINEL_A6[dbcc_ccr_preserve_ble_taken]="a601009c"
SENTINEL_A6[dbvc_loop_v_set]="a6010052"
SENTINEL_A6[dbvs_loop_v_clear]="a6010053"
SENTINEL_A6[dbvc_not_taken_v_clear]="a6010054"
SENTINEL_A6[dbvs_not_taken_v_set]="a6010055"
SENTINEL_A6[dbne_loop_z_set]="a601003c"
SENTINEL_A6[dbeq_loop_z_clear]="a601003d"
SENTINEL_A6[moveq_edges]="a60100aa"
SENTINEL_A6[alu_negative_roundtrip]="a60100ab"
SENTINEL_A6[imm_logic_word_highbit]="a60100ac"
SENTINEL_A6[branch_chain_z_clear]="a60100ad"
SENTINEL_A6[branch_chain_carry_set]="a60100ae"
SENTINEL_A6[branch_chain_overflow_set]="a60100af"
SENTINEL_A6[scc_ccr_preserve_bvs_taken]="a60100b0"
SENTINEL_A6[dbra_four_iter]="a60100b1"
SENTINEL_A6[scc_ccr_preserve_bvc_taken]="a60100b2"
SENTINEL_A6[scc_ccr_preserve_bhi_taken]="a60100b3"
SENTINEL_A6[scc_ccr_preserve_bls_taken]="a60100b4"
SENTINEL_A6[dbra_five_iter]="a60100b5"
SENTINEL_A6[branch_chain_eq_then_ne]="a60100b6"
SENTINEL_A6[branch_chain_carry_clear]="a60100b7"
SENTINEL_A6[imm_logic_long_highbit]="a60100b8"
SENTINEL_A6[dbra_six_iter]="a60100b9"
SENTINEL_A6[lsl_l_count0]="a6010125"
SENTINEL_A6[asr_l_8_neg]="a6010126"
SENTINEL_A6[rol_l_16]="a6010127"
SENTINEL_A6[lsl_b_7]="a6010128"
SENTINEL_A6[asr_b_1_sign]="a6010129"
SENTINEL_A6[divs_word_hardfail]="a601014b"
SENTINEL_A6[divu_word_hardfail]="a601014c"
SENTINEL_A6[mull_32_hardfail]="a601014d"
SENTINEL_A6[divl_32_hardfail]="a601014e"
SENTINEL_A6[aslw_mem_hardfail]="a601014f"
SENTINEL_A6[lsrw_mem_hardfail]="a6010150"
SENTINEL_A6[rolw_mem_hardfail]="a6010151"
SENTINEL_A6[ori_sr_hardfail]="a6010152"
SENTINEL_A6[andi_sr_hardfail]="a6010153"
SENTINEL_A6[eori_sr_hardfail]="a6010154"
SENTINEL_A6[move_from_sr_hardfail]="a6010155"
SENTINEL_A6[move_to_sr_hardfail]="a6010156"
SENTINEL_A6[divs_neg_by_neg_edge]="a60001c0"
SENTINEL_A6[divs_by_minus_one_edge]="a60001c1"
SENTINEL_A6[divs_zero_dividend_edge]="a60001c2"
SENTINEL_A6[divs_overflow_edge]="a60001c3"
SENTINEL_A6[divu_exact_edge]="a60001c4"
SENTINEL_A6[divu_with_remainder_edge]="a60001c5"
SENTINEL_A6[divu_overflow_edge]="a60001c6"
SENTINEL_A6[mull_unsigned_32]="a60001c7"
SENTINEL_A6[mull_signed_32]="a60001c8"
SENTINEL_A6[divl_unsigned_32]="a60001c9"
SENTINEL_A6[divl_signed_32]="a60001ca"
SENTINEL_A6[asrw_mem_edge]="a60001cb"
SENTINEL_A6[roxlw_mem_edge]="a60001cc"
SENTINEL_A6[roxrw_mem_edge]="a60001cd"
SENTINEL_A6[abcd_99_plus_01_edge]="a60001ce"
SENTINEL_A6[sbcd_with_x_edge]="a60001cf"
SENTINEL_A6[nbcd_99_edge]="a60001d0"
SENTINEL_A6[bfextu_reg_edge]="a60001d1"
SENTINEL_A6[bfexts_reg_edge]="a60001d2"
SENTINEL_A6[bfffo_reg_edge]="a60001d3"
SENTINEL_A6[bfset_reg_edge]="a60001d4"
SENTINEL_A6[bfclr_reg_edge]="a60001d5"
SENTINEL_A6[bfchg_reg_edge]="a60001d6"
SENTINEL_A6[bftst_reg_edge]="a60001d7"
SENTINEL_A6[bfins_reg_edge]="a60001d8"
SENTINEL_A6[pack_dn_edge]="a60001d9"
SENTINEL_A6[unpk_dn_edge]="a60001da"
SENTINEL_A6[movep_l_roundtrip]="a60001db"
SENTINEL_A6[sr_ops_combo]="a60001dc"
SENTINEL_A6[moves_write_read]="a60001dd"
SENTINEL_A6[move_b_flags]="a601012a"
SENTINEL_A6[move_w_zero]="a601012b"
SENTINEL_A6[cmpi_l_abs_short_eq]="a6010136"
SENTINEL_A6[cmpi_l_abs_short_ne]="a6010137"
SENTINEL_A6[cmpi_bne_w_not_taken]="a6010138"
SENTINEL_A6[cmpi_bne_w_taken]="a6010139"
SENTINEL_A6[cmpi_b_abs_short_blt]="a601013a"
SENTINEL_A6[movem_save_modify_restore]="a601013b"
SENTINEL_A6[movec_cacr_roundtrip]="a6010147"
SENTINEL_A6[cache_init_sequence]="a6010148"
SENTINEL_A6[move_l_neg_disp_a5]="a6010149"
SENTINEL_A6[sr_barrier_cache_init]="a601014a"
SENTINEL_A6[bsr_l_long]="a601013c"
SENTINEL_A6[tst_bne_after_bsr_rts]="a6010140"
SENTINEL_A6[tst_bne_after_jsr_an]="a6010141"
SENTINEL_A6[save_clear_slot_restore_tst]="a6010146"
SENTINEL_A6[jmp_d8_pc_dn_w]="a601013d"
SENTINEL_A6[pea_movem_stack]="a601013e"
SENTINEL_A6[subq_sp_movea_write]="a601013f"
SENTINEL_A6[add_l_an_dn]="a601012c"
SENTINEL_A6[sub_w_dn_an]="a601012d"
SENTINEL_A6[cmp_b]="a601012e"
SENTINEL_A6[cmp_w]="a601012f"
SENTINEL_A6[ori_w_mem]="a6010130"
SENTINEL_A6[andi_b_mem]="a6010131"
SENTINEL_A6[link_neg16]="a6010132"
SENTINEL_A6[mulu_max]="a6010133"
SENTINEL_A6[divs_neg_rem]="a6010134"
SENTINEL_A6[negx_64bit]="a6010135"
SENTINEL_A6[not_sizes]="a60100ba"
SENTINEL_A6[asl_w_vflag]="a601010a"
SENTINEL_A6[asl_b_overflow]="a601010b"
SENTINEL_A6[lsr_w_regcount]="a601010c"
SENTINEL_A6[asr_w_preserve]="a601010d"
SENTINEL_A6[movem_w_signext]="a601010e"
SENTINEL_A6[cmpm_l_equal]="a601010f"
SENTINEL_A6[cmpm_b_unequal]="a6010110"
SENTINEL_A6[all_regs_alive]="a601011a"
SENTINEL_A6[scaled_index_word]="a601011b"
SENTINEL_A6[byte_indexed_load]="a601011c"
SENTINEL_A6[indexed_store_load]="a601011d"
SENTINEL_A6[addq_subq_sizes]="a601011e"
SENTINEL_A6[x_flag_chain]="a601011f"
SENTINEL_A6[sub_w_subx_chain]="a6010120"
SENTINEL_A6[dbeq_loop_50]="a6010123"
SENTINEL_A6[dbmi_loop_neg]="a6010124"
SENTINEL_A6[exg_dn_an]="a6010121"
SENTINEL_A6[push_pop_a0]="a6010122"
SENTINEL_A6[addx_64bit]="a6010111"
SENTINEL_A6[subx_64bit]="a6010112"
SENTINEL_A6[muls_boundary]="a6010113"
SENTINEL_A6[divu_max_quotient]="a6010114"
SENTINEL_A6[move_b_preserve_flags]="a6010115"
SENTINEL_A6[byte_logic_chain]="a6010116"
SENTINEL_A6[bchg_imm_high]="a6010117"
SENTINEL_A6[neg_w_partial]="a6010118"
SENTINEL_A6[clr_b_tst]="a6010119"
SENTINEL_A6[not_word_preserve_upper]="a60100bb"
SENTINEL_A6[not_byte_preserve_upper]="a60100bc"
SENTINEL_A6[scc_ccr_preserve_bpl_taken]="a60100bd"
SENTINEL_A6[scc_ccr_preserve_bmi_taken]="a60100be"
SENTINEL_A6[scc_ccr_preserve_bge_taken]="a60100bf"
SENTINEL_A6[scc_ccr_preserve_bgt_taken]="a60100c0"
SENTINEL_A6[scc_ccr_preserve_ble_taken]="a60100c1"
SENTINEL_A6[dbne_loop_cmpi]="a6010101"
SENTINEL_A6[bsr_in_dbra_loop]="a6010102"
SENTINEL_A6[table_lookup]="a6010103"
SENTINEL_A6[dbra_loop_1000]="a6010104"
SENTINEL_A6[swap_pack]="a6010105"
SENTINEL_A6[lea_scaled_index]="a6010106"
SENTINEL_A6[multi_branch]="a6010107"
SENTINEL_A6[andi_l_dn]="a6010108"
SENTINEL_A6[eor_self]="a6010109"
SENTINEL_A6[adda_w_cov]="a60001e0"
SENTINEL_A6[adda_l_cov]="a60001e1"
SENTINEL_A6[adda_w_neg_cov]="a60001e2"
SENTINEL_A6[eori_ccr_cov]="a60001e3"
SENTINEL_A6[rtr_cov]="a60001e4"
SENTINEL_A6[mvr2usp_cov]="a60001e5"
SENTINEL_A6[move_b_d16_an_cov]="a60001e6"
SENTINEL_A6[move_w_d16_an_cov]="a60001e7"
SENTINEL_A6[move_l_d16_an_cov]="a60001e8"
SENTINEL_A6[move_b_idx_cov]="a60001e9"
SENTINEL_A6[move_l_idx_scale_cov]="a60001ea"
SENTINEL_A6[move_l_pc_rel_cov]="a60001eb"
SENTINEL_A6[move_l_abs_w_cov]="a60001ec"
SENTINEL_A6[move_l_abs_l_cov]="a60001ed"
SENTINEL_A6[predec_postinc_cov]="a60001ee"
SENTINEL_A6[imm_to_mem_b_cov]="a60001ef"
SENTINEL_A6[imm_to_mem_w_cov]="a60001f0"
SENTINEL_A6[imm_to_mem_l_cov]="a60001f1"
SENTINEL_A6[add_b_overflow_cov]="a60001f2"
SENTINEL_A6[sub_w_borrow_cov]="a60001f3"
SENTINEL_A6[cmp_l_equal_cov]="a60001f4"
SENTINEL_A6[and_l_zero_cov]="a60001f5"
SENTINEL_A6[or_l_allones_cov]="a60001f6"
SENTINEL_A6[eor_self_cov]="a60001f7"
SENTINEL_A6[neg_b_overflow_cov]="a60001f8"
SENTINEL_A6[not_b_cov]="a60001f9"
SENTINEL_A6[odd_addr_cov]="a60001fa"
SENTINEL_A6[a7_byte_postinc_cov]="a60001fb"
# Additional opcode coverage sentinels
SENTINEL_A6[chk_w_in_range]="a6f03200"
SENTINEL_A6[chk_w_zero]="a6f03300"
SENTINEL_A6[chk_w_equal]="a6f03400"
SENTINEL_A6[sbcd_borrow_chain]="a6f03500"
SENTINEL_A6[sbcd_zero_zero]="a6f03600"
SENTINEL_A6[nbcd_zero_no_x]="a6f03700"
SENTINEL_A6[nbcd_with_x]="a6f03800"
SENTINEL_A6[bfins_low8]="a6f03900"
SENTINEL_A6[bfins_mid8]="a6f03a00"
SENTINEL_A6[movec_vbr_roundtrip]="a6f03b00"
SENTINEL_A6[movec_sfc_roundtrip]="a6f03c00"
SENTINEL_A6[movec_dfc_roundtrip]="a6f03d00"
SENTINEL_A6[mull_u64]="a6f03e00"
SENTINEL_A6[mull_s32_neg]="a6f03f00"
SENTINEL_A6[divl_u32_rem]="a6f04000"
SENTINEL_A6[divl_s32_neg]="a6f04100"
SENTINEL_A6[divl_u32_max]="a6f04200"
SENTINEL_A6[divl_s32_neg_divisor]="a6f04300"
SENTINEL_A6[mull_s64_neg]="a6f04400"
SENTINEL_A6[divl_same_dq_dr]="a6f04500"
SENTINEL_A6[divl_u64]="a6f04600"
SENTINEL_A6[divl_s64]="a6f04700"
SENTINEL_A6[bfins_dreg_imm]="a6f04800"
SENTINEL_A6[bfins_dreg_narrow]="a6f04900"
# Fuzz vector sentinels
SENTINEL_A6[fuzz_alu_0]="a6f00000"
SENTINEL_A6[fuzz_shift_0]="a6f00100"
SENTINEL_A6[fuzz_bitops_0]="a6f00200"
SENTINEL_A6[fuzz_muldiv_0]="a6f00300"
SENTINEL_A6[fuzz_extswap_0]="a6f00400"
SENTINEL_A6[fuzz_addxsubx_0]="a6f00500"
SENTINEL_A6[fuzz_memrt_0]="a6f00600"
SENTINEL_A6[fuzz_exg_0]="a6f00700"
SENTINEL_A6[fuzz_mixed_0]="a6f00800"
SENTINEL_A6[fuzz_flags_0]="a6f00900"
SENTINEL_A6[fuzz_alu_1]="a6f00a00"
SENTINEL_A6[fuzz_shift_1]="a6f00b00"
SENTINEL_A6[fuzz_bitops_1]="a6f00c00"
SENTINEL_A6[fuzz_muldiv_1]="a6f00d00"
SENTINEL_A6[fuzz_extswap_1]="a6f00e00"
SENTINEL_A6[fuzz_addxsubx_1]="a6f00f00"
SENTINEL_A6[fuzz_memrt_1]="a6f01000"
SENTINEL_A6[fuzz_exg_1]="a6f01100"
SENTINEL_A6[fuzz_mixed_1]="a6f01200"
SENTINEL_A6[fuzz_flags_1]="a6f01300"
SENTINEL_A6[fuzz_alu_2]="a6f01400"
SENTINEL_A6[fuzz_shift_2]="a6f01500"
SENTINEL_A6[fuzz_bitops_2]="a6f01600"
SENTINEL_A6[fuzz_muldiv_2]="a6f01700"
SENTINEL_A6[fuzz_extswap_2]="a6f01800"
SENTINEL_A6[fuzz_addxsubx_2]="a6f01900"
SENTINEL_A6[fuzz_memrt_2]="a6f01a00"
SENTINEL_A6[fuzz_exg_2]="a6f01b00"
SENTINEL_A6[fuzz_mixed_2]="a6f01c00"
SENTINEL_A6[fuzz_flags_2]="a6f01d00"
SENTINEL_A6[fuzz_alu_3]="a6f01e00"
SENTINEL_A6[fuzz_shift_3]="a6f01f00"
SENTINEL_A6[fuzz_bitops_3]="a6f02000"
SENTINEL_A6[fuzz_muldiv_3]="a6f02100"
SENTINEL_A6[fuzz_extswap_3]="a6f02200"
SENTINEL_A6[fuzz_addxsubx_3]="a6f02300"
SENTINEL_A6[fuzz_memrt_3]="a6f02400"
SENTINEL_A6[fuzz_exg_3]="a6f02500"
SENTINEL_A6[fuzz_mixed_3]="a6f02600"
SENTINEL_A6[fuzz_flags_3]="a6f02700"
SENTINEL_A6[fuzz_alu_4]="a6f02800"
SENTINEL_A6[fuzz_shift_4]="a6f02900"
SENTINEL_A6[fuzz_bitops_4]="a6f02a00"
SENTINEL_A6[fuzz_muldiv_4]="a6f02b00"
SENTINEL_A6[fuzz_extswap_4]="a6f02c00"
SENTINEL_A6[fuzz_addxsubx_4]="a6f02d00"
SENTINEL_A6[fuzz_memrt_4]="a6f02e00"
SENTINEL_A6[fuzz_exg_4]="a6f02f00"
SENTINEL_A6[fuzz_mixed_4]="a6f03000"
SENTINEL_A6[fuzz_flags_4]="a6f03100"

# Risk-focused subset used for strict mismatch-first autoresearch.
# Only these vectors count toward risky_total progression.
declare -A RISKY_TESTS=(
    [roxl_x_propagation]=1
    [roxr_x_propagation]=1
    [roxl_count_2]=1
    [asl_overflow]=1
    [lsr_count_32]=1
    [asr_count_0]=1
    [ror_word]=1
    [rol_word]=1
    [shift_chain]=1
    [roxl_reg_count_32]=1
    [roxl_reg_count_33]=1
    [roxr_reg_count_33]=1
    [roxr_reg_count_32]=1
    [roxr_reg_count_0]=1
    [roxl_reg_count_63]=1
    [roxr_reg_count_63]=1
    [roxr_roxl_chain_x]=1
    [roxl_lsr_chain_x]=1
    [movem_predec_postinc]=1
    [movem_predec_mixed_order]=1
    [movem]=1
    [dbra]=1
    [dbra_not_taken]=1
    [dbra_start_minus1_branch]=1
    [dbra_start_8000_branch]=1
    [dbt_true_not_taken]=1
    [dbra_three_iter]=1
    [dbra_four_iter]=1
    [dbra_five_iter]=1
    [dbra_six_iter]=1
    [dbcc_loop_c_set]=1
    [dbcs_not_taken_c_set]=1
    [dbpl_loop_n_set]=1
    [dbmi_not_taken_n_set]=1
    [dbhi_not_taken_hi_set]=1
    [dbls_not_taken_ls_set]=1
    [dbge_not_taken_n_eq_v]=1
    [dblt_not_taken_n_ne_v]=1
    [dbgt_not_taken_gt_set]=1
    [dble_not_taken_le_set]=1
    [dbhi_false_dec_terminal_ls_set]=1
    [dbls_false_dec_terminal_hi_set]=1
    [dbge_false_dec_terminal_n_ne_v]=1
    [dblt_false_dec_terminal_n_eq_v]=1
    [dbgt_false_dec_terminal_z_set]=1
    [dble_false_dec_terminal_gt_set]=1
    [dbcc_ccr_preserve_beq_taken]=1
    [dbcc_ccr_preserve_bne_taken]=1
    [dbcc_ccr_preserve_bcs_taken]=1
    [dbcc_ccr_preserve_bvc_taken]=1
    [dbcc_ccr_preserve_bvs_taken]=1
    [dbcc_ccr_preserve_bhi_taken]=1
    [dbcc_ccr_preserve_bls_taken]=1
    [dbcc_ccr_preserve_bge_taken]=1
    [dbcc_ccr_preserve_blt_taken]=1
    [dbcc_ccr_preserve_bgt_taken]=1
    [dbcc_ccr_preserve_ble_taken]=1
    [dbvc_loop_v_set]=1
    [dbvs_loop_v_clear]=1
    [dbvc_not_taken_v_clear]=1
    [dbvs_not_taken_v_set]=1
    [dbne_loop_z_set]=1
    [dbeq_loop_z_clear]=1
    [btst_reg_high_bit]=1
    [bitops_highbit]=1
    [bitops_chg_highbit]=1
    [flags]=1
    [flags_eori_ccr]=1
    [move_sr_roundtrip]=1
    [muls_neg_neg]=1
    [muls_zero]=1
    [divs_neg_neg]=1
    [divs_overflow]=1
    [mulu_large]=1
    [divu_remainder]=1
    [muldiv]=1
    [abcd_basic]=1
    [sbcd_basic]=1
    [abcd_with_carry]=1
    [nbcd_basic]=1
    [negx_with_x]=1
    [negx_zero]=1
    [addx_basic]=1
    [subx_basic]=1
    [addx_chain]=1
    [flag_chain_xzn]=1
    [dbne_loop_cmpi]=1
    [bsr_in_dbra_loop]=1
    [table_lookup]=1
    [dbra_loop_1000]=1
    [swap_pack]=1
    [lea_scaled_index]=1
    [multi_branch]=1
    [andi_l_dn]=1
    [eor_self]=1
    [asl_w_vflag]=1
    [asl_b_overflow]=1
    [lsr_w_regcount]=1
    [asr_w_preserve]=1
    [movem_w_signext]=1
    [cmpm_l_equal]=1
    [cmpm_b_unequal]=1
    [addx_64bit]=1
    [subx_64bit]=1
    [muls_boundary]=1
    [divu_max_quotient]=1
    [move_b_preserve_flags]=1
    [byte_logic_chain]=1
    [bchg_imm_high]=1
    [neg_w_partial]=1
    [clr_b_tst]=1
    [all_regs_alive]=1
    [scaled_index_word]=1
    [byte_indexed_load]=1
    [indexed_store_load]=1
    [addq_subq_sizes]=1
    [x_flag_chain]=1
    [sub_w_subx_chain]=1
    [exg_dn_an]=1
    [push_pop_a0]=1
    [dbeq_loop_50]=1
    [dbmi_loop_neg]=1
    [lsl_l_count0]=1
    [asr_l_8_neg]=1
    [rol_l_16]=1
    [lsl_b_7]=1
    [asr_b_1_sign]=1
    [move_b_flags]=1
    [move_w_zero]=1
    [add_l_an_dn]=1
    [sub_w_dn_an]=1
    [cmp_b]=1
    [cmp_w]=1
    [ori_w_mem]=1
    [andi_b_mem]=1
    [link_neg16]=1
    [mulu_max]=1
    [divs_neg_rem]=1
    [negx_64bit]=1
    [cmpi_l_abs_short_eq]=1
    [cmpi_l_abs_short_ne]=1
    [cmpi_bne_w_not_taken]=1
    [cmpi_bne_w_taken]=1
    [cmpi_b_abs_short_blt]=1
    [movem_save_modify_restore]=1
    [bsr_l_long]=1
    [jmp_d8_pc_dn_w]=1
    [pea_movem_stack]=1
    [subq_sp_movea_write]=1
    [tst_bne_after_bsr_rts]=1
    [tst_bne_after_jsr_an]=1
    [save_clear_slot_restore_tst]=1
    [movec_cacr_roundtrip]=1
    [cache_init_sequence]=1
    [move_l_neg_disp_a5]=1
    [sr_barrier_cache_init]=1
    [divs_word_hardfail]=1
    [divu_word_hardfail]=1
    [mull_32_hardfail]=1
    [divl_32_hardfail]=1
    [aslw_mem_hardfail]=1
    [lsrw_mem_hardfail]=1
    [rolw_mem_hardfail]=1
    [ori_sr_hardfail]=1
    [andi_sr_hardfail]=1
    [eori_sr_hardfail]=1
    [move_from_sr_hardfail]=1
    [move_to_sr_hardfail]=1
    [divs_neg_by_neg_edge]=1
    [divs_by_minus_one_edge]=1
    [divs_zero_dividend_edge]=1
    [divs_overflow_edge]=1
    [divu_exact_edge]=1
    [divu_with_remainder_edge]=1
    [divu_overflow_edge]=1
    [mull_unsigned_32]=1
    [mull_signed_32]=1
    [divl_unsigned_32]=1
    [divl_signed_32]=1
    [asrw_mem_edge]=1
    [roxlw_mem_edge]=1
    [roxrw_mem_edge]=1
    [abcd_99_plus_01_edge]=1
    [sbcd_with_x_edge]=1
    [nbcd_99_edge]=1
    [bfextu_reg_edge]=1
    [bfexts_reg_edge]=1
    [bfffo_reg_edge]=1
    [bfset_reg_edge]=1
    [bfclr_reg_edge]=1
    [bfchg_reg_edge]=1
    [bftst_reg_edge]=1
    [bfins_reg_edge]=1
    [pack_dn_edge]=1
    [unpk_dn_edge]=1
    [movep_l_roundtrip]=1
    [sr_ops_combo]=1
    [moves_write_read]=1
    [adda_w_cov]=1
    [adda_l_cov]=1
    [adda_w_neg_cov]=1
    [eori_ccr_cov]=1
    [rtr_cov]=1
    [mvr2usp_cov]=1
    [move_b_d16_an_cov]=1
    [move_w_d16_an_cov]=1
    [move_l_d16_an_cov]=1
    [move_b_idx_cov]=1
    [move_l_idx_scale_cov]=1
    [move_l_pc_rel_cov]=1
    [move_l_abs_w_cov]=1
    [move_l_abs_l_cov]=1
    [predec_postinc_cov]=1
    [imm_to_mem_b_cov]=1
    [imm_to_mem_w_cov]=1
    [imm_to_mem_l_cov]=1
    [add_b_overflow_cov]=1
    [sub_w_borrow_cov]=1
    [cmp_l_equal_cov]=1
    [and_l_zero_cov]=1
    [or_l_allones_cov]=1
    [eor_self_cov]=1
    [neg_b_overflow_cov]=1
    [not_b_cov]=1
    [odd_addr_cov]=1
    [a7_byte_postinc_cov]=1
    [dbra_loop_100]=1

    [fuzz_alu_0]=1
    [fuzz_shift_0]=1
    [fuzz_bitops_0]=1
    [fuzz_muldiv_0]=1
    [fuzz_extswap_0]=1
    [fuzz_addxsubx_0]=1
    [fuzz_memrt_0]=1
    [fuzz_exg_0]=1
    [fuzz_mixed_0]=1
    [fuzz_flags_0]=1
    [fuzz_alu_1]=1
    [fuzz_shift_1]=1
    [fuzz_bitops_1]=1
    [fuzz_muldiv_1]=1
    [fuzz_extswap_1]=1
    [fuzz_addxsubx_1]=1
    [fuzz_memrt_1]=1
    [fuzz_exg_1]=1
    [fuzz_mixed_1]=1
    [fuzz_flags_1]=1
    [fuzz_alu_2]=1
    [fuzz_shift_2]=1
    [fuzz_bitops_2]=1
    [fuzz_muldiv_2]=1
    [fuzz_extswap_2]=1
    [fuzz_addxsubx_2]=1
    [fuzz_memrt_2]=1
    [fuzz_exg_2]=1
    [fuzz_mixed_2]=1
    [fuzz_flags_2]=1
    [fuzz_alu_3]=1
    [fuzz_shift_3]=1
    [fuzz_bitops_3]=1
    [fuzz_muldiv_3]=1
    [fuzz_extswap_3]=1
    [fuzz_addxsubx_3]=1
    [fuzz_memrt_3]=1
    [fuzz_exg_3]=1
    [fuzz_mixed_3]=1
    [fuzz_flags_3]=1
    [fuzz_alu_4]=1
    [fuzz_shift_4]=1
    [fuzz_bitops_4]=1
    [fuzz_muldiv_4]=1
    [fuzz_extswap_4]=1
    [fuzz_addxsubx_4]=1
    [fuzz_memrt_4]=1
    [fuzz_exg_4]=1
    [fuzz_mixed_4]=1
    [fuzz_flags_4]=1

    [chk_w_in_range]=1
    [chk_w_zero]=1
    [chk_w_equal]=1
    [sbcd_borrow_chain]=1
    [sbcd_zero_zero]=1
    [nbcd_zero_no_x]=1
    [nbcd_with_x]=1
    [bfins_low8]=1
    [bfins_mid8]=1
    [movec_vbr_roundtrip]=1
    [movec_sfc_roundtrip]=1
    [movec_dfc_roundtrip]=1
    [mull_u64]=1
    [mull_s32_neg]=1
    [divl_u32_rem]=1
    [divl_s32_neg]=1
    [divl_u32_max]=1
    [divl_s32_neg_divisor]=1
    [mull_s64_neg]=1
    [divl_same_dq_dr]=1
    [divl_u64]=1
    [divl_s64]=1
    [bfins_dreg_imm]=1
    [bfins_dreg_narrow]=1
)

# Preflight harness invariants: deterministic mapping and sentinel hygiene.
declare -A _seen_test_names=()
declare -A _seen_sentinels=()
for name in "${TEST_ORDER[@]}"; do
    if [ -n "${_seen_test_names[$name]+x}" ]; then
        emit_failure_metrics 1 "duplicate test name in TEST_ORDER: $name" 1
    fi
    _seen_test_names[$name]=1

    if [ -z "${TESTS[$name]+x}" ]; then
        emit_failure_metrics 1 "missing TESTS entry for test: $name" 1
    fi
    if [ -z "${SENTINEL_A6[$name]+x}" ]; then
        emit_failure_metrics 1 "missing SENTINEL_A6 entry for test: $name" 1
    fi

    hex_words="${TESTS[$name]}"
    if ! [[ "$hex_words" =~ ^[0-9A-Fa-f]{4}([[:space:]]+[0-9A-Fa-f]{4})*$ ]]; then
        emit_failure_metrics 1 "invalid TESTS encoding for $name: expected 4-hex words" 1
    fi
    if [[ "$hex_words" =~ (^|[[:space:]])2[Cc]7[Cc]($|[[:space:]]) ]]; then
        emit_failure_metrics 1 "TESTS for $name must not include MOVEA immediate opcode 2C7C (reserved for harness sentinel append)" 1
    fi

    sentinel="${SENTINEL_A6[$name]}"
    if ! [[ "$sentinel" =~ ^[0-9a-fA-F]{8}$ ]]; then
        emit_failure_metrics 1 "invalid sentinel format for $name: $sentinel" 1
    fi
    if [ -n "${_seen_sentinels[$sentinel]+x}" ]; then
        emit_failure_metrics 1 "duplicate sentinel value detected: $sentinel" 1
    fi
    _seen_sentinels[$sentinel]=1
done

for name in "${!TESTS[@]}"; do
    if [ -z "${_seen_test_names[$name]+x}" ]; then
        emit_failure_metrics 1 "TESTS entry not present in TEST_ORDER: $name" 1
    fi
done

for name in "${!SENTINEL_A6[@]}"; do
    if [ -z "${_seen_test_names[$name]+x}" ]; then
        emit_failure_metrics 1 "SENTINEL_A6 entry not present in TEST_ORDER: $name" 1
    fi
done

for name in "${!RISKY_TESTS[@]}"; do
    if [ -z "${_seen_test_names[$name]+x}" ]; then
        emit_failure_metrics 1 "RISKY_TESTS entry not present in TEST_ORDER: $name" 1
    fi
done

# Active mismatch-first campaign vectors.
# Add at most one new line to jit-test/active-risky-tests.txt per iteration.
ACTIVE_RISKY_FILE="$SCRIPT_DIR/active-risky-tests.txt"
if [ ! -f "$ACTIVE_RISKY_FILE" ]; then
    emit_failure_metrics 1 "missing active risky vector list: $ACTIVE_RISKY_FILE" 1
fi

mapfile -t ACTIVE_TEST_ORDER < <(grep -E '^[[:space:]]*[^#[:space:]][a-z0-9_]*[[:space:]]*$' "$ACTIVE_RISKY_FILE" | sed -E 's/^[[:space:]]+|[[:space:]]+$//g')
if [ "${#ACTIVE_TEST_ORDER[@]}" -eq 0 ]; then
    emit_failure_metrics 1 "no active risky vectors listed in $ACTIVE_RISKY_FILE" 1
fi

# Validate active list: known test names, risky-only, and no duplicates.
declare -A _seen_active=()
for name in "${ACTIVE_TEST_ORDER[@]}"; do
    if [ -n "${_seen_active[$name]+x}" ]; then
        emit_failure_metrics 1 "duplicate active risky vector: $name" 1
    fi
    _seen_active[$name]=1

    if [ -z "${_seen_test_names[$name]+x}" ]; then
        emit_failure_metrics 1 "active risky vector not present in TEST_ORDER: $name" 1
    fi
    if [ -z "${RISKY_TESTS[$name]+x}" ]; then
        emit_failure_metrics 1 "active vector is not tagged risky: $name" 1
    fi
done

# ---- Run active risky test cases and score -----------------------------------
PASS=0
FAIL=0
INFRA_FAIL=0
EQUIV_FAIL=0
INFRA_TIMEOUT=0
INFRA_EMU_EXIT=0
INFRA_NO_REGDUMP=0
INFRA_MULTI_REGDUMP=0
INFRA_SENTINEL=0
INFRA_OTHER=0
TOTAL=${#ACTIVE_TEST_ORDER[@]}
RISKY_TOTAL=0
RISKY_PASS=0
RISKY_FAIL_EQUIV=0
RISKY_INFRA_FAIL=0

for name in "${ACTIVE_TEST_ORDER[@]}"; do
    hex="${TESTS[$name]}"
    sentinel_a6="${SENTINEL_A6[$name]}"
    ifile="$RUN_DIR/${name}-interp.txt"
    jfile="$RUN_DIR/${name}-jit.txt"

    is_risky=0
    if [ -n "${RISKY_TESTS[$name]+x}" ]; then
        is_risky=1
        RISKY_TOTAL=$((RISKY_TOTAL+1))
    fi

    interp_ok=1
    jit_ok=1
    init="${INIT_REGS[$name]:-}"
    run_test "$name" "$hex" "false" "$sentinel_a6" "$ifile" "$init" || interp_ok=0
    run_test "$name" "$hex" "true"  "$sentinel_a6" "$jfile" "$init" || jit_ok=0

    if [ "$interp_ok" -eq 1 ] && [ "$jit_ok" -eq 1 ]; then
        if diff -q "$ifile" "$jfile" >/dev/null 2>&1; then
            echo "METRIC opcode_${name}=1"
            PASS=$((PASS+1))
            if [ "$is_risky" -eq 1 ]; then
                RISKY_PASS=$((RISKY_PASS+1))
            fi
        else
            echo "METRIC opcode_${name}=0"
            echo "  DIFF for $name:" >&2
            diff "$ifile" "$jfile" >&2 || true
            FAIL=$((FAIL+1))
            EQUIV_FAIL=$((EQUIV_FAIL+1))
            if [ "$is_risky" -eq 1 ]; then
                RISKY_FAIL_EQUIV=$((RISKY_FAIL_EQUIV+1))
            fi
        fi
    else
        echo "METRIC opcode_${name}=-1"  # harness infrastructure issue
        FAIL=$((FAIL+1))
        INFRA_FAIL=$((INFRA_FAIL+1))
        if [ "$is_risky" -eq 1 ]; then
            RISKY_INFRA_FAIL=$((RISKY_INFRA_FAIL+1))
        fi

        interp_reason=$(cat "${ifile}.reason" 2>/dev/null || echo "unknown")
        jit_reason=$(cat "${jfile}.reason" 2>/dev/null || echo "unknown")
        if [[ "$interp_reason,$jit_reason" == *"timeout"* ]]; then
            INFRA_TIMEOUT=$((INFRA_TIMEOUT+1))
        elif [[ "$interp_reason,$jit_reason" == *"emu_exit_"* ]]; then
            INFRA_EMU_EXIT=$((INFRA_EMU_EXIT+1))
        elif [[ "$interp_reason,$jit_reason" == *"no_regdump"* ]]; then
            INFRA_NO_REGDUMP=$((INFRA_NO_REGDUMP+1))
        elif [[ "$interp_reason,$jit_reason" == *"multi_regdump"* ]]; then
            INFRA_MULTI_REGDUMP=$((INFRA_MULTI_REGDUMP+1))
        elif [[ "$interp_reason,$jit_reason" == *"sentinel_mismatch"* ]]; then
            INFRA_SENTINEL=$((INFRA_SENTINEL+1))
        else
            INFRA_OTHER=$((INFRA_OTHER+1))
        fi
        echo "INFRA $name: interp_reason=$interp_reason jit_reason=$jit_reason" >&2
    fi
done

# Score: fraction of passing tests (0-100)
if [ "$TOTAL" -gt 0 ]; then
    SCORE=$(( PASS * 100 / TOTAL ))
else
    SCORE=0
fi

RISKY_FAIL=$((RISKY_TOTAL - RISKY_PASS))

echo "METRIC pass=$PASS"
echo "METRIC fail=$FAIL"
echo "METRIC total=$TOTAL"
echo "METRIC infra_fail=$INFRA_FAIL"
echo "METRIC fail_equiv=$EQUIV_FAIL"
echo "METRIC infra_timeout=$INFRA_TIMEOUT"
echo "METRIC infra_emu_exit=$INFRA_EMU_EXIT"
echo "METRIC infra_no_regdump=$INFRA_NO_REGDUMP"
echo "METRIC infra_multi_regdump=$INFRA_MULTI_REGDUMP"
echo "METRIC infra_sentinel=$INFRA_SENTINEL"
echo "METRIC infra_other=$INFRA_OTHER"
echo "METRIC risky_total=$RISKY_TOTAL"
echo "METRIC risky_pass=$RISKY_PASS"
echo "METRIC risky_fail=$RISKY_FAIL"
echo "METRIC risky_fail_equiv=$RISKY_FAIL_EQUIV"
echo "METRIC risky_infra_fail=$RISKY_INFRA_FAIL"
echo "METRIC score=$SCORE"

# DBNE_LOOP_CMPI: DBNE with CMPI condition, exits when D1==3
# BSR_IN_DBRA_LOOP: BSR to subroutine inside DBRA loop, 4 iterations
# TABLE_LOOKUP: PC-relative table read via scaled index
# DBRA_LOOP_1000: 1000-iteration loop
# SWAP_PACK: pack two words into a long via SWAP+MOVE.W+SWAP
# LEA_SCALED_INDEX: LEA (0,A0,D1.L*4) scaled indexed addressing
# MULTI_BRANCH: sequential BEQ+BNE with flag propagation
# ANDI_L_DN: AND.L immediate with register
# EOR_SELF: EOR.L Dn,Dn (self-XOR = clear, Z=1)
