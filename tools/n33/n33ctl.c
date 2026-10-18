// n33ctl: userspace side of the N33 stage 2a call (Navi 33 observe build of Navi48Bringup).
// Build: clang -O2 -Wall -framework IOKit -framework CoreFoundation n33ctl.c -o n33ctl
// Run as root. Every write is checked again in the kext (allowlist, protected VRAM ranges).
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>
#include "n33_fw.h"

enum { kSelRegRead = 7, kSelReadLog = 10, kSelN33 = 0x4e33 };
enum { kLogBytes = 2048 * 1024 };   // NAVI48_LOG_BYTES: the kext's log stops appending when full
enum { aStatus = 0, aRegWrite = 1, aBar0Read = 2, aBar0Write = 3, aCopy = 4, aFill = 5, aProtected = 6, aFbPublish = 7 };
enum { kHubp0Pitch = 0x3AC7, kHubp0Addr = 0x3ACA, kHubp0AddrHigh = 0x3ACB, kHubp0FlipCtl = 0x3ADB };

static io_connect_t conn;

static kern_return_t n33(uint64_t action, const uint64_t *args, uint32_t n, uint64_t *out, uint32_t *outn) {
	uint64_t in[8] = { action };
	if (n > 7) return kIOReturnBadArgument;
	memcpy(in + 1, args, n * sizeof(uint64_t));
	return IOConnectCallScalarMethod(conn, kSelN33, in, n + 1, out, outn);
}

static uint64_t num(const char *s) { return strtoull(s, NULL, 0); }

// The kext's protected-range kinds (n33_protected): 0 is the GOP scanout, 1-8 are DMCUB
// windows CW0-CW7, 9-11 are REGION4-REGION6, 12 the PSP ring pages (milestone 2), 13 the SMU driver table
// page (S3), 14 the GART table and mem_scratch (GMC G2), 15 the step-5 SDMA pages (E1), 16 the step-6 compute pages (K1). Names match the kext's comments.
static const char *kindName(uint64_t k) {
	static char b[16];
	if (k == 0) return "GOP scanout";
	if (k <= 8) { snprintf(b, sizeof b, "DMCUB CW%llu", k - 1); return b; }
	if (k <= 11) { snprintf(b, sizeof b, "DMCUB REGION%llu", k + 1); return b; }
	if (k == 12) return "PSP ring pages";
	if (k == 13) return "SMU driver table";
	if (k == 14) return "GART table and mem_scratch";
	if (k == 15) return "step-5 SDMA ring, wb and test pages";
	if (k == 16) return "step-6 compute queue pages";
	if (k == 17) return "step-7 T1 IB page";
	if (k == 18) return "DMCUB proof DMUB windows";
	if (k == 19) return "step-4 IH ring";
	if (k == 20) return "step-7 W2 VRAM BO slab";
	if (k == 21) return "step-7 W2V page-table arena";
	return "?";
}

// The kext's write-refusal reason codes (n33WriteAllowed plus the per-register checks in
// kN33RegWrite / kN33Bar0Copy). Keep in step with Navi48Bringup.cpp.
static const char *whyName(uint64_t why) {
	switch (why) {
	case 0:   return "ok";
	case 1:   return "bad args (null BAR0, misaligned, or len out of range)";
	case 2:   return "offset below the 16 MiB floor or past the end of BAR0";
	case 3:   return "address HIGH word would leave the current 4 GiB";
	case 4:   return "flip control: only UPDATE_LOCK and FLIP_TYPE may be set";
	case 5:   return "surface pitch may not change";
	case 6:   return "register not on the allowlist";
	case 7:   return "copy source range outside BAR0";
	case 8:   return "copy source and destination overlap";
	case 9:   return "PSP command is not one of the four bootloader commands";
	case 10:  return "PSP window is not a whole 1 MiB inside BAR0";
	case 11:  return "PSP bootloader is not idle (C2PMSG_35 != 0x80000000)";
	case 12:  return "no PSP window set, or C2PMSG_36 changed since it was validated";
	case 13:  return "sOS is already alive (C2PMSG_81 != 0)";
	case 14:  return "PSP window overlaps the framebuffer surface";
	case 15:  return "psp2: no KM ring created by this kext load";
	case 16:  return "psp2: command out of order, or sent twice";
	case 17:  return "psp2: fw_type not on the milestone 2 list";
	case 18:  return "psp2: image not page aligned inside fw_buf / fw_pri, or empty";
	case 19:  return "psp2: LOAD_TOC answer 0 or above 32 MiB";
	case 20:  return "psp2: TMR overlaps a live DMCUB window or REGION";
	case 21:  return "psp2: an earlier command failed; reload the kext";
	case 22:  return "psp2: ring state (sOS down, C2PMSG_64 not ready, wptr not 0, MMHUB base, or ring exists)";
	case 23:  return "smu: message not on the allowlist";
	case 24:  return "smu: argument is not the one allowed for this message";
	case 25:  return "smu: out of the ladder's order, not armed, or sent twice";
	case 26:  return "smu: PMFW marked hung (timeout or unknown response); reload the kext";
	case 27:  return "smu: C2PMSG_90 still 0 after 2 s before sending; nothing was written";
	case 28:  return "smu: an earlier state-changing message did not answer OK; the ladder is over";
	case 29:  return "smu: driver table page not writable, or not taken by S2's arm";
	case 30:  return "gmc: not a step number (1, 2, 3, 4, 6), or a bad count, kind or VRAM alignment";
	case 31:  return "gmc: out of order, twice, no G0 snapshot, no GART table yet, or G4 not done";
	case 32:  return "gmc: FB location, DCN_VM, HUBP0 or the GC FB location changed since G0";
	case 33:  return "gmc: a protection fault status is not 0";
	case 34:  return "gmc: D4, the system aperture would not cover the FB";
	case 35:  return "gmc: D4, the system aperture would reach the GART range";
	case 36:  return "gmc: live FB location or MEMSIZE does not fit the D2/D3 placement";
	case 37:  return "gmc: GC not up (no successful AUTOLOAD_RLC in this load, or BOOTLOAD_COMPLETE clear)";
	case 38:  return "gmc: GART slot range outside the GART";
	case 39:  return "gmc: GART table / mem_scratch range not writable (protected)";
	case 40:  return "gmc: no dummy or host page below 44 bits";
	case 41:  return "gmc: an earlier step or flush failed, or a restore ran; reload the kext";
	case 42:  return "gmc: restore refused, an engine holds a GART address";
	case 43:  return "step 5: GC not up (no successful AUTOLOAD_RLC in this load, BOOTLOAD_COMPLETE clear, or CP_STAT not 0)";
	case 44:  return "step 5: out of order (C1 after G6 or twice, G6 before C1, start before G6 or twice, test before start, "
	                 "after stop or before the previous test, stop with no queue, ring or journal full)";
	case 45:  return "step 5: 2c did not load PFP/ME/MEC (types 87-89) with the pinned sizes in this load";
	case 46:  return "step 5: SDMA0 not as expected before start (RB_ENABLE 1 or STATUS_REG not idle)";
	case 47:  return "step 5: a step-5 VRAM page overlaps a live DMCUB window or another protected range";
	case 48:  return "step 5: GART slot 0 not bound to host page A, or a hub flush did not acknowledge";
	case 49:  return "step 5: latched failure (or this mode already timed out); sdma stop, then reload or the other mode";
	case 50:  return "step 5: GCVM_L2_PROTECTION_FAULT_STATUS not 0 before the step";
	case 51:  return "step 5: start in the other mode while the queue runs; sdma stop first";
	case 52:  return "step 5: db mode: BAR2 not mapped or too small, or the NBIO doorbell aperture not enabled";
	case 53:  return "step 5: not a test number (1 to 3) or a mode (mmio, db)";
	case 54:  return "step 6: out of order (cp start before C1 or G6, while SDMA0 runs, or twice in one mode; cp test before start, "
	                 "after stop or before the previous test; cp stop with no queue; plan or journal full)";
	case 55:  return "step 6: MEC or HQD not as expected before start (CP_MEC_RS64_CNTL not as C1 left it, CP_HQD_ACTIVE 1 or CP_HQD_ERROR set)";
	case 56:  return "step 6: latched failure (or this mode already timed out); cp stop, then reload or the other mode";
	case 57:  return "step 6: GART slot 1 not bound to host page B, or a hub flush did not acknowledge";
	case 58:  return "step 6: cp start in the other mode while the queue runs; cp stop first";
	case 59:  return "step 6: db mode: BAR2 not mapped or smaller than 0x20 bytes, or the NBIO doorbell aperture not enabled";
	case 60:  return "step 6: not a test number (1 to 5) or a mode (mmio, db)";
	case 61:  return "step 6: the embedded gfx11 shader blob is not the pinned one (size or sha256)";
	case 62:  return "step 7 T1: cp ib before cp start, or before a plain ring packet (cp test 1/K2 or test 2/K3) succeeded since the start";
	case 63:  return "step 7 T1: the IB page overlaps a live DMCUB window or another protected range";
	case 64:  return "step 7 W1: cp waitseq with no outstanding cp submit in this load, or a seqno that is not the outstanding one";
	case 65:  return "step 7 X4b: cp xib's xlat12-translated stream differs from the native builder, or xlat12 refused it (kext log: first diff)";
	case 66:  return "step 7 W2: bad argument (kind, size 0/unaligned/over 1 MiB, handle, VM flags, VA alignment, dword index, or copy src==dst / dst not writable)";
	case 67:  return "step 7 W2: out of order (G6 not done or GMC stopped; map a mapped BO, unmap/copy an unmapped one, free a mapped one; or a map/unmap/free while a submit is outstanding)";
	case 68:  return "step 7 W2: full (no free handle, no room in the VRAM slab, or host total over 4 MiB)";
	case 69:  return "step 7 W2: host allocation, wiring or kernel mapping failed, or a page at or above 44 bits";
	case 70:  return "step 7 W2: VA outside the BO window, overlapping another mapped BO, or (fault) a VA some BO maps";
	case 71:  return "step 7 W2: import page is device memory (inside a PCI BAR of this GPU), or no BAR list latched";
	case 72:  return "step 7 W2: a hub flush did not acknowledge after a map or unmap (the GMC is stopped)";
	case 73:  return "step 7 W2: gart map / gart unmap into the reserved BO VA window (slots 0x1000..0x1FFF)";
	case 74:  return "step 7 W2: the VRAM BO slab overlaps a live protected range when first taken";
	case 75:  return "step 7 W2V: bad argument (VA not canonical/4 KiB aligned/below 0x10000/straddling bit 47, size/offset, flags, PRT/NOALLOC, neither R nor W, bad handle)";
	case 76:  return "step 7 W2V: out of order (G6 not done or GMC stopped; vm create twice or a vm action before it; a submit outstanding)";
	case 77:  return "step 7 W2V: CONTEXT8 is not as G6 left it (masked CNTL), or its page-table base changed since vm create";
	case 78:  return "step 7 W2V: the page-table arena is full (nothing was written)";
	case 79:  return "step 7 W2V: VA overlaps a VMID-8 mapping; unmap of a (handle, VA) not mapped; or a fault VA that is mapped";
	case 80:  return "step 7 W2V: a VMID-8 engine-17 invalidate did not acknowledge in 100 ms (the VM is stopped)";
	case 81:  return "step 7 W2V: the page-table arena overlaps a live protected range when first taken";
	case 82:  return "step 7 W2V: IB VA not in an R|X VRAM mapping / stream too big; code VA not X-mapped; copy source not R-mapped or destination not W-mapped";
	case 83:  return "step 7 W2V: SH_MEM_CONFIG or SH_MEM_BASES under the VMID-8 select did not read back";
	case 84:  return "step 7 W2V: bo free of a BO that still has a VMID-8 mapping (vm unmap it first)";
	case 85:  return "step 7 W2V: vm fault VA inside the VMID-0 apertures other than the two V6 sentinels";
	case 200: return "dmub: a DMCUB write before dmub snapshot armed the gate";
	case 201: return "dmub: HUBP0 is not on the GOP scanout surface";
	case 202: return "dmub: the N33 framebuffer nub is published";
	case 203: return "dmub: the step-6/7 compute queue is running";
	case 204: return "dmub: out of order (load before snapshot/autoload, reset before load, init before reset)";
	case 205: return "dmub: STOP_FW not acked (logged; the kext proceeds to soft reset as Linux does)";
	case 206: return "dmub: DMCUB did not reach dal_fw + mailbox_rdy in 100 ms";
	case 207: return "dmub: inbox1 ring busy, wrong size, or the next slot would wrap";
	case 208: return "dmub: command template not on the allowlist (only QUERY_FEATURE_CAPS)";
	case 209: return "dmub: a DMUB window overlaps a live protected range";
	case 210: return "dmub: VBIOS image missing or wrong size";
	case 211: return "dmub: the PSP refused the type-51 DMUB load at this point";
	case 212: return "dmub: the staged firmware header is malformed, out of bounds, or not the pinned layout";
	case 220: return "ih: out of order (state regs or start before G6, start twice, recv before start or with its queue not ready, "
	                 "sdma source while the compute queue runs, a follow-up without its first variant's no-IV, a variant repeated "
	                 "after no IV, arm twice, stop with no ring, journal full)";
	case 221: return "ih: the IH ring block overlaps a live DMCUB window or another protected range";
	case 222: return "ih: GC or MMHUB fault status not 0 before the step";
	case 223: return "ih: latched (an unexpected IV, an overflow, a fault after, or a read-back that did not hold): ih stop, then reload";
	case 224: return "ih: MSI gate (n33-ih-msi not set, no MSI source, MSI already enabled by another owner, event source not "
	                 "registered; mode 2: MSI, MSI-X or INTx not confirmed off)";
	case 225: return "ih: GART slot 2 / host page C not bound or flushed, or the source's host page PTE is not the kext's";
	case 226: return "ih: not a mode (1, 2), a source (0 to 3) or a journal kind";
	default:
		if (why >= 100 && why < 200) { static char b[48]; snprintf(b, sizeof b, "overlaps protected %s", kindName(why - 100)); return b; }
		return "?";
	}
}

static int call(uint64_t action, const uint64_t *args, uint32_t n, uint64_t *out, uint32_t outn) {
	uint32_t got = outn;
	kern_return_t kr = n33(action, args, n, out, &got);
	if (kr == kIOReturnNotPermitted) {
		// IOKit copies no scalar output back on an error return, so out[0] is not the kext's reason.
		fprintf(stderr, "refused; the kext logs the reason: "
		        "n33ctl log | grep 'n33: refused' | tail -1, then n33ctl why <code>\n");
		return 1;
	}
	if (kr != kIOReturnSuccess) { fprintf(stderr, "call failed: 0x%x\n", kr); return 1; }
	return 0;
}

static int doStatus(void) {
	uint64_t o[11] = {0};
	if (call(aStatus, NULL, 0, o, 11)) return 1;
	printf("BAR0 size        %llu MiB (%#llx)\n", o[0] >> 20, o[0]);
	printf("FB base (GPU)    %#llx\n", o[1]);
	printf("FB offset        %#llx\n", o[2]);
	printf("GOP scanout      VRAM %#llx, len %#llx (%llu KiB)\n", o[3], o[4], o[4] >> 10);
	printf("HUBP0 surface    GPU %#llx (VRAM %#llx)\n", o[5], o[5] - o[1]);
	printf("HUBP0 pitch reg  %#llx (%llu px)\n", o[6], (o[6] & 0x3FFF) + 1);
	printf("HUBP0 flip ctl   %#llx\n", o[7]);
	printf("HUBP0 surf cfg   %#llx\n", o[9]);
	printf("BAR0 physical    %#llx\n", o[10]);
	printf("protected ranges %llu\n", o[8]);
	for (uint64_t i = 0; i < o[8]; i++) {
		uint64_t a = i, p[3] = {0};
		if (call(aProtected, &a, 1, p, 3)) return 1;
		printf("  [%llu] VRAM %#llx len %#llx  %s\n", i, p[0], p[1], kindName(p[2]));
	}
	return 0;
}

static void usage(void) {
	fprintf(stderr,
	    "usage: n33ctl <command>\n"
	    "  status                       card/FB/scanout state and protected VRAM ranges\n"
	    "  protected                    list protected VRAM ranges only\n"
	    "  reg <dword>                  read an absolute MMIO dword (RREG32)\n"
	    "  regwrite <dword> <val>       write an allowlisted MMIO dword\n"
	    "  r <bar0off>                  read a BAR0 (VRAM) dword\n"
	    "  w <bar0off> <val>            write a BAR0 dword (guarded)\n"
	    "  copy <src> <dst> <len>       copy within BAR0 (guarded dst)\n"
	    "  fill <dst> <len> <val>       fill BAR0 with a dword (guarded)\n"
	    "  flip <bar0off>               point HUBP0 scanout at a VRAM offset (latches)\n"
	    "  psp check|load <sos.bin> [off]  PSP 13.0.7 bootloader chain (check is read-only)\n"
	    "  psp2 check <fwdir> <smu.bin> | 2a <fwdir> <smu.bin> | 2b <smu.bin> --power-cycled [--size-ok] | 2c <fwdir> | state | destroy\n"
	    "                               psp2 send <cmd> <type> <off> <size> | ring: raw, for psp2-negative.sh only\n"
	    "                               PSP milestone 2: KM ring, TOC, TMR, GC firmware, RLC autoload\n"
	    "  smu check | s1 | s2 | s3 | state      SMU 13.0.7 mailbox (check is read-only; s1 sends queries)\n"
	    "                               smu send <msg> <arg>: raw, for smu-negative.sh only\n"
	    "  gmc state | g1 | g2 | g3 | g4 | g5 | g6 | restore   GMC 11 / MMHUB / GFXHUB / GART (state is G0, read-only)\n"
	    "                               gmc flags | step <n> | map <slot> <count> <kind> <arg> | unmap <slot> <count> | flush:\n"
	    "                               raw, for gmc-negative.sh only (flags reads no register)\n"
	    "  gfx rs64                     C1: CP RS64 start addresses and pipe resets (after psp2 2c, before gmc g6)\n"
	    "  sdma state | start mmio|db | test 1|2|3 | stop   SDMA0 queue 0 and the first GART DMA (state is C0, read-only)\n"
	    "                               sdma flags | journal <i> | start <n>: raw, for sdma-negative.sh only (flags reads no register)\n"
	    "  cp state | hqd | start db|mmio | test 1..5 | stop   step 6: one MEC compute queue, K2 to K5b (state, hqd read-only)\n"
	    "  cp ib                        step 7 T1: K4's store_magic dispatch run from a VRAM IB through INDIRECT_BUFFER (after cp test 2/K3)\n"
	    "  cp submit | waitseq <seq>    step 7 W1: the winsys submit->wait split of cp ib (submit returns a seqno, waitseq polls its fence)\n"
	    "  cp xib 1|2                   step 7 X4b: a fixed gfx10.3 stream (1 wd, 2 store_magic dispatch) translated by xlat12, submitted\n"
	    "                               as cp submit; retire it with cp waitseq <seq>\n"
	    "                               cp flags | journal <i> | start <n>: raw, for cp-negative.sh only (flags reads no register)\n"
	    "  dmub snapshot|reset|init|query|check | load <dcn_3_2_1_dmcub.bin>   stage 3 DMCUB proof (snapshot is read-only)\n"
	    "                               order: snapshot, load, reset, init, query (QUERY_FEATURE_CAPS = the proof), check\n"
	    "  ih state | start 1|2 | recv 0..3 | arm | stop   step 4: IH 6.0.2 ring 0 (state is I0, read-only; start 1 polled,\n"
	    "                               2 polled with ENABLE_INTR 1; recv 0 cp EOP, 1 cp + TIME_STAMP_INT_ENABLE, 2 sdma trap\n"
	    "                               after cp stop, 3 sdma + TRAP_ENABLE; arm = MSI, boot-arg n33-ih-msi=1 only)\n"
	    "                               ih flags | journal <i> | iv <i> | journal-kind <k>: flags reads no register; journal-kind is raw, for ih-negative.sh\n"
	    "  bo create vram|gtt <bytes> | import <bytes> | map <h> <va> [flags] | unmap <h> | free <h>   step 7 W2: buffer objects in VMID 0\n"
	    "  bo info <h> | read <h> <dword> | copy <src> <dst> | fault <va> | import-test <vram h> <va>   (copy/fault return a seqno; retire with cp waitseq)\n"
	    "  vm create | map <h> <va> [<off> <size> [flags]] | unmap <h> <va> | info [va] | destroy         step 7 W2V: a per-process GPU VM in VMID 8\n"
	    "  vm copy <ib va> <src va> <dst va> | dispatch <ib va> <code va> <dst va> | fault <ib va> <va>    (return a seqno; retire with cp waitseq)\n"
	    "  fbsum [bytes]                sha256 of the GOP scanout in VRAM (read-only; default length from status)\n"
	    "  log                          print the kext's own log (holds every refusal line, unlike the unified log)\n"
	    "  why <code>                   explain a refusal code from n33ctl log (no kext needed)\n"
	    "all offsets/dwords accept 0x.. ; writes are re-checked in the kext\n");
}

// ---- psp: the PSP 13.0.7 bootloader chain (stage 3 step 1) ----------------------------------------
// n33ctl psp check <sos.bin> [window-offset]   read-only: container, window, register state
// n33ctl psp load  <sos.bin> [window-offset]   run KDB, SPL (the KDB image, as Linux), SYS_DRV, SOS
enum { kC2p35 = 0x16063, kC2p36 = 0x16064, kC2p58 = 0x1607A, kC2p64 = 0x16080, kC2p81 = 0x16091 };
enum { kBlSysDrv = 0x10000, kBlSos = 0x20000, kBlKdb = 0x80000, kBlSpl = 0x10000000 };
enum { kFwSos = 1, kFwSysDrv = 2, kFwKdb = 3 };
enum { kPspSosBytes = 248496, kPspWinBytes = 1 << 20, kPspDefaultWin = 0x8000000 };
enum { kMmhubFbOffset = 0x1A857, kMmhubFbBase = 0x1A86C, kMmhubFbTop = 0x1A86D };
enum { kSurfOff = 0x2000000, kSurfBytes = 14745600 };   // the stage 2 framebuffer surface (n33fb::kSurfOff, kBytes)
static const char kPspSosSha256[] = "94db505fa6482f258c33a0a8d412050f6d843ab4ada368252e988f82f8a26fa8";

struct pspimg { const uint8_t *p; uint32_t n; uint32_t ver; };

static int pspRd(uint64_t dw, uint32_t *v) {
	uint64_t out[1] = {0}; uint32_t got = 1;
	if (IOConnectCallScalarMethod(conn, kSelRegRead, &dw, 1, out, &got) != kIOReturnSuccess) return 1;
	*v = (uint32_t)out[0];
	return 0;
}

static int pspWr(uint64_t dw, uint32_t v) {
	uint64_t a[2] = { dw, v }, o[3] = {0};
	return call(aRegWrite, a, 2, o, 3);
}

// Strict, as psp_v13_0_wait_for_bootloader: exactly 0x80000000 is ready; bit 31 with a non-zero low
// byte is an error code; 0xffffffff is a card that does not answer.
static int pspWaitIdle(unsigned ms, uint32_t *last) {
	for (unsigned t = 0; t < ms; t++) {
		uint32_t v;
		if (pspRd(kC2p35, &v)) return 2;
		*last = v;
		if (v == 0xFFFFFFFFu) return 2;
		if (v == 0x80000000u) return 0;
		if ((v & 0x80000000u) && (v & 0xFFu)) return 3;
		usleep(1000);
	}
	return 1;
}

static int pspParse(const uint8_t *b, size_t n, struct pspimg *kdb, struct pspimg *sys, struct pspimg *sos) {
	uint16_t major, minor; uint32_t arr, cnt;
	if (n < 40) return 1;
	memcpy(&major, b + 8, 2); memcpy(&minor, b + 10, 2);
	memcpy(&arr, b + 24, 4); memcpy(&cnt, b + 32, 4);
	const size_t d = 36 + (minor >= 1 ? 4 : 0);
	if (major != 2 || arr > n || cnt == 0 || cnt > 32 || d + (size_t)cnt * 16 > n) return 1;
	for (uint32_t i = 0; i < cnt; i++) {
		uint32_t t, ver, off, sz;
		const uint8_t *q = b + d + (size_t)i * 16;
		memcpy(&t, q, 4); memcpy(&ver, q + 4, 4); memcpy(&off, q + 8, 4); memcpy(&sz, q + 12, 4);
		if ((uint64_t)arr + off + sz > n) return 1;
		struct pspimg im = { b + arr + off, sz, ver };
		if (t == kFwKdb) *kdb = im; else if (t == kFwSysDrv) *sys = im; else if (t == kFwSos) *sos = im;
	}
	return !(kdb->n && sys->n && sos->n && !(kdb->n & 3) && !(sys->n & 3) && !(sos->n & 3));
}

static int pspStage(uint64_t winOff, const struct pspimg *im) {
	uint64_t f[3] = { winOff, kPspWinBytes, 0 }, o[1] = {0};
	if (call(aFill, f, 3, o, 1)) return 1;
	for (uint32_t i = 0; i < im->n; i += 4) {
		uint32_t w; memcpy(&w, im->p + i, 4);
		uint64_t a[2] = { winOff + i, w };
		if (call(aBar0Write, a, 2, o, 1)) return 1;
	}
	return 0;
}

static int doPsp(int argc, char **argv) {
	const int load = !strcmp(argv[2], "load");
	if (!load && strcmp(argv[2], "check")) { usage(); return 2; }
	FILE *f = fopen(argv[3], "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", argv[3]); return 1; }
	static uint8_t buf[kPspSosBytes + 1];
	const size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	if (n != kPspSosBytes) { fprintf(stderr, "refusing: %zu bytes, expected %d\n", n, kPspSosBytes); return 1; }
	uint8_t dg[CC_SHA256_DIGEST_LENGTH]; char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
	CC_SHA256(buf, (CC_LONG)n, dg);
	for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", dg[i]);
	if (strcmp(hex, kPspSosSha256)) { fprintf(stderr, "refusing: sha256 %s\n", hex); return 1; }
	struct pspimg kdb = {0}, sys = {0}, sos = {0};
	if (pspParse(buf, n, &kdb, &sys, &sos)) { fprintf(stderr, "refusing: container did not parse\n"); return 1; }
	printf("container ok: KDB %u B, SYS_DRV %u B (ver %#x), SOS %u B (ver %#x)\n", kdb.n, sys.n, sys.ver, sos.n, sos.ver);

	uint64_t s[9] = {0};
	if (call(aStatus, NULL, 0, s, 9)) return 1;
	const uint64_t winOff = argc > 4 ? num(argv[4]) : kPspDefaultWin;
	const uint32_t winReg = (uint32_t)((s[1] + winOff) >> 20);
	printf("window: VRAM %#llx (GPU %#llx), C2PMSG_36 will be %#x\n", winOff, s[1] + winOff, winReg);
	if (winOff < (16u << 20) || winOff > s[0] || s[0] - winOff < kPspWinBytes) {
		fprintf(stderr, "refusing: window outside BAR0 or below the 16 MiB floor\n"); return 1;
	}
	if (winOff < (uint64_t)kSurfOff + kSurfBytes && kSurfOff < winOff + kPspWinBytes) {
		fprintf(stderr, "refusing: window overlaps the framebuffer surface (VRAM %#x, %u bytes)\n", kSurfOff, kSurfBytes); return 1;
	}
	for (uint64_t i = 0; i < s[8]; i++) {
		uint64_t a = i, p[3] = {0};
		if (call(aProtected, &a, 1, p, 3)) return 1;
		const int hit = winOff < p[0] + p[1] && p[0] < winOff + kPspWinBytes;
		printf("protected [%llu] VRAM %#llx len %#llx: %s\n", i, p[0], p[1], hit ? "OVERLAPS THE WINDOW" : "clear");
		if (hit) { fprintf(stderr, "refusing: window overlaps a protected range\n"); return 1; }
	}
	uint32_t mbase = 0, mtop = 0, moff = 0;
	if (pspRd(kMmhubFbBase, &mbase) || pspRd(kMmhubFbTop, &mtop) || pspRd(kMmhubFbOffset, &moff)) {
		fprintf(stderr, "MMHUB register read failed\n"); return 1;
	}
	printf("MMHUB: FB_LOCATION_BASE %#x  TOP %#x  FB_OFFSET %#x  (DCN FB base %#llx)\n", mbase, mtop, moff, s[1]);
	if ((((uint64_t)(mbase & 0xFFFFFFu)) << 24) != s[1]) { fprintf(stderr, "refusing: MMHUB FB base disagrees with the DCN one\n"); return 1; }
	if (s[1] + winOff + kPspWinBytes > (((uint64_t)(mtop & 0xFFFFFFu)) << 24) + (1ull << 24)) {
		fprintf(stderr, "refusing: window ends past MMHUB FB_LOCATION_TOP\n"); return 1;
	}

	uint32_t c35 = 0, c36 = 0, c58 = 0, c64 = 0, c81 = 0;
	if (pspRd(kC2p35, &c35) || pspRd(kC2p36, &c36) || pspRd(kC2p58, &c58) || pspRd(kC2p64, &c64) || pspRd(kC2p81, &c81)) {
		fprintf(stderr, "register read failed\n"); return 1;
	}
	printf("before: C2PMSG_35 %#x  _36 %#x  _58 %#x  _64 %#x  _81 %#x\n", c35, c36, c58, c64, c81);
	if (c81 != 0) { printf("sOS already alive (C2PMSG_81 %#x): nothing to do\n", c81); return 0; }
	if (c35 != 0x80000000u) { printf("ABORT: bootloader is not idle (C2PMSG_35 %#x)\n", c35); return 1; }
	if (!load) { printf("check ok: the chain would run\n"); return 0; }

	const struct { const char *name; const struct pspimg *im; uint32_t cmd; } steps[] = {
		{ "KDB", &kdb, kBlKdb },
		{ "SPL (KDB image, as Linux)", &kdb, kBlSpl },
		{ "SYS_DRV", &sys, kBlSysDrv },
	};
	for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++) {
		uint32_t last = 0, alive = 0;
		if (pspRd(kC2p81, &alive)) return 1;
		if (alive) { printf("%s: sOS alive, skipped\n", steps[i].name); continue; }
		if (pspStage(winOff, steps[i].im) || pspWr(kC2p36, winReg) || pspWr(kC2p35, steps[i].cmd)) {
			printf("FAILED at %s: write refused\n", steps[i].name); return 1;
		}
		const int w = pspWaitIdle(10000, &last);
		if (w) { printf("FAILED at %s: wait %d, C2PMSG_35 %#x\n", steps[i].name, w, last); return 1; }
		printf("%s: ok (cmd %#x, status %#x)\n", steps[i].name, steps[i].cmd, last);
	}

	uint32_t before = 0, last = 0;
	if (pspRd(kC2p81, &before)) return 1;
	if (before == 0) {
		if (pspWaitIdle(10000, &last)) { printf("FAILED before SOS: C2PMSG_35 %#x\n", last); return 1; }
		if (pspStage(winOff, &sos) || pspWr(kC2p36, winReg) || pspWr(kC2p35, kBlSos)) { printf("FAILED at SOS: write refused\n"); return 1; }
		usleep(20000);
		uint32_t v = 0;
		for (unsigned t = 0; t < 5000; t++) {
			if (pspRd(kC2p81, &v)) return 1;
			if (v != 0 && v != before && v != 0xFFFFFFFFu) break;
			usleep(1000);
		}
		if (v == 0 || v == 0xFFFFFFFFu) { pspRd(kC2p35, &last); printf("FAILED at SOS: C2PMSG_81 %#x, C2PMSG_35 %#x\n", v, last); return 1; }
	}
	if (pspRd(kC2p81, &c81) || pspRd(kC2p58, &c58) || pspRd(kC2p35, &c35)) return 1;
	printf("after: C2PMSG_81 %#x  _58 %#x  _35 %#x\n", c81, c58, c35);
	printf("%s\n", (c81 != 0 && c58 == sos.ver) ? "sOS ALIVE, version matches the container" : "sOS alive, version differs from the container: record it");
	return 0;
}

// ---- psp2: PSP 13.0.7 milestone 2 (stage 3 step 1; overview repo stage3-psp-m2-design.md) ----------------
// n33ctl psp2 check <fwdir> <smu.bin>        read-only preflight (D10)
// n33ctl psp2 2a <fwdir> <smu.bin>            preflight, ring create, LOAD_TOC (prints the TMR size)
// n33ctl psp2 2b <smu.bin> --power-cycled [--size-ok]   LOAD_IP_FW SMU, SETUP_TMR
// n33ctl psp2 2c <fwdir>                      26 x LOAD_IP_FW, AUTOLOAD_RLC, then the RLC/CP check
// n33ctl psp2 state | destroy
// The kext builds every ring frame and command; this tool stages images through the guarded BAR0 path
// and decides the order. Any non-zero PSP status or a timeout ends the run, and the kext refuses every
// later command until it is reloaded (D5).
enum { aPspRing = 8, aPspCmd = 9, aPspDestroy = 10, aPspState = 11 };
enum { cToc = 1, cIpFw = 2, cTmr = 3, cAutoload = 4 };
#define kP2FwPri      0x8000000ull
#define kP2FwBuf      0x8200000ull
#define kP2FwBufSize  0x300000ull
#define kP2TmrOff     0x3FC000000ull
#define kP2TmrMax     (32ull << 20)
#define kP2TmrExpect  0x1300000u        // LOAD_TOC's answer under Linux on this card (dmesg)
#define kP2SmuVer     0x00525F00u       // 82.95.0, the running PMFW (D7)
enum { kC2p33 = 0x16061, kC2p67 = 0x16083, kHdpRemap = 0xE4D,
       kMp1C2p66 = 0x16282, kMp1C2p82 = 0x16292, kMp1C2p90 = 0x1629A,
       kRlcBootload = 0xEE82, kCpStat = 0x21A0, kImuScratch0 = 0xE068 };
static const struct { uint32_t reg; const char *name; } kP2Mmhub[] = {
	{ 0x1A86C, "FB_LOCATION_BASE" }, { 0x1A86D, "FB_LOCATION_TOP" }, { 0x1A857, "FB_OFFSET" },
	{ 0x1A86E, "AGP_TOP" }, { 0x1A86F, "AGP_BOT" }, { 0x1A870, "AGP_BASE" },
	{ 0x1A871, "SYSTEM_APERTURE_LOW" }, { 0x1A872, "SYSTEM_APERTURE_HIGH" },
	{ 0x1A873, "MX_L1_TLB_CNTL" }, { 0x1A6C0, "VM_CONTEXT0_CNTL" },
};
// D11: linux-firmware afabaf77, except the SMU image (217ca6e4, the file Ubuntu loaded on this card).
static const struct { const char *file; uint32_t size; const char *sha; } kP2Pins[] = {
	{ "sdma_6_0_2.bin",      34560,  "16c374344d2894da751f7028f9ec1f7520035fa9548d8c750d99a00a4afa86c7" },
	{ "gc_11_0_2_pfp.bin",   234944, "9020f53788ad881fa01aa656fc082f9f8d3cdfc81f70aaac0bed6e6001491128" },
	{ "gc_11_0_2_me.bin",    324080, "f5fe48f97acbd3ce13b35929290bfbac01ce522631cc91dcef1fdeb3ff35c8ed" },
	{ "gc_11_0_2_mec.bin",   406528, "19bf080d6e672de5ed3fb86e3fdbdda4d700d8e3bda2dbdcc923101484ad645b" },
	{ "gc_11_0_2_mes_2.bin", 263696, "719f446830ec8766f47b9cfcf1dc606b77dbf720a9e868c4cfcdc60e0767d1a7" },
	{ "gc_11_0_2_mes1.bin",  221408, "5f8f0b50b7a9f8c34f68ee516a51968ed190cb0d0fb36c99e5301799dd65097c" },
	{ "gc_11_0_2_imu.bin",   132352, "9befca62b0b0cfd252c3df4a9edca295526f4d43821cd99a6326454995a6ca2d" },
	{ "gc_11_0_2_rlc.bin",   178848, "b43eb2fd0600f50a1a5796bc9983d6b39b5c20960234920f5e89cb362193e0b8" },
	{ "psp_13_0_7_sos.bin",  248496, "94db505fa6482f258c33a0a8d412050f6d843ab4ada368252e988f82f8a26fa8" },
	{ "smu_13_0_7.bin",      287080, "ccecc0fd0196b9613c920a51c2fd9436e739ff19dda5bdf74d97562387231732" },
};
enum { kPinSdma, kPinPfp, kPinMe, kPinMec, kPinMes2, kPinMes1, kPinImu, kPinRlc, kPinSos, kPinSmu, kPins };

static void sha256hex(const uint8_t *b, size_t n, char hex[65]) {
	uint8_t dg[CC_SHA256_DIGEST_LENGTH];
	CC_SHA256(b, (CC_LONG)n, dg);
	for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", dg[i]);
}

// Read a pinned file (path = dir/name, or the full path when dir is NULL); refuse on size or sha256.
static uint8_t *p2load(const char *dir, const char *path, int pin, size_t *n) {
	char full[1024];
	snprintf(full, sizeof full, "%s%s%s", dir ? dir : "", dir ? "/" : "", dir ? kP2Pins[pin].file : path);
	FILE *f = fopen(full, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", full); return NULL; }
	uint8_t *b = malloc(kP2Pins[pin].size + 1);
	*n = b ? fread(b, 1, kP2Pins[pin].size + 1, f) : 0;
	fclose(f);
	char hex[65];
	if (!b || *n != kP2Pins[pin].size) { fprintf(stderr, "refusing %s: %zu bytes, pinned %u\n", full, *n, kP2Pins[pin].size); free(b); return NULL; }
	sha256hex(b, *n, hex);
	if (strcmp(hex, kP2Pins[pin].sha)) { fprintf(stderr, "refusing %s: sha256 %s\n", full, hex); free(b); return NULL; }
	return b;
}

static int p2stage(uint64_t off, const uint8_t *p, uint32_t n, const char *what) {
	uint64_t o[1] = {0};
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = 0;
		memcpy(&w, p + i, n - i < 4 ? n - i : 4);
		uint64_t a[2] = { off + i, w };
		if (call(aBar0Write, a, 2, o, 1)) { fprintf(stderr, "staging %s failed at VRAM %#llx\n", what, off + i); return 1; }
	}
	return 0;
}

static int p2state(uint64_t s[8]) {
	uint32_t got = 8;
	if (n33(aPspState, NULL, 0, s, &got) != kIOReturnSuccess) { fprintf(stderr, "psp2 state call failed\n"); return 1; }
	return 0;
}
static void p2printState(const uint64_t *s) {
	printf("kext: ring %llu toc %llu smu %llu tmr %llu rlcG %llu autoload %llu FAILED %llu; tmr answer %#llx; fence %llu; "
	       "C2PMSG_64 %#llx _67 %#llx _81 %#llx\n", s[0] & 1, s[0] >> 1 & 1, s[0] >> 2 & 1, s[0] >> 3 & 1, s[0] >> 4 & 1,
	       s[0] >> 5 & 1, s[0] >> 6 & 1, s[1], s[2], s[5], s[6], s[7]);
}

// resp.fw_addr is the "FW address within TMR" (psp_gfx_if.h:452) but its form (MC, physical, or an offset
// into the TMR) is not documented, so any of the three is accepted, with the whole image inside the TMR.
static int p2inTmr(uint64_t a, uint64_t n, uint64_t mc, uint64_t pa, uint64_t len, const char **form) {
	const uint64_t base[3] = { mc, pa, 0 };
	static const char *const nm[3] = { "MC address", "physical address", "TMR offset" };
	for (int i = 0; i < 3; i++)
		if (a >= base[i] && a - base[i] <= len && n <= len - (a - base[i])) { *form = nm[i]; return 1; }
	return 0;
}

// One ring command. 0 only when the PSP fenced it with status 0. *o gets fenced, status, tmr_size, fw_addr.
static int p2cmd(uint32_t cmd, uint32_t type, uint64_t off, uint64_t size, const char *name, uint64_t o[7]) {
	uint64_t a[4] = { cmd, type, off, size };
	memset(o, 0, 7 * sizeof o[0]);
	if (call(aPspCmd, a, 4, o, 7)) { printf("%s: REFUSED by the kext\n", name); return 1; }
	printf("%-26s index %llu  %s  status %#llx  fw_addr %#llx  tmr_size %#llx\n", name, o[5],
	       o[1] ? "fenced" : "TIMEOUT", o[2], o[4], o[3]);
	return !(o[1] == 1 && o[2] == 0);
}

// D10: the read-only preflight. 2a runs it first and stops on any refusal. 0 = ok.
static int p2check(const char *fwdir, const char *smu, const uint64_t *st, uint64_t fbBase, const uint64_t *s) {
	int bad = 0;
	p2printState(st);
	const uint32_t regs[] = { kC2p81, kC2p58, kC2p64, kC2p67, kC2p33, kC2p35, kC2p36, kHdpRemap };
	const char *names[] = { "C2PMSG_81", "C2PMSG_58", "C2PMSG_64", "C2PMSG_67", "C2PMSG_33", "C2PMSG_35", "C2PMSG_36", "HDP remap (0xe4d)" };
	uint32_t v[8] = {0}, mm[10] = {0};
	for (int i = 0; i < 8; i++) { if (pspRd(regs[i], &v[i])) return 1; printf("%-20s %#x\n", names[i], v[i]); }
	for (int i = 0; i < 10; i++) { if (pspRd(kP2Mmhub[i].reg, &mm[i])) return 1; printf("MMHUB %-20s %#x\n", kP2Mmhub[i].name, mm[i]); }
	if (v[0] == 0) { printf("REFUSE: sOS not alive (C2PMSG_81 0): run milestone 1 first\n"); bad = 1; }
	if ((v[2] & 0x8000FFFFu) != 0x80000000u) { printf("REFUSE: C2PMSG_64 not ready for ring creation\n"); bad = 1; }
	if (v[3] != 0 && !(st[0] & 1)) { printf("REFUSE: C2PMSG_67 is %#x with no ring of ours (a ring from an earlier load?)\n", v[3]); bad = 1; }
	const uint64_t fbLo = (uint64_t)(mm[0] & 0xFFFFFFu) << 24, fbHi = ((uint64_t)(mm[1] & 0xFFFFFFu) << 24) + (1ull << 24);
	const uint64_t lo = fbBase + kP2FwPri, hi = fbBase + kP2TmrOff + kP2TmrMax;
	printf("plan: MC %#llx .. %#llx; FB_LOCATION %#llx .. %#llx\n", lo, hi, fbLo, fbHi);
	if (fbLo != fbBase || lo < fbLo || hi > fbHi) { printf("REFUSE: the plan's MC addresses fall outside FB_LOCATION\n"); bad = 1; }
	const uint64_t apLo = (uint64_t)mm[6] << 18, apHi = ((uint64_t)mm[7] << 18) + (1ull << 18);
	if (mm[7] > mm[6]) {
		printf("system aperture %#llx .. %#llx: %s\n", apLo, apHi, (lo >= apLo && hi <= apHi) ? "plan inside" : "PLAN OUTSIDE");
		if (!(lo >= apLo && hi <= apHi)) bad = 1;
	} else printf("system aperture not set (LOW %#x HIGH %#x)\n", mm[6], mm[7]);
	const struct { uint64_t off, len; const char *what; } plan[] = {
		{ kP2FwPri, 1ull << 20, "fw_pri" }, { 0x8100000, 0x3000, "ring, command, fence" },
		{ kP2FwBuf, kP2FwBufSize, "fw_buf" }, { kP2TmrOff, kP2TmrMax, "TMR (up to 32 MiB)" } };
	for (uint64_t i = 0; i < s[8]; i++) {
		uint64_t a = i, p[3] = {0};
		if (call(aProtected, &a, 1, p, 3)) return 1;
		printf("protected [%llu] VRAM %#llx len %#llx  %s\n", i, p[0], p[1], kindName(p[2]));
		for (unsigned k = 0; k < sizeof plan / sizeof plan[0]; k++)
			if (plan[k].off < p[0] + p[1] && p[0] < plan[k].off + plan[k].len && p[2] != 12) {
				printf("REFUSE: %s overlaps protected [%llu]\n", plan[k].what, i); bad = 1;
			}
	}
	
	for (int i = 0; i < kPins; i++) {
		size_t n = 0;
		uint8_t *b = i == kPinSmu ? p2load(NULL, smu, i, &n) : p2load(fwdir, NULL, i, &n);
		if (!b) { bad = 1; continue; }
		printf("firmware %-20s %zu bytes, ucode_version %#x, sha256 ok\n", kP2Pins[i].file, n, n33_rd32(b, 16));
		if (i == kPinSmu && n33_rd32(b, 16) != kP2SmuVer) { printf("REFUSE: SMU image is not 82.95.0\n"); bad = 1; }
		free(b);
	}
	printf("%s\n", bad ? "check FAILED: do not run 2a" : "check ok");
	return bad;
}

static int doPsp2(int argc, char **argv) {
	const char *sub = argv[2];
	uint64_t st[8] = {0}, s[9] = {0}, o[7] = {0};
	if (p2state(st) || call(aStatus, NULL, 0, s, 9)) return 1;
	const uint64_t fbBase = s[1];

	if (!strcmp(sub, "state")) { p2printState(st); return 0; }
	// Raw access for the negative tests (tools/n33/psp2-negative.sh). The kext checks every call; a command
	// it accepts is really sent, so use these only for commands that must be refused.
	if (!strcmp(sub, "send") && argc == 7) {
		const char *names[] = { "?", "LOAD_TOC", "LOAD_IP_FW", "SETUP_TMR", "AUTOLOAD_RLC" };
		const uint64_t c = num(argv[3]);
		return p2cmd((uint32_t)c, (uint32_t)num(argv[4]), num(argv[5]), num(argv[6]), c <= 4 ? names[c] : "?", o);
	}
	if (!strcmp(sub, "ring")) {
		uint64_t r[3] = {0};
		if (call(aPspRing, NULL, 0, r, 3)) return 1;
		printf("ring create: %s, C2PMSG_64 %#llx\n", r[2] ? "ok" : "NO ANSWER", r[1]);
		return r[2] ? 0 : 1;
	}
	if (!strcmp(sub, "destroy")) {
		uint64_t d[3] = {0};
		if (call(aPspDestroy, NULL, 0, d, 3)) return 1;
		printf("ring destroyed: %s, C2PMSG_64 %#llx (no more commands until the kext is reloaded)\n", d[2] ? "ok" : "NO ANSWER", d[1]);
		return d[2] ? 0 : 1;
	}

	if (!strcmp(sub, "check") && argc == 5) return p2check(argv[3], argv[4], st, fbBase, s);

	if (!strcmp(sub, "2a") && argc == 5) {
		if (st[0] != 0) { printf("refusing: the kext already has PSP state (reload the kext; see psp2 state)\n"); p2printState(st); return 1; }
		if (p2check(argv[3], argv[4], st, fbBase, s)) return 1;   // D10: the preflight is not optional
		size_t n = 0;
		uint8_t *sos = p2load(argv[3], NULL, kPinSos, &n);
		struct n33img toc;
		if (!sos || n33_fw_toc(sos, n, &toc)) { fprintf(stderr, "TOC not found\n"); free(sos); return 1; }
		uint64_t f[3] = { kP2FwPri, 1ull << 20, 0 }, fo[1];
		int rc = call(aFill, f, 3, fo, 1) || p2stage(kP2FwPri, toc.p, toc.n, "TOC");
		free(sos);
		if (rc) return 1;
		printf("TOC staged at VRAM %#llx (%u bytes)\n", kP2FwPri, toc.n);
		uint64_t r[3] = {0};
		if (call(aPspRing, NULL, 0, r, 3)) { printf("ring create REFUSED\n"); return 1; }
		printf("ring create: %s, C2PMSG_64 %#llx\n", r[2] ? "ok" : "NO ANSWER", r[1]);
		if (!r[2]) return 1;
		if (p2cmd(cToc, 0, 0, toc.n, "LOAD_TOC", o)) return 1;
		uint32_t c81 = 0;
		pspRd(kC2p81, &c81);
		printf("C2PMSG_81 %#x, C2PMSG_67 %#llx\n", c81, o[6]);
		if (o[3] == kP2TmrExpect) { printf("2a done: TMR size %#llx as predicted. 2b is a separate go.\n", o[3]); return 0; }
		if (o[3] && o[3] <= kP2TmrMax) printf("2a done, STOP: D6 address unchanged, size %#llx differs from the prediction %#x. 2b is William's call (--size-ok).\n", o[3], kP2TmrExpect);
		else printf("2a done, STOP: TMR size %#llx is 0 or above 32 MiB; a new placement is designed before 2b.\n", o[3]);
		return 1;
	}

	if (!strcmp(sub, "2b") && argc >= 5) {
		int cycled = 0, sizeOk = 0;
		for (int i = 4; i < argc; i++) { if (!strcmp(argv[i], "--power-cycled")) cycled = 1; else if (!strcmp(argv[i], "--size-ok")) sizeOk = 1; }
		if (!cycled) { printf("refusing: 2b runs at most once per power cycle (D13); pass --power-cycled to state it\n"); return 1; }
		if ((st[0] & 0x4F) != 0x3) { printf("refusing: needs ring and LOAD_TOC done, nothing after, no failure\n"); p2printState(st); return 1; }
		if (st[1] != kP2TmrExpect && !sizeOk) { printf("refusing: TMR size %#llx is not the predicted %#x (Q9: William's call, --size-ok)\n", st[1], kP2TmrExpect); return 1; }
		size_t n = 0;
		uint8_t *smu = p2load(NULL, argv[3], kPinSmu, &n);
		struct n33img im; uint32_t ver = 0;
		if (!smu || n33_fw_smu(smu, n, &im, &ver) || ver != kP2SmuVer) { fprintf(stderr, "SMU image refused\n"); free(smu); return 1; }
		uint32_t m[3][2] = {{0}};
		const uint32_t mr[3] = { kMp1C2p90, kMp1C2p66, kMp1C2p82 };
		for (int i = 0; i < 3; i++) pspRd(mr[i], &m[i][0]);
		printf("before: MP1 C2PMSG_90 %#x _66 %#x _82 %#x\n", m[0][0], m[1][0], m[2][0]);
		uint64_t f[3] = { kP2FwBuf, (im.n + 0xFFFull) & ~0xFFFull, 0 }, fo[1];
		int rc = call(aFill, f, 3, fo, 1) || p2stage(kP2FwBuf, im.p, im.n, "SMU");
		free(smu);
		if (rc) return 1;
		if (p2cmd(cIpFw, 18, kP2FwBuf, im.n, "LOAD_IP_FW SMU (82.95.0)", o)) return 1;
		if (p2cmd(cTmr, 0, 0, 0, "SETUP_TMR", o)) return 1;
		for (int i = 0; i < 3; i++) pspRd(mr[i], &m[i][1]);
		uint32_t c81 = 0;
		pspRd(kC2p81, &c81);
		printf("after: MP1 C2PMSG_90 %#x _66 %#x _82 %#x; C2PMSG_81 %#x\n", m[0][1], m[1][1], m[2][1], c81);
		printf("2b done%s. Check the monitor still shows the desktop.\n", c81 ? "" : ", BUT C2PMSG_81 is 0");
		return c81 ? 0 : 1;
	}

	if (!strcmp(sub, "2c") && argc == 4) {
		if ((st[0] & 0x7F) != 0xF) { printf("refusing: needs ring, LOAD_TOC, SMU and SETUP_TMR done, nothing after, no failure\n"); p2printState(st); return 1; }
		uint8_t *b[kPinRlc + 1] = {0}; size_t n[kPinRlc + 1] = {0};
		int rc = 0;
		for (int i = 0; i <= kPinRlc && !rc; i++) if (!(b[i] = p2load(argv[3], NULL, i, &n[i]))) rc = 1;
		struct n33files fl;
		struct n33img gc[26];
		uint64_t offs[26], used = 0;
		if (!rc) {
			for (int i = 0; i < 7; i++) { fl.p[i] = b[i]; fl.n[i] = n[i]; }
			rc = n33_fw_gc(&fl, b[kPinRlc], n[kPinRlc], gc);
			if (!rc) used = n33_pack(gc, 26, offs);
			if (!rc && used > kP2FwBufSize) rc = 1;
		}
		if (!rc) {
			uint64_t f[3] = { kP2FwBuf, used, 0 }, fo[1];
			rc = call(aFill, f, 3, fo, 1);
			for (int i = 0; i < 26 && !rc; i++) {
				rc = p2stage(kP2FwBuf + offs[i], gc[i].p, gc[i].n, gc[i].name);
				printf("staged %-26s VRAM %#llx  %#x bytes\n", gc[i].name, kP2FwBuf + offs[i], gc[i].n);
			}
		}
		const uint64_t tmrMc = fbBase + kP2TmrOff, tmrLen = ((uint64_t)st[1] + 0xFFF) & ~0xFFFull;
		for (int i = 0; i < 26 && !rc; i++) {
			rc = p2cmd(cIpFw, gc[i].type, kP2FwBuf + offs[i], gc[i].n, gc[i].name, o);
			const char *form = NULL;
			if (!rc && !p2inTmr(o[4], gc[i].n, tmrMc, kP2TmrOff, tmrLen, &form)) { printf("STOP: %s landed at %#llx, not inside the TMR in any form\n", gc[i].name, o[4]); rc = 1; }
			if (!rc && i == 0) printf("fw_addr form: %s\n", form);
		}
		for (int i = 0; i <= kPinRlc; i++) free(b[i]);
		if (rc) return 1;
		if (p2cmd(cAutoload, 0, 0, 0, "AUTOLOAD_RLC", o)) return 1;
		uint32_t boot = 0, cp = 0, imu = 0;
		for (int t = 0; t < 2000; t++) {                      // 2 s: Linux polls 100 ms, but only after at least 79 ms more of SMU init on this card (stage3-smu-handshake-design.md D1)
			if (pspRd(kRlcBootload, &boot) || pspRd(kCpStat, &cp)) return 1;
			if ((boot & 0x80000000u) && cp == 0) break;
			usleep(1000);
		}
		pspRd(kImuScratch0, &imu);
		printf("RLC_RLCS_BOOTLOAD_STATUS %#x  CP_STAT %#x  GFX_IMU_SCRATCH_0 %#x (expected 0x0b214700)\n", boot, cp, imu);
		const int ok = (boot & 0x80000000u) && cp == 0;
		printf("%s\n", ok ? "2c done: RLC autoload complete" : "2c: autoload NOT complete (next suspect: the SMU handshake, Q4)");
		return !ok;
	}
	usage();
	return 2;
}

// ---- smu: the SMU 13.0.7 driver mailbox (overview repo stage3-smu-handshake-design.md, S0 to S3) ---------
// n33ctl smu check     S0: read the driver, debug and DAL mailboxes and the kext's state. No writes.
// n33ctl smu s1        S1: GetSmuVersion, GetDriverIfVersion, GetRunningSmuFeatures High and Low (queries)
// n33ctl smu s2        milestone 2d, only after a 2c that did not complete: arm, driver table address,
//                      EnableAllSmuFeatures with no masks (tinygrad AM's order on this card, 2026-10-17)
// n33ctl smu s3        only if S2's enable answered 0xFD: arm, allowed masks, EnableAllSmuFeatures again
// The kext runs each message with Linux's protocol and refuses anything off the D4 allowlist; this tool
// keeps the order and the stop points. Any answer other than 0x1 ends the run.
enum { aSmuSend = 12, aSmuArm = 13, aSmuState = 14 };
enum { mGetSmuVersion = 0x2, mGetDriverIfVersion = 0x3, mMaskLow = 0x4, mMaskHigh = 0x5, mEnableAll = 0x6,
       mRunningLow = 0xC, mRunningHigh = 0xD, mDramHigh = 0xE, mDramLow = 0xF };
#define kSmuMaskHigh 0x0001a3a4u
#define kSmuMaskLow  0x71fe7f7fu
#define kSmuTableOff 0x8600000ull
static const struct { uint32_t reg; const char *name; } kSmuRegs[] = {
	{ 0x1629A, "C2PMSG_90 response" }, { 0x16282, "C2PMSG_66 message" }, { 0x16292, "C2PMSG_82 argument" },
	{ 0x1628B, "C2PMSG_75 debug msg" }, { 0x16275, "C2PMSG_53 debug arg" }, { 0x16276, "C2PMSG_54 debug resp" },
	{ 0x1628A, "C2PMSG_74 DAL msg" }, { 0x16273, "C2PMSG_51 DAL arg" }, { 0x16274, "C2PMSG_52 DAL resp" },
};
static const struct { int bit; const char *name; } kSmuBits[] = {
	{ 1, "DPM_GFXCLK" }, { 3, "DPM_UCLK" }, { 4, "DPM_FCLK" }, { 5, "DPM_SOCCLK" }, { 8, "DPM_DCN" },
	{ 19, "GFXOFF" }, { 29, "FW_CTF" }, { 37, "GFX_IMU" },
};

static int smuState(uint64_t st[6]) {
	uint32_t got = 6;
	if (n33(aSmuState, NULL, 0, st, &got) != kIOReturnSuccess) { fprintf(stderr, "smu state call failed\n"); return 1; }
	return 0;
}
static void smuPrintState(const uint64_t *st) {
	printf("kext: armed S%llu hung %llu stopped %llu maskHigh %llu maskLow %llu enables %llu (last %#llx) dramHigh %llu dramLow %llu "
	       "page %llu; C2PMSG_90 %#llx _66 %#llx _82 %#llx\n", st[0] >> 8 & 0xFF, st[0] & 1, st[0] >> 1 & 1, st[0] >> 2 & 1,
	       st[0] >> 3 & 1, st[1], st[2], st[0] >> 4 & 1, st[0] >> 5 & 1, st[0] >> 7 & 1, st[3], st[4], st[5]);
}

// One message. 0 only when it answered 0x1. *result gets C2PMSG_82 after, *resp the response.
static int smuMsg(uint32_t msg, uint32_t arg, const char *name, uint32_t *result, uint32_t *resp) {
	uint64_t a[2] = { msg, arg }, o[4] = {0};
	if (call(aSmuSend, a, 2, o, 4)) { printf("%-28s REFUSED by the kext\n", name); return 1; }
	if (result) *result = (uint32_t)o[3];
	if (resp) *resp = (uint32_t)o[2];
	printf("%-28s arg %#010x  %s  response %#llx  result %#010llx\n", name, arg, o[1] ? "answered" : "TIMEOUT", o[2], o[3]);
	return !(o[1] == 1 && o[2] == 1);
}

static int smuFeatures(uint64_t *mask) {
	uint32_t hi = 0, lo = 0;
	if (smuMsg(mRunningHigh, 0, "GetRunningSmuFeaturesHigh", &hi, NULL) ||
	    smuMsg(mRunningLow, 0, "GetRunningSmuFeaturesLow", &lo, NULL)) return 1;
	*mask = (uint64_t)hi << 32 | lo;
	printf("running features %#018llx:", *mask);
	for (unsigned i = 0; i < sizeof kSmuBits / sizeof kSmuBits[0]; i++)
		printf(" %s=%llu", kSmuBits[i].name, *mask >> kSmuBits[i].bit & 1);
	printf("\n");
	return 0;
}

// The 2c completion registers, polled for up to 2 s (D1). 1 = complete.
static int smuAutoloadDone(void) {
	uint32_t boot = 0, cp = 0;
	for (int t = 0; t < 2000; t++) {
		if (pspRd(kRlcBootload, &boot) || pspRd(kCpStat, &cp)) return 0;
		if ((boot & 0x80000000u) && cp == 0) break;
		usleep(1000);
	}
	const int done = (boot & 0x80000000u) && cp == 0;
	printf("RLC_RLCS_BOOTLOAD_STATUS %#x  CP_STAT %#x: %s\n", boot, cp, done ? "autoload complete" : "autoload NOT complete");
	return done;
}

static int doSmu(int argc, char **argv) {
	const char *sub = argv[2];
	uint64_t st[6] = {0}, p2[8] = {0}, s[9] = {0};
	if (smuState(st) || p2state(p2) || call(aStatus, NULL, 0, s, 9)) return 1;

	if (!strcmp(sub, "state")) { smuPrintState(st); return 0; }
	if (!strcmp(sub, "send") && argc == 5) {   // raw, for tools/n33/smu-negative.sh: a message the kext accepts is really sent
		uint32_t r = 0;
		return smuMsg((uint32_t)num(argv[3]), (uint32_t)num(argv[4]), "raw", &r, NULL);
	}
	if (!strcmp(sub, "check") && argc == 3) {
		for (unsigned i = 0; i < sizeof kSmuRegs / sizeof kSmuRegs[0]; i++) {
			uint32_t v = 0;
			if (pspRd(kSmuRegs[i].reg, &v)) return 1;
			printf("%-22s %#x\n", kSmuRegs[i].name, v);
		}
		smuPrintState(st);
		p2printState(p2);
		return 0;
	}
	if (!strcmp(sub, "s1") && argc == 3) {
		uint32_t ver = 0, ifv = 0; uint64_t mask = 0;
		if (smuMsg(mGetSmuVersion, 0, "GetSmuVersion", &ver, NULL)) return 1;
		printf("PMFW version %u.%u.%u (%#x)\n", ver >> 16 & 0xFF, ver >> 8 & 0xFF, ver & 0xFF, ver);
		if (smuMsg(mGetDriverIfVersion, 0, "GetDriverIfVersion", &ifv, NULL)) return 1;
		printf("driver interface version %#x\n", ifv);
		return smuFeatures(&mask);
	}
	if (!strcmp(sub, "s2") && argc == 3) {
		// D2: only after AUTOLOAD_RLC was sent and the autoload did not complete in 2 s, and only if S1 shows
		// GFX_IMU or DPM_GFXCLK off.
		if (!(p2[0] >> 5 & 1)) { printf("refusing: AUTOLOAD_RLC has not been sent by this kext load (run psp2 2c first)\n"); return 1; }
		if (smuAutoloadDone()) { printf("refusing: the autoload is complete; S2 is not needed\n"); return 1; }
		uint64_t mask = 0;
		if (smuFeatures(&mask)) return 1;
		if ((mask >> 37 & 1) && (mask >> 1 & 1)) { printf("refusing: GFX_IMU and DPM_GFXCLK are both on, so the design does not expect S2 to help\n"); return 1; }
		const uint64_t mc = s[1] + kSmuTableOff;
		uint64_t lv = 2, o[1] = {0};
		if (call(aSmuArm, &lv, 1, o, 1)) { printf("arm S2 REFUSED (driver table page not writable?)\n"); return 1; }
		printf("armed S2: driver table page VRAM %#llx (MC %#llx) zeroed and protected. Watch the monitor.\n", kSmuTableOff, mc);
		uint32_t resp = 0;
		if (smuMsg(mDramHigh, (uint32_t)(mc >> 32), "SetDriverDramAddrHigh", NULL, NULL) ||
		    smuMsg(mDramLow, (uint32_t)mc, "SetDriverDramAddrLow", NULL, NULL)) return 1;
		const int bad = smuMsg(mEnableAll, 0, "EnableAllSmuFeatures", NULL, &resp);
		if (bad && resp == 0xFD) { printf("EnableAllSmuFeatures answered 0xFD (bad prerequisite): S3 is next, a separate go\n"); return 1; }
		if (bad) return 1;
		smuFeatures(&mask);
		return !smuAutoloadDone();
	}
	if (!strcmp(sub, "s3") && argc == 3) {
		if (st[1] != 1 || st[2] != 0xFD) { printf("refusing: S3 runs only after S2's enable answered 0xFD\n"); smuPrintState(st); return 1; }
		uint64_t lv = 3, o[1] = {0}, mask = 0;
		if (call(aSmuArm, &lv, 1, o, 1)) { printf("arm S3 REFUSED\n"); return 1; }
		printf("armed S3. Watch the monitor.\n");
		if (smuMsg(mMaskHigh, kSmuMaskHigh, "SetAllowedFeaturesMaskHigh", NULL, NULL) ||
		    smuMsg(mMaskLow, kSmuMaskLow, "SetAllowedFeaturesMaskLow", NULL, NULL) ||
		    smuMsg(mEnableAll, 0, "EnableAllSmuFeatures (again)", NULL, NULL)) return 1;
		smuFeatures(&mask);
		return !smuAutoloadDone();
	}
	usage();
	return 2;
}

// ---- gmc: NBIO 4.3, GMC 11 / MMHUB 3.0.2, GFXHUB 3.0 and the GART (overview repo stage3-gmc-design.md) -----
// n33ctl gmc state     G0, read-only: take the kext's snapshot, print each step's registers (current and the
//                      kext's target, "equals" or "differs"), the fault and invalidation registers, DCN_VM
// n33ctl gmc g1..g4    one step each (G1 NBIO; G2 table, mem_scratch, apertures; G3 L1 TLB and L2; G4 VMID 0
//                      live and the engine 17 flush), with before/after tables and the display check
// n33ctl gmc g5        the GART bind test: 4 kext pages at slots 0 to 3, flush, read back, unmap, flush
// n33ctl gmc g6        GFXHUB, only after psp2 2c in this kext load
// n33ctl gmc restore   write every journalled before-value back (refused once an engine holds a GART address)
// The kext computes every value and refuses anything off-plan; this tool keeps the order and stops after each
// step for the display check. G1 to G4 are the fallback for m2 2a (design Q10): each is William's go.
enum { aGmcState = 15, aGmcStep = 16, aGartMap = 17, aGartUnmap = 18, aGartFlush = 19, aGmcRestore = 20,
       aGmcPlan = 21, aGmcRecord = 22 };
enum { kGmcOpWrite = 0, kGmcOpHdp = 1, kGmcOpFlushMm = 2, kGmcOpFlushGc = 3 };
#define kGmcTableOff    0x8800000ull
#define kGmcPteFlagsSys 0x0003000000000077ull
#define kGmcPteAddr     0x0000FFFFFFFFF000ull
enum { kGmcMmFault = 0x1A68C, kGmcGcFault = 0x2828 };
static const struct { uint32_t reg; const char *name; } kGmcNames[] = {
	{ 0xE4D, "NBIF REMAP_HDP_MEM_FLUSH_CNTL" }, { 0xE4E, "NBIF REMAP_HDP_REG_FLUSH_CNTL" },
	{ 0xDE0, "NBIF DOORBELL_APER_EN" }, { 0xE20, "NBIF BIF_FB_EN" }, { 0xDE3, "RCC_CONFIG_MEMSIZE" },
	{ 0x1A72B, "MM CTX0_PT_BASE_LO32" }, { 0x1A72C, "MM CTX0_PT_BASE_HI32" }, { 0x1A74B, "MM CTX0_PT_START_LO32" },
	{ 0x1A74C, "MM CTX0_PT_START_HI32" }, { 0x1A76B, "MM CTX0_PT_END_LO32" }, { 0x1A76C, "MM CTX0_PT_END_HI32" },
	{ 0x1A86E, "MM AGP_TOP" }, { 0x1A86F, "MM AGP_BOT" }, { 0x1A870, "MM AGP_BASE" },
	{ 0x1A871, "MM SYSTEM_APERTURE_LOW" }, { 0x1A872, "MM SYSTEM_APERTURE_HIGH" },
	{ 0x1A858, "MM SYSTEM_APERTURE_DEFAULT_LSB" }, { 0x1A859, "MM SYSTEM_APERTURE_DEFAULT_MSB" },
	{ 0x1A688, "MM L2_PROTECTION_FAULT_CNTL" }, { 0x1A689, "MM L2_PROTECTION_FAULT_CNTL2" },
	{ 0x1A68C, "MM L2_PROTECTION_FAULT_STATUS" }, { 0x1A68D, "MM L2_PROTECTION_FAULT_ADDR_LO32" },
	{ 0x1A68E, "MM L2_PROTECTION_FAULT_ADDR_HI32" }, { 0x1A68F, "MM L2_PROTECTION_FAULT_DEFAULT_LO32" },
	{ 0x1A690, "MM L2_PROTECTION_FAULT_DEFAULT_HI32" }, { 0x1A873, "MM MX_L1_TLB_CNTL" },
	{ 0x1A680, "MM VM_L2_CNTL" }, { 0x1A681, "MM VM_L2_CNTL2" }, { 0x1A682, "MM VM_L2_CNTL3" },
	{ 0x1A698, "MM VM_L2_CNTL4" }, { 0x1A69E, "MM VM_L2_CNTL5" }, { 0x1A6C0, "MM VM_CONTEXT0_CNTL" },
	{ 0x1A692, "MM IDENTITY_LOW_LO32" }, { 0x1A693, "MM IDENTITY_LOW_HI32" }, { 0x1A694, "MM IDENTITY_HIGH_LO32" },
	{ 0x1A695, "MM IDENTITY_HIGH_HI32" }, { 0x1A696, "MM IDENTITY_PHYS_OFFSET_LO32" }, { 0x1A697, "MM IDENTITY_PHYS_OFFSET_HI32" },
	{ 0x1A706, "MM INVALIDATE_ENG17_ACK" }, { 0x1A69B, "MM L2_BANK_SELECT_RESERVED_CID2" },
	{ 0x2953, "GC CTX0_PT_BASE_LO32" }, { 0x2954, "GC CTX0_PT_BASE_HI32" }, { 0x2973, "GC CTX0_PT_START_LO32" },
	{ 0x2974, "GC CTX0_PT_START_HI32" }, { 0x2993, "GC CTX0_PT_END_LO32" }, { 0x2994, "GC CTX0_PT_END_HI32" },
	{ 0x28DA, "GC AGP_TOP" }, { 0x28DB, "GC AGP_BOT" }, { 0x28DC, "GC AGP_BASE" },
	{ 0x28DD, "GC SYSTEM_APERTURE_LOW" }, { 0x28DE, "GC SYSTEM_APERTURE_HIGH" },
	{ 0x2808, "GC SYSTEM_APERTURE_DEFAULT_LSB" }, { 0x2809, "GC SYSTEM_APERTURE_DEFAULT_MSB" },
	{ 0x2824, "GC L2_PROTECTION_FAULT_CNTL" }, { 0x2825, "GC L2_PROTECTION_FAULT_CNTL2" },
	{ 0x2828, "GC L2_PROTECTION_FAULT_STATUS" }, { 0x282B, "GC L2_PROTECTION_FAULT_DEFAULT_LO32" },
	{ 0x282C, "GC L2_PROTECTION_FAULT_DEFAULT_HI32" }, { 0x28DF, "GC MX_L1_TLB_CNTL" },
	{ 0x281C, "GC VM_L2_CNTL" }, { 0x281D, "GC VM_L2_CNTL2" }, { 0x281E, "GC VM_L2_CNTL3" },
	{ 0x2834, "GC VM_L2_CNTL4" }, { 0x283A, "GC VM_L2_CNTL5" }, { 0x28E8, "GC VM_CONTEXT0_CNTL" },
	{ 0x282E, "GC IDENTITY_LOW_LO32" }, { 0x282F, "GC IDENTITY_LOW_HI32" }, { 0x2830, "GC IDENTITY_HIGH_LO32" },
	{ 0x2831, "GC IDENTITY_HIGH_HI32" }, { 0x2832, "GC IDENTITY_PHYS_OFFSET_LO32" }, { 0x2833, "GC IDENTITY_PHYS_OFFSET_HI32" },
	{ 0x292E, "GC INVALIDATE_ENG17_ACK" }, { 0x307F, "CP_DEBUG" },
	{ 0x3935, "DCN_VM_FB_LOCATION_BASE" }, { 0x3936, "DCN_VM_FB_LOCATION_TOP" }, { 0x3937, "DCN_VM_FB_OFFSET" },
	{ 0x3938, "DCN_VM_AGP_BOT" }, { 0x3939, "DCN_VM_AGP_TOP" }, { 0x393A, "DCN_VM_AGP_BASE" }, { 0x3A19, "DCN_VM_CONTEXT0_CNTL" },
};
// Per-context and per-engine registers: { first, count, stride, name }. Stride 2 is a LO32/HI32 pair.
static const struct { uint32_t base, count, stride; const char *name; } kGmcRanges[] = {
	{ 0x1A6C1, 15, 1, "MM CONTEXT%u_CNTL" }, { 0x1A74D, 15, 2, "MM CONTEXT%u_PT_START" }, { 0x1A76D, 15, 2, "MM CONTEXT%u_PT_END" },
	{ 0x1A707, 18, 2, "MM INV_ENG%u_ADDR_RANGE" },
	{ 0x28E9, 15, 1, "GC CONTEXT%u_CNTL" }, { 0x2975, 15, 2, "GC CONTEXT%u_PT_START" }, { 0x2995, 15, 2, "GC CONTEXT%u_PT_END" },
	{ 0x292F, 18, 2, "GC INV_ENG%u_ADDR_RANGE" },
};
static const char *gmcName(uint32_t reg) {
	static char b[64];
	for (unsigned i = 0; i < sizeof kGmcNames / sizeof kGmcNames[0]; i++) if (kGmcNames[i].reg == reg) return kGmcNames[i].name;
	for (unsigned i = 0; i < sizeof kGmcRanges / sizeof kGmcRanges[0]; i++) {
		const uint32_t base = kGmcRanges[i].base, st = kGmcRanges[i].stride;
		if (reg < base || reg >= base + kGmcRanges[i].count * st) continue;
		const uint32_t k = (reg - base) / st, first = strstr(kGmcRanges[i].name, "ENG") ? 0 : 1;
		char n[48];
		snprintf(n, sizeof n, kGmcRanges[i].name, k + first);
		snprintf(b, sizeof b, "%s%s", n, st == 2 ? ((reg - base) % 2 ? "_HI32" : "_LO32") : "");
		return b;
	}
	snprintf(b, sizeof b, "dword %#x", reg);
	return b;
}

static int gmcState(uint64_t st[12], int take) {
	uint64_t a = take ? 1 : 0;
	memset(st, 0, 12 * sizeof st[0]);
	return call(aGmcState, &a, 1, st, 12);
}
static int gmcDone(const uint64_t *st, int n) { return (int)(st[0] >> (n + 1) & 1); }
static void gmcPrintState(const uint64_t *st) {
	printf("kext: G0 %llu  done G1 %d G2 %d G3 %d G4 %d G6 %d  table %llu stopped %llu restored %llu engine %llu "
	       "dummy %llu host %llu  journal %llu  last MMHUB flush %#llx  GC up %llu\n", st[0] & 1, gmcDone(st, 1),
	       gmcDone(st, 2), gmcDone(st, 3), gmcDone(st, 4), gmcDone(st, 6), st[0] >> 8 & 1, st[0] >> 9 & 1, st[0] >> 10 & 1,
	       st[0] >> 11 & 1, st[0] >> 12 & 1, st[0] >> 13 & 1, st[8], st[10], st[11]);
	printf("G0 inputs: FB_LOCATION_BASE %#llx TOP %#llx FB_OFFSET %#llx MEMSIZE %#llx  system aperture as found %#llx..%#llx  "
	       "dummy page %#llx\n", st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
}

// A step's plan as the kext would write it now: current value, target, and whether they already agree.
// *ffff counts registers that read 0xffffffff (G0's done condition).
static int gmcPlan(uint32_t step, int *ffff, int *differs) {
	uint64_t a[2] = { step, 0 }, o[8] = {0};
	if (call(aGmcPlan, a, 2, o, 8)) { printf("G%u plan: REFUSED by the kext (n33ctl why <code> from the log)\n", step); return 1; }
	const uint64_t n = o[7];
	printf("G%u plan, %llu entries (current -> target):\n", step, n);
	for (uint64_t i = 0; i < n; i++) {
		a[1] = i;
		if (call(aGmcPlan, a, 2, o, 8)) return 1;
		if (o[0] == kGmcOpHdp) { printf("  -- HDP flush\n"); continue; }
		if (o[0] == kGmcOpFlushMm || o[0] == kGmcOpFlushGc) { printf("  -- %s engine 17 flush, VMID 0\n", o[0] == kGmcOpFlushMm ? "MMHUB" : "GFXHUB"); continue; }
		const int same = ((o[5] ^ o[6]) & o[4]) == 0;
		if (o[5] == 0xFFFFFFFFull && ffff) (*ffff)++;
		if (!same && differs) (*differs)++;
		printf("  %-36s %#010llx -> %#010llx  %s%s\n", gmcName((uint32_t)o[1]), o[5], o[6],
		       o[4] ? (same ? "equals" : "differs") : "self-clearing", o[2] == 0xFFFFFFFFull ? "" : " (fields only)");
	}
	return 0;
}

static int gmcJournal(uint64_t first, uint64_t count) {
	for (uint64_t i = first; i < first + count; i++) {
		uint64_t a = i, o[6] = {0};
		if (call(aGmcRecord, &a, 1, o, 6)) return 1;
		printf("  G%llu %-36s before %#010llx  after %#010llx  target %#010llx  %s\n", o[0], gmcName((uint32_t)o[1]), o[2], o[3], o[4],
		       ((o[3] ^ o[4]) & o[5]) ? "MISMATCH" : o[5] ? "ok" : "recorded");
	}
	return 0;
}

// The display check's register half: HUBP0, DCN_VM and the protected ranges (the live DMCUB windows).
struct gmcDisp { uint64_t hubp, pitch, n, prot[20][3]; uint32_t dcn[7]; };   // the kext lists up to 20 ranges (kN33Ranges)
static int gmcDisplay(struct gmcDisp *d) {
	static const uint32_t dcn[7] = { 0x3935, 0x3936, 0x3937, 0x3938, 0x3939, 0x393A, 0x3A19 };
	uint64_t s[11] = {0};
	memset(d, 0, sizeof *d);
	if (call(aStatus, NULL, 0, s, 11)) return 1;
	d->hubp = s[5]; d->pitch = s[6]; d->n = s[8] > 20 ? 20 : s[8];
	for (uint64_t i = 0; i < d->n; i++) { uint64_t a = i; if (call(aProtected, &a, 1, d->prot[i], 3)) return 1; }
	for (int i = 0; i < 7; i++) if (pspRd(dcn[i], &d->dcn[i])) return 1;
	return 0;
}
static int gmcDisplaySame(const struct gmcDisp *a, const struct gmcDisp *b) {
	int same = a->hubp == b->hubp && a->pitch == b->pitch && !memcmp(a->dcn, b->dcn, sizeof a->dcn);
	// The GART table joins the list at G2 (kind 14): compare the ranges that existed before.
	for (uint64_t i = 0; i < a->n; i++) {
		int found = 0;
		for (uint64_t j = 0; j < b->n; j++) if (!memcmp(a->prot[i], b->prot[j], sizeof a->prot[i])) found = 1;
		if (!found) { printf("protected range [%llu] VRAM %#llx len %#llx (%s) moved or went away\n", i, a->prot[i][0], a->prot[i][1], kindName(a->prot[i][2])); same = 0; }
	}
	printf("display registers: HUBP0 %#llx pitch %#llx, DCN_VM %#x %#x %#x, %llu protected ranges: %s\n", b->hubp, b->pitch,
	       b->dcn[0], b->dcn[1], b->dcn[2], b->n, same ? "unchanged" : "CHANGED");
	return same;
}

static int gmcStep(uint32_t step) {
	uint64_t st[12], o[9] = {0}, a = step;
	struct gmcDisp before, after;
	uint32_t cp0 = 0, cp1 = 0;
	if (gmcState(st, 0)) return 1;
	gmcPrintState(st);
	if (!(st[0] & 1)) { printf("refusing: no G0 snapshot in this kext load (run gmc state first)\n"); return 1; }
	if (gmcPlan(step, NULL, NULL) || gmcDisplay(&before)) return 1;
	if (step == 6 && pspRd(kCpStat, &cp0)) return 1;
	if (call(aGmcStep, &a, 1, o, 9)) { printf("G%u: REFUSED by the kext\n", step); return 1; }
	printf("G%u: %s, %llu registers written, %llu mismatches, MMHUB fault status %#llx, GC fault status %#llx, flush bits %#llx%s\n",
	       step, o[1] ? "done" : "FAILED (nothing more this load; gmc restore is available)", o[3], o[4], o[5], o[6], o[7],
	       step == 2 ? (o[8] ? ", table read back ok" : ", TABLE READ BACK WRONG") : "");
	if (gmcJournal(o[2], o[3]) || gmcDisplay(&after)) return 1;
	const int same = gmcDisplaySame(&before, &after);
	if (step == 6) {
		if (pspRd(kCpStat, &cp1)) return 1;
		printf("CP_STAT %#x -> %#x: %s\n", cp0, cp1, cp0 == cp1 ? "unchanged" : "CHANGED");
	}
	printf("display check: confirm the desktop is still on the monitor 60 s from now. The next step is a separate go.\n");
	return !(o[1] == 1 && same && cp0 == cp1);
}

// G0: read-only. Every register the steps will write, the fault and invalidation state and DCN_VM.
static int gmcG0(void) {
	static const uint32_t extra[] = { 0x1A68C, 0x1A68D, 0x1A68E, 0x1A706, 0x1A69B, 0xE20, 0xDE3,
	                                  0x3935, 0x3936, 0x3937, 0x3938, 0x3939, 0x393A, 0x3A19 };
	uint64_t st[12];
	int bad = 0, ffff = 0, differs = 0;
	if (gmcState(st, 1)) return 1;
	gmcPrintState(st);
	if (st[1] != 0x8000 || st[2] != 0x83ff || st[3] != 0) { printf("FB location DIFFERS from the measured 0x8000 / 0x83ff / 0\n"); bad = 1; }
	const uint32_t steps[] = { 1, 2, 3, 4, 6 };
	for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++) {
		if (steps[i] == 6 && !st[11]) { printf("G6 plan: GC not up in this load (no GC register read)\n"); continue; }
		if (gmcPlan(steps[i], &ffff, &differs)) bad = 1;
	}
	if (!(st[0] >> 12 & 1)) printf("(the fault default address targets read 0 until G2 allocates the dummy page)\n");
	for (unsigned i = 0; i < sizeof extra / sizeof extra[0]; i++) {
		uint32_t v = 0;
		if (pspRd(extra[i], &v)) return 1;
		if (v == 0xFFFFFFFFu) ffff++;
		printf("  %-36s %#010x\n", gmcName(extra[i]), v);
		if (extra[i] == kGmcMmFault && v) { printf("MMHUB fault status is not 0\n"); bad = 1; }
	}
	struct gmcDisp d;
	if (gmcDisplay(&d)) return 1;
	for (uint64_t i = 0; i < d.n; i++) printf("protected [%llu] VRAM %#llx len %#llx  %s\n", i, d.prot[i][0], d.prot[i][1], kindName(d.prot[i][2]));
	printf("IOMMU (Q2): record `ioreg -l | grep -ic AppleVTD` with this output (expect 0)\n");
	if (ffff) { printf("%d register(s) read 0xffffffff\n", ffff); bad = 1; }
	printf("%d planned register(s) differ from their target now\n", differs);
	printf("%s\n", bad ? "G0 check FAILED: do not run G1" : "G0 ok");
	return bad;
}

// G5: the bind test. Not a DMA proof (design Q5).
static int gmcG5(void) {
	uint64_t st[12], m[4] = { 0, 4, 0, 0 }, o[4] = {0}, f[4] = {0}, u[2] = { 0, 4 }, uo[3] = {0};
	int bad = 0;
	if (gmcState(st, 0)) return 1;
	gmcPrintState(st);
	if (!gmcDone(st, 4)) { printf("refusing: G5 needs G4 done in this kext load\n"); return 1; }
	const int gcLive = gmcDone(st, 6) && st[11];
	if (call(aGartMap, m, 4, o, 4)) { printf("gart map REFUSED\n"); return 1; }
	printf("mapped 4 kext pages at slots 0..3 (MC %#llx), first PTE %#llx\n", o[2], o[1]);
	for (int pass = 0; pass < 2; pass++) {
		if (call(aGartFlush, NULL, 0, f, 4)) { printf("gart flush REFUSED\n"); return 1; }
		const int ok = f[1] == 7 && (!gcLive || (f[2] & 2));
		printf("flush: MMHUB bits %#llx, GFXHUB bits %#llx, MMHUB fault status %#llx: %s\n", f[1], f[2], f[3], ok && !f[3] ? "ok" : "FAILED");
		if (!ok || f[3]) bad = 1;
		uint64_t prev = 0;
		for (uint32_t s = 0; s < 4; s++) {
			uint64_t a0 = kGmcTableOff + 8ull * s, a1 = a0 + 4, lo = 0, hi = 0;
			if (call(aBar0Read, &a0, 1, &lo, 1) || call(aBar0Read, &a1, 1, &hi, 1)) return 1;
			const uint64_t pte = hi << 32 | lo, pa = pte & kGmcPteAddr;
			int good;
			if (pass == 0) good = (pte & ~kGmcPteAddr) == kGmcPteFlagsSys && pa && pa < (1ull << 44) && pa != prev && (s || pte == o[1]);
			else good = pte == (st[7] & kGmcPteAddr);
			prev = pa;
			printf("  slot %u PTE %#018llx  %s\n", s, pte, good ? "ok" : "WRONG");
			if (!good) bad = 1;
		}
		if (pass == 0) {
			if (call(aGartUnmap, u, 2, uo, 3)) { printf("gart unmap REFUSED\n"); return 1; }
			printf("unmapped slots 0..3 to the dummy page (PTE %#llx)\n", uo[1]);
			if (gmcState(st, 0)) return 1;
		}
	}
	printf("%s\n", bad ? "G5 FAILED" : "G5 done: PTEs bound and unbound, every flush acknowledged, no fault (not a DMA proof)");
	return bad;
}

static int doGmc(int argc, char **argv) {
	const char *sub = argv[2];
	uint64_t st[12], o[9] = {0};
	if (!strcmp(sub, "flags") && argc == 3) { if (gmcState(st, 0)) return 1; gmcPrintState(st); return 0; }
	if (!strcmp(sub, "state") && argc == 3) return gmcG0();
	if (sub[0] == 'g' && sub[1] >= '1' && sub[1] <= '6' && sub[2] == 0 && argc == 3) {
		if (sub[1] == '5') return gmcG5();
		return gmcStep((uint32_t)(sub[1] - '0'));
	}
	if (!strcmp(sub, "restore") && argc == 3) {
		if (call(aGmcRestore, NULL, 0, o, 3)) { printf("restore REFUSED\n"); return 1; }
		printf("restored %llu registers (newest first), %llu read back differently. Nothing more runs until the kext is reloaded.\n", o[1], o[2]);
		return o[2] ? 1 : 0;
	}
	// Raw access for tools/n33/gmc-negative.sh. The kext checks every call; one it accepts really writes.
	if (!strcmp(sub, "step") && argc == 4) {
		uint64_t a = num(argv[3]);
		if (call(aGmcStep, &a, 1, o, 9)) return 1;
		printf("step %llu: ok %llu, %llu registers\n", a, o[1], o[3]);
		return o[1] == 1 ? 0 : 1;
	}
	if (!strcmp(sub, "map") && argc == 7) {
		uint64_t a[4] = { num(argv[3]), num(argv[4]), num(argv[5]), num(argv[6]) };
		if (call(aGartMap, a, 4, o, 4)) return 1;
		printf("mapped: first PTE %#llx at MC %#llx\n", o[1], o[2]);
		return 0;
	}
	if (!strcmp(sub, "unmap") && argc == 5) {
		uint64_t a[2] = { num(argv[3]), num(argv[4]) };
		if (call(aGartUnmap, a, 2, o, 3)) return 1;
		printf("unmapped %llu slot(s)\n", o[2]);
		return 0;
	}
	if (!strcmp(sub, "flush") && argc == 3) {
		if (call(aGartFlush, NULL, 0, o, 4)) return 1;
		printf("flush: MMHUB bits %#llx, GFXHUB bits %#llx, MMHUB fault status %#llx\n", o[1], o[2], o[3]);
		return 0;
	}
	usage();
	return 2;
}

// ---- gfx / sdma: stage 3 step 5, CP RS64 start and SDMA0 queue 0 (overview repo stage3-step5-gfx-sdma-design.md) -----
// n33ctl gfx rs64           C1: config_gfx_rs64 with the pinned PFP/ME/MEC start addresses; after psp2 2c, before gmc g6
// n33ctl sdma state         C0, read-only: SDMA0, CP, GRBM, RLC and GCVM registers (refused before autoload)
// n33ctl sdma start mmio|db E1: SDMA0 queue 0, write pointer by MMIO (mmio) or by the BAR2 doorbell (db); after gmc g6
// n33ctl sdma test 1|2|3    E2 WRITE to the VRAM test page, E3 WRITE to host page A through GART slot 0, E4 FENCE
// n33ctl sdma stop          E6: queue off, doorbell off, engine halted, pointers zeroed; a start in either mode may follow
// The kext computes every value and builds every packet; this tool keeps the order and stops after each step for the
// display check. Each step is William's go. If test 1 times out in mmio mode: sdma stop, sdma start db, sdma test 1.
enum { aGfxRs64 = 23, aSdmaState = 24, aSdmaStart = 25, aSdmaTest = 26, aSdmaStop = 27, aSdmaRecord = 28 };
static const struct { uint32_t reg; const char *name; } kSdmaNames[] = {
	{ 0xF89A, "SDMA0_F32_CNTL" }, { 0x127C, "SDMA0_CNTL" }, { 0x1282, "SDMA0_SEM_WAIT_FAIL_TIMER_CNTL" },
	{ 0x1285, "SDMA0_STATUS_REG" }, { 0x128E, "SDMA0_WATCHDOG_CNTL" }, { 0x129C, "SDMA0_UTCL1_CNTL" },
	{ 0x129F, "SDMA0_UTCL1_PAGE" }, { 0x12B0, "SDMA0_ERROR_LOG" }, { 0x12E0, "SDMA0_QUEUE0_RB_CNTL" },
	{ 0x12E1, "SDMA0_QUEUE0_RB_BASE" }, { 0x12E2, "SDMA0_QUEUE0_RB_BASE_HI" }, { 0x12E3, "SDMA0_QUEUE0_RB_RPTR" },
	{ 0x12E4, "SDMA0_QUEUE0_RB_RPTR_HI" }, { 0x12E5, "SDMA0_QUEUE0_RB_WPTR" }, { 0x12E6, "SDMA0_QUEUE0_RB_WPTR_HI" },
	{ 0x12E8, "SDMA0_QUEUE0_RB_RPTR_ADDR_HI" }, { 0x12E9, "SDMA0_QUEUE0_RB_RPTR_ADDR_LO" }, { 0x12EA, "SDMA0_QUEUE0_IB_CNTL" },
	{ 0x12F1, "SDMA0_QUEUE0_CONTEXT_STATUS" }, { 0x12F2, "SDMA0_QUEUE0_DOORBELL" }, { 0x130B, "SDMA0_QUEUE0_DOORBELL_OFFSET" },
	{ 0x1312, "SDMA0_QUEUE0_RB_WPTR_POLL_ADDR_HI" }, { 0x1313, "SDMA0_QUEUE0_RB_WPTR_POLL_ADDR_LO" },
	{ 0x1315, "SDMA0_QUEUE0_MINOR_PTR_UPDATE" }, { 0x21A0, "CP_STAT" }, { 0x2004, "GRBM_STATUS" }, { 0xA803, "CP_ME_CNTL" },
	{ 0xC904, "CP_MEC_RS64_CNTL" }, { 0xF840, "CP_PFP_IC_BASE_LO" }, { 0xF844, "CP_ME_IC_BASE_LO" }, { 0xF84C, "CP_CPC_IC_BASE_LO" },
	{ 0xA98A, "RLC_CP_SCHEDULERS" }, { 0xC807, "CP_MES_CNTL" }, { 0xA900, "GRBM_GFX_CNTL" },
	{ 0x2828, "GCVM_L2_PROTECTION_FAULT_STATUS" }, { 0x2829, "GCVM_L2_PROTECTION_FAULT_ADDR_LO32" },
	{ 0x282A, "GCVM_L2_PROTECTION_FAULT_ADDR_HI32" }, { 0x30A4, "CP_PFP_PRGRM_CNTR_START" }, { 0x30B9, "CP_PFP_PRGRM_CNTR_START_HI" },
	{ 0x30A5, "CP_ME_PRGRM_CNTR_START" }, { 0x30D9, "CP_ME_PRGRM_CNTR_START_HI" }, { 0xC900, "CP_MEC_RS64_PRGRM_CNTR_START" },
	{ 0xC938, "CP_MEC_RS64_PRGRM_CNTR_START_HI" }, { 0x507A42, "NBIO S2A_DOORBELL_ENTRY_2_CTRL (RSMU)" },
};
static const char *sdmaName(uint32_t reg) {
	static char b[24];
	for (unsigned i = 0; i < sizeof kSdmaNames / sizeof kSdmaNames[0]; i++) if (kSdmaNames[i].reg == reg) return kSdmaNames[i].name;
	snprintf(b, sizeof b, "dword %#x", reg);
	return b;
}

static int sdmaFlags(uint64_t st[15]) {
	uint64_t a = 0;
	memset(st, 0, 15 * sizeof st[0]);
	return call(aSdmaState, &a, 1, st, 15);
}
static void sdmaPrintFlags(const uint64_t *st) {
	static const char *mode[] = { "none", "mmio", "db" }, *latch[] = { "none", "timeout", "hard" };
	printf("kext: C1 attempted %llu done %llu  pages %llu running %llu mode %s latch %s timed-out modes %#llx  fw pinned %llu "
	       "BAR2 %llu (%llu bytes)  engine holds GART %llu  G6 %llu  GC up %llu\n", st[0] & 1, st[0] >> 1 & 1, st[0] >> 2 & 1,
	       st[0] >> 3 & 1, st[1] < 3 ? mode[st[1]] : "?", st[2] < 3 ? latch[st[2]] : "?", st[3], st[0] >> 4 & 1, st[0] >> 5 & 1,
	       st[10], st[0] >> 6 & 1, st[0] >> 7 & 1, st[11]);
	printf("      wptr %llu dwords, fence seq %llu, tests ok %#llx, starts %llu, journal %llu, S2A entry 2 as found %#llx, "
	       "2c sizes 87/88/89 %#llx/%#llx/%#llx\n", st[4], st[5], st[6], st[7], st[8], st[9], st[12], st[13], st[14]);
}
static int sdmaJournal(uint64_t first, uint64_t count) {
	static const char *phase[] = { "?", "C1", "E1", "E6" };
	for (uint64_t i = first; i < first + count; i++) {
		uint64_t a = i, o[7] = {0};
		if (call(aSdmaRecord, &a, 1, o, 7)) return 1;
		printf("  %s %-40s before %#010llx  after %#010llx  target %#010llx  %s\n", o[0] < 4 ? phase[o[0]] : "?",
		       sdmaName((uint32_t)o[2]), o[3], o[4], o[5], ((o[4] ^ o[5]) & o[6]) ? "MISMATCH" : o[6] ? "ok" : "recorded");
	}
	return 0;
}

// C0: read-only. The kext refuses before autoload (no GC register is read then).
static int sdmaState(void) {
	uint64_t st[15], a = 1, o[3] = {0};
	int ffff = 0;
	if (sdmaFlags(st)) return 1;
	sdmaPrintFlags(st);
	if (call(aSdmaState, &a, 1, o, 3)) { printf("sdma state: REFUSED by the kext (GC not up?)\n"); return 1; }
	const uint64_t n = o[2];
	for (uint64_t i = 1; i <= n; i++) {
		a = i;
		if (call(aSdmaState, &a, 1, o, 3)) return 1;
		if (o[1] == 0xFFFFFFFFull) ffff++;
		printf("  %-40s %#010llx%s\n", sdmaName((uint32_t)o[0]), o[1], o[1] == 0xFFFFFFFFull ? "  (0xffffffff)" : "");
	}
	if (ffff) printf("%d register(s) read 0xffffffff: C0 not done\n", ffff);
	else printf("C0 done: no register reads 0xffffffff\n");
	return ffff ? 1 : 0;
}

static int gfxRs64(void) {
	uint64_t st[15], o[10] = {0};
	struct gmcDisp before, after;
	if (sdmaFlags(st)) return 1;
	sdmaPrintFlags(st);
	if (gmcDisplay(&before)) return 1;
	if (call(aGfxRs64, NULL, 0, o, 10)) { printf("C1: REFUSED by the kext\n"); return 1; }
	printf("C1: %s, %llu writes, %llu mismatches, CP_STAT %#llx, GRBM_GFX_CNTL %#llx, CP_ME_CNTL %#llx, CP_MEC_RS64_CNTL %#llx, "
	       "GC fault status %#llx\n", o[1] ? "done" : "FAILED (gmc g6 stays refused this load)", o[3], o[4], o[5], o[6], o[7], o[8], o[9]);
	if (sdmaJournal(o[2], o[3]) || gmcDisplay(&after)) return 1;
	const int same = gmcDisplaySame(&before, &after);
	printf("each *_PRGRM_CNTR_START should read 0x00000c00 and each _HI 0x0001c000 (rows above).\n");
	printf("display check: confirm the desktop is still on the monitor 60 s from now. gmc g6 is the next go.\n");
	return !(o[1] == 1 && same);
}

static int sdmaStart(uint64_t mode) {
	uint64_t st[15], o[16] = {0};
	struct gmcDisp before, after;
	if (sdmaFlags(st)) return 1;
	sdmaPrintFlags(st);
	if (gmcDisplay(&before)) return 1;
	if (call(aSdmaStart, &mode, 1, o, 16)) { printf("E1 (%s): REFUSED by the kext\n", mode == 2 ? "db" : "mmio"); return 1; }
	printf("E1 (%s): %s, %llu writes, %llu mismatches\n", mode == 2 ? "db" : "mmio", o[1] ? "done" : "FAILED (latched: sdma stop)", o[3], o[4]);
	if (sdmaJournal(o[2], o[3]) || gmcDisplay(&after)) return 1;
	printf("RB_CNTL %#llx (want RB_ENABLE 1), RB_BASE %#llx (want 0x8008a000) _HI %#llx (want 0), F32_CNTL %#llx (want HALT 0), "
	       "STATUS_REG %#llx (want IDLE), RB_RPTR %#llx (want 0), GC fault status %#llx\n", o[5], o[6], o[7], o[8], o[9], o[10], o[11]);
	printf("DOORBELL %#llx, DOORBELL_OFFSET %#llx", o[13], o[14]);
	if (mode == 2) printf(", S2A entry 2 %#llx (want fields 0x3009001d under 0xf1ffffbf), doorbell at BAR2 + %#llx", o[15], o[12]);
	printf("\n");
	const int same = gmcDisplaySame(&before, &after);
	printf("display check: confirm the desktop is still on the monitor 60 s from now. sdma test 1 is the next go.\n");
	return !(o[1] == 1 && same);
}

static int sdmaTest(uint64_t n) {
	static const char *what[] = { "", "WRITE to the VRAM test page (E2)", "WRITE to host page A via GART slot 0 (E3)",
	                              "FENCE to host page A + 8 (E4)" };
	uint64_t st[15], o[16] = {0};
	struct gmcDisp before, after;
	if (sdmaFlags(st)) return 1;
	sdmaPrintFlags(st);
	if (gmcDisplay(&before)) return 1;
	if (call(aSdmaTest, &n, 1, o, 16)) { printf("test %llu: REFUSED by the kext\n", n); return 1; }
	printf("test %llu, %s, %s mode: %s after %llu us\n", n, n <= 3 ? what[n] : "?", st[1] == 2 ? "db" : "mmio",
	       o[1] ? "DONE" : o[14] == 1 ? "TIMEOUT (timeout only)" : "FAILED (latched)", o[15]);
	printf("  target MC %#llx reads %#llx, RB_RPTR %#llx, RB_WPTR %#llx, wptr %#llx bytes, rptr slot %#llx\n", o[12], o[2], o[3], o[4], o[5], o[6]);
	printf("  GC fault status %#llx, ERROR_LOG %#llx, STATUS_REG %#llx, CONTEXT_STATUS %#llx, F32_CNTL %#llx", o[7], o[8], o[9], o[10], o[11]);
	if (n >= 2) printf(", slot 0 PTE %#llx", o[13]);
	printf("\n");
	if (gmcDisplay(&after)) return 1;
	const int same = gmcDisplaySame(&before, &after);
	if (!o[1] && o[14] == 1 && st[1] == 1)
		printf("mmio timed out with nothing else wrong. Same-boot fallback, each its own go: sdma stop, sdma start db, sdma test 1.\n");
	else if (!o[1]) printf("record the lines above, then sdma stop. Nothing more until the kext is reloaded.\n");
	printf("display check: confirm the desktop is still on the monitor 60 s from now.\n");
	return !(o[1] == 1 && same);
}

static int doSdma(int argc, char **argv) {
	const char *sub = argv[2];
	uint64_t st[15], o[10] = {0};
	if (!strcmp(sub, "flags") && argc == 3) { if (sdmaFlags(st)) return 1; sdmaPrintFlags(st); return 0; }
	if (!strcmp(sub, "state") && argc == 3) return sdmaState();
	if (!strcmp(sub, "start") && argc == 4) {
		if (!strcmp(argv[3], "mmio")) return sdmaStart(1);
		if (!strcmp(argv[3], "db")) return sdmaStart(2);
		uint64_t m = num(argv[3]);   // raw number, for sdma-negative.sh only
		if (call(aSdmaStart, &m, 1, o, 10)) return 1;
		printf("start %llu: ok %llu\n", m, o[1]);
		return o[1] == 1 ? 0 : 1;
	}
	if (!strcmp(sub, "test") && argc == 4) return sdmaTest(num(argv[3]));
	if (!strcmp(sub, "stop") && argc == 3) {
		if (call(aSdmaStop, NULL, 0, o, 10)) { printf("E6: REFUSED by the kext\n"); return 1; }
		printf("E6: %s, %llu writes, %llu mismatches, RB_RPTR %#llx -> %#llx over 1 s, now %#llx, F32_CNTL %#llx (want HALT 1), RB_CNTL %#llx\n",
		       o[1] ? "done" : "FAILED (latched)", o[3], o[4], o[5], o[6], o[7], o[8], o[9]);
		if (sdmaJournal(o[2], o[3])) return 1;
		if (sdmaFlags(st)) return 1;
		sdmaPrintFlags(st);
		return o[1] == 1 ? 0 : 1;
	}
	if (!strcmp(sub, "journal") && argc == 4) return sdmaJournal(num(argv[3]), 1);
	usage();
	return 2;
}

// ---- cp: stage 3 step 6, one MEC compute queue (overview repo stage3-step6-compute-design.md) ---------------------
// n33ctl cp state            K0, read-only: the kext's step-6 state and the global CP registers (refused before autoload)
// n33ctl cp hqd              K0, read-only: the 56 HQD registers of ME 1 pipe 0 queue 0 (writes only the GRBM select)
// n33ctl cp start db|mmio    K1: MEC pipe 0, the HQD, the doorbell (db) or MMIO write pointer (mmio) kick; after C1 and G6
// n33ctl cp test 1..5        K2 SET_UCONFIG_REG to SCRATCH_REG0, K3 WRITE_DATA via GART slot 1, K4 the store_magic
//                            dispatch, K5a the dispatch to the VRAM page without GL2_WB, K5b a RELEASE_MEM with GL2_WB
// n33ctl cp stop             K6: drain (the reset stage if needed), pointers and doorbell off, MEC halted
// The kext computes every value and builds every packet; this tool keeps the order and stops after each step for the
// display check. Each step is William's go. If K2 times out in db mode: cp stop, cp start mmio, cp test 1 (D4).
enum { aCpState = 29, aCpHqd = 30, aCpStart = 31, aCpTest = 32, aCpStop = 33, aCpRecord = 34, aCpIb = 35, aCpSubmit = 36, aCpWait = 37, aCpXib = 38 };
static const struct { uint32_t reg; const char *name; } kCpNames[] = {
	{ 0xC904, "CP_MEC_RS64_CNTL" }, { 0xC908, "CP_MEC_RS64_INSTR_PNTR" }, { 0x3083, "CP_PQ_WPTR_POLL_CNTL" },
	{ 0x30B8, "CP_PQ_STATUS" }, { 0x305C, "CP_MEC_DOORBELL_RANGE_LOWER" }, { 0x305D, "CP_MEC_DOORBELL_RANGE_UPPER" },
	{ 0xA98A, "RLC_CP_SCHEDULERS" }, { 0xC807, "CP_MES_CNTL" }, { 0x21A0, "CP_STAT" }, { 0x2004, "GRBM_STATUS" },
	{ 0x2084, "CP_CPC_STATUS" }, { 0xC040, "SCRATCH_REG0" }, { 0x2828, "GCVM_L2_PROTECTION_FAULT_STATUS" },
	{ 0x2829, "GCVM_L2_PROTECTION_FAULT_ADDR_LO32" }, { 0x282A, "GCVM_L2_PROTECTION_FAULT_ADDR_HI32" },
	{ 0xF84C, "CP_CPC_IC_BASE_LO" }, { 0xA900, "GRBM_GFX_CNTL" }, { 0xA9E4, "SH_MEM_CONFIG" },
	{ 0x31D3, "SPI_COMPUTE_QUEUE_RESET" }, { 0x507A40, "NBIO S2A_DOORBELL_ENTRY_0_CTRL (RSMU)" },
	{ 0x3209, "CP_MQD_BASE_ADDR" }, { 0x320A, "CP_MQD_BASE_ADDR_HI" }, { 0x320B, "CP_HQD_ACTIVE" }, { 0x320C, "CP_HQD_VMID" },
	{ 0x320D, "CP_HQD_PERSISTENT_STATE" }, { 0x320E, "CP_HQD_PIPE_PRIORITY" }, { 0x320F, "CP_HQD_QUEUE_PRIORITY" },
	{ 0x3210, "CP_HQD_QUANTUM" }, { 0x3211, "CP_HQD_PQ_BASE" }, { 0x3212, "CP_HQD_PQ_BASE_HI" }, { 0x3213, "CP_HQD_PQ_RPTR" },
	{ 0x3214, "CP_HQD_PQ_RPTR_REPORT_ADDR" }, { 0x3215, "CP_HQD_PQ_RPTR_REPORT_ADDR_HI" },
	{ 0x3216, "CP_HQD_PQ_WPTR_POLL_ADDR" }, { 0x3217, "CP_HQD_PQ_WPTR_POLL_ADDR_HI" }, { 0x3218, "CP_HQD_PQ_DOORBELL_CONTROL" },
	{ 0x3219, "(unnamed 0x1FB9, MQD reserved_144)" }, { 0x321A, "CP_HQD_PQ_CONTROL" }, { 0x321B, "CP_HQD_IB_BASE_ADDR" },
	{ 0x321C, "CP_HQD_IB_BASE_ADDR_HI" }, { 0x321D, "CP_HQD_IB_RPTR" }, { 0x321E, "CP_HQD_IB_CONTROL" },
	{ 0x321F, "CP_HQD_IQ_TIMER" }, { 0x3220, "CP_HQD_IQ_RPTR" }, { 0x3221, "CP_HQD_DEQUEUE_REQUEST" },
	{ 0x3222, "CP_HQD_DMA_OFFLOAD" }, { 0x3223, "CP_HQD_SEMA_CMD" }, { 0x3224, "CP_HQD_MSG_TYPE" },
	{ 0x3225, "CP_HQD_ATOMIC0_PREOP_LO" }, { 0x3226, "CP_HQD_ATOMIC0_PREOP_HI" }, { 0x3227, "CP_HQD_ATOMIC1_PREOP_LO" },
	{ 0x3228, "CP_HQD_ATOMIC1_PREOP_HI" }, { 0x3229, "CP_HQD_HQ_SCHEDULER0" }, { 0x322A, "CP_HQD_HQ_CONTROL0" },
	{ 0x322B, "CP_MQD_CONTROL" }, { 0x322C, "CP_HQD_HQ_STATUS1" }, { 0x322D, "CP_HQD_HQ_CONTROL1" },
	{ 0x322E, "CP_HQD_EOP_BASE_ADDR" }, { 0x322F, "CP_HQD_EOP_BASE_ADDR_HI" }, { 0x3230, "CP_HQD_EOP_CONTROL" },
	{ 0x3231, "CP_HQD_EOP_RPTR" }, { 0x3232, "CP_HQD_EOP_WPTR" }, { 0x3233, "CP_HQD_EOP_EVENTS" },
	{ 0x3234, "CP_HQD_CTX_SAVE_BASE_ADDR_LO" }, { 0x3235, "CP_HQD_CTX_SAVE_BASE_ADDR_HI" }, { 0x3236, "CP_HQD_CTX_SAVE_CONTROL" },
	{ 0x3237, "CP_HQD_CNTL_STACK_OFFSET" }, { 0x3238, "CP_HQD_CNTL_STACK_SIZE" }, { 0x3239, "CP_HQD_WG_STATE_OFFSET" },
	{ 0x323A, "CP_HQD_CTX_SAVE_SIZE" }, { 0x323B, "CP_HQD_GDS_RESOURCE_STATE" }, { 0x323C, "CP_HQD_ERROR" },
	{ 0x323D, "CP_HQD_EOP_WPTR_MEM" }, { 0x323E, "CP_HQD_AQL_CONTROL" }, { 0x323F, "CP_HQD_PQ_WPTR_LO" },
	{ 0x3240, "CP_HQD_PQ_WPTR_HI" },
};
static const char *cpName(uint32_t reg) {
	static char b[24];
	for (unsigned i = 0; i < sizeof kCpNames / sizeof kCpNames[0]; i++) if (kCpNames[i].reg == reg) return kCpNames[i].name;
	snprintf(b, sizeof b, "dword %#x", reg);
	return b;
}

static int cpFlags(uint64_t st[15]) {
	uint64_t a = 0;
	memset(st, 0, 15 * sizeof st[0]);
	return call(aCpState, &a, 1, st, 15);
}
static void cpPrintFlags(const uint64_t *st) {
	static const char *mode[] = { "none", "mmio", "db" }, *latch[] = { "none", "timeout", "hard" };
	printf("kext: pages %llu running %llu mode %s latch %s timed-out modes %#llx  blob pinned %llu (%llu bytes)  BAR2 %llu "
	       "(%llu bytes)  engine holds GART %llu  C1 %llu G6 %llu  SDMA running %llu  GC up %llu\n", st[0] & 1, st[0] >> 1 & 1,
	       st[1] < 3 ? mode[st[1]] : "?", st[2] < 3 ? latch[st[2]] : "?", st[3], st[0] >> 2 & 1, st[13], st[0] >> 3 & 1, st[12],
	       st[0] >> 4 & 1, st[0] >> 5 & 1, st[0] >> 6 & 1, st[0] >> 7 & 1, st[11]);
	printf("      wptr %llu dwords, fence seq %llu, tests ok %#llx, starts %llu, journal %llu, K5a data all %llu, "
	       "IB page owned %llu, S2A entry 0 as found %#llx, MEC doorbell range as found %#llx/%#llx, GRBM_STATUS at K1 %#llx\n",
	       st[4], st[5], st[6], st[7], st[8], st[0] >> 8 & 1, st[0] >> 9 & 1, st[9], st[10] & 0xFFFFFFFFull, st[10] >> 32, st[14]);
}
static int cpJournal(uint64_t first, uint64_t count) {
	static const char *phase[] = { "?", "K1", "K6" };
	for (uint64_t i = first; i < first + count; i++) {
		uint64_t a = i, o[7] = {0};
		if (call(aCpRecord, &a, 1, o, 7)) return 1;
		const char *ph = o[0] < 3 ? phase[o[0]] : "?";
		if (o[1] == 4) printf("  %s %-40s poll       %#010llx  last  %#010llx\n", ph, cpName((uint32_t)o[2]), o[3], o[4]);
		else printf("  %s %-40s before %#010llx  after %#010llx  target %#010llx  %s%s\n", ph, cpName((uint32_t)o[2]), o[3], o[4],
		            o[5], ((o[4] ^ o[5]) & o[6]) ? "MISMATCH" : o[6] ? "ok" : "recorded", o[1] == 5 ? " (reset stage)" : "");
	}
	return 0;
}

// K0, read-only. The kext refuses before autoload (no GC register is read then).
static int cpState(void) {
	uint64_t st[15], a = 1, o[3] = {0};
	int ffff = 0;
	if (cpFlags(st)) return 1;
	cpPrintFlags(st);
	if (call(aCpState, &a, 1, o, 3)) { printf("cp state: REFUSED by the kext (GC not up?)\n"); return 1; }
	const uint64_t n = o[2];
	for (uint64_t i = 1; i <= n; i++) {
		a = i;
		if (call(aCpState, &a, 1, o, 3)) return 1;
		if (o[1] == 0xFFFFFFFFull) ffff++;
		printf("  %-40s %#010llx%s\n", cpName((uint32_t)o[0]), o[1], o[1] == 0xFFFFFFFFull ? "  (0xffffffff)" : "");
	}
	if (ffff) printf("%d register(s) read 0xffffffff\n", ffff);
	return ffff ? 1 : 0;
}
static int cpHqd(void) {
	uint64_t a = 0, o[3] = {0};
	int ffff = 0;
	if (call(aCpHqd, &a, 1, o, 3)) { printf("cp hqd: REFUSED by the kext (GC not up?)\n"); return 1; }
	const uint64_t n = o[2];
	printf("HQD block, ME 1 pipe 0 queue 0 (selected, then deselected):\n");
	for (uint64_t i = 0; i < n; i++) {
		a = i;
		if (call(aCpHqd, &a, 1, o, 3)) return 1;
		if (o[1] == 0xFFFFFFFFull) ffff++;
		printf("  %-40s %#010llx%s\n", cpName((uint32_t)o[0]), o[1], o[1] == 0xFFFFFFFFull ? "  (0xffffffff)" : "");
	}
	if (ffff) printf("%d register(s) read 0xffffffff: K0 not done\n", ffff);
	else printf("K0: no HQD register reads 0xffffffff. Want CP_HQD_ACTIVE 0, CP_HQD_ERROR 0, CP_HQD_PQ_CONTROL 0x00308509, "
	            "and CP_MEC_RS64_CNTL 0x40000000 in cp state\n");
	return ffff ? 1 : 0;
}

static int cpStart(uint64_t mode) {
	uint64_t st[15], o[16] = {0};
	struct gmcDisp before, after;
	if (cpFlags(st)) return 1;
	cpPrintFlags(st);
	if (gmcDisplay(&before)) return 1;
	if (call(aCpStart, &mode, 1, o, 16)) { printf("K1 (%s): REFUSED by the kext\n", mode == 2 ? "db" : "mmio"); return 1; }
	printf("K1 (%s): %s, %llu writes, %llu mismatches\n", mode == 2 ? "db" : "mmio", o[1] ? "done" : "FAILED (latched: cp stop)", o[3], o[4]);
	if (cpJournal(o[2], o[3]) || gmcDisplay(&after)) return 1;
	printf("CP_MEC_RS64_CNTL %#llx (want 0x04000000), INSTR_PNTR %#llx -> %#llx (recorded), CP_HQD_ACTIVE %#llx (want 1), "
	       "PQ_BASE %#llx (want 0x8008a050), EOP_CONTROL %#llx (want 0x9), PQ_RPTR %#llx (want 0), HQD_ERROR %#llx (want 0), "
	       "GC fault status %#llx\n", o[5], o[6], o[7], o[8], o[9], o[10], o[11], o[12], o[13]);
	printf("DOORBELL_CONTROL %#llx (want %s), CP_PQ_STATUS %#llx", o[14] & 0xFFFFFFFFull, mode == 2 ? "0x40000018" : "0", o[14] >> 32);
	if (mode == 2) printf(" (want bit 1), S2A entry 0 %#llx (want fields 0x30000007 under 0xf1ffffbf), doorbell at BAR2 + 0x18",
	                      o[15] & 0xFFFFFFFFull);
	printf("\ncode page %s, shader blob %s\n", o[15] >> 32 & 1 ? "reads back" : "MISMATCH", o[15] >> 33 & 1 ? "pinned" : "NOT pinned (K4 refused)");
	const int same = gmcDisplaySame(&before, &after);
	printf("display check: confirm the desktop is still on the monitor 60 s from now. cp test 1 is the next go.\n");
	return !(o[1] == 1 && same);
}

static int cpTest(uint64_t n) {
	static const char *what[] = { "", "SET_UCONFIG_REG to SCRATCH_REG0 (K2)", "WRITE_DATA to host page B + 0x100 via GART slot 1 (K3)",
	                              "store_magic dispatch to host page B, fence + 0x180 (K4)",
	                              "dispatch to the VRAM page, fence without GL2_WB + 0x188 (K5a)", "RELEASE_MEM with GL2_WB + 0x190 (K5b)" };
	uint64_t st[15], o[16] = {0};
	struct gmcDisp before, after;
	if (cpFlags(st)) return 1;
	cpPrintFlags(st);
	if (gmcDisplay(&before)) return 1;
	if (call(aCpTest, &n, 1, o, 16)) { printf("test %llu: REFUSED by the kext\n", n); return 1; }
	printf("test %llu, %s, %s mode: %s after %llu us\n", n, n <= 5 ? what[n] : "?", st[1] == 2 ? "db" : "mmio",
	       o[1] ? "DONE" : o[14] == 1 ? "TIMEOUT (timeout only)" : "FAILED (latched)", o[15]);
	if (n <= 2) printf("  target %#llx reads %#llx (want 0xdeadbeef)", o[12], o[2]);
	else printf("  result MC %#llx: %llu of 32 dwords 0xca11ed47 + i; fence reads %#llx; first data / fence at %lld / %lld us",
	            o[12], o[2], o[3], (long long)(int32_t)(o[11] >> 32), (long long)(int32_t)(o[11] & 0xFFFFFFFFull));
	printf("\n  PQ_RPTR %#llx, wptr %llu dwords, rptr slot %#llx, ACTIVE %#llx, HQD_ERROR %#llx, GRBM_STATUS %#llx, GC fault status %#llx",
	       o[4], o[5], o[6], o[9], o[8], o[10], o[7]);
	if (n >= 2) printf(", slot 1 PTE %#llx", o[13]);
	printf("\n");
	if (n == 4 && o[1]) printf("  K5a: %s\n", o[2] == 32 ? "all 32 in VRAM without GL2_WB: the question is answered, K5b is not run"
	                                                    : "data short with the fence landed: K5b (cp test 5) is the next go");
	if (gmcDisplay(&after)) return 1;
	const int same = gmcDisplaySame(&before, &after);
	if (n == 1 && !o[1]) { cpHqd(); cpState(); }   // the design's K2 failure print
	if (!o[1] && o[14] == 1 && st[1] == 2)
		printf("db timed out with nothing else wrong. Same-boot fallback, each its own go: cp stop, cp start mmio, cp test 1.\n");
	else if (!o[1]) printf("record the lines above, then cp stop. Nothing more until the kext is reloaded.\n");
	printf("display check: confirm the desktop is still on the monitor 60 s from now.\n");
	return !(o[1] == 1 && same);
}

// T1 (step 7): K4's store_magic dispatch run from a VRAM IB through INDIRECT_BUFFER. After cp test 2 (K3) or test 3 (K4).
static int cpIb(void) {
	uint64_t st[15], o[16] = {0};
	struct gmcDisp before, after;
	if (cpFlags(st)) return 1;
	cpPrintFlags(st);
	if (gmcDisplay(&before)) return 1;
	if (call(aCpIb, NULL, 0, o, 16)) { printf("T1 (cp ib): REFUSED by the kext\n"); return 1; }
	printf("T1 (cp ib), %s mode: %s after %llu us\n", st[1] == 2 ? "db" : "mmio",
	       o[1] ? "DONE" : o[14] == 1 ? "TIMEOUT (timeout only)" : "FAILED (latched)", o[15]);
	printf("  IB at MC %#llx, %llu dwords, read-back %s; host page B: %llu of 32 dwords 0xca11ed47 + i; fence reads %#llx; "
	       "first data / fence at %lld / %lld us\n", o[12], o[13] >> 8, o[13] & 1 ? "ok" : "MISMATCH", o[2], o[3],
	       (long long)(int32_t)(o[11] >> 32), (long long)(int32_t)(o[11] & 0xFFFFFFFFull));
	printf("  PQ_RPTR %#llx, wptr %llu dwords (ring: %llu-dword INDIRECT_BUFFER + NOP pad), rptr slot %#llx, ACTIVE %#llx, "
	       "HQD_ERROR %#llx, GRBM_STATUS %#llx, GC fault status %#llx\n", o[4], o[5], (o[13] >> 1) & 0x7f, o[6], o[9], o[8], o[10], o[7]);
	if (gmcDisplay(&after)) return 1;
	const int same = gmcDisplaySame(&before, &after);
	if (!o[1] && o[14] == 1 && st[1] == 2)
		printf("db timed out with nothing else wrong. Same-boot fallback, each its own go: cp stop, cp start mmio, step 6 to K3, cp ib.\n");
	else if (!o[1]) printf("record the lines above, then cp stop. Nothing more until the kext is reloaded.\n");
	printf("display check: confirm the desktop is still on the monitor 60 s from now.\n");
	return !(o[1] == 1 && same);
}

// W1 (step 7): cp submit, the winsys submit half of cp ib. Builds and kicks T1's IB, returns a seqno; cp waitseq polls it.
static int cpSubmit(void) {
	uint64_t st[15], o[16] = {0};
	if (cpFlags(st)) return 1;
	cpPrintFlags(st);
	if (call(aCpSubmit, NULL, 0, o, 16)) { printf("W1 (cp submit): REFUSED by the kext\n"); return 1; }
	printf("W1 submit, %s mode: seqno %llu kicked; IB at MC %#llx, %llu dwords, read-back %s; PQ_RPTR %#llx, wptr %llu dwords\n",
	       st[1] == 2 ? "db" : "mmio", o[1], o[12], o[13] >> 8, o[13] & 1 ? "ok" : "MISMATCH", o[4], o[5]);
	printf("next go: n33ctl cp waitseq %llu (polls the RELEASE_MEM fence of this submit)\n", o[1]);
	return 0;
}

// X4b (step 7): cp xib 1|2, cp submit with the kext's fixed gfx10.3 stream translated by xlat12 (1 wd, 2 dispatch).
static int cpXib(uint64_t stream) {
	uint64_t st[15], o[16] = {0};
	if (cpFlags(st)) return 1;
	cpPrintFlags(st);
	if (call(aCpXib, &stream, 1, o, 16)) { printf("X4b (cp xib %llu): REFUSED by the kext\n", stream); return 1; }
	printf("X4b xib %llu (%s), %s mode: seqno %llu kicked; translated IB at MC %#llx, %llu dwords, read-back %s; PQ_RPTR %#llx, "
	       "wptr %llu dwords\n", stream, stream == 1 ? "wd" : "dispatch", st[1] == 2 ? "db" : "mmio", o[1], o[12], o[13] >> 8,
	       o[13] & 1 ? "ok" : "MISMATCH", o[4], o[5]);
	printf("next go: n33ctl cp waitseq %llu (polls the RELEASE_MEM fence of this submit)\n", o[1]);
	return 0;
}

// W1 (step 7): cp waitseq <seq>, the winsys wait half. Polls the outstanding submit's fence and reports retired.
static int cpWait(uint64_t seq) {
	uint64_t st[15], o[16] = {0};
	struct gmcDisp before, after;
	if (cpFlags(st)) return 1;
	cpPrintFlags(st);
	if (gmcDisplay(&before)) return 1;
	if (call(aCpWait, &seq, 1, o, 16)) { printf("W1 (cp waitseq %llu): REFUSED by the kext\n", seq); return 1; }
	printf("W1 waitseq %llu, %s mode: %s after %llu us\n", seq, st[1] == 2 ? "db" : "mmio",
	       o[1] ? "RETIRED" : o[14] == 1 ? "TIMEOUT (timeout only)" : "FAILED (latched)", o[15]);
	if (o[12] == 1)   // X4b wd: one dword, 0xca11ed47 + seqno at host page B + 0x100
		printf("  stream wd: host page B: %llu of %llu dword 0xca11ed47 + seqno at +0x100; fence reads %#llx; first data / fence at %lld / %lld us\n",
		       o[2], o[13], o[3], (long long)(int32_t)(o[11] >> 32), (long long)(int32_t)(o[11] & 0xFFFFFFFFull));
	else if (o[12] == 3 || o[12] == 5)   // bo copy (W2) / vm copy (W2V): out[2] is the count of destination samples carrying the pattern (of 16)
		printf("  stream %s copy: %llu of %llu destination samples carry the pattern; fence reads %#llx\n", o[12] == 5 ? "vm" : "bo", o[2], o[13], o[3]);
	else if (o[12] == 4 || o[12] == 7)   // bo fault (W2) / vm fault (W2V): gfx11 compute cannot continue past a VM fault, so the queue halts
		printf("  stream %s fault: %s; GCVM_L2_PROTECTION_FAULT_STATUS %#llx, logical page %#llx, after the clear %#llx\n",
		       o[12] == 7 ? "vm" : "bo",
		       o[1] ? "fault latched as expected and the queue halted (recover with cp stop + cp start)" : "NOT the expected fault, or the queue did not halt",
		       o[2], o[13], o[6]);
	else if (o[12] == 6)   // vm dispatch (W2V): 32 dwords of 0xca11ed47 + i in the destination BO
		printf("  stream vm dispatch: %llu of %llu dwords 0xca11ed47 + i in the destination BO; fence reads %#llx\n", o[2], o[13], o[3]);
	else
		printf("  stream %s: host page B: %llu of %llu dwords 0xca11ed47 + i; fence reads %#llx; first data / fence at %lld / %lld us\n",
		       o[12] == 2 ? "dispatch" : "K4", o[2], o[13], o[3], (long long)(int32_t)(o[11] >> 32),
		       (long long)(int32_t)(o[11] & 0xFFFFFFFFull));
	printf("  PQ_RPTR %#llx, wptr %llu dwords, rptr slot %#llx, ACTIVE %#llx, HQD_ERROR %#llx, GRBM_STATUS %#llx, GC fault status %#llx\n",
	       o[4], o[5], o[6], o[9], o[8], o[10], o[7]);
	if (gmcDisplay(&after)) return 1;
	const int same = gmcDisplaySame(&before, &after);
	if (!o[1] && o[14] == 1 && st[1] == 2)
		printf("db timed out with nothing else wrong. Same-boot fallback, each its own go: cp stop, cp start mmio, step 6 to K3, cp submit.\n");
	else if (!o[1]) printf("record the lines above, then cp stop. Nothing more until the kext is reloaded.\n");
	printf("display check: confirm the desktop is still on the monitor 60 s from now.\n");
	return !(o[1] == 1 && same);
}

static int doCp(int argc, char **argv) {
	const char *sub = argv[2];
	uint64_t st[15], o[10] = {0};
	if (!strcmp(sub, "flags") && argc == 3) { if (cpFlags(st)) return 1; cpPrintFlags(st); return 0; }
	if (!strcmp(sub, "state") && argc == 3) return cpState();
	if (!strcmp(sub, "hqd") && argc == 3) return cpHqd();
	if (!strcmp(sub, "start") && argc == 4) {
		if (!strcmp(argv[3], "mmio")) return cpStart(1);
		if (!strcmp(argv[3], "db")) return cpStart(2);
		uint64_t m = num(argv[3]);   // raw number (0 for a word), for cp-negative.sh only
		if (call(aCpStart, &m, 1, o, 10)) return 1;
		printf("start %llu: ok %llu\n", m, o[1]);
		return o[1] == 1 ? 0 : 1;
	}
	if (!strcmp(sub, "test") && argc == 4) return cpTest(num(argv[3]));
	if (!strcmp(sub, "ib") && argc == 3) return cpIb();
	if (!strcmp(sub, "submit") && argc == 3) return cpSubmit();
	if (!strcmp(sub, "waitseq") && argc == 4) return cpWait(num(argv[3]));
	if (!strcmp(sub, "xib") && argc == 4) return cpXib(num(argv[3]));
	if (!strcmp(sub, "stop") && argc == 3) {
		if (call(aCpStop, NULL, 0, o, 10)) { printf("K6: REFUSED by the kext\n"); return 1; }
		printf("K6: %s, %llu writes, %llu mismatches, drain %s%s, ACTIVE %#llx (want 0), PQ_RPTR %#llx (want 0), "
		       "DOORBELL_CONTROL %#llx (want 0), CP_MEC_RS64_CNTL %#llx (want MEC_HALT)\n", o[1] ? "done" : "FAILED (latched)", o[3],
		       o[4], o[9] & 1 ? "TIMED OUT" : "done", o[9] & 2 ? " after the reset stage" : "", o[5], o[6], o[7], o[8]);
		if (cpJournal(o[2], o[3])) return 1;
		if (cpFlags(st)) return 1;
		cpPrintFlags(st);
		return o[1] == 1 ? 0 : 1;
	}
	if (!strcmp(sub, "journal") && argc == 4) return cpJournal(num(argv[3]), 1);
	usage();
	return 2;
}

// ---- dmub: the stage 3 DMCUB bring-up proof (actions 60-65) ----------------------------------------
// snapshot (read-only, arms the gate) -> load <dcn_3_2_1_dmcub.bin> (PSP type 51) -> reset -> init ->
// query (QUERY_FEATURE_CAPS, the proof) -> check (hold/revert). The firmware is staged into fw_buf first,
// its sha256 pinned, exactly as psp2 stages the GC firmware. The kext re-checks the order and every write.
enum { aDmubSnap = 60, aDmubReset = 61, aDmubLoad = 62, aDmubInit = 63, aDmubCmd = 64, aDmubCheck = 65 };
static const char *kDmubSha256 = "3aa486a0ce8fe477151f462ed162106c4ed520ceb006bc0791a1c2f7daf77371";
static int doDmub(int argc, char **argv) {
	const char *sub = argv[2];
	uint64_t o[9] = {0};
	if (!strcmp(sub, "snapshot") && argc == 3) {
		if (call(aDmubSnap, NULL, 0, o, 7)) { printf("dmub snapshot: REFUSED (why %llu: %s)\n", o[0], whyName(o[0])); return 1; }
		printf("dmub snapshot: DMCUB_CNTL %#llx SCRATCH0 %#llx inbox1(wptr<<32|rptr) %#llx size %#llx\n", o[0], o[1], o[2], o[3]);
		printf("  VBIOS %#llx bytes; DMUB block %#llx; HUBP0 on GOP %llu\n", o[4], o[5], o[6]);
		return 0;
	}
	if (!strcmp(sub, "load") && argc == 4) {
		FILE *f = fopen(argv[3], "rb");
		if (!f) { perror(argv[3]); return 1; }
		static uint8_t buf[0x40000];
		size_t n = fread(buf, 1, sizeof buf, f);
		fclose(f);
		char hex[65];
		sha256hex(buf, n, hex);
		if (strcmp(hex, kDmubSha256)) { fprintf(stderr, "refusing %s: sha256 %s (want %s)\n", argv[3], hex, kDmubSha256); return 1; }
		// The PSP gets only the inst-const region at ucode_array_offset_bytes (amdgpu_ucode.c:1008-1011), on a page as
		// Linux places it; stage the whole file so that region starts at fw_buf + 0x1000. The kext reparses the header.
		const uint32_t arr = n33_rd32(buf, 24);
		if (arr == 0 || arr > 0x1000) { fprintf(stderr, "refusing: ucode_array_offset_bytes %#x\n", arr); return 1; }
		const uint64_t at = kP2FwBuf + 0x1000 - arr;
		printf("dmub load: %s staged (%zu bytes, sha256 ok) at VRAM %#llx; inst_const %#x bytes at %#llx\n",
		       argv[3], n, at, n33_rd32(buf, 32), at + arr);
		if (p2stage(at, buf, (uint32_t)n, "dmub firmware")) return 1;
		uint64_t a[2] = { at, n };
		if (call(aDmubLoad, a, 2, o, 7)) { printf("dmub load: REFUSED (why %llu: %s)\n", o[0], whyName(o[0])); return 1; }
		printf("dmub load: PSP type-51 accepted  index %llu  %s  status %#llx\n", o[5], o[1] ? "fenced" : "TIMEOUT", o[2]);
		return 0;
	}
	if (!strcmp(sub, "reset") && argc == 3) {
		if (call(aDmubReset, NULL, 0, o, 3)) { printf("dmub reset: REFUSED (why %llu: %s)\n", o[0], whyName(o[0])); return 1; }
		printf("dmub reset: done  STOP_FW ack %llu  DMCUB_CNTL %#llx\n", o[0], o[1]);
		return 0;
	}
	if (!strcmp(sub, "init") && argc == 3) {
		if (call(aDmubInit, NULL, 0, o, 3)) { printf("dmub init: REFUSED (why %llu: %s)\n", o[0], whyName(o[0])); return 1; }
		printf("dmub init: auto-load %llu  SCRATCH0 %#llx  DMCUB_CNTL %#llx\n", o[0], o[1], o[2]);
		return o[0] == 1 ? 0 : 1;
	}
	if ((!strcmp(sub, "query") || !strcmp(sub, "cmd")) && argc == 3) {
		uint64_t a = 0;   // template 0 = QUERY_FEATURE_CAPS
		if (call(aDmubCmd, &a, 1, o, 5)) { printf("dmub %s: REFUSED (why %llu: %s)\n", sub, o[0], whyName(o[0])); return 1; }
		printf("dmub query: QUERY_FEATURE_CAPS %s  reply header %#llx  ret_status-ok %llu  wptr %#llx\n",
		       o[0] ? "returned" : "TIMED OUT", o[1], o[2], o[3]);
		return (o[0] == 1 && o[2] == 1) ? 0 : 1;   // pass = reply received and ret_status cleared to 0
	}
	if (!strcmp(sub, "check") && argc == 3) {
		if (call(aDmubCheck, NULL, 0, o, 5)) { printf("dmub check: REFUSED (why %llu: %s)\n", o[0], whyName(o[0])); return 1; }
		printf("dmub check: HUBP0 %#llx on GOP %llu  SCRATCH0 %#llx  DMCUB_CNTL %#llx  proof %llu\n", o[0], o[1], o[2], o[3], o[4]);
		return 0;
	}
	usage();
	return 2;
}

// ---- ih: stage 3 step 4, IH 6.0.2 ring 0 (actions 70-75) ------------------------------------------------------
// state (I0, read-only) -> start 1 (I1, polled) -> recv 0 (I2, CP EOP on the step-6 queue) [-> recv 1 if no IV] ->
// [arm (I3, n33-ih-msi=1) -> recv 0/1] -> cp stop -> sdma start, test 1..3 -> recv 2 (I4) [-> recv 3 if no IV] -> stop.
enum { aIhState = 70, aIhStart = 71, aIhRecv = 72, aIhArm = 73, aIhStop = 74, aIhJournal = 75 };
static const struct { uint32_t reg; const char *name; } kIhNames[] = {
	{ 0x1120, "IH_RB_CNTL" }, { 0x1121, "IH_RB_BASE" }, { 0x1122, "IH_RB_BASE_HI" }, { 0x1123, "IH_RB_RPTR" },
	{ 0x1124, "IH_RB_WPTR" }, { 0x1125, "IH_RB_WPTR_ADDR_HI" }, { 0x1126, "IH_RB_WPTR_ADDR_LO" }, { 0x1127, "IH_DOORBELL_RPTR" },
	{ 0x1160, "IH_CNTL" }, { 0x1175, "IH_INT_FLOOD_CNTL" }, { 0x117A, "IH_STORM_CLIENT_LIST_CNTL" }, { 0x1191, "IH_MSI_STORM_CTRL" },
	{ 0x122D, "IH_CHICKEN" }, { 0xE11, "BIF_BX0_INTERRUPT_CNTL" }, { 0xE12, "BIF_BX0_INTERRUPT_CNTL2" },
	{ 0x3085, "CP_ME1_PIPE0_INT_CNTL" }, { 0x127C, "SDMA0_CNTL" }, { 0x2828, "GCVM_L2_PROTECTION_FAULT_STATUS" },
	{ 0x1A68C, "MMVM_L2_PROTECTION_FAULT_STATUS" },
};
static const char *ihName(uint32_t reg) {
	static char b[24];
	for (unsigned i = 0; i < sizeof kIhNames / sizeof kIhNames[0]; i++) if (kIhNames[i].reg == reg) return kIhNames[i].name;
	snprintf(b, sizeof b, "dword %#x", reg);
	return b;
}
static int ihFlags(void) {
	uint64_t a = 0, st[15] = {0};
	if (call(aIhState, &a, 1, st, 15)) return 1;
	static const char *mode[] = { "none", "polled", "polled+intr" }, *latch[] = { "none", "timeout", "hard" };
	printf("kext: ring owned %llu running %llu mode %s armed %llu latch %s  G6 %llu GC up %llu  cp running %llu sdma running %llu\n",
	       st[0] & 1, st[0] >> 1 & 1, st[1] < 3 ? mode[st[1]] : "?", st[0] >> 2 & 1, st[2] < 3 ? latch[st[2]] : "?",
	       st[0] >> 3 & 1, st[0] >> 4 & 1, st[0] >> 7 & 1, st[0] >> 8 & 1);
	printf("      rptr %#llx starts %llu sources ok %#llx no-IV %#llx  MSI handler runs %llu (expected %llu)  journal %llu  IVs %llu\n",
	       st[3], st[4], st[5], st[6], st[7] & 0xFFFFFFFFull, st[7] >> 32, st[8], st[9]);
	printf("      PCI Command %#llx MSI control %#llx MSI-X control %#llx (quiet %llu)  n33-ih-msi %llu  dummy page %#llx\n",
	       st[10] & 0xFFFF, st[10] >> 16 & 0xFFFF, st[10] >> 32 & 0xFFFF, st[0] >> 5 & 1, st[11], st[12]);
	return 0;
}
static int ihJournal(uint64_t first, uint64_t count) {
	static const char *phase[] = { "?", "start", "recv", "arm", "stop" };
	for (uint64_t i = first; i < first + count; i++) {
		uint64_t a[2] = { 0, i }, o[7] = {0};
		if (call(aIhJournal, a, 2, o, 7)) return 1;
		printf("  %-5s %-34s before %#010llx  after %#010llx  target %#010llx  %s\n", o[0] < 5 ? phase[o[0]] : "?",
		       ihName((uint32_t)o[2]), o[3], o[4], o[5], ((o[4] ^ o[5]) & o[6]) ? "MISMATCH" : o[6] ? "ok" : "recorded");
	}
	return 0;
}
static int ihIv(uint64_t i) {
	uint64_t a[2] = { 1, i }, o[4] = {0};
	if (call(aIhJournal, a, 2, o, 4)) return 1;
	uint32_t d[8];
	for (int k = 0; k < 4; k++) { d[2 * k] = (uint32_t)o[k]; d[2 * k + 1] = (uint32_t)(o[k] >> 32); }
	printf("  IV %llu: %08x %08x %08x %08x %08x %08x %08x %08x  client %#x src %u ring %#x vmid %u pasid %u data0 %#x\n", i,
	       d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[0] & 0xFF, d[0] >> 8 & 0xFF, d[0] >> 16 & 0xFF, d[0] >> 24 & 0xF,
	       d[3] & 0xFFFF, d[4]);
	return 0;
}
static int doIh(int argc, char **argv) {
	const char *sub = argv[2];
	uint64_t o[16] = {0};
	if (!strcmp(sub, "flags") && argc == 3) return ihFlags();
	if (!strcmp(sub, "state") && argc == 3) {
		if (ihFlags()) return 1;
		for (uint64_t i = 1;; i++) {
			uint64_t a = i;
			if (call(aIhState, &a, 1, o, 3)) { if (i == 1) printf("ih state: REFUSED by the kext (before G6?)\n"); return i == 1; }
			printf("  %-34s %#010llx\n", ihName((uint32_t)o[0]), o[1]);
			if (i >= o[2]) return 0;
		}
	}
	if (!strcmp(sub, "start") && argc == 4) {
		uint64_t m = num(argv[3]);
		if (call(aIhStart, &m, 1, o, 16)) { printf("I1 (ih start %llu): REFUSED by the kext\n", m); return 1; }
		printf("I1 (mode %llu): %s, %llu writes, %llu mismatches, IH_RB_CNTL %#llx (want %#llx), RPTR %#llx WPTR %#llx shadow %#llx "
		       "(want 0), GC fault %#llx MMHUB fault %#llx, INTERRUPT_CNTL %#llx CNTL2 %#llx, slot 2 PTE %#llx\n", m,
		       o[1] ? "done" : "FAILED (latched; ih stop)", o[3], o[4], o[5], o[6], o[7], o[8], o[9], o[10], o[11], o[12], o[13], o[15]);
		if (ihJournal(o[2], o[3])) return 1;
		printf("display check: confirm the desktop is still on the monitor 60 s from now.\n");
		return o[1] == 1 ? 0 : 1;
	}
	if (!strcmp(sub, "recv") && argc == 4) {
		uint64_t v = num(argv[3]);
		static const char *oc[] = { "done", "fence landed, NO IV: the follow-up (recv 1 or 3) is the next go",
		                            "fence did NOT land: the queue's failure", "FAILED (latched; ih stop)" };
		if (call(aIhRecv, &v, 1, o, 16)) { printf("ih recv %llu: REFUSED by the kext\n", v); return 1; }
		printf("recv %llu: %s. fence %#llx (seq %llu) after %llu us, IH_RB_WPTR %#llx after %llu us, rptr %#llx, %llu entries "
		       "(%llu expected, %llu other), shadow %#llx, GC fault %#llx MMHUB fault %#llx, %s %#llx\n", v, o[2] < 4 ? oc[o[2]] : "?",
		       o[3], o[4], o[14] & 0xFFFFFFFFull, o[5], o[14] >> 32, o[6], o[7] & 0xFFFF, o[7] >> 16 & 0xFFFF, o[7] >> 32, o[11],
		       o[12] & 0xFFFFFFFFull, o[12] >> 32, v < 2 ? "CP_HQD_ERROR" : "SDMA ERROR_LOG", o[13]);
		if (o[7] & 0xFFFF) printf("first IV: dw0 %#010llx (want %s), dw1..2 %#llx, dw3..4 %#llx\n", o[8],
		                          v < 2 ? "0x0004b514" : "client 0x0a src 0x31", o[9], o[10]);
		if (v == 1 || v == 3) printf("interrupt enable now %#llx\n", o[15] >> 32);
		return o[1] == 1 ? 0 : 1;
	}
	if (!strcmp(sub, "arm") && argc == 3) {
		if (call(aIhArm, NULL, 0, o, 7)) { printf("I3 (ih arm): REFUSED by the kext\n"); return 1; }
		printf("I3: %s, MSI index %llu, IH_RB_CNTL %#llx, MSI control %#llx -> %#llx, PCI Command %#llx\n", o[1] ? "armed" : "FAILED (disarmed)",
		       o[3], o[4], o[5] & 0xFFFF, o[5] >> 16, o[6]);
		if (ihJournal(o[2], 1)) return 1;
		return o[1] == 1 ? 0 : 1;
	}
	if (!strcmp(sub, "stop") && argc == 3) {
		if (call(aIhStop, NULL, 0, o, 7)) { printf("I5 (ih stop): REFUSED by the kext\n"); return 1; }
		printf("I5: %s, %llu writes, %llu mismatches, IH_RB_CNTL %#llx (want RB_ENABLE 0, ENABLE_INTR 0), was armed %llu\n",
		       o[1] ? "done" : "FAILED (latched)", o[3], o[4], o[5], o[6]);
		if (ihJournal(o[2], o[3])) return 1;
		return o[1] == 1 ? 0 : 1;
	}
	if (!strcmp(sub, "journal") && argc == 4) return ihJournal(num(argv[3]), 1);
	if (!strcmp(sub, "iv") && argc == 4) return ihIv(num(argv[3]));
	if (!strcmp(sub, "journal-kind") && argc == 4) {   // raw, for ih-negative.sh only
		uint64_t a[2] = { num(argv[3]), 0 };
		return call(aIhJournal, a, 2, o, 7);
	}
	usage();
	return 2;
}

// ---- step 7 W2: buffer objects (n33bo::, design stage3-step7-w2-design.md). Handles 1..8. ----
enum { aBoCreate = 39, aBoImport = 40, aBoMap = 41, aBoUnmap = 42, aBoFree = 43, aBoInfo = 44, aBoRead = 45, aBoCopy = 46, aBoFault = 47 };
enum { kBoVmR = 1u << 1, kBoVmW = 1u << 2, kBoVmX = 1u << 3, kBoVmRWX = kBoVmR | kBoVmW | kBoVmX };   // uapi AMDGPU_VM_PAGE_*
enum { kBoCopyN = 16 };
static uint64_t boCopyOff(uint32_t k, uint64_t minBytes) { return ((uint64_t)k * (minBytes / kBoCopyN)) & ~3ull; }

static int boCreate(uint64_t kind, uint64_t bytes) {
	uint64_t a[2] = { kind, bytes }, o[4] = {0};
	if (call(aBoCreate, a, 2, o, 4)) { printf("bo create %s %llu: REFUSED by the kext\n", kind ? "gtt" : "vram", bytes); return 1; }
	printf("bo create %s: handle %llu, %llu pages, %s %#llx\n", kind ? "gtt" : "vram", o[1], o[2], kind ? "first page" : "VRAM", o[3]);
	return 0;
}
static int boInfo(uint64_t h) {
	uint64_t a = h, o[8] = {0};
	if (call(aBoInfo, &a, 1, o, 8)) { printf("bo info %llu: REFUSED by the kext\n", h); return 1; }
	static const char *kn[] = { "VRAM", "GTT", "host-import" };
	printf("bo %llu: %s, %llu pages, VA %#llx, %s %#llx, flags %#llx, %s, live PTE %#llx\n", h, o[1] < 3 ? kn[o[1]] : "?", o[2], o[3],
	       o[1] == 0 ? "slab offset" : "first page", o[4], o[5], o[6] ? "mapped" : "unmapped", o[7]);
	return 0;
}
static int doBo(int argc, char **argv) {
	const char *sub = argv[2];
	uint64_t o[4] = {0};
	if (!strcmp(sub, "create") && argc == 5) {
		if (!strcmp(argv[3], "vram")) return boCreate(0, num(argv[4]));
		if (!strcmp(argv[3], "gtt")) return boCreate(1, num(argv[4]));
		uint64_t a[2] = { num(argv[3]), num(argv[4]) };   // raw kind, for bo-negative.sh
		if (call(aBoCreate, a, 2, o, 4)) { printf("bo create REFUSED\n"); return 1; }
		printf("bo create: handle %llu, %llu pages, %#llx\n", o[1], o[2], o[3]); return 0;
	}
	if (!strcmp(sub, "import") && argc == 4) {   // allocate the caller's own page-aligned range and import it (negative tests; a leaked buffer)
		const uint64_t bytes = num(argv[3]);
		void *p = NULL;
		if (bytes && posix_memalign(&p, 4096, bytes)) { fprintf(stderr, "bo import: posix_memalign %llu failed\n", bytes); return 1; }
		if (p) memset(p, 0, bytes);
		uint64_t a[2] = { (uint64_t)(uintptr_t)p, bytes };
		if (call(aBoImport, a, 2, o, 4)) { printf("bo import %llu: REFUSED by the kext\n", bytes); return 1; }
		printf("bo import: handle %llu, %llu pages, first page %#llx (caller range %#llx)\n", o[1], o[2], o[3], a[0]);
		return 0;
	}
	if (!strcmp(sub, "map") && (argc == 5 || argc == 6)) {
		uint64_t a[3] = { num(argv[3]), num(argv[4]), argc == 6 ? num(argv[5]) : kBoVmRWX };
		if (call(aBoMap, a, argc == 6 ? 3 : 2, o, 4)) { printf("bo map %llu: REFUSED by the kext\n", a[0]); return 1; }
		printf("bo map h%llu at VA %#llx: %llu pages, first PTE %#llx\n", a[0], o[1], o[3], o[2]);
		return 0;
	}
	if (!strcmp(sub, "unmap") && argc == 4) {
		uint64_t a = num(argv[3]);
		if (call(aBoUnmap, &a, 1, o, 4)) { printf("bo unmap %llu: REFUSED by the kext\n", a); return 1; }
		printf("bo unmap h%llu: %llu pages -> dummy page\n", a, o[1]); return 0;
	}
	if (!strcmp(sub, "free") && argc == 4) {
		uint64_t a = num(argv[3]);
		if (call(aBoFree, &a, 1, o, 4)) { printf("bo free %llu: REFUSED by the kext\n", a); return 1; }
		printf("bo free h%llu: ok\n", a); return 0;
	}
	if (!strcmp(sub, "info") && argc == 4) return boInfo(num(argv[3]));
	if (!strcmp(sub, "read") && argc == 5) {
		uint64_t a[2] = { num(argv[3]), num(argv[4]) };
		if (call(aBoRead, a, 2, o, 2)) { printf("bo read %llu[%llu]: REFUSED by the kext\n", a[0], a[1]); return 1; }
		printf("bo %llu dword %llu = %#010llx\n", a[0], a[1], o[1]); return 0;
	}
	if (!strcmp(sub, "copy") && argc == 5) {
		uint64_t a[2] = { num(argv[3]), num(argv[4]) };
		if (call(aBoCopy, a, 2, o, 2)) { printf("bo copy %llu -> %llu: REFUSED by the kext\n", a[0], a[1]); return 1; }
		printf("bo copy h%llu -> h%llu: seqno %llu kicked; next go: n33ctl cp waitseq %llu\n", a[0], a[1], o[1], o[1]);
		return 0;
	}
	if (!strcmp(sub, "fault") && argc == 4) {
		uint64_t a = num(argv[3]);
		if (call(aBoFault, &a, 1, o, 2)) { printf("bo fault %#llx: REFUSED by the kext\n", a); return 1; }
		printf("bo fault %#llx: seqno %llu kicked; next go: n33ctl cp waitseq %llu\n", a, o[1], o[1]);
		return 0;
	}
	// import-test <vram h> <va>: the whole host-import proof in one process, so the caller's pages stay alive.
	if (!strcmp(sub, "import-test") && argc == 5) {
		const uint64_t vramH = num(argv[3]), va = num(argv[4]), bytes = 64u << 10;
		uint64_t vi[8] = {0}, a = vramH;
		if (call(aBoInfo, &a, 1, vi, 8)) { printf("import-test: bo info %llu REFUSED (create and map a VRAM BO first)\n", vramH); return 1; }
		if (vi[1] != 0 || !vi[6]) { printf("import-test: handle %llu is not a mapped VRAM BO\n", vramH); return 1; }
		void *p = NULL;
		if (posix_memalign(&p, 4096, bytes)) { fprintf(stderr, "import-test: posix_memalign failed\n"); return 1; }
		memset(p, 0, bytes);
		uint64_t im[4] = {0}, ia[2] = { (uint64_t)(uintptr_t)p, bytes };
		if (call(aBoImport, ia, 2, im, 4)) { printf("import-test: bo import REFUSED\n"); free(p); return 1; }
		const uint64_t importH = im[1];
		uint64_t ma[3] = { importH, va, kBoVmRWX }, mo[4] = {0};
		if (call(aBoMap, ma, 3, mo, 4)) { printf("import-test: bo map h%llu at %#llx REFUSED\n", importH, va); free(p); return 1; }
		uint64_t ca[2] = { vramH, importH }, co[2] = {0};
		if (call(aBoCopy, ca, 2, co, 2)) { printf("import-test: bo copy h%llu -> h%llu REFUSED\n", vramH, importH); free(p); return 1; }
		printf("import-test: bo copy seqno %llu; retiring with cp waitseq\n", co[1]);
		if (cpWait(co[1])) { printf("import-test: cp waitseq did not retire the copy\n"); free(p); return 1; }
		// The caller's own pages (p) are the import BO's pages. After the copy they hold the pattern; so does the VRAM
		// source read through bo read. Compare the two at each of the 16 sample offsets.
		const uint64_t minBytes = bytes;   // both BOs are 64 KiB
		int ok = 1;
		for (uint32_t k = 0; k < kBoCopyN; k++) {
			const uint64_t off = boCopyOff(k, minBytes);
			uint64_t ra[2] = { vramH, off / 4 }, ro[2] = {0};
			if (call(aBoRead, ra, 2, ro, 2)) { ok = 0; break; }
			const uint32_t mine = *(volatile uint32_t *)((uint8_t *)p + off);
			if (mine != (uint32_t)ro[1]) { printf("import-test: sample %u at +%#llx: my page %#010x != VRAM %#010llx\n", k, off, mine, ro[1]); ok = 0; }
		}
		uint64_t ua = importH, uo[4] = {0};
		call(aBoUnmap, &ua, 1, uo, 4);
		call(aBoFree, &ua, 1, uo, 4);
		free(p);
		printf("import-test: %s (the caller's own mapping sees the GPU's writes)\n", ok ? "PASS" : "FAIL");
		return ok ? 0 : 1;
	}
	usage();
	return 2;
}

// ---- step 7 W2V: a per-process GPU VM in VMID 8 (n33vm::, design stage3-step7-gpuvm-design.md) ----
enum { aVmCreate = 48, aVmMap = 49, aVmUnmap = 50, aVmInfo = 51, aVmCopy = 52, aVmDispatch = 53, aVmFault = 54, aVmDestroy = 55 };
static int doVm(int argc, char **argv) {
	const char *sub = argv[2];
	uint64_t o[8] = {0};
	if (!strcmp(sub, "create") && argc == 3) {
		if (call(aVmCreate, NULL, 0, o, 8)) { printf("vm create: REFUSED by the kext\n"); return 1; }
		printf("vm create: root PA %#llx, CONTEXT8_CNTL %#llx, SH_MEM_CONFIG %#llx, SH_MEM_BASES %#llx, BASE %#llx\n", o[1], o[2], o[3], o[4], o[5]);
		return 0;
	}
	if (!strcmp(sub, "map") && (argc == 5 || argc == 7 || argc == 8)) {
		// vm map <h> <va> [<off> <size> [<flags>]]
		uint64_t a[5] = { num(argv[3]), num(argv[4]), argc >= 7 ? num(argv[5]) : 0, argc >= 7 ? num(argv[6]) : 0, argc == 8 ? num(argv[7]) : kBoVmRWX };
		const uint32_t n = argc == 5 ? 2u : argc == 7 ? 4u : 5u;
		if (call(aVmMap, a, n, o, 8)) { printf("vm map h%llu at %#llx: REFUSED by the kext\n", a[0], a[1]); return 1; }
		printf("vm map h%llu at VA %#llx: %llu pages, first leaf %#llx, last leaf %#llx\n", a[0], o[1], o[4], o[2], o[3]);
		return 0;
	}
	if (!strcmp(sub, "unmap") && argc == 5) {
		uint64_t a[2] = { num(argv[3]), num(argv[4]) };
		if (call(aVmUnmap, a, 2, o, 8)) { printf("vm unmap h%llu at %#llx: REFUSED by the kext\n", a[0], a[1]); return 1; }
		printf("vm unmap h%llu at VA %#llx: %llu pages\n", a[0], a[1], o[1]); return 0;
	}
	if (!strcmp(sub, "info") && (argc == 3 || argc == 4)) {
		uint64_t a = argc == 4 ? num(argv[3]) : 0, vi[10] = {0};
		if (call(aVmInfo, argc == 4 ? &a : NULL, argc == 4 ? 1 : 0, vi, 10)) { printf("vm info: REFUSED by the kext\n"); return 1; }
		printf("vm info: live %llu, CONTEXT8_CNTL %#llx, base %#llx, start %#llx, end %#llx, arena pages %llu, mappings %llu\n",
		       vi[7], vi[1], vi[2], vi[3], vi[4], vi[5], vi[6]);
		if (argc == 4) printf("  VA %#llx: leaf %#llx -> pa %#llx\n", a, vi[8], vi[9]);
		return 0;
	}
	if (!strcmp(sub, "copy") && argc == 6) {
		uint64_t a[3] = { num(argv[3]), num(argv[4]), num(argv[5]) };   // ib va, src va, dst va
		if (call(aVmCopy, a, 3, o, 2)) { printf("vm copy: REFUSED by the kext\n"); return 1; }
		printf("vm copy ib %#llx src %#llx dst %#llx: seqno %llu kicked; next go: n33ctl cp waitseq %llu\n", a[0], a[1], a[2], o[1], o[1]);
		return 0;
	}
	if (!strcmp(sub, "dispatch") && argc == 6) {
		uint64_t a[3] = { num(argv[3]), num(argv[4]), num(argv[5]) };   // ib va, code va, dst va
		if (call(aVmDispatch, a, 3, o, 2)) { printf("vm dispatch: REFUSED by the kext\n"); return 1; }
		printf("vm dispatch ib %#llx code %#llx dst %#llx: seqno %llu kicked; next go: n33ctl cp waitseq %llu\n", a[0], a[1], a[2], o[1], o[1]);
		return 0;
	}
	if (!strcmp(sub, "fault") && argc == 5) {
		uint64_t a[2] = { num(argv[3]), num(argv[4]) };   // ib va, fault va
		if (call(aVmFault, a, 2, o, 2)) { printf("vm fault: REFUSED by the kext\n"); return 1; }
		printf("vm fault ib %#llx va %#llx: seqno %llu kicked; next go: n33ctl cp waitseq %llu\n", a[0], a[1], o[1], o[1]);
		return 0;
	}
	if (!strcmp(sub, "destroy") && argc == 3) {
		if (call(aVmDestroy, NULL, 0, o, 8)) { printf("vm destroy: REFUSED by the kext (BASE %#llx)\n", o[1]); return 1; }
		printf("vm destroy: CONTEXT8 parked (BASE %#llx)\n", o[1]); return 0;
	}
	usage();
	return 2;
}

// fbsum: SHA-256 of the GOP scanout in VRAM, read through action 2 (one BAR0 dword per call, read-only).
// The region is the kext's GOP range from status: HUBP0 address at kext start, pitch px * 4 * height, page rounded.
// Run before and after card work and compare: a change means something wrote the scanout.
static int fbSum(uint64_t bytes) {
	uint64_t s[11] = {0};
	if (call(aStatus, NULL, 0, s, 11)) return 1;
	const uint64_t off = s[3], len = bytes ? bytes : s[4];
	if (!len || (len & 3) || off > s[0] || s[0] - off < len) { fprintf(stderr, "fbsum: bad range VRAM %#llx len %#llx\n", off, len); return 1; }
	if (s[5] - s[1] != off) fprintf(stderr, "fbsum: note: HUBP0 now scans VRAM %#llx, not the GOP range\n", s[5] - s[1]);
	CC_SHA256_CTX c; CC_SHA256_Init(&c);
	for (uint64_t a = off; a < off + len; a += 4) {
		uint64_t v = 0; uint64_t in = a; uint32_t w;
		if (call(aBar0Read, &in, 1, &v, 1)) { fprintf(stderr, "fbsum: read failed at VRAM %#llx\n", a); return 1; }
		w = (uint32_t)v;
		CC_SHA256_Update(&c, &w, 4);
	}
	unsigned char d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(d, &c);
	printf("fbsum VRAM %#llx len %#llx (%llu bytes) sha256 ", off, len, len);
	for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) printf("%02x", d[i]);
	printf("\n");
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 2) { usage(); return 2; }
	if (!strcmp(argv[1], "why") && argc == 3) { printf("%s\n", whyName(num(argv[2]))); return 0; }

	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching("Navi48Bringup"));
	if (!svc) { fprintf(stderr, "Navi48Bringup not found (is the N33 observe kext loaded?)\n"); return 1; }
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0 /* legacy admin client */, &conn);
	IOObjectRelease(svc);
	if (kr != kIOReturnSuccess) { fprintf(stderr, "IOServiceOpen failed: 0x%x (run as root)\n", kr); return 1; }

	int rc = 0;
	const char *cmd = argv[1];
	uint64_t o[9] = {0};

	if (!strcmp(cmd, "status")) {
		rc = doStatus();
	} else if (!strcmp(cmd, "protected")) {
		uint64_t s[9] = {0};
		rc = call(aStatus, NULL, 0, s, 9);
		for (uint64_t i = 0; !rc && i < s[8]; i++) {
			uint64_t a = i, p[3] = {0};
			rc = call(aProtected, &a, 1, p, 3);
			if (!rc) printf("[%llu] VRAM %#llx len %#llx  %s\n", i, p[0], p[1], kindName(p[2]));
		}
	} else if (!strcmp(cmd, "reg") && argc == 3) {
		uint64_t in = num(argv[2]); uint32_t got = 1;
		kr = IOConnectCallScalarMethod(conn, kSelRegRead, &in, 1, o, &got);
		if (kr != kIOReturnSuccess) { fprintf(stderr, "reg read failed: 0x%x\n", kr); rc = 1; }
		else printf("dword %#llx = %#llx\n", in, o[0]);
	} else if (!strcmp(cmd, "regwrite") && argc == 4) {
		uint64_t a[2] = { num(argv[2]), num(argv[3]) };
		rc = call(aRegWrite, a, 2, o, 3);
		if (!rc) printf("dword %#llx: %#llx -> %#llx\n", a[0], o[1], o[2]);
	} else if (!strcmp(cmd, "r") && argc == 3) {
		uint64_t a = num(argv[2]);
		rc = call(aBar0Read, &a, 1, o, 1);
		if (!rc) printf("VRAM %#llx = %#llx\n", a, o[0]);
	} else if (!strcmp(cmd, "w") && argc == 4) {
		uint64_t a[2] = { num(argv[2]), num(argv[3]) };
		rc = call(aBar0Write, a, 2, o, 1);
		if (!rc) printf("wrote VRAM %#llx = %#llx\n", a[0], a[1]);
	} else if (!strcmp(cmd, "copy") && argc == 5) {
		uint64_t a[3] = { num(argv[2]), num(argv[3]), num(argv[4]) };
		rc = call(aCopy, a, 3, o, 1);
		if (!rc) printf("copied VRAM %#llx -> %#llx, %llu bytes\n", a[0], a[1], a[2]);
	} else if (!strcmp(cmd, "fill") && argc == 5) {
		uint64_t a[3] = { num(argv[2]), num(argv[3]), num(argv[4]) };
		rc = call(aFill, a, 3, o, 1);
		if (!rc) printf("filled VRAM %#llx, %llu bytes with %#llx\n", a[0], a[1], a[2]);
	} else if (!strcmp(cmd, "flip") && argc == 3) {
		// Point HUBP0 at a VRAM offset. GPU address = FB base + offset; the low dword latches the
		// flip, so write HIGH first (the kext refuses a HIGH that leaves the current 4 GiB), then
		// LOW. The kext also refuses a LOW whose target is neither the GOP surface nor writable.
		uint64_t s[9] = {0};
		rc = call(aStatus, NULL, 0, s, 9);
		if (!rc) {
			uint64_t gpu = s[1] + num(argv[2]);
			uint64_t hi[2] = { kHubp0AddrHigh, gpu >> 32 }, lo[2] = { kHubp0Addr, gpu & 0xffffffff };
			rc = call(aRegWrite, hi, 2, o, 3);
			if (!rc) rc = call(aRegWrite, lo, 2, o, 3);
			if (!rc) printf("HUBP0 scanout -> GPU %#llx (VRAM %#llx)\n", gpu, num(argv[2]));
		}
	} else if (!strcmp(cmd, "psp") && argc >= 4) {
		rc = doPsp(argc, argv);
	} else if (!strcmp(cmd, "psp2") && argc >= 3) {
		rc = doPsp2(argc, argv);
	} else if (!strcmp(cmd, "smu") && argc >= 3) {
		rc = doSmu(argc, argv);
	} else if (!strcmp(cmd, "gmc") && argc >= 3) {
		rc = doGmc(argc, argv);
	} else if (!strcmp(cmd, "gfx") && argc == 3 && !strcmp(argv[2], "rs64")) {
		rc = gfxRs64();
	} else if (!strcmp(cmd, "sdma") && argc >= 3) {
		rc = doSdma(argc, argv);
	} else if (!strcmp(cmd, "cp") && argc >= 3) {
		rc = doCp(argc, argv);
	} else if (!strcmp(cmd, "dmub") && argc >= 3) {
		rc = doDmub(argc, argv);
	} else if (!strcmp(cmd, "ih") && argc >= 3) {
		rc = doIh(argc, argv);
	} else if (!strcmp(cmd, "bo") && argc >= 3) {
		rc = doBo(argc, argv);
	} else if (!strcmp(cmd, "vm") && argc >= 3) {
		rc = doVm(argc, argv);
	} else if (!strcmp(cmd, "fbsum") && argc <= 3) {
		rc = fbSum(argc == 3 ? num(argv[2]) : 0);
	} else if (!strcmp(cmd, "log") && argc == 2) {
		// The negative tests read refusals here: logd can quarantine the kernel and drop IOLog lines.
		char chunk[4097];
		uint64_t off = 0, total = 0;
		do {
			size_t sz = sizeof chunk - 1; uint32_t n = 1;
			kr = IOConnectCallMethod(conn, kSelReadLog, &off, 1, NULL, 0, &total, &n, chunk, &sz);
			if (kr != kIOReturnSuccess) { fprintf(stderr, "log: ReadLog failed 0x%x\n", kr); rc = 1; break; }
			if (sz == 0) break;
			chunk[sz] = 0;
			fputs(chunk, stdout);
			off += sz;
		} while (off < total);
		if (total >= kLogBytes - 1024) fprintf(stderr, "log: %llu bytes, at capacity: new lines are being dropped\n", total);
	} else if (!strcmp(cmd, "fbpublish") && argc == 2) {
		static const char *why[] = { "published", "not ready (no BAR0 or not observing)", "already published",
		                             "HUBP0 is not on the GOP scanout", "layout gate refused", "snapshot refused",
		                             "surface range is protected", "publish failed" };
		uint32_t got = 3;
		kr = n33(aFbPublish, NULL, 0, o, &got);
		if (kr != kIOReturnSuccess) { fprintf(stderr, "fbpublish: call failed 0x%x\n", kr); IOServiceClose(conn); return 1; }
		uint64_t st = o[0];
		printf("fbpublish: %s (status %llu)\n", st < 8 ? why[st] : "?", (unsigned long long)st);
		if (st == 4) printf("  gate verdict %llu at slot %llu (1 fam size, 2 our size, 3 inherited slot, 4 override, 5 terminator, 6 pin)\n", (unsigned long long)o[1], (unsigned long long)o[2]);
		if (st == 5) printf("  snapshot verdict %llu (1 bar0, 2 align, 3 range, 4 geom, 5 overlap)\n", (unsigned long long)o[1]);
		if (st == 6) printf("  refusal reason %llu\n", (unsigned long long)o[1]);
		rc = st == 0 ? 0 : 1;
	} else {
		usage(); rc = 2;
	}

	IOServiceClose(conn);
	return rc;
}
