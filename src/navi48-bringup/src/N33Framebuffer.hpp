//
//  N33Framebuffer.hpp - the N33 (RX 7600 XT) framebuffer of stage 2 step 3. Answers IOFramebuffer's
//  questions from an immutable snapshot and writes no register; the flip is the user's n33ctl flip.
//  Design: stage2-framebuffer-design.md. Pure decisions: amd/n33_fb_pure.h.
//
#ifndef N33Framebuffer_hpp
#define N33Framebuffer_hpp

#if defined(N33_OBSERVE)
#include <IOKit/IOService.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include "amd/n33_fb_pure.h"

// The software nub the framebuffer personality matches. Published on demand, never at boot, and never withdrawn.
class N33DisplayNub : public IOService {
	OSDeclareDefaultStructors(N33DisplayNub)
	using super = IOService;
public:
	n33fb::Snap snap;
	static bool publish(IOService *pci, const n33fb::Snap &s);
	static IOService *published();     // NOT retained: an identity
	void free() override;
};

class N33Framebuffer : public IOFramebuffer {
	OSDeclareDefaultStructors(N33Framebuffer)
public:
	n33fb::Snap snap;
	uint32_t powerState;
	uint32_t connPower;
	bool pmRegistered;
	void initForPM();

	bool start(IOService *provider) override;
	IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
	IODeviceMemory *getVRAMRange() override;
	IOReturn enableController() override;
	const char *getPixelFormats() override;
	IOItemCount getDisplayModeCount() override;
	IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) override;
	IOReturn getInformationForDisplayMode(IODisplayModeID displayMode, IODisplayModeInformation *info) override;
	UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
	IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pixelInfo) override;
	IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;
	IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;
	IOReturn getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth) override;
	IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
	IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override;
	IOItemCount getConnectionCount() override;
	IOReturn setAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t value) override;
	IOReturn getAttributeForConnection(IOIndex connectIndex, IOSelect attribute, uintptr_t *value) override;
	bool hasDDCConnect(IOIndex connectIndex) override;
	IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber, IOSelect blockType, IOOptionBits options, UInt8 *data, IOByteCount *length) override;
};

// The layout gate. Returns an n33fb::GateVerdict (0 = pass); *slot gets the failing slot.
// Run it before the nub is registered: once the personality matches, IOKit instantiates the class.
uint32_t n33_fb_gate(uint32_t *slot);

#endif /* N33_OBSERVE */
#endif /* N33Framebuffer_hpp */
