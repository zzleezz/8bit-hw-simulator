/* atlas_browse.c - "Browse the atlas" (see atlas_browse.h).
 *
 * The atlas is read when the window first opens (a few MB, a fraction of
 * a second) and kept, with the filters, until the program ends. A PICTURE here is an outcome of the
 * atlas: what a run ended up showing, with every fault that gives it (its
 * causes). Only normal boot pictures, of the board on the bench; a board
 * the atlas has no pictures of yet gets its family's first board's, the
 * same circuit (as "Diagnose from screenshot" does), and its faults go on
 * the board on the bench.
 *
 * Each picture has a value for every FILTER (a drop-down): its kind and
 * colors from its label (probe_screen_kind(), the probing window's), the
 * border and screen colors from its fingerprint (the color covering most
 * of each), its behavior over time, and, from its causes, what the CPU was
 * doing at the end and the chips they are on (several values: a picture
 * fits a CPU state or a chip if one of its causes does). A drop-down lists
 * how many pictures each of its values leaves with the other filters as
 * they are. Probe readings taken on the real machine narrow them down too:
 * a picture stays if one of its causes shows every reading (as the
 * probing window counts a reading agreeing, probe_cost() under 0.5).
 *
 * The thumbnails are textures made when a page of the grid is drawn and
 * freed when the page or the pictures change: a board has a few hundred
 * pictures, a page shows PAGE of them (thumbtex.h: freed after the frame,
 * which may still draw them). */
#include "atlas_browse.h"
#include "atlas.h"
#include "diagnose.h"
#include "probe_db.h"
#include "thumbtex.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 24                 /* pictures on a page of the grid */
#define SHOWN_CAUSES 8          /* causes listed before "show all" */
#define CELL_W 184              /* a picture of the grid, wide */
#define MAX_READINGS 8

enum { F_KIND, F_BORDER, F_SCREEN, F_BEHAVE, F_CPU, F_CHIP, NF };
static const char *const filter_names[NF] = { "Kind", "Border", "Screen", "Behavior", "CPU", "Chip" };
/* Said above the drop-downs (a tooltip on one would keep its list shut:
 * a window has one popup at a time). */
static const char *const filters_help =
    "Narrow the pictures down by what the screen shows. Border and Screen are the colors covering most of each; "
    "CPU is what it was doing at the end under one of the causes (a probe on the real machine tells: the clock, "
    "reset, the address lines moving); Chip, a part one of the causes is on. Each choice says how many pictures "
    "it leaves.";
enum { B_NONE, B_STILL, B_BLINK, B_CHANGING, B_FLASH, NBEHAVE };
static const char *const behave_names[NBEHAVE] = { "No picture (no video signal)", "Still", "Blinking",
                                                   "Changing (animated)", "Flashing a code" };
static const char *const cpu_names[] = { "running", "held in reset", "stopped (no clock)", "jammed (crashed)" };

/* A value of a filter: its name, and the pictures that have it. */
typedef struct { char name[96]; int total; } value_t;

/* A picture: its outcome, its label, its value of each filter (multi
 * valued filters: a list). */
typedef struct {
    int o;                      /* in A.a.outcomes */
    char label[256];
    int v[NF];                  /* single valued filters */
    int *chips, nchips;         /* F_CHIP values */
    unsigned cpu;               /* F_CPU values, a bit each */
    bool healthy;               /* the healthy board gives it */
    char summary[160];          /* "3 causes: U7, U12" */
    bool probe_ok;              /* one of its causes shows every probe reading */
} pic_t;

static struct {
    bool open, reopen;
    atlas_t a;
    bool loaded;
    char error[300];
    int machine;                /* the board on the bench */
    int shown;                  /* the board whose pictures are listed: MACHINE or its family's */
    pic_t *pics; int npics;
    value_t *vals[NF]; int nvals[NF];
    int sel[NF];                /* the value picked of each filter, -1 any */
    int *list, nlist;           /* the pictures that fit, in order */
    int *count[NF], any[NF];    /* per value of a filter, the pictures it would leave (any: with it "any") */
    int page;
    int picked;                 /* the picture whose causes are shown (in pics), -1 none */
    bool all_causes;
    thumbtex_t *tex;            /* per picture, made when drawn */
    thumbtex_t picked_tex;
    struct nk_rect bounds;
    /* Probe readings: the pin (of the board on the bench), its net in the
     * atlas's NETS of the board shown, what the probe showed; the pin
     * field, the reading to add, what went wrong adding one. */
    struct { int pin, net, reading; } rd[MAX_READINGS];
    int nrd;
    char pin_text[24], pin_note[160];
    int reading;
    bool typing;
} A = { .picked = -1, .reading = 2 };

/* ---- Textures ----------------------------------------------------------- */

static void free_page(void) {
    for (int i = 0; A.tex && i < A.npics; i++) thumbtex_free(&A.tex[i]);
}

/* The picture of outcome O (the lit one of a flash code), as a texture;
 * nothing for snow. */
static void tex_make(thumbtex_t *t, const atlas_outcome_t *o) {
    if (t->live) return;
    bool alt = o->h.alt_enc != THUMB_NONE && o->h.alt_len;
    if (o->h.enc == THUMB_NONE && !alt) return;
    int w = o->h.w, h = o->h.h;
    uint8_t *idx = malloc((size_t)w * h);
    uint32_t *px = malloc((size_t)w * h * sizeof(uint32_t));
    if (idx && px && atlas_decode_thumb(o, alt, idx)) {
        const uint32_t *pal = atlas_palette(o->h.machine);
        for (int i = 0; i < w * h; i++) px[i] = pal[idx[i] & 15];
        thumbtex_make(t, px, w, h);
    }
    free(idx);
    free(px);
}

/* ---- The pictures and their values -------------------------------------- */

/* NAME's index among the values of filter F (added if new). */
static int value_of(int f, const char *name) {
    for (int i = 0; i < A.nvals[f]; i++) if (!strcmp(A.vals[f][i].name, name)) return i;
    A.vals[f] = realloc(A.vals[f], (size_t)(A.nvals[f] + 1) * sizeof(value_t));
    value_t *v = &A.vals[f][A.nvals[f]];
    snprintf(v->name, sizeof(v->name), "%s", name);
    v->total = 0;
    return A.nvals[f]++;
}

/* The color covering most of a histogram, -1 if none does. */
static int main_color(const uint8_t hist[16]) {
    int best = -1, most = 0;
    for (int i = 0; i < 16; i++) if (hist[i] > most) { most = hist[i]; best = i; }
    return best;
}

static int behavior(const atlas_outcome_t *o) {
    const atlas_temporal_t *t = &o->h.t;
    if (t->no_signal) return B_NONE;
    if (t->nseries) return B_FLASH;
    if (t->distinct <= 1) return B_STILL;
    return t->blink_period ? B_BLINK : B_CHANGING;
}

/* The part a fault spec is on: "U7.12:gnd", "U13.11-12:short", "U4:flip=..." */
static void spec_ref(const char *spec, char *ref, int len) {
    snprintf(ref, (size_t)len, "%.*s", (int)strcspn(spec, ".: "), spec);
}

/* A part's name for the chip drop-down: "U7 (CPU)". */
static void part_name(int machine, const char *ref, char *out, int len) {
    const machine_t *m = atlas_machine(machine);
    for (int i = 0; i < m->nparts; i++)
        if (!strcmp(m->parts[i].ref, ref)) {
            char l[64];
            snprintf(out, (size_t)len, "%s (%s)", ref, one_line(l, sizeof(l), m->parts[i].label));
            return;
        }
    snprintf(out, (size_t)len, "%s", ref);
}

static void free_pics(void) {
    free_page();
    thumbtex_free(&A.picked_tex);
    for (int i = 0; i < A.npics; i++) free(A.pics[i].chips);
    free(A.pics);
    free(A.tex);
    free(A.list);
    A.pics = NULL;
    A.tex = NULL;
    A.list = NULL;
    A.npics = A.nlist = 0;
    for (int f = 0; f < NF; f++) {
        free(A.vals[f]);
        free(A.count[f]);
        A.vals[f] = NULL;
        A.count[f] = NULL;
        A.nvals[f] = A.any[f] = 0;
        A.sel[f] = -1;
    }
    A.picked = -1;
    A.page = 0;
    A.nrd = 0;                  /* pins of another board */
    A.pin_note[0] = 0;
}

static int cmp_pics(const void *x, const void *y) {
    const pic_t *a = x, *b = y;
    if (a->healthy != b->healthy) return a->healthy ? -1 : 1;
    int na = A.a.outcomes[a->o].nfaults, nb = A.a.outcomes[b->o].nfaults;
    return na != nb ? nb - na : strcmp(a->label, b->label);
}

/* Values most pictures have first. */
static int cmp_values(const void *x, const void *y) {
    const value_t *a = x, *b = y;
    return a->total != b->total ? b->total - a->total : strcmp(a->name, b->name);
}

/* The pictures of MACHINE's normal boot, with their values. */
static void collect(int machine) {
    free_pics();
    A.machine = A.shown = machine;
    if (!A.loaded) return;
    for (int pass = 0; pass < 2 && !A.npics; pass++) {
        if (pass) A.shown = atlas_family(machine);
        if (pass && A.shown == machine) break;
        for (int o = 0; o < A.a.noutcomes; o++) {
            const atlas_outcome_t *oc = &A.a.outcomes[o];
            if (oc->h.machine != A.shown || oc->h.mode != MODE_BOOT || !oc->nfaults) continue;
            A.pics = realloc(A.pics, (size_t)(A.npics + 1) * sizeof(pic_t));
            pic_t *p = &A.pics[A.npics++];
            memset(p, 0, sizeof(*p));
            p->o = o;
            atlas_outcome_label(oc, p->label, sizeof(p->label));
            for (int k = 0; k < oc->nfaults; k++) if (!A.a.faults[oc->faults[k]].spec[0]) p->healthy = true;
        }
    }
    qsort(A.pics, (size_t)A.npics, sizeof(pic_t), cmp_pics);
    A.tex = calloc((size_t)A.npics + 1, sizeof(thumbtex_t));
    A.list = malloc(((size_t)A.npics + 1) * sizeof(int));
    for (int i = 0; i < A.npics; i++) {
        pic_t *p = &A.pics[i];
        const atlas_outcome_t *oc = &A.a.outcomes[p->o];
        char kind[96], colors[96], name[96];
        probe_screen_kind(p->label, kind, sizeof(kind), colors, sizeof(colors));
        p->v[F_KIND] = value_of(F_KIND, kind);
        int beh = behavior(oc);
        p->v[F_BEHAVE] = value_of(F_BEHAVE, behave_names[beh]);
        int bc = beh == B_NONE ? -1 : main_color(oc->h.fp.border_hist);
        int sc = beh == B_NONE ? -1 : main_color(oc->h.fp.active_hist);
        p->v[F_BORDER] = value_of(F_BORDER, bc < 0 ? "none (no picture)" : atlas_color_name(A.shown, bc));
        p->v[F_SCREEN] = value_of(F_SCREEN, sc < 0 ? "none (no picture)" : atlas_color_name(A.shown, sc));
        /* From its causes: CPU states, chips, and the summary. */
        int len = snprintf(p->summary, sizeof(p->summary), "%d cause%s", oc->nfaults, oc->nfaults == 1 ? "" : "s");
        int listed = 0;
        for (int k = 0; k < oc->nfaults; k++) {
            const atlas_fault_t *ft = &A.a.faults[oc->faults[k]];
            if (ft->h.cpu < 4) p->cpu |= 1u << ft->h.cpu;
            if (!ft->spec[0]) continue;     /* the healthy board */
            for (const char *s = ft->spec; *s; ) {
                size_t sl = strcspn(s, " ");
                char ref[32];
                spec_ref(s, ref, sizeof(ref));
                part_name(A.shown, ref, name, sizeof(name));
                int v = value_of(F_CHIP, name), j = 0;
                while (j < p->nchips && p->chips[j] != v) j++;
                if (j == p->nchips) {
                    p->chips = realloc(p->chips, (size_t)(p->nchips + 1) * sizeof(int));
                    p->chips[p->nchips++] = v;
                    if (listed < 4 && len < (int)sizeof(p->summary) - 40)
                        len += snprintf(p->summary + len, sizeof(p->summary) - (size_t)len, "%s%s", listed ? ", " : ": ", ref);
                    else if (listed == 4)
                        len += snprintf(p->summary + len, sizeof(p->summary) - (size_t)len, ", ...");
                    listed++;
                }
                s += sl + (s[sl] == ' ');
            }
        }
        if (p->healthy) snprintf(p->summary + len, sizeof(p->summary) - (size_t)len, "%s", listed ? " (and healthy)" : ": healthy");
        for (int c = 0; c < 4; c++) if (p->cpu & (1u << c)) value_of(F_CPU, cpu_names[c]);
    }
    /* Each value's pictures, then the values most pictures have first:
     * the pictures' values are indices, so they are mapped after. */
    for (int f = 0; f < NF; f++) {
        int n = A.nvals[f], *map = malloc(((size_t)n + 1) * sizeof(int));
        value_t *old = malloc(((size_t)n + 1) * sizeof(value_t));
        for (int i = 0; i < A.npics; i++) {
            pic_t *p = &A.pics[i];
            if (f == F_CHIP) for (int j = 0; j < p->nchips; j++) A.vals[f][p->chips[j]].total++;
            else if (f == F_CPU) {
                for (int c = 0; c < 4; c++)
                    if (p->cpu & (1u << c)) A.vals[f][value_of(F_CPU, cpu_names[c])].total++;
            } else A.vals[f][p->v[f]].total++;
        }
        memcpy(old, A.vals[f], (size_t)n * sizeof(value_t));
        qsort(A.vals[f], (size_t)n, sizeof(value_t), cmp_values);
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) if (!strcmp(old[i].name, A.vals[f][j].name)) map[i] = j;
        for (int i = 0; i < A.npics; i++) {
            pic_t *p = &A.pics[i];
            if (f == F_CHIP) for (int j = 0; j < p->nchips; j++) p->chips[j] = map[p->chips[j]];
            else if (f != F_CPU) p->v[f] = map[p->v[f]];
        }
        free(map);
        free(old);
        A.count[f] = calloc((size_t)n + 1, sizeof(int));
    }
}

/* Does picture P have value V of filter F? */
static bool has(const pic_t *p, int f, int v) {
    if (f == F_CHIP) {
        for (int j = 0; j < p->nchips; j++) if (p->chips[j] == v) return true;
        return false;
    }
    if (f == F_CPU) {
        for (int c = 0; c < 4; c++)
            if ((p->cpu & (1u << c)) && !strcmp(A.vals[F_CPU][v].name, cpu_names[c])) return true;
        return false;
    }
    return p->v[f] == v;
}

/* Does P fit every filter picked, but SKIP's (-1 none), and the probe
 * readings? */
static bool fits(const pic_t *p, int skip) {
    if (!p->probe_ok) return false;
    for (int f = 0; f < NF; f++) if (f != skip && A.sel[f] >= 0 && !has(p, f, A.sel[f])) return false;
    return true;
}

/* Which pictures one of whose causes shows every probe reading. */
static void apply_readings(void) {
    int nn = A.a.nnet_names[A.shown];
    for (int i = 0; i < A.npics; i++) {
        pic_t *p = &A.pics[i];
        const atlas_outcome_t *oc = &A.a.outcomes[p->o];
        p->probe_ok = !A.nrd;
        for (int k = 0; k < oc->nfaults && !p->probe_ok; k++) {
            const atlas_fault_t *ft = &A.a.faults[oc->faults[k]];
            if (!ft->nets || ft->nnets != nn) continue;
            bool all = true;
            for (int r = 0; r < A.nrd && all; r++) all = probe_cost(ft->nets[A.rd[r].net], A.rd[r].reading) < 0.5f;
            p->probe_ok = all;
        }
    }
}

/* What the healthy board shows on atlas net NET (PROBE_*, -1 unknown). */
static int healthy_reading(int net) {
    for (int i = 0; i < A.a.nfaults; i++) {
        const atlas_fault_t *ft = &A.a.faults[i];
        if (ft->h.machine == A.shown && ft->h.mode == MODE_BOOT && !ft->spec[0] && ft->nets &&
            net < ft->nnets)
            return probe_reading_of(ft->nets[net]);
    }
    return -1;
}

/* The pictures that fit; the one picked stays picked if it does. */
static void filter(void) {
    free_page();
    apply_readings();
    A.nlist = 0;
    for (int i = 0; i < A.npics; i++) if (fits(&A.pics[i], -1)) A.list[A.nlist++] = i;
    A.page = 0;
    /* What each drop-down's values would leave, with the other filters
     * as they are. */
    int cpu_val[4];
    for (int c = 0; c < 4; c++) {          /* (value_of() would add a state no picture has) */
        cpu_val[c] = -1;
        for (int v = 0; v < A.nvals[F_CPU]; v++) if (!strcmp(A.vals[F_CPU][v].name, cpu_names[c])) cpu_val[c] = v;
    }
    for (int f = 0; f < NF; f++) {
        A.any[f] = 0;
        if (A.count[f]) memset(A.count[f], 0, (size_t)A.nvals[f] * sizeof(int));
    }
    for (int i = 0; i < A.npics; i++) {
        const pic_t *p = &A.pics[i];
        for (int f = 0; f < NF; f++) {
            if (!A.count[f] || !fits(p, f)) continue;
            A.any[f]++;
            if (f == F_CHIP) for (int j = 0; j < p->nchips; j++) A.count[f][p->chips[j]]++;
            else if (f == F_CPU) { for (int c = 0; c < 4; c++) if ((p->cpu & (1u << c)) && cpu_val[c] >= 0) A.count[f][cpu_val[c]]++; }
            else A.count[f][p->v[f]]++;
        }
    }
    if (A.picked >= 0 && !fits(&A.pics[A.picked], -1)) {
        A.picked = -1;
        thumbtex_free(&A.picked_tex);
    }
}

/* ---- The window --------------------------------------------------------- */

static bool load(void) {
    if (A.loaded) return true;
    char path[2100];
    diagnose_atlas_file(path, sizeof(path));
    bool exists = false;
    if (!atlas_load(&A.a, path, &exists) || !exists) {
        snprintf(A.error, sizeof(A.error), exists ? "%s isn't a fault atlas" : "No fault atlas: %s isn't there", path);
        atlas_free(&A.a);
        return false;
    }
    A.error[0] = 0;
    A.loaded = true;
    return true;
}

void atlas_browse_open(int machine) {
    if (load() && (!A.pics || machine != A.machine)) { collect(machine); filter(); }
    A.machine = machine;
    A.open = A.reopen = true;
}

bool atlas_browse_wants_keyboard(void) { return A.open && A.typing; }

void atlas_browse_close(void) {
    A.open = false;
    free_page();
    thumbtex_free(&A.picked_tex);
}

bool atlas_browse_is_open(void) { return A.open; }

void atlas_browse_pick(int n) {
    if (n < 0 || n >= A.nlist) return;
    A.picked = A.list[n];
    A.all_causes = false;
    thumbtex_free(&A.picked_tex);
}

bool atlas_browse_bounds(struct nk_rect *r) {
    if (!A.open) return false;
    *r = A.bounds;
    return true;
}

/* A row tall enough for TEXT wrapped in WIDTH. */
static float wrapped_height(struct nk_context *ctx, const char *text, float width) {
    float fh = ctx->style.font->height;
    int lines = (int)(text_width(ctx, text) / fmaxf(width - 12, 40)) + 1;
    return fmaxf(22, lines * (fh + 3) + 6);
}

/* The drop-down of filter F: "any", then every value with the pictures it
 * leaves (those that leave none but the one picked aren't listed). */
static void filter_combo(struct nk_context *ctx, int f) {
    char cur[160], item[200];
    if (A.sel[f] < 0) snprintf(cur, sizeof(cur), "%s: any", filter_names[f]);
    else snprintf(cur, sizeof(cur), "%s: %s", filter_names[f], A.vals[f][A.sel[f]].name);
    float h = fminf(420, 26.0f * (A.nvals[f] + 2) + 8);
    struct nk_rect wb = nk_widget_bounds(ctx);   /* wider than the widget, inside the window */
    float pw = fminf(wb.w * 1.6f, A.bounds.x + A.bounds.w - wb.x - 8);
    if (!nk_combo_begin_label(ctx, cur, nk_vec2(fmaxf(pw, wb.w), h))) return;
    nk_layout_row_dynamic(ctx, 22, 1);
    int any = A.any[f];
    snprintf(item, sizeof(item), "any   (%d picture%s)", any, any == 1 ? "" : "s");
    if (nk_combo_item_label(ctx, item, NK_TEXT_LEFT) && A.sel[f] >= 0) { A.sel[f] = -1; filter(); }
    for (int v = 0; v < A.nvals[f]; v++) {
        int n = A.count[f] ? A.count[f][v] : 0;
        if (!n && v != A.sel[f]) continue;
        snprintf(item, sizeof(item), "%s   (%d)", A.vals[f][v].name, n);
        if (nk_combo_item_label(ctx, item, NK_TEXT_LEFT) && A.sel[f] != v) {
            A.sel[f] = v;
            filter();
        }
    }
    nk_combo_end(ctx);
}

static void thumb(struct nk_context *ctx, thumbtex_t *t, const atlas_outcome_t *o, bool button, bool *clicked) {
    tex_make(t, o);
    if (t->live) {
        struct nk_image img = thumbtex_image(t);
        if (button) { if (nk_button_image(ctx, img)) *clicked = true; }
        else nk_image(ctx, img);
    } else if (button) {
        if (nk_button_label(ctx, "(no picture: snow)")) *clicked = true;
    } else nk_label_colored(ctx, "(no picture: snow)", NK_TEXT_CENTERED, COL_DIMTEXT);
}

/* The picture picked: big, what is known of it, its causes. */
static void details(struct nk_context *ctx, float cw, atlas_browse_action_t *act) {
    pic_t *p = &A.pics[A.picked];
    const atlas_outcome_t *o = &A.a.outcomes[p->o];
    char line[400], desc[700];
    float tw = o->h.w ? (float)o->h.w : 196, th = o->h.h ? (float)o->h.h : 60;
    nk_layout_row_begin(ctx, NK_STATIC, fmaxf(th, 180), 2);
    nk_layout_row_push(ctx, tw);
    bool unused = false;
    thumb(ctx, &A.picked_tex, o, false, &unused);
    nk_layout_row_push(ctx, cw - tw - 10);
    if (nk_group_begin(ctx, "atlas picked", NK_WINDOW_NO_SCROLLBAR)) {
        float hw = cw - tw - 24;
        nk_layout_row_dynamic(ctx, wrapped_height(ctx, p->label, hw), 1);
        nk_label_colored_wrap(ctx, p->label, COL_TRACE);
        nk_layout_row_dynamic(ctx, 20, 1);
        snprintf(line, sizeof(line), "%d possible cause%s, all giving this picture", o->nfaults, o->nfaults == 1 ? "" : "s");
        nk_label(ctx, line, NK_TEXT_LEFT);
        int len = snprintf(line, sizeof(line), "The CPU at the end:");
        for (int c = 0, n = 0; c < 4; c++)
            if (p->cpu & (1u << c)) len += snprintf(line + len, sizeof(line) - (size_t)len, "%s %s", n++ ? "," : "", cpu_names[c]);
        nk_label(ctx, line, NK_TEXT_LEFT);
        if (p->healthy) nk_label_colored(ctx, "A healthy board shows this picture too.", NK_TEXT_LEFT, COL_TRACE);
        if (o->h.t.nseries) nk_label_colored(ctx, "A flash code: count the flashes on the machine.", NK_TEXT_LEFT, COL_PULSE);
        nk_layout_row_static(ctx, 28, 210, 2);
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, "Open \"Diagnose by probing\" with only this picture's faults: it asks for the pins that tell "
                            "them apart");
        if (nk_button_label(ctx, "Narrow down by probing...")) {
            act->probe = true;
            snprintf(act->label, sizeof(act->label), "%s", p->label);
        }
        if (nk_button_label(ctx, "Back to the pictures")) { A.picked = -1; thumbtex_free(&A.picked_tex); }
        nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);
    if (A.picked < 0) return;
    int shown = A.all_causes || o->nfaults <= SHOWN_CAUSES + 2 ? o->nfaults : SHOWN_CAUSES;
    float dw = cw - 150 - 130 - 16;
    for (int i = 0; i < shown; i++) {
        const atlas_fault_t *ft = &A.a.faults[o->faults[i]];
        int n = 0;
        for (const char *s = ft->desc; *s && n < (int)sizeof(desc) - 3; s++) {
            if (*s == '\n') { desc[n++] = ','; desc[n++] = ' '; }
            else desc[n++] = *s;
        }
        desc[n] = 0;
        nk_layout_row_begin(ctx, NK_STATIC, wrapped_height(ctx, desc, dw), 3);
        nk_layout_row_push(ctx, 150);
        if (ft->spec[0] && !atlas_same_refs(A.shown, A.machine)) {
            /* Another board's parts (the 250407's for the 250469): shown, not applied. */
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "A fault of the other board: its parts are named otherwise on this one");
            nk_label_colored(ctx, atlas_machine(A.shown)->issue, NK_TEXT_CENTERED, COL_DIMTEXT);
        } else {
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, ft->spec[0] ? "Repair everything, put this fault on the board, power cycle, and show where it is"
                                            : "Repair everything and power cycle: the healthy board");
            if (nk_button_label(ctx, "Apply & Inspect")) {
                act->apply = true;
                act->machine = A.machine;
                snprintf(act->spec, sizeof(act->spec), "%s", ft->spec);
            }
        }
        nk_layout_row_push(ctx, 130);
        nk_label_colored(ctx, ft->spec[0] ? ft->spec : "(healthy)", NK_TEXT_LEFT, COL_FAULT);
        nk_layout_row_push(ctx, dw);
        nk_label_wrap(ctx, desc);
        nk_layout_row_end(ctx);
    }
    if (o->nfaults > shown || A.all_causes) {
        nk_layout_row_static(ctx, 24, 220, 1);
        snprintf(line, sizeof(line), A.all_causes ? "Show the first %d causes" : "Show all %d causes",
                 A.all_causes ? SHOWN_CAUSES : o->nfaults);
        if (nk_button_label(ctx, line)) A.all_causes = !A.all_causes;
    }
    nk_layout_row_dynamic(ctx, 10, 1);
    nk_spacing(ctx, 1);
}

/* TEXT cut to fit WIDTH, "..." at the end if it was. */
static const char *fit(struct nk_context *ctx, const char *text, float width, char *out, int len) {
    snprintf(out, (size_t)len, "%s", text);
    int n = (int)strlen(out);
    if (text_width(ctx, out) <= width) return out;
    while (n > 0) {
        snprintf(out + n, (size_t)(len - n), "...");
        if (text_width(ctx, out) <= width) break;
        out[--n] = 0;
    }
    return out;
}

/* A page of the grid: the pictures, each with its label and causes. */
static void grid(struct nk_context *ctx, float cw) {
    int cols = (int)fmaxf(1, cw / (CELL_W + 8));
    int first = A.page * PAGE, last = first + PAGE < A.nlist ? first + PAGE : A.nlist;
    float fh = ctx->style.font->height;
    for (int r = first; r < last; r += cols) {
        int n = r + cols < last ? cols : last - r;
        /* The pictures, scaled to the cell (same aspect: the board's). */
        float th = 0;
        for (int k = 0; k < n; k++) {
            const atlas_outcome_t *o = &A.a.outcomes[A.pics[A.list[r + k]].o];
            if (o->h.w) th = fmaxf(th, CELL_W * (float)o->h.h / o->h.w);
        }
        if (th < 40) th = 60;
        nk_layout_row_static(ctx, th + 8, CELL_W, cols);
        for (int k = 0; k < n; k++) {
            int i = A.list[r + k];
            bool clicked = false;
            if (nk_widget_is_hovered(ctx)) {
                char tip[440];
                snprintf(tip, sizeof(tip), "%s; %s. Click: its causes", A.pics[i].label, A.pics[i].summary);
                nk_tooltip(ctx, tip);
            }
            thumb(ctx, &A.tex[i], &A.a.outcomes[A.pics[i].o], true, &clicked);
            if (clicked) {
                A.picked = i;
                A.all_causes = false;
                thumbtex_free(&A.picked_tex);
                nk_window_set_scroll(ctx, 0, 0);
            }
        }
        nk_layout_row_static(ctx, 2 * (fh + 2) + 4, CELL_W, cols);
        for (int k = 0; k < n; k++) {
            const pic_t *p = &A.pics[A.list[r + k]];
            nk_label_colored_wrap(ctx, p->label, p->healthy ? COL_TRACE : COL_TEXT);
        }
        nk_layout_row_static(ctx, fh + 4, CELL_W, cols);
        for (int k = 0; k < n; k++) {
            char cut[160];
            nk_label_colored(ctx, fit(ctx, A.pics[A.list[r + k]].summary, CELL_W - 6, cut, sizeof(cut)), NK_TEXT_LEFT,
                             COL_DIMTEXT);
        }
        nk_layout_row_dynamic(ctx, 6, 1);
        nk_spacing(ctx, 1);
    }
}

/* "U7.3", "U7 3", "u7-3": a pin of board B, -1 if none (as the probing
 * window reads one). */
static int parse_pin(board_t *b, const char *text) {
    char ref[16], pad[8];
    if (sscanf(text, " %15[^.: -]%*[.: -]%7s", ref, pad) != 2) return -1;
    for (char *c = ref; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    int part = sim_find_part(&b->sim, ref);
    return part >= 0 ? board_pad_pin(b, part, pad) : -1;
}

/* Add the reading of the pin field. */
static void add_reading(board_t *b) {
    int pin = parse_pin(b, A.pin_text), net = pin >= 0 ? b->sim.pins[pin].net : -1, an = -1;
    const char *name = net >= 0 ? b->sim.nets[net].name : NULL;
    for (int i = 0; name && i < A.a.nnet_names[A.shown] && an < 0; i++)
        if (!strcmp(A.a.net_names[A.shown][i], name)) an = i;
    if (pin < 0) snprintf(A.pin_note, sizeof(A.pin_note), "No pin %s on this board (write it like U7.3)", A.pin_text);
    else if (an < 0) snprintf(A.pin_note, sizeof(A.pin_note), "The atlas has no probe readings of %s's net", A.pin_text);
    else {
        int r = 0;
        while (r < A.nrd && A.rd[r].pin != pin) r++;
        if (r == MAX_READINGS) {
            snprintf(A.pin_note, sizeof(A.pin_note), "That's as many readings as it takes");
            return;
        }
        if (r == A.nrd) A.nrd++;        /* the same pin again: the new reading */
        A.rd[r].pin = pin;
        A.rd[r].net = an;
        A.rd[r].reading = A.reading;
        A.pin_text[0] = A.pin_note[0] = 0;
        filter();
    }
}

/* The probe readings: the pin field, what the probe shows, the readings
 * taken, each with what a healthy board shows there. */
static void readings(struct nk_context *ctx, float cw, board_t *b) {
    static const char *const shows[PROBE_NREADINGS] = { "low", "high", "pulsing", "floating / no light", "bad levels" };
    nk_layout_row_begin(ctx, NK_STATIC, 26, 5);
    nk_layout_row_push(ctx, 150);
    nk_label(ctx, "Probe reading at pin:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 100);
    if (nk_widget_is_hovered(ctx))
        nk_tooltip(ctx, "A pin of the real machine, probed on the solder side: U7.3, IC5.20... (Enter adds it)");
    nk_flags ev = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, A.pin_text,
                                                 sizeof(A.pin_text), nk_filter_ascii);
    A.typing = (ev & NK_EDIT_ACTIVE) != 0;
    nk_layout_row_push(ctx, 170);
    A.reading = nk_combo(ctx, shows, PROBE_NREADINGS, A.reading, 22, nk_vec2(170, 155));
    nk_layout_row_push(ctx, 70);
    bool add = nk_button_label(ctx, "Add") || (ev & NK_EDIT_COMMITED);
    nk_layout_row_push(ctx, cw - 150 - 100 - 170 - 70 - 30);
    nk_label_colored(ctx, A.pin_note[0] ? A.pin_note : A.nrd ? "" : "a picture stays if one of its causes shows them all",
                     NK_TEXT_LEFT, A.pin_note[0] ? COL_HIGH : COL_DIMTEXT);
    nk_layout_row_end(ctx);
    if (add && A.pin_text[0]) add_reading(b);
    int removed = -1;
    for (int r = 0; r < A.nrd; r++) {
        char desc[160], line[300];
        pin_desc(b, A.rd[r].pin, desc, sizeof(desc));
        int h = healthy_reading(A.rd[r].net);
        snprintf(line, sizeof(line), "%s: %s%s%s", desc, probe_reading_name(A.rd[r].reading),
                 h >= 0 ? "   (a healthy board: " : "", h >= 0 ? probe_reading_name(h) : "");
        if (h >= 0) strcat(line, ")");
        nk_layout_row_begin(ctx, NK_STATIC, 22, 2);
        nk_layout_row_push(ctx, 90);
        if (nk_button_label(ctx, "remove")) removed = r;
        nk_layout_row_push(ctx, cw - 100);
        nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_HOVER);
        nk_layout_row_end(ctx);
    }
    if (removed >= 0) {
        A.rd[removed] = A.rd[--A.nrd];
        filter();
    }
}

void atlas_browse_window(struct nk_context *ctx, struct nk_rect area, board_t *b, int machine,
                         atlas_browse_action_t *act) {
    memset(act, 0, sizeof(*act));
    A.typing = false;
    if (!A.open) return;
    if (A.loaded && machine != A.machine) { collect(machine); filter(); }
    float w = fminf(1060, area.w - 32), h = area.h - 32;
    struct nk_rect r = nk_rect(area.x + 16, area.y + 16, w, h);
    bool fresh = A.reopen;
    if (A.reopen) {
        nk_window_show(ctx, "AtlasBrowse", NK_SHOWN);
        nk_window_set_bounds(ctx, "AtlasBrowse", r);
        nk_window_set_focus(ctx, "AtlasBrowse");
        A.reopen = false;
    }
    if (!nk_begin_titled(ctx, "AtlasBrowse", "Browse the fault atlas", r,
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE |
                         NK_WINDOW_CLOSABLE)) {
        nk_end(ctx);
        atlas_browse_close();
        return;
    }
    if (fresh) nk_window_set_scroll(ctx, 0, 0);
    A.bounds = nk_window_get_bounds(ctx);
    float cw = nk_window_get_content_region(ctx).w - 20;
    char line[400];

    if (!A.loaded) {
        nk_layout_row_dynamic(ctx, wrapped_height(ctx, A.error, cw), 1);
        nk_label_colored_wrap(ctx, A.error, COL_HIGH);
        nk_end(ctx);
        return;
    }
    /* Whose pictures, how many fit. */
    const machine_t *bm = atlas_machine(A.machine);
    int len = snprintf(line, sizeof(line), "The pictures of the %s (the board on the bench), booting normally", bm->board_name);
    if (A.shown != A.machine)
        snprintf(line + len, sizeof(line) - (size_t)len, ": none of it in the atlas yet, the %s's shown (the same circuit)",
                 atlas_machine(A.shown)->issue ? atlas_machine(A.shown)->issue : atlas_machine(A.shown)->name);
    nk_layout_row_dynamic(ctx, wrapped_height(ctx, line, cw), 1);
    nk_label_colored_wrap(ctx, line, A.shown != A.machine ? COL_HOVER : COL_TEXT);
    if (!A.npics) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label_colored(ctx, "The atlas has no pictures of this board.", NK_TEXT_LEFT, COL_HIGH);
        nk_end(ctx);
        return;
    }
    /* The filters: three to a row. */
    nk_layout_row_dynamic(ctx, wrapped_height(ctx, filters_help, cw), 1);
    nk_label_colored_wrap(ctx, filters_help, COL_DIMTEXT);
    nk_layout_row_dynamic(ctx, 26, 3);
    for (int f = 0; f < NF; f++) filter_combo(ctx, f);
    readings(ctx, cw, b);
    int picked = A.nrd;
    for (int f = 0; f < NF; f++) picked += A.sel[f] >= 0;
    int pages = (A.nlist + PAGE - 1) / PAGE;
    nk_layout_row_begin(ctx, NK_STATIC, 26, 5);
    nk_layout_row_push(ctx, cw - 3 * 110 - 130 - 30);
    snprintf(line, sizeof(line), "%d of %d pictures%s", A.nlist, A.npics, picked ? "" : " (most causes first)");
    nk_label(ctx, line, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 130);
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Every filter back to \"any\", the probe readings removed");
    if (nk_button_label(ctx, "Clear filters") && picked) {
        for (int f = 0; f < NF; f++) A.sel[f] = -1;
        A.nrd = 0;
        filter();
    }
    nk_layout_row_push(ctx, 110);
    if (nk_button_label(ctx, "< Previous") && A.page > 0) { free_page(); A.page--; nk_window_set_scroll(ctx, 0, 0); }
    nk_layout_row_push(ctx, 110);
    snprintf(line, sizeof(line), "page %d / %d", pages ? A.page + 1 : 0, pages);
    nk_label(ctx, line, NK_TEXT_CENTERED);
    nk_layout_row_push(ctx, 110);
    if (nk_button_label(ctx, "Next >") && A.page + 1 < pages) { free_page(); A.page++; nk_window_set_scroll(ctx, 0, 0); }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);

    if (A.picked >= 0) details(ctx, cw, act);
    grid(ctx, cw);
    nk_end(ctx);
}

void atlas_browse_shutdown(void) {
    free_pics();
    if (A.loaded) atlas_free(&A.a);
    A.loaded = false;
}
