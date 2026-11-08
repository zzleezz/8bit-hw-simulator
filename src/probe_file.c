/* probe_file.c - "Diagnose by probing" saved to a text file and read back
 * (see probe_db.h): the readings and the settings that rank them, then a
 * report of what they say. --probe-load FILE prints the same report
 * without a window, with the weights the file was saved with.
 *
 * AI-generated (Claude Code). */
#include "probe_db.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* "U7.3", "U7 3", "U7-3", "u7.3": a pin of the board, -1 if none. */
int probe_parse_pin(board_t *b, const char *text) {
    char ref[16], pad[8];
    if (sscanf(text, " %15[^.: -]%*[.: -]%7s", ref, pad) != 2) return -1;
    for (char *c = ref; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    int part = sim_find_part(&b->sim, ref);
    return part >= 0 ? board_pad_pin(b, part, pad) : -1;
}

/* LABEL without how the picture moves (", animated", ", changing",
 * ", blinking (16 frames)"), which a screenshot can't show. */
static void still_label(const char *label, char *out, int len) {
    static const char *const moves[] = { ", animated", ", changing", ", blinking" };
    snprintf(out, (size_t)len, "%s", label);
    for (size_t i = 0; i < sizeof(moves) / sizeof(moves[0]); i++) {
        char *p = strstr(out, moves[i]);
        if (p) *p = 0;
    }
}

void probe_db_pick_screens(probe_db_t *db, int kind, int colors, int exact) {
    bool *on = calloc((size_t)db->nscreens + 1, sizeof(bool));
    for (int s = 0; s < db->nscreens; s++) {
        const struct probe_screen *sc = &db->screens[s];
        on[s] = exact >= 0 ? s == exact : kind >= 0 && sc->kind == kind && (colors < 0 || sc->colors == colors);
    }
    probe_db_set_screens(db, on);
    free(on);
}

int probe_db_pick_shot(probe_db_t *db, const char (*labels)[256], int n, int *first) {
    bool *on = calloc((size_t)db->nscreens + 1, sizeof(bool));
    int found = 0;
    char a[256], b[256];
    for (int s = 0; s < db->nscreens; s++) {
        still_label(db->screens[s].label, a, sizeof(a));
        for (int i = 0; i < n; i++) {
            still_label(labels[i], b, sizeof(b));
            if (strcmp(a, b)) continue;
            if (!found || i < *first) *first = i;
            on[s] = true;
            found++;
            break;
        }
    }
    if (found) probe_db_set_screens(db, on);
    free(on);
    return found;
}

/* ---- reading the file --------------------------------------------------- */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

bool probe_file_read(const char *path, probe_file_t *pf, char *err, int errlen) {
    memset(pf, 0, sizeof(*pf));
    pf->strategy = pf->on_legs = -1;
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "can't open it"); return false; }
    char line[1200];
    int ln = 0;
    bool head = false;
    while (fgets(line, sizeof(line), f)) {
        ln++;
        if (!strncmp(line, "----", 4)) break;           /* the report: not read back */
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char *colon = strchr(l, ':');
        if (!colon) continue;
        *colon = 0;
        char *key = trim(l), *val = trim(colon + 1);
        if (!strcmp(key, "board")) { snprintf(pf->board, sizeof(pf->board), "%s", val); head = true; }
        else if (!strcmp(key, "strategy")) {
            for (int s = 0; s < PROBE_NSTRATEGIES; s++) if (!strcmp(val, probe_strategy_name(s))) pf->strategy = s;
        } else if (!strcmp(key, "probing-on")) pf->on_legs = !strcmp(val, "legs");
        else if (!strcmp(key, "screen-kind")) snprintf(pf->kind, sizeof(pf->kind), "%s", val);
        else if (!strcmp(key, "screen-colors")) snprintf(pf->colors, sizeof(pf->colors), "%s", val);
        else if (!strcmp(key, "screen-exact")) snprintf(pf->exact, sizeof(pf->exact), "%s", val);
        else if (!strcmp(key, "screenshot") && pf->nshot < ATLAS_MAX_GROUPS)
            snprintf(pf->shot[pf->nshot++], sizeof(pf->shot[0]), "%s", val);
        else if (!strcmp(key, "mark") && pf->nmarks < PROBE_FILE_MAX) {
            char ref[16], m[16];
            if (sscanf(val, "%15s %15s", ref, m) != 2) continue;
            int mk = !strcmp(m, "MT") ? MARK_MT : !strcmp(m, "MOS") ? MARK_MOS : !strcmp(m, "other") ? MARK_OTHER : -1;
            if (mk < 0) continue;
            snprintf(pf->marks[pf->nmarks].ref, sizeof(pf->marks[0].ref), "%s", ref);
            pf->marks[pf->nmarks++].mark = mk;
        } else if (!strcmp(key, "weight") && pf->nboost < PROBE_FILE_MAX) {
            char ref[16];
            float x;
            if (sscanf(val, "%15s %f", ref, &x) != 2) continue;
            snprintf(pf->boost[pf->nboost].ref, sizeof(pf->boost[0].ref), "%s", ref);
            pf->boost[pf->nboost++].f = x;
        } else if (!strcmp(key, "reading") && pf->n < PROBE_FILE_MAX) {
            /* PIN pad|leg [analyser] READING */
            char pin[24], place[8];
            int used = 0;
            if (sscanf(val, "%23s %7s %n", pin, place, &used) != 2) continue;
            char *rest = val + used;
            bool an = !strncmp(rest, "analyser ", 9);
            if (an) rest += 9;
            int r = -1;
            for (int k = 0; k < PROBE_NREADINGS; k++) if (!strcmp(rest, probe_reading_name(k))) r = k;
            if (r < 0) {
                snprintf(err, errlen, "line %d: \"%s\" isn't a reading (low, high, pulsing, floating, bad levels)", ln, rest);
                fclose(f);
                return false;
            }
            struct probe_file_reading *x = &pf->rd[pf->n++];
            snprintf(x->pin, sizeof(x->pin), "%s", pin);
            x->leg = !strcmp(place, "leg");
            x->analyser = an;
            x->reading = r;
        }
    }
    fclose(f);
    if (!head) { snprintf(err, errlen, "not a saved probing (no \"board:\" line)"); return false; }
    return true;
}

/* ---- the report ---------------------------------------------------------- */

/* "IC7.3 /CAS (4116)" */
static void pin_text(board_t *b, int pin, char *out, int len) {
    const sim_t *s = &b->sim;
    const part_t *pt = &s->parts[s->pins[pin].part];
    const char *nm = sim_pin_name(s, pin), *lab = board_part_label(b, s->pins[pin].part);
    char l[64] = "";
    if (lab) {
        snprintf(l, sizeof(l), " (%s)", lab);
        for (char *c = l; *c; c++) if (*c == '\n') *c = ' ';
    }
    snprintf(out, len, "%s.%s%s%s%s", pt->ref, s->bd->pads[pin].name, nm && *nm ? " " : "", nm ? nm : "",
             pt->kind == PART_IC ? l : "");
}

static void one_line_out(FILE *f, const char *pre, const char *text) {
    fputs(pre, f);
    for (const char *c = text; *c; c++) fputc(*c == '\n' ? ' ' : *c, f);
    fputc('\n', f);
}

void probe_report(FILE *f, probe_db_t *db, board_t *b, const probe_reading_t *rd, int n) {
    int *order = malloc((size_t)(db->ngroups + 1) * sizeof(int));
    int nrun = probe_rank(db, rd, n, order);
    int nbest = 0;
    float best = db->ngroups ? db->groups[order[0]].cost : 0;
    while (nbest < nrun && db->groups[order[nbest]].cost <= best + 1e-4f) nbest++;
    const probe_group_t *top = db->ngroups ? &db->groups[order[0]] : NULL;
    const probe_group_t *hg = db->healthy >= 0 ? &db->groups[db->healthy] : NULL;
    char pt[200];

    fprintf(f, "Board: %s; the atlas's readings of the %s\n", atlas_machine(db->machine)->board_name,
            atlas_machine(db->source)->board_name);
    fprintf(f, "Next pin by: %s; probing on: %s\n", probe_strategy_name(db->strategy),
            db->on_legs ? "the chips' legs" : "the solder side (pads)");
    if (db->screen_any) fprintf(f, "The screen: any picture\n");
    else {
        int k = 0;
        for (int s = 0; s < db->nscreens; s++) k += db->screen_on[s];
        fprintf(f, "The screen: %d picture%s picked:\n", k, k == 1 ? "" : "s");
        for (int s = 0, shown = 0; s < db->nscreens && shown < 12; s++)
            if (db->screen_on[s]) { fprintf(f, "  %s (%d faults)\n", db->screens[s].label, db->screens[s].nfaults); shown++; }
        if (k > 12) fprintf(f, "  ...\n");
    }

    fprintf(f, "\nReadings (%d):\n", n);
    for (int i = 0; i < n; i++) {
        const probe_reading_t *r = &rd[i];
        pin_text(b, r->pin, pt, sizeof(pt));
        const char *net = r->net >= 0 ? b->sim.nets[r->net].name : "-";
        int h = hg ? probe_reading_of(probe_group_state(hg, r)) : -1;
        int w = top ? probe_reading_of(probe_group_state(top, r)) : -1;
        bool off = top && probe_reading_cost(probe_group_state(top, r), r) >= 0.5f;
        fprintf(f, "  %-36s %s  %-10s healthy: %-10s%s%s%s   (net %s)\n", pt, r->leg ? "leg" : "pad",
                probe_reading_name(r->reading), h >= 0 ? probe_reading_name(h) : "?", off ? "  the best fit gives: " : "",
                off ? probe_reading_name(w) : "", r->analyser ? "  (from the analyser)" : "", net ? net : "?");
    }

    int ngive = 0;
    while (ngive < db->ngroups && db->groups[order[ngive]].nmatch) ngive++;
    int nf = 0;
    for (int k = 0; k < nbest; k++) nf += db->groups[order[k]].nmatch;
    fprintf(f, "\n");
    if (!n) fprintf(f, "No readings yet.\n");
    else if (top && top->healthy && top->cost < 0.01f && nbest == 1)
        fprintf(f, "Every reading is what a healthy board shows.\n");
    else if (top && top->mismatches == 0)
        fprintf(f, "%d fault%s fit the readings best (%d group%s); %d more group%s about one reading off.\n", nf,
                nf == 1 ? "" : "s", nbest, nbest == 1 ? "" : "s", nrun - nbest, nrun - nbest == 1 ? "" : "s");
    else fprintf(f, "No fault of the atlas fits every reading: the closest follow.\n");
    fprintf(f, "%d groups give the screen picked, %d in the running.\n", ngive, nrun);

    float total = 0;
    for (int i = 0; i < nrun; i++) total += db->groups[order[i]].weight;
    int ng = ngive < 15 ? ngive : 15;
    for (int k = 0; k < ng; k++) {
        const probe_group_t *g = &db->groups[order[k]];
        fprintf(f, "\n#%d  cost %.2f, %d reading%s off, %d fault%s", k + 1, g->cost, g->mismatches,
                g->mismatches == 1 ? "" : "s", g->nmatch, g->nmatch == 1 ? "" : "s");
        if (db->strategy != PROBE_FEWEST && k < nrun && total > 0)
            fprintf(f, ", likelihood %.1f%%", 100 * g->weight / total);
        fprintf(f, "%s%s\n", k < nrun ? "" : " (out of the running)", g->healthy ? " (the healthy board)" : "");
        for (int i = 0; i < n; i++) {
            const probe_reading_t *r = &rd[i];
            if (probe_reading_cost(probe_group_state(g, r), r) < 0.5f) continue;
            pin_text(b, r->pin, pt, sizeof(pt));
            fprintf(f, "    off: %s reads %s, this gives %s\n", pt, probe_reading_name(r->reading),
                    probe_reading_name(probe_reading_of(probe_group_state(g, r))));
        }
        for (int i = 0, listed = 0; i < g->nfaults && listed < 30; i++) {
            if (!probe_fault_shown(db, g->faults[i])) continue;
            listed++;
            const atlas_fault_t *af = &db->atlas.faults[g->faults[i]];
            char pre[160];
            snprintf(pre, sizeof(pre), "    %-22s ", af->spec[0] ? af->spec : "(healthy)");
            one_line_out(f, pre, af->desc ? af->desc : "");
        }
    }
    if (ngive > ng) fprintf(f, "\n(%d more groups)\n", ngive - ng);

    int pin = -1, counts[PROBE_NREADINGS], bench = 0;
    bool leg = false;
    int net = probe_suggest(db, b, rd, n, order, nrun, &pin, &leg, counts, &bench);
    if (net >= 0 && pin >= 0) {
        pin_text(b, pin, pt, sizeof(pt));
        fprintf(f, "\nProbe next: %s, %s (low %d, high %d, pulsing %d, floating %d, bad levels %d groups)%s%s\n", pt,
                leg ? "on the chip's leg" : "on its pad", counts[PROBE_LOW], counts[PROBE_HIGH], counts[PROBE_PULSING],
                counts[PROBE_FLOATING], counts[PROBE_BAD], bench ? "; bench step: " : "",
                bench ? probe_bench_step(db->machine, bench) : "");
    } else fprintf(f, "\nNo pin tells the faults left apart.\n");
    free(order);
}

/* ---- --probe-load ---------------------------------------------------------- */

int probe_load_cli(const char *atlas_path, const char *file) {
    static probe_file_t pf;
    char err[400];
    if (!probe_file_read(file, &pf, err, sizeof(err))) { fprintf(stderr, "--probe-load %s: %s\n", file, err); return 1; }
    int machine = -1;
    for (int i = 0; i < ATLAS_NMACHINES; i++) if (!strcmp(atlas_machine_id(i), pf.board)) machine = i;
    if (machine < 0) { fprintf(stderr, "--probe-load: no board \"%s\"\n", pf.board); return 1; }
    board_t *b = board_create(atlas_machine(machine));
    probe_db_t db;
    if (!probe_db_load(&db, atlas_path, machine, b, err, sizeof(err))) {
        fprintf(stderr, "--probe-load: %s\n", err);
        board_destroy(b);
        return 1;
    }
    int rc = 0;
    if (pf.strategy >= 0) db.strategy = pf.strategy;
    if (pf.on_legs >= 0) db.on_legs = pf.on_legs;
    /* The weights the file was saved with: its chip markings and repair records. */
    int np = b->sim.nparts;
    uint8_t *marks = calloc((size_t)np, 1);
    float *boost = malloc((size_t)np * sizeof(float));
    for (int i = 0; i < np; i++) boost[i] = 1;
    for (int i = 0; i < pf.nmarks; i++) {
        int p = sim_find_part(&b->sim, pf.marks[i].ref);
        if (p >= 0) marks[p] = (uint8_t)pf.marks[i].mark;
    }
    for (int i = 0; i < pf.nboost; i++) {
        int p = sim_find_part(&b->sim, pf.boost[i].ref);
        if (p >= 0) boost[p] = pf.boost[i].f;
    }
    probe_db_set_weights(&db, b, marks, boost);
    /* The screen */
    if (pf.nshot) {
        int first = 0;
        if (!probe_db_pick_shot(&db, (const char (*)[256])pf.shot, pf.nshot, &first))
            fprintf(stderr, "(the screenshot's pictures aren't in this atlas: any picture)\n");
    } else if (pf.kind[0] || pf.exact[0]) {
        int kind = -1, colors = -1, exact = -1;
        for (int k = 0; k < db.nkinds; k++) if (!strcmp(db.kinds[k].name, pf.kind)) kind = k;
        for (int c = 0; c < db.ncolorsets; c++) if (pf.colors[0] && !strcmp(db.colorsets[c].name, pf.colors)) colors = c;
        for (int s = 0; s < db.nscreens; s++) if (pf.exact[0] && !strcmp(db.screens[s].label, pf.exact)) exact = s;
        if (kind < 0 && exact < 0) fprintf(stderr, "(the screen \"%s\" isn't in this atlas: any picture)\n", pf.kind);
        else probe_db_pick_screens(&db, kind, colors, exact);
    }
    probe_reading_t rd[PROBE_FILE_MAX];
    int n = 0;
    for (int i = 0; i < pf.n; i++) {
        int pin = probe_parse_pin(b, pf.rd[i].pin);
        if (pin < 0 || b->sim.pins[pin].net < 0) { fprintf(stderr, "(no pin %s on this board: left out)\n", pf.rd[i].pin); rc = 1; continue; }
        rd[n++] = (probe_reading_t){ pin, b->sim.pins[pin].net, pf.rd[i].reading, pf.rd[i].leg, pf.rd[i].analyser };
    }
    printf("%s\n\n", file);
    probe_report(stdout, &db, b, rd, n);
    free(marks);
    free(boost);
    probe_db_free(&db);
    board_destroy(b);
    return rc;
}
