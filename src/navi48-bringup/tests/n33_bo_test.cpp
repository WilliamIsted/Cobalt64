// n33_bo_test.cpp: host test of amd/n33_bo_pure.h (stage 3 step 7 W2: BOs, VMID 0 VAs, the gfx11 PTE, the copy and
// fault streams).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_bo_test.cpp -o $SCRATCH/n33_bo_test && \
//       $SCRATCH/n33_bo_test [<linux tree root>]
// Part 1 is the pure logic against the design's numbers. Part 2, given the pinned Linux tree's root (3857c2fe), checks
// the PTE bits against amdgpu_vm.h, navi10_enum.h and gmc_v11_0.c's PTE layout, the VM flags against amdgpu_drm.h, the
// COPY_DATA fields against nvd.h and the fault registers and fields against gc_11_0_0_offset.h / _sh_mask.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include "n33_bo_pure.h"
#include "n33_dmub_pure.h"
#include "n33_ih_pure.h"
#include "n33_fb_pure.h"
#include "n33_psp2_pure.h"
#include "n33_smu_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

using namespace n33bo;

static void pte_cases() {
	const uint64_t pa = 0x123456000ull;
	CHECK(pte(pa, true, kVmDefault) == n33gmc::pte_sys(pa));      // the proven host word 0x0003000123456077
	CHECK(pte(pa, false, kVmDefault) == n33gmc::pte_vram(pa));    // the GMC code's VRAM word ...71
	CHECK(pte(pa, true, kVmDefault) == 0x0003000123456077ull);
	CHECK(pte(0xC000000, false, kVmDefault) == 0x000300000C000071ull);
	CHECK(pte(pa, true, kVmRead) == 0x0003000123456027ull);       // read only: no WRITEABLE, no EXECUTABLE
	CHECK(pte(pa, false, kVmRead | kVmWrite) == 0x0003000123456061ull);
	CHECK((pte(0x0000FFFFFFFFF000ull, true, kVmDefault) >> 63) == 0);   // no gfx12 IS_PTE
	CHECK(((pte(pa, true, kVmDefault) >> 48) & 7) == 3);                 // MTYPE UC at 50:48
	CHECK(((pte(pa, true, kVmDefault) >> 7) & 0x1F) == 0);               // fragment 0
	CHECK(pte(0x123456FFFull, true, kVmDefault) == 0x0003000123456077ull);   // low bits dropped
	CHECK(pte(0xFFFF123456000ull, true, kVmDefault) == 0x0003FFF123456077ull);   // bits above 47 dropped
	CHECK(vm_flags_ok(kVmRead) && vm_flags_ok(kVmWrite) && vm_flags_ok(kVmMask));
	CHECK(!vm_flags_ok(0) && !vm_flags_ok(kVmExec) && !vm_flags_ok(1) && !vm_flags_ok(kVmRead | (1u << 4)));
}

struct R { uint64_t off, len; };
static bool hit(R a, R b) { return a.off < b.off + b.len && b.off < a.off + a.len; }
static void placement_cases() {
	CHECK(kVaBase == 0x8401000000ull && kVaEnd == 0x8402000000ull);
	CHECK(slot_of(kVaBase) == 0x1000 && slot_of(0x8401FFF000ull) == 0x1FFF);
	CHECK(!window_hits(0, 4) && !window_hits(2, 1) && window_hits(0x1000, 1) && window_hits(0xFFF, 2) &&
	      window_hits(0x1FFF, 1) && !window_hits(0x2000, 16) && !window_hits(0x1000, 0));
	CHECK(in_window(kVaBase, kBoMaxBytes) && in_window(kVaEnd - kPage, kPage) && !in_window(kVaEnd - kPage, 2 * kPage));
	CHECK(!in_window(kVaBase - kPage, kPage) && !in_window(n33gmc::slot_mc(1), kPage));
	// The slab is clear of every owned or protected range the other steps use (design "Placement").
	const R slab = { kSlabOff, kSlabLen };
	const R others[] = {
		{ 0, 0xE10000 }, { n33fb::kSurfOff, n33fb::kBytes }, { n33dmub::kDmubOff, n33dmub::kDmubMax },
		{ 0x8000000, 0x500000 }, { n33psp2::kRing, n33psp2::kPspPages }, { n33smu::kTableOff, n33smu::kTableLen },
		{ n33gmc::kOwnedOff, n33gmc::kOwnedLen }, { n33sdma::kOwnedOff, n33sdma::kOwnedLen },
		{ n33cp::kOwnedOff, n33cp::kOwnedLen }, { n33cp::kIbOff, n33cp::kIbLen }, { n33ih::kRingOff, n33ih::kRingBytes },
	};
	for (const R &o : others) CHECK(!hit(slab, o));
	CHECK(kSlabOff + kSlabLen <= 0x10000000ull);   // inside the 256 MiB BAR0: the CPU map
	CHECK(kSlabOff >= (16ull << 20));              // above the kext's write floor
	CHECK(kKind == 20 && n33ih::kKind == 19);      // the next protected kind
	// The window is clear of the slots the other steps bind (0 SDMA A, 1 CP B, 2 IH C, 3 G5).
	for (uint32_t s = 0; s < 4; s++) CHECK(!window_hits(s, 1));
	CHECK(kVaSlot0 > n33ih::kSlotC && kVaSlot0 > n33cp::kSlotB);
}

static void lifecycle_cases() {
	State s = {};
	CHECK(may_create(s, kVram, 0x10000, false) == kWhyOrder);
	CHECK(may_create(s, 3, 0x10000, true) == kWhyArg);
	CHECK(may_create(s, kVram, 0, true) == kWhyArg && may_create(s, kVram, 0x1001, true) == kWhyArg);
	CHECK(may_create(s, kVram, kBoMaxBytes + kPage, true) == kWhyArg && may_create(s, kVram, kBoMaxBytes, true) == kWhyOk);
	uint64_t off = 1;
	CHECK(vram_place(s, 16, &off) && off == 0);
	const uint32_t h1 = record_create(s, kVram, 16, off);
	CHECK(h1 == 1 && s.slabOwned && s.hostBytes == 0);
	CHECK(vram_place(s, 16, &off) && off == 0x10000);
	const uint32_t h2 = record_create(s, kVram, 16, off);
	CHECK(may_create(s, kGtt, 0x10000, true) == kWhyOk);
	const uint32_t h3 = record_create(s, kGtt, 16, 0);
	CHECK(h2 == 2 && h3 == 3 && s.hostBytes == 0x10000);
	// first fit reuses a freed run
	CHECK(may_free(s, h1, false) == kWhyOk);
	record_free(s, h1);
	CHECK(vram_place(s, 8, &off) && off == 0);
	CHECK(vram_place(s, 32, &off) && off == 0x20000);   // 0x0..0x10000 is too small for 128 KiB
	CHECK(!handle_ok(s, h1) && handle_ok(s, h2) && !handle_ok(s, 0) && !handle_ok(s, 9));
	// map / unmap / free order
	CHECK(may_map(s, h2, kVaBase, kVmDefault, false, false) == kWhyOrder);
	CHECK(may_map(s, h2, kVaBase + 4, kVmDefault, true, false) == kWhyArg);
	CHECK(may_map(s, h2, kVaBase, 0, true, false) == kWhyArg);
	CHECK(may_map(s, h2, n33gmc::slot_mc(1), kVmDefault, true, false) == kWhyVa);
	CHECK(may_map(s, h2, kVaEnd - kPage, kVmDefault, true, false) == kWhyVa);   // runs past the window
	CHECK(may_map(s, h2, kVaBase, kVmDefault, true, true) == kWhyOrder);        // a submit outstanding
	CHECK(may_map(s, h2, kVaBase, kVmDefault, true, false) == kWhyOk);
	record_map(s, h2, kVaBase, kVmDefault);
	CHECK(may_map(s, h2, kVaBase + 0x100000, kVmDefault, true, false) == kWhyOrder);   // already mapped
	CHECK(may_map(s, h3, kVaBase + 0xF000, kVmDefault, true, false) == kWhyVa);        // overlaps h2's last page
	CHECK(may_map(s, h3, kVaBase + 0x10000, kVmDefault, true, false) == kWhyOk);       // just after it
	CHECK(may_free(s, h2, false) == kWhyOrder);
	CHECK(may_unmap(s, h3, true, false) == kWhyOrder);   // not mapped
	CHECK(may_unmap(s, h2, true, true) == kWhyOrder);
	CHECK(may_unmap(s, h2, true, false) == kWhyOk);
	record_unmap(s, h2);
	CHECK(!s.bo[h2 - 1].mapped && may_free(s, h2, true) == kWhyOrder && may_free(s, h2, false) == kWhyOk);
	CHECK(may_read(s, h2, 0) == kWhyOk && may_read(s, h2, 0x10000 / 4) == kWhyArg && may_read(s, 7, 0) == kWhyArg);
	record_free(s, h3);
	CHECK(s.hostBytes == 0);
	// the host cap and the handle limit
	State t = {};
	for (uint32_t i = 0; i < 4; i++) { CHECK(may_create(t, kGtt, kBoMaxBytes, true) == kWhyOk); record_create(t, kGtt, kBoMaxPages, 0); }
	CHECK(may_create(t, kGtt, kPage, true) == kWhyFull && may_import(t, 0x10000000, kPage, true) == kWhyFull);
	CHECK(may_create(t, kVram, kPage, true) == kWhyOk);
	for (uint32_t i = 0; i < 4; i++) record_create(t, kVram, 1, i * kPage);
	CHECK(may_create(t, kVram, kPage, true) == kWhyFull && record_create(t, kVram, 1, 0) == 0);
	// a full slab
	State u = {};
	CHECK(vram_place(u, kBoMaxPages, &off));
	for (uint32_t i = 0; i < 8; i++) record_create(u, kVram, kBoMaxPages, i * kBoMaxBytes);
	State v = {};
	v.bo[0] = { true, false, kVram, (uint32_t)(kSlabLen / kPage), 0, 0, 0 };
	CHECK(!vram_place(v, 1, &off));
}

static void import_cases() {
	State s = {};
	CHECK(may_import(s, 0x100000000ull, 0x10000, true) == kWhyOk);
	CHECK(may_import(s, 0x100000000ull, 0x10000, false) == kWhyOrder);
	CHECK(may_import(s, 0, 0x10000, true) == kWhyArg && may_import(s, 0x1000, 0, true) == kWhyArg);
	CHECK(may_import(s, 0x100000800ull, 0x10000, true) == kWhyArg && may_import(s, 0x100000000ull, 0x10800, true) == kWhyArg);
	CHECK(may_import(s, kUserMax - kPage, 2 * kPage, true) == kWhyArg && may_import(s, kUserMax, kPage, true) == kWhyArg);
	CHECK(may_import(s, 0x100000000ull, kBoMaxBytes + kPage, true) == kWhyArg);
	const Bar bars[3] = { { 0x90000000ull, 0x10000000ull }, { 0, 0 }, { 0xA0000000ull, 0x200000 } };
	CHECK(!page_in_bars(0x1000, bars, 3) && !page_in_bars(0x8FFFF000ull, bars, 3));
	CHECK(page_in_bars(0x90000000ull, bars, 3) && page_in_bars(0x9FFFF000ull, bars, 3) && page_in_bars(0xA01FF000ull, bars, 3));
	CHECK(!page_in_bars(0xA0200000ull, bars, 3));
	CHECK(page_in_bars(0x1000, nullptr, 0) && page_in_bars(0x1000, bars, 0));   // fails closed
	const Bar odd[1] = { { 0x90000800ull, 0x100 } };                              // a BAR inside one page
	CHECK(page_in_bars(0x90000000ull, odd, 1));
	const Bar wrap[1] = { { 0xFFFFFFFFFFFFF000ull, 0x2000 } };
	CHECK(page_in_bars(0xFFFFFFFFFFFFF000ull, wrap, 1) && !page_in_bars(0x1000, wrap, 1));
}

static void stream_cases() {
	uint32_t p[n33cp::kSubmitDw];
	const uint64_t src = kVaBase + 0x100000, dst = kVaBase, fence = n33gmc::slot_mc(n33cp::kSlotB) + n33cp::kHostFence4;
	const uint32_t n = copy_stream(p, src, dst, 0x10000, fence, 7);
	CHECK(n == 104);
	CHECK(p[0] == 0xC0044000u && p[1] == 0x00100501u);
	for (uint32_t k = 0; k < kCopyN; k++) {
		const uint32_t *q = p + 6 * k;
		CHECK(q[0] == 0xC0044000u && q[1] == kCopyCtl);
		CHECK(((uint64_t)q[3] << 32 | q[2]) == src + k * 0x1000 && ((uint64_t)q[5] << 32 | q[4]) == dst + k * 0x1000);
	}
	uint32_t rm[8];
	n33cp::pkt_release_mem(rm, fence, 7, true);
	CHECK(!memcmp(p + 96, rm, sizeof rm));
	CHECK(copy_off(15, 0x10000) == 0xF000 && copy_off(1, 0x1000) == 0x100 && copy_off(3, 0x1000) == 0x300);
	CHECK((copy_off(5, 0x3000) & 3) == 0 && copy_off(15, 0x3000) + 4 <= 0x3000);
	CHECK(pattern(7, 0) == 0xB0000070u && pattern(7, 15) == 0xB000007Fu && pattern(0x12345, 1) == 0xB0123451u);
	State s = {};
	s.bo[0] = { true, true, kVram, 16, kVmDefault, 0, kVaBase };
	s.bo[1] = { true, true, kGtt, 16, kVmRead, 0, kVaBase + 0x100000 };
	s.bo[2] = { true, false, kGtt, 16, 0, 0, 0 };
	CHECK(may_copy(s, 2, 1, true) == kWhyOk);
	CHECK(may_copy(s, 1, 2, true) == kWhyArg);    // the destination is read only
	CHECK(may_copy(s, 3, 1, true) == kWhyOrder && may_copy(s, 2, 1, false) == kWhyOrder);
	CHECK(may_copy(s, 1, 1, true) == kWhyArg && may_copy(s, 9, 1, true) == kWhyArg);
	// fault stream and its verdict
	const uint64_t fva = 0x8401FFF000ull;
	const uint32_t m = fault_stream(p, fva, fence, 9);
	CHECK(m == 13 && p[0] == n33cp::pkt3(n33cp::kOpWriteData, 3) && p[2] == 0x01FFF000u && p[3] == 0x84 && p[4] == kFaultMarker);
	CHECK(may_fault(s, fva, true) == kWhyOk && may_fault(s, fva + 4, true) == kWhyArg && may_fault(s, fva, false) == kWhyOrder);
	CHECK(may_fault(s, kVaBase + 0x3000, true) == kWhyVa && may_fault(s, n33gmc::slot_mc(5), true) == kWhyVa);
	CHECK(fault_match(kStatusRw | 1, 0x8401FFF, 0, fva));
	CHECK(!fault_match(0, 0x8401FFF, 0, fva) && !fault_match(1, 0x8401FFF, 0, fva));         // nothing, or a read
	CHECK(!fault_match(kStatusRw | (1u << 20), 0x8401FFF, 0, fva));                            // VMID 1
	CHECK(!fault_match(kStatusRw, 0x8401FFE, 0, fva) && !fault_match(kStatusRw, 0x8401FFF, 1, fva));
}

// ---- Part 2: the Linux headers --------------------------------------------------------------------------------------
using Text = std::map<std::string, std::string>;   // macro name -> body
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
static bool read_all(const std::string &path, std::string &out) {
	std::ifstream f(path);
	if (!f) { printf("cannot open %s\n", path.c_str()); return false; }
	std::stringstream ss; ss << f.rdbuf(); out = ss.str();
	return true;
}
// "(1ULL << 5)" / "(1 << 1)" -> 1 << 5; a plain number -> itself; anything else -> ~0.
static uint64_t bit_value(const std::string &body) {
	const size_t s = body.find("<<");
	if (s == std::string::npos) return strtoull(body.c_str() + body.find_first_not_of(" \t("), nullptr, 0);
	const size_t a = body.find_first_of("0123456789");
	return strtoull(body.c_str() + a, nullptr, 0) << strtoull(body.c_str() + body.find_first_of("0123456789", s), nullptr, 0);
}
// "((((unsigned)(x)) & 0xF) << 8)" -> mask 0xF, shift 8.
static void field(const std::string &body, uint32_t *mask, uint32_t *shift) {
	const size_t a = body.find("& "), s = body.find("<<");
	*mask = a == std::string::npos ? 0 : (uint32_t)strtoul(body.c_str() + a + 2, nullptr, 0);
	*shift = s == std::string::npos ? 0 : (uint32_t)strtoul(body.c_str() + body.find_first_of("0123456789", s), nullptr, 0);
}
static int expect(const Text &t, const char *name, uint64_t want, const char *what) {
	const auto m = t.find(name);
	if (m == t.end()) { printf("FAIL %s: %s not found\n", what, name); return 1; }
	const uint64_t v = bit_value(m->second);
	if (v != want) { printf("FAIL %s: %s is %#llx, ours %#llx\n", what, name, (unsigned long long)v, (unsigned long long)want); return 1; }
	return 0;
}
static int contains(const std::string &hay, const char *needle, const char *what) {
	if (hay.find(needle) != std::string::npos) return 0;
	printf("FAIL %s: no \"%s\"\n", what, needle);
	return 1;
}

static int header_check(const char *root) {
	const std::string amd = std::string(root) + "/drivers/gpu/drm/amd/";
	Text vm, drm, nvd, gcOff, gcMask;
	std::string gmc, en;
	if (!load_macros(amd + "amdgpu/amdgpu_vm.h", vm) || !load_macros(std::string(root) + "/include/uapi/drm/amdgpu_drm.h", drm) ||
	    !load_macros(amd + "amdgpu/nvd.h", nvd) || !load_macros(amd + "include/asic_reg/gc/gc_11_0_0_offset.h", gcOff) ||
	    !load_macros(amd + "include/asic_reg/gc/gc_11_0_0_sh_mask.h", gcMask) || !read_all(amd + "amdgpu/gmc_v11_0.c", gmc) ||
	    !read_all(amd + "include/navi10_enum.h", en)) return 1;
	int bad = 0;
	using namespace n33gmc;
	// PTE bits (amdgpu_vm.h:57-68) and the MTYPE field (:114-118); gfx12's IS_PTE is bit 63 (:133), which we never set.
	bad += expect(vm, "AMDGPU_PTE_VALID", kPteValid, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_SYSTEM", kPteSystem, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_SNOOPED", kPteSnooped, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_EXECUTABLE", kPteExec, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_READABLE", kPteRead, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_WRITEABLE", kPteWrite, "amdgpu_vm.h");
	bad += expect(vm, "AMDGPU_PTE_IS_PTE", 1ull << 63, "amdgpu_vm.h");
	bad += contains(vm["AMDGPU_PTE_MTYPE_NV10_SHIFT"], "<< 48", "amdgpu_vm.h MTYPE_NV10_SHIFT");
	bad += contains(vm["AMDGPU_PTE_MTYPE_NV10_MASK"], "7ULL", "amdgpu_vm.h MTYPE_NV10_MASK");
	bad += contains(vm["AMDGPU_PTE_FRAG"], "0x1fULL) << 7", "amdgpu_vm.h FRAG");
	if ((kPteMtypeUc >> 48) != 3 || (kPteMtypeUc & ~(7ull << 48))) { printf("FAIL kPteMtypeUc %#llx\n", (unsigned long long)kPteMtypeUc); bad++; }
	bad += contains(en, "MTYPE_UC                                 = 0x00000003,", "navi10_enum.h MTYPE_UC");
	// gmc_v11_0.c: the gfx11 layout comment (:414-436), the address mask (:824) and the GART's MTYPE UC (:734).
	const char *layout[] = { " * 63:59 reserved", " * 50:48 mtype", " * 47:12 4k physical page base address", " * 11:7 fragment",
	                         " * 6 write", " * 5 read", " * 4 exe", " * 2 snooped", " * 1 system", " * 0 valid" };
	for (const char *l : layout) bad += contains(gmc, l, "gmc_v11_0.c PTE format");
	bad += contains(gmc, "pte_addr_mask = 0x0000FFFFFFFFF000ULL", "gmc_v11_0.c pte_addr_mask");
	if (kPteAddrMask != 0x0000FFFFFFFFF000ull) { printf("FAIL kPteAddrMask\n"); bad++; }
	bad += contains(gmc, "gart_pte_flags = AMDGPU_PTE_MTYPE_NV10(0ULL, MTYPE_UC)", "gmc_v11_0.c GART MTYPE");
	// uapi VM flags (amdgpu_drm.h:882-886).
	bad += expect(drm, "AMDGPU_VM_PAGE_READABLE", kVmRead, "amdgpu_drm.h");
	bad += expect(drm, "AMDGPU_VM_PAGE_WRITEABLE", kVmWrite, "amdgpu_drm.h");
	bad += expect(drm, "AMDGPU_VM_PAGE_EXECUTABLE", kVmExec, "amdgpu_drm.h");
	// COPY_DATA (nvd.h:252-271).
	bad += expect(nvd, "PACKET3_COPY_DATA", kOpCopyData, "nvd.h");
	struct F { const char *name; uint32_t mask, shift, ours; } fs[] = {
		{ "PACKET3_COPY_DATA__SRC_SEL", 0xF, 0, kCopySrcMem }, { "PACKET3_COPY_DATA__DST_SEL", 0xF, 8, kCopyDstMem },
		{ "PACKET3_COPY_DATA__COUNT_SEL", 0x1, 16, 0 }, { "PACKET3_COPY_DATA__WR_CONFIRM", 0x1, 20, 1 },
	};
	uint32_t ctl = 0;
	for (const F &f : fs) {
		uint32_t m = 0, s = 0;
		field(nvd[f.name], &m, &s);
		if (m != f.mask || s != f.shift) { printf("FAIL nvd.h: %s mask %#x shift %u, want %#x %u\n", f.name, m, s, f.mask, f.shift); bad++; }
		ctl |= (f.ours & m) << s;
	}
	if (ctl != kCopyCtl) { printf("FAIL COPY_DATA control %#x, ours %#x\n", ctl, kCopyCtl); bad++; }
	// The fault registers (GC segment 0 at 0x1260) and fields.
	struct A { const char *name; uint32_t abs; } as[] = {
		{ "regGCVM_L2_PROTECTION_FAULT_CNTL", kFaultCntl }, { "regGCVM_L2_PROTECTION_FAULT_STATUS", n33cp::kGcFaultStatus },
		{ "regGCVM_L2_PROTECTION_FAULT_ADDR_LO32", n33cp::kGcFaultAddrLo }, { "regGCVM_L2_PROTECTION_FAULT_ADDR_HI32", n33cp::kGcFaultAddrHi },
	};
	for (const A &a : as) {
		const uint64_t off = bit_value(gcOff[a.name]);
		if (0x1260 + off != a.abs) { printf("FAIL gc_11_0_0_offset.h: %s %#llx + 0x1260, ours %#x\n", a.name, (unsigned long long)off, a.abs); bad++; }
	}
	if (kFaultCntl != kGc.faultCntl) { printf("FAIL kFaultCntl is not n33gmc::kGc.faultCntl\n"); bad++; }
	bad += expect(gcMask, "GCVM_L2_PROTECTION_FAULT_CNTL__CLEAR_PROTECTION_FAULT_STATUS_ADDR_MASK", kFaultClear, "gc_11_0_0_sh_mask.h");
	bad += expect(gcMask, "GCVM_L2_PROTECTION_FAULT_STATUS__RW_MASK", kStatusRw, "gc_11_0_0_sh_mask.h");
	bad += expect(gcMask, "GCVM_L2_PROTECTION_FAULT_STATUS__VMID_MASK", kStatusVmidMask, "gc_11_0_0_sh_mask.h");
	bad += expect(gcMask, "GCVM_L2_PROTECTION_FAULT_STATUS__VMID__SHIFT", kStatusVmidShift, "gc_11_0_0_sh_mask.h");
	bad += expect(gcMask, "GCVM_L2_PROTECTION_FAULT_STATUS__CID_MASK", kStatusCidMask, "gc_11_0_0_sh_mask.h");
	bad += expect(gcMask, "GCVM_L2_PROTECTION_FAULT_STATUS__CID__SHIFT", kStatusCidShift, "gc_11_0_0_sh_mask.h");
	bad += expect(gcMask, "GCVM_L2_PROTECTION_FAULT_ADDR_LO32__LOGICAL_PAGE_ADDR_LO32_MASK", 0xFFFFFFFFull, "gc_11_0_0_sh_mask.h");
	bad += expect(gcMask, "GCVM_L2_PROTECTION_FAULT_ADDR_HI32__LOGICAL_PAGE_ADDR_HI4_MASK", 0xF, "gc_11_0_0_sh_mask.h");
	printf("header check: 7 PTE bits, MTYPE, PTE layout, 3 VM flags, COPY_DATA, 4 fault registers, 8 fault fields: %s\n",
	       bad ? "FAILED" : "all match");
	return bad;
}

int main(int argc, char **argv) {
	pte_cases(); placement_cases(); lifecycle_cases(); import_cases(); stream_cases();
	if (argc > 1) fails += header_check(argv[1]);
	else printf("header check skipped: pass the Linux tree root (3857c2fe) as the first argument\n");
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("n33_bo_test: all passed\n");
	return 0;
}
