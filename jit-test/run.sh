#!/bin/bash
# BasiliskII AArch64 JIT Opcode Correctness Test
# Autoresearch harness: compare interpreter vs JIT register state for each opcode class
set -euo pipefail

UNIX_DIR="$(cd "$(dirname "$0")/../BasiliskII/src/Unix" && pwd)"
ROM="/workspace/projects/rpi-basilisk2-sdl2-nox/Quadra800.ROM"
DISK="/workspace/fixtures/basilisk/images/HD200MB"
RUN_DIR="/tmp/ar-jit-opcodes-$$"
mkdir -p "$RUN_DIR"

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
    echo "METRIC infra_fail=0"
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
    mkdir -p "$td"

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
    SDL_VIDEODRIVER=x11 DISPLAY=:99 HOME="$td" \
      B2_TEST_HEX="$full_hex" \
      B2_TEST_DUMP=1 \
      timeout -k 5s 30s "$UNIX_DIR/BasiliskII" --config "$td/prefs" \
      > "$td/emu.log" 2>&1 || true

    pkill -f "$UNIX_DIR/BasiliskII --config $td/prefs" 2>/dev/null || true
    sleep 0.2

    local dump_count
    dump_count=$(grep -c "^REGDUMP:" "$td/emu.log" 2>/dev/null || true)
    if [ "$dump_count" -ne 1 ]; then
        echo "INFRA $name jit=$use_jit: expected 1 REGDUMP, got $dump_count" >&2
        return 1
    fi

    grep "^REGDUMP:" "$td/emu.log" > "$outfile"
    if ! grep -qi "A6=$sentinel_a6" "$outfile"; then
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

declare -a TEST_ORDER=(move alu shift bitops branch compare muldiv movem misc flags exg imm_logic bra_taken bne_not_taken bne_taken beq_taken beq_not_taken bpl_taken bmi_not_taken bvc_taken bvs_not_taken bge_taken blt_not_taken bgt_taken ble_not_taken bcc_taken bcc_not_taken bcs_taken bcs_not_taken scc_basic quick_ops dbra dbra_not_taken)
declare -A TESTS
# MOVE: MOVEQ #0x42,D0; MOVE.L D0,D1; MOVEQ #-1,D2; MOVE.W D2,D3
TESTS[move]="7042 2200 74FF 3602"
# ALU: MOVEQ #5,D0; MOVEQ #3,D1; ADD.L D1,D0; SUB.L D1,D0; AND.L D1,D0
TESTS[alu]="7005 7203 D081 9081 C081"
# SHIFT: MOVEQ #8,D0; LSL.L #1,D0; LSR.L #2,D0; ASR.L #1,D0; ROL.L #1,D0
TESTS[shift]="7008 E388 E888 E080 E398"
# BITOPS: MOVEQ #0,D0; BSET #3,D0; BTST #3,D0; BCLR #3,D0; BTST #3,D0
TESTS[bitops]="7000 08C0 0003 0800 0003 0880 0003 0800 0003"
# BRANCH: MOVEQ #0,D0; CMP.L D0,D0; BEQ.S +2; MOVEQ #1,D0 (should skip); MOVEQ #2,D1
TESTS[branch]="7000 B080 6702 7001 7202"
# COMPARE: MOVEQ #5,D0; MOVEQ #3,D1; CMP.L D1,D0; TST.L D0; CMPI.L #5,D0
TESTS[compare]="7005 7203 B081 4A80 0C80 0000 0005"
# MULDIV: MOVEQ #7,D0; MULU.W #3,D0; MOVEQ #21,D1; DIVU.W #3,D1
TESTS[muldiv]="7007 C0FC 0003 7215 82FC 0003"
# MOVEM: setup stack; MOVEM.L D0-D3,-(SP); MOVEM.L (SP)+,D4-D7
TESTS[movem]="7011 7213 7415 7617 48E7 F000 4CDF 000F"
# MISC: MOVEQ #0x5A,D0; SWAP D0; EXT.L D0; CLR.W D1; NEG.L D0
TESTS[misc]="705A 4840 4880 4241 4480"
# FLAGS: MOVE #0x2700,SR; ORI #0x10,CCR; ANDI #0xEF,CCR; MOVE SR,D0
TESTS[flags]="46FC 2700 003C 0010 023C 00EF 40C0"
# EXG: MOVEQ #1,D0; MOVEQ #2,D1; EXG D0,D1
TESTS[exg]="7001 7202 C141"
# IMM_LOGIC: MOVEQ #0,D0; ORI.B #0x0f,D0; EORI.B #0xf0,D0; ANDI.B #0x3c,D0
TESTS[imm_logic]="7000 0000 000F 0A00 00F0 0200 003C"
# BRA_TAKEN: unconditional branch should skip MOVEQ #9,D1
TESTS[bra_taken]="7001 6002 7209 7402"
# BNE_NOT_TAKEN: CMP.L D0,D0 sets Z=1; BNE should not branch
TESTS[bne_not_taken]="7001 B080 6602 7207"
# BNE_TAKEN: CMPI.L #2,D0 sets Z=0; BNE should branch and skip MOVEQ #7,D1
TESTS[bne_taken]="7001 0C80 0000 0002 6602 7207 7408"
# BEQ_TAKEN: CMP.L D0,D0 sets Z=1; BEQ should branch and skip MOVEQ #7,D1
TESTS[beq_taken]="7001 B080 6702 7207 7408"
# BEQ_NOT_TAKEN: CMPI.L #2,D0 sets Z=0; BEQ should not branch
TESTS[beq_not_taken]="7001 0C80 0000 0002 6702 7207"
# BPL_TAKEN: N=0 so BPL should branch and skip MOVEQ #9,D1
TESTS[bpl_taken]="7001 6A02 7209 7402"
# BMI_NOT_TAKEN: N=0 so BMI should not branch
TESTS[bmi_not_taken]="7001 6B02 7203"
# BVC_TAKEN: V=0 in this sequence, so BVC should branch and skip MOVEQ #9,D1
TESTS[bvc_taken]="7001 6802 7209 7402"
# BVS_NOT_TAKEN: V=0 in this sequence, so BVS should not branch
TESTS[bvs_not_taken]="7001 6902 7203"
# BGE_TAKEN: N==V==0, so BGE should branch and skip MOVEQ #9,D1
TESTS[bge_taken]="7001 6C02 7209 7402"
# BLT_NOT_TAKEN: N==V==0, so BLT should not branch
TESTS[blt_not_taken]="7001 6D02 7203"
# BGT_TAKEN: Z==0 and N==V==0, so BGT should branch and skip MOVEQ #9,D1
TESTS[bgt_taken]="7001 6E02 7209 7402"
# BLE_NOT_TAKEN: Z==0 and N==V==0, so BLE should not branch
TESTS[ble_not_taken]="7001 6F02 7203"
# BCC_TAKEN: CMPI.L #0,D0 sets C=0; BCC should branch and skip MOVEQ #7,D1
TESTS[bcc_taken]="7001 0C80 0000 0000 6402 7207 7408"
# BCC_NOT_TAKEN: CMPI.L #2,D0 sets C=1; BCC should not branch
TESTS[bcc_not_taken]="7001 0C80 0000 0002 6402 7207"
# BCS_TAKEN: CMPI.L #2,D0 sets C=1; BCS should branch and skip MOVEQ #7,D1
TESTS[bcs_taken]="7001 0C80 0000 0002 6502 7207 7408"
# BCS_NOT_TAKEN: CMPI.L #0,D0 sets C=0; BCS should not branch
TESTS[bcs_not_taken]="7001 0C80 0000 0000 6502 7207"
# SCC_BASIC: MOVEQ #0,D0/D1; ST D0 (set true); SF D1 (set false)
TESTS[scc_basic]="7000 7200 50C0 51C1"
# QUICK_OPS: MOVEQ #5,D0; ADDQ.L #1,D0; SUBQ.L #1,D0; MOVE.L D0,D1
TESTS[quick_ops]="7005 5280 5180 2200"
# DBRA: MOVEQ #1,D0; DBRA D0,+2 (taken once, skips MOVEQ #9,D1); NOP
TESTS[dbra]="7001 51C8 0002 7209 4E71"
# DBRA_NOT_TAKEN: MOVEQ #0,D0; DBRA D0,+2 should not branch (counter reaches -1)
TESTS[dbra_not_taken]="7000 51C8 0002 7207"

declare -A SENTINEL_A6
SENTINEL_A6[move]="a6010001"
SENTINEL_A6[alu]="a6010002"
SENTINEL_A6[shift]="a6010003"
SENTINEL_A6[bitops]="a6010004"
SENTINEL_A6[branch]="a6010005"
SENTINEL_A6[compare]="a6010006"
SENTINEL_A6[muldiv]="a6010007"
SENTINEL_A6[movem]="a6010008"
SENTINEL_A6[misc]="a6010009"
SENTINEL_A6[flags]="a601000a"
SENTINEL_A6[exg]="a601000b"
SENTINEL_A6[imm_logic]="a601000c"
SENTINEL_A6[bra_taken]="a601000d"
SENTINEL_A6[bne_not_taken]="a601000e"
SENTINEL_A6[bne_taken]="a601000f"
SENTINEL_A6[beq_taken]="a6010010"
SENTINEL_A6[beq_not_taken]="a6010011"
SENTINEL_A6[bpl_taken]="a6010012"
SENTINEL_A6[bmi_not_taken]="a6010013"
SENTINEL_A6[bvc_taken]="a6010014"
SENTINEL_A6[bvs_not_taken]="a6010015"
SENTINEL_A6[bge_taken]="a6010016"
SENTINEL_A6[blt_not_taken]="a6010017"
SENTINEL_A6[bgt_taken]="a6010018"
SENTINEL_A6[ble_not_taken]="a6010019"
SENTINEL_A6[bcc_taken]="a601001a"
SENTINEL_A6[bcc_not_taken]="a601001b"
SENTINEL_A6[bcs_taken]="a601001c"
SENTINEL_A6[bcs_not_taken]="a601001d"
SENTINEL_A6[scc_basic]="a601001e"
SENTINEL_A6[quick_ops]="a601001f"
SENTINEL_A6[dbra]="a6010020"
SENTINEL_A6[dbra_not_taken]="a6010021"

# ---- Run all test cases and score --------------------------------------------
PASS=0
FAIL=0
INFRA_FAIL=0
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
        fi
    else
        echo "METRIC opcode_${name}=-1"  # harness infrastructure issue
        FAIL=$((FAIL+1))
        INFRA_FAIL=$((INFRA_FAIL+1))
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
echo "METRIC score=$SCORE"
