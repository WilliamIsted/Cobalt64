#!/bin/bash
# smu-negative.sh: the SMU mailbox negative tests (hardware gate step 2 of stage3-smu-handshake-design.md,
# overview repo). Every command here must be REFUSED by the kext before anything is written to the mailbox;
# none changes state (a refusal never latches anything). Run it on a fresh kext load, before `smu s1`'s
# first state-changing rung: the expected codes assume nothing is armed. The kext logs each refusal as
# "n33: refused action A (why C)"; the script reads those lines back and compares them with the codes.
#
#   sudo tools/n33/smu-negative.sh [n33ctl] 2>&1 | tee ~/ScratchPad/opencore-tahoe/psp2/smu-neg-output.txt
set -u
N33CTL="${1:-./n33ctl}"
[ -x "$N33CTL" ] || { echo "no n33ctl at $N33CTL" >&2; exit 2; }

START="$(date '+%Y-%m-%d %H:%M:%S')"
EXPECT=()
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

"$N33CTL" smu state || exit 1
t "Mode1Reset (0x2F)"                         12 23 smu send 0x2F 0
t "TransferTableDram2Smu (0x13)"              12 23 smu send 0x13 0
t "DisableAllSmuFeatures (0x7)"               12 23 smu send 0x7 0
t "EnableAllSmuFeatures before arming"        12 25 smu send 0x6 0
t "SetAllowedFeaturesMaskHigh unarmed"        12 25 smu send 0x5 0x0001a3a4
t "SetAllowedFeaturesMaskLow, another mask"   12 24 smu send 0x4 0x12345678
t "SetDriverDramAddrLow to the GOP scanout"   12 24 smu send 0xF 0x0
t "GetSmuVersion with a non-zero argument"    12 24 smu send 0x2 1
t "direct write to C2PMSG_66"                  1  6 regwrite 0x16282 0x2
t "direct write to C2PMSG_90"                  1  6 regwrite 0x1629A 0x0
"$N33CTL" smu state || FAIL=1

sleep 2
GOT=()
while IFS= read -r line; do GOT+=("$line"); done < <(
	log show --start "$START" --predicate 'process == "kernel"' 2>/dev/null |
	sed -n 's/.*n33: refused action \([0-9]*\) (why \([0-9]*\)).*/\1 \2/p')
echo "== kernel log: ${#GOT[@]} refusal line(s), ${#EXPECT[@]} expected"
for i in "${!EXPECT[@]}"; do
	g="${GOT[$i]:-none}"
	if [ "$g" = "${EXPECT[$i]}" ]; then r=ok; else r=MISMATCH; FAIL=1; fi
	echo "  [$i] expected ${EXPECT[$i]}  got $g  $r"
done
[ "${#GOT[@]}" -eq "${#EXPECT[@]}" ] || { echo "  count differs"; FAIL=1; }
if [ "$FAIL" = 0 ]; then echo "smu-negative: all refused as expected"; else echo "smu-negative: FAILED, read the lines above"; fi
exit "$FAIL"
