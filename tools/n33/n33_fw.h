/* n33_fw.h: slices the GC 11.0.2 / SDMA 6.0.2 / SMU 13.0.7 firmware files the way Linux's
 * amdgpu_ucode_init_single_fw does (pinned tree 3857c2fe, amdgpu_ucode.c:889-1112, amdgpu_rlc.c), for PSP
 * milestone 2 (n33ctl psp2). Plain C, no I/O: host test tests/n33_fw_test.c runs it on the real files.
 * Header layouts: amdgpu_ucode.h (common header 32 bytes; byte offsets below). */
#ifndef N33_FW_H
#define N33_FW_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct n33img { const uint8_t *p; uint32_t n; uint32_t type; const char *name; };

static inline uint32_t n33_rd32(const uint8_t *b, size_t o) { uint32_t v; memcpy(&v, b + o, 4); return v; }
static inline uint16_t n33_rd16(const uint8_t *b, size_t o) { uint16_t v; memcpy(&v, b + o, 2); return v; }

/* One slice, bounds-checked against the file. 0 ok, 1 out of range or empty. */
static inline int n33_slice(const uint8_t *f, size_t n, uint64_t off, uint64_t sz, uint32_t type, const char *name, struct n33img *im) {
	if (sz == 0 || off > n || sz > n - off) return 1;
	im->p = f + off; im->n = (uint32_t)sz; im->type = type; im->name = name;
	return 0;
}

/* Common header: size_bytes +0, header_size +4, major +8, minor +10, ucode_version +16,
 * ucode_size_bytes +20, ucode_array_offset_bytes +24. */
static inline int n33_hdr(const uint8_t *f, size_t n, uint16_t major, uint16_t minMinor) {
	if (n < 64 || n33_rd32(f, 0) != n) return 1;
	return n33_rd16(f, 8) != major || n33_rd16(f, 10) < minMinor;
}

/* sdma_firmware_header_v2_0: TH0 at the array offset, ctx_ucode_size +36; TH1 at ctl_ucode_offset +48,
 * ctl_ucode_size +52. */
static inline int n33_fw_sdma(const uint8_t *f, size_t n, struct n33img im[2]) {
	if (n33_hdr(f, n, 2, 0)) return 1;
	return n33_slice(f, n, n33_rd32(f, 24), n33_rd32(f, 36), 71, "SDMA_TH0", &im[0]) ||
	       n33_slice(f, n, n33_rd32(f, 48), n33_rd32(f, 52), 72, "SDMA_TH1", &im[1]);
}

/* gfx_firmware_header_v2_0 (RS64): ucode at the array offset with size +36; the stack at data_offset
 * +48 with data_size +44, once per pipe (the same bytes). */
static inline int n33_fw_rs64(const uint8_t *f, size_t n, uint32_t ucodeType, const char *name,
                              const uint32_t *stackTypes, const char *const *stackNames, unsigned nstack, struct n33img *im) {
	if (n33_hdr(f, n, 2, 0)) return 1;
	if (n33_slice(f, n, n33_rd32(f, 24), n33_rd32(f, 36), ucodeType, name, &im[0])) return 1;
	for (unsigned i = 0; i < nstack; i++)
		if (n33_slice(f, n, n33_rd32(f, 48), n33_rd32(f, 44), stackTypes[i], stackNames[i], &im[1 + i])) return 1;
	return 0;
}

/* mes_firmware_header_v1_0: ucode size +36 offset +40; data size +48 offset +52. */
static inline int n33_fw_mes(const uint8_t *f, size_t n, uint32_t tU, const char *nU, uint32_t tD, const char *nD, struct n33img im[2]) {
	if (n33_hdr(f, n, 1, 0)) return 1;
	return n33_slice(f, n, n33_rd32(f, 40), n33_rd32(f, 36), tU, nU, &im[0]) ||
	       n33_slice(f, n, n33_rd32(f, 52), n33_rd32(f, 48), tD, nD, &im[1]);
}

/* imu_firmware_header_v1_0: IRAM at the array offset, size +32; DRAM right after it, size +40. */
static inline int n33_fw_imu(const uint8_t *f, size_t n, struct n33img im[2]) {
	if (n33_hdr(f, n, 1, 0)) return 1;
	const uint64_t a = n33_rd32(f, 24), i = n33_rd32(f, 32);
	return n33_slice(f, n, a, i, 68, "IMU_I", &im[0]) ||
	       n33_slice(f, n, a + i, n33_rd32(f, 40), 69, "IMU_D", &im[1]);
}

/* rlc_firmware_header_v2_3 (v2_0 is 104 bytes): GPM size +132 off +136, SRM size +148 off +152, IRAM
 * size +156 off +160, DRAM size +164 off +168, RLC_P size +180 off +184, RLC_V size +196 off +200; RLC_G
 * is the common payload. Offsets are from the start of the header. Order: Linux's (AMDGPU_UCODE_ID). */
static inline int n33_fw_rlc(const uint8_t *f, size_t n, struct n33img im[7]) {
	if (n33_hdr(f, n, 2, 3) || n < 204) return 1;
	return n33_slice(f, n, n33_rd32(f, 136), n33_rd32(f, 132), 20, "RLC_RESTORE_LIST_GPM_MEM", &im[0]) ||
	       n33_slice(f, n, n33_rd32(f, 152), n33_rd32(f, 148), 21, "RLC_RESTORE_LIST_SRM_MEM", &im[1]) ||
	       n33_slice(f, n, n33_rd32(f, 160), n33_rd32(f, 156), 26, "RLC_IRAM", &im[2]) ||
	       n33_slice(f, n, n33_rd32(f, 168), n33_rd32(f, 164), 48, "RLC_DRAM", &im[3]) ||
	       n33_slice(f, n, n33_rd32(f, 184), n33_rd32(f, 180), 25, "RLC_P", &im[4]) ||
	       n33_slice(f, n, n33_rd32(f, 200), n33_rd32(f, 196), 7, "RLC_V", &im[5]) ||
	       n33_slice(f, n, n33_rd32(f, 24), n33_rd32(f, 20), 8, "RLC_G", &im[6]);
}

/* SMU: the common payload (default branch). *ver gets the header's ucode_version (82.95.0 = 0x00525F00). */
static inline int n33_fw_smu(const uint8_t *f, size_t n, struct n33img *im, uint32_t *ver) {
	if (n33_hdr(f, n, 2, 0)) return 1;
	*ver = n33_rd32(f, 16);
	return n33_slice(f, n, n33_rd32(f, 24), n33_rd32(f, 20), 18, "SMU", im);
}

/* The TOC: sub-image type 4 of the psp_13_0_7_sos.bin container (psp_firmware_header_v2_0, the parse of
 * n33ctl psp). */
static inline int n33_fw_toc(const uint8_t *f, size_t n, struct n33img *im) {
	if (n < 40 || n33_rd16(f, 8) != 2) return 1;
	const uint32_t arr = n33_rd32(f, 24), cnt = n33_rd32(f, 32);
	const size_t d = 36 + (n33_rd16(f, 10) >= 1 ? 4 : 0);
	if (arr > n || cnt == 0 || cnt > 32 || d + (size_t)cnt * 16 > n) return 1;
	for (uint32_t i = 0; i < cnt; i++) {
		const size_t q = d + (size_t)i * 16;
		if (n33_rd32(f, q) == 4) return n33_slice(f, n, (uint64_t)arr + n33_rd32(f, q + 8), n33_rd32(f, q + 12), 0, "TOC", im);
	}
	return 1;
}

/* The 26 GC images in Linux's order (the design's table rows 1 to 26), from the seven files. */
struct n33files { const uint8_t *p[7]; size_t n[7]; };   /* sdma, pfp, me, mec, mes_2, mes1, imu, then rlc below */
static inline int n33_fw_gc(const struct n33files *fl, const uint8_t *rlc, size_t rlcN, struct n33img out[26]) {
	static const uint32_t pfpS[2] = { 90, 91 }, meS[2] = { 92, 93 }, mecS[4] = { 94, 95, 96, 97 };
	static const char *const pfpN[2] = { "RS64_PFP_P0_STACK", "RS64_PFP_P1_STACK" };
	static const char *const meN[2] = { "RS64_ME_P0_STACK", "RS64_ME_P1_STACK" };
	static const char *const mecN[4] = { "RS64_MEC_P0_STACK", "RS64_MEC_P1_STACK", "RS64_MEC_P2_STACK", "RS64_MEC_P3_STACK" };
	struct n33img sd[2], pfp[3], me[3], mec[5], mes[2], mes1[2], imu[2], r[7];
	if (n33_fw_sdma(fl->p[0], fl->n[0], sd) ||
	    n33_fw_rs64(fl->p[1], fl->n[1], 87, "RS64_PFP", pfpS, pfpN, 2, pfp) ||
	    n33_fw_rs64(fl->p[2], fl->n[2], 88, "RS64_ME", meS, meN, 2, me) ||
	    n33_fw_rs64(fl->p[3], fl->n[3], 89, "RS64_MEC", mecS, mecN, 4, mec) ||
	    n33_fw_mes(fl->p[4], fl->n[4], 33, "CP_MES", 34, "CP_MES_DATA", mes) ||
	    n33_fw_mes(fl->p[5], fl->n[5], 81, "CP_MES1", 82, "CP_MES1_DATA", mes1) ||
	    n33_fw_imu(fl->p[6], fl->n[6], imu) ||
	    n33_fw_rlc(rlc, rlcN, r)) return 1;
	const struct n33img all[26] = { sd[0], sd[1], pfp[0], me[0], mec[0], pfp[1], pfp[2], me[1], me[2],
	                                mec[1], mec[2], mec[3], mec[4], mes[0], mes[1], mes1[0], mes1[1],
	                                imu[0], imu[1], r[0], r[1], r[2], r[3], r[4], r[5], r[6] };
	memcpy(out, all, sizeof all);
	return 0;
}

/* Page-aligned packing into fw_buf, as amdgpu_ucode_init_bo packs fw_buf. Returns the bytes used. */
static inline uint64_t n33_pack(const struct n33img *im, unsigned k, uint64_t *offs) {
	uint64_t o = 0;
	for (unsigned i = 0; i < k; i++) { offs[i] = o; o += ((uint64_t)im[i].n + 0xFFF) & ~0xFFFull; }
	return o;
}

#endif
