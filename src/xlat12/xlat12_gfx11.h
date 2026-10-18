/* xlat12_gfx11.h - stage 3 step 7 X3: the hand-written gfx11 constants of the IB layer (xlat12_ib.c) and of the
 * SW_MODE rule (xlat12.c). Not generated. tests/test_xlat12_ib_gfx11.py checks every XLAT12_G11_* address below
 * against Mesa src/amd/registers/gfx11.json AND Linux gc_11_0_0_offset.h (byte = (dword + base) * 4, base 0x1260
 * for BASE_IDX 0, 0xa000 for BASE_IDX 1), and the SW_MODE table against addrlib's two SwizzleModeTables.
 * Sources: Mesa bffd9194 (registers JSON identical to 4519cc56), Linux 3857c2fe.
 *
 * Pure data, header-only, so xlat12.c and xlat12_ib.c share it without a new exported symbol. */
#ifndef XLAT12_GFX11_H
#define XLAT12_GFX11_H
#include <stdint.h>

/* ---- SW_MODE: gfx10.3 -> gfx11 -------------------------------------------------------------------------------
 * Both generations write addrlib's AddrSwizzleMode value raw into the register field: ac_descriptors.c:1500-1515
 * (CB_COLOR_ATTRIB3.COLOR_SW_MODE = surf->u.gfx9.swizzle_mode on gfx11 and on gfx10) and :1026-1032 (DB_Z_INFO /
 * DB_STENCIL_INFO SW_MODE, the same for every gfx9+ level). The numbering is addrtypes.h:227-275, shared by both.
 * So a mode keeps its number; what differs is which modes exist. A mode is mapped when it is valid in BOTH
 * Gfx10Lib::SwizzleModeTable (gfx10addrlib.cpp:51-92) and Gfx11Lib::SwizzleModeTable (gfx11addrlib.cpp:50-92):
 * those rows are the same flags on both (block size, Z/S/D/R, XOR, T), so the identity is by name, not by value
 * coincidence. Refused (absent below, XLAT12_ERR_SWMODE / XLAT12_IB_ERR_SWMODE):
 *   1  ADDR_SW_256B_S   valid on gfx10, Reserved on gfx11
 *   23 ADDR_SW_4KB_R_X  valid on gfx10, Reserved on gfx11
 *   28 ADDR_SW_VAR_Z_X  gfx10 VAR block (16K * num_pipes, gfx10addrlib.cpp:971-976); gfx11 reuses 28 for 256KB_Z_X,
 *                       a fixed 256 KiB block with its own patterns, so the same number names a different layout
 *   31 ADDR_SW_VAR_R_X  the same, gfx11 256KB_R_X
 *   every other value   Reserved on gfx10 (32 LINEAR_GENERAL does not fit the 5-bit field) */
static const uint8_t kXlat12SwModeG10ToG11[][2] = {
    { 0u, 0u },     /* ADDR_SW_LINEAR */
    { 2u, 2u },     /* ADDR_SW_256B_D */
    { 5u, 5u },     /* ADDR_SW_4KB_S */
    { 6u, 6u },     /* ADDR_SW_4KB_D */
    { 9u, 9u },     /* ADDR_SW_64KB_S */
    { 10u, 10u },   /* ADDR_SW_64KB_D */
    { 17u, 17u },   /* ADDR_SW_64KB_S_T */
    { 18u, 18u },   /* ADDR_SW_64KB_D_T */
    { 21u, 21u },   /* ADDR_SW_4KB_S_X */
    { 22u, 22u },   /* ADDR_SW_4KB_D_X */
    { 24u, 24u },   /* ADDR_SW_64KB_Z_X */
    { 25u, 25u },   /* ADDR_SW_64KB_S_X */
    { 26u, 26u },   /* ADDR_SW_64KB_D_X */
    { 27u, 27u },   /* ADDR_SW_64KB_R_X */
};
#define XLAT12_SWMODE_G11_MAP_COUNT 14u

/* gfx11 mode for gfx10 mode `m`, or 0xFFFFFFFF when gfx11 has no counterpart. */
static inline uint32_t xlat12_swmode_g10_to_g11(uint32_t m)
{
    for (uint32_t i = 0; i < XLAT12_SWMODE_G11_MAP_COUNT; i++)
        if (kXlat12SwModeG10ToG11[i][0] == m) return kXlat12SwModeG10ToG11[i][1];
    return 0xFFFFFFFFu;
}

/* ---- the draw policy's gfx11 destinations (xlat12_ib.c d_reg / d_synth / d_rings / d_policy) ----------------
 * Each address is the gfx11 register RADV writes for an NGG vertex stage on GFX11 (cited per line), in place of
 * the gfx12 one d_reg uses. gc_11_0_0_offset.h line numbers in brackets. */
/* the legacy-VS program re-homed onto the merged NGG stage: radv_shader.c:2134 (pgm_lo = PGM_LO_ES below GFX12),
 * :2140 (RSRC4_GS 0xb204 on GFX11), :2143-2144 (RSRC1/2_GS) */
#define XLAT12_G11_SPI_SHADER_PGM_LO_ES          0xb320u   /* [3826] dw 0x1a68 */
#define XLAT12_G11_SPI_SHADER_PGM_HI_ES          0xb324u   /* [3828] dw 0x1a69 */
#define XLAT12_G11_SPI_SHADER_PGM_RSRC1_GS       0xb228u   /* [3744] */
#define XLAT12_G11_SPI_SHADER_PGM_RSRC2_GS       0xb22cu   /* [3746] */
#define XLAT12_G11_SPI_SHADER_PGM_RSRC3_GS       0xb21cu   /* [3738] the GS/ES CU mask d_rings writes (ex->rsrc3_gs) */
#define XLAT12_G11_SPI_SHADER_PGM_RSRC4_GS       0xb204u   /* [3728] */
/* NGG user data: radv_shader_info.c:1000-1001 (is_ngg -> USER_DATA_GS_0 for GFX10+), the same +0x100 as gfx12 */
#define XLAT12_G11_SPI_SHADER_USER_DATA_GS_0     0xb230u   /* [3748] */
#define XLAT12_G11_SPI_SHADER_USER_DATA_GS_31    0xb2acu   /* [3810] */
/* fragment / pipeline state (the gfx12 path moves these; on gfx11 they stay at their gfx10.3 address) */
#define XLAT12_G11_SPI_SHADER_PGM_RSRC4_PS       0xb004u   /* [3638] radv_cmd_buffer.c:4091-4092 (GFX11) */
#define XLAT12_G11_SPI_SHADER_PGM_RSRC1_PS       0xb028u   /* [3648] */
#define XLAT12_G11_SPI_SHADER_PGM_RSRC2_PS       0xb02cu   /* [3650] */
#define XLAT12_G11_SPI_VS_OUT_CONFIG             0x286c4u  /* [5756] radv_cmd_buffer.c:3466 (gfx12 folds it into 0xb0c4) */
#define XLAT12_G11_SPI_PS_INPUT_ENA              0x286ccu  /* [5758] radv_cmd_buffer.c:12735 */
#define XLAT12_G11_SPI_PS_INPUT_ADDR             0x286d0u  /* [5760] */
#define XLAT12_G11_SPI_SHADER_IDX_FORMAT         0x28708u  /* [5774] radv_cmd_buffer.c:3464 */
#define XLAT12_G11_GE_MAX_OUTPUT_PER_SUBGROUP    0x287fcu  /* [5840] radv_cmd_buffer.c:3468 */
#define XLAT12_G11_VGT_GS_MAX_VERT_OUT           0x28b38u  /* [5934] */
#define XLAT12_G11_GE_NGG_SUBGRP_CNTL            0x28b4cu  /* [5936] radv_cmd_buffer.c:3470 */
#define XLAT12_G11_VGT_SHADER_STAGES_EN          0x28b54u  /* [5940] radv_cmd_buffer.c:4200 */
#define XLAT12_G11_VGT_GS_INSTANCE_CNT           0x28b90u  /* [5960] radv_cmd_buffer.c:3460 (gfx12: 0x28b3c) */
#define XLAT12_G11_PA_SC_BINNER_CNTL_0           0x28c44u  /* [6018] */
#define XLAT12_G11_CB_COLOR0_ATTRIB3             0x28ee0u  /* [6172] CB_COLORn_ATTRIB3 = this + 4n, n = 0..7 */
#define XLAT12_G11_DB_Z_INFO                     0x28040u  /* [5178] */
#define XLAT12_G11_DB_STENCIL_INFO               0x28044u  /* [5180] */
#define XLAT12_G11_GE_CNTL                       0x3096cu  /* [7196] radv_cmd_buffer.c:3518 */
#define XLAT12_G11_VGT_GS_OUT_PRIM_TYPE          0x30998u  /* [7216] radv_cmd_buffer.c:3810-3811 (uconfig on GFX11+) */
/* the user-data windows the policy passes through (identical on gfx10.3 and gfx11) */
#define XLAT12_G11_SPI_SHADER_USER_DATA_PS_0     0xb030u   /* [3652] */
#define XLAT12_G11_SPI_SHADER_USER_DATA_PS_31    0xb0acu   /* [3714] */
#define XLAT12_G11_SPI_SHADER_USER_DATA_HS_0     0xb430u   /* [3852] */
#define XLAT12_G11_SPI_SHADER_USER_DATA_HS_31    0xb4acu   /* [3914] */
#define XLAT12_G11_COMPUTE_USER_DATA_0           0xb900u   /* [4040] */
#define XLAT12_G11_COMPUTE_USER_DATA_15          0xb93cu   /* [4070] */
/* ring-offsets s[0:1]: radv_queue.c:571-577, the GFX11 branch {USER_DATA_PS_0, PGM_LO_HS 0xb420, PGM_LO_GS 0xb220},
 * each a 64-bit pointer (radeon_emit_64bit_pointer: LO then HI). On gfx12 the same three are 0xb030/0xb410/0xb210
 * (:564-566). PGM_LO_GS is free on gfx11 NGG because the merged stage's program is PGM_LO_ES (radv_shader.c:2134). */
#define XLAT12_G11_SPI_SHADER_PGM_LO_GS          0xb220u   /* [3740] */
#define XLAT12_G11_SPI_SHADER_PGM_HI_GS          0xb224u   /* [3742] */
#define XLAT12_G11_SPI_SHADER_PGM_LO_HS          0xb420u   /* [3844] */
#define XLAT12_G11_SPI_SHADER_PGM_HI_HS          0xb424u   /* [3846] */
/* the attribute ring: ac_cmdbuf_cp.c:303-309 ac_emit_cp_gfx11_ge_rings, unconditional for GFX11+; the GE_POS /
 * GE_PRIM rings (:311-327) are GFX12-only, and gfx11.json has no register for them. */
#define XLAT12_G11_SPI_GS_THROTTLE_CNTL1         0x31110u  /* [7422] */
#define XLAT12_G11_SPI_GS_THROTTLE_CNTL2         0x31114u  /* [7424] */
#define XLAT12_G11_SPI_ATTRIBUTE_RING_BASE       0x31118u  /* [7426] */
#define XLAT12_G11_SPI_ATTRIBUTE_RING_SIZE       0x3111cu  /* [7428] */

/* Values (not addresses; the test does not read these as registers). */
#define XLAT12_G11V_GS_THROTTLE_CNTL1            0x12355123u   /* ac_cmdbuf_cp.c:304 */
#define XLAT12_G11V_GS_THROTTLE_CNTL2            0x0001544Du   /* ac_cmdbuf_cp.c:305 */
/* ac_cmdbuf_cp.c:307-309: MEM_SIZE = (attribute_ring_size_per_se >> 16) - 1, BIG_PAGE = discardable_allows_big_page,
 * L1_POLICY 1. ac_gpu_info.c:1737-1738: 1400 KiB per SE on a part with an L3 (MALL), aligned to 64 KiB (:1749) =
 * 1408 KiB = 22 units, so MEM_SIZE 0x15; ac_gpu_info.c:1641-1642: big pages on GFX10_3..GFX11 with dedicated VRAM,
 * so BIG_PAGE 1 (bit 16; gfx11.json SPI_ATTRIBUTE_RING_SIZE: MEM_SIZE [7:0], BIG_PAGE [16], L1_POLICY [18:17]).
 * shortcut: assumes Navi 33 reports an L3 (its 32 MiB Infinity Cache as mall_size, ac_gpu_info.c:609); without one
 * the size is l2_cache_size / 2 (:1741-1745) - recheck against the card's ac_gpu_info before X4 draws. The ring
 * buffer the caller maps (XLAT12_GE_RING_TOTAL, 10.5 MiB) holds 1408 KiB for up to 7 SEs. */
#define XLAT12_G11V_ATTRIBUTE_RING_SIZE          0x00030015u

#endif /* XLAT12_GFX11_H */
