# Changelog

What changed in each release of 8bit-hw-simulator, newest first.
Changes not yet in a release are under "Unreleased".

This project is AI-generated: the code and these notes were written with
Claude (Anthropic's AI), directed and tested by zzleezz.

## Unreleased

- Diagnose: a "Paste screenshot" button (and Ctrl+V) diagnoses the picture
  on the clipboard: a screenshot copied from the capture program, or a
  picture file copied in the file manager. No need to save it first.

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
