/* parts.c - pin-level models of the C64 glue logic and memories.
 *
 * Every model follows the same pattern:
 *   eval()   reads input pins (pread) and drives output pins (pdrive).
 *            It must have no side effects other than driving pins.
 *   commit() is called after the board settled, at every bus step: this is
 *            where latches and memories store what they see (clock edges).
 *
 * Pin numbers always follow the chip datasheet, so the code can be checked
 * against a pinout diagram. */
#include "parts.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */
/* ------------------------------------------------------------------------ */

void part_set_type(sim_t *s, int part, const part_type_t *t) {
    part_t *p = &s->parts[part];
    p->name = t->name;
    p->eval = t->eval;
    p->commit = t->commit;
    p->reset = t->reset;
    p->tech = (uint8_t)t->tech;
    if (t->state_size) p->state = calloc(1, t->state_size);
    p->state_size = (uint32_t)t->state_size;
    /* Parse "NAME:K NAME:K ..." into pin names and flags. */
    char *copy = strdup(t->pins), *save = NULL;
    int number = 1;
    for (char *tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save), number++) {
        char *colon = strchr(tok, ':');
        uint8_t flags = 0;
        if (colon) {
            *colon = 0;
            switch (colon[1]) {
            case 'T': flags = PIN_TTL | PIN_SENSE | PIN_IN; break;
            case 'M': flags = PIN_SENSE | PIN_IN; break;
            case 'm': flags = PIN_IN; break;
            case 'O': flags = PIN_OUT; break;
            case 'B': flags = PIN_IN | PIN_OUT; break;
            case 'X': flags = PIN_SENSE | PIN_IN | PIN_OUT; break;
            case 'P': flags = PIN_POWER; break;
            case 'S': flags = PIN_SENSE | PIN_SWITCH | PIN_IN | PIN_OUT; break;
            case 'A': flags = PIN_ANALOG; break;
            default: flags = 0; break;
            }
        }
        int pi = p->pinmap[number < PIN_NUMBERS ? number : 0];
        if (number < PIN_NUMBERS && pi >= 0) {
            s->pin_names[pi] = strdup(tok);
            s->pins[pi].flags = flags;
        }
    }
    free(copy);
}

unsigned part_read_bus(sim_t *s, part_t *p, const int *pins, int n) {
    return sim_read_group(s, p, pins, n);
}

void part_drive_bus(sim_t *s, part_t *p, const int *pins, int n, unsigned v, bool enable) {
    for (int i = 0; i < n; i++)
        pin_out(s, p, pins[i], enable ? DRV_BIT((v >> i) & 1) : DRV_Z);
}

/* ------------------------------------------------------------------------ */
/* The big chips: only pin descriptions here, the models are in board.c     */
/* ------------------------------------------------------------------------ */

#define PINS_6510 \
    "PHI0:M RDY:m /IRQ:m /NMI:m AEC:M VCC:P A0:O A1:O A2:O A3:O A4:O A5:O A6:O A7:O " \
    "A8:O A9:O A10:O A11:O A12:O A13:O GND:P A14:O A15:O P5:B P4:B P3:B P2:B P1:B P0:B " \
    "D7:B D6:B D5:B D4:B D3:B D2:B D1:B D0:B R/W:O PHI2:O /RES:m"
const part_type_t PT_6510 = { "6510 CPU", PINS_6510, 0, 0, 0, 0, TECH_NMOS };

#define PINS_6569 \
    "D6:B D5:B D4:B D3:B D2:B D1:B D0:B /IRQ:O LP:m /CS:m R/W:m BA:O VDD:P COLOR:O " \
    "S/LUM:O AEC:O PHI0:O /RAS:O /CAS:O GND:P PHICOL:m PHIIN:m A11:O A0/A8:B A1/A9:B " \
    "A2/A10:B A3/A11:B A4/A12:B A5/A13:B A6:O A7:O A8:O A9:O A10:O D11:B D10:B D9:B " \
    "D8:B D7:B VCC:P"
const part_type_t PT_6569 = { "6569 VIC-II", PINS_6569, 0, 0, 0, 0, TECH_NMOS };

const part_type_t PT_6526 = { "6526 CIA",
    "GND:P PA0:B PA1:B PA2:B PA3:B PA4:B PA5:B PA6:B PA7:B PB0:B PB1:B PB2:B PB3:B "
    "PB4:B PB5:B PB6:B PB7:B /PC:O TOD:m VCC:P /IRQ:O R/W:m /CS:m /FLAG:m PHI2:m "
    "D7:B D6:B D5:B D4:B D3:B D2:B D1:B D0:B /RES:m RS3:m RS2:m RS1:m RS0:m SP:O CNT:O",
    0, 0, 0, 0, TECH_NMOS };

#define PINS_6581 \
    "CAP1A:O CAP1B:O CAP2A:O CAP2B:O /RES:m PHI2:m R/W:m /CS:m A0:m A1:m A2:m A3:m A4:m " \
    "GND:P D0:B D1:B D2:B D3:B D4:B D5:B D6:B D7:B POTY:m POTX:m VCC:P EXTIN:m " \
    "AUDIO:O VDD:P"
const part_type_t PT_6581 = { "6581 SID", PINS_6581, 0, 0, 0, 0, TECH_NMOS };

/* ------------------------------------------------------------------------ */
/* 82S100 PLA (U17)                                                          */
/* ------------------------------------------------------------------------ */

/* The logic equations of the C64 PLA, from the 82S100 read-out by Mark Smith
 * (ABEL file in "C64 PLA logic equations", Marko Mäkelä et al.), written as
 * the chip is made: an AND array of product terms (every term requires some
 * inputs high and some low) and an OR array (every output is true when any
 * of its terms is). The inspector in the UI shows this same table live.
 *
 * Inputs are pin levels, named as in the schematic: /LORAM=1 means that pin
 * is high (BASIC visible). The PLA is bipolar, so its inputs are TTL: a
 * floating input reads 1. */
const int pla_in_pin[PLA_NIN] = { 2, 3, 4, 5, 6, 7, 8, 9, 20, 21, 22, 23, 24, 25, 26, 27 };
const int pla_out_pin[PLA_NOUT] = { 10, 11, 12, 13, 15, 16, 17, 18 };

#define HI(x) ((1u << PI_##x) | (1u << (PI_##x + 16)))     /* input x high */
#define LO(x) (1u << PI_##x)                                 /* input x low */
#define TERM(spec, outs) { (uint16_t)((spec) & 0xFFFF), (uint16_t)((spec) >> 16), (uint8_t)(outs) }
#define PO(x) (1u << PO_##x)
#define T_CPU LO(AEC)                                       /* the CPU has the bus */
#define T_VIC HI(AEC)                                       /* the VIC has the bus */
#define T_IO  (HI(A15) | HI(A14) | LO(A13) | HI(A12))       /* $D000-$DFFF */

/* Every term but the color RAM write also feeds /CASRAM: RAM is selected
 * only when nothing else is. */
const pla_term_t pla_terms[] = {
    /* /BASIC: BASIC ROM at $A000-$BFFF. */
    TERM(HI(LORAM) | HI(HIRAM) | HI(A15) | LO(A14) | HI(A13) | T_CPU | HI(RW) | HI(GAME), PO(BASIC) | PO(CASRAM)),
    /* /KERNAL: KERNAL ROM at $E000-$FFFF. */
    TERM(HI(HIRAM) | HI(A15) | HI(A14) | HI(A13) | T_CPU | HI(RW) | HI(GAME), PO(KERNAL) | PO(CASRAM)),
    TERM(HI(HIRAM) | HI(A15) | HI(A14) | HI(A13) | T_CPU | HI(RW) | LO(EXROM) | LO(GAME), PO(KERNAL) | PO(CASRAM)),
    /* /CHAROM: character ROM at $D000-$DFFF for the CPU, $1000/$9000 for the VIC. */
    TERM(HI(HIRAM) | LO(CHAREN) | T_IO | T_CPU | HI(RW) | HI(GAME), PO(CHAROM) | PO(CASRAM)),
    TERM(HI(LORAM) | LO(CHAREN) | T_IO | T_CPU | HI(RW) | HI(GAME), PO(CHAROM) | PO(CASRAM)),
    TERM(HI(HIRAM) | LO(CHAREN) | T_IO | T_CPU | HI(RW) | LO(EXROM) | LO(GAME), PO(CHAROM) | PO(CASRAM)),
    TERM(HI(VA14) | T_VIC | HI(GAME) | LO(VA13) | HI(VA12), PO(CHAROM) | PO(CASRAM)),
    TERM(HI(VA14) | T_VIC | LO(EXROM) | LO(GAME) | LO(VA13) | HI(VA12), PO(CHAROM) | PO(CASRAM)),
    /* /IO: the I/O chips at $D000-$DFFF. CPU reads only while BA is high, so
     * a read stalled by the VIC has no side effects. */
    TERM(HI(HIRAM) | HI(CHAREN) | T_IO | T_CPU | HI(BA) | HI(RW) | HI(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(HIRAM) | HI(CHAREN) | T_IO | T_CPU | LO(RW) | HI(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(LORAM) | HI(CHAREN) | T_IO | T_CPU | HI(BA) | HI(RW) | HI(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(LORAM) | HI(CHAREN) | T_IO | T_CPU | LO(RW) | HI(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(HIRAM) | HI(CHAREN) | T_IO | T_CPU | HI(BA) | HI(RW) | LO(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(HIRAM) | HI(CHAREN) | T_IO | T_CPU | LO(RW) | LO(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(LORAM) | HI(CHAREN) | T_IO | T_CPU | HI(BA) | HI(RW) | LO(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    TERM(HI(LORAM) | HI(CHAREN) | T_IO | T_CPU | LO(RW) | LO(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    TERM(T_IO | T_CPU | HI(BA) | HI(RW) | HI(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    TERM(T_IO | T_CPU | LO(RW) | HI(EXROM) | LO(GAME), PO(IO) | PO(CASRAM)),
    /* GR/W: write to the color RAM. */
    TERM(LO(CAS) | T_IO | T_CPU | LO(RW), PO(GRW)),
    /* /ROML: cartridge ROM at $8000-$9FFF. */
    TERM(HI(LORAM) | HI(HIRAM) | HI(A15) | LO(A14) | LO(A13) | T_CPU | HI(RW) | LO(EXROM), PO(ROML) | PO(CASRAM)),
    TERM(HI(A15) | LO(A14) | LO(A13) | T_CPU | HI(EXROM) | LO(GAME), PO(ROML) | PO(CASRAM)),
    /* /ROMH: cartridge ROM at $A000 or $E000 (Ultimax), and for the VIC. */
    TERM(HI(HIRAM) | HI(A15) | LO(A14) | HI(A13) | T_CPU | HI(RW) | LO(EXROM) | LO(GAME), PO(ROMH) | PO(CASRAM)),
    TERM(HI(A15) | HI(A14) | HI(A13) | T_CPU | HI(EXROM) | LO(GAME), PO(ROMH) | PO(CASRAM)),
    TERM(T_VIC | HI(EXROM) | LO(GAME) | HI(VA13) | HI(VA12), PO(ROMH) | PO(CASRAM)),
    /* Ultimax mode leaves holes where nothing, not even RAM, answers. */
    TERM(HI(EXROM) | LO(GAME) | LO(A15) | LO(A14) | HI(A12), PO(CASRAM)),
    TERM(HI(EXROM) | LO(GAME) | LO(A15) | LO(A14) | HI(A13), PO(CASRAM)),
    TERM(HI(EXROM) | LO(GAME) | LO(A15) | HI(A14), PO(CASRAM)),
    TERM(HI(EXROM) | LO(GAME) | HI(A15) | LO(A14) | HI(A13), PO(CASRAM)),
    TERM(HI(EXROM) | LO(GAME) | HI(A15) | HI(A14) | LO(A13) | LO(A12), PO(CASRAM)),
    /* RAM is selected only while the VIC's /CAS is low: the PLA gates the
     * DRAM column strobe. */
    TERM(HI(CAS), PO(CASRAM)),
};
const int pla_nterms = sizeof(pla_terms) / sizeof(pla_terms[0]);

unsigned pla_read_inputs(sim_t *s, part_t *p) {
    unsigned in = 0;
    for (int k = 0; k < PLA_NIN; k++) in |= (unsigned)pin_in(s, p, pla_in_pin[k]) << k;
    return in;
}

static unsigned pla_logic_terms(unsigned in) {
    unsigned any = 0;
    for (int t = 0; t < pla_nterms; t++)
        if ((in & pla_terms[t].mask) == pla_terms[t].value) any |= pla_terms[t].outputs;
    /* The selects are active low; /CASRAM is high when RAM is NOT selected. */
    return (any ^ ~(1u << PO_CASRAM)) & 0xFF;
}

/* The PLA is evaluated on almost every bus change: look its outputs up in
 * a table of all 2^16 input combinations, built from the terms above the
 * first time it is needed, instead of matching every term each time.
 * Boards running in parallel threads share the table: call pla_logic()
 * once before starting them (see atlas.c). */
unsigned pla_logic(unsigned in) {
    static uint8_t table[1u << PLA_NIN];
    static bool ready;
    if (!ready) {
        for (unsigned i = 0; i < (1u << PLA_NIN); i++) table[i] = (uint8_t)pla_logic_terms(i);
        ready = true;
    }
    return table[in];
}

static void pla_eval(sim_t *s, part_t *p) {
    unsigned out = pla_logic(pla_read_inputs(s, p));
    bool en = !pin_in(s, p, 19);            /* /OE, tied to GND */
    for (int k = 0; k < PLA_NOUT; k++)
        pin_out(s, p, pla_out_pin[k], en ? DRV_BIT((out >> k) & 1) : DRV_Z);
}

const part_type_t PT_82S100 = { "82S100 PLA",
    "FE:O A13:T A14:T A15:T /VA14:T /CHAREN:T /HIRAM:T /LORAM:T /CAS:T /ROMH:O /ROML:O "
    "/IO:O GR/W:O GND:P /CHAROM:O /KERNAL:O /BASIC:O /CASRAM:O /OE:T VA12:T VA13:T "
    "/GAME:T /EXROM:T R/W:T /AEC:T BA:T A12:T VCC:P", pla_eval, 0, 0, 0, TECH_TTL };

/* ------------------------------------------------------------------------ */
/* Memory faults (part_t.mfault, see sim.h)                                  */
/* ------------------------------------------------------------------------ */

/* The address the cells of a chip see: a dead address line reads 0. */
static inline unsigned mem_addr(const part_t *p, unsigned addr) {
    return p->mfault == MF_ADDR ? addr & ~(1u << p->mf_bit) : addr;
}

/* What a faulty chip returns for the data 'v' stored at 'addr'. */
static unsigned mem_read(const part_t *p, unsigned addr, unsigned v) {
    switch (p->mfault) {
    case MF_DATA: return p->mf_val ? v | 1u << p->mf_bit : v & ~(1u << p->mf_bit);
    case MF_FLIP: return addr == p->mf_addr ? v ^ p->mf_val : v;
    case MF_WEAK: return (addr * 2654435761u) >> 26 ? v : 0;
    default:      return v;
    }
}

/* ------------------------------------------------------------------------ */
/* ROMs: 2364 (8K, BASIC and KERNAL) and 2332 (4K, characters)               */
/* ------------------------------------------------------------------------ */

static const int rom_addr_pins[13] = { 8, 7, 6, 5, 4, 3, 2, 1, 23, 22, 19, 18, 21 };
static const int rom_data_pins[8] = { 9, 10, 11, 13, 14, 15, 16, 17 };

static unsigned rom_byte(const part_t *p, const rom_state_t *st, unsigned addr) {
    if (!st->data) return 0xFF;
    if (!p->mfault) return st->data[addr];
    addr = mem_addr(p, addr);
    return mem_read(p, addr, st->data[addr]);
}

static void rom2364_eval(sim_t *s, part_t *p) {
    rom_state_t *st = p->state;
    bool selected = !pin_in(s, p, 20);
    unsigned addr = selected ? part_read_bus(s, p, rom_addr_pins, 13) : 0;
    part_drive_bus_cached(s, p, rom_data_pins, 8, rom_byte(p, st, addr), selected, &st->out_cache);
}

static void rom2332_eval(sim_t *s, part_t *p) {
    rom_state_t *st = p->state;
    bool selected = !pin_in(s, p, 20) && pin_in(s, p, 21);   /* CS1 low, CS2 high */
    unsigned addr = selected ? part_read_bus(s, p, rom_addr_pins, 12) : 0;
    part_drive_bus_cached(s, p, rom_data_pins, 8, rom_byte(p, st, addr), selected, &st->out_cache);
}

const part_type_t PT_2364 = { "2364 ROM",
    "A7:M A6:M A5:M A4:M A3:M A2:M A1:M A0:M D0:O D1:O D2:O GND:P D3:O D4:O D5:O D6:O "
    "D7:O A11:M A10:M /CS:M A12:M A9:M A8:M VCC:P", rom2364_eval, 0, 0, sizeof(rom_state_t), TECH_NMOS };

const part_type_t PT_2332 = { "2332 ROM",
    "A7:M A6:M A5:M A4:M A3:M A2:M A1:M A0:M D0:O D1:O D2:O GND:P D3:O D4:O D5:O D6:O "
    "D7:O A11:M A10:M /CS1:M CS2:M A9:M A8:M VCC:P", rom2332_eval, 0, 0, sizeof(rom_state_t), TECH_NMOS };

/* ------------------------------------------------------------------------ */
/* 2114 static RAM 1K x 4 (color RAM, U6)                                    */
/* ------------------------------------------------------------------------ */

static const int sram_addr_pins[10] = { 5, 6, 7, 4, 3, 2, 1, 17, 16, 15 };
static const int sram_io_pins[4] = { 14, 13, 12, 11 };

static void sram_eval(sim_t *s, part_t *p) {
    sram_state_t *st = p->state;
    bool read = !pin_in(s, p, 8) && pin_in(s, p, 10);         /* /CS low, /WE high */
    unsigned addr = read ? mem_addr(p, part_read_bus(s, p, sram_addr_pins, 10)) : 0;
    unsigned v = p->mfault ? mem_read(p, addr, st->cells[addr]) & 15 : st->cells[addr];
    part_drive_bus_cached(s, p, sram_io_pins, 4, v, read, &st->out_cache);
}

static void sram_commit(sim_t *s, part_t *p) {
    sram_state_t *st = p->state;
    if (!pin_in(s, p, 8) && !pin_in(s, p, 10))                 /* /CS low, /WE low */
        st->cells[mem_addr(p, part_read_bus(s, p, sram_addr_pins, 10))] =
            (uint8_t)part_read_bus(s, p, sram_io_pins, 4);
}

static void sram_reset(sim_t *s, part_t *p) {
    (void)s;
    sram_state_t *st = p->state;
    for (int i = 0; i < 1024; i++) st->cells[i] = (uint8_t)((i * 7 + (i >> 4)) & 15);
}

const part_type_t PT_2114 = { "2114 SRAM",
    "A6:M A5:M A4:M A3:M A0:M A1:M A2:M /CS:M GND:P /WE:M IO4:B IO3:B IO2:B IO1:B "
    "A9:M A8:M A7:M VCC:P", sram_eval, sram_commit, sram_reset, sizeof(sram_state_t), TECH_NMOS };

/* ------------------------------------------------------------------------ */
/* 4164 dynamic RAM 64K x 1 (U9-U12, U21-U24)                                */
/* ------------------------------------------------------------------------ */

/* The DRAM receives 8 multiplexed address lines: the row is latched when
 * /RAS falls, the column when /CAS falls. On the C64 a CPU address maps to
 * row = A0-A7, column = A8-A15. Each chip stores one bit of every byte. */
static const int dram_addr_pins[8] = { 5, 7, 6, 12, 11, 10, 13, 9 };

/* Everything a DRAM does happens on the edges of /RAS and /CAS, so the
 * work is in commit(); eval() just drives Q (pin 14) during a read. */
static void dram_eval(sim_t *s, part_t *p) {
    dram_state_t *st = p->state;
    pin_out(s, p, 14, st->out_en ? DRV_BIT(st->out_bit) : DRV_Z);
}

/* A DRAM cycle with 'bits' address lines. A 4532 or a 3732 is a 4164
 * sold with one good half: the cells of the other half (by 'half', below)
 * don't store anything and read back garbage. */
enum {
    HALF_ALL,           /* a whole chip (4164, 4116) */
    HALF_ROW7_LOW,      /* the rows with A7 low: TI 4532-3 */
    HALF_ROW7_HIGH,     /* the rows with A7 high: TI 4532-4 */
    HALF_COL7_LOW,      /* the columns with A7 low: OKI 3732-L */
    HALF_COL7_HIGH,     /* the columns with A7 high: OKI 3732-H */
};
static void dram_cycle(sim_t *s, part_t *p, int bits, int half) {
    dram_state_t *st = p->state;
    int ras = pin_in(s, p, 4), cas = pin_in(s, p, 15);
    uint8_t out_en = st->out_en, out_bit = st->out_bit;
    /* A dead address line inside the chip is dead for the row and the
     * column alike: they come in through the same buffer. */
    if (st->ras_prev && !ras) st->row = (uint8_t)mem_addr(p, part_read_bus(s, p, dram_addr_pins, bits));
    if (st->cas_prev && !cas && !ras) {
        st->col = (uint8_t)mem_addr(p, part_read_bus(s, p, dram_addr_pins, bits));
        int cell = (st->row << 8) | st->col;
        bool bad = half == HALF_ROW7_LOW ? (st->row & 0x80) : half == HALF_ROW7_HIGH ? !(st->row & 0x80) :
                   half == HALF_COL7_LOW ? (st->col & 0x80) : half == HALF_COL7_HIGH ? !(st->col & 0x80) : false;
        if (!pin_in(s, p, 3)) {          /* /WE low before /CAS: early write */
            if (!bad) st->cells[cell] = (uint8_t)pin_in(s, p, 2);
            st->out_en = 0;
        } else {                        /* read: Q drives until /CAS rises */
            st->out_en = 1;
            st->out_bit = bad ? (uint8_t)(((unsigned)cell * 2654435761u) >> 31) : st->cells[cell];
            if (p->mfault) st->out_bit = (uint8_t)(mem_read(p, (unsigned)cell, st->out_bit) & 1);
        }
    }
    /* Q is valid until /CAS rises (it outlives /RAS: the ZX Spectrum's
     * ULA reads the attribute byte that way). */
    if (cas) st->out_en = 0;
    st->ras_prev = (uint8_t)ras;
    st->cas_prev = (uint8_t)cas;
    if (st->out_en != out_en || st->out_bit != out_bit) sim_mark_dirty(s, (int)(p - s->parts));
}

static void dram_commit(sim_t *s, part_t *p) { dram_cycle(s, p, 8, HALF_ALL); }

/* Power-on pattern used by c64-llm: 64 bytes of $00, then 64 of $FF. With
 * row = A0-A7 and column = A8-A15 the address of a cell is (col << 8) | row. */
static void dram_reset(sim_t *s, part_t *p) {
    (void)s;
    dram_state_t *st = p->state;
    for (int row = 0; row < 256; row++)
        for (int col = 0; col < 256; col++) {
            int addr = (col << 8) | row;
            st->cells[(row << 8) | col] = (addr & 64) ? 1 : 0;
        }
    st->ras_prev = st->cas_prev = 1;
    st->out_en = 0;
}

const part_type_t PT_4164 = { "4164",
    "NC D:m /WE:m /RAS:m A0:m A2:m A1:m VCC:P A7:m A5:m A4:m A3:m A6:m Q:O /CAS:m GND:P",
    dram_eval, dram_commit, dram_reset, sizeof(dram_state_t), TECH_NMOS };

/* ------------------------------------------------------------------------ */
/* TTL glue                                                                  */
/* ------------------------------------------------------------------------ */

/* 74LS08: quad 2-input AND. */
static void ls08_eval(sim_t *s, part_t *p) {
    static const int g[4][3] = { {1, 2, 3}, {4, 5, 6}, {9, 10, 8}, {12, 13, 11} };
    for (int i = 0; i < 4; i++)
        pin_out(s, p, g[i][2], DRV_BIT(pin_in(s, p, g[i][0]) && pin_in(s, p, g[i][1])));
}
const part_type_t PT_74LS08 = { "74LS08",
    "1A:T 1B:T 1Y:O 2A:T 2B:T 2Y:O GND:P 3Y:O 3A:T 3B:T 4Y:O 4A:T 4B:T VCC:P",
    ls08_eval, 0, 0, 0, TECH_TTL };

/* 7406: hex inverter with open collector outputs: it can only pull low. */
static void ls06_eval(sim_t *s, part_t *p) {
    static const int g[6][2] = { {1, 2}, {3, 4}, {5, 6}, {9, 8}, {11, 10}, {13, 12} };
    for (int i = 0; i < 6; i++)
        pin_out(s, p, g[i][1], pin_in(s, p, g[i][0]) ? DRV_LO : DRV_Z);
}
const part_type_t PT_7406 = { "7406",
    "1A:T 1Y:O 2A:T 2Y:O 3A:T 3Y:O GND:P 4Y:O 4A:T 5Y:O 5A:T 6Y:O 6A:T VCC:P",
    ls06_eval, 0, 0, 0, TECH_TTL_OC };

/* 74LS139: dual 2-to-4 decoder, active low outputs. */
static void ls139_eval(sim_t *s, part_t *p) {
    static const int half[2][7] = { {1, 2, 3, 4, 5, 6, 7}, {15, 14, 13, 12, 11, 10, 9} };
    for (int h = 0; h < 2; h++) {
        const int *pin = half[h];
        bool enabled = !pin_in(s, p, pin[0]);
        int sel = pin_in(s, p, pin[1]) | (pin_in(s, p, pin[2]) << 1);
        for (int y = 0; y < 4; y++) pin_out(s, p, pin[3 + y], DRV_BIT(!(enabled && sel == y)));
    }
}
const part_type_t PT_74LS139 = { "74LS139",
    "1/G:T 1A:T 1B:T 1Y0:O 1Y1:O 1Y2:O 1Y3:O GND:P 2Y3:O 2Y2:O 2Y1:O 2Y0:O 2B:T 2A:T "
    "2/G:T VCC:P", ls139_eval, 0, 0, 0, TECH_TTL };

/* 74LS257 / 74LS258: quad 2-to-1 multiplexer (258 has inverted outputs).
 * S low selects the A inputs, S high selects B. /G high: outputs off. */
static void mux_eval(sim_t *s, part_t *p, bool invert) {
    static const int sec[4][3] = { {2, 3, 4}, {5, 6, 7}, {11, 10, 9}, {14, 13, 12} };
    static const int outputs[4] = { 4, 7, 9, 12 };
    bool enabled = !pin_in(s, p, 15);
    int sel = pin_in(s, p, 1);
    unsigned v = 0;
    for (int i = 0; i < 4; i++) v |= (unsigned)(pin_in(s, p, sel ? sec[i][1] : sec[i][0]) ^ invert) << i;
    part_drive_bus(s, p, outputs, 4, v, enabled);
}
static void ls257_eval(sim_t *s, part_t *p) { mux_eval(s, p, false); }
static void ls258_eval(sim_t *s, part_t *p) { mux_eval(s, p, true); }
#define MUX_PINS "S:T 1A:T 1B:T 1Y:O 2A:T 2B:T 2Y:O GND:P 3Y:O 3B:T 3A:T 4Y:O 4B:T 4A:T /G:T VCC:P"
const part_type_t PT_74LS257 = { "74LS257", MUX_PINS, ls257_eval, 0, 0, 0, TECH_TTL };
const part_type_t PT_74LS258 = { "74LS258", MUX_PINS, ls258_eval, 0, 0, 0, TECH_TTL };

/* 74LS373: octal transparent latch. While LE is high the outputs follow the
 * inputs; when LE goes low the last value is held. /OE high: outputs off. */
typedef struct { uint8_t latch; int32_t out_cache; } ls373_state_t;
static const int ls373_d[8] = { 3, 4, 7, 8, 13, 14, 17, 18 };
static const int ls373_q[8] = { 2, 5, 6, 9, 12, 15, 16, 19 };

static void ls373_eval(sim_t *s, part_t *p) {
    ls373_state_t *st = p->state;
    bool enabled = !pin_in(s, p, 1);
    unsigned v = enabled && pin_in(s, p, 11) ? part_read_bus(s, p, ls373_d, 8) : st->latch;
    part_drive_bus_cached(s, p, ls373_q, 8, v, enabled, &st->out_cache);
}
static void ls373_commit(sim_t *s, part_t *p) {
    ls373_state_t *st = p->state;
    if (pin_in(s, p, 11)) st->latch = (uint8_t)part_read_bus(s, p, ls373_d, 8);
}
const part_type_t PT_74LS373 = { "74LS373",
    "/OE:T 1Q:O 1D:T 2D:T 2Q:O 3Q:O 3D:T 4D:T 4Q:O GND:P LE:T 5Q:O 5D:T 6D:T 6Q:O 7Q:O "
    "7D:T 8D:T 8Q:O VCC:P", ls373_eval, ls373_commit, 0, sizeof(ls373_state_t), TECH_TTL };

/* 4066: four bidirectional analog switches. When a switch is on, each side
 * gets whatever drives the other side (excluding the switch itself), so a
 * signal passes through in either direction. */
static void cd4066_eval(sim_t *s, part_t *p) {
    static const int sw[4][3] = { {1, 2, 13}, {4, 3, 5}, {8, 9, 6}, {11, 10, 12} };
    for (int i = 0; i < 4; i++) {
        int a = part_pin(p, sw[i][0]), b = part_pin(p, sw[i][1]);
        if (a < 0 || b < 0) continue;
        if (pin_in(s, p, sw[i][2])) {
            drive_t from_a = sim_drive_others(s, a);
            drive_t from_b = sim_drive_others(s, b);
            sim_drive(s, a, from_b);
            sim_drive(s, b, from_a);
        } else {
            sim_drive(s, a, DRV_Z);
            sim_drive(s, b, DRV_Z);
        }
    }
}
const part_type_t PT_4066 = { "4066",
    "1A:S 1B:S 2B:S 2A:S C2:M C3:M VSS:P 3A:S 3B:S 4B:S 4A:S C4:M C1:M VDD:P",
    cd4066_eval, 0, 0, 0, TECH_CMOS };

/* 556 dual timer (U20), modeled by what it does on the C64:
 *  - timer 2: holds the system in reset for a while after power-on (OUT2
 *    high, inverted by U8 into /RESET low);
 *  - timer 1: turns the RESTORE key press into a clean pulse (OUT1 high,
 *    inverted by U8 into /NMI low).
 * The RESTORE key reaches TRIG1 through a capacitor (C38) we don't simulate,
 * so the model watches the RESTORE net directly. */
#define RESET_CYCLES   50000    /* ~50 ms */
#define NMI_CYCLES      1000    /* ~1 ms */

static void t556_eval(sim_t *s, part_t *p) {
    timer556_state_t *st = p->state;
    pin_out(s, p, 5, DRV_BIT(s->cycle < st->nmi_until));
    pin_out(s, p, 9, DRV_BIT(s->cycle < st->reset_until));
}
static void t556_commit(sim_t *s, part_t *p) {
    timer556_state_t *st = p->state;
    if (st->last_cycle == s->cycle) return;     /* nothing new in this cycle */
    st->last_cycle = s->cycle;
    uint8_t restore = 1;
    if (st->restore_net >= 0) restore = s->nets[s->nets[st->restore_net].root].level;
    if (st->restore_prev && !restore) st->nmi_until = s->cycle + NMI_CYCLES;
    st->restore_prev = restore;
    bool out1 = s->cycle < st->nmi_until, out2 = s->cycle < st->reset_until;
    if (out1 != (s->pins[part_pin(p, 5)].drive == DRV_HI) ||
        out2 != (s->pins[part_pin(p, 9)].drive == DRV_HI))
        sim_mark_dirty(s, (int)(p - s->parts));
}
static void t556_reset(sim_t *s, part_t *p) {
    timer556_state_t *st = p->state;
    st->reset_until = s->cycle + RESET_CYCLES;
    st->nmi_until = 0;
    st->restore_prev = 1;
    st->last_cycle = UINT64_MAX;
}
const part_type_t PT_556 = { "556 TIMER",
    "DIS1:O THR1:m CV1:A /RES1:m OUT1:O TRIG1:m GND:P TRIG2:m OUT2:O /RES2:m CV2:A THR2:m "
    "DIS2:O VCC:P", t556_eval, t556_commit, t556_reset, sizeof(timer556_state_t), TECH_TTL };

/* 74LS629: two voltage controlled oscillators (U31). On the C64 one makes
 * the dot clock for the VIC (pin 7), the other, with the crystal Y1, the
 * color clock (pin 10). Real clocks are MHz signals: here they toggle once
 * per half bus cycle, enough for the board to know they are alive. */
static void ls629_eval(sim_t *s, part_t *p) {
    vco_state_t *st = p->state;
    pin_out(s, p, 7, DRV_BIT(s->clock_level));
    pin_out(s, p, 10, st->crystal_ok ? DRV_BIT(s->clock_level) : DRV_LO);
}
const part_type_t PT_74LS629 = { "74LS629 VCO",
    "2FC:A 1FC:A 1RNG:m 1CX:A 1CX:A /1EN:m 1Y:O OSCGND:P GND:P 2Y:O /2EN:m 2CX:A 2CX:A "
    "2RNG:m OSCVCC:P VCC:P", ls629_eval, 0, 0, sizeof(vco_state_t), TECH_TTL };

/* The rest of the PLL is not simulated: removing these chips changes nothing. */
const part_type_t PT_74LS74 = { "74LS74",
    "/1CLR:m 1D:m 1CK:m /1PR:m 1Q:O /1Q:O GND:P /2Q:O 2Q:O /2PR:m 2CK:m 2D:m /2CLR:m VCC:P",
    0, 0, 0, 0, TECH_TTL };
const part_type_t PT_74LS193 = { "74LS193",
    "B:m QB:O QA:O DOWN:m UP:m QC:O QD:O GND:P D:m C:m /LOAD:m /CO:O /BO:O CLR:m A:m VCC:P",
    0, 0, 0, 0, TECH_TTL };
const part_type_t PT_MC4044 = { "MC4044 PLL",
    "R:m 2:O V:m 4:O 5:O 6:O GND:P 8:O 9:O 10:O 11:O 12:O 13:O VCC:P", 0, 0, 0, 0, TECH_TTL };

/* ------------------------------------------------------------------------ */
/* Resistors                                                                 */
/* ------------------------------------------------------------------------ */

/* A pull-up drives its net weakly high, unless it is open; a shorted pull-up
 * ties the net to the rail. Links are handled by the board (net merging). */
/* What a series resistor passes from one side to the other: the drive on
 * that side (its own drive excluded), one step weaker. Through two
 * resistors a chip output is only as strong as a pull-up. */
static drive_t through(sim_t *s, int pin) {
    drive_t d = sim_drive_others(s, pin);
    int st = d >> 1;
    if (st >= S_STRONG) return DRIVE(S_RES, d & 1);
    if (st >= S_PULL) return DRIVE(S_PULL, d & 1);
    return DRV_Z;
}

void resistor_eval(sim_t *s, part_t *p) {
    resistor_state_t *st = p->state;
    for (int i = 0; i < st->nel; i++) {
        element_t *e = &st->el[i];
        switch (e->type) {
        case EL_PULLUP:
        case EL_PULLDOWN: {
            /* Open, or its leg on the supply lifted: it pulls nothing. */
            int level = e->type == EL_PULLUP;
            drive_t d = p->rfault == RF_OPEN || s->pins[e->pin_b].fault == PF_OPEN ? DRV_Z :
                        p->rfault == RF_SHORT ? DRIVE(S_SUPPLY, level) : DRIVE(S_PULL, level);
            sim_drive(s, e->pin_a, d);
            break;
        }
        case EL_SERIES: {
            /* Open: nothing passes. Shorted: a link (see board_rebuild).
             * Both ends on one net (shorted around it): nothing to do. */
            const pin_t *a = &s->pins[e->pin_a], *b = &s->pins[e->pin_b];
            bool off = p->rfault != RF_OK || a->root < 0 || b->root < 0 || a->root == b->root ||
                       a->fault == PF_OPEN || b->fault == PF_OPEN;
            sim_drive(s, e->pin_b, off ? DRV_Z : through(s, e->pin_a));
            sim_drive(s, e->pin_a, off ? DRV_Z : through(s, e->pin_b));
            break;
        }
        case EL_DIODE: {
            /* Conducts when the cathode is driven low: the anode follows,
             * through the diode's drop, stronger than a pull-up. */
            const pin_t *k = &s->pins[e->pin_b];
            bool on = p->rfault == RF_OK && k->root >= 0 && k->fault != PF_OPEN &&
                      s->nets[k->root].strength != S_Z && !s->nets[k->root].level;
            if (p->rfault == RF_SHORT) on = k->root >= 0 && !s->nets[k->root].level;
            sim_drive(s, e->pin_a, on ? DRIVE(S_RES, 0) : DRV_Z);
            break;
        }
        case EL_DELAY:
            /* Shorted: a wire, joined by board_rebuild(), with no delay. */
            sim_drive(s, e->pin_b, p->rfault != RF_OK ? DRV_Z : DRV_BIT(st->delayed[i]));
            break;
        case EL_NPN:
        case EL_FOLLOWER: {
            /* On while the base is driven high (a short from collector to
             * emitter: always on, open: never). */
            const pin_t *bp = &s->pins[e->pin_a];
            bool on = p->rfault == RF_SHORT ||
                      (p->rfault == RF_OK && bp->root >= 0 && bp->fault != PF_OPEN &&
                       s->nets[bp->root].strength != S_Z && s->nets[bp->root].level);
            sim_drive(s, e->pin_b, !on ? DRV_Z : e->type == EL_NPN ? DRV_LO : DRV_HI);
            break;
        }
        default:
            break;
        }
    }
}

/* RC delays: at every bus step the capacitor side takes the level the
 * driven side had (eval() then drives it). */
void resistor_commit(sim_t *s, part_t *p) {
    resistor_state_t *st = p->state;
    for (int i = 0; i < st->nel; i++) {
        if (st->el[i].type != EL_DELAY) continue;
        uint8_t v = (uint8_t)sim_read(s, st->el[i].pin_a);
        if (v != st->delayed[i]) { st->delayed[i] = v; sim_mark_dirty(s, (int)(p - s->parts)); }
    }
}

void element_set_type(sim_t *s, part_t *p, int index, int type) {
    resistor_state_t *st = p->state;
    element_t *e = &st->el[index];
    e->type = type;
    pin_t *a = &s->pins[e->pin_a], *b = &s->pins[e->pin_b];
    switch (type) {
    case EL_SERIES:     /* re-evaluated when anything on either side changes */
        a->flags = b->flags = PIN_SENSE | PIN_SWITCH | PIN_IN | PIN_OUT;
        break;
    case EL_DIODE:
        a->flags = PIN_OUT;
        b->flags = PIN_SENSE | PIN_IN;
        break;
    case EL_NPN:
    case EL_FOLLOWER:
        a->flags = PIN_SENSE | PIN_IN;
        b->flags = PIN_OUT;
        break;
    case EL_DELAY:
        a->flags = PIN_IN;
        b->flags = PIN_OUT;
        p->commit = resistor_commit;
        break;
    }
    p->eval = resistor_eval;
}

/* ------------------------------------------------------------------------ */
/* ZX Spectrum 48K: memories and glue logic                                  */
/* ------------------------------------------------------------------------ */

/* 4116 dynamic RAM 16K x 1 (the lower 16K, IC6-IC13): a 4164 with seven
 * address lines and three supplies (+12V, +5V, -5V). */
static void dram4116_commit(sim_t *s, part_t *p) { dram_cycle(s, p, 7, HALF_ALL); }
const part_type_t PT_4116 = { "4116",
    "VBB:P D:m /WE:m /RAS:m A0:m A2:m A1:m VDD:P VCC:P A5:m A4:m A3:m A6:m Q:O /CAS:m VSS:P",
    dram_eval, dram4116_commit, dram_reset, sizeof(dram_state_t), TECH_NMOS };

/* 4532 (TI) and 3732 (OKI): a 4164 with one good half, the Spectrum's
 * upper 32K (IC15-IC22). The TI's halves are by the row A7, the OKI's by
 * the column A7: the board's links put A14 in the other address and the
 * A7 of the good half in this one (zx48.c). PT_4532 is the TI 4532-3, the
 * rows with A7 low. */
#define DRAM_4532_PINS "NC:O D:m /WE:m /RAS:m A0:m A2:m A1:m VCC:P A7:m A5:m A4:m A3:m A6:m Q:O /CAS:m GND:P"
static void dram4532_commit(sim_t *s, part_t *p) { dram_cycle(s, p, 8, HALF_ROW7_LOW); }
static void dram4532_4_commit(sim_t *s, part_t *p) { dram_cycle(s, p, 8, HALF_ROW7_HIGH); }
static void dram3732l_commit(sim_t *s, part_t *p) { dram_cycle(s, p, 8, HALF_COL7_LOW); }
static void dram3732h_commit(sim_t *s, part_t *p) { dram_cycle(s, p, 8, HALF_COL7_HIGH); }
const part_type_t PT_4532 = { "4532", DRAM_4532_PINS,
    dram_eval, dram4532_commit, dram_reset, sizeof(dram_state_t), TECH_NMOS };
const part_type_t PT_4532_4 = { "4532-4", DRAM_4532_PINS,
    dram_eval, dram4532_4_commit, dram_reset, sizeof(dram_state_t), TECH_NMOS };
const part_type_t PT_3732L = { "3732-L", DRAM_4532_PINS,
    dram_eval, dram3732l_commit, dram_reset, sizeof(dram_state_t), TECH_NMOS };
const part_type_t PT_3732H = { "3732-H", DRAM_4532_PINS,
    dram_eval, dram3732h_commit, dram_reset, sizeof(dram_state_t), TECH_NMOS };
/* A 4164 in the 4532's place (the whole chip works). */
const part_type_t PT_4164_ZX = { "4164", DRAM_4532_PINS,
    dram_eval, dram_commit, dram_reset, sizeof(dram_state_t), TECH_NMOS };

bool part_is_dram64(const part_t *p) {
    return p->commit == dram_commit || p->commit == dram4532_commit || p->commit == dram4532_4_commit ||
           p->commit == dram3732l_commit || p->commit == dram3732h_commit;
}

/* 16K x 8 mask ROMs (the Spectrum's IC5), two makers, the same pins but
 * 20 and 27 (their active levels are the customer's: Sinclair's are low):
 *   Hitachi HN613128: static. Pin 20 CS, 22 OE0, 27 OE1: the data comes
 *     out while all three are low, from the address of the moment.
 *   NEC uPD23128: clocked. Pin 20 /CE latches the address when it falls
 *     (it must go high between reads), 22 OD, 27 CS.
 * The board's link (JROM1, LNK2) puts /MREQ on pin 20 and /ROMCS on 27
 * for a NEC, the other way round for a Hitachi. A Hitachi works either
 * way. A NEC with /ROMCS on /CE sees no edge while the CPU stays in the
 * ROM (/ROMCS is A14 and A15): it gives the byte it latched at the first
 * read after the last RAM access, again and again. Both types share the
 * state, so the option can swap them (zx48.c). */
static const int rom23128_addr_pins[14] = { 10, 9, 8, 7, 6, 5, 4, 3, 25, 24, 21, 23, 2, 26 };
static const int rom23128_data_pins[8] = { 11, 12, 13, 15, 16, 17, 18, 19 };
typedef struct { rom_state_t rom; uint16_t addr; uint8_t ce_prev; } rom23128_state_t;
static void hn613128_eval(sim_t *s, part_t *p) {
    rom23128_state_t *st = p->state;
    bool selected = !pin_in(s, p, 20) && !pin_in(s, p, 22) && !pin_in(s, p, 27);
    unsigned addr = selected ? part_read_bus(s, p, rom23128_addr_pins, 14) : 0;
    part_drive_bus_cached(s, p, rom23128_data_pins, 8, rom_byte(p, &st->rom, addr), selected,
                          &st->rom.out_cache);
}
static void upd23128_eval(sim_t *s, part_t *p) {
    rom23128_state_t *st = p->state;
    bool selected = !pin_in(s, p, 20) && !pin_in(s, p, 22) && !pin_in(s, p, 27);
    part_drive_bus_cached(s, p, rom23128_data_pins, 8, rom_byte(p, &st->rom, st->addr), selected,
                          &st->rom.out_cache);
}
static void upd23128_commit(sim_t *s, part_t *p) {
    rom23128_state_t *st = p->state;
    int ce = pin_in(s, p, 20);
    if (st->ce_prev && !ce) {
        unsigned addr = part_read_bus(s, p, rom23128_addr_pins, 14);
        if (addr != st->addr) { st->addr = (uint16_t)addr; sim_mark_dirty(s, (int)(p - s->parts)); }
    }
    st->ce_prev = (uint8_t)ce;
}
static void rom23128_reset(sim_t *s, part_t *p) {
    (void)s;
    rom23128_state_t *st = p->state;
    st->addr = 0;
    st->ce_prev = 1;
}
#define ROM23128_PINS(ce, oe, cs) \
    "NC:O A12:M A7:M A6:M A5:M A4:M A3:M A2:M A1:M A0:M D0:O D1:O D2:O GND:P D3:O D4:O D5:O D6:O " \
    "D7:O " ce ":M A10:M " oe ":M A11:M A9:M A8:M A13:M " cs ":M VCC:P"
const part_type_t PT_UPD23128 = { "uPD23128 ROM", ROM23128_PINS("/CE", "OD", "CS"), upd23128_eval,
    upd23128_commit, rom23128_reset, sizeof(rom23128_state_t), TECH_NMOS };
const part_type_t PT_HN613128 = { "HN613128 ROM", ROM23128_PINS("CS", "OE0", "OE1"), hn613128_eval, 0,
    rom23128_reset, sizeof(rom23128_state_t), TECH_NMOS };

/* 74LS157: quad 2-to-1 multiplexer. S low selects A, high selects B. With
 * /G high the outputs are low (not off, unlike the 74LS257). */
static void ls157_eval(sim_t *s, part_t *p) {
    static const int sec[4][3] = { {2, 3, 4}, {5, 6, 7}, {11, 10, 9}, {14, 13, 12} };
    bool enabled = !pin_in(s, p, 15);
    int sel = pin_in(s, p, 1);
    for (int i = 0; i < 4; i++)
        pin_out(s, p, sec[i][2], DRV_BIT(enabled && pin_in(s, p, sel ? sec[i][1] : sec[i][0])));
}
const part_type_t PT_74LS157 = { "74LS157", MUX_PINS, ls157_eval, 0, 0, 0, TECH_TTL };

/* 74LS32: quad 2-input OR. 74LS00: quad 2-input NAND. */
static const int quad_gates[4][3] = { {1, 2, 3}, {4, 5, 6}, {9, 10, 8}, {12, 13, 11} };
static void ls32_eval(sim_t *s, part_t *p) {
    for (int i = 0; i < 4; i++)
        pin_out(s, p, quad_gates[i][2], DRV_BIT(pin_in(s, p, quad_gates[i][0]) || pin_in(s, p, quad_gates[i][1])));
}
static void ls00_eval(sim_t *s, part_t *p) {
    for (int i = 0; i < 4; i++)
        pin_out(s, p, quad_gates[i][2], DRV_BIT(!(pin_in(s, p, quad_gates[i][0]) && pin_in(s, p, quad_gates[i][1]))));
}
const part_type_t PT_74LS32 = { "74LS32",
    "1A:T 1B:T 1Y:O 2A:T 2B:T 2Y:O GND:P 3Y:O 3A:T 3B:T 4Y:O 4A:T 4B:T VCC:P", ls32_eval, 0, 0, 0, TECH_TTL };
const part_type_t PT_74LS00 = { "74LS00",
    "1A:T 1B:T 1Y:O 2A:T 2B:T 2Y:O GND:P 3Y:O 3A:T 3B:T 4Y:O 4A:T 4B:T VCC:P", ls00_eval, 0, 0, 0, TECH_TTL };

/* 74LS04: hex inverter. */
static void ls04_eval(sim_t *s, part_t *p) {
    static const int g[6][2] = { {1, 2}, {3, 4}, {5, 6}, {9, 8}, {11, 10}, {13, 12} };
    for (int i = 0; i < 6; i++) pin_out(s, p, g[i][1], DRV_BIT(!pin_in(s, p, g[i][0])));
}
const part_type_t PT_74LS04 = { "74LS04",
    "1A:T 1Y:O 2A:T 2Y:O 3A:T 3Y:O GND:P 4Y:O 4A:T 5Y:O 5A:T 6Y:O 6A:T VCC:P", ls04_eval, 0, 0, 0, TECH_TTL };

/* ZX8401 (Philips PCF1306P): the Issue 6A's glue in one chip, the 4B's
 * IC3, IC4 and IC23-IC26 as a drop-in replacement made of 74LS parts
 * draws it (trastero.speccy.org/cosas/JL/pcf1306p). Four 74LS157
 * sections each: IC3 and IC4 switch the CPU address onto the lower RAM's
 * (DMA0-DMA6, by DRASLL: /RAS delayed outside), IC25 and IC26 onto the
 * upper RAM's (MA0-MA7, MA7 from the type links on TS1 and TS2), by the
 * select of the 4B's IC23 and IC24: high (the row) until MREQDL (its
 * /URAS delayed outside) falls in a cycle for the upper RAM (A15 and RD or
 * WR). The upper RAM's /CAS is /MREQ or that select, a step later (the
 * gates, and C105 on the pin: the column is on the RAM's pins before
 * /CAS falls). zx8401_mux: each section's output, A and B pins (B the
 * row: the select is high before /RAS falls), the lower RAM's 7 first. */
const int zx8401_mux[ZX8401_NMUX][3] = {
    {19, 18, 17}, {16, 15, 14}, {13, 12, 11}, {23, 22, 21}, {7, 6, 5}, {4, 3, 2}, {10, 9, 8},
    {28, 18, 17}, {29, 15, 14}, {30, 12, 11}, {27, 22, 21}, {31, 6, 5}, {33, 3, 2}, {26, 24, 25}, {32, 9, 8} };
enum { ZX8401_DRASLL = 1, ZX8401_CASL = 34, ZX8401_MREQDL = 35, ZX8401_MREQL = 36, ZX8401_A15 = 37, ZX8401_RDL = 38,
       ZX8401_WRL = 39 };
typedef struct { uint8_t sel_delayed; } zx8401_state_t;

static bool zx8401_upper_sel(sim_t *s, part_t *p) {
    bool access = !(pin_in(s, p, ZX8401_RDL) && pin_in(s, p, ZX8401_WRL));
    return pin_in(s, p, ZX8401_MREQDL) || !(pin_in(s, p, ZX8401_A15) && access);
}
static void zx8401_eval(sim_t *s, part_t *p) {
    bool lower = pin_in(s, p, ZX8401_DRASLL), upper = zx8401_upper_sel(s, p);
    for (int i = 0; i < ZX8401_NMUX; i++) {
        bool sel = i < 7 ? lower : upper;
        pin_out(s, p, zx8401_mux[i][0], DRV_BIT(pin_in(s, p, zx8401_mux[i][sel ? 2 : 1])));
    }
    const zx8401_state_t *st = p->state;
    pin_out(s, p, ZX8401_CASL, DRV_BIT(pin_in(s, p, ZX8401_MREQL) || st->sel_delayed));
}
static void zx8401_commit(sim_t *s, part_t *p) {
    zx8401_state_t *st = p->state;
    uint8_t v = zx8401_upper_sel(s, p);
    if (v != st->sel_delayed) { st->sel_delayed = v; sim_mark_dirty(s, (int)(p - s->parts)); }
}
static void zx8401_reset(sim_t *s, part_t *p) { ((zx8401_state_t *)p->state)->sel_delayed = 1; }
const part_type_t PT_ZX8401 = { "ZX8401",
    "DRASLL:T A0:T A7:T DMA0:O A1:T A8:T DMA1:O A2:T A9:T DMA2:O A3:T A10:T DMA3:O A4:T A11:T DMA4:O A5:T "
    "A12:T DMA5:O VSS:P A6:T A13:T DMA6:O TS1:T TS2:T MA7:O MA6:O MA5:O MA4:O MA3:O MA2:O MA1:O MA0:O "
    "/CAS:O /MREQD:T /MREQ:T A15:T /RD:T /WR:T VDD:P",
    zx8401_eval, zx8401_commit, zx8401_reset, sizeof(zx8401_state_t), TECH_TTL };

/* LM1889 video modulator (IC14): analog, its pins are only named. */
const part_type_t PT_LM1889 = { "LM1889",
    "CHROMA_LEAD:A R-Y_IN:A CHROMA_BIAS:A B-Y_IN:A GND:P TANK_B1:A TANK_B2:A TANK_A1:A TANK_A2:A "
    "OUT_B:A OUT_A:A VIDEO_IN:A SUBCARRIER:A VCC:P VCC:P VCC:P OSC_OUT:A CHROMA_LAG:A", 0, 0, 0, 0, TECH_NMOS };


/* ------------------------------------------------------------------------ */
/* C64 ASSY 250469: memories, clock and the Super PLA                        */
/* ------------------------------------------------------------------------ */

/* 41464 dynamic RAM 64K x 4 (U10, U11): a 4164 with four data pins. /OE
 * is tied low on the board; an early write (/WE low before /CAS) takes
 * DQ1-DQ4, a read drives them until /CAS rises. Pin names as the
 * datasheet's; the board wires them to MA0-MA7 in another order (the
 * cell of an address is c64.c's business). */
static const int d41464_addr_pins[8] = { 14, 13, 12, 11, 8, 7, 6, 10 };
static const int d41464_dq_pins[4] = { 2, 3, 15, 17 };

static void d41464_eval(sim_t *s, part_t *p) {
    dram4_state_t *st = p->state;
    part_drive_bus_cached(s, p, d41464_dq_pins, 4, st->out_val, st->out_en && !pin_in(s, p, 1), &st->out_cache);
}

static void d41464_commit(sim_t *s, part_t *p) {
    dram4_state_t *st = p->state;
    int ras = pin_in(s, p, 5), cas = pin_in(s, p, 16);
    uint8_t out_en = st->out_en, out_val = st->out_val;
    if (st->ras_prev && !ras) st->row = (uint8_t)mem_addr(p, part_read_bus(s, p, d41464_addr_pins, 8));
    if (st->cas_prev && !cas && !ras) {
        st->col = (uint8_t)mem_addr(p, part_read_bus(s, p, d41464_addr_pins, 8));
        int cell = (st->row << 8) | st->col;
        if (!pin_in(s, p, 4)) {
            st->cells[cell] = (uint8_t)part_read_bus(s, p, d41464_dq_pins, 4);
            st->out_en = 0;
        } else {
            st->out_en = 1;
            st->out_val = st->cells[cell];
            if (p->mfault) st->out_val = (uint8_t)(mem_read(p, (unsigned)cell, st->out_val) & 15);
        }
    }
    if (cas) st->out_en = 0;
    st->ras_prev = (uint8_t)ras;
    st->cas_prev = (uint8_t)cas;
    if (st->out_en != out_en || st->out_val != out_val) sim_mark_dirty(s, (int)(p - s->parts));
}

/* Power-on pattern: the 4164's (64 bytes of $00, then 64 of $FF, by the
 * CPU address bit 6), on the row bit the board wires A6 to. */
static void d41464_reset(sim_t *s, part_t *p) {
    (void)s;
    dram4_state_t *st = p->state;
    for (int row = 0; row < 256; row++)
        memset(st->cells + (row << 8), (row >> st->a6_row_bit) & 1 ? 15 : 0, 256);
    st->ras_prev = st->cas_prev = 1;
    st->out_en = 0;
    st->out_cache = 0;
}

const part_type_t PT_41464 = { "41464",
    "/OE:m DQ1:B DQ2:B /WE:m /RAS:m A6:m A5:m A4:m VCC:P A7:m A3:m A2:m A1:m A0:m DQ3:B /CAS:m DQ4:B VSS:P",
    d41464_eval, d41464_commit, d41464_reset, sizeof(dram4_state_t), TECH_NMOS };

/* 27128 EPROM 16K x 8 (U4: BASIC and the KERNAL in one chip, 251913-01).
 * The data comes out while /CE and /OE are low; VPP and /PGM are only for
 * programming it. */
static void rom27128_eval(sim_t *s, part_t *p) {
    rom_state_t *st = p->state;
    bool selected = !pin_in(s, p, 20) && !pin_in(s, p, 22);
    unsigned addr = selected ? part_read_bus(s, p, rom23128_addr_pins, 14) : 0;
    part_drive_bus_cached(s, p, rom23128_data_pins, 8, rom_byte(p, st, addr), selected, &st->out_cache);
}
const part_type_t PT_27128 = { "27128 EPROM",
    "VPP:m A12:M A7:M A6:M A5:M A4:M A3:M A2:M A1:M A0:M D0:O D1:O D2:O GND:P D3:O D4:O D5:O D6:O "
    "D7:O /CE:M A10:M /OE:M A11:M A9:M A8:M A13:M /PGM:m VCC:P", rom27128_eval, 0, 0, sizeof(rom_state_t), TECH_NMOS };

/* 74LS14: hex Schmitt trigger inverter (the analog side of its inputs,
 * the reset RC and the mains, is not simulated: see c64.c). */
const part_type_t PT_74LS14 = { "74LS14",
    "1A:T 1Y:O 2A:T 2Y:O 3A:T 3Y:O GND:P 4Y:O 4A:T 5Y:O 5A:T 6Y:O 6A:T VCC:P", ls04_eval, 0, 0, 0, TECH_TTL };

/* MOS 8701 clock generator (U20): with the crystal Y1 it makes the color
 * clock (pin 8) and, divided down, the dot clock (pin 6) for the VIC.
 * Pin 7 selects PAL (to GND through J3) or NTSC. As the 250407's 74LS629:
 * the clocks toggle once per half bus cycle, enough for the board to know
 * they are alive; a broken crystal stops both (the dot clock comes from
 * it too, unlike the 250407's). The pins without a job here are named
 * NC: the datasheet's names for them weren't found. */
static void mos8701_eval(sim_t *s, part_t *p) {
    vco_state_t *st = p->state;
    pin_out(s, p, 6, st->crystal_ok ? DRV_BIT(s->clock_level) : DRV_LO);
    pin_out(s, p, 8, st->crystal_ok ? DRV_BIT(s->clock_level) : DRV_LO);
}
const part_type_t PT_8701 = { "8701 CLOCK",
    "NC GND:P NC GND:P NC DOT:O PAL/NTSC:m COLOR:O GND:P NC NC VCC:P XTAL1:A XTAL2:A VCC:P NC",
    mos8701_eval, 0, 0, sizeof(vco_state_t), TECH_NMOS };

/* The 250469's 6510, VIC-II and SID: the 8500, 8565 and 8580, the HMOS
 * versions of the same chips, run by the same cores. Same pins (the 8565
 * and the 8580 have no 12 V: their VDD is +5V and +9V here). */
const part_type_t PT_8500 = { "8500 CPU", PINS_6510, 0, 0, 0, 0, TECH_NMOS };
const part_type_t PT_8565 = { "8565 VIC-II", PINS_6569, 0, 0, 0, 0, TECH_NMOS };
const part_type_t PT_8580 = { "8580 SID", PINS_6581, 0, 0, 0, 0, TECH_NMOS };

/* 251715-01 "Super PLA" (U8): the 250469's glue in one 64 pin chip. Its
 * logic was never published, so it is modeled as the 250407 chips whose
 * jobs its pins took over (pin names from bwack's replica), wired as they
 * were on that board:
 *   the 906114 PLA (U17): the same equations (pla_logic), its VA12/VA13
 *     inputs on MA4/MA5 as the 250407's on VA4/VA5, /AEC inverted inside;
 *   the 74LS139 (U15): its /IO output decoded by A10/A11 into /VIC, /SID,
 *     the color RAM and the CIAs, these by A8/A9 into /CIA1, /CIA2, /I/O1,
 *     /I/O2;
 *   a 74LS08 gate (U27): the color RAM selected in VIC cycles too (AEC
 *     low);
 *   two 74LS257 (U25, U13): in CPU cycles (AEC high) CPU A0-A7 on MA0-MA7
 *     while /CAS is high (the row), A8-A15 when it falls (the column);
 *   the 74LS258 (U14): in VIC cycles MA6/MA7 from the VIC's A6/A7 (row)
 *     and CIA2's bank bits /VA14, /VA15 inverted (column);
 *   the 74LS373 (U26): in VIC cycles MA0-MA7, latched when /RAS falls,
 *     on A0-A7 (the color RAM and the character ROM see the VIC address);
 *   the 556 half that made the RESTORE NMI (U20): NMI follows RESTORE
 *     (the 74LS14 output, the key debounced by R17 and C59).
 * Assumed, not known: that the equations are the 906114's; RAM_R/W is
 * the CPU's R/W, held high in VIC cycles, for the RAMs and the color RAM
 * alike (the 250407 gave the color RAM the PLA's GR/W, timed by /CAS);
 * the NMI has no timer of its own (the 556 made a 1 ms pulse); PHI0 and
 * /CAS0-/CAS3 do nothing (the latter aren't connected on the board); no
 * delays inside beyond the usual one step. */
enum { SPLA_A13 = 1, SPLA_A14, SPLA_A15, SPLA_RW, SPLA_LORAM, SPLA_HIRAM, SPLA_CHAREN, SPLA_NMI, SPLA_RESTORE,
       SPLA_VA14, SPLA_VA15, SPLA_CIA2, SPLA_CHARROM, SPLA_BASIC, SPLA_KERNAL, SPLA_COLRAM = 17, SPLA_CASRAM = 22,
       SPLA_RAMRW, SPLA_CIA1 = 34, SPLA_CAS, SPLA_RAS, SPLA_PHI0, SPLA_AEC, SPLA_BA, SPLA_VIC, SPLA_VA6, SPLA_VA7,
       SPLA_SID, SPLA_ROML, SPLA_IO2, SPLA_EXROM, SPLA_GAME, SPLA_IO1 = 49, SPLA_ROMH };
static const int spla_ma[8] = { 26, 29, 27, 25, 28, 30, 24, 31 };         /* MA0-MA7 */
static const int spla_a[16] = { 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 1, 2, 3 };    /* A0-A15 */
typedef struct { uint8_t latch; int32_t ma_cache, a_cache; } superpla_state_t;

/* The PLA's inputs, in the 82S100's order (pla_in_pin): VA12/VA13 are
 * MA4/MA5, PI_AEC is AEC inverted. */
const int superpla_in_pin[PLA_NIN] = {
    [PI_A13] = SPLA_A13, [PI_A14] = SPLA_A14, [PI_A15] = SPLA_A15, [PI_VA14] = SPLA_VA14,
    [PI_CHAREN] = SPLA_CHAREN, [PI_HIRAM] = SPLA_HIRAM, [PI_LORAM] = SPLA_LORAM, [PI_CAS] = SPLA_CAS,
    [PI_VA12] = 28, [PI_VA13] = 30, [PI_GAME] = SPLA_GAME, [PI_EXROM] = SPLA_EXROM,
    [PI_RW] = SPLA_RW, [PI_AEC] = SPLA_AEC, [PI_BA] = SPLA_BA, [PI_A12] = 63 };
const int superpla_out_pin[PLA_NOUT] = {
    [PO_ROMH] = SPLA_ROMH, [PO_ROML] = SPLA_ROML, [PO_IO] = 0, [PO_GRW] = 0,
    [PO_CHAROM] = SPLA_CHARROM, [PO_KERNAL] = SPLA_KERNAL, [PO_BASIC] = SPLA_BASIC, [PO_CASRAM] = SPLA_CASRAM };

unsigned superpla_read_inputs(sim_t *s, part_t *p) {
    unsigned in = 0;
    for (int k = 0; k < PLA_NIN; k++) in |= (unsigned)pin_in(s, p, superpla_in_pin[k]) << k;
    return in ^ 1u << PI_AEC;
}

static void superpla_eval(sim_t *s, part_t *p) {
    superpla_state_t *st = p->state;
    bool cpu = pin_in(s, p, SPLA_AEC);
    unsigned a = part_read_bus(s, p, spla_a, 16);
    unsigned out = pla_logic(superpla_read_inputs(s, p));
    static const int pla_outs[][2] = { { SPLA_ROMH, PO_ROMH }, { SPLA_ROML, PO_ROML }, { SPLA_CHARROM, PO_CHAROM },
                                       { SPLA_KERNAL, PO_KERNAL }, { SPLA_BASIC, PO_BASIC }, { SPLA_CASRAM, PO_CASRAM } };
    for (int i = 0; i < 6; i++) pin_out(s, p, pla_outs[i][0], DRV_BIT((out >> pla_outs[i][1]) & 1));
    /* The I/O decoder. */
    bool y1[4] = { true, true, true, true }, y2[4] = { true, true, true, true };
    if (!((out >> PO_IO) & 1)) y1[a >> 10 & 3] = false;
    if (!y1[3]) y2[a >> 8 & 3] = false;
    pin_out(s, p, SPLA_VIC, DRV_BIT(y1[0]));
    pin_out(s, p, SPLA_SID, DRV_BIT(y1[1]));
    pin_out(s, p, SPLA_COLRAM, DRV_BIT(y1[2] && cpu));
    pin_out(s, p, SPLA_CIA1, DRV_BIT(y2[0]));
    pin_out(s, p, SPLA_CIA2, DRV_BIT(y2[1]));
    pin_out(s, p, SPLA_IO1, DRV_BIT(y2[2]));
    pin_out(s, p, SPLA_IO2, DRV_BIT(y2[3]));
    pin_out(s, p, SPLA_RAMRW, DRV_BIT(!cpu || pin_in(s, p, SPLA_RW)));
    pin_out(s, p, SPLA_NMI, DRV_BIT(pin_in(s, p, SPLA_RESTORE)));
    /* The RAM address: the CPU's row or column, or in VIC cycles MA6/MA7
     * (MA0-MA5 are the VIC's own pins). */
    bool row = pin_in(s, p, SPLA_CAS);
    if (cpu) {
        part_drive_bus_cached(s, p, spla_ma, 8, row ? a & 0xFF : a >> 8, true, &st->ma_cache);
    } else {
        unsigned hi = row ? (unsigned)(pin_in(s, p, SPLA_VA6) | pin_in(s, p, SPLA_VA7) << 1) :
                            (pin_in(s, p, SPLA_VA14) ? 0u : 1u) | (pin_in(s, p, SPLA_VA15) ? 0u : 2u);
        int32_t key = (int32_t)hi + 1000;
        if (st->ma_cache != key) {
            st->ma_cache = key;
            part_drive_bus(s, p, spla_ma, 6, 0, false);
            part_drive_bus(s, p, spla_ma + 6, 2, hi, true);
        }
    }
    /* The latch: the VIC address on A0-A7 in VIC cycles. */
    unsigned latched = !cpu && pin_in(s, p, SPLA_RAS) ? part_read_bus(s, p, spla_ma, 8) : st->latch;
    part_drive_bus_cached(s, p, spla_a, 8, latched, !cpu, &st->a_cache);
}

static void superpla_commit(sim_t *s, part_t *p) {
    superpla_state_t *st = p->state;
    if (!pin_in(s, p, SPLA_RAS)) return;
    uint8_t v = (uint8_t)part_read_bus(s, p, spla_ma, 8);
    if (v != st->latch) { st->latch = v; sim_mark_dirty(s, (int)(p - s->parts)); }
}

const part_type_t PT_SUPERPLA = { "251715-01 PLA",
    "A13:T A14:T A15:T R/W:T /LORAM:T /HIRAM:T /CHAREN:T NMI:O RESTORE:T /VA14:T /VA15:T /CIA2:O /CHARROM:O "
    "/BASIC:O /KERNAL:O VSS:P /COLRAM:O /CAS0:O /CAS1:O /CAS2:O /CAS3:O /CASRAM:O RAM_R/W:O MA6:X MA3:X MA0:X "
    "MA2:X MA4:X MA1:X MA5:X MA7:X NC VSS:P /CIA1:O /CAS:T /RAS:T PHI0:m AEC:T BA:T /VIC:O VA6:T VA7:T "
    "/SID:O /ROML:O /I/O2:O /EXROM:T /GAME:T VSS:P /I/O1:O /ROMH:O A0:X A1:X A2:X A3:X A4:X A5:X A6:X A7:X "
    "A8:T A9:T A10:T A11:T A12:T VDD:P",
    superpla_eval, superpla_commit, 0, sizeof(superpla_state_t), TECH_TTL };

/* A memory chip that can take memory faults (part_t.mfault): its address
 * lines and data bits (0 if it isn't one). A DRAM has its multiplexed
 * lines: 7 for a 4116, the chip address is (row << 8) | column. */
int part_memory(const part_t *p, int *data_bits, bool *dram) {
    int lines = 0, bits = 8;
    bool d = false;
    if (p->eval == rom2364_eval) lines = 13;
    else if (p->eval == rom2332_eval) lines = 12;
    else if (p->eval == hn613128_eval || p->eval == upd23128_eval || p->eval == rom27128_eval) lines = 14;
    else if (p->eval == sram_eval) { lines = 10; bits = 4; }
    else if (part_is_dram64(p)) { lines = 8; bits = 1; d = true; }
    else if (p->commit == dram4116_commit) { lines = 7; bits = 1; d = true; }
    else if (p->commit == d41464_commit) { lines = 8; bits = 4; d = true; }
    if (data_bits) *data_bits = bits;
    if (dram) *dram = d;
    return lines;
}
