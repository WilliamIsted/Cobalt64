// n33_ih_pure.h: decisions for stage 3 step 4 on Navi 33: one IH 6.0.2 ring (ring 0) in VRAM, its write pointer
// written back to GART host page C, no doorbell, polled first (I1, I2), an ENABLE_INTR 1 polled variant, the MSI path
// behind the boot-arg n33-ih-msi (I3), the SDMA trap source after `cp stop` (I4) and the stop (I5), of the overview
// repo's RDNA3/Tasks/navi33-cobalt64-port/stage3-step4-ih-design.md with its design pass (stage3-step4-ih-design-pass.md)
// and William's answers of 2026-10-18. Host-testable: no kernel types, no I/O. The kext reads the live registers, asks
// start_plan() and stop_plan() for every IH and NBIO write with each value computed here, and builds every packet with
// the helpers below. The plan entries, the journal records and their read-back rule are step 5's (n33sdma::Entry,
// PlanOut, Rec). Linux references are to the pinned tree 3857c2fe (ih_v6_0.c, nbio_v4_3.c, amdgpu_ih.c, gfx_v11_0.c,
// sdma_v6_0.c); AM references are to tinygrad 02e698160eed (tinygrad/runtime/support/am/ip.py).
#pragma once
#include <stdint.h>
#include "n33_sdma_pure.h"
#include "n33_gmc_pure.h"
#include "n33_cp_pure.h"

namespace n33ih {

using n33sdma::Entry;
using n33sdma::PlanOut;
using n33sdma::Rec;
using n33sdma::target;
using n33sdma::mismatch;
using n33sdma::kOpW;
using n33sdma::kLatchNone;
using n33sdma::kLatchHard;

// ---- Registers (absolute dwords) ---------------------------------------------------------------------------------
// OSSSYS 6.0.2 segment 0 at 0x10A0 (measured: the kext's IP discovery dump, hwId 40, 2026-10-17) plus the
// osssys_6_0_0_offset.h offsets, all BASE_IDX 0. v6, not the fork's v7 header: v7 permutes RPTR, WPTR and BASE.
constexpr uint32_t kOssSeg0 = 0x10A0;
constexpr uint32_t kRbCntl = kOssSeg0 + 0x80, kRbBase = kOssSeg0 + 0x81, kRbBaseHi = kOssSeg0 + 0x82,
                   kRbRptr = kOssSeg0 + 0x83, kRbWptr = kOssSeg0 + 0x84, kWptrAddrHi = kOssSeg0 + 0x85,
                   kWptrAddrLo = kOssSeg0 + 0x86, kDoorbellRptr = kOssSeg0 + 0x87, kIhCntl = kOssSeg0 + 0xC0,
                   kFloodCntl = kOssSeg0 + 0xD5, kStormCntl = kOssSeg0 + 0xDA, kMsiStorm = kOssSeg0 + 0xF1,
                   kChicken = kOssSeg0 + 0x18D;
// NBIO 4.3 BIF_BX0_INTERRUPT_CNTL / _CNTL2, BASE_IDX 2 (nbio_4_3_0_offset.h:370-373) on NBIF segment 2 at 0xD20
// (n33_gmc_pure.h; AM trace's BIF_BX0_BIF_DOORBELL_INT_CNTL at 0xE1E = 0xD20 + 0xFE).
constexpr uint32_t kNbioSeg2 = 0xD20;
constexpr uint32_t kIntCntl = kNbioSeg2 + 0xF1, kIntCntl2 = kNbioSeg2 + 0xF2;
// GC: CP_ME1_PIPE0_INT_CNTL (0x1E25 at BASE_IDX 0, gc_11_0_0_offset.h:4232) on GC segment 0 at 0x1260; the GC and
// MMHUB fault status (n33_cp_pure.h, n33_gmc_pure.h); SDMA0_CNTL (n33_sdma_pure.h).
constexpr uint32_t kMe1Pipe0IntCntl = 0x1260 + 0x1E25;
constexpr uint32_t kGcFaultStatus = n33cp::kGcFaultStatus;
constexpr uint32_t kMmFaultStatus = n33gmc::kMm.faultStatus;
constexpr uint32_t kSdmaCntl = n33sdma::kSdmaCntl;

// ---- Fields (osssys_6_0_0_sh_mask.h:170-230, 442, 488, 856; nbio_4_3_0_sh_mask.h:703-723; gc_11_0_0_sh_mask.h) ----
constexpr uint32_t kRbEnable = 1u << 0, kRbSizeShift = 1, kRbSizeMask = 0x3Eu, kWbEnable = 1u << 8,
                   kOvfEnable = 1u << 16, kEnableIntr = 1u << 17, kSnoop = 1u << 20, kRearm = 1u << 21, kMcRo = 1u << 22,
                   kVmidMask = 0xFu << 24, kSpaceShift = 28, kSpaceMask = 7u << 28, kOvfClear = 1u << 31;
constexpr uint32_t kWptrOverflow = 1u << 0, kWptrOffsetMask = 0x3FFFCu;
constexpr uint32_t kDbRptrEnable = 1u << 28;
constexpr uint32_t kStormClient18 = 1u << 18, kFloodEnable = 1u << 3, kMsiStormDelayMask = 0xFFFu;
constexpr uint32_t kDummyRdOverride = 1u << 0, kReqNonsnoop = 1u << 3;
constexpr uint32_t kTimeStampIntEnable = 1u << 26;           // CP_ME1_PIPE0_INT_CNTL (gc_11_0_0_sh_mask.h:15776)
constexpr uint32_t kSdmaTrapEnable = 1u << 0;                // SDMA0_CNTL (gc_11_0_0_sh_mask.h:62)

// ---- Placement (design-pass section 3) ---------------------------------------------------------------------------
// Ring 0: 256 KiB at VRAM 0x8B00000 (Q-IH-a; amdgpu_ih.h:30, AM ip.py:458), protected kind 19 once `ih start` owns it.
// The wptr write-back: GART host page C (the GMC code's host page 2) at slot 2, dword 0. The NBIO dummy read address is
// the GMC dummy page's bus address (n33GmcDummyPa), as Linux writes adev->dummy_page_addr (nbio_v4_3.c:221).
constexpr uint64_t kRingOff = 0x8B00000, kRingBytes = 0x40000;
constexpr uint32_t kPtrMask = (uint32_t)kRingBytes - 1;
constexpr uint32_t kRbSize = 16;                             // order_base_2(262144 / 4), ih_v6_0.c:217
constexpr uint32_t kKind = 19;
constexpr uint32_t kSlotC = 2, kHostC = 2;
constexpr uint32_t kHostWb = 0;                              // byte offset of the wptr shadow in host page C
constexpr uint32_t kIvBytes = 32, kIvDw = 8;                 // amdgpu_ih.c:263-296: rptr advances 32 bytes per entry
// The fences each source's packet writes: host page B (step 6's) past its K5b fence; host page A (step 5's) past E4's.
constexpr uint32_t kHostFenceCp = 0x198, kHostFenceSdma = 0x10;
struct Geo { uint64_t ring, wb, fenceCp, fenceSdma; };       // MC addresses
inline Geo geo(uint64_t fbBase) {
	return { fbBase + kRingOff, n33gmc::slot_mc(kSlotC) + kHostWb, n33gmc::slot_mc(n33cp::kSlotB) + kHostFenceCp,
	         n33gmc::slot_mc(0) + kHostFenceSdma };
}

// ---- Modes (William, Q-IH-i): 1 polled with ENABLE_INTR 0 (I1/I2); 2 polled with ENABLE_INTR 1 and RPTR_REARM 1,
// as Linux (ih_v6_0.c:168, :280) and AM (ip.py:484) run ring 0, allowed only with MSI, MSI-X and INTx confirmed off.
enum Mode : uint32_t { kModeNone = 0, kModePolled = 1, kModePolledIntr = 2 };
inline bool valid_mode(uint32_t m) { return m == kModePolled || m == kModePolledIntr; }

// ---- IH_RB_CNTL (D3, corrected by design-pass 1.3) ---------------------------------------------------------------
// ih_v6_0_rb_cntl (ih_v6_0.c:215-236): MC_SPACE 4 (a VRAM ring, AM's; use_bus_addr false), WPTR_OVERFLOW_CLEAR 1,
// WPTR_OVERFLOW_ENABLE 1, RB_SIZE 16, WPTR_WRITEBACK_ENABLE 1, MC_SNOOP 1, MC_RO 0, MC_VMID 0; RPTR_REARM only with an
// interrupt line (ih_v6_0.c:279-280). The kext read-modify-writes only these fields.
constexpr uint32_t kCntlFields = kRbSizeMask | kWbEnable | kOvfEnable | kSnoop | kRearm | kMcRo | kVmidMask | kSpaceMask |
                                 kOvfClear;
inline uint32_t cntl_value(uint32_t mode) {
	return (4u << kSpaceShift) | kOvfClear | kOvfEnable | (kRbSize << kRbSizeShift) | kWbEnable | kSnoop |
	       (mode == kModePolledIntr ? kRearm : 0);
}
// The value I1's done condition expects, from a register whose other fields read 0: RB_ENABLE 1, the write-only
// WPTR_OVERFLOW_CLEAR back at 0 (AM wrote 0xC0310020 and read 0x40310020, am-trace:92, 110). Mode 1: 0x40110121.
inline uint32_t cntl_expect(uint32_t mode, uint32_t others = 0) {
	return (others & ~(kCntlFields | kRbEnable | kEnableIntr)) | ((cntl_value(mode) | kRbEnable) & ~kOvfClear) |
	       (mode == kModePolledIntr ? kEnableIntr : 0);
}
constexpr uint32_t kCntlCheck = kCntlFields | kRbEnable | kEnableIntr;

// ---- Plans ------------------------------------------------------------------------------------------------------
// I1 (D4, ih_v6_0_irq_init, ih_v6_0.c:321-398, reduced to ring 0 with no doorbell):
//  1 disable (toggle_ring_interrupts false, :128-188): RB_ENABLE 0, ENABLE_INTR 0, then RPTR and WPTR to 0;
//  2 nbio ih_control (nbio_v4_3.c:216-236): CNTL2 = dummy >> 8, then IH_DUMMY_RD_OVERRIDE 0 and IH_REQ_NONSNOOP_EN 0;
//  3 enable_ring (:265-308): BASE, BASE_HI, IH_RB_CNTL, WPTR_ADDR_LO/HI, WPTR 0, RPTR 0, DOORBELL_RPTR.ENABLE 0;
//  4 storm, flood and MSI-storm controls (:359-376);
//  5 toggle true (:136-168): RB_ENABLE 1 with OVERFLOW_CLEAR 0, then OVERFLOW_CLEAR 1, then OVERFLOW_CLEAR 0 with
//    ENABLE_INTR per mode. Linux's pci_set_master (bus mastering already works) and the self-IV force update (no
//    ring 1) are not done. IH_CHICKEN is left alone (its GPA bit is for direct load, :336-343).
// IH_REQ_NONSNOOP_EN: Linux's code writes 0 (its ring is in GTT), but its own comment at nbio_v4_3.c:231 says 1 for a
// ring in non-cacheable memory such as VRAM, which ours is. William (2026-10-18): take the code's 0 and flag it.
// Which one the hardware needs here is unverified.
constexpr uint32_t kNonsnoopValue = 0;
constexpr uint32_t kAll = 0xFFFFFFFFu;
inline uint32_t start_plan(uint32_t mode, uint64_t ringMc, uint64_t wbMc, uint64_t dummyPa, Entry *out, uint32_t max,
                           uint32_t *count) {
	*count = 0;
	if (!valid_mode(mode) || (ringMc & 0xFF) || (wbMc & 3) || !dummyPa || (dummyPa & 0xFF) || (dummyPa >> 40)) return 226;
	PlanOut p = { out, max, 0, false };
	p.rmw(kRbCntl, kRbEnable | kEnableIntr, 0);                              // 1
	p.w(kRbRptr, 0, kAll); p.w(kRbWptr, 0, 0);                               //   WPTR recorded: the IH owns it
	p.w(kIntCntl2, (uint32_t)(dummyPa >> 8), kAll);                          // 2
	p.rmw(kIntCntl, kDummyRdOverride | kReqNonsnoop, kNonsnoopValue ? kReqNonsnoop : 0);
	p.w(kRbBase, (uint32_t)(ringMc >> 8), kAll);                             // 3
	p.w(kRbBaseHi, (uint32_t)(ringMc >> 40) & 0xFFu, 0xFFu);
	p.add(kOpW, kRbCntl, kCntlFields, cntl_value(mode), kCntlFields & ~kOvfClear);
	p.w(kWptrAddrLo, (uint32_t)wbMc, kAll);
	p.w(kWptrAddrHi, (uint32_t)(wbMc >> 32) & 0xFFFFu, 0xFFFFu);
	p.w(kRbWptr, 0, 0); p.w(kRbRptr, 0, kAll);
	p.rmw(kDoorbellRptr, kDbRptrEnable, 0);
	p.rmw(kStormCntl, kStormClient18, kStormClient18);                       // 4
	p.rmw(kFloodCntl, kFloodEnable, kFloodEnable);
	p.rmw(kMsiStorm, kMsiStormDelayMask, 3);
	p.add(kOpW, kRbCntl, kRbEnable | kOvfClear, kRbEnable, kRbEnable);      // 5
	p.add(kOpW, kRbCntl, kOvfClear, kOvfClear, 0);                           //   the pulse: write-only, recorded
	p.add(kOpW, kRbCntl, kOvfClear | kEnableIntr, mode == kModePolledIntr ? kEnableIntr : 0, kRbEnable | kEnableIntr);
	if (p.over) return 220;
	*count = p.n;
	return 0;
}
// I5 (ih_v6_0_irq_disable -> toggle_interrupts false, ih_v6_0.c:415-421): RB_ENABLE 0, ENABLE_INTR 0 and (ours)
// RPTR_REARM 0, then RPTR and WPTR to 0. The CP and SDMA enables go back to what the follow-ups found, if they set them.
struct Found { bool cpInt, trap; uint32_t cpIntBit, trapBit; };
inline uint32_t stop_plan(const Found &f, bool gcUp, Entry *out, uint32_t max, uint32_t *count) {
	*count = 0;
	PlanOut p = { out, max, 0, false };
	p.rmw(kRbCntl, kRbEnable | kEnableIntr | kRearm, 0);
	p.w(kRbRptr, 0, kAll); p.w(kRbWptr, 0, 0);
	if (gcUp && f.cpInt) p.rmw(kMe1Pipe0IntCntl, kTimeStampIntEnable, f.cpIntBit & kTimeStampIntEnable);
	if (gcUp && f.trap) p.rmw(kSdmaCntl, kSdmaTrapEnable, f.trapBit & kSdmaTrapEnable);
	if (p.over) return 220;
	*count = p.n;
	return 0;
}

// ---- `ih state` (I0): the registers it reads, all read-only; refused before G6 -----------------------------------
constexpr uint32_t kStateRegs[] = {
	kRbCntl, kRbBase, kRbBaseHi, kRbRptr, kRbWptr, kWptrAddrHi, kWptrAddrLo, kDoorbellRptr, kIhCntl, kFloodCntl,
	kStormCntl, kMsiStorm, kChicken, kIntCntl, kIntCntl2, kMe1Pipe0IntCntl, kSdmaCntl, kGcFaultStatus, kMmFaultStatus,
};
constexpr uint32_t kStateCount = sizeof kStateRegs / sizeof kStateRegs[0];

// ---- PCI config (design-pass section 4): the ENABLE_INTR 1 polled mode needs no host vector live --------------------
// Command bit 10 (Interrupt Disable) set, MSI Message Control bit 0 (Enable) clear, MSI-X Message Control bit 15
// (Enable) clear. A missing capability counts as off.
constexpr uint16_t kCmdIntxDisable = 1u << 10, kMsiEnable = 1u << 0, kMsixEnable = 1u << 15;
inline bool pci_quiet(uint16_t command, uint16_t msiCtl, uint16_t msixCtl) {
	return (command & kCmdIntxDisable) && !(msiCtl & kMsiEnable) && !(msixCtl & kMsixEnable);
}

// ---- Sources and packets -----------------------------------------------------------------------------------------
// `ih recv <v>`: 0 CP EOP from the step-6 queue; 1 the same after setting CP_ME1_PIPE0_INT_CNTL.TIME_STAMP_INT_ENABLE
// (Q-IH-h, only after 0 saw its fence and no IV); 2 SDMA trap from the step-5 queue (I4, only after `cp stop`); 3 the
// same after setting SDMA0_CNTL.TRAP_ENABLE (only after 2 saw its fence and no IV).
enum Src : uint32_t { kSrcCp = 0, kSrcCpInt = 1, kSrcSdma = 2, kSrcSdmaTrap = 3 };
inline bool valid_src(uint32_t v) { return v <= kSrcSdmaTrap; }
inline bool src_is_cp(uint32_t v) { return v == kSrcCp || v == kSrcCpInt; }
// CP EOP: SOC21_IH_CLIENTID_GRBM_CP 0x14 (soc15_ih_clientid.h:100), GFX_11_0_0__SRCID__CP_EOP_INTERRUPT 181
// (irqsrcs_gfx_11_0_0.h:57); ring_id for MEC1 pipe 0 queue 0 is 0x04 (gfx_v11_0.c:6547-6549: me 3:2, pipe 1:0, queue
// 6:4), so dw0 0x0004B514 with VMID 0. SDMA trap: client GFX 0x0A (soc15_ih_clientid.h:93), source 49 (:33).
constexpr uint32_t kClientCp = 0x14, kSrcIdEop = 181, kRingIdMec1P0Q0 = 0x04;
constexpr uint32_t kClientGfx = 0x0A, kSrcIdSdmaTrap = 49;
inline uint32_t want_client(uint32_t v) { return src_is_cp(v) ? kClientCp : kClientGfx; }
inline uint32_t want_srcid(uint32_t v) { return src_is_cp(v) ? kSrcIdEop : kSrcIdSdmaTrap; }

// RELEASE_MEM as step 6's K5b fence (GL2_WB, DATA_SEL 1) with INT_SEL 2: dword 2 0x22000000 (nvd.h:382;
// gfx_v11_0.c:6093-6094 emits INT_SEL(2) when an interrupt is wanted). 8 dwords.
constexpr uint32_t kIntSel2 = 2u << 24;
inline uint32_t pkt_release_mem_int(uint32_t *p, uint64_t mc, uint32_t seq) {
	const uint32_t n = n33cp::pkt_release_mem(p, mc, seq, true);
	p[2] |= kIntSel2;
	return n;
}
// SDMA FENCE then TRAP with INT_CONTEXT 0, as sdma_v6_0_ring_emit_fence with AMDGPU_FENCE_FLAG_INT (sdma_v6_0.c:356-
// 385; SDMA_OP_TRAP 6, sdma_v6_0_0_pkt_open.h:31). 6 dwords before the pad.
constexpr uint32_t kSdmaOpTrap = 6;
inline uint32_t pkt_sdma_fence_trap(uint32_t *p, uint64_t mc, uint32_t seq) {
	const uint32_t n = n33sdma::pkt_fence(p, mc, seq);
	p[n] = kSdmaOpTrap; p[n + 1] = 0;
	return n + 2;
}

// ---- IV decode (amdgpu_ih_decode_iv_helper, amdgpu_ih.c:263-296) --------------------------------------------------
struct Iv { uint32_t client, src, ring, vmid, vmidSrc, tsSrc, pasid, node; uint64_t ts; uint32_t data[4]; };
inline Iv decode(const uint32_t dw[kIvDw]) {
	Iv e = {};
	e.client = dw[0] & 0xFF; e.src = (dw[0] >> 8) & 0xFF; e.ring = (dw[0] >> 16) & 0xFF; e.vmid = (dw[0] >> 24) & 0xF;
	e.vmidSrc = dw[0] >> 31;
	e.ts = dw[1] | ((uint64_t)(dw[2] & 0xFFFF) << 32); e.tsSrc = dw[2] >> 31;
	e.pasid = dw[3] & 0xFFFF; e.node = (dw[3] >> 16) & 0xFF;
	for (int i = 0; i < 4; i++) e.data[i] = dw[4 + i];
	return e;
}
inline uint32_t wptr_bytes(uint32_t wptrReg) { return wptrReg & kWptrOffsetMask; }     // ih_v6_0_get_wptr: & ptr_mask
inline uint32_t entries_between(uint32_t rptr, uint32_t wptr) { return ((wptr - rptr) & kPtrMask) / kIvBytes; }
inline uint32_t next_rptr(uint32_t rptr) { return (rptr + kIvBytes) & kPtrMask; }
constexpr uint32_t kDrainMax = 16;                           // entries handled per recv or per MSI
constexpr uint32_t kIvJournalMax = 64;
constexpr uint32_t kJournalMax = 128;
constexpr uint32_t kPlanMax = 32;
constexpr uint32_t kFenceUs = 100000, kIvUs = 100000;        // D8 / the design's I2: 100 ms each, bounded

// ---- Refusal reasons (design-pass Q-IH-j: 220 to 226; 43 keeps its step-6 meaning, GC not up) ---------------------
enum Why : uint32_t {
	kWhyOk = 0,
	kWhyGcDown = n33cp::kWhyGcDown,   // 43
	kWhyOrder = 220,     // ih state register reads or ih start before G6; ih start twice; ih recv before ih start, cp
	                     // source with no running step-6 queue (or before cp test 2/K3, or a submit outstanding), SDMA
	                     // source while the compute queue runs or before sdma test 3, a follow-up without its first
	                     // variant's fence-landed-no-IV, a variant repeated after it saw no IV; ih arm before start or
	                     // twice; ih stop with no ring; a plan or journal full; the SDMA ring full
	kWhyOverlap = 221,   // the IH ring block overlaps a live DMCUB window or another protected range
	kWhyFault = 222,     // GC or MMHUB fault status non-zero before the step
	kWhyLatched = 223,   // D8: a recv saw an unexpected IV, an overflow, a fault after, or a start or arm did not read back
	kWhyMsiGate = 224,   // ih arm: n33-ih-msi not set, no messaged interrupt source, MSI already enabled by another owner,
	                     // or the event source could not be registered; mode 2: MSI, MSI-X or INTx not confirmed off
	kWhyGart = 225,      // GART slot 2 could not be bound to host page C, the slot 1 (host page B) or slot 0 (host
	                     // page A) PTE is not the kext's, or an MMHUB flush did not acknowledge
	kWhyArg = 226,       // not a mode (1, 2), a source (0 to 3) or a journal index kind
};

// ---- State and gates (D8) ----------------------------------------------------------------------------------------
enum Outcome : uint32_t { kOutOk = 0, kOutNoIv = 1, kOutFenceMissing = 2, kOutBad = 3 };
struct State {
	bool ringOwned;      // ih start took the ring block (kind 19); kept until unload, as the other steps' pages
	bool running;        // ih start ran and ih stop has not
	uint32_t mode;
	bool armed;          // I3: the event source is live and ENABLE_INTR/RPTR_REARM are set
	uint32_t latch;
	uint32_t rptr;       // bytes; the kext's read pointer (the handler's while armed)
	uint32_t starts;
	uint32_t recvOk;     // bit per source that succeeded
	uint32_t noIv;       // bit per source whose fence landed with no IV (D8: reported, not retried blindly)
	Found found;         // the CP and SDMA interrupt enables as the follow-ups found them
	uint32_t msiCount;   // handler runs while armed
	uint32_t msiWant;    // handler runs that drained the expected entry
	uint32_t ivN;        // IV records kept
};

inline uint32_t may_state_regs(bool g6) { return g6 ? kWhyOk : kWhyOrder; }
inline uint32_t may_start(const State &s, uint32_t mode, bool gcUp, bool g6, bool pciQuiet) {
	if (!valid_mode(mode)) return kWhyArg;
	if (!gcUp) return kWhyGcDown;
	if (s.latch == kLatchHard) return kWhyLatched;
	if (s.running || !g6) return kWhyOrder;
	if (mode == kModePolledIntr && !pciQuiet) return kWhyMsiGate;
	return kWhyOk;
}
inline void record_start(State &s, uint32_t mode, bool ok) {
	s.running = true; s.mode = mode; s.armed = false; s.rptr = 0; s.starts++; s.recvOk = 0; s.noIv = 0;
	if (!ok) s.latch = kLatchHard;
}
// cpReady: the step-6 queue runs, unlatched, K3 (host page B bound) succeeded, no W1 submit outstanding.
// sdmaReady: the step-5 queue runs, unlatched, E4 (host page A bound) succeeded, ring room for one more 16 dwords.
inline uint32_t may_recv(const State &s, uint32_t v, bool cpRunning, bool cpReady, bool sdmaReady, bool pciQuiet) {
	if (!valid_src(v)) return kWhyArg;
	if (!s.running) return kWhyOrder;
	if (s.latch != kLatchNone) return kWhyLatched;
	if (src_is_cp(v) ? !cpReady : (cpRunning || !sdmaReady)) return kWhyOrder;   // I4 only after `cp stop`
	if (s.noIv >> v & 1u) return kWhyOrder;                                    // no blind retry
	if ((v == kSrcCpInt || v == kSrcSdmaTrap) && !(s.noIv >> (v - 1) & 1u)) return kWhyOrder;
	if (s.mode == kModePolledIntr && !s.armed && !pciQuiet) return kWhyMsiGate;
	return kWhyOk;
}
// D8: done is the fence landed, one or more expected IVs and none other, no overflow, no fault, and when armed the
// handler drained it. The fence landing with nothing in the ring is the follow-up case, not a latch; a fence that did
// not land is the queue's failure (step 5's or step 6's latch), not the IH's.
struct RecvRead { bool fence; uint32_t want, other; bool overflow; uint32_t gcFault, mmFault; bool armed, handled; };
inline uint32_t outcome(const RecvRead &r) {
	if (!r.fence) return kOutFenceMissing;
	if (r.other || r.overflow || r.gcFault || r.mmFault) return kOutBad;
	if (r.armed) return r.handled && r.want ? kOutOk : kOutBad;
	return r.want ? kOutOk : kOutNoIv;
}
inline void record_recv(State &s, uint32_t v, uint32_t out) {
	if (out == kOutOk) s.recvOk |= 1u << v;
	else if (out == kOutNoIv) s.noIv |= 1u << v;
	else if (out == kOutBad) s.latch = kLatchHard;
}
inline uint32_t may_arm(const State &s, bool bootArg) {
	if (!s.running || s.armed) return kWhyOrder;
	if (s.latch != kLatchNone) return kWhyLatched;
	if (!bootArg) return kWhyMsiGate;
	return kWhyOk;
}
inline uint32_t may_stop(const State &s) { return s.running ? kWhyOk : kWhyOrder; }
inline void record_stop(State &s, bool ok) {
	s.running = false; s.armed = false; s.rptr = 0;
	if (!ok) s.latch = kLatchHard;
}

enum Phase : uint32_t { kPhaseStart = 1, kPhaseRecv = 2, kPhaseArm = 3, kPhaseStop = 4 };

} // namespace n33ih
