// n33_dmub_pure.h: decisions for the stage 3 DMCUB bring-up proof on Navi 33 (DCN 3.2.1): re-init DMCUB from reset
// with the open dcn_3_2_1_dmcub.bin (7.0.53.0), loaded through the PSP as fw_type 51 after RLC autoload, then one
// DMUB_CMD__QUERY_FEATURE_CAPS through inbox1 with the reply read back. Mailbox proof only (William, 2026-10-18); no
// transmitter/PHY command, no modeset. Host-testable: no kernel types, no I/O. The kext reads the live registers, asks
// these functions for every decision and value, and does the register and VRAM writes itself.
// Design: the overview repo, RDNA3/Tasks/navi33-cobalt64-port/stage3-dmcub-proof-design.md and
// stage3-dmcub-design-pass.md. Linux references are to the pinned tree 3857c2fe, paths under
// drivers/gpu/drm/amd/display/dmub/ unless noted. No Apple code.
#pragma once
#include <stdint.h>

namespace n33dmub {

// ---- DMU registers (DCN 3.2.1 segment 2 BASE_IDX 2 = 0x34C0 on this card; offsets from dcn_3_2_1_offset.h) ----------
// Absolute dwords, as the rest of the N33 kext stores them (kN33CwBase 0x3665 = 0x34C0 + 0x1a5). The offset form is
// kept beside each so the host test can check it against Linux's dcn_3_2_1_offset.h.
constexpr uint32_t kDmuBase = 0x34C0;
constexpr uint32_t kOffSecCntl = 0x1ce, kOffCntl = 0x1f6, kOffCntl2 = 0x200;
constexpr uint32_t kOffGpintIn1 = 0x1f8, kOffGpintOut = 0x1f9;
constexpr uint32_t kOffMmhubbubSoftReset = 0x342, kOffCcDcPipeDis = 0x0ca;
constexpr uint32_t kOffScratch0 = 0x1e3;                       // SCRATCH0..15 run 0x1e3..0x1f2
constexpr uint32_t kOffScratch14 = 0x1f1, kOffScratch15 = 0x1f2, kOffScratch7 = 0x1ea;
constexpr uint32_t kOffInbox1Base = 0x1d4, kOffInbox1Size = 0x1d5, kOffInbox1Wptr = 0x1d6, kOffInbox1Rptr = 0x1d7;
// REGION3_CW0..7 base/top/offset triplets, REGION4..6, as the kext already names them.
constexpr uint32_t kOffCwBase = 0x1a5, kOffCwTop = 0x1ad, kOffCwOffset = 0x1b5;   // each 8 regs (CW0..7)
constexpr uint32_t kOffRegionOffset = 0x196, kOffRegionTop = 0x1a1;               // REGION4..6 (kN33RegionOffset 0x3656 = base+0x196)
inline constexpr uint32_t reg(uint32_t off) { return kDmuBase + off; }

constexpr uint32_t kSecCntl = reg(kOffSecCntl), kCntl = reg(kOffCntl), kCntl2 = reg(kOffCntl2);
constexpr uint32_t kGpintIn1 = reg(kOffGpintIn1), kGpintOut = reg(kOffGpintOut);
constexpr uint32_t kMmhubbubSoftReset = reg(kOffMmhubbubSoftReset);
constexpr uint32_t kScratch0 = reg(kOffScratch0), kScratch7 = reg(kOffScratch7),
                   kScratch14 = reg(kOffScratch14), kScratch15 = reg(kOffScratch15);
constexpr uint32_t kInbox1Base = reg(kOffInbox1Base), kInbox1Size = reg(kOffInbox1Size),
                   kInbox1Wptr = reg(kOffInbox1Wptr), kInbox1Rptr = reg(kOffInbox1Rptr);
inline constexpr uint32_t scratch(uint32_t i) { return reg(kOffScratch0 + i); }

// ---- DMCUB_CNTL / SCRATCH0 fields (dcn_3_2_1_sh_mask.h; dmub_dcn32.c:126-163, :335-344) ------------------------------
constexpr uint32_t kCntlEnable = 1u << 16;        // DMCUB_CNTL.DMCUB_ENABLE
constexpr uint32_t kCntlSoftReset = 1u << 0;      // DMCUB_CNTL2.DMCUB_SOFT_RESET (dmub_dcn32.c uses CNTL2 soft reset)
constexpr uint32_t kCntlTraceportEn = 1u << 20;   // DMCUB_CNTL.DMCUB_TRACEPORT_EN
constexpr uint32_t kPwaitModeStatus = 1u << 20;   // DMCUB_CNTL.DMCUB_PWAIT_MODE_STATUS (baseline 0x001900c6)
// union dmub_fw_boot_status (dmub/inc/dmub_cmd.h:1084-1108): dal_fw bit 0, mailbox_rdy bit 1, read from SCRATCH0.
constexpr uint32_t kBootDalFw = 1u << 0, kBootMailboxRdy = 1u << 1;
inline bool dal_fw(uint32_t scratch0) { return scratch0 & kBootDalFw; }
inline bool mailbox_rdy(uint32_t scratch0) { return scratch0 & kBootMailboxRdy; }
// is_hw_init (dmub_dcn32.c:335-344): DMCUB_ENABLE != 0 and dal_fw set. We read the registers directly (the library
// wrapper dmub_srv_is_hw_init also needs a software flag, dmub_srv.c:786-800; not relevant to a register read).
inline bool is_hw_init(uint32_t cntl, uint32_t scratch0) { return (cntl & kCntlEnable) && dal_fw(scratch0); }
// auto-load complete (dmub_srv_wait_for_auto_load, dmub_srv.c:1075-1098): dal_fw AND mailbox_rdy.
inline bool auto_loaded(uint32_t scratch0) { return dal_fw(scratch0) && mailbox_rdy(scratch0); }
// reset's STOP_FW handshake (dmub_dcn32.c:102-124): GPINT STOP_FW, poll DMCUB_SCRATCH7 for 0xDEADDEAD.
constexpr uint32_t kDeadDead = 0xDEADDEADu;
constexpr uint32_t kGpintStopFw = 2;              // DMUB_GPINT__STOP_FW (dmub/inc/dmub_cmd.h:1503)
// GPINT_DATAIN1 layout: param 0-15, command_code 16-27, status 28-31 (dmub_cmd.h:1474-1481); status 1 = new command.
constexpr uint32_t gpint(uint32_t cmd, uint32_t param = 0) { return (param & 0xFFFFu) | ((cmd & 0xFFFu) << 16) | (1u << 28); }
// DMCUB-side (virtual) window bases: BASE/TOP registers take these, only OFFSET takes the VRAM address
// (dmub_srv.c:83-88, :876-911; dmub_dcn32.c setup_windows).
constexpr uint32_t kCw3Base = 0x63000000, kCw4Base = 0x64000000, kCw5Base = 0x65000000, kCw6Base = 0x66000000;
constexpr uint32_t kRegion5Offset = reg(0x198), kRegion5OffsetHigh = reg(0x199), kRegion5Top = reg(0x1a2);   // dcn_3_2_1_offset.h:476-496

// ---- Firmware blob (dcn_3_2_1_dmcub.bin, measured 2026-10-18; rules from amdgpu_ucode.h, dmub_srv.c) ----------------
// common_firmware_header (32 bytes): ucode_version @0x10, ucode_size_bytes @0x14, ucode_array_offset_bytes @0x18.
// dmcub_firmware_header_v1_0 adds inst_const_bytes @0x20, bss_data_bytes @0x24 (measured against the real file).
constexpr uint32_t kFwVersion = 0x07003500u;      // 7.0.53.0 (this file)
constexpr uint32_t kFwFileSize = 232608;          // 0x38ca0
constexpr uint32_t kUcodeArrayOffset = 256;       // 0x100
constexpr uint32_t kInstConstBytes = 232352;      // 0x38ba0 (== ucode_size for DMCUB)
constexpr uint32_t kBssDataBytes = 0;
constexpr uint32_t kPspHeader = 0x100, kPspFooter = 0x100;   // dmub_srv.h:76-77
constexpr uint32_t kFwMetaMagic = 0x444D5542u;    // dmub_cmd.h:994 ("DMUB")
constexpr uint32_t kFwMetaSize = 64;              // sizeof(union dmub_fw_meta)
// fw_inst_size = inst_const_bytes - PSP header - PSP footer (amdgpu_dm_dmub.c:615-616).
constexpr uint32_t kFwInstSize = kInstConstBytes - kPspHeader - kPspFooter;   // 231840 = 0x389a0
// The meta of this file (measured by fwparse.c against the real file): the host test reparses and compares.
constexpr uint32_t kMetaFwRegionSize = 0xb8c0, kMetaTraceSize = 0x10010, kMetaSharedSize = 0;

struct FwHeader { uint32_t ucodeVersion, ucodeSize, ucodeArrayOffset, instConstBytes, bssDataBytes; };
inline bool parse_header(const uint8_t *b, uint32_t size, FwHeader *h) {
	if (!b || size < 0x30) return false;
	auto rd = [&](uint32_t o) -> uint32_t { return (uint32_t)b[o] | ((uint32_t)b[o+1] << 8) | ((uint32_t)b[o+2] << 16) | ((uint32_t)b[o+3] << 24); };
	h->ucodeVersion = rd(0x10); h->ucodeSize = rd(0x14); h->ucodeArrayOffset = rd(0x18);
	h->instConstBytes = rd(0x20); h->bssDataBytes = rd(0x24);
	return true;
}
struct FwMeta { uint32_t fwRegionSize, traceSize, fwVersion, sharedSize; uint8_t dalFw; };
// Linux's combined-metadata scan (dmub_srv.c:156-162): the inst-const blob is [ucode_array_offset + PSP_HEADER,
// + inst_const_size), inst_const_size = inst_const_bytes - PSP_HEADER - PSP_FOOTER; the meta sits at
// blob_end - i - 64 for the first i in 0..15 whose magic matches.
inline bool find_meta(const uint8_t *b, uint32_t size, const FwHeader &h, FwMeta *m) {
	if (!b) return false;
	const uint32_t blobStart = h.ucodeArrayOffset + kPspHeader;
	if ((uint64_t)h.instConstBytes < (uint64_t)kPspHeader + kPspFooter) return false;
	const uint32_t blobSize = h.instConstBytes - kPspHeader - kPspFooter;
	for (uint32_t i = 0; i < 16; i++) {
		const uint64_t off = (uint64_t)blobStart + blobSize - i - kFwMetaSize;
		if (off + kFwMetaSize > size) continue;
		auto rd = [&](uint32_t o) -> uint32_t { return (uint32_t)b[off+o] | ((uint32_t)b[off+o+1] << 8) | ((uint32_t)b[off+o+2] << 16) | ((uint32_t)b[off+o+3] << 24); };
		if (rd(0) != kFwMetaMagic) continue;
		m->fwRegionSize = rd(4); m->traceSize = rd(8); m->fwVersion = rd(12); m->dalFw = b[off+16]; m->sharedSize = rd(20);
		return true;
	}
	return false;
}

// The PSP type-51 image is the inst-const region only: inst_const_bytes from ucode_array_offset_bytes, the header and
// any bss left out (amdgpu_ucode.c:1008-1011). Linux copies it to a page-aligned mc_addr in adev->firmware.fw_buf
// (amdgpu_ucode.c:1119, :1214) and sends fw_size = ucode_size unpadded (amdgpu_psp.c:3379-3384). The PSP header and
// footer stay inside the image; the PSP strips them and programs CW0 itself, so the driver neither copies inst_const
// nor calls backdoor_load (amdgpu_dm_dmub.c:204-211, :241-242; dmub_srv.c:857).
// Common header (amdgpu_ucode.h): size_bytes @0, header_size_bytes @4, header_version_major @8 (u16).
constexpr uint32_t kDmcubHdrBytes = 0x28;          // sizeof(struct dmcub_firmware_header_v1_0)
struct FwSlice { uint32_t off, size; };
// hdr: at least kDmcubHdrBytes of the file's start; fileSize: the whole file. False on any field out of bounds.
inline bool fw_slice(const uint8_t *hdr, uint32_t avail, uint32_t fileSize, FwSlice *s) {
	if (!hdr || !s || avail < kDmcubHdrBytes || fileSize < kDmcubHdrBytes) return false;
	auto rd = [&](uint32_t o) -> uint32_t { return (uint32_t)hdr[o] | ((uint32_t)hdr[o+1] << 8) | ((uint32_t)hdr[o+2] << 16) | ((uint32_t)hdr[o+3] << 24); };
	const uint32_t hdrSize = rd(4), major = rd(8) & 0xFFFFu, off = rd(0x18), inst = rd(0x20), bss = rd(0x24);
	if (rd(0) != fileSize || major != 1 || hdrSize < kDmcubHdrBytes || hdrSize > fileSize) return false;
	if (off < hdrSize || inst < kPspHeader + kPspFooter) return false;   // Linux subtracts both from inst_const_bytes
	if ((uint64_t)off + inst + bss > fileSize) return false;
	s->off = off; s->size = inst;
	return true;
}
// The kext loads only this file's layout: the region math uses its measured meta (kMetaFwRegionSize, kMetaTraceSize).
inline bool fw_slice_pinned(const FwSlice &s, uint32_t fileSize) {
	return fileSize == kFwFileSize && s.off == kUcodeArrayOffset && s.size == kInstConstBytes;
}

// ---- DMUB window layout (dmub_srv_calc_region_info, dmub_srv.c:659-736; sizes dmub_srv.c:57-75) --------------------
// William chose VRAM for every window (one FB block), above the 16 MiB floor, in a new owned range. So we lay all the
// windows out contiguously exactly as dmub_srv_calc_regions_for_memory_type does for one memory type: each base aligned
// to 256, each size aligned to 64, the total aligned to 4096.
enum Window : uint32_t {
	kWinInstConst = 0, kWinStack, kWinBssData, kWinVbios, kWinMailbox, kWinTracebuff, kWinFwState,
	kWinScratchMem, kWinIbMem, kWinSharedState, kWinLsdma, kWinCursorOffload, kWinTotal
};
constexpr uint32_t kStackSize = 128u << 10, kContextSize = 512u << 10;   // dmub_srv.c:57,:60
constexpr uint32_t kRbSize = 64 * 128;                 // DMUB_RB_SIZE = DMUB_RB_CMD_SIZE(64) * DMUB_RB_MAX_ENTRY(128) = 0x2000
constexpr uint32_t kMailboxSize = 2 * kRbSize;         // 0x4000; inbox1 = first 0x2000, outbox1 = next 0x2000
constexpr uint32_t kScratchMemSize = 1024, kLsdmaSize = 64u << 10;
constexpr uint32_t kFwStateDefault = 64u << 10, kTraceDefault = 64u << 10;
// IB_MEM = sizeof(struct dmub_fams2_config_v2), SHARED_STATE and CURSOR_OFFLOAD are struct sizes that vary by tree.
// We do not program those windows for a QUERY_FEATURE_CAPS proof; reserve a conservative slab so the total still bounds
// the owned range. The exact struct sizes are only needed if a later step drives FAMS2 / cursor offload (unverified).
constexpr uint32_t kIbMemReserve = 0x1000, kSharedStateReserve = 0x1000, kCursorReserve = 0x1000;
constexpr uint32_t kSlabRound4k = 0x1000;
inline constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

struct Region { uint32_t base, top; };
struct RegionInfo { Region w[kWinTotal]; uint32_t total; };
// vbiosSize: the card's expansion-ROM image size (read at run time). fwRegionSize/traceSize: from the firmware meta.
inline RegionInfo region_info(uint32_t vbiosSize, uint32_t fwRegionSize, uint32_t traceSize) {
	uint32_t sizes[kWinTotal] = {0};
	sizes[kWinInstConst]    = kFwInstSize;
	sizes[kWinStack]        = kStackSize + kContextSize;
	sizes[kWinBssData]      = kBssDataBytes;
	sizes[kWinVbios]        = vbiosSize;
	sizes[kWinMailbox]      = kMailboxSize;
	sizes[kWinTracebuff]    = traceSize ? traceSize : kTraceDefault;
	sizes[kWinFwState]      = fwRegionSize ? fwRegionSize : kFwStateDefault;
	sizes[kWinScratchMem]   = align_up(kScratchMemSize, 64);
	sizes[kWinIbMem]        = kIbMemReserve;
	sizes[kWinSharedState]  = kSharedStateReserve;
	sizes[kWinLsdma]        = kLsdmaSize;
	sizes[kWinCursorOffload]= kCursorReserve;
	RegionInfo r{};
	uint32_t top = 0;
	for (uint32_t i = 0; i < kWinTotal; i++) {
		r.w[i].base = align_up(top, 256);
		r.w[i].top  = r.w[i].base + align_up(sizes[i], 64);
		top = r.w[i].top;
	}
	r.total = align_up(top, kSlabRound4k);
	return r;
}

// ---- Placement: a new owned VRAM range above the 16 MiB floor, clear of every range the snapshot lists -------------
// Snapshot 2026-10-18 05:37 (11 ranges): GOP 0..0x300000; fb surface at 0x2000000 (nub, when published); PSP/SMU/GART/
// step6/step7 0x8100000..0x8a0a000; the VBIOS DMCUB windows near the top of VRAM 0x83fe...  0x4000000 (64 MiB) is clear
// of all of them and leaves room up to the PSP region at 0x8100000 for the ~1 MiB DMUB block. The kext still runs the
// live protected-range overlap check (code 209) before owning it, so a future layout change cannot silently clash.
constexpr uint64_t kWriteFloor = 16ull << 20;          // nothing below 16 MiB is ever written (as kN33WriteFloor)
constexpr uint64_t kDmubOff = 0x4000000;               // 64 MiB
constexpr uint64_t kDmubMax = 0x200000;                // 2 MiB slab cap; region_info total must fit
inline bool placement_ok(const RegionInfo &r) { return kDmubOff >= kWriteFloor && r.total <= kDmubMax; }
// Mailbox / inbox1 / outbox1 MC addresses (fbBase + offset). inbox1 is the first DMUB_RB_SIZE of the mailbox window,
// outbox1 the next (dmub_srv.c:891-894).
inline uint64_t mailbox_mc(uint64_t fbBase, const RegionInfo &r) { return fbBase + kDmubOff + r.w[kWinMailbox].base; }
inline uint64_t inbox1_mc(uint64_t fbBase, const RegionInfo &r)  { return mailbox_mc(fbBase, r); }
inline uint64_t outbox1_mc(uint64_t fbBase, const RegionInfo &r) { return mailbox_mc(fbBase, r) + kRbSize; }
inline uint64_t vbios_mc(uint64_t fbBase, const RegionInfo &r)   { return fbBase + kDmubOff + r.w[kWinVbios].base; }

// ---- The inbox1 command ring (DMUB_CMD_INTERFACE_FB; dmub_cmd.h:1818-1828, dc_dmub_srv.c) --------------------------
// One 64-byte slot = 16 dwords. Ring base is the inbox1 window; WPTR/RPTR are byte offsets. Push: write the slot at
// base + WPTR, bump DMCUB_INBOX1_WPTR, poll DMCUB_INBOX1_RPTR until it reaches the new WPTR (dmub_srv.c:1157-1181).
constexpr uint32_t kSlotBytes = 64, kSlotDwords = 16;
constexpr uint32_t kInbox1RingSize = kRbSize;          // 0x2000, matching the snapshot's INBOX1_SIZE
// Header dword layout (struct dmub_cmd_header, dmub_cmd.h:2069-2078): type bits 0..7, sub_type 8..15, ret_status bit 16,
// multi_cmd_pending 17, is_reg_based 18, reserved 19..23, payload_bytes bits 24..29 (6 bits). QUERY_FEATURE_CAPS:
// type 6 (dmub_cmd.h), sub 0, ret_status 1, payload 16 -> 0x10010006 (dc_dmub_srv.c:524-527).
constexpr uint32_t kCmdQueryFeatureCaps = 6;
constexpr uint32_t kQfcPayloadBytes = 16;              // sizeof(struct dmub_cmd_query_feature_caps_data) (dmub_cmd.h:819-832)
inline constexpr uint32_t cmd_header(uint32_t type, uint32_t sub, uint32_t retStatus, uint32_t payloadBytes) {
	return (type & 0xFF) | ((sub & 0xFF) << 8) | ((retStatus & 1u) << 16) | ((payloadBytes & 0x3Fu) << 24);
}
constexpr uint32_t kHdrQueryFeatureCaps = cmd_header(kCmdQueryFeatureCaps, 0, 1, kQfcPayloadBytes);   // 0x10010006

// Allowlisted command templates. Only T0 (QUERY_FEATURE_CAPS) is permitted for the mailbox proof; the transmitter
// DISABLE/ENABLE templates are a separate later step with its own AUX training (design pass sec 2.5), not built here.
enum Template : uint32_t { kTplQueryFeatureCaps = 0 };
inline bool template_ok(uint32_t t) { return t == kTplQueryFeatureCaps; }
inline uint32_t template_header(uint32_t t) { return t == kTplQueryFeatureCaps ? kHdrQueryFeatureCaps : 0; }
// Zero the slot, write only the header (the QUERY_FEATURE_CAPS payload is read back from the reply, not sent).
inline void build_slot(uint32_t slot[kSlotDwords], uint32_t header) {
	for (uint32_t i = 0; i < kSlotDwords; i++) slot[i] = 0;
	slot[0] = header;
}
inline uint32_t next_wptr(uint32_t wptr) { return (wptr + kSlotBytes) % kInbox1RingSize; }
// Ring is usable only when idle (wptr == rptr), the size is what we programmed, and the next slot does not wrap the
// ring end (no wrap handling, as the Navi 48 build decided, navi48_dmubcmd.h:144-146).
inline bool ring_usable(uint32_t size, uint32_t wptr, uint32_t rptr) {
	return size == kInbox1RingSize && wptr == rptr && (wptr % kSlotBytes) == 0 &&
	       wptr + kSlotBytes < size;
}
// The reply slot sits at the new RPTR minus one slot (dmub_srv.c:1423-1435). The firmware clears ret_status to 0 in the
// reply header on success (sent with ret_status 1; dc_dmub_srv.c:530-531). So reply_ok = reply_ret_status == 0.
inline uint32_t reply_ret_status(uint32_t replyHeader) { return (replyHeader >> 16) & 1u; }

// ---- Poll bounds (Linux: 100 ms auto-load, amdgpu_dm_dmub.c:280; 1 s each STOP_FW / PWAIT, dmub_dcn32.c) -----------
constexpr uint32_t kAutoLoadPollUs = 100000, kStopFwPollUs = 1000000, kCmdPollUs = 100000;
constexpr uint32_t kPollStepUs = 100;

// ---- Protected-range kind (added to n33_protected): 18 our DMUB windows (19 is the IH ring, n33_ih_pure.h) ----------
constexpr uint32_t kKindOurWindows = 18;

// ---- Refusal reasons (design-pass sec 3.5; 200..211). why is carried as a 64-bit scalar, never truncated ----------
enum Why : uint32_t {
	kWhyOk = 0,
	kWhyNotSnapped = 200,   // a DMCUB write before action 60 armed the gate
	kWhyHubpNotGop = 201,   // HUBP0 is not on the GOP scanout surface (the gate's "display is as the snapshot left it")
	kWhyNubPublished = 202, // the N33 framebuffer nub is published (WindowServer owns a scanout)
	kWhyCpRunning = 203,    // the step-6/7 compute queue is running
	kWhyOrder = 204,        // out of order: load before snapshot/autoload, reset before load, init before reset, etc.
	kWhyStopFwNoAck = 205,  // STOP_FW not acked in 1 s (William chose proceed-as-Linux; this is logged, see may_reset)
	kWhyAutoLoadTimeout = 206, // DMCUB did not reach dal_fw + mailbox_rdy in 100 ms
	kWhyRing = 207,         // inbox1 ring busy, wrong size, or the next slot would wrap
	kWhyTemplate = 208,     // command template not on the allowlist (only QUERY_FEATURE_CAPS now)
	kWhyWindowOverlap = 209,// a DMUB window overlaps a live protected range
	kWhyVbios = 210,        // VBIOS image missing or wrong size
	kWhyPspType51 = 211,    // the PSP refused the type-51 DMUB load at this point
	kWhyFwHeader = 212,     // the staged firmware's header is malformed, out of bounds, or not the pinned layout
};

// ---- Gate state. The kext owns one of these per load; it reads the hardware and fills the bool arguments. -----------
struct State {
	bool snapped;       // action 60 ran: the gate is armed and the before-state saved
	bool dmubLoaded;    // action 62: the PSP accepted the type-51 DMUB load this load
	bool reset;         // action 61: dmub_dcn32_reset ran
	bool inited;        // action 63: windows/mailbox programmed and reset_release done
	bool autoLoaded;    // action 63's poll saw dal_fw + mailbox_rdy
	bool proofDone;     // action 64: QUERY_FEATURE_CAPS got a reply
	uint32_t vbiosSize; // the expansion-ROM image size the snapshot read (0 if not read yet)
};

// Preconditions the kext reads live and passes in (like n33cp::may_start's gcUp/c1/g6).
inline uint32_t may_reset(const State &s, bool hubpOnGop, bool nubPublished, bool cpRunning) {
	if (!s.snapped) return kWhyNotSnapped;
	if (!s.dmubLoaded) return kWhyOrder;          // PSP load, then reset, then init (William's order)
	if (!hubpOnGop) return kWhyHubpNotGop;
	if (nubPublished) return kWhyNubPublished;
	if (cpRunning) return kWhyCpRunning;
	return kWhyOk;
}
// autoload true = the existing milestone-2 RLC autoload is done (type 51 needs it); dmubRefused = the PSP said no.
inline uint32_t may_load(const State &s, bool autoload, bool dmubRefused) {
	if (!s.snapped) return kWhyNotSnapped;
	if (!autoload) return kWhyOrder;
	if (s.dmubLoaded) return kWhyOrder;           // once
	if (dmubRefused) return kWhyPspType51;
	return kWhyOk;
}
// init needs the reset done, the region to fit and own no protected range (windowsClear read live), and the VBIOS image.
inline uint32_t may_init(const State &s, const RegionInfo &r, bool windowsClear, bool vbiosOk) {
	if (!s.snapped) return kWhyNotSnapped;
	if (!s.reset) return kWhyOrder;
	if (!placement_ok(r)) return kWhyWindowOverlap;
	if (!windowsClear) return kWhyWindowOverlap;
	if (!vbiosOk) return kWhyVbios;
	return kWhyOk;
}
// cmd (QUERY_FEATURE_CAPS) needs init done, auto-load seen, the template allowlisted, and an idle, well-formed ring.
inline uint32_t may_cmd(const State &s, uint32_t tpl, uint32_t ringSize, uint32_t wptr, uint32_t rptr) {
	if (!s.inited) return kWhyOrder;
	if (!s.autoLoaded) return kWhyAutoLoadTimeout;
	if (!template_ok(tpl)) return kWhyTemplate;
	if (!ring_usable(ringSize, wptr, rptr)) return kWhyRing;
	return kWhyOk;
}
inline uint32_t may_check(const State &s) { return s.snapped ? kWhyOk : kWhyNotSnapped; }

inline void record_snap(State &s, uint32_t vbiosSize) { s.snapped = true; s.vbiosSize = vbiosSize; }
inline void record_load(State &s, bool ok) { if (ok) s.dmubLoaded = true; }
inline void record_reset(State &s) { s.reset = true; }
inline void record_init(State &s, bool autoLoaded) { s.inited = true; s.autoLoaded = autoLoaded; }
inline void record_proof(State &s, bool replyOk) { if (replyOk) s.proofDone = true; }

} // namespace n33dmub
