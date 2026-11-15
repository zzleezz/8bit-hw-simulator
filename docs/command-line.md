# Command line

    ./c64-hw-sim --headless 200 --text --shot out.png --fault U17:removed

This runs four seconds (200 frames) without a window. It prints the text
read from the monitor picture, as a person would read it, and saves the
picture. A fault is written `REF:what` or `REF.PIN:what`:

    U17:removed   U21.14:gnd   U12.14:vcc   U9.2:open   CN6.B:open   RP4:short

A solder bridge between two neighbor legs of a chip is `REF.PIN-PIN:short`,
for example `U13.11-12:short` (only legs side by side: on a 16-pin chip, 8
and 9 sit at opposite sides). In the window, the menu of a chip's pin has a
"Solder bridge to pin N" choice for each neighbor; both legs then go on the
oscilloscope, with what each one drives itself drawn in amber.

An output dead inside the chip is `REF.PIN:out0` or `REF.PIN:out1` (stuck
low or high). Faults inside a ROM or RAM chip, with addresses in hex:

    U21:data0=0       data bit D0 reads 0 everywhere (a dead 4164)
    U4:data3=1        data bit D3 of the KERNAL ROM reads 1
    U4:addr12         address line A12 dead inside the chip
    U4:flip=047E^01   bit 0 of byte $047E of the chip reads inverted
    U21:flip@05F4     one bad cell: the one of CPU address $05F4
    U21:weak          weak DRAM cells: about 1 in 64 forget (read 0)

A chip's own address in a DRAM is (row << 8) | column; `flip@` takes a
CPU address instead, and finds where it is in that chip. `tests/mem-faults.sh`
runs these on the C64 and Spectrum, and under Dead Test and the Spectrum
diagnostics ROM.

A `.crt`, `.rom`, `.bin` or `.a26` file given on the command line is plugged in before
power on. A positional `.a26` file selects the Atari if no machine was
chosen. Other raw images may select a machine based on their contents.
`--harness` plugs the 586220 test harness, `--bank N` sets the switch of a ROM of
several banks, `--socket u4|port` puts an 8K C64 image in the KERNAL
socket or the expansion port (on the 250469 the KERNAL socket holds BASIC
too: see the [Commodore 64 guide](commodore/64.md)).

Other options:

- `--at 150:U6.17:open --at 300:repair` breaks and repairs parts while the
  C64 runs; `RESET:pressed` and `RESET:released` work there too.
  `--at 150:save:FILE.hwstate` saves the board to a file at frame 150;
  `--at 150:load:FILE` loads a program, snapshot or cartridge then.
- `--load-state FILE.hwstate` starts with the board of a save file, as it
  was saved (with or without a window).
- `--healthy-sheet DIR` writes the healthy board sheet of each C64 and Spectrum board
  to DIR (`c64-healthy.html`, `zx48-healthy.html`...).
- `--board 250469` (or `--machine c64c`) runs the C64's 250469 board
  instead of the 250407; fault specs use its references (`U8:removed` is
  the Super PLA, `U10.2:gnd` a RAM data pin).
- `--machine zx48` runs the ZX Spectrum; fault specs use its references
  (`IC10:removed`, `D12:open`, `TI-3:open`). `--board issue2` (or
  `--machine zx48i2`) runs the Issue 2 instead of the 3B, `--board
  issue1` (`zx48i1`) the Issue 1, `--board issue4b` (`zx48i4b`) the
  Issue 4B, `--board issue6a` (`zx48i6a`) the Issue 6A. `--option
  ram=16k` makes any board but the Issue 1 a 16K
  Spectrum, `--option ula=6c001` puts a 6C001 in an Issue 2
  or 1 (`ula=5c102` and `ula=5c112` are the others). `--option
  uchips=okil` fits other upper RAM chips (`ti3`, `ti4`, `okil`,
  `okih`, `4164`) with the links to match, `--option ulinks=ti4` sets
  the links (`ti3`, `ti4`, `okil`, `okih`; the Issue 2 `ti3` or `ti4`).
  Give it after `uchips` to set links that don't match. `--option
  rom=hitachi` fits the Hitachi ROM (`nec`, `hitachi`) with its link,
  `--option romlink=h` sets the ROM link (`n`, `h`): give it after
  `rom`.
- `--machine a2600pal` runs the [PAL Atari 2600A bench](atari/2600-pal.md).
  Plug in `carts/pal-bench.a26` to run the 312-line diagnostic raster.
- `--nets` reports the nets where outputs fought.
- `C64HW_TYPE=$'PRINT 1+1\n'` types text after boot.
- `ZXHW_ULALOG=file[:cycle]` logs the ULA's pins for `tools/ulacheck.c`;
  `ZXHW_CONTENTION=1` prints every contended T-state and its delay;
  `ZXHW_RAMT=1` adds to `--text` the last good RAM address the ROM's
  boot test found (P_RAMT: FFFF on a 48K, 7FFF on a 16K);
  `ZXHW_TRACE=FROM:COUNT` prints the buses at every half period of COUNT
  T-states from T-state FROM.
- `C64HW_CHECK=1` checks every KERNAL read and every VIC fetch against the
  ideal memory map.
- `C64HW_MATCH_VS=N` with `--match ... --match-debug DIR` scores every
  reading of the screenshot against match #N instead of #1: why a match
  you expected lost.
- `--record 900:DIR --frames 90` (graphical mode) saves the window 90
  times, every 4 emulated frames from frame 900 on. This is how the GIF
  in the [Commodore 64 guide](commodore/64.md#example-pin-faults) was made.
- `--inspect U8`, `--browse`, `--browse-atlas[=N]`, `--view X,Y,ZOOM`, `--mouse X,Y`,
  `--down X,Y@F`, `--up X,Y@F`, `--wheel X,Y,DY@F`, `--key NAME@F` and `--chars TEXT@F`
  (graphical mode) open the inspector, the file browser or the atlas browser, aim the board
  view, click the mouse, turn the wheel and press keys at UI frame F, for
  screenshots and tests of the interface. `--ui-stats` prints the
  interface's frame times every second.
- `--theme light`, `--theme dark` or `--theme system` (the default)
  (graphical mode) picks light or dark windows.

`--probe-test N` tests "Diagnose by probing": N faults of the atlas at
random on each machine (as likely as their bench weight), run on a board
and probed wherever the tool says, with each of the three ways of picking
the next pin, first without knowing the screen, then knowing it. `--probe-noise PCT`
takes that share of the readings wrong on purpose; `--seed S` picks other
faults.
