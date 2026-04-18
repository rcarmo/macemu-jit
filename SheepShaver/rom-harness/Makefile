# ROM Harness — headless JIT exerciser for SheepShaver AArch64 JIT
#
# Compiles the rom-harness standalone against the JIT backend,
# without any SheepShaver runtime dependencies.

CXX      = g++
CXXFLAGS = -g -O2 -std=c++11 -Wall -Wno-unused-function
LDFLAGS  =

JITDIR   = ../src/kpx_cpu/src/cpu/jit/aarch64

INCLUDES = -I$(JITDIR)

ROM      ?= /workspace/tmp/rpi-basilisk-assets/ppc-roms/PowerMac-9500-OldWorld.rom

SRCS     = rom-harness.cpp $(JITDIR)/ppc-jit.cpp
OBJS     = rom-harness.o ppc-jit.o

.PHONY: all clean run test quick

all: rom-harness

rom-harness.o: rom-harness.cpp $(JITDIR)/ppc-jit.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ rom-harness.cpp

ppc-jit.o: $(JITDIR)/ppc-jit.cpp $(JITDIR)/ppc-jit.h $(JITDIR)/ppc-codegen-aarch64.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -DHAVE_CONFIG_H -c -o $@ $<

rom-harness: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

run: rom-harness
	./rom-harness $(ROM) --verbose --count=100

test: rom-harness
	./rom-harness $(ROM) --count=0 --min-insns=1 --max-insns=64

quick: rom-harness
	./rom-harness $(ROM) --count=500 --min-insns=2 --max-insns=16 --verbose --stop-on-fail

clean:
	rm -f rom-harness $(OBJS)
