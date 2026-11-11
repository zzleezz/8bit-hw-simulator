/* probe.c - "Diagnose by probing" (see probe.h, and probe_db.h for the
 * matching). The atlas is read when the window first opens for a machine
 * (a fraction of a second) and kept; the readings belong to the machine on
 * the bench and go when it changes. */
#include "probe.h"
#include "probe_db.h"
#include "atlas.h"
#include "diagnose.h"
#include "filedialog.h"
#include "lacap.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_READINGS 64
#define SHOWN_GROUPS 6          /* groups listed before "show more" */
#define SHOWN_CAUSES 3          /* causes of a group before "show all" */

static struct {
    bool open, reopen;
    board_t *b;                 /* the board the readings are of */
    int machine;
    bool loaded;                /* db holds the machine's faults (else err says why) */
    probe_db_t db;
    char err[400];
    bool *expanded;             /* per group: every cause listed */
    int shown;                  /* groups listed */
    probe_reading_t rd[MAX_READINGS];
    int n;
    int *order, nrun, nbest;    /* the groups ranked; in the running; fitting best */
    int sug_net, sug_pin, counts[PROBE_NREADINGS];
    bool sug_leg;               /* the suggestion is of the chip's leg (else its pad) */
    bool on_legs;               /* readings taken on the chips' legs (else the pads), kept when the board changes */
    int sug_bench;              /* the suggestion's bench procedure step, 0 none */
    int strategy;               /* PROBE_*, kept when the board changes */
    /* The screen picked, in steps: its kind and colors (in db.kinds and
     * db.colorsets, -1 any), then one of those pictures (-1 any). */
    int kind, colors, exact;
    /* Or the pictures a screenshot matched best (diagnose.c): their
     * labels, applied once the faults are loaded (pending), how many of
     * them the faults with readings give, and whether they are the ones
     * picked now. */
    char shot[ATLAS_MAX_GROUPS][256];
    int nshot, shot_found, shot_first;
    bool shot_pending, from_shot;
    /* What is printed on the chips of the real machine (MARK_*, per part
     * of the machine's board), kept when the board changes. */
    uint8_t *marks[ATLAS_NMACHINES];
    int cur_pin;                /* the pin the reading buttons are for */
    int last_scope;             /* the scope pin seen last frame */
    char pin_text[24], pin_note[96];
    bool typing;
    struct nk_rect bounds;
    /* What the repair records say of each part (diagnose_record_boost()). */
    float *boost;
    char boost_note[600];
    unsigned boost_serial;
    char import_note[300];      /* what "Import capture" took in */
} P = { .sug_net = -1, .sug_pin = -1, .cur_pin = -1, .last_scope = -1, .on_legs = true };

void probe_open(void) { P.open = P.reopen = true; }
void probe_close(void) { P.open = false; P.typing = false; }
bool probe_is_open(void) { return P.open; }
bool probe_wants_keyboard(void) { return P.open && P.typing; }
int probe_marked_pin(void) { return P.open ? P.cur_pin : -1; }

bool probe_bounds(struct nk_rect *r) {
    if (!P.open) return false;
    *r = P.bounds;
    return true;
}

static void unload(void) {
    if (P.loaded) probe_db_free(&P.db);
    P.loaded = false;
    free(P.order);
    free(P.expanded);
    free(P.boost);
    P.order = NULL;
    P.expanded = NULL;
    P.boost = NULL;
    P.boost_note[0] = 0;
}

/* Rank the groups on the readings, and the pin to probe next. */
static void update(void) {
    if (!P.loaded) return;
    P.nrun = probe_rank(&P.db, P.rd, P.n, P.order);
    P.nbest = 0;
    float best = P.db.ngroups ? P.db.groups[P.order[0]].cost : 0;
    while (P.nbest < P.nrun && P.db.groups[P.order[P.nbest]].cost <= best + 1e-4f) P.nbest++;
    P.sug_net = probe_suggest(&P.db, P.b, P.rd, P.n, P.order, P.nrun, &P.sug_pin, &P.sug_leg, P.counts, &P.sug_bench);
    P.cur_pin = P.sug_pin;
    P.shown = SHOWN_GROUPS;
    memset(P.expanded, 0, (size_t)P.db.ngroups * sizeof(bool));
}

/* The weights of the faults: the chip markings, and the repair records. */
static void set_weights(void) {
    free(P.boost);
    P.boost = malloc((size_t)P.b->sim.nparts * sizeof(float));
    diagnose_record_boost(P.b, P.machine, P.boost, P.boost_note, sizeof(P.boost_note));
    P.boost_serial = diagnose_records_serial();
    probe_db_set_weights(&P.db, P.b, P.marks[P.machine], P.boost);
}

/* A new board on the bench (or the first): its faults, no readings. */
static void attach(board_t *b, int machine) {
    unload();
    P.b = b;
    P.machine = machine;
    P.n = 0;
    P.kind = P.colors = P.exact = -1;
    P.from_shot = false;
    P.last_scope = b->scope_pin;
    P.pin_note[0] = 0;
    char path[2100];
    diagnose_atlas_file(path, sizeof(path));
    P.loaded = probe_db_load(&P.db, path, machine, b, P.err, sizeof(P.err));
    if (!P.loaded) return;
    if (!P.marks[machine]) P.marks[machine] = calloc((size_t)b->sim.nparts, 1);
    P.db.strategy = P.strategy;
    P.db.on_legs = P.on_legs;
    set_weights();
    P.order = malloc((size_t)P.db.ngroups * sizeof(int));
    P.expanded = calloc((size_t)P.db.ngroups, sizeof(bool));
    update();
}

static void wrapped(struct nk_context *ctx, const char *text, float width, struct nk_color c);

/* How the next pin is picked. */
static void strategy_choice(struct nk_context *ctx, float cw) {
    static const char *const tips[PROBE_NSTRATEGIES] = {
        "Start the way a repair does: reset and clock at the CPU, then the PLA outputs; then the likeliest faults",
        "The pin that best splits the likeliest faults (PLA, CIAs, CPU, RAM, ROMs, and the chip markings below)",
        "The pin that splits all the faults left most evenly, every fault as likely: the fewest probes on average" };
    const char *names[PROBE_NSTRATEGIES];
    for (int s = 0; s < PROBE_NSTRATEGIES; s++) names[s] = probe_strategy_name(s);
    nk_layout_row_begin(ctx, NK_STATIC, 28, 2);
    nk_layout_row_push(ctx, 140);
    nk_label(ctx, "Next pin by:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, cw - 146);
    int s = combo_tip(ctx, names, PROBE_NSTRATEGIES, P.strategy, 22, nk_vec2(cw - 146, 100), tips[P.strategy]);
    nk_layout_row_end(ctx);
    if (s != P.strategy) {
        P.strategy = P.db.strategy = s;
        update();
    }
    static const char *const where[2] = { "The chips' legs (top side)", "The solder side (pads)" };
    nk_layout_row_begin(ctx, NK_STATIC, 28, 2);
    nk_layout_row_push(ctx, 140);
    nk_label(ctx, "Probing on:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, cw - 146);
    const char *tip = P.on_legs ? "On a leg that is lifted or not soldered, the probe shows the chip's side: what "
                                  "the chip drives there, or floating for an input. That finds open legs"
                                : "A pad shows the trace, whatever the chip's leg does: an open leg can look "
                                  "healthy there. When only a leg tells the faults left apart, it asks for the leg";
    int on = combo_tip(ctx, where, 2, P.on_legs ? 0 : 1, 22, nk_vec2(cw - 146, 80), tip) == 0;
    nk_layout_row_end(ctx);
    if (on != P.on_legs) {
        P.on_legs = P.db.on_legs = on;
        update();
    }
}

/* What is printed on the chips of the real machine: the maker moves how
 * likely each is to be the fault. */
static void markings(struct nk_context *ctx, float cw) {
    bool any = false;       /* none on a Spectrum: its ranking has no makers */
    for (int i = 0; i < P.b->sim.nparts && !any; i++) any = probe_part_markable(P.b, i) != MARK_UNKNOWN;
    if (!any || !nk_tree_push(ctx, NK_TREE_TAB, "Chip markings on the real board", NK_MINIMIZED)) return;
    uint8_t *m = P.marks[P.machine];
    sim_t *s = &P.b->sim;
    bool changed = false;
    wrapped(ctx, "MT (Micron) RAM and MOS made logic chips fail a lot; a PLA not made by MOS (the early ones) "
                 "hardly ever.", cw - 30, COL_DIMTEXT);
    nk_layout_row_dynamic(ctx, 24, 3);
    if (nk_button_label(ctx, "All RAM: MT"))
        for (int i = 0; i < s->nparts; i++) if (probe_part_markable(P.b, i) == MARK_MT) { m[i] = MARK_MT; changed = true; }
    if (nk_button_label(ctx, "All logic: MOS"))
        for (int i = 0; i < s->nparts; i++)
            if (probe_part_markable(P.b, i) == MARK_MOS && board_part_role(P.b, i) != ROLE_PLA) { m[i] = MARK_MOS; changed = true; }
    if (nk_button_label(ctx, "Clear all")) { memset(m, 0, (size_t)s->nparts); changed = true; }
    /* The PLA, the RAM, the logic: two chips a row. */
    static const int roles[][2] = { { ROLE_PLA, ROLE_PLA }, { ROLE_RAM, ROLE_VRAM }, { ROLE_GLUE, ROLE_CLOCK } };
    for (int r = 0; r < 3; r++) {
        int col = 0;
        for (int i = 0; i < s->nparts; i++) {
            int kind = probe_part_markable(P.b, i), role = board_part_role(P.b, i);
            if (!kind || (role != roles[r][0] && role != roles[r][1])) continue;
            if (col == 0) nk_layout_row_dynamic(ctx, 24, 4);
            char label[40], mk[8];
            const char *pl = board_part_label(P.b, i);
            snprintf(label, sizeof(label), "%s %s", s->parts[i].ref, pl ? pl : "");
            snprintf(mk, sizeof(mk), "%s", kind == MARK_MT ? "MT" : "MOS");
            const char *items[3] = { "not checked", mk, "other maker" };
            int sel = m[i] == kind ? 1 : m[i] == MARK_OTHER ? 2 : 0;
            nk_label(ctx, label, NK_TEXT_LEFT);
            int ns = nk_combo(ctx, items, 3, sel, 20, nk_vec2(130, 90));
            if (ns != sel) {
                m[i] = (uint8_t)(ns == 1 ? kind : ns == 2 ? MARK_OTHER : MARK_UNKNOWN);
                changed = true;
            }
            col = (col + 1) % 2;
        }
    }
    nk_tree_pop(ctx);
    if (changed) {
        set_weights();
        update();
    }
}

/* Is the reading at PIN of its leg? The suggestion says; another pin is
 * probed where the window says. */
static bool leg_of(int pin) { return pin == P.sug_pin ? P.sug_leg : P.on_legs; }

static void add_reading(int pin, int reading) {
    int net = pin >= 0 ? P.b->sim.pins[pin].net : -1;
    if (net < 0 || P.n >= MAX_READINGS) return;     /* a leg on no net (NC) tells nothing */
    bool leg = leg_of(pin);
    /* Probing the same place again: the new reading replaces the old one.
     * Two pads of a net are the same place; two legs aren't (one of them
     * may be lifted). */
    for (int i = 0; i < P.n; i++)
        if ((P.rd[i].pin == pin && P.rd[i].leg == leg) || (P.rd[i].net == net && !leg && !P.rd[i].leg)) {
            memmove(&P.rd[i], &P.rd[i + 1], (size_t)(P.n - i - 1) * sizeof(probe_reading_t));
            P.n--;
            break;
        }
    P.rd[P.n++] = (probe_reading_t){ pin, net, reading, leg, false };
    update();
}

/* The pin a capture's reading of NET is put on: the edge connector's (where
 * the analyser is clipped on), else the first pin of the net. */
static int pin_for_net(int net) {
    const sim_t *s = &P.b->sim;
    int root = net;
    while (s->nets[root].root != root) root = s->nets[root].root;
    int first = -1;
    for (int p = 0; p < s->npins; p++) {
        int r = s->pins[p].net;
        if (r < 0) continue;
        while (s->nets[r].root != r) r = s->nets[r].root;
        if (r != root) continue;
        if (s->parts[s->pins[p].part].kind == PART_CONNECTOR) return p;
        if (first < 0) first = p;
    }
    return first;
}

/* "Import capture": the lines of a logic analyser capture (best idle.lac,
 * the board left running) as readings on their pads; one already taken
 * on the same net is replaced. */
static void import_from(const char *path) {
    char err[300];
    int net[64], rd[64];
    int n = la_probe_readings(P.b, path, net, rd, 64, err, sizeof(err));
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (n < 0) { snprintf(P.import_note, sizeof(P.import_note), "Couldn't take %.60s: %.200s", base, err); return; }
    static const int as[3] = { PROBE_LOW, PROBE_HIGH, PROBE_PULSING };
    int added = 0;
    for (int k = 0; k < n; k++) {
        int pin = pin_for_net(net[k]);
        if (pin < 0) continue;
        int pn = P.b->sim.pins[pin].net;
        for (int i = 0; i < P.n; i++)
            if (P.rd[i].net == pn && !P.rd[i].leg) {
                memmove(&P.rd[i], &P.rd[i + 1], (size_t)(P.n - i - 1) * sizeof(probe_reading_t));
                P.n--;
                break;
            }
        if (P.n >= MAX_READINGS) break;
        P.rd[P.n++] = (probe_reading_t){ .pin = pin, .net = pn, .reading = as[rd[k]], .leg = false, .analyser = true };
        added++;
    }
    snprintf(P.import_note, sizeof(P.import_note),
             "Took %d line%s from %.60s (marked \"analyser\"). Its \"low\" counts as low or floating: the analyser reads a "
             "floating line (about 1.2 V) as low.", added, added == 1 ? "" : "s", base);
    update();
}

static void remove_reading(int i) {
    memmove(&P.rd[i], &P.rd[i + 1], (size_t)(P.n - i - 1) * sizeof(probe_reading_t));
    P.n--;
    update();
}

static int parse_pin(board_t *b, const char *text) { return probe_parse_pin(b, text); }

static void apply_screens(void);
static void apply_shot(void);
static void save_to(const char *path);
static void load_from(const char *path);

/* The file dialogs the buttons ask for: opened by ui.c after the frame,
 * with the board unlocked (a dialog opened while drawing the window hung
 * the program: its message loop ran the frame again, which waited for
 * the board this frame had locked). */
static int want_dialog;         /* PROBE_DIALOG_*, 0 none */

int probe_wants_dialog(void) {
    int w = want_dialog;
    want_dialog = 0;
    return w;
}

int probe_ask_dialog(int what, char *path, int len) {
    return what == PROBE_DIALOG_CAPTURE ? filedialog_capture(path, len) : filedialog_probe(what == PROBE_DIALOG_SAVE, path, len);
}

void probe_dialog_done(int what, const char *path) {
    if (!P.loaded) return;
    if (what == PROBE_DIALOG_CAPTURE) import_from(path);
    else if (what == PROBE_DIALOG_SAVE) save_to(path);
    else if (what == PROBE_DIALOG_LOAD) load_from(path);
}

/* "Save...": the readings and the settings that rank them (read back by
 * Load...), then the report of what they say (probe_file.c). */
static void save_to(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    FILE *f = fopen(path, "w");
    if (!f) { snprintf(P.import_note, sizeof(P.import_note), "Couldn't write %.200s", base); return; }
    time_t now = time(NULL);
    char when[40];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&now));
    fprintf(f, "# Diagnose by probing, saved %s by 8bit-hw-simulator (AI-generated).\n"
               "# The lines up to \"----\" are read back by Load...; the report after them is for reading.\n", when);
    fprintf(f, "board: %s\n", atlas_machine_id(P.machine));
    fprintf(f, "strategy: %s\n", probe_strategy_name(P.strategy));
    fprintf(f, "probing-on: %s\n", P.on_legs ? "legs" : "pads");
    if (P.from_shot)
        for (int i = 0; i < P.nshot; i++) fprintf(f, "screenshot: %s\n", P.shot[i]);
    else {
        if (P.kind >= 0) fprintf(f, "screen-kind: %s\n", P.db.kinds[P.kind].name);
        if (P.colors >= 0) fprintf(f, "screen-colors: %s\n", P.db.colorsets[P.colors].name);
        if (P.exact >= 0) fprintf(f, "screen-exact: %s\n", P.db.screens[P.exact].label);
    }
    const sim_t *s = &P.b->sim;
    const uint8_t *m = P.marks[P.machine];
    for (int i = 0; m && i < s->nparts; i++)
        if (m[i]) fprintf(f, "mark: %s %s\n", s->parts[i].ref, m[i] == MARK_MT ? "MT" : m[i] == MARK_MOS ? "MOS" : "other");
    for (int i = 0; P.boost && i < s->nparts; i++)
        if (P.boost[i] != 1) fprintf(f, "weight: %s %.3f\n", s->parts[i].ref, P.boost[i]);
    for (int i = 0; i < P.n; i++) {
        const probe_reading_t *r = &P.rd[i];
        fprintf(f, "reading: %s.%s %s %s%s\n", s->parts[s->pins[r->pin].part].ref, s->bd->pads[r->pin].name,
                r->leg ? "leg" : "pad", r->analyser ? "analyser " : "", probe_reading_name(r->reading));
    }
    fprintf(f, "---- the report (not read back) ----\n\n");
    if (P.boost_note[0]) fprintf(f, "%s\n", P.boost_note);
    probe_report(f, &P.db, P.b, P.rd, P.n);
    bool ok = !ferror(f);
    if (fclose(f) || !ok) snprintf(P.import_note, sizeof(P.import_note), "Couldn't write all of %.200s", base);
    else snprintf(P.import_note, sizeof(P.import_note), "Saved %d reading%s and the suspects to %.200s", P.n,
                  P.n == 1 ? "" : "s", path);
    update();       /* probe_report() ranked them: the window's order again */
}

/* "Load...": a saved probing's readings and settings, on this board. The
 * repair records' weights are this computer's own, not the file's. */
static void load_from(const char *path) {
    char err[400];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    static probe_file_t pf;
    if (!probe_file_read(path, &pf, err, sizeof(err))) {
        snprintf(P.import_note, sizeof(P.import_note), "Couldn't load %.60s: %.200s", base, err);
        return;
    }
    if (strcmp(pf.board, atlas_machine_id(P.machine))) {
        snprintf(P.import_note, sizeof(P.import_note), "%.60s is of another board (%s): put that one on the bench first",
                 base, pf.board);
        return;
    }
    if (pf.strategy >= 0) P.strategy = P.db.strategy = pf.strategy;
    if (pf.on_legs >= 0) P.on_legs = P.db.on_legs = pf.on_legs;
    uint8_t *m = P.marks[P.machine];
    if (m) {
        memset(m, 0, (size_t)P.b->sim.nparts);
        for (int i = 0; i < pf.nmarks; i++) {
            int p = sim_find_part(&P.b->sim, pf.marks[i].ref);
            if (p >= 0) m[p] = (uint8_t)pf.marks[i].mark;
        }
    }
    P.kind = P.colors = P.exact = -1;
    if (pf.nshot) {
        P.nshot = pf.nshot;
        for (int i = 0; i < pf.nshot; i++) snprintf(P.shot[i], sizeof(P.shot[i]), "%s", pf.shot[i]);
        apply_shot();
        if (!P.shot_found) apply_screens();
    } else {
        for (int k = 0; k < P.db.nkinds; k++) if (!strcmp(P.db.kinds[k].name, pf.kind)) P.kind = k;
        for (int c = 0; c < P.db.ncolorsets; c++) if (pf.colors[0] && !strcmp(P.db.colorsets[c].name, pf.colors)) P.colors = c;
        for (int sc = 0; sc < P.db.nscreens; sc++) if (pf.exact[0] && !strcmp(P.db.screens[sc].label, pf.exact)) P.exact = sc;
        apply_screens();
    }
    P.n = 0;
    int missing = 0;
    for (int i = 0; i < pf.n && P.n < MAX_READINGS; i++) {
        int pin = probe_parse_pin(P.b, pf.rd[i].pin);
        if (pin < 0 || P.b->sim.pins[pin].net < 0) { missing++; continue; }
        P.rd[P.n++] = (probe_reading_t){ pin, P.b->sim.pins[pin].net, pf.rd[i].reading, pf.rd[i].leg, pf.rd[i].analyser };
    }
    set_weights();
    update();
    snprintf(P.import_note, sizeof(P.import_note), "Loaded %d reading%s from %.60s%s", P.n, P.n == 1 ? "" : "s", base,
             missing ? " (some pins aren't on this board: left out)" : "");
}

/* A row tall enough for TEXT wrapped in WIDTH. */
static float wrapped_height(struct nk_context *ctx, const char *text, float width) {
    float fh = ctx->style.font->height;
    int lines = (int)(text_width(ctx, text) / fmaxf(width - 12, 40)) + 1;
    return fmaxf(22, lines * (fh + 3) + 6);
}

static void wrapped(struct nk_context *ctx, const char *text, float width, struct nk_color c) {
    nk_layout_row_dynamic(ctx, wrapped_height(ctx, text, width), 1);
    nk_label_colored_wrap(ctx, text, c);
}

static struct nk_color reading_color(int r) {
    return r == PROBE_LOW ? COL_LOW : r == PROBE_HIGH ? COL_HIGH : r == PROBE_PULSING ? COL_PULSE
         : r == PROBE_BAD ? COL_FIGHT : COL_FLOAT;
}

/* A button filled with reading R's color (a little darker, so the text
 * reads): black text on the light ones, white on the dark. */
static bool reading_button(struct nk_context *ctx, const char *label, int r) {
    struct nk_color c = reading_color(r);
    struct nk_color bg = nk_rgb(c.r * 4 / 5, c.g * 4 / 5, c.b * 4 / 5);
    float lum = 0.299f * bg.r + 0.587f * bg.g + 0.114f * bg.b;
    return color_button(ctx, label, bg, lum > 140 ? nk_rgb(0, 0, 0) : nk_rgb(255, 255, 255));
}

/* A wrapped line that says more when pointed at (TIP NULL: nothing). */
static void wrapped_tip(struct nk_context *ctx, const char *text, float width, struct nk_color c, const char *tip) {
    nk_layout_row_dynamic(ctx, wrapped_height(ctx, text, width), 1);
    if (tip && nk_widget_is_hovered(ctx)) nk_tooltip(ctx, tip);
    nk_label_colored_wrap(ctx, text, c);
}

/* What a healthy board shows on a net, "" if the atlas doesn't say. */
static const char *healthy_on(int net) {
    if (P.db.healthy < 0 || net < 0) return "";
    return probe_reading_name(probe_reading_of(P.db.groups[P.db.healthy].nets[net]));
}

/* A picture of the kind and colors picked. */
static bool screen_fits(int s) {
    const struct probe_screen *sc = &P.db.screens[s];
    return (P.kind < 0 || sc->kind == P.kind) && (P.colors < 0 || sc->colors == P.colors);
}

/* Only the faults giving the pictures picked. */
static void apply_screens(void) {
    probe_db_pick_screens(&P.db, P.kind, P.colors, P.exact);
    P.from_shot = false;
    update();
}

/* The screenshot's pictures, the faults being loaded: any of them,
 * however it moves. */
static void apply_shot(void) {
    P.shot_pending = false;
    P.shot_first = 0;
    P.shot_found = probe_db_pick_shot(&P.db, (const char (*)[256])P.shot, P.nshot, &P.shot_first);
    if (P.shot_found) {
        P.kind = P.colors = P.exact = -1;
        P.from_shot = true;
        update();
    }
}

void probe_screens_from_screenshot(const char (*labels)[256], int n) {
    P.nshot = n < ATLAS_MAX_GROUPS ? n : ATLAS_MAX_GROUPS;
    for (int i = 0; i < P.nshot; i++) snprintf(P.shot[i], sizeof(P.shot[i]), "%s", labels[i]);
    P.shot_pending = true;
}

/* What tells picture S apart from the others of its kind and colors: its
 * label without "Vertical bars: " and " (white border, black screen)". */
static void screen_detail(int s, char *out, int len) {
    const char *label = P.db.screens[s].label, *colors = P.db.colorsets[P.db.screens[s].colors].name;
    const char *colon = strstr(label, ": ");
    if (colon && colon - label < 40) label = colon + 2;
    snprintf(out, (size_t)len, "%s", label);
    char paren[80];
    snprintf(paren, sizeof(paren), " (%s)", colors);
    char *p = colors[0] ? strstr(out, paren) : NULL;
    if (p) memmove(p, p + strlen(paren), strlen(p + strlen(paren)) + 1);
    if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
}

/* One step of picking the screen: a label and a combo. */
static bool screen_combo(struct nk_context *ctx, float cw, const char *what, const char *cur) {
    nk_layout_row_begin(ctx, NK_STATIC, 28, 2);
    nk_layout_row_push(ctx, 140);
    nk_label(ctx, what, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, cw - 146);
    bool open = nk_combo_begin_label(ctx, cur, nk_vec2(cw - 146, 420));
    if (open) nk_layout_row_dynamic(ctx, 22, 1);
    return open;
}

static void screen_end(struct nk_context *ctx, bool open) {
    if (open) nk_combo_end(ctx);
    nk_layout_row_end(ctx);
}

/* What the real machine's screen shows: only the faults that give that.
 * Picked in steps, so a few hundred pictures stay easy to go through: the
 * kind of picture, its colors, then (if it helps) the very one. */
static void screen_choice(struct nk_context *ctx, float cw) {
    char cur[256], item[300];
    int nf[2] = { 0, 0 };       /* faults: of the kind and colors, pictures */
    for (int s = 0; s < P.db.nscreens; s++)
        if (screen_fits(s)) { nf[0] += P.db.screens[s].nfaults; nf[1]++; }

    if (P.from_shot)
        snprintf(cur, sizeof(cur), "As the screenshot (%d picture%s)", P.shot_found, P.shot_found == 1 ? "" : "s");
    else snprintf(cur, sizeof(cur), "%s", P.kind < 0 ? "Don't know / any picture" : P.db.kinds[P.kind].name);
    bool open = screen_combo(ctx, cw, "The screen shows:", cur);
    if (open) {
        if (P.shot_found && nk_combo_item_label(ctx, "As the screenshot (its best matches)", NK_TEXT_LEFT))
            apply_shot();
        if (nk_combo_item_label(ctx, "Don't know / any picture", NK_TEXT_LEFT)) {
            P.kind = P.colors = P.exact = -1;
            apply_screens();
        }
        for (int k = 0; k < P.db.nkinds; k++) {
            snprintf(item, sizeof(item), "%s   (%d fault%s)", P.db.kinds[k].name, P.db.kinds[k].nfaults,
                     P.db.kinds[k].nfaults == 1 ? "" : "s");
            if (nk_combo_item_label(ctx, item, NK_TEXT_LEFT)) {
                P.kind = k;
                P.colors = P.exact = -1;
                apply_screens();
            }
        }
    }
    screen_end(ctx, open);
    if (P.from_shot) {
        char line[600];
        snprintf(line, sizeof(line), "From the screenshot: only the faults giving %s%s", P.shot[P.shot_first],
                 P.shot_found > 1 ? ", or a picture about as alike" : "");
        wrapped(ctx, line, cw, COL_DIMTEXT);
    } else if (P.nshot && !P.shot_found && P.kind < 0) {
        wrapped(ctx, "The screenshot's best matches have no probe readings in the atlas: pick what the screen "
                     "shows here.", cw, COL_PULSE);
    }
    if (P.kind < 0) return;

    /* The colors pictures of that kind come in. */
    int *ncol = calloc((size_t)P.db.ncolorsets + 1, sizeof(int)), shown = 0;
    for (int s = 0; s < P.db.nscreens; s++)
        if (P.db.screens[s].kind == P.kind) ncol[P.db.screens[s].colors] += P.db.screens[s].nfaults;
    for (int c = 0; c < P.db.ncolorsets; c++) shown += ncol[c] > 0;
    if (shown > 1) {            /* one: nothing to pick */
        snprintf(cur, sizeof(cur), "%s", P.colors < 0 ? "Any colors" : P.db.colorsets[P.colors].name[0] ?
                 P.db.colorsets[P.colors].name : "(colors not stated)");
        open = screen_combo(ctx, cw, "Colors:", cur);
        if (open) {
            if (nk_combo_item_label(ctx, "Any colors", NK_TEXT_LEFT)) { P.colors = P.exact = -1; apply_screens(); }
            for (int c = 0; c < P.db.ncolorsets; c++) {
                if (!ncol[c]) continue;
                snprintf(item, sizeof(item), "%s   (%d fault%s)", P.db.colorsets[c].name[0] ? P.db.colorsets[c].name :
                         "(colors not stated)", ncol[c], ncol[c] == 1 ? "" : "s");
                if (nk_combo_item_label(ctx, item, NK_TEXT_LEFT)) { P.colors = c; P.exact = -1; apply_screens(); }
            }
        }
        screen_end(ctx, open);
    }
    free(ncol);

    /* Then any of those pictures, or one of them. */
    if (P.exact >= 0) screen_detail(P.exact, cur, sizeof(cur));
    else snprintf(cur, sizeof(cur), "Any of these %d picture%s (%d fault%s)", nf[1], nf[1] == 1 ? "" : "s", nf[0],
                  nf[0] == 1 ? "" : "s");
    open = screen_combo(ctx, cw, "Exactly:", cur);
    if (open) {
        if (nk_combo_item_label(ctx, "Any of these", NK_TEXT_LEFT)) { P.exact = -1; apply_screens(); }
        for (int s = 0; s < P.db.nscreens; s++) {
            if (!screen_fits(s)) continue;
            char detail[256];
            screen_detail(s, detail, sizeof(detail));
            snprintf(item, sizeof(item), "%s   (%d fault%s)", detail, P.db.screens[s].nfaults,
                     P.db.screens[s].nfaults == 1 ? "" : "s");
            if (nk_combo_item_label(ctx, item, NK_TEXT_LEFT)) { P.exact = s; apply_screens(); }
        }
    }
    screen_end(ctx, open);
}

/* The pin the buttons are for, what to expect there, the buttons. */
static void probe_next(struct nk_context *ctx, float cw, probe_action_t *act) {
    char line[300], desc[160];
    int pin = P.cur_pin;
    if (pin < 0) {
        const char *why = P.n && P.nbest <= 1 ? "Nothing left to tell apart."
                        : P.n ? "No pin on the board tells the faults left apart: to a logic probe they all look the same."
                              : "Click a pin on the board to probe it.";
        wrapped(ctx, why, cw, COL_TRACE);
    } else {
        int net = P.b->sim.pins[pin].net;
        pin_desc(P.b, pin, desc, sizeof(desc));
        bool leg = leg_of(pin);
        /* Where to put the probe only when it isn't where "Probing on" says;
         * how the pin splits the suspects when pointed at. */
        char tip[300] = "";
        if (pin == P.sug_pin) {
            int nk = 0;
            for (int r = 0; r < PROBE_NREADINGS; r++) nk += P.counts[r];
            snprintf(tip, sizeof(tip), "Splits the %d groups of suspects: low %d, high %d, pulsing %d, floating %d, "
                     "bad levels %d", nk, P.counts[PROBE_LOW], P.counts[PROBE_HIGH], P.counts[PROBE_PULSING],
                     P.counts[PROBE_FLOATING], P.counts[PROBE_BAD]);
        }
        snprintf(line, sizeof(line), "%s %s%s", pin == P.sug_pin ? "Probe next:" : "Probing:", desc,
                 leg == P.on_legs ? "" : leg ? ", on the chip's leg" : ", on its pad (solder side)");
        wrapped_tip(ctx, line, cw, COL_HOVER, tip[0] ? tip : NULL);
        if (pin == P.sug_pin && leg && !P.on_legs)
            wrapped(ctx, "Only the chip's side of this leg tells the faults left apart: probe the leg itself, "
                         "above the board (a lifted or unsoldered leg shows what the chip does, not the trace).",
                    cw, COL_TRACE);
        if (pin == P.sug_pin && P.sug_bench) {
            snprintf(line, sizeof(line), "Bench procedure, step %d: %s", P.sug_bench,
                     probe_bench_step(P.machine, P.sug_bench));
            wrapped(ctx, line, cw, COL_TRACE);
        }
        snprintf(line, sizeof(line), "A healthy board shows %s here (net %s)",
                 healthy_on(net)[0] ? healthy_on(net) : "(not known)", net >= 0 ? P.b->sim.nets[net].name : "-");
        wrapped(ctx, line, cw, COL_DIMTEXT);
        /* What the probe shows: one button a reading, in the board view's
         * colors (low blue, high red, pulsing yellow, floating grey, bad
         * levels magenta). */
        static const char *const labels[PROBE_NREADINGS] = { "Low", "High", "Pulsing", "Floating", "Bad levels" };
        static const char *const tips[PROBE_NREADINGS] = {
            "Steady low, no activity",
            "Steady high, no activity",
            "Any activity at all, even an occasional pulse on a line that is mostly high or low",
            "Nothing drives it: no light on a logic probe, or a steady level around 1.4 V with no activity",
            "Active, but the levels are wrong on the scope: swings that don't reach a proper low or high "
            "(two chips driving it at once, a weak or dead output)" };
        nk_layout_row_dynamic(ctx, 30, PROBE_NREADINGS);
        for (int i = 0; i < PROBE_NREADINGS; i++) {
            if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, tips[i]);
            if (reading_button(ctx, labels[i], i)) add_reading(pin, i);
        }
        /* The scope looks like the simulator's healthy board: whatever it
         * shows there, without putting it into words. */
        int healthy = P.db.healthy >= 0 && net >= 0 ? probe_reading_of(P.db.groups[P.db.healthy].nets[net]) : -1;
        nk_layout_row_begin(ctx, NK_STATIC, 28, 2);
        nk_layout_row_push(ctx, cw - 130);
        if (healthy >= 0) {
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "The real board looks like the simulator's healthy board here: enters what it shows");
            snprintf(line, sizeof(line), "Same as a healthy board: %s", probe_reading_name(healthy));
            if (reading_button(ctx, line, healthy)) add_reading(pin, healthy);
        } else nk_label_colored(ctx, "(what a healthy board shows here isn't known)", NK_TEXT_LEFT, COL_DIMTEXT);
        nk_layout_row_push(ctx, 120);
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Zoom the board in on this pin");
        if (nk_button_label(ctx, "Show on board")) act->show_pin = pin;
        nk_layout_row_end(ctx);
    }
    /* Another pin: typed, or clicked on the board. */
    nk_layout_row_begin(ctx, NK_STATIC, 28, 4);
    nk_layout_row_push(ctx, 100);
    nk_label(ctx, "Another pin:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 90);
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Write it like U7.3 (the chip, a dot, the pin)");
    nk_flags ev = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, P.pin_text,
                                                 sizeof(P.pin_text), nk_filter_ascii);
    P.typing = (ev & NK_EDIT_ACTIVE) != 0;
    nk_layout_row_push(ctx, 50);
    bool go = nk_button_label(ctx, "Go") || (ev & NK_EDIT_COMMITED);
    nk_layout_row_push(ctx, cw - 100 - 90 - 50 - 12);
    nk_label_colored(ctx, "or click one on the board", NK_TEXT_LEFT, COL_DIMTEXT);
    nk_layout_row_end(ctx);
    if (go && P.pin_text[0]) {
        int p = parse_pin(P.b, P.pin_text);
        if (p >= 0 && P.b->sim.pins[p].net < 0)
            snprintf(P.pin_note, sizeof(P.pin_note), "%s isn't connected to anything: nothing to probe", P.pin_text);
        else if (p >= 0) {
            P.cur_pin = p;
            act->show_pin = p;
            P.pin_note[0] = 0;
        } else snprintf(P.pin_note, sizeof(P.pin_note), "No pin %s on this board (write it like U7.3)", P.pin_text);
    }
    if (P.pin_note[0]) wrapped(ctx, P.pin_note, cw, COL_HIGH);
    if (pin >= 0 && pin != P.sug_pin && P.sug_pin >= 0) {
        nk_layout_row_static(ctx, 26, 240, 1);
        pin_desc(P.b, P.sug_pin, desc, sizeof(desc));
        snprintf(line, sizeof(line), "Back to the suggestion");
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, desc);
        if (nk_button_label(ctx, line)) { P.cur_pin = P.sug_pin; act->show_pin = P.sug_pin; }
    }
}

/* Bad levels on NET: the chips that drive it, and one resistor further.
 * The simulation can't make an output weak, so the atlas rarely has the
 * cause: the board's wiring says where to look. */
static void bad_levels_note(int net, char *buf, size_t len) {
    const sim_t *s = &P.b->sim;
    char desc[160];
    size_t n = (size_t)snprintf(buf, len, "Bad levels on %s. A weak output can't be simulated, so the atlas "
                                "may not have the cause: check what drives it:", s->nets[net].name);
    const char *sep = " ";
    for (int i = 0; i < s->npins && n < len; i++) {
        const pin_t *p = &s->pins[i];
        if (p->net != net || !(p->flags & PIN_OUT) || s->parts[p->part].kind != PART_IC) continue;
        pin_desc(P.b, i, desc, sizeof(desc));
        n += (size_t)snprintf(buf + n, len - n, "%s%s", sep, desc);
        sep = ", ";
    }
    /* Through a series resistor: the outputs on its other side. */
    for (int i = 0; i < s->npins && n < len; i++) {
        const pin_t *p = &s->pins[i];
        if (p->net != net || s->parts[p->part].kind != PART_RESISTOR) continue;
        for (int j = 0; j < s->npins && n < len; j++) {
            const pin_t *q = &s->pins[j];
            if (q->part != p->part || j == i || q->net < 0) continue;
            for (int k = 0; k < s->npins && n < len; k++) {
                const pin_t *o = &s->pins[k];
                if (o->net != q->net || !(o->flags & PIN_OUT) || s->parts[o->part].kind != PART_IC) continue;
                pin_desc(P.b, k, desc, sizeof(desc));
                n += (size_t)snprintf(buf + n, len - n, "%s%s through %s", sep, desc, s->parts[p->part].ref);
                sep = ", ";
            }
        }
    }
    if (n < len) snprintf(buf + n, len - n, ". A short to a neighbor pin or trace does it too.");
}

/* The readings so far, each with what a healthy board shows there. */
static void readings(struct nk_context *ctx, float cw) {
    char line[300], desc[160];
    bool zx = atlas_family(P.machine) == ATLAS_ZX48;
    nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
    nk_layout_row_push(ctx, cw - 196);
    snprintf(line, sizeof(line), "Readings: %d", P.n);
    nk_label(ctx, line, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 90);
    if (nk_button_label(ctx, "Undo") && P.n) remove_reading(P.n - 1);
    nk_layout_row_push(ctx, 90);
    if (nk_button_label(ctx, "Clear all") && P.n) { P.n = 0; P.import_note[0] = 0; update(); }
    nk_layout_row_end(ctx);
    /* The files: an analyser capture in, the probing out and back. */
    nk_layout_row_dynamic(ctx, 26, zx ? 3 : 2);
    if (zx) {
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, "Take the readings from a logic analyser capture: every line at the edge connector, low, "
                            "high or pulsing. Best idle.lac from a diagnosis folder (the Spectrum left running)");
        if (nk_button_label(ctx, "Import capture...")) want_dialog = PROBE_DIALOG_CAPTURE;
    }
    if (nk_widget_is_hovered(ctx))
        nk_tooltip(ctx, "Save the readings, the settings and the suspects to a text file (to read, to send, or to load "
                        "again later)");
    if (nk_button_label(ctx, "Save...")) want_dialog = PROBE_DIALOG_SAVE;
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Load the readings and settings of a saved probing (this board's)");
    if (nk_button_label(ctx, "Load...")) want_dialog = PROBE_DIALOG_LOAD;
    if (P.import_note[0]) wrapped(ctx, P.import_note, cw, COL_DIMTEXT);
    const probe_group_t *best = P.db.ngroups ? &P.db.groups[P.order[0]] : NULL;
    for (int i = 0; i < P.n; i++) {
        const probe_reading_t *r = &P.rd[i];
        pin_desc(P.b, r->pin, desc, sizeof(desc));
        if (r->analyser) {
            size_t dl = strlen(desc);
            snprintf(desc + dl, sizeof(desc) - dl, " (analyser)");
        } else if (r->leg != P.on_legs) {
            size_t dl = strlen(desc);
            snprintf(desc + dl, sizeof(desc) - dl, r->leg ? " (leg)" : " (pad)");
        }
        nk_layout_row_begin(ctx, NK_STATIC, 22, 4);
        nk_layout_row_push(ctx, cw - 34 - 80 - 170);
        nk_label(ctx, desc, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 80);
        nk_label_colored(ctx, probe_reading_name(r->reading), NK_TEXT_LEFT, reading_color(r->reading));
        nk_layout_row_push(ctx, 170);
        /* Where the best fit disagrees, say what it expects. */
        int want = best ? probe_reading_of(probe_group_state(best, r)) : -1;
        if (want >= 0 && probe_reading_cost(probe_group_state(best, r), r) >= 0.5f)
            snprintf(line, sizeof(line), "best fit: %s", probe_reading_name(want));
        else snprintf(line, sizeof(line), "healthy: %s", healthy_on(r->net));
        nk_label_colored(ctx, line, NK_TEXT_LEFT,
                         !strncmp(line, "best", 4) ? COL_FAULT
                         : strcmp(healthy_on(r->net), probe_reading_name(r->reading)) ? COL_PULSE : COL_DIMTEXT);
        nk_layout_row_push(ctx, 26);
        if (nk_button_label(ctx, "x")) { remove_reading(i); nk_layout_row_end(ctx); break; }
        nk_layout_row_end(ctx);
        if (r->reading == PROBE_BAD) {
            char note[1000];
            bad_levels_note(r->net, note, sizeof(note));
            wrapped(ctx, note, cw, COL_FIGHT);
        }
    }
}

static void group(struct nk_context *ctx, int k, float cw, probe_action_t *act) {
    int g = P.order[k];
    const probe_group_t *gr = &P.db.groups[g];
    char line[400], desc[700];
    nk_layout_row_dynamic(ctx, 6, 1);
    nk_spacing(ctx, 1);
    const char *fit = !P.n ? "" : gr->mismatches == 0 && gr->cost < 0.01f ? "fits every reading"
                    : gr->mismatches == 0 ? "near enough to every reading" : NULL;
    char off[64];
    if (!fit) {
        snprintf(off, sizeof(off), "%d reading%s off", gr->mismatches, gr->mismatches == 1 ? "" : "s");
        fit = off;
    }
    int nm = gr->nmatch;
    snprintf(line, sizeof(line), "#%d   %s%s%d fault%s%s", k + 1, fit, fit[0] ? "   " : "", nm, nm == 1 ? "" : "s",
             nm > 1 ? " a probe can't tell apart" : "");
    /* How likely it is, against the others still in the running. */
    if (P.strategy != PROBE_FEWEST && k < P.nrun) {
        float total = 0;
        for (int i = 0; i < P.nrun; i++) total += P.db.groups[P.order[i]].weight;
        if (total > 0) {
            int pct = (int)(100 * gr->weight / total + 0.5f);
            snprintf(line + strlen(line), sizeof(line) - strlen(line), "   (likelihood %s%d%%)", pct < 1 ? "<" : "",
                     pct < 1 ? 1 : pct);
        }
    }
    struct nk_color c = k < P.nbest ? COL_TRACE : k < P.nrun ? COL_PULSE : COL_DIMTEXT;
    wrapped(ctx, line, cw, c);
    /* Where it disagrees with the readings. */
    if (gr->mismatches) {
        char what[300] = "";
        int shown = 0;
        for (int i = 0; i < P.n && shown < 4; i++) {
            const probe_reading_t *r = &P.rd[i];
            if (probe_reading_cost(probe_group_state(gr, r), r) < 0.5f) continue;
            const part_t *pt = &P.b->sim.parts[P.b->sim.pins[r->pin].part];
            size_t len = strlen(what);
            snprintf(what + len, sizeof(what) - len, "%s%s.%s would be %s", shown ? ", " : "", pt->ref,
                     P.b->sim.bd->pads[r->pin].name, probe_reading_name(probe_reading_of(probe_group_state(gr, r))));
            shown++;
        }
        wrapped(ctx, what, cw, COL_DIMTEXT);
    }
    int ncauses = P.expanded[g] || nm <= SHOWN_CAUSES + 1 ? nm : SHOWN_CAUSES;
    /* Each cause: its name and the button on one line, what it is under
     * them (the button stays a button's height however long that is). */
    for (int i = 0, listed = 0; i < gr->nfaults && listed < ncauses; i++) {
        if (!probe_fault_shown(&P.db, gr->faults[i])) continue;
        listed++;
        const atlas_fault_t *f = &P.db.atlas.faults[gr->faults[i]];
        nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
        nk_layout_row_push(ctx, cw - 150 - 8);
        nk_label_colored(ctx, f->spec[0] ? f->spec : "The healthy board", NK_TEXT_LEFT, f->spec[0] ? COL_FAULT : COL_TRACE);
        nk_layout_row_push(ctx, 150);
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, f->spec[0] ? "Repair everything, put this fault on the board, power cycle, and show where it is"
                                       : "Repair everything and power cycle: the healthy board");
        if (nk_button_label(ctx, "Apply & Inspect")) {
            act->apply = true;
            snprintf(act->spec, sizeof(act->spec), "%s", f->spec);
        }
        nk_layout_row_end(ctx);
        /* The healthy board's description is the test's own ("expected: the
         * copyright message"): nothing to add to its name. */
        if (f->spec[0]) wrapped(ctx, one_line(desc, sizeof(desc), f->desc), cw, COL_DIMTEXT);
    }
    if (nm > ncauses || P.expanded[g]) {
        nk_layout_row_static(ctx, 24, 220, 1);
        snprintf(line, sizeof(line), P.expanded[g] ? "Show the first %d" : "Show all %d", P.expanded[g] ? SHOWN_CAUSES : nm);
        if (nk_button_label(ctx, line)) P.expanded[g] = !P.expanded[g];
    }
}

static void results(struct nk_context *ctx, float cw, probe_action_t *act) {
    char line[400];
    int nf = 0;
    for (int k = 0; k < P.nbest; k++) nf += P.db.groups[P.order[k]].nmatch;
    const probe_group_t *top = P.db.ngroups ? &P.db.groups[P.order[0]] : NULL;
    /* The groups that give the screen picked (the others come last). */
    int ngive = 0, ngiving = 0;
    while (ngive < P.db.ngroups && P.db.groups[P.order[ngive]].nmatch) ngiving += P.db.groups[P.order[ngive++]].nmatch;
    if (!P.n)
        snprintf(line, sizeof(line), "No readings yet: any of the %d faults %s could be it (%d groups a "
                 "logic probe can tell apart).", ngiving, P.db.screen_any ? "of the atlas" : P.db.screen >= 0 ? "giving that picture" : "giving those pictures", ngive);
    else if (top && top->healthy && top->cost < 0.01f && P.nbest == 1)
        snprintf(line, sizeof(line), "Every reading so far is what a healthy board shows. Keep probing.");
    else if (top && top->mismatches == 0) {
        int near = P.nrun - P.nbest;
        snprintf(line, sizeof(line), "%d fault%s fit%s the readings best (%d group%s).", nf, nf == 1 ? "" : "s",
                 nf == 1 ? "s" : "", P.nbest, P.nbest == 1 ? "" : "s");
        if (near)
            snprintf(line + strlen(line), sizeof(line) - strlen(line), " %d more group%s %s about one reading off, "
                     "kept in case a reading was wrong.", near, near == 1 ? "" : "s", near == 1 ? "is" : "are");
    }
    else
        snprintf(line, sizeof(line), "No fault of the atlas fits every reading: the closest are listed. Check the "
                 "readings (and that the machine has been on for a few seconds), or it may be a fault the atlas "
                 "doesn't have.");
    wrapped(ctx, line, cw, COL_TEXT);
    int shown = P.shown < ngive ? P.shown : ngive;
    for (int k = 0; k < shown; k++) group(ctx, k, cw, act);
    if (shown < ngive) {
        nk_layout_row_static(ctx, 26, 220, 1);
        if (nk_button_label(ctx, "Show more")) P.shown += SHOWN_GROUPS;
    }
}

void probe_window(struct nk_context *ctx, struct nk_rect area, board_t *b, int machine, probe_action_t *act) {
    memset(act, 0, sizeof(*act));
    act->show_pin = -1;
    if (!P.open) { P.typing = false; return; }
    if (b != P.b || machine != P.machine || (!P.loaded && P.reopen)) attach(b, machine);
    if (P.loaded && P.shot_pending) apply_shot();
    /* A pin clicked on the board: the one to read. */
    if (b->scope_pin != P.last_scope) {
        if (b->scope_pin >= 0 && b->sim.pins[b->scope_pin].net >= 0) P.cur_pin = b->scope_pin;
        P.last_scope = b->scope_pin;
    }
    float w = fminf(560, area.w - 32), h = area.h - 32;
    struct nk_rect r = nk_rect(area.x + 16, area.y + 16, w, h);
    bool fresh = P.reopen;
    if (P.reopen) {
        nk_window_show(ctx, "Probe", NK_SHOWN);
        nk_window_set_bounds(ctx, "Probe", r);
        nk_window_set_focus(ctx, "Probe");
        P.reopen = false;
    }
    if (!nk_begin_titled(ctx, "Probe", "Diagnose by probing", r,
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE |
                         NK_WINDOW_CLOSABLE)) {
        P.open = P.typing = false;
        nk_end(ctx);
        return;
    }
    if (fresh) nk_window_set_scroll(ctx, 0, 0);
    P.bounds = nk_window_get_bounds(ctx);
    float cw = nk_window_get_content_region(ctx).w - 20;
    if (!P.n)       /* how to start: once readings come in, it's under way */
        wrapped(ctx, "Switch the real machine on and wait a few seconds. Probe the pin it asks for and click what "
                     "the logic probe shows: the faults that fit stay at the top.", cw, COL_DIMTEXT);
    if (P.loaded && !P.db.have_legs)
        wrapped(ctx, "This atlas has no readings of the chips' own side of open legs yet (build it again: "
                     "--build-atlas=full): a lifted leg is judged by its pad only.", cw, COL_PULSE);
    if (!P.loaded) {
        wrapped(ctx, P.err, cw, COL_HIGH);
        nk_end(ctx);
        return;
    }
    if (P.db.source != P.machine) {
        char note[256];
        snprintf(note, sizeof(note), "The atlas has no readings of the %s yet: these are the %s's (the same circuit, "
                 "but a fault on a part only one board has is missing).", atlas_machine(P.machine)->board_name,
                 atlas_machine(P.db.source)->board_name);
        wrapped(ctx, note, cw, COL_PULSE);
    }
    screen_choice(ctx, cw);
    strategy_choice(ctx, cw);
    diagnose_caps(ctx, cw, true);
    markings(ctx, cw);
    /* The records changed, or a screenshot matched others. */
    if (P.boost_serial != diagnose_records_serial()) {
        set_weights();
        update();
    }
    if (P.boost_note[0]) wrapped(ctx, P.boost_note, cw, COL_TRACE);
    probe_next(ctx, cw, act);
    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);
    readings(ctx, cw);
    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);
    results(ctx, cw, act);
    nk_end(ctx);
}

void probe_shutdown(void) {
    unload();
    for (int m = 0; m < ATLAS_NMACHINES; m++) { free(P.marks[m]); P.marks[m] = NULL; }
}
