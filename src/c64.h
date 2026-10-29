/* c64.h - the Commodore 64 boards, ASSY 250407 and 250469: netlist + part
 * models + the four big chip cores (6510, VIC-II, two CIAs, SID), and the
 * bus cycle that moves them. Shared by c64.c, cart.c and harness.c. */
#ifndef C64_H
#define C64_H
#include "board.h"
#include "chips_common.h"
#include "m6502.h"
#include "m6526.h"
#include "m6569.h"
#include "m6581.h"
#include "kbd.h"

#define C64_FREQ        985248          /* PAL CPU clock, Hz */
#define C64_CYCLES_PER_FRAME (63 * 312) /* PAL: 63 cycles per line, 312 lines */
#define C64_SCREEN_W    392
#define C64_SCREEN_H    272
#define C64_STEPS_PER_CYCLE 6           /* two halves, three bus steps each */

/* Bus steps of each half cycle (see c64.c for the full story). */
enum { STEP_ROW, STEP_RAS, STEP_CAS };

/* Special C64 keys (same codes as c64-llm). */
#define C64_KEY_SPACE    0x20
#define C64_KEY_CSRLEFT  0x08
#define C64_KEY_CSRRIGHT 0x09
#define C64_KEY_CSRDOWN  0x0A
#define C64_KEY_CSRUP    0x0B
#define C64_KEY_DEL      0x01
#define C64_KEY_INST     0x10
#define C64_KEY_HOME     0x0C
#define C64_KEY_CLR      0x02
#define C64_KEY_RETURN   0x0D
#define C64_KEY_CTRL     0x0E
#define C64_KEY_CBM      0x0F
#define C64_KEY_RESTORE  0xFF
#define C64_KEY_STOP     0x03
#define C64_KEY_F1       0xF1
#define C64_KEY_F3       0xF3
#define C64_KEY_F5       0xF5
#define C64_KEY_F7       0xF7

typedef struct c64 {
    board_t board;              /* first: a c64_t is a board_t */
    /* Chip cores and their parts on the board. */
    m6502_t cpu;    uint64_t cpu_pins;  int p_cpu;
    m6569_t vic;    int p_vic;
    m6526_t cia[2]; int p_cia[2];
    m6581_t sid;    int p_sid;
    int p_osc, p_y1, p_cn1;     /* the clock oscillator (U31, the 250469's U20) */
    int dram_part[8];           /* the DRAM holding each data bit */
    /* The 250469's two 41464s: the one holding D0-D3, the one holding
     * D4-D7, and the MA line on each of their address pins A0-A7. */
    int dram4_part[2], dram4_ma[8];
    int pin_cn1_pa[8], pin_cn1_pb[8], pin_restore, net_gnd;
    /* State of the current bus half cycle. */
    int phase;                  /* 1 = phi1 (VIC), 2 = phi2 (CPU or VIC) */
    int step;                   /* STEP_* */
    bool vic_owns;              /* the VIC drives the address bus */
    uint16_t vic_addr;          /* 14 bit address of the VIC access */
    int vic_fetches;            /* VIC accesses done in this cycle */
    bool vic_steal;             /* VIC owns phi2 too (AEC low) */
    int ba_streak;              /* cycles BA has been low */
    bool vic_ba_low, vic_irq;   /* VIC outputs */
    uint8_t vic_refresh;        /* DRAM refresh counter */
    uint8_t vic_reg;            /* register address latched when /RAS falls */
    /* Read data a chip core puts on the data bus at the end of phi2:
     * index 0 = VIC, 1 = CIA1, 2 = CIA2, 3 = SID. */
    bool dout_en[4]; uint8_t dout_val[4];
    bool cia_irq[2];
    uint8_t cia_outs[2];        /* /PC, SP, CNT as last driven */
    bool cpu_ticked;            /* the CPU core advanced in this cycle */
    /* Caches for part_drive_bus_cached() (speed only, see parts.h). */
    int32_t cache_cpu_addr, cache_cpu_data, cache_cpu_port, cache_vic_mux, cache_vic_addr,
            cache_vic_data, cache_cia_pa[2], cache_cia_pb[2], cache_cia_data[2], cache_sid_data;
    /* Clock detection: level of each clock pin in phi1 and phi2. */
    uint8_t seen_phi0[2], seen_dot[2], seen_color[2], seen_phi2[3][2];
    bool cpu_running, vic_running, color_ok;
    bool cpu_in_reset;
    uint64_t vic_last_tick;
    /* Keyboard: host keys and the switch matrix they close. */
    kbd_t kbd;
    uint16_t key_matrix[9];
    /* The mains, as the board sees it through R5 and R100, and the
     * cassette motor driver (see c64.c). */
    int pin_mains, p_r5, p_r100, pin_motor, pin_motor_ctl;
    /* The 250469's power-on reset: C43 holds the 74LS14 input low until
     * this cycle (see c64.c). */
    int pin_por, p_por;
    uint64_t por_until;
    uint64_t mains_next, mains_edges;     /* next flip of the mains, flips so far */
    /* The power path (see c64_power): its parts, whether the 9 V AC gets
     * through, and whether the board was powered at the last rebuild (when
     * the power comes back, the C64 starts cold, as switched on). */
    int p_sw1, p_cn7, p_l5, p_l4, p_f1, p_cr4, p_cr5, p_cr6, p_c90, p_vr1, p_vr2, p_q5;
    bool ac_ok, powered, cold_start;
    /* The 586220 diagnostic harness (harness.c): plugged, and the level of
     * WRITE that sets its switches. */
    bool harness, harness_write;
    int pin_hmotor, pin_hwrite, p_u28;
    /* The cartridge in the expansion port (cart.c), NULL if none. */
    void *cart;
    int p_cn6;
} c64_t;

static inline c64_t *C64(board_t *b) { return (c64_t *)b; }
/* A C64 board, of either assembly? The 250469 (the C64C's, with the Super
 * PLA) is the other one. */
static inline bool board_is_c64(const board_t *b) { return b->m == &machine_c64 || b->m == &machine_c64c; }
static inline bool board_is_250469(const board_t *b) { return b->m == &machine_c64c; }

/* Load a .prg straight into the DRAM chips (a "backdoor", like a freezer
 * cartridge: it works even on a broken machine). Optionally type RUN. */
bool c64_load_prg(board_t *b, const char *path, bool run, char *err, int errlen);
/* Plug a .CRT cartridge into the expansion port, or pull it out. Both
 * switch the C64 off and on again, as you would. 'msg' tells what was
 * loaded or why not. The name is NULL without a cartridge (or a C64). */
bool c64_insert_cart(board_t *b, const char *path, char *msg, int msglen);
void c64_remove_cart(board_t *b);
const char *c64_cart_name(board_t *b);
/* Raw .rom/.bin images (cart.c). Several 8K banks are a DesTestSwitch
 * EPROM: how many (0 for none or a .crt), the one selected, a line about
 * one, and flipping the switch. An 8K image goes in the expansion port or
 * in the KERNAL socket U4 (on the 250469 with BASIC below it, as one
 * 27128); a 16K BASIC + KERNAL image only in U4: c64_cart_socket() is 1
 * in U4, 0 in the port, -1 if it can't move. All of them restart the C64. */
int c64_cart_banks(board_t *b);
int c64_cart_bank(board_t *b);
const char *c64_cart_bank_info(board_t *b, int bank);
void c64_set_cart_bank(board_t *b, int bank);
int c64_cart_socket(board_t *b);
bool c64_cart_in_u4(board_t *b);
void c64_set_cart_socket(board_t *b, bool u4);
/* What a raw image is, a line for the file browser (false: not one the C64
 * takes), and whether it looks like a C64 ROM rather than a Spectrum one. */
bool c64_raw_rom_info(const uint8_t *buf, size_t n, char *info, int len);
/* The image in the KERNAL socket U4, NULL for the stock one: 8K on the
 * 250407, 16K (BASIC + KERNAL, see c64_basic_kernal()) on the 250469. */
void c64_set_kernal(board_t *b, const uint8_t *image);
/* BASIC and an 8K KERNAL, as one 16K image for the 250469's U4. */
void c64_basic_kernal(uint8_t out[16384], const uint8_t *kernal);
/* Plug or unplug the 586220 diagnostic harness (harness.c); the C64 is
 * switched off and on. board_harness() is false on other machines. */
void board_set_harness(board_t *b, bool plugged);
bool board_harness(board_t *b);
/* For c64.c: the harness connections, its per-cycle work, and the value
 * the SID reads on a POT pin. */
void harness_joins(board_t *b);
void harness_tick(board_t *b);
int board_pot_value(board_t *b, int sid_pin);
/* The character ROM (U5): 2K upper case and graphics, 2K lower case. */
const uint8_t *c64_char_rom(void);

#endif
