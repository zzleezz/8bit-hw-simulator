# To do: making the simulator a better repair tool

Ideas for making this more useful when repairing real broken C64s and ZX
Spectrums, roughly in order of value. Tick items off as they're done.

## 1. Guided diagnosis from real probe readings (done)
- [x] Enter readings taken on the real board (e.g. "U7.3 pulsing",
      "U12.5 floating") and narrow the list of candidate faults with each one.
      ("Diagnose by probing..." in the control panel.)
- [x] Suggest the next pin to probe: the one that best splits the remaining
      suspects.
- [x] Build on the fault atlas: it now also records what a logic probe shows
      on every net under every fault (5-6 s after power on).
- [x] "The screen shows" choice, so faults a probe can't see are still found.
- [ ] Follow-up: probing a chip's leg as well as its pad. Lifted or open legs
      look the same as a healthy board at the pad (a whole group of 75 such
      faults on the C64), but differ on the chip side.
- [x] Follow-up: pre-select the screen from a screenshot diagnosis, when one
      has been done (2026-10-28: "Diagnose by probing..." in the control
      panel takes a screenshot diagnosed since it last took one; "Narrow
      down by probing..." and an open probing window already did).

## 2. A reference sheet for a known-good board
- [ ] Record every IC pin's healthy state (high, low or pulsing, plus roughly
      how fast) for both machines. (The atlas now has the healthy board's
      readings: the probing window already shows them per pin.)
- [ ] Browse it in the app, next to the pin under the mouse.
- [ ] Export it as a sheet you can print.

## 3. The faults that actually kill these machines (done, except timing)
Built and tested 2026-10-26: the 31 old fault pictures are identical;
tests/mem-faults.sh results checked by eye. Atlas updated the same night
(boot mode only, 813 new faults in 27 min, 0 failed: 3578 faults, 736
outcomes). Screenshot diagnosis puts the true fault first for 6 of 7
test pictures (a one-pixel char ROM fault ranks 4th). Probe test with
10% wrong readings: in the running 96-98%, first six 68% blind / 86%
with the screen. --match-test 12: #1 for 7 (was 10), top 3 for 12: new
near-healthy screens (a few garbage cells) are close neighbours.
- [x] RAM with dead bits: a dead 4164 / 4116 (data0=0/1), dead address
      lines inside a chip, one bad cell (flip, flip@CPU address), weak
      cells. Also the 2114 color RAM.
- [x] Partly dead chips: any output can die inside the chip, stuck low or
      high (out0/out1): unlike a short it can be fought on a bus. A PLA
      with some outputs dead is the same fault on its output pins.
- [x] Corrupted ROM bytes, dead ROM data bits and address lines.
- [ ] Weak or slow outputs that look right on a probe but are wrong in timing
      (marginal VIC / PLA timing: sparkles, garbage characters). Not
      started: the simulator has no propagation delays, so this needs a
      new delay model. Least certain; can be dropped.

## 4. Training mode, made into a game (to expand on later)
- [ ] Make it a game that appeals to fixers: details to be worked out together.
- [x] Groundwork: "Hide fault markers (practice)" in the Faults section hides
      the badges, the marked pins, the inspector's fault text and the list.
- [ ] Put a hidden random fault on the board, with only the tools you'd have
      on the bench.
- [ ] Score each repair on how many probes and chip swaps it took.
- [ ] Difficulty levels, from a missing chip up to a single bad RAM bit.

## 5. Smaller wins
- [ ] Multimeter mode with real voltages on the power section. This would also
      fix the C64's +5V pads showing high while it's switched off.
- [x] Save states to disk, not just the one in-memory slot (2026-10-28:
      "Save to file..." / "Load file...", .hwstate files, --load-state,
      --at N:save:FILE). The file is the save state's memory plus the same
      board made fresh; loading takes pointers from a board made again the
      same way, so files survive restarts and rebuilds (refused only if
      the machine's layout changed). The cartridge goes in the file. Tested:
      10 cases saved in one process, loaded in another (and in the next
      build), same picture and registers.
- [x] A repair-notes panel, kept with the saved state (a text box under
      the save buttons; in the file).
- [x] Make save files safe to open from other people (found in the
      2026-10-28 code review; fixed the same day). A doctored .hwstate
      could point a part's eval/commit, or a chip core's callback,
      anywhere and run code when the board steps. Now (sf_relocate() in
      board.c) a word pointing into the program on the new board always
      comes from the new board, and a file whose state holds an address
      inside the program is refused; the parts' read caches (whose keys
      point at pin tables) are cleared first. Measured on real files of
      every board: no program pointer changes while a machine runs, no
      state word holds a program address. Tested: the 10 cases, files of
      earlier builds, and doctored files (a function pointer moved, an
      empty word aimed into the program: refused; a state word changed:
      loads).
- [ ] Left of it: a file's state words are trusted as values, and some are
      indices (a pin, a part, a cell) used without a bounds check: a
      doctored one could read or write outside an array. Harder to use
      than a function pointer (no address to aim at), but not ruled out.
      Fix: check the indices a loaded machine uses, or sign files saved
      on this PC and warn before loading one that isn't.
- [ ] More board revisions: other C64 boards (250407, 250425, 250466, C64C)
      and Spectrum issues 2–6. This is a lot of work per board.
      C64 KiCad sources found on GitHub (2026-10-29):
      - bwack/C64C-250469-KiCAD-Replica: the C64C short board, traced
        from real PCBs, full layout; CERN OHL 1.2 (as the 250407). The
        best next C64 board. Done 2026-10-29, see below.
      - bwack/KU-14194HB-RevB-KiCad: the rare KU board (the 250407 is
        drawn from it); KiCad 5; no licence, only a disclaimer: ask bwack.
      - boelle/326298-Rev-A-Kicad: the first C64 board (1982); KiCad 5;
        CC BY-NC-ND (no derivatives): ask boelle before using it.
      - NeedfulThings/Commodore64C: 250469 Rev A and B redrawn from the
        schematics, with substitute parts; CERN OHL 2 strongly reciprocal.
        A cross-check for the 250469, not a layout of the real board.
      - Nothing for the 250425 or the 250466.
      KiCad projects for Spectrum issues 1, 2, 4B and 6A are in
      ../Schematics (CC0; the 4B folder has no LICENSE file: check).
      Order: Issue 2 -> Issue 1 -> 4B -> 6A (4B and 6A share the bare PCB;
      6A adds the ZX8401).
  - [x] Issue 2 (2026-10-27): same circuit as the 3B (netlists compared
        pin by pin). Board picker and options in the controls (--board,
        --option): RAM 48K/16K (also on the 3B), ULA 5C112/6C001 (labels
        and R47/R49 only). TR6 and C74 added by hand (no place on the PCB).
  - [x] Issue 2 atlas run (2026-10-28, with the Issues 1 and 4B:
        3876 runs in 2.5 h on 28 threads, 0 failed). Issue 2: 1442 faults,
        430 outcomes (the 3B: 439). Matching, probing and the atlas
        browser now use each board's own data.
  - [x] Upper RAM chips and type links as options (2026-10-28): TI
        4532-3/-4 (split by the row A7), OKI 3732-L/-H (by the column
        A7), 4164; the TI/OKI links on the 3B and 4B, LNK1 on the Issue 2.
        Picking chips sets the links; other links after give a half bad
        upper RAM (all 50 chip/link pairs boot with the P_RAMT the wiring
        predicts: 48K, 32K or 16K). Defaults unchanged (TI-3), so the
        atlas stands. Spectrum save files of earlier builds are refused
        (zx48_t grew); C64 ones still load.
  - [x] The ROM link (2026-10-28; JROM1, LNK2 on the Issues 1 and 2):
        "rom" (NEC uPD23128, Hitachi HN613128) and "romlink" (N, H)
        options. From the datasheets (Schematics/): NEC pin 20 a clocked
        /CE (address latched on its fall), 27 CS; Hitachi static, 20 CS,
        27 OE1. A NEC on H is dead (one byte over and over); a Hitachi
        works on either. Default NEC on N; the Issue 1's default link was
        H wiring before (its LNK2 legs differ), now N. 16 boots pass, the
        state and snapshot tests pass.
  - [x] The Spectrum atlas for the NEC model (2026-10-28): against the
        old build, IC5.20 low/open, IC2.19 (/MREQ) out0/open and, on the
        Issue 1, IC5.20 and IC5.27 faults gave other pictures. Only those
        run again, with the new C64HW_ATLAS_REDO ("IC5 JROM1 LNK2 IC5.20
        IC5.27": 520 runs on the 4 boards, 19:42 on 28 threads, not a
        2.5 h rebuild); the loader keeps a fault's newest run (backup
        fault_atlas.bin.bak-20261028). 73 faults changed picture, 49 of
        them open ROM pins (noise, now read only on /CE edges). The
        Issue 1's link change swapped its catalog's IC5.20:gnd/vcc for
        IC5.27:gnd/vcc: the two old entries were cut out of the file.
  - [x] Score the matcher again on the updated atlas: `--match-test 60`
        (seed 31; last 37/60 #1, 48/60 top 3). Delete the old captures
        folder first: the outcomes changed, so the pick list may have, and
        captures are reused by file name. A few minutes.
  - [x] Issue 1 (2026-10-27): a 16K board (the Issue 2's circuit without
        the upper RAM). RP1 keyboard pull-ups, D14 by R24, no TR6.
        ULA 5C102/5C112/6C001; the 5C ULAs pull their clock up
        themselves, a 6C001 comes with R73 (the Issue 1 has none).
  - [ ] Issue 1 32K RAM board (EXT1/EXT2) as an option: the user has its
        KiCad files. Probably the Issue 2's upper RAM circuit (it needs
        only EXT1/EXT2 signals), drawn as a second board.
  - [ ] Issue 1: D14 does nothing in the model (R24 is a link); with R24
        open a real board still clocks through D14.
  - [x] Issue 1 atlas run (2026-10-28): 969 faults, 377 outcomes.
  - [x] Issue 4B (2026-10-27): the 3B's logic; IC24's spare gates delay
        /RAS for the lower RAM mux (R32 is 0R: it keeps the 3B's one step
        delay, standing for the gates). DC-DC by parts (zx_variant_t):
        -5V C46 D11 D18 C79 R79 D19, +12V pump C80 D17 D15, 9 V through
        L1 L2. 16K keeps IC24.
  - [x] Issue 4B: the folder has no LICENSE file. Ask the author / check
        before publishing the board data. (2026-10-29: the GitHub
        repository liveboxandy/ZX-Spectrum-48K-Issue-4B has one now, CC0.)
  - [x] Issue 4B atlas run (2026-10-28): 1465 faults, 437 outcomes.
  - [x] Issue 6A (2026-10-28): its own layout (not the 4B's PCB), the
        4B's circuit with the ZX8401 (IC27) and a 74LS04 (IC28) in place
        of IC3, IC4, IC23-IC26. The ZX8401 is simulated as the 74LS parts
        a drop-in replacement is made of (trastero.speccy.org/cosas/JL/
        pcf1306p/, Documentos/placa-pdf.zip, esquema.png; the .sch/.brd
        are old binary EAGLE), checked against the 6A netlist: four
        LS157 sections (lower: DMA0-6 by DRASLL; upper: MA0-7, MA7 from
        TS1/TS2) and the 4B's IC23/IC24 gates for the upper select and
        /CAS (pin 34, the only output but the muxes; 39 is /UWRITE in,
        from IC28). /CAS a step after the select (C105). R87 and R88 are
        the delays (as the 4B's R32 and R71). J1 (the RAS/CAS link) is
        LK5, fitted 1-2 3-5 4-6; J2/J3 are LKV/LKU; LK1-4 open. No R57.
        The schematic's RFSH/RFSHL labels joined. Checked: boots, RAM
        test under every chip/link option and 16K, the diagnostics ROM
        (healthy, upper and lower RAM chips out), glue faults against the
        4B's, a .sna loaded (the same picture as the 4B), save states.
        Also seen: github.com/pmandes/zx-spectrum-issue6a-replica (BOM,
        layout PDF, CC BY-NC-ND: nothing to model from).
  - [x] Issue 6A atlas run (2026-10-28): 1431 faults in 61 min on 28
        threads, 426 outcomes. --match-test 60 seed 31 (six boards now):
        34/60 #1, 51/60 top 3; the 6A's 10 screens 5 #1, 8 top 3.
        --probe-test, six boards (120 faults): listed first 76-78%, best
        fits 94-95% (screen known: 90-95%, 96-97%), as with five boards.
        tests/zx-faults.sh and zx-diag.sh have 6A cases (i6a_*), all as
        expected. Matcher: no "signal is the active display" reading when
        the content sits framed by a margin all round (a border to read):
        a dark capture of bars no longer passes for a blank screen by the
        unknown-border half credit (#45: 26th -> 4th, nothing else moved).
  - [x] C64 ASSY 250469 Rev. 4 (2026-10-29): bwack's replica (KiCad 7;
        kicad2c.py skips the copper zones, reads fp_text references, the
        {slash} names). --board 250469 / --machine c64c, and the board
        drop-down in the controls. New parts: 251715-01 Super PLA (64
        pins: PIN_NUMBERS 48 -> 65), 41464, 27128 (BASIC + KERNAL),
        74LS14, 8701; 8500/8565/8580 on the same cores. c64.c takes the
        refs from c64_variant_t. Boots to READY (38911 free), self check
        0 errors, Dead Test and the 586220 (with the harness) run. The
        250407 is unchanged (10 cases, pictures identical to the old exe).
        The assumptions are listed in the README ("The 250469 board"):
        Super PLA = the 906114 equations + 74LS139/257/258/373 + a 74LS08
        gate, RAM_R/W = R/W held high in VIC cycles, NMI = RESTORE (no
        pulse), 8701 pins other than 6/7/8/13/14 unknown, C43 holds reset
        50 ms, R17/C59 debounce and the EM filters' capacitors not
        simulated.
  - [x] 250469 interface (2026-10-29, checked by the user): the board
        drop-down, save and load state, the atlas browser. The inspector
        stopped at pin 47 (inspect.c, ui.c's pin labels and board.c had
        48 pin arrays left): now PIN_NUMBERS.
  - [x] Before its atlas (2026-10-29): 24 named scenarios of its own in
        atlas.c (U8, U10, U20, C43...; "250469 scenario" in the causes;
        the expected results are a repairer's guesses, written before the
        runs) and 8 bench steps in probe_db.c (U6.40, U6.1, U8.22...).
        The catalog: 1516 faults (C64HW_ATLAS_TODO=1 lists them; the six
        other boards: 0 to run, the atlas stands). Without its readings,
        the probing guide no longer borrows the 250407's (they named
        other parts: "probe U7.1, the CPU clock" is the 250469's VIC), and
        the screenshot diagnosis and the atlas browser show the 250407's
        causes without "Apply & Inspect" (atlas_same_refs()).
  - [x] U4 images on the 250469 (2026-10-29): an 8K KERNAL goes in the
        27128's high half with stock BASIC below; a 16K BASIC + KERNAL
        image (new: raw_is_basic_kernal()) goes in whole, and on the
        250407 its KERNAL half. Tested with patched images on both boards.
  - [x] The PLA inspector on the Super PLA (2026-10-29): the same table on
        U8's pins (b->pla_super, superpla_in_pin/out_pin), /AEC shown as
        its AEC pin, /IO and GR/W as inside outputs.
  - [x] Y1:removed ignored on both C64 boards (2026-10-29): now stops the
        crystal like Y1:open. The atlas has no Y1:removed fault, so no
        outcome changed.
  - [ ] 250469 left to do:
    - [ ] Its fault atlas (--build-atlas: ask first, it is hours).
    - [x] tests/faults.sh and diag.sh (2026-10-29): MACHINE=c64c runs the
          250469's cases (25 faults; Dead Test with U10, U11 and U8 out,
          the 586220 with U18 and R16 for the 250407's U28 and R100).
          SERIAL=1 runs one case at a time. faults.sh on the 250469: all
          as expected except A15 open (garbage characters, not black) and
          the color RAM out (one color, not flickering), the same on the
          250407. diag.sh on the 250469 (16 checks pass): Dead Test with a
          41464 out flashes once (any multi-bit error does, on both
          boards), so single lifted legs check the chart; the 586220's
          CONTROL PORT is BAD on a healthy board (2200 pF on C80/C81 in
          the replica: the harness paddles read ~131, not ~107), so that
          check is left out there.
    - [ ] C80/C81 on a real 250469: 2200 pF as in bwack's replica, or
          1800 pF as on the 250407? Read the capacitors on a board or a
          Commodore schematic; if 1800 pF, correct the board data and the
          586220's control port check works on the 250469 too.
    - [ ] If a read-out of the 251715 turns up, check the assumptions.

## 6. Atlas browser
Look through the atlas by what the screen shows, rather than by matching a
screenshot. Estimate: medium, about 1-2 sessions (~800-1200 lines). Needs no
atlas rebuild: everything it filters on is already in fault_atlas.bin.
Done: "Browse the atlas..." (src/atlas_browse.c), probe readings as a
filter too (2026-10-28).
- [x] Pick a characteristic to start from, per machine: border color,
      screen color (from the fingerprint's border/active histograms), kind
      of screen ("Vertical bars", "Garbled characters"...: the probing
      window's kinds), how it behaves (still, blinking, animated, no
      signal), CPU state.
- [x] Show every outcome that has it: a grid of thumbnails, each with its
      causes (fault count, chips involved).
- [x] Add more characteristics to narrow it down, one at a time, with how
      many outcomes each choice leaves (like the probing window's
      drop-downs). Also probe readings (pin + what the probe shows: a
      picture stays if one of its causes shows them all) and "involves
      chip X".
- [x] From any outcome: put its fault on the board, or open "Diagnose by
      probing" narrowed to it.

## 7. Real repair records, added to the atlas
Repairs done on real machines (a screenshot plus the chips that were found
faulty), kept apart from the simulated atlas, used by the diagnosis when
switched on, and shareable. Estimate: large (see the end of this item).
Best done after item 6, which can then show them too.
- Step 1 done (2026-10-28): src/repairs.c, repairs.bin next to the
  program (--repairs FILE), "Record the repair..." in the diagnose window
  (a small window; clicks on the board pick the chips, outlined in
  orange), "Seen on real boards" above the simulated matches (75% and
  up, best 3), a switch to leave them out. --add-repair PICTURE --chips
  IC1,IC13 and --diagnose-text PICTURE do the same without the window.
  Real captures of the same fault can look different (Test images/u7_2
  and u7_6: 68%), so a record mostly finds the same screen again.
- Step 2 done (2026-10-28): the "Repair records" window (also in the
  control panel): every record with its picture; leave one out (SETR
  chunk, this PC only) or a whole dataset; edit (how sure, notes) and
  delete one's own (a new version, flagged deleted, so merges take it
  back); merge a file, export one's own or all (.hwrepairs); record one
  without a screenshot; statistics of the chips found faulty. The
  probing window's ranking: a chip on real records x(1 + 0.25 per record,
  at most +2), x(1 + 3) more if in a record the last screenshot matched
  (diagnose_record_boost()). ATLAS_READ_VERSION: records read by an
  older matcher are read again from the stored screenshot when loaded,
  kept in a NORM chunk (local). CLI: --list-repairs, --merge-repairs,
  --export-repairs [--all], --reread-repairs.
- [x] A record: machine (board id), the screenshot, the faulty chips
      (picked by clicking them on the board, each with what it is there),
      how sure, notes, who (the dataset's owner name) and when. A unique
      ID (dataset ID + number) and a version, so it can be merged.
      Still to do: fault kind, probe readings, chip markings.
- [x] Kept in its own file (not fault_atlas.bin, which gets rebuilt). The
      screenshot itself is stored, not only its fingerprint, and read
      again when the matching changes.
- [x] Switch each record (or a whole dataset) on or off for diagnosis.
- [x] In the diagnosis: a screenshot that matches a real record lists it
      beside the simulated matches; the chips that really fail weigh the
      probing window's ranking; a list of the records to look at, edit or
      delete one.
- [x] Save, open and share datasets. Merge someone else's into yours
      without overwriting: records already there (same ID) are skipped,
      newer versions replace older ones, the rest added; each keeps its
      dataset (whose it is).
- [ ] Bonus: score the screenshot matcher against the real records (a real
      --match-test), to see how well it does on real captures.
- [ ] What was wrong with the chip is optional: many repairs are just "swapped
      it and it worked". Three levels of detail:
      1. chip only (the default);
      2. roughly what it did, from a short list for that chip type (dead,
         one output stuck, bad RAM bit N, corrupt ROM...);
      3. the exact fault as the atlas names it: once the chip and the
         screenshot are given, list that chip's simulated faults that also
         match the screenshot, to confirm one of.
- [ ] How sure: tested bad in a chip tester / swapping it fixed it /
      replaced several, not sure which (done, per record). Still to do,
      per chip: replaced, or
      confirmed bad (eight RAMs swapped at once say much less than one).
- [x] Each dataset has its owner's display name (changeable) and a random
      ID made when it's created (never changes: two people can both be
      "Dave"). Shown beside every record in the lists, so you can see where
      a result came from. Record IDs are the dataset ID plus a number.
- [x] Records have a version: a merge that meets a newer version of a
      record from the same dataset updates it (without asking).
      (No signing: a name is a label, not proof.)
- [ ] Board revisions: store what each chip is (part type and role, "4164
      RAM, bit 3", "PLA") as well as its number, since U numbers differ
      between boards. Records from other revisions then still count, and
      map across when more boards are added (item 5).
- [ ] Chip markings: maker and date code (MOS, MT, TI...), optional. Real
      failure counts per maker can replace the guessed weights in the
      probing ranking.
- [ ] Records without a screenshot (done: chips only, they count in the
      statistics and the probing ranking). Still to do: describe the symptom with the atlas
      browser's characteristics (yellow border, vertical bars, blinking),
      so a record's symptom and a browser search are the same thing.
- [ ] Phone photos of a CRT: record what the picture is (capture card /
      photo) and trust photos less in matching. (Correcting a photo's angle
      would be a project of its own.)
- [x] Failure statistics: which chips fail most, per machine (the records
      window), and they feed the probing ranking. Still to do: with which
      symptoms.
- [ ] Gaps in the simulator: confirmed repairs that match nothing in the
      atlas, listed as faults the catalog is missing.
- [x] File format: screenshots inside, in the atlas's chunk format (one
      file holds several datasets; export writes one's own) (the code exists, and survives a damaged file).
- [ ] Later: a shared place for datasets (a GitHub repo or forum thread);
      the merge makes it work with no server.
- Estimate with all of the above: about 5 sessions (~2500-3000 lines).

## 8. Code review findings left (2026-10-28)
The review of all of src/ (four parts) found these; the rest are fixed.
- [x] Save files safe to open from other people: see item 5 (what's left
      of it is there too).
- [x] The continuity test (shift-click) follows copper and passives only:
      a solder bridge (U13.11-12:short) or a pin shorted to GND/+5V doesn't
      beep, and adding a bridge doesn't re-run the trace (ui.c,
      cont_signature has no bridges). Fixed 2026-10-28: both are hops of
      the trace ("Bridge U13.11-12 [VA4]: ...", "U13.11 shorted to GND",
      and from GND "Shorted at U13.11 [A15]: ..."); bridges are in the
      signature.
- [x] atlas_match.c: seen_pair is a uint8_t counter, so a paper/ink pair
      on more than 256 cells counts again (at 256, 512): a garbled screen
      with ~10 pairs can be labelled "Attribute garbage". Changes labels
      and scores: fix, then run --match-test (60, seed 31: 37/48 now) and
      --probe-test before keeping it. Fixed 2026-10-29 (a bool): the
      match test unchanged (34/51, no rank moved).
- [x] --match-test keeps the first 512 outcomes per board in hash order
      before sorting by causes: with more, "most causes first" is wrong.
      Changes which screens the test picks (the numbers aren't comparable
      across the fix). Fixed 2026-10-29: the lists hold every outcome.
      No board had over 437, so the picks didn't change.
- [x] atlas.c: a fault counts as done if it has any PINS, so after a board
      data change (a new NETS) rebuilding never refreshes stale readings
      (probe_db_load drops them: the probing window asks for a rebuild that
      does nothing). Run faults again whose PINS don't match the last NETS.
      Fixed 2026-10-29: PINS of another net count than the board has now
      are run again, for their PINS only (the same test probe_db_load
      drops them by). The current atlas: nothing stale.
- [ ] Speed, none urgent: draw_pins asks board_net_simulated for every
      passive pad every frame (a flag per net, made once); sim_rebuild
      reallocs each net's lists one pin at a time (count, then fill);
      swapping a cartridge cold-starts twice; the paddles look their pins
      up by name on every POT read; c64.c cia_outs is never used;
      --atlas-diff compares every fault with every other (hash them).

## 9. Publishing on GitHub (prepared 2026-10-29)
Everything is ready in ../github-upload (antirez's repository with this
version staged on top, source only) and ../github-release (the Windows
zip, fault_atlas.bin, HOW-TO-UPLOAD.md with the steps and texts,
commit-message.txt).
- [x] Ask Henry whether the light theme can be published (2026-10-29: Henry
      gives full permission).
- [ ] Make a GitHub account; Settings → Emails → keep the address private,
      and commit with the noreply address it gives.
- [ ] Fork antirez/c64-hw-simulator; commit and push from ../github-upload
      (the commands are in HOW-TO-UPLOAD.md).
- [ ] Make a release: attach c64-hw-sim-windows.zip and fault_atlas.bin.
- [ ] Open an issue on antirez's repository with the drafted message.
- [ ] Afterwards: work changed here has to be copied to ../github-upload
      again before a new commit (or move the work into that folder).

## Left over from earlier work
- [x] Rebuild `fault_atlas.bin` (`--build-atlas`): it was out of date for the
      power-path faults (F1, L4, L5 said "CPU running"). Rebuilt with item 1.

## Dropped
- Intermittent faults (bad sockets, faults that appear once the board is warm):
  too vague to model well.

## Notes
- Check with the user before anything that uses every core (atlas runs);
  tests run 4 at a time (JOBS=4 tests/mem-faults.sh).
