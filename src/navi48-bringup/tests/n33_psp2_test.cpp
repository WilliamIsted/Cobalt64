// n33_psp2_test.cpp: host test of amd/n33_psp2_pure.h (PSP 13.0.7 milestone 2).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_psp2_test.cpp -o $SCRATCH/n33_psp2_test && $SCRATCH/n33_psp2_test
#include <cstdio>
#include "n33_psp2_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

using namespace n33psp2;

static State ringOnly() { State s = {}; s.ring = true; return s; }

static void order_cases() {
	State s = {};
	CHECK(may_send(s, kToc, 0, 0, 2304) == kWhyNoRing);
	s = ringOnly();
	CHECK(may_send(s, kIpFw, kFwSmu, kFwBuf, 0x40000) == kWhyOrder);       // SMU before LOAD_TOC
	CHECK(may_send(s, kTmr, 0, 0, 0) == kWhyOrder);                          // SETUP_TMR before LOAD_TOC
	CHECK(may_send(s, kIpFw, 71, kFwBuf, 0x4400) == kWhyOrder);              // GC before the TMR
	CHECK(may_send(s, kAutoload, 0, 0, 0) == kWhyOrder);
	CHECK(may_send(s, kToc, 0, 0, 0) == kWhyRange);
	CHECK(may_send(s, kToc, 0, 0, 2302) == kWhyRange);                       // not a dword multiple
	CHECK(may_send(s, kToc, 0, 0, (1u << 20) + 4) == kWhyRange);
	CHECK(may_send(s, kToc, 0, 0, 2304) == kWhyOk);
	record(s, kToc, 0, true, 0, 0x1300000);
	CHECK(s.toc && s.tmrAnswer == 0x1300000);
	CHECK(may_send(s, kToc, 0, 0, 2304) == kWhyOrder);                       // once
	CHECK(may_send(s, kTmr, 0, 0, 0) == kWhyOrder);                          // SMU first (D7)
	CHECK(may_send(s, kIpFw, kFwSmu, kFwBuf, 0x3FE00) == kWhyOk);
	CHECK(may_send(s, kIpFw, kFwSmu, kFwBuf + 0x10, 0x3FE00) == kWhyRange);  // not page aligned
	CHECK(may_send(s, kIpFw, kFwSmu, kFwBuf - kPage, 0x1000) == kWhyRange);
	CHECK(may_send(s, kIpFw, kFwSmu, kFwBuf + kFwBufSize - kPage, 0x2000) == kWhyRange);   // runs past fw_buf
	CHECK(may_send(s, kIpFw, kFwSmu, ~0ull & ~0xFFFull, 0x1000) == kWhyRange);             // no wraparound
	record(s, kIpFw, kFwSmu, true, 0, 0);
	CHECK(s.smu && !s.rlcG);
	CHECK(may_send(s, kIpFw, kFwSmu, kFwBuf, 0x3FE00) == kWhyOrder);         // SMU once, before the TMR
	CHECK(may_send(s, kTmr, 0, 0, 0) == kWhyOk);
	record(s, kTmr, 0, true, 0, 0);
	CHECK(may_send(s, kTmr, 0, 0, 0) == kWhyOrder);
	CHECK(may_send(s, kIpFw, 13, kFwBuf, 0x1000) == kWhyType);               // VCN
	CHECK(may_send(s, kIpFw, 51, kFwBuf, 0x1000) == kWhyType);               // DMUB: never (live under the GOP)
	CHECK(may_send(s, kIpFw, 73, kFwBuf, 0x1000) == kWhyType);               // PPTABLE
	CHECK(may_send(s, kAutoload, 0, 0, 0) == kWhyOrder);                     // before RLC_G
	uint64_t off = kFwBuf;
	for (uint32_t t : kGcOrder) {
		CHECK(may_send(s, kIpFw, t, off, 0x1000) == kWhyOk);
		record(s, kIpFw, t, true, 0, 0);
		CHECK(may_send(s, kIpFw, t, off, 0x1000) == kWhyOrder);              // each type once
		off += kPage;
	}
	CHECK(s.rlcG);
	CHECK(may_send(s, kAutoload, 0, 0, 0) == kWhyOk);
	record(s, kAutoload, 0, true, 0, 0);
	CHECK(s.autoload && !s.failed);
	CHECK(may_send(s, kAutoload, 0, 0, 0) == kWhyOrder);
}

static void failure_cases() {
	State s = ringOnly();
	record(s, kToc, 0, true, 0x115, 0);                                   // non-zero status latches
	CHECK(s.failed && !s.toc);
	CHECK(may_send(s, kToc, 0, 0, 2304) == kWhyFailed);
	s = ringOnly();
	record(s, kToc, 0, false, 0, 0);                                      // a timeout latches too
	CHECK(may_send(s, kToc, 0, 0, 2304) == kWhyFailed);
}

static void tmr_cases() {
	CHECK(tmr_bytes(0x1300000) == 0x1300000);
	CHECK(tmr_bytes(0x1300001) == 0x1301000);                             // rounded up, not refused
	CHECK(tmr_bytes(0) == 0);
	CHECK(tmr_bytes(32u << 20) == (32ull << 20));
	CHECK(tmr_bytes((32u << 20) + 1) == 0);                               // above 32 MiB: new placement first
	CHECK(tmr_bytes(0xFFFFFFFFu) == 0);
	CHECK(kTmrOff + kTmrMax <= 0x3FEC00000ull);                           // clear of the VBIOS 4 MiB reservation
	State s = ringOnly(); s.toc = true; s.smu = true; s.tmrAnswer = 0;
	CHECK(may_send(s, kTmr, 0, 0, 0) == kWhyTmrSize);
	s.tmrAnswer = 0x2100000;
	CHECK(may_send(s, kTmr, 0, 0, 0) == kWhyTmrSize);
}

static void ring_cases() {
	CHECK(frame_slot(0) == 0);
	CHECK(frame_slot(16) == 1);
	CHECK(frame_slot(1008) == 63);
	CHECK(frame_slot(1024) == 0);                                         // wrap counts as slot 0, as Linux
	CHECK(frame_slot(8) == -1);
	CHECK(frame_slot(2048 + 16) == -1);
	CHECK(next_wptr(0) == 16);
	CHECK(next_wptr(1008) == 0);
	CHECK(tos_ok(0x80000000u));
	CHECK(!tos_ok(0x80000115u));                                          // a failure status in the low word
	CHECK(tos_ok(0x80010000u));                                           // bits outside the mask are ignored
	CHECK(!tos_ok(0));
	CHECK(kRing + kPspPages == kFence + kPage);
	CHECK(kFwPri + kFwPriSize <= kRing && kFence + kPage <= kFwBuf);
	CHECK(kFwBuf + kFwBufSize <= (256ull << 20));                         // inside BAR0
}

static void layout_cases() {
	uint32_t f[kFrameDw];
	build_frame(f, 0x8008101000ull, 0x8008102000ull, 7);
	CHECK(f[0] == 0x08101000 && f[1] == 0x80 && f[2] == 0);              // cmd_buf_size stays 0
	CHECK(f[3] == 0x08102000 && f[4] == 0x80 && f[5] == 7);
	for (uint32_t i = 6; i < kFrameDw; i++) CHECK(f[i] == 0);
	uint32_t c[kCmdDw];
	build_cmd(c, kToc, 0, 0x8008000000ull, 2304, 0);
	CHECK(c[0] == 0 && c[1] == 0 && c[2] == 0x20);                        // buf_size, buf_version 0; cmd_id at +8
	CHECK(c[7] == 0x08000000 && c[8] == 0x80 && c[9] == 2304);           // body at +28
	build_cmd(c, kIpFw, 89, 0x8008200000ull, 0x41300, 0);
	CHECK(c[2] == 6 && c[7] == 0x08200000 && c[8] == 0x80 && c[9] == 0x41300 && c[10] == 89);
	build_cmd(c, kTmr, 0, 0x83FC000000ull, 0x1300000, 0x3FC000000ull);
	CHECK(c[2] == 5 && c[7] == 0xFC000000u && c[8] == 0x83 && c[9] == 0x1300000 && c[10] == 2);
	CHECK(c[11] == 0xFC000000u && c[12] == 0x3);                          // system_phy_addr = VRAM offset
	build_cmd(c, kAutoload, 0, 0, 0, 0);
	CHECK(c[2] == 0x21 && c[7] == 0);
	CHECK(c[kRespStatus / 4] == 0);
	CHECK(kRespTmrSize == 864 + 16 && kRespFwAddrLo == 864 + 8);
}

int main() {
	order_cases(); failure_cases(); tmr_cases(); ring_cases(); layout_cases();
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("n33_psp2_test: all passed\n");
	return 0;
}
