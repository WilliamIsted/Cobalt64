// n33_gmc_test.cpp: host test of amd/n33_gmc_pure.h and amd/n33_gmc11_fields.h (GMC 11, G0 to G6).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_gmc_test.cpp -o $SCRATCH/n33_gmc_test && \
//       $SCRATCH/n33_gmc_test [<linux tree root>]
// Part 1 is the pure logic against the design's numbers for the measured inputs. Part 2, given the pinned
// Linux tree's root (3857c2fe), parses mmhub_3_0_2_sh_mask.h and gc_11_0_0_sh_mask.h and compares every SHIFT
// and MASK in n33_gmc11_fields.h, then the hub register addresses against the offset headers.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include "n33_gmc_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

using namespace n33gmc;

// Measured on this card (milestone 1 psp check, stage 1): FB_LOCATION_BASE 0x8000, TOP 0x83ff, FB_OFFSET 0,
// RCC_CONFIG_MEMSIZE 0x3ff0. A system aperture of LOW > HIGH counts as disabled.
static constexpr uint64_t kDummy = 0x123456000ull;
static Inputs measured(uint32_t low = 0x3FFFFFFF, uint32_t high = 0) {
	Inputs in = {};
	in.fbBase = 0x8000; in.fbTop = 0x83ff; in.fbOffset = 0; in.memsize = 0x3ff0;
	in.sysLow = low; in.sysHigh = high; in.dummyPa = kDummy;
	return in;
}

static const Entry *find(const Entry *e, uint32_t n, uint32_t reg) {
	for (uint32_t i = 0; i < n; i++) if (e[i].op == kWrite && e[i].reg == reg) return &e[i];
	return nullptr;
}
// The value a write leaves when the register read `before`.
static uint32_t val(const Entry *e, uint32_t n, uint32_t reg, uint32_t before = 0) {
	const Entry *x = find(e, n, reg);
	if (!x) { printf("  no entry for %#x\n", reg); fails++; return 0xDEADBEEF; }
	return target(*x, before);
}

static void geometry_cases() {
	Geo g;
	CHECK(geometry(measured(), &g) == kWhyOk);
	CHECK(g.fbStart == 0x8000000000ull && g.fbEnd == 0x83FEFFFFFFull);   // design: vram_start / fb_end
	CHECK(g.gartStart == 0x8400000000ull && g.gartEnd == 0x840FFFFFFFull);
	CHECK(g.pdBase == 0x08800001ull && g.scratchPa == 0x8880000ull && g.vramBaseOffset == 0);
	Inputs in = measured(); in.memsize = 0;
	CHECK(geometry(in, &g) == kWhyGeometry);
	in = measured(); in.memsize = 0x4001;                                 // VRAM past FB_LOCATION_TOP
	CHECK(geometry(in, &g) == kWhyGeometry);
	in = measured(); in.fbTop = 0x8400;                                   // the FB window would reach the GART
	CHECK(geometry(in, &g) == kWhyGeometry);
	in = measured(); in.fbTop = 0x7fff;                                   // TOP below BASE
	CHECK(geometry(in, &g) == kWhyGeometry);
	in = measured(); in.memsize = 0x88;                                   // mem_scratch past the end of VRAM
	CHECK(geometry(in, &g) == kWhyGeometry);
	in = measured(); in.fbOffset = 0x1;                                   // FB_OFFSET moves the physical addresses
	CHECK(geometry(in, &g) == kWhyOk && g.pdBase == (0x01000000ull + 0x8800000ull + 1) && g.scratchPa == 0x01000000ull + 0x8880000ull);
}

static void aperture_cases() {
	Geo g; uint32_t lo = 0, hi = 0;
	geometry(measured(), &g);
	CHECK(aperture(measured(), g, &lo, &hi) == kWhyOk && lo == 0x200000 && hi == 0x20FFBF);          // disabled: Linux's
	CHECK(aperture(measured(0x200000, 0x20FFFF), g, &lo, &hi) == kWhyOk && lo == 0x200000 && hi == 0x20FFFF);   // never shrinks
	CHECK(aperture(measured(0x200010, 0x200020), g, &lo, &hi) == kWhyOk && lo == 0x200000 && hi == 0x20FFBF);   // grows to cover
	CHECK(aperture(measured(0x100000, 0x20FFBF), g, &lo, &hi) == kWhyOk && lo == 0x100000 && hi == 0x20FFBF);
	CHECK(aperture(measured(0, 0), g, &lo, &hi) == kWhyOk && lo == 0 && hi == 0x20FFBF);              // LOW <= HIGH: enabled
	// Refusal 2: an aperture reaching the GART at 0x8400000000 (>> 18 = 0x210000) would pass GART addresses
	// through untranslated.
	CHECK(aperture(measured(0x200000, 0x210000), g, &lo, &hi) == kWhyApertureGart);
	CHECK(aperture(measured(0x200000, 0x20FFFF), g, &lo, &hi) == kWhyOk);
	CHECK(aperture(measured(0x200000, 0x3FFFFFFF), g, &lo, &hi) == kWhyApertureGart);
	CHECK(aperture(measured(0xC0200000u, 0xC020FFBFu), g, &lo, &hi) == kWhyOk && lo == 0x200000 && hi == 0x20FFBF);   // bits above the field ignored
	// Refusal 1: not covering the FB. min/max keeps Linux's range, so only a geometry the field cannot express
	// reaches it: an FB end at 2^48 (HIGH field wraps to 0).
	Geo big = g; big.fbEnd = 1ull << 48;
	CHECK(aperture(measured(), big, &lo, &hi) == kWhyAperture);
	CHECK(!aperture_covers(0x200001, 0x20FFBF, g.fbStart, g.fbEnd));
	CHECK(!aperture_covers(0x200000, 0x20FFBE, g.fbStart, g.fbEnd));
	CHECK(aperture_covers(0x200000, 0x20FFBF, g.fbStart, g.fbEnd));
	CHECK(!aperture_covers(0x20FFBF, 0x200000, g.fbStart, g.fbEnd));      // LOW > HIGH covers nothing
	CHECK(aperture_hits(0x200000, 0x210000, g.gartStart, g.gartEnd) && !aperture_hits(0x200000, 0x20FFFF, g.gartStart, g.gartEnd));
	// Every plan that writes an aperture carries the refusal.
	Entry e[kPlanMax]; uint32_t n = 99;
	CHECK(plan(2, measured(0x200000, 0x210000), e, kPlanMax, &n) == kWhyApertureGart && n == 0);
	CHECK(plan(6, measured(0x200000, 0x210000), e, kPlanMax, &n) == kWhyApertureGart && n == 0);
	CHECK(plan(3, measured(0x200000, 0x210000), e, kPlanMax, &n) == kWhyOk);   // G3 writes no aperture
}

static void plan_g1_g2_g3() {
	Entry e[kPlanMax]; uint32_t n = 0;
	CHECK(plan(1, measured(), e, kPlanMax, &n) == kWhyOk && n == 3);
	CHECK(val(e, n, 0xE4D) == 0x7F000 && val(e, n, 0xE4E) == 0x7F004);
	CHECK(val(e, n, 0xDE0, 0x10) == 0x11 && find(e, n, 0xDE0)->mask == 1);   // RMW: bit 0 only

	CHECK(plan(2, measured(), e, kPlanMax, &n) == kWhyOk && n == 16);
	// Row 1, the design's values for our placement.
	CHECK(val(e, n, 0x1A72B) == 0x08800001 && val(e, n, 0x1A72C) == 0);
	CHECK(val(e, n, 0x1A74B) == 0x08400000 && val(e, n, 0x1A74C) == 0);
	CHECK(val(e, n, 0x1A76B) == 0x0840FFFF && val(e, n, 0x1A76C) == 0);
	CHECK(e[0].reg == 0x1A72B && e[5].reg == 0x1A76C);                    // Linux's order: row 1, then row 2
	// Row 2.
	CHECK(val(e, n, 0x1A870) == 0 && val(e, n, 0x1A86F) == 0xFFFFFF && val(e, n, 0x1A86E) == 0);
	CHECK(val(e, n, 0x1A871) == 0x200000 && val(e, n, 0x1A872) == 0x20FFBF);
	CHECK(val(e, n, 0x1A858) == 0x8880 && val(e, n, 0x1A859) == 0);
	CHECK(val(e, n, 0x1A68F) == (uint32_t)(kDummy >> 12) && val(e, n, 0x1A690) == 0);
	CHECK(val(e, n, 0x1A689, 0x12345678) == (0x12345678u | 0x40000u) && find(e, n, 0x1A689)->mask == 0x40000);
	CHECK(find(e, n, 0x1A872)->cmp == 0x3FFFFFFF && find(e, n, 0x1A86F)->cmp == 0xFFFFFF);

	CHECK(plan(3, measured(), e, kPlanMax, &n) == kWhyOk && n == 6);
	CHECK(find(e, n, 0x1A873)->mask == 0x3FF9 && val(e, n, 0x1A873) == 0x1859);   // design row 3
	CHECK(val(e, n, 0x1A873, 0xFFFFFFFF) == ((0xFFFFFFFFu & ~0x3FF9u) | 0x1859u));
	CHECK(find(e, n, 0x1A680)->mask == 0x03FC0903 && val(e, n, 0x1A680) == 0x00080801);
	CHECK(find(e, n, 0x1A681)->mask == 0x3 && val(e, n, 0x1A681) == 0x3 && find(e, n, 0x1A681)->cmp == 0);   // live invalidate, self-clearing
	CHECK(val(e, n, 0x1A682, 0xFFFFFFFF) == 0x80130009);                   // whole-register writes ignore before
	CHECK(val(e, n, 0x1A698) == 0x00000001 && val(e, n, 0x1A69E) == 0x00003FE0);
	CHECK(find(e, n, 0x1A682)->cmp == 0xFFFF9FFF && find(e, n, 0x1A69E)->cmp == 0x3FFFF);
}

static void plan_g4() {
	Entry e[kPlanMax]; uint32_t n = 0;
	CHECK(plan(4, measured(), e, kPlanMax, &n) == kWhyOk && n == 121);
	CHECK(e[0].reg == 0x1A6C0 && e[0].mask == 0x87 && e[0].value == 1);   // ENABLE 1, depth 0, retry 0
	CHECK(val(e, n, 0x1A692) == 0xFFFFFFFF && val(e, n, 0x1A693) == 0xF);
	for (uint32_t r = 0x1A694; r <= 0x1A697; r++) CHECK(val(e, n, r) == 0);
	for (uint32_t c = 1; c <= 15; c++) {
		const Entry *x = find(e, n, 0x1A6C0 + c);
		CHECK(x && x->mask == 0x005554FF && x->value == 0x00555407);         // design row 7, retry 0
		CHECK(val(e, n, 0x1A74D + 2 * (c - 1)) == 0 && val(e, n, 0x1A74E + 2 * (c - 1)) == 0);
		CHECK(val(e, n, 0x1A76D + 2 * (c - 1)) == 0xFFFFFFFF && val(e, n, 0x1A76E + 2 * (c - 1)) == 0xF);
	}
	CHECK(find(e, n, 0x1A78A) && !find(e, n, 0x1A78C));                    // context 15's END_HI32 is the last
	for (uint32_t i = 0; i < 18; i++) CHECK(val(e, n, 0x1A707 + 2 * i) == 0xFFFFFFFF && val(e, n, 0x1A708 + 2 * i) == 0x1F);
	CHECK(e[n - 3].op == kHdpFlush);
	CHECK(e[n - 2].reg == 0x1A688 && e[n - 2].mask == 0x1FFC && e[n - 2].value == 0x1FFC);   // eleven defaults, no CRASH bits
	CHECK(e[n - 1].op == kFlushMm);
	CHECK(!find(e, n, 0x1A6E2));                                           // never the semaphore
	CHECK(kMmV.req == 0x00F80001 && kGcV.req == 0x00F80001);
	CHECK(kMm.invSem17 == 0x1A6E2 && kMm.invReq17 == 0x1A6F4 && kMm.invAck17 == 0x1A706 && kMm.cid2 == 0x1A69B);
	CHECK(n33gmcf::MMVM_L2_BANK_SELECT_RESERVED_CID2__RESERVED_CACHE_PRIVATE_INVALIDATION.mask == (1u << 25));
}

static void plan_g6() {
	Entry e[kPlanMax]; uint32_t n = 0;
	CHECK(plan(6, measured(), e, kPlanMax, &n) == kWhyOk && n == 144);
	CHECK(val(e, n, 0x2953) == 0x08800001 && val(e, n, 0x2973) == 0x08400000 && val(e, n, 0x2993) == 0x0840FFFF);
	CHECK(val(e, n, 0x28DD) == 0x200000 && val(e, n, 0x28DE) == 0x20FFBF && val(e, n, 0x28DB) == 0xFFFFFF);
	CHECK(val(e, n, 0x2808) == 0x8880 && val(e, n, 0x282B) == (uint32_t)(kDummy >> 12));
	CHECK(val(e, n, 0x28DF) == 0x1859 && val(e, n, 0x281E) == 0x80130009);    // GC default 0x80120007, same result
	CHECK(find(e, n, 0x283A)->cmp == 0x1FFFF);
	CHECK(val(e, n, 0x28E8) == 1 && val(e, n, 0x28E9) == 0x00555407 && val(e, n, 0x28F7) == 0x00555407);
	CHECK(val(e, n, 0x292F) == 0xFFFFFFFF && val(e, n, 0x2952) == 0x1F);
	CHECK(e[n - 4].op == kHdpFlush);
	CHECK(e[n - 3].reg == kCpDebug && e[n - 3].mask == 0x8000 && e[n - 3].value == 0x8000);
	CHECK(e[n - 2].reg == 0x2824 && e[n - 2].mask == 0xC0001FFC && e[n - 2].value == 0x1FFC);   // CRASH_ON_* = 0
	CHECK(e[n - 1].op == kFlushGc);
	for (uint32_t i = 0; i < n; i++) CHECK(e[i].op != kWrite || e[i].reg < 0x16000);   // GC and nothing on MMHUB
	CHECK(kGc.invSem17 == 0 && kGc.cid2 == 0 && kGc.invReq17 == 0x291C && kGc.invAck17 == 0x292E);
	// Bad step numbers and a short buffer.
	CHECK(plan(5, measured(), e, kPlanMax, &n) == kWhyArg && n == 0);
	CHECK(plan(0, measured(), e, kPlanMax, &n) == kWhyArg);
	CHECK(plan(4, measured(), e, 100, &n) == kWhyArg && n == 0);
}

static void pte_cases() {
	CHECK(pte_sys(0x123456000ull) == 0x0003000123456077ull);
	CHECK((pte_sys(0x0000FFFFFFFFF000ull) >> 63) == 0);                    // no IS_PTE
	CHECK(pte_sys(0x123456FFFull) == 0x0003000123456077ull);               // low bits dropped
	CHECK(pte_vram(0x2000000) == 0x0003000002000071ull && !(pte_vram(0x2000000) & kPteSystem));
	CHECK(pte_unbound(kDummy) == kDummy && (pte_unbound(kDummy) & kPteValid) == 0);
	CHECK(slot_mc(0) == 0x8400000000ull && slot_mc(kGartPages - 1) == 0x840FFFF000ull);
	CHECK(slot_off(0) == 0x8800000 && slot_off(kGartPages - 1) == 0x887FFF8);
	CHECK(host_pa_ok(0x1000) && host_pa_ok((1ull << 44) - 0x1000) && !host_pa_ok(1ull << 44) && !host_pa_ok(0) && !host_pa_ok(0x1800));
}

static void order_cases() {
	State s = {};
	const uint32_t steps[5] = { 1, 2, 3, 4, 6 };
	for (uint32_t n : steps) CHECK(may_step(s, n, true) == kWhyOrder);    // no G0
	CHECK(may_step(s, 5, true) == kWhyArg && may_step(s, 0, true) == kWhyArg && may_step(s, 99, true) == kWhyArg);
	CHECK(may_step(s, 6, false) == kWhyGcDown);                            // the GC gate comes before order
	CHECK(snapshot_fresh(s));
	s.g0 = true;
	CHECK(may_step(s, 3, true) == kWhyOrder);                              // negative: 3 before 2
	CHECK(may_step(s, 2, true) == kWhyOrder);
	CHECK(may_step(s, 1, true) == kWhyOk);
	s.attempted |= 1u << 1; record_step(s, 1, true);
	CHECK(!snapshot_fresh(s));
	CHECK(may_step(s, 1, true) == kWhyOrder);                              // twice
	CHECK(may_step(s, 2, true) == kWhyOk && may_step(s, 4, true) == kWhyOrder);
	record_step(s, 2, true); record_step(s, 3, true);
	CHECK(may_step(s, 6, true) == kWhyOrder);                              // G6 needs G4
	CHECK(may_step(s, 6, false) == kWhyGcDown);                            // negative: 6 before 2c
	record_step(s, 4, true);
	CHECK(may_step(s, 6, true) == kWhyOk && may_step(s, 6, false) == kWhyGcDown);
	record_step(s, 6, false);
	CHECK(s.stopped && !(s.done >> 6 & 1u));
	CHECK(may_step(s, 6, true) == kWhyStopped);
	State t = {}; t.g0 = true;
	t.attempted |= 1u << 1; record_step(t, 1, false);                     // a failed step stops the ladder
	CHECK(may_step(t, 2, true) == kWhyStopped && may_step(t, 1, true) == kWhyStopped);
}

static void precondition_cases() {
	Snap a = {};
	a.fbBase = 0x8000; a.fbTop = 0x83ff; a.fbOffset = 0;
	a.dcn[0] = 0x8000; a.dcn[1] = 0x83ff; a.hubpLo = 0; a.hubpHi = 0x80; a.pitch = 0x9FF;
	Snap b = a;
	CHECK(preconditions(a, b, 0) == kWhyOk);
	CHECK(preconditions(a, b, 0x200) == kWhyFault);
	b.fbBase = 0x8001; CHECK(preconditions(a, b, 0) == kWhyChanged); b = a;
	b.fbOffset = 1; CHECK(preconditions(a, b, 0) == kWhyChanged); b = a;
	b.dcn[6] = 1; CHECK(preconditions(a, b, 0) == kWhyChanged); b = a;
	b.hubpLo = 0x2000000; CHECK(preconditions(a, b, 0) == kWhyChanged); b = a;    // HUBP0 flipped since G0
	b.pitch = 0x9FE; CHECK(preconditions(a, b, 0) == kWhyChanged); b = a;
	Snap c = a; c.dcn[0] = 0x8100;                                        // DCN FB base != MMHUB FB base
	CHECK(preconditions(c, c, 0) == kWhyChanged);
	CHECK(preconditions(a, b, 0x200) == kWhyFault);
}

static void map_cases() {
	CHECK(map_args(0, 4, kMapHost, 0) == kWhyOk);
	CHECK(map_args(65536, 1, kMapVram, 0x2000000) == kWhyRange);          // negative: outside the GART
	CHECK(map_args(65535, 2, kMapHost, 0) == kWhyRange);
	CHECK(map_args(65535, 1, kMapHost, 0) == kWhyOk);
	CHECK(map_args(0xFFFFFFFFu, 16, kMapHost, 0) == kWhyRange);           // no wrap
	CHECK(map_args(0, 0, kMapHost, 0) == kWhyArg && map_args(0, 17, kMapVram, 0) == kWhyArg);
	CHECK(map_args(0, 1, kMapHost, 4) == kWhyArg && map_args(0, 2, kMapHost, 3) == kWhyArg && map_args(0, 1, kMapHost, 3) == kWhyOk);
	CHECK(map_args(0, 1, kMapVram, 0x2000010) == kWhyArg && map_args(0, 1, 2, 0) == kWhyArg);
	CHECK(map_args(0, 1, kMapVram, 0x0) == kWhyOk);                       // the GOP scanout: refused by the kext's n33WriteAllowed next
	CHECK(unmap_args(65536, 1) == kWhyRange && unmap_args(0, 0) == kWhyArg && unmap_args(0, 16) == kWhyOk);
	State s = {};
	CHECK(map_state(s) == kWhyOrder);                                      // no table before G2
	CHECK(may_flush(s) == kWhyOrder);
	s.tableOwned = true; CHECK(map_state(s) == kWhyOk);
	s.done = 1u << 4; CHECK(may_flush(s) == kWhyOk);
	s.stopped = true; CHECK(map_state(s) == kWhyStopped && may_flush(s) == kWhyStopped);
	CHECK(flush_ok(false, kFlushSem | kFlushAck | kFlushCid2) && !flush_ok(false, kFlushAck | kFlushCid2) && !flush_ok(false, kFlushSem | kFlushCid2));
	CHECK(flush_ok(true, kFlushAck) && !flush_ok(true, 0) && !flush_ok(false, 0));
}

static void restore_cases() {
	State s = {};
	CHECK(may_restore(s, 0, false, false) == kWhyOrder);                   // nothing written
	CHECK(may_restore(s, 3, false, false) == kWhyOk);
	CHECK(may_restore(s, 3, true, false) == kWhyGcDown && may_restore(s, 3, true, true) == kWhyOk);
	s.engineHolds = true;
	CHECK(may_restore(s, 3, false, false) == kWhyEngine);
	s = {}; s.stopped = true;                                             // a failed step: restore stays available
	CHECK(may_restore(s, 3, false, false) == kWhyOk);
	s = {}; s.g0 = true; s.done = (1u << 1) | (1u << 2);
	record_restore(s);
	CHECK(s.restored && s.stopped && s.done == 0);
	CHECK(may_restore(s, 3, false, false) == kWhyOrder);                   // once
	CHECK(may_step(s, 1, true) == kWhyStopped);
	// Newest first: journal [a, b, c] is written back c, b, a, each with its before-value.
	Rec j[3] = { { 1, 0xE4D, 0x0, 0x7F000, 0x7F000, kRemapAddrMask }, { 1, 0xE4E, 0x5, 0x7F004, 0x7F004, kRemapAddrMask },
	             { 1, 0xDE0, 0x10, 0x11, 0x11, 1 } };
	CHECK(restore_index(0, 3) == 2 && restore_index(1, 3) == 1 && restore_index(2, 3) == 0);
	CHECK(j[restore_index(0, 3)].reg == 0xDE0 && j[restore_index(0, 3)].before == 0x10);
	CHECK(!mismatch(j[0]));
	Rec m = j[0]; m.after = 0x7F001; CHECK(!mismatch(m));                  // bit 0 is outside the ADDRESS field
	m.after = 0x7E000; CHECK(mismatch(m));
	Rec inv = { 3, 0x1A681, 0, 0, 3, 0 }; CHECK(!mismatch(inv));          // self-clearing: recorded only
	// D6 at unload.
	s = {};
	Unload u = unload_plan(s);
	CHECK(!u.unmapAll && !u.flushMm && !u.flushGc);
	s.tableOwned = true; s.attempted = (1u << 2);
	u = unload_plan(s);
	CHECK(u.unmapAll && !u.flushMm && !u.flushGc);
	s.attempted |= (1u << 3) | (1u << 4) | (1u << 6);
	u = unload_plan(s);
	CHECK(u.unmapAll && u.flushMm && u.flushGc);
	CHECK(free_host_pages(s, true) && !free_host_pages(s, false));
	s.engineHolds = true; CHECK(!free_host_pages(s, true));
}

// Layout (D2, D3) against the rest of the N33 VRAM map.
static_assert(kHdpRemapDword == 0x1FC00, "BAR5 byte 0x7F000");
static_assert(kPteFlagsSys == 0x0003000000000077ull && kPteFlagsVram == 0x0003000000000071ull, "PTE flags, no bit 63");
static_assert(((kPteFlagsSys >> 48) & 7) == 3, "MTYPE UC at 48..50");
static_assert(kDmaPhysMask == 0x00000FFFFFFFF000ull, "44-bit page mask");
static_assert(kTableLen == 512 * 1024 && kGartPages == 65536, "512 KiB table, 65536 PTEs");
static_assert(kScratchOff == kTableOff + kTableLen && kOwnedLen == 0x81000, "mem_scratch right after the table");
static_assert(kTableOff >= 0x8700000, "above the SMU driver table window (0x8600000..0x8700000) and the m2 pages");
static_assert(kOwnedOff + kOwnedLen <= 0x8A00000 && kOwnedOff + kOwnedLen <= (256ull << 20), "inside the step's reserve and BAR0");
static_assert(kTableOff % kPage == 0 && kTableOff % 64 == 0 && kScratchOff % kPage == 0, "aligned");
static_assert(kGartMc % (1ull << 32) == 0 && kGartMc + kGartSize <= kHole, "D2: 4 GiB aligned, below the hole");
static_assert(kMmV.l1Val == 0x1859 && kMmV.l1Mask == 0x3FF9 && kMmV.ctx1Val == 0x00555407, "design rows 3 and 7");
static_assert(kMmV.l2c3 == 0x80130009 && kGcV.l2c3 == 0x80130009 && kMmV.l2c4 == 1 && kMmV.l2c5 == 0x3FE0, "design row 4");
static_assert(kMmV.faultMask == 0x1FFC && kGcV.faultMask == 0xC0001FFC && kGcV.faultVal == 0x1FFC, "design row 10");
static_assert(kMm.ctx0Cntl + 15 == 0x1A6CF && kMm.invRangeLo + 35 == 0x1A72A, "MMHUB strides");
static_assert(kGc.ctx0Cntl + 15 == 0x28F7 && kGc.invRangeLo + 35 == 0x2952, "GFXHUB strides");
// CONTEXTn_CNTL on GMC 11 sits one bit lower than the fork's GMC 12 file (design item 2).
static_assert(n33gmcf::MMVM_CONTEXT1_CNTL__PAGE_TABLE_BLOCK_SIZE.shift == 3 &&
              n33gmcf::MMVM_CONTEXT1_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT.shift == 7 &&
              n33gmcf::MMVM_CONTEXT1_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT.shift == 0xa &&
              n33gmcf::GCVM_CONTEXT1_CNTL__EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT.shift == 0x16, "GMC 11 shifts");

// ---- Part 2: the field file and the hub addresses against the Linux headers ------------------------------
using Defs = std::map<std::string, uint64_t>;
static bool load_defs(const std::string &path, Defs &d) {
	std::ifstream f(path);
	if (!f) { printf("cannot open %s\n", path.c_str()); return false; }
	std::string line;
	while (std::getline(f, line)) {
		if (line.compare(0, 8, "#define ") != 0) continue;
		const size_t a = line.find_first_not_of(" \t", 8), b = line.find_first_of(" \t", a);
		if (a == std::string::npos || b == std::string::npos) continue;
		const size_t c = line.find_first_not_of(" \t", b);
		if (c == std::string::npos) continue;
		d[line.substr(a, b - a)] = strtoull(line.c_str() + c, nullptr, 0);   // "0x1fL" parses up to the L
	}
	return true;
}

struct FieldRow { const char *reg, *field; uint32_t shift, mask; };
#define N33_ROW(R, F, S, M) { #R, #F, S, M },
static const FieldRow kMmRows[] = { N33_GMC11_MM_FIELDS(N33_ROW) };
static const FieldRow kGcRows[] = { N33_GMC11_GC_FIELDS(N33_ROW) };
#undef N33_ROW
struct BitsRow { const char *reg; uint32_t bits; };
#define N33_BROW(R, M) { #R, M },
static const BitsRow kMmBits[] = { N33_GMC11_MM_REGBITS(N33_BROW) };
static const BitsRow kGcBits[] = { N33_GMC11_GC_REGBITS(N33_BROW) };
#undef N33_BROW

static int check_fields(const Defs &d, const FieldRow *rows, size_t n, const char *what) {
	int bad = 0;
	for (size_t i = 0; i < n; i++) {
		const std::string base = std::string(rows[i].reg) + "__" + rows[i].field;
		auto s = d.find(base + "__SHIFT"), m = d.find(base + "_MASK");
		if (s == d.end() || m == d.end()) { printf("FAIL %s: %s not in the header\n", what, base.c_str()); bad++; continue; }
		if (s->second != rows[i].shift || m->second != rows[i].mask) {
			printf("FAIL %s: %s shift %#llx mask %#llx, header %#llx %#llx\n", what, base.c_str(), (unsigned long long)rows[i].shift,
			       (unsigned long long)rows[i].mask, (unsigned long long)s->second, (unsigned long long)m->second);
			bad++;
		}
	}
	return bad;
}
static int check_bits(const Defs &d, const BitsRow *rows, size_t n, const char *what) {
	int bad = 0;
	for (size_t i = 0; i < n; i++) {
		const std::string pre = std::string(rows[i].reg) + "__";
		uint64_t u = 0; int fields = 0;
		for (auto &kv : d) {
			const std::string &k = kv.first;
			if (k.compare(0, pre.size(), pre) == 0 && k.size() > 5 && k.compare(k.size() - 5, 5, "_MASK") == 0) { u |= kv.second; fields++; }
		}
		if (!fields || u != rows[i].bits) { printf("FAIL %s: %s bits %#x, header union %#llx (%d fields)\n", what, rows[i].reg, rows[i].bits, (unsigned long long)u, fields); bad++; }
	}
	return bad;
}

struct AddrRow { const char *name; uint32_t abs, seg, idx; };
static int check_addrs(const Defs &d, const AddrRow *rows, size_t n, const char *what) {
	int bad = 0;
	for (size_t i = 0; i < n; i++) {
		const std::string r = std::string("reg") + rows[i].name;
		auto o = d.find(r), b = d.find(r + "_BASE_IDX");
		if (o == d.end() || b == d.end()) { printf("FAIL %s: %s not in the header\n", what, r.c_str()); bad++; continue; }
		if (o->second + rows[i].seg != rows[i].abs || b->second != rows[i].idx) {
			printf("FAIL %s: %s at %#x, header %#llx + %#x (base %llu)\n", what, rows[i].name, rows[i].abs,
			       (unsigned long long)o->second, rows[i].seg, (unsigned long long)b->second);
			bad++;
		}
	}
	return bad;
}

static int header_check(const char *root) {
	const std::string inc = std::string(root) + "/drivers/gpu/drm/amd/include/asic_reg/";
	Defs mmMask, gcMask, mmOff, gcOff, nbOff;
	if (!load_defs(inc + "mmhub/mmhub_3_0_2_sh_mask.h", mmMask) || !load_defs(inc + "gc/gc_11_0_0_sh_mask.h", gcMask) ||
	    !load_defs(inc + "mmhub/mmhub_3_0_2_offset.h", mmOff) || !load_defs(inc + "gc/gc_11_0_0_offset.h", gcOff) ||
	    !load_defs(inc + "nbio/nbio_4_3_0_offset.h", nbOff)) return 1;
	int bad = 0;
	bad += check_fields(mmMask, kMmRows, sizeof kMmRows / sizeof kMmRows[0], "mmhub_3_0_2_sh_mask.h");
	bad += check_fields(gcMask, kGcRows, sizeof kGcRows / sizeof kGcRows[0], "gc_11_0_0_sh_mask.h");
	bad += check_bits(mmMask, kMmBits, sizeof kMmBits / sizeof kMmBits[0], "mmhub_3_0_2_sh_mask.h");
	bad += check_bits(gcMask, kGcBits, sizeof kGcBits / sizeof kGcBits[0], "gc_11_0_0_sh_mask.h");
	const uint32_t mm = 0x1A000, gc = 0x1260;
	const AddrRow mmRows[] = {
		{ "MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32", kMm.ptBaseLo, mm, 0 }, { "MMVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32", kMm.ptBaseLo + 1, mm, 0 },
		{ "MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32", kMm.ptStartLo, mm, 0 }, { "MMVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32", kMm.ptStartLo + 1, mm, 0 },
		{ "MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32", kMm.ptEndLo, mm, 0 }, { "MMVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32", kMm.ptEndLo + 1, mm, 0 },
		{ "MMVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32", kMm.ctx1PtStartLo, mm, 0 }, { "MMVM_CONTEXT15_PAGE_TABLE_START_ADDR_HI32", kMm.ctx1PtStartLo + 29, mm, 0 },
		{ "MMVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32", kMm.ctx1PtEndLo, mm, 0 }, { "MMVM_CONTEXT15_PAGE_TABLE_END_ADDR_HI32", kMm.ctx1PtEndLo + 29, mm, 0 },
		{ "MMMC_VM_AGP_TOP", kMm.agpTop, mm, 0 }, { "MMMC_VM_AGP_BOT", kMm.agpBot, mm, 0 }, { "MMMC_VM_AGP_BASE", kMm.agpBase, mm, 0 },
		{ "MMMC_VM_SYSTEM_APERTURE_LOW_ADDR", kMm.sysLow, mm, 0 }, { "MMMC_VM_SYSTEM_APERTURE_HIGH_ADDR", kMm.sysHigh, mm, 0 },
		{ "MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB", kMm.defLsb, mm, 0 }, { "MMMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB", kMm.defMsb, mm, 0 },
		{ "MMVM_L2_PROTECTION_FAULT_CNTL", kMm.faultCntl, mm, 0 }, { "MMVM_L2_PROTECTION_FAULT_CNTL2", kMm.faultCntl2, mm, 0 },
		{ "MMVM_L2_PROTECTION_FAULT_STATUS", kMm.faultStatus, mm, 0 },
		{ "MMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32", kMm.faultDefLo, mm, 0 }, { "MMVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32", kMm.faultDefHi, mm, 0 },
		{ "MMMC_VM_MX_L1_TLB_CNTL", kMm.l1Tlb, mm, 0 }, { "MMVM_L2_CNTL", kMm.l2Cntl, mm, 0 }, { "MMVM_L2_CNTL2", kMm.l2Cntl2, mm, 0 },
		{ "MMVM_L2_CNTL3", kMm.l2Cntl3, mm, 0 }, { "MMVM_L2_CNTL4", kMm.l2Cntl4, mm, 0 }, { "MMVM_L2_CNTL5", kMm.l2Cntl5, mm, 0 },
		{ "MMVM_CONTEXT0_CNTL", kMm.ctx0Cntl, mm, 0 }, { "MMVM_CONTEXT15_CNTL", kMm.ctx0Cntl + 15, mm, 0 },
		{ "MMVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32", kMm.identity, mm, 0 },
		{ "MMVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32", kMm.identity + 5, mm, 0 },
		{ "MMVM_INVALIDATE_ENG0_ADDR_RANGE_LO32", kMm.invRangeLo, mm, 0 }, { "MMVM_INVALIDATE_ENG17_ADDR_RANGE_HI32", kMm.invRangeLo + 35, mm, 0 },
		{ "MMVM_INVALIDATE_ENG17_SEM", kMm.invSem17, mm, 0 }, { "MMVM_INVALIDATE_ENG17_REQ", kMm.invReq17, mm, 0 },
		{ "MMVM_INVALIDATE_ENG17_ACK", kMm.invAck17, mm, 0 }, { "MMVM_L2_BANK_SELECT_RESERVED_CID2", kMm.cid2, mm, 0 },
		{ "MMMC_VM_FB_LOCATION_BASE", kMm.fbBase, mm, 0 }, { "MMMC_VM_FB_LOCATION_TOP", kMm.fbTop, mm, 0 }, { "MMMC_VM_FB_OFFSET", kMm.fbOffset, mm, 0 },
	};
	const AddrRow gcRows[] = {
		{ "GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32", kGc.ptBaseLo, gc, 0 }, { "GCVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32", kGc.ptStartLo, gc, 0 },
		{ "GCVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32", kGc.ptEndLo, gc, 0 },
		{ "GCVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32", kGc.ctx1PtStartLo, gc, 0 }, { "GCVM_CONTEXT15_PAGE_TABLE_END_ADDR_HI32", kGc.ctx1PtEndLo + 29, gc, 0 },
		{ "GCMC_VM_AGP_TOP", kGc.agpTop, gc, 0 }, { "GCMC_VM_AGP_BOT", kGc.agpBot, gc, 0 }, { "GCMC_VM_AGP_BASE", kGc.agpBase, gc, 0 },
		{ "GCMC_VM_SYSTEM_APERTURE_LOW_ADDR", kGc.sysLow, gc, 0 }, { "GCMC_VM_SYSTEM_APERTURE_HIGH_ADDR", kGc.sysHigh, gc, 0 },
		{ "GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB", kGc.defLsb, gc, 0 }, { "GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB", kGc.defMsb, gc, 0 },
		{ "GCVM_L2_PROTECTION_FAULT_CNTL", kGc.faultCntl, gc, 0 }, { "GCVM_L2_PROTECTION_FAULT_CNTL2", kGc.faultCntl2, gc, 0 },
		{ "GCVM_L2_PROTECTION_FAULT_STATUS", kGc.faultStatus, gc, 0 },
		{ "GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32", kGc.faultDefLo, gc, 0 }, { "GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32", kGc.faultDefHi, gc, 0 },
		{ "GCMC_VM_MX_L1_TLB_CNTL", kGc.l1Tlb, gc, 0 }, { "GCVM_L2_CNTL", kGc.l2Cntl, gc, 0 }, { "GCVM_L2_CNTL2", kGc.l2Cntl2, gc, 0 },
		{ "GCVM_L2_CNTL3", kGc.l2Cntl3, gc, 0 }, { "GCVM_L2_CNTL4", kGc.l2Cntl4, gc, 0 }, { "GCVM_L2_CNTL5", kGc.l2Cntl5, gc, 0 },
		{ "GCVM_CONTEXT0_CNTL", kGc.ctx0Cntl, gc, 0 }, { "GCVM_CONTEXT15_CNTL", kGc.ctx0Cntl + 15, gc, 0 },
		{ "GCVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32", kGc.identity, gc, 0 },
		{ "GCVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32", kGc.identity + 5, gc, 0 },
		{ "GCVM_INVALIDATE_ENG0_ADDR_RANGE_LO32", kGc.invRangeLo, gc, 0 }, { "GCVM_INVALIDATE_ENG17_ADDR_RANGE_HI32", kGc.invRangeLo + 35, gc, 0 },
		{ "GCVM_INVALIDATE_ENG17_REQ", kGc.invReq17, gc, 0 }, { "GCVM_INVALIDATE_ENG17_ACK", kGc.invAck17, gc, 0 },
		{ "GCMC_VM_FB_LOCATION_BASE", kGc.fbBase, gc, 0 }, { "GCMC_VM_FB_LOCATION_TOP", kGc.fbTop, gc, 0 }, { "GCMC_VM_FB_OFFSET", kGc.fbOffset, gc, 0 },
		{ "CP_DEBUG", kCpDebug, gc, 0 }, { "CP_STAT", kCpStat, gc, 0 }, { "RLC_RLCS_BOOTLOAD_STATUS", kRlcBootload, 0xA000, 1 },
	};
	const AddrRow nbRows[] = {
		{ "BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL", kRemapHdpMem, 0xD20, 2 }, { "BIF_BX0_REMAP_HDP_REG_FLUSH_CNTL", kRemapHdpReg, 0xD20, 2 },
		{ "RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN", kDoorbellAperEn, 0xD20, 2 },
		{ "BIF_BX_DEV0_EPF0_VF0_HDP_MEM_COHERENCY_FLUSH_CNTL", kHdpFlushDirect, 0xD20, 2 },
		{ "RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE", kMemsize, 0xD20, 2 }, { "BIF_BX0_BIF_FB_EN", kFbEn, 0xD20, 2 },
	};
	bad += check_addrs(mmOff, mmRows, sizeof mmRows / sizeof mmRows[0], "mmhub_3_0_2_offset.h");
	bad += check_addrs(gcOff, gcRows, sizeof gcRows / sizeof gcRows[0], "gc_11_0_0_offset.h");
	bad += check_addrs(nbOff, nbRows, sizeof nbRows / sizeof nbRows[0], "nbio_4_3_0_offset.h");
	printf("header check: %zu MMHUB and %zu GC fields, %zu + %zu register widths, %zu + %zu + %zu addresses: %s\n",
	       sizeof kMmRows / sizeof kMmRows[0], sizeof kGcRows / sizeof kGcRows[0], sizeof kMmBits / sizeof kMmBits[0],
	       sizeof kGcBits / sizeof kGcBits[0], sizeof mmRows / sizeof mmRows[0], sizeof gcRows / sizeof gcRows[0],
	       sizeof nbRows / sizeof nbRows[0], bad ? "FAILED" : "all match");
	return bad;
}

int main(int argc, char **argv) {
	geometry_cases(); aperture_cases(); plan_g1_g2_g3(); plan_g4(); plan_g6(); pte_cases(); order_cases();
	precondition_cases(); map_cases(); restore_cases();
	if (argc > 1) fails += header_check(argv[1]);
	else printf("header check skipped: pass the Linux tree root (3857c2fe) as the first argument\n");
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("n33_gmc_test: all passed\n");
	return 0;
}
