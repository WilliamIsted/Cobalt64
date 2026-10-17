#!/bin/bash
# gmc-negative.sh: the GMC negative tests (hardware gate step 2 of stage3-gmc-design.md, overview repo). Every
# command here must be REFUSED by the kext before anything is written; a refusal never changes state. The kext
# logs each refusal as "n33: refused action A (why C)"; the script reads those lines back and compares them.
#
#   sudo tools/n33/gmc-negative.sh pre  [n33ctl]   fresh kext load: no G0, no step, no psp2 2c
#   sudo tools/n33/gmc-negative.sh post [n33ctl]   after G2 and before G3 (the GART table is owned)
#
# The BAR0 writes to the table page and mem_scratch are refused only once G2 has taken them (they join the
# protected list as kind 14 then; before G2 that VRAM is ordinary and a write would land). So `pre` skips
# them and says so, and `post` runs them. Each phase first checks the kext's state with `gmc flags`, which
# reads no register, and stops if the state is not the one its expected codes assume: a step this script
# expects to be refused would otherwise really run.
#
# Run it with output captured, e.g. ... 2>&1 | tee ~/ScratchPad/opencore-tahoe/gmc/gmc-neg-pre-output.txt
set -u
PHASE="${1:-}"
N33CTL="${2:-./n33ctl}"
[ "$PHASE" = pre ] || [ "$PHASE" = post ] || { echo "usage: $0 pre|post [n33ctl]" >&2; exit 2; }
[ -x "$N33CTL" ] || { echo "no n33ctl at $N33CTL" >&2; exit 2; }

FLAGS="$("$N33CTL" gmc flags)" || exit 1
echo "$FLAGS"
want() { echo "$FLAGS" | grep -q -- "$1" || { echo "refusing: the kext state does not show '$1' (phase $PHASE)"; exit 2; }; }
if [ "$PHASE" = pre ]; then
	want "kext: G0 0 "; want "done G1 0 G2 0 G3 0 G4 0 G6 0 "; want "stopped 0 "; want "GC up 0"
else
	want "done G1 1 G2 1 G3 0 G4 0 G6 0 "; want "table 1 "; want "stopped 0 "; want "GC up 0"
fi

# Refusal lines already in the kext's own log; this run's are the ones after them. The unified log is not used:
# it held none of the kext's lines from the 2026-10-16 and -17 boots (logd quarantined the kernel on 10-16).
BEFORE="$("$N33CTL" log | grep -c 'n33: refused action')"
EXPECT=()          # "action why" per test, in order
FAIL=0

# t <description> <expected action> <expected why> <n33ctl args...>
t() {
	local desc="$1" act="$2" why="$3"; shift 3
	echo "== $desc (expect action $act why $why)"
	if "$N33CTL" "$@"; then
		echo "!! ACCEPTED: $desc. STOP: this should have been refused"; FAIL=1
	fi
	EXPECT+=("$act $why")
}

if [ "$PHASE" = pre ]; then
	t "gmc step 3 before step 2"                    16 31 gmc step 3
	t "gmc step 6 before psp2 2c"                   16 37 gmc step 6
	t "gmc step 5 (G5 is the bind test, no step)"   16 30 gmc step 5
	t "gart map outside the GART (slot 65536)"      17 38 gmc map 65536 1 1 0x2000000
	t "gart map past the GART's end"                17 38 gmc map 65535 2 0 0
	t "gart map of VRAM on the GOP scanout"         17 2  gmc map 0 1 1 0x0
	t "gart map before G2 owns the table"           17 31 gmc map 0 1 0 0
	t "gart flush before G4"                        19 31 gmc flush
	t "gmc restore with nothing written"            20 31 gmc restore
	t "raw write to MMHUB SYSTEM_APERTURE_LOW"       1 6  regwrite 0x1a871 0x200000
	t "raw write to MMHUB VM_CONTEXT0_CNTL"          1 6  regwrite 0x1a6c0 0x1
	t "raw write to the HDP remap register"          1 6  regwrite 0xe4d 0x7f000
	echo "== skipped here: BAR0 writes to the GART table page and mem_scratch (refused only once G2 owns them; run post)"
else
	t "BAR0 write to the GART table page"            3 114 w 0x8800000 0
	t "BAR0 fill of mem_scratch"                     5 114 fill 0x8880000 16 0
	t "BAR0 copy onto the GART table"                4 114 copy 0x2000000 0x8800000 4096
	t "gart map of VRAM onto the table itself"      17 114 gmc map 0 1 1 0x8800000
	t "gart map of VRAM on the GOP scanout"         17 2  gmc map 0 1 1 0x0
	t "gart map outside the GART (slot 65536)"      17 38 gmc map 65536 1 1 0x2000000
	t "gmc step 2 a second time"                    16 31 gmc step 2
	t "gmc step 4 before step 3"                    16 31 gmc step 4
	t "gmc step 6 before psp2 2c"                   16 37 gmc step 6
	t "gart flush before G4"                        19 31 gmc flush
	t "raw write to MMHUB SYSTEM_APERTURE_LOW"       1 6  regwrite 0x1a871 0x200000
fi
"$N33CTL" gmc flags || FAIL=1

sleep 2
GOT=()
while IFS= read -r line; do GOT+=("$line"); done < <(
	"$N33CTL" log | sed -n 's/.*n33: refused action \([0-9]*\) (why \([0-9]*\)).*/\1 \2/p' | tail -n +$((BEFORE + 1)))
echo "== kext log: ${#GOT[@]} refusal line(s), ${#EXPECT[@]} expected"
for i in "${!EXPECT[@]}"; do
	g="${GOT[$i]:-none}"
	if [ "$g" = "${EXPECT[$i]}" ]; then r=ok; else r=MISMATCH; FAIL=1; fi
	echo "  [$i] expected ${EXPECT[$i]}  got $g  $r"
done
[ "${#GOT[@]}" -eq "${#EXPECT[@]}" ] || { echo "  count differs"; FAIL=1; }
if [ "$FAIL" = 0 ]; then echo "gmc-negative $PHASE: all refused as expected"; else echo "gmc-negative $PHASE: FAILED, read the lines above"; fi
exit "$FAIL"
