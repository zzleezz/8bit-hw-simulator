# ZX Diagnostics compared with our test ROM

What ZX Diagnostics tests on a 48K Spectrum, from its source, compared with
our test ROM (zxtest, version 5/6). To-do item 13.6.

Source read: github.com/brendanalford/zx-diagnostics, the head of 2024-01-06
(f224084, the v0.38 release; our cartridge copy is v0.37, whose 48K tests
are the same macros). Files: testrommain.asm (the order), defines.asm (the
RAM tests), 48tests.asm (upper RAM), ulatest.asm (the ULA menu),
romtables.asm (ROM CRCs and IC names).

AI-generated (Claude Code), 2026-11-09.

## What it does on a 48K

| Step | ZX Diagnostics | Ours (zxtest) |
|---|---|---|
| CPU data/address lines | not tested on their own | tests 0 and 1, checked on the bus by the analyser |
| Lower RAM, data | walking 1 in **every byte** (WALKLOOP) | walking pattern at one address (4000) |
| Lower RAM, addresses | random fill: PRNG words up then down (a wrong address reads another word) | test 9: each line A0-A13 alone; names the line, the mux pin or the chip pin |
| Lower RAM, cells | inversion (alternate bytes fill/~fill, both phases, 00 and FF) and March X | March X (the same four passes) |
| Upper RAM present? | 00 written, 255 reads ORed: FF = none ("16K?") | 55/AA at 8000/8001 (v3 on) |
| Upper RAM | the same four tests | the same as the lower bank (data walk, test 10, March X) |
| Refresh | not tested | test 8 (1.6 s untouched: a fail proves a fault) |
| ROM | CRC-16 of the **Spectrum's own ROM** (paged in by the diag board), looked up in a table of known ROMs; names the ROM IC | the cartridge's own image (a sum to v5, CRC-16 from v6): the bus reads, not the internal ROM |
| Interrupts | counts 0x300 interrupts with HALT: only that they come | test 6: the frame's length in loop turns, within a tolerance |
| ULA timing | frame length by a counting loop: names the ULA type (48K PAL 06CB, 128K, NTSC; contended or not) | test 7: contention, loop turns with reads of 4000 against a working board's |
| Floating bus | IN from port FF 4095 times: any non-FF read = the ULA's screen fetches are visible | **not tested** |
| ULA port decoding | menu item: alternate OUT FE / OUT FF with different borders; a steady border = only A0 decoded (judged by eye) | **not tested** |
| EAR/MIC | menu items: tones to MIC and EAR (by ear), the live FE read shown | test 11: EAR's level after OUT 00/08/10/18 |
| Keyboard | an interactive test (press each key) | test 5: every row reads all ones with no key |
| CPU type | NMOS or CMOS (OUT (C),0 then read back) | not tested |
| Failing IC | tables per board of which IC holds each bit | from the board's wiring in the simulator (any issue it models) |

## What it catches that we don't

1. **The floating bus.** Reading a port nothing answers (FF) returns what the
   ULA is fetching for the screen at that moment. It is the only trace of
   the ULA's screen reads the CPU can see, and we say "Not tested here: the
   picture". A test could fill the screen with a known pattern, read port
   FF many times, and check that the bytes seen are that pattern's (and FF
   in the border). That tests the ULA's video address counter and the
   screen data path. **Worth adding (v7)**; the analyser also sees each
   read's data, so a wrong byte could be timed.
2. **ULA port decoding (A0).** ZX Diagnostics judges it by eye on the
   border. The CPU can check it itself: OUT 00 to FE, OUT 18 to FF (A0 = 1:
   the ULA must ignore it), then IN FE: EAR must still read 0. If the ULA
   took the OUT to FF, the speaker bit is set and EAR reads 1. **Cheap: add
   to result 11**, once we know the SteamCard Pro doesn't decode any port
   with A0 = 1 for its own paging (an OUT there could page the test ROM
   out). Not in v6 for that reason.
3. **A walking 1 in every byte.** Not needed here: each DRAM on these
   boards holds one bit, so two data bits can only be shorted on the
   board's wiring, the same at every address, and our walk at one address
   (and test 0 on the bus) finds that. Per cell, the march's 0s and 1s
   already cover what a walk at each byte would. No gain.
4. **Inversion (alternate-byte) patterns.** These catch faults between
   neighbouring cells that depend on the data pattern. March X catches the
   common coupling faults; the alternate patterns add little for 4116/4164
   faults seen in repairs. Low priority.

## What we catch that it doesn't

- The data and address lines between the CPU and the edge connector, in
  both directions (tests 0 and 1, read on the bus).
- Each address line by itself, named as a mux output, a mux input, the mux
  chip, or one chip's pin (tests 9 and 10). Its random fill only says "a
  bit is bad".
- The interrupt's timing and the contention against a working board's
  counts (it only checks that interrupts arrive, and names the ULA type).
- Refresh (test 8).
- The EAR comparator's level by issue (result 11, judged per issue).

## What neither can do from a cartridge

The Spectrum's own ROM. ZX Diagnostics reads it by paging its own ROM out
(its diag board or other hardware). The SteamCard keeps /ROMCS asserted
while our ROM runs, so the internal ROM can't be read. If the SteamCard can
release /ROMCS from software, a CRC of the internal ROM (16K at 0000) would
be a worthwhile test 12. **To check: the SteamCard Pro's paging.**

## Suggested next steps, in order

1. Find out which I/O ports the SteamCard Pro decodes, and whether it can
   page itself out from software. That decides 2 and 4.
2. Add the port A0 decode check to result 11 (a few bytes; no new capture).
3. A floating bus test (v7): the ULA's screen fetches as seen by the CPU.
4. If the card can page out: a CRC of the Spectrum's own ROM.
