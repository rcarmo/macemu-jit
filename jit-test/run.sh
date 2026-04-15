#!/bin/bash
# BasiliskII AArch64 JIT Opcode Correctness Test
# Autoresearch harness: compare interpreter vs JIT register state for each opcode class
set -euo pipefail

UNIX_DIR="$(cd "$(dirname "$0")/../BasiliskII/src/Unix" && pwd)"
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
    if ! env "${env_vars[@]}" \
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

declare -a TEST_ORDER=(nop move moveq_signext alu alu_overflow addi_subi_long addi_subi_long_wrap addi_subi_word addi_subi_word_wrap addi_subi_byte addi_subi_byte_wrap shift bitops bitops_chg bitops_highbit bitops_chg_highbit branch branch_chain compare compare_negative cmpi_sizes cmpi_sizes_zero cmpi_byte_negative cmpi_word_negative cmpi_long_negative cmpi_beq_taken muldiv movem misc clr_sizes clr_byte_preserve_upper clr_word_preserve_upper neg_sizes neg_zero_sizes swap_roundtrip flags flags_eori_ccr exg exg_roundtrip imm_logic imm_logic_alt imm_logic_byte_highbit imm_logic_word imm_logic_long imm_logic_long_alt tst_sizes tst_zero tst_positive bra_taken bra_w_taken bne_not_taken bne_taken bne_w_not_taken bne_w_taken beq_taken beq_not_taken beq_w_taken beq_w_not_taken bpl_taken bpl_not_taken bpl_w_taken bpl_w_not_taken bmi_taken bmi_not_taken bmi_w_taken bmi_w_not_taken bvc_taken bvc_not_taken_overflow bvc_w_taken bvc_w_not_taken_overflow bvs_taken_overflow bvs_not_taken bvs_w_taken_overflow bvs_w_not_taken bge_taken bge_not_taken bge_w_taken bge_w_not_taken blt_taken blt_not_taken blt_w_taken blt_w_not_taken bgt_taken bgt_not_taken bgt_w_taken bgt_w_not_taken ble_taken ble_not_taken ble_w_taken ble_w_not_taken bcc_taken bcc_not_taken bcc_w_taken bcc_w_not_taken bcs_taken bcs_not_taken bcs_w_taken bcs_w_not_taken bhi_taken bhi_not_taken bhi_w_taken bhi_w_not_taken bls_taken bls_not_taken bls_w_taken bls_w_not_taken scc_basic scc_eq_ne scc_carry scc_hi_ls scc_hi_ls_z scc_vc_vs scc_pl_mi scc_ge_lt scc_gt_le scc_ccr_preserve_blt scc_ccr_preserve_bcs scc_ccr_preserve_bne_not_taken scc_ccr_preserve_beq_taken quick_ops quick_ops_long_neg_roundtrip quick_ops_word quick_ops_word_wrap quick_ops_long_wrap quick_ops_byte quick_ops_byte_wrap quick_ops_addr dbra dbra_not_taken dbt_true_not_taken dbra_three_iter dbcc_loop_c_set dbcs_not_taken_c_set dbpl_loop_n_set dbmi_not_taken_n_set dbhi_not_taken_hi_set dbls_not_taken_ls_set dbge_not_taken_n_eq_v dblt_not_taken_n_ne_v dbgt_not_taken_gt_set dble_not_taken_le_set dbhi_false_dec_terminal_ls_set dbls_false_dec_terminal_hi_set dbge_false_dec_terminal_n_ne_v dblt_false_dec_terminal_n_eq_v dbgt_false_dec_terminal_z_set dble_false_dec_terminal_gt_set dbcc_ccr_preserve_beq_taken dbcc_ccr_preserve_bne_taken dbcc_ccr_preserve_bcs_taken dbcc_ccr_preserve_bvc_taken dbcc_ccr_preserve_bvs_taken dbcc_ccr_preserve_bhi_taken dbcc_ccr_preserve_bls_taken dbcc_ccr_preserve_bge_taken dbcc_ccr_preserve_blt_taken dbcc_ccr_preserve_bgt_taken dbcc_ccr_preserve_ble_taken dbvc_loop_v_set dbvs_loop_v_clear dbvc_not_taken_v_clear dbvs_not_taken_v_set dbne_loop_z_set dbeq_loop_z_clear moveq_edges alu_negative_roundtrip imm_logic_word_highbit branch_chain_z_clear branch_chain_carry_set branch_chain_overflow_set scc_ccr_preserve_bvs_taken dbra_four_iter scc_ccr_preserve_bvc_taken scc_ccr_preserve_bhi_taken scc_ccr_preserve_bls_taken dbra_five_iter branch_chain_eq_then_ne branch_chain_carry_clear imm_logic_long_highbit dbra_six_iter not_sizes not_word_preserve_upper not_byte_preserve_upper scc_ccr_preserve_bpl_taken scc_ccr_preserve_bmi_taken scc_ccr_preserve_bge_taken scc_ccr_preserve_bgt_taken scc_ccr_preserve_ble_taken nop_triplet roxl_x_propagation roxr_x_propagation roxl_count_2 asl_overflow lsr_count_32 asr_count_0 ror_word rol_word btst_reg_high_bit muls_neg_neg muls_zero divs_neg_neg divs_overflow abcd_basic sbcd_basic negx_with_x negx_zero addx_basic subx_basic ext_word ext_long)
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

declare -A SENTINEL_A6
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
SENTINEL_A6[not_sizes]="a60100ba"
SENTINEL_A6[not_word_preserve_upper]="a60100bb"
SENTINEL_A6[not_byte_preserve_upper]="a60100bc"
SENTINEL_A6[scc_ccr_preserve_bpl_taken]="a60100bd"
SENTINEL_A6[scc_ccr_preserve_bmi_taken]="a60100be"
SENTINEL_A6[scc_ccr_preserve_bge_taken]="a60100bf"
SENTINEL_A6[scc_ccr_preserve_bgt_taken]="a60100c0"
SENTINEL_A6[scc_ccr_preserve_ble_taken]="a60100c1"

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

# ---- Run all test cases and score --------------------------------------------
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
TOTAL=${#TEST_ORDER[@]}

for name in "${TEST_ORDER[@]}"; do
    hex="${TESTS[$name]}"
    sentinel_a6="${SENTINEL_A6[$name]}"
    ifile="$RUN_DIR/${name}-interp.txt"
    jfile="$RUN_DIR/${name}-jit.txt"

    interp_ok=1
    jit_ok=1
    run_test "$name" "$hex" "false" "$sentinel_a6" "$ifile" || interp_ok=0
    run_test "$name" "$hex" "true"  "$sentinel_a6" "$jfile" || jit_ok=0

    if [ "$interp_ok" -eq 1 ] && [ "$jit_ok" -eq 1 ]; then
        if diff -q "$ifile" "$jfile" >/dev/null 2>&1; then
            echo "METRIC opcode_${name}=1"
            PASS=$((PASS+1))
        else
            echo "METRIC opcode_${name}=0"
            echo "  DIFF for $name:" >&2
            diff "$ifile" "$jfile" >&2 || true
            FAIL=$((FAIL+1))
            EQUIV_FAIL=$((EQUIV_FAIL+1))
        fi
    else
        echo "METRIC opcode_${name}=-1"  # harness infrastructure issue
        FAIL=$((FAIL+1))
        INFRA_FAIL=$((INFRA_FAIL+1))

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
echo "METRIC score=$SCORE"
