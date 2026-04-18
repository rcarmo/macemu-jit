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
g++ -c -o obj/ppc-jit.o "$JITDIR/ppc-jit.cpp" -I"$JITDIR" -DHAVE_CONFIG_H -g -O2 -std=c++11 >>"$RUN_DIR/build.log" 2>&1
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


# --- FP operations ---
# fneg: store 2.0 as double, negate it, check sign
# 2.0 double = 0x40000000_00000000
TESTS[fp_neg]="3C604000 90610100 38600000 90610104 C8210100 FC2000D0 D8210108"
TEST_ORDER+=(fp_neg)

# fabs: store -2.0 (0xC0000000_00000000), take abs
TESTS[fp_abs]="3C60C000 90610100 38600000 90610104 C8210100 FC200210 D8210108"
TEST_ORDER+=(fp_abs)

# --- Branch ---
# bl +8; nop; mfspr r5,LR → r5 should equal address of nop
# bl = 0x48000009 (LK=1, +8 bytes)... actually bl offset must be from current insn
# bl +8 = 0x48000009 (branch 8 bytes forward, link)
TESTS[bl_basic]="7CA802A6"
TEST_ORDER+=(bl_basic)

# --- srawi ---
# li r3,-128; srawi r5,r3,3 → r5 = -16 = 0xFFFFFFF0
# srawi r5,r3,3: XO=31 XO=824, rS=3 rA=5 SH=3 = 0x7C651E70
TESTS[srawi_basic]="3860ff80 7C651E70"
TEST_ORDER+=(srawi_basic)

# --- lha (sign-extending halfword) ---
# li r3,0x8000; sth r3,0x300(r1); lha r5,0x300(r1) → r5 = 0xFFFF8000
TESTS[lha_signext]="38608000 B0610300 A8A10300"
TEST_ORDER+=(lha_signext)

# --- lmw/stmw ---
# li r28,0x28; li r29,0x29; li r30,0x30; li r31,0x31; stmw r28,0x400(r1); 
# li r28,0; li r29,0; li r30,0; li r31,0; lmw r28,0x400(r1)
TESTS[lmw_stmw]="3B800028 3BA00029 3BC00030 3BE00031 BF810400 3B800000 3BA00000 3BC00000 3BE00000 BB810400"
TEST_ORDER+=(lmw_stmw)

# --- mcrf ---
# cmpwi cr0,r3,0 (r3=-1 → LT); mcrf cr1,cr0; then check cr1 has LT
# cmpwi cr0,r3,0 = 0x2C030000; mcrf cr1,cr0 = 0x4C840000
TESTS[mcrf_basic]="3860ffff 2C030000 4C840000"
TEST_ORDER+=(mcrf_basic)

# --- subfe (simplified) ---
# li r3,5; li r4,10; subfe r5,r3,r4 → r5 = r4 + ~r3 + CA ≈ 4 (simplified as subf)
# subfe = XO=31 XO=136: 0x7C000110 | (5<<21)|(3<<16)|(4<<11) = 0x7CA32110
TESTS[subfe_basic]="38600005 3880000a 7CA32110"
TEST_ORDER+=(subfe_basic)

# --- addze (simplified) ---
# li r3,42; addze r5,r3 → r5 = 42 (simplified: ignores CA)
# addze = XO=31 XO=202: 0x7C000194 | (5<<21)|(3<<16) = 0x7CA30194
TESTS[addze_basic]="3860002a 7CA30194"
TEST_ORDER+=(addze_basic)

# --- dcbz ---
# stw r3,0x500(r1); li r4,0x500; dcbz r1,r4; lwz r5,0x500(r1) → r5=0
# dcbz = XO=31 XO=1014: 0x7C0007EC | (0<<21)|(1<<16)|(4<<11) = 0x7C0127EC
TESTS[dcbz_basic]="3860beef 90610500 38800500 7C0127EC 80A10500"
TEST_ORDER+=(dcbz_basic)

# --- xori ---
# li r3,0xFF; xori r5,r3,0xF0 → r5=0x0F
# xori rA=5,rS=3,UIMM=0xF0: 0x686500F0
TESTS[xori_basic]="386000ff 686500F0"
TEST_ORDER+=(xori_basic)

# --- FP compare ---
# Store 1.0 and 2.0, compare: fcmpu cr0,f0,f1 → CR0.LT
TESTS[fcmpu_basic]="3C603F80 90610100 38600000 90610104 C0010100 3C604000 90610108 C021010C C8010100 C8210108 FC000000"
TEST_ORDER+=(fcmpu_basic)

# --- FP mul ---
# Store 3.0 (0x40080000) and 4.0 (0x40100000), multiply → 12.0
TESTS[fp_mul]="3C604008 90610100 38600000 90610104 C8010100 3C604010 90610108 90610104 C8210108 FC000072 D8010110"
TEST_ORDER+=(fp_mul)

# --- isync (should be NOP) ---
TESTS[isync_basic]="4C00012C 60000000"
TEST_ORDER+=(isync_basic)

# --- eieio (should be NOP) ---
# eieio = 0x7C0006AC
TESTS[eieio_basic]="7C0006AC 60000000"
TEST_ORDER+=(eieio_basic)

# --- sync (should be NOP) ---
# sync = 0x7C0004AC
TESTS[sync_basic]="7C0004AC 60000000"
TEST_ORDER+=(sync_basic)


# ============================================================
# FUZZING VECTORS — edge cases, boundary values, corner cases
# ============================================================

# --- Integer overflow/underflow ---
# add with MAX_INT + 1 → overflow
TESTS[fuzz_add_overflow]="3C607FFF 6063FFFF 38800001 7CA32214"
TEST_ORDER+=(fuzz_add_overflow)

# sub producing MIN_INT
TESTS[fuzz_sub_minint]="3C608000 38800001 7CA42050"
TEST_ORDER+=(fuzz_sub_minint)

# neg of MIN_INT (0x80000000) → still 0x80000000 (overflow)
TESTS[fuzz_neg_minint]="3C608000 7CA300D0"
TEST_ORDER+=(fuzz_neg_minint)

# --- Shift edge cases ---
# slw by 0 (no shift)
TESTS[fuzz_slw_zero]="3860FFFF 38800000 7C652030"
TEST_ORDER+=(fuzz_slw_zero)

# slw by 31 (max valid shift)
TESTS[fuzz_slw_31]="38600001 3880001F 7C652030"
TEST_ORDER+=(fuzz_slw_31)

# slw by 32 (should produce 0 on PPC)
TESTS[fuzz_slw_32]="38600001 38800020 7C652030"
TEST_ORDER+=(fuzz_slw_32)

# srw by 32 (should produce 0)
TESTS[fuzz_srw_32]="3860FFFF 38800020 7C652430"
TEST_ORDER+=(fuzz_srw_32)

# sraw by 31 (sign bit fill)
TESTS[fuzz_sraw_31]="3C608000 3880001F 7C652630"
TEST_ORDER+=(fuzz_sraw_31)

# sraw of 0 by any amount
TESTS[fuzz_sraw_zero]="38600000 38800010 7C652630"
TEST_ORDER+=(fuzz_sraw_zero)

# --- rlwinm edge cases ---
# rotate by 0, full mask
TESTS[fuzz_rlwinm_nop]="3860DEAD 5463003E"
TEST_ORDER+=(fuzz_rlwinm_nop)

# rotate by 16, swap halfwords: rlwinm r3,r3,16,0,31
TESTS[fuzz_rlwinm_swap16]="3C6012AB 606360CD 5463801E"
TEST_ORDER+=(fuzz_rlwinm_swap16)

# rlwinm with wrapping mask (MB > ME)
# rlwinm r4,r3,0,28,3 → mask = 0xF000000F
TESTS[fuzz_rlwinm_wrapmask]="3C60ABCD 6063EF01 5464001E"
TEST_ORDER+=(fuzz_rlwinm_wrapmask)

# --- Multiply edge cases ---
# multiply -1 × -1 = 1
TESTS[fuzz_mul_neg1]="3860FFFF 3880FFFF 7CA321D6"
TEST_ORDER+=(fuzz_mul_neg1)

# multiply MAX_INT × 2 → overflow (low word)
TESTS[fuzz_mul_overflow]="3C607FFF 6063FFFF 38800002 7CA321D6"
TEST_ORDER+=(fuzz_mul_overflow)

# mulhw: high word of large multiply
TESTS[fuzz_mulhw_big]="3C607FFF 6063FFFF 3C807FFF 6084FFFF 7CA32096"
TEST_ORDER+=(fuzz_mulhw_big)

# --- Divide edge cases ---
# divw MIN_INT / -1 → undefined (PPC produces 0)
TESTS[fuzz_divw_minint]="3C608000 3880FFFF 7CA323D6"
TEST_ORDER+=(fuzz_divw_minint)

# divw by 0 → undefined
TESTS[fuzz_divw_zero]="38600064 38800000 7CA323D6"
TEST_ORDER+=(fuzz_divw_zero)

# divwu large / small
TESTS[fuzz_divwu_large]="3C60FFFF 6063FFFF 38800002 7CA32396"
TEST_ORDER+=(fuzz_divwu_large)

# --- Compare edge cases ---
# cmpw: equal values
TESTS[fuzz_cmpw_equal]="3860002A 3880002A 7C032000"
TEST_ORDER+=(fuzz_cmpw_equal)

# cmpw: MAX_INT vs MIN_INT
TESTS[fuzz_cmpw_extremes]="3C607FFF 6063FFFF 3C808000 7C032000"
TEST_ORDER+=(fuzz_cmpw_extremes)

# cmplwi: 0 vs 0
TESTS[fuzz_cmplwi_zero]="38600000 28030000"
TEST_ORDER+=(fuzz_cmplwi_zero)

# --- CR logical edge cases ---
# crxor bit with itself → always 0
TESTS[fuzz_crxor_self]="3860FFFF 2C030000 4C000182"
TEST_ORDER+=(fuzz_crxor_self)

# creqv bit with itself → always 1
TESTS[fuzz_creqv_self]="3860FFFF 2C030000 4C000242"
TEST_ORDER+=(fuzz_creqv_self)

# --- Load/store with displacement 0 ---
TESTS[fuzz_lwz_disp0]="3860BEEF 90610000 80A10000"
TEST_ORDER+=(fuzz_lwz_disp0)

# --- Load/store negative displacement ---
# stwu r1,-32(r1) then lwz from that address
TESTS[fuzz_stwu_neg]="9421FFE0 80610000"
TEST_ORDER+=(fuzz_stwu_neg)

# --- Byte operations with 0xFF ---
TESTS[fuzz_stb_ff]="386000FF 98610200 88A10200"
TEST_ORDER+=(fuzz_stb_ff)

# --- Halfword sign extension edge ---
# lha of 0x7FFF (positive, no sign ext)
TESTS[fuzz_lha_pos]="38607FFF B0610300 A8A10300"
TEST_ORDER+=(fuzz_lha_pos)

# lha of 0xFFFF (-1 sign extended)
TESTS[fuzz_lha_neg1]="3860FFFF B0610300 A8A10300"
TEST_ORDER+=(fuzz_lha_neg1)

# --- Record form with zero result ---
# add. 0 + 0 → CR0.EQ should be set
TESTS[fuzz_add_dot_zero]="38600000 38800000 7CA32215"
TEST_ORDER+=(fuzz_add_dot_zero)

# --- Carry chain ---
# addic -1,1 → 0 with CA=1; addze r5,r0 → r5 = 0 + CA = 1
TESTS[fuzz_carry_chain]="3860FFFF 30630001 7CA00194"
TEST_ORDER+=(fuzz_carry_chain)

# subfic 0,0 → 0 with CA=1; addze r5,r0 → 1
TESTS[fuzz_subfic_carry]="20600000 7CA00194"
TEST_ORDER+=(fuzz_subfic_carry)

# --- FP edge cases ---
# fneg of 0.0 → -0.0 (different bit pattern)
TESTS[fuzz_fneg_zero]="38600000 90610100 90610104 C8210100 FC2000D0 D8210108"
TEST_ORDER+=(fuzz_fneg_zero)

# fabs of -0.0 → +0.0
TESTS[fuzz_fabs_negzero]="3C608000 90610100 38600000 90610104 C8210100 FC200210 D8210108"
TEST_ORDER+=(fuzz_fabs_negzero)

# --- Multi-register operations ---
# lmw/stmw with r31 only (minimum case)
TESTS[fuzz_lmw_r31]="3BE0CAFE BFE10400 3BE00000 BBE10400"
TEST_ORDER+=(fuzz_lmw_r31)

# --- bdnz with count=1 (single iteration then fall through) ---
TESTS[fuzz_bdnz_one]="38600000 38800001 7C8903A6 38630001 4200FFFC"
TEST_ORDER+=(fuzz_bdnz_one)

# --- cntlzw of powers of 2 ---
TESTS[fuzz_cntlzw_bit0]="3C608000 7C650034"
TEST_ORDER+=(fuzz_cntlzw_bit0)

TESTS[fuzz_cntlzw_bit31]="38600001 7C650034"
TEST_ORDER+=(fuzz_cntlzw_bit31)

# --- extsb/extsh boundary ---
# extsb of 0x80 → 0xFFFFFF80
TESTS[fuzz_extsb_boundary]="38600080 7C650774"
TEST_ORDER+=(fuzz_extsb_boundary)

# extsb of 0x7F → 0x0000007F (no extension)
TESTS[fuzz_extsb_noext]="3860007F 7C650774"
TEST_ORDER+=(fuzz_extsb_noext)

# extsh of 0x8000 → 0xFFFF8000
TESTS[fuzz_extsh_boundary]="38608000 7C650734"
TEST_ORDER+=(fuzz_extsh_boundary)

# --- All-ones patterns ---
TESTS[fuzz_and_allones]="3860FFFF 3880FFFF 7C651838"
TEST_ORDER+=(fuzz_and_allones)

TESTS[fuzz_or_allzero]="38600000 38800000 7C651B78"
TEST_ORDER+=(fuzz_or_allzero)

TESTS[fuzz_xor_same]="3860ABCD 7C651A78"
TEST_ORDER+=(fuzz_xor_same)


# ============================================================
# FULL COVERAGE VECTORS — every remaining untested opcode class
# ============================================================

# --- FP single precision ---
# fadds: 2.0f + 3.0f via stw/lfs pattern
TESTS[fp_fadds]="3C604000 90610100 38600000 90610104 C0010100 3C604040 90610108 C0210108 EC211028 D0010110"
TEST_ORDER+=(fp_fadds)

# --- FP fused multiply-add ---
# fmadd f0,f1,f2,f3 = f1*f2+f3
TESTS[fp_fmadd]="3C604000 90610100 38600000 90610104 C8210100 FC00083A D8010108"
TEST_ORDER+=(fp_fmadd)

# --- frsp (round to single) ---
TESTS[fp_frsp]="3C604000 90610100 38600000 90610104 C8210100 FC000018 D8010108"
TEST_ORDER+=(fp_frsp)

# --- fsel (select) ---
TESTS[fp_fsel]="3C604000 90610100 38600000 90610104 C8010100 C8210100 FC00082E D8010108"
TEST_ORDER+=(fp_fsel)

# --- mffs/mtfsf ---
TESTS[fp_mffs]="FC00048E D8010100"
TEST_ORDER+=(fp_mffs)

# --- Indexed load/store ---
# stwx: li r3,0xDEAD; li r4,0; stwx r3,r1,r4; lwzx r5,r1,r4
TESTS[stwx_basic]="3860dead 38800000 7C61212E 7CA1202E"
TEST_ORDER+=(stwx_basic)

# stbx/lbzx round-trip
TESTS[stbx_lbzx]="38600042 38800100 7C6120AE 7CA120AE"
TEST_ORDER+=(stbx_lbzx)

# sthx/lhzx round-trip  
TESTS[sthx_lhzx]="38601234 38800200 7C61232E 7CA1232E"
TEST_ORDER+=(sthx_lhzx)

# lhax (sign-extending indexed)
TESTS[lhax_basic]="38608000 B0610300 38800300 7CA122AE"
TEST_ORDER+=(lhax_basic)

# --- Byte-reversed loads ---
# lhbrx: store 0x1234 normally, load byte-reversed → 0x3412
TESTS[lhbrx_basic]="38601234 B0610400 38800400 7CA1262C"
TEST_ORDER+=(lhbrx_basic)

# lwbrx
TESTS[lwbrx_basic]="3C60DEAD 6063BEEF 90610500 38800500 7CA1242C"
TEST_ORDER+=(lwbrx_basic)

# --- Update forms ---
# lbzu
TESTS[lbzu_basic]="38600042 98610200 388101FF 8CA40001"
TEST_ORDER+=(lbzu_basic)

# sthu
TESTS[sthu_basic]="38601234 388102FE B0640002"
TEST_ORDER+=(sthu_basic)

# --- Carry extended ---
# adde: set CA via addic, then adde
TESTS[adde_chain]="3860FFFF 30630001 38800005 7CA42114"
TEST_ORDER+=(adde_chain)

# subfe
TESTS[subfe_chain]="3860FFFF 30630001 38800005 38600003 7CA32110"
TEST_ORDER+=(subfe_chain)

# addme: rA + CA - 1
TESTS[addme_basic]="3860FFFF 30630001 38600005 7CA301D4"
TEST_ORDER+=(addme_basic)

# addze: rA + CA
TESTS[addze_chain]="3860FFFF 30630001 38600005 7CA30194"
TEST_ORDER+=(addze_chain)

# subfze: ~rA + CA
TESTS[subfze_basic]="3860FFFF 30630001 38600005 7CA30190"
TEST_ORDER+=(subfze_basic)

# subfme: ~rA + CA - 1
TESTS[subfme_basic]="3860FFFF 30630001 38600005 7CA301D0"
TEST_ORDER+=(subfme_basic)

# --- mulhwu (unsigned high multiply) ---
TESTS[mulhwu_basic]="3C60FFFF 6063FFFF 3C80FFFF 6084FFFF 7CA32016"
TEST_ORDER+=(mulhwu_basic)

# --- mfcr/mtcrf round-trip ---
TESTS[mfcr_mtcrf]="3860FFFF 2C030000 7CA00026 7CA0F120"
TEST_ORDER+=(mfcr_mtcrf)

# --- mcrxr ---
TESTS[mcrxr_basic]="3860FFFF 30630001 7C200400"
TEST_ORDER+=(mcrxr_basic)

# --- Conditional bclr ---
# Set CR0.LT via cmpwi, then beqlr (should NOT branch since LT not EQ)
TESTS[bclr_cond]="3860FFFF 2C030000 4D820020"
TEST_ORDER+=(bclr_cond)

# --- orc ---
TESTS[orc_basic]="38600000 388000FF 7C652338"
TEST_ORDER+=(orc_basic)

# --- eqv ---
TESTS[eqv_basic2]="386000FF 388000FF 7C652238"
TEST_ORDER+=(eqv_basic2)

# --- andc ---
TESTS[andc_basic2]="386000FF 3880000F 7C652078"
TEST_ORDER+=(andc_basic2)

# --- nor ---
TESTS[nor_basic2]="38600000 38800000 7C6518F8"
TEST_ORDER+=(nor_basic2)

# --- nand ---
TESTS[nand_basic2]="3860FFFF 388000FF 7C6523B8"
TEST_ORDER+=(nand_basic2)

# --- rlwimi ---
TESTS[rlwimi_basic2]="3860FF00 38A000FF 5065043E"
TEST_ORDER+=(rlwimi_basic2)

# --- srawi edge ---
TESTS[srawi_neg]="3860FF80 7C651E70"
TEST_ORDER+=(srawi_neg)

# --- subfic ---
TESTS[subfic_basic2]="3860001E 20A30064"
TEST_ORDER+=(subfic_basic2)

# --- addic carry ---
TESTS[addic_ca]="3860FFFF 30A30001"
TEST_ORDER+=(addic_ca)

# --- mfspr/mtspr XER ---
TESTS[mfspr_xer]="7CA102A6"
TEST_ORDER+=(mfspr_xer)

# --- cmpw with negative ---
TESTS[cmpw_neg]="3860FFFF 38800001 7C032000"
TEST_ORDER+=(cmpw_neg)

# --- cmplw ---
TESTS[cmplw_basic]="3860FFFF 38800001 7C032040"
TEST_ORDER+=(cmplw_basic)

# --- lmw with 4 regs ---
TESTS[lmw_4regs]="3B800011 3BA00022 3BC00033 3BE00044 BF810400 3B800000 3BA00000 3BC00000 3BE00000 BB810400"
TEST_ORDER+=(lmw_4regs)

# --- divwu ---
TESTS[divwu_basic2]="3C60FFFF 6063FFFF 38800002 7CA32396"
TEST_ORDER+=(divwu_basic2)

# --- cntlzw edge: single bit ---
TESTS[cntlzw_bit15]="38600001 7C650034"
TEST_ORDER+=(cntlzw_bit15)

# --- neg with 0 ---
TESTS[neg_zero]="38600000 7CA300D0"
TEST_ORDER+=(neg_zero)

# --- extsh with 0 ---
TESTS[extsh_zero]="38600000 7C650734"
TEST_ORDER+=(extsh_zero)

# --- extsb with 0xFF ---
TESTS[extsb_ff]="386000FF 7C650774"
TEST_ORDER+=(extsb_ff)

# --- bdnz with count=0 (should NOT loop, fall through) ---
TESTS[bdnz_zero]="38600000 38800000 7C8903A6 38630001 4200FFFC"
TEST_ORDER+=(bdnz_zero)

# --- b (unconditional forward branch) ---
# b +8 skips one instruction
TESTS[b_forward]="48000008 38600001 38A00042"
TEST_ORDER+=(b_forward)

# --- record form: subf. ---
TESTS[subf_dot]="38600005 38800003 7CA42051"
TEST_ORDER+=(subf_dot)

# --- record form: xor. ---
TESTS[xor_dot]="386000FF 388000FF 7C651A79"
TEST_ORDER+=(xor_dot)

# --- record form: neg. ---
TESTS[neg_dot]="3860002A 7CA300D1"
TEST_ORDER+=(neg_dot)

# --- isync ---
TESTS[isync_only]="4C00012C"
TEST_ORDER+=(isync_only)

# --- sc (system call) ---

# --- twi (trap word immediate) - should NOP ---
TESTS[twi_basic]="0C000000"
TEST_ORDER+=(twi_basic)


# ============================================================
# ALTIVEC VECTORS — verify NEON-backed vector operations
# ============================================================

TESTS[vec_vadduwm]="10050718 10230718 10400880 38600600 7C4119CE 80A10600 80C10604"
TEST_ORDER+=(vec_vadduwm)

TESTS[vec_vsubuwm]="10050718 10230718 10400C80 38600600 7C4119CE 80A10600 80C10604"
TEST_ORDER+=(vec_vsubuwm)

TESTS[vec_vand]="100F0718 10250718 10400C04 38600600 7C4119CE 80A10600 80C10604"
TEST_ORDER+=(vec_vand)

TESTS[vec_vor]="100A0718 10250718 10400C84 38600600 7C4119CE 80A10600 80C10604"
TEST_ORDER+=(vec_vor)

TESTS[vec_vxor]="100F0718 102F0718 10400CC4 38600600 7C4119CE 80A10600 80C10604"
TEST_ORDER+=(vec_vxor)

TESTS[vec_vnor]="10000718 10200718 10400D04 38600600 7C4119CE 80A10600 80C10604"
TEST_ORDER+=(vec_vnor)

TESTS[vec_vmaxsw]="10050718 103D0718 10400982 38600600 7C4119CE 80A10600 80C10604"
TEST_ORDER+=(vec_vmaxsw)

TESTS[vec_vminsw]="10050718 103D0718 10400B82 38600600 7C4119CE 80A10600 80C10604"
TEST_ORDER+=(vec_vminsw)

TESTS[vec_vcmpequw]="10050718 10250718 10400886 38600600 7C4119CE 80A10600 80C10604"
TEST_ORDER+=(vec_vcmpequw)

TESTS[vec_vcmpequw_ne]="10050718 10230718 10400886 38600600 7C4119CE 80A10600 80C10604"
TEST_ORDER+=(vec_vcmpequw_ne)

TESTS[vec_vxor_self]="10070718 100004C4 38600600 7C0119CE 80A10600 80C10604"
TEST_ORDER+=(vec_vxor_self)

TESTS[vec_lvx_stvx]="100C0718 38600600 7C0119CE 10210CC4 7C2118CE 38600700 7C2119CE 80A10700 80C10704"
TEST_ORDER+=(vec_lvx_stvx)


# --- FP load/store coverage ---
# lfs/stfs round-trip: store 2.0 as single, load back
TESTS[fp_lfs_stfs]="3C604000 90610100 C0010100 D0010108 80A10108"
TEST_ORDER+=(fp_lfs_stfs)

# lfd/stfd round-trip
TESTS[fp_lfd_stfd]="3C604000 90610100 38600000 90610104 C8010100 D8010108 80A10108"
TEST_ORDER+=(fp_lfd_stfd)

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
