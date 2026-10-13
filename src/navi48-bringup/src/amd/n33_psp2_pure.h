// n33_psp2_pure.h: decisions for PSP 13.0.7 milestone 2 (KM ring, LOAD_TOC, SMU, SETUP_TMR, the GC
// firmware loads, AUTOLOAD_RLC). Host-testable: no kernel types, no I/O. The kext builds every frame and
// command from these, in pages userspace cannot write. Design: the overview repo,
// RDNA3/Tasks/navi33-cobalt64-port/stage3-psp-m2-design.md (D3 to D6, D13). Linux references are to
// the pinned tree 3857c2fe.
#pragma once
#include <stdint.h>

namespace n33psp2 {

// MP0 absolute dwords (segment 0 at 0x16000; mp_13_0_2_offset.h _64 0x80, _67 0x83, _69..71 0x85..0x87).
constexpr uint32_t kC2pmsg64 = 0x16080;
constexpr uint32_t kC2pmsg67 = 0x16083;   // KM ring write pointer, in dwords: the doorbell
constexpr uint32_t kC2pmsg69 = 0x16085;
constexpr uint32_t kC2pmsg70 = 0x16086;
constexpr uint32_t kC2pmsg71 = 0x16087;
constexpr uint32_t kC2pmsg81 = 0x16091;
// HDP flush (D9): BIF_BX_DEV0_EPF0_VF0_HDP_MEM_COHERENCY_FLUSH_CNTL (NBIF segment 2 at 0xd20), then a
// read of RCC_CONFIG_MEMSIZE, as amdgpu_hdp_generic_flush.
constexpr uint32_t kHdpFlush = 0xE17;
constexpr uint32_t kMemsize  = 0xDE3;

// Mailbox flags (amdgpu_psp.h:57-65): ready and response share flag and mask.
constexpr uint32_t kTosFlag = 0x80000000u, kTosMask = 0x8000FFFFu;
constexpr uint32_t kRingCreateKm = 0x00020000u;   // PSP_RING_TYPE__KM (2) << 16
constexpr uint32_t kRingDestroy  = 0x00030000u;   // GFX_CTRL_CMD_ID_DESTROY_RINGS
inline bool tos_ok(uint32_t v) { return (v & kTosMask) == kTosFlag; }

// VRAM layout (offsets; MC = FB base + offset). Design table "Memory map and clash check".
constexpr uint64_t kPage      = 0x1000;
constexpr uint64_t kFwPri     = 0x8000000;    // the milestone 1 window, 1 MiB
constexpr uint64_t kFwPriSize = 1ull << 20;
constexpr uint64_t kRing      = 0x8100000;    // 4 KiB ring, 64 frames
constexpr uint64_t kCmd       = 0x8101000;    // 4 KiB command buffer (1 KiB used)
constexpr uint64_t kFence     = 0x8102000;    // 4 KiB fence page (one dword used)
constexpr uint64_t kPspPages  = 3 * kPage;    // ring + command + fence, contiguous from kRing
constexpr uint64_t kFwBuf     = 0x8200000;
constexpr uint64_t kFwBufSize = 0x300000;
constexpr uint64_t kTmrOff    = 0x3FC000000ull;   // where Linux put it on this card (D6)
constexpr uint64_t kTmrMax    = 32ull << 20;
constexpr uint32_t kRingBytes = 0x1000;
constexpr uint32_t kRingDw    = kRingBytes / 4;   // 1024
constexpr uint32_t kFrameDw   = 16;               // psp_gfx_rb_frame, 64 bytes

// GFX command ids (psp_gfx_if.h) and response byte offsets inside the 1 KiB buffer (+864 response).
constexpr uint32_t kCmdSetupTmr = 0x5, kCmdLoadIpFw = 0x6, kCmdLoadToc = 0x20, kCmdAutoloadRlc = 0x21;
constexpr uint32_t kRespStatus = 864, kRespFwAddrLo = 872, kRespFwAddrHi = 876, kRespTmrSize = 880;
constexpr uint32_t kCmdDw = 256;                  // 1024 bytes
constexpr uint32_t kTmrFlagsVirtPhys = 0x2;       // bitfield.virt_phy_addr = 1 (amdgpu_psp.c:846)

// fw_type values (psp_gfx_if.h, amdgpu_psp_get_fw_type).
constexpr uint32_t kFwSmu = 18, kFwRlcG = 8;
// The GC 11.0.2 / SDMA 6.0.2 set of the design's table, in Linux's order (rows 1 to 26).
constexpr uint32_t kGcOrder[26] = { 71, 72, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97,
                                    33, 34, 81, 82, 68, 69, 20, 21, 26, 48, 25, 7, 8 };
inline bool gc_type(uint32_t t) {
	for (uint32_t x : kGcOrder) if (x == t) return true;
	return false;
}

// Refusal reasons. They continue the kext's list (0..14 and 100 + kind are taken).
enum Why : uint32_t {
	kWhyOk = 0,
	kWhyNoRing = 15,      // no ring created by this kext load
	kWhyOrder = 16,       // command out of D3's order, or sent twice
	kWhyType = 17,        // fw_type not on the list (VCN 13, DMUB 51, PPTABLE 73 and the rest)
	kWhyRange = 18,       // image not inside fw_buf / fw_pri, misaligned, or empty
	kWhyTmrSize = 19,     // LOAD_TOC answer 0 or above 32 MiB after rounding
	kWhyTmrOverlap = 20,  // TMR overlaps a live DMCUB window or REGION4-6
	kWhyFailed = 21,      // an earlier command failed: nothing more is sent (D5)
	kWhyRingState = 22,   // ring create: sOS down, C2PMSG_64 not ready, wptr not 0, MMHUB base, or a ring exists
};
// D4's "no BAR0 write while a command is in flight" needs no code here: the kext serializes n33Call, and
// a command and its fence wait run inside one call.

enum Cmd : uint32_t { kToc = 1, kIpFw = 2, kTmr = 3, kAutoload = 4 };

struct State {
	bool ring, toc, smu, tmr, rlcG, autoload, failed;
	uint32_t tmrAnswer;          // LOAD_TOC's resp.tmr_size, as answered
	uint32_t fence;              // last fence index sent
	uint64_t loaded[2];          // fw_type bits 0..127 sent successfully
};

inline bool loaded(const State &s, uint32_t t) { return t < 128 && ((s.loaded[t >> 6] >> (t & 63)) & 1u); }

// The TMR size SETUP_TMR carries: the LOAD_TOC answer rounded up to 4 KiB, as Linux's allocator does
// (amdgpu_object.c:659). 0 means refuse (no answer, or above 32 MiB: a new placement is designed first).
inline uint64_t tmr_bytes(uint32_t answer) {
	const uint64_t b = ((uint64_t)answer + kPage - 1) & ~(kPage - 1);
	return (b == 0 || b > kTmrMax) ? 0 : b;
}

// Is [off, off+size) a valid image inside [base, base+len), page aligned at its start?
inline bool in_buf(uint64_t off, uint64_t size, uint64_t base, uint64_t len) {
	return size != 0 && (off & (kPage - 1)) == 0 && off >= base && off - base <= len && size <= len - (off - base);
}

// May this command be sent now? D3. The TMR overlap with live DMCUB windows is the caller's (it reads
// the hardware).
inline uint32_t may_send(const State &s, uint32_t cmd, uint32_t fwType, uint64_t off, uint64_t size) {
	if (s.failed) return kWhyFailed;
	if (!s.ring) return kWhyNoRing;
	switch (cmd) {
	case kToc:
		if (s.toc) return kWhyOrder;
		if (size == 0 || size > kFwPriSize || (size & 3)) return kWhyRange;
		return kWhyOk;
	case kIpFw:
		if (fwType == kFwSmu) {
			if (!s.toc || s.smu || s.tmr) return kWhyOrder;
		} else if (gc_type(fwType)) {
			if (!s.tmr || loaded(s, fwType) || s.autoload) return kWhyOrder;
		} else {
			return kWhyType;
		}
		if (!in_buf(off, size, kFwBuf, kFwBufSize)) return kWhyRange;
		return kWhyOk;
	case kTmr:
		if (!s.toc || !s.smu || s.tmr) return kWhyOrder;
		if (tmr_bytes(s.tmrAnswer) == 0) return kWhyTmrSize;
		return kWhyOk;
	case kAutoload:
		if (!s.rlcG || s.autoload) return kWhyOrder;
		return kWhyOk;
	default:
		return kWhyType;
	}
}

// After a submitted command: a timeout or a non-zero status latches failure (D5, no retries).
inline void record(State &s, uint32_t cmd, uint32_t fwType, bool fenced, uint32_t status, uint32_t tmrAnswer) {
	if (!fenced || status != 0) { s.failed = true; return; }
	switch (cmd) {
	case kToc: s.toc = true; s.tmrAnswer = tmrAnswer; break;
	case kTmr: s.tmr = true; break;
	case kAutoload: s.autoload = true; break;
	case kIpFw:
		if (fwType == kFwSmu) s.smu = true;
		if (fwType == kFwRlcG) s.rlcG = true;
		if (fwType < 128) s.loaded[fwType >> 6] |= 1ull << (fwType & 63);
		break;
	}
}

// psp_ring_cmd_submit's slot arithmetic (amdgpu_psp.c:3938-3941): the frame index for a write pointer,
// or -1 when it is not a frame boundary inside the ring.
inline int32_t frame_slot(uint32_t wptr) {
	if (wptr % kRingDw == 0) return 0;
	if (wptr % kFrameDw != 0 || wptr / kFrameDw >= kRingDw / kFrameDw) return -1;
	return (int32_t)(wptr / kFrameDw);
}
inline uint32_t next_wptr(uint32_t wptr) { return (wptr + kFrameDw) % kRingDw; }

// The 64-byte frame: only the command and fence addresses and the fence value (amdgpu_psp.c:3953-3960).
inline void build_frame(uint32_t f[kFrameDw], uint64_t cmdMc, uint64_t fenceMc, uint32_t fenceValue) {
	for (uint32_t i = 0; i < kFrameDw; i++) f[i] = 0;
	f[0] = (uint32_t)cmdMc; f[1] = (uint32_t)(cmdMc >> 32);
	f[3] = (uint32_t)fenceMc; f[4] = (uint32_t)(fenceMc >> 32);
	f[5] = fenceValue;
}

// The 1 KiB command. buf_size and buf_version stay 0, as Linux leaves them; the body starts at +28 (dword 7).
// kToc: mc = fw_pri, size = TOC bytes. kIpFw: mc = image, size, fwType. kTmr: mc = TMR MC, size = TMR
// bytes, pa = TMR physical (VRAM offset). kAutoload: no body.
inline void build_cmd(uint32_t c[kCmdDw], uint32_t cmd, uint32_t fwType, uint64_t mc, uint64_t size, uint64_t pa) {
	for (uint32_t i = 0; i < kCmdDw; i++) c[i] = 0;
	uint32_t *b = c + 7;
	switch (cmd) {
	case kToc:
		c[2] = kCmdLoadToc;
		b[0] = (uint32_t)mc; b[1] = (uint32_t)(mc >> 32); b[2] = (uint32_t)size;
		break;
	case kIpFw:
		c[2] = kCmdLoadIpFw;
		b[0] = (uint32_t)mc; b[1] = (uint32_t)(mc >> 32); b[2] = (uint32_t)size; b[3] = fwType;
		break;
	case kTmr:
		c[2] = kCmdSetupTmr;
		b[0] = (uint32_t)mc; b[1] = (uint32_t)(mc >> 32); b[2] = (uint32_t)size; b[3] = kTmrFlagsVirtPhys;
		b[4] = (uint32_t)pa; b[5] = (uint32_t)(pa >> 32);
		break;
	case kAutoload:
		c[2] = kCmdAutoloadRlc;
		break;
	}
}

} // namespace n33psp2
