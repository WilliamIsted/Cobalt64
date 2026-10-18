/* Host tests for the gfx11 path of src/xlat12 (stage 3 step 7, sub-step X2).
 * Build and run: make -C src/xlat12 test. Expected outputs: tests/golden_vectors_gfx11.h,
 * from gen/gen_golden_gfx11.py's oracle over Mesa gfx103.json / gfx11.json. */
#include <stdio.h>
#include <string.h>
#include "xlat12.h"
#include "golden_vectors_gfx11.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define CAP 16384u
static uint32_t g_out[CAP];

static void test_golden(void)
{
    for (size_t v = 0; v < GOLDEN11_COUNT; v++) {
        const GoldenVec11 *g = &kGolden11[v];
        xlat12_ctx ctx; ctx.vs_mode = g->ngg_in ? XLAT12_VS_NGG : XLAT12_VS_UNKNOWN;
        xlat12_stats st; uint32_t n = 0;
        xlat12_status s = xlat12_translate_level(XLAT12_LEVEL_GFX11, &ctx, g->in, g->in_len, g_out, CAP, &n, &st);
        const char *sn = xlat12_status_name(s);
        CHECK(strcmp(sn, g->status) == 0, "%s: status %s, oracle %s (err_addr 0x%x name %s)", g->name, sn, g->status,
              st.err_reg_addr, st.err_name ? st.err_name : "-");
        CHECK(st.regs_in == g->regs_in && st.regs_identical == g->identical && st.regs_moved == g->moved &&
              st.regs_repacked == g->repacked && st.regs_dropped_absent == g->dropped_absent &&
              st.regs_dropped_legacy_vs == g->dropped_legacy && st.regs_reused == g->regs_reused &&
              st.regs_scissor_adjusted == 0,
              "%s: counts in/id/mv/rp/abs/lvs/reu/sci %u/%u/%u/%u/%u/%u/%u/%u, oracle %u/%u/%u/%u/%u/%u/%u/0", g->name,
              st.regs_in, st.regs_identical, st.regs_moved, st.regs_repacked, st.regs_dropped_absent,
              st.regs_dropped_legacy_vs, st.regs_reused, st.regs_scissor_adjusted, g->regs_in, g->identical, g->moved,
              g->repacked, g->dropped_absent, g->dropped_legacy, g->regs_reused);
        if (s != XLAT12_OK) {
            CHECK(st.err_reg_addr == g->err_addr, "%s: err_addr 0x%x, oracle 0x%x", g->name, st.err_reg_addr, g->err_addr);
            printf("  gfx11 golden %-30s %-20s at 0x%05x (%s) after %u regs\n", g->name, sn, st.err_reg_addr,
                   st.err_name ? st.err_name : "-", st.regs_in);
            continue;
        }
        CHECK(n == g->exp_len, "%s: out %u dwords, oracle %u", g->name, n, g->exp_len);
        uint32_t first_bad = UINT32_MAX;
        for (uint32_t i = 0; i < n && i < g->exp_len; i++)
            if (g_out[i] != g->exp[i]) { first_bad = i; break; }
        CHECK(first_bad == UINT32_MAX, "%s: dword %u = 0x%08x, oracle 0x%08x", g->name, first_bad,
              first_bad == UINT32_MAX ? 0 : g_out[first_bad], first_bad == UINT32_MAX ? 0 : g->exp[first_bad]);
        CHECK(st.set_packets_out == g->set_packets_out && st.set_runs_split == g->set_runs_split,
              "%s: set_out/split %u/%u, oracle %u/%u", g->name, st.set_packets_out, st.set_runs_split,
              g->set_packets_out, g->set_runs_split);
        CHECK((ctx.vs_mode == XLAT12_VS_NGG) == (g->ngg_out != 0), "%s: ngg_out", g->name);
        printf("  gfx11 golden %-30s OK  in %4u -> out %4u dw; regs %u: id %u mv %u rp %u, dropped absent %u legacy-VS %u\n",
               g->name, g->in_len, n, st.regs_in, st.regs_identical, st.regs_moved, st.regs_repacked,
               st.regs_dropped_absent, st.regs_dropped_legacy_vs);
    }
}

/* Every gfx10.3 SET_* register, alone in a SET packet: the gfx11 address and value the oracle expects. */
static void test_every_register(void)
{
    unsigned bad = 0;
    for (size_t k = 0; k < REGCASE11_COUNT; k++) {
        const RegCase11 *c = &kRegCases11[k];
        uint32_t op = c->g10 >= 0x30000u ? 0x79u : c->g10 >= 0x28000u ? 0x69u : 0x76u;
        uint32_t base = op == 0x79u ? 0x30000u : op == 0x69u ? 0x28000u : 0x0B000u;
        uint32_t in[3] = { 0xC0010000u | (op << 8), (c->g10 - base) >> 2, c->in };
        xlat12_ctx ctx; ctx.vs_mode = XLAT12_VS_NGG;
        xlat12_stats st; uint32_t n = 0;
        xlat12_status s = xlat12_translate_level(XLAT12_LEVEL_GFX11, &ctx, in, 3, g_out, CAP, &n, &st);
        int ok = strcmp(xlat12_status_name(s), c->status) == 0 &&
                 (s != XLAT12_OK || (n == c->exp_len && (n == 0 || memcmp(g_out, c->exp, sizeof c->exp) == 0)));
        if (!ok && bad++ < 20)
            printf("FAIL %s 0x%05x in 0x%08x: %s n %u [%08x %08x %08x], oracle %s n %u [%08x %08x %08x]\n", c->name,
                   c->g10, c->in, xlat12_status_name(s), n, g_out[0], g_out[1], g_out[2], c->status, c->exp_len,
                   c->exp[0], c->exp[1], c->exp[2]);
    }
    CHECK(bad == 0, "%u of %u register cases differ from the oracle", bad, (unsigned)REGCASE11_COUNT);
    printf("  gfx11 register cases: %u, %u differ\n", (unsigned)REGCASE11_COUNT, bad);
}

static void test_level_arg(void)
{
    const uint32_t in[3] = { 0xC0016900u, 0x00000000u, 0x00000000u };   /* DB_RENDER_CONTROL = 0 */
    uint32_t n = 0;
    CHECK(xlat12_translate_level(10, 0, in, 3, g_out, CAP, &n, 0) == XLAT12_ERR_ARG, "gfx_level 10 accepted");
    CHECK(xlat12_translate_level(13, 0, in, 3, g_out, CAP, &n, 0) == XLAT12_ERR_ARG, "gfx_level 13 accepted");
    CHECK(xlat12_translate_level(XLAT12_LEVEL_GFX11, 0, in, 3, g_out, CAP, &n, 0) == XLAT12_OK && n == 3,
          "gfx11 DB_RENDER_CONTROL");
    /* gfx12 by level == the old entry point */
    uint32_t o2[8], n2 = 0;
    CHECK(xlat12_translate_level(XLAT12_LEVEL_GFX12, 0, in, 3, g_out, CAP, &n, 0) ==
          xlat12_translate(0, in, 3, o2, 8, &n2, 0) && n == n2 && memcmp(g_out, o2, n * 4) == 0,
          "GFX12 level differs from xlat12_translate");
    /* the gfx12 inclusive-scissor rule is not applied on gfx11: BR 0 stays (gfx12 refuses it) */
    const uint32_t br0[3] = { 0xC0016900u, (0x28208u - 0x28000u) >> 2, 0x00000000u };  /* PA_SC_WINDOW_SCISSOR_BR */
    CHECK(xlat12_translate(0, br0, 3, g_out, CAP, &n, 0) == XLAT12_ERR_SCISSOR, "gfx12 BR 0 not refused");
    CHECK(xlat12_translate_level(XLAT12_LEVEL_GFX11, 0, br0, 3, g_out, CAP, &n, 0) == XLAT12_OK && n == 3 &&
          g_out[2] == 0, "gfx11 BR 0 not passed through");
}

int main(void)
{
    test_golden();
    test_every_register();
    test_level_arg();
    printf("xlat12 gfx11 tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
