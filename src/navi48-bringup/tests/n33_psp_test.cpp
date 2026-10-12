// n33_psp_test.cpp: host test of amd/n33_psp_pure.h.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_psp_test.cpp -o $SCRATCH/n33_psp_test && $SCRATCH/n33_psp_test
#include <cstdio>
#include "n33_psp_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void registers() {
	using namespace n33psp;
	CHECK(kC2pmsg35 == 0x16000 + 0x63);   // MP0 base 0x16000 plus mp_13_0_2_offset.h
	CHECK(kC2pmsg36 == 0x16000 + 0x64);
	CHECK(kC2pmsg58 == 0x16000 + 0x7A);
	CHECK(kC2pmsg64 == 0x16000 + 0x80);
	CHECK(kC2pmsg81 == 0x16000 + 0x91);
	CHECK(kMmhubFbBase == 0x1A000 + 0x86C);     // MMHUB segment 0 plus mmhub_3_0_2_offset.h
	CHECK(kMmhubFbTop == 0x1A000 + 0x86D);
	CHECK(kMmhubFbOffset == 0x1A000 + 0x857);
}

static void commands() {
	using namespace n33psp;
	CHECK(cmd_allowed(0x80000)); CHECK(cmd_allowed(0x10000000)); CHECK(cmd_allowed(0x10000)); CHECK(cmd_allowed(0x20000));
	CHECK(!cmd_allowed(0));
	CHECK(!cmd_allowed(0x30000));                         // DESTROY_RINGS is a ring command, not a bootloader one
	CHECK(!cmd_allowed(0x100000)); CHECK(!cmd_allowed(0x200000));   // DRAM training (design D8)
	CHECK(!cmd_allowed(0x80000000u)); CHECK(!cmd_allowed(0xFFFFFFFFu));
}

static void windows() {
	using namespace n33psp;
	const uint64_t fb = 0x8000000000ull, bar0 = 256ull << 20;
	uint64_t off = 0;
	CHECK(window_reg(fb, 0x8000000) == 0x80080);
	CHECK(window_from_reg(0x80080, fb, bar0, &off) && off == 0x8000000);
	CHECK(window_from_reg(window_reg(fb, 0xFF00000), fb, bar0, &off) && off == 0xFF00000);   // the last whole MiB
	CHECK(!window_from_reg(window_reg(fb, 0x10000000), fb, bar0, &off));                      // at the aperture end
	CHECK(!window_from_reg(0x80200, fb, bar0, &off));                                         // 512 MiB: past the aperture
	CHECK(!window_from_reg((uint32_t)(fb >> 20) - 1, fb, bar0, &off));                         // below the FB base
	CHECK(!window_from_reg(0, fb, bar0, &off));
	CHECK(!window_from_reg(0xFFFFFFFFu, fb, bar0, &off));                                      // no wraparound
	CHECK(!window_from_reg(0x80080, fb | 0x1000, bar0, &off));                                 // unaligned FB base
	CHECK(!window_from_reg(0x80080, fb, (1ull << 20) - 4, &off));                              // aperture smaller than a window
}

static void overlap_cases() {
	using namespace n33psp;
	const uint64_t surf = 0x2000000, sb = 14745600;                    // the stage 2 framebuffer surface
	CHECK(overlaps(0x2000000, kWinBytes, surf, sb));                   // on the surface
	CHECK(overlaps(0x2000000 - 0x80000, kWinBytes, surf, sb));         // straddles the start
	CHECK(overlaps(0x2000000 + sb - 4, kWinBytes, surf, sb));          // straddles the end
	CHECK(!overlaps(0x2000000 - kWinBytes, kWinBytes, surf, sb));      // ends exactly at the start
	CHECK(!overlaps(0x2000000 + sb, kWinBytes, surf, sb));             // begins exactly at the end
	CHECK(!overlaps(0x8000000, kWinBytes, surf, sb));                  // the default window
}

static void mmhub_cases() {
	using namespace n33psp;
	CHECK(mmhub_agrees(0x8000, 0x8000000000ull));                       // 16 MiB units: 0x8000000000 >> 24
	CHECK(mmhub_agrees(0xFF008000u, 0x8000000000ull));                 // only the low 24 bits count
	CHECK(!mmhub_agrees(0x8001, 0x8000000000ull));
	CHECK(!mmhub_agrees(0, 0x8000000000ull));
}

static void waits() {
	using namespace n33psp;
	CHECK(classify_wait(0x80000000u) == kIdle);
	CHECK(classify_wait(0xFFFFFFFFu) == kUnreadable);
	CHECK(classify_wait(0x80000002u) == kError);          // ready bit with an error code in 7:0
	CHECK(classify_wait(0) == kPending);
	CHECK(classify_wait(0x00000010u) == kPending);
	CHECK(classify_wait(0x80000100u) == kPending);        // not exactly ready, no code in 7:0
}

static void command_writes() {
	using namespace n33psp;
	const uint32_t w = 0x80080;                            // the validated window value
	CHECK(cmd_write_check(kBlLoadKdb, kReady, 0, w, w) == kWhyOk);
	CHECK(cmd_write_check(kBlLoadSos, kReady, 0, w, w) == kWhyOk);
	CHECK(cmd_write_check(0x30000, kReady, 0, w, w) == kWhyCmd);
	CHECK(cmd_write_check(kBlLoadKdb, kReady, 0x330024, w, w) == kWhySosAlive);
	CHECK(cmd_write_check(kBlLoadKdb, 0, 0, w, w) == kWhyNotReady);
	CHECK(cmd_write_check(kBlLoadKdb, 0x80000001u, 0, w, w) == kWhyNotReady);
	CHECK(cmd_write_check(kBlLoadKdb, kReady, 0, 0, 0) == kWhyNoWindow);          // no window validated
	CHECK(cmd_write_check(kBlLoadKdb, kReady, 0, 0x80081, w) == kWhyNoWindow);    // C2PMSG_36 changed behind the guard
	CHECK(cmd_write_check(0, 0, 0x330024, 0, 0) == kWhyCmd);                      // a bad command is named first
}

int main() {
	registers(); commands(); windows(); overlap_cases(); mmhub_cases(); waits(); command_writes();
	if (fails) { printf("n33_psp_test: %d failed\n", fails); return 1; }
	printf("n33_psp_test: all passed\n");
	return 0;
}
