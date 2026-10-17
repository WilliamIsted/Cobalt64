//
//  n33_fb_pure.h - every decision of the N33 framebuffer (stage 2 step 3) that needs no kernel header.
//  Host test: tests/n33_fb_test.cpp. Kernel glue: N33Framebuffer.cpp. Design:
//  RDNA3/Tasks/navi33-cobalt64-port/stage2-framebuffer-design.md in the overview repo.
//
#pragma once
#include <stdint.h>

namespace n33fb {

constexpr uint32_t kModeId      = 1;
constexpr uint32_t kW           = 2560;
constexpr uint32_t kH           = 1440;
constexpr uint32_t kPitchBytes  = 10240;              // 2560 * 4
constexpr uint64_t kBytes       = 14745600ull;        // 2560 * 1440 * 4: the reserved surface window at kSurfOff (the
                                                      // largest mode we honour); the self-flip and publish write-allow
                                                      // guard this whole window, a smaller mode draws into its front
constexpr uint32_t kRefresh1616 = 75u << 16;          // reported for every mode; cosmetic for a fixed mode (design 3)
constexpr uint64_t kSurfOff     = 0x2000000ull;       // 32 MiB: above the kext's 16 MiB write floor
constexpr uint64_t kWriteFloor  = 16ull << 20;
constexpr uint64_t kAlign       = 0x10000ull;         // 64 KiB
// B0: the snapshot mode is taken from the live GOP/OTG timing, not fixed at 2560x1440. Accept any sane
// active size (the iGPU-boot case is 1024x768); the surface still must fit BAR0 above the write floor.
constexpr uint32_t kWmin = 320, kWmax = 7680, kHmin = 200, kHmax = 4320;
constexpr uint32_t kFbSlots     = 350;                // IOFramebuffer vtable slots (Tahoe 26.6.2, measured by the aux kext)
constexpr uint32_t kFbClassSize = 0x1d0;              // IOFramebuffer class size (same source)

// ---- the snapshot -------------------------------------------------------------------------------------
struct Snap {
	uint64_t bar0Phys;     // config-space BAR0 physical address
	uint64_t bar0Size;
	uint64_t surfOff;      // our surface, a BAR0/VRAM offset
	uint64_t gopOff;       // the GOP scanout at start
	uint64_t gopLen;
	uint32_t w, h, pitchBytes;
	uint32_t surfCfg;      // raw HUBP0 SURFACE_CONFIG at start: logged, never judged
};

enum SnapVerdict : uint32_t { kSnapOk = 0, kSnapBar0, kSnapAlign, kSnapRange, kSnapGeom, kSnapOverlap };

// The surface's real footprint: height times the real stride (pitch may exceed w*4 if padded).
inline uint64_t snap_bytes(const Snap &s) { return (uint64_t)s.h * s.pitchBytes; }

inline uint32_t snap_valid(const Snap &s) {
	if (s.bar0Phys == 0 || (s.bar0Phys & (kAlign - 1)) != 0 || s.bar0Size == 0) return kSnapBar0;
	if ((s.surfOff & (kAlign - 1)) != 0) return kSnapAlign;
	if (s.w < kWmin || s.w > kWmax || s.h < kHmin || s.h > kHmax) return kSnapGeom;
	if (s.pitchBytes < (uint64_t)s.w * 4 || (s.pitchBytes & 3) != 0) return kSnapGeom;
	const uint64_t bytes = snap_bytes(s);
	if (bytes > kBytes) return kSnapRange;   // the aperture must stay inside the window the write guard checks
	if (s.surfOff < kWriteFloor || s.surfOff > s.bar0Size || bytes > s.bar0Size - s.surfOff) return kSnapRange;
	if (s.gopLen > ~0ull - s.gopOff) return kSnapOverlap;
	if (s.surfOff < s.gopOff + s.gopLen && s.gopOff < s.surfOff + bytes) return kSnapOverlap;
	return kSnapOk;
}

// ---- answers --------------------------------------------------------------------------------------------
// B0: the mode is the snapshot's live geometry, not the kW/kH constants. Refresh stays the GOP's
// nominal (cosmetic for a single fixed mode).
struct ModeInfo { uint32_t w, h, refresh1616, maxDepthIndex; bool valid, safe, isDefault; };
inline bool mode_info(uint32_t mode, const Snap &s, ModeInfo *m) {
	if (mode != kModeId || !m) return false;
	*m = ModeInfo{ s.w, s.h, kRefresh1616, 0, true, true, true };
	return true;
}

struct PixelInfo { uint32_t bytesPerRow, bitsPerPixel, componentCount, bitsPerComponent, w, h; uint32_t masks[3]; };
inline bool pixel_info(uint32_t mode, int32_t depth, const Snap &s, PixelInfo *p) {
	if (mode != kModeId || depth != 0 || !p) return false;
	*p = PixelInfo{ s.pitchBytes, 32, 3, 8, s.w, s.h, { 0x00FF0000u, 0x0000FF00u, 0x000000FFu } };
	return true;
}

inline bool aperture(const Snap &s, uint64_t *phys, uint64_t *len) {
	if (!phys || !len || snap_valid(s) != kSnapOk) return false;
	*phys = s.bar0Phys + s.surfOff;
	*len = snap_bytes(s);
	return true;
}

constexpr uint32_t fourcc(char a, char b, char c, char d) {
	return ((uint32_t)(uint8_t)a << 24) | ((uint32_t)(uint8_t)b << 16) | ((uint32_t)(uint8_t)c << 8) | (uint32_t)(uint8_t)d;
}
constexpr uint32_t kAttrCursor      = fourcc('c', 'r', 's', 'r');   // kIOHardwareCursorAttribute
constexpr uint32_t kAttrPower       = fourcc('p', 'o', 'w', 'r');   // kIOPowerAttribute / kConnectionPower
constexpr uint32_t kConnEnable      = fourcc('e', 'n', 'a', 'b');
constexpr uint32_t kConnCheckEnable = fourcc('c', 'e', 'n', 'a');
constexpr uint32_t kConnFlags       = fourcc('f', 'l', 'g', 's');
constexpr uint32_t kConnHlDdc       = fourcc('h', 'd', 'd', 'c');

enum AttrRc : uint32_t { kRcSuper = 0, kRcValue = 1, kRcUnsupported = 2 };
struct Attr { uint32_t rc; uintptr_t value; };

inline Attr fb_attr(uint32_t a) {
	if (a == kAttrCursor) return Attr{ kRcValue, 0 };      // 0 = software cursor
	return Attr{ kRcSuper, 0 };
}
inline Attr conn_attr(uint32_t a) {
	if (a == kConnEnable || a == kConnCheckEnable) return Attr{ kRcValue, 1 };
	if (a == kConnFlags) return Attr{ kRcValue, 0 };
	if (a == kConnHlDdc) return Attr{ kRcUnsupported, 0 };  // no EDID in this design
	return Attr{ kRcSuper, 0 };
}

// ---- the self-flip ----------------------------------------------------------------------------------
// The framebuffer flips HUBP0 itself (William, 2026-10-12): our surface while it is enabled and both
// power attributes are on, the GOP scanout otherwise. OTG, DIG and the link stay the GOP's either way.
inline bool scanout_ours(bool enabled, uint32_t fbPower, uint32_t connPower) {
	return enabled && fbPower != 0 && connPower != 0;
}

// ---- the layout gate ---------------------------------------------------------------------------------
// Compares the vtable the linker built for our class with the family's real one, so a layout the SDK
// header does not describe is refused instead of dispatched through. The eight pure slots cannot be
// pinned (the family holds __cxa_pure_virtual in each); their order is trusted from the SDK header.
enum GateVerdict : uint32_t { kGateOk = 0, kGateFamSize, kGateOurSize, kGateInherited, kGateOverride, kGateTerm, kGatePin };
struct GateResult { uint32_t verdict; uint32_t slot; };
struct GateInput {
	const uintptr_t *ours;       // slot 0 first (the vtable symbol + 16 bytes)
	const uintptr_t *fam;
	uint32_t nslots;
	const uint16_t *ovr;         // slot indices our class defines itself
	uint32_t novr;
	const uint16_t *pinSlot;     // slots whose family entry must equal a named exported symbol
	const uintptr_t *pinAddr;
	uint32_t npins;
	uintptr_t textLo, textHi;    // our own image
	uint32_t famSize, wantFamSize;
	uint32_t ourSize;
};

inline bool in_set(const uint16_t *v, uint32_t n, uint32_t x) {
	for (uint32_t k = 0; k < n; k++) if (v[k] == x) return true;
	return false;
}

inline GateResult gate_run(const GateInput &g) {
	if (g.famSize != g.wantFamSize) return GateResult{ kGateFamSize, 0 };
	if (g.ourSize < g.famSize) return GateResult{ kGateOurSize, 0 };
	for (uint32_t i = 0; i < g.nslots; i++) {
		if (in_set(g.ovr, g.novr, i)) {
			if (g.ours[i] < g.textLo || g.ours[i] >= g.textHi) return GateResult{ kGateOverride, i };
		} else if (g.fam[i] == 0 || g.ours[i] != g.fam[i]) {
			return GateResult{ kGateInherited, i };
		}
	}
	if (g.fam[g.nslots] != 0 || g.ours[g.nslots] != 0) return GateResult{ kGateTerm, g.nslots };
	for (uint32_t k = 0; k < g.npins; k++) {
		if (g.pinSlot[k] >= g.nslots || g.fam[g.pinSlot[k]] != g.pinAddr[k]) return GateResult{ kGatePin, g.pinSlot[k] };
	}
	return GateResult{ kGateOk, 0 };
}

}  // namespace n33fb
