# c64-hw-sim

c64-hw-sim simulates the motherboards of the Commodore 64 (ASSY 250407
and 250469) and of the ZX Spectrum (boards Issue 1, 2, 3B, 4B and 6A), pin by pin. You can lift the
leg of a chip, short it to ground or to +5V, pull a chip out of its
socket, or open a resistor. Then you watch what the machine does: the
picture on the monitor, the keyboard, every signal on the board.

> **An AI-generated project.** What this fork adds to antirez's original
> (the code, the board models, the tests and this documentation) was
> written by Claude, Anthropic's AI model, working in Claude Code, at the
> direction of zzleezz, who decided what to build and tested it. Where
> the model had to assume something about the hardware, the assumption
> is written down next to it (for example in "The 250469 board"). Check
> a diagnosis against the real machine before acting on it.

> **Credits.** Based on [c64-hw-simulator](https://github.com/antirez/c64-hw-simulator)
> by Salvatore Sanfilippo ([antirez](https://github.com/antirez)), the
> original project. This is a modified version of it, not the original:
> the ZX Spectrum, the C64's 250469 board, the fault atlas and the
> diagnosis tools were added here. The ZX Spectrum board layouts (Issues
> 1, 2, 3B, 4B and 6A) come from the KiCad projects of
> [liveboxandy](https://github.com/liveboxandy), the C64 ones from
> [bwack](https://github.com/bwack)'s.
> The rest of the people whose work it builds on are in
> [Acknowledgements](#acknowledgements).

The program knows the board's netlist (the electrical connections on the
PCB) and how each chip behaves at its pins. It knows nothing about what a
broken Commodore 64 or Spectrum looks like: when you simulate a broken
part and the screen fills with garbage, that garbage was computed from
the circuit, not chosen by someone who knew what to expect.

Here is an example. Lift pin 17 (A7) of the C64 color RAM before power
on, and the machine boots and looks healthy. The CPU writes and the VIC
reads with the same wrong address, so the RAM simply aliases. Put the leg
back, and the half of the color RAM that was never written shows up as
random colors. Lift it again, and the screen is clean. A real 2114 does
the same.

With this program you can do mainly two things:

1. Learn how the Commodore 64 and the Spectrum work at a more intimate
   level, in a way that is hard to reproduce on real hardware without the
   risk of damaging it (and on most boards many chips are soldered, not
   socketed).
2. If you have a broken C64 or Spectrum and some hypothesis about what
   could be broken, simulate it with this program and perhaps gain clues,
   so that you can desolder chips in the most logical order, and desolder
   as few of them as possible (desoldering is a boring process, and every
   time it risks damaging the board...). A matching symptom is a clue,
   not a proof: different faults can look the same, and the simulation
   has its limits (see below).

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

![The character ROM pulled out while a BASIC program runs, then put back](docs/char-rom.gif)

Above: the classic C64 `10 PRINT CHR$(205.5+RND(1));:GOTO 10` maze is
running when the character ROM (U5) is pulled out of its socket, and
later put back. The CPU, the RAM and the program keep working; only the
shapes of the characters are lost, as repairers know well. The scope is
on the data line D0 of the ROM socket: with the socket empty, the bus
floats (the dotted segments) whenever the VIC fetches a character shape.
Shown at C64 speed.

## Acknowledgements

I (antirez) got the idea for this program while using and observing Andre
Weissflog's (floooh) chips, emulators of the 6502, VIC-II, CIA, SID and
friends that use pins as their interface. The next step was to take this
approach, with his code as a starting point, to its logical extreme:
pin-level models of every other part of the C64, the RAMs, the ROMs, the
logic chips, the PLA, and so forth.

The chip cores are floooh's chips, taken from c64-llm (zlib license). The
boards come from bwack's C64-250407-Replica-KiCad and
C64C-250469-KiCAD-Replica (both CERN OHL). The PLA equations come from
the 82S100 read-out by Mark Smith and Marko Mäkelä.

For the ZX Spectrum: the Z80 is floooh's too, altered to run on the edges
of its clock. The five boards come from the KiCad projects of
[liveboxandy](https://github.com/liveboxandy): the Issue 3B, redrawn
from PABB's gerbers (CC0); the Issue 2 (ZX-Spectrum-48K-Issue-2, CC0),
drawn from photos of a bare board; the Issue 1 (ZX-Spectrum-48K-Issue-1,
CC0), drawn from a bare board too; the Issue 4B
(ZX-Spectrum-48K-Issue-4B, CC0); and the Issue 6A (ZX-Spectrum-48K-Issue-6A,
CC0). The ZX8401's insides (its 74LS equivalent, pin by pin) come from
the drop-in replacement on trastero.speccy.org (cosas/JL/pcf1306p). The ULA model is checked against
the gate netlist of the 6C001 recovered from the die by the emu-russia/ula
project (CC0). The ROM is Amstrad's, who allow its distribution. The ZX
Spectrum Diagnostics ROM is by Brendan Alford and its contributors (LGPL).

The light theme is by Henry, published here with permission.

## Building

    make
    ./c64-hw-sim [program.prg]

It runs on macOS, Linux (X11) and Windows, with OpenGL. On Windows, build
with MinGW-w64 gcc from a POSIX shell (Git Bash or MSYS2), for example
`mingw32-make`, then run `c64-hw-sim.exe`. The graphics library (sokol)
and the interface library (nuklear) are in `third_party/`. Two of their
headers carry local fixes to mouse input and window handling; they are
listed in `third_party/PATCHES.md`.

The icon is a piece of the C64 board: `src/c64-hw-sim.ico` for the
Windows .exe (build.bat compiles `src/icon.rc` into it), and the same
pixels in `src/icon_data.c` for the window and the taskbar on every
system. `tools/make_icon.ps1 shot.png [X Y SIZE]` makes both again from
a `--screenshot`.

The mouse wheel scrolls whatever window is under the mouse (over the
board, it zooms). Esc closes the window on top. If the interface ever
seems stuck, press F12: it writes the state of the windows to
`ui-debug.log`, next to where the program was started. A frame that takes
over 1.5 s is logged there too, with what the interface was doing.

The windows are light or dark as the system is set (macOS, Windows 10
and later, GNOME), and follow it when it changes. The Theme row at the
bottom of the controls picks light or dark for good. The board, the
monitor and the scope screen look the same either way.

## Using it

The window shows the board, the monitor, a control panel and an
oscilloscope. The two buttons at the top of the control panel switch
between the Commodore 64 and the ZX Spectrum 48K: the machine that runs
is switched off, the other one on.

Pins show their state as a logic probe would: red high, blue low, yellow
pulsing, gray floating, magenta when two outputs fight. Zoom in and the
chips show references, address ranges and pin numbers; zoom in more and
the logic chips show their gates, the others their pin names. Hovering a
pin tells what it is: connector pins have their function ("CN4.3 ATN
(SERIAL)").

Click a pin to put it on the oscilloscope and to break it: floating,
shorted to GND, shorted to +5V, or, on an output, dead inside the chip
(stuck low or high: unlike a short, another chip on a bus fights it).
Click a chip to pull it out, or to inspect it. A ROM or RAM chip can also
break inside ("Inside the chip"): a dead data bit (on a 4164 or 4116,
the whole chip), a dead address line, one bad cell or corrupted byte,
or, on a DRAM, weak cells that forget. Click a resistor, a ferrite bead
or the crystal to open it. The control panel lists the faults, each with a
repair button; its heading counts them, in red. Click a fault in the list
to zoom in on it. On the board, a part with a fault has a red outline and a
"!" badge, seen even with the whole board in view; point at it to read
what is wrong. "Hide fault markers (practice)" hides the badges, the marked
pins and the list (a pulled chip still shows its empty socket, as it would
on a real board): put a fault on, hide it, and find it with the probe and
the scope.

Shift-click (or middle click) a pin for a continuity test, like the
beeper of a multimeter on a trace. In yellow, what beeps: every pad on
that pin's net, and on the nets joined to it by ferrite beads, links,
jumpers and fuses. In cyan, dashed, what is on the other side of a
resistor or a diode on those (one step, not further). A pull-up or a
pull-down gets a `+5V` or `GND` badge instead: a supply isn't followed
to every chip of the board. A part or a leg broken open stops the test
there, in orange. A solder bridge beeps like copper (`Bridge U13.11-12
[VA4]: U19.28, ...`), and a pin shorted to GND or +5V beeps to that
supply (`U13.11 shorted to GND`), both ways: probing GND reaches the
shorted net too. The bottom of the view lists it all:
`Beep [/RESET]: Direct: U7.40, FB4.2, ... | Via FB4 [/RESET']: CN6.C |
Via R36: +5V (pull-up)`. Click the bare board to stop the test.

The inspector draws the chip large, like a datasheet: the pins numbered
and colored, the nets outside, the gates inside, wired to their pins. A
wire shows what is on it; the output of a gate shows what the gate itself
drives, so a gate driving high into a net that stays low is plain to see.

Open (Ctrl+O, Cmd+O on the Mac) shows a file browser, starting in
`carts/`. It lists folders and the files the running machine takes, and
says what each file is, from its header, or why it can't be used. Type to
narrow the list, or type a path and press Enter to go there. Arrows and
Enter, or a double click, open a file; Backspace goes up; Esc closes.
Recent files stay in the control panel, one click away, and files can be
dropped on the window too.

"Save state" keeps the machine as it is, faults and all, in one slot in
memory, until you quit; "Load state" puts it back. "Save to file..." writes
the same to a `.hwstate` file, with a copy of the cartridge or ROM plugged
in and the repair notes typed in the box below the buttons. "Load file...",
Open, or dropping the file on the window brings the board back exactly as
it was, whichever board is on the bench: the same picture, the same pins,
the notes. A file saved by an older version of the simulator loads too,
unless that version laid the machine out differently in memory (it then
says so). A file is 2.5 to 3.5 MB. A file that has been altered so
that it would point the simulator at its own code is refused; still,
a save file is the simulator's memory, so open those from people you
trust.

### Diagnosing a screenshot

A picture of a real broken machine's screen (a USB capture card
screenshot: PNG, JPG or BMP) can be matched against `fault_atlas.bin`,
the screens of thousands of simulated faults: drop it on the window,
use "Diagnose screenshot..." in the control panel (`--diagnose FILE` on
the command line), or copy it and press "Paste screenshot" (or Ctrl+V). The window lists the faults whose screen looks most
alike; "Apply & Inspect" puts one on the board, power cycles it and shows
where it is. It only matches against the board on the bench. All six
boards have their own pictures in the atlas. An atlas file with none of
the board's (an older one, or a new board before `--build-atlas` has run
for it) falls back to the Issue 3B's for a Spectrum, the same circuit,
and probing does the same with its readings; both windows say so.
"Narrow down by probing..." takes the best matches (however they move:
a screenshot can't tell) to "Diagnose by probing" below, which then asks
for the pins that tell their causes apart; with that window open, a
screenshot dropped on the window narrows its faults by itself, and
opening it later from the control panel starts it on the screenshot's
pictures too.

Keep `fault_atlas.bin` and `carts/` next to the program (or pass `--atlas`
and `--carts`). The atlas is not in the source code: download it from the
[Releases page](https://github.com/zzleezz/8bit-hw-simulator/releases), or build it with `--build-atlas=full` (hours on a fast PC). The file dialog is Windows' own, AppleScript's on macOS,
and zenity or kdialog on Linux; without either, drop the file on the
window. `--build-atlas=full` builds the atlas again (see Command line).
It only adds the faults the file doesn't have yet. After a change to how
a few parts work, `C64HW_ATLAS_REDO="IC5 JROM1 LNK2 IC5.20 IC5.27"
./c64-hw-sim --build-atlas=full` runs again the faults on those parts and
on the nets of those pins (as the links join them, and one series
resistor further); the newest run of a fault counts, and a picture no
fault gives any more goes. `C64HW_ATLAS_TODO=1` lists what would run.

### Real repair records

Found what was wrong with a real machine? "Record the repair..." in the
diagnose window keeps it: a small window opens beside the board, and a
click on a chip of the board marks it faulty (outlined in orange; click
again to take it off). Say how sure it is (swapping it fixed it, tested
bad in a chip tester, or several replaced and not sure which), add notes
and your name if you like, and save. The record keeps the screenshot,
the board, the chips and what each one is, in `repairs.bin` next to the
program (`--repairs FILE` for another), a file of its own: the atlas
gets rebuilt, the records don't. From then on, a screenshot that looks
like a record's (75% and up) shows it above the simulated matches, under
"Seen on real boards", with its chips, how sure, who and when. "Match
real repair records too" leaves them out. A record mostly finds the same
screen again: a dead CPU gives a different garbage screen each time.

"Repair records..." (in the diagnose window and the control panel) lists
them all, newest first, with their pictures:
- "Use in diagnosis" leaves one out (on this PC only), and under
  "Datasets" all of one person's at once.
- Your own can be edited (how sure, notes) or deleted (click Delete
  twice). Someone else's are read only.
- "Merge a file..." adds someone else's records: new ones are added,
  newer versions replace older ones, the rest are skipped. "Export
  yours..." writes your own to a `.hwrepairs` file to share; "Export
  all..." everyone's. An edit or a deletion travels with the next export:
  a deleted record stays in the file, flagged, so merging an old copy
  doesn't bring it back.
- "Record a repair without a screenshot..." for one you know the chips
  of but have no picture of.
- "Statistics" counts the chips found faulty, per kind of machine.
- Searching by hand: click a chip on the board (or open the Inspector
  on it) and "Repair records with U7 faulty (3)..." shows only the
  records that found that chip faulty, on any board of that kind.

The records also move chips up in "Diagnose by probing": a chip found
faulty on real boards of the family a little (more with each record), a
chip of a record the last screenshot matched a lot (the window says
which, and by how much). A record not sure which of several chips counts
half for each.

Each record keeps its screenshot. When the way screenshots are read
changes (`ATLAS_READ_VERSION` in atlas.h), records read the old way are
read again from it the next time they load, once.

Without the window: `--add-repair shot.jpg --machine zx48 --chips
IC1,IC13 [--certainty 0|1|2] [--notes TEXT] [--owner NAME]` records one,
and `--diagnose-text shot.jpg --machine zx48` prints the diagnosis,
records and their scores included. `--list-repairs`, `--merge-repairs
FILE`, `--export-repairs FILE [--all]` and `--reread-repairs` do the
rest. The file is a chunk log like the atlas: a damaged end loses only
the damaged record, and a file that isn't a records file is never
written to or merged.

### Diagnosing by probing

"Diagnose by probing..." in the control panel turns a logic probe on the
real broken machine into a search. For every fault, the atlas also keeps
what a probe showed on every net of the simulated board, a few seconds
after power on. Switch the real machine on, wait a few seconds, and say
what its screen shows, in steps: the kind of picture (vertical bars,
garbage in some cells, a blank screen...), then its colors, then if it
helps the very picture; stopping at any step keeps every picture that
fits it. The window asks for one pin at a time, ringed in
cyan on the board: probe it on the solder side, where pad and trace are
one, and click Low, High, Pulsing or Floating. Each reading narrows the
list. Any other pin can be probed instead: click it on the board, or type
it (`U7.3`).

Faults aren't all as likely. The window weights each one the way they
come to the bench: the PLA first, then the CIAs, the CPU, the RAM and the
ROMs, then the video and sound chips and the logic; a chip dead or its
output stuck counts fully, a leg open or a solder bridge much less. A net
stuck low or high is named after the chip driving it ("U17 PLA pin 16
(/KERNAL) shorted to GND, or the output dead inside the chip"). Under
"Chip markings on the real board", say who made the chips: MT RAM comes up
to the PLA's level, MOS made logic close to the CIAs', and a PLA not made
by MOS (the early ones) drops to the bottom. Groups that fit the readings
as well are listed the likeliest first, each with its likelihood.

"Next pin by" picks the pin asked for:

- **Bench procedure** (the default) starts the way a repair does: reset and
  the clock at the CPU, then the PLA's outputs (/CASRAM, /KERNAL, /BASIC,
  /CHAROM, /IO, GR/W; on the Spectrum, the ROM select and /MREQ), skipping
  any that can't tell the suspects left apart; then as below.
- **Likeliest faults first**: the pin whose reading best splits the likely
  faults.
- **Fewest probes**: the pin that splits all the faults left most evenly,
  every fault as likely, on the smallest chip that has that net.

Faults that show the same thing on every net are one group: no probe can
tell them apart, and the window says so. A reading that doesn't fit drops
a fault down the list rather than off it, so one misread pin doesn't lose
the true fault. Readings are of nets: a leg that doesn't reach its pad
reads differently on the chip's side.

`--probe-test` (see Command line) puts faults with a symptom on a board,
probes it later than the atlas did, and lets the tool diagnose it, for
each "Next pin by" on the same faults, picked as often as they come to
the bench. With every reading right (120 faults over the six boards),
the true fault is among the best fits 93-96% of the time and listed
first 75-81% (96-97% and 90-95% when the screen is picked too). With one reading in ten wrong on
purpose, it stays on the list 95-97% of the time and is in the first six
groups shown 65-74% (86-91% with the screen). The bench procedure costs
about one probe more than the others and does as well. About a quarter of the
C64's faults and a fifth of the Spectrum's look healthy on every net:
probing can't find those, and the screen is what tells them apart.

### Browsing the atlas

"Browse the atlas..." in the control panel shows every picture the fault
atlas has of the board on the bench (booting normally), as a grid of
thumbnails, most causes first, each with its label, its number of causes
and the chips they are on. Drop-downs narrow it down by what the screen
shows: the kind of picture (the probing window's kinds), the border and
screen colors (the color covering most of each), how it behaves (still,
blinking, changing, flashing a code, no picture), what the CPU was doing
at the end under one of the causes (running, held in reset, stopped,
jammed), and a chip one of the causes is on. Each choice says how many
pictures it leaves with the other filters as they are. Probe readings
narrow it down too: type a pin (`IC2.26`), pick what the probe shows on
the real machine and click Add. A picture stays if one of its causes
shows every reading; each reading also says what a healthy board shows
there. Click a picture
for its causes: Apply & Inspect puts one on the board, and "Narrow down
by probing..." opens "Diagnose by probing" with only that picture's
faults. A Spectrum board the atlas file has no pictures of shows the
Issue 3B's, the same circuit, and its faults go on the board on the
bench. `--browse-atlas[=N]` opens the
window at start, its Nth picture picked.

## Commodore 64

Chips are boxes named by role:

- RAM1 to RAM8 are the 4164 DRAMs. Each one holds a single bit of every
  byte: RAMn has data bit n-1.
- VRAM is the 2114 color RAM.
- BASIC, KERNAL and CHAR are the ROMs.
- PLA is the address decoder.

In the inspector, the output mark of a 7406 is the open collector
transistor: blue while it pulls the line low, gray while it is off and
the pull-up decides. For the PLA the inspector lists its product terms,
lit while they are true, with every input colored by its level: which
term selects what, and which input keeps a term from firing.

RESET works like a reset cartridge: it shorts /RESET to GND on the user
port. Hold it, or keep it pressed to look at the board in reset. The CPU,
the CIAs and the SID reset; the VIC, which has no reset pin, and the RAM
don't. Power cycle switches the C64 off and on again.

Programs (.prg) are loaded and RUN, or only loaded with Shift+Enter; the
browser says if one is BASIC or machine code, and its address. They are
written straight into the DRAM chips, as a freezer cartridge would do it,
so loading works on a broken board too.

Cartridges (.crt) are plugged into the expansion port (the C64 is
switched off and on, as you would do); the browser shows a cartridge's
kind and name. The plain ones are supported, 8K, 16K and Ultimax, which
is what the diagnostic cartridges are: Dead Test replaces the KERNAL and
runs on boards with dead RAM or ROMs. A cartridge only drives the
expansion port pins, everything else is the board: a dirty contact on
the port is a fault on a CN6 pin.

Raw ROM images (.rom, .bin: an EPROM dump, no header) go in too. A 2-byte
load address in front (8194, 16386 bytes...) is skipped, and the wiring
is guessed from the contents: "CBM80" at offset 4 is an autostart
cartridge at $8000 (8K, or 16K when a second bank without signature
follows); otherwise a reset vector into $E000-$FFFF at the end of the
bank is an Ultimax ROM, like Dead Test and DesTestMAX. An 8K image can
also go in the KERNAL socket U4 in place of the KERNAL ("Cartridge
(Ultimax / 8K)" or "U4 KERNAL socket" in the Programs and cartridges
panel); an image with a KERNAL jump table goes there by itself. On the
250469, whose U4 is a 27128 holding BASIC too, the 8K image goes in its
high half with the stock BASIC below, as an owner burns one. A 16K image
of BASIC then a KERNAL (a 27128 dump for the 250469) goes in U4 whole;
on the 250407 only its KERNAL half is used (U3 holds BASIC). An image
of several 8K banks (16K, 32K, 64K) is a DesTestSwitch EPROM: each bank
is wired as it asks, one at a time, and a "ROM bank" switch appears under
RESET; flipping it switches the C64 off and on, like the real switch.

Dead Test gives the flash codes of its manual (one flash for U12, bit 7,
up to eight for U21, bit 0). For the Diagnostic 586220 cartridge you can
plug in its test harness, the dongles that loop back the user port, the
cassette port, the serial port, both control ports and the keyboard (as
built by Sven Petersen, an open hardware copy of Commodore's). With it a
healthy board passes every test, and a broken contact, chip or clock
shows where Commodore's manual says it should. Both cartridges are in
`carts/` (they are Commodore's, see `carts/LICENSE`).

Type on your keyboard as usual. ESC is RUN/STOP, PgUp is RESTORE, TAB is
CTRL, and left ALT is C=.

### The 250469 board

The C64 has two boards: the ASSY 250407 (the breadbin's, the default)
and the ASSY 250469 (Rev. 4: the C64C's, also fitted in late breadbins).
Pick one with the drop-down under the machine buttons, or `--board
250469` (`--machine c64c`). Its layout and netlist are bwack's replica.

It has the same machine with fewer chips:

- RAM1 and RAM2 are two 41464s (64K x 4): RAM1 holds the low four bits
  of every byte, RAM2 the high four.
- ROM (U4) is one 27128 with BASIC (in its low half, at $A000) and the
  KERNAL (high half, at $E000), as the 251913-01. CHAR is the 2332.
- The CPU, VIC-II and SID are the 8500, 8565 and 8580, the HMOS
  versions of the 6510, 6569 and 6581, simulated by the same cores. The
  8565 and 8580 have no 12 V: the VIC runs on +5V, the SID on 9 V made
  by Q5.
- An 8701 (U20) makes the dot clock and the color clock from the crystal
  Y1: an open crystal stops both, so there is no picture at all (on the
  250407 only the colors go).
- PLA (U8) is the 251715-01 "Super PLA", a 64 pin chip that does the
  work of the 250407's PLA, its two 74LS257 and the 74LS258 (the RAM
  address multiplexers), the 74LS373 (the VIC's address latch), the
  74LS139 (the I/O decoder), a 74LS08 gate (the color RAM select) and
  the 556's RESTORE half. There is no 556: the power-on reset comes from
  C43 and a 74LS14 (U23), which also squares the mains for the time of
  day clocks and buffers the RESTORE key.

Its pin names are bwack's. Commodore never published what is inside the
Super PLA, so it is simulated as the 250407 chips it replaced, wired as
they were (the same approach as the Spectrum Issue 6A's ZX8401). That
much follows from its pins and from the rest of the board, and the
healthy board boots, passes the self check and runs Dead Test and the
586220 diagnostic cartridge. But some of it is assumed, and a fault on
these parts may not look exactly as on a real 250469:

- The memory map equations are the 906114 PLA's. The 251715 is said to
  implement the same map, but no read-out of it was found.
- Its RAM_R/W pin (to the RAMs' and the color RAM's /WE) is taken to be
  the CPU's R/W, held high in VIC cycles. On the 250407 the color RAM got
  the PLA's GR/W instead, timed by /CAS.
- NMI simply follows the RESTORE line. On the 250407 the 556 made a
  pulse of about 1 ms; the Super PLA may have a timer of its own.
- PHI0 and the four /CAS0-/CAS3 outputs do nothing (the /CAS pins aren't
  connected on this board). Inside there is no timing beyond the usual
  one step per gate, as everywhere in this simulator.
- The PLA inspector shows the 906114's product terms on the Super PLA
  too, on its pins (and says they are assumed). Two of the 906114's
  outputs have no pin on it: /IO, which feeds the I/O decoder inside, and
  GR/W, unused here; they are shown with the level they would have.

Other things assumed or simplified on this board:

- The 8701's pins 6 (dot clock), 7 (PAL/NTSC, to GND through J3), 8
  (color clock), 13 and 14 (the crystal) are known from the netlist; its
  other pins are named NC (no description of them was found). Its clocks
  toggle once per half bus cycle, like the 250407's oscillator.
- The power-on reset: capacitors aren't simulated, so C43 holds the
  74LS14's input low for 50 ms after power on (as the 556 did on the
  250407). Removed or open, there is no reset pulse; shorted, the C64
  stays in reset.
- The RESTORE key's debounce (R17 and C59) is not simulated: the line
  follows the key.
- The EMI filters on the joystick lines (EM1-EM9) pass the signal from
  pin 1 to pin 3; their capacitors aren't simulated. Of the choke L1, only
  the +5V winding is a wire; the others are checked as part of the power
  path.
- The 41464's address pins are named as in the datasheet; the board
  wires MA0-MA7 to them in another order (MA0 on A6, MA6 on A0). That
  only matters for which cells a fault inside the chip hits.
- The fault atlas has this board's own 1516 faults (188
  different pictures), so screenshots and probing use its parts. An atlas
  built before them falls back to the 250407's pictures, listed without
  an "Apply & Inspect" button (the 250407's U7 is the CPU, the 250469's
  the VIC); the probing guide then asks for the atlas to be rebuilt.

## ZX Spectrum 48K

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
  drop-in replacement credited above; the pin names are the 6A KiCad
  project's. The gates' order matters: /CAS falls a step after the
  select (the column is on the RAM's pins first: on the board, the
  gates and C105).
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

### The ULA

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

### The Z80

floooh's Z80 runs one T-state per call and puts its pins out when it suits
an emulator. The ULA needs more: it looks at the address and /MREQ at the
right half of a T-state to decide whether to stop the clock, and it can
stop the clock in either phase. So `chips/z80.h` is now stepped on every
edge of its clock, and its pins follow the Zilog timing diagrams. Its
instructions, flags and timings are those of the original core.
`tests/z80` has the tests: the pin waveforms of every kind of bus cycle,
and ZEXDOC and ZEXALL. `chips/codegen` has the code generator.

## Command line

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
runs these on both machines, and under Dead Test and the Spectrum
diagnostics ROM.

A .crt, .rom or .bin file given on the command line is plugged in before
power on (a raw image picks the machine it is made for), `--harness`
plugs the 586220 test harness, `--bank N` sets the switch of a ROM of
several banks, `--socket u4|port` puts an 8K C64 image in the KERNAL
socket or the expansion port (on the 250469 the KERNAL socket holds BASIC
too: see "Commodore 64").

Other options:

- `--at 150:U6.17:open --at 300:repair` breaks and repairs parts while the
  C64 runs; `RESET:pressed` and `RESET:released` work there too.
  `--at 150:save:FILE.hwstate` saves the board to a file at frame 150;
  `--at 150:load:FILE` loads a program, snapshot or cartridge then.
- `--load-state FILE.hwstate` starts with the board of a save file, as it
  was saved (with or without a window).
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
  above was made.
- `--inspect U8`, `--browse`, `--browse-atlas[=N]`, `--view X,Y,ZOOM`, `--mouse X,Y`,
  `--down X,Y@F`, `--up X,Y@F`, `--wheel X,Y,DY@F`, `--key NAME@F` and `--chars TEXT@F`
  (graphical mode) open the inspector, the file browser or the atlas browser, aim the board
  view, click the mouse, turn the wheel and press keys at UI frame F, for
  screenshots and tests of the interface. `--ui-stats` prints the
  interface's frame times every second.
- `--theme light`, `--theme dark` or `--theme system` (the default)
  (graphical mode) picks light or dark windows.

`tests/faults.sh` runs 31 common faults. The expected symptom of each one
was written down before the simulator was run. `MACHINE=c64c
tests/faults.sh` runs 25 on the 250469: the same faults on its parts,
and its own chips (the Super PLA, the 41464s, the 8701, C43).
`SERIAL=1` runs one case at a time instead of all at once, on one core.
Two expectations the simulator doesn't meet, on both boards alike: with
the CPU's A15 open the C64 boots with a screen of garbage characters
(not a black screen; not looked into further: a guess is that A15
floats high, so the CPU's low 32K lands in the high 32K and the screen
it writes isn't where the VIC reads), and without the color RAM the text has one color, not random
flickering ones.

`tests/diag.sh` runs the diagnostic cartridges and checks what they report
against their manuals: Dead Test's flash code for every DRAM, the 586220
with its harness on a healthy board and with faults. It takes a few
minutes, so it runs only when you start it. `MACHINE=c64c` runs it on the
250469. There Dead Test's chart can't name a chip: a 41464 holds four
bits, and Dead Test flashes once for any error of more than one bit (two
4164s out on the 250407 do the same), so a lifted data leg is checked
instead (D7 1 flash ... D0 8, as the chart). The 586220's control port
test fails even on a healthy 250469: bwack's replica has 2200 pF on the
paddle lines (C80, C81) where the 250407 has 1800 pF, the harness's
paddles then read about 131 instead of 107, and the cartridge calls the
port bad and blames the SID. Whether a real 250469 has 2200 pF there is
not known here; that check is left out on this board. `--flashes` (headless) counts
Dead Test's flash series.

`--probe-test N` tests "Diagnose by probing": N faults of the atlas at
random on each machine (as likely as their bench weight), run on a board
and probed wherever the tool says, with each of the three ways of picking
the next pin, first without knowing the screen, then knowing it. `--probe-noise PCT`
takes that share of the readings wrong on purpose; `--seed S` picks other
faults.

## How it works

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
- the 250469's Super PLA, as the 250407 chips it replaced (see above).

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

PLAN.md explains the reasoning behind these choices.

## Limits

It is slow, between a third and half of real time on the computers listed
above (a fifth to a third for the Spectrum), because every pin of every
chip is simulated.

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
