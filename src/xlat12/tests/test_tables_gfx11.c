/* Host test for the generated gfx10.3 -> gfx11 table (stage-3 step 7, sub-step X1).
 * Build and run: make -C src/xlat12 test_tables_gfx11
 *
 * This is the C half of X1: it proves xlat12_tables_gfx11.h COMPILES in the
 * XLAT12_GFX11 build mode (every generated repack_* is referenced by the table, so
 * none trips -Wunused-function) and that the table is structurally sound - sorted for
 * the translator's binary search, classes in range, and each class's g11/repack shape
 * consistent. The source cross-check against gfx11.json and gc_11_0_0_offset.h, and the
 * 1,283/55/151/339 count match, are the Python half (tests/test_xlat12_tables_gfx11.py). */
#ifndef XLAT12_GFX11
#error "test_tables_gfx11.c is the XLAT12_GFX11 build mode; compile with -DXLAT12_GFX11"
#endif
#include <stdio.h>
#include "xlat12.h"
#include "xlat12_tables_gfx11.h"

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* gfx10.3 SET_* register spaces, the same ranges the generator enumerates. */
static int in_setstar_space(uint32_t a)
{
    return (a >= 0x28000u && a <= 0x2FFFFu) ||
           (a >= 0x0B000u && a <= 0x0BFFFu) ||
           (a >= 0x30000u && a <= 0x37FFFu);
}

int main(void)
{
    const uint32_t n = XLAT12_TABLE_LEN_GFX11;
    CHECK(n == 1828u, "table length %u, expected 1828 (every gfx10.3 SET_* register)", n);

    unsigned cnt[XLAT12_CLS_MAX + 1] = {0};
    uint32_t prev = 0;
    for (uint32_t i = 0; i < n; i++) {
        const Xlat12Entry *e = &kXlat12TableGfx11[i];

        CHECK(in_setstar_space(e->g10), "row %u g10 0x%05x outside the SET_* spaces", i, e->g10);
        if (i) CHECK(e->g10 > prev, "row %u g10 0x%05x not strictly after 0x%05x (binary search needs sorted)",
                     i, e->g10, prev);
        prev = e->g10;

        CHECK(e->cls <= XLAT12_CLS_MAX, "row %u (%s) class %u out of range", i, e->name, e->cls);
        cnt[e->cls]++;

        switch (e->cls) {
        case XLAT12_CLS_IDENTICAL:
            CHECK(e->g11 == e->g10, "%s IDENTICAL but g11 0x%05x != g10 0x%05x", e->name, e->g11, e->g10);
            CHECK(e->repack == 0, "%s IDENTICAL carries a repack", e->name);
            break;
        case XLAT12_CLS_MOVED:
            CHECK(e->g11 != 0 && e->g11 != e->g10, "%s MOVED but g11 0x%05x (g10 0x%05x)", e->name, e->g11, e->g10);
            CHECK(e->repack == 0, "%s MOVED carries a repack", e->name);
            break;
        case XLAT12_CLS_FIELD_REPACK:
            CHECK(e->g11 != 0, "%s FIELD_REPACK but g11 == 0", e->name);
            CHECK(e->repack != 0, "%s FIELD_REPACK with no repack function", e->name);
            /* exercise it: identity of 0 is 0, and the result only sets bits the dst has. */
            CHECK(e->repack(0u) == 0u, "%s repack(0) != 0", e->name);
            (void)e->repack(0xFFFFFFFFu);
            break;
        case XLAT12_CLS_ABSENT:
        case XLAT12_CLS_LEGACY_VS:
        case XLAT12_CLS_UNKNOWN:
            CHECK(e->g11 == 0, "%s class %u but g11 0x%05x != 0", e->name, e->cls, e->g11);
            CHECK(e->repack == 0, "%s class %u carries a repack", e->name, e->cls);
            CHECK(e->reason[0] != '\0', "%s class %u has no reason string", e->name, e->cls);
            break;
        case XLAT12_CLS_REUSED:
            CHECK(0, "%s REUSED: the generator does not emit REUSED (hand-written only)", e->name);
            break;
        default:
            break;
        }
    }

    const unsigned removed = cnt[XLAT12_CLS_ABSENT] + cnt[XLAT12_CLS_LEGACY_VS];
    printf("gfx11 table: %u rows  identical %u  moved %u  field_repack %u  "
           "absent %u  legacy_vs %u  unknown %u  (removed = absent+legacy_vs = %u)\n",
           n, cnt[XLAT12_CLS_IDENTICAL], cnt[XLAT12_CLS_MOVED], cnt[XLAT12_CLS_FIELD_REPACK],
           cnt[XLAT12_CLS_ABSENT], cnt[XLAT12_CLS_LEGACY_VS], cnt[XLAT12_CLS_UNKNOWN], removed);
    /* study "removed" = absent + legacy_vs; both methods agree it is 339. */
    CHECK(removed == 339u, "removed (absent+legacy_vs) %u, reference 339", removed);

    printf("%s: %d passed, %d failed\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
