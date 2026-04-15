# BasiliskII AArch64 JIT — Opcode Correctness Autoresearch

## Goal

Achieve maximum M68K opcode correctness in the AArch64 JIT by:
1. Building a test harness that runs M68K programs in both interpreter and JIT mode
2. Comparing register/flag state output between the two
3. Identifying and fixing opcodes where JIT output diverges from interpreter

## Metric

`score` = number of opcode test cases where JIT output exactly matches interpreter output.

Target: all test cases pass (score == total_tests).

## Key Constraint

**Do NOT add ROM patches, stub regions, or MAC RAM pre-sets to work around JIT bugs.**
Fix the JIT opcode handler directly. Every fix must be verifiable by the test harness.

## Repository state

- Source: `/workspace/projects/macemu/BasiliskII/`
- Clean baseline: commit `4946daac`
- Build dir: `/workspace/projects/macemu/BasiliskII/src/Unix`
- ROM: `/workspace/projects/rpi-basilisk2-sdl2-nox/Quadra800.ROM`
- Disk: `/workspace/fixtures/basilisk/images/HD200MB`

## Test harness design

See `autoresearch.sh` for implementation. Each iteration:
1. Writes M68K test bytecode into the test harness
2. Runs under interpreter (jit false) → captures register dump
3. Runs under JIT (jit true) → captures register dump
4. Diffs the dumps — any difference is a JIT bug
5. Reports METRIC lines for scoring

## Opcode test cases

One test per opcode class. Each sets known register state, executes the opcode(s),
then the harness dumps D0-D7, A0-A6, SR.

| ID | Opcodes | Key flags to check |
|----|---------|-------------------|
| move | MOVE.L/W/B, MOVEA, MOVEQ, MOVEM | N, Z |
| alu | ADD, SUB, AND, OR, EOR, NOT, NEG | N, Z, C, V, X |
| shift | LSL, LSR, ASL, ASR, ROL, ROR, ROXL, ROXR | N, Z, C, X |
| bitops | BTST, BSET, BCLR, BCHG | Z only |
| branch | Bcc (all conditions), DBcc | condition codes |
| compare | CMP, CMPA, CMPI, TST | N, Z, C, V |
| muldiv | MULU, MULS, DIVU, DIVS | N, Z, V |
| movem | MOVEM predecrement, postincrement | register values |
| misc | EXG, SWAP, EXT, CLR, ABCD, SBCD | N, Z, C, X |
| flags | ANDI/EORI/ORI to SR, MOVE to/from SR | SR value |
