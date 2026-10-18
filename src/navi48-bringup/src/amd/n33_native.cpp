//
//  n33_native.cpp - stage 3 step 7 W3: the N48N native user-client subset on the N33 build, as Navi48Bringup::n33n*
//  methods. Built ONLY with N33=1 (the whole file is empty otherwise, since the Makefile wildcard compiles it in
//  every build). The pure decisions are amd/n33_native_pure.h; here they are wired to the W1/W2/W2V kernel code
//  (n33BoCreate/Import/Free, n33VmMap/Unmap, the step-6 compute queue through a new caller-IB submit). The client
//  Navi48NativeClient.cpp dispatches to these when n33IsObserving(). One client at a time (the open gate is n33nOpen).
//
//  The submit path keeps tonight's three hardware lessons (the ring kick): the compute queue is a privileged KMD queue
//  (K1), ACQUIRE_MEM sits before every INDIRECT_BUFFER (ring_build), and a VM fault halts the queue (K6 + cp start
//  recovers it). For a VMID-8 submit the RELEASE_MEM fence sits in the ring in VMID 0, in host page B, exactly as W2V.
//
#if defined(N33_OBSERVE)
#include "Navi48Bringup.hpp"
#include "amd/n33_native_pure.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>   // n33GmcHost[] is IOBufferMemoryDescriptor* (getBytesNoCopy)
#include <string.h>

#define N33NLOG(fmt, ...) N48LOG("n33 w3: " fmt, ##__VA_ARGS__)

// The pure header's codes -> the IOReturn.h values the client returns to userspace.
static IOReturn n33n_io(n33n::Code c) {
	switch (c) {
	case n33n::kOk:          return kIOReturnSuccess;
	case n33n::kBadArg:      return kIOReturnBadArgument;
	case n33n::kUnsupported: return kIOReturnUnsupported;
	case n33n::kNotReady:    return kIOReturnNotReady;
	case n33n::kNoResources: return kIOReturnNoResources;
	case n33n::kNotPermitted:return kIOReturnNotPermitted;
	case n33n::kNotFound:    return kIOReturnNotFound;
	case n33n::kExclusive:   return kIOReturnExclusiveAccess;
	}
	return kIOReturnError;
}
// The n33bo / n33vm / n33cp kernel "why" codes -> IOReturn (the BO / VM / submit methods return their own codes).
static IOReturn why_io(uint32_t w) {
	if (w == 0) return kIOReturnSuccess;
	if (w == n33bo::kWhyArg || w == n33vm::kCodeArg || w == n33vm::kCodeStream) return kIOReturnBadArgument;
	if (w == n33bo::kWhyFull || w == n33bo::kWhyAlloc) return kIOReturnNoMemory;
	return kIOReturnNotReady;   // order / GMC not ready / fault / gart / doorbell
}

IOReturn Navi48Bringup::n33nOpen(uint32_t *idOut) {
	// The gate: the step-6 compute queue is running (K1-K3) and a W2V VM exists, and no client is already open.
	const n33n::Code c = n33n::may_open(n33Cp.running, n33Vm.live, n33NativeOpen);
	if (c != n33n::kOk) {
		N33NLOG("open refused: cp running %u, vm live %u, already open %u", n33Cp.running, n33Vm.live, n33NativeOpen);
		return n33n_io(c);
	}
	n33n::client_reset(n33NativeClient);
	n33NativeOpen = true;
	n33NativeId++;
	if (idOut) *idOut = n33NativeId;
	N33NLOG("open: N48N client id %u (VMID %u, queue running, VM live)", n33NativeId, n33n::kVmid);
	return kIOReturnSuccess;
}

void Navi48Bringup::n33nClose() {
	if (!n33NativeOpen) return;
	// Item 3 teardown: unmap every VMID-8 mapping, then free every BO (one client at a time, so these are its own).
	uint64_t tmp[4];
	for (uint32_t i = 0; i < n33vm::kMapMax; i++)
		if (n33Vm.live && n33Vm.map[i].used) (void)n33VmUnmap(n33Vm.map[i].handle, n33Vm.map[i].va, tmp, 4);
	for (uint32_t h = 1; h <= n33bo::kBoMax; h++)
		if (n33bo::handle_ok(n33Bo, h)) (void)n33BoFree(h);
	n33n::client_reset(n33NativeClient);
	n33NativeOpen = false;
	N33NLOG("close: client torn down (mappings unmapped, BOs freed)");
}

IOReturn Navi48Bringup::n33nHello(uint64_t clientAbi, uint64_t flags, uint64_t out[4]) {
	if (!n33NativeOpen) return kIOReturnNotReady;
	const n33n::Code c = n33n::hello_check(clientAbi, flags);
	if (c != n33n::kOk) return n33n_io(c);
	n33NativeClient.hello = true;
	n33n::hello_fill(flags, n33n::kVmid, out);
	N33NLOG("hello: client ABI %llu flags %#llx -> ABI %u.%u build %u VMID %u", clientAbi, flags,
	        (unsigned)N48N_ABI_VERSION, (unsigned)N48N_ABI_MINOR, n33n::kKextBuild, n33n::kVmid);
	return kIOReturnSuccess;
}

IOReturn Navi48Bringup::n33nQueryInfo(n48n_info *out) {
	if (!n33NativeOpen || !n33NativeClient.hello) return kIOReturnNotReady;
	memset(out, 0, sizeof(*out));
	n33n::InfoLive L {};
	L.gb_addr_config = rmmio ? rmmio[0x263e] : 0x343u;   // GB_ADDR_CONFIG, read live (0x343 on this card)
	L.ring_size_dw = n33cp::kRingDw;
	L.info_flags = N48N_INFO_NATIVE_BOOT;
	L.vram_vis_total = n33bo::kSlabLen;
	uint64_t held = 0;
	for (uint32_t h = 1; h <= n33bo::kBoMax; h++)
		if (n33bo::handle_ok(n33Bo, h) && n33Bo.bo[h - 1].kind == n33bo::kVram) held += n33bo::bytes(n33Bo.bo[h - 1]);
	L.vram_vis_free = n33bo::kSlabLen > held ? n33bo::kSlabLen - held : 0;
	L.gtt_cap = n33bo::kHostCap; L.gtt_used = n33Bo.hostBytes; L.gtt_max_bo = n33bo::kBoMaxBytes;
	L.pt_cap = n33vm::kArenaLen; L.pt_used = (uint64_t)n33Vm.nextPage * n33gmc::kPage;
	L.seq_emitted = n33Cp.seq; L.seq_retired = n33Cp.seq;
	n33n::info_fill(out, L);
	return kIOReturnSuccess;
}

IOReturn Navi48Bringup::n33nReadRegs(uint64_t off, uint64_t count, uint64_t instance, uint32_t *out) {
	if (!n33NativeOpen || !n33NativeClient.hello) return kIOReturnNotReady;
	const n33n::Code c = n33n::read_regs_check(off, count, instance);
	if (c != n33n::kOk) return n33n_io(c);
	if (!rmmio) return kIOReturnNotReady;
	for (uint64_t i = 0; i < count; i++) out[i] = rmmio[off + i];
	return kIOReturnSuccess;
}

IOReturn Navi48Bringup::n33nBoCreate(const n48n_gem_create_in *in, uint64_t out[4]) {
	if (!n33NativeOpen || !n33NativeClient.hello) return kIOReturnNotReady;
	uint32_t kind = 0;
	const n33n::Code c = n33n::domain_to_kind(in->domains, &kind);
	if (c != n33n::kOk) return n33n_io(c);
	uint64_t bo[8] = {};
	const uint32_t w = n33BoCreate(kind, in->bo_size, bo, 8);   // bo[1] handle, bo[2] pages
	if (w) return why_io(w);
	const uint32_t h = (uint32_t)bo[1];
	out[0] = h;
	out[1] = (uint64_t)n33Bo.bo[h - 1].pages * n33gmc::kPage;
	out[2] = (kind == n33bo::kVram) ? N48N_GEM_DOMAIN_VRAM : N48N_GEM_DOMAIN_GTT;
	out[3] = N48N_PLACED_CPU_MAPPABLE | N48N_PLACED_ZEROED;   // both VRAM (BAR0 window) and GTT are CPU-mappable
	return kIOReturnSuccess;
}

IOReturn Navi48Bringup::n33nBoFree(uint32_t handle) {
	if (!n33NativeOpen || !n33NativeClient.hello) return kIOReturnNotReady;
	return why_io(n33BoFree(handle));
}

IOReturn Navi48Bringup::n33nBoImportHost(task_t importTask, uint64_t hostVa, uint64_t size, uint64_t flags, uint64_t gpuVa, uint64_t out[4]) {
	if (!n33NativeOpen || !n33NativeClient.hello) return kIOReturnNotReady;
	(void)flags;
	if (gpuVa != 0) {   // RADV's Darwin backend imports handle-only and maps later with GemVa; map-at-import is not served on N33 yet
		N33NLOG("bo import refused: map-at-import (gpuVa != 0) is not supported on N33; import handle-only and map with GemVa");
		return kIOReturnUnsupported;
	}
	uint64_t bo[8] = {};
	const uint32_t w = n33BoImport(importTask, hostVa, size, bo, 8);
	if (w) return why_io(w);
	const uint32_t h = (uint32_t)bo[1];
	out[0] = h;
	out[1] = (uint64_t)n33Bo.bo[h - 1].pages * n33gmc::kPage;
	out[2] = 0;   // not mapped at import
	out[3] = N48N_PLACED_HOST_IMPORT;
	return kIOReturnSuccess;
}

IOReturn Navi48Bringup::n33nGemVa(const n48n_gem_va *in) {
	if (!n33NativeOpen || !n33NativeClient.hello) return kIOReturnNotReady;
	uint32_t op = 0, vmFlags = 0;
	const n33n::Code c = n33n::gemva_check(in, &op, &vmFlags);
	if (c != n33n::kOk) return n33n_io(c);
	uint64_t o[4] = {};
	const uint32_t w = (op == N48N_VA_OP_MAP)
	    ? n33VmMap(in->handle, in->va_address, in->offset_in_bo, in->map_size, vmFlags, o, 4)
	    : n33VmUnmap(in->handle, in->va_address, o, 4);
	return why_io(w);
}

IOReturn Navi48Bringup::n33nCtx(const n48n_ctx *in, n48n_ctx *out) {
	if (!n33NativeOpen || !n33NativeClient.hello) return kIOReturnNotReady;
	return n33n_io(n33n::ctx_op(n33NativeClient, in, out));
}

IOReturn Navi48Bringup::n33nSubmit(const uint8_t *in, uint32_t size, uint64_t *seqOut) {
	if (!n33NativeOpen) return kIOReturnNotReady;
	n33n::SubmitReq req {};
	const n33n::Code c = n33n::submit_check(in, size, n33Vm, n33Bo, n33Vm.live, n33NativeClient, &req);
	if (c != n33n::kOk) return n33n_io(c);
	if (req.hasFence) {   // Mesa's W3 test carries no fence BO; a caller fence BO needs its MC resolved + a BO-side wait, not done yet
		N33NLOG("submit refused: a caller fence BO is not yet supported on N33 (the kernel seqno fence is used instead)");
		return kIOReturnUnsupported;
	}
	uint64_t out[16] = {};
	const uint32_t w = n33VmSubmitUser(req, out, 16);
	if (w) return why_io(w);
	if (seqOut) *seqOut = out[1];
	return kIOReturnSuccess;
}

IOReturn Navi48Bringup::n33nWait(uint64_t target, uint64_t timeoutNs, uint64_t out[3]) {
	if (!n33NativeOpen) return kIOReturnNotReady;
	(void)timeoutNs;   // n33CpWait uses the step-6 2 s dispatch timeout
	uint64_t w[16] = {};
	const uint32_t rc = n33CpWait((uint32_t)target, w, 16);   // may_waitseq refuses a seqno that is not the outstanding submit
	if (rc) { out[0] = 0; out[1] = 0; out[2] = n33Cp.seq; return why_io(rc); }
	const bool retired = (w[1] != 0);   // n33CpIbWait put(1, latch == kLatchNone ? 1 : 0)
	out[0] = retired ? 0ull : 1ull;     // busy
	out[1] = retired ? 1ull : 0ull;     // retired
	out[2] = n33Cp.seq;                 // last emitted
	return kIOReturnSuccess;
}

IOReturn Navi48Bringup::n33nMemoryForHandle(uint32_t handle, IOOptionBits *options, IOMemoryDescriptor **memory) {
	if (!n33NativeOpen || !n33NativeClient.hello) return kIOReturnNotReady;
	if (!n33bo::handle_ok(n33Bo, handle)) return kIOReturnNotFound;
	const n33bo::Bo &b = n33Bo.bo[handle - 1];
	const uint64_t bytes = n33bo::bytes(b);
	if (b.kind == n33bo::kVram) {
		if (!n33Bar0Phys) return kIOReturnNotReady;
		// The BO lives in the slab inside BAR0; hand userspace an IODeviceMemory of that physical window (as the gfx12 path did).
		IODeviceMemory *dm = IODeviceMemory::withRange(n33Bar0Phys + n33bo::kSlabOff + b.vramOff, bytes);
		if (!dm) return kIOReturnNoMemory;
		if (options) *options = kIOMapAnywhere;
		*memory = dm;   // +1; IOUserClient releases it
		return kIOReturnSuccess;
	}
	// GTT / import: the descriptor the BO was made with (IOBufferMemoryDescriptor / the wired caller range).
	IOMemoryDescriptor *md = n33BoMd[handle - 1];
	if (!md) return kIOReturnNotFound;
	md->retain();
	if (options) *options = 0;
	*memory = md;
	return kIOReturnSuccess;
}

// The W3 caller-IB submit kick. It does NOT build or touch the IB bodies (Mesa wrote them into the BOs). The preamble
// and the wptr/doorbell tail mirror n33CpIbBuildKick (W1/W2/W2V stay byte-identical); only the ring content differs:
// ACQUIRE_MEM + INDIRECT_BUFFER per caller IB (VMID 8), one RELEASE_MEM to host page B's fence (VMID 0), as W2V.
uint32_t Navi48Bringup::n33VmSubmitUser(const n33n::SubmitReq &req, uint64_t *out, uint32_t outCount) {
	using namespace n33cp;
	auto put = [&](uint32_t i, uint64_t v) { if (i < outCount) out[i] = v; };
	auto w32 = [&](uint64_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(n33Bar0 + off) = v; };
	uint32_t w = may_submit(n33Cp);   // queue running, not latched, no submit outstanding (W1's gate)
	if (w) return w;
	if (!n33GmcGcUp()) return kWhyGcDown;
	if (rmmio[kGcFaultStatus]) { N33NLOG("submit refused: GC fault status %#x", rmmio[kGcFaultStatus]); return kWhyFault; }
	if (n33P2.failed) return n33psp2::kWhyFailed;
	const bool db = n33Cp.mode == kModeDb;
	if (db && (!dev.bar2 || dev.bar2Size < kBar2Need)) return kWhyDoorbell;
	const Geo g = geo(n33FbBase);
	const uint32_t seq = n33Cp.seq + 1;
	// Bind host page B at GART slot 1 (VMID 0, the RELEASE_MEM fence target), flush both hubs, verify the PTE - exactly
	// the W1/W2/W2V kick's preamble, so the fence lands where n33CpWait polls it.
	const uint64_t want = n33GmcHost[kHostB] ? n33gmc::pte_sys(n33GmcHostPa[kHostB]) : 0;
	if (!want || *reinterpret_cast<volatile uint64_t *>(n33Bar0 + n33gmc::slot_off(kSlotB)) != want) {
		uint64_t mo[4] = {};
		const uint32_t mw = n33GmcMap(kSlotB, 1, n33gmc::kMapHost, kHostB, mo, 4);
		if (mw) { N33NLOG("submit refused: gart map of host page B (why %u)", mw); return kWhyGart; }
	}
	const uint32_t mm = n33GmcFlush(false), gc = n33GmcFlush(true);
	if (!n33gmc::flush_ok(false, mm) || !n33gmc::flush_ok(true, gc)) {
		n33Gmc.stopped = true;
		N33NLOG("submit refused: hub flush MMHUB %#x GFXHUB %#x (GMC stopped)", mm, gc);
		return kWhyGart;
	}
	const uint64_t pte = *reinterpret_cast<volatile uint64_t *>(n33Bar0 + n33gmc::slot_off(kSlotB));
	if (!n33GmcHost[kHostB] || pte != n33gmc::pte_sys(n33GmcHostPa[kHostB])) { N33NLOG("submit refused: slot 1 PTE %#llx", pte); return kWhyGart; }
	volatile uint32_t *hb = static_cast<volatile uint32_t *>(n33GmcHost[kHostB]->getBytesNoCopy());
	hb[kHostFence4 / 4] = kBefore;
	n33GmcHdpFlush();
	n33Gmc.engineHolds = true;   // the GMC restore is refused and the host pages leak from now on (as W2V)
	// The ring: ACQUIRE_MEM before every INDIRECT_BUFFER, one IB per caller IB in VMID 8, RELEASE_MEM to host page B.
	uint32_t pk[kSubmitDw];
	uint32_t len = n33n::ring_build(pk, req, g.hostB + kHostFence4, seq);
	len = pad(pk, len);
	const uint64_t at = ring_off(n33Cp.wptrDw);
	for (uint32_t i = 0; i < len; i++) w32(kRingOff + at + 4ull * i, pk[i]);
	const uint64_t wp = n33Cp.wptrDw + len;
	const uint32_t rptr0 = n33CpHqdRd(kPqRptr);
	*reinterpret_cast<volatile uint64_t *>(n33Bar0 + kWptrSlot) = wp;
	n33GmcHdpFlush();
	if (db) {
		*reinterpret_cast<volatile uint64_t *>(dev.bar2 + kDbByte) = wp;
	} else {
		rmmio[kGrbmGfxCntl] = kSelQ0;
		rmmio[kPqWptrLo] = lo32(wp);
		rmmio[kPqWptrHi] = hi32(wp);
		rmmio[kGrbmGfxCntl] = 0;
	}
	n33Cp.seq = seq;
	record_submit(n33Cp, seq, wp, rptr0, kXibVmUser);
	N33NLOG("user submit seqno %u (%u IBs in VMID %u, wptr %llu, PQ_RPTR %#x); run cp waitseq %u", seq, req.numIbs, n33n::kVmid, wp, rptr0, seq);
	put(0, 0); put(1, seq); put(4, rptr0); put(5, wp);
	return kWhyOk;
}

#endif /* N33_OBSERVE */
