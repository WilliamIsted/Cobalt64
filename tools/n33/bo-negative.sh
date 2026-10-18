#!/bin/bash
# bo-negative.sh: the stage 3 step 7 W2 negative tests (hardware run order step 2/3 of stage3-step7-w2-design.md).
# Every command here must be REFUSED by the kext before anything is written; a refusal never changes state. The kext
# logs each refusal as "n33: refused action A (why C)"; the script reads those lines back and compares them.
#
#   sudo tools/n33/bo-negative.sh pre   [n33ctl]   fresh kext load: no G6, no BO; arg checks and the reserved window
#   sudo tools/n33/bo-negative.sh post  [n33ctl]   after G6 and K1, no BO yet: every BO command refused, nothing written
#   sudo tools/n33/bo-negative.sh owned [n33ctl]   after bo create vram h1 (mapped at 0x8401000000) and a spare gtt BO:
#                                                  the slab is protected (kind 20) and the window reserved
#
# The BAR0 writes into the slab are refused only once a VRAM BO owns it (kind 20, why 120); before that the VRAM is
# ordinary and a write would land, so only `owned` runs them. The reserved VA window (gart map/unmap, why 73) is
# refused in every phase. `owned` creates a throwaway gtt BO for the overlap test and frees it again, so it leaves the
# card as it found it.
set -u
PHASE="${1:-}"
N33CTL="${2:-./n33ctl}"
case "$PHASE" in pre|post|owned) ;; *) echo "usage: $0 pre|post|owned [n33ctl]" >&2; exit 2 ;; esac
[ -x "$N33CTL" ] || { echo "no n33ctl at $N33CTL" >&2; exit 2; }

PROT="$("$N33CTL" protected)" || exit 1
echo "$PROT"
has_slab() { echo "$PROT" | grep -q "W2 VRAM BO slab"; }
case "$PHASE" in
pre|post) has_slab && { echo "refusing: the VRAM BO slab is already owned (phase $PHASE)"; exit 2; } ;;
owned)    has_slab || { echo "refusing: the VRAM BO slab is not owned yet (phase owned: create and map a VRAM BO first)"; exit 2; } ;;
esac

VA0=0x8401000000      # where the runner maps h1
VA1=0x8401100000      # where the runner maps h2
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
	t "bo create kind 3"                     39 66  bo create 3 0x10000
	t "bo create vram size 0"                39 66  bo create vram 0
	t "bo create vram unaligned"             39 66  bo create vram 0x1001
	t "bo create vram over 1 MiB"            39 66  bo create vram 0x101000
	t "bo create vram before G6"             39 67  bo create vram 0x10000
	t "bo create gtt before G6"              39 67  bo create gtt 0x10000
	t "bo map a nonexistent handle"          41 66  bo map 1 $VA0
	t "bo unmap a nonexistent handle"        42 66  bo unmap 1
	t "bo free a nonexistent handle"         43 66  bo free 1
	t "bo info a nonexistent handle"         44 66  bo info 1
	t "bo read a nonexistent handle"         45 66  bo read 1 0
	t "bo copy nonexistent handles"          46 66  bo copy 1 2
	t "bo fault an unaligned VA"             47 66  bo fault 0x8401000004
	t "bo fault before G6"                   47 67  bo fault 0x8401FFF000
	t "gart map into the BO VA window"       17 73  gmc map 0x1000 1 1 0x8000000
	t "gart unmap into the BO VA window"     18 73  gmc unmap 0x1800 1
	;;
post)
	t "bo create kind 3"                     39 66  bo create 3 0x10000
	t "bo create vram size 0"                39 66  bo create vram 0
	t "bo create vram over 1 MiB"            39 66  bo create vram 0x101000
	t "bo map a nonexistent handle"          41 66  bo map 1 $VA0
	t "bo unmap a nonexistent handle"        42 66  bo unmap 1
	t "bo free a nonexistent handle"         43 66  bo free 1
	t "bo copy nonexistent handles"          46 66  bo copy 1 2
	t "bo fault an unaligned VA"             47 66  bo fault 0x8401000004
	t "gart map into the BO VA window"       17 73  gmc map 0x1000 1 1 0x8000000
	t "gart unmap into the BO VA window"     18 73  gmc unmap 0x1800 1
	;;
owned)
	# A spare, unmapped gtt BO for the overlap test; its handle is whatever create returns.
	CR="$("$N33CTL" bo create gtt 0x10000)" || { echo "owned: could not create the spare gtt BO"; exit 1; }
	echo "$CR"
	SPARE="$(echo "$CR" | sed -n 's/.*handle \([0-9][0-9]*\).*/\1/p')"
	[ -n "$SPARE" ] || { echo "owned: no handle from create"; exit 1; }
	t "BAR0 write into the VRAM slab"         3 120 w 0xC000000 0
	t "BAR0 fill of the VRAM slab"            5 120 fill 0xC000000 16 0
	t "gart map into the BO VA window"       17 73  gmc map 0x1000 1 1 0x8000000
	t "gart unmap into the BO VA window"     18 73  gmc unmap 0x1000 1
	t "a second map of h1"                   41 67  bo map 1 $VA1
	t "an overlapping map onto h1"           41 70  bo map "$SPARE" $VA0
	t "free h1 while it is mapped"           43 67  bo free 1
	"$N33CTL" bo free "$SPARE" || echo "owned: note: could not free the spare gtt BO h$SPARE"
	;;
esac

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
if [ "$FAIL" = 0 ]; then echo "bo-negative $PHASE: all refused as expected"; else echo "bo-negative $PHASE: FAILED, read the lines above"; fi
exit "$FAIL"
