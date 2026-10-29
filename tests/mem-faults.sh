#!/bin/sh
# tests/mem-faults.sh - faults inside the chips: dead RAM chips and bits,
# bad cells, dead address lines inside a chip, weak DRAM cells, corrupted
# ROM bytes, and outputs dead inside a chip on a bus. On both machines, at
# power on, and under the diagnostic cartridges made to find them.
#
#   tests/mem-faults.sh [outdir]
#
# As in tests/faults.sh, the EXPECTED lines were written BEFORE running
# the simulator: what repair experience (or the cartridge's manual) says.
# The simulator has no code that knows about any of these symptoms.
# It takes a few minutes (the Dead Test and diagnostics ROM runs are long).
OUT=${1:-/tmp/hw-mem-faults}
BIN=./c64-hw-sim
DT=carts/deadtest-781220.crt
ZXDIAG=carts/zx-diagnostics-v0.37.rom
KEYS=111111111111111111111111111111111111111111111111111111111111
mkdir -p "$OUT"

# JOBS=N runs at most N at a time (default: all at once).
throttle() {
    [ -n "$JOBS" ] || return 0
    while [ "$(jobs -p | wc -l)" -ge "$JOBS" ]; do sleep 1; done
}

c64() {     # name "expected" faults...
    name=$1; expected=$2; shift 2
    throttle
    args=""
    for f in "$@"; do args="$args --fault $f"; done
    {
        echo "== c64 $name"
        echo "faults:   $*"
        echo "expected: $expected"
        $BIN --headless 200 --text --shot "$OUT/c64_$name.png" $args
    } > "$OUT/c64_$name.txt" 2>&1 &
}

dead() {    # name "expected" faults...
    name=$1; expected=$2; shift 2
    throttle
    args=""
    for f in "$@"; do args="$args --fault $f"; done
    {
        echo "== deadtest $name"
        echo "faults:   $*"
        echo "expected: $expected"
        $BIN --headless 1500 --flashes --shot "$OUT/dt_$name.png" $args $DT
    } > "$OUT/dt_$name.txt" 2>&1 &
}

zx() {      # name "expected" faults...
    name=$1; expected=$2; shift 2
    throttle
    args=""
    for f in "$@"; do args="$args --fault $f"; done
    {
        echo "== zx48 $name"
        echo "faults:   $*"
        echo "expected: $expected"
        $BIN --machine zx48 --headless 250 --shot "$OUT/zx_$name.png" $args
    } > "$OUT/zx_$name.txt" 2>&1 &
}

zxdiag() {  # name frames "expected" faults...
    name=$1; frames=$2; expected=$3; shift 3
    throttle
    args=""
    for f in "$@"; do args="$args --fault $f"; done
    {
        echo "== zx diagnostics ROM $name"
        echo "faults:   $*"
        echo "expected: $expected"
        C64HW_TYPE=$KEYS $BIN --machine zx48 $ZXDIAG --headless $frames --shot "$OUT/zxdiag_$name.png" $args
    } > "$OUT/zxdiag_$name.txt" 2>&1 &
}

# C64, power on.
c64 dram_u21_reads0   "black screen (bit 0 of every byte is 0)"            U21:data0=0
c64 dram_u12_reads1   "black screen (bit 7 of every byte is 1)"            U12:data0=1
c64 dram_u21_a7_dead  "black screen or garbage (half the RAM aliased)"     U21:addr7
c64 dram_u21_weak     "black screen or a crash"                            U21:weak
c64 dram_screen_cell  "READY screen, one wrong character mid-screen (space becomes !)" U21:flip@05F4
c64 dram_stack_cell   "boots normally, or crashes"                         U21:flip@01F8
c64 colorram_d0_1     "text light grey instead of light blue"              U6:data0=1
c64 colorram_cell     "the C of COMMODORE in another color"                U6:flip@D831^04
c64 colorram_a9_dead  "looks normal (the colors are all the same)"         U6:addr9
c64 kernal_byte       "banner reads BOMMODORE"                             U4:flip=047E^01
c64 kernal_a12_dead   "black screen"                                       U4:addr12
c64 basic_d0_0        "no banner, or a crash"                              U3:data0=0
c64 char_byte         "every R has a wrong pixel"                          U5:flip=0093^10
c64 char_d7_1         "a vertical line at the left of every character"     U5:data7=1
c64 sid_d0_out0       "black screen (the SID fights the data bus)"         U18.15:out0

# C64, Dead Test (the RAM Error Flash Code Chart: 1 flash U12 ... 8 U21).
dead dram_u21_reads0  "8 flashes (U21, bit 0)"                             U21:data0=0
dead dram_u12_a3_dead "1 flash (U12, bit 7)"                               U12:addr3
dead dram_u21_cell    "test screen: RAM test reports U21 BAD"              U21:flip@05F4
dead colorram_d0_1    "test screen: COLOR RAM BAD"                         U6:data0=1

# ZX Spectrum 48K, power on.
zx lram_ic6_reads0    "no copyright message, striped garbage screen"       IC6:data0=0
zx lram_ic6_cell      "copyright message with one wrong pixel"             IC6:flip@53E2
zx lram_ic9_a6_dead   "garbage screen, no copyright message"               IC9:addr6
zx uram_ic15_reads1   "copyright message (boots as a 16K Spectrum)"        IC15:data0=1
zx uram_stack_cell    "copyright message, or a reset loop"                 IC15:flip@FF4E
zx rom_byte           "copyright message reads sinclair"                   IC5:flip=1540^20
zx rom_d0_0           "black or garbage screen, no copyright message"      IC5:data0=0

# ZX Spectrum, diagnostics ROM.
zxdiag lram_ic8_reads0 1500 "lower RAM: RAM FAIL 2 (IC8 is D2)"            IC8:data0=0
zxdiag lram_ic8_cell   1500 "lower RAM: RAM FAIL 2"                        IC8:flip@53E2
zxdiag uram_ic17_weak  3000 "upper RAM tests FAIL in bit 2"                IC17:weak
wait

for f in "$OUT"/*.txt; do
    sed -n '1,3p' "$f"
    grep -v '^== \|^faults:\|^expected:' "$f" | head -12
    echo
done
