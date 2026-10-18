// n33_native_pure.h: the pure decisions of stage 3 step 7 W3 on Navi 33 - the N48N user-client subset that lets RADV's
// Darwin backend drive our compute queue. These are the shape checks, the QueryInfo fill, the context-id bitmap, the
// GemVa envelope check, the Submit parser (compute only, each IB in a mapped R|X BO of this client) and the fence-BO
// check. No kernel types and no I/O: the kext body (amd/n33_native.cpp, built only with N33=1) calls these and wires
// them to the W1/W2/W2V code (n33CpSubmit/Wait, n33Bo*, n33Vm*). Host-tested by tests/n33_native_test.cpp.
//
// Design: RDNA3/Tasks/navi33-cobalt64-port/stage3-step7-command-design.md, "W3 kext bridge: the N48N subset on the N33
// build" (items 1-4). The VA ranges, VMID 8 and the gfx11 page table come from n33_vm_pure.h; the BOs from
// n33_bo_pure.h; the compute packets from n33_cp_pure.h. The N48N ABI is the one RADV already speaks (Navi48NativeABI.h).
#pragma once
#include <stdint.h>
#include "../Navi48NativeABI.h"
#include "n33_vm_pure.h"
#include "n33_bo_pure.h"
#include "n33_cp_pure.h"

namespace n33n {

// ---- build and identity ------------------------------------------------------------------------------------------
constexpr uint32_t kKextBuild = 675;                 // 0.0.675 (Hello / QueryInfo report it; the host test pins it to Info.plist)
constexpr uint32_t kVmid      = n33vm::kVmid;         // 8: the Darwin backend's and W2V's static VMID
constexpr uint32_t kGcMajor = 11, kGcMinor = 0, kGcRev = 2;   // gfx11.0.2 (Navi 33); (maj<<16)|(min<<8)|rev
constexpr uint32_t kGcVersion = (kGcMajor << 16) | (kGcMinor << 8) | kGcRev;   // 0x0B0002
static_assert(kGcVersion == 0x000B0002u, "gc_version is gfx11.0.2");

// IB engine type (include/uapi/drm/amdgpu_drm.h): GFX 0, COMPUTE 1. W3 runs on the step-6 MEC compute queue, so only
// COMPUTE is accepted; GFX (what the Navi 48 kext used, on its graphics ring) and every other type are refused.
constexpr uint32_t kIpCompute = 1u, kIpGfx = 0u;
// IB flags we tolerate: the "accepted, ignored" ones (Navi48NativeABI.h). Every other flag, including the "Unsupported"
// ones (CE, SECURE, RESET_GDS, EMIT_MEM_SYNC), is refused.
constexpr uint32_t kIbFlagsAllowed = N48N_IB_FLAG_PREAMBLE | N48N_IB_FLAG_PREEMPT | N48N_IB_FLAG_TC_WB_NOT_INVALIDATE;

// The ring submit buffer (n33cp::kSubmitDw dwords, padded to 256) holds, per IB, an ACQUIRE_MEM (8 dw, before EVERY
// INDIRECT_BUFFER - the W2 GL2 lesson) plus an INDIRECT_BUFFER (4 dw), and one RELEASE_MEM (8 dw) at the end. So the
// ring caps the IB count below N48N_MAX_IBS: (kSubmitDw - 8) / 12.
constexpr uint32_t kRingPerIbDw = 8u + 4u, kRingTailDw = 8u;
constexpr uint32_t kMaxRingIbs = (n33cp::kSubmitDw - kRingTailDw) / kRingPerIbDw;   // 20
static_assert(kMaxRingIbs >= 2u && kMaxRingIbs <= N48N_MAX_IBS, "the ring holds at least two IBs and no more than the ABI cap");

// ---- return codes (kept free of IOReturn; the kext body maps them to the IOReturn.h values) -----------------------
enum Code : uint32_t { kOk = 0, kBadArg, kUnsupported, kNotReady, kNoResources, kNotPermitted, kNotFound, kExclusive };

// ---- the native client's own state (the BO/VM state lives in n33bo::State / n33vm::State) -------------------------
struct Client {
	bool    hello;                          // Hello succeeded this open
	bool    hung;                           // a fault or hang latched this client: every further Submit is refused
	uint8_t ctxUsed[N48N_MAX_CTX + 1];      // context ids 1..64 (index 0 unused, as the ABI reserves 0)
};
inline void client_reset(Client &c) { c = Client{}; }

// ---- newUserClient gate (item 1): one client at a time, only after step 6 K1-K3 ran and a W2V VM exists -----------
// cpQueueReady: the step-6 compute queue is up and K2/K3 landed this load. vmLive: n33Vm.live (vm create done).
// legacyBoLive / legacyMapping: a legacy n33ctl BO or VMID-8 mapping already exists (beyond the VM tree). The native
// client and the legacy n33ctl path share the one n33Bo/n33Vm state, so the native open is refused unless that state is
// clear of legacy allocations - the client then owns every BO and mapping it makes, and the legacy actions are refused
// while it is open (checked in n33CallLocked).
inline Code may_open(bool cpQueueReady, bool vmLive, bool alreadyOpen, bool legacyBoLive, bool legacyMapping) {
	if (alreadyOpen) return kExclusive;                 // one N48N client at a time on N33
	if (!cpQueueReady || !vmLive) return kNotReady;
	if (legacyBoLive || legacyMapping) return kExclusive;   // ownership: no legacy BOs / VMID-8 mappings may exist at open
	return kOk;
}

// ---- BoCreate domain -> n33bo kind (item 2 / W4) -----------------------------------------------------------------
// RADV passes GEM domains; VRAM is preferred when both VRAM and GTT are set (a VRAM-with-GTT-fallback BO). A CPU-only
// or unsupported domain is refused. The kext gives VRAM BOs CPU access for the map path.
inline Code domain_to_kind(uint64_t domains, uint32_t *kindOut) {
	if (domains & N48N_GEM_DOMAIN_VRAM) { *kindOut = n33bo::kVram; return kOk; }
	if (domains & N48N_GEM_DOMAIN_GTT)  { *kindOut = n33bo::kGtt;  return kOk; }
	return kBadArg;
}

// ---- Hello (selector 0) ------------------------------------------------------------------------------------------
inline Code hello_check(uint64_t clientAbi, uint64_t flags) {
	if (flags & ~(uint64_t)N48N_HELLO_F_MINOR) return kBadArg;
	if (clientAbi != (uint64_t)N48N_ABI_VERSION) return kUnsupported;
	return kOk;
}
inline void hello_fill(uint64_t flags, uint32_t vmid, uint64_t out[4]) {
	out[0] = (flags & N48N_HELLO_F_MINOR) ? ((uint64_t)N48N_ABI_VERSION | ((uint64_t)N48N_ABI_MINOR << 16)) : (uint64_t)N48N_ABI_VERSION;
	out[1] = kKextBuild; out[2] = N48N_MAX_IBS; out[3] = vmid;
}

// ---- QueryInfo (selector 1) --------------------------------------------------------------------------------------
// The live values the kext reads and hands in; everything else is a constant of this build. The VA ranges come from
// n33_vm_pure.h and already contain the canned navi33 row (ac_darwin_drm.c:163-174; static_assert ranges_contain_row).
struct InfoLive {
	uint32_t gb_addr_config;                       // live RREG32(0x263e): 0x343 on this card
	uint32_t ring_size_dw;                         // step-6 compute queue ring size in dwords
	uint32_t info_flags;                           // N48N_INFO_* (HUNG / S1B_POSITIVE / NATIVE_BOOT), the kext's call
	uint64_t vram_vis_total, vram_vis_free;        // BAR0-visible pool (W2's slab budget)
	uint64_t vram_hi_total, vram_hi_free;          // pool above BAR0 (0 if none)
	uint64_t gtt_cap, gtt_used, gtt_max_bo;        // W2 import limits
	uint64_t pt_cap, pt_used;                      // W2V page-table arena
	uint64_t seq_emitted, seq_retired;             // the compute queue's seqno counters
};
inline bool info_ranges_ok() { return n33vm::ranges_contain_row(); }
// Caller zeroes *o first.
inline void info_fill(n48n_info *o, const InfoLive &L) {
	o->abi_version = N48N_ABI_VERSION; o->kext_build = kKextBuild; o->flags = L.info_flags; o->vmid = kVmid;
	o->gb_addr_config = L.gb_addr_config; o->gc_version = kGcVersion;
	o->ring_size_dw = L.ring_size_dw; o->max_ibs = N48N_MAX_IBS;
	o->vram_vis_total = L.vram_vis_total; o->vram_vis_free = L.vram_vis_free;
	o->vram_hi_total = L.vram_hi_total; o->vram_hi_free = L.vram_hi_free;
	o->gtt_cap = L.gtt_cap; o->gtt_used = L.gtt_used; o->gtt_max_bo = L.gtt_max_bo;
	o->pt_cap = L.pt_cap; o->pt_used = L.pt_used;
	o->seq_emitted = L.seq_emitted; o->seq_retired = L.seq_retired;
	o->va_low_first = n33vm::kInfoLowFirst; o->va_low_last = n33vm::kInfoLowLast;
	o->va_high_first = n33vm::kInfoHighFirst; o->va_high_last = n33vm::kInfoHighLast;
	o->max_bos = N48N_MAX_BOS; o->fence_slots = N48N_FENCE_SLOTS;
	o->reserved[2] = N48N_ABI_MINOR;               // ABI minor, as the gfx12 kext reports it
}

// ---- ReadRegs (selector 2) ---------------------------------------------------------------------------------------
// Root only (enforced by the kext). The allow-list is the registers RADV's Darwin gpu_info needs on gfx11:
//   0x2249 CC_GC_SA_UNIT_DISABLE, 0x226f CC_GC_SHADER_ARRAY_CONFIG, 0x263d CC_RB_BACKEND_DISABLE, 0x263e GB_ADDR_CONFIG.
// A read is allowed only if every dword in [off, off+count) is in the set. (Provisional: RADV's exact ReadRegs calls
// are confirmed by the Mesa backend; widen here if it reads a contiguous block instead.)
inline bool reg_allowed_one(uint64_t dw) {
	return dw == 0x2249u || dw == 0x226fu || dw == 0x263du || dw == 0x263eu;
}
inline Code read_regs_check(uint64_t off, uint64_t count, uint64_t instance) {
	if (count < 1 || count > 16 || instance != 0xFFFFFFFFull) return kBadArg;
	for (uint64_t i = 0; i < count; i++) if (!reg_allowed_one(off + i)) return kBadArg;
	return kOk;
}

// ---- Ctx (selector 6): a plain id bitmap, no priority or scheduling --------------------------------------------
// out overlays n48n_ctx: ALLOC -> ctx_id in dword0; QUERY_STATE2 -> qword0 = 0 (never reset / guilty here).
inline Code ctx_op(Client &c, const n48n_ctx *in, n48n_ctx *out) {
	out->op = 0; out->flags = 0; out->ctx_id = 0; out->priority = 0;
	switch (in->op) {
	case N48N_CTX_OP_ALLOC: {
		for (uint32_t id = 1; id <= N48N_MAX_CTX; id++)
			if (!c.ctxUsed[id]) { c.ctxUsed[id] = 1; out->ctx_id = id; return kOk; }
		return kNoResources;
	}
	case N48N_CTX_OP_FREE:
		if (in->ctx_id < 1 || in->ctx_id > N48N_MAX_CTX || !c.ctxUsed[in->ctx_id]) return kBadArg;
		c.ctxUsed[in->ctx_id] = 0; return kOk;
	case N48N_CTX_OP_QUERY_STATE2:
		if (in->ctx_id < 1 || in->ctx_id > N48N_MAX_CTX || !c.ctxUsed[in->ctx_id]) return kBadArg;
		return kOk;   // out already zero: no reset, not guilty
	default:
		return kBadArg;
	}
}
inline bool ctx_valid(const Client &c, uint32_t id) { return id >= 1 && id <= N48N_MAX_CTX && c.ctxUsed[id]; }

// ---- GemVa (selector 5): the envelope; the VA/alignment/containment is n33vm::gemva_check in the kext ------------
// The R/W/X page bits (3:1) are the same positions n33vm uses, so they pass straight through. The MTYPE field (8:5)
// does NOT: the N48N/amdgpu uapi encoding is DEFAULT 0, NC 1, WC 2, CC 3, UC 4, RW 5, while n33vm (and the gfx11 PTE)
// want the navi10_enum hardware MTYPE NC 0, WC 1, CC 2, UC 3. Translate it; an RW or unknown MTYPE is refused (no gfx11
// hardware MTYPE for it). Returns the operation and the translated n33vm flags; refuses the syncobj/timeline fields.
inline Code gemva_check(const n48n_gem_va *in, uint32_t *opOut, uint32_t *vmFlagsOut) {
	if (in->_pad != 0 || in->vm_timeline_point != 0 || in->vm_timeline_syncobj_out != 0 ||
	    in->num_syncobj_handles != 0 || in->input_fence_syncobj_handles != 0) return kBadArg;
	if (in->operation != N48N_VA_OP_MAP && in->operation != N48N_VA_OP_UNMAP) return kUnsupported;
	*opOut = in->operation;
	const uint32_t rwx = in->flags & (N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE | N48N_VM_PAGE_EXECUTABLE);
	// those three bits are at 3:1 in both encodings:
	const uint32_t n33rwx = ((in->flags & N48N_VM_PAGE_READABLE) ? n33vm::kVmRead : 0u) |
	                        ((in->flags & N48N_VM_PAGE_WRITEABLE) ? n33vm::kVmWrite : 0u) |
	                        ((in->flags & N48N_VM_PAGE_EXECUTABLE) ? n33vm::kVmExec : 0u);
	uint32_t hwMtype;
	switch ((in->flags & N48N_VM_MTYPE_MASK) >> 5) {   // amdgpu uapi MTYPE field
	case 0: case 1: hwMtype = n33vm::kMtypeNc; break;  // DEFAULT and NC both map to the hardware NC
	case 2: hwMtype = n33vm::kMtypeWc; break;
	case 3: hwMtype = n33vm::kMtypeCc; break;
	case 4: hwMtype = n33vm::kMtypeUc; break;
	default: return kBadArg;                            // RW (5) or anything higher: no gfx11 hardware MTYPE
	}
	if (in->operation == N48N_VA_OP_MAP) {
		// no bits outside R/W/X/MTYPE (no PRT, NOALLOC, DELAY_UPDATE); R or W must be present (n33vm::vm_flags_ok).
		if (in->flags & ~(uint32_t)(N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE | N48N_VM_PAGE_EXECUTABLE | N48N_VM_MTYPE_MASK)) return kBadArg;
		if (!(rwx & (N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE))) return kBadArg;
	}
	*vmFlagsOut = n33rwx | (hwMtype << n33vm::kVmMtypeShift);
	return kOk;
}

// ---- the fence BO (Submit's optional RELEASE_MEM target) --------------------------------------------------------
inline uint64_t bo_bytes(const n33bo::State &bo, uint32_t h) { return n33bo::bytes(bo.bo[h - 1]); }
inline Code fence_check(const n33bo::State &bo, uint32_t handle, uint32_t offset) {
	if (!n33bo::handle_ok(bo, handle)) return kBadArg;
	if (offset & 7u) return kBadArg;                                  // 8-aligned (Mesa gives bytes)
	if ((uint64_t)offset + 8ull > bo_bytes(bo, handle)) return kBadArg;
	return kOk;
}

// ---- Submit (selector 7): parse n48n_cs_in, compute only, each IB in a mapped R|X BO of this client -------------
struct SubmitIb { uint64_t va; uint32_t dw; };    // dw = ib_bytes / 4
struct SubmitReq {
	uint32_t numIbs;
	SubmitIb ib[N48N_MAX_IBS];
	bool     hasFence;
	uint32_t fenceHandle, fenceOffset;
	uint32_t ctxId;
};
// `in`/`size` are the structureInput the client already bounded to [32+32, 32+32*N48N_MAX_IBS]. vmLive: a W2V VM exists.
// An IB must lie wholly inside one mapped BO carrying READ and EXECUTE (covering_map), and be dword aligned.
inline Code submit_check(const uint8_t *in, uint32_t size, const n33vm::State &vm, const n33bo::State &bo,
                         bool vmLive, const Client &client, SubmitReq *req) {
	*req = SubmitReq{};
	if (!client.hello) return kNotReady;
	if (client.hung) return kNotReady;               // latched: the kext maps this to kIOReturnAborted
	if (!vmLive) return kNotReady;                   // no VM yet (W2V vm create has not run)
	if (size < 32u + 32u || ((size - 32u) % 32u) != 0u) return kBadArg;
	n48n_cs_in hdr;
	__builtin_memcpy(&hdr, in, sizeof hdr);
	const uint32_t n = (size - 32u) / 32u;
	if (hdr.abi != N48N_ABI_VERSION) return kBadArg;
	if (hdr.num_ibs != n || n < 1u || n > kMaxRingIbs) return kBadArg;   // the ring caps the count (kMaxRingIbs <= N48N_MAX_IBS)
	if (hdr.flags & ~(uint32_t)N48N_CS_HAS_FENCE) return kBadArg;
	if (hdr.reserved != 0ull) return kBadArg;
	if (!ctx_valid(client, hdr.ctx_id)) return kBadArg;
	req->ctxId = hdr.ctx_id;
	req->hasFence = (hdr.flags & N48N_CS_HAS_FENCE) != 0u;
	if (req->hasFence) {
		Code fc = fence_check(bo, hdr.fence_handle, hdr.fence_offset);
		if (fc != kOk) return fc;
		req->fenceHandle = hdr.fence_handle; req->fenceOffset = hdr.fence_offset;
	} else if (hdr.fence_handle != 0u || hdr.fence_offset != 0u) {
		return kBadArg;
	}
	req->numIbs = n;
	for (uint32_t i = 0; i < n; i++) {
		n48n_cs_ib ib;
		__builtin_memcpy(&ib, in + 32u + (size_t)i * 32u, sizeof ib);
		if (ib._pad != 0u) return kBadArg;
		if (ib.flags & ~kIbFlagsAllowed) return kBadArg;
		if (ib.ip_type != kIpCompute) return kBadArg;        // gfx and others refused
		if (ib.ip_instance != 0u || ib.ring != 0u) return kBadArg;
		if (ib.ib_bytes == 0u || (ib.ib_bytes & 3u)) return kBadArg;
		if (ib.va_start & 3u) return kBadArg;
		// the IB must be wholly inside one of this client's VMID-8 mappings, with READ and EXECUTE.
		if (n33vm::covering_map(vm, ib.va_start, ib.ib_bytes, n33vm::kVmRead | n33vm::kVmExec) < 0) return kBadArg;
		req->ib[i].va = ib.va_start; req->ib[i].dw = ib.ib_bytes / 4u;
	}
	return kOk;
}

// ---- the W3 ring content (new stream; W1/W2/W2V streams stay byte-identical) -------------------------------------
// ACQUIRE_MEM before EVERY INDIRECT_BUFFER (the W2 GL2 lesson), one INDIRECT_BUFFER per caller IB in order at its
// VMID-8 VA, then one RELEASE_MEM to the fence MC (the caller's fence BO when given, else the kernel seqno fence in
// host page B, exactly as W2V's ring does). The kext does NOT build or touch the IB bodies (Mesa wrote them into the
// BOs). Returns the dword count written (<= kSubmitDw - the pad tail); the kext pads to kSubmitDw and kicks.
inline uint32_t ring_build(uint32_t *p, const SubmitReq &req, uint64_t fenceMc, uint32_t seq) {
	uint32_t n = 0;
	for (uint32_t i = 0; i < req.numIbs; i++) {
		n += n33cp::pkt_acquire_mem(p + n);
		n += n33cp::pkt_indirect_buffer(p + n, req.ib[i].va, req.ib[i].dw, kVmid);
	}
	n += n33cp::pkt_release_mem(p + n, fenceMc, seq, true);   // GL2_WB before the fence, as every n33 ring fence does
	return n;
}

} // namespace n33n
