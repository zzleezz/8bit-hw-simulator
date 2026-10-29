/* zx48.h - the Sinclair ZX Spectrum 48K, boards Issue 3B, 4B, 2 and 1:
 * netlist + part models + the Z80 core and the ULA model, and the clock
 * that moves them. */
#ifndef ZX48_H
#define ZX48_H
#include "board.h"
#include "chips_common.h"
#include "z80.h"
#include "kbd.h"
#include "ula.h"

#define ZX_FREQ             3500000         /* CPU clock, Hz */
#define ZX_CYCLES_PER_FRAME (224 * 312)     /* T states per frame */
#define ZX_STEPS_PER_CYCLE  8               /* half periods of the 14 MHz oscillator */

typedef struct zx48 {
    board_t board;              /* first: a zx48_t is a board_t */
    z80_t cpu;
    uint64_t cpu_pins;
    int p_cpu, p_ula, p_rom;
    int pin_clk;                /* the Z80's CLK input */
    int clk;                    /* its level at the last step */
    bool in_reset, cpu_running;
    uint64_t last_edge;         /* cycle of the last clock edge */
    int32_t cache_addr, cache_data;
    uint64_t changes_seen;      /* sim.changes at the last commit */
    int lram_part[8], uram_part[8]; /* the DRAM of each data bit (-1: none,
                                       the Issue 1 has no upper RAM) */
    int *ram_cells[2];          /* DRAM cell of each lower/upper RAM address
                                   (zx_ram_poke, computed on first use) */
    /* Keyboard: the half rows (KB2 pads, one per address line A8-A15
     * through D1-D8) and the lines into the ULA (KB1 pads, KB0-KB4). */
    kbd_t kbd;
    uint16_t key_matrix[5];
    int pin_kb_row[8], pin_kb_line[5];
    /* Power: the 9 V input, and the DC-DC converter's +12V and -5V. */
    int pin_9v, pin_12v, pin_m5v;
    /* A ROM cartridge in the edge connector (zx48.c), NULL if none. */
    void *cart;
    /* Power on reset: C27 holds /RESET low for a while. */
    int pin_reset_cap;
    uint64_t reset_until;
    /* How the board is built (machine_option_t): 16K (the upper RAM and
     * its glue not fitted), and which ULA (the index in the values of the
     * board's "ula" option: Issue 2 5C112 6C001, Issue 1 5C102 5C112
     * 6C001). */
    bool ram16k;
    int ula_type;
    /* The upper RAM: which chips (an index in zx_uram_chips: TI 4532-3,
     * TI 4532-4, OKI 3732-L, OKI 3732-H, 4164) and how its links are set
     * (an index in the values of the board's "ulinks" option). */
    int uram_chips, uram_links;
    /* The ROM: which chip (0 NEC uPD23128, 1 Hitachi HN613128) and how
     * its link is set (0 N, 1 H): zx_rom_fitted(). */
    int rom_chip, rom_link;
} zx48_t;

static inline zx48_t *ZX(board_t *b) { return (zx48_t *)b; }

extern const machine_t machine_zx48, machine_zx48i2, machine_zx48i1, machine_zx48i4b, machine_zx48i6a;
/* A Spectrum board, of any issue? */
static inline bool board_is_zx48(const board_t *b) {
    return b->m == &machine_zx48 || b->m == &machine_zx48i2 || b->m == &machine_zx48i1 || b->m == &machine_zx48i4b ||
           b->m == &machine_zx48i6a;
}
/* The 48K ROM (its character set is at 0x3D00, characters 32-127). */
const uint8_t *zx48_rom_image(void);

#endif
