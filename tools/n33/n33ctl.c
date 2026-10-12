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
// windows CW0-CW7, 9-11 are REGION4-REGION6. Names match the kext's comments.
static const char *kindName(uint64_t k) {
	static char b[16];
	if (k == 0) return "GOP scanout";
	if (k <= 8) { snprintf(b, sizeof b, "DMCUB CW%llu", k - 1); return b; }
	if (k <= 11) { snprintf(b, sizeof b, "DMCUB REGION%llu", k + 1); return b; }
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
	} else if (!strcmp(cmd, "fbpublish") && argc == 2) {
		static const char *why[] = { "published", "not ready (no BAR0 or not observing)", "already published",
		                             "HUBP0 is not on the GOP scanout", "layout gate refused", "snapshot refused",
		                             "surface range is protected", "publish failed" };
		uint32_t got = 3;
		kr = n33(aFbPublish, NULL, 0, o, &got);
		uint64_t st = o[0];
		printf("fbpublish: %s (status %llu)\n", st < 8 ? why[st] : "?", (unsigned long long)st);
		if (st == 4) printf("  gate verdict %llu at slot %llu (1 fam size, 2 our size, 3 inherited slot, 4 override, 5 terminator, 6 pin)\n", (unsigned long long)o[1], (unsigned long long)o[2]);
		if (st == 5) printf("  snapshot verdict %llu (1 bar0, 2 align, 3 range, 4 geom, 5 overlap)\n", (unsigned long long)o[1]);
		if (st == 6) printf("  refusal reason %llu\n", (unsigned long long)o[1]);
		rc = (kr == kIOReturnSuccess && st == 0) ? 0 : 1;
	} else {
		usage(); rc = 2;
	}

	IOServiceClose(conn);
	return rc;
}
