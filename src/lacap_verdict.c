/* lacap_verdict.c - the diagnosis: what to look at, in words.
 *
 * AI-generated (Claude Code). */
#include "lacap_int.h"

const char *ref_of(const char *label) {
    for (int i = 0; vboard && i < vboard->sim.nparts; i++) {
        const char *l = board_part_label(vboard, i);
        if (l && !strcmp(l, label)) return vboard->sim.parts[i].ref;
    }
    return NULL;
}

/* The parts that serve one bank of RAM as a whole (their description
 * says "lower RAM" or "upper RAM"), but its chips. */
static void bank_parts(const char *bank, const char *chips, char *out, int len) {
    int n = 0;
    out[0] = 0;
    for (int i = 0; vboard && i < vboard->sim.nparts && n < len; i++) {
        const char *d = board_part_detail(vboard, i), *l = board_part_label(vboard, i);
        if (!d || !strstr(d, bank) || (l && !strncmp(l, chips, 4))) continue;
        n += snprintf(out + n, len - n, "%s%s (%s)", n ? ", " : "", vboard->sim.parts[i].ref, l ? l : "?");
    }
}

/* The resistors between the CPU's data lines and the chips labelled
 * CHIPS* (the lower RAM's: the ULA's side of the bus). */
static void data_resistors(const char *chips, char *out, int len) {
    sim_t *s = &vboard->sim;
    int n = 0;
    out[0] = 0;
    for (int bit = 0; bit < 8; bit++) {
        char nm[4];
        snprintf(nm, sizeof(nm), "D%d", bit);
        int dn = sim_find_net(s, nm);
        if (dn < 0) continue;
        int droot = s->nets[dn].root;
        for (int i = 0; i < s->nparts && n < len; i++) {
            const part_t *r = &s->parts[i];
            if (r->npins != 2 || strcmp(board_part_kind_name(vboard, i), "resistor")) continue;
            int r0 = s->pins[r->first_pin].root, r1 = s->pins[r->first_pin + 1].root;
            int other = r0 == droot ? r1 : r1 == droot ? r0 : -1;
            if (other < 0 || board_net_supply(vboard, other) != RAIL_NONE) continue;    /* a pull-up */
            bool to_chip = false;       /* the other side reaches a CHIPS chip */
            for (int j = 0; j < s->nparts && !to_chip; j++) {
                const char *l = board_part_label(vboard, j);
                if (!l || strncmp(l, chips, strlen(chips))) continue;
                for (int k = 0; k < s->parts[j].npins && !to_chip; k++) to_chip = s->pins[s->parts[j].first_pin + k].root == other;
            }
            if (to_chip) n += snprintf(out + n, len - n, "%s%s", n ? " " : "", r->ref);
        }
    }
}

static int z80_pin(int s) {
    static const int a[16] = { 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 1, 2, 3, 4, 5 };
    static const int d[8] = { 14, 15, 12, 8, 7, 9, 10, 13 };
    if (s >= LA_A0 && s <= LA_A15) return a[s - LA_A0];
    if (s >= LA_D0 && s <= LA_D7) return d[s - LA_D0];
    switch (s) {
    case LA_MREQ: return 19; case LA_RD: return 21; case LA_WR: return 22; case LA_M1: return 27; case LA_RFSH: return 28;
    case LA_CLK: return 6; case LA_INT: return 16; case LA_NMI: return 17; case LA_HALT: return 18;
    case LA_WAIT: return 24; case LA_BUSRQ: return 25; case LA_RESET: return 26;
    default: return 0;
    }
}

/* The chips on the signal's line, the CPU apart (and those through a
 * resistor from it, not a pull-up): "IC26 pin 2, IC5 pin 9". How many. */
static int line_chips(int sig, char *out, int len) {
    out[0] = 0;
    if (!vboard) return 0;
    int net[LA_NSIG];
    find_nets(vboard, net, NULL);
    if (net[sig] < 0) return 0;
    sim_t *s = &vboard->sim;
    int roots[8], nr = 0;
    roots[nr++] = s->nets[net[sig]].root;
    bool bus = (sig >= LA_A0 && sig <= LA_A15) || (sig >= LA_D0 && sig <= LA_D7);   /* to the RAM through resistors */
    for (int i = 0; bus && i < s->nparts && nr < 8; i++) {
        const part_t *r = &s->parts[i];
        if (r->npins != 2 || strcmp(board_part_kind_name(vboard, i), "resistor")) continue;
        int r0 = s->pins[r->first_pin].root, r1 = s->pins[r->first_pin + 1].root;
        if (r0 == roots[0] && board_net_supply(vboard, r1) == RAIL_NONE) roots[nr++] = r1;
        else if (r1 == roots[0] && board_net_supply(vboard, r0) == RAIL_NONE) roots[nr++] = r0;
    }
    const char *cpu = ref_of("CPU");
    int k = 0, m = 0;
    for (int i = 0; i < s->nparts; i++) {
        const char *kind = board_part_kind_name(vboard, i);
        if (!strcmp(kind, "resistor") || !strcmp(kind, "capacitor") || !strcmp(kind, "connector")) continue;
        if (cpu && !strcmp(s->parts[i].ref, cpu)) continue;
        for (int j = 0; j < s->parts[i].npins; j++) {
            bool on = false;
            for (int q = 0; q < nr && !on; q++) on = s->pins[s->parts[i].first_pin + j].root == roots[q];
            if (!on) continue;
            if (m < len) m += snprintf(out + m, len - m, "%s%s pin %d", !k ? "" : k % 8 ? ", " : ",\n     ", s->parts[i].ref,
                                       s->pins[s->parts[i].first_pin + j].number);
            k++;
            break;
        }
    }
    return k;
}

/* The pin of part REF on the signal's line (0: none). */
static int pin_on(const char *ref, int sig) {
    if (!vboard || !ref) return 0;
    int net[LA_NSIG];
    find_nets(vboard, net, NULL);
    if (net[sig] < 0) return 0;
    sim_t *s = &vboard->sim;
    int root = s->nets[net[sig]].root;
    for (int i = 0; i < s->nparts; i++) {
        if (strcmp(s->parts[i].ref, ref)) continue;
        for (int j = 0; j < s->parts[i].npins; j++)
            if (s->pins[s->parts[i].first_pin + j].root == root) return s->pins[s->parts[i].first_pin + j].number;
    }
    return 0;
}

/* The same, or through a resistor in series (not a pull-up): the pin, and
 * the resistor in VIA ("" if direct). The Issue 1 and 2's ULA drives /INT
 * through R26. */
static int pin_on_via(const char *ref, int sig, char *via, int vlen) {
    via[0] = 0;
    int pin = pin_on(ref, sig);
    if (pin || !vboard || !ref) return pin;
    int net[LA_NSIG];
    find_nets(vboard, net, NULL);
    if (net[sig] < 0) return 0;
    sim_t *s = &vboard->sim;
    int root = s->nets[net[sig]].root;
    for (int i = 0; i < s->nparts; i++) {
        const part_t *r = &s->parts[i];
        if (r->npins != 2 || strcmp(board_part_kind_name(vboard, i), "resistor")) continue;
        int r0 = s->pins[r->first_pin].root, r1 = s->pins[r->first_pin + 1].root;
        int other = r0 == root ? r1 : r1 == root ? r0 : -1;
        if (other < 0 || board_net_supply(vboard, other) != RAIL_NONE) continue;
        for (int k = 0; k < s->nparts; k++) {
            if (strcmp(s->parts[k].ref, ref)) continue;
            for (int j = 0; j < s->parts[k].npins; j++)
                if (s->pins[s->parts[k].first_pin + j].root == other) {
                    snprintf(via, vlen, "%s", r->ref);
                    return s->pins[s->parts[k].first_pin + j].number;
                }
        }
    }
    return 0;
}

static const char *bits_way(const ev_t *e, int ar, int bit) {
    if (!e->high[ar][bit]) return "reads 0 where it should be 1";
    if (!e->low[ar][bit]) return "reads 1 where it should be 0";
    return "reads wrong both ways";
}

static void bits_text(unsigned m, char *out, int len) {
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < 8 && n < len; i++) if (m >> i & 1) n += snprintf(out + n, len - n, "%sD%d", n ? " " : "", i);
}

static int popcount8(unsigned m) { int c = 0; for (; m; m &= m - 1) c++; return c; }

/* The bank's multiplexers that carry CPU address line A<k> (on its net, or
 * through a resistor from it). */
static void mux_for_addr(const char *bank, const char *chips, int k, char *out, int len) {
    sim_t *s = &vboard->sim;
    char nm[16];
    snprintf(nm, sizeof(nm), "A%d", k & 15);
    int an = sim_find_net(s, nm);
    int n = 0;
    out[0] = 0;
    if (an < 0) return;
    int roots[8], nr = 0;
    roots[nr++] = s->nets[an].root;
    for (int i = 0; i < s->nparts && nr < 8; i++) {        /* through a resistor */
        const part_t *r = &s->parts[i];
        if (r->npins != 2 || strcmp(board_part_kind_name(vboard, i), "resistor")) continue;
        int r0 = s->pins[r->first_pin].root, r1 = s->pins[r->first_pin + 1].root;
        if (r0 == roots[0] && board_net_supply(vboard, r1) == RAIL_NONE) roots[nr++] = r1;
        else if (r1 == roots[0] && board_net_supply(vboard, r0) == RAIL_NONE) roots[nr++] = r0;
    }
    for (int i = 0; i < s->nparts && n < len; i++) {
        const char *d = board_part_detail(vboard, i), *l = board_part_label(vboard, i);
        if (!d || !strstr(d, bank) || !l || strcmp(l, "MUX")) continue;
        bool on = false;
        for (int j = 0; j < s->parts[i].npins && !on; j++)
            for (int q = 0; q < nr && !on; q++) on = s->pins[s->parts[i].first_pin + j].root == roots[q];
        if (on) n += snprintf(out + n, len - n, "%s%s", n ? " or " : "", s->parts[i].ref);
    }
    (void)chips;
}

/* What it points at, most likely first. */
int verdict(const ev_t *e, const machine_t *m, const char *stage, bool missing, const cmp_out_t *co, const char *where) {
    vboard = la_new_board(m);
    const char *cpu = ref_of("CPU"), *rom = ref_of("ROM"), *ula = ref_of("ULA");
    if (!cpu) cpu = "the CPU";
    if (!rom) rom = "the ROM";
    int n = 0;
    char buf[400], bits[64], res[200];
    bool idle_st = !strcmp(stage, "idle");
    if (e->in_reset && !idle_st)
        printf("\nNote: /RESET was low all through that capture: it was taken while the reset button was held (or /RESET\n"
               "is stuck low), when the CPU drives nothing; what it shows isn't the boot.\n");
    printf("\nWhat to look at, most likely first:\n");
    /* Most of the lines dead: the CPU isn't running at all. */
    int nstuck = 0, nctl = 0;
    for (int s = 0; s < LA_NSIG; s++)
        if (e->stuck[s]) { nstuck++; nctl += s == LA_MREQ || s == LA_RD || s == LA_WR || s == LA_M1 || s == LA_RFSH; }
    /* The analyser's two boards the wrong way round: each line is read from
     * another, and a working Spectrum looks dead. CLK stopped with /WAIT and
     * /BUSRQ both held low (each has a pull-up) is that, not a fault. */
    if (idle_st && e->stuck[LA_CLK] && e->held[LA_WAIT] && e->held[LA_BUSRQ]) {
        printf("  %d. The analyser's two boards look the wrong way round, not a fault on the Spectrum: CLK stopped with\n"
               "     /WAIT and /BUSRQ both low all through (each has a pull-up) is how a working one reads then. The\n"
               "     master is the board nearest the Spectrum (channels 1-24): press Swap in the window (or give\n"
               "     --la-ports MASTER,OTHER the other way round) and run it again.\n", ++n);
        goto done;
    }
    if (e->rst_pulses) {
        printf("  %d. /RESET goes low %d time%s in %d ms, left running with nobody at the button: the CPU keeps\n"
               "     restarting (a working one's stays high). Something pulls /RESET low now and then: the CPU (%s pin 26:\n"
               "     a faulty Z80 can short its input), the reset circuit (its capacitor and resistor), the button, or the\n"
               "     5 V supply dipping. Pull the CPU and watch pin 26's socket hole: it should stay at 5 V.\n", ++n,
               e->rst_pulses, e->rst_pulses == 1 ? "" : "s", e->slow_ms, cpu);
        goto done;
    }
    if (e->in_reset && idle_st && !e->stuck[LA_CLK]) {
        printf("  %d. /RESET is low all through, left running with nobody at the button, so the CPU never starts.\n"
               "     Something holds /RESET low: the CPU (%s pin 26: a faulty Z80 can short its input), the reset\n"
               "     circuit (its capacitor and resistor) or the button. Pull the CPU and measure pin 26's socket hole:\n"
               "     it should rise to 5 V.\n", ++n, cpu);
        goto done;
    }
    if (e->in_reset && !e->stuck[LA_CLK]) {
        printf("  %d. /RESET is low all through, so the CPU never starts. If the reset button wasn't held then (it asks\n"
               "     once more when it sees this), something holds /RESET low: the CPU (%s pin 26: a faulty Z80 can\n"
               "     short its input), the reset circuit (its capacitor and resistor) or the button. Pull the CPU and\n"
               "     measure pin 26's socket hole: it should rise to 5 V.\n", ++n, cpu);
        goto done;
    }
    /* A CPU whose outputs are dead or floating: every address line low,
     * /BUSACK low though nothing asks for the bus (/BUSRQ high), which a
     * working Z80 never does. A real Issue 1 read so, its address lines at
     * about 1.2 V on a scope, its clock there at the CPU: the CPU first. */
    {
        bool addr_low = true;
        for (int k = LA_A0; k <= LA_A15; k++) addr_low = addr_low && e->lvl[k] == 1;
        if (idle_st && addr_low && e->lvl[LA_BUSACK] == 1 && e->lvl[LA_BUSRQ] == 2) {
            printf("  %d. The CPU (%s): its outputs look dead or floating. Every address line A0-A15 reads low and\n"
                   "     /BUSACK is low though nothing asks for the bus (/BUSRQ is high): a working Z80 never does that,\n"
                   "     and floating lines (about 1.2 V on a scope) read as low. Check its +5 V (pin 11) and 0 V (pin 29)\n"
                   "     first, then change the CPU.\n", ++n, cpu);
            if (e->stuck[LA_CLK])
                printf("  %d. CLK reads stuck %s at the edge connector. That is the ULA's side of it (%s pin 32), not the\n"
                       "     CPU's pin 6: measure pin 6. A real Issue 1 that read like this had its clock at the CPU. If it\n"
                       "     is missing there too: the ULA, the crystal, or what feeds the CPU's clock from it.\n", ++n,
                       e->stuck[LA_CLK] == 1 ? "low" : "high", ula ? ula : "the ULA's");
            goto done;
        }
    }
    if (e->stuck[LA_CLK])
        printf("  %d. No clock: CLK (%s's pin 6) is stuck %s. It comes from %s (the ULA, from the 14 MHz crystal): the ULA, the\n"
               "     crystal, or a short on the line. Nothing runs without it.\n", ++n, cpu, e->stuck[LA_CLK] == 1 ? "low" : "high",
               ula ? ula : "ULA");
    if (!e->stuck[LA_CLK] && e->clk_pct >= 0 && e->clk_pct < 30)
        printf("  %d. The CPU's clock is wrong: the analyser sees only %d%% of CLK's pulses (a working ULA's: about 90%%),\n"
               "     so they are too narrow or too weak, from %s (the ULA). A CPU on a clock like that misreads and\n"
               "     miswrites: what follows may all come from it.\n", ++n, e->clk_pct, ula ? ula : "the ULA");
    if (e->clk_stopped)
        printf("  %d. Later the clock stopped: CLK stuck %s, so the CPU stopped. It comes from %s (the ULA): a ULA that\n"
               "     fails as it warms up does this.\n", ++n, e->clk_stopped == 1 ? "low" : "high", ula ? ula : "the ULA");
    if (!e->stuck[LA_CLK] && ((e->clk_pct >= 0 && e->clk_pct < 30) || e->clk_stopped)) {
        unsigned all = ev_mask(e, AR_ROM) | ev_mask(e, AR_LRAM) | ev_mask(e, AR_URAM);
        if (all) {
            bits_text(all, bits, sizeof(bits));
            printf("     What the bus shows on that clock (look at it again once the clock is right): bits %s read wrong\n"
                   "     (ROM %d, lower RAM %d, upper RAM %d reads)", bits, e->nbad[AR_ROM], e->nbad[AR_LRAM], e->nbad[AR_URAM]);
            if (ev_pramt_bad(e)) printf("; the RAM test stopped at %04X", e->pramt + 1);
            printf(".\n");
        }
        goto done;
    }
    if (nstuck >= 8 && e->stuck[LA_CLK]) {
        printf("     So the CPU isn't running (%d of the lines a running Z80 changes never change): that follows from the clock.\n",
               nstuck - 1);
        goto done;
    }
    if (e->held[LA_WAIT] || e->held[LA_BUSRQ]) {
        int sg = e->held[LA_WAIT] ? LA_WAIT : LA_BUSRQ;
        printf("  %d. %s (%s pin %d) is low all through: the CPU %s. It's the CPU's input, from the\n"
               "     edge connector (anything plugged in?) and a pull-up: a short to 0 V on the line, or the CPU itself.\n",
               ++n, la_signal_name(sg), cpu, z80_pin(sg),
               sg == LA_WAIT ? "waits for ever in its bus cycle" : "hands the bus over and stops");
        goto done;
    }
    bool inputs_ok = e->idle && !e->in_reset && !e->rst_pulses && !e->stuck[LA_CLK] && e->clk_pct >= 30 &&
                     !e->held[LA_WAIT] && !e->held[LA_BUSRQ];
    if (nstuck >= 8 && inputs_ok) {
        bool together = e->stuck[LA_RD] == 1 && e->stuck[LA_WR] == 1;
        printf("  %d. The CPU isn't running: %d of the lines a running Z80 changes all the time never change. What it\n"
               "     needs is right, left running: its clock, /RESET high, /WAIT and /BUSRQ high. So it's the CPU (%s)\n"
               "     itself, or its power: measure 5 V on pin 11 and 0 V on pin 29 at the chip's legs.\n", ++n, nstuck, cpu);
        if (together)
            printf("     /RD and /WR are both low all through, which a working Z80 never does: it drives nothing right,\n"
                   "     as a dead or unpowered one.\n");
        goto done;
    }
    if (nstuck >= 8) {
        printf("  %d. The CPU isn't running: %d of the lines a running Z80 changes all the time never change. Check what it\n"
               "     needs: its clock (pin 6), /RESET (pin 26) high, /WAIT (24) and /BUSRQ (25) high, 5 V on pin 11, 0 V on\n"
               "     pin 29; then the CPU (%s) itself.\n", ++n, nstuck, cpu);
        goto done;
    }
    /* Two of the CPU's own outputs stuck, the rest of the bus going: one
     * fault on two lines is the CPU (not /M1: the 48K runs without it). */
    {
        static const int outs[] = { LA_MREQ, LA_RD, LA_WR };
        char names[64];
        int k = 0, m = 0;
        for (int q = 0; q < 3; q++)
            if (e->stuck[outs[q]]) { m += snprintf(names + m, sizeof(names) - m, "%s%s", k ? " and " : "", la_signal_name(outs[q])); k++; }
        if (k >= 2 && nstuck < 8)
            printf("  %d. Two of the CPU's own outputs, %s, are stuck while the rest of the bus goes on: one fault on\n"
                   "     two lines is most likely the CPU (%s). Try another one first.\n", ++n, names, cpu);
    }
    /* A line that doesn't change: stuck, whatever drives it. */
    for (int s = 0; s < LA_NSIG; s++) {
        if (!e->stuck[s] || s == LA_CLK || s == LA_M1 || s == LA_RFSH) continue;
        char how[64], on[400];
        if (e->nearly[s]) snprintf(how, sizeof(how), "but for %d moment%s", (e->nearly[s] + 1) / 2, e->nearly[s] > 2 ? "s" : "");
        else snprintf(how, sizeof(how), "all through");
        int k = line_chips(s, on, sizeof(on));
        printf("  %d. %s is stuck %s %s, where a working board's changes all the time: a short to %s, or the line\n"
               "     broken. It's %s's pin %d.", ++n, la_signal_name(s), e->stuck[s] == 1 ? "low" : "high", how,
               e->stuck[s] == 1 ? "0 V (or another line)" : "5 V (or another line)", cpu, z80_pin(s));
        if (!k)
            printf(" No other chip is on it (the edge connector apart): the CPU itself, unless the line is\n"
                   "     shorted to its neighbour.\n");
        else
            printf("\n     Also on it: %s.\n     Any of them can hold it (a failed input), and its tracks: pull the socketed ones first,\n"
                   "     then the CPU.\n", on);
    }
    for (int s = LA_M1; s <= LA_RFSH; s++) {
        if (!e->stuck[s]) continue;
        char on[200];
        if (line_chips(s, on, sizeof(on))) continue;        /* a board that uses it: not here */
        printf("  Note: %s (%s pin %d) is stuck %s%s. Nothing on this board uses it (only the edge connector has\n"
               "  it), so a Z80 with that output dead still runs it: not the fault, unless something plugged in needs it.\n",
               la_signal_name(s), cpu, s == LA_M1 ? 27 : 28, e->stuck[s] == 1 ? "low" : "high",
               e->nearly[s] ? " (but for a moment or two)" : "");
    }
    /* Left running: the frame interrupt (the ULA's, every 20 ms) and HALT. */
    {
        char via[16], vtxt[64];
        int ip = pin_on_via(ula, LA_INT, via, sizeof(via));
        char up[64];
        if (ip && via[0]) snprintf(up, sizeof(up), "%s pin %d, through %s", ula ? ula : "the ULA", ip, via);
        else if (ip) snprintf(up, sizeof(up), "%s pin %d", ula ? ula : "the ULA", ip);
        else snprintf(up, sizeof(up), "%s", ula ? ula : "the ULA");
        if (e->held[LA_INT])
            printf("  %d. /INT (%s pin 16) is low all through, left running: a working one's goes low for 32 T every 20 ms.\n"
                   "     It comes from the ULA (%s): the ULA, %sor a short to 0 V on the line.\n", ++n, cpu, up,
                   via[0] ? (snprintf(vtxt, sizeof(vtxt), "%s open (the line left floating), ", via), vtxt) : "");
        else if (int_wrong(e) && !e->int_n)
            printf("  %d. No frame interrupt: /INT (%s pin 16) never goes low in %d ms, left running; a working one's does\n"
                   "     %d times (every 20 ms). It comes from the ULA (%s): the ULA, %s Without it\n"
                   "     the keyboard isn't read.\n", ++n, cpu, e->slow_ms, e->int_ref, up,
                   via[0] ? (snprintf(vtxt, sizeof(vtxt), "%s open, or the line broken.", via), vtxt) : "or the line broken.");
        else if (int_wrong(e))
            printf("  %d. The frame interrupt: /INT goes low %d times in %d ms, left running, a working one's %d (every 20 ms).\n"
                   "     It comes from the ULA (%s): its timing, or something else pulling the line.\n", ++n,
                   e->int_n, e->slow_ms, e->int_ref, up);
    }
    /* The keyboard: a line low on every half row is no key (nobody holds
     * a whole column down): the line or the ULA's input. Else the keys. */
    static const char *const keys[8][5] = {
        { "CAPS SHIFT", "Z", "X", "C", "V" }, { "A", "S", "D", "F", "G" }, { "Q", "W", "E", "R", "T" },
        { "1", "2", "3", "4", "5" }, { "0", "9", "8", "7", "6" }, { "P", "O", "I", "U", "Y" },
        { "ENTER", "L", "K", "J", "H" }, { "SPACE", "SYMBOL SHIFT", "M", "N", "B" } };
    static const int ula_kb_pin[5] = { 19, 20, 23, 24, 26 };
    for (int k = 0; k < 5; k++) {
        if (!e->kb_low[k]) continue;
        unsigned rows = e->kb_rows_low[k];
        /* Three rows or more (the ROM's KEY-SCAN gives up after three keys). */
        if (popcount8(rows) >= 3) {
            char col[96];
            int m = 0;
            for (int r = 0; r < 8; r++) m += snprintf(col + m, sizeof(col) - m, "%s%s", r ? " " : "", keys[r][k]);
            printf("  %d. Keyboard line KB%d reads low on every half row (%d reads): as if %s were all held\n"
                   "     down, which no hand does. The line is shorted to 0 V (the keyboard connector, the membrane's\n"
                   "     track, a bridge to its neighbour) or the ULA's KB%d input (%s pin %d) is.\n",
                   ++n, k, e->kb_low[k], col, k, ula ? ula : "the ULA", ula_kb_pin[k]);
        } else {
            for (int r = 0; r < 8; r++)
                if (rows >> r & 1)
                    printf("  %d. The key %s reads as held down (half row A%d, line KB%d): a key stuck or the membrane\n"
                           "     shorted there%s\n", ++n, keys[r][k], 8 + r, k, "");
        }
    }
    unsigned mr = ev_mask(e, AR_ROM), ml = ev_mask(e, AR_LRAM), mu = ev_mask(e, AR_URAM);
    unsigned shared = (mr & ml) | (mr & mu) | (ml & mu);
    /* The ROM's wrong reads are its own bytes from the address with one
     * bit the other way: that address line, not the data. */
    int abit = -1;
    for (int k = 0; k < 14; k++)
        if (e->rom_abit[k] >= 8 && 2 * e->rom_abit[k] >= e->nbad[AR_ROM] && (abit < 0 || e->rom_abit[k] > e->rom_abit[abit])) abit = k;
    if (abit >= 0) {
        printf("  %d. Address line A%d: %d of the %d wrong ROM reads are the ROM's own byte from the address with A%d the\n"
               "     other way. So the ROM sees A%d otherwise than the edge connector does: the line is weak or floating\n"
               "     (in between 0 and 5 V), not stuck. Its driver is the CPU (%s pin %d): a faulty CPU's output does this;\n"
               "     else a break in the track to the ROM (%s) or a chip loading the line.\n", ++n, abit, e->rom_abit[abit],
               e->nbad[AR_ROM], abit, abit, cpu, z80_pin(LA_A0 + abit), rom);
        printf("     The wrong data that follows (bits %s from the ROM and the RAM) all comes from it.\n",
               (bits_text(mr | ml | mu, bits, sizeof(bits)), bits));
        goto done;
    }
    /* The RAM test's first bad byte reads 00 and lies 2^k above the bank's
     * start: the same cell as the start (tested and left at 00), so the
     * bank's address line A<k> doesn't get through. */
    int alias_ar = -1, alias_k = -1;
    if (e->rt_addr >= 0x4000 && e->rt_got == 0x00) {
        unsigned base = e->rt_addr < 0x8000 ? 0x4000 : 0x8000, off = (unsigned)e->rt_addr - base;
        if (off && !(off & (off - 1)))
            for (int k = 0; k < 15; k++) if (off == 1u << k) { alias_k = k; alias_ar = base == 0x4000 ? AR_LRAM : AR_URAM; }
    }
    if (alias_k >= 0) {
        const char *bank = alias_ar == AR_LRAM ? "lower RAM" : "upper RAM", *chips = alias_ar == AR_LRAM ? "LRAM" : "URAM";
        unsigned base = alias_ar == AR_LRAM ? 0x4000 : 0x8000;
        char mx[64];
        mux_for_addr(bank, chips, alias_k, mx, sizeof(mx));
        printf("  %d. The %s's address line A%d: the RAM test found %04X reading 00, which is what it had just left\n"
               "     at %04X: those two are the same cell, so A%d doesn't get through to the %s's chips. Its\n"
               "     multiplexer%s%s%s, or the line from it to all the chips (a break, a short to the line next to it).\n",
               ++n, bank, alias_k, e->rt_addr, base, alias_k, bank, mx[0] ? " (" : "", mx, mx[0] ? ")" : "");
        printf("     The wrong bytes later on (the system variables there) come from it.\n");
        if (alias_ar == AR_LRAM) ml = 0; else mu = 0;
        shared &= ~0u;
        shared = (mr & ml) | (mr & mu) | (ml & mu);
    }
    unsigned sh = shared;
    for (int i = 0; i < 8; i++) if (e->stuck[LA_D0 + i]) sh &= ~(1u << i);
    if (popcount8(sh) >= 6 && (mr & sh) && ((ml | mu) & sh)) {
        bits_text(sh, bits, sizeof(bits));
        printf("  %d. Data lines %s are all wrong, from the ROM and the RAM alike: not a memory chip and not one\n"
               "     line. What they share is the CPU (%s) reading them (its data inputs, its timing), or the data bus\n"
               "     as a whole: a chip driving it when it shouldn't (the ULA, %s; the ROM, %s), a short across it.\n"
               "     Swap the CPU first.\n", ++n, bits, cpu, ula ? ula : "the ULA", rom);
        mr &= ~sh; ml &= ~sh; mu &= ~sh;
        shared = sh = 0;
    }
    for (int i = 0; i < 8; i++) {
        if (!(shared >> i & 1) || e->stuck[LA_D0 + i]) continue;
        printf("  %d. Data line D%d is wrong from %s%s%s: not one chip, the line itself (%s's pin %d) or a chip that\n"
               "     holds it (a short, a chip driving it when it shouldn't)\n", ++n, i,
               mr >> i & 1 ? "the ROM" : "", mr >> i & 1 && (ml | mu) >> i & 1 ? " and " : "", (ml | mu) >> i & 1 ? "the RAM" : "",
               cpu, z80_pin(LA_D0 + i));
    }
    unsigned stuckd = 0;
    for (int i = 0; i < 8; i++) if (e->stuck[LA_D0 + i]) stuckd |= 1u << i;
    mr &= ~(shared | stuckd); ml &= ~(shared | stuckd); mu &= ~(shared | stuckd);
    /* A stuck control or address line: the wrong bytes follow from it. */
    bool addr_stuck = false;
    for (int i = 0; i < 16; i++) addr_stuck |= e->stuck[LA_A0 + i] != 0;
    if (nctl || addr_stuck) { mr = ml = mu = 0; shared = 0; }
    if (mr) {
        bits_text(mr, bits, sizeof(bits));
        if (popcount8(mr) >= 3)
            printf("  %d. %s, the ROM: %d reads with bits %s wrong (the first at %04X): the chip, or its address lines or\n"
                   "     select (/ROMCS)\n", ++n, rom, e->nbad[AR_ROM], bits, e->first[AR_ROM]);
        else
            for (int i = 0; i < 8; i++)
                if (mr >> i & 1)
                    printf("  %d. %s, the ROM: its D%d %s (%d read%s, the first at %04X)\n", ++n, rom, i, bits_way(e, AR_ROM, i),
                           e->bad[AR_ROM][i], e->bad[AR_ROM][i] == 1 ? "" : "s", e->first[AR_ROM]);
    }
    /* The lower RAM wrong only where the ULA was fetching the screen: its
     * timing (contention), whichever chips show it. */
    bool timing = e->c_have && e->c_bad >= 2 && e->c_bad_win == e->c_bad && e->c_good > e->c_good_win && ml;
    if (timing) {
        bits_text(ml, bits, sizeof(bits));
        printf("  %d. %s, the ULA: its timing with the CPU (contention). All %d wrong bytes of the lower RAM were written\n"
               "     or read while the ULA was fetching the screen; of the %d that read back right (with those bits), %d\n"
               "     %s outside it. A working ULA holds the CPU's clock then; this one lets their accesses clash. The bits\n"
               "     that lose (%s) are the chips that are slowest, which show it first:\n", ++n, ula ? ula : "the ULA", e->c_bad,
               e->c_good, e->c_good - e->c_good_win, e->c_good - e->c_good_win == 1 ? "was" : "were", bits);
    }

    for (int ar = AR_LRAM; ar <= AR_URAM; ar++) {
        if (!e->late_n[ar]) continue;
        const char *bank = ar == AR_LRAM ? "lower RAM" : "upper RAM", *chips = ar == AR_LRAM ? "LRAM" : "URAM";
        for (int i = 0; i < 8; i++) {
            if (!(e->late_bits[ar] >> i & 1)) continue;
            char lab[8];
            snprintf(lab, sizeof(lab), "%s%d", chips, i);
            const char *ic = ref_of(lab);
            printf("  %d. %s, the %s's bit D%d: the CPU read it otherwise than the analyser saw. The RAM test read %02X\n"
                   "     at %04X (as the analyser saw it) but wrote back %02X, one less than %02X: what the CPU got. So D%d\n"
                   "     changes late in the read: a slow or failing chip (or one at the edge of its timing), or one not\n"
                   "     there at all (its pin floats and reads either way).\n", ++n,
                   ic ? ic : "the chip", bank, i, e->late_seen[ar], e->late_addr[ar], e->late_wrote[ar], (e->late_wrote[ar] + 1) & 0xFF, i);
            if (ar == AR_LRAM) ml &= ~(1u << i); else mu &= ~(1u << i);
        }
    }
    for (int ar = AR_LRAM; ar <= AR_URAM; ar++) {
        unsigned mm = ar == AR_LRAM ? ml : mu;
        const char *bank = ar == AR_LRAM ? "lower RAM" : "upper RAM", *chips = ar == AR_LRAM ? "LRAM" : "URAM";
        const char *range = ar == AR_LRAM ? "4000-7FFF" : "8000-FFFF";
        if (!mm) continue;
        bits_text(mm, bits, sizeof(bits));
        if (popcount8(mm) >= 3 && !(timing && ar == AR_LRAM)) {
            bank_parts(bank, chips, buf, sizeof(buf));
            data_resistors(chips, res, sizeof(res));
            printf("  %d. The %s (%s) as a whole: bits %s wrong (the first at %04X). Not one chip: what all its\n"
                   "     chips share: %s%s%s\n", ++n, bank, range, bits, e->first[ar], buf[0] ? buf : "its address multiplexers and select",
                   res[0] ? "; and for the data, the resistors to the CPU's data lines: " : "", res);
            if (ar == AR_LRAM && !e->c_have)
                printf("     The ULA's timing with the CPU (contention) can make this too; it can't be told here (no /INT in the\n"
                       "     capture to place the ULA's screen fetch). Run again with --la-all: the later stages show more.\n");
            continue;
        }
        int reads = 0;
        for (int i = 0; i < 8; i++) {
            if (!(mm >> i & 1)) continue;
            char lab[8];
            snprintf(lab, sizeof(lab), "%s%d", chips, i);
            const char *ic = ref_of(lab);
            reads += e->bad[ar][i];
            if (timing && ar == AR_LRAM) {
                printf("     %s (bit D%d, %d wrong read%s)\n", ic ? ic : "the chip", i, e->bad[ar][i], e->bad[ar][i] == 1 ? "" : "s");
                continue;
            }
            printf("  %d. %s, the %s's bit D%d (%s): it %s (%d read%s, the first at %04X)\n", ++n, ic ? ic : "the chip", bank, i,
                   range, bits_way(e, ar, i), e->bad[ar][i], e->bad[ar][i] == 1 ? "" : "s", e->first[ar]);
        }
        if (timing && ar == AR_LRAM) continue;
        if (reads <= 2) {
            /* One byte wrong can be the bank's addressing as well as a chip. */
            bank_parts(bank, chips, buf, sizeof(buf));
            if (buf[0])
                printf("  %d. Or what the %s's chips share, if they are all addressed wrong (only %d wrong read%s to go on):\n"
                       "     %s\n", ++n, bank, reads, reads == 1 ? "" : "s", buf);
        }
    }
    if (ev_pramt_bad(e)) {
        printf("     The RAM test (the ROM's own, at the switch on) stopped at %04X, in the %s; a working one's goes to %04X.\n",
               e->pramt + 1, e->pramt + 1 >= 0x8000 ? "upper RAM" : "lower RAM", e->pramt_ref);
        if (e->test_addr >= 0)
            printf("     There it %s; a working one's reads 02, writes 01, reads 01, writes 00.\n", e->test_seq);
        if (!ml && !mu && e->test_addr < 0)
            printf("  %d. The %s, from %04X: which bit isn't in the capture (it holds the last %d us before the test ended)\n", ++n,
                   e->pramt + 1 >= 0x8000 ? "upper RAM" : "lower RAM", e->pramt + 1, 9000 * 1000 / 14285);
    }
    if (e->kb_all)
        printf("  %d. %d keyboard read%s (port FE) gave all five lines low at once: not keys, the read itself. The ULA\n"
               "     (%s) answers it: its port read, or its data pins D0-D4, or its timing with the CPU.\n", ++n, e->kb_all,
               e->kb_all == 1 ? "" : "s", ula ? ula : "the ULA");
    if (e->held[LA_HALT]) {
        bool above = n > 0;
        printf("  %d. Left running, the CPU halts (/HALT, %s pin 18, low all through): it ran into a HALT%s. So it\n"
               "     went astray before that%s.\n", ++n, cpu, e->slow_ms && !e->int_n ? " with no interrupt to wake it" : "",
               above ? ", from what's above" : ": the boot's stages show where");
    }
    if (!n) {
        if (idle_st && !e->clk_edge && e->rom_writes < 50)
            printf("  No one part shows: left running, the CPU was %s.\n", where && where[0] ? where : "somewhere else");
        else if (missing)
            printf("  No one part shows in the captures: %s never came. When it should have, the CPU was %s.\n"
                   "  So it went astray after the stage before passed: the captures are in the folder, and the log has them.\n",
                   stage, where && where[0] ? where : "somewhere else");
        else if (co && co->diverged) {
            char tx[32], ty[32];
            cycle_text(&co->a, tx, sizeof(tx));
            cycle_text(&co->b, ty, sizeof(ty));
            printf("  No one part shows: at %s the CPU went another way, %s where a working one does %s.\n"
                   "  Look at the cycles before it in the log: the byte it read last decides where it goes.\n", stage, tx, ty);
        }
    }
done:
    if (e->clk_edge && !e->stuck[LA_CLK]) {
        printf("  %d. %s, the ULA: its clock output is weak. The edge connector's CLK (B8) is the ULA's pin 32 and reads\n"
               "     stuck %s, though the CPU ran the ROM's code (it gets its clock through a transistor, so it runs).\n"
               "     A working ULA's reads fine there. Measure B8 or the ULA's pin 32: a real Issue 2 had 1.4 V there,\n"
               "     and a new ULA fixed it (the ULA drives the lower RAM as well, so that can fail with it).\n", ++n,
               ula ? ula : "The ULA", e->clk_edge == 1 ? "low" : "high");
    }
    if (idle_st) {
        char still[200] = "";
        int ns = 0, m2 = 0;
        for (int k = 0; k < LA_NSIG; k++)
            if (e->idle_still[k] && m2 < (int)sizeof(still) - 20)
                m2 += snprintf(still + m2, sizeof(still) - m2, "%s%s (always %s)", ns++ ? ", " : "", la_signal_name(k),
                               e->idle_still[k] == 1 ? "low" : "high");
        if (e->rom_writes >= 50)
            printf("  %d. Left running, the CPU writes to the ROM (0000-3FFF) %d times: its stack is there. The ROM's RAM\n"
                   "     test sets the top of RAM below the first byte that fails it, so the RAM failed it at or near its\n"
                   "     start (4000): the lower RAM (4000-7FFF), the chips all of it shares (the ULA %s, the multiplexers),\n"
                   "     or one chip's bit there. The boot's stages (the RAM test) name it: run them.\n", ++n, e->rom_writes,
                   ula ? ula : "");
        if (ns)
            printf("  Note: left running, %s never changed. With the CPU running the ROM's code that is where it runs, not\n"
                   "     a stuck line (a 48K whose RAM test failed the upper RAM never goes above 8000): the boot's stages\n"
                   "     show the lines working or not.\n", still);
    }
    board_destroy(vboard);
    vboard = NULL;
    return n;
}

/* 0: as a working one, 1: something wrong, 2: not running, -1: none. */
int idle_stage(const la_cli_t *o, const machine_t *m, const char *dir, ev_t *ie) {
    char fpath[1024], spath[1024], err[512];
    snprintf(fpath, sizeof(fpath), "%s/idle.lac", dir);
    snprintf(spath, sizeof(spath), "%s/idle-slow.lac", dir);
    ev_init(ie);
    printf("\nFirst, idle: what it does left running, before any reset (the clock, /RESET, the frame interrupt, the CPU)\n");
    la_capture_t f, sl;
    bool have_slow = false;
    if (o->replay) {
        if (!la_read(fpath, &f, err, sizeof(err))) {
            printf("  Not in the folder (taken before there was this stage): on to the boot\n");
            return -1;
        }
        have_slow = la_read(spath, &sl, err, sizeof(err));
    } else if (o->simulate) {
        if (!record_idle(m, o->faults, o->nfaults, &f, &sl, err, sizeof(err))) { la_err(err); return -1; }
        have_slow = true;
        if (!la_write(fpath, &f, err, sizeof(err)) || !la_write(spath, &sl, err, sizeof(err))) la_err(err);
    } else {
        la_cli_t q = *o;
        q.diagnose = NULL;
        for (int t = 0; ; t++) {
            if (t == 0 && !want_enter("Leave the Spectrum switched on and running, don't touch the reset button or the keys, and press Enter\n"))
                return -1;
            if (!capture_now(&q, 0, fpath, &f, err, sizeof(err))) {
                la_err(err);
                printf("  Couldn't capture it: on to the boot\n");
                return -1;
            }
            printf("Now the same for 49 ms, slower (the frame interrupt is every 20 ms)\n");
            q.freq = IDLE_SLOW;
            have_slow = capture_now(&q, 0, spath, &sl, err, sizeof(err));
            q.freq = o->freq;
            if (!have_slow) { la_err(err); printf("  (without it: not the frame interrupt)\n"); }
            if (!la_write(fpath, &f, err, sizeof(err))) la_err(err);
            if (have_slow && !la_write(spath, &sl, err, sizeof(err))) la_err(err);
            bool rlow = (f.have >> LA_RESET & 1) && low_all(&f, LA_RESET) && (!have_slow || low_all(&sl, LA_RESET));
            if (t == 0 && rlow &&
                want_enter("/RESET was low all through: was the reset button held? Let go of it and press Enter to take it again\n"
                           "(or type n, Enter to go on with this one)\n")) {
                la_free(&f);
                if (have_slow) la_free(&sl);
                have_slow = false;
                continue;
            }
            break;
        }
    }
    la_capture_t rf, rs;
    if (!record_idle(m, NULL, 0, &rf, &rs, err, sizeof(err))) {
        la_err(err);
        la_free(&f);
        if (have_slow) la_free(&sl);
        return -1;
    }
    gather(ie, &f, &rf);
    where_text(&f, ie->idle_where, sizeof(ie->idle_where));
    if (have_slow) slow_ev(ie, &sl, &rs);
    ie->idle = 1;
    /* Left running with the CPU running the ROM's code, an address or data
     * line that never changes is where the code runs (a 48K whose RAM test
     * failed the upper RAM never goes above 8000), not proof of a stuck line. */
    if (ie->rom_runs)
        for (int k = 0; k < LA_NSIG; k++)
            if (ie->stuck[k] && ((k >= LA_A0 && k <= LA_A15) || (k >= LA_D0 && k <= LA_D7))) {
                ie->idle_still[k] = ie->stuck[k];
                ie->stuck[k] = 0;
            }
    la_free(&f); la_free(&rf); la_free(&rs);
    if (have_slow) la_free(&sl);
    /* What it shows. */
    if (ie->stuck[LA_CLK]) printf("  CLK: stuck %s\n", ie->stuck[LA_CLK] == 1 ? "low" : "high");
    else if (ie->clk_edge)
        printf("  CLK: reads stuck %s at the edge connector, but the CPU runs the ROM's code, so it has its clock\n",
               ie->clk_edge == 1 ? "low" : "high");
    else if (ie->clk_pct >= 0) printf("  CLK: running (the analyser sees %d%% of its pulses)\n", ie->clk_pct);
    if (ie->in_reset) printf("  /RESET: low all through\n");
    else if (ie->rst_pulses) printf("  /RESET: goes low %d time%s in %d ms\n", ie->rst_pulses, ie->rst_pulses == 1 ? "" : "s", ie->slow_ms);
    else printf("  /RESET: high\n");
    if (ie->held[LA_INT]) printf("  /INT: low all through\n");
    else if (ie->slow_ms) printf("  /INT: low %d time%s in %d ms (a working one's: %d, every 20 ms)\n", ie->int_n, ie->int_n == 1 ? "" : "s",
                                 ie->slow_ms, ie->int_ref);
    for (int k = 0; k < LA_NSIG; k++)
        if (ie->held[k] && k != LA_RESET && k != LA_INT) printf("  %s: low all through\n", la_signal_name(k));
    printf("  The CPU: %s\n", ie->idle_where);
    bool dead = idle_dead(ie), bad = dead || ev_any(ie);
    if (!bad && ie->rom_writes >= 50) bad = true;      /* its stack in the ROM: never on a working one */
    if (!bad && (strstr(ie->idle_where, "astray") || strstr(ie->idle_where, "not running") || strstr(ie->idle_where, "not one instruction"))) bad = true;
    if (!bad) { printf("  OK: the same as a working one\n"); return 0; }
    if (dead) { printf("  FAULT: it isn't running, so the boot's stages can't be taken (they need a reset and a running CPU)\n"); return 2; }
    printf("  FAULT: something's wrong (below); the boot's stages next show more\n");
    return 1;
}
