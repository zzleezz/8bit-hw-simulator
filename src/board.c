/* board.c - what every board has, whatever the machine: setting it up
 * from its netlist, the passive parts, faults, the network rebuild, the
 * oscilloscope and the logic probe statistics, labels and connectors.
 * The machines (c64.c) add the cores of their chips and the bus cycle
 * that moves them, through their machine_t. */
#include "board.h"
#include "c64.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/ldsyms.h>
#endif
#ifdef _WIN32
#include <direct.h>
#endif

/* Is 'word' one of the space separated words of 'list'? */
static bool in_list(const char *list, const char *word) {
    size_t n = strlen(word);
    for (const char *s = list; s && *s; ) {
        size_t len = strcspn(s, " ");
        if (len == n && !strncmp(s, word, n)) return true;
        s += len + (s[len] == ' ');
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Scope and statistics                                                      */
/* ------------------------------------------------------------------------ */

/* A scope sample of a pin: its net, and what its own leg drives. */
static uint8_t scope_sample(const sim_t *s, int pi) {
    const pin_t *pin = &s->pins[pi];
    uint8_t v = 0;
    if (pin->root >= 0) {
        const net_t *n = &s->nets[pin->root];
        v = n->level ? SC_LEVEL : 0;
        if (n->strength == S_Z) v |= SC_FLOATING;
        if (n->contention) v |= SC_FIGHT;
    }
    if ((pin->drive >> 1) != S_Z) v |= (pin->drive & 1) ? SC_OWN_HI : SC_OWN_LO;
    return v;
}

/* Edge counting (board_edges_start()): one board at a time, looked at in
 * every bus step while on (other boards, in other threads, only compare
 * their address). Kept out of board_t, so save files keep their layout. */
static struct {
    const board_t *b;
    uint8_t *last;
    uint32_t *rises, *highs, steps;
} edges;

static void count_edges(const sim_t *s) {
    for (int i = 0; i < s->nnets; i++) {
        const net_t *n = &s->nets[s->nets[i].root];
        uint8_t hi = n->strength != S_Z && n->level;
        edges.rises[i] += hi && !edges.last[i];
        edges.highs[i] += hi;
        edges.last[i] = hi;
    }
    edges.steps++;
}

void board_edges_start(board_t *b) {
    board_edges_stop(NULL, NULL, NULL);
    int n = b->sim.nnets;
    edges.last = calloc((size_t)n + 1, 1);
    edges.rises = calloc((size_t)n + 1, sizeof(uint32_t));
    edges.highs = calloc((size_t)n + 1, sizeof(uint32_t));
    edges.steps = 0;
    count_edges(&b->sim);           /* the levels to start from */
    memset(edges.rises, 0, (size_t)n * sizeof(uint32_t));
    memset(edges.highs, 0, (size_t)n * sizeof(uint32_t));
    edges.steps = 0;
    edges.b = b;
}

uint32_t board_edges_stop(board_t *b, uint32_t *rises, uint32_t *highs) {
    uint32_t steps = edges.steps;
    if (b && edges.b == b) {
        memcpy(rises, edges.rises, (size_t)b->sim.nnets * sizeof(uint32_t));
        memcpy(highs, edges.highs, (size_t)b->sim.nnets * sizeof(uint32_t));
    }
    free(edges.last); free(edges.rises); free(edges.highs);
    memset(&edges, 0, sizeof(edges));
    return steps;
}

void board_sample(board_t *b) {
    sim_t *s = &b->sim;
    if (edges.b == b) count_edges(s);
    if (b->scope_pin >= 0) {
        uint32_t at = b->scope_pos++ & (SCOPE_SAMPLES - 1);
        b->scope[at] = scope_sample(s, b->scope_pin);
        if (b->scope_pin2 >= 0) b->scope2[at] = scope_sample(s, b->scope_pin2);
    }
    /* Logic probe statistics: sample every net once in a while. */
    if (--b->st_countdown > 0) return;
    b->st_countdown = 97;
    /* Inputs reading an erratic level (a floating net near the threshold, a
     * lifted MOS leg) can change without any event: look at them again. */
    sim_wake_erratic(s);
    for (int i = 0; i < s->nnets; i++) {
        const net_t *n = &s->nets[s->nets[i].root];
        if (n->contention) b->st_fight[i]++;
        else if (n->strength == S_Z) b->st_float[i]++;
        else if (n->level) b->st_hi[i]++;
        else b->st_lo[i]++;
    }
    if (!b->pin_stats) return;
    for (int i = 0; i < s->npins; i++) {
        drive_t d = s->pins[i].drive;
        if ((d >> 1) == S_Z) b->pst_off[i]++;
        else if (d & 1) b->pst_hi[i]++;
        else b->pst_lo[i]++;
        if ((d >> 1) == S_PULL) b->pst_weak[i]++;
    }
    /* The PLA inputs are TTL: reading them has no side effects. */
    if (b->p_pla < 0) return;
    part_t *pla = &s->parts[b->p_pla];
    if (!pla->dead) {
        unsigned in = b->pla_super ? superpla_read_inputs(s, pla) : pla_read_inputs(s, pla);
        for (int t = 0; t < pla_nterms; t++)
            if ((in & pla_terms[t].mask) == pla_terms[t].value) b->pla_hits[t]++;
        b->pla_samples++;
    }
}

/* ------------------------------------------------------------------------ */
/* Labels                                                                    */
/* ------------------------------------------------------------------------ */

static const part_def_t *part_def(board_t *b, int part) {
    const part_def_t *d = b->m->part_def ? b->m->part_def(b, part) : NULL;
    if (d) return d;
    const char *ref = b->sim.parts[part].ref;
    for (int i = 0; i < b->m->nparts; i++)
        if (!strcmp(b->m->parts[i].ref, ref)) return &b->m->parts[i];
    return NULL;
}

const char *board_part_label(board_t *b, int part) {
    const part_def_t *d = part_def(b, part);
    return d ? d->label : b->sim.parts[part].name;
}

const char *board_part_detail(board_t *b, int part) {
    const part_def_t *d = part_def(b, part);
    return d ? d->detail : b->sim.parts[part].name;
}

const char *board_part_info(board_t *b, int part) {
    const part_def_t *d = part_def(b, part);
    return d ? d->info : "";
}

int board_part_role(board_t *b, int part) {
    const part_def_t *d = part_def(b, part);
    return d ? d->role : ROLE_OTHER;
}

/* Does a reference start with a prefix followed by its number ("C" matches
 * C12, not CN4 or CR1; "TI" matches TI-4)? */
static bool ref_is(const char *ref, const char *prefix) {
    size_t n = strlen(prefix);
    return !strncmp(ref, prefix, n) && ((ref[n] >= '0' && ref[n] <= '9') || ref[n] == '-' || !ref[n]);
}

const char *board_part_kind_name(board_t *b, int part) {
    const part_t *p = &b->sim.parts[part];
    if (p->kind == PART_IC) return "chip";
    if (p->kind == PART_CONNECTOR) return "connector";
    /* VR is a regulator on the C64, a trimmer ("2K2") on the Spectrum. */
    if (ref_is(p->ref, "VR") && p->name && p->name[0] >= '0' && p->name[0] <= '9' && strpbrk(p->name, "KkRM"))
        return "trimmer";
    static const char *const names[][2] = {
        { "FB", "ferrite bead" }, { "EM", "EMI filter" }, { "RP", "resistor pack" }, { "CR", "diode" }, { "JP", "solder jumper" },
        { "JROM", "wire link" }, { "OKI", "wire link" }, { "TI", "wire link" }, { "TR", "transistor" }, { "VR", "voltage regulator" }, { "REG", "voltage regulator" },
        { "SW", "switch" }, { "LS", "speaker" }, { "C", "capacitor" }, { "R", "resistor" }, { "D", "diode" },
        { "Q", "transistor" }, { "Y", "crystal" }, { "X", "crystal" }, { "L", "coil" }, { "F", "fuse" },
        { "E", "jumper wire" }, { "T", "transformer" }, { "M", "modulator" },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (ref_is(p->ref, names[i][0])) return names[i][1];
    return p->kind == PART_LINK ? "link" : p->kind == PART_RESISTOR ? "resistor" : "part";
}

static bool is_capacitor(board_t *b, int part) { return !strcmp(board_part_kind_name(b, part), "capacitor"); }

bool board_part_can_short(board_t *b, int part) {
    const part_t *p = &b->sim.parts[part];
    if (p->kind == PART_IC || p->kind == PART_CONNECTOR || p->kind == PART_LINK) return false;
    if (p->kind == PART_RESISTOR) return p->state && ((resistor_state_t *)p->state)->nel > 0;
    /* Legs 1 and 2: a capacitor (some footprints have a third pad for
     * another lead spacing), a diode, a two pin crystal. */
    return part_pin(p, 1) >= 0 && part_pin(p, 2) >= 0 && (p->npins == 2 || is_capacitor(b, part));
}

/* ------------------------------------------------------------------------ */
/* Passive parts                                                             */
/* ------------------------------------------------------------------------ */

/* Find which elements a resistor (or pack, or ferrite bead) is made of. */
static void setup_passive(board_t *b, int idx) {
    sim_t *s = &b->sim;
    part_t *p = &s->parts[idx];
    resistor_state_t *st = calloc(1, sizeof(*st));
    p->state = st;
    p->state_size = sizeof(*st);
    int pairs[16][2], npairs = 0;
    /* Bussed by its value (the C64's "R_Pack_Bussed") or its footprint
     * (KiCad's SIP array, pin 1 common: the Spectrum Issue 1's RP1; the
     * RP_ARRAY of the 250469, whose values don't say: the 250407's say
     * "isolated" on its RP1 and RP2). */
    const char *fp = s->bd->comps[idx].footprint;
    bool isolated = strstr(p->name, "isolated") != NULL;
    bool bussed = !isolated && (strstr(p->name, "Bussed") || strstr(fp, "R_Array_SIP") || strstr(fp, "RP_ARRAY"));
    if (bussed) {
        for (int n = 2; n < PIN_NUMBERS && npairs < 16; n++)
            if (p->pinmap[n] >= 0) { pairs[npairs][0] = p->pinmap[n]; pairs[npairs][1] = p->pinmap[1]; npairs++; }
    } else if (isolated) {
        for (int n = 1; n + 1 < PIN_NUMBERS && npairs < 16; n += 2)
            if (p->pinmap[n] >= 0 && p->pinmap[n + 1] >= 0) {
                pairs[npairs][0] = p->pinmap[n]; pairs[npairs][1] = p->pinmap[n + 1]; npairs++;
            }
    } else if (strstr(fp, "T-FILTER")) {
        /* A T filter (the 250469's EM1-EM9 on the joystick lines): the
         * line goes from pin 1 to pin 3, the capacitor to ground (pin 2)
         * is not simulated. */
        if (p->pinmap[1] >= 0 && p->pinmap[3] >= 0) { pairs[0][0] = p->pinmap[1]; pairs[0][1] = p->pinmap[3]; npairs = 1; }
    } else if (p->pinmap[1] >= 0 && p->pinmap[2] >= 0) {
        pairs[0][0] = p->pinmap[1]; pairs[0][1] = p->pinmap[2]; npairs = 1;
    }
    /* A "dashed" jumper is an option not fitted on this board (E2 selects
     * NTSC on the C64): it connects nothing. */
    if (strstr(p->name, "DASHED")) npairs = 0;
    /* Series resistors that are part of the logic (the others are analog). */
    bool series = in_list(b->m->link_resistors, p->ref);
    for (int i = 0; i < npairs; i++) {
        element_t *e = &st->el[st->nel++];
        int a = pairs[i][0], c = pairs[i][1];
        int ra = s->pins[a].net >= 0 ? s->nets[s->pins[a].net].rail : RAIL_NONE;
        int rc = s->pins[c].net >= 0 ? s->nets[s->pins[c].net].rail : RAIL_NONE;
        if (ra && !rc) { int t = a; a = c; c = t; t = ra; ra = rc; rc = t; }
        e->pin_a = a; e->pin_b = c;
        if (p->kind == PART_LINK) e->type = EL_LINK;
        else if (rc == RAIL_VCC && !ra) e->type = EL_PULLUP;
        else if (rc == RAIL_GND && !ra) e->type = EL_PULLDOWN;
        else if (series) e->type = EL_LINK;
        else e->type = EL_ANALOG;
        if (e->type == EL_PULLUP || e->type == EL_PULLDOWN) p->eval = resistor_eval;
    }
}

/* Does a reference start with one of the prefixes of a list, followed by
 * its number ("CR" matches CR5, not CRT1)? */
static bool in_prefix_list(const char *list, const char *ref) {
    for (const char *s = list; s && *s; ) {
        size_t len = strcspn(s, " ");
        if (!strncmp(s, ref, len) && ref[len] >= '0' && ref[len] <= '9') return true;
        s += len + (s[len] == ' ');
    }
    return false;
}

int board_passives(board_t *b, int part, passive_t *out, int max) {
    sim_t *s = &b->sim;
    part_t *p = &s->parts[part];
    int n = 0;
    if ((p->kind == PART_RESISTOR || p->kind == PART_LINK) && p->state) {
        resistor_state_t *st = p->state;
        for (int i = 0; i < st->nel && n < max; i++) {
            const element_t *e = &st->el[i];
            if (e->type == EL_NPN || e->type == EL_FOLLOWER) continue;     /* transistors */
            out[n++] = (passive_t){ e->pin_a, e->pin_b,
                                    e->type == EL_DIODE ? PASS_DIODE : p->kind == PART_LINK ? PASS_LINK : PASS_RESISTOR };
        }
        return n;
    }
    if (max < 1 || p->kind == PART_IC || p->kind == PART_CONNECTOR) return 0;
    int a = part_pin(p, 1), c = part_pin(p, 2);
    if (a < 0 || c < 0) return 0;
    if (is_capacitor(b, part)) out[n++] = (passive_t){ a, c, PASS_CAP };
    else if (p->npins != 2) return 0;
    else if (in_prefix_list(b->m->diodes, p->ref)) out[n++] = (passive_t){ a, c, PASS_DIODE };
    else if (in_prefix_list(b->m->jumpers, p->ref)) out[n++] = (passive_t){ a, c, PASS_LINK };
    /* Any other two pin part shorted is a wire to the meter. */
    else if (p->rfault == RF_SHORT) out[n++] = (passive_t){ a, c, PASS_LINK };
    return n;
}

int board_net_supply(board_t *b, int net) {
    if (net < 0) return RAIL_NONE;
    const net_t *n = &b->sim.nets[net];
    if (n->rail) return n->rail;
    return in_list(b->m->supplies, n->name) ? RAIL_VCC : RAIL_NONE;
}

bool board_part_simulated(board_t *b, int part) {
    part_t *p = &b->sim.parts[part];
    if (p->kind == PART_IC || p->kind == PART_CONNECTOR) return true;
    if (in_list(b->m->crystals, p->ref)) return true;
    if (p->kind == PART_RESISTOR || p->kind == PART_LINK) {
        resistor_state_t *st = p->state;
        for (int i = 0; st && i < st->nel; i++) if (st->el[i].type != EL_ANALOG) return true;
    }
    return false;
}

/* Two legs joined by a part, unless one is lifted. */
static void join_legs(sim_t *s, int pin_a, int pin_b) {
    if (s->pins[pin_a].fault == PF_OPEN || s->pins[pin_b].fault == PF_OPEN) return;
    sim_join(s, s->pins[pin_a].net, s->pins[pin_b].net);
}

/* The supplies of the healthy board, from its first rebuild: where each
 * power pin of a chip gets its supply. That is the biggest net (the plane)
 * of what the pin is joined to: a chip fed through a ferrite bead gets
 * +5V or +12V through it. */
static void supply_baseline(board_t *b) {
    sim_t *s = &b->sim;
    int *npins = calloc(s->nnets, sizeof(int)), *big = malloc(sizeof(int) * s->nnets);
    for (int i = 0; i < s->npins; i++) if (s->pins[i].net >= 0) npins[s->pins[i].net]++;
    for (int i = 0; i < s->nnets; i++) big[i] = -1;
    b->gnd_net = b->vcc_net = -1;
    for (int i = 0; i < s->nnets; i++) {
        int r = s->nets[i].root;
        if (big[r] < 0 || npins[i] > npins[big[r]]) big[r] = i;
        int *main = s->nets[i].rail == RAIL_GND ? &b->gnd_net : s->nets[i].rail == RAIL_VCC ? &b->vcc_net : NULL;
        if (main && (*main < 0 || npins[i] > npins[*main])) *main = i;
    }
    b->pwr_src = malloc(sizeof(int) * s->npins);
    b->pwr_rail = calloc(s->npins, 1);
    for (int i = 0; i < s->npins; i++) {
        const pin_t *p = &s->pins[i];
        b->pwr_src[i] = (p->flags & PIN_POWER) && p->root >= 0 ? big[p->root] : -1;
        if (b->pwr_src[i] >= 0) b->pwr_rail[i] = s->nets[p->root].rail;
    }
    free(npins);
    free(big);
}

/* Which chips lost a supply since the healthy board: the copper from the
 * power pin to its supply is cut (an open ferrite bead, a lifted leg of
 * it), or the supply is shorted to another (a shorted decoupling
 * capacitor pulls +5V down to ground). True if that changed for a chip. */
static bool supply_check(board_t *b) {
    sim_t *s = &b->sim;
    bool changed = false;
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        uint8_t cut = 0;
        for (int k = 0; k < p->npins; k++) {
            int pi = p->first_pin + k, src = b->pwr_src[pi];
            if (src < 0) continue;
            int root = s->nets[src].root;
            if (s->pins[pi].root == root && s->nets[root].rail == b->pwr_rail[pi]) continue;
            const char *name = s->pin_names[pi];
            cut |= name && (strstr(name, "GND") || !strcmp(name, "VSS")) ? 2 : 1;
        }
        if (p->supply_cut != cut) { p->supply_cut = cut; changed = true; }
    }
    return changed;
}

/* Rebuild the electrical network after a change: links, keys, faults. */
void board_rebuild(board_t *b) {
    sim_t *s = &b->sim;
    sim_join_begin(s);
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        if (p->rfault == RF_OPEN) continue;
        if ((p->kind == PART_RESISTOR || p->kind == PART_LINK) && p->state) {
            resistor_state_t *st = p->state;
            for (int k = 0; k < st->nel; k++) {
                element_t *e = &st->el[k];
                /* A shorted resistor (in series, in an RC delay, or not
                 * simulated) is a wire. Pull-ups, diodes and transistors
                 * do their short themselves (see resistor_eval). */
                bool wire = e->type == EL_LINK || (p->rfault == RF_SHORT &&
                            (e->type == EL_SERIES || e->type == EL_DELAY || e->type == EL_ANALOG));
                if (wire) join_legs(s, e->pin_a, e->pin_b);
            }
        } else if (p->rfault == RF_SHORT && board_part_can_short(b, i)) {
            /* A shorted capacitor, diode, crystal... */
            join_legs(s, part_pin(p, 1), part_pin(p, 2));
        }
    }
    /* Pressed keys, dongles... */
    if (b->m->joins) b->m->joins(b);
    /* The RESET button closes /RESET to GND. */
    if (b->reset_button && b->pin_reset >= 0 && b->pin_reset_gnd >= 0 &&
        s->pins[b->pin_reset].fault != PF_OPEN && s->pins[b->pin_reset_gnd].fault != PF_OPEN)
        sim_join(s, s->pins[b->pin_reset].net, s->pins[b->pin_reset_gnd].net);
    /* Solder bridges: the two nets become one (a lifted leg isn't bridged).
     * A bridge from an output to an input that drives it is a loop that
     * oscillates without end: after a few times, settle with less work.
     * (Only with a bridge, so every other fault runs as it always did.) */
    s->osc_after = b->nbridges ? 16 : 0;
    for (int i = 0; i < b->nbridges; i++) {
        int pa = b->bridge[i][0], pb = b->bridge[i][1];
        if (s->pins[pa].fault == PF_OPEN || s->pins[pb].fault == PF_OPEN) continue;
        sim_join(s, s->pins[pa].net, s->pins[pb].net);
    }
    for (int i = 0; i < s->nparts; i++) sim_update_part_power(s, i);
    sim_rebuild(s);
    if (!b->pwr_src) { supply_baseline(b); return; }
    /* A leg shorted to GND on a supply (a pad of a decoupling capacitor on
     * +5V, or on a supply behind a ferrite bead), or to +5V on ground: a
     * dead short between the two, which pulls the supply down. */
    bool more = false;
    for (int i = 0; i < s->npins; i++) {
        const pin_t *p = &s->pins[i];
        if (p->root < 0 || (p->fault != PF_GND && p->fault != PF_VCC)) continue;
        int rail = s->nets[p->root].rail, supply = rail ? rail : board_net_supply(b, p->net);
        bool dead = p->fault == PF_GND ? supply == RAIL_VCC : rail == RAIL_GND;
        int to = p->fault == PF_GND ? b->gnd_net : b->vcc_net;
        if (!dead || to < 0) continue;
        sim_join(s, p->net, to);
        more = true;
    }
    if (more) sim_rebuild(s);
    if (supply_check(b)) {
        for (int i = 0; i < s->nparts; i++) sim_update_part_power(s, i);
        sim_rebuild(s);
    }
}

/* ------------------------------------------------------------------------ */
/* Connectors                                                                */
/* ------------------------------------------------------------------------ */

/* Does a pad name match? The C64 cassette port pads are named after both
 * sides of the edge connector ("D,4"): either name matches. */
static bool pad_matches(const char *name, const char *pad) {
    size_t n = strlen(pad);
    for (const char *s = name; *s; ) {
        size_t len = strcspn(s, ",");
        if (len == n && !strncmp(s, pad, n)) return true;
        s += len + (s[len] == ',');
    }
    return false;
}

int board_pad_pin(board_t *b, int part, const char *pad) {
    if (part < 0) return -1;
    const part_t *p = &b->sim.parts[part];
    for (int k = 0; k < p->npins; k++)
        if (pad_matches(b->sim.bd->pads[p->first_pin + k].name, pad)) return p->first_pin + k;
    return -1;
}

static void setup_connectors(board_t *b) {
    sim_t *s = &b->sim;
    b->pin_func = calloc(s->npins, sizeof(char *));
    b->conn_strings = calloc((size_t)b->m->nconns + 1, sizeof(char *));
    for (int c = 0; c < b->m->nconns; c++) {
        int part = sim_find_part(s, b->m->conns[c].ref);
        if (part < 0) continue;
        /* The copy stays allocated: its pieces are the labels. */
        char *copy = strdup(b->m->conns[c].pins), *save = NULL;
        b->conn_strings[c] = copy;
        for (char *tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
            char *eq = strchr(tok, '=');
            *eq = 0;
            for (char *u = eq + 1; *u; u++) if (*u == '_') *u = ' ';
            /* A pad name can repeat (both sides of an edge connector). */
            const part_t *p = &s->parts[part];
            for (int k = 0; k < p->npins; k++)
                if (pad_matches(s->bd->pads[p->first_pin + k].name, tok)) b->pin_func[p->first_pin + k] = eq + 1;
        }
    }
}

const char *board_connector_name(board_t *b, int part) {
    for (int c = 0; c < b->m->nconns; c++)
        if (!strcmp(b->m->conns[c].ref, b->sim.parts[part].ref)) return b->m->conns[c].name;
    return NULL;
}

const char *board_pin_func(board_t *b, int pin) { return pin >= 0 ? b->pin_func[pin] : NULL; }

bool board_net_simulated(board_t *b, int net) {
    if (net < 0) return false;
    const sim_t *s = &b->sim;
    const net_t *n = &s->nets[s->nets[net].root];
    if (n->rail) return true;
    for (int k = 0; k < n->npins; k++) {
        int part = s->pins[n->pins[k]].part;
        if (s->parts[part].kind == PART_IC && board_part_simulated(b, part)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Life                                                                      */
/* ------------------------------------------------------------------------ */

board_t *board_create(const machine_t *m) {
    board_t *b = calloc(1, m->size);
    b->m = m;
    board_config_changed(b);
    sim_t *s = &b->sim;
    sim_init(s, m->data, &m->sim);
    for (int i = 0; i < s->nparts; i++) s->parts[i].user = b;
    for (int i = 0; i < m->nparts; i++) {
        int idx = sim_find_part(s, m->parts[i].ref);
        if (idx >= 0) part_set_type(s, idx, m->parts[i].type);
    }
    for (int i = 0; i < s->nparts; i++)
        if (s->parts[i].kind == PART_RESISTOR || s->parts[i].kind == PART_LINK) setup_passive(b, i);
    b->screen = calloc((size_t)m->screen_w * m->screen_h, sizeof(uint32_t));
    b->snow_seed = 12345;
    b->check = getenv("C64HW_CHECK") != NULL;
    b->scope_pin = b->scope_pin2 = -1;
    b->p_pla = b->pin_reset = b->pin_reset_gnd = -1;
    b->st_hi = calloc(s->nnets, sizeof(uint32_t));
    b->st_lo = calloc(s->nnets, sizeof(uint32_t));
    b->st_float = calloc(s->nnets, sizeof(uint32_t));
    b->st_fight = calloc(s->nnets, sizeof(uint32_t));
    b->pst_hi = calloc(s->npins, sizeof(uint32_t));
    b->pst_lo = calloc(s->npins, sizeof(uint32_t));
    b->pst_off = calloc(s->npins, sizeof(uint32_t));
    b->pst_weak = calloc(s->npins, sizeof(uint32_t));
    setup_connectors(b);
    m->create(b);
    board_power_on(b);
    return b;
}

void board_destroy(board_t *b) {
    if (b->m->destroy) b->m->destroy(b);
    sim_free(&b->sim);
    for (int c = 0; c < b->m->nconns; c++) free(b->conn_strings[c]);
    free(b->conn_strings);
    free(b->pin_func);
    free(b->pwr_src); free(b->pwr_rail);
    free(b->screen);
    free(b->st_hi); free(b->st_lo); free(b->st_float); free(b->st_fight);
    free(b->pst_hi); free(b->pst_lo); free(b->pst_off); free(b->pst_weak);
    free(b);
}

/* Cold start: like switching the machine on. Faults stay where they are. */
void board_power_on(board_t *b) {
    sim_t *s = &b->sim;
    s->cycle = 0;
    b->m->power_on(b);
    /* Nets start discharged, parts in their power-on state, and the noise
     * starts over (the same faults give the same run). */
    sim_discharge(s);
    sim_seed(s, 1);
    s->oscillations = 0;
    for (int i = 0; i < s->nparts; i++)
        if (s->parts[i].reset) s->parts[i].reset(s, &s->parts[i]);
    board_rebuild(b);
}

static uint32_t next_config_id;     /* boards are made in parallel threads (the atlas) */

void board_config_changed(board_t *b) { b->config_id = __atomic_add_fetch(&next_config_id, 1, __ATOMIC_RELAXED); }

/* ------------------------------------------------------------------------ */
/* Save states                                                               */
/* ------------------------------------------------------------------------ */

/* A copy of everything that changes while the machine runs: the machine's
 * struct (board_t, the sim_t in it, the chip cores), the pins and nets,
 * the parts' states (the DRAM cells...) and the picture. What only
 * changes with the hardware set up (a cartridge's ROM, the tables) is
 * left out: config_id makes sure it is the same. */
struct board_state {
    const machine_t *m;
    uint32_t config_id;
    void *machine;
    pin_t *pins;
    net_t *nets;
    uint8_t *nread;
    uint64_t *open_since;
    int *noisy;
    uint32_t *screen;
    uint8_t *states;
};

static void *dup_bytes(const void *p, size_t n) {
    void *d = malloc(n ? n : 1);
    memcpy(d, p, n);
    return d;
}

static size_t screen_bytes(const board_t *b) { return (size_t)b->m->screen_w * b->m->screen_h * sizeof(uint32_t); }

board_state_t *board_save_state(board_t *b) {
    sim_t *s = &b->sim;
    board_state_t *st = calloc(1, sizeof(*st));
    st->m = b->m;
    st->config_id = b->config_id;
    st->machine = dup_bytes(b, b->m->size);
    st->pins = dup_bytes(s->pins, sizeof(pin_t) * s->npins);
    st->nets = dup_bytes(s->nets, sizeof(net_t) * s->nnets);
    st->nread = dup_bytes(s->nread, s->nnets);
    st->open_since = dup_bytes(s->open_since, sizeof(uint64_t) * s->npins);
    st->noisy = dup_bytes(s->noisy, sizeof(int) * SIM_MAX_PARTS);
    st->screen = dup_bytes(b->screen, screen_bytes(b));
    size_t total = 0;
    for (int i = 0; i < s->nparts; i++) if (s->parts[i].state) total += s->parts[i].state_size;
    st->states = malloc(total ? total : 1);
    uint8_t *q = st->states;
    for (int i = 0; i < s->nparts; i++)
        if (s->parts[i].state) { memcpy(q, s->parts[i].state, s->parts[i].state_size); q += s->parts[i].state_size; }
    return st;
}

/* Copy a save state into the board. What the board allocated since (the
 * pools of the nets, rebuilt by every sim_rebuild) and the interface's
 * choices (the scope's pins) stay as they are. */
static void put_back(board_t *b, const board_state_t *st) {
    sim_t *s = &b->sim;
    board_t *before = dup_bytes(b, b->m->size);
    const sim_t *o = &before->sim;
    memcpy(b, st->machine, b->m->size);
    s->pin_pool = o->pin_pool; s->reader_pool = o->reader_pool; s->switch_pool = o->switch_pool;
    s->commit_list = o->commit_list; s->ncommit = o->ncommit;
    s->join_a = o->join_a; s->join_b = o->join_b; s->njoin = o->njoin; s->join_cap = o->join_cap;
    b->scope_pin = before->scope_pin; b->scope_pin2 = before->scope_pin2;
    b->pin_stats = before->pin_stats;
    b->check = before->check; b->check_errors = before->check_errors;
    if (b->m->state_loaded) b->m->state_loaded(b, before);
    free(before);
    memcpy(s->pins, st->pins, sizeof(pin_t) * s->npins);
    for (int i = 0; i < s->nnets; i++) {     /* the lists point into the pools */
        net_t *n = &s->nets[i];
        int *pins = n->pins, *readers = n->readers, *switches = n->switches;
        *n = st->nets[i];
        n->pins = pins; n->readers = readers; n->switches = switches;
    }
    memcpy(s->nread, st->nread, s->nnets);
    memcpy(s->open_since, st->open_since, sizeof(uint64_t) * s->npins);
    memcpy(s->noisy, st->noisy, sizeof(int) * SIM_MAX_PARTS);
    memcpy(b->screen, st->screen, screen_bytes(b));
    const uint8_t *q = st->states;
    for (int i = 0; i < s->nparts; i++)
        if (s->parts[i].state) { memcpy(s->parts[i].state, q, s->parts[i].state_size); q += s->parts[i].state_size; }
}

bool board_load_state(board_t *b, const board_state_t *st) {
    if (!st || st->m != b->m || st->config_id != b->config_id) return false;
    /* The faults, keys and links first, so the rebuild merges the nets as
     * they were; then the electrical state of those nets, over what the
     * rebuild worked out. */
    put_back(b, st);
    board_rebuild(b);
    put_back(b, st);
    return true;
}

void board_free_state(board_state_t *st) {
    if (!st) return;
    free(st->machine); free(st->pins); free(st->nets); free(st->nread);
    free(st->open_since); free(st->noisy); free(st->screen); free(st->states);
    free(st);
}

/* ------------------------------------------------------------------------ */
/* Save files                                                                */
/* ------------------------------------------------------------------------ */

/* A save state on disk. It is the memory of the machine, as a save state
 * in memory is, so the pointers in it (to what the board allocated, to
 * the program's functions and tables) are those of the program that
 * saved it. Loading makes the board again, set up the same way (the
 * machine, its options, the harness, what is plugged in), and takes the
 * pointers from it: a word that differs between two boards made the same
 * way points at what each allocated; one that differs from the same board
 * made by the program that saved it (kept in the file too) points into
 * the program (Windows loads it at another address each time, and a new
 * version has its functions elsewhere): if it is still what it was when
 * the board was made, the new board's is right. Every other word is the
 * machine's state and comes from the file. One whose machine has another
 * size (a version that lays it out differently) is refused.
 *
 * A file can come from anyone, so nothing in it may point the program
 * anywhere: a word that points into the program on the new board (the
 * parts' functions, the chips' tables) always comes from the new board,
 * never from the file (no such pointer changes while a machine runs: a
 * file in which one did is refused), and a file whose state holds an
 * address inside the program, where the new board keeps none, is refused
 * (a doctored file aiming a function pointer that is empty on a fresh
 * board). The parts' read caches, which do point into the program once a
 * machine has run, are cleared first (sf_forget_caches()). Measured on
 * save files of every board, with cartridges, faults and the harness:
 * neither then happens in a real one.
 *
 * The file: text lines ("KEY VALUE"), the notes (their length on the
 * "notes" line, then the text), a line "data", then binary blocks, each
 * its length (8 bytes, little endian) and its bytes: what is plugged in
 * (if anything), the fresh board, the saved one; last the signature (see
 * "Signing" below), which a loader not checking it never reads. */

#define SF_MAGIC "c64-hw-sim save state 1"
#define SF_EXT ".hwstate"
#define SF_MAX_OPTIONS 8
#define SF_NBLOBS 8

static const machine_t *const sf_machines[] = { &machine_c64, &machine_zx48, &machine_zx48i2, &machine_zx48i1,
                                                &machine_zx48i4b, &machine_zx48i6a, &machine_c64c };

/* How a board is set up. */
typedef struct {
    const machine_t *m;
    int options[SF_MAX_OPTIONS];
    bool harness;
    char plugged[260];          /* the file to plug, "" for none */
    int bank;
    int socket;                 /* a C64 ROM in the port (0) or in U4 (1) */
} sf_config_t;

static board_t *sf_make(const sf_config_t *c, char *err, int errlen) {
    board_t *b = board_create(c->m);
    for (int i = 0; i < c->m->noptions && i < SF_MAX_OPTIONS; i++)
        if (c->m->get_option(b, i) != c->options[i]) c->m->set_option(b, i, c->options[i]);
    if (c->harness) board_set_harness(b, true);
    if (c->plugged[0]) {
        char msg[256];
        if (!board_load(b, c->plugged, false, msg, sizeof(msg))) {
            snprintf(err, errlen, "can't plug in %s again: %s", c->plugged, msg);
            board_destroy(b);
            return NULL;
        }
        if (c->bank) board_set_cart_bank(b, c->bank);
        if (board_is_c64(b) && c64_cart_socket(b) >= 0 && c64_cart_socket(b) != c->socket)
            c64_set_cart_socket(b, c->socket);
    }
    return b;
}

/* The blocks of a save state of board b: where, how long. */
typedef struct { void **p; size_t n; } sf_blob_t;

static void sf_blobs(const board_t *b, board_state_t *st, sf_blob_t *out) {
    const sim_t *s = &b->sim;
    size_t total = 0;
    for (int i = 0; i < s->nparts; i++) if (s->parts[i].state) total += s->parts[i].state_size;
    out[0] = (sf_blob_t){ &st->machine, b->m->size };
    out[1] = (sf_blob_t){ (void **)&st->pins, sizeof(pin_t) * s->npins };
    out[2] = (sf_blob_t){ (void **)&st->nets, sizeof(net_t) * s->nnets };
    out[3] = (sf_blob_t){ (void **)&st->nread, (size_t)s->nnets };
    out[4] = (sf_blob_t){ (void **)&st->open_since, sizeof(uint64_t) * s->npins };
    out[5] = (sf_blob_t){ (void **)&st->noisy, sizeof(int) * SIM_MAX_PARTS };
    out[6] = (sf_blob_t){ (void **)&st->screen, screen_bytes(b) };
    out[7] = (sf_blob_t){ (void **)&st->states, total };
}

static void sf_put(FILE *f, const void *p, uint64_t n) {
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(n >> (8 * i));
    fwrite(len, 1, 8, f);
    if (n) fwrite(p, 1, n, f);
}

/* A block of the length expected (want), or of any length (want ~0: *got
 * says it). NULL if the file is short or the length isn't that. */
static void *sf_get(FILE *f, uint64_t want, size_t *got) {
    uint8_t len[8];
    if (fread(len, 1, 8, f) != 8) return NULL;
    uint64_t n = 0;
    for (int i = 0; i < 8; i++) n |= (uint64_t)len[i] << (8 * i);
    if ((want != ~(uint64_t)0 && n != want) || n > (64u << 20)) return NULL;
    void *p = malloc(n ? n : 1);
    if (n && fread(p, 1, n, f) != n) { free(p); return NULL; }
    if (got) *got = n;
    return p;
}

static void sf_put_state(FILE *f, board_t *b, board_state_t *st) {
    sf_blob_t bl[SF_NBLOBS];
    sf_blobs(b, st, bl);
    for (int i = 0; i < SF_NBLOBS; i++) sf_put(f, *bl[i].p, bl[i].n);
}

/* A save state of board b from the file (NULL if it doesn't fit b). */
static board_state_t *sf_get_state(FILE *f, board_t *b) {
    board_state_t *st = calloc(1, sizeof(*st));
    sf_blob_t bl[SF_NBLOBS];
    sf_blobs(b, st, bl);
    for (int i = 0; i < SF_NBLOBS; i++)
        if (!(*bl[i].p = sf_get(f, bl[i].n, NULL))) { board_free_state(st); return NULL; }
    st->m = b->m;
    st->config_id = b->config_id;
    return st;
}

/* Where the program itself is in memory (its code and its tables), to
 * tell pointers into it; 0, 0 if that can't be found out. */
static void sf_program(uint64_t *lo, uint64_t *hi) {
    *lo = *hi = 0;
#if defined(_WIN32)
    extern const uint8_t __ImageBase[];     /* the program's PE header, its size in it */
    const uint8_t *b = __ImageBase;
    uint32_t pe, size;
    memcpy(&pe, b + 0x3c, 4);
    memcpy(&size, b + pe + 24 + 56, 4);
    *lo = (uint64_t)(uintptr_t)b;
    *hi = *lo + size;
#elif defined(__APPLE__)
    const struct mach_header_64 *h = &_mh_execute_header;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    uint64_t a = ~(uint64_t)0, z = 0;
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const struct segment_command_64 *sg = (const void *)lc;
        if (sg->cmd == LC_SEGMENT_64 && strcmp(sg->segname, "__PAGEZERO")) {
            if (sg->vmaddr + slide < a) a = sg->vmaddr + slide;
            if (sg->vmaddr + slide + sg->vmsize > z) z = sg->vmaddr + slide + sg->vmsize;
        }
        lc += sg->cmdsize;
    }
    if (z) { *lo = a; *hi = z; }
#elif defined(__GNUC__)
    extern char __executable_start, _end;
    *lo = (uint64_t)(uintptr_t)&__executable_start;
    *hi = (uint64_t)(uintptr_t)&_end;
#endif
}

/* The pointers of the saved block r from the new board (see above): f is
 * the block of the fresh board of the program that saved, b1 that of the
 * new board as made, b2 of another made the same way, [lo, hi) the
 * program. Returns how many words differ unexplained, or point where they
 * may not. */
static int sf_relocate(uint8_t *r, const uint8_t *f, const uint8_t *b1, const uint8_t *b2, size_t n,
                       uint64_t lo, uint64_t hi) {
    int bad = 0;
    for (size_t o = 0; o + 8 <= n; o += 8) {
        uint64_t vr, vf, v1, v2;
        memcpy(&vr, r + o, 8);
        memcpy(&vf, f + o, 8);
        memcpy(&v1, b1 + o, 8);
        memcpy(&v2, b2 + o, 8);
        if (v1 != v2) memcpy(r + o, &v1, 8);                 /* what the board allocated */
        else if (v1 >= lo && v1 < hi) {                       /* into the program: the new board's */
            if (vr != vf) bad++;
            memcpy(r + o, &v1, 8);
        } else if (v1 != vf) {                                /* the program's, of another version */
            if (vr == vf) memcpy(r + o, &v1, 8);
            else bad++;
        } else if (vr >= lo && vr < hi) bad++;                /* state may not point into the program */
    }
    return bad;
}

/* The parts' read caches (group_t, sim_read_group()) out of a saved
 * machine: made again when first read, as after every rebuild. Their keys
 * point at the program's pin tables, set as the machine runs: not state,
 * and not to be taken from a file. */
static void sf_forget_caches(const board_t *b, board_state_t *st) {
    for (int i = 0; i < b->sim.nparts; i++) {
        size_t at = (size_t)((const uint8_t *)b->sim.parts[i].groups - (const uint8_t *)b);
        memset((uint8_t *)st->machine + at, 0, sizeof(b->sim.parts[i].groups));
    }
}

bool board_is_state_file(const char *path) {
    size_t n = strlen(path), e = strlen(SF_EXT);
    if (n <= e) return false;
    for (size_t i = 0; i < e; i++)
        if ((path[n - e + i] | 0x20) != SF_EXT[i] && path[n - e + i] != SF_EXT[i]) return false;
    return true;
}

const char *board_basename(const char *path) {
    const char *s = strrchr(path, '/'), *t = strrchr(path, '\\');
    if (t && (!s || t > s)) s = t;
    return s ? s + 1 : path;
}

static void *sf_slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = size >= 0 && size < (64l << 20) ? malloc((size_t)size + 1) : NULL;
    if (p && fread(p, 1, (size_t)size, f) != (size_t)size) { free(p); p = NULL; }
    fclose(f);
    *n = (size_t)size;
    return p;
}

/* Signing. A file whose words are taken as the machine's state can hold
 * indices (a pin, a part, a cell) the program uses without a check, so a
 * doctored one could make it read or write outside an array. Every file
 * saved here ends with a signature, HMAC-SHA-256 with a key made on this
 * PC and kept in the user's settings: a file that hasn't one of this PC
 * (someone else's, an earlier version's, or one changed since) is loaded
 * only once the user says so (board_state_file_signed()). */

static void sf_mkdir(const char *dir);

#define SF_SIGN_MAGIC "HWSIGN01"
#define SF_SIGN_LEN (8 + 32)

typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; } sha256_t;

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR32(x, r) (((x) >> (r)) | ((x) << (32 - (r))))

static void sha256_block(sha256_t *c, const uint8_t *p) {
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, c->h, sizeof(v));
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ROR32(v[4], 6) ^ ROR32(v[4], 11) ^ ROR32(v[4], 25);
        uint32_t t1 = v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha256_k[i] + w[i];
        uint32_t s0 = ROR32(v[0], 2) ^ ROR32(v[0], 13) ^ ROR32(v[0], 22);
        uint32_t t2 = s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof(uint32_t));
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) c->h[i] += v[i];
}

static void sha256_init(sha256_t *c) {
    static const uint32_t h0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, h0, sizeof(h0));
    c->n = 0;
}

static void sha256_add(sha256_t *c, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        size_t at = c->n & 63, take = 64 - at < n ? 64 - at : n;
        memcpy(c->buf + at, p, take);
        c->n += take; p += take; n -= take;
        if ((c->n & 63) == 0) sha256_block(c, c->buf);
    }
}

static void sha256_end(sha256_t *c, uint8_t out[32]) {
    uint64_t bits = c->n * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    sha256_add(c, &pad, 1);
    while ((c->n & 63) != 56) sha256_add(c, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_add(c, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

static void hmac_sha256(const uint8_t key[32], const uint8_t *data, size_t n, uint8_t out[32]) {
    uint8_t pad[64], inner[32];
    sha256_t c;
    memset(pad, 0x36, sizeof(pad));
    for (int i = 0; i < 32; i++) pad[i] ^= key[i];
    sha256_init(&c);
    sha256_add(&c, pad, 64);
    sha256_add(&c, data, n);
    sha256_end(&c, inner);
    memset(pad, 0x5c, sizeof(pad));
    for (int i = 0; i < 32; i++) pad[i] ^= key[i];
    sha256_init(&c);
    sha256_add(&c, pad, 64);
    sha256_add(&c, inner, 32);
    sha256_end(&c, out);
}

#ifdef _WIN32
/* RtlGenRandom, in advapi32. */
__declspec(dllimport) unsigned char __stdcall SystemFunction036(void *buf, unsigned long len);
#endif

static bool sf_random(uint8_t *out, size_t n) {
#ifdef _WIN32
    return SystemFunction036(out, (unsigned long)n) != 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    bool ok = f && fread(out, 1, n, f) == n;
    if (f) fclose(f);
    return ok;
#endif
}

/* This PC's signing key: read, or made the first time (MAKE). False if
 * there is none and it can't be made. */
static bool sf_key(uint8_t key[32], bool make) {
    char path[1100];
#ifdef _WIN32
    const char *home = getenv("APPDATA");
    if (!home) return false;
    snprintf(path, sizeof(path), "%s/c64-hw-sim", home);
#else
    const char *home = getenv("HOME");
    if (!home) return false;
    snprintf(path, sizeof(path), "%s/.config", home);
    sf_mkdir(path);
    snprintf(path, sizeof(path), "%s/.config/c64-hw-sim", home);
#endif
    sf_mkdir(path);
    snprintf(path + strlen(path), sizeof(path) - strlen(path), "/save.key");
    FILE *f = fopen(path, "rb");
    if (f) {
        bool ok = fread(key, 1, 32, f) == 32;
        fclose(f);
        if (ok || !make) return ok;
    }
    if (!make || !sf_random(key, 32) || !(f = fopen(path, "wb"))) return false;
    bool ok = fwrite(key, 1, 32, f) == 32;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* Sign the save file at PATH (its signature appended). */
static void sf_sign(const char *path) {
    uint8_t key[32], mac[32];
    size_t n;
    uint8_t *data;
    if (!sf_key(key, true) || !(data = sf_slurp(path, &n))) return;
    hmac_sha256(key, data, n, mac);
    free(data);
    FILE *f = fopen(path, "ab");
    if (!f) return;
    fwrite(SF_SIGN_MAGIC, 1, 8, f);
    fwrite(mac, 1, 32, f);
    fclose(f);
}

bool board_state_file_signed(const char *path) {
    uint8_t key[32], mac[32];
    size_t n;
    uint8_t *data;
    if (!sf_key(key, false) || !(data = sf_slurp(path, &n))) return false;
    bool ok = n > SF_SIGN_LEN && !memcmp(data + n - SF_SIGN_LEN, SF_SIGN_MAGIC, 8);
    if (ok) {
        hmac_sha256(key, data, n - SF_SIGN_LEN, mac);
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= mac[i] ^ data[n - 32 + i];
        ok = diff == 0;
    }
    free(data);
    return ok;
}

bool board_save_file(board_t *b, const char *path, const char *notes, char *err, int errlen) {
    sf_config_t c;
    memset(&c, 0, sizeof(c));
    c.m = b->m;
    for (int i = 0; i < b->m->noptions && i < SF_MAX_OPTIONS; i++) c.options[i] = b->m->get_option(b, i);
    c.harness = board_harness(b);
    snprintf(c.plugged, sizeof(c.plugged), "%s", b->plugged);
    c.bank = c.plugged[0] ? board_cart_bank(b) : 0;
    c.socket = board_is_c64(b) && c64_cart_socket(b) > 0;
    size_t nplug = 0;
    uint8_t *plug = NULL;
    if (c.plugged[0] && !(plug = sf_slurp(c.plugged, &nplug))) {
        snprintf(err, errlen, "can't read %s, which is plugged in", c.plugged);
        return false;
    }
    board_t *fresh = sf_make(&c, err, errlen);
    if (!fresh) { free(plug); return false; }
    memset(fresh->plugged, 0, sizeof(fresh->plugged));     /* where it was is no part of the machine */
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(err, errlen, "can't write %s", path);
        free(plug);
        board_destroy(fresh);
        return false;
    }
    fprintf(f, "%s\nboard %s\n", SF_MAGIC, b->m->board_name);
    for (int i = 0; i < b->m->noptions && i < SF_MAX_OPTIONS; i++)
        fprintf(f, "option %s %s\n", b->m->options[i].id, b->m->options[i].value_ids[c.options[i]]);
    fprintf(f, "harness %d\n", c.harness);
    if (c.plugged[0]) fprintf(f, "plugged %s\nbank %d\nsocket %d\n", board_basename(c.plugged), c.bank, c.socket);
    fprintf(f, "time %.3f\n", (double)b->sim.cycle / b->m->cpu_hz);
    fprintf(f, "base %llu\n", (unsigned long long)(uintptr_t)&machine_c64);
    fprintf(f, "notes %zu\n", strlen(notes));
    fwrite(notes, 1, strlen(notes), f);
    fprintf(f, "\ndata\n");
    if (c.plugged[0]) sf_put(f, plug, nplug);
    board_state_t *fs = board_save_state(fresh), *rs = board_save_state(b);
    sf_put_state(f, fresh, fs);
    sf_put_state(f, b, rs);
    board_free_state(fs);
    board_free_state(rs);
    board_destroy(fresh);
    free(plug);
    if (fclose(f) != 0) {
        snprintf(err, errlen, "can't write %s", path);
        return false;
    }
    sf_sign(path);
    return true;
}

/* The text part of a save file: the setup, the notes (if notes), the
 * base address of the program that saved it and when. False if it isn't
 * one. */
static bool sf_header(FILE *f, sf_config_t *c, char *plugged_name, int namelen, char *notes, int noteslen,
                      uint64_t *base, double *time, char *err, int errlen) {
    char line[512];
    memset(c, 0, sizeof(*c));
    if (notes && noteslen) notes[0] = 0;
    if (!fgets(line, sizeof(line), f) || strncmp(line, SF_MAGIC, strlen(SF_MAGIC))) {
        snprintf(err, errlen, "not a save file of this simulator");
        return false;
    }
    plugged_name[0] = 0;
    *base = 0;
    *time = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, ' ');
        if (v) *v++ = 0;
        else v = line + strlen(line);
        if (!strcmp(line, "data")) {
            if (!c->m) snprintf(err, errlen, "no board in the file");
            return c->m != NULL;
        }
        if (!strcmp(line, "board")) {
            for (size_t i = 0; i < sizeof(sf_machines) / sizeof(sf_machines[0]); i++)
                if (!strcmp(v, sf_machines[i]->board_name)) c->m = sf_machines[i];
            if (!c->m) { snprintf(err, errlen, "a board this simulator doesn't have: %s", v); return false; }
        } else if (!strcmp(line, "option") && c->m) {
            char id[64], val[64];
            if (sscanf(v, "%63s %63s", id, val) != 2) continue;
            bool found = false;
            for (int i = 0; i < c->m->noptions && i < SF_MAX_OPTIONS; i++) {
                if (strcmp(c->m->options[i].id, id)) continue;
                for (int k = 0; k < c->m->options[i].nvalues; k++)
                    if (!strcmp(c->m->options[i].value_ids[k], val)) { c->options[i] = k; found = true; }
            }
            if (!found) { snprintf(err, errlen, "an option this board doesn't have: %s %s", id, val); return false; }
        } else if (!strcmp(line, "harness")) c->harness = atoi(v) != 0;
        else if (!strcmp(line, "plugged")) snprintf(plugged_name, namelen, "%s", board_basename(v));
        else if (!strcmp(line, "bank")) c->bank = atoi(v);
        else if (!strcmp(line, "socket")) c->socket = atoi(v) != 0;
        else if (!strcmp(line, "time")) *time = atof(v);
        else if (!strcmp(line, "base")) *base = strtoull(v, NULL, 10);
        else if (!strcmp(line, "notes")) {
            size_t n = (size_t)strtoull(v, NULL, 10);
            for (size_t i = 0; i < n; i++) {
                int ch = fgetc(f);
                if (ch == EOF) break;
                if (notes && (int)i < noteslen - 1) { notes[i] = (char)ch; notes[i + 1] = 0; }
            }
            fgetc(f);           /* the end of the line */
        }
    }
    snprintf(err, errlen, "the file is cut short");
    return false;
}

const file_kind_t *board_state_file_kind(const char *path, char *info, int len, bool *bad) {
    static const file_kind_t kind = { "save state", "Load the save state", false, -1 };
    if (!board_is_state_file(path)) return NULL;
    FILE *f = fopen(path, "rb");
    sf_config_t c;
    char name[260], err[128];
    uint64_t base;
    double time;
    *bad = !f || !sf_header(f, &c, name, sizeof(name), NULL, 0, &base, &time, err, sizeof(err));
    if (!f) snprintf(info, len, "can't read the file");
    else if (*bad) snprintf(info, len, "%s", err);
    else snprintf(info, len, "%s at %.1f s%s%s", c.m->board_name, time, name[0] ? ", with " : "", name);
    if (f) fclose(f);
    return &kind;
}

static void sf_mkdir(const char *dir) {
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0777);
#endif
}

/* Where a plugged file from a save file goes: the system's temporary
 * folder, under its own name (the cartridge is called by it), in a folder
 * named by its contents: two files of the same name (in two save files,
 * or two copies of the program) don't overwrite each other, and the same
 * file loaded again goes where it went before. */
static bool sf_temp_path(const char *name, const uint8_t *data, size_t n, char *out, int len) {
    const char *dir = getenv("TEMP");
    if (!dir) dir = getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    uint64_t h = 14695981039346656037ull;       /* FNV-1a */
    for (size_t i = 0; i < n; i++) h = (h ^ data[i]) * 1099511628211ull;
    snprintf(out, len, "%s/c64-hw-sim", dir);
    sf_mkdir(out);
    snprintf(out + strlen(out), len - strlen(out), "/%016llx", (unsigned long long)h);
    sf_mkdir(out);
    snprintf(out + strlen(out), len - strlen(out), "/%s", name);
    return true;
}

board_t *board_load_file(const char *path, char *notes, int noteslen, char *err, int errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "can't read the file"); return NULL; }
    sf_config_t c;
    char name[260];
    uint64_t base;
    double time;
    if (!sf_header(f, &c, name, sizeof(name), notes, noteslen, &base, &time, err, errlen)) { fclose(f); return NULL; }
    if (name[0]) {
        size_t n;
        uint8_t *plug = sf_get(f, ~(uint64_t)0, &n);
        FILE *o = NULL;
        if (plug && sf_temp_path(name, plug, n, c.plugged, sizeof(c.plugged))) o = fopen(c.plugged, "wb");
        bool ok = o && fwrite(plug, 1, n, o) == n;
        if (o && fclose(o) != 0) ok = false;
        free(plug);
        if (!ok) { snprintf(err, errlen, "can't put %s in the temporary folder", name); fclose(f); return NULL; }
    }
    board_t *b1 = sf_make(&c, err, errlen), *b2 = b1 ? sf_make(&c, err, errlen) : NULL;
    board_state_t *fs = b2 ? sf_get_state(f, b1) : NULL, *rs = fs ? sf_get_state(f, b1) : NULL;
    fclose(f);
    if (!rs) {
        if (b2) snprintf(err, errlen, "the file is cut short, or made by another version of the simulator");
        board_free_state(fs);
        if (b1) board_destroy(b1);
        if (b2) board_destroy(b2);
        return NULL;
    }
    /* The pointers, from the new board. */
    char plugged[sizeof(b1->plugged)];
    snprintf(plugged, sizeof(plugged), "%s", b1->plugged);
    memset(b1->plugged, 0, sizeof(b1->plugged));
    memset(b2->plugged, 0, sizeof(b2->plugged));
    board_state_t *s1 = board_save_state(b1), *s2 = board_save_state(b2);
    uint64_t lo, hi;
    sf_program(&lo, &hi);
    (void)base;                 /* where the program was: its pointers all come from the new board now */
    sf_forget_caches(b1, fs);
    sf_forget_caches(b1, rs);
    sf_forget_caches(b1, s1);
    sf_forget_caches(b1, s2);
    sf_blob_t bf[SF_NBLOBS], br[SF_NBLOBS], b1l[SF_NBLOBS], b2l[SF_NBLOBS];
    sf_blobs(b1, fs, bf);
    sf_blobs(b1, rs, br);
    sf_blobs(b1, s1, b1l);
    sf_blobs(b1, s2, b2l);
    int bad = 0;
    for (int i = 0; i < SF_NBLOBS - 1; i++)
        bad += sf_relocate(*br[i].p, *bf[i].p, *b1l[i].p, *b2l[i].p, br[i].n, lo, hi);
    /* The parts' states one by one: each starts on a word of its own. */
    size_t at = 0;
    for (int i = 0; i < b1->sim.nparts; i++) {
        const part_t *p = &b1->sim.parts[i];
        if (!p->state) continue;
        bad += sf_relocate(rs->states + at, fs->states + at, s1->states + at, s2->states + at, p->state_size, lo, hi);
        at += p->state_size;
    }
    board_free_state(fs);
    board_free_state(s1);
    board_free_state(s2);
    board_destroy(b2);
    if (bad) {
        snprintf(err, errlen, "made by another version of the simulator, or altered (%d words of the machine "
                 "don't fit this one): it can't be loaded", bad);
        board_free_state(rs);
        board_destroy(b1);
        return NULL;
    }
    board_load_state(b1, rs);
    snprintf(b1->plugged, sizeof(b1->plugged), "%s", plugged);
    board_free_state(rs);
    return b1;
}

void board_run(board_t *b, int cycles) { b->m->run(b, cycles); }

void board_key_down(board_t *b, int key) { b->m->key_down(b, key); }
void board_key_up(board_t *b, int key) { b->m->key_up(b, key); }
void board_frame_update(board_t *b, uint32_t us) { b->m->frame_update(b, us); }
void board_monitor(board_t *b, uint32_t *out) { b->m->monitor(b, out); }

bool board_load(board_t *b, const char *path, bool run, char *msg, int msglen) {
    uint32_t was = b->config_id;
    bool ok = b->m->load(b, path, run, msg, msglen);
    if (ok && b->config_id != was) snprintf(b->plugged, sizeof(b->plugged), "%s", path);
    return ok;
}

const char *board_cart_name(board_t *b) { return b->m->cart_name ? b->m->cart_name(b) : NULL; }
void board_remove_cart(board_t *b) {
    if (b->m->remove_cart) b->m->remove_cart(b);
    b->plugged[0] = 0;
}
int board_cart_banks(board_t *b) { return b->m->cart_banks ? b->m->cart_banks(b) : 0; }
int board_cart_bank(board_t *b) { return b->m->cart_bank ? b->m->cart_bank(b) : 0; }
const char *board_cart_bank_info(board_t *b, int bank) {
    return b->m->cart_bank_info ? b->m->cart_bank_info(b, bank) : "";
}
void board_set_cart_bank(board_t *b, int bank) { if (b->m->set_cart_bank) b->m->set_cart_bank(b, bank); }

/* ------------------------------------------------------------------------ */
/* Faults                                                                    */
/* ------------------------------------------------------------------------ */

void board_set_pin_fault(board_t *b, int pin, int fault) {
    sim_set_pin_fault(&b->sim, pin, fault);
    board_rebuild(b);
}

void board_set_removed(board_t *b, int part, bool removed) {
    if (b->sim.parts[part].unfitted) return;    /* nothing there to put back */
    b->sim.parts[part].removed = removed;
    board_rebuild(b);
}

void board_set_rfault(board_t *b, int part, int rfault) {
    b->sim.parts[part].rfault = rfault;
    board_rebuild(b);
}

/* The RF_* faults as fault specs ("TR4:be_short"). */
static const char *const rf_specs[] = { "ok", "open", "short", "be_short", "bc_short", "be_open", "bc_open", "weak" };

const char *board_rfault_spec(int rfault) {
    return rfault >= 0 && rfault < (int)(sizeof(rf_specs) / sizeof(rf_specs[0])) ? rf_specs[rfault] : "ok";
}

bool board_rfault_fits(board_t *b, int part, int rfault) {
    if (rfault <= RF_SHORT) return true;
    if (strcmp(board_part_kind_name(b, part), "transistor")) return false;
    return rfault != RF_WEAK || in_list(b->m->crystals, b->sim.parts[part].ref);     /* the machine models it */
}

const char *board_rfault_name(board_t *b, int part, int rfault) {
    bool tr = !strcmp(board_part_kind_name(b, part), "transistor");
    switch (rfault) {
    case RF_OK: return "working";
    case RF_SHORT: return tr ? "shorted collector to emitter" : "shorted";
    case RF_BE_SHORT: return "base-emitter junction shorted";
    case RF_BC_SHORT: return "base-collector junction shorted";
    case RF_BE_OPEN: return "base-emitter junction open";
    case RF_BC_OPEN: return "base-collector junction open";
    case RF_WEAK: return "weak: tests good, fails under load";
    default: return !strcmp(board_part_kind_name(b, part), "switch") ? "switched off" : "open";
    }
}

void board_set_mem_fault(board_t *b, int part, const mem_fault_t *mf) {
    part_t *p = &b->sim.parts[part];
    p->mfault = mf->kind;
    p->mf_bit = mf->bit;
    p->mf_val = mf->val;
    p->mf_addr = mf->addr;
    board_rebuild(b);
}

bool board_parse_mem_fault(board_t *b, int part, const char *what, mem_fault_t *mf) {
    int bits;
    bool dram;
    int lines = part >= 0 ? part_memory(&b->sim.parts[part], &bits, &dram) : 0;
    unsigned n, v, addr, mask = 1;
    char end;
    memset(mf, 0, sizeof(*mf));
    if (!lines) return false;
    if (sscanf(what, "data%u=%u%c", &n, &v, &end) == 2) {
        if (n >= (unsigned)bits || v > 1) return false;
        *mf = (mem_fault_t){ MF_DATA, (uint8_t)n, (uint8_t)v, 0 };
    } else if (sscanf(what, "addr%u%c", &n, &end) == 1) {
        if (n >= (unsigned)lines) return false;
        *mf = (mem_fault_t){ MF_ADDR, (uint8_t)n, 0, 0 };
    } else if (!strncmp(what, "flip=", 5) || !strncmp(what, "flip@", 5)) {
        int r = sscanf(what + 5, "%x%c", &addr, &end);
        if (r < 1 || (r == 2 && end != '^')) return false;
        if (r == 2 && sscanf(strchr(what, '^') + 1, "%x%c", &mask, &end) != 1) return false;
        if (what[4] == '@') {       /* a CPU address: where it is in this chip */
            int cell = b->m->ram_cell && addr <= 0xFFFF ? b->m->ram_cell(b, part, (uint16_t)addr) : -1;
            if (cell < 0) return false;
            addr = (unsigned)cell;
        }
        if (!mask || mask >= 1u << bits) return false;
        bool fits = dram ? (addr >> 8) < 1u << lines && (addr & 255) < 1u << lines : addr < 1u << lines;
        if (!fits) return false;
        *mf = (mem_fault_t){ MF_FLIP, 0, (uint8_t)mask, addr };
    } else if (!strcmp(what, "weak")) {
        if (!dram) return false;
        mf->kind = MF_WEAK;
    } else {
        return !strcmp(what, "ok");     /* MF_OK */
    }
    return true;
}

void board_mem_fault_spec(const mem_fault_t *mf, char *out, int len) {
    switch (mf->kind) {
    case MF_DATA: snprintf(out, (size_t)len, "data%d=%d", mf->bit, mf->val); break;
    case MF_ADDR: snprintf(out, (size_t)len, "addr%d", mf->bit); break;
    case MF_FLIP:
        if (mf->val == 1) snprintf(out, (size_t)len, "flip=%04X", (unsigned)mf->addr);
        else snprintf(out, (size_t)len, "flip=%04X^%02X", (unsigned)mf->addr, mf->val);
        break;
    case MF_WEAK: snprintf(out, (size_t)len, "weak"); break;
    default:      snprintf(out, (size_t)len, "ok"); break;
    }
}

void board_mem_fault_desc(board_t *b, int part, const mem_fault_t *mf, char *out, int len) {
    bool dram;
    int bits;
    part_memory(&b->sim.parts[part], &bits, &dram);
    switch (mf->kind) {
    case MF_DATA:
        if (bits == 1) snprintf(out, (size_t)len, "dead: reads %d at every address", mf->val);
        else snprintf(out, (size_t)len, "data bit D%d dead: reads %d at every address", mf->bit, mf->val);
        break;
    case MF_ADDR:
        snprintf(out, (size_t)len, "address line A%d dead inside the chip: half of it is never reached, "
                 "the other half answers twice", mf->bit);
        break;
    case MF_FLIP:
        if (dram)
            snprintf(out, (size_t)len, "one bad cell (row $%02X, column $%02X) reads back wrong",
                     (unsigned)mf->addr >> 8, (unsigned)mf->addr & 255);
        else if (bits == 4)
            snprintf(out, (size_t)len, "one bad cell at $%03X reads back wrong (bits $%X inverted)",
                     (unsigned)mf->addr, mf->val);
        else
            snprintf(out, (size_t)len, "byte $%04X corrupted (bits $%02X inverted)", (unsigned)mf->addr, mf->val);
        break;
    case MF_WEAK:
        snprintf(out, (size_t)len, "weak cells: about 1 in 64 forget what was written (read 0)");
        break;
    default:
        snprintf(out, (size_t)len, "working");
        break;
    }
}

/* The RESET button: not a fault, a switch (see board_rebuild). */
void board_set_reset_button(board_t *b, bool pressed) {
    if (b->reset_button == pressed) return;
    b->reset_button = pressed;
    board_rebuild(b);
}

int board_find_bridge(board_t *b, int pin_a, int pin_b) {
    for (int i = 0; i < b->nbridges; i++)
        if ((b->bridge[i][0] == pin_a && b->bridge[i][1] == pin_b) ||
            (b->bridge[i][0] == pin_b && b->bridge[i][1] == pin_a)) return i;
    return -1;
}

bool board_set_bridge(board_t *b, int pin_a, int pin_b, bool on) {
    int i = board_find_bridge(b, pin_a, pin_b);
    if (on) {
        if (i >= 0) return true;
        if (b->nbridges == BOARD_MAX_BRIDGES || pin_a == pin_b) return false;
        /* Lower pin number first, so the spec reads "11-12". */
        if (b->sim.pins[pin_a].number > b->sim.pins[pin_b].number) { int t = pin_a; pin_a = pin_b; pin_b = t; }
        b->bridge[b->nbridges][0] = pin_a;
        b->bridge[b->nbridges][1] = pin_b;
        b->nbridges++;
    } else {
        if (i < 0) return true;
        b->bridge[i][0] = b->bridge[b->nbridges - 1][0];
        b->bridge[i][1] = b->bridge[b->nbridges - 1][1];
        b->nbridges--;
    }
    board_rebuild(b);
    return true;
}

int board_dip_neighbor(board_t *b, int pin, int dir) {
    const sim_t *s = &b->sim;
    const part_t *p = &s->parts[s->pins[pin].part];
    int n = p->npins, num = s->pins[pin].number, other = num + dir;
    /* A DIP: pins 1..N, N even, every number there. */
    if (p->kind != PART_IC || n < 4 || n % 2 || n >= (int)(sizeof(p->pinmap) / sizeof(p->pinmap[0])) ||
        num < 1 || num > n) return -1;
    if (other < 1 || other > n || (num <= n / 2) != (other <= n / 2)) return -1;
    return p->pinmap[other];
}

void board_bridge_desc(board_t *b, int pin_a, int pin_b, char *out, int len) {
    const sim_t *s = &b->sim;
    if (s->pins[pin_a].number > s->pins[pin_b].number) { int t = pin_a; pin_a = pin_b; pin_b = t; }
    int part = s->pins[pin_a].part;
    const part_t *p = &s->parts[part];
    /* The signals: the net's name, or the pin's when the net has none of
     * its own ("Net-(U13-Pad11)"). */
    const char *what[2];
    for (int k = 0; k < 2; k++) {
        int pi = k ? pin_b : pin_a, net = s->pins[pi].net;
        const char *nn = net >= 0 ? s->nets[net].name : NULL, *pn = sim_pin_name(s, pi);
        what[k] = nn && strncmp(nn, "Net-", 4) ? nn : pn ? pn : nn ? nn : "?";
    }
    snprintf(out, (size_t)len, "%s %s pins %d-%d shorted: %s and %s bridged", p->ref,
             p->name ? p->name : board_part_label(b, part), s->pins[pin_a].number, s->pins[pin_b].number,
             what[0], what[1]);
}

/* Text form of a fault, used by the command line (see board.h). */
bool board_apply_fault_spec(board_t *b, const char *spec) {
    if (!strcmp(spec, "RESET:pressed") || !strcmp(spec, "RESET:released")) {
        board_set_reset_button(b, !strcmp(spec, "RESET:pressed"));
        return true;
    }
    char ref[16] = {0}, pad[8] = {0}, pad2[8] = {0}, what[16] = {0};
    /* A solder bridge between two legs of a part: U13.11-12:short */
    if (sscanf(spec, "%15[^.:].%7[^-:]-%7[^:]:%15s", ref, pad, pad2, what) == 4) {
        int part = sim_find_part(&b->sim, ref);
        if (part < 0) return false;
        int pa = board_pad_pin(b, part, pad), pb = board_pad_pin(b, part, pad2);
        if (pa < 0 || pb < 0 || pa == pb) return false;
        if (strcmp(what, "short") && strcmp(what, "ok")) return false;
        /* Only legs next to each other (not across the end of the chip). */
        if (board_dip_neighbor(b, pa, -1) != pb && board_dip_neighbor(b, pa, +1) != pb) return false;
        return board_set_bridge(b, pa, pb, !strcmp(what, "short"));
    }
    pad[0] = 0;
    if (sscanf(spec, "%15[^.:].%7[^:]:%15s", ref, pad, what) != 3 &&
        sscanf(spec, "%15[^:]:%15s", ref, what) != 2) return false;
    int part = sim_find_part(&b->sim, ref);
    if (part < 0) return false;
    if (pad[0]) {       /* a pin, by its pad name: "18", or "B" on CN6 */
        int pi = board_pad_pin(b, part, pad);
        if (pi < 0) return false;
        bool out = !strcmp(what, "out0") || !strcmp(what, "out1");
        if (out && !(b->sim.pins[pi].flags & PIN_OUT)) return false;     /* not an output */
        int f = !strcmp(what, "gnd") ? PF_GND : !strcmp(what, "vcc") ? PF_VCC :
                !strcmp(what, "open") ? PF_OPEN : !strcmp(what, "out0") ? PF_OUT0 :
                !strcmp(what, "out1") ? PF_OUT1 : PF_OK;
        board_set_pin_fault(b, pi, f);
    } else if (!strcmp(what, "removed") || !strcmp(what, "present")) {
        board_set_removed(b, part, !strcmp(what, "removed"));
    } else if (part_memory(&b->sim.parts[part], NULL, NULL) && strcmp(what, "open") && strcmp(what, "short")) {
        mem_fault_t mf;
        if (!board_parse_mem_fault(b, part, what, &mf)) return false;
        board_set_mem_fault(b, part, &mf);
    } else {
        int rf = RF_OK;
        for (int k = 0; k < (int)(sizeof(rf_specs) / sizeof(rf_specs[0])); k++) if (!strcmp(what, rf_specs[k])) rf = k;
        if (!board_rfault_fits(b, part, rf)) return false;
        board_set_rfault(b, part, rf);
    }
    return true;
}

void board_clear_faults(board_t *b) {
    sim_t *s = &b->sim;
    for (int i = 0; i < s->npins; i++) sim_set_pin_fault(s, i, PF_OK);
    for (int i = 0; i < s->nparts; i++) {
        s->parts[i].removed = s->parts[i].unfitted;
        s->parts[i].rfault = RF_OK;
        s->parts[i].mfault = MF_OK;
    }
    b->nbridges = 0;
    board_rebuild(b);
}

/* ------------------------------------------------------------------------ */
/* Logic probe                                                               */
/* ------------------------------------------------------------------------ */

int board_net_state(board_t *b, int net) {
    if (net < 0) return NET_NONE;
    uint32_t hi = b->st_hi[net], lo = b->st_lo[net], fl = b->st_float[net], fi = b->st_fight[net];
    uint32_t tot = hi + lo + fl + fi;
    if (tot == 0) {     /* no samples yet: use the current state */
        const net_t *n = &b->sim.nets[b->sim.nets[net].root];
        if (n->contention) return NET_CONTENTION;
        if (n->strength == S_Z) return NET_FLOATING;
        return n->level ? NET_HIGH : NET_LOW;
    }
    if (fi * 20 > tot) return NET_CONTENTION;
    if (fl == tot) return NET_FLOATING;
    if (hi && lo) return NET_PULSING;
    if (hi) return fl ? NET_PULSING : NET_HIGH;
    if (lo) return fl ? NET_PULSING : NET_LOW;
    return NET_FLOATING;
}

void board_stats_reset(board_t *b) {
    size_t n = (size_t)b->sim.nnets * sizeof(uint32_t);
    memset(b->st_hi, 0, n); memset(b->st_lo, 0, n);
    memset(b->st_float, 0, n); memset(b->st_fight, 0, n);
    n = (size_t)b->sim.npins * sizeof(uint32_t);
    memset(b->pst_hi, 0, n); memset(b->pst_lo, 0, n);
    memset(b->pst_off, 0, n); memset(b->pst_weak, 0, n);
    memset(b->pla_hits, 0, sizeof(b->pla_hits));
    b->pla_samples = 0;
}

int board_pin_drive_state(board_t *b, int pin) {
    uint32_t hi = b->pst_hi[pin], lo = b->pst_lo[pin], off = b->pst_off[pin];
    if (hi + lo + off == 0) {   /* no samples yet: the current drive */
        drive_t d = b->sim.pins[pin].drive;
        return (d >> 1) == S_Z ? NET_FLOATING : (d & 1) ? NET_HIGH : NET_LOW;
    }
    if ((hi && lo) || ((hi || lo) && off)) return NET_PULSING;
    if (hi) return NET_HIGH;
    if (lo) return NET_LOW;
    return NET_FLOATING;
}
