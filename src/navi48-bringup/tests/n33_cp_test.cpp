// n33_cp_test.cpp: host test of amd/n33_cp_pure.h (stage 3 step 6: MEC pipe 0, one compute HQD by MMIO, the doorbell
// and mmio kicks, the K2 to K5b packets).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_cp_test.cpp -o $SCRATCH/n33_cp_test && \
//       $SCRATCH/n33_cp_test [<linux tree root>]
// Part 1 is the pure logic against the design's numbers (packet encodings, both modes' plans, the MQD, placement, the
// gates and the latch). Part 2, given the pinned Linux tree's root (3857c2fe), checks every register address, field
// mask, MQD dword, PM4 opcode and event number used here against gc_11_0_0_offset.h / _sh_mask.h / _default.h,
// nbio_4_3_0_*.h, v11_structs.h, nvd.h and soc21_enum.h.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include "n33_cp_pure.h"
#include "n33_fb_pure.h"
#include "n33_psp2_pure.h"
#include "n33_smu_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

using namespace n33cp;

static constexpr uint64_t kFbBase = 0x8000000000ull;   // MMHUB FB_LOCATION_BASE 0x8000 << 24, measured

// Reset defaults (gc_11_0_0_default.h) where the design quotes them, the measured post-C1 MEC value; 0 elsewhere.
static uint32_t reset_default(uint32_t reg) {
	switch (reg) {
	case kCpMecRs64Cntl: return kMecAfterC1;
	case kShMemConfig: return 0xC000u;
	case kPqWptrPollCntl: return 0x1u;
	case kMecDbLower: return 0x110u;
	case kMecDbUpper: return 0xFFCu;
	case kHqdPersistent: return kPersistentDefault;
	case kPqControl: return kPqControlDefault;
	case kMqdControl: return kMqdControlDefault;
	case kEopControl: return kEopControlDefault;
	case kIbControl: return kIbControlDefault;
	default: return 0;
	}
}

// Applies a plan to a register file that starts at the reset defaults, as the kext's n33CpRun does. `stuck` keeps
// CP_HQD_ACTIVE set through the polls until a reset-stage write, to drive K6's second stage.
struct Regs {
	std::map<uint32_t, uint32_t> v;
	bool stuck = false, busy = false, resetRan = false;
	uint32_t sleeps = 0;
	uint32_t get(uint32_t r) { auto it = v.find(r); return it == v.end() ? reset_default(r) : it->second; }
	void run(const Entry *e, uint32_t n) {
		for (uint32_t i = 0; i < n; i++) {
			const Entry &x = e[i];
			if (x.op == kOpSleepMs) { sleeps += x.value; continue; }
			if (x.op == kOpPoll) {
				if (!stuck && x.reg == kHqdActive && get(kDequeue)) v[kHqdActive] = 0;   // a requested drain completes
				busy = (get(x.reg) & x.mask) != 0;
				continue;
			}
			if (x.op == kOpWIfBusy || x.op == kOpWIfSet) {
				if (!busy) continue;
				if (x.op == kOpWIfBusy) { resetRan = true; stuck = false; }
			}
			v[x.reg] = target(x, get(x.reg));
		}
	}
};
static int find(const Entry *e, uint32_t n, uint32_t reg, uint32_t from = 0) {
	for (uint32_t i = from; i < n; i++) if (e[i].reg == reg && (e[i].op == kOpW || e[i].op == kOpWInd)) return (int)i;
	return -1;
}

static void placement_cases() {
	const Geo g = geo(kFbBase);
	CHECK(g.mqd == 0x8008A03000ull && g.eop == 0x8008A04000ull && g.ring == 0x8008A05000ull);
	CHECK(g.rptr == 0x8008A06000ull && g.wptr == 0x8008A06020ull && g.code == 0x8008A07000ull && g.vram == 0x8008A08000ull);
	CHECK(g.hostB == 0x8400001000ull);
	CHECK(lo32(g.eop >> 8) == 0x8008A040u && hi32(g.eop >> 8) == 0);
	CHECK(lo32(g.ring >> 8) == 0x8008A050u && hi32(g.ring >> 8) == 0);
	CHECK(lo32(g.code >> 8) == 0x8008A070u && lo32(g.code >> 40) == 0);
	CHECK(kOwnedOff == 0x8A03000 && kOwnedOff + kOwnedLen == 0x8A09000);
	// T1 (step 7): the IB page is the one page past the six step-6 pages, inside BAR0 above the floor.
	CHECK(g.ib == 0x8008A09000ull && kIbOff == 0x8A09000 && kIbOff == kOwnedOff + kOwnedLen && kIbLen == 0x1000);
	CHECK(kIbOff >= (16ull << 20) && kIbOff + kIbLen <= (256ull << 20));
	// Clear of step 5's kind 15, the GMC table (14), the SMU table (13), the PSP pages (12) and our surface; above the
	// 16 MiB floor and inside the 256 MiB BAR0.
	auto clear = [](uint64_t off, uint64_t len) { return kOwnedOff + kOwnedLen <= off || off + len <= kOwnedOff; };
	CHECK(clear(n33sdma::kOwnedOff, n33sdma::kOwnedLen) && n33sdma::kOwnedOff + n33sdma::kOwnedLen == kOwnedOff);
	CHECK(clear(n33gmc::kOwnedOff, n33gmc::kOwnedLen));
	CHECK(clear(n33smu::kTableOff, n33smu::kTableLen));
	CHECK(clear(n33psp2::kRing, n33psp2::kPspPages));
	CHECK(clear(n33fb::kSurfOff, n33fb::kBytes));
	CHECK(kOwnedOff >= (16ull << 20) && kOwnedOff + kOwnedLen <= (256ull << 20));
	// Host page B is the GMC code's page 1 at slot 1, not step 5's page A at slot 0; the offsets sit in one page.
	CHECK(kSlotB == 1 && kHostB == 1 && kHostB < n33gmc::kHostPages);
	CHECK(kResultDw * 4 <= kHostK3 && kHostK3 + 4 <= kHostFence4 && kHostFence4 + 8 <= kHostFence5a &&
	      kHostFence5a + 8 <= kHostFence5b && kHostFence5b + 8 <= kPage);
	CHECK(kHostFence4 % 8 == 0 && kHostFence5a % 8 == 0 && kHostFence5b % 8 == 0);
	// Doorbell: dword index 6, BAR2 byte 0x18, inside the 2 MiB BAR2 measured on macOS.
	CHECK(kDbIndex == (3u << 1) && kDbByte == 0x18 && kBar2Need == 0x20);
	CHECK(kDbControl == 0x40000018u && (kDbControl & ~(kDbOffMask | kDbEn)) == 0);
	CHECK(kSelQ0 == 0x4 && kSelQ0 == n33sdma::grbm_select(1, 0));
	// S2A entry 0: enable, AWID 3, offset 0, size 0, AWADDR 3; the RSMU path as entry 2.
	CHECK(kS2aValue0 == 0x30000007u && (kS2aValue0 & ~kS2aMask) == 0);
	CHECK(n33sdma::needs_indirect(kS2aEntry0) && (uint64_t)kS2aEntry0 * 4 == 0x141E900ull);
	for (uint32_t i = 0; i < kStateCount; i++) CHECK(!n33sdma::needs_indirect(kStateRegs[i]));
	CHECK(kStateCount == 16);
	CHECK(kHqdFirst + kHqdCount - 1 == kPqWptrHi && mqd_dw(kMqdBase) == 128 && mqd_dw(kPqWptrHi) == 183);
}

static void shader_cases() {
	// Build output (build-shader.sh, gfx1102): the design expected RSRC1 0x60AF0000 (unverified); the build confirms it.
	CHECK(kShaderBytes == 36 && kShaderBytes % 4 == 0);
	CHECK(kRsrc1 == 0x60AF0000u && kRsrc2 == 0x00000084u && kRsrc3 == 0);
	CHECK((kRsrc1 & 0x3F) == 0 && ((kRsrc1 >> 6) & 0xF) == 0);              // VGPRS 0 (one block of 8), SGPRS 0
	CHECK((kRsrc1 >> 21 & 1) && (kRsrc1 >> 23 & 1) && (kRsrc1 >> 29 & 1) && (kRsrc1 >> 30 & 1));   // DX10, IEEE, WGP, MEM_ORDERED
	CHECK(((kRsrc2 >> 1) & 0x1F) == 2 && (kRsrc2 >> 7 & 1) && (kRsrc2 & 1) == 0);   // USER_SGPR 2, TGID_X_EN, no scratch
	uint8_t d[32];
	memcpy(d, kShaderSha256, 32);
	CHECK(blob_ok(36, d));
	CHECK(d[0] == 0xA2 && d[31] == 0x40);
	CHECK(!blob_ok(40, d) && !blob_ok(0, d));
	d[5] ^= 1;
	CHECK(!blob_ok(36, d));
	CHECK(kEndpgm == 0xBFB00000u);
}

static void packet_cases() {
	uint32_t p[kSubmitDw * 2];
	const Geo g = geo(kFbBase);
	CHECK(kNop == 0xFFFF1000u);
	CHECK(pkt3(kOpSetUconfigReg, 1) == 0xC0017900u);
	CHECK(pkt_set_uconfig(p, kScratchReg0, kAfter) == 3);
	CHECK(p[0] == 0xC0017900u && p[1] == 0x40 && p[2] == 0xDEADBEEFu);
	CHECK(pkt_write_data(p, g.hostB + kHostK3, kAfter) == 5);
	CHECK(p[0] == 0xC0033700u && p[1] == 0x00100500u && p[2] == 0x00001100u && p[3] == 0x84 && p[4] == 0xDEADBEEFu);
	CHECK(pkt_release_mem(p, g.hostB + kHostFence5b, 3, true) == 8);
	CHECK(p[0] == 0xC0064900u && p[1] == 0x06603514u && p[2] == 0x20000000u && p[3] == 0x00001190u && p[4] == 0x84 &&
	      p[5] == 3 && p[6] == 0 && p[7] == 0);
	CHECK(pkt_release_mem(p, g.hostB + kHostFence5a, 2, false) == 8 && p[1] == 0x06403514u);   // K5a: GL2_WB cleared only
	CHECK((0x06603514u ^ 0x06403514u) == kGl2Wb);

	// K4: the design's table, row by row.
	memset(p, 0, sizeof p);
	const uint32_t n = pkt_dispatch(p, g.code, g.hostB, g.hostB + kHostFence4, 1, true);
	CHECK(n == 41);
	const uint32_t want[41] = {
		0xC0027600u, 0x20C, 0x8008A070u, 0,                       // (a) PGM_LO, _HI
		0xC0027600u, 0x212, 0x60AF0000u, 0x84,                    // (b) RSRC1, RSRC2
		0xC0017600u, 0x215, 0,                                    // (c) RESOURCE_LIMITS
		0xC0017600u, 0x218, 0,                                    // (d) TMPRING_SIZE
		0xC0017600u, 0x228, 0,                                    // (e) RSRC3
		0xC0037600u, 0x207, 32, 1, 1,                             // (f) NUM_THREAD_X/Y/Z
		0xC0027600u, 0x240, 0x00001000u, 0x84,                    // (g) USER_DATA_0/1: host page B
		0xC0031500u, 1, 1, 1, 0x00008045u,                        // (h) DISPATCH_DIRECT
		0xC0004600u, 0x00000407u,                                 // (i) CS_PARTIAL_FLUSH
		0xC0064900u, 0x06603514u, 0x20000000u, 0x00001180u, 0x84, 1, 0, 0,   // (j) fence
	};
	for (uint32_t i = 0; i < 41; i++) if (p[i] != want[i]) { printf("K4 dword %u: %#x, want %#x\n", i, p[i], want[i]); fails++; }
	// K5a: the same stream with the VRAM page and the fence without GL2_WB.
	CHECK(pkt_dispatch(p, g.code, g.vram, g.hostB + kHostFence5a, 2, false) == 41);
	CHECK(p[24] == 0x08A08000u && p[25] == 0x80 && p[34] == 0x06403514u && p[36] == 0x00001188u && p[38] == 2);
	CHECK(kInitiator == 0x8045 && kCsPartialFlush == 0x407);
	// Every SET_SH_REG index lies in 0x2C00..0x3000.
	CHECK(kComputeInitiator - kShRegStart == 0x200 && kComputeUserData0 + 1 < 0x3000);
	// Padding: one-dword NOPs to 256; K2 to K5a fill the ring exactly, K5b wraps to offset 0.
	CHECK(pad(p, 41) == 256 && p[41] == kNop && p[255] == kNop);
	CHECK(pad(p, 256) == 256);
	CHECK(ring_off(0) == 0 && ring_off(256) == 0x400 && ring_off(768) == 0xC00 && ring_off(1024) == 0);
	CHECK(rptr_at(256, 256) && rptr_at(0, 1024) && rptr_at(1024, 1024) && rptr_at(256, 1280) && !rptr_at(0, 256));
	CHECK(timeout_us(1) == 100000 && timeout_us(3) == 2000000 && timeout_us(4) == 2000000 && timeout_us(5) == 100000);
}

static void mqd_cases() {
	uint32_t m[kMqdDw], d[kMqdDw];
	mqd_build(kModeMmio, kFbBase, m);
	mqd_build(kModeDb, kFbBase, d);
	CHECK(m[0] == 0xC0310800u && m[11] == 1 && m[32] == 7);
	CHECK(m[23] == 0xFFFFFFFFu && m[24] == 0xFFFFFFFFu && m[25] == 0 && m[26] == 0xFFFFFFFFu && m[27] == 0xFFFFFFFFu);
	CHECK(m[44] == 0xFFFFFFFFu && m[47] == 0xFFFFFFFFu);
	CHECK(m[128] == 0x08A03000u && m[129] == 0x80 && m[130] == 1 && m[131] == 0 && m[132] == 0x0BE05501u);
	CHECK(m[133] == 2 && m[134] == 15 && m[135] == 0x111 && m[136] == 0x8008A050u && m[137] == 0 && m[138] == 0);
	CHECK(m[139] == 0x08A06000u && m[140] == 0x80 && m[141] == 0x08A06020u && m[142] == 0x80);
	CHECK(m[143] == 0 && d[143] == 0x40000018u && m[144] == 0);
	CHECK(m[145] == 0xC0308509u && m[149] == 0x00300000u && m[162] == 0x100);
	CHECK(m[165] == 0x8008A040u && m[166] == 0 && m[167] == 0x9 && m[179] == 0 && m[182] == 0 && m[183] == 0);
	CHECK(m[160] == 0);                                       // hq_status0 left 0, as Linux
	for (uint32_t i = 0; i < kMqdDw; i++) if (i != 143) CHECK(m[i] == d[i]);
}

static void start_cases() {
	Entry m[kPlanMax], d[kPlanMax];
	uint32_t nm = 0, nd = 0, x = 0;
	CHECK(k1_plan(0, kFbBase, m, kPlanMax, &x) == kWhyArg && x == 0);
	CHECK(k1_plan(3, kFbBase, m, kPlanMax, &x) == kWhyArg);
	CHECK(k1_plan(kModeDb, kFbBase, m, 8, &x) == kWhyOrder && x == 0);
	CHECK(k1_plan(kModeMmio, kFbBase, m, kPlanMax, &nm) == kWhyOk);
	CHECK(k1_plan(kModeDb, kFbBase, d, kPlanMax, &nd) == kWhyOk);
	CHECK(nm == 28 && nd == 32);
	// K1a first (after db's NBIO entry): the MEC RMW, 1 ms, SH_MEM_CONFIG under VMID 0.
	CHECK(m[0].reg == kCpMecRs64Cntl && m[0].mask == 0x44010010u && m[0].value == 0x04000000u);
	CHECK(m[1].op == kOpSleepMs && m[1].value == 1);
	CHECK(m[2].reg == kGrbmGfxCntl && m[2].value == 0 && m[3].reg == kShMemConfig && m[3].value == 0xC00Cu && m[4].reg == kGrbmGfxCntl);
	CHECK(d[0].op == kOpWInd && d[0].reg == kS2aEntry0 && d[0].value == kS2aValue0 && d[0].mask == kS2aMask);
	for (uint32_t i = 0; i < nm; i++) CHECK(m[i].op != kOpWInd && m[i].reg != kMecDbLower && m[i].reg != kPqStatus);
	// K1b in Linux's kiq_init_register order, under the select, ending with ACTIVE, (db) CP_PQ_STATUS, deselect.
	const uint32_t order[] = { kPqWptrPollCntl, kEopBase, kEopBaseHi, kEopControl, kPqDoorbell, kMqdBase, kMqdBaseHi,
	                           kMqdControl, kPqBase, kPqBaseHi, kPqControl, kRptrReport, kRptrReportHi, kWptrPoll,
	                           kWptrPollHi, kMecDbLower, kMecDbUpper, kPqDoorbell, kPqWptrLo, kPqWptrHi, kHqdVmid,
	                           kHqdPersistent, kHqdActive, kPqStatus };
	int sel = -1;
	for (uint32_t i = 0; i < nd; i++) if (d[i].reg == kGrbmGfxCntl && d[i].value == kSelQ0) sel = (int)i;
	CHECK(sel == 6);
	for (uint32_t k = 0; k < sizeof order / sizeof order[0]; k++) CHECK(d[sel + 1 + k].reg == order[k]);
	CHECK(d[nd - 1].reg == kGrbmGfxCntl && d[nd - 1].value == 0 && m[nm - 1].reg == kGrbmGfxCntl && m[nm - 1].value == 0);
	CHECK(m[nm - 2].reg == kHqdActive && d[nd - 2].reg == kPqStatus);
	for (uint32_t i = 0; i < nd; i++) if (d[i].reg == kGrbmGfxCntl) CHECK(d[i].cmp == 0 && d[i].mask == 0xFFFFFFFFu);

	Regs rm, rd;
	rd.v[kS2aEntry0] = 0x02000040u;          // 64BIT_SUPPORT_DIS and FENCE_ENABLE as found: kept
	rm.run(m, nm); rd.run(d, nd);
	CHECK(rm.sleeps == 1);
	for (Regs *r : { &rm, &rd }) {
		CHECK(r->get(kCpMecRs64Cntl) == 0x04000000u);
		CHECK(r->get(kShMemConfig) == 0xC00Cu && r->get(kShMemBases) == 0);
		CHECK(r->get(kPqWptrPollCntl) == 0x1u);
		CHECK(r->get(kEopBase) == 0x8008A040u && r->get(kEopBaseHi) == 0 && r->get(kEopControl) == 0x9);
		CHECK(r->get(kMqdBase) == 0x08A03000u && r->get(kMqdBaseHi) == 0x80 && r->get(kMqdControl) == 0x100);
		CHECK(r->get(kPqBase) == 0x8008A050u && r->get(kPqBaseHi) == 0 && r->get(kPqControl) == 0xC0308509u);
		CHECK(r->get(kRptrReport) == 0x08A06000u && r->get(kRptrReportHi) == 0x80);
		CHECK(r->get(kWptrPoll) == 0x08A06020u && r->get(kWptrPollHi) == 0x80);
		CHECK(r->get(kPqWptrLo) == 0 && r->get(kPqWptrHi) == 0 && r->get(kHqdVmid) == 0);
		CHECK(r->get(kHqdPersistent) == 0x0BE05501u && r->get(kHqdActive) == 1 && r->get(kGrbmGfxCntl) == 0);
	}
	CHECK(rm.get(kPqDoorbell) == 0 && rm.get(kMecDbLower) == 0x110 && rm.get(kMecDbUpper) == 0xFFC && rm.get(kPqStatus) == 0);
	CHECK(rd.get(kPqDoorbell) == 0x40000018u && rd.get(kMecDbLower) == 0 && rd.get(kMecDbUpper) == 0x450 && rd.get(kPqStatus) == 2);
	CHECK(rd.get(kS2aEntry0) == (0x02000040u | kS2aValue0));
	// The plan's register values and the MQD agree, dword for dword.
	uint32_t q[kMqdDw];
	mqd_build(kModeDb, kFbBase, q);
	for (uint32_t r = kHqdFirst; r < kHqdFirst + kHqdCount; r++) {
		if (r == kHqdPipePrio || r == kHqdQueuePrio || r == kHqdQuantum || r == kIbControl) continue;   // MQD only
		if (find(d, nd, r) < 0) continue;
		CHECK(q[mqd_dw(r)] == rd.get(r));
	}
	// A compared field is always one that was written.
	for (uint32_t i = 0; i < nd; i++) if (d[i].op == kOpW || d[i].op == kOpWInd) CHECK((d[i].cmp & ~d[i].mask) == 0);
	// From a K6 state (pipe 0 active, halted) K1a gives the same MEC value.
	Regs r6;
	r6.v[kCpMecRs64Cntl] = 0x44000000u;
	r6.run(m, nm);
	CHECK(r6.get(kCpMecRs64Cntl) == 0x04000000u);
}

static void stop_cases() {
	Entry m[kPlanMax], d[kPlanMax], s[kPlanMax];
	uint32_t nm = 0, nd = 0, ns = 0;
	const Found f = { 0x02000040u, 0x110, 0xFFC };
	CHECK(k6_plan(kModeNone, f, false, m, kPlanMax, &nm) == kWhyArg);
	CHECK(k6_plan(kModeMmio, f, false, m, kPlanMax, &nm) == kWhyOk);
	CHECK(k6_plan(kModeDb, f, false, d, kPlanMax, &nd) == kWhyOk);
	CHECK(nm == 16 && nd == 20);
	CHECK(m[0].reg == kGrbmGfxCntl && m[0].value == kSelQ0);
	CHECK(m[1].op == kOpPoll && m[1].reg == kHqdActive && m[1].value == 0);
	CHECK(m[2].op == kOpWIfSet && m[2].reg == kDequeue && m[2].value == 1);
	CHECK(m[3].op == kOpPoll && m[3].reg == kHqdActive && m[3].value == 100);
	CHECK(m[4].op == kOpWIfBusy && m[4].reg == kDequeue && m[4].value == 2 && m[5].op == kOpWIfBusy && m[5].reg == kSpiQueueReset);
	CHECK(m[6].op == kOpPoll && m[nm - 1].reg == kCpMecRs64Cntl && m[nm - 1].mask == kMecHalt && m[nm - 1].value == kMecHalt);
	Entry u[kPlanMax];
	uint32_t nu = 0;
	CHECK(k6_plan(kModeMmio, f, true, u, kPlanMax, &nu) == kWhyOk && nu == nm && u[1].value == 0 && u[3].value == 0 && u[6].value == 0);

	// db start, one test, then stop: everything K1 changed in db mode is back, the MEC halted.
	CHECK(k1_plan(kModeDb, kFbBase, s, kPlanMax, &ns) == kWhyOk);
	Regs r;
	r.v[kS2aEntry0] = 0x02000040u;
	r.run(s, ns);
	r.v[kPqRptr] = 256; r.v[kPqWptrLo] = 256;
	r.v[kHqdError] = 0x80000;                              // a faulted queue: K6 must clear TC_UTCL1_ERROR
	r.run(d, nd);
	CHECK(!r.resetRan && !r.busy);
	CHECK(r.get(kHqdActive) == 0 && r.get(kPqRptr) == 0 && r.get(kPqWptrLo) == 0 && r.get(kPqDoorbell) == 0 && r.get(kDequeue) == 0 && r.get(kHqdError) == 0);
	CHECK(r.get(kPqStatus) == 0 && r.get(kMecDbLower) == 0x110 && r.get(kMecDbUpper) == 0xFFC && r.get(kS2aEntry0) == 0x02000040u);
	CHECK(r.get(kCpMecRs64Cntl) == 0x44000000u && r.get(kGrbmGfxCntl) == 0);
	// A queue that does not drain: the reset stage runs, then the second poll sees it go.
	Regs k;
	k.run(s, ns);
	k.stuck = true;
	k.run(d, nd);
	CHECK(k.resetRan && k.get(kSpiQueueReset) == 1 && k.get(kHqdActive) == 0);
	// A queue that never went active (a failed K1): the drain is skipped, no DEQUEUE_REQUEST write.
	Regs q;
	q.run(d, 3);
	CHECK(!q.busy && q.v.count(kDequeue) == 0);
	// mmio after a db stop programs from that state (the fallback order is db first here).
	Entry m2[kPlanMax];
	uint32_t n2 = 0;
	CHECK(k1_plan(kModeMmio, kFbBase, m2, kPlanMax, &n2) == kWhyOk);
	r.run(m2, n2);
	CHECK(r.get(kPqDoorbell) == 0 && r.get(kHqdActive) == 1 && r.get(kCpMecRs64Cntl) == 0x04000000u && r.get(kPqStatus) == 0);
}

static void gate_cases() {
	State s = {};
	CHECK(may_start(s, 0, true, true, true, false) == kWhyArg && may_start(s, 9, true, true, true, false) == kWhyArg);
	CHECK(may_start(s, kModeDb, false, true, true, false) == kWhyGcDown);
	CHECK(may_start(s, kModeDb, true, false, true, false) == kWhyOrder);   // before C1
	CHECK(may_start(s, kModeDb, true, true, false, false) == kWhyOrder);   // before G6
	CHECK(may_start(s, kModeDb, true, true, true, true) == kWhyOrder);     // SDMA0 queue running (D7)
	CHECK(may_test(s, 1) == kWhyOrder && may_stop(s) == kWhyOrder);
	CHECK(may_test(s, 0) == kWhyArg && may_test(s, 6) == kWhyArg && may_test(s, 9) == kWhyArg);
	CHECK(mec_ready(s, 0x40000000u) && !mec_ready(s, 0x44000000u) && !mec_ready(s, 0x400F0000u) && !mec_ready(s, 0x04000000u));
	CHECK(may_start(s, kModeDb, true, true, true, false) == kWhyOk);
	record_start(s, kModeDb, true);
	CHECK(mec_ready(s, 0x44000000u));                                       // after a start: K6's value too
	CHECK(may_start(s, kModeDb, true, true, true, false) == kWhyOrder);    // twice in one mode
	CHECK(may_start(s, kModeMmio, true, true, true, false) == kWhyModeSwitch);
	CHECK(may_test(s, 2) == kWhyOrder && may_test(s, 3) == kWhyOrder && may_test(s, 1) == kWhyOk);

	// D4's fallback: db K2 times out with nothing else wrong, K6, mmio, K2 again, then K3 to K5.
	record_test(s, 1, outcome({ false, false, true, 0, 0, false }));
	CHECK(s.latch == kLatchTimeout && s.timedOut == (1u << kModeDb) && s.wptrDw == 256);
	CHECK(may_test(s, 1) == kWhyLatched && may_start(s, kModeMmio, true, true, true, false) == kWhyModeSwitch);
	record_stop(s, true);
	CHECK(s.latch == kLatchNone && !s.running && s.wptrDw == 0 && may_test(s, 1) == kWhyOrder);
	CHECK(may_start(s, kModeDb, true, true, true, false) == kWhyLatched);
	CHECK(may_start(s, kModeMmio, true, true, true, false) == kWhyOk);
	record_start(s, kModeMmio, true);
	CHECK(s.mode == kModeMmio && s.starts == 2 && s.testsOk == 0);
	for (uint32_t n = 1; n <= 3; n++) { CHECK(may_test(s, n) == kWhyOk); record_test(s, n, kLatchNone, false); }
	CHECK(may_test(s, 5) == kWhyOrder && may_test(s, 4) == kWhyOk);
	// K5a's data all landed: K5b is refused.
	State a = s;
	record_test(a, 4, kLatchNone, true);
	CHECK(a.k5aDataAll && may_test(a, 5) == kWhyOrder && a.wptrDw == 1024);
	// K5a's fence without all the data: K5b runs, and wraps.
	record_test(s, 4, kLatchNone, false);
	CHECK(!s.k5aDataAll && may_test(s, 5) == kWhyOk && ring_off(s.wptrDw) == 0);
	record_test(s, 5, kLatchNone, false);
	CHECK(s.wptrDw == 1280 && s.testsOk == 0x3E);
	// A second timeout leaves no mode to start in.
	record_test(s, 1, kLatchTimeout, false);
	record_stop(s, true);
	CHECK(may_start(s, kModeDb, true, true, true, false) == kWhyLatched && may_start(s, kModeMmio, true, true, true, false) == kWhyLatched);
	// Hard failures: no start until reload, stop allowed; a stop that does not read back latches hard.
	State h = {};
	record_start(h, kModeDb, false);
	CHECK(h.latch == kLatchHard && may_test(h, 1) == kWhyLatched && may_stop(h) == kWhyOk);
	record_stop(h, true);
	CHECK(h.latch == kLatchHard && may_start(h, kModeMmio, true, true, true, false) == kWhyLatched);
	State k = {};
	record_start(k, kModeDb, true);
	record_stop(k, false);
	CHECK(k.latch == kLatchHard);
	State c = {};
	record_start(c, kModeDb, true);
	record_stop(c, true);
	CHECK(may_start(c, kModeDb, true, true, true, false) == kWhyOk);       // a clean stop allows the same mode again
}

static void outcome_cases() {
	CHECK(outcome({ true, true, true, 0, 0, true }) == kLatchNone);
	CHECK(outcome({ false, false, true, 0, 0, false }) == kLatchTimeout);
	CHECK(outcome({ false, false, true, 0, 0, true }) == kLatchHard);      // rptr moved: the CP saw the kick
	CHECK(outcome({ false, false, false, 0, 0, false }) == kLatchHard);    // queue no longer active
	CHECK(outcome({ false, false, true, 0, 1, false }) == kLatchHard);     // CP_HQD_ERROR
	CHECK(outcome({ false, false, true, 1, 0, false }) == kLatchHard);     // GC VM fault
	CHECK(outcome({ true, false, true, 0, 0, true }) == kLatchHard);       // seen, rptr short
	CHECK(outcome({ true, true, true, 0, 2, true }) == kLatchHard);
	Rec r = { kPhaseK1, kOpW, kHqdActive, 0, 1, 1, kActive };
	CHECK(!mismatch(r));
	r.after = 0;
	CHECK(mismatch(r));
}

// T1 (step 7): the INDIRECT_BUFFER packet, the may_ib gate and record_ib bookkeeping.
static void t1_cases() {
	const Geo g = geo(kFbBase);
	uint32_t p[kSubmitDw];
	CHECK(kOpIndirectBuffer == 0x3F && kIbValid == 0x00800000u);
	// INDIRECT_BUFFER (3): header 0xC0023F00, ib_lo (dword-aligned), ib_hi, control = VALID | IB_SIZE | VMID (D3).
	CHECK(pkt_indirect_buffer(p, g.ib, 41, 0) == 4);
	CHECK(p[0] == 0xC0023F00u && p[1] == 0x08A09000u && p[2] == 0x80 && p[3] == 0x00800029u);
	CHECK(ib_control(41, 0) == 0x00800029u && ib_control(41, 0) == (kIbValid | 41));
	CHECK(ib_control(0xFFFFFu, 3) == (kIbValid | 0xFFFFFu | (3u << 24)));   // IB_SIZE bits 19:0, VMID bits 27:24
	CHECK(ib_control(0x100000u, 0) == kIbValid);                           // IB_SIZE masked to 20 bits
	// The ring packet pads to 256; the IB content is the 41-dword K4 stream, dword-aligned, never ring-padded.
	CHECK(pad(p, 4) == 256 && p[4] == kNop && p[255] == kNop);
	uint32_t ib[kSubmitDw];
	CHECK(pkt_dispatch(ib, g.code, g.hostB, g.hostB + kHostFence4, 1, true) == 41);

	CHECK(kWhyIbOrder == 62 && kWhyIbOverlap == 63);
	// may_ib: needs running, not latched, and K2 (test 1) or K3 (test 2) landed since the start.
	State s = {};
	CHECK(may_ib(s) == kWhyIbOrder);                       // not running
	record_start(s, kModeMmio, true);
	CHECK(may_ib(s) == kWhyIbOrder);                       // running but no ring packet landed yet
	record_test(s, 1, kLatchNone, false);                  // K2
	CHECK(may_ib(s) == kWhyOk);
	State s3 = {};
	record_start(s3, kModeDb, true);
	s3.testsOk = 1u << 2;                                   // only K3 landed: also enough
	CHECK(may_ib(s3) == kWhyOk);
	// A latched queue refuses with 56 (kWhyLatched), not 62, matching the hardware gate.
	State h = {};
	record_start(h, kModeMmio, true);
	record_test(h, 1, kLatchNone, false);
	record_test(h, 1, kLatchHard, false);
	CHECK(may_ib(h) == kWhyLatched);
	// record_ib: one 256-dword ring submission, no test bit set, the latch carried.
	State r = {};
	record_start(r, kModeMmio, true);
	record_test(r, 1, kLatchNone, false);
	const uint32_t tOk = r.testsOk;
	const uint64_t wp0 = r.wptrDw;
	record_ib(r, kLatchNone);
	CHECK(r.wptrDw == wp0 + kSubmitDw && r.testsOk == tOk && r.latch == kLatchNone);
	record_ib(r, kLatchTimeout);
	CHECK(r.latch == kLatchTimeout && r.timedOut == (1u << kModeMmio) && r.wptrDw == wp0 + 2 * kSubmitDw);
}

// W1 (step 7): the cp submit / cp waitseq gates and bookkeeping, the winsys submit->wait split of cp ib.
static void w1_cases() {
	CHECK(kWhyNoSubmit == 64);
	// may_submit: the same gate as may_ib, plus one submit outstanding at a time.
	State s = {};
	CHECK(may_submit(s) == kWhyIbOrder);                   // not running
	record_start(s, kModeMmio, true);
	CHECK(may_submit(s) == kWhyIbOrder);                   // running but no ring packet landed yet
	record_test(s, 1, kLatchNone, false);                  // K2
	CHECK(may_submit(s) == kWhyOk);
	CHECK(!s.submitPending && may_waitseq(s, 1) == kWhyNoSubmit);   // running, nothing outstanding
	// A submit: wptr grows by one 256-dword submission (as record_ib), the seqno is live, one submit outstanding.
	const uint64_t wp0 = s.wptrDw;
	record_submit(s, 7, wp0 + kSubmitDw, 0x40);
	CHECK(s.submitPending && s.submitSeq == 7 && s.submitWp == wp0 + kSubmitDw && s.submitRptr0 == 0x40);
	CHECK(s.seq == 7 && s.wptrDw == wp0 + kSubmitDw);
	CHECK(may_submit(s) == kWhyIbOrder);                   // a second submit before waitseq is out of order
	CHECK(may_ib(s) == kWhyIbOrder);                       // and so is a T1 cp ib
	CHECK(may_waitseq(s, 6) == kWhyNoSubmit && may_waitseq(s, 8) == kWhyNoSubmit);   // a wrong seqno is refused
	CHECK(may_waitseq(s, 7) == kWhyOk);                    // the outstanding one
	// waitseq retires: the submit is consumed, the wptr is not advanced again, another submit is allowed.
	record_waitseq(s, kLatchNone);
	CHECK(!s.submitPending && s.latch == kLatchNone && s.wptrDw == wp0 + kSubmitDw);
	CHECK(may_waitseq(s, 7) == kWhyNoSubmit && may_submit(s) == kWhyOk);
	record_submit(s, 8, s.wptrDw + kSubmitDw, 0x80);
	CHECK(s.submitPending && may_waitseq(s, 8) == kWhyOk);
	// A timeout on wait marks the mode and consumes the submit; a hard failure latches.
	record_waitseq(s, kLatchTimeout);
	CHECK(!s.submitPending && s.timedOut == (1u << kModeMmio) && s.latch == kLatchTimeout);
	State h = {};
	record_start(h, kModeDb, true);
	record_test(h, 1, kLatchNone, false);
	record_submit(h, 1, kSubmitDw, 0);
	record_waitseq(h, kLatchHard);
	CHECK(!h.submitPending && h.latch == kLatchHard);
	// A latched queue refuses a submit with 56 (kWhyLatched), as may_ib does, not 62.
	State l = {};
	record_start(l, kModeMmio, true);
	record_test(l, 1, kLatchNone, false);
	record_test(l, 1, kLatchHard, false);
	CHECK(may_submit(l) == kWhyLatched);
	// cp stop abandons an outstanding submit.
	State c = {};
	record_start(c, kModeDb, true);
	record_test(c, 1, kLatchNone, false);
	record_submit(c, 1, kSubmitDw, 0);
	record_stop(c, true);
	CHECK(!c.submitPending && !c.running);
	// A fresh start clears any stale pending flag.
	State f = {};
	f.submitPending = true;
	record_start(f, kModeMmio, true);
	CHECK(!f.submitPending);
}

// X4b (step 7): cp xib's gate, stream selection, the 65 verdict and the seqno bookkeeping it shares with W1. The
// translator itself is not linked here; src/xlat12/tests/test_n33_xib.cpp runs xib_g103 through it against xib_native.
static void x4b_cases() {
	CHECK(kWhyXibMismatch == 65 && kWhyXibMismatch != kWhyNoSubmit);
	CHECK(kXibCap >= 53 && kXibCap <= kSubmitDw);         // X4a's worst translated case fits; the IB buffer holds it
	CHECK(kHostXibWd % 4 == 0 && kHostXibWd >= kResultDw * 4 && kHostXibWd < kHostFence4);   // apart from data and fence
	// Stream selection: 1 and 2 only; anything else is 60 whatever the queue state.
	CHECK(!valid_xib(kXibK4) && valid_xib(kXibWd) && valid_xib(kXibDispatch) && !valid_xib(3) && !valid_xib(0xFFFFFFFFu));
	State s = {};
	CHECK(may_xib(s, 0) == kWhyArg && may_xib(s, 3) == kWhyArg);
	// The gate is cp submit's: not running 62, no K2/K3 yet 62, then ok; latched 56.
	CHECK(may_xib(s, kXibWd) == kWhyIbOrder);
	record_start(s, kModeDb, true);
	CHECK(may_xib(s, kXibWd) == kWhyIbOrder && may_xib(s, kXibDispatch) == kWhyIbOrder);
	record_test(s, 2, kLatchNone, false);                  // K3 alone also opens it
	CHECK(may_xib(s, kXibWd) == kWhyOk && may_xib(s, kXibDispatch) == kWhyOk);
	// One outstanding submit across cp submit and cp xib, one seqno sequence, waitseq retires either.
	const uint64_t wp0 = s.wptrDw;
	record_submit(s, 3, wp0 + kSubmitDw, 0x10, kXibWd);
	CHECK(s.submitPending && s.submitStream == kXibWd && s.seq == 3 && s.wptrDw == wp0 + kSubmitDw);
	CHECK(may_xib(s, kXibDispatch) == kWhyIbOrder && may_submit(s) == kWhyIbOrder && may_ib(s) == kWhyIbOrder);
	CHECK(may_waitseq(s, 2) == kWhyNoSubmit && may_waitseq(s, 3) == kWhyOk);
	record_waitseq(s, kLatchNone);
	CHECK(!s.submitPending && may_xib(s, kXibDispatch) == kWhyOk);
	record_submit(s, 4, s.wptrDw + kSubmitDw, 0x20, kXibDispatch);
	CHECK(s.submitStream == kXibDispatch && may_waitseq(s, 4) == kWhyOk);
	record_waitseq(s, kLatchNone);
	record_submit(s, 5, s.wptrDw + kSubmitDw, 0x30);       // a plain cp submit after xib: stream back to K4
	CHECK(s.submitStream == kXibK4 && s.seq == 5 && s.wptrDw == wp0 + 3 * kSubmitDw);
	record_waitseq(s, kLatchHard);
	CHECK(may_xib(s, kXibWd) == kWhyLatched);
	// Stores waitseq counts: one for wd, 32 for the dispatches.
	CHECK(xib_want(kXibWd) == 1 && xib_want(kXibDispatch) == kResultDw && xib_want(kXibK4) == kResultDw);
	// Streams: the native forms are T1/W1's addresses; the dispatch native equals T1's K4 exactly.
	const Geo g = geo(kFbBase);
	uint32_t a[kXibCap], b[kXibCap];
	CHECK(xib_g103(kXibK4, a, g, 1) == 0 && xib_g103(3, a, g, 1) == 0 && xib_native(3, a, g, 1) == 0);
	uint32_t n = xib_native(kXibDispatch, a, g, 9), m = pkt_dispatch(b, g.code, g.hostB, g.hostB + kHostFence4, 9, true);
	CHECK(n == 41 && m == 41 && xib_first_diff(a, n, b, m) == kXibSame);
	CHECK(xib_native(kXibK4, b, g, 9) == 41 && xib_first_diff(a, n, b, 41) == kXibSame);
	n = xib_native(kXibWd, a, g, 9);
	CHECK(n == 13 && a[0] == pkt3(kOpWriteData, 3) && a[2] == (uint32_t)(g.hostB + kHostXibWd) && a[3] == hi32(g.hostB) &&
	      a[4] == kMagic + 9 && a[5] == pkt3(kOpReleaseMem, 6) && a[8] == (uint32_t)(g.hostB + kHostFence4) && a[10] == 9);
	m = xib_g103(kXibWd, b, g, 9);
	CHECK(m == 13 && xib_first_diff(a, n, b, m) == kXibSame);   // wd's gfx10.3 and gfx11 encodings are the same dwords
	CHECK(xib_g103(kXibDispatch, b, g, 9) == 41);
	// The verdict: equal is 0; a translator status, an empty output, a changed dword or a length difference is 65.
	CHECK(xib_verdict(0, a, n, a, n) == kWhyOk);
	CHECK(xib_verdict(21, a, n, a, n) == kWhyXibMismatch);
	CHECK(xib_verdict(0, a, 0, a, 0) == kWhyXibMismatch);
	CHECK(xib_verdict(0, a, n - 1, a, n) == kWhyXibMismatch && xib_first_diff(a, n - 1, a, n) == n - 1);
	for (uint32_t i = 0; i < n; i++) b[i] = a[i];
	b[10] ^= 1;                                            // a forced mismatch in the fence seqno
	CHECK(xib_verdict(0, b, n, a, n) == kWhyXibMismatch && xib_first_diff(b, n, a, n) == 10);
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
		else if (sscanf(line.c_str(), "#define\t%159s %63s", name, val) == 2) d[name] = strtoull(val, NULL, 0);
	}
	return true;
}
// v11_structs.h: "uint32_t <field>; // offset: <n>" inside struct v11_compute_mqd.
static bool load_mqd(const std::string &path, Defs &d) {
	std::ifstream f(path);
	if (!f) { printf("FAIL cannot read %s\n", path.c_str()); return false; }
	std::string line;
	bool in = false;
	while (std::getline(f, line)) {
		if (line.find("struct v11_compute_mqd {") != std::string::npos) { in = true; continue; }
		if (in && line.rfind("};", 0) == 0) break;
		char name[96]; unsigned off = 0;
		if (in && sscanf(line.c_str(), " uint32_t %95[^;]; // offset: %u", name, &off) == 2) d[name] = off;
	}
	return in;
}
// soc21_enum.h: "NAME = 0x...,"
static bool load_enum(const std::string &path, Defs &d) {
	std::ifstream f(path);
	if (!f) { printf("FAIL cannot read %s\n", path.c_str()); return false; }
	std::string line;
	while (std::getline(f, line)) {
		char name[128]; char val[32];
		if (sscanf(line.c_str(), "%127s = %31[0-9a-fA-Fx]", name, val) == 2) d.emplace(name, strtoull(val, NULL, 0));
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
	Defs gcOff, gcMask, gcDef, nbOff, nbMask, mqd, nvd, en;
	if (!load_defs(inc + "gc/gc_11_0_0_offset.h", gcOff) || !load_defs(inc + "gc/gc_11_0_0_sh_mask.h", gcMask) ||
	    !load_defs(inc + "gc/gc_11_0_0_default.h", gcDef) || !load_defs(inc + "nbio/nbio_4_3_0_offset.h", nbOff) ||
	    !load_defs(inc + "nbio/nbio_4_3_0_sh_mask.h", nbMask) || !load_mqd(amd + "include/v11_structs.h", mqd) ||
	    !load_defs(amd + "amdgpu/nvd.h", nvd) || !load_enum(amd + "include/soc21_enum.h", en)) return 1;
	const uint32_t s0 = 0x1260, s1 = 0xA000;
	const AddrRow gc[] = {
		{ "CP_MEC_RS64_CNTL", kCpMecRs64Cntl, s1, 1 }, { "CP_MEC_RS64_INSTR_PNTR", kMecInstrPntr, s1, 1 },
		{ "SH_MEM_CONFIG", kShMemConfig, s1, 1 }, { "SH_MEM_BASES", kShMemBases, s1, 1 }, { "GRBM_GFX_CNTL", kGrbmGfxCntl, s1, 1 },
		{ "SCRATCH_REG0", kScratchReg0, s1, 1 }, { "RLC_CP_SCHEDULERS", kRlcCpSchedulers, s1, 1 }, { "CP_MES_CNTL", kCpMesCntl, s1, 1 },
		{ "CP_CPC_IC_BASE_LO", kCpcIcBaseLo, s1, 1 },
		{ "CP_PQ_WPTR_POLL_CNTL", kPqWptrPollCntl, s0, 0 }, { "CP_MEC_DOORBELL_RANGE_LOWER", kMecDbLower, s0, 0 },
		{ "CP_MEC_DOORBELL_RANGE_UPPER", kMecDbUpper, s0, 0 }, { "CP_PQ_STATUS", kPqStatus, s0, 0 },
		{ "SPI_COMPUTE_QUEUE_RESET", kSpiQueueReset, s0, 0 }, { "CP_STAT", kCpStat, s0, 0 }, { "GRBM_STATUS", kGrbmStatus, s0, 0 },
		{ "CP_CPC_STATUS", kCpcStatus, s0, 0 }, { "GCVM_L2_PROTECTION_FAULT_STATUS", kGcFaultStatus, s0, 0 },
		{ "GCVM_L2_PROTECTION_FAULT_ADDR_LO32", kGcFaultAddrLo, s0, 0 }, { "GCVM_L2_PROTECTION_FAULT_ADDR_HI32", kGcFaultAddrHi, s0, 0 },
		{ "CP_MQD_BASE_ADDR", kMqdBase, s0, 0 }, { "CP_MQD_BASE_ADDR_HI", kMqdBaseHi, s0, 0 }, { "CP_HQD_ACTIVE", kHqdActive, s0, 0 },
		{ "CP_HQD_VMID", kHqdVmid, s0, 0 }, { "CP_HQD_PERSISTENT_STATE", kHqdPersistent, s0, 0 },
		{ "CP_HQD_PIPE_PRIORITY", kHqdPipePrio, s0, 0 }, { "CP_HQD_QUEUE_PRIORITY", kHqdQueuePrio, s0, 0 },
		{ "CP_HQD_QUANTUM", kHqdQuantum, s0, 0 }, { "CP_HQD_PQ_BASE", kPqBase, s0, 0 }, { "CP_HQD_PQ_BASE_HI", kPqBaseHi, s0, 0 },
		{ "CP_HQD_PQ_RPTR", kPqRptr, s0, 0 }, { "CP_HQD_PQ_RPTR_REPORT_ADDR", kRptrReport, s0, 0 },
		{ "CP_HQD_PQ_RPTR_REPORT_ADDR_HI", kRptrReportHi, s0, 0 }, { "CP_HQD_PQ_WPTR_POLL_ADDR", kWptrPoll, s0, 0 },
		{ "CP_HQD_PQ_WPTR_POLL_ADDR_HI", kWptrPollHi, s0, 0 }, { "CP_HQD_PQ_DOORBELL_CONTROL", kPqDoorbell, s0, 0 },
		{ "CP_HQD_PQ_CONTROL", kPqControl, s0, 0 }, { "CP_HQD_IB_CONTROL", kIbControl, s0, 0 },
		{ "CP_HQD_DEQUEUE_REQUEST", kDequeue, s0, 0 }, { "CP_MQD_CONTROL", kMqdControl, s0, 0 },
		{ "CP_HQD_EOP_BASE_ADDR", kEopBase, s0, 0 }, { "CP_HQD_EOP_BASE_ADDR_HI", kEopBaseHi, s0, 0 },
		{ "CP_HQD_EOP_CONTROL", kEopControl, s0, 0 }, { "CP_HQD_EOP_RPTR", kEopRptr, s0, 0 }, { "CP_HQD_EOP_WPTR", kEopWptr, s0, 0 },
		{ "CP_HQD_ERROR", kHqdError, s0, 0 }, { "CP_HQD_PQ_WPTR_LO", kPqWptrLo, s0, 0 }, { "CP_HQD_PQ_WPTR_HI", kPqWptrHi, s0, 0 },
		{ "COMPUTE_DISPATCH_INITIATOR", kComputeInitiator, s0, 0 }, { "COMPUTE_NUM_THREAD_X", kComputeNumX, s0, 0 },
		{ "COMPUTE_PGM_LO", kComputePgmLo, s0, 0 }, { "COMPUTE_PGM_HI", kComputePgmHi, s0, 0 },
		{ "COMPUTE_PGM_RSRC1", kComputeRsrc1, s0, 0 }, { "COMPUTE_PGM_RSRC2", kComputeRsrc2, s0, 0 },
		{ "COMPUTE_RESOURCE_LIMITS", kComputeResLimits, s0, 0 }, { "COMPUTE_TMPRING_SIZE", kComputeTmpring, s0, 0 },
		{ "COMPUTE_PGM_RSRC3", kComputeRsrc3, s0, 0 }, { "COMPUTE_USER_DATA_0", kComputeUserData0, s0, 0 },
	};
	const AddrRow nb[] = { { "S2A_DOORBELL_ENTRY_0_CTRL", kS2aEntry0, 0x10400, 3 } };
	const ValRow gm[] = {
		{ "CP_MEC_RS64_CNTL__MEC_INVALIDATE_ICACHE_MASK", kMecInvIcache }, { "CP_MEC_RS64_CNTL__MEC_PIPE0_RESET_MASK", kMecPipe0Reset },
		{ "CP_MEC_RS64_CNTL__MEC_PIPE0_ACTIVE_MASK", kMecPipe0Active }, { "CP_MEC_RS64_CNTL__MEC_HALT_MASK", kMecHalt },
		{ "GRBM_GFX_CNTL__MEID_MASK", 0xC }, { "GRBM_GFX_CNTL__VMID_MASK", 0xF0 }, { "GRBM_GFX_CNTL__QUEUEID_MASK", 0x700 },
		{ "CP_PQ_WPTR_POLL_CNTL__EN_MASK", kPollEn }, { "CP_PQ_STATUS__DOORBELL_ENABLE_MASK", kPqStatusDbEn },
		{ "CP_HQD_ACTIVE__ACTIVE_MASK", kActive }, { "CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_OFFSET_MASK", kDbOffMask },
		{ "CP_HQD_PQ_DOORBELL_CONTROL__DOORBELL_EN_MASK", kDbEn }, { "CP_HQD_PQ_CONTROL__QUEUE_SIZE_MASK", kPqQueueSize },
		{ "CP_HQD_PQ_CONTROL__RPTR_BLOCK_SIZE_MASK", kPqRptrBlock }, { "CP_HQD_PQ_CONTROL__TMZ_MASK", kPqTmz },
		{ "CP_HQD_PQ_CONTROL__UNORD_DISPATCH_MASK", kPqUnord }, { "CP_HQD_PQ_CONTROL__TUNNEL_DISPATCH_MASK", kPqTunnel },
		{ "CP_HQD_PQ_CONTROL__PRIV_STATE_MASK", kPqPriv }, { "CP_HQD_PQ_CONTROL__KMD_QUEUE_MASK", kPqKmd },
		{ "CP_MQD_CONTROL__VMID_MASK", kMqdVmidMask }, { "CP_HQD_EOP_CONTROL__EOP_SIZE_MASK", kEopSizeMask },
		{ "CP_HQD_PERSISTENT_STATE__PRELOAD_SIZE_MASK", kPreloadMask }, { "GRBM_STATUS__SPI_BUSY_MASK", kSpiBusy },
		{ "GRBM_STATUS__CP_BUSY_MASK", kCpBusy },
	};
	const ValRow gd[] = {
		{ "regCP_MEC_RS64_CNTL_DEFAULT", kMecAfterC1 }, { "regSH_MEM_CONFIG_DEFAULT", 0xC000 },
		{ "regCP_PQ_WPTR_POLL_CNTL_DEFAULT", 0x1 }, { "regCP_HQD_PERSISTENT_STATE_DEFAULT", kPersistentDefault },
		{ "regCP_HQD_PQ_CONTROL_DEFAULT", kPqControlDefault }, { "regCP_MQD_CONTROL_DEFAULT", kMqdControlDefault },
		{ "regCP_HQD_EOP_CONTROL_DEFAULT", kEopControlDefault }, { "regCP_HQD_IB_CONTROL_DEFAULT", kIbControlDefault },
	};
	const ValRow nm[] = {
		{ "S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_ENABLE_MASK", 0x1 },
		{ "S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_AWID_MASK", n33sdma::kS2aAwidMask },
		{ "S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_RANGE_OFFSET_MASK", n33sdma::kS2aOffMask },
		{ "S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_RANGE_SIZE_MASK", n33sdma::kS2aSizeMask },
		{ "S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_AWADDR_31_28_VALUE_MASK", n33sdma::kS2aAddrMask },
	};
	const ValRow md[] = {
		{ "header", 0 }, { "compute_pipelinestat_enable", 11 }, { "compute_static_thread_mgmt_se0", 23 },
		{ "compute_static_thread_mgmt_se1", 24 }, { "compute_static_thread_mgmt_se2", 26 }, { "compute_static_thread_mgmt_se3", 27 },
		{ "compute_misc_reserved", 32 }, { "compute_static_thread_mgmt_se4", 44 }, { "compute_static_thread_mgmt_se7", 47 },
		{ "cp_mqd_base_addr_lo", mqd_dw(kMqdBase) }, { "cp_hqd_active", mqd_dw(kHqdActive) },
		{ "cp_hqd_persistent_state", mqd_dw(kHqdPersistent) }, { "cp_hqd_pipe_priority", mqd_dw(kHqdPipePrio) },
		{ "cp_hqd_quantum", mqd_dw(kHqdQuantum) }, { "cp_hqd_pq_base_lo", mqd_dw(kPqBase) },
		{ "cp_hqd_pq_rptr_report_addr_lo", mqd_dw(kRptrReport) }, { "cp_hqd_pq_wptr_poll_addr_hi", mqd_dw(kWptrPollHi) },
		{ "cp_hqd_pq_doorbell_control", mqd_dw(kPqDoorbell) }, { "cp_hqd_pq_control", mqd_dw(kPqControl) },
		{ "cp_hqd_ib_control", mqd_dw(kIbControl) }, { "cp_hqd_dequeue_request", mqd_dw(kDequeue) },
		{ "cp_mqd_control", mqd_dw(kMqdControl) }, { "cp_hqd_eop_base_addr_lo", mqd_dw(kEopBase) },
		{ "cp_hqd_eop_control", mqd_dw(kEopControl) }, { "cp_hqd_error", mqd_dw(kHqdError) },
		{ "cp_hqd_pq_wptr_lo", mqd_dw(kPqWptrLo) }, { "cp_hqd_pq_wptr_hi", mqd_dw(kPqWptrHi) },
	};
	const ValRow pk[] = {
		{ "PACKET3_NOP", kOpNop }, { "PACKET3_DISPATCH_DIRECT", kOpDispatchDirect }, { "PACKET3_WRITE_DATA", kOpWriteData },
		{ "PACKET3_EVENT_WRITE", kOpEventWrite }, { "PACKET3_RELEASE_MEM", kOpReleaseMem }, { "PACKET3_SET_SH_REG", kOpSetShReg },
		{ "PACKET3_SET_UCONFIG_REG", kOpSetUconfigReg }, { "PACKET3_SET_SH_REG_START", kShRegStart },
		{ "PACKET3_SET_UCONFIG_REG_START", kUconfigStart }, { "PACKET3_INDIRECT_BUFFER", kOpIndirectBuffer },
	};
	const ValRow ev[] = { { "CS_PARTIAL_FLUSH", kCsPartialFlush & 0xFF }, { "CACHE_FLUSH_AND_INV_TS_EVENT", kRelMemBase & 0xFF } };
	int bad = 0;
	bad += check_addrs(gcOff, gc, sizeof gc / sizeof gc[0], "gc_11_0_0_offset.h");
	bad += check_addrs(nbOff, nb, sizeof nb / sizeof nb[0], "nbio_4_3_0_offset.h");
	bad += check_vals(gcMask, gm, sizeof gm / sizeof gm[0], "gc_11_0_0_sh_mask.h");
	bad += check_vals(gcDef, gd, sizeof gd / sizeof gd[0], "gc_11_0_0_default.h");
	bad += check_vals(nbMask, nm, sizeof nm / sizeof nm[0], "nbio_4_3_0_sh_mask.h");
	bad += check_vals(mqd, md, sizeof md / sizeof md[0], "v11_structs.h");
	bad += check_vals(nvd, pk, sizeof pk / sizeof pk[0], "nvd.h");
	bad += check_vals(en, ev, sizeof ev / sizeof ev[0], "soc21_enum.h");
	printf("header check: %zu GC and %zu NBIO addresses, %zu GC masks, %zu defaults, %zu NBIO masks, %zu MQD dwords, "
	       "%zu PM4 values, %zu events: %s\n", sizeof gc / sizeof gc[0], sizeof nb / sizeof nb[0], sizeof gm / sizeof gm[0],
	       sizeof gd / sizeof gd[0], sizeof nm / sizeof nm[0], sizeof md / sizeof md[0], sizeof pk / sizeof pk[0],
	       sizeof ev / sizeof ev[0], bad ? "FAILED" : "all match");
	return bad;
}

int main(int argc, char **argv) {
	placement_cases(); shader_cases(); packet_cases(); mqd_cases(); start_cases(); stop_cases(); gate_cases(); outcome_cases();
	t1_cases();
	w1_cases();
	x4b_cases();
	if (argc > 1) fails += header_check(argv[1]);
	else printf("header check skipped: pass the Linux tree root (3857c2fe) as the first argument\n");
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("n33_cp_test: all passed\n");
	return 0;
}
