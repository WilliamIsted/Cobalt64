#!/bin/bash
# sdma-negative.sh: the stage 3 step 5 negative tests (hardware gate step 2 of stage3-step5-gfx-sdma-design.md, overview
# repo). Every command here must be REFUSED by the kext before anything is written; a refusal never changes state. The
# kext logs each refusal as "n33: refused action A (why C)"; the script reads those lines back and compares them.
#
#   sudo tools/n33/sdma-negative.sh pre  [n33ctl]   fresh kext load: no psp2 2c, nothing of step 5
#   sudo tools/n33/sdma-negative.sh gc   [n33ctl]   after psp2 2c and gmc g1 to g4, before gfx rs64 (C1)
#   sudo tools/n33/sdma-negative.sh post [n33ctl]   after sdma start mmio (E1), before sdma test 1
#
# The BAR0 writes to the step-5 pages are refused only once E1 owns them (kind 15); before that the VRAM is ordinary
# and a write would land, so only `post` runs them. Each phase first checks the kext's state with `sdma flags` and
# `gmc flags`, which read no GC register, and stops if the state is not the one its expected codes assume: a command
# this script expects to be refused would otherwise really run. `gmc restore` is not tried in `post`: no test has run,
# so no engine holds a GART address yet and the restore would really run.
#
# Run it with output captured, e.g. ... 2>&1 | tee ~/ScratchPad/opencore-tahoe/step5/sdma-neg-pre-output.txt
set -u
PHASE="${1:-}"
N33CTL="${2:-./n33ctl}"
[ "$PHASE" = pre ] || [ "$PHASE" = gc ] || [ "$PHASE" = post ] || { echo "usage: $0 pre|gc|post [n33ctl]" >&2; exit 2; }
[ -x "$N33CTL" ] || { echo "no n33ctl at $N33CTL" >&2; exit 2; }

FLAGS="$("$N33CTL" sdma flags)" || exit 1
GFLAGS="$("$N33CTL" gmc flags)" || exit 1
echo "$FLAGS"; echo "$GFLAGS"
want() { echo "$FLAGS" | grep -q -- "$1" || { echo "refusing: the kext state does not show '$1' (phase $PHASE)"; exit 2; }; }
gwant() { echo "$GFLAGS" | grep -q -- "$1" || { echo "refusing: the GMC state does not show '$1' (phase $PHASE)"; exit 2; }; }
case "$PHASE" in
pre)  want "C1 attempted 0 done 0 "; want "pages 0 running 0 "; want "GC up 0" ;;
gc)   want "C1 attempted 0 done 0 "; want "running 0 "; want "fw pinned 1 "; want "GC up 1"
      gwant "G4 1 G6 0 "; gwant "stopped 0 " ;;
post) want "C1 attempted 1 done 1 "; want "running 1 mode mmio latch none "; want "tests ok 0,"; want "G6 1 " ;;
esac

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

case "$PHASE" in
pre)
	t "gfx rs64 before psp2 2c"                    23 43 gfx rs64
	t "sdma state before psp2 2c"                  24 43 sdma state
	t "sdma start mmio before psp2 2c"             25 43 sdma start mmio
	t "sdma start with mode 9"                     25 53 sdma start 9
	t "sdma test 2 before start"                   26 44 sdma test 2
	t "sdma test 9"                                26 53 sdma test 9
	t "sdma stop with no queue"                    27 44 sdma stop
	t "raw write to SDMA0_QUEUE0_RB_CNTL"           1 6  regwrite 0x12e0 0
	t "raw write to SDMA0_F32_CNTL"                 1 6  regwrite 0xf89a 0
	t "raw write to GRBM_GFX_CNTL"                  1 6  regwrite 0xa900 0
	t "raw write to RSMU_INDEX"                     1 6  regwrite 0x14 0
	t "raw write to SDMA0_QUEUE0_DOORBELL"          1 6  regwrite 0x12f2 0
	echo "== skipped here: BAR0 writes to the step-5 pages (refused only once E1 owns them; run post)"
	;;
gc)
	t "gmc step 6 before gfx rs64"                 16 44 gmc step 6
	t "sdma start mmio before gmc g6"              25 44 sdma start mmio
	t "sdma start db before gmc g6"                25 44 sdma start db
	t "sdma test 1 before start"                   26 44 sdma test 1
	t "sdma stop with no queue"                    27 44 sdma stop
	;;
post)
	t "BAR0 write to the SDMA ring"                 3 115 w 0x8a00000 0
	t "BAR0 fill of the wb page"                    5 115 fill 0x8a01000 16 0
	t "BAR0 copy onto the VRAM test page"           4 115 copy 0x2000000 0x8a02000 4096
	t "gart map of VRAM onto the SDMA ring"        17 115 gmc map 0 1 1 0x8a00000
	t "a second sdma start mmio"                   25 44 sdma start mmio
	t "sdma start db while mmio runs"              25 51 sdma start db
	t "sdma test 2 before test 1"                  26 44 sdma test 2
	t "sdma test 3 before test 2"                  26 44 sdma test 3
	t "sdma test 9"                                26 53 sdma test 9
	t "gfx rs64 a second time (after G6)"          23 44 gfx rs64
	t "gmc step 6 a second time"                   16 31 gmc step 6
	t "raw write to SDMA0_QUEUE0_RB_WPTR"           1 6  regwrite 0x12e5 0x40
	t "raw write to SDMA0_F32_CNTL"                 1 6  regwrite 0xf89a 1
	;;
esac
"$N33CTL" sdma flags || FAIL=1

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
if [ "$FAIL" = 0 ]; then echo "sdma-negative $PHASE: all refused as expected"; else echo "sdma-negative $PHASE: FAILED, read the lines above"; fi
exit "$FAIL"
