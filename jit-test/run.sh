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
    echo "METRIC build_ok=$build_ok"
    echo "METRIC pass=0"
    echo "METRIC fail=0"
    echo "METRIC total=0"
    echo "METRIC infra_fail=$(( build_ok == 1 ? 0 : 1 ))"
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
    if ! SDL_VIDEODRIVER=x11 DISPLAY=:99 HOME="$td" \
      B2_TEST_HEX="$full_hex" \
      B2_TEST_DUMP=1 \
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

declare -a TEST_ORDER=(nop move alu alu_overflow addi_subi_long addi_subi_word addi_subi_word_wrap addi_subi_byte addi_subi_byte_wrap shift bitops bitops_chg bitops_highbit branch branch_chain compare compare_negative cmpi_sizes cmpi_byte_negative cmpi_word_negative cmpi_beq_taken muldiv movem misc flags flags_eori_ccr exg exg_roundtrip imm_logic imm_logic_alt imm_logic_word imm_logic_long tst_sizes bra_taken bra_w_taken bne_not_taken bne_taken bne_w_not_taken bne_w_taken beq_taken beq_not_taken beq_w_taken beq_w_not_taken bpl_taken bpl_not_taken bpl_w_taken bpl_w_not_taken bmi_taken bmi_not_taken bmi_w_taken bmi_w_not_taken bvc_taken bvc_not_taken_overflow bvc_w_taken bvc_w_not_taken_overflow bvs_taken_overflow bvs_not_taken bvs_w_taken_overflow bvs_w_not_taken bge_taken bge_not_taken bge_w_taken bge_w_not_taken blt_taken blt_not_taken blt_w_taken blt_w_not_taken bgt_taken bgt_not_taken bgt_w_taken bgt_w_not_taken ble_taken ble_not_taken ble_w_taken ble_w_not_taken bcc_taken bcc_not_taken bcc_w_taken bcc_w_not_taken bcs_taken bcs_not_taken bcs_w_taken bcs_w_not_taken bhi_taken bhi_not_taken bhi_w_taken bhi_w_not_taken bls_taken bls_not_taken bls_w_taken bls_w_not_taken scc_basic scc_eq_ne scc_carry scc_hi_ls scc_hi_ls_z scc_vc_vs scc_pl_mi scc_ge_lt scc_gt_le quick_ops quick_ops_word quick_ops_word_wrap quick_ops_byte quick_ops_byte_wrap quick_ops_addr dbra dbra_not_taken dbt_true_not_taken dbra_three_iter dbvc_loop_v_set dbvs_loop_v_clear dbvc_not_taken_v_clear dbvs_not_taken_v_set dbne_loop_z_set dbeq_loop_z_clear)
declare -A TESTS
# NOP: trivial decode/execute path sanity check
TESTS[nop]="4E71 4E71"
# MOVE: MOVEQ #0x42,D0; MOVE.L D0,D1; MOVEQ #-1,D2; MOVE.W D2,D3
TESTS[move]="7042 2200 74FF 3602"
# ALU: MOVEQ #5,D0; MOVEQ #3,D1; ADD.L D1,D0; SUB.L D1,D0; AND.L D1,D0
TESTS[alu]="7005 7203 D081 9081 C081"
# ALU_OVERFLOW: MOVEQ #0x7f,D0; ADDQ.L #1,D0; SUBQ.L #1,D0
TESTS[alu_overflow]="707F 5280 5180"
# ADDI_SUBI_LONG: MOVEQ #5,D0; ADDI.L #3,D0; SUBI.L #1,D0
TESTS[addi_subi_long]="7005 0680 0000 0003 0480 0000 0001"
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
# CMPI_BYTE_NEGATIVE: verify CMPI.B sign/boundary behavior against 0xff and BEQ taken path
TESTS[cmpi_byte_negative]="70FF 0C00 00FF 6702 7207 7408"
# CMPI_WORD_NEGATIVE: verify CMPI.W sign/boundary behavior against 0xffff and BEQ taken path
TESTS[cmpi_word_negative]="70FF 0C40 FFFF 6702 7207 7408"
# CMPI_BEQ_TAKEN: compare equal immediate then take BEQ short path
TESTS[cmpi_beq_taken]="7000 0C80 0000 0000 6702 7207 7408"
# MULDIV: MOVEQ #7,D0; MULU.W #3,D0; MOVEQ #21,D1; DIVU.W #3,D1
TESTS[muldiv]="7007 C0FC 0003 7215 82FC 0003"
# MOVEM: setup stack; MOVEM.L D0-D3,-(SP); MOVEM.L (SP)+,D4-D7
TESTS[movem]="7011 7213 7415 7617 48E7 F000 4CDF 000F"
# MISC: MOVEQ #0x5A,D0; SWAP D0; EXT.L D0; CLR.W D1; NEG.L D0
TESTS[misc]="705A 4840 4880 4241 4480"
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
# IMM_LOGIC_WORD: immediate word-width OR/EOR/AND sequence to cover non-byte forms
TESTS[imm_logic_word]="7000 0040 00FF 0A40 0F0F 0240 00F0"
# IMM_LOGIC_LONG: immediate long-width OR/EOR/AND sequence to cover .L forms
TESTS[imm_logic_long]="7000 0080 00FF 00FF 0A80 0F0F 0F0F 0280 00F0 00F0"
# TST_SIZES: exercise TST.B/W/L decode+flag paths on a negative value
TESTS[tst_sizes]="70FF 4A00 4A40 4A80"
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
# QUICK_OPS: MOVEQ #5,D0; ADDQ.L #1,D0; SUBQ.L #1,D0; MOVE.L D0,D1
TESTS[quick_ops]="7005 5280 5180 2200"
# QUICK_OPS_WORD: word-sized add/sub quick on D0 low word, then move.w to D1
TESTS[quick_ops_word]="70FF 5240 5140 3200"
# QUICK_OPS_WORD_WRAP: ADDQ/SUBQ word across 0x7fff/0x8000 boundary with explicit CMPI.W check
TESTS[quick_ops_word_wrap]="7000 0640 7FFF 5240 5140 0C40 7FFF"
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

declare -A SENTINEL_A6
SENTINEL_A6[nop]="a601005a"
SENTINEL_A6[move]="a6010001"
SENTINEL_A6[alu]="a6010002"
SENTINEL_A6[alu_overflow]="a6010031"
SENTINEL_A6[addi_subi_long]="a6010043"
SENTINEL_A6[addi_subi_word]="a6010044"
SENTINEL_A6[addi_subi_word_wrap]="a6010073"
SENTINEL_A6[addi_subi_byte]="a6010056"
SENTINEL_A6[addi_subi_byte_wrap]="a601006f"
SENTINEL_A6[shift]="a6010003"
SENTINEL_A6[bitops]="a6010004"
SENTINEL_A6[bitops_chg]="a6010032"
SENTINEL_A6[bitops_highbit]="a601006d"
SENTINEL_A6[branch]="a6010005"
SENTINEL_A6[branch_chain]="a6010057"
SENTINEL_A6[compare]="a6010006"
SENTINEL_A6[compare_negative]="a6010033"
SENTINEL_A6[cmpi_sizes]="a6010045"
SENTINEL_A6[cmpi_byte_negative]="a6010071"
SENTINEL_A6[cmpi_word_negative]="a6010072"
SENTINEL_A6[cmpi_beq_taken]="a601005b"
SENTINEL_A6[muldiv]="a6010007"
SENTINEL_A6[movem]="a6010008"
SENTINEL_A6[misc]="a6010009"
SENTINEL_A6[flags]="a601000a"
SENTINEL_A6[flags_eori_ccr]="a601006e"
SENTINEL_A6[exg]="a601000b"
SENTINEL_A6[exg_roundtrip]="a6010034"
SENTINEL_A6[imm_logic]="a601000c"
SENTINEL_A6[imm_logic_alt]="a6010035"
SENTINEL_A6[imm_logic_word]="a6010068"
SENTINEL_A6[imm_logic_long]="a601006a"
SENTINEL_A6[tst_sizes]="a601006b"
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
SENTINEL_A6[quick_ops]="a601001f"
SENTINEL_A6[quick_ops_word]="a6010058"
SENTINEL_A6[quick_ops_word_wrap]="a6010074"
SENTINEL_A6[quick_ops_byte]="a6010069"
SENTINEL_A6[quick_ops_byte_wrap]="a6010070"
SENTINEL_A6[quick_ops_addr]="a601006c"
SENTINEL_A6[dbra]="a6010020"
SENTINEL_A6[dbra_not_taken]="a6010021"
SENTINEL_A6[dbt_true_not_taken]="a6010059"
SENTINEL_A6[dbra_three_iter]="a6010030"
SENTINEL_A6[dbvc_loop_v_set]="a6010052"
SENTINEL_A6[dbvs_loop_v_clear]="a6010053"
SENTINEL_A6[dbvc_not_taken_v_clear]="a6010054"
SENTINEL_A6[dbvs_not_taken_v_set]="a6010055"
SENTINEL_A6[dbne_loop_z_set]="a601003c"
SENTINEL_A6[dbeq_loop_z_clear]="a601003d"

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
