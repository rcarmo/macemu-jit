# AArch64 JIT L2 Isolation Matrix

## Purpose

This matrix is for isolating the remaining `optlev=2` failure **systematically**.

Current working assessment:

> The remaining L2 failure is no longer best described as a broad async/tick problem. The strongest current lead is a **successor-entry / direct-chain contract bug** in the tiny helper `JMP` chain around `0x040b3566 -> 0x040b35c6 -> 0x040b35c8`.

So the matrix should now prioritize **JIT continuation structure for specific helper successor blocks** before returning to generic opcode-family hunting.

---

## Rules of the experiment set

1. Change **one factor at a time** relative to the chosen baseline.
2. Keep these fixed unless the row explicitly changes them:
   - ROM
   - disk image
   - prefs
   - Xvfb/display setup
   - `B2_JIT_MANAGED_IRQ=1`
   - run duration
3. Record the **first matched-PC divergence**, not just boot/no-boot.
4. Run each experiment at least **3 times** if results are noisy.
5. Treat `optlev=1` as the architectural JIT-structure control and `optlev=2` as the async/native candidate.

---

## Common metrics to collect for every run

| Metric | Why it matters |
|---|---|
| `DiskStatus` count | coarse boot progress |
| `SCSIGet` count | coarse boot progress |
| first `SDL_VIDEOINT` / `SDL_PRESENT` | whether VBL/video servicing progressed |
| `JIT_COMPILE optlev=2` count | how far native execution got |
| first matched-PC divergence | best structural comparator |
| first bad `pc_p` / low-PC recovery | whether corruption path still appears |
| `InterruptFlags`, `spcflags`, `Ticks` (`0x16a`) traces when relevant | async timing visibility |
| `PCTRACE` around the divergence window | aligned L1/L2 comparison |

Recommended baseline trace settings when comparing L1/L2:

```bash
B2_JIT_PCTRACE=2000
```

Recommended extra trace for IRQ-focused rows:

```bash
B2_TRACE_IRQMANAGED=1
B2_TRACE_IRQSEQ=1
```

---

## Baseline control set

| ID | Configuration | Purpose | Expected interpretation |
|---|---|---|---|
| C0 | non-JIT | architectural emulator control | if this fails, stop; not an L2 issue |
| C1 | JIT `B2_JIT_MAX_OPTLEV=1` | JIT structure control | if this fails while C0 passes, structural JIT issue |
| C2 | JIT default `optlev=2` | failing baseline | reference for all comparisons |
| C3 | JIT `B2_JIT_FLUSH_EACH_OP=1` | timing/continuation perturbation control | if divergence moves without local state delta, async/safe-point sensitivity is implicated |

Run these first and keep logs.

---

## Axis A — interrupt generation / sampling

These rows test whether the problem comes from **when** asynchronous work becomes visible.

| ID | Factor changed | How to change it | Signal if it helps |
|---|---|---|---|
| A1 | 60Hz tick generation disabled | temporary build flag / env to skip `one_tick()` body | L2 stabilizes or divergence moves much later → tick thread is primary driver |
| A2 | 60Hz tick generation without SDL event pump | disable `SDL_PumpEventsFromMainThread()` inside `one_tick()` | improvement points to main-thread SDL pump / host event delivery |
| A3 | 60Hz tick generation without `INTFLAG_1HZ` | suppress the 1Hz path only | improvement points to 1Hz storage/time side effects |
| A4 | 60Hz tick generation without `INTFLAG_60HZ` | suppress 60Hz IRQ while leaving thread running | improvement points to VBL/timer IRQ fan-out |
| A5 | tick snapshot at dispatcher only | sample pending ticks only on dispatcher return, not mid-chain | improvement points to safe-point timing rather than opcode semantics |
| A6 | forced dispatcher return / no direct chaining | disable or heavily reduce direct chain continuation | improvement points to block-return timing / re-entry |

### Priority within Axis A

Run in this order:

1. `A6`
2. `A2`
3. `A4`
4. `A3`
5. `A1`
6. `A5`

Reason: these are the highest-value tests for proving that the bug is boundary timing, not local arithmetic semantics.

---

## Axis B — IRQ fan-out isolation

These rows keep interrupt arrival but remove **individual guest-visible side effects**.

| ID | IRQ service branch | How to isolate | Interpretation if it helps |
|---|---|---|---|
| B1 | `VideoInterrupt()` | temporarily no-op in `M68K_EMUL_OP_IRQ` | video/VBL handling is perturbing timing or shared state |
| B2 | `DoVBLTask` trap | skip `Execute68kTrap(0xa072)` | guest VBL task execution is the destabilizer |
| B3 | `TimerInterrupt()` | temporarily no-op | time-manager callbacks are the destabilizer |
| B4 | `ADBInterrupt()` | temporarily no-op | host input / ADB polling path is the destabilizer |
| B5 | 1Hz storage path (`SonyInterrupt`, `DiskInterrupt`, `CDROMInterrupt`) | temporarily no-op | storage side effects from 1Hz path matter |
| B6 | `EtherInterrupt()` | temporarily no-op | network async path matters |
| B7 | `AudioInterrupt()` | temporarily no-op | audio path matters |

### Priority within Axis B

Run in this order:

1. `B1`
2. `B2`
3. `B4`
4. `B3`
5. `B5`
6. `B6`
7. `B7`

Reason: video/VBL/ADB are the most plausible active perturbation sources in the current setup.

---

## Axis C — host/UI side effects

These rows isolate factors that are external to guest architecture but can perturb wall time or event delivery.

| ID | Factor | How to change it | Interpretation if it helps |
|---|---|---|---|
| Cx1 | redraw thread | disable redraw thread, keep CPU running | redraw cadence / event handling is perturbing L2 |
| Cx2 | SDL input handling | suppress host input processing / keep no synthetic events | ADB path is externally driven |
| Cx3 | VNC | ensure VNC disabled | background screenshot/event thread contributes timing noise |
| Cx4 | audio | keep `nosound true` vs explicit audio-on comparison | audio callback thread contributes |
| Cx5 | renderer backend | compare `opengles2` vs software renderer | GPU/driver/compositor timing contributes |
| Cx6 | event pumping only on redraw thread | remove main-thread SDL pump from tick path | split SDL/event responsibilities are destabilizing |

---

## Axis D — JIT structure perturbations (not opcode semantics)

These rows perturb continuation structure while keeping native code enabled.

| ID | Perturbation | How to change it | Interpretation if it helps |
|---|---|---|---|
| D1 | flush after every op | `B2_JIT_FLUSH_EACH_OP=1` | continuation sensitivity confirmed |
| D2 | flush after selected PCs | `B2_JIT_FLUSH_OP_PCS=...` | identifies boundary-sensitive windows |
| D3 | end block after every compiled op | compile native op, then force endblock | stronger proof of block-boundary sensitivity |
| D4 | no direct successor chaining | return to dispatcher for successors | chain timing / cache handoff issue |
| D5 | disable inter-instruction `spcflags` check | only sample at block end (diagnostic only) | whether mid-block safe-point injection itself is perturbing execution |
| D6 | charge countdown only at block end | diagnostic timing perturbation | whether partial-cycle accounting feeds the issue |

---

## Suggested execution order

### Phase 0 — controls
Run:
- `C0`, `C1`, `C2`, `C3`

### Phase 1 — prove or disprove async boundary dominance
Run:
- `A6` → `A2` → `A4` → `B1` → `B2`

Stop early if one of these dramatically changes:
- first matched-PC divergence location
- low-PC corruption family frequency
- `DiskStatus` / `SCSIGet`

### Phase 2 — isolate the active branch
If `A4` helps, continue with:
- `B1`, `B2`, `B3`, `B4`, `B5`

If `A2` helps, continue with:
- `Cx1`, `Cx2`, `Cx6`

If `A6` helps, continue with:
- `D3`, `D4`, `D5`

### Phase 3 — only then return to opcode/helper semantics
Only after async/safe-point sensitivity is bounded should we revisit:
- `0x0404b0be..0x0404b0cc`
- `0x040b3566..0x040b3638`
- helper transform semantics near `0x040b34dc`

---

## Result recording template

Use one row per run:

| Run ID | Base | Factor changed | First matched-PC divergence | `DiskStatus` | `SCSIGet` | first `pc_p` failure | Notes |
|---|---|---|---|---:|---:|---|---|
| example | C2 | B1 (`VideoInterrupt` off) | `0x0401b7ea -> 0x040b34dc` | 0 | 0 | none | divergence moved later |

---

## Interpretation guide

| Observation | Meaning |
|---|---|
| Disabling tick generation stabilizes L2 | issue is fundamentally async/tick visibility |
| Disabling `VideoInterrupt()` helps | VBL/video side effects are perturbing execution |
| Disabling `ADBInterrupt()` helps | host input/event path is the active destabilizer |
| No direct chaining helps | safe-point/re-entry cadence is the real issue |
| Flush perturbations help but show no local architectural delta | timing/re-entry sensitivity, not a local semantic bug |
| None of the async factors matter | only then return to helper/opcode semantics |

---

## Current recommendation

The highest-value next rows are now targeted structure probes in the helper chain:

- force only successor target `0x040b35c6`
- force only successor target `0x040b35c8`
- compare direct vs non-direct vs `execute_normal` entry for those targets
- instrument the actual handoff contract (`target pc`, chosen handler, live regs/flags, `pc_p`) at those exits

Broader async rows (`A2`, `B1`, `B2`) remain useful only after the helper successor contract is either repaired or ruled out.
