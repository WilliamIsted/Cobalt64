#!/usr/bin/env python3
"""
gen_tables_gfx11.py - generate the gfx10.3 (Navi21) -> gfx11 (Navi33 / RDNA3)
render-state translation table for src/xlat12 (sub-step X1 of stage-3 step 7).

WHY A SIBLING, NOT A FLAG ON gen_tables.py
  gen_tables.py emits the gfx10.3 -> gfx12 table and MUST stay byte-identical in its
  gfx12 output. That output cannot be regenerated in this clone (its Apple inputs,
  re/graphics/mtl-regrecs.txt and pm4census.txt, are not checked in), so the only way
  to guarantee the gfx12 generator is unchanged is to not touch it. This file imports
  gen_tables.py's shared, side-effect-free helpers (MesaDB, load_linux, derive_bases,
  classify, emit_repack, linux_addr, linux_name, cstr) and reuses them unmodified;
  only the per-target enumeration, the gfx11 label strings and the output symbols differ.

HOW THE gfx11 TABLE DIFFERS FROM THE gfx12 TABLE
  - Target generation is gfx11 (Mesa gfx11.json + Linux gc_11_0_0_offset.h), not gfx12.
  - It is a FULL-SET table: every gfx10.3 register Mesa lists in the SET_* register
    spaces (context 0x28000-0x2FFFF, SH 0xB000-0xBFFF, uconfig 0x30000-0x37FFF), 1,828
    registers. The gfx12 table is the Apple-emitted subset (it is driven by the Apple
    register census, which is not in this clone). The full set is used here because
    (a) the done condition counts "of 1,828 gfx10.3 registers", (b) the Apple subset
    inputs are unavailable, and (c) the gfx10.3 SET_* space has NO internal address
    aliases (verified: 1,828 unique addresses, one name each), so an address-keyed
    table is unambiguous. A full-set table is a strict superset of the Apple subset.

AUTHORITATIVE SOURCES (named, with the commit each was read at)
  gfx10.3 / Navi21 addresses + fields
    Mesa  src/amd/registers/gfx103.json   @ mesa 4519cc56 (byte-identical at the clone
                                            head bffd9194; verified by git blob sha256)
    Linux .../gc/gc_10_3_0_offset.h        @ linux 3857c2fe
  gfx11 / Navi3x addresses + fields
    Mesa  src/amd/registers/gfx11.json     @ mesa 4519cc56 (byte-identical at bffd9194)
    Linux .../gc/gc_11_0_0_offset.h        @ linux 3857c2fe  (GC 11.0.2 uses the
                                            gc_11_0_0 set)

CLASSIFICATION (gen_tables.py's classify(), unchanged): a gfx10.3 register is
  identical     present on gfx11 at the same address with the same field list
  moved         present at a different address, same field list
  field_repack  present with a changed field list (its g11 target is still recorded in
                g12, plus a by-name repack function) - this is field-FIRST: a register
                that both moved and changed fields is field_repack and still carries its
                moved g11 address. The reference study buckets address-FIRST (such a
                register counts as "moved"); the two agree on 1,822 of 1,828 registers.
                The count self-check (tests/test_xlat12_tables_gfx11.py) reproduces the
                study's address-first figures exactly; this table keeps classify()'s
                field-first buckets so its semantics match the existing gfx12 table and
                the translator's decide() (X2).
  absent        no gfx11 register of that name: dropped (reason in the row)
  legacy_vs     a legacy-VS slot absent on gfx11 (gfx11 keeps the VS path, so in
                practice none fire; kept for parity with classify())
  unknown       the two sources contradict, or a repack whose field lists share no
                field name: refused by the translator

Outputs (checked in): src/xlat12/xlat12_tables_gfx11.h, xlat12_repack_gfx11.h,
                      xlat12_provenance_gfx11.h
Run:  python3 src/xlat12/gen/gen_tables_gfx11.py [--check]
"""
import os, sys, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_tables as G   # shared helpers; importing does not run its main()

REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))        # the Cobalt64 clone root
RDNA3_REPO = os.path.abspath(os.path.join(REPO, ".."))              # RDNA3/Repo, holding sibling clones

# Mesa JSONs: byte-identical between the design pin 4519cc56 and the clone head
# bffd9194 (verified by git blob sha256), so the clone's copies are used.
MESA_COMMIT  = "4519cc5645880023ed1c83417d33c4cad99e88ee"
LINUX_COMMIT = "3857c2fe"  # gc_11_0_0_offset.h read at linux 3857c2fe (short hash; clone is sparse)

DEF_MESA  = os.path.join(RDNA3_REPO, "mesa/src/amd/registers")
DEF_LINUX = os.path.join(RDNA3_REPO, "linux/drivers/gpu/drm/amd/include/asic_reg/gc")

CTX_LO, CTX_HI = 0x28000, 0x2FFFF
SH_LO,  SH_HI  = 0x0B000, 0x0BFFF
UCF_LO, UCF_HI = 0x30000, 0x37FFF

def in_setstar_space(at):
    return (CTX_LO <= at <= CTX_HI) or (SH_LO <= at <= SH_HI) or (UCF_LO <= at <= UCF_HI)

def setstar_names(m10):
    """Every gfx10.3 register Mesa maps to MMIO ('mm') in the SET_* spaces, sorted by
       address. No Apple census is used (it is not in this clone); the full set is taken."""
    out = []
    for name, ent in m10.names.items():
        if ent["map"].get("to") != "mm":
            continue
        at = ent["map"]["at"]
        if in_setstar_space(at):
            out.append((at, name))
    out.sort()
    return [name for _, name in out]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mesa", default=DEF_MESA)
    ap.add_argument("--linux", default=DEF_LINUX)
    ap.add_argument("--out", default=os.path.join(REPO, "src/xlat12"))
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    m10 = G.MesaDB(os.path.join(args.mesa, "gfx103.json"))
    m11 = G.MesaDB(os.path.join(args.mesa, "gfx11.json"))
    l10, l10line = G.load_linux(args.linux, "gc_10_3_0_offset.h", "mm")
    l11, l11line = G.load_linux(args.linux, "gc_11_0_0_offset.h", "reg")
    b10, rep10 = G.derive_bases(m10, l10)
    b11, rep11 = G.derive_bases(m11, l11)

    entries = {}
    for nm in setstar_names(m10):
        r = G.classify(nm, m10, m11)   # m11 passed as the "m12" target
        if r is None:
            continue
        # classify() hardcodes gfx12 wording only on the ABSENT cite and reason.
        if r["cls"] in ("absent", "legacy_vs"):
            r["cite"] = r["cite"].replace("absent from gfx12.json", "absent from gfx11.json")
            r["reason"] = r["reason"].replace("gfx12", "gfx11")
        g10 = r["g10"]
        if g10 in entries:
            # the SET_* space has no internal aliases (verified); guard anyway.
            G.die("unexpected gfx10.3 address alias 0x%x: %s vs %s"
                  % (g10, entries[g10]["name"], r["name"]))
        entries[g10] = r

    # --- Linux cross-check (ported from gen_tables.py main()). A register whose two
    # sources CONTRADICT becomes UNKNOWN (the translator refuses it) and is listed. ---
    lx = dict(g10_agree=0, g10_unnamed=0, g11_agree=0, g11_unnamed=0, contradictions=0)
    contradictions = []
    for a, r in entries.items():
        if r["cls"] == "unknown":
            continue
        why = None
        la = G.linux_addr(l10, b10, r["name"])
        if la is None:
            lx["g10_unnamed"] += 1
        elif la != r["g10"]:
            why = ("contradictory gfx10.3 sources: gfx103.json 0x%x vs gc_10_3_0_offset.h:%d 0x%x"
                   % (r["g10"], l10line[G.linux_name(l10, r["name"])], la))
        else:
            lx["g10_agree"] += 1
            r["cite"] += ", gc_10_3_0_offset.h:%d" % l10line[G.linux_name(l10, r["name"])]
        if why is None and r["g12"]:
            lb = G.linux_addr(l11, b11, r["name"])
            if lb is None:
                lx["g11_unnamed"] += 1
            elif lb != r["g12"]:
                why = ("contradictory gfx11 sources: gfx11.json 0x%x vs gc_11_0_0_offset.h:%d 0x%x"
                       % (r["g12"], l11line[G.linux_name(l11, r["name"])], lb))
            else:
                lx["g11_agree"] += 1
                r["cite"] += ", gc_11_0_0_offset.h:%d" % l11line[G.linux_name(l11, r["name"])]
        if why is not None:
            lx["contradictions"] += 1
            contradictions.append("%s 0x%05x: %s" % (r["name"], a, why))
            r.update(cls="unknown", g12=0, reason=why, fa=None, fb=None)

    # an unknown address that Linux DOES name would be a missed source: refuse.
    inv10 = {}
    for nm, e in l10.items():
        la = (b10[e[1]] + e[0]) * 4 if e[0] is not None and e[1] in b10 else None
        if la:
            inv10.setdefault(la, nm[:-4] if nm.endswith("_UMD") else nm)
    for a, r in entries.items():
        if r["cls"] == "unknown" and r["name"].startswith("UNNAMED_") and a in inv10:
            G.die("0x%x is classed unknown but gc_10_3_0_offset.h names it %s" % (a, inv10[a]))

    # A field-changed register whose two field lists share NO field name has no
    # established value mapping (a name-matched repack would silently return 0). Refuse.
    noshared = []
    for a, r in sorted(entries.items()):
        if r["cls"] != "field_repack":
            continue
        n10 = set(x[0] for x in r["fa"])
        if not any(x[0] in n10 for x in r["fb"]):
            noshared.append("%s 0x%05x: gfx10.3 %s %s | gfx11 %s %s"
                            % (r["name"], a, m10.cite_type(r["t10"]), [x[0] for x in r["fa"]],
                               m11.cite_type(r["t12"]), [x[0] for x in r["fb"]]))
            r.update(cls="unknown", g12=0, fa=None, fb=None,
                     reason="field lists share no field name (%s vs %s): no established value mapping"
                            % (m10.cite_type(r["t10"]), m11.cite_type(r["t12"])))

    # NGG rule constants, from the gfx10.3 JSON (used by the translator in X2).
    vse = m10.names.get("VGT_SHADER_STAGES_EN")
    if vse is None:
        G.die("VGT_SHADER_STAGES_EN missing from gfx103.json")
    vf, vtr = m10.fields("VGT_SHADER_STAGES_EN")
    prim = [f for f in vf if f[0] == "PRIMGEN_EN"]
    if len(prim) != 1 or prim[0][1] != prim[0][2]:
        G.die("PRIMGEN_EN is not a single bit in gfx103.json")
    ngg = dict(addr=vse["map"]["at"], bit=prim[0][1], cite=m10.cite_type(vtr))

    counts = {k: 0 for k in ("identical", "moved", "field_repack", "absent", "legacy_vs", "unknown")}
    for r in entries.values():
        counts[r["cls"]] += 1

    repacks, notes_all = [], []
    for a in sorted(entries):
        r = entries[a]
        if r["cls"] == "field_repack":
            code, notes, _ = G.emit_repack(r, m10, m11)
            code = code.replace("-> gfx12 0x", "-> gfx11 0x").replace("new on gfx12", "new on gfx11")
            repacks.append(code)
            notes_all += notes

    summary = ["gfx10.3 -> gfx11 FULL SET: every gfx10.3 SET_* register (ctx 0x28000-0x2FFFF, "
               "SH 0xB000-0xBFFF, uconfig 0x30000-0x37FFF)",
               "entries %d" % len(entries)]
    for k, v in counts.items():
        summary.append("%-12s %d" % (k, v))
    summary.append("Linux bases gfx10.3 %s  gfx11 %s" %
                   ({k: "0x%x (%d/%d names)" % v for k, v in rep10.items()},
                    {k: "0x%x (%d/%d names)" % v for k, v in rep11.items()}))
    summary.append("Linux cross-check: gfx10.3 agree %(g10_agree)d, not named %(g10_unnamed)d; "
                   "gfx11 agree %(g11_agree)d, not named %(g11_unnamed)d; "
                   "contradictions (refused) %(contradictions)d" % lx)
    summary += ["contradiction (refused): " + x for x in contradictions]
    summary += ["no shared field (refused): " + x for x in noshared]
    summary.append("NGG rule: VGT_SHADER_STAGES_EN g10 0x%x PRIMGEN_EN bit %d (%s)"
                   % (ngg["addr"], ngg["bit"], ngg["cite"]))

    if args.check:
        print("\n".join(summary))
        print("width-changed shared fields (value masked to the gfx11 width):")
        for n in notes_all:
            print("   ", n)
        print("unknown (refused) addresses:")
        for a in sorted(entries):
            if entries[a]["cls"] == "unknown":
                print("    0x%05x %s" % (a, entries[a]["reason"]))
        return

    write_headers(args.out, entries, repacks, notes_all, summary, ngg)
    print("\n".join(summary))

def write_headers(out, entries, repacks, notes, summary, ngg):
    banner = "/* GENERATED by src/xlat12/gen/gen_tables_gfx11.py - do not edit. Regenerate instead. */\n"
    with open(os.path.join(out, "xlat12_provenance_gfx11.h"), "w") as f:
        f.write(banner + "#ifndef XLAT12_PROVENANCE_GFX11_H\n#define XLAT12_PROVENANCE_GFX11_H\n")
        f.write('#define XLAT12_GFX11_SRC_MESA_COMMIT  "%s"\n' % MESA_COMMIT)
        f.write('#define XLAT12_GFX11_SRC_LINUX_COMMIT "%s"\n' % LINUX_COMMIT)
        f.write("/*\n")
        for s in summary:
            f.write(" * %s\n" % s)
        f.write(" */\n#endif\n")
    with open(os.path.join(out, "xlat12_repack_gfx11.h"), "w") as f:
        f.write(banner + "#ifndef XLAT12_REPACK_GFX11_H\n#define XLAT12_REPACK_GFX11_H\n#include <stdint.h>\n\n")
        f.write("/* One pure value function per field-changed register: each shared field is\n"
                " * copied BY NAME from its gfx10.3 bit range to its gfx11 bit range; gfx10.3-only\n"
                " * fields are dropped, gfx11-only fields are left 0. */\n")
        if notes:
            f.write("/* Shared fields whose width changed (masked to the gfx11 width):\n")
            for n in notes:
                f.write(" *   %s\n" % n)
            f.write(" */\n")
        f.write("\n")
        for code in repacks:
            f.write(code + "\n\n")
        f.write("#endif\n")
    with open(os.path.join(out, "xlat12_tables_gfx11.h"), "w") as f:
        f.write(banner + "#ifndef XLAT12_TABLES_GFX11_H\n#define XLAT12_TABLES_GFX11_H\n#include <stdint.h>\n")
        f.write('#include "xlat12.h"\n#include "xlat12_repack_gfx11.h"\n\n/*\n')
        for s in summary:
            f.write(" * %s\n" % s)
        f.write(" */\n\n")
        f.write("/* Xlat12Class values are defined in xlat12.h. This header defines the gfx11\n"
                " * table and its row struct; include it INSTEAD OF xlat12_tables.h (never both\n"
                " * in one translation unit). The XLAT12_GFX11 build mode selects it. */\n\n")
        f.write("typedef uint32_t (*Xlat12Repack)(uint32_t);\n\n")
        f.write("typedef struct {\n"
                "    uint32_t     g10;     /* gfx10.3 MMIO byte address (sort key) */\n"
                "    uint32_t     g11;     /* gfx11 MMIO byte address; 0 if absent/unknown */\n"
                "    Xlat12Repack repack;  /* field_repack only, else 0 */\n"
                "    uint8_t      cls;     /* Xlat12Class */\n"
                "    const char  *name;\n"
                "    const char  *reason;  /* absent/legacy_vs/unknown: why */\n"
                "} Xlat12Entry;\n\n")
        f.write("#define XLAT12_G10_VGT_SHADER_STAGES_EN 0x%05xu /* %s */\n" % (ngg["addr"], ngg["cite"]))
        f.write("#define XLAT12_G10_PRIMGEN_EN_BIT       %du      /* NGG: radeonsi si_state_shaders.cpp:1352 */\n\n" % ngg["bit"])
        f.write("static const Xlat12Entry kXlat12TableGfx11[] = {\n")
        for a in sorted(entries):
            r = entries[a]
            rp = ("repack_%s" % r["name"]) if r["cls"] == "field_repack" else "0"
            f.write('    /* %s */\n' % G.cstr(r["cite"]))
            f.write('    { 0x%05xu, 0x%05xu, %s, XLAT12_CLS_%s, "%s", "%s" },\n'
                    % (a, r["g12"], rp, r["cls"].upper(), r["name"], G.cstr(r["reason"])))
        f.write("};\n#define XLAT12_TABLE_LEN_GFX11 "
                "((uint32_t)(sizeof(kXlat12TableGfx11) / sizeof(kXlat12TableGfx11[0])))\n\n#endif\n")

if __name__ == "__main__":
    main()
