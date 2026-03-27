# Autoresearch: AArch64 JIT low-address RAM mapping

## Objective
Fix AArch64 JIT instability from host pointers to Mac RAM landing above 4GB.

Current failure signature is an allocator mismatch:
- low-address `MAP_FIXED` in `vm_acquire_mac()` bypasses the allocator’s reserved-pool setup,
- later SDL2 framebuffer allocation (`vm_acquire_reserved`) asserts on `reserved_buf`.

Goal: preserve allocator invariants while biasing the initial reservation into low address space.

## Metrics
- **Primary**: `lowaddr_score` (higher is better)
  - `40 * boot_alive + 30 * mac_ram_low32 + 30 * jit_code_low32`
  - 100 means alive run with both RAM and JIT code mapped below 4GB.
- **Secondary**:
  - `jit_alive_sec`
  - `build_ok`
  - `boot_alive`
  - `reserved_assert`
  - `popall_alloc_fail`
  - `block_pool_fail`
  - `jit_high_addr_warn`
  - `mac_ram_low32`
  - `jit_code_low32`
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
- `MAP_FIXED` low-address RAM mapping in `vm_acquire_mac()`: caused `vm_acquire_reserved` assertion (confirmed).
- **Kept fix set:**
  - `main_unix.cpp`: AArch64 `vm_acquire_mac()` now uses normal `vm_acquire()` (no fixed mapping bypass).
  - `vm_alloc.cpp`: AArch64 path uses low `MAP_BASE` and advances `next_address` by full allocated span (`size + RESERVED_SIZE` on first reservation) so JIT allocations do not collide with reserved framebuffer slice.
  - `compemu_support_arm.cpp`: ARM64 block pool allocation avoids `VM_MAP_32BIT` option bit to prevent allocator sanity-check failures.
- **Validation signal:** repeated JIT=true runs hit `lowaddr_score=100`, `reserved_assert=0`, `mac_ram_low32=1`, `jit_code_low32=1`.
- `MAP_FIXED_NOREPLACE` reservation probing was trialed as a hardening step; later removed for simplicity after consecutive stable low-address runs with the hint+ordering fix.
- **Rejected simplifications:**
  - Removing ARM64 block-pool `VM_MAP_32BIT` gate rollback test failed with `block_pool_fail=1` and `jit_alive_sec=0`.
  - Reverting `next_address` advancement to `+size` regressed `jit_code_low32` to `0`.
  - Removing AArch64 low `MAP_BASE` hint kept process alive but regressed both low-address metrics to `0`.
