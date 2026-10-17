// n33_sdma_pure.h: decisions for stage 3 step 5 on Navi 33: the CP RS64 start addresses (C1) and SDMA 6.0.2
// queue 0 with a polled first DMA proof of the GART (E1 to E4, E6) of the overview repo's
// RDNA3/Tasks/navi33-cobalt64-port/stage3-step5-gfx-sdma-design.md (D1 to D10, revised 2026-10-14).
// Host-testable: no kernel types, no I/O. The kext reads the live registers, asks c1_plan(), start_plan() and
// stop_plan() for every write with each value computed here, and builds every packet with the helpers below.
// Linux references are to the pinned tree 3857c2fe (sdma_v6_0.c, gfx_v11_0.c, nbio_v4_3.c); AM references are
// to tinygrad 02e698160eed (tinygrad/runtime/support/am/ip.py, amdev.py; tinygrad/runtime/ops_amd.py).
#pragma once
#include <stdint.h>

namespace n33sdma {

// ---- Registers (absolute dwords) ------------------------------------------------------------------------
// SDMA0: GC segment 0 at 0x1260, F32_CNTL on segment 1 at 0xA000 (gc_11_0_0_offset.h; sdma_v6_0.c:127-143).
constexpr uint32_t kF32Cntl = 0xF89A, kSdmaCntl = 0x127C, kSemWaitFail = 0x1282, kStatus = 0x1285, kWatchdog = 0x128E,
                   kUtcl1Cntl = 0x129C, kUtcl1Page = 0x129F, kErrorLog = 0x12B0;
constexpr uint32_t kRbCntl = 0x12E0, kRbBase = 0x12E1, kRbBaseHi = 0x12E2, kRbRptr = 0x12E3, kRbRptrHi = 0x12E4,
                   kRbWptr = 0x12E5, kRbWptrHi = 0x12E6, kRptrAddrHi = 0x12E8, kRptrAddrLo = 0x12E9, kIbCntl = 0x12EA,
                   kCtxStatus = 0x12F1, kDoorbell = 0x12F2, kDoorbellOff = 0x130B, kWptrPollHi = 0x1312,
                   kWptrPollLo = 0x1313, kMinorPtr = 0x1315;
// CP, GRBM, RLC and GCVM (GC segments 0 and 1).
constexpr uint32_t kGrbmStatus = 0x2004, kCpStat = 0x21A0, kGcFaultStatus = 0x2828, kGcFaultAddrLo = 0x2829,
                   kGcFaultAddrHi = 0x282A, kPfpStart = 0x30A4, kMeStart = 0x30A5, kPfpStartHi = 0x30B9,
                   kMeStartHi = 0x30D9, kCpMeCntl = 0xA803, kGrbmGfxCntl = 0xA900, kRlcCpSchedulers = 0xA98A,
                   kCpMesCntl = 0xC807, kMecStart = 0xC900, kCpMecRs64Cntl = 0xC904, kMecStartHi = 0xC938,
                   kPfpIcBaseLo = 0xF840, kMeIcBaseLo = 0xF844, kCpcIcBaseLo = 0xF84C, kRlcBootload = 0xEE82;
// NBIO 4.3. S2A_DOORBELL_ENTRY_2_CTRL is base index 3 (0x10400 + 0x4F7642, nbio_4_3_0_offset.h:14872-14873): byte
// 0x141E908, outside the 1 MiB BAR5, so it is reached through the RSMU index/data pair at base index 1 (0x14 + 0, 0x14
// + 1, nbio_4_3_0_offset.h:342-345), as Linux (amdgpu_reg_access.c:500-511 -> soc21.c:557-558 ->
// amdgpu_reg_access.c:758-776, nbio_v4_3.c:313-321) and AM (amdev.py:310-314, :334-340) both do.
constexpr uint32_t kS2aEntry2 = 0x507A42, kRsmuIndex = 0x14, kRsmuData = 0x15, kDoorbellAperEn = 0xDE0;
constexpr uint32_t kBar5Bytes = 1u << 20;   // measured on macOS (baseline study); the S2A register is past it
inline bool needs_indirect(uint32_t reg) { return (uint64_t)reg * 4 >= kBar5Bytes; }

// ---- Fields (gc_11_0_0_sh_mask.h, nbio_4_3_0_sh_mask.h:60797-60812) --------------------------------------
constexpr uint32_t kF32Halt = 1u << 0, kF32Th1Reset = 1u << 13;
constexpr uint32_t kStatusIdle = 1u << 0;
constexpr uint32_t kRbEnable = 1u << 0, kRbSizeMask = 0x3Eu, kRbSizeShift = 1, kRbWptrPoll = 1u << 8,
                   kRbF32WptrPoll = 1u << 11, kRbRptrWb = 1u << 12, kRbPriv = 1u << 23;
constexpr uint32_t kIbEnable = 1u << 0;
constexpr uint32_t kDbEnable = 1u << 28, kDbOffMask = 0x0FFFFFFCu, kDbOffShift = 2;
constexpr uint32_t kHangCountMask = 0xFFu;
constexpr uint32_t kUtclRespMask = 0x600u, kUtclRedoMask = 0x1Fu;
constexpr uint32_t kUtclPageKeep = 0x00FF0FFFu;                                  // sdma_v6_0.c:590-596
constexpr uint32_t kUtclPageSet = (2u << 12) | (3u << 14) | (1u << 24);          // read NOA, write BYPASS, LLC_NOALLOC
constexpr uint32_t kAddrLoMask = 0xFFFFFFFCu, kBaseHiMask = 0x00FFFFFFu;
constexpr uint32_t kGrbmFields = 0x3FFFu;                                        // PIPEID, MEID, VMID, QUEUEID, CTXID
constexpr uint32_t kPfpReset = (1u << 18) | (1u << 19), kMeReset = (1u << 20) | (1u << 21);
constexpr uint32_t kMecReset = 0xFu << 16;
constexpr uint32_t kStartHiMask = 0x3FFFFFFFu;
// S2A_DOORBELL_ENTRY_2_CTRL: ENABLE 0, AWID 5:1, RANGE_OFFSET 16:7, RANGE_SIZE 24:17, AWADDR_31_28 31:28.
constexpr uint32_t kS2aEnable = 1u << 0, kS2aAwidMask = 0x3Eu, kS2aOffMask = 0x1FF80u, kS2aSizeMask = 0x1FE0000u,
                   kS2aAddrMask = 0xF0000000u;
constexpr uint32_t kS2aMask = kS2aEnable | kS2aAwidMask | kS2aOffMask | kS2aSizeMask | kS2aAddrMask;

// ---- Doorbell (revised D3, 2026-10-14) ------------------------------------------------------------------
// SDMA0 queue 0's doorbell index is a dword index: AMDGPU_NAVI10_DOORBELL_sDMA_ENGINE0 (0x100, 64-bit units)
// << 1 = 0x200 (sdma_v6_0.c:1341-1342, amdgpu_doorbell.h:210; AM ip.py:592 and :601 write doorbell * 2).
// WDOORBELL64 stores 64 bits at the u32 pointer cpu_addr + index (amdgpu_doorbell_mgr.c:103-109; the kernel
// doorbells are the first pages of BAR2, :149-178), so the byte offset in BAR2 is 0x200 * 4 = 0x800; AM rings
// doorbell64 (BAR2 as 64-bit words) at 0x100 * 8 = 0x800 (ops_amd.py:759, amdev.py:159).
constexpr uint32_t kDbIndex = 0x200;
constexpr uint64_t kDbByte = (uint64_t)kDbIndex * 4;
constexpr uint64_t kBar2Need = kDbByte + 8;
// NBIO routing for SDMA, port 2: enable, AWID 0xE, AWADDR_31_28 3, RANGE_OFFSET the doorbell index, RANGE_SIZE 4
// (AM ip.py:44-50, :573). Linux writes the same fields with RANGE_SIZE 20 x 2 instances = 40 (nbio_v4_3.c:64-100,
// soc21.c:517, sdma_v6_0.c:565-568); 4 covers queue 0's doorbell and leaves SDMA1's (0x214) unrouted. Applied as
// Linux does, read-modify-write over these five fields.
constexpr uint32_t kS2aRangeSize = 4;
constexpr uint32_t kS2aValue = kS2aEnable | (0xEu << 1) | (kDbIndex << 7) | (kS2aRangeSize << 17) | (3u << 28);

// ---- Placement (D2) ---------------------------------------------------------------------------------------
constexpr uint64_t kPage     = 0x1000;
constexpr uint64_t kRingOff  = 0x8A00000;            // 4 KiB ring, 1024 dwords
constexpr uint64_t kWbOff    = 0x8A01000;            // rptr slot at +0, wptr poll slot at +0x20
constexpr uint64_t kRptrSlot = kWbOff, kWptrSlot = kWbOff + 0x20;
constexpr uint64_t kTestOff  = 0x8A02000;            // E2's VRAM target
constexpr uint64_t kOwnedOff = kRingOff, kOwnedLen = 3 * kPage;   // protected range kind 15
constexpr uint32_t kRingDw   = 1024, kRbSize = 10;   // log2(ring dwords), sdma_v6_0.c:498-507
constexpr uint32_t kAlignDw  = 16;                   // commit pads to 16 dwords with NOPs (align_mask 0xf)
constexpr uint32_t kHostOff3 = 8;                    // E4's fence lands at host page A + 8
constexpr uint32_t kBefore = 0xCAFEDEADu, kAfter = 0xDEADBEEFu;   // sdma_v6_0.c:911, :934

struct Geo { uint64_t ring, rptr, wptr, test; };     // MC addresses
inline Geo geo(uint64_t fbBase) { return { fbBase + kRingOff, fbBase + kRptrSlot, fbBase + kWptrSlot, fbBase + kTestOff }; }

// ---- Packets (sdma_v6_0_0_pkt_open.h; op in 7:0, sub-op in 15:8) ------------------------------------------
constexpr uint32_t kOpNop = 0, kOpWrite = 2, kOpFence = 5;
constexpr uint32_t kHdrWrite = kOpWrite;                  // SUB_OP WRITE_LINEAR 0
constexpr uint32_t kHdrFence = kOpFence | (3u << 16);     // MTYPE 3 (UC), sdma_v6_0.c:361-362
// One linear WRITE of one dword (ring test, sdma_v6_0.c:929-934): 5 dwords.
inline uint32_t pkt_write(uint32_t *p, uint64_t mc, uint32_t data) {
	p[0] = kHdrWrite; p[1] = (uint32_t)mc; p[2] = (uint32_t)(mc >> 32); p[3] = 0; p[4] = data;   // COUNT = n - 1 = 0
	return 5;
}
// One 32-bit FENCE without TRAP (sdma_v6_0.c:356-367): 4 dwords. The address must be dword aligned.
inline uint32_t pkt_fence(uint32_t *p, uint64_t mc, uint32_t seq) {
	p[0] = kHdrFence; p[1] = (uint32_t)mc; p[2] = (uint32_t)(mc >> 32); p[3] = seq;
	return 4;
}
// Pad with NOP (0) to the next 16-dword boundary, as amdgpu_ring_commit. Returns the padded length.
inline uint32_t pad(uint32_t *p, uint32_t n) {
	while (n % kAlignDw) p[n++] = kOpNop;
	return n;
}

// ---- Modes (revised D3) --------------------------------------------------------------------------------
enum Mode : uint32_t { kModeNone = 0, kModeMmio = 1, kModeDb = 2 };
inline bool valid_mode(uint32_t m) { return m == kModeMmio || m == kModeDb; }

// ---- Refusal reasons. They continue the kext's list after the GMC's (30..42). ----------------------------------
enum Why : uint32_t {
	kWhyOk = 0,
	kWhyGcDown = 43,     // AUTOLOAD_RLC not successful in this load, BOOTLOAD_COMPLETE clear, or CP_STAT not 0
	kWhyOrder = 44,      // C1 after G6 or twice; G6 before C1; start before G6 or twice in one mode; test before start,
	                     // after stop, or before the previous test; stop with no queue; ring or journal full
	kWhyFirmware = 45,   // D6: 2c did not load types 87, 88, 89 with the pinned sizes in this load
	kWhyState = 46,      // SDMA0 not as expected before start (RB_ENABLE 1, or STATUS_REG not idle)
	kWhyOverlap = 47,    // a step-5 VRAM page overlaps a live DMCUB window or another protected range
	kWhyGart = 48,       // GART slot 0 could not be bound to the kext's page A, or a hub flush did not acknowledge
	kWhyLatched = 49,    // D7: a test failed (or this mode already timed out once); `sdma stop`, then reload
	kWhyFault = 50,      // GCVM_L2_PROTECTION_FAULT_STATUS not 0 before the step
	kWhyModeSwitch = 51, // a start in the other mode while the queue runs: `sdma stop` first
	kWhyDoorbell = 52,   // db mode: BAR2 not mapped (or IOPCIFamily and config space disagree), smaller than the
	                     // doorbell offset + 8, or the NBIO doorbell aperture (G1) not enabled
	kWhyArg = 53,        // not a test number (1 to 3) or a mode (mmio, db)
};

// ---- Plans --------------------------------------------------------------------------------------------------
// One entry: read-modify-write of reg (bits in mask set to value; mask 0xFFFFFFFF writes the register whole, mask 0
// writes it back as read). cmp is what the read-back must match (0: recorded only). An indirect entry goes through
// the RSMU pair. kOpRptrCheck is the stop plan's 1 s RB_RPTR stability check (no register write).
enum Op : uint32_t { kOpW = 0, kOpWInd = 1, kOpRptrCheck = 2 };
struct Entry { uint32_t op, reg, mask, value, cmp; };
inline uint32_t target(const Entry &e, uint32_t before) { return (before & ~e.mask) | (e.value & e.mask); }

constexpr uint32_t kPlanMax = 64;
constexpr uint32_t kJournalMax = 192;

struct PlanOut {
	Entry *e; uint32_t max, n; bool over;
	void add(uint32_t op, uint32_t reg, uint32_t mask, uint32_t value, uint32_t cmp) {
		if (n < max) e[n++] = { op, reg, mask, value, cmp }; else over = true;
	}
	void w(uint32_t reg, uint32_t value, uint32_t cmp) { add(kOpW, reg, 0xFFFFFFFFu, value, cmp); }
	void rmw(uint32_t reg, uint32_t mask, uint32_t value) { add(kOpW, reg, mask, value, mask); }
};

// C1: gfx_v11_0_config_gfx_rs64 (gfx_v11_0.c:3019-3102) with the pinned PFP, ME and MEC headers (lo 0x3000, hi
// 0x70000, all three). Each start register is read back right after its write, under its GRBM_GFX_CNTL selection;
// the deselect follows each start loop, before its reset pulse. The set half of a pulse is recorded, not compared.
// GRBM_GFX_CNTL is recorded too: on the 7600 XT it reads back 0 after every select (2026-10-17), although the
// per-pipe start values show each select took effect. Linux (soc21_grbm_select) and AM only write it.
constexpr uint32_t kUcodeLo = 0x3000, kUcodeHi = 0x70000;
constexpr uint32_t kStartLo = (uint32_t)((kUcodeHi << 30) | (kUcodeLo >> 2));   // 32-bit, as Linux: 0x00000C00
constexpr uint32_t kStartHi = kUcodeHi >> 2;                                   // 0x0001C000
inline uint32_t grbm_select(uint32_t me, uint32_t pipe) { return (pipe & 3u) | ((me & 3u) << 2); }   // soc21.c:270-280
inline uint32_t c1_plan(Entry *out, uint32_t max, uint32_t *count) {
	PlanOut p = { out, max, 0, false };
	*count = 0;
	for (uint32_t pipe = 0; pipe < 2; pipe++) {
		p.w(kGrbmGfxCntl, grbm_select(0, pipe), 0);
		p.w(kPfpStart, kStartLo, 0xFFFFFFFFu); p.w(kPfpStartHi, kStartHi, kStartHiMask);
	}
	p.w(kGrbmGfxCntl, 0, 0);
	p.add(kOpW, kCpMeCntl, kPfpReset, kPfpReset, 0); p.rmw(kCpMeCntl, kPfpReset, 0);
	for (uint32_t pipe = 0; pipe < 2; pipe++) {
		p.w(kGrbmGfxCntl, grbm_select(0, pipe), 0);
		p.w(kMeStart, kStartLo, 0xFFFFFFFFu); p.w(kMeStartHi, kStartHi, kStartHiMask);
	}
	p.w(kGrbmGfxCntl, 0, 0);
	p.add(kOpW, kCpMeCntl, kMeReset, kMeReset, 0); p.rmw(kCpMeCntl, kMeReset, 0);
	for (uint32_t pipe = 0; pipe < 4; pipe++) {
		p.w(kGrbmGfxCntl, grbm_select(1, pipe), 0);
		p.w(kMecStart, kStartLo, 0xFFFFFFFFu); p.w(kMecStartHi, kStartHi, kStartHiMask);
	}
	p.w(kGrbmGfxCntl, 0, 0);
	p.add(kOpW, kCpMecRs64Cntl, kMecReset, kMecReset, 0); p.rmw(kCpMecRs64Cntl, kMecReset, 0);
	if (p.over) return kWhyOrder;
	*count = p.n;
	return kWhyOk;
}

// E1: sdma_v6_0_start for SDMA0 queue 0 (Q2a rows 0 to 17 except 0b; row 10 only in db mode). The pointer registers
// are recorded, not compared; E1's done condition reads RB_RPTR afterwards. In mmio mode DOORBELL.ENABLE is 0 and
// DOORBELL_OFFSET is written back as read (sdma_v6_0.c:552-561, the non-doorbell branch); in db mode ENABLE is 1,
// OFFSET the doorbell index, and NBIO's SDMA range is routed (row 10). F32_WPTR_POLL_ENABLE is 1 in both, as Linux
// and AM (ip.py:604) set it.
inline uint32_t rb_cntl_initial_mask() { return kRbSizeMask | kRbPriv; }
inline uint32_t rb_cntl_initial_value() { return (kRbSize << kRbSizeShift) | kRbPriv; }
inline uint32_t rb_cntl_final_mask() { return kRbEnable | kRbRptrWb | kRbWptrPoll | kRbF32WptrPoll; }
inline uint32_t rb_cntl_final_value() { return kRbEnable | kRbRptrWb | kRbF32WptrPoll; }
inline uint32_t start_plan(uint32_t mode, uint64_t fbBase, Entry *out, uint32_t max, uint32_t *count) {
	*count = 0;
	if (!valid_mode(mode)) return kWhyArg;
	const Geo g = geo(fbBase);
	PlanOut p = { out, max, 0, false };
	const bool db = mode == kModeDb;
	p.rmw(kF32Cntl, kF32Halt, 0);                                                  // 0  enable(true)
	p.w(kSemWaitFail, 0, 0xFFFFFFFFu);                                             // 1
	p.rmw(kRbCntl, rb_cntl_initial_mask(), rb_cntl_initial_value());               // 2  RB_SIZE 10, RB_PRIV
	p.w(kRbRptr, 0, 0); p.w(kRbRptrHi, 0, 0); p.w(kRbWptr, 0, 0); p.w(kRbWptrHi, 0, 0);   // 3
	p.w(kWptrPollLo, (uint32_t)g.wptr, kAddrLoMask); p.w(kWptrPollHi, (uint32_t)(g.wptr >> 32), 0xFFFFFFFFu);   // 4
	p.w(kRptrAddrHi, (uint32_t)(g.rptr >> 32), 0xFFFFFFFFu);                       // 5
	p.w(kRptrAddrLo, (uint32_t)g.rptr & kAddrLoMask, kAddrLoMask);
	p.w(kRbBase, (uint32_t)(g.ring >> 8), 0xFFFFFFFFu);                            // 7
	p.w(kRbBaseHi, (uint32_t)(g.ring >> 40), kBaseHiMask);
	p.w(kMinorPtr, 1, 1);                                                          // 8
	p.w(kRbWptr, 0, 0); p.w(kRbWptrHi, 0, 0);
	p.rmw(kDoorbell, kDbEnable, db ? kDbEnable : 0);                               // 9
	if (db) p.rmw(kDoorbellOff, kDbOffMask, kDbIndex << kDbOffShift);
	else p.add(kOpW, kDoorbellOff, 0, 0, 0);                                       //    written back as read
	if (db) p.add(kOpWInd, kS2aEntry2, kS2aMask, kS2aValue, kS2aMask);             // 10 nbio sdma_doorbell_range
	p.w(kMinorPtr, 0, 1);                                                          // 11
	p.rmw(kWatchdog, kHangCountMask, 1);                                           // 12 max(100000 / 100000, 1)
	p.rmw(kUtcl1Cntl, kUtclRespMask | kUtclRedoMask, (3u << 9) | 9u);              // 13
	p.rmw(kUtcl1Page, ~kUtclPageKeep, kUtclPageSet);                               // 14
	p.rmw(kF32Cntl, kF32Halt | kF32Th1Reset, 0);                                   // 15
	p.rmw(kRbCntl, rb_cntl_final_mask(), rb_cntl_final_value());                   // 16
	p.rmw(kIbCntl, kIbEnable, kIbEnable);                                          // 17
	if (p.over) return kWhyOrder;
	*count = p.n;
	return kWhyOk;
}

// E6 (D9 plus the same-boot fallback): sdma_v6_0_gfx_stop and enable(false) (sdma_v6_0.c:395-415, :453-481), the
// doorbell off as AM's fini_hw (ip.py:575-580), then RB_RPTR checked stable over 1 s, then both pointers back to 0
// under MINOR_PTR_UPDATE so the next start begins from an empty ring. In db mode DOORBELL_OFFSET goes to 0 (AM) and
// NBIO's S2A entry 2 back to the value E1 found (s2aFound). The kext zeroes the ring and wb pages afterwards.
inline uint32_t stop_plan(uint32_t mode, uint32_t s2aFound, Entry *out, uint32_t max, uint32_t *count) {
	*count = 0;
	if (!valid_mode(mode)) return kWhyArg;
	PlanOut p = { out, max, 0, false };
	p.rmw(kRbCntl, kRbEnable, 0);
	p.rmw(kIbCntl, kIbEnable, 0);
	p.rmw(kDoorbell, kDbEnable, 0);
	if (mode == kModeDb) {
		p.rmw(kDoorbellOff, kDbOffMask, 0);
		p.add(kOpWInd, kS2aEntry2, 0xFFFFFFFFu, s2aFound, kS2aMask);
	}
	p.rmw(kF32Cntl, kF32Halt, kF32Halt);
	p.add(kOpRptrCheck, kRbRptr, 0, 0, 0);
	p.w(kMinorPtr, 1, 1);
	p.w(kRbRptr, 0, 0); p.w(kRbRptrHi, 0, 0); p.w(kRbWptr, 0, 0); p.w(kRbWptrHi, 0, 0);
	p.w(kMinorPtr, 0, 1);
	if (p.over) return kWhyOrder;
	*count = p.n;
	return kWhyOk;
}

// ---- C0: the registers `sdma state` reads (none before GC is up) -------------------------------------------
constexpr uint32_t kC0Regs[] = {
	kF32Cntl, kSdmaCntl, kSemWaitFail, kStatus, kWatchdog, kUtcl1Cntl, kUtcl1Page, kErrorLog,
	kRbCntl, kRbBase, kRbBaseHi, kRbRptr, kRbRptrHi, kRbWptr, kRbWptrHi, kRptrAddrHi, kRptrAddrLo, kIbCntl,
	kCtxStatus, kDoorbell, kDoorbellOff, kWptrPollHi, kWptrPollLo, kMinorPtr,
	kCpStat, kGrbmStatus, kCpMeCntl, kCpMecRs64Cntl, kPfpIcBaseLo, kMeIcBaseLo, kCpcIcBaseLo, kRlcCpSchedulers,
	kCpMesCntl, kGrbmGfxCntl, kGcFaultStatus, kGcFaultAddrLo, kGcFaultAddrHi,
};
constexpr uint32_t kC0Count = sizeof kC0Regs / sizeof kC0Regs[0];

// ---- State and gates (D6, D7, revised D3) --------------------------------------------------------------------
constexpr uint32_t kPfpBytes = 0x174C0, kMeBytes = 0x2D0F0, kMecBytes = 0x41300;   // m2 design table, types 87..89
enum Latch : uint32_t { kLatchNone = 0, kLatchTimeout = 1, kLatchHard = 2 };
struct State {
	bool c1Attempted, c1;   // C1 began writing / succeeded in this load
	bool pagesOwned;        // E1 took the step-5 pages (kind 15); kept until unload
	bool running;           // E1 ran and E6 has not
	uint32_t mode;          // the mode of the running (or last) queue
	uint32_t latch;         // D7
	uint32_t timedOut;      // bit per mode that has had a timeout-only failure in this load
	uint32_t wptrDw;        // dwords committed since the start
	uint32_t seq;           // last fence sequence sent
	uint32_t testsOk;       // bit n: test n succeeded since the start
	uint32_t starts;        // E1 attempts in this load
	uint32_t s2aFound;      // S2A_DOORBELL_ENTRY_2_CTRL as E1 found it (db mode)
	uint64_t cpFw[3];       // LOAD_IP_FW sizes 2c sent for types 87, 88, 89 (0: not loaded)
};

inline bool fw_pinned(const State &s) { return s.cpFw[0] == kPfpBytes && s.cpFw[1] == kMeBytes && s.cpFw[2] == kMecBytes; }

// C1: the GC gate first, then the firmware, then the order. g6Attempted: the GMC G6 step began in this load.
inline uint32_t may_c1(const State &s, bool gcUp, bool g6Attempted) {
	if (!gcUp) return kWhyGcDown;
	if (!fw_pinned(s)) return kWhyFirmware;
	if (s.c1Attempted || g6Attempted) return kWhyOrder;
	return kWhyOk;
}
// D6: G6 is allowed only after C1 succeeded in this load.
inline uint32_t may_g6(const State &s) { return s.c1 ? kWhyOk : kWhyOrder; }

// E1: argument, GC gate, latch, mode switch, order. A start is allowed before any start, or after E6. A hard latch
// blocks every start until reload; a timeout-only failure blocks a restart in the mode that timed out, so the
// revised D3's fallback (mmio timed out, E6, start db) is allowed once per mode.
inline uint32_t may_start(const State &s, uint32_t mode, bool gcUp, bool g6Done) {
	if (!valid_mode(mode)) return kWhyArg;
	if (!gcUp) return kWhyGcDown;
	if (s.latch == kLatchHard || (s.timedOut & (1u << mode))) return kWhyLatched;
	if (s.running) return mode == s.mode ? kWhyOrder : kWhyModeSwitch;
	if (!g6Done) return kWhyOrder;
	return kWhyOk;
}
inline void record_start(State &s, uint32_t mode, bool ok) {
	s.running = true; s.mode = mode; s.wptrDw = 0; s.testsOk = 0; s.starts++;
	if (!ok) s.latch = kLatchHard;
}

// E2 to E4: argument, order, latch, ring space. Test n needs test n - 1 to have succeeded since this start.
inline bool valid_test(uint32_t n) { return n >= 1 && n <= 3; }
inline uint32_t may_test(const State &s, uint32_t n) {
	if (!valid_test(n)) return kWhyArg;
	if (!s.running) return kWhyOrder;
	if (s.latch != kLatchNone) return kWhyLatched;
	if (n > 1 && !(s.testsOk >> (n - 1) & 1u)) return kWhyOrder;
	if (s.wptrDw + kAlignDw > kRingDw) return kWhyOrder;   // this step never wraps the ring
	return kWhyOk;
}
// D7's outcome of one test. Done: the target read the expected value, RB_RPTR caught up with the write pointer,
// STATUS_REG idle, no GC VM fault and ERROR_LOG unchanged. A timeout with nothing else wrong is timeout-only (the
// engine did not see the write pointer); anything else is hard.
struct TestRead { bool seen, rptrOk, idle; uint32_t fault; bool errChanged; };
inline uint32_t outcome(const TestRead &r) {
	if (r.seen && r.rptrOk && r.idle && !r.fault && !r.errChanged) return kLatchNone;
	if (!r.seen && r.idle && !r.fault && !r.errChanged) return kLatchTimeout;
	return kLatchHard;
}
inline void record_test(State &s, uint32_t n, uint32_t latch) {
	s.wptrDw += kAlignDw;
	if (latch == kLatchNone) { s.testsOk |= 1u << n; return; }
	if (latch == kLatchTimeout) s.timedOut |= 1u << s.mode;
	if (latch > s.latch) s.latch = latch;
}

// E6: allowed whenever the queue runs, latched or not.
inline uint32_t may_stop(const State &s) { return s.running ? kWhyOk : kWhyOrder; }
// A stop clears a timeout-only latch (the mode stays marked in timedOut); a hard latch stays. A stop whose writes did
// not read back latches hard.
inline void record_stop(State &s, bool ok) {
	s.running = false; s.wptrDw = 0; s.testsOk = 0;
	if (s.latch == kLatchTimeout) s.latch = kLatchNone;
	if (!ok) s.latch = kLatchHard;
}

// Kick, after the packet and NOP padding are in the ring: the new write pointer in bytes goes to the wptr poll slot
// (64 bits), then an HDP flush, then mmio: RB_WPTR and RB_WPTR_HI (sdma_v6_0.c:226-237); db: one 64-bit store at BAR2
// + kDbByte (WDOORBELL64, sdma_v6_0.c:211-224; AM ops_amd.py:71-85 writes the wptr slot, then the doorbell).
inline uint64_t wptr_bytes(uint32_t wptrDw) { return (uint64_t)wptrDw << 2; }

// Records one before/after/target, as the GMC journal.
enum Phase : uint32_t { kPhaseC1 = 1, kPhaseE1 = 2, kPhaseE6 = 3 };
struct Rec { uint32_t phase, op, reg, before, after, target, cmp; };
inline bool mismatch(const Rec &r) { return ((r.after ^ r.target) & r.cmp) != 0; }

} // namespace n33sdma
