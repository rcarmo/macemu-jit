# SheepShaver AArch64 — PPC Opcode Equivalence Harness

## Goal

Verify PPC opcode correctness by comparing register state between the
interpreter and (eventually) the JIT after executing short PPC programs.

## How it works

1. Each test vector is a hex-encoded PPC instruction sequence (big-endian 32-bit words)
2. The harness injects the sequence at a known RAM address via `SS_TEST_HEX`
3. SheepShaver runs it in interpreter mode and dumps GPR0-31, CR, LR, CTR, XER
4. (Phase 2+) Also runs under JIT, diffs register dumps
5. Any mismatch = bug

## Metrics

- `METRIC pass=N` — vectors where interpreter output is deterministic
- `METRIC fail=N` — vectors with unexpected behavior
- `METRIC total=N` — total vectors executed
- `METRIC score=N` — percentage passing

## Phase 1 usage (interpreter-only)

In Phase 1, the harness validates that the optimized interpreter produces
identical results to the baseline interpreter. This catches regressions
from computed-goto dispatch, inlining, or memory-path changes.

## Vector format

Each vector provides:
- Initial GPR/CR/LR/CTR/XER state (via `SS_TEST_INIT`)
- PPC instruction sequence (via `SS_TEST_HEX`)
- Expected: deterministic register dump on repeated runs

## Current vectors

See `run.sh` for the active test list.
