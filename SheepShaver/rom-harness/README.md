# ROM Harness — Headless JIT Exerciser

A standalone tool that loads a Mac ROM file and exercises the SheepShaver AArch64 JIT
by compiling and executing real ROM code blocks, comparing JIT output against a built-in
reference interpreter.

## What It Does

1. **Loads the ROM** — reads the raw 4MB ROM file into memory
2. **Scans for blocks** — finds PPC basic blocks (instruction sequences ending at branches)
3. **Filters** — skips blocks with memory access, privileged ops, or EMUL_OP trampolines
4. **Tests each block** — seeds registers with deterministic pseudo-random values, runs both
   interpreter and JIT, compares all 32 GPRs + CR + XER + LR + CTR + PC
5. **Reports** — shows pass/fail counts, JIT coverage stats, and detailed mismatch info

## Building

```bash
make        # builds rom-harness
make clean  # removes build artifacts
```

## Usage

```bash
# Full scan of all testable blocks
./rom-harness <rom-file>

# Quick smoke test (first 500 blocks, stop on first failure)
./rom-harness <rom-file> --count=500 --stop-on-fail --verbose

# Test a single block at a specific ROM offset
./rom-harness <rom-file> --entry=0x1910 --verbose

# Multi-pass with different random seeds
./rom-harness <rom-file> --passes=10 --seed=42

# Control block size
./rom-harness <rom-file> --min-insns=2 --max-insns=16
```

## Options

| Option | Description | Default |
|--------|------------|---------|
| `--offset=0xN` | Start scanning at ROM offset | 0 |
| `--count=N` | Max blocks to test (0=all) | 0 |
| `--verbose` | Print each block result | off |
| `--stop-on-fail` | Stop at first mismatch | off |
| `--min-insns=N` | Minimum block size | 1 |
| `--max-insns=N` | Maximum block size | 64 |
| `--entry=0xN` | Test single block at offset | — |
| `--seed=N` | Random seed for register init | 0xDEADBEEF |
| `--passes=N` | Number of random passes | 1 |
| `--compute-only` | Skip memory-access blocks | on |
| `--all-blocks` | Include memory-access blocks | off |

## Architecture

- **No SheepShaver dependencies** — compiles standalone against only the JIT source
- **Built-in reference interpreter** — subset PPC interpreter covering all ops the JIT handles
- **Register layout** matches the JIT's expected struct offsets exactly (verified with static_assert)
- **SIGSEGV protection** — JIT crashes are caught and reported, never abort the harness

## Interpreting Results

```
Score: 663/766
```

- **Score** = passed / (passed + failed)
- **Skipped** blocks are excluded from the score (incomplete JIT, unsupported interp ops)
- **JIT compile fail** = JIT couldn't compile the block (incomplete, unknown opcode)
- **Interp unsupported** = harness interpreter doesn't handle an opcode in the block

## Bugs Found By This Harness

1. **CR logical NOP-default** — `mcrf`, `crand`, `cror`, etc. silently treated as NOPs
2. **Missing XER[SO] in comparisons** — `cmp`, `cmpi`, `cmpli`, `cmpl` didn't set CR[SO]
3. **Wrong NZCV→CR mapping in cmpi** — raw ARM64 NZCV used instead of proper signed LT/GT/EQ
4. **bdz not implemented** — only bdnz was handled, bdz (BO=0b01111) fell into wrong path
5. **bc epilogue skip-over bug** — CBZ/CBNZ +8 didn't skip the full multi-instruction epilogue
