// n33_vm_pure.h: decisions for stage 3 step 7 W2V on Navi 33: a per-process GPU VM in VMID 8 with a 4-level gfx11 page
// table, the BOs of W2 mapped at caller-chosen canonical VAs, compute IBs that run in VMID 8, and a deliberate VMID-8
// fault. Design: RDNA3/Tasks/navi33-cobalt64-port/stage3-step7-gpuvm-design.md.
// Host-testable: no kernel types, no I/O. It ports the gfx12 n48native (native_s1b_pure.h / native_s1c_pure.h) VA helpers,
// walker and context pattern to gfx11: no bit 63 IS_PTE, MTYPE at 50:48 (UC = 3), CONTEXTn_CNTL BLOCK at bit 3 / RETRY at
// bit 7. Linux references are to the pinned tree 3857c2fe (gmc_v11_0.c, gfxhub_v3_0.c, gfx_v11_0.c, amdgpu_vm*.c, the
// gc_11_0_0 register headers and include/navi10_enum.h) and Mesa bffd9194 (common/darwin/ac_darwin_drm.c).
#pragma once
#include <stdint.h>
#include "n33_cp_pure.h"
#include "n33_bo_pure.h"

namespace n33vm {

using n33gmc::kPage;

// ---- VMID and geometry (design "VMID ownership", "Geometry for GC 11") --------------------------------------------
constexpr uint32_t kVmid = 8;                 // D1: the Navi 48 kext's and the Darwin backend's static VMID
constexpr uint32_t kCtxDepth = 3;             // num_level: PDB2 -> PDB1 -> PDB0 -> PTB (gmc_v11_0.c:771-791)
constexpr uint32_t kCtxBlock = 0;             // block_size (9) - 9
constexpr uint64_t kMaxPfn = (1ull << 48) >> 12;   // 2^36 (amdgpu_vm.c:2423); the END register is max_pfn - 1

// ---- Virtual addresses (gfx11 = gfx12 here: 48-bit, 4 levels of 9 bits) -------------------------------------------
constexpr uint64_t kVaMask48 = (1ull << 48) - 1;
constexpr uint64_t va_strip(uint64_t va) { return va & kVaMask48; }          // amdgpu_gmc.h HOLE_MASK
constexpr bool va_canonical(uint64_t va) { const uint64_t t = va >> 47; return t == 0ull || t == 0x1FFFFull; }
constexpr uint64_t va_canonicalize(uint64_t v) { v &= kVaMask48; return (v & (1ull << 47)) ? (v | ~kVaMask48) : v; }
constexpr bool va_high_half(uint64_t va) { return (va_strip(va) >> 47) & 1ull; }
constexpr uint32_t idx_pdb2(uint64_t va) { return (uint32_t)((va_strip(va) >> 39) & 0x1FFu); }   // root: 256..511 = high half
constexpr uint32_t idx_pdb1(uint64_t va) { return (uint32_t)((va_strip(va) >> 30) & 0x1FFu); }
constexpr uint32_t idx_pdb0(uint64_t va) { return (uint32_t)((va_strip(va) >> 21) & 0x1FFu); }
constexpr uint32_t idx_ptb (uint64_t va) { return (uint32_t)((va_strip(va) >> 12) & 0x1FFu); }
constexpr uint64_t va_pfn(uint64_t va) { return va_strip(va) >> 12; }
constexpr bool va_in_context(uint64_t va) { return va_pfn(va) < kMaxPfn; }

// ---- Entries (gfx11: gmc_v11_0.c:414-443; amdgpu_vm.h). No bit 63; MTYPE 50:48; UC = 3. ---------------------------
constexpr uint64_t kPteValid = n33gmc::kPteValid, kPteSystem = n33gmc::kPteSystem, kPteSnooped = n33gmc::kPteSnooped,
                   kPteExec = n33gmc::kPteExec, kPteRead = n33gmc::kPteRead, kPteWrite = n33gmc::kPteWrite;
constexpr uint64_t kPteAddrMask = n33gmc::kPteAddrMask;          // 0x0000FFFFFFFFF000
constexpr uint64_t kPdeAddrMask = 0x0000FFFFFFFFF000ull;        // tables placed on 4 KiB boundaries (the field is 47:6)
constexpr uint64_t kMtypeShift = 48;                            // MTYPE at 50:48 (amdgpu_vm.h:114-118)
enum Mtype : uint32_t { kMtypeNc = 0, kMtypeWc = 1, kMtypeCc = 2, kMtypeUc = 3 };   // navi10_enum.h:285-309
constexpr uint64_t mtype_bits(uint32_t m) { return ((uint64_t)(m & 7u)) << kMtypeShift; }

// uapi VM flags (amdgpu_drm.h:882-886); MTYPE carried in bits 8:5 of the caller flags (design D8).
constexpr uint32_t kVmRead = 1u << 1, kVmWrite = 1u << 2, kVmExec = 1u << 3;
constexpr uint32_t kVmMtypeShift = 5, kVmMtypeMask = 0xFu << kVmMtypeShift;     // bits 8:5
constexpr uint32_t kVmFlagMask = kVmRead | kVmWrite | kVmExec | kVmMtypeMask;
constexpr uint32_t kVmDefault = kVmRead | kVmWrite | kVmExec;                   // R|W|X NC (mtype 0)
constexpr uint32_t vm_mtype(uint32_t f) { return (f & kVmMtypeMask) >> kVmMtypeShift; }
// A legal mapping flag set: inside the known bits, MTYPE 0..3, R or W present. PRT/NOALLOC are not uapi PAGE_* bits, so
// any bit outside kVmFlagMask (which includes the gfx11 PRT/NOALLOC-carrying positions) is refused here as 75.
constexpr bool vm_flags_ok(uint32_t f) { return !(f & ~kVmFlagMask) && vm_mtype(f) <= 3u && (f & (kVmRead | kVmWrite)); }

// A directory entry: table PA | VALID, nothing else (gmc_v11_0.c:433-443, amdgpu_ttm.c:1439-1460). 0 for a bad address.
constexpr uint64_t pde_encode(uint64_t pa) { return ((pa & ~kPdeAddrMask) != 0ull) ? 0ull : (pa | kPteValid); }
// The non-address bits of a leaf for (system, flags): VALID, MTYPE, SYSTEM|SNOOPED for host pages, R/W/X from the flags.
constexpr uint64_t leaf_flags(bool system, uint32_t vmFlags) {
	uint64_t f = kPteValid | mtype_bits(vm_mtype(vmFlags));
	if (system) f |= kPteSystem | kPteSnooped;
	if (vmFlags & kVmRead) f |= kPteRead;
	if (vmFlags & kVmWrite) f |= kPteWrite;
	if (vmFlags & kVmExec) f |= kPteExec;
	return f;
}
// A 4 KiB leaf: address | flags, no bit 63 (gfx11). 0 for a bad address.
constexpr uint64_t pte_leaf(uint64_t pa, uint64_t flags) { return ((pa & ~kPteAddrMask) != 0ull) ? 0ull : (pa | flags); }
constexpr bool entry_valid(uint64_t e) { return (e & kPteValid) != 0ull; }
constexpr uint64_t entry_pa(uint64_t e) { return e & kPteAddrMask; }

// ---- The page-table arena (D3): VRAM 0xD000000..0xD800000, protected kind 21, inside BAR0 ------------------------
constexpr uint64_t kArenaOff = 0xD000000ull, kArenaLen = 8ull << 20;   // right after W2's slab (ends at 0xD000000)
constexpr uint32_t kArenaPages = (uint32_t)(kArenaLen / kPage);         // 2048
constexpr uint32_t kKind = 21;
constexpr uint32_t kParkPageIdx = 0;                   // always zero: every PDB2 entry invalid
constexpr uint32_t kRootPageIdx = 1;                   // the root PDB2
constexpr uint32_t kFirstFreePageIdx = 2;              // tables handed out from here up
constexpr uint32_t kSentinelPageIdx = kArenaPages - 1; // never handed out; V6 checks it stays zero
static_assert(kArenaOff == n33bo::kSlabOff + n33bo::kSlabLen, "the arena sits right after W2's VRAM BO slab");
static_assert(kArenaOff + kArenaLen <= 0x10000000ull, "the arena is inside the 256 MiB BAR0 (CPU-written entries)");
static_assert(kKind == n33bo::kKind + 1, "kind 21 is the next protected kind after W2's slab (20)");
// The VRAM offset of a page index, and the offset of entry `idx` inside it.
constexpr uint64_t page_off(uint32_t pageIdx) { return kArenaOff + (uint64_t)pageIdx * kPage; }
constexpr uint64_t entry_off(uint64_t pageOff, uint32_t idx) { return pageOff + (uint64_t)idx * 8; }

// ---- CONTEXT8 register fields and absolutes (D5). Absolute dword = GC seg 0 0x1260 + offset, seg 1 0xA000 + offset. --
constexpr uint32_t kGcSeg0 = 0x1260, kGcSeg1 = 0xA000;
// gc_11_0_0_offset.h regGCVM_CONTEXT8_* (checked in the host test):
constexpr uint32_t kCtx8Cntl       = kGcSeg0 + 0x1690;   // 0x28F0
constexpr uint32_t kCtx8BaseLo     = kGcSeg0 + 0x1703;   // 0x2963
constexpr uint32_t kCtx8BaseHi     = kGcSeg0 + 0x1704;   // 0x2964
constexpr uint32_t kCtx8StartLo    = kGcSeg0 + 0x1723;   // 0x2983
constexpr uint32_t kCtx8StartHi    = kGcSeg0 + 0x1724;   // 0x2984
constexpr uint32_t kCtx8EndLo      = kGcSeg0 + 0x1743;   // 0x29A3
constexpr uint32_t kCtx8EndHi      = kGcSeg0 + 0x1744;   // 0x29A4
constexpr uint32_t kInvEng17Req    = kGcSeg0 + 0x16bc;   // 0x291C
constexpr uint32_t kInvEng17Ack    = kGcSeg0 + 0x16ce;   // 0x292E
constexpr uint32_t kFaultCntl      = n33bo::kFaultCntl;  // 0x2824
constexpr uint32_t kFaultStatus    = n33cp::kGcFaultStatus;   // 0x2828
constexpr uint32_t kFaultAddrLo    = n33cp::kGcFaultAddrLo;   // 0x2829
constexpr uint32_t kFaultAddrHi    = n33cp::kGcFaultAddrHi;   // 0x282A
constexpr uint32_t kGrbmGfxCntl    = n33cp::kGrbmGfxCntl;     // 0xA900 (seg 1 0x0900)
constexpr uint32_t kShMemBases     = kGcSeg1 + 0x09e3;   // 0xA9E3
constexpr uint32_t kShMemConfig    = kGcSeg1 + 0x09e4;   // 0xA9E4

// CONTEXT1_CNTL field shifts, gfx11 (gc_11_0_0_sh_mask.h; the host test checks them):
constexpr uint32_t kCntlEnableShift = 0, kCntlDepthShift = 1, kCntlBlockShift = 3, kCntlRetryShift = 7;
constexpr uint32_t kCntlFault0Shift = 0xa;    // RANGE_PROTECTION_FAULT_ENABLE_DEFAULT; the 7 bits step by 2 up to 0x16
constexpr uint32_t kCntlEnableMask = 1u << kCntlEnableShift, kCntlDepthMask = 3u << kCntlDepthShift,
                   kCntlBlockMask = 0xFu << kCntlBlockShift, kCntlRetryMask = 1u << kCntlRetryShift;
// The 7 *_PROTECTION_FAULT_ENABLE_DEFAULT bits G6 wrote for every context (gfxhub_v3_0.c:290-337).
constexpr uint32_t ctx_fault7_mask() { uint32_t m = 0; for (uint32_t i = 0; i < 7; i++) m |= 1u << (kCntlFault0Shift + 2u * i); return m; }
// The fields G6 set (ENABLE, DEPTH, BLOCK, RETRY, the 7 fault bits) and the value under them for a VMID 1..15 context:
constexpr uint32_t kCtxCheckMask = kCntlEnableMask | kCntlDepthMask | kCntlBlockMask | kCntlRetryMask | ctx_fault7_mask();
constexpr uint32_t kCtxCheckVal = kCntlEnableMask | (kCtxDepth << kCntlDepthShift) | ctx_fault7_mask();   // BLOCK 0, RETRY 0
static_assert(kCtxCheckVal == 0x00555407u, "G6's CONTEXT8 fields read 0x00555407 (design D5)");
// CONTEXT8_CNTL reads as G6 left it when its fields under kCtxCheckMask equal kCtxCheckVal (whole register ~0x01fffe07).
constexpr bool ctx8_cntl_ok(uint32_t cntl) { return (cntl & kCtxCheckMask) == kCtxCheckVal; }

// ---- TLB invalidation for VMID 8 (D7; gfxhub_v3_0.c:56-72) --------------------------------------------------------
// PER_VMID_INVALIDATE_REQ = 1 << vmid; FLUSH_TYPE 0; INVALIDATE_L2_PTES/PDE0/PDE1/PDE2/L1_PTES = 1.
constexpr uint32_t kInvReqVmid8 = (1u << kVmid) | (1u << 0x13) | (1u << 0x14) | (1u << 0x15) | (1u << 0x16) | (1u << 0x17);
static_assert(kInvReqVmid8 == 0x00F80100u, "the VMID-8 engine-17 request word (design D5)");
constexpr uint32_t kInvAckVmid8 = 1u << kVmid;   // ACK bit 8 (PER_VMID_INVALIDATE_ACK bits 15:0)

// ---- The VMID-8 select and SH_MEM (D5, D6) ------------------------------------------------------------------------
constexpr uint32_t kGrbmVmidShift = 4;                 // GRBM_GFX_CNTL.VMID (gc_11_0_0_sh_mask.h)
constexpr uint32_t kGrbmSelVmid8 = kVmid << kGrbmVmidShift;   // 0x80
constexpr uint32_t kShMemConfigVal = 0xC00Cu;          // every VMID (gfx_v11_0.c:336-339)
constexpr uint32_t kShMemBasesVal = 0x20001000u;       // D6: Linux's graphics value for VMIDs 1..7

// ---- The Darwin backend's VA-range predicate (ac_darwin_drm.c:163-174) --------------------------------------------
// The kext reports info.va_{low,high}_{first,last}; the backend accepts the device iff the canned row's VA ranges lie
// inside them, with the row's `*_max` EXCLUSIVE: offset >= first and (max - 1) <= last on each half (and the high half
// only when high_va_offset is non-zero). The info values are the Navi 48 ones (ac_darwin_mock.c:628-631).
constexpr uint64_t kInfoLowFirst  = 0x10000ull,            kInfoLowLast  = 0x7FFFFFFFFFFFull;
constexpr uint64_t kInfoHighFirst = 0xFFFF800000000000ull, kInfoHighLast = 0xFFFFFFFFFFFFFFFFull;
// The canned navi33 row (virtual_address_offset/max, high_va_offset/max; design D8, the Navi 48 values).
constexpr uint64_t kRowVaOffset   = 0x10000ull,            kRowVaMax    = 0x800000000000ull;
constexpr uint64_t kRowHighOffset = 0xFFFF800000000000ull, kRowHighMax  = 0xFFFFFFFFFFBFE000ull;
constexpr bool ranges_contain_row() {
	return kRowVaOffset >= kInfoLowFirst && (kRowVaMax - 1) <= kInfoLowLast &&
	       (kRowHighOffset == 0ull || (kRowHighOffset >= kInfoHighFirst && (kRowHighMax - 1) <= kInfoHighLast));
}
static_assert(ranges_contain_row(), "the reported ranges must contain the navi33 row (ac_darwin_drm.c:165-167)");

// ---- Refusal reasons (75 to 85; design "New kext actions ... and protected kind 21"). The design's exact numbers. ---
constexpr uint32_t kCodeOk = 0;
constexpr uint32_t kCodeArg = 75;          // VA not canonical / aligned / >= 0x10000 / straddling bit 47; size, offset, flags, handle
constexpr uint32_t kCodeOrder = 76;        // G6 not done or GMC stopped; vm create twice or an action before it; a submit outstanding
constexpr uint32_t kCodeCtx = 77;          // CONTEXT8 not as G6 left it, or its BASE changed since vm create
constexpr uint32_t kCodeArena = 78;        // the page-table arena is full (nothing written)
constexpr uint32_t kCodeOverlap = 79;      // a VA range overlaps a VMID-8 mapping; unmap of a (handle, VA) not mapped; a fault VA mapped
constexpr uint32_t kCodeAck = 80;          // a VMID-8 invalidate did not acknowledge in 100 ms (the VM is stopped)
constexpr uint32_t kCodeSlab = 81;         // the arena overlaps a live protected range when first taken
constexpr uint32_t kCodeStream = 82;       // IB VA not in an R|X VRAM mapping / stream too big; code VA not X; copy src not R or dst not W
constexpr uint32_t kCodeShMem = 83;        // SH_MEM_CONFIG / SH_MEM_BASES under the VMID-8 select did not read back
constexpr uint32_t kCodeFreeMapped = 84;   // bo free of a BO that still has a VMID-8 mapping
constexpr uint32_t kCodeApertureVa = 85;   // vm fault VA inside the VMID-0 apertures other than the two V6 sentinels

// ---- gemva_check (design D8; the fork's native_s1c_pure.h gemva_check for gfx11) ----------------------------------
// A mapping VA must be canonical, 4 KiB aligned, stripped start >= 0x10000, not straddle bit 47, inside the context,
// and (with its size) inside the BO.  off and size 4 KiB multiples.
constexpr bool va_straddles_seam(uint64_t va, uint64_t size) {
	// [strip(va), strip(va)+size) must stay in one 47-bit half.
	const uint64_t s = va_strip(va);
	return (s >> 47) != ((s + size - 1u) >> 47);
}
constexpr uint32_t gemva_check(uint32_t vmFlags, uint64_t va, uint64_t off, uint64_t size, uint64_t boBytes) {
	if (size == 0ull || (size & (kPage - 1)) || (off & (kPage - 1)) || off > boBytes || size > boBytes - off) return kCodeArg;
	if (!va_canonical(va) || (va & (kPage - 1)) || va_strip(va) < 0x10000ull) return kCodeArg;
	if (va_straddles_seam(va, size) || !va_in_context(va_canonicalize(va_strip(va) + size - 1u))) return kCodeArg;
	if (!vm_flags_ok(vmFlags)) return kCodeArg;
	return kCodeOk;
}

// ---- The VMID-0 apertures a vm fault VA must not fall in, except the two V6 sentinels (code 85) --------------------
// FB aperture + GART: 0x8000000000 .. 0x84FFFFFFFF (design stream 7).
constexpr uint64_t kApertureLo = 0x8000000000ull, kApertureHi = 0x8500000000ull;
constexpr bool va_in_apertures(uint64_t va) { const uint64_t s = va_strip(va); return s >= kApertureLo && s < kApertureHi; }

// ---- State: one VM, up to kMapMax mappings ------------------------------------------------------------------------
constexpr uint32_t kMapMax = 64;   // W3: RADV maps each of its BOs (peak ~11); 64 is headroom. Only the map array grows; the arena (2048 pages) holds the extra page tables.
struct Map { bool used; uint32_t handle; uint32_t vmFlags; uint64_t va; uint64_t bytes; uint64_t off; };   // off: the BO byte offset the mapping starts at
struct State {
	bool live;             // vm create done, not destroyed
	bool stopped;          // an invalidate did not acknowledge: every later action refused 80
	uint64_t rootPa;       // the root PDB2's VRAM physical address (page kRootPageIdx)
	uint32_t nextPage;     // next free arena page index (grows from kFirstFreePageIdx)
	uint64_t ctx8BaseAtCreate;   // CONTEXT8 BASE as read back at create; a change is 77
	Map map[kMapMax];
};
inline bool gmc_ready_ok(bool gmcReady) { return gmcReady; }

// vm create: G6 done, GMC not stopped, not already live.
inline uint32_t may_create(const State &s, bool gmcReady, bool submitPending) {
	if (!gmcReady) return kCodeOrder;
	if (s.live || submitPending) return kCodeOrder;
	return kCodeOk;
}
// Every action but create needs the VM live and not stopped; a submit outstanding blocks maps/unmaps/destroy.
inline uint32_t vm_ready(const State &s, bool gmcReady) {
	if (!gmcReady || !s.live || s.stopped) return kCodeOrder;
	return kCodeOk;
}
// A VA run [va, va+len) clear of every mapping but `except` (0 = none).
inline bool va_free(const State &s, uint64_t va, uint64_t len, uint32_t exceptIdx) {
	const uint64_t a = va_strip(va), b = a + len;
	for (uint32_t i = 0; i < kMapMax; i++) {
		if (i == exceptIdx || !s.map[i].used) continue;
		const uint64_t c = va_strip(s.map[i].va), d = c + s.map[i].bytes;
		if (a < d && c < b) return false;
	}
	return true;
}
inline uint32_t may_map(const State &s, bool gmcReady, bool submitPending, bool handleLive, uint32_t vmFlags,
                        uint64_t va, uint64_t off, uint64_t size, uint64_t boBytes) {
	uint32_t w = vm_ready(s, gmcReady); if (w) return w;
	if (submitPending) return kCodeOrder;
	if (!handleLive) return kCodeArg;
	w = gemva_check(vmFlags, va, off, size, boBytes); if (w) return w;
	uint32_t i = 0; while (i < kMapMax && s.map[i].used) i++;
	if (i == kMapMax) return kCodeArena;           // no record slot (treated as arena-full for the test's purpose)
	if (!va_free(s, va, size, kMapMax)) return kCodeOverlap;
	return kCodeOk;
}
inline int find_map(const State &s, uint32_t handle, uint64_t va) {
	for (uint32_t i = 0; i < kMapMax; i++) if (s.map[i].used && s.map[i].handle == handle && va_strip(s.map[i].va) == va_strip(va)) return (int)i;
	return -1;
}
inline uint32_t may_unmap(const State &s, bool gmcReady, bool submitPending, uint32_t handle, uint64_t va) {
	uint32_t w = vm_ready(s, gmcReady); if (w) return w;
	if (submitPending) return kCodeOrder;
	if ((va & (kPage - 1)) || !va_canonical(va)) return kCodeArg;
	if (find_map(s, handle, va) < 0) return kCodeOverlap;
	return kCodeOk;
}
inline uint32_t record_map(State &s, uint32_t handle, uint32_t vmFlags, uint64_t va, uint64_t bytes, uint64_t off = 0) {
	uint32_t i = 0; while (i < kMapMax && s.map[i].used) i++;
	if (i == kMapMax) return kMapMax;
	s.map[i] = { true, handle, vmFlags, va, bytes, off };
	return i;
}
// The BO byte offset a covered VA resolves to (the mapping's BO offset plus the VA's offset into the mapping).
inline uint64_t bo_offset_of(const State &s, uint32_t mapIdx, uint64_t va) {
	return s.map[mapIdx].off + (va_strip(va) - va_strip(s.map[mapIdx].va));
}
inline void record_unmap(State &s, uint32_t idx) { if (idx < kMapMax) s.map[idx] = {}; }
// bo free gains this check (design code 84): refuse while the BO has any VMID-8 mapping.
inline bool bo_has_mapping(const State &s, uint32_t handle) {
	for (uint32_t i = 0; i < kMapMax; i++) if (s.map[i].used && s.map[i].handle == handle) return true;
	return false;
}

// The mapping (if any) that covers [va, va+bytes) with all of `need` flags. -1 when none covers it fully.
inline int covering_map(const State &s, uint64_t va, uint64_t bytes, uint32_t need) {
	const uint64_t a = va_strip(va), b = a + bytes;
	for (uint32_t i = 0; i < kMapMax; i++) {
		if (!s.map[i].used) continue;
		const uint64_t c = va_strip(s.map[i].va), d = c + s.map[i].bytes;
		if (c <= a && b <= d && (s.map[i].vmFlags & need) == need) return (int)i;
	}
	return -1;
}
// vm fault VA: canonical, dword aligned (a WRITE_DATA target, so sub-page offsets are allowed for the V6 aliasing test),
// not covered by a mapping, and not in the VMID-0 apertures except the two V6 sentinels (passed by the kext).
inline uint32_t may_fault(const State &s, uint64_t va, uint64_t sentinelA, uint64_t sentinelB) {
	if ((va & 3u) || !va_canonical(va)) return kCodeArg;
	if (covering_map(s, va & ~(kPage - 1), kPage, 0) >= 0) return kCodeOverlap;
	if (va_in_apertures(va) && va_strip(va) != va_strip(sentinelA) && va_strip(va) != va_strip(sentinelB)) return kCodeApertureVa;
	return kCodeOk;
}

// ---- The page-table walker over an abstract memory (ported from native_s1c_pure.h for gfx11) ----------------------
// `M` supplies: uint64_t rd(uint64_t tablePa, uint32_t idx); void wr(uint64_t tablePa, uint32_t idx, uint64_t v);
//               uint64_t alloc_page()  -> the VRAM pa of a fresh ZEROED table page, or 0 when the arena is full.
template <class M>
inline uint64_t pt_ensure_ptb(uint64_t rootPa, uint64_t va, M &m) {
	uint64_t tbl = rootPa;
	const uint32_t idx[3] = { idx_pdb2(va), idx_pdb1(va), idx_pdb0(va) };
	for (uint32_t lvl = 0; lvl < 3; lvl++) {
		uint64_t e = m.rd(tbl, idx[lvl]);
		if (!entry_valid(e)) {
			const uint64_t pa = m.alloc_page();
			const uint64_t enc = pa ? pde_encode(pa) : 0ull;
			if (enc == 0ull) return 0ull;
			m.wr(tbl, idx[lvl], enc);
			e = enc;
		}
		tbl = entry_pa(e);
	}
	return tbl;
}
// Map `pages` 4 KiB pages at `va` to consecutive physical pages from `firstPa`. Two passes (tables, then leaves), so a
// failed call leaves no partial mapping. Returns kCodeOk / kCodeArena / kCodeArg.
template <class M>
inline uint32_t pt_map(uint64_t rootPa, uint64_t va, uint64_t pages, uint64_t firstPa, uint64_t leafFlagBits, M &m) {
	if (pages == 0ull) return kCodeArg;
	for (uint64_t p = 0; p < pages;) {
		const uint64_t v = va + p * kPage;
		if (pt_ensure_ptb(rootPa, v, m) == 0ull) return kCodeArena;
		p += 512ull - idx_ptb(v);
	}
	for (uint64_t p = 0; p < pages;) {
		const uint64_t v = va + p * kPage;
		const uint64_t ptb = pt_ensure_ptb(rootPa, v, m);
		if (ptb == 0ull) return kCodeArena;
		const uint64_t run = 512ull - idx_ptb(v) < pages - p ? 512ull - idx_ptb(v) : pages - p;
		for (uint64_t k = 0; k < run; k++) {
			const uint64_t pte = pte_leaf(firstPa + (p + k) * kPage, leafFlagBits);
			if (pte == 0ull) return kCodeArg;
			m.wr(ptb, idx_ptb(v) + (uint32_t)k, pte);
		}
		p += run;
	}
	return kCodeOk;
}
// Like pt_map but the physical address of page p comes from a functor pa(p) (GTT/import BOs are not contiguous).
template <class M, class PaFn>
inline uint32_t pt_map_pages(uint64_t rootPa, uint64_t va, uint64_t pages, PaFn pa, uint64_t leafFlagBits, M &m) {
	if (pages == 0ull) return kCodeArg;
	for (uint64_t p = 0; p < pages;) {
		const uint64_t v = va + p * kPage;
		if (pt_ensure_ptb(rootPa, v, m) == 0ull) return kCodeArena;
		p += 512ull - idx_ptb(v);
	}
	for (uint64_t p = 0; p < pages;) {
		const uint64_t v = va + p * kPage;
		const uint64_t ptb = pt_ensure_ptb(rootPa, v, m);
		if (ptb == 0ull) return kCodeArena;
		const uint64_t run = 512ull - idx_ptb(v) < pages - p ? 512ull - idx_ptb(v) : pages - p;
		for (uint64_t k = 0; k < run; k++) {
			const uint64_t pte = pte_leaf(pa(p + k), leafFlagBits);
			if (pte == 0ull) return kCodeArg;
			m.wr(ptb, idx_ptb(v) + (uint32_t)k, pte);
		}
		p += run;
	}
	return kCodeOk;
}
template <class M>
inline void pt_unmap(uint64_t rootPa, uint64_t va, uint64_t pages, M &m) {
	for (uint64_t p = 0; p < pages;) {
		const uint64_t v = va + p * kPage;
		uint64_t tbl = rootPa; bool ok = true;
		const uint32_t idx[3] = { idx_pdb2(v), idx_pdb1(v), idx_pdb0(v) };
		for (uint32_t lvl = 0; lvl < 3 && ok; lvl++) {
			const uint64_t e = m.rd(tbl, idx[lvl]);
			if (!entry_valid(e)) ok = false; else tbl = entry_pa(e);
		}
		const uint64_t run = 512ull - idx_ptb(v) < pages - p ? 512ull - idx_ptb(v) : pages - p;
		if (ok) for (uint64_t k = 0; k < run; k++) m.wr(tbl, idx_ptb(v) + (uint32_t)k, 0ull);
		p += run;
	}
}
template <class M>
inline uint64_t pt_walk(uint64_t rootPa, uint64_t va, M &m, uint64_t *leafOut) {
	uint64_t tbl = rootPa;
	const uint32_t idx[3] = { idx_pdb2(va), idx_pdb1(va), idx_pdb0(va) };
	for (uint32_t lvl = 0; lvl < 3; lvl++) {
		const uint64_t e = m.rd(tbl, idx[lvl]);
		if (!entry_valid(e)) { if (leafOut) *leafOut = 0; return 0ull; }
		tbl = entry_pa(e);
	}
	const uint64_t leaf = m.rd(tbl, idx_ptb(va));
	if (leafOut) *leafOut = leaf;
	return entry_valid(leaf) ? (entry_pa(leaf) | (va & (kPage - 1ull))) : 0ull;
}

// ---- The three VMID-8 test streams (design "The test streams"). No RELEASE_MEM in the IB: the ring carries the fence --
// Stream 5, vm copy: W2's 16 COPY_DATA dword copies at VMID-8 VAs, without its fence. 96 dwords.
inline uint32_t copy_stream(uint32_t *p, uint64_t srcVa, uint64_t dstVa, uint64_t minBytes) {
	uint32_t n = 0;
	for (uint32_t k = 0; k < n33bo::kCopyN; k++)
		n += n33bo::pkt_copy_data(p + n, srcVa + n33bo::copy_off(k, minBytes), dstVa + n33bo::copy_off(k, minBytes));
	return n;
}
// Stream 6, vm dispatch: step 6's K4 dispatch, without the RELEASE_MEM. COMPUTE_PGM = code VA >> 8, USER_DATA = dst VA.
inline uint32_t dispatch_stream(uint32_t *p, uint64_t codeVa, uint64_t dstVa) {
	using namespace n33cp;
	uint32_t n = 0;
	const uint32_t pgm[2] = { lo32(codeVa >> 8), lo32(codeVa >> 40) }, rsrc[2] = { kRsrc1, kRsrc2 }, zero = 0, r3 = kRsrc3;
	const uint32_t thr[3] = { kResultDw, 1, 1 }, ud[2] = { lo32(dstVa), hi32(dstVa) };
	n += set_sh(p + n, kComputePgmLo, pgm, 2);
	n += set_sh(p + n, kComputeRsrc1, rsrc, 2);
	n += set_sh(p + n, kComputeResLimits, &zero, 1);
	n += set_sh(p + n, kComputeTmpring, &zero, 1);
	n += set_sh(p + n, kComputeRsrc3, &r3, 1);
	n += set_sh(p + n, kComputeNumX, thr, 3);
	n += set_sh(p + n, kComputeUserData0, ud, 2);
	p[n++] = pkt3(kOpDispatchDirect, 3); p[n++] = 1; p[n++] = 1; p[n++] = 1; p[n++] = kInitiator;
	p[n++] = pkt3(kOpEventWrite, 0); p[n++] = kCsPartialFlush;
	return n;
}
// Stream 7, vm fault: WRITE_DATA of the marker to va, without the fence. 5 dwords.
inline uint32_t fault_stream(uint32_t *p, uint64_t va) { return n33cp::pkt_write_data(p, va, n33bo::kFaultMarker); }

// The ring content for a VMID-8 submit (D2): ACQUIRE_MEM, INDIRECT_BUFFER(ib va, VMID 8), then the RELEASE_MEM fence
// in VMID 0.
inline uint32_t ring_stream(uint32_t *p, uint64_t ibVa, uint32_t ibDw, uint64_t fenceMc, uint32_t seq) {
	uint32_t n = n33cp::pkt_acquire_mem(p);
	n += n33cp::pkt_indirect_buffer(p + n, ibVa, ibDw, kVmid);
	return n + n33cp::pkt_release_mem(p + n, fenceMc, seq, true);   // GL2_WB writes back NC lines before the fence
}

// ---- The VMID-8 fault the test expects (design stream 7) ----------------------------------------------------------
// Same as W2's but VMID 8 in bits 23:20. gfx11 compute still cannot continue past it (W2 finding): the MEC queue halts.
inline bool fault_match(uint32_t status, uint32_t addrLo, uint32_t addrHi, uint64_t va) {
	return status != 0 && (status & n33bo::kStatusRw) && ((status & n33bo::kStatusVmidMask) >> n33bo::kStatusVmidShift) == kVmid &&
	       addrLo == (uint32_t)(va_strip(va) >> 12) && (addrHi & 0xFu) == (uint32_t)((va_strip(va) >> 44) & 0xFu);
}
// Pass = the fault matched, cleared to 0, and the queue halted (W2's fault_pass; gfx11 compute cannot continue).
inline bool fault_pass(bool match, uint32_t statusAfterClear, bool queueHalted) { return n33bo::fault_pass(match, statusAfterClear, queueHalted); }

} // namespace n33vm
