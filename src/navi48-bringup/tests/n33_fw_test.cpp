// n33_fw_test.cpp: host test of tools/n33/n33_fw.h against the real firmware files (linux-firmware
// afabaf77 for GC/SDMA/PSP, 217ca6e4 for the SMU image). Expected sizes and types: the milestone 2
// design's firmware table (overview repo, stage3-psp-m2-design.md).
//   clang++ -std=c++17 -Wall -Wextra -Werror -I tools/n33 src/navi48-bringup/tests/n33_fw_test.cpp -o $SCRATCH/n33_fw_test
//   $SCRATCH/n33_fw_test <dir with gc_11_0_2_*.bin, sdma_6_0_2.bin, psp_13_0_7_sos.bin> <smu_13_0_7.bin (82.95.0)>
#include <cstdio>
#include <string>
#include <vector>
#include "../../../tools/n33/n33_fw.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static std::vector<uint8_t> load(const std::string &p) {
	std::vector<uint8_t> b;
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) { printf("FAIL: cannot open %s\n", p.c_str()); fails++; return b; }
	int c;
	while ((c = fgetc(f)) != EOF) b.push_back((uint8_t)c);
	fclose(f);
	return b;
}

int main(int argc, char **argv) {
	if (argc < 3) { printf("usage: n33_fw_test <fwdir> <smu.bin>\n"); return 2; }
	const std::string d = std::string(argv[1]) + "/";
	const char *names[7] = { "sdma_6_0_2.bin", "gc_11_0_2_pfp.bin", "gc_11_0_2_me.bin", "gc_11_0_2_mec.bin",
	                         "gc_11_0_2_mes_2.bin", "gc_11_0_2_mes1.bin", "gc_11_0_2_imu.bin" };
	std::vector<uint8_t> f[7];
	n33files fl = {};
	for (int i = 0; i < 7; i++) { f[i] = load(d + names[i]); fl.p[i] = f[i].data(); fl.n[i] = f[i].size(); }
	std::vector<uint8_t> rlc = load(d + "gc_11_0_2_rlc.bin"), sos = load(d + "psp_13_0_7_sos.bin"), smu = load(argv[2]);
	if (fails) return 1;

	n33img gc[26];
	CHECK(n33_fw_gc(&fl, rlc.data(), rlc.size(), gc) == 0);
	static const uint32_t types[26] = { 71, 72, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97,
	                                    33, 34, 81, 82, 68, 69, 20, 21, 26, 48, 25, 7, 8 };
	// the design table's byte column (sizes measured from the pinned files by the design agent)
	static const uint32_t sizes[26] = { 0x4400, 0x4200, 0x174C0, 0x2D0F0, 0x41300, 0x22000, 0x22000, 0x22000, 0x22000,
	                                    0x22000, 0x22000, 0x22000, 0x22000, 0x20510, 0x20000, 0x15FE0, 0x20000,
	                                    0x10200, 0x10200, 0xA00, 0x75A0, 0x10200, 0x8200, 0x2200, 0x3200, 0x6200 };
	for (int i = 0; i < 26; i++) {
		CHECK(gc[i].type == types[i]);
		if (gc[i].n != sizes[i]) printf("  %s: %#x, table %#x\n", gc[i].name, gc[i].n, sizes[i]);
		CHECK(gc[i].n == sizes[i]);
	}
	CHECK(gc[5].p == gc[6].p);                         // P0 and P1 stacks are the same bytes
	CHECK(gc[18].p == gc[17].p + gc[17].n);            // IMU DRAM follows IRAM
	uint64_t offs[26];
	const uint64_t used = n33_pack(gc, 26, offs);
	CHECK(used == 0x26C000);                           // the design's total for rows 1 to 26
	CHECK(used <= 0x300000);                           // fits fw_buf
	for (int i = 0; i < 26; i++) CHECK((offs[i] & 0xFFF) == 0);

	n33img s = {}; uint32_t ver = 0;
	CHECK(n33_fw_smu(smu.data(), smu.size(), &s, &ver) == 0);
	CHECK(ver == 0x00525F00u && s.n == 261632 && s.type == 18);   // 82.95.0 (D7); payload at 256

	n33img toc = {};
	CHECK(n33_fw_toc(sos.data(), sos.size(), &toc) == 0);
	CHECK(toc.n == 2304 && toc.p == sos.data() + 240640);          // design: TOC at file offset 240640

	std::vector<uint8_t> bad = rlc; bad[0] ^= 1;                     // size_bytes no longer the file size
	n33img r[7];
	CHECK(n33_fw_rlc(bad.data(), bad.size(), r) != 0);
	CHECK(n33_fw_sdma(rlc.data(), rlc.size(), r) != 0);              // wrong header version

	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("n33_fw_test: all passed\n");
	return 0;
}
