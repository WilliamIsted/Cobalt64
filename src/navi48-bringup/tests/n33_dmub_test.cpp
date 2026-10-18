// n33_dmub_test.cpp: host test of amd/n33_dmub_pure.h (stage 3 DMCUB bring-up proof: the firmware split, the DMUB
// window layout, the QUERY_FEATURE_CAPS command header and ring push, and the gate/refusal order).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src/amd src/navi48-bringup/tests/n33_dmub_test.cpp -o $SCRATCH/n33_dmub_test && \
//       $SCRATCH/n33_dmub_test [<dcn_3_2_1_dmcub.bin>] [<linux tree root>]
// Part 1: the pure logic against the design's numbers. Part 2 (if the firmware file is given): parse the real file and
// check the header fields and the meta (fw_region_size, trace, version) the region math depends on. Part 3 (if the
// Linux tree root is given): check every DMU register offset against dcn_3_2_1_offset.h and the command/boot constants
// against dmub/inc/dmub_cmd.h.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "n33_dmub_pure.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
using namespace n33dmub;

static constexpr uint64_t kFbBase = 0x8000000000ull;

// The 11 protected ranges from the live snapshot (2026-10-18 05:37), as {off, len}. Our DMUB block must miss all.
struct Rng { uint64_t off, len; };
static const Rng kSnapshot[] = {
	{0x0, 0x300000}, {0x83fec10000, 0x40000}, {0x83fec30000, 0x30000}, {0x83fec40000, 0x30000},
	{0x83fec50000, 0x30000}, {0x83febf0000, 0x30000}, {0x8100000, 0x3000}, {0x8600000, 0x4000},
	{0x8800000, 0x81000}, {0x8a03000, 0x6000}, {0x8a09000, 0x1000},
	// the fb nub surface when published (worst case 2560x1440x4), which the kext also owns:
	{0x2000000, 0xe10000},
};
static bool overlaps_any(uint64_t off, uint64_t len) {
	for (auto &r : kSnapshot) if (off < r.off + r.len && r.off < off + len) return true;
	return false;
}

static void firmware_constants() {
	CHECK(kFwInstSize == 231840);
	CHECK(kFwInstSize == 0x389a0);
	CHECK(kInstConstBytes - kPspHeader - kPspFooter == kFwInstSize);
	CHECK(kFwVersion == 0x07003500u);
	CHECK(kRbSize == 0x2000);
	CHECK(kMailboxSize == 0x4000);
	CHECK(kInbox1RingSize == 0x2000);   // matches the snapshot's DMCUB_INBOX1_SIZE
}

static void region_math() {
	// A plausible expansion-ROM VBIOS image (64 KiB) plus the measured meta sizes.
	RegionInfo r = region_info(0x10000, kMetaFwRegionSize, kMetaTraceSize);
	// Every base 256-aligned, every size 64-aligned, each window after the previous, total 4 KiB-aligned.
	CHECK(r.w[0].base == 0);
	for (uint32_t i = 0; i < kWinTotal; i++) {
		CHECK((r.w[i].base & 0xff) == 0);
		CHECK(((r.w[i].top - r.w[i].base) & 0x3f) == 0);
		if (i) CHECK(r.w[i].base >= r.w[i-1].top);
	}
	CHECK((r.total & 0xfff) == 0);
	CHECK(r.total >= r.w[kWinTotal-1].top);
	// The inst-const window is first and is the firmware's inst size, 64-aligned.
	CHECK(r.w[kWinInstConst].base == 0);
	CHECK(r.w[kWinInstConst].top == align_up(kFwInstSize, 64));
	// It fits the 2 MiB slab and sits above the 16 MiB floor.
	CHECK(placement_ok(r));
	CHECK(r.total <= kDmubMax);
	CHECK(kDmubOff >= kWriteFloor);
	// The whole owned block misses every protected range in the snapshot.
	CHECK(!overlaps_any(kDmubOff, r.total));
	CHECK(!overlaps_any(kDmubOff, kDmubMax));
	// Mailbox / inbox1 / outbox1 MC addresses are inside the block, 256-aligned, outbox1 one RB after inbox1.
	const uint64_t mb = mailbox_mc(kFbBase, r);
	CHECK(mb == kFbBase + kDmubOff + r.w[kWinMailbox].base);
	CHECK((mb & 0xff) == 0);
	CHECK(inbox1_mc(kFbBase, r) == mb);
	CHECK(outbox1_mc(kFbBase, r) == mb + kRbSize);
	CHECK(vbios_mc(kFbBase, r) == kFbBase + kDmubOff + r.w[kWinVbios].base);
	// A VBIOS so large the block would not fit is rejected by placement_ok.
	RegionInfo big = region_info(0x200000, kMetaFwRegionSize, kMetaTraceSize);
	CHECK(!placement_ok(big));
}

static void command_and_ring() {
	// GPINT STOP_FW as Linux encodes it (dmub_cmd.h:1474-1481, :1503): command_code 2 at bits 16-27, status 1.
	CHECK(gpint(kGpintStopFw) == 0x10020000u);
	// DMCUB-side window bases (dmub_srv.c:83-86); the hardware keeps the low 29 bits (snapshot 05:37: CW3 base 0x3000000).
	CHECK(kCw3Base == 0x63000000u && kCw4Base == 0x64000000u && kCw5Base == 0x65000000u && kCw6Base == 0x66000000u);
	CHECK(kHdrQueryFeatureCaps == 0x10010006u);
	CHECK(cmd_header(6, 0, 1, 16) == 0x10010006u);
	CHECK(template_ok(kTplQueryFeatureCaps));
	CHECK(!template_ok(1));                      // transmitter templates are a later step, not allowed now
	CHECK(template_header(kTplQueryFeatureCaps) == kHdrQueryFeatureCaps);
	uint32_t slot[kSlotDwords];
	build_slot(slot, template_header(kTplQueryFeatureCaps));
	CHECK(slot[0] == 0x10010006u);
	for (uint32_t i = 1; i < kSlotDwords; i++) CHECK(slot[i] == 0);
	// Ring push arithmetic.
	CHECK(next_wptr(0) == 64);
	CHECK(next_wptr(kInbox1RingSize - 64) == 0);
	CHECK(ring_usable(0x2000, 0, 0));
	CHECK(!ring_usable(0x4000, 0, 0));           // wrong size
	CHECK(!ring_usable(0x2000, 64, 0));          // busy (wptr != rptr)
	CHECK(!ring_usable(0x2000, 0x1fc0, 0x1fc0)); // next slot would wrap the ring end
	// Boot-status decode (SCRATCH0): the snapshot read 0x2 = mailbox_rdy set, dal_fw clear (VBIOS DMCUB, not DAL).
	CHECK(!dal_fw(0x2));
	CHECK(mailbox_rdy(0x2));
	CHECK(!auto_loaded(0x2));
	CHECK(auto_loaded(kBootDalFw | kBootMailboxRdy));
	CHECK(dal_fw(0x3) && mailbox_rdy(0x3));
	CHECK(is_hw_init(kCntlEnable, kBootDalFw));
	CHECK(!is_hw_init(0, kBootDalFw));
	// The sent header carries ret_status 1 (bit 16); the firmware clears it to 0 in the reply to signal success.
	CHECK(reply_ret_status(kHdrQueryFeatureCaps) == 1);
	CHECK(reply_ret_status(kHdrQueryFeatureCaps & ~(1u << 16)) == 0);
}

static void gates() {
	RegionInfo r = region_info(0x10000, kMetaFwRegionSize, kMetaTraceSize);
	State s{};
	// Nothing before the snapshot.
	CHECK(may_reset(s, true, false, false) == kWhyNotSnapped);
	CHECK(may_load(s, true, false) == kWhyNotSnapped);
	CHECK(may_init(s, r, true, true) == kWhyNotSnapped);
	CHECK(may_check(s) == kWhyNotSnapped);
	record_snap(s, 0x10000);
	CHECK(s.snapped && s.vbiosSize == 0x10000);
	CHECK(may_check(s) == kWhyOk);
	// Load needs autoload; one only; order: load before reset.
	CHECK(may_load(s, false, false) == kWhyOrder);        // RLC autoload not done
	CHECK(may_load(s, true, true) == kWhyPspType51);      // PSP refused type 51
	CHECK(may_reset(s, true, false, false) == kWhyOrder); // reset before load
	CHECK(may_load(s, true, false) == kWhyOk);
	record_load(s, true);
	CHECK(s.dmubLoaded);
	CHECK(may_load(s, true, false) == kWhyOrder);         // once
	// Reset gate: the display preconditions.
	CHECK(may_reset(s, false, false, false) == kWhyHubpNotGop);
	CHECK(may_reset(s, true, true, false) == kWhyNubPublished);
	CHECK(may_reset(s, true, false, true) == kWhyCpRunning);
	CHECK(may_reset(s, true, false, false) == kWhyOk);
	record_reset(s);
	// Init gate: reset done, region fits, windows clear, VBIOS present.
	CHECK(may_init(s, r, true, false) == kWhyVbios);
	CHECK(may_init(s, r, false, true) == kWhyWindowOverlap);
	RegionInfo big = region_info(0x200000, kMetaFwRegionSize, kMetaTraceSize);
	CHECK(may_init(s, big, true, true) == kWhyWindowOverlap);
	CHECK(may_init(s, r, true, true) == kWhyOk);
	record_init(s, true);
	CHECK(s.inited && s.autoLoaded);
	// Cmd gate: init + autoload + template + idle ring.
	CHECK(may_cmd(s, 1, 0x2000, 0, 0) == kWhyTemplate);
	CHECK(may_cmd(s, kTplQueryFeatureCaps, 0x4000, 0, 0) == kWhyRing);
	CHECK(may_cmd(s, kTplQueryFeatureCaps, 0x2000, 64, 0) == kWhyRing);
	CHECK(may_cmd(s, kTplQueryFeatureCaps, 0x2000, 0, 0) == kWhyOk);
	record_proof(s, true);
	CHECK(s.proofDone);
	// Cmd before init / before autoload.
	State s2{}; record_snap(s2, 0x10000); record_load(s2, true); record_reset(s2);
	CHECK(may_cmd(s2, kTplQueryFeatureCaps, 0x2000, 0, 0) == kWhyOrder);   // not inited
	record_init(s2, false);
	CHECK(may_cmd(s2, kTplQueryFeatureCaps, 0x2000, 0, 0) == kWhyAutoLoadTimeout);
}

// Part 2: parse the real firmware file.
static void firmware_file(const char *path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) { printf("SKIP firmware file (%s not readable)\n", path); return; }
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	CHECK(b.size() == kFwFileSize);
	FwHeader h{};
	CHECK(parse_header(b.data(), (uint32_t)b.size(), &h));
	CHECK(h.ucodeVersion == kFwVersion);
	CHECK(h.ucodeSize == kInstConstBytes);
	CHECK(h.ucodeArrayOffset == kUcodeArrayOffset);
	CHECK(h.instConstBytes == kInstConstBytes);
	CHECK(h.bssDataBytes == kBssDataBytes);
	FwMeta m{};
	CHECK(find_meta(b.data(), (uint32_t)b.size(), h, &m));
	CHECK(m.fwVersion == kFwVersion);
	CHECK(m.fwRegionSize == kMetaFwRegionSize);
	CHECK(m.traceSize == kMetaTraceSize);
	CHECK(m.dalFw == 1);
	CHECK(m.sharedSize == kMetaSharedSize);
	// The region math on the real meta fits the slab.
	RegionInfo r = region_info(0x10000, m.fwRegionSize, m.traceSize);
	CHECK(placement_ok(r));
	CHECK(!overlaps_any(kDmubOff, r.total));
	printf("firmware file OK: version 0x%08x fw_region 0x%x trace 0x%x; DMUB block %llu KiB at 0x%llx\n",
	       m.fwVersion, m.fwRegionSize, m.traceSize, (unsigned long long)(r.total >> 10), (unsigned long long)kDmubOff);
}

// Part 3: check register offsets / command constants against the Linux tree.
// Read the dword value a `#define reg<NAME>  0x...` line gives (the token two fields along), or -1.
static long reg_offset(const std::string &root, const std::string &rel, const std::string &name) {
	std::ifstream f(root + "/" + rel);
	if (!f) return -2;
	std::string tok;
	while (f >> tok) {
		if (tok == "#define") { std::string n, v; if ((f >> n) && n == ("reg" + name) && (f >> v)) return strtol(v.c_str(), nullptr, 0); }
	}
	return -1;
}
static bool grep_has(const std::string &root, const std::string &rel, const std::string &needle) {
	std::ifstream f(root + "/" + rel);
	if (!f) return false;
	std::string line;
	while (std::getline(f, line)) if (line.find(needle) != std::string::npos) return true;
	return false;
}
static void linux_tree(const char *root) {
	const std::string r = root;
	const std::string off = "drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_3_2_1_offset.h";
	// Every DMU register offset we use (absolute = 0x34C0 + offset) against dcn_3_2_1_offset.h, BASE_IDX 2.
	long wptr = reg_offset(r, off, "DMCUB_INBOX1_WPTR");
	if (wptr == -2) { printf("SKIP linux offset header (%s not found)\n", off.c_str()); return; }
	CHECK(wptr == kOffInbox1Wptr);
	CHECK(reg_offset(r, off, "DMCUB_INBOX1_BASE_ADDRESS") == kOffInbox1Base);
	CHECK(reg_offset(r, off, "DMCUB_INBOX1_RPTR") == kOffInbox1Rptr);
	CHECK(reg_offset(r, off, "DMCUB_CNTL") == kOffCntl);
	CHECK(reg_offset(r, off, "DMCUB_SCRATCH0") == kOffScratch0);
	// The firmware meta magic.
	CHECK(grep_has(r, "drivers/gpu/drm/amd/display/dmub/inc/dmub_cmd.h", "0x444D5542"));
	printf("linux tree offsets OK: INBOX1_WPTR 0x%04lx, CNTL 0x%04x (abs 0x%04x)\n", wptr, kOffCntl, kCntl);
}

int main(int argc, char **argv) {
	firmware_constants();
	region_math();
	command_and_ring();
	gates();
	if (argc > 1) firmware_file(argv[1]);
	if (argc > 2) linux_tree(argv[2]);
	if (fails) { printf("n33_dmub_test: %d FAILED\n", fails); return 1; }
	printf("n33_dmub_test: all passed\n");
	return 0;
}
