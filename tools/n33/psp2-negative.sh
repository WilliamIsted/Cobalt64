#!/bin/bash
# psp2-negative.sh: the PSP milestone 2 negative tests (hardware gate step 2 of
# stage3-psp-m2-design.md, overview repo). Every command here must be REFUSED by the kext; none changes
# state (a refusal never latches failure). The kext logs each refusal as "n33: refused action A (why C)";
# the script reads those lines back and compares them with the expected codes.
#
#   sudo tools/n33/psp2-negative.sh pre  [n33ctl]   before 2a (no ring yet)
#   sudo tools/n33/psp2-negative.sh post [n33ctl]   after 2a, before 2b (ring and LOAD_TOC done)
#
# Run it with output captured, e.g. ... 2>&1 | tee ~/ScratchPad/opencore-tahoe/psp2/neg-pre-output.txt
set -u
PHASE="${1:-}"
N33CTL="${2:-./n33ctl}"
[ "$PHASE" = pre ] || [ "$PHASE" = post ] || { echo "usage: $0 pre|post [n33ctl]" >&2; exit 2; }
[ -x "$N33CTL" ] || { echo "no n33ctl at $N33CTL" >&2; exit 2; }

FWBUF=0x8200000
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

"$N33CTL" psp2 state || exit 1
if [ "$PHASE" = pre ]; then
	t "LOAD_TOC with no ring"                9 15 psp2 send 1 0 0 2304
	t "LOAD_IP_FW DMUB with no ring"         9 15 psp2 send 2 51 $FWBUF 4096
	t "destroy with no ring"                10 15 psp2 destroy
else
	t "LOAD_IP_FW DMUB (51)"                 9 17 psp2 send 2 51 $FWBUF 4096
	t "LOAD_IP_FW VCN (13)"                  9 17 psp2 send 2 13 $FWBUF 4096
	t "LOAD_IP_FW PPTABLE (73)"              9 17 psp2 send 2 73 $FWBUF 4096
	t "SETUP_TMR before the SMU image"       9 16 psp2 send 3 0 0 0
	t "AUTOLOAD_RLC before RLC_G"            9 16 psp2 send 4 0 0 0
	t "GC image (SDMA TH0) before the TMR"   9 16 psp2 send 2 71 $FWBUF 0x4400
	t "LOAD_TOC a second time"               9 16 psp2 send 1 0 0 2304
	t "SMU image not page aligned"           9 18 psp2 send 2 18 0x8200010 4096
	t "SMU image past the end of fw_buf"     9 18 psp2 send 2 18 0x8500000 4096
	t "a second ring create"                 8 22 psp2 ring
	t "BAR0 write to the ring page"          3 112 w 0x8100000 0
	t "BAR0 fill of the fence page"          5 112 fill 0x8102000 16 0
fi
"$N33CTL" psp2 state || FAIL=1

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
if [ "$FAIL" = 0 ]; then echo "psp2-negative $PHASE: all refused as expected"; else echo "psp2-negative $PHASE: FAILED, read the lines above"; fi
exit "$FAIL"
