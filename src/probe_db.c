/* probe_db.c - "Diagnose by probing": the matching (see probe_db.h). */
#include "probe_db.h"
#include "c64.h"
#include "zx48.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How badly a simulated net state fits what the probe showed. A real
 * probe catches the shortest pulse, so PULSING against a steady level is a
 * near miss only when the simulation saw a net mostly at that level (we
 * don't know: half). A floating TTL input sits at ~1.4 V, which some
 * probes show as high. A fight is an in-between level, or a fast one. */
static const float cost_table[NET_NONE][PROBE_NREADINGS] = {
    /*                  low   high  pulse float */
    [NET_LOW]        = { 0.0f, 1.0f, 0.6f, 1.0f },
    [NET_HIGH]       = { 1.0f, 0.0f, 0.6f, 1.0f },
    [NET_PULSING]    = { 0.6f, 0.6f, 0.0f, 1.0f },
    [NET_FLOATING]   = { 1.0f, 0.5f, 0.6f, 0.0f },
    [NET_CONTENTION] = { 0.5f, 0.5f, 0.4f, 0.25f },
};

float probe_cost(int net_state, int reading) {
    if (net_state < 0 || net_state >= NET_NONE || reading < 0 || reading >= PROBE_NREADINGS) return 0;
    return cost_table[net_state][reading];
}

int probe_reading_of(int net_state) {
    switch (net_state) {
    case NET_LOW: return PROBE_LOW;
    case NET_HIGH: return PROBE_HIGH;
    case NET_PULSING: return PROBE_PULSING;
    case NET_FLOATING: case NET_CONTENTION: return PROBE_FLOATING;
    default: return -1;
    }
}

const char *probe_reading_name(int reading) {
    static const char *const names[PROBE_NREADINGS] = { "low", "high", "pulsing", "floating" };
    return reading >= 0 && reading < PROBE_NREADINGS ? names[reading] : "?";
}

const char *probe_strategy_name(int strategy) {
    static const char *const names[PROBE_NSTRATEGIES] = { "Bench procedure", "Likeliest faults first", "Fewest probes" };
    return strategy >= 0 && strategy < PROBE_NSTRATEGIES ? names[strategy] : "?";
}

/* ------------------------------------------------------------------------ */
/* How likely a fault is                                                     */
/* ------------------------------------------------------------------------ */

int probe_part_markable(board_t *b, int part) {
    if (part < 0 || b->sim.parts[part].kind != PART_IC) return MARK_UNKNOWN;
    switch (board_part_role(b, part)) {
    case ROLE_PLA: case ROLE_GLUE: case ROLE_CLOCK: return MARK_MOS;
    case ROLE_RAM: case ROLE_VRAM: return MARK_MT;
    default: return MARK_UNKNOWN;
    }
}

/* The bench ranking: PLA, CIAs, CPU, RAM, ROMs; then the video and sound
 * chips, the logic. MT RAM comes up to the PLA's level, MOS logic to the
 * CIAs', and a PLA another maker made (the early ones) is very reliable.
 * The Spectrum's ULA counts as its PLA. */
float probe_part_weight(board_t *b, int part, int mark) {
    if (part < 0 || b->sim.parts[part].kind != PART_IC) return 0.5f;
    const char *label = board_part_label(b, part);
    switch (board_part_role(b, part)) {
    case ROLE_PLA: return mark == MARK_MOS ? 12 : mark == MARK_OTHER ? 2.5f : 10;
    case ROLE_RAM: case ROLE_VRAM: return mark == MARK_MT ? 9 : 3.5f;
    case ROLE_ROM: return 3;
    case ROLE_CHIP:
        if (label && !strncmp(label, "CIA", 3)) return 7;
        if (label && !strcmp(label, "ULA")) return 8;
        if (label && !strcmp(label, "CPU")) return 5;
        return 2.5f;
    case ROLE_GLUE: case ROLE_CLOCK: return mark == MARK_MOS ? 6 : 1;
    default: return 1;
    }
}

/* What the repair records say of each part (a factor, per part), while
 * probe_db_set_weights() runs. */
static const float *part_boost;

static float marked_weight(board_t *b, int part, const uint8_t *marks) {
    return probe_part_weight(b, part, marks && part >= 0 ? marks[part] : MARK_UNKNOWN) *
           (part_boost && part >= 0 ? part_boost[part] : 1);
}

/* A net stuck low or high: a chip driving it, dead inside or shorted
 * there (the likeliest of them); with no chip driving it, half the
 * likeliest chip it goes into. */
static float net_weight(board_t *b, int pin, const uint8_t *marks) {
    sim_t *s = &b->sim;
    if (pin < 0 || s->pins[pin].net < 0) return 0.5f;
    const net_t *n = &s->nets[s->nets[s->pins[pin].net].root];
    float drv = 0, any = 0;
    for (int k = 0; k < n->npins; k++) {
        int q = n->pins[k], part = s->pins[q].part;
        if (s->parts[part].kind != PART_IC || !s->pins[q].number) continue;
        float w = marked_weight(b, part, marks);
        if (s->pins[q].flags & PIN_OUT) drv = fmaxf(drv, w);
        any = fmaxf(any, w);
    }
    return drv > 0 ? drv : any > 0 ? 0.5f * any : 0.5f;
}

/* One fault spec (as board_apply_fault_spec() takes it): a chip dead, an
 * output or memory fault inside it count fully; a leg open (the solder,
 * the socket) and a solder bridge much less. */
static float spec_weight(board_t *b, const char *spec, const uint8_t *marks) {
    sim_t *s = &b->sim;
    char ref[16] = { 0 }, pad[8] = { 0 }, pad2[8] = { 0 }, what[24] = { 0 };
    if (sscanf(spec, "%15[^.:].%7[^-:]-%7[^:]:%23s", ref, pad, pad2, what) == 4)
        return 0.2f * marked_weight(b, sim_find_part(s, ref), marks);
    if (sscanf(spec, "%15[^.:].%7[^:]:%23s", ref, pad, what) == 3) {
        int part = sim_find_part(s, ref), pin = part >= 0 ? board_pad_pin(b, part, pad) : -1;
        if (!strcmp(what, "gnd") || !strcmp(what, "vcc")) return net_weight(b, pin, marks);
        if (!strcmp(what, "open")) return part >= 0 && s->parts[part].kind == PART_IC ? 0.3f * marked_weight(b, part, marks) : 0.5f;
        return marked_weight(b, part, marks);
    }
    if (sscanf(spec, "%15[^:]:%23s", ref, what) == 2) return marked_weight(b, sim_find_part(s, ref), marks);
    return 0.5f;
}

static float fault_weight(board_t *b, const char *spec, const uint8_t *marks) {
    float w = 0;
    for (const char *s = spec; *s; ) {
        size_t len = strcspn(s, " ");
        char one[64];
        snprintf(one, sizeof(one), "%.*s", (int)len, s);
        if (len) w = fmaxf(w, spec_weight(b, one, marks));
        s += len + (s[len] == ' ');
    }
    return w > 0 ? w : 1;       /* the healthy board */
}

static uint64_t fnv64(const uint8_t *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

/* The KIND of picture a screen label is of, and its COLORS ("" if the
 * label doesn't say): "Vertical bars: 32 of them, ... (white border, black
 * screen), animated" is "Vertical bars" in "white border, black screen". */
void probe_screen_kind(const char *label, char *kind, int klen, char *colors, int clen) {
    static const struct { const char *prefix, *kind; } kinds[] = {
        { "No video signal", "No picture (no video signal)" },
        { "Solid ", "Blank or solid color, nothing drawn" },
        { "Blank ", "Blank or solid color, nothing drawn" },
        { "BASIC READY", "Boot text (it booted, or partly)" },
        { "BASIC banner", "Boot text (it booted, or partly)" },
        { "READY prompt", "Boot text (it booted, or partly)" },
        { "Sinclair copyright", "Boot text (it booted, or partly)" },
        { "Garbage on", "Garbage in some cells" },
        { "Attribute garbage", "Random color blocks (attribute garbage)" },
    };
    colors[0] = 0;
    snprintf(kind, (size_t)klen, "%.*s", (int)strcspn(label, ":(,"), label);
    char *end = kind + strlen(kind);
    while (end > kind && end[-1] == ' ') *--end = 0;
    char *over = strstr(kind, " over ");                        /* "Garbled character cells over 33%" */
    if (over) *over = 0;
    char *on = strstr(kind, " on ");                            /* "Unreadable characters on 1% of the cells" */
    if (on) *on = 0;
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (!strncmp(label, kinds[i].prefix, strlen(kinds[i].prefix))) snprintf(kind, (size_t)klen, "%s", kinds[i].kind);
    /* "(X border, Y screen)" */
    for (const char *p = strchr(label, '('); p; p = strchr(p + 1, '(')) {
        int n = (int)strcspn(p + 1, ")");
        char in[64];
        snprintf(in, sizeof(in), "%.*s", n, p + 1);
        if (strstr(in, " border, ") && strstr(in, " screen")) { snprintf(colors, (size_t)clen, "%s", in); return; }
    }
    /* "Blank black screen, white border": the same, the other way round;
     * "Solid black screen": all one color. */
    if (!strncmp(label, "Blank ", 6)) {
        const char *scr = label + 6, *comma = strchr(scr, ',');
        if (comma) {
            int sl = (int)(comma - scr), bl = (int)strcspn(comma + 2, ",");
            snprintf(colors, (size_t)clen, "%.*s, %.*s", bl, comma + 2, sl, scr);
        }
    } else if (!strncmp(label, "Solid ", 6)) {
        int n = (int)strcspn(label + 6, ",");
        snprintf(colors, (size_t)clen, "%.*s", n, label + 6);
        char *s = strstr(colors, " screen");
        if (s) snprintf(s, (size_t)(clen - (s - colors)), " all over");
    }
}

/* NAME's index in NAMES (added, with no faults, if new), its faults
 * counted. */
static int name_index(struct probe_kind *names, int *n, const char *name, int nfaults) {
    int i = 0;
    while (i < *n && strcmp(names[i].name, name)) i++;
    if (i == *n) {
        snprintf(names[i].name, sizeof(names[i].name), "%s", name);
        names[i].nfaults = 0;
        (*n)++;
    }
    names[i].nfaults += nfaults;
    return i;
}

/* Sort NAMES most faults first, the screens' indices into them with them
 * (KIND: the screens' kinds, else their colors). */
static void sort_names(probe_db_t *db, struct probe_kind *names, int n, bool kind) {
    int *rank = malloc(((size_t)n + 1) * sizeof(int)), *where = malloc(((size_t)n + 1) * sizeof(int));
    for (int i = 0; i < n; i++) rank[i] = i;
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0 && names[rank[k]].nfaults > names[rank[k - 1]].nfaults; k--) {
            int t = rank[k]; rank[k] = rank[k - 1]; rank[k - 1] = t;
        }
    struct probe_kind *sorted = malloc(((size_t)n + 1) * sizeof(*sorted));
    for (int i = 0; i < n; i++) { sorted[i] = names[rank[i]]; where[rank[i]] = i; }
    memcpy(names, sorted, (size_t)n * sizeof(*sorted));
    for (int s = 0; s < db->nscreens; s++) {
        int *f = kind ? &db->screens[s].kind : &db->screens[s].colors;
        *f = where[*f];
    }
    free(sorted);
    free(rank);
    free(where);
}

bool probe_db_load(probe_db_t *db, const char *path, int machine, board_t *b, char *err, int errlen) {
    atlas_init_shared();     /* the palettes, for the screen labels */
    memset(db, 0, sizeof(*db));
    db->machine = machine;
    db->healthy = -1;
    db->nnets = b->sim.nnets;
    bool exists;
    if (!atlas_load(&db->atlas, path, &exists) || !exists) {
        snprintf(err, (size_t)errlen, exists ? "%s is not a fault atlas" : "%s is missing (keep it next to the program)",
                 path);
        atlas_free(&db->atlas);
        return false;
    }
    atlas_t *a = &db->atlas;
    /* A board the atlas has no readings of yet: those of the family's
     * first board, if it has some and the same references (the Spectrum
     * issues). The C64's two boards don't: the 250407's faults ("U7.24")
     * would name other parts on the 250469 (its U7 is the VIC). */
    int src = machine;
    bool any = false;
    for (int i = 0; i < a->nfaults && !any; i++) any = a->faults[i].h.machine == machine && a->faults[i].nets;
    if (!any && atlas_family(machine) != machine && atlas_family(machine) == ATLAS_ZX48) src = atlas_family(machine);
    if (!any && src == machine) {
        snprintf(err, (size_t)errlen, "The atlas has no logic probe readings of the %s yet, and those of the "
                 "other C64 board name other parts: build the atlas for it (--build-atlas).", atlas_machine(machine)->board_name);
        atlas_free(&db->atlas);
        return false;
    }
    db->source = src;
    /* The atlas's nets -> the board's, by name. */
    int an = a->nnet_names[src];
    int *map = malloc(((size_t)an + 1) * sizeof(int));
    for (int i = 0; i < an; i++) {
        const char *name = a->net_names[src][i];
        map[i] = -1;
        if (i < db->nnets && b->sim.nets[i].name && !strcmp(b->sim.nets[i].name, name)) { map[i] = i; continue; }
        for (int k = 0; k < db->nnets; k++)
            if (b->sim.nets[k].name && !strcmp(b->sim.nets[k].name, name)) { map[i] = k; break; }
    }
    /* The faults, grouped by what they show on every net. */
    int cap = 0, hcap = 1024;
    while (hcap < 4 * a->nfaults) hcap *= 2;
    struct { uint64_t h; int g; } *hash = calloc((size_t)hcap, sizeof(*hash));
    uint8_t *v = malloc((size_t)db->nnets);
    for (int i = 0; i < a->nfaults; i++) {
        const atlas_fault_t *f = &a->faults[i];
        if (f->h.machine != src || f->h.mode != MODE_BOOT || !f->nets || f->nnets != an) continue;
        memset(v, NET_NONE, (size_t)db->nnets);
        for (int k = 0; k < an; k++) if (map[k] >= 0) v[map[k]] = f->nets[k];
        uint64_t h = fnv64(v, (size_t)db->nnets) | 1;
        int at = (int)(h & (uint64_t)(hcap - 1)), g = -1;
        while (hash[at].h) {
            if (hash[at].h == h && !memcmp(db->groups[hash[at].g].nets, v, (size_t)db->nnets)) { g = hash[at].g; break; }
            at = (at + 1) & (hcap - 1);
        }
        if (g < 0) {
            if (db->ngroups == cap) db->groups = realloc(db->groups, (size_t)(cap = cap * 2 + 64) * sizeof(probe_group_t));
            g = db->ngroups++;
            probe_group_t *ng = &db->groups[g];
            memset(ng, 0, sizeof(*ng));
            ng->nets = malloc((size_t)db->nnets);
            memcpy(ng->nets, v, (size_t)db->nnets);
            hash[at].h = h;
            hash[at].g = g;
        }
        probe_group_t *gr = &db->groups[g];
        gr->faults = realloc(gr->faults, (size_t)(gr->nfaults + 1) * sizeof(int));
        /* The healthy board first in its group, then as the atlas has them. */
        if (!f->spec[0]) {
            memmove(gr->faults + 1, gr->faults, (size_t)gr->nfaults * sizeof(int));
            gr->faults[0] = i;
            gr->healthy = true;
            db->healthy = g;
        } else gr->faults[gr->nfaults] = i;
        gr->nfaults++;
        db->nfaults++;
    }
    free(v);
    free(hash);
    free(map);
    if (!db->nfaults) {
        snprintf(err, (size_t)errlen,
                 "%s has no logic probe readings for this machine: build it again (--build-atlas=full)", path);
        probe_db_free(db);
        return false;
    }
    /* What the screen shows with each fault. */
    db->screen = -1;
    db->fault_screen = malloc(((size_t)a->nfaults + 1) * sizeof(int));
    int *of_outcome = malloc(((size_t)a->noutcomes + 1) * sizeof(int)), scap = 0;
    for (int o = 0; o < a->noutcomes; o++) of_outcome[o] = -2;     /* not labeled yet */
    for (int i = 0; i < a->nfaults; i++) db->fault_screen[i] = -1;
    for (int g = 0; g < db->ngroups; g++)
        for (int k = 0; k < db->groups[g].nfaults; k++) {
            int fi = db->groups[g].faults[k], o = a->faults[fi].outcome;
            if (o < 0) continue;
            if (of_outcome[o] == -2) {
                char label[256];
                atlas_outcome_label(&a->outcomes[o], label, sizeof(label));
                int s = 0;
                while (s < db->nscreens && strcmp(db->screens[s].label, label)) s++;
                if (s == db->nscreens) {
                    if (s == scap) db->screens = realloc(db->screens, (size_t)(scap = scap * 2 + 32) * sizeof(*db->screens));
                    snprintf(db->screens[s].label, sizeof(db->screens[s].label), "%s", label);
                    db->screens[s].nfaults = 0;
                    db->nscreens++;
                }
                of_outcome[o] = s;
            }
            db->fault_screen[fi] = of_outcome[o];
            db->screens[of_outcome[o]].nfaults++;
        }
    free(of_outcome);
    /* Most faults first. */
    int *rank = malloc(((size_t)db->nscreens + 1) * sizeof(int)), *where = malloc(((size_t)db->nscreens + 1) * sizeof(int));
    for (int s = 0; s < db->nscreens; s++) rank[s] = s;
    for (int i = 1; i < db->nscreens; i++)       /* insertion sort: stable, and only a few hundred */
        for (int k = i; k > 0 && db->screens[rank[k]].nfaults > db->screens[rank[k - 1]].nfaults; k--) {
            int t = rank[k]; rank[k] = rank[k - 1]; rank[k - 1] = t;
        }
    struct probe_screen *sorted = malloc(((size_t)db->nscreens + 1) * sizeof(*sorted));
    for (int s = 0; s < db->nscreens; s++) { sorted[s] = db->screens[rank[s]]; where[rank[s]] = s; }
    for (int i = 0; i < a->nfaults; i++) if (db->fault_screen[i] >= 0) db->fault_screen[i] = where[db->fault_screen[i]];
    free(db->screens);
    db->screens = sorted;
    free(rank);
    free(where);
    /* Their kinds and colors. */
    db->kinds = calloc((size_t)db->nscreens + 1, sizeof(*db->kinds));
    db->colorsets = calloc((size_t)db->nscreens + 1, sizeof(*db->colorsets));
    for (int s = 0; s < db->nscreens; s++) {
        char kind[64], colors[64];
        probe_screen_kind(db->screens[s].label, kind, sizeof(kind), colors, sizeof(colors));
        db->screens[s].kind = name_index(db->kinds, &db->nkinds, kind, db->screens[s].nfaults);
        db->screens[s].colors = name_index(db->colorsets, &db->ncolorsets, colors, db->screens[s].nfaults);
    }
    sort_names(db, db->kinds, db->nkinds, true);
    sort_names(db, db->colorsets, db->ncolorsets, false);
    db->screen_on = calloc((size_t)db->nscreens + 1, sizeof(bool));
    db->strategy = PROBE_BENCH;
    db->fault_weight = malloc(((size_t)a->nfaults + 1) * sizeof(float));
    probe_db_set_weights(db, b, NULL, NULL);
    probe_db_set_screen(db, -1);
    return true;
}

/* The faults that give the pictures picked, and their weight, per group. */
static void count_shown(probe_db_t *db) {
    for (int g = 0; g < db->ngroups; g++) {
        probe_group_t *gr = &db->groups[g];
        gr->nmatch = 0;
        gr->weight = 0;
        for (int k = 0; k < gr->nfaults; k++)
            if (probe_fault_shown(db, gr->faults[k])) {
                gr->nmatch++;
                gr->weight += db->fault_weight[gr->faults[k]];
            }
    }
}

void probe_db_set_screen(probe_db_t *db, int s) {
    bool one = s >= 0 && s < db->nscreens;
    for (int i = 0; i < db->nscreens; i++) db->screen_on[i] = one && i == s;
    db->screen_any = !one;
    db->screen = one ? s : -1;
    count_shown(db);
}

void probe_db_set_screens(probe_db_t *db, const bool *on) {
    int n = 0, last = -1;
    for (int i = 0; i < db->nscreens; i++) {
        db->screen_on[i] = on && on[i];
        if (db->screen_on[i]) { n++; last = i; }
    }
    db->screen_any = !n;
    db->screen = n == 1 ? last : -1;
    count_shown(db);
}

static const probe_db_t *sort_db;

/* The healthy board first, then the likeliest, then as the atlas has them. */
static int cmp_cause(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    bool hx = !sort_db->atlas.faults[x].spec[0], hy = !sort_db->atlas.faults[y].spec[0];
    if (hx != hy) return hx ? -1 : 1;
    float wx = sort_db->fault_weight[x], wy = sort_db->fault_weight[y];
    if (wx != wy) return wx > wy ? -1 : 1;
    return x - y;
}

void probe_db_set_weights(probe_db_t *db, board_t *b, const uint8_t *marks, const float *boost) {
    part_boost = boost;
    for (int i = 0; i < db->atlas.nfaults; i++) db->fault_weight[i] = fault_weight(b, db->atlas.faults[i].spec, marks);
    part_boost = NULL;
    sort_db = db;
    for (int g = 0; g < db->ngroups; g++)
        qsort(db->groups[g].faults, (size_t)db->groups[g].nfaults, sizeof(int), cmp_cause);
    count_shown(db);
}

bool probe_fault_shown(const probe_db_t *db, int fault) {
    return db->screen_any || (db->fault_screen[fault] >= 0 && db->screen_on[db->fault_screen[fault]]);
}

void probe_db_free(probe_db_t *db) {
    for (int g = 0; g < db->ngroups; g++) { free(db->groups[g].faults); free(db->groups[g].nets); }
    free(db->screens);
    free(db->kinds);
    free(db->colorsets);
    free(db->screen_on);
    free(db->fault_screen);
    free(db->fault_weight);
    free(db->groups);
    atlas_free(&db->atlas);
    memset(db, 0, sizeof(*db));
    db->healthy = -1;
}

/* PROBE_TOLERANCE, or C64HW_PROBE_TOL to try others (--probe-test). */
static float probe_tolerance(void) {
    static float tol = -1;
    if (tol < 0) {
        const char *e = getenv("C64HW_PROBE_TOL");
        tol = e ? (float)atof(e) : PROBE_TOLERANCE;
    }
    return tol;
}

static int cmp_rank(const void *a, const void *b) {
    const probe_group_t *x = &sort_db->groups[*(const int *)a], *y = &sort_db->groups[*(const int *)b];
    if (x->cost != y->cost) return x->cost < y->cost ? -1 : 1;
    if (x->healthy != y->healthy) return x->healthy ? -1 : 1;     /* a healthy board is the likeliest */
    if (sort_db->strategy != PROBE_FEWEST && x->weight != y->weight) return x->weight > y->weight ? -1 : 1;
    if (x->nmatch != y->nmatch) return y->nmatch - x->nmatch;
    return *(const int *)a - *(const int *)b;
}

int probe_rank(probe_db_t *db, const probe_reading_t *readings, int n, int *order) {
    for (int g = 0; g < db->ngroups; g++) {
        probe_group_t *gr = &db->groups[g];
        gr->cost = 0;
        gr->mismatches = 0;
        for (int i = 0; i < n; i++) {
            int net = readings[i].net;
            if (net < 0 || net >= db->nnets) continue;
            float c = probe_cost(gr->nets[net], readings[i].reading);
            gr->cost += c;
            gr->mismatches += c >= 0.5f;
        }
        if (!gr->nmatch) gr->cost += 1e6f;     /* not the screen picked */
        order[g] = g;
    }
    sort_db = db;
    qsort(order, (size_t)db->ngroups, sizeof(int), cmp_rank);
    int running = 0;
    if (db->ngroups) {
        float best = db->groups[order[0]].cost;
        while (running < db->ngroups && db->groups[order[running]].cost <= best + probe_tolerance() + 1e-4f) running++;
    }
    return running;
}

int probe_pin_for_net(board_t *b, int net) {
    sim_t *s = &b->sim;
    int best = -1, best_size = 1 << 30;
    for (int i = 0; i < s->npins; i++) {
        if (s->pins[i].net != net) continue;
        const part_t *p = &s->parts[s->pins[i].part];
        /* An IC's leg is easy to find; a connector's next best. */
        int size = p->kind == PART_IC ? p->npins : p->kind == PART_CONNECTOR ? 1000 + p->npins : -1;
        if (size < 0 || p->removed) continue;
        if (size < best_size) { best = i; best_size = size; }
    }
    return best;
}

/* The bench procedure: what a repair checks first on a dead machine. A
 * family's steps (ATLAS_C64, ATLAS_ZX48) are for every board of it that
 * has none of its own (the 250469 has). */
static const struct { int machine; const char *pin, *what; } bench_pins[] = {
    { ATLAS_C64C, "U6.40", "reset at the CPU" },
    { ATLAS_C64C, "U6.1", "the clock (PHI0) into the CPU" },
    { ATLAS_C64C, "U8.22", "Super PLA output /CASRAM (the RAM)" },
    { ATLAS_C64C, "U8.15", "Super PLA output /KERNAL" },
    { ATLAS_C64C, "U8.14", "Super PLA output /BASIC" },
    { ATLAS_C64C, "U8.13", "Super PLA output /CHARROM" },
    { ATLAS_C64C, "U8.40", "Super PLA output /VIC" },
    { ATLAS_C64C, "U8.17", "Super PLA output /COLRAM (the color RAM)" },
    { ATLAS_C64, "U7.40", "reset at the CPU" },
    { ATLAS_C64, "U7.1", "the clock (PHI0) into the CPU" },
    { ATLAS_C64, "U17.18", "PLA output /CASRAM (the RAM)" },
    { ATLAS_C64, "U17.16", "PLA output /KERNAL" },
    { ATLAS_C64, "U17.17", "PLA output /BASIC" },
    { ATLAS_C64, "U17.15", "PLA output /CHAROM" },
    { ATLAS_C64, "U17.12", "PLA output /IO" },
    { ATLAS_C64, "U17.13", "PLA output GR/W (the color RAM)" },
    { ATLAS_ZX48, "IC2.26", "reset at the CPU" },
    { ATLAS_ZX48, "IC2.6", "the clock into the CPU" },
    { ATLAS_ZX48, "IC5.20", "the ROM select" },
    { ATLAS_ZX48, "IC2.19", "/MREQ at the CPU" },
};

/* The board whose steps a board follows: its own, or its family's. */
static int bench_machine_of(int machine) {
    for (size_t i = 0; i < sizeof(bench_pins) / sizeof(bench_pins[0]); i++)
        if (bench_pins[i].machine == machine) return machine;
    return atlas_family(machine);
}

const char *probe_bench_step(int machine, int k) {
    int bm = bench_machine_of(machine);
    for (size_t i = 0; i < sizeof(bench_pins) / sizeof(bench_pins[0]); i++)
        if (bench_pins[i].machine == bm && --k == 0) return bench_pins[i].what;
    return NULL;
}

/* What the groups in the running would show on NET: COUNTS per reading,
 * and the information of a reading there (bits, over the groups' weights
 * unless the strategy is PROBE_FEWEST). */
static double net_info(const probe_db_t *db, int net, const int *order, int nrunning, int counts[PROBE_NREADINGS]) {
    double w[PROBE_NREADINGS] = { 0 }, known = 0, total = 0;
    memset(counts, 0, sizeof(int) * PROBE_NREADINGS);
    for (int k = 0; k < nrunning; k++) {
        const probe_group_t *g = &db->groups[order[k]];
        double gw = db->strategy == PROBE_FEWEST ? 1 : fmax(g->weight, 1e-3);
        total += gw;
        int r = probe_reading_of(g->nets[net]);
        if (r >= 0) { counts[r]++; w[r] += gw; known += gw; }
    }
    int nknown = 0;
    for (int r = 0; r < PROBE_NREADINGS; r++) nknown += counts[r];
    if (nknown < 2) return 0;
    /* Information (entropy, bits) of the reading over the groups. */
    double info = 0;
    for (int r = 0; r < PROBE_NREADINGS; r++)
        if (w[r] > 0) { double p = w[r] / known; info -= p * log2(p); }
    /* Nets some groups know nothing about count for less. */
    return info * known / total;
}

int probe_suggest(const probe_db_t *db, board_t *b, const probe_reading_t *readings, int n, const int *order,
                  int nrunning, int *pin, int counts[PROBE_NREADINGS], int *bench) {
    *pin = -1;
    if (bench) *bench = 0;
    memset(counts, 0, sizeof(int) * PROBE_NREADINGS);
    if (nrunning < 2) return -1;
    /* The bench procedure first: its pins not probed yet that tell some
     * of the suspects apart. */
    int bm = bench_machine_of(db->machine);
    for (size_t i = 0, step = 0; db->strategy == PROBE_BENCH && i < sizeof(bench_pins) / sizeof(bench_pins[0]); i++) {
        if (bench_pins[i].machine != bm) continue;
        step++;
        char ref[16], pad[8];
        if (sscanf(bench_pins[i].pin, "%15[^.].%7s", ref, pad) != 2) continue;
        int part = sim_find_part(&b->sim, ref), p = part >= 0 ? board_pad_pin(b, part, pad) : -1;
        int net = p >= 0 ? b->sim.pins[p].net : -1;
        if (net < 0 || net >= db->nnets) continue;
        bool done = false;
        for (int k = 0; k < n && !done; k++) done = readings[k].net == net;
        if (done || net_info(db, net, order, nrunning, counts) < 1e-6) continue;
        *pin = p;
        if (bench) *bench = (int)step;
        return net;
    }
    memset(counts, 0, sizeof(int) * PROBE_NREADINGS);
    double best_info = 1e-6;
    int best_net = -1, best_pin = -1, best_size = 1 << 30;
    for (int net = 0; net < db->nnets; net++) {
        bool done = false;
        for (int i = 0; i < n && !done; i++) done = readings[i].net == net;
        if (done) continue;
        int c[PROBE_NREADINGS];
        double info = net_info(db, net, order, nrunning, c);
        if (info < best_info - 1e-9) continue;
        int p = probe_pin_for_net(b, net);
        if (p < 0) continue;
        int size = b->sim.parts[b->sim.pins[p].part].npins;
        if (info > best_info + 1e-9 || size < best_size) {
            best_info = info;
            best_net = net;
            best_pin = p;
            best_size = size;
            memcpy(counts, c, sizeof(c));
        }
    }
    *pin = best_pin;
    return best_net;
}

/* ------------------------------------------------------------------------ */
/* The self test                                                             */
/* ------------------------------------------------------------------------ */

static uint32_t rng_next(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* Run a fault the way the atlas does, but probed later (frames 400-450,
 * 8-9 s, where the atlas has 5-6 s), the way a person might. */
static board_t *run_broken(int machine, const char *spec) {
    board_t *b = board_create(atlas_machine(machine));
    for (const char *s = spec; *s; ) {
        size_t len = strcspn(s, " ");
        char one[64];
        snprintf(one, sizeof(one), "%.*s", (int)len, s);
        if (len && !board_apply_fault_spec(b, one)) { board_destroy(b); return NULL; }
        s += len + (s[len] == ' ');
    }
    for (int f = 0; f < 450; f++) {
        board_run(b, b->m->cycles_per_frame);
        board_frame_update(b, 20000);
        if (f == 399) board_stats_reset(b);
    }
    return b;
}

typedef struct { int total, top1, first, named, running, top6, probes, left; } test_stats_t;

/* Probe board B (broken with atlas fault FI, of group G) where the tool
 * says until it can't tell the groups in the running apart. RNG makes the
 * wrong readings. A line of the result goes in LINE. */
static void diagnose_one(probe_db_t *db, board_t *ref, board_t *b, int *order, int fi, int g, int noise_pct,
                         uint32_t rng, test_stats_t *st, char *line, int len) {
    probe_reading_t rd[32];
    int n = 0, nrun = probe_rank(db, rd, 0, order);
    while (n < 25) {
        int pin, counts[PROBE_NREADINGS];
        int net = probe_suggest(db, ref, rd, n, order, nrun, &pin, counts, NULL);
        if (net < 0) break;
        int r = probe_reading_of(board_net_state(b, net));
        if (r < 0) r = PROBE_FLOATING;
        if ((int)(rng_next(&rng) % 100) < noise_pct)
            r = (r + 1 + (int)(rng_next(&rng) % (PROBE_NREADINGS - 1))) % PROBE_NREADINGS;
        rd[n++] = (probe_reading_t){ pin, net, r };
        nrun = probe_rank(db, rd, n, order);
    }
    int pos = 0;
    while (pos < db->ngroups && order[pos] != g) pos++;
    /* The faults left: those fitting best (what the user is shown first). */
    int nleft = 0;
    float best = db->groups[order[0]].cost;
    for (int k = 0; k < nrun && db->groups[order[k]].cost <= best + 1e-4f; k++) nleft += db->groups[order[k]].nmatch;
    /* The first cause listed: the true fault itself? */
    int named = -1;
    for (int k = 0; k < db->groups[order[0]].nfaults && named < 0; k++)
        if (probe_fault_shown(db, db->groups[order[0]].faults[k])) named = db->groups[order[0]].faults[k];
    st->total++;
    st->top1 += db->groups[g].cost <= best + 1e-4f;
    st->first += pos == 0;
    st->named += named == fi;
    st->running += pos < nrun;
    st->top6 += pos < 6;
    st->probes += n;
    st->left += nleft;
    snprintf(line, (size_t)len, "%2d probes #%-3d%s", n, pos + 1, pos >= nrun ? "MISSED" : named == fi ? "named " : "      ");
}

static void print_stats(const char *what, const test_stats_t *s) {
    if (!s->total) return;
    printf("%-36s true fault listed #1 %3d%%, named first %3d%%, among the best fits %3d%%,\n"
           "%-36s in the first 6 %3d%%, in the running %3d%%, %4.1f probes, %5.1f faults fit best\n", what,
           100 * s->first / s->total, 100 * s->named / s->total, 100 * s->top1 / s->total, "", 100 * s->top6 / s->total,
           100 * s->running / s->total, (double)s->probes / s->total, (double)s->left / s->total);
}

int probe_test(const char *atlas_path, int count, unsigned seed, int noise_pct) {
    test_stats_t st[PROBE_NSTRATEGIES][2];      /* blind, screen known */
    memset(st, 0, sizeof(st));
    int skipped = 0;
    uint32_t rng = seed * 2654435761u + 1;
    for (int machine = 0; machine < ATLAS_NMACHINES; machine++) {
        board_t *ref = board_create(atlas_machine(machine));
        probe_db_t db;
        char err[300];
        if (!probe_db_load(&db, atlas_path, machine, ref, err, sizeof(err))) {
            printf("%s: %s\n", atlas_machine_id(machine), err);
            board_destroy(ref);
            continue;
        }
        if (db.source != machine) {     /* the test is of the atlas's own readings */
            printf("%s: no probe readings of its own in the atlas yet: skipped\n", atlas_machine_id(machine));
            probe_db_free(&db);
            board_destroy(ref);
            continue;
        }
        /* The faults that show nothing (the healthy screen and healthy
         * readings) aren't what anybody brings to the bench: not picked. */
        int *cand = malloc((size_t)db.nfaults * 2 * sizeof(int)), ncand = 0;
        double total_w = 0;
        int hs = db.healthy >= 0 ? db.fault_screen[db.groups[db.healthy].faults[0]] : -1;
        for (int g = 0; g < db.ngroups; g++)
            for (int k = 0; k < db.groups[g].nfaults; k++) {
                int fi = db.groups[g].faults[k];
                if (g == db.healthy && db.fault_screen[fi] == hs) continue;
                cand[2 * ncand] = fi;
                cand[2 * ncand++ + 1] = g;
                total_w += db.fault_weight[fi];
            }
        printf("== %s: %d faults with readings, %d groups a probe can tell apart, %d different screens;\n"
               "   %d show nothing (healthy screen and readings), not picked\n"
               "   %-22s %-26s %-26s %-26s  (blind, then screen known)\n", atlas_machine_id(machine), db.nfaults,
               db.ngroups, db.nscreens, db.nfaults - ncand, "", probe_strategy_name(PROBE_BENCH),
               probe_strategy_name(PROBE_LIKELY), probe_strategy_name(PROBE_FEWEST));
        int *order = malloc((size_t)db.ngroups * sizeof(int));
        for (int t = 0; t < count && ncand; t++) {
            /* A fault at random, as likely as it comes to the bench. */
            double at = (double)rng_next(&rng) / (double)(1u << 24) * total_w;
            int pick = 0;
            while (pick < ncand - 1 && (at -= db.fault_weight[cand[2 * pick]]) > 0) pick++;
            int fi = cand[2 * pick], g = cand[2 * pick + 1];
            board_t *b = run_broken(machine, db.atlas.faults[fi].spec);
            if (!b) { skipped++; continue; }
            uint32_t noise = rng_next(&rng);
            char lines[2][PROBE_NSTRATEGIES][40];
            for (int known = 0; known < 2; known++)
                for (int s = 0; s < PROBE_NSTRATEGIES; s++) {
                    db.strategy = s;
                    probe_db_set_screen(&db, known ? db.fault_screen[fi] : -1);
                    diagnose_one(&db, ref, b, order, fi, g, noise_pct, noise, &st[s][known], lines[known][s], 40);
                }
            const char *spec = db.atlas.faults[fi].spec;
            for (int known = 0; known < 2; known++)
                printf("   %-22s %-26s %-26s %-26s\n", known ? "" : spec[0] ? spec : "(healthy)", lines[known][0],
                       lines[known][1], lines[known][2]);
            board_destroy(b);
        }
        free(order);
        free(cand);
        probe_db_free(&db);
        board_destroy(ref);
    }
    if (!st[0][0].total) return 1;
    printf("\n%d faults (picked by bench likelihood), %d%% of the readings wrong%s:\n", st[0][0].total, noise_pct,
           skipped ? " (some skipped: their spec didn't apply)" : "");
    for (int known = 0; known < 2; known++)
        for (int s = 0; s < PROBE_NSTRATEGIES; s++) {
            char what[64];
            snprintf(what, sizeof(what), "%s, %s:", probe_strategy_name(s), known ? "screen known" : "probing only");
            print_stats(what, &st[s][known]);
        }
    return 0;
}
