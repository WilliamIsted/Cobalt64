#!/bin/bash
# cp-negative.sh: the stage 3 step 6 negative tests (hardware gate step 2 of stage3-step6-compute-design.md, overview
# repo). Every command here must be REFUSED by the kext before anything is written; a refusal never changes state. The
# kext logs each refusal as "n33: refused action A (why C)"; the script reads those lines back and compares them.
#
#   sudo tools/n33/cp-negative.sh pre   [n33ctl]   fresh kext load: no psp2 2c, nothing of step 6
#   sudo tools/n33/cp-negative.sh gc    [n33ctl]   after psp2 2c, gmc g1 to g4 and gfx rs64 (C1), before gmc g6
#   sudo tools/n33/cp-negative.sh post  [n33ctl]   after cp start db or mmio (K1), before cp test 1
#   sudo tools/n33/cp-negative.sh ibown [n33ctl]   after a successful cp ib (step 7 T1), the IB page owned (kind 17)
#
# The BAR0 writes to the step-6 pages are refused only once K1 owns them (kind 16, why 116); before that the VRAM is
# ordinary and a write would land, so only `post` runs them. Each phase first checks the kext's state with `cp flags`,
# which reads no GC register, and stops if the state is not the one its expected codes assume: a command this script
# expects to be refused would otherwise really run. `sdma start mmio` in `post` relies on the SDMA steps not having run
# in this load (Q9: a fresh boot with them skipped); the phase checks that.
#
# Run it with output captured, e.g. ... 2>&1 | tee ~/ScratchPad/opencore-tahoe/step6/cp-neg-pre-output.txt
set -u
PHASE="${1:-}"
N33CTL="${2:-./n33ctl}"
[ "$PHASE" = pre ] || [ "$PHASE" = gc ] || [ "$PHASE" = post ] || [ "$PHASE" = ibown ] || { echo "usage: $0 pre|gc|post|ibown [n33ctl]" >&2; exit 2; }
[ -x "$N33CTL" ] || { echo "no n33ctl at $N33CTL" >&2; exit 2; }

FLAGS="$("$N33CTL" cp flags)" || exit 1
SFLAGS="$("$N33CTL" sdma flags)" || exit 1
echo "$FLAGS"; echo "$SFLAGS"
want() { echo "$FLAGS" | grep -q -- "$1" || { echo "refusing: the kext state does not show '$1' (phase $PHASE)"; exit 2; }; }
swant() { echo "$SFLAGS" | grep -q -- "$1" || { echo "refusing: the step-5 state does not show '$1' (phase $PHASE)"; exit 2; }; }
case "$PHASE" in
pre)  want "pages 0 running 0 "; want "GC up 0" ;;
gc)   want "pages 0 running 0 "; want "C1 1 G6 0 "; want "GC up 1" ;;
post) want "running 1 mode "; want "tests ok 0,"; want "C1 1 G6 1 "; swant "pages 0 running 0 "
      # The start tests depend on the running mode: the same mode is out of order (54), the other one a switch (58).
      if echo "$FLAGS" | grep -q -- "running 1 mode db latch none "; then MODE=db; OTHER=mmio
      elif echo "$FLAGS" | grep -q -- "running 1 mode mmio latch none "; then MODE=mmio; OTHER=db
      else want "running 1 mode db|mmio latch none "; fi ;;
ibown) want "running 1 "; want "latch none "; want "IB page owned 1" ;;
esac

# Refusal lines already in the kext's own log; this run's are the ones after them.
BEFORE="$("$N33CTL" log | grep -c 'n33: refused action')"
EXPECT=()          # "action why" per test, in order
FAIL=0

# t <description> <expected action> <expected why[|other why]> <n33ctl args...>
t() {
	local desc="$1" act="$2" why="$3" e="" w; shift 3
	echo "== $desc (expect action $act why $why)"
	if "$N33CTL" "$@"; then
		echo "!! ACCEPTED: $desc. STOP: this should have been refused"; FAIL=1
	fi
	for w in ${why//|/ }; do e+="${e:+|}$act $w"; done
	EXPECT+=("$e")
}

case "$PHASE" in
pre)
	t "cp state before psp2 2c"                    29 43 cp state
	t "cp hqd before psp2 2c"                      30 43 cp hqd
	t "cp start db before psp2 2c"                 31 43 cp start db
	t "cp start with mode x"                       31 60 cp start x
	t "cp test 9"                                  32 60 cp test 9
	t "cp test 1 before start"                     32 54 cp test 1
	t "cp ib before start"                         35 62 cp ib
	t "cp submit before start"                     36 62 cp submit
	t "cp waitseq before start"                    37 62 cp waitseq 1
	t "cp stop with no queue"                      33 54 cp stop
	t "raw write to CP_MEC_RS64_CNTL"               1 6  regwrite 0xc904 0
	t "raw write to CP_HQD_ACTIVE"                  1 6  regwrite 0x320b 1
	t "raw write to GRBM_GFX_CNTL"                  1 6  regwrite 0xa900 4
	t "raw write to CP_HQD_PQ_DOORBELL_CONTROL"     1 6  regwrite 0x3218 0
	t "raw write to RSMU_INDEX"                     1 6  regwrite 0x14 0
	echo "== skipped here: BAR0 writes to the step-6 pages (refused only once K1 owns them; run post)"
	;;
gc)
	t "cp start db before gmc g6"                  31 54 cp start db
	t "cp ib before start"                         35 62 cp ib
	t "cp submit before start"                     36 62 cp submit
	;;
post)
	t "BAR0 write to the compute ring"              3 116 w 0x8a05000 0
	t "BAR0 fill of the wb page"                    5 116 fill 0x8a06000 16 0
	t "BAR0 copy onto the code page"                4 116 copy 0x2000000 0x8a07000 4096
	t "gart map of VRAM onto the compute ring"     17 116 gmc map 0 1 1 0x8a05000
	# 43 is also right: the start gates check CP_STAT == 0 first, and the active MEC pipe may hold it non-zero.
	t "a second cp start $MODE"                    31 "54|43" cp start "$MODE"
	t "cp start $OTHER while $MODE runs"               31 "58|43" cp start "$OTHER"
	t "cp test 2 before test 1"                    32 54 cp test 2
	t "cp test 3 before test 2"                    32 54 cp test 3
	t "cp test 9"                                  32 60 cp test 9
	t "cp ib before a ring packet (K2/K3)"         35 62 cp ib
	t "cp submit before a ring packet (K2/K3)"     36 62 cp submit
	t "cp waitseq with no submit outstanding"      37 64 cp waitseq 1
	t "sdma start mmio while the compute queue runs" 25 "44|43" sdma start mmio
	t "raw write to CP_HQD_PQ_WPTR_LO"              1 6  regwrite 0x323f 0x100
	t "raw write to CP_MEC_RS64_CNTL"               1 6  regwrite 0xc904 0x40000000
	echo "== skipped here: BAR0 writes to the IB page (refused only once cp ib owns it; run ibown)"
	;;
ibown)
	# The IB page (VRAM 0x8a09000) is protected kind 17 only after a successful cp ib; why 117 = 100 + 17.
	t "BAR0 write into the IB page"                 3 117 w 0x8a09000 0
	t "gart map of VRAM onto the IB page"          17 117 gmc map 0 1 1 0x8a09000
	;;
esac
"$N33CTL" cp flags || FAIL=1

sleep 2
GOT=()
while IFS= read -r line; do GOT+=("$line"); done < <(
	"$N33CTL" log | sed -n 's/.*n33: refused action \([0-9]*\) (why \([0-9]*\)).*/\1 \2/p' | tail -n +$((BEFORE + 1)))
echo "== kext log: ${#GOT[@]} refusal line(s), ${#EXPECT[@]} expected"
for i in "${!EXPECT[@]}"; do
	g="${GOT[$i]:-none}"
	case "|${EXPECT[$i]}|" in *"|$g|"*) r=ok ;; *) r=MISMATCH; FAIL=1 ;; esac
	echo "  [$i] expected ${EXPECT[$i]}  got $g  $r"
done
[ "${#GOT[@]}" -eq "${#EXPECT[@]}" ] || { echo "  count differs"; FAIL=1; }
if [ "$FAIL" = 0 ]; then echo "cp-negative $PHASE: all refused as expected"; else echo "cp-negative $PHASE: FAILED, read the lines above"; fi
exit "$FAIL"
