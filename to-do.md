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
- [x] Follow-up: probing a chip's leg as well as its pad. Lifted or open legs
      look the same as a healthy board at the pad (a whole group of 75 such
      faults on the C64), but differ on the chip side.
      Code done 2026-10-29: LEGS chunks in the atlas (what the probe shows
      on each open chip leg, over the PINS frames; read with pin_stats on),
      "Probing on: chips' legs / solder side" in the probing window (legs
      the default), suggestions can ask for one leg, --probe-test reads
      legs (C64HW_PROBE_PADS=1: pads). Checked: deterministic (two runs of
      16 chunks identical); pads mode with no LEGS gives --probe-test 5
      seed 7 identical to V1.0 (17 min).
  - [x] Atlas run 2026-10-29/30 (backup: fault_atlas.bin.bak-20261029-
        before-legs): 2650 runs in 1:45 on 28 threads (not ~70 min), 0
        failed, 2646 LEGS chunks. --atlas-diff: 10377 outcomes the same,
        0 changed, 4 new, 21 cut out (C64HW_ATLAS_PRUNE, see 5.).
  - [x] STALE SCREENS IN THE ATLAS (found 2026-10-30, also in V1.0's;
        the open legs and all the bridges of the four older Spectrums
        refreshed. What's left is the item after this one).
        The 295 faults below, run again whole into a copy
        (C64HW_ATLAS_REDO_FAULTS=list, new: "zx48 IC23.8:open" per line;
        12 min): 274 of their stored screens change (zx48 66, zx48i1 77,
        zx48i2 64, zx48i4b 67). Repeatable (IC23 twice: identical). V1.0's
        own runner gives the new screens too (IC15 run again with the V1.0
        exe), so the old ones are from an older build, not from tonight.
        Swapped in 2026-10-30 at the user's word (backup:
        fault_atlas.bin.bak-20261030-before-refresh; --atlas-diff: 274
        changed, nothing else). Faults other than ":open" on
        these four boards may be stale as well: only the open ones were
        re-run. A full re-run of the four is ~4500 runs, ~3.5 h.
        Sample check 2026-10-30: 200 random non-open faults of the four
        (seed 30), run again into a copy (8:20): 4 changed (2%), all solder
        bridges (zx48i1 IC3.3-4, IC1.3-4; zx48i4b IC4.12-13, IC4.1-2).
        Bridges between two outputs oscillate: maybe stale, maybe touchy.
        Checked: the 4 run again twice more, all three runs identical, so
        stale, not touchy. All 1716 bridges of the four boards run again
        into a copy (1:15, scratchpad night/fault_atlas.shorts.bin): 42
        changed (zx48 10, zx48i1 9, zx48i2 10, zx48i4b 13), nearly all on
        IC3 and IC4. Swapped in 2026-10-30 at the user's word (backup:
        fault_atlas.bin.bak-20261030-before-shorts; --atlas-diff: 42
        changed, nothing else). Not sampled yet: the C64s and the 6A (the
        6A's PINS all matched in the LEGS run; a 200-fault sample of each,
        ~8 min, would tell).
  - [x] Stale check left: a 200-fault sample of the 250407, the 250469
        and the Issue 6A (~8 min each, all cores), as above.
        Done 2026-10-31: 200 random faults of each (seed 30, any kind),
        run again into a copy (23 min): 0 of 600 changed. Not stale.
  - [x] "The atlas runner and the simulator disagree" (2026-10-30): no,
        a misreading. --atlas A --atlas-diff B prints A's outcome, then
        "-> " B's: the old screens were the wrong ones. zx48 IC15.7:open
        was stored "Blank black screen, white border"; the refreshed run,
        V1.0's runner and --headless all say it boots (the ROM's RAM test
        finds 16K, as on a real Spectrum with dead upper RAM; IC15:removed
        is the same). So the refresh swapped in fixed wrong screens.
  - [x] 295 of the re-run faults got PINS different from the old ones
        (settled by the refresh: those faults were run again whole and
        their new PINS are what loads):
        all ":open", all Spectrum (zx48 71, zx48i2 70, zx48i1 85, zx48i4b
        69; none on the 6A or the C64s). Spread over many nets, each in
        only some faults (low/high/pulsing swapped), so not tonight's
        LM1889 pin change. Likely old PINS from an older Spectrum build
        (IC23.8 was the first seen). The loader takes the last chunk, so
        the new ones count. To confirm: re-run a few into a copy and
        compare.
  - [x] Legs vs pads, --probe-test 5 --seed 7 (35 faults), probing only,
        legs / pads: bench #1 54% / 57%, likeliest first 60% / 68%,
        fewest probes 62% / 62%; "in the running" 100% / 97%. Screen
        known: about the same (80-88% both). One fault is ~3%: too few to
        tell.
        --probe-test 20 --seed 11 (140 faults, 1:07 legs, 1:16 pads),
        probing only, legs / pads: bench #1 62% / 60%, likeliest first
        67% / 67%, fewest probes 65% / 62%; faults fitting best 176 / 193
        (bench). Screen known: the same within 1%. A small gain: only 24
        of the 140 are ":open" faults. Of those, legs did better on
        U9.10 (missed on pads), IC12.15 (#121 -> #1, fewest probes),
        IC22.3 (#9 -> #1, bench) and a few places on most; worse on
        IC1.38 (bench #1 -> #3).
  - [-] (Dropped 2026-10-30, see Dropped.) Most open legs still end far down after 25 probes (U19.32 #321,
        U9.10 #204, R68 #213, IC27.3 #165, IC10.12 #124...). The probe
        rarely gets to the one leg that tells: suggest the leg of a pin
        whose pad reads "floating input" or "healthy" sooner, and look
        at why so few faults are named with 25 probes.
        Looked at 2026-10-30: probe_suggest() takes the probe that splits
        the faults in the running best (entropy). An open leg on a net
        that is otherwise fine shows only on that leg: probing it tells
        one fault from a few hundred, so it never wins, and 25 probes run
        out first. Not a tweak; ideas for the user to choose from: "check
        every leg of chip X" as one step once the readings point at a
        chip (as a technician does with a magnifier or a continuity
        test), or a final "legs to check" list of the open-leg faults
        still in the running.
- [x] Follow-up: pre-select the screen from a screenshot diagnosis, when one
      has been done (2026-10-28: "Diagnose by probing..." in the control
      panel takes a screenshot diagnosed since it last took one; "Narrow
      down by probing..." and an open probing window already did).

## 2. A reference sheet for a known-good board
- [x] Record every IC pin's healthy state (high, low or pulsing, plus roughly
      how fast) for both machines. (The atlas now has the healthy board's
      readings: the probing window already shows them per pin.)
      Done 2026-10-31 (healthy.c): measured on demand, frames 250-300 as
      the atlas's PINS, rising edges and share high over the last 10
      frames (board_edges_start(): every net at every bus step, one board
      at a time, outside board_t so save files keep their layout). 15-23 s
      a board; the simulator ~1% slower for the extra compare per step.
      Analog pins (the ULA's OSC floats, U/V/Y read LOW) show the model,
      not a probe: said on the page.
- [x] Browse it in the app, next to the pin under the mouse (the
      tooltip: "healthy board: ...", measured in the background the
      first time).
- [x] Export it as a sheet you can print ("Healthy board sheet": an HTML
      page opened in the browser; --healthy-sheet DIR for every board).

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
- [x] Pin directions (2026-10-29): arrows in the chip view; U31's 74LS629,
      U20's 556 THR/CV, the 8701, the LM1889 and NC pins were marked
      outputs. The atlas build of 2026-10-29 ran with C64HW_ATLAS_PRUNE=1:
      21 c64 entries cut out (their out0/out1, U31.15 OSCVCC now a power
      pin, and gnd/vcc/out faults now named after another driver).
- [x] Paste a screenshot to diagnose: "Paste screenshot" button and Ctrl+V
      (2026-10-29).
- [x] Unfitted parts said "an option in the controls" even when no option
      fits them (a board's links that are simply open): each now says why
      (2026-10-29).
- [x] The pin menu was as wide as its title only: "Output dead inside the
      chip, stuck low/high" and long bridge choices were cut off. Now as
      wide as its longest choice (2026-10-29).
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
- [x] Left of it: a file's state words are trusted as values, and some are
      indices (a pin, a part, a cell) used without a bounds check: a
      doctored one could read or write outside an array. Harder to use
      than a function pointer (no address to aim at), but not ruled out.
      Fix: check the indices a loaded machine uses, or sign files saved
      on this PC and warn before loading one that isn't.
      Done 2026-10-31, signing: HMAC-SHA-256 (checked against Python's)
      appended to every file saved, key in %APPDATA%/c64-hw-sim/save.key
      (made on the first save). A file without it asks "Load it anyway"
      (the window LoadUnsigned); --load-state warns and loads. The index
      checks themselves aren't done: a trusted file is still trusted.
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
      Next Spectrum boards, Issues 4A and 4S (2026-10-29): LiveboxAndy will
      draw KiCad projects for them once HappyLittleDiodes (Jim) can supply
      PCBs. Per LiveboxAndy, the 4A's layout is not close to the 4B's, nor
      the 4S's to the 3B's: each needs its own board, not a variant. Wait
      for his projects.
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
        LiveboxAndy (2026-10-29, from checking its PCB against the
        schematic): its A0-A14 go to the 74LS157s in another order than
        the Issue 2's 32K, electrically still good. So model it from its
        own KiCad netlist, not as a copy of the Issue 2's: which CPU
        address bit reaches which RAM address pin decides what an
        address-line or mux fault looks like, and where a bad cell
        (flip@CPU address, machine_t.ram_cell) sits.
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
    - [x] Its fault atlas (2026-10-29): 1516 runs in 39:12 on 28 threads
          (idle priority), 188 outcomes, 0 failed; the atlas: 10398 runs,
          2591 outcomes, seven boards. Backup fault_atlas.bin.bak-20261029.
          A 250469 capture of the 4066 pulled matches U21 alone, 99.8%.
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
      First look 2026-10-30, by hand (`--match --top 400 --causes all
      --machine zx48i2`): only one record yet, so the user's labelled
      Test images, all Issue 2. Bad ULA (ZXULAbad 1-3, bad-ula.png): the
      ULA is in group #1 or #2 every time, but only as "ULA (inside)" or a
      ULA pin, cause 2-10 of the group (10 of 15 is behind "show all").
      Not held out: the matcher was tuned on ZXULAbad.jpg and bad-ula.png.
      Dead Z80 (ZX-CPU-bad 1-3): not found; IC2 shows only as unrelated
      pin faults (#8, #7, and #1 at 67% by chance), IC2:removed #288,
      #293, #33 (see item 11). The C64 u*.jpg images: cause unknown, left
      out. Ideas: order causes in a group by likelihood with the ULA high
      (it fails a lot); a real --match-test over repairs.bin once it holds
      more records.
      UNDONE 2026-10-30 at the user's word: the Spectrum weights (TR4,
      lower RAM, upper RAM, then CPU/ULA/ROM alike) are the user's, from
      repair experience (CHANGELOG "Unreleased"); 4 test pictures, 2 of
      them tuned on, don't overrule that. ULA_WEIGHT is 5 again. What
      raising it did, for the record:
      Tried 2026-10-30: the matcher's ULA_WEIGHT 5 -> 10 (a lower RAM
      chip's; atlas_match.c only, probing still has the ULA at 5). The ULA
      now comes right after the lower RAM chips' causes and before
      everything else: ZXULAbad 10th -> 7th of 15 (still behind "show
      all", 6 shown), ZXULAbad2 2nd -> 1st, the others unchanged (3rd,
      2nd). Groups didn't move. Left: ties with the lower RAM (weight 10)
      keep the ULA last among them; "ULA first on a tie" would make it
      1st in three of four, but these pictures aren't held out. Also: a
      net shorted to +5V/GND gets its driver's full weight (IC7.2:vcc as
      likely as IC7 dead), too high for a short on a trace.
      Tried 2026-10-30 and UNDONE at the user's word: REF.PIN:gnd/vcc at
      half net_weight() (with the ULA at 10: ULA cause 5/15, 1/7, 2/6,
      2/2, top 9 groups unchanged). A pin stuck at a supply also stands
      for a chip shorted inside on that pin, a common failure.
- [ ] What was wrong with the chip is optional: many repairs are just "swapped
      it and it worked". Three levels of detail:
      1. chip only (the default);
      2. roughly what it did, from a short list for that chip type (dead,
         one output stuck, bad RAM bit N, corrupt ROM...);
      3. the exact fault as the atlas names it: once the chip and the
         screenshot are given, list that chip's simulated faults that also
         match the screenshot, to confirm one of.
- [x] How sure, per record: tested bad in a chip tester / swapping it
      fixed it / replaced several, not sure which.
- [ ] How sure, per chip: replaced, or confirmed bad (eight RAMs swapped
      at once say much less than one).
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
- [x] Records without a screenshot: chips only, they count in the
      statistics and the probing ranking.
- [ ] A record's symptom in the atlas browser's terms (yellow border,
      vertical bars, blinking), for records without a screenshot, so a
      record's symptom and a browser search are the same thing.
- [ ] Phone photos of a CRT: record what the picture is (capture card /
      photo) and trust photos less in matching. (Correcting a photo's angle
      would be a project of its own.)
      2026-10-29, Test images/c64_garbled_photo.png (a phone photo off the
      internet, said to be U27; garbled characters, light blue border, blue
      screen) matched as "Vertical bars" (U5). Three causes found:
      - [x] Uneven lighting (brighter in the middle): the border wasn't
            found at the top, so the whole photo was taken as the active
            display. Fixed: the border is compared with its color NEAR each
            pixel (a Coons patch of the ring, analyze() in atlas_match.c).
            Real test pictures: ZXULAbad.jpg same #1, 78% -> 91%; u4_2's
            IC4 bridge #2 -> #3; the rest unchanged or #3 only.
            --match-test 60, new atlas, old edge code -> new edge code,
            the same pictures: seed 31 32 -> 30 #1, top 3 45 -> 45; seed
            32 35 -> 33 #1, top 3 53 -> 53. Mostly near ties (0.1-0.4%)
            flipping both ways, but seed 31's #25 (zx48i1, blank black
            screen, white border, match_test/capture-25-zx48i1-boot.png)
            drops 98.6% -> 88.0% (#1 -> #2): the same box is found, yet
            "drawn cells 0 vs 0" becomes "3 vs 3" and the color gain
            moves a little, so something the box/content code feeds (the
            content box?) changed.
            Found 2026-10-30: the edge moved only 0.09 px. A blurred edge
            leaves the active display's outermost line half border; the
            old code had the same, on the bottom line (24 px), and was
            lucky. quantize() already turned outer pixels in the border's
            color into the background, but these had snapped to bright
            white (the cell's ink), not white. Fixed: an outer pixel that
            is a blend of the background and the border counts too.
            #25 is #1 again (98.8%). Tried and dropped: moving each edge
            to the middle of its blur (refine_edges): worse.
            Seeds 31 + 32 together (120 screens, #1 / top 3): old edge
            code 67 / 98, old + blend 68 / 98, Coons 63 / 98, Coons +
            blend (kept) 64 / 99, Coons + refine + blend 61 / 93. The
            same code swings by 4 between the seeds: within the noise.
            Only the Coons versions get ZXULAbad.jpg (90% vs 78%).
            (Seed 31 was 34/60 before tonight: the new atlas changed which
            screens are picked.) Test images, old -> new edge: ZXULAbad.jpg
            78.4% -> 90.6% (screen edge found); the rest the same or
            within 1.3%.
            --match-test reuses the pictures already in its folder, so a
            new --seed does nothing there: use --match-dir with an empty
            folder for another seed.
      - [ ] Colors: the camera clipped the blue channel (border and screen
            both ~238 blue), so no gain + offset correction can tell light
            blue from blue: most of the border snaps to blue. Corrected by
            hand (two anchors per channel), the top matches become garbled
            characters / repeating patterns. Idea: a "photo" color model,
            a monotone curve per channel anchored on the border and the
            background, ignoring a clipped channel.
      - [ ] Even corrected, U27.9:open ranks #73: its simulated screen has
            garbage on the top 64% and stays so (4 s and 30 s the same),
            the photo has it all over. A gap in the simulator, or the
            photo is of another fault: check against a real U27 failure.
- [x] Failure statistics: which chips fail most, per machine (the records
      window), and they feed the probing ranking. Still to do: with which
      symptoms.
- [x] Gaps in the simulator: confirmed repairs that match nothing in the
      atlas, listed as faults the catalog is missing.
      Done 2026-10-31: "Gaps in the simulator" in the records window,
      --repair-gaps: each record's screenshot matched (8 groups, all
      causes); a record whose chips no group's causes name is a gap, one
      named only below the best is listed next. The one real record
      (Issue 2, IC1 ULA): explained by the best match.
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
- [x] Published 2026-10-29: github.com/zzleezz/8bit-hw-simulator (fork of
      antirez/c64-hw-simulator, commits as zzleezz with the noreply
      address, Claude as co-author, "An AI-generated project" at the top
      of the README); release V1.0 with the zip and the seven-board atlas;
      the issue to antirez posted.
- [ ] Afterwards: work changed here has to be copied to ../github-upload
      again before a new commit (or move the work into that folder).
- [x] V1.1 released 2026-10-30 (by the user, from github-release/). The
      next one (V1.2) when enough is under "Unreleased" in CHANGELOG.md.

## 10. Stretch goal: the real machine's bus, read into the simulator
(the user's idea, 2026-10-29; reworked 2026-10-30)
The interface: Jim's (HappyLittleDiodes) 48-channel logic analyser for the
ZX Spectrum, plugged into its expansion port. Jim is a good friend of the
user. It is two of Agustin Gimenez Bernad's Pico W analyser boards (24
channels each, up to 100 Msps, wireless: his open-source LogicAnalyzer,
gusmanb on GitHub) stacked on Jim's Spectrum interface board, reading the
address and data buses and control signals
(https://www.hackster.io/news/the-zx-spectrum-spills-its-secrets-a9fed4c6d02c).
So the hardware (5 V levels, channel count, wiring) is solved; our side is
software: read its captures.
- [ ] Ask Jim (through the user):
      - the capture file format (the LogicAnalyzer's own, CSV, sigrok?) and
        which channel is which signal on his interface board;
      - how long a capture can be at a useful rate (the Pico's RAM: a few
        times the Z80's 3.5 MHz clock, how many ms?);
      - whether it can trigger on /RESET going high (power-on or reset);
      - whether his interface board is open, and if he'd make a C64
        adapter (or let us/the user adapt it).
      - a sample capture or two from a healthy 48K, to start with.
- [ ] Step 1, readings: from a capture, each signal low / high / pulsing,
      imported into "Diagnose by probing" as readings ("Import capture").
      Floating can't be told by a digital input: low/high/pulsing only.
- [ ] Step 2, a trace from reset: the capture from /RESET released, cut
      into bus cycles (on /MREQ, /RD, /WR, /M1 for the Z80), compared with
      the simulated healthy board from reset: the first cycle that differs
      ("at cycle 412 the CPU read $FD at $FCE2, the ROM holds $F5"). Works
      on a dead machine with a black screen. Then against the faults: which
      ones go wrong at that cycle the same way. Needs the simulation's
      early timing to match a real board: check on a healthy one first
      (a sample capture from Jim).
- [ ] C64 version: the same two analyser boards on an adapter for the
      C64's 44-pin expansion port (A0-A15, D0-D7, R/W, PHI2, BA, /DMA,
      /IRQ, /NMI, /RESET, /IO1, /IO2, /ROML, /ROMH, /GAME, /EXROM, dot
      clock), the cycles cut on PHI2. Listen only: never drive /GAME,
      /EXROM or /DMA.
- [ ] Later: a DIP test clip (on the PLA, the CPU...) for nets the port
      doesn't carry (/CASRAM, /KERNAL, /CHAROM, the VIC's RAM addresses,
      color RAM): all of a chip's pins at once.

## 11. Dead Z80s on the Spectrum not recognised (found 2026-10-30, for the weekend)
Two bad Z80s (Backbit Chip Tester Pro: completely faulty), screenshots
`Test images/ZX-CPU-bad1.png` and `ZX-CPU-bad2.png`: a white border and
thin vertical stripes with colour blocks, the power-on contents of the
screen RAM (the CPU never runs). `--diagnose-text` puts ULA faults first
(75%), no IC2 cause in the top 5. The simulated dead CPU (IC2:removed)
looks nothing like it: a black border and two big white blocks, because
the ULA's border powers up 0 (`ula_reset`, ula.c) and the 4116s power up
as whole rows of 00/FF (parts.c:860). Faults inside the Z80 wouldn't help:
the dead CPU is there, its picture is wrong. For now: a repair record
(bad1 recorded as IC2 matched bad2 at 86%, tried in a scratch file).
A third chip, `ZX-CPU-bad3.png`: a magenta border (the border is random
at power-on), but the same black and white vertical stripes in the same
places, the two halves of the screen alike, with random colours (some
flashing: FLASH bits in the garbage attributes). The bad1 record matched
it at only 65% (under 75%: not shown): the border colour and the
attribute colours count too much. So B should match on the stripes of
the bitmap and leave out the border colour and the attribute colours.
- [x] B (recommended): a picture class in the matcher, "screen and border
      never written" (power-on RAM garbage, nothing ran), causes the
      CPU, the ROM, the clock, reset. Matches by kind, not by the exact
      pattern, so any board's RAM chips. Then rebuild the Spectrum
      pictures (estimate first).
      Done 2026-10-31 (atlas_match.c, no atlas rebuild needed).
      unwritten_likeness() on the
      capture's cell colours: the three thirds alike (>= 0.7), busy across
      (>= 0.2), changing row to row (>= 0.15: tells it from the bars a ULA
      fault draws, which run down unbroken; real bad ULAs 0-0.06, dead
      Z80s 0.18-0.71), 40% of cells drawn. Such a capture scores 70-82%
      against the board's never written screen (the outcomes with the
      picture of IC2:removed), labelled "Screen never written...".
      Results, all five Spectrums: ZX-CPU-bad1/2/3 #1 at 82/79/81% (were
      not in the top 5); ZXULAbad 1-3 and bad-ula.png unchanged.
      --match-test 60, seeds 31 and 32: identical per capture (36/51 and
      42/54). --match-debug prints the measures per reading.
      Open: IC2:removed is cause 26 of 128 in that group (the user's
      weights put upper RAM and ROM faults first); the user to decide.
- [ ] A: a power-on closer to the real one: a random border colour, the
      4116s' fine stripes instead of whole 00/FF rows. Only ever like one
      board's chips, so a weak match on its own; maybe with B.
- [x] One record shown twice in `--diagnose-text`: not a bug (checked
      2026-10-30). The 97% line was `--add-repair`'s own diagnosis of
      bad1 (it prints one after recording), the 86% line `--diagnose-text`
      on bad2: two commands' output read as one.
- [x] Re-run `--probe-test` after the probing readings changed
      (2026-10-30): a fifth reading "bad levels" (PROBE_BAD, matched to
      NET_CONTENTION, which used to read as floating; cost table in
      probe_db.c) and a "Same as a healthy board" button. The test's
      simulated probe now reads fights as bad levels and misreads over
      five readings, not four, so compare with the last figures (true
      fault in the best fits 93-96%, README "Diagnosing by probing").
      Uses every core: say how long first (5 faults took 17 min).
      Run 2026-10-30 (68 min each, 28 threads), --probe-test 20 --seed 11
      (140 faults), also with bad levels against pulsing raised 0.3 -> 0.6:
      clean: #1 65/70/68% (bench/likeliest/fewest; 62/67/65% on
      2026-10-29), best fits 92%, in the running 95%, 229/211/208 faults
      fit best (176 bench before); screen known #1 81-84%, best fits 95%.
      --probe-noise 10: in the running 92% (README: 95-97%), first 6
      65-69% (65-74%), screen known first 6 84-87% (86-91%), in the
      running 96-97%. Not a clean A/B: the 10-29 figures predate the atlas
      refresh, and the README's are 120 faults on other seeds. To settle
      it: the same two runs with the exe from before the change
      (github-upload/c64-hw-sim.exe, built 13:20) on today's atlas.
      Settled 2026-10-30: the old probing code (github-upload/src, today's
      atlas, same seed) gives the same within 1% clean (#1 65/70/69%,
      best fits 91-92%, 228.6 fit best) and with 10% noise in the running
      91-92% too, the rest within a few points both ways (the misreads are
      drawn over five readings, not four, so they differ). No change from
      bad levels; the drop against the README's figures is the atlas
      refresh / this fault set, not the probing code.

## Left over from earlier work
- [x] Rebuild `fault_atlas.bin` (`--build-atlas`): it was out of date for the
      power-path faults (F1, L4, L5 said "CPU running"). Rebuilt with item 1.

## Dropped
- Intermittent faults (bad sockets, faults that appear once the board is warm):
  too vague to model well.
- A "legs to check" list or "check every leg of chip X" step for open legs
  in probing (2026-10-30): the user's call, not a good diagnostic
  scenario on the bench. The open-leg faults stay in the atlas.

## Notes
- Check with the user before anything that uses every core (atlas runs);
  tests run 4 at a time (JOBS=4 tests/mem-faults.sh).
