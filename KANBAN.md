# Kanban

## Board

| ID | Title | Status | Priority | Owner | Branch | Last Update |
|---|---|---|---|---|---|---|
| ARM-JIT-001 | BasiliskII ARM JIT port from Amiberry backend | In Progress | High | @orange-agent | `feature/amiberry-arm-jit-port` | 2026-03-17 |

---

## Ticket: ARM-JIT-001 — BasiliskII ARM JIT port from Amiberry backend

### Why
Enable JIT acceleration for ARM hosts in `macemu/BasiliskII`, starting with controlled experimental gating and progressive bring-up.

### Scope (current)
- Add/maintain experimental build toggles.
- Import and wire ARM backend assets borrowed from Amiberry.
- Keep x86/x86_64 behavior unchanged.
- Track progress in one parent ticket (split later during triage).

### Recorded progress

#### Completed
- Created working branch: `feature/amiberry-arm-jit-port`.
- Added initial ARM backend import from Amiberry into:
  - `BasiliskII/src/uae_cpu_2021/compiler/`
  - imported: `codegen_arm*`, `codegen_arm64*`, `compemu_midfunc_arm*`, `compemu_midfunc_arm64*`, `flags_arm.h`, `aarch64.h`.
- Added experimental configure toggle:
  - `--enable-arm-jit-experimental`
  - currently ARM32 opt-in only; AArch64 intentionally warns and remains non-JIT.
- Added and updated plan file:
  - `AMIBERRY_ARM_JIT_PORT_PLAN.md`
  - includes validated toggle behavior and next phases.

#### Validation notes
- `configure --help` exposes `--enable-arm-jit-experimental`.
- On AArch64 with toggle enabled:
  - `Experimental ARM JIT toggle: yes`
  - `Use JIT compiler: no`
  - warning confirms AArch64 JIT wiring not complete yet.

### Commits recorded
- `29810006` — `jit: start amiberry arm backend import and port plan`
- `fa4d07d0` — `build: add experimental ARM JIT configure toggle`
- `81ec9ec0` — `docs: record experimental arm jit toggle status and validation`

### Next actions (parent ticket)
1. Add separate AArch64 experimental gate (independent from ARM32).
2. Wire AArch64 JIT path selection in core compiler files.
3. Resolve first compile breaks and produce a minimal successful ARM64 build.
4. Capture runtime smoke-test results.
5. Split into child tickets after triage (build wiring, core porting, runtime/debug, benchmarking).

### Risks / blockers
- Addressing mode defaults to memory banks in current environment (JIT requires direct addressing).
- ARM64 pointer-width and register-layout mismatches are likely in shared JIT paths.
- Runtime executable memory/ICache semantics differ across hosts.
