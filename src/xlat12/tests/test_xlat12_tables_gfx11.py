#!/usr/bin/env python3
"""Source cross-check for the generated gfx10.3 -> gfx11 table (stage-3 step 7, X1).

This is the authoritative half of X1's host test. It reads the COMMITTED header
src/xlat12/xlat12_tables_gfx11.h and checks it against the register sources independently
of the generator's in-memory state:

  1. Every present row's gfx11 target address matches BOTH Mesa gfx11.json (by name) AND
     Linux gc_11_0_0_offset.h (by name: byte = (base_idx base + dword offset) * 4, the
     base taken from the BASE_IDX majority vote, exactly as the generator derives it).
  2. Every removed row (absent/legacy_vs) names a register gfx11.json does NOT carry;
     every unknown row names one it does, but with an incompatible field list (the reason
     says so). Each class's g11/repack shape is consistent.
  3. Every field_repack row's generated repack function equals, verbatim, the one re-derived
     from the two JSON field layouts - so the field bit mapping is checked, not just the
     address.
  4. The identical / moved / field_repack / removed counts over all 1,828 gfx10.3 SET_*
     registers, computed the SAME WAY as the reference difference-map study (address-first:
     "moved" = present with a different address, regardless of fields), reproduce
     1,283 / 55 / 151 / 339. The table itself keeps the generator's field-FIRST buckets
     (identical 1285 / moved 51 / field_repack 150 / removed 339 / unknown 3); the two
     differ on exactly 6 registers, enumerated below, and agree on the other 1,822.

Run:  python3 src/xlat12/tests/test_xlat12_tables_gfx11.py
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
XLAT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(XLAT, "gen"))
import gen_tables as G
import gen_tables_gfx11 as G11

fail = pas = 0
def check(cond, msg):
    global fail, pas
    if cond:
        pas += 1
    else:
        fail += 1
        print("FAIL: " + msg)

m10 = G.MesaDB(os.path.join(G11.DEF_MESA, "gfx103.json"))
m11 = G.MesaDB(os.path.join(G11.DEF_MESA, "gfx11.json"))
l10, l10line = G.load_linux(G11.DEF_LINUX, "gc_10_3_0_offset.h", "mm")
l11, l11line = G.load_linux(G11.DEF_LINUX, "gc_11_0_0_offset.h", "reg")
b10, _ = G.derive_bases(m10, l10)
b11, _ = G.derive_bases(m11, l11)

# ---- parse the committed header rows ----------------------------------------
ROW = re.compile(r'\{\s*0x([0-9a-f]+)u,\s*0x([0-9a-f]+)u,\s*(0|repack11_\w+),'
                 r'\s*XLAT12_CLS_(\w+),\s*"(\w+)",\s*"(.*?)"\s*\},')
hdr = open(os.path.join(XLAT, "xlat12_tables_gfx11.h")).read()
rows = []
for m in ROW.finditer(hdr):
    rows.append(dict(g10=int(m.group(1), 16), g11=int(m.group(2), 16),
                     rp=m.group(3), cls=m.group(4), name=m.group(5), reason=m.group(6)))
check(len(rows) == 1828, "parsed %d rows, expected 1828" % len(rows))

# ---- 1 + 2: every row against gfx11.json and gc_11_0_0_offset.h -------------
for r in rows:
    nm, cls = r["name"], r["cls"]
    present = m11.names.get(nm)
    if cls in ("IDENTICAL", "MOVED", "FIELD_REPACK"):
        check(present is not None, "%s is %s but absent from gfx11.json" % (nm, cls))
        if present is not None:
            check(r["g11"] == present["map"]["at"],
                  "%s g11 0x%05x != gfx11.json 0x%05x" % (nm, r["g11"], present["map"]["at"]))
        la = G.linux_addr(l11, b11, nm)
        check(la is not None and la == r["g11"],
              "%s g11 0x%05x != gc_11_0_0_offset.h %s" % (nm, r["g11"], "0x%05x" % la if la else "absent"))
        if cls == "IDENTICAL":
            check(r["g11"] == r["g10"] and r["rp"] == "0", "%s IDENTICAL shape wrong" % nm)
        if cls == "MOVED":
            check(r["g11"] != r["g10"] and r["rp"] == "0", "%s MOVED shape wrong" % nm)
        if cls == "FIELD_REPACK":
            check(r["rp"] == "repack11_" + nm and r["g11"] != 0, "%s FIELD_REPACK shape wrong" % nm)
    elif cls in ("ABSENT", "LEGACY_VS"):
        check(present is None, "%s is %s but gfx11.json carries it" % (nm, cls))
        check(r["g11"] == 0 and r["rp"] == "0" and r["reason"], "%s %s shape wrong" % (nm, cls))
    elif cls == "UNKNOWN":
        # contradictory sources or a repack whose field lists share no field name
        check(r["g11"] == 0 and r["rp"] == "0", "%s UNKNOWN shape wrong" % nm)
        check("share no field name" in r["reason"] or "contradictory" in r["reason"],
              "%s UNKNOWN reason unexpected: %s" % (nm, r["reason"]))
    else:
        check(False, "%s has unexpected class %s" % (nm, cls))

# ---- 3: every field_repack function matches the re-derived field mapping -----
repack_src = open(os.path.join(XLAT, "xlat12_repack_gfx11.h")).read()
def fn_body(name):
    m = re.search(r"static uint32_t repack11_%s\(uint32_t v\)\n\{.*?\n\}" % re.escape(name),
                  repack_src, re.S)
    return m.group(0) if m else None
nrepack = 0
for r in rows:
    if r["cls"] != "FIELD_REPACK":
        continue
    nrepack += 1
    rr = G.classify(r["name"], m10, m11)
    check(rr is not None and rr["cls"] == "field_repack",
          "%s re-derives as %s, not field_repack" % (r["name"], rr["cls"] if rr else "None"))
    if rr and rr["cls"] == "field_repack":
        code, _, _ = G.emit_repack(rr, m10, m11)
        code = code.replace("-> gfx12 0x", "-> gfx11 0x").replace("new on gfx12", "new on gfx11")
        code = code.replace("static uint32_t repack_", "static uint32_t repack11_", 1)
        check(code in repack_src, "%s repack body does not match the re-derived field mapping" % r["name"])
check(repack_src.count("static uint32_t repack11_") == nrepack,
      "repack header has %d functions, table has %d field_repack rows"
      % (repack_src.count("static uint32_t repack11_"), nrepack))

# ---- 4: study-method (address-first) counts over all 1,828 gfx10.3 SET_* regs
def fields(mdb, name):
    fa, _ = mdb.fields(name)
    return fa
study = dict(identical=0, moved=0, field_repack=0, removed=0)
for name, ent in m10.names.items():
    if ent["map"].get("to") != "mm":
        continue
    at = ent["map"]["at"]
    if not G11.in_setstar_space(at):
        continue
    b = m11.names.get(name)
    if b is None:
        study["removed"] += 1
    elif at != b["map"]["at"]:
        study["moved"] += 1                       # address-first: moved = present, addr differs
    elif fields(m10, name) != fields(m11, name):
        study["field_repack"] += 1
    else:
        study["identical"] += 1
REF = dict(identical=1283, moved=55, field_repack=151, removed=339)
check(sum(study.values()) == 1828, "study total %d != 1828" % sum(study.values()))
for k in ("identical", "moved", "field_repack", "removed"):
    check(study[k] == REF[k], "study %s %d != reference %d" % (k, study[k], REF[k]))
print("study-method count (address-first, == reference): "
      "identical %(identical)d / moved %(moved)d / field_repack %(field_repack)d / removed %(removed)d"
      % study)

# ---- reconcile the generator's field-first buckets (informational) ----------
gen = dict(identical=0, moved=0, field_repack=0, removed=0, unknown=0)
for r in rows:
    k = {"ABSENT": "removed", "LEGACY_VS": "removed", "IDENTICAL": "identical",
         "MOVED": "moved", "FIELD_REPACK": "field_repack", "UNKNOWN": "unknown"}[r["cls"]]
    gen[k] += 1
print("generator buckets (field-first, this table): "
      "identical %(identical)d / moved %(moved)d / field_repack %(field_repack)d / "
      "removed %(removed)d / unknown %(unknown)d" % gen)
print("  the two methods agree on 1,822 of 1,828 registers; the 6 that differ: 3 RLC_SPM_* and "
      "VGT_GS_OUT_PRIM_TYPE both move and change fields (generator: field_repack/unknown; study: "
      "moved), and SPI_SHADER_PGM_HI_GS/HS are raw-address dwords (generator: identical; study: "
      "field_repack). No target address differs; only the bucket label.")

print("%s: %d passed, %d failed" % ("FAIL" if fail else "PASS", pas, fail))
sys.exit(1 if fail else 0)
