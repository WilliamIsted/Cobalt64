// n33_sdma_test.cpp: host test of amd/n33_sdma_pure.h (stage 3 step 5: C1, SDMA0 queue 0 in mmio and doorbell mode).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_sdma_test.cpp -o $SCRATCH/n33_sdma_test && \
//       $SCRATCH/n33_sdma_test [<linux tree root>]
// Part 1 is the pure logic against the design's numbers (packet encodings, both modes' register values, the
// doorbell offset, the mode-switch and latch rules). Part 2, given the pinned Linux tree's root (3857c2fe), checks
// every register address and field mask used here against gc_11_0_0_offset.h / _sh_mask.h and nbio_4_3_0_*.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include "n33_sdma_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

using namespace n33sdma;

static constexpr uint64_t kFbBase = 0x8000000000ull;   // MMHUB FB_LOCATION_BASE 0x8000 << 24, measured

// Reset defaults (gc_11_0_0_default.h) where the design quotes them; 0 elsewhere.
static uint32_t reset_default(uint32_t reg) {
	switch (reg) {
	case kRbCntl: return 0x00040800u;
	case kF32Cntl: return 0x08084001u;
	case kUtcl1Page: return 0x010CEC00u;
	default: return 0;
	}
}

// Applies a plan to a register file that starts at the reset defaults (or at `start` when given), as the kext does.
struct Regs {
	std::map<uint32_t, uint32_t> v;
	uint32_t get(uint32_t r) { auto it = v.find(r); return it == v.end() ? reset_default(r) : it->second; }
	void run(const Entry *e, uint32_t n) {
		for (uint32_t i = 0; i < n; i++) if (e[i].op != kOpRptrCheck) v[e[i].reg] = target(e[i], get(e[i].reg));
	}
};
static int find(const Entry *e, uint32_t n, uint32_t reg, uint32_t from = 0) {
	for (uint32_t i = from; i < n; i++) if (e[i].reg == reg && e[i].op != kOpRptrCheck) return (int)i;
	return -1;
}

static void packet_cases() {
	uint32_t p[kAlignDw] = {};
	const Geo g = geo(kFbBase);
	CHECK(g.ring == 0x8008A00000ull && g.rptr == 0x8008A01000ull && g.wptr == 0x8008A01020ull && g.test == 0x8008A02000ull);
	CHECK(pkt_write(p, g.test, kAfter) == 5);
	CHECK(p[0] == 0x00000002u && p[1] == 0x08A02000u && p[2] == 0x80u && p[3] == 0 && p[4] == 0xDEADBEEFu);
	CHECK(pad(p, 5) == 16);
	for (int i = 5; i < 16; i++) CHECK(p[i] == 0);
	memset(p, 0xAB, sizeof p);
	CHECK(pkt_fence(p, 0x8400000008ull, 7) == 4);
	CHECK(p[0] == 0x00030005u && p[1] == 0x00000008u && p[2] == 0x84u && p[3] == 7);
	CHECK(pad(p, 4) == 16 && p[4] == 0 && p[15] == 0);
	CHECK(pad(p, 16) == 16);
	CHECK(kBefore == 0xCAFEDEADu && kAfter == 0xDEADBEEFu);
	CHECK(wptr_bytes(16) == 0x40 && wptr_bytes(32) == 0x80);
}

static void doorbell_cases() {
	CHECK(kDbIndex == (0x100u << 1));                   // AMDGPU_NAVI10_DOORBELL_sDMA_ENGINE0 << 1
	CHECK(kDbByte == 0x800);                            // dword index * 4 = qword index 0x100 * 8
	CHECK(kBar2Need == 0x808 && kBar2Need <= (2u << 20));   // fits the 2 MiB BAR2 measured on macOS
	CHECK((kDbIndex << kDbOffShift) == 0x800 && ((kDbIndex << kDbOffShift) & ~kDbOffMask) == 0);
	// S2A entry 2: enable, AWID 0xE, offset 0x200, size 4, AWADDR 3, each inside its field.
	CHECK(kS2aValue == 0x3009001Du);
	CHECK((kS2aValue & kS2aEnable) == 1);
	CHECK(((kS2aValue & kS2aAwidMask) >> 1) == 0xE);
	CHECK(((kS2aValue & kS2aOffMask) >> 7) == kDbIndex);
	CHECK(((kS2aValue & kS2aSizeMask) >> 17) == 4);
	CHECK(((kS2aValue & kS2aAddrMask) >> 28) == 3);
	CHECK((kS2aValue & ~kS2aMask) == 0 && kS2aMask == 0xF1FFFFBFu);   // FENCE_ENABLE (bit 6) is not one of the five
	// Linux's size (20 x 2 instances) also fits the field; ours is the narrower AM value.
	CHECK(((40u << 17) & ~kS2aSizeMask) == 0);
	// The S2A register lies past the 1 MiB BAR5 (byte 0x141E908), the RSMU pair and every C0 register inside it.
	CHECK(needs_indirect(kS2aEntry2) && (uint64_t)kS2aEntry2 * 4 == 0x141E908ull);
	CHECK(!needs_indirect(kRsmuIndex) && !needs_indirect(kRsmuData) && !needs_indirect(kDoorbellAperEn));
	for (uint32_t i = 0; i < kC0Count; i++) CHECK(!needs_indirect(kC0Regs[i]));   // C0 writes nothing, not even an RSMU index
}

static void start_cases() {
	Entry m[kPlanMax], d[kPlanMax];
	uint32_t nm = 0, nd = 0, x = 0;
	CHECK(start_plan(0, kFbBase, m, kPlanMax, &x) == kWhyArg && x == 0);
	CHECK(start_plan(3, kFbBase, m, kPlanMax, &x) == kWhyArg);
	CHECK(start_plan(kModeMmio, kFbBase, m, 4, &x) == kWhyOrder && x == 0);   // overflow refused, nothing returned
	CHECK(start_plan(kModeMmio, kFbBase, m, kPlanMax, &nm) == kWhyOk);
	CHECK(start_plan(kModeDb, kFbBase, d, kPlanMax, &nd) == kWhyOk);
	CHECK(nm == 25 && nd == 26);
	// Linux's order: unhalt first, RB_CNTL size, pointers, poll and rptr addresses, base, MINOR_PTR bracket around the
	// doorbell rows, watchdog, UTCL1, the final unhalt, RB_ENABLE, IB_ENABLE last.
	CHECK(m[0].reg == kF32Cntl && m[0].mask == kF32Halt && m[0].value == 0);
	CHECK(find(m, nm, kRbCntl) == 2);
	const int minor1 = find(m, nm, kMinorPtr), minor0 = find(m, nm, kMinorPtr, minor1 + 1);
	CHECK(minor1 > find(m, nm, kRbBaseHi) && minor0 > find(m, nm, kDoorbellOff) && minor0 > minor1);
	CHECK(m[minor1].value == 1 && m[minor0].value == 0);
	CHECK(m[nm - 1].reg == kIbCntl && m[nm - 2].reg == kRbCntl && m[nm - 3].reg == kF32Cntl);
	// No indirect access and no NBIO register in mmio mode; exactly one, the S2A entry, in db mode, inside the bracket.
	for (uint32_t i = 0; i < nm; i++) CHECK(m[i].op == kOpW && m[i].reg != kS2aEntry2);
	int ind = -1;
	for (uint32_t i = 0; i < nd; i++) if (d[i].op == kOpWInd) { CHECK(ind < 0); ind = (int)i; }
	CHECK(ind > 0 && d[ind].reg == kS2aEntry2 && d[ind].mask == kS2aMask && d[ind].value == kS2aValue);
	CHECK(ind > find(d, nd, kDoorbellOff) && ind < find(d, nd, kMinorPtr, find(d, nd, kMinorPtr) + 1));

	Regs rm, rd;
	rm.v[kDoorbell] = kDbEnable | 0x123;    // a doorbell left enabled by someone else is turned off in mmio mode
	rm.v[kDoorbellOff] = 0x00000ABCu;        // and its offset is written back as found
	rd.v[kS2aEntry2] = 0x02000040u;          // 64BIT_SUPPORT_DIS and FENCE_ENABLE as found: kept (Linux's RMW)
	rm.run(m, nm); rd.run(d, nd);
	for (Regs *r : { &rm, &rd }) {
		CHECK(r->get(kRbCntl) == 0x00841815u);                 // RB_SIZE 10, RB_PRIV, RPTR_WB, F32 poll, RB_ENABLE
		CHECK((r->get(kRbCntl) & kRbWptrPoll) == 0);
		CHECK(r->get(kF32Cntl) == 0x08084000u);                // HALT 0, TH1_RESET 0
		CHECK(r->get(kUtcl1Page) == 0x010CEC00u);              // the design's computed value
		CHECK(r->get(kUtcl1Cntl) == ((3u << 9) | 9u));
		CHECK(r->get(kWatchdog) == 1);
		CHECK(r->get(kRbBase) == 0x8008A000u && r->get(kRbBaseHi) == 0);
		CHECK(r->get(kWptrPollLo) == 0x08A01020u && r->get(kWptrPollHi) == 0x80);
		CHECK(r->get(kRptrAddrLo) == 0x08A01000u && r->get(kRptrAddrHi) == 0x80);
		CHECK(r->get(kRbRptr) == 0 && r->get(kRbWptr) == 0 && r->get(kRbRptrHi) == 0 && r->get(kRbWptrHi) == 0);
		CHECK(r->get(kMinorPtr) == 0 && r->get(kIbCntl) == 1 && r->get(kSemWaitFail) == 0);
	}
	CHECK(rm.get(kDoorbell) == 0x123 && rm.get(kDoorbellOff) == 0xABC);
	CHECK(rd.get(kDoorbell) == kDbEnable && rd.get(kDoorbellOff) == 0x800);
	CHECK(rd.get(kS2aEntry2) == (0x02000040u | kS2aValue));
	// Every compared field is what was written: a register that takes the write reads back clean.
	for (uint32_t i = 0; i < nd; i++) CHECK((d[i].cmp & ~d[i].mask) == 0 || d[i].mask == 0xFFFFFFFFu);
}

static void stop_cases() {
	Entry m[kPlanMax], d[kPlanMax];
	uint32_t nm = 0, nd = 0;
	CHECK(stop_plan(kModeNone, 0, m, kPlanMax, &nm) == kWhyArg);
	CHECK(stop_plan(kModeMmio, 0, m, kPlanMax, &nm) == kWhyOk);
	CHECK(stop_plan(kModeDb, 0x02000040u, d, kPlanMax, &nd) == kWhyOk);
	CHECK(nm == 11 && nd == 13);
	for (uint32_t i = 0; i < nm; i++) CHECK(m[i].op != kOpWInd && m[i].reg != kDoorbellOff);
	// Order: queue off, IB off, doorbell off, (db: offset 0, S2A back), halt, the 1 s check, then the pointers to 0.
	CHECK(m[0].reg == kRbCntl && m[1].reg == kIbCntl && m[2].reg == kDoorbell);
	const int halt = find(d, nd, kF32Cntl);
	int chk = -1;
	for (uint32_t i = 0; i < nd; i++) if (d[i].op == kOpRptrCheck) chk = (int)i;
	CHECK(halt > find(d, nd, kDoorbell) && chk == halt + 1 && find(d, nd, kMinorPtr) == chk + 1);
	CHECK(d[nd - 1].reg == kMinorPtr && d[nd - 1].value == 0);
	Regs rd;
	Entry s[kPlanMax];
	uint32_t ns = 0;
	CHECK(start_plan(kModeDb, kFbBase, s, kPlanMax, &ns) == kWhyOk);
	rd.v[kS2aEntry2] = 0x02000040u;
	rd.run(s, ns);
	rd.v[kRbRptr] = 0x40; rd.v[kRbWptr] = 0x40;   // after one test
	rd.run(d, nd);
	CHECK((rd.get(kRbCntl) & kRbEnable) == 0 && (rd.get(kIbCntl) & kIbEnable) == 0);
	CHECK((rd.get(kDoorbell) & kDbEnable) == 0 && (rd.get(kDoorbellOff) & kDbOffMask) == 0);
	CHECK(rd.get(kS2aEntry2) == 0x02000040u);        // NBIO entry 2 back to the value E1 found
	CHECK((rd.get(kF32Cntl) & kF32Halt) != 0);
	CHECK(rd.get(kRbRptr) == 0 && rd.get(kRbWptr) == 0 && rd.get(kMinorPtr) == 0);
	// The mmio start that follows (the same-boot fallback reversed, or a clean restart) programs from that state.
	Entry m2[kPlanMax];
	uint32_t n2 = 0;
	CHECK(start_plan(kModeMmio, kFbBase, m2, kPlanMax, &n2) == kWhyOk);
	rd.run(m2, n2);
	CHECK((rd.get(kDoorbell) & kDbEnable) == 0 && rd.get(kRbCntl) == 0x00841815u && (rd.get(kF32Cntl) & kF32Halt) == 0);
}

static void c1_cases() {
	Entry e[kPlanMax];
	uint32_t n = 0;
	CHECK(kStartLo == 0x00000C00u && kStartHi == 0x0001C000u);
	CHECK(c1_plan(e, 8, &n) == kWhyOrder && n == 0);
	CHECK(c1_plan(e, kPlanMax, &n) == kWhyOk && n == 33);
	// Selections in Linux's order: PFP pipes 0, 1 (ME 0), deselect, ME pipes 0, 1, deselect, MEC pipes 0 to 3 (ME 1),
	// deselect. Each start register follows its selection.
	const uint32_t sel[] = { 0, 1, 0, 0, 1, 0, 4, 5, 6, 7, 0 };
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (e[i].reg != kGrbmGfxCntl) continue;
		CHECK(k < 11 && e[i].value == sel[k] && e[i].mask == 0xFFFFFFFFu);
		k++;
	}
	CHECK(k == 11);
	for (uint32_t i = 0; i < n; i++) {
		if (e[i].reg == kPfpStart || e[i].reg == kMeStart || e[i].reg == kMecStart) CHECK(e[i].value == 0xC00 && e[i - 1].reg == kGrbmGfxCntl);
		if (e[i].reg == kPfpStartHi || e[i].reg == kMeStartHi || e[i].reg == kMecStartHi) CHECK(e[i].value == 0x1C000);
	}
	// Each reset pulse: set (recorded only), then clear (compared), right after a deselect.
	const uint32_t cntl[3] = { kCpMeCntl, kCpMeCntl, kCpMecRs64Cntl }, bits[3] = { kPfpReset, kMeReset, kMecReset };
	int at = 0;
	for (int j = 0; j < 3; j++) {
		at = find(e, n, cntl[j], (uint32_t)at);
		CHECK(at > 0 && e[at - 1].reg == kGrbmGfxCntl && e[at - 1].value == 0);
		CHECK(e[at].mask == bits[j] && e[at].value == bits[j] && e[at].cmp == 0);
		CHECK(e[at + 1].reg == cntl[j] && e[at + 1].mask == bits[j] && e[at + 1].value == 0 && e[at + 1].cmp == bits[j]);
		at += 2;
	}
	CHECK(kPfpReset == 0x000C0000u && kMeReset == 0x00300000u && kMecReset == 0x000F0000u);
	Regs r;
	r.run(e, n);
	CHECK(r.get(kGrbmGfxCntl) == 0 && r.get(kCpMeCntl) == 0 && r.get(kCpMecRs64Cntl) == 0);
}

static State loaded() {
	State s = {};
	s.cpFw[0] = kPfpBytes; s.cpFw[1] = kMeBytes; s.cpFw[2] = kMecBytes;
	return s;
}

static void gate_cases() {
	State s = loaded();
	// C1 and G6 (D6).
	CHECK(may_c1(s, false, false) == kWhyGcDown);
	State f = s; f.cpFw[1] = 0x2D0F4;
	CHECK(may_c1(f, true, false) == kWhyFirmware);
	f = s; f.cpFw[2] = 0;
	CHECK(may_c1(f, true, false) == kWhyFirmware);
	CHECK(may_c1(s, true, true) == kWhyOrder);         // after G6
	CHECK(may_g6(s) == kWhyOrder);
	CHECK(may_c1(s, true, false) == kWhyOk);
	s.c1Attempted = true;
	CHECK(may_c1(s, true, false) == kWhyOrder);        // twice
	CHECK(may_g6(s) == kWhyOrder);                     // attempted but failed
	s.c1 = true;
	CHECK(may_g6(s) == kWhyOk);

	// Start: argument, GC, latch, mode switch, order.
	CHECK(may_start(s, 0, true, true) == kWhyArg && may_start(s, 9, true, true) == kWhyArg);
	CHECK(may_start(s, kModeMmio, false, true) == kWhyGcDown);
	CHECK(may_start(s, kModeMmio, true, false) == kWhyOrder);   // before G6
	CHECK(may_test(s, 1) == kWhyOrder && may_stop(s) == kWhyOrder);
	CHECK(may_test(s, 0) == kWhyArg && may_test(s, 4) == kWhyArg && may_test(s, 9) == kWhyArg);
	CHECK(may_start(s, kModeMmio, true, true) == kWhyOk);
	record_start(s, kModeMmio, true);
	CHECK(may_start(s, kModeMmio, true, true) == kWhyOrder);        // twice
	CHECK(may_start(s, kModeDb, true, true) == kWhyModeSwitch);     // switching while the queue runs
	CHECK(may_test(s, 2) == kWhyOrder && may_test(s, 3) == kWhyOrder);
	CHECK(may_test(s, 1) == kWhyOk);

	// The same-boot fallback: mmio test 1 times out, stop, db start, test 1 again.
	record_test(s, 1, outcome({ false, false, true, 0, false }));
	CHECK(s.latch == kLatchTimeout && s.timedOut == (1u << kModeMmio));
	CHECK(may_test(s, 1) == kWhyLatched);
	CHECK(may_start(s, kModeDb, true, true) == kWhyModeSwitch);     // still running: stop first
	CHECK(may_stop(s) == kWhyOk);
	record_stop(s, true);
	CHECK(s.latch == kLatchNone && !s.running && s.wptrDw == 0);
	CHECK(may_test(s, 1) == kWhyOrder);                             // after stop
	CHECK(may_start(s, kModeMmio, true, true) == kWhyLatched);      // mmio already timed out in this load
	CHECK(may_start(s, kModeDb, true, true) == kWhyOk);
	record_start(s, kModeDb, true);
	CHECK(s.mode == kModeDb && s.wptrDw == 0 && s.testsOk == 0 && s.starts == 2);
	CHECK(may_test(s, 1) == kWhyOk);
	record_test(s, 1, outcome({ true, true, true, 0, false }));
	CHECK(s.testsOk == 2 && s.wptrDw == 16 && s.latch == kLatchNone);
	CHECK(may_test(s, 2) == kWhyOk && may_test(s, 3) == kWhyOrder);
	record_test(s, 2, kLatchNone);
	CHECK(may_test(s, 3) == kWhyOk);
	record_test(s, 3, kLatchNone);
	CHECK(may_test(s, 1) == kWhyOk);                                // repeats are allowed while nothing failed
	// A second timeout (now in db) leaves no mode to start in.
	record_test(s, 1, kLatchTimeout);
	record_stop(s, true);
	CHECK(may_start(s, kModeDb, true, true) == kWhyLatched && may_start(s, kModeMmio, true, true) == kWhyLatched);

	// Hard failures: no start in either mode until reload; stop stays allowed.
	State h = loaded(); h.c1Attempted = h.c1 = true;
	record_start(h, kModeMmio, false);
	CHECK(h.latch == kLatchHard && may_test(h, 1) == kWhyLatched && may_stop(h) == kWhyOk);
	record_stop(h, true);
	CHECK(h.latch == kLatchHard && may_start(h, kModeDb, true, true) == kWhyLatched);
	State k2 = loaded(); k2.c1 = true;
	record_start(k2, kModeMmio, true);
	record_test(k2, 1, outcome({ false, false, true, 0x1234, false }));   // a GC VM fault with the timeout: hard
	CHECK(k2.latch == kLatchHard);
	record_stop(k2, false);
	CHECK(k2.latch == kLatchHard);
	State k3 = loaded();
	record_start(k3, kModeMmio, true);
	record_stop(k3, false);                                         // a stop that does not read back latches hard
	CHECK(k3.latch == kLatchHard && may_start(k3, kModeDb, true, true) == kWhyLatched);
	// A clean stop allows a restart in the same mode.
	State c = loaded();
	record_start(c, kModeMmio, true);
	record_stop(c, true);
	CHECK(may_start(c, kModeMmio, true, true) == kWhyOk && may_start(c, kModeDb, true, true) == kWhyOk);
	// The ring is never wrapped: 64 tests of 16 dwords fill it.
	c.running = true; c.testsOk = 0xE; c.wptrDw = kRingDw - kAlignDw;
	CHECK(may_test(c, 1) == kWhyOk);
	c.wptrDw = kRingDw;
	CHECK(may_test(c, 1) == kWhyOrder);
}

static void outcome_cases() {
	CHECK(outcome({ true, true, true, 0, false }) == kLatchNone);
	CHECK(outcome({ false, false, true, 0, false }) == kLatchTimeout);
	CHECK(outcome({ false, false, false, 0, false }) == kLatchHard);   // engine busy: not just a missed write pointer
	CHECK(outcome({ false, false, true, 0, true }) == kLatchHard);     // ERROR_LOG changed
	CHECK(outcome({ true, false, true, 0, false }) == kLatchHard);     // seen, but RB_RPTR did not catch up
	CHECK(outcome({ true, true, false, 0, false }) == kLatchHard);     // seen, but not idle
	CHECK(outcome({ true, true, true, 1, false }) == kLatchHard);      // seen, with a fault
	Rec r = { kPhaseE1, kOpW, kRbCntl, 0, 0x00841815u, 0x00841815u, 0xFFFFFFFFu };
	CHECK(!mismatch(r));
	r.after = 0x00841814u;
	CHECK(mismatch(r));
	r.cmp = 0;
	CHECK(!mismatch(r));
}

// ---- Part 2: the Linux headers --------------------------------------------------------------------------------
typedef std::map<std::string, unsigned long long> Defs;
static bool load_defs(const std::string &path, Defs &d) {
	std::ifstream f(path);
	if (!f) { printf("FAIL cannot read %s\n", path.c_str()); return false; }
	std::string line;
	while (std::getline(f, line)) {
		char name[160]; char val[64];
		if (sscanf(line.c_str(), "#define %159s %63s", name, val) == 2) d[name] = strtoull(val, NULL, 0);
	}
	return true;
}
struct AddrRow { const char *name; uint32_t abs, seg, idx; };
static int check_addrs(const Defs &d, const AddrRow *rows, size_t n, const char *what) {
	int bad = 0;
	for (size_t i = 0; i < n; i++) {
		const std::string r = std::string("reg") + rows[i].name;
		auto o = d.find(r), b = d.find(r + "_BASE_IDX");
		if (o == d.end() || b == d.end()) { printf("FAIL %s: %s not in the header\n", what, r.c_str()); bad++; continue; }
		if (o->second + rows[i].seg != rows[i].abs || b->second != rows[i].idx) {
			printf("FAIL %s: %s at %#x, header %#llx + %#x (base %llu)\n", what, rows[i].name, rows[i].abs, o->second, rows[i].seg, b->second);
			bad++;
		}
	}
	return bad;
}
struct MaskRow { const char *name; uint32_t mask; };
static int check_masks(const Defs &d, const MaskRow *rows, size_t n, const char *what) {
	int bad = 0;
	for (size_t i = 0; i < n; i++) {
		auto m = d.find(rows[i].name);
		if (m == d.end()) { printf("FAIL %s: %s not in the header\n", what, rows[i].name); bad++; continue; }
		if ((uint32_t)m->second != rows[i].mask) { printf("FAIL %s: %s is %#llx, ours %#x\n", what, rows[i].name, m->second, rows[i].mask); bad++; }
	}
	return bad;
}

static int header_check(const char *root) {
	const std::string inc = std::string(root) + "/drivers/gpu/drm/amd/include/asic_reg/";
	Defs gcOff, gcMask, nbOff, nbMask;
	if (!load_defs(inc + "gc/gc_11_0_0_offset.h", gcOff) || !load_defs(inc + "gc/gc_11_0_0_sh_mask.h", gcMask) ||
	    !load_defs(inc + "nbio/nbio_4_3_0_offset.h", nbOff) || !load_defs(inc + "nbio/nbio_4_3_0_sh_mask.h", nbMask)) return 1;
	const uint32_t s0 = 0x1260, s1 = 0xA000;
	const AddrRow gc[] = {
		{ "SDMA0_F32_CNTL", kF32Cntl, s1, 1 }, { "SDMA0_CNTL", kSdmaCntl, s0, 0 }, { "SDMA0_SEM_WAIT_FAIL_TIMER_CNTL", kSemWaitFail, s0, 0 },
		{ "SDMA0_STATUS_REG", kStatus, s0, 0 }, { "SDMA0_WATCHDOG_CNTL", kWatchdog, s0, 0 }, { "SDMA0_UTCL1_CNTL", kUtcl1Cntl, s0, 0 },
		{ "SDMA0_UTCL1_PAGE", kUtcl1Page, s0, 0 }, { "SDMA0_ERROR_LOG", kErrorLog, s0, 0 },
		{ "SDMA0_QUEUE0_RB_CNTL", kRbCntl, s0, 0 }, { "SDMA0_QUEUE0_RB_BASE", kRbBase, s0, 0 }, { "SDMA0_QUEUE0_RB_BASE_HI", kRbBaseHi, s0, 0 },
		{ "SDMA0_QUEUE0_RB_RPTR", kRbRptr, s0, 0 }, { "SDMA0_QUEUE0_RB_RPTR_HI", kRbRptrHi, s0, 0 }, { "SDMA0_QUEUE0_RB_WPTR", kRbWptr, s0, 0 },
		{ "SDMA0_QUEUE0_RB_WPTR_HI", kRbWptrHi, s0, 0 }, { "SDMA0_QUEUE0_RB_RPTR_ADDR_HI", kRptrAddrHi, s0, 0 },
		{ "SDMA0_QUEUE0_RB_RPTR_ADDR_LO", kRptrAddrLo, s0, 0 }, { "SDMA0_QUEUE0_IB_CNTL", kIbCntl, s0, 0 },
		{ "SDMA0_QUEUE0_CONTEXT_STATUS", kCtxStatus, s0, 0 }, { "SDMA0_QUEUE0_DOORBELL", kDoorbell, s0, 0 },
		{ "SDMA0_QUEUE0_DOORBELL_OFFSET", kDoorbellOff, s0, 0 }, { "SDMA0_QUEUE0_RB_WPTR_POLL_ADDR_HI", kWptrPollHi, s0, 0 },
		{ "SDMA0_QUEUE0_RB_WPTR_POLL_ADDR_LO", kWptrPollLo, s0, 0 }, { "SDMA0_QUEUE0_MINOR_PTR_UPDATE", kMinorPtr, s0, 0 },
		{ "GRBM_STATUS", kGrbmStatus, s0, 0 }, { "CP_STAT", kCpStat, s0, 0 }, { "GCVM_L2_PROTECTION_FAULT_STATUS", kGcFaultStatus, s0, 0 },
		{ "GCVM_L2_PROTECTION_FAULT_ADDR_LO32", kGcFaultAddrLo, s0, 0 }, { "GCVM_L2_PROTECTION_FAULT_ADDR_HI32", kGcFaultAddrHi, s0, 0 },
		{ "CP_PFP_PRGRM_CNTR_START", kPfpStart, s0, 0 }, { "CP_ME_PRGRM_CNTR_START", kMeStart, s0, 0 },
		{ "CP_PFP_PRGRM_CNTR_START_HI", kPfpStartHi, s0, 0 }, { "CP_ME_PRGRM_CNTR_START_HI", kMeStartHi, s0, 0 },
		{ "CP_ME_CNTL", kCpMeCntl, s1, 1 }, { "GRBM_GFX_CNTL", kGrbmGfxCntl, s1, 1 }, { "RLC_CP_SCHEDULERS", kRlcCpSchedulers, s1, 1 },
		{ "CP_MES_CNTL", kCpMesCntl, s1, 1 }, { "CP_MEC_RS64_PRGRM_CNTR_START", kMecStart, s1, 1 },
		{ "CP_MEC_RS64_CNTL", kCpMecRs64Cntl, s1, 1 }, { "CP_MEC_RS64_PRGRM_CNTR_START_HI", kMecStartHi, s1, 1 },
		{ "CP_PFP_IC_BASE_LO", kPfpIcBaseLo, s1, 1 }, { "CP_ME_IC_BASE_LO", kMeIcBaseLo, s1, 1 }, { "CP_CPC_IC_BASE_LO", kCpcIcBaseLo, s1, 1 },
		{ "RLC_RLCS_BOOTLOAD_STATUS", kRlcBootload, s1, 1 },
	};
	const AddrRow nb[] = {
		{ "S2A_DOORBELL_ENTRY_2_CTRL", kS2aEntry2, 0x10400, 3 }, { "BIF_BX_PF0_RSMU_INDEX", kRsmuIndex, 0x14, 1 },
		{ "BIF_BX_PF0_RSMU_DATA", kRsmuData, 0x14, 1 }, { "RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN", kDoorbellAperEn, 0xD20, 2 },
	};
	const MaskRow gm[] = {
		{ "SDMA0_F32_CNTL__HALT_MASK", kF32Halt }, { "SDMA0_F32_CNTL__TH1_RESET_MASK", kF32Th1Reset },
		{ "SDMA0_STATUS_REG__IDLE_MASK", kStatusIdle }, { "SDMA0_QUEUE0_RB_CNTL__RB_ENABLE_MASK", kRbEnable },
		{ "SDMA0_QUEUE0_RB_CNTL__RB_SIZE_MASK", kRbSizeMask }, { "SDMA0_QUEUE0_RB_CNTL__WPTR_POLL_ENABLE_MASK", kRbWptrPoll },
		{ "SDMA0_QUEUE0_RB_CNTL__F32_WPTR_POLL_ENABLE_MASK", kRbF32WptrPoll },
		{ "SDMA0_QUEUE0_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK", kRbRptrWb }, { "SDMA0_QUEUE0_RB_CNTL__RB_PRIV_MASK", kRbPriv },
		{ "SDMA0_QUEUE0_IB_CNTL__IB_ENABLE_MASK", kIbEnable }, { "SDMA0_QUEUE0_DOORBELL__ENABLE_MASK", kDbEnable },
		{ "SDMA0_QUEUE0_DOORBELL_OFFSET__OFFSET_MASK", kDbOffMask }, { "SDMA0_WATCHDOG_CNTL__QUEUE_HANG_COUNT_MASK", kHangCountMask },
		{ "SDMA0_UTCL1_CNTL__RESP_MODE_MASK", kUtclRespMask }, { "SDMA0_UTCL1_CNTL__REDO_DELAY_MASK", kUtclRedoMask },
		{ "SDMA0_UTCL1_PAGE__RD_L2_POLICY_MASK", 0x3000u }, { "SDMA0_UTCL1_PAGE__WR_L2_POLICY_MASK", 0xC000u },
		{ "SDMA0_UTCL1_PAGE__LLC_NOALLOC_MASK", 1u << 24 }, { "SDMA0_QUEUE0_RB_RPTR_ADDR_LO__ADDR_MASK", kAddrLoMask },
		{ "SDMA0_QUEUE0_RB_WPTR_POLL_ADDR_LO__ADDR_MASK", kAddrLoMask }, { "SDMA0_QUEUE0_RB_BASE_HI__ADDR_MASK", kBaseHiMask },
		{ "CP_ME_CNTL__PFP_PIPE0_RESET_MASK", 1u << 18 }, { "CP_ME_CNTL__PFP_PIPE1_RESET_MASK", 1u << 19 },
		{ "CP_ME_CNTL__ME_PIPE0_RESET_MASK", 1u << 20 }, { "CP_ME_CNTL__ME_PIPE1_RESET_MASK", 1u << 21 },
		{ "CP_MEC_RS64_CNTL__MEC_PIPE0_RESET_MASK", 1u << 16 }, { "CP_MEC_RS64_CNTL__MEC_PIPE3_RESET_MASK", 1u << 19 },
		{ "GRBM_GFX_CNTL__PIPEID_MASK", 0x3u }, { "GRBM_GFX_CNTL__MEID_MASK", 0xCu }, { "GRBM_GFX_CNTL__CTXID_MASK", 0x3800u },
		{ "CP_PFP_PRGRM_CNTR_START_HI__IP_START_MASK", kStartHiMask }, { "CP_MEC_RS64_PRGRM_CNTR_START_HI__IP_START_MASK", kStartHiMask },
	};
	const MaskRow nm[] = {
		{ "S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_ENABLE_MASK", kS2aEnable },
		{ "S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_AWID_MASK", kS2aAwidMask },
		{ "S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_OFFSET_MASK", kS2aOffMask },
		{ "S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_SIZE_MASK", kS2aSizeMask },
		{ "S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_AWADDR_31_28_VALUE_MASK", kS2aAddrMask },
	};
	int bad = 0;
	bad += check_addrs(gcOff, gc, sizeof gc / sizeof gc[0], "gc_11_0_0_offset.h");
	bad += check_addrs(nbOff, nb, sizeof nb / sizeof nb[0], "nbio_4_3_0_offset.h");
	bad += check_masks(gcMask, gm, sizeof gm / sizeof gm[0], "gc_11_0_0_sh_mask.h");
	bad += check_masks(nbMask, nm, sizeof nm / sizeof nm[0], "nbio_4_3_0_sh_mask.h");
	printf("header check: %zu GC and %zu NBIO addresses, %zu GC and %zu NBIO masks: %s\n", sizeof gc / sizeof gc[0],
	       sizeof nb / sizeof nb[0], sizeof gm / sizeof gm[0], sizeof nm / sizeof nm[0], bad ? "FAILED" : "all match");
	return bad;
}

int main(int argc, char **argv) {
	packet_cases(); doorbell_cases(); start_cases(); stop_cases(); c1_cases(); gate_cases(); outcome_cases();
	if (argc > 1) fails += header_check(argv[1]);
	else printf("header check skipped: pass the Linux tree root (3857c2fe) as the first argument\n");
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("n33_sdma_test: all passed\n");
	return 0;
}
