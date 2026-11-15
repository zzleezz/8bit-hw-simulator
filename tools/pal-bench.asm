; Atari 2600 PAL bench diagnostic, 2 KiB linear cartridge.
; Build from the repository root:
;     dasm tools/pal-bench.asm -f3 -ocarts/pal-bench.a26

        processor 6502

VSYNC   = $00
VBLANK  = $01
WSYNC   = $02
COLUBK  = $09
PF0     = $0D
PF1     = $0E
PF2     = $0F
AUDV0   = $19
AUDV1   = $1A
GRP0    = $1B
GRP1    = $1C
ENAM0   = $1D
ENAM1   = $1E
ENABL   = $1F
VDELP0  = $25
VDELP1  = $26
VDELBL  = $27
Ram     = $80

        org $F800
Reset:
        sei
        cld
        ldx #$FF
        txs
        lda #2
        sta VBLANK
        lda #0
        sta VSYNC
        sta COLUBK
        sta PF0
        sta PF1
        sta PF2
        sta VDELP0
        sta VDELP1
        sta VDELBL
        sta GRP0
        sta GRP1
        sta ENAM0
        sta ENAM1
        sta ENABL
        sta AUDV0
        sta AUDV1

        ; Fill the whole RIOT RAM before checking it to detect address aliasing.
        ; Test each data bit with both polarities. Keep all state in registers
        ; so a failed RAM test can still draw a diagnostic pattern.
        ldy #0
TestRam:
        ldx #127
FillRam:
        txa
        eor RamSeeds,y
        sta Ram,x
        dex
        bpl FillRam
        ldx #127
CheckRam:
        txa
        eor RamSeeds,y
        cmp Ram,x
        bne RamFailed
        dex
        bpl CheckRam
        iny
        cpy #2
        bne TestRam
        ldy #0
        lda #$A5
        bne Result
RamFailed:
        ldy #12
        lda #$5A
Result:
        sta Ram

        ; Finish the previous overscan line before asserting VSYNC at a
        ; fixed point in the new line. Each WSYNC advances to the next line.
Frame:
        lda #2
        sta WSYNC
        sta VSYNC
        sta WSYNC
        sta WSYNC
        sta WSYNC
        lda #0
        sta VSYNC

        ldx #45
VerticalBlank:
        sta WSYNC
        dex
        bne VerticalBlank
        sta VBLANK

Bar:
        lda BarColours,y
        sta COLUBK
        ldx #18
BarLines:
        sta WSYNC
        dex
        bne BarLines
        iny
        cpy #12
        beq Overscan
        cpy #24
        beq Overscan
        sta WSYNC
        jmp Bar

Overscan:
        sta WSYNC
        lda #2
        sta VBLANK
        ldx #35
OverscanLines:
        sta WSYNC
        dex
        bne OverscanLines
        tya
        sec
        sbc #12
        tay
        jmp Frame

RamSeeds:
        dc.b $55, $AA
BarColours:
        dc.b $0E, $2C, $3A, $48, $58, $68, $78, $88, $98, $A8, $B8, $C8
        dc.b $00, $0E, $00, $0E, $00, $0E, $00, $0E, $00, $0E, $00, $0E

        org $FFFA
        dc.w Reset, Reset, Reset
