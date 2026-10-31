/* diagnose.c - "Diagnose from screenshot" (see diagnose.h).
 *
 * The matching is atlas_match_run() (atlas_match.c), against the normal
 * boot groups only (a machine that can run a diagnostic cartridge says
 * itself what is wrong) of the machine on the bench only: whoever
 * diagnoses a machine has picked it already, and the two machines'
 * garbage screens can look much alike. Another machine on the bench, the
 * picture is matched again. It takes a fraction of a second, done when the
 * picture comes in. The pictures (the screenshot, each group's simulated
 * one) are textures of their own, made then and freed with the results
 * (thumbtex.h: after the frame, which may still draw them). */
#include "diagnose.h"
#include "atlas.h"
#include "c64.h"
#include "filedialog.h"
#include "repairs.h"
#include "thumbtex.h"
#include "zx48.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define NTEX (1 + ATLAS_MAX_GROUPS)
#define SHOWN_CAUSES 6          /* causes listed before "show all" */
#define BEST_MARGIN 0.02f       /* matches this close to the best are as good (the score's noise) */
#define REAL_MIN 0.75f          /* a real repair this alike is shown */
#define REAL_SHOWN 3

static struct {
    char atlas[1024], carts[1024], repairs[1024];
    char path[4096];            /* the picture, "" none yet */
    int machine;                /* what it is matched against (the board on the bench) */
    int matched;                /* the board whose pictures it was matched with: MACHINE, or
                                   the family's first when the atlas has none of it yet */
    bool open, reopen;
    atlas_result_t r;
    bool ok;                    /* r holds results (else r.error says why) */
    thumbtex_t tex[NTEX];       /* [0] the screenshot, [1 + k] group k */
    bool all[ATLAS_MAX_GROUPS]; /* every cause of the group listed */
    struct nk_rect bounds;
    /* The real repair records (repairs.h), and whether they are matched. */
    repairs_t rep;
    bool rep_loaded, use_real;
    int real_rec[ATLAS_MAX_REALS];          /* D.r.reals[k]: the record, in rep.r */
    thumbtex_t real_tex[ATLAS_MAX_REALS];
    /* Recording a repair: a small window of its own while the chips are
     * clicked on the board. */
    bool recording, rec_reopen, typing;
    repair_chip_t rec_chips[REPAIR_MAX_CHIPS];
    int rec_n, rec_cert;
    char rec_notes[1024], rec_owner[64], rec_msg[300];
    struct nk_rect rec_bounds;
    bool headless;              /* diagnose_cli(): no window, no textures */
    /* The window of all the records: look, leave out, edit, delete,
     * merge and export. */
    bool list_open, list_reopen, list_dirty;
    thumbtex_t *list_tex;       /* one per record, made when the list changes */
    int nlist_tex;
    int edit, confirm;          /* the record being edited, the one Delete was clicked on once (-1) */
    int edit_cert;
    char edit_notes[1024], list_msg[400];
    struct nk_rect list_bounds;
    unsigned rec_serial;        /* diagnose_records_serial() */
    int bench;                  /* the board on the bench (diagnose_set_machine()) */
    int rec_machine;            /* the board of the record being made */
    bool rec_noshot;            /* ... recorded without a screenshot */
    char chip[16];              /* the records window shows only the records of this chip... */
    int chip_family;            /* ... on boards of this family */
    /* Gaps in the simulator (find_gaps()): per record, how the atlas does
     * on its screenshot; checked when asked, forgotten when the records
     * change. */
    struct gap { uint8_t state; int rank; float score, best; char best_label[256]; } *gaps;
    int ngaps;
} D;

/* struct gap.state: its chips are causes of the best match (or one as
 * good), of a lower one, of none of the matches; no screenshot; not read. */
enum { GAP_FOUND, GAP_LOWER, GAP_MISSING, GAP_NOSHOT, GAP_FAILED };

void diagnose_init(const char *atlas_path, const char *carts) {
    snprintf(D.atlas, sizeof(D.atlas), "%s", atlas_path ? atlas_path : "fault_atlas.bin");
    snprintf(D.carts, sizeof(D.carts), "%s", carts ? carts : "carts");
}

bool diagnose_is_picture(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") ||
                   !strcasecmp(dot, ".bmp"));
}

static void free_textures(void) {
    for (int i = 0; i < NTEX; i++) thumbtex_free(&D.tex[i]);
    for (int i = 0; i < ATLAS_MAX_REALS; i++) thumbtex_free(&D.real_tex[i]);
}

/* The atlas board of a machine_t.id, -1 if none. */
static int machine_index(const char *id) {
    for (int m = 0; m < ATLAS_NMACHINES; m++)
        if (!strcmp(atlas_machine(m)->id, id)) return m;
    return -1;
}

/* A file or folder next to the program when it isn't where we run from
 * (started from a shortcut, from another folder). stat(), not fopen():
 * Windows can't fopen() a folder (the carts). */
static void beside_exe(const char *name, char *out, int len) {
    snprintf(out, (size_t)len, "%s", name);
    struct stat st;
    if (stat(name, &st) == 0) return;
    char dir[1024];
    if (name[0] != '/' && !(name[0] && name[1] == ':') && filedialog_exe_dir(dir, sizeof(dir)))
        snprintf(out, (size_t)len, "%s/%s", dir, name);
}

void diagnose_atlas_file(char *out, int len) { beside_exe(D.atlas, out, len); }
void diagnose_repairs_file(const char *path) { snprintf(D.repairs, sizeof(D.repairs), "%s", path ? path : ""); }

/* Read the stored screenshots of the records again: those read by an
 * older matcher (ATLAS_READ_VERSION), or with ALL every one. The number
 * read. */
static int reread_records(bool all) {
    char atlas[2100], carts[2100];
    beside_exe(D.atlas, atlas, sizeof(atlas));
    beside_exe(D.carts, carts, sizeof(carts));
    int n = 0;
    for (int i = 0; i < D.rep.n; i++) {
        repair_t *rp = &D.rep.r[i];
        if (!repair_live(rp) || !rp->shot || (!all && rp->read_version >= ATLAS_READ_VERSION)) continue;
        int mi = machine_index(rp->machine);
        if (mi < 0) continue;
        atlas_match_opts_t o = { .path = atlas, .image = rp->shot_name, .image_data = rp->shot,
                                 .image_len = (int)rp->shot_len, .top = 1, .causes = 0, .machine = mi, .cart = 0,
                                 .carts = carts };
        atlas_result_t r;
        if (!atlas_match_run(&o, &r) || !r.norm) {
            atlas_result_free(&r);
            continue;
        }
        repairs_set_reading(&D.rep, i, ATLAS_READ_VERSION, r.norm_machine, r.norm_w, r.norm_h, r.norm_snow, r.norm);
        r.norm = NULL;              /* the record's now */
        atlas_result_free(&r);
        n++;
    }
    return n;
}

/* The repair records, read the first time they are needed (next to the
 * program, like the atlas). */
static void load_repairs(void) {
    if (D.rep_loaded) return;
    char path[2100];
    beside_exe(D.repairs[0] ? D.repairs : "repairs.bin", path, sizeof(path));
    repairs_load(&D.rep, path);
    snprintf(D.rec_owner, sizeof(D.rec_owner), "%s", D.rep.own.owner);
    D.rep_loaded = D.use_real = true;
    reread_records(false);
}

/* A record's picture: its screenshot made small. Not the reading it is
 * matched with (the screen at half its resolution, in the machine's 16
 * colors): fine patterns, bars 2 pixels wide, come out uneven in that. The
 * reading only if the screenshot can't be read. */
static void record_texture(thumbtex_t *t, const repair_t *rp) {
    int w, h;
    uint32_t *px = atlas_small_picture(rp->shot, (int)rp->shot_len, 400, &w, &h);
    if (!px && rp->norm && rp->norm_machine < ATLAS_NMACHINES) {
        const uint32_t *pal = atlas_palette(rp->norm_machine);
        w = rp->norm_w;
        h = rp->norm_h;
        px = malloc((size_t)w * h * sizeof(uint32_t));
        for (int i = 0; i < w * h; i++) px[i] = pal[rp->norm[i] & 15];
    }
    if (px) thumbtex_make(t, px, w, h);
    free(px);
}

/* Match the picture against the faults of D.machine, and against the
 * real repairs of its family. */
static void run(void) {
    free_textures();
    atlas_result_free(&D.r);
    load_repairs();
    char atlas[2100], carts[2100];
    beside_exe(D.atlas, atlas, sizeof(atlas));
    beside_exe(D.carts, carts, sizeof(carts));
    atlas_real_t *reals = calloc((size_t)D.rep.n + 1, sizeof(atlas_real_t));
    int *rec_of = calloc((size_t)D.rep.n + 1, sizeof(int)), nreals = 0;
    for (int i = 0; D.use_real && i < D.rep.n; i++) {
        const repair_t *rp = &D.rep.r[i];
        if (!repair_live(rp) || rp->off) continue;
        if (rp->norm_machine >= ATLAS_NMACHINES || atlas_family(rp->norm_machine) != atlas_family(D.machine)) continue;
        reals[nreals] = (atlas_real_t){ rp->norm_machine, rp->norm, rp->snow };
        rec_of[nreals++] = i;
    }
    atlas_match_opts_t o = { .path = atlas, .image = D.path, .top = 5, .causes = -1, .machine = D.machine,
                             .cart = 0, .carts = carts, .reals = reals, .nreals = nreals };
    D.matched = D.machine;
    D.ok = atlas_match_run(&o, &D.r);
    /* A board the atlas has no pictures of yet (the Issue 2 before its
     * atlas run): the pictures of the family's first board, the same
     * circuit. Its faults go on the board on the bench. */
    if ((!D.ok || !D.r.ngroups) && atlas_family(D.machine) != D.machine) {
        atlas_result_free(&D.r);
        o.machine = D.matched = atlas_family(D.machine);
        D.ok = atlas_match_run(&o, &D.r);
        for (int k = 0; D.ok && k < D.r.ngroups; k++) D.r.groups[k].machine = D.machine;
    }
    for (int k = 0; D.ok && k < D.r.nreals; k++) D.real_rec[k] = rec_of[D.r.reals[k].index];
    D.rec_serial++;
    if (D.ok && !D.headless) {
        thumbtex_make(&D.tex[0], D.r.picture, D.r.w, D.r.h);
        for (int k = 0; k < D.r.ngroups; k++)
            thumbtex_make(&D.tex[1 + k], D.r.groups[k].thumb, D.r.groups[k].w, D.r.groups[k].h);
        /* The real repairs' screenshots. */
        for (int k = 0; k < D.r.nreals; k++) record_texture(&D.real_tex[k], &D.rep.r[D.real_rec[k]]);
    }
    free(reals);
    free(rec_of);
    memset(D.all, 0, sizeof(D.all));
}

void diagnose_open(const char *path, int machine) {
    snprintf(D.path, sizeof(D.path), "%s", path);
    D.machine = machine;
    run();
    D.open = D.reopen = true;
}

void diagnose_set_machine(int machine) {
    if (D.rec_noshot && machine != D.bench) {
        D.rec_machine = machine;
        D.rec_n = 0;            /* chips of the other board */
    }
    D.bench = machine;
    if (!D.path[0] || machine == D.machine) return;
    D.machine = machine;
    D.rec_n = 0;                /* chips of the other board */
    run();
}

void diagnose_close(void) { D.open = false; }
bool diagnose_has_results(void) { return D.ok; }
void diagnose_show(void) { D.open = D.reopen = true; }
bool diagnose_is_open(void) { return D.open; }

int diagnose_best_labels(char (*labels)[256], int max) {
    if (!D.ok || !D.r.ngroups) return 0;
    int n = 0;
    float best = D.r.groups[0].score;
    for (int k = 0; k < D.r.ngroups && n < max; k++)
        if (D.r.groups[k].score >= best - BEST_MARGIN) snprintf(labels[n++], 256, "%s", D.r.groups[k].label);
    return n;
}

bool diagnose_bounds(struct nk_rect *r) {
    if (D.recording) { *r = D.rec_bounds; return true; }
    if (!D.open) return false;
    *r = D.bounds;
    return true;
}

static void save_record(void);

static float wrapped_height(struct nk_context *ctx, const char *text, float width);
static bool original_caps = true;

void diagnose_caps(struct nk_context *ctx, float cw) {
    static const char *const caps[2] = { "Original (never replaced)", "Replaced (recapped)" };
    static const char *const warn =
        "Original electrolytic capacitors: after 40 years many are dried out. Worn capacitors make faults the "
        "simulator doesn't have (ripple on the supplies, a sagging -5V or +12V, a slow or unsteady reset, "
        "picture interference), so these results may not be reliable. Check the supplies with a scope, or "
        "recap the machine, and diagnose again.";
    nk_layout_row_begin(ctx, NK_STATIC, 28, 2);
    nk_layout_row_push(ctx, 140);
    nk_label(ctx, "Electrolytic caps:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, cw - 146 < 300 ? cw - 146 : 300);
    if (nk_widget_is_hovered(ctx))
        nk_tooltip(ctx, "The electrolytic capacitors on the real machine. The simulator's capacitors are always good");
    original_caps = nk_combo(ctx, caps, 2, original_caps ? 0 : 1, 22, nk_vec2(300, 80)) == 0;
    nk_layout_row_end(ctx);
    if (original_caps) {
        nk_layout_row_dynamic(ctx, wrapped_height(ctx, warn, cw), 1);
        nk_label_colored_wrap(ctx, warn, COL_PULSE);
    }
}

/* A picture in its own proportions, at the top left of the cell (nk_image()
 * stretches it over the cell: a 16:9 screenshot beside taller text). */
static void image(struct nk_context *ctx, const thumbtex_t *t) {
    if (!t->live) {
        nk_label_colored(ctx, "(no picture: snow)", NK_TEXT_CENTERED, COL_DIMTEXT);
        return;
    }
    struct nk_rect r;
    if (nk_widget(&r, ctx) == NK_WIDGET_INVALID) return;
    float w = r.w, h = t->w > 0 ? r.w * t->h / t->w : r.h;
    if (h > r.h) { h = r.h; w = t->h > 0 ? h * t->w / t->h : r.w; }
    struct nk_image img = thumbtex_image(t);
    nk_draw_image(nk_window_get_canvas(ctx), nk_rect(r.x, r.y, w, h), &img, nk_rgb(255, 255, 255));
}

/* A row tall enough for TEXT wrapped in WIDTH. */
static float wrapped_height(struct nk_context *ctx, const char *text, float width) {
    float fh = ctx->style.font->height;
    int lines = (int)(text_width(ctx, text) / fmaxf(width - 12, 40)) + 1;
    return fmaxf(26, lines * (fh + 3) + 6);
}

static void group(struct nk_context *ctx, int k, float cw, diagnose_action_t *act) {
    const atlas_group_t *g = &D.r.groups[k];
    const machine_t *m = atlas_machine(g->machine);
    char line[400], desc[700];
    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);
    /* The rank, the confidence, the symptom. */
    struct nk_color c = g->score >= 0.85f ? COL_TRACE : g->score >= 0.7f ? COL_PULSE : COL_DIMTEXT;
    snprintf(line, sizeof(line), "#%d   %.0f%%   %s", k + 1, 100 * g->score, g->label);
    nk_layout_row_dynamic(ctx, wrapped_height(ctx, line, cw), 1);
    nk_label_colored_wrap(ctx, line, c);

    /* Its picture as simulated, and what to know about it. */
    float tw = g->thumb ? (float)g->w : 196, th = g->thumb ? (float)g->h : 60;
    nk_layout_row_begin(ctx, NK_STATIC, th, 2);
    nk_layout_row_push(ctx, tw);
    image(ctx, &D.tex[1 + k]);
    nk_layout_row_push(ctx, cw - tw - 10);
    char gname[32];
    snprintf(gname, sizeof(gname), "diag group %d", k);
    if (nk_group_begin(ctx, gname, NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, 20, 1);
        snprintf(line, sizeof(line), "%s, normal boot (no cartridge)", m->name);
        nk_label(ctx, line, NK_TEXT_LEFT);
        snprintf(line, sizeof(line), "%d possible cause%s, all giving this picture", g->ncauses, g->ncauses == 1 ? "" : "s");
        nk_label(ctx, line, NK_TEXT_LEFT);
        if (g->healthy) nk_label_colored(ctx, "A healthy board shows this picture too.", NK_TEXT_LEFT, COL_TRACE);
        if (g->same_as >= 0) {
            snprintf(line, sizeof(line), "The very same picture as #%d: a screenshot can't tell them apart.", g->same_as + 1);
            nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_PULSE);
        }
        if (g->flash) nk_label_colored(ctx, "A flash code: count the flashes on the machine.", NK_TEXT_LEFT, COL_PULSE);
        if (atlas_same_refs(D.matched, D.machine))
            nk_label_colored(ctx, "Apply one to see it happen on the board and probe it.", NK_TEXT_LEFT, COL_DIMTEXT);
        else {
            snprintf(line, sizeof(line), "The causes name the %s's parts: they can't go on this board.",
                     atlas_machine(D.matched)->board_name);
            nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_PULSE);
        }
        nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);

    /* The causes, each with its button. */
    int shown = D.all[k] || g->ncauses <= SHOWN_CAUSES + 2 ? g->ncauses : SHOWN_CAUSES;
    float dw = cw - 150 - 120 - 16;
    for (int i = 0; i < shown; i++) {
        const atlas_cause_t *cs = &g->causes[i];
        nk_layout_row_begin(ctx, NK_STATIC, wrapped_height(ctx, one_line(desc, sizeof(desc), cs->desc), dw), 3);
        nk_layout_row_push(ctx, 150);
        if (cs->suspect) {
            /* Nothing to put on the board: the simulator can't break it so. */
            nk_label_colored(ctx, "not simulated", NK_TEXT_CENTERED, COL_DIMTEXT);
        } else if (cs->spec[0] && !atlas_same_refs(D.matched, D.machine)) {
            /* Another board's parts (the 250407's for the 250469): shown, not applied. */
            nk_label_colored(ctx, atlas_machine(D.matched)->issue, NK_TEXT_CENTERED, COL_DIMTEXT);
        } else {
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, cs->spec[0] ? "Repair everything, put this fault on the board, power cycle, and show where it is"
                                            : "Repair everything and power cycle: the healthy board");
            if (nk_button_label(ctx, "Apply & Inspect")) {
                act->apply = true;
                act->machine = g->machine;
                snprintf(act->spec, sizeof(act->spec), "%s", cs->spec);
            }
        }
        nk_layout_row_push(ctx, 120);
        nk_label_colored(ctx, cs->spec[0] ? cs->spec : "(healthy)", NK_TEXT_LEFT, COL_FAULT);
        nk_layout_row_push(ctx, dw);
        nk_label_wrap(ctx, desc);
        nk_layout_row_end(ctx);
    }
    if (g->ncauses > shown || D.all[k]) {
        nk_layout_row_static(ctx, 24, 220, 1);
        snprintf(line, sizeof(line), D.all[k] ? "Show the first %d causes" : "Show all %d causes",
                 D.all[k] ? SHOWN_CAUSES : g->ncauses);
        if (nk_button_label(ctx, line)) D.all[k] = !D.all[k];
    }
}

/* The real repairs that look like the screenshot, above the simulated
 * matches: what was found faulty on a real board. */
static void real_matches(struct nk_context *ctx, float cw) {
    char line[600];
    int shown = 0;
    for (int k = 0; k < D.r.nreals && shown < REAL_SHOWN; k++) {
        if (D.r.reals[k].score < REAL_MIN) break;
        const repair_t *rp = &D.rep.r[D.real_rec[k]];
        if (!shown++) {
            nk_layout_row_dynamic(ctx, 8, 1);
            nk_spacing(ctx, 1);
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label_colored(ctx, "Seen on real boards (repair records):", NK_TEXT_LEFT, COL_TRACE);
        }
        /* The chips, and what each is. */
        int n = snprintf(line, sizeof(line), "%.0f%%   Faulty: ", 100 * D.r.reals[k].score);
        for (int i = 0; i < rp->nchips && n < (int)sizeof(line) - 1; i++)
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%s%s (%s)", i ? ", " : "", rp->chips[i].ref,
                          rp->chips[i].what);
        const thumbtex_t *tt = &D.real_tex[k];
        float tw = 240, th = tt->live && tt->w ? tw * tt->h / tt->w : rp->norm_h;
        nk_layout_row_begin(ctx, NK_STATIC, fmaxf(th, 90), 2);
        nk_layout_row_push(ctx, tw);
        image(ctx, &D.real_tex[k]);
        nk_layout_row_push(ctx, cw - tw - 10);
        char gname[32];
        snprintf(gname, sizeof(gname), "diag real %d", k);
        if (nk_group_begin(ctx, gname, 0)) {
            float gw = cw - tw - 34;
            nk_layout_row_dynamic(ctx, wrapped_height(ctx, line, gw), 1);
            nk_label_colored_wrap(ctx, line, D.r.reals[k].score >= 0.85f ? COL_TRACE : COL_PULSE);
            char when[32] = "?";
            time_t t = (time_t)rp->when;
            struct tm *tm = localtime(&t);
            if (tm) strftime(when, sizeof(when), "%Y-%m-%d", tm);
            int mi = machine_index(rp->machine);
            const char *owner = repairs_owner(&D.rep, rp->dataset);
            snprintf(line, sizeof(line), "%s; %s; recorded %s by %s; screenshot %s",
                     mi >= 0 ? atlas_machine(mi)->board_name : rp->machine, repairs_certainty_name(rp->certainty),
                     when, owner[0] ? owner : "(no name)", rp->shot_name);
            nk_layout_row_dynamic(ctx, wrapped_height(ctx, line, gw), 1);
            nk_label_colored_wrap(ctx, line, COL_DIMTEXT);
            if (rp->notes[0]) {
                nk_layout_row_dynamic(ctx, wrapped_height(ctx, rp->notes, gw), 1);
                nk_label_wrap(ctx, rp->notes);
            }
            nk_group_end(ctx);
        }
        nk_layout_row_end(ctx);
    }
}

/* The screenshot's file, whole (NULL if it can't be read). */
static uint8_t *read_file(const char *path, uint32_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    *len = d ? (uint32_t)n : 0;
    return d;
}

static void save_record(void) {
    repair_t rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.machine, sizeof(rec.machine), "%s", atlas_machine(D.rec_machine)->id);
    rec.certainty = (uint8_t)D.rec_cert;
    rec.nchips = D.rec_n;
    memcpy(rec.chips, D.rec_chips, sizeof(rec.chips));
    snprintf(rec.notes, sizeof(rec.notes), "%s", D.rec_notes);
    rec.read_version = ATLAS_READ_VERSION;
    if (D.rec_noshot) {
        rec.norm_machine = 255;     /* no picture: never matched, still counted */
    } else {
        const char *base = strrchr(D.path, '/'), *base2 = strrchr(D.path, '\\');
        if (base2 > base) base = base2;
        snprintf(rec.shot_name, sizeof(rec.shot_name), "%.127s", base ? base + 1 : D.path);
        rec.shot = read_file(D.path, &rec.shot_len);
        if (!rec.shot) {
            snprintf(D.rec_msg, sizeof(D.rec_msg), "Can't read the screenshot %.200s again: not saved.", D.path);
            return;
        }
        rec.norm_machine = D.r.norm_machine;
        rec.norm_w = (uint16_t)D.r.norm_w;
        rec.norm_h = (uint16_t)D.r.norm_h;
        rec.snow = D.r.norm_snow;
        rec.norm = malloc((size_t)D.r.norm_w * D.r.norm_h);
        memcpy(rec.norm, D.r.norm, (size_t)D.r.norm_w * D.r.norm_h);
    }
    /* The name goes with the dataset: written once, or when it changes. */
    bool ok = true;
    if (!D.rep.own.id) snprintf(D.rep.own.owner, sizeof(D.rep.own.owner), "%s", D.rec_owner);
    else if (strcmp(D.rep.own.owner, D.rec_owner)) ok = repairs_set_owner(&D.rep, D.rec_owner);
    ok = ok && repairs_add(&D.rep, &rec);
    if (!ok) {
        free(rec.shot);
        free(rec.norm);
        snprintf(D.rec_msg, sizeof(D.rec_msg), "Not saved: %.280s", D.rep.error);
        return;
    }
    D.recording = false;
    D.typing = false;
    D.list_dirty = true;
    D.rec_serial++;
    if (D.rec_noshot) {
        D.rec_noshot = false;
        D.list_open = D.list_reopen = true;
        snprintf(D.list_msg, sizeof(D.list_msg), "Record saved (no screenshot).");
        return;
    }
    D.open = D.reopen = true;
    run();                      /* it shows among the matches now */
}

void diagnose_record_cancel(void) {
    D.recording = D.typing = false;
    if (D.rec_noshot) D.list_open = D.list_reopen = true;
    else D.open = D.reopen = true;
    D.rec_noshot = false;
}

bool diagnose_recording(void) { return D.recording; }
bool diagnose_wants_keyboard(void) { return (D.recording || D.list_open) && D.typing; }

bool diagnose_record_marked(const char *ref) {
    for (int i = 0; D.recording && i < D.rec_n; i++)
        if (!strcmp(D.rec_chips[i].ref, ref)) return true;
    return false;
}

void diagnose_record_toggle(const char *ref, const char *what) {
    if (!D.recording) return;
    for (int i = 0; i < D.rec_n; i++)
        if (!strcmp(D.rec_chips[i].ref, ref)) {
            memmove(&D.rec_chips[i], &D.rec_chips[i + 1], (size_t)(D.rec_n - i - 1) * sizeof(repair_chip_t));
            D.rec_n--;
            return;
        }
    if (D.rec_n == REPAIR_MAX_CHIPS) {
        snprintf(D.rec_msg, sizeof(D.rec_msg), "At most %d chips in a record.", REPAIR_MAX_CHIPS);
        return;
    }
    snprintf(D.rec_chips[D.rec_n].ref, sizeof(D.rec_chips[0].ref), "%s", ref);
    snprintf(D.rec_chips[D.rec_n++].what, sizeof(D.rec_chips[0].what), "%s", what);
    D.rec_msg[0] = 0;
}

/* ------------------------------------------------------------------------ */
/* The window of all the records                                             */
/* ------------------------------------------------------------------------ */

static void free_list_textures(void) {
    for (int i = 0; i < D.nlist_tex; i++) thumbtex_free(&D.list_tex[i]);
    free(D.list_tex);
    D.list_tex = NULL;
    D.nlist_tex = 0;
}

static void make_list_textures(void) {
    free_list_textures();
    D.list_tex = calloc((size_t)D.rep.n + 1, sizeof(thumbtex_t));
    D.nlist_tex = D.rep.n;
    for (int i = 0; i < D.rep.n; i++) {
        record_texture(&D.list_tex[i], &D.rep.r[i]);
    }
    D.list_dirty = false;
}

/* The records changed: the list's pictures, and the diagnosis matched again. */
static void records_changed(void) {
    free(D.gaps);
    D.gaps = NULL;
    D.ngaps = 0;
    D.list_dirty = true;
    D.rec_serial++;
    if (D.ok && D.path[0]) run();
}

/* A record naming chip REF on a board of FAMILY. */
static bool names_chip(const repair_t *rp, int family, const char *ref) {
    int mi = machine_index(rp->machine);
    if (mi < 0 || atlas_family(mi) != family) return false;
    for (int k = 0; k < rp->nchips; k++)
        if (!strcmp(rp->chips[k].ref, ref)) return true;
    return false;
}

int diagnose_records_of_chip(int machine, const char *ref) {
    load_repairs();
    int n = 0;
    for (int i = 0; i < D.rep.n; i++) n += repair_live(&D.rep.r[i]) && names_chip(&D.rep.r[i], atlas_family(machine), ref);
    return n;
}

void diagnose_records_open_chip(int bench, const char *ref) {
    diagnose_records_open(bench);
    snprintf(D.chip, sizeof(D.chip), "%s", ref);
    D.chip_family = atlas_family(bench);
}

void diagnose_records_open(int bench) {
    D.bench = bench;
    D.chip[0] = 0;
    load_repairs();
    D.list_open = D.list_reopen = D.list_dirty = true;
    D.edit = D.confirm = -1;
    D.list_msg[0] = 0;
}

void diagnose_records_close(void) {
    D.list_open = false;
    D.edit = D.confirm = -1;
    D.typing = D.recording && D.typing;
    free_list_textures();
}

bool diagnose_records_bounds(struct nk_rect *r) {
    if (!D.list_open) return false;
    *r = D.list_bounds;
    return true;
}

void diagnose_records_file(int what, const char *path) {
    load_repairs();
    if (what == 1) {
        int added, updated, skipped;
        if (repairs_merge(&D.rep, path, &added, &updated, &skipped))
            snprintf(D.list_msg, sizeof(D.list_msg), "Merged %.200s: %d new, %d newer versions, %d already here.", path,
                     added, updated, skipped);
        else snprintf(D.list_msg, sizeof(D.list_msg), "Not merged: %.300s", D.rep.error);
        records_changed();
    } else {
        if (repairs_export(&D.rep, path, what == 3))
            snprintf(D.list_msg, sizeof(D.list_msg), "%s written to %.250s%s%.80s", what == 3 ? "All the records" :
                     "Your records", path, D.rep.error[0] ? ": " : "", D.rep.error);
        else snprintf(D.list_msg, sizeof(D.list_msg), "Not exported: %.300s", D.rep.error);
    }
}

unsigned diagnose_records_serial(void) { return D.rec_serial; }

int diagnose_record_boost(board_t *b, int machine, float *boost, char *note, int len) {
    sim_t *s = &b->sim;
    for (int i = 0; i < s->nparts; i++) boost[i] = 1;
    note[0] = 0;
    load_repairs();
    if (!D.use_real) return 0;
    float *all = calloc((size_t)s->nparts, sizeof(float)), *like = calloc((size_t)s->nparts, sizeof(float));
    bool *shown = calloc((size_t)D.rep.n + 1, sizeof(bool));
    /* The records the last screenshot of this board matched. */
    for (int k = 0; D.ok && D.matched >= 0 && atlas_family(D.matched) == atlas_family(machine) && k < D.r.nreals; k++)
        if (D.r.reals[k].score >= REAL_MIN) shown[D.real_rec[k]] = true;
    for (int i = 0; i < D.rep.n; i++) {
        const repair_t *rp = &D.rep.r[i];
        int mi = machine_index(rp->machine);
        if (!repair_live(rp) || rp->off || mi < 0 || atlas_family(mi) != atlas_family(machine)) continue;
        /* Not sure which of several: each counts half. */
        float c = rp->certainty == REPAIR_UNSURE ? 0.5f : 1;
        for (int k = 0; k < rp->nchips; k++) {
            int part = sim_find_part(s, rp->chips[k].ref);
            if (part < 0) continue;
            all[part] += c;
            if (shown[i]) like[part] += c;
        }
    }
    int n = 0, at = snprintf(note, (size_t)len, "Repair records move up:");
    for (int i = 0; i < s->nparts; i++) {
        if (!all[i]) continue;
        boost[i] = (1 + fminf(2, 0.25f * all[i])) * (1 + 3 * like[i]);
        n++;
        const char *label = board_part_label(b, i);
        char like_text[48] = "";
        if (like[i]) snprintf(like_text, sizeof(like_text), ", %g like this screen", like[i]);
        if (at < len - 1)
            at += snprintf(note + at, (size_t)(len - at), "%s %s%s%s x%.1f (%g on real boards%s)", n > 1 ? "," : "",
                           s->parts[i].ref, label ? " " : "", label ? label : "", boost[i], all[i], like_text);
    }
    if (!n) note[0] = 0;
    free(all);
    free(like);
    free(shown);
    return n;
}

/* One record's line of chips. */
static void chips_text(const repair_t *rp, char *out, int len) {
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < rp->nchips && n < len - 1; i++)
        n += snprintf(out + n, (size_t)(len - n), "%s%s (%s)", i ? ", " : "", rp->chips[i].ref, rp->chips[i].what);
}

static void wrapped_label(struct nk_context *ctx, const char *text, float width, struct nk_color col) {
    nk_layout_row_dynamic(ctx, wrapped_height(ctx, text, width), 1);
    nk_label_colored_wrap(ctx, text, col);
}

/* Each dataset's records used in the diagnosis, or left out, at once. */
static void datasets_part(struct nk_context *ctx, float cw) {
    if (!nk_tree_push(ctx, NK_TREE_TAB, "Datasets (whose records)", NK_MINIMIZED)) return;
    for (int d = 0; d < D.rep.nsets; d++) {
        uint64_t id = D.rep.sets[d].id;
        int n = 0, off = 0;
        for (int i = 0; i < D.rep.n; i++)
            if (D.rep.r[i].dataset == id && repair_live(&D.rep.r[i])) { n++; off += D.rep.r[i].off; }
        if (!n) continue;
        char line[200];
        snprintf(line, sizeof(line), "%s%s: %d record%s%s", D.rep.sets[d].owner[0] ? D.rep.sets[d].owner : "(no name)",
                 id == D.rep.own.id ? " (you)" : "", n, n == 1 ? "" : "s",
                 off == n ? ", all left out" : off ? ", some left out" : "");
        nk_layout_row_begin(ctx, NK_STATIC, 24, 3);
        nk_layout_row_push(ctx, cw - 360);
        nk_label(ctx, line, NK_TEXT_LEFT);
        int set = -1;
        nk_layout_row_push(ctx, 160);
        if (nk_button_label(ctx, "Use them all")) set = 0;
        nk_layout_row_push(ctx, 160);
        if (nk_button_label(ctx, "Leave them all out")) set = 1;
        nk_layout_row_end(ctx);
        if (set < 0) continue;
        for (int i = 0; i < D.rep.n; i++)
            if (D.rep.r[i].dataset == id && repair_live(&D.rep.r[i]) && D.rep.r[i].off != (set == 1) &&
                !repairs_set_off(&D.rep, i, set == 1))
                snprintf(D.list_msg, sizeof(D.list_msg), "%.390s", D.rep.error);
        records_changed();
    }
    nk_tree_pop(ctx);
}

/* Does a cause of the atlas name a chip of the record? A spec names its
 * parts before the '.' or ':' ("U13.11-12:short", "IC2:removed"), several
 * space separated; the ULA as a suspect (inside, not at its pins) names
 * the chip that is the ULA. */
static bool cause_names_record(const atlas_cause_t *c, const repair_t *rp) {
    for (int i = 0; i < rp->nchips; i++) {
        if (c->suspect) {
            if (!strncmp(rp->chips[i].what, "ULA", 3)) return true;
            continue;
        }
        size_t n = strlen(rp->chips[i].ref);
        for (const char *s = c->spec; *s; ) {
            size_t len = strcspn(s, " ");
            if (strcspn(s, ".: ") == n && !strncmp(s, rp->chips[i].ref, n)) return true;
            s += len + (s[len] == ' ');
        }
    }
    return false;
}

/* Gaps in the simulator: each record's screenshot matched against the
 * atlas as a diagnosis would, and where the chips it found faulty come
 * among the matches' causes. A confirmed repair whose chips no match
 * names is a fault the simulator doesn't have (or doesn't show as the
 * real board did). */
static void find_gaps(void) {
    char atlas[2100], carts[2100];
    beside_exe(D.atlas, atlas, sizeof(atlas));
    beside_exe(D.carts, carts, sizeof(carts));
    free(D.gaps);
    D.ngaps = D.rep.n;
    D.gaps = calloc((size_t)D.rep.n + 1, sizeof(*D.gaps));
    for (int i = 0; i < D.rep.n; i++) {
        const repair_t *rp = &D.rep.r[i];
        struct gap *g = &D.gaps[i];
        int mi = machine_index(rp->machine);
        g->rank = -1;
        if (!rp->shot) { g->state = GAP_NOSHOT; continue; }
        g->state = GAP_FAILED;
        if (!repair_live(rp) || mi < 0) continue;
        atlas_match_opts_t o = { .path = atlas, .image = rp->shot_name, .image_data = rp->shot,
                                 .image_len = (int)rp->shot_len, .top = ATLAS_MAX_GROUPS, .causes = -1,
                                 .machine = mi, .cart = 0, .carts = carts };
        atlas_result_t r;
        bool ok = atlas_match_run(&o, &r);
        if ((!ok || !r.ngroups) && atlas_family(mi) != mi) {     /* as run() does */
            atlas_result_free(&r);
            o.machine = atlas_family(mi);
            ok = atlas_match_run(&o, &r);
        }
        if (ok && r.ngroups) {
            g->best = r.groups[0].score;
            snprintf(g->best_label, sizeof(g->best_label), "%s", r.groups[0].label);
            for (int k = 0; k < r.ngroups && g->rank < 0; k++)
                for (int c = 0; c < r.groups[k].ncauses; c++)
                    if (cause_names_record(&r.groups[k].causes[c], rp)) {
                        g->rank = k;
                        g->score = r.groups[k].score;
                        break;
                    }
            g->state = g->rank < 0 ? GAP_MISSING : g->score >= g->best - BEST_MARGIN ? GAP_FOUND : GAP_LOWER;
        }
        atlas_result_free(&r);
    }
}

/* A record's gap in words. */
static void gap_text(int i, char *out, int len) {
    const repair_t *rp = &D.rep.r[i];
    const struct gap *g = &D.gaps[i];
    int mi = machine_index(rp->machine);
    char chips[500];
    chips_text(rp, chips, sizeof(chips));
    int n = snprintf(out, (size_t)len, "%s: %s: ", mi >= 0 ? atlas_machine(mi)->board_name : rp->machine, chips);
    if (n >= len) return;
    if (g->state == GAP_MISSING)
        snprintf(out + n, (size_t)(len - n), "none of the %d best matches has these chips among its causes. The "
                 "best is %.0f%% \"%.250s\".", ATLAS_MAX_GROUPS, 100 * g->best, g->best_label);
    else if (g->state == GAP_LOWER)
        snprintf(out + n, (size_t)(len - n), "match #%d (%.0f%%) has them; the best (%.0f%%) is \"%.250s\".",
                 g->rank + 1, 100 * g->score, 100 * g->best, g->best_label);
    else if (g->state == GAP_FOUND)
        snprintf(out + n, (size_t)(len - n), "the best match (%.0f%%) has them.", 100 * g->score);
    else if (g->state == GAP_NOSHOT) snprintf(out + n, (size_t)(len - n), "no screenshot, nothing to match.");
    else snprintf(out + n, (size_t)(len - n), "its screenshot couldn't be matched.");
}

/* The gaps, in the records window. */
static void gaps_part(struct nk_context *ctx, float cw) {
    if (!nk_tree_push(ctx, NK_TREE_TAB, "Gaps in the simulator: repairs the atlas can't explain", NK_MINIMIZED)) return;
    wrapped_label(ctx, "Each record's screenshot is diagnosed as a new one would be. A repair whose faulty chips "
                       "none of the best matches names is a fault the simulator is missing, or shows otherwise "
                       "than the real board did.", cw - 30, COL_DIMTEXT);
    nk_layout_row_static(ctx, 28, 300, 1);
    if (nk_button_label(ctx, D.gaps ? "Check them again" : "Check the records (takes a moment)")) find_gaps();
    if (D.gaps) {
        int count[GAP_FAILED + 1] = { 0 };
        for (int i = 0; i < D.ngaps; i++)
            if (repair_live(&D.rep.r[i])) count[D.gaps[i].state]++;
        char line[900];
        snprintf(line, sizeof(line), "%d not explained, %d by a lower match, %d by the best; %d without a "
                 "screenshot%s.", count[GAP_MISSING], count[GAP_LOWER], count[GAP_FOUND], count[GAP_NOSHOT],
                 count[GAP_FAILED] ? ", some couldn't be read" : "");
        wrapped_label(ctx, line, cw - 30, COL_TEXT);
        /* The gaps first, then the ones found lower down. */
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < D.ngaps; i++) {
                if (!repair_live(&D.rep.r[i]) || D.gaps[i].state != (pass ? GAP_LOWER : GAP_MISSING)) continue;
                gap_text(i, line, sizeof(line));
                wrapped_label(ctx, line, cw - 30, pass ? COL_PULSE : COL_FAULT);
            }
    }
    nk_tree_pop(ctx);
}

/* Which chips the records found faulty most, per kind of machine. */
static void statistics_part(struct nk_context *ctx, float cw) {
    if (!nk_tree_push(ctx, NK_TREE_TAB, "Statistics: the chips found faulty most", NK_MINIMIZED)) return;
    typedef struct { int fam; char ref[16], what[64]; float n; } count_t;
    count_t *c = calloc((size_t)D.rep.n * REPAIR_MAX_CHIPS + 1, sizeof(count_t));
    int nc = 0, nrec[ATLAS_NMACHINES] = { 0 };
    for (int i = 0; i < D.rep.n; i++) {
        const repair_t *rp = &D.rep.r[i];
        int mi = machine_index(rp->machine);
        if (!repair_live(rp) || mi < 0) continue;
        int fam = atlas_family(mi);
        nrec[fam]++;
        for (int k = 0; k < rp->nchips; k++) {
            int at = -1;
            for (int j = 0; j < nc && at < 0; j++)
                if (c[j].fam == fam && !strcmp(c[j].ref, rp->chips[k].ref)) at = j;
            if (at < 0) {
                at = nc++;
                c[at].fam = fam;
                snprintf(c[at].ref, sizeof(c[at].ref), "%s", rp->chips[k].ref);
                /* What it is, without the details after the label. */
                snprintf(c[at].what, sizeof(c[at].what), "%.63s", rp->chips[k].what);
                char *colon = strchr(c[at].what, ':');
                if (colon) *colon = 0;
            }
            c[at].n += rp->certainty == REPAIR_UNSURE ? 0.5f : 1;
        }
    }
    wrapped_label(ctx, "Each record counts once per chip; a record not sure which of several, half.", cw - 30,
                  COL_DIMTEXT);
    for (int fam = 0; fam < ATLAS_NMACHINES; fam++) {
        if (!nrec[fam]) continue;
        char line[1200];
        int at = snprintf(line, sizeof(line), "%s (%d record%s):", fam == ATLAS_C64 ? "Commodore 64" : "ZX Spectrum",
                          nrec[fam], nrec[fam] == 1 ? "" : "s");
        /* Most first. */
        for (int shown = 0; shown < 12; shown++) {
            int best = -1;
            for (int j = 0; j < nc; j++)
                if (c[j].fam == fam && c[j].n > 0 && (best < 0 || c[j].n > c[best].n)) best = j;
            if (best < 0) break;
            if (at < (int)sizeof(line) - 1)
                at += snprintf(line + at, sizeof(line) - (size_t)at, "%s %s %s %g", shown ? "," : "", c[best].ref,
                               c[best].what, c[best].n);
            c[best].n = -c[best].n;     /* listed */
        }
        wrapped_label(ctx, line, cw - 30, COL_TEXT);
    }
    free(c);
    nk_tree_pop(ctx);
}

static void records_window(struct nk_context *ctx, struct nk_rect area, diagnose_action_t *act) {
    float w = fminf(860, area.w - 32), h = area.h - 32;
    struct nk_rect r = nk_rect(area.x + area.w - w - 16, area.y + 16, w, h);
    if (D.list_reopen) {
        nk_window_show(ctx, "RepairRecords", NK_SHOWN);
        nk_window_set_bounds(ctx, "RepairRecords", r);
        nk_window_set_focus(ctx, "RepairRecords");
        D.list_reopen = false;
    }
    if (!nk_begin_titled(ctx, "RepairRecords", "Repair records", r,
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE |
                         NK_WINDOW_CLOSABLE)) {
        diagnose_records_close();
        nk_end(ctx);
        return;
    }
    if (D.list_dirty) make_list_textures();
    D.list_bounds = nk_window_get_bounds(ctx);
    float cw = nk_window_get_content_region(ctx).w - 20;
    char line[700];
    int live = 0, mine = 0, off = 0;
    for (int i = 0; i < D.rep.n; i++) {
        if (!repair_live(&D.rep.r[i])) continue;
        live++;
        mine += D.rep.r[i].dataset == D.rep.own.id;
        off += D.rep.r[i].off;
    }
    if (D.rep.foreign) snprintf(line, sizeof(line), "%s", D.rep.error);
    else snprintf(line, sizeof(line), "%d record%s: %d yours%s%s%s, %d from others; %d left out of the diagnosis.",
                  live, live == 1 ? "" : "s", mine, D.rep.own.owner[0] ? " (as \"" : "", D.rep.own.owner,
                  D.rep.own.owner[0] ? "\")" : "", live - mine, off);
    nk_layout_row_dynamic(ctx, wrapped_height(ctx, line, cw), 1);
    nk_label_colored_wrap(ctx, line, D.rep.foreign ? COL_HIGH : COL_TEXT);
    /* The path on a line of its own: a long one doesn't wrap. */
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label_colored(ctx, D.rep.path, NK_TEXT_LEFT, COL_DIMTEXT);
    nk_layout_row_static(ctx, 28, 190, 4);
    if (nk_widget_is_hovered(ctx))
        nk_tooltip(ctx, "Add the records of someone else's file (or your own from another PC): new ones are added, "
                        "newer versions replace older ones, the rest are skipped");
    if (nk_button_label(ctx, "Merge a file...")) act->records_file = 1;
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Write your own records to a file, to share");
    if (nk_button_label(ctx, "Export yours...")) act->records_file = 2;
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Write every record here, others' too, to a file");
    if (nk_button_label(ctx, "Export all...")) act->records_file = 3;
    if (nk_button_label(ctx, "Close")) diagnose_records_close();
    nk_layout_row_static(ctx, 28, 386, 1);
    if (nk_widget_is_hovered(ctx))
        nk_tooltip(ctx, "A repair you know the faulty chips of but have no screenshot of: it counts in the "
                        "statistics and in the probing window's ranking");
    if (nk_button_label(ctx, "Record a repair without a screenshot...")) {
        D.recording = D.rec_reopen = D.rec_noshot = true;
        D.rec_machine = D.bench;
        D.rec_n = 0;
        D.rec_cert = REPAIR_SWAP_FIXED;
        D.rec_notes[0] = D.rec_msg[0] = 0;
        D.list_open = false;            /* out of the board's way */
        D.edit = D.confirm = -1;
        free_list_textures();
        nk_end(ctx);
        return;
    }
    if (D.list_msg[0]) {
        nk_layout_row_dynamic(ctx, wrapped_height(ctx, D.list_msg, cw), 1);
        nk_label_colored_wrap(ctx, D.list_msg, COL_HOVER);
    }
    if (live) {
        datasets_part(ctx, cw);
        statistics_part(ctx, cw);
        gaps_part(ctx, cw);
    }
    if (!live) {
        nk_layout_row_dynamic(ctx, 40, 1);
        nk_label_colored_wrap(ctx, "No records yet. Diagnose a screenshot, then \"Record the repair...\" once you "
                                   "know which chips were faulty.", COL_DIMTEXT);
    }
    /* Only one chip's (from the chip's menu on the board). */
    if (D.chip[0]) {
        int n = 0;
        for (int i = 0; i < D.rep.n; i++) n += repair_live(&D.rep.r[i]) && names_chip(&D.rep.r[i], D.chip_family, D.chip);
        snprintf(line, sizeof(line), "Showing the %d record%s with %s faulty (on any %s board).", n, n == 1 ? "" : "s",
                 D.chip, D.chip_family == ATLAS_C64 ? "C64" : "Spectrum");
        nk_layout_row_dynamic(ctx, 8, 1);
        nk_spacing(ctx, 1);
        nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
        nk_layout_row_push(ctx, cw - 170);
        nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_TRACE);
        nk_layout_row_push(ctx, 160);
        if (nk_button_label(ctx, "Show them all")) D.chip[0] = 0;
        nk_layout_row_end(ctx);
    }
    bool typing = false;
    /* Newest first. */
    for (int i = D.rep.n - 1; i >= 0 && D.list_open; i--) {
        repair_t *rp = &D.rep.r[i];
        if (!repair_live(rp) || (D.chip[0] && !names_chip(rp, D.chip_family, D.chip))) continue;
        bool own = rp->dataset == D.rep.own.id;
        const thumbtex_t *lt = i < D.nlist_tex ? &D.list_tex[i] : NULL;
        float tw = 180, th = lt && lt->live && lt->w ? tw * lt->h / lt->w : 100;
        nk_layout_row_dynamic(ctx, 6, 1);
        nk_spacing(ctx, 1);
        nk_layout_row_begin(ctx, NK_STATIC, fmaxf(th, 104), 2);
        nk_layout_row_push(ctx, tw);
        if (!rp->norm) nk_label_colored(ctx, "(no screenshot)", NK_TEXT_CENTERED, COL_DIMTEXT);
        else if (i < D.nlist_tex) image(ctx, &D.list_tex[i]);
        else nk_spacing(ctx, 1);
        nk_layout_row_push(ctx, cw - tw - 10);
        char gname[32];
        snprintf(gname, sizeof(gname), "rec %d", i);
        if (nk_group_begin(ctx, gname, NK_WINDOW_NO_SCROLLBAR)) {
            char when[32] = "?", chips[500];
            time_t t = (time_t)rp->when;
            struct tm *tm = localtime(&t);
            if (tm) strftime(when, sizeof(when), "%Y-%m-%d", tm);
            int mi = machine_index(rp->machine);
            chips_text(rp, chips, sizeof(chips));
            snprintf(line, sizeof(line), "%s   %s: %s", when, mi >= 0 ? atlas_machine(mi)->board_name : rp->machine,
                     chips);
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label_colored(ctx, line, NK_TEXT_LEFT, rp->off ? COL_DIMTEXT : COL_FAULT);
            const char *who = repairs_owner(&D.rep, rp->dataset);
            snprintf(line, sizeof(line), "%s; by %s%s; %s%s", repairs_certainty_name(rp->certainty),
                     who[0] ? who : "(no name)", own ? " (you)" : "", rp->shot ? "screenshot " : "no screenshot",
                     rp->shot ? rp->shot_name : "");
            nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_DIMTEXT);
            char notes[sizeof(rp->notes)];
            nk_label(ctx, rp->notes[0] ? one_line(notes, sizeof(notes), rp->notes) : "", NK_TEXT_LEFT);
            nk_layout_row_begin(ctx, NK_STATIC, 24, own ? 3 : 2);
            nk_layout_row_push(ctx, 150);
            nk_bool use = !rp->off;
            if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Match screenshots against this record (on this PC)");
            if (nk_checkbox_label(ctx, "Use in diagnosis", &use)) {
                if (!repairs_set_off(&D.rep, i, !use)) snprintf(D.list_msg, sizeof(D.list_msg), "%s", D.rep.error);
                records_changed();
            }
            if (own) {
                nk_layout_row_push(ctx, 150);
                if (nk_button_label(ctx, D.edit == i ? "Stop editing" : "Edit...")) {
                    D.edit = D.edit == i ? -1 : i;
                    D.edit_cert = rp->certainty;
                    snprintf(D.edit_notes, sizeof(D.edit_notes), "%s", rp->notes);
                }
                nk_layout_row_push(ctx, 150);
                if (nk_widget_is_hovered(ctx))
                    nk_tooltip(ctx, D.confirm == i ? "Click again to delete it (merged copies are taken back too)"
                                                   : "Delete this record");
                if (nk_button_label(ctx, D.confirm == i ? "Sure? Delete" : "Delete")) {
                    if (D.confirm != i) D.confirm = i;
                    else {
                        rp->flags |= REPAIR_DELETED;
                        if (!repairs_update(&D.rep, i)) {
                            rp->flags &= (uint8_t)~REPAIR_DELETED;
                            snprintf(D.list_msg, sizeof(D.list_msg), "Not deleted: %.300s", D.rep.error);
                        } else snprintf(D.list_msg, sizeof(D.list_msg), "Record deleted.");
                        D.confirm = D.edit = -1;
                        records_changed();
                    }
                }
            } else {
                nk_layout_row_push(ctx, 300);
                nk_label_colored(ctx, "(someone else's: read only)", NK_TEXT_LEFT, COL_DIMTEXT);
            }
            nk_layout_row_end(ctx);
            nk_group_end(ctx);
        }
        nk_layout_row_end(ctx);
        /* The editor of the record. */
        if (D.edit == i && repair_live(rp)) {
            nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
            nk_layout_row_push(ctx, 80);
            nk_label(ctx, "How sure:", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 300);
            static const char *items[REPAIR_NCERTAINTY];
            for (int k = 0; k < REPAIR_NCERTAINTY; k++) items[k] = repairs_certainty_name(k);
            D.edit_cert = nk_combo(ctx, items, REPAIR_NCERTAINTY, D.edit_cert, 22, nk_vec2(300, 100));
            nk_layout_row_end(ctx);
            nk_layout_row_dynamic(ctx, 70, 1);
            typing |= (nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX, D.edit_notes, sizeof(D.edit_notes),
                                                      nk_filter_default) & NK_EDIT_ACTIVE) != 0;
            nk_layout_row_static(ctx, 26, 150, 2);
            if (nk_button_label(ctx, "Save changes")) {
                repair_t keep = *rp;
                rp->certainty = (uint8_t)D.edit_cert;
                snprintf(rp->notes, sizeof(rp->notes), "%s", D.edit_notes);
                if (!repairs_update(&D.rep, i)) {
                    rp->certainty = keep.certainty;
                    memcpy(rp->notes, keep.notes, sizeof(rp->notes));
                    snprintf(D.list_msg, sizeof(D.list_msg), "Not saved: %.300s", D.rep.error);
                } else snprintf(D.list_msg, sizeof(D.list_msg), "Record changed (version %u).", rp->version);
                D.edit = -1;
                records_changed();
            }
            if (nk_button_label(ctx, "Cancel")) D.edit = -1;
        }
    }
    if (!D.recording) D.typing = typing;
    nk_end(ctx);
}

/* The window that records a repair: small, at the side of the board, so
 * the chips can be clicked on it. */
static void record_window(struct nk_context *ctx, struct nk_rect area) {
    float w = 440, h = fminf(560, area.h - 32);
    struct nk_rect r = nk_rect(area.x + area.w - w - 16, area.y + 16, w, h);
    if (D.rec_reopen) {
        nk_window_show(ctx, "RecordRepair", NK_SHOWN);
        nk_window_set_bounds(ctx, "RecordRepair", r);
        nk_window_set_focus(ctx, "RecordRepair");
        D.rec_reopen = false;
    }
    if (!nk_begin_titled(ctx, "RecordRepair", "Record the repair", r,
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE |
                         NK_WINDOW_CLOSABLE)) {
        diagnose_record_cancel();
        nk_end(ctx);
        return;
    }
    D.rec_bounds = nk_window_get_bounds(ctx);
    float cw = nk_window_get_content_region(ctx).w - 20;
    bool typing = false;
    char line[300];
    const char *how = D.rec_noshot
                          ? "Click the chips that were faulty on the board (click one again to take it off). With "
                            "no screenshot, the record counts in the statistics and in the probing window's ranking, "
                            "not in screenshot matches."
                          : "Click the chips that were faulty on the board (click one again to take it off). They "
                            "are kept with the screenshot in repairs.bin, so a screenshot like it later shows them.";
    nk_layout_row_dynamic(ctx, wrapped_height(ctx, how, cw), 1);
    nk_label_wrap(ctx, how);
    /* The screenshot, small, and the board. */
    snprintf(line, sizeof(line), "Board: %s", atlas_machine(D.rec_machine)->board_name);
    if (D.rec_noshot) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, line, NK_TEXT_LEFT);
    } else {
        float pw = 160, ph = D.r.picture ? pw * D.r.h / D.r.w : 40;
        nk_layout_row_begin(ctx, NK_STATIC, ph, 2);
        nk_layout_row_push(ctx, pw);
        image(ctx, &D.tex[0]);
        nk_layout_row_push(ctx, cw - pw - 10);
        nk_label_wrap(ctx, line);
        nk_layout_row_end(ctx);
    }
    /* The chips picked. */
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label_colored(ctx, D.rec_n ? "Faulty chips:" : "Faulty chips: none yet (click them on the board)",
                     NK_TEXT_LEFT, COL_FAULT);
    for (int i = 0; i < D.rec_n; i++) {
        nk_layout_row_begin(ctx, NK_STATIC, 22, 2);
        nk_layout_row_push(ctx, cw - 80);
        snprintf(line, sizeof(line), "%s   %s", D.rec_chips[i].ref, D.rec_chips[i].what);
        nk_label(ctx, line, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 70);
        if (nk_button_label(ctx, "Remove")) {
            memmove(&D.rec_chips[i], &D.rec_chips[i + 1], (size_t)(D.rec_n - i - 1) * sizeof(repair_chip_t));
            D.rec_n--;
            i--;
        }
        nk_layout_row_end(ctx);
    }
    /* How sure, notes, name. */
    nk_layout_row_begin(ctx, NK_STATIC, 24, 2);
    nk_layout_row_push(ctx, 80);
    nk_label(ctx, "How sure:", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, cw - 90);
    static const char *items[REPAIR_NCERTAINTY];
    for (int i = 0; i < REPAIR_NCERTAINTY; i++) items[i] = repairs_certainty_name(i);
    D.rec_cert = nk_combo(ctx, items, REPAIR_NCERTAINTY, D.rec_cert, 22, nk_vec2(cw - 90, 100));
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label_colored(ctx, "Notes (what you saw, measured, replaced):", NK_TEXT_LEFT, COL_DIMTEXT);
    nk_layout_row_dynamic(ctx, 70, 1);
    typing |= (nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX, D.rec_notes, sizeof(D.rec_notes),
                                              nk_filter_default) & NK_EDIT_ACTIVE) != 0;
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label_colored(ctx, "Your name (shown with your records, optional):", NK_TEXT_LEFT, COL_DIMTEXT);
    nk_layout_row_dynamic(ctx, 26, 1);
    typing |= (nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, D.rec_owner, sizeof(D.rec_owner),
                                              nk_filter_default) & NK_EDIT_ACTIVE) != 0;
    D.typing = typing;
    if (D.rec_msg[0]) {
        nk_layout_row_dynamic(ctx, wrapped_height(ctx, D.rec_msg, cw), 1);
        nk_label_colored_wrap(ctx, D.rec_msg, COL_HIGH);
    }
    nk_layout_row_static(ctx, 28, 150, 2);
    if (nk_button_label(ctx, "Save the record")) {
        if (!D.rec_n) snprintf(D.rec_msg, sizeof(D.rec_msg), "Click at least one chip on the board first.");
        else save_record();
    }
    if (nk_button_label(ctx, "Cancel")) diagnose_record_cancel();
    nk_end(ctx);
}

static void main_window(struct nk_context *ctx, struct nk_rect area, diagnose_action_t *act);

void diagnose_window(struct nk_context *ctx, struct nk_rect area, diagnose_action_t *act) {
    memset(act, 0, sizeof(*act));
    act->bench = -1;
    if (D.recording) {
        record_window(ctx, area);
        return;
    }
    main_window(ctx, area, act);
    /* After it: shown again, it comes on top. */
    if (D.list_open) records_window(ctx, area, act);
}

/* The diagnosis: the screenshot and its matches. */
static void main_window(struct nk_context *ctx, struct nk_rect area, diagnose_action_t *act) {
    if (!D.open) return;
    float w = fminf(1000, area.w - 32), h = area.h - 32;
    struct nk_rect r = nk_rect(area.x + 16, area.y + 16, w, h);
    bool fresh = D.reopen;
    if (D.reopen) {
        nk_window_show(ctx, "Diagnose", NK_SHOWN);
        nk_window_set_bounds(ctx, "Diagnose", r);
        nk_window_set_focus(ctx, "Diagnose");
        D.reopen = false;
    }
    if (!nk_begin_titled(ctx, "Diagnose", "Diagnose from screenshot", r,
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE |
                         NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR)) {
        D.open = false;
        nk_end(ctx);
        return;
    }
    if (fresh) nk_window_set_scroll(ctx, 0, 0);
    D.bounds = nk_window_get_bounds(ctx);
    float cw = nk_window_get_content_region(ctx).w - 20;

    /* The screenshot, what was read of it, the buttons. */
    float pw = 260, ph = D.r.picture ? pw * D.r.h / D.r.w : 60;
    if (ph > 220) { pw = pw * 220 / ph; ph = 220; }
    float hw = cw - pw - 24;
    const char *text = D.ok ? D.r.summary : D.r.error;
    const char *note = "Matched against the normal boot pictures of the fault atlas (no diagnostic cartridge: "
                       "with one, the cartridge itself tells what is wrong).";
    /* The board it is matched against: the one on the bench. Another one's
     * button puts that on the bench, and matches again. */
    char against[200];
    snprintf(against, sizeof(against), "Matching against: %s (active board)", atlas_machine(D.machine)->board_name);
    if (D.matched != D.machine)
        snprintf(against + strlen(against), sizeof(against) - strlen(against),
                 ": no pictures of it in the atlas yet, the %s's used", atlas_machine(D.matched)->issue);
    /* Tall enough for all of it, the buttons too (the group doesn't scroll). */
    float need = wrapped_height(ctx, text, hw) + wrapped_height(ctx, note, hw) + wrapped_height(ctx, against, hw) +
                 26 + 28 + 28 + 6 * 4 + 12;
    nk_layout_row_begin(ctx, NK_STATIC, fmaxf(ph, need), 2);
    nk_layout_row_push(ctx, pw);
    image(ctx, &D.tex[0]);
    nk_layout_row_push(ctx, cw - pw - 10);
    if (nk_group_begin(ctx, "diag head", NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, wrapped_height(ctx, text, hw), 1);
        nk_label_colored_wrap(ctx, text, D.ok ? COL_TEXT : COL_HIGH);
        nk_layout_row_dynamic(ctx, wrapped_height(ctx, note, hw), 1);
        nk_label_colored_wrap(ctx, note, COL_DIMTEXT);
        nk_layout_row_dynamic(ctx, wrapped_height(ctx, against, hw), 1);
        nk_label_colored_wrap(ctx, against, COL_HOVER);
        nk_layout_row_dynamic(ctx, 26, ATLAS_NMACHINES);
        for (int mi = 0; mi < ATLAS_NMACHINES; mi++) {
            const machine_t *bm = atlas_machine(mi);
            char name[64];
            snprintf(name, sizeof(name), atlas_family(mi) == ATLAS_ZX48 ? "ZX %s" : "C64 %s", bm->issue);
            bool cur = D.machine == mi;
            if (nk_widget_is_hovered(ctx) && !cur)
                nk_tooltip(ctx, "Put this board on the bench and match the screenshot against its faults");
            if (nk_select_label(ctx, name, NK_TEXT_CENTERED, cur) && !cur) act->bench = mi;
        }
        nk_layout_row_static(ctx, 28, 200, 3);
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, "Take the best matches to \"Diagnose by probing\": it asks for the pins that tell "
                            "their causes apart");
        if (D.ok && nk_button_label(ctx, "Narrow down by probing...")) act->probe = true;
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Pick another screenshot (or drop one on the window)");
        if (nk_button_label(ctx, "Open another screenshot...")) act->pick = true;
        if (nk_button_label(ctx, "Close")) D.open = false;
        /* Real repairs: record this one, match against them or not. */
        nk_layout_row_begin(ctx, NK_STATIC, 28, 3);
        nk_layout_row_push(ctx, 200);
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Every repair record: look, leave out, edit, delete, share");
        if (nk_button_label(ctx, "Repair records...")) diagnose_records_open(D.machine);
        nk_layout_row_push(ctx, 200);
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, "Found what was wrong with this machine? Keep it: the screenshot and the chips that "
                            "were faulty, picked on the board. A screenshot like it later shows them.");
        if (D.ok && nk_button_label(ctx, "Record the repair...")) {
            D.recording = D.rec_reopen = true;
            D.rec_noshot = false;
            D.rec_machine = D.machine;
            D.rec_n = 0;
            D.rec_cert = REPAIR_SWAP_FIXED;
            D.rec_notes[0] = D.rec_msg[0] = 0;
        }
        nk_layout_row_push(ctx, hw - 420);
        char use[120];
        if (D.rep.foreign) snprintf(use, sizeof(use), "Repair records: %.100s", D.rep.error);
        else snprintf(use, sizeof(use), "Match real repair records too (%d in repairs.bin)", D.rep.n);
        nk_bool on = D.use_real;
        if (nk_checkbox_label(ctx, use, &on)) {
            D.use_real = on;
            run();
        }
        nk_layout_row_end(ctx);
        nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);
    diagnose_caps(ctx, cw);

    /* The matches scroll under the screenshot, which stays in sight to
     * compare them with. */
    static nk_uint list_x, list_y;
    if (fresh) list_x = list_y = 0;
    struct nk_rect region = nk_window_get_content_region(ctx);
    float list_h = region.y + region.h - (ctx->current->layout->at_y + ctx->current->layout->row.height) -
                   ctx->style.window.spacing.y - 4;
    if (list_h < 120) list_h = 120;
    nk_layout_row_dynamic(ctx, list_h, 1);
    if (nk_group_scrolled_offset_begin(ctx, &list_x, &list_y, "diag list", 0)) {
        float lw = cw - ctx->style.window.scrollbar_size.x - 2 * ctx->style.window.group_padding.x;
        if (D.ok) {
            real_matches(ctx, lw);
            for (int k = 0; k < D.r.ngroups; k++) group(ctx, k, lw, act);
        }
        nk_group_end(ctx);
    }
    nk_end(ctx);
}

/* Without a window: the screenshot diagnosed as the window does it, and
 * printed; with CHIPS (references, comma separated), the repair recorded
 * first, as "Record the repair..." does. */
int diagnose_cli(const char *picture, int machine, const char *chips, int certainty, const char *notes,
                 const char *owner) {
    D.headless = true;
    snprintf(D.path, sizeof(D.path), "%s", picture);
    D.machine = machine;
    run();
    if (!D.ok) {
        fprintf(stderr, "%s\n", D.r.error);
        return 1;
    }
    if (chips) {
        /* What each chip is on this board, from the board itself. */
        board_t *b = board_create(atlas_machine(machine));
        D.recording = true;
        D.rec_machine = machine;
        D.rec_n = 0;
        D.rec_cert = certainty >= 0 && certainty < REPAIR_NCERTAINTY ? certainty : REPAIR_SWAP_FIXED;
        snprintf(D.rec_notes, sizeof(D.rec_notes), "%s", notes ? notes : "");
        if (owner) snprintf(D.rec_owner, sizeof(D.rec_owner), "%s", owner);
        for (const char *s = chips; *s; ) {
            size_t len = strcspn(s, ",");
            char ref[16];
            snprintf(ref, sizeof(ref), "%.*s", (int)(len < 15 ? len : 15), s);
            s += len + (s[len] == ',');
            int part = sim_find_part(&b->sim, ref);
            if (part < 0) {
                fprintf(stderr, "%s has no %s\n", atlas_machine(machine)->board_name, ref);
                board_destroy(b);
                return 1;
            }
            const char *det = board_part_detail(b, part);
            char what[64];
            snprintf(what, sizeof(what), "%s%s%.*s", board_part_label(b, part), det && *det ? ": " : "",
                     det ? (int)strcspn(det, "\n,") : 0, det ? det : "");
            diagnose_record_toggle(ref, what);
        }
        board_destroy(b);
        save_record();
        if (D.recording) {
            fprintf(stderr, "%s\n", D.rec_msg);
            return 1;
        }
        printf("Recorded in %s: record %u of dataset %016llx\n", D.rep.path, D.rep.r[D.rep.n - 1].number,
               (unsigned long long)D.rep.own.id);
    }
    if (D.rep.foreign) printf("%s\n", D.rep.error);
    printf("%s\n", D.r.summary);
    for (int k = 0; k < D.r.nreals; k++) {
        const repair_t *rp = &D.rep.r[D.real_rec[k]];
        printf("  real %3.0f%%%s ", 100 * D.r.reals[k].score, D.r.reals[k].score >= REAL_MIN ? " (shown)" : "        ");
        for (int i = 0; i < rp->nchips; i++) printf("%s%s (%s)", i ? ", " : "", rp->chips[i].ref, rp->chips[i].what);
        const char *who = repairs_owner(&D.rep, rp->dataset);
        printf("; %s; %s; by %s\n", rp->machine, repairs_certainty_name(rp->certainty), who[0] ? who : "(no name)");
    }
    for (int k = 0; k < D.r.ngroups; k++) {
        const atlas_group_t *g = &D.r.groups[k];
        printf("  #%d %3.0f%% %s (%d causes:", k + 1, 100 * g->score, g->label, g->ncauses);
        for (int i = 0; i < g->ncauses && i < 4; i++) printf(" %s", g->causes[i].spec[0] ? g->causes[i].spec : "healthy");
        printf("%s)\n", g->ncauses > 4 ? " ..." : "");
    }
    return 0;
}

/* Without a window: the records listed, merged into, exported from. */
int diagnose_cli_records(const char *list, const char *merge, const char *export_to, bool all, bool reread,
                         bool gaps) {
    D.headless = true;
    load_repairs();
    if (reread) printf("%d records read again from their screenshots\n", reread_records(true));
    if (D.rep.foreign) {
        fprintf(stderr, "%s\n", D.rep.error);
        return 1;
    }
    if (merge) {
        int added, updated, skipped;
        if (!repairs_merge(&D.rep, merge, &added, &updated, &skipped)) {
            fprintf(stderr, "not merged: %s\n", D.rep.error);
            return 1;
        }
        printf("merged %s: %d new, %d newer versions, %d already here\n", merge, added, updated, skipped);
    }
    if (export_to) {
        if (!repairs_export(&D.rep, export_to, all)) {
            fprintf(stderr, "not exported: %s\n", D.rep.error);
            return 1;
        }
        printf("%s records written to %s%s%s\n", all ? "all the" : "your", export_to, D.rep.error[0] ? ": " : "",
               D.rep.error);
    }
    if (list) {
        printf("%s: own dataset %016llx \"%s\"\n", D.rep.path, (unsigned long long)D.rep.own.id, D.rep.own.owner);
        for (int i = 0; i < D.rep.n; i++) {
            const repair_t *rp = &D.rep.r[i];
            char chips[500];
            chips_text(rp, chips, sizeof(chips));
            const char *who = repairs_owner(&D.rep, rp->dataset);
            printf("  %016llx/%u v%u r%u %s%s%s: %s; %s; by %s; %s\n", (unsigned long long)rp->dataset, rp->number,
                   rp->version, rp->read_version, rp->machine, repair_live(rp) ? "" : " DELETED", rp->off ? " (left out)" : "", chips,
                   repairs_certainty_name(rp->certainty), who[0] ? who : "(no name)", rp->notes);
        }
    }
    if (gaps) {
        static const char *const state[] = { "explained", "lower", "GAP", "no shot", "unread" };
        find_gaps();
        printf("Gaps in the simulator (each record's screenshot against the atlas):\n");
        for (int i = 0; i < D.ngaps; i++) {
            if (!repair_live(&D.rep.r[i])) continue;
            char line[900];
            gap_text(i, line, sizeof(line));
            printf("  %-9s %016llx/%u %s\n", state[D.gaps[i].state], (unsigned long long)D.rep.r[i].dataset,
                   D.rep.r[i].number, line);
        }
    }
    return 0;
}

void diagnose_shutdown(void) {
    free(D.gaps);
    D.gaps = NULL;
    free_list_textures();
    free_textures();
    atlas_result_free(&D.r);
    repairs_free(&D.rep);
}
