#!/bin/bash
# ih-negative.sh: the stage 3 step 4 negative tests (hardware gate step 2 of stage3-step4-ih-design.md, overview repo).
# Every command here must be REFUSED by the kext before anything is written; a refusal never changes state. The kext
# logs each refusal as "n33: refused action A (why C)"; the script reads those lines back and compares them.
#
#   sudo tools/n33/ih-negative.sh pre  [n33ctl]   fresh kext load: no psp2 2c, nothing of step 4
#   sudo tools/n33/ih-negative.sh post [n33ctl]   after ih start 1 (I1) succeeded, the step-6 queue running, no recv yet,
#                                                 and the boot without n33-ih-msi
#
# The BAR0 writes into the IH ring are refused only once ih start owns it (kind 19, why 119); before that the VRAM is
# ordinary and a write would land, so only `post` runs them. Each phase first checks the kext's state with `ih flags`,
# which reads no OSSSYS or GC register, and stops if the state is not the one its expected codes assume.
#
# Run it with output captured, e.g. ... 2>&1 | tee ~/ScratchPad/opencore-tahoe/ih/ih-neg-pre-output.txt
set -u
PHASE="${1:-}"
N33CTL="${2:-./n33ctl}"
[ "$PHASE" = pre ] || [ "$PHASE" = post ] || { echo "usage: $0 pre|post [n33ctl]" >&2; exit 2; }
[ -x "$N33CTL" ] || { echo "no n33ctl at $N33CTL" >&2; exit 2; }

FLAGS="$("$N33CTL" ih flags)" || exit 1
echo "$FLAGS"
want() { echo "$FLAGS" | grep -q -- "$1" || { echo "refusing: the kext state does not show '$1' (phase $PHASE)"; exit 2; }; }
case "$PHASE" in
pre)  want "ring owned 0 running 0 "; want "G6 0 GC up 0 " ;;
post) want "ring owned 1 running 1 mode polled armed 0 latch none "; want "cp running 1 "; want "no-IV 0 "
      want "n33-ih-msi 0 "; want "sources ok 0 " ;;
esac

BEFORE="$("$N33CTL" log | grep -c 'n33: refused action')"
EXPECT=()
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
	t "ih state registers before G6"               70 220 ih state
	t "ih start 1 before psp2 2c"                  71 43  ih start 1
	t "ih start with mode 3"                       71 226 ih start 3
	t "ih recv 0 before ih start"                  72 220 ih recv 0
	t "ih recv 9"                                  72 226 ih recv 9
	t "ih arm before ih start"                     73 220 ih arm
	t "ih stop with no ring"                       74 220 ih stop
	t "ih journal of kind 2"                       75 226 ih journal-kind 2
	t "raw write to IH_RB_CNTL"                     1 6   regwrite 0x1120 0x40110121
	t "raw write to IH_RB_BASE"                     1 6   regwrite 0x1121 0
	t "raw write to BIF_BX0_INTERRUPT_CNTL2"        1 6   regwrite 0xe12 0
	t "raw write to CP_ME1_PIPE0_INT_CNTL"          1 6   regwrite 0x3085 0x04000000
	t "raw write to SDMA0_CNTL"                     1 6   regwrite 0x127c 1
	echo "== skipped here: BAR0 writes into the IH ring (refused only once ih start owns it; run post)"
	;;
post)
	t "BAR0 write into the IH ring"                 3 119 w 0x8b00000 0
	t "BAR0 fill of the IH ring's last page"        5 119 fill 0x8b3f000 16 0
	t "gart map of VRAM onto the IH ring"          17 119 gmc map 0 1 1 0x8b00000
	t "a second ih start 1"                        71 220 ih start 1
	t "ih start 2 while the ring runs"             71 220 ih start 2
	t "ih recv 1 before recv 0 saw no IV"          72 220 ih recv 1
	t "ih recv 2 (SDMA) while the compute queue runs" 72 220 ih recv 2
	t "ih arm without n33-ih-msi"                  73 224 ih arm
	t "ih journal of kind 2"                       75 226 ih journal-kind 2
	t "raw write to IH_RB_RPTR"                     1 6   regwrite 0x1123 0
	;;
esac
"$N33CTL" ih flags || FAIL=1

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
if [ "$FAIL" = 0 ]; then echo "ih-negative $PHASE: all refused as expected"; else echo "ih-negative $PHASE: FAILED, read the lines above"; fi
exit "$FAIL"
