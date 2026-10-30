# Changelog

What changed in each release of 8bit-hw-simulator, newest first.
Changes not yet in a release are under "Unreleased".

This project is AI-generated: the code and these notes were written with
Claude (Anthropic's AI), directed and tested by zzleezz.

## Unreleased

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
