; zxbench.asm - bench check of the SteamCard Pro before the test ROM
; (to-do.md section 13, step 1). AI-generated (Claude Code).
;
; A 16K cartridge ROM for a 16K/48K Spectrum. It uses no RAM, no stack and
; no interrupts, and loops for ever through three signposts the logic
; analyser can be armed on (each is the address of the first bus cycle):
;
;   0200  writes: 20 writes into the ROM area, 3F80-3F93, data
;         00 FF 55 AA, 01 02 04 .. 80 (walking 1), FE FD FB .. 7F (walking 0)
;   0300  reads:  two reads of 3F80 and 3F81 (the cartridge's FF fill:
;         does it still answer the same after the writes?)
;   0400  ports:  the same 20 values OUT to port (80..93)BB: an odd port,
;         nothing on a 48K answers it; B = 80+n is A8-A15
;
; At the bench: does the border keep cycling slowly (the cartridge didn't
; page on the writes)? In the capture: the writes' data on D0-D7 exactly
; as listed (nobody else driving the bus during a write), /ROMCS high.
; Assemble: sjasmplus zxbench.asm (writes zxbench.rom, 16384 bytes).

        OUTPUT  "zxbench.rom"

MBOX    EQU     3F80h           ; the mailbox: 3F80-3F9F, nothing else there
PORT    EQU     0BBh            ; the odd port's low byte

        ORG     0
        di
        jp      start

        ds      38h - $, 0FFh
        jp      start           ; IM 1 interrupt (never: DI)
        ds      66h - $, 0FFh
        jp      start           ; NMI: start again (no stack to RETN with)

        ds      70h - $, 0FFh
        db      "ZXBENCH 1 - SteamCard Pro bench check", 0

        ds      100h - $, 0FFh
start:
        ld      a, 2            ; red border: we're running
        out     (0FEh), a
        ld      de, 0           ; E counts loops, D the border colour
        jp      sp_write

; ---- 0200: writes into the ROM area --------------------------------------
        ds      200h - $, 0FFh
sp_write:
        ld      a, 00h
        ld      (MBOX + 0), a
        ld      a, 0FFh
        ld      (MBOX + 1), a
        ld      a, 55h
        ld      (MBOX + 2), a
        ld      a, 0AAh
        ld      (MBOX + 3), a
.n      = 0
        DUP     8
        ld      a, 1 << .n
        ld      (MBOX + 4 + .n), a
.n      = .n + 1
        EDUP
.n      = 0
        DUP     8
        ld      a, 0FFh ^ (1 << .n)
        ld      (MBOX + 12 + .n), a
.n      = .n + 1
        EDUP
        jp      sp_read

; ---- 0300: read the mailbox back -----------------------------------------
        ds      300h - $, 0FFh
sp_read:
        ld      a, (MBOX + 0)
        ld      a, (MBOX + 1)
        jp      sp_out

; ---- 0400: the same values to an odd port --------------------------------
        ds      400h - $, 0FFh
sp_out:
        ld      c, PORT
        ld      b, 80h
        ld      a, 00h
        out     (c), a
        inc     b
        ld      a, 0FFh
        out     (c), a
        inc     b
        ld      a, 55h
        out     (c), a
        inc     b
        ld      a, 0AAh
        out     (c), a
.n      = 0
        DUP     8
        inc     b
        ld      a, 1 << .n
        out     (c), a
.n      = .n + 1
        EDUP
.n      = 0
        DUP     8
        inc     b
        ld      a, 0FFh ^ (1 << .n)
        out     (c), a
.n      = .n + 1
        EDUP

; The border's next colour every 512 loops (about 7 a second): it shows
; the loop is still running.
        inc     e
        jp      nz, sp_write
        inc     d
        ld      a, d
        rra
        and     7
        out     (0FEh), a
        jp      sp_write

        ds      4000h - $, 0FFh
