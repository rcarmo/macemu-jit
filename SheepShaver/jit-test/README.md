# SheepShaver PPC Opcode Equivalence Harness

## Status

**28 deterministic test vectors**, all passing against the PPC interpreter.

## How it works

1. Set `SS_TEST_HEX` to a space-separated hex sequence of PPC instructions (big-endian 32-bit words)
2. Set `SS_TEST_DUMP=1` to emit a `REGDUMP:` line with GPR0-31, CR, LR, CTR, XER
3. Optionally set `SS_TEST_INIT` to seed GPR0-31 + optional CR before execution
4. Optionally set `SS_TEST_JIT=1` to compile and execute via the AArch64 JIT

The harness runs each vector twice and diffs the output for determinism.

## Running

```bash
# Full harness
./jit-test/run.sh

# Single vector (interpreter)
SS_TEST_HEX="38600064 388000c8 7CA32214" SS_TEST_DUMP=1 src/Unix/SheepShaver

# Single vector (JIT)
SS_TEST_HEX="38600064 388000c8 7CA32214" SS_TEST_DUMP=1 SS_TEST_JIT=1 src/Unix/SheepShaver
```

## Metrics

- `METRIC pass=N` — vectors where both runs match
- `METRIC fail=N` — vectors with mismatch or error
- `METRIC total=N` — total vectors
- `METRIC score=N` — pass percentage

## Current vectors

| Vector | Opcodes tested |
|--------|---------------|
| `alu_add` | `li`, `add` |
| `alu_sub` | `li`, `subf` |
| `alu_and` | `li`, `and` |
| `alu_or` | `li`, `or` |
| `alu_xor` | `li`, `xor` |
| `li_wide` | `lis`, `ori` (32-bit immediate) |
| `shift_slw` | `li`, `slw` |
| `shift_srw` | `li`, `srw` |
| `cmp_beq` | `cmpw`, `beq`, conditional branch skip |
| `bdnz_loop` | `mtctr`, `addi`, `bdnz` (5 iterations) |
| `mul_basic` | `li`, `mullw` |
| `rlwinm_basic` | `rlwinm` rotate + mask |
| `nop` | NOP sanity |
| `neg_basic` | `neg` |
| `sraw_signext` | `sraw` with sign extension + XER CA |
| `stw_lwz` | `stw`/`lwz` memory round-trip |
| `stb_lbz` | `stb`/`lbz` byte round-trip |
| `sth_lhz` | `sth`/`lhz` halfword round-trip |
| `addic_dot` | `addic.` with CR0 update |
| `add_dot_neg` | `add.` with negative result → CR0.LT |
| `divw_basic` | `divw` integer divide |
| `mtctr_mfctr` | `mtctr`/`mfctr` SPR round-trip |
| `addic_carry` | `addic` carry flag (XER.CA) |
| `adde_carry` | `adde` extended add with carry |
| `rlwimi_insert` | `rlwimi` rotate and mask insert |
| `cntlzw_basic` | `cntlzw` count leading zeros |
| `extsh_basic` | `extsh` sign-extend halfword |
| `extsb_basic` | `extsb` sign-extend byte |
