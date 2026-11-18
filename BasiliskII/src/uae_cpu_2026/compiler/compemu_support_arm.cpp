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
#include "timer.h"
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
#include "jit_native_helpers.h"
#include "my_sdl.h"

extern "C" void jit_op_bftst(void);
extern "C" void jit_op_bfextu(void);
extern "C" void jit_op_bfchg(void);
extern "C" void jit_op_bfexts(void);
extern "C" void jit_op_bfclr(void);
extern "C" void jit_op_bfffo(void);
extern "C" void jit_op_bfset(void);
extern "C" void jit_op_bfins(void);

/* ARM64 JIT is PIE-compatible: it uses register-indirect addressing
 * (R_MEMSTART/R15) rather than PC-relative globals, so code placement
 * relative to .data does not matter. */

#ifdef __MACH__
// Needed for sys_cache_invalidate on the JIT space region, Mac OS X specific
#include <libkern/OSCacheControl.h>
#endif

#if defined(__APPLE__) && defined(CPU_AARCH64)
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
/* Apple Silicon enforces W^X on MAP_JIT pages per thread:
   enable=true -> executable (writes fault), enable=false -> writable. */
static inline void uae_vm_jit_write_protect(bool enable)
{
	pthread_jit_write_protect_np(enable ? 1 : 0);
}
static inline unsigned long uae_vm_page_size(void)
{
	return (unsigned long)getpagesize();
}
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

/* Strict mode verifies execution policy, not opcode semantics: first-seen
   execute_normal tracing is intrinsic to this trace JIT, but translated blocks
   may not contain interpreter-table calls, optlev-0 stubs, or exec_nostats
   entries. Guest state and cache invalidation semantics remain unchanged. */
static inline bool jit_guest_path_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = getenv("B2_JIT_GUEST_PATH");
        enabled = env && *env && strcmp(env, "0") != 0;
    }
    return enabled != 0;
}

static inline bool jit_guest_path_arm_start_env(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = getenv("B2_JIT_GUEST_PATH_ARM_START");
        enabled = env && *env && strcmp(env, "0") != 0;
    }
    return enabled != 0;
}

enum { JIT_GUEST_PATH_CAPACITY = 16777216 };
/* This diagnostic buffer is 64 MiB. Allocate it only when capture is armed so
   ordinary emulator runs pay neither resident-memory nor BSS-image cost. */
static uae_u32* jit_guest_path_ring = NULL;
static unsigned long jit_guest_path_index = 0;
static bool jit_guest_path_armed = false;
static bool jit_guest_path_dumped = false;

static void jit_guest_path_dump(const char* reason)
{
    if (jit_guest_path_dumped)
        return;
    jit_guest_path_dumped = true;
    const unsigned long start = jit_guest_path_index > JIT_GUEST_PATH_CAPACITY
        ? jit_guest_path_index - JIT_GUEST_PATH_CAPACITY : 0;
    const char* path = getenv("B2_JIT_GUEST_PATH_FILE");
    fprintf(stderr, "JIT_GUEST_PATH_BEGIN reason=%s count=%lu total=%lu file=%s\n",
        reason, jit_guest_path_index - start, jit_guest_path_index,
        path ? path : "stderr");
    if (path && *path) {
        FILE* f = fopen(path, "wb");
        if (f) {
            for (unsigned long i = start; i < jit_guest_path_index; i++) {
                const uae_u32 pc = jit_guest_path_ring[i & (JIT_GUEST_PATH_CAPACITY - 1)];
                fwrite(&pc, sizeof(pc), 1, f);
            }
            fclose(f);
        }
    } else {
        for (unsigned long i = start; i < jit_guest_path_index; i++)
            fprintf(stderr, "JIT_GUEST_PATH step=%lu pc=%08x\n",
                i, jit_guest_path_ring[i & (JIT_GUEST_PATH_CAPACITY - 1)]);
    }
    fprintf(stderr, "JIT_GUEST_PATH_END reason=%s\n", reason);
}

extern void jit_one_tick(void);
extern "C" {
extern uae_u32 jit_current_interp_pc;
extern uae_u32 jit_current_interp_opcode;
}

static inline unsigned long jit_retirement_tick_every(void)
{
    static unsigned long value = 0;
    static bool initialized = false;
    if (!initialized) {
        const char *env = getenv("B2_JIT_RETIREMENT_TICK_EVERY");
        value = env && *env ? strtoul(env, NULL, 0) : 0;
        initialized = true;
    }
    return value;
}

/* Product execution has no per-instruction callback. This observer is emitted
   only for an explicitly requested path capture or deterministic retirement-
   tick run. Keep the scheduler independent of capture arming: asking for
   retirement ticks must work without also allocating or recording a path. */
extern "C" bool jit_guest_instruction_observer_enabled(void)
{
    return jit_guest_path_enabled() || jit_retirement_tick_every() != 0;
}

static void jit_guest_path_record(uae_u32 pc)
{
    static uae_u32 target = 0;
    static unsigned long target_after = 0;
    static unsigned long count_target = 0;
    static bool initialized = false;
    if (!jit_guest_path_enabled())
        return;
    if (!initialized) {
        const char* env = getenv("B2_JIT_GUEST_PATH_TARGET");
        target = env && *env ? (uae_u32)strtoul(env, NULL, 0) : 0;
        const char* after_env = getenv("B2_JIT_GUEST_PATH_TARGET_AFTER");
        target_after = after_env && *after_env ? strtoul(after_env, NULL, 0) : 0;
        const char* count_env = getenv("B2_JIT_GUEST_PATH_COUNT");
        count_target = count_env && *count_env ? strtoul(count_env, NULL, 0) : 0;
        initialized = true;
    }
    if (jit_guest_path_arm_start_env() && !jit_guest_path_armed)
        jit_guest_path_armed = true;
    if (!jit_guest_path_armed)
        return;
    if (!jit_guest_path_ring) {
        jit_guest_path_ring = (uae_u32*)malloc(sizeof(uae_u32) * JIT_GUEST_PATH_CAPACITY);
        if (!jit_guest_path_ring) {
            fprintf(stderr, "JIT_GUEST_PATH allocation failed (%lu bytes)\n",
                (unsigned long)(sizeof(uae_u32) * JIT_GUEST_PATH_CAPACITY));
            abort();
        }
    }
    jit_guest_path_ring[jit_guest_path_index++ & (JIT_GUEST_PATH_CAPACITY - 1)] = pc;
    if (target && jit_guest_path_index >= target_after && pc == target)
        jit_guest_path_dump("target");
    else if (count_target && jit_guest_path_index >= count_target)
        jit_guest_path_dump("count");
}

static void jit_guest_instruction_retired(uae_u32 pc)
{
    static unsigned long retirement_count = 0;
    const unsigned long tick_every = jit_retirement_tick_every();
    if (tick_every && (++retirement_count % tick_every) == 0)
        jit_one_tick();
    jit_guest_path_record(pc);
}

extern "C" void jit_guest_path_record_native(uae_u32 pc)
{
    jit_guest_instruction_retired(pc);
}

extern "C" void jit_guest_path_record_reference(uae_u32 pc)
{
    /* Verifier replay is not architectural retirement and must not advance the
       deterministic clock or inject an extra interrupt. */
    jit_guest_path_record(pc);
}

extern "C" void jit_guest_path_record_nostats(uae_u32 pc)
{
    jit_guest_instruction_retired(pc);
}

extern "C" void jit_guest_path_record_trace(uae_u32 pc)
{
    jit_guest_instruction_retired(pc);
}

static inline bool jit_strict_full_jit_env(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *env = getenv("B2_JIT_STRICT_FULL");
		cached = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
	}
	return cached != 0;
}

static unsigned long long jit_strict_trace_ops = 0;
static unsigned long long jit_strict_native_entries = 0;
static unsigned long long jit_strict_trace_warmups = 0;
static unsigned long long jit_strict_verify_references = 0;
static unsigned long long jit_strict_compiled_blocks = 0;

static inline bool jit_strict_allow_verifier_reference(void)
{
	const char *env = getenv("B2_JIT_ALLOW_VERIFY_REFERENCE");
	return env && *env && strcmp(env, "0") != 0;
}

/* Test-only fault injection for the fail-closed contract. It is effective only
   inside strict mode and forces the generic fallback branch to prove that the
   branch aborts before emitting or executing an opcode-table call. */
static inline bool jit_strict_probe_opcode_fallback(void)
{
	if (!jit_strict_full_jit_env())
		return false;
	const char *env = getenv("B2_JIT_STRICT_PROBE_OPCODE_FALLBACK");
	return env && *env && strcmp(env, "0") != 0;
}

static inline void jit_strict_note_trace_op(uae_u32 pc, uae_u32 opcode)
{
	(void)pc;
	(void)opcode;
	if (jit_strict_full_jit_env())
		jit_strict_trace_ops++;
}

/* A trace JIT must observe mutable RAM before caching it. Strict mode never
   installs an optlev-0/exec_nostats block: cold RAM is retired by the existing
   first-seen tracer, and only a repeatedly visited PC is handed to compile_block
   (which then selects L2). This avoids filling the cache with one-shot generated
   RAM while keeping interpreter fallback paths forbidden. */
static bool jit_strict_defer_cold_ram_trace(cpu_history *pc_hist, int blocklen)
{
	if (!jit_strict_full_jit_env() || blocklen <= 0 || !pc_hist[0].location)
		return false;
	const char *force_l2_ram = getenv("B2_TEST_FORCE_L2_RAM");
	if (force_l2_ram && *force_l2_ram && strcmp(force_l2_ram, "0") != 0)
		return false;
	const char *force_translate = getenv("B2_JIT_FORCE_TRANSLATE");
	if (force_translate && *force_translate && strcmp(force_translate, "0") != 0)
		return false;
	const uae_u32 pc = get_virtual_address((uae_u8 *)pc_hist[0].location);
	if (pc >= (uae_u32)ROMBaseMac)
		return false;
	enum { STRICT_RAM_HOT_SLOTS = 16384, STRICT_RAM_HOT_THRESHOLD = 10 };
	struct hot_slot { uae_u32 pc; uae_u16 visits; };
	static hot_slot slots[STRICT_RAM_HOT_SLOTS] = {};
	hot_slot &slot = slots[(pc >> 1) & (STRICT_RAM_HOT_SLOTS - 1)];
	if (slot.pc != pc) {
		slot.pc = pc;
		slot.visits = 0;
	}
	if (slot.visits < STRICT_RAM_HOT_THRESHOLD)
		slot.visits++;
	if (slot.visits < STRICT_RAM_HOT_THRESHOLD) {
		jit_strict_trace_warmups++;
		return true;
	}
	return false;
}

static void jit_strict_report_native_entry(uae_u32 pc)
{
	/* The generated native entry has already incremented the counter and calls
	   here only when it reaches a power of two.  Keeping the common path out of
	   C avoids making branch-dense strict workloads measure observer overhead. */
	fprintf(stderr,
		"JIT_STRICT_SUMMARY native=%llu trace=%llu warmup=%llu verify=%llu blocks=%llu opt0=0 fallback=0 exec_nostats=0 pc=%08x\n",
		jit_strict_native_entries, jit_strict_trace_ops,
		jit_strict_trace_warmups, jit_strict_verify_references,
		jit_strict_compiled_blocks, (unsigned)pc);
	fflush(stderr);
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
	}
	return value;
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
static int jit_block_verify_compiled_ops = 0;
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
    /* Verifier snapshots should compare architectural SR, not whatever stale
       regs.sr value was last materialized for diagnostics. JIT flags live in
       regflags/NZCV between helpers, so sync SR before taking a snapshot. */
    MakeSR();
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

static inline uae_u32 jit_block_verify_arch_spcflags(uae_u32 spcflags)
{
#if defined(USE_JIT)
    /* SPCFLAG_JIT_* bits are compiler/dispatcher control state, not guest
       architectural state.  The interpreter side can legitimately finish a
       verifier replay with JIT_END_COMPILE set while the native replay exits
       through a clean block boundary. */
    return spcflags & ~(SPCFLAG_JIT_END_COMPILE | SPCFLAG_JIT_EXEC_RETURN);
#else
    return spcflags;
#endif
}

static void jit_block_verify_arch_regs(const regstruct *src, regstruct *dst)
{
    memcpy(dst, src, sizeof(*dst));
    dst->spcflags = jit_block_verify_arch_spcflags(dst->spcflags);
}

static void jit_block_verify_compare(const jit_block_verify_snapshot *expected, const jit_block_verify_snapshot *actual, uae_u32 block_pc, int blocklen)
{
    regstruct expected_regs, actual_regs;
    jit_block_verify_arch_regs(&expected->regs, &expected_regs);
    jit_block_verify_arch_regs(&actual->regs, &actual_regs);
    if (expected_regs.pc_p == actual_regs.pc_p) {
        /* regs.pc/fault_pc/pc_oldp can carry the last dispatch/fault metadata
           from different but equivalent block segmentations.  If the current
           executable PC agrees, do not report those bookkeeping fields as a
           guest-visible verifier mismatch. */
        expected_regs.pc = actual_regs.pc;
        expected_regs.fault_pc = actual_regs.fault_pc;
        expected_regs.pc_oldp = actual_regs.pc_oldp;
    }

    bool mismatch = false;
    if (memcmp(&expected_regs, &actual_regs, sizeof(regs)) != 0)
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
        if (expected_regs.regs[i] != actual_regs.regs[i]) {
            fprintf(stderr, "  reg[%d] interp=%08x native=%08x\n", i,
                (unsigned)expected_regs.regs[i], (unsigned)actual_regs.regs[i]);
        }
    }
    if (expected_regs.pc != actual_regs.pc || expected_regs.fault_pc != actual_regs.fault_pc ||
        expected_regs.pc_p != actual_regs.pc_p || expected_regs.pc_oldp != actual_regs.pc_oldp ||
        expected_regs.sr != actual_regs.sr || expected_regs.spcflags != actual_regs.spcflags) {
        fprintf(stderr,
            "  pc interp=%08x/%p/%p native=%08x/%p/%p sr interp=%04x native=%04x spc interp=%08x native=%08x\n",
            (unsigned)expected_regs.pc, (void*)expected_regs.pc_p, (void*)expected_regs.pc_oldp,
            (unsigned)actual_regs.pc, (void*)actual_regs.pc_p, (void*)actual_regs.pc_oldp,
            (unsigned)expected_regs.sr, (unsigned)actual_regs.sr,
            (unsigned)expected_regs.spcflags, (unsigned)actual_regs.spcflags);
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
    if (jit_strict_full_jit_env() && !jit_strict_allow_verifier_reference())
        jit_abort("strict full-JIT: verifier interpreter reference block=%08x", block_pc);
    if (jit_strict_full_jit_env())
        jit_strict_verify_references++;
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

    /* === HARDENED VERIFIER (ported from @previous, 2026-06-30 alignment note) ===
       Delta 1: NATIVE-FIRST. Compile the block under the sandbox and run it,
       capturing its NATURAL stop PC. countdown=-1 forces every block-exit edge
       to the dispatcher (popall_do_nothing) with regs.pc_p set to the exit
       target, so the native run is bounded to ONE block (no chain) without an
       explicit get_handler_for_edge override. */
    jit_block_verify_snapshot_restore(&jit_block_verify_entry_state);
    regs.spcflags = 0;
    InterruptFlags = 0;
    jit_block_verify_compile_active = true;
    jit_block_verify_compile_pc = block_pc;
    jit_block_verify_compiled_ops = 0;
    compile_block(pc_hist, blocklen, total_cycles);
    jit_block_verify_compile_active = false;
    jit_block_verify_compile_pc = 0xffffffffu;

    countdown = -1;
    jit_block_verify_reentrant = true;
    ((jit_compiled_handler)pushall_call_handler)();
    jit_block_verify_reentrant = false;

    if (!jit_block_verify_snapshot_capture(&native)) {
#if defined(CPU_AARCH64)
        tick_inhibit = saved_tick_inhibit;
#endif
        jit_block_verify_snapshot_restore(&resume);
        jit_block_verify_snapshot_free(&resume);
        jit_block_verify_entry_reset();
        return;
    }
    const uae_u32 native_stop_pc =
        (uae_u32)get_virtual_address((uae_u8*)native.regs.pc_p);

    /* Delta 2: INTERP REFERENCE with an exact retirement bound. The block
       builder terminates at control flow, while runtime-helper barriers can
       deliberately end emitted native code even earlier. The compiler records
       the last guest op it emitted. Stopping merely when the reference first
       visits native_stop_pc is invalid for backward branches whose target lies
       inside the block; replay exactly native's emitted-op count instead. */
    jit_block_verify_snapshot_restore(&jit_block_verify_entry_state);
    regs.spcflags = 0;
    InterruptFlags = 0;
    const int reference_ops = jit_block_verify_compiled_ops > 0
        ? jit_block_verify_compiled_ops : blocklen;
    for (int step = 0; step < reference_ops; step++) {
        uae_u32 opcode = get_opcode_cft_map((uae_u16)*(uae_u16*)regs.pc_p);
        (*cpufunctbl[opcode])(opcode);
    }
    const bool interp_reached_stop = ((uae_u32)m68k_getpc() == native_stop_pc);
    if (!jit_block_verify_snapshot_capture(&interp)) {
#if defined(CPU_AARCH64)
        tick_inhibit = saved_tick_inhibit;
#endif
        jit_block_verify_snapshot_restore(&resume);
        jit_block_verify_snapshot_free(&resume);
        jit_block_verify_snapshot_free(&native);
        jit_block_verify_entry_reset();
        return;
    }

    /* Delta 3: APPLES-TO-APPLES REACH GUARD. Compare only when the interpreter
       reference actually stopped at native's stop PC; else emit SKIP-NOREACH
       (which, when native_stop_pc is a PC interp never visits in the boot, is
       the phantom-successor / control-flow divergence signal). */
    if (interp_reached_stop) {
        fprintf(stderr, "JITBLOCKVERIFY block=%08x len=%d native_ops=%d REACHED native_stop_pc=%08x interp_stop_pc=%08x\n",
            (unsigned)block_pc, blocklen, reference_ops,
            (unsigned)native_stop_pc, (unsigned)m68k_getpc());
        jit_block_verify_compare(&interp, &native, block_pc, blocklen);
    } else
        fprintf(stderr, "JITBLOCKVERIFY block=%08x len=%d native_ops=%d SKIP-NOREACH interp_pc=%08x native_pc=%08x\n",
            (unsigned)block_pc, blocklen, reference_ops,
            (unsigned)m68k_getpc(), (unsigned)native_stop_pc);
    jit_block_verify_snapshot_free(&native);

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

static void jit_verify_post(uae_u32 pc, uae_u32 tagged_opcode)
{
	if (jit_strict_full_jit_env() && !jit_strict_allow_verifier_reference())
		jit_abort("strict full-JIT: verifier opcode reference pc=%08x op=%04x",
			pc, tagged_opcode & 0xffffu);
	if (jit_strict_full_jit_env())
		jit_strict_verify_references++;
	if (!jit_verify_pre_valid)
		return;
	/* The emitter tags the opcode with its live-flag mask in the upper bits. */
	const uae_u32 opcode = tagged_opcode & 0xffffu;
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
	bool reg_mismatch = false;
	for (int ri = 0; ri < 16; ri++) {
		if (regs.regs[ri] != compiled_post.regs.regs[ri]) {
			reg_mismatch = true;
			mismatch = true;
			break;
		}
	}
	if (interp_mem_a2 != compiled_post.mem_a2_byte ||
		interp_mem_a2_p400 != compiled_post.mem_a2_p400_byte) {
		reg_mismatch = true;
		mismatch = true;
	}
	if (regflags.nzcv != compiled_post.flags.nzcv ||
		regflags.x != compiled_post.flags.x)
		mismatch = true;

	// Skip flag-only mismatches (lazy flag updates are expected and not bugs).
	static const bool log_flag_only = getenv("B2_JIT_VERIFY_FLAG_ONLY") != NULL;
	bool should_log = reg_mismatch || (mismatch && log_flag_only);

	if (should_log && jit_verify_log_count < 400) {
		fprintf(stderr,
			"JITVERIFY pc=%08x op=%04x interp:d0=%08x d1=%08x d2=%08x d3=%08x d4=%08x d5=%08x d6=%08x d7=%08x a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x nzcv=%08x x=%08x m[a2]=%02x m[a2+400]=%02x compiled:d0=%08x d1=%08x d2=%08x d3=%08x d4=%08x d5=%08x d6=%08x d7=%08x a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x a7=%08x nzcv=%08x x=%08x m[a2]=%02x m[a2+400]=%02x\n",
			(unsigned)pc, (unsigned)opcode,
			(unsigned)regs.regs[0], (unsigned)regs.regs[1], (unsigned)regs.regs[2], (unsigned)regs.regs[3],
			(unsigned)regs.regs[4], (unsigned)regs.regs[5], (unsigned)regs.regs[6], (unsigned)regs.regs[7],
			(unsigned)regs.regs[8], (unsigned)regs.regs[9], (unsigned)regs.regs[10], (unsigned)regs.regs[11],
			(unsigned)regs.regs[12], (unsigned)regs.regs[13], (unsigned)regs.regs[14], (unsigned)regs.regs[15],
			regflags.nzcv, regflags.x,
			(unsigned)interp_mem_a2, (unsigned)interp_mem_a2_p400,
			(unsigned)compiled_post.regs.regs[0], (unsigned)compiled_post.regs.regs[1],
			(unsigned)compiled_post.regs.regs[2], (unsigned)compiled_post.regs.regs[3],
			(unsigned)compiled_post.regs.regs[4], (unsigned)compiled_post.regs.regs[5],
			(unsigned)compiled_post.regs.regs[6], (unsigned)compiled_post.regs.regs[7],
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

void flush(int save_regs);

static void op_fullsr_orsr_w_comp_ff(uae_u32 opcode);
static void op_fullsr_andsr_w_comp_ff(uae_u32 opcode);
static void op_fullsr_eorsr_w_comp_ff(uae_u32 opcode);
static void op_fullsr_mv2sr_w_comp_ff(uae_u32 opcode);
static void op_bitfield_comp_ff(uae_u32 opcode);
static void op_cas_comp_ff(uae_u32 opcode);
static void op_rts_comp_ff(uae_u32 opcode);
static void op_bsr_comp_ff(uae_u32 opcode);
extern "C" void jit_trace_add(uae_u32 pc, uae_u32 opcode);
extern "C" void jit_trace_pc_hit(uae_u32 pc, uae_u32 tagged_opcode);
static void op_aline_trap_comp_ff(uae_u32 opcode);
static void op_illegal_trap_comp_ff(uae_u32 opcode);
static void op_emulop_comp_ff(uae_u32 opcode);
static void op_fsave_comp_ff(uae_u32 opcode);
static void op_frestore_comp_ff(uae_u32 opcode);
static void op_fpu_semantic_comp_ff(uae_u32 opcode);
static uintptr jit_compile_current_op_host_pc = 0;
static uae_u32 jit_compile_current_op_m68k_pc = 0;

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

static inline uae_u32 jit_hostpc_to_macpc(uintptr hostpc)
{
    return get_virtual_address((uae_u8*)hostpc);
}

static inline bool jit_trace_edges_env(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("B2_JIT_TRACE_EDGES") && *getenv("B2_JIT_TRACE_EDGES") && strcmp(getenv("B2_JIT_TRACE_EDGES"), "0") != 0) ? 1 : 0;
    return cached != 0;
}

static inline unsigned long jit_trace_edges_limit(void)
{
    static unsigned long value = 0;
    static bool init = false;
    if (!init) {
        const char *env = getenv("B2_JIT_TRACE_EDGES_LIMIT");
        value = env && *env ? strtoul(env, NULL, 0) : 400;
        init = true;
    }
    return value;
}

static unsigned long jit_trace_edges_count = 0;

static inline uae_u32 jit_stable_edge_min_exec_env(void)
{
    static uae_u32 value = 0;
    static bool init = false;
    if (!init) {
        const char *env = getenv("B2_JIT_STABLE_EDGE_MIN_EXEC");
        value = env && *env ? (uae_u32)strtoul(env, NULL, 0) : 32;
        init = true;
    }
    return value;
}

static inline uae_u32 jit_stable_edge_min_pct_env(void)
{
    static uae_u32 value = 0;
    static bool init = false;
    if (!init) {
        const char *env = getenv("B2_JIT_STABLE_EDGE_MIN_PCT");
        value = env && *env ? (uae_u32)strtoul(env, NULL, 0) : 80;
        if (value > 100)
            value = 100;
        init = true;
    }
    return value;
}

static inline uae_u32 jit_stable_edge_profile_exec_env(void)
{
    static uae_u32 value = 0;
    static bool init = false;
    if (!init) {
        const char *env = getenv("B2_JIT_STABLE_EDGE_PROFILE_EXEC");
        value = env && *env ? (uae_u32)strtoul(env, NULL, 0) : 16;
        if (value < 1)
            value = 1;
        if (value < jit_stable_edge_min_exec_env())
            value = jit_stable_edge_min_exec_env();
        init = true;
    }
    return value;
}

static inline bool jit_enable_stable_direct_edges_env(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("B2_JIT_ENABLE_STABLE_DIRECT_EDGES") && *getenv("B2_JIT_ENABLE_STABLE_DIRECT_EDGES") && strcmp(getenv("B2_JIT_ENABLE_STABLE_DIRECT_EDGES"), "0") != 0) ? 1 : 0;
    return cached != 0;
}

static inline bool jit_stable_direct_rom_only_env(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("B2_JIT_STABLE_DIRECT_ROM_ONLY");
#if defined(CPU_AARCH64)
        cached = (!env || !*env || strcmp(env, "0") != 0) ? 1 : 0;
#else
        cached = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
#endif
    }
    return cached != 0;
}

static inline bool jit_trace_stable_direct_env(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("B2_JIT_TRACE_STABLE_DIRECT") && *getenv("B2_JIT_TRACE_STABLE_DIRECT") && strcmp(getenv("B2_JIT_TRACE_STABLE_DIRECT"), "0") != 0) ? 1 : 0;
    return cached != 0;
}

static inline unsigned long jit_trace_stable_direct_limit(void)
{
    static unsigned long value = 0;
    static bool init = false;
    if (!init) {
        const char *env = getenv("B2_JIT_TRACE_STABLE_DIRECT_LIMIT");
        value = env && *env ? strtoul(env, NULL, 0) : 200;
        init = true;
    }
    return value;
}

static unsigned long jit_trace_stable_direct_count = 0;

static inline void write_jmp_target(uae_u32* jmpaddr, uintptr a);

static inline int jit_dominant_edge_index(const blockinfo *bi)
{
    if (!bi)
        return -1;
    if (bi->edge_exec_count[0] > bi->edge_exec_count[1])
        return 0;
    if (bi->edge_exec_count[1] > bi->edge_exec_count[0])
        return 1;
    return -1;
}

static inline bool jit_dominant_edge_stable(const blockinfo *bi)
{
    if (!bi)
        return false;
    const uae_u32 total = bi->edge_exec_count[0] + bi->edge_exec_count[1];
    const int dominant = jit_dominant_edge_index(bi);
    if (dominant < 0 || total == 0)
        return false;
    const uae_u32 dom_count = bi->edge_exec_count[dominant];
    if (dom_count < jit_stable_edge_min_exec_env())
        return false;
    return (uae_u64)dom_count * 100 >= (uae_u64)total * jit_stable_edge_min_pct_env();
}

static inline int jit_dependency_edge_slot(const dependency *d)
{
    if (!d || !d->source)
        return -1;
    if (d == &d->source->dep[0])
        return 0;
    if (d == &d->source->dep[1])
        return 1;
    return -1;
}

static void jit_trace_stable_direct_event(const char *tag, const blockinfo *source_bi, int edge_slot, uintptr target_hostpc, const blockinfo *target_bi, cpuop_func *chosen)
{
    if (!jit_trace_stable_direct_env() || jit_trace_stable_direct_count >= jit_trace_stable_direct_limit())
        return;
    const uae_u32 src_pc = (source_bi && source_bi->pc_p) ? jit_hostpc_to_macpc((uintptr)source_bi->pc_p) : 0xffffffffu;
    const uae_u32 tgt_pc = target_hostpc ? jit_hostpc_to_macpc(target_hostpc) : 0xffffffffu;
    const uae_u8 summary = source_bi ? source_bi->stable_edge_mask : 0;
    fprintf(stderr,
        "JITDIRECT %lu tag=%s src=%08x edge=%d tgt=%08x summary=%02x romonly=%d chosen=%p tgt_dir=%p tgt_val=%p\n",
        ++jit_trace_stable_direct_count,
        tag,
        (unsigned)src_pc,
        edge_slot,
        (unsigned)tgt_pc,
        (unsigned)summary,
        jit_stable_direct_rom_only_env() ? 1 : 0,
        (void*)chosen,
        target_bi ? (void*)target_bi->direct_handler : NULL,
        target_bi ? (void*)target_bi->handler_to_use : NULL);
}

static unsigned long jit_test_metadata_rmw_rebuilds = 0;
static unsigned long jit_test_metadata_rmw_edge_total = 0;
static unsigned long jit_test_metadata_rmw_summary_mask = 0;

static inline void jit_commit_edge_summary_for_rebuild(blockinfo *bi)
{
    if (!bi)
        return;
    bi->stable_edge_mask = 0;
    bi->stable_edge_pc[0] = 0;
    bi->stable_edge_pc[1] = 0;
    const int dominant = jit_dominant_edge_index(bi);
    if (dominant >= 0 && jit_dominant_edge_stable(bi) &&
        bi->edge_target_pc[dominant] != 0) {
        bi->stable_edge_mask = (uae_u8)(1u << dominant);
        bi->stable_edge_pc[dominant] = bi->edge_target_pc[dominant];
        jit_trace_stable_direct_event("SUMMARY", bi, dominant, (uintptr)get_real_address(bi->stable_edge_pc[dominant], 0, sz_word), NULL, NULL);
    }
    const char *metadata_rmw_count = getenv("B2_TEST_METADATA_RMW_COUNT");
    const unsigned long metadata_edges =
        (unsigned long)bi->edge_exec_count[0] + bi->edge_exec_count[1];
    if (metadata_rmw_count && *metadata_rmw_count && metadata_edges != 0) {
        jit_test_metadata_rmw_rebuilds++;
        jit_test_metadata_rmw_edge_total += metadata_edges;
        jit_test_metadata_rmw_summary_mask |= bi->stable_edge_mask;
    }
}

static inline bool jit_source_edge_prefers_direct(const blockinfo *source_bi, int edge_slot, uintptr hostpc)
{
    if (!source_bi || edge_slot < 0 || edge_slot > 1)
        return false;
    if (!jit_enable_stable_direct_edges_env())
        return false;
    const uae_u8 mask = (uae_u8)(1u << edge_slot);
    if ((source_bi->stable_edge_mask & mask) == 0)
        return false;
    const uae_u32 target_pc = jit_hostpc_to_macpc(hostpc);
    if (source_bi->stable_edge_pc[edge_slot] != target_pc)
        return false;
    if (jit_stable_direct_rom_only_env()) {
        const uae_u32 src_pc = jit_hostpc_to_macpc((uintptr)source_bi->pc_p);
        if (src_pc < ROMBaseMac || target_pc < ROMBaseMac) {
            jit_trace_stable_direct_event("SKIP_ROMONLY", source_bi, edge_slot, hostpc, NULL, NULL);
            return false;
        }
    }
    return true;
}

/* Edge profiling is opt-in. The first stable-edge generation has a bounded
   countdown and is rebuilt once with its summary; ordinary blocks must not
   retain metadata writes or C callbacks on every exit. */
static inline bool jit_collect_edge_profile(const blockinfo *bi)
{
    return jit_trace_edges_env() ||
        (jit_enable_stable_direct_edges_env() && bi && bi->count >= 0);
}

static void jit_trace_edge_snapshot(const char *tag, const blockinfo *bi)
{
    if (!jit_trace_edges_env() || jit_trace_edges_count >= jit_trace_edges_limit() || !bi || !bi->pc_p)
        return;
    const uae_u32 src_pc = jit_hostpc_to_macpc((uintptr)bi->pc_p);
    const uae_u32 e0pc = bi->edge_target_pc[0];
    const uae_u32 e1pc = bi->edge_target_pc[1];
    const uae_u32 e0cnt = bi->edge_exec_count[0];
    const uae_u32 e1cnt = bi->edge_exec_count[1];
    if (!e0pc && !e1pc && !e0cnt && !e1cnt)
        return;
    const uae_u32 total = e0cnt + e1cnt;
    const int dominant = jit_dominant_edge_index(bi);
    const int stable = jit_dominant_edge_stable(bi) ? 1 : 0;
    fprintf(stderr,
        "JITEDGE %lu tag=%s src=%08x opt=%u status=%u flags=%02x total=%u "
        "e0pc=%08x e0cnt=%u e1pc=%08x e1cnt=%u dom=%d stable=%d summary=%02x\n",
        ++jit_trace_edges_count,
        tag,
        (unsigned)src_pc,
        (unsigned)bi->optlevel,
        (unsigned)bi->status,
        (unsigned)bi->needed_flags,
        (unsigned)total,
        (unsigned)e0pc,
        (unsigned)e0cnt,
        (unsigned)e1pc,
        (unsigned)e1cnt,
        dominant,
        stable,
        (unsigned)bi->stable_edge_mask);
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

static inline bool jit_prefer_validated_successor_handler(void)
{
    static int prefer_validated = -1;
    if (prefer_validated < 0) {
        const char *env = getenv("B2_JIT_PREFER_DIRECT_SUCCESSOR_HANDLER");
        /* Contract-first default on ARM64: validated successor entry unless
           explicitly overridden for direct-handler experiments. */
        prefer_validated = (env && *env && strcmp(env, "0") != 0) ? 0 : 1;
    }
    return prefer_validated != 0;
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
    0,		// Translate immediately: optlev-0 interpreter warm-up (exec_nostats)
    		// is itself an interpreter fallback and froze the slot-ROM bfextu
    		// blit self-loop at 0402e8d0. 0 = no whole-block interpretation.
#else
    0,		// Translate immediately (was 10). The optlev-0 interpreter warm-up
    		// (exec_nostats) is an interpreter fallback the 100%-JIT goal forbids,
    		// and its self-loop re-entry froze the 0402e8d0 slot-ROM bfextu blit.
#endif
    0,		// How often to use naive translation
    0, 0, 0, 0,
    -1, -1, -1, -1
};

static void jit_force_translate_check(void)
{
    const char *env = getenv("B2_JIT_FORCE_TRANSLATE");
    if (env && *env && strcmp(env, "0") != 0)
        optcount[0] = 0;  // Skip interpreter warm-up, compile immediately
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
/* Keep architectural CACR state separate from translator availability.
   Strict full-JIT keeps translation available while CACR is disabled, but
   cache-disabled 68k stores must then make translated RAM code coherent. */
static int guest_cache_enabled = 0;
/* Strict mode keeps cache_enabled true while CACR is disabled. Remember that
   the disable transition's ordinary hard-flush boundary has already run, so
   repeated writes of the same disabled CACR state do not flush every block. */
static bool strict_cache_disable_boundary_seen = false;
static bool jit_emitted_guest_memory_write = false;
static unsigned long long jit_coherent_write_count = 0;
static unsigned long long jit_coherent_invalidation_count = 0;
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
static unsigned long jit_test_direct_checksum_entries = 0;
static unsigned long jit_test_direct_exec_nostats_entries = 0;
static unsigned long jit_test_direct_execute_normal_entries = 0;
static unsigned long jit_test_execute_normal_cycles_entries = 0;
static unsigned long jit_test_execute_normal_cycles_before = 0;
static unsigned long jit_test_execute_normal_cycles_after = 0;
static unsigned long jit_test_handle_except_checks = 0;
static unsigned long jit_test_handle_except_taken = 0;
static unsigned long jit_test_handle_except_cycles = 0;
static unsigned long jit_test_handle_except_received_cycles = 0;
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
    if (!jit_diag_enabled())
        return;
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
    if (!jit_diag_enabled())
        return;
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

static bool jit_test_dispatch_summary_enabled(void)
{
    const char *enabled = getenv("B2_TEST_DISPATCH_SUMMARY");
    return enabled && *enabled && strcmp(enabled, "0") != 0;
}

void jit_test_dump_dispatch_summary(void)
{
#if defined(CPU_AARCH64)
    if (jit_test_dispatch_summary_enabled()) {
        fprintf(stderr, "JIT_TEST_DISPATCH direct_checksum=%lu check_checksum=%lu good=%lu bad=%lu exec_normal=%lu exec_nostats=%lu recompile_block=%lu metadata_rebuild=%lu metadata_edges=%lu metadata_summary=%02lx direct_exec_nostats=%lu direct_execute_normal=%lu execute_normal_cycles=%lu cycles_before=%lu cycles_after=%lu handle_except_checks=%lu handle_except_taken=%lu handle_except_cycles=%lu handle_except_received_cycles=%lu\n",
            jit_test_direct_checksum_entries, jit_diag_check_checksum_calls,
            jit_diag_checksum_good, jit_diag_checksum_bad,
            jit_diag_execute_normal_calls, jit_diag_exec_nostats_calls,
            jit_diag_recompile_block_calls, jit_test_metadata_rmw_rebuilds,
            jit_test_metadata_rmw_edge_total, jit_test_metadata_rmw_summary_mask,
            jit_test_direct_exec_nostats_entries,
            jit_test_direct_execute_normal_entries,
            jit_test_execute_normal_cycles_entries,
            jit_test_execute_normal_cycles_before,
            jit_test_execute_normal_cycles_after,
            jit_test_handle_except_checks,
            jit_test_handle_except_taken,
            jit_test_handle_except_cycles,
            jit_test_handle_except_received_cycles);
    }
#endif
}
/* ---- end JIT dispatch diagnostic counters ---- */

extern bool UseJIT;

static void disable_jit_runtime(const char* reason)
{
	/* Strict mode is an execution-policy assertion: initialization failure must
	   abort rather than silently selecting the ordinary interpreter. */
	if (jit_strict_full_jit_env())
		jit_abort("strict full-JIT: %s", reason);

	jit_log("JIT disabled: %s", reason);
	/* Lazy initialization can fail after popallspace or a separate code cache
	   has been mapped. Tear the partial runtime down while cache_size still
	   describes the allocation; compiler_exit() is idempotent and clears every
	   generated dispatcher pointer before ordinary execution resumes. */
	compiler_exit();
	currprefs.cachesize = 0;
	changed_prefs.cachesize = 0;
	cache_size = 0;
	cache_enabled = 0;
	UseJIT = false;
	/* TimerInit runs before compiler initialization and may already have handed
	   precise-timer ownership to the requested JIT. Restore the asynchronous
	   source when ordinary mode falls back during initialization. */
	TimerRestoreAsyncOwnership();
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
static inline void flush_icache_architectural(int); static inline void reset_lists(void);

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

static inline void set_dhtu_policy(blockinfo* bi, cpuop_func* dh, bool honor_prefer_direct)
{
    jit_log2("bi is %p", bi);
    dependency* x = bi->deplist;
    jit_log2("bi->deplist=%p", bi->deplist);
    while (x) {
        jit_log2("x is %p", x);
        jit_log2("x->next is %p", x->next);
        jit_log2("x->prev_p is %p", x->prev_p);

        if (x->jmp_off) {
            cpuop_func *dep_handler =
                (honor_prefer_direct && x->prefer_direct && bi->direct_handler)
                    ? bi->direct_handler : dh;
            if (honor_prefer_direct && x->prefer_direct && bi->direct_handler)
                jit_trace_stable_direct_event("REPATCH_DIRECT", x->source, jit_dependency_edge_slot(x), (uintptr)bi->pc_p, bi, dep_handler);
            else if (x->prefer_direct)
                jit_trace_stable_direct_event("REPATCH_FALLBACK", x->source, jit_dependency_edge_slot(x), (uintptr)bi->pc_p, bi, dep_handler);
            adjust_jmpdep(x, dep_handler);
        }
        x = x->next;
    }
    bi->direct_handler_to_use = (cpuop_func*)dh;
}

static inline void set_dhtu(blockinfo* bi, cpuop_func* dh)
{
    set_dhtu_policy(bi, dh, true);
}

static inline void set_dhtu_validated(blockinfo* bi, cpuop_func* dh)
{
    /* Validation transitions must override stable direct-edge preference.
       Keep direct_handler intact for reactivation after a successful check,
       but route every existing inbound branch through dh in the meantime. */
    set_dhtu_policy(bi, dh, false);
}

void invalidate_block(blockinfo* bi)
{
    int i;

    jit_trace_edge_snapshot("INVALIDATE", bi);
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
        bi->dep[i].prefer_direct = 0;
        /* A checksum failure means this is a different code incarnation;
           discard the old incarnation's edge profile. */
        bi->edge_exec_count[i] = 0;
        bi->edge_target_pc[i] = 0;
        bi->stable_edge_pc[i] = 0;
    }
    bi->stable_edge_mask = 0;
    remove_deps(bi);
}

static inline void create_jmpdep(blockinfo* bi, int i, uae_u32* jmpaddr, uintptr target, bool prefer_direct)
{
    blockinfo* tbi = get_blockinfo_addr((void*)target);

    Dif(!tbi) {
        jit_abort("Could not create jmpdep!");
    }
    bi->dep[i].jmp_off = jmpaddr;
    bi->dep[i].source = bi;
    bi->dep[i].target = tbi;
    bi->dep[i].prefer_direct = prefer_direct ? 1 : 0;
    bi->dep[i].next = tbi->deplist;
    if (bi->dep[i].next)
        bi->dep[i].next->prev_p = &(bi->dep[i].next);
    bi->dep[i].prev_p = &(tbi->deplist);
    tbi->deplist = &(bi->dep[i]);
    bi->edge_target_pc[i] = jit_hostpc_to_macpc((uintptr)tbi->pc_p);
    jit_trace_edge_snapshot(i == 0 ? "EDGE0" : "EDGE1", bi);
}

static inline void block_need_recompile(blockinfo* bi)
{
    uae_u32 cl = cacheline(bi->pc_p);

    jit_trace_edge_snapshot("RECOMP", bi);
    /* Disable the old native target before repatching inbound dependencies.
       set_dhtu() honours prefer_direct edges by selecting direct_handler when
       non-null; doing this assignment afterwards leaves those edges pointing
       at stale code which has already been marked for recompilation. */
    bi->direct_handler = bi->direct_pen;
    set_dhtu(bi, bi->direct_pen);

    bi->handler_to_use = (cpuop_func*)popall_execute_normal;
    bi->handler = (cpuop_func*)popall_execute_normal;
    if (bi == cache_tags[cl + 1].bi)
        cache_tags[cl].handler = (cpuop_func*)popall_execute_normal;
    bi->status = BI_NEED_RECOMP;
}

static inline bool jit_strict_cache_disabled_coherence(void)
{
    return jit_strict_full_jit_env() && !guest_cache_enabled;
}

static inline bool jit_write_overlaps_checksum(const blockinfo* bi,
                                                const uae_u8* start,
                                                uae_u32 size)
{
    const uintptr write_start = (uintptr)start;
    const uintptr write_end = write_start + size;
    for (const checksum_info* csi = bi->csi; csi; csi = csi->next) {
        const uintptr code_start = (uintptr)csi->start_p;
        const uintptr code_end = code_start + csi->length;
        if (write_start < code_end && code_start < write_end)
            return true;
    }
    return false;
}

static void jit_invalidate_guest_code_linear(uae_u32 address, uae_u32 size,
                                             bool trace,
                                             bool count_coherent_write)
{
    if (size == 0 || address >= (uae_u32)RAMSize)
        return;
    if (size > (uae_u32)RAMSize - address)
        size = (uae_u32)RAMSize - address;

    uae_u8* start = RAMBaseHost + address;
    if (trace && jit_coherent_write_count <= 100)
        fprintf(stderr, "JIT_COHERENT_WRITE n=%llu addr=%08x size=%u host=%p host_code=%u\n",
            jit_coherent_write_count, address, size, (void*)start,
            count_coherent_write ? 0u : 1u);
    blockinfo* lists[2] = { active, dormant };
    for (int list = 0; list < 2; list++) {
        for (blockinfo* bi = lists[list]; bi; bi = bi->next) {
            if (bi->status != BI_INVALID &&
                bi->status != BI_NEED_RECOMP &&
                jit_write_overlaps_checksum(bi, start, size)) {
                jit_coherent_invalidation_count++;
                if (trace && jit_coherent_write_count <= 100)
                    fprintf(stderr, "JIT_COHERENT_INVALIDATE n=%llu block=%08x status=%d\n",
                        jit_coherent_invalidation_count,
                        jit_hostpc_to_macpc((uintptr)bi->pc_p), bi->status);
                block_need_recompile(bi);
            }
        }
    }
}

static void jit_invalidate_guest_code_range(uae_u32 address, uae_u32 size,
                                            bool count_coherent_write)
{
    if (size == 0)
        return;
    if (count_coherent_write)
        jit_coherent_write_count++;
    static const bool trace = [] {
        const char *env = getenv("B2_JIT_TRACE_COHERENCE");
        return env && *env && strcmp(env, "0") != 0;
    }();

    if (!currprefs.address_space_24) {
        jit_invalidate_guest_code_linear(address, size, trace,
            count_coherent_write);
        return;
    }

    /* A 24-bit bus wraps at 16 MiB. Split a store crossing that boundary so
       neither the tail nor the wrapped prefix can retain stale translated
       code. (Normal instruction stores are small, but keep arbitrary host-code
       write spans correct too.) */
    const uae_u32 bus_size = 0x01000000u;
    address &= bus_size - 1;
    while (size) {
        const uae_u32 chunk = size < bus_size - address
            ? size : bus_size - address;
        jit_invalidate_guest_code_linear(address, chunk, trace,
            count_coherent_write);
        size -= chunk;
        address = 0;
    }
}

/* Host-created Execute68k/Execute68kTrap instruction streams are not guest
   data-cache writes.  They must be visible on the very next fetch regardless
   of the emulated CACR state, and their stack slots are deliberately reused
   with different opcodes. */
void jit_invalidate_host_code_write(uae_u32 address, uae_u32 size)
{
    jit_invalidate_guest_code_range(address, size, false);
}

/* Cache-disabled 68k instruction fetch is coherent with data stores.  The
   legacy translator relied on MOVEC/CACR disabling JIT entirely, so generated
   direct stores had no write barrier.  Strict full-JIT deliberately keeps JIT
   available: invalidate every translated RAM block whose checksum span is
   touched before the writer returns to the dispatcher. */
void jit_notify_guest_memory_write(uae_u32 address, uae_u32 size)
{
    if (!jit_strict_cache_disabled_coherence())
        return;
    jit_invalidate_guest_code_range(address, size, true);
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
    if (live.flags_in_flags == VALID) {
        /* Branch emission consumes hardware NZCV directly. Some arithmetic
           helpers leave M68K carry represented as inverted ARM C; normalize
           it before any direct condition-code consumer. */
        if (flags_carry_inverted) {
            MRS_NZCV_x(REG_WORK1);
            EOR_xxCflag(REG_WORK1, REG_WORK1);
            MSR_NZCV_x(REG_WORK1);
            flags_carry_inverted = false;
        }
        return;
    }
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
    /* PC_P and scratch vregs can hold 64-bit host pointers. Scratch values
       use dedicated pointer-width spill backing even when they currently hold
       ordinary 32-bit guest data. */
    if (r == PC_P || r >= S1) {
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
        /* PC_P and scratch vregs can hold 64-bit host pointers. Never route
           either through compemu_raw_mov_l_mr's 32-bit STR. */
        if (r == PC_P || r >= S1) {
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
    } else if (r >= S1) {
        LOAD_U64(REG_WORK2, live.state[r].val);
        LOAD_U64(REG_WORK3, (uintptr)live.state[r].mem);
        STR_xXi(REG_WORK2, REG_WORK3, 0);
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
        /* Evicting a locked physical register destroys a value which an
           in-progress midfunc still owns.  Silently clearing the lock merely
           turns allocator corruption into guest corruption; fail at the first
           violated ownership boundary instead. */
        jit_abort("register %d in nreg %d is locked!", r, live.state[r].realreg);
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
                if (r == PC_P || r >= S1) {
                    /* PC_P and scratch vregs (e.g. the DBcc branch target built
                       by arm_ADD_l_ri_hostptr) can hold 64-bit host pointers —
                       use LOAD_U64. compemu_raw_mov_l_ri uses LOAD_U32 which
                       truncates; that only went unnoticed while the Mac address
                       space sat below 4 GB (Linux), not on macOS where
                       __PAGEZERO forces it above. LOAD_U64 of a 32-bit value
                       zero-extends exactly like LOAD_U32. */
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
extern int g_jvlock_reg; extern int g_jvlock_active;
/* Diagnostic-only forced-alias controls.  The original pressure cells target
   scratch destinations from architectural sources.  Family-level ownership
   audits also need the inverse direction (a scratch source crossing a write to
   an architectural destination), so both selectors accept every integer vreg.
   With no explicit target, retain the historical scratch-only behaviour. */
static int b2_force_scratch_alias_vreg(void)
{
    static int cached = -2;
    if (cached != -2)
        return cached;
    cached = -1;
    const char *env = getenv("B2_FORCE_SCRATCH_ALIAS_VREG");
    if (env && *env) {
        char *end = NULL;
        long v = strtol(env, &end, 0);
        if (end != env && v >= 0 && v < VREGS)
            cached = (int)v;
    }
    return cached;
}

static int b2_force_scratch_target_vreg(void)
{
    static int cached = -2;
    if (cached != -2)
        return cached;
    cached = -1;
    const char *env = getenv("B2_FORCE_SCRATCH_VREG");
    if (env && *env) {
        char *end = NULL;
        long v = strtol(env, &end, 0);
        if (end != env && v >= 0 && v < VREGS)
            cached = (int)v;
    }
    return cached;
}

static int writereg(int r)
{
    int answer = -1;

#if defined(CPU_AARCH64)
    const int force_target = b2_force_scratch_target_vreg();
    if ((force_target < 0 ? r >= S1 : r == force_target)) {
        int pin_vreg = b2_force_scratch_alias_vreg();
        if (pin_vreg >= 0 && isinreg(pin_vreg)) {
            int pin_host = live.state[pin_vreg].realreg;
            if (pin_host >= 0 && pin_host < N_REGS && !live.nat[pin_host].locked) {
                if (isinreg(r) && live.state[r].realreg != pin_host)
                    disassociate(r);
                if (!isinreg(r)) {
                    live.state[r].realreg = pin_host;
                    live.state[r].realind = live.nat[pin_host].nholds;
                    live.nat[pin_host].holds[live.nat[pin_host].nholds++] = r;
                }
                live.nat[pin_host].locked++;
                live.nat[pin_host].touched = touchcnt++;
                live.state[r].val = 0;
                set_status(r, DIRTY);
                fprintf(stderr, "REGPRESSURE_PIN_HIT scratch_vreg=%d pin_vreg=%d host=%d pc=%08x\n",
                    r, pin_vreg, pin_host, get_virtual_address(comp_pc_p));
                return pin_host;
            }
            fprintf(stderr, "REGPRESSURE_PIN_SKIP scratch_vreg=%d pin_vreg=%d host=%d locked=%d pc=%08x\n",
                r, pin_vreg, pin_host,
                (pin_host >= 0 && pin_host < N_REGS) ? live.nat[pin_host].locked : -1,
                get_virtual_address(comp_pc_p));
        }
    }
#endif

    /* If r is stale-associated to the host register currently pinned by
       jit_value_lock (a move's source value held across the destination-EA
       computation), the isinreg fast-path below would reuse that register and
       make_exclusive would EVICT the locked value -- so the store would write
       the destination ADDRESS instead of the value (the self-address
       corruption that spun 0403c19c). Drop the stale association first so a
       fresh, non-conflicting register is allocated. r is a write target, so
       its current contents are irrelevant. Scoped strictly to the active
       jit_value_lock pin so normal transient operation locks are unaffected. */
    if (g_jvlock_active && g_jvlock_reg >= 0 &&
        isinreg(r) && live.state[r].realreg == g_jvlock_reg) {
        disassociate(r);
    }

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

#if defined(CPU_AARCH64)
    /* The pressure hook normally intercepts write-only allocation in
       writereg(). Constant-backed scratch RMW values (NEGX) and explicitly
       selected architectural or private RMW destinations enter here instead.
       Private RMW targeting is required to prove that an already-fetched
       memory value cannot steal its still-live effective-address mapping.
       A correctly ordered operation has already pinned every crossing value,
       so the attempted host alias must be rejected. */
    const int force_target = b2_force_scratch_target_vreg();
    const bool historical_const_scratch = r >= S1 && isconst(r) &&
        (force_target < 0 || r == force_target);
    const bool explicit_target = force_target >= 0 && r == force_target;
    if (historical_const_scratch || explicit_target) {
        int pin_vreg = b2_force_scratch_alias_vreg();
        if (pin_vreg >= 0 && isinreg(pin_vreg)) {
            int pin_host = live.state[pin_vreg].realreg;
            if (pin_host >= 0 && pin_host < N_REGS && !live.nat[pin_host].locked) {
                if (isinreg(r) && live.state[r].realreg != pin_host)
                    disassociate(r);
                if (!isinreg(r)) {
                    live.state[r].realreg = pin_host;
                    live.state[r].realind = live.nat[pin_host].nholds;
                    live.nat[pin_host].holds[live.nat[pin_host].nholds++] = r;
                }
                live.nat[pin_host].locked++;
                live.nat[pin_host].touched = touchcnt++;
                live.state[r].val = 0;
                set_status(r, DIRTY);
                fprintf(stderr, "REGPRESSURE_PIN_HIT scratch_vreg=%d pin_vreg=%d host=%d pc=%08x\n",
                    r, pin_vreg, pin_host, get_virtual_address(comp_pc_p));
                return pin_host;
            }
            fprintf(stderr, "REGPRESSURE_PIN_SKIP scratch_vreg=%d pin_vreg=%d host=%d locked=%d pc=%08x\n",
                r, pin_vreg, pin_host,
                (pin_host >= 0 && pin_host < N_REGS) ? live.nat[pin_host].locked : -1,
                get_virtual_address(comp_pc_p));
        }
    }
#endif

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

static inline void f_mark_runtime_dirty(int r)
{
#if defined(CPU_AARCH64) && defined(USE_JIT_FPU)
    if (r >= 0 && r <= FP_RESULT) {
        compemu_raw_mov_l_rm(REG_WORK1, (uintptr)&regs.jit_fp_dirty_mask);
        LOAD_U32(REG_WORK2, 1u << r);
        ORR_www(REG_WORK1, REG_WORK1, REG_WORK2);
        compemu_raw_mov_l_mr((uintptr)&regs.jit_fp_dirty_mask, REG_WORK1);
    }
#else
    (void)r;
#endif
}

static inline int f_writereg(int r)
{
    int answer;

    if (f_isinreg(r)) {
        answer = live.fate[r].realreg;
    }  else {
        answer = f_alloc_reg(r, 1);
    }
    f_mark_runtime_dirty(r);
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
    f_mark_runtime_dirty(r);
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

static void jit_runtime_mvsr2_full(uae_u32 opcode)
{
    const uae_u32 real_opcode = opcode;
    const uae_u32 dstreg = real_opcode & 7;
    const bool ccr_only = (real_opcode & 0x0200) != 0;
    uaecptr dsta;

    if (!ccr_only && !regs.s) {
        Exception(8, 0);
        return;
    }
    MakeSR();
    const uae_u16 value = ccr_only ? (uae_u16)(regs.sr & 0x00ff) : regs.sr;

    switch (real_opcode & 0x0038) {
    case 0x0000: /* Dn */
        m68k_dreg(regs, dstreg) = (m68k_dreg(regs, dstreg) & 0xffff0000u) | value;
        m68k_incpc(2);
        return;
    case 0x0010: /* (An) */
        dsta = m68k_areg(regs, dstreg);
        m68k_incpc(2);
        break;
    case 0x0018: /* (An)+ */
        dsta = m68k_areg(regs, dstreg);
        m68k_areg(regs, dstreg) += 2;
        m68k_incpc(2);
        break;
    case 0x0020: /* -(An) */
        dsta = m68k_areg(regs, dstreg) - 2;
        m68k_areg(regs, dstreg) = dsta;
        m68k_incpc(2);
        break;
    case 0x0028: /* (d16,An) */
        dsta = m68k_areg(regs, dstreg) + (uae_s32)(uae_s16)get_iword(2);
        m68k_incpc(4);
        break;
    case 0x0030: /* (d8,An,Xn) */
        m68k_incpc(2);
        dsta = get_disp_ea_020(m68k_areg(regs, dstreg), next_iword());
        break;
    case 0x0038: /* absolute extension modes */
        if (dstreg == 0) {
            dsta = (uae_s32)(uae_s16)get_iword(2);
            m68k_incpc(4);
        } else if (dstreg == 1) {
            dsta = get_ilong(2);
            m68k_incpc(6);
        } else {
            op_illg(opcode);
            return;
        }
        break;
    default:
        op_illg(opcode);
        return;
    }
    regs.fault_pc = m68k_getpc();
    put_word(dsta, value);
}

static void jit_runtime_mv2sr_word_full(uae_u32 opcode)
{
    if (!regs.s) {
        Exception(8, 0);
        return;
    }

    uae_u32 real_opcode = opcode;
    uae_u32 srcreg = real_opcode & 7;
    uaecptr srca;
    uae_u16 src;

    switch (real_opcode & 0x0038) {
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

static void jit_runtime_rts(uae_u32 opcode)
{
    (void)opcode;
    m68k_do_rts();
}

static void jit_runtime_bsr(uae_u32 opcode)
{
    const uaecptr pc = m68k_getpc();
    const uae_u8 disp8 = opcode & 0xff;
    uaecptr oldpc;
    uae_s32 offset;

    if (disp8 == 0) {
        oldpc = pc + 4;
        offset = (uae_s32)(uae_s16)get_word(pc + 2) + 2;
    } else if (disp8 == 0xff) {
        oldpc = pc + 6;
        offset = (uae_s32)get_long(pc + 2) + 2;
    } else {
        oldpc = pc + 2;
        offset = (uae_s32)(uae_s8)disp8 + 2;
    }
    m68k_do_bsr(oldpc, offset);
}

static void jit_runtime_trap(uae_u32 opcode)
{
    const uae_u32 vector = 32 + (opcode & 15);
    m68k_incpc(2);
    regs.fault_pc = m68k_getpc();
    Exception(vector, 0);
}

static void jit_runtime_trapcc(uae_u32 opcode)
{
    const uaecptr oldpc = m68k_getpc();
    const unsigned condition = (opcode >> 8) & 15;
    switch (opcode & 7) {
    case 2: /* immediate word */
        (void)get_iword(2);
        m68k_incpc(4);
        break;
    case 3: /* immediate long */
        (void)get_ilong(2);
        m68k_incpc(6);
        break;
    case 4: /* no operand */
        m68k_incpc(2);
        break;
    default:
        op_illg(opcode);
        return;
    }
    if (cctrue(condition))
        Exception(7, oldpc);
}

static void jit_runtime_system_control(uae_u32 opcode)
{
    /* This is the single AArch64 semantic boundary for privileged integer
       control instructions.  The helper enters at the exact pc_hist[] opcode
       PC, checks privilege before fetching extension words, and advances only
       at the same commit points as the canonical 68040 core. */
    if (!regs.s) {
        Exception(8, 0);
        return;
    }

    if ((opcode & 0xfff8) == 0x4e60) { /* MOVE An,USP */
        regs.usp = m68k_areg(regs, opcode & 7);
        m68k_incpc(2);
        return;
    }
    if ((opcode & 0xfff8) == 0x4e68) { /* MOVE USP,An */
        m68k_areg(regs, opcode & 7) = regs.usp;
        m68k_incpc(2);
        return;
    }

    switch (opcode) {
    case 0x4e70: /* RESET */
        AtariReset();
        m68k_incpc(2);
        return;
    case 0x4e72: { /* STOP #<sr> */
        const uae_u16 new_sr = (uae_u16)get_iword(2);
        /* On the configured 68040, a supervisor STOP that clears S traps from
           the successor without committing SR or entering the stopped state. */
        if (!(new_sr & 0x2000)) {
            m68k_incpc(4);
            Exception(8, 0);
            return;
        }
        regs.sr = new_sr;
        MakeFromSR();
        m68k_setstopped(1);
        m68k_incpc(4);
        return;
    }
    case 0x4e73: /* RTE */
        ex_rte();
        return;
    case 0x4e7a: /* MOVEC control register to general register */
    case 0x4e7b: { /* MOVEC general register to control register */
        const uae_u16 ext = (uae_u16)get_iword(2);
        uae_u32 *regp = &regs.regs[(ext >> 12) & 15];
        const bool valid = opcode == 0x4e7a
            ? m68k_movec2(ext & 0x0fff, regp) != 0
            : m68k_move2c(ext & 0x0fff, regp) != 0;
        if (valid)
            m68k_incpc(4);
        return;
    }
    default:
        jit_abort("system-control semantic service received opcode %04x",
            (unsigned)opcode);
    }
}

static void jit_runtime_cache_control(uae_u32 opcode)
{
    if (!regs.s) {
        Exception(8, 0);
        return;
    }
    flush_internals();
    /* CINV and CPUSH use one architectural transition contract.  The
       canonical core invalidates translated code only for instruction-cache
       forms (bit 7); data-cache-only forms have no additional host state. */
    if (opcode & 0x80)
        flush_icache_architectural(0);
    m68k_incpc(2);
}

static void jit_runtime_illegal_advanced(uae_u32 opcode)
{
    /* BKPT, CALLM and RTM are represented in the generic legal table but the
       configured 68040 core canonically treats them as illegal after consuming
       the opcode word. Keep that architectural trap explicit, not a fallback. */
    m68k_incpc(2);
    op_illg(opcode);
}

static void jit_runtime_moves(uae_u32 opcode)
{
    /* MOVES is a privileged, extension-word-directed transfer through the
       source/destination function-code address spaces.  Own the complete
       transaction here: privilege precedes extension fetch, register sources
       are snapshotted before EA updates, reads precede post/predecrement
       writeback, and stores see the architecturally advanced fault PC. */
    if (!regs.s) {
        Exception(8, 0);
        return;
    }

    const uae_u16 extension = get_iword(2);
    const unsigned transfer_reg = (extension >> 12) & 15;
    const bool register_to_memory = (extension & 0x0800) != 0;
    const unsigned mode = (opcode >> 3) & 7;
    const unsigned areg = opcode & 7;
    const unsigned size_code = (opcode >> 6) & 3;
    const unsigned size = size_code == 0 ? 1 : size_code == 1 ? 2 : 4;
    const unsigned step = size == 1 ? areg_byteinc[areg] : size;
    const uae_u32 register_source = regs.regs[transfer_reg];
    uaecptr ea = 0;
    unsigned fixed_length = 0;
    bool pc_already_advanced = false;

    switch (mode) {
    case 2: /* (An) */
    case 3: /* (An)+ */
        ea = m68k_areg(regs, areg);
        fixed_length = 4;
        break;
    case 4: /* -(An) */
        ea = m68k_areg(regs, areg) - step;
        fixed_length = 4;
        break;
    case 5: /* (d16,An) */
        ea = m68k_areg(regs, areg) + (uae_s32)(uae_s16)get_iword(4);
        fixed_length = 6;
        break;
    case 6: /* (d8,An,Xn), including full-format memory-indirect forms */
        m68k_incpc(4);
        ea = get_disp_ea_020(m68k_areg(regs, areg), next_iword());
        pc_already_advanced = true;
        break;
    case 7:
        if (areg == 0) {
            ea = (uae_s32)(uae_s16)get_iword(4);
            fixed_length = 6;
        } else if (areg == 1) {
            ea = get_ilong(4);
            fixed_length = 8;
        } else {
            op_illg(opcode);
            return;
        }
        break;
    default:
        op_illg(opcode);
        return;
    }

    if (register_to_memory) {
        /* Both auto-update modes commit before the faultable write. */
        if (mode == 3)
            m68k_areg(regs, areg) += step;
        else if (mode == 4)
            m68k_areg(regs, areg) = ea;
        if (!pc_already_advanced)
            m68k_incpc(fixed_length);
        regs.fault_pc = m68k_getpc();
#ifdef FULLMMU
        if (size == 1)
            dfc_put_byte(ea, register_source);
        else if (size == 2)
            dfc_put_word(ea, register_source);
        else
            dfc_put_long(ea, register_source);
#else
        if (size == 1)
            put_byte(ea, register_source);
        else if (size == 2)
            put_word(ea, register_source);
        else
            put_long(ea, register_source);
#endif
        return;
    }

    uae_u32 memory_value;
#ifdef FULLMMU
    if (size == 1)
        memory_value = (uae_u8)sfc_get_byte(ea);
    else if (size == 2)
        memory_value = (uae_u16)sfc_get_word(ea);
    else
        memory_value = sfc_get_long(ea);
#else
    if (size == 1)
        memory_value = (uae_u8)phys_get_byte(ea);
    else if (size == 2)
        memory_value = (uae_u16)phys_get_word(ea);
    else
        memory_value = phys_get_long(ea);
#endif

    /* Canonical read ordering commits An update only after a successful read;
       an address-register destination then wins if it aliases the EA register. */
    if (mode == 3)
        m68k_areg(regs, areg) += step;
    else if (mode == 4)
        m68k_areg(regs, areg) = ea;

    if (extension & 0x8000) {
        if (size == 1)
            m68k_areg(regs, transfer_reg & 7) = (uae_s32)(uae_s8)memory_value;
        else if (size == 2)
            m68k_areg(regs, transfer_reg & 7) = (uae_s32)(uae_s16)memory_value;
        else
            m68k_areg(regs, transfer_reg & 7) = memory_value;
    } else if (size == 1) {
        m68k_dreg(regs, transfer_reg) =
            (m68k_dreg(regs, transfer_reg) & ~0xffu) | (memory_value & 0xffu);
    } else if (size == 2) {
        m68k_dreg(regs, transfer_reg) =
            (m68k_dreg(regs, transfer_reg) & ~0xffffu) | (memory_value & 0xffffu);
    } else {
        m68k_dreg(regs, transfer_reg) = memory_value;
    }
    if (!pc_already_advanced)
        m68k_incpc(fixed_length);
}

static void jit_runtime_bitfield(uae_u32 opcode)
{
    /* Decode the complete bitfield instruction at its exact pc_hist[] PC.
       In particular, fixed-format memory faults retain the opcode PC, indexed
       forms retain the PC advanced by get_disp_ea_020(), and successful
       operations alone commit the fixed-format successor. */
    const uae_u16 extension = (uae_u16)get_iword(2);
    const unsigned mode = (opcode >> 3) & 7;
    const unsigned reg = opcode & 7;
    uaecptr ea = 0;
    unsigned fixed_length = 0;
    bool is_dreg = false;
    bool pc_already_advanced = false;

    switch (mode) {
    case 0: /* Dn */
        ea = reg;
        is_dreg = true;
        fixed_length = 4;
        break;
    case 2: /* (An) */
        ea = m68k_areg(regs, reg);
        fixed_length = 4;
        break;
    case 5: /* (d16,An) */
        ea = m68k_areg(regs, reg) + (uae_s32)(uae_s16)get_iword(4);
        fixed_length = 6;
        break;
    case 6: /* (d8,An,Xn), including full-format extensions */
        m68k_incpc(4);
        ea = get_disp_ea_020(m68k_areg(regs, reg), next_iword());
        pc_already_advanced = true;
        break;
    case 7:
        switch (reg) {
        case 0: /* (xxx).W */
            ea = (uae_s32)(uae_s16)get_iword(4);
            fixed_length = 6;
            break;
        case 1: /* (xxx).L */
            ea = get_ilong(4);
            fixed_length = 8;
            break;
        case 2: /* (d16,PC) */
            ea = m68k_getpc() + 4 + (uae_s32)(uae_s16)get_iword(4);
            fixed_length = 6;
            break;
        case 3: /* (d8,PC,Xn), including full-format extensions */
            m68k_incpc(4);
            ea = get_disp_ea_020(m68k_getpc(), next_iword());
            pc_already_advanced = true;
            break;
        default:
            jit_abort("bitfield semantic service received invalid EA opcode %04x",
                (unsigned)opcode);
        }
        break;
    default:
        jit_abort("bitfield semantic service received invalid EA opcode %04x",
            (unsigned)opcode);
    }

    regs.jit_exception = extension;
    regs.scratchregs[0] = ea;
    regs.scratchregs[1] = is_dreg ? 1 : 0;
    switch ((opcode >> 8) & 7) {
    case 0: jit_op_bftst(); break;
    case 1: jit_op_bfextu(); break;
    case 2: jit_op_bfchg(); break;
    case 3: jit_op_bfexts(); break;
    case 4: jit_op_bfclr(); break;
    case 5: jit_op_bfffo(); break;
    case 6: jit_op_bfset(); break;
    case 7: jit_op_bfins(); break;
    }
    if (!pc_already_advanced)
        m68k_incpc(fixed_length);
}

static void jit_runtime_cas(uae_u32 opcode)
{
    const uae_u16 extension = get_iword(2);
    const unsigned update_reg = (extension >> 6) & 7;
    const unsigned compare_reg = extension & 7;
    const unsigned mode = (opcode >> 3) & 7;
    const unsigned areg = opcode & 7;
    const unsigned size_code = (opcode >> 9) & 3;
    const unsigned size = size_code == 1 ? 1 : size_code == 2 ? 2 : 4;
    uaecptr ea = 0;
    unsigned fixed_length = 0;
    bool pc_already_advanced = false;

    switch (mode) {
    case 2: /* (An) */
    case 3: /* (An)+ */
        ea = m68k_areg(regs, areg);
        fixed_length = 4;
        break;
    case 4: /* -(An) */
        ea = m68k_areg(regs, areg) - (size == 1 ? areg_byteinc[areg] : size);
        fixed_length = 4;
        break;
    case 5: /* (d16,An) */
        ea = m68k_areg(regs, areg) + (uae_s32)(uae_s16)get_iword(4);
        fixed_length = 6;
        break;
    case 6: /* (d8,An,Xn), including full-format extensions */
        m68k_incpc(4);
        ea = get_disp_ea_020(m68k_areg(regs, areg), next_iword());
        pc_already_advanced = true;
        break;
    case 7:
        if (areg == 0) {
            ea = (uae_s32)(uae_s16)get_iword(4);
            fixed_length = 6;
        } else if (areg == 1) {
            ea = get_ilong(4);
            fixed_length = 8;
        } else {
            op_illg(opcode);
            return;
        }
        break;
    default:
        op_illg(opcode);
        return;
    }

    uae_u32 memory_value;
#ifdef FULLMMU
    if (size == 1)
        memory_value = (uae_u8)get_byte(ea);
    else if (size == 2)
        memory_value = (uae_u16)get_word(ea);
    else
        memory_value = get_long(ea);
#else
    if (size == 1)
        memory_value = (uae_u8)phys_get_byte(ea);
    else if (size == 2)
        memory_value = (uae_u16)phys_get_word(ea);
    else
        memory_value = phys_get_long(ea);
#endif
    if (mode == 3)
        m68k_areg(regs, areg) += size == 1 ? areg_byteinc[areg] : size;
    else if (mode == 4)
        m68k_areg(regs, areg) = ea;

    if (size == 1)
        optflag_cmpb((uae_s8)m68k_dreg(regs, compare_reg), (uae_s8)memory_value);
    else if (size == 2)
        optflag_cmpw((uae_s16)m68k_dreg(regs, compare_reg), (uae_s16)memory_value);
    else
        optflag_cmpl((uae_s32)m68k_dreg(regs, compare_reg), (uae_s32)memory_value);
    if (!pc_already_advanced)
        m68k_incpc(fixed_length);

    if (GET_ZFLG()) {
        regs.fault_pc = m68k_getpc();
        const uae_u32 update_value = m68k_dreg(regs, update_reg);
        if (size == 1)
            put_byte(ea, update_value);
        else if (size == 2)
            put_word(ea, update_value);
        else
            put_long(ea, update_value);
    } else if (size == 1) {
        m68k_dreg(regs, compare_reg) =
            (m68k_dreg(regs, compare_reg) & ~0xffu) | (memory_value & 0xffu);
    } else if (size == 2) {
        m68k_dreg(regs, compare_reg) =
            (m68k_dreg(regs, compare_reg) & ~0xffffu) | (memory_value & 0xffffu);
    } else {
        m68k_dreg(regs, compare_reg) = memory_value;
    }
}

static void jit_runtime_cas2(uae_u32 opcode)
{
    /* CAS2 owns both extension words, snapshots both address registers and
       memory operands before changing flags or compare registers, and commits
       both update writes only when both comparisons succeed. Keep the exact
       canonical ordering: compare #1 then #2, write #1 then #2 on success,
       update compare #2 then #1 on failure (the last assignment matters when
       the compare-register fields alias). PC remains on the opcode throughout
       faultable memory access and advances only after the transaction. */
    const uae_u32 extension = get_ilong(2);
    const unsigned address_reg1 = (extension >> 28) & 15;
    const unsigned update_reg1 = (extension >> 22) & 7;
    const unsigned compare_reg1 = (extension >> 16) & 7;
    const unsigned address_reg2 = (extension >> 12) & 15;
    const unsigned update_reg2 = (extension >> 6) & 7;
    const unsigned compare_reg2 = extension & 7;
    const uaecptr address1 = regs.regs[address_reg1];
    const uaecptr address2 = regs.regs[address_reg2];
    const bool is_long = (opcode & 0x0200) != 0;

    if (is_long) {
        const uae_u32 value1 = get_long(address1);
        const uae_u32 value2 = get_long(address2);
        optflag_cmpl((uae_s32)m68k_dreg(regs, compare_reg1), (uae_s32)value1);
        if (GET_ZFLG()) {
            optflag_cmpl((uae_s32)m68k_dreg(regs, compare_reg2), (uae_s32)value2);
            if (GET_ZFLG()) {
                put_long(address1, m68k_dreg(regs, update_reg1));
                put_long(address2, m68k_dreg(regs, update_reg2));
            }
        }
        if (!GET_ZFLG()) {
            m68k_dreg(regs, compare_reg2) = value2;
            m68k_dreg(regs, compare_reg1) = value1;
        }
    } else {
        const uae_u16 value1 = get_word(address1);
        const uae_u16 value2 = get_word(address2);
        optflag_cmpw((uae_s16)m68k_dreg(regs, compare_reg1), (uae_s16)value1);
        if (GET_ZFLG()) {
            optflag_cmpw((uae_s16)m68k_dreg(regs, compare_reg2), (uae_s16)value2);
            if (GET_ZFLG()) {
                put_word(address1, m68k_dreg(regs, update_reg1));
                put_word(address2, m68k_dreg(regs, update_reg2));
            }
        }
        if (!GET_ZFLG()) {
            m68k_dreg(regs, compare_reg2) =
                (m68k_dreg(regs, compare_reg2) & ~0xffffu) | value2;
            m68k_dreg(regs, compare_reg1) =
                (m68k_dreg(regs, compare_reg1) & ~0xffffu) | value1;
        }
    }
    m68k_incpc(6);
}

static void op_trap_comp_ff(uae_u32 opcode)
{
    const uintptr op_pc = jit_compile_current_op_host_pc
        ? jit_compile_current_op_host_pc : (uintptr)(comp_pc_p + m68k_pc_offset);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_trap,
        op_pc, opcode, 0, false);
}

static void op_trapcc_comp_ff(uae_u32 opcode)
{
    const uintptr op_pc = jit_compile_current_op_host_pc
        ? jit_compile_current_op_host_pc : (uintptr)(comp_pc_p + m68k_pc_offset);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_trapcc,
        op_pc, opcode, 0, false);
}

static void op_system_control_comp_ff(uae_u32 opcode)
{
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_system_control,
        jit_compile_current_op_host_pc, opcode, 0, false);
}

static void op_cache_control_comp_ff(uae_u32 opcode)
{
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_cache_control,
        jit_compile_current_op_host_pc, opcode, 0, false);
}

static void op_illegal_advanced_comp_ff(uae_u32 opcode)
{
    const uintptr op_pc = jit_compile_current_op_host_pc
        ? jit_compile_current_op_host_pc : (uintptr)(comp_pc_p + m68k_pc_offset);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_illegal_advanced,
        op_pc, opcode, 0, false);
}

static void op_moves_comp_ff(uae_u32 opcode)
{
    /* MOVES can privilege-trap and owns faultable SFC/DFC memory semantics,
       so it is an explicit end-block semantic service, never a fallback. */
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_moves,
        jit_compile_current_op_host_pc, opcode, 0, false);
}

static void op_bitfield_comp_ff(uae_u32 opcode)
{
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_bitfield,
        jit_compile_current_op_host_pc, opcode, 0, false);
}

static void op_cas_comp_ff(uae_u32 opcode)
{
    /* CAS owns a read/compare/conditional-write transaction, lazy flags, EA
       updates and fault PC.  Translate it as one semantic helper boundary;
       never route it through cpufunctbl. */
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_cas,
        jit_compile_current_op_host_pc, opcode, 0, false);
}

static void op_cas2_comp_ff(uae_u32 opcode)
{
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_cas2,
        jit_compile_current_op_host_pc, opcode, 0, false);
}

static void op_rts_comp_ff(uae_u32 opcode)
{
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_rts,
        (uintptr)(comp_pc_p + m68k_pc_offset_thisinst), opcode, 0, false);
}

static void op_bsr_comp_ff(uae_u32 opcode)
{
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    uintptr op_pc = jit_compile_current_op_host_pc ? jit_compile_current_op_host_pc : (uintptr)(comp_pc_p + m68k_pc_offset_thisinst);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_bsr, op_pc, opcode, 0, false);
}

static void jit_runtime_aline_trap(uae_u32 opcode)
{
    Exception(0xA, 0);
}

static void jit_runtime_illegal_trap(uae_u32 opcode)
{
    op_illg(opcode);
}

static void jit_runtime_fpp(uae_u32 opcode)
{
    const uae_u16 extension = get_iword(2);
    m68k_incpc(4);
    fpuop_arithmetic(opcode, extension);
}

static void jit_runtime_fscc(uae_u32 opcode)
{
    const uae_u16 extension = get_iword(2);
    m68k_incpc(4);
    fpuop_scc(opcode, extension);
}

static void jit_runtime_fbcc(uae_u32 opcode)
{
    m68k_incpc(2);
    const uaecptr displacement_pc = m68k_getpc();
    if (opcode & 0x40) {
        const uae_s32 displacement = (uae_s32)get_ilong(0);
        m68k_incpc(4);
        fpuop_bcc(opcode, displacement_pc, displacement);
    } else {
        const uae_s16 displacement = (uae_s16)get_iword(0);
        m68k_incpc(2);
        fpuop_bcc(opcode, displacement_pc, displacement);
    }
}

static void jit_runtime_fdbcc(uae_u32 opcode)
{
    /* Canonical FDBcc fetch order: extension at +2, then PC points at the
       displacement while fpuop_dbcc consumes it and owns the dynamic join. */
    const uae_u16 extension = get_iword(2);
    m68k_incpc(4);
    fpuop_dbcc(opcode, extension);
}

static void jit_runtime_ftrapcc(uae_u32 opcode)
{
    /* oldpc is the extension-word address.  Fetch the optional operand even
       though its value is ignored: an operand bus fault must precede condition
       evaluation exactly as in the generated interpreter. */
    m68k_incpc(2);
    const uaecptr oldpc = m68k_getpc();
    const uae_u16 extension = get_iword(0);
    switch (opcode & 0xff) {
    case 0x7a:
        (void)get_iword(2);
        m68k_incpc(4);
        break;
    case 0x7b:
        (void)get_ilong(2);
        m68k_incpc(6);
        break;
    case 0x7c:
        m68k_incpc(2);
        break;
    default:
        op_illg(opcode);
        return;
    }
    fpuop_trapcc(opcode, oldpc, extension);
}

static void jit_runtime_fsave(uae_u32 opcode)
{
    if (!regs.s) {
        Exception(8, 0);
        return;
    }
    /* Match gencpu's architectural ordering: FSAVE advances past the opcode
       before fpuop_save resolves extension words and performs EA updates. */
    m68k_incpc(2);
    fpuop_save(opcode);
}

static void jit_runtime_frestore(uae_u32 opcode)
{
    if (!regs.s) {
        Exception(8, 0);
        return;
    }
    m68k_incpc(2);
    fpuop_restore(opcode);
}

static void jit_runtime_fpu_semantic(uae_u32 opcode)
{
    switch (table68k[opcode].mnemo) {
    case i_FPP:
        jit_runtime_fpp(opcode);
        break;
    case i_FDBcc:
        jit_runtime_fdbcc(opcode);
        break;
    case i_FScc:
        jit_runtime_fscc(opcode);
        break;
    case i_FTRAPcc:
        jit_runtime_ftrapcc(opcode);
        break;
    case i_FBcc:
        jit_runtime_fbcc(opcode);
        break;
    case i_FSAVE:
        jit_runtime_fsave(opcode);
        break;
    case i_FRESTORE:
        jit_runtime_frestore(opcode);
        break;
    default:
        op_illg(opcode);
        break;
    }
}

static void jit_runtime_emulop(uae_u32 opcode)
{
    /* Generated and reference execution share the privilege, PC-advance and
       EXEC_RETURN termination contract in newcpu.cpp. */
    m68k_dispatch_emulop(opcode);
}

static void init_comp(void);  /* forward declaration */

static void op_fpu_semantic_comp_ff(uae_u32 opcode)
{
    const uintptr op_pc = jit_compile_current_op_host_pc
        ? jit_compile_current_op_host_pc : (uintptr)(comp_pc_p + m68k_pc_offset);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_fpu_semantic,
        op_pc, opcode, 0, false);
}

static void op_fdbcc_comp_ff(uae_u32 opcode)
{
    const uintptr op_pc = jit_compile_current_op_host_pc
        ? jit_compile_current_op_host_pc : (uintptr)(comp_pc_p + m68k_pc_offset);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_fdbcc,
        op_pc, opcode, 0, false);
}

static void op_ftrapcc_comp_ff(uae_u32 opcode)
{
    const uintptr op_pc = jit_compile_current_op_host_pc
        ? jit_compile_current_op_host_pc : (uintptr)(comp_pc_p + m68k_pc_offset);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_ftrapcc,
        op_pc, opcode, 0, false);
}

static void op_fsave_comp_ff(uae_u32 opcode)
{
    const uintptr op_pc = jit_compile_current_op_host_pc
        ? jit_compile_current_op_host_pc : (uintptr)(comp_pc_p + m68k_pc_offset);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_fsave,
        op_pc, opcode, 0, false);
}

static void op_frestore_comp_ff(uae_u32 opcode)
{
    const uintptr op_pc = jit_compile_current_op_host_pc
        ? jit_compile_current_op_host_pc : (uintptr)(comp_pc_p + m68k_pc_offset);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_frestore,
        op_pc, opcode, 0, false);
}

static void op_emulop_comp_ff(uae_u32 opcode)
{
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    uintptr op_pc = jit_compile_current_op_host_pc
        ? jit_compile_current_op_host_pc
        : (uintptr)(comp_pc_p + m68k_pc_offset_thisinst);
    /* EMUL_OP is a host-service boundary, not an interpreter opcode fallback.
       Call its semantic service directly from translated code and end at the
       runtime PC/state established by that service. */
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_emulop,
        op_pc, opcode, 0, false);
}

static void op_aline_trap_comp_ff(uae_u32 opcode)
{
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    uintptr op_pc = jit_compile_current_op_host_pc ? jit_compile_current_op_host_pc : (uintptr)(comp_pc_p + m68k_pc_offset_thisinst);
    /* A-line opcodes are trap control-flow, not local fallbacks. The exception
       frame must point at the exact opcode from pc_hist[i], not at a linearized
       comp_pc_p+m68k_pc_offset value that can be stale after in-block branch
       paths. */
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_aline_trap,
        op_pc, opcode, 0, false);
}

static void op_illegal_trap_comp_ff(uae_u32 opcode)
{
    uae_u32 m68k_pc_offset_thisinst = m68k_pc_offset;
    /* Invalid opcode slots are architectural traps, not permission to drop
       into the interpreter. This also covers ROM declaration-table probes that
       can land on invalid words while preserving a fully coherent PC triple. */
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_illegal_trap,
        (uintptr)(comp_pc_p + m68k_pc_offset_thisinst), opcode, 0, false);
}

static inline void jit_emit_runtime_helper_barrier(uintptr helper, uintptr pc, uae_u32 arg1, uae_u32 arg2, bool has_arg2)
{
    if (!pc)
        jit_abort("runtime semantic helper: missing exact opcode PC");
    /* B2 helper barrier contract: flush() materializes lazy registers/flags,
       then rebuild the full PC triple before entering C helper code. Some of
       these helpers use m68k_getpc(), MakeSR()/MakeFromSR(), or exception
       paths that must not observe a stale regs.pc/regs.pc_oldp pair. */
    flush(1);
    compemu_raw_mov_l_ri(REG_PAR1, arg1);
    if (has_arg2)
        compemu_raw_mov_l_ri(REG_PAR2, arg2);
    compemu_raw_set_pc_full_i(jit_hostpc_to_macpc(pc), pc);
    /* The C helper follows the platform ABI and may clobber caller-saved
       host registers. After flush(1), guest state is sound in memory; reset
       allocator host-register associations before the call so forced endblock
       code cannot later use stale clobbered host values. */
    prepare_for_call_2();
    compemu_raw_call(helper);
    live.state[PC_P].realreg = -1;
    live.state[PC_P].val = 0;
    set_status(PC_P, INMEM);
    jit_force_runtime_pc_endblock = true;
}

void jit_emit_ordered_semantic_helper_call(uintptr helper, uae_u32 instruction_bytes)
{
    /* Generated whole-instruction services need two distinct architectural
       PC states: the exact opcode for source accesses, and the linear successor
       at the interpreter-defined commit point.  Do not infer the opcode by
       rewinding whatever PC_P happened to contain after flush(1): traced blocks
       can be non-linear and compiler cursor re-anchoring deliberately emits no
       runtime write.  The compile loop records the exact pc_hist[] entry for the
       current opcode; pass its length-derived successor explicitly through x0. */
    if (!jit_compile_current_op_host_pc || instruction_bytes < 2 || (instruction_bytes & 1))
        jit_abort("ordered semantic helper: invalid compile PC/length host=%p bytes=%u",
            (void *)jit_compile_current_op_host_pc, (unsigned)instruction_bytes);

    const uintptr op_host_pc = jit_compile_current_op_host_pc;
    const uae_u32 op_m68k_pc = jit_compile_current_op_m68k_pc;
    const uae_u32 next_m68k_pc = op_m68k_pc + instruction_bytes;

    flush(1);
    compemu_raw_set_pc_full_i(op_m68k_pc, op_host_pc);
    compemu_raw_mov_l_ri(REG_PAR1, next_m68k_pc);
    prepare_for_call_2();
    compemu_raw_call(helper);

    /* The helper owns the final PC triple (successor, trap, or fault).  Forget
       the compile-time PC_P fact and terminate through the runtime value. */
    live.state[PC_P].realreg = -1;
    live.state[PC_P].val = 0;
    set_status(PC_P, INMEM);
    jit_force_runtime_pc_endblock = true;
}

static void op_fullsr_orsr_w_comp_ff(uae_u32 opcode)
{
    (void)opcode;
    const uintptr op_pc = jit_compile_current_op_host_pc;
    m68k_pc_offset += 2;
    uae_u32 src = (uae_u32)(uae_u16)comp_get_iword((m68k_pc_offset += 2) - 2);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_orsr_word,
        op_pc, src, 0, false);
}

static void op_fullsr_andsr_w_comp_ff(uae_u32 opcode)
{
    (void)opcode;
    const uintptr op_pc = jit_compile_current_op_host_pc;
    m68k_pc_offset += 2;
    uae_u32 src = (uae_u32)(uae_u16)comp_get_iword((m68k_pc_offset += 2) - 2);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_andsr_word,
        op_pc, src, 0, false);
}

static void op_fullsr_eorsr_w_comp_ff(uae_u32 opcode)
{
    (void)opcode;
    const uintptr op_pc = jit_compile_current_op_host_pc;
    m68k_pc_offset += 2;
    uae_u32 src = (uae_u32)(uae_u16)comp_get_iword((m68k_pc_offset += 2) - 2);
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_eorsr_word,
        op_pc, src, 0, false);
}

static void op_fullsr_mvsr2_comp_ff(uae_u32 opcode)
{
    /* MOVE SR/CCR can raise privilege or destination access exceptions.  The
       helper owns all EA side effects and the live successor, so terminate at
       its runtime PC rather than continuing from compile-time PC facts. */
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_mvsr2_full,
        jit_compile_current_op_host_pc, opcode, 0, false);
}

static void op_fullsr_mv2sr_w_comp_ff(uae_u32 opcode)
{
    /* Delegate exact EA semantics to the runtime helper. It advances PC on
       success and raises privilege/address exceptions with the correct live PC. */
    jit_emit_runtime_helper_barrier((uintptr)jit_runtime_mv2sr_word_full,
        jit_compile_current_op_host_pc, opcode, 0, false);
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
        arm_ADD_ptr_ri(PC_P, m68k_pc_offset);
        comp_pc_p += m68k_pc_offset;
        m68k_pc_offset = 0;
    }
}

/********************************************************************
 * Support functions exposed to newcpu                              *
 ********************************************************************/

/* ---------------------------------------------------------------------------
 * JIT I/O bank handlers (root-cause fix for the full-JIT boot hang)
 *
 * The 2026 uae_cpu tree dropped the UAE addrbank model: compiled loads/stores
 * use direct NATMEM (MEMBaseDiff + addr) for *every* address, so register-
 * indirect I/O polls (SCC/VIA/Cuda at 0x5xxxxxxx) read raw RAM pages instead of
 * the emulated device, and the boot poll loops spin forever. The interpreter
 * routes those same accesses through memory.h get_byte/put_byte, which carry the
 * real device behaviour (e.g. the 0x50000002 scanner-status auto-clear).
 *
 * These wrappers expose the interpreter's get/put to the JIT "special memory"
 * (mem-bank) dispatch used by jnf_MEM_READMEMBANK / jnf_MEM_WRITEMEMBANK. The
 * bank-callback contract (offsets used by readmem_special/writemem_special) is:
 *   [0]=lget [1]=wget [2]=bget [3]=lput [4]=wput [5]=bput [6]=xlate
 * Each slot is a host function pointer; reads take (addr) -> value, writes take
 * (addr, value), xlate takes (addr) -> host pointer. Routing compiled I/O here
 * makes JIT accesses bit-identical to the interpreter by construction.
 * --------------------------------------------------------------------------- */
static uae_u32 jit_bank_lget(uae_u32 addr) { return get_long(addr); }
static uae_u32 jit_bank_wget(uae_u32 addr) { return get_word(addr); }
static uae_u32 jit_bank_bget(uae_u32 addr) { return get_byte(addr); }
static void    jit_bank_lput(uae_u32 addr, uae_u32 v) { put_long(addr, v); }
static void    jit_bank_wput(uae_u32 addr, uae_u32 v) { put_word(addr, v); }
static void    jit_bank_bput(uae_u32 addr, uae_u32 v) { put_byte(addr, v); }
static uintptr jit_bank_xlate(uae_u32 addr) { return (uintptr)get_real_address(addr); }

static void *jit_io_bank[7] = {
    (void *)jit_bank_lget, (void *)jit_bank_wget, (void *)jit_bank_bget,
    (void *)jit_bank_lput, (void *)jit_bank_wput, (void *)jit_bank_bput,
    (void *)jit_bank_xlate,
};

void compiler_init(void)
{
    static bool initialized = false;
    if (initialized)
        return;

    flush_icache = flush_icache_none;

    flush_icache = lazy_flush ? flush_icache_lazy : flush_icache_hard;

    for (int bank = 0; bank < 65536; ++bank) {
        baseaddr[bank] = (uae_u8 *)MEMBaseDiff;
        /* Point every bank at the interpreter-backed handler so the JIT
         * special-memory (distrust) path dispatches real device emulation
         * instead of dereferencing a NULL stub. Only the distrust path uses
         * this table; default JIT (distrust=0) still uses direct NATMEM, so
         * this leaves the default code path unchanged. */
        mem_banks[bank] = jit_io_bank;
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

    /* Detach every block record and dependency before releasing code they may
       still name. This also returns reserved hold_bi records and checksum spans
       to their allocators. A partial lazy initialization has no compiled cache,
       but may still own popallspace, so reset_lists() remains necessary. */
    if (compiled_code && popall_execute_normal)
        flush_icache_hard(3);
    else
        reset_lists();

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

    /* No pointer into released executable storage may survive ordinary
       initialization fallback or shutdown. In particular,
       ensure_aarch64_jit_runtime_ready() uses pushall_call_handler as its
       readiness predicate. */
    pushall_call_handler = NULL;
    popall_do_nothing = NULL;
    popall_exec_nostats = NULL;
    popall_execute_normal = NULL;
    popall_cache_miss = NULL;
    popall_recompile_block = NULL;
    popall_check_checksum = NULL;
#ifdef AMIBERRY
    popall_exec_nostats_setpc = NULL;
    popall_execute_normal_setpc = NULL;
    popall_check_checksum_setpc = NULL;
    popall_execute_exception = NULL;
#endif
    current_compile_p = NULL;
    max_compile_start = NULL;
    target = NULL;
    current_cache_size = 0;
    cache_enabled = 0;
    guest_cache_enabled = 0;
    strict_cache_disable_boundary_seen = false;
    memset(cache_tags, 0, sizeof(cache_tags));
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
    static_assert(sizeof(regs.scratchregs) / sizeof(regs.scratchregs[0]) >= SCRATCH_REGS,
        "regstruct helper scratch cardinality must cover every integer scratch vreg");
    static_assert(sizeof(regs.jit_scratch_vregs) / sizeof(regs.jit_scratch_vregs[0]) >= SCRATCH_REGS,
        "regstruct pointer-width spill backing must cover every integer scratch vreg");
    static_assert(sizeof(regs.jit_scratch_vregs[0]) == sizeof(uintptr),
        "integer scratch spill slots must preserve host pointers");

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
            live.state[i].mem = (uae_u32*)&regs.jit_scratch_vregs[i - S1];
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
#ifdef USE_JIT_FPU
            /* Use shadow double array instead of fpu.registers[] (mpfr_t) */
            live.fate[i].mem = (uae_u32*)&regs.jit_fpregs[i];
#else
            live.fate[i].mem = (uae_u32*)fpu_register_address(i);
#endif
            live.fate[i].needflush = NF_TOMEM;
            live.fate[i].status = INMEM;
        } else if (i == FP_RESULT) {
#ifdef USE_JIT_FPU
            live.fate[i].mem = (uae_u32*)&regs.jit_fp_result;
#else
            live.fate[i].mem = (uae_u32*)(&regs.fp_result);
#endif
            live.fate[i].needflush = NF_TOMEM;
            live.fate[i].status = INMEM;
        } else {
#ifdef USE_JIT_FPU
            live.fate[i].mem = (uae_u32*)&regs.jit_scratchfregs[i - 8 - 1];
#else
            live.fate[i].mem = (uae_u32*)(&regs.scratchfregs[i - 8]);
#endif
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
void flush(int save_regs)
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
        jit_abort("release_scratch(): %d is not a scratch reg.", i);
    if (!live.scratch_in_use[i - S1])
        jit_abort("release_scratch(): %d not in use.", i);

    forget_about(i);
    live.scratch_in_use[i - S1] = 0;
}

static void freescratch(void)
{
    int i;
    for (i = 0; i < N_REGS; i++) {
        bool reserved = false;
        for (const uae_s8 *au = always_used; *au >= 0; ++au) {
            if (*au == i) {
                reserved = true;
                break;
            }
        }
        if (live.nat[i].locked && !reserved)
            jit_abort("physical register %d still locked at opcode boundary", i);
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
    /* A conditional native exception reaches the common deferred-exception gate
       after its inline test.  Carry the exact pc_hist[] opcode PC independently
       of PC_P: compile cursor re-anchoring and block finalisation are not an
       architectural source for the format-2 instruction-address field.  The
       store is harmless on the non-trapping path and is consumed only by a
       request tagged JIT_EXCEPTION_OLDPC_VALID. */
    if (!jit_compile_current_op_host_pc)
        jit_abort("deferred exception: missing exact opcode PC");
    const uintptr oldpc_idx = (uintptr)(&regs.jit_exception_oldpc) - (uintptr)(&regs);
    LOAD_U32(REG_WORK3, jit_compile_current_op_m68k_pc);
    STR_wXi(REG_WORK3, R_REGSTRUCT, oldpc_idx);
    may_raise_exception = true;
}

void register_possible_exception_at_successor(void)
{
    /* Arithmetic traps use a format-2 frame with two distinct addresses: the
       ordinary stacked PC is the post-instruction successor, while the extra
       instruction-address field is the opcode PC published above.  At this
       point genamode has consumed every extension word, so the compile cursor
       is the canonical successor for register, memory and indexed EAs alike. */
    register_possible_exception();
    const uintptr next_host_pc = (uintptr)(comp_pc_p + m68k_pc_offset);
    const uae_u32 next_m68k_pc = jit_compile_current_op_m68k_pc +
        (uae_u32)(next_host_pc - jit_compile_current_op_host_pc);
    compemu_raw_set_pc_full_i(next_m68k_pc, next_host_pc);
}

/* Note: get_handler may fail in 64 Bit environments, if direct_handler_to_use is
 *       outside 32 bit
 */
static uintptr get_handler_for_edge(const blockinfo *source_bi, int edge_slot, uintptr addr)
{
    addr = jit_canonicalize_target_pc(addr);
    if (jit_force_execute_normal_successor_env() || jit_force_execute_normal_target_env(addr))
        return (uintptr)popall_execute_normal;
    blockinfo* bi = get_blockinfo_addr_new((void*)(uintptr)addr);
#if defined(CPU_AARCH64)
    const bool forced_validated = jit_force_nondirect_handler_env() ||
        jit_force_nondirect_target_env(addr);
    if (!forced_validated && bi->status == BI_ACTIVE &&
        jit_source_edge_prefers_direct(source_bi, edge_slot, addr)) {
        uintptr h = (uintptr)(bi->direct_handler ? bi->direct_handler : bi->direct_handler_to_use);
        if (h)
            jit_trace_stable_direct_event("PROMOTE_DIRECT", source_bi, edge_slot, addr, bi, (cpuop_func*)h);
        else
            jit_trace_stable_direct_event("PROMOTE_FALLBACK", source_bi, edge_slot, addr, bi, (cpuop_func*)popall_execute_normal);
        return h ? h : (uintptr)popall_execute_normal;
    }
    const bool use_validated = jit_prefer_validated_successor_handler() || forced_validated;
    uintptr h = (uintptr)(use_validated ? bi->handler_to_use : bi->direct_handler_to_use);
#else
    uintptr h = (uintptr)(jit_force_nondirect_handler_env() ? bi->handler_to_use : bi->direct_handler_to_use);
#endif
    return h ? h : (uintptr)popall_execute_normal;
}

static uintptr get_handler(uintptr addr)
{
    return get_handler_for_edge(NULL, -1, addr);
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
    if (jit_strict_cache_disabled_coherence())
        jit_emitted_guest_memory_write = true;
    if (jit_force_all_special_mem() || (special_mem & S_WRITE) || distrust_byte() || jit_n_addr_unsafe)
        writemem_special(address, source, SIZEOF_VOID_P * 5);
    else
        writemem_real(address, source, 1);
}

void writeword(int address, int source)
{
    if (jit_strict_cache_disabled_coherence())
        jit_emitted_guest_memory_write = true;
    if (jit_force_all_special_mem() || (special_mem & S_WRITE) || distrust_word() || jit_n_addr_unsafe)
        writemem_special(address, source, SIZEOF_VOID_P * 4);
    else
        writemem_real(address, source, 2);
}

void writelong(int address, int source)
{
    if (jit_strict_cache_disabled_coherence())
        jit_emitted_guest_memory_write = true;
    if (jit_force_all_special_mem() || (special_mem & S_WRITE) || distrust_long() || jit_n_addr_unsafe)
        writemem_special(address, source, SIZEOF_VOID_P * 3);
    else
        writemem_real(address, source, 4);
}

// Now the same for clobber variant
void writeword_clobber(int address, int source)
{
    if (jit_strict_cache_disabled_coherence())
        jit_emitted_guest_memory_write = true;
    if (jit_force_all_special_mem() || (special_mem & S_WRITE) || distrust_word() || jit_n_addr_unsafe)
        writemem_special(address, source, SIZEOF_VOID_P * 4);
    else
        writemem_real(address, source, 2);
    forget_about(source);
}

void writelong_clobber(int address, int source)
{
    if (jit_strict_cache_disabled_coherence())
        jit_emitted_guest_memory_write = true;
    if (jit_force_all_special_mem() || (special_mem & S_WRITE) || distrust_long() || jit_n_addr_unsafe)
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
    guest_cache_enabled = enabled != 0;
    if (jit_strict_full_jit_env() && compiled_code && !enabled) {
        /* CACR remains guest-visible in regs.cacr. Strict verification only
           decouples host translation availability from the guest cache-enable
           bit. Preserve exactly one ordinary invalidation boundary per enabled
           -> disabled transition before keeping the translator active. */
        if (cache_enabled && !strict_cache_disable_boundary_seen)
            flush_icache_hard(3);
        cache_enabled = 1;
        strict_cache_disable_boundary_seen = true;
        return;
    }
    strict_cache_disable_boundary_seen = false;
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

static bool jit_probe_code_allocation_failure(void)
{
	/* Deterministic lifecycle fault injection. "all" rejects every executable
	   mapping; a positive integer rejects only that allocation attempt. */
	static unsigned long attempt = 0;
	const char *env = getenv("B2_JIT_PROBE_CODE_ALLOC_FAIL");
	if (!env || !*env || env[0] == '0')
		return false;
	attempt++;
	if (!strcmp(env, "all"))
		return true;
	char *end = NULL;
	const unsigned long selected = strtoul(env, &end, 0);
	return end && *end == '\0' && selected != 0 && attempt == selected;
}

static uint8 *do_alloc_code(uint32 size, int depth)
{
	UNUSED(depth);
	if (jit_probe_code_allocation_failure()) {
		jit_log("JIT lifecycle probe: rejecting executable allocation of %u bytes", size);
		return NULL;
	}
#if defined(CPU_AARCH64) && (defined(__linux__) || defined(__APPLE__))
	/* AArch64 code pointers are 64-bit clean and helper calls use BLR.
	   Do not allocate JIT code through the low-4GB scanner: in direct-addressing
	   builds, low host addresses alias the emulated Mac address space
	   (host = MEMBaseDiff + mac), so ROM/NuBus probes can read or fault on the
	   JIT cache. Keep generated code in a normal high host mapping instead. */
#if defined(__APPLE__)
	/* macOS on Apple Silicon only allows RWX pages created with MAP_JIT;
	   writes are then gated per thread by pthread_jit_write_protect_np(). */
	void *code = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
	void *code = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
	return code == MAP_FAILED ? NULL : (uint8 *)code;
#else
	uint8* code = (uint8 *)vm_acquire_code(size, VM_MAP_DEFAULT | VM_MAP_32BIT);
	return code == VM_MAP_FAILED ? NULL : code;
#endif
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

static bool calc_checksum(blockinfo* bi, uae_u32* c1, uae_u32* c2)
{
    uae_u32 k1 = 0;
    uae_u32 k2 = 0;

    *c1 = *c2 = 0;
    checksum_info* csi = bi->csi;
    if (!csi)
        return false;
    while (csi) {
        uae_s32 len = csi->length;
        uintptr tmp = (uintptr)csi->start_p;
        uae_u32* pos;

        // Validate before adding alignment padding (including signed overflow).
        // Invalid spans must never masquerade as valid all-zero guest source.
        if (!tmp || len <= 0 || len > MAX_CHECKSUM_LEN - (uae_s32)(tmp & 3))
            return false;
        len += (tmp & 3);
        tmp &= ~((uintptr)3);
        pos = (uae_u32*)tmp;

        if (len >= 0 && len <= MAX_CHECKSUM_LEN) {
            while (len > 0) {
                const uae_u32 word = *pos;
                k1 += word;
                // Source order matters: sum/XOR accepts permuted instructions.
                // Mix each byte, not whole words (odd multiplication alone
                // cannot distinguish a transposition differing only in bit 31).
                // Preserve zero-source checksums and carry across span links.
                for (unsigned shift = 0; shift < 32; shift += 8)
                    k2 = (k2 ^ ((word >> shift) & 0xff)) * 16777619u;
                pos++;
                len -= 4;
            }
        }

        csi = csi->next;
    }

    *c1 = k1;
    *c2 = k2;
    return true;
}

int check_for_cache_miss(void)
{
    blockinfo* bi = get_blockinfo_addr(regs.pc_p);

    if (bi) {
        int cl = cacheline(regs.pc_p);
        if (bi != cache_tags[cl + 1].bi) {
            raise_in_cl_list(bi);
#if defined(CPU_AARCH64)
            if (jit_diag_enabled())
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
    if (jit_diag_enabled()) {
        jit_diag_recompile_block_calls++;
        jit_diag_dispatch_count++;
        jit_diag_maybe_print();
    }
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
    if (jit_diag_enabled()) {
        jit_diag_cache_miss_calls++;
        jit_diag_dispatch_count++;
        jit_diag_maybe_print();
    }
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
    uae_u32 c1 = 0;
    uae_u32 c2 = 0;
    int isgood;

    if (bi->status != BI_NEED_CHECK)
        return 1;  /* This block is in a checked state */

    /* A zero checksum is a valid checksum for zero-filled guest code.  The
       legacy c1||c2 test overloaded that value as "checksum absent", causing
       strict zero-source RAM blocks to invalidate forever instead of being
       validated and promoted.  Presence is represented structurally by the
       checksum span chain; ROM blocks deliberately have no chain. */
    isgood = bi->csi != NULL;
    if (isgood) {
        isgood = calc_checksum(bi, &c1, &c2) &&
            (c1 == bi->c1 && c2 == bi->c2);
    }

    if (isgood) {
        /* This block is still OK. So we reactivate. Of course, that
           means we have to move it into the needs-to-be-flushed list */
        bi->handler_to_use = bi->handler;
        set_dhtu(bi, (jit_force_nondirect_handler_env() || jit_force_nondirect_target_env((uintptr)bi->pc_p)) ? bi->handler : bi->direct_handler);
        bi->status = BI_CHECKING; /* Strict lazy flush already gates every target's inbound edges. */
        isgood = jit_strict_full_jit_env() || called_check_checksum(bi) != 0;
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
    if (jit_diag_enabled()) {
        jit_diag_check_checksum_calls++;
        jit_diag_dispatch_count++;
        jit_diag_maybe_print();
    }
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

bool jit_test_prepare_direct_checksum_entry(void)
{
#if defined(CPU_AARCH64)
    const char *enabled = getenv("B2_TEST_FORCE_DIRECT_CHECKSUM");
    if (!(enabled && *enabled && strcmp(enabled, "0") != 0))
        return true;
    blockinfo* bi = get_blockinfo_addr(regs.pc_p);
    if (!bi || !bi->csi || !bi->direct_pcc)
        return false;
    const uae_u32 cl = cacheline(bi->pc_p);
    if (bi != cache_tags[cl + 1].bi)
        return false;
    /* Unlike the ordinary shared-handler path, this test deliberately enters
       the per-block raw stub. Keep cache-tag and relink policy coherent, and
       only override a cache line that this block actually owns. */
    bi->handler_to_use = bi->direct_pcc;
    set_dhtu_validated(bi, bi->direct_pcc);
    cache_tags[cl].handler = bi->handler_to_use;
    bi->status = BI_NEED_CHECK;
#endif
    return true;
}

bool jit_test_prepare_direct_execute_normal_entry(void)
{
#if defined(CPU_AARCH64)
    const char *enabled = getenv("B2_TEST_FORCE_DIRECT_EXECUTE_NORMAL");
    if (!(enabled && *enabled && strcmp(enabled, "0") != 0))
        return true;
    blockinfo* bi = get_blockinfo_addr(regs.pc_p);
    if (!bi || !bi->direct_pen || bi->status != BI_ACTIVE)
        return false;
    const uae_u32 cl = cacheline(bi->pc_p);
    if (bi != cache_tags[cl + 1].bi)
        return false;
    /* Re-enter the canonical invalidation/recompile lifecycle before selecting
       this block's raw direct_pen test stub. The cache line is the only
       temporary override; block_need_recompile() owns handler/dependency state. */
    block_need_recompile(bi);
    cache_tags[cl].handler = bi->direct_pen;
#endif
    return true;
}

bool jit_test_prepare_execute_normal_cycles_entry(void)
{
#if defined(CPU_AARCH64)
    const char *seed = getenv("B2_TEST_EXECUTE_NORMAL_CYCLES_SEED");
    if (!(seed && *seed))
        return true;
    char *end = NULL;
    const unsigned long parsed = strtoul(seed, &end, 0);
    if (end == seed || *end || parsed == 0 || parsed > 0x7fffffffUL)
        return false;
    countdown = (uae_s32)parsed;
    jit_test_execute_normal_cycles_entries = 0;
    jit_test_execute_normal_cycles_before = 0;
    jit_test_execute_normal_cycles_after = 0;
#endif
    return true;
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
            /* The caller owns execution policy. Strict full-JIT will abort in
               disable_jit_runtime(); ordinary mode must release any partial
               state and fall back to the interpreter rather than terminating
               the emulator here. */
            jit_log("WARNING: Could not allocate popallspace!");
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
#ifdef USE_JIT_FPU
    /* Interpreter/C code owns the architectural MPFR state; every fresh JIT
       entry must import both FP registers and FPSR condition state. Direct
       native chains intentionally retain the existing shadows. */
    compemu_raw_call((uintptr)jit_fpu_sync_to_shadow);
#endif
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
    /* Match m68k_setpc(): pc_p = pc_oldp = get_real_address(newpc),
       pc = newpc. Centralize this instead of open-coding the triple
       at every dispatcher-entry stub. */
    if (jit_test_dispatch_summary_enabled()) {
        LOAD_U64(REG_WORK4, (uintptr)&jit_test_direct_execute_normal_entries);
        LDR_xXi(R18_INDEX, REG_WORK4, 0);
        ADD_xxi(R18_INDEX, R18_INDEX, 1);
        STR_xXi(R18_INDEX, REG_WORK4, 0);
    }
    compemu_raw_set_pc_from_reg(REG_WORK1);
#else
    STR_rRI(REG_WORK1, R_REGSTRUCT, idx);
#endif
    popall_execute_normal = get_target();
    /* No fast dispatch for now - just the slow path */
#ifdef USE_JIT_FPU
    compemu_raw_call((uintptr)jit_fpu_sync_from_shadow);
#endif
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)execute_normal);

    popall_check_checksum_setpc = get_target();
#if defined(CPU_AARCH64)
    if (jit_test_dispatch_summary_enabled()) {
        LOAD_U64(REG_WORK4, (uintptr)&jit_test_direct_checksum_entries);
        LDR_xXi(R18_INDEX, REG_WORK4, 0);
        ADD_xxi(R18_INDEX, R18_INDEX, 1);
        STR_xXi(R18_INDEX, REG_WORK4, 0);
    }
    compemu_raw_set_pc_from_reg(REG_WORK1);
#else
    STR_rRI(REG_WORK1, R_REGSTRUCT, idx);
#endif
    popall_check_checksum = get_target();
#ifdef USE_JIT_FPU
    compemu_raw_call((uintptr)jit_fpu_sync_from_shadow);
#endif
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)check_checksum);

    popall_exec_nostats_setpc = get_target();
#if defined(CPU_AARCH64)
    if (jit_test_dispatch_summary_enabled()) {
        LOAD_U64(REG_WORK4, (uintptr)&jit_test_direct_exec_nostats_entries);
        LDR_xXi(R18_INDEX, REG_WORK4, 0);
        ADD_xxi(R18_INDEX, R18_INDEX, 1);
        STR_xXi(R18_INDEX, REG_WORK4, 0);
    }
    compemu_raw_set_pc_from_reg(REG_WORK1);
#else
    STR_rRI(REG_WORK1, R_REGSTRUCT, idx);
#endif
    popall_exec_nostats = get_target();
#ifdef USE_JIT_FPU
    compemu_raw_call((uintptr)jit_fpu_sync_from_shadow);
#endif
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)exec_nostats);

    popall_recompile_block = get_target();
#ifdef USE_JIT_FPU
    compemu_raw_call((uintptr)jit_fpu_sync_from_shadow);
#endif
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)recompile_block);

    popall_do_nothing = get_target();
#ifdef USE_JIT_FPU
    compemu_raw_call((uintptr)jit_fpu_sync_from_shadow);
#endif
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)do_nothing);

    popall_cache_miss = get_target();
#ifdef USE_JIT_FPU
    compemu_raw_call((uintptr)jit_fpu_sync_from_shadow);
#endif
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)cache_miss);

    popall_execute_exception = get_target();
#ifdef USE_JIT_FPU
#if defined(CPU_AARCH64)
    /* execute_exception's retired-cycle argument arrives in W0. The FPU shadow
       sync is an AAPCS64 C call and may clobber X0, so preserve the complete
       argument register in a balanced 16-byte stack slot across that call. */
    STR_xXpre(REG_PAR1, RSP_INDEX, -16);
    compemu_raw_call((uintptr)jit_fpu_sync_from_shadow);
    LDR_xXpost(REG_PAR1, RSP_INDEX, 16);
#else
    compemu_raw_call((uintptr)jit_fpu_sync_from_shadow);
#endif
#endif
    raw_pop_preserved_regs();
    compemu_raw_jmp((uintptr)execute_exception);

#if defined(USE_DATA_BUFFER)
    reset_data_buffer();
#endif

    // no need to further write into popallspace
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

    /* Reserved block records are not linked into active/dormant yet.  A hard
       cache flush used to drop these pointers without returning the records
       to BlockInfoAllocator, permanently consuming up to MAX_HOLD_BI chunks
       on every flush.  Their per-block entry stubs live in the code cache that
       is being discarded, so release the metadata before clearing the slots. */
    for (i = 0; i < MAX_HOLD_BI; i++) {
        if (hold_bi[i]) {
            free_blockinfo(hold_bi[i]);
            hold_bi[i] = NULL;
        }
    }
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
        bi->dep[i].prefer_direct = 0;
        bi->edge_exec_count[i] = 0;
        bi->edge_target_pc[i] = 0;
        bi->stable_edge_pc[i] = 0;
    }
    bi->stable_edge_mask = 0;
    bi->status = BI_INVALID;
    bi->needed_flags = FLAG_ALL;
}

void compemu_reset(void)
{
    flush_icache = lazy_flush ? flush_icache_lazy : flush_icache_hard;
    strict_cache_disable_boundary_seen = false;
    set_cache_state(0);
}

// OPCODE is in big endian format
static inline void reset_compop(int opcode)
{
    compfunctbl[opcode] = NULL;
    nfcompfunctbl[opcode] = NULL;
}

#if defined(CPU_AARCH64)
static void jit_install_fast_interpreter_overrides(void);
#endif

static bool jit_same_compiler_shape(const struct instr& a, const struct instr& b)
{
    return a.mnemo == b.mnemo &&
        a.cc == b.cc && a.plev == b.plev && a.clev == b.clev &&
        a.size == b.size && a.cflow == b.cflow &&
        a.smode == b.smode && a.stype == b.stype && a.dmode == b.dmode &&
        a.suse == b.suse && a.duse == b.duse && a.sduse == b.sduse &&
        a.spos == b.spos && a.dpos == b.dpos &&
        (a.spos >= 0 || a.sreg == b.sreg) &&
        (a.dpos >= 0 || a.dreg == b.dreg) &&
        a.flagdead == b.flagdead && a.flaglive == b.flaglive;
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

#if defined(CPU_AARCH64)
    jit_install_fast_interpreter_overrides();
#endif

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
        /* The no-flags table is generated independently.  Its FPU marker must
           be read from the entry being installed, not from the same numeric
           slot in the normal table (the two table orders are not an ABI). */
        bool uses_fpu = (nftbl[i].specific & COMP_OPCODE_USES_FPU) != 0;
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
            /* Architectural control-flow classification must not depend on
               whether this opcode family has a generated compiler handler.
               Unsupported canonical handlers retain prop's fl_trap default;
               copying that into aliases (for example BVC/BVS) makes block
               formation and fallback finalisation treat real branches as
               straight-line instructions.  Keep generated refinements for
               compiled aliases, but use table68k semantics for fallbacks. */
            cflow = f ? prop[cft_map(table68k[opcode].handler)].cflow
                      : table68k[opcode].cflow;
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
        /* BTST only writes Z, but the generated metadata can understate that
           for no-flags selection.  A following Bcc must therefore see the
           freshly materialized BTST Z instead of incoming/stale NZCV. */
        if (table68k[opcode].mnemo == i_BTST)
            prop[cft_map(opcode)].set_flags |= FLAG_Z;
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
    nfcompfunctbl[cft_map(0x007c)] = op_fullsr_orsr_w_comp_ff;
    nfcompfunctbl[cft_map(0x027c)] = op_fullsr_andsr_w_comp_ff;
    nfcompfunctbl[cft_map(0x0a7c)] = op_fullsr_eorsr_w_comp_ff;
    /* MOVE SR/CCR to every legal destination EA.  gencomp intentionally has
       no MVSR2 handlers, but hot ROM interrupt-mask code uses these forms. */
    for (opcode = 0; opcode < 65536; opcode++) {
        if (table68k[opcode].mnemo == i_MVSR2) {
            compfunctbl[cft_map(opcode)] = op_fullsr_mvsr2_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_fullsr_mvsr2_comp_ff;
        }
    }
    /* MV2SR.W (all EA modes): use runtime helper for correct stack swap */
    for (opcode = 0x46c0; opcode <= 0x46ff; opcode++) {
        if (compfunctbl[cft_map(opcode)]) {
            compfunctbl[cft_map(opcode)] = op_fullsr_mv2sr_w_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_fullsr_mv2sr_w_comp_ff;
        }
    }
    /* Resolve architectural traps in L2 instead of exact interpreter fallback.
       A-line has its own helper for clarity; all other invalid opcode slots use
       op_illg() through a native helper barrier so full-JIT mode never needs a
       local interpreter fallback for trap delivery. */
    for (opcode = 0xa000; opcode <= 0xafff; opcode++) {
        compfunctbl[cft_map(opcode)] = op_aline_trap_comp_ff;
        nfcompfunctbl[cft_map(opcode)] = op_aline_trap_comp_ff;
        prop[cft_map(opcode)].cflow = fl_trap;
    }
    for (opcode = 0; opcode < 65536; opcode++) {
        if ((opcode & 0xf000) != 0xa000 && table68k[opcode].mnemo == i_ILLG) {
            compfunctbl[cft_map(opcode)] = op_illegal_trap_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_illegal_trap_comp_ff;
            prop[cft_map(opcode)].cflow = fl_trap;
        }
    }
    /* Close legal control-flow and system-control table holes explicitly.
       These are semantic helper/trap boundaries, never cpufunctbl fallback. */
    for (opcode = 0; opcode < 65536; opcode++) {
        const unsigned mnemonic = table68k[opcode].mnemo;
        if (mnemonic == i_TRAP) {
            compfunctbl[cft_map(opcode)] = op_trap_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_trap_comp_ff;
            prop[cft_map(opcode)].cflow = fl_trap;
        } else if (mnemonic == i_TRAPcc) {
            compfunctbl[cft_map(opcode)] = op_trapcc_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_trapcc_comp_ff;
            prop[cft_map(opcode)].cflow = fl_trap;
        } else if (mnemonic == i_MVR2USP || mnemonic == i_MVUSP2R ||
                   mnemonic == i_RESET || mnemonic == i_STOP || mnemonic == i_RTE ||
                   mnemonic == i_MOVEC2 || mnemonic == i_MOVE2C) {
            compfunctbl[cft_map(opcode)] = op_system_control_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_system_control_comp_ff;
        } else if (mnemonic == i_CINVA || mnemonic == i_CINVL || mnemonic == i_CINVP ||
                   mnemonic == i_CPUSHA || mnemonic == i_CPUSHL || mnemonic == i_CPUSHP) {
            compfunctbl[cft_map(opcode)] = op_cache_control_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_cache_control_comp_ff;
        } else if (mnemonic == i_BKPT || mnemonic == i_CALLM || mnemonic == i_RTM) {
            compfunctbl[cft_map(opcode)] = op_illegal_advanced_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_illegal_advanced_comp_ff;
            prop[cft_map(opcode)].cflow = fl_trap;
        }
    }
    /* EMUL_OP (0x71xx): translated host-service boundary. The compiled
       handler calls m68k_emulop/m68k_emulop_return semantics directly and
       never emits an interpreter opcode-table call. */
    for (opcode = 0x7100; opcode <= 0x71ff; opcode++) {
        compfunctbl[cft_map(opcode)] = op_emulop_comp_ff;
        nfcompfunctbl[cft_map(opcode)] = op_emulop_comp_ff;
    }
    /* Floating control-flow operations own dynamic PC/exception state and are
       therefore explicit end-block semantic services.  This closes every
       legal FDBcc/FTRAPcc slot without using opcode-table fallback. */
    for (opcode = 0; opcode < 65536; opcode++) {
        if (table68k[opcode].mnemo == i_FDBcc) {
            compfunctbl[cft_map(opcode)] = op_fdbcc_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_fdbcc_comp_ff;
        } else if (table68k[opcode].mnemo == i_FTRAPcc) {
            compfunctbl[cft_map(opcode)] = op_ftrapcc_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_ftrapcc_comp_ff;
        }
    }
    /* FSAVE/FRESTORE are FPU state-frame memory operations, not host floating-
       point arithmetic. Give every legal EA encoding a translated semantic
       service boundary even when the arithmetic FPU compiler lacks a handler;
       this preserves privilege, EA update, frame format, and runtime PC without
       dispatching through cpufunctbl. */
    for (opcode = 0; opcode < 65536; opcode++) {
        if (table68k[opcode].mnemo == i_FSAVE) {
            compfunctbl[cft_map(opcode)] = op_fsave_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_fsave_comp_ff;
        } else if (table68k[opcode].mnemo == i_FRESTORE) {
            compfunctbl[cft_map(opcode)] = op_frestore_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_frestore_comp_ff;
        }
    }
    /* When host FPU translation is disabled, execute every legal FPU family
       through one native semantic-service boundary.  This is not an opcode-
       table fallback: generated code enters the canonical FPU semantic helpers
       directly with the same extension/displacement fetch order and runtime PC
       ownership as cpuemu.cpp.  Keeping the decision here also means the
       translator policy (compfpu) cannot silently turn legal FPU encodings into
       strict-mode fallback holes. */
    if (avoid_fpu) {
        for (opcode = 0; opcode < 65536; opcode++) {
            const unsigned mnemonic = table68k[opcode].mnemo;
            if (mnemonic == i_FPP || mnemonic == i_FDBcc ||
                mnemonic == i_FScc || mnemonic == i_FTRAPcc ||
                mnemonic == i_FBcc || mnemonic == i_FSAVE ||
                mnemonic == i_FRESTORE) {
                compfunctbl[cft_map(opcode)] = op_fpu_semantic_comp_ff;
                nfcompfunctbl[cft_map(opcode)] = op_fpu_semantic_comp_ff;
            }
        }
    }
    /* CAS.B/W/L and CAS2.W/L: translate each complete atomic transaction as
       one semantic helper boundary. The generic generator intentionally leaves
       these families unsupported, but strict execution must not hide either
       behind interpreter fallback. */
    for (opcode = 0; opcode < 65536; opcode++) {
        if (table68k[opcode].mnemo == i_CAS) {
            compfunctbl[cft_map(opcode)] = op_cas_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_cas_comp_ff;
        } else if (table68k[opcode].mnemo == i_CAS2) {
            compfunctbl[cft_map(opcode)] = op_cas2_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_cas2_comp_ff;
        } else if (table68k[opcode].mnemo == i_MOVES) {
            compfunctbl[cft_map(opcode)] = op_moves_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_moves_comp_ff;
        } else if (table68k[opcode].mnemo == i_BFTST ||
                   table68k[opcode].mnemo == i_BFEXTU ||
                   table68k[opcode].mnemo == i_BFCHG ||
                   table68k[opcode].mnemo == i_BFEXTS ||
                   table68k[opcode].mnemo == i_BFCLR ||
                   table68k[opcode].mnemo == i_BFFFO ||
                   table68k[opcode].mnemo == i_BFSET ||
                   table68k[opcode].mnemo == i_BFINS) {
            compfunctbl[cft_map(opcode)] = op_bitfield_comp_ff;
            nfcompfunctbl[cft_map(opcode)] = op_bitfield_comp_ff;
        }
    }
    /* RTS: dynamic stack return must not reuse a traced return target. */
    compfunctbl[cft_map(0x4e75)] = op_rts_comp_ff;
    nfcompfunctbl[cft_map(0x4e75)] = op_rts_comp_ff;

    /* BSR is a call boundary: materialize live state, push the return address,
       and end the native block at the runtime target instead of direct-chaining
       into a reusable callee block with caller-specific live-register state. */
    compfunctbl[cft_map(0x6100)] = op_bsr_comp_ff;
    nfcompfunctbl[cft_map(0x6100)] = op_bsr_comp_ff;
    compfunctbl[cft_map(0x6101)] = op_bsr_comp_ff;
    nfcompfunctbl[cft_map(0x6101)] = op_bsr_comp_ff;
    compfunctbl[cft_map(0x61ff)] = op_bsr_comp_ff;
    nfcompfunctbl[cft_map(0x61ff)] = op_bsr_comp_ff;

    /* Propagate generated handlers only through readcpu's explicit opcode
       handler chain.  Mnemonic equality is not a compatibility contract:
       Bcc conditions share i_Bcc, for example, but BRA's generated handler
       cannot implement unsupported BVC/BVS encodings. */
    for (opcode = 0; opcode < 65536; opcode++) {
        if (table68k[opcode].mnemo == i_ILLG || table68k[opcode].handler == -1)
            continue;
        if (!compfunctbl[cft_map(opcode)]) {
            int base = table68k[opcode].handler;
            int hops = 0;
            while (base >= 0 && base < 65536 &&
                   !compfunctbl[cft_map(base)] && hops < 10) {
                if (table68k[base].handler == -1 || table68k[base].handler == base)
                    break;
                base = table68k[base].handler;
                hops++;
            }
            compop_func *f = (base >= 0 && base < 65536)
                ? compfunctbl[cft_map(base)] : NULL;
            compop_func *nff = (base >= 0 && base < 65536)
                ? nfcompfunctbl[cft_map(base)] : NULL;
            /* Some readcpu aliases have no complete handler chain.  Reuse a
               generated handler only when every field that shapes emitted
               code matches; mnemonic alone conflates Bcc conditions and other
               encodings with different immediate/register extraction. */
            if (!f) {
                for (int probe = 0; probe < 65536 && !f; probe++) {
                    if (compfunctbl[cft_map(probe)] &&
                        jit_same_compiler_shape(table68k[opcode], table68k[probe])) {
                        f = compfunctbl[cft_map(probe)];
                        nff = nfcompfunctbl[cft_map(probe)];
                        base = probe;
                    }
                }
            }
            if (f) {
                compfunctbl[cft_map(opcode)] = f;
                nfcompfunctbl[cft_map(opcode)] = nff;
                prop[cft_map(opcode)].cflow = prop[cft_map(base)].cflow;
            } else {
                /* Compiler availability cannot redefine architectural control
                   flow.  Unsupported aliases retain readcpu's classification. */
                prop[cft_map(opcode)].cflow = table68k[opcode].cflow;
            }
            prop[cft_map(opcode)].set_flags = table68k[opcode].flagdead;
            prop[cft_map(opcode)].use_flags = table68k[opcode].flaglive;
            if (table68k[opcode].mnemo == i_BTST)
                prop[cft_map(opcode)].set_flags |= FLAG_Z;
        }
    }

    /* Optional post-registration coverage map. This observes the tables the
       compiler will actually dispatch through after canonical propagation and
       all AArch64 semantic-helper overrides, rather than guessing coverage from
       gencomp source cases or sparse base tables. */
    if (const char *coverage_path = getenv("B2_JIT_COVERAGE_FILE")) {
        if (*coverage_path) {
            FILE *coverage = fopen(coverage_path, "w");
            if (!coverage) {
                fprintf(stderr, "JIT_COVERAGE open failed path=%s errno=%d\n",
                    coverage_path, errno);
            } else {
                auto mnemonic_name = [](unsigned mnemo) -> const char * {
                    for (int n = 0; lookuptab[n].name[0]; n++)
                        if ((unsigned)lookuptab[n].mnemo == mnemo)
                            return lookuptab[n].name;
                    return "UNKNOWN";
                };
                auto helper_name = [](compop_func *handler) -> const char * {
                    if (handler == op_aline_trap_comp_ff) return "aline_trap";
                    if (handler == op_illegal_trap_comp_ff) return "illegal_trap";
                    if (handler == op_emulop_comp_ff) return "emulop_service";
                    if (handler == op_fullsr_orsr_w_comp_ff) return "orsr_helper";
                    if (handler == op_fullsr_andsr_w_comp_ff) return "andsr_helper";
                    if (handler == op_fullsr_eorsr_w_comp_ff) return "eorsr_helper";
                    if (handler == op_fullsr_mvsr2_comp_ff) return "mvsr2_helper";
                    if (handler == op_fullsr_mv2sr_w_comp_ff) return "mv2sr_helper";
                    if (handler == op_fdbcc_comp_ff) return "fdbcc_helper";
                    if (handler == op_ftrapcc_comp_ff) return "ftrapcc_helper";
                    if (handler == op_fsave_comp_ff) return "fsave_helper";
                    if (handler == op_frestore_comp_ff) return "frestore_helper";
                    if (handler == op_fpu_semantic_comp_ff) return "fpu_semantic_service";
                    if (handler == op_cas_comp_ff) return "cas_helper";
                    if (handler == op_cas2_comp_ff) return "cas2_helper";
                    if (handler == op_moves_comp_ff) return "moves_helper";
                    if (handler == op_bitfield_comp_ff) return "bitfield_helper";
                    if (handler == op_trap_comp_ff) return "trap_helper";
                    if (handler == op_trapcc_comp_ff) return "trapcc_helper";
                    if (handler == op_system_control_comp_ff) return "system_control_helper";
                    if (handler == op_cache_control_comp_ff) return "cache_control_helper";
                    if (handler == op_illegal_advanced_comp_ff) return "illegal_advanced_trap";
                    if (handler == op_rts_comp_ff) return "rts_helper";
                    if (handler == op_bsr_comp_ff) return "bsr_helper";
                    return NULL;
                };
                /* Some generated handlers deliberately emit a complete runtime
                   semantic service instead of native per-operation code.  They
                   are non-null and retire through generated entry stubs, but
                   classifying them as native would hide the helper boundary. */
                auto generated_helper_name = [](unsigned mnemo) -> const char * {
                    switch (mnemo) {
                    case i_MVPRM: return "generated_movep_reg_to_mem_helper";
                    case i_MVPMR: return "generated_movep_mem_to_reg_helper";
                    case i_CHK2: return "generated_chk2_helper";
                    case i_BFCHG: return "generated_bfchg_helper";
                    case i_BFCLR: return "generated_bfclr_helper";
                    case i_BFSET: return "generated_bfset_helper";
                    case i_BFTST: return "generated_bftst_helper";
                    case i_BFEXTS: return "generated_bfexts_helper";
                    case i_BFEXTU: return "generated_bfextu_helper";
                    case i_BFFFO: return "generated_bfffo_helper";
                    case i_BFINS: return "generated_bfins_helper";
                    case i_PACK: return "generated_pack_helper";
                    case i_UNPK: return "generated_unpk_helper";
                    default: return NULL;
                    }
                };
                fprintf(coverage,
                    "opcode,mapped,canonical,mnemonic,size,smode,dmode,clev,cflow,flagdead,flaglive,legal,normal,noflags,kind,implementation\n");
                unsigned legal_count = 0, generated_count = 0, helper_count = 0;
                unsigned trap_count = 0, fallback_count = 0, normal_only_count = 0;
                for (unsigned op = 0; op < 65536; op++) {
                    const unsigned mapped = cft_map(op);
                    compop_func *normal = compfunctbl[mapped];
                    compop_func *noflags_handler = nfcompfunctbl[mapped];
                    const bool legal = table68k[op].mnemo != i_ILLG &&
                        table68k[op].clev <= cpu_level;
                    const char *implementation = normal ? helper_name(normal) : NULL;
                    if (normal && !implementation)
                        implementation = generated_helper_name(table68k[op].mnemo);
                    const char *kind;
                    if (!normal) {
                        kind = legal ? "fallback_null" : "unavailable";
                        if (legal) fallback_count++;
                    } else if (normal == op_aline_trap_comp_ff ||
                               normal == op_illegal_trap_comp_ff ||
                               normal == op_trap_comp_ff ||
                               normal == op_illegal_advanced_comp_ff) {
                        kind = "architectural_trap";
                        if (legal) trap_count++;
                    } else if (implementation) {
                        kind = "semantic_helper";
                        if (legal) helper_count++;
                    } else {
                        kind = "native_generated";
                        if (legal) generated_count++;
                    }
                    if (legal) {
                        legal_count++;
                        if (normal && !noflags_handler)
                            normal_only_count++;
                    }
                    fprintf(coverage,
                        "%04x,%04x,%d,%s,%d,%d,%d,%d,%d,%u,%u,%d,%d,%d,%s,%s\n",
                        op, mapped, table68k[op].handler,
                        mnemonic_name(table68k[op].mnemo),
                        (int)table68k[op].size, (int)table68k[op].smode,
                        (int)table68k[op].dmode, table68k[op].clev,
                        table68k[op].cflow, (unsigned)table68k[op].flagdead,
                        (unsigned)table68k[op].flaglive, legal ? 1 : 0,
                        normal ? 1 : 0, noflags_handler ? 1 : 0, kind,
                        implementation ? implementation : (normal ? "gencomp" : "none"));
                }
                fclose(coverage);
                fprintf(stderr,
                    "JIT_COVERAGE path=%s legal=%u generated=%u helper=%u traps=%u fallback=%u normal_only=%u\n",
                    coverage_path, legal_count, generated_count, helper_count,
                    trap_count, fallback_count, normal_only_count);
            }
        }
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
    /* build_comp() deliberately makes the translator available after reset;
       the first later guest disable request still owns a flush boundary. */
    strict_cache_disable_boundary_seen = false;
    SPCFLAGS_CLEAR(SPCFLAG_JIT_EXEC_RETURN);
}

static void flush_icache_none(int v)
{
    /* Nothing to do.  */
}

void flush_icache_hard(int n)
{
#if defined(CPU_AARCH64)
    if (jit_diag_enabled()) {
        jit_diag_flush_icache_hard_calls++;
        fprintf(stderr, "JIT_DIAG flush_icache_hard called (n=%d), total=%lu\n", n, jit_diag_flush_icache_hard_calls);
        fflush(stderr);
        jit_diag_maybe_print();
    }
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
            set_dhtu_validated(bi, bi->direct_pcc);
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

#if defined(CPU_AARCH64)
static unsigned long b2_native_entry_count = 0;
static inline bool b2_native_entry_observer_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("B2_NATIVE_ASSERT_PC");
        enabled = (jit_strict_full_jit_env() || (env && *env)) ? 1 : 0;
    }
    return enabled != 0;
}

static void b2_test_native_entry(uae_u32 pc)
{
    const char *env = getenv("B2_NATIVE_ASSERT_PC");
    if (!env || !*env)
        return;
    char *end = NULL;
    unsigned long want = strtoul(env, &end, 0);
    if (end != env && (uae_u32)want == pc && b2_native_entry_count < 32) {
        b2_native_entry_count++;
        fprintf(stderr, "NATEXEC pc=%08x count=%lu d0=%08x d1=%08x d2=%08x d3=%08x d4=%08x d5=%08x d7=%08x a0=%08x a1=%08x a2=%08x a3=%08x a6=%08x a7=%08x sr=%04x\n",
            pc, b2_native_entry_count,
            regs.regs[0], regs.regs[1], regs.regs[2], regs.regs[3],
            regs.regs[4], regs.regs[5], regs.regs[7], regs.regs[8],
            regs.regs[9], regs.regs[10], regs.regs[11], regs.regs[14],
            regs.regs[15], regs.sr);
    }
}
#endif

void compile_block(cpu_history* pc_hist, int blocklen, int totcycles)
{
#if defined(CPU_AARCH64)
    /* A trace is a sequence of retired instruction locations and opcodes, not
       merely a list of pointers to mutable guest RAM. A store later in the
       trace may rewrite an earlier instruction before codegen starts. Mixing
       the new opcode with the old successor sequence creates native control
       flow that never existed architecturally. Discard the whole observation
       and let the dispatcher retrace the now-current stream. */
    for (int i = 0; i < blocklen; i++) {
        const uae_u16 current_opcode = (uae_u16)DO_GET_OPCODE(pc_hist[i].location);
        if (current_opcode != pc_hist[i].opcode ||
            memcmp(pc_hist[i].source, pc_hist[i].location,
                JIT_TRACE_SOURCE_BYTES) != 0)
            return;
    }

    if (jit_strict_full_jit_env() &&
        (!cache_enabled || !compiled_code || currprefs.cpu_model < 68020)) {
        const uae_u32 pc = (blocklen > 0 && pc_hist[0].location)
            ? jit_hostpc_to_macpc((uintptr)pc_hist[0].location) : 0xffffffffu;
        jit_abort("strict full-JIT: translator unavailable pc=%08x cache=%d code=%p cpu=%d",
            pc, cache_enabled, compiled_code, currprefs.cpu_model);
    }
    if (jit_diag_enabled())
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
            flush_icache_hard(3); /* code cache full: lazy flush does not rewind current_compile_p */

        alloc_blockinfos();

        bi = get_blockinfo_addr_new(pc_hist[0].location);
        bi2 = get_blockinfo(cl);
        const bool bi_was_invalid = (bi->status == BI_INVALID);

        optlev = bi->optlevel;
#if defined(CPU_AARCH64)
        /* When cpu_compatible is set, force blocks to interpreter mode */
        if (currprefs.cpu_compatible) {
            optlev = 0;
        }
#endif
        if (bi->status != BI_INVALID) {
#if defined(CPU_AARCH64)
            if (jit_diag_enabled())
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
        else if (jit_diag_enabled()) {
            jit_diag_compile_block_fresh++;
        }
#endif
        if (bi->count == -1) {
#if defined(CPU_AARCH64)
            if (currprefs.cpu_compatible) {
                optlev = 0;
            } else {
                const int max_optlev = jit_max_optlev();
                const uae_u32 blk_pc = (uae_u32)((uintptr)pc_hist[0].location - MEMBaseDiff);
                if (blk_pc >= ROMBaseMac) {
                    /* ROM: immediate L2 native codegen (immutable code).
                       When stable direct-edge profiling is explicitly enabled,
                       let the first native generation execute a bounded number
                       of times before one rebuild.  That rebuild is the only
                       point where edge_exec_count[]/edge_target_pc[] are
                       summarized into stable_edge_* for the next generation. */
                    optlev = max_optlev;
                    if (jit_enable_stable_direct_edges_env() && bi_was_invalid)
                        bi->count = (int)jit_stable_edge_profile_exec_env();
                    else
                        bi->count = -2;
                } else {
                    const char *metadata_rmw_count = getenv("B2_TEST_METADATA_RMW_COUNT");
                    const char *force_l2_ram = getenv("B2_TEST_FORCE_L2_RAM");
                    if (metadata_rmw_count && *metadata_rmw_count && bi_was_invalid) {
                        char *end = NULL;
                        const unsigned long parsed = strtoul(metadata_rmw_count, &end, 0);
                        if (end == metadata_rmw_count || *end || parsed < 1 || parsed > 64)
                            jit_abort("B2_TEST_METADATA_RMW_COUNT must be 1..64");
                        /* Test-only conjunction of two ordinary policies: emit
                           native L2 code while retaining a bounded positive
                           first-generation countdown. This exercises the real
                           dec_m/recompile and edge-counter inc_m paths; later
                           generations follow the ordinary no-countdown policy. */
                        optlev = max_optlev;
                        bi->count = (int)parsed;
                    } else if (jit_strict_full_jit_env() ||
                        (force_l2_ram && *force_l2_ram && strcmp(force_l2_ram, "0") != 0)) {
                        optlev = max_optlev;
                        bi->count = -2;
                    } else if (optlev == 0) {
                        /* RAM: use interpreter dispatch initially. Transient code
                           (memclear) runs once per address and never escalates.
                           Hot loops run many times and will escalate on the next
                           count expiry after 10 dispatches. */
                        bi->count = 9;
                    } else if (optlev < max_optlev) {
                        optlev = max_optlev;
                        bi->count = -2;
                    }
                }
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

        jit_commit_edge_summary_for_rebuild(bi);
        jit_trace_edge_snapshot("REBUILD", bi);
        remove_deps(bi); /* We are about to create new code */
        bi->optlevel = optlev;
        bi->pc_p = (uae_u8*)pc_hist[0].location;
        free_checksum_info_chain(bi->csi);
        bi->csi = NULL;
        bi->edge_exec_count[0] = bi->edge_exec_count[1] = 0;
        bi->edge_target_pc[0] = bi->edge_target_pc[1] = 0;
        bi->dep[0].prefer_direct = bi->dep[1].prefer_direct = 0;

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
#if defined(CPU_AARCH64)
        /* Account at the actual native entry so direct chains are included.
           Strict mode increments inline and enters C only for power-of-two
           summaries.  Explicit per-PC assertion remains a diagnostic observer
           and therefore retains the full save/call/restore path. */
        const uae_u32 native_entry_pc =
            (uae_u32)((uintptr)pc_hist[0].location - MEMBaseDiff);
        if (jit_strict_full_jit_env()) {
            compemu_raw_strict_entry_counter((uintptr)&jit_strict_native_entries,
                (uintptr)jit_strict_report_native_entry, native_entry_pc);
        }
        if (b2_native_entry_observer_enabled() && getenv("B2_NATIVE_ASSERT_PC") &&
            *getenv("B2_NATIVE_ASSERT_PC")) {
            compemu_raw_call_observer_i((uintptr)b2_test_native_entry,
                native_entry_pc);
        }
#endif

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
        if (jit_force_optlev0() || jit_force_optlev0_block_env(block_m68k_pc)) {
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
#if defined(CPU_AARCH64)
        if (jit_strict_full_jit_env() && optlev == 1)
            jit_abort("strict full-JIT: non-L2 block pc=%08x len=%d optlev=%d",
                block_m68k_pc, blocklen, optlev);
#endif
        if (optlev == 0) { /* No need to actually translate */
#if defined(CPU_AARCH64)
          if (jit_strict_full_jit_env())
              jit_abort("strict full-JIT: optlev-0 block pc=%08x len=%d", block_m68k_pc, blocklen);
          if (jit_diag_enabled())
              jit_diag_optlev0_blocks++;
#endif
          /* Execute normally without keeping stats */
            compemu_raw_exec_nostats((uintptr)pc_hist[0].location);
        } else {
#if defined(CPU_AARCH64)
            if (jit_diag_enabled())
                jit_diag_optlev_gt0_blocks++;
            if (jit_strict_full_jit_env())
                jit_strict_compiled_blocks++;
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
            /* Selective materialization: only restore incoming hardware NZCV
               when this block actually consumes incoming CZNV state.
               FLAGX remains separately architectural via regflags.x/FLAGX. */
            if (bi->needed_flags & FLAG_CZNV) {
                LOAD_U64(REG_WORK1, (uintptr)&regflags.nzcv);
                LDR_wXi(REG_WORK2, REG_WORK1, 0);
                MSR_NZCV_x(REG_WORK2);
            }
#endif

            if (trace_emuneigh_env() && trace_emuneigh_target(block_m68k_pc) && trace_emuneigh_count < trace_emuneigh_limit()) {
                uae_u16 first_op = DO_GET_OPCODE(pc_hist[0].location);
                compemu_raw_call_observer_ii((uintptr)trace_emuneigh_entry, block_m68k_pc, first_op);
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
#if defined(CPU_AARCH64)
                if (jit_block_verify_compile_active &&
                    block_m68k_pc == jit_block_verify_compile_pc)
                    jit_block_verify_compiled_ops = i + 1;
#endif
                /* The no-flags table is only safe for instructions that do not
                   architecturally set CCR/X.  A flag-setting op whose flags are
                   not consumed later in the same traced block still has to leave
                   correct architectural flags at the block boundary for the next
                   dispatch/successor block.  Using nfcompfunctbl for CLR/TST/MOVE/
                   arithmetic etc. drops that result-derived CCR state and caused
                   the video-init 0401b70e CLR.W -> block-exit mismatch. */
                if (!needed_flags && currprefs.compnf && !prop[cft_map(opcode)].set_flags) {
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


#if defined(CPU_AARCH64)
                if (was_comp && ((uintptr)comp_pc_p + (uintptr)m68k_pc_offset) != (uintptr)pc_hist[i].location) {
                    /* pc_hist[] can describe traced non-linear control-flow. Keep
                       the compiler's PC cursor and live PC_P fact aligned with
                       the current opcode so extension-word fetches use the right
                       ROM address, but do not emit any runtime PC write here. */
                    comp_pc_p = (uae_u8*)pc_hist[i].location;
                    m68k_pc_offset = 0;
                    set_const(PC_P, (uintptr)pc_hist[i].location);
                }
#endif

#if defined(CPU_AARCH64)
                if (i > 0 && was_comp && (((uae_u16)opcode & 0xfff8) == 0x48e0 || (((uae_u16)opcode & 0xfff8) == 0x4cd8))) {
                    /* MOVEM predecrement/postincrement frame save/restore is very
                       sensitive to coherent A7/A6/PC state. If a traced L2 block
                       reaches it after prior control-flow/frame setup, split the
                       native block here so MOVEM starts from a dispatcher entry.
                       This remains native strict JIT (no interpreter fallback). */
                    flush(1);
                    LOAD_U64(REG_PC_TMP, (uintptr)pc_hist[i].location);
                    compemu_raw_endblock_pc_inreg(REG_PC_TMP, scaled_cycles(i * 4 * CYCLE_UNIT));
                    if (jit_block_verify_compile_active &&
                        block_m68k_pc == jit_block_verify_compile_pc)
                        jit_block_verify_compiled_ops = i;
                    forced_interpreter_barrier = true;
                    break;
                }
#endif

                if (jit_force_exact_exec_nostats_pc(op_m68k_pc)) {
#if defined(CPU_AARCH64)
                    if (jit_strict_full_jit_env())
                        jit_abort("strict full-JIT: exact exec_nostats pc=%08x op=%04x", op_m68k_pc, opcode);
#endif
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
                /* Per-instruction interpreter barriers are empty in production;
                   the strict-only probe exercises the fail-closed fallback path. */
                if (jit_force_interpreter_barrier_opcode((uae_u16)opcode) ||
                    jit_strict_probe_opcode_fallback())
                    allow_l2 = false;
#endif
                if (comptbl[cft_map(opcode)] && optlev > 1 && allow_l2) {
                    failure = 0;
                    if (!was_comp) {
                        comp_pc_p = (uae_u8*)pc_hist[i].location;
                        init_comp();
                    }
                    was_comp = 1;

#if defined(CPU_AARCH64)
                    const bool _verify_this_op = jit_verify_target_pc(op_m68k_pc);
                    const bool _trace_this_op = jit_trace_target_pc(op_m68k_pc);
                    if (_verify_this_op || _trace_this_op) {
                        flush(1);
                        if (_trace_this_op) {
                            compemu_raw_call_observer_ii((uintptr)jit_trace_pc_hit,
                                op_m68k_pc, (1u << 16) | (opcode & 0xffff));
                        }
                        if (_verify_this_op) {
                            compemu_raw_set_pc_i((uintptr)pc_hist[i].location);
                            compemu_raw_call_observer_ii((uintptr)jit_verify_pre, op_m68k_pc, opcode);
                        }
                        comp_pc_p = (uae_u8*)pc_hist[i].location;
                        init_comp();
                    }
#endif
                    jit_compile_current_op_host_pc = (uintptr)pc_hist[i].location;
                    jit_compile_current_op_m68k_pc = op_m68k_pc;
                    jit_emitted_guest_memory_write = false;
                    if (jit_guest_instruction_observer_enabled())
                        compemu_raw_call_observer_i((uintptr)jit_guest_path_record_native,
                            op_m68k_pc);
                    comptbl[cft_map(opcode)](opcode);
                    jit_compile_current_op_host_pc = 0;
                    jit_compile_current_op_m68k_pc = 0;
#if defined(CPU_AARCH64)
                    if (next_pc_p && taken_pc_p &&
                        branch_cc >= NATIVE_CC_F_F && branch_cc <= NATIVE_CC_F_T) {
                        /* FBcc's FCMP NZCV is an edge predicate, not integer
                           CCR.  Its generator saved architectural CCR before
                           target plumbing.  Mark the temporary host flags stale
                           immediately after codegen, before any generic post-op
                           path can flush or drop them. The block edge still
                           consumes the physical NZCV; this is metadata-only. */
                        live.flags_in_flags = TRASH;
                        live.flags_on_stack = VALID;
                        flags_carry_inverted = false;
                    }
                    /* Trace compiled family-d instructions at runtime */
                    if (_verify_this_op ||
                        (((opcode >> 12) & 0xf) == 0xd && getenv("B2_JIT_TRACE_ADD")) ||
                        false /* watch_mem removed */) {
                        uae_u32 pc_val = (uae_u32)((uintptr)pc_hist[i].location - (uintptr)ROMBaseHost + ROMBaseMac);
                        /* Save all caller-saved regs around the trace/verify call */
                        flush(1);
                        if (_verify_this_op)
                            compemu_raw_call_observer_ii((uintptr)jit_verify_post, pc_val,
                                opcode | ((uae_u32)(needed_flags & FLAG_ALL) << 16));
                        else
                            compemu_raw_call_observer_ii((uintptr)jit_trace_add, pc_val, opcode);
                        comp_pc_p = (uae_u8*)pc_hist[i].location;
                        init_comp();
                        was_comp = 0; /* force re-init for next instruction */
                    }
#endif
                    if (jit_emitted_guest_memory_write &&
                        !(prop[cft_map(opcode)].cflow & fl_end_block) &&
                        !jit_force_runtime_pc_endblock) {
                        /* With CACR disabled, an instruction may rewrite code
                           that the tracer had already folded into this block.
                           End after an ordinary writer so the next fetch sees
                           the store. Control-flow writers (JSR/BSR stack pushes,
                           exceptions, runtime helpers) retain their existing
                           dynamic-PC barrier below rather than substituting a
                           sequential successor. */
                        const uintptr write_next_pc =
                            (uintptr)comp_pc_p + (uintptr)m68k_pc_offset;
                        live.flags_are_important = 1;
                        flush(1);
                        LOAD_U64(REG_PC_TMP, write_next_pc);
                        compemu_raw_endblock_pc_inreg(REG_PC_TMP, retired_cycles);
                        forced_interpreter_barrier = true;
                        jit_emitted_guest_memory_write = false;
                        if (jit_block_verify_compile_active &&
                            block_m68k_pc == jit_block_verify_compile_pc)
                            jit_block_verify_compiled_ops = i + 1;
                        break;
                    }
                    jit_emitted_guest_memory_write = false;
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
#if defined(CPU_AARCH64)
                    {
                        const uae_u16 ret_op = (uae_u16)opcode;
                        const uae_u16 ret_op_swapped = (uae_u16)(((ret_op & 0xff) << 8) | (ret_op >> 8));
                        const bool is_dynamic_return =
                            ret_op == 0x4e73 || ret_op == 0x4e74 || ret_op == 0x4e75 || ret_op == 0x4e77 ||
                            ret_op_swapped == 0x4e73 || ret_op_swapped == 0x4e74 || ret_op_swapped == 0x4e75 || ret_op_swapped == 0x4e77;
                        if (is_dynamic_return) {
                        /* Dynamic return opcodes set PC from the stack at runtime.
                           The trace recorder may have followed one observed return
                           target, but the compiled block is reused for later calls
                           with different stack return addresses. End the native
                           block immediately at the runtime PC_P before scratch
                           cleanup can discard the live computed PC. */
                        live.flags_are_important = 1;
                        flush(1);
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
                    }
#endif
#if defined(CPU_AARCH64)
                    {
                        /* DBcc loop back-edge: the per-op codegen computes the
                           correct dynamic PC_P (loop target vs fall-through,
                           honoring the Dn.W decrement and the -1 wrap). Like
                           dynamic returns, the trace recorder only followed one
                           observed outcome, so if the block is allowed to
                           trace-follow PAST the DBcc it bakes in that outcome.
                           End the native block here at the runtime PC_P so both
                           the loop and exit edges are honored. This includes
                           cc=1 (DBF/DBRA): gencomp.c materializes runtime PC_P
                           from the pre-decrement counter for ARM64. DBT/cc=0
                           remains excluded because it is an elaborate nop. */
                        const uae_u16 dop = (uae_u16)opcode;
                        const bool is_dbcc_cond =
                            (((dop & 0xF0F8) == 0x50C8) && (((dop >> 8) & 0xf) >= 1));
                        if (is_dbcc_cond) {
                            live.flags_are_important = 1;
                            flush(1);
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
                    }
#endif
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
                        if (_flush_this_op)
                            compemu_raw_call_observer_i((uintptr)jit_flush_delta_compare, op_m68k_pc);
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
                        /* Translate branch_cc from x86 encoding (gencomp uses
                           flags_x86.h) to ARM64 native condition codes */
                        int arm_branch_cc = branch_cc;
#if defined(CPU_AARCH64)
                        {
                            static const int x86_to_arm[] = {
                                NATIVE_CC_VS, NATIVE_CC_VC, NATIVE_CC_CS, NATIVE_CC_CC,
                                NATIVE_CC_EQ, NATIVE_CC_NE, NATIVE_CC_LS, NATIVE_CC_HI,
                                NATIVE_CC_MI, NATIVE_CC_PL, NATIVE_CC_VS, NATIVE_CC_VC,
                                NATIVE_CC_LT, NATIVE_CC_GE, NATIVE_CC_LE, NATIVE_CC_GT,
                            };
                            if (arm_branch_cc >= 0 && arm_branch_cc < 16)
                                arm_branch_cc = x86_to_arm[arm_branch_cc];
                        }
#endif
                        const bool is_fp_edge =
                            arm_branch_cc >= NATIVE_CC_F_F && arm_branch_cc <= NATIVE_CC_F_T;
                        if (next_traced == taken_pc_p) {
                            side_exit_pc = next_pc_p;
                            /* ARM integer conditions use adjacent inverse IDs.
                               FBcc's sixteen guest predicates use the 68881
                               complement pairing cc ^ 15 instead. */
                            side_cond = is_fp_edge
                                ? (NATIVE_CC_F_F + ((arm_branch_cc - NATIVE_CC_F_F) ^ 0xf))
                                : (arm_branch_cc ^ 1);
                        } else if (next_traced == next_pc_p) {
                            side_exit_pc = taken_pc_p;
                            side_cond = arm_branch_cc;
                        }
                        const bool valid_side_cond =
                            (side_cond >= 0 && side_cond < NATIVE_CC_AL) ||
                            (side_cond >= NATIVE_CC_F_F && side_cond <= NATIVE_CC_F_T);
                        if (valid_side_cond) {
                            bigstate saved_live = live;
                            flush(1);
                            /* Integer Bcc consumes architectural CCR, so restore
                               it after the flush. FBcc must instead retain FCMP's
                               temporary NZCV until this guard has consumed it;
                               its architectural CCR is already saved in memory. */
                            if (!is_fp_edge)
                                make_flags_live();
                            /* Emit side exit for the non-traced branch path.
                               The traced path must skip over the side-exit code;
                               otherwise both outcomes fall through into the
                               side exit and the branch is effectively forced.

                               Do not emit raw ARM CC_B_i here: M68K HI/LS are
                               not identical to ARM HI/LS after our carry
                               normalisation convention.  Use the same helper
                               as end-of-block Bcc emission so composite integer
                               and floating-point conditions share one patch
                               contract. */
                            const int skip_cond = is_fp_edge
                                ? (NATIVE_CC_F_F + ((side_cond - NATIVE_CC_F_F) ^ 0xf))
                                : (side_cond ^ 1);
                            compemu_raw_jcc_l_oponly(skip_cond);
                            uae_u32* patch_skip = (uae_u32*)get_target() - 1;
                            /* Side exit: load PC and endblock */
                            LOAD_U64(REG_PC_TMP, side_exit_pc); /* load into x1 for endblock */
                            compemu_raw_endblock_pc_inreg(REG_PC_TMP,
                                scaled_cycles((i + 1) * 4 * CYCLE_UNIT));
                            /* Patch skip branch to the traced-path continuation */
                            write_jmp_target(patch_skip, (uintptr)get_target());
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
                        {
                            uae_u32 next_m68k_pc = block_m68k_pc + (uae_u32)((uintptr)pc_hist[i + 1].location - (uintptr)pc_hist[0].location);
                            compemu_raw_set_pc_full_i(next_m68k_pc, (uintptr)pc_hist[i + 1].location);
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
                        /* Sync PC to the NEXT instruction — full triple. */
                        {
                            uae_u32 next_m68k_pc = block_m68k_pc + (uae_u32)((uintptr)pc_hist[i + 1].location - (uintptr)pc_hist[0].location);
                            compemu_raw_set_pc_full_i(next_m68k_pc, (uintptr)pc_hist[i + 1].location);
                        }
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
#if defined(CPU_AARCH64)
                    if (jit_strict_full_jit_env())
                        jit_abort("strict full-JIT: opcode fallback pc=%08x op=%04x block=%08x i=%d/%d",
                            op_m68k_pc, opcode, block_m68k_pc, i, blocklen);
#endif
                    {
                        static int fail_log = 0;
                        if (fail_log < 200) {
                            fail_log++;
                            fprintf(stderr, "JIT_FALLBACK op=%04x pc=%08x block=%08x i=%d/%d host=%p comptbl=%p optlev=%d allow_l2=%d\n",
                                (unsigned)opcode, (unsigned)op_m68k_pc,
                                (unsigned)block_m68k_pc, i, blocklen,
                                (void*)pc_hist[i].location,
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
                    /* Static-audit fix: interpreter fallback from a direct-chained
                       compiled block must not inherit stale PC base metadata.
                       Many ARM64 L2 helper blocks are mixed native+fallback
                       because shift/rotate families are still forced through the
                       interpreter. Direct chains typically update regs.pc_p, but
                       m68k_getpc() in the fallback/interpreter uses the full
                       triple regs.pc + (regs.pc_p - regs.pc_oldp). Rebuild a
                       self-consistent triple for the current opcode before the
                       fallback call. */
                    compemu_raw_set_pc_full_i(op_m68k_pc, (uintptr)pc_hist[i].location);
                    if (jit_trace_target_pc(op_m68k_pc)) {
                        compemu_raw_call_observer_ii((uintptr)jit_trace_pc_hit,
                            op_m68k_pc, (2u << 16) | (opcode & 0xffff));
                    }
                    /* Every raw C call below follows AAPCS64 and may overwrite all
                       caller-saved integer and FP registers. Consecutive fallback
                       opcodes can otherwise retain a CLEAN FLAGTMP association from
                       the preceding CCR reload and re-use its clobbered host value.
                       Materialise anything still owned by the allocator, then drop
                       every association before crossing the first C boundary. */
                    prepare_for_call_1();
                    prepare_for_call_2();
#ifdef USE_JIT_FPU
                    /* The interpreter now owns architectural MPFR state. First
                       publish only native-dirty shadows, then re-import the
                       serviced result and clear ownership before any following
                       native instruction or dispatcher exit. */
                    compemu_raw_call_preserve_nzcv((uintptr)jit_fpu_sync_from_shadow);
#endif
                    /* Synchronisation calls are also free to clobber x0/x1, so form
                       the interpreter table arguments only at the final call seam. */
                    compemu_raw_mov_l_ri(REG_PAR1, (uae_u32)cft_map(opcode));
                    compemu_raw_mov_l_rr(REG_PAR2, R_REGSTRUCT);
                    compemu_raw_call((uintptr)cputbl[cft_map(opcode)]);
#ifdef USE_JIT_FPU
                    compemu_raw_call_preserve_nzcv((uintptr)jit_fpu_sync_to_shadow);
#endif
                    /* The C opcode owns architectural CCR in regflags, while
                       AAPCS64 permits it to clobber host NZCV. Re-materialise
                       the guest flags before any native continuation. */
                    live.flags_in_flags = TRASH;
                    live.flags_on_stack = VALID;
                    flags_carry_inverted = false;
                    make_flags_live_internal();
                    {
                        /* cont86 FIX: the interpreter-FALLBACK path must re-dispatch at
                           the LIVE regs.pc_p after ANY control-transfer op (end_block:
                           branch/jump/return/trap), exactly like exec_nostats. A trace-
                           FOLLOWED data-dependent control op (rts/jmp(An)/Bcc) otherwise
                           bakes the compile-time-traced successor and the multi-op block
                           runs the wrong op (measured: rts@0401b6a2 expected 0401b6d2 vs
                           live 0401be50). The L2 NATIVE path already handles this via
                           is_dynamic_return/DBcc/runtime-pc-endblock; this closes the same
                           gap for the interpreter-fallback dispatch. */
                        /* A fallback control-transfer owns the live successor in
                           regs.pc_p.  This is equally true when it is the final
                           traced instruction: allowing finalisation to consult
                           compile-time PC_P in that case can resurrect a stale
                           constant from the preceding native instruction. */
                        if ((prop[cft_map(opcode)].cflow & fl_end_block) != 0) {
                            compemu_raw_mov_l_rm(0, (uintptr)specflags);
                            compemu_raw_maybe_do_nothing(retired_cycles);
                            compemu_raw_mov_l_rm(REG_PC_TMP, (uintptr)&regs.pc_p);
                            compemu_raw_endblock_pc_inreg(REG_PC_TMP, retired_cycles);
                            forced_interpreter_barrier = true;
                            break;
                        }
                    }
                    /* Trace interpreter-executed family-d instructions */
                    if (((opcode >> 12) & 0xf) == 0xd && getenv("B2_JIT_TRACE_ADD")) {
                        uae_u32 pc_val = (uae_u32)((uintptr)pc_hist[i].location - (uintptr)ROMBaseHost + ROMBaseMac);
                        compemu_raw_call_observer_ii((uintptr)jit_trace_add, pc_val, opcode);
                    }
#ifdef PROFILE_UNTRANSLATED_INSNS
                    // raw_cputbl_count[] is indexed with plain opcode (in m68k order)
                    compemu_raw_inc_opcount(opcode);
#endif
                    if (trace_emulopflow_env() && trace_emulopflow_opcode((uae_u16)opcode)) {
                        uae_u32 next_pc = 0xffffffff;
                        if (i + 1 < blocklen)
                            next_pc = block_m68k_pc + (uae_u32)((uintptr)pc_hist[i + 1].location - (uintptr)pc_hist[0].location);
                        compemu_raw_call_observer_ii((uintptr)trace_emulop_resume,
                            (uae_u32)opcode, next_pc);
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
                if (cc >= NATIVE_CC_F_F && cc <= NATIVE_CC_F_T) {
                    /* The FBcc generator saved integer CCR before FCMP. Mark
                       the predicate NZCV as edge-only before flush: flush(1)
                       must write registers and PC, but must not overwrite the
                       saved CCR with FCMP. This changes metadata only; FCMP
                       remains in hardware for the immediately following edge. */
                    live.flags_in_flags = TRASH;
                    live.flags_on_stack = VALID;
                    flags_carry_inverted = false;
                }
                flush(1);                       // Emitted code of this call doesn't modify flags
                compemu_raw_jcc_l_oponly(cc);   // Last emitted opcode is branch to target
                branchadd = (uae_u32*)get_target() - 1;

                /* predicted outcome */
                uintptr ct1 = jit_canonicalize_target_pc(t1);
                tbi = get_blockinfo_addr_new((void*)ct1);
                match_states(tbi);

                /* Use endblock_pc_isconst for ALL blocks (including DBF)
                   to enable countdown + direct chaining. */
                if (jit_collect_edge_profile(bi))
                    compemu_raw_inc_m((uintptr)&bi->edge_exec_count[0]);
                tba = compemu_raw_endblock_pc_isconst(scaled_cycles(totcycles), ct1);
                const bool pred_prefer_direct = jit_source_edge_prefers_direct(bi, 0, ct1);
                write_jmp_target(tba, get_handler_for_edge(bi, 0, ct1));
                create_jmpdep(bi, 0, tba, ct1, pred_prefer_direct);

                /* not-predicted outcome */
                write_jmp_target(branchadd, (uintptr)get_target());
                uintptr ct2 = jit_canonicalize_target_pc(t2);
                tbi = get_blockinfo_addr_new((void*)ct2);
                match_states(tbi);

                if (jit_collect_edge_profile(bi))
                    compemu_raw_inc_m((uintptr)&bi->edge_exec_count[1]);
                tba = compemu_raw_endblock_pc_isconst(scaled_cycles(totcycles), ct2);
                const bool notpred_prefer_direct = jit_source_edge_prefers_direct(bi, 1, ct2);
                write_jmp_target(tba, get_handler_for_edge(bi, 1, ct2));
                create_jmpdep(bi, 1, tba, ct2, notpred_prefer_direct);
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
                    if (jit_collect_edge_profile(bi))
                        compemu_raw_inc_m((uintptr)&bi->edge_exec_count[0]);
                    tba = compemu_raw_endblock_pc_isconst(scaled_cycles(totcycles), cv);
                    const bool linear_prefer_direct = jit_source_edge_prefers_direct(bi, 0, cv);
                    write_jmp_target(tba, get_handler_for_edge(bi, 0, cv));
                    create_jmpdep(bi, 0, tba, cv, linear_prefer_direct);
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
                    if (jit_collect_edge_profile(bi))
                        compemu_raw_inc_m((uintptr)&bi->edge_exec_count[0]);
                    tba = compemu_raw_endblock_pc_isconst(scaled_cycles(totcycles), cv);
                    const bool linear_prefer_direct = jit_source_edge_prefers_direct(bi, 0, cv);
                    write_jmp_target(tba, get_handler_for_edge(bi, 0, cv));
                    create_jmpdep(bi, 0, tba, cv, linear_prefer_direct);
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

        /* Contract-first default on ARM64: route successor handoffs through
           the validated/non-direct wrapper unless explicitly overridden.
           This keeps constant-successor branch chains aligned with block
           lifecycle validation while hot-chain PC/state transfer is still
           being normalized. It also covers the existing optlev=0 case, where
           exec_nostats() requires fully canonical in-memory guest state. */
#if defined(CPU_AARCH64)
        if (!was_comp || jit_prefer_validated_successor_handler() || jit_force_nondirect_handler_env() || jit_force_nondirect_target_env((uintptr)bi->pc_p)) {
            set_dhtu(bi, bi->handler);
        }
#else
        if (!was_comp || jit_force_nondirect_handler_env() || jit_force_nondirect_target_env((uintptr)bi->pc_p)) {
            set_dhtu(bi, bi->handler);
        }
#endif



        flush_cpu_icache((void*)current_block_start_target, (void*)target);
        current_compile_p = get_target();
        raise_in_cl_list(bi);
        bi->nexthandler = current_compile_p;

        bi->status = BI_ACTIVE;
#if defined(CPU_AARCH64)
        /* RAM blocks compiled from zeroed source: keep as BI_NEED_CHECK.
           The checksum validates (zeros) so subsequent dispatches promote
           to BI_ACTIVE if content didn't change, or invalidate if it did.
           This prevents infinite execution of stale zeros-compiled native code
           when a branch accidentally targets uninitialized RAM. */
        if (block_m68k_pc < ROMBaseMac && blocklen > 0) {
            const uae_u16 *_w0 = (const uae_u16 *)pc_hist[0].location;
            if (*_w0 == 0) {
                if (jit_strict_full_jit_env()) {
                    /* Strict mode must execute translated code, but zero-filled
                       RAM is mutable and may later receive real guest code.
                       Validate its source checksum before every activation; a
                       change invalidates/recompiles the block without ever
                       routing unchanged source through the interpreter. */
                    bi->handler_to_use = (cpuop_func*)popall_check_checksum;
                    set_dhtu_validated(bi, bi->direct_pcc);
                    cache_tags[cacheline(pc_hist[0].location)].handler =
                        (cpuop_func*)popall_check_checksum;
                    bi->status = BI_NEED_CHECK;
                } else {
                    /* Ordinary mode avoids compiling transient zero runs. Route
                       both cache dispatch and inbound dependencies through the
                       interpreter stub so no edge retains stale native zeros. */
                    bi->handler_to_use = (cpuop_func*)popall_execute_normal;
                    bi->handler = (cpuop_func*)popall_execute_normal;
                    bi->direct_handler = bi->direct_pen;
                    set_dhtu(bi, bi->direct_pen);
                    cache_tags[cacheline(pc_hist[0].location)].handler =
                        (cpuop_func*)popall_execute_normal;
                }
            }
        }
#endif
        jit_trace_edge_snapshot("BUILD", bi);
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
        /* Reclaim only after the final use of bi.  flush_icache_hard() frees
           every active/dormant blockinfo, including the block just linked
           above; flushing before status/finalization updates was a use-after-
           free at the code-cache boundary.  A hard flush is required here
           because a lazy flush deliberately does not rewind current_compile_p. */
        if (current_compile_p >= MAX_COMPILE_PTR)
            flush_icache_hard(3);
		jit_end_write_window();
    }
}

/* Instruction-cache invalidation requested through the core/host bridge is a
   validation boundary, not a code-cache allocation boundary. This covers guest
   CINV/CPUSH/CACR transitions and host code-patch notification. Strict mode uses
   the repaired lazy lifecycle so unchanged RAM blocks reactivate only after
   checksum validation; changed blocks invalidate and retrace. Ordinary mode
   retains its configured hard/lazy policy. Allocation, teardown and code-cache
   exhaustion continue to call flush_icache_hard() directly. Keep this definition
   after inventoried boundaries so policy-only changes do not rewrite their
   historical locations. */
static inline void flush_icache_architectural(int reason)
{
    if (jit_strict_full_jit_env() || lazy_flush)
        flush_icache_lazy(reason);
    else
        flush_icache_hard(reason);
}

#endif /* JIT */

