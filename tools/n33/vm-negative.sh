#!/bin/bash
# vm-negative.sh: the stage 3 step 7 W2V negative tests (design stage3-step7-gpuvm-design.md, "vm-negative.sh").
# Every command here must be REFUSED by the kext before anything is written; a refusal never changes state. The kext
# logs each refusal as "n33: refused action A (why C)"; the script reads those lines back and compares them.
#
#   sudo tools/n33/vm-negative.sh pre   [n33ctl]            before G6: every state-changing vm action refused 76
#   sudo tools/n33/vm-negative.sh post  [n33ctl]            after G6 and K1, before vm create: every action but create refused 76
#   sudo tools/n33/vm-negative.sh owned [n33ctl] [<ib va>]  after vm create and V2's maps: the arena is protected (121), the
#                                                           context is one VM (76 on a second create), VA/flag checks (75),
#                                                           an overlap (79), bo free of a mapped BO (84), fault aperture (85)
#
# `owned` creates a throwaway gtt BO, maps it at a free low VA for the overlap/VA/free tests, then unmaps and frees it, so
# it leaves the card as it found it. Pass the runner's R|X VRAM IB VA as the 3rd arg to also test the fault aperture (85);
# without it that one test is skipped (it needs an R|X mapping to resolve the IB before the aperture check).
set -u
PHASE="${1:-}"
N33CTL="${2:-./n33ctl}"
IBVA="${3:-}"
case "$PHASE" in pre|post|owned) ;; *) echo "usage: $0 pre|post|owned [n33ctl] [ib va]" >&2; exit 2 ;; esac
[ -x "$N33CTL" ] || { echo "no n33ctl at $N33CTL" >&2; exit 2; }

PROT="$("$N33CTL" protected)" || exit 1
echo "$PROT"
has_arena() { echo "$PROT" | grep -q "W2V page-table arena"; }
case "$PHASE" in
pre|post) has_arena && { echo "refusing: the page-table arena is already owned (phase $PHASE)"; exit 2; } ;;
owned)    has_arena || { echo "refusing: the arena is not owned yet (phase owned: run vm create first)"; exit 2; } ;;
esac

IB=0xFFFF800000100000      # a canonical high VA used only for the map-argument checks (the BO need not cover it here)
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
	t "vm create before G6"                  48 76  vm create
	t "vm map before create"                 49 76  vm map 1 $IB
	t "vm unmap before create"               50 76  vm unmap 1 $IB
	t "vm copy before create"                52 76  vm copy $IB $IB $IB
	t "vm dispatch before create"            53 76  vm dispatch $IB $IB $IB
	t "vm fault before create"               54 76  vm fault $IB 0xFFFFA00000000000
	t "vm destroy before create"             55 76  vm destroy
	;;
post)
	t "vm map before create"                 49 76  vm map 1 $IB
	t "vm unmap before create"               50 76  vm unmap 1 $IB
	t "vm copy before create"                52 76  vm copy $IB $IB $IB
	t "vm dispatch before create"            53 76  vm dispatch $IB $IB $IB
	t "vm fault before create"               54 76  vm fault $IB 0xFFFFA00000000000
	t "vm destroy before create"             55 76  vm destroy
	;;
owned)
	CR="$("$N33CTL" bo create gtt 0x10000)" || { echo "owned: could not create the spare gtt BO"; exit 1; }
	echo "$CR"
	SPARE="$(echo "$CR" | sed -n 's/.*handle \([0-9][0-9]*\).*/\1/p')"
	[ -n "$SPARE" ] || { echo "owned: no handle from create"; exit 1; }
	LOWVA=0x100000            # a free canonical low VA for the spare, clear of V2's maps
	"$N33CTL" vm map "$SPARE" "$LOWVA" >/dev/null || { echo "owned: could not map the spare BO at $LOWVA"; "$N33CTL" bo free "$SPARE"; exit 1; }
	t "BAR0 write into the page-table arena"  3 121 w 0xD000000 0
	t "BAR0 fill of the page-table arena"     5 121 fill 0xD000000 16 0
	t "a second vm create"                   48 76  vm create
	t "an overlapping map onto the spare"    49 79  vm map "$SPARE" "$LOWVA"
	t "a non-canonical map VA"               49 75  vm map "$SPARE" 0x0000800000000000
	t "a map VA straddling bit 47"           49 75  vm map "$SPARE" 0x7FFFFFFFF000
	t "a map with an unknown flag bit"       49 75  vm map "$SPARE" 0x200000 0 0 0x402
	t "bo free the spare while it is mapped" 43 84  bo free "$SPARE"
	if [ -n "$IBVA" ]; then
		t "vm fault at a VMID-0 aperture VA"  54 85  vm fault "$IBVA" 0x8400000000
	fi
	"$N33CTL" vm unmap "$SPARE" "$LOWVA" || echo "owned: note: could not unmap the spare BO"
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
if [ "$FAIL" = 0 ]; then echo "vm-negative $PHASE: all refused as expected"; else echo "vm-negative $PHASE: FAILED, read the lines above"; fi
exit "$FAIL"
