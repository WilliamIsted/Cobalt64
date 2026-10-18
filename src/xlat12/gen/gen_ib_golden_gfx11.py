#!/usr/bin/env python3
"""
gen_ib_golden_gfx11.py - gfx11 IB-level golden vectors for src/xlat12 (stage 3 step 7, sub-step X3).

The IB inputs are synthetic gfx10.3 IBs built here from register NAMES looked up in Mesa gfx103.json
(each exercises one rule of xlat12_ib_translate / census), plus every X2 SET stream of
golden_vectors.h wrapped as an IB (CONTEXT_CONTROL first, a draw last). There is no Apple capture in
this clone (the r30/r44/census fixtures and the private re/ tree are not here), so this is the
substitute for an IB census over captured traffic.

The EXPECTED output is an independent Python re-implementation of the IB contract (xlat12_ib.h:
packets copied verbatim, CONTEXT_CONTROL's proven form, the memory-loaded refusal, register operands
that must be IDENTICAL, the proven compute SETs, COND_EXEC exec counts) over gen_golden_gfx11.py's
per-register oracle, which reads gfx103.json / gfx11.json and addrlib, never the C tables.

REGISTER-HEADER CHECK: every register a SET packet of an expected output writes must be a gfx11
register at that address in BOTH Mesa gfx11.json and Linux gc_11_0_0_offset.h; the generator refuses
otherwise. The same set is emitted as kGfx11RegAddrs for the C test's draw-policy checks.

Output: src/xlat12/tests/golden_ib_gfx11.h
Run:    python3 src/xlat12/gen/gen_ib_golden_gfx11.py [--mesa DIR] [--linux DIR]
"""
import argparse, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, HERE)
import gen_tables as G
import gen_tables_gfx11 as G11
import gen_golden_gfx11 as O

NOP1 = 0xFFFF1000
CTXCTL = [0xC0012800, 0x80000000, 0x80000000]
OP_NOP, OP_COND_EXEC, OP_CTXCTL = 0x10, 0x22, 0x28
OP_WRITE_DATA, OP_WAIT_REG_MEM, OP_COPY_DATA = 0x37, 0x3C, 0x40
PASSTHROUGH = {0x49, 0x58, 0x46, 0x47, 0x15, 0x2D, 0x27, 0x35, 0x2F, 0x2A, 0x42, 0x11, 0x26, 0x13}
MEMLOADED = {0x12, 0x5E, 0x5F, 0x60, 0x61, 0x63, 0x9F}
SETS = {0x69, 0x76, 0x79, 0x9B, 0x7A}
DW_BASE = {0x69: 0xA000, 0x76: 0x2C00, 0x9B: 0x2C00, 0x79: 0xC000, 0x7A: 0xC000}
CLS_IDX = {"identical": 0, "moved": 1, "field_repack": 2, "absent": 3, "legacy_vs": 4, "unknown": 5}

def cs_offset_proven(off):
    # the compute SH offsets Apple's blit IB programs (xlat12_ib.c cs_offset_proven: a policy list, not a source)
    return (0x204 <= off <= 0x209) or off in (0x20c, 0x20d, 0x212, 0x213) or (0x215 <= off <= 0x21a) or \
           (0x222 <= off <= 0x228) or off == 0x22a or (0x240 <= off <= 0x247)

class Oracle:
    def __init__(self, D10, D11):
        self.D10, self.D11 = D10, D11
    def cls(self, a10):
        return O.oracle_class(self.D10, self.D11, a10)[0]
    def identical(self, dw):
        return dw <= 0x3FFFFFFF and self.cls(dw << 2) == "identical"
    def cs_proven(self, p):
        h = p[0]; op = (h >> 8) & 0xFF
        if not (h & 2) or op not in (0x76, 0x9B) or len(p) < 3: return False
        off = p[1] & 0xFFFF
        if op == 0x9B and (((p[1] >> 28) & 0xF) != 3 or (p[1] & 0x0FFF0000)): return False
        return all(cs_offset_proven(off + k) and self.identical(0x2C00 + off + k) for k in range(len(p) - 2))
    def operand_ok(self, p):
        op = (p[0] >> 8) & 0xFF
        if op == OP_WRITE_DATA:
            if len(p) < 4: return False
            return ((p[1] >> 8) & 0xF) != 0 or self.identical(p[2])
        if op == OP_WAIT_REG_MEM:
            if len(p) < 7: return False
            if ((p[1] >> 4) & 3) == 1: return True
            return self.identical(p[2]) and (((p[1] >> 6) & 3) != 1 or self.identical(p[3]))
        if op == OP_COPY_DATA:
            if len(p) < 6: return False
            return ((p[1] & 0xF) != 0 or self.identical(p[2])) and (((p[1] >> 8) & 0xF) != 0 or self.identical(p[4]))
        return True

def plen(s, i):
    h = s[i]
    if h == NOP1 or (h >> 30) == 2: return 1
    if (h >> 30) != 3: return 0
    l = ((h >> 16) & 0x3FFF) + 2
    return l if i + l <= len(s) else 0

def translate(orc, s, ngg, allow_cond=True):
    """(status, out, ngg) - the IB contract of xlat12_ib_translate_level(GFX11)."""
    out, i = [], 0
    while i < len(s):
        l = plen(s, i)
        if not l: return ("ERR_TRUNCATED" if (s[i] >> 30) == 3 else "ERR_BAD_PACKET", [], ngg)
        p = s[i:i + l]; h = p[0]
        op = OP_NOP if (h == NOP1 or (h >> 30) == 2) else (h >> 8) & 0xFF
        if op == OP_NOP or op in PASSTHROUGH or (op == OP_CTXCTL and p == CTXCTL) or \
           (op in (OP_WRITE_DATA, OP_WAIT_REG_MEM, OP_COPY_DATA) and orc.operand_ok(p)) or \
           (op in SETS and orc.cs_proven(p)):
            out += p; i += l; continue
        if op == OP_CTXCTL or op in MEMLOADED: return ("ERR_MEMLOADED", [], ngg)
        if op in (OP_WRITE_DATA, OP_WAIT_REG_MEM, OP_COPY_DATA): return ("IB_ERR_REG_OPERAND", [], ngg)
        if op in SETS:
            r = O.oracle(orc.D10, orc.D11, p, ngg)
            if r[0] != "OK": return (r[0], [], ngg)
            out += r[2]; ngg = r[4]; i += l; continue
        if op == OP_COND_EXEC:
            if not allow_cond or l != 5: return ("IB_ERR_COND_EXEC", [], ngg)
            ex = p[4] & 0x3FFF
            if i + l + ex > len(s): return ("IB_ERR_COND_EXEC", [], ngg)
            st, sub, ngg = translate(orc, s[i + l:i + l + ex], ngg, False)
            if st in ("ERR_TRUNCATED", "ERR_BAD_PACKET"): return ("IB_ERR_COND_EXEC", [], ngg)
            if st != "OK": return (st, [], ngg)
            out += p[:4] + [(p[4] & ~0x3FFF & 0xFFFFFFFF) | len(sub)] + sub
            i += l + ex; continue
        return ("IB_ERR_UNLISTED", [], ngg)
    return ("OK", out, ngg)

def census(orc, s):
    c = dict(regs=0, cls=[0] * 7, cs_proven=0, operand_bad=0, unlisted=0, first_unknown=0, packets=0)
    i = 0
    while i < len(s):
        l = plen(s, i)
        if not l: break
        p = s[i:i + l]; h = p[0]; c["packets"] += 1
        if h != NOP1 and (h >> 30) == 3:
            op = (h >> 8) & 0xFF
            if op in SETS:
                if orc.cs_proven(p): c["cs_proven"] += l - 2; c["regs"] += l - 2
                else:
                    off = p[1] & 0xFFFF
                    for k in range(l - 2):
                        a = (DW_BASE[op] + off + k) << 2
                        k2 = CLS_IDX[orc.cls(a)]
                        c["cls"][k2] += 1; c["regs"] += 1
                        if k2 == 5 and not c["first_unknown"]: c["first_unknown"] = a
            elif op in (OP_WRITE_DATA, OP_WAIT_REG_MEM, OP_COPY_DATA):
                if not orc.operand_ok(p): c["operand_bad"] += 1
            elif op in (OP_NOP, OP_CTXCTL, OP_COND_EXEC, 0x3F, 0x50) or op in MEMLOADED or op in PASSTHROUGH:
                pass
            else:
                c["unlisted"] += 1
        i += l
    return c

def set_regs(out):
    """Every (op, byte address) a SET packet of a translated stream writes."""
    regs, i = [], 0
    while i < len(out):
        h = out[i]
        if h == NOP1 or (h >> 30) == 2: i += 1; continue
        op = (h >> 8) & 0xFF; l = ((h >> 16) & 0x3FFF) + 2
        if op in SETS:
            off = out[i + 1] & 0xFFFF
            regs += [(DW_BASE[op] + off + k) << 2 for k in range(l - 2)]
        i += l
    return regs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mesa", default=G11.DEF_MESA)
    ap.add_argument("--linux", default=G11.DEF_LINUX)
    args = ap.parse_args()
    D10, D11 = O.DB(os.path.join(args.mesa, "gfx103.json")), O.DB(os.path.join(args.mesa, "gfx11.json"))
    O.SWMODE_OK = O.swmode_ok_set(args.mesa)
    orc = Oracle(D10, D11)

    # the gfx11 register set: gfx11.json mm addresses in the SET_* blocks that gc_11_0_0_offset.h names at the same address
    m11 = G.MesaDB(os.path.join(args.mesa, "gfx11.json"))
    l11, _ = G.load_linux(args.linux, "gc_11_0_0_offset.h", "reg")
    b11, _ = G.derive_bases(m11, l11)
    regset = set()
    for nm, ent in m11.names.items():
        a = ent["map"]["at"]
        if ent["map"].get("to") == "mm" and (0x28000 <= a < 0x30000 or 0xB000 <= a < 0xC000 or 0x30000 <= a < 0x40000):
            if G.linux_addr(l11, b11, nm) == a: regset.add(a)
    if len(regset) < 1000: G.die("only %d gfx11 SET-space registers agree between gfx11.json and Linux" % len(regset))

    a = lambda nm: D10.name[nm]["map"]["at"]
    hdr = O.hdr
    ctx = lambda nm, vals, f=0: [hdr(0x69, len(vals), f), (a(nm) - 0x28000) >> 2] + vals
    sh = lambda nm, vals, f=0: [hdr(0x76, len(vals), f), (a(nm) - 0xB000) >> 2] + vals
    ucf = lambda nm, vals: [hdr(0x79, len(vals)), (a(nm) - 0x30000) >> 2] + vals
    pkt = lambda op, body: [hdr(op, len(body) - 1)] + body
    draw = pkt(0x2D, [3, 2])                                       # DRAW_INDEX_AUTO count 3, initiator
    rel = pkt(0x49, [0x00030514, 0x80000000, 0x1000, 0, 0, 0, 0])  # RELEASE_MEM, verbatim
    acq = pkt(0x58, [0x80000000, 0, 0, 0, 0, 0, 0x0000040a])       # ACQUIRE_MEM, verbatim

    vecs = []
    def add(name, s, ngg, note):
        st, out, ngg_out = translate(orc, s, ngg)
        c = census(orc, s)
        if st == "OK":
            for r in set_regs(out):
                if r not in regset:
                    G.die("%s: output writes 0x%05x, not a gfx11 register in both gfx11.json and gc_11_0_0_offset.h" % (name, r))
        vecs.append(dict(name=name, note=note, inp=s, st=st, out=out if st == "OK" else [], ngg=ngg, c=c))

    cs_blit = [hdr(0x76, 6, 2), 0x204, 0, 0, 0, 1, 1, 1] + [hdr(0x76, 2, 2), 0x20c, 0x00400000, 0] + \
              [hdr(0x76, 2, 2), 0x212, 0x002c0041, 0x00000090] + [hdr(0x76, 6, 2), 0x215] + [0] * 6
    add("blit_shape", CTXCTL + cs_blit + pkt(0x15, [8, 1, 1, 0x45]) + rel + acq + pkt(0x46, [0x16]) + [NOP1] * 3, False,
        "CONTEXT_CONTROL, compute SET_SH at the proven offsets, DISPATCH_DIRECT, RELEASE/ACQUIRE_MEM, EVENT_WRITE, NOPs")
    render = ctx("DB_RENDER_CONTROL", [0, 0, 0, 0, 0, 0]) + \
             sh("SPI_SHADER_PGM_RSRC1_PS", [0x002c0041, 0x00000090]) + \
             ctx("PA_SC_WINDOW_SCISSOR_BR", [0x00400040]) + \
             ucf("VGT_PRIMITIVE_TYPE", [4]) + [hdr(0x7A, 1), (2 << 28) | ((a("VGT_INDEX_TYPE") - 0x30000) >> 2), 1] + \
             pkt(0x2A, [1]) + pkt(0x2F, [1])
    cond_body = ctx("VGT_GS_OUT_PRIM_TYPE", [2]) + pkt(0x37, [0x00000500, 0x1000, 0, 0xCAFE])
    render += pkt(0x22, [0x2000, 0, 0, len(cond_body)]) + cond_body + draw
    add("render_shape", CTXCTL + render, True,
        "context run, PS SH, scissor BR (exclusive: unchanged), uconfig + _INDEX, INDEX_TYPE, NUM_INSTANCES, COND_EXEC "
        "over a context->uconfig move and a memory WRITE_DATA, draw")
    vpt_dw = a("VGT_PRIMITIVE_TYPE") >> 2
    add("write_data_reg_identical", pkt(0x37, [0, vpt_dw, 0, 4]), True, "WRITE_DATA to an IDENTICAL register: verbatim")
    add("write_data_reg_moved", pkt(0x37, [0, a("VGT_GS_OUT_PRIM_TYPE") >> 2, 0, 2]), True,
        "WRITE_DATA to VGT_GS_OUT_PRIM_TYPE (moved to uconfig on gfx11): refused")
    add("wait_reg_mem_mem", pkt(0x3C, [0x00000013, 0x1000, 0, 1, 0xFFFFFFFF, 4]), True, "WAIT_REG_MEM on memory: verbatim")
    add("clear_state", pkt(0x12, [0]), True, "CLEAR_STATE: memory-loaded, refused")
    add("ctxctl_other", [0xC0012800, 0x80000002, 0x80000000], True, "a non-proven CONTEXT_CONTROL: refused")
    add("indirect_buffer", pkt(0x3F, [0x1000, 0, 4]), True, "INDIRECT_BUFFER: unlisted, refused")
    add("cond_exec_nested", pkt(0x22, [0, 0, 0, 5]) + pkt(0x22, [0, 0, 0, 0]), True, "COND_EXEC inside COND_EXEC: refused")
    add("swmode_tiled", CTXCTL + ctx("CB_COLOR0_ATTRIB3", [9 << 14]) + ctx("DB_Z_INFO", [(22 << 4) | 3]) + draw, True,
        "COLOR_SW_MODE 9 and DB SW_MODE 22: kept")
    add("swmode_23", CTXCTL + ctx("CB_COLOR0_ATTRIB3", [23 << 14]) + draw, True, "COLOR_SW_MODE 23 (Reserved on gfx11): refused")
    add("legacy_vs_unknown", sh("SPI_SHADER_PGM_LO_VS", [0x1234, 0]) + draw, False,
        "non-zero legacy-VS write, NGG not known: refused")
    add("ngg_then_legacy_vs", ctx("VGT_SHADER_STAGES_EN", [1 << 13]) + sh("SPI_SHADER_PGM_LO_VS", [0x1234, 0]) + draw, False,
        "NGG learnt, legacy-VS writes dropped")
    for nm, note, ngg, stream in O.read_gfx12_inputs():
        add("wrapped_" + nm, CTXCTL + stream + draw, ngg, "X2 golden SET stream %s inside an IB" % nm)
        if nm.startswith("encoder_"):
            add("wrapped_" + nm + "_swlinear", CTXCTL + O.zero_swmode(stream) + draw, ngg,
                "X2 golden SET stream %s, SW_MODE zeroed, inside an IB" % nm)

    outp = os.path.join(REPO, "src/xlat12/tests/golden_ib_gfx11.h")
    with open(outp, "w") as f:
        f.write("/* GENERATED by src/xlat12/gen/gen_ib_golden_gfx11.py - do not edit.\n"
                " * Synthetic gfx10.3 IBs and the X2 SET streams wrapped as IBs; expected gfx11 IB translation and census\n"
                " * from an independent Python oracle over Mesa gfx103.json / gfx11.json and addrlib. */\n")
        f.write("#ifndef XLAT12_GOLDEN_IB_GFX11_H\n#define XLAT12_GOLDEN_IB_GFX11_H\n#include <stdint.h>\n\n")
        f.write("typedef struct {\n  const char *name; const char *note;\n  const uint32_t *in; uint32_t in_len;\n"
                "  const uint32_t *exp; uint32_t exp_len; const char *status; uint32_t ngg_in;\n"
                "  uint32_t c_regs, c_cls[7], c_cs_proven, c_operand_bad, c_unlisted, c_first_unknown, c_packets;\n} GoldenIb11;\n\n")
        for v in vecs:
            for tag, arr in (("in", v["inp"]), ("exp", v["out"])):
                f.write("static const uint32_t gib11_%s_%s[] = {" % (v["name"], tag))
                for k, w in enumerate(arr):
                    f.write(("\n  " if k % 8 == 0 else " ") + "0x%08x," % w)
                if not arr: f.write(" 0")
                f.write("\n};\n")
        f.write("\nstatic const GoldenIb11 kGoldenIb11[] = {\n")
        for v in vecs:
            c = v["c"]
            f.write('  { "%s", "%s", gib11_%s_in, %d, gib11_%s_exp, %d, "%s", %d, %d, { %s }, %d, %d, %d, 0x%05x, %d },\n'
                    % (v["name"], v["note"], v["name"], len(v["inp"]), v["name"], len(v["out"]), v["st"], int(v["ngg"]),
                       c["regs"], ", ".join(str(x) for x in c["cls"]), c["cs_proven"], c["operand_bad"], c["unlisted"],
                       c["first_unknown"], c["packets"]))
        f.write("};\n#define GOLDEN_IB11_COUNT (sizeof(kGoldenIb11)/sizeof(kGoldenIb11[0]))\n\n")
        f.write("/* every gfx11 register address in the SET_* blocks named at that address by BOTH gfx11.json and\n"
                " * gc_11_0_0_offset.h (sorted): the register-header check's set */\n")
        f.write("static const uint32_t kGfx11RegAddrs[] = {")
        for k, r in enumerate(sorted(regset)):
            f.write(("\n  " if k % 8 == 0 else " ") + "0x%05x," % r)
        f.write("\n};\n#define GFX11_REG_ADDRS_COUNT (sizeof(kGfx11RegAddrs)/sizeof(kGfx11RegAddrs[0]))\n\n#endif\n")
    for v in vecs:
        print("%-38s in %4d  %-20s out %4d  census regs %d cls %s cs %d" % (v["name"], len(v["inp"]), v["st"], len(v["out"]),
              v["c"]["regs"], v["c"]["cls"], v["c"]["cs_proven"]))
    print("gfx11 register set: %d addresses" % len(regset))
    print("wrote", outp)

if __name__ == "__main__":
    main()
