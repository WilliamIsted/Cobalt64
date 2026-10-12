// n33_psp_pure.h: decisions for the PSP 13.0.7 bootloader chain (stage 3 step 1). Host-testable:
// no kernel types, no I/O. The kext applies these to the two PSP registers it lets a user client write.
#pragma once
#include <stdint.h>

namespace n33psp {

// MP0 absolute dwords. MP0 segment 0 is at 0x16000 on this card (card-ip-discovery.csv); the offsets
// are mp_13_0_2_offset.h's (psp-smu-firmware.md): _35 0x63, _36 0x64, _58 0x7A, _64 0x80, _81 0x91.
constexpr uint32_t kC2pmsg35 = 0x16063;
constexpr uint32_t kC2pmsg36 = 0x16064;
constexpr uint32_t kC2pmsg58 = 0x1607A;
constexpr uint32_t kC2pmsg64 = 0x16080;
constexpr uint32_t kC2pmsg81 = 0x16091;

// MMHUB 3.0.2 segment 0 is at 0x1a000 on this card; offsets from mmhub_3_0_2_offset.h (base idx 0).
constexpr uint32_t kMmhubFbOffset = 0x1A857;   // regMMMC_VM_FB_OFFSET       0x0857
constexpr uint32_t kMmhubFbBase   = 0x1A86C;   // regMMMC_VM_FB_LOCATION_BASE 0x086c
constexpr uint32_t kMmhubFbTop    = 0x1A86D;   // regMMMC_VM_FB_LOCATION_TOP  0x086d

// Bootloader commands (psp_v13_0.c, enum psp_bootloader_cmd).
constexpr uint32_t kBlLoadSysDrv = 0x00010000;
constexpr uint32_t kBlLoadSos    = 0x00020000;
constexpr uint32_t kBlLoadKdb    = 0x00080000;
constexpr uint32_t kBlLoadSpl    = 0x10000000;

constexpr uint32_t kReady    = 0x80000000u;   // C2PMSG_35 exactly this: bootloader idle and ready
constexpr uint64_t kWinBytes = 1ull << 20;    // PSP_1_MEG: the fw_pri window

// Refusal reasons. They continue the kext's list (0..8 and 100+kind are taken).
enum Why : uint32_t { kWhyOk = 0, kWhyCmd = 9, kWhyWindow = 10, kWhyNotReady = 11, kWhyNoWindow = 12, kWhySosAlive = 13, kWhySurface = 14 };

inline bool cmd_allowed(uint32_t v) {
	return v == kBlLoadKdb || v == kBlLoadSpl || v == kBlLoadSysDrv || v == kBlLoadSos;
}

// C2PMSG_36 carries the window's MC address >> 20.
inline uint32_t window_reg(uint64_t fbBase, uint64_t off) { return (uint32_t)((fbBase + off) >> 20); }

// The VRAM offset a C2PMSG_36 value names, or false when it is not a whole 1 MiB window inside BAR0.
inline bool window_from_reg(uint32_t v, uint64_t fbBase, uint64_t bar0Size, uint64_t *off) {
	if (fbBase & (kWinBytes - 1)) return false;
	const uint64_t mc = (uint64_t)v << 20;
	if (mc < fbBase) return false;
	const uint64_t o = mc - fbBase;
	if (o > bar0Size || bar0Size - o < kWinBytes) return false;
	*off = o;
	return true;
}

// Do two ranges overlap? Callers keep both inside BAR0 (window_from_reg and the stage 2 snapshot
// guarantee it), so nothing wraps.
inline bool overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
	return a < b + blen && b < a + alen;
}

// The MMHUB FB_LOCATION_BASE register (24 bits, 16 MiB units) names the same base the DCN register gave.
inline bool mmhub_agrees(uint32_t mmhubBaseReg, uint64_t fbBase) {
	return ((uint64_t)(mmhubBaseReg & 0x00FFFFFFu) << 24) == fbBase;
}

enum Wait : uint32_t { kPending = 0, kIdle = 1, kError = 2, kUnreadable = 3 };

// psp_v13_0_wait_for_bootloader: exactly 0x80000000 is ready; bit 31 with a non-zero low byte is an
// error code; 0xffffffff is a card that does not answer.
inline Wait classify_wait(uint32_t v) {
	if (v == 0xFFFFFFFFu) return kUnreadable;
	if (v == kReady) return kIdle;
	if ((v & kReady) && (v & 0xFFu)) return kError;
	return kPending;
}

// May a write of `val` to C2PMSG_35 be accepted now? c35, c81 and c36 are the registers' current
// values; winReg is the C2PMSG_36 value the kext validated (0 = none). The live C2PMSG_36 must still
// equal it, so a window changed behind the guard is not used.
inline uint32_t cmd_write_check(uint32_t val, uint32_t c35, uint32_t c81, uint32_t c36, uint32_t winReg) {
	if (!cmd_allowed(val)) return kWhyCmd;
	if (c81 != 0) return kWhySosAlive;
	if (c35 != kReady) return kWhyNotReady;
	if (winReg == 0 || c36 != winReg) return kWhyNoWindow;
	return kWhyOk;
}

} // namespace n33psp
