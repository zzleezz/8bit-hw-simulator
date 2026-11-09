; zxtest.asm - a test ROM for 16K/48K Spectrums, made to be watched by the
; logic analyser on the edge connector (to-do.md section 13).
; AI-generated (Claude Code). Assemble with build.sh (sjasmplus, then the
; ROM checksum patched in by patchsum.py).
;
; A 16K cartridge ROM (the SteamCard Pro's bank 3). It loops for ever
; through nine tests, and three more results inside tests 3, 4 and 5
; (9-11). Each test:
;   - starts by writing its number to its marker, MARK+n (3FA0-3FA8): the
;     analyser arms on that address (9-11 have none: they come right after
;     test 3's data walk, test 4's, and test 5's result);
;   - sets the border to its number;
;   - ends by writing four result bytes to RES+4n (3FC0-3FEF):
;       +0 status: 00 OK, 01 FAIL, 02 none / skipped
;       +1 bits:   the data bits found wrong (RAM), or the test's flags
;       +2 +3:     an address or a count, high byte first
; Writes to 0000-3FFF go nowhere on a 16K/48K (checked on the SteamCard Pro
; with zxbench, 2026-11-08), so they need no RAM and put every data bit on
; the bus. Nothing else in the ROM is at 3F80-3FFF, and nothing READS
; there either: the analyser's trigger is the address alone, so a read of
; 3FC8 would fire a trigger set for test 2's result.
;
; RAM is used only after it has passed: tests 0-5 use no RAM and no stack
; (subroutines return with JP (IX)). Tests 6 and 7 need a stack (for the
; interrupts): in the lower RAM if it passed, else in the upper; skipped if
; neither did. The screen (results as text) needs the lower RAM.
;
;   0 alive     20 writes: 00 FF 55 AA, walking 1, walking 0 (3F80-3F93);
;               +2 +3 = the ROM's VERSION
;   1 address   reads with A0-A15 walking 1 then walking 0 (for the analyser)
;   2 ROM CRC   CRC-16/CCITT-FALSE of 0000-3F7D (from v6; v1-5: a 16-bit sum of
;               0000-3F7F but CSUM) (bits 00, +2 +3 = the CRC)
;   3 lower RAM 4000-7FFF: data walk at 4000, the address lines (test 9),
;               then a march (w0 up, r0w1 up, r1w0 down, r0 down); +1 the
;               bad bits, +2 +3 the first bad address
;   4 upper RAM 8000-FFFF: there at all? (55 to 8000, AA to 8001; none:
;               status 02, +2 +3 the two bytes read back, FF FF on a 16K),
;               then the same data walk (at 8000), address lines (test 10)
;               and march
;   5 ULA ports OUT FE each border, MIC, EAR value; IN FE each keyboard
;               row, no key pressed: +1 key bits read low, +2 the rows they
;               were on, +3 IN FE with every row selected
;   6 interrupts IM 1: the frame's length (+2 +3 = loop counts between two
;               interrupts); IM 2 (table at 3B00, handler at 3A3A).
;               +1 bit 0 no IM 1 interrupt, bit 1 no IM 2, bit 2 the
;               frame's length out of range
;   7 contention the same count with a read of 4000 in the loop (the ULA
;               holds the CPU's clock while it fetches the screen)
;   8 refresh   the upper RAM (if it passed) filled with 00 and FF in
;               turns of 128 bytes (every row both ways) before the screen,
;               read back after the 1.5 s hold: nothing touched it in
;               between, only the CPU's refresh kept it. +1 the bad bits,
;               +2 +3 the first bad address. (The lower RAM needs no such
;               test: the ULA's screen fetches refresh it.) DRAMs often
;               keep their data a second or more unrefreshed when cool, so
;               a pass doesn't prove the refresh; a fail does disprove it.
;   9 lower RAM's address lines A0-A13 (in test 3, before its march): for
;               each line k, BASE and every BASE+2^j set to AA, then 55 to
;               BASE+2^k; any other of them not reading AA back (in the data
;               bits the walk found good) = line k doesn't reach the chips
;               as its own (stuck, open, or shorted to another line).
;               +1 the data bits it showed in (all: a line common to the
;               chips, the multiplexer; one: that chip's address pin),
;               +2 +3 the lines (bit k = Ak). Status 02: not run (every
;               data bit bad).
;  10 upper RAM's address lines A0-A14 (in test 4), the same
;  11 EAR in    IN FE's bit 6 after OUT 00, 08, 10, 18 (bits 0-3 of +1),
;               +2 IN FE after OUT 00, +3 after OUT 18 (no keyboard row).
;               FAIL if 00 reads 1 or 18 reads 0 (the same on every issue
;               with nothing in the EAR socket; 08 and 10 are reported only:
;               an Issue 2 and an Issue 3 are known to differ there).
; Then the results on the screen (if the lower RAM passed; test 8's line
; WAIT until it is read back), the border green (all OK), yellow (no upper
; RAM: right on a 16K only) or red, a beep, about 1.5 s to read it, test
; 8's read-back (red, the low beep and 1.5 s more if it failed, else 0.5
; s), and again. MARK+0F (3FAF) = the loop's end, data = the loop count.

        OUTPUT  "zxtest.rom"

; A new VERSION: keep the old zxtest.rom as carts/ and zxtest/zxtest-vN.rom
; (N the old version): captures of a cartridge not reprogrammed yet are
; compared with it.
VERSION EQU     6               ; test 0's result +3, "ZXTEST n" at 0070, the title
MARK    EQU     3FA0h           ; test n's marker
RES     EQU     3FC0h           ; test n's four result bytes
ALIVE   EQU     3F80h           ; test 0's writes
CSUM    EQU     3F7Eh           ; the ROM's CRC (patched in), after the bytes it covers
SAVE    EQU     5B00h           ; the results kept in the lower RAM
STACKL  EQU     7FF0h
STACKU  EQU     0FFF0h
IM2TAB  EQU     3B00h           ; 257 bytes of IM2V
IM2V    EQU     3Ah             ; the handler is at 3A3A
WAITN   EQU     6000            ; a wait loop's count: about 2 frames
; The counts of a working 48K (from the simulator; the wait loop is 24 T
; a turn, so a 69888 T frame is 2912 turns): +-8 allowed.
FRAME_N EQU     2912
CONT_N  EQU     2209            ; measured on the simulated Issue 2
NOCONT_N EQU    2254            ; the same loop with no contention (not used here: for the PC)
NTOL    EQU     8
NTESTS  EQU     12              ; results 0-11 (9-11 within tests 3-5)

; shadow registers: B' bit 0 lower RAM OK, bit 1 upper RAM OK;
; C' test 2's status; HL' test 2's sum; D' the loop count.

; ---- macros ---------------------------------------------------------------
; An interrupt's end (IM 1 at 0038, IM 2 at 3A3A): drop its return address
; and go where IX says - but only if IX is in test 6 or 7, which wait for
; it. Anything else got here by mistake: an FF (RST 38) read at power on
; before the cartridge drove the bus, with IX still random (a real Issue
; 3B hung for ever in 0038 -> IX=0020, an FF -> 0038). Then start again.
        MACRO   INTBACK
        inc     sp
        inc     sp
        ld      a, ixh
        cp      t6 >> 8
        jr      z, .back
        cp      t7 >> 8
        jr      z, .back
        jp      reset
.back   jp      (ix)
        ENDM

; (A label in a macro is local to each use of it: sjasmplus 1.24.)

; Call a subroutine that returns with JP (IX) (no stack).
        MACRO   CALLX addr
        ld      ix, .ret
        jp      addr
.ret
        ENDM

; A test's start: the marker write and the border (n AND 7: 8 is black,
; not MIC).
        MACRO   START n
        ld      a, n
        ld      (MARK + n), a
        IF n > 7
        ld      a, n & 7
        ENDIF
        out     (0FEh), a
        ENDM

; A test's result, D E H L, to the mailbox; to SAVE too if the lower RAM
; passed.
        MACRO   RESULT n
        ld      a, d
        ld      (RES + 4 * n), a
        ld      a, e
        ld      (RES + 4 * n + 1), a
        ld      a, h
        ld      (RES + 4 * n + 2), a
        ld      a, l
        ld      (RES + 4 * n + 3), a
        exx
        bit     0, b
        exx
        jr      z, .nosave
        ld      (SAVE + 4 * n + 2), hl
        ld      (SAVE + 4 * n), de
.nosave
        ENDM
; (SAVE+4n) = E (bits), +1 = D (status), +2 = L, +3 = H: little-endian.

; A read that was wrong (A, not D): its bits into E; the first such
; address (HL) into IY.
        MACRO   FAILBITS
        xor     d
        or      e
        ld      e, a
        ld      a, iyh
        or      iyl
        jr      nz, .had            ; IY has one already
        ld      a, h
        ld      iyh, a
        ld      a, l
        ld      iyl, a
.had
        ENDM

; A march over PAGES pages from START (bad bits into E, the first bad
; address into IY).
        MACRO   MARCH start, pages
        ld      hl, start
        ld      c, pages
        ld      d, 0
        CALLX   fill_up
        ld      hl, start
        ld      c, pages
        ld      d, 0
        CALLX   rw_up
        ld      hl, start + pages * 256 - 1
        ld      c, pages
        ld      d, 0FFh
        CALLX   rw_down
        ld      hl, start + pages * 256 - 1
        ld      c, pages
        ld      d, 0
        CALLX   read_down
        ENDM

; The next of BASE, BASE+1, BASE+2, BASE+4 ... BASE+2^k in HL (BASE 4000
; or 8000: its one bit is above every offset). Uses A.
        MACRO   NEXTADR base
        ld      a, l
        or      a
        jr      nz, .dbl
        ld      a, h
        cp      base >> 8
        jr      nz, .dbl
        inc     l                   ; BASE -> BASE+1
        jr      .done
.dbl    add     hl, hl              ; 2 (BASE + off): the offset doubled,
        IF base == 4000h            ; BASE's bit moved up one (or out)
        res     7, h
        set     6, h
        ELSE
        set     7, h
        ENDIF
.done
        ENDM

; Test SLOT: the address lines A0..A(NL-1) of the bank at BASE (see the
; top). In: E the data bits the walk found bad. Out: E as it came, the
; result written (RESULT SLOT), HL the lines (also in SP: no stack is in
; use), A' the bits it showed in. Uses BC, D, IX, IY.
        MACRO   ADDRTEST base, nl, slot
        ld      a, e
        cpl
        ld      ixl, a              ; the good bits
        ld      hl, 0
        ld      de, 2 << 8          ; status 02, no bits: not run
        or      a
        jp      z, .res
        ld      iy, 0               ; the lines
        ld      c, 0                ; the bits
        ld      de, 1               ; line k's offset, 2^k
        ld      ixh, nl
.k      ld      hl, base            ; BASE and each BASE+2^j: AA
        ld      b, nl + 1
.f      ld      (hl), 0AAh
        NEXTADR base
        djnz    .f
        ld      hl, base            ; BASE+2^k: 55
        add     hl, de
        ld      (hl), 55h
        ld      hl, base            ; read them back
        ld      b, nl + 1
.c      ld      a, l                ; this one BASE+2^k?
        cp      e
        jr      nz, .aa
        ld      a, h
        xor     base >> 8
        cp      d
        jr      nz, .aa
        ld      a, 55h
        jr      .x
.aa     ld      a, 0AAh
.x      xor     (hl)
        and     ixl
        jr      z, .ok
        or      c                   ; wrong: the bits, and line k
        ld      c, a
        ld      a, iyl
        or      e
        ld      iyl, a
        ld      a, iyh
        or      d
        ld      iyh, a
.ok     NEXTADR base
        djnz    .c
        ex      de, hl              ; the next line
        add     hl, hl
        ex      de, hl
        dec     ixh
        jp      nz, .k
        ld      a, iyh
        ld      h, a
        ld      a, iyl
        ld      l, a
        ld      e, c
        ld      d, 0
        or      h
        jr      z, .res
        inc     d                   ; FAIL
.res    RESULT  slot
        ld      sp, hl              ; the lines and the bits, for later
        ld      a, e
        ex      af, af'
        ld      a, ixl              ; E as it came
        cpl
        ld      e, a
        ENDM

; ---- 0000 ----------------------------------------------------------------
        ORG     0
        di
        jp      reset

        ds      38h - $, 0FFh
int_im1:                        ; test 6/7 only: back to where IX says
        INTBACK

        ds      66h - $, 0FFh
        jp      reset           ; NMI: start again

        ds      70h - $, 0FFh
        db      "ZXTEST ", '0' + VERSION, " - logic analyser test ROM", 0

; The constants the PC side (lacap_zxtest.c) needs, read from this image
; (version 4 on): "ZTAB", then bytes VERSION, NTESTS, then words, low byte
; first. Nothing reads it here.
        ds      0A0h - $, 0FFh
        db      "ZTAB", VERSION, NTESTS
        dw      MARK, RES, ALIVE, CSUM, t1, FRAME_N, CONT_N, NOCONT_N, NTOL

        ds      100h - $, 0FFh
reset:
        di
        im      1
        xor     a
        ld      i, a
        exx
        ld      bc, 0
        ld      de, 0
        ld      hl, 0
        exx
        jp      t0

; ---- test 0: alive (0200) -------------------------------------------------
        ds      200h - $, 0FFh
t0:
        START   0
        ld      a, 00h
        ld      (ALIVE + 0), a
        ld      a, 0FFh
        ld      (ALIVE + 1), a
        ld      a, 55h
        ld      (ALIVE + 2), a
        ld      a, 0AAh
        ld      (ALIVE + 3), a
.n      = 0
        DUP     8
        ld      a, 1 << .n
        ld      (ALIVE + 4 + .n), a
.n      = .n + 1
        EDUP
.n      = 0
        DUP     8
        ld      a, 0FFh ^ (1 << .n)
        ld      (ALIVE + 12 + .n), a
.n      = .n + 1
        EDUP
        ld      de, 0
        ld      hl, VERSION         ; which ROM this is (version 1 wrote 0000)
        RESULT  0
        jp      t1

; ---- test 1: the address lines (0300) ------------------------------------
        ds      300h - $, 0FFh
t1:
        START   1
.n      = 0
        DUP     16
        ld      a, (1 << .n)
.n      = .n + 1
        EDUP
.n      = 0
        DUP     16
        ld      a, (0FFFFh ^ (1 << .n))
.n      = .n + 1
        EDUP
        ld      de, 0
        ld      hl, 0
        RESULT  1
        jp      t2

; ---- test 2: the ROM's CRC (0400) ----------------------------------------
        ds      400h - $, 0FFh
t2:
        START   2
        ; CRC-16/CCITT-FALSE (poly 1021, from FFFF) of 0000 up to CSUM, by
        ; the two tables (a byte: hi' = lo ^ crc_hi[hi ^ byte], lo' =
        ; crc_lo[hi ^ byte]); about 0.45 s. It catches what a sum doesn't:
        ; bytes swapped or in the wrong place (an address line).
        ld      ix, 0
        ld      de, 0FFFFh
        ld      bc, CSUM            ; the stored CRC: the next two bytes
.l      ld      a, (ix + 0)
        inc     ix
        xor     d
        ld      l, a
        ld      h, crc_hi >> 8
        ld      a, (hl)
        xor     e
        ld      d, a
        inc     h                   ; crc_lo
        ld      e, (hl)
        dec     bc
        ld      a, b
        or      c
        jr      nz, .l
        ex      de, hl
        ; HL = the CRC; compare with the stored one
        ld      de, (CSUM)
        ld      a, h
        cp      d
        jr      nz, .bad
        ld      a, l
        cp      e
        jr      nz, .bad
        ld      d, 0
        jr      .rep
.bad    ld      d, 1
.rep    ld      e, 0
        ; keep it for the screen: status in C', the sum in HL'
        ld      a, d
        exx
        ld      c, a
        exx
        ld      a, h
        exx
        ld      h, a
        exx
        ld      a, l
        exx
        ld      l, a
        exx
        RESULT  2
        jp      t3

; ---- test 3: the lower RAM (0600) ----------------------------------------
        ds      600h - $, 0FFh
t3:
        START   3
        exx
        res     0, b                ; not OK until it passes
        exx
        ld      e, 0
        ld      iy, 0
        ; the data lines at one address: walking 1, walking 0, 00, FF
        ld      hl, 4000h
        ld      ix, walk_tab
        ld      b, 18
.w      ld      a, (ix + 0)
        ld      (hl), a
        ld      a, (hl)
        xor     (ix + 0)
        or      e
        ld      e, a
        inc     ix
        djnz    .w
        ADDRTEST 4000h, 14, 9       ; test 9: its result now, kept for SAVE
        ld      iy, 0
        ld      a, e
        or      a
        jr      z, .m
        ld      iy, 4000h
.m
        MARCH   4000h, 64
        ld      d, 0
        ld      a, e
        or      a
        jr      z, .ok
        ld      d, 1
.ok
        ld      a, iyh
        ld      h, a
        ld      a, iyl
        ld      l, a
        ld      a, d
        or      a
        jr      nz, .r
        exx
        set     0, b
        exx
        ; the lower RAM is good: keep tests 0-2's results now
        ld      bc, VERSION         ; test 0's +2 +3 (L, H)
        ld      (SAVE + 2), bc
        ld      bc, 0
        ld      (SAVE + 0), bc
        ld      (SAVE + 4), bc
        ld      (SAVE + 6), bc
        exx
        ld      a, c
        exx
        ld      (SAVE + 9), a       ; test 2's status
        xor     a
        ld      (SAVE + 8), a
        exx
        ld      (SAVE + 10), hl     ; test 2's sum (HL')
        exx
        ; and test 9's (the lines in SP, the bits in A')
        ld      hl, 0
        add     hl, sp
        ld      (SAVE + 4 * 9 + 2), hl
        ex      af, af'
        ld      (SAVE + 4 * 9), a
        ld      a, h
        or      l
        jr      z, .s9
        ld      a, 1
.s9     ld      (SAVE + 4 * 9 + 1), a
        ld      hl, 0               ; test 3's result again: passed
.r
        RESULT  3
        jp      t4

; ---- test 4: the upper RAM (0900) ----------------------------------------
        ds      900h - $, 0FFh
t4:
        START   4
        exx
        res     1, b
        exx
        ; there at all? 55 to 8000, AA to 8001: one read back right is enough.
        ; None: the two bytes read are the result's +2 +3 (FF FF: nothing
        ; drives the bus, a 16K; else a dead bank, or two chips stuck)
        ld      a, 55h
        ld      (8000h), a
        cpl
        ld      (8001h), a
        ld      a, (8000h)
        ld      h, a
        ld      a, (8001h)
        ld      l, a
        ld      a, h
        cp      55h
        jr      z, .yes
        ld      a, l
        cp      0AAh
        jr      z, .yes
        ld      b, h                ; test 10 not run (status 02)
        ld      c, l
        ld      de, 2 << 8
        ld      hl, 0
        RESULT  10
        ld      h, b
        ld      l, c
        ld      d, 2
        ld      e, 0
        jp      .r
.yes    ; the data lines at 8000, as test 3 at 4000
        ld      e, 0
        ld      hl, 8000h
        ld      ix, walk_tab
        ld      b, 18
.w      ld      a, (ix + 0)
        ld      (hl), a
        ld      a, (hl)
        xor     (ix + 0)
        or      e
        ld      e, a
        inc     ix
        djnz    .w
        ADDRTEST 8000h, 15, 10      ; test 10
        ld      iy, 0
        ld      a, e
        or      a
        jr      z, .m
        ld      iy, 8000h
.m
        MARCH   8000h, 128
        ld      d, 0
        ld      a, e
        or      a
        jr      z, .ok
        ld      d, 1
.ok     ld      a, iyh
        ld      h, a
        ld      a, iyl
        ld      l, a
        ld      a, d
        or      a
        jr      nz, .r
        exx
        set     1, b
        exx
.r
        RESULT  4
        jp      t5

; ---- test 5: the ULA's ports (0C00) --------------------------------------
        ds      0C00h - $, 0FFh
t5:
        START   5
        ; the border colours, then MIC (bit 3) and EAR (bit 4)
        xor     a
.b      out     (0FEh), a
        inc     a
        cp      8
        jr      nz, .b
        ld      a, 08h
        out     (0FEh), a
        ld      a, 10h
        out     (0FEh), a
        ld      a, 18h
        out     (0FEh), a
        ld      a, 5
        out     (0FEh), a
        ; each keyboard row: B = FE, FD, FB, ... 7F
        ld      de, 0               ; E: key bits low, D: their rows
        ld      c, 0FEh
        ld      b, 0FEh
        ld      h, 1                ; this row's bit
.k      in      a, (c)
        cpl
        and     1Fh
        jr      z, .kn
        or      e
        ld      e, a
        ld      a, d
        or      h
        ld      d, a
.kn     rlc     b
        sla     h
        jr      nz, .k
        ld      h, d
        ld      b, 0
        in      l, (c)              ; every row at once
        ld      d, 0
        ld      a, e
        or      a
        jr      z, .r
        ld      d, 1
.r
        RESULT  5
        ; test 11, EAR in: IN FE's bit 6 after OUT 00, 08, 10, 18
        ld      e, 0                ; the bits read 1
        ld      d, 1                ; this OUT's bit in E
        ld      h, 0                ; the OUT
.e      ld      a, h
        out     (0FEh), a
        ld      b, 0                ; about 1 ms to settle
.ew     djnz    .ew
        ld      a, 0FFh             ; no keyboard row
        in      a, (0FEh)
        ld      c, a
        bit     6, a
        jr      z, .e0
        ld      a, e
        or      d
        ld      e, a
.e0     ld      a, h
        or      a
        jr      nz, .e1
        ld      iyl, c              ; after OUT 00
.e1     sla     d
        ld      a, h
        add     a, 8
        ld      h, a
        cp      20h
        jr      nz, .e
        ld      a, iyl
        ld      h, a
        ld      l, c                ; after OUT 18
        ld      d, 0
        ld      a, e
        and     9                   ; 00 low and 18 high on every issue
        cp      8
        jr      z, .eok
        inc     d
.eok    RESULT  11
        jp      t6

; ---- test 6: interrupts (0D00) -------------------------------------------
        ds      0D00h - $, 0FFh
t6:
        START   6
        exx
        ld      a, b
        exx
        ld      sp, STACKL
        bit     0, a
        jr      nz, .st
        ld      sp, STACKU
        bit     1, a
        jr      nz, .st
        ld      d, 2                ; no RAM for a stack: skipped
        ld      e, 0
        ld      hl, 0
        jp      .r
.st     ld      e, 0
        im      1
        ; wait for one interrupt (to start on a frame's edge)
        ld      ix, .s1
        ld      bc, WAITN
        ei
.w1     dec     bc
        ld      a, b
        or      c
        jp      nz, .w1
        di
        set     0, e
        ld      hl, 0
        jp      .im2
        ; the next one: count the turns
.s1     ld      ix, .s2
        ld      bc, WAITN
        ei
.w2     dec     bc
        ld      a, b
        or      c
        jp      nz, .w2
        di
        set     0, e
        ld      hl, 0
        jp      .im2
.s2     ld      hl, WAITN
        or      a
        sbc     hl, bc
        push    hl                  ; HL in FRAME_N +- NTOL?
        ld      bc, FRAME_N - NTOL
        or      a
        sbc     hl, bc
        jr      c, .out
        pop     hl
        push    hl
        ld      bc, FRAME_N + NTOL + 1
        or      a
        sbc     hl, bc
        jr      c, .in
.out    set     2, e
.in     pop     hl
        ; IM 2
.im2    ld      a, IM2TAB >> 8
        ld      i, a
        im      2
        ld      ix, .s3
        ld      bc, WAITN
        ei
.w3     dec     bc
        ld      a, b
        or      c
        jp      nz, .w3
        di
        set     1, e
.s3     im      1
        xor     a
        ld      i, a
        ld      d, 0
        ld      a, e
        or      a
        jr      z, .r
        ld      d, 1
.r
        RESULT  6
        jp      t7

; ---- test 7: contention (0E00) -------------------------------------------
        ds      0E00h - $, 0FFh
t7:
        START   7
        exx
        ld      a, b
        exx
        and     3
        jr      nz, .st             ; SP is set by test 6
        ld      d, 2
        ld      e, 0
        ld      hl, 0
        jp      .r
.st     ld      e, 0
        im      1
        ld      ix, .s1
        ld      bc, WAITN
        ei
.w1     dec     bc
        ld      a, b
        or      c
        jp      nz, .w1
        di
        set     0, e
        ld      hl, 0
        jp      .done
.s1     ld      ix, .s2
        ld      bc, WAITN
        ld      hl, 4000h
        ei
.w2     ld      a, (hl)             ; the contended read
        dec     bc
        ld      a, b
        or      c
        jp      nz, .w2
        di
        set     0, e
        ld      hl, 0
        jp      .done
.s2     ld      hl, WAITN
        or      a
        sbc     hl, bc
        push    hl
        ld      bc, CONT_N - NTOL
        or      a
        sbc     hl, bc
        jr      c, .out
        pop     hl
        push    hl
        ld      bc, CONT_N + NTOL + 1
        or      a
        sbc     hl, bc
        jr      c, .in
.out    set     2, e
.in     pop     hl
.done   ld      d, 0
        ld      a, e
        or      a
        jr      z, .r
        ld      d, 1
.r
        RESULT  7
        jp      show

; ---- the march's parts: HL from, C pages, D the value; JP (IX) back ------
        ds      1000h - $, 0FFh
fill_up:
        ld      b, 0
.l      ld      (hl), d
        inc     hl
        djnz    .l
        dec     c
        jr      nz, .l
        jp      (ix)

; read D, write its complement, going up
rw_up:
        ld      b, 0
.l      ld      a, (hl)
        cp      d
        jr      nz, .f
.c      cpl
        ld      (hl), a
        inc     hl
        djnz    .l
        dec     c
        jr      nz, .l
        jp      (ix)
.f      FAILBITS
        ld      a, d
        jr      .c

; read D, write its complement, going down
rw_down:
        ld      b, 0
.l      ld      a, (hl)
        cp      d
        jr      nz, .f
.c      cpl
        ld      (hl), a
        dec     hl
        djnz    .l
        dec     c
        jr      nz, .l
        jp      (ix)
.f      FAILBITS
        ld      a, d
        jr      .c

; read D, going down
read_down:
        ld      b, 0
.l      ld      a, (hl)
        cp      d
        jr      nz, .f
.c      dec     hl
        djnz    .l
        dec     c
        jr      nz, .l
        jp      (ix)
.f      FAILBITS
        jr      .c

; test 8: HL from, C halves of 128 bytes (0 = 256), 00 and FF in turns
ref_fill:
        ld      d, 0
.l      ld      b, 128
.h      ld      (hl), d
        inc     hl
        djnz    .h
        ld      a, d
        cpl
        ld      d, a
        dec     c
        jr      nz, .l
        jp      (ix)

; the same read back: the bad bits into E, the first bad address into IY
ref_check:
        ld      d, 0
.l      ld      b, 128
.h      ld      a, (hl)
        cp      d
        jr      nz, .f
.c      inc     hl
        djnz    .h
        ld      a, d
        cpl
        ld      d, a
        dec     c
        jr      nz, .l
        jp      (ix)
.f      FAILBITS
        jr      .c

; the border C, a beep (high and short, or low and long for red: C = 2),
; then D times about 0.5 s; no RAM, no stack: JP (IX) back
beephold:
        ld      a, c
        out     (0FEh), a
        ld      hl, 200             ; cycles (even: C ends as it was)
        ld      e, 60               ; half period
        cp      2
        jr      nz, .bp
        ld      hl, 300
        ld      e, 200
.bp     ld      a, c
        xor     10h
        ld      c, a
        out     (0FEh), a
        ld      b, e
.bd     djnz    .bd
        dec     hl
        ld      a, h
        or      l
        jr      nz, .bp
        ld      a, c
        and     7
        out     (0FEh), a
hold:   ld      hl, 0
.l      dec     hl
        ld      a, h
        or      l
        jr      nz, .l
        dec     d
        jr      nz, .l
        jp      (ix)

walk_tab:
        db      01h, 02h, 04h, 08h, 10h, 20h, 40h, 80h
        db      0FEh, 0FDh, 0FBh, 0F7h, 0EFh, 0DFh, 0BFh, 07Fh
        db      00h, 0FFh

; ---- the results on the screen (1400) ------------------------------------
        ds      1400h - $, 0FFh
show:
        ; test 8's first half: the upper RAM filled now, read back after the hold
        START   8
        exx
        ld      a, b
        exx
        bit     1, a
        jr      z, .nofill
        ld      hl, 8000h
        ld      c, 0                ; 256 halves: 8000-FFFF
        CALLX   ref_fill
.nofill exx
        ld      a, b
        exx
        bit     0, a
        jp      z, .noscr
        ld      sp, STACKL
        ; test 8's line: WAIT till it's read back (NONE with no upper RAM)
        ld      a, 2
        exx
        bit     1, b
        exx
        jr      z, .w8
        inc     a
.w8     ld      (SAVE + 4 * 8 + 1), a
        ; the march left the screen's pixels 0: white ink, black paper
        ld      hl, 5800h
        ld      de, 5801h
        ld      bc, 767
        ld      (hl), 07h
        ldir
        ld      hl, title
        ld      de, 4000h + 32 + 1
        call    print
        exx
        ld      a, d
        exx
        ld      de, 4000h + 32 + 26
        call    hex
        ; one line per test
        ld      b, 0
.t      call    line
        inc     b
        ld      a, b
        cp      NTESTS
        jr      nz, .t
.noscr
        ; all OK? statuses 0 -> green; the upper RAM's 2 (none) -> yellow
        ; (right on a 16K only); a FAIL -> red
        exx
        ld      a, c                ; test 2's status
        exx
        ld      e, a
        exx
        ld      a, b                ; the RAM flags
        exx
        ld      c, 4                ; green
        bit     0, a
        jr      z, .bad
        ld      a, e
        or      a
        jr      nz, .bad
        ld      hl, SAVE + 4 * 4 + 1
        ld      b, 4                ; tests 4-7
.ck     ld      a, (hl)
        cp      1
        jr      z, .bad
        inc     l
        inc     l
        inc     l
        inc     l
        djnz    .ck
        ld      hl, SAVE + 4 * 9 + 1
        ld      b, 3                ; tests 9-11
.c2     ld      a, (hl)
        cp      1
        jr      z, .bad
        inc     l
        inc     l
        inc     l
        inc     l
        djnz    .c2
        ld      a, (SAVE + 4 * 4 + 1)
        cp      2
        jr      nz, .beep
        ld      c, 6                ; yellow: no upper RAM
        jr      .beep
.bad    ld      c, 2                ; red
.beep   ; a beep, then about 1.5 s to read it (no RAM, no stack)
        ld      d, 3
        CALLX   beephold

; ---- test 8, the second half: the upper RAM read back ---------------------
t8:
        exx
        ld      a, b
        exx
        bit     1, a
        jr      nz, .st
        ld      d, 2                ; the upper RAM failed or isn't there
        ld      e, 0
        ld      hl, 0
        jp      .r
.st     ld      e, 0
        ld      iy, 0
        ld      hl, 8000h
        ld      c, 0
        CALLX   ref_check
        ld      d, 0
        ld      a, e
        or      a
        jr      z, .ok
        ld      d, 1
.ok     ld      a, iyh
        ld      h, a
        ld      a, iyl
        ld      l, a
.r
        RESULT  8
        ld      c, d                ; the status (the line keeps BC)
        exx
        ld      a, b
        exx
        bit     0, a
        jr      z, .ns
        ld      b, 8
        call    line                ; WAIT -> OK / FAIL
.ns     ld      a, c
        dec     a
        ld      d, 1                ; OK or none: 0.5 s to see it
        jr      nz, .h
        ld      c, 2                ; FAIL: red, the low beep, 1.5 s
        ld      d, 3
        CALLX   beephold
        jr      .end
.h      CALLX   hold
.end    exx
        inc     d
        ld      a, d
        exx
        ld      (MARK + 0Fh), a     ; the loop's end
        jp      t0

; test B's line on the screen (row 3 + B) from SAVE + 4B; keeps BC
line:   push    bc
        ld      a, b
        add     a, 3
        call    rowaddr             ; DE = the row's first cell
        inc     e
        ld      a, b
        add     a, '0'
        call    putc
        inc     e
        inc     e
        ld      a, b                ; the name: names + 10 n
        ld      l, a
        add     a, a
        add     a, a
        add     a, l
        add     a, a
        ld      l, a
        ld      h, 0
        push    de
        ld      de, names
        add     hl, de
        pop     de
        call    print
        ld      a, b
        add     a, a
        add     a, a
        ld      l, a
        ld      h, SAVE >> 8
        ; L -> SAVE + 4n: (E bits, D status, L, H)
        push    hl
        inc     l
        ld      a, (hl)             ; the status, and the row's paper (C)
        ld      hl, w_ok
        ld      c, 07h              ; white ink, black paper
        or      a
        jr      z, .st
        ld      hl, w_fail
        ld      c, 17h              ; red paper
        dec     a
        jr      z, .st
        ld      hl, w_none
        ld      c, 30h              ; yellow paper, black ink
        dec     a
        jr      z, .st
        ld      hl, w_wait
        ld      c, 07h
.st     call    print
        pop     hl
        ld      a, (hl)             ; the bits
        inc     e
        call    hex
        inc     l
        inc     l
        inc     l
        ld      a, (hl)             ; high byte
        inc     e
        call    hex
        dec     l
        ld      a, (hl)
        call    hex
        ld      a, b
        add     a, 3
        call    rowaddr
        ld      a, d                ; the row's attributes: 5800 + 32 row
        and     18h
        rrca
        rrca
        rrca
        or      58h
        ld      h, a
        ld      l, e
        ld      b, 32
.a      ld      (hl), c
        inc     hl
        djnz    .a
        pop     bc
        ret

; DE = the first cell of text row A (0-23)
rowaddr:
        push    af
        and     18h
        or      40h
        ld      d, a
        pop     af
        and     7
        rrca
        rrca
        rrca
        ld      e, a
        ret

; print the 0-ended string at HL at DE (DE moves on)
print:  ld      a, (hl)
        or      a
        ret     z
        call    putc
        inc     hl
        inc     e
        jr      print

; A as two hex digits at DE (DE moves on)
hex:    push    af
        rrca
        rrca
        rrca
        rrca
        call    .d
        pop     af
.d      and     0Fh
        add     a, '0'
        cp      '9' + 1
        jr      c, .p
        add     a, 7
.p      call    putc
        inc     e
        ret

; character A at DE (DE unchanged)
putc:   push    hl
        push    de
        push    bc
        sub     32
        ld      l, a
        ld      h, 0
        add     hl, hl
        add     hl, hl
        add     hl, hl
        ld      bc, font
        add     hl, bc
        ld      b, 8
.l      ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     d
        djnz    .l
        pop     bc
        pop     de
        pop     hl
        ret

title:  db      "ZX LA TEST ", '0' + VERSION, "         LOOP", 0
names:  db      "ALIVE    ", 0
        db      "ADDRESS  ", 0
        db      "ROM CRC  ", 0
        db      "LOWER RAM", 0
        db      "UPPER RAM", 0
        db      "ULA PORTS", 0
        db      "INTERRUPT", 0
        db      "CONTENTN ", 0
        db      "REFRESH  ", 0
        db      "LOW ADDR ", 0
        db      "UP ADDR  ", 0
        db      "EAR IN   ", 0
w_ok:   db      " OK  ", 0
w_fail: db      " FAIL", 0
w_none: db      " NONE", 0
w_wait: db      " WAIT", 0

; ---- test 2's CRC tables (1800, 1900: a page each) -----------------------
; crc_hi[i], crc_lo[i]: the CRC-16 (poly 1021) of i << 8 after 8 shifts.
        ds      1800h - $, 0FFh
crc_hi:
.i = 0
        DUP     256
.c = .i << 8
        DUP     8
.c = ((.c << 1) ^ (((.c >> 15) & 1) * 1021h)) & 0FFFFh
        EDUP
        db      .c >> 8
.i = .i + 1
        EDUP
crc_lo:
.i = 0
        DUP     256
.c = .i << 8
        DUP     8
.c = ((.c << 1) ^ (((.c >> 15) & 1) * 1021h)) & 0FFFFh
        EDUP
        db      .c & 0FFh
.i = .i + 1
        EDUP

; ---- the font (2000) -----------------------------------------------------
        ds      2000h - $, 0FFh
font:
        INCLUDE "font.inc"

; ---- the IM 2 handler and table ------------------------------------------
        ds      (IM2V << 8 | IM2V) - $, 0FFh
int_im2:
        INTBACK
        ds      IM2TAB - $, 0FFh
        ds      257, IM2V

        ds      CSUM - $, 0FFh
        dw      0                   ; patched by patchsum.py
        ds      4000h - $, 0FFh
