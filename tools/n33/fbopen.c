// fbopen: open N33Framebuffer's server connection (type 0) from userspace, the way WindowServer
// would, dump the registry entry, hold the connection, then close it. Stage 2 step 3 diagnostic.
// Build: clang -O2 -Wall -framework IOKit -framework CoreFoundation fbopen.c -o fbopen
// Run as root, in a separate Terminal window: sudo ./fbopen [hold-seconds]
//   hold-seconds defaults to 10; 0 holds until Ctrl-C. Closing (exit, Ctrl-C) closes the framebuffer.
//
// What IOFramebuffer::newUserClient does for type 0 (IOGraphics-599):
//   - kIOReturnExclusiveAccess if a server connection already exists (WindowServer cannot take it
//     while this tool holds it);
//   - setPlatformConsole(kPEReleaseScreen) only if this framebuffer is the console one or no console
//     framebuffer is set;
//   - open(): the family open path. Per the fb-open study, IOGraphics writes a gray fill into our
//     surface at VRAM 0x2000000 and WindowServer-side code draws there. HUBP0 stays on the GOP
//     buffer, so nothing is scanned out of it until a flip.
// N33Framebuffer::enableController writes no register.
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum { kServerConnectType = 0 };   // kIOFBServerConnectType

static volatile sig_atomic_t stop;
static void onSig(int s) { (void)s; stop = 1; }

static void dump(io_service_t svc, const char *label) {
	CFMutableDictionaryRef p = NULL;
	kern_return_t kr = IORegistryEntryCreateCFProperties(svc, &p, kCFAllocatorDefault, 0);
	printf("---- %s ----\n", label);
	if (kr != KERN_SUCCESS || !p) { printf("properties read failed: 0x%x\n", kr); return; }
	CFShow(p);
	CFRelease(p);
	fflush(stdout);
}

int main(int argc, char **argv) {
	unsigned hold = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 10;
	signal(SIGINT, onSig);
	signal(SIGTERM, onSig);

	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("N33Framebuffer"));
	if (!svc) { fprintf(stderr, "no N33Framebuffer in the registry (kext not loaded or nub not published)\n"); return 2; }
	uint64_t id = 0;
	IORegistryEntryGetRegistryEntryID(svc, &id);
	printf("euid %d, N33Framebuffer registry id %#llx\n", geteuid(), id);
	dump(svc, "before open");

	io_connect_t conn = IO_OBJECT_NULL;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), kServerConnectType, &conn);
	printf("IOServiceOpen type %d: kr 0x%x\n", kServerConnectType, kr);
	if (kr != KERN_SUCCESS) { IOObjectRelease(svc); return 1; }

	dump(svc, "after open");
	if (hold) printf("holding %u s (Ctrl-C closes)\n", hold); else printf("holding until Ctrl-C\n");
	fflush(stdout);
	for (unsigned t = 0; !stop && (!hold || t < hold); t++) sleep(1);

	kr = IOServiceClose(conn);
	printf("IOServiceClose: kr 0x%x\n", kr);
	IOObjectRelease(svc);
	return kr == KERN_SUCCESS ? 0 : 1;
}
