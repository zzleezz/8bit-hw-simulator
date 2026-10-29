/* probe.c - "Diagnose by probing" (see probe.h, and probe_db.h for the
 * matching). The atlas is read when the window first opens for a machine
 * (a fraction of a second) and kept; the readings belong to the machine on
 * the bench and go when it changes. */
#include "probe.h"
#include "probe_db.h"
#include "atlas.h"
#include "diagnose.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

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
} P = { .sug_net = -1, .sug_pin = -1, .cur_pin = -1, .last_scope = -1 };

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
    P.sug_net = probe_suggest(&P.db, P.b, P.rd, P.n, P.order, P.nrun, &P.sug_pin, P.counts, &P.sug_bench);
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
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, tips[P.strategy]);
    int s = nk_combo(ctx, names, PROBE_NSTRATEGIES, P.strategy, 22, nk_vec2(cw - 146, 100));
    nk_layout_row_end(ctx);
    if (s != P.strategy) {
        P.strategy = P.db.strategy = s;
        update();
    }
}

/* What is printed on the chips of the real machine: the maker moves how
 * likely each is to be the fault. */
static void markings(struct nk_context *ctx, float cw) {
    if (!nk_tree_push(ctx, NK_TREE_TAB, "Chip markings on the real board", NK_MINIMIZED)) return;
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

static void add_reading(int pin, int reading) {
    int net = pin >= 0 ? P.b->sim.pins[pin].net : -1;
    if (net < 0 || P.n >= MAX_READINGS) return;     /* a leg on no net (NC) tells nothing */
    /* Probing the same net again: the new reading replaces the old one. */
    for (int i = 0; i < P.n; i++)
        if (P.rd[i].net == net) {
            memmove(&P.rd[i], &P.rd[i + 1], (size_t)(P.n - i - 1) * sizeof(probe_reading_t));
            P.n--;
            break;
        }
    P.rd[P.n++] = (probe_reading_t){ pin, net, reading };
    update();
}

static void remove_reading(int i) {
    memmove(&P.rd[i], &P.rd[i + 1], (size_t)(P.n - i - 1) * sizeof(probe_reading_t));
    P.n--;
    update();
}

/* "U7.3", "U7 3", "U7-3", "u7.3": a pin of the board, -1 if none. */
static int parse_pin(board_t *b, const char *text) {
    char ref[16], pad[8];
    if (sscanf(text, " %15[^.: -]%*[.: -]%7s", ref, pad) != 2) return -1;
    for (char *c = ref; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    int part = sim_find_part(&b->sim, ref);
    return part >= 0 ? board_pad_pin(b, part, pad) : -1;
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
    return r == PROBE_LOW ? COL_LOW : r == PROBE_HIGH ? COL_HIGH : r == PROBE_PULSING ? COL_PULSE : COL_FLOAT;
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
    bool *on = calloc((size_t)P.db.nscreens + 1, sizeof(bool));
    for (int s = 0; s < P.db.nscreens; s++) on[s] = P.exact >= 0 ? s == P.exact : P.kind >= 0 && screen_fits(s);
    probe_db_set_screens(&P.db, on);
    free(on);
    P.from_shot = false;
    update();
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

/* The screenshot's pictures, the faults being loaded: any of them,
 * however it moves. */
static void apply_shot(void) {
    P.shot_pending = false;
    bool *on = calloc((size_t)P.db.nscreens + 1, sizeof(bool));
    P.shot_found = 0;
    char still[ATLAS_MAX_GROUPS][256], a[256];
    for (int i = 0; i < P.nshot; i++) still_label(P.shot[i], still[i], sizeof(still[i]));
    for (int s = 0; s < P.db.nscreens; s++) {
        still_label(P.db.screens[s].label, a, sizeof(a));
        for (int i = 0; i < P.nshot; i++)
            if (!strcmp(a, still[i])) {
                if (!P.shot_found || i < P.shot_first) P.shot_first = i;
                on[s] = true;
                P.shot_found++;
                break;
            }
    }
    if (P.shot_found) {
        P.kind = P.colors = P.exact = -1;
        probe_db_set_screens(&P.db, on);
        P.from_shot = true;
        update();
    }
    free(on);
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
        snprintf(line, sizeof(line), "%s %s", pin == P.sug_pin ? "Probe next:" : "Probing:", desc);
        wrapped(ctx, line, cw, COL_HOVER);
        if (pin == P.sug_pin && P.sug_bench) {
            snprintf(line, sizeof(line), "Bench procedure, step %d: %s", P.sug_bench,
                     probe_bench_step(P.machine, P.sug_bench));
            wrapped(ctx, line, cw, COL_TRACE);
        }
        snprintf(line, sizeof(line), "net %s; a healthy board shows %s", net >= 0 ? P.b->sim.nets[net].name : "-",
                 healthy_on(net)[0] ? healthy_on(net) : "(not known)");
        wrapped(ctx, line, cw, COL_DIMTEXT);
        if (pin == P.sug_pin) {
            int nk = 0;
            for (int r = 0; r < PROBE_NREADINGS; r++) nk += P.counts[r];
            snprintf(line, sizeof(line), "Splits the %d groups of suspects: low %d, high %d, pulsing %d, floating %d", nk,
                     P.counts[PROBE_LOW], P.counts[PROBE_HIGH], P.counts[PROBE_PULSING], P.counts[PROBE_FLOATING]);
            wrapped(ctx, line, cw, COL_DIMTEXT);
        }
        /* What the probe shows: the reading buttons. */
        static const char *const labels[PROBE_NREADINGS] = { "Low", "High", "Pulsing", "Floating / no light" };
        static const int at[PROBE_NREADINGS] = { PROBE_LOW, PROBE_HIGH, PROBE_PULSING, PROBE_FLOATING };
        nk_layout_row_begin(ctx, NK_STATIC, 30, 4);
        for (int i = 0; i < PROBE_NREADINGS; i++) {
            nk_layout_row_push(ctx, i == 3 ? 170 : 90);
            struct nk_color c = reading_color(at[i]);
            nk_style_push_color(ctx, &ctx->style.button.text_normal, ink(c));
            nk_style_push_color(ctx, &ctx->style.button.text_hover, ink(c));
            if (nk_button_label(ctx, labels[i])) add_reading(pin, at[i]);
            nk_style_pop_color(ctx);
            nk_style_pop_color(ctx);
        }
        nk_layout_row_end(ctx);
    }
    /* Another pin: shown on the board, typed, or clicked there. */
    nk_layout_row_begin(ctx, NK_STATIC, 28, 4);
    nk_layout_row_push(ctx, 120);
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Zoom the board in on this pin");
    if (nk_button_label(ctx, "Show on board") && pin >= 0) act->show_pin = pin;
    nk_layout_row_push(ctx, 80);
    nk_label_colored(ctx, "or type:", NK_TEXT_RIGHT, COL_DIMTEXT);
    nk_layout_row_push(ctx, 90);
    nk_flags ev = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, P.pin_text,
                                                 sizeof(P.pin_text), nk_filter_ascii);
    P.typing = (ev & NK_EDIT_ACTIVE) != 0;
    nk_layout_row_push(ctx, 50);
    bool go = nk_button_label(ctx, "Go") || (ev & NK_EDIT_COMMITED);
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
    wrapped(ctx, P.pin_note[0] ? P.pin_note : "or click a pin on the board", cw,
            P.pin_note[0] ? COL_HIGH : COL_DIMTEXT);
    if (pin >= 0 && pin != P.sug_pin && P.sug_pin >= 0) {
        nk_layout_row_static(ctx, 26, 240, 1);
        pin_desc(P.b, P.sug_pin, desc, sizeof(desc));
        snprintf(line, sizeof(line), "Back to the suggestion");
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, desc);
        if (nk_button_label(ctx, line)) { P.cur_pin = P.sug_pin; act->show_pin = P.sug_pin; }
    }
}

/* The readings so far, each with what a healthy board shows there. */
static void readings(struct nk_context *ctx, float cw) {
    char line[300], desc[160];
    nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
    nk_layout_row_push(ctx, cw - 200);
    snprintf(line, sizeof(line), "Readings: %d", P.n);
    nk_label(ctx, line, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 90);
    if (nk_button_label(ctx, "Undo") && P.n) remove_reading(P.n - 1);
    nk_layout_row_push(ctx, 90);
    if (nk_button_label(ctx, "Clear all") && P.n) { P.n = 0; update(); }
    nk_layout_row_end(ctx);
    const probe_group_t *best = P.db.ngroups ? &P.db.groups[P.order[0]] : NULL;
    for (int i = 0; i < P.n; i++) {
        const probe_reading_t *r = &P.rd[i];
        pin_desc(P.b, r->pin, desc, sizeof(desc));
        nk_layout_row_begin(ctx, NK_STATIC, 22, 4);
        nk_layout_row_push(ctx, cw - 34 - 80 - 170);
        nk_label(ctx, desc, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 80);
        nk_label_colored(ctx, probe_reading_name(r->reading), NK_TEXT_LEFT, reading_color(r->reading));
        nk_layout_row_push(ctx, 170);
        /* Where the best fit disagrees, say what it expects. */
        int want = best ? probe_reading_of(best->nets[r->net]) : -1;
        if (want >= 0 && probe_cost(best->nets[r->net], r->reading) >= 0.5f)
            snprintf(line, sizeof(line), "best fit: %s", probe_reading_name(want));
        else snprintf(line, sizeof(line), "healthy: %s", healthy_on(r->net));
        nk_label_colored(ctx, line, NK_TEXT_LEFT,
                         !strncmp(line, "best", 4) ? COL_FAULT
                         : strcmp(healthy_on(r->net), probe_reading_name(r->reading)) ? COL_PULSE : COL_DIMTEXT);
        nk_layout_row_push(ctx, 26);
        if (nk_button_label(ctx, "x")) { remove_reading(i); nk_layout_row_end(ctx); break; }
        nk_layout_row_end(ctx);
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
            if (probe_cost(gr->nets[r->net], r->reading) < 0.5f) continue;
            const part_t *pt = &P.b->sim.parts[P.b->sim.pins[r->pin].part];
            size_t len = strlen(what);
            snprintf(what + len, sizeof(what) - len, "%s%s.%s would be %s", shown ? ", " : "", pt->ref,
                     P.b->sim.bd->pads[r->pin].name, probe_reading_name(probe_reading_of(gr->nets[r->net])));
            shown++;
        }
        wrapped(ctx, what, cw, COL_DIMTEXT);
    }
    int ncauses = P.expanded[g] || nm <= SHOWN_CAUSES + 1 ? nm : SHOWN_CAUSES;
    float dw = cw - 150 - 110 - 16;
    for (int i = 0, listed = 0; i < gr->nfaults && listed < ncauses; i++) {
        if (!probe_fault_shown(&P.db, gr->faults[i])) continue;
        listed++;
        const atlas_fault_t *f = &P.db.atlas.faults[gr->faults[i]];
        nk_layout_row_begin(ctx, NK_STATIC, wrapped_height(ctx, one_line(desc, sizeof(desc), f->desc), dw), 3);
        nk_layout_row_push(ctx, 150);
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, f->spec[0] ? "Repair everything, put this fault on the board, power cycle, and show where it is"
                                       : "Repair everything and power cycle: the healthy board");
        if (nk_button_label(ctx, "Apply & Inspect")) {
            act->apply = true;
            snprintf(act->spec, sizeof(act->spec), "%s", f->spec);
        }
        nk_layout_row_push(ctx, 110);
        nk_label_colored(ctx, f->spec[0] ? f->spec : "(healthy)", NK_TEXT_LEFT, COL_FAULT);
        nk_layout_row_push(ctx, dw);
        nk_label_wrap(ctx, desc);
        nk_layout_row_end(ctx);
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
    wrapped(ctx, "Switch the real machine on and wait a few seconds. Probe each pin on the solder side, where pad and "
                 "trace are one, and click what the logic probe shows. The faults that fit stay at the top.",
            cw, COL_DIMTEXT);
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
