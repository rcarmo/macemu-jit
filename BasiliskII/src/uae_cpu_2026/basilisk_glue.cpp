/*
 *  basilisk_glue.cpp - Glue UAE CPU to Basilisk II CPU engine interface
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"

#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "emul_op.h"
#include "rom_patches.h"
#include "timer.h"
#include "m68k.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"
#include "fpu/fpu.h"
#include "compiler/compemu.h"
#include "unicorn_cpu.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static uint64_t double_bits_glue(double value)
{
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static bool test_dump_fp_enabled_glue()
{
	const char *env = getenv("B2_TEST_DUMP_FP");
	return env && *env && strcmp(env, "0") != 0;
}

static bool trace_d6_enabled_glue()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_D6") && *getenv("B2_TRACE_D6")) ? 1 : 0;
	return cached != 0;
}

static bool trace_irqmanaged_env_glue()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TRACE_IRQMANAGED") && *getenv("B2_TRACE_IRQMANAGED") && strcmp(getenv("B2_TRACE_IRQMANAGED"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

// RAM and ROM pointers
uint32 RAMBaseMac = 0;		// RAM base (Mac address space) gb-- initializer is important
uint8 *RAMBaseHost;			// RAM base (host address space)
uint32 RAMSize;				// Size of RAM
uint32 ROMBaseMac;			// ROM base (Mac address space)
uint8 *ROMBaseHost;			// ROM base (host address space)
uint32 ROMSize;				// Size of ROM

#if !REAL_ADDRESSING
// Mac frame buffer
uint8 *MacFrameBaseHost;	// Frame buffer base (host address space)
uint32 MacFrameSize;		// Size of frame buffer
int MacFrameLayout;			// Frame buffer layout
#endif

#if DIRECT_ADDRESSING
uintptr MEMBaseDiff;		// Global offset between a Mac address and its Host equivalent
#endif

#if USE_JIT
bool UseJIT = false;
#endif

static bool deferred_irq_env()
{
	static int cached = -1;
	if (cached < 0) {
		const char *managed = getenv("B2_JIT_MANAGED_IRQ");
		const char *deferred = getenv("B2_JIT_DEFER_IRQ");
		cached = ((managed && *managed && strcmp(managed, "0") != 0) ||
			(deferred && *deferred && strcmp(deferred, "0") != 0)) ? 1 : 0;
	}
	return cached != 0;
}

static uint32 deferred_irq_flags = 0;
static bool deferred_irq_active = false;

// #if defined(ENABLE_EXCLUSIVE_SPCFLAGS) && !defined(HAVE_HARDWARE_LOCKS)
B2_mutex *spcflags_lock = NULL;
// #endif

// From newcpu.cpp
extern int quit_program;


/*
 *  Initialize 680x0 emulation, CheckROM() must have been called first
 */

bool Init680x0(void)
{
	spcflags_lock = B2_create_mutex();
#if REAL_ADDRESSING
	// Mac address space = host address space
	RAMBaseMac = (uintptr)RAMBaseHost;
	ROMBaseMac = (uintptr)ROMBaseHost;
#elif DIRECT_ADDRESSING
	// Mac address space = host address space minus constant offset (MEMBaseDiff)
	// NOTE: MEMBaseDiff is set up in main_unix.cpp/main()
	RAMBaseMac = 0;
	ROMBaseMac = Host2MacAddr(ROMBaseHost);
#else
	// Initialize UAE memory banks
	RAMBaseMac = 0;
	switch (ROMVersion) {
		case ROM_VERSION_64K:
		case ROM_VERSION_PLUS:
		case ROM_VERSION_CLASSIC:
			ROMBaseMac = 0x00400000;
			break;
		case ROM_VERSION_II:
			ROMBaseMac = 0x00a00000;
			break;
		case ROM_VERSION_32:
			ROMBaseMac = 0x40800000;
			break;
		default:
			return false;
	}
	memory_init();
#endif

	init_m68k();

	// Unicorn replaces both the UAE interpreter and the JIT when available
	const bool use_unicorn = unicorn_cpu_init();
#if USE_JIT
	UseJIT = !use_unicorn && compiler_use_jit();
	if (UseJIT)
	    compiler_init();
	else
		TimerRestoreAsyncOwnership();
#else
	(void)use_unicorn;
#endif
	return true;
}


/*
 *  Host code wrote guest memory: invalidate translated code
 */

void HostWroteMacMemory(uint32 addr, uint32 size)
{
	if (unicorn_cpu_enabled)
		unicorn_cpu_invalidate(addr, size);
}


/*
 *  Deinitialize 680x0 emulation
 */

void Exit680x0(void)
{
	unicorn_cpu_exit();
#if USE_JIT
    if (UseJIT)
	compiler_exit();
#endif
	exit_m68k();
}


/*
 *  Initialize memory mapping of frame buffer (called upon video mode change)
 */

void InitFrameBufferMapping(void)
{
#if !REAL_ADDRESSING && !DIRECT_ADDRESSING
	memory_init();
#endif
}

/*
 *  Reset and start 680x0 emulation (doesn't return)
 */

static bool test_dump_enabled_glue()
{
	static int cached = -1;
	if (cached < 0)
		cached = (getenv("B2_TEST_DUMP") && *getenv("B2_TEST_DUMP") && strcmp(getenv("B2_TEST_DUMP"), "0") != 0) ? 1 : 0;
	return cached != 0;
}

static bool parse_test_hex_words_glue(const char *hex, uint16 *out_words, size_t max_words, size_t *out_count)
{
	size_t n = 0;
	const char *p = hex;
	while (*p) {
		while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';' || *p == ':'))
			p++;
		if (!*p)
			break;
		if (n >= max_words)
			return false;
		char *end = NULL;
		unsigned long v = strtoul(p, &end, 16);
		if (end == p || v > 0xffff)
			return false;
		out_words[n++] = (uint16)v;
		p = end;
	}
	*out_count = n;
	return n > 0;
}

static bool parse_test_hex_longs_glue(const char *hex, uint32 *out_longs, size_t max_longs, size_t *out_count)
{
	size_t n = 0;
	const char *p = hex;
	while (*p) {
		while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';' || *p == ':'))
			p++;
		if (!*p)
			break;
		if (n >= max_longs)
			return false;
		char *end = NULL;
		unsigned long v = strtoul(p, &end, 16);
		if (end == p || v > 0xffffffffUL)
			return false;
		out_longs[n++] = (uint32)v;
		p = end;
	}
	*out_count = n;
	return n > 0;
}

static bool restore_test_bytes_glue(const char *env_name)
{
	const char *env = getenv(env_name);
	if (!(env && *env))
		return true;

	uint32 pairs[128];
	size_t count = 0;
	if (!parse_test_hex_longs_glue(env, pairs, lengthof(pairs), &count) ||
		(count & 1) != 0) {
		fprintf(stderr, "%s parse failed (need address/value pairs)\n", env_name);
		return false;
	}
	for (size_t i = 0; i < count; i += 2) {
		const uint32 offset = pairs[i];
		const uint32 value = pairs[i + 1];
		if (offset >= RAMSize || value > 0xff) {
			fprintf(stderr, "%s range failed at pair %lu\n", env_name,
				(unsigned long)(i / 2));
			return false;
		}
		put_byte(RAMBaseMac + (uaecptr)offset, (uint8)value);
	}
	return true;
}

static bool restore_test_replay_bytes_glue()
{
	/* A dedicated replay image may override the initial byte fixture.  Otherwise
	   restore that fixture before every exact-PC pass so RMW/copy vectors cannot
	   consume state left by the trace pass. */
	const char *replay = getenv("B2_TEST_REPLAY_BYTES");
	return restore_test_bytes_glue(replay && *replay
		? "B2_TEST_REPLAY_BYTES" : "B2_TEST_MEMORY_BYTES");
}

static void dump_test_mem_ranges_glue()
{
	const char *env = getenv("B2_TEST_MEMDUMP");
	if (!(env && *env))
		return;
	const char *p = env;
	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';')
			p++;
		if (!*p)
			break;
		char *end = NULL;
		unsigned long addr = strtoul(p, &end, 0);
		if (end == p)
			break;
		p = end;
		while (*p == ' ' || *p == '\t' || *p == ':' || *p == '+')
			p++;
		unsigned long len = strtoul(p, &end, 0);
		if (end == p || len == 0)
			break;
		p = end;
		fprintf(stderr, "MEMDUMP %08lx:", addr);
		for (unsigned long i = 0; i < len; i++)
			fprintf(stderr, " %02x", (unsigned)get_byte((uaecptr)(addr + i)) & 0xff);
		fprintf(stderr, "\n");
	}
}

static bool run_opcode_test_mode_glue()
{
	const char *hex = getenv("B2_TEST_HEX");
	if (!(hex && *hex))
		return false;

	uint16 words[1024];
	size_t n_words = 0;
	if (!parse_test_hex_words_glue(hex, words, lengthof(words), &n_words)) {
		fprintf(stderr, "B2_TEST_HEX parse failed\n");
		quit_program = 1;
		return true;
	}

	const uaecptr test_addr = RAMBaseMac + 0x1000;
	const uaecptr stack_addr = RAMBaseMac + RAMSize - 0x2000;
	for (size_t i = 0; i < n_words; i++)
		put_word(test_addr + (uaecptr)(i * 2), words[i]);
	put_word(test_addr + (uaecptr)(n_words * 2), M68K_EXEC_RETURN);
	if (!restore_test_bytes_glue("B2_TEST_MEMORY_BYTES")) {
		quit_program = 1;
		return true;
	}
#if defined(USE_JIT) && (defined(CPU_AARCH64) || defined(CPU_aarch64))
	jit_invalidate_host_code_write(test_addr, (uae_u32)((n_words + 1) * 2));
#endif

	for (int i = 0; i < 8; i++) {
		m68k_dreg(regs, i) = 0;
		m68k_areg(regs, i) = 0;
	}
	m68k_areg(regs, 7) = stack_addr;
	regs.usp = regs.isp = regs.msp = stack_addr;
	regs.sr = 0x2700;
	MakeFromSR(); /* ensure regflags matches regs.sr */

	const char *init = getenv("B2_TEST_INIT");
	if (init && *init) {
		uint32 init_words[17]; /* D0-D7, A0-A7, optional SR */
		size_t init_count = 0;
		if (!parse_test_hex_longs_glue(init, init_words, lengthof(init_words), &init_count) ||
			(init_count != 16 && init_count != 17)) {
			fprintf(stderr, "B2_TEST_INIT parse failed (need 16 or 17 hex words)\n");
			quit_program = 1;
			return true;
		}
		for (int i = 0; i < 8; i++)
			m68k_dreg(regs, i) = init_words[i];
		for (int i = 0; i < 8; i++)
			m68k_areg(regs, i) = init_words[8 + i];
		if (init_count == 17)
			regs.sr = (uint16)(init_words[16] & 0xffff);
		regs.usp = regs.isp = regs.msp = m68k_areg(regs, 7);
	}
	MakeFromSR();
	regs.stopped = 0;
	SPCFLAGS_CLEAR(SPCFLAG_STOP | SPCFLAG_BRK | SPCFLAG_DOTRACE | SPCFLAG_TRACE);

	m68k_setpc(test_addr);
	fill_prefetch_0();
	quit_program = 0;
#if USE_JIT
	if (UseJIT)
		m68k_compile_execute();
	else
#endif
		m68k_execute();

	const char *two_pass = getenv("B2_TEST_TWO_PASS");
	if (two_pass && *two_pass && two_pass[0] != '0') {
		/* A host-injected stream may deliberately reuse an address with new
		   opcodes (Execute68kTrap does this on the guest stack). Rewrite it only
		   after the first pass has had a chance to enter the translation cache. */
		const char *rewrite_hex = getenv("B2_TEST_REWRITE_HEX");
		if (rewrite_hex && *rewrite_hex) {
			uint16 rewrite_words[1024];
			size_t rewrite_count = 0;
			if (!parse_test_hex_words_glue(rewrite_hex, rewrite_words,
					lengthof(rewrite_words), &rewrite_count)) {
				fprintf(stderr, "B2_TEST_REWRITE_HEX parse failed\n");
				quit_program = 1;
				return true;
			}
			for (size_t i = 0; i < rewrite_count; i++)
				put_word(test_addr + (uaecptr)(i * 2), rewrite_words[i]);
			put_word(test_addr + (uaecptr)(rewrite_count * 2), M68K_EXEC_RETURN);
#if defined(USE_JIT) && (defined(CPU_AARCH64) || defined(CPU_aarch64))
			jit_invalidate_host_code_write(test_addr,
				(uae_u32)((rewrite_count + 1) * 2));
#endif
		}

		/* Harness mode: replay from restored architectural input state. One
		   replay gives the historical two-pass trace/native proof. Coherency
		   vectors may request another replay: pass one dirties and invalidates,
		   pass two retraces stable rewritten code, and pass three must enter it
		   natively. Keep this test-only control out of ordinary execution. */
		int replay_count = 1;
		const char *replay_count_env = getenv("B2_TEST_REPLAY_COUNT");
		if (replay_count_env && *replay_count_env) {
			char *end = NULL;
			long parsed = strtol(replay_count_env, &end, 0);
			if (end != replay_count_env && parsed >= 1 && parsed <= 8)
				replay_count = (int)parsed;
		}
		for (int replay = 0; replay < replay_count; replay++) {
			if (!restore_test_replay_bytes_glue()) {
				quit_program = 1;
				return true;
			}
			for (int i = 0; i < 8; i++) {
				m68k_dreg(regs, i) = 0;
				m68k_areg(regs, i) = 0;
			}
			m68k_areg(regs, 7) = stack_addr;
			regs.usp = regs.isp = regs.msp = stack_addr;
			regs.sr = 0x2700;
			if (init && *init) {
				uint32 init_words[17];
				size_t init_count = 0;
				if (!parse_test_hex_longs_glue(init, init_words, lengthof(init_words), &init_count) ||
					(init_count != 16 && init_count != 17)) {
					fprintf(stderr, "B2_TEST_INIT parse failed on replay reset\n");
					quit_program = 1;
					return true;
				}
				for (int i = 0; i < 8; i++)
					m68k_dreg(regs, i) = init_words[i];
				for (int i = 0; i < 8; i++)
					m68k_areg(regs, i) = init_words[8 + i];
				if (init_count == 17)
					regs.sr = (uint16)(init_words[16] & 0xffff);
				regs.usp = regs.isp = regs.msp = m68k_areg(regs, 7);
			}
			MakeFromSR();
			/* Exact-native FPU vectors may preserve an FP operand prepared by the
			   trace pass while requiring a known architectural FPSR baseline. This
			   prevents trace-pass exceptions from masquerading as native evidence. */
			const char *replay_fpsr_env = getenv("B2_TEST_REPLAY_FPSR");
			if (replay_fpsr_env && *replay_fpsr_env) {
				char *end = NULL;
				unsigned long replay_fpsr = strtoul(replay_fpsr_env, &end, 16);
				if (end == replay_fpsr_env || *end != '\0' || replay_fpsr > 0xffffffffUL) {
					fprintf(stderr, "B2_TEST_REPLAY_FPSR parse failed\n");
					quit_program = 1;
					return true;
				}
				fpu_set_fpsr((uae_u32)replay_fpsr);
			}
			const char *replay_fpcr_env = getenv("B2_TEST_REPLAY_FPCR");
			if (replay_fpcr_env && *replay_fpcr_env) {
				char *end = NULL;
				unsigned long replay_fpcr = strtoul(replay_fpcr_env, &end, 16);
				if (end == replay_fpcr_env || *end != '\0' || replay_fpcr > 0xffffffffUL) {
					fprintf(stderr, "B2_TEST_REPLAY_FPCR parse failed\n");
					quit_program = 1;
					return true;
				}
				fpu_set_fpcr((uae_u32)replay_fpcr);
			}
#ifdef FPU_MPFR
			for (int fpreg = 0; fpreg < 8; fpreg++) {
				char env_name[32];
				snprintf(env_name, sizeof(env_name), "B2_TEST_REPLAY_FP%d_EXT", fpreg);
				const char *replay_fp_env = getenv(env_name);
				if (!replay_fp_env || !*replay_fp_env)
					continue;
				uint32 words[3];
				size_t count = 0;
				if (!parse_test_hex_longs_glue(replay_fp_env, words, lengthof(words), &count)
						|| count != 3 || !fpu_test_set_register_extended(fpreg, words[0], words[1], words[2])) {
					fprintf(stderr, "%s parse failed (need 3 hex words)\n", env_name);
					quit_program = 1;
					return true;
				}
			}
#endif
			regs.stopped = 0;
			SPCFLAGS_CLEAR(SPCFLAG_STOP | SPCFLAG_BRK | SPCFLAG_DOTRACE | SPCFLAG_TRACE);
			uaecptr second_addr = test_addr;
			const char *second_pc_env = getenv("B2_TEST_SECOND_PC");
			if (second_pc_env && *second_pc_env) {
				char *end = NULL;
				unsigned long off = strtoul(second_pc_env, &end, 0);
				if (end != second_pc_env)
					second_addr = RAMBaseMac + (uaecptr)off;
			}
			m68k_setpc(second_addr);
			fill_prefetch_0();
			quit_program = 0;
#if defined(USE_JIT) && (defined(CPU_AARCH64) || defined(CPU_aarch64))
			if (UseJIT) {
				extern bool jit_test_prepare_direct_checksum_entry(void);
				extern bool jit_test_prepare_direct_execute_normal_entry(void);
				extern bool jit_test_prepare_execute_normal_cycles_entry(void);
				if (!jit_test_prepare_direct_checksum_entry()) {
					fprintf(stderr, "B2_TEST_FORCE_DIRECT_CHECKSUM could not prepare direct checksum entry\n");
					quit_program = 1;
					return true;
				}
				if (!jit_test_prepare_direct_execute_normal_entry()) {
					fprintf(stderr, "B2_TEST_FORCE_DIRECT_EXECUTE_NORMAL could not prepare direct execute_normal entry\n");
					quit_program = 1;
					return true;
				}
				if (!jit_test_prepare_execute_normal_cycles_entry()) {
					fprintf(stderr, "B2_TEST_EXECUTE_NORMAL_CYCLES_SEED could not prepare execute_normal_cycles entry\n");
					quit_program = 1;
					return true;
				}
				const char *maybe_do_nothing_spcflags = getenv("B2_TEST_MAYBE_DO_NOTHING_SPCFLAGS");
				if (maybe_do_nothing_spcflags && *maybe_do_nothing_spcflags) {
					if (strcmp(maybe_do_nothing_spcflags, "0") != 0 && strcmp(maybe_do_nothing_spcflags, "64") != 0) {
						fprintf(stderr, "B2_TEST_MAYBE_DO_NOTHING_SPCFLAGS parse failed (need 0 or 64)\n");
						quit_program = 1;
						return true;
					}
					if (strcmp(maybe_do_nothing_spcflags, "64") == 0)
						SPCFLAGS_SET(SPCFLAG_JIT_END_COMPILE);
				}
			}
#endif
#if USE_JIT
			if (UseJIT)
				m68k_compile_execute();
			else
#endif
				m68k_execute();
		}
	}

#if defined(USE_JIT) && (defined(CPU_AARCH64) || defined(CPU_aarch64))
	extern void jit_test_dump_dispatch_summary(void);
	jit_test_dump_dispatch_summary();
#endif

	if (test_dump_enabled_glue()) {
		MakeSR();
		fprintf(stderr,
			"REGDUMP: D0=%08x D1=%08x D2=%08x D3=%08x D4=%08x D5=%08x D6=%08x D7=%08x "
			"A0=%08x A1=%08x A2=%08x A3=%08x A4=%08x A5=%08x A6=%08x A7=%08x SR=%04x FPSR=%08x",
			(unsigned)m68k_dreg(regs, 0), (unsigned)m68k_dreg(regs, 1),
			(unsigned)m68k_dreg(regs, 2), (unsigned)m68k_dreg(regs, 3),
			(unsigned)m68k_dreg(regs, 4), (unsigned)m68k_dreg(regs, 5),
			(unsigned)m68k_dreg(regs, 6), (unsigned)m68k_dreg(regs, 7),
			(unsigned)m68k_areg(regs, 0), (unsigned)m68k_areg(regs, 1),
			(unsigned)m68k_areg(regs, 2), (unsigned)m68k_areg(regs, 3),
			(unsigned)m68k_areg(regs, 4), (unsigned)m68k_areg(regs, 5),
			(unsigned)m68k_areg(regs, 6), (unsigned)m68k_areg(regs, 7), (unsigned)regs.sr,
			(unsigned)fpu_get_fpsr());
#if defined(USE_JIT) && (defined(CPU_arm) || defined(CPU_aarch64) || defined(CPU_AARCH64))
		if (test_dump_fp_enabled_glue())
			fprintf(stderr,
			" FP0=%016llx FP1=%016llx FP2=%016llx FP3=%016llx FP4=%016llx FP5=%016llx FP6=%016llx FP7=%016llx",
			(unsigned long long)double_bits_glue(regs.jit_fpregs[0]),
			(unsigned long long)double_bits_glue(regs.jit_fpregs[1]),
			(unsigned long long)double_bits_glue(regs.jit_fpregs[2]),
			(unsigned long long)double_bits_glue(regs.jit_fpregs[3]),
			(unsigned long long)double_bits_glue(regs.jit_fpregs[4]),
			(unsigned long long)double_bits_glue(regs.jit_fpregs[5]),
			(unsigned long long)double_bits_glue(regs.jit_fpregs[6]),
			(unsigned long long)double_bits_glue(regs.jit_fpregs[7]));
#endif
		fputc('\n', stderr);
		dump_test_mem_ranges_glue();
	}

	return true;
}

void Start680x0(void)
{
	m68k_reset();
	if (run_opcode_test_mode_glue())
		return;
#if USE_JIT
    if (UseJIT)
	m68k_compile_execute();
    else
#endif
	m68k_execute();
}


/*
 *  Trigger interrupt
 */

void TriggerInterrupt(void)
{
	idle_resume();
	/* Always surface SPCFLAG_INT when there are pending interrupts.
	   The old deferred model suppressed SPCFLAG_INT when intmask >= 1,
	   which caused interrupts arriving during high-IPL critical sections
	   to be silently lost — InterruptFlags was set but SPCFLAG_INT never
	   was, and when IPL dropped back to 0 nobody re-checked.
	   Now we always set SPCFLAG_INT; the spcflags dispatch in newcpu.cpp
	   already checks intmask vs interrupt level before delivery. */
	SPCFLAGS_SET(SPCFLAG_INT);
	if (unicorn_cpu_enabled)
		unicorn_cpu_request_stop();
	if (UseDeferredInterruptModel() && trace_irqmanaged_env_glue()) {
		static unsigned long trace_count = 0;
		if (trace_count < 4000) {
			MakeSR();
			fprintf(stderr, "IRQM trigger %lu pc=%08x sr=%04x intmask=%u spc=%08x live=%08x latched=%08x active=%d\n",
				++trace_count,
				(unsigned)m68k_getpc(),
				(unsigned)regs.sr,
				(unsigned)regs.intmask,
				(unsigned)regs.spcflags,
				(unsigned)InterruptFlags,
				(unsigned)deferred_irq_flags,
				deferred_irq_active ? 1 : 0);
		}
	}
}

void TriggerNMI(void)
{
	//!! not implemented yet
	// SPCFLAGS_SET( SPCFLAG_BRK ); // use _BRK for NMI
}

bool UseDeferredInterruptModel(void)
{
#if USE_JIT
	return UseJIT && deferred_irq_env();
#else
	return false;
#endif
}

uint32 ConsumeDeferredInterruptFlags(void)
{
	uint32 flags = deferred_irq_flags;
	deferred_irq_flags = 0;
	deferred_irq_active = false;
	if (trace_irqmanaged_env_glue()) {
		static unsigned long trace_count = 0;
		if (trace_count < 4000) {
			MakeSR();
			fprintf(stderr, "IRQM consume %lu pc=%08x sr=%04x intmask=%u spc=%08x take=%08x live=%08x\n",
				++trace_count,
				(unsigned)m68k_getpc(),
				(unsigned)regs.sr,
				(unsigned)regs.intmask,
				(unsigned)regs.spcflags,
				(unsigned)flags,
				(unsigned)InterruptFlags);
		}
	}
	if (InterruptFlags)
		SPCFLAGS_SET(SPCFLAG_INT);
	return flags;
}

/*
 *  Get 68k interrupt level
 */

int intlev(void)
{
	if (!UseDeferredInterruptModel())
		return InterruptFlags ? 1 : 0;

	if (deferred_irq_active) {
		if (trace_irqmanaged_env_glue()) {
			static unsigned long busy_count = 0;
			if (busy_count < 4000) {
				MakeSR();
				fprintf(stderr, "IRQM intlev busy %lu pc=%08x sr=%04x intmask=%u spc=%08x live=%08x latched=%08x\n",
					++busy_count,
					(unsigned)m68k_getpc(),
					(unsigned)regs.sr,
					(unsigned)regs.intmask,
					(unsigned)regs.spcflags,
					(unsigned)InterruptFlags,
					(unsigned)deferred_irq_flags);
			}
		}
		return 0;
	}

	if (regs.intmask >= 1) {
		if (trace_irqmanaged_env_glue()) {
			static unsigned long masked_count = 0;
			if (masked_count < 4000) {
				MakeSR();
				fprintf(stderr, "IRQM intlev masked %lu pc=%08x sr=%04x intmask=%u spc=%08x live=%08x\n",
					++masked_count,
					(unsigned)m68k_getpc(),
					(unsigned)regs.sr,
					(unsigned)regs.intmask,
					(unsigned)regs.spcflags,
					(unsigned)InterruptFlags);
			}
		}
		return InterruptFlags ? 1 : 0;
	}

	const uint32 flags = ConsumeInterruptFlags();
	if (trace_irqmanaged_env_glue()) {
		static unsigned long sample_count = 0;
		if (sample_count < 4000) {
			MakeSR();
			fprintf(stderr, "IRQM intlev sample %lu pc=%08x sr=%04x intmask=%u spc=%08x sampled=%08x live_after=%08x\n",
				++sample_count,
				(unsigned)m68k_getpc(),
				(unsigned)regs.sr,
				(unsigned)regs.intmask,
				(unsigned)regs.spcflags,
				(unsigned)flags,
				(unsigned)InterruptFlags);
		}
	}
	if (!flags)
		return 0;

	deferred_irq_flags = flags;
	deferred_irq_active = true;
	if (trace_irqmanaged_env_glue()) {
		static unsigned long accept_count = 0;
		if (accept_count < 4000) {
			MakeSR();
			fprintf(stderr, "IRQM intlev accept %lu pc=%08x sr=%04x intmask=%u spc=%08x latched=%08x\n",
				++accept_count,
				(unsigned)m68k_getpc(),
				(unsigned)regs.sr,
				(unsigned)regs.intmask,
				(unsigned)regs.spcflags,
				(unsigned)deferred_irq_flags);
		}
	}
	return 1;
}


/*
 *  Execute MacOS 68k trap
 *  r->a[7] and r->sr are unused!
 */

void Execute68kTrap(uint16 trap, struct M68kRegisters *r)
{
	int i;

	if (trace_d6_enabled_glue())
		fprintf(stderr, "TRACE_D6 Execute68kTrap enter trap=%04x oldpc=%08x d6=%08x d7=%08x a4=%08x a5=%08x\n", trap, m68k_getpc(), r->d[6], r->d[7], r->a[4], r->a[5]);

	// Save old PC
	uaecptr oldpc = m68k_getpc();

	// Set registers
	for (i=0; i<8; i++)
		m68k_dreg(regs, i) = r->d[i];
	for (i=0; i<7; i++)
		m68k_areg(regs, i) = r->a[i];

	// Push trap and EXEC_RETURN on stack
	m68k_areg(regs, 7) -= 2;
	put_word(m68k_areg(regs, 7), M68K_EXEC_RETURN);
	m68k_areg(regs, 7) -= 2;
	put_word(m68k_areg(regs, 7), trap);
#if defined(USE_JIT) && (defined(CPU_AARCH64) || defined(CPU_aarch64))
	jit_invalidate_host_code_write(m68k_areg(regs, 7), 4);
#endif

	// Execute trap
	m68k_setpc(m68k_areg(regs, 7));
	fill_prefetch_0();
	quit_program = 0;
	m68k_execute();

	// Clean up stack
	m68k_areg(regs, 7) += 4;

	// Restore old PC
	m68k_setpc(oldpc);
	fill_prefetch_0();

	// Get registers
	for (i=0; i<8; i++)
		r->d[i] = m68k_dreg(regs, i);
	for (i=0; i<7; i++)
		r->a[i] = m68k_areg(regs, i);
	if (trace_d6_enabled_glue())
		fprintf(stderr, "TRACE_D6 Execute68kTrap leave trap=%04x restorepc=%08x d6=%08x d7=%08x a4=%08x a5=%08x\n", trap, oldpc, r->d[6], r->d[7], r->a[4], r->a[5]);
	quit_program = 0;
}


/*
 *  Execute 68k subroutine
 *  The executed routine must reside in UAE memory!
 *  r->a[7] and r->sr are unused!
 */

void Execute68k(uint32 addr, struct M68kRegisters *r)
{
	int i;

	if (trace_d6_enabled_glue())
		fprintf(stderr, "TRACE_D6 Execute68k enter addr=%08x oldpc=%08x d6=%08x d7=%08x a4=%08x a5=%08x\n", addr, m68k_getpc(), r->d[6], r->d[7], r->a[4], r->a[5]);

	// Save old PC
	uaecptr oldpc = m68k_getpc();

	// Set registers
	for (i=0; i<8; i++)
		m68k_dreg(regs, i) = r->d[i];
	for (i=0; i<7; i++)
		m68k_areg(regs, i) = r->a[i];

	// Push EXEC_RETURN and faked return address (points to EXEC_RETURN) on stack
	m68k_areg(regs, 7) -= 2;
	put_word(m68k_areg(regs, 7), M68K_EXEC_RETURN);
	m68k_areg(regs, 7) -= 4;
	put_long(m68k_areg(regs, 7), m68k_areg(regs, 7) + 4);
#if defined(USE_JIT) && (defined(CPU_AARCH64) || defined(CPU_aarch64))
	jit_invalidate_host_code_write(m68k_areg(regs, 7), 6);
#endif

	// Execute routine
	m68k_setpc(addr);
	fill_prefetch_0();
	quit_program = 0;
	m68k_execute();

	// Clean up stack
	m68k_areg(regs, 7) += 2;

	// Restore old PC
	m68k_setpc(oldpc);
	fill_prefetch_0();

	// Get registers
	for (i=0; i<8; i++)
		r->d[i] = m68k_dreg(regs, i);
	for (i=0; i<7; i++)
		r->a[i] = m68k_areg(regs, i);
	if (trace_d6_enabled_glue())
		fprintf(stderr, "TRACE_D6 Execute68k leave addr=%08x restorepc=%08x d6=%08x d7=%08x a4=%08x a5=%08x\n", addr, oldpc, r->d[6], r->d[7], r->a[4], r->a[5]);
	quit_program = 0;
}

void report_double_bus_error()
{
#if 0
	panicbug("CPU: Double bus fault detected !");
	/* would be cool to open SDL dialog here: */
	/* [Double bus fault detected. The emulated system crashed badly.
	    Do you want to reset ARAnyM or quit ?] [Reset] [Quit]"
	*/
	panicbug(CPU_MSG);
	CPU_ACTION;
#endif
}
