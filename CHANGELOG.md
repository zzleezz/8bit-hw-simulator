# Changelog

What changed in each release of 8bit-hw-simulator, newest first.
Changes not yet in a release are under "Unreleased".

This project is AI-generated: the code and these notes were written with
Claude (Anthropic's AI), directed and tested by zzleezz.

## Unreleased

- Continuity: the readout names what drives the net tested, from what
  each pin drove this frame ("Driven by U7.40"), and triangles on the
  yellow wires point the way the signal goes. A net several chips drive
  (a bus, open collector lines) names them; "Nothing drives it" (orange)
  is a net with inputs and no output driving it: a dead chip or an open
  leg.
- Pin colors: -5V and +12V get their own colors (to a logic probe they
  read low and high, like GND and +5V), and high is a true red.
- A fault on the board is outlined in white (no pin color is white),
  not red or orange.
- The drop-down lists with a tooltip open on the first click: the
  tooltip is drawn after the list now.
- The test ROM, version 7 (not yet run on the bench): result 12, the
  Spectrum's own ROM. A routine copied to the lower RAM writes 20 to
  port 1F, the ZX Diagnostics paging port the SteamCard Pro decodes
  too, which releases /ROMCS. It takes the CRC of 0000-3F7F and pages
  the cartridge back in; a good 48K ROM gives E295. The diagnosis tells
  apart a cartridge that didn't page out (the CRC is the test ROM's own)
  from a bad ROM. Result 11 also writes 18 to port FF: EAR must not
  change, as the ULA only answers ports with A0 low. Twelve captures
  now; version 6's image is kept (carts/zxtest-v6.rom). The simulated
  cartridge pages out the same way: a write to ports 00-1F with bit 5
  set, and /RESET pages it back in.
- The SteamCard Pro's I/O, traced on the card (v1.2): an LS138 on
  /IORQ, /WR and A5-A7 (writes only). Ports 00-1F are paging, 20-3F the
  data LEDs, and nothing is on E0-FF. The bank is set by the DIP switch
  only.
- Diagnose by probing: Save... and Load... The readings, the settings
  that rank them (the screen, the strategy, legs or pads, chip markings,
  the repair records' weights) and a report of the suspects, as a text
  file to read, send, or load again. --probe-load FILE prints the same
  report without the window, with the weights it was saved with.
- Diagnose by probing: Import capture... froze the program (its file
  dialog was opened while the window was being drawn, with the board
  locked). Its dialogs now open after the frame, like the others.
- Diagnose by probing, tidied: the reading buttons are filled in the
  board view's colours (low blue, high red, pulsing yellow, floating
  grey, bad levels magenta) and sit on one row; "Same as a healthy
  board" says, and is coloured as, the reading it enters. Less repeated
  text: where to probe is said only when it isn't what "Probing on"
  says, the suspects' split and the capacitor warning's reasons are
  tooltips, the start text goes once there are readings, the pin entry
  is one row, the file buttons one row. Each cause: its name and a
  normal-height Apply & Inspect button, the description under them; the
  healthy board no longer shows the test script's description.
- tests/probe-file.sh: a file saved by the window's Save... read back by
  --probe-load must give the report it holds; the files Load must refuse.
- The test ROM, version 5: each RAM bank's address lines (results 9 and
  10, after the bank's data walk: the upper bank now has one too) and
  the EAR input's level after OUT 00/08/10/18 (result 11). The
  diagnosis names, from the board's own wiring, the multiplexer output
  (both of a DRAM pin's lines wrong), the multiplexer input (one), the
  multiplexer itself (several of its outputs), or one chip's address pin
  (one or two data bits); the march's failure in the same bits then
  points back to it. Eleven captures now (results in the ROM's order);
  version 4's image is kept (carts/zxtest-v4.rom) and its captures are
  read as before. tests/la-zxtest.sh: two more faults (IC3.2 open,
  IC25.7 to GND).
- The test ROM, version 6: test 2 checks a CRC-16 (CCITT-FALSE) of the
  cartridge's bytes instead of a sum, so bytes read swapped or from the
  wrong address no longer cancel out. The screen says ROM CRC. Version 5's
  image is kept (carts/zxtest-v5.rom).
- The EAR input on the Issue 1 and 2: MIC alone (OUT 08) reads as EAR, as
  a real Issue 2 does (0 1 1 1 after OUT 00/08/10/18; the Issue 3 on reads
  0 0 1 1). The test ROM's diagnosis judges result 11 by the board's
  issue: other values name the resistors around the ULA's SOUND pin, or
  a wrongly set issue.
- The test ROM's diagnosis: result 11 is printed under its own heading
  (it comes in the upper RAM's capture); a folder of older captures
  counts its own number of captures; on an Issue 6A the ZX8401's input
  pins are named (not the 74LS157's); "the CPU drives it right" only
  when test 1 passed; IN FE reading FF both times is "nothing answered
  the port read", not the EAR circuit; with the contention wrong, the
  lower RAM's address lines aren't listed pin by pin (the contention
  explains them).
- tests/la-zxtest.sh: a real Issue 2's v5 captures, the same read as an
  Issue 3B (its EAR values aren't a 3B's), a healthy Issue 3B; three
  duplicate cases dropped (IC8/IC13 data0=0, IC2.16 to GND).
- zxtest/zx-diagnostics-crosscheck.md: what ZX Diagnostics tests on a 48K
  compared with our test ROM, and what to add (the floating bus, the
  ULA's port decoding).
- A test ROM for 16K/48K Spectrums, made for the logic analyser
  (zxtest/, built with sjasmplus): nine tests in a loop (data and address
  lines, the ROM's sum, a march over the lower and upper RAM, the ULA's
  ports, the interrupts, contention, the upper RAM's refresh), each
  writing its result to an address in the ROM area the analyser triggers
  on, and the results on the screen. Version 3: the refresh test (the
  upper RAM filled, read back after 1.6 s untouched); the border yellow,
  not green, when no upper RAM answers (right on a 16K only), with the
  two bytes read back in the result (FF FF: nothing there; else the stuck
  chips are named); the ALIVE line shows the version (it showed 0000).
  Version 4: a table of its constants at 00A0 (addresses, the frame
  counts, the version), read by the diagnosis instead of copies in the C
  code. Captures of an older version are compared with that version's
  image when it is kept (carts/zxtest-v3.rom). Unlike the ROM's own RAM test it holds a 1 in every bit: a
  simulated weak IC22 (D7) passes all six boot stages and fails it. Run
  on a real Issue 2 from a SteamCard Pro cartridge: every result the same
  as the simulated board's. zxtest/zxbench.rom: the cartridge's bench
  check.
- --la-record plugs a ROM image named on the command line into the
  simulated board.
- The test ROM's diagnosis: --la-diagnose DIR --la-zxtest, and in the
  logic analyser window "It runs: the test ROM (a cartridge)". No reset:
  eight captures in about 30 s, each test's result put into parts (the
  RAM chip of each bad bit, the bank as a whole, /INT held low, the ULA's
  contention first when it breaks the lower RAM too). tests/la-zxtest.sh
  checks it on real captures and 18 simulated faults.

## V1.2 (2026-11-08)

- Logic analyser diagnosis, each thing said once: a dead board's idle
  stage says "it isn't running", and the summary why the boot's stages
  weren't taken (it said both twice). A board that passes says so in one
  line; the window keeps what the boot doesn't test (the same at every
  pass) behind **More**. The window leaves out the ROM routine's address
  in a stage's heading ("11E2") and "P-RAMT"; the terminal and
  diagnosis.txt keep them.
- tests/la-score.py says how many verdicts name the real fault in their
  first item, from the faults known: each simulated fault inside a chip,
  and the real folders listed in tests/la-faults.txt (four so far, from
  the records). The regression prints the score at the end. Now: 23 of
  26 in the first item, 2 right passes, 1 missed (IC22, which the ROM's
  own RAM test can't see). Each rule in the verdict notes which board it
  came from.

- "Diagnose by probing", on a Spectrum: **Import capture...** takes a
  logic analyser capture's lines (best idle.lac) as readings, low, high
  or pulsing; the analyser's "low" also fits floating (it reads 1.2 V
  low). `--probe-capture FILE --board B` on the command line. On the two
  dead Issue 1 captures the CPU is the only fault left; working boards
  fit the healthy board.
- The check of the verdict's parts tries what goes wrong inside a chip
  (an address line) before its pins, and gives the first item's parts
  two tries a turn: simulated IC6 addr2 is now found exactly. The
  regression has two check cases.

- The check against the captures works without a screenshot: after a
  fault, **Check the named parts' faults against the captures** (or
  `--la-check` with no `--la-screen`) puts the atlas's faults of the
  parts the verdict names on the simulated board (each part removed
  first, then its data bits, then the rest) and compares every stage.
  On simulated RAM faults it finds the right chip (IC8 data 0: exactly,
  6 of 6 stages).
- The "CPU isn't running" count says the lines are still (never
  changing, or only a few times), not that they never change.

- The build has no warnings left. Typing a path too long for the file
  browser says so, instead of opening a cut-off path. The logic
  analyser's allocations are checked: out of memory, it says so and stops.

- The regression (tests/la-diagnose.sh) replays each real capture folder
  as the board it was taken from, read from its diagnosis.txt (Issue 1,
  2, 3B, 4B, 6A, and RAM 16K), not always as an Issue 2. Four Issue 3B
  folders now say Issue 3B; nothing else changed.
- src/lacap.c (4,000 lines) is split by job into lacap.c (output,
  options, command line), lacap_file.c, lacap_record.c,
  lacap_compare.c, lacap_capture.c, lacap_rules.c, lacap_verdict.c and
  lacap_diagnose.c, sharing lacap_int.h. No change in what it does: the
  regression gives the same 73 verdicts.

- Logic analyser window, after a run: what it found comes first, under
  the stage boxes, and the run stage by stage below it. The captures'
  folder is a short name with an **Open folder** button. The stages are
  called as the boxes call them (RAM test, not ramcheck), "about 110 T"
  is "about 110 clock cycles", and "(cycle 57)" is "after 57 bus
  cycles". Skipped stage boxes are plain, without the strike-through.
  The window keeps the width you drag it to.
- While the logic analyser window is open, the Monitor shows the real
  Spectrum's screenshot (or says none was given), not the simulated
  board's healthy screen, which could be taken for the real one.

- Diagnose from screenshot: vertical bars are matched by how wide they are
  and how far apart, not where they start or which color comes first
  (both vary from board to board), and jailbars (the faint lines most
  Spectrums' video puts on the picture) no longer break them up. The
  screen of a CPU that never ran ignores the border (nothing set it: a
  real one's changes at every switch on). A real dead Issue 1's 2 white
  and 2 black bars went from "32 bars, 2 pixels wide" (63%) to the same
  bars (81%), all its causes the Z80. The analyser's verdict says what
  the screenshot shows first, then the nearest simulated screen, and
  calls a match under 80% rough. The matcher's own test is unchanged
  (60 screens: 36 first, 51 in the top 3).
- Logic analyser diagnosis: a CPU whose outputs are dead or floating
  (every address line low, /BUSACK low with /BUSRQ high) is named first,
  before the clock: a real Issue 1 read so (its address lines at 1.2 V,
  its clock there at the CPU) and was told "No clock".
- Logic analyser window: the results text is laid out as paragraphs. It
  was wrapped twice (for a terminal, then for the window), which broke
  sentences into fragments, and only an item's first line was colored.

- Logic analyser diagnosis: no more "it's in the folder from before: use
  it?" at every stage. A run always captures afresh (the folder's earlier
  captures go first). In the window it works in `last-diagnosis`, and
  **Save as** keeps a run in a folder of your choosing; its questions now
  say Continue and No (not "press Enter", "type n"). `diagnose.bat` asks
  before it replaces a board's earlier diagnosis.
- The logic analyser window is easier to follow at the bench: a row of
  stage boxes (blue now, green OK, red wrong, click for the stage's
  text); a big red ARMED with "LET GO OF RESET" while it waits for the
  Spectrum, TRIGGERED once captured; green Start and Continue, red Stop.
  The setup is hidden during a run, and **New diagnosis** clears the
  results and the outlines on the board.
- Logic analyser diagnosis: the frame interrupt's verdict names the ULA's
  pin through its resistor ("IC1 pin 12, through R26" on the Issue 1
  and 2, and R26 open as a cause). A 16K Issue 1 no longer fails the RAM
  test's end: its reads of 8000 (no RAM there, as on a working one) aren't
  a fault. Checked, simulated, on the Issue 1, 3B, 4B and 6A.
- The logic analyser window no longer outlines diode D7 when the verdict
  names a RAM chip's "bit D7" (a data bit after "bit", "bits", "line" or
  "pins" isn't a part on the board).
- README: the title is the repository's name; pictures of the analyser
  window (ARMED, TRIGGERED, a real IC13 verdict) and the outlined board.
- Logic analyser diagnosis with the screen: a screenshot of the monitor
  (Choose, Paste, drop, or **Pick a type...** from the simulated faults'
  screens when there's no capture card) is matched against the fault
  atlas with what the bus showed: whether it agrees with the bus's
  suspect, or, when the bus finds nothing, its own suspects (a bad ULA's
  video side). Given after a run it's matched at once; it's kept with the
  captures (`screen.png`). `--la-screen PICTURE` on the command line.
- **Check the screen's faults against the captures** (`--la-check`): each
  of the screen's faults put on the simulated board and every stage
  compared with what the analyser took, against a working board's. On a
  real faulty IC13, IC13's faults came first (5 of 6 stages the same, a
  working board 2). A minute or two on half the processors.
- Simulated boards can be recorded on several threads at once (the step
  hook is the board's own, not one for the program).
- Logic analyser diagnosis: working boards no longer fail the reset
  stage. /RESET rises slowly, so the analyser can see it low all through
  while the CPU runs: it now counts as held only when the CPU is still
  (before, "/RESET is low all through, the CPU never starts", and a
  needless "hold it, once more"). The reset button's bounce (the CPU
  starting again from 0000, up to 140 times) is skipped: the stage is
  compared from the last restart.
- The logic analyser window remembers which board is the master by its
  USB serial number (`la-boards.txt` beside the program), so another PC's
  COM numbers don't matter: **Find boards** shows the master and the
  slave, a **Swap** button, and whether the order is remembered or a
  guess. The wrong way round, a working Spectrum read as a dead one.
  `--la-diagnose` without `--la-ports` uses the remembered order.
- Logic analyser diagnosis: the boards the wrong way round are said so
  (CLK stopped with /WAIT and /BUSRQ both held low), not taken for a dead
  Spectrum. A /RESET that the analyser sees rise only after the CPU has
  been running no longer marks where the CPU started (it made a working
  part of the RAM test look like A8 and A12 stuck). A reset capture that
  begins just after the fetch at 0000 is lined up on its first cycle.
- Logic analyser diagnosis: a board that passes all six stages is no
  longer said to have "all of the RAM good". The ROM's own RAM test only
  writes 02, 01 and 00, so bits D2-D7 never hold a 1: a real Issue 3B
  with a faulty IC22 (upper RAM, D7) booted and passed, while ZX
  Diagnostics failed it. The verdict says so and points to a full RAM
  test.
- Logic analyser diagnosis: CLK reading stuck at the edge connector while
  the CPU runs the ROM's code now names the ULA's weak clock output (a
  real Issue 2: 1.4 V there, a new ULA fixed it), not "no clock". Left
  running, an address line that never changes while the ROM runs is a
  note, not a stuck line; the stack found in the ROM (the RAM test failed
  at its start) is named.
- Logic analyser diagnosis: the board's options (Controls: RAM 16K, chips
  fitted) are used for the working board it compares with, in the window
  and with --option on the command line. A 16K board used to fail at the
  upper RAM, compared with a 48K one. The header says "(RAM 16K)".
- Logic analyser captures of the Spectrum's edge connector (work in
  progress, before the real analyser was built): read a `.lac` or CSV file
  from the LogicAnalyzer program (`--la-info`), record the simulated board
  as the analyser would (`--la-record`), and compare a capture with the
  simulated board bus cycle by bus cycle from reset (`--la-compare`). See
  README, "Diagnosing with a logic analyser".
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
- Logic analyser in the window: **Diagnose** > **Diagnose with the logic
  analyser...** runs the diagnosis (the real Spectrum over the analyser, a
  folder of captures again, or the simulated board with a fault) with its
  questions as buttons, its text in the window, and the chips the verdict
  names outlined on the board. `--la-window` opens and starts it (tests,
  screenshots).
- Logic analyser: `tests/la-diagnose.sh` replays every diagnosis folder and
  a set of simulated faults, and shows what changed against
  `tests/la-diagnose-expected.txt` (`LA_JOBS=N` for fewer cores).
- Logic analyser, `--la-diagnose`, from two real faulty Z80s: a CPU that
  isn't running while the idle capture shows its clock, /RESET, /WAIT and
  /BUSRQ right is named itself (or its power), and /RD with /WR both low
  marks one that drives nothing; all the data lines wrong from the ROM and
  the RAM alike are one item (the CPU first). A stuck line lists every chip
  on it from the netlist (a real faulty 74LS157 held A9 high). /M1 and
  /RFSH, which nothing on a 48K uses, are only a note when stuck (a Z80
  with a dead /M1 runs a 48K), and the bus is decoded without a line of
  them that hardly changes. A line that changes a few times where a working
  one's changes thousands counts as stuck.
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
