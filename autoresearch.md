# Autoresearch: AArch64 JIT low-address RAM mapping

## Objective
Fix AArch64 JIT instability from host pointers to Mac RAM landing above 4GB.

Current failure signature is an allocator mismatch:
- low-address `MAP_FIXED` in `vm_acquire_mac()` bypasses the allocator’s reserved-pool setup,
- later SDL2 framebuffer allocation (`vm_acquire_reserved`) asserts on `reserved_buf`.

Goal: preserve allocator invariants while biasing the initial reservation into low address space.

## Metrics
- **Primary**: `jit_alive_sec` (s, higher is better)
  - `20` means process stayed alive for the full 20s run window.
- **Secondary**:
  - `build_ok`
  - `boot_alive`
  - `reserved_assert`
  - `popall_alloc_fail`
  - `block_pool_fail`
  - `jit_high_addr_warn`
  - `stage_coverage_score`

## How to Run
`B2_CPU_CORE_MODE=uae_cpu_2021 B2_JIT_PREF=true B2_ENABLE_AARCH64_JIT_EXPERIMENTAL=true ./autoresearch.sh`

## Files in Scope
- `BasiliskII/src/CrossPlatform/vm_alloc.cpp`
- `BasiliskII/src/Unix/main_unix.cpp`
- `BasiliskII/src/uae_cpu_2021/compiler/compemu_support_arm.cpp`
- `autoresearch.sh` (only minimal parameterization needed for this target)
- `autoresearch.md`
- `autoresearch.ideas.md`

## Off Limits
- `/workspace/fixtures/basilisk/images/Quadra800.ROM`
- `/workspace/fixtures/basilisk/images/HD200MB`

## Constraints
- Build with `--enable-aarch64-jit-experimental`.
- Use incremental `make`.
- Run with `jit=true`.
- No unsafe `MAP_FIXED` overwrite behavior.

## What’s Been Tried
- `VM_MAP_32BIT` path on AArch64: unsuitable.
- `MAP_FIXED` low-address RAM mapping in `vm_acquire_mac()`: caused `vm_acquire_reserved` assertion.
- Working hypothesis: shift low-address bias to the allocator reservation path (hint-based), then let normal reservation/sub-allocation flow proceed.
