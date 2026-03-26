# Autoresearch: BasiliskII ARM boot divergence (uae_cpu vs uae_cpu_2021)

## Objective
The emulator is not reaching real boot; framebuffer stays zero because ROM startup appears stuck early.

This session pivots from capture-pipeline tuning to **CPU-core divergence diagnosis**:
1. Validate a trusted reference build using the **original `uae_cpu`** core with known configure flags.
2. Compare behavior against `uae_cpu_2021` on identical ROM/disk/runtime conditions.
3. Apply minimal fixes so `uae_cpu_2021` reaches the same boot milestones.

## Metrics
- **Primary**: `stage_coverage_score` (higher is better)
  - +15 reset reached
  - +15 saw CLKNOMEM activity
  - +15 saw any intmask transition
  - +15 reached PATCH_BOOT_GLOBS
  - +15 reached CheckLoad
  - +15 reached first VideoInterrupt
  - +10 reached first framebuffer write
- **Secondary**:
  - `build_ok`
  - `boot_alive`
  - `reset_seen`
  - `clknomem_calls`
  - `intmask_transition_count`
  - `patch_boot_globs_seen`
  - `checkload_seen`
  - `video_interrupt_seen`
  - `framebuffer_write_seen`
  - `screenshot_count`
  - `core_is_original`
  - `core_is_2021`

## How to Run
`./autoresearch.sh`

Notes:
- Use env `B2_CPU_CORE_MODE=uae_cpu` for original core runs.
- Use env `B2_CPU_CORE_MODE=uae_cpu_2021` for 2021 core runs.
- The script emits `METRIC ...` lines and writes artifacts under `/workspace/tmp/autoresearch-boot-divergence-<timestamp>`.

## Files in Scope
- `autoresearch.sh` — build/run harness and metrics extraction
- `BasiliskII/src/emul_op.cpp` — ROM emul-op milestone logs (RESET/CLKNOMEM/PATCH_BOOT_GLOBS/CheckLoad/IRQ)
- `BasiliskII/src/SDL/video_sdl2.cpp` — first framebuffer write milestone log
- `BasiliskII/src/Unix/configure.ac` — only if needed for robust per-core selection
- `autoresearch.md` — session state and learned context
- `autoresearch.ideas.md` — deferred ideas backlog

## Off Limits
- ROM asset: `/workspace/fixtures/basilisk/images/Quadra800.ROM`
- Disk asset: `/workspace/fixtures/basilisk/images/HD200MB`
- No ROM/disk modifications.

## Constraints
- Every experiment run must use `./autoresearch.sh`.
- Hard 120s timeout on diagnostic runs.
- Robust teardown (TERM then KILL).
- Keep runtime non-JIT (`jit false`) for this phase.
- Do not overfit by faking milestones; all metrics must come from real logs/events.

## Phase Plan
1. **Phase 1 (runs 1-2):** build/run original `uae_cpu` with reference configure flags (plus Linux fallback `--disable-sdl-framework` when Objective-C frontend is unavailable):
   - `--enable-sdl-audio --enable-sdl-framework --enable-sdl-video --disable-vosf --without-mon --without-esd --without-gtk --disable-jit-compiler --with-uae-core=legacy`
2. **Phase 2 (runs 3-10):** compare original vs `uae_cpu_2021` milestone coverage and logs; focus on:
   - CLKNOMEM behavior
   - interrupt/intmask transitions
   - reset/bootstrap path
   - register state presented to emulops
3. **Phase 3 (runs 11+):** apply minimal targeted fixes informed by phase-2 evidence.

## What's Been Tried
- Added explicit boot-stage instrumentation:
  - `BOOT_STAGE RESET fired`
  - CLKNOMEM counter snapshots + register payload
  - intmask transition tracing from EMUL_OP entry
  - PATCH_BOOT_GLOBS and CHECKLOAD first-hit markers
  - first VideoInterrupt marker
  - first framebuffer write marker in SDL2 update path
- Phase 1 complete:
  - Original `uae_cpu` now builds/runs in this environment using reference flags plus an automatic Linux-only fallback from `--enable-sdl-framework` to `--disable-sdl-framework` when Objective-C toolchain support is missing (`cc1obj`).
  - Reference runs are stable at `stage_coverage_score=100` with high intmask-transition activity and both CheckLoad+VideoInterrupt reached.
- Phase 2 complete:
  - `uae_cpu_2021` initially reproduced the historical divergence at `stage_coverage_score=70`:
    - `checkload_seen=0`
    - `video_interrupt_seen=0`
    - `intmask_transition_count=1` (stuck at initial 7)
- Root-cause direction validated:
  - The AArch64 `-DOPTIMIZED_FLAGS` path is implicated in the divergence.
  - Disabling that path for 2021 recovers full milestones.
- Durable fix applied:
  - `BasiliskII/src/Unix/configure.ac` no longer injects `-DOPTIMIZED_FLAGS` in the AArch64 define set.
  - Harness now regenerates `configure` when `configure.ac` is newer, ensuring config changes actually take effect.
  - Added first-class `--with-uae-core=auto|legacy|2021` configure support so core selection is reproducible without ad-hoc Makefile rewriting.
  - MPFR FPU and `cpufunctbl.cpp` are now gated to the 2021 core path on ARM/AArch64.
  - After these fixes, both cores reach full stage coverage (`100`) via clean configure-time selection.
- Additional harness hardening/simplification after fix:
  - configure invocation paths were deduplicated through a shared helper in `autoresearch.sh`.
  - process teardown is centralized via `terminate_pid()` to keep TERM/KILL behavior consistent.
  - stale precheck/config noise was removed (unused `python3` prerequisite and obsolete `--disable-nls` configure flag).
  - crash-side artifact handling was simplified to lightweight metadata + tail logs (no heavy debugger/core-processing path in normal workflow), and is now gated to actual crash-like exits (`SIGABRT`/`SIGSEGV`) to avoid false crash artifacts from intentional harness shutdown.
  - ideas backlog was pruned after trying crash-capture instrumentation in a no-crash run; remaining high-value path is dual-run comparative mode.
  - dual-run mode is still deferred because a full two-core build+run sequence appears likely to exceed the session's 120s per-command diagnostic budget; keep it as optional/off-path tooling.
- Current state:
  - Both cores (`uae_cpu`, `uae_cpu_2021`) now reach full observed milestones under identical ROM/disk/non-JIT/Xvfb workload.
