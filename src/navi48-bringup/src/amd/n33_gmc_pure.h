// n33_gmc_pure.h: decisions for NBIO 4.3, MMHUB 3.0.2, GFXHUB 3.0 and the GART on Navi 33 (stage 3 step 1,
// G0 to G6 of the overview repo's RDNA3/Tasks/navi33-cobalt64-port/stage3-gmc-design.md, D1 to D10).
// Host-testable: no kernel types, no I/O. The kext reads the live inputs, asks plan() for each step's
// register list with every value computed here, writes it, and keeps the before-values for restore.
// Linux references are to the pinned tree 3857c2fe (mmhub_v3_0_2.c, gfxhub_v3_0.c, gmc_v11_0.c).
#pragma once
#include <stdint.h>
#include "n33_gmc11_fields.h"

namespace n33gmc {

// ---- Registers (absolute dwords) ----------------------------------------------------------------------
// NBIO 4.3 (NBIF segment 2 at 0xd20; nbio_4_3_0_offset.h, base index 2).
constexpr uint32_t kRemapHdpMem    = 0xE4D;   // BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL   0x12d
constexpr uint32_t kRemapHdpReg    = 0xE4E;   // BIF_BX0_REMAP_HDP_REG_FLUSH_CNTL   0x12e
constexpr uint32_t kDoorbellAperEn = 0xDE0;   // RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN 0xc0, bit 0
constexpr uint32_t kHdpFlushDirect = 0xE17;   // BIF_BX_DEV0_EPF0_VF0_HDP_MEM_COHERENCY_FLUSH_CNTL 0xf7
constexpr uint32_t kMemsize        = 0xDE3;   // RCC_DEV0_EPF0_RCC_CONFIG_MEMSIZE 0xc3 (MiB)
constexpr uint32_t kFbEn           = 0xE20;   // BIF_BX0_BIF_FB_EN 0x100 (read only here)
// rmmio_remap.reg_offset on bare metal with 4 KiB pages (nbio_v4_3.c:474-480): the HDP flush registers
// reappear at these BAR5 byte offsets once G1 has run. ADDRESS field mask from nbio_4_3_0_sh_mask.h:897.
constexpr uint32_t kRemapMemVal = 0x7F000, kRemapRegVal = 0x7F004, kRemapAddrMask = 0x0007FFFCu;
constexpr uint32_t kHdpRemapDword = kRemapMemVal / 4;   // 0x1FC00
constexpr uint32_t kDoorbellEnBit = 0x1;

// DCN 3.2 VM registers (segment 2 at 0x34C0; dcn_3_2_0_offset.h:1742-1753, :1830). Read, never written.
constexpr uint32_t kDcnVm[7] = { 0x3935, 0x3936, 0x3937, 0x3938, 0x3939, 0x393A, 0x3A19 };

// GC (segment 0 at 0x1260, segment 1 at 0xa000; gc_11_0_0_offset.h).
constexpr uint32_t kRlcBootload = 0xEE82;           // RLC_RLCS_BOOTLOAD_STATUS 0x4e82, base 1
constexpr uint32_t kBootloadComplete = 0x80000000u;  // BOOTLOAD_COMPLETE, bit 31
constexpr uint32_t kCpDebug = 0x307F;                // CP_DEBUG 0x1e1f
constexpr uint32_t kCpStat  = 0x21A0;                // CP_STAT 0x0f40

// One VM hub. MMHUB: segment 0 at 0x1a000 + mmhub_3_0_2_offset.h. GFXHUB: GC segment 0 at 0x1260 +
// gc_11_0_0_offset.h. Strides as Linux's hub init: ctx_distance 1, ctx_addr_distance 2, eng_distance 1,
// eng_addr_distance 2 (mmhub_v3_0_2.c:486-497, gfxhub_v3_0.c likewise).
struct Hub {
	uint32_t ptBaseLo, ptStartLo, ptEndLo;            // CONTEXT0_PAGE_TABLE_*_LO32; HI32 = LO32 + 1
	uint32_t ctx1PtStartLo, ctx1PtEndLo;              // CONTEXT1; context c (1..15) at + 2 (c - 1)
	uint32_t agpTop, agpBot, agpBase, sysLow, sysHigh, defLsb, defMsb;
	uint32_t faultCntl, faultCntl2, faultStatus, faultDefLo, faultDefHi;
	uint32_t l1Tlb, l2Cntl, l2Cntl2, l2Cntl3, l2Cntl4, l2Cntl5;
	uint32_t ctx0Cntl;                                // context c at + c
	uint32_t identity;                                // the six identity aperture registers from here
	uint32_t invRangeLo;                              // engine e at + 2e; HI32 = LO32 + 1
	uint32_t invSem17, invReq17, invAck17;            // invSem17 0: no semaphore (GFXHUB, gmc_v11_0.c:194-199)
	uint32_t cid2;                                    // 0: no private invalidation (GFXHUB, :295-297)
	uint32_t fbBase, fbTop, fbOffset;
	uint32_t l2Cntl3Default;                          // mmhub_v3_0_2.c:33, gc_11_0_0_default.h:1256
	bool gc;
};
constexpr Hub kMm = {
	0x1A72B, 0x1A74B, 0x1A76B, 0x1A74D, 0x1A76D,
	0x1A86E, 0x1A86F, 0x1A870, 0x1A871, 0x1A872, 0x1A858, 0x1A859,
	0x1A688, 0x1A689, 0x1A68C, 0x1A68F, 0x1A690,
	0x1A873, 0x1A680, 0x1A681, 0x1A682, 0x1A698, 0x1A69E,
	0x1A6C0, 0x1A692, 0x1A707,
	0x1A6E2, 0x1A6F4, 0x1A706, 0x1A69B,
	0x1A86C, 0x1A86D, 0x1A857,
	0x80100007u, false,
};
constexpr Hub kGc = {
	0x2953, 0x2973, 0x2993, 0x2975, 0x2995,
	0x28DA, 0x28DB, 0x28DC, 0x28DD, 0x28DE, 0x2808, 0x2809,
	0x2824, 0x2825, 0x2828, 0x282B, 0x282C,
	0x28DF, 0x281C, 0x281D, 0x281E, 0x2834, 0x283A,
	0x28E8, 0x282E, 0x292F,
	0, 0x291C, 0x292E, 0,
	0x28D8, 0x28D9, 0x2807,
	0x80120007u, true,
};

// ---- Field values (n33_gmc11_fields.h; per hub, though MMHUB 3.0.2 and GC 11 agree) --------------------
struct Values {
	uint32_t l1Mask, l1Val;          // init_tlb_regs
	uint32_t l2Mask, l2Val;          // init_cache_regs, L2_CNTL
	uint32_t l2InvMask;              // L2_CNTL2 INVALIDATE_ALL_L1_TLBS | INVALIDATE_L2_CACHE
	uint32_t l2c3, l2c4, l2c5;       // written whole
	uint32_t ctx0Mask, ctx0Val;      // enable_system_domain
	uint32_t ctx1Mask, ctx1Val;      // setup_vmid_config
	uint32_t faultMask, faultVal;    // set_fault_enable_default(true)
	uint32_t fault2Mask;             // ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY
	uint32_t req;                    // engine request for VMID 0, flush type 0
	uint32_t ptHiBits, agpBits, apBits, defMsbBits, faultDefHiBits, l2c3Bits, l2c4Bits, l2c5Bits, idHiBits, rangeHiBits;
};

constexpr Values values(bool gc) {
	using n33gmcf::set;
#define N33_FS(R, F) (gc ? n33gmcf::GC##R##__##F : n33gmcf::MM##R##__##F)
#define N33_BS(R) (gc ? n33gmcf::GC##R##__BITS : n33gmcf::MM##R##__BITS)
	Values v = {};
	v.l1Mask = N33_FS(MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB).mask | N33_FS(MC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE).mask |
	           N33_FS(MC_VM_MX_L1_TLB_CNTL, SYSTEM_APERTURE_UNMAPPED_ACCESS).mask |
	           N33_FS(MC_VM_MX_L1_TLB_CNTL, ENABLE_ADVANCED_DRIVER_MODEL).mask | N33_FS(MC_VM_MX_L1_TLB_CNTL, ECO_BITS).mask |
	           N33_FS(MC_VM_MX_L1_TLB_CNTL, MTYPE).mask;
	v.l1Val = set(set(set(set(set(set(0, N33_FS(MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB), 1),
	              N33_FS(MC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE), 3),
	              N33_FS(MC_VM_MX_L1_TLB_CNTL, ENABLE_ADVANCED_DRIVER_MODEL), 1),
	              N33_FS(MC_VM_MX_L1_TLB_CNTL, SYSTEM_APERTURE_UNMAPPED_ACCESS), 0),
	              N33_FS(MC_VM_MX_L1_TLB_CNTL, ECO_BITS), 0),
	              N33_FS(MC_VM_MX_L1_TLB_CNTL, MTYPE), 3);                   // MTYPE_UC (navi10_enum.h)
	v.l2Mask = N33_FS(VM_L2_CNTL, ENABLE_L2_CACHE).mask | N33_FS(VM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING).mask |
	           N33_FS(VM_L2_CNTL, ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY).mask |
	           N33_FS(VM_L2_CNTL, L2_PDE0_CACHE_TAG_GENERATION_MODE).mask | N33_FS(VM_L2_CNTL, PDE_FAULT_CLASSIFICATION).mask |
	           N33_FS(VM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE).mask | N33_FS(VM_L2_CNTL, IDENTITY_MODE_FRAGMENT_SIZE).mask;
	v.l2Val = set(set(set(0, N33_FS(VM_L2_CNTL, ENABLE_L2_CACHE), 1),
	              N33_FS(VM_L2_CNTL, ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY), 1),
	              N33_FS(VM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE), 1);
	v.l2InvMask = N33_FS(VM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS).mask | N33_FS(VM_L2_CNTL2, INVALIDATE_L2_CACHE).mask;
	// translate_further is never set on GMC 11: BANK_SELECT 9, BIGK 6.
	v.l2c3 = set(set(gc ? kGc.l2Cntl3Default : kMm.l2Cntl3Default, N33_FS(VM_L2_CNTL3, BANK_SELECT), 9),
	             N33_FS(VM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE), 6);
	v.l2c4 = set(set(0xC1u, N33_FS(VM_L2_CNTL4, VMC_TAP_PDE_REQUEST_PHYSICAL), 0),
	             N33_FS(VM_L2_CNTL4, VMC_TAP_PTE_REQUEST_PHYSICAL), 0);
	v.l2c5 = set(0x3FE0u, N33_FS(VM_L2_CNTL5, L2_CACHE_SMALLK_FRAGMENT_SIZE), 0);
	v.ctx0Mask = N33_FS(VM_CONTEXT0_CNTL, ENABLE_CONTEXT).mask | N33_FS(VM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH).mask |
	             N33_FS(VM_CONTEXT0_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT).mask;
	v.ctx0Val = set(0, N33_FS(VM_CONTEXT0_CNTL, ENABLE_CONTEXT), 1);    // depth 0, retry 0
	const n33gmcf::Field def[7] = {
		N33_FS(VM_CONTEXT1_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT), N33_FS(VM_CONTEXT1_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT),
		N33_FS(VM_CONTEXT1_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT), N33_FS(VM_CONTEXT1_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT),
		N33_FS(VM_CONTEXT1_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT), N33_FS(VM_CONTEXT1_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT),
		N33_FS(VM_CONTEXT1_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT),
	};
	v.ctx1Mask = N33_FS(VM_CONTEXT1_CNTL, ENABLE_CONTEXT).mask | N33_FS(VM_CONTEXT1_CNTL, PAGE_TABLE_DEPTH).mask |
	             N33_FS(VM_CONTEXT1_CNTL, PAGE_TABLE_BLOCK_SIZE).mask | N33_FS(VM_CONTEXT1_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT).mask;
	v.ctx1Val = set(set(0, N33_FS(VM_CONTEXT1_CNTL, ENABLE_CONTEXT), 1), N33_FS(VM_CONTEXT1_CNTL, PAGE_TABLE_DEPTH), 3);
	for (uint32_t i = 0; i < 7; i++) { v.ctx1Mask |= def[i].mask; v.ctx1Val = set(v.ctx1Val, def[i], 1); }
	// PAGE_TABLE_BLOCK_SIZE block_size - 9 = 0; RETRY = !noretry = 0 (amdgpu_gmc.c:1000-1020): left 0.
	const n33gmcf::Field fd[11] = {
		N33_FS(VM_L2_PROTECTION_FAULT_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT), N33_FS(VM_L2_PROTECTION_FAULT_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT),
		N33_FS(VM_L2_PROTECTION_FAULT_CNTL, PDE1_PROTECTION_FAULT_ENABLE_DEFAULT), N33_FS(VM_L2_PROTECTION_FAULT_CNTL, PDE2_PROTECTION_FAULT_ENABLE_DEFAULT),
		N33_FS(VM_L2_PROTECTION_FAULT_CNTL, TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT),
		N33_FS(VM_L2_PROTECTION_FAULT_CNTL, NACK_PROTECTION_FAULT_ENABLE_DEFAULT),
		N33_FS(VM_L2_PROTECTION_FAULT_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT),
		N33_FS(VM_L2_PROTECTION_FAULT_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT), N33_FS(VM_L2_PROTECTION_FAULT_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT),
		N33_FS(VM_L2_PROTECTION_FAULT_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT), N33_FS(VM_L2_PROTECTION_FAULT_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT),
	};
	for (uint32_t i = 0; i < 11; i++) { v.faultMask |= fd[i].mask; v.faultVal = set(v.faultVal, fd[i], 1); }
	// GFXHUB also writes CRASH_ON_NO_RETRY_FAULT and CRASH_ON_RETRY_FAULT = !value = 0 (gfxhub_v3_0.c:449-452);
	// MMHUB touches them only when value is false (mmhub_v3_0_2.c:453-458).
	if (gc) v.faultMask |= N33_FS(VM_L2_PROTECTION_FAULT_CNTL, CRASH_ON_NO_RETRY_FAULT).mask |
	                       N33_FS(VM_L2_PROTECTION_FAULT_CNTL, CRASH_ON_RETRY_FAULT).mask;
	v.fault2Mask = N33_FS(VM_L2_PROTECTION_FAULT_CNTL2, ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY).mask;
	v.req = set(set(set(set(set(set(set(0, N33_FS(VM_INVALIDATE_ENG0_REQ, PER_VMID_INVALIDATE_REQ), 1u << 0),
	            N33_FS(VM_INVALIDATE_ENG0_REQ, FLUSH_TYPE), 0),
	            N33_FS(VM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PTES), 1), N33_FS(VM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE0), 1),
	            N33_FS(VM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE1), 1), N33_FS(VM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE2), 1),
	            N33_FS(VM_INVALIDATE_ENG0_REQ, INVALIDATE_L1_PTES), 1);   // CLEAR_PROTECTION_FAULT_STATUS_ADDR 0
	v.ptHiBits = N33_BS(VM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32);
	v.agpBits = N33_BS(MC_VM_AGP_BASE);
	v.apBits = N33_BS(MC_VM_SYSTEM_APERTURE_LOW_ADDR);
	v.defMsbBits = N33_BS(MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB);
	v.faultDefHiBits = N33_BS(VM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32);
	v.l2c3Bits = N33_BS(VM_L2_CNTL3);
	v.l2c4Bits = N33_BS(VM_L2_CNTL4);
	v.l2c5Bits = N33_BS(VM_L2_CNTL5);
	v.idHiBits = N33_BS(VM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32);
	v.rangeHiBits = N33_BS(VM_INVALIDATE_ENG0_ADDR_RANGE_HI32);
#undef N33_FS
#undef N33_BS
	return v;
}
constexpr Values kMmV = values(false);
constexpr Values kGcV = values(true);

// ---- Geometry (D2, D3, D8) ----------------------------------------------------------------------------
constexpr uint64_t kPage       = 0x1000;
constexpr uint64_t kGartMc     = 0x8400000000ull;    // D2: 256 MiB, 4 GiB aligned, above fb_end
constexpr uint64_t kGartSize   = 256ull << 20;
constexpr uint32_t kGartPages  = (uint32_t)(kGartSize / kPage);   // 65536
constexpr uint64_t kTableOff   = 0x8800000;          // D3: VRAM offset of the PTE table, 512 KiB
constexpr uint64_t kTableLen   = (uint64_t)kGartPages * 8;
constexpr uint64_t kScratchOff = 0x8880000;          // D3: mem_scratch, the system aperture default page
constexpr uint64_t kScratchLen = kPage;
constexpr uint64_t kOwnedOff   = kTableOff;          // both join n33_protected as kind 14
constexpr uint64_t kOwnedLen   = kScratchOff + kScratchLen - kTableOff;
constexpr uint64_t kHole       = 0x0000800000000000ull;   // AMDGPU_GMC_HOLE_START with 48-bit VA
constexpr uint64_t kMaxPfnM1   = (1ull << 36) - 1;   // max_pfn - 1 for 256 TiB of VA (vm size 262144 GB)
constexpr uint32_t kHostPages  = 4;                  // G5's kext pages
constexpr uint32_t kMapMax     = 16;                 // pages per gart map / unmap call
constexpr uint64_t kDmaLimit   = 1ull << 44;         // gmc_v11_0.c:826, DMA mask 44 bits
constexpr uint64_t kDmaPhysMask = (kDmaLimit - 1) & ~(kPage - 1);   // inTaskWithPhysicalMask's mask

// PTEs (gmc_v11_0.c:413-443; amdgpu_vm.h:57-118). No bit 63 (GFX12's IS_PTE); MTYPE at 48..50, UC = 3.
constexpr uint64_t kPteAddrMask  = 0x0000FFFFFFFFF000ull;
constexpr uint64_t kPteValid = 1ull << 0, kPteSystem = 1ull << 1, kPteSnooped = 1ull << 2, kPteExec = 1ull << 4,
                   kPteRead = 1ull << 5, kPteWrite = 1ull << 6, kPteMtypeUc = 3ull << 48;
constexpr uint64_t kPteFlagsSys  = kPteValid | kPteSystem | kPteSnooped | kPteExec | kPteRead | kPteWrite | kPteMtypeUc;
constexpr uint64_t kPteFlagsVram = kPteValid | kPteExec | kPteRead | kPteWrite | kPteMtypeUc;   // amdgpu_gart_map_vram_range
inline uint64_t pte_sys(uint64_t pa) { return (pa & kPteAddrMask) | kPteFlagsSys; }
inline uint64_t pte_vram(uint64_t vramPa) { return (vramPa & kPteAddrMask) | kPteFlagsVram; }
inline uint64_t pte_unbound(uint64_t dummyPa) { return dummyPa & kPteAddrMask; }   // flags 0 (amdgpu_gart.c:308-339)
inline uint64_t slot_mc(uint32_t slot) { return kGartMc + (uint64_t)slot * kPage; }
inline uint64_t slot_off(uint32_t slot) { return kTableOff + (uint64_t)slot * 8; }
// A kext host page: page aligned, non-zero, wholly below 44 bits (D8).
inline bool host_pa_ok(uint64_t pa) { return pa != 0 && (pa & (kPage - 1)) == 0 && pa < kDmaLimit; }

// ---- Refusal reasons. They continue the kext's list after the SMU's (23..29). -----------------------------
enum Why : uint32_t {
	kWhyOk = 0,
	kWhyArg = 30,          // not a step number, a bad count or kind, or a misaligned VRAM offset
	kWhyOrder = 31,        // out of order, twice, no G0 snapshot, no table yet, or G4 not done for a flush
	kWhyChanged = 32,      // FB location, DCN_VM, HUBP0 or the GC FB location differ from the G0 snapshot
	kWhyFault = 33,        // a protection fault status is non-zero before the step
	kWhyAperture = 34,     // D4: the system aperture would not cover [fb_start, fb_end]
	kWhyApertureGart = 35, // D4: the system aperture would reach the GART range
	kWhyGeometry = 36,     // live FB location or MEMSIZE inconsistent with the D2/D3 placement
	kWhyGcDown = 37,       // G6: m2 autoload not done in this load, or BOOTLOAD_COMPLETE clear
	kWhyRange = 38,        // GART slot range outside the GART
	kWhyTable = 39,        // the table / mem_scratch range is not writable (protected) when taken
	kWhyHostPage = 40,     // dummy or host page allocation failed, or above 44 bits
	kWhyStopped = 41,      // an earlier step or flush failed, or the state was restored: reload the kext
	kWhyEngine = 42,       // restore refused: an engine holds a GART address (D6)
};

// ---- Inputs and geometry --------------------------------------------------------------------------------
// The live values the kext reads (at G0, and for G6 the GC aperture at step time) plus the dummy page.
struct Inputs {
	uint32_t fbBase, fbTop, fbOffset;   // the hub's MC_VM_FB_LOCATION_BASE / TOP, MC_VM_FB_OFFSET registers
	uint32_t memsize;                   // RCC_CONFIG_MEMSIZE, MiB
	uint32_t sysLow, sysHigh;           // the system aperture as found (the VBIOS's, for D4)
	uint64_t dummyPa;                   // 0 until G2 allocates it
};

struct Geo {
	uint64_t fbStart, fbEnd, vramBaseOffset, gartStart, gartEnd;
	uint64_t pdBase;      // CONTEXT0_PAGE_TABLE_BASE: the table's VRAM physical address with VALID
	uint64_t scratchPa;   // SYSTEM_APERTURE_DEFAULT_ADDR: mem_scratch's VRAM physical address
};

// vram_gtt_location and gart_init for our placement (gmc_v11_0.c:652-738), plus the checks that make D2/D3
// valid on the live card.
inline uint32_t geometry(const Inputs &in, Geo *g) {
	*g = {};
	const uint64_t base = in.fbBase & n33gmcf::MMMC_VM_FB_LOCATION_BASE__FB_BASE.mask;
	const uint64_t top = in.fbTop & n33gmcf::MMMC_VM_FB_LOCATION_TOP__FB_TOP.mask;
	if (in.memsize == 0 || top < base) return kWhyGeometry;
	g->fbStart = base << 24;
	const uint64_t topEnd = ((top + 1) << 24) - 1;
	g->fbEnd = g->fbStart + ((uint64_t)in.memsize << 20) - 1;
	g->vramBaseOffset = (uint64_t)(in.fbOffset & 0x00FFFFFFu) << 24;
	g->gartStart = kGartMc;
	g->gartEnd = kGartMc + kGartSize - 1;
	g->pdBase = (kTableOff + g->vramBaseOffset) | 1;   // amdgpu_gmc_pd_addr: VRAM physical, VALID
	g->scratchPa = kScratchOff + g->vramBaseOffset;      // amdgpu_gmc_vram_mc2pa
	if (g->fbEnd > topEnd) return kWhyGeometry;                         // MEMSIZE beyond the FB window
	if (g->gartStart <= topEnd || (g->gartStart & 0xFFFFFFFFull) || g->gartEnd >= kHole) return kWhyGeometry;
	if (kScratchOff + kScratchLen > ((uint64_t)in.memsize << 20)) return kWhyGeometry;
	return kWhyOk;
}

// D4. The aperture covers [low << 18, (high << 18) | 0x3FFFF].
inline bool aperture_covers(uint32_t low, uint32_t high, uint64_t a, uint64_t b) {
	return low <= high && ((uint64_t)low << 18) <= a && (((uint64_t)high << 18) | 0x3FFFF) >= b;
}
inline bool aperture_hits(uint32_t low, uint32_t high, uint64_t a, uint64_t b) {
	return low <= high && ((uint64_t)low << 18) <= b && (((uint64_t)high << 18) | 0x3FFFF) >= a;
}
// LOW = min(found LOW, Linux's), HIGH = max(found HIGH, Linux's) when the found aperture is enabled
// (LOW <= HIGH), Linux's otherwise. Linux's: min(fb_start, agp_start) >> 18, max(fb_end, agp_end) >> 18
// with AGP disabled (agp_start 0xFFFFFFFFFFFF, agp_end 0).
inline uint32_t aperture(const Inputs &in, const Geo &g, uint32_t *low, uint32_t *high) {
	const uint32_t bits = kMmV.apBits;
	const uint32_t linLow = (uint32_t)(g.fbStart >> 18) & bits, linHigh = (uint32_t)(g.fbEnd >> 18) & bits;
	const uint32_t vl = in.sysLow & bits, vh = in.sysHigh & bits;
	*low = linLow; *high = linHigh;
	if (vl <= vh) { *low = vl < linLow ? vl : linLow; *high = vh > linHigh ? vh : linHigh; }
	if (!aperture_covers(*low, *high, g.fbStart, g.fbEnd)) return kWhyAperture;
	if (aperture_hits(*low, *high, g.gartStart, g.gartEnd)) return kWhyApertureGart;
	return kWhyOk;
}

// ---- Step plans ---------------------------------------------------------------------------------------
enum Op : uint32_t { kWrite = 0, kHdpFlush = 1, kFlushMm = 2, kFlushGc = 3 };
// One plan entry. A write sets the bits in mask to value and keeps the rest (read-modify-write; mask
// 0xFFFFFFFF writes the register whole). cmp is what the read-back must match: the fields written, or the
// register's defined bits, or 0 for self-clearing bits that are only recorded.
struct Entry { uint32_t op, reg, mask, value, cmp; };
inline uint32_t target(const Entry &e, uint32_t before) { return (before & ~e.mask) | (e.value & e.mask); }

constexpr uint32_t kPlanMax = 160;
constexpr uint32_t kJournalMax = 320;
constexpr uint32_t kSteps = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 6);
inline bool valid_step(uint32_t n) { return n < 32 && ((kSteps >> n) & 1u); }

struct PlanOut {
	Entry *e; uint32_t max, n; bool over;
	void add(uint32_t op, uint32_t reg, uint32_t mask, uint32_t value, uint32_t cmp) {
		if (n < max) e[n++] = { op, reg, mask, value, cmp }; else over = true;
	}
	void w(uint32_t reg, uint32_t value, uint32_t bits) { add(kWrite, reg, 0xFFFFFFFFu, value, bits); }   // whole register
	void rmw(uint32_t reg, uint32_t mask, uint32_t value) { add(kWrite, reg, mask, value, mask); }
};

// mmhub_v3_0_2_gart_enable / gfxhub_v3_0_gart_enable, rows 1 to 8 of the design's MMHUB table.
inline void hub_rows(PlanOut &p, const Hub &h, const Values &v, const Geo &g, uint32_t low, uint32_t high, uint64_t dummyPa,
                     uint32_t fromRow, uint32_t toRow) {
	const uint32_t full = 0xFFFFFFFFu;
	for (uint32_t row = fromRow; row <= toRow; row++) switch (row) {
	case 1:   // init_gart_aperture_regs
		p.w(h.ptBaseLo, (uint32_t)g.pdBase, full);         p.w(h.ptBaseLo + 1, (uint32_t)(g.pdBase >> 32), full);
		p.w(h.ptStartLo, (uint32_t)(g.gartStart >> 12), full); p.w(h.ptStartLo + 1, (uint32_t)(g.gartStart >> 44), v.ptHiBits);
		p.w(h.ptEndLo, (uint32_t)(g.gartEnd >> 12), full);     p.w(h.ptEndLo + 1, (uint32_t)(g.gartEnd >> 44), v.ptHiBits);
		break;
	case 2:   // init_system_aperture_regs
		p.w(h.agpBase, 0, v.agpBits);
		p.w(h.agpBot, (uint32_t)(0xFFFFFFFFFFFFull >> 24), v.agpBits);   // agp_start >> 24
		p.w(h.agpTop, 0, v.agpBits);                                      // agp_end >> 24
		p.w(h.sysLow, low, v.apBits);
		p.w(h.sysHigh, high, v.apBits);
		p.w(h.defLsb, (uint32_t)(g.scratchPa >> 12), full);
		p.w(h.defMsb, (uint32_t)(g.scratchPa >> 44), v.defMsbBits);
		p.w(h.faultDefLo, (uint32_t)(dummyPa >> 12), full);
		p.w(h.faultDefHi, (uint32_t)(dummyPa >> 44), v.faultDefHiBits);
		p.rmw(h.faultCntl2, v.fault2Mask, v.fault2Mask);
		break;
	case 3:   // init_tlb_regs
		p.rmw(h.l1Tlb, v.l1Mask, v.l1Val);
		break;
	case 4:   // init_cache_regs. The L2_CNTL2 invalidate bits may self-clear: recorded, not compared.
		p.rmw(h.l2Cntl, v.l2Mask, v.l2Val);
		p.add(kWrite, h.l2Cntl2, v.l2InvMask, v.l2InvMask, 0);
		p.w(h.l2Cntl3, v.l2c3, v.l2c3Bits);
		p.w(h.l2Cntl4, v.l2c4, v.l2c4Bits);
		p.w(h.l2Cntl5, v.l2c5, v.l2c5Bits);
		break;
	case 5:   // enable_system_domain
		p.rmw(h.ctx0Cntl, v.ctx0Mask, v.ctx0Val);
		break;
	case 6:   // disable_identity_aperture
		p.w(h.identity + 0, 0xFFFFFFFFu, full); p.w(h.identity + 1, 0xF, v.idHiBits);
		p.w(h.identity + 2, 0, full);          p.w(h.identity + 3, 0, v.idHiBits);
		p.w(h.identity + 4, 0, full);          p.w(h.identity + 5, 0, v.idHiBits);
		break;
	case 7:   // setup_vmid_config, contexts 1 to 15
		for (uint32_t i = 0; i < 15; i++) {
			p.rmw(h.ctx0Cntl + 1 + i, v.ctx1Mask, v.ctx1Val);
			p.w(h.ctx1PtStartLo + 2 * i, 0, full);
			p.w(h.ctx1PtStartLo + 2 * i + 1, 0, v.ptHiBits);
			p.w(h.ctx1PtEndLo + 2 * i, (uint32_t)(kMaxPfnM1 & 0xFFFFFFFFu), full);
			p.w(h.ctx1PtEndLo + 2 * i + 1, (uint32_t)(kMaxPfnM1 >> 32), v.ptHiBits);
		}
		break;
	case 8:   // program_invalidation, engines 0 to 17
		for (uint32_t i = 0; i < 18; i++) {
			p.w(h.invRangeLo + 2 * i, 0xFFFFFFFFu, full);
			p.w(h.invRangeLo + 2 * i + 1, 0x1F, v.rangeHiBits);
		}
		break;
	}
}

// The register list of step n (1, 2, 3, 4 or 6) for these inputs, in Linux's order. G2's table fill and
// mem_scratch zeroing come before its list and are the kext's (it owns the pages); everything else is here.
inline uint32_t plan(uint32_t n, const Inputs &in, Entry *out, uint32_t max, uint32_t *count) {
	*count = 0;
	if (!valid_step(n)) return kWhyArg;
	PlanOut p = { out, max, 0, false };
	if (n == 1) {   // D9: remap and doorbell aperture only (soc21_common_hw_init without ASPM)
		p.add(kWrite, kRemapHdpMem, 0xFFFFFFFFu, kRemapMemVal, kRemapAddrMask);
		p.add(kWrite, kRemapHdpReg, 0xFFFFFFFFu, kRemapRegVal, kRemapAddrMask);
		p.rmw(kDoorbellAperEn, kDoorbellEnBit, kDoorbellEnBit);
	} else {
		Geo g;
		uint32_t w = geometry(in, &g);
		if (w) return w;
		uint32_t low = 0, high = 0;
		if (n == 2 || n == 6) { w = aperture(in, g, &low, &high); if (w) return w; }
		const Hub &h = n == 6 ? kGc : kMm;
		const Values &v = n == 6 ? kGcV : kMmV;
		if (n == 2) hub_rows(p, h, v, g, low, high, in.dummyPa, 1, 2);
		if (n == 3) hub_rows(p, h, v, g, low, high, in.dummyPa, 3, 4);
		if (n == 4) hub_rows(p, h, v, g, low, high, in.dummyPa, 5, 8);
		if (n == 6) hub_rows(p, h, v, g, low, high, in.dummyPa, 1, 8);
		if (n == 4 || n == 6) {
			p.add(kHdpFlush, 0, 0, 0, 0);                                   // gart_enable's HDP flush
			if (n == 6) p.rmw(kCpDebug, n33gmcf::CP_DEBUG__CPG_UTCL1_ERROR_HALT_DISABLE.mask,
			                  n33gmcf::CP_DEBUG__CPG_UTCL1_ERROR_HALT_DISABLE.mask);
			p.rmw(h.faultCntl, v.faultMask, v.faultVal);                   // set_fault_enable_default(true)
			p.add(n == 6 ? kFlushGc : kFlushMm, 0, 0, 0, 0);                // flush_gpu_tlb(vmid 0)
		}
	}
	if (p.over) return kWhyArg;
	*count = p.n;
	return kWhyOk;
}

// ---- Step order (D5) ----------------------------------------------------------------------------------
struct State {
	bool g0;            // the G0 snapshot was taken in this load
	uint32_t done;      // bit n: step n succeeded in this load
	uint32_t attempted; // bit n: step n began writing in this load
	bool tableOwned;    // G2 took the table and mem_scratch (protected, kind 14)
	bool stopped;       // a step or flush failed, or a restore ran: nothing more until reload
	bool restored;
	bool engineHolds;   // an engine was given a GART address. Set by stage 3 step 5's first consumer; nothing sets it yet.
};
inline uint32_t prev_step(uint32_t n) { return n == 6 ? 4 : n - 1; }

// May step n run now? gcUp is the kext's G6 gate (m2 autoload succeeded in this load and BOOTLOAD_COMPLETE
// set). Checks run in the order argument, GC gate, state, so the negatives get a stable reason.
inline uint32_t may_step(const State &s, uint32_t n, bool gcUp) {
	if (!valid_step(n)) return kWhyArg;
	if (n == 6 && !gcUp) return kWhyGcDown;
	if (s.stopped) return kWhyStopped;
	if (!s.g0 || (s.done >> n & 1u)) return kWhyOrder;
	if (n != 1 && !(s.done >> prev_step(n) & 1u)) return kWhyOrder;
	return kWhyOk;
}
// G0 is retaken only while nothing has been written in this load.
inline bool snapshot_fresh(const State &s) { return !s.attempted && !s.tableOwned && !s.stopped; }
inline void record_step(State &s, uint32_t n, bool ok) {
	if (ok) s.done |= 1u << n; else s.stopped = true;
}

// What must not change between G0 and every step.
struct Snap {
	uint32_t fbBase, fbTop, fbOffset;   // MMHUB
	uint32_t dcn[7];                    // kDcnVm
	uint32_t hubpLo, hubpHi, pitch;     // HUBP0 surface address and pitch
};
inline uint32_t preconditions(const Snap &g0, const Snap &now, uint32_t faultStatus) {
	if (now.fbBase != g0.fbBase || now.fbTop != g0.fbTop || now.fbOffset != g0.fbOffset) return kWhyChanged;
	for (uint32_t i = 0; i < 7; i++) if (now.dcn[i] != g0.dcn[i]) return kWhyChanged;
	if (now.hubpLo != g0.hubpLo || now.hubpHi != g0.hubpHi || now.pitch != g0.pitch) return kWhyChanged;
	if ((now.dcn[0] & 0x00FFFFFFu) != (now.fbBase & 0x00FFFFFFu)) return kWhyChanged;   // DCN FB base = MMHUB FB base
	if (faultStatus != 0) return kWhyFault;
	return kWhyOk;
}

// ---- GART map, unmap, flush ---------------------------------------------------------------------------
enum MapKind : uint32_t { kMapHost = 0, kMapVram = 1 };
// Argument checks, before any state: count, slot range, then the source. For kMapHost arg is the first
// kext host page index; for kMapVram it is a VRAM offset the kext checks with n33WriteAllowed next.
inline uint32_t map_args(uint32_t slot, uint32_t count, uint32_t kind, uint64_t arg) {
	if (count == 0 || count > kMapMax) return kWhyArg;
	if (slot >= kGartPages || count > kGartPages - slot) return kWhyRange;
	if (kind == kMapHost) return (arg < kHostPages && count <= kHostPages - arg) ? kWhyOk : kWhyArg;
	if (kind == kMapVram) return (arg & (kPage - 1)) ? kWhyArg : kWhyOk;
	return kWhyArg;
}
inline uint32_t unmap_args(uint32_t slot, uint32_t count) {
	if (count == 0 || count > kMapMax) return kWhyArg;
	if (slot >= kGartPages || count > kGartPages - slot) return kWhyRange;
	return kWhyOk;
}
inline uint32_t map_state(const State &s) {
	if (s.stopped) return kWhyStopped;
	return s.tableOwned ? kWhyOk : kWhyOrder;
}
inline uint32_t may_flush(const State &s) {
	if (s.stopped) return kWhyStopped;
	return (s.done >> 4 & 1u) ? kWhyOk : kWhyOrder;
}
// flush_gpu_tlb's outcome bits: the semaphore read 1, ENG17_ACK bit 0 set, the CID2 private invalidation
// written. MMHUB needs all three, GFXHUB only the ack.
enum FlushBits : uint32_t { kFlushSem = 1, kFlushAck = 2, kFlushCid2 = 4 };
inline bool flush_ok(bool gc, uint32_t bits) {
	return gc ? (bits & kFlushAck) != 0 : (bits & (kFlushSem | kFlushAck | kFlushCid2)) == (kFlushSem | kFlushAck | kFlushCid2);
}

// ---- Journal, restore and unload (D6) -----------------------------------------------------------------
struct Rec { uint32_t step, reg, before, after, target, cmp; };
inline bool mismatch(const Rec &r) { return ((r.after ^ r.target) & r.cmp) != 0; }

// gmc restore writes every journalled before-value back, newest first, and is refused once an engine holds
// a GART address. GC entries need GC still up.
inline uint32_t may_restore(const State &s, uint32_t journalN, bool hasGc, bool gcUp) {
	if (s.engineHolds) return kWhyEngine;
	if (s.restored || journalN == 0) return kWhyOrder;
	if (hasGc && !gcUp) return kWhyGcDown;
	return kWhyOk;
}
inline uint32_t restore_index(uint32_t i, uint32_t journalN) { return journalN - 1 - i; }
inline void record_restore(State &s) { s.restored = true; s.stopped = true; s.done = 0; }

// At unload the hubs are left as they are (like the TMR). Every PTE goes back to the dummy page with flags
// 0 and each hub that was given the table is flushed. The dummy page is leaked always (the fault default
// and the unbound PTEs keep pointing at it); the host pages are freed only when no engine holds a GART
// address and every needed flush acknowledged, else leaked too.
struct Unload { bool unmapAll, flushMm, flushGc; };
inline Unload unload_plan(const State &s) {
	return { s.tableOwned, s.tableOwned && (s.attempted >> 4 & 1u) != 0, s.tableOwned && (s.attempted >> 6 & 1u) != 0 };
}
inline bool free_host_pages(const State &s, bool flushesOk) { return !s.engineHolds && flushesOk; }

} // namespace n33gmc
