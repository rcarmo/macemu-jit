# Autoresearch: BasiliskII non-JIT SDL control boot baseline

## Objective
Re-establish a trustworthy **pre-JIT control path** for BasiliskII on this host.

The experiment is **not** trying to optimize AArch64 JIT right now. It should instead maximize the chance that the emulator boots far enough to provide useful evidence about:
- boot progress,
- disk activity,
- video initialization,
- framebuffer/window behavior,
- and internal framebuffer dumps.

## Fixed starting points
Keep these fixed unless there is a very strong reason to change them:
- **ROM:** `/workspace/fixtures/basilisk/images/Quadra800.ROM`
- **Disk:** `/workspace/fixtures/basilisk/images/HD200MB`
- **Runtime mode:** `jit false`
- **Rendering path:** SDL only, software renderer requested
- **Disable other acceleration paths:** no JIT, no VOSF, no XF86 DGA, no XF86 VidMode, no fbdev DGA
- **Video mode:** `screen win/640/480`, `displaycolordepth 8`
- **Machine profile:** `modelid 14`, `cpu 4`, `fpu true`

## Metrics
- **Primary:** `control_boot_score` (**higher is better**)
  - `+40` if configure+make succeeds (`build_ok=1`)
  - `+20` if the emulator survives for 20 seconds (`boot_alive=1`)
  - `+10` if a BasiliskII window is found (`window_found=1`)
  - `+10` if enough explicit boot milestones are logged (`boot_progress=1`)
  - `+10` if disk/storage activity is visible in logs (`disk_activity=1`)
  - `+5` if at least one internal PNG dump is captured (`dump_activity=1`)
  - `+5` if the final window capture is non-solid (`window_nonsolid=1`)
  - `-20 * crash_count`
- **Secondary:**
  - `build_ok`
  - `boot_alive`
  - `window_found`
  - `boot_progress`
  - `disk_activity`
  - `dump_activity`
  - `window_nonsolid`
  - `boot_steps_seen`
  - `png_dump_count`
  - `crash_count`
  - `dump_signal_seen`
  - `dump_attempt_seen`
  - `dump_save_seen`

## How to run
`./autoresearch.sh`

Artifacts are written under `/workspace/tmp/autoresearch-control-<timestamp>`.

## Files in scope
- `autoresearch.sh` — conservative control harness
- `BasiliskII/src/main.cpp` — core boot-step logging
- `BasiliskII/src/Unix/main_unix.cpp` — process/bootstrap logging and line buffering
- `BasiliskII/src/video.cpp` — video init/debug traces
- `BasiliskII/src/SDL/video_sdl2.cpp` — SDL renderer/video traces and internal PNG dumps
- `BasiliskII/src/disk.cpp` — disk activity traces
- `BasiliskII/src/scsi.cpp` — storage probing traces
- `BasiliskII/src/sony.cpp` — floppy/disk driver traces

## Constraints
- Do **not** turn JIT back on in this experiment.
- Keep ROM and disk images fixed to the local fixture copies.
- Prefer observability and reproducibility over performance.
- Minimize build/runtime feature surface so the emulator has the best chance to boot.
- Keep internal framebuffer PNG dumps and boot logs for every run.
- Treat the SIGUSR2-triggered PNG dump path itself as a hypothesis that may require validation; do not assume missing PNGs means missing framebuffer updates until the signal/handler/dump pipeline is verified end-to-end.

## What's Been Tried
- Baseline on commit `cf71555d`: the direct non-JIT SDL software-rendered control lane builds, survives 20s, opens a window, and logs core boot milestones with score 90.
- That baseline produced `png_dump_count=0`, but the absence of PNGs is not yet trustworthy evidence of missing framebuffer activity because the signal/request/write path itself has not been validated end-to-end.
- Instrumentation run after the baseline showed that all four scheduled SIGUSR2 signals reached the process, but the dump path never advanced to request handling or file output.
- Next step: keep the conservative SDL path unchanged but route SIGUSR2 to the main thread only, so helper threads cannot consume the signal while the presentation thread misses the request latch.

## Definition of success for this phase
A good run is one where we can clearly answer:
1. Did BasiliskII build and stay alive?
2. Did it open a window?
3. Did the storage path show activity?
4. Which boot milestones were reached?
5. Did internal dumps/captures show any non-solid progression?

Only after this control path is trustworthy should JIT be reintroduced.
