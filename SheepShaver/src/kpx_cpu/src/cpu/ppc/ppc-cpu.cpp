/*
 *  ppc-cpu.cpp - PowerPC CPU definition
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
#include <stdlib.h>
#include <assert.h>
#ifdef SHEEPSHAVER
#include "thunks.h"
#endif
#include "vm_alloc.h"
#include "cpu/vm.hpp"
#include "cpu/ppc/ppc-cpu.hpp"
#if PPC_ENABLE_UNICORN
#include "cpu/ppc/ppc-unicorn.hpp"
#endif
#ifndef SHEEPSHAVER
#include "basic-kernel.hpp"
#endif

#if PPC_ENABLE_JIT
#include "cpu/jit/dyngen-exec.h"
#endif

#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
#define PPC_AARCH64_DIRECT_JIT 1
#else
#define PPC_AARCH64_DIRECT_JIT 0
#endif

#if PPC_AARCH64_DIRECT_JIT
/* The AArch64 PPC JIT emits guest EAs as host pointers, so it requires the
 * guest address space to be mapped 1:1 (VMBaseDiff == 0). Direct addressing
 * with a non-zero offset (e.g. NATMEM_OFFSET on Apple Silicon, where the 4 GB
 * __PAGEZERO forbids low fixed mappings) must use the interpreter instead. */
static bool ppc_aarch64_jit_address_space_ok(void)
{
#if DIRECT_ADDRESSING
	static bool warned = false;
	if (VMBaseDiff != 0) {
		if (!warned) {
			fprintf(stderr, "AArch64 PPC JIT disabled: needs 1:1 guest mapping, "
				"VMBaseDiff=0x%lx; using the interpreter\n", (unsigned long)VMBaseDiff);
			warned = true;
		}
		return false;
	}
#endif
	return true;
}
#endif

static bool ppc_aarch64_reentrant_direct_jit_enabled(void)
{
#if PPC_AARCH64_DIRECT_JIT
	if (!ppc_aarch64_jit_address_space_ok())
		return false;
	/* AArch64 direct-JIT reentrant execution is the default product path after
	 * the slow-path store byte-order fix and 3600s opt-in cold-boot hold.  Keep
	 * SS_JIT_REENTRANT_DIRECT=0 as an emergency/runtime diagnostic opt-out. */
	static const char *env = getenv("SS_JIT_REENTRANT_DIRECT");
	return !(env && env[0] == '0' && env[1] == '\0');
#else
	return false;
#endif
}

#if ENABLE_MON
#include "mon.h"
#include "mon_disass.h"
#endif

#define DEBUG 0
#include "debug.h"

#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
extern uint32 RAMBase;
extern uint8 *RAMBaseHost;
extern uint32 RAMSize;
extern uint32 ROMBase;
extern uint8 *ROMBaseHost;
extern uint32 ROMEnd;
#include "cpu/jit/aarch64/ppc-jit.h"

static inline bool ppc_jit_contains32(uint32 pc, uint32 base, uint64 size, uint32 access_size = 4)
{
	uint64 p = pc;
	uint64 b = base;
	return p >= b && p + access_size >= p && p + access_size <= b + size;
}

static uint64 ppc_jit_ratio_native_dispatches = 0;
static uint64 ppc_jit_ratio_native_insns_known = 0;
static uint64 ppc_jit_ratio_exec_normal_blocks = 0;
static uint64 ppc_jit_ratio_exec_normal_insns = 0;
static uint64 ppc_jit_ratio_skip_jit_entries = 0;
static uint64 ppc_jit_ratio_gate3_entries = 0;
static uint64 ppc_jit_ratio_next_report = 1000;

#ifdef SS_JIT_BENCH_CENSUS
static ppc_jit_bench_exec_stats ppc_jit_bench_exec = {};

void ppc_jit_aarch64_bench_exec_reset(void)
{
	memset(&ppc_jit_bench_exec, 0, sizeof(ppc_jit_bench_exec));
}

void ppc_jit_aarch64_bench_exec_snapshot(ppc_jit_bench_exec_stats *out)
{
	if (out) *out = ppc_jit_bench_exec;
}
#endif

static bool ppc_jit_ratio_enabled(void)
{
	static const char *env = getenv("SS_JIT_RATIO");
	return env && env[0] && !(env[0] == '0' && env[1] == '\0');
}

static void ppc_jit_ratio_report(bool force)
{
	if (!ppc_jit_ratio_enabled()) return;
	uint64 total_blocks = ppc_jit_ratio_native_dispatches + ppc_jit_ratio_exec_normal_blocks;
	if (!force && total_blocks < ppc_jit_ratio_next_report) return;
	while (ppc_jit_ratio_next_report <= total_blocks)
		ppc_jit_ratio_next_report += 1000;
	uint64 total_known_insns = ppc_jit_ratio_native_insns_known + ppc_jit_ratio_exec_normal_insns;
	fprintf(stderr,
		"PPC-JIT-A64-RATIO: native_dispatch=%llu exec_normal_blocks=%llu native_dispatch_pct=%.6f "
		"native_insns_known=%llu exec_normal_insns=%llu native_known_insn_pct=%.6f "
		"skip_jit=%llu gate3=%llu\n",
		(unsigned long long)ppc_jit_ratio_native_dispatches,
		(unsigned long long)ppc_jit_ratio_exec_normal_blocks,
		total_blocks ? (double)ppc_jit_ratio_native_dispatches * 100.0 / (double)total_blocks : 0.0,
		(unsigned long long)ppc_jit_ratio_native_insns_known,
		(unsigned long long)ppc_jit_ratio_exec_normal_insns,
		total_known_insns ? (double)ppc_jit_ratio_native_insns_known * 100.0 / (double)total_known_insns : 0.0,
		(unsigned long long)ppc_jit_ratio_skip_jit_entries,
		(unsigned long long)ppc_jit_ratio_gate3_entries);
}

static bool ppc_jit_hist_enabled(void)
{
	static const char *env = getenv("SS_JIT_HIST");
	return env && env[0] && !(env[0] == '0' && env[1] == '\0');
}

static bool ppc_jit_failprobe_enabled(void)
{
	static const char *env = getenv("SS_JIT_FAILPROBE");
	return env && env[0] && !(env[0] == '0' && env[1] == '\0');
}

static bool ppc_jit_failprobe_pc_match(uint32 pc)
{
	/* Default target: the 2026-06-24 interpreted desktop hot clusters. */
	return pc == 0x18466080 || pc == 0x18466084 ||
	       (pc >= 0x18467d00 && pc <= 0x18467fff);
}

static uint32 ppc_jit_failprobe_pending_start = 0;
static uint32 ppc_jit_failprobe_pending_fail_pc = 0;
static uint32 ppc_jit_failprobe_pending_fail_opcode = 0;
static uint64 ppc_jit_failprobe_reports = 0;

static void ppc_jit_failprobe_note(uint32 block_start, uint32 fail_pc, const char *why)
{
	if (!ppc_jit_failprobe_enabled()) return;
	if (!ppc_jit_failprobe_pc_match(block_start) && !ppc_jit_failprobe_pc_match(fail_pc)) return;
	if (ppc_jit_failprobe_reports >= 256) return;
	uint32 fail_opcode = vm_read_memory_4(fail_pc);
	ppc_jit_failprobe_pending_start = block_start;
	ppc_jit_failprobe_pending_fail_pc = fail_pc;
	ppc_jit_failprobe_pending_fail_opcode = fail_opcode;
	fprintf(stderr,
		"PPC-JIT-A64-FAILPROBE: %s block_start=0x%08x fail_pc=0x%08x "
		"fail_opcode=0x%08x opc=%u xo31=%u\n",
		why, block_start, fail_pc, fail_opcode, fail_opcode >> 26,
		((fail_opcode >> 26) == 31) ? ((fail_opcode >> 1) & 0x3ff) : 0);
	ppc_jit_failprobe_reports++;
}

static void ppc_jit_failprobe_exec_entry(uint32 entry_pc, uint32 first_opcode, uint32 insns)
{
	if (!ppc_jit_failprobe_enabled()) return;
	bool pending = ppc_jit_failprobe_pending_fail_pc == entry_pc;
	if (!pending && !ppc_jit_failprobe_pc_match(entry_pc)) return;
	if (ppc_jit_failprobe_reports >= 256) return;
	fprintf(stderr,
		"PPC-JIT-A64-FAILPROBE: exec_entry=0x%08x first_opcode=0x%08x "
		"insns=%u prev_block_start=0x%08x prev_fail_pc=0x%08x prev_fail_opcode=0x%08x\n",
		entry_pc, first_opcode, insns,
		pending ? ppc_jit_failprobe_pending_start : 0,
		pending ? ppc_jit_failprobe_pending_fail_pc : 0,
		pending ? ppc_jit_failprobe_pending_fail_opcode : 0);
	if (pending) {
		ppc_jit_failprobe_pending_start = 0;
		ppc_jit_failprobe_pending_fail_pc = 0;
		ppc_jit_failprobe_pending_fail_opcode = 0;
	}
	ppc_jit_failprobe_reports++;
}

struct ppc_jit_exec_hist_entry {
	uint32 pc;
	uint32 opcode;
	uint64 blocks;
	uint64 insns;
};

static const uint32 PPC_JIT_EXEC_HIST_SIZE = 4096;
static ppc_jit_exec_hist_entry ppc_jit_exec_hist[PPC_JIT_EXEC_HIST_SIZE];
static uint64 ppc_jit_exec_hist_total_blocks = 0;
static uint64 ppc_jit_exec_hist_total_insns = 0;
static uint64 ppc_jit_exec_hist_next_report = 10000000ULL;

static void ppc_jit_hist_report(bool force)
{
	if (!ppc_jit_hist_enabled()) return;
	if (!force && ppc_jit_exec_hist_total_blocks < ppc_jit_exec_hist_next_report) return;
	while (ppc_jit_exec_hist_next_report <= ppc_jit_exec_hist_total_blocks)
		ppc_jit_exec_hist_next_report += 10000000ULL;
	fprintf(stderr,
		"PPC-JIT-A64-HIST: exec_normal_total_blocks=%llu exec_normal_total_insns=%llu top_by_insns:\n",
		(unsigned long long)ppc_jit_exec_hist_total_blocks,
		(unsigned long long)ppc_jit_exec_hist_total_insns);
	int selected[20];
	for (int i = 0; i < 20; i++) selected[i] = -1;
	for (int rank = 0; rank < 20; rank++) {
		int best = -1;
		for (uint32 i = 0; i < PPC_JIT_EXEC_HIST_SIZE; i++) {
			if (ppc_jit_exec_hist[i].blocks == 0) continue;
			bool used = false;
			for (int j = 0; j < rank; j++) if (selected[j] == (int)i) { used = true; break; }
			if (used) continue;
			if (best < 0 || ppc_jit_exec_hist[i].insns > ppc_jit_exec_hist[best].insns)
				best = (int)i;
		}
		if (best < 0) break;
		selected[rank] = best;
		uint32 opcode = ppc_jit_exec_hist[best].opcode;
		uint32 opc = opcode >> 26;
		uint32 xo = (opcode >> 1) & 0x3ff;
		double pct = ppc_jit_exec_hist_total_insns ?
			(double)ppc_jit_exec_hist[best].insns * 100.0 / (double)ppc_jit_exec_hist_total_insns : 0.0;
		fprintf(stderr,
			"PPC-JIT-A64-HIST: rank=%d pc=0x%08x opcode=0x%08x opc=%u xo=%u blocks=%llu insns=%llu insn_pct=%.6f\n",
			rank + 1,
			ppc_jit_exec_hist[best].pc,
			opcode,
			opc,
			xo,
			(unsigned long long)ppc_jit_exec_hist[best].blocks,
			(unsigned long long)ppc_jit_exec_hist[best].insns,
			pct);
	}
}

static void ppc_jit_hist_record_exec_normal(uint32 pc, uint32 opcode, uint32 insns)
{
	if (!ppc_jit_hist_enabled()) return;
	ppc_jit_exec_hist_total_blocks++;
	ppc_jit_exec_hist_total_insns += insns;
	uint32 h = ((pc >> 2) ^ opcode ^ (opcode >> 16)) & (PPC_JIT_EXEC_HIST_SIZE - 1);
	for (uint32 probe = 0; probe < PPC_JIT_EXEC_HIST_SIZE; probe++) {
		uint32 idx = (h + probe) & (PPC_JIT_EXEC_HIST_SIZE - 1);
		if (ppc_jit_exec_hist[idx].blocks == 0 ||
		    (ppc_jit_exec_hist[idx].pc == pc && ppc_jit_exec_hist[idx].opcode == opcode)) {
			ppc_jit_exec_hist[idx].pc = pc;
			ppc_jit_exec_hist[idx].opcode = opcode;
			ppc_jit_exec_hist[idx].blocks++;
			ppc_jit_exec_hist[idx].insns += insns;
			break;
		}
	}
	ppc_jit_hist_report(false);
}

struct ppc_jit_native_hist_entry {
	uint32 pc;
	uint32 opcode;
	uint64 blocks;
	uint64 insns;
};

static const uint32 PPC_JIT_NATIVE_HIST_SIZE = 4096;
static ppc_jit_native_hist_entry ppc_jit_native_hist[PPC_JIT_NATIVE_HIST_SIZE];
static uint64 ppc_jit_native_hist_total_blocks = 0;
static uint64 ppc_jit_native_hist_total_insns = 0;
static uint64 ppc_jit_native_hist_next_report = 10000000ULL;

static bool ppc_jit_native_hist_enabled(void)
{
	static const char *env = getenv("SS_JIT_NATIVE_HIST");
	return env && env[0] && !(env[0] == '0' && env[1] == '\0');
}

static void ppc_jit_native_hist_report(bool force)
{
	if (!ppc_jit_native_hist_enabled()) return;
	if (!force && ppc_jit_native_hist_total_blocks < ppc_jit_native_hist_next_report) return;
	while (ppc_jit_native_hist_next_report <= ppc_jit_native_hist_total_blocks)
		ppc_jit_native_hist_next_report += 10000000ULL;
	fprintf(stderr,
		"PPC-JIT-A64-NATIVEHIST: native_total_blocks=%llu native_total_insns=%llu top_by_insns:\n",
		(unsigned long long)ppc_jit_native_hist_total_blocks,
		(unsigned long long)ppc_jit_native_hist_total_insns);
	int selected[20];
	for (int i = 0; i < 20; i++) selected[i] = -1;
	for (int rank = 0; rank < 20; rank++) {
		int best = -1;
		for (uint32 i = 0; i < PPC_JIT_NATIVE_HIST_SIZE; i++) {
			if (ppc_jit_native_hist[i].blocks == 0) continue;
			bool used = false;
			for (int j = 0; j < rank; j++) if (selected[j] == (int)i) { used = true; break; }
			if (used) continue;
			if (best < 0 || ppc_jit_native_hist[i].insns > ppc_jit_native_hist[best].insns)
				best = (int)i;
		}
		if (best < 0) break;
		selected[rank] = best;
		uint32 opcode = ppc_jit_native_hist[best].opcode;
		uint32 opc = opcode >> 26;
		uint32 xo = (opcode >> 1) & 0x3ff;
		double pct = ppc_jit_native_hist_total_insns ?
			(double)ppc_jit_native_hist[best].insns * 100.0 / (double)ppc_jit_native_hist_total_insns : 0.0;
		fprintf(stderr,
			"PPC-JIT-A64-NATIVEHIST: rank=%d pc=0x%08x opcode=0x%08x opc=%u xo=%u blocks=%llu insns=%llu insn_pct=%.6f\n",
			rank + 1,
			ppc_jit_native_hist[best].pc,
			opcode,
			opc,
			xo,
			(unsigned long long)ppc_jit_native_hist[best].blocks,
			(unsigned long long)ppc_jit_native_hist[best].insns,
			pct);
	}
}

static void ppc_jit_native_hist_record(uint32 pc, uint32 opcode, uint32 insns)
{
	if (!ppc_jit_native_hist_enabled() || insns == 0) return;
	ppc_jit_native_hist_total_blocks++;
	ppc_jit_native_hist_total_insns += insns;
	uint32 h = ((pc >> 2) ^ opcode ^ (opcode >> 16)) & (PPC_JIT_NATIVE_HIST_SIZE - 1);
	for (uint32 probe = 0; probe < PPC_JIT_NATIVE_HIST_SIZE; probe++) {
		uint32 idx = (h + probe) & (PPC_JIT_NATIVE_HIST_SIZE - 1);
		if (ppc_jit_native_hist[idx].blocks == 0 ||
		    (ppc_jit_native_hist[idx].pc == pc && ppc_jit_native_hist[idx].opcode == opcode)) {
			ppc_jit_native_hist[idx].pc = pc;
			ppc_jit_native_hist[idx].opcode = opcode;
			ppc_jit_native_hist[idx].blocks++;
			ppc_jit_native_hist[idx].insns += insns;
			break;
		}
	}
	ppc_jit_native_hist_report(false);
}

static bool ppc_jit_skip_hist_enabled(void)
{
	static const char *env = getenv("SS_JIT_SKIP_HIST");
	return env && env[0] && !(env[0] == '0' && env[1] == '\0');
}

static bool ppc_jit_skip_log_enabled(void)
{
	static const char *env = getenv("SS_JIT_SKIP_LOG");
	return env && env[0] && !(env[0] == '0' && env[1] == '\0');
}

enum ppc_jit_skip_reason {
	PPC_JIT_SKIP_NONE = 0,
	PPC_JIT_SKIP_DISABLED,
	PPC_JIT_SKIP_REGION,
	PPC_JIT_SKIP_COMPILE_FALSE,
	PPC_JIT_SKIP_GATE3
};

static const char *ppc_jit_skip_reason_name(int reason)
{
	switch (reason) {
	case PPC_JIT_SKIP_DISABLED: return "disabled";
	case PPC_JIT_SKIP_REGION: return "region";
	case PPC_JIT_SKIP_COMPILE_FALSE: return "compile_false";
	case PPC_JIT_SKIP_GATE3: return "gate3";
	default: return "unknown";
	}
}

struct ppc_jit_skip_hist_entry {
	uint32 pc;
	uint32 opcode;
	uint32 reason;
	uint64 count;
};

static const uint32 PPC_JIT_SKIP_HIST_SIZE = 1024;
static ppc_jit_skip_hist_entry ppc_jit_skip_hist[PPC_JIT_SKIP_HIST_SIZE];
static uint64 ppc_jit_skip_hist_total = 0;
static uint64 ppc_jit_skip_hist_next_report = 64;
static uint64 ppc_jit_skip_hist_last_report_total = 0;

static void ppc_jit_skip_hist_report(bool force)
{
	if (!ppc_jit_skip_hist_enabled()) return;
	if (!force && ppc_jit_skip_hist_total < ppc_jit_skip_hist_next_report) return;
	if (force && ppc_jit_skip_hist_total == ppc_jit_skip_hist_last_report_total) return;
	while (ppc_jit_skip_hist_next_report <= ppc_jit_skip_hist_total)
		ppc_jit_skip_hist_next_report += 64;
	ppc_jit_skip_hist_last_report_total = ppc_jit_skip_hist_total;
	fprintf(stderr,
		"PPC-JIT-A64-SKIPHIST: total=%llu top_by_count:\n",
		(unsigned long long)ppc_jit_skip_hist_total);
	int selected[32];
	for (int i = 0; i < 32; i++) selected[i] = -1;
	for (int rank = 0; rank < 32; rank++) {
		int best = -1;
		for (uint32 i = 0; i < PPC_JIT_SKIP_HIST_SIZE; i++) {
			if (ppc_jit_skip_hist[i].count == 0) continue;
			bool used = false;
			for (int j = 0; j < rank; j++) if (selected[j] == (int)i) { used = true; break; }
			if (used) continue;
			if (best < 0 || ppc_jit_skip_hist[i].count > ppc_jit_skip_hist[best].count)
				best = (int)i;
		}
		if (best < 0) break;
		selected[rank] = best;
		uint32 opcode = ppc_jit_skip_hist[best].opcode;
		uint32 opc = opcode >> 26;
		uint32 xo = (opcode >> 1) & 0x3ff;
		double pct = ppc_jit_skip_hist_total ?
			(double)ppc_jit_skip_hist[best].count * 100.0 / (double)ppc_jit_skip_hist_total : 0.0;
		fprintf(stderr,
			"PPC-JIT-A64-SKIPHIST: rank=%d pc=0x%08x opcode=0x%08x opc=%u xo=%u reason=%s count=%llu pct=%.6f\n",
			rank + 1,
			ppc_jit_skip_hist[best].pc,
			opcode,
			opc,
			xo,
			ppc_jit_skip_reason_name(ppc_jit_skip_hist[best].reason),
			(unsigned long long)ppc_jit_skip_hist[best].count,
			pct);
	}
}

static void ppc_jit_skip_hist_record(uint32 pc, uint32 opcode, int reason)
{
	if (reason == PPC_JIT_SKIP_NONE) return;
#ifdef SS_JIT_BENCH_CENSUS
	switch (reason) {
	case PPC_JIT_SKIP_DISABLED: ppc_jit_bench_exec.skip_disabled++; break;
	case PPC_JIT_SKIP_REGION: ppc_jit_bench_exec.skip_region++; break;
	case PPC_JIT_SKIP_COMPILE_FALSE: ppc_jit_bench_exec.skip_compile_false++; break;
	case PPC_JIT_SKIP_GATE3: ppc_jit_bench_exec.skip_gate3++; break;
	default: break;
	}
#endif
	if (!ppc_jit_skip_hist_enabled() && !ppc_jit_skip_log_enabled()) return;
	ppc_jit_skip_hist_total++;
	if (ppc_jit_skip_log_enabled()) {
		fprintf(stderr,
			"PPC-JIT-A64-SKIP: seq=%llu pc=0x%08x opcode=0x%08x opc=%u xo=%u reason=%s\n",
			(unsigned long long)ppc_jit_skip_hist_total,
			pc,
			opcode,
			opcode >> 26,
			(opcode >> 1) & 0x3ff,
			ppc_jit_skip_reason_name(reason));
	}
	if (!ppc_jit_skip_hist_enabled()) return;
	uint32 h = ((pc >> 2) ^ opcode ^ (opcode >> 16) ^ (uint32)reason) & (PPC_JIT_SKIP_HIST_SIZE - 1);
	for (uint32 probe = 0; probe < PPC_JIT_SKIP_HIST_SIZE; probe++) {
		uint32 idx = (h + probe) & (PPC_JIT_SKIP_HIST_SIZE - 1);
		if (ppc_jit_skip_hist[idx].count == 0 ||
		    (ppc_jit_skip_hist[idx].pc == pc && ppc_jit_skip_hist[idx].opcode == opcode && ppc_jit_skip_hist[idx].reason == (uint32)reason)) {
			ppc_jit_skip_hist[idx].pc = pc;
			ppc_jit_skip_hist[idx].opcode = opcode;
			ppc_jit_skip_hist[idx].reason = (uint32)reason;
			ppc_jit_skip_hist[idx].count++;
			break;
		}
	}
	ppc_jit_skip_hist_report(false);
}

static bool ppc_jit_aarch64_region_for_pc(uint32 pc, const uint8 **host_base, uint32 *guest_base, size_t *size)
{
	uint32 ram_start = RAMBase ? RAMBase : (uint32)(uintptr_t)RAMBaseHost;
	if (ppc_jit_contains32(pc, ram_start, RAMSize)) {
		*host_base = RAMBaseHost;
		*guest_base = ram_start;
		*size = RAMSize;
		return true;
	}
	if (ppc_jit_contains32(pc, ROMBase, 0x500000)) {
		*host_base = ROMBaseHost;
		*guest_base = ROMBase;
		*size = 0x500000;
		return true;
	}
#ifdef SHEEPSHAVER
	if (ppc_jit_contains32(pc, SheepMem::Base(), (uint64)SheepMem::End() - SheepMem::Base())) {
		*host_base = (const uint8 *)(uintptr_t)SheepMem::Base();
		*guest_base = SheepMem::Base();
		*size = SheepMem::End() - SheepMem::Base();
		return true;
	}
#endif
	*host_base = NULL;
	*guest_base = 0;
	*size = 0;
	return false;
}
#endif

#if PPC_PROFILE_GENERIC_CALLS
uint32 powerpc_cpu::generic_calls_count[PPC_I(MAX)];
static int generic_calls_ids[PPC_I(MAX)];
const int generic_calls_top_ten = 20;

int generic_calls_compare(const void *e1, const void *e2)
{
	const int id1 = *(const int *)e1;
	const int id2 = *(const int *)e2;
	return powerpc_cpu::generic_calls_count[id2] - powerpc_cpu::generic_calls_count[id1];
}
#endif

#if PPC_PROFILE_REGS_USE
int register_info_compare(const void *e1, const void *e2)
{
	const powerpc_cpu::register_info *ri1 = (powerpc_cpu::register_info *)e1;
	const powerpc_cpu::register_info *ri2 = (powerpc_cpu::register_info *)e2;
	return ri2->count - ri1->count;
}
#endif

static int ppc_refcount = 0;

#ifdef DO_CONVENTION_CALL_STATICS
template<> bool nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_init_done = false;
template<> int nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_code_len = 0;
template<> int nv_mem_fun1_t<void, powerpc_cpu, uint32>::do_convention_call_pf_offset = 0;
#endif

void powerpc_cpu::set_register(int id, any_register const & value)
{
	if (id >= powerpc_registers::GPR(0) && id <= powerpc_registers::GPR(31)) {
		gpr(id - powerpc_registers::GPR_BASE) = value.i;
		return;
	}
	if (id >= powerpc_registers::FPR(0) && id <= powerpc_registers::FPR(31)) {
		fpr(id - powerpc_registers::FPR_BASE) = value.d;
		return;
	}
	switch (id) {
	case powerpc_registers::CR:			cr().set(value.i);		break;
	case powerpc_registers::FPSCR:		fpscr() = value.i;		break;
	case powerpc_registers::XER:		xer().set(value.i);		break;
	case powerpc_registers::LR:			lr() = value.i;			break;
	case powerpc_registers::CTR:		ctr() = value.i;		break;
	case basic_registers::PC:
	case powerpc_registers::PC:			pc() = value.i;			break;
	case basic_registers::SP:
	case powerpc_registers::SP:			gpr(1)= value.i;		break;
	default:							abort();				break;
	}
}

any_register powerpc_cpu::get_register(int id)
{
	any_register value;
	if (id >= powerpc_registers::GPR(0) && id <= powerpc_registers::GPR(31)) {
		value.i = gpr(id - powerpc_registers::GPR_BASE);
		return value;
	}
	if (id >= powerpc_registers::FPR(0) && id <= powerpc_registers::FPR(31)) {
		value.d = fpr(id - powerpc_registers::FPR_BASE);
		return value;
	}
	switch (id) {
	case powerpc_registers::CR:			value.i = cr().get();	break;
	case powerpc_registers::FPSCR:		value.i = fpscr();		break;
	case powerpc_registers::XER:		value.i = xer().get();	break;
	case powerpc_registers::LR:			value.i = lr();			break;
	case powerpc_registers::CTR:		value.i = ctr();		break;
	case basic_registers::PC:
	case powerpc_registers::PC:			value.i = pc();			break;
	case basic_registers::SP:
	case powerpc_registers::SP:			value.i = gpr(1);		break;
	default:							abort();				break;
	}
	return value;
}

#if KPX_MAX_CPUS != 1
uint32 powerpc_registers::reserve_valid = 0;
uint32 powerpc_registers::reserve_addr = 0;
uint32 powerpc_registers::reserve_data = 0;
#endif

void powerpc_cpu::init_registers()
{
	assert((((uintptr)&vr(0)) % 16) == 0);
	for (int i = 0; i < 32; i++) {
		gpr(i) = 0;
		fpr(i) = 0;
	}
	cr().set(0);
	fpscr() = 0;
	xer().set(0);
	lr() = 0;
	ctr() = 0;
	pc() = 0;
}

void powerpc_cpu::init_flight_recorder()
{
#if PPC_FLIGHT_RECORDER
	log_ptr = 0;
	log_ptr_wrapped = false;
#endif
}

void powerpc_cpu::do_record_step(uint32 pc, uint32 opcode)
{
#if PPC_FLIGHT_RECORDER
	log[log_ptr].pc = pc;
	log[log_ptr].opcode = opcode;
#ifdef SHEEPSHAVER
	log[log_ptr].sp = gpr(1);
	log[log_ptr].r24 = gpr(24);
#endif
#if PPC_FLIGHT_RECORDER >= 2
	for (int i = 0; i < 32; i++) {
		log[log_ptr].r[i] = gpr(i);
		log[log_ptr].fr[i] = fpr(i);
	}
	log[log_ptr].lr = lr();
	log[log_ptr].ctr = ctr();
	log[log_ptr].cr = cr().get();
	log[log_ptr].xer = xer().get();
	log[log_ptr].fpscr = fpscr();
#endif
	log_ptr++;
	if (log_ptr == LOG_SIZE) {
		log_ptr = 0;
		log_ptr_wrapped = true;
	}
#endif
}

#if PPC_FLIGHT_RECORDER
void powerpc_cpu::start_log()
{
	logging = true;
	invalidate_cache();
}

void powerpc_cpu::stop_log()
{
	logging = false;
	invalidate_cache();
}

void powerpc_cpu::dump_log(const char *filename)
{
	if (filename == NULL)
		filename = "ppc.log";

	FILE *f = fopen(filename, "w");
	if (f == NULL)
		return;

	int start_ptr = 0;
	int log_size = log_ptr;
	if (log_ptr_wrapped) {
		start_ptr = log_ptr;
		log_size = LOG_SIZE;
	}

	for (int i = 0; i < log_size; i++) {
		int j = (i + start_ptr) % LOG_SIZE;
#if PPC_FLIGHT_RECORDER >= 2
		fprintf(f, " pc %08x  lr %08x ctr %08x  cr %08x xer %08x ", log[j].pc, log[j].lr, log[j].ctr, log[j].cr, log[j].xer);
		fprintf(f, " r0 %08x  r1 %08x  r2 %08x  r3 %08x ", log[j].r[0], log[j].r[1], log[j].r[2], log[j].r[3]);
		fprintf(f, " r4 %08x  r5 %08x  r6 %08x  r7 %08x ", log[j].r[4], log[j].r[5], log[j].r[6], log[j].r[7]);
		fprintf(f, " r8 %08x  r9 %08x r10 %08x r11 %08x ", log[j].r[8], log[j].r[9], log[j].r[10], log[j].r[11]);
		fprintf(f, "r12 %08x r13 %08x r14 %08x r15 %08x ", log[j].r[12], log[j].r[13], log[j].r[14], log[j].r[15]);
		fprintf(f, "r16 %08x r17 %08x r18 %08x r19 %08x ", log[j].r[16], log[j].r[17], log[j].r[18], log[j].r[19]);
		fprintf(f, "r20 %08x r21 %08x r22 %08x r23 %08x ", log[j].r[20], log[j].r[21], log[j].r[22], log[j].r[23]);
		fprintf(f, "r24 %08x r25 %08x r26 %08x r27 %08x ", log[j].r[24], log[j].r[25], log[j].r[26], log[j].r[27]);
		fprintf(f, "r28 %08x r29 %08x r30 %08x r31 %08x\n", log[j].r[28], log[j].r[29], log[j].r[30], log[j].r[31]);
		fprintf(f, "opcode %08x\n", log[j].opcode);
#else
		fprintf(f, " pc %08x opc %08x", log[j].pc, log[j].opcode);
#ifdef SHEEPSHAVER
		fprintf(f, " sp %08x r24 %08x", log[j].sp, log[j].r24);
#endif
		fprintf(f, "| ");
#if !ENABLE_MON
		fprintf(f, "\n");
#endif
#endif
#if ENABLE_MON
		disass_ppc(f, log[j].pc, log[j].opcode);
#endif
	}
	fclose(f);
}
#endif

#if ENABLE_MON
static uint32 mon_read_byte_ppc(uintptr addr)
{
	return *((uint8 *)addr);
}

static void mon_write_byte_ppc(uintptr addr, uint32 b)
{
	uint8 *m = (uint8 *)addr;
	*m = b;
}
#endif

void powerpc_cpu::initialize()
{
#ifdef SHEEPSHAVER
	printf("PowerPC CPU emulator by Gwenole Beauchesne\n");
#endif

#if PPC_PROFILE_REGS_USE
	reginfo = new register_info[32];
	for (int i = 0; i < 32; i++) {
		reginfo[i].id = i;
		reginfo[i].count = 0;
	}
#endif

	init_flight_recorder();
	init_decoder();
	init_registers();
	init_decode_cache();
	execute_depth = 0;
#if PPC_ENABLE_UNICORN
	unicorn = NULL;
#endif

	// Initialize block lookup table
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	my_block_cache.initialize();
#endif

	// Init cache range invalidate recorder
	cache_range.start = cache_range.end = 0;

	// Init syscalls handler
	execute_do_syscall = NULL;

	// Init field2mask
	for (int i = 0; i < 256; i++) {
		uint32 mask = 0;
		if (i & 0x01) mask |= 0x0000000f;
		if (i & 0x02) mask |= 0x000000f0;
		if (i & 0x04) mask |= 0x00000f00;
		if (i & 0x08) mask |= 0x0000f000;
		if (i & 0x10) mask |= 0x000f0000;
		if (i & 0x20) mask |= 0x00f00000;
		if (i & 0x40) mask |= 0x0f000000;
		if (i & 0x80) mask |= 0xf0000000;
		field2mask[i] = mask;
	}

#if ENABLE_MON
	mon_init();
	mon_read_byte = mon_read_byte_ppc;
	mon_write_byte = mon_write_byte_ppc;
#endif

#if PPC_PROFILE_COMPILE_TIME
	compile_count = 0;
	compile_time = 0;
	emul_start_time = clock();
#endif
}

#if PPC_ENABLE_JIT
void powerpc_cpu::enable_jit(uint32 cache_size)
{
	use_jit = true;
	if (cache_size)
		codegen.set_cache_size(cache_size);
	codegen.initialize();
}
#endif

// Memory allocator returning powerpc_cpu objects aligned on 16-byte boundaries
// FORMAT: [ alignment ] magic identifier, offset to malloc'ed data, powerpc_cpu data
void *powerpc_cpu::operator new(size_t size)
{
	const int ALIGN = 16;

	// Allocate enough space for powerpc_cpu data + signature + align pad
	uint8 *ptr = (uint8 *)malloc(size + ALIGN * 2);
	if (ptr == NULL)
		throw std::bad_alloc();

	// Align memory
	int ofs = 0;
	while ((((uintptr)ptr) % ALIGN) != 0)
		ofs++, ptr++;

	// Insert signature and offset
	struct aligned_block_t {
		uint32 pad[(ALIGN - 8) / 4];
		uint32 signature;
		uint32 offset;
		uint8  data[sizeof(powerpc_cpu)];
	};
	aligned_block_t *blk = (aligned_block_t *)ptr;
	blk->signature = 0x53435055;		/* 'SCPU' */
	blk->offset = ofs + (&blk->data[0] - (uint8 *)blk);
	assert((((uintptr)&blk->data) % ALIGN) == 0);
	return &blk->data[0];
}

void powerpc_cpu::operator delete(void *p)
{
	uint32 *blk = (uint32 *)p;
	assert(blk[-2] == 0x53435055);		/* 'SCPU' */
	void *ptr = (void *)(((uintptr)p) - blk[-1]);
	free(ptr);
}

#ifdef SHEEPSHAVER
powerpc_cpu::powerpc_cpu()
#if PPC_ENABLE_JIT
	: codegen(this)
#endif
#else
powerpc_cpu::powerpc_cpu(task_struct *parent_task)
	: basic_cpu(parent_task)
#if PPC_ENABLE_JIT
	, codegen(this)
#endif
#endif
{
#if PPC_ENABLE_JIT
	use_jit = false;
#endif
	spcflags().init();
	++ppc_refcount;
	initialize();
}

powerpc_cpu::~powerpc_cpu()
{
	--ppc_refcount;
#if PPC_ENABLE_UNICORN
	delete unicorn;
	unicorn = NULL;
#endif
#if PPC_PROFILE_COMPILE_TIME
	clock_t emul_end_time = clock();

	const char *type = NULL;
#if PPC_ENABLE_JIT
	if (use_jit)
		type = "compile";
#endif
#if PPC_DECODE_CACHE
	if (!type)
		type = "predecode";
#endif
	if (type) {
		printf("### Statistics for block %s\n", type);
		printf("Total block %s count : %d\n", type, compile_count);
		uint32 emul_time = emul_end_time - emul_start_time;
		printf("Total emulation time : %.1f sec\n",
			   double(emul_time) / double(CLOCKS_PER_SEC));
		printf("Total %s time : %.1f sec (%.1f%%)\n", type,
			   double(compile_time) / double(CLOCKS_PER_SEC),
			   100.0 * double(compile_time) / double(emul_time));
		printf("\n");
	}
#endif

#if PPC_PROFILE_GENERIC_CALLS
	if (use_jit && ppc_refcount == 0) {
		uint64 total_generic_calls_count = 0;
		for (int i = 0; i < PPC_I(MAX); i++) {
			generic_calls_ids[i] = i;
			total_generic_calls_count += generic_calls_count[i];
		}
		qsort(generic_calls_ids, PPC_I(MAX), sizeof(int), generic_calls_compare);
		printf("Rank      Count Ratio Name\n");
		for (int i = 0; i < generic_calls_top_ten; i++) {
			uint32 mnemo = generic_calls_ids[i];
			uint32 count = generic_calls_count[mnemo];
			const instr_info_t *ii = powerpc_ii_table;
			while (ii->mnemo != mnemo)
				ii++;
			printf("%03d: %10lu %2.1f%% %s\n", i, count, 100.0*double(count)/double(total_generic_calls_count), ii->name);
		}
	}
#endif

#if PPC_PROFILE_REGS_USE
	printf("\n### Statistics for register usage\n");
	uint64 tot_reg_count = 0;
	for (int i = 0; i < 32; i++)
		tot_reg_count += reginfo[i].count;
	qsort(reginfo, 32, sizeof(register_info), register_info_compare);
	uint64 cum_reg_count = 0;
	for (int i = 0; i < 32; i++) {
		cum_reg_count += reginfo[i].count;
	    printf("r%-2d : %16llu %2.1f%% [%3.1f%%]\n",
			   reginfo[i].id, reginfo[i].count,
			   100.0*double(reginfo[i].count)/double(tot_reg_count),
			   100.0*double(cum_reg_count)/double(tot_reg_count));
	}
	delete[] reginfo;
#endif

	kill_decode_cache();

#if ENABLE_MON
	mon_exit();
#endif
}

void powerpc_cpu::dump_registers()
{
	fprintf(stderr, " r0 %08x   r1 %08x   r2 %08x   r3 %08x\n", gpr(0), gpr(1), gpr(2), gpr(3));
	fprintf(stderr, " r4 %08x   r5 %08x   r6 %08x   r7 %08x\n", gpr(4), gpr(5), gpr(6), gpr(7));
	fprintf(stderr, " r8 %08x   r9 %08x  r10 %08x  r11 %08x\n", gpr(8), gpr(9), gpr(10), gpr(11));
	fprintf(stderr, "r12 %08x  r13 %08x  r14 %08x  r15 %08x\n", gpr(12), gpr(13), gpr(14), gpr(15));
	fprintf(stderr, "r16 %08x  r17 %08x  r18 %08x  r19 %08x\n", gpr(16), gpr(17), gpr(18), gpr(19));
	fprintf(stderr, "r20 %08x  r21 %08x  r22 %08x  r23 %08x\n", gpr(20), gpr(21), gpr(22), gpr(23));
	fprintf(stderr, "r24 %08x  r25 %08x  r26 %08x  r27 %08x\n", gpr(24), gpr(25), gpr(26), gpr(27));
	fprintf(stderr, "r28 %08x  r29 %08x  r30 %08x  r31 %08x\n", gpr(28), gpr(29), gpr(30), gpr(31));
	fprintf(stderr, " f0 %02.5f   f1 %02.5f   f2 %02.5f   f3 %02.5f\n", fpr(0), fpr(1), fpr(2), fpr(3));
	fprintf(stderr, " f4 %02.5f   f5 %02.5f   f6 %02.5f   f7 %02.5f\n", fpr(4), fpr(5), fpr(6), fpr(7));
	fprintf(stderr, " f8 %02.5f   f9 %02.5f  f10 %02.5f  f11 %02.5f\n", fpr(8), fpr(9), fpr(10), fpr(11));
	fprintf(stderr, "f12 %02.5f  f13 %02.5f  f14 %02.5f  f15 %02.5f\n", fpr(12), fpr(13), fpr(14), fpr(15));
	fprintf(stderr, "f16 %02.5f  f17 %02.5f  f18 %02.5f  f19 %02.5f\n", fpr(16), fpr(17), fpr(18), fpr(19));
	fprintf(stderr, "f20 %02.5f  f21 %02.5f  f22 %02.5f  f23 %02.5f\n", fpr(20), fpr(21), fpr(22), fpr(23));
	fprintf(stderr, "f24 %02.5f  f25 %02.5f  f26 %02.5f  f27 %02.5f\n", fpr(24), fpr(25), fpr(26), fpr(27));
	fprintf(stderr, "f28 %02.5f  f29 %02.5f  f30 %02.5f  f31 %02.5f\n", fpr(28), fpr(29), fpr(30), fpr(31));
	fprintf(stderr, " lr %08x  ctr %08x   cr %08x  xer %08x\n", lr(), ctr(), cr().get(), xer().get());
	fprintf(stderr, " pc %08x fpscr %08x\n", pc(), fpscr());
	fflush(stderr);
}

void powerpc_cpu::dump_instruction(uint32 opcode)
{
	fprintf(stderr, "[%08x]-> %08x\n", pc(), opcode);
}

void powerpc_cpu::fake_dump_registers(uint32)
{
	dump_registers();
}

void powerpc_registers::interrupt_copy(powerpc_registers &oregs, powerpc_registers const &iregs)
{
	for (int i = 0; i < 32; i++) {
		oregs.gpr[i] = iregs.gpr[i];
		oregs.fpr[i] = iregs.fpr[i];
	}
	oregs.cr	= iregs.cr;
	oregs.fpscr	= iregs.fpscr;
	oregs.xer	= iregs.xer;
	oregs.lr	= iregs.lr;
	oregs.ctr	= iregs.ctr;
	oregs.pc	= iregs.pc;

	uint32 vrsave = iregs.vrsave;
	oregs.vrsave  = vrsave;
	if (vrsave) {
		for (int i = 31; i >= 0; i--) {
			if (vrsave & 1)
				oregs.vr[i] = iregs.vr[i];
			vrsave >>= 1;
		}
	}
}

bool powerpc_cpu::check_spcflags()
{
	if (spcflags().test(SPCFLAG_CPU_EXEC_RETURN)) {
		spcflags().clear(SPCFLAG_CPU_EXEC_RETURN);
		return false;
	}
#ifdef SHEEPSHAVER
	if (spcflags().test(SPCFLAG_CPU_HANDLE_INTERRUPT)) {
		spcflags().clear(SPCFLAG_CPU_HANDLE_INTERRUPT);
		static bool processing_interrupt = false;
		if (!processing_interrupt) {
			processing_interrupt = true;
			powerpc_registers r;
			powerpc_registers::interrupt_copy(r, regs());
#if PPC_ENABLE_UNICORN
			if (unicorn)
				unicorn->save_interrupted_state();
#endif
			HandleInterrupt(&r);
#if PPC_ENABLE_UNICORN
			if (unicorn)
				unicorn->restore_interrupted_state();
#endif
			powerpc_registers::interrupt_copy(regs(), r);
			processing_interrupt = false;
		}
	}
	if (spcflags().test(SPCFLAG_CPU_TRIGGER_INTERRUPT)) {
		spcflags().clear(SPCFLAG_CPU_TRIGGER_INTERRUPT);
		spcflags().set(SPCFLAG_CPU_HANDLE_INTERRUPT);
	}
#endif
	if (spcflags().test(SPCFLAG_CPU_ENTER_MON)) {
		spcflags().clear(SPCFLAG_CPU_ENTER_MON);
#if ENABLE_MON
		// Start up mon in real-mode
		const char *arg[] = {
			"mon",
#ifdef SHEEPSHAVER
			"-m",
#endif
			"-r",
			NULL
		};
		mon(sizeof(arg)/sizeof(arg[0]) - 1, arg);
#endif
	}
	return true;
}

#if DYNGEN_DIRECT_BLOCK_CHAINING
void * powerpc_cpu::call_compile_chain_block(powerpc_cpu * the_cpu, block_info *sbi)
{
	return the_cpu->compile_chain_block(sbi);
}

void * PF_CONVENTION powerpc_cpu::compile_chain_block(block_info *sbi)
{
	// Block index is stuffed into the source basic block pointer,
	// which is aligned at least on 4-byte boundaries
	const int n = ((uintptr)sbi) & 3;
	sbi = (block_info *)(((uintptr)sbi) & ~3L);

	const uint32 tpc = sbi->li[n].jmp_pc;
	block_info *tbi = my_block_cache.find(tpc);
	if (tbi == NULL)
		tbi = compile_block(tpc);
	assert(tbi && tbi->pc == tpc);

	dg_set_jmp_target(sbi->li[n].jmp_addr, tbi->entry_point);
	return tbi->entry_point;
}
#endif

void powerpc_cpu::execute(uint32 entry)
{
#if PPC_ENABLE_UNICORN
	if (unicorn) {
		execute_unicorn(entry);
		return;
	}
#endif
	bool invalidated_cache = false;
	pc() = entry;
#if PPC_EXECUTE_DUMP_STATE
	const bool dump_state = true;
#endif
	execute_depth++;
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	if (execute_depth == 1 || ((PPC_ENABLE_JIT || ppc_aarch64_reentrant_direct_jit_enabled()) && PPC_REENTRANT_JIT)) {
#if PPC_ENABLE_JIT
		if (use_jit) {
			block_info *bi = my_block_cache.find(pc());
			if (bi == NULL)
				bi = compile_block(pc());
			for (;;) {
				// Execute all cached blocks
				for (;;) {
					codegen.execute(bi->entry_point);

					if (!spcflags().empty()) {
						if (!check_spcflags())
							goto return_site;

						// Force redecoding if cache was invalidated
						if (spcflags().test(SPCFLAG_JIT_EXEC_RETURN)) {
							spcflags().clear(SPCFLAG_JIT_EXEC_RETURN);
							invalidated_cache = true;
							break;
						}
					}

					// Don't check for backward branches here as this
					// is now done by generated code. Besides, we will
					// get here if the fast cache lookup failed too.
					if ((bi = my_block_cache.find(pc())) == NULL)
						break;
				}

				// Compile new block
				bi = compile_block(pc());
			}
		}
#endif
#if PPC_DECODE_CACHE
		block_info *bi = my_block_cache.find(pc());
		if (bi != NULL)
			goto pdi_execute;
		for (;;) {
#if PPC_PROFILE_COMPILE_TIME
			compile_count++;
			clock_t start_time;
			start_time = clock();
#endif
			bi = my_block_cache.new_blockinfo();
			bi->init(pc());

			// Predecode a new block
			block_info::decode_info *di;
			const instr_info_t *ii;
			uint32 dpc;
			di = bi->di = decode_cache_p;
			dpc = pc() - 4;
			do {
				uint32 opcode = vm_read_memory_4(dpc += 4);
				ii = decode(opcode);
#if PPC_EXECUTE_DUMP_STATE
				if (dump_state) {
					di->opcode = opcode;
					di->execute = nv_mem_fun(&powerpc_cpu::dump_instruction);
					di++;
				}
#endif
#if PPC_FLIGHT_RECORDER
				if (is_logging()) {
					di->opcode = opcode;
					di->execute = nv_mem_fun(&powerpc_cpu::record_step);
					di++;
				}
#endif
				di->opcode = opcode;
				di->execute = ii->execute;
				di++;
#if PPC_EXECUTE_DUMP_STATE
				if (dump_state) {
					di->opcode = 0;
					di->execute = nv_mem_fun(&powerpc_cpu::fake_dump_registers);
					di++;
				}
#endif
				if (di >= decode_cache_end_p) {
					// Invalidate cache and move current code to start
					invalidate_cache();
					const int blocklen = di - bi->di;
					memmove(decode_cache_p, bi->di, blocklen * sizeof(*di));
					bi->di = decode_cache_p;
					di = bi->di + blocklen;
				}
			} while ((ii->cflow & CFLOW_END_BLOCK) == 0);
			bi->end_pc = dpc;
			bi->min_pc = dpc;
			bi->max_pc = entry;
			bi->size = di - bi->di;
			my_block_cache.add_to_cl_list(bi);
			my_block_cache.add_to_active_list(bi);
			decode_cache_p += bi->size;
#if PPC_PROFILE_COMPILE_TIME
			compile_time += (clock() - start_time);
#endif

			// Execute all cached blocks
		  pdi_execute:
			;
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
			int jit_skip_reason = PPC_JIT_SKIP_NONE;
			uint32 jit_skip_pc = pc();
			uint32 jit_skip_opcode = 0;
			/* AArch64 direct-codegen JIT: try to execute block natively.
			 *
			 * Gates in this block — see SheepShaver/docs/AARCH64_JIT_RUNTIME_CONTRACT.md:
			 *
			 * GATE 1 (SS_USE_JIT=0): OVERRIDE — JIT enabled by default; set SS_USE_JIT=0
			 *   to force interpreter execution for diagnostics or regressions.
			 *
			 * GATE 2 (jblk.complete): CONTAINMENT — only execute fully compiled blocks.
			 *   Status: overcautious; partial blocks are safe (truncation epilogue writes
			 *   valid PPCR_PC and interpreter can resume from there). Candidate for removal.
			 *   Expiry: remove when parity harness confirms partial-block execution is correct.
			 *
			 * GATE 3 (PC range check): DIAGNOSTIC — detects JIT compiler bugs that produce
			 *   out-of-range PCs.  Should log before skipping, not silently continue.
			 */
			{
				static bool jit_init_done = false;
				static const char *jit_env = getenv("SS_USE_JIT");
				static bool jit_enabled = !(jit_env && jit_env[0] == '0' && jit_env[1] == '\0') &&
					ppc_aarch64_jit_address_space_ok();
				if (!jit_enabled) {
					jit_skip_reason = PPC_JIT_SKIP_DISABLED;
					jit_skip_pc = pc();
					jit_skip_opcode = vm_read_memory_4(jit_skip_pc);
					goto skip_jit;
				} /* GATE 1: SS_USE_JIT=0 diagnostic override */
				if (!jit_init_done) { ppc_jit_aarch64_init(8192); jit_init_done = true; } /* 8MB JIT code cache */
				ppc_jit_block jblk;
				const uint8 *jit_region_base = NULL;
				uint32 jit_region_guest_base = 0;
				size_t jit_region_size = 0;
				jit_skip_pc = pc();
				bool jit_region_ok = ppc_jit_aarch64_region_for_pc(jit_skip_pc, &jit_region_base, &jit_region_guest_base, &jit_region_size);
				if (jit_region_ok)
					jit_skip_opcode = vm_read_memory_4(jit_skip_pc);
				bool jit_compiled = jit_region_ok && ppc_jit_aarch64_compile(jit_skip_pc, jit_region_base, jit_region_guest_base, jit_region_size, &jblk);
				if (!jit_region_ok)
					jit_skip_reason = PPC_JIT_SKIP_REGION;
				else if (!jit_compiled) {
					jit_skip_reason = PPC_JIT_SKIP_COMPILE_FALSE;
					ppc_jit_failprobe_note(jit_skip_pc, jit_skip_pc, "compile_false");
				}
				/* GATE 2 removed: partial native blocks are safe. The direct compiler
				 * emits an epilogue that stores the first uncompiled PPC PC before
				 * returning, so executing a supported prefix is more faithful than
				 * falling back to the interpreter at the block entry. */
				if (jit_compiled) {
					/* Match dyngen block-entry semantics: drain pending special flags
					 * before direct native block execution.  trigger_interrupt() first
					 * raises TRIGGER, then check_spcflags() converts it to HANDLE, so
					 * loop a few times to process the resulting interrupt immediately. */
					for (int spc_iter = 0; !spcflags().empty() && spc_iter < 4; spc_iter++)
						if (!check_spcflags()) goto return_site;
					ppc_jit_entry_fn fn = (ppc_jit_entry_fn)(void*)jblk.code;
					if (ppc_jit_ratio_enabled()) {
						ppc_jit_ratio_native_dispatches++;
						if (jblk.n_insns > 0) ppc_jit_ratio_native_insns_known += (uint64)jblk.n_insns;
						ppc_jit_ratio_report(false);
					}
#ifdef SS_JIT_BENCH_CENSUS
					ppc_jit_bench_exec.native_dispatches++;
					if (jblk.n_insns > 0) ppc_jit_bench_exec.native_retired += (uint64)jblk.n_insns;
#endif
					if (jblk.n_insns > 0) ppc_jit_native_hist_record(jblk.ppc_start_pc, vm_read_memory_4(jblk.ppc_start_pc), (uint32)jblk.n_insns);
					fn((void*)regs_ptr());
					if (!jblk.complete)
						ppc_jit_failprobe_note(jblk.ppc_start_pc, pc(), "partial_return");
				  pdi_jit_post:
					/* GATE 3: PC range check.
					 * A returned native block has already committed its supported prefix
					 * and PPCR_PC is the next guest instruction. Replaying the block leader
					 * would duplicate those architectural effects. Preserve that successor,
					 * invalidate the offending native block, and enter the uncached
					 * one-instruction interpreter loop, which decodes directly from pc().
					 * Contract: see AARCH64_JIT_RUNTIME_CONTRACT.md */
					uint32 jit_pc = pc();
					uint32 ram_start = RAMBase ? RAMBase : (uint32)(uintptr_t)RAMBaseHost;
					bool jit_pc_mapped = ppc_jit_contains32(jit_pc, ram_start, RAMSize, 1) ||
					                     ppc_jit_contains32(jit_pc, ROMBase, ROMEnd ? ((uint64)ROMEnd - ROMBase) : 0x500000, 1) ||
#ifdef SHEEPSHAVER
					                     ppc_jit_contains32(jit_pc, SheepMem::Base(), (uint64)SheepMem::End() - SheepMem::Base(), 1) ||
#endif
					                     false;
					if (!jit_pc_mapped) {
						jit_skip_reason = PPC_JIT_SKIP_GATE3;
						jit_skip_pc = jblk.ppc_start_pc;
						jit_skip_opcode = vm_read_memory_4(jit_skip_pc);
						if (ppc_jit_ratio_enabled()) {
							ppc_jit_ratio_gate3_entries++;
							ppc_jit_ratio_skip_jit_entries++;
						}
						fprintf(stderr, "PPC-JIT-A64: GATE3: out-of-range PC 0x%08x after block at 0x%08x — handing to interpreter\n",
						        jit_pc, jblk.ppc_start_pc);
						/* Preserve the JIT-produced successor: the native prefix is committed. */
						ppc_jit_aarch64_invalidate_pc(jblk.ppc_start_pc);
						ppc_jit_skip_hist_record(jit_skip_pc, jit_skip_opcode, jit_skip_reason);
						goto do_interpret;
					}
					if (!spcflags().empty()) {
						if (!check_spcflags()) goto return_site;
					}
					/* Progress-based idle detection: when a block branches
					 * back to itself, compare architectural state across
					 * iterations.  A bounded loop (memcpy, page-table init)
					 * changes ≥1 register every iteration → progress → run.
					 * A true idle poll (e.g. 18310dd0: lwz/cmpwi/beq/b .−)
					 * leaves ALL registers unchanged → no progress → yield.
					 *
					 * This replaces the fixed-threshold counter that falsely
					 * tripped on legitimate bounded loops. */
					{
						static uint32 idle_snap_pc = 0;
						static uint32 idle_snap_gpr[32];
						static uint32 idle_snap_cr = 0;
						static uint32 idle_snap_ctr = 0;
						static int no_progress_count = 0;

						if (pc() == jblk.ppc_start_pc) {
							/* Self-loop detected — check progress */
							if (jblk.ppc_start_pc == idle_snap_pc) {
								/* Compare current GPRs/CR/CTR to snapshot */
								bool progress = false;
								for (int i = 0; i < 32; i++) {
									if (gpr(i) != idle_snap_gpr[i]) {
										progress = true;
										break;
									}
								}
								if (!progress && regs().cr.get() != idle_snap_cr)
									progress = true;
								if (!progress && regs().ctr != idle_snap_ctr)
									progress = true;

								if (progress) {
									no_progress_count = 0;
								} else {
									if (++no_progress_count >= 8) {
										no_progress_count = 0;
										/* True idle: no architectural change
										 * across 8 consecutive iterations.
										 * Yield until tick thread fires. */
										for (int w = 0; w < 200 && spcflags().empty(); w++)
											usleep(100);
										if (!spcflags().empty()) {
											if (!check_spcflags()) goto return_site;
										}
									}
								}
							} else {
								/* New self-loop target — take initial snapshot */
								idle_snap_pc = jblk.ppc_start_pc;
								no_progress_count = 0;
							}
							/* Always update snapshot for next comparison */
							for (int i = 0; i < 32; i++)
								idle_snap_gpr[i] = gpr(i);
							idle_snap_cr = regs().cr.get();
							idle_snap_ctr = regs().ctr;
						} else {
							/* Not a self-loop — reset */
							idle_snap_pc = 0;
							no_progress_count = 0;
						}
					}
					/* Fast dispatch: if next PC is already in JIT cache, stay in the
					 * JIT loop without touching the interpreter block cache.
					 * This eliminates my_block_cache.find() + pdi_execute overhead for
					 * hot block-to-block transitions where both blocks are JIT-compiled. */
					jit_region_ok = ppc_jit_aarch64_region_for_pc(pc(), &jit_region_base, &jit_region_guest_base, &jit_region_size);
					if (jit_region_ok && ppc_jit_aarch64_compile(pc(), jit_region_base, jit_region_guest_base, jit_region_size, &jblk)) {
						for (int spc_iter = 0; !spcflags().empty() && spc_iter < 4; spc_iter++)
							if (!check_spcflags()) goto return_site;
						fn = (ppc_jit_entry_fn)(void*)jblk.code;
						if (ppc_jit_ratio_enabled()) {
							ppc_jit_ratio_native_dispatches++;
							if (jblk.n_insns > 0) ppc_jit_ratio_native_insns_known += (uint64)jblk.n_insns;
							ppc_jit_ratio_report(false);
						}
#ifdef SS_JIT_BENCH_CENSUS
						ppc_jit_bench_exec.native_dispatches++;
						if (jblk.n_insns > 0) ppc_jit_bench_exec.native_retired += (uint64)jblk.n_insns;
#endif
						if (jblk.n_insns > 0) ppc_jit_native_hist_record(jblk.ppc_start_pc, vm_read_memory_4(jblk.ppc_start_pc), (uint32)jblk.n_insns);
						fn((void*)regs_ptr());
						if (!jblk.complete)
							ppc_jit_failprobe_note(jblk.ppc_start_pc, pc(), "partial_return_fast");
						goto pdi_jit_post;
					}
					bi = my_block_cache.find(pc());
					if (bi) goto pdi_execute;
					continue;
				}
			}
#endif
		  skip_jit:
			ppc_jit_skip_hist_record(jit_skip_pc, jit_skip_opcode, jit_skip_reason);
			if (ppc_jit_ratio_enabled()) ppc_jit_ratio_skip_jit_entries++;
			for (;;) {
				if (ppc_jit_ratio_enabled()) {
					ppc_jit_ratio_exec_normal_blocks++;
					ppc_jit_ratio_exec_normal_insns += (uint64)bi->size;
					ppc_jit_ratio_report(false);
				}
#ifdef SS_JIT_BENCH_CENSUS
				ppc_jit_bench_exec.interpreter_blocks++;
				ppc_jit_bench_exec.interpreter_retired += (uint64)bi->size;
#endif
				if (bi->di && bi->size > 0) {
					ppc_jit_failprobe_exec_entry(bi->pc, bi->di[0].opcode, (uint32)bi->size);
					ppc_jit_hist_record_exec_normal(bi->pc, bi->di[0].opcode, (uint32)bi->size);
				}
				const int r = bi->size % 4;
				di = bi->di + r;
				int n = (bi->size + 3) / 4;
				switch (r) {
				case 0: do {
						di += 4;
						di[-4].execute(this, di[-4].opcode);
				case 3: di[-3].execute(this, di[-3].opcode);
				case 2: di[-2].execute(this, di[-2].opcode);
				case 1: di[-1].execute(this, di[-1].opcode);
					} while (--n > 0);
				}

				if (!spcflags().empty()) {
					if (!check_spcflags())
						goto return_site;

					// Force redecoding if cache was invalidated
					if (spcflags().test(SPCFLAG_JIT_EXEC_RETURN)) {
						spcflags().clear(SPCFLAG_JIT_EXEC_RETURN);
						invalidated_cache = true;
						break;
					}
				}

				if (bi->pc != pc()) {
					bi = my_block_cache.find(pc());
					if (bi == NULL)
						break;
					goto pdi_execute;
				}
			}
		}
#else
		goto do_interpret;
#endif
	}
#endif
  do_interpret:
	for (;;) {
		uint32 opcode = vm_read_memory_4(pc());
		const instr_info_t *ii = decode(opcode);
#if PPC_EXECUTE_DUMP_STATE
		if (dump_state)
			dump_instruction(opcode);
#endif
#if PPC_FLIGHT_RECORDER
		if (is_logging())
			record_step(opcode);
#endif
#ifdef __MINGW32__
		assert(ii->execute.default_call_conv_ptr() != 0);
#else
		assert(ii->execute.ptr() != 0);
#endif
		if (ppc_jit_ratio_enabled()) {
			ppc_jit_ratio_exec_normal_insns++;
			ppc_jit_ratio_report(false);
		}
#ifdef SS_JIT_BENCH_CENSUS
		ppc_jit_bench_exec.interpreter_retired++;
#endif
		ppc_jit_failprobe_exec_entry(pc(), opcode, 1);
		ppc_jit_hist_record_exec_normal(pc(), opcode, 1);
		ii->execute(this, opcode);
#if PPC_EXECUTE_DUMP_STATE
		if (dump_state)
			dump_registers();
#endif
		if (!spcflags().empty() && !check_spcflags())
			goto return_site;
	}
  return_site:
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	ppc_jit_ratio_report(true);
	ppc_jit_hist_report(true);
	ppc_jit_native_hist_report(true);
	ppc_jit_skip_hist_report(true);
#endif
	// Tell upper level we invalidated cache?
	if (invalidated_cache)
		spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
	--execute_depth;
}

void powerpc_cpu::execute()
{
	execute(pc());
}

#if PPC_ENABLE_UNICORN
/*
 *  Unicorn engine backend
 *
 *  Guest code runs inside Unicorn until it needs the host: an instruction
 *  that traps (SheepShaver's opcode-6 EMUL_OP/NATIVE_OP/EXEC_RETURN
 *  instructions, privileged instructions, anything Unicorn does not
 *  implement), a store to a read-only range, or an asynchronous stop for
 *  an interrupt.  The trapping instruction is then executed by this
 *  interpreter on the kpx register file, so SheepShaver semantics are
 *  identical to the interpreter path.  Nested execute() calls (interrupts,
 *  Execute68k, call_macos) simply start another Unicorn run.
 */

void powerpc_cpu::enable_unicorn(ppc_unicorn *engine)
{
	delete unicorn;
	unicorn = engine;
}

void powerpc_cpu::unicorn_request_stop()
{
	ppc_unicorn *u = unicorn;
	if (u)
		u->request_stop();
}

void powerpc_cpu::unicorn_invalidate_range(uint32 start, uint32 end)
{
	if (unicorn)
		unicorn->invalidate_range(start, end);
}

void powerpc_cpu::unicorn_dump_history()
{
	if (unicorn)
		unicorn->dump_history();
}

/*
 *  Differential self test: execute random instructions with the
 *  interpreter and with Unicorn from identical state and compare.
 */

static uint32 st_rng_state;
static uint32 st_rand()
{
	st_rng_state ^= st_rng_state << 13;
	st_rng_state ^= st_rng_state >> 17;
	st_rng_state ^= st_rng_state << 5;
	return st_rng_state;
}

static uint32 st_value()
{
	static const uint32 specials[] = {
		0, 1, 2, 0xffffffff, 0x80000000, 0x7fffffff, 0x0000ffff, 0xffff0000,
		0x00008000, 0xffff8000, 0x00000020, 0x0000001f, 0x12345678, 0xfffffffe
	};
	switch (st_rand() % 4) {
	case 0: return specials[st_rand() % (sizeof(specials) / sizeof(specials[0]))];
	case 1: return st_rand() & 0xff;
	default: return st_rand();
	}
}

static uint64 st_fp_value()
{
	static const uint64 specials[] = {
		0x0000000000000000ULL, 0x8000000000000000ULL, 0x3ff0000000000000ULL, 0xbff0000000000000ULL,
		0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000000ULL, 0x7ff4000000000000ULL,
		0x0000000000000001ULL, 0x000fffffffffffffULL, 0x4330000000000000ULL, 0x41dfffffffc00000ULL,
		0xc1e0000000000000ULL, 0x3fe0000000000000ULL, 0x4008000000000000ULL, 0x47efffffe0000000ULL,
		0x3810000000000000ULL, 0x36a0000000000000ULL
	};
	switch (st_rand() % 3) {
	case 0: return specials[st_rand() % (sizeof(specials) / sizeof(specials[0]))];
	case 1: {	// "nice" doubles
		union { double d; uint64 j; } u;
		u.d = (double)(int32)st_rand() / (double)((st_rand() & 0xffff) + 1);
		return u.j;
	}
	default: return ((uint64)st_rand() << 32) | st_rand();
	}
}

// Build a random instruction.  Memory forms get their base register pointed
// into the data buffer by the caller (*mem_ra / *mem_rb).
static uint32 st_make_insn(int *mem_ra, int *mem_rb, bool *mem_update, bool *skip)
{
	*mem_ra = *mem_rb = -1;
	*mem_update = false;
	*skip = false;
	const uint32 rd = st_rand() & 31, ra = st_rand() & 31, rb = st_rand() & 31, rc = st_rand() & 31;
	const uint32 rcbit = st_rand() & 1, oe = (st_rand() & 1) << 10;
	const uint32 imm = st_rand() & 0xffff;
	#define D_FORM(op) (((op) << 26) | (rd << 21) | (ra << 16) | imm)
	#define X_FORM(xo) ((31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | ((xo) << 1) | rcbit)
	#define XO_FORM(xo) ((31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | oe | ((xo) << 1) | rcbit)
	static const int xo_arith[] = { 266, 10, 138, 234, 202, 40, 8, 136, 232, 200, 104, 235, 75, 11, 491, 459 };
	static const int x_logic[] = { 28, 60, 444, 412, 316, 476, 124, 284, 24, 536, 792, 824, 26, 954, 922 };
	static const int d_alu[] = { 14, 15, 12, 13, 8, 7, 28, 29, 24, 25, 26, 27 };
	static const int d_load[] = { 32, 33, 34, 35, 40, 41, 42, 43, 48, 49, 50, 51 };
	static const int d_store[] = { 36, 37, 38, 39, 44, 45, 52, 53, 54, 55 };
	static const int x_mem[] = { 23, 55, 87, 119, 151, 183, 215, 247, 279, 311, 343, 375, 407, 439,
		534, 662, 790, 918, 535, 599, 663, 727, 983, 1014, 567, 631, 695, 759 };
	static const int fp_a[] = { 21, 20, 25, 18, 29, 28, 31, 30, 23 };
	static const int fp_x[] = { 14, 15, 12, 40, 264, 136, 72, 0, 32, 583, 711, 134, 70, 38, 64 };
	static const int cr_ops[] = { 257, 449, 193, 225, 33, 289, 129, 417, 0 };
	#define PICK(a) (a[st_rand() % (sizeof(a) / sizeof(a[0]))])
	switch (st_rand() % 14) {
	case 0: case 1: return XO_FORM(PICK(xo_arith));
	case 2: case 3: {
		int xo = PICK(x_logic);
		// X-form logical ops: rS in rd field, rA destination
		return X_FORM(xo);
	}
	case 4: return D_FORM(PICK(d_alu));
	case 5: {	// rotates
		static const int rot[] = { 20, 21, 23 };
		return (PICK(rot) << 26) | (rd << 21) | (ra << 16) | (rb << 11) | ((st_rand() & 31) << 6) | ((st_rand() & 31) << 1) | rcbit;
	}
	case 6: {	// compares
		const uint32 crf = (st_rand() & 7) << 23;
		switch (st_rand() % 4) {
		case 0: return (11u << 26) | crf | (ra << 16) | imm;
		case 1: return (10u << 26) | crf | (ra << 16) | imm;
		case 2: return (31u << 26) | crf | (ra << 16) | (rb << 11) | (0 << 1);
		default: return (31u << 26) | crf | (ra << 16) | (rb << 11) | (32 << 1);
		}
	}
	case 7: {	// CR / SPR moves
		switch (st_rand() % 6) {
		case 0: return (31u << 26) | (rd << 21) | (19 << 1);								// mfcr
		case 1: return (31u << 26) | (rd << 21) | ((st_rand() & 0xff) << 12) | (144 << 1);	// mtcrf
		case 2: return (31u << 26) | ((st_rand() & 7) << 23) | (512 << 1);					// mcrxr
		case 3: { static const int spr[] = { 1, 8, 9 }; int s = PICK(spr);
			return (31u << 26) | (rd << 21) | ((s & 31) << 16) | ((s >> 5) << 11) | (339 << 1); }	// mfspr
		case 4: { static const int spr[] = { 1, 8, 9 }; int s = PICK(spr);
			return (31u << 26) | (rd << 21) | ((s & 31) << 16) | ((s >> 5) << 11) | (467 << 1); }	// mtspr
		default: return (19u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (PICK(cr_ops) << 1);	// CR logical
		}
	}
	case 8: case 9: {	// D-form loads/stores
		const int op = (st_rand() & 1) ? PICK(d_load) : PICK(d_store);
		const uint32 a = (ra == 0) ? 3 : ra;
		*mem_ra = a;
		*mem_update = (op & 1) && op != 42 + 0 ? ((op == 33 || op == 35 || op == 41 || op == 43 || op == 49 || op == 51 || op == 37 || op == 39 || op == 45 || op == 53 || op == 55)) : false;
		const bool is_load = (op >= 32 && op <= 35) || (op >= 40 && op <= 43);
		if (*mem_update && is_load && rd == a)
			*skip = true;	// invalid form
		return (op << 26) | (rd << 21) | (a << 16) | ((st_rand() % 512) & ~0u);
	}
	case 10: {	// X-form loads/stores
		const int xo = PICK(x_mem);
		const uint32 a = (ra == 0) ? 3 : ra;
		uint32 b = rb;
		if (b == a) b = (a + 1) & 31;
		if (b == 0) b = 4;
		*mem_ra = a;
		*mem_rb = b;
		*mem_update = (xo == 55 || xo == 119 || xo == 183 || xo == 247 || xo == 311 || xo == 375 || xo == 439 ||
					   xo == 567 || xo == 631 || xo == 695 || xo == 759);
		const bool is_load = (xo == 55 || xo == 119 || xo == 311 || xo == 375);
		if ((*mem_update && is_load && rd == a) || rd == b)
			*skip = true;
		return (31u << 26) | (rd << 21) | (a << 16) | (b << 11) | (xo << 1);
	}
	case 11: {	// lmw / stmw / lswi / stswi / lwarx+stwcx.
		const uint32 a = (ra == 0) ? 3 : ra;
		*mem_ra = a;
		switch (st_rand() % 4) {
		case 0: { uint32 d = 20 + (st_rand() % 12); if (a >= d) *skip = true; return (46u << 26) | (d << 21) | (a << 16) | (st_rand() % 256); }
		case 1: { uint32 d = 20 + (st_rand() % 12); return (47u << 26) | (d << 21) | (a << 16) | (st_rand() % 256); }
		case 2: { uint32 nb = st_rand() & 31; uint32 d = 24 + (st_rand() % 4); uint32 nr = ((nb ? nb : 32) + 3) / 4;
			for (uint32 i = 0; i < nr; i++) if (((d + i) & 31) == a) *skip = true;
			return (31u << 26) | (d << 21) | (a << 16) | (nb << 11) | (597 << 1); }
		default: { uint32 nb = st_rand() & 31; return (31u << 26) | (rd << 21) | (a << 16) | (nb << 11) | (725 << 1); }
		}
	}
	case 12: {	// FP A-form (double and single)
		const uint32 op = (st_rand() & 1) ? 63 : 59;
		const int xo = PICK(fp_a);
		if (op == 59 && xo == 23) return (63u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (rc << 6) | (23 << 1) | rcbit;
		return (op << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (rc << 6) | (xo << 1) | rcbit;
	}
	default: {	// FP X-form
		const int xo = PICK(fp_x);
		if (xo == 0 || xo == 32 || xo == 64)	// fcmpu/fcmpo/mcrfs: crfD
			return (63u << 26) | ((st_rand() & 7) << 23) | (ra << 16) | (rb << 11) | (xo << 1);
		if (xo == 711)	// mtfsf
			return (63u << 26) | ((st_rand() & 0xff) << 17) | (rb << 11) | (xo << 1) | rcbit;
		if (xo == 134)	// mtfsfi
			return (63u << 26) | ((st_rand() & 7) << 23) | ((st_rand() & 15) << 12) | (xo << 1) | rcbit;
		if (xo == 70 || xo == 38) {	// mtfsb0/1 (not FEX/VX)
			uint32 bt = st_rand() & 31; if (bt == 1 || bt == 2) bt = 24;
			return (63u << 26) | (bt << 21) | (xo << 1) | rcbit;
		}
		return (63u << 26) | (rd << 21) | (rb << 11) | (xo << 1) | rcbit;
	}
	}
	#undef PICK
	#undef D_FORM
	#undef X_FORM
	#undef XO_FORM
}

int powerpc_cpu::unicorn_selftest(uint32 code_addr, uint32 data_addr, int count, uint32 seed)
{
	if (unicorn == NULL)
		return -1;
	ppc_unicorn *engine = unicorn;
	st_rng_state = seed ? seed : 0x2545f491;
	const uint32 DATA_SIZE = 4096;
	static uint8 data0[4096], data_i[4096], data_u[4096];
	int failures = 0, ran = 0;
	const bool verbose = getenv("SS_UNICORN_SELFTEST_VERBOSE") != NULL;
	const bool check_fpscr = getenv("SS_UNICORN_SELFTEST_NOFPSCR") == NULL;

	for (int t = 0; t < count; t++) {
		int mem_ra, mem_rb;
		bool mem_update, skip;
		const uint32 insn = st_make_insn(&mem_ra, &mem_rb, &mem_update, &skip);
		if (skip)
			continue;

		// Initial state
		powerpc_registers s0 = regs();
		for (int i = 0; i < 32; i++) {
			s0.gpr[i] = st_value();
			s0.fpr[i].j = st_fp_value();
		}
		if (mem_ra >= 0)
			s0.gpr[mem_ra] = data_addr + 1024 + ((st_rand() % 256) & ~3u);
		if (mem_rb >= 0)
			s0.gpr[mem_rb] = (st_rand() % 512) & ~3u;
		// lswx/stswx byte count etc.
		s0.cr.set(st_rand());
		s0.xer.set(st_rand() & 0xe000007f);
		s0.lr = st_value();
		s0.ctr = st_value();
		s0.fpscr = (st_rand() & 1) ? (st_rand() & 3) : 0;	// rounding mode only
		for (uint32 i = 0; i < DATA_SIZE; i++)
			data0[i] = st_rand() & 0xff;

		vm_write_memory_4(code_addr, insn);
		vm_write_memory_4(code_addr + 4, 0x18000001);	// EXEC_RETURN
		invalidate_cache_range(code_addr, code_addr + 8);	// interpreter decode cache and Unicorn
		spcflags().clear(SPCFLAG_JIT_EXEC_RETURN);

		// Interpreter
		memcpy(vm_do_get_real_address(data_addr), data0, DATA_SIZE);
		regs() = s0;
		unicorn = NULL;
		execute(code_addr);
		unicorn = engine;
		powerpc_registers ri = regs();
		memcpy(data_i, vm_do_get_real_address(data_addr), DATA_SIZE);

		// Unicorn
		memcpy(vm_do_get_real_address(data_addr), data0, DATA_SIZE);
		regs() = s0;
		execute(code_addr);
		powerpc_registers ru = regs();
		memcpy(data_u, vm_do_get_real_address(data_addr), DATA_SIZE);
		ran++;

		// Compare
		char diff[1024];
		int len = 0;
		#define ADD(...) len += snprintf(diff + len, sizeof(diff) - len > 0 ? sizeof(diff) - len : 0, __VA_ARGS__)
		for (int i = 0; i < 32; i++)
			if (ri.gpr[i] != ru.gpr[i]) ADD(" r%d:%08x/%08x", i, ri.gpr[i], ru.gpr[i]);
		for (int i = 0; i < 32; i++)
			if (ri.fpr[i].j != ru.fpr[i].j) ADD(" f%d:%016llx/%016llx", i, (unsigned long long)ri.fpr[i].j, (unsigned long long)ru.fpr[i].j);
		if (ri.cr.get() != ru.cr.get()) ADD(" cr:%08x/%08x", ri.cr.get(), ru.cr.get());
		if (ri.xer.get() != ru.xer.get()) ADD(" xer:%08x/%08x", ri.xer.get(), ru.xer.get());
		if (ri.lr != ru.lr) ADD(" lr:%08x/%08x", ri.lr, ru.lr);
		if (ri.ctr != ru.ctr) ADD(" ctr:%08x/%08x", ri.ctr, ru.ctr);
		if (ri.pc != ru.pc) ADD(" pc:%08x/%08x", ri.pc, ru.pc);
		if (check_fpscr && ri.fpscr != ru.fpscr) ADD(" fpscr:%08x/%08x", ri.fpscr, ru.fpscr);
		for (uint32 i = 0; i < DATA_SIZE; i++)
			if (data_i[i] != data_u[i]) { ADD(" mem+%u:%02x/%02x", i, data_i[i], data_u[i]); break; }
		#undef ADD
		if (len > 0) {
			failures++;
			fprintf(stderr, "SELFTEST MISMATCH insn %08x (interp/unicorn):%s [rS/frS=%08x/%016llx]\n", insn, diff,
					s0.gpr[(insn >> 21) & 31], (unsigned long long)s0.fpr[(insn >> 21) & 31].j);
		}
		else if (verbose)
			fprintf(stderr, "SELFTEST ok %08x\n", insn);
	}
	fprintf(stderr, "SELFTEST %d instructions, %d mismatches\n", ran, failures);
	return failures;
}

void powerpc_cpu::execute_one_interpreted()
{
	const uint32 opcode = vm_read_memory_4(pc());
	const instr_info_t *ii = decode(opcode);
#if PPC_FLIGHT_RECORDER
	if (is_logging())
		record_step(opcode);
#endif
	ii->execute(this, opcode);
}

// Skip a store that targeted a read-only guest range (e.g. ROM), like the
// SIGSEGV handler does for the interpreter: the memory access is dropped
// but the base register update of update-form stores still happens.
void powerpc_cpu::skip_protected_store()
{
	const uint32 opcode = vm_read_memory_4(pc());
	const uint32 ra = (opcode >> 16) & 0x1f;
	const uint32 rb = (opcode >> 11) & 0x1f;
	switch (opcode >> 26) {
	case 37:	// stwu
	case 39:	// stbu
	case 45:	// sthu
	case 53:	// stfsu
	case 55:	// stfdu
		gpr(ra) += (int32)(int16)(opcode & 0xffff);
		break;
	case 31:
		switch ((opcode >> 1) & 0x3ff) {
		case 183:	// stwux
		case 247:	// stbux
		case 439:	// sthux
		case 695:	// stfsux
		case 759:	// stfdux
			gpr(ra) += gpr(rb);
			break;
		}
		break;
	}
	pc() += 4;
}

void powerpc_cpu::execute_unicorn(uint32 entry)
{
	pc() = entry;
	execute_depth++;

	for (;;) {
		ppc_unicorn::exit_info ei;
		unicorn->load_state(regs());
		unicorn->run(ei);
		unicorn->store_state(regs());

		switch (ei.reason) {
		case ppc_unicorn::EXIT_ASYNC:
			break;

		case ppc_unicorn::EXIT_EXCEPTION:
			pc() = ei.insn_pc;
			execute_one_interpreted();
			break;

		case ppc_unicorn::EXIT_WRITE_PROTECT:
			pc() = ei.insn_pc;
			skip_protected_store();
			break;

		case ppc_unicorn::EXIT_ERROR:
			// Typically an access to a guest address without host memory.
			// Let the interpreter execute the instruction: it either succeeds
			// or faults on the host, where the SIGSEGV handler applies the
			// usual SheepShaver policy (skip, or report and quit)
			pc() = ei.insn_pc;
			if (ei.restore_gpr >= 0)
				gpr(ei.restore_gpr) = ei.restore_val;
			{
				static const bool debug = getenv("SS_UNICORN_DEBUG") != NULL;
				if (debug)
					fprintf(stderr, "Unicorn PPC: %s at pc %08x (address %08x), using interpreter\n",
							ppc_unicorn::error_string(ei.err), ei.insn_pc, ei.fault_addr);
			}
			execute_one_interpreted();
			break;
		}

		if (!spcflags().empty()) {
			spcflags().clear(SPCFLAG_JIT_EXEC_RETURN);
			if (!check_spcflags())
				break;
			// check_spcflags() turns TRIGGER into HANDLE; handle it right away
			// instead of after the next Unicorn run
			if (spcflags().test(SPCFLAG_CPU_HANDLE_INTERRUPT) && !check_spcflags())
				break;
		}
	}

	execute_depth--;
}
#endif

void powerpc_cpu::init_decode_cache()
{
#if PPC_DECODE_CACHE
	decode_cache = (block_info::decode_info *)vm_acquire(DECODE_CACHE_SIZE);
	if (decode_cache == VM_MAP_FAILED) {
		fprintf(stderr, "powerpc_cpu: Could not allocate decode cache\n");
		abort();
	}

	D(bug("powerpc_cpu: Allocated decode cache: %d KB at %p\n", DECODE_CACHE_SIZE / 1024, decode_cache));
	decode_cache_p = decode_cache;
	decode_cache_end_p = decode_cache + DECODE_CACHE_MAX_ENTRIES;
#if FLIGHT_RECORDER
	// Leave enough room to last call to record_step()
	decode_cache_end_p -= 2;
#endif
#if PPC_EXECUTE_DUMP_STATE
	// Leave enough room to last calls to dump state functions
	decode_cache_end_p -= 2;
#endif
#endif
}

void powerpc_cpu::kill_decode_cache()
{
#if PPC_DECODE_CACHE
	vm_release(decode_cache, DECODE_CACHE_SIZE);
#endif
}

void powerpc_cpu::invalidate_cache()
{
	D(bug("Invalidate all cache blocks\n"));
#if PPC_ENABLE_UNICORN
	if (unicorn)
		unicorn->invalidate_all();
#endif
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
	my_block_cache.clear();
	my_block_cache.initialize();
	spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
#endif
#if PPC_ENABLE_JIT
	codegen.invalidate_cache();
#endif
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	ppc_jit_aarch64_flush();
#endif
#if PPC_DECODE_CACHE
	decode_cache_p = decode_cache;
#endif
}

void powerpc_block_info::invalidate()
{
#if PPC_DECODE_CACHE
	// Don't do anything if this is a predecoded block
	if (di)
		return;
#endif
#if DYNGEN_DIRECT_BLOCK_CHAINING
	for (int i = 0; i < MAX_TARGETS; i++) {
		link_info * const tli = &li[i];
		uint32 tpc = tli->jmp_pc;
		// For any jump within page boundaries, reset the jump address
		// to the target block resolver (trampoline)
		if (tpc != INVALID_PC && ((tpc ^ pc) >> 12) == 0)
			dg_set_jmp_target(tli->jmp_addr, tli->jmp_resolve_addr);
	}
#endif
}

void powerpc_cpu::invalidate_cache_range(uintptr start, uintptr end)
{
	D(bug("Invalidate cache block [%08x - %08x]\n", start, end));
#if PPC_ENABLE_UNICORN
	if (unicorn)
		unicorn->invalidate_range((uint32)start, (uint32)end);
#endif
#if PPC_DECODE_CACHE || PPC_ENABLE_JIT
#if DYNGEN_DIRECT_BLOCK_CHAINING
	if (use_jit) {
		// Invalidate on page boundaries
		start &= -4096;
		end = (end + 4095) & -4096;
		D(bug("    at page boundaries [%08x - %08x]\n", start, end));
	}
#endif
	spcflags().set(SPCFLAG_JIT_EXEC_RETURN);
	my_block_cache.clear_range(start, end);
#if defined(__aarch64__) && defined(USE_AARCH64_JIT)
	ppc_jit_aarch64_flush();
#endif
#endif
}
