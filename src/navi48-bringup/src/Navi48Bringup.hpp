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
#include "amd/n48log.h"
#include "amd/rebar_pure.h"   // 0.0.663: Resizable BAR planning (pure)
#include "amd/amdgpu_init.h"
struct Navi48Counters;

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
	IOReturn n33Call(uint32_t action, const uint64_t *in, uint32_t inCount, uint64_t *out, uint32_t outCount);
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
	IOReturn n33FbPublish(uint64_t *out, uint32_t outCount);
	void n33RestoreScanout();
	void n33FbAttach();
	void n33FbDetach();
	// N33 stage 3 step 1, milestone 2: the PSP KM ring (n33psp2::, Navi48Bringup.cpp).
	IOReturn n33CallLocked(uint32_t action, const uint64_t *in, uint32_t inCount, uint64_t *out, uint32_t outCount);
	n33psp2::State n33P2 {};
	bool     n33PspPagesOwned() const { return n33P2.ring || n33P2.fence || n33P2.failed; }   // sticky once a ring was sent
	void     n33PspW(uint64_t off, uint32_t v);
	uint32_t n33PspR(uint64_t off) const;
	uint32_t n33PspRingCreate(uint32_t *c64);
	bool     n33PspRingDestroy(uint32_t *c64);
	void     n33PspStop();
	uint32_t n33PspSubmit(uint32_t cmd, uint32_t fwType, uint64_t off, uint64_t size, uint64_t *out, uint32_t outCount);
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
