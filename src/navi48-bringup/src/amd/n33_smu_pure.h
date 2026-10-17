// n33_smu_pure.h: decisions for the SMU 13.0.7 driver mailbox (S1 to S3 of the overview repo's
// RDNA3/Tasks/navi33-cobalt64-port/stage3-smu-handshake-design.md, D3 to D6). Host-testable: no kernel
// types, no I/O. The kext runs Linux's protocol (smu_cmn.c:311-323, :459-530 in the pinned tree 3857c2fe)
// for one allowlisted message per call; n33ctl decides the ladder.
#pragma once
#include <stdint.h>

namespace n33smu {

// MP1 absolute dwords (MP1 segment 0 at 0x16000; mp_13_0_0_offset.h).
constexpr uint32_t kMsg  = 0x16282;   // C2PMSG_66: message index
constexpr uint32_t kArg  = 0x16292;   // C2PMSG_82: argument in, result out
constexpr uint32_t kResp = 0x1629A;   // C2PMSG_90: response

// Responses (smu_cmn.c:76-82).
constexpr uint32_t kRespNone = 0, kRespOk = 1, kRespFail = 0xFF, kRespUnknown = 0xFE,
                   kRespPrereq = 0xFD, kRespBusy = 0xFC;
inline bool known_resp(uint32_t r) {
	return r == kRespOk || r == kRespFail || r == kRespUnknown || r == kRespPrereq || r == kRespBusy;
}

// Messages (smu_v13_0_7_ppsmc.h). Only these are ever written; D4 refuses the rest.
constexpr uint32_t kGetSmuVersion = 0x2, kGetDriverIfVersion = 0x3, kSetAllowedMaskLow = 0x4,
                   kSetAllowedMaskHigh = 0x5, kEnableAllFeatures = 0x6, kGetRunningLow = 0xC,
                   kGetRunningHigh = 0xD, kSetDramAddrHigh = 0xE, kSetDramAddrLow = 0xF;
constexpr uint32_t kMaskHigh = 0x0001a3a4u, kMaskLow = 0x71fe7f7fu;   // D4, from Linux on this card

// D6: the driver table page, 16 KiB at VRAM 0x8600000 (MC = FB base + offset).
constexpr uint64_t kTableOff = 0x8600000;
constexpr uint64_t kTableLen = 0x4000;

// Refusal reasons. They continue the kext's list after milestone 2's (15..22).
enum Why : uint32_t {
	kWhyOk = 0,
	kWhyMsg = 23,      // message not on the allowlist
	kWhyArg = 24,      // argument is not the one D4 allows
	kWhyOrder = 25,    // out of the ladder's order, not armed, or sent twice
	kWhyHung = 26,     // PMFW marked hung (timeout or unknown response): reload the kext
	kWhyBusy = 27,     // C2PMSG_90 still 0 after 2 s before sending: nothing was written
	kWhyStopped = 28,  // an earlier state-changing message did not answer OK: the ladder is over
	kWhyPage = 29,     // driver table page not writable (protected range) or not owned
};

struct State {
	bool hung, stopped;
	uint8_t armed;            // 0, 2 (S2) or 3 (S3)
	bool maskHigh, maskLow;   // 0x5 and 0x4 answered OK
	uint32_t enables;         // 0x6 sent this many times (at most 2)
	uint32_t enableResp;      // the last 0x6 answer
	bool dramHigh, dramLow;
	bool pageOwned;           // the table page is ours and protected
};

inline bool query(uint32_t m) {
	return m == kGetSmuVersion || m == kGetDriverIfVersion || m == kGetRunningLow || m == kGetRunningHigh;
}

// May (msg, arg) be sent now? tableMc is the MC address of the driver table page. Checks run in the order
// allowlist, argument, state, so a wrong message or argument is refused for that reason whatever the state.
inline uint32_t may_send(const State &s, uint32_t msg, uint32_t arg, uint64_t tableMc) {
	uint32_t want;
	switch (msg) {
	case kGetSmuVersion: case kGetDriverIfVersion: case kGetRunningLow: case kGetRunningHigh:
	case kEnableAllFeatures: want = 0; break;
	case kSetAllowedMaskHigh: want = kMaskHigh; break;
	case kSetAllowedMaskLow: want = kMaskLow; break;
	case kSetDramAddrHigh: want = (uint32_t)(tableMc >> 32); break;
	case kSetDramAddrLow: want = (uint32_t)tableMc; break;
	default: return kWhyMsg;
	}
	if (arg != want) return kWhyArg;
	if (s.hung) return kWhyHung;
	if (query(msg)) return kWhyOk;
	if (s.stopped) return kWhyStopped;
	// S2 is the tinygrad AM order measured on this card (2026-10-17): driver table address, then the enable,
	// with no masks. S3, only after that enable answered 0xFD, sends the masks and the enable again.
	switch (msg) {
	case kSetDramAddrHigh:
		if (!s.pageOwned) return kWhyPage;
		return (s.armed == 2 && !s.dramHigh) ? kWhyOk : kWhyOrder;
	case kSetDramAddrLow:
		return (s.dramHigh && !s.dramLow) ? kWhyOk : kWhyOrder;
	case kEnableAllFeatures:
		if (s.enables == 0) return s.dramLow ? kWhyOk : kWhyOrder;
		return (s.enables == 1 && s.enableResp == kRespPrereq && s.maskLow) ? kWhyOk : kWhyOrder;
	case kSetAllowedMaskHigh:
		return (s.armed == 3 && !s.maskHigh) ? kWhyOk : kWhyOrder;
	case kSetAllowedMaskLow:
		return (s.maskHigh && !s.maskLow) ? kWhyOk : kWhyOrder;
	}
	return kWhyMsg;
}

// Arming is the visible go for a state-changing rung. S3 only after S2's enable answered 0xFD (D2).
inline uint32_t may_arm(const State &s, uint32_t level) {
	if (s.hung) return kWhyHung;
	if (s.stopped) return kWhyStopped;
	if (level == 2) return s.armed == 0 ? kWhyOk : kWhyOrder;
	if (level == 3) return (s.armed == 2 && s.enables == 1 && s.enableResp == kRespPrereq) ? kWhyOk : kWhyOrder;
	return kWhyArg;
}

// After a sent message (D5). A timeout or an unknown response marks PMFW hung. A known non-OK answer to a
// state-changing message stops the ladder, except 0xFD from the first enable, which is S3's trigger.
inline void record(State &s, uint32_t msg, bool answered, uint32_t resp) {
	if (!answered || !known_resp(resp)) { s.hung = true; return; }
	if (msg == kEnableAllFeatures) {
		s.enables++;
		s.enableResp = resp;
		if (resp != kRespOk && !(s.enables == 1 && resp == kRespPrereq)) s.stopped = true;
		return;
	}
	if (query(msg)) return;
	if (resp != kRespOk) { s.stopped = true; return; }
	switch (msg) {
	case kSetAllowedMaskHigh: s.maskHigh = true; break;
	case kSetAllowedMaskLow: s.maskLow = true; break;
	case kSetDramAddrHigh: s.dramHigh = true; break;
	case kSetDramAddrLow: s.dramLow = true; break;
	}
}

} // namespace n33smu
