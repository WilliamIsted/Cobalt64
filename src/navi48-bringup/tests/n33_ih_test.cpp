// n33_ih_test.cpp: host test of amd/n33_ih_pure.h (stage 3 step 4: one IH 6.0.2 ring in VRAM, polled, the ENABLE_INTR 1
// polled variant, the MSI gate, the CP EOP and SDMA trap sources, the stop).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_ih_test.cpp -o $SCRATCH/n33_ih_test && \
//       $SCRATCH/n33_ih_test [<linux tree root>]
// Part 1 is the pure logic against the design's numbers (register values, the D4 order, packets, decode, gates and the
// latch). Part 2, given the pinned Linux tree's root (3857c2fe), checks every register address and field mask used
// here against osssys_6_0_0_*.h, nbio_4_3_0_*.h and gc_11_0_0_*.h, and the ids and packet values against
// soc15_ih_clientid.h, irqsrcs_gfx_11_0_0.h, nvd.h and sdma_v6_0_0_pkt_open.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include "n33_ih_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

using namespace n33ih;

static constexpr uint64_t kFbBase = 0x8000000000ull;   // MMHUB FB_LOCATION_BASE 0x8000 << 24, measured
static constexpr uint64_t kDummyPa = 0x12345000ull;

static int find(const Entry *e, uint32_t n, uint32_t reg, uint32_t from = 0) {
	for (uint32_t i = from; i < n; i++) if (e[i].reg == reg) return (int)i;
	return -1;
}
// Applies a plan to a register file that starts at 0 (or the given values), as the kext's n33IhRun does.
struct Regs {
	std::map<uint32_t, uint32_t> v;
	uint32_t get(uint32_t r) { auto it = v.find(r); return it == v.end() ? 0 : it->second; }
	void run(const Entry *e, uint32_t n) { for (uint32_t i = 0; i < n; i++) v[e[i].reg] = target(e[i], get(e[i].reg)); }
};

static void value_cases() {
	CHECK(kRbCntl == 0x1120 && kRbBase == 0x1121 && kRbBaseHi == 0x1122 && kRbRptr == 0x1123 && kRbWptr == 0x1124);
	CHECK(kWptrAddrHi == 0x1125 && kWptrAddrLo == 0x1126 && kDoorbellRptr == 0x1127 && kStormCntl == 0x117A);
	CHECK(kFloodCntl == 0x1175 && kMsiStorm == 0x1191 && kChicken == 0x122D);
	CHECK(kIntCntl == 0xE11 && kIntCntl2 == 0xE12);                 // design-pass correction 5
	CHECK(kMe1Pipe0IntCntl == 0x3085);                              // design-pass correction 9
	CHECK(kMmFaultStatus == 0x1A68C && kGcFaultStatus == 0x2828 && kSdmaCntl == 0x127C);
	CHECK(cntl_value(kModePolled) == 0xC0110120u);                  // D3
	CHECK(cntl_expect(kModePolled) == 0x40110121u);                 // design-pass correction 3
	CHECK(cntl_expect(kModePolledIntr) == 0x40330121u);             // + ENABLE_INTR, RPTR_REARM (AM: 0x40330021 without WB)
	CHECK(cntl_expect(kModePolled, 0x00000200u) == 0x40110321u);    // a field we do not own is kept
	const Geo g = geo(kFbBase);
	CHECK(g.ring == 0x8008B00000ull && g.wb == 0x8400002000ull);
	CHECK(g.fenceCp == 0x8400001198ull && g.fenceSdma == 0x8400000010ull);
	CHECK((uint32_t)(g.ring >> 8) == 0x8008B000u && ((g.ring >> 40) & 0xFF) == 0);
	CHECK(kRingOff >= n33cp::kIbOff + n33cp::kIbLen && kRingOff % kRingBytes == 0);   // past step 7's IB page
	CHECK(kRingOff + kRingBytes <= (256ull << 20));                 // inside the 256 MiB BAR0
	CHECK(kSlotC < n33gmc::kHostPages && kHostC == kSlotC && kSlotC != n33cp::kSlotB && kSlotC != 0);
	CHECK(kHostFenceCp >= n33cp::kHostFence5b + 8 && kHostFenceCp + 4 <= 0x1000);
	CHECK(kHostFenceSdma >= n33sdma::kHostOff3 + 4);
	CHECK((1u << (kRbSize + 2)) == kRingBytes);                     // RB_SIZE is log2(dwords)
	CHECK(kKind == 19);
}

static void plan_cases() {
	Entry e[kPlanMax];
	uint32_t n = 0;
	const Geo g = geo(kFbBase);
	CHECK(start_plan(0, g.ring, g.wb, kDummyPa, e, kPlanMax, &n) == kWhyArg && n == 0);
	CHECK(start_plan(kModePolled, g.ring + 4, g.wb, kDummyPa, e, kPlanMax, &n) == kWhyArg);
	CHECK(start_plan(kModePolled, g.ring, g.wb, 0, e, kPlanMax, &n) == kWhyArg);          // G2 has not allocated it
	CHECK(start_plan(kModePolled, g.ring, g.wb, kDummyPa, e, 4, &n) == kWhyOrder && n == 0);
	CHECK(start_plan(kModePolled, g.ring, g.wb, kDummyPa, e, kPlanMax, &n) == kWhyOk);
	// D4 order: disable first, NBIO before the ring registers, base before CNTL, CNTL before WPTR_ADDR, the storm
	// controls before the enable, and RB_ENABLE set only in the last three writes.
	const int dis = find(e, n, kRbCntl), c2 = find(e, n, kIntCntl2), c1 = find(e, n, kIntCntl), base = find(e, n, kRbBase);
	const int cntl = find(e, n, kRbCntl, dis + 1), wb = find(e, n, kWptrAddrLo), storm = find(e, n, kStormCntl);
	const int en = find(e, n, kRbCntl, cntl + 1);
	CHECK(dis == 0 && c2 > dis && c1 > c2 && base > c1 && cntl > base && wb > cntl && storm > wb && en > storm);
	CHECK(en == (int)n - 3);
	for (uint32_t i = 0; i < n - 3; i++) CHECK(!(e[i].reg == kRbCntl && (e[i].value & e[i].mask & kRbEnable)));
	CHECK(e[c2].value == (uint32_t)(kDummyPa >> 8) && e[c2].mask == kAll);
	CHECK(e[c1].mask == (kDummyRdOverride | kReqNonsnoop) && e[c1].value == 0);   // Linux's code value (flagged)
	CHECK(e[base].value == 0x8008B000u);   // the design D2 table says 0x08008B00, a shift slip
	CHECK(e[find(e, n, kRbBaseHi)].value == 0);
	CHECK(e[wb].value == 0x2000u && e[find(e, n, kWptrAddrHi)].value == 0x84u);
	CHECK(e[find(e, n, kDoorbellRptr)].mask == kDbRptrEnable && e[find(e, n, kDoorbellRptr)].value == 0);
	CHECK(e[find(e, n, kMsiStorm)].value == 3 && e[find(e, n, kFloodCntl)].value == kFloodEnable);
	// The register file after the plan: I1's done condition, from reset 0 (and from a ring that was live).
	for (uint32_t m = kModePolled; m <= kModePolledIntr; m++) {
		CHECK(start_plan(m, g.ring, g.wb, kDummyPa, e, kPlanMax, &n) == kWhyOk);
		Regs r; r.v[kRbCntl] = 0x40330121u; r.v[kIntCntl] = 0xFFu;
		r.run(e, n);
		CHECK(r.get(kRbCntl) == cntl_expect(m));
		CHECK(r.get(kIntCntl) == 0xF6u);                        // only OVERRIDE and NONSNOOP cleared
		CHECK(r.get(kRbRptr) == 0 && r.get(kRbWptr) == 0);
		// The OVERFLOW_CLEAR pulse is recorded, not compared; every compared field reads back as written.
		int pulses = 0;
		for (uint32_t i = 0; i < n; i++) if (e[i].reg == kRbCntl && (e[i].value & kOvfClear)) { pulses++; CHECK(!(e[i].cmp & kOvfClear)); }
		CHECK(pulses == 2);
	}
	// I5: disable and the enables back as found; nothing GC without GC up.
	Found f = { true, true, 0, kSdmaTrapEnable };
	CHECK(stop_plan(f, true, e, kPlanMax, &n) == kWhyOk && n == 5);
	CHECK(e[0].reg == kRbCntl && e[0].mask == (kRbEnable | kEnableIntr | kRearm) && e[0].value == 0);
	CHECK(e[3].reg == kMe1Pipe0IntCntl && e[3].value == 0 && e[4].reg == kSdmaCntl && e[4].value == kSdmaTrapEnable);
	CHECK(stop_plan(f, false, e, kPlanMax, &n) == kWhyOk && n == 3);
	f = {};
	CHECK(stop_plan(f, true, e, kPlanMax, &n) == kWhyOk && n == 3);
}

static void packet_cases() {
	uint32_t p[16] = {};
	CHECK(pkt_release_mem_int(p, 0x8400001198ull, 7) == 8);
	CHECK(p[0] == 0xC0064900u && p[1] == 0x06603514u && p[2] == 0x22000000u);
	CHECK(p[3] == 0x00001198u && p[4] == 0x84u && p[5] == 7 && p[6] == 0 && p[7] == 0);
	memset(p, 0xEE, sizeof p);
	CHECK(pkt_sdma_fence_trap(p, 0x8400000010ull, 9) == 6);
	CHECK(p[0] == 0x00030005u && p[1] == 0x10 && p[2] == 0x84u && p[3] == 9 && p[4] == 6 && p[5] == 0);
	CHECK(n33sdma::pad(p, 6) == 16 && p[15] == 0);
	CHECK(pci_quiet(kCmdIntxDisable, 0, 0) && !pci_quiet(0, 0, 0) && !pci_quiet(kCmdIntxDisable, 1, 0) &&
	      !pci_quiet(kCmdIntxDisable, 0, 0x8000));
}

static void decode_cases() {
	const uint32_t dw[8] = { 0x0004B514u, 0x89ABCDEFu, 0x80001234u, 0x00020003u, 0x18u, 2, 3, 4 };
	const Iv e = decode(dw);
	CHECK(e.client == kClientCp && e.src == kSrcIdEop && e.ring == kRingIdMec1P0Q0 && e.vmid == 0 && e.vmidSrc == 0);
	CHECK(e.ts == 0x123489ABCDEFull && e.tsSrc == 1 && e.pasid == 3 && e.node == 2 && e.data[0] == 0x18 && e.data[3] == 4);
	CHECK(decode((const uint32_t[8]){ 0x80F0310Au, 0, 0, 0, 0, 0, 0, 0 }).vmidSrc == 1);
	CHECK(want_client(kSrcCp) == 0x14 && want_srcid(kSrcCpInt) == 0xB5 && want_client(kSrcSdma) == 0x0A &&
	      want_srcid(kSrcSdmaTrap) == 0x31);
	CHECK(wptr_bytes(0x00000021u) == 0x20 && wptr_bytes(0x3FFFDu) == 0x3FFFC);
	CHECK(entries_between(0, 0x40) == 2 && entries_between(0x3FFE0, 0x20) == 2 && entries_between(0x20, 0x20) == 0);
	CHECK(next_rptr(0x3FFE0) == 0 && next_rptr(0x20) == 0x40);
}

static void gate_cases() {
	State s = {};
	CHECK(may_state_regs(false) == kWhyOrder && may_state_regs(true) == kWhyOk);
	CHECK(may_start(s, 3, true, true, true) == kWhyArg);
	CHECK(may_start(s, kModePolled, false, true, true) == kWhyGcDown);
	CHECK(may_start(s, kModePolled, true, false, true) == kWhyOrder);                // before G6
	CHECK(may_start(s, kModePolledIntr, true, true, false) == kWhyMsiGate);          // MSI/INTx not confirmed off
	CHECK(may_start(s, kModePolled, true, true, false) == kWhyOk);
	CHECK(may_recv(s, kSrcCp, true, true, false, true) == kWhyOrder);               // before ih start
	CHECK(may_arm(s, true) == kWhyOrder && may_stop(s) == kWhyOrder);
	record_start(s, kModePolled, true);
	CHECK(may_start(s, kModePolled, true, true, true) == kWhyOrder);                 // twice
	CHECK(may_recv(s, 9, true, true, false, true) == kWhyArg);
	CHECK(may_recv(s, kSrcCp, true, false, false, true) == kWhyOrder);               // no ready step-6 queue
	CHECK(may_recv(s, kSrcCpInt, true, true, false, true) == kWhyOrder);             // the follow-up before 0
	CHECK(may_recv(s, kSrcSdma, true, true, true, true) == kWhyOrder);               // I4 while the compute queue runs
	CHECK(may_recv(s, kSrcSdma, false, false, false, true) == kWhyOrder);            // no SDMA queue
	CHECK(may_recv(s, kSrcCp, true, true, false, false) == kWhyOk);                  // mode 1 does not need PCI quiet
	// Fence landed, ring empty: the follow-up opens, the same variant does not repeat.
	RecvRead r = { true, 0, 0, false, 0, 0, false, false };
	CHECK(outcome(r) == kOutNoIv);
	record_recv(s, kSrcCp, outcome(r));
	CHECK(s.latch == kLatchNone && may_recv(s, kSrcCp, true, true, false, true) == kWhyOrder);
	CHECK(may_recv(s, kSrcCpInt, true, true, false, true) == kWhyOk);
	r.want = 1;
	CHECK(outcome(r) == kOutOk);
	record_recv(s, kSrcCpInt, kOutOk);
	CHECK(may_recv(s, kSrcCpInt, true, true, false, true) == kWhyOk);                 // a success may be repeated
	CHECK(may_recv(s, kSrcSdma, false, false, true, true) == kWhyOk);                // after cp stop
	CHECK(may_recv(s, kSrcSdmaTrap, false, false, true, true) == kWhyOrder);
	// Outcomes that latch, and the one that is the queue's.
	r = { false, 0, 0, false, 0, 0, false, false }; CHECK(outcome(r) == kOutFenceMissing);
	r = { true, 1, 1, false, 0, 0, false, false }; CHECK(outcome(r) == kOutBad);
	r = { true, 1, 0, true, 0, 0, false, false }; CHECK(outcome(r) == kOutBad);
	r = { true, 1, 0, false, 0, 4, false, false }; CHECK(outcome(r) == kOutBad);
	r = { true, 1, 0, false, 0, 0, true, false }; CHECK(outcome(r) == kOutBad);      // armed, the handler did not run
	r = { true, 1, 0, false, 0, 0, true, true }; CHECK(outcome(r) == kOutOk);
	record_recv(s, kSrcCp, kOutFenceMissing);
	CHECK(s.latch == kLatchNone);
	// Arm: the boot-arg gate, once.
	CHECK(may_arm(s, false) == kWhyMsiGate && may_arm(s, true) == kWhyOk);
	s.armed = true;
	CHECK(may_arm(s, true) == kWhyOrder);
	record_recv(s, kSrcCpInt, kOutBad);
	CHECK(s.latch == kLatchHard && may_recv(s, kSrcCpInt, true, true, false, true) == kWhyLatched);
	CHECK(may_arm(s, true) == kWhyOrder && may_stop(s) == kWhyOk);                    // stop stays allowed
	record_stop(s, true);
	CHECK(!s.running && !s.armed && may_start(s, kModePolled, true, true, true) == kWhyLatched);
	// Mode 2: a recv re-checks PCI quiet while not armed.
	State t = {};
	record_start(t, kModePolledIntr, true);
	CHECK(may_recv(t, kSrcCp, true, true, false, false) == kWhyMsiGate);
	CHECK(may_recv(t, kSrcCp, true, true, false, true) == kWhyOk);
	t.armed = true;
	CHECK(may_recv(t, kSrcCp, true, true, false, false) == kWhyOk);
	State u = {};
	record_start(u, kModePolled, false);
	CHECK(u.latch == kLatchHard && may_recv(u, kSrcCp, true, true, false, true) == kWhyLatched);
	// The block is distinct from every other number in use.
	CHECK(kWhyOrder == 220 && kWhyArg == 226 && kWhyGcDown == 43);
}

// ---- Part 2: the Linux headers --------------------------------------------------------------------------------
typedef std::map<std::string, unsigned long long> Defs;
static bool load_defs(const std::string &path, Defs &d) {
	std::ifstream f(path);
	if (!f) { printf("FAIL cannot read %s\n", path.c_str()); return false; }
	std::string line;
	while (std::getline(f, line)) {
		char name[160]; char val[64];
		for (char &c : line) if (c == '\t') c = ' ';
		if (sscanf(line.c_str(), " #define %159s %63s", name, val) == 2) d[name] = strtoull(val, NULL, 0);
	}
	return true;
}
// soc15_ih_clientid.h: "NAME = 0x..,"
static bool load_enum(const std::string &path, Defs &d) {
	std::ifstream f(path);
	if (!f) { printf("FAIL cannot read %s\n", path.c_str()); return false; }
	std::string line;
	while (std::getline(f, line)) {
		char name[128]; char val[32];
		for (char &c : line) if (c == '\t') c = ' ';
		if (sscanf(line.c_str(), " %127s = %31[0-9a-fA-Fx]", name, val) == 2) d.emplace(name, strtoull(val, NULL, 0));
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
struct ValRow { const char *name; unsigned long long v; };
static int check_vals(const Defs &d, const ValRow *rows, size_t n, const char *what) {
	int bad = 0;
	for (size_t i = 0; i < n; i++) {
		auto m = d.find(rows[i].name);
		if (m == d.end()) { printf("FAIL %s: %s not in the header\n", what, rows[i].name); bad++; continue; }
		if (m->second != rows[i].v) { printf("FAIL %s: %s is %#llx, ours %#llx\n", what, rows[i].name, m->second, rows[i].v); bad++; }
	}
	return bad;
}

static int header_check(const char *root) {
	const std::string amd = std::string(root) + "/drivers/gpu/drm/amd/";
	const std::string inc = amd + "include/asic_reg/";
	Defs oOff, oMask, nOff, nMask, gOff, gMask, ids, srcs, nvd, sdp;
	if (!load_defs(inc + "oss/osssys_6_0_0_offset.h", oOff) || !load_defs(inc + "oss/osssys_6_0_0_sh_mask.h", oMask) ||
	    !load_defs(inc + "nbio/nbio_4_3_0_offset.h", nOff) || !load_defs(inc + "nbio/nbio_4_3_0_sh_mask.h", nMask) ||
	    !load_defs(inc + "gc/gc_11_0_0_offset.h", gOff) || !load_defs(inc + "gc/gc_11_0_0_sh_mask.h", gMask) ||
	    !load_enum(amd + "include/soc15_ih_clientid.h", ids) || !load_defs(amd + "include/ivsrcid/gfx/irqsrcs_gfx_11_0_0.h", srcs) ||
	    !load_defs(amd + "amdgpu/nvd.h", nvd) || !load_defs(amd + "amdgpu/sdma_v6_0_0_pkt_open.h", sdp)) return 1;
	const AddrRow oss[] = {
		{ "IH_RB_CNTL", kRbCntl, kOssSeg0, 0 }, { "IH_RB_BASE", kRbBase, kOssSeg0, 0 }, { "IH_RB_BASE_HI", kRbBaseHi, kOssSeg0, 0 },
		{ "IH_RB_RPTR", kRbRptr, kOssSeg0, 0 }, { "IH_RB_WPTR", kRbWptr, kOssSeg0, 0 },
		{ "IH_RB_WPTR_ADDR_HI", kWptrAddrHi, kOssSeg0, 0 }, { "IH_RB_WPTR_ADDR_LO", kWptrAddrLo, kOssSeg0, 0 },
		{ "IH_DOORBELL_RPTR", kDoorbellRptr, kOssSeg0, 0 }, { "IH_CNTL", kIhCntl, kOssSeg0, 0 },
		{ "IH_INT_FLOOD_CNTL", kFloodCntl, kOssSeg0, 0 }, { "IH_STORM_CLIENT_LIST_CNTL", kStormCntl, kOssSeg0, 0 },
		{ "IH_MSI_STORM_CTRL", kMsiStorm, kOssSeg0, 0 }, { "IH_CHICKEN", kChicken, kOssSeg0, 0 },
	};
	const AddrRow nb[] = { { "BIF_BX0_INTERRUPT_CNTL", kIntCntl, kNbioSeg2, 2 }, { "BIF_BX0_INTERRUPT_CNTL2", kIntCntl2, kNbioSeg2, 2 } };
	const AddrRow gc[] = { { "CP_ME1_PIPE0_INT_CNTL", kMe1Pipe0IntCntl, 0x1260, 0 }, { "SDMA0_CNTL", kSdmaCntl, 0x1260, 0 },
	                       { "GCVM_L2_PROTECTION_FAULT_STATUS", kGcFaultStatus, 0x1260, 0 } };
	const ValRow om[] = {
		{ "IH_RB_CNTL__RB_ENABLE_MASK", kRbEnable }, { "IH_RB_CNTL__RB_SIZE_MASK", kRbSizeMask },
		{ "IH_RB_CNTL__RB_SIZE__SHIFT", kRbSizeShift }, { "IH_RB_CNTL__WPTR_WRITEBACK_ENABLE_MASK", kWbEnable },
		{ "IH_RB_CNTL__WPTR_OVERFLOW_ENABLE_MASK", kOvfEnable }, { "IH_RB_CNTL__ENABLE_INTR_MASK", kEnableIntr },
		{ "IH_RB_CNTL__MC_SNOOP_MASK", kSnoop }, { "IH_RB_CNTL__RPTR_REARM_MASK", kRearm }, { "IH_RB_CNTL__MC_RO_MASK", kMcRo },
		{ "IH_RB_CNTL__MC_VMID_MASK", kVmidMask }, { "IH_RB_CNTL__MC_SPACE_MASK", kSpaceMask },
		{ "IH_RB_CNTL__MC_SPACE__SHIFT", kSpaceShift }, { "IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK", kOvfClear },
		{ "IH_RB_WPTR__RB_OVERFLOW_MASK", kWptrOverflow }, { "IH_RB_WPTR__OFFSET_MASK", kWptrOffsetMask },
		{ "IH_DOORBELL_RPTR__ENABLE_MASK", kDbRptrEnable },
		{ "IH_STORM_CLIENT_LIST_CNTL__CLIENT18_IS_STORM_CLIENT_MASK", kStormClient18 },
		{ "IH_INT_FLOOD_CNTL__FLOOD_CNTL_ENABLE_MASK", kFloodEnable }, { "IH_MSI_STORM_CTRL__DELAY_MASK", kMsiStormDelayMask },
	};
	const ValRow nm[] = {
		{ "BIF_BX0_INTERRUPT_CNTL__IH_DUMMY_RD_OVERRIDE_MASK", kDummyRdOverride },
		{ "BIF_BX0_INTERRUPT_CNTL__IH_REQ_NONSNOOP_EN_MASK", kReqNonsnoop },
		{ "BIF_BX0_INTERRUPT_CNTL2__IH_DUMMY_RD_ADDR_MASK", 0xFFFFFFFFull },
	};
	const ValRow gm[] = { { "CP_ME1_PIPE0_INT_CNTL__TIME_STAMP_INT_ENABLE_MASK", kTimeStampIntEnable },
	                      { "SDMA0_CNTL__TRAP_ENABLE_MASK", kSdmaTrapEnable } };
	const ValRow id[] = { { "SOC21_IH_CLIENTID_GRBM_CP", kClientCp }, { "SOC21_IH_CLIENTID_GFX", kClientGfx } };
	const ValRow sr[] = { { "GFX_11_0_0__SRCID__CP_EOP_INTERRUPT", kSrcIdEop }, { "GFX_11_0_0__SRCID__SDMA_TRAP", kSrcIdSdmaTrap } };
	const ValRow pk[] = { { "PACKET3_RELEASE_MEM", n33cp::kOpReleaseMem } };
	const ValRow sp[] = { { "SDMA_OP_TRAP", kSdmaOpTrap }, { "SDMA_OP_FENCE", n33sdma::kOpFence } };
	int bad = 0;
	bad += check_addrs(oOff, oss, sizeof oss / sizeof oss[0], "osssys_6_0_0_offset.h");
	bad += check_addrs(nOff, nb, sizeof nb / sizeof nb[0], "nbio_4_3_0_offset.h");
	bad += check_addrs(gOff, gc, sizeof gc / sizeof gc[0], "gc_11_0_0_offset.h");
	bad += check_vals(oMask, om, sizeof om / sizeof om[0], "osssys_6_0_0_sh_mask.h");
	bad += check_vals(nMask, nm, sizeof nm / sizeof nm[0], "nbio_4_3_0_sh_mask.h");
	bad += check_vals(gMask, gm, sizeof gm / sizeof gm[0], "gc_11_0_0_sh_mask.h");
	bad += check_vals(ids, id, sizeof id / sizeof id[0], "soc15_ih_clientid.h");
	bad += check_vals(srcs, sr, sizeof sr / sizeof sr[0], "irqsrcs_gfx_11_0_0.h");
	bad += check_vals(nvd, pk, sizeof pk / sizeof pk[0], "nvd.h");
	bad += check_vals(sdp, sp, sizeof sp / sizeof sp[0], "sdma_v6_0_0_pkt_open.h");
	// nvd.h's INT_SEL is a macro: PACKET3_RELEASE_MEM_INT_SEL(x) ((x) << 24). Check the shift by text.
	std::ifstream f(amd + "amdgpu/nvd.h");
	std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (all.find("PACKET3_RELEASE_MEM_INT_SEL(x)\t\t((x) << 24)") == std::string::npos) { printf("FAIL nvd.h: INT_SEL not << 24\n"); bad++; }
	printf("header check: %zu OSSSYS, %zu NBIO and %zu GC addresses, %zu OSSSYS, %zu NBIO and %zu GC masks, %zu ids, "
	       "%zu sources, %zu packet values: %s\n", sizeof oss / sizeof oss[0], sizeof nb / sizeof nb[0], sizeof gc / sizeof gc[0],
	       sizeof om / sizeof om[0], sizeof nm / sizeof nm[0], sizeof gm / sizeof gm[0], sizeof id / sizeof id[0],
	       sizeof sr / sizeof sr[0], sizeof pk / sizeof pk[0] + sizeof sp / sizeof sp[0] + 1, bad ? "FAILED" : "all match");
	return bad;
}

int main(int argc, char **argv) {
	value_cases(); plan_cases(); packet_cases(); decode_cases(); gate_cases();
	if (argc > 1) fails += header_check(argv[1]);
	else printf("header check skipped: pass the Linux tree root (3857c2fe) as the first argument\n");
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("n33_ih_test: all passed\n");
	return 0;
}
