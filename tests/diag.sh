#!/bin/sh
# tests/diag.sh - run Commodore's diagnostic cartridges (carts/) on the
# simulated board and check that they report what their manuals say a
# real C64 reports. Not run by the other tests: it takes a few minutes
# (the cases run in parallel, about 4 minutes on a fast machine).
#
#   tests/diag.sh [outdir]
#   MACHINE=c64c tests/diag.sh [outdir]     the 250469 board
#   SERIAL=1 tests/diag.sh                  one case at a time (one core)
#
# Dead Test (rev 781220): on a healthy board the test screen comes up;
# every DRAM pulled out gives the flash code of the manual's "RAM Error
# Flash Code Chart" (1 flash U12, bit 7 ... 8 flashes U21, bit 0); without
# the PLA nothing comes up ("initial startup failure").
#
# Diagnostic 586220 with its test harness: a healthy board passes every
# test; a broken user port or keyboard contact, the paddle switch U28
# pulled out, or the mains cut (no 50 Hz for the TOD clocks) give the
# failures the manual describes. Without the harness the keyboard is OPEN.
#
# On the 250469 the same checks run on its parts: the paddle switch is
# U18, the mains come in through R16, the PLA is U8. Its RAM is two
# 41464s, U10 (D0-D3) and U11 (D4-D7). Dead Test flashes once for any
# error of more than one bit (two 4164s out on the 250407 do the same),
# so a 41464 pulled out only has to give flashes; a lifted data leg, one
# bit, gives the chart's count for that bit (D7 1 flash ... D0 8). Its
# control port test can't run: bwack's replica has 2200 pF on the paddle
# lines (C80, C81; the 250407 1800 pF), the harness's 110K then reads
# about 131 instead of 107, and the 586220 calls even a healthy board's
# CONTROL PORT BAD and blames the SID (whether a real 250469 has 2200 pF
# is not known here). And with the mains cut, by the end of the run the
# 586220 is in its second pass, the INTERRUPT line cleared: its blaming
# CIA2 (6526 U2) counts too.
#
# The simulator knows nothing about these cartridges: what they report
# comes from the netlist and the chip models.
MACHINE=${MACHINE:-c64}
OUT=${1:-/tmp/c64hw-diag-$MACHINE}
BIN=./c64-hw-sim
DT=carts/deadtest-781220.crt
DIAG=carts/diag-586220.crt
mkdir -p "$OUT"

dead() {    # name faults...
    name=$1; shift
    args=""
    for f in "$@"; do args="$args --fault $f"; done
    $BIN --machine $MACHINE --headless 1500 --flashes --shot "$OUT/$name.png" $args $DT > "$OUT/$name.txt" 2>&1 &
    [ -n "$SERIAL" ] && wait
}

diag() {    # name harness faults...
    name=$1; harness=$2; shift 2
    args=""
    [ "$harness" = 1 ] && args="--harness"
    for f in "$@"; do args="$args --fault $f"; done
    $BIN --machine $MACHINE --headless 3000 --text --shot "$OUT/$name.png" $args $DIAG > "$OUT/$name.txt" 2>&1 &
    [ -n "$SERIAL" ] && wait
}

if [ "$MACHINE" = c64c ]; then
    RAMS="U11 U10"; PLA=U8; PADDLES=U18; MAINS=R16
    dead dt_d7 U11.17:open
    dead dt_d0 U10.2:open
else
    RAMS="U12 U24 U11 U23 U10 U22 U9 U21"; PLA=U17; PADDLES=U28; MAINS=R100
fi
dead dt_healthy
for u in $RAMS; do dead dt_$u $u:removed; done
dead dt_pla $PLA:removed
diag d_healthy 1
diag d_userport 1 CN2.C:open
[ "$MACHINE" = c64c ] || diag d_paddles 1 $PADDLES:removed
diag d_keyboard 1 CN1.13:open
diag d_mains 1 $MAINS:open
diag d_noharness 0
wait

fail=0
check() {   # what, then a command that succeeds if it is right
    what=$1; shift
    if "$@"; then echo "ok    $what"; else echo "FAIL  $what"; fail=1; fi
}
has() { grep -q "$2" "$OUT/$1.txt"; }
hasnt() { ! grep -q "$2" "$OUT/$1.txt"; }
# Every complete series (the last one may be cut by the end of the run)
# has n flashes, and there are at least two of them.
flashes() {
    awk -v n="$2" '/^flash series:/ {
        if (NF < 5) exit 1
        for (i = 3; i < NF; i++) if ($i != n) exit 1
        ok = 1
    } END { exit !ok }' "$OUT/$1.txt"
}

check "Dead Test, healthy: no flashes, the test screen is up" has dt_healthy "flash series: none"
check "Dead Test, healthy: the screen is lit" has dt_healthy "screen at the end: lit"
if [ "$MACHINE" = c64c ]; then
    # Every complete series has the same count, one of LO..HI.
    check "Dead Test, U11 (D4-D7) pulled out: flashes" hasnt dt_U11 "flash series: none"
    check "Dead Test, U10 (D0-D3) pulled out: flashes" hasnt dt_U10 "flash series: none"
    check "Dead Test, U11.17 (D7) lifted: 1 flash" flashes dt_d7 1
    check "Dead Test, U10.2 (D0) lifted: 8 flashes" flashes dt_d0 8
else
    n=1
    for u in $RAMS; do
        check "Dead Test, $u pulled out: $n flashes" flashes dt_$u $n
        n=$((n + 1))
    done
fi
check "Dead Test, PLA pulled out: no flashes" has dt_pla "flash series: none"
check "Dead Test, PLA pulled out: the screen stays dark" has dt_pla "screen at the end: dark"
if [ "$MACHINE" = c64c ]; then
    # Everything but the control port and the SID it blames (see above).
    check "586220 + harness, healthy: no BAD but CONTROL PORT and the SID" \
        sh -c "sed 's/CONTROL PORT *BAD//; s/6581 U18 *BAD//' '$OUT/d_healthy.txt' | grep -q BAD && exit 1 || exit 0"
else
    check "586220 + harness, healthy: no BAD" hasnt d_healthy "BAD"
fi
check "586220 + harness, healthy: no OPEN" hasnt d_healthy "OPEN"
check "586220 + harness, healthy: USER PORT OK" has d_healthy "USER PORT *OK"
check "586220, user port PB0 contact open: USER PORT BAD" has d_userport "USER PORT *BAD"
# (The cartridge names the SID "6581 U18", the 250407's, on either board.)
if [ "$MACHINE" != c64c ]; then
    check "586220, $PADDLES pulled out: CONTROL PORT BAD" has d_paddles "CONTROL PORT *BAD"
    check "586220, $PADDLES pulled out: the SID is blamed" has d_paddles "6581 U18 *BAD"
fi
check "586220, keyboard COL0 contact open: KEYBOARD BAD" has d_keyboard "KEYBOARD *BAD"
if [ "$MACHINE" = c64c ]; then
    check "586220, mains cut: INTERRUPT BAD or CIA2 blamed (no TOD alarm)" has d_mains "INTERRUPT *BAD\|6526 U2 *BAD"
else
    check "586220, mains cut: INTERRUPT BAD (no TOD alarm)" has d_mains "INTERRUPT *BAD"
fi
check "586220, mains cut: the clocks don't move" has d_mains "00:00:00AM 00:00:00PM"
check "586220 without harness: KEYBOARD OPEN" has d_noharness "KEYBOARD *OPEN"

[ $fail = 0 ] && echo "all the diagnostic checks passed (pictures and text in $OUT)"
exit $fail
