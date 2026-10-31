# Changelog

What changed in each release of 8bit-hw-simulator, newest first.
Changes not yet in a release are under "Unreleased".

This project is AI-generated: the code and these notes were written with
Claude (Anthropic's AI), directed and tested by zzleezz.

## Unreleased

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
