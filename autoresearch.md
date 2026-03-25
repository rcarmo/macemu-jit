# Autoresearch: BasiliskII AArch64 experimental JIT smoke bring-up

## Objective
Get the JIT-enabled BasiliskII build working with both runtime modes:
- `--jit false` must survive for at least 20 seconds and render a non-solid window.
- `--jit true` must survive for at least 20 seconds and render a non-solid window.

This is for the opt-in AArch64 JIT path only (`--enable-aarch64-jit-experimental`).
Display correctness is prioritized first.

## Metrics
- **Primary**: `jit_smoke_score` (points, **higher is better**)
  - `+30` if JIT-enabled configure+make succeeds (`build_ok=1`)
  - `+20` if `jit=false` survives >=20s with window (`off_alive=1`)
  - `+20` if `jit=false` capture is non-solid (`off_nonsolid=1`)
  - `+20` if `jit=true` survives >=20s with window (`on_alive=1`)
  - `+10` if `jit=true` capture is non-solid (`on_nonsolid=1`)
  - `-15 * crash_count` crash penalty (`SIGSEGV`/`SIGABRT`/abort/segfault indications)
- **Secondary**:
  - `build_ok`
  - `off_alive`
  - `off_nonsolid`
  - `on_alive`
  - `on_nonsolid`
  - `crash_count`

## How to Run
`./autoresearch.sh`

The script emits structured lines:
- `METRIC jit_smoke_score=<number>`
- `METRIC build_ok=<0|1>`
- `METRIC off_alive=<0|1>`
- `METRIC off_nonsolid=<0|1>`
- `METRIC on_alive=<0|1>`
- `METRIC on_nonsolid=<0|1>`
- `METRIC crash_count=<integer>`

Artifacts are written under `/workspace/tmp/autoresearch-jit-<timestamp>`.

## Files in Scope
- `BasiliskII/src/Unix/configure.ac` — configure-time enablement guards and feature flags.
- `BasiliskII/src/Unix/Makefile.in` — JIT source wiring for AArch64 experimental build.
- `BasiliskII/src/uae_cpu_2021/compiler/compemu_support.cpp` — allocator/protection/lifecycle for JIT code cache and block pools.
- `BasiliskII/src/uae_cpu_2021/compiler/compemu.h` — shared JIT invariants and pointer-width checks.
- `BasiliskII/src/uae_cpu_2021/compiler/compemu_midfunc_arm64.cpp` — AArch64 emitter/patching behavior.
- `BasiliskII/src/uae_cpu_2021/compiler/compemu_midfunc_arm64_2.cpp` — AArch64 helper emission path.
- `BasiliskII/src/uae_cpu_2021/compiler/gencomp_arm.c` — generated ARM/AArch64 backend glue.
- `BasiliskII/src/uae_cpu_2021/registers.h` — register model layout used by JIT backend assumptions.

## Off Limits
- Do not remove or auto-enable JIT globally; keep AArch64 JIT **opt-in only** behind `--enable-aarch64-jit-experimental`.
- Do not regress x86/x86_64 JIT paths.
- Do not modify external ROM/disk assets in `/workspace/projects/rpi-basilisk2-sdl2-nox`.

## Constraints
- Build must continue to work for existing non-AArch64 configurations.
- Prefer architecture-guarded changes over broad behavioral changes.
- Prioritize display correctness and survival in the smoke harness before deeper performance work.
- Keep diagnostic artifacts for each run in `/workspace/tmp/autoresearch-jit-<ts>`.

## What's Been Tried
- Existing branch state contains initial experimental AArch64 JIT wiring and generator imports.
- Known blocker from user report: JIT-enabled binaries may crash early; recent GDB traces hit `vm_release` while in `vm_alloc`, even with runtime `jit=false`.
- Known symptom in prior diagnostics: repeated `JIT: Branch to target too long.` messages and eventual abort with runtime `jit=true`.
- First step in this session validated autoresearch tools and METRIC parsing path.
