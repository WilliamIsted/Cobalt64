// n33_smu_test.cpp: host test of amd/n33_smu_pure.h (SMU 13.0.7 driver mailbox, S1 to S3; S2 in AM's order).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_smu_test.cpp -o $SCRATCH/n33_smu_test && $SCRATCH/n33_smu_test
#include <cstdio>
#include "n33_smu_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

using namespace n33smu;

static constexpr uint64_t kMc = 0x8008600000ull;   // FB base 0x8000000000 + kTableOff

// Run S2 (AM's order: driver table address, then the enable) up to and including the enable, which answers resp.
static State s2(uint32_t resp) {
	State s = {};
	CHECK(may_arm(s, 2) == kWhyOk); s.armed = 2; s.pageOwned = true;          // the arm action owns the page
	CHECK(may_send(s, kSetDramAddrHigh, 0x80, kMc) == kWhyOk);       record(s, kSetDramAddrHigh, true, kRespOk);
	CHECK(may_send(s, kSetDramAddrLow, 0x08600000, kMc) == kWhyOk);  record(s, kSetDramAddrLow, true, kRespOk);
	CHECK(may_send(s, kEnableAllFeatures, 0, kMc) == kWhyOk);        record(s, kEnableAllFeatures, true, resp);
	return s;
}

static void allowlist_cases() {
	State s = {};
	static const uint32_t refused[] = { 0x7, 0x8, 0x12, 0x13, 0x15, 0x17, 0x19, 0x20, 0x29, 0x2E, 0x2F, 0x32, 0x1, 0x14, 0x10 };
	for (uint32_t m : refused) CHECK(may_send(s, m, 0, kMc) == kWhyMsg);
	CHECK(may_send(s, kGetSmuVersion, 0, kMc) == kWhyOk);
	CHECK(may_send(s, kGetRunningHigh, 0, kMc) == kWhyOk);
	CHECK(may_send(s, kGetSmuVersion, 1, kMc) == kWhyArg);                       // queries take 0
	CHECK(may_send(s, kSetAllowedMaskLow, 0x12345678, kMc) == kWhyArg);           // a different mask
	CHECK(may_send(s, kSetAllowedMaskHigh, kMaskLow, kMc) == kWhyArg);            // masks swapped
	CHECK(may_send(s, kSetDramAddrLow, 0x00000000, kMc) == kWhyArg);              // the GOP scanout's address
	CHECK(may_send(s, kSetDramAddrHigh, 0x80, kMc) == kWhyPage);                  // right value, page not ours
	CHECK(may_send(s, kEnableAllFeatures, 0, kMc) == kWhyOrder);                  // not armed
	CHECK(may_send(s, kSetAllowedMaskHigh, kMaskHigh, kMc) == kWhyOrder);         // not armed
}

static void s2_cases() {
	State s = {};
	CHECK(may_arm(s, 3) == kWhyOrder);                                            // S3 before S2
	CHECK(may_arm(s, 4) == kWhyArg);
	s.armed = 2;
	CHECK(may_arm(s, 2) == kWhyOrder);                                            // once
	CHECK(may_send(s, kSetDramAddrHigh, 0x80, kMc) == kWhyPage);                  // armed, page not yet owned
	s.pageOwned = true;
	CHECK(may_send(s, kSetDramAddrLow, 0x08600000, kMc) == kWhyOrder);            // High first
	CHECK(may_send(s, kEnableAllFeatures, 0, kMc) == kWhyOrder);                  // not before the address
	CHECK(may_send(s, kSetAllowedMaskHigh, kMaskHigh, kMc) == kWhyOrder);         // masks are S3's
	CHECK(may_send(s, kSetDramAddrHigh, 0x80, kMc) == kWhyOk);       record(s, kSetDramAddrHigh, true, kRespOk);
	CHECK(may_send(s, kSetDramAddrHigh, 0x80, kMc) == kWhyOrder);                 // once
	s = s2(kRespOk);
	CHECK(s.enables == 1 && !s.stopped && !s.hung);
	CHECK(may_send(s, kEnableAllFeatures, 0, kMc) == kWhyOrder);                  // at most once after OK
	CHECK(may_send(s, kSetAllowedMaskHigh, kMaskHigh, kMc) == kWhyOrder);
	CHECK(may_arm(s, 3) == kWhyOrder);                                            // no S3 after an OK enable
	CHECK(may_send(s, kGetRunningLow, 0, kMc) == kWhyOk);                         // queries stay allowed
}

static void s3_cases() {
	State s = s2(kRespPrereq);
	CHECK(!s.stopped && s.enableResp == kRespPrereq);
	CHECK(may_send(s, kEnableAllFeatures, 0, kMc) == kWhyOrder);                  // not before S3
	CHECK(may_send(s, kSetAllowedMaskHigh, kMaskHigh, kMc) == kWhyOrder);         // not before S3's arm
	CHECK(may_arm(s, 3) == kWhyOk);
	s.armed = 3;
	CHECK(may_send(s, kSetDramAddrHigh, 0x80, kMc) == kWhyOrder);                 // S2's address is not re-sent
	CHECK(may_send(s, kSetAllowedMaskLow, kMaskLow, kMc) == kWhyOrder);           // High first
	CHECK(may_send(s, kSetAllowedMaskHigh, kMaskHigh, kMc) == kWhyOk); record(s, kSetAllowedMaskHigh, true, kRespOk);
	CHECK(may_send(s, kSetAllowedMaskHigh, kMaskHigh, kMc) == kWhyOrder);
	CHECK(may_send(s, kEnableAllFeatures, 0, kMc) == kWhyOrder);                  // not before both masks
	CHECK(may_send(s, kSetAllowedMaskLow, kMaskLow, kMc) == kWhyOk);   record(s, kSetAllowedMaskLow, true, kRespOk);
	CHECK(may_send(s, kEnableAllFeatures, 0, kMc) == kWhyOk);                     // the re-sent enable
	record(s, kEnableAllFeatures, true, kRespPrereq);
	CHECK(s.stopped && s.enables == 2);                                           // a second 0xFD ends the ladder
	CHECK(may_send(s, kEnableAllFeatures, 0, kMc) == kWhyStopped);
	CHECK(may_send(s, kGetSmuVersion, 0, kMc) == kWhyOk);
}

static void failure_cases() {
	State s = {};
	record(s, kGetSmuVersion, false, 0);                                          // timeout
	CHECK(s.hung);
	CHECK(may_send(s, kGetSmuVersion, 0, kMc) == kWhyHung);
	CHECK(may_arm(s, 2) == kWhyHung);
	CHECK(may_send(s, 0x2F, 0, kMc) == kWhyMsg);                                  // the message reason still wins
	s = {};
	record(s, kGetDriverIfVersion, true, 0xFB);                                   // not one of the five
	CHECK(s.hung);
	s = {};
	record(s, kGetSmuVersion, true, kRespFail);                                   // a query failing does not stop the ladder
	CHECK(!s.hung && !s.stopped);
	s = {}; s.armed = 2; s.pageOwned = true;
	record(s, kSetDramAddrHigh, true, kRespBusy);                                 // 0xFC is not retried
	CHECK(s.stopped && !s.dramHigh);
	CHECK(may_send(s, kSetDramAddrHigh, 0x80, kMc) == kWhyStopped);
	CHECK(may_arm(s, 3) == kWhyStopped);
	s = {}; s.armed = 2; s.pageOwned = true;
	record(s, kSetAllowedMaskHigh, true, kRespPrereq);                            // attempt 2's answer, now to an S3 rung
	CHECK(s.stopped && !s.maskHigh);
	s = s2(kRespFail);                                                            // enable 0xFF: no S3
	CHECK(s.stopped);
	CHECK(may_arm(s, 3) == kWhyStopped);
	s = s2(kRespPrereq); s.armed = 3;
	record(s, kSetAllowedMaskHigh, true, kRespUnknown);                           // 0xFE ends S3 too
	CHECK(s.stopped && !s.maskHigh);
	CHECK(known_resp(kRespOk) && known_resp(kRespBusy) && !known_resp(0) && !known_resp(0xFB));
}

static_assert(kMsg == 0x16000 + 0x282 && kArg == 0x16000 + 0x292 && kResp == 0x16000 + 0x29A, "MP1 mailbox");
static_assert(kTableOff % 0x100000 == 0 && kTableLen == 16 * 1024, "table page");
static_assert(kTableOff >= 0x8500000 && kTableOff + kTableLen <= 0x8700000, "the SMU window, above fw_buf");
static_assert((kMc >> 32) == 0x80 && (kMc & 0xFFFFFFFFu) == 0x08600000u, "table MC");

int main() {
	allowlist_cases(); s2_cases(); s3_cases(); failure_cases();
	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("n33_smu_test: all passed\n");
	return 0;
}
