#!/bin/sh
# tests/zx-faults.sh - common ZX Spectrum 48K faults (board Issue 3B), and
# what the monitor shows. Like tests/faults.sh for the C64: the EXPECTED
# column was written before running the simulator, from the service manual
# and the repair guides; the simulator has no code that knows about any of
# these symptoms. The i6a_ cases are the Issue 6A's own glue (the ZX8401
# and IC28): their EXPECTED lines come from the circuit, the 4B's cases
# that do the same job.
#
#   tests/zx-faults.sh [outdir]
OUT=${1:-/tmp/zxhw-faults}
BIN=./c64-hw-sim
FRAMES=200
mkdir -p "$OUT"

run() {     # name "expected" faults...
    name=$1; expected=$2; shift 2
    args=""
    for f in "$@"; do args="$args --fault $f"; done
    {
        echo "EXPECTED: $expected"
        $BIN --machine ${MACHINE:-zx48} --headless $FRAMES --text --shot "$OUT/$name.png" $args
    } > "$OUT/$name.txt" 2>&1 &
}

run healthy "the copyright message, (c) 1982 Sinclair Research Ltd"
run lram_ic10 "no copyright: the RAM test fails at 16384, its fill pattern stays on screen" IC10:removed
run lram_ic6 "no copyright: the RAM test fails at 16384" IC6:removed
run uram_ic15 "the copyright message (it boots as a 16K Spectrum)" IC15:removed
run no_m5v "no -5V: the 4116s don't work, no copyright, a broken picture" D12:open
run no_dcdc "no +12V and no -5V: same as a dead lower RAM" TR4:open
run no_rom "black border, the power-on garbage of the screen RAM" IC5:removed
run no_cpu "black border, the power-on garbage of the screen RAM" IC2:removed
run no_ula "no picture: snow" IC1:removed
run no_crystal "no clock: no picture, the CPU stopped" X1:open
run no_clock_tr3 "no CPU clock: black border, garbage" TR3:open
run mux_ic3 "scrambled characters and colours, or no boot" IC3:removed
run uram_link "wrong RAM type link: the upper RAM fails, a 16K Spectrum" TI-3:open
wait
MACHINE=zx48i6a
run i6a_healthy "the copyright message, (c) 1982 Sinclair Research Ltd"
run i6a_no_zx8401 "no address multiplexing for the lower RAM: no copyright" IC27:removed
run i6a_no_ic28 "the lower RAM's /RAS never reaches the multiplexers: no copyright" IC28:removed
run i6a_rascas_link "RAS/CAS link off: the multiplexers stay on the row address, no copyright" LK5:open
run i6a_drasll "the ZX8401's lower select open: no copyright" IC27.1:open
run i6a_ucas "the upper RAM's /CAS open: the copyright message, a 16K Spectrum" IC27.34:open
run i6a_r88 "no upper /RAS for the select: the copyright message, a 16K Spectrum" R88:open
run i6a_lram_ic10 "no copyright: the RAM test fails at 16384" IC10:removed
run i6a_uram_ic15 "the copyright message (it boots as a 16K Spectrum)" IC15:removed
run i6a_no_m5v "no -5V: the 4116s don't work, no copyright, a broken picture" D18:open
wait
for f in "$OUT"/*.txt; do
    echo "== $(basename "$f" .txt)"
    head -1 "$f"
    grep "^|" "$f" | grep -v "^|$" | tail -3
done
