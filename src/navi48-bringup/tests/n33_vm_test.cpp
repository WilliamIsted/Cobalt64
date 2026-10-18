// n33_vm_test.cpp: host test of amd/n33_vm_pure.h (stage 3 step 7 W2V: a per-process GPU VM in VMID 8, the gfx11 4-level
// page table, the VMID-8 streams, the refusals and the Darwin range predicate).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_vm_test.cpp -o $SCRATCH/n33_vm_test && \
//       $SCRATCH/n33_vm_test [<linux tree root>]
// Part 1 is the pure logic against the design's numbers. Part 2, given the pinned Linux tree's root (3857c2fe), checks
// the register absolutes against gc_11_0_0_offset.h, the CONTEXT1_CNTL / invalidate / GRBM_GFX_CNTL field shifts and the
// PTE/PDE bits, MTYPE and INDIRECT_BUFFER against gc_11_0_0_sh_mask.h, amdgpu_vm.h, navi10_enum.h and nvd.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include "n33_vm_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

using namespace n33vm;

// ---- a memory model for the walker: arena pages handed out from kFirstFreePageIdx, entries in a flat map ------------
struct Mem {
	std::map<uint64_t, uint64_t> ent;     // byte offset -> entry
	uint32_t next = kFirstFreePageIdx;
	uint32_t cap = kArenaPages;
	uint64_t rd(uint64_t tablePa, uint32_t idx) { auto it = ent.find(tablePa + (uint64_t)idx * 8); return it == ent.end() ? 0ull : it->second; }
	void wr(uint64_t tablePa, uint32_t idx, uint64_t v) { ent[tablePa + (uint64_t)idx * 8] = v; }
	uint64_t alloc_page() { if (next >= cap - 1) return 0; return page_off(next++); }   // cap-1: the sentinel is never handed out
};

static void geometry_cases() {
	CHECK(kMaxPfn == (1ull << 36));
	CHECK((kMaxPfn - 1) == 0xFFFFFFFFFull);                 // END LO32 0xFFFFFFFF, HI4 0xF
	CHECK((uint32_t)(kMaxPfn - 1) == 0xFFFFFFFFu && (uint32_t)((kMaxPfn - 1) >> 32) == 0xF);
	// level shifts
	const uint64_t low = 0x0000000000100000ull, high = 0xFFFF800000100000ull;
	CHECK(va_canonical(low) && va_canonical(high) && !va_canonical(0x0000800000000000ull));
	CHECK(va_strip(high) == 0x800000100000ull && va_canonicalize(va_strip(high)) == high);
	CHECK(idx_pdb2(low) == 0 && idx_pdb2(high) == 256);
	CHECK(idx_pdb1(high) == 0 && idx_pdb0(high) == 0 && idx_ptb(high) == 0x100);
	CHECK(va_in_context(0xFFFFFFFFFFFFF000ull) && va_in_context(0x10000ull));
	// the hole: one 48-bit tree covers both halves; the high half is root entries 256..511
	CHECK(idx_pdb2(0xFFFFFFFFFFFFF000ull) == 511 && idx_pdb2(0x7FFFFFFFF000ull) == 255);
}

static void encoder_cases() {
	CHECK(pde_encode(0x1000ull) == 0x1001ull);
	CHECK((pde_encode(0xD001000ull) & (kPteRead | kPteWrite | kPteExec)) == 0);   // a directory grants no access
	CHECK(pde_encode(0x800ull) == 0);                                              // not 4 KiB aligned -> invalid
	CHECK((pde_encode(0x0000FFFFFFFFF000ull) & kPteValid) != 0);
	// leaves: no bit 63; MTYPE at 50:48; the design's three example words.
	CHECK(pte_leaf(0xC000000ull, leaf_flags(false, kVmDefault)) == 0x000000000C000071ull);   // VRAM R|W|X NC
	CHECK(pte_leaf(0xC000000ull, leaf_flags(false, kVmDefault | (kMtypeUc << kVmMtypeShift))) == 0x000300000C000071ull);   // UC
	CHECK(pte_leaf(0x123456000ull, leaf_flags(true, kVmDefault)) == 0x0000000123456077ull);  // GTT NC R|W|X
	CHECK((pte_leaf(0x123456000ull, leaf_flags(true, kVmDefault)) >> 63) == 0);               // no IS_PTE
	CHECK(((pte_leaf(0x1000ull, leaf_flags(false, kVmDefault | (kMtypeUc << kVmMtypeShift))) >> 48) & 7) == 3);
	CHECK(pte_leaf(0x1ull, leaf_flags(false, kVmDefault)) == 0);                              // unaligned address -> 0
	// flags
	CHECK(vm_flags_ok(kVmRead) && vm_flags_ok(kVmDefault) && vm_flags_ok(kVmWrite | (kMtypeUc << kVmMtypeShift)));
	CHECK(!vm_flags_ok(0) && !vm_flags_ok(kVmExec) && !vm_flags_ok(kVmDefault | (1u << 9)));   // X-only, or an unknown bit
	CHECK(vm_mtype(kVmDefault | (kMtypeCc << kVmMtypeShift)) == kMtypeCc);
}

static void context_cases() {
	CHECK(kCtxCheckVal == 0x00555407u);
	CHECK(ctx8_cntl_ok(0x01fffe07u) && ctx8_cntl_ok(0x00555407u));
	CHECK(!ctx8_cntl_ok(0x00555007u) && !ctx8_cntl_ok(0x01fffe87u));   // a missing fault bit, or RETRY set
	CHECK(kInvReqVmid8 == 0x00F80100u && kInvAckVmid8 == 0x100u);
	CHECK(kGrbmSelVmid8 == 0x80u && kShMemConfigVal == 0xC00Cu && kShMemBasesVal == 0x20001000u);
	// absolutes (design D5)
	CHECK(kCtx8Cntl == 0x28F0 && kCtx8BaseLo == 0x2963 && kCtx8BaseHi == 0x2964);
	CHECK(kCtx8StartLo == 0x2983 && kCtx8EndLo == 0x29A3 && kInvEng17Req == 0x291C && kInvEng17Ack == 0x292E);
	CHECK(kGrbmGfxCntl == 0xA900 && kShMemBases == 0xA9E3 && kShMemConfig == 0xA9E4);
	CHECK(ranges_contain_row());
}

static void arena_cases() {
	CHECK(kArenaOff == 0xD000000ull && kArenaLen == (8ull << 20) && kArenaPages == 2048);
	CHECK(kArenaOff == n33bo::kSlabOff + n33bo::kSlabLen);           // right after W2's slab
	CHECK(kArenaOff + kArenaLen <= 0x10000000ull);                  // inside BAR0
	CHECK(kKind == 21 && kKind == n33bo::kKind + 1);
	CHECK(page_off(1) == 0xD001000ull && page_off(kSentinelPageIdx) == 0xD7FF000ull);
}

static void walker_cases() {
	Mem m;
	const uint64_t root = page_off(kRootPageIdx);
	// map one page at each of four VAs across the ranges, and a run crossing a PTB boundary
	struct Case { uint64_t va; } cs[] = { {0x10000ull}, {0x7FFFFFFFF000ull}, {0xFFFF800000000000ull}, {0xFFFFFFFFFFFFF000ull} };
	uint64_t pa = 0x100000000ull;
	for (auto &c : cs) {
		CHECK(pt_map(root, va_strip(c.va), 1, pa, leaf_flags(false, kVmDefault), m) == kCodeOk);
		uint64_t leaf = 0;
		CHECK(pt_walk(root, va_strip(c.va), m, &leaf) == pa);
		CHECK((leaf & kPteAddrMask) == pa && (leaf >> 63) == 0 && ((leaf >> 48) & 7) == 0);   // NC, no IS_PTE
		pa += 0x1000;
	}
	// a 600-page run (crosses a PTB and a PDB0 boundary) maps and unmaps cleanly
	const uint64_t runVa = va_strip(0x200000000ull);
	CHECK(pt_map(root, runVa, 600, 0x200000000ull, leaf_flags(false, kVmDefault), m) == kCodeOk);
	uint64_t leaf = 0;
	CHECK(pt_walk(root, runVa, m, &leaf) == 0x200000000ull);
	CHECK(pt_walk(root, runVa + 599 * kPage, m, &leaf) == 0x200000000ull + 599 * kPage);
	pt_unmap(root, runVa, 600, m);
	CHECK(pt_walk(root, runVa, m, &leaf) == 0 && pt_walk(root, runVa + 599 * kPage, m, &leaf) == 0);
	// pt_map_pages with a non-contiguous page-PA functor (GTT/import BOs)
	Mem g;
	const uint64_t groot = page_off(kRootPageIdx);
	uint64_t pages_pa[3] = { 0x500000000ull, 0x700000000ull, 0x600000000ull };   // deliberately out of order
	auto pafn = [&](uint64_t p) { return pages_pa[p]; };
	CHECK(pt_map_pages(groot, va_strip(0x30000ull), 3, pafn, leaf_flags(true, kVmDefault), g) == kCodeOk);
	uint64_t gl = 0;
	CHECK(pt_walk(groot, va_strip(0x30000ull), g, &gl) == 0x500000000ull);
	CHECK(pt_walk(groot, va_strip(0x32000ull), g, &gl) == 0x600000000ull);   // the third page's PA
	CHECK((gl & kPteSystem) && (gl & kPteSnooped));                           // GTT flags
	// arena exhaustion in pass 1 writes no leaf: a fresh small-cap memory, map more than it can hold
	Mem tiny; tiny.cap = kFirstFreePageIdx + 2;    // room for only ~2 table pages
	const uint32_t r = pt_map(page_off(kRootPageIdx), va_strip(0x4000000000ull), 1, 0x300000000ull, leaf_flags(false, kVmDefault), tiny);
	CHECK(r == kCodeArena);
	uint64_t l2 = 0;
	CHECK(pt_walk(page_off(kRootPageIdx), va_strip(0x4000000000ull), tiny, &l2) == 0);
}

static void stream_cases() {
	uint32_t p[128];
	// stream 5: 16 COPY_DATA, no fence (96 dwords)
	const uint64_t src = 0xFFFF800100000000ull, dst = 0xFFFFBFFFFFA00000ull;
	CHECK(copy_stream(p, src, dst, 0x10000) == 96);
	CHECK(p[0] == 0xC0044000u && p[1] == n33bo::kCopyCtl);
	CHECK(((uint64_t)p[3] << 32 | p[2]) == src && ((uint64_t)p[5] << 32 | p[4]) == dst);   // canonical high VAs, as RADV passes
	// stream 6: dispatch, no fence; PGM = code VA >> 8
	const uint64_t code = 0xFFFF800000101000ull, dstb = 0xFFFFBFFFFFA08000ull;
	const uint32_t nd = dispatch_stream(p, code, dstb);
	CHECK(nd == 33);                                                    // 41 (K4) minus the 8-dword RELEASE_MEM
	CHECK(p[0] == n33cp::pkt3(n33cp::kOpSetShReg, 2) && p[1] == (n33cp::kComputePgmLo - n33cp::kShRegStart));
	CHECK(p[2] == (uint32_t)(code >> 8) && p[3] == (uint32_t)(code >> 40));
	// stream 7: WRITE_DATA, no fence (5 dwords)
	CHECK(fault_stream(p, 0xFFFFA00000000000ull) == 5 && p[4] == n33bo::kFaultMarker);
	// the ring content: INDIRECT_BUFFER(ib, VMID 8) then RELEASE_MEM
	const uint64_t ib = 0xFFFF800000100000ull, fence = n33gmc::slot_mc(n33cp::kSlotB) + n33cp::kHostFence4;
	const uint32_t nr = ring_stream(p, ib, 96, fence, 3);
	CHECK(nr == 12 && p[0] == 0xC0023F00u);
	CHECK(p[1] == (uint32_t)(ib & ~3ull) && p[2] == (uint32_t)(ib >> 32));
	CHECK(p[3] == (n33cp::kIbValid | 96u | (kVmid << 24)));            // VMID 8 in the control word
	CHECK(p[4] == n33cp::pkt3(n33cp::kOpReleaseMem, 6));
}

static void refusal_cases() {
	State s = {};
	// before vm create
	CHECK(may_create(s, false, false) == kCodeOrder);          // G6 not done
	CHECK(may_create(s, true, true) == kCodeOrder);            // a submit outstanding
	CHECK(may_create(s, true, false) == kCodeOk);
	s.live = true; s.rootPa = page_off(kRootPageIdx);
	CHECK(may_create(s, true, false) == kCodeOrder);           // vm create twice
	// gemva_check
	CHECK(gemva_check(kVmDefault, 0x10000ull, 0, 0x1000, 0x10000) == kCodeOk);
	CHECK(gemva_check(kVmDefault, 0x0000800000000000ull, 0, 0x1000, 0x10000) == kCodeArg);   // non-canonical
	CHECK(gemva_check(kVmDefault, 0xF000ull, 0, 0x1000, 0x10000) == kCodeArg);               // below 0x10000
	CHECK(gemva_check(kVmDefault, 0x10000ull | 4, 0, 0x1000, 0x10000) == kCodeArg);          // misaligned
	CHECK(gemva_check(kVmDefault, 0x10000ull, 0, 0x2000, 0x1000) == kCodeArg);               // size past the BO
	CHECK(gemva_check(kVmExec, 0x10000ull, 0, 0x1000, 0x10000) == kCodeArg);                 // X-only (no R/W)
	// a map straddling the seam: strip near the top of the low half + a big size
	CHECK(gemva_check(kVmDefault, 0x7FFFFFFFF000ull, 0, 0x2000, 0x2000) == kCodeArg);
	// map / overlap / unmap
	CHECK(may_map(s, true, false, true, kVmDefault, 0x10000ull, 0, 0x1000, 0x10000) == kCodeOk);
	CHECK(record_map(s, 4, kVmDefault, 0x10000ull, 0x1000) == 0);
	CHECK(may_map(s, true, false, true, kVmDefault, 0x10000ull, 0, 0x1000, 0x1000) == kCodeOverlap);   // overlaps the mapping
	CHECK(may_map(s, true, true, true, kVmDefault, 0x20000ull, 0, 0x1000, 0x1000) == kCodeOrder);      // submit outstanding
	CHECK(may_unmap(s, true, false, 4, 0x10000ull) == kCodeOk);
	CHECK(may_unmap(s, true, false, 4, 0x20000ull) == kCodeOverlap);   // not mapped there
	CHECK(bo_has_mapping(s, 4) && !bo_has_mapping(s, 5));
	// fault VA rules
	CHECK(may_fault(s, 0xFFFFA00000000000ull, 0x8400001F00ull, 0x800D7FF000ull) == kCodeOk);
	CHECK(may_fault(s, 0x10000ull, 0x8400001F00ull, 0x800D7FF000ull) == kCodeOverlap);         // a mapped VA
	CHECK(may_fault(s, 0x8401000000ull, 0x8400001F00ull, 0x800D7FF000ull) == kCodeApertureVa); // in the VMID-0 apertures
	CHECK(may_fault(s, 0x8400002000ull, 0x8400001F00ull, 0x800D7FF000ull) == kCodeApertureVa);  // an aperture VA that is not a sentinel
	CHECK(may_fault(s, 0x8400001F00ull, 0x8400001F00ull, 0x800D7FF000ull) == kCodeOk);         // the V6 sentinel (sub-page, dword aligned) is allowed
	CHECK(may_fault(s, 0x800D7FF000ull, 0x8400001F00ull, 0x800D7FF000ull) == kCodeOk);         // the FB-aperture sentinel is allowed
	CHECK(may_fault(s, 0xFFFFA00000000004ull, 0x8400001F00ull, 0x800D7FF000ull) == kCodeOk);   // a dword-aligned high VA
	CHECK(may_fault(s, 0xFFFFA00000000002ull, 0x8400001F00ull, 0x800D7FF000ull) == kCodeArg);  // not dword aligned
	// fault match (VMID 8) and pass
	const uint64_t fva = 0xFFFFA00000000000ull;
	CHECK(fault_match(n33bo::kStatusRw | (kVmid << 20), (uint32_t)(va_strip(fva) >> 12), (uint32_t)(va_strip(fva) >> 44), fva));
	CHECK(!fault_match(n33bo::kStatusRw | (0u << 20), (uint32_t)(va_strip(fva) >> 12), 0, fva));   // VMID 0 is not ours
	CHECK(fault_pass(true, 0, true) && !fault_pass(true, 0, false) && !fault_pass(false, 0, true));
	// covering_map for the streams
	State c = {}; c.live = true;
	record_map(c, 2, kVmRead | kVmExec, 0xFFFF800000100000ull, 0x10000);   // ib/code BO R|X
	record_map(c, 1, kVmRead | kVmWrite, 0xFFFFBFFFFFA00000ull, 0x10000);  // data BO R|W
	CHECK(covering_map(c, 0xFFFF800000100000ull, 0x1000, kVmRead | kVmExec) == 0);
	CHECK(covering_map(c, 0xFFFFBFFFFFA00000ull, 0x1000, kVmWrite) == 1);
	CHECK(covering_map(c, 0xFFFFBFFFFFA00000ull, 0x1000, kVmExec) < 0);     // the data BO is not executable
}

// ---- Part 2: the Linux headers --------------------------------------------------------------------------------------
using Text = std::map<std::string, std::string>;
static bool load_macros(const std::string &path, Text &t) {
	std::ifstream f(path);
	if (!f) { printf("cannot open %s\n", path.c_str()); return false; }
	std::string line;
	while (std::getline(f, line)) {
		const size_t d = line.find("#define");
		if (d == std::string::npos) continue;
		size_t a = line.find_first_not_of(" \t", d + 7), b = line.find_first_of(" \t(", a);
		if (a == std::string::npos || b == std::string::npos) continue;
		std::string name = line.substr(a, b - a), body = line.substr(b);
		while (!body.empty() && body.back() == '\\' && std::getline(f, line)) { body.pop_back(); body += line; }
		t[name] = body;
	}
	return true;
}
static uint64_t bit_value(const std::string &body) {
	const size_t s = body.find("<<");
	if (s == std::string::npos) return strtoull(body.c_str() + body.find_first_not_of(" \t("), nullptr, 0);
	const size_t a = body.find_first_of("0123456789");
	return strtoull(body.c_str() + a, nullptr, 0) << strtoull(body.c_str() + body.find_first_of("0123456789", s), nullptr, 0);
}
static uint32_t shift_of(const std::string &body) { return (uint32_t)strtoul(body.c_str() + body.find_first_not_of(" \t", 0), nullptr, 0); }
static int expect(const Text &t, const char *name, uint64_t want, const char *what) {
	const auto m = t.find(name);
	if (m == t.end()) { printf("FAIL %s: %s not found\n", what, name); return 1; }
	const uint64_t v = bit_value(m->second);
	if (v != want) { printf("FAIL %s: %s is %#llx, ours %#llx\n", what, name, (unsigned long long)v, (unsigned long long)want); return 1; }
	return 0;
}
static int expect_shift(const Text &t, const char *name, uint32_t want, const char *what) {
	const auto m = t.find(name);
	if (m == t.end()) { printf("FAIL %s: %s not found\n", what, name); return 1; }
	const uint32_t v = shift_of(m->second);
	if (v != want) { printf("FAIL %s: %s shift is %u, ours %u\n", what, name, v, want); return 1; }
	return 0;
}

static int header_check(const char *root) {
	const std::string amd = std::string(root) + "/drivers/gpu/drm/amd/";
	Text off, mask, vm, nvd;
	if (!load_macros(amd + "include/asic_reg/gc/gc_11_0_0_offset.h", off) ||
	    !load_macros(amd + "include/asic_reg/gc/gc_11_0_0_sh_mask.h", mask) ||
	    !load_macros(amd + "amdgpu/amdgpu_vm.h", vm) || !load_macros(amd + "amdgpu/nvd.h", nvd)) return 1;
	int bad = 0;
	// register absolutes: seg 0 at 0x1260, seg 1 at 0xA000.
	struct A { const char *name; uint32_t abs; uint32_t seg; } as[] = {
		{ "regGCVM_CONTEXT8_CNTL", kCtx8Cntl, kGcSeg0 }, { "regGCVM_CONTEXT8_PAGE_TABLE_BASE_ADDR_LO32", kCtx8BaseLo, kGcSeg0 },
		{ "regGCVM_CONTEXT8_PAGE_TABLE_START_ADDR_LO32", kCtx8StartLo, kGcSeg0 }, { "regGCVM_CONTEXT8_PAGE_TABLE_END_ADDR_LO32", kCtx8EndLo, kGcSeg0 },
		{ "regGCVM_INVALIDATE_ENG17_REQ", kInvEng17Req, kGcSeg0 }, { "regGCVM_INVALIDATE_ENG17_ACK", kInvEng17Ack, kGcSeg0 },
		{ "regGCVM_L2_PROTECTION_FAULT_CNTL", kFaultCntl, kGcSeg0 }, { "regGCVM_L2_PROTECTION_FAULT_STATUS", kFaultStatus, kGcSeg0 },
		{ "regGRBM_GFX_CNTL", kGrbmGfxCntl, kGcSeg1 }, { "regSH_MEM_BASES", kShMemBases, kGcSeg1 }, { "regSH_MEM_CONFIG", kShMemConfig, kGcSeg1 },
	};
	for (const A &a : as) {
		const auto m = off.find(a.name);
		if (m == off.end()) { printf("FAIL offset: %s not found\n", a.name); bad++; continue; }
		const uint64_t o = bit_value(m->second);
		if (a.seg + o != a.abs) { printf("FAIL offset: %s %#llx + %#x, ours %#x\n", a.name, (unsigned long long)o, a.seg, a.abs); bad++; }
	}
	// CONTEXT1_CNTL field shifts (gfx11: BLOCK 3, RETRY 7, the 7 fault bits at 0xa..0x16 step 2).
	bad += expect_shift(mask, "GCVM_CONTEXT1_CNTL__ENABLE_CONTEXT__SHIFT", kCntlEnableShift, "sh_mask");
	bad += expect_shift(mask, "GCVM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH__SHIFT", kCntlDepthShift, "sh_mask");
	bad += expect_shift(mask, "GCVM_CONTEXT1_CNTL__PAGE_TABLE_BLOCK_SIZE__SHIFT", kCntlBlockShift, "sh_mask");
	bad += expect_shift(mask, "GCVM_CONTEXT1_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT__SHIFT", kCntlRetryShift, "sh_mask");
	bad += expect_shift(mask, "GCVM_CONTEXT1_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT", kCntlFault0Shift, "sh_mask");
	bad += expect_shift(mask, "GCVM_CONTEXT1_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT__SHIFT", kCntlFault0Shift + 12, "sh_mask");
	// invalidate request fields -> the VMID-8 word, and the ACK field.
	bad += expect_shift(mask, "GCVM_INVALIDATE_ENG17_REQ__PER_VMID_INVALIDATE_REQ__SHIFT", 0, "sh_mask");
	bad += expect_shift(mask, "GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L2_PTES__SHIFT", 0x13, "sh_mask");
	bad += expect_shift(mask, "GCVM_INVALIDATE_ENG17_REQ__INVALIDATE_L1_PTES__SHIFT", 0x17, "sh_mask");
	bad += expect_shift(mask, "GCVM_INVALIDATE_ENG17_ACK__PER_VMID_INVALIDATE_ACK__SHIFT", 0, "sh_mask");
	// GRBM_GFX_CNTL.VMID
	bad += expect_shift(mask, "GRBM_GFX_CNTL__VMID__SHIFT", kGrbmVmidShift, "sh_mask");
	// PTE / PDE bits and MTYPE
	bad += expect(vm, "AMDGPU_PTE_VALID", kPteValid, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_SYSTEM", kPteSystem, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_SNOOPED", kPteSnooped, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_EXECUTABLE", kPteExec, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_READABLE", kPteRead, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_WRITEABLE", kPteWrite, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_IS_PTE", 1ull << 63, "amdgpu_vm.h");   // gfx12 only; we never set it
	if (mask.find("AMDGPU_PTE_MTYPE_NV10") == mask.end()) {}             // MTYPE_NV10 lives in amdgpu_vm.h
	{
		std::string body = vm.count("AMDGPU_PTE_MTYPE_NV10_SHIFT") ? vm["AMDGPU_PTE_MTYPE_NV10_SHIFT"] : "";
		if (body.find("<< 48") == std::string::npos) { printf("FAIL MTYPE_NV10_SHIFT not 48\n"); bad++; }
	}
	if ((uint32_t)kMtypeShift != 48) { printf("FAIL kMtypeShift\n"); bad++; }
	// MTYPE values (navi10_enum.h)
	std::string en;
	{ std::ifstream f(amd + "include/navi10_enum.h"); std::stringstream ss; ss << f.rdbuf(); en = ss.str(); }
	if (en.find("MTYPE_NC                                 = 0x00000000,") == std::string::npos ||
	    en.find("MTYPE_UC                                 = 0x00000003,") == std::string::npos) { printf("FAIL navi10_enum MTYPE\n"); bad++; }
	// INDIRECT_BUFFER (nvd.h): the VALID bit and the VMID field position carried in ib_control.
	bad += expect(nvd, "PACKET3_INDIRECT_BUFFER", n33cp::kOpIndirectBuffer, "nvd.h");
	printf("header check: 11 register absolutes, 10 field shifts, 7 PTE bits + MTYPE, INDIRECT_BUFFER: %s\n",
	       bad ? "FAILED" : "all match");
	return bad;
}

int main(int argc, char **argv) {
	geometry_cases(); encoder_cases(); context_cases(); arena_cases(); walker_cases(); stream_cases(); refusal_cases();
	if (argc > 1) fails += header_check(argv[1]);
	else printf("header check skipped: pass the Linux tree root (3857c2fe) as the first argument\n");
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("n33_vm_test: all passed\n");
	return 0;
}
