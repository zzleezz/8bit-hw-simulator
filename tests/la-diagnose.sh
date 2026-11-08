#!/bin/bash
# tests/la-diagnose.sh - the logic analyser diagnosis (--la-diagnose) run
# over every real capture folder and a set of simulated faults, and each
# verdict compared with the last good run (tests/la-diagnose-expected.txt).
#
#   tests/la-diagnose.sh [captures dir] [out dir]
#
# Captures dir: where diagnose.bat puts its diagnosis-NAME folders (and
# Old/diagnosis-NAME); default ../Logic Analyser. They stay out of git
# (5 MB a capture): a folder missing here shows as GONE, a new one as NEW.
# The folders are replayed from copies (their diagnosis.txt untouched).
# Each case is a process: all cores (LA_JOBS=N for fewer), about 7 minutes on 14.
#
# Then: tests/la-compare.py tests/la-diagnose-expected.txt OUT/all.txt
# prints what changed. When every change is meant, make OUT/all.txt the
# new expected file.
#
# AI-generated (Claude Code).
cd "$(dirname "$0")/.."
LA=${1:-"../Logic Analyser"}
OUT=${2:-/tmp/la-diagnose}
EXE=./c64-hw-sim.exe
[ -x "$EXE" ] || EXE=./c64-hw-sim
EXE=$(pwd)/${EXE#./}
export OUT EXE
rm -rf "$OUT"; mkdir -p "$OUT"

for d in "$LA"/[Dd]iagnosis-* "$LA"/Old/[Dd]iagnosis-*; do
    [ -d "$d" ] || continue
    n=$(basename "$d"); mkdir -p "$OUT/real_$n"; cp "$d"/*.lac "$OUT/real_$n/" 2>/dev/null
    # The board the folder was taken from: its diagnosis.txt's first line
    # ("Diagnosing a ZX Spectrum 48K Issue 3B (RAM 16K), ..."); a folder
    # named 16k is a 16K board too (taken before the header said so).
    head=$(head -1 "$d/diagnosis.txt" 2>/dev/null)
    case $head in
        *"Issue 1"*) board=issue1 ;;
        *"Issue 3B"*) board=issue3b ;;
        *"Issue 4B"*) board=issue4b ;;
        *"Issue 6A"*) board=issue6a ;;
        *) board=issue2 ;;
    esac
    opt=
    case "$head $n" in *"RAM 16K"*|*16k*|*16K*) [ $board != issue1 ] && opt="--option ram=16k" ;; esac
    echo "--board $board $opt" > "$OUT/real_$n.args"
    echo "real $n"
done > "$OUT/jobs.txt"
# Simulated: the healthy board; the CPU's inputs held (/RESET, /WAIT,
# /BUSRQ, /INT); lines stuck; RAM, ROM, ULA and multiplexer faults.
for f in healthy IC2.26:gnd IC2.24:gnd IC2.25:gnd IC2.16:gnd IC3.4:gnd IC3.7:gnd IC1:nocontention IC1.19:gnd \
         IC8:data0=0 IC17:data0=1 IC10:flip@4005 IC5:data3=0 IC2.30:gnd IC2.12:gnd IC20:removed IC3:removed \
         IC2.5:gnd IC2.34:vcc IC12:data0=1 IC15:addr3 IC6:addr2 IC6:removed IC5:removed IC2.21:vcc IC2.22:gnd \
         IC25:removed IC2:removed IC1:removed IC13:data0=0 R26:removed; do
    echo "sim $f"
done >> "$OUT/jobs.txt"
# With the screen (--la-screen): the simulated fault's own screenshot.
for f in IC13:data0=1 IC5:removed; do
    echo "screen $f"
done >> "$OUT/jobs.txt"
# The check with no screen (--la-check): the faults of the verdict's parts
# put on the board; the fault itself should come out on top.
for f in IC8:data0=0 IC6:addr2; do
    echo "check $f"
done >> "$OUT/jobs.txt"

job() {
    kind=$1; f=$2
    if [ "$kind" = real ]; then
        n=real_$f
        args=$(cat "$OUT/$n.args" 2>/dev/null || echo "--board issue2")
        out=$("$EXE" --la-diagnose "$OUT/$n" --la-replay --la-all $args 2>&1)
    elif [ "$kind" = screen ]; then
        n=screen_$(echo "$f" | tr ':.=@' '____')
        mkdir -p "$OUT/$n"
        "$EXE" --board issue2 --headless 150 --shot "$OUT/$n/shot.png" --fault "$f" >/dev/null 2>&1
        out=$("$EXE" --la-diagnose "$OUT/$n" --la-simulate --la-all --board issue2 --fault "$f" \
              --la-screen "$OUT/$n/shot.png" 2>&1)
    elif [ "$kind" = check ]; then
        n=check_$(echo "$f" | tr ':.=@' '____')
        out=$(LA_JOBS=4 "$EXE" --la-diagnose "$OUT/$n" --la-simulate --la-all --board issue2 --fault "$f" --la-check 2>&1)
    else
        n=sim_$(echo "$f" | tr ':.=@' '____'); ff=$f; [ "$f" = healthy ] && ff=""
        out=$("$EXE" --la-diagnose "$OUT/$n" --la-simulate --la-all --board issue2 ${ff:+--fault $ff} 2>&1)
    fi
    {
        echo "######## $kind $f"
        echo "$out" | grep -E "^First, idle|^  (CLK|/RESET|/INT|The CPU):|^[0-9] of 6|^  (OK|FAULT|Wrong too|It never|Not in)|^Note|/RESET was low"
        echo "$out" | sed -n '/^The boot goes wrong\|^All 6\|^Left running\|^The boot.s/,$p' |
            grep -v "^$\|^The captures and\|^If the picture\|^the screen right\|^address and data\|^screenshot\|^fault inside\|^and the start of BASIC are good. A wrong\|^over its own lines to the lower RAM"
    } | sed "s#$OUT/#OUT/#g; s#([^()]*/\(reset\|ramcheck\|ramdone\|copyright\|main\|interrupt\)\.lac)#(\1.lac)#" > "$OUT/$n.txt"
}
export -f job
xargs -P "${LA_JOBS:-$(nproc 2>/dev/null || echo 4)}" -L 1 bash -c 'job "$0" "$1"' < "$OUT/jobs.txt"
cat "$OUT"/real_*.txt "$OUT"/sim_*.txt "$OUT"/screen_*.txt "$OUT"/check_*.txt > "$OUT/all.txt"
echo "Verdicts in $OUT/all.txt"
if [ ! -f tests/la-diagnose-expected.txt ]; then
    echo "No tests/la-diagnose-expected.txt yet: copy $OUT/all.txt there to compare against it next time"
    exit 0
fi
if command -v py >/dev/null; then py -I tests/la-compare.py tests/la-diagnose-expected.txt "$OUT/all.txt"
else python3 -I tests/la-compare.py tests/la-diagnose-expected.txt "$OUT/all.txt"; fi
