#!/usr/bin/env python3
"""Source cross-check for xlat12_gfx11.h (stage-3 step 7, X3).

  1. Every XLAT12_G11_<NAME> address equals <NAME>'s address in BOTH Mesa gfx11.json and Linux
     gc_11_0_0_offset.h (byte = (base_idx base + dword offset) * 4, bases derived as in X1's test).
  2. CB_COLOR1..7_ATTRIB3 sit at XLAT12_G11_CB_COLOR0_ATTRIB3 + 4n (the draw policy indexes them so).
  3. kXlat12SwModeG10ToG11 is exactly the identity over the modes addrlib's Gfx10Lib and Gfx11Lib
     SwizzleModeTables agree on (gen_golden_gfx11.swmode_ok_set, parsed from the .cpp sources).
  4. XLAT12_G11V_ATTRIBUTE_RING_SIZE decodes, by gfx11.json's field list, to MEM_SIZE 0x15, BIG_PAGE 1,
     L1_POLICY 1 and nothing else; the gfx12-only registers the gfx11 path leaves out are absent from
     gfx11.json; the scissor registers have the same address and fields on gfx10.3 and gfx11.

Run:  python3 src/xlat12/tests/test_xlat12_ib_gfx11.py
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
XLAT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(XLAT, "gen"))
import gen_tables as G
import gen_tables_gfx11 as G11
import gen_golden_gfx11 as O

fail = pas = 0
def check(cond, msg):
    global fail, pas
    if cond: pas += 1
    else:
        fail += 1
        print("FAIL: " + msg)

hdr = open(os.path.join(XLAT, "xlat12_gfx11.h")).read()
m10 = G.MesaDB(os.path.join(G11.DEF_MESA, "gfx103.json"))
m11 = G.MesaDB(os.path.join(G11.DEF_MESA, "gfx11.json"))
l11, l11line = G.load_linux(G11.DEF_LINUX, "gc_11_0_0_offset.h", "reg")
b11, _ = G.derive_bases(m11, l11)

# 1
addrs = re.findall(r"#define XLAT12_G11_(\w+)\s+0x([0-9a-fA-F]+)u", hdr)
check(len(addrs) >= 40, "parsed only %d XLAT12_G11_ addresses" % len(addrs))
for nm, v in addrs:
    v = int(v, 16)
    e = m11.names.get(nm)
    check(e is not None and e["map"].get("to") == "mm" and e["map"]["at"] == v,
          "%s 0x%05x: gfx11.json %s" % (nm, v, "0x%05x" % e["map"]["at"] if e else "absent"))
    la = G.linux_addr(l11, b11, nm)
    check(la == v, "%s 0x%05x: gc_11_0_0_offset.h %s" % (nm, v, "0x%05x" % la if la else "absent"))
    # the cited [line] is the register's line in the Linux header
    cm = re.search(r"#define XLAT12_G11_%s\s+0x[0-9a-fA-F]+u\s+/\* \[(\d+)\]" % nm, hdr)
    if cm:
        check(l11line.get(G.linux_name(l11, nm)) == int(cm.group(1)),
              "%s cites gc_11_0_0_offset.h line %s, the header has it at %s" % (nm, cm.group(1), l11line.get(nm)))
base_a3 = int(dict(addrs)["CB_COLOR0_ATTRIB3"], 16)
# 2
for n in range(1, 8):
    e = m11.names.get("CB_COLOR%d_ATTRIB3" % n)
    check(e is not None and e["map"]["at"] == base_a3 + 4 * n, "CB_COLOR%d_ATTRIB3 not at base + %d" % (n, 4 * n))

# 3
pairs = [(int(a), int(b)) for a, b in re.findall(r"\{ (\d+)u, (\d+)u \},", hdr.split("kXlat12SwModeG10ToG11")[1].split("};")[0])]
ok = O.swmode_ok_set(G11.DEF_MESA)
check(sorted(pairs) == sorted((m, m) for m in ok), "SW_MODE table %s != addrlib identity over %s" % (pairs, sorted(ok)))
cnt = int(re.search(r"#define XLAT12_SWMODE_G11_MAP_COUNT (\d+)u", hdr).group(1))
check(cnt == len(pairs), "XLAT12_SWMODE_G11_MAP_COUNT %d != %d rows" % (cnt, len(pairs)))
for f in ("COLOR_SW_MODE",):
    check(("COLOR_SW_MODE", 14, 18) in m11.fields("CB_COLOR0_ATTRIB3")[0], "gfx11 COLOR_SW_MODE not at [18:14]")
for r in ("DB_Z_INFO", "DB_STENCIL_INFO"):
    check(("SW_MODE", 4, 8) in m11.fields(r)[0] and ("SW_MODE", 4, 8) in m10.fields(r)[0], "%s SW_MODE not at [8:4] on both" % r)

# 4
v = int(re.search(r"#define XLAT12_G11V_ATTRIBUTE_RING_SIZE\s+0x([0-9a-fA-F]+)u", hdr).group(1), 16)
fl = {n: (v >> lo) & ((1 << (hi - lo + 1)) - 1) for n, lo, hi in m11.fields("SPI_ATTRIBUTE_RING_SIZE")[0]}
check(fl == {"MEM_SIZE": 0x15, "BIG_PAGE": 1, "L1_POLICY": 1, "L2_POLICY": 0, "LLC_NOALLOC": 0, "GL1_PERF_COUNTER_DISABLE": 0},
      "SPI_ATTRIBUTE_RING_SIZE 0x%08x decodes to %s" % (v, fl))
covered = 0
for n, lo, hi in m11.fields("SPI_ATTRIBUTE_RING_SIZE")[0]: covered |= ((1 << (hi - lo + 1)) - 1) << lo
check(v & ~covered == 0, "SPI_ATTRIBUTE_RING_SIZE 0x%08x sets bits outside its fields" % v)
for r in ("GE_POS_RING_BASE", "GE_POS_RING_SIZE", "GE_PRIM_RING_BASE", "GE_PRIM_RING_SIZE", "DB_SPI_VRS_CENTER_LOCATION",
          "SPI_SHADER_GS_OUT_CONFIG_PS", "PA_SC_HISZ_CONTROL"):
    check(r not in m11.names, "%s is in gfx11.json: the gfx11 path leaves it out" % r)
for r in ("PA_SC_SCREEN_SCISSOR_BR", "PA_SC_WINDOW_SCISSOR_BR", "PA_SC_GENERIC_SCISSOR_BR", "PA_SC_VPORT_SCISSOR_0_BR",
          "PA_SC_VPORT_SCISSOR_15_BR"):
    check(m10.names[r]["map"]["at"] == m11.names[r]["map"]["at"] and m10.fields(r)[0] == m11.fields(r)[0],
          "%s differs between gfx10.3 and gfx11" % r)

print("%s: %d passed, %d failed (%d XLAT12_G11_ addresses, SW_MODE identity over %d modes)" %
      ("PASS" if not fail else "FAIL", pas, fail, len(addrs), len(pairs)))
sys.exit(1 if fail else 0)
