#!/usr/bin/env python3
"""
gen_golden_gfx11.py - gfx11 golden vectors for src/xlat12 (stage 3 step 7, sub-step X2).

The INPUT streams are the gfx10.3 PM4 streams already in tests/golden_vectors.h (gen_golden.py
transcribed them; its Apple inputs are not in this clone, so they are read back from that header,
not rebuilt). The EXPECTED gfx11 output comes from an independent Python oracle that reads Mesa
gfx103.json / gfx11.json directly. It does not read the generated C tables or repack code.

The oracle is gen_golden.py's oracle with the gfx11 rules of xlat12_translate_level (xlat12.h):
  - classes by name and field list, as gen_golden.py / gen_tables.py classify()
  - no REUSED rows: gfx11.json names no register at 0x28b94 / 0x28b98 (checked below)
  - legacy-VS: the same NGG rule as gfx12 (gfx11.json has no SPI_SHADER_*_VS register)
  - SW_MODE (CB_COLORn_ATTRIB3.COLOR_SW_MODE, DB_Z_INFO / DB_STENCIL_INFO.SW_MODE), X3: a mode passes
    unchanged when addrlib's Gfx10Lib and Gfx11Lib SwizzleModeTables (parsed here from the .cpp sources,
    not from xlat12_gfx11.h) give it the same non-empty flag row with no flag in a column the two tables
    name differently (gfx10 "Var" vs gfx11 "256KB"); any other mode refuses ERR_SWMODE
  - no scissor BR adjust (the inclusive BR is GFX12-only, radv_cmd_buffer.c:4597)

Also emitted:
  - extra streams: a legacy-VS pipeline, an NGG pipeline with zero legacy-VS writes, the one
    register that both moves and repacks (VGT_GS_OUT_PRIM_TYPE, context -> uconfig), and SW_MODE
    linear / tiled writes
  - one single-register case per gfx10.3 SET_* register (1,828) x 3 values

Output: src/xlat12/tests/golden_vectors_gfx11.h
Run:    python3 src/xlat12/gen/gen_golden_gfx11.py [--mesa DIR]
"""
import argparse, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, HERE)
import gen_tables_gfx11 as G11   # DEF_MESA and in_setstar_space only

CTX, SH, UCF = 0x28000, 0xB000, 0x30000
OP_CTX, OP_SH, OP_UCF, OP_SHI, OP_UCFI = 0x69, 0x76, 0x79, 0x9B, 0x7A
BASE = {OP_CTX: CTX, OP_SH: SH, OP_UCF: UCF, OP_SHI: SH, OP_UCFI: UCF}
LEGACY = re.compile(r"^SPI_SHADER_(PGM_(LO|HI|RSRC[1-4])_VS|LATE_ALLOC_VS|USER_DATA_VS_\d+)$")
SWMODE_LO = {a: 14 for a in (0x28ee0, 0x28ee4, 0x28ee8, 0x28eec, 0x28ef0, 0x28ef4, 0x28ef8, 0x28efc)}
SWMODE_LO.update({0x28040: 4, 0x28044: 4})

def die(m):
    sys.stderr.write("gen_golden_gfx11: REFUSING - %s\n" % m); sys.exit(2)

def hdr(op, cnt, flags=0):
    return 0xC0000000 | ((cnt & 0x3FFF) << 16) | ((op & 0xFF) << 8) | (flags & 0xFF)

class DB:
    def __init__(self, path):
        j = json.load(open(path))
        self.name, self.addr = {}, {}
        for e in j["register_mappings"]:
            if e["map"].get("to") != "mm":
                continue
            self.name.setdefault(e["name"], e)
            self.addr.setdefault(e["map"]["at"], e["name"])
        self.types = j["register_types"]
    def fields(self, nm):
        e = self.name[nm]; t = self.types.get(e.get("type_ref", nm))
        return None if t is None else [(f["name"], f["bits"][0], f["bits"][1]) for f in t["fields"]]

def oracle_class(D10, D11, a10):
    nm = D10.addr.get(a10)
    if nm is None or not G11.in_setstar_space(a10): return ("unknown", None, None, None)
    if nm not in D11.name: return ("legacy_vs" if LEGACY.match(nm) else "absent", nm, 0, None)
    a11 = D11.name[nm]["map"]["at"]
    f10, f11 = D10.fields(nm), D11.fields(nm)
    if f10 is not None and f11 is not None and f10 != f11:
        if not set(x[0] for x in f10) & set(x[0] for x in f11):
            return ("unknown", nm, None, None)        # no shared field: no established mapping
        return ("field_repack", nm, a11, (f10, f11))
    return ("identical" if a11 == a10 else "moved", nm, a11, None)

def oracle_repack(v, ff):
    f10, f11 = ff; src = {n: (lo, hi) for n, lo, hi in f10}; o = 0
    for n, lo, hi in f11:
        if n in src:
            slo, shi = src[n]; w = min(shi - slo, hi - lo) + 1
            o |= ((v >> slo) & ((1 << w) - 1)) << lo
    return o & 0xFFFFFFFF

def op_for(a):
    if CTX <= a < 0x30000: return OP_CTX
    if SH <= a < 0xC000: return OP_SH
    if UCF <= a < 0x40000: return OP_UCF
    return None

def addrlib_table(path, cls):
    """{mode: (flags...)} and the column names of addrlib's <cls>::SwizzleModeTable in one .cpp file."""
    src = open(path).read()
    m = re.search(r"const SwizzleModeFlags %s::SwizzleModeTable\[ADDR_SW_MAX_TYPE\] =\s*\{//([^\n]*)\n(.*?)\n\};" % cls, src, re.S)
    if not m: die("no %s::SwizzleModeTable in %s" % (cls, path))
    cols = m.group(1).split()
    rows = re.findall(r"\{\{([0-9,\s]+)\}\}", m.group(2))
    if len(rows) != 33: die("%s: %d SwizzleModeTable rows, expected ADDR_SW_MAX_TYPE 33" % (path, len(rows)))
    return {k: tuple(int(x) for x in r.split(",")) for k, r in enumerate(rows)}, cols

def swmode_ok_set(mesa):
    addrlib = os.path.join(mesa, "..", "addrlib", "src")
    t10, c10 = addrlib_table(os.path.join(addrlib, "gfx10", "gfx10addrlib.cpp"), "Gfx10Lib")
    t11, c11 = addrlib_table(os.path.join(addrlib, "gfx11", "gfx11addrlib.cpp"), "Gfx11Lib")
    if len(c10) != len(c11) or len(c10) != len(t10[0]): die("SwizzleModeTable column headers differ in length")
    differ = [k for k in range(len(c10)) if c10[k] != c11[k]]
    ok = set()
    for mode in range(32):                         # the field is 5 bits
        a, b = t10[mode], t11[mode]
        if any(a) and a == b and not any(a[k] or b[k] for k in differ): ok.add(mode)
    return ok

SWMODE_OK = None   # set in main()

def oracle(D10, D11, stream, ngg):
    """Independent re-implementation of xlat12_translate_level(XLAT12_LEVEL_GFX11, ...)."""
    PRIMGEN = [f for f in D10.fields("VGT_SHADER_STAGES_EN") if f[0] == "PRIMGEN_EN"][0][1]
    VSE = D10.name["VGT_SHADER_STAGES_EN"]["map"]["at"]
    out, st = [], dict(regs_in=0, identical=0, moved=0, repacked=0, absent=0, legacy=0, reused=0,
                       set_out=0, split=0)
    i = 0
    while i < len(stream):
        h = stream[i]; op = (h >> 8) & 0xFF; flags = h & 0xFF; body = ((h >> 16) & 0x3FFF) + 1
        pkt = stream[i + 1:i + 1 + body]
        if op in (OP_CTX, OP_SH, OP_UCF, OP_SHI, OP_UCFI):
            isidx = op in (OP_SHI, OP_UCFI)
            off = pkt[0] & (0xFFFF if isidx else 0xFFFFFFFF); idx = pkt[0] >> 28
            runs = []
            for k, v in enumerate(pkt[1:]):
                a10 = BASE[op] + ((off + k) << 2)
                st["regs_in"] += 1
                cls, nm, a11, ff = oracle_class(D10, D11, a10)
                if cls == "unknown": return ("ERR_UNKNOWN_REG", a10, out, st, ngg)
                if cls == "absent": st["absent"] += 1; continue
                if cls == "legacy_vs":
                    if ngg or v == 0: st["legacy"] += 1; continue
                    return ("ERR_VS_MODE_UNKNOWN", a10, out, st, ngg)
                if a10 in SWMODE_LO and ((v >> SWMODE_LO[a10]) & 0x1F) not in SWMODE_OK:
                    return ("ERR_SWMODE", a10, out, st, ngg)
                if a10 == VSE:
                    if not (v >> PRIMGEN) & 1: return ("ERR_LEGACY_VS", a10, out, st, ngg)
                    ngg = True
                if cls == "field_repack": v = oracle_repack(v, ff); st["repacked"] += 1
                elif cls == "moved": st["moved"] += 1
                else: st["identical"] += 1
                if isidx:
                    runs.append([op, (a11 - BASE[op]) >> 2, [v], True])
                    continue
                o = op_for(a11); off11 = (a11 - BASE[o]) >> 2
                if runs and runs[-1][0] == o and runs[-1][1] + len(runs[-1][2]) == off11:
                    runs[-1][2].append(v)
                else:
                    runs.append([o, off11, [v], False])
            for o, off11, vals, ix in runs:
                out.append(hdr(o, len(vals), flags))
                out.append(((idx << 28) | off11) if ix else off11)
                out += vals
            st["set_out"] += len(runs)
            st["split"] += max(0, len(runs) - 1)
        else:
            out += stream[i:i + 1 + body]
        i += 1 + body
    return ("OK", 0, out, st, ngg)

def zero_swmode(stream):
    """The same stream with every SW_MODE field written through a SET_CONTEXT_REG cleared to 0."""
    s, i = list(stream), 0
    while i < len(s):
        h = s[i]; body = ((h >> 16) & 0x3FFF) + 1
        if (h >> 8) & 0xFF == OP_CTX:
            for k in range(body - 1):
                a = CTX + ((s[i + 1] + k) << 2)
                if a in SWMODE_LO: s[i + 2 + k] &= ~(0x1F << SWMODE_LO[a]) & 0xFFFFFFFF
        i += 1 + body
    return s

def read_gfx12_inputs():
    """(name, note, ngg_in, stream) for every kGolden row of tests/golden_vectors.h."""
    src = open(os.path.join(REPO, "src/xlat12/tests/golden_vectors.h")).read()
    arrays = {m.group(1): [int(x, 16) for x in re.findall(r"0x([0-9a-f]{8})", m.group(2))]
              for m in re.finditer(r"static const uint32_t g_(\w+)_in\[\] = \{(.*?)\};", src, re.S)}
    rows = re.findall(r'\{ "(\w+)", "((?:[^"\\]|\\.)*)", g_\w+_in, (\d+), g_\w+_exp, \d+, "\w+", 0x[0-9a-f]+, (\d+),', src)
    if len(rows) != 10: die("expected 10 kGolden rows in golden_vectors.h, parsed %d" % len(rows))
    out = []
    for nm, note, n, ngg in rows:
        if len(arrays[nm]) != int(n): die("%s: parsed %d dwords, header says %s" % (nm, len(arrays[nm]), n))
        out.append((nm, note, ngg == "1", arrays[nm]))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mesa", default=G11.DEF_MESA)
    args = ap.parse_args()
    D10, D11 = DB(os.path.join(args.mesa, "gfx103.json")), DB(os.path.join(args.mesa, "gfx11.json"))
    global SWMODE_OK
    SWMODE_OK = swmode_ok_set(args.mesa)
    print("SW_MODE passed unchanged gfx10.3 -> gfx11 (addrlib):", sorted(SWMODE_OK))
    for a in (0x28b94, 0x28b98):          # the gfx12 REUSED addresses: gfx11 must leave them empty
        if a in D11.addr: die("gfx11.json names %s at 0x%05x: a gfx11 REUSED row is needed" % (D11.addr[a], a))

    vecs = []
    def add(name, stream, ngg_in, note):
        r = oracle(D10, D11, stream, ngg_in)
        vecs.append(dict(name=name, inp=stream, status=r[0], err=r[1], exp=r[2] if r[0] == "OK" else [],
                         st=r[3], ngg_in=ngg_in, ngg_out=r[4], note=note))
    for nm, note, ngg, stream in read_gfx12_inputs():
        add(nm, stream, ngg, note + " [gfx11]")
        if nm.startswith("encoder_"):   # the encoder writes tiled SW_MODEs: also the linear-only form (X2's)
            add(nm + "_swlinear", zero_swmode(stream), ngg, note + " [gfx11, SW_MODE fields zeroed]")

    vse = D10.name["VGT_SHADER_STAGES_EN"]["map"]["at"]
    lo_vs = D10.name["SPI_SHADER_PGM_LO_VS"]["map"]["at"]
    rsrc1_vs = D10.name["SPI_SHADER_PGM_RSRC1_VS"]["map"]["at"]
    pa_cl = D10.name["PA_CL_VS_OUT_CNTL"]["map"]["at"]
    gs_prim = D10.name["VGT_GS_OUT_PRIM_TYPE"]["map"]["at"]
    db_z = D10.name["DB_Z_INFO"]["map"]["at"]
    cb_a3 = D10.name["CB_COLOR0_ATTRIB3"]["map"]["at"]
    ctxp = lambda a, vals: [hdr(OP_CTX, len(vals)), (a - CTX) >> 2] + vals
    shp = lambda a, vals: [hdr(OP_SH, len(vals)), (a - SH) >> 2] + vals
    # gfx10.3 legacy-VS pipeline: VGT_SHADER_STAGES_EN PRIMGEN_EN=0, then the VS program registers.
    add("legacy_vs_pipeline", ctxp(vse, [0x00000000]) + shp(lo_vs, [0x00123400, 0x00000000]) +
        shp(rsrc1_vs, [0x002c0041]) + ctxp(pa_cl, [0x0000000f]), False,
        "legacy-VS pipeline (PRIMGEN_EN=0, VS program written): gfx11 has no VS stage, refused")
    add("legacy_vs_regs_unknown", shp(lo_vs, [0x00123400, 0x00000000]), False,
        "non-zero legacy-VS program address before NGG is known: refused")
    add("ngg_with_zero_legacy_vs", ctxp(vse, [1 << 13]) + shp(lo_vs, [0, 0]) + ctxp(pa_cl, [0x0000000f]), False,
        "NGG pipeline (PRIMGEN_EN=1) with zeroed legacy-VS slots: NGG learnt, VS writes dropped")
    add("ngg_learnt_drops_legacy_vs", ctxp(vse, [1 << 13]) + shp(lo_vs, [0x00123400, 0x00000000]), False,
        "NGG learnt in the same stream: non-zero legacy-VS writes then dropped")
    add("moved_and_repacked", ctxp(gs_prim, [0xFFFFFFFF]), True,
        "VGT_GS_OUT_PRIM_TYPE: context -> uconfig address and field repack in one register")
    add("swmode_linear", ctxp(db_z, [0x00000003]) + ctxp(cb_a3, [0x00000000]), True,
        "DB_Z_INFO / CB_COLOR0_ATTRIB3 with SW_MODE 0 (linear): translated")
    add("swmode_tiled", ctxp(cb_a3, [9 << 14]) + ctxp(db_z, [(22 << 4) | 3]), True,
        "CB_COLOR0_ATTRIB3 COLOR_SW_MODE 9 64KB_S, DB_Z_INFO SW_MODE 22 4KB_D_X: on both tables, passed unchanged")
    for mode, why in ((1, "256B_S: Reserved on gfx11"), (23, "4KB_R_X: Reserved on gfx11"),
                      (28, "gfx10 VAR_Z_X vs gfx11 256KB_Z_X"), (31, "gfx10 VAR_R_X vs gfx11 256KB_R_X"),
                      (12, "Reserved on gfx10")):
        add("swmode_%d_refused" % mode, ctxp(cb_a3, [mode << 14]), True,
            "CB_COLOR0_ATTRIB3 COLOR_SW_MODE %d (%s): refused" % (mode, why))
    add("swmode_23_db_refused", ctxp(db_z, [(23 << 4) | 3]), True, "DB_Z_INFO SW_MODE 23: refused")

    # one single-register case per gfx10.3 SET_* register, 3 values
    cases = []
    regs = sorted(a for a in D10.addr if G11.in_setstar_space(a))
    if len(regs) != 1828: die("gfx103.json SET_* registers %d, expected 1828" % len(regs))
    for a in regs:
        for v in (0xFFFFFFFF, (0x5A5AA5A5 ^ a) & 0xFFFFFFFF, (a * 2654435761) & 0xFFFFFFFF):
            r = oracle(D10, D11, [hdr(op_for(a), 1), (a - BASE[op_for(a)]) >> 2, v], True)
            e = r[2] if r[0] == "OK" else []
            if r[0] == "OK" and len(e) not in (0, 3): die("0x%05x: %d output dwords" % (a, len(e)))
            cases.append((a, D10.addr[a], v, r[0], (e + [0, 0, 0])[:3], len(e)))

    outp = os.path.join(REPO, "src/xlat12/tests/golden_vectors_gfx11.h")
    with open(outp, "w") as f:
        f.write("/* GENERATED by src/xlat12/gen/gen_golden_gfx11.py - do not edit.\n"
                " * gfx10.3 inputs from golden_vectors.h plus X2 streams; expected gfx11 output from an\n"
                " * independent Python oracle over Mesa gfx103.json / gfx11.json. */\n")
        f.write("#ifndef XLAT12_GOLDEN_VECTORS_GFX11_H\n#define XLAT12_GOLDEN_VECTORS_GFX11_H\n#include <stdint.h>\n\n")
        f.write("typedef struct {\n  const char *name; const char *note;\n  const uint32_t *in; uint32_t in_len;\n"
                "  const uint32_t *exp; uint32_t exp_len;\n  const char *status; uint32_t err_addr;\n"
                "  uint32_t ngg_in, ngg_out;\n  uint32_t regs_in, identical, moved, repacked, dropped_absent, dropped_legacy,"
                " set_packets_out, set_runs_split, regs_reused;\n} GoldenVec11;\n\n")
        for v in vecs:
            for tag, arr in (("in", v["inp"]), ("exp", v["exp"])):
                f.write("static const uint32_t g11_%s_%s[] = {" % (v["name"], tag))
                for k, w in enumerate(arr):
                    f.write(("\n  " if k % 8 == 0 else " ") + "0x%08x," % w)
                if not arr: f.write(" 0")
                f.write("\n};\n")
        f.write("\nstatic const GoldenVec11 kGolden11[] = {\n")
        for v in vecs:
            st = v["st"]
            f.write('  { "%s", "%s", g11_%s_in, %d, g11_%s_exp, %d, "%s", 0x%05x, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d },\n'
                    % (v["name"], v["note"], v["name"], len(v["inp"]), v["name"], len(v["exp"]), v["status"], v["err"],
                       int(v["ngg_in"]), int(v["ngg_out"]), st["regs_in"], st["identical"], st["moved"], st["repacked"],
                       st["absent"], st["legacy"], st["set_out"], st["split"], st["reused"]))
        f.write("};\n#define GOLDEN11_COUNT (sizeof(kGolden11)/sizeof(kGolden11[0]))\n\n")
        f.write("/* one SET packet per gfx10.3 register, ctx seeded NGG: expected status and output */\n")
        f.write("typedef struct { uint32_t g10; const char *name; uint32_t in; const char *status;"
                " uint32_t exp[3]; uint32_t exp_len; } RegCase11;\n")
        f.write("static const RegCase11 kRegCases11[] = {\n")
        for a, nm, v, s, e, n in cases:
            f.write('  { 0x%05x, "%s", 0x%08x, "%s", { 0x%08x, 0x%08x, 0x%08x }, %d },\n' % (a, nm, v, s, e[0], e[1], e[2], n))
        f.write("};\n#define REGCASE11_COUNT (sizeof(kRegCases11)/sizeof(kRegCases11[0]))\n\n#endif\n")
    for v in vecs:
        print("%-28s in %4d  %-20s err 0x%05x  out %4d  %s" % (v["name"], len(v["inp"]), v["status"], v["err"], len(v["exp"]), v["st"]))
    hist = {}
    for c in cases: hist[c[3]] = hist.get(c[3], 0) + 1
    print("register cases: %d over %d registers %s" % (len(cases), len(regs), hist))
    print("wrote", outp)

if __name__ == "__main__":
    main()
