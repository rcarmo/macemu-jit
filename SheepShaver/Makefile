# SheepShaver AArch64 JIT — Master Makefile
SHELL       := /bin/bash
SS_DIR      := src/Unix
SS_BIN      := $(SS_DIR)/SheepShaver
JIT_DIR     := src/kpx_cpu/src/cpu/jit/aarch64
ROM_HARNESS := rom-harness/rom-harness

ROM         := /workspace/tmp/rpi-basilisk-assets/ppc-roms/PowerMac-9500-OldWorld.rom
DISK        := /workspace/tmp/ss-test-disk.img
BENCH_DISK  := /workspace/tmp/Benchmark.hda
PREFS_DIR   := /tmp/ss-desktop
PREFS_FILE  := $(PREFS_DIR)/.sheepshaver_prefs
VNC_PORT    := 5999

JIT_CXX     := g++
JIT_CXXFLAGS := -g -O2 -std=c++11 -DHAVE_CONFIG_H
JIT_INCLUDES := -I$(JIT_DIR)

.PHONY: all build build-ss build-rom-harness \
        test test-opcodes test-rom \
        run run-jit screenshot kill clean \
        ensure-xvfb ensure-prefs

all: build

build: build-ss build-rom-harness

build-ss:
	@echo "=== Building SheepShaver ==="
	@cd $(SS_DIR) && $(MAKE) -j$$(nproc) 2>&1 | tail -3 || true
	@echo "=== Compiling JIT ==="
	@$(JIT_CXX) $(JIT_CXXFLAGS) $(JIT_INCLUDES) -c \
		-o $(SS_DIR)/obj/ppc-jit.o $(JIT_DIR)/ppc-jit.cpp
	@cd $(SS_DIR) && $(JIT_CXX) -o SheepShaver obj/*.o \
		-lpthread -lm -lSDL2 \
		-lgtk-x11-2.0 -lgdk-x11-2.0 -lpangocairo-1.0 -latk-1.0 -lcairo \
		-lgdk_pixbuf-2.0 -lgio-2.0 -lpangoft2-1.0 -lpango-1.0 \
		-lgobject-2.0 -lglib-2.0 -lharfbuzz -lvncserver -lfontconfig -lfreetype
	@echo "Built: $(SS_BIN)"

build-rom-harness:
	@echo "=== Building ROM harness ==="
	@cd rom-harness && $(MAKE)

test: test-opcodes test-rom
	@echo ""
	@echo "=== All tests complete ==="

test-opcodes: build-ss
	@echo ""
	@echo "=== JIT Opcode Equivalence Harness ==="
	@cd jit-test && bash run.sh 2>&1 | grep "METRIC\|FAIL\|ERROR" | head -20

test-rom: build-rom-harness
	@echo ""
	@echo "=== ROM Harness (10K blocks) ==="
	@timeout 120 $(ROM_HARNESS) $(ROM) \
		--count=10000 --min-insns=1 --max-insns=64 2>&1 | \
		grep "Score:\|Passed:\|Failed:\|Testable\|JIT compile\|SIGSEGV"

ensure-xvfb:
	@pgrep -x Xvfb > /dev/null 2>&1 || \
		{ Xvfb :99 -screen 0 800x600x24 & sleep 1; echo "Started Xvfb :99"; }

ensure-prefs:
	@mkdir -p $(PREFS_DIR)
	@printf 'rom $(ROM)\ndisk $(DISK)\nramsize 134217728\nscreen win/640/480\ndisplaycolordepth 8\nnosound true\nnocdrom true\nnogui true\nignoreillegal true\nvncserver true\nvncport $(VNC_PORT)\n' > $(PREFS_FILE)
	@if [ -f "$(BENCH_DISK)" ]; then \
		grep -q "Benchmark" $(PREFS_FILE) || \
		echo "disk $(BENCH_DISK)" >> $(PREFS_FILE); \
	fi

run: build-ss ensure-xvfb ensure-prefs kill
	@echo "=== Starting SheepShaver (interpreter) on VNC port $(VNC_PORT) ==="
	@GDK_BACKEND=x11 SDL_VIDEODRIVER=x11 DISPLAY=:99 HOME=$(PREFS_DIR) \
		$(SS_BIN) > /dev/null 2>/tmp/ss-stderr.log &
	@sleep 2
	@echo "PID: $$(pgrep -x SheepShaver | head -1)"
	@echo "VNC: vnc://localhost:$(VNC_PORT)"

run-jit: build-ss ensure-xvfb ensure-prefs kill
	@echo "=== Starting SheepShaver (JIT) on VNC port $(VNC_PORT) ==="
	@SS_USE_JIT=1 GDK_BACKEND=x11 SDL_VIDEODRIVER=x11 DISPLAY=:99 HOME=$(PREFS_DIR) \
		$(SS_BIN) > /dev/null 2>/tmp/ss-jit-stderr.log &
	@sleep 2
	@echo "PID: $$(pgrep -x SheepShaver | head -1)"
	@echo "VNC: vnc://localhost:$(VNC_PORT)"

screenshot:
	@export PATH="$$HOME/.local/bin:$$PATH"; \
	vncdotool -s 127.0.0.1::$(VNC_PORT) capture /workspace/tmp/ss-screenshot-$$(date +%H%M%S).png 2>&1 && \
		echo "Saved: /workspace/tmp/ss-screenshot-$$(date +%H%M%S).png" || \
		echo "VNC not available on port $(VNC_PORT)"

kill:
	@pkill -9 -x SheepShaver 2>/dev/null; true
	@sleep 0.3
	@echo "All SheepShaver processes killed"

clean: kill
	@cd $(SS_DIR) && rm -f obj/ppc-jit.o SheepShaver 2>/dev/null; true
	@cd rom-harness && $(MAKE) clean 2>/dev/null; true
	@echo "Clean complete"
