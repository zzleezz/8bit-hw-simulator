# Diagnostics and repair records

The fault atlas, screenshot diagnosis and probe-guided diagnosis cover the
Commodore 64 and ZX Spectrum. For live Spectrum captures, see the
[logic analyser guide](sinclair/logic-analyser.md).

## Diagnosing a screenshot

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
window. `--build-atlas=full` builds the atlas again (see the [command-line reference](command-line.md)).
It only adds the faults the file doesn't have yet. After a change to how
a few parts work, `C64HW_ATLAS_REDO="IC5 JROM1 LNK2 IC5.20 IC5.27"
./c64-hw-sim --build-atlas=full` runs again the faults on those parts and
on the nets of those pins (as the links join them, and one series
resistor further); the newest run of a fault counts, and a picture no
fault gives any more goes. `C64HW_ATLAS_TODO=1` lists what would run.

## Real repair records

Found what was wrong with a real machine? "Record the repair..." in the
diagnose window keeps it: a small window opens beside the board, and a
click on a chip of the board marks it faulty (outlined in white; click
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
- "Gaps in the simulator" diagnoses every record's screenshot as a new
  one would be, and lists the repairs whose faulty chips none of the 8
  best matches names: faults the simulator is missing, or shows
  otherwise than the real board did. Those explained only by a lower
  match come next. `--repair-gaps` prints the same for every record.
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
FILE`, `--export-repairs FILE [--all]`, `--reread-repairs` and
`--repair-gaps` do the rest. The file is a chunk log like the atlas: a damaged end loses only
the damaged record, and a file that isn't a records file is never
written to or merged.

## Diagnosing by probing

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
- Pulsing is any activity at all: a line mostly high with an occasional
  pulse is pulsing, as a logic probe's pulse light shows it.
- Floating is nothing driving it: no light, or a steady level around 1.4 V.
- Bad levels (on a scope) is activity that doesn't reach a proper low or
  high: two chips driving the line at once, or a weak or dead output. It
  matches the nets where the simulation saw outputs fight.
- "Same as a healthy board": the scope looks like the simulator's healthy
  board there (whatever it shows: pulsing, high...).

A level a logic chip still reads right (a high wobbling between 4.0 and
4.8 V) is that level: the simulation knows logic levels, not volts.

On a Spectrum, **Import capture...** takes the readings from the logic
analyser instead: every line at the edge connector, low, high or pulsing,
from a capture (best `idle.lac` from a diagnosis folder, the board left
running; `idle-slow.lac` next to it is read too, for the frame
interrupt). They are marked "analyser". The analyser reads a floating
line (about 1.2 V) as low, so its "low" counts as low or floating. On a
working or running board they fit the healthy board: a probe can't see a
RAM bit read wrong; on a dead one they narrow it fast (the dead Issue 1
boards: the CPU, the only fault left). `--probe-capture FILE --board B`
does the same on the command line and lists the faults that fit.

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

`--probe-test` (see the [command-line reference](command-line.md)) puts faults with a symptom on a board,
probes it later than the atlas did, and lets the tool diagnose it, for
each "Next pin by" on the same faults, picked as often as they come to
the bench. With every reading right (`--probe-test 20 --seed 11`: 140 faults
over the seven boards), the true fault is among the best fits 92% of the
time and listed first 65-70% (95% and 81-84% when the screen is picked
too). With one reading in ten wrong on purpose, it stays on the list 92%
of the time (96-98% with the screen) and is in the first six groups
shown 65-69% (84-88% with the screen). The bench procedure costs about
one probe more than the others and does as well. About a quarter of the
C64's faults and a fifth of the Spectrum's look healthy on every net:
probing can't find those, and the screen is what tells them apart.

## Browsing the atlas

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
