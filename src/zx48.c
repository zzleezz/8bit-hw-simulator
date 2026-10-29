/* zx48.c - the Sinclair ZX Spectrum 48K, boards Issue 3B, 2, 1, 4B and 6A.
 *
 * HOW TIME MOVES
 *
 * Everything on this board is timed by the ULA: its 14 MHz oscillator (X1)
 * is the board clock. We simulate every half period of it (8 per CPU T
 * state): the ULA advances, the board settles, the Z80 sees an edge of its
 * clock if there was one (the ULA makes the clock, and stops it to keep
 * the CPU off the lower RAM while it reads the screen), then the memories
 * see their strobes (a commit) and the board settles again.
 *
 * What is on the board, as on the real one: the CPU and the ULA share the
 * lower 16K (eight 4116 DRAMs) through 470 ohm resistors on the data bus
 * and 330 ohm resistors after the 74LS157 address multiplexers, so either
 * side can drive its own half of the bus; the upper 32K (eight 4532, half
 * good 64K DRAMs) has its own glue (IC23-IC26) with two RC delays timing
 * its RAS and CAS; the ROM is selected by the ULA. The keyboard half rows
 * are the address lines A8-A15, through D1-D8.
 *
 * Board: liveboxandy's KiCad Issue 3B (CC0, from PABB's gerbers), checked
 * against Timex's Issue 3 schematic (356-201891, June 1983).
 *
 * THE ISSUE 2 is the same circuit, chip for chip (checked pin by pin
 * against the 3B: see tools/kicad2c.py), laid out differently and with
 * other names for a few parts: the edge connector is J3, the 9 V jack J4,
 * the 7805 U2, the ROM link LNK2, and the RAM type is wired (A14 into
 * IC26) but for LNK1, the row A7 the 4532s get. TR6 had no place on the
 * PCB: it was soldered across other parts. Its ULA was a 5C112 (or a
 * 6C001 put in later): the two differ in analog ways the simulator
 * doesn't have, so the choice only changes labels and resistor values.
 *
 * THE ISSUE 1 is the Issue 2's circuit without the upper RAM: a 16K
 * machine. The 32K came on a board of its own plugged into EXT1 and EXT2
 * (not simulated yet). Its keyboard pull-ups are the pack RP1, TR3's base
 * has D14 next to R24 (R24 alone does the logic here) and no R73: the
 * 5C ULAs pull their clock output up themselves (a 6C001 needs R73, so
 * it comes with that option), TR6 isn't there
 * (the ULA's /IORQ doesn't look at A0), and some values differ (analog).
 * Its ULA was a 5C102 or a 5C112, often replaced by a 6C001 since.
 *
 * THE ISSUE 4B is the 3B's logic but for the lower RAM multiplexers'
 * select: two spare NAND gates of IC24 (pins 1-3, 12-13) delay /RAS
 * where the 3B has the RC delay of R32, and R32 is a 0 ohm link. Gates
 * here take no time, so the 4B's R32 keeps the 3B's one step delay (35
 * ns; two 74LS00 take about 20): it stands for the gates' delay. Its
 * DC-DC converter makes -5V another way (C46, D11, D18, C79, R79 and the
 * zener D19 where the 3B has D12, R55 and D16), +12V through a charge
 * pump (C80, D17, D15), and the 9 V comes in through the ferrite links
 * L1 and L2.
 *
 * THE ISSUE 6A is the 4B's PCB with the glue in one chip: the ZX8401
 * (IC27, see parts.c) does what IC3, IC4 and IC23-IC26 do, and a 74LS04
 * (IC28) what the gates of IC23 and IC24 around them did: two inverters
 * in a row each make the upper RAM's /RAS (from /MREQ) and /WRITE (from
 * /WR), two more delay the lower RAM's /RAS for the multiplexers' select
 * (through the RAS/CAS link LK5, 1-2 3-5 4-6 for a Ferranti ULA, and R87:
 * a 0 ohm link that, as the 4B's R32, stands for the gates' delay). R88
 * delays the upper RAM's /RAS for the ZX8401's select (MREQDL). There is
 * no R57: the Z80's /RFSH goes to the edge connector only. LK1-LK4 and
 * the LKU and LKV links are for a Saga ULA and the video lines on the
 * edge connector: open.
 * What differs between the boards is in zx_variant_t. */
#include "zx48.h"
#include "zx48-rom.h"
#include "sokol/sokol_app.h"   /* key codes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Z80 pins (the address pins A0..A15, the data pins D0..D7). */
static const int z80_addr_pins[16] = { 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 1, 2, 3, 4, 5 };
static const int z80_data_pins[8] = { 14, 15, 12, 8, 7, 9, 10, 13 };
enum { Z_CLK = 6, Z_INT = 16, Z_NMI = 17, Z_HALT = 18, Z_MREQ = 19, Z_IORQ = 20, Z_RD = 21, Z_WR = 22,
       Z_BUSAK = 23, Z_WAIT = 24, Z_BUSRQ = 25, Z_RESET = 26, Z_M1 = 27, Z_RFSH = 28 };

/* ------------------------------------------------------------------------ */
/* Z80 (IC2)                                                                 */
/* ------------------------------------------------------------------------ */

/* ZXHW_CONTENTION=1: for every T state in which the CPU puts out an
 * address in 0x4000-0x7FFF without /MREQ (the first T state of a memory
 * cycle, or an internal one), print when it started (T states since /INT
 * went low) and how many T states the ULA held the clock. */
static bool contention_log;
static uint64_t frame_step, rise_step;
static bool rise_contended;

/* The outputs of the CPU, from the core's pins: the address and the bus
 * control lines while AOE says the CPU drives them (not in reset, not
 * while it gave the bus away), the data bus while DOE says so. */
static void zx_cpu_eval(sim_t *s, part_t *p) {
    zx48_t *z = ZX(p->user);
    uint64_t pins = z->cpu_pins;
    bool aoe = (pins & Z80_AOE) != 0;
    part_drive_bus_cached(s, p, z80_addr_pins, 16, Z80_GET_ADDR(pins), aoe, &z->cache_addr);
    pin_out(s, p, Z_MREQ, aoe ? DRV_BIT(!(pins & Z80_MREQ)) : DRV_Z);
    pin_out(s, p, Z_IORQ, aoe ? DRV_BIT(!(pins & Z80_IORQ)) : DRV_Z);
    pin_out(s, p, Z_RD, aoe ? DRV_BIT(!(pins & Z80_RD)) : DRV_Z);
    pin_out(s, p, Z_WR, aoe ? DRV_BIT(!(pins & Z80_WR)) : DRV_Z);
    pin_out(s, p, Z_M1, DRV_BIT(!(pins & Z80_M1)));
    pin_out(s, p, Z_RFSH, DRV_BIT(!(pins & Z80_RFSH)));
    pin_out(s, p, Z_HALT, DRV_BIT(!(pins & Z80_HALT)));
    pin_out(s, p, Z_BUSAK, DRV_BIT(!(pins & Z80_BUSAK)));
    part_drive_bus_cached(s, p, z80_data_pins, 8, Z80_GET_DATA(pins), (pins & Z80_DOE) != 0, &z->cache_data);
}

/* An edge of the CPU clock: the core (z80.h, stepped on every edge with
 * the Zilog bus timing) gets the clock level and what it sees on its input
 * pins, and puts out its pins for the half cycle that starts. */
static void cpu_edge(board_t *b, int clk) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    part_t *p = &s->parts[z->p_cpu];
    z->last_edge = s->cycle;
    if (p->dead) return;
    if (contention_log) {
        if (!clk && rise_contended) {
            uint64_t high = frame_step - rise_step;
            printf("contend t=%llu delay=%llu\n", (unsigned long long)(rise_step / 8),
                   (unsigned long long)((high - 5) / 8));
        }
        if (clk) rise_step = frame_step;
    }
    uint64_t pins = clk ? Z80_CLK : 0;
    Z80_SET_DATA(pins, (uint64_t)part_read_bus(s, p, z80_data_pins, 8));
    if (!pin_in(s, p, Z_INT)) pins |= Z80_INT;
    if (!pin_in(s, p, Z_NMI)) pins |= Z80_NMI;
    if (!pin_in(s, p, Z_WAIT)) pins |= Z80_WAIT;
    if (!pin_in(s, p, Z_RESET)) pins |= Z80_RES;
    if (!pin_in(s, p, Z_BUSRQ)) pins |= Z80_BUSRQ;
    z->in_reset = (pins & Z80_RES) != 0;
    uint64_t out = z80_tick(&z->cpu, pins);
    if (out != z->cpu_pins) { z->cpu_pins = out; sim_mark_dirty(s, z->p_cpu); }
    if (contention_log && clk) {
        uint16_t a = Z80_GET_ADDR(out);
        rise_contended = (out & Z80_AOE) && (a & 0xC000) == 0x4000 && !(out & (Z80_MREQ | Z80_IORQ));
    }
}

static const part_type_t PT_Z80 = { "Z80A",
    "A11:O A12:O A13:O A14:O A15:O CLK:m D4:B D3:B D5:B D6:B VCC:P D2:B D7:B D0:B D1:B /INT:m /NMI:m "
    "/HALT:O /MREQ:O /IORQ:O /RD:O /WR:O /BUSAK:O /WAIT:m /BUSRQ:m /RESET:m /M1:O /RFSH:O GND:P A0:O "
    "A1:O A2:O A3:O A4:O A5:O A6:O A7:O A8:O A9:O A10:O", zx_cpu_eval, 0, 0, 0, TECH_NMOS };

/* ------------------------------------------------------------------------ */
/* Time                                                                      */
/* ------------------------------------------------------------------------ */

/* ZXHW_TRACE=FROM:COUNT prints the lower RAM signals at every half
 * period of T states FROM..FROM+COUNT (for debugging the timing). */
static uint64_t trace_from, trace_to;
static FILE *ula_log_file;      /* ZXHW_ULALOG=path[:from]: the ULA at every step */
static uint64_t ula_log_step, ula_log_from;     /* (from T state 'from' on) */
static int netlvl(sim_t *s, const char *name) {
    int n = sim_find_net(s, name);
    if (n < 0) return -1;
    const net_t *r = &s->nets[s->nets[n].root];
    return r->strength == S_Z ? 2 : r->level;
}
static unsigned netbus(sim_t *s, const char *fmt, int n) {
    unsigned v = 0;
    char name[32];
    for (int i = 0; i < n; i++) {
        snprintf(name, sizeof(name), fmt, i);
        v |= (unsigned)(netlvl(s, name) == 1) << i;
    }
    return v;
}
static void trace_step(board_t *b, int k) {
    sim_t *s = &b->sim;
    zx48_t *z = ZX(b);
    ula_state_t *u = s->parts[z->p_ula].state;
    printf("%llu.%d C=%3u.%u V=%3u clk=%d mreq=%d rd=%d wr=%d A=%04X D=%02X | LRAS=%d S=%d LCAS=%d WE=%d LA=%02X UD=%02X ae=%d\n",
           (unsigned long long)s->cycle, k, u->c, u->half, u->v, netlvl(s, "Net-(IC2-/CLK)"), netlvl(s, "/MREQ"),
           netlvl(s, "/RD"), netlvl(s, "/WR"), netbus(s, "A%d", 16), netbus(s, "D%d", 8), netlvl(s, "/LRAS"),
           netlvl(s, "Net-(IC3-S)"), netlvl(s, "/LCAS"), netlvl(s, "LDRAM_WE"), netbus(s, "LA%d", 7),
           netbus(s, "Net-(IC1-D%d)", 8), u->ae);
}

/* One half period of the oscillator. */
static void zx_step(board_t *b) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    ula_step(s, &s->parts[z->p_ula]);
    if (contention_log) {
        static bool irq_prev;
        bool irq = ((ula_state_t *)s->parts[z->p_ula].state)->irq;
        frame_step = irq && !irq_prev ? 0 : frame_step + 1;
        irq_prev = irq;
    }
    sim_settle(s);
    int clk = sim_read(s, z->pin_clk);
    if (clk != z->clk) {
        z->clk = clk;
        cpu_edge(b, clk);
        sim_settle(s);
    }
    /* The memories and delays only act on edges: if no net changed since
     * the last commit there is nothing to see. */
    if (s->changes != z->changes_seen) {
        sim_commit(s);
        sim_settle(s);
        z->changes_seen = s->changes;
    }
    board_sample(b);
    if (ula_log_file && s->cycle >= ula_log_from) {
        char line[160];
        ula_log(s->parts[z->p_ula].state, ula_log_step++, line, sizeof(line));
        fprintf(ula_log_file, "%s\n", line);
    }
}

/* The power on reset: C27 discharged at power on keeps /RESET low until
 * R31 charges it. */
static void reset_tick(board_t *b) {
    zx48_t *z = ZX(b);
    if (z->pin_reset_cap < 0) return;
    sim_drive(&b->sim, z->pin_reset_cap, b->sim.cycle < z->reset_until ? DRV_LO : DRV_Z);
}

static void zx_run(board_t *b, int cycles) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    for (int i = 0; i < cycles; i++) {
        s->cycle++;
        reset_tick(b);
        for (int k = 0; k < ZX_STEPS_PER_CYCLE; k++) {
            zx_step(b);
            if (s->cycle >= trace_from && s->cycle < trace_to) trace_step(b, k);
        }
        z->cpu_running = s->cycle - z->last_edge < 1000;
    }
}

/* ------------------------------------------------------------------------ */
/* The board                                                                 */
/* ------------------------------------------------------------------------ */

/* The upper RAM's type links (zx_variant_t.uram_links): none (the Issue
 * 1, no upper RAM), the 3B's and 4B's six (TI with TI-3 or TI-4, OKI with
 * OKI-L or OKI-H), the Issue 2's LNK1 (the row A7 low or high: TI only). */
enum { ULINKS_NONE, ULINKS_TI_OKI, ULINKS_LNK1 };

/* What differs between the Spectrum boards (machine_t.variant). */
typedef struct {
    const char *edge;           /* the edge connector */
    const char *power_in;       /* where the 9 V comes in: its pin on +9V */
    const char *regulator;      /* the 7805 */
    /* The power supply, by what is broken (zx_power): the parts the 9 V
     * goes through, those +12V needs, those -5V needs, and the -5V part
     * the supply is driven from (its pin on -5V). */
    const char *in_parts, *p12_parts, *m5_parts, *m5_pin;
    const char *kept_16k;       /* upper RAM glue a 16K board still needs (the 4B's IC24), or NULL */
    const char *with_6c001;     /* fitted only with a 6C001 (the Issue 1's R73), or NULL */
    int uram_links;             /* ULINKS_*: the upper RAM's type links */
    const char *rom_link;       /* the ROM's link (JROM1, LNK2): zx_rom_fitted() */
    /* Links of the board itself (the Issue 6A's): one as fitted, and its
     * legs ("1-2 3-5"), and those left open; NULL if none. */
    const char *link, *link_legs, *links_open;
} zx_variant_t;

static const zx_variant_t *V(board_t *b) { return b->m->variant; }

static int zx_rail_of(const char *name) {
    if (!strcmp(name, "GND")) return RAIL_GND;
    if (!strcmp(name, "+5V")) return RAIL_VCC;
    return RAIL_NONE;
}

static int zx_kind_of(const char *ref) {
    if (!strncmp(ref, "IC", 2)) return PART_IC;
    if (!strncmp(ref, "TR", 2) || !strncmp(ref, "TV", 2)) return !strncmp(ref, "TV", 2) ? PART_CONNECTOR : PART_OTHER;
    if (!strcmp(ref, "U1")) return PART_CONNECTOR;     /* the Issue 2's modulator, the 3B's TV1 */
    if (ref[0] == 'R' && ((ref[1] >= '0' && ref[1] <= '9') || ref[1] == 'P')) return PART_RESISTOR;
    if (!strncmp(ref, "JROM", 4) || !strncmp(ref, "TI", 2) || !strncmp(ref, "OKI", 3) || !strncmp(ref, "LNK", 3))
        return PART_LINK;
    if (!strncmp(ref, "LK", 2) && ((ref[2] >= '0' && ref[2] <= '9') || ref[2] == 'U' || ref[2] == 'V'))
        return PART_LINK;     /* the Issue 6A's */
    if (!strncmp(ref, "KB", 2) || !strncmp(ref, "EAR", 3) || !strncmp(ref, "MIC", 3) || !strcmp(ref, "E1") ||
        ref[0] == 'J') return PART_CONNECTOR;
    return PART_OTHER;
}

static const part_def_t zx_part_defs[] = {
    { "IC1", &PT_ULA, "ULA", ROLE_CHIP, "6C001\nvideo, I/O",
      "Ferranti ULA 6C001: video, CPU clock and contention, lower RAM control, port 0xFE" },
    { "IC2", &PT_Z80, "CPU", ROLE_CHIP, "Z80A", "Z80A CPU, 3.5 MHz" },
    { "IC5", &PT_UPD23128, "ROM", ROLE_ROM, "uPD23128\n$0000-$3FFF", "NEC uPD23128 ROM, $0000-$3FFF" },
    { "IC6", &PT_4116, "LRAM0", ROLE_VRAM, "4116\nbit D0\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D0 of $4000-$7FFF (the screen is here)" },
    { "IC7", &PT_4116, "LRAM1", ROLE_VRAM, "4116\nbit D1\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D1 of $4000-$7FFF (the screen is here)" },
    { "IC8", &PT_4116, "LRAM2", ROLE_VRAM, "4116\nbit D2\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D2 of $4000-$7FFF (the screen is here)" },
    { "IC9", &PT_4116, "LRAM3", ROLE_VRAM, "4116\nbit D3\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D3 of $4000-$7FFF (the screen is here)" },
    { "IC10", &PT_4116, "LRAM4", ROLE_VRAM, "4116\nbit D4\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D4 of $4000-$7FFF (the screen is here)" },
    { "IC11", &PT_4116, "LRAM5", ROLE_VRAM, "4116\nbit D5\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D5 of $4000-$7FFF (the screen is here)" },
    { "IC12", &PT_4116, "LRAM6", ROLE_VRAM, "4116\nbit D6\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D6 of $4000-$7FFF (the screen is here)" },
    { "IC13", &PT_4116, "LRAM7", ROLE_VRAM, "4116\nbit D7\n$4000-$7FFF",
      "4116 DRAM, lower RAM: data bit D7 of $4000-$7FFF (the screen is here)" },
    { "IC15", &PT_4532, "URAM0", ROLE_RAM, "4532\nbit D0\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D0 of $8000-$FFFF" },
    { "IC16", &PT_4532, "URAM1", ROLE_RAM, "4532\nbit D1\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D1 of $8000-$FFFF" },
    { "IC17", &PT_4532, "URAM2", ROLE_RAM, "4532\nbit D2\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D2 of $8000-$FFFF" },
    { "IC18", &PT_4532, "URAM3", ROLE_RAM, "4532\nbit D3\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D3 of $8000-$FFFF" },
    { "IC19", &PT_4532, "URAM4", ROLE_RAM, "4532\nbit D4\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D4 of $8000-$FFFF" },
    { "IC20", &PT_4532, "URAM5", ROLE_RAM, "4532\nbit D5\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D5 of $8000-$FFFF" },
    { "IC21", &PT_4532, "URAM6", ROLE_RAM, "4532\nbit D6\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D6 of $8000-$FFFF" },
    { "IC22", &PT_4532, "URAM7", ROLE_RAM, "4532\nbit D7\n$8000-$FFFF", "4532 DRAM, upper RAM: data bit D7 of $8000-$FFFF" },
    { "IC3", &PT_74LS157, "MUX", ROLE_GLUE, "74LS157",
      "74LS157 lower RAM address mux: CPU A0-A3 (row), A7-A10 (column), switched by /RAS" },
    { "IC4", &PT_74LS157, "MUX", ROLE_GLUE, "74LS157",
      "74LS157 lower RAM address mux: CPU A4-A6 (row), A11-A13 (column), switched by /RAS" },
    { "IC25", &PT_74LS157, "MUX", ROLE_GLUE, "74LS157", "74LS157 upper RAM address mux (row, column)" },
    { "IC26", &PT_74LS157, "MUX", ROLE_GLUE, "74LS157",
      "74LS157 upper RAM address mux (row, column; A7 set by the TI/OKI links)" },
    { "IC23", &PT_74LS32, "OR", ROLE_GLUE, "74LS32", "74LS32 OR gates: upper RAM /RAS, /CAS, mux select, /WRITE" },
    { "IC24", &PT_74LS00, "NAND", ROLE_GLUE, "74LS00", "74LS00 NAND gates: upper RAM select (A15 and RD or WR)" },
    { "IC27", &PT_ZX8401, "GLUE", ROLE_CHIP, "ZX8401",
      "ZX8401 (PCF1306P): the RAM address multiplexers (lower and upper) and the upper RAM's /CAS and select" },
    { "IC28", &PT_74LS04, "INV", ROLE_GLUE, "74LS04",
      "74LS04 inverters: upper RAM /RAS and /WRITE, the lower RAM multiplexers' select delay" },
    { "IC14", &PT_LM1889, "PAL", ROLE_OTHER, "LM1889\nnot\nsimulated",
      "LM1889 PAL video modulator (analog, not simulated: the monitor shows the ULA's picture)" },
};

/* The Issue 2's chips are those, but for its ULA (the option) and IC26
 * (A14 wired in, LNK1 giving the row A7). The Issue 1's too, but for the
 * upper RAM it hasn't. The ULAs, in the order of zx_ula_ids. */
static const char *const zx_ula_ids[] = { "5c102", "5c112", "6c001" };
static const part_def_t zx_ulas[3] = {
    { "IC1", &PT_ULA, "ULA", ROLE_CHIP, "5C102\nvideo, I/O",
      "Ferranti ULA 5C102: video, CPU clock and contention, lower RAM control, port 0xFE "
      "(simulated as a 6C001: they differ in analog ways only)" },
    { "IC1", &PT_ULA, "ULA", ROLE_CHIP, "5C112\nvideo, I/O",
      "Ferranti ULA 5C112: video, CPU clock and contention, lower RAM control, port 0xFE "
      "(simulated as a 6C001: they differ in analog ways only)" },
    { "IC1", &PT_ULA, "ULA", ROLE_CHIP, "6C001\nvideo, I/O",
      "Ferranti ULA 6C001: video, CPU clock and contention, lower RAM control, port 0xFE" },
};
static const part_def_t zx_i2_ic26 = { "IC26", &PT_74LS157, "MUX", ROLE_GLUE, "74LS157",
    "74LS157 upper RAM address mux (row, column; A14 wired in, row A7 set by LNK1)" };

static int zx_option(const machine_t *m, const char *id) {
    for (int i = 0; i < m->noptions; i++) if (!strcmp(m->options[i].id, id)) return i;
    return -1;
}

/* Which ULA is fitted: an index in zx_ulas (the 3B's is a 6C001). */
static int zx_ula(board_t *b) {
    int o = zx_option(b->m, "ula");
    if (o < 0) return 2;
    for (int k = 0; k < 3; k++)
        if (!strcmp(b->m->options[o].value_ids[ZX(b)->ula_type], zx_ula_ids[k])) return k;
    return 2;
}

/* What goes with the ULA: its clock output, and on the Issue 1 R73 (the
 * pull-up a 6C001 needs, fitted when one is put in). */
static void zx_ula_fitted(board_t *b) {
    zx48_t *z = ZX(b);
    bool c6 = zx_ula(b) == 2;
    ((ula_state_t *)b->sim.parts[z->p_ula].state)->clk_pulls_up = !c6;
    int r = V(b)->with_6c001 ? sim_find_part(&b->sim, V(b)->with_6c001) : -1;
    if (r >= 0) b->sim.parts[r].unfitted = b->sim.parts[r].removed = !c6;
}

/* The 4B's IC24: its two spare gates are the mux select delay. */
static const part_def_t zx_i4b_ic24 = { "IC24", &PT_74LS00, "NAND", ROLE_GLUE, "74LS00",
    "74LS00 NAND gates: upper RAM select (A15 and RD or WR), and two in a row delaying /RAS for the "
    "lower RAM multiplexers" };

static const part_def_t *zx_i4b_part_def(board_t *b, int part) {
    return !strcmp(b->sim.parts[part].ref, "IC24") ? &zx_i4b_ic24 : NULL;
}

static const part_def_t *zx_early_part_def(board_t *b, int part) {
    const char *ref = b->sim.parts[part].ref;
    if (!strcmp(ref, "IC1")) return &zx_ulas[zx_ula(b)];
    if (!strcmp(ref, "IC26")) return &zx_i2_ic26;
    return NULL;
}

/* The upper RAM chips but the TI 4532-3 (zx_part_defs has those): their
 * labels, made the first time. */
static const part_def_t *zx_uram_part_def(board_t *b, int part) {
    static part_def_t defs[4][8];
    static char text[4][8][2][160];
    static const char *const chip[4] = { "4532-4", "3732-L", "3732-H", "4164" };
    static const char *const what[4] = { "TI 4532-4 DRAM (a 4164 with the rows with A7 high good)",
                                         "OKI 3732-L DRAM (a 4164 with the columns with A7 low good)",
                                         "OKI 3732-H DRAM (a 4164 with the columns with A7 high good)",
                                         "4164 DRAM (all of it good)" };
    int k = ZX(b)->uram_chips - 1;
    const char *ref = b->sim.parts[part].ref;
    if (k < 0 || strncmp(ref, "IC", 2) || atoi(ref + 2) < 15 || atoi(ref + 2) > 22) return NULL;
    int bit = atoi(ref + 2) - 15;
    part_def_t *d = &defs[k][bit];
    if (!d->ref) {
        const part_def_t *base = NULL;
        for (size_t i = 0; i < sizeof(zx_part_defs) / sizeof(zx_part_defs[0]); i++)
            if (!strcmp(zx_part_defs[i].ref, ref)) base = &zx_part_defs[i];
        if (!base) return NULL;
        *d = *base;
        snprintf(text[k][bit][0], sizeof(text[k][bit][0]), "%s\nbit D%d\n$8000-$FFFF", chip[k], bit);
        snprintf(text[k][bit][1], sizeof(text[k][bit][1]), "%s, upper RAM: data bit D%d of $8000-$FFFF",
                 what[k], bit);
        d->detail = text[k][bit][0];
        d->info = text[k][bit][1];
    }
    return d;
}


/* Series resistors between logic nets: the 470 ohm on the data bus, the
 * 330 ohm on the lower RAM address lines, /INT, /IORQ to the ULA, the mux
 * select, /ROMCS and /RFSH to /RAS (the lower RAM refresh in the border). */
static const char *const series[] = {
    "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R17", "R18", "R19", "R20", "R21", "R22", "R23",
    "R26", "R27", "R33", "R57",
};
/* The links go with the chips fitted: the ROM's (JROM1, the Issues 1
 * and 2: LNK2; the "rom" and "romlink" options, zx_rom_fitted()) and the
 * upper RAM's type links (the "uchips" and "ulinks" options,
 * zx_uram_fitted()). */
#define ZX_DCDC_3B .p12_parts = "D15", .m5_parts = "D11 D12 D16 R55", .m5_pin = "D12"
static const zx_variant_t zx_3b = { .edge = "E1", .power_in = "J7", .regulator = "REG1", .in_parts = "",
                                    ZX_DCDC_3B, .uram_links = ULINKS_TI_OKI, .rom_link = "JROM1" };
static const zx_variant_t zx_i2 = { .edge = "J3", .power_in = "J4", .regulator = "U2", .in_parts = "",
                                    ZX_DCDC_3B, .uram_links = ULINKS_LNK1, .rom_link = "LNK2" };
static const zx_variant_t zx_i1 = { .edge = "J3", .power_in = "J4", .regulator = "U2", .in_parts = "",
                                    ZX_DCDC_3B, .with_6c001 = "R73", .uram_links = ULINKS_NONE,
                                    .rom_link = "LNK2" };
static const zx_variant_t zx_i4b = { .edge = "E1", .power_in = "L2", .regulator = "REG1", .in_parts = "L1 L2",
                                     .p12_parts = "D15 D17 C80", .m5_parts = "D11 D18 R79 D19", .m5_pin = "D19",
                                     .kept_16k = "IC24", .uram_links = ULINKS_TI_OKI,
                                     .rom_link = "JROM1" };
static const zx_variant_t zx_i6a = { .edge = "E1", .power_in = "L2", .regulator = "REG1", .in_parts = "L1 L2",
                                     .p12_parts = "D15 D17 C80", .m5_parts = "D11 D18 R79 D19", .m5_pin = "D19",
                                     .uram_links = ULINKS_TI_OKI, .rom_link = "JROM1",
                                     .link = "LK5", .link_legs = "1-2 3-5 4-6",
                                     .links_open = "LK1 LK2 LK3 LK4 LKU LKV" };

static const part_def_t *zx_part_def(board_t *b, int part) {
    const part_def_t *d = zx_uram_part_def(b, part);
    if (d) return d;
    if (ZX(b)->rom_chip && part == ZX(b)->p_rom) {
        static part_def_t hitachi;
        if (!hitachi.ref) {
            for (size_t i = 0; i < sizeof(zx_part_defs) / sizeof(zx_part_defs[0]); i++)
                if (!strcmp(zx_part_defs[i].ref, "IC5")) hitachi = zx_part_defs[i];
            hitachi.detail = "HN613128\n$0000-$3FFF";
            hitachi.info = "Hitachi HN613128 ROM, $0000-$3FFF";
        }
        return &hitachi;
    }
    if (V(b) == &zx_i4b) return zx_i4b_part_def(b, part);
    if (V(b) == &zx_i1 || V(b) == &zx_i2) return zx_early_part_def(b, part);
    return NULL;
}

/* The connectors: the keyboard ribbons, the sockets, the edge connector
 * (its pins named after their nets, see setup_edge). */
static const conn_def_t zx_conn_defs[] = {
    { "KB1", "KEYBOARD LINES", "1=KB0 2=KB1 3=KB2 4=KB3 5=KB4" },
    { "KB2", "KEYBOARD HALF ROWS", "1=A11 2=A10 3=A9 4=A12 5=A13 6=A8 7=A14 8=A15" },
    { "EAR1", "EAR", "1=GND 2=EAR_IN" },
    { "MIC1", "MIC", "1=GND 2=MIC_OUT" },
    { "TV1", "TV", "1=RF_SUPPLY 2=VIDEO 3=GND" },
    { "J7", "9V DC", "1=GND 2=+9V 3=NC" },
    { "E1", "EDGE CONNECTOR", "" },
};
static const conn_def_t zx_conn_defs_i2[] = {
    { "KB1", "KEYBOARD LINES", "1=KB4 2=KB3 3=KB2 4=KB1 5=KB0" },
    { "KB2", "KEYBOARD HALF ROWS", "1=A11 2=A10 3=A9 4=A12 5=A13 6=A8 7=A14 8=A15" },
    { "J2", "EAR", "1=GND 2=EAR_IN" },
    { "J1", "MIC", "1=GND 2=MIC_OUT" },
    { "U1", "TV", "1=RF_SUPPLY 2=VIDEO 3=GND" },
    { "J4", "9V DC", "1=+9V 2=GND 3=+9V" },
    { "J3", "EDGE CONNECTOR", "" },
};
static const conn_def_t zx_conn_defs_i4b[] = {
    { "KB1", "KEYBOARD LINES", "1=KB0 2=KB1 3=KB2 4=KB3 5=KB4" },
    { "KB2", "KEYBOARD HALF ROWS", "1=A11 2=A10 3=A9 4=A12 5=A13 6=A8 7=A14 8=A15" },
    { "EAR1", "EAR", "1=GND 2=EAR_IN" },
    { "MIC1", "MIC", "1=GND 2=MIC_OUT" },
    { "TV1", "TV", "1=RF_SUPPLY 2=VIDEO 3=SW1 4=GND" },
    { "J7", "9V DC", "1=0V 2=+9V" },
    { "E1", "EDGE CONNECTOR", "" },
};
static const conn_def_t zx_conn_defs_i1[] = {
    { "KB1", "KEYBOARD LINES", "1=KB4 2=KB3 3=KB2 4=KB1 5=KB0" },
    { "KB2", "KEYBOARD HALF ROWS", "1=A11 2=A10 3=A9 4=A12 5=A13 6=A8 7=A14 8=A15" },
    { "J2", "EAR", "1=GND 2=EAR_IN" },
    { "J1", "MIC", "1=GND 2=MIC_OUT" },
    { "U1", "TV", "1=RF_SUPPLY 2=VIDEO 3=GND" },
    { "J4", "9V DC", "1=+9V 2=GND 3=+9V" },
    { "J3", "EDGE CONNECTOR", "" },
    { "EXT1", "32K RAM BOARD (ADDRESS)", "1=GND 2=A5 3=A10 4=A7 5=A8 6=A2 7=A14 8=A0 9=A12 10=A1 11=A13 12=A3 "
      "13=A9 14=A6 15=A11 16=A4" },
    { "EXT2", "32K RAM BOARD (DATA)", "1=+5V 2=D6 3=/MREQ 4=/RD 5=/WR 6=D7 7=D0 8=D3 9=A15 10=D4 11=D2 12=D5 "
      "13=D1 14=GND" },
};

static part_t *find(board_t *b, const char *ref) {
    int i = sim_find_part(&b->sim, ref);
    return i >= 0 ? &b->sim.parts[i] : NULL;
}

/* Turn a two pin part into one active element (diodes, transistors). */
static void make_element(board_t *b, const char *ref, int pin_a, int pin_b, int type) {
    part_t *p = find(b, ref);
    if (!p || part_pin(p, pin_a) < 0 || part_pin(p, pin_b) < 0) return;
    resistor_state_t *st = p->state;
    if (!st) { p->state = st = calloc(1, sizeof(*st)); p->state_size = sizeof(*st); }
    p->kind = PART_RESISTOR;
    st->nel = 1;
    st->el[0] = (element_t){ part_pin(p, pin_a), part_pin(p, pin_b), EL_ANALOG };
    element_set_type(&b->sim, p, 0, type);
}

/* The edge connector: each pin's function is the name of its net. */
static void setup_edge(board_t *b) {
    sim_t *s = &b->sim;
    part_t *e1 = find(b, V(b)->edge);
    for (int k = 0; e1 && k < e1->npins; k++) {
        int pin = e1->first_pin + k, net = s->pins[pin].net;
        if (net < 0 || b->pin_func[pin]) continue;
        const char *n = s->nets[net].name;
        if (strncmp(n, "Net-(", 5) && strncmp(n, "unconnected", 11)) b->pin_func[pin] = n;
    }
}

static void zx_init_keymap(zx48_t *z) {
    /* As floooh's zx.h: column = address line A8+n, line = KB0..KB4. */
    kbd_init(&z->kbd, 1);
    kbd_register_modifier(&z->kbd, 0, 0, 0);     /* caps shift */
    kbd_register_modifier(&z->kbd, 1, 7, 1);     /* symbol shift */
    const char *keymap =
        " zxcv" "asdfg" "qwert" "12345" "09876" "poiuy" " lkjh" "  mnb"
        " ZXCV" "ASDFG" "QWERT" "     " "     " "POIUY" " LKJH" "  MNB"
        " : ?/" "     " "   <>" "!@#$%" "_)('&" "\";   " " =+-^" "  .,*";
    for (int layer = 0; layer < 3; layer++)
        for (int column = 0; column < 8; column++)
            for (int line = 0; line < 5; line++) {
                int c = keymap[layer * 40 + column * 5 + line];
                if (c != ' ') kbd_register_key(&z->kbd, c, column, line, layer > 0 ? 1 << (layer - 1) : 0);
            }
    kbd_register_key(&z->kbd, ' ', 7, 0, 0);
    kbd_register_key(&z->kbd, 0x0F, 7, 1, 0);    /* symbol shift */
    kbd_register_key(&z->kbd, 0x0E, 0, 0, 0);    /* caps shift */
    kbd_register_key(&z->kbd, 0x08, 3, 4, 1);    /* cursor left: caps shift + 5 */
    kbd_register_key(&z->kbd, 0x0A, 4, 4, 1);    /* down: caps shift + 6 */
    kbd_register_key(&z->kbd, 0x0B, 4, 3, 1);    /* up: caps shift + 7 */
    kbd_register_key(&z->kbd, 0x09, 4, 2, 1);    /* right: caps shift + 8 */
    kbd_register_key(&z->kbd, 0x07, 3, 0, 1);    /* edit: caps shift + 1 */
    kbd_register_key(&z->kbd, 0x0C, 4, 0, 1);    /* delete: caps shift + 0 */
    kbd_register_key(&z->kbd, 0x03, 7, 0, 1);    /* break: caps shift + space */
    kbd_register_key(&z->kbd, 0x0D, 6, 0, 0);    /* enter */
}

static int pin_of(board_t *b, const char *ref, int number) {
    part_t *p = find(b, ref);
    return p ? part_pin(p, number) : -1;
}

/* The (lowest numbered) pin of a part on a net, or -1: for the parts the
 * boards draw either way round. Pads with no number (a jack's mounting
 * pin "MP") only if no numbered one is. */
static int pin_on_net(board_t *b, const char *ref, const char *net) {
    sim_t *s = &b->sim;
    part_t *p = find(b, ref);
    int best = -1;
    for (int k = 0; p && k < p->npins; k++) {
        int pin = p->first_pin + k, n = s->pins[pin].number;
        if (s->pins[pin].net < 0 || strcmp(s->nets[s->pins[pin].net].name, net)) continue;
        int bn = best < 0 ? 0 : s->pins[best].number;
        if (best < 0 || (n > 0 && (bn <= 0 || n < bn))) best = pin;
    }
    return best;
}

/* A link of several legs, as fitted: "1-2 3-4" joins legs 1 and 2, 3 and 4. */
static void set_link(board_t *b, const char *ref, const char *legs) {
    part_t *p = find(b, ref);
    if (!p || !p->state) return;
    resistor_state_t *st = p->state;
    st->nel = 0;
    int a, c, n;
    while (st->nel < 16 && sscanf(legs, " %d-%d%n", &a, &c, &n) == 2) {
        st->el[st->nel++] = (element_t){ part_pin(p, a), part_pin(p, c), EL_LINK };
        legs += n;
    }
}

static void zx_uram_fitted(board_t *b);
static void zx_rom_fitted(board_t *b);

/* The board's own links (zx_variant_t.link): one fitted as its legs say,
 * the others open (not fitted: they join nothing). */
static void zx_links_fitted(board_t *b) {
    if (V(b)->link) set_link(b, V(b)->link, V(b)->link_legs);
    for (const char *l = V(b)->links_open; l && *l; ) {
        size_t len = strcspn(l, " ");
        char ref[16];
        snprintf(ref, sizeof(ref), "%.*s", (int)len, l);
        part_t *p = find(b, ref);
        if (p && p->state) {
            ((resistor_state_t *)p->state)->nel = 0;
            p->unfitted = p->removed = true;
        }
        l += len + (l[len] == ' ');
    }
}

static void zx_create(board_t *b) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    contention_log = getenv("ZXHW_CONTENTION") != NULL;
    const char *ul = getenv("ZXHW_ULALOG");
    if (ul && !ula_log_file) {
        char path[1024];
        snprintf(path, sizeof(path), "%s", ul);
        char *colon = strrchr(path, ':');
        if (colon) { ula_log_from = strtoull(colon + 1, NULL, 10); *colon = 0; }
        ula_log_file = fopen(path, "w");
    }
    const char *tr = getenv("ZXHW_TRACE");
    if (tr) {
        unsigned long long from = 0, count = 0;
        sscanf(tr, "%llu:%llu", &from, &count);
        trace_from = from;
        trace_to = from + count;
    }
    z->p_cpu = sim_find_part(s, "IC2");
    z->p_ula = sim_find_part(s, "IC1");
    z->p_rom = sim_find_part(s, "IC5");
    z->pin_clk = part_pin(&s->parts[z->p_cpu], Z_CLK);
    ((rom_state_t *)s->parts[z->p_rom].state)->data = zx48_rom;
    ((ula_state_t *)s->parts[z->p_ula].state)->fb = b->screen;
    ((ula_state_t *)s->parts[z->p_ula].state)->logging = ula_log_file != NULL;
    zx_ula_fitted(b);
    /* Resistors and links that do logic. */
    for (size_t i = 0; i < sizeof(series) / sizeof(series[0]); i++) {
        part_t *p = find(b, series[i]);
        if (p && p->state && ((resistor_state_t *)p->state)->nel) element_set_type(s, p, 0, EL_SERIES);
    }
    part_t *r24 = find(b, "R24");                   /* /PHICPU to TR3's base */
    if (r24 && r24->state) ((resistor_state_t *)r24->state)->el[0].type = EL_LINK;
    /* RC delays, from the side on the net named to the other: R32 and the
     * inputs of the lower RAM multiplexers (/RAS switches them to the
     * column only after the 4116s latched the row; on the 4B the delay of
     * the gates after R32), R70/C64 and R71/C63 (the upper RAM timing).
     * The Issue 6A: R87 (the gates' delay, as R32) and R88 (MREQDL). */
    static const struct { const char *ref, *from; } delays[] = {
        { "R32", "/LRAS" }, { "R70", "Net-(IC25-S)" }, { "R71", "/URAS" }, { "R87", "Net-(J1-Pin_5)" },
        { "R88", "/URAS" } };
    for (size_t i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        part_t *p = find(b, delays[i].ref);
        int from = pin_on_net(b, delays[i].ref, delays[i].from);
        if (!p || !p->state || from < 0) continue;
        element_t *e = &((resistor_state_t *)p->state)->el[0];
        e->pin_a = from;
        e->pin_b = part_pin(p, s->pins[from].number == 1 ? 2 : 1);
        element_set_type(s, p, 0, EL_DELAY);
    }
    zx_uram_fitted(b);
    zx_rom_fitted(b);
    zx_links_fitted(b);
    /* The keyboard diodes (anode pin 2, cathode pin 1 on A8-A15), TR3 (the
     * clock inverter: base to collector) and TR6 (emitter follower: A0
     * keeps the ULA's /IOREQ high). */
    for (int i = 1; i <= 8; i++) {
        char ref[8];
        snprintf(ref, sizeof(ref), "D%d", i);
        make_element(b, ref, 2, 1, EL_DIODE);
    }
    make_element(b, "TR3", 2, 1, EL_NPN);
    make_element(b, "TR6", 2, 3, EL_FOLLOWER);
    /* Which DRAM holds which bit: the one whose D pin is on Dn (upper) or
     * on the ULA's Dn (lower). */
    for (int i = 0; i < 8; i++) z->lram_part[i] = z->uram_part[i] = -1;
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        int net = part_pin(p, 2) >= 0 ? s->pins[part_pin(p, 2)].net : -1;
        const char *name = net >= 0 ? s->nets[net].name : "";
        if (part_is_dram64(p) && name[0] == 'D' &&
            name[1] >= '0' && name[1] <= '7')
            z->uram_part[name[1] - '0'] = i;
        if (p->commit == PT_4116.commit && !strncmp(name, "Net-(IC1-D", 10) && name[10] >= '0' && name[10] <= '7')
            z->lram_part[name[10] - '0'] = i;
    }
    /* Keyboard pins: the KB2 pad on the anode of the diode of each address
     * line, the KB1 pads 1-5. */
    for (int c = 0; c < 8; c++) {
        z->pin_kb_row[c] = -1;
        char an[8];
        snprintf(an, sizeof(an), "A%d", 8 + c);
        for (int i = 1; i <= 8; i++) {
            char ref[8];
            snprintf(ref, sizeof(ref), "D%d", i);
            int k = pin_of(b, ref, 1), a = pin_of(b, ref, 2);
            if (k < 0 || a < 0 || s->pins[k].net < 0 || strcmp(s->nets[s->pins[k].net].name, an)) continue;
            part_t *kb2 = find(b, "KB2");
            for (int j = 0; kb2 && j < kb2->npins; j++)
                if (s->pins[kb2->first_pin + j].net == s->pins[a].net && atoi(s->bd->pads[kb2->first_pin + j].name) < 10)
                    z->pin_kb_row[c] = kb2->first_pin + j;
        }
    }
    for (int l = 0; l < 5; l++) {
        char net[24];
        snprintf(net, sizeof(net), "Net-(IC1-KB%d)", l);
        z->pin_kb_line[l] = pin_on_net(b, "KB1", net);
    }
    zx_init_keymap(z);
    /* Buses, for the bus bitmap (speed only). */
    char name[32];
    int bit = 0;
    static const struct { const char *fmt; int n; } buses[] = {
        { "A%d", 16 }, { "D%d", 8 }, { "LA%d", 7 }, { "UA%d", 8 }, { "Net-(IC1-D%d)", 8 } };
    for (size_t k = 0; k < sizeof(buses) / sizeof(buses[0]); k++)
        for (int i = 0; i < buses[k].n; i++) {
            snprintf(name, sizeof(name), buses[k].fmt, i);
            sim_set_bus_bit(s, sim_find_net(s, name), bit++);
        }
    /* Power, reset, the RESET button (edge connector /RESET to GND). */
    z->pin_9v = pin_on_net(b, V(b)->power_in, "+9V");
    z->pin_12v = pin_on_net(b, "D15", "+12V");
    z->pin_m5v = pin_on_net(b, V(b)->m5_pin, "-5V");
    z->pin_reset_cap = pin_on_net(b, "C27", "/RESET");
    part_t *e1 = find(b, V(b)->edge);
    for (int k = 0; e1 && k < e1->npins; k++) {
        int pin = e1->first_pin + k, net = s->pins[pin].net;
        if (net < 0) continue;
        if (!strcmp(s->nets[net].name, "/RESET") && b->pin_reset < 0) b->pin_reset = pin;
        if (!strcmp(s->nets[net].name, "GND") && b->pin_reset_gnd < 0) b->pin_reset_gnd = pin;
    }
    setup_edge(b);
}

static void zx_power_on(board_t *b) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    z->cpu_pins = z80_init(&z->cpu);
    z->in_reset = true;
    z->clk = 0;
    z->last_edge = 0;
    z->reset_until = ZX_FREQ * 7 / 100;     /* 70 ms: R31 (220K) charging C27 (1 uF) */
    memset(b->screen, 0, ULA_SCREEN_W * ULA_SCREEN_H * sizeof(uint32_t));
    sim_drive(s, z->pin_9v, DRIVE(S_SUPPLY, 1));     /* the 9 V input (see zx_power) */
}

/* The power supply, from what is broken. The 7805 makes +5V from the 9 V
 * input; the DC-DC converter (TR4, TR5 and the coil T1 oscillating) makes
 * +12V through D15 and -5V through the charge pump C46, D11, D12, R55 and
 * the zener D16 (the 4B: see the top of the file, and zx_variant_t). The
 * 4116s need all three: without one they don't work (and without -5V the
 * real ones overheat and die). The crystal X1 is the ULA's oscillator:
 * without it there is no clock and no picture. */
static bool broken(board_t *b, const char *ref) {
    part_t *p = find(b, ref);
    if (!p) return false;
    /* A lifted leg breaks it too. */
    for (int k = 0; k < p->npins; k++) if (b->sim.pins[p->first_pin + k].fault == PF_OPEN) return true;
    return p->removed || p->rfault != RF_OK;
}

/* Is a part of a list ("D11 D12") broken? */
static bool any_broken(board_t *b, const char *list) {
    for (const char *s = list; *s; ) {
        size_t len = strcspn(s, " ");
        char ref[16];
        snprintf(ref, sizeof(ref), "%.*s", (int)len, s);
        if (len && broken(b, ref)) return true;
        s += len + (s[len] == ' ');
    }
    return false;
}

static void zx_power(board_t *b) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    bool v5 = !broken(b, V(b)->regulator) && !any_broken(b, V(b)->in_parts);
    bool osc = v5 && !broken(b, "TR4") && !broken(b, "TR5") && !broken(b, "T1") && !broken(b, "R58") &&
               !broken(b, "R59") && !broken(b, "R60") && !broken(b, "R61");
    bool p12 = osc && !any_broken(b, V(b)->p12_parts);
    bool m5 = osc && !any_broken(b, V(b)->m5_parts);
    sim_drive(s, z->pin_12v, p12 ? DRIVE(S_SUPPLY, 1) : DRV_Z);
    sim_drive(s, z->pin_m5v, m5 ? DRIVE(S_SUPPLY, 0) : DRV_Z);
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        if (p->kind != PART_IC) continue;
        p->no_supply = !v5 || (p->commit == PT_4116.commit && (!p12 || !m5));
    }
    ((ula_state_t *)s->parts[z->p_ula].state)->running = !broken(b, "X1");
}

/* Pressed keys: a key joins its half row (after the diode) and its line.
 * And the supplies (before the power state of the chips is computed). */
static void zx_joins(board_t *b) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    zx_power(b);
    for (int l = 0; l < 5; l++)
        for (int c = 0; c < 8; c++) {
            if (!((z->key_matrix[l] >> c) & 1)) continue;
            int r = z->pin_kb_row[c], k = z->pin_kb_line[l];
            if (r < 0 || k < 0 || s->pins[r].fault == PF_OPEN || s->pins[k].fault == PF_OPEN) continue;
            sim_join(s, s->pins[r].net, s->pins[k].net);
        }
}

static void zx_sync_keys(board_t *b) {
    zx48_t *z = ZX(b);
    bool changed = false;
    for (int l = 0; l < 5; l++) {
        uint16_t m = z->kbd.scanout_column_masks[l];
        if (m != z->key_matrix[l]) { z->key_matrix[l] = m; changed = true; }
    }
    if (changed) board_rebuild(b);
}

static void zx_key_down(board_t *b, int key) { kbd_key_down(&ZX(b)->kbd, key); zx_sync_keys(b); }
static void zx_key_up(board_t *b, int key) { kbd_key_up(&ZX(b)->kbd, key); zx_sync_keys(b); }
static void zx_frame_update(board_t *b, uint32_t us) { kbd_update(&ZX(b)->kbd, us); zx_sync_keys(b); }

static int zx_host_key(int keycode) {
    switch (keycode) {
    case SAPP_KEYCODE_ENTER: return 0x0D;
    case SAPP_KEYCODE_BACKSPACE: return 0x0C;
    case SAPP_KEYCODE_LEFT: return 0x08;
    case SAPP_KEYCODE_RIGHT: return 0x09;
    case SAPP_KEYCODE_UP: return 0x0B;
    case SAPP_KEYCODE_DOWN: return 0x0A;
    case SAPP_KEYCODE_ESCAPE: return 0x03;
    case SAPP_KEYCODE_TAB: return 0x0F;
    case SAPP_KEYCODE_LEFT_SHIFT: return 0x0E;
    default: return 0;
    }
}

static int zx_host_char(int c) { return c; }

static void zx_monitor(board_t *b, uint32_t *out) {
    zx48_t *z = ZX(b);
    part_t *ula = &b->sim.parts[z->p_ula];
    b->no_signal = ula->dead;
    for (int i = 0; i < ULA_SCREEN_W * ULA_SCREEN_H; i++) {
        uint32_t c = b->screen[i];
        if (ula->dead) {    /* no video signal: snow */
            b->snow_seed = b->snow_seed * 1103515245u + 12345u;
            uint32_t g = ((b->snow_seed >> 24) & 0xFF) * 3 / 4;
            c = 0xFF000000u | (g << 16) | (g << 8) | g;
        }
        out[i] = c;
    }
}

/* ------------------------------------------------------------------------ */
/* Loading snapshots                                                         */
/* ------------------------------------------------------------------------ */

/* Which CPU address bit a net carries, following links ("A14" joined to
 * a mux input by the TI1 link): 0..15, or -2 for GND, -3 for +5V. */
static int addr_bit_of_net(sim_t *s, int net) {
    int root = s->nets[net].root;
    for (int i = 0; i < s->nnets; i++) {
        if (s->nets[i].root != root) continue;
        const char *n = s->nets[i].name;
        if (n[0] == 'A' && n[1] >= '0' && n[1] <= '9') return atoi(n + 1);
        if (!strcmp(n, "GND")) return -2;
        if (!strcmp(n, "+5V")) return -3;
    }
    return -1;
}

/* The row and column address bit a DRAM address pin gets, from the board:
 * its net comes from a 74LS157 output (maybe through a series resistor);
 * that section's B input is the row (the mux select is high before /RAS
 * falls), its A input the column. */
static void dram_pin_bits(board_t *b, int pin, int *row, int *col) {
    sim_t *s = &b->sim;
    static const int sec[4][3] = { {4, 2, 3}, {7, 5, 6}, {9, 11, 10}, {12, 14, 13} };   /* Y, A, B */
    *row = *col = -1;
    int net = s->pins[pin].net;
    for (int hop = 0; hop < 2 && net >= 0; hop++) {
        int next = -1;
        for (int i = 0; i < s->npins; i++) {
            if (s->pins[i].net != net) continue;
            part_t *p = &s->parts[s->pins[i].part];
            /* A 74LS157's four sections, or the ZX8401's fifteen. */
            bool zx8401 = p->eval == PT_ZX8401.eval;
            if (p->eval == PT_74LS157.eval || zx8401) {
                const int (*tab)[3] = zx8401 ? zx8401_mux : sec;
                for (int k = 0; k < (zx8401 ? ZX8401_NMUX : 4); k++)
                    if (part_pin(p, tab[k][0]) == i) {
                        *col = addr_bit_of_net(s, s->pins[part_pin(p, tab[k][1])].net);
                        *row = addr_bit_of_net(s, s->pins[part_pin(p, tab[k][2])].net);
                        return;
                    }
            }
            if (p->kind == PART_RESISTOR && p->state && ((resistor_state_t *)p->state)->nel) {
                element_t *e = &((resistor_state_t *)p->state)->el[0];
                if (e->type == EL_SERIES) next = s->pins[e->pin_a == i ? e->pin_b : e->pin_a].net;
            }
        }
        net = next;
    }
}

/* The row and column address bit of each address pin of DRAM PART, as
 * the board wires it now; the number of address pins. */
static int dram_wiring(board_t *b, int part, int rb[8], int cb[8]) {
    static const int addr_pins[8] = { 5, 7, 6, 12, 11, 10, 13, 9 };
    part_t *p = &b->sim.parts[part];
    int bits = p->commit == PT_4116.commit ? 7 : 8;
    for (int i = 0; i < bits; i++) dram_pin_bits(b, part_pin(p, addr_pins[i]), &rb[i], &cb[i]);
    return bits;
}

static int wired_cell(uint16_t addr, int bits, const int rb[8], const int cb[8]) {
    int row = 0, col = 0;
    for (int i = 0; i < bits; i++) {
        int r = rb[i] >= 0 ? (addr >> rb[i]) & 1 : rb[i] == -3, c = cb[i] >= 0 ? (addr >> cb[i]) & 1 : cb[i] == -3;
        row |= r << i;
        col |= c << i;
    }
    return (row << 8) | col;
}

/* The DRAM cell of a CPU address, as the board wires it. */
static int dram_cell(board_t *b, int part, uint16_t addr) {
    int rb[8], cb[8], bits = dram_wiring(b, part, rb, cb);
    return wired_cell(addr, bits, rb, cb);
}

/* The cell table of zx_ram_poke() goes: the next snapshot makes it again
 * from the wiring as it is then (faults merge nets, a repair splits them). */
static void zx_forget_cells(zx48_t *z) {
    for (int u = 0; u < 2; u++) {
        free(z->ram_cells[u]);
        z->ram_cells[u] = NULL;
    }
}

/* Write a byte straight into the DRAM chips (a backdoor, like the C64's
 * program loading: it works even on a broken board). The cell of each
 * address is computed once per snapshot (zx_forget_cells()). */
static void zx_ram_poke(board_t *b, uint16_t addr, uint8_t v) {
    zx48_t *z = ZX(b);
    if (addr < 0x4000) return;
    int upper = addr >= 0x8000;
    if (upper && z->uram_part[0] < 0) return;       /* no upper RAM */
    if (!z->ram_cells[0]) {
        for (int u = 0; u < 2; u++) {
            if (z->uram_part[0] < 0 && u) break;
            int rb[8], cb[8], bits = dram_wiring(b, u ? z->uram_part[0] : z->lram_part[0], rb, cb);
            z->ram_cells[u] = calloc(u ? 32768 : 16384, sizeof(int));
            for (int a = 0; a < (u ? 32768 : 16384); a++)
                z->ram_cells[u][a] = wired_cell((uint16_t)((u ? 0x8000 : 0x4000) + a), bits, rb, cb);
        }
    }
    int cell = z->ram_cells[upper][addr - (upper ? 0x8000 : 0x4000)];
    for (int bit = 0; bit < 8; bit++) {
        dram_state_t *st = b->sim.parts[upper ? z->uram_part[bit] : z->lram_part[bit]].state;
        st->cells[cell] = (v >> bit) & 1;
    }
}

static uint8_t ram_peek(board_t *b, uint16_t addr) {
    zx48_t *z = ZX(b);
    if (addr < 0x4000) return zx48_rom[addr];
    int upper = addr >= 0x8000;
    if (upper && z->uram_part[0] < 0) return 0xFF;  /* no upper RAM: the bus floats high */
    int cell = dram_cell(b, upper ? z->uram_part[0] : z->lram_part[0], addr);
    uint8_t v = 0;
    for (int bit = 0; bit < 8; bit++)
        v |= (uint8_t)(((dram_state_t *)b->sim.parts[upper ? z->uram_part[bit] : z->lram_part[bit]].state)->cells[cell] << bit);
    return v;
}

/* Start the CPU at pc with the registers of a snapshot. */
static void cpu_start(board_t *b, uint16_t pc) {
    zx48_t *z = ZX(b);
    z->cpu_pins = z80_prefetch(&z->cpu, pc);
    sim_mark_dirty(&b->sim, z->p_cpu);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* .sna (48K): 27 bytes of registers, then the RAM from 0x4000; PC is on
 * the stack (the snapshot was taken inside an interrupt). */
static bool load_sna(board_t *b, const uint8_t *d, size_t n, char *msg, int msglen) {
    zx_forget_cells(ZX(b));
    zx48_t *z = ZX(b);
    if (n != 27 + 49152) { snprintf(msg, msglen, "not a 48K .sna (%zu bytes)", n); return false; }
    for (int a = 0; a < 49152; a++) zx_ram_poke(b, (uint16_t)(0x4000 + a), d[27 + a]);
    z80_t *c = &z->cpu;
    c->i = d[0];
    c->hl2 = le16(d + 1); c->de2 = le16(d + 3); c->bc2 = le16(d + 5); c->af2 = le16(d + 7);
    c->hl = le16(d + 9); c->de = le16(d + 11); c->bc = le16(d + 13);
    c->iy = le16(d + 15); c->ix = le16(d + 17);
    c->iff1 = c->iff2 = (d[19] & 4) != 0;
    c->r = d[20];
    c->af = le16(d + 21);
    uint16_t sp = le16(d + 23);
    c->im = d[25] & 3;
    ((ula_state_t *)b->sim.parts[z->p_ula].state)->border = d[26] & 7;
    uint16_t pc = (uint16_t)(ram_peek(b, sp) | ram_peek(b, (uint16_t)(sp + 1)) << 8);
    c->sp = (uint16_t)(sp + 2);
    cpu_start(b, pc);
    return true;
}

/* .z80, versions 1 to 3, 48K: a header, then the memory, maybe compressed
 * (ED ED count byte). */
static size_t z80_unpack(const uint8_t *src, size_t n, uint8_t *dst, size_t max, bool v1) {
    size_t i = 0, o = 0;
    while (i < n && o < max) {
        if (v1 && i + 3 < n && src[i] == 0 && src[i + 1] == 0xED && src[i + 2] == 0xED && src[i + 3] == 0) break;
        if (i + 3 < n && src[i] == 0xED && src[i + 1] == 0xED) {
            for (int k = 0; k < src[i + 2] && o < max; k++) dst[o++] = src[i + 3];
            i += 4;
        } else {
            dst[o++] = src[i++];
        }
    }
    return o;
}

#define Z80_MEM 49152

/* 'mem' (Z80_MEM bytes) is where the 48K are unpacked before going into
 * the DRAM chips: given by load_z80(), not static, so boards in parallel
 * threads can load at the same time. */
static bool load_z80_into(board_t *b, const uint8_t *d, size_t n, uint8_t *mem, char *msg, int msglen) {
    zx48_t *z = ZX(b);
    uint16_t pc = le16(d + 6);
    uint8_t flags = d[12] == 255 ? 1 : d[12];
    if (pc) {           /* version 1: the 48K follow */
        size_t got = (flags & 0x20) ? z80_unpack(d + 30, n - 30, mem, Z80_MEM, true)
                                    : (n - 30 < Z80_MEM ? n - 30 : Z80_MEM);
        if (!(flags & 0x20)) memcpy(mem, d + 30, got);
        if (got != Z80_MEM) { snprintf(msg, msglen, ".z80: memory is %zu bytes, not 48K", got); return false; }
    } else {            /* versions 2 and 3: pages of 16K */
        if (n < 35) { snprintf(msg, msglen, ".z80: the header is cut short"); return false; }
        size_t hlen = le16(d + 30), at = 32 + hlen;
        pc = le16(d + 32);
        int hw = d[34];
        bool v2 = hlen == 23;
        if ((v2 && hw > 1) || (!v2 && hw != 0 && hw != 1 && hw != 3)) {
            snprintf(msg, msglen, ".z80 for another machine (hardware %d): only the 48K", hw);
            return false;
        }
        int pages = 0;          /* a bit each of the three found whole */
        while (at + 3 <= n) {
            size_t len = le16(d + at);
            int page = d[at + 2];
            at += 3;
            size_t in = len == 0xFFFF ? 16384 : len;
            if (at + in > n) break;
            int base = page == 8 ? 0 : page == 4 ? 16384 : page == 5 ? 32768 : -1;
            if (base >= 0) {
                size_t got = 16384;
                if (len == 0xFFFF) memcpy(mem + base, d + at, 16384);
                else got = z80_unpack(d + at, in, mem + base, 16384, false);
                if (got == 16384) pages |= 1 << (base / 16384);
            }
            at += in;
        }
        if (pages != 7) {
            int found = (pages & 1) + (pages >> 1 & 1) + (pages >> 2 & 1);
            snprintf(msg, msglen, ".z80: %d whole pages of the 48K found, not 3", found);
            return false;
        }
    }
    for (int a = 0; a < 49152; a++) zx_ram_poke(b, (uint16_t)(0x4000 + a), mem[a]);
    z80_t *c = &z->cpu;
    c->a = d[0]; c->f = d[1]; c->bc = le16(d + 2); c->hl = le16(d + 4); c->sp = le16(d + 8);
    c->i = d[10]; c->r = (uint8_t)((d[11] & 0x7F) | ((flags & 1) << 7));
    c->de = le16(d + 13); c->bc2 = le16(d + 15); c->de2 = le16(d + 17); c->hl2 = le16(d + 19);
    c->af2 = (uint16_t)(d[21] << 8 | d[22]);
    c->iy = le16(d + 23); c->ix = le16(d + 25);
    c->iff1 = d[27] != 0; c->iff2 = d[28] != 0;
    c->im = d[29] & 3;
    ((ula_state_t *)b->sim.parts[z->p_ula].state)->border = (flags >> 1) & 7;
    cpu_start(b, pc);
    return true;
}

static bool load_z80(board_t *b, const uint8_t *d, size_t n, char *msg, int msglen) {
    zx_forget_cells(ZX(b));
    if (n < 30) { snprintf(msg, msglen, "too short for a .z80"); return false; }
    uint8_t *mem = malloc(Z80_MEM);
    bool ok = mem && load_z80_into(b, d, n, mem, msg, msglen);
    free(mem);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* ROM cartridges                                                            */
/* ------------------------------------------------------------------------ */

/* A 16K ROM in the edge connector, like the Interface 2 cartridges and the
 * diagnostic ROM boards: it holds /ROMCS high (stronger than the ULA,
 * which drives it through R33) so the internal ROM stays quiet, and puts
 * its own byte on the data bus for memory reads below 0x4000. Its pins are
 * the edge connector's: a bad contact there is a bad contact for it.
 * A 32K or 64K image is an EPROM of several 16K banks behind a switch on
 * its upper address lines (like DesTestSwitch): one bank is seen at a
 * time, and flipping the switch restarts the Spectrum. */
#define ZX_MAX_BANKS 16
typedef struct {
    char name[64];
    uint8_t rom[16384];         /* the selected bank */
    int pin_romcs, pin_mreq, pin_rd, pin_a[16], pin_d[8];
    int nbanks, bank;
    uint8_t image[];            /* nbanks * 16K */
} zxcart_t;

static void zxcart_eval(sim_t *s, part_t *p) {
    zxcart_t *c = p->state;
    bool sel = !sim_read(s, c->pin_mreq) && !sim_read(s, c->pin_rd) && !sim_read(s, c->pin_a[14]) &&
               !sim_read(s, c->pin_a[15]);
    unsigned a = 0;
    for (int i = 0; sel && i < 14; i++) a |= (unsigned)sim_read(s, c->pin_a[i]) << i;
    for (int i = 0; i < 8; i++) sim_drive(s, c->pin_d[i], sel ? DRV_BIT((c->rom[a] >> i) & 1) : DRV_Z);
    sim_drive(s, c->pin_romcs, DRV_HI);
}

/* The edge connector pin on a net, or -1. */
static int edge_pin(board_t *b, const char *net) {
    sim_t *s = &b->sim;
    part_t *e1 = find(b, V(b)->edge);
    for (int k = 0; e1 && k < e1->npins; k++) {
        int pin = e1->first_pin + k;
        if (s->pins[pin].net >= 0 && !strcmp(s->nets[s->pins[pin].net].name, net)) return pin;
    }
    return -1;
}

static void cart_pins(board_t *b, zxcart_t *c, bool plugged) {
    sim_t *s = &b->sim;
    int ins[18] = { c->pin_mreq, c->pin_rd }, nin = 2;
    for (int i = 0; i < 16; i++) ins[nin++] = c->pin_a[i];
    for (int i = 0; i < nin; i++) if (ins[i] >= 0) s->pins[ins[i]].flags = plugged ? PIN_IN | PIN_SENSE : 0;
    int outs[9] = { c->pin_romcs };
    for (int i = 0; i < 8; i++) outs[1 + i] = c->pin_d[i];
    for (int i = 0; i < 9; i++) if (outs[i] >= 0) { s->pins[outs[i]].flags = plugged ? PIN_OUT : 0; s->pins[outs[i]].drive = DRV_Z; }
}

static void zx_remove_cart(board_t *b) {
    zx48_t *z = ZX(b);
    part_t *e1 = find(b, V(b)->edge);
    if (!z->cart || !e1) return;
    cart_pins(b, z->cart, false);
    free(z->cart);
    z->cart = e1->state = NULL;
    e1->state_size = 0;
    board_config_changed(b);
    e1->eval = NULL;
    board_rebuild(b);
    board_power_on(b);
}

static const char *zx_cart_name(board_t *b) { return ZX(b)->cart ? ((zxcart_t *)ZX(b)->cart)->name : NULL; }

static int zx_cart_banks(board_t *b) { return ZX(b)->cart ? ((zxcart_t *)ZX(b)->cart)->nbanks : 0; }
static int zx_cart_bank(board_t *b) { return ZX(b)->cart ? ((zxcart_t *)ZX(b)->cart)->bank : 0; }
static const char *zx_cart_bank_info(board_t *b, int bank) { (void)b; (void)bank; return "16K, 0x0000-0x3FFF"; }

static void zx_set_cart_bank(board_t *b, int bank) {
    zxcart_t *c = ZX(b)->cart;
    if (!c || bank < 0 || bank >= c->nbanks) return;
    c->bank = bank;
    memcpy(c->rom, c->image + (size_t)bank * 16384, 16384);
    board_config_changed(b);
    board_rebuild(b);
    board_power_on(b);
}

static bool plug_rom(board_t *b, const char *path, const uint8_t *d, size_t n, char *msg, int msglen) {
    zx48_t *z = ZX(b);
    part_t *e1 = find(b, V(b)->edge);
    if (!n || n % 16384 || n / 16384 > ZX_MAX_BANKS || !e1) {
        snprintf(msg, msglen, "a ROM cartridge is 16384 bytes (or banks of 16384), this is %zu", n);
        return false;
    }
    zx_remove_cart(b);
    zxcart_t *c = calloc(1, sizeof(*c) + n);
    snprintf(c->name, sizeof(c->name), "%s", board_basename(path));
    c->nbanks = (int)(n / 16384);
    memcpy(c->image, d, n);
    memcpy(c->rom, d, 16384);
    c->pin_romcs = edge_pin(b, "/ROMCS");
    c->pin_mreq = edge_pin(b, "/MREQ");
    c->pin_rd = edge_pin(b, "/RD");
    char name[8];
    for (int i = 0; i < 16; i++) { snprintf(name, sizeof(name), "A%d", i); c->pin_a[i] = edge_pin(b, name); }
    for (int i = 0; i < 8; i++) { snprintf(name, sizeof(name), "D%d", i); c->pin_d[i] = edge_pin(b, name); }
    cart_pins(b, c, true);
    z->cart = e1->state = c;
    e1->state_size = 0;         /* a save state leaves the cartridge alone */
    board_config_changed(b);
    e1->eval = zxcart_eval;
    /* In with the power off: switch the Spectrum on again. */
    board_rebuild(b);
    board_power_on(b);
    snprintf(msg, msglen, "ROM cartridge %s plugged into the edge connector", c->name);
    if (c->nbanks > 1) snprintf(msg + strlen(msg), msglen - strlen(msg), ", bank 0 of %d", c->nbanks);
    return true;
}

/* Snapshots: what the header says. */
static const file_kind_t zx_snapshot = { "snapshot", "Load the snapshot", false, -1 };
static const file_kind_t zx_rom = { "ROM cartridge", "Plug the ROM", false, ROLE_ROM };

static const file_kind_t *zx_file_kind(const char *path, char *info, int len, bool *bad) {
    const file_kind_t *state = board_state_file_kind(path, info, len, bad);
    if (state) return state;
    size_t pl = strlen(path);
    bool sna = pl > 4 && !strcasecmp(path + pl - 4, ".sna"), z80 = pl > 4 && !strcasecmp(path + pl - 4, ".z80");
    bool rom = pl > 4 && (!strcasecmp(path + pl - 4, ".rom") || !strcasecmp(path + pl - 4, ".bin"));
    if (!sna && !z80 && !rom) return NULL;
    FILE *f = fopen(path, "rb");
    uint8_t h[40] = {0};
    size_t n = f ? fread(h, 1, sizeof(h), f) : 0;
    long size = 0;
    if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
    *bad = false;
    if (rom) {
        /* A C64 image of 16K or 32K is a size a Spectrum ROM can be too:
         * the contents tell. */
        uint8_t *img = size > 0 && size <= ZX_MAX_BANKS * 16384 + 2 ? malloc((size_t)size) : NULL;
        f = img ? fopen(path, "rb") : NULL;
        bool c64 = f && fread(img, 1, (size_t)size, f) == (size_t)size && rom_image_is_c64(img, (size_t)size);
        if (f) fclose(f);
        free(img);
        if (c64) { snprintf(info, len, "a C64 ROM image (CBM80 or 6502 vectors)"); *bad = true; }
        else if (size == 16384) snprintf(info, len, "16K ROM for the edge connector");
        else if (size > 0 && size % 16384 == 0 && size / 16384 <= ZX_MAX_BANKS)
            snprintf(info, len, "switch ROM: %ld banks of 16K", size / 16384);
        else { snprintf(info, len, "%ld bytes: not a 16K ROM", size); *bad = true; }
        return &zx_rom;
    }
    if (sna) {
        if (size == 27 + 49152) snprintf(info, len, "48K snapshot");
        else { snprintf(info, len, "%ld bytes: not a 48K snapshot", size); *bad = true; }
        return &zx_snapshot;
    }
    if (n < 30) { snprintf(info, len, "too short for a snapshot"); *bad = true; return &zx_snapshot; }
    if (h[6] | h[7]) { snprintf(info, len, "48K snapshot (version 1)"); return &zx_snapshot; }
    unsigned hlen = (unsigned)(h[30] | h[31] << 8), hw = h[34];
    bool v2 = hlen == 23, ok = v2 ? hw <= 1 : hw == 0 || hw == 1 || hw == 3;
    if (ok) snprintf(info, len, "48K snapshot (version %d)", v2 ? 2 : 3);
    else { snprintf(info, len, "for another model (hardware %u): only the 48K", hw); *bad = true; }
    return &zx_snapshot;
}

static bool zx_load(board_t *b, const char *path, bool run, char *msg, int msglen) {
    (void)run;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, msglen, "can't open %s", path); return false; }
    /* Not static: boards may load in parallel threads. */
    uint8_t *buf = malloc(256 * 1024);
    size_t n = buf ? fread(buf, 1, 256 * 1024, f) : 0;
    fclose(f);
    size_t len = strlen(path);
    bool ok, snapshot = false;
    if (!buf) { snprintf(msg, msglen, "out of memory"); ok = false; }
    else if (len > 4 && (!strcasecmp(path + len - 4, ".rom") || !strcasecmp(path + len - 4, ".bin")))
        ok = plug_rom(b, path, buf, n, msg, msglen);
    else if (len > 4 && !strcasecmp(path + len - 4, ".sna")) ok = snapshot = load_sna(b, buf, n, msg, msglen);
    else if (len > 4 && !strcasecmp(path + len - 4, ".z80")) ok = snapshot = load_z80(b, buf, n, msg, msglen);
    else { snprintf(msg, msglen, "%s: the Spectrum takes .sna and .z80 snapshots, .rom cartridges", path); ok = false; }
    if (snapshot) snprintf(msg, msglen, "snapshot %s loaded", path);
    free(buf);
    return ok;
}

/* A save state was loaded: the RAM cell table (zx_ram_poke) is a cache of
 * the wiring, made once, not machine state: keep the one there was. */
static void zx_rom_pin_names(board_t *b);

static void zx_state_loaded(board_t *b, const board_t *before) {
    for (int u = 0; u < 2; u++) ZX(b)->ram_cells[u] = ((const zx48_t *)before)->ram_cells[u];
    zx_rom_pin_names(b);
}

/* The DRAM cell of a CPU address: lower RAM $4000-$7FFF, upper $8000-$FFFF. */
static int zx_ram_cell(board_t *b, int part, uint16_t addr) {
    zx48_t *z = ZX(b);
    for (int i = 0; i < 8; i++) {
        if (z->lram_part[i] == part) return addr >= 0x4000 && addr < 0x8000 ? dram_cell(b, part, addr) : -1;
        if (z->uram_part[i] == part) return addr >= 0x8000 ? dram_cell(b, part, addr) : -1;
    }
    return -1;
}

/* The cartridge is the state of E1: sim_free() frees it with the parts. */
static void zx_destroy(board_t *b) {
    zx48_t *z = ZX(b);
    free(z->ram_cells[0]);
    free(z->ram_cells[1]);
}

static const char *zx_cpu_status(board_t *b) {
    zx48_t *z = ZX(b);
    return !z->cpu_running ? "stopped (no clock)" : z->in_reset ? "in reset" : "running";
}

static void zx_cpu_regs(board_t *b, char *buf, int len) {
    zx48_t *z = ZX(b);
    snprintf(buf, len, "PC=%04X AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X IR=%04X IM%d %s", z->cpu.pc,
             z->cpu.af, z->cpu.bc, z->cpu.de, z->cpu.hl, z->cpu.sp, z->cpu.ir, z->cpu.im,
             !z->cpu_running ? "(no clock)" : z->in_reset ? "(in reset)" : "");
}

const uint8_t *zx48_rom_image(void) { return zx48_rom; }

/* The text on the picture, matched against the ROM's character set (at
 * 0x3D00, characters 32-127), cell by cell like a person reading it. */
static void zx_print_text(board_t *b) {
    const uint32_t *px = b->screen;
    /* ZXHW_RAMT=1: also the last good RAM address the ROM found at boot
     * (P_RAMT, 23732): 7FFF on a 16K Spectrum, FFFF on a 48K. */
    if (getenv("ZXHW_RAMT")) printf("P_RAMT %04X\n", ram_peek(b, 23732) | ram_peek(b, 23733) << 8);
    for (int row = 0; row < 24; row++) {
        char line[64];
        int n = 0;
        for (int col = 0; col < 32; col++) {
            int x0 = 48 + col * 8, y0 = 48 + row * 8;
            uint32_t bg = px[y0 * ULA_SCREEN_W + x0];
            uint8_t cell[8];
            for (int y = 0; y < 8; y++) {
                cell[y] = 0;
                for (int x = 0; x < 8; x++)
                    if (px[(y0 + y) * ULA_SCREEN_W + x0 + x] != bg) cell[y] |= (uint8_t)(0x80 >> x);
            }
            int ch = '?';
            for (int c = 32; c < 128; c++) {
                const uint8_t *g = &zx48_rom[0x3D00 + (c - 32) * 8];
                bool same = true, inv = true;
                for (int y = 0; y < 8; y++) {
                    if (g[y] != cell[y]) same = false;
                    if ((uint8_t)~g[y] != cell[y]) inv = false;
                }
                if (same || inv) { ch = c; break; }
            }
            if (ch == 0x7F) { line[n++] = '('; line[n++] = 'c'; ch = ')'; }    /* copyright sign */
            line[n++] = (char)ch;
        }
        line[n] = 0;
        while (n > 0 && line[n - 1] == ' ') line[--n] = 0;
        printf("|%s\n", line);
    }
}

/* ------------------------------------------------------------------------ */
/* How the board is built                                                    */
/* ------------------------------------------------------------------------ */

/* 16K Spectrums are the same boards with the upper RAM and its glue left
 * out (but for the 4B's IC24, whose spare gates time the lower RAM):
 * the ROM finds 16K and says nothing (the copyright message is the
 * same). The ULA of the Issues 1 and 2: see the top of the file; the
 * values of the resistors that depend on it are those of the Issue 2
 * KiCad project's README (5C112 is what the projects have; the 5C102
 * takes the same). */
static const char *const ram_values[] = { "48K", "16K" }, *const ram_ids[] = { "48k", "16k" };
static const char *const ula_values[] = { "5C112", "6C001" }, *const ula_ids[] = { "5c112", "6c001" };
static const char *const ula_values_i1[] = { "5C102", "5C112", "6C001" };
#define ULA_HELP "Which ULA is fitted: labels and the resistors that go with it only (the 5C ULAs and the " \
                 "6C001 differ in analog ways, not simulated)"
/* The upper RAM: the chips (a 4164 with one good half, by the row A7 for
 * TI, the column A7 for OKI; or a whole 4164) and the links that say which
 * (the 3B and 4B: TI or OKI, and the half; the Issue 2: LNK1, the row A7,
 * "TI only"). Chips and links that don't agree make half the upper RAM bad:
 * the ROM's RAM test stops there, and the Spectrum has less memory. */
static const char *const uchips_values[] = { "TI 4532-3", "TI 4532-4", "OKI 3732-L", "OKI 3732-H", "4164" },
                  *const uchips_ids[] = { "ti3", "ti4", "okil", "okih", "4164" };
static const part_type_t *const uchips_types[] = { &PT_4532, &PT_4532_4, &PT_3732L, &PT_3732H, &PT_4164_ZX };
static const char *const ulinks_values[] = { "TI-3", "TI-4", "OKI-L", "OKI-H" },     /* with TI or OKI */
                  *const ulinks_ids[] = { "ti3", "ti4", "okil", "okih" };
static const char *const ulinks_values_i2[] = { "TI-3 (low)", "TI-4 (high)" };
#define UCHIPS_HELP "Which chips the upper RAM (IC15-IC22) has: 4164s with one good half (TI 4532 by the " \
                    "row A7, OKI 3732 by the column A7), or whole 4164s. Picking them sets the links to match"
#define ULINKS_HELP "How the upper RAM's type links are set: TI with TI-3 or TI-4, or OKI with OKI-L or " \
                    "OKI-H. Links that don't match the chips leave half the upper RAM bad"
#define ULINKS_HELP_I2 "How LNK1 is set (the row A7 of the upper RAM: TI chips only): TI-3 low, TI-4 high. " \
                       "A setting that doesn't match the chips leaves half the upper RAM bad"
/* The ROM (IC5): NEC or Hitachi, and its link, N or H (see parts.c: a
 * NEC on the H link gives the same byte again and again; a Hitachi works
 * on either). */
static const char *const rom_values[] = { "NEC", "Hitachi" }, *const rom_ids[] = { "nec", "hitachi" };
static const char *const romlink_values[] = { "N", "H" }, *const romlink_ids[] = { "n", "h" };
#define ROM_HELP "Which ROM (IC5) is fitted: a NEC uPD23128 latches the address when its pin 20 falls, a " \
                 "Hitachi HN613128 is static. Picking it sets the link to match"
#define ROMLINK_HELP "How the ROM link is set: N (NEC) puts /MREQ on pin 20 and /ROMCS on pin 27, H (Hitachi) " \
                     "the other way round. A NEC on H gives the same byte again and again; a Hitachi works on " \
                     "either"
#define ROM_OPTIONS { "rom", "ROM", ROM_HELP, rom_values, rom_ids, 2 }, \
                    { "romlink", "ROM link", ROMLINK_HELP, romlink_values, romlink_ids, 2 }
static const machine_option_t zx_options[] = {
    { "ram", "RAM", "16K: the upper RAM (IC15-IC22) and its glue (IC23-IC26) not fitted", ram_values, ram_ids, 2 },
    { "uchips", "Chips", UCHIPS_HELP, uchips_values, uchips_ids, 5 },
    { "ulinks", "Links", ULINKS_HELP, ulinks_values, ulinks_ids, 4 },
    ROM_OPTIONS,
};
static const machine_option_t zx_options_i2[] = {
    { "ram", "RAM", "16K: the upper RAM (IC15-IC22) and its glue (IC23-IC26) not fitted", ram_values, ram_ids, 2 },
    { "ula", "ULA", ULA_HELP, ula_values, ula_ids, 2 },
    { "uchips", "Chips", UCHIPS_HELP, uchips_values, uchips_ids, 5 },
    { "ulinks", "Links", ULINKS_HELP_I2, ulinks_values_i2, ulinks_ids, 2 },
    ROM_OPTIONS,
};
static const machine_option_t zx_options_i6a[] = {
    { "ram", "RAM", "16K: the upper RAM (IC15-IC22) not fitted (the ZX8401 stays: it is the lower RAM's "
      "multiplexer too)", ram_values, ram_ids, 2 },
    { "uchips", "Chips", UCHIPS_HELP, uchips_values, uchips_ids, 5 },
    { "ulinks", "Links", ULINKS_HELP, ulinks_values, ulinks_ids, 4 },
    ROM_OPTIONS,
};
static const machine_option_t zx_options_i1[] = {
    { "ula", "ULA", ULA_HELP, ula_values_i1, zx_ula_ids, 3 },
    ROM_OPTIONS,
};
static const char *const upper_ram_parts[] = { "IC15", "IC16", "IC17", "IC18", "IC19", "IC20", "IC21", "IC22",
                                               "IC23", "IC24", "IC25", "IC26" };
static const struct { const char *ref, *value[3]; } ula_resistors[] = {     /* by zx_ulas */
    { "R47", { "220R", "220R", "1K" } }, { "R49", { "8K2", "8K2", "10K" } } };

static int zx_get_option(board_t *b, int option) {
    const char *id = b->m->options[option].id;
    zx48_t *z = ZX(b);
    return !strcmp(id, "ram") ? z->ram16k : !strcmp(id, "uchips") ? z->uram_chips :
           !strcmp(id, "ulinks") ? z->uram_links : !strcmp(id, "rom") ? z->rom_chip :
           !strcmp(id, "romlink") ? z->rom_link : z->ula_type;
}

/* The ROM's pin names: they differ at 20, 22 and 27. */
static void zx_rom_pin_names(board_t *b) {
    static const char *const names[2][3] = { { "/CE", "OD", "CS" }, { "CS", "OE0", "OE1" } };
    static const int numbers[3] = { 20, 22, 27 };
    sim_t *s = &b->sim;
    part_t *rom = &s->parts[ZX(b)->p_rom];
    for (int i = 0; i < 3; i++) {
        int pin = part_pin(rom, numbers[i]);
        free((char *)s->pin_names[pin]);
        s->pin_names[pin] = strdup(names[ZX(b)->rom_chip != 0][i]);
    }
}

/* The ROM as the options say: the chip IC5 is, and its link: N joins
 * pin 20 to /MREQ and pin 27 to /ROMCS, H the other way round. The legs
 * by their nets (the Issue 1's LNK2 has them in another order). */
static void zx_rom_fitted(board_t *b) {
    zx48_t *z = ZX(b);
    sim_t *s = &b->sim;
    part_t *rom = &s->parts[z->p_rom];
    const part_type_t *t = z->rom_chip ? &PT_HN613128 : &PT_UPD23128;
    rom->name = t->name;
    rom->eval = t->eval;
    rom->commit = t->commit;
    zx_rom_pin_names(b);
    part_t *p = find(b, V(b)->rom_link);
    if (!p || !p->state) return;
    const char *n20 = s->nets[s->pins[part_pin(rom, 20)].net].name;
    const char *n27 = s->nets[s->pins[part_pin(rom, 27)].net].name;
    int l20 = pin_on_net(b, p->ref, n20), l27 = pin_on_net(b, p->ref, n27);
    int mreq = pin_on_net(b, p->ref, "/MREQ"), romcs = pin_on_net(b, p->ref, "/ROMCS");
    if (l20 < 0 || l27 < 0 || mreq < 0 || romcs < 0) return;
    resistor_state_t *st = p->state;
    st->nel = 2;
    st->el[0] = (element_t){ l20, z->rom_link ? romcs : mreq, EL_LINK };
    st->el[1] = (element_t){ l27, z->rom_link ? mreq : romcs, EL_LINK };
}

/* The upper RAM as the options say: the chips IC15-IC22 are, and the type
 * links fitted (the others not fitted: they join nothing). */
static void zx_uram_fitted(board_t *b) {
    zx48_t *z = ZX(b);
    const part_type_t *t = uchips_types[z->uram_chips];
    for (int i = 0; i < 8; i++) {
        part_t *p = find(b, upper_ram_parts[i]);
        if (p) { p->commit = t->commit; p->name = t->name; }
    }
    if (V(b)->uram_links == ULINKS_TI_OKI) {
        /* By the "ulinks" values: TI and TI-3, TI and TI-4, OKI and OKI-L,
         * OKI and OKI-H. */
        static const char *const refs[] = { "TI1", "OKI1", "TI-3", "TI-4", "OKI-L1", "OKI-H1" };
        static const char *const fitted[4][2] = { { "TI1", "TI-3" }, { "TI1", "TI-4" },
                                                  { "OKI1", "OKI-L1" }, { "OKI1", "OKI-H1" } };
        for (int i = 0; i < 6; i++) {
            part_t *p = find(b, refs[i]);
            if (!p || !p->state) continue;
            bool on = !strcmp(refs[i], fitted[z->uram_links][0]) || !strcmp(refs[i], fitted[z->uram_links][1]);
            if (on) set_link(b, refs[i], "1-2");
            else ((resistor_state_t *)p->state)->nel = 0;
            p->unfitted = p->removed = !on;
        }
    } else if (V(b)->uram_links == ULINKS_LNK1) {
        set_link(b, "LNK1", z->uram_links ? "1-2" : "2-3");    /* the row A7 to +5V or GND */
    }
    zx_forget_cells(z);
}

static void zx_set_option(board_t *b, int option, int value) {
    zx48_t *z = ZX(b);
    const char *id = b->m->options[option].id;
    if (!strcmp(id, "uchips") || !strcmp(id, "ulinks")) {
        if (id[1] == 'c') {
            z->uram_chips = value;
            /* The links that go with them, if the board has those. */
            int o = zx_option(b->m, "ulinks");
            if (o >= 0 && value < b->m->options[o].nvalues) z->uram_links = value;
        } else {
            z->uram_links = value;
        }
        zx_uram_fitted(b);
    } else if (!strcmp(id, "rom") || !strcmp(id, "romlink")) {
        if (!strcmp(id, "rom")) z->rom_chip = z->rom_link = value;     /* with the link to match */
        else z->rom_link = value;
        zx_rom_fitted(b);
    } else if (!strcmp(id, "ram")) {
        z->ram16k = value != 0;
        for (size_t i = 0; i < sizeof(upper_ram_parts) / sizeof(upper_ram_parts[0]); i++) {
            part_t *p = find(b, upper_ram_parts[i]);
            if (p && !(V(b)->kept_16k && !strcmp(V(b)->kept_16k, p->ref))) p->unfitted = p->removed = z->ram16k;
        }
    } else {
        z->ula_type = value;
        for (size_t i = 0; i < sizeof(ula_resistors) / sizeof(ula_resistors[0]); i++) {
            part_t *p = find(b, ula_resistors[i].ref);
            if (p) p->name = ula_resistors[i].value[zx_ula(b)];
        }
        zx_ula_fitted(b);
    }
    board_config_changed(b);
    board_rebuild(b);
    board_power_on(b);
}

/* The machine, for every board but what differs. */
#define ZX_MACHINE \
    .name = "ZX Spectrum 48K", \
    .size = sizeof(zx48_t), \
    .sim = { .rail_of = zx_rail_of, .kind_of = zx_kind_of, .cycles_per_us = 3.5 }, \
    .cpu_hz = ZX_FREQ, \
    .cycles_per_frame = ZX_CYCLES_PER_FRAME, \
    .steps_per_cycle = ZX_STEPS_PER_CYCLE, \
    .screen_w = ULA_SCREEN_W, .screen_h = ULA_SCREEN_H, \
    .ready_cycles = 3 * ZX_FREQ, \
    .parts = zx_part_defs, .nparts = sizeof(zx_part_defs) / sizeof(zx_part_defs[0]), \
    .link_resistors = "", \
    .diodes = "D", \
    .supplies = "-5V +12V -12VAC +9V +12VA", \
    .keys_help = "Keys: ESC=BREAK  TAB=SYMBOL SHIFT  left SHIFT=CAPS SHIFT", \
    .reset_help = "Hold to press RESET: shorts /RESET to GND on the edge connector", \
    .create = zx_create, \
    .destroy = zx_destroy, \
    .power_on = zx_power_on, \
    .run = zx_run, \
    .joins = zx_joins, \
    .key_down = zx_key_down, \
    .key_up = zx_key_up, \
    .frame_update = zx_frame_update, \
    .host_key = zx_host_key, \
    .host_char = zx_host_char, \
    .monitor = zx_monitor, \
    .files_title = "Open a snapshot (.sna, .z80) or a ROM (.rom, .bin)", \
    .cart_name = zx_cart_name, \
    .remove_cart = zx_remove_cart, \
    .cart_banks = zx_cart_banks, \
    .cart_bank = zx_cart_bank, \
    .cart_bank_info = zx_cart_bank_info, \
    .set_cart_bank = zx_set_cart_bank, \
    .file_kind = zx_file_kind, \
    .load = zx_load, \
    .cpu_status = zx_cpu_status, \
    .cpu_regs = zx_cpu_regs, \
    .print_text = zx_print_text, \
    .state_loaded = zx_state_loaded, \
    .ram_cell = zx_ram_cell, \
    .get_option = zx_get_option, \
    .set_option = zx_set_option, \
    .part_def = zx_part_def

const machine_t machine_zx48 = {
    ZX_MACHINE,
    .id = "zx48",
    .board_name = "ZX Spectrum 48K Issue 3B",
    .issue = "Issue 3B",
    .variant = &zx_3b,
    .data = &bd_zx48,
    .conns = zx_conn_defs, .nconns = sizeof(zx_conn_defs) / sizeof(zx_conn_defs[0]),
    .crystals = "X1 REG1 TR4 TR5 T1 D11 D12 D15 D16 R55 R58 R59 R60 R61",
    .cart_slot = "E1",
    .options = zx_options, .noptions = 5,
};

const machine_t machine_zx48i2 = {
    ZX_MACHINE,
    .id = "zx48i2",
    .board_name = "ZX Spectrum 48K Issue 2",
    .issue = "Issue 2",
    .variant = &zx_i2,
    .data = &bd_zx48i2,
    .conns = zx_conn_defs_i2, .nconns = sizeof(zx_conn_defs_i2) / sizeof(zx_conn_defs_i2[0]),
    .crystals = "X1 U2 TR4 TR5 T1 D11 D12 D15 D16 R55 R58 R59 R60 R61",
    .cart_slot = "J3",
    .options = zx_options_i2, .noptions = 6,
};

const machine_t machine_zx48i4b = {
    ZX_MACHINE,
    .id = "zx48i4b",
    .board_name = "ZX Spectrum 48K Issue 4B",
    .issue = "Issue 4B",
    .variant = &zx_i4b,
    .data = &bd_zx48i4b,
    .conns = zx_conn_defs_i4b, .nconns = sizeof(zx_conn_defs_i4b) / sizeof(zx_conn_defs_i4b[0]),
    .crystals = "X1 REG1 L1 L2 TR4 TR5 T1 D11 D15 D17 D18 D19 C80 R79 R58 R59 R60 R61",
    .cart_slot = "E1",
    .options = zx_options, .noptions = 5,
};

const machine_t machine_zx48i6a = {
    ZX_MACHINE,
    .id = "zx48i6a",
    .board_name = "ZX Spectrum 48K Issue 6A",
    .issue = "Issue 6A",
    .variant = &zx_i6a,
    .data = &bd_zx48i6a,
    .conns = zx_conn_defs_i4b, .nconns = sizeof(zx_conn_defs_i4b) / sizeof(zx_conn_defs_i4b[0]),
    .crystals = "X1 REG1 L1 L2 TR4 TR5 T1 D11 D15 D17 D18 D19 C80 R79 R58 R59 R60 R61",
    .cart_slot = "E1",
    .options = zx_options_i6a, .noptions = 5,
};

const machine_t machine_zx48i1 = {
    ZX_MACHINE,
    .id = "zx48i1",
    .board_name = "ZX Spectrum 16K Issue 1",
    .issue = "Issue 1",
    .variant = &zx_i1,
    .data = &bd_zx48i1,
    .conns = zx_conn_defs_i1, .nconns = sizeof(zx_conn_defs_i1) / sizeof(zx_conn_defs_i1[0]),
    .crystals = "X1 U2 TR4 TR5 T1 D11 D12 D15 D16 R55 R58 R59 R60 R61",
    .cart_slot = "J3",
    .options = zx_options_i1, .noptions = 3,
};
