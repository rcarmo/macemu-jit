# macemu — top-level Makefile
# Targets for building, testing, and running BasiliskII with ARM64 JIT.

SHELL      := /bin/bash
UNIX_DIR   := BasiliskII/src/Unix
BIN        := $(UNIX_DIR)/BasiliskII
ROM        := /workspace/projects/rpi-basilisk2-sdl2-nox/Quadra800.ROM
DISK       := /workspace/fixtures/basilisk/images/HD200MB
BENCH_DISK := /workspace/fixtures/basilisk/images/Benchmark.hda
NPROC      := $(shell nproc)
VNC_PORT   := 5900

.PHONY: build clean test test-jit test-headless run-vnc kill screenshot help

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
	  awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-18s\033[0m %s\n", $$1, $$2}'

# ── Build ─────────────────────────────────────────────────────────

build: ## Build BasiliskII (incremental)
	cd $(UNIX_DIR) && make -j$(NPROC)

rebuild: ## Full rebuild (regenerate gencomp + compemu)
	cd $(UNIX_DIR) && \
	  rm -f obj/gencomp compemu.cpp compstbl.cpp comptbl.h obj/compemu*.o obj/compstbl.o && \
	  make obj/gencomp && ./obj/gencomp && make -j$(NPROC)

clean: ## Clean build artifacts
	cd $(UNIX_DIR) && \
	  rm -f obj/*.o compemu.cpp compstbl.cpp comptbl.h cpudefs.cpp obj/gencomp obj/build68k

# ── Test ──────────────────────────────────────────────────────────

test: test-jit ## Run all tests

test-jit: build ## Run JIT opcode equivalence harness (301 vectors)
	./jit-test/run.sh

test-headless: build ## Headless boot test (no display, no disk, 60s timeout)
	@echo "=== Headless ROM boot (optlev=0, 60s) ==="
	@W=$$(mktemp -d /tmp/b2-headless-XXXXXX); \
	  trap 'rm -rf "$$W"' EXIT; \
	  printf 'rom $(ROM)\nramsize 16777216\nmodelid 14\ncpu 4\nnogui true\n' > "$$W/prefs"; \
	  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy HOME="$$W" \
	    timeout --kill-after=5s 60s $(BIN) --config "$$W/prefs" \
	    > "$$W/stdout.log" 2> "$$W/stderr.log"; \
	  rc=$$?; \
	  insn=$$(grep -oP 'insn=\K[0-9]+' "$$W/stderr.log" | tail -1); \
	  pcs=$$(grep -oP 'pc=0x[0-9a-f]+' "$$W/stderr.log" | sort -u | wc -l); \
	  echo "Exit: $$rc  Instructions: $${insn:-0}  Unique PCs: $${pcs:-0}"; \
	  tail -5 "$$W/stderr.log"; \
	  rm -rf "$$W"

test-boot: build ## Full boot test with Xvfb + screenshot (45s)
	@echo "=== Full boot test (Xvfb, 45s) ==="
	@pkill -9 Xvfb 2>/dev/null; sleep 0.3; \
	  Xvfb :99 -screen 0 640x480x24 & XPID=$$!; sleep 1; \
	  W=$$(mktemp -d /tmp/b2-boot-XXXXXX); \
	  printf 'rom $(ROM)\ndisk $(DISK)\nramsize 16777216\nmodelid 14\ncpu 4\nnogui true\n' > "$$W/prefs"; \
	  SDL_VIDEODRIVER=x11 DISPLAY=:99 \
	    timeout --kill-after=5s 45s $(BIN) --config "$$W/prefs" \
	    > "$$W/stdout.log" 2> "$$W/stderr.log" & B2PID=$$!; \
	  sleep 40; \
	  DISPLAY=:99 import -window root /tmp/b2-boot-screenshot.png 2>/dev/null; \
	  wait $$B2PID 2>/dev/null; rc=$$?; \
	  insn=$$(grep -oP 'insn=\K[0-9]+' "$$W/stderr.log" | tail -1); \
	  echo "Exit: $$rc  Instructions: $${insn:-0}"; \
	  echo "Screenshot: /tmp/b2-boot-screenshot.png"; \
	  kill $$XPID 2>/dev/null; rm -rf "$$W"

# ── Run ───────────────────────────────────────────────────────────

run-vnc: build ## Run with VNC server on port $(VNC_PORT) (headless)
	@echo "=== BasiliskII + VNC on port $(VNC_PORT) ==="
	@pkill -9 Xvfb 2>/dev/null; sleep 0.3; \
	  Xvfb :99 -screen 0 640x480x24 & sleep 1; \
	  SDL_VIDEODRIVER=x11 DISPLAY=:99 $(BIN) \
	    --rom $(ROM) \
	    --disk $(DISK) \
	    --disk $(BENCH_DISK) \
	    --ramsize 16777216 --modelid 14 --cpu 4 \
	    --noclipconversion --nogui \
	    --vnc true --vnc-port $(VNC_PORT)

# ── Cleanup ───────────────────────────────────────────────────────

kill: ## Kill all BasiliskII and Xvfb processes
	@echo "Killing BasiliskII and Xvfb..."
	@pkill -9 -x BasiliskII 2>/dev/null || true
	@pkill -9 Xvfb 2>/dev/null || true
	@echo "Done."

screenshot: ## Take a screenshot of the running emulator (DISPLAY=:99)
	@DISPLAY=:99 import -window root /tmp/b2-screenshot-$$(date +%H%M%S).png 2>/dev/null && \
	  echo "Saved: /tmp/b2-screenshot-$$(date +%H%M%S).png" || \
	  echo "No display on :99"
