// n33_fb_test.cpp: host test of amd/n33_fb_pure.h and source pins on N33Framebuffer.cpp.//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_fb_test.cpp -o $SCRATCH/n33_fb_test && $SCRATCH/n33_fb_test <FORK>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "n33_fb_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static n33fb::Snap good() {
	n33fb::Snap s = {};
	s.bar0Phys = 0x4000000000ull; s.bar0Size = 256ull << 20; s.surfOff = n33fb::kSurfOff;
	s.gopOff = 0; s.gopLen = 0xe10000;
	s.w = 2560; s.h = 1440; s.pitchBytes = 10240; s.surfCfg = 0;
	return s;
}

// B0: the iGPU-as-boot-display case, the card's GOP pipe at 1024x768 pitch 1024 px (measured 2026-10-17).
static n33fb::Snap good768() {
	n33fb::Snap s = {};
	s.bar0Phys = 0x4000000000ull; s.bar0Size = 256ull << 20; s.surfOff = n33fb::kSurfOff;
	s.gopOff = 0; s.gopLen = 0x300000;
	s.w = 1024; s.h = 768; s.pitchBytes = 4096; s.surfCfg = 0;
	return s;
}

static void snapshot_cases() {
	using namespace n33fb;
	CHECK(snap_valid(good()) == kSnapOk);
	CHECK(snap_valid(good768()) == kSnapOk);        // B0: 1024x768 now accepted, not refused as non-2560x1440
	Snap s = good(); s.bar0Phys = 0;               CHECK(snap_valid(s) == kSnapBar0);       // snap_bar0
	s = good(); s.bar0Phys = 0x4000001000ull;      CHECK(snap_valid(s) == kSnapBar0);       // not 64 KiB aligned
	s = good(); s.surfOff = 0x2001000;             CHECK(snap_valid(s) == kSnapAlign);
	s = good(); s.surfOff = 0x800000;              CHECK(snap_valid(s) == kSnapRange);      // below the 16 MiB floor
	s = good(); s.surfOff = (256ull << 20) - 0x10000; CHECK(snap_valid(s) == kSnapRange);    // runs past the aperture (snap_range)
	s = good(); s.surfOff = ~0ull & ~0xffffull;    CHECK(snap_valid(s) == kSnapRange);      // no wraparound
	s = good(); s.w = 1920; s.pitchBytes = 7680;   CHECK(snap_valid(s) == kSnapOk);         // B0: an in-bounds mode is valid
	s = good(); s.w = 200;                         CHECK(snap_valid(s) == kSnapGeom);       // width below kWmin
	s = good(); s.w = 8000;                        CHECK(snap_valid(s) == kSnapGeom);       // width above kWmax
	s = good(); s.h = 100;                         CHECK(snap_valid(s) == kSnapGeom);       // height below kHmin
	s = good(); s.h = 5000;                        CHECK(snap_valid(s) == kSnapGeom);       // height above kHmax
	s = good(); s.pitchBytes = 7680;               CHECK(snap_valid(s) == kSnapGeom);       // pitch below w*4 (2560*4)
	s = good(); s.pitchBytes = 10242;              CHECK(snap_valid(s) == kSnapGeom);       // stride not a multiple of 4
	s = good(); s.pitchBytes = 12288;              CHECK(snap_valid(s) == kSnapOk);         // a padded pitch (> w*4) is allowed
	s = good(); s.gopOff = 0x2000000; s.gopLen = 0x10000; CHECK(snap_valid(s) == kSnapOverlap);   // snap_overlap
	s = good(); s.gopOff = 0x2e00000; s.gopLen = 0x20000; CHECK(snap_valid(s) == kSnapOverlap);   // GOP inside the surface tail
	s = good(); s.gopOff = ~0ull; s.gopLen = 2;    CHECK(snap_valid(s) == kSnapOverlap);          // length overflow
	s = good(); s.surfCfg = 0xdeadbeef;            CHECK(snap_valid(s) == kSnapOk);              // logged, not judged
}

static void answer_cases() {
	using namespace n33fb;
	ModeInfo m; PixelInfo p; uint64_t phys = 1, len = 1;
	CHECK(mode_info(1, good(), &m) && m.w == 2560 && m.h == 1440 && m.refresh1616 == (75u << 16) && m.valid && m.isDefault);
	CHECK(mode_info(1, good768(), &m) && m.w == 1024 && m.h == 768);                              // B0: mode from the snapshot
	CHECK(!mode_info(2, good(), &m));
	CHECK(pixel_info(1, 0, good(), &p) && p.bytesPerRow == 10240 && p.w == 2560 && p.h == 1440 && p.bitsPerPixel == 32 && p.masks[0] == 0x00FF0000u && p.masks[2] == 0xFFu);
	CHECK(pixel_info(1, 0, good768(), &p) && p.bytesPerRow == 4096 && p.w == 1024 && p.h == 768); // B0: pitch/size from the snapshot
	CHECK(!pixel_info(1, 1, good(), &p));
	CHECK(!pixel_info(2, 0, good(), &p));
	CHECK(aperture(good(), &phys, &len) && phys == 0x4002000000ull && len == 14745600ull);
	CHECK(aperture(good768(), &phys, &len) && phys == 0x4002000000ull && len == 3145728ull);      // B0: 768 * 4096
	n33fb::Snap bad = good(); bad.bar0Phys = 0;
	CHECK(!aperture(bad, &phys, &len));                                                           // aperture_null_when_invalid
	CHECK(fb_attr(kAttrCursor).rc == kRcValue && fb_attr(kAttrCursor).value == 0);               // software cursor
	CHECK(fb_attr(0x12345678).rc == kRcSuper);
	CHECK(conn_attr(kConnEnable).value == 1 && conn_attr(kConnCheckEnable).value == 1);
	CHECK(conn_attr(kConnFlags).rc == kRcValue && conn_attr(kConnFlags).value == 0);
	CHECK(conn_attr(kConnHlDdc).rc == kRcUnsupported);                                            // no EDID in this design
	CHECK(conn_attr(0x12345678).rc == kRcSuper);
}

static void scanout_cases() {
	using namespace n33fb;
	CHECK(scanout_ours(true, 2, 1));                // enabled and on: our surface
	CHECK(scanout_ours(true, 1, 1));                // any non-zero power state counts as on
	CHECK(!scanout_ours(false, 2, 1));              // not enabled yet: the GOP's
	CHECK(!scanout_ours(true, 0, 1));               // framebuffer powered off
	CHECK(!scanout_ours(true, 2, 0));               // connection powered off
}

struct GateRig {
	std::vector<uintptr_t> fam, ours;
	std::vector<uint16_t> ovr = { 0, 1, 7, 184, 309 };
	std::vector<uint16_t> pinSlot = { 184 };
	std::vector<uintptr_t> pinAddr;
	n33fb::GateInput in = {};
	GateRig() : fam(n33fb::kFbSlots + 1, 0), ours(n33fb::kFbSlots + 1, 0) {
		for (uint32_t i = 0; i < n33fb::kFbSlots; i++) { fam[i] = 0x1000 + i * 16; ours[i] = fam[i]; }
		for (uint16_t o : ovr) ours[o] = 0x500000 + o * 16;     // inside our fake text window
		pinAddr = { fam[184] };
	}
	n33fb::GateResult run() {
		in.ours = ours.data(); in.fam = fam.data(); in.nslots = n33fb::kFbSlots;
		in.ovr = ovr.data(); in.novr = (uint32_t)ovr.size();
		in.pinSlot = pinSlot.data(); in.pinAddr = pinAddr.data(); in.npins = (uint32_t)pinSlot.size();
		in.textLo = 0x500000; in.textHi = 0x600000;
		in.famSize = n33fb::kFbClassSize; in.wantFamSize = n33fb::kFbClassSize; in.ourSize = 0x200;
		return n33fb::gate_run(in);
	}
};

static void gate_cases() {
	using namespace n33fb;
	{ GateRig g; auto r = g.run(); CHECK(r.verdict == kGateOk); }
	{ GateRig g; g.run(); g.in.famSize = 0x1e0; auto r = gate_run(g.in); CHECK(r.verdict == kGateFamSize); }
	{ GateRig g; g.run(); g.in.ourSize = 0x100; auto r = gate_run(g.in); CHECK(r.verdict == kGateOurSize); }
	{ GateRig g; g.ours[100] ^= 0x10; auto r = g.run(); CHECK(r.verdict == kGateInherited && r.slot == 100); }   // a slot moved
	{ GateRig g; g.ours[184] = 0x900000; auto r = g.run(); CHECK(r.verdict == kGateOverride && r.slot == 184); } // override outside our image
	{ GateRig g; g.fam[kFbSlots] = 0x7777; auto r = g.run(); CHECK(r.verdict == kGateTerm && r.slot == kFbSlots); } // gate_term: family has a 351st slot
	{ GateRig g; g.pinAddr[0] = 0xdead; auto r = g.run(); CHECK(r.verdict == kGatePin && r.slot == 184); }        // gate_pin
	{ GateRig g; g.pinSlot[0] = 400; auto r = g.run(); CHECK(r.verdict == kGatePin); }                             // pin slot out of range
	{ GateRig g; g.fam[50] = 0; g.ours[50] = 0; auto r = g.run(); CHECK(r.verdict == kGateInherited && r.slot == 50); } // a null family slot
}

static void source_pins(const std::string &fork) {
	std::ifstream f(fork + "/src/navi48-bringup/src/N33Framebuffer.cpp");
	CHECK(f.good());
	std::stringstream ss; ss << f.rdbuf();
	const std::string src = ss.str();
	// the framebuffer touches no register itself: no mapped-register access, no HUBP0 names, no n33Call;
	// the self-flip goes through n33_fb_scanout (the bringup kext's guard) and nothing else
	CHECK(src.find("rmmio") == std::string::npos);
	CHECK(src.find("kN33Hubp0") == std::string::npos);
	CHECK(src.find("n33Call") == std::string::npos);
	CHECK(src.find("n33ScanoutTo") == std::string::npos);
	CHECK(src.find("n33_fb_gate") != std::string::npos);
	CHECK(src.find("n33_fb_scanout(want)") != std::string::npos);
	CHECK(src.find("n33fb::scanout_ours(") != std::string::npos);
}

int main(int argc, char **argv) {
	snapshot_cases(); answer_cases(); scanout_cases(); gate_cases();
	if (argc > 1) source_pins(argv[1]); else { printf("FAIL: pass FORK as argv[1]\n"); fails++; }
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("n33_fb_test: all passed\n");
	return 0;
}
