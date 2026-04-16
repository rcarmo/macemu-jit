/*
 * compiler/compemu_support.cpp - Core dynamic translation engine
 *
 * Copyright (c) 2001-2009 Milan Jurik of ARAnyM dev team (see AUTHORS)
 * 
 * Inspired by Christian Bauer's Basilisk II
 *
 * This file is part of the ARAnyM project which builds a new and powerful
 * TOS/FreeMiNT compatible virtual machine running on almost any hardware.
 *
 * JIT compiler m68k -> ARM
 *
 * Original 68040 JIT compiler for UAE, copyright 2000-2002 Bernd Meyer
 * Adaptation for Basilisk II and improvements, copyright 2000-2004 Gwenole Beauchesne
 * Portions related to CPU detection come from linux/arch/i386/kernel/setup.c
 *
 * ARAnyM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * ARAnyM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ARAnyM; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "sysdeps.h"

#include <math.h>
#if defined(CPU_AARCH64)
#include <sys/mman.h>
#endif

#ifdef JIT_DEBUG_MEM_CORRUPTION
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <errno.h>
#include <dlfcn.h>
#endif

#include "sysdeps.h"

#if defined(JIT)

#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "vm_alloc.h"
#include "m68k.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"
#include "fpu/core.h"
#include "fpu/fpu.h"
#include "fpu/flags.h"
#include "comptbl_arm.h"
#include "compemu_arm.h"
#include <SDL2/SDL.h>

/* ARM64 JIT is PIE-compatible: it uses register-indirect addressing
 * (R_MEMSTART/R15) rather than PC-relative globals, so code placement
 * relative to .data does not matter. */

#ifdef __MACH__
// Needed for sys_cache_invalidate on the JIT space region, Mac OS X specific
#include <libkern/OSCacheControl.h>
#endif

static inline void* vm_acquire_code(uae_u32 size, int options = VM_MAP_DEFAULT)
{
	void *code = vm_acquire(size, options);
	if (code != VM_MAP_FAILED)
		vm_protect(code, size, VM_PAGE_READ | VM_PAGE_WRITE | VM_PAGE_EXECUTE);
	return code;
}

#if defined(CPU_AARCH64)
#define PRINT_PTR "%016llx"
#else
#define PRINT_PTR "%08x"
#endif

#define jit_log(format, ...) \
  write_log("JIT: " format "\n", ##__VA_ARGS__);
#define jit_log2(format, ...)

#ifndef NATMEM_OFFSET
#define NATMEM_OFFSET MEMBaseDiff
#endif
#ifdef NATMEM_OFFSET
#define FIXED_ADDRESSING 1
#endif
#ifndef uae_bswap_16
#define uae_bswap_16 do_byteswap_16
#endif
#ifndef uae_bswap_32
#define uae_bswap_32 do_byteswap_32
#endif
#ifndef xfree
#define xfree free
#endif

#include "../compemu_prefs.cpp"

/* BUG 13 fix: emulated_ticks is uint16 but the JIT's endblock code uses
   32-bit LDR/STR and checks bit 31 for sign. With a uint16, the high 16
   bits are garbage from adjacent memory, making the countdown sign check
   unpredictable. Use a dedicated int32 variable instead. */
int32 jit_countdown = 10000000;
#define countdown jit_countdown

#if defined(CPU_AARCH64)
#include <signal.h>
#include <ucontext.h>
static void jit_sigsegv_diag(int sig, siginfo_t *si, void *ctx_raw) {
    ucontext_t *uc = (ucontext_t *)ctx_raw;
    uintptr_t pc = uc->uc_mcontext.pc;
    uintptr_t fault_addr = (uintptr_t)si->si_addr;
    fprintf(stderr, "\n=== JIT SIGSEGV DIAG ===\n");
    fprintf(stderr, "sig=%d fault_addr=0x%lx PC=0x%lx\n", sig, fault_addr, pc);
    for (int i = 0; i < 31; i++) {
        fprintf(stderr, "x%02d=0x%016lx ", i, (unsigned long)uc->uc_mcontext.regs[i]);
        if ((i % 4) == 3) fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");
    if (pc) {
        fprintf(stderr, "Instructions around crash PC:\n");
        for (int i = -4; i <= 4; i++) {
            uae_u32 *insn = (uae_u32*)(pc + i*4);
            fprintf(stderr, "  %s 0x%lx: %08x\n", i==0 ? ">>>" : "   ", (uintptr_t)insn, *insn);
        }
    }
    fprintf(stderr, "regs.pc_p=0x%lx regs.pc=0x%x\n", (unsigned long)regs.pc_p, regs.pc);
    fprintf(stderr, "=== END DIAG ===\n");
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void jit_install_diag_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = jit_sigsegv_diag;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}
#endif

enum {
	S_READ = 1,
	S_WRITE = 2,
	S_N_ADDR = 4,
};

int special_mem = 0;
int special_mem_default = 0;
int jit_n_addr_unsafe = 0;
static uae_u8 *baseaddr[65536] = { 0 };
static void *mem_banks[65536] = { 0 };

static inline bool jit_force_special_fb_writes(void)
{
	static int enabled = -1;
	if (enabled < 0)
		enabled = (getenv("B2_JIT_FB_SPECIAL_WRITES") && *getenv("B2_JIT_FB_SPECIAL_WRITES")) ? 1 : 0;
	return enabled != 0;
}

static inline bool jit_force_all_special_mem(void)
{
	static int enabled = -1;
	if (enabled < 0)
		enabled = (getenv("B2_JIT_ALL_SPECIAL_MEM") && *getenv("B2_JIT_ALL_SPECIAL_MEM")) ? 1 : 0;
	return enabled != 0;
}

static inline bool jit_force_optlev0(void)
{
	static int enabled = -1;
	if (enabled < 0)
		enabled = (getenv("B2_JIT_FORCE_OPTLEV0") && *getenv("B2_JIT_FORCE_OPTLEV0")) ? 1 : 0;
	return enabled != 0;
}

static inline bool jit_force_optlev0_block_env(uae_u32 pc)
{
	static int initialized = 0;
	static int range_count = 0;
	static struct {
		uae_u32 start;
		uae_u32 end;
	} ranges[64];
	if (!initialized) {
		const char *env = getenv("B2_JIT_FORCE_OPTLEV0_PCS");
		initialized = 1;
		if (env && *env) {
			const char *p = env;
			while (*p && range_count < (int)(sizeof(ranges) / sizeof(ranges[0]))) {
				while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')
					p++;
				if (!*p)
					break;
				char *endp = NULL;
				unsigned long start = strtoul(p, &endp, 0);
				unsigned long end = start;
				if (endp == p)
					break;
				p = endp;
				if (*p == '-') {
					p++;
					end = strtoul(p, &endp, 0);
					if (endp == p)
						end = start;
					p = endp;
				}
				ranges[range_count].start = (uae_u32)start;
				ranges[range_count].end = (uae_u32)end;
				range_count++;
				while (*p && *p != ',')
					p++;
			}
		}
	}
	for (int i = 0; i < range_count; i++) {
		if (pc >= ranges[i].start && pc <= ranges[i].end)
			return true;
	}
	return false;
}

static inline int jit_max_optlev(void)
{
	static int value = -1;
	if (value < 0) {
		const char *env = getenv("B2_JIT_MAX_OPTLEV");
		value = env && *env ? atoi(env) : 2; /* Default to L2 on ARM64 */
		if (value < 0)
			value = 0;
		if (value > 2)
			value = 2;
		fprintf(stderr, "JIT: max_optlev=%d (env=%s)\n", value, env ? env : "(not set)");
	}
	return value;
}

static inline bool jit_force_optlev0_block_exact(uae_u32 pc)
{
#if defined(CPU_AARCH64)
	/* Low ROM 04000000-0400ffff: $dd0 I/O polling, timer init, and early
	   boot sequences that read unmapped hardware registers. */
	if (pc >= 0x04000000 && pc <= 0x0400ffff)
		return true;
	/* NuBus slot init 040b0000-040bffff: reads unmapped NuBus hardware
	   registers at 0x50Fxxxxx. ROM patches cover the video probe (0xb27c)
	   and one slot probe (0xba0b0) but dozens of other polling points remain. */
	if (pc >= 0x040b0000 && pc <= 0x040bffff)
		return true;
#endif
	(void)pc;
	return false;
}

static inline bool jit_force_optlev1_block(uae_u32 pc)
{
	static int initialized = 0;
	static int range_count = 0;
	static struct {
		uae_u32 start;
		uae_u32 end;
	} ranges[64];
	if (!initialized) {
		const char *env = getenv("B2_JIT_FORCE_OPTLEV1_PCS");
		initialized = 1;
		if (env && *env) {
			const char *p = env;
			while (*p && range_count < (int)(sizeof(ranges) / sizeof(ranges[0]))) {
				while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')
					p++;
				if (!*p)
					break;
				char *endp = NULL;
				unsigned long start = strtoul(p, &endp, 0);
				unsigned long end = start;
				if (endp == p)
					break;
				p = endp;
				if (*p == '-') {
					p++;
					end = strtoul(p, &endp, 0);
					if (endp == p)
						end = start;
					p = endp;
				}
				ranges[range_count].start = (uae_u32)start;
				ranges[range_count].end = (uae_u32)end;
				range_count++;
				while (*p && *p != ',')
					p++;
			}
		}
	}
	for (int i = 0; i < range_count; i++) {
		if (pc >= ranges[i].start && pc <= ranges[i].end)
			return true;
	}
	return false;
}

static inline bool jit_force_optlev1_env_opcode(uae_u16 op)
{
	static int initialized = 0;
	static int opcode_count = 0;
	static uae_u16 opcodes[128];
	if (!initialized) {
		const char *env = getenv("B2_JIT_FORCE_OPTLEV1_OPS");
		initialized = 1;
		if (env && *env) {
			const char *p = env;
			while (*p && opcode_count < (int)(sizeof(opcodes) / sizeof(opcodes[0]))) {
				while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')
					p++;
				if (!*p)
					break;
				char *endp = NULL;
				unsigned long value = strtoul(p, &endp, 16);
				if (endp == p)
					break;
				opcodes[opcode_count++] = (uae_u16)value;
				p = endp;
				while (*p && *p != ',')
					p++;
			}
		}
	}
	for (int i = 0; i < opcode_count; i++) {
		if (op == opcodes[i])
			return true;
	}
	return false;
}

static inline bool trace_flagflow_env(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		const char *env = getenv("B2_TRACE_FLAGFLOW");
		enabled = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
		if (enabled) {
			const char *l2_only = getenv("B2_JIT_L2_ONLY");
			const char *disable = getenv("B2_JIT_DISABLE_HARDCODED_OPTLEV1");
			int l2 = ((l2_only && *l2_only && strcmp(l2_only, "0") != 0) ||
				(disable && *disable && strcmp(disable, "0") != 0)) ? 1 : 0;
			fprintf(stderr, "FLAGFLOW_CONFIG enabled=1 l2only=%d\n", l2);
		}
	}
	return enabled != 0;
}

static inline unsigned long trace_flagflow_limit(void)
{
	static unsigned long value = 0;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_FLAGFLOW_LIMIT");
		value = env && *env ? strtoul(env, NULL, 0) : 2000;
		init = true;
	}
	return value;
}

static inline bool trace_flagflow_opcode(uae_u16 op)
{
	return op == 0x7000 || op == 0x7001 || op == 0x7128 || op == 0x7129 || op == 0x7130 ||
		op == 0x7104 || op == 0x7111 || op == 0x7123 || op == 0x712c ||
		op == 0x4a80 || op == 0x4290 || op == 0x4aa9 || op == 0x4278 || op == 0x4ab8 ||
		(op & 0xf000) == 0x6000;
}

static inline bool trace_propbuild_env(void)
{
	static int enabled = -1;
	if (enabled < 0)
		enabled = (getenv("B2_TRACE_PROPBUILD") && *getenv("B2_TRACE_PROPBUILD") && strcmp(getenv("B2_TRACE_PROPBUILD"), "0") != 0) ? 1 : 0;
	return enabled != 0;
}

static inline bool trace_propbuild_opcode(uae_u16 op)
{
	return op == 0x7104 || op == 0x7000 || op == 0x6704 || op == 0x4a80 || op == 0x4278 ||
		op == 0x0471 || op == 0x0070 || op == 0x0467 || op == 0x804a || op == 0x7842;
}

static inline bool trace_emulopflow_env(void)
{
	static int enabled = -1;
	if (enabled < 0)
		enabled = (getenv("B2_TRACE_EMULOPFLOW") && *getenv("B2_TRACE_EMULOPFLOW") && strcmp(getenv("B2_TRACE_EMULOPFLOW"), "0") != 0) ? 1 : 0;
	return enabled != 0;
}

static inline unsigned long trace_emulopflow_limit(void)
{
	static unsigned long value = 0;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_EMULOPFLOW_LIMIT");
		value = env && *env ? strtoul(env, NULL, 0) : 2000;
		init = true;
	}
	return value;
}

static unsigned long trace_emulopflow_count = 0;

static inline bool trace_emulopflow_opcode(uae_u16 op)
{
	return (op & 0xff00) == 0x7100;
}

static inline bool trace_emuneigh_env()
{
	static int enabled = -1;
	if (enabled < 0)
		enabled = (getenv("B2_TRACE_EMUNEIGH") && *getenv("B2_TRACE_EMUNEIGH") && strcmp(getenv("B2_TRACE_EMUNEIGH"), "0") != 0) ? 1 : 0;
	return enabled != 0;
}

static inline unsigned long trace_emuneigh_limit()
{
	static unsigned long value = 0;
	static bool init = false;
	if (!init) {
		const char *env = getenv("B2_TRACE_EMUNEIGH_LIMIT");
		value = env && *env ? strtoul(env, NULL, 0) : 4000;
		init = true;
	}
	return value;
}

static unsigned long trace_emuneigh_count = 0;

static inline bool trace_emuneigh_target(uae_u32 pc)
{
	/* When B2_TRACE_REGCHECK=1, trace ALL block entries for register
	   state comparison between L1 and L2 runs. */
	static int regcheck = -1;
	if (regcheck < 0) {
		const char *env = getenv("B2_TRACE_REGCHECK");
		regcheck = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
	}
	if (regcheck) return true;
	return (pc >= 0x0400a3dc && pc <= 0x0400a3f2) ||
		(pc >= 0x040b3566 && pc <= 0x040b35c8);
}

static void trace_emuneigh_entry(uae_u32 block_pc, uae_u32 first_op)
{
	if (!trace_emuneigh_env() || trace_emuneigh_count >= trace_emuneigh_limit())
		return;
	MakeSR();
	fprintf(stderr,
		"EMUNEIGH %lu ENTRY block=%08x first=%04x pc=%08x sr=%04x spc=%08x d0=%08x d1=%08x d2=%08x a0=%08x a1=%08x a7=%08x lm160=%02x lm162=%08x ticks=%08x irq=%08x\n",
		++trace_emuneigh_count,
		(unsigned)block_pc,
		(unsigned)first_op,
		(unsigned)m68k_getpc(),
		(unsigned)regs.sr,
		(unsigned)regs.spcflags,
		(unsigned)m68k_dreg(regs,0),
		(unsigned)m68k_dreg(regs,1),
		(unsigned)m68k_dreg(regs,2),
		(unsigned)m68k_areg(regs,0),
		(unsigned)m68k_areg(regs,1),
		(unsigned)m68k_areg(regs,7),
		(unsigned)ReadMacInt8(0x160),
		(unsigned)ReadMacInt32(0x162),
		(unsigned)ReadMacInt32(0x16a),
		(unsigned)InterruptFlags);
}

static void trace_emulop_resume(uae_u32 opcode, uae_u32 next_pc)
{
	if (!trace_emulopflow_env() || trace_emulopflow_count >= trace_emulopflow_limit())
		return;
	MakeSR();
	fprintf(stderr,
		"EMUFLOW %lu RESUME op=%04x next=%08x pc=%08x sr=%04x spc=%08x quit=%d d0=%08x d1=%08x a0=%08x a1=%08x a7=%08x\n",
		++trace_emulopflow_count,
		(unsigned)opcode,
		(unsigned)next_pc,
		(unsigned)m68k_getpc(),
		(unsigned)regs.sr,
		(unsigned)regs.spcflags,
		quit_program,
		(unsigned)m68k_dreg(regs, 0),
		(unsigned)m68k_dreg(regs, 1),
		(unsigned)m68k_areg(regs, 0),
		(unsigned)m68k_areg(regs, 1),
		(unsigned)m68k_areg(regs, 7));
}

static inline bool jit_pc_in_env_ranges(const char *env_name, uae_u32 pc)
{
	struct range_pair { uae_u32 start; uae_u32 end; };
	struct cache_entry {
		const char *name;
		int initialized;
		int range_count;
		range_pair ranges[64];
	};
	static cache_entry caches[3] = {
		{"B2_JIT_VERIFY_PCS", 0, 0, {}},
		{"B2_JIT_FLUSH_OP_PCS", 0, 0, {}},
		{"B2_JIT_TRACE_PCS", 0, 0, {}},
	};
	cache_entry *cache = NULL;
	for (size_t ci = 0; ci < sizeof(caches) / sizeof(caches[0]); ci++) {
		if (strcmp(caches[ci].name, env_name) == 0) {
			cache = &caches[ci];
			break;
		}
	}
	if (!cache)
		return false;
	if (!cache->initialized) {
		const char *env = getenv(env_name);
		cache->initialized = 1;
		if (env && *env) {
			const char *p = env;
			while (*p && cache->range_count < (int)(sizeof(cache->ranges) / sizeof(cache->ranges[0]))) {
				while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')
					p++;
				if (!*p)
					break;
				char *endp = NULL;
				unsigned long start = strtoul(p, &endp, 0);
				unsigned long end = start;
				if (endp == p)
					break;
				p = endp;
				if (*p == '-') {
					p++;
					end = strtoul(p, &endp, 0);
					if (endp == p)
						end = start;
					p = endp;
				}
				cache->ranges[cache->range_count].start = (uae_u32)start;
				cache->ranges[cache->range_count].end = (uae_u32)end;
				cache->range_count++;
			}
		}
	}
	for (int i = 0; i < cache->range_count; i++) {
		if (pc >= cache->ranges[i].start && pc <= cache->ranges[i].end)
			return true;
	}
	return false;
}

static inline bool jit_verify_target_pc(uae_u32 pc)
{
	return jit_pc_in_env_ranges("B2_JIT_VERIFY_PCS", pc);
}

static inline bool jit_flush_target_pc(uae_u32 pc)
{
	return jit_pc_in_env_ranges("B2_JIT_FLUSH_OP_PCS", pc);
}

static inline bool jit_trace_target_pc(uae_u32 pc)
{
	return jit_pc_in_env_ranges("B2_JIT_TRACE_PCS", pc);
}

struct jit_verify_snapshot {
	regstruct regs;
	flag_struct flags;
	uae_u32 mem_a2_addr;
	uae_u32 mem_a2_p400_addr;
	uae_u8 mem_a2_byte;
	uae_u8 mem_a2_p400_byte;
};

static jit_verify_snapshot jit_verify_pre_state;
static bool jit_verify_pre_valid = false;
static unsigned long jit_verify_log_count = 0;

struct jit_block_verify_snapshot {
    regstruct regs;
    flag_struct flags;
    uae_s32 countdown;
    uae_u32 interrupt_flags;
    uae_u8 *mem;
    size_t mem_size;
};

static bool jit_block_verify_reentrant = false;
static bool jit_block_verify_compile_active = false;
static uae_u32 jit_block_verify_compile_pc = 0xffffffffu;
static unsigned long jit_block_verify_log_count = 0;
static unsigned long jit_block_verify_run_count = 0;
static jit_block_verify_snapshot jit_block_verify_entry_state = {};
static bool jit_block_verify_entry_valid = false;
static uae_u32 jit_block_verify_entry_pc = 0xffffffffu;

typedef void (*jit_compiled_handler)(void);
static inline unsigned int get_opcode_cft_map(unsigned int f);

static void jit_block_verify_snapshot_free(jit_block_verify_snapshot *snap)
{
    if (snap->mem) {
        free(snap->mem);
        snap->mem = NULL;
    }
    snap->mem_size = 0;
}

static bool jit_block_verify_snapshot_capture(jit_block_verify_snapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->mem_size = (size_t)RAMSize + (size_t)ROMSize;
    snap->mem = (uae_u8*)malloc(snap->mem_size);
    if (!snap->mem)
        return false;
    memcpy(&snap->regs, &regs, sizeof(regs));
    memcpy(&snap->flags, &regflags, sizeof(regflags));
    snap->countdown = countdown;
    snap->interrupt_flags = InterruptFlags;
    memcpy(snap->mem, RAMBaseHost, snap->mem_size);
    return true;
}

static void jit_block_verify_snapshot_restore(const jit_block_verify_snapshot *snap)
{
    memcpy(&regs, &snap->regs, sizeof(regs));
    memcpy(&regflags, &snap->flags, sizeof(regflags));
    countdown = snap->countdown;
    InterruptFlags = snap->interrupt_flags;
    memcpy(RAMBaseHost, snap->mem, snap->mem_size);
}

static void jit_block_verify_entry_reset(void)
{
    if (jit_block_verify_entry_valid)
        jit_block_verify_snapshot_free(&jit_block_verify_entry_state);
    memset(&jit_block_verify_entry_state, 0, sizeof(jit_block_verify_entry_state));
    jit_block_verify_entry_valid = false;
    jit_block_verify_entry_pc = 0xffffffffu;
}

static void jit_block_verify_entry_capture(uae_u32 block_pc)
{
    jit_block_verify_entry_reset();
    if (!jit_block_verify_snapshot_capture(&jit_block_verify_entry_state))
        return;
    jit_block_verify_entry_valid = true;
    jit_block_verify_entry_pc = block_pc;
}

static void jit_block_verify_compare(const jit_block_verify_snapshot *expected, const jit_block_verify_snapshot *actual, uae_u32 block_pc, int blocklen)
{
    bool mismatch = false;
    if (memcmp(&expected->regs, &actual->regs, sizeof(regs)) != 0)
        mismatch = true;
    if (memcmp(&expected->flags, &actual->flags, sizeof(regflags)) != 0)
        mismatch = true;
    if (!mismatch && memcmp(expected->mem, actual->mem, expected->mem_size) != 0)
        mismatch = true;
    if (!mismatch) {
        if (jit_block_verify_run_count <= 20)
            fprintf(stderr, "JITBLOCKVERIFY block=%08x len=%d mismatch=0\n", (unsigned)block_pc, blocklen);
        return;
    }
    if (jit_block_verify_log_count >= 20)
        return;

    fprintf(stderr, "JITBLOCKVERIFY block=%08x len=%d mismatch=1\n", (unsigned)block_pc, blocklen);
    for (int i = 0; i < 16; i++) {
        if (expected->regs.regs[i] != actual->regs.regs[i]) {
            fprintf(stderr, "  reg[%d] interp=%08x native=%08x\n", i,
                (unsigned)expected->regs.regs[i], (unsigned)actual->regs.regs[i]);
        }
    }
    if (expected->regs.pc != actual->regs.pc || expected->regs.fault_pc != actual->regs.fault_pc ||
        expected->regs.pc_p != actual->regs.pc_p || expected->regs.pc_oldp != actual->regs.pc_oldp ||
        expected->regs.sr != actual->regs.sr || expected->regs.spcflags != actual->regs.spcflags) {
        fprintf(stderr,
            "  pc interp=%08x/%p/%p native=%08x/%p/%p sr interp=%04x native=%04x spc interp=%08x native=%08x\n",
            (unsigned)expected->regs.pc, (void*)expected->regs.pc_p, (void*)expected->regs.pc_oldp,
            (unsigned)actual->regs.pc, (void*)actual->regs.pc_p, (void*)actual->regs.pc_oldp,
            (unsigned)expected->regs.sr, (unsigned)actual->regs.sr,
            (unsigned)expected->regs.spcflags, (unsigned)actual->regs.spcflags);
    }
    if (expected->flags.nzcv != actual->flags.nzcv || expected->flags.x != actual->flags.x) {
        fprintf(stderr, "  flags interp nzcv=%08x x=%08x native nzcv=%08x x=%08x\n",
            (unsigned)expected->flags.nzcv, (unsigned)expected->flags.x,
            (unsigned)actual->flags.nzcv, (unsigned)actual->flags.x);
    }
    int memdiffs = 0;
    for (size_t i = 0; i < expected->mem_size && memdiffs < 16; i++) {
        if (expected->mem[i] != actual->mem[i]) {
            fprintf(stderr, "  mem[%08zx] interp=%02x native=%02x\n", i,
                (unsigned)expected->mem[i], (unsigned)actual->mem[i]);
            memdiffs++;
        }
    }
    jit_block_verify_log_count++;
}

static void jit_block_verify_run(cpu_history *pc_hist, int blocklen, int total_cycles, uae_u32 block_pc)
{
    jit_block_verify_snapshot resume = {}, interp = {}, native = {};
    jit_block_verify_run_count++;
    if (!jit_block_verify_entry_valid || jit_block_verify_entry_pc != block_pc)
        return;
    if (!jit_block_verify_snapshot_capture(&resume)) {
        jit_block_verify_entry_reset();
        return;
    }

#if defined(CPU_AARCH64)
    extern bool tick_inhibit;
    const bool saved_tick_inhibit = tick_inhibit;
    tick_inhibit = true;
#endif

    /* Re-run the exact interpreter block from the true entry state so the
       verifier compares whole-block semantics, not a post-trace side state. */
    jit_block_verify_snapshot_restore(&jit_block_verify_entry_state);
    regs.spcflags = 0;
    InterruptFlags = 0;
    for (int i = 0; i < blocklen; i++) {
        uae_u32 opcode = get_opcode_cft_map((uae_u16)*pc_hist[i].location);
        (*cpufunctbl[opcode])(opcode);
    }
    if (!jit_block_verify_snapshot_capture(&interp)) {
#if defined(CPU_AARCH64)
        tick_inhibit = saved_tick_inhibit;
#endif
        jit_block_verify_snapshot_restore(&resume);
        jit_block_verify_snapshot_free(&resume);
        jit_block_verify_entry_reset();
        return;
    }

    jit_block_verify_snapshot_restore(&jit_block_verify_entry_state);
    regs.spcflags = 0;
    InterruptFlags = 0;
    jit_block_verify_compile_active = true;
    jit_block_verify_compile_pc = block_pc;
    compile_block(pc_hist, blocklen, total_cycles);
    jit_block_verify_compile_active = false;
    jit_block_verify_compile_pc = 0xffffffffu;

    countdown = -1;
    jit_block_verify_reentrant = true;
    ((jit_compiled_handler)pushall_call_handler)();
    jit_block_verify_reentrant = false;

    if (jit_block_verify_snapshot_capture(&native)) {
        jit_block_verify_compare(&interp, &native, block_pc, blocklen);
        jit_block_verify_snapshot_free(&native);
    }

#if defined(CPU_AARCH64)
    tick_inhibit = saved_tick_inhibit;
#endif
    /* Continue execution from the interpreter-established state. */
    jit_block_verify_snapshot_restore(&resume);
    jit_block_verify_snapshot_free(&resume);
    jit_block_verify_snapshot_free(&interp);
    jit_block_verify_entry_reset();
}

struct jit_flush_delta_snapshot {
	uae_u32 regs[16];
	uae_u32 flagx;
	uae_u32 nzcv;
	uae_u32 spcflags;
	uae_u32 interrupt_flags;
	uae_s32 countdown;
};

static jit_flush_delta_snapshot jit_flush_delta_state;
static unsigned long jit_flush_delta_log_count = 0;

static void jit_verify_pre(uae_u32 pc, uae_u32 opcode)
{
	(void)pc;
	(void)opcode;
	memcpy(&jit_verify_pre_state.regs, &regs, sizeof(regs));
	memcpy(&jit_verify_pre_state.flags, &regflags, sizeof(regflags));
	jit_verify_pre_state.mem_a2_addr = regs.regs[10];
	jit_verify_pre_state.mem_a2_p400_addr = regs.regs[10] + 0x400;
	jit_verify_pre_state.mem_a2_byte = get_byte(jit_verify_pre_state.mem_a2_addr);
	jit_verify_pre_state.mem_a2_p400_byte = get_byte(jit_verify_pre_state.mem_a2_p400_addr);
	jit_verify_pre_valid = true;
}

static void jit_verify_post(uae_u32 pc, uae_u32 opcode)
{
	if (!jit_verify_pre_valid)
		return;
	jit_verify_snapshot compiled_post;
	memcpy(&compiled_post.regs, &regs, sizeof(regs));
	memcpy(&compiled_post.flags, &regflags, sizeof(regflags));
	compiled_post.mem_a2_addr = jit_verify_pre_state.mem_a2_addr;
	compiled_post.mem_a2_p400_addr = jit_verify_pre_state.mem_a2_p400_addr;
	compiled_post.mem_a2_byte = get_byte(compiled_post.mem_a2_addr);
	compiled_post.mem_a2_p400_byte = get_byte(compiled_post.mem_a2_p400_addr);

	memcpy(&regs, &jit_verify_pre_state.regs, sizeof(regs));
	memcpy(&regflags, &jit_verify_pre_state.flags, sizeof(regflags));
	put_byte(jit_verify_pre_state.mem_a2_addr, jit_verify_pre_state.mem_a2_byte);
	put_byte(jit_verify_pre_state.mem_a2_p400_addr, jit_verify_pre_state.mem_a2_p400_byte);
	(*cpufunctbl[opcode])(opcode);

	const uae_u8 interp_mem_a2 = get_byte(jit_verify_pre_state.mem_a2_addr);
	const uae_u8 interp_mem_a2_p400 = get_byte(jit_verify_pre_state.mem_a2_p400_addr);
	bool mismatch = false;
	for (int ri = 0; ri < 16; ri++) {
		if (regs.regs[ri] != compiled_post.regs.regs[ri]) {
			mismatch = true;
			break;
		}
	}
	if (regflags.nzcv != compiled_post.flags.nzcv ||
		regflags.x != compiled_post.flags.x ||
		interp_mem_a2 != compiled_post.mem_a2_byte ||
		interp_mem_a2_p400 != compiled_post.mem_a2_p400_byte)
		mismatch = true;

	if (mismatch && jit_verify_log_count < 400) {
		fprintf(stderr,
			"JITVERIFY pc=%08x op=%04x interp:d0=%08x d1=%08x d2=%08x d3=%08x a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x nzcv=%08x x=%08x m[a2]=%02x m[a2+400]=%02x compiled:d0=%08x d1=%08x d2=%08x d3=%08x a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x nzcv=%08x x=%08x m[a2]=%02x m[a2+400]=%02x\n",
			(unsigned)pc, (unsigned)opcode,
			(unsigned)regs.regs[0], (unsigned)regs.regs[1], (unsigned)regs.regs[2], (unsigned)regs.regs[3],
			(unsigned)regs.regs[8], (unsigned)regs.regs[9], (unsigned)regs.regs[10], (unsigned)regs.regs[11],
			(unsigned)regs.regs[12], (unsigned)regs.regs[13], (unsigned)regs.regs[14], (unsigned)regs.regs[15],
			regflags.nzcv, regflags.x,
			(unsigned)interp_mem_a2, (unsigned)interp_mem_a2_p400,
			(unsigned)compiled_post.regs.regs[0], (unsigned)compiled_post.regs.regs[1],
			(unsigned)compiled_post.regs.regs[2], (unsigned)compiled_post.regs.regs[3],
			(unsigned)compiled_post.regs.regs[8], (unsigned)compiled_post.regs.regs[9],
			(unsigned)compiled_post.regs.regs[10], (unsigned)compiled_post.regs.regs[11],
			(unsigned)compiled_post.regs.regs[12], (unsigned)compiled_post.regs.regs[13],
			(unsigned)compiled_post.regs.regs[14], (unsigned)compiled_post.regs.regs[15],
			compiled_post.flags.nzcv, compiled_post.flags.x,
			(unsigned)compiled_post.mem_a2_byte, (unsigned)compiled_post.mem_a2_p400_byte);
		jit_verify_log_count++;
	}

	memcpy(&regs, &compiled_post.regs, sizeof(regs));
	memcpy(&regflags, &compiled_post.flags, sizeof(regflags));
	put_byte(compiled_post.mem_a2_addr, compiled_post.mem_a2_byte);
	put_byte(compiled_post.mem_a2_p400_addr, compiled_post.mem_a2_p400_byte);
	jit_verify_pre_valid = false;
}

static void jit_flush_delta_compare(uae_u32 pc)
{
	if (jit_flush_delta_log_count >= 200)
		return;
	bool mismatch = false;
	for (int i = 0; i < 16; i++) {
		if (jit_flush_delta_state.regs[i] != regs.regs[i]) {
			mismatch = true;
			break;
		}
	}
	if (jit_flush_delta_state.flagx != regflags.x ||
		jit_flush_delta_state.nzcv != regflags.nzcv ||
		jit_flush_delta_state.spcflags != regs.spcflags ||
		jit_flush_delta_state.interrupt_flags != InterruptFlags ||
		jit_flush_delta_state.countdown != countdown)
		mismatch = true;
	fprintf(stderr,
		"JITFLUSHDELTA %lu pc=%08x mismatch=%d pre:d0=%08x d1=%08x d2=%08x d3=%08x a0=%08x a1=%08x a2=%08x a3=%08x a7=%08x x=%08x nzcv=%08x spc=%08x irq=%08x cd=%08x "
		"post:d0=%08x d1=%08x d2=%08x d3=%08x a0=%08x a1=%08x a2=%08x a3=%08x a7=%08x x=%08x nzcv=%08x spc=%08x irq=%08x cd=%08x mem[a2]=%02x mem[a2+400]=%02x lm160=%02x lm162=%08x lm16a=%08x\n",
		++jit_flush_delta_log_count,
		(unsigned)pc,
		mismatch ? 1 : 0,
		jit_flush_delta_state.regs[0], jit_flush_delta_state.regs[1], jit_flush_delta_state.regs[2], jit_flush_delta_state.regs[3],
		jit_flush_delta_state.regs[8], jit_flush_delta_state.regs[9], jit_flush_delta_state.regs[10], jit_flush_delta_state.regs[11], jit_flush_delta_state.regs[15],
		jit_flush_delta_state.flagx, jit_flush_delta_state.nzcv, jit_flush_delta_state.spcflags, jit_flush_delta_state.interrupt_flags, (uae_u32)jit_flush_delta_state.countdown,
		regs.regs[0], regs.regs[1], regs.regs[2], regs.regs[3],
		regs.regs[8], regs.regs[9], regs.regs[10], regs.regs[11], regs.regs[15],
		regflags.x, regflags.nzcv, regs.spcflags, InterruptFlags, (uae_u32)countdown,
		(unsigned)get_byte(regs.regs[10]),
		(unsigned)get_byte(regs.regs[10] + 0x400),
		(unsigned)ReadMacInt8(0x160),
		(unsigned)ReadMacInt32(0x162),
		(unsigned)ReadMacInt32(0x16a));
}

static void jit_emit_flush_delta_snapshot(void);

static unsigned long trace_flagflow_count = 0;
static uae_u32 trace_flagflow_block_pc = 0;
static uae_u32 trace_flagflow_pc = 0;
static uae_u16 trace_flagflow_op = 0;

static inline void trace_flagflow_log(const char *tag, uae_u32 a = 0, uae_u32 b = 0, uae_u32 c = 0, uae_u32 d = 0)
{
	if (!trace_flagflow_env() || trace_flagflow_count >= trace_flagflow_limit())
		return;
	fprintf(stderr,
		"FLAGFLOW %lu %s block=%08x pc=%08x op=%04x a=%08x b=%08x c=%08x d=%08x\n",
		++trace_flagflow_count,
		tag,
		(unsigned)trace_flagflow_block_pc,
		(unsigned)trace_flagflow_pc,
		(unsigned)trace_flagflow_op,
		(unsigned)a,
		(unsigned)b,
		(unsigned)c,
		(unsigned)d);
}

static inline void trace_flagflow_log_opmeta(uae_u16 opcode, uae_u32 next_live, uae_u16 next_op)
{
	if (!trace_flagflow_env() || trace_flagflow_count >= trace_flagflow_limit())
		return;
	unsigned int mapped = uae_bswap_16(opcode);
	fprintf(stderr,
		"FLAGFLOW %lu OPMETA block=%08x pc=%08x op=%04x mapped=%04x cur_use=%02x cur_set=%02x map_use=%02x map_set=%02x next_live=%08x next_op=%04x\n",
		++trace_flagflow_count,
		(unsigned)trace_flagflow_block_pc,
		(unsigned)trace_flagflow_pc,
		(unsigned)opcode,
		(unsigned)mapped,
		(unsigned)prop[opcode].use_flags,
		(unsigned)prop[opcode].set_flags,
		(unsigned)prop[mapped].use_flags,
		(unsigned)prop[mapped].set_flags,
		(unsigned)next_live,
		(unsigned)next_op);
}

static inline bool jit_disable_hardcoded_optlev1(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		const char *l2_only = getenv("B2_JIT_L2_ONLY");
		const char *disable = getenv("B2_JIT_DISABLE_HARDCODED_OPTLEV1");
		enabled = ((l2_only && *l2_only && strcmp(l2_only, "0") != 0) ||
			(disable && *disable && strcmp(disable, "0") != 0)) ? 1 : 0;
	}
	return enabled != 0;
}

static void flush(int save_regs);

static void op_fullsr_orsr_w_comp_ff(uae_u32 opcode);
static void op_fullsr_andsr_w_comp_ff(uae_u32 opcode);
static void op_fullsr_eorsr_w_comp_ff(uae_u32 opcode);
static void op_fullsr_mv2sr_w_comp_ff(uae_u32 opcode);
static void op_move_l_d8anxn_absw_comp_ff(uae_u32 opcode);
static void op_move_l_reg_d16an_comp_ff(uae_u32 opcode);
extern "C" void jit_trace_add(uae_u32 pc, uae_u32 opcode);
extern "C" void jit_trace_pc_hit(uae_u32 pc, uae_u32 tagged_opcode);
static void op_movea_l_postinc_an_comp_ff(uae_u32 opcode);
static void op_aline_trap_comp_ff(uae_u32 opcode);
static void op_emulop_comp_ff(uae_u32 opcode);
static inline void jit_emit_runtime_helper_barrier(uintptr helper, uintptr pc, uae_u32 arg1, uae_u32 arg2, bool has_arg2);

static inline bool jit_force_optlev1_opcode(uae_u16 op)
{
	if (jit_force_optlev1_env_opcode(op))
		return true;
	/* All structural and semantic gates moved to
	   jit_force_interpreter_barrier_opcode() as per-instruction barriers.
	   Blocks stay at optlev=2; only the specific control-flow instruction
	   falls back to interpreter and ends the block. */
	return false;
}

static inline bool jit_env_has_csv_token(const char *env_name, const char *token)
{
	const char *env = getenv(env_name);
	if (!env || !*env)
		return false;
	const size_t token_len = strlen(token);
	while (*env) {
		while (*env == ' ' || *env == '\t' || *env == ',')
			env++;
		if (!*env)
			break;
		const char *start = env;
		while (*env && *env != ',')
			env++;
		const char *end = env;
		while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
			end--;
		if ((size_t)(end - start) == token_len && strncmp(start, token, token_len) == 0)
			return true;
	}
	return false;
}

static inline bool jit_restore_barrier(const char *token)
{
	return jit_env_has_csv_token("B2_JIT_RESTORE_BARRIERS", token) ||
		jit_env_has_csv_token("B2_JIT_RESTORE_BARRIERS", "all");
}

static inline bool jit_force_exact_exec_nostats_opcode(uae_u16 op)
{
	(void)op;
	return false;
}

static inline bool jit_force_exact_exec_nostats_pc(uae_u32 pc)
{
	(void)pc;
	return false;
}

static inline bool jit_force_interpreter_barrier_opcode(uae_u16 op)
{
	/* ARM64: zero hardcoded barriers.
	   MOVE16 uses readlong/writelong in gencomp.c.
	   EMUL_OP has compiled handler op_emulop_comp_ff.
	   MOVEM uses readlong/writelong in gencomp.c.
	   PC_P uses 64-bit eviction/reload in tomem/do_load_reg. */

	/* Environment-gated barriers for debugging (B2_JIT_RESTORE_BARRIERS). */
	if (jit_restore_barrier("sr")) {
		if (table68k[op].mnemo == i_MV2SR && table68k[op].size == sz_word)
			return true;
	}
	if (jit_restore_barrier("jsr") && (op & 0xffc0) == 0x4e80)
		return true;
	if (jit_restore_barrier("jmp") && (op & 0xffc0) == 0x4ec0)
		return true;
	if (jit_restore_barrier("ret") &&
		(op == 0x4e73 || op == 0x4e74 || op == 0x4e75 || op == 0x4e76 || op == 0x4e77))
		return true;
	if (jit_restore_barrier("branch") && (op & 0xf000) == 0x6000)
		return true;
	if (jit_restore_barrier("movem") && (op & 0xfb80) == 0x4880)
		return true;
	if (jit_restore_barrier("aline") && (op & 0xf000) == 0xa000)
		return true;
	if (jit_restore_barrier("emulop") && (op & 0xff00) == 0x7100)
		return true;

	return false;
}
static inline bool jit_flush_each_op_env(void)
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_JIT_FLUSH_EACH_OP") && *getenv("B2_JIT_FLUSH_EACH_OP") && strcmp(getenv("B2_JIT_FLUSH_EACH_OP"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

static inline bool jit_end_block_on_fallback_env(void)
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_JIT_END_BLOCK_ON_FALLBACK") && *getenv("B2_JIT_END_BLOCK_ON_FALLBACK") && strcmp(getenv("B2_JIT_END_BLOCK_ON_FALLBACK"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

static inline bool jit_force_nondirect_handler_env(void)
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_JIT_FORCE_NONDIRECT_HANDLER") && *getenv("B2_JIT_FORCE_NONDIRECT_HANDLER") && strcmp(getenv("B2_JIT_FORCE_NONDIRECT_HANDLER"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

static inline bool jit_force_execute_normal_successor_env(void)
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_JIT_FORCE_EXECUTE_NORMAL_SUCCESSOR") && *getenv("B2_JIT_FORCE_EXECUTE_NORMAL_SUCCESSOR") && strcmp(getenv("B2_JIT_FORCE_EXECUTE_NORMAL_SUCCESSOR"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

static inline bool jit_store_pcp_on_chain_env(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *env = getenv("B2_JIT_STORE_PCP_ON_CHAIN");
#if defined(CPU_AARCH64)
		cached = (!env || !*env || strcmp(env, "0") != 0) ? 1 : 0;
#else
		cached = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
#endif
	}
	return cached != 0;
}

static inline bool jit_trace_setpc_env(void)
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_JIT_TRACE_SETPC") && *getenv("B2_JIT_TRACE_SETPC") && strcmp(getenv("B2_JIT_TRACE_SETPC"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

static inline uae_u32 jit_hostpc_to_macpc(uintptr hostpc)
{
    return get_virtual_address((uae_u8*)hostpc);
}

static inline uintptr jit_canonicalize_target_pc(uintptr pc)
{
    uintptr base = (uintptr)RAMBaseHost;
    uintptr limit = base + RAMSize + ROMSize + 0x1000000;
    if (pc >= base && pc < limit)
        return pc;
    /* If a guest Mac PC leaked into a const-target path, convert it back
       to the host fetch pointer expected by PC_P / blockinfo. */
    uae_u32 guest = (uae_u32)pc;
    if ((guest & 1) == 0 && guest < (uae_u32)(RAMSize + ROMSize + 0x1000000))
        return (uintptr)get_real_address(guest, 0, sz_word);
    return pc;
}

static inline bool jit_target_pc_in_env_ranges(const char *env_name, uintptr hostpc)
{
    struct range_cache {
        const char *env_name;
        int initialized;
        int range_count;
        struct { uae_u32 start; uae_u32 end; } ranges[64];
    };
    static range_cache caches[] = {
        {"B2_JIT_FORCE_NONDIRECT_TARGET_PCS", 0, 0, {}},
        {"B2_JIT_FORCE_EXECUTE_NORMAL_TARGET_PCS", 0, 0, {}},
    };
    range_cache *cache = NULL;
    for (size_t i = 0; i < sizeof(caches) / sizeof(caches[0]); i++) {
        if (strcmp(caches[i].env_name, env_name) == 0) {
            cache = &caches[i];
            break;
        }
    }
    if (!cache)
        return false;
    if (!cache->initialized) {
        const char *env = getenv(env_name);
        cache->initialized = 1;
        if (env && *env) {
            const char *p = env;
            while (*p && cache->range_count < (int)(sizeof(cache->ranges) / sizeof(cache->ranges[0]))) {
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')
                    p++;
                if (!*p)
                    break;
                char *endp = NULL;
                unsigned long start = strtoul(p, &endp, 0);
                unsigned long end = start;
                if (endp == p)
                    break;
                p = endp;
                if (*p == '-') {
                    p++;
                    end = strtoul(p, &endp, 0);
                    if (endp == p)
                        end = start;
                    p = endp;
                }
                cache->ranges[cache->range_count].start = (uae_u32)start;
                cache->ranges[cache->range_count].end = (uae_u32)end;
                cache->range_count++;
                while (*p && *p != ',')
                    p++;
            }
        }
    }
    const uae_u32 macpc = jit_hostpc_to_macpc(hostpc);
    for (int i = 0; i < cache->range_count; i++) {
        if (macpc >= cache->ranges[i].start && macpc <= cache->ranges[i].end)
            return true;
    }
    return false;
}

static inline bool jit_force_nondirect_target_env(uintptr hostpc)
{
    return jit_target_pc_in_env_ranges("B2_JIT_FORCE_NONDIRECT_TARGET_PCS", hostpc);
}

static inline bool jit_force_execute_normal_target_env(uintptr hostpc)
{
    return jit_target_pc_in_env_ranges("B2_JIT_FORCE_EXECUTE_NORMAL_TARGET_PCS", hostpc);
}

static inline bool jit_verify_block_target_pc(uae_u32 pc)
{
    static int initialized = 0;
    static int range_count = 0;
    static struct { uae_u32 start; uae_u32 end; } ranges[64];
    if (!initialized) {
        const char *env = getenv("B2_JIT_VERIFY_BLOCKS");
        initialized = 1;
        if (env && *env) {
            const char *p = env;
            while (*p && range_count < (int)(sizeof(ranges) / sizeof(ranges[0]))) {
                while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')
                    p++;
                if (!*p)
                    break;
                char *endp = NULL;
                unsigned long start = strtoul(p, &endp, 0);
                unsigned long end = start;
                if (endp == p)
                    break;
                p = endp;
                if (*p == '-') {
                    p++;
                    end = strtoul(p, &endp, 0);
                    if (endp == p)
                        end = start;
                    p = endp;
                }
                ranges[range_count].start = (uae_u32)start;
                ranges[range_count].end = (uae_u32)end;
                range_count++;
                while (*p && *p != ',')
                    p++;
            }
        }
    }
    for (int i = 0; i < range_count; i++) {
        if (pc >= ranges[i].start && pc <= ranges[i].end)
            return true;
    }
    return false;
}

static inline bool jit_is_framebuffer_addr(uae_u32 addr)
{
	/* Diagnostic path: current BasiliskII SDL framebuffer lives in low Mac RAM
	   at 0x14110000 for our test setups. Use a generous 2 MiB window so we can
	   force JIT VRAM writes through the slow mem-bank helpers and distinguish
	   store-path bugs from value-generation bugs. */
	return addr >= 0x14110000 && addr < 0x14310000;
}

#if defined(CPU_AARCH64)
static uintptr jit_last_setpc_value = 0;
static uae_u32 jit_last_setpc_kind = 0;
static unsigned long jit_last_setpc_seq = 0;

static const char *jit_setpc_kind_name(uae_u32 kind)
{
    return kind == 1 ? "execute_normal_setpc" :
        kind == 2 ? "check_checksum_setpc" :
        kind == 3 ? "exec_nostats_setpc" :
        kind == 4 ? "endblock_pc_inreg" :
        kind == 5 ? "endblock_pc_isconst" :
        kind == 6 ? "endblock_pc_isconst_slow" :
        kind == 7 ? "mov_l_mi_pcptr" :
        kind == 8 ? "mov_l_mr_pcptr" :
        kind == 9 ? "raw_set_pc_i" :
        "unknown";
}

extern "C" void jit_trace_setpc_value(uintptr value, uae_u32 kind)
{
    static unsigned long count = 0;
    if (!jit_trace_setpc_env())
        return;
    jit_last_setpc_value = value;
    jit_last_setpc_kind = kind;
    jit_last_setpc_seq++;
    uintptr base = (uintptr)RAMBaseHost;
    uintptr limit = base + RAMSize + ROMSize + 0x1000000;
    bool suspicious = (value < base || value >= limit || (value & 1));
    if (!suspicious && count >= 32)
        return;
    fprintf(stderr,
        "JIT_SETPC[%lu] kind=%s value=%p regs.pc=%08x regs.pc_p=%p oldp=%p d0=%08x d1=%08x a0=%08x a7=%08x suspicious=%d\n",
        ++count, jit_setpc_kind_name(kind), (void*)value,
        (unsigned)regs.pc, (void*)regs.pc_p, (void*)regs.pc_oldp,
        (unsigned)regs.regs[0], (unsigned)regs.regs[1], (unsigned)regs.regs[8], (unsigned)regs.regs[15],
        suspicious ? 1 : 0);
}
#endif


static inline int distrust_check(int value)
{
#ifdef JIT_ALWAYS_DISTRUST
    return 1;
#else
    int distrust = value;
#ifdef FSUAE
    switch (value) {
    case 0: distrust = 0; break;
    case 1: distrust = 1; break;
    case 2: distrust = ((start_pc & 0xF80000) == 0xF80000); break;
    case 3: distrust = !have_done_picasso; break;
    default: abort();
    }
#endif
    return distrust;
#endif
}

static inline int distrust_byte(void)
{
    return distrust_check(currprefs.comptrustbyte);
}

static inline int distrust_word(void)
{
    return distrust_check(currprefs.comptrustword);
}

static inline int distrust_long(void)
{
    return distrust_check(currprefs.comptrustlong);
}

static inline int distrust_addr(void)
{
    return distrust_check(currprefs.comptrustnaddr);
}

//#if DEBUG
//#define PROFILE_COMPILE_TIME        1
//#endif
//#define PROFILE_UNTRANSLATED_INSNS    1

#ifdef JIT_DEBUG
#undef abort
#define abort() do { \
  fprintf(stderr, "Abort in file %s at line %d\n", __FILE__, __LINE__); \
  SDL_Quit();  \
  exit(EXIT_FAILURE); \
} while (0)
#endif

#ifdef RECORD_REGISTER_USAGE
static uint64 reg_count[16];
static uint64 reg_count_local[16];

static int reg_count_compare(const void* ap, const void* bp)
{
    const int a = *((int*)ap);
    const int b = *((int*)bp);
    return reg_count[b] - reg_count[a];
}
#endif

#ifdef PROFILE_COMPILE_TIME
#include <time.h>
static uae_u32 compile_count    = 0;
static clock_t compile_time     = 0;
static clock_t emul_start_time  = 0;
static clock_t emul_end_time    = 0;
#endif

static uae_u32 raw_cputbl_count[65536] = { 0, };
#ifdef PROFILE_UNTRANSLATED_INSNS
static const int untranslated_top_ten = 50;
static uae_u16 opcode_nums[65536];


static int __cdecl untranslated_compfn(const void* e1, const void* e2)
{
    int v1 = *(const uae_u16*)e1;
    int v2 = *(const uae_u16*)e2;
    return (int)raw_cputbl_count[v2] - (int)raw_cputbl_count[v1];
}
#endif

static compop_func *compfunctbl[65536];
static compop_func *nfcompfunctbl[65536];
#ifdef NOFLAGS_SUPPORT_GENCOMP
static cpuop_func* nfcpufunctbl[65536];
#endif

uae_u8* comp_pc_p;

// gb-- Extra data for Basilisk II/JIT
#define follow_const_jumps (currprefs.comp_constjump != 0)

static uae_u32 cache_size = 0;            // Size of total cache allocated for compiled blocks
static uae_u32 current_cache_size   = 0;  // Cache grows upwards: how much has been consumed already
#ifdef USE_JIT_FPU
#define avoid_fpu (!currprefs.compfpu)
#define lazy_flush (!currprefs.comp_hardflush)
#else
#define avoid_fpu (true)
#define lazy_flush (true)
#endif
static bool		have_cmov = false;	// target has CMOV instructions ?
static bool		have_rat_stall = true;	// target has partial register stalls ?
const bool		tune_alignment = true;	// Tune code alignments for running CPU ?
const bool		tune_nop_fillers = true;	// Tune no-op fillers for architecture
static bool		setzflg_uses_bsf = false;	// setzflg virtual instruction can use native BSF instruction correctly?
static int		align_loops = 32;	// Align the start of loops
static int		align_jumps = 32;	// Align the start of jumps
static int		optcount[10] = {
#ifdef UAE
    4,		// How often a block has to be executed before it is translated
#else
    10,		// How often a block has to be executed before it is translated
#endif
    0,		// How often to use naive translation
    0, 0, 0, 0,
    -1, -1, -1, -1
};

static void jit_force_translate_check(void)
{
    const char *env = getenv("B2_JIT_FORCE_TRANSLATE");
    if (env && *env && strcmp(env, "0") != 0) {
        optcount[0] = 0;  // Skip interpreter warm-up, compile immediately
        fprintf(stderr, "JIT: B2_JIT_FORCE_TRANSLATE active — optcount[0] set to 0\n");
    }
}

op_properties prop[65536];

#ifdef AMIBERRY
bool may_raise_exception = false;
static bool flags_carry_inverted = false;
static bool disasm_this = false;
#endif

static inline bool is_const_jump(uae_u32 opcode)
{
    return (prop[uae_bswap_16(opcode)].cflow == fl_const_jump);
}

static inline unsigned int cft_map(unsigned int f)
{
#ifdef UAE
#if !defined(HAVE_GET_WORD_UNSWAPPED)
    return f;
#else
    return do_byteswap_16(f);
#endif
#else
#if !defined(HAVE_GET_WORD_UNSWAPPED) || defined(FULLMMU)
    return f;
#else
    return ((f >> 8) & 255) | ((f & 255) << 8);
#endif
#endif
}

uae_u8* start_pc_p;
uae_u32 start_pc;
uintptr current_block_pc_p;
static uintptr current_block_start_target;
uae_u32 needed_flags;
static uintptr next_pc_p;
static uintptr taken_pc_p;
static int     branch_cc;
static int redo_current_block;
static bool jit_force_runtime_pc_endblock = false;

#ifdef UAE
int segvcount = 0;
#endif
uae_u8* current_compile_p = NULL;
static uae_u8* max_compile_start;
uae_u8* compiled_code = NULL;
static uae_s32 reg_alloc_run;
const int POPALLSPACE_SIZE = 4096; /* That should be enough space */
uae_u8* popallspace = NULL;

#if defined(CPU_AARCH64)
/* On ARM64, popallspace and JIT cache are allocated as one contiguous
 * block to guarantee the cache is within ARM64 B/BL branch range
 * (+-128 MB) of popallspace. Separate allocations may be scattered
 * too far apart by the kernel's mmap placement. */
static size_t popall_combined_alloc_size = 0;
static uint8 *popall_combined_cache_start = NULL;
static uint32 popall_combined_cache_kb = 0;
#endif

void* pushall_call_handler = NULL;
static void* popall_do_nothing = NULL;
static void* popall_exec_nostats = NULL;
static void* popall_execute_normal = NULL;
static void* popall_cache_miss = NULL;
static void* popall_recompile_block = NULL;
static void* popall_check_checksum = NULL;

#ifdef AMIBERRY
static void* popall_exec_nostats_setpc = NULL;
static void* popall_execute_normal_setpc = NULL;
static void* popall_check_checksum_setpc = NULL;
static void* popall_execute_exception = NULL;
#endif

/* The 68k only ever executes from even addresses. So right now, we
 * waste half the entries in this array
 * UPDATE: We now use those entries to store the start of the linked
 * lists that we maintain for each hash result.
 */
static cacheline cache_tags[TAGSIZE];
static int cache_enabled = 0;
static blockinfo* hold_bi[MAX_HOLD_BI];
blockinfo* active;
blockinfo* dormant;

/* ---- JIT dispatch diagnostic counters ---- */
#if defined(CPU_AARCH64)
#include <cstdio>
#include <ctime>
static unsigned long jit_diag_execute_normal_calls = 0;
static unsigned long jit_diag_execute_normal_cache_hit = 0;  /* check_for_cache_miss returned 1 */
static unsigned long jit_diag_compile_block_calls = 0;
static unsigned long jit_diag_compile_block_fresh = 0;     /* bi->status == BI_INVALID */
static unsigned long jit_diag_compile_block_recomp = 0;    /* bi->status == BI_NEED_RECOMP */
static unsigned long jit_diag_do_nothing_calls = 0;
static unsigned long jit_diag_exec_nostats_calls = 0;
static unsigned long jit_diag_cache_miss_calls = 0;
static unsigned long jit_diag_recompile_block_calls = 0;
static unsigned long jit_diag_check_checksum_calls = 0;
static unsigned long jit_diag_flush_icache_hard_calls = 0;
static unsigned long jit_diag_dispatch_count = 0;          /* total helper/dispatcher entries */
static unsigned long jit_diag_optlev0_blocks = 0;          /* blocks compiled at optlev 0 */
static unsigned long jit_diag_optlev_gt0_blocks = 0;       /* blocks compiled at optlev > 0 */
static unsigned long long jit_diag_compiled_m68k_insns = 0;
static unsigned long long jit_diag_compiled_m68k_cycles = 0;
static unsigned long long jit_diag_compiled_code_bytes = 0;
static unsigned long long jit_diag_peak_cache_bytes = 0;
static unsigned long jit_diag_max_blocklen = 0;
static unsigned long jit_diag_max_block_cycles = 0;
static unsigned long jit_diag_max_block_bytes = 0;
static unsigned long jit_diag_checksum_good = 0;
static unsigned long jit_diag_checksum_bad = 0;
static time_t jit_diag_last_print = 0;
static time_t jit_diag_start_time = 0;

static bool jit_diag_enabled(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("B2_JIT_DIAG") && *getenv("B2_JIT_DIAG")) ? 1 : 0;
    return cached != 0;
}

static void jit_diag_note_compile_block(unsigned blocklen, unsigned totcycles, unsigned emitted_bytes, unsigned long long cache_bytes_used)
{
    jit_diag_compiled_m68k_insns += blocklen;
    jit_diag_compiled_m68k_cycles += totcycles;
    jit_diag_compiled_code_bytes += emitted_bytes;
    if (blocklen > jit_diag_max_blocklen)
        jit_diag_max_blocklen = blocklen;
    if (totcycles > jit_diag_max_block_cycles)
        jit_diag_max_block_cycles = totcycles;
    if (emitted_bytes > jit_diag_max_block_bytes)
        jit_diag_max_block_bytes = emitted_bytes;
    if (cache_bytes_used > jit_diag_peak_cache_bytes)
        jit_diag_peak_cache_bytes = cache_bytes_used;
}

static void jit_diag_note_checksum_result(bool isgood)
{
    if (isgood)
        jit_diag_checksum_good++;
    else
        jit_diag_checksum_bad++;
}

static void jit_diag_maybe_print(void)
{
    if (!jit_diag_enabled())
        return;
    static unsigned long call_count = 0;
    if (++call_count % 100000 != 0)
        return;
    if (jit_diag_start_time == 0)
        jit_diag_start_time = time(NULL);
    time_t now = time(NULL);
    if (now - jit_diag_last_print < 2)
        return;
    jit_diag_last_print = now;
    unsigned long elapsed = (unsigned long)(now - jit_diag_start_time);
    const double avg_blocklen = jit_diag_compile_block_calls ? (double)jit_diag_compiled_m68k_insns / (double)jit_diag_compile_block_calls : 0.0;
    const double avg_block_bytes = jit_diag_compile_block_calls ? (double)jit_diag_compiled_code_bytes / (double)jit_diag_compile_block_calls : 0.0;
    const double bytes_per_insn = jit_diag_compiled_m68k_insns ? (double)jit_diag_compiled_code_bytes / (double)jit_diag_compiled_m68k_insns : 0.0;
    fprintf(stderr,
        "JIT_DIAG t=%lus dispatch=%lu exec_normal=%lu (cache_hit=%lu) compile=%lu (fresh=%lu recomp=%lu opt0=%lu opt>0=%lu) "
        "do_nothing=%lu exec_nostats=%lu cache_miss=%lu recompile_block=%lu check_checksum=%lu (good=%lu bad=%lu) flush_hard=%lu "
        "avg_block=%.1f insn avg_code=%.1fB code_per_insn=%.2fB peak_cache=%.1fKB max_block=%lu insn/%lu cyc/%luB pc=0x%08x\n",
        elapsed, jit_diag_dispatch_count,
        jit_diag_execute_normal_calls, jit_diag_execute_normal_cache_hit,
        jit_diag_compile_block_calls, jit_diag_compile_block_fresh, jit_diag_compile_block_recomp,
        jit_diag_optlev0_blocks, jit_diag_optlev_gt0_blocks,
        jit_diag_do_nothing_calls, jit_diag_exec_nostats_calls,
        jit_diag_cache_miss_calls, jit_diag_recompile_block_calls,
        jit_diag_check_checksum_calls, jit_diag_checksum_good, jit_diag_checksum_bad,
        jit_diag_flush_icache_hard_calls,
        avg_blocklen, avg_block_bytes, bytes_per_insn,
        (double)jit_diag_peak_cache_bytes / 1024.0,
        jit_diag_max_blocklen, jit_diag_max_block_cycles, jit_diag_max_block_bytes,
        (unsigned)m68k_getpc());
    fflush(stderr);
}
#else
static inline bool jit_diag_enabled(void) { return false; }
static inline void jit_diag_note_compile_block(unsigned, unsigned, unsigned, unsigned long long) {}
static inline void jit_diag_note_checksum_result(bool) {}
static inline void jit_diag_maybe_print(void) {}
#endif
/* ---- end JIT dispatch diagnostic counters ---- */

static void disable_jit_runtime(const char* reason)
{
	jit_log("JIT disabled: %s", reason);
	currprefs.cachesize = 0;
	changed_prefs.cachesize = 0;
	cache_size = 0;
	cache_enabled = 0;
}

#ifdef NOFLAGS_SUPPORT_GENCOMP
/* 68040 */
extern const struct cputbl op_smalltbl_0[];
#endif
extern const struct comptbl op_smalltbl_0_comp_nf[];
extern const struct comptbl op_smalltbl_0_comp_ff[];

static void flush_icache_hard(int);
static void flush_icache_lazy(int);
static void flush_icache_none(int);

#ifdef JIT_DEBUG_MEM_CORRUPTION
// JIT Page 0 DMA Guard
// Protects the first 4KB of Amiga memory (natmem page 0) from corruption
// caused by blitter DMA during Kickstart initialization.
//
// Root cause: Kickstart ROM programs the blitter to clear M68k addresses
// 0x004-0x01B (exception vectors 1-6) during init. With JIT's asynchronous
// blitter, this DMA fires BETWEEN JIT blocks, creating a window where
// exception vectors are zeroed. If an exception fires during this window,
// the CPU jumps to address 0, causing illegal instruction cascades.
//
// Fix: After the first vec2 (Bus Error vector) change — which signals that
// exec library init is replacing ROM handlers — we snapshot the entire first
// page and protect it with mprotect(PROT_READ). All writes trigger SIGSEGV,
// which uses BRK single-step to allow each write through individually.
// After each write completes (SIGTRAP from BRK):
//   - DMA writes (from blitter C code): restored from shadow
//   - CPU writes (from JIT compiled code): shadow updated with new value
// Exception_normal() also has a safety net guard using vector shadows.

// Vec2 tracking — detect when exec library init changes vectors, to arm protection
static uae_u32 jit_dbg_vec2_last = 0;

// Signal handler saved state
static struct sigaction jit_dbg_old_sigaction;
static struct sigaction jit_dbg_old_sigtrap_action;
static volatile int jit_dbg_vec2_page_protected = 0;
static volatile int jit_dbg_vec2_trap_armed = 0;

// BRK single-step state
static volatile uint32_t jit_dbg_saved_next_insn = 0;
static volatile uint32_t *jit_dbg_saved_next_insn_addr = NULL;
static volatile int jit_dbg_brk_step_count = 0;
#define JIT_DBG_BRK_IMM 0xD42EEEE0  // BRK #0x7777

// SIGSEGV→SIGTRAP communication
static volatile uae_u32 jit_dbg_last_write_m68k_addr = 0xFFFFFFFF;
static volatile uae_u64 jit_dbg_last_write_arm64_pc = 0;
static volatile int jit_dbg_vec2_sigsegv_count = 0;

// Full-page shadow (4KB) — initialized when protection is armed
static uae_u8 jit_page0_shadow[4096];
static int jit_page0_shadow_valid = 0;
static int jit_dbg_page0_restore_count = 0;
static int jit_dbg_vec_restore_count = 0;
static int jit_dbg_vec2_write_count = 0;

// Vector shadow for Exception_normal() safety net (M68k big-endian format)
#define JIT_VEC_SHADOW_COUNT 7
uae_u32 jit_vec_shadow[JIT_VEC_SHADOW_COUNT] = {0};


// v36: SIGTRAP handler for BRK single-step.
// When a non-vec2 store to the protected natmem page triggers SIGSEGV, the
// SIGSEGV handler inserts a BRK at the next instruction (PC+4) and unprotects
// the page. The faulting store executes, then hits BRK which fires SIGTRAP.
// This handler restores the original instruction and immediately re-protects
// the natmem page, eliminating the gap where vec2 corruption could slip through.
static void jit_dbg_vec2_sigtrap_handler(int sig, siginfo_t *si, void *ctx_raw)
{
    ucontext_t *uc = (ucontext_t*)ctx_raw;
    unsigned long long trap_pc = uc->uc_mcontext.pc;

    // Check if this is our BRK — the PC should point to our saved address
    if (jit_dbg_saved_next_insn_addr != NULL &&
        trap_pc == (unsigned long long)(uintptr_t)jit_dbg_saved_next_insn_addr)
    {
        jit_dbg_brk_step_count++;

        // Restore the original instruction that was replaced by BRK
        // First ensure the code page is writable
        uintptr_t code_page = (uintptr_t)jit_dbg_saved_next_insn_addr & ~0xFFFUL;
        mprotect((void*)code_page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);

        *(volatile uint32_t*)jit_dbg_saved_next_insn_addr = jit_dbg_saved_next_insn;
        __builtin___clear_cache(
            (char*)jit_dbg_saved_next_insn_addr,
            (char*)(jit_dbg_saved_next_insn_addr + 1));

        jit_dbg_saved_next_insn_addr = NULL;

        // v43: Full-page shadow restore/update after writes to page 0.
        // v42 only protected vectors 1-6 (0x004-0x01B). v43 protects the
        // entire 4KB page, catching DMA writes to FPU vectors (0x0c0-0x0d8),
        // OS data (0x5d0+), and any other low-memory locations.
        //   - DMA writes (ARM64 PC outside JIT cache): restore from shadow
        //   - CPU writes (ARM64 PC inside JIT cache): update shadow
        // The page is still unprotected here, so we can read/write natmem freely.
        {
            uae_u32 wr_addr = jit_dbg_last_write_m68k_addr;
            if (wr_addr < 4096 && jit_page0_shadow_valid) {
                uae_u64 wr_pc = jit_dbg_last_write_arm64_pc;
                // JIT code cache: compiled_code .. current_compile_p
                // If the write came from outside this range, it's DMA → restore
                int from_jit = (compiled_code != NULL &&
                    wr_pc >= (uae_u64)(uintptr_t)compiled_code &&
                    wr_pc < (uae_u64)(uintptr_t)current_compile_p);

                // Align to 4-byte boundary for the restore/update
                uae_u32 aligned_addr = wr_addr & ~3u;

                if (!from_jit) {
                    // DMA write — restore from shadow (undo the corruption)
                    uae_u32 shadow_val = *(uae_u32*)(jit_page0_shadow + aligned_addr);
                    *(volatile uae_u32*)(natmem_offset + aligned_addr) = shadow_val;
                    jit_dbg_page0_restore_count++;

                    // Also update jit_vec_shadow if this was a vector address (safety net)
                    int vec_nr = aligned_addr / 4;
                    if (vec_nr >= 1 && vec_nr <= 6) {
                        jit_dbg_vec_restore_count++;
                    }

                    if (jit_dbg_page0_restore_count <= 30 ||
                        (jit_dbg_page0_restore_count <= 300 && jit_dbg_page0_restore_count % 10 == 0) ||
                        (jit_dbg_page0_restore_count % 500 == 0)) {
                        write_log("JIT_VEC v43: page0 M68k 0x%03x restored from shadow "
                            "after DMA write #%d (ARM64 PC=0x%016llx)\n",
                            aligned_addr, jit_dbg_page0_restore_count,
                            (unsigned long long)wr_pc);
                    }
                } else {
                    // CPU write (from JIT cache) — update shadow with new value
                    uae_u32 new_val = *(volatile uae_u32*)(natmem_offset + aligned_addr);
                    *(uae_u32*)(jit_page0_shadow + aligned_addr) = new_val;

                    // Also update jit_vec_shadow for vectors (Exception_normal safety net)
                    int vec_nr = aligned_addr / 4;
                    if (vec_nr >= 1 && vec_nr <= 6) {
                        uae_u32 m68k_val = do_byteswap_32(new_val);
                        if (m68k_val != 0) {
                            jit_vec_shadow[vec_nr] = m68k_val;
                        }
                    }
                }
            }
            jit_dbg_last_write_m68k_addr = 0xFFFFFFFF;  // Reset for next write
        }

        // Re-protect the natmem page IMMEDIATELY — this is the key improvement.
        // The page was only unprotected for exactly ONE instruction (the faulting store).
        unsigned long page_base = (unsigned long)natmem_offset & ~0xFFFUL;
        mprotect((void*)page_base, 4096, PROT_READ);
        jit_dbg_vec2_page_protected = 1;

        // Log periodic stats
        if (jit_dbg_brk_step_count <= 10 ||
            (jit_dbg_brk_step_count <= 100 && jit_dbg_brk_step_count % 10 == 0) ||
            (jit_dbg_brk_step_count % 500 == 0)) {
            write_log("JIT_VEC v36:BRK step #%d completed, page re-protected.\n",
                jit_dbg_brk_step_count);
        }

        // Return — CPU re-executes at PC, which now has the restored original instruction
        return;
    }

    // Not our BRK — chain to original SIGTRAP handler
    if (jit_dbg_old_sigtrap_action.sa_flags & SA_SIGINFO) {
        jit_dbg_old_sigtrap_action.sa_sigaction(sig, si, ctx_raw);
    } else if (jit_dbg_old_sigtrap_action.sa_handler != SIG_DFL &&
               jit_dbg_old_sigtrap_action.sa_handler != SIG_IGN) {
        jit_dbg_old_sigtrap_action.sa_handler(sig);
    } else {
        // Default action for SIGTRAP is to terminate — but only if not ours
        write_log("JIT_VEC v36:WARNING: unexpected SIGTRAP at PC=0x%016llx (not our BRK)\n",
            trap_pc);
        // Don't terminate — just return and hope for the best
    }
}

// v36: SIGSEGV handler for mprotect-based vec2 write trap.
// When the first page of natmem is read-only, any write triggers this handler.
// If the write targets the vec2 area (M68k 0x008-0x00b), we dump the ARM64 PC
// and all registers — this identifies the EXACT JIT-compiled instruction responsible.
// For non-vec2 writes: uses BRK single-step to keep the page protected.
static void jit_dbg_vec2_sigsegv_handler(int sig, siginfo_t *si, void *ctx_raw)
{
    uae_u8 *fault_addr = (uae_u8*)si->si_addr;

    // Check if fault is in our protected page
    unsigned long page_base = (unsigned long)natmem_offset & ~0xFFFUL;
    if ((unsigned long)fault_addr < page_base ||
        (unsigned long)fault_addr >= page_base + 4096) {
        // Not our fault — chain to original handler
        if (jit_dbg_old_sigaction.sa_flags & SA_SIGINFO) {
            jit_dbg_old_sigaction.sa_sigaction(sig, si, ctx_raw);
        } else if (jit_dbg_old_sigaction.sa_handler != SIG_DFL &&
                   jit_dbg_old_sigaction.sa_handler != SIG_IGN) {
            jit_dbg_old_sigaction.sa_handler(sig);
        } else {
            signal(SIGSEGV, SIG_DFL);
            raise(SIGSEGV);
        }
        return;
    }

    ucontext_t *uc = (ucontext_t*)ctx_raw;
    unsigned long long arm64_pc = uc->uc_mcontext.pc;
    uae_u32 m68k_addr = (uae_u32)(fault_addr - natmem_offset);

    jit_dbg_vec2_sigsegv_count++;

    // v42: Save write info for SIGTRAP handler (immediate vector restore)
    jit_dbg_last_write_m68k_addr = m68k_addr;
    jit_dbg_last_write_arm64_pc = arm64_pc;

    // Unprotect the page so the write can complete and we can log
    mprotect((void*)page_base, 4096, PROT_READ | PROT_WRITE);
    jit_dbg_vec2_page_protected = 0;

    // Check if this write targets the vector table area (M68k 0x004-0x01b)
    // v41: expanded from vec2-only (0x008-0x00b) to full blitter-cleared range.
    // Vectors 1-6 all get corrupted by blitter DMA during Kickstart init.
    if (m68k_addr >= 0x004 && m68k_addr <= 0x01b) {
        jit_dbg_vec2_write_count++;
        if (jit_dbg_vec2_write_count <= 20 ||
            (jit_dbg_vec2_write_count <= 200 && jit_dbg_vec2_write_count % 10 == 0) ||
            (jit_dbg_vec2_write_count % 100 == 0)) {
            int vec_nr = m68k_addr / 4;
            write_log("JIT_VEC v41: vector DMA write #%d: vec%d M68k 0x%03x, "
                "ARM64 PC=0x%016llx, val=0x%04llx (BRK step)\n",
                jit_dbg_vec2_write_count, vec_nr, m68k_addr, arm64_pc,
                (unsigned long long)uc->uc_mcontext.regs[1]);
        }
        // Fall through to BRK single-step below (same path as non-vector writes)
    }

    // BRK single-step for ALL writes (vec2 and non-vec2).
    // Strategy: insert BRK at next instruction (PC+4), return.
    // Faulting store re-executes (succeeds), then hits BRK -> SIGTRAP handler
    // immediately re-protects the page. Page is unprotected for exactly ONE insn.

    if (jit_dbg_vec2_sigsegv_count <= 30 ||
        (jit_dbg_vec2_sigsegv_count <= 300 && jit_dbg_vec2_sigsegv_count % 10 == 0) ||
        (jit_dbg_vec2_sigsegv_count % 500 == 0)) {
        write_log("JIT_VEC v36:page write #%d: M68k 0x%03x ARM64_PC=0x%016llx (BRK step)\n",
            jit_dbg_vec2_sigsegv_count, m68k_addr, arm64_pc);
    }

    // Safety check: if a previous BRK is still pending, something went wrong.
    // Fall back to unprotect-and-return (v35 behavior) for safety.
    if (jit_dbg_saved_next_insn_addr != NULL) {
        write_log("JIT_VEC v36:WARNING: previous BRK still pending at %p! "
            "Falling back to unprotect-and-return.\n",
            (void*)jit_dbg_saved_next_insn_addr);
        // Page is already unprotected (we did it at line 684 above)
        jit_dbg_vec2_page_protected = 0;
        return;
    }

    // Insert BRK #0x7777 at the instruction AFTER the faulting store (PC+4).
    // When the store re-executes (page is unprotected), control flows to PC+4
    // which now has BRK. This fires SIGTRAP, and our handler re-protects the page.
    uint32_t *next_insn_addr = (uint32_t*)((uintptr_t)arm64_pc + 4);

    // Ensure the code page containing PC+4 is writable (JIT cache may be RX)
    uintptr_t code_page = (uintptr_t)next_insn_addr & ~0xFFFUL;
    mprotect((void*)code_page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);

    // Save the original instruction and insert BRK
    jit_dbg_saved_next_insn = *next_insn_addr;
    jit_dbg_saved_next_insn_addr = next_insn_addr;
    *next_insn_addr = JIT_DBG_BRK_IMM;

    // Flush instruction cache so CPU sees the BRK
    __builtin___clear_cache((char*)next_insn_addr, (char*)(next_insn_addr + 1));

    // Page is already unprotected (we did it above for logging).
    // The faulting store will re-execute successfully, then hit BRK → SIGTRAP.
    jit_dbg_vec2_page_protected = 0;
    // Return — store executes, then BRK fires SIGTRAP → handler re-protects page
}

// v34: Vec2 check function callable from ALL C dispatch functions.
// This covers the "dark zone" where compiled blocks chain via hash table
// dispatch without any vec2 monitoring. Called from:
//   do_nothing(), exec_nostats(), execute_normal() [in newcpu.cpp]
//   cache_miss(), recompile_block(), compile_block end [in this file]
// Called from C dispatch functions to detect vec2 changes and arm page protection.
// Once armed, signal handlers take over — this function stops being called.
void jit_dbg_check_vec2_dispatch(const char* func_name)
{
    if (jit_dbg_vec2_trap_armed || !natmem_offset)
        return;

    // Read vec2 (Bus Error vector at M68k 0x008)
    uae_u32 cur_vec2 = *(volatile uae_u32*)(natmem_offset + 0x008);
    if (cur_vec2 == 0)
        return;  // ROM hasn't initialized vectors yet

    // Track first non-zero value
    if (jit_dbg_vec2_last == 0) {
        jit_dbg_vec2_last = cur_vec2;
        return;
    }

    // No change — nothing to do
    if (cur_vec2 == jit_dbg_vec2_last)
        return;

    // Vec2 changed — exec library is replacing ROM handlers.
    // Time to arm the page 0 DMA guard.
    write_log("JIT: Page 0 DMA guard: vec2 changed in %s, arming protection.\n", func_name);
    jit_dbg_vec2_last = cur_vec2;

    // Initialize vector shadows (M68k big-endian format)
    for (int vi = 1; vi <= 6; vi++) {
        uae_u32 raw_val = *(volatile uae_u32*)(natmem_offset + vi * 4);
        uae_u32 m68k_val = do_byteswap_32(raw_val);
        if (m68k_val != 0)
            jit_vec_shadow[vi] = m68k_val;
    }

    // Install SIGTRAP handler (for BRK single-step)
    struct sigaction sa_trap;
    memset(&sa_trap, 0, sizeof(sa_trap));
    sa_trap.sa_sigaction = jit_dbg_vec2_sigtrap_handler;
    sa_trap.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa_trap.sa_mask);
    if (sigaction(SIGTRAP, &sa_trap, &jit_dbg_old_sigtrap_action) != 0) {
        write_log("JIT: WARNING: SIGTRAP handler install failed, errno=%d\n", errno);
        return;
    }

    // Install SIGSEGV handler (for page fault interception)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = jit_dbg_vec2_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &jit_dbg_old_sigaction) != 0) {
        write_log("JIT: WARNING: SIGSEGV handler install failed, errno=%d\n", errno);
        return;
    }

    // Snapshot the entire first page BEFORE protecting it
    memcpy(jit_page0_shadow, (void*)natmem_offset, 4096);
    jit_page0_shadow_valid = 1;
    write_log("JIT: Page 0 shadow initialized (4096 bytes).\n");

    // Protect the first page of natmem (M68k 0x000-0xFFF)
    unsigned long page_base = (unsigned long)natmem_offset & ~0xFFFUL;
    if (mprotect((void*)page_base, 4096, PROT_READ) == 0) {
        jit_dbg_vec2_page_protected = 1;
        jit_dbg_vec2_trap_armed = 1;
        write_log("JIT: Page 0 DMA guard active at %p.\n", (void*)page_base);
    } else {
        write_log("JIT: WARNING: mprotect failed, errno=%d\n", errno);
    }
}
#endif

static bigstate live;
static smallstate empty_ss;
static smallstate default_ss;
static int optlev;

static int writereg(int r);
static void unlock2(int r);
static void setlock(int r);
static int readreg_specific(int r, int size, int spec);
static int writereg_specific(int r, int size, int spec);

#ifdef AMIBERRY
static int readreg(int r);
static void prepare_for_call_1(void);
static void prepare_for_call_2(void);

STATIC_INLINE void flush_cpu_icache(void *from, void *to);
STATIC_INLINE void jit_begin_write_window(void);
STATIC_INLINE void jit_end_write_window(void);
#endif
STATIC_INLINE void write_jmp_target(uae_u32 *jmpaddr, uintptr a);

static int jit_write_window_depth = 0;

STATIC_INLINE void jit_begin_write_window(void)
{
	jit_write_window_depth++;
#if defined(__APPLE__) && defined(CPU_AARCH64)
	if (jit_write_window_depth == 1) {
		uae_vm_jit_write_protect(false);
	}
#endif
}

STATIC_INLINE void jit_end_write_window(void)
{
	if (jit_write_window_depth <= 0) {
		write_log("JIT: write window underflow\n");
		jit_write_window_depth = 0;
		return;
	}
	jit_write_window_depth--;
#if defined(__APPLE__) && defined(CPU_AARCH64)
	if (jit_write_window_depth == 0) {
		uae_vm_jit_write_protect(true);
	}
#endif
}

uae_u32 m68k_pc_offset;

/* Some arithmetic operations can be optimized away if the operands
 * are known to be constant. But that's only a good idea when the
 * side effects they would have on the flags are not important. This
 * variable indicates whether we need the side effects or not
 */
static uae_u32 needflags = 0;

/* Flag handling is complicated.
 *
 * ARM instructions create flags, which quite often are exactly what we
 * want. So at times, the "68k" flags are actually in the ARM flags.
 * Exception: carry is often inverted.
 *
 * Then again, sometimes we do ARM instructions that clobber the ARM
 * flags, but don't represent a corresponding m68k instruction. In that
 * case, we have to save them.
 *
 * We used to save them to the stack, but now store them back directly
 * into the regflags.nzcv of the traditional emulation. Thus some odd
 * names.
 *
 * So flags can be in either of two places (used to be three; boy were
 * things complicated back then!); And either place can contain either
 * valid flags or invalid trash (and on the stack, there was also the
 * option of "nothing at all", now gone). A couple of variables keep
 * track of the respective states.
 *
 * To make things worse, we might or might not be interested in the flags.
 * by default, we are, but a call to dont_care_flags can change that
 * until the next call to live_flags. If we are not, pretty much whatever
 * is in the register and/or the native flags is seen as valid.
*/

static inline blockinfo* get_blockinfo(uae_u32 cl)
{
    return cache_tags[cl + 1].bi;
}

static inline blockinfo* get_blockinfo_addr(void* addr)
{
    blockinfo* bi = get_blockinfo(cacheline(addr));
    int safety = 0;

    while (bi) {
        /* Guard against corrupted hash chain pointers */
        if ((uintptr)bi < 0x1000 || (uintptr)bi > 0x0000FFFFFFFFFFFFUL) {
            static int warn = 0;
            if (warn++ < 3)
                fprintf(stderr, "JIT: corrupt blockinfo chain bi=%p for addr=%p\n", bi, addr);
            return NULL;
        }
        if (bi->pc_p == addr)
            return bi;
        bi = bi->next_same_cl;
        if (++safety > 10000) {
            static int warn2 = 0;
            if (warn2++ < 3)
                fprintf(stderr, "JIT: blockinfo chain too long for addr=%p\n", addr);
            return NULL;
        }
    }
    return NULL;
}


/*******************************************************************
 * All sorts of list related functions for all of the lists        *
 *******************************************************************/

static inline void remove_from_cl_list(blockinfo* bi)
{
    uae_u32 cl = cacheline(bi->pc_p);

    if (bi->prev_same_cl_p)
        *(bi->prev_same_cl_p) = bi->next_same_cl;
    if (bi->next_same_cl)
        bi->next_same_cl->prev_same_cl_p = bi->prev_same_cl_p;
    if (cache_tags[cl + 1].bi)
        cache_tags[cl].handler = cache_tags[cl + 1].bi->handler_to_use;
    else
        cache_tags[cl].handler = (cpuop_func*)popall_execute_normal;
}

static inline void remove_from_list(blockinfo* bi)
{
    if (bi->prev_p)
        *(bi->prev_p) = bi->next;
    if (bi->next)
        bi->next->prev_p = bi->prev_p;
}

static inline void add_to_cl_list(blockinfo* bi)
{
    uae_u32 cl = cacheline(bi->pc_p);

    if (cache_tags[cl + 1].bi)
        cache_tags[cl + 1].bi->prev_same_cl_p = &(bi->next_same_cl);
    bi->next_same_cl = cache_tags[cl + 1].bi;

    cache_tags[cl + 1].bi = bi;
    bi->prev_same_cl_p = &(cache_tags[cl + 1].bi);

    cache_tags[cl].handler = bi->handler_to_use;
}

void raise_in_cl_list(blockinfo* bi)
{
    remove_from_cl_list(bi);
    add_to_cl_list(bi);
}

static inline void add_to_active(blockinfo* bi)
{
    if (active)
        active->prev_p = &(bi->next);
    bi->next = active;

    active = bi;
    bi->prev_p = &active;
}

static inline void add_to_dormant(blockinfo* bi)
{
    if (dormant)
        dormant->prev_p = &(bi->next);
    bi->next = dormant;

    dormant = bi;
    bi->prev_p = &dormant;
}

static inline void remove_dep(dependency* d)
{
    if (d->prev_p)
        *(d->prev_p) = d->next;
    if (d->next)
        d->next->prev_p = d->prev_p;
    d->prev_p = NULL;
    d->next = NULL;
}

/* This block's code is about to be thrown away, so it no longer
   depends on anything else */
static inline void remove_deps(blockinfo* bi)
{
    remove_dep(&(bi->dep[0]));
    remove_dep(&(bi->dep[1]));
}

static inline void adjust_jmpdep(dependency* d, cpuop_func* a)
{
    write_jmp_target(d->jmp_off, (uintptr)a);
}

/********************************************************************
 * Soft flush handling support functions                            *
 ********************************************************************/

static inline void set_dhtu(blockinfo* bi, cpuop_func* dh)
{
    jit_log2("bi is %p", bi);
    if (dh != bi->direct_handler_to_use) {
        dependency* x = bi->deplist;
        jit_log2("bi->deplist=%p", bi->deplist);
        while (x) {
            jit_log2("x is %p", x);
            jit_log2("x->next is %p", x->next);
            jit_log2("x->prev_p is %p", x->prev_p);

            if (x->jmp_off) {
                adjust_jmpdep(x, dh);
            }
            x = x->next;
        }
        bi->direct_handler_to_use = (cpuop_func*)dh;
    }
}

void invalidate_block(blockinfo* bi)
{
    int i;

    bi->optlevel = 0;
    bi->count = optcount[0] - 1;
    bi->handler = NULL;
    bi->handler_to_use = (cpuop_func*)popall_execute_normal;
    bi->direct_handler = NULL;
    set_dhtu(bi, bi->direct_pen);
    bi->needed_flags = 0xff;
    bi->status = BI_INVALID;
    for (i = 0; i < 2; i++) {
        bi->dep[i].jmp_off = NULL;
        bi->dep[i].target = NULL;
    }
    remove_deps(bi);
}

static inline void create_jmpdep(blockinfo* bi, int i, uae_u32* jmpaddr, uintptr target)
{
    blockinfo* tbi = get_blockinfo_addr((void*)target);

    Dif(!tbi) {
        jit_abort("Could not create jmpdep!");
    }
    bi->dep[i].jmp_off = jmpaddr;
    bi->dep[i].source = bi;
    bi->dep[i].target = tbi;
    bi->dep[i].next = tbi->deplist;
    if (bi->dep[i].next)
        bi->dep[i].next->prev_p = &(bi->dep[i].next);
    bi->dep[i].prev_p = &(tbi->deplist);
    tbi->deplist = &(bi->dep[i]);
}

static inline void block_need_recompile(blockinfo* bi)
{
    uae_u32 cl = cacheline(bi->pc_p);

    set_dhtu(bi, bi->direct_pen);
    bi->direct_handler = bi->direct_pen;

    bi->handler_to_use = (cpuop_func*)popall_execute_normal;
    bi->handler = (cpuop_func*)popall_execute_normal;
    if (bi == cache_tags[cl + 1].bi)
        cache_tags[cl].handler = (cpuop_func*)popall_execute_normal;
    bi->status = BI_NEED_RECOMP;
}

static inline blockinfo* get_blockinfo_addr_new(void* addr)
{
    blockinfo* bi = get_blockinfo_addr(addr);
    int i;

    if (!bi) {
        for (i = 0; i < MAX_HOLD_BI && !bi; i++) {
            if (hold_bi[i]) {
                (void)cacheline(addr);

                bi = hold_bi[i];
                hold_bi[i] = NULL;
                bi->pc_p = (uae_u8*)addr;
                invalidate_block(bi);
                add_to_active(bi);
                add_to_cl_list(bi);
            }
        }
    }
    if (!bi) {
        jit_abort(_T("JIT: Looking for blockinfo, can't find free one\n"));
    }
    return bi;
}

static void prepare_block(blockinfo* bi);

/* Management of blockinfos.

   A blockinfo struct is allocated whenever a new block has to be
   compiled. If the list of free blockinfos is empty, we allocate a new
   pool of blockinfos and link the newly created blockinfos altogether
   into the list of free blockinfos. Otherwise, we simply pop a structure
   of the free list.

   Blockinfo are lazily deallocated, i.e. chained altogether in the
   list of free blockinfos whenvever a translation cache flush (hard or
   soft) request occurs.
*/

template< class T >
class LazyBlockAllocator
{
    enum {
        kPoolSize = 1 + (16384 - sizeof(T) - sizeof(void*)) / sizeof(T)
    };
    struct Pool {
        T chunk[kPoolSize];
        Pool* next;
    };
    Pool* mPools;
    T* mChunks;
public:
    LazyBlockAllocator() : mPools(0), mChunks(0) { }
#ifdef UAE
#else
    ~LazyBlockAllocator();
#endif
    T* acquire();
    void release(T* const);
};

#ifdef UAE
/* uae_vm_release may do logging, which isn't safe to do when the application
 * is shutting down. Better to release memory manually with a function call
 * to a release_all method on shutdown, or even simpler, just let the OS
 * handle it (we're shutting down anyway). */
#else
template< class T >
LazyBlockAllocator<T>::~LazyBlockAllocator()
{
    Pool* currentPool = mPools;
    while (currentPool) {
        Pool* deadPool = currentPool;
        currentPool = currentPool->next;
        vm_release(deadPool, sizeof(Pool));
    }
}
#endif

template< class T >
T* LazyBlockAllocator<T>::acquire()
{
    if (!mChunks) {
        // There is no chunk left, allocate a new pool and link the
        // chunks into the free list
#if defined(CPU_AARCH64)
		Pool * newPool = (Pool *)vm_acquire(sizeof(Pool), VM_MAP_DEFAULT);
#else
		Pool * newPool = (Pool *)vm_acquire(sizeof(Pool), VM_MAP_DEFAULT | VM_MAP_32BIT);
#endif
		if (newPool == VM_MAP_FAILED) {
			jit_abort("Could not allocate block pool!\n");
        }
        for (T* chunk = &newPool->chunk[0]; chunk < &newPool->chunk[kPoolSize]; chunk++) {
            chunk->next = mChunks;
            mChunks = chunk;
        }
        newPool->next = mPools;
        mPools = newPool;
    }
    T* chunk = mChunks;
    mChunks = chunk->next;
    return chunk;
}

template< class T >
void LazyBlockAllocator<T>::release(T* const chunk)
{
    chunk->next = mChunks;
    mChunks = chunk;
}

template< class T >
class HardBlockAllocator
{
public:
    T* acquire() {
        T* data = (T*)current_compile_p;
        current_compile_p += sizeof(T);
        return data;
    }

    void release(T* const) {
        // Deallocated on invalidation
    }
};

static LazyBlockAllocator<blockinfo> BlockInfoAllocator;
static LazyBlockAllocator<checksum_info> ChecksumInfoAllocator;

static inline checksum_info* alloc_checksum_info(void)
{
    checksum_info* csi = ChecksumInfoAllocator.acquire();
    csi->next = NULL;
    return csi;
}

static inline void free_checksum_info(checksum_info* csi)
{
    csi->next = NULL;
    ChecksumInfoAllocator.release(csi);
}

static inline void free_checksum_info_chain(checksum_info* csi)
{
    while (csi != NULL) {
        checksum_info* csi2 = csi->next;
        free_checksum_info(csi);
        csi = csi2;
    }
}

static inline blockinfo* alloc_blockinfo(void)
{
    blockinfo* bi = BlockInfoAllocator.acquire();
    bi->csi = NULL;
    return bi;
}

static inline void free_blockinfo(blockinfo* bi)
{
    free_checksum_info_chain(bi->csi);
    bi->csi = NULL;
    BlockInfoAllocator.release(bi);
}

static inline void alloc_blockinfos(void)
{
    int i;
    blockinfo* bi;

    for (i = 0; i < MAX_HOLD_BI; i++) {
        if (hold_bi[i])
            return;
        bi = hold_bi[i] = alloc_blockinfo();
        prepare_block(bi);
    }
}

/********************************************************************
 * Functions to emit data into memory, and other general support    *
 ********************************************************************/

static uae_u8* target;

static inline void emit_byte(uae_u8 x)
{
    *target++ = x;
}

static inline void skip_n_bytes(int n) {
    target += n;
}

static inline void skip_byte()
{
    skip_n_bytes(1);
}

static inline void skip_word()
{
    skip_n_bytes(2);
}

static inline void skip_long()
{
    skip_n_bytes(4);
}

static inline void skip_quad()
{
    skip_n_bytes(8);
}

static inline void emit_word(uae_u16 x)
{
    *((uae_u16*)target) = x;
    skip_word();
}

static inline void emit_long(uae_u32 x)
{
    *((uae_u32*)target) = x;
    skip_long();
}

static inline void emit_quad(uae_u64 x)
{
    *((uae_u64*)target) = x;
    skip_quad();
}

static inline void emit_block(const uae_u8* block, uae_u32 blocklen)
{
    memcpy((uae_u8*)target, block, blocklen);
    target += blocklen;
}

#define MAX_COMPILE_PTR max_compile_start

static inline uae_u32 reverse32(uae_u32 v)
{
    return uae_bswap_32(v);
}

static void set_target(uae_u8* t)
{
    target = t;
}

static inline uae_u8* get_target_noopt(void)
{
    return target;
}

inline uae_u8* get_target(void)
{
    return get_target_noopt();
}

/********************************************************************
 * New version of data buffer: interleave data and code             *
 ********************************************************************/
#if defined(USE_DATA_BUFFER)

#define DATA_BUFFER_SIZE 768             // Enlarge POPALLSPACE_SIZE if this value is greater than 768
#define DATA_BUFFER_MAXOFFSET 4096 - 32  // max range between emit of data and use of data
static uae_u8* data_writepos = 0;
static uae_u8* data_endpos = 0;
#ifdef DEBUG_DATA_BUFFER
static uae_u32 data_wasted = 0;
static uae_u32 data_buffers_used = 0;
#endif

STATIC_INLINE void compemu_raw_branch(IM32 d);

STATIC_INLINE void data_check_end(uae_s32 n, uae_s32 codesize)
{
    if (data_writepos + n > data_endpos || get_target() + codesize - data_writepos > DATA_BUFFER_MAXOFFSET) {
        // Start new buffer
#ifdef DEBUG_DATA_BUFFER
        if (data_writepos < data_endpos)
            data_wasted += data_endpos - data_writepos;
        data_buffers_used++;
#endif
        compemu_raw_branch(DATA_BUFFER_SIZE);
        data_writepos = get_target();
        data_endpos = data_writepos + DATA_BUFFER_SIZE;
        set_target(get_target() + DATA_BUFFER_SIZE);
    }
}

STATIC_INLINE uae_s32 data_word_offs(uae_u16 x)
{
    data_check_end(4, 4);
    *((uae_u16*)data_writepos) = x;
    data_writepos += 2;
    *((uae_u16*)data_writepos) = 0;
    data_writepos += 2;
    return (uae_s32)data_writepos - (uae_s32)get_target() - 12;
}

STATIC_INLINE uae_s32 data_long(uae_u32 x, uae_s32 codesize)
{
    data_check_end(4, codesize);
    *((uae_u32*)data_writepos) = x;
    data_writepos += 4;
    return (uae_s32)data_writepos - 4;
}

STATIC_INLINE uae_s32 data_long_offs(uae_u32 x)
{
    data_check_end(4, 4);
    *((uae_u32*)data_writepos) = x;
    data_writepos += 4;
    return (uae_s32)data_writepos - (uae_s32)get_target() - 12;
}

STATIC_INLINE uae_s32 get_data_offset(uae_s32 t)
{
    return t - (uae_s32)get_target() - 8;
}

STATIC_INLINE void reset_data_buffer(void)
{
    data_writepos = 0;
    data_endpos = 0;
}

#endif

/********************************************************************
 * Getting the information about the target CPU                     *
 ********************************************************************/
#ifdef AMIBERRY
STATIC_INLINE void clobber_flags(void);
#endif

#if defined(CPU_AARCH64) 
#include "codegen_arm64.cpp"
#elif defined(CPU_arm) 
#include "codegen_arm.cpp"
#endif

/********************************************************************
 * Flags status handling. EMIT TIME!                                *
 ********************************************************************/

static void bt_l_ri_noclobber(RR4 r, IM8 i);

static void make_flags_live_internal(void)
{
    if (live.flags_in_flags == VALID)
        return;
    Dif(live.flags_on_stack == TRASH) {
        jit_abort("Want flags, got something on stack, but it is TRASH");
    }
    if (live.flags_on_stack == VALID) {
        trace_flagflow_log("FLAGS_RESTORE");
        int tmp;
        tmp = readreg(FLAGTMP);
        raw_reg_to_flags(tmp);
        unlock2(tmp);
        flags_carry_inverted = false;

        live.flags_in_flags = VALID;
        return;
    }
    jit_abort("Huh? live.flags_in_flags=%d, live.flags_on_stack=%d, but need to make live",
        live.flags_in_flags, live.flags_on_stack);
}

static void flags_to_stack(void)
{
    if (live.flags_on_stack == VALID) {
        /* Flags already saved to memory, but if carry is still inverted in
           the hardware NZCV register, we must flip it now so that any
           subsequent branch instruction (compemu_raw_jcc_l_oponly) that
           tests the hardware NZCV directly sees the correct polarity.
           This matters for DBRA/DBcc: sub_w_ri sets flags_carry_inverted
           and register_branch uses NATIVE_CC_CC which tests hardware C. */
        if (flags_carry_inverted) {
            MRS_NZCV_x(REG_WORK1);
            EOR_xxCflag(REG_WORK1, REG_WORK1);
            MSR_NZCV_x(REG_WORK1);
        }
        flags_carry_inverted = false;
        return;
    }
    if (!live.flags_are_important) {
        trace_flagflow_log("FLAGS_SKIP_STORE");
        live.flags_on_stack = VALID;
        flags_carry_inverted = false;
        return;
    }
    Dif(live.flags_in_flags != VALID)
        jit_abort("flags_to_stack != VALID");
    else {
        trace_flagflow_log("FLAGS_STORE");
        /* If carry is inverted (ARM64 convention), flip it to M68K convention
           before storing so that restores always see M68K semantics. */
        if (flags_carry_inverted) {
            MRS_NZCV_x(REG_WORK1);
            EOR_xxCflag(REG_WORK1, REG_WORK1);
            MSR_NZCV_x(REG_WORK1);
            flags_carry_inverted = false;
        }
        int tmp = writereg(FLAGTMP);
        raw_flags_to_reg(tmp);
        unlock2(tmp);
    }
    live.flags_on_stack = VALID;
}

STATIC_INLINE void clobber_flags(void)
{
    if (live.flags_in_flags == VALID && live.flags_on_stack != VALID)
        flags_to_stack();
    live.flags_in_flags = TRASH;
}

/* Prepare for leaving the compiled stuff */
static inline void flush_flags(void)
{
    flags_to_stack();
}

static int touchcnt;

/********************************************************************
 * Partial register flushing for optimized calls                    *
 ********************************************************************/

struct regusage {
    uae_u16 rmask;
    uae_u16 wmask;
};

/********************************************************************
 * register allocation per block logging                            *
 ********************************************************************/

static uae_s8 vstate[VREGS];
static uae_s8 vwritten[VREGS];
static uae_s8 nstate[N_REGS];

#define L_UNKNOWN -127
#define L_UNAVAIL -1
#define L_NEEDED -2
#define L_UNNEEDED -3

static inline void log_startblock(void)
{
    int i;

    for (i = 0; i < VREGS; i++) {
        vstate[i] = L_UNKNOWN;
        vwritten[i] = 0;
    }
    for (i = 0; i < N_REGS; i++)
        nstate[i] = L_UNKNOWN;
}

/* Using an n-reg for a temp variable */
static inline void log_isused(int n)
{
    if (nstate[n] == L_UNKNOWN)
        nstate[n] = L_UNAVAIL;
}

static inline void log_visused(int r)
{
    if (vstate[r] == L_UNKNOWN)
        vstate[r] = L_NEEDED;
}

STATIC_INLINE void do_load_reg(int n, int r)
{
#if defined(CPU_AARCH64)
    if (r == FLAGX) {
        compemu_raw_mov_l_rm(n, (uintptr)live.state[r].mem);
        UBFX_xxii(n, n, 29, 1); /* interpreter bit-29 format -> JIT 0/1 */
        return;
    }
    /* PC_P holds a 64-bit host pointer — must use 64-bit load. */
    if (r == PC_P) {
        LOAD_U64(REG_WORK2, (uintptr)live.state[r].mem);
        LDR_xXi(n, REG_WORK2, 0);
        return;
    }
#endif
    compemu_raw_mov_l_rm(n, (uintptr)live.state[r].mem);
}

static inline void log_vwrite(int r)
{
    vwritten[r] = 1;
}

/* Using an n-reg to hold a v-reg */
static inline void log_isreg(int n, int r)
{
    if (nstate[n] == L_UNKNOWN && r < 16 && !vwritten[r] && 0)
        nstate[n] = r;
    else {
        do_load_reg(n, r);
        if (nstate[n] == L_UNKNOWN)
            nstate[n] = L_UNAVAIL;
    }
    if (vstate[r] == L_UNKNOWN)
        vstate[r] = L_NEEDED;
}

static inline void log_clobberreg(int r)
{
    if (vstate[r] == L_UNKNOWN)
        vstate[r] = L_UNNEEDED;
}

/* This ends all possibility of clever register allocation */

static inline void log_flush(void)
{
    int i;

    for (i = 0; i < VREGS; i++)
        if (vstate[i] == L_UNKNOWN)
            vstate[i] = L_NEEDED;
    for (i = 0; i < N_REGS; i++)
        if (nstate[i] == L_UNKNOWN)
            nstate[i] = L_UNAVAIL;
}

static inline void log_dump(void)
{
    return;
}

/********************************************************************
 * register status handling. EMIT TIME!                             *
 ********************************************************************/

static inline void set_status(int r, int status)
{
    if (status == ISCONST)
        log_clobberreg(r);
    live.state[r].status = status;
}

static inline int isinreg(int r)
{
    return live.state[r].status == CLEAN || live.state[r].status == DIRTY;
}

static void tomem(int r)
{
    int rr = live.state[r].realreg;

    if (live.state[r].status == DIRTY) {
#if defined(CPU_AARCH64)
        /* Convert FLAGX from JIT 0/1 format to interpreter bit-29 format.
           The JIT stores X as 0 or 1 (from CSET in DUPLICACTE_CARRY).
           The interpreter expects X at bit 29 (via COPY_CARRY/GET_XFLG).
           Use a work register to avoid modifying the live native register
           (which the hot path of the spcflags check may still need). */
        if (r == FLAGX) {
            LSL_wwi(REG_WORK2, rr, 29);
            compemu_raw_mov_l_mr((uintptr)live.state[r].mem, REG_WORK2);
            set_status(r, CLEAN);
            return;
        }
        /* PC_P holds a 64-bit host pointer — must use 64-bit store.
           compemu_raw_mov_l_mr uses 32-bit STR which truncates. */
        if (r == PC_P) {
            LOAD_U64(REG_WORK2, (uintptr)live.state[r].mem);
            STR_xXi(rr, REG_WORK2, 0);
            set_status(r, CLEAN);
            return;
        }
#endif
        compemu_raw_mov_l_mr((uintptr)live.state[r].mem, live.state[r].realreg);
        set_status(r, CLEAN);
    }
}

static inline int isconst(int r)
{
    return live.state[r].status == ISCONST;
}

int is_const(int r)
{
    return isconst(r);
}

static inline void writeback_const(int r)
{
    if (!isconst(r))
        return;
    Dif(live.state[r].needflush == NF_HANDLER) {
        jit_abort("Trying to write back constant NF_HANDLER!");
    }

    if (r == PC_P) {
        /* PC_P holds a 64-bit host pointer — use the dedicated 64-bit
           store path (LOAD_U64 + STR_xXi) instead of compemu_raw_mov_l_mi
           which truncates to 32 bits via its IM32 parameter. */
        compemu_raw_set_pc_i(live.state[r].val);
#if defined(CPU_AARCH64)
    } else if (r == FLAGX) {
        /* Convert from JIT 0/1 to interpreter bit-29 format */
        uae_u32 val = (live.state[r].val & 1) << 29;
        compemu_raw_mov_l_mi((uintptr)live.state[r].mem, val);
#endif
    } else {
        compemu_raw_mov_l_mi((uintptr)live.state[r].mem, live.state[r].val);
    }
    log_vwrite(r);
    live.state[r].val = 0;
    set_status(r, INMEM);
}

static inline void tomem_c(int r)
{
    if (isconst(r)) {
        writeback_const(r);
    }
    else
        tomem(r);
}

static void evict(int r)
{
    if (!isinreg(r))
        return;
    tomem(r);
    int rr = live.state[r].realreg;

    Dif(live.nat[rr].locked &&
        live.nat[rr].nholds == 1) {
#if defined(CPU_AARCH64)
        /* AArch64: force-unlock instead of aborting. Register sharing
           from mov_l_rr and similar functions can leave registers in
           a state where eviction is blocked by stale locks. */
        live.nat[rr].locked = 0;
#else
        jit_abort("register %d in nreg %d is locked!", r, live.state[r].realreg);
#endif
    }

    live.nat[rr].nholds--;
    if (live.nat[rr].nholds != live.state[r].realind) { /* Was not last */
        int topreg = live.nat[rr].holds[live.nat[rr].nholds];
        int thisind = live.state[r].realind;

        live.nat[rr].holds[thisind] = topreg;
        live.state[topreg].realind = thisind;
    }
    live.state[r].realreg = -1;
    set_status(r, INMEM);
}

static inline void free_nreg(int r)
{
    int i = live.nat[r].nholds;

    while (i) {
        int vr;

        --i;
        vr = live.nat[r].holds[i];
        evict(vr);
    }
    Dif(live.nat[r].nholds != 0) {
        jit_abort("Failed to free nreg %d, nholds is %d", r, live.nat[r].nholds);
    }
}

/* Use with care! */
static inline void isclean(int r)
{
    if (!isinreg(r))
        return;
    live.state[r].val = 0;
    set_status(r, CLEAN);
}

static inline void disassociate(int r)
{
    isclean(r);
    evict(r);
}

static inline void set_const(int r, uintptr val)
{
    disassociate(r);
#ifdef CPU_AARCH64
    // On ARM64, guest registers (Dn, An, flags) are 32-bit values.
    // Only PC_P holds a 64-bit host pointer.  Mask val to 32-bit for
    // all other registers to prevent 64-bit arithmetic leaking into
    // constant-propagation paths (sub_l_ri underflow, etc.).
    if (r != PC_P)
        val = (uae_u32)val;
#endif
    live.state[r].val = val;
    set_status(r, ISCONST);
}

static inline uae_u32 get_offset(int r)
{
    return live.state[r].val;
}

#ifdef AMIBERRY
bool has_free_reg(void)
{
    for (int i = N_REGS - 1; i >= 0; i--) {
        if (!live.nat[i].locked) {
            if (live.nat[i].nholds == 0)
                return true;
        }
    }
    return false;
}
#endif

static int alloc_reg_hinted(int r, int willclobber, int hint)
{
    int bestreg = -1;
    uae_s32 when = 2000000000;
    int i;

    for (i = N_REGS - 1; i >= 0; i--) {
        if (!live.nat[i].locked) {
            uae_s32 badness = live.nat[i].touched;
            if (live.nat[i].nholds == 0)
                badness = 0;
#if defined(CPU_AARCH64)
            /* Skip hw regs that have >1 virtual reg (shared).
               Evicting shared regs causes lock conflicts. Prefer empty
               or singly-held regs to avoid the issue entirely. */
            if (live.nat[i].nholds > 1)
                badness += 1000000000;
#endif
            if (i == hint)
                badness -= 200000000;
            if (badness < when) {
                bestreg = i;
                when = badness;
                if (live.nat[i].nholds == 0 && hint < 0)
                    break;
                if (i == hint)
                    break;
            }
        }
    }
    Dif(bestreg == -1)
        jit_abort("alloc_reg_hinted bestreg=-1");

    if (live.nat[bestreg].nholds > 0) {
        free_nreg(bestreg);
    }

    if (!willclobber) {
        if (live.state[r].status != UNDEF) {
            if (isconst(r)) {
                if (r == PC_P) {
                    /* PC_P holds a 64-bit host pointer — use LOAD_U64.
                       compemu_raw_mov_l_ri uses LOAD_U32 which truncates. */
                    LOAD_U64(bestreg, live.state[r].val);
                } else {
                    compemu_raw_mov_l_ri(bestreg, live.state[r].val);
                }
                live.state[r].val = 0;
                set_status(r, DIRTY);
                log_isused(bestreg);
            } else {
                do_load_reg(bestreg, r);
                set_status(r, CLEAN);
            }
        } else {
            live.state[r].val = 0;
            set_status(r, CLEAN);
            log_isused(bestreg);
        }
    } else { /* this is the easiest way, but not optimal. */
        live.state[r].val = 0;
        set_status(r, DIRTY);
    }
    live.state[r].realreg = bestreg;
    live.state[r].realind = 0;
    live.nat[bestreg].touched = touchcnt++;
    live.nat[bestreg].holds[0] = r;
    live.nat[bestreg].nholds = 1;

    return bestreg;
}


static void unlock2(int r)
{
    Dif(!live.nat[r].locked)
        jit_abort("unlock2 %d not locked", r);
    live.nat[r].locked--;
}

static void setlock(int r)
{
    live.nat[r].locked++;
}


static void mov_nregs(int d, int s)
{
    if (s == d)
        return;

    if (live.nat[d].nholds > 0)
        free_nreg(d);

    log_isused(d);
    compemu_raw_mov_l_rr(d, s);

    for (int i = 0; i < live.nat[s].nholds; i++) {
        int vs = live.nat[s].holds[i];

        live.state[vs].realreg = d;
        live.state[vs].realind = i;
        live.nat[d].holds[i] = vs;
    }
    live.nat[d].nholds = live.nat[s].nholds;

    live.nat[s].nholds = 0;
}


static inline void make_exclusive(int r, int needcopy)
{
    reg_status oldstate;
    int rr = live.state[r].realreg;
    int nr;
    int nind;

    if (!isinreg(r))
        return;
    if (live.nat[rr].nholds == 1)
        return;

    /* We have to split the register */
    oldstate = live.state[r];

    setlock(rr); /* Make sure this doesn't go away */
    /* Forget about r being in the register rr */
    disassociate(r);
    /* Get a new register, that we will clobber completely */
    nr = alloc_reg_hinted(r, 1, -1);
    nind = live.state[r].realind;
    live.state[r] = oldstate;   /* Keep all the old state info */
    live.state[r].realreg = nr;
    live.state[r].realind = nind;

    if (needcopy) {
        compemu_raw_mov_l_rr(nr, rr);  /* Make another copy */
    }
    unlock2(rr);
}

static inline int readreg_general(int r, int spec)
{
    int answer = -1;

    if (live.state[r].status == UNDEF) {
        jit_log("WARNING: Unexpected read of undefined register %d", r);
    }

    if (isinreg(r)) {
        answer = live.state[r].realreg;
    } else {
        /* the value is in memory to start with */
        answer = alloc_reg_hinted(r, 0, spec);
    }

    if (spec >= 0 && spec != answer) {
        /* Too bad */
        mov_nregs(spec, answer);
        answer = spec;
    }
    live.nat[answer].locked++;
    live.nat[answer].touched = touchcnt++;
    return answer;
}


static int readreg(int r)
{
    return readreg_general(r, -1);
}

static int readreg_specific(int r, int spec)
{
    return readreg_general(r, spec);
}

/* writereg(r)
 *
 * INPUT
 * - r    : mid-layer register
 *
 * OUTPUT
 * - hard (physical, x86 here) register allocated to virtual register r
 */
static int writereg(int r)
{
    int answer = -1;

    make_exclusive(r, 0);
    if (isinreg(r)) {
        answer = live.state[r].realreg;
    } else {
        /* the value is in memory to start with */
        answer = alloc_reg_hinted(r, 1, -1);
    }

    live.nat[answer].locked++;
    live.nat[answer].touched = touchcnt++;
    live.state[r].val = 0;
    set_status(r, DIRTY);
    return answer;
}

static int rmw(int r)
{
    int answer = -1;

    if (live.state[r].status == UNDEF) {
        jit_log("WARNING: Unexpected read of undefined register %d", r);
    }
    make_exclusive(r, 1);

    if (isinreg(r)) {
        answer = live.state[r].realreg;
    } else {
        /* the value is in memory to start with */
        answer = alloc_reg_hinted(r, 0, -1);
    }

    set_status(r, DIRTY);

    live.nat[answer].locked++;
    live.nat[answer].touched = touchcnt++;

    return answer;
}

/********************************************************************
 * FPU register status handling. EMIT TIME!                         *
 ********************************************************************/

static void f_tomem_drop(int r)
{
    if (live.fate[r].status == DIRTY) {
        compemu_raw_fmov_mr_drop((uintptr)live.fate[r].mem, live.fate[r].realreg);
        live.fate[r].status = INMEM;
    }
}


static int f_isinreg(int r)
{
    return live.fate[r].status == CLEAN || live.fate[r].status == DIRTY;
}

static void f_evict(int r)
{
    int rr;

    if (!f_isinreg(r))
        return;
    rr = live.fate[r].realreg;
    f_tomem_drop(r);

    live.fat[rr].nholds = 0;
    live.fate[r].status = INMEM;
    live.fate[r].realreg = -1;
}

static inline void f_free_nreg(int r)
{
    int vr = live.fat[r].holds;
    f_evict(vr);
}


/* Use with care! */
static inline void f_isclean(int r)
{
    if (!f_isinreg(r))
        return;
    live.fate[r].status = CLEAN;
}

static inline void f_disassociate(int r)
{
    f_isclean(r);
    f_evict(r);
}

static int f_alloc_reg(int r, int willclobber)
{
    int bestreg;

    if (r < 8)
        bestreg = r + 8;   // map real Amiga reg to ARM VFP reg 8-15 (call save)
    else if (r == FP_RESULT)
        bestreg = 6;         // map FP_RESULT to ARM VFP reg 6
    else // FS1
        bestreg = 7;         // map FS1 to ARM VFP reg 7

    if (!willclobber) {
        if (live.fate[r].status == INMEM) {
            compemu_raw_fmov_rm(bestreg, (uintptr)live.fate[r].mem);
            live.fate[r].status = CLEAN;
        }
    } else {
        live.fate[r].status = DIRTY;
    }
    live.fate[r].realreg = bestreg;
    live.fat[bestreg].holds = r;
    live.fat[bestreg].nholds = 1;

    return bestreg;
}

static void f_unlock(int r)
{
}

static inline int f_readreg(int r)
{
    int answer;

    if (f_isinreg(r)) {
        answer = live.fate[r].realreg;
    } else {
        /* the value is in memory to start with */
        answer = f_alloc_reg(r, 0);
    }

    return answer;
}

static inline int f_writereg(int r)
{
    int answer;

    if (f_isinreg(r)) {
        answer = live.fate[r].realreg;
    }  else {
        answer = f_alloc_reg(r, 1);
    }
    live.fate[r].status = DIRTY;
    return answer;
}

STATIC_INLINE int f_rmw(int r)
{
    int n;

    if (f_isinreg(r)) {
        n = live.fate[r].realreg;
    } else {
        n = f_alloc_reg(r, 0);
    }
    live.fate[r].status = DIRTY;
    return n;
}

static void fflags_into_flags_internal(void)
{
    int r = f_readreg(FP_RESULT);
    raw_fflags_into_flags(r);
    f_unlock(r);
    live_flags();
}

/********************************************************************
 * Support functions, internal                                      *
 ********************************************************************/

static inline int isinrom(uintptr addr)
{
#ifdef UAE
    if (addr >= (uintptr)kickmem_bank.baseaddr &&
        addr < (uintptr)kickmem_bank.baseaddr + 8 * 65536) {
        return 1;
    }
    /* Treat UAE Boot ROM (rtarea) as ROM too for ARM64 JIT safety guards. */
    if (rtarea_bank.baseaddr &&
        addr >= (uintptr)rtarea_bank.baseaddr &&
        addr < (uintptr)rtarea_bank.baseaddr + 65536) {
        return 1;
    }
    return 0;
#else
    return ((addr >= (uintptr)ROMBaseHost) && (addr < (uintptr)ROMBaseHost + ROMSize));
#endif
}

static void flush_all(void)
{
    int i;

    for (i = 0; i < VREGS; i++) {
        if (live.state[i].status == DIRTY) {
            if (!call_saved[live.state[i].realreg]) {
                tomem(i);
            }
        }
    }

    if (f_isinreg(FP_RESULT))
        f_evict(FP_RESULT);
    if (f_isinreg(FS1))
        f_evict(FS1);
}


/* Make sure all registers that will get clobbered by a call are
   save and sound in memory */
static void prepare_for_call_1(void)
{
    flush_all();  /* If there are registers that don't get clobbered,
                   * we should be a bit more selective here */
}

/* We will call a C routine in a moment. That will clobber all registers,
   so we need to disassociate everything */
static void prepare_for_call_2(void)
{
    int i;
    for (i = 0; i < N_REGS; i++) {
#if defined(CPU_AARCH64)
        if (live.nat[i].nholds > 0) // in aarch64: first 18 regs not call saved
#else
        if (!call_saved[i] && live.nat[i].nholds > 0)
#endif
            free_nreg(i);
    }

#ifdef USE_JIT_FPU
    for (i = 6; i <= 7; i++) // only FP_RESULT and FS1, FP0-FP7 are call save
        if (live.fat[i].nholds > 0)
            f_free_nreg(i);
#endif
    live.flags_in_flags = TRASH;  /* Note: We assume we already rescued the
                                     flags at the very start of the call_r functions! */
}

static void jit_runtime_orsr_word(uae_u32 src)
{
    if (!regs.s) {
        Exception(8, 0);
        return;
    }
    MakeSR();
    regs.sr |= (uae_u16)src;
    MakeFromSR();
    m68k_incpc(4);
}

static void jit_runtime_andsr_word(uae_u32 src)
{
    if (!regs.s) {
        Exception(8, 0);
        return;
    }
    MakeSR();
    regs.sr &= (uae_u16)src;
    MakeFromSR();
    m68k_incpc(4);
}

static void jit_runtime_eorsr_word(uae_u32 src)
{
    if (!regs.s) {
        Exception(8, 0);
        return;
    }
    MakeSR();
    regs.sr ^= (uae_u16)src;
    MakeFromSR();
    m68k_incpc(4);
}

static void jit_runtime_mv2sr_word_full(uae_u32 opcode)
{
    if (!regs.s) {
        Exception(8, 0);
        return;
    }

    uae_u32 real_opcode = cft_map(opcode);
    uae_u32 srcreg = real_opcode & 7;
    uaecptr srca;
    uae_u16 src;

    switch (real_opcode & 0x003f) {
    case 0x0000: /* Dn */
        src = (uae_u16)m68k_dreg(regs, srcreg);
        regs.sr = src;
        MakeFromSR();
        m68k_incpc(2);
        return;
    case 0x0010: /* (An) */
        srca = m68k_areg(regs, srcreg);
        src = get_word(srca);
        regs.sr = src;
        MakeFromSR();
        m68k_incpc(2);
        return;
    case 0x0018: /* (An)+ */
        srca = m68k_areg(regs, srcreg);
        src = get_word(srca);
        m68k_areg(regs, srcreg) += 2;
        regs.sr = src;
        MakeFromSR();
        m68k_incpc(2);
        return;
    case 0x0020: /* -(An) */
        srca = m68k_areg(regs, srcreg) - 2;
        src = get_word(srca);
        m68k_areg(regs, srcreg) = srca;
        regs.sr = src;
        MakeFromSR();
        m68k_incpc(2);
        return;
    case 0x0028: /* (d16,An) */
        srca = m68k_areg(regs, srcreg) + (uae_s32)(uae_s16)get_iword(2);
        src = get_word(srca);
        regs.sr = src;
        MakeFromSR();
        m68k_incpc(4);
        return;
    case 0x0030: /* (d8,An,Xn) */
        m68k_incpc(2);
        srca = get_disp_ea_020(m68k_areg(regs, srcreg), next_iword());
        src = get_word(srca);
        regs.sr = src;
        MakeFromSR();
        return;
    case 0x0038: /* extension modes */
        switch (srcreg) {
        case 0: /* (xxx).W */
            srca = (uae_s32)(uae_s16)get_iword(2);
            src = get_word(srca);
            regs.sr = src;
            MakeFromSR();
            m68k_incpc(4);
            return;
        case 1: /* (xxx).L */
            srca = get_ilong(2);
            src = get_word(srca);
            regs.sr = src;
            MakeFromSR();
            m68k_incpc(6);
            return;
        case 2: /* (d16,PC) */
            srca = m68k_getpc() + 2 + (uae_s32)(uae_s16)get_iword(2);
            src = get_word(srca);
            regs.sr = src;
            MakeFromSR();
            m68k_incpc(4);
            return;
        case 3: /* (d8,PC,Xn) */
            m68k_incpc(2);
            srca = get_disp_ea_020(m68k_getpc(), next_iword());
            src = get_word(srca);
            regs.sr = src;
            MakeFromSR();
            return;
        case 4: /* #<data>.W */
            src = (uae_u16)get_iword(2);
            regs.sr = src;
            MakeFromSR();
            m68k_incpc(4);
            return;
        default:
            op_illg(opcode);
            return;
        }
    default:
        op_illg(opcode);
        return;
    }
}

static void jit_runtime_move_l_d8anxn_absw(uae_u32 opcode)
{
    uae_u32 real_opcode = cft_map(opcode);
    uae_u32 srcreg = real_opcode & 7;

    m68k_incpc(2);
    uaecptr srca = get_disp_ea_020(m68k_areg(regs, srcreg), next_iword());
    uae_s32 src = get_long(srca);
    uaecptr dsta = (uae_s32)(uae_s16)get_iword(0);

    CLEAR_CZNV();
    SET_ZFLG(((uae_s32)(src)) == 0);
    SET_NFLG(((uae_s32)(src)) < 0);

    m68k_incpc(2);
    regs.fault_pc = m68k_getpc();
    put_long(dsta, src);
}

static void jit_runtime_movea_l_postinc_an(uae_u32 opcode)
{
    uae_u32 real_opcode = cft_map(opcode);
    uae_u32 srcreg = real_opcode & 7;
    uae_u32 dstreg = (real_opcode >> 9) & 7;

    uaecptr srca = m68k_areg(regs, srcreg);
    uae_s32 src = get_long(srca);
    m68k_areg(regs, srcreg) += 4;
    m68k_areg(regs, dstreg) = (uae_u32)src;
    m68k_incpc(2);
}

static void jit_runtime_move_l_reg_d16an(uae_u32 opcode)
{
    uae_u32 real_opcode = cft_map(opcode);
    uae_u32 srcreg = real_opcode & 7;
    uae_u32 dstreg = (real_opcode >> 9) & 7;
    uae_s32 src = table68k[cft_map(opcode)].smode == Dreg
        ? (uae_s32)m68k_dreg(regs, srcreg)
        : (uae_s32)m68k_areg(regs, srcreg);
    uaecptr dsta = m68k_areg(regs, dstreg) + (uae_s32)(uae_s16)get_iword(2);

    CLEAR_CZNV();
    SET_ZFLG(((uae_s32)(src)) == 0);
    SET_NFLG(((uae_s32)(src)) < 0);

    m68k_incpc(4);
    regs.fault_pc = m68k_getpc();
    put_long(dsta, src);
}

static void op_move_l_d8anxn_absw_comp_ff(uae_u32 opcode)
{
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_move_l_d8anxn_absw,
        (uintptr)(comp_pc_p + m68k_pc_offset_thisinst), opcode, 0, false);
}

static void op_move_l_reg_d16an_comp_ff(uae_u32 opcode)
{
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_move_l_reg_d16an,
        (uintptr)(comp_pc_p + m68k_pc_offset_thisinst), opcode, 0, false);
}

static void op_movea_l_postinc_an_comp_ff(uae_u32 opcode)
{
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_movea_l_postinc_an,
        (uintptr)(comp_pc_p + m68k_pc_offset_thisinst), opcode, 0, false);
}

static void jit_runtime_aline_trap(uae_u32 opcode)
{
    Exception(0xA, 0);
}

static void init_comp(void);  /* forward declaration */

static void op_emulop_comp_ff(uae_u32 opcode)
{
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    uae_u8 *this_pc_p = comp_pc_p + m68k_pc_offset_thisinst;
    uae_u32 this_m68k_pc = get_virtual_address(this_pc_p);

    /* Replicate the interpreter fallback path exactly:
       flush -> reset comp_pc_p -> init_comp -> PC triple -> call cpufunctbl -> end block */
    flush(1);
    comp_pc_p = this_pc_p;
    init_comp();

    compemu_raw_mov_l_ri(REG_PAR1, (uae_u32)cft_map(opcode));
    compemu_raw_mov_l_rr(REG_PAR2, R_REGSTRUCT);
    compemu_raw_mov_l_mi((uintptr)&regs.pc, this_m68k_pc);
    compemu_raw_set_pc_i((uintptr)this_pc_p);
    compemu_raw_mov_l_rm(REG_WORK1, (uintptr)&regs.pc_p);
    compemu_raw_mov_l_mr((uintptr)&regs.pc_oldp, REG_WORK1);
    compemu_raw_call((uintptr)cpufunctbl[cft_map(opcode)]);

    /* Advance past the 2-byte EMUL_OP opcode */
    compemu_raw_set_pc_i((uintptr)(this_pc_p + 2));
    live.state[PC_P].realreg = -1;
    live.state[PC_P].val = 0;
    set_status(PC_P, INMEM);
    jit_force_runtime_pc_endblock = true;
}

static void op_aline_trap_comp_ff(uae_u32 opcode)
{
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    /* A-line opcodes are trap control-flow, not local fallbacks. Execute the
       real trap helper at the live guest PC, then end the block from the
       helper-updated regs.pc_p so runtime resumes from the exception vector
       or trap-established return path without interpreter fallback. */
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_aline_trap,
        (uintptr)(comp_pc_p + m68k_pc_offset_thisinst), opcode, 0, false);
}

static inline void jit_emit_runtime_helper_barrier(uintptr helper, uintptr pc, uae_u32 arg1, uae_u32 arg2, bool has_arg2)
{
    flush(1);
    compemu_raw_mov_l_ri(REG_PAR1, arg1);
    if (has_arg2)
        compemu_raw_mov_l_ri(REG_PAR2, arg2);
    compemu_raw_set_pc_i(pc);
    compemu_raw_call(helper);
    live.state[PC_P].realreg = -1;
    live.state[PC_P].val = 0;
    set_status(PC_P, INMEM);
    jit_force_runtime_pc_endblock = true;
}

static void op_fullsr_orsr_w_comp_ff(uae_u32 opcode)
{
    (void)opcode;
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    m68k_pc_offset += 2;
    uae_u32 src = (uae_u32)(uae_u16)comp_get_iword((m68k_pc_offset += 2) - 2);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_orsr_word,
        (uintptr)(comp_pc_p + m68k_pc_offset_thisinst), src, 0, false);
}

static void op_fullsr_andsr_w_comp_ff(uae_u32 opcode)
{
    (void)opcode;
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    m68k_pc_offset += 2;
    uae_u32 src = (uae_u32)(uae_u16)comp_get_iword((m68k_pc_offset += 2) - 2);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_andsr_word,
        (uintptr)(comp_pc_p + m68k_pc_offset_thisinst), src, 0, false);
}

static void op_fullsr_eorsr_w_comp_ff(uae_u32 opcode)
{
    (void)opcode;
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    m68k_pc_offset += 2;
    uae_u32 src = (uae_u32)(uae_u16)comp_get_iword((m68k_pc_offset += 2) - 2);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_eorsr_word,
        (uintptr)(comp_pc_p + m68k_pc_offset_thisinst), src, 0, false);
}

static void op_fullsr_mv2sr_w_comp_ff(uae_u32 opcode)
{
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    /* Delegate exact EA semantics to the runtime helper. It advances PC on
       success and raises privilege/address exceptions with the correct live PC. */
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_mv2sr_word_full,
        (uintptr)(comp_pc_p + m68k_pc_offset_thisinst), opcode, 0, false);
}

#if defined(CPU_AARCH64) 
#include "compemu_midfunc_arm64.cpp"
#include "compemu_midfunc_arm64_2.cpp"
#elif defined(CPU_arm)
#include "compemu_midfunc_arm.cpp"
#include "compemu_midfunc_arm2.cpp"
#endif

/********************************************************************
 * Support functions exposed to gencomp. CREATE time                *
 ********************************************************************/

#ifdef __MACH__
void cache_invalidate(void) {
        // Invalidate cache on the JIT cache
        sys_icache_invalidate(popallspace, POPALLSPACE_SIZE + MAX_JIT_CACHE * 1024);
}
#endif

uintptr get_const(int r)
{
    Dif(!isconst(r)) {
        jit_abort("Register %d should be constant, but isn't", r);
    }
    return live.state[r].val;
}

uae_u8* compemu_host_pc_from_const(uintptr pc_const)
{
    return (uae_u8*)pc_const;
}

void sync_m68k_pc(void)
{
    if (m68k_pc_offset) {
        arm_ADD_l_ri(PC_P, m68k_pc_offset);
        comp_pc_p += m68k_pc_offset;
        m68k_pc_offset = 0;
    }
}

/********************************************************************
 * Support functions exposed to newcpu                              *
 ********************************************************************/

void compiler_init(void)
{
    static bool initialized = false;
    if (initialized)
        return;

    flush_icache = flush_icache_none;

    flush_icache = lazy_flush ? flush_icache_lazy : flush_icache_hard;

    for (int bank = 0; bank < 65536; ++bank) {
        baseaddr[bank] = (uae_u8 *)MEMBaseDiff;
        mem_banks[bank] = NULL;
    }

    initialized = true;

    // Allow test harness to force immediate native compilation
    jit_force_translate_check();

#ifdef PROFILE_UNTRANSLATED_INSNS
    jit_log("<JIT compiler> : gather statistics on untranslated insns count");
#endif

#ifdef PROFILE_COMPILE_TIME
    jit_log("<JIT compiler> : gather statistics on translation time");
    emul_start_time = clock();
#endif
}

void compiler_exit(void)
{

#ifdef PROFILE_COMPILE_TIME
    emul_end_time = clock();
#endif

#ifdef UAE
#else
#if DEBUG
#if defined(USE_DATA_BUFFER)
    jit_log("data_wasted = %ld bytes", data_wasted);
#endif
#endif

    // Deallocate translation cache
    if (compiled_code) {
#if defined(CPU_AARCH64)
        /* Don't free separately if part of the combined popallspace block */
        if (compiled_code != popall_combined_cache_start) {
            vm_release(compiled_code, cache_size * 1024);
        }
#else
        vm_release(compiled_code, cache_size * 1024);
#endif
        compiled_code = 0;
    }

    // Deallocate popallspace
    if (popallspace) {
#if defined(CPU_AARCH64)
        vm_release(popallspace, popall_combined_alloc_size ? popall_combined_alloc_size : POPALLSPACE_SIZE);
        popall_combined_alloc_size = 0;
        popall_combined_cache_start = NULL;
        popall_combined_cache_kb = 0;
#else
        vm_release(popallspace, POPALLSPACE_SIZE);
#endif
        popallspace = 0;
    }
#endif

#ifdef PROFILE_COMPILE_TIME
    jit_log("### Compile Block statistics");
    jit_log("Number of calls to compile_block : %d", compile_count);
    uae_u32 emul_time = emul_end_time - emul_start_time;
    jit_log("Total emulation time   : %.1f sec", double(emul_time) / double(CLOCKS_PER_SEC));
    jit_log("Total compilation time : %.1f sec (%.1f%%)", double(compile_time) / double(CLOCKS_PER_SEC), 100.0 * double(compile_time) / double(emul_time));
#endif

#ifdef PROFILE_UNTRANSLATED_INSNS
    uae_u64 untranslated_count = 0;
    for (int i = 0; i < 65536; i++) {
        opcode_nums[i] = i;
        untranslated_count += raw_cputbl_count[i];
    }
    bug("Sorting out untranslated instructions count, total %llu...\n", untranslated_count);
    qsort(opcode_nums, 65536, sizeof(uae_u16), untranslated_compfn);
    jit_log("Rank  Opc      Count Name\n");
    for (int i = 0; i < untranslated_top_ten; i++) {
        uae_u32 count = raw_cputbl_count[opcode_nums[i]];
        struct instr* dp;
        struct mnemolookup* lookup;
        if (!count)
            break;
        dp = table68k + opcode_nums[i];
        for (lookup = lookuptab; lookup->mnemo != (instrmnem)dp->mnemo; lookup++)
            ;
        bug(_T("%03d: %04x %10u %s\n"), i, opcode_nums[i], count, lookup->name);
    }
#endif

#ifdef RECORD_REGISTER_USAGE
    int reg_count_ids[16];
    uint64 tot_reg_count = 0;
    for (int i = 0; i < 16; i++) {
        reg_count_ids[i] = i;
        tot_reg_count += reg_count[i];
    }
    qsort(reg_count_ids, 16, sizeof(int), reg_count_compare);
    uint64 cum_reg_count = 0;
    for (int i = 0; i < 16; i++) {
        int r = reg_count_ids[i];
        cum_reg_count += reg_count[r];
        jit_log("%c%d : %16ld %2.1f%% [%2.1f]", r < 8 ? 'D' : 'A', r % 8,
            reg_count[r],
            100.0 * double(reg_count[r]) / double(tot_reg_count),
            100.0 * double(cum_reg_count) / double(tot_reg_count));
    }
#endif

	exit_table68k();
}

static void init_comp(void)
{
    int i;
    uae_s8* au = always_used;

    for (i = 0; i < VREGS; i++) {
        live.state[i].realreg = -1;
        live.state[i].val = 0;
        set_status(i, UNDEF);
    }
    for (i = 0; i < SCRATCH_REGS; ++i)
        live.scratch_in_use[i] = 0;

    for (i = 0; i < VFREGS; i++) {
        live.fate[i].status = UNDEF;
        live.fate[i].realreg = -1;
        live.fate[i].needflush = NF_SCRATCH;
    }

    for (i = 0; i < VREGS; i++) {
        if (i < 16) { /* First 16 registers map to 68k registers */
            live.state[i].mem = &regs.regs[i];
            set_status(i, INMEM);
        } else if (i >= S1) {
            live.state[i].mem = &regs.scratchregs[i - S1];
        }
    }
    live.state[PC_P].mem = (uae_u32*)&(regs.pc_p);
    set_const(PC_P, (uintptr)comp_pc_p);

    live.state[FLAGX].mem = (uae_u32*)&(regflags.x);
    set_status(FLAGX, INMEM);
  
    live.state[FLAGTMP].mem = (uae_u32*)&(regflags.nzcv);

    set_status(FLAGTMP, INMEM);
    flags_carry_inverted = false;


    for (i = 0; i < VFREGS; i++) {
        if (i < 8) { /* First 8 registers map to 68k FPU registers */
            live.fate[i].mem = (uae_u32*)fpu_register_address(i);
            live.fate[i].needflush = NF_TOMEM;
            live.fate[i].status = INMEM;
        } else if (i == FP_RESULT) {
            live.fate[i].mem = (uae_u32*)(&regs.fp_result);
            live.fate[i].needflush = NF_TOMEM;
            live.fate[i].status = INMEM;
        } else {
            live.fate[i].mem = (uae_u32*)(&regs.scratchfregs[i - 8]);
        }
    }

    for (i = 0; i < N_REGS; i++) {
        live.nat[i].touched = 0;
        live.nat[i].nholds = 0;
        live.nat[i].locked = 0;
        if (*au == i) {
            live.nat[i].locked = 1;
            au++;
        }
    }

    for (i = 0; i < N_FREGS; i++) {
        live.fat[i].nholds = 0;
    }

    touchcnt = 1;
    m68k_pc_offset = 0;
    live.flags_in_flags = TRASH;
    live.flags_on_stack = VALID;
    live.flags_are_important = 1;

    regs.jit_exception = 0;
}

/* Only do this if you really mean it! The next call should be to init!*/
static void flush(int save_regs)
{
    int i;

    log_flush();
    /* ARM64 cross-block fix: at block boundaries, ALWAYS store flags to memory.
       dont_care_flags() may have cleared flags_are_important mid-block when the
       liveflags analysis determined no downstream consumer WITHIN this block.
       But the NEXT block may need flags. Force important before flush_flags()
       so flags_to_stack() never skips the store at a block boundary. */
    if (save_regs)
        live.flags_are_important = 1;
    flush_flags(); /* low level */
    sync_m68k_pc(); /* mid level */

    if (save_regs) {
        for (i = 0; i < VFREGS; i++) {
            if (live.fate[i].needflush == NF_SCRATCH || live.fate[i].status == CLEAN) {
                f_disassociate(i);
            }
        }
        for (i = 0; i <= FLAGTMP; i++) {
            switch (live.state[i].status) {
            case INMEM:
                if (live.state[i].val) {
                    write_log("JIT: flush INMEM and val != 0!\n");
                }
                break;
            case CLEAN:
            case DIRTY:
                tomem(i);
                break;
            case ISCONST:
                if (i != PC_P)
                    writeback_const(i);
                break;
            default:
                break;
            }
        }
        for (i = 0; i <= FP_RESULT; i++) {
            if (live.fate[i].status == DIRTY) {
                f_evict(i);
            }
        }
    }
}

static void jit_emit_flush_delta_snapshot(void)
{
    for (int j = 0; j < 16; j++) {
        uintptr dst = (uintptr)&jit_flush_delta_state.regs[j];
        if ((live.state[j].status == DIRTY || live.state[j].status == CLEAN) && live.state[j].realreg >= 0) {
            compemu_raw_mov_l_mr(dst, live.state[j].realreg);
        } else if (live.state[j].status == ISCONST) {
            compemu_raw_mov_l_mi(dst, live.state[j].val);
        } else {
            compemu_raw_mov_l_rm(REG_WORK2, (uintptr)live.state[j].mem);
            compemu_raw_mov_l_mr(dst, REG_WORK2);
        }
    }

    if ((live.state[FLAGX].status == DIRTY || live.state[FLAGX].status == CLEAN) && live.state[FLAGX].realreg >= 0) {
        LSL_wwi(REG_WORK2, live.state[FLAGX].realreg, 29);
        compemu_raw_mov_l_mr((uintptr)&jit_flush_delta_state.flagx, REG_WORK2);
    } else if (live.state[FLAGX].status == ISCONST) {
        compemu_raw_mov_l_mi((uintptr)&jit_flush_delta_state.flagx, (live.state[FLAGX].val & 1) << 29);
    } else {
        compemu_raw_mov_l_rm(REG_WORK2, (uintptr)&regflags.x);
        compemu_raw_mov_l_mr((uintptr)&jit_flush_delta_state.flagx, REG_WORK2);
    }

    if (live.flags_in_flags == VALID) {
        MRS_NZCV_x(REG_WORK2);
        if (flags_carry_inverted)
            EOR_xxCflag(REG_WORK2, REG_WORK2);
        compemu_raw_mov_l_mr((uintptr)&jit_flush_delta_state.nzcv, REG_WORK2);
    } else {
        compemu_raw_mov_l_rm(REG_WORK2, (uintptr)&regflags.nzcv);
        compemu_raw_mov_l_mr((uintptr)&jit_flush_delta_state.nzcv, REG_WORK2);
    }

    compemu_raw_mov_l_rm(REG_WORK2, (uintptr)&regs.spcflags);
    compemu_raw_mov_l_mr((uintptr)&jit_flush_delta_state.spcflags, REG_WORK2);
    compemu_raw_mov_l_rm(REG_WORK2, (uintptr)&InterruptFlags);
    compemu_raw_mov_l_mr((uintptr)&jit_flush_delta_state.interrupt_flags, REG_WORK2);
    compemu_raw_mov_l_rm(REG_WORK2, (uintptr)&countdown);
    compemu_raw_mov_l_mr((uintptr)&jit_flush_delta_state.countdown, REG_WORK2);
}

int alloc_scratch(void)
{
    for (int i = 0; i < SCRATCH_REGS; ++i) {
        if (live.scratch_in_use[i] == 0) {
            live.scratch_in_use[i] = 1;
            return S1 + i;
        }
    }
    jit_log("Running out of scratch register.");
    abort();
}

void release_scratch(int i)
{
    if (i < S1 || i >= S1 + SCRATCH_REGS)
        jit_log("release_scratch(): %d is not a scratch reg.", i);
    if (live.scratch_in_use[i - S1]) {
        forget_about(i);
        live.scratch_in_use[i - S1] = 0;
    }
    else {
        jit_log("release_scratch(): %d not in use.", i);
    }
}

static void freescratch(void)
{
    int i;
    for (i = 0; i < N_REGS; i++) {
#if defined(CPU_AARCH64) 
        if (live.nat[i].locked && i > 5 && i < 18) {
#elif defined(CPU_arm)
        if (live.nat[i].locked && i != 2 && i != 3 && i != 10 && i != 11 && i != 12) {
#else
        if (live.nat[i].locked && i != 4 && i != 12) {
#endif
            live.nat[i].locked = 0;
            jit_log("Warning! %d is locked", i);
        }
    }

    for (i = S1; i < VREGS; i++)
        forget_about(i);
    for (i = 0; i < SCRATCH_REGS; ++i)
        live.scratch_in_use[i] = 0;

#ifdef USE_JIT_FPU
    f_forget_about(FS1);
#endif
}

/********************************************************************
 * Memory access and related functions, CREATE time                 *
 ********************************************************************/

void register_branch(uintptr not_taken, uintptr taken, uae_u8 cond)
{
    next_pc_p = not_taken;
    taken_pc_p = taken;
    branch_cc = cond;
}

void register_possible_exception(void)
{
    may_raise_exception = true;
}

/* Note: get_handler may fail in 64 Bit environments, if direct_handler_to_use is
 *       outside 32 bit
 */
static uintptr get_handler(uintptr addr)
{
    addr = jit_canonicalize_target_pc(addr);
    if (jit_force_execute_normal_successor_env() || jit_force_execute_normal_target_env(addr))
        return (uintptr)popall_execute_normal;
    blockinfo* bi = get_blockinfo_addr_new((void*)(uintptr)addr);
    uintptr h = (uintptr)(jit_force_nondirect_handler_env() ? bi->handler_to_use : bi->direct_handler_to_use);
    return h ? h : (uintptr)popall_execute_normal;
}

/* This version assumes that it is writing *real* memory, and *will* fail
 *  if that assumption is wrong! No branches, no second chances, just
 *  straight go-for-it attitude */

static void writemem_real(int address, int source, int size)
{
    if (currprefs.address_space_24) {
        switch (size) {
        case 1: jnf_MEM_WRITE24_OFF_b(address, source); break;
        case 2: jnf_MEM_WRITE24_OFF_w(address, source); break;
        case 4: jnf_MEM_WRITE24_OFF_l(address, source); break;
        }
    } else {
        switch (size) {
        case 1: jnf_MEM_WRITE_OFF_b(address, source); break;
        case 2: jnf_MEM_WRITE_OFF_w(address, source); break;
        case 4: jnf_MEM_WRITE_OFF_l(address, source); break;
        }
    }
}

static inline void writemem_special(int address, int source, int offset)
{
    jnf_MEM_WRITEMEMBANK(address, source, offset);
}

void writebyte(int address, int source)
{
    if (jit_force_all_special_mem() || (special_mem & S_WRITE) || distrust_byte() || jit_n_addr_unsafe || (jit_force_special_fb_writes() && jit_is_framebuffer_addr(address)))
        writemem_special(address, source, SIZEOF_VOID_P * 5);
    else
        writemem_real(address, source, 1);
}

void writeword(int address, int source)
{
    if (jit_force_all_special_mem() || (special_mem & S_WRITE) || distrust_word() || jit_n_addr_unsafe || (jit_force_special_fb_writes() && jit_is_framebuffer_addr(address)))
        writemem_special(address, source, SIZEOF_VOID_P * 4);
    else
        writemem_real(address, source, 2);
}

void writelong(int address, int source)
{
    if (jit_force_all_special_mem() || (special_mem & S_WRITE) || distrust_long() || jit_n_addr_unsafe || (jit_force_special_fb_writes() && jit_is_framebuffer_addr(address)))
        writemem_special(address, source, SIZEOF_VOID_P * 3);
    else
        writemem_real(address, source, 4);
}

// Now the same for clobber variant
void writeword_clobber(int address, int source)
{
    if (jit_force_all_special_mem() || (special_mem & S_WRITE) || distrust_word() || jit_n_addr_unsafe || (jit_force_special_fb_writes() && jit_is_framebuffer_addr(address)))
        writemem_special(address, source, SIZEOF_VOID_P * 4);
    else
        writemem_real(address, source, 2);
    forget_about(source);
}

void writelong_clobber(int address, int source)
{
    if (jit_force_all_special_mem() || (special_mem & S_WRITE) || distrust_long() || jit_n_addr_unsafe || (jit_force_special_fb_writes() && jit_is_framebuffer_addr(address)))
        writemem_special(address, source, SIZEOF_VOID_P * 3);
    else
        writemem_real(address, source, 4);
    forget_about(source);
}


/* This version assumes that it is reading *real* memory, and *will* fail
 *  if that assumption is wrong! No branches, no second chances, just
 *  straight go-for-it attitude */

static void readmem_real(int address, int dest, int size)
{
    if (currprefs.address_space_24) {
        switch (size) {
        case 1: jnf_MEM_READ24_OFF_b(dest, address); break;
        case 2: jnf_MEM_READ24_OFF_w(dest, address); break;
        case 4: jnf_MEM_READ24_OFF_l(dest, address); break;
        }
    } else {
        switch (size) {
        case 1: jnf_MEM_READ_OFF_b(dest, address); break;
        case 2: jnf_MEM_READ_OFF_w(dest, address); break;
        case 4: jnf_MEM_READ_OFF_l(dest, address); break;
        }
    }
}

static inline void readmem_special(int address, int dest, int offset)
{
    jnf_MEM_READMEMBANK(dest, address, offset);
}

void readbyte(int address, int dest)
{
    if (jit_force_all_special_mem() || (special_mem & S_READ) || distrust_byte() || jit_n_addr_unsafe)
        readmem_special(address, dest, SIZEOF_VOID_P * 2);
    else
        readmem_real(address, dest, 1);
}

void readword(int address, int dest)
{
    if (jit_force_all_special_mem() || (special_mem & S_READ) || distrust_word() || jit_n_addr_unsafe)
        readmem_special(address, dest, SIZEOF_VOID_P * 1);
    else
        readmem_real(address, dest, 2);
}

void readlong(int address, int dest)
{
    if (jit_force_all_special_mem() || (special_mem & S_READ) || distrust_long() || jit_n_addr_unsafe)
        readmem_special(address, dest, SIZEOF_VOID_P * 0);
    else
        readmem_real(address, dest, 4);
}

/* This one might appear a bit odd... */
STATIC_INLINE void get_n_addr_old(int address, int dest)
{
    readmem_special(address, dest, SIZEOF_VOID_P * 6);
}

STATIC_INLINE void get_n_addr_real(int address, int dest)
{
    if (currprefs.address_space_24)
        jnf_MEM_GETADR24_OFF(dest, address);
    else
        jnf_MEM_GETADR_OFF(dest, address);
}

void get_n_addr(int address, int dest)
{
    if (special_mem || distrust_addr() || jit_n_addr_unsafe)
        get_n_addr_old(address, dest);
    else
        get_n_addr_real(address, dest);
}

void get_n_addr_jmp(int address, int dest)
{
    /* For this, we need to get the same address as the rest of UAE
       would --- otherwise we end up translating everything twice */
    if (special_mem || distrust_addr() || jit_n_addr_unsafe)
        get_n_addr_old(address, dest);
    else
        jnf_MEM_GETADR_JMP_OFF(dest, address);
}

/* base is a register, but dp is an actual value. 
   target is a register */
void calc_disp_ea_020(int base, uae_u32 dp, int target)
{
    int reg = (dp >> 12) & 15;
    int regd_shift = (dp >> 9) & 3;

    if (dp & 0x100) {
        int ignorebase = (dp & 0x80);
        int ignorereg = (dp & 0x40);
        int addbase = 0;
        int outer = 0;

        if ((dp & 0x30) == 0x20)
            addbase = (uae_s32)(uae_s16)comp_get_iword((m68k_pc_offset += 2) - 2);
        if ((dp & 0x30) == 0x30)
            addbase = comp_get_ilong((m68k_pc_offset += 4) - 4);

        if ((dp & 0x3) == 0x2)
            outer = (uae_s32)(uae_s16)comp_get_iword((m68k_pc_offset += 2) - 2);
        if ((dp & 0x3) == 0x3)
            outer = comp_get_ilong((m68k_pc_offset += 4) - 4);

        if ((dp & 0x4) == 0) {  /* add regd *before* the get_long */
            if (!ignorereg) {
                disp_ea20_target_mov(target, reg, regd_shift, ((dp & 0x800) == 0));
            } else {
                mov_l_ri(target, 0);
            }

            /* target is now regd */
            if (!ignorebase)
                arm_ADD_l(target, base);
            arm_ADD_l_ri(target, addbase);
            if (dp & 0x03)
                readlong(target, target);
        } else { /* do the getlong first, then add regd */
            if (!ignorebase) {
                mov_l_rr(target, base);
                arm_ADD_l_ri(target, addbase);
            } else {
                mov_l_ri(target, addbase);
            }
            if (dp & 0x03)
                readlong(target, target);

            if (!ignorereg) {
                disp_ea20_target_add(target, reg, regd_shift, ((dp & 0x800) == 0));
            }
        }
        arm_ADD_l_ri(target, outer);
    } else { /* 68000 version */
        if ((dp & 0x800) == 0) { /* Sign extend */
            sign_extend_16_rr(target, reg);
            lea_l_brr_indexed(target, base, target, 1 << regd_shift, (uae_s8)dp);
        } else {
            lea_l_brr_indexed(target, base, reg, 1 << regd_shift, (uae_s8)dp);
        }
    }
}

void set_cache_state(int enabled)
{
    if (enabled != cache_enabled)
        flush_icache_hard(3);
    cache_enabled = enabled;
}

int get_cache_state(void)
{
	return cache_enabled;
}

uae_u32 get_jitted_size(void)
{
	if (compiled_code)
		return JITPTR current_compile_p - JITPTR compiled_code;
	return 0;
}

static uint8 *do_alloc_code(uint32 size, int depth)
{
	UNUSED(depth);
	uint8* code = (uint8 *)vm_acquire_code(size, VM_MAP_DEFAULT | VM_MAP_32BIT);
	return code == VM_MAP_FAILED ? NULL : code;
}

static inline uint8 *alloc_code(uint32 size)
{
    uint8 *ptr = do_alloc_code(size, 0);
	/* allocated code must fit in 32-bit boundaries */
#ifdef CPU_64_BIT
#if defined(CPU_AARCH64)
	/* ARM64 JIT is 64-bit pointer clean — tolerate high addresses. */
	if (ptr && (uintptr)ptr + size > (uintptr)0xffffffff) {
		static bool arm64_high_jit_logged = false;
		if (!arm64_high_jit_logged) {
			jit_log("ARM64: JIT code allocated above 32-bit boundary at %p (size %u)", ptr, size);
			arm64_high_jit_logged = true;
		}
	}
#else
	if (ptr && (uintptr)ptr + size > (uintptr)0xffffffff) {
		jit_log("WARNING: JIT code allocated above 32-bit boundary at %p (size %u)", ptr, size);
		vm_release(ptr, size);
		return NULL;
	}
#endif
#endif
	return ptr;
}

#if defined(CPU_AARCH64)
static inline bool arm64_uncond_branch_reachable(uintptr from, uintptr to)
{
	/* AArch64 B immediate range: signed 26-bit immediate, shifted left by 2. */
	const intptr_t diff = (intptr_t)to - (intptr_t)from;
	const intptr_t min = -(128 * 1024 * 1024);
	const intptr_t max = (128 * 1024 * 1024) - 4;
	return diff >= min && diff <= max;
}

static inline bool arm64_cache_reaches_popall(uint8 *cache_start, uint32 cache_size_bytes)
{
	if (!cache_start || !cache_size_bytes || !popallspace) {
		return false;
	}
	const uintptr popall = (uintptr)popallspace;
	const uintptr start = (uintptr)cache_start;
	const uintptr end = start + cache_size_bytes - 4;
	return arm64_uncond_branch_reachable(start, popall) &&
		arm64_uncond_branch_reachable(end, popall);
}

#if defined(__APPLE__)
static uint8 *alloc_code_near_popall(uint32 size)
{
	if (!popallspace || size == 0) {
		return alloc_code(size);
	}
#ifdef MAP_JIT
	const int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
	const int flags = MAP_PRIVATE | MAP_ANON | MAP_JIT;
	const uintptr page = (uintptr)uae_vm_page_size();
	const uintptr anchor = (uintptr)popallspace;
	const intptr_t max_delta = 120 * 1024 * 1024;
	const intptr_t step = 4 * 1024 * 1024;

	for (intptr_t delta = 0; delta <= max_delta; delta += step) {
		for (int dir = 0; dir < 2; dir++) {
			if (delta == 0 && dir == 1) {
				continue;
			}
			const intptr_t signed_delta = dir == 0 ? delta : -delta;
			uintptr hint = (uintptr)((intptr_t)anchor + signed_delta);
			hint &= ~(page - 1);
			void *p = mmap((void *)hint, size, prot, flags, -1, 0);
			if (p == MAP_FAILED) {
				continue;
			}
			uint8 *code = (uint8 *)p;
			if (arm64_cache_reaches_popall(code, size)) {
				return code;
			}
			munmap(code, size);
		}
	}
#endif
	/* Fallback allocation may place cache out of branch range, checked by caller. */
	return alloc_code(size);
}
#endif /* __APPLE__ */
#endif /* CPU_AARCH64 */

void alloc_cache(void)
{
    if (compiled_code) {
        flush_icache_hard(3);
#if defined(CPU_AARCH64)
        /* Don't free if it's part of the combined popallspace block */
        if (compiled_code != popall_combined_cache_start) {
            vm_release(compiled_code, cache_size * 1024);
        }
#else
		vm_release(compiled_code, cache_size * 1024);
#endif
        compiled_code = 0;
    }

    cache_size = currprefs.cachesize;
    if (cache_size == 0)
        return;

#if defined(CPU_AARCH64)
	/* Use pre-allocated cache from the combined popallspace block if available */
	if (popall_combined_cache_start && (uint32)cache_size <= popall_combined_cache_kb) {
		compiled_code = popall_combined_cache_start;
	} else {
		/* Fall back to separate allocation */
		while (!compiled_code && cache_size) {
			const uint32 cache_bytes = cache_size * 1024;
#if defined(__APPLE__)
			compiled_code = alloc_code_near_popall(cache_bytes);
#else
			compiled_code = alloc_code(cache_bytes);
#endif
			if (compiled_code && !arm64_cache_reaches_popall(compiled_code, cache_bytes)) {
				jit_log("ARM64: JIT cache %p (size %u) is out of branch range from popallspace %p",
					compiled_code, cache_bytes, popallspace);
				vm_release(compiled_code, cache_bytes);
				compiled_code = NULL;
			}
			if (compiled_code == NULL) {
				compiled_code = 0;
				cache_size /= 2;
			}
		}
	}
#else
	while (!compiled_code && cache_size) {
		const uint32 cache_bytes = cache_size * 1024;
		compiled_code = alloc_code(cache_bytes);
		if (compiled_code == NULL) {
			compiled_code = 0;
			cache_size /= 2;
		}
	}
	if (compiled_code) {
		if (!vm_protect(compiled_code, cache_size * 1024, VM_PAGE_READ | VM_PAGE_WRITE | VM_PAGE_EXECUTE)) {
			jit_log("WARNING: Failed to set JIT cache to RWX — JIT may crash");
		}
	}
#endif

    if (compiled_code) {
#if defined(CPU_AARCH64)
		static bool arm64_jit_mode_logged = false;
		if (!arm64_jit_mode_logged) {
#if defined(__APPLE__)
			jit_log("ARM64 macOS JIT mode: MAP_JIT + write/execute switching");
#elif defined(__ANDROID__)
			jit_log("ARM64 Android JIT mode: RWX anonymous mapping");
#else
			jit_log("ARM64 JIT mode: RWX anonymous mapping");
#endif
			arm64_jit_mode_logged = true;
		}
#endif
        jit_log("<JIT compiler> : actual translation cache size : %d KB at %p-%p\n", cache_size, compiled_code, compiled_code + cache_size * 1024);
#ifdef USE_DATA_BUFFER
        max_compile_start = compiled_code + cache_size * 1024 - BYTES_PER_INST - DATA_BUFFER_SIZE;
#else
        max_compile_start = compiled_code + cache_size * 1024 - BYTES_PER_INST;
#endif
        current_compile_p = compiled_code;
        current_cache_size = 0;
#if defined(USE_DATA_BUFFER)
        reset_data_buffer();
#endif
    }
}

static void calc_checksum(blockinfo* bi, uae_u32* c1, uae_u32* c2)
{
    uae_u32 k1 = 0;
    uae_u32 k2 = 0;

    checksum_info* csi = bi->csi;
    Dif(!csi) abort();
    while (csi) {
        uae_s32 len = csi->length;
        uintptr tmp = (uintptr)csi->start_p;
        uae_u32* pos;

        len += (tmp & 3);
        tmp &= ~((uintptr)3);
        pos = (uae_u32*)tmp;

        if (len >= 0 && len <= MAX_CHECKSUM_LEN) {
            while (len > 0) {
                k1 += *pos;
                k2 ^= *pos;
                pos++;
                len -= 4;
            }
        }

        csi = csi->next;
    }

    *c1 = k1;
    *c2 = k2;
}

int check_for_cache_miss(void)
{
#if defined(CPU_AARCH64)
    /* Guard against corrupted pc_p. Valid pc_p must point into the
       Mac memory region (RAM + ROM). Compiled endblock code can write
       a truncated or garbage value to regs.pc_p. Detect and recover
       by re-deriving pc_p from pc_oldp + m68k PC. */
    {
        uintptr pcp = (uintptr)regs.pc_p;
        uintptr base = (uintptr)RAMBaseHost;
        uintptr limit = base + RAMSize + ROMSize + 0x1000000; /* ROM + NuBus slot ROM space */
        if (pcp < base || pcp >= limit || (pcp & 1)) {
            static int bad_count = 0;
            uae_u32 safe_pc = regs.pc & ~1u;
            if (bad_count++ < 50)
                fprintf(stderr, "JIT: bad pc_p=%p regs.pc=%08x safe=%08x "
                    "d0=%08x d1=%08x a0=%08x a7=%08x sr=%04x spc=%08x oldp=%p last_setpc=%p last_kind=%u last_seq=%lu\n",
                    (void*)regs.pc_p, regs.pc, safe_pc,
                    regs.regs[0], regs.regs[1], regs.regs[8], regs.regs[15],
                    (unsigned)regs.sr, (unsigned)regs.spcflags, (void*)regs.pc_oldp,
                    (void*)jit_last_setpc_value, (unsigned)jit_last_setpc_kind, jit_last_setpc_seq);
            flush_icache_hard(7);
            regs.pc = safe_pc;
            regs.pc_p = get_real_address(safe_pc, 0, sz_word);
            regs.pc_oldp = regs.pc_p - safe_pc;
            /* If the derived pc_p is also outside valid range, let caller
               fall back to interpreter rather than flush-looping. */
            pcp = (uintptr)regs.pc_p;
            if (pcp < base || pcp >= limit || (pcp & 1))
                return 1; /* signal caller to use interpreter */
            return 0;
        }
    }
#endif
    blockinfo* bi = get_blockinfo_addr(regs.pc_p);

    if (bi) {
        int cl = cacheline(regs.pc_p);
        if (bi != cache_tags[cl + 1].bi) {
            raise_in_cl_list(bi);
#if defined(CPU_AARCH64)
            jit_diag_execute_normal_cache_hit++;
#endif
            return 1;
        }
    }
    return 0;
}


static void recompile_block(void)
{
#if defined(CPU_AARCH64)
    jit_diag_recompile_block_calls++;
    jit_diag_dispatch_count++;
    jit_diag_maybe_print();
    if ((uintptr)regs.pc_p & 1) {
        execute_normal();
        return;
    }
#endif
#ifdef JIT_DEBUG_MEM_CORRUPTION
    jit_dbg_check_vec2_dispatch("recompile_block");
#endif
    /* An existing block's countdown code has expired. We need to make
       sure that execute_normal doesn't refuse to recompile due to a
       perceived cache miss... */
    blockinfo* bi = get_blockinfo_addr(regs.pc_p);

    Dif(!bi)
        jit_abort("recompile_block");
    raise_in_cl_list(bi);
    execute_normal();
}

static void cache_miss(void)
{
#if defined(CPU_AARCH64)
    jit_diag_cache_miss_calls++;
    jit_diag_dispatch_count++;
    jit_diag_maybe_print();
#endif
#ifdef JIT_DEBUG_MEM_CORRUPTION
    jit_dbg_check_vec2_dispatch("cache_miss");
#endif
    blockinfo* bi = get_blockinfo_addr(regs.pc_p);
#if COMP_DEBUG
    uae_u32     cl = cacheline(regs.pc_p);
    blockinfo* bi2 = get_blockinfo(cl);
#endif

    if (!bi) {
        execute_normal(); /* Compile this block now */
        return;
    }
#if COMP_DEBUG
    if (!bi2 || bi == bi2) {
        jit_abort("Unexplained cache miss %p %p", bi, bi2);
    }
#endif
    raise_in_cl_list(bi);
}

static int called_check_checksum(blockinfo* bi);

static inline int block_check_checksum(blockinfo* bi)
{
    uae_u32 c1, c2;
    int isgood;

    if (bi->status != BI_NEED_CHECK)
        return 1;  /* This block is in a checked state */

    if (bi->c1 || bi->c2)
        calc_checksum(bi, &c1, &c2);
    else
        c1 = c2 = 1;  /* Make sure it doesn't match */

    isgood = (c1 == bi->c1 && c2 == bi->c2);

    if (isgood) {
        /* This block is still OK. So we reactivate. Of course, that
           means we have to move it into the needs-to-be-flushed list */
        bi->handler_to_use = bi->handler;
        set_dhtu(bi, (jit_force_nondirect_handler_env() || jit_force_nondirect_target_env((uintptr)bi->pc_p)) ? bi->handler : bi->direct_handler);
        bi->status = BI_CHECKING;
        isgood = called_check_checksum(bi) != 0;
    }
    jit_diag_note_checksum_result(isgood != 0);
    if (isgood) {
        jit_log2("reactivate %p/%p (%x %x/%x %x)", bi, bi->pc_p, c1, c2, bi->c1, bi->c2);
        remove_from_list(bi);
        add_to_active(bi);
        raise_in_cl_list(bi);
        bi->status = BI_ACTIVE;
    } else {
        /* This block actually changed. We need to invalidate it,
           and set it up to be recompiled */
        jit_log2("discard %p/%p (%x %x/%x %x)", bi, bi->pc_p, c1, c2, bi->c1, bi->c2);
        invalidate_block(bi);
        raise_in_cl_list(bi);
    }
    return isgood;
}

static int called_check_checksum(blockinfo* bi)
{
    int isgood = 1;
    int i;

    for (i = 0; i < 2 && isgood; i++) {
        if (bi->dep[i].jmp_off) {
            isgood = block_check_checksum(bi->dep[i].target);
        }
    }
    return isgood;
}

static void check_checksum(void)
{
#if defined(CPU_AARCH64)
    jit_diag_check_checksum_calls++;
    jit_diag_dispatch_count++;
    jit_diag_maybe_print();
#endif
    blockinfo* bi = get_blockinfo_addr(regs.pc_p);
    uae_u32 cl = cacheline(regs.pc_p);
    blockinfo* bi2 = get_blockinfo(cl);

    /* These are not the droids you are looking for...  */
    if (!bi) {
        /* Whoever is the primary target is in a dormant state, but
           calling it was accidental, and we should just compile this
           new block */
        execute_normal();
        return;
    }
    if (bi != bi2) {
        /* The block was hit accidentally, but it does exist. Cache miss */
        cache_miss();
        return;
    }

    if (!block_check_checksum(bi))
        execute_normal();
}

STATIC_INLINE void match_states(blockinfo* bi)
{
    if (bi->status == BI_NEED_CHECK) {
        block_check_checksum(bi);
    }
}

STATIC_INLINE void create_popalls(void)
{
    int i, r;

    if (popallspace == NULL) {
#if defined(CPU_AARCH64)
        /* On ARM64, allocate popallspace + JIT cache as one contiguous block
         * to guarantee the cache is within B/BL branch range (+-128 MB). */
        const uint32 cache_kb = currprefs.cachesize > 0 ? currprefs.cachesize : MAX_JIT_CACHE;
        const size_t cache_bytes = (size_t)cache_kb * 1024;
        const size_t combined_size = POPALLSPACE_SIZE + cache_bytes;
        popallspace = alloc_code(combined_size);
        if (popallspace) {
            popall_combined_alloc_size = combined_size;
            popall_combined_cache_start = popallspace + POPALLSPACE_SIZE;
            popall_combined_cache_kb = cache_kb;
            jit_log("ARM64: combined popallspace+cache allocation at %p (%u KB cache)",
                popallspace, cache_kb);
        } else {
            /* Fall back to popallspace-only allocation */
            popall_combined_alloc_size = 0;
            popall_combined_cache_start = NULL;
            popall_combined_cache_kb = 0;
            popallspace = alloc_code(POPALLSPACE_SIZE);
        }
        if (popallspace == NULL) {
#else
        if ((popallspace = alloc_code(POPALLSPACE_SIZE)) == NULL) {
#endif
            jit_log("WARNING: Could not allocate popallspace!");
            if (currprefs.cachesize > 0)
            {
                jit_abort("Could not allocate popallspace!");
            }
            /* This is not fatal if JIT is not used. If JIT is
             * turned on, it will crash, but it would have crashed
             * anyway. */
            return;
        }
    }
#if !defined(CPU_AARCH64)
    /* On non-ARM64, code memory was allocated RW-only; this is a no-op there.
     * On ARM64, code memory is already RWX from allocation — skip. */
    vm_protect(popallspace, POPALLSPACE_SIZE, VM_PAGE_READ | VM_PAGE_WRITE);
#endif
	jit_begin_write_window();

    current_compile_p = popallspace;
    set_target(current_compile_p);

#if defined(CPU_arm) && !defined(ARMV6T2) && !defined(CPU_AARCH64)
    reset_data_buffer();
    data_long(0, 0); // Make sure we emit the branch over the first buffer outside pushall_call_handler
#endif

  /* We need to guarantee 16-byte stack alignment on x86 at any point
     within the JIT generated code. We have multiple exit points
     possible but a single entry. A "jmp" is used so that we don't
     have to generate stack alignment in generated code that has to
     call external functions (e.g. a generic instruction handler).

     In summary, JIT generated code is not leaf so we have to deal
     with it here to maintain correct stack alignment. */
    current_compile_p = get_target();
    pushall_call_handler = get_target();
    raw_push_regs_to_preserve();
#ifdef JIT_DEBUG
    write_log("Address of regs: 0x%016x, regs.pc_p: 0x%016x\n", &regs, &regs.pc_p);
    write_log("Address of natmem_offset: 0x%016x, natmem_offset = 0x%016x\n", &natmem_offset, natmem_offset);
    write_log("Address of cache_tags: 0x%016x\n", cache_tags);
#endif
    compemu_raw_init_r_regstruct((uintptr)&regs);
    compemu_raw_jmp_pc_tag();

    /* now the exit points */
    popall_execute_normal_setpc = get_target();
    uintptr idx = (uintptr)&(regs.pc_p) - (uintptr)&regs;
#if defined(CPU_AARCH64)
    if (jit_trace_setpc_env()) {
        STR_xXpre(REG_WORK1, RSP_INDEX, -16);
        MOV_xx(REG_PAR1, REG_WORK1);
        LOAD_U32(REG_PAR2, 1);
        compemu_raw_call((uintptr)jit_trace_setpc_value);
        LDR_xXpost(REG_WORK1, RSP_INDEX, 16);
    }
    {
        const uintptr idx_pc = (uintptr)&(regs.pc) - (uintptr)&regs;
        const uintptr idx_oldp = (uintptr)&(regs.pc_oldp) - (uintptr)&regs;
        /* Match m68k_setpc(): pc_p = pc_oldp = get_real_address(newpc), pc = newpc.
           REG_WORK1 already holds the host PC for the target block. */
        STR_xXi(REG_WORK1, R_REGSTRUCT, idx);
        LOAD_U64(REG_WORK2, (uintptr)&MEMBaseDiff);
        LDR_xXi(REG_WORK2, REG_WORK2, 0);
        SUB_xxx(REG_WORK3, REG_WORK1, REG_WORK2);
        STR_wXi(REG_WORK3, R_REGSTRUCT, idx_pc);
        STR_xXi(REG_WORK1, R_REGSTRUCT, idx_oldp);
    }
#else
    STR_rRI(REG_WORK1, R_REGSTRUCT, idx);
#endif
    popall_execute_normal = get_target();
    /* No fast dispatch for now - just the slow path */
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)execute_normal);

    fprintf(stderr, "JIT: popall at %p\n", popall_execute_normal);
    fflush(stderr);

    popall_check_checksum_setpc = get_target();
#if defined(CPU_AARCH64)
    if (jit_trace_setpc_env()) {
        STR_xXpre(REG_WORK1, RSP_INDEX, -16);
        MOV_xx(REG_PAR1, REG_WORK1);
        LOAD_U32(REG_PAR2, 2);
        compemu_raw_call((uintptr)jit_trace_setpc_value);
        LDR_xXpost(REG_WORK1, RSP_INDEX, 16);
    }
    {
        const uintptr idx_pc = (uintptr)&(regs.pc) - (uintptr)&regs;
        const uintptr idx_oldp = (uintptr)&(regs.pc_oldp) - (uintptr)&regs;
        STR_xXi(REG_WORK1, R_REGSTRUCT, idx);
        LOAD_U64(REG_WORK2, (uintptr)&MEMBaseDiff);
        LDR_xXi(REG_WORK2, REG_WORK2, 0);
        SUB_xxx(REG_WORK3, REG_WORK1, REG_WORK2);
        STR_wXi(REG_WORK3, R_REGSTRUCT, idx_pc);
        STR_xXi(REG_WORK1, R_REGSTRUCT, idx_oldp);
    }
#else
    STR_rRI(REG_WORK1, R_REGSTRUCT, idx);
#endif
    popall_check_checksum = get_target();
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)check_checksum);

    popall_exec_nostats_setpc = get_target();
#if defined(CPU_AARCH64)
    if (jit_trace_setpc_env()) {
        STR_xXpre(REG_WORK1, RSP_INDEX, -16);
        MOV_xx(REG_PAR1, REG_WORK1);
        LOAD_U32(REG_PAR2, 3);
        compemu_raw_call((uintptr)jit_trace_setpc_value);
        LDR_xXpost(REG_WORK1, RSP_INDEX, 16);
    }
    {
        const uintptr idx_pc = (uintptr)&(regs.pc) - (uintptr)&regs;
        const uintptr idx_oldp = (uintptr)&(regs.pc_oldp) - (uintptr)&regs;
        STR_xXi(REG_WORK1, R_REGSTRUCT, idx);
        LOAD_U64(REG_WORK2, (uintptr)&MEMBaseDiff);
        LDR_xXi(REG_WORK2, REG_WORK2, 0);
        SUB_xxx(REG_WORK3, REG_WORK1, REG_WORK2);
        STR_wXi(REG_WORK3, R_REGSTRUCT, idx_pc);
        STR_xXi(REG_WORK1, R_REGSTRUCT, idx_oldp);
    }
#else
    STR_rRI(REG_WORK1, R_REGSTRUCT, idx);
#endif
    popall_exec_nostats = get_target();
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)exec_nostats);

    popall_recompile_block = get_target();
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)recompile_block);

    popall_do_nothing = get_target();
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)do_nothing);

    popall_cache_miss = get_target();
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)cache_miss);

    popall_execute_exception = get_target();
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)execute_exception);

#if defined(USE_DATA_BUFFER)
    reset_data_buffer();
#endif

    // no need to further write into popallspace
    {
        uae_u8 *end = (uae_u8*)get_target();
        size_t popall_code_size = end - popallspace;
        fprintf(stderr, "POPALL_DUMP size=%lu\n", (unsigned long)popall_code_size);
        fprintf(stderr, "  pushall=%p exec_norm_setpc=%p exec_norm=%p\n",
            pushall_call_handler, popall_execute_normal_setpc, popall_execute_normal);
        fprintf(stderr, "  chk_setpc=%p chk=%p nostats_setpc=%p nostats=%p\n",
            popall_check_checksum_setpc, popall_check_checksum,
            popall_exec_nostats_setpc, popall_exec_nostats);
        fprintf(stderr, "  recomp=%p do_nothing=%p cache_miss=%p exception=%p\n",
            popall_recompile_block, popall_do_nothing, popall_cache_miss, popall_execute_exception);
        uae_u32 *p = (uae_u32*)popallspace;
        for (size_t i = 0; i < popall_code_size && i < 512; i += 4) {
            fprintf(stderr, "  [%04lx] %08x\n", (unsigned long)i, *p++);
        }
        fflush(stderr);
    }
#if defined(CPU_AARCH64)
    // ARM64 has separate I-cache and D-cache: we MUST flush the I-cache
    // after writing code before making it executable, or we'll execute
    // stale/random data from the I-cache.
    flush_cpu_icache((void *)popallspace, (void *)get_target());
#endif
#if !defined(CPU_AARCH64)
	vm_protect(popallspace, POPALLSPACE_SIZE, VM_PAGE_READ | VM_PAGE_EXECUTE);
#endif
	jit_end_write_window();
}

static inline void reset_lists(void)
{
    int i;

    for (i = 0; i < MAX_HOLD_BI; i++)
        hold_bi[i] = NULL;
    active = NULL;
    dormant = NULL;
}

static void prepare_block(blockinfo* bi)
{
    int i;

	jit_begin_write_window();
    set_target(current_compile_p);
    bi->direct_pen = (cpuop_func*)get_target();
    compemu_raw_execute_normal((uintptr) & (bi->pc_p));

    bi->direct_pcc = (cpuop_func*)get_target();
    compemu_raw_check_checksum((uintptr) & (bi->pc_p));

    flush_cpu_icache((void*)current_compile_p, (void*)target);
	jit_end_write_window();
    current_compile_p = get_target();

    bi->deplist = NULL;
    for (i = 0; i < 2; i++) {
        bi->dep[i].prev_p = NULL;
        bi->dep[i].next = NULL;
    }
    bi->status = BI_INVALID;
    bi->needed_flags = FLAG_ALL;
}

void compemu_reset(void)
{
    flush_icache = lazy_flush ? flush_icache_lazy : flush_icache_hard;
    set_cache_state(0);
}

// OPCODE is in big endian format
static inline void reset_compop(int opcode)
{
    compfunctbl[opcode] = NULL;
    nfcompfunctbl[opcode] = NULL;
}

void build_comp(void)
{
    int i, j;
    unsigned long opcode;
    const struct comptbl* tbl = op_smalltbl_0_comp_ff;
    const struct comptbl* nftbl = op_smalltbl_0_comp_nf;
    unsigned int cpu_level = (currprefs.cpu_model - 68000) / 10;
    if (cpu_level > 4)
        cpu_level--;
#ifdef NOFLAGS_SUPPORT_GENCOMP
    const struct cputbl *nfctbl = op_smalltbl_0_nf;
#endif

    regs.raw_cputbl_count = raw_cputbl_count;
    regs.mem_banks = (uintptr)mem_banks;
    regs.cache_tags = (uintptr)cache_tags;

    jit_log("<JIT compiler> : building compiler function tables");

    for (opcode = 0; opcode < 65536; opcode++) {
        reset_compop(opcode);
#ifdef NOFLAGS_SUPPORT_GENCOMP
        nfcpufunctbl[opcode] = op_illg;
#endif
        prop[opcode].use_flags = FLAG_ALL;
        prop[opcode].set_flags = FLAG_ALL;
        prop[opcode].cflow = fl_trap; // ILLEGAL instructions do trap
    }

    for (i = 0; tbl[i].opcode < 65536; i++) {
        int cflow = table68k[tbl[i].opcode].cflow;
        if (follow_const_jumps && (tbl[i].specific & COMP_OPCODE_ISCJUMP))
            cflow = fl_const_jump;
        else
            cflow &= ~fl_const_jump;
        prop[cft_map(tbl[i].opcode)].cflow = cflow;

        bool uses_fpu = (tbl[i].specific & COMP_OPCODE_USES_FPU) != 0;
        if (uses_fpu && avoid_fpu)
            compfunctbl[cft_map(tbl[i].opcode)] = NULL;
        else
            compfunctbl[cft_map(tbl[i].opcode)] = tbl[i].handler;

        if (trace_propbuild_env() && trace_propbuild_opcode((uae_u16)tbl[i].opcode)) {
            fprintf(stderr,
                "PROPBUILD tbl op=%04x map=%04x handler=%p specific=%x cflow=%x table_handler=%ld mnemo=%u\n",
                (unsigned)tbl[i].opcode,
                (unsigned)cft_map(tbl[i].opcode),
                (void*)tbl[i].handler,
                (unsigned)tbl[i].specific,
                (unsigned)cflow,
                (long)table68k[tbl[i].opcode].handler,
                (unsigned)table68k[tbl[i].opcode].mnemo);
        }
    }

    for (i = 0; nftbl[i].opcode < 65536; i++) {
        bool uses_fpu = (tbl[i].specific & COMP_OPCODE_USES_FPU) != 0;
        if (uses_fpu && avoid_fpu)
            nfcompfunctbl[cft_map(nftbl[i].opcode)] = NULL;
        else
            nfcompfunctbl[cft_map(nftbl[i].opcode)] = nftbl[i].handler;
#ifdef NOFLAGS_SUPPORT_GENCOMP
        nfcpufunctbl[cft_map(nftbl[i].opcode)] = nfctbl[i].handler;
#endif
    }

#ifdef NOFLAGS_SUPPORT_GENCOMP
    for (i = 0; nfctbl[i].handler; i++) {
        nfcpufunctbl[cft_map(nfctbl[i].opcode)] = nfctbl[i].handler;
    }
#endif

    for (opcode = 0; opcode < 65536; opcode++) {
        compop_func* f;
        compop_func* nff;
#ifdef NOFLAGS_SUPPORT_GENCOMP
        cpuop_func* nfcf;
#endif
        int isaddx, cflow;

        if (table68k[opcode].mnemo == i_ILLG || table68k[opcode].clev > cpu_level)
            continue;

        if (table68k[opcode].handler != -1) {
            f = compfunctbl[cft_map(table68k[opcode].handler)];
            nff = nfcompfunctbl[cft_map(table68k[opcode].handler)];
#ifdef NOFLAGS_SUPPORT_GENCOMP
            nfcf = nfcpufunctbl[cft_map(table68k[opcode].handler)];
#endif
            isaddx = prop[cft_map(table68k[opcode].handler)].is_addx;
            prop[cft_map(opcode)].is_addx = isaddx;
            cflow = prop[cft_map(table68k[opcode].handler)].cflow;
            prop[cft_map(opcode)].cflow = cflow;
            compfunctbl[cft_map(opcode)] = f;
            nfcompfunctbl[cft_map(opcode)] = nff;
#ifdef NOFLAGS_SUPPORT_GENCOMP
            Dif(nfcf == op_illg)
                abort();
            nfcpufunctbl[cft_map(opcode)] = nfcf;
#endif
        }
        prop[cft_map(opcode)].set_flags = table68k[opcode].flagdead;
        prop[cft_map(opcode)].use_flags = table68k[opcode].flaglive;
        /* Unconditional jumps don't evaluate condition codes, so they
         * don't actually use any flags themselves */
        if (prop[cft_map(opcode)].cflow & fl_const_jump)
            prop[cft_map(opcode)].use_flags = 0;

        if (trace_propbuild_env() && trace_propbuild_opcode((uae_u16)opcode)) {
            fprintf(stderr,
                "PROPBUILD final op=%04lx map=%04x handler=%ld mnemo=%u flagdead=%02x flaglive=%02x prop_use=%02x prop_set=%02x cflow=%x comp=%p\n",
                opcode,
                (unsigned)cft_map(opcode),
                (long)table68k[opcode].handler,
                (unsigned)table68k[opcode].mnemo,
                (unsigned)table68k[opcode].flagdead,
                (unsigned)table68k[opcode].flaglive,
                (unsigned)prop[cft_map(opcode)].use_flags,
                (unsigned)prop[cft_map(opcode)].set_flags,
                (unsigned)prop[cft_map(opcode)].cflow,
                (void*)compfunctbl[cft_map(opcode)]);
        }
    }

#ifdef NOFLAGS_SUPPORT_GENCOMP
    for (i = 0; nfctbl[i].handler != NULL; i++) {
        if (nfctbl[i].specific)
            nfcpufunctbl[cft_map(tbl[i].opcode)] = nfctbl[i].handler;
    }
#endif

    /* AArch64 central full-SR support that bypasses the generic failure path.
       These still terminate the block after the helper call, but they now have
       dedicated compiled handlers instead of relying on unsupported-op fallback. */
    compfunctbl[cft_map(0x007c)] = op_fullsr_orsr_w_comp_ff;
    compfunctbl[cft_map(0x027c)] = op_fullsr_andsr_w_comp_ff;
    compfunctbl[cft_map(0x0a7c)] = op_fullsr_eorsr_w_comp_ff;
    /* Resolve A-line startup traps in L2 instead of exact interpreter fallback.
       They are real control-flow/trap ops: run op_illg()/Exception() through a
       runtime helper and end the block on the helper-updated regs.pc_p. */
    for (opcode = 0xa000; opcode <= 0xafff; opcode++) {
        compfunctbl[cft_map(opcode)] = op_aline_trap_comp_ff;
        nfcompfunctbl[cft_map(opcode)] = op_aline_trap_comp_ff;
        prop[cft_map(opcode)].cflow = fl_trap;
    }
    /* EMUL_OP (0x71xx): compiled handler that replicates the interpreter
       fallback path exactly — flush, init_comp, PC triple, cputbl call. */
    for (opcode = 0x7100; opcode <= 0x71ff; opcode++) {
        compfunctbl[cft_map(opcode)] = op_emulop_comp_ff;
        nfcompfunctbl[cft_map(opcode)] = op_emulop_comp_ff;
    }
    /* Keep MV2SR.W on the exact legacy/interpreter path for now; the block
       barrier above preserves correctness after MakeFromSR()-style state
       changes while we continue narrowing the native helper bug. */
    /* Exact semantic containment for the confirmed remaining optlev=2 blocker:
       MOVE.L (d8,An,Xn),(xxx).W. The failing boot path reaches this through
       68020 memory-indirect full-format EAs; route the family through a
       runtime helper until the native source-read bug is fixed from first
       principles. */
    for (opcode = 0x21f0; opcode <= 0x21f7; opcode++) {
        compfunctbl[cft_map(opcode)] = op_move_l_d8anxn_absw_comp_ff;
        nfcompfunctbl[cft_map(opcode)] = op_move_l_d8anxn_absw_comp_ff;
    }
    /* Next proven deep-surface candidate: MOVE.L register -> (d16,An) long
       store family. The strongest 60s improvement came from gating the exact
       `2149` + `2140` pair, which are just two members of this semantic
       family. Route the whole family through an exact runtime helper barrier
       while we keep narrowing the deeper block interaction around it. */
    for (opcode = 0; opcode < 65536; opcode++) {
        if (table68k[opcode].mnemo == i_MOVE &&
            table68k[opcode].size == sz_long &&
            table68k[opcode].dmode == Ad16 &&
            (table68k[opcode].smode == Dreg || table68k[opcode].smode == Areg)) {
            compfunctbl[cft_map(opcode)] = op_move_l_reg_d16an_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_move_l_reg_d16an_comp_ff;
        }
    }
    /* Current next frontier: MOVEA.L (An)+,An postincrement stack-pop family.
       Route the whole semantic family through an exact runtime helper barrier
       while we keep narrowing the native state-transition bug exposed by
       0x225f (MOVEA.L (A7)+,A1). */
    for (opcode = 0; opcode < 65536; opcode++) {
        if (table68k[opcode].mnemo == i_MOVEA &&
            table68k[opcode].size == sz_long &&
            table68k[opcode].smode == Aipi &&
            table68k[opcode].dmode == Areg) {
            compfunctbl[cft_map(opcode)] = op_movea_l_postinc_an_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_movea_l_postinc_an_comp_ff;
        }
    }

    /* Propagate compiled handlers from base opcodes to variants.
       gencomp generates handlers for base opcodes only (e.g., e008 for LSR.B).
       Variant opcodes (e.g., e214 = LSR.B #1,D4) need to be mapped via
       table68k[opcode].handler to the base handler. */
    for (opcode = 0; opcode < 65536; opcode++) {
        if (table68k[opcode].mnemo == i_ILLG || table68k[opcode].handler == -1)
            continue;
        if (!compfunctbl[cft_map(opcode)] && table68k[opcode].handler != -1) {
            /* Follow the handler chain — may need multiple hops */
            int base = table68k[opcode].handler;
            int hops = 0;
            while (base >= 0 && base < 65536 && !compfunctbl[base] && hops < 10) {
                if (table68k[base].handler == -1 || table68k[base].handler == base)
                    break;
                base = table68k[base].handler;
                hops++;
            }
            compop_func *f = (base >= 0 && base < 65536) ? compfunctbl[base] : NULL;
            compop_func *nff = (base >= 0 && base < 65536) ? nfcompfunctbl[base] : NULL;
            /* If chain failed, search by instruction family (same mnemo) */
            if (!f) {
                int mnemo = table68k[opcode].mnemo;
                for (int probe = 0; probe < 65536 && !f; probe++) {
                    if (table68k[probe].mnemo == mnemo && compfunctbl[probe]) {
                        f = compfunctbl[probe];
                        nff = nfcompfunctbl[probe];
                        base = probe;
                    }
                }
            }
            if (f) {
                compfunctbl[cft_map(opcode)] = f;
                nfcompfunctbl[cft_map(opcode)] = nff;
            }
            if (base >= 0 && base < 65536) {
                prop[cft_map(opcode)].cflow = prop[base].cflow;
            }
            prop[cft_map(opcode)].set_flags = table68k[opcode].flagdead;
            prop[cft_map(opcode)].use_flags = table68k[opcode].flaglive;
        }
    }

    /* DEBUG: check e214 handler propagation */
    {
        int h = table68k[0xe214].handler;
        fprintf(stderr, "DEBUG: e214 handler=%d(0x%x) compfunctbl[e214]=%p compfunctbl[handler]=%p compfunctbl[e008]=%p\n",
            h, h, (void*)compfunctbl[cft_map(0xe214)], (void*)(h>=0 ? compfunctbl[h] : NULL), (void*)compfunctbl[cft_map(0xe008)]);
        fflush(stderr);
    }
    int count = 0;
    for (opcode = 0; opcode < 65536; opcode++) {
        if (compfunctbl[cft_map(opcode)])
            count++;
    }

#if defined(CPU_AARCH64)
    /* ARM64: shift/rotate handlers were previously nulled due to suspected
       codegen bugs. Re-enabled now that carry flag handling is fixed.
       If specific shifts still cause issues, null them individually. */
    jit_log("<JIT compiler> : shift/rotate handlers ENABLED for ARM64");
#endif

    jit_log("<JIT compiler> : supposedly %d compileable opcodes!", count);

	/* Initialise state */
	create_popalls();
	if (!pushall_call_handler || !popall_execute_normal) {
		disable_jit_runtime("failed to initialize JIT dispatcher stubs (popallspace)");
		return;
	}
#if defined(CPU_AARCH64)
	// jit_install_diag_handler(); // Use BasiliskII SIGSEGV handler for recovery
	fprintf(stderr, "JIT: popallspace=%p cache_start=%p popall_execute_normal=%p\n",
		popallspace, popall_combined_cache_start, popall_execute_normal);
	fflush(stderr);
	if (popall_execute_normal) {
		uae_u32 *p = (uae_u32*)popall_execute_normal;
		for (int d=0;d<20;d++) fprintf(stderr,"  [%02d] %08x\n",d*4,p[d]);
		fflush(stderr);
	}
#endif
	alloc_cache();
	if (!compiled_code) {
		disable_jit_runtime("failed to allocate ARM64 JIT code cache");
		return;
	}
	reset_lists();

    for (i = 0; i < TAGSIZE; i += 2) {
        cache_tags[i].handler = (cpuop_func*)popall_execute_normal;
        cache_tags[i + 1].bi = NULL;
    }
    compemu_reset();
    cache_enabled = 1;
    SPCFLAGS_CLEAR(SPCFLAG_JIT_EXEC_RETURN);
}

static void flush_icache_none(int v)
{
    /* Nothing to do.  */
}

void flush_icache_hard(int n)
{
#if defined(CPU_AARCH64)
    jit_diag_flush_icache_hard_calls++;
    fprintf(stderr, "JIT_DIAG flush_icache_hard called (n=%d), total=%lu\n", n, jit_diag_flush_icache_hard_calls);
    fflush(stderr);
    jit_diag_maybe_print();
#endif
    blockinfo* bi, * dbi;

    bi = active;
    while (bi) {
        cache_tags[cacheline(bi->pc_p)].handler = (cpuop_func*)popall_execute_normal;
        cache_tags[cacheline(bi->pc_p) + 1].bi = NULL;
        dbi = bi;
        bi = bi->next;
        free_blockinfo(dbi);
    }
    bi = dormant;
    while (bi) {
        cache_tags[cacheline(bi->pc_p)].handler = (cpuop_func*)popall_execute_normal;
        cache_tags[cacheline(bi->pc_p) + 1].bi = NULL;
        dbi = bi;
        bi = bi->next;
        free_blockinfo(dbi);
    }

    reset_lists();
    if (!compiled_code)
        return;

#if defined(USE_DATA_BUFFER)
    reset_data_buffer();
#endif

    current_compile_p = compiled_code;
    SPCFLAGS_SET( SPCFLAG_JIT_EXEC_RETURN ); /* To get out of compiled code */
}

/* "Soft flushing" --- instead of actually throwing everything away,
we simply mark everything as "needs to be checked".
*/

static inline void flush_icache_lazy(int v)
{
    blockinfo* bi;
    blockinfo* bi2;

    if (!active)
        return;

    bi = active;
    while (bi) {
        uae_u32 cl = cacheline(bi->pc_p);
        if (bi->status == BI_INVALID || bi->status == BI_NEED_RECOMP) {
            if (bi == cache_tags[cl + 1].bi)
                cache_tags[cl].handler = (cpuop_func*)popall_execute_normal;
            bi->handler_to_use = (cpuop_func*)popall_execute_normal;
            set_dhtu(bi, bi->direct_pen);
            bi->status = BI_INVALID;
        } else {
            if (bi == cache_tags[cl + 1].bi)
                cache_tags[cl].handler = (cpuop_func*)popall_check_checksum;
            bi->handler_to_use = (cpuop_func*)popall_check_checksum;
            set_dhtu(bi, bi->direct_pcc);
            bi->status = BI_NEED_CHECK;
        }
        bi2 = bi;
        bi = bi->next;
    }
    /* bi2 is now the last entry in the active list */
    bi2->next = dormant;
    if (dormant)
        dormant->prev_p = &(bi2->next);

    dormant = active;
    active->prev_p = &dormant;
    active = NULL;
}

int failure;

static inline unsigned int get_opcode_cft_map(unsigned int f)
{
    return uae_bswap_16(f);
}
#define DO_GET_OPCODE(a) (get_opcode_cft_map((uae_u16)*(a)))

void compile_block(cpu_history* pc_hist, int blocklen, int totcycles)
{
#if defined(CPU_AARCH64)
    jit_diag_compile_block_calls++;
#endif
    if (cache_enabled && compiled_code && currprefs.cpu_model >= 68020) {
		jit_begin_write_window();
#ifdef PROFILE_COMPILE_TIME
        compile_count++;
        clock_t start_time = clock();
#endif

        /* OK, here we need to 'compile' a block */
        int i;
        int r;
        int was_comp = 0;
        uae_u8 liveflags[MAXRUN + 1];
        bool trace_in_rom = isinrom((uintptr)pc_hist[0].location) != 0;
        uintptr max_pcp = (uintptr)pc_hist[blocklen - 1].location;
        uintptr min_pcp = max_pcp;
        uae_u32 cl = cacheline(pc_hist[0].location);
		void* specflags=(void*)&regs.spcflags;
        blockinfo* bi = NULL;
        blockinfo* bi2;
		int extra_len=0;

        redo_current_block = 0;
        if (current_compile_p >= MAX_COMPILE_PTR)
            flush_icache_lazy(3);

        alloc_blockinfos();

        bi = get_blockinfo_addr_new(pc_hist[0].location);
        bi2 = get_blockinfo(cl);

        optlev = bi->optlevel;
#if defined(CPU_AARCH64)
        /* When cpu_compatible is set, force blocks to interpreter mode */
        if (currprefs.cpu_compatible) {
            optlev = 0;
        }
#endif
        if (bi->status != BI_INVALID) {
#if defined(CPU_AARCH64)
            jit_diag_compile_block_recomp++;
#endif
            Dif(bi != bi2) {
                /* I don't think it can happen anymore. Shouldn't, in
                   any case. So let's make sure... */
                jit_abort("WOOOWOO count=%d, ol=%d %p %p", bi->count, bi->optlevel, bi->handler_to_use, cache_tags[cl].handler);
            }

            Dif(bi->count != -1 && bi->status != BI_NEED_RECOMP) {
                jit_abort("bi->count=%d, bi->status=%d,bi->optlevel=%d", bi->count, bi->status, bi->optlevel);
                /* What the heck? We are not supposed to be here! */
            }
        }
#if defined(CPU_AARCH64)
        else {
            jit_diag_compile_block_fresh++;
        }
#endif
        if (bi->count == -1) {
#if defined(CPU_AARCH64)
            if (currprefs.cpu_compatible) {
                optlev = 0;
            } else {
                const int max_optlev = jit_max_optlev();
                if (optlev < max_optlev) {
                    optlev = max_optlev;
                    bi->count = -2; /* Stay at this level permanently */
                }
                /* else: already at the selected cap, don't escalate further */
            }
#else
            {
            optlev++;
            while (!optcount[optlev])
                optlev++;
            bi->count = optcount[optlev] - 1;
            }
#endif
        }
        current_block_pc_p = JITPTR pc_hist[0].location;

        /* Save successor needed_flags BEFORE remove_deps clears them.
           On recompilation, deps still point to the previous successor blocks.
           Use their needed_flags to initialize liveflags[blocklen] more
           precisely than the conservative FLAG_ALL. This allows the backward
           liveflags analysis to eliminate more flag operations within the block.
           First compilation or after invalidation: deps are NULL, falls back to FLAG_ALL.
           Only use needed_flags from blocks that have actually been compiled
           (status != BI_INVALID), since uncompiled blocks have uninitialized flags. */
        uae_u8 successor_flags = FLAG_ALL;
        if (bi->dep[0].target && bi->dep[0].target->status != BI_INVALID) {
            successor_flags = bi->dep[0].target->needed_flags;
            if (bi->dep[1].target) {
                if (bi->dep[1].target->status != BI_INVALID)
                    successor_flags |= bi->dep[1].target->needed_flags;
                else
                    successor_flags = FLAG_ALL; /* dep[1] uncompiled, be conservative */
            }
        }

        remove_deps(bi); /* We are about to create new code */
        bi->optlevel = optlev;
        bi->pc_p = (uae_u8*)pc_hist[0].location;
        free_checksum_info_chain(bi->csi);
        bi->csi = NULL;

        liveflags[blocklen] = successor_flags; /* Use successor info if available, else FLAG_ALL */
        i = blocklen;
        while (i--) {
            uae_u16* currpcp = pc_hist[i].location;
            uae_u32 op = DO_GET_OPCODE(currpcp);

            trace_in_rom = trace_in_rom && isinrom((uintptr)currpcp);
            if (follow_const_jumps && is_const_jump(op)) {
                checksum_info* csi = alloc_checksum_info();
                csi->start_p = (uae_u8*)min_pcp;
                csi->length = JITPTR max_pcp - JITPTR min_pcp + LONGEST_68K_INST;
                csi->next = bi->csi;
                bi->csi = csi;
                max_pcp = (uintptr)currpcp;
            }
            min_pcp = (uintptr)currpcp;

            if (!currprefs.compnf) {
                liveflags[i] = FLAG_ALL;
            }
            else
            {
                liveflags[i] = ((liveflags[i + 1] & (~prop[cft_map(op)].set_flags)) | prop[cft_map(op)].use_flags);
                if (prop[cft_map(op)].is_addx && (liveflags[i + 1] & FLAG_Z) == 0)
                    liveflags[i] &= ~FLAG_Z;
            }
        }

        checksum_info* csi = alloc_checksum_info();
        csi->start_p = (uae_u8*)min_pcp;
        csi->length = max_pcp - min_pcp + LONGEST_68K_INST;
        csi->next = bi->csi;
        bi->csi = csi;

        bi->needed_flags = liveflags[0];

        /* This is the non-direct handler */
        was_comp = 0;

        bi->direct_handler = (cpuop_func*)get_target();
        set_dhtu(bi, bi->direct_handler);
        bi->status = BI_COMPILING;
        current_block_start_target = (uintptr)get_target();

        if (bi->count >= 0) { /* Need to generate countdown code */
            compemu_raw_set_pc_i((uintptr)pc_hist[0].location);
            compemu_raw_dec_m((uintptr) & (bi->count));
            compemu_raw_maybe_recompile();
        }
        uae_u32 block_m68k_pc = 0;
        if (pc_hist[0].location) {
            /* Anchor block PC derivation to the actual host fetch address,
               not the mutable regs.pc/regs.pc_oldp metadata. This keeps
               PC-relative codegen stable across mixed-mode/direct-chain
               transitions. */
            block_m68k_pc = jit_hostpc_to_macpc((uintptr)pc_hist[0].location);
        }
#if defined(CPU_AARCH64)
        if (jit_force_optlev0() || jit_force_optlev0_block_exact(block_m68k_pc) || jit_force_optlev0_block_env(block_m68k_pc)) {
            optlev = 0;
        } else if (optlev > 0) {
            if (optlev > 1 && jit_force_optlev1_block(block_m68k_pc)) {
                optlev = 1;
            }
            for (int _i = 0; optlev > 0 && _i < blocklen; _i++) {
                uae_u16 _op = do_get_mem_word(pc_hist[_i].location);
                if (jit_restore_barrier("dbcc") && (_op & 0xF0F8) == 0x50C8) {
                    optlev = 0;
                    break;
                }
                if (optlev > 1 && jit_force_optlev1_opcode(_op)) {
                    optlev = 1;
                    break;
                }
            }
        }
#endif
#if 0 /* Removed: all gates now use per-instruction barriers, blocks always stay at optlev=2 */
        if (optlev > 1 && jit_disable_hardcoded_optlev1()) {
            optlev = 1;
        }
#endif
        if (optlev == 0) { /* No need to actually translate */
#if defined(CPU_AARCH64)
          jit_diag_optlev0_blocks++;
#endif
          /* Execute normally without keeping stats */
            compemu_raw_exec_nostats((uintptr)pc_hist[0].location);
        } else {
#if defined(CPU_AARCH64)
            jit_diag_optlev_gt0_blocks++;
            /* Block compilation logging — disabled for performance */
#if 0
            if (block_m68k_pc >= 0x1000 && block_m68k_pc <= 0x1020) {
                fprintf(stderr, "JIT_COMPILE optlev=%d pc=0x%08x blocklen=%d opcodes=",
                    optlev, block_m68k_pc, blocklen);
                for (int di = 0; di < blocklen && di < 20; di++) {
                    uae_u32 op = DO_GET_OPCODE(pc_hist[di].location);
                    fprintf(stderr, "%04x ", op);
                }
                fprintf(stderr, "\n");
                fflush(stderr);
            }
#endif /* 0 - logging disabled */
#endif
            reg_alloc_run = 0;
            next_pc_p = 0;
            taken_pc_p = 0;
            branch_cc = 0; // Only to be initialized. Will be set together with next_pc_p
            jit_force_runtime_pc_endblock = false;
            bool forced_interpreter_barrier = false;

            comp_pc_p = (uae_u8*)pc_hist[0].location;
            init_comp();
            was_comp = 1;

#if defined(CPU_AARCH64)
            /* ARM64: reload hardware NZCV from regflags.nzcv at block entry. */
            {
                LOAD_U64(REG_WORK1, (uintptr)&regflags.nzcv);
                LDR_wXi(REG_WORK2, REG_WORK1, 0);
                MSR_NZCV_x(REG_WORK2);
                /* Do NOT set flags_in_flags=VALID here; leave it as TRASH
                   from init_comp.  This prevents flush(1) from trying to
                   save the hardware NZCV back to regflags.nzcv. */
            }
#endif

            if (trace_emuneigh_env() && trace_emuneigh_target(block_m68k_pc) && trace_emuneigh_count < trace_emuneigh_limit()) {
                uae_u16 first_op = DO_GET_OPCODE(pc_hist[0].location);
                compemu_raw_mov_l_ri(REG_PAR1, block_m68k_pc);
                compemu_raw_mov_l_ri(REG_PAR2, first_op);
                compemu_raw_call((uintptr)trace_emuneigh_entry);
            }

            for (i = 0; i < blocklen && get_target() < MAX_COMPILE_PTR; i++) {
                may_raise_exception = false;
                cpuop_func** cputbl;
                compop_func** comptbl;
                uae_u32 opcode = DO_GET_OPCODE(pc_hist[i].location);
                const uae_u32 op_m68k_pc = block_m68k_pc + (uae_u32)((uintptr)pc_hist[i].location - (uintptr)pc_hist[0].location);
                needed_flags = (liveflags[i + 1] & prop[cft_map(opcode)].set_flags);
#ifdef UAE
                special_mem = pc_hist[i].specmem;
#else
                special_mem = special_mem_default;
#endif
                {
                    trace_flagflow_block_pc = block_m68k_pc;
                    trace_flagflow_pc = op_m68k_pc;
                    trace_flagflow_op = (uae_u16)opcode;
                    if (optlev > 1 && trace_flagflow_opcode((uae_u16)opcode)) {
                        uae_u16 next_op = 0xffff;
                        if (i + 1 < blocklen)
                            next_op = DO_GET_OPCODE(pc_hist[i + 1].location);
                        trace_flagflow_log_opmeta((uae_u16)opcode, liveflags[i + 1], next_op);
                        trace_flagflow_log("COMPILE_OP", liveflags[i + 1], prop[cft_map(opcode)].use_flags, prop[cft_map(opcode)].set_flags, ((uae_u32)needed_flags << 16) | next_op);
                    }
                }
                const int retired_cycles = scaled_cycles((i + 1) * 4 * CYCLE_UNIT);
                if (!needed_flags && currprefs.compnf) {
#ifdef NOFLAGS_SUPPORT_GENCOMP
                    cputbl = nfcpufunctbl;
#else
                    cputbl = cpufunctbl;
#endif
                    comptbl = nfcompfunctbl;
                } else {
                    cputbl = cpufunctbl;
                    comptbl = compfunctbl;
                }


                if (jit_force_exact_exec_nostats_pc(op_m68k_pc)) {
                    if (was_comp) {
                        flush(1);
                        was_comp = 0;
                    }
                    fprintf(stderr, "JIT_EXACT_EXEC_NOSTATS block=%08x pc=%08x op=%04x reason=pc\n",
                        (unsigned)block_m68k_pc,
                        (unsigned)op_m68k_pc,
                        (unsigned)opcode);
                    compemu_raw_exec_nostats((uintptr)pc_hist[i].location);
                    forced_interpreter_barrier = true;
                    break;
                }

                failure = 1; // gb-- defaults to failure state
                /* ARM64 L2 bisection: only allow native codegen for specific families */
                bool allow_l2 = true;
#if defined(CPU_AARCH64)
                /* Per-instruction interpreter barrier: force specific instructions
                   through interpreter even if they have compiled handlers.
                   This prevents L2 native codegen for control-flow, MOVEM, etc. */
                if (jit_force_interpreter_barrier_opcode((uae_u16)opcode))
                    allow_l2 = false;
#endif
#if defined(CPU_AARCH64)
                {
                    static int l2_bisect = -1;
                    if (l2_bisect < 0) {
                        const char *env = getenv("B2_JIT_L2_BISECT");
                        l2_bisect = env && *env ? atoi(env) : 0;
                    }
                    if (l2_bisect > 0) {
                        /* Force interpreter for families NOT in the bisect set */
                        uae_u16 lop = (uae_u16)opcode; /* logical opcode */
                        uae_u16 top = (lop >> 12) & 0xf;
                        if (l2_bisect == 1) allow_l2 = (top <= 0x3); /* only ORI..MOVE */
                        else if (l2_bisect == 2) allow_l2 = (top >= 0x4 && top <= 0x7); /* misc+Bcc+MOVEQ */
                        else if (l2_bisect == 3) allow_l2 = (top >= 0x8); /* arith+shifts */
                        else if (l2_bisect == 4) allow_l2 = (top <= 0x3 || top >= 0x8); /* skip misc/Bcc */
                        else if (l2_bisect == 5) allow_l2 = (top != 0xe); /* skip shifts only */
                        else if (l2_bisect == 6) allow_l2 = (top == 0xe); /* shifts ONLY */
                        else if (l2_bisect == 7) allow_l2 = (top >= 0x4 && top <= 0x5); /* misc only */
                        else if (l2_bisect == 8) allow_l2 = (top == 0x4); /* 4xxx only */
                        else if (l2_bisect == 9) allow_l2 = (top == 0x6); /* Bcc only */
                        else if (l2_bisect == 10) allow_l2 = false; /* nothing - pure L1 */
                    }
                }
#endif
                if (comptbl[cft_map(opcode)] && optlev > 1 && allow_l2) {
                    failure = 0;
                    if (!was_comp) {
                        comp_pc_p = (uae_u8*)pc_hist[i].location;
                        init_comp();
                    }
                    was_comp = 1;

#if defined(CPU_AARCH64)
                    uae_u8* _before = get_target();
                    const bool _verify_this_op = jit_verify_target_pc(op_m68k_pc);
                    const bool _trace_this_op = jit_trace_target_pc(op_m68k_pc);
                    if (_verify_this_op || _trace_this_op) {
                        flush(1);
                        if (_trace_this_op) {
                            compemu_raw_mov_l_ri(REG_PAR1, op_m68k_pc);
                            compemu_raw_mov_l_ri(REG_PAR2, (1u << 16) | (opcode & 0xffff));
                            compemu_raw_call((uintptr)jit_trace_pc_hit);
                        }
                        if (_verify_this_op) {
                            compemu_raw_set_pc_i((uintptr)pc_hist[i].location);
                            compemu_raw_mov_l_ri(REG_PAR1, op_m68k_pc);
                            compemu_raw_mov_l_ri(REG_PAR2, opcode);
                            compemu_raw_call((uintptr)jit_verify_pre);
                        }
                        comp_pc_p = (uae_u8*)pc_hist[i].location;
                        init_comp();
                    }
#endif
                    comptbl[cft_map(opcode)](opcode);
#if defined(CPU_AARCH64)
                    /* Trace compiled family-d instructions at runtime */
                    if (_verify_this_op ||
                        (((opcode >> 12) & 0xf) == 0xd && getenv("B2_JIT_TRACE_ADD")) ||
                        false /* watch_mem removed */) {
                        uae_u32 pc_val = (uae_u32)((uintptr)pc_hist[i].location - (uintptr)ROMBaseHost + ROMBaseMac);
                        /* Save all caller-saved regs around the trace/verify call */
                        flush(1);
                        compemu_raw_mov_l_ri(REG_PAR1, pc_val);
                        compemu_raw_mov_l_ri(REG_PAR2, opcode);
                        if (_verify_this_op)
                            compemu_raw_call((uintptr)jit_verify_post);
                        else
                            compemu_raw_call((uintptr)jit_trace_add);
                        comp_pc_p = (uae_u8*)pc_hist[i].location;
                        init_comp();
                        was_comp = 0; /* force re-init for next instruction */
                    }
#endif
#if defined(CPU_AARCH64)
                    uae_u8* _after = get_target();
#if 0  /* JIT_CODEGEN logging disabled for performance */
                    if (i < 10) {
                        fprintf(stderr, "JIT_CODEGEN pc=0x%08x op=0x%04x jit_range=[%p,%p] size=%ld\n",
                            (unsigned)(uintptr)pc_hist[i].location - (unsigned)(uintptr)ROMBaseHost + ROMBaseMac,
                            opcode, _before, _after, (long)(_after - _before));
                    }
#endif
                    /* Dump first 3 blocks' native code to file for disassembly */
                    {
                        static int dump_count = 0;
                        if (getenv("B2_JIT_DUMP") && (_after - _before) > 0 && (dump_count < 3 || opcode == 0x11df || opcode == 0x11d8 || opcode == 0x31c0 || opcode == 0x51c8)) {
                            char fname[256];
                            snprintf(fname, sizeof(fname), "/workspace/tmp/jitdump/block%d_op%04x.bin", dump_count, opcode);
                            FILE *f = fopen(fname, "wb");
                            if (f) {
                                fwrite(_before, 1, _after - _before, f);
                                fclose(f);
                                fprintf(stderr, "JIT_DUMP: wrote %ld bytes to %s\n", (long)(_after - _before), fname);
                                dump_count++;
                            }
                        }
                    }
#endif
                    if (jit_force_runtime_pc_endblock) {
                        /* Runtime helper barriers can change guest PC/state in
                           ways that require a fresh dispatcher entry. End the
                           block exactly like the interpreter-side full-SR
                           barrier path instead of just breaking out of codegen. */
                        compemu_raw_mov_l_rm(0, (uintptr)specflags);
#if defined(USE_DATA_BUFFER)
                        data_check_end(12, 64);
#endif
                        compemu_raw_maybe_do_nothing(retired_cycles);
                        compemu_raw_mov_l_rm(REG_PC_TMP, (uintptr)&regs.pc_p);
                        compemu_raw_endblock_pc_inreg(REG_PC_TMP, retired_cycles);
                        forced_interpreter_barrier = true;
                        break;
                    }
                    freescratch();
                    bool flushed_after_native_op = false;
#if defined(CPU_AARCH64)
                    const bool _flush_this_op = jit_flush_target_pc(op_m68k_pc);
                    if (jit_flush_each_op_env() || _flush_this_op) {
                        /* Diagnostic mode: snapshot the effective live native
                           state, then force canonical in-memory guest state.
                           Comparing the snapshot against post-flush memory tells
                           us what native continuation was carrying implicitly. */
                        if (_flush_this_op) {
                            fprintf(stderr, "JIT_FLUSH_INSERT pc=%08x op=%04x\n", op_m68k_pc, opcode);
                            jit_emit_flush_delta_snapshot();
                        }
                        live.flags_are_important = 1;
                        flush(1);
                        if (_flush_this_op) {
                            compemu_raw_mov_l_ri(REG_PAR1, op_m68k_pc);
                            compemu_raw_call((uintptr)jit_flush_delta_compare);
                        }
                        init_comp();
                        was_comp = 0;
                        flushed_after_native_op = true;
                    }
#endif
                    if (!flushed_after_native_op && !(liveflags[i + 1] & FLAG_CZNV)) {
                        /* We can forget about flags */
                        if (optlev > 1 && trace_flagflow_opcode((uae_u16)opcode))
                            trace_flagflow_log("DROP_AFTER_OP", liveflags[i + 1], prop[cft_map(opcode)].use_flags, prop[cft_map(opcode)].set_flags);
                        dont_care_flags();
                    }

                    /* Mid-block tick injection + spcflags check.
                       Every JIT_TICK_INTERVAL compiled instructions, emit a
                       full flush → cpu_do_check_ticks() → spcflags check.
                       This ensures one_tick() fires at the correct cadence
                       even inside long compiled blocks, and that pending
                       interrupts are delivered promptly.
                       For shorter intervals, just check spcflags inline. */
#define JIT_TICK_INTERVAL 64

#if defined(CPU_AARCH64)
                    /* Mid-block branch side-exit: emit a guard for non-traced path */
                    if (i < blocklen - 1 && was_comp && next_pc_p && taken_pc_p) {
                        uintptr next_traced = (uintptr)pc_hist[i + 1].location;
                        uintptr side_exit_pc = 0;
                        int side_cond = -1;
                        if (next_traced == taken_pc_p) {
                            side_exit_pc = next_pc_p;
                            side_cond = branch_cc ^ 1;
                        } else if (next_traced == next_pc_p) {
                            side_exit_pc = taken_pc_p;
                            side_cond = branch_cc;
                        }
                        if (side_cond >= 0) {
                            bigstate saved_live = live;
                            flush(1);
                            make_flags_live();
                            /* Emit guard: if (side_cond) goto side_exit */
                            uae_u8* patch_guard = (uae_u8*)get_target();
                            CC_B_i(side_cond, 0); /* patched below */
                            /* Side exit: load PC and endblock */
                            uae_u8* guard_target = (uae_u8*)get_target();
                            LOAD_U64(REG_PC_TMP, side_exit_pc); /* load into x1 for endblock */
                            compemu_raw_endblock_pc_inreg(REG_PC_TMP,
                                scaled_cycles((i + 1) * 4 * CYCLE_UNIT));
                            /* Patch guard branch */
                            int goff = (int)(guard_target - patch_guard) / 4;
                            *(uae_u32*)patch_guard = 0x54000000u
                                | ((goff & 0x7ffff) << 5) | (side_cond & 0xf);
                            /* Restore register allocator for traced path */
                            live = saved_live;
                        }
                        next_pc_p = 0;
                        taken_pc_p = 0;
                    }
#endif
                    if (i < blocklen - 1 && was_comp && (i + 1) % JIT_TICK_INTERVAL == 0) {
                        /* Full tick injection: flush state, call cpu_do_check_ticks,
                           check spcflags, and if set, exit block. */
                        flush(1);
                        compemu_raw_call((uintptr)cpu_do_check_ticks);
                        /* Now check spcflags — cpu_do_check_ticks may have
                           called one_tick → TriggerInterrupt → SPCFLAG_INT */
                        uintptr idx_spc = (uintptr)&regs.spcflags - (uintptr)&regs;
                        LDR_wXi(REG_WORK1, R_REGSTRUCT, idx_spc);
                        uae_u32* branch_skip_tick = (uae_u32*)get_target();
                        CBZ_wi(REG_WORK1, 0);
                        /* Cold: spcflags set → exit block */
                        compemu_raw_set_pc_i((uintptr)pc_hist[i + 1].location);
                        {
                            uae_u32 next_m68k_pc = block_m68k_pc + (uae_u32)((uintptr)pc_hist[i + 1].location - (uintptr)pc_hist[0].location);
                            compemu_raw_mov_l_mi((uintptr)&regs.pc, next_m68k_pc);
                            LOAD_U64(REG_WORK1, (uintptr)pc_hist[i + 1].location);
                            uintptr offs_oldp = (uintptr)&regs.pc_oldp - (uintptr)&regs;
                            STR_xXi(REG_WORK1, R_REGSTRUCT, offs_oldp);
                        }
                        LOAD_U64(REG_WORK3, (uintptr)&countdown);
                        LDR_wXi(REG_WORK2, REG_WORK3, 0);
                        LOAD_U32(REG_WORK1, retired_cycles);
                        SUB_www(REG_WORK2, REG_WORK2, REG_WORK1);
                        STR_wXi(REG_WORK2, REG_WORK3, 0);
                        uae_u32* branch_exit_tick = (uae_u32*)get_target();
                        B_i(0);
                        write_jmp_target(branch_exit_tick, (uintptr)popall_do_nothing);
                        write_jmp_target((uae_u32*)branch_skip_tick, (uintptr)get_target());
                        init_comp();
                        was_comp = 0;
                    } else if (i < blocklen - 1) {
                        /* Check spcflags */
                        uintptr idx_spc = (uintptr)&regs.spcflags - (uintptr)&regs;
                        LDR_wXi(REG_WORK1, R_REGSTRUCT, idx_spc);
                        uae_u32* branch_skip = (uae_u32*)get_target();
                        CBZ_wi(REG_WORK1, 0);  /* → skip (patched below) */

                        /* Cold path: save all dirty registers to memory WITHOUT
                           modifying the compile-time allocator state. */
                        for (int j = 0; j <= FLAGTMP; j++) {
                            if (live.state[j].status == DIRTY && live.state[j].realreg >= 0) {
                                int rr = live.state[j].realreg;
                                if (j == FLAGX) {
                                    /* Convert X from JIT 0/1 format to interpreter bit-29 */
                                    LSL_wwi(REG_WORK2, rr, 29);
                                    compemu_raw_mov_l_mr((uintptr)live.state[j].mem, REG_WORK2);
                                } else {
                                    compemu_raw_mov_l_mr((uintptr)live.state[j].mem, rr);
                                }
                            } else if (live.state[j].status == ISCONST && j != PC_P) {
                                if (j == FLAGX) {
                                    uae_u32 val = (live.state[j].val & 1) << 29;
                                    compemu_raw_mov_l_mi((uintptr)live.state[j].mem, val);
                                } else {
                                    compemu_raw_mov_l_mi((uintptr)live.state[j].mem, live.state[j].val);
                                }
                            }
                        }
                        /* Flush flags (NZCV) to regflags.nzcv if valid in ARM64 NZCV */
                        if (live.flags_in_flags == VALID) {
                            MRS_NZCV_x(REG_WORK2);
                            if (flags_carry_inverted) {
                                EOR_xxCflag(REG_WORK2, REG_WORK2);
                            }
                            LOAD_U64(REG_WORK3, (uintptr)&(regflags.nzcv));
                            STR_wXi(REG_WORK2, REG_WORK3, 0);
                        }
                        /* Sync PC to the NEXT instruction — full triple */
                        compemu_raw_set_pc_i((uintptr)pc_hist[i + 1].location);
#if defined(CPU_AARCH64)
                        /* ARM64: also store regs.pc and regs.pc_oldp so that
                           m68k_getpc() in m68k_do_specialties() returns the
                           correct guest PC. Without this, the stale triple
                           from block entry causes wrong interrupt delivery. */
                        {
                            uae_u32 next_m68k_pc = block_m68k_pc + (uae_u32)((uintptr)pc_hist[i + 1].location - (uintptr)pc_hist[0].location);
                            compemu_raw_mov_l_mi((uintptr)&regs.pc, next_m68k_pc);
                            LOAD_U64(REG_WORK1, (uintptr)pc_hist[i + 1].location);
                            uintptr offs_oldp = (uintptr)&regs.pc_oldp - (uintptr)&regs;
                            STR_xXi(REG_WORK1, R_REGSTRUCT, offs_oldp);
                        }
#endif
                        /* Subtract only the cycles retired up to this point. */
                        LOAD_U64(REG_WORK3, (uintptr)&countdown);
                        LDR_wXi(REG_WORK2, REG_WORK3, 0);
                        LOAD_U32(REG_WORK1, retired_cycles);
                        SUB_www(REG_WORK2, REG_WORK2, REG_WORK1);
                        STR_wXi(REG_WORK2, REG_WORK3, 0);
                        uae_u32* branch_exit = (uae_u32*)get_target();
                        B_i(0); /* → popall_do_nothing (patched below) */
                        write_jmp_target(branch_exit, (uintptr)popall_do_nothing);

                        /* Patch skip branch to here (hot path continues) */
                        write_jmp_target((uae_u32*)branch_skip, (uintptr)get_target());
                    }
                }


                if (failure) {
                    {
                        static int fail_log = 0;
                        if (fail_log < 200) {
                            fail_log++;
                            fprintf(stderr, "JIT_FALLBACK op=%04x pc=%08x comptbl=%p optlev=%d allow_l2=%d\n",
                                (unsigned)opcode, (unsigned)op_m68k_pc,
                                (void*)(comptbl ? comptbl[cft_map(opcode)] : NULL),
                                optlev, (int)allow_l2);
                            fflush(stderr);
                        }
                    }
                    if (jit_force_exact_exec_nostats_opcode((uae_u16)opcode)) {
                        if (was_comp) {
                            flush(1);
                            was_comp = 0;
                        }
                        fprintf(stderr, "JIT_EXACT_EXEC_NOSTATS block=%08x pc=%08x op=%04x\n",
                            (unsigned)block_m68k_pc,
                            (unsigned)(block_m68k_pc + (uae_u32)((uintptr)pc_hist[i].location - (uintptr)pc_hist[0].location)),
                            (unsigned)opcode);
                        compemu_raw_exec_nostats((uintptr)pc_hist[i].location);
                        forced_interpreter_barrier = true;
                        break;
                    }
                    if (was_comp) {
                        flush(1);
                        was_comp = 0;
                    }
                    if (trace_emulopflow_env() && trace_emulopflow_opcode((uae_u16)opcode) && trace_emulopflow_count < trace_emulopflow_limit()) {
                        uae_u16 next_op = (i + 1 < blocklen) ? DO_GET_OPCODE(pc_hist[i + 1].location) : 0xffff;
                        fprintf(stderr,
                            "EMUFLOW %lu COMPILE block=%08x pc=%08x op=%04x next=%04x was_comp=%d live=%08x needed=%08x\n",
                            ++trace_emulopflow_count,
                            (unsigned)block_m68k_pc,
                            (unsigned)(block_m68k_pc + (uae_u32)((uintptr)pc_hist[i].location - (uintptr)pc_hist[0].location)),
                            (unsigned)opcode,
                            (unsigned)next_op,
                            was_comp ? 1 : 0,
                            (unsigned)liveflags[i + 1],
                            (unsigned)needed_flags);
                    }
                    if (was_comp) {
                        /* First-principles mixed-mode fix: before falling back to
                           the interpreter inside a block, flush the full native
                           live state and reset allocator assumptions. Otherwise
                           the fallback sees stale guest state in memory, and any
                           later native ops continue from stale compile-time
                           register/flag facts. */
                        flush(1);
                        comp_pc_p = (uae_u8*)pc_hist[i].location;
                        init_comp();
                        was_comp = 0;
                    }
                    compemu_raw_mov_l_ri(REG_PAR1, (uae_u32)cft_map(opcode));
                    compemu_raw_mov_l_rr(REG_PAR2, R_REGSTRUCT);
                    /* Static-audit fix: interpreter fallback from a direct-chained
                       compiled block must not inherit stale PC base metadata.
                       Many ARM64 L2 helper blocks are mixed native+fallback
                       because shift/rotate families are still forced through the
                       interpreter. Direct chains typically update regs.pc_p, but
                       m68k_getpc() in the fallback/interpreter uses the full
                       triple regs.pc + (regs.pc_p - regs.pc_oldp). Rebuild a
                       self-consistent triple for the current opcode before the
                       fallback call. */
                    compemu_raw_mov_l_mi((uintptr)&regs.pc, op_m68k_pc);
                    compemu_raw_set_pc_i((uintptr)pc_hist[i].location);
                    compemu_raw_mov_l_rm(REG_WORK1, (uintptr)&regs.pc_p);
                    compemu_raw_mov_l_mr((uintptr)&regs.pc_oldp, REG_WORK1);
                    if (jit_trace_target_pc(op_m68k_pc)) {
                        compemu_raw_mov_l_ri(REG_PAR1, op_m68k_pc);
                        compemu_raw_mov_l_ri(REG_PAR2, (2u << 16) | (opcode & 0xffff));
                        compemu_raw_call((uintptr)jit_trace_pc_hit);
                    }
                    compemu_raw_call((uintptr)cputbl[cft_map(opcode)]);
                    /* Trace interpreter-executed family-d instructions */
                    if (((opcode >> 12) & 0xf) == 0xd && getenv("B2_JIT_TRACE_ADD")) {
                        uae_u32 pc_val = (uae_u32)((uintptr)pc_hist[i].location - (uintptr)ROMBaseHost + ROMBaseMac);
                        compemu_raw_mov_l_ri(REG_PAR1, pc_val);
                        compemu_raw_mov_l_ri(REG_PAR2, opcode);
                        compemu_raw_call((uintptr)jit_trace_add);
                    }
#ifdef PROFILE_UNTRANSLATED_INSNS
                    // raw_cputbl_count[] is indexed with plain opcode (in m68k order)
                    compemu_raw_inc_opcount(opcode);
#endif
                    if (trace_emulopflow_env() && trace_emulopflow_opcode((uae_u16)opcode)) {
                        uae_u32 next_pc = 0xffffffff;
                        if (i + 1 < blocklen)
                            next_pc = block_m68k_pc + (uae_u32)((uintptr)pc_hist[i + 1].location - (uintptr)pc_hist[0].location);
                        compemu_raw_mov_l_ri(REG_PAR1, (uae_u32)opcode);
                        compemu_raw_mov_l_ri(REG_PAR2, next_pc);
                        compemu_raw_call((uintptr)trace_emulop_resume);
                    }

                    if (jit_force_interpreter_barrier_opcode((uae_u16)opcode)) {
                        /* Interpreter barrier opcodes already executed via the
                           fallback call above. Runtime keeps using the normal
                           execute_normal() handoff. In verifier mode, if the
                           traced block ends exactly at this barrier, stop at the
                           current regs.pc_p so the comparison stays block-local. */
                        compemu_raw_mov_l_rm(0, (uintptr)specflags);
#if defined(USE_DATA_BUFFER)
                        data_check_end(12, 64);
#endif
                        compemu_raw_maybe_do_nothing(retired_cycles);
                        if (i == blocklen - 1 &&
                            jit_block_verify_compile_active && block_m68k_pc == jit_block_verify_compile_pc) {
                            compemu_raw_mov_l_rm(REG_PC_TMP, (uintptr)&regs.pc_p);
                            compemu_raw_endblock_pc_inreg(REG_PC_TMP, retired_cycles);
                        } else {
                            compemu_raw_execute_normal_cycles((uintptr)&regs.pc_p, retired_cycles);
                        }
                        forced_interpreter_barrier = true;
                        break;
                    }
                    if (jit_end_block_on_fallback_env()) {
                        /* Diagnostic mode: after any interpreter fallback,
                           end the block immediately and return to dispatcher. */
                        compemu_raw_mov_l_rm(0, (uintptr)specflags);
#if defined(USE_DATA_BUFFER)
                        data_check_end(12, 64);
#endif
                        compemu_raw_maybe_do_nothing(retired_cycles);
                        compemu_raw_mov_l_rm(REG_PC_TMP, (uintptr)&regs.pc_p);
                        compemu_raw_endblock_pc_inreg(REG_PC_TMP, retired_cycles);
                        forced_interpreter_barrier = true;
                        break;
                    }
                    if (i < blocklen - 1) {
                        uae_u8* branchadd;

                        /* if (SPCFLAGS_TEST(SPCFLAG_ALL)) popall_do_nothing() */
                        compemu_raw_mov_l_rm(0, (uintptr)specflags);
#if defined(USE_DATA_BUFFER)
                        data_check_end(8, 64);
#endif
                        compemu_raw_maybe_do_nothing(retired_cycles);
                    }
                } else if (may_raise_exception) {
#if defined(USE_DATA_BUFFER)
                    data_check_end(8, 64);
#endif
                    compemu_raw_handle_except(retired_cycles);
                    may_raise_exception = false;
                }
            }
            if (!forced_interpreter_barrier && next_pc_p && taken_pc_p &&
                was_comp && taken_pc_p == current_block_pc_p)
            {
                blockinfo* bi1 = get_blockinfo_addr_new((void*)next_pc_p);
                blockinfo* bi2 = get_blockinfo_addr_new((void*)taken_pc_p);
                uae_u8 x = bi1->needed_flags;

                if (x == 0xff) {  /* Block not yet compiled — use conservative single-op lookahead */
                    uae_u16* next = (uae_u16*)next_pc_p;
                    uae_u32 op = DO_GET_OPCODE(next);

                    x = FLAG_ALL;
                    x &= (~prop[cft_map(op)].set_flags);
                    x |= prop[cft_map(op)].use_flags;
                }
                /* else: bi1->needed_flags already has the full block's requirements */

                x |= bi2->needed_flags;
                if (!(x & FLAG_CZNV)) {
                    /* Save flags to memory FIRST, so the interpreter slow path
                       (countdown < 0 → popall_do_nothing) has correct regflags.nzcv.
                       Then mark flags as unimportant so successor blocks benefit
                       from not needing to restore them. flush(1) will see
                       flags_on_stack==VALID and skip the redundant save. */
                    trace_flagflow_pc = block_m68k_pc;
                    trace_flagflow_op = 0xffff;
                    trace_flagflow_log("JOIN_DROP", bi1->needed_flags, bi2->needed_flags, x, next_pc_p);
                    flush_flags();
                    dont_care_flags();
                }
            }
            log_flush();

            if (!forced_interpreter_barrier && next_pc_p) { /* A branch was registered */
                uintptr t1 = next_pc_p;
                uintptr t2 = taken_pc_p;
                int cc = branch_cc; // from gencomp (x86 condition code encoding)

#if defined(CPU_AARCH64)
                /* gencomp.c generates condition codes using flags_x86.h encoding.
                   Map to ARM/AArch64 NATIVE_CC_* values for compemu_raw_jcc_l_oponly. */
                {
                    static const int x86_to_arm_cc[] = {
                        /* x86 0=VS */ NATIVE_CC_VS,  /* x86 1=VC */ NATIVE_CC_VC,
                        /* x86 2=CS */ NATIVE_CC_CS,  /* x86 3=CC */ NATIVE_CC_CC,
                        /* x86 4=EQ */ NATIVE_CC_EQ,  /* x86 5=NE */ NATIVE_CC_NE,
                        /* x86 6=LS */ NATIVE_CC_LS,  /* x86 7=HI */ NATIVE_CC_HI,
                        /* x86 8=MI */ NATIVE_CC_MI,  /* x86 9=PL */ NATIVE_CC_PL,
                        /* x86 10=VS2 */ NATIVE_CC_VS, /* x86 11=VC2 */ NATIVE_CC_VC,
                        /* x86 12=LT */ NATIVE_CC_LT, /* x86 13=GE */ NATIVE_CC_GE,
                        /* x86 14=LE */ NATIVE_CC_LE, /* x86 15=GT */ NATIVE_CC_GT,
                    };
                    if (cc >= 0 && cc < 16)
                        cc = x86_to_arm_cc[cc];
                }
#endif

                uae_u32* branchadd;
                uae_u32* tba;
                bigstate tmp;
                blockinfo* tbi;
                const uae_u16 final_op = DO_GET_OPCODE(pc_hist[blocklen - 1].location);
                const bool final_is_dbcc = ((final_op & 0x00f8) == 0x00c8 && (final_op & 0xf000) == 0x5000);

#if defined(CPU_AARCH64)
                /* ARM64 bringup: keep backward branches on the simple
                   forward-style layout too. The old taken-path inversion
                   optimization still misroutes some DBcc-controlled loops
                   (for example the hot 04009abc -> 04009ab0 ROM loop),
                   leaving the guest stuck before CHECKLOAD/VIDEOINT.
                   Preserve semantics first; re-introduce prediction later
                   once the branch inversion path is proven correct. */
#else
                if (taken_pc_p < next_pc_p) {
                    /* backward branch. Optimize for the "taken" case ---
                       which means the raw_jcc should fall through when
                       the 68k branch is taken. */
                    t1 = taken_pc_p;
                    t2 = next_pc_p;
                    if (cc < NATIVE_CC_AL)
                        cc = cc ^ 1;
                    else if (cc > NATIVE_CC_AL)
                        cc = 0x10 | (cc ^ 0xf);
                }
#endif

#if defined(USE_DATA_BUFFER)
                data_check_end(8, 128);
#endif
                flush(1);                       // Emitted code of this call doesn't modify flags
                compemu_raw_jcc_l_oponly(cc);   // Last emitted opcode is branch to target
                branchadd = (uae_u32*)get_target() - 1;

                /* predicted outcome */
                uintptr ct1 = jit_canonicalize_target_pc(t1);
                tbi = get_blockinfo_addr_new((void*)ct1);
                match_states(tbi);
                if (jit_trace_setpc_env()) {
                    uintptr _base = (uintptr)RAMBaseHost;
                    uintptr _limit = _base + RAMSize + ROMSize + 0x100000;
                    if (t1 < _base || t1 >= _limit || (t1 & 1)) {
                        fprintf(stderr,
                            "JIT_ENDCONST src=%08x path=pred target=%p canon=%p next=%p taken=%p cc=%d\n",
                            (unsigned)block_m68k_pc, (void*)t1, (void*)ct1, (void*)next_pc_p, (void*)taken_pc_p, cc);
                    }
                }

                /* Use endblock_pc_isconst for ALL blocks (including DBF)
                   to enable countdown + direct chaining. */
                tba = compemu_raw_endblock_pc_isconst(scaled_cycles(totcycles), ct1);
                write_jmp_target(tba, get_handler(ct1));
                create_jmpdep(bi, 0, tba, ct1);

                /* not-predicted outcome */
                write_jmp_target(branchadd, (uintptr)get_target());
                uintptr ct2 = jit_canonicalize_target_pc(t2);
                tbi = get_blockinfo_addr_new((void*)ct2);
                match_states(tbi);
                if (jit_trace_setpc_env()) {
                    uintptr _base = (uintptr)RAMBaseHost;
                    uintptr _limit = _base + RAMSize + ROMSize + 0x100000;
                    if (t2 < _base || t2 >= _limit || (t2 & 1)) {
                        fprintf(stderr,
                            "JIT_ENDCONST src=%08x path=notpred target=%p canon=%p next=%p taken=%p cc=%d\n",
                            (unsigned)block_m68k_pc, (void*)t2, (void*)ct2, (void*)next_pc_p, (void*)taken_pc_p, cc);
                    }
                }

                tba = compemu_raw_endblock_pc_isconst(scaled_cycles(totcycles), ct2);
                write_jmp_target(tba, get_handler(ct2));
                create_jmpdep(bi, 1, tba, ct2);
            } else if (!forced_interpreter_barrier) {
                if (was_comp) {
                    flush(1);
                } else {
                    /* Interpreter-only / fallback-only blocks can still leave
                       pending SPCFLAGS (for example A-line/F-line/illegal-op
                       exceptions). Their endblock path must not hot-chain past
                       those requests; re-check spcflags before choosing the
                       successor so exception/interrupt handling returns to C. */
                    compemu_raw_mov_l_rm(0, (uintptr)specflags);
#if defined(USE_DATA_BUFFER)
                    data_check_end(4, 64);
#endif
                    compemu_raw_maybe_do_nothing(scaled_cycles(totcycles));
                }

                /* Let's find out where next_handler is... */
                if (was_comp && isinreg(PC_P)) {
#if defined(USE_DATA_BUFFER)
                    data_check_end(4, 64);
#endif
                    r = live.state[PC_P].realreg;
                    compemu_raw_endblock_pc_inreg(r, scaled_cycles(totcycles));
                } else if (was_comp && isconst(PC_P)) {
                    uintptr v = live.state[PC_P].val;
#if defined(CPU_AARCH64)
                    /* ARM64: flush(1) already wrote PC_P to regs.pc_p via
                       writeback_const. Use direct B chaining through
                       endblock_pc_isconst — the full PC triple is stored
                       on its hot path. Do NOT redundantly write PC_P here
                       (that was the bug — conflicting with flush's write). */
                    {
                    uae_u32* tba;
                    blockinfo* tbi;
                    uintptr cv = jit_canonicalize_target_pc(v);
                    tbi = get_blockinfo_addr_new((void*)cv);
                    match_states(tbi);
#if defined(USE_DATA_BUFFER)
                    data_check_end(4, 64);
#endif
                    tba = compemu_raw_endblock_pc_isconst(scaled_cycles(totcycles), cv);
                    write_jmp_target(tba, get_handler(cv));
                    create_jmpdep(bi, 0, tba, cv);
                    }
#else /* !CPU_AARCH64 */
                    {
                    uae_u32* tba;
                    blockinfo* tbi;
                    const uae_u16 final_op = DO_GET_OPCODE(pc_hist[blocklen - 1].location);
                    const bool final_is_braq = ((final_op & 0xff00) == 0x6000 && final_op != 0x6000 && final_op != 0x60ff);
                    uintptr cv = jit_canonicalize_target_pc(v);
                    tbi = get_blockinfo_addr_new((void*)cv);
                    match_states(tbi);
#if defined(USE_DATA_BUFFER)
                    data_check_end(4, 64);
#endif
                    tba = compemu_raw_endblock_pc_isconst(scaled_cycles(totcycles), cv);
                    write_jmp_target(tba, get_handler(cv));
                    create_jmpdep(bi, 0, tba, cv);
                    }
#endif /* CPU_AARCH64 */
                } else {
                    r = REG_PC_TMP;
                    compemu_raw_mov_l_rm(r, (uintptr)&regs.pc_p);
#if defined(USE_DATA_BUFFER)
                    data_check_end(4, 64);
#endif
                    compemu_raw_endblock_pc_inreg(r, scaled_cycles(totcycles));
                }
            }
        }
endblock_done:

        remove_from_list(bi);
        if (trace_in_rom) {
            // No need to checksum that block trace on cache invalidation
            free_checksum_info_chain(bi->csi);
            bi->csi = NULL;
            add_to_dormant(bi);
        } else {
            calc_checksum(bi, &(bi->c1), &(bi->c2));
            add_to_active(bi);
        }

        const unsigned emitted_bytes = (unsigned)(JITPTR get_target() - JITPTR current_compile_p);
        current_cache_size += emitted_bytes;
        jit_diag_note_compile_block((unsigned)blocklen, (unsigned)totcycles, emitted_bytes, (unsigned long long)current_cache_size);
        jit_diag_maybe_print();

        /* This is the non-direct handler */
        bi->handler = bi->handler_to_use = (cpuop_func*)get_target();
        compemu_raw_cmp_pc((uintptr)pc_hist[0].location);
        compemu_raw_maybe_cachemiss();
        comp_pc_p = (uae_u8*)pc_hist[0].location;

        bi->status = BI_FINALIZING;
        init_comp();
        match_states(bi);
        flush(1);

        compemu_raw_jmp((uintptr)bi->direct_handler);

        /* Structural diagnostic mode: route all successor handoffs through the
           non-direct wrapper so every chained entry revalidates pc/cache state.
           This also covers the existing optlev=0 case, where exec_nostats()
           requires fully canonical in-memory guest state. */
        if (!was_comp || jit_force_nondirect_handler_env() || jit_force_nondirect_target_env((uintptr)bi->pc_p)) {
            set_dhtu(bi, bi->handler);
        }



        flush_cpu_icache((void*)current_block_start_target, (void*)target);
        current_compile_p = get_target();
        raise_in_cl_list(bi);
        bi->nexthandler = current_compile_p;

        /* We will flush soon, anyway, so let's do it now */
        if (current_compile_p >= MAX_COMPILE_PTR)
            flush_icache_lazy(3);

        bi->status = BI_ACTIVE;
        if (redo_current_block)
            block_need_recompile(bi);

#ifdef JIT_DEBUG_MEM_CORRUPTION
        jit_dbg_check_vec2_dispatch("compile_block_end");
#endif

#ifdef PROFILE_COMPILE_TIME
        compile_time += (clock() - start_time);
#endif
#ifdef USE_CPU_EMUL_SERVICES
        cpu_do_check_ticks();
#endif
		jit_end_write_window();
    }
}

#endif /* JIT */

