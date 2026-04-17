#!/bin/bash
# SheepShaver PPC opcode equivalence test harness
# Phase 1: interpreter determinism validation
# Phase 2+: interpreter vs JIT comparison
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UNIX_DIR="$(cd "$SCRIPT_DIR/../src/Unix" && pwd)"
RUN_DIR="/tmp/ss-jit-test-$$"
mkdir -p "$RUN_DIR"

# ---- Build -------------------------------------------------------------------
cd "$UNIX_DIR"

if [ ! -x ./configure ] && [ -x ./autogen.sh ]; then
    NO_CONFIGURE=1 ./autogen.sh >"$RUN_DIR/autogen.log" 2>&1 || true
fi

if [ ! -f config.h ] || [ ! -f Makefile ]; then
    if [ ! -x ./configure ]; then
        echo "METRIC build_ok=0"
        echo "METRIC pass=0"
        echo "METRIC fail=0"
        echo "METRIC total=0"
        echo "METRIC score=0"
        rm -rf "$RUN_DIR"
        exit 0
    fi
    if ! ./configure --enable-sdl-video --enable-sdl-audio \
      >"$RUN_DIR/configure.log" 2>&1; then
        echo "METRIC build_ok=0"
        echo "METRIC pass=0"
        echo "METRIC fail=0"
        echo "METRIC total=0"
        echo "METRIC score=0"
        rm -rf "$RUN_DIR"
        exit 0
    fi
fi

if ! make -j12 >"$RUN_DIR/build.log" 2>&1; then
    echo "METRIC build_ok=0"
    echo "METRIC pass=0"
    echo "METRIC fail=0"
    echo "METRIC total=0"
    echo "METRIC score=0"
    tail -20 "$RUN_DIR/build.log" >&2
    rm -rf "$RUN_DIR"
    exit 0
fi
echo "METRIC build_ok=1"

BIN="$UNIX_DIR/SheepShaver"
if [ ! -x "$BIN" ]; then
    echo "METRIC build_ok=0"
    echo "METRIC pass=0 fail=0 total=0 score=0"
    rm -rf "$RUN_DIR"
    exit 0
fi

# ---- Xvfb -------------------------------------------------------------------
if ! pgrep -x Xvfb >/dev/null 2>&1; then
    Xvfb :99 -screen 0 640x480x24 &>/dev/null &
    sleep 1
fi

# ---- Test runner -------------------------------------------------------------
run_ppc_test() {
    local name="$1"
    local hex="$2"   # space-separated 32-bit PPC hex words
    local outfile="$3"

    local td="$RUN_DIR/test-${name}"
    mkdir -p "$td"

    # Minimal prefs — no ROM needed for test mode (SS_TEST_HEX bypasses boot)
    cat > "$td/prefs" <<EOF
nogui true
nosound true
nocdrom true
noclipconversion true
ramsize 16777216
EOF

    # Kill any stale SheepShaver processes
    pkill -f "SheepShaver --config $td/prefs" 2>/dev/null || true

    # Run with test mode env vars
    SDL_VIDEODRIVER=x11 DISPLAY=:99 HOME="$td" \
      SS_TEST_HEX="$hex" \
      SS_TEST_DUMP=1 \
      timeout -k 5s 15s "$BIN" --config "$td/prefs" \
      > "$td/emu.log" 2>&1 || true

    # Extract REGDUMP line
    grep "^REGDUMP:" "$td/emu.log" > "$outfile" 2>/dev/null || true
}

# ---- Test vectors ------------------------------------------------------------
# PPC instruction encodings (big-endian 32-bit words, space-separated)
# Each vector ends implicitly with blr (appended by the harness in C)

declare -A TESTS
declare -a TEST_ORDER

# --- Integer ALU ---
# li r3,100; li r4,200; add r5,r3,r4
TESTS[alu_add]="38600064 388000c8 7CA32214"
TEST_ORDER+=(alu_add)

# li r3,50; li r4,30; subf r5,r4,r3  (r5 = r3 - r4 = 20)
TESTS[alu_sub]="38600032 3880001e 7CA42050"
TEST_ORDER+=(alu_sub)

# li r3,0xFF; li r4,0x0F; and r5,r3,r4
TESTS[alu_and]="386000ff 3880000f 7C651838"
TEST_ORDER+=(alu_and)

# li r3,0xA0; li r4,0x05; or r5,r3,r4
TESTS[alu_or]="386000a0 38800005 7C651B78"
TEST_ORDER+=(alu_or)

# li r3,0xFF; li r4,0x0F; xor r5,r3,r4
TESTS[alu_xor]="386000ff 3880000f 7C651A78"
TEST_ORDER+=(alu_xor)

# --- Load immediate ---
# lis r3,0x1234; ori r3,r3,0x5678  → r3 = 0x12345678
TESTS[li_wide]="3C601234 60635678"
TEST_ORDER+=(li_wide)

# --- Shift ---
# li r3,1; li r4,4; slw r5,r3,r4  → r5 = 16
TESTS[shift_slw]="38600001 38800004 7C642030"
TEST_ORDER+=(shift_slw)

# li r3,256; li r4,4; srw r5,r3,r4  → r5 = 16
TESTS[shift_srw]="38600100 38800004 7C642430"
TEST_ORDER+=(shift_srw)

# --- Compare + branch ---
# li r3,10; li r4,10; cmpw cr0,r3,r4; beq +8; li r5,1; b +8; li r5,2; nop
TESTS[cmp_beq]="3860000a 3880000a 7C032000 41820008 38a00001 48000008 38a00002 60000000"
TEST_ORDER+=(cmp_beq)

# --- Counter loop (bdnz) ---
# li r3,0; li r4,5; mtctr r4; addi r3,r3,1; bdnz -4
TESTS[bdnz_loop]="38600000 38800005 7C8903A6 38630001 4200FFFC"
TEST_ORDER+=(bdnz_loop)

# --- Multiply ---
# li r3,7; li r4,6; mullw r5,r3,r4  → r5 = 42
TESTS[mul_basic]="38600007 38800006 7CA31D96"
TEST_ORDER+=(mul_basic)

# --- Rotate/mask ---
# li r3,0xFF; rlwinm r4,r3,4,0,27
TESTS[rlwinm_basic]="386000ff 546421b6"
TEST_ORDER+=(rlwinm_basic)

# --- NOP (sanity) ---
TESTS[nop]="60000000"
TEST_ORDER+=(nop)

# ---- Execute all tests -------------------------------------------------------
PASS=0
FAIL=0
TOTAL=${#TEST_ORDER[@]}

for name in "${TEST_ORDER[@]}"; do
    hex="${TESTS[$name]}"
    out1="$RUN_DIR/${name}-run1.txt"
    out2="$RUN_DIR/${name}-run2.txt"

    # Run twice for determinism check (Phase 1)
    run_ppc_test "$name" "$hex" "$out1"
    run_ppc_test "${name}_r2" "$hex" "$out2"

    if [ -s "$out1" ] && [ -s "$out2" ]; then
        if diff -q "$out1" "$out2" >/dev/null 2>&1; then
            echo "METRIC opcode_${name}=1"
            PASS=$((PASS+1))
        else
            echo "METRIC opcode_${name}=0"
            echo "  DIFF for $name:" >&2
            diff "$out1" "$out2" >&2 || true
            FAIL=$((FAIL+1))
        fi
    else
        echo "METRIC opcode_${name}=-1"
        FAIL=$((FAIL+1))
        # Show what happened
        if [ ! -s "$out1" ]; then
            echo "  $name: no REGDUMP from run 1" >&2
            tail -5 "$RUN_DIR/test-${name}/emu.log" >&2 2>/dev/null || true
        fi
    fi
done

SCORE=$(( TOTAL > 0 ? PASS * 100 / TOTAL : 0 ))
echo "METRIC pass=$PASS"
echo "METRIC fail=$FAIL"
echo "METRIC total=$TOTAL"
echo "METRIC score=$SCORE"

rm -rf "$RUN_DIR"
