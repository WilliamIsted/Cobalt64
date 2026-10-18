// Host test for stage 3 step 7 X4a: the gfx10.3 forms of the K4 dispatch and the shader-free WRITE_DATA stream
// (n33_cp_pure.h pkt_dispatch_g103, pkt_wd_g103), translated to gfx11 by xlat12, must equal the native builders
// (pkt_dispatch, the hardware-proven K4, and pkt_wd) dword for dword. The expected output is always the native
// builder, never the translator's own output. Both entries are checked: the core (xlat12_translate_level) and the IB
// layer (xlat12_ib_translate_level). Build and run: make -C src/xlat12 test.
#include <cstdio>
#include <cstring>
#include "xlat12.h"
#include "xlat12_ib.h"
#include "../../navi48-bringup/src/amd/n33_cp_pure.h"

using namespace n33cp;

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __func__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static constexpr uint32_t kCap = 64;   // the kext's buffer for X4b: the stream's worst case is below it (see report)

// One translation through the chosen entry, compared with the native stream. cap is the output capacity.
static void check_one(const char *what, bool ib, const uint32_t *in, uint32_t n, const uint32_t *want, uint32_t wn,
                      uint32_t cap, uint32_t wantRepacked) {
	uint32_t out[kCap + 8], len = 0xFFFFFFFFu, eop = 0;
	for (uint32_t k = 0; k < kCap + 8; k++) out[k] = 0xDEADBEEFu;
	xlat12_ctx ctx; ctx.vs_mode = XLAT12_VS_UNKNOWN;
	xlat12_stats st;
	uint32_t s = ib ? xlat12_ib_translate_level(XLAT12_LEVEL_GFX11, &ctx, in, n, out, cap, &len, &st, &eop)
	                : (uint32_t)xlat12_translate_level(XLAT12_LEVEL_GFX11, &ctx, in, n, out, cap, &len, &st);
	const char *e = ib ? "ib" : "core";
	CHECK(s == 0, "%s %s: status %s (op 0x%x reg 0x%x)", what, e, xlat12_ib_status_name(s), eop, st.err_reg_addr);
	if (s) return;
	CHECK(len == wn, "%s %s: %u dwords, native %u", what, e, len, wn);
	for (uint32_t k = 0; k < wn && k < len; k++)
		CHECK(out[k] == want[k], "%s %s: dword %u 0x%08x, native 0x%08x", what, e, k, out[k], want[k]);
	CHECK(out[cap] == 0xDEADBEEFu, "%s %s: wrote past the capacity", what, e);
	CHECK(st.regs_repacked == wantRepacked, "%s %s: %u registers repacked, expected %u", what, e, st.regs_repacked,
	      wantRepacked);
}

// Capacity one short of the output: refused, nothing past the end.
static void check_short(const char *what, bool ib, const uint32_t *in, uint32_t n, uint32_t wn) {
	uint32_t out[kCap + 8], len = 0;
	for (uint32_t k = 0; k < kCap + 8; k++) out[k] = 0xDEADBEEFu;
	xlat12_ctx ctx; ctx.vs_mode = XLAT12_VS_UNKNOWN;
	xlat12_stats st;
	uint32_t s = ib ? xlat12_ib_translate_level(XLAT12_LEVEL_GFX11, &ctx, in, n, out, wn - 1, &len, &st, nullptr)
	                : (uint32_t)xlat12_translate_level(XLAT12_LEVEL_GFX11, &ctx, in, n, out, wn - 1, &len, &st);
	CHECK(s == XLAT12_ERR_CAPACITY, "%s %s: cap %u gave %s", what, ib ? "ib" : "core", wn - 1, xlat12_ib_status_name(s));
	CHECK(out[wn - 1] == 0xDEADBEEFu, "%s %s: wrote past a short capacity", what, ib ? "ib" : "core");
}

int main() {
	const uint64_t fb[] = { 0x8000000000ull, 0x8000000000ull + 0x10000000ull, 0x0000F40000000000ull };
	const uint64_t hostB[] = { n33gmc::slot_mc(kSlotB), n33gmc::slot_mc(0), n33gmc::slot_mc(5) + 0x40 };
	const uint32_t seqs[] = { 1, 2, 0x7FFFFFFFu, 0xFFFFFFFFu };
	uint32_t cases = 0;
	for (uint64_t f : fb)
		for (uint64_t h : hostB)
			for (uint32_t seq : seqs)
				for (int gl2 = 0; gl2 < 2; gl2++) {
					const Geo g = geo(f);
					const uint64_t code = g.code, result = h, fence = h + kHostFence4;
					uint32_t in[kCap], want[kCap];
					const uint32_t n = pkt_dispatch_g103(in, code, result, fence, seq, gl2);
					const uint32_t wn = pkt_dispatch(want, code, result, fence, seq, gl2);
					CHECK(n == 41 && wn == 41, "dispatch lengths %u %u", n, wn);
					for (int ib = 0; ib < 2; ib++) {
						check_one("dispatch", ib, in, n, want, wn, kCap, 2);
						check_one("dispatch exact-cap", ib, in, n, want, wn, wn, 2);
						check_short("dispatch", ib, in, n, wn);
					}
					const uint32_t m = pkt_wd_g103(in, h + kHostK3, kMagic + seq, fence, seq, gl2);
					const uint32_t wm = pkt_wd(want, h + kHostK3, kMagic + seq, fence, seq, gl2);
					CHECK(m == 13 && wm == 13, "wd lengths %u %u", m, wm);
					for (int ib = 0; ib < 2; ib++) {
						check_one("wd", ib, in, m, want, wm, kCap, 0);
						check_one("wd exact-cap", ib, in, m, want, wm, wm, 0);
						check_short("wd", ib, in, m, wm);
					}
					cases++;
				}
	// The comparison itself: a gfx10.3 stream with one register wrong must not match (TMPRING_SIZE.WAVESIZE 1).
	{
		uint32_t in[kCap], want[kCap], out[kCap], len = 0;
		const Geo g = geo(fb[0]);
		const uint32_t n = pkt_dispatch_g103(in, g.code, hostB[0], hostB[0] + kHostFence4, 1, true);
		pkt_dispatch(want, g.code, hostB[0], hostB[0] + kHostFence4, 1, true);
		CHECK(in[11] == pkt3(kOpSetShReg, 1) && in[12] == kComputeTmpring - kShRegStart, "layout");
		in[13] = 1u << 12;   // TMPRING_SIZE's value dword (PGM 0-3, RSRC1/2 4-7, RESOURCE_LIMITS 8-10, TMPRING 11-13)
		xlat12_ctx ctx; ctx.vs_mode = XLAT12_VS_UNKNOWN;
		CHECK(xlat12_ib_translate_level(XLAT12_LEVEL_GFX11, &ctx, in, n, out, kCap, &len, nullptr, nullptr) == 0, "neg");
		CHECK(len == 41 && memcmp(out, want, 41 * 4) != 0, "a wrong TMPRING_SIZE value matched the native stream");
	}
	printf("test_n33_xib: %u cases (3 fb bases x 3 host pages x 4 seqnos x 2 gl2wb), %d checks passed, %d failed\n",
	       cases, g_pass, g_fail);
	return g_fail ? 1 : 0;
}
