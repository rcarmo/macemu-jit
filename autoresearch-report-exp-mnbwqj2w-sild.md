# Autoresearch Report: AArch64 JIT native codegen stability

**Generated:** 2026-03-29T16:07:36.288Z
**Project:** /home/agent/workspace/projects/macemu
**Primary metric:** score (unitless, higher is better)

## Summary

| Stat | Value |
|------|-------|
| Total runs | 7 |
| Kept | 4 |
| Discarded | 3 |
| Crashed | 0 |
| Checks failed | 0 |
| Best score | 100 |
| Confidence | 2.0× |

## Run History

| # | Status | score | Commit | Description |
|---|--------|-------|--------|-------------|
| 1 | discard | 80 | 7699f8c | Baseline with cpu_compatible=true: boots, runs 120s, no segf |
| 2 | keep | 90 | c33cb4a | Baseline: cpu_compatible=true, score=90 (boot+alive+no_segfa |
| 3 | discard | 20 | c33cb4a | cpu_compatible=false + prepare_block handler init: builds, 7 |
| 4 | discard | 20 | dcf2844 | cpu_compatible=false with crash recovery: compiles 39 blocks |
| 5 | keep | 100 | 8b93de7 | cpu_compatible=false + optlev capped at 1: native block disp |
| 6 | keep | 100 | f222924 | Confirmation run: score=100 again, 37252 JIT blocks, boots 1 |
| 7 | keep | 100 | d7c5750 | Confirmed optlev=1 stable (score=100, 36567 blocks). optlev= |

## Experiment Brief

# Autoresearch: AArch64 JIT native codegen stability

## Objective
Fix AArch64 JIT so it boots to Mac OS desktop and runs stably for 120 seconds
with native code generation enabled (`cpu_compatible=false`, optlev > 0).

Current state:
- `cpu_compatible=true` (optlev=0, interpreter wrappers): boots 5/5, 120s stable, score=90 (no native blocks)
- `cpu_compatible=false` (native codegen): crashes early

Goal: score=100 = boot + 120s uptime + 0 segfaults + native JIT blocks compiled.

## Metrics
- **Primary**: `score` (higher is better)
  - `40*(boot_ok) + 40*(alive_120s) + 10*(no_segfaults) + 10*(jit_blocks>0)`
  - 100 = fully working native JIT
- **Secondary**:
  - `build_ok` — compilation succeeded
  - `boot_ok` — Mac OS desktop reached
  - `uptime` — seconds alive
  - `alive_120s` — survived full 120s
  - `segfaults` — SIGSEGV count
  - `jit_blocks` — native blocks compiled (JIT_COMPILE log lines)

## How to Run
`./autoresearch.sh`

## Files in Scope
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_support_arm.cpp` — compile_block, prepare_block, get_handler, get_blockinfo_addr_new, invalidate_block, create_popalls, build_comp
- `BasiliskII/src/uae_cpu_2026/compiler/codegen_arm64.cpp` — endblock dispatch codegen (compemu_raw_endblock_pc_isconst, compemu_raw_endblock_pc_inreg, compemu_raw_jmp_pc_tag)
- `BasiliskII/src/uae_cpu_2026/compiler/compemu_midfunc_arm64.cpp` — native opcode implementations, write_jmp_target
- `BasiliskII/src/uae_cpu_2026/compemu_prefs.cpp` — cpu_compatible setting
- `BasiliskII/src/uae_cpu_2026/compiler/compemu.h` — blockinfo struct, cacheline definitions
- `autoresearch.sh` — benchmark harness

## Off Limits
- `/workspace/fixtures/basilisk/images/Quadra800.ROM`
- `/workspace/fixtures/basilisk/images/HD200MB`
- Build system / configure scripts

## Constraints
- Build with `--enable-aarch64-jit-experimental`
- Use incremental `make -j12`
- Run with `jit=true`
- Must boot to Mac OS desktop and survive 120s
- No unsafe memory practices

## What's Been Tried
- **Low-address mapping fixes** (KEPT): vm_alloc.cpp AArch64 path uses low MAP_BASE, next_address advances by full span. RAM+JIT both below 4GB.
- **JIT optflag fixes** (KEPT): optflag_addw/addb/subw/subb result stored correctly.
- **Combined popallspace+cache allocation** (KEPT): guarantees B/BL range for JIT dispatch.
- **HasMacStarted guard removal** (KEPT): allows boot to proceed.
- **DBcc workaround** (KEPT): forces optlev=0 for blocks containing DBcc opcodes.
- **cpu_compatible=true baseline**: score=90, boots and runs 120s but no native JIT blocks.