/*
 *  unicorn_cpu.cpp - 680x0 execution backend using the Unicorn engine
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

/*
 *  Design notes (see also unicorn_cpu.h)
 *
 *  - State exchange: D0-D7/A0-A7, PC, SR, USP/ISP/MSP, SFC/DFC and CACR go
 *    through the Unicorn register API.  SR is written before A7 and the
 *    banked stack pointers because writing SR may swap stacks.
 *
 *  - VBR is not exposed by Unicorn's API.  It only matters to UAE when UAE
 *    builds an exception frame, so it is read with a "movec vbr,d0" stub
 *    right before UAE handles an exception or interrupt.
 *
 *  - The FPU is not exposed either.  It lives in Unicorn and is copied to
 *    UAE (and back) only around F-line instructions that UAE must execute.
 *    The copy uses fmovem.x/fmovem.l stubs executed by *both* engines through
 *    a small guest buffer, so no extended-precision conversion code is
 *    needed and every UAE FPU core works.
 *
 *  - TC: BasiliskII has no 68040 MMU emulation (movec to TC is ignored by
 *    UAE).  Unicorn would honour TC.E, so it is forced back to 0 whenever
 *    the guest sets it.
 *
 *  - Memory: guest pages are mapped into Unicorn lazily, the first time
 *    they are touched, as views of the host memory that UAE uses
 *    (Mac2HostAddr).  Unbacked addresses read as 0 and ignore writes.
 *
 *  - Host code writes (ROM/resource patches, disk reads, BlockMove cache
 *    flush) are reported through unicorn_cpu_invalidate() because Unicorn
 *    only detects self-modifying code written by the guest itself.
 */

#include "sysdeps.h"

#ifdef USE_UNICORN_M68K

#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "emul_op.h"
#include "m68k.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"
#include "unicorn_cpu.h"

#include <unicorn/unicorn.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#define DEBUG 0
#include "debug.h"

// QEMU m68k exception numbers (qemu/target/m68k/cpu.h)
enum {
	UC_M68K_EXCP_ACCESS		= 2,
	UC_M68K_EXCP_ILLEGAL	= 4,
};

// From newcpu.cpp / main_*.cpp
extern int quit_program;
extern void cpu_do_check_ticks(void);
extern uint8 *ScratchMem;

bool unicorn_cpu_enabled = false;

static uc_engine *uc = NULL;
static uc_hook hook_intr_handle, hook_unmapped_handle, hook_block_handle;

// Set by the interrupt hook, reset before each run
static int last_intno;

// An asynchronous stop request was consumed by an internal stub run
static bool pending_async = false;

// Stop request from another thread, polled by hook_block()
static volatile int stop_flag = 0;

// Set when hook_block() stopped the current run
static bool block_stopped = false;

// Guest address of the helper stubs (0 if unavailable)
static uint32 stub_base = 0;
enum {
	STUB_VBR_READ	= 0x00,		// movec vbr,d0
	STUB_VBR_END	= 0x04,
	STUB_FP_SAVE	= 0x10,		// fmovem.x fp0-fp7,(FPREGS).l ; fmovem.l fpcr/fpsr/fpiar,(FPCTL).l
	STUB_FP_SAVE_END= 0x20,
	STUB_FP_LOAD	= 0x22,		// fmovem.x (FPREGS).l,fp0-fp7 ; fmovem.l (FPCTL).l,fpcr/fpsr/fpiar
	STUB_FP_LOAD_END= 0x32,
	STUB_CCR_READ	= 0x34,		// move.w ccr,d0
	STUB_CCR_END	= 0x36,
	STUB_FPREGS		= 0x40,		// 8 x 12 bytes
	STUB_FPCTL		= 0xa0,		// 3 x 4 bytes
	STUB_SIZE		= 0xb0
};
static const uint16 STUB_TERMINATOR = M68K_EXEC_RETURN;	// illegal in Unicorn

// Last values exchanged with Unicorn for rarely changing control registers
static uint32 shadow_sfc, shadow_dfc, shadow_cacr;
static bool shadow_valid = false;

// Periodic stop thread, so the host tick/event code runs regularly
static pthread_t tick_thread;
static volatile bool tick_thread_active = false;

// Statistics
static uint64 stat_runs, stat_exceptions, stat_io_pages, stat_regions, stat_fpu_syncs, stat_tb_flushes, stat_uae_memory_steps;


/*
 *  Helpers
 */

static uint64 now_usec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static bool env_disabled(const char *name)
{
	const char *v = getenv(name);
	return v && v[0] == '0' && v[1] == '\0';
}

static inline uintptr host_base(void)
{
	return (uintptr)Mac2HostAddr(0);
}

static uint32 uc_read32(int regid)
{
	uint32 v = 0;
	uc_reg_read(uc, regid, &v);
	return v;
}

static void uc_write32(int regid, uint32 v)
{
	uc_reg_write(uc, regid, &v);
}


/*
 *  Host memory region lookup
 */

#if defined(__APPLE__)
static bool host_region_at(uintptr addr, uintptr *start, uintptr *end, bool *readable)
{
	mach_vm_address_t a = addr;
	mach_vm_size_t size = 0;
	vm_region_basic_info_data_64_t info;
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object = MACH_PORT_NULL;
	kern_return_t kr = mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64,
	                                  (vm_region_info_t)&info, &count, &object);
	if (kr != KERN_SUCCESS || a > addr)
		return false;
	*start = (uintptr)a;
	*end = (uintptr)(a + size);
	*readable = (info.protection & VM_PROT_READ) != 0;
	return true;
}
#elif defined(__linux__)
static bool host_region_at(uintptr addr, uintptr *start, uintptr *end, bool *readable)
{
	FILE *f = fopen("/proc/self/maps", "r");
	if (f == NULL)
		return false;
	char line[512];
	bool found = false;
	while (fgets(line, sizeof(line), f)) {
		unsigned long s, e;
		char perms[8];
		if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) != 3)
			continue;
		if (addr >= s && addr < e) {
			*start = s;
			*end = e;
			*readable = perms[0] == 'r';
			found = true;
			break;
		}
	}
	fclose(f);
	return found;
}
#else
static bool host_region_at(uintptr addr, uintptr *start, uintptr *end, bool *readable)
{
	// Conservative fallback: a single page we know nothing about
	(void)addr; (void)start; (void)end; (void)readable;
	return false;
}
#endif


/*
 *  Lazy guest memory mapping
 */

static uint64_t io_read(uc_engine *, uint64_t, unsigned, void *)
{
	return 0;
}

static void io_write(uc_engine *, uint64_t, unsigned, uint64_t, void *)
{
}

static bool map_io_page(uint32 page)
{
	// Unicorn cannot map a region that ends exactly at 4 GB
	if (page >= 0xfffff000U)
		return false;
	uc_err err = uc_mmio_map(uc, page, 0x1000, io_read, NULL, io_write, NULL);
	if (err != UC_ERR_OK)
		return false;
	if (stat_io_pages++ < 16)
		fprintf(stderr, "Unicorn: guest page %08x is not backed by host memory, reads as 0\n", page);
	return true;
}

// Map the readable host region around guest address 'addr' into Unicorn
static bool map_host_region(uint32 addr)
{
	const uintptr base = host_base();
	const uint64 guest_limit = 0x100000000ULL;
	const uintptr host = base + (addr & ~0xfffU);

	uintptr hs, he;
	bool readable;
	if (!host_region_at(host, &hs, &he, &readable) || !readable)
		return false;

	// Merge adjacent readable host regions
	for (;;) {
		uintptr ps, pe;
		bool pr;
		if (hs <= base || !host_region_at(hs - 1, &ps, &pe, &pr) || !pr || pe != hs)
			break;
		hs = ps;
	}
	for (;;) {
		uintptr ns, ne;
		bool nr;
		if ((uint64)(he - base) >= guest_limit || !host_region_at(he, &ns, &ne, &nr) || !nr || ns != he)
			break;
		he = ne;
	}

	// Clip to the 32-bit guest window.  Unicorn misbehaves with regions that
	// reach the very top of the 32-bit space, so the last page is never mapped.
	uint64 lo = hs > base ? (uint64)(hs - base) : 0;
	uint64 hi = (uint64)(he - base);
	if (hi > guest_limit - 0x1000)
		hi = guest_limit - 0x1000;

	// Map at most one aligned 64 MB window per fault; huge regions are mapped
	// piecewise as they get touched
	const uint64 WINDOW = 64ULL << 20;
	const uint64 win_lo = (uint64)addr & ~(WINDOW - 1);
	if (lo < win_lo)
		lo = win_lo;
	if (hi > win_lo + WINDOW)
		hi = win_lo + WINDOW;

	// Clip around regions Unicorn already has
	uc_mem_region *regions = NULL;
	uint32_t count = 0;
	if (uc_mem_regions(uc, &regions, &count) == UC_ERR_OK) {
		for (uint32_t i = 0; i < count; i++) {
			const uint64 rb = regions[i].begin, re = regions[i].end + 1;
			if (re <= addr && re > lo)
				lo = re;
			if (rb > addr && rb < hi)
				hi = rb;
		}
		uc_free(regions);
	}
	lo &= ~0xfffULL;
	hi &= ~0xfffULL;
	if (lo > addr || hi <= addr)
		return false;

	uc_err err = uc_mem_map_ptr(uc, lo, hi - lo, UC_PROT_ALL, (void *)(base + (uintptr)lo));
	if (err != UC_ERR_OK) {
		fprintf(stderr, "Unicorn: failed to map guest %08llx-%08llx: %s\n",
		        (unsigned long long)lo, (unsigned long long)hi, uc_strerror(err));
		return false;
	}
	if (stat_regions++ < 32 || getenv("B2_UNICORN_TRACE_MAP"))
		fprintf(stderr, "Unicorn: mapped guest %08llx-%08llx\n", (unsigned long long)lo, (unsigned long long)hi);
	return true;
}

static bool hook_unmapped(uc_engine *, uc_mem_type type, uint64_t address, int size, int64_t, void *)
{
	const uint32 addr = (uint32)address;
	if (map_host_region(addr))
		return true;
	if (type == UC_MEM_FETCH_UNMAPPED) {
		fprintf(stderr, "Unicorn: instruction fetch from unbacked guest address %08x\n", addr);
		return false;
	}
	if (!map_io_page(addr & ~0xfffU))
		return false;
	// An access straddling into the next page faults again for that page
	(void)size;
	return true;
}

static void hook_intr(uc_engine *u, uint32_t intno, void *)
{
	last_intno = (int)intno;
	uc_emu_stop(u);
}

/*
 *  Stop requests (interrupts, ticks) are only acted upon at the start of a
 *  translation block.
 *
 *  Unicorn checks for pending exit requests *after* every guest memory load,
 *  when the loaded value is already in the destination register, and then
 *  rewinds the PC to the start of that instruction.  An asynchronous
 *  uc_emu_stop() would thus re-execute loads such as "movea.l (a0),a0" with
 *  a clobbered address register.  It is also not thread-safe on Apple
 *  Silicon, where every Unicorn API call updates the JIT W^X bookkeeping.
 *  A stop issued here, before the block's first instruction, is safe.
 */
static void hook_block(uc_engine *u, uint64_t, uint32_t, void *)
{
	if (stop_flag) {
		stop_flag = 0;
		block_stopped = true;
		uc_emu_stop(u);
	}
}


/*
 *  TC handling: keep Unicorn's MMU disabled like UAE's
 */

static bool clear_tc(void)
{
	uint16 tc = 0;
	uc_reg_read(uc, UC_M68K_REG_CR_TC, &tc);
	if ((tc & 0x8000) == 0)
		return false;
	tc = 0;
	uc_reg_write(uc, UC_M68K_REG_CR_TC, &tc);
	uc_ctl_flush_tlb(uc);
	return true;
}


/*
 *  Register exchange
 */

static void load_state(void)
{
	MakeSR();

	uint32 sr = regs.sr;
	uint32 vals[20];
	int ids[20];
	int n = 0;

	// SR first: writing it may swap the active stack pointer
	ids[n] = UC_M68K_REG_SR; vals[n++] = sr;
	for (int i = 0; i < 8; i++) {
		ids[n] = UC_M68K_REG_D0 + i; vals[n++] = m68k_dreg(regs, i);
	}
	for (int i = 0; i < 8; i++) {
		ids[n] = UC_M68K_REG_A0 + i; vals[n++] = m68k_areg(regs, i);
	}
	ids[n] = UC_M68K_REG_CR_USP; vals[n++] = regs.usp;
	ids[n] = UC_M68K_REG_CR_ISP; vals[n++] = regs.isp;
	ids[n] = UC_M68K_REG_CR_MSP; vals[n++] = regs.msp;

	void *ptrs[20];
	for (int i = 0; i < n; i++)
		ptrs[i] = &vals[i];
	uc_reg_write_batch(uc, ids, ptrs, n);

	if (!shadow_valid || regs.sfc != shadow_sfc) {
		uc_write32(UC_M68K_REG_CR_SFC, regs.sfc);
		shadow_sfc = regs.sfc;
	}
	if (!shadow_valid || regs.dfc != shadow_dfc) {
		uc_write32(UC_M68K_REG_CR_DFC, regs.dfc);
		shadow_dfc = regs.dfc;
	}
	if (!shadow_valid || regs.cacr != shadow_cacr) {
		uc_write32(UC_M68K_REG_CR_CACR, regs.cacr);
		shadow_cacr = regs.cacr;
	}
	shadow_valid = true;
}

static bool uc_run_stub(uint32 start, uint32 end);

/*
 *  Unicorn's register API returns a wrong CCR whenever the condition codes
 *  are still in QEMU's lazy form: reg_read(UC_M68K_REG_SR) sets cc_op to
 *  CC_OP_FLAGS *before* decoding the raw cc_* fields.  After a stop that
 *  follows e.g. CMP or ADD, X/N/Z/V/C come back as garbage, so the CCR is
 *  read by the guest itself with "move.w ccr,d0", which honours cc_op.
 *  Must run before anything reads SR through the API.  Clobbers Unicorn's D0
 *  (load_state() rewrites it before the next run).
 */
static bool read_ccr(uint32 *ccr)
{
	if (stub_base == 0)
		return false;
	if (!uc_run_stub(stub_base + STUB_CCR_READ, stub_base + STUB_CCR_END))
		return false;
	*ccr = uc_read32(UC_M68K_REG_D0) & 0x1f;
	return true;
}

static void store_state(uint32 pc)
{
	// Correct condition codes first (see read_ccr()); D0 is saved because
	// the stub overwrites it
	const uint32 saved_d0 = uc_read32(UC_M68K_REG_D0);
	uint32 ccr = 0;
	const bool ccr_ok = read_ccr(&ccr);

	int ids[23];
	uint32 vals[23];
	void *ptrs[23];
	int n = 0;

	ids[n++] = UC_M68K_REG_SR;
	for (int i = 0; i < 8; i++)
		ids[n++] = UC_M68K_REG_D0 + i;
	for (int i = 0; i < 8; i++)
		ids[n++] = UC_M68K_REG_A0 + i;
	ids[n++] = UC_M68K_REG_CR_USP;
	ids[n++] = UC_M68K_REG_CR_ISP;
	ids[n++] = UC_M68K_REG_CR_MSP;
	ids[n++] = UC_M68K_REG_CR_SFC;
	ids[n++] = UC_M68K_REG_CR_DFC;
	ids[n++] = UC_M68K_REG_CR_CACR;
	for (int i = 0; i < n; i++) {
		vals[i] = 0;
		ptrs[i] = &vals[i];
	}
	uc_reg_read_batch(uc, ids, ptrs, n);

	int k = 0;
	uint32 sr = vals[k++];
	if (ccr_ok)
		sr = (sr & ~0x1fU) | ccr;
	for (int i = 0; i < 8; i++)
		m68k_dreg(regs, i) = vals[k++];
	m68k_dreg(regs, 0) = saved_d0;
	for (int i = 0; i < 8; i++)
		m68k_areg(regs, i) = vals[k++];
	regs.usp = vals[k++];
	regs.isp = vals[k++];
	regs.msp = vals[k++];
	regs.sfc = shadow_sfc = vals[k++];
	regs.dfc = shadow_dfc = vals[k++];
	regs.cacr = shadow_cacr = vals[k++];

	// Unicorn already banked the stack pointers for the new SR, so tell
	// MakeFromSR() that the mode did not change (no stack swap)
	regs.sr = (uae_u16)sr;
	regs.s = (sr >> 13) & 1;
	regs.m = (sr >> 12) & 1;
	MakeFromSR();

	m68k_setpc(pc);
	fill_prefetch_0();
}


/*
 *  Helper stubs
 */

// Run a Unicorn stub from 'start' until its terminator at 'end'
static bool uc_run_stub(uint32 start, uint32 end)
{
	uint32 pc = start;
	for (int tries = 0; tries < 64; tries++) {
		last_intno = -1;
		uc_err err = uc_emu_start(uc, pc, 0, 0, 0);
		uint32 npc = uc_read32(UC_M68K_REG_PC);
		if (err != UC_ERR_OK) {
			fprintf(stderr, "Unicorn: stub at %08x failed: %s\n", start, uc_strerror(err));
			return false;
		}
		if (last_intno == UC_M68K_EXCP_ILLEGAL && npc == end)
			return true;
		if (last_intno >= 0) {
			fprintf(stderr, "Unicorn: stub at %08x raised exception %d at %08x\n", start, last_intno, npc);
			return false;
		}
		// Interrupted by an asynchronous stop request: continue the stub,
		// and make sure the request is honoured afterwards
		pending_async = true;
		pc = npc;
	}
	return false;
}

// Run a UAE stub from 'start' until its terminator at 'end'
static void uae_run_stub(uint32 start, uint32 end)
{
	const uaecptr saved_pc = m68k_getpc();
	m68k_setpc(start);
	fill_prefetch_0();
	for (int i = 0; i < 8 && m68k_getpc() != end; i++) {
		const uae_u32 opcode = get_word(m68k_getpc());
		(*cpufunctbl[opcode])(opcode);
	}
	m68k_setpc(saved_pc);
	fill_prefetch_0();
}

// Copy VBR from Unicorn to UAE (needed before UAE builds an exception frame)
static void sync_vbr_from_unicorn(void)
{
	if (stub_base == 0)
		return;
	const uint32 d0 = uc_read32(UC_M68K_REG_D0);
	const uint32 sr = uc_read32(UC_M68K_REG_SR);
	if ((sr & 0x2000) == 0)
		uc_write32(UC_M68K_REG_SR, sr | 0x2000);
	if (uc_run_stub(stub_base + STUB_VBR_READ, stub_base + STUB_VBR_END))
		regs.vbr = uc_read32(UC_M68K_REG_D0);
	if ((sr & 0x2000) == 0)
		uc_write32(UC_M68K_REG_SR, sr);
	uc_write32(UC_M68K_REG_D0, d0);
}

static inline bool fpu_sync_possible(void)
{
	return stub_base != 0 && FPUType != 0;
}

static void sync_fpu_to_uae(void)
{
	stat_fpu_syncs++;
	if (uc_run_stub(stub_base + STUB_FP_SAVE, stub_base + STUB_FP_SAVE_END))
		uae_run_stub(stub_base + STUB_FP_LOAD, stub_base + STUB_FP_LOAD_END);
}

static void sync_fpu_to_unicorn(void)
{
	uae_run_stub(stub_base + STUB_FP_SAVE, stub_base + STUB_FP_SAVE_END);
	uc_run_stub(stub_base + STUB_FP_LOAD, stub_base + STUB_FP_LOAD_END);
}

static void install_stubs(void)
{
	if (ScratchMem == NULL)
		return;

	// ScratchMem points to the middle of a 64 KB block that ROM patches use
	// as a dummy hardware base (positive offsets); take its lowest bytes.
	const uint32 s = Host2MacAddr(ScratchMem) - 0x8000;
	const uint32 fpregs = s + STUB_FPREGS, fpctl = s + STUB_FPCTL;
	const uint16 code[] = {
		// STUB_VBR_READ
		0x4e7a, 0x0801,							// movec vbr,d0
		STUB_TERMINATOR,
		0, 0, 0, 0, 0,
		// STUB_FP_SAVE (0x10)
		0xf239, 0xf0ff, (uint16)(fpregs >> 16), (uint16)fpregs,	// fmovem.x fp0-fp7,(fpregs).l
		0xf239, 0xbc00, (uint16)(fpctl >> 16), (uint16)fpctl,	// fmovem.l fpcr/fpsr/fpiar,(fpctl).l
		STUB_TERMINATOR,
		// STUB_FP_LOAD (0x22)
		0xf239, 0xd0ff, (uint16)(fpregs >> 16), (uint16)fpregs,	// fmovem.x (fpregs).l,fp0-fp7
		0xf239, 0x9c00, (uint16)(fpctl >> 16), (uint16)fpctl,	// fmovem.l (fpctl).l,fpcr/fpsr/fpiar
		STUB_TERMINATOR,
		// STUB_CCR_READ (0x34)
		0x42c0,									// move.w ccr,d0
		STUB_TERMINATOR,
	};
	for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); i++)
		WriteMacInt16(s + 2 * i, code[i]);
	Mac_memset(s + STUB_FPREGS, 0, STUB_SIZE - STUB_FPREGS);
	stub_base = s;
}


/*
 *  Periodic stop thread
 */

static void *tick_thread_func(void *)
{
	while (tick_thread_active) {
		struct timespec ts = { 0, 4 * 1000 * 1000 };	// 4 ms
		nanosleep(&ts, NULL);
		unicorn_cpu_request_stop();
	}
	return NULL;
}

// PC histogram of asynchronous stops (B2_UNICORN_STATS=1)
static const int PC_HIST_SIZE = 64;
static uint32 pc_hist_pc[PC_HIST_SIZE];
static uint32 pc_hist_count[PC_HIST_SIZE];

static void pc_hist_record(uint32 pc)
{
	pc &= ~0xfU;
	int free_slot = -1;
	for (int i = 0; i < PC_HIST_SIZE; i++) {
		if (pc_hist_count[i] && pc_hist_pc[i] == pc) {
			pc_hist_count[i]++;
			return;
		}
		if (!pc_hist_count[i] && free_slot < 0)
			free_slot = i;
	}
	if (free_slot >= 0) {
		pc_hist_pc[free_slot] = pc;
		pc_hist_count[free_slot] = 1;
	}
}

// Trapping opcode histogram (B2_UNICORN_STATS=1)
static uint32 op_hist_op[PC_HIST_SIZE], op_hist_pc[PC_HIST_SIZE], op_hist_count[PC_HIST_SIZE];

static void op_hist_record(uint32 opcode, uint32 pc)
{
	int free_slot = -1;
	for (int i = 0; i < PC_HIST_SIZE; i++) {
		if (op_hist_count[i] && op_hist_op[i] == opcode) {
			op_hist_count[i]++;
			op_hist_pc[i] = pc;
			return;
		}
		if (!op_hist_count[i] && free_slot < 0)
			free_slot = i;
	}
	if (free_slot >= 0) {
		op_hist_op[free_slot] = opcode;
		op_hist_pc[free_slot] = pc;
		op_hist_count[free_slot] = 1;
	}
}

static void dump_code(uint32 addr, uint32 len)
{
	fprintf(stderr, "UNICORN_CODE %08x:", addr);
	for (uint32 i = 0; i < len; i += 2)
		fprintf(stderr, " %04x", (unsigned)ReadMacInt16(addr + i));
	fprintf(stderr, "\n");
}

static void pc_hist_dump(void)
{
	{
		int top = -1;
		for (int i = 0; i < PC_HIST_SIZE; i++)
			if (pc_hist_count[i] && (top < 0 || pc_hist_count[i] > pc_hist_count[top]))
				top = i;
		if (top >= 0) {
			const uint32 a = pc_hist_pc[top] >= 0x60 ? pc_hist_pc[top] - 0x60 : 0;
			dump_code(a, 0xc0);
		}
	}
	fprintf(stderr, "UNICORN_OPHIST");
	for (int n = 0; n < 10; n++) {
		int best = -1;
		for (int i = 0; i < PC_HIST_SIZE; i++)
			if (op_hist_count[i] && (best < 0 || op_hist_count[i] > op_hist_count[best]))
				best = i;
		if (best < 0)
			break;
		fprintf(stderr, " %04x@%08x:%u", op_hist_op[best], op_hist_pc[best], op_hist_count[best]);
		op_hist_count[best] = 0;
	}
	fprintf(stderr, "\n");
	memset(op_hist_count, 0, sizeof(op_hist_count));

	fprintf(stderr, "UNICORN_PCHIST");
	for (int n = 0; n < 8; n++) {
		int best = -1;
		for (int i = 0; i < PC_HIST_SIZE; i++)
			if (pc_hist_count[i] && (best < 0 || pc_hist_count[i] > pc_hist_count[best]))
				best = i;
		if (best < 0)
			break;
		fprintf(stderr, " %08x:%u", pc_hist_pc[best], pc_hist_count[best]);
		pc_hist_count[best] = 0;
	}
	fprintf(stderr, "\n");
	memset(pc_hist_count, 0, sizeof(pc_hist_count));
}

static void report_status(uint64 now)
{
	static int enabled = -1;
	static uint64 last = 0;
	if (enabled < 0) {
		const char *e = getenv("B2_UNICORN_STATS");
		enabled = e && *e && strcmp(e, "0") != 0;
	}
	if (!enabled || now - last < 2000000)
		return;
	last = now;
	MakeSR();
	fprintf(stderr, "UNICORN_STATS runs=%llu exc=%llu pc=%08x sr=%04x a7=%08x vbr=%08x spc=%08x irq=%08x ticks=%08x regions=%llu io=%llu\n",
	        (unsigned long long)stat_runs, (unsigned long long)stat_exceptions,
	        (unsigned)m68k_getpc(), (unsigned)regs.sr, (unsigned)m68k_areg(regs, 7), (unsigned)regs.vbr,
	        (unsigned)regs.spcflags, (unsigned)InterruptFlags, (unsigned)ReadMacInt32(0x16a),
	        (unsigned long long)stat_regions, (unsigned long long)stat_io_pages);
	pc_hist_dump();
}

static void check_ticks(void)
{
	static uint64 last = 0;
	const uint64 now = now_usec();
	if (now - last >= 1000) {
		last = now;
		cpu_do_check_ticks();
		report_status(now);
	}
}


/*
 *  Public interface
 */

bool unicorn_cpu_init(void)
{
#if !(REAL_ADDRESSING || DIRECT_ADDRESSING)
	fprintf(stderr, "Unicorn: needs real or direct addressing, using the UAE core\n");
	return false;
#else
	if (env_disabled("B2_USE_UNICORN")) {
		fprintf(stderr, "Unicorn: disabled by B2_USE_UNICORN=0, using the UAE core\n");
		return false;
	}

	uc_err err = uc_open(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, &uc);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "Unicorn: uc_open failed: %s, using the UAE core\n", uc_strerror(err));
		uc = NULL;
		return false;
	}

	int model;
	const char *model_name;
	switch (CPUType) {
	case 0: case 1: model = UC_CPU_M68K_M68000; model_name = "68000"; break;
	case 2:         model = UC_CPU_M68K_M68020; model_name = "68020"; break;
	case 3:         model = UC_CPU_M68K_M68030; model_name = "68030"; break;
	default:        model = UC_CPU_M68K_M68040; model_name = "68040"; break;
	}
	err = uc_ctl_set_cpu_model(uc, model);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "Unicorn: cannot select the %s model: %s, using the UAE core\n", model_name, uc_strerror(err));
		uc_close(uc);
		uc = NULL;
		return false;
	}

	err = uc_hook_add(uc, &hook_intr_handle, UC_HOOK_INTR, (void *)hook_intr, NULL, 1, 0);
	if (err == UC_ERR_OK)
		err = uc_hook_add(uc, &hook_unmapped_handle, UC_HOOK_MEM_UNMAPPED, (void *)hook_unmapped, NULL, 1, 0);
	if (err == UC_ERR_OK)
		err = uc_hook_add(uc, &hook_block_handle, UC_HOOK_BLOCK, (void *)hook_block, NULL, 1, 0);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "Unicorn: cannot install hooks: %s, using the UAE core\n", uc_strerror(err));
		uc_close(uc);
		uc = NULL;
		return false;
	}

	install_stubs();
	if (stub_base == 0)
		fprintf(stderr, "Unicorn: no scratch memory for helper stubs; VBR/FPU sync disabled\n");

	shadow_valid = false;
	pending_async = false;
	unicorn_cpu_enabled = true;

	tick_thread_active = true;
	if (pthread_create(&tick_thread, NULL, tick_thread_func, NULL) != 0)
		tick_thread_active = false;

	unsigned int uc_major = 0, uc_minor = 0;
	uc_version(&uc_major, &uc_minor);
	fprintf(stderr, "Unicorn: executing 68k code with Unicorn %u.%u (%s model)\n", uc_major, uc_minor, model_name);
	return true;
#endif
}

void unicorn_cpu_exit(void)
{
	if (!unicorn_cpu_enabled)
		return;
	if (tick_thread_active) {
		tick_thread_active = false;
		pthread_join(tick_thread, NULL);
	}
	fprintf(stderr, "Unicorn: %llu runs, %llu exceptions, %llu TB flushes, %llu UAE memory steps, %llu FPU syncs, %llu mapped regions, %llu I/O pages\n",
	        (unsigned long long)stat_runs, (unsigned long long)stat_exceptions, (unsigned long long)stat_tb_flushes,
	        (unsigned long long)stat_uae_memory_steps,
	        (unsigned long long)stat_fpu_syncs, (unsigned long long)stat_regions,
	        (unsigned long long)stat_io_pages);
	unicorn_cpu_enabled = false;
	uc_close(uc);
	uc = NULL;
}

void unicorn_cpu_request_stop(void)
{
	// Acted upon by hook_block() on the CPU thread, see there
	__atomic_store_n(&stop_flag, 1, __ATOMIC_RELEASE);
}

/*
 *  Drop translated code in guest range [start, end).
 *
 *  uc_ctl_remove_cache() translates only the start address and assumes the
 *  range is contiguous host RAM, so it is applied per mapped region.  It is
 *  used instead of uc_ctl_flush_tb(): flushing the whole TCG buffer while
 *  chained TBs survive made the host jump into stale code buffer pages.
 */
static void invalidate_range(uint64 start, uint64 end)
{
	uc_mem_region *regions = NULL;
	uint32_t count = 0;
	if (uc_mem_regions(uc, &regions, &count) != UC_ERR_OK)
		return;
	for (uint32_t i = 0; i < count; i++) {
		const uint64 rb = regions[i].begin, re = regions[i].end + 1;
		const uint64 lo = start > rb ? start : rb;
		const uint64 hi = end < re ? end : re;
		if (lo < hi)
			uc_ctl_remove_cache(uc, lo, hi);
	}
	uc_free(regions);
}

void unicorn_cpu_invalidate(uint32 mac_addr, uint32 size)
{
	if (uc == NULL || size == 0)
		return;
	invalidate_range(mac_addr, (uint64)mac_addr + size);
}

void unicorn_cpu_invalidate_all(void)
{
	if (uc == NULL)
		return;
	static int full_flush = -1;
	if (full_flush < 0) {
		const char *e = getenv("B2_UNICORN_FULL_FLUSH");
		full_flush = e && *e && strcmp(e, "0") != 0;
	}
	if (full_flush)
		uc_ctl_flush_tb(uc);
	else
		invalidate_range(0, 0x100000000ULL);
}

/*
 *  Host EmulOp handlers write guest memory directly (driver installation,
 *  boot patches, procedures built for Execute68k, ...), which Unicorn cannot
 *  see.  After an EmulOp all translated code is dropped, except for frequent
 *  EmulOps known not to write 68k code.  Disk/CD/ExtFS reads are covered
 *  precisely by HostWroteMacMemory(), BlockMove by FlushCodeCache().
 */
static bool emulop_may_write_code(uint32 opcode)
{
	// B2_UNICORN_FLUSH_OPS=7100-71ff (debugging): always flush after these
	static uint32 force_lo = 1, force_hi = 0;
	static bool force_init = false;
	if (!force_init) {
		force_init = true;
		const char *r = getenv("B2_UNICORN_FLUSH_OPS");
		if (r && *r) {
			char *end = NULL;
			force_lo = strtoul(r, &end, 16);
			force_hi = (end && *end == '-') ? strtoul(end + 1, NULL, 16) : force_lo;
		}
	}
	if (opcode >= force_lo && opcode <= force_hi)
		return true;
	switch (opcode) {
	case M68K_EXEC_RETURN:
	case M68K_EMUL_OP_IRQ:
	case M68K_EMUL_OP_IDLE_TIME:
	case M68K_EMUL_OP_MICROSECONDS:
	case M68K_EMUL_OP_INSTIME:
	case M68K_EMUL_OP_RMVTIME:
	case M68K_EMUL_OP_PRIMETIME:
	case M68K_EMUL_OP_ADBOP:
	case M68K_EMUL_OP_SONY_PRIME:
	case M68K_EMUL_OP_SONY_STATUS:
	case M68K_EMUL_OP_DISK_PRIME:
	case M68K_EMUL_OP_DISK_STATUS:
	case M68K_EMUL_OP_CDROM_PRIME:
	case M68K_EMUL_OP_CDROM_STATUS:
	case M68K_EMUL_OP_VIDEO_STATUS:
	case M68K_EMUL_OP_SERIAL_PRIME:
	case M68K_EMUL_OP_SERIAL_STATUS:
	case M68K_EMUL_OP_ETHER_READ_PACKET:
	case M68K_EMUL_OP_AUDIO:
	case M68K_EMUL_OP_EXTFS_COMM:
	case M68K_EMUL_OP_EXTFS_HFS:
	case M68K_EMUL_OP_BLOCK_MOVE:
	case M68K_EMUL_OP_SOUNDIN_PRIME:
	case M68K_EMUL_OP_SOUNDIN_STATUS:
	case M68K_EMUL_OP_DEBUGUTIL:
		return false;
	default:
		return true;
	}
}

// Execute one instruction at the current PC with the UAE interpreter
static void uae_step(void)
{
	const uaecptr pc = m68k_getpc();
	regs.fault_pc = pc;
	const uae_u32 opcode = get_word(pc);
	(*cpufunctbl[opcode])(opcode);
}

static void fatal_error(const char *what, uint32 pc, uc_err err)
{
	fprintf(stderr, "Unicorn: %s at %08x: %s\n", what, pc, uc_strerror(err));
	m68k_dumpstate(stderr, NULL);
	QuitEmulator();
}

void unicorn_cpu_execute(void)
{
	for (;;) {
		SPCFLAGS_CLEAR(SPCFLAG_JIT_END_COMPILE | SPCFLAG_JIT_EXEC_RETURN);

		// Pending interrupt or stop request: let the host handle it first
		const bool host_work = pending_async ||
			SPCFLAGS_TEST(SPCFLAG_INT | SPCFLAG_DOINT | SPCFLAG_BRK | SPCFLAG_STOP);
		pending_async = false;

		if (!host_work) {
			load_state();

			uint32 pc = m68k_getpc();
			int intno;
			uc_err err;
			uint32 npc;
			for (;;) {
				last_intno = -1;
				block_stopped = false;
				stat_runs++;
				err = uc_emu_start(uc, pc, 0, 0, 0);
				npc = uc_read32(UC_M68K_REG_PC);
				intno = last_intno;
				// The guest turned the MMU on: undo it and retry
				if (clear_tc() && (err == UC_ERR_OK && intno == UC_M68K_EXCP_ACCESS)) {
					pc = npc;
					continue;
				}
				break;
			}

			store_state(npc);

			if (err != UC_ERR_OK) {
				// Memory Unicorn cannot reach (e.g. the last page below 4 GB,
				// which it cannot map) is still host memory for UAE: let UAE
				// execute the faulting instruction
				const bool memory_error =
					err == UC_ERR_READ_UNMAPPED || err == UC_ERR_WRITE_UNMAPPED ||
					err == UC_ERR_READ_PROT || err == UC_ERR_WRITE_PROT;
				if (!memory_error) {
					fatal_error("emulation error", npc, err);
					return;
				}
				stat_uae_memory_steps++;
				sync_vbr_from_unicorn();
				uae_step();
			} else if (intno >= 0) {
				// Exception: npc is the faulting instruction. UAE executes it,
				// which runs EmulOps and builds genuine exception frames.
				stat_exceptions++;
				const uae_u32 opcode = get_word(npc);
				if (getenv("B2_UNICORN_STATS"))
					op_hist_record(opcode, npc);
				const bool is_emulop = (opcode & 0xff00) == 0x7100;
				const bool is_fpu = (opcode & 0xfe00) == 0xf200;
				if (!is_emulop)
					sync_vbr_from_unicorn();
				if (is_fpu && fpu_sync_possible()) {
					sync_fpu_to_uae();
					uae_step();
					sync_fpu_to_unicorn();
				} else
					uae_step();
				// B2_UNICORN_FLUSH_AFTER_STEP=1 (debugging): drop all
				// translations after every instruction UAE executes
				static int flush_after_step = -1;
				if (flush_after_step < 0) {
					const char *e = getenv("B2_UNICORN_FLUSH_AFTER_STEP");
					flush_after_step = e && *e && strcmp(e, "0") != 0;
				}
				if (flush_after_step || (is_emulop && emulop_may_write_code(opcode))) {
					stat_tb_flushes++;
					unicorn_cpu_invalidate_all();
				}
			} else if (!block_stopped && npc >= 4 && get_word(npc - 4) == 0x4e72) {
				// Unicorn returned on its own after STOP #imm: let UAE run the
				// STOP so its idle/interrupt logic takes over
				m68k_setpc(npc - 4);
				fill_prefetch_0();
				uae_step();
			}
			else if (getenv("B2_UNICORN_STATS"))
				pc_hist_record(npc);	// asynchronous stop request
		}

		check_ticks();

		if (SPCFLAGS_TEST(SPCFLAG_ALL_BUT_EXEC_RETURN)) {
			if (SPCFLAGS_TEST(SPCFLAG_INT | SPCFLAG_DOINT | SPCFLAG_STOP))
				sync_vbr_from_unicorn();
			if (m68k_do_specialties())
				return;
			// SPCFLAG_INT is promoted to SPCFLAG_DOINT on one pass and
			// delivered on the next; don't wait for another stop
			if (SPCFLAGS_TEST(SPCFLAG_DOINT) && m68k_do_specialties())
				return;
		}
		if (quit_program > 0)
			return;
	}
}

#endif /* USE_UNICORN_M68K */
