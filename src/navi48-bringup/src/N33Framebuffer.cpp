//
//  N33Framebuffer.cpp - see the header. Adapted from the aux kext's Navi48Framebuffer (itself adapted
//  from RDNA4FB's AMDRDNA4FB, BSD-3-Clause, (c) the RDNA4FB authors): the overrides keep that
//  contract; the answers come from n33fb:: and the snapshot the nub carries.
//
#include "N33Framebuffer.hpp"

#if defined(N33_OBSERVE)
#include "Navi48Bringup.hpp"          // N48LOG
#include "amd/n33_fb_slots.h"

#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/OSAtomic.h>

static_assert(n33fb::kAttrCursor == kIOHardwareCursorAttribute, "four-character codes are IOKit's");
static_assert(n33fb::kAttrPower == kIOPowerAttribute && n33fb::kAttrPower == kConnectionPower, "power code");
static_assert(n33fb::kConnEnable == kConnectionEnable && n33fb::kConnCheckEnable == kConnectionCheckEnable, "enable codes");
static_assert(n33fb::kConnFlags == kConnectionFlags && n33fb::kConnHlDdc == kConnectionSupportsHLDDCSense, "flag codes");

OSDefineMetaClassAndStructors(N33DisplayNub, IOService)
OSDefineMetaClassAndStructors(N33Framebuffer, IOFramebuffer)

// ---- the nub -----------------------------------------------------------------------------------------
static N33DisplayNub *gNub = nullptr;

IOService *N33DisplayNub::published() { return gNub; }

bool N33DisplayNub::publish(IOService *pci, const n33fb::Snap &s) {
	if (gNub || !pci) return false;
	N33DisplayNub *n = OSTypeAlloc(N33DisplayNub);
	if (!n) return false;
	if (!n->init()) { n->release(); return false; }
	n->snap = s;
	n->setProperty("N33DisplayIndex", (uint64_t)1, 32);
	if (!n->attach(pci)) { n->release(); return false; }
	gNub = n;
	n->registerService();      // the N33Framebuffer personality matches the nub
	n->release();              // the registry holds it
	return true;
}

void N33DisplayNub::free() {
	if (gNub == this) gNub = nullptr;
	super::free();
}

// ---- the layout gate ---------------------------------------------------------------------------------
extern "C" const uintptr_t n33_ztv_N33Framebuffer[] __asm__("__ZTV14N33Framebuffer");
extern "C" const uintptr_t n33_ztvfam_IOFramebuffer[] __asm__("__ZTV13IOFramebuffer");
extern "C" kmod_info_t kmod_info;

// OSDefine slots (0 destructor, 1 deleting destructor, 7 getMetaClass), the eight pure slots
// (309 and 312-318) and the twelve named overrides (the pins' slots).
static const uint16_t kN33FbOvr[] = { 0, 1, 7, 184, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318,
                                      319, 322, 325, 326, 330, 331, 332, 344, 345 };

uint32_t n33_fb_gate(uint32_t *slotOut) {
	const OSSymbol *nm = OSSymbol::withCString("IOFramebuffer");
	const OSMetaClass *fm = nm ? OSMetaClass::getMetaClassWithName(nm) : nullptr;
	if (nm) nm->release();
	const uintptr_t pins[] = N33_FB_PIN_ADDRS;
	n33fb::GateInput in = {};
	in.ours = n33_ztv_N33Framebuffer + 2;          // slot 0 sits at the vtable symbol + 16
	in.fam = n33_ztvfam_IOFramebuffer + 2;
	in.nslots = n33fb::kFbSlots;
	in.ovr = kN33FbOvr; in.novr = (uint32_t)(sizeof(kN33FbOvr) / sizeof(kN33FbOvr[0]));
	in.pinSlot = kN33FbPinSlot; in.pinAddr = pins; in.npins = (uint32_t)(sizeof(kN33FbPinSlot) / sizeof(kN33FbPinSlot[0]));
	in.textLo = (uintptr_t)kmod_info.address; in.textHi = in.textLo + (uintptr_t)kmod_info.size;
	in.famSize = fm ? (uint32_t)fm->getClassSize() : 0u;
	in.wantFamSize = n33fb::kFbClassSize;
	in.ourSize = (uint32_t)sizeof(N33Framebuffer);
	const n33fb::GateResult r = n33fb::gate_run(in);
	N48LOG("n33 fb gate: verdict %u slot %u (family size %#x, want %#x, our size %#x)",
	       r.verdict, r.slot, in.famSize, in.wantFamSize, in.ourSize);
	if (slotOut) *slotOut = r.slot;
	return r.verdict;
}

// ---- the framebuffer ---------------------------------------------------------------------------------
bool N33Framebuffer::start(IOService *provider) {
	N33DisplayNub *nub = OSDynamicCast(N33DisplayNub, provider);
	if (!nub) return false;
	uint32_t slot = 0;
	if (n33_fb_gate(&slot) != n33fb::kGateOk) return false;           // belt and braces: publish ran it already
	if (n33fb::snap_valid(nub->snap) != n33fb::kSnapOk) return false;
	snap = nub->snap; powerState = 2u; connPower = 1u; pmRegistered = false; enabled = false; onOurs = false;
	return IOFramebuffer::start(provider);
}

// RDNA4FB's initForPM as the aux kext carries it: three power states, system sleep vetoed, registered with the
// policy maker, no sleep until the displays allow it.
void N33Framebuffer::initForPM() {
	if (pmRegistered) return;
	pmRegistered = true;
	static IOPMPowerState powerStates[3] = {
		{ 1, 0,                 0,            0,            0, 0, 0, 0, 0, 0, 0, 0 },
		{ 1, 0,                 0,            kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
	};
	for (auto &state : powerStates) state.capabilityFlags |= kIOPMPreventSystemSleep;
	registerPowerDriver(this, powerStates, 3);
	temporaryPowerClampOn();
	changePowerStateTo(1);
	IOService *p = getProvider();
	if (p) p->setProperty("IOPMIsPowerManaged", true);
}

// HUBP0 follows n33fb::scanout_ours. A refusal leaves it where it was and is logged; the display keeps working on
// whichever surface HUBP0 shows, so no caller fails over it.
void N33Framebuffer::syncScanout(const char *why) {
	const bool want = n33fb::scanout_ours(enabled, powerState, connPower);
	if (want == onOurs) return;
	const uint32_t r = n33_fb_scanout(want);
	N48LOG("n33 fb: %s: HUBP0 -> %s: %s (%u)", why, want ? "our surface" : "the GOP scanout", r ? "refused" : "done", r);
	if (r == 0) onOurs = want;
}

IOReturn N33Framebuffer::enableController() {
	initForPM();
	enabled = true;
	syncScanout("enableController");
	return kIOReturnSuccess;
}

IODeviceMemory *N33Framebuffer::getApertureRange(IOPixelAperture aperture) {
	uint64_t phys = 0, len = 0;
	if (aperture != kIOFBSystemAperture || !n33fb::aperture(snap, &phys, &len)) return nullptr;
	return IODeviceMemory::withRange((IOPhysicalAddress)phys, (IOPhysicalLength)len);
}
IODeviceMemory *N33Framebuffer::getVRAMRange() {
	uint64_t phys = 0, len = 0;
	if (!n33fb::aperture(snap, &phys, &len)) return nullptr;
	return IODeviceMemory::withRange((IOPhysicalAddress)phys, (IOPhysicalLength)len);
}

const char *N33Framebuffer::getPixelFormats() {
	static const char formats[] = IO32BitDirectPixels "\0";            // NUL-separated, double-NUL terminated
	return formats;
}
IOItemCount N33Framebuffer::getDisplayModeCount() { return 1; }
IOReturn N33Framebuffer::getDisplayModes(IODisplayModeID *all) {
	if (!all) return kIOReturnBadArgument;
	all[0] = (IODisplayModeID)n33fb::kModeId;
	return kIOReturnSuccess;
}
IOReturn N33Framebuffer::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) {
	n33fb::ModeInfo m;
	if (!info || !n33fb::mode_info((uint32_t)mode, snap, &m)) return kIOReturnBadArgument;
	bzero(info, sizeof(*info));
	info->nominalWidth = m.w; info->nominalHeight = m.h; info->refreshRate = m.refresh1616;
	info->maxDepthIndex = (IOIndex)m.maxDepthIndex;
	info->flags = (m.valid ? kDisplayModeValidFlag : 0u) | (m.safe ? kDisplayModeSafeFlag : 0u) | (m.isDefault ? kDisplayModeDefaultFlag : 0u);
	return kIOReturnSuccess;
}
UInt64 N33Framebuffer::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) { return 0; }   // obsolete: must return 0
IOReturn N33Framebuffer::getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) {
	n33fb::PixelInfo p;
	if (!pi || aperture != kIOFBSystemAperture || !n33fb::pixel_info((uint32_t)mode, (int32_t)depth, snap, &p)) return kIOReturnBadArgument;
	bzero(pi, sizeof(*pi));
	pi->bytesPerRow = p.bytesPerRow; pi->bitsPerPixel = p.bitsPerPixel; pi->pixelType = kIORGBDirectPixels;
	pi->componentCount = p.componentCount; pi->bitsPerComponent = p.bitsPerComponent;
	for (unsigned i = 0; i < 3u; i++) pi->componentMasks[i] = p.masks[i];
	strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
	pi->activeWidth = p.w; pi->activeHeight = p.h;
	return kIOReturnSuccess;
}
IOReturn N33Framebuffer::getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) {
	if (mode) *mode = (IODisplayModeID)n33fb::kModeId;
	if (depth) *depth = 0;
	return kIOReturnSuccess;
}
IOReturn N33Framebuffer::getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) {
	if (mode) *mode = (IODisplayModeID)n33fb::kModeId;
	if (depth) *depth = 0;
	return kIOReturnSuccess;
}
IOReturn N33Framebuffer::setDisplayMode(IODisplayModeID mode, IOIndex depth) {
	return (depth == 0 && (uint32_t)mode == n33fb::kModeId) ? kIOReturnSuccess : kIOReturnUnsupported;   // the only mode; no hardware write
}

IOReturn N33Framebuffer::getAttribute(IOSelect attribute, uintptr_t *value) {
	const n33fb::Attr a = n33fb::fb_attr((uint32_t)attribute);
	if (a.rc == n33fb::kRcSuper) return IOFramebuffer::getAttribute(attribute, value);
	if (value) *value = a.value;
	return kIOReturnSuccess;
}
IOReturn N33Framebuffer::setAttribute(IOSelect attribute, uintptr_t value) {
	if ((uint32_t)attribute != n33fb::kAttrPower) return IOFramebuffer::setAttribute(attribute, value);
	powerState = (uint32_t)value;
	syncScanout("power");
	return kIOReturnSuccess;
}
IOItemCount N33Framebuffer::getConnectionCount() { return 1; }
IOReturn N33Framebuffer::getAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t *value) {
	const n33fb::Attr a = n33fb::conn_attr((uint32_t)attribute);
	if (a.rc == n33fb::kRcSuper) return IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
	if (a.rc == n33fb::kRcUnsupported) return kIOReturnUnsupported;
	if (value) *value = a.value;
	return kIOReturnSuccess;
}
IOReturn N33Framebuffer::setAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t value) {
	if ((uint32_t)attribute != n33fb::kAttrPower) return IOFramebuffer::setAttributeForConnection(connectIndex, attribute, value);
	connPower = (uint32_t)value;
	syncScanout("connection power");
	return kIOReturnSuccess;
}
bool N33Framebuffer::hasDDCConnect(IOIndex) { return false; }
IOReturn N33Framebuffer::getDDCBlock(IOIndex, UInt32, IOSelect, IOOptionBits, UInt8 *, IOByteCount *) { return kIOReturnUnsupported; }

#endif /* N33_OBSERVE */
