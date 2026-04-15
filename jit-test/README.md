# BasiliskII AArch64 JIT — Opcode Equivalence Harness

## Goal

Keep `jit-test/run.sh` trustworthy and deterministic so every run emits numeric metrics:
- `score`
- `pass`
- `fail`
- `total`

Harness-first scope: prioritize benchmark correctness/completeness over emulator product behavior.

## Metric contract

`jit-test/run.sh` always emits:
- `METRIC pass=<int>` — number of opcode vectors where JIT and interpreter REGDUMP match
- `METRIC fail=<int>` — vectors with mismatch or harness infra error
- `METRIC total=<int>` — number of vectors executed
- `METRIC score=<int>` — `floor(pass * 100 / total)` (0 when `total=0`)
- `METRIC infra_fail=<int>` — subset of failures caused by harness/runtime issues
- `METRIC build_ok=<0|1>` — whether build/setup succeeded before test execution
- Infra/equivalence breakdown counters for triage (`fail_equiv`, `infra_timeout`, `infra_emu_exit`, `infra_no_regdump`, `infra_multi_regdump`, `infra_sentinel`, `infra_other`)

## Harness model

For each test vector:
1. Build BasiliskII (configures if needed)
2. Run bytecode in interpreter mode (`jit false`) and JIT mode (`jit true`)
3. Require exactly one `REGDUMP:` line in each run
4. Verify sentinel write to A6 occurred
5. Diff full REGDUMP lines to decide pass/fail

## Current deterministic vectors

`run.sh` currently covers 122 vectors across:
- Decode/dispatch sanity (`nop`)
- Bit manipulation boundary behavior (`bitops`, `bitops_chg`, high-bit immediate `bitops_highbit`, high-bit toggle `bitops_chg_highbit`)
- Core arithmetic/data movement (`move` + `moveq_signext`, `alu`, `addi/subi` incl. byte/word/long + byte/word-boundary-wrap checks, `quick_ops` incl. word+word-wrap+long-wrap+byte+byte-wrap+address-register variants, `compare` + `cmpi` size + negative byte/word/long boundary forms, `muldiv`, `movem`, `misc`, `flags` incl. OR/AND/EOR-CCR path, `exg`, `imm_logic` incl. byte+word+long variants, `tst` size forms on negative and zero inputs)
- Branch condition behavior (`bra` short+word, `bne/beq` short+word, both short + `.W` displacement forms for `bpl/bmi`, `bvc/bvs`, `bge/blt`, `bgt/ble`, `bcc/bcs`, `bhi/bls`, plus chained-condition branch sequencing)
- Condition-byte writes via `Scc` families (`st/sf`, `shi/sls`, `scc/scs`, `sne/seq`, `svc/svs`, `spl/smi`, `sge/slt`, `sgt/sle`)
- Loop control (`dbra` taken, terminal non-taken, bounded multi-iteration loop, plus bounded `dbne`/`dbeq` and `dbvc`/`dbvs` loops, and deterministic `dbvc`/`dbvs` non-taken condition-true paths)

All vectors are designed to terminate without unbounded loops.

## Harness integrity checks

Before executing vectors, `run.sh` performs deterministic preflight validation:
- no duplicate test names in `TEST_ORDER`
- every ordered test has both `TESTS[...]` bytecode and `SENTINEL_A6[...]`
- each sentinel is an 8-hex-digit value
- sentinel values are unique across vectors

Any invariant violation aborts with machine-parseable failure metrics (`infra_fail=1`) instead of silently running a malformed suite.

## Constraints

- No ROM patches, stub-region hacks, or RAM presets to mask bugs.
- Keep outputs machine-parseable and numeric.
- Keep vectors deterministic and bounded-time.
