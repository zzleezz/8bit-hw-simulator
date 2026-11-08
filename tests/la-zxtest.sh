#!/bin/bash
# tests/la-zxtest.sh - the test ROM diagnosis (--la-diagnose --la-zxtest)
# over the real zxtest-* capture folders and a set of simulated faults;
# the verdicts compared with tests/la-zxtest-expected.txt.
#
#   tests/la-zxtest.sh [captures dir] [out dir]
#
# Captures dir: where the zxtest-NAME folders are (default ../Logic
# Analyser); they stay out of git. Each line of the result: the case, the
# nine tests (eight in captures of version 2: O ok, F fail, n none, - no
# result), then the verdict's numbered items' first lines. Faults joined
# by + go on one board. LA_JOBS=N cases at a time (default: half the
# logical processors).
# A changed line is printed; when every change is meant, copy OUT/all.txt
# over the expected file.
#
# AI-generated (Claude Code).
cd "$(dirname "$0")/.."
LA=${1:-"../Logic Analyser"}
OUT=${2:-/tmp/la-zxtest}
EXE=./c64-hw-sim.exe
[ -x "$EXE" ] || EXE=./c64-hw-sim
EXE=$(pwd)/${EXE#./}
rm -rf "$OUT"; mkdir -p "$OUT"
# half the logical processors: more gains little (the simulations share
# the caches and slow each other down)
NP=$(nproc 2>/dev/null || echo 8)
JOBS=${LA_JOBS:-$(( NP / 2 > 1 ? NP / 2 : 1 ))}

summary() {   # NAME OUTFILE
    local t
    t=$(grep -E '^  Test [0-9]: ' "$2" | awk '{s = substr($3,1,1); if ($3 == "no") s = "-"; printf "%s", s}')
    printf '%s  %s' "$1" "${t:-none}"
    grep -E '^  [0-9]+\. |^Nothing to look at|^No test' "$2" | sed 's/^  //' | cut -c1-110 | while IFS= read -r l; do printf ' | %s' "$l"; done
    echo
}

cases=()
for d in "$LA"/zxtest-*; do
    [ -d "$d" ] || continue
    n=$(basename "$d"); mkdir -p "$OUT/real_$n"; cp "$d"/*.lac "$OUT/real_$n/"
    b=issue2
    case $n in *-i1-*) b=issue1 ;; *-i3*) b=issue3b ;; *-i4*) b=issue4b ;; *-i6*) b=issue6a ;; esac
    cases+=("real_$n|--la-replay --board $b")
done
for f in healthy IC22:weak IC22:removed IC7.14:vcc IC8:data0=0 IC13:data0=0 IC6:addr2 IC15:addr3 IC3.4:gnd \
         IC25:removed IC1:nocontention R26:removed IC2.16:gnd IC2.34:vcc IC23:removed IC15:data0=0+IC16:data0=0; do
    n=sim_$(echo "$f" | tr ':.=@+' '_____')
    ff=; [ "$f" != healthy ] && ff="--fault $(echo "$f" | sed 's/+/ --fault /g')"
    cases+=("$n|--la-simulate --board issue2 $ff")
done
cases+=("sim_issue1|--la-simulate --board issue1" "sim_issue2_16k|--la-simulate --board issue2 --option ram=16k")

# a pool: the next case starts as soon as one ends (the simulated ones
# take 40 s alone, the real captures' replays a moment)
for c in "${cases[@]}"; do
    n=${c%%|*}; a=${c#*|}
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
    ( "$EXE" --la-diagnose "$OUT/$n" --la-zxtest $a > "$OUT/$n.out" 2>&1 ) &
done
wait
for c in "${cases[@]}"; do n=${c%%|*}; summary "$n" "$OUT/$n.out"; done > "$OUT/all.txt"
if [ -f tests/la-zxtest-expected.txt ]; then
    if diff tests/la-zxtest-expected.txt "$OUT/all.txt" > "$OUT/diff.txt"; then echo "All ${#cases[@]} as expected."
    else echo "Changed:"; cat "$OUT/diff.txt"; fi
else
    cat "$OUT/all.txt"
fi
