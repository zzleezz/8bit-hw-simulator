# ZX Spectrum 48K

[Sinclair systems](README.md) · [Logic analyser and live diagnosis](logic-analyser.md) · [Command-line options](../command-line.md)

`--machine zx48` starts with the Spectrum, and so does a snapshot or a
ROM given on the command line.

There are five boards: the Issue 1, the Issue 2, the Issue 3B (the
default), the Issue 4B and the Issue 6A. Pick one with the drop-down under
the machine buttons, or `--board issue2` (`issue1`, `issue4b`, `issue6a`)
on the command line.
Next to it are the choices of how the board is built:

- RAM 48K or 16K (Issues 2, 3B, 4B and 6A). A 16K Spectrum is the same board with the upper RAM and
  its glue (IC15 to IC26) left out; on the 4B IC24 stays, as it times
  the lower RAM too, and on the 6A the ZX8401 and IC28 stay (they are
  the lower RAM's glue as well): only IC15 to IC22 go. They show as empty sockets, and are
  not faults: Repair all leaves them out. The ROM finds 16K, and the
  copyright message is the same.
- ULA 5C112 or 6C001 (Issue 2), 5C102, 5C112 or 6C001 (Issue 1). The
  early boards left the factory with a 5C ULA; many have had a 6C001 put
  in since. They differ in analog ways the simulator doesn't have, so
  this changes the ULA's label and the resistors that go with it (R47,
  R49), not the picture. One digital difference is modelled: the 5C ULAs
  drive their clock output high themselves, while the 6C001's is open
  collector and needs R73 to pull it up. The Issue 1 has no R73, so with
  a 6C001 it gets one, as the swap needs (drawn between R25 and R1, next
  to TR3).
- Chips and Links: the upper RAM (Issues 2, 3B, 4B and 6A). Sinclair fitted
  64K chips with one bad half, and links on the board to use the good
  one. A TI 4532 is split by the row address A7: link TI, and TI-3 (a
  4532-3, the rows with A7 low) or TI-4 (a 4532-4, A7 high). An OKI 3732
  is split by the column A7: link OKI, and OKI-L or OKI-H. The TI links
  put A14 in the column and the half's A7 in the row; the OKI links the
  other way round. A whole 4164 works with either. The Issue 2 takes TI
  chips only: LNK1 puts the row A7 low (TI-3) or high (TI-4). Picking
  chips sets the links to match; pick other links after to see what a
  wrong setting does: half the upper RAM is bad, the ROM's RAM test stops
  at it, and the Spectrum comes up with 32K or 16K (it still shows the
  copyright message; `PRINT PEEK 23732+256*PEEK 23733` gives 49151 or
  32767 instead of 65535). The links not fitted are drawn grey. The
  chips' labels say which they are.
- ROM and ROM link (every Spectrum board): the ROM (IC5) is a NEC
  uPD23128 or a Hitachi HN613128, and its link (JROM1; LNK2 on the
  Issues 1 and 2) is set N or H. The two chips swap pins 20 and 27 (NEC
  and Hitachi datasheets): the NEC's pin 20 is a clocked /CE that latches
  the address when it falls, its pin 27 a chip select; the Hitachi is
  static, pin 20 a chip select, 27 an output enable. N puts /MREQ on pin
  20 and /ROMCS on 27, H the other way round. A NEC on H gets /ROMCS on
  /CE, which stays low while the CPU runs in the ROM: no new edge, the
  same byte again and again, and the Spectrum is dead (black border, the
  power-on garbage of the screen RAM). A Hitachi works on either, as its
  datasheet says it should. Picking the ROM sets the link to match. The
  default is the NEC on N. A fault that keeps pin 20 from going high
  between reads (IC5.20 held low or open, /MREQ dead) now stops the ROM
  too, as on a real NEC.

The Issue 2 is the same circuit as the 3B, chip for chip: the two netlists
were compared pin by pin. Its layout differs, and a few parts have other
names: the edge connector is J3, the 9 V jack J4, the 7805 U2, the ROM
link LNK2 (its legs in another order on the Issue 1: the link is set by
the nets it joins). The upper RAM type is wired in, except for LNK1 (TI chips
only, see Chips and Links above). TR6 (A0
keeping the ULA's /IORQ high) had no place on the Issue 2 PCB: it was
soldered across other parts. Here it is drawn in the gap between the glue
chips, and C74 (a power supply fix fitted the same way) next to TR5.

The Issue 1 is the Issue 2's circuit without the upper RAM: a 16K
Spectrum. The upper 32K came on a separate board, plugged into the
sockets EXT1 and EXT2; that board isn't simulated yet, so EXT1 and EXT2
stay empty. Otherwise it differs in small ways: the keyboard pull-ups
are the resistor pack RP1, TR3's base has D14 beside R24 (the later
boards have C67 there and R73), there is no TR6 (the ULA's /IORQ doesn't
look at A0), and some analog values differ. The "dead cockroach" and
"spider" fixes are not drawn, as on the KiCad project.

The Issue 4B has the 3B's chips and logic, with three differences:

- The lower RAM multiplexers switch to the column address after two
  spare NAND gates of IC24 have delayed /RAS. The 3B uses the RC delay of
  R32 for this, and on the 4B R32 is a 0 ohm link. The simulator's gates
  take no time, so the 4B keeps the 3B's one-step delay (35 ns, where the
  two gates take about 20) on R32: it stands for the gates.
- The DC-DC converter makes -5V through C46, D11, D18, C79, R79 and the
  zener D19 (the 3B: D12, R55, D16), and +12V through a charge pump (C80,
  D17, D15). Break D18, for example, and the lower RAM stops, as with the
  3B's D12.
- The 9 V comes in through the ferrite links L1 and L2. They are drawn
  upright: on the board they sit at a slant, crossing.

The Issue 6A has the 4B's circuit with the glue in one chip, laid out
anew:

- The ZX8401 (IC27, Philips PCF1306P) does the work of the 4B's IC3,
  IC4 and IC23 to IC26: the lower RAM's address multiplexers (DMA0-DMA6,
  switched by DRASLL), the upper RAM's (MA0-MA7, MA7 from the type
  links), and the upper RAM's select and /CAS. It is simulated as what
  it replaced (four 74LS157 and a few gates), as drawn by the 74LS
  drop-in replacement and uses the 6A KiCad project's pin names. The
  gates' order matters: /CAS falls a step after the
  select (the column is on the RAM's pins first: on the board, the
  gates and C105). [Acknowledgements](../../README.md#acknowledgements)
- A 74LS04 (IC28) makes the upper RAM's /RAS (from /MREQ) and /WRITE
  (from /WR), two inverters in a row each, and two more delay the lower
  RAM's /RAS for the multiplexers. That path goes through the RAS/CAS
  link, a 2x3 header (J1 in the KiCad project, LK5 here, next to LK1-LK4),
  fitted 1-2 3-5 4-6 for a Ferranti ULA, and R87, a 0 ohm link standing
  for the gates' delay as the 4B's R32 does. R88 delays the upper RAM's
  /RAS for the ZX8401's select (MREQDL), as the 4B's R71.
- LK1-LK4 and the LKU and LKV links (J2 and J3 in the KiCad project) are
  for a Saga ULA and the video lines on the edge connector: open, as the
  KiCad project's README says for a Ferranti ULA. R90, C100, C102 and
  C103 are not fitted either, but are drawn.
- There is no R57: the Z80's /RFSH goes to the edge connector only. The
  KiCad schematic names the edge pin RFSH and the CPU pin RFSHL, two
  nets; they are one on the board, joined here.
- The power supply, the 9 V input and the connectors are the 4B's.

The chips of the Issue 3B (the Issues 2, 4B and 6A have the same, but
the 6A's glue, the Issue 1 all but the upper RAM):

- ULA is the Ferranti 6C001. It makes the picture, makes the CPU clock,
  drives the RAS and CAS of the lower RAM, and is the port 0xFE (keyboard,
  border, MIC, EAR).
- CPU is the Z80A, and ROM is the 23128.
- LRAM0 to LRAM7 are the 4116 DRAMs of the lower 16K, where the screen is.
  LRAMn has data bit n.
- URAM0 to URAM7 are the 4532s of the upper 32K: 64K chips with one bad
  half. The links next to them tell the board which half is good; here TI
  chips are fitted.
- MUX are the 74LS157 address multiplexers. OR and NAND, a 74LS32 and a
  74LS00, select the upper RAM and time its RAS and CAS, with two RC
  delays.
- PAL is the LM1889 video modulator. It is analog and the one chip that
  is not simulated: the monitor shows the ULA's picture.

The CPU and the ULA share the lower 16K, and this is what makes the
Spectrum interesting at the pin level. They sit on the two sides of a row
of resistors: 470 ohm on the data bus, 330 ohm on the address lines after
the multiplexers. When both drive the bus, each side sees its own value.
When the CPU wants the lower 16K while the ULA fetches the screen, the ULA
stops the CPU's clock: the famous contention. Here that happens because
the ULA holds its clock pin, not because a table of delays says so. On the
simulated board, the delays follow the 6,5,4,3,2,1,0,0 pattern known from
the real machine. The first contended T-state comes 14337 T-states after
/INT falls. Emulators put it at 14335 or 14336, counting from their own
frame start.

Keys: ESC is BREAK, TAB is SYMBOL SHIFT, left SHIFT is CAPS SHIFT. RESET
shorts /RESET to GND on the edge connector. Open loads 48K snapshots
(.sna, .z80): they are written straight into the DRAM chips, like the C64
programs. A 16K ROM (.rom, .bin) is plugged into the edge connector like a
ROM cartridge: it holds /ROMCS high and answers in place of the internal
ROM. A 32K or 64K image is several 16K banks behind a switch: the "ROM
bank" buttons under RESET flip it. A raw image dropped on the wrong
machine puts the right one on the bench (the contents tell a C64 image
from a Spectrum one).
The ZX Spectrum Diagnostics ROM is in `carts/` (LGPL). On a healthy board
it passes its tests; on a broken one it says what it found.

The power supply is modeled by what it does. The 7805 makes +5V. The
DC-DC converter (TR4, TR5 and the coil T1) makes +12V, and its charge
pump makes -5V. The 4116s need all three: break TR4, or D12 of the -5V
pump, and the lower RAM stops working. Without the crystal X1 there is no
clock at all.

`tests/zx-faults.sh` runs 13 common faults. As on the C64, the expected
symptom of each one was written down before running the simulator. A
missing lower RAM chip leaves the ROM's RAM test pattern on the screen,
and a missing upper RAM chip gives a 16K Spectrum. `tests/zx-diag.sh`
runs the Diagnostics ROM on a healthy board and with bad RAM chips: it
finds the failing data bit (so the chip) where its manual says it does.

## The ULA

The ULA is not an emulator core: it is a model at its pins, at the
resolution of its 14 MHz oscillator. Every half period it decides /RAS,
/CAS, the address pins, the CPU clock, /INT, what it puts on the data bus
for a port read, and the colour of the pixel. Its logic and its timing
come from the die: the emu-russia/ula project recovered the 6C001's
netlist, 661 NOR gates.

`tools/ulacheck.c` keeps the model honest. The simulator logs what the
ULA saw on its input pins at every half period (`ZXHW_ULALOG=file`), and
the checker runs the 661 gates on the same inputs. Then it compares every
output, at every half period, and the colour of every pixel. It has been
run on a Spectrum booting, on the ROM's editor, and on a BASIC program
that changes the border colour as fast as it can. There are three
documented simplifications, listed at the top of `src/ula.c`, none of
which matters to the chips on the board. Apart from them, every output
and every pixel are the same. The gates also taught a detail: a new
border colour shows only from the next 8-pixel boundary, because it goes
through the same latch as the attributes. The netlist is not in this
repository; `tools/ulacheck.c` says where to get it.

## The Z80

floooh's Z80 runs one T-state per call and puts its pins out when it suits
an emulator. The ULA needs more: it looks at the address and /MREQ at the
right half of a T-state to decide whether to stop the clock, and it can
stop the clock in either phase. So `chips/z80.h` is now stepped on every
edge of its clock, and its pins follow the Zilog timing diagrams. Its
instructions, flags and timings are those of the original core.
`tests/z80` has the tests: the pin waveforms of every kind of bus cycle,
and ZEXDOC and ZEXALL. `chips/codegen` has the code generator.
