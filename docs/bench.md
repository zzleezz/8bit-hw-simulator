# Using the bench

The window shows the board, the monitor, a control panel and an
oscilloscope. Use the buttons at the top of the control panel to switch between
the Commodore 64, ZX Spectrum 48K and Atari 2600 PAL.

Pins show their state as a logic probe would: red high, blue low, yellow
pulsing, gray floating, magenta when two outputs fight. Zoom in and the
chips show references, address ranges and pin numbers; zoom in more and
the logic chips show their gates, the others their pin names. Hovering a
pin tells what it is: connector pins have their function ("CN4.3 ATN
(SERIAL)"). It also says what a healthy board shows there ("healthy
board: PULSING, 705 kHz, high 83%"), to compare with the live reading:
the first time on a board, the healthy one is measured in the background
(15 to 25 seconds) and the tooltip says "being measured".

On the C64 and Spectrum, "Healthy board sheet" in the control panel's Diagnose section opens the
same as a page in the browser, to read or print: every pin of every chip
and connector of the board on the bench, with its net and what a logic
probe shows there on a working board, 5 to 6 seconds after power on (as
the probing window's healthy readings), and for a pulsing pin how fast
(its rising edges a second) and how much of the time it is high. The
simulator looks at the pins once per bus step, so it shows nothing faster
than half that rate (about 3 MHz on the C64, 14 MHz on the Spectrum), and
pins that are analog on the real board (video outputs, the ULA's
oscillator) show what the simulator makes of them, not what a probe sees.
`--healthy-sheet DIR` writes all C64 and Spectrum pages to DIR (about 2.5
minutes).

Click a pin to put it on the oscilloscope and to break it: floating,
shorted to GND, shorted to +5V, or, on an output, dead inside the chip
(stuck low or high: unlike a short, another chip on a bus fights it).
Click a chip to pull it out, or to inspect it. A ROM or RAM chip can also
break inside ("Inside the chip"): a dead data bit (on a 4164 or 4116,
the whole chip), a dead address line, one bad cell or corrupted byte,
or, on a DRAM, weak cells that forget. Click a resistor, a ferrite bead
or the crystal to open it. The control panel lists the faults, each with a
repair button; its heading counts them, in red. Click a fault in the list
to zoom in on it. On the board, a part with a fault has a white outline and a
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

The list starts with what drives the net, from what each pin drove this
frame: `Driven by U7.40`, and small triangles on the yellow wires point
away from it, the way the signal goes. A net several chips drive in turn
(a data bus, open collector lines) names them and has no triangles.
`Nothing drives it` (in orange) is a net with inputs but no output
driving it: a dead or pulled chip, or the output's leg broken open. A
supply, and a net that isn't simulated (analog), say nothing.

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
says so). File size varies by board. A file that has been altered so
that it would point the simulator at its own code is refused. Still, a
save file is the simulator's memory, and one altered on purpose could
make it read or write where it shouldn't. So every file is signed when
saved, with a key made on this PC (kept in `%APPDATA%\c64-hw-sim`, or
`~/.config/c64-hw-sim`), and a file without this PC's signature
(someone else's, one saved by an earlier version, or one changed since)
loads only after a question ("Load it anyway"). Saving it again here
makes it one of yours. `--load-state` loads one anyway, with a warning.

See the [diagnostics guide](diagnostics.md) for the C64 and Spectrum repair tools.
