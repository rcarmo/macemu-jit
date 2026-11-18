/*
 *  ppc-unicorn.hpp - PowerPC execution backend using the Unicorn engine
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

#ifndef PPC_UNICORN_H
#define PPC_UNICORN_H


/*
 *  The Unicorn backend runs guest PowerPC code with Unicorn's TCG-based
 *  translator.  The kpx register file (powerpc_registers) remains the
 *  architectural source of truth whenever Unicorn is not running: state is
 *  loaded into Unicorn before each run and stored back after it stops.
 *
 *  Unicorn runs the guest in user mode (MSR[PR]=1) with address translation
 *  off, so every privileged instruction (mfmsr, mfspr PVR, ...) traps.  Any
 *  instruction that traps -- including SheepShaver's primary-opcode-6
 *  EMUL_OP/NATIVE_OP/EXEC_RETURN instructions -- is reported back to the
 *  caller, which executes that single instruction with the kpx interpreter.
 *  This keeps the interpreter's SheepShaver-specific semantics unchanged.
 */

struct powerpc_registers;
struct uc_struct;
struct uc_context;

struct ppc_unicorn_config {
	// Value of the emulated processor version register (selects the model)
	uint32 pvr;

	// Guest address of PPC_UNICORN_STUB_SIZE bytes of permanently reserved,
	// word-aligned guest memory used for internal helper stubs
	uint32 stub_addr;

	// Timebase frequency (Hz) and host microsecond clock for mftb
	uint32 timebase_hz;
	uint64 (*ticks_usec)(void);

	// Guest ranges that must be mapped read-only (e.g. ROM, zero page).
	// Writes there are reported as EXIT_WRITE_PROTECT.
	struct range { uint32 start, size; };
	static const int MAX_RO_RANGES = 8;
	range ro_ranges[MAX_RO_RANGES];
	int n_ro_ranges;

	// Called for guest accesses to addresses that are not backed by host
	// memory.  Returning true maps the page as "reads 0 / writes ignored";
	// returning false stops emulation with EXIT_ERROR.
	bool (*ignore_fault)(uint32 pc, uint32 addr, bool is_write);
};

class ppc_unicorn {
public:
	static const uint32 STUB_SIZE = 48;

	enum exit_reason {
		EXIT_ASYNC,			// request_stop() was called
		EXIT_EXCEPTION,		// instruction at insn_pc trapped
		EXIT_WRITE_PROTECT,	// store at insn_pc hit a read-only range
		EXIT_ERROR			// unrecoverable error (see err/fault_addr)
	};

	struct exit_info {
		exit_reason reason;
		uint32 insn_pc;		// faulting instruction (EXCEPTION, WRITE_PROTECT, ERROR)
		uint32 fault_addr;	// data address (WRITE_PROTECT, ERROR)
		int intno;			// Unicorn/QEMU exception number (EXCEPTION)
		int err;			// uc_err value (ERROR)
		int restore_gpr;	// ERROR: GPR clobbered by the failed load, or -1
		uint32 restore_val;	//        and its value before the instruction
	};

	// Returns NULL (after printing why) if Unicorn could not be set up
	static ppc_unicorn *create(const ppc_unicorn_config &config);
	~ppc_unicorn();

	// Copy architectural state between kpx registers and Unicorn
	void load_state(const powerpc_registers &r);
	void store_state(powerpc_registers &r);

	// Preserve state that is not visible through powerpc_registers (AltiVec
	// registers, VSCR, VRSAVE) across a host interrupt handler, the way
	// powerpc_registers::interrupt_copy() does for the interpreter
	void save_interrupted_state();
	void restore_interrupted_state();

	// Run from the PC loaded by load_state() until something needs the host
	void run(exit_info &info);

	// Ask a running run() to return with EXIT_ASYNC; callable from any thread
	void request_stop();

	// Discard translated code for guest range [start, end)
	void invalidate_range(uint32 start, uint32 end);
	void invalidate_all();

	// Print the most recent runs (debugging aid for crashes)
	void dump_history();

	// Human readable error string for exit_info.err
	static const char *error_string(int err);

private:
	ppc_unicorn();
	bool init(const ppc_unicorn_config &config);
	bool run_stub(uint32 addr);
	void run_internal(exit_info &info);
	void record_history(uint32 entry_pc, const exit_info &info);
	bool locate_timebase();
	void refresh_timebase();
	bool map_host_region(uint32 addr);
	bool map_io_page(uint32 addr);
	uint64 segment_end(uint64 start, uint64 end, bool *readonly) const;

	friend struct ppc_unicorn_hooks;

	uc_struct *uc;
	ppc_unicorn_config cfg;

	// Interrupt hook results
	int last_intno;
	uint32 last_fault_addr;
	bool pending_async;
	volatile int stop_flag;		// stop request, polled at block boundaries

	// Destination register of a load that hit unbacked memory
	int fault_gpr;
	uint32 fault_gpr_val;

	// Shadow of the state last exchanged with Unicorn, to avoid redundant writes
	uint32 shadow_gpr[32];
	uint64 shadow_fpr[32];
	uint32 shadow_cr, shadow_lr, shadow_ctr, shadow_fpscr, shadow_xer;
	bool shadow_valid;

	// Timebase emulation (see locate_timebase())
	uc_context *tb_ctx;
	uc_context *irq_ctx[4];		// per interrupt nesting level
	int irq_depth;
	long tb_offset_tbl;		// byte offset of TBL within tb_ctx, or -1
	uint64 tb_last_refresh;
	uint32 tb_refresh_usec;

	// Ring buffer of recent runs
	struct history_entry { uint32 entry_pc, exit_pc, opcode, r1, lr, ctr, r3; int reason, intno; };
	static const int HISTORY_SIZE = 512;
	history_entry history[HISTORY_SIZE];
	uint32 history_pos;

	// Statistics
	uint64 stat_runs, stat_exceptions, stat_io_pages, stat_mapped_regions;
};

#endif /* PPC_UNICORN_H */
