// n33ctl: userspace side of the N33 stage 2a call (Navi 33 observe build of Navi48Bringup).
// Build: clang -O2 -Wall -framework IOKit -framework CoreFoundation n33ctl.c -o n33ctl
// Run as root. Every write is checked again in the kext (allowlist, protected VRAM ranges).
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
	default:
		if (why >= 100) { static char b[48]; snprintf(b, sizeof b, "overlaps protected %s", kindName(why - 100)); return b; }
		return "?";
	}
}

static int call(uint64_t action, const uint64_t *args, uint32_t n, uint64_t *out, uint32_t outn) {
	uint32_t got = outn;
	kern_return_t kr = n33(action, args, n, out, &got);
	if (kr == kIOReturnNotPermitted) {
		fprintf(stderr, "refused: %s (why %llu)\n", whyName(out[0]), (unsigned long long)out[0]);
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
	    "all offsets/dwords accept 0x.. ; writes are re-checked in the kext\n");
}

int main(int argc, char **argv) {
	if (argc < 2) { usage(); return 2; }

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
