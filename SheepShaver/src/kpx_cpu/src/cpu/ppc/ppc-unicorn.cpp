/*
 *  ppc-unicorn.cpp - PowerPC execution backend using the Unicorn engine
 *
 *  Kheperix (C) 2003-2005 Gwenole Beauchesne
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
#include "cpu/vm.hpp"
#include "cpu/ppc/ppc-registers.hpp"
#include "cpu/ppc/ppc-unicorn.hpp"

#include <unicorn/unicorn.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif defined(__linux__)
#include <inttypes.h>
#else
#include <sys/mman.h>
#endif

// Machine State Register bits
static const uint32 MSR_VEC = 1u << 25;		// AltiVec available
static const uint32 MSR_PR  = 1u << 14;		// Problem (user) state
static const uint32 MSR_FP  = 1u << 13;		// FPU available
static const uint32 MSR_ME  = 1u << 12;		// Machine check enable

// Guest MSR: user mode, FPU and AltiVec enabled, external interrupts and
// address translation disabled (effective address == guest address)
static const uint32 GUEST_MSR = MSR_VEC | MSR_PR | MSR_FP | MSR_ME;

// Instructions used by the helper stubs
static const uint32 INSN_STOP     = 0x18000001;	// primary opcode 6: traps into run()
static const uint32 INSN_MFXER_R3 = 0x7c6102a6;	// mfxer  r3
static const uint32 INSN_MTXER_R3 = 0x7c6103a6;	// mtxer  r3
static const uint32 INSN_MTTBL_R3 = 0x7c7c43a6;	// mtspr  284,r3 (TBL write SPR)
static const uint32 INSN_MTTBU_R7 = 0x7cfd43a6;	// mtspr  285,r7 (TBU write SPR)
static const uint32 INSN_MFTB_R3  = 0x7c6c42e6;	// mftb   r3
static const uint32 INSN_MFTBU_R7 = 0x7ced42e6;	// mftbu  r7

// Helper stub layout (offsets from cfg.stub_addr)
enum {
	STUB_MFXER = 0,		// mfxer r3; stop
	STUB_MTXER = 8,		// mtxer r3; stop
	STUB_SETTB = 16,	// mtspr 284,r3; mtspr 285,r7; stop
	STUB_GETTB = 28,	// mftb r3; mftbu r7; stop
	STUB_UNTIL = 40,	// never executed: used as uc_emu_start() 'until'
	STUB_END   = 48
};

static const uint32 GUEST_PAGE_SIZE = 4096;

// Stop requests (interrupts from the 60 Hz tick thread) are only acted upon
// at the start of a translation block, from a block hook.
//
// Unicorn checks for pending exit requests *after* every guest memory load,
// when the loaded value is already in the destination register, and then
// rewinds the PC to the start of that instruction.  An asynchronous
// uc_emu_stop() would thus re-execute loads such as "lwz r3,0(r3)" or
// "lwzx r4,r3,r4" with a clobbered address register.  (It is also not
// thread-safe on Apple Silicon, where it updates the JIT write-protection
// bookkeeping.)  A stop issued by the block hook, before the first
// instruction of the block, is caught by the check that follows the hook.

// Hook trampolines with the exact signatures Unicorn expects
struct ppc_unicorn_hooks {
	static void intr(uc_engine *uc, uint32_t intno, void *user)
	{
		ppc_unicorn *self = (ppc_unicorn *)user;
		self->last_intno = (int)intno;
		uc_emu_stop(uc);
	}

	static bool unmapped(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user)
	{
		ppc_unicorn *self = (ppc_unicorn *)user;
		const uint32 a = (uint32)addr;
		if (self->map_host_region(a))
			return true;

		if (type != UC_MEM_FETCH_UNMAPPED) {
			uint32 pc = 0;
			uc_reg_read(uc, UC_PPC_REG_PC, &pc);
			const bool is_write = (type == UC_MEM_WRITE_UNMAPPED);
			if (self->cfg.ignore_fault && self->cfg.ignore_fault(pc, a, is_write)) {
				if (self->map_io_page(a))
					return true;
			}
		}
		self->last_fault_addr = a;

		// The failed load still writes its destination register before
		// Unicorn stops (see the comment on stop requests above), so
		// remember it for the interpreter retry.  PC is synced here.
		if (type == UC_MEM_READ_UNMAPPED) {
			uint32 pc = 0;
			uc_reg_read(uc, UC_PPC_REG_PC, &pc);
			const uint32 opcode = vm_read_memory_4(pc);
			const uint32 op = opcode >> 26, xo = (opcode >> 1) & 0x3ff;
			const bool int_load = (op >= 32 && op <= 35) || (op >= 40 && op <= 43) ||
				(op == 31 && (xo == 20 || xo == 23 || xo == 55 || xo == 87 || xo == 119 || xo == 279 ||
							  xo == 311 || xo == 343 || xo == 375 || xo == 534 || xo == 790));
			if (int_load) {
				self->fault_gpr = (opcode >> 21) & 31;
				uc_reg_read(uc, UC_PPC_REG_0 + self->fault_gpr, &self->fault_gpr_val);
			}
		}
		return false;
	}

	static bool write_prot(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *user)
	{
		ppc_unicorn *self = (ppc_unicorn *)user;
		self->last_fault_addr = (uint32)addr;
		return false;	// stop: run() reports EXIT_WRITE_PROTECT
	}

	static void block(uc_engine *uc, uint64_t address, uint32_t size, void *user)
	{
		ppc_unicorn *self = (ppc_unicorn *)user;
		if (self->stop_flag) {
			self->stop_flag = 0;
			uc_emu_stop(uc);
		}
	}

	static uint64_t io_read(uc_engine *uc, uint64_t offset, unsigned size, void *user)
	{
		return 0;
	}

	static void io_write(uc_engine *uc, uint64_t offset, unsigned size, uint64_t value, void *user)
	{
	}
};

static int pvr_to_model(uint32 pvr)
{
	switch (pvr >> 16) {
	case 0x0001: return UC_CPU_PPC32_601_V2;
	case 0x0003: return UC_CPU_PPC32_603;
	case 0x0006: return UC_CPU_PPC32_603E_V4_1;
	case 0x0007: return UC_CPU_PPC32_603E7V2;
	case 0x0004: return UC_CPU_PPC32_604;
	case 0x0009: return UC_CPU_PPC32_604E_V2_4;
	case 0x000a: return UC_CPU_PPC32_604R;
	case 0x0008: return UC_CPU_PPC32_750_V3_1;
	case 0x7000: return UC_CPU_PPC32_750FX_V2_3;
	case 0x7002: return UC_CPU_PPC32_750GX_V1_2;
	case 0x000c: return UC_CPU_PPC32_7400_V2_9;
	case 0x800c: return UC_CPU_PPC32_7410_V1_4;
	case 0x8000: return UC_CPU_PPC32_7450_V2_1;
	case 0x8001: return UC_CPU_PPC32_7455_V3_4;
	case 0x8002: return UC_CPU_PPC32_7457_V1_2;
	case 0x8003: return UC_CPU_PPC32_7447A_V1_2;
	case 0x8004: return UC_CPU_PPC32_7448_V2_1;
	}
	return UC_CPU_PPC32_7400_V2_9;
}

static uint32 env_u32(const char *name, uint32 def)
{
	const char *s = getenv(name);
	if (s == NULL || *s == '\0')
		return def;
	char *end = NULL;
	unsigned long v = strtoul(s, &end, 0);
	return (end && *end == '\0') ? (uint32)v : def;
}

ppc_unicorn::ppc_unicorn()
	: uc(NULL), last_intno(-1), last_fault_addr(0), pending_async(false), stop_flag(0),
	  fault_gpr(-1), fault_gpr_val(0),
	  shadow_valid(false), tb_ctx(NULL), irq_depth(0), tb_offset_tbl(-1), tb_last_refresh(0),
	  tb_refresh_usec(0), stat_runs(0), stat_exceptions(0), stat_io_pages(0),
	  stat_mapped_regions(0), history_pos(0)
{
	memset(&cfg, 0, sizeof(cfg));
	memset(history, 0, sizeof(history));
	memset(irq_ctx, 0, sizeof(irq_ctx));
}

ppc_unicorn::~ppc_unicorn()
{
	if (getenv("SS_UNICORN_STATS")) {
		fprintf(stderr, "Unicorn PPC: %llu runs, %llu trapped instructions, "
				"%llu mapped regions, %llu I/O pages\n",
				(unsigned long long)stat_runs, (unsigned long long)stat_exceptions,
				(unsigned long long)stat_mapped_regions, (unsigned long long)stat_io_pages);
	}
	if (tb_ctx)
		uc_context_free(tb_ctx);
	for (int i = 0; i < 4; i++)
		if (irq_ctx[i])
			uc_context_free(irq_ctx[i]);
	if (uc)
		uc_close(uc);
}

const char *ppc_unicorn::error_string(int err)
{
	return uc_strerror((uc_err)err);
}

ppc_unicorn *ppc_unicorn::create(const ppc_unicorn_config &config)
{
	ppc_unicorn *u = new ppc_unicorn();
	if (!u->init(config)) {
		delete u;
		return NULL;
	}
	return u;
}

bool ppc_unicorn::init(const ppc_unicorn_config &config)
{
	cfg = config;

	// Unicorn maps guest memory in 4 KB pages: widen read-only ranges to that
	for (int i = 0; i < cfg.n_ro_ranges; i++) {
		uint64 start = cfg.ro_ranges[i].start & ~(uint64)(GUEST_PAGE_SIZE - 1);
		uint64 end = ((uint64)cfg.ro_ranges[i].start + cfg.ro_ranges[i].size + GUEST_PAGE_SIZE - 1)
			& ~(uint64)(GUEST_PAGE_SIZE - 1);
		if (end > 0x100000000ULL)
			end = 0x100000000ULL;
		cfg.ro_ranges[i].start = (uint32)start;
		cfg.ro_ranges[i].size = (uint32)(end - start);
	}

	if (cfg.stub_addr == 0 || (cfg.stub_addr & 3) != 0) {
		fprintf(stderr, "Unicorn PPC: invalid stub address %08x\n", cfg.stub_addr);
		return false;
	}

	uc_err err = uc_open(UC_ARCH_PPC, (uc_mode)(UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN), &uc);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "Unicorn PPC: uc_open failed: %s\n", uc_strerror(err));
		uc = NULL;
		return false;
	}

	const int model = pvr_to_model(cfg.pvr);
	err = uc_ctl_set_cpu_model(uc, model);
	if (err != UC_ERR_OK) {
		fprintf(stderr, "Unicorn PPC: cannot select CPU model %d: %s\n", model, uc_strerror(err));
		return false;
	}

	uc_hook h;
	if ((err = uc_hook_add(uc, &h, UC_HOOK_INTR, (void *)ppc_unicorn_hooks::intr, this, 1, 0)) != UC_ERR_OK ||
		(err = uc_hook_add(uc, &h, UC_HOOK_MEM_UNMAPPED, (void *)ppc_unicorn_hooks::unmapped, this, 1, 0)) != UC_ERR_OK ||
		(err = uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE_PROT, (void *)ppc_unicorn_hooks::write_prot, this, 1, 0)) != UC_ERR_OK) {
		fprintf(stderr, "Unicorn PPC: uc_hook_add failed: %s\n", uc_strerror(err));
		return false;
	}

	// Interrupt stop requests are polled at block boundaries
	if ((err = uc_hook_add(uc, &h, UC_HOOK_BLOCK, (void *)ppc_unicorn_hooks::block, this, 1, 0)) != UC_ERR_OK) {
		fprintf(stderr, "Unicorn PPC: uc_hook_add(BLOCK) failed: %s\n", uc_strerror(err));
		return false;
	}

	// Install helper stubs in guest memory
	static const uint32 stub[STUB_END / 4] = {
		INSN_MFXER_R3, INSN_STOP,						// STUB_MFXER
		INSN_MTXER_R3, INSN_STOP,						// STUB_MTXER
		INSN_MTTBL_R3, INSN_MTTBU_R7, INSN_STOP,		// STUB_SETTB
		INSN_MFTB_R3, INSN_MFTBU_R7, INSN_STOP,			// STUB_GETTB
		INSN_STOP, INSN_STOP							// STUB_UNTIL (never reached)
	};
	for (uint32 i = 0; i < STUB_END / 4; i++)
		vm_write_memory_4(cfg.stub_addr + i * 4, stub[i]);
	invalidate_range(cfg.stub_addr, cfg.stub_addr + STUB_END);

	uint32 msr = GUEST_MSR;
	uc_reg_write(uc, UC_PPC_REG_MSR, &msr);

	tb_refresh_usec = env_u32("SS_UNICORN_TB_USEC", 250);
	if (getenv("SS_UNICORN_NOTB"))
		fprintf(stderr, "Unicorn PPC: timebase emulation disabled (SS_UNICORN_NOTB)\n");
	else if (cfg.ticks_usec == NULL || cfg.timebase_hz == 0 || !locate_timebase())
		fprintf(stderr, "Unicorn PPC: warning: timebase emulation unavailable, mftb will not advance\n");

	uc_reg_write(uc, UC_PPC_REG_MSR, &msr);
	shadow_valid = false;

	unsigned int major = 0, minor = 0;
	uc_version(&major, &minor);
	printf("Unicorn PPC engine %u.%u enabled (model %d, PVR %08x)\n", major, minor, model, cfg.pvr);
	return true;
}

/*
 *  Run a helper stub until its terminating stop instruction.  An
 *  asynchronous stop request that lands while a stub runs is remembered
 *  and delivered by the next run().
 */

bool ppc_unicorn::run_stub(uint32 addr)
{
	uint32 pc = addr;
	for (int tries = 0; tries < 64; tries++) {
		last_intno = -1;
		uc_err err = uc_emu_start(uc, pc, cfg.stub_addr + STUB_UNTIL, 0, 0);
		if (err != UC_ERR_OK)
			return false;
		uint32 npc = 0;
		uc_reg_read(uc, UC_PPC_REG_PC, &npc);
		if (last_intno >= 0)
			return vm_read_memory_4(npc - 4) == INSN_STOP && (npc - 4 - cfg.stub_addr) < STUB_END;
		pending_async = true;
		pc = npc;
	}
	return false;
}

/*
 *  Timebase emulation
 *
 *  Unicorn does not attach a clock to the PowerPC timebase: mftb reads a
 *  plain SPR slot that the guest cannot write (TBL/TBU are read via SPRs
 *  268/269, written via SPRs 284/285).  We locate the read slots inside
 *  Unicorn's saved CPU context by planting a marker in the adjacent write
 *  SPRs, verify the result with mftb, and then periodically patch the
 *  host-derived timebase into the context.
 */

bool ppc_unicorn::locate_timebase()
{
	static const uint32 MARK_LO = 0x5ee9f00d;
	static const uint32 MARK_HI = 0xc0de7b01;

	// mtspr 284/285 are supervisor-only
	uint32 msr = GUEST_MSR & ~MSR_PR;
	uc_reg_write(uc, UC_PPC_REG_MSR, &msr);

	uint32 v = MARK_LO;
	uc_reg_write(uc, UC_PPC_REG_3, &v);
	v = MARK_HI;
	uc_reg_write(uc, UC_PPC_REG_7, &v);
	if (!run_stub(cfg.stub_addr + STUB_SETTB))
		return false;

	if (uc_context_alloc(uc, &tb_ctx) != UC_ERR_OK) {
		tb_ctx = NULL;
		return false;
	}
	if (uc_context_save(uc, tb_ctx) != UC_ERR_OK)
		return false;

	// The context blob holds a raw copy of the CPU state; the SPR array
	// stores 32-bit target_ulong values in host byte order
	const uint8 *blob = (const uint8 *)tb_ctx;
	const size_t size = uc_context_size(uc);
	long found = -1;
	for (size_t i = 0; i + 8 <= size; i += 4) {
		uint32 lo, hi;
		memcpy(&lo, blob + i, 4);
		memcpy(&hi, blob + i + 4, 4);
		if (lo == MARK_LO && hi == MARK_HI) {
			if (found >= 0)
				return false;	// ambiguous
			found = (long)i;
		}
	}
	// SPR 268 (TBL read) is 16 slots before SPR 284 (TBL write)
	if (found < 16 * 4)
		return false;
	tb_offset_tbl = found - 16 * 4;

	// Verify by reading back a known value with mftb/mftbu
	const uint64 probe = 0x0123456789abcdefULL;
	uint32 lo = (uint32)probe, hi = (uint32)(probe >> 32);
	memcpy((uint8 *)tb_ctx + tb_offset_tbl, &lo, 4);
	memcpy((uint8 *)tb_ctx + tb_offset_tbl + 4, &hi, 4);
	uc_context_restore(uc, tb_ctx);
	uint32 r3 = 0, r7 = 0;
	if (!run_stub(cfg.stub_addr + STUB_GETTB) ||
		uc_reg_read(uc, UC_PPC_REG_3, &r3) != UC_ERR_OK ||
		uc_reg_read(uc, UC_PPC_REG_7, &r7) != UC_ERR_OK ||
		r3 != lo || r7 != hi) {
		tb_offset_tbl = -1;
		return false;
	}

	tb_last_refresh = 0;
	refresh_timebase();
	return true;
}

void ppc_unicorn::refresh_timebase()
{
	if (tb_offset_tbl < 0)
		return;
	const uint64 now = cfg.ticks_usec();
	if (tb_last_refresh != 0 && now - tb_last_refresh < tb_refresh_usec)
		return;
	tb_last_refresh = now;

	// now * hz / 1e6 without 64-bit overflow
	const uint64 hz = cfg.timebase_hz;
	const uint64 tb = (now / 1000000) * hz + ((now % 1000000) * hz) / 1000000;

	if (uc_context_save(uc, tb_ctx) != UC_ERR_OK)
		return;
	uint32 lo = (uint32)tb, hi = (uint32)(tb >> 32);
	memcpy((uint8 *)tb_ctx + tb_offset_tbl, &lo, 4);
	memcpy((uint8 *)tb_ctx + tb_offset_tbl + 4, &hi, 4);
	uc_context_restore(uc, tb_ctx);
}

/*
 *  Register state transfer
 *
 *  Unicorn's XER register accessor ignores the separately tracked SO/OV/CA
 *  bits, so XER goes through mfxer/mtxer stubs instead.
 */

void ppc_unicorn::load_state(const powerpc_registers &r)
{
	refresh_timebase();

	int ids[32 + 32 + 5];
	const void *vals[32 + 32 + 5];
	uint32 v32[32 + 5];
	uint64 v64[32];
	int n = 0, n32 = 0;

	static const bool no_shadow = getenv("SS_UNICORN_NOSHADOW") != NULL;
	if (no_shadow)
		shadow_valid = false;

	const uint32 xer = r.xer.get();
	if (!shadow_valid || xer != shadow_xer) {
		uint32 x = xer;
		uc_reg_write(uc, UC_PPC_REG_3, &x);
		if (run_stub(cfg.stub_addr + STUB_MTXER))
			shadow_xer = xer;
		shadow_gpr[3] = ~r.gpr[3];	// force r3 rewrite below
	}

	for (int i = 0; i < 32; i++) {
		if (!shadow_valid || shadow_gpr[i] != r.gpr[i]) {
			v32[n32] = shadow_gpr[i] = r.gpr[i];
			ids[n] = UC_PPC_REG_0 + i;
			vals[n++] = &v32[n32++];
		}
	}
	for (int i = 0; i < 32; i++) {
		if (!shadow_valid || shadow_fpr[i] != r.fpr[i].j) {
			v64[i] = shadow_fpr[i] = r.fpr[i].j;
			ids[n] = UC_PPC_REG_FPR0 + i;
			vals[n++] = &v64[i];
		}
	}
	const uint32 cr = r.cr.get();
	if (!shadow_valid || shadow_cr != cr) {
		v32[n32] = shadow_cr = cr;
		ids[n] = UC_PPC_REG_CR;
		vals[n++] = &v32[n32++];
	}
	if (!shadow_valid || shadow_lr != r.lr) {
		v32[n32] = shadow_lr = r.lr;
		ids[n] = UC_PPC_REG_LR;
		vals[n++] = &v32[n32++];
	}
	if (!shadow_valid || shadow_ctr != r.ctr) {
		v32[n32] = shadow_ctr = r.ctr;
		ids[n] = UC_PPC_REG_CTR;
		vals[n++] = &v32[n32++];
	}
	if (!shadow_valid || shadow_fpscr != r.fpscr) {
		v32[n32] = shadow_fpscr = r.fpscr;
		ids[n] = UC_PPC_REG_FPSCR;
		vals[n++] = &v32[n32++];
	}
	v32[n32] = r.pc;
	ids[n] = UC_PPC_REG_PC;
	vals[n++] = &v32[n32++];

	uc_reg_write_batch(uc, ids, (void *const *)vals, n);
	shadow_valid = true;
}

void ppc_unicorn::store_state(powerpc_registers &r)
{
	int ids[32 + 32 + 5];
	void *vals[32 + 32 + 5];
	uint32 gpr[32], cr, lr, ctr, fpscr, pc;
	uint64 fpr[32];
	int n = 0;

	for (int i = 0; i < 32; i++) {
		ids[n] = UC_PPC_REG_0 + i;
		vals[n++] = &gpr[i];
	}
	for (int i = 0; i < 32; i++) {
		ids[n] = UC_PPC_REG_FPR0 + i;
		vals[n++] = &fpr[i];
	}
	ids[n] = UC_PPC_REG_CR;		vals[n++] = &cr;
	ids[n] = UC_PPC_REG_LR;		vals[n++] = &lr;
	ids[n] = UC_PPC_REG_CTR;	vals[n++] = &ctr;
	ids[n] = UC_PPC_REG_FPSCR;	vals[n++] = &fpscr;
	ids[n] = UC_PPC_REG_PC;		vals[n++] = &pc;
	uc_reg_read_batch(uc, ids, vals, n);

	// Fetch XER through the stub, then restore r3
	uint32 xer = shadow_xer;
	if (run_stub(cfg.stub_addr + STUB_MFXER))
		uc_reg_read(uc, UC_PPC_REG_3, &xer);
	uc_reg_write(uc, UC_PPC_REG_3, &gpr[3]);

	for (int i = 0; i < 32; i++) {
		r.gpr[i] = shadow_gpr[i] = gpr[i];
		r.fpr[i].j = shadow_fpr[i] = fpr[i];
	}
	r.cr.set(cr);
	r.xer.set(xer);
	r.lr = lr;
	r.ctr = ctr;
	r.fpscr = fpscr;
	r.pc = pc;
	shadow_cr = cr;
	shadow_xer = r.xer.get();
	shadow_lr = lr;
	shadow_ctr = ctr;
	shadow_fpscr = fpscr;
	shadow_valid = true;
}

/*
 *  Execution
 */

void ppc_unicorn::run(exit_info &info)
{
	uint32 entry_pc = 0;
	uc_reg_read(uc, UC_PPC_REG_PC, &entry_pc);
	run_internal(info);
	record_history(entry_pc, info);
}

void ppc_unicorn::run_internal(exit_info &info)
{
	info.insn_pc = 0;
	info.fault_addr = 0;
	info.intno = -1;
	info.err = UC_ERR_OK;
	info.restore_gpr = -1;
	info.restore_val = 0;

	uint32 pc = 0;
	uc_reg_read(uc, UC_PPC_REG_PC, &pc);

	if (pending_async) {
		pending_async = false;
		info.reason = EXIT_ASYNC;
		return;
	}

	last_intno = -1;
	last_fault_addr = 0;
	fault_gpr = -1;
	stat_runs++;
	uc_err err = uc_emu_start(uc, pc, cfg.stub_addr + STUB_UNTIL, 0, 0);
	uc_reg_read(uc, UC_PPC_REG_PC, &pc);

	static const bool stats = getenv("SS_UNICORN_STATS") != NULL;
	if (stats && cfg.ticks_usec) {
		static uint64 next_report = 0, last_runs = 0, last_exc = 0;
		const uint64 now = cfg.ticks_usec();
		if (now >= next_report) {
			if (next_report) {
				uint32 lr = 0;
				uc_reg_read(uc, UC_PPC_REG_LR, &lr);
				fprintf(stderr, "Unicorn PPC: %llu runs/2s, %llu traps/2s, pc %08x lr %08x intno %d err %d\n",
						(unsigned long long)(stat_runs - last_runs),
						(unsigned long long)(stat_exceptions - last_exc), pc, lr, last_intno, (int)err);
			}
			static const bool dump = getenv("SS_UNICORN_DUMP") != NULL;
			if (dump && next_report) {
				fprintf(stderr, "  code @%08x:", pc - 16);
				for (int i = 0; i < 12; i++)
					fprintf(stderr, " %08x", vm_read_memory_4(pc - 16 + i * 4));
				fprintf(stderr, "\n");
			}
			last_runs = stat_runs;
			last_exc = stat_exceptions;
			next_report = now + 2000000;
		}
	}

	if (err == UC_ERR_OK) {
		if (last_intno >= 0) {
			// Unicorn reports PPC exceptions with PC past the trapping instruction
			stat_exceptions++;
			info.reason = EXIT_EXCEPTION;
			info.insn_pc = pc - 4;
			info.intno = last_intno;
			static const bool trace = getenv("SS_UNICORN_TRACE_TRAPS") != NULL;
			if (trace) {
				const uint32 opcode = vm_read_memory_4(info.insn_pc);
				static const bool all = getenv("SS_UNICORN_TRACE_TRAPS")[0] == '2';
				if (all || (opcode >> 26) != 6)
					fprintf(stderr, "Unicorn PPC: trap %d at %08x opcode %08x\n", last_intno, info.insn_pc, opcode);
			}
		}
		else
			info.reason = EXIT_ASYNC;
		return;
	}

	info.insn_pc = pc;
	info.fault_addr = last_fault_addr;
	info.err = err;
	info.restore_gpr = fault_gpr;
	info.restore_val = fault_gpr_val;
	info.reason = (err == UC_ERR_WRITE_PROT) ? EXIT_WRITE_PROTECT : EXIT_ERROR;
}

void ppc_unicorn::record_history(uint32 entry_pc, const exit_info &info)
{
	history_entry &h = history[history_pos++ % HISTORY_SIZE];
	uint32 pc = 0;
	uc_reg_read(uc, UC_PPC_REG_PC, &pc);
	h.entry_pc = entry_pc;
	h.exit_pc = info.reason == EXIT_ASYNC ? pc : info.insn_pc;
	h.opcode = (info.reason == EXIT_EXCEPTION) ? vm_read_memory_4(info.insn_pc) : 0;
	uc_reg_read(uc, UC_PPC_REG_1, &h.r1);
	uc_reg_read(uc, UC_PPC_REG_LR, &h.lr);
	uc_reg_read(uc, UC_PPC_REG_CTR, &h.ctr);
	uc_reg_read(uc, UC_PPC_REG_3, &h.r3);
	h.reason = info.reason;
	h.intno = info.intno;
}

void ppc_unicorn::dump_history()
{
	static const char *names[] = { "async", "trap", "wprot", "error" };
	const uint32 n = history_pos < (uint32)HISTORY_SIZE ? history_pos : HISTORY_SIZE;
	fprintf(stderr, "Unicorn PPC: last %u runs (oldest first):\n", n);
	for (uint32 i = history_pos - n; i != history_pos; i++) {
		const history_entry &h = history[i % HISTORY_SIZE];
		fprintf(stderr, "  %08x -> %08x %-5s op %08x r1 %08x lr %08x ctr %08x r3 %08x\n",
				h.entry_pc, h.exit_pc, names[h.reason & 3], h.opcode, h.r1, h.lr, h.ctr, h.r3);
	}
}

/*
 *  Interrupts
 *
 *  The host interrupt handler (HandleInterrupt) runs guest code that may use
 *  AltiVec.  The interpreter restores the interrupted code's vector registers
 *  afterwards (powerpc_registers::interrupt_copy); Unicorn does not expose
 *  them, so snapshot the whole CPU context instead.  GPRs, FPRs etc. are
 *  reloaded from the kpx register file by the next load_state().
 */

void ppc_unicorn::save_interrupted_state()
{
	const int level = irq_depth++;
	if (level >= 4)
		return;
	if (irq_ctx[level] == NULL && uc_context_alloc(uc, &irq_ctx[level]) != UC_ERR_OK) {
		irq_ctx[level] = NULL;
		return;
	}
	uc_context_save(uc, irq_ctx[level]);
}

void ppc_unicorn::restore_interrupted_state()
{
	const int level = --irq_depth;
	if (level < 0) {
		irq_depth = 0;
		return;
	}
	if (level >= 4 || irq_ctx[level] == NULL)
		return;
	uc_context_restore(uc, irq_ctx[level]);
	shadow_valid = false;	// full register reload on the next load_state()
	tb_last_refresh = 0;	// the restored timebase is stale
}

void ppc_unicorn::request_stop()
{
	stop_flag = 1;
}

void ppc_unicorn::invalidate_range(uint32 start, uint32 end)
{
	if (end > start)
		uc_ctl_remove_cache(uc, start, end);
}

void ppc_unicorn::invalidate_all()
{
	uc_ctl_flush_tb(uc);
}

/*
 *  Guest memory
 *
 *  Guest memory is mapped lazily: the first access to an unmapped guest
 *  page maps the whole host VM region backing it (at VMBaseDiff + guest
 *  address) into Unicorn.  Host write protection is not mirrored, so that
 *  VOSF dirty tracking keeps working through the host SIGSEGV handler; only
 *  the configured read-only ranges are mapped without write permission.
 */

uint64 ppc_unicorn::segment_end(uint64 start, uint64 end, bool *readonly) const
{
	*readonly = false;
	for (int i = 0; i < cfg.n_ro_ranges; i++) {
		const uint64 rs = cfg.ro_ranges[i].start;
		const uint64 re = rs + cfg.ro_ranges[i].size;
		if (start >= rs && start < re) {
			*readonly = true;
			if (re < end)
				end = re;
		}
	}
	if (!*readonly) {
		for (int i = 0; i < cfg.n_ro_ranges; i++) {
			const uint64 rs = cfg.ro_ranges[i].start;
			if (rs > start && rs < end)
				end = rs;
		}
	}
	return end;
}

// Find the readable host VM region containing ADDR
static bool host_region(uintptr addr, uintptr *start, uintptr *end)
{
#if defined(__APPLE__)
	mach_vm_address_t a = addr;
	mach_vm_size_t size = 0;
	vm_region_basic_info_data_64_t info;
	mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t object = MACH_PORT_NULL;
	kern_return_t kr = mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64,
									  (vm_region_info_t)&info, &count, &object);
	if (kr != KERN_SUCCESS || a > addr || addr - a >= size)
		return false;
	if ((info.protection & VM_PROT_READ) == 0)
		return false;
	*start = (uintptr)a;
	*end = (uintptr)(a + size);
	return true;
#elif defined(__linux__)
	FILE *fp = fopen("/proc/self/maps", "r");
	if (fp == NULL)
		return false;
	char line[512];
	bool found = false;
	while (fgets(line, sizeof(line), fp)) {
		uintmax_t s, e;
		char perms[8];
		if (sscanf(line, "%jx-%jx %7s", &s, &e, perms) != 3)
			continue;
		if (addr >= s && addr < e) {
			found = perms[0] == 'r';
			*start = (uintptr)s;
			*end = (uintptr)e;
			break;
		}
	}
	fclose(fp);
	return found;
#else
	const uintptr psize = getpagesize();
	const uintptr page = addr & ~(psize - 1);
	unsigned char vec;
	if (mincore((void *)page, psize, (char *)&vec) != 0)
		return false;
	*start = page;
	*end = page + psize;
	return true;
#endif
}

bool ppc_unicorn::map_host_region(uint32 addr)
{
	const uintptr host = (uintptr)vm_do_get_real_address(addr);
	uintptr hs, he;
	if (!host_region(host, &hs, &he))
		return false;

	// Host region -> guest range, clipped to the 32-bit guest space
	const uintptr base = host - addr;	// == VMBaseDiff
	uint64 gs = (hs > base) ? (uint64)(hs - base) : 0;
	uint64 ge = (uint64)(he - base);
	if (ge > 0x100000000ULL)
		ge = 0x100000000ULL;

	// Clip to the gap between already mapped Unicorn regions around ADDR
	uc_mem_region *regions = NULL;
	uint32_t count = 0;
	if (uc_mem_regions(uc, &regions, &count) == UC_ERR_OK) {
		for (uint32_t i = 0; i < count; i++) {
			const uint64 rb = regions[i].begin, re = regions[i].end + 1;
			if (re <= addr && re > gs)
				gs = re;
			if (rb > addr && rb < ge)
				ge = rb;
		}
		uc_free(regions);
	}

	gs = (gs + GUEST_PAGE_SIZE - 1) & ~(uint64)(GUEST_PAGE_SIZE - 1);
	ge &= ~(uint64)(GUEST_PAGE_SIZE - 1);
	if (gs > addr || ge <= addr)
		return false;

	for (uint64 s = gs; s < ge; ) {
		bool ro;
		const uint64 e = segment_end(s, ge, &ro);
		const uint32 perms = ro ? (UC_PROT_READ | UC_PROT_EXEC) : UC_PROT_ALL;
		uc_err err = uc_mem_map_ptr(uc, s, e - s, perms, (void *)(base + (uintptr)s));
		if (err != UC_ERR_OK) {
			fprintf(stderr, "Unicorn PPC: cannot map guest %08llx-%08llx: %s\n",
					(unsigned long long)s, (unsigned long long)e, uc_strerror(err));
			return false;
		}
		stat_mapped_regions++;
		if (getenv("SS_UNICORN_DEBUG"))
			fprintf(stderr, "Unicorn PPC: mapped %08llx-%08llx %s\n",
					(unsigned long long)s, (unsigned long long)e, ro ? "ro" : "rw");
		s = e;
	}
	return true;
}

bool ppc_unicorn::map_io_page(uint32 addr)
{
	const uint32 page = addr & ~(GUEST_PAGE_SIZE - 1);
	uc_err err = uc_mmio_map(uc, page, GUEST_PAGE_SIZE,
							 ppc_unicorn_hooks::io_read, this,
							 ppc_unicorn_hooks::io_write, this);
	if (err != UC_ERR_OK)
		return false;
	stat_io_pages++;
	if (getenv("SS_UNICORN_DEBUG"))
		fprintf(stderr, "Unicorn PPC: unbacked page %08x mapped as reads-zero/writes-ignored\n", page);
	return true;
}
