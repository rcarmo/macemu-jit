# Amiberry ARM JIT Port Plan (macemu)

## Goal
Enable BasiliskII 68k JIT on ARM hosts in `macemu`, prioritizing ARM64 while keeping Linux x86/x86_64 JIT behavior unchanged.

## Current status
- `master` has x86/x86_64 JIT enabled.
- ARM/AArch64 builds are currently non-JIT by policy/configuration.
- `uae_cpu_2021/compiler/compemu_support.cpp` already contains ARM code paths, but key ARM backend files were missing from this tree.
- Experimental configure toggles now exist in-tree:
  - `--enable-arm-jit-experimental`
  - `--enable-aarch64-jit-experimental`
- Current behavior on AArch64 host:
  - toggle is visible and tracked in configure summary
  - `Use JIT compiler` still falls back to `no` in this environment because addressing mode resolves to `memory banks`
- Toggle validation runs (2026-03-17):
  - `configure --help` shows both ARM and AArch64 experimental toggles
  - `--enable-aarch64-jit-experimental` summary reports:
    - `Experimental AArch64 JIT toggle: yes`
    - `Use JIT compiler: no` (due to addressing mode gate)
- AArch64 JIT compile probe (2026-03-17):
  - Forced direct-addressing probe build (`ac_cv_have_asm_extended_signals=yes`)
    reaches JIT compilation stage with:
    - `Use JIT compiler: yes`
    - `Addressing mode: direct`
  - Successfully compiled initial JIT units:
    - `obj/compemu_support.o`
    - `obj/compemu1.o` through `obj/compemu8.o`
    - `obj/compstbl.o`
  - Current environment blockers before full build:
    - missing SDL headers (`SDL.h`) for video objects
    - missing MPFR headers (`mpfr.h`) for `compemu_fpp.cpp`

## Branch
`feature/amiberry-arm-jit-port`

## Borrowing strategy from Amiberry

### High-confidence direct borrow (already started)
Import backend support files from Amiberry’s `src/jit/arm/` into:
`BasiliskII/src/uae_cpu_2021/compiler/`

Imported files:
- `codegen_arm.cpp`
- `codegen_arm.h`
- `codegen_arm64.cpp`
- `codegen_arm64.h`
- `compemu_midfunc_arm.cpp`
- `compemu_midfunc_arm.h`
- `compemu_midfunc_arm2.cpp`
- `compemu_midfunc_arm2.h`
- `compemu_midfunc_arm64.cpp`
- `compemu_midfunc_arm64_2.cpp`
- `flags_arm.h`
- `aarch64.h`

### Medium-confidence borrow
- AArch64-specific JIT correctness fixes in Amiberry (`uintptr` cleanliness, pointer-width handling, JIT memory allocation semantics).
- JIT exception handling and cache maintenance patterns for ARM64.

### Low-confidence / adapt-required
- Direct drop-in of Amiberry ARM JIT engine files (`compemu_support_arm.cpp`, `compemu_arm.h`) due host emulator differences (Mac glue, register struct shape, memory subsystem integration).

## Execution phases

### Phase 1 — Compile baseline on ARM with imported backend files
1. Wire includes/ifdefs so ARM backend files are actually selectable during JIT builds.
2. Keep JIT disabled by default on ARM until compile/test passes.
3. Add explicit experimental configure flags to enable ARM32/AArch64 JIT attempts. ✅ (present in-tree)

Exit criteria:
- `configure && make` succeeds for ARM target with experimental ARM JIT toggled on.

### Phase 2 — ARM32 JIT bring-up (if still needed)
1. Validate ARM32 (`CPU_arm`) codegen path.
2. Fix compile breaks and runtime assertions.
3. Run smoke tests through Mac boot and basic workload.

Exit criteria:
- No immediate crashes with JIT enabled on ARM32.

### Phase 3 — ARM64 JIT enablement (primary)
1. Add/adjust `CPU_aarch64` path selection in JIT core.
2. Port pointer-width and register-layout assumptions (from Amiberry fixes where applicable).
3. Ensure instruction cache flush + executable memory mapping are correct for Linux ARM64.

Exit criteria:
- BasiliskII boots with JIT on Linux ARM64.
- Stable under representative UI and disk workloads.

### Phase 4 — Validation + hardening
1. Regression checks: x86/x86_64 JIT unchanged.
2. Add benchmarks (interpreter vs JIT on ARM64).
3. Document caveats and default setting decisions.

Exit criteria:
- PR-ready patchset with reproducible performance and stability notes.

## Risks
- JIT code assumes x86-centric behaviors in shared paths.
- ARM64 pointer-width assumptions may require structural changes beyond simple file import.
- Runtime memory mapping constraints vary by OS/kernel hardening.

## Notes
- Keep this work isolated in this branch until ARM64 boot stability is proven.
- Prefer small, testable commits per subsystem (build wiring, compile fixes, runtime fixes).
