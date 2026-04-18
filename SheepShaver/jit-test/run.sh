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

# Build base objects (make will fail on link due to missing JIT, that's ok)
make -j12 >"$RUN_DIR/build.log" 2>&1 || true
# Compile JIT-specific objects with USE_AARCH64_JIT
JITDIR="../kpx_cpu/src/cpu/jit/aarch64"
g++ -c -o obj/ppc-jit-aarch64.o "$JITDIR/ppc-jit-aarch64.cpp" -I"$JITDIR" -DHAVE_CONFIG_H -g -O2 -std=c++11 >>"$RUN_DIR/build.log" 2>&1
g++ -c -o obj/ppc-cpu.o ../kpx_cpu/src/cpu/ppc/ppc-cpu.cpp -I../include -I. -I.. -I../CrossPlatform -I../kpx_cpu/include -I../kpx_cpu/src -DHAVE_CONFIG_H -DUSE_AARCH64_JIT -g -O2 >>"$RUN_DIR/build.log" 2>&1
g++ -c -o obj/sheepshaver_glue.o ../kpx_cpu/sheepshaver_glue.cpp -I../include -I. -I.. -I../CrossPlatform -I../kpx_cpu/include -I../kpx_cpu/src -DHAVE_CONFIG_H -DUSE_AARCH64_JIT -g -O2 >>"$RUN_DIR/build.log" 2>&1
# Final link
if ! g++ -o SheepShaver obj/*.o -lpthread -lm -lSDL2 -lgtk-x11-2.0 -lgdk-x11-2.0 -lpangocairo-1.0 -latk-1.0 -lcairo -lgdk_pixbuf-2.0 -lgio-2.0 -lpangoft2-1.0 -lpango-1.0 -lgobject-2.0 -lglib-2.0 -lharfbuzz -lvncserver -lfontconfig -lfreetype >>"$RUN_DIR/build.log" 2>&1; then
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
TESTS[shift_slw]="38600001 38800004 7C652030"
TEST_ORDER+=(shift_slw)

# li r3,256; li r4,4; srw r5,r3,r4  → r5 = 16
TESTS[shift_srw]="38600100 38800004 7C652430"
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
TESTS[mul_basic]="38600007 38800006 7CA321D6"
TEST_ORDER+=(mul_basic)

# --- Rotate/mask ---
# li r3,0xFF; rlwinm r4,r3,4,0,27
TESTS[rlwinm_basic]="386000ff 546421b6"
TEST_ORDER+=(rlwinm_basic)

# --- NOP (sanity) ---
TESTS[nop]="60000000"
TEST_ORDER+=(nop)


# --- Negate ---
# li r3,42; neg r5,r3  → r5 = 0xFFFFFFD6
TESTS[neg_basic]="3860002a 7CA300D0"
TEST_ORDER+=(neg_basic)

# --- Arithmetic shift ---
# li r3,-1; li r4,16; sraw r5,r3,r4  → r5 = 0xFFFFFFFF, XER.CA=1
TESTS[sraw_signext]="3860ffff 38800010 7C652630"
TEST_ORDER+=(sraw_signext)

# --- Store/load round-trip ---
# li r3,0xBEEF; stw r3,0x100(r1); li r3,0; lwz r5,0x100(r1)
TESTS[stw_lwz]="3860beef 90610100 38600000 80A10100"
TEST_ORDER+=(stw_lwz)

# --- Byte store/load ---
# li r3,0x42; stb r3,0x200(r1); li r3,0; lbz r5,0x200(r1)
# stb rS,d(rA) = 0x98000000 | ... ; lbz rD,d(rA) = 0x88000000 | ...
TESTS[stb_lbz]="38600042 98610200 38600000 88A10200"
TEST_ORDER+=(stb_lbz)

# --- Halfword store/load ---
# li r3,0x1234; sth r3,0x300(r1); li r3,0; lhz r5,0x300(r1)
# sth = 0xB0000000; lhz = 0xA0000000
TESTS[sth_lhz]="38601234 B0610300 38600000 A0A10300"
TEST_ORDER+=(sth_lhz)

# --- Record form (sets CR0) ---
# li r3,42; addic. r5,r3,0  → CR0 should have GT bit set (positive result)
# addic. rD,rA,SIMM = 0x34000000 | (rD<<21) | (rA<<16) | SIMM
TESTS[addic_dot]="3860002a 34A30000"
TEST_ORDER+=(addic_dot)

# --- CR record with negative ---
# li r3,-1; add. r5,r3,r3  → r5 = -2, CR0.LT set
# add. rD,rA,rB = 0x7C000215 | (rD<<21) | (rA<<16) | (rB<<11)
TESTS[add_dot_neg]="3860ffff 7CA31A15"
TEST_ORDER+=(add_dot_neg)

# --- Divide ---
# li r3,100; li r4,7; divw r5,r3,r4  → r5 = 14
# divw rD,rA,rB = 0x7C0003D6 | (rD<<21) | (rA<<16) | (rB<<11)
TESTS[divw_basic]="38600064 38800007 7CA323D6"
TEST_ORDER+=(divw_basic)

# --- Counter branch (bctrl pattern) ---
# li r3,0; lis r4,hi(target); ori r4,r4,lo(target); mtctr r4; bctrl
# Can't easily encode absolute target, so just test mtctr+mfctr round-trip
# li r3,0xDEAD; mtctr r3; li r3,0; mfctr r5
# mtctr r3 = mtspr 9,r3 = 0x7C6903A6; mfctr r5 = mfspr 9,r5 = 0x7CA902A6
TESTS[mtctr_mfctr]="3860dead 7C6903A6 38600000 7CA902A6"
TEST_ORDER+=(mtctr_mfctr)

# --- XER carry flag ---
# Test addic (add immediate carrying): li r3,-1; addic r5,r3,2 → r5=1, XER.CA=1
# addic rD,rA,SIMM = 0x30000000 | (rD<<21) | (rA<<16) | (SIMM&0xFFFF)
TESTS[addic_carry]="3860ffff 30A30002"
TEST_ORDER+=(addic_carry)

# --- Extended ops ---
# adde (add extended with carry): li r3,5; li r4,3; addic r5,r3,-1 (set CA); adde r6,r4,r3
# adde rD,rA,rB = 0x7C000114 | (rD<<21) | (rA<<16) | (rB<<11)
TESTS[adde_carry]="38600005 38800003 30A3ffff 7CC42114"
TEST_ORDER+=(adde_carry)

# --- rlwimi (rotate left word immediate then mask insert) ---
# li r3,0xFF00; li r5,0x00FF; rlwimi r5,r3,0,24,31  → insert low byte of r3 into r5
# rlwimi rA,rS,SH,MB,ME = 0x50000000 | (rS<<21) | (rA<<16) | (SH<<11) | (MB<<6) | (ME<<1)
# rlwimi r5,r3,0,24,31 = 0x5065043E
TESTS[rlwimi_insert]="3860ff00 38a000ff 5065043E"
TEST_ORDER+=(rlwimi_insert)

# --- cntlzw (count leading zeros) ---
# li r3,0x100; cntlzw r5,r3  → r5 = 23 (0x100 = bit 8, 31-8=23)
# cntlzw rA,rS = 0x7C000034 | (rS<<21) | (rA<<16)
TESTS[cntlzw_basic]="38600100 7C650034"
TEST_ORDER+=(cntlzw_basic)

# --- extsh (extend sign halfword) ---
# li r3,0x8000; extsh r5,r3  → r5 = 0xFFFF8000
# extsh rA,rS = 0x7C000734 | (rS<<21) | (rA<<16)
TESTS[extsh_basic]="38608000 7C650734"
TEST_ORDER+=(extsh_basic)

# --- extsb (extend sign byte) ---
# li r3,0x80; extsb r5,r3  → r5 = 0xFFFFFF80
# extsb rA,rS = 0x7C000774 | (rS<<21) | (rA<<16)
TESTS[extsb_basic]="38600080 7C650774"
TEST_ORDER+=(extsb_basic)


# --- Carry/overflow ALU ---
# addc r5,r3,r4: li r3,-1; li r4,2; addc r5,r3,r4 → r5=1, XER.CA=1
# addc = 0x7C000014 | (5<<21)|(3<<16)|(4<<11)
TESTS[addc_basic]="3860ffff 38800002 7CA32014"
TEST_ORDER+=(addc_basic)

# subfc r5,r4,r3: li r3,10; li r4,3; subfc r5,r4,r3 → r5=7
# subfc = 0x7C000010 | (5<<21)|(4<<16)|(3<<11)
TESTS[subfc_basic]="3860000a 38800003 7CA41810"
TEST_ORDER+=(subfc_basic)

# subfic r5,r3,100: li r3,30; subfic r5,r3,100 → r5=70
# subfic = 0x20000000 | (5<<21)|(3<<16)|100
TESTS[subfic_basic]="3860001e 20A30064"
TEST_ORDER+=(subfic_basic)

# --- Logical ops ---
# andc r5,r3,r4: li r3,0xFF; li r4,0x0F; andc r5,r3,r4 → r5=0xF0
# andc = 0x7C000078 | (3<<21)|(5<<16)|(4<<11)
TESTS[andc_basic]="386000ff 3880000f 7C652078"
TEST_ORDER+=(andc_basic)

# nor r5,r3,r3: li r3,0; nor r5,r3,r3 → r5=0xFFFFFFFF
# nor = 0x7C0000F8 | (3<<21)|(5<<16)|(3<<11)
TESTS[nor_basic]="38600000 7C6518F8"
TEST_ORDER+=(nor_basic)

# nand r5,r3,r4: li r3,-1; li r4,0xFF; nand r5,r3,r4 → r5=0xFFFFFF00
# nand = 0x7C0003B8 | (3<<21)|(5<<16)|(4<<11)
TESTS[nand_basic]="3860ffff 388000ff 7C6523B8"
TEST_ORDER+=(nand_basic)

# eqv r5,r3,r4: li r3,0xFF; li r4,0xFF; eqv r5,r3,r4 → r5=0xFFFFFFFF (XNOR)
# eqv = 0x7C000238 | (3<<21)|(5<<16)|(4<<11)
TESTS[eqv_basic]="386000ff 388000ff 7C652238"
TEST_ORDER+=(eqv_basic)

# orc r5,r3,r4: li r3,0; li r4,0xFF; orc r5,r3,r4 → r5=0xFFFFFF00
# orc = 0x7C000338 | (3<<21)|(5<<16)|(4<<11)
TESTS[orc_basic]="38600000 388000ff 7C652338"
TEST_ORDER+=(orc_basic)

# --- Multiply/divide ---
# divwu r5,r3,r4: li r3,100; li r4,7; divwu r5,r3,r4 → r5=14
# divwu = 0x7C000396 | (5<<21)|(3<<16)|(4<<11)
TESTS[divwu_basic]="38600064 38800007 7CA32396"
TEST_ORDER+=(divwu_basic)

# mulhw r5,r3,r4: lis r3,0x1000; lis r4,0x1000; mulhw r5,r3,r4 → r5=0x01000000
# mulhw = 0x7C000096 | (5<<21)|(3<<16)|(4<<11)
TESTS[mulhw_basic]="3C601000 3C801000 7CA32096"
TEST_ORDER+=(mulhw_basic)

# --- Rotate ---
# rlwnm r5,r3,r4,0,31: li r3,1; li r4,8; rlwnm r5,r3,r4,0,31 → r5=256
# rlwnm = 0x5C000000 | (3<<21)|(5<<16)|(4<<11)|(0<<6)|(31<<1)
TESTS[rlwnm_basic]="38600001 38800008 5C65203E"
TEST_ORDER+=(rlwnm_basic)

# --- CR logical ---
# cmpwi cr0,r3,0; cmpwi cr1,r4,0; crand 0,0,4 (AND cr0.lt with cr1.lt)
# cmpwi cr0,r3,0 = 0x2C030000; cmpwi cr1,r4,0 = 0x2C840000
# crand 0,0,4 = 0x4C000202
TESTS[crand_basic]="3860ffff 3880ffff 2C030000 2C840000 4C000202"
TEST_ORDER+=(crand_basic)

# crxor 0,0,0 (clear CR bit 0) then cror 0,0,4 (OR)
# crxor 0,0,0 = 0x4C000182; cror 0,0,4 = 0x4C000382
TESTS[crxor_cror]="3860ffff 2C030000 4C000182 4C000382"
TEST_ORDER+=(crxor_cror)

# --- FP operations ---
# fadd: load 2.0 and 3.0 via lis/stw/lfs, add them
# This is complex in hex. Use simpler approach: stfd a known pattern.
# li r3,0x4000; stw r3,0x100(r1); li r3,0; stw r3,0x104(r1); lfd f1,0x100(r1)
# That stores 0x40000000_00000000 as a double = 2.0
TESTS[fp_add]="3C604000 90610100 38600000 90610104 C8210100 FC200890 FC211028 D8210108"
TEST_ORDER+=(fp_add)

# --- Load/store indexed ---
# lwzx: li r3,0xBEEF; stw r3,0(r1); li r4,0; lwzx r5,r1,r4
# lwzx = 0x7C00002E | (5<<21)|(1<<16)|(4<<11)
TESTS[lwzx_basic]="3860beef 90610000 38800000 7CA1202E"
TEST_ORDER+=(lwzx_basic)

# lbzx: li r3,0x42; stb r3,0x200(r1); li r4,0x200; lbzx r5,r1,r4
# lbzx = 0x7C0000AE | (5<<21)|(1<<16)|(4<<11)
TESTS[lbzx_basic]="38600042 98610200 38800200 7CA120AE"
TEST_ORDER+=(lbzx_basic)

# --- Record forms ---
# or. r5,r3,r3 with negative value (sets CR0.LT)
# or. = 0x7C000379 | (3<<21)|(5<<16)|(3<<11)
TESTS[or_dot_neg]="3860ffff 7C651B79"
TEST_ORDER+=(or_dot_neg)

# and. r5,r3,r4 with zero result (sets CR0.EQ)
# and. = 0x7C000039 | (3<<21)|(5<<16)|(4<<11)
TESTS[and_dot_zero]="38600ff0 3880000f 7C651839"
TEST_ORDER+=(and_dot_zero)

# --- mftb (time base) ---
# mftb r5 → should return non-zero
# mftb = 0x7C0002E6 | (5<<21) with TBR=268
TESTS[mftb_basic]="7CA602A6"
TEST_ORDER+=(mftb_basic)

# --- cntlzw edge cases ---
# cntlzw r5,r3: li r3,0; cntlzw r5,r3 → r5=32
TESTS[cntlzw_zero]="38600000 7C650034"
TEST_ORDER+=(cntlzw_zero)

# cntlzw r5,r3: li r3,-1; cntlzw r5,r3 → r5=0
TESTS[cntlzw_allones]="3860ffff 7C650034"
TEST_ORDER+=(cntlzw_allones)

# --- bdnz loop (longer) ---
# li r3,0; li r4,10; mtctr r4; addi r3,r3,1; bdnz -4
TESTS[bdnz_10]="38600000 3880000a 7C8903A6 38630001 4200FFFC"
TEST_ORDER+=(bdnz_10)

# --- compare unsigned ---
# cmplwi cr0,r3,100: li r3,200; cmplwi cr0,r3,100
# cmplwi = 0x28030064
TESTS[cmplwi_basic]="386000c8 28030064"
TEST_ORDER+=(cmplwi_basic)

# --- stwu/lwzu stack frame ---
# stwu r1,-16(r1); lwzu r3,16(r1)
TESTS[stwu_lwzu]="9421FFF0 84610010"
TEST_ORDER+=(stwu_lwzu)

# --- extsh/extsb edge ---
# li r3,0x7FFF; extsh r5,r3 → r5=0x7FFF (positive, no sign ext)
TESTS[extsh_positive]="38607fff 7C650734"
TEST_ORDER+=(extsh_positive)

# li r3,0x7F; extsb r5,r3 → r5=0x7F
TESTS[extsb_positive]="3860007f 7C650774"
TEST_ORDER+=(extsb_positive)

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
