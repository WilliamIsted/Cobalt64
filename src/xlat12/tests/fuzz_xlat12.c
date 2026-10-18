/* Bounds fuzz for src/xlat12, run under -fsanitize=address,undefined.
 * Input and output buffers are malloc'd at exactly their stated sizes, so any
 * read past in_dwords or write past out_cap is an ASan finding. Streams are random
 * PM4-shaped packets (SET/INDEX/memory-loaded/verbatim opcodes, counts 0..0x3FFF,
 * offsets near and past block ends), mutated golden vectors, truncations, and
 * random output capacities including 0. Every OK result is re-parsed: the output
 * must be exactly a sequence of well-formed type-3 packets filling *out_dwords.
 * X3: the same streams also go through the IB layer at the same gfx_level - xlat12_ib_translate_level (an OK
 * output must walk to exactly its length), xlat12_ib_census_run_level, and the draw policy (in place, so an OK
 * output must walk to exactly the input length; gfx11 with XLAT12_EXTRA_GFX11 and a ring).
 * Usage: fuzz_xlat12 [iterations] [gfx_level] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xlat12.h"
#include "xlat12_ib.h"
#include "golden_vectors.h"

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static const uint32_t kOps[] = { 0x69, 0x76, 0x79, 0x9B, 0x7A, 0x61, 0x5F, 0x12, 0x28, 0x10, 0x2D, 0x46, 0x3F, 0x22,
                                 0x37, 0x3C, 0x40, 0x49, 0x58, 0x15 };   /* X3: the IB layer's verbatim / operand ops */
static const uint32_t kOffsets[] = { 0x0, 0x1, 0x8e, 0x191, 0x1e0, 0x201, 0x2d5, 0x0f0, 0x1fff, 0x2000, 0x3ff, 0x400,
                                     0x3fff, 0x4000, 0xFFFF, 0x30000001, 0x20000243, 0x10000242, 0xFFFFFFFF };

static uint32_t gen_stream(uint32_t *s, uint32_t cap)
{
    uint32_t n = 0;
    while (n < cap) {
        uint32_t kind = rnd() % 8;
        if (kind == 0) { s[n++] = rnd(); continue; }                    /* raw dword */
        uint32_t op = kOps[rnd() % (sizeof kOps / sizeof kOps[0])];
        uint32_t cnt;
        switch (rnd() % 6) {
        case 0: cnt = 0; break;
        case 1: cnt = 0x3FFF; break;
        case 2: cnt = rnd() % 40; break;
        default: cnt = 1 + rnd() % 8; break;
        }
        uint32_t hdrw = 0xC0000000u | (cnt << 16) | (op << 8) | (rnd() % 4 == 0 ? (rnd() & 0xFF) : 0);
        s[n++] = hdrw;
        uint32_t body = cnt + 1;
        for (uint32_t k = 0; k < body && n < cap; k++) {
            if (k == 0 && rnd() % 2) s[n++] = kOffsets[rnd() % (sizeof kOffsets / sizeof kOffsets[0])];
            else s[n++] = (rnd() % 3 == 0) ? (1u << 13) : rnd();
        }
    }
    return n;
}

static int wellformed(const uint32_t *o, uint32_t n)
{
    uint32_t i = 0;
    while (i < n) {
        if (((o[i] >> 30) & 3u) != 3u) return 0;
        uint32_t body = ((o[i] >> 16) & 0x3FFFu) + 1u;
        if ((uint64_t)i + 1u + body > n) return 0;
        i += 1u + body;
    }
    return i == n;
}

int main(int argc, char **argv)
{
    unsigned long iters = argc > 1 ? strtoul(argv[1], 0, 10) : 100000ul;
    const uint32_t level = argc > 2 ? (uint32_t)strtoul(argv[2], 0, 10) : XLAT12_LEVEL_GFX12;  /* X2: 11 = gfx11 */
    unsigned long hist[16] = { 0 };
    unsigned long malformed_out = 0, ib_ok = 0, draw_ok = 0;
    uint32_t *scratch = (uint32_t *)malloc(4096 * sizeof(uint32_t));
    for (unsigned long it = 0; it < iters; it++) {
        uint32_t len;
        if (rnd() % 3 == 0) {                                            /* mutate a golden vector */
            const GoldenVec *g = &kGolden[rnd() % GOLDEN_COUNT];
            len = g->in_len;
            memcpy(scratch, g->in, len * sizeof(uint32_t));
            uint32_t flips = rnd() % 4;
            for (uint32_t f = 0; f < flips && len; f++) scratch[rnd() % len] ^= 1u << (rnd() % 32);
            if (rnd() % 2 && len) len = rnd() % len;                     /* truncate */
            if (rnd() % 2) {                                             /* X3: end it in a draw, for the draw policy */
                scratch[len++] = 0xC0012D00u; scratch[len++] = 3u; scratch[len++] = 2u;
            }
        } else {
            len = gen_stream(scratch, 1 + rnd() % 256);
            if (rnd() % 2 && len) len = rnd() % len;
        }
        uint32_t *in = (uint32_t *)malloc((len ? len : 1) * sizeof(uint32_t));
        memcpy(in, scratch, len * sizeof(uint32_t));
        uint32_t cap;
        switch (rnd() % 4) { case 0: cap = 0; break; case 1: cap = rnd() % 8; break;
                             case 2: cap = len; break; default: cap = len * 3 + 16; break; }
        uint32_t *out = (uint32_t *)malloc((cap ? cap : 1) * sizeof(uint32_t));
        xlat12_ctx ctx; ctx.vs_mode = rnd() % 2 ? XLAT12_VS_NGG : XLAT12_VS_UNKNOWN;
        xlat12_stats st; uint32_t n = 0xFFFFFFFFu;
        xlat12_status s = xlat12_translate_level(level, rnd() % 8 ? &ctx : 0, in, len, out, cap, &n, rnd() % 8 ? &st : 0);
        if ((unsigned)s < 16) hist[s]++;
        if (s == XLAT12_OK) {
            if (n > cap || !wellformed(out, n)) malformed_out++;
        }
        {   /* X3: the IB layer */
            uint32_t m = 0xFFFFFFFFu, eop = 0;
            xlat12_ctx c2; c2.vs_mode = ctx.vs_mode;
            const uint32_t si = xlat12_ib_translate_level(level, rnd() % 8 ? &c2 : 0, in, len, out, cap, &m, rnd() % 8 ? &st : 0, &eop);
            if (si == 0u) { ib_ok++; if (m > cap || xlat12_ib_walk(out, m) != m) malformed_out++; }
            xlat12_ib_census cen;
            xlat12_ib_census_run_level(level, in, len, &cen);
            if (cen.walk_len > len) malformed_out++;
            if (len) {
                uint32_t *dout = (uint32_t *)malloc(len * sizeof(uint32_t)), dn = 0;
                xlat12_draw_extra ex; memset(&ex, 0, sizeof ex);
                ex.flags = level == XLAT12_LEVEL_GFX11 ? XLAT12_EXTRA_GFX11 : 0u;
                if (rnd() % 2) { ex.ring_va = 0x100000000ull; ex.gs_sgpr0_va = 0x200000000ull; ex.rsrc3_gs = 0xfffffdfdu; }
                xlat12_draw_stats ds;
                if (xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, in, len, dout, &dn, &ds) == 0u) {
                    draw_ok++;
                    if (dn != len || xlat12_ib_walk(dout, dn) != dn ||
                        xlat12_ib_draw_verify_ex(dout, dn, ex.flags, 0, 0) != 0u) malformed_out++;
                }
                free(dout);
            }
        }
        free(in); free(out);
    }
    free(scratch);
    printf("fuzz: gfx_level %u, %lu iterations; statuses:", level, iters);
    for (int k = 0; k <= XLAT12_ERR_CAPACITY; k++)
        printf(" %s=%lu", xlat12_status_name((xlat12_status)k), hist[k]);
    printf("\nfuzz: IB translate OK %lu, draw policy OK %lu\n", ib_ok, draw_ok);
    printf("fuzz: malformed OK outputs %lu\n", malformed_out);
    return malformed_out ? 1 : 0;
}
