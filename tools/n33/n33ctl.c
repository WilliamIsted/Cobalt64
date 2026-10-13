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

enum { kSelRegRead = 7, kSelN33 = 0x4e33 };
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
// page (S3). Names match the kext's comments.
static const char *kindName(uint64_t k) {
	static char b[16];
	if (k == 0) return "GOP scanout";
	if (k <= 8) { snprintf(b, sizeof b, "DMCUB CW%llu", k - 1); return b; }
	if (k <= 11) { snprintf(b, sizeof b, "DMCUB REGION%llu", k + 1); return b; }
	if (k == 12) return "PSP ring pages";
	if (k == 13) return "SMU driver table";
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
	case 29:  return "smu: driver table page not writable, or not taken by S3's arm";
	default:
		if (why >= 100) { static char b[48]; snprintf(b, sizeof b, "overlaps protected %s", kindName(why - 100)); return b; }
		return "?";
	}
}

static int call(uint64_t action, const uint64_t *args, uint32_t n, uint64_t *out, uint32_t outn) {
	uint32_t got = outn;
	kern_return_t kr = n33(action, args, n, out, &got);
	if (kr == kIOReturnNotPermitted) {
		// IOKit copies no scalar output back on an error return, so out[0] is not the kext's reason.
		fprintf(stderr, "refused; the kext logs the reason: "
		        "log show --last 2m --predicate 'process == \"kernel\"' | grep 'n33: refused', then n33ctl why <code>\n");
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
	    "  smu check | s1 | s2 | s3 | enable | state      SMU 13.0.7 mailbox (check is read-only; s1 sends queries)\n"
	    "                               smu send <msg> <arg>: raw, for smu-negative.sh only\n"
	    "  why <code>                   explain a refusal code from the kernel log (no kext needed)\n"
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
// n33ctl smu s2        milestone 2d, only after a 2c that did not complete: arm, allowed masks, EnableAllSmuFeatures
// n33ctl smu s3        only if S2's enable answered 0xFD: arm, driver table address, TransferTableSmu2Dram
// n33ctl smu enable    the re-sent EnableAllSmuFeatures after S3 (a separate go)
// The kext runs each message with Linux's protocol and refuses anything off the D4 allowlist; this tool
// keeps the order and the stop points. Any answer other than 0x1 ends the run.
enum { aSmuSend = 12, aSmuArm = 13, aSmuState = 14 };
enum { mGetSmuVersion = 0x2, mGetDriverIfVersion = 0x3, mMaskLow = 0x4, mMaskHigh = 0x5, mEnableAll = 0x6,
       mRunningLow = 0xC, mRunningHigh = 0xD, mDramHigh = 0xE, mDramLow = 0xF, mSmu2Dram = 0x12 };
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
	       "xfer %llu page %llu; C2PMSG_90 %#llx _66 %#llx _82 %#llx\n", st[0] >> 8 & 0xFF, st[0] & 1, st[0] >> 1 & 1, st[0] >> 2 & 1,
	       st[0] >> 3 & 1, st[1], st[2], st[0] >> 4 & 1, st[0] >> 5 & 1, st[0] >> 6 & 1, st[0] >> 7 & 1, st[3], st[4], st[5]);
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
		uint64_t lv = 2, o[1] = {0};
		if (call(aSmuArm, &lv, 1, o, 1)) { printf("arm S2 REFUSED\n"); return 1; }
		printf("armed S2. Watch the monitor.\n");
		uint32_t resp = 0;
		if (smuMsg(mMaskHigh, kSmuMaskHigh, "SetAllowedFeaturesMaskHigh", NULL, NULL) ||
		    smuMsg(mMaskLow, kSmuMaskLow, "SetAllowedFeaturesMaskLow", NULL, NULL)) return 1;
		const int bad = smuMsg(mEnableAll, 0, "EnableAllSmuFeatures", NULL, &resp);
		if (bad && resp == 0xFD) { printf("EnableAllSmuFeatures answered 0xFD (bad prerequisite): S3 is next, a separate go\n"); return 1; }
		if (bad) return 1;
		smuFeatures(&mask);
		return !smuAutoloadDone();
	}
	if (!strcmp(sub, "s3") && argc == 3) {
		if (st[1] != 1 || st[2] != 0xFD) { printf("refusing: S3 runs only after S2's enable answered 0xFD\n"); smuPrintState(st); return 1; }
		const uint64_t mc = s[1] + kSmuTableOff;
		uint64_t lv = 3, o[1] = {0};
		if (call(aSmuArm, &lv, 1, o, 1)) { printf("arm S3 REFUSED (driver table page not writable?)\n"); return 1; }
		printf("armed S3: driver table page VRAM %#llx (MC %#llx) zeroed and protected\n", kSmuTableOff, mc);
		if (smuMsg(mDramHigh, (uint32_t)(mc >> 32), "SetDriverDramAddrHigh", NULL, NULL) ||
		    smuMsg(mDramLow, (uint32_t)mc, "SetDriverDramAddrLow", NULL, NULL) ||
		    smuMsg(mSmu2Dram, 1, "TransferTableSmu2Dram COMBO", NULL, NULL)) return 1;
		uint32_t nz = 0;
		for (uint64_t off = 0; off < 0x4000; off += 4) {
			uint64_t a = kSmuTableOff + off, v = 0;
			if (call(aBar0Read, &a, 1, &v, 1)) return 1;
			if (v) nz++;
			if (off < 32) printf("table +%#06llx %#010llx\n", off, v);
		}
		printf("S3 done: %u of 4096 dwords non-zero. The re-sent enable is `smu enable`, a separate go.\n", nz);
		return nz ? 0 : 1;
	}
	if (!strcmp(sub, "enable") && argc == 3) {
		if (!(st[0] >> 6 & 1) || st[1] != 1) { printf("refusing: needs a completed S3 and exactly one earlier enable\n"); smuPrintState(st); return 1; }
		uint32_t resp = 0; uint64_t mask = 0;
		if (smuMsg(mEnableAll, 0, "EnableAllSmuFeatures (again)", NULL, &resp)) return 1;
		smuFeatures(&mask);
		return !smuAutoloadDone();
	}
	usage();
	return 2;
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
