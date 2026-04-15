#!/bin/bash
# BasiliskII AArch64 JIT Opcode Correctness Test
# Autoresearch harness: compare interpreter vs JIT register state for each opcode class
set -euo pipefail

UNIX_DIR="$(cd "$(dirname "$0")/BasiliskII/src/Unix" && pwd)"
ROM="/workspace/projects/rpi-basilisk2-sdl2-nox/Quadra800.ROM"
DISK="/workspace/fixtures/basilisk/images/HD200MB"
RUN_DIR="/tmp/ar-jit-opcodes-$$"
mkdir -p "$RUN_DIR"

# ---- Build -------------------------------------------------------------------
cd "$UNIX_DIR"
if [ ! -f Makefile ]; then
    ac_cv_have_asm_extended_signals=yes \
      ./configure --with-uae-core=2021 --enable-aarch64-jit-experimental --disable-vosf \
      >/dev/null 2>&1
fi
if ! make -j12 >"$RUN_DIR/build.log" 2>&1; then
    echo "METRIC build_ok=0"
    echo "METRIC score=0"
    tail -20 "$RUN_DIR/build.log" >&2
    rm -rf "$RUN_DIR"
    exit 0
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
    local hex_code="$2"  # hex M68K bytecode, space-separated words
    local use_jit="$3"   # "true" or "false"
    local outfile="$4"

    local td="$RUN_DIR/test-${name}-jit${use_jit}"
    mkdir -p "$td"

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

    # Set env to trace register state at STOP instruction
    # B2_JIT_PCTRACE_STOP=1 makes the emulator dump regs when STOP is executed
    SDL_VIDEODRIVER=x11 DISPLAY=:99 HOME="$td" \
      B2_TEST_HEX="$hex_code" \
      B2_TEST_DUMP=1 \
      timeout 30 "$UNIX_DIR/BasiliskII" --config "$td/prefs" \
      > "$td/emu.log" 2>&1 || true

    # Extract register dump from log
    grep "^REGDUMP:" "$td/emu.log" > "$outfile" 2>/dev/null || true
}

# Start Xvfb if needed
if ! pgrep -x Xvfb >/dev/null 2>&1; then
    Xvfb :99 -screen 0 640x480x24 &>/dev/null &
    sleep 1
fi

# ---- Define test cases -------------------------------------------------------
# Format: name|hex_words (M68K big-endian, STOP #0x2700 appended automatically)
# Each test sets up known state and exercises one opcode class.

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

# ---- Run all test cases and score --------------------------------------------
PASS=0
FAIL=0
TOTAL=${#TESTS[@]}

for name in "${!TESTS[@]}"; do
    hex="${TESTS[$name]}"
    ifile="$RUN_DIR/${name}-interp.txt"
    jfile="$RUN_DIR/${name}-jit.txt"

    run_test "$name" "$hex" "false" "$ifile"
    run_test "$name" "$hex" "true"  "$jfile"

    if [ -s "$ifile" ] && [ -s "$jfile" ]; then
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
        echo "METRIC opcode_${name}=-1"  # no output (test infra issue)
        FAIL=$((FAIL+1))
    fi
done

# Score: fraction of passing tests (0-100)
SCORE=$(( PASS * 100 / TOTAL ))
echo "METRIC pass=$PASS"
echo "METRIC fail=$FAIL"
echo "METRIC total=$TOTAL"
echo "METRIC score=$SCORE"

rm -rf "$RUN_DIR"
