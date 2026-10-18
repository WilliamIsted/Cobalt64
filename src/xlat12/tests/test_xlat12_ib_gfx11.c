/* Host tests for the gfx11 IB layer of src/xlat12 (stage 3 step 7, sub-step X3). No Apple fixture needed.
 *   1. xlat12_ib_translate_level / xlat12_ib_census_run_level (GFX11) against gen/gen_ib_golden_gfx11.py's oracle
 *      (tests/golden_ib_gfx11.h), whose every output register is checked against gfx11.json + gc_11_0_0_offset.h.
 *   2. The draw policy under XLAT12_EXTRA_GFX11 on a synthetic legacy-VS draw: each gfx11 destination written with
 *      the expected value (addresses written out here from the sources, not taken from xlat12_gfx11.h), no gfx12-only
 *      destination, and every SET register in kGfx11RegAddrs (the register-header check).
 *   3. The level arguments of the new entry points, and gfx12 equivalence of the unsuffixed forms.
 * Build and run: make -C src/xlat12 test. */
#include <stdio.h>
#include <string.h>
#include "xlat12.h"
#include "xlat12_ib.h"
#include "golden_ib_gfx11.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define CAP 16384u
static uint32_t g_out[CAP], g_out2[CAP];

static void test_ib_golden(void)
{
    for (size_t v = 0; v < GOLDEN_IB11_COUNT; v++) {
        const GoldenIb11 *g = &kGoldenIb11[v];
        xlat12_ctx ctx; ctx.vs_mode = g->ngg_in ? XLAT12_VS_NGG : XLAT12_VS_UNKNOWN;
        xlat12_stats st; uint32_t n = 0, eop = 0;
        const uint32_t s = xlat12_ib_translate_level(XLAT12_LEVEL_GFX11, &ctx, g->in, g->in_len, g_out, CAP, &n, &st, &eop);
        const char *sn = xlat12_ib_status_name(s);
        CHECK(strcmp(sn, g->status) == 0, "%s: status %s, oracle %s (op 0x%x reg 0x%x)", g->name, sn, g->status, eop, st.err_reg_addr);
        if (s == 0u) {
            CHECK(n == g->exp_len && memcmp(g_out, g->exp, n * 4u) == 0, "%s: output differs from the oracle (%u vs %u dw)",
                  g->name, n, g->exp_len);
        }
        xlat12_ib_census c;
        xlat12_ib_census_run_level(XLAT12_LEVEL_GFX11, g->in, g->in_len, &c);
        int cls_ok = 1;
        for (int k = 0; k < 7; k++) cls_ok &= c.reg_cls[k] == g->c_cls[k];
        CHECK(cls_ok && c.regs == g->c_regs && c.cs_proven_regs == g->c_cs_proven && c.reg_operand_bad == g->c_operand_bad &&
              c.unlisted == g->c_unlisted && c.first_unknown_reg == g->c_first_unknown && c.packets == g->c_packets,
              "%s: census regs %u cls %u/%u/%u/%u/%u/%u cs %u bad %u unl %u unk 0x%x pk %u; oracle regs %u cls %u/%u/%u/%u/%u/%u "
              "cs %u bad %u unl %u unk 0x%x pk %u", g->name, c.regs, c.reg_cls[0], c.reg_cls[1], c.reg_cls[2], c.reg_cls[3],
              c.reg_cls[4], c.reg_cls[5], c.cs_proven_regs, c.reg_operand_bad, c.unlisted, c.first_unknown_reg, c.packets,
              g->c_regs, g->c_cls[0], g->c_cls[1], g->c_cls[2], g->c_cls[3], g->c_cls[4], g->c_cls[5], g->c_cs_proven,
              g->c_operand_bad, g->c_unlisted, g->c_first_unknown, g->c_packets);
        /* the gfx12 form of the new entry point is the old one, byte for byte */
        xlat12_ctx c1, c2; c1.vs_mode = c2.vs_mode = ctx.vs_mode = g->ngg_in ? XLAT12_VS_NGG : XLAT12_VS_UNKNOWN;
        uint32_t n1 = 0, n2 = 0, e1 = 0, e2 = 0;
        const uint32_t s1 = xlat12_ib_translate(&c1, g->in, g->in_len, g_out, CAP, &n1, 0, &e1);
        const uint32_t s2 = xlat12_ib_translate_level(XLAT12_LEVEL_GFX12, &c2, g->in, g->in_len, g_out2, CAP, &n2, 0, &e2);
        CHECK(s1 == s2 && n1 == n2 && e1 == e2 && memcmp(g_out, g_out2, n1 * 4u) == 0, "%s: GFX12 level != xlat12_ib_translate", g->name);
        printf("  gfx11 IB %-40s %-20s in %4u -> out %4u; census regs %u (id %u mv %u rp %u abs %u lvs %u unk %u) cs-proven %u\n",
               g->name, sn, g->in_len, s == 0u ? n : 0u, c.regs, c.reg_cls[0], c.reg_cls[1], c.reg_cls[2], c.reg_cls[3],
               c.reg_cls[4], c.reg_cls[5], c.cs_proven_regs);
    }
}

/* ---- the draw policy ---------------------------------------------------------------------------------------- */
#define HDR(op, n) (0xC0000000u | (((uint32_t)(n) - 1u) << 16) | ((uint32_t)(op) << 8))
static uint32_t g_in[512];
static uint32_t put_set(uint32_t at, uint32_t op, uint32_t base, uint32_t addr, const uint32_t *v, uint32_t nv)
{
    g_in[at++] = HDR(op, nv + 1u);
    g_in[at++] = (addr - base) >> 2;
    for (uint32_t k = 0; k < nv; k++) g_in[at++] = v[k];
    return at;
}
#define CTX(addr, ...) do { const uint32_t v_[] = { __VA_ARGS__ }; n = put_set(n, 0x69u, 0x28000u, addr, v_, sizeof v_ / 4u); } while (0)
#define SH(addr, ...)  do { const uint32_t v_[] = { __VA_ARGS__ }; n = put_set(n, 0x76u, 0x0B000u, addr, v_, sizeof v_ / 4u); } while (0)

/* A gfx10.3 legacy-VS draw. attrib3 / br replace CB_COLOR0_ATTRIB3 and PA_SC_WINDOW_SCISSOR_BR. */
static uint32_t build_draw(uint32_t attrib3, uint32_t br)
{
    uint32_t n = 0;
    g_in[n++] = 0xC0012800u; g_in[n++] = 0x80000000u; g_in[n++] = 0x80000000u;   /* the proven CONTEXT_CONTROL */
    for (int k = 0; k < 8; k++) SH(0xb1c4u, 0, 0, 0, 0, 0);   /* zero SPI_SHADER_USER_ACCUM_VS_0..3 + 0xb1d4: dropped, room for the blocks */
    CTX(0x28b54u, 0x00000000u);                                /* VGT_SHADER_STAGES_EN: plain VS */
    SH(0xb120u, 0x00001234u, 0x00000000u, 0x0000AAAAu, 3u << 1); /* PGM_LO/HI_VS, RSRC1_VS, RSRC2_VS (USER_SGPR 3) */
    SH(0xb130u, 0x5555u, 0x6666u);                             /* USER_DATA_VS_0/1 */
    SH(0xb420u, 0u, 0u);                                       /* PGM_LO/HI_HS: unused, the ring-offsets HS pair */
    SH(0xb028u, 0x0000BBBBu, 2u << 1);                         /* RSRC1/2_PS */
    CTX(0x286ccu, 0x1u, 0x1u);                                 /* SPI_PS_INPUT_ENA / ADDR */
    CTX(0x28708u, 0u);                                         /* SPI_SHADER_IDX_FORMAT */
    CTX(0x28a6cu, 0u);                                         /* VGT_GS_OUT_PRIM_TYPE */
    CTX(0x286c4u, 0x77u);                                      /* SPI_VS_OUT_CONFIG: Apple's, replaced by the profile's */
    CTX(0x28ee0u, attrib3);                                    /* CB_COLOR0_ATTRIB3 */
    CTX(0x28040u, (22u << 4) | 3u);                            /* DB_Z_INFO SW_MODE 22 4KB_D_X */
    CTX(0x28208u, br);                                         /* PA_SC_WINDOW_SCISSOR_BR */
    g_in[n++] = HDR(0x2Du, 2u); g_in[n++] = 3u; g_in[n++] = 2u;   /* DRAW_INDEX_AUTO */
    return n;
}

static xlat12_draw_profile profile(void)
{
    xlat12_draw_profile p;
    memset(&p, 0, sizeof p);
    p.vs_rsrc1_gs = 0x11110000u; p.vs_rsrc2_gs = 0x22000000u; p.vs_stages_en = 0x00200000u;
    p.ps_rsrc1 = 0x33330000u; p.ps_rsrc2 = 0x44000000u; p.ps_input_ena = 0x2u; p.ps_input_addr = 0x3u;
    p.ge_cntl = 0x5555u; p.ge_max_output = 0x40u; p.rsrc4_gs = 0x66u; p.rsrc4_ps = 0x77u; p.gs_out_config_ps = 0x8u;
    p.binner_cntl0 = 0x9u; p.ngg_subgrp_cntl = 0xAu; p.gs_max_vert_out = 1u; p.gs_instance_cnt = 0u;
    p.gs_out_prim_type = 2u; p.spi_shader_idx_format = 1u;
    return p;
}

static xlat12_draw_extra extra(uint32_t flags)
{
    xlat12_draw_extra e;
    memset(&e, 0, sizeof e);
    e.ring_va = 0x100000000ull; e.rsrc3_gs = 0xfffffdfdu; e.gs_sgpr0_va = 0x200003000ull; e.flags = flags;
    return e;
}

static int in_gfx11_set(uint32_t a)
{
    uint32_t lo = 0, hi = (uint32_t)GFX11_REG_ADDRS_COUNT;
    while (lo < hi) {
        const uint32_t m = lo + ((hi - lo) >> 1);
        if (kGfx11RegAddrs[m] == a) return 1;
        if (kGfx11RegAddrs[m] < a) lo = m + 1u; else hi = m;
    }
    return 0;
}

/* every SET register of out[0..n) (NOP bodies skipped) is a gfx11 register; returns the first that is not, else 0 */
static uint32_t first_non_gfx11(const uint32_t *o, uint32_t n)
{
    for (uint32_t i = 0; i < n; ) {
        const uint32_t h = o[i];
        if (h == XLAT12_IB_NOP || (h >> 30) == 2u) { i++; continue; }
        const uint32_t op = (h >> 8) & 0xFFu, l = ((h >> 16) & 0x3FFFu) + 2u;
        const uint32_t base = op == 0x69u ? 0xa000u : (op == 0x76u || op == 0x9Bu) ? 0x2c00u : (op == 0x79u || op == 0x7Au) ? 0xc000u : 0u;
        if (base)
            for (uint32_t k = 0; k + 2u < l; k++) {
                const uint32_t a = (base + (o[i + 1] & 0xFFFFu) + k) << 2;
                if (!in_gfx11_set(a)) return a;
            }
        i += l;
    }
    return 0;
}

static uint32_t val_at(const uint32_t *o, uint32_t n, uint32_t a, int *found)
{
    uint32_t v = 0xDEADBEEFu;
    *found = xlat12_ib_find_set(o, n, a, &v);
    return v;
}

static void test_draw_gfx11(void)
{
    const xlat12_draw_profile pf = profile();
    xlat12_draw_extra ex = extra(XLAT12_EXTRA_GFX11);
    xlat12_draw_stats ds;
    uint32_t n = build_draw(9u << 14, 0x00400040u), on = 0;
    uint32_t s = xlat12_ib_translate_draw_ex(&pf, &ex, g_in, n, g_out, &on, &ds);
    CHECK(s == 0u, "gfx11 draw: %s (reg 0x%x op 0x%x at %u)", xlat12_ib_status_name(s), ds.err_reg, ds.err_op, ds.err_in_dword);
    if (s != 0u) return;
    CHECK(ds.draws == 1u, "draws %u", ds.draws);
    /* expected (address, value): addresses from Mesa gfx11.json / RADV GFX11 (see xlat12_gfx11.h for the citations) */
    const struct { uint32_t a, v; const char *what; } want[] = {
        { 0xb320u, 0x1234u, "SPI_SHADER_PGM_LO_ES <- PGM_LO_VS" },     { 0xb324u, 0u, "SPI_SHADER_PGM_HI_ES <- PGM_HI_VS" },
        { 0xb228u, 0x11110000u, "RSRC1_GS (profile)" },                { 0xb22cu, 0x22000000u | (3u << 1), "RSRC2_GS (profile | USER_SGPR)" },
        { 0xb230u, 0x5555u, "USER_DATA_GS_0 <- VS_0" },                { 0xb234u, 0x6666u, "USER_DATA_GS_1 <- VS_1" },
        { 0xb028u, 0x33330000u, "RSRC1_PS (profile)" },                { 0xb02cu, 0x44000000u | (2u << 1), "RSRC2_PS" },
        { 0x286ccu, 0x2u, "SPI_PS_INPUT_ENA" },                        { 0x286d0u, 0x3u, "SPI_PS_INPUT_ADDR" },
        { 0x28708u, 1u, "SPI_SHADER_IDX_FORMAT" },                     { 0x30998u, 2u, "VGT_GS_OUT_PRIM_TYPE (uconfig)" },
        { 0x28b54u, 0x00200000u, "VGT_SHADER_STAGES_EN" },
        { 0x287fcu, 0x40u, "GE_MAX_OUTPUT_PER_SUBGROUP" },            { 0xb004u, 0x77u, "SPI_SHADER_PGM_RSRC4_PS" },
        { 0x286c4u, 0x8u, "SPI_VS_OUT_CONFIG (profile, not Apple's)" }, { 0xb204u, 0x66u, "SPI_SHADER_PGM_RSRC4_GS" },
        { 0x3096cu, 0x5555u, "GE_CNTL" },                              { 0x28b38u, 1u, "VGT_GS_MAX_VERT_OUT" },
        { 0x28b90u, 0u, "VGT_GS_INSTANCE_CNT" },                       { 0x28b4cu, 0xAu, "GE_NGG_SUBGRP_CNTL" },
        { 0x31110u, 0x12355123u, "SPI_GS_THROTTLE_CNTL1" },            { 0x31114u, 0x1544Du, "SPI_GS_THROTTLE_CNTL2" },
        { 0x31118u, 0x10000u, "SPI_ATTRIBUTE_RING_BASE" },             { 0x3111cu, 0x00030015u, "SPI_ATTRIBUTE_RING_SIZE" },
        { 0xb21cu, 0xfffffdfdu, "SPI_SHADER_PGM_RSRC3_GS" },
        { 0xb220u, 0x00003000u, "ring-offsets GS lo (PGM_LO_GS)" },    { 0xb224u, 0x2u, "ring-offsets GS hi (PGM_HI_GS)" },
        { 0xb420u, 0x00003000u, "ring-offsets HS lo, in place" },      { 0xb424u, 0x2u, "ring-offsets HS hi, in place" },
        { 0x28ee0u, 9u << 14, "CB_COLOR0_ATTRIB3 SW_MODE 9 kept" },    { 0x28040u, (22u << 4) | 3u, "DB_Z_INFO SW_MODE 22 kept" },
        { 0x28208u, 0x00400040u, "PA_SC_WINDOW_SCISSOR_BR exclusive, unchanged" },
    };
    for (size_t k = 0; k < sizeof want / sizeof want[0]; k++) {
        int f = 0; const uint32_t v = val_at(g_out, on, want[k].a, &f);
        CHECK(f && v == want[k].v, "gfx11 draw %s 0x%05x: %s 0x%08x, want 0x%08x", want[k].what, want[k].a, f ? "got" : "absent", v, want[k].v);
    }
    /* gfx12-only destinations (or gfx12 places of gfx11 registers) must not appear */
    static const uint32_t never[] = { 0xb218u, 0x28a98u, 0x2865cu, 0x28660u, 0x28648u, 0xb0c4u, 0x28b3cu, 0x309a0u, 0x309a4u,
                                      0x309a8u, 0x309acu, 0x28068u, 0x28018u, 0x2801cu, 0xb410u, 0xb414u, 0xb210u, 0xb214u,
                                      0x28c7cu, 0xb01cu, 0x28bbcu };
    for (size_t k = 0; k < sizeof never / sizeof never[0]; k++) {
        int f = 0; (void)val_at(g_out, on, never[k], &f);
        CHECK(!f, "gfx11 draw writes the gfx12 destination 0x%05x", never[k]);
    }
    const uint32_t bad = first_non_gfx11(g_out, on);
    CHECK(bad == 0u, "gfx11 draw writes 0x%05x, not a gfx11 register in gfx11.json + gc_11_0_0_offset.h", bad);
    uint32_t ba = 0, bo = 0;
    CHECK(xlat12_ib_draw_verify_ex(g_out, on, XLAT12_EXTRA_GFX11, &ba, &bo) == 0u, "gfx11 verifier refuses 0x%x op 0x%x", ba, bo);
    CHECK(xlat12_ib_draw_verify(g_out, on, &ba, &bo) != 0u, "the gfx12 verifier accepts gfx11 output");
    printf("  gfx11 draw: %u dw in place, %zu destinations checked, gfx11 verifier clean, ring %u dw, synth blocks %u\n",
           on, sizeof want / sizeof want[0], ds.ring_dwords, ds.synth_blocks);

    /* the same stream at gfx12 keeps its gfx12 output (spot checks; the full gfx12 draw suite needs the Apple fixtures) */
    xlat12_draw_extra e12 = extra(0u);
    s = xlat12_ib_translate_draw_ex(&pf, &e12, g_in, n, g_out2, &on, &ds);
    int f = 0;
    CHECK(s == 0u && val_at(g_out2, on, 0xb224u, &f) == 0x1234u && f && val_at(g_out2, on, 0x28a98u, &f) == 0x00200000u && f &&
          val_at(g_out2, on, 0x28208u, &f) == 0x003f003fu && f && val_at(g_out2, on, 0x3111cu, &f) == 0x00020015u && f,
          "gfx12 draw of the same stream changed (%s)", xlat12_ib_status_name(s));
    CHECK(s == 0u && xlat12_ib_draw_verify(g_out2, on, &ba, &bo) == 0u, "gfx12 verifier refuses gfx12 output 0x%x", ba);
    CHECK(s == 0u && xlat12_ib_draw_verify_ex(g_out2, on, XLAT12_EXTRA_GFX11, &ba, &bo) != 0u, "the gfx11 verifier accepts gfx12 output");

    /* SW_MODE refusals and the scissor BR 0 */
    static const uint32_t refused[] = { 1u, 23u, 28u, 31u, 12u };
    for (size_t k = 0; k < sizeof refused / sizeof refused[0]; k++) {
        n = build_draw(refused[k] << 14, 0x00400040u);
        s = xlat12_ib_translate_draw_ex(&pf, &ex, g_in, n, g_out, &on, &ds);
        CHECK(s == XLAT12_IB_ERR_SWMODE && ds.err_reg == 0x28ee0u && ds.err_op == refused[k], "gfx11 draw SW_MODE %u: %s err_op %u",
              refused[k], xlat12_ib_status_name(s), ds.err_op);
    }
    n = build_draw(9u << 14, 0u);
    s = xlat12_ib_translate_draw_ex(&pf, &ex, g_in, n, g_out, &on, &ds);
    CHECK(s == 0u && val_at(g_out, on, 0x28208u, &f) == 0u && f, "gfx11 draw BR 0: %s", xlat12_ib_status_name(s));
    s = xlat12_ib_translate_draw_ex(&pf, &e12, g_in, n, g_out, &on, &ds);
    CHECK(s == XLAT12_ERR_SCISSOR, "gfx12 draw BR 0: %s", xlat12_ib_status_name(s));

    /* switches: the three NGG-word ones combine with GFX11, the gfx12-state ones refuse */
    n = build_draw(9u << 14, 0x00400040u);
    static const uint32_t ok_with[] = { XLAT12_EXTRA_APPLE_IDXFMT, XLAT12_EXTRA_APPLE_OUTPRIM, XLAT12_EXTRA_SYNTH_IDXPRIM };
    for (size_t k = 0; k < sizeof ok_with / sizeof ok_with[0]; k++) {
        xlat12_draw_extra e = extra(XLAT12_EXTRA_GFX11 | ok_with[k]);
        s = xlat12_ib_translate_draw_ex(&pf, &e, g_in, n, g_out, &on, &ds);
        CHECK(s == 0u && first_non_gfx11(g_out, on) == 0u && xlat12_ib_draw_verify_ex(g_out, on, XLAT12_EXTRA_GFX11, &ba, &bo) == 0u,
              "GFX11 | 0x%x: %s / bad reg 0x%x", ok_with[k], xlat12_ib_status_name(s), first_non_gfx11(g_out, on));
    }
    static const uint32_t refuse_with[] = { XLAT12_EXTRA_PREAMBLE, XLAT12_EXTRA_RASTER, XLAT12_EXTRA_INLINE_DESC,
                                            XLAT12_EXTRA_PAIR_PRE, XLAT12_EXTRA_READSET, XLAT12_EXTRA_PWS, XLAT12_EXTRA_TBLCACHE };
    for (size_t k = 0; k < sizeof refuse_with / sizeof refuse_with[0]; k++) {
        xlat12_draw_extra e = extra(XLAT12_EXTRA_GFX11 | refuse_with[k]);
        s = xlat12_ib_translate_draw_ex(&pf, &e, g_in, n, g_out, &on, &ds);
        CHECK(s == XLAT12_ERR_ARG, "GFX11 | 0x%x accepted: %s", refuse_with[k], xlat12_ib_status_name(s));
    }
}

static void test_levels(void)
{
    const uint32_t in[3] = { 0xC0016900u, 0u, 0u };
    uint32_t n = 0, eop = 0;
    CHECK(xlat12_ib_translate_level(13u, 0, in, 3, g_out, CAP, &n, 0, &eop) == XLAT12_ERR_ARG, "ib level 13 accepted");
    CHECK(xlat12_ib_translate_level(10u, 0, in, 3, g_out, CAP, &n, 0, &eop) == XLAT12_ERR_ARG, "ib level 10 accepted");
    xlat12_ib_census c;
    xlat12_ib_census_run_level(13u, in, 3, &c);
    CHECK(c.regs == 1u && c.reg_cls[XLAT12_CLS_UNKNOWN] == 1u, "census level 13 classified a register");
    CHECK((xlat12_ib_selftest_level(XLAT12_LEVEL_GFX11) & XLAT12_IB_SELFTEST_TABLE) == XLAT12_IB_SELFTEST_TABLE,
          "gfx11 selftest bits 0x%x", xlat12_ib_selftest_level(XLAT12_LEVEL_GFX11));
    CHECK(xlat12_ib_selftest_level(XLAT12_LEVEL_GFX12) == xlat12_ib_selftest(), "gfx12 selftest by level differs");
    CHECK((xlat12_ib_selftest() & XLAT12_IB_SELFTEST_TABLE) == XLAT12_IB_SELFTEST_TABLE, "gfx12 selftest table bits 0x%x", xlat12_ib_selftest());
    CHECK(xlat12_ib_selftest_level(13u) == 0u, "selftest level 13 passed something");
    /* xlat12.c's level introspection */
    CHECK(xlat12_table_len_level(XLAT12_LEVEL_GFX11) == 1828u && xlat12_table_len_level(XLAT12_LEVEL_GFX12) == xlat12_table_len() &&
          xlat12_table_len_level(13u) == 0u, "table_len_level");
    unsigned diff = 0;
    for (uint32_t i = 0; i < xlat12_table_len(); i++) {
        uint32_t g10 = 0, g12 = 0, cls = 0, a = 0, b = 0, c2 = 0; const char *nm = 0, *nm2 = 0;
        xlat12_table_entry(i, &g10, &g12, &cls, &nm);
        xlat12_table_entry_level(XLAT12_LEVEL_GFX12, i, &a, &b, &c2, &nm2);
        uint32_t lg = 0, lc = 0, lg2 = 0, lc2 = 0;
        const int l1 = xlat12_lookup(g10, &lg, &lc), l2 = xlat12_lookup_level(XLAT12_LEVEL_GFX12, g10, &lg2, &lc2);
        if (a != g10 || b != g12 || c2 != cls || nm2 != nm || l1 != l2 || lg != lg2 || lc != lc2 ||
            xlat12_repack_value(g10, 0x5A5AA5A5u ^ g10) != xlat12_repack_value_level(XLAT12_LEVEL_GFX12, g10, 0x5A5AA5A5u ^ g10)) diff++;
    }
    CHECK(diff == 0u, "%u gfx12 table rows differ between the plain and _level introspection", diff);
    uint32_t d = 0, cl = 0;
    CHECK(xlat12_lookup_level(XLAT12_LEVEL_GFX11, 0x28ee0u, &d, &cl) && d == 0x28ee0u && cl == XLAT12_CLS_FIELD_REPACK,
          "gfx11 CB_COLOR0_ATTRIB3 0x%x cls %u", d, cl);
    CHECK(xlat12_lookup_level(XLAT12_LEVEL_GFX11, 0x28a6cu, &d, &cl) && d == 0x30998u, "gfx11 VGT_GS_OUT_PRIM_TYPE 0x%x", d);
    CHECK(!xlat12_lookup_level(13u, 0x28000u, &d, &cl) && xlat12_repack_value_level(13u, 0x28ee0u, 0x1234u) == 0x1234u &&
          !xlat12_table_entry_level(13u, 0, 0, 0, 0, 0) && !xlat12_table_entry_level(XLAT12_LEVEL_GFX11, 1828u, 0, 0, 0, 0),
          "level / index bounds");
}

int main(void)
{
    test_ib_golden();
    test_draw_gfx11();
    test_levels();
    printf("xlat12 gfx11 IB tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
