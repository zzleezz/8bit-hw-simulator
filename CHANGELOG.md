# Changelog

What changed in each release of 8bit-hw-simulator, newest first.
Changes not yet in a release are under "Unreleased".

This project is AI-generated: the code and these notes were written with
Claude (Anthropic's AI), directed and tested by zzleezz.

## Unreleased

- Logic analyser captures of the Spectrum's edge connector (work in
  progress, before the real analyser was built): read a `.lac` or CSV file
  from the LogicAnalyzer program (`--la-info`), record the simulated board
  as the analyser would (`--la-record`), and compare a capture with the
  simulated board bus cycle by bus cycle from reset (`--la-compare`). See
  README, "Logic analyser captures".
- Logic analyser: capture from the analyser itself over USB, without the
  LogicAnalyzer program (`--la-devices`, `--la-capture`; Windows and
  Linux). The channel map is now the real one (Jim's video), checked on a
  real Issue 2 that matches the simulated one for every bus cycle.
- Logic analyser: capture at a stage of the boot (`--la-trigger reset`,
  `ramcheck`, `ramdone`, `copyright`, `main`, `interrupt`, `addr=XXXX`),
  re-armed when the trigger was only a glitch on the address lines; the
  simulated board is recorded at the same stage to compare with. The
  comparison now tells what the CPU does apart from the data it moves (RAM
  data differences alone are the machine's state, not a fault), checks that
  RAM reads give back what was written, and reports the RAM test's result
  (P-RAMT). `--la-timeline` lists the boot's milestones, `--la-log` keeps a
  copy of what's printed.
- Logic analyser, `--la-diagnose`: a first stage, idle, before any reset:
  the Spectrum left running, captured at once at 14 MHz and for 49 ms at
  2 MHz. It needs no reset, so it works when /RESET is broken: /RESET low
  or pulsing, no clock, /WAIT or /BUSRQ held, /INT held low, missing or
  wrong, the CPU halted or astray. When the CPU isn't running at all the
  boot's stages are skipped. Taken on a real working Issue 2 at the
  copyright screen: the same as the simulated one. A capture taken at once
  no longer starts with a fake fetch from 0000 (the master board zero-fills
  its samples before the trigger).
- Logic analyser, `--la-diagnose`, from real faulty CPUs, logic and RAM:
  an address line the ROM sees otherwise than the edge connector (its
  wrong reads are its own bytes from the address with that bit the other
  way: a weak or floating CPU output, found on a real faulty Z80's A11);
  a RAM bank's address line that doesn't get through (the RAM test finds
  the byte 2^k above the bank's start the same cell as the start), with
  its multiplexer; a RAM bit the CPU reads otherwise than the analyser
  (the RAM test's DEC writes back one less than what the CPU got: a slow
  chip); /RESET held low with the button let go (a CPU can short it). A
  capture that can't be taken after a stage was wrong now ends the
  captures, not the diagnosis.
- Logic analyser: a bus cycle's address is now the one held longest in
  the cycle, not the one at /MREQ's fall, which with some ULAs (their
  clock shaped differently) was still changing: a faulty-looking ROM and
  a stage that seemed to go astray on a real board were only that.
  `--la-diagnose` also tells a CPU clock the analyser can hardly see (its
  pulses too narrow, from the ULA) and one that stops later, and then
  gives the rest as what the bus shows on that clock.
- Logic analyser, `--la-diagnose`: the ULA's timing with the CPU
  (contention). When every wrong byte of the lower RAM was written or read
  while the ULA fetched the screen (placed from /INT in the capture), and
  the right ones weren't, it names the ULA first and the RAM chips of the
  bits that lose after it (a real faulty ULA lost D3, D4 and D6 this way).
  A RAM bank wrong as a whole now names the data resistors to the CPU too,
  not only the address multiplexers. `--la-all` takes the later stages
  after one that's wrong and adds what they show (diagnose.bat uses it).
  Also: a keyboard line or key read low, a port read with all five lines
  low (the ULA's), "taken while /RESET was held" (and once more), no clock
  and a CPU not running as one finding, and the RAM test's reads and
  writes at the byte it stopped on. A simulated ULA fault for testing it:
  `--fault IC1:nocontention`.
- Logic analyser: `--la-diagnose DIR` diagnoses a Spectrum. It captures the
  boot stage by stage (reset, RAM test, its end, the copyright message, the
  main loop, a frame interrupt; it asks for each reset), compares each with
  the simulated working board, stops at the first that's wrong and says
  what to look at: a stuck line (with the Z80's pin), a data line, the ROM,
  the RAM chip by its bit and bank (or what the bank's chips share), where
  the ROM's own RAM test stopped, a CPU that isn't running or has no clock.
  A stage that never comes: what the CPU was doing then instead.
  `--la-replay` runs it again on DIR's captures, `--la-simulate --fault`
  on a simulated faulty board (21 faults tested, each named right).
- Logic analyser: the zx firmware for the boards (`la-firmware`, a small
  change to gusmanb's, GPL-3): its trigger needs the address for 60 ns, so
  the glitches as the Spectrum's address lines change don't fire it (they
  faked the later boot stages' addresses on a real Issue 2), and a capture
  is sent as soon as it's done. `--la-bootsel` restarts a board to flash
  it, `--la-devices` says which firmware each has, `--la-at MS` captures a
  set time after the reset.
- Diagnose by probing: the bench procedure always starts with reset, then
  the clock, at the CPU (C64 and Spectrum), even after picking the screen.
  Before, it skipped them when the faults left all showed the same there.
- Diagnose by probing: a net reachable only at a connector (the Spectrum's
  keyboard rows, which are A8-A15 behind diodes) is suggested only when it
  tells clearly more than a chip's leg. Before, it won ties against the
  chip's leg, so the keyboard connector came up far too often.
- How likely each part is to fail now follows a repairer's experience on the
  Spectrum: TR4 (the DC-DC converter's switch) first, then the lower RAM,
  the upper RAM, the CPU, the ULA and the ROM alike, then the logic.
  Resistors, capacitors, diodes, links, coils and crystals hardly ever fail
  (on the C64 too). Before, the ULA ranked above the RAM and TR4 with the
  resistors.
- Diagnose from screenshot: pictures that match about as well are now put
  in order of how likely their causes are (a cause 10 times likelier is
  worth about 0.7 points), and each picture's causes are listed likeliest
  first. A real bad ULA no longer comes out as "resistor R17 shorted".
- Diagnose from screenshot (Spectrum): a picture caused by faults between
  the ULA and the lower RAM also names "ULA (inside)". The simulator has the
  ULA at its pins only and can't show a ULA failing inside, but such a ULA
  gives the same pictures.
- TR4 and TR5 (Spectrum DC-DC converter) can fail at either junction,
  shorted or open (no +12V and no -5V, like the transistor taken out), or
  weak: it tests good but fails under load, so the 4116s' supplies sag and
  their cells forget. In the part's menu and in the atlas (50 new runs).
- An "Electrolytic caps: original / replaced" choice in both diagnosis
  windows. With original capacitors they warn that worn ones make faults
  the simulator doesn't have, so the results may not be reliable.
- Diagnose from screenshot: the screenshot stays at the top of the window
  while the list of matches scrolls under it, so it can be compared with
  every match.
- Diagnose by probing: a fifth reading, "Bad levels", for a line that is
  active on a scope but never reaches a proper low or high (two chips
  driving it at once, a weak or dead output). It matches the nets where
  the simulation saw outputs fight, and against a healthy-looking net it
  counts as a reading off. Under each such reading the window lists the
  chips that drive that net, and those one resistor further, since the
  simulator can't make an output weak and the atlas rarely has the cause.
  Before, a fight counted as "floating".
- Diagnose by probing: "Same as a healthy board" enters whatever the
  simulated healthy board shows on that pin, for when the scope looks
  like the simulator's. Each reading button explains itself when pointed
  at: pulsing is any activity at all, even a rare pulse; floating is
  nothing driving the line.
- The README's probing figures measured again on today's atlas (140
  faults over the seven boards). "Bad levels" changes them by no more
  than chance: the probing code from before it gives the same.
- Diagnose from screenshot (Spectrum): a screen nothing ever wrote, as when
  the CPU never runs, is recognised by its kind: the three thirds of the
  screen alike, busy across and changing from one character row to the
  next. It is matched to the board's "CPU never ran" pictures, labelled
  "Screen never written". The real RAM's power-on pattern and the border
  are random, so the simulated picture never matched pixel for pixel: a
  real dead Z80 used to come out as ULA faults, and now comes first.
  Pictures of a bad ULA, whose bars run down the screen unbroken, are not
  affected.
- A healthy board sheet: pointing at a pin also says what a working board
  shows there ("healthy board: PULSING, 705 kHz, high 83%"), and "Healthy
  board sheet" (Diagnose section) opens every pin of every chip and
  connector of the board on the bench as a page to read or print: its
  net, what a logic probe shows, and for a pulsing pin how fast and how
  much of the time high. `--healthy-sheet DIR` writes the page of every
  board.
- Save files are signed with a key made on this PC. A file without this
  PC's signature (someone else's, one saved by an earlier version, or one
  changed since) loads only after a question, since one altered on
  purpose could make the simulator read or write where it shouldn't.
- Repair records: "Gaps in the simulator" diagnoses every record's
  screenshot again and lists the repairs whose faulty chips none of the
  best matches names: faults the simulator is missing. `--repair-gaps`
  prints the same.

## V1.1 (2026-10-30)

- Diagnose: a "Paste screenshot" button (and Ctrl+V) diagnoses the picture
  on the clipboard: a screenshot copied from the capture program, or a
  picture file copied in the file manager. No need to save it first.
- Diagnose by probing: probe on the chips' legs (top side) or on the solder
  side ("Probing on" in the window). On a lifted or unsoldered leg the
  probe shows the chip's side (what it drives, or floating for an input),
  which tells an open leg apart from a healthy board; the pad alone often
  can't. It can also ask for one leg when only that tells the faults left
  apart. Uses the atlas's new leg readings (LEGS), in the atlas that
  comes with this release.
- A link or part that isn't fitted now says why when clicked: which option
  in the controls fits it, or that the board has no link there.
- Diagnose from a screenshot: the screen's edge is found in unevenly lit
  pictures too (a phone photo, brighter in the middle), where the whole
  picture used to be taken for the screen. A blurred screen edge, half
  border color, is no longer taken for something drawn on the screen.
- The menu of a chip's pin is wide enough for its longest choices
  ("Output dead inside the chip, stuck high" was cut off).
- Chip view: an arrow inside each pin shows which way it goes (into the
  chip: an input, out of it: an output, a diamond: both ways; none for
  power and analog pins).
- Pins wrongly called outputs are fixed: on the 74LS629 oscillators (U31)
  the frequency, range, capacitor and OSCVCC pins, on the 556 timer (U20)
  the THR and CV pins, the 8701's crystal pins, the LM1889's analog pins
  and some unused (NC) pins. Analog pins now say "analog" when pointed at.
  The atlas build drops the faults this removes ("output dead" on those
  pins) when run with C64HW_ATLAS_PRUNE=1.
- The fault atlas is corrected: 316 Spectrum faults (Issues 3B, 2, 1 and
  4B) had screens from an older version of the simulator, so a screenshot
  or probe diagnosis could suggest the wrong fault for them: 274 open pins
  and 42 solder bridges, mostly on the address multiplexers IC3 and IC4.
  It also has the new leg readings for probing on the chips' legs.

## V1.0 (2026-10-29)

First release of this fork of antirez/c64-hw-simulator. New compared
with the original:

- Four more ZX Spectrum 48K boards beside the Issue 3B: Issues 1, 2, 4B
  and 6A (the 6A's ZX8401 simulated as the 74LS chips it replaced), 48K
  or 16K, with their ULA, ROM and RAM chip options
- The C64C's board, ASSY 250469, beside the breadbin's 250407; its Super
  PLA (251715-01) simulated as the chips it replaced
- A fault atlas: about 10,400 simulated faults over the seven boards,
  with the picture each gives and the logic probe readings
- Diagnose from a screenshot, and diagnose by probing (it asks for the
  pins that tell the suspects apart)
- Repair records, kept with their screenshots, that feed the diagnosis
- Save states to files, a continuity test, native file dialogs, a light
  theme (by Henry), a one-step Windows build (build.bat)
