#!/usr/bin/env bun
/**
 * M68K JIT Fuzz Vector Generator
 *
 * Generates randomized but architecturally valid M68K instruction sequences
 * paired with random initial register states. Each vector exercises a specific
 * opcode class with varying operands, addressing modes, and flag conditions.
 *
 * Output: bash fragments to paste into run.sh
 */

// ---- PRNG (deterministic for reproducibility) ----
let seed = 0xDEAD_BEEF;
function rand32(): number {
  seed ^= seed << 13;
  seed ^= seed >>> 17;
  seed ^= seed << 5;
  return seed >>> 0;
}
function randRange(lo: number, hi: number): number {
  return lo + (rand32() % (hi - lo + 1));
}
function randBool(): boolean { return (rand32() & 1) === 1; }
function hex16(v: number): string { return (v & 0xFFFF).toString(16).toUpperCase().padStart(4, '0'); }
function hex32(v: number): string { return (v >>> 0).toString(16).toUpperCase().padStart(8, '0'); }

// ---- Random register state generator ----
interface FuzzVector {
  name: string;
  hex: string[];        // M68K instruction words (16-bit hex)
  initRegs?: string[];  // D0-D7 A0-A7 [SR] as 8-hex-digit strings
  comment: string;
}

/** Generate an "interesting" 32-bit value for register init */
function interestingU32(): number {
  const r = rand32() % 20;
  if (r < 3) return 0;
  if (r < 5) return 0xFFFFFFFF;
  if (r < 7) return 0x80000000;
  if (r < 9) return 0x7FFFFFFF;
  if (r < 11) return 0x000000FF;
  if (r < 13) return 0x0000FFFF;
  if (r < 15) return randRange(1, 255);
  return rand32();
}

/** Random stack-aligned address in safe RAM area (must be ≥4-aligned, in 0x1000..0x7F0000) */
function safeAddr(): number {
  return (randRange(0x400, 0x1FC000) << 2);  // 0x1000..0x7F0000, 4-aligned
}

/** Random supervisor SR with various interrupt masks and flag combos */
function randomSR(): number {
  const flags = rand32() & 0x1F;  // XNZVC
  const intmask = randRange(0, 7);
  return 0x2000 | (intmask << 8) | flags;  // supervisor, S=1
}

function makeInitRegs(overrides: Record<string, number> = {}): string[] {
  const regs: number[] = [];
  // D0-D7
  for (let i = 0; i < 8; i++) {
    regs.push(overrides[`d${i}`] ?? interestingU32());
  }
  // A0-A5: safe addresses
  for (let i = 0; i < 6; i++) {
    regs.push(overrides[`a${i}`] ?? safeAddr());
  }
  // A6: will be overwritten by sentinel
  regs.push(0);
  // A7: stack pointer — must be valid
  regs.push(overrides.a7 ?? (0x7F0000 - 256));
  return regs.map(v => hex32(v));
}

// ---- Opcode encoders ----
// MOVEQ #imm8, Dn: 0111_nnn0_iiiiiiii
function encMoveq(dn: number, imm8: number): string {
  return hex16(0x7000 | ((dn & 7) << 9) | (imm8 & 0xFF));
}

// MOVE.L #imm32, Dn: 2x3C + two words
function encMoveLImm(dn: number, imm32: number): string[] {
  const op = 0x203C | ((dn & 7) << 9);
  return [hex16(op), hex16(imm32 >>> 16), hex16(imm32 & 0xFFFF)];
}

// ADD.L Dm,Dn: D0n0 + src_reg
function encAddLReg(dn: number, dm: number): string {
  return hex16(0xD080 | ((dn & 7) << 9) | (dm & 7));
}
// SUB.L Dm,Dn
function encSubLReg(dn: number, dm: number): string {
  return hex16(0x9080 | ((dn & 7) << 9) | (dm & 7));
}
// AND.L Dm,Dn
function encAndLReg(dn: number, dm: number): string {
  return hex16(0xC080 | ((dn & 7) << 9) | (dm & 7));
}
// OR.L Dm,Dn
function encOrLReg(dn: number, dm: number): string {
  return hex16(0x8080 | ((dn & 7) << 9) | (dm & 7));
}
// EOR.L Dm,Dn (EOR Dm → Dn)
function encEorLReg(dn: number, dm: number): string {
  return hex16(0xB180 | ((dm & 7) << 9) | (dn & 7));
}
// LSL.L #cnt, Dn
function encLslLImm(dn: number, cnt: number): string {
  return hex16(0xE188 | ((cnt & 7) << 9) | (dn & 7));
}
// LSR.L #cnt, Dn
function encLsrLImm(dn: number, cnt: number): string {
  return hex16(0xE088 | ((cnt & 7) << 9) | (dn & 7));
}
// ASR.L #cnt, Dn
function encAsrLImm(dn: number, cnt: number): string {
  return hex16(0xE080 | ((cnt & 7) << 9) | (dn & 7));
}
// ASL.L #cnt, Dn
function encAslLImm(dn: number, cnt: number): string {
  return hex16(0xE180 | ((cnt & 7) << 9) | (dn & 7));
}
// ROL.L #cnt, Dn
function encRolLImm(dn: number, cnt: number): string {
  return hex16(0xE198 | ((cnt & 7) << 9) | (dn & 7));
}
// ROR.L #cnt, Dn
function encRorLImm(dn: number, cnt: number): string {
  return hex16(0xE098 | ((cnt & 7) << 9) | (dn & 7));
}
// SWAP Dn
function encSwap(dn: number): string {
  return hex16(0x4840 | (dn & 7));
}
// EXT.W Dn
function encExtW(dn: number): string {
  return hex16(0x4880 | (dn & 7));
}
// EXT.L Dn
function encExtL(dn: number): string {
  return hex16(0x48C0 | (dn & 7));
}
// NEG.L Dn
function encNegL(dn: number): string {
  return hex16(0x4480 | (dn & 7));
}
// NOT.L Dn
function encNotL(dn: number): string {
  return hex16(0x4680 | (dn & 7));
}
// TST.L Dn
function encTstL(dn: number): string {
  return hex16(0x4A80 | (dn & 7));
}
// CLR.L Dn
function encClrL(dn: number): string {
  return hex16(0x4280 | (dn & 7));
}
// CMP.L Dm,Dn
function encCmpLReg(dn: number, dm: number): string {
  return hex16(0xB080 | ((dn & 7) << 9) | (dm & 7));
}
// MULU.W Dm,Dn
function encMuluW(dn: number, dm: number): string {
  return hex16(0xC0C0 | ((dn & 7) << 9) | (dm & 7));
}
// MULS.W Dm,Dn
function encMulsW(dn: number, dm: number): string {
  return hex16(0xC1C0 | ((dn & 7) << 9) | (dm & 7));
}
// DIVU.W Dm,Dn
function encDivuW(dn: number, dm: number): string {
  return hex16(0x80C0 | ((dn & 7) << 9) | (dm & 7));
}
// DIVS.W Dm,Dn
function encDivsW(dn: number, dm: number): string {
  return hex16(0x81C0 | ((dn & 7) << 9) | (dm & 7));
}
// EXG Dm,Dn
function encExgDD(dm: number, dn: number): string {
  return hex16(0xC140 | ((dm & 7) << 9) | (dn & 7));
}
// ADDX.L Dm,Dn
function encAddxL(dn: number, dm: number): string {
  return hex16(0xD180 | ((dn & 7) << 9) | (dm & 7));
}
// SUBX.L Dm,Dn
function encSubxL(dn: number, dm: number): string {
  return hex16(0x9180 | ((dn & 7) << 9) | (dm & 7));
}
// NEGX.L Dn
function encNegxL(dn: number): string {
  return hex16(0x4080 | (dn & 7));
}
// BTST #imm, Dn (bit test immediate on register)
function encBtstImm(dn: number, bit: number): string[] {
  return [hex16(0x0800 | (dn & 7)), hex16(bit & 31)];
}
// BSET #imm, Dn
function encBsetImm(dn: number, bit: number): string[] {
  return [hex16(0x08C0 | (dn & 7)), hex16(bit & 31)];
}
// BCLR #imm, Dn
function encBclrImm(dn: number, bit: number): string[] {
  return [hex16(0x0880 | (dn & 7)), hex16(bit & 31)];
}
// BCHG #imm, Dn
function encBchgImm(dn: number, bit: number): string[] {
  return [hex16(0x0840 | (dn & 7)), hex16(bit & 31)];
}
// ORI #imm8, CCR
function encOriCCR(imm8: number): string[] {
  return ['003C', hex16(imm8 & 0xFF)];
}
// ANDI #imm8, CCR
function encAndiCCR(imm8: number): string[] {
  return ['023C', hex16(imm8 & 0xFF)];
}
// MOVE.L Dn,(d16,An)
function encMoveLDnAn16(dn: number, an: number, disp: number): string[] {
  return [hex16(0x2140 | ((an & 7) << 9) | (dn & 7)), hex16(disp & 0xFFFF)];
}
// MOVE.L (d16,An),Dn
function encMoveLAn16Dn(dn: number, an: number, disp: number): string[] {
  return [hex16(0x2028 | ((dn & 7) << 9) | (an & 7)), hex16(disp & 0xFFFF)];
}

// ---- Fuzz vector generators ----

function fuzzALU(idx: number): FuzzVector {
  const ops = [encAddLReg, encSubLReg, encAndLReg, encOrLReg, encEorLReg];
  const opNames = ['add', 'sub', 'and', 'or', 'eor'];
  const hex: string[] = [];
  const dn = randRange(0, 5);  // avoid D6 (sentinel) and D7 (scratch)
  const dm = randRange(0, 5);
  // Chain 2-4 random ALU ops on the same register
  const nops = randRange(2, 4);
  const parts: string[] = [];
  for (let i = 0; i < nops; i++) {
    const oi = rand32() % ops.length;
    const src = randRange(0, 5);
    hex.push(ops[oi](dn, src));
    parts.push(`${opNames[oi]}.l d${src},d${dn}`);
  }
  return {
    name: `fuzz_alu_${idx}`,
    hex,
    initRegs: makeInitRegs(),
    comment: `ALU chain: ${parts.join('; ')}`,
  };
}

function fuzzShift(idx: number): FuzzVector {
  const ops = [encLslLImm, encLsrLImm, encAsrLImm, encAslLImm, encRolLImm, encRorLImm];
  const opNames = ['lsl', 'lsr', 'asr', 'asl', 'rol', 'ror'];
  const hex: string[] = [];
  const dn = randRange(0, 5);
  const nops = randRange(2, 3);
  const parts: string[] = [];
  for (let i = 0; i < nops; i++) {
    const oi = rand32() % ops.length;
    const cnt = randRange(1, 8);
    hex.push(ops[oi](dn, cnt));
    parts.push(`${opNames[oi]}.l #${cnt},d${dn}`);
  }
  return {
    name: `fuzz_shift_${idx}`,
    hex,
    initRegs: makeInitRegs(),
    comment: `Shift chain: ${parts.join('; ')}`,
  };
}

function fuzzBitOps(idx: number): FuzzVector {
  const hex: string[] = [];
  const dn = randRange(0, 5);
  const nops = randRange(2, 4);
  const parts: string[] = [];
  const ops = [encBtstImm, encBsetImm, encBclrImm, encBchgImm];
  const opNames = ['btst', 'bset', 'bclr', 'bchg'];
  for (let i = 0; i < nops; i++) {
    const oi = rand32() % ops.length;
    const bit = randRange(0, 31);
    hex.push(...ops[oi](dn, bit));
    parts.push(`${opNames[oi]} #${bit},d${dn}`);
  }
  return {
    name: `fuzz_bitops_${idx}`,
    hex,
    initRegs: makeInitRegs(),
    comment: `Bit ops: ${parts.join('; ')}`,
  };
}

function fuzzMulDiv(idx: number): FuzzVector {
  const hex: string[] = [];
  const dn = randRange(0, 3);
  const dm = randRange(4, 5);  // Use different reg for divisor
  const parts: string[] = [];
  // Ensure non-zero divisor via init
  const overrides: Record<string, number> = {};
  overrides[`d${dm}`] = randRange(1, 0xFFFF);  // non-zero word divisor

  if (randBool()) {
    hex.push(encMuluW(dn, dm));
    parts.push(`mulu.w d${dm},d${dn}`);
  } else {
    hex.push(encMulsW(dn, dm));
    parts.push(`muls.w d${dm},d${dn}`);
  }
  // Follow with a divide to test result
  if (randBool()) {
    hex.push(encDivuW(dn, dm));
    parts.push(`divu.w d${dm},d${dn}`);
  } else {
    hex.push(encDivsW(dn, dm));
    parts.push(`divs.w d${dm},d${dn}`);
  }
  return {
    name: `fuzz_muldiv_${idx}`,
    hex,
    initRegs: makeInitRegs(overrides),
    comment: `Mul/Div: ${parts.join('; ')}`,
  };
}

function fuzzExtSwap(idx: number): FuzzVector {
  const hex: string[] = [];
  const dn = randRange(0, 5);
  const nops = randRange(2, 4);
  const parts: string[] = [];
  const ops = [
    () => { hex.push(encSwap(dn)); return `swap d${dn}`; },
    () => { hex.push(encExtW(dn)); return `ext.w d${dn}`; },
    () => { hex.push(encExtL(dn)); return `ext.l d${dn}`; },
    () => { hex.push(encNegL(dn)); return `neg.l d${dn}`; },
    () => { hex.push(encNotL(dn)); return `not.l d${dn}`; },
    () => { hex.push(encTstL(dn)); return `tst.l d${dn}`; },
  ];
  for (let i = 0; i < nops; i++) {
    const oi = rand32() % ops.length;
    parts.push(ops[oi]());
  }
  return {
    name: `fuzz_extswap_${idx}`,
    hex,
    initRegs: makeInitRegs(),
    comment: `Ext/Swap: ${parts.join('; ')}`,
  };
}

function fuzzAddxSubx(idx: number): FuzzVector {
  const hex: string[] = [];
  const parts: string[] = [];
  // Set X flag randomly
  if (randBool()) {
    hex.push(...encOriCCR(0x10)); // set X
    parts.push('ori #$10,ccr');
  } else {
    hex.push(...encAndiCCR(0xEF)); // clear X
    parts.push('andi #$EF,ccr');
  }
  const dn = randRange(0, 3);
  const dm = randRange(4, 5);
  if (randBool()) {
    hex.push(encAddxL(dn, dm));
    parts.push(`addx.l d${dm},d${dn}`);
  } else {
    hex.push(encSubxL(dn, dm));
    parts.push(`subx.l d${dm},d${dn}`);
  }
  // Maybe chain another
  if (randBool()) {
    hex.push(encNegxL(dn));
    parts.push(`negx.l d${dn}`);
  }
  return {
    name: `fuzz_addxsubx_${idx}`,
    hex,
    initRegs: makeInitRegs(),
    comment: `Addx/Subx: ${parts.join('; ')}`,
  };
}

function fuzzMemRoundtrip(idx: number): FuzzVector {
  const hex: string[] = [];
  const parts: string[] = [];
  const dn = randRange(0, 3);
  const an = randRange(0, 2);  // Use A0-A2 for addressing
  const disp = randRange(0, 252) & ~3;  // 4-aligned displacement
  // Store Dn to memory, modify Dn, load back
  hex.push(...encMoveLDnAn16(dn, an, disp));
  parts.push(`move.l d${dn},(${disp},a${an})`);
  // Modify Dn
  hex.push(encNotL(dn));
  parts.push(`not.l d${dn}`);
  // Load back into another register
  const dn2 = (dn + 1) % 4;
  hex.push(...encMoveLAn16Dn(dn2, an, disp));
  parts.push(`move.l (${disp},a${an}),d${dn2}`);
  // Compare
  hex.push(encCmpLReg(dn2, dn));
  parts.push(`cmp.l d${dn},d${dn2}`);
  return {
    name: `fuzz_memrt_${idx}`,
    hex,
    initRegs: makeInitRegs(),
    comment: `Mem roundtrip: ${parts.join('; ')}`,
  };
}

function fuzzExgChain(idx: number): FuzzVector {
  const hex: string[] = [];
  const parts: string[] = [];
  const nops = randRange(2, 4);
  for (let i = 0; i < nops; i++) {
    const dm = randRange(0, 5);
    const dn = randRange(0, 5);
    if (dm !== dn) {
      hex.push(encExgDD(dm, dn));
      parts.push(`exg d${dm},d${dn}`);
    }
  }
  // Verify with a TST
  const dt = randRange(0, 5);
  hex.push(encTstL(dt));
  parts.push(`tst.l d${dt}`);
  return {
    name: `fuzz_exg_${idx}`,
    hex,
    initRegs: makeInitRegs(),
    comment: `Exg chain: ${parts.join('; ')}`,
  };
}

function fuzzMixedALUShift(idx: number): FuzzVector {
  const hex: string[] = [];
  const parts: string[] = [];
  const dn = randRange(0, 3);
  const nops = randRange(3, 6);
  for (let i = 0; i < nops; i++) {
    const choice = rand32() % 8;
    const dm = randRange(0, 5);
    const cnt = randRange(1, 8);
    switch (choice) {
      case 0: hex.push(encAddLReg(dn, dm)); parts.push(`add.l d${dm},d${dn}`); break;
      case 1: hex.push(encSubLReg(dn, dm)); parts.push(`sub.l d${dm},d${dn}`); break;
      case 2: hex.push(encLslLImm(dn, cnt)); parts.push(`lsl.l #${cnt},d${dn}`); break;
      case 3: hex.push(encLsrLImm(dn, cnt)); parts.push(`lsr.l #${cnt},d${dn}`); break;
      case 4: hex.push(encAndLReg(dn, dm)); parts.push(`and.l d${dm},d${dn}`); break;
      case 5: hex.push(encOrLReg(dn, dm)); parts.push(`or.l d${dm},d${dn}`); break;
      case 6: hex.push(encSwap(dn)); parts.push(`swap d${dn}`); break;
      case 7: hex.push(encNegL(dn)); parts.push(`neg.l d${dn}`); break;
    }
  }
  return {
    name: `fuzz_mixed_${idx}`,
    hex,
    initRegs: makeInitRegs(),
    comment: `Mixed ALU+Shift: ${parts.join('; ')}`,
  };
}

function fuzzFlagStress(idx: number): FuzzVector {
  const hex: string[] = [];
  const parts: string[] = [];
  // Set specific CCR pattern
  const ccr = rand32() & 0x1F;
  hex.push(...encOriCCR(ccr));
  parts.push(`ori #$${ccr.toString(16)},ccr`);
  // Do operations that depend on flags
  const dn = randRange(0, 3);
  const dm = randRange(4, 5);
  if (randBool()) {
    hex.push(encAddxL(dn, dm));
    parts.push(`addx.l d${dm},d${dn}`);
  }
  // TST to check flag state
  hex.push(encTstL(dn));
  parts.push(`tst.l d${dn}`);
  // Another flag-dependent op
  if (randBool()) {
    hex.push(encSubxL(dm, dn));
    parts.push(`subx.l d${dn},d${dm}`);
  }
  return {
    name: `fuzz_flags_${idx}`,
    hex,
    initRegs: makeInitRegs(),
    comment: `Flag stress: ${parts.join('; ')}`,
  };
}

// ---- Generate all vectors ----
const vectors: FuzzVector[] = [];
const N_PER_CLASS = 5;

for (let i = 0; i < N_PER_CLASS; i++) {
  vectors.push(fuzzALU(i));
  vectors.push(fuzzShift(i));
  vectors.push(fuzzBitOps(i));
  vectors.push(fuzzMulDiv(i));
  vectors.push(fuzzExtSwap(i));
  vectors.push(fuzzAddxSubx(i));
  vectors.push(fuzzMemRoundtrip(i));
  vectors.push(fuzzExgChain(i));
  vectors.push(fuzzMixedALUShift(i));
  vectors.push(fuzzFlagStress(i));
}

// ---- Emit bash ----
let sentinelIdx = 0xF000;
const testOrderNames: string[] = [];
const testDefs: string[] = [];
const sentinelDefs: string[] = [];
const initDefs: string[] = [];
const riskyDefs: string[] = [];

for (const v of vectors) {
  const sval = `a6${(sentinelIdx++).toString(16).padStart(4, '0')}00`;
  testOrderNames.push(v.name);
  testDefs.push(`# ${v.comment}`);
  testDefs.push(`TESTS[${v.name}]="${v.hex.join(' ')}"`);
  sentinelDefs.push(`SENTINEL_A6[${v.name}]="${sval}"`);
  riskyDefs.push(`    [${v.name}]=1`);
  if (v.initRegs) {
    initDefs.push(`INIT_REGS[${v.name}]="${v.initRegs.join(' ')}"`);
  }
}

console.log('# ---- FUZZ VECTORS (auto-generated by gen-fuzz.ts) ----');
console.log(`# ${vectors.length} vectors, ${N_PER_CLASS} per class, seed=0xDEADBEEF`);
console.log();
console.log('# Add to TEST_ORDER (append these names):');
console.log(`# ${testOrderNames.join(' ')}`);
console.log();
console.log('# Test definitions:');
for (const line of testDefs) console.log(line);
console.log();
console.log('# Sentinel values:');
for (const line of sentinelDefs) console.log(line);
console.log();
console.log('# Initial register state:');
for (const line of initDefs) console.log(line);
console.log();
console.log('# Risky tags:');
console.log('# Add to RISKY_TESTS:');
for (const line of riskyDefs) console.log(line);
