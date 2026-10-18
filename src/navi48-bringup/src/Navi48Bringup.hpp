//
//  Navi48Bringup.hpp — Phase 3 GPU bring-up service for Navi 48 (gfx1201)
//  on x86 macOS (Hackintosh).
//
//  A plain IOService on the GPU's IOPCIDevice, opt-in via boot-arg
//  "navi48bringup=1". With both this and RDNA4FB installed, RDNA4FB owns the
//  display unless disabled with "rdna4-off=1".
//
//  Stages (each gated; later ones only run when explicitly requested):
//    survey (default)      read-only: BAR5, on-die IP discovery, IP/PSP/SMU state
//    navi48-smu=1          read-only SMU TestMessage + version (benign mailbox)
//    navi48-psp=1          FIRST WRITING STAGE: PSP bootloader chain -> SOS alive
//
//  VRAM use: everything this kext puts in VRAM lives at `vramBase`, chosen at
//  runtime to sit above the UEFI boot framebuffer (which the video console and
//  IONDRVFramebuffer keep drawing into) and inside the BAR0 CPU aperture.
//
//  Register harness ported from RDNA4FB (BSD-3); PSP sequence from
//  lemonade-sdk/mac-amdgpu (MIT); semantics cross-checked with Linux amdgpu.
//
#ifndef Navi48Bringup_hpp
#define Navi48Bringup_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <pexpert/pexpert.h>

#include "ipdiscovery.hpp"
#include "psp.hpp"
#include "amd/amdgpu_regs.h"
#include "amd/n33_psp2_pure.h"      // N33 milestone 2: n33psp2::State (pure, no kernel types)
#include "amd/n33_smu_pure.h"       // N33 SMU mailbox: n33smu::State (pure, no kernel types)
#include "amd/n33_gmc_pure.h"       // N33 GMC 11 / GART: n33gmc::State, Snap, Inputs, Rec (pure, no kernel types)
#include "amd/n33_sdma_pure.h"      // N33 step 5, CP RS64 and SDMA 6.0.2: n33sdma::State, Entry, Rec (pure, no kernel types)
#include "amd/n33_cp_pure.h"        // N33 step 6, one MEC compute queue: n33cp::State (pure, no kernel types)
#include "amd/n33_dmub_pure.h"      // N33 stage 3 DMCUB proof: n33dmub::State, RegionInfo (pure, no kernel types)
#include "amd/n33_ih_pure.h"        // N33 step 4, IH 6.0.2 ring 0: n33ih::State (pure, no kernel types)
#include "amd/n33_bo_pure.h"        // N33 step 7 W2, buffer objects in VMID 0: n33bo::State (pure, no kernel types)
#include "amd/n33_vm_pure.h"        // N33 step 7 W2V, a per-process GPU VM in VMID 8: n33vm::State (pure, no kernel types)
#if defined(N33_OBSERVE)
#include "amd/n33_native_pure.h"    // N33 step 7 W3, the N48N user-client subset: n33n::Client / SubmitReq (pure; brings in Navi48NativeABI.h)
#endif
#include "amd/n48log.h"
#include "amd/rebar_pure.h"   // 0.0.663: Resizable BAR planning (pure)
#include "amd/amdgpu_init.h"
struct Navi48Counters;
class IOBufferMemoryDescriptor;

#define N48LOG(fmt, ...) ::amdgpu::n48_logf("Navi48Bringup: " fmt "\n", ##__VA_ARGS__)

class Navi48Bringup : public IOService {
	OSDeclareDefaultStructors(Navi48Bringup)
	using super = IOService;

public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool       start(IOService *provider) override;
	void       stop(IOService *provider) override;
	void       free() override;

	// --- userspace access (Navi48UserClient, Stage A6) ---
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	                       OSDictionary *properties, IOUserClient **handler) override;
	amdgpu::BringupContext *bringupContext();
	amdgpu::DeviceContext  *deviceContext() { return &dev; }
#if defined(N33_OBSERVE)
	// N33 stage 2a: guarded runtime access for the display flip experiments (n33ctl).
	bool n33IsObserving() const { return n33Observing; }
	uint32_t n33ScanoutTo(bool ours);   // the self-flip; called only by n33_fb_scanout, under its lock
	IOReturn n33Call(uint32_t action, const uint64_t *in, uint32_t inCount, uint64_t *out, uint32_t outCount,
	                 task_t caller = TASK_NULL);   // caller: the user client's task, for W2's bo import only
	// Stage 3 step 7 W3: the N48N native client on the N33 build. IOAccelNavi48NativeClient dispatches to these when
	// n33IsObserving(); the pure decisions are n33_native_pure.h. One client at a time. Defined in amd/n33_native.cpp.
	IOReturn n33nOpen(uint32_t *idOut);
	void     n33nClose();
	bool     n33nIsOpen() const { return n33NativeOpen; }
	IOReturn n33nHello(uint64_t clientAbi, uint64_t flags, uint64_t out[4]);
	IOReturn n33nQueryInfo(n48n_info *out);
	IOReturn n33nReadRegs(uint64_t off, uint64_t count, uint64_t instance, uint32_t *out);
	IOReturn n33nBoCreate(const n48n_gem_create_in *in, uint64_t out[4]);
	IOReturn n33nBoFree(uint32_t handle);
	IOReturn n33nBoImportHost(task_t task, uint64_t hostVa, uint64_t size, uint64_t flags, uint64_t gpuVa, uint64_t out[4]);
	IOReturn n33nGemVa(const n48n_gem_va *in);
	IOReturn n33nCtx(const n48n_ctx *in, n48n_ctx *out);
	IOReturn n33nSubmit(const uint8_t *in, uint32_t size, uint64_t *seqOut);
	IOReturn n33nWait(uint64_t target, uint64_t timeoutNs, uint64_t out[3]);
	IOReturn n33nMemoryForHandle(uint32_t handle, IOOptionBits *options, IOMemoryDescriptor **memory);
#endif
	// Navi48Ttl answers Apple's queryHwBlockRegisterBase straight out of this.
	const IpDiscovery      &ipDisc() const { return ipDiscovery; }
	bool interruptsArmed() const { return intArmed; }
	// Phase 4: install the TTL hook and let Apple's accelerator match. Called
	// from the user client, never from start() — see kNavi48SelAccelExperiment.
	// outExtra, when non-null, is an array of kAccelExtraScalars verb-specific
	// u64s handed straight back to userspace as scalarOutput[3..]. Rule 14: a
	// verb whose result matters must not depend on the shared log buffer.
	// 13 is the CEILING, not a preference: IOConnectCallScalarMethod's
	// io_scalar_inband64_t is uint64_t[16] (device_types.h:105) and the extras
	// start at scalarOutput[3], so 3 + 13 = 16 uses every slot there is. A 14th
	// extra would be dropped silently on the way out, which is exactly the kind
	// of quiet loss rule 14 exists to prevent.
	static constexpr unsigned kAccelExtraScalars = 13;
	// 0.0.194: `argScalar` is scalarInput[1] — a verb-specific argument, 0 when
	// userspace sent only the action. `vmib` (42) reads it as the VA to walk.
	IOReturn accelExperiment(uint32_t action, uint64_t *outInstalled,
	                         uint64_t *outTtlCalls, uint64_t *outFirstUnsupported,
	                         uint64_t *outExtra = nullptr, uint64_t argScalar = 0);
	void noteSubmit() { submits++; }
	void noteFence(bool ok) { if (ok) fencesCompleted++; else fenceTimeouts++; }
	void fillCounters(struct Navi48Counters *out);

	// --- hardware access used by the stage modules (via n48::HwAccess bridge) ---
	uint32_t regReadIp (uint16_t hwId, uint8_t inst, uint8_t seg, uint32_t dword);
	bool     regWriteIp(uint16_t hwId, uint8_t inst, uint8_t seg, uint32_t dword, uint32_t value);
	bool     vramWrite (uint64_t vramOffset, const void *src, size_t len);   // via BAR0, fenced
	bool     vramMemset(uint64_t vramOffset, uint8_t value, size_t len);     // via BAR0, fenced
	// 0.0.613 (the display pipe's v1 present): the console buffer (the GOP / RDNA4FB scanout) inside BAR0. consoleGeometry is false until the boot capture found it inside BAR0 and its
	// length equals rowBytes * height; consoleWrite is bounds-checked against the console's length (overflow-safe) before the fenced BAR0 write.
	bool     consoleGeometry(uint64_t *off, uint64_t *len, uint32_t *w, uint32_t *h, uint32_t *rowBytes) const;
	bool     consoleWrite(uint64_t consoleOff, const void *src, size_t len);
	bool     bar0WriteCombined() const { return bar0Map && (bar0Map->getMapOptions() & kIOMapWriteCombineCache) != 0; }   // 0.0.615 (W1): the option the BAR0 map was made with (the same test mapVramAperture logs)

private:
	// --- register harness (BAR5) ---
	bool     mapRegisters();
	void     unmapRegisters();
	uint32_t regRead32 (uint32_t byteOffset) const;
	bool     regWrite32(uint32_t byteOffset, uint32_t value);
	uint32_t vramRead32(uint64_t pos) const;           // MM_INDEX/MM_DATA indirect

	// --- VRAM aperture (BAR0, CPU-visible window onto the start of VRAM) ---
	bool mapVramAperture();
	void unmapVramAperture();
	uint32_t bar0Read32(uint64_t vramOffset) const;
	bool captureBootFramebuffer();      // where the UEFI/GOP console lives in VRAM
	bool chooseVramBase();              // pick vramBase clear of it
	bool apertureCheck(uint64_t off);   // BAR0 write == MM-window read at the same offset? (0.0.663: the original bytes are saved and RESTORED)
	bool finishVramPlan();              // 0.0.663 (ReBAR item 2/3): re-plan with the console located; sets bar0Limit or refuses
	void dropBar0();                    // 0.0.664 (F4): unmap BAR0 and clear every BAR0 field: exactly the 'no BAR0' state
	bool apertureCheckWindow();         // 0.0.663 (ReBAR item 4): apertureCheck at vramBase, the middle and vramLimit - 64 KiB, before the allocator exists

#if defined(N33_OBSERVE)
	bool startN33Observe();
	bool n33MapBar0();
	void n33SnapshotScanout();
	bool n33WriteAllowed(uint64_t off, uint64_t len, uint64_t *why) const;
	bool n33Observing { false };
	IOMemoryMap      *n33Bar0Map { nullptr };
	volatile uint8_t *n33Bar0    { nullptr };
	uint64_t          n33Bar0Size { 0 };
	uint64_t          n33FbBase   { 0 };   // DCN_VM_FB_LOCATION_BASE << 24 (GPU address of VRAM offset 0)
	uint64_t          n33FbOffset { 0 };   // DCN_VM_FB_OFFSET << 24 (DMCUB window offsets are relative to this)
	uint64_t          n33GopOff   { 0 };   // the GOP's scanout surface at start, as a VRAM offset
	uint64_t          n33GopLen   { 0 };
	uint32_t          n33PspWinReg { 0 };      // C2PMSG_36 value of the validated PSP fw_pri window; 0 = none set
	uint64_t          n33Bar0Phys { 0 };   // config-space BAR0 physical address (stage 2 step 3)
	uint32_t          n33SurfCfg  { 0 };   // HUBP0 SURFACE_CONFIG at start: logged, never judged
	// B0: the publish body, shared by the fbpublish verb and the boot-time publish. status matches the
	// verb's out[0] (0 ok, 1 not ready, 2 already published, 3 HUBP0 not on GOP, 4 gate, 5 snapshot,
	// 6 protected range, 7 publish failed); v1/v2 are the per-status detail (verdict, slot/why).
	struct N33PublishResult { uint32_t status, v1, v2; };
	N33PublishResult n33PublishFbNub();
	IOReturn n33FbPublish(uint64_t *out, uint32_t outCount);
	void n33RestoreScanout();
	void n33FbAttach();
	void n33FbDetach();
	// N33 stage 3 step 1, milestone 2: the PSP KM ring (n33psp2::, Navi48Bringup.cpp).
	IOReturn n33CallLocked(uint32_t action, const uint64_t *in, uint32_t inCount, uint64_t *out, uint32_t outCount, task_t caller);
	n33psp2::State n33P2 {};
	bool     n33PspPagesOwned() const { return n33P2.ring || n33P2.fence || n33P2.failed; }   // sticky once a ring was sent
	void     n33PspW(uint64_t off, uint32_t v);
	uint32_t n33PspR(uint64_t off) const;
	uint32_t n33PspRingCreate(uint32_t *c64);
	bool     n33PspRingDestroy(uint32_t *c64);
	void     n33PspStop();
	uint32_t n33PspSubmit(uint32_t cmd, uint32_t fwType, uint64_t off, uint64_t size, uint64_t *out, uint32_t outCount,
	                      bool dmubCaller = false);
	// N33 SMU 13.0.7 driver mailbox, S1 to S3 (n33smu::, Navi48Bringup.cpp).
	n33smu::State n33Smu {};
	uint32_t n33SmuSend(uint32_t msg, uint32_t arg, uint64_t *out, uint32_t outCount);
	uint32_t n33SmuArm(uint32_t level);
	// N33 GMC 11 / MMHUB 3.0.2 / GFXHUB 3.0 and the GART, G0 to G6 (n33gmc::, Navi48Bringup.cpp).
	n33gmc::State  n33Gmc {};
	n33gmc::Snap   n33GmcSnap {};                       // taken at G0
	n33gmc::Inputs n33GmcIn {};                         // MMHUB FB location, MEMSIZE and system aperture at G0
	n33gmc::Rec    n33GmcJournal[n33gmc::kJournalMax] {};   // every register a step wrote, before and after
	uint32_t       n33GmcJournalN { 0 };
	n33gmc::Entry  n33GmcPlanBuf[n33gmc::kPlanMax] {};    // plan() output, off the kernel stack (calls are serialized)
	uint32_t       n33GmcLastFlush { 0 };                // n33gmc::FlushBits of the last MMHUB flush
	IOBufferMemoryDescriptor *n33GmcDummy { nullptr };   // D6: leaked at unload, never freed
	uint64_t       n33GmcDummyPa { 0 };
	IOBufferMemoryDescriptor *n33GmcHost[n33gmc::kHostPages] {};   // G5's pages
	uint64_t       n33GmcHostPa[n33gmc::kHostPages] {};
	void     n33GmcTakeSnap(n33gmc::Snap *s) const;
	bool     n33GmcGcUp() const;
	void     n33GmcHdpFlush();
	uint32_t n33GmcFlush(bool gc);
	IOBufferMemoryDescriptor *n33GmcAllocPage(uint64_t *pa, uint32_t pattern);
	void     n33GmcPte(uint32_t slot, uint64_t pte);
	uint32_t n33GmcStep(uint32_t step, uint64_t *out, uint32_t outCount);
	uint32_t n33GmcMap(uint32_t slot, uint32_t count, uint32_t kind, uint64_t arg, uint64_t *out, uint32_t outCount);
	uint32_t n33GmcUnmap(uint32_t slot, uint32_t count, uint64_t *out, uint32_t outCount);
	uint32_t n33GmcRestore(uint64_t *out, uint32_t outCount);
	bool     n33GmcStop();   // true: host memory may be freed (flushes acknowledged, no engine held a GART address)
	// N33 stage 3 step 5: CP RS64 start (C1) and SDMA0 queue 0, mmio or doorbell kick (n33sdma::, Navi48Bringup.cpp).
	n33sdma::State n33Sdma {};
	n33sdma::Rec   n33SdmaJournal[n33sdma::kJournalMax] {};
	uint32_t       n33SdmaJournalN { 0 };
	n33sdma::Entry n33SdmaPlanBuf[n33sdma::kPlanMax] {};
	uint32_t n33SdmaRd(uint32_t reg);
	void     n33SdmaWr(uint32_t reg, uint32_t v);
	uint32_t n33SdmaRun(uint32_t phase, uint32_t n, uint32_t *bad, uint32_t rptr[2]);
	uint32_t n33SdmaPagesClear() const;
	uint32_t n33GfxRs64(uint64_t *out, uint32_t outCount);
	uint32_t n33SdmaStart(uint32_t mode, uint64_t *out, uint32_t outCount);
	uint32_t n33SdmaTest(uint32_t n, uint64_t *out, uint32_t outCount);
	uint32_t n33SdmaStop(bool unload, uint64_t *out, uint32_t outCount);
	// N33 stage 3 step 6: MEC pipe 0 and one compute HQD, db or mmio kick, K1 to K6 (n33cp::, Navi48Bringup.cpp).
	n33cp::State   n33Cp {};
	n33sdma::Rec   n33CpJournal[n33cp::kJournalMax] {};
	uint32_t       n33CpJournalN { 0 };
	n33sdma::Entry n33CpPlanBuf[n33cp::kPlanMax] {};
	uint32_t n33CpRun(uint32_t phase, uint32_t n, uint32_t *bad, bool *busy, bool *resetRan);
	uint32_t n33CpHqdRd(uint32_t reg);
	bool     n33CpBlobOk() const;
	uint32_t n33CpPagesClear() const;
	uint32_t n33CpStart(uint32_t mode, uint64_t *out, uint32_t outCount);
	uint32_t n33CpTest(uint32_t n, uint64_t *out, uint32_t outCount);
	uint32_t n33CpStop(bool unload, uint64_t *out, uint32_t outCount);
	uint32_t n33CpIbPageClear() const;
	uint32_t n33CpStreamBuild(uint32_t stream, const n33cp::Geo &g, uint32_t seq, uint32_t *ib, uint32_t &ibDw);   // X4b
	uint32_t n33CpIbBuildKick(uint32_t stream, uint32_t &seq, uint64_t &wp, uint32_t &rptr0, volatile uint32_t *&hb, uint64_t *out, uint32_t outCount);
	uint32_t n33CpIbWait(uint32_t stream, uint32_t seq, uint64_t wp, uint32_t rptr0, volatile uint32_t *hb, uint64_t *out, uint32_t outCount);
	uint32_t n33CpIb(uint64_t *out, uint32_t outCount);
	uint32_t n33CpSubmit(uint32_t stream, uint64_t *out, uint32_t outCount, bool internal = false);   // W1 cp submit (0), X4b cp xib (1, 2), W2 (3, 4: internal)
	uint32_t n33CpWait(uint32_t seq, uint64_t *out, uint32_t outCount);   // W1 (step 7): the winsys waitseq half
	// N33 stage 3 DMCUB bring-up proof (n33dmub::, Navi48Bringup.cpp): snapshot, PSP type-51 load, reset, init,
	// QUERY_FEATURE_CAPS, and the hold/revert check. No writes until snapshot arms the gate.
	n33dmub::State     n33Dmub {};
	n33dmub::RegionInfo n33DmubRgn {};
	IOMemoryMap       *n33RomMap { nullptr };    // the expansion-ROM BAR, mapped lazily by the snapshot (smallest ROM read)
	volatile uint8_t  *n33Rom { nullptr };
	uint64_t           n33RomLen { 0 };
	bool     n33DmubRomMap();                    // map the expansion ROM once; false if the card exposes none
	bool     n33DmubHubpOnGop() const;           // HUBP0 is on the GOP scanout surface (the gate's display precondition)
	uint32_t n33DmubSnap(uint64_t *out, uint32_t outCount);   // action 60
	uint32_t n33DmubReset(uint64_t *out, uint32_t outCount);  // action 61
	uint32_t n33DmubLoad(uint64_t off, uint64_t size, uint64_t *out, uint32_t outCount);   // action 62 (PSP type 51)
	uint32_t n33DmubInit(uint64_t *out, uint32_t outCount);   // action 63
	uint32_t n33DmubCmd(uint32_t tpl, uint64_t *out, uint32_t outCount);   // action 64 (QUERY_FEATURE_CAPS)
	uint32_t n33DmubCheck(uint64_t *out, uint32_t outCount);  // action 65
	// N33 stage 3 step 4: one IH 6.0.2 ring, polled, then MSI behind n33-ih-msi (n33ih::, Navi48Bringup.cpp). Reached
	// only through actions 70 to 75; nothing at load or start.
	n33ih::State   n33Ih {};
	n33sdma::Rec   n33IhJournal[n33ih::kJournalMax] {};
	uint32_t       n33IhJournalN { 0 };
	uint32_t       n33IhIv[n33ih::kIvJournalMax][n33ih::kIvDw] {};   // IV entries as read from the ring
	n33sdma::Entry n33IhPlanBuf[n33ih::kPlanMax] {};
	IOWorkLoop             *n33IhWorkLoop { nullptr };   // I3 only: a private work loop and the MSI event source
	IOInterruptEventSource *n33IhIntSource { nullptr };
	static void n33IhTrampoline(OSObject *owner, IOInterruptEventSource *src, int count);
	void     n33IhDrain();                       // the I3 handler, on n33IhWorkLoop, under gN33IhLock
	uint32_t n33IhRun(uint32_t phase, uint32_t n, uint32_t *bad);
	uint32_t n33IhRingClear() const;
	bool     n33IhPciQuiet(uint16_t *cmd, uint16_t *msi, uint16_t *msix) const;
	uint32_t n33IhTake(uint32_t want, uint32_t wptr, uint32_t *wantN, uint32_t *otherN, uint32_t *first);
	uint32_t n33IhStart(uint32_t mode, uint64_t *out, uint32_t outCount);   // action 71 (I1)
	uint32_t n33IhRecv(uint32_t src, uint64_t *out, uint32_t outCount);     // action 72 (I2, I4)
	uint32_t n33IhArm(uint64_t *out, uint32_t outCount);                    // action 73 (I3)
	uint32_t n33IhStop(bool unload, uint64_t *out, uint32_t outCount);      // action 74 (I5)
	void     n33IhDisarm();
	// N33 stage 3 step 7 W2: buffer objects mapped at caller VAs in VMID 0's GART, actions 39 to 47 (n33bo::).
	n33bo::State        n33Bo {};
	IOMemoryDescriptor *n33BoMd[n33bo::kBoMax] {};    // GTT: an IOBufferMemoryDescriptor; import: the wired caller range
	IOMemoryMap        *n33BoMap[n33bo::kBoMax] {};   // import: its kernel mapping
	volatile uint32_t  *n33BoCpu[n33bo::kBoMax] {};   // host BOs' kernel CPU map (VRAM BOs: BAR0)
	bool     n33BoGmcReady() const;
	uint64_t n33BoPagePa(uint32_t h, uint32_t page);
	volatile uint32_t *n33BoWord(uint32_t h, uint64_t byteOff);
	uint32_t n33BoFlushBoth();
	void     n33BoRelease(uint32_t h);
	uint32_t n33BoCreate(uint32_t kind, uint64_t size, uint64_t *out, uint32_t outCount);              // action 39
	uint32_t n33BoImport(task_t task, uint64_t uaddr, uint64_t size, uint64_t *out, uint32_t outCount);  // action 40
	uint32_t n33BoMapVa(uint32_t h, uint64_t va, uint32_t vmFlags, uint64_t *out, uint32_t outCount);   // action 41
	uint32_t n33BoUnmap(uint32_t h, uint64_t *out, uint32_t outCount);                                  // action 42
	uint32_t n33BoFree(uint32_t h);                                                                      // action 43
	uint32_t n33BoInfo(uint32_t h, uint64_t *out, uint32_t outCount);                                   // action 44
	uint32_t n33BoSubmit(uint32_t stream, uint32_t a, uint32_t b, uint64_t va, uint64_t *out, uint32_t outCount);   // 46, 47
	void     n33BoPrefill(uint32_t seq);
	uint32_t n33BoLanded(uint32_t seq);
	void     n33BoStop(bool freeMem);
	// N33 stage 3 step 7 W2V: a per-process GPU VM in VMID 8, actions 48 to 55 (n33vm::, design stage3-step7-gpuvm-design.md).
	n33vm::State n33Vm {};
	uint64_t n33VmVbo() const;                                      // vram_base_offset = MC_VM_FB_OFFSET << 24
	uint32_t n33VmFlush();                                          // engine-17 invalidate for VMID 8 (100 ms), VMID-8 ack bit
	uint32_t n33VmArenaCheck();                                     // the arena clear of every live protected range (81)
	uint32_t n33VmCreate(uint64_t *out, uint32_t outCount);                                                     // 48
	uint32_t n33VmMap(uint32_t h, uint64_t va, uint64_t off, uint64_t size, uint32_t flags, uint64_t *out, uint32_t outCount);   // 49
	uint32_t n33VmUnmap(uint32_t h, uint64_t va, uint64_t *out, uint32_t outCount);                             // 50
	uint32_t n33VmInfo(uint64_t va, bool haveVa, uint64_t *out, uint32_t outCount);                             // 51
	uint32_t n33VmSubmit(uint32_t stream, uint64_t ibVa, uint64_t aVa, uint64_t bVa, uint64_t *out, uint32_t outCount);   // 52, 53, 54
	uint32_t n33VmLanded(uint32_t seq);                            // the VM copy/dispatch samples in the destination BO
	uint32_t n33VmDestroy(uint64_t *out, uint32_t outCount);                                                    // 55
	void     n33VmStop();                                          // unload: park CONTEXT8 and release the arena
	// The VM submit context, resolved by n33VmSubmit for n33CpIbBuildKick and the shared wait.
	uint64_t n33VmSubIbVa { 0 };      // the IB VA (a VRAM BO mapped R|X in VMID 8); the ring's INDIRECT_BUFFER target
	uint32_t n33VmSubIbBo { 0 };      // that BO's handle
	uint64_t n33VmSubIbOff { 0 };     // the IB's byte offset inside the BO
	uint64_t n33VmSubSrcOff { 0 };    // the source BO byte offset (copy)
	uint64_t n33VmSubDstOff { 0 };    // the destination BO byte offset (copy/dispatch); the handles are n33Bo.copySrc/copyDst
	uint64_t n33VmSubSrcVa { 0 }, n33VmSubDstVa { 0 }, n33VmSubCodeVa { 0 }, n33VmSubFaultVa { 0 };
	// W3 (step 7): the N48N native client's own state (one client at a time) and the caller-IB submit kick.
	n33n::Client n33NativeClient {};
	bool         n33NativeOpen { false };
	uint32_t     n33NativeId { 0 };
	uint32_t n33VmSubmitUser(const n33n::SubmitReq &req, uint64_t *out, uint32_t outCount);   // the caller's IBs, VMID 8, fence = kernel seqno in host page B (as W2V)
#endif
	// --- stages ---
	bool probeMemSize();
	bool loadOnDieDiscovery();
	void surveyIPs();
	void surveyPSP();
	void surveySMU();
	void stagePSP();       // navi48-psp=1 (== navi48-stage=5)
	bool buildDeviceContext();   // fills `dev` for the ported amdgpu modules
	bool mapDoorbells();         // BAR2 -> dev.bar2 (CP/MES/SDMA stages)
	void runStages(uint32_t target);   // navi48-stage=N: mac-amdgpu BringupStage ladder beyond SOS

	// --- interrupts (navi48-interrupts=1) ---
	// The GPU signals the host with a PCI message-signalled interrupt when the
	// IH ring goes non-empty. Until 0.0.18 the ring was brought up with
	// IH_RB_CNTL.ENABLE_INTR = 0 and drained by polling; this arms the real
	// path: MSI index -> IOInterruptEventSource on our own work loop -> drain
	// and classify. Gated because an interrupt we fail to ack is a live-lock,
	// and the ladder's polled path is the known-good fallback.
	bool armInterrupts();
	void disarmInterrupts();
	static void interruptTrampoline(OSObject *owner, IOInterruptEventSource *src, int count);
	void handleInterrupt(int count);
	void publishInterruptCounters();

	IOWorkLoop             *workLoop  { nullptr };
	IOInterruptEventSource *intSource { nullptr };
	bool     intArmed     { false };
	uint64_t irqCount     { 0 };   // times the handler ran
	uint64_t irqEntries   { 0 };   // IH entries drained
	uint64_t irqEop       { 0 };   // src 181 CP end-of-pipe
	uint64_t irqFaults    { 0 };   // src 0 UTCL2/VM fault
	uint64_t irqCpError   { 0 };   // src 183/184/185 command-stream errors
	uint64_t irqOther     { 0 };
	uint32_t irqLogBudget { 0 };
	// 0.0.385 — class A's walk is 43 src-0 entries on three boots and 102 on a fourth, and the
	// shared 32-line budget above could not print even one whole walk. src-0 entries now draw on their OWN
	// budget and print dw1-3 and src_data[2..3] (the element count and the timing) instead of the two-dword
	// line. Read-only, unconditional, and BOUNDED: at most kIrqSrc0LogLines lines per boot however many faults
	// arrive. On a boot with no VM fault it prints nothing and the log is byte-identical to 0.0.384's.
	// 128 = the widest walk we have measured (arm6's 102) plus room for a second burst and the class-B
	// NACK entries that precede it (arm10's ordering: 8 PA writes, then the read-NACKs). WORST CASE IS
	// 128 LINES FOR THE WHOLE BOOT.
	static constexpr uint32_t kIrqSrc0LogLines = 128u;
	uint32_t irqSrc0LogBudget { 0 };
	uint64_t submits         { 0 };
	uint64_t fencesCompleted { 0 };
	uint64_t fenceTimeouts   { 0 };

	IOPCIDevice       *pciDevice  { nullptr };
	IOMemoryMap       *rmmioMap   { nullptr };
	volatile uint32_t *rmmio      { nullptr };
	size_t             rmmioSize  { 0 };
	IOMemoryMap       *bar2Map    { nullptr };
	IOMemoryMap       *bar0Map    { nullptr };
	volatile uint8_t  *bar0       { nullptr };
	size_t             bar0Size   { 0 };
	uint64_t           bar0Phys   { 0 };
	bool               rebarFound { false };  // 0.0.664: the census result for BAR0 (vramstat reports it)
	uint32_t           rebarMask  { 0 };      // BAR0's supported-sizes mask (bit k = 1 MiB << k)
	uint64_t           rebarCurMB { 0 };      // BAR0's current size in MiB
	uint64_t           bar0Limit  { 0 };      // 0.0.663: vramLimit planned by n48rebar::plan_bar0 (== bar0Size at a 256 MiB BAR0); 0 until finishVramPlan succeeds
	n48rebar::PlanIn   bar0Plan   {};         // 0.0.663: the plan's inputs, kept so finishVramPlan can add the console
	uint64_t           bootFbPhys { 0 };      // console framebuffer, physical
	uint64_t           bootFbLen  { 0 };
	uint64_t           bootFbRow  { 0 };      // 0.0.613: its rowBytes (getConsoleInfo v_rowBytes)
	uint64_t           bootFbOff  { ~0ULL };  // its VRAM offset, or ~0 if not in BAR0
	uint64_t           vramBase   { 0 };      // our bring-up region (VRAM byte offset)
	uint8_t           *onDieDisc  { nullptr };
	uint32_t           vramMB     { 0 };
	IpDiscovery        ipDiscovery;
	amdgpu::DeviceContext dev;          // compat layer for src/amd/* modules
	uint32_t           targetStage { 0 };
	bool               accelExperimentArmed { false };
	bool               accelExperimentFired { false };
};

#endif /* Navi48Bringup_hpp */
