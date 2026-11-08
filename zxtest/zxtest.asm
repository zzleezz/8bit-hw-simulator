; zxtest.asm - a test ROM for 16K/48K Spectrums, made to be watched by the
; logic analyser on the edge connector (to-do.md section 13).
; AI-generated (Claude Code). Assemble with build.sh (sjasmplus, then the
; ROM checksum patched in by patchsum.py).
;
; A 16K cartridge ROM (the SteamCard Pro's bank 3). It loops for ever
; through eight tests. Each test:
;   - starts by writing its number to its marker, MARK+n (3FA0-3FA7): the
;     analyser arms on that address;
;   - sets the border to its number;
;   - ends by writing four result bytes to RES+4n (3FC0-3FDF):
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
;   0 alive     20 writes: 00 FF 55 AA, walking 1, walking 0 (3F80-3F93)
;   1 address   reads with A0-A15 walking 1 then walking 0 (for the analyser)
;   2 ROM sum   16-bit sum of 0000-3F7F (bits 00, +2 +3 = the sum)
;   3 lower RAM 4000-7FFF: data walk at 4000, then a march
;               (w0 up, r0w1 up, r1w0 down, r0 down); +1 the bad bits,
;               +2 +3 the first bad address
;   4 upper RAM 8000-FFFF: there at all?, then the same march
;   5 ULA ports OUT FE each border, MIC, EAR value; IN FE each keyboard
;               row, no key pressed: +1 key bits read low, +2 the rows they
;               were on, +3 IN FE with every row selected
;   6 interrupts IM 1: the frame's length (+2 +3 = loop counts between two
;               interrupts); IM 2 (table at 3B00, handler at 3A3A).
;               +1 bit 0 no IM 1 interrupt, bit 1 no IM 2, bit 2 the
;               frame's length out of range
;   7 contention the same count with a read of 4000 in the loop (the ULA
;               holds the CPU's clock while it fetches the screen)
; Then the results on the screen (if the lower RAM passed), the border
; green (all OK) or red, a beep, about 1.5 s to read it, and again.
; MARK+0F (3FAF) = the loop's end, data = the loop count.

        OUTPUT  "zxtest.rom"

MARK    EQU     3FA0h           ; test n's marker
RES     EQU     3FC0h           ; test n's four result bytes
ALIVE   EQU     3F80h           ; test 0's writes
CSUM    EQU     3F7Eh           ; the ROM's sum (patched in), not counted
SAVE    EQU     5B00h           ; the results kept in the lower RAM
STACKL  EQU     7FF0h
STACKU  EQU     0FFF0h
IM2TAB  EQU     3B00h           ; 257 bytes of IM2V
IM2V    EQU     3Ah             ; the handler is at 3A3A
WAITN   EQU     6000            ; a wait loop's count: about 2 frames
; The counts of a working 48K (from the simulator; the wait loop is 24 T
; a turn, so a 69888 T frame is 2912 turns): +-8 allowed.
FRAME_N EQU     2912
CONT_N  EQU     2209            ; measured on the simulated Issue 2 (no contention: 2254)
NTOL    EQU     8

; shadow registers: B' bit 0 lower RAM OK, bit 1 upper RAM OK;
; C' test 2's status; HL' test 2's sum; D' the loop count.

; ---- macros ---------------------------------------------------------------
; Call a subroutine that returns with JP (IX) (no stack).
        MACRO   CALLX addr
        ld      ix, $ + 7
        jp      addr
        ENDM

; A test's start: the marker write and the border.
        MACRO   START n
        ld      a, n
        ld      (MARK + n), a
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
        jr      z, $ + 9            ; skip the two saves (3 + 4 bytes)
        ld      (SAVE + 4 * n + 2), hl
        ld      (SAVE + 4 * n), de
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
        jr      nz, $ + 8           ; IY has one already (skip 6 bytes)
        ld      a, h
        ld      iyh, a
        ld      a, l
        ld      iyl, a
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

; ---- 0000 ----------------------------------------------------------------
        ORG     0
        di
        jp      reset

        ds      38h - $, 0FFh
int_im1:                        ; test 6/7 only: back to where IX says
        inc     sp
        inc     sp
        jp      (ix)

        ds      66h - $, 0FFh
        jp      reset           ; NMI: start again

        ds      70h - $, 0FFh
        db      "ZXTEST 1 - logic analyser test ROM", 0

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
        ld      hl, 0
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

; ---- test 2: the ROM's sum (0400) ----------------------------------------
        ds      400h - $, 0FFh
t2:
        START   2
        ld      hl, 0
        ld      de, 0
        ld      bc, 003Fh           ; 63 pages (B = 0: 256 a page), then 3F00-3F7F
.l      ld      a, (hl)
        add     a, e
        ld      e, a
        jr      nc, .n
        inc     d
.n      inc     hl
        djnz    .l
        dec     c
        jr      nz, .l
        ld      a, l
        or      a
        jr      nz, .sd             ; L = 80: 3F00-3F7F done too
        ld      bc, 8001h           ; 128 bytes, one turn
        jr      .l
.sd     ; take out the two bytes of the stored sum
        ld      a, (CSUM)
        ld      l, a
        ld      h, 0
        ex      de, hl
        or      a
        sbc     hl, de
        ld      a, (CSUM + 1)
        ld      e, a
        ld      d, 0
        or      a
        sbc     hl, de
        ; HL = the sum; compare with the stored one
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
        ld      bc, 0
        ld      (SAVE + 0), bc
        ld      (SAVE + 2), bc
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
        ; there at all? 55 to 8000, AA to 8001: one read back right is enough
        ld      a, 55h
        ld      (8000h), a
        cpl
        ld      (8001h), a
        ld      a, (8000h)
        cp      55h
        jr      z, .yes
        ld      a, (8001h)
        cp      0AAh
        jr      z, .yes
        ld      d, 2
        ld      e, 0
        ld      hl, 0
        jp      .r
.yes    ld      e, 0
        ld      iy, 0
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

walk_tab:
        db      01h, 02h, 04h, 08h, 10h, 20h, 40h, 80h
        db      0FEh, 0FDh, 0FBh, 0F7h, 0EFh, 0DFh, 0BFh, 07Fh
        db      00h, 0FFh

; ---- the results on the screen (1400) ------------------------------------
        ds      1400h - $, 0FFh
show:
        exx
        ld      a, b
        exx
        bit     0, a
        jp      z, .noscr
        ld      sp, STACKL
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
.t      push    bc
        ld      a, b
        add     a, 3                ; row 3 + n
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
        ld      a, (hl)             ; the status
        ld      hl, w_ok
        or      a
        jr      z, .st
        ld      hl, w_fail
        dec     a
        jr      z, .st
        ld      hl, w_none
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
        ; a failed line on red paper
        ld      a, b
        add     a, a
        add     a, a
        ld      l, a
        inc     l
        ld      h, SAVE >> 8
        ld      a, (hl)
        dec     a
        jr      nz, .nored
        ld      a, b
        add     a, 3
        call    rowaddr
        ld      a, d                ; the row's attributes: 5800 + 32 row
        and     18h
        rrca
        rrca
        rrca
        or      58h
        ld      d, a
        ld      h, d
        ld      l, e
        ld      c, 32
.red    ld      (hl), 17h
        inc     hl
        dec     c
        jr      nz, .red
.nored  pop     bc
        inc     b
        ld      a, b
        cp      8
        jp      nz, .t
.noscr
        ; all OK? statuses 0 (or 2 for the upper RAM: a 16K) -> green
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
        jr      .beep
.bad    ld      c, 2                ; red
.beep   ; a beep: high and short if OK, low and long if not
        ld      a, c
        out     (0FEh), a
        ld      hl, 200             ; cycles
        ld      e, 60               ; half period (OK)
        cp      4
        jr      z, .bp
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
        ; about 1.5 s to read it (no RAM, no stack)
        ld      hl, 0
        ld      d, 3
.hold   dec     hl
        ld      a, h
        or      l
        jr      nz, .hold
        dec     d
        jr      nz, .hold
        exx
        inc     d
        ld      a, d
        exx
        ld      (MARK + 0Fh), a     ; the loop's end
        jp      t0

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

title:  db      "ZX LA TEST 1         LOOP", 0
names:  db      "ALIVE    ", 0
        db      "ADDRESS  ", 0
        db      "ROM SUM  ", 0
        db      "LOWER RAM", 0
        db      "UPPER RAM", 0
        db      "ULA PORTS", 0
        db      "INTERRUPT", 0
        db      "CONTENTN ", 0
w_ok:   db      " OK  ", 0
w_fail: db      " FAIL", 0
w_none: db      " NONE", 0

; ---- the font (2000) -----------------------------------------------------
        ds      2000h - $, 0FFh
font:
        INCLUDE "font.inc"

; ---- the IM 2 handler and table ------------------------------------------
        ds      (IM2V << 8 | IM2V) - $, 0FFh
int_im2:
        inc     sp
        inc     sp
        jp      (ix)
        ds      IM2TAB - $, 0FFh
        ds      257, IM2V

        ds      CSUM - $, 0FFh
        dw      0                   ; patched by patchsum.py
        ds      4000h - $, 0FFh
