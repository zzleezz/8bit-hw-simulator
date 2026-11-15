# How the simulation works

`tools/kicad2c.py` turns bwack's KiCad replica of the 250407 into C tables:
216 parts, their pads, and 249 nets. It does the same for his 250469 (137
parts, 186 nets; KiCad 7, so it skips the copper zones that make the
file 44 MB) and for the Spectrum's Issues 3B, 4B, 6A, 2 and 1. For the Issues 2, 1 and 6A it renames nets to what
the 3B and 4B call them (so the model, and the probe readings you see, use the
same names on every board) and adds the parts the PCBs have no place for.

Every pin drives its net strongly, weakly (a pull-up), from a supply short,
or not at all. The strongest drive wins. Two equal drives fighting are a
contention, and low wins.

A net that nobody drives keeps its charge, and then drifts:

- to about 1.4V within microseconds, if TTL inputs load it;
- toward 0V in a fraction of a second, if only MOS inputs are on it.

MOS inputs compare that voltage, plus some noise, with their own threshold,
so a floating line can read erratically.

Power faults depend on the chip:

- A chip without VCC is dead.
- A chip without GND lets its outputs float high.
- A CMOS chip keeps running on phantom power, which reaches it through the
  protection diodes of its pins.

The noise is seeded, so a run can be repeated exactly.

`src/parts.c` models these parts at their pins:

- the glue logic;
- the ROMs;
- the DRAMs, which latch the row on /RAS and the column on /CAS;
- the color RAM;
- the PLA, with its real logic equations;
- the 250469's Super PLA, as the [250407 chips it replaced](commodore/64.md#the-250469-board).

The 6510, VIC-II, CIAs and SID are cycle-accurate cores from floooh's
chips, with a few additions (marked in their headers): the CIAs' time of
day clock, serial port and /PC handshake, the SID's paddle registers. In
`src/board.c` each core sits in a socket that sees only its pins.
Each half cycle runs in three steps: the row address, then /RAS falls,
then /CAS falls. The whole board settles after each step.

On the Spectrum everything is timed by the ULA's oscillator: the board
settles at every half period of it, eight times per CPU T-state. The ULA
steps, the board settles, and the Z80 gets an edge of its clock if there
was one. Then the memories see their strobes, and the board settles
again.

The Atari bench samples the PAL TIA oscillator twice per colour clock.
The TIA divides that clock for the 6507, whose phase-two output times the
TIA and RIOT bus accesses. [PAL timing and hardware references](atari/2600-pal.md)

The [design plan](../PLAN.md) explains the reasoning behind these choices.

## Limits

Please note that **even on fast computers** this program struggles to
simulate these machines at real speed. A real C64 is simulated at 31% of
its speed on an Apple M3 Max, 39% on an AMD Ryzen AI Max+ PRO 495 (Gorgon
Halo), and 48% on an Apple M5 Max. The ZX Spectrum is simulated at 23%,
28% and 34% of its speed on the same computers. This is not a problem for
the goals of this program, but be warned: it is not an emulator to play
games with.

Why is the Spectrum slower to simulate than the C64, even though its
board is simpler? Because of the time resolution. The ULA is simulated
at a very low level, so that the bus contention just emerges from the
simulation. Its signals are timed to the half period of its 14 MHz
oscillator, so the whole board settles 28 million times per simulated
second, against about 6 million for the C64.

The floating and power models have the right shape, but their numbers
(thresholds, drift times, crosstalk) are guesses. They are constants in
`src/sim.c`.

These analog parts are not simulated: the video output, the SID filter,
the power supply, and the dot clock PLL. On the Spectrum: the video
modulator, the sound, and the tape input. Three of them are modeled by what
they do, because the logic depends on them: the 50 Hz from the mains that
drives the time of day clocks, the cassette motor driver, and the SID's
paddle A/D converters (the time the 1800 pF capacitor takes to charge
through the paddle).

Faults inside a chip are digital: dead bits, address lines, cells and
outputs. Slow or marginal parts (timing) are not modeled: a chip's
outputs change in the same step as its inputs.
