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
TMX        := emu

.PHONY: build clean test test-jit test-headless run run-vnc start stop kill screenshot status help

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

# ── Run (tmux) ────────────────────────────────────────────────────

start: build ## Start emulator in tmux session '$(TMX)' with Xvfb + VNC
	@if tmux has-session -t $(TMX) 2>/dev/null; then \
	  echo "Session '$(TMX)' already exists. Use 'make stop' first."; exit 1; fi
	@pkill -9 -x BasiliskII 2>/dev/null; pkill -9 Xvfb 2>/dev/null; sleep 0.3
	tmux new-session -d -s $(TMX) -x 200 -y 50
	tmux send-keys -t $(TMX) 'Xvfb :99 -screen 0 640x480x24 -ac & sleep 1 && \
	  cd /workspace/projects/macemu && \
	  SDL_VIDEODRIVER=x11 DISPLAY=:99 $(BIN) \
	    --rom $(ROM) --disk $(DISK) --disk $(BENCH_DISK) \
	    --ramsize 16777216 --modelid 14 --cpu 4 \
	    --noclipconversion --nogui \
	    --vnc true --vnc-port $(VNC_PORT) \
	    2>/tmp/b2.stderr' Enter
	@echo "Emulator starting in tmux session '$(TMX)'"
	@echo "  VNC: port $(VNC_PORT)"
	@echo "  Logs: /tmp/b2.stderr"
	@echo "  Attach: tmux attach -t $(TMX)"
	@echo "  Screenshot: make screenshot"

stop: ## Stop emulator tmux session
	@pkill -9 -x BasiliskII 2>/dev/null || true
	@pkill -9 Xvfb 2>/dev/null || true
	@tmux kill-session -t $(TMX) 2>/dev/null || true
	@echo "Stopped."

status: ## Show emulator status
	@if pgrep -x BasiliskII >/dev/null; then \
	  pid=$$(pgrep -x BasiliskII); \
	  insn=$$(grep -oP 'insn=\K[0-9]+' /tmp/b2.stderr 2>/dev/null | tail -1); \
	  echo "Running (PID=$$pid, insn=$${insn:-?})"; \
	else echo "Not running"; fi
	@tmux has-session -t $(TMX) 2>/dev/null && echo "tmux session '$(TMX)': active" || echo "tmux session '$(TMX)': none"

screenshot: ## Take a screenshot of the running emulator
	@if ! pgrep Xvfb >/dev/null; then echo "No Xvfb"; exit 1; fi
	@f=/tmp/b2-screenshot-$$(date +%Y%m%d-%H%M%S).png; \
	  DISPLAY=:99 import -window root "$$f" && echo "Saved: $$f" || echo "Failed"

# ── Legacy (blocking) ─────────────────────────────────────────────

run-vnc: build ## Run with VNC (blocking, no tmux)
	@echo "=== BasiliskII + VNC on port $(VNC_PORT) ==="
	@pkill -9 Xvfb 2>/dev/null; sleep 0.3; \
	  Xvfb :99 -screen 0 640x480x24 & sleep 1; \
	  SDL_VIDEODRIVER=x11 DISPLAY=:99 $(BIN) \
	    --rom $(ROM) --disk $(DISK) --disk $(BENCH_DISK) \
	    --ramsize 16777216 --modelid 14 --cpu 4 \
	    --noclipconversion --nogui \
	    --vnc true --vnc-port $(VNC_PORT)

# ── Cleanup ───────────────────────────────────────────────────────

kill: ## Kill all BasiliskII, Xvfb, and tmux emu session
	@pkill -9 -x BasiliskII 2>/dev/null || true
	@pkill -9 Xvfb 2>/dev/null || true
	@tmux kill-session -t $(TMX) 2>/dev/null || true
	@echo "Done."
