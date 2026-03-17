# Amiberry ARM JIT Port Plan (macemu)

## Goal
Enable BasiliskII 68k JIT on ARM hosts in `macemu`, prioritizing ARM64 while keeping Linux x86/x86_64 JIT behavior unchanged.

## Current status
- `master` has x86/x86_64 JIT enabled.
- ARM/AArch64 builds are currently non-JIT by policy/configuration.
- `uae_cpu_2021/compiler/compemu_support.cpp` already contains ARM code paths, but key ARM backend files were missing from this tree.

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
3. Add an explicit experimental configure flag to enable ARM JIT attempts.

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
