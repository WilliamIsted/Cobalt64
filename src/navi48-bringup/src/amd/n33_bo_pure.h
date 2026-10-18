// n33_bo_pure.h: decisions for stage 3 step 7 W2 on Navi 33: buffer objects (VRAM, GTT, host import), their GPU VAs in
// VMID 0's flat GART and the gfx11 PTE, their CPU maps, and the two W2 test streams (a COPY_DATA host -> VRAM copy and a
// deliberate fault), per the overview repo's RDNA3/Tasks/navi33-cobalt64-port/stage3-step7-w2-design.md.
// Host-testable: no kernel types, no I/O. The kext owns the memory and the PTE writes; every check, address, PTE and
// packet comes from here. It ports the gfx12 n1c_bo_create / n1c_gem_va / n1c_bo_import_host (native_s1c.cpp) to VMID 0
// on gfx11: no page directory (the GART is one flat level), so a VA is a GART slot.
// Linux references are to the pinned tree 3857c2fe (amdgpu_vm.h, gmc_v11_0.c, nvd.h, gc_11_0_0_sh_mask.h, amdgpu_drm.h).
#pragma once
#include <stdint.h>
#include "n33_cp_pure.h"

namespace n33bo {

using n33gmc::kPage;

// ---- Placement (design "Placement") -------------------------------------------------------------------------------
constexpr uint64_t kSlabOff = 0xC000000, kSlabLen = 16ull << 20;   // VRAM BO slab, inside BAR0; protected kind 20
constexpr uint32_t kKind = 20;
constexpr uint32_t kVaSlot0 = 0x1000, kVaSlots = 0x1000;           // BO VA window: GART slots 4096..8191 (16 MiB)
constexpr uint64_t kVaBase = n33gmc::kGartMc + (uint64_t)kVaSlot0 * kPage;   // 0x8401000000
constexpr uint64_t kVaEnd = kVaBase + (uint64_t)kVaSlots * kPage;              // 0x8402000000
static_assert(kVaSlot0 + kVaSlots <= n33gmc::kGartPages, "window inside the GART");
static_assert(kSlabOff >= n33cp::kIbOff + n33cp::kIbLen && kSlabOff + kSlabLen <= 0x10000000ull, "slab above the owned pages, inside BAR0");

constexpr uint32_t kBoMax = 64;                      // handles 1..64 (W3: RADV's device + one dispatch peaks at 11 live BOs; 64 is headroom. Raising it only grows the handle arrays, no VRAM-layout change)
constexpr uint64_t kBoMaxBytes = 1ull << 20;         // 256 pages per BO (W3's measured max single BO is exactly 1 MiB; a larger single BO needs the slab resized, which moves the W2V arena offset and needs a hardware re-test)
constexpr uint32_t kBoMaxPages = (uint32_t)(kBoMaxBytes / kPage);
constexpr uint64_t kHostCap = 4ull << 20;            // GTT + import, all BOs
constexpr uint64_t kUserMax = 1ull << 47;            // an import must end in the x86-64 user half

enum Kind : uint32_t { kVram = 0, kGtt = 1, kImport = 2 };

// ---- The gfx11 PTE (gmc_v11_0.c:414-436; amdgpu_vm.h:57-118). n33gmc's constants are the proven bits. --------------
// The caller's flags are the uapi AMDGPU_VM_PAGE_* (amdgpu_drm.h:882-886); MTYPE is UC for every BO (the GART's default,
// gmc_v11_0.c:734). shortcut: one MTYPE; per-BO MTYPE (gmc_v11_0_get_vm_pte's NC default) comes with per-process VMIDs.
constexpr uint32_t kVmRead = 1u << 1, kVmWrite = 1u << 2, kVmExec = 1u << 3;
constexpr uint32_t kVmMask = kVmRead | kVmWrite | kVmExec, kVmDefault = kVmMask;
inline bool vm_flags_ok(uint32_t f) { return !(f & ~kVmMask) && (f & (kVmRead | kVmWrite)); }
inline uint64_t pte(uint64_t pa, bool system, uint32_t vmFlags) {
	using namespace n33gmc;
	uint64_t f = kPteValid | kPteMtypeUc;
	if (system) f |= kPteSystem | kPteSnooped;
	if (vmFlags & kVmRead) f |= kPteRead;
	if (vmFlags & kVmWrite) f |= kPteWrite;
	if (vmFlags & kVmExec) f |= kPteExec;
	return (pa & kPteAddrMask) | f;
}

// ---- Refusal reasons (66 to 74; 62, 56, 50, 57, 61 come from W1's submit gate) ---------------------------------------
enum Why : uint32_t {
	kWhyOk = 0,
	kWhyArg = 66,      // kind, size, handle, flags, VA alignment or dword index
	kWhyOrder = 67,    // G6 not done / GMC stopped; map twice, unmap or copy unmapped, free mapped; a submit outstanding
	kWhyFull = 68,     // no free handle, no room in the slab, host total over the cap
	kWhyAlloc = 69,    // host allocation, wiring or kernel mapping failed, or a page at or above 44 bits
	kWhyVa = 70,       // VA outside the window or overlapping a mapped BO; a fault VA that a BO maps
	kWhyDevice = 71,   // import: a page inside a PCI BAR of this GPU, or no BAR list
	kWhyFlush = 72,    // a hub flush after a map or unmap did not acknowledge (the GMC is stopped)
	kWhyWindow = 73,   // gart map / gart unmap (17/18) into the BO VA window
	kWhySlab = 74,     // the slab overlaps a live protected range when first taken
};

// ---- State --------------------------------------------------------------------------------------------------------
struct Bo {
	bool live, mapped;
	uint32_t kind, pages, vmFlags;
	uint64_t vramOff;    // kVram: offset inside the slab
	uint64_t va;         // when mapped
};
struct State {
	Bo bo[kBoMax];
	bool slabOwned;      // kind 20 live from the first VRAM BO until unload
	uint64_t hostBytes;  // GTT + import bytes live
	uint32_t copySrc, copyDst;   // the outstanding bo copy (handles)
	uint64_t faultVa;            // the outstanding bo fault
};
inline bool handle_ok(const State &s, uint32_t h) { return h >= 1 && h <= kBoMax && s.bo[h - 1].live; }
inline uint64_t bytes(const Bo &b) { return (uint64_t)b.pages * kPage; }

// gmcReady: GMC table owned, G6 done (both hubs live) and the GMC not stopped.
inline uint32_t may_create(const State &s, uint32_t kind, uint64_t size, bool gmcReady) {
	if ((kind != kVram && kind != kGtt) || size == 0 || (size & (kPage - 1)) || size > kBoMaxBytes) return kWhyArg;
	if (!gmcReady) return kWhyOrder;
	uint32_t i = 0;
	while (i < kBoMax && s.bo[i].live) i++;
	if (i == kBoMax) return kWhyFull;
	if (kind == kGtt && s.hostBytes + size > kHostCap) return kWhyFull;
	return kWhyOk;
}
inline uint32_t may_import(const State &s, uint64_t uaddr, uint64_t size, bool gmcReady) {
	if (!uaddr || !size || ((uaddr | size) & (kPage - 1)) || size > kBoMaxBytes || uaddr >= kUserMax || size > kUserMax - uaddr)
		return kWhyArg;
	if (!gmcReady) return kWhyOrder;
	uint32_t i = 0;
	while (i < kBoMax && s.bo[i].live) i++;
	if (i == kBoMax || s.hostBytes + size > kHostCap) return kWhyFull;
	return kWhyOk;
}
// First fit in the slab, below every live VRAM BO's run. Returns false when nothing fits.
inline bool vram_place(const State &s, uint32_t pages, uint64_t *off) {
	const uint64_t len = (uint64_t)pages * kPage;
	for (uint64_t at = 0; at + len <= kSlabLen;) {
		uint64_t next = 0;
		for (uint32_t i = 0; i < kBoMax; i++) {
			const Bo &b = s.bo[i];
			if (b.live && b.kind == kVram && at < b.vramOff + bytes(b) && b.vramOff < at + len) { next = b.vramOff + bytes(b); break; }
		}
		if (!next) { *off = at; return true; }
		at = next;
	}
	return false;
}
// The new BO's handle (1-based); its fields set. The kext calls this only after the memory exists.
inline uint32_t record_create(State &s, uint32_t kind, uint32_t pages, uint64_t vramOff) {
	uint32_t i = 0;
	while (i < kBoMax && s.bo[i].live) i++;
	if (i == kBoMax) return 0;
	s.bo[i] = { true, false, kind, pages, 0, vramOff, 0 };
	if (kind == kVram) s.slabOwned = true; else s.hostBytes += (uint64_t)pages * kPage;
	return i + 1;
}

// A VA run [va, va + len) for BO h: page aligned, inside the window, clear of every other mapped BO.
inline bool va_free(const State &s, uint64_t va, uint64_t len, uint32_t except) {
	for (uint32_t i = 0; i < kBoMax; i++) {
		const Bo &b = s.bo[i];
		if (i + 1 != except && b.live && b.mapped && va < b.va + bytes(b) && b.va < va + len) return false;
	}
	return true;
}
inline bool in_window(uint64_t va, uint64_t len) { return va >= kVaBase && len <= kVaEnd - kVaBase && va <= kVaEnd - len; }
inline uint32_t slot_of(uint64_t va) { return (uint32_t)((va - n33gmc::kGartMc) / kPage); }
// gart map / unmap (17/18) of slots [slot, slot + count): refused when they touch the window.
inline bool window_hits(uint32_t slot, uint32_t count) {
	return count && (uint64_t)slot < (uint64_t)kVaSlot0 + kVaSlots && (uint64_t)slot + count > kVaSlot0;
}

inline uint32_t may_map(const State &s, uint32_t h, uint64_t va, uint32_t vmFlags, bool gmcReady, bool submitPending) {
	if (!handle_ok(s, h) || (va & (kPage - 1)) || !vm_flags_ok(vmFlags)) return kWhyArg;
	if (!gmcReady || s.bo[h - 1].mapped || submitPending) return kWhyOrder;
	const uint64_t len = bytes(s.bo[h - 1]);
	if (!in_window(va, len) || !va_free(s, va, len, h)) return kWhyVa;
	return kWhyOk;
}
inline void record_map(State &s, uint32_t h, uint64_t va, uint32_t vmFlags) { Bo &b = s.bo[h - 1]; b.mapped = true; b.va = va; b.vmFlags = vmFlags; }
inline uint32_t may_unmap(const State &s, uint32_t h, bool gmcReady, bool submitPending) {
	if (!handle_ok(s, h)) return kWhyArg;
	if (!gmcReady || !s.bo[h - 1].mapped || submitPending) return kWhyOrder;
	return kWhyOk;
}
inline void record_unmap(State &s, uint32_t h) { Bo &b = s.bo[h - 1]; b.mapped = false; b.va = 0; b.vmFlags = 0; }
// gmcReady false after a failed flush (GMC stopped): a stale PTE may still name the pages, so they stay until unload.
inline uint32_t may_free(const State &s, uint32_t h, bool gmcReady, bool submitPending) {
	if (!handle_ok(s, h)) return kWhyArg;
	if (!gmcReady || s.bo[h - 1].mapped || submitPending) return kWhyOrder;
	return kWhyOk;
}
inline void record_free(State &s, uint32_t h) {
	Bo &b = s.bo[h - 1];
	if (b.kind != kVram) s.hostBytes -= s.hostBytes >= bytes(b) ? bytes(b) : s.hostBytes;
	b = {};
}
inline uint32_t may_read(const State &s, uint32_t h, uint64_t dword) {
	if (!handle_ok(s, h) || dword >= bytes(s.bo[h - 1]) / 4) return kWhyArg;
	return kWhyOk;
}

// ---- Host import: the device-memory refusal (mirrors native_hostimport_pure.h import_page_verdict, review item A) ----
struct Bar { uint64_t base, size; };   // size 0: absent
// The 4 KiB page at pa overlaps a BAR (fails closed with no BAR list).
inline bool page_in_bars(uint64_t pa, const Bar *bars, uint32_t n) {
	if (!bars || !n) return true;
	const uint64_t pe = pa + kPage;
	if (pe < pa) return true;
	for (uint32_t i = 0; i < n; i++) {
		if (!bars[i].size) continue;
		const uint64_t be = bars[i].base + bars[i].size < bars[i].base ? ~0ull : bars[i].base + bars[i].size;
		if (pa < be && pe > bars[i].base) return true;
	}
	return false;
}

// ---- Stream 3: bo copy (COPY_DATA, nvd.h:252-271; Mesa sid.h:99-116, ac_cmdbuf_cp.c:238-263) ------------------------
constexpr uint32_t kOpCopyData = 0x40;
constexpr uint32_t kCopySrcMem = 1, kCopyDstMem = 5;   // SRC_SEL memory (Mesa COPY_DATA_SRC_MEM), DST_SEL memory
constexpr uint32_t kCopyWrConfirm = 1u << 20;
constexpr uint32_t kCopyCtl = kCopySrcMem | (kCopyDstMem << 8) | kCopyWrConfirm;   // 0x00100501, COUNT_SEL 0 (32-bit)
constexpr uint32_t kCopyN = 16;                        // = n33cp::xib_want(kXibBoCopy)
constexpr uint32_t kCopyBefore = 0xDEADBEEFu;          // dst samples before the copy
static_assert(kCopyN == 16 && n33cp::xib_want(n33cp::kXibBoCopy) == kCopyN && n33cp::xib_want(n33cp::kXibBoFault) == 0,
              "W1's wait counts the W2 samples");
inline uint32_t pkt_copy_data(uint32_t *p, uint64_t src, uint64_t dst) {
	p[0] = n33cp::pkt3(kOpCopyData, 4); p[1] = kCopyCtl;
	p[2] = n33cp::lo32(src) & ~3u; p[3] = n33cp::hi32(src); p[4] = n33cp::lo32(dst) & ~3u; p[5] = n33cp::hi32(dst);
	return 6;
}
// Sample k's byte offset in both BOs: spread over the smaller one, so a multi-page pair crosses many PTEs.
inline uint64_t copy_off(uint32_t k, uint64_t minBytes) { return ((uint64_t)k * (minBytes / kCopyN)) & ~3ull; }
inline uint32_t pattern(uint32_t seq, uint32_t k) { return 0xB0000000u | ((seq & 0xFFFFFu) << 4) | (k & 0xFu); }
// 16 COPY_DATA then the fence: 104 dwords.
inline uint32_t copy_stream(uint32_t *p, uint64_t srcVa, uint64_t dstVa, uint64_t minBytes, uint64_t fenceMc, uint32_t seq) {
	uint32_t n = 0;
	for (uint32_t k = 0; k < kCopyN; k++) n += pkt_copy_data(p + n, srcVa + copy_off(k, minBytes), dstVa + copy_off(k, minBytes));
	return n + n33cp::pkt_release_mem(p + n, fenceMc, seq, true);
}
inline uint32_t may_copy(const State &s, uint32_t src, uint32_t dst, bool gmcReady) {
	if (!handle_ok(s, src) || !handle_ok(s, dst) || src == dst) return kWhyArg;
	if (!gmcReady || !s.bo[src - 1].mapped || !s.bo[dst - 1].mapped) return kWhyOrder;
	if (!(s.bo[src - 1].vmFlags & kVmRead) || !(s.bo[dst - 1].vmFlags & kVmWrite)) return kWhyArg;
	return kWhyOk;
}

// ---- Stream 4: bo fault (WRITE_DATA to an unmapped VA, then the fence: the X4b wd builder, 13 dwords) ---------------
constexpr uint32_t kFaultMarker = 0xFA017ED0u;
inline uint32_t fault_stream(uint32_t *p, uint64_t va, uint64_t fenceMc, uint32_t seq) {
	return n33cp::pkt_wd(p, va, kFaultMarker, fenceMc, seq, true);
}
inline uint32_t may_fault(const State &s, uint64_t va, bool gmcReady) {
	if (va & (kPage - 1)) return kWhyArg;
	if (!gmcReady) return kWhyOrder;
	if (!in_window(va, kPage) || !va_free(s, va, kPage, 0)) return kWhyVa;
	return kWhyOk;
}
// GCVM_L2_PROTECTION_FAULT_STATUS / ADDR (gc_11_0_0_sh_mask.h:11003-11030) and the clear (gmc_v11_0.c:133-134).
constexpr uint32_t kFaultCntl = 0x2824;                 // GCVM_L2_PROTECTION_FAULT_CNTL (n33gmc::kGc.faultCntl)
constexpr uint32_t kFaultClear = 1u << 0;               // CLEAR_PROTECTION_FAULT_STATUS_ADDR
constexpr uint32_t kStatusRw = 1u << 18, kStatusVmidShift = 20, kStatusVmidMask = 0xF00000u;
constexpr uint32_t kStatusCidShift = 9, kStatusCidMask = 0x3FE00u;
// The fault the test expects: something latched, a write, VMID 0, and the logical page of va.
inline bool fault_match(uint32_t status, uint32_t addrLo, uint32_t addrHi, uint64_t va) {
	return status != 0 && (status & kStatusRw) && ((status & kStatusVmidMask) >> kStatusVmidShift) == 0 &&
	       addrLo == (uint32_t)(va >> 12) && (addrHi & 0xFu) == (uint32_t)((va >> 44) & 0xFu);
}

// ---- What a VM fault does to the MEC compute queue on gfx11 (hardware, task README "bo fault"; sources) --------------
// A VM fault on a compute (MEC) queue is FATAL on gfx11 and the queue cannot continue: the only CP UTCL1 error-halt-disable
// is CP_DEBUG.CPG_UTCL1_ERROR_HALT_DISABLE, which is the graphics ring (CPG). gc_11_0_0_sh_mask.h has no CPC/compute
// equivalent, and gfx_v11_0.c:6721-6725 treats a compute/user-queue fault as fatal, leaving the HQD halted for a per-queue
// reset. So the deliberate fault latches GCVM_L2_PROTECTION_FAULT_STATUS correctly AND halts the queue: CP_HQD_ERROR latches
// TC_UTCL1_ERROR (bit 19, 0x80000) and the IB's RELEASE_MEM fence never lands. The W2 design's R3 (redirect to the dummy
// page so the queue carries on) is wrong for the compute queue. The correct result is a correctly latched, cleared fault
// and a halted queue; a K6 drain then recovers it (a fence that landed would mean the queue continued, which it does not).
constexpr uint32_t kHqdErrTcUtcl1 = 0x80000u;   // CP_HQD_ERROR.TC_UTCL1_ERROR (gc_11_0_0_sh_mask.h:17714, :17728)
inline bool fault_pass(bool match, uint32_t statusAfterClear, bool queueHalted) {
	return match && statusAfterClear == 0 && queueHalted;
}

} // namespace n33bo
