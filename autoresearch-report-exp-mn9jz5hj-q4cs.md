# Autoresearch Report: Experiment

**Generated:** 2026-03-28T00:18:47.054Z
**Project:** /workspace/projects/macemu
**Primary metric:** metric (unitless, lower is better)

## Summary

| Stat | Value |
|------|-------|
| Total runs | 0 |
| Kept | 0 |
| Discarded | 0 |
| Crashed | 0 |
| Checks failed | 0 |
| Best metric | — |
| Confidence | — |

## Run History

| # | Status | metric | Commit | Description |
|---|--------|--------|--------|-------------|

## Experiment Brief

# Autoresearch: AArch64 JIT boot progression (45s stability workload)

## Objective
Increase boot progression under `jit=true` while keeping the AArch64 JIT runtime stable.

Current practical target: improve `stage_coverage_score` (now heartbeat-aware) without regressing low-address placement or 45s liveness.

## Metrics
- **Primary**: `stage_coverage_score` (higher is better)
  - components:
    - legacy BOOT_STAGE probes (when available)
    - `BOOT InitAll: complete`
    - `Starting emulation...`
    - JIT table build + cache-ready
    - full-window liveness (`boot_alive`)
    - heartbeat bucket from `cpu_heartbeat_count`
- **Secondary**:
  - `jit_alive_sec`
  - `lowaddr_score`
  - `build_ok`, `boot_alive`
  - `mac_ram_low32`, `jit_code_low32`
  - `cpu_heartbeat_count`, `heartbeat_score`
  - `jit_compiled_opcodes`
  - `reserved_assert`, `popall_alloc_fail`, `block_pool_fail`, `jit_high_addr_warn`

## How to Run
`./autoresearch.sh`

Notes:
- Default run window: 45 seconds.
- Optional diagnostic toggles via files:
  - `autoresearch.gdb` enables GDB run mode.
  - `autoresearch.jitinline` sets `jitinline=true` in prefs.

## Files in Scope
- `BasiliskII/src/uae_cpu_2021/compiler/compemu_support.cpp`
- `BasiliskII/src/uae_cpu_2021/compiler/compemu_support_arm.cpp`
- `BasiliskII/src/uae_cpu_2021/compiler/codegen_arm64.cpp`
- `BasiliskII/src/uae_cpu_2021/compiler/compemu_arm.h`
- `BasiliskII/src/uae_cpu_2021/compiler/compemu_legacy_arm64_compat.cpp`
- `autoresearch.sh`
- `autoresearch.md`
- `autoresearch.ideas.md`

## Constraints
- Keep incremental make flow.
- Keep `jit=true`.
- Build with `--enable-aarch64-jit-experimental`.
- Do not fabricate metrics.

## What’s Been Tried

### Kept
- Runtime-ready guard in AArch64 path (`ensure_aarch64_jit_runtime_ready`): call `compiler_init()`, ensure `table68k` is initialized.
- Even-slot cache tag indexing hardening:
  - `cacheline()` clears bit0
  - AArch64 generated dispatcher/endblock lookups force even slot.
- Harness improvements:
  - early Return/click events sent during run loop (not after timeout)
  - heartbeat parsing added to metrics.
- AArch64 code allocator adjustment:
  - `alloc_code` now attempts `VM_MAP_32BIT` first, then falls back to default mapping.
  - this stabilized low-address RAM/JIT placement in recent confirmations.

### Discarded / no primary gain
- Multiple GDB-only diagnostic formatting/stop-behavior changes.
- `jitinline=true` toggle (no measurable progression gain).
- Higher-frequency heartbeat logging variants that did not improve progression and occasionally correlated with noisy lowaddr runs.
- Additional post-call heartbeat trace around first opcode handler (did not add actionable signal).

## Current Baseline (latest kept)
- `stage_coverage_score=90`
- `jit_alive_sec=45`
- `lowaddr_score=100`
- `cpu_heartbeat_count=1`
- `jit_compiled_opcodes=0`

## Current Bottleneck
- Execution appears to reach first opcode heartbeat (`opcode=0x4EFA @ pc=0x0400002A`) but no observed progression beyond heartbeat count 1 in current instrumentation.
- Need better signal from dispatcher/compiled-block path (not just interpreter pre-call hook) to locate where forward progress stalls.