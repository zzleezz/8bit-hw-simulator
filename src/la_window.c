/* la_window.c - "Diagnose with the logic analyser" (see la_window.h).
 *
 * The diagnosis is lacap.c's --la-diagnose, run as from the command line
 * but in a thread: its text goes through la_text_hook into a buffer shown
 * here, and when it asks the user to go on (la_ask_hook) the thread waits
 * for the window's buttons. The simulated boards it builds are its own,
 * not the one on the bench.
 *
 * AI-generated (Claude Code). */
#include "la_window.h"
#ifdef _WIN32
#include <direct.h>
#endif
#include "atlas.h"
#include "diagnose.h"
#include "thumbtex.h"
#include "lacap.h"
#include "lapico.h"
#include "zx48.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>

#define MAX_MARKS 48
#define NSTAGES 7                   /* idle, then the boot's six */

static const char *const stage_label[NSTAGES] = { "Idle", "Reset", "RAM test", "RAM done", "Copyright", "Main loop",
                                                  "Interrupt" };

enum { MODE_ANALYSER, MODE_REPLAY, MODE_SIMULATE };

static struct {
    bool open, reopen, typing;
    struct nk_rect bounds;
    int mode;
    char folder[700], ports[128], fault[128];
    char base[512];                 /* where the diagnosis folders go */
    char work[600];                 /* this run's captures (emptied at each start) */
    char save[700], saved[900];     /* "Save the diagnosis": where to, and how it went */
    int run_mode;
    bool all;
    bool start_pending;             /* --la-window: start on the next frame */
    char boards[400];               /* "Find boards": what was found */
    lp_port_t bp[8];                /* the boards found, the master first */
    int nbp;
    bool bp_known;                  /* that order was remembered (la-boards.txt) */

    /* The run. */
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool started_once;
    atomic_bool running;
    bool stop;
    char *text;                     /* all it printed */
    size_t len, cap;
    size_t shown_len;               /* the text the window last drew (to scroll down on more) */
    bool asking;
    char prompt[600];
    int answer;                     /* -1 none yet, 0 no, 1 go on */
    int rc;
    char rc_text[200];

    /* The verdict's chips, on this machine's board. */
    const machine_t *m;             /* the machine diagnosed */
    char mark_ref[MAX_MARKS][16];
    int mark_level[MAX_MARKS], nmarks;
    char summary[400];

    /* How far it is: each stage's state (LA_ST_...) and where its text
     * starts; the analyser's sign (0 none, 1 armed, 2 triggered) and what
     * to do now. */
    bool has_run;                   /* a run started: the setup is hidden till "New diagnosis" */
    int stage_st[NSTAGES];
    size_t stage_off[NSTAGES];
    int cur_stage;
    int sign, arms;
    bool expect_release;            /* the last question had the reset button held */
    char doing[200];
    long jump;                      /* a stage's box clicked: the text offset to show (-1 none) */

    /* The screen: a picture of the monitor (the user's file; "" none), shown
     * small; "Choose..." / "Paste" asked of the UI (1, 2). */
    char screen[1100];
    char screen_msg[300];
    thumbtex_t screen_tex;
    int want_pic;
    /* "Pick a type...": the kinds of screen, a typical picture of each. */
    bool picking;
    int pick_cat;                   /* -1: the kinds; else its pictures */
} L = { .all = true, .answer = -1, .jump = -1 };

static pthread_once_t once = PTHREAD_ONCE_INIT;
static void init_sync(void) {
    pthread_mutex_init(&L.mu, NULL);
    pthread_cond_init(&L.cv, NULL);
}

/* ---- from the diagnosis's thread ---------------------------------------- */

/* The console's "press Enter" / "type n, Enter" as this window's buttons.
 * Returns a malloc'd copy. */
static char *button_words(const char *t) {
    static const char *const from[] = { "press Enter here", "type n, Enter", "press Enter", "Press Enter" };
    static const char *const to[] = { "press Continue", "press No", "press Continue", "Press Continue" };
    size_t n = strlen(t);
    char *o = malloc(n * 2 + 1);
    if (!o) return NULL;
    size_t k = 0;
    while (*t) {
        int f = -1;
        for (int i = 0; i < 4 && f < 0; i++)
            if (!strncmp(t, from[i], strlen(from[i]))) f = i;
        if (f >= 0) {
            memcpy(o + k, to[f], strlen(to[f]));
            k += strlen(to[f]);
            t += strlen(from[f]);
        } else o[k++] = *t++;
    }
    o[k] = 0;
    return o;
}

static void text_put(const char *t);
static void text_hook(const char *t) {
    char *w = button_words(t);
    text_put(w ? w : t);
    free(w);
}
static void text_put(const char *t) {
    size_t n = strlen(t);
    pthread_mutex_lock(&L.mu);
    if (L.len + n + 1 > L.cap) {
        size_t cap = L.cap ? L.cap : 65536;
        while (cap < L.len + n + 1) cap *= 2;
        char *p = realloc(L.text, cap);
        if (p) { L.text = p; L.cap = cap; }
    }
    if (L.len + n + 1 <= L.cap) {
        memcpy(L.text + L.len, t, n + 1);
        L.len += n;
    }
    pthread_mutex_unlock(&L.mu);
}

static bool ask_hook(const char *prompt) {
    pthread_mutex_lock(&L.mu);
    char *w = prompt && prompt[0] ? button_words(prompt) : NULL;
    snprintf(L.prompt, sizeof(L.prompt), "%s", w ? w : prompt && prompt[0] ? prompt : "Continue when ready");
    free(w);
    size_t k = strlen(L.prompt);
    while (k && (L.prompt[k - 1] == '\n' || L.prompt[k - 1] == ' ')) L.prompt[--k] = 0;
    L.answer = -1;
    L.asking = true;
    L.sign = 0;
    L.arms = 0;
    L.doing[0] = 0;
    L.expect_release = strstr(L.prompt, "Hold the") || strstr(L.prompt, "hold the") || strstr(L.prompt, "hold it");
    while (L.answer < 0 && !L.stop) pthread_cond_wait(&L.cv, &L.mu);
    bool go = L.answer == 1 && !L.stop;
    L.asking = false;
    pthread_mutex_unlock(&L.mu);
    text_hook(go ? "  [continued]\n" : "  [no]\n");
    return go;
}

static void status_hook(int kind, int stage, int state) {
    pthread_mutex_lock(&L.mu);
    if (kind == LA_EV_STAGE && stage >= 0 && stage < NSTAGES) {
        L.stage_st[stage] = state;
        if (state == LA_ST_RUN) {
            L.stage_off[stage] = L.len;
            L.cur_stage = stage;
            L.sign = 0;
            L.arms = 0;
            L.expect_release = false;
            L.doing[0] = 0;
        }
    } else if (kind == LA_EV_ARMED) {
        L.sign = 1;
        if (++L.arms == 1 && L.expect_release) snprintf(L.doing, sizeof(L.doing), "LET GO OF RESET");
        else if (L.cur_stage == 0) snprintf(L.doing, sizeof(L.doing), "Waiting for the Spectrum (left running: don't touch it)");
        else snprintf(L.doing, sizeof(L.doing), "Waiting for %s", stage_label[L.cur_stage]);
    } else if (kind == LA_EV_PROGRESS) {
        snprintf(L.doing, sizeof(L.doing), "Checking the screen's faults against the captures: %d of %d simulated", stage,
                 state);
    } else if (kind == LA_EV_TRIGGERED) {
        L.sign = 2;
        snprintf(L.doing, sizeof(L.doing), "Reading the samples, then comparing with a working board");
    }
    pthread_mutex_unlock(&L.mu);
}

static void add_mark(const char *ref, int level) {
    for (int k = 0; k < L.nmarks; k++)
        if (!strcmp(L.mark_ref[k], ref)) {
            if (level > L.mark_level[k]) L.mark_level[k] = level;
            return;
        }
    if (L.nmarks >= MAX_MARKS) return;
    snprintf(L.mark_ref[L.nmarks], sizeof(L.mark_ref[0]), "%s", ref);
    L.mark_level[L.nmarks++] = level;
}

/* The chips the verdict names (la_text_parts: the items of "What to look
 * at" and the screen's), item 1's the first to look at. */
static void find_marks(const char *text) {
    L.nmarks = 0;
    L.summary[0] = 0;
    char ref[MAX_MARKS][16];
    int lv[MAX_MARKS];
    int n = la_text_parts(L.m, text, ref, lv, MAX_MARKS);
    for (int k = 0; k < n; k++) add_mark(ref[k], lv[k]);
    int m = 0;
    for (int lev = 2; lev >= 1; lev--) {
        bool any = false;
        for (int k = 0; k < L.nmarks && m < (int)sizeof(L.summary) - 20; k++)
            if (L.mark_level[k] == lev) {
                m += snprintf(L.summary + m, sizeof(L.summary) - m, "%s%s",
                              any ? ", " : lev == 2 ? "first (thick orange) " : m ? "; then (thin yellow) " : "then ",
                              L.mark_ref[k]);
                any = true;
            }
    }
}

static bool copy_file(const char *from, const char *to);
static void make_dir(const char *d) {
#ifdef _WIN32
    _mkdir(d);
#else
    mkdir(d, 0777);
#endif
}

/* The screen's picture in DIR as screen.EXT (an earlier one there goes),
 * for Save to keep with the captures: its path in OUT, or NULL for none. */
static const char *screen_into(const char *dir, const char *screen, char *out, int len) {
    static const char *const ext[] = { "png", "jpg", "jpeg", "bmp" };
    char p[1200];
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof(p), "%s/screen.%s", dir, ext[i]);
        remove(p);
    }
    if (!screen[0]) return NULL;
    const char *dot = strrchr(screen, '.');
    char e[8] = "png";
    if (dot && strlen(dot + 1) < sizeof(e)) {
        snprintf(e, sizeof(e), "%s", dot + 1);
        for (char *c = e; *c; c++) *c = (char)tolower((unsigned char)*c);
    }
    make_dir(dir);
    snprintf(out, (size_t)len, "%s/screen.%s", dir, e);
    return copy_file(screen, out) ? out : screen;
}

/* The text the diagnosis printed before its screen's part. */
static size_t bus_text_len(void) {
    const char *t = L.text ? strstr(L.text, "\nThe screen (") : NULL;
    return t ? (size_t)(t - L.text) : L.len;
}

static void finish(int rc) {
    pthread_mutex_lock(&L.mu);
    for (int k = 0; k < NSTAGES; k++)
        if (L.stage_st[k] == LA_ST_RUN) L.stage_st[k] = LA_ST_NONE;    /* stopped in it */
    L.sign = 0;
    L.doing[0] = 0;
    L.rc = rc;
    snprintf(L.rc_text, sizeof(L.rc_text), "%s",
             L.stop ? "Stopped." : rc == 0 ? "Done: no fault found." : rc == 2 ? "Done: the verdict is below." :
             "It stopped before a verdict (see the last lines below).");
    char *copy = L.text ? strdup(L.text) : NULL;
    pthread_mutex_unlock(&L.mu);
    if (copy) { find_marks(copy); free(copy); }
    atomic_store(&L.running, false);
}

static char atlas_path[1100];

/* After a run: only the screen's part again, for a picture given (or
 * taken away) then. */
static void *screen_worker(void *arg) {
    (void)arg;
    static char screen[1300];
    pthread_mutex_lock(&L.mu);
    size_t bl = bus_text_len();
    if (L.text) { L.text[bl] = 0; L.len = bl; }
    char *bus = L.text ? strdup(L.text) : strdup("");
    snprintf(screen, sizeof(screen), "%s", L.screen);
    int was = L.rc;
    pthread_mutex_unlock(&L.mu);
    bool bus_ok = bus && !strstr(bus, "What to look at");
    int rc = was;
    if (screen[0] && bus) {
        la_cli_t o;
        memset(&o, 0, sizeof(o));
        o.machine = L.m;
        o.screen = screen;
        o.atlas = atlas_path;
        o.carts = "carts";
        la_text_hook = text_hook;
        int r = la_screen_again(&o, bus, bus_ok);
        la_text_hook = NULL;
        if (bus_ok && was != 1) rc = r > 0 ? 2 : 0;
    } else if (bus_ok && was == 2)
        rc = 0;                         /* the screen was all it had */
    free(bus);
    finish(rc);
    return NULL;
}

/* After a run: the screen's faults checked against its captures. */
static void *check_worker(void *arg) {
    (void)arg;
    static char screen[1300], folder[700];
    pthread_mutex_lock(&L.mu);
    char *t = L.text ? strstr(L.text, "\nChecked against the captures:") : NULL;  /* an earlier check's */
    if (t) { *t = 0; L.len = (size_t)(t - L.text); }
    snprintf(screen, sizeof(screen), "%s", L.screen);
    snprintf(folder, sizeof(folder), "%s", L.run_mode == MODE_REPLAY ? L.folder : L.work);
    snprintf(L.doing, sizeof(L.doing), "Checking the screen's faults against the captures");
    int was = L.rc;
    pthread_mutex_unlock(&L.mu);
    la_cli_t o;
    memset(&o, 0, sizeof(o));
    o.machine = L.m;
    o.diagnose = folder;
    o.screen = screen[0] ? screen : NULL;
    o.atlas = atlas_path;
    o.carts = "carts";
    o.check = CHECK_DEFAULT;
    la_text_hook = text_hook;
    la_status_hook = status_hook;
    int r = la_check(&o);
    la_text_hook = NULL;
    la_status_hook = NULL;
    finish(r > 0 && was == 0 ? 2 : was);
    return NULL;
}

static void *worker(void *arg) {
    (void)arg;
    la_cli_t o;
    memset(&o, 0, sizeof(o));
    static char folder[700], ports[128], fault[128], screen[1300];
    static const char *faults[1];
    snprintf(folder, sizeof(folder), "%s", L.folder);
    snprintf(ports, sizeof(ports), "%s", L.ports);
    snprintf(fault, sizeof(fault), "%s", L.fault);
    o.diagnose = folder;
    if (L.mode != MODE_REPLAY) snprintf(folder, sizeof(folder), "%s", L.work);
    o.machine = L.m;
    o.all = L.all;
    if (L.mode == MODE_ANALYSER) o.ports = ports[0] ? ports : NULL;
    else if (L.mode == MODE_REPLAY) o.replay = true;
    else {
        o.simulate = true;
        if (fault[0]) { faults[0] = fault; o.faults = faults; o.nfaults = 1; }
    }
    /* The screen: kept with a fresh run's captures (and taken from a
     * replayed folder's own screen.png when none is given). */
    pthread_mutex_lock(&L.mu);
    char given[1100];
    snprintf(given, sizeof(given), "%s", L.screen);
    pthread_mutex_unlock(&L.mu);
    if (L.mode != MODE_REPLAY) o.screen = screen_into(folder, given, screen, sizeof(screen));
    else if (given[0]) o.screen = given;
    else {                      /* the folder's own, shown in the Screen row */
        static const char *const ext[] = { "png", "jpg", "jpeg", "bmp" };
        for (int i = 0; i < 4 && !o.screen; i++) {
            snprintf(screen, sizeof(screen), "%s/screen.%s", folder, ext[i]);
            FILE *f = fopen(screen, "rb");
            if (!f) continue;
            fclose(f);
            o.screen = screen;
            pthread_mutex_lock(&L.mu);
            snprintf(L.screen, sizeof(L.screen), "%.1090s", screen);
            snprintf(L.screen_msg, sizeof(L.screen_msg), "screen.%s", ext[i]);
            pthread_mutex_unlock(&L.mu);
        }
    }
    o.atlas = atlas_path;
    o.carts = "carts";
    la_text_hook = text_hook;
    la_ask_hook = ask_hook;
    la_status_hook = status_hook;
    int rc = la_cli(&o);
    la_text_hook = NULL;
    la_ask_hook = NULL;
    la_status_hook = NULL;
    finish(rc);
    return NULL;
}

/* ---- the window ----------------------------------------------------------- */

static bool is_zx48(const machine_t *m) {
    return m == &machine_zx48 || m == &machine_zx48i2 || m == &machine_zx48i1 || m == &machine_zx48i4b ||
           m == &machine_zx48i6a;
}

/* Everything a run left: its text, verdict, outlines, stages. */
static void clear_results(void) {
    pthread_mutex_lock(&L.mu);
    L.len = 0;
    if (L.text) L.text[0] = 0;
    L.shown_len = 0;
    L.asking = false;
    L.answer = -1;
    L.rc_text[0] = 0;
    L.nmarks = 0;
    L.summary[0] = 0;
    L.saved[0] = 0;
    memset(L.stage_st, 0, sizeof(L.stage_st));
    memset(L.stage_off, 0, sizeof(L.stage_off));
    L.cur_stage = 0;
    L.sign = L.arms = 0;
    L.doing[0] = 0;
    L.jump = -1;
    pthread_mutex_unlock(&L.mu);
}

/* The bench board's options, as "ID=VALUE", taken when a run starts. */
static struct board *bench_board;
void la_window_board(struct board *b) { bench_board = b; }
static char opt_s[8][64];
static const char *opt_p[8];

static void start(const machine_t *m) {
    pthread_once(&once, init_sync);
    if (atomic_load(&L.running)) return;
    int no = 0;
    if (bench_board && bench_board->m == m)
        for (int o = 0; o < m->noptions && no < 8; o++) {
            int v = m->get_option(bench_board, o);
            if (v < 0 || v >= m->options[o].nvalues) continue;
            snprintf(opt_s[no], sizeof(opt_s[0]), "%s=%s", m->options[o].id, m->options[o].value_ids[v]);
            opt_p[no] = opt_s[no];
            no++;
        }
    la_set_options(opt_p, no);
    if (L.started_once) pthread_join(L.th, NULL);
    clear_results();
    pthread_mutex_lock(&L.mu);
    L.stop = false;
    L.m = m;
    L.run_mode = L.mode;
    L.has_run = true;
    pthread_mutex_unlock(&L.mu);
    atomic_store(&L.running, true);
    if (pthread_create(&L.th, NULL, worker, NULL) != 0) {
        atomic_store(&L.running, false);
        snprintf(L.rc_text, sizeof(L.rc_text), "Couldn't start it (no thread).");
        return;
    }
    L.started_once = true;
}

/* After a run, on its thread: FN (the screen's part again, or the check). */
static void after_run(void *(*fn)(void *)) {
    if (atomic_load(&L.running) || !L.has_run || !L.m) return;
    if (L.started_once) pthread_join(L.th, NULL);
    L.started_once = false;
    L.stop = false;
    atomic_store(&L.running, true);
    if (pthread_create(&L.th, NULL, fn, NULL) != 0) {
        atomic_store(&L.running, false);
        return;
    }
    L.started_once = true;
}

/* The screen given or taken away after a run: its part again, now. */
static void screen_again(void) {
    if (atomic_load(&L.running) || !L.has_run || !L.m) return;
    if (L.run_mode != MODE_REPLAY) {        /* kept with the captures */
        char out[1300];
        screen_into(L.work, L.screen, out, sizeof(out));
    }
    after_run(screen_worker);
}

bool la_window_takes_picture(void) { return L.open && !atomic_load(&L.running); }

int la_window_wants_picture(void) {
    int w = L.want_pic;
    L.want_pic = 0;
    return w;
}

static void set_screen_none(void) {
    thumbtex_free(&L.screen_tex);
    L.screen[0] = 0;
    L.screen_msg[0] = 0;
}

/* The screen's picture made small for the row (false if unreadable). */
static bool load_thumb(const char *path) {
    FILE *f = fopen(path, "rb");
    unsigned char *data = NULL;
    long n = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        data = n > 0 && n < 64L * 1024 * 1024 ? malloc((size_t)n) : NULL;
        if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
        fclose(f);
    }
    int w = 0, h = 0;
    uint32_t *px = data ? atlas_small_picture(data, (int)n, 320, &w, &h) : NULL;
    free(data);
    if (!px) return false;
    thumbtex_free(&L.screen_tex);
    thumbtex_make(&L.screen_tex, px, w, h);
    free(px);
    return true;
}

void la_window_set_picture(const char *path) {
    pthread_once(&once, init_sync);
    if (!load_thumb(path)) {
        snprintf(L.screen_msg, sizeof(L.screen_msg), "Can't read that picture: %s", path);
        return;
    }
    pthread_mutex_lock(&L.mu);
    snprintf(L.screen, sizeof(L.screen), "%s", path);
    pthread_mutex_unlock(&L.mu);
    const char *nm = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') nm = p + 1;
    snprintf(L.screen_msg, sizeof(L.screen_msg), "%s", nm);
    screen_again();
}

/* ---- The analyser's two boards: which is the master, Swap ---- */

static void boards_rows(struct nk_context *ctx, float cw) {
    char t[200];
    for (int i = 0; i < 2; i++) {
        nk_layout_row_dynamic(ctx, 20, 1);
        snprintf(t, sizeof(t), "%s %.60s, serial %.60s", i == 0 ? "Master (nearest the Spectrum):" : "Slave:", L.bp[i].port,
                 L.bp[i].serial[0] ? L.bp[i].serial : "?");
        nk_label_colored(ctx, t, NK_TEXT_LEFT, i == 0 ? COL_TEXT : COL_DIMTEXT);
    }
    nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
    nk_layout_row_push(ctx, 90);
    if (nk_widget_is_hovered(ctx))
        nk_tooltip(ctx, "The other board is the master: swap them, and remember it for next time");
    if (nk_button_label(ctx, "Swap")) {
        lp_port_t x = L.bp[0]; L.bp[0] = L.bp[1]; L.bp[1] = x;
        snprintf(L.ports, sizeof(L.ports), "%s,%s", L.bp[0].port, L.bp[1].port);
        L.bp_known = la_boards_remember(L.bp[0].serial, L.bp[1].serial);
    }
    nk_layout_row_push(ctx, fmaxf(120, cw - 100));
    if (L.bp_known)
        nk_label_colored(ctx, "Remembered from before (la-boards.txt)", NK_TEXT_LEFT, nk_rgb(90, 200, 110));
    else {
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, "Not sure which is which? Unplug the slave's USB lead and press Find boards: the one left is the "
                            "master. Wrong way round, a working Spectrum reads as CLK stuck, /WAIT and /BUSRQ low.");
        nk_label_colored(ctx, "A guess: check it, Swap if wrong (hover for how)", NK_TEXT_LEFT,
                         COL_FAULT);
    }
    nk_layout_row_end(ctx);
}

/* ---- "Pick a type...": the atlas's screens of the board, by kind ---- */

#define PICK_NCAT 9
#define PICK_MAXV 12
static const char *const pick_name[PICK_NCAT] = {
    "No picture (no video signal)", "One colour all over", "A blank screen inside a border",
    "Wide vertical bars", "Thin vertical stripes", "A repeating pattern (lines, checks)",
    "Coloured blocks all over", "Garbled text, stray marks", "The copyright message (looks right)" };
static struct {
    atlas_t a;
    bool loaded;
    int machine;                    /* the atlas board */
    int v[PICK_NCAT][PICK_MAXV], nv[PICK_NCAT];
    char label[PICK_NCAT][PICK_MAXV][256];
    thumbtex_t tex[PICK_NCAT][PICK_MAXV];
    char err[1300];
} P;

static int pick_kind(const atlas_outcome_t *o, const char *label) {
    if (o->h.t.no_signal || strstr(label, "signal")) return 0;
    if (!strncmp(label, "Solid", 5)) return 1;
    if (!strncmp(label, "Blank", 5)) return 2;
    if (!strncmp(label, "Vertical bars", 13)) {
        const char *w = strstr(label, "pixels wide");
        int px = 0;
        if (w) {
            const char *q = w;
            while (q > label && q[-1] == ' ') q--;
            while (q > label && isdigit((unsigned char)q[-1])) q--;
            px = atoi(q);
        }
        return px >= 16 ? 3 : 4;
    }
    if (!strncmp(label, "Repeating", 9)) return 5;
    if (!strncmp(label, "Attribute", 9)) return 6;
    if (strstr(label, "copyright") || strstr(label, "booted")) return 8;
    return 7;
}

static void pick_close(void) {
    for (int c = 0; c < PICK_NCAT; c++)
        for (int k = 0; k < PICK_MAXV; k++) thumbtex_free(&P.tex[c][k]);
    if (P.loaded) atlas_free(&P.a);
    P.loaded = false;
    L.picking = false;
}

static void pick_open(const machine_t *m) {
    pick_close();
    memset(P.nv, 0, sizeof(P.nv));
    P.err[0] = 0;
    bool exists = false;
    if (!atlas_load(&P.a, atlas_path, &exists) || !exists) {
        snprintf(P.err, sizeof(P.err), "No fault atlas (%s): the screens come from it", atlas_path);
        L.picking = true;
        L.pick_cat = -1;
        return;
    }
    P.loaded = true;
    int am = atlas_machine_of(m);
    for (int pass = 0; pass < 2 && am >= 0; pass++) {
        P.machine = pass ? atlas_family(am) : am;
        /* Each kind's pictures, the commonest (most faults) first. */
        for (int i = 0; i < P.a.noutcomes; i++) {
            const atlas_outcome_t *o = &P.a.outcomes[i];
            if (o->h.machine != P.machine || o->h.mode != MODE_BOOT || !o->thumb) continue;
            char label[256];
            atlas_outcome_label(o, label, sizeof(label));
            int c = pick_kind(o, label), n = P.nv[c];
            int at = n;
            while (at > 0 && P.a.outcomes[P.v[c][at - 1]].nfaults < o->nfaults) at--;
            if (at >= PICK_MAXV) continue;
            if (n == PICK_MAXV) n--;
            for (int k = n; k > at; k--) {
                P.v[c][k] = P.v[c][k - 1];
                snprintf(P.label[c][k], sizeof(P.label[0][0]), "%s", P.label[c][k - 1]);
            }
            P.v[c][at] = i;
            snprintf(P.label[c][at], sizeof(P.label[0][0]), "%s", label);
            P.nv[c] = n + 1;
        }
        int any = 0;
        for (int c = 0; c < PICK_NCAT; c++) any += P.nv[c];
        if (any || P.machine == atlas_family(am)) break;
    }
    const uint32_t *pal = atlas_palette(P.machine);
    for (int c = 0; c < PICK_NCAT; c++)
        for (int k = 0; k < P.nv[c]; k++) {
            const atlas_outcome_t *o = &P.a.outcomes[P.v[c][k]];
            int n = o->h.w * o->h.h;
            uint8_t *idx = malloc((size_t)n);
            uint32_t *px = malloc((size_t)n * sizeof(uint32_t));
            if (idx && px && atlas_decode_thumb(o, false, idx)) {
                for (int q = 0; q < n; q++) px[q] = pal[idx[q] & 15];
                thumbtex_make(&P.tex[c][k], px, o->h.w, o->h.h);
            }
            free(idx);
            free(px);
        }
    L.picking = true;
    L.pick_cat = -1;
}

/* The picked screen as a picture file (twice the thumbnail's size, a BMP
 * the matcher reads like a screenshot): its path, or NULL. */
static const char *pick_write(const atlas_outcome_t *o, char *path, int len) {
    int w = o->h.w, h = o->h.h, n = w * h;
    uint8_t *idx = malloc((size_t)n);
    if (!idx || !atlas_decode_thumb(o, false, idx)) { free(idx); return NULL; }
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMPDIR");
    snprintf(path, (size_t)len, "%s/la-picked-screen.bmp", tmp ? tmp : ".");
    FILE *f = fopen(path, "wb");
    if (!f) { free(idx); return NULL; }
    int W = w * 2, H = h * 2, row = (W * 3 + 3) & ~3;
    uint32_t size = 54u + (uint32_t)row * (uint32_t)H;
    uint8_t hd[54] = { 'B', 'M' };
    #define PUT32(o_, v_) do { uint32_t x_ = (uint32_t)(v_); hd[o_] = (uint8_t)x_; hd[o_ + 1] = (uint8_t)(x_ >> 8); \
                               hd[o_ + 2] = (uint8_t)(x_ >> 16); hd[o_ + 3] = (uint8_t)(x_ >> 24); } while (0)
    PUT32(2, size); PUT32(10, 54); PUT32(14, 40); PUT32(18, W); PUT32(22, H);
    hd[26] = 1; hd[28] = 24;
    PUT32(34, (uint32_t)row * (uint32_t)H);
    #undef PUT32
    fwrite(hd, 1, 54, f);
    const uint32_t *pal = atlas_palette(P.machine);
    uint8_t *line = calloc((size_t)row, 1);
    for (int y = H - 1; y >= 0 && line; y--) {         /* bottom up */
        for (int x = 0; x < W; x++) {
            uint32_t c = pal[idx[(y / 2) * w + x / 2] & 15];   /* RGBA8: R in the low byte */
            line[x * 3] = (uint8_t)(c >> 16);
            line[x * 3 + 1] = (uint8_t)(c >> 8);
            line[x * 3 + 2] = (uint8_t)c;
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    free(idx);
    fclose(f);
    return path;
}

static void pick_image(struct nk_context *ctx, const thumbtex_t *t) {
    struct nk_rect r;
    if (nk_widget(&r, ctx) == NK_WIDGET_INVALID) return;
    if (!t->live) {                 /* no signal: no picture kept */
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        nk_fill_rect(cv, r, 2, nk_rgb(20, 20, 22));
        text_centered(cv, ctx, r, "no signal", nk_rgb(170, 170, 170));
        return;
    }
    float w = r.w, h = t->w > 0 ? r.w * t->h / t->w : r.h;
    if (h > r.h) { h = r.h; w = t->h > 0 ? h * t->w / t->h : r.w; }
    struct nk_image img = thumbtex_image(t);
    nk_draw_image(nk_window_get_canvas(ctx), nk_rect(r.x, r.y, w, h), &img, nk_rgb(255, 255, 255));
}

static void set_picked(int c, int k) {
    char path[1100];
    const atlas_outcome_t *o = &P.a.outcomes[P.v[c][k]];
    char label[256];
    snprintf(label, sizeof(label), "%s", P.label[c][k]);
    if (!pick_write(o, path, sizeof(path))) {
        snprintf(P.err, sizeof(P.err), "Couldn't write the picked screen to the temp folder");
        return;
    }
    pick_close();
    la_window_set_picture(path);
    snprintf(L.screen_msg, sizeof(L.screen_msg), "picked: %s", label);
}


/* The picker, where the text goes. */
static int settle;          /* frames since a kind was opened */
static void pick_view(struct nk_context *ctx, float cw, float height) {
    nk_layout_row_dynamic(ctx, height, 1);
    if (!nk_group_begin(ctx, "la pick", 0)) return;
    nk_layout_row_dynamic(ctx, 22, 1);
    if (P.err[0]) {
        nk_label_colored(ctx, P.err, NK_TEXT_LEFT, COL_FAULT);
    } else if (L.pick_cat < 0) {
        nk_label_colored(ctx, "What does the Spectrum's screen show? Pick the kind nearest to it:", NK_TEXT_LEFT, COL_HOVER);
        for (int c = 0; c < PICK_NCAT; c++) {
            if (!P.nv[c]) continue;
            nk_layout_row_begin(ctx, NK_STATIC, 66, 2);
            nk_layout_row_push(ctx, 88);
            bool hit = nk_widget_is_mouse_clicked(ctx, NK_BUTTON_LEFT);
            pick_image(ctx, &P.tex[c][0]);
            nk_layout_row_push(ctx, fmaxf(120, cw - 120));
            char t[120];
            snprintf(t, sizeof(t), "%s  (%d kind%s)", pick_name[c], P.nv[c], P.nv[c] == 1 ? "" : "s");
            if (nk_button_label(ctx, t) || hit) { L.pick_cat = c; settle = 0; }
            nk_layout_row_end(ctx);
        }
    } else {
        int c = L.pick_cat;
        char t[160];
        snprintf(t, sizeof(t), "%s: pick the nearest (hover for what it is)", pick_name[c]);
        nk_label_colored(ctx, t, NK_TEXT_LEFT, COL_HOVER);
        int per = (int)fmaxf(1, (cw - 10) / 140);
        settle++;           /* the click that opened it isn't a pick */
        for (int k = 0; k < P.nv[c]; k++) {
            if (k % per == 0) nk_layout_row_static(ctx, 100, 132, per);
            bool hov = nk_widget_is_hovered(ctx);
            if (hov) nk_tooltip(ctx, P.label[c][k]);
            bool hit = nk_widget_is_mouse_clicked(ctx, NK_BUTTON_LEFT);
            pick_image(ctx, &P.tex[c][k]);
            if (hit && settle > 3) { set_picked(c, k); break; }
        }
        if (L.picking) {
            nk_layout_row_static(ctx, 30, 120, 1);
            if (nk_button_label(ctx, "Back")) L.pick_cat = -1;
        }
    }
    if (L.picking) {
        nk_layout_row_static(ctx, 30, 120, 1);
        if (nk_button_label(ctx, "Cancel")) pick_close();
    }
    nk_group_end(ctx);
}

/* The screen's row: its picture small, Choose... / Paste / None. */
static void screen_row(struct nk_context *ctx, float cw, const machine_t *machine) {
    static const char *tip = "A screenshot of the Spectrum's monitor (a capture card's, PNG, JPG or BMP): matched against "
                             "the fault atlas with what the bus shows. Or drop it on this window, or Ctrl+V";
    nk_layout_row_begin(ctx, NK_STATIC, 26, 6);
    nk_layout_row_push(ctx, 60);
    nk_label(ctx, "Screen", NK_TEXT_LEFT);
    nk_layout_row_push(ctx, fmaxf(80, cw - 60 - 3 * 76 - 104 - 28));
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, tip);
    nk_label_colored(ctx, L.screen[0] ? L.screen_msg : L.screen_msg[0] ? L.screen_msg : "none (optional)", NK_TEXT_LEFT,
                     L.screen[0] ? COL_TEXT : L.screen_msg[0] ? COL_FAULT : COL_DIMTEXT);
    nk_layout_row_push(ctx, 76);
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, tip);
    if (nk_button_label(ctx, "Choose...")) L.want_pic = 1;
    nk_layout_row_push(ctx, 76);
    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "The picture on the clipboard (Ctrl+V)");
    if (nk_button_label(ctx, "Paste")) L.want_pic = 2;
    nk_layout_row_push(ctx, 104);
    if (nk_widget_is_hovered(ctx))
        nk_tooltip(ctx, "No screenshot? Pick what the screen looks like from the simulated faults' screens");
    if (nk_button_label(ctx, "Pick a type...")) {
        if (L.picking) pick_close();
        else pick_open(machine);
    }
    nk_layout_row_push(ctx, 76);
    if (L.screen[0]) {
        if (nk_button_label(ctx, "None")) {
            set_screen_none();
            screen_again();
        }
    } else nk_spacing(ctx, 1);
    nk_layout_row_end(ctx);
    static char tried[1100];
    if (L.screen[0] && !L.screen_tex.live && strcmp(tried, L.screen)) {    /* given before the window drew */
        snprintf(tried, sizeof(tried), "%s", L.screen);
        load_thumb(L.screen);
    }
    if (L.screen[0] && L.screen_tex.live) {
        nk_layout_row_begin(ctx, NK_STATIC, 72, 2);
        nk_layout_row_push(ctx, 60);
        nk_spacing(ctx, 1);
        nk_layout_row_push(ctx, 96);
        struct nk_rect r;
        if (nk_widget(&r, ctx) != NK_WIDGET_INVALID) {
            const thumbtex_t *t = &L.screen_tex;
            float w = r.w, h = t->w > 0 ? r.w * t->h / t->w : r.h;
            if (h > r.h) { h = r.h; w = t->h > 0 ? h * t->w / t->h : r.w; }
            struct nk_image img = thumbtex_image(t);
            nk_draw_image(nk_window_get_canvas(ctx), nk_rect(r.x, r.y, w, h), &img, nk_rgb(255, 255, 255));
        }
        nk_layout_row_end(ctx);
    }
}

static void answer(int a) {
    pthread_mutex_lock(&L.mu);
    L.answer = a;
    pthread_cond_broadcast(&L.cv);
    pthread_mutex_unlock(&L.mu);
}

void la_window_open(void) {
    pthread_once(&once, init_sync);
    L.open = L.reopen = true;
    if (!L.base[0]) {
        /* Next to the captures diagnose.bat takes, if that folder is here. */
        struct stat st;
        snprintf(L.base, sizeof(L.base), "%s",
                 !stat("../Logic Analyser", &st) && (st.st_mode & S_IFDIR) ? "../Logic Analyser" : ".");
        snprintf(L.work, sizeof(L.work), "%s/last-diagnosis", L.base);
        snprintf(L.folder, sizeof(L.folder), "%s", L.work);
        snprintf(L.save, sizeof(L.save), "%s/diagnosis-", L.base);
        diagnose_atlas_file(atlas_path, sizeof(atlas_path));
    }
}
bool la_window_is_open(void) { return L.open; }

static void test_screen(const char *path) {
    snprintf(L.screen, sizeof(L.screen), "%s", path);
    const char *nm = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') nm = p + 1;
    snprintf(L.screen_msg, sizeof(L.screen_msg), "%s", nm);
}

void la_window_open_test(const char *spec) {
    la_window_open();
    if (!strncmp(spec, "replay:", 7)) {
        L.mode = MODE_REPLAY;
        const char *b = strchr(spec + 7, '|');
        snprintf(L.folder, sizeof(L.folder), "%.*s", b ? (int)(b - spec - 7) : (int)strlen(spec + 7), spec + 7);
        if (b) test_screen(b + 1);
    } else if (!strncmp(spec, "simulate:", 9)) {
        L.mode = MODE_SIMULATE;
        const char *c = strchr(spec + 9, '|');
        if (!c) return;
        snprintf(L.fault, sizeof(L.fault), "%.*s", (int)(c - spec - 9), spec + 9);
        const char *b = strchr(c + 1, '|');
        snprintf(L.folder, sizeof(L.folder), "%.*s", b ? (int)(b - c - 1) : (int)strlen(c + 1), c + 1);
        if (b) test_screen(b + 1);
    } else if (!strncmp(spec, "demo:", 5)) {
        /* How it looks with the analyser, for screenshots: "demo:ask",
         * "demo:armed", "demo:triggered". */
        L.has_run = true;
        L.mode = L.run_mode = MODE_ANALYSER;
        L.stage_st[0] = LA_ST_OK;
        L.stage_st[1] = LA_ST_OK;
        L.stage_st[2] = LA_ST_RUN;
        L.cur_stage = 2;
        text_put("First, idle: what it does left running\n  OK: the same as a working one\n\n1 of 6, reset: ...\n"
                 "  OK: the same as a working one\n\n2 of 6, ramcheck: the RAM test reading back\n");
        atomic_store(&L.running, true);
        if (!strcmp(spec + 5, "ask")) {
            L.asking = true;
            snprintf(L.prompt, sizeof(L.prompt),
                     "Hold the Spectrum's reset button down, then press Continue (keep holding the button)");
        } else if (!strcmp(spec + 5, "armed")) {
            L.sign = 1;
            snprintf(L.doing, sizeof(L.doing), "LET GO OF RESET");
        } else {
            L.sign = 2;
            snprintf(L.doing, sizeof(L.doing), "Reading the samples, then comparing with a working board");
        }
        return;
    } else return;
    L.start_pending = true;
}
void la_window_close(void) { L.open = false; }
bool la_window_bounds(struct nk_rect *r) {
    if (!L.open) return false;
    *r = L.bounds;
    return true;
}
bool la_window_wants_keyboard(void) { return L.open && L.typing; }

int la_window_mark(const machine_t *machine, const char *ref) {
    if (!L.nmarks || machine != L.m || atomic_load(&L.running)) return 0;
    for (int k = 0; k < L.nmarks; k++)
        if (!strcmp(L.mark_ref[k], ref)) return L.mark_level[k];
    return 0;
}

void la_window_shutdown(void) {
    if (!L.started_once) return;
    pthread_mutex_lock(&L.mu);
    L.stop = true;
    pthread_cond_broadcast(&L.cv);
    pthread_mutex_unlock(&L.mu);
    if (!atomic_load(&L.running)) pthread_join(L.th, NULL);
    else pthread_detach(L.th);        /* in a capture or a simulation: it ends with the program */
    L.started_once = false;
}

static bool copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return false;
    FILE *b = fopen(to, "wb");
    if (!b) { fclose(a); return false; }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0)
        if (fwrite(buf, 1, n, b) != n) { ok = false; break; }
    fclose(a);
    if (fclose(b)) ok = false;
    return ok;
}

/* Does DIR have any files in it? */
static bool has_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool any = false;
    while (!any && (e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) any = true;
    closedir(d);
    return any;
}

static void save_run(void) {
    size_t n = strlen(L.save);
    while (n && (L.save[n - 1] == '/' || L.save[n - 1] == '\\' || L.save[n - 1] == ' ')) L.save[--n] = 0;
    char empty[600];
    snprintf(empty, sizeof(empty), "%s/diagnosis-", L.base);
    if (!n || !strcmp(L.save, empty)) {
        snprintf(L.saved, sizeof(L.saved), "Type a name for the board after diagnosis- first");
        return;
    }
    if (!strcmp(L.save, L.work)) {
        snprintf(L.saved, sizeof(L.saved), "That's the folder it works in: choose another");
        return;
    }
    if (has_files(L.save)) {
        snprintf(L.saved, sizeof(L.saved), "%s has files in it already: choose another name", L.save);
        return;
    }
#ifdef _WIN32
    _mkdir(L.save);
#else
    mkdir(L.save, 0777);
#endif
    DIR *d = opendir(L.work);
    if (!d) { snprintf(L.saved, sizeof(L.saved), "Couldn't read %s", L.work); return; }
    struct dirent *e;
    int files = 0, bad = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[1200], b[1200];
        snprintf(a, sizeof(a), "%s/%s", L.work, e->d_name);
        snprintf(b, sizeof(b), "%s/%s", L.save, e->d_name);
        if (copy_file(a, b)) files++;
        else bad++;
    }
    closedir(d);
    if (bad) snprintf(L.saved, sizeof(L.saved), "Couldn't save %d of the files to %s", bad, L.save);
    else {
        snprintf(L.saved, sizeof(L.saved), "Saved (%d files) to %s", files, L.save);
        snprintf(L.folder, sizeof(L.folder), "%s", L.save);    /* to diagnose again from */
    }
}

static void field(struct nk_context *ctx, const char *label, char *buf, int len, float lw, float cw, const char *tip) {
    nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
    nk_layout_row_push(ctx, lw);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, cw - lw - 8);
    if (tip && nk_widget_is_hovered(ctx)) nk_tooltip(ctx, tip);
    nk_flags ev = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, buf, len, nk_filter_default);
    if (ev & NK_EDIT_ACTIVE) L.typing = true;
    nk_layout_row_end(ctx);
}

/* A button in a color of its own (the one to press now). */
static bool color_button(struct nk_context *ctx, const char *label, struct nk_color bg, struct nk_color fg) {
    struct nk_style_button *b = &ctx->style.button;
    struct nk_color hov = nk_rgb(bg.r + (255 - bg.r) / 5, bg.g + (255 - bg.g) / 5, bg.b + (255 - bg.b) / 5);
    nk_style_push_style_item(ctx, &b->normal, nk_style_item_color(bg));
    nk_style_push_style_item(ctx, &b->hover, nk_style_item_color(hov));
    nk_style_push_style_item(ctx, &b->active, nk_style_item_color(hov));
    nk_style_push_color(ctx, &b->text_normal, fg);
    nk_style_push_color(ctx, &b->text_hover, fg);
    nk_style_push_color(ctx, &b->text_active, fg);
    nk_style_push_color(ctx, &b->border_color, hov);
    bool r = nk_button_label(ctx, label);
    nk_style_pop_color(ctx); nk_style_pop_color(ctx); nk_style_pop_color(ctx); nk_style_pop_color(ctx);
    nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx);
    return r;
}

#define COL_GO   nk_rgb(30, 150, 60)
#define COL_STOP nk_rgb(190, 40, 35)

/* Text in the big font (or the usual one), centered in R. */
static void big_text(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect r, const char *t,
                     struct nk_color c) {
    const struct nk_user_font *f = ui_big_font();
    if (!f) f = ctx->style.font;
    float w = f->width(f->userdata, f->height, t, (int)strlen(t));
    nk_draw_text(cv, nk_rect(r.x + (r.w - w) / 2, r.y + (r.h - f->height) / 2, w + 2, f->height), t, (int)strlen(t), f,
                 nk_rgba(0, 0, 0, 0), c);
}

/* The stages as boxes along the window: grey to come, blue now, green
 * OK, red wrong, dark crossed out skipped. A click shows its text. */
static void stage_strip(struct nk_context *ctx, const int *st, int cur, bool running) {
    static int frame;
    frame++;
    nk_layout_row_dynamic(ctx, 44, NSTAGES);
    for (int k = 0; k < NSTAGES; k++) {
        bool clicked = nk_widget_is_mouse_clicked(ctx, NK_BUTTON_LEFT);
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Show this stage's text below");
        struct nk_rect r;
        if (!nk_widget(&r, ctx)) continue;
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        struct nk_color fill, fg = nk_rgb(255, 255, 255);
        const char *what = "";
        switch (st[k]) {
        case LA_ST_RUN: {
            float pulse = 0.5f + 0.5f * sinf(frame * 0.12f);
            fill = nk_rgb(30 + (int)(40 * pulse), 90 + (int)(50 * pulse), 200 + (int)(40 * pulse));
            what = running ? "now" : "";
            break;
        }
        case LA_ST_OK: fill = nk_rgb(35, 140, 60); what = "OK"; break;
        case LA_ST_FAULT: fill = nk_rgb(200, 45, 40); what = "WRONG"; break;
        case LA_ST_NEVER: fill = nk_rgb(200, 45, 40); what = "never came"; break;
        case LA_ST_SKIP: fill = nk_rgb(45, 45, 48); fg = nk_rgb(120, 120, 120); what = "skipped"; break;
        default: fill = nk_rgb(75, 75, 80); fg = nk_rgb(170, 170, 170); what = ""; break;
        }
        (void)cur;
        nk_fill_rect(cv, r, 4, fill);
        if (st[k] == LA_ST_SKIP) nk_stroke_line(cv, r.x + 4, r.y + r.h - 4, r.x + r.w - 4, r.y + 4, 1.5f, nk_rgb(110, 110, 110));
        float fh = ctx->style.font->height;
        text_centered(cv, ctx, nk_rect(r.x, r.y + 4, r.w, fh + 2), stage_label[k], fg);
        if (what[0]) text_centered(cv, ctx, nk_rect(r.x, r.y + r.h - fh - 6, r.w, fh + 2), what, fg);
        if (clicked) {
            pthread_mutex_lock(&L.mu);
            if (st[k] != LA_ST_NONE) L.jump = (long)L.stage_off[k];
            pthread_mutex_unlock(&L.mu);
        }
    }
}

void la_window(struct nk_context *ctx, struct nk_rect area, const machine_t *machine) {
    if (!L.open) return;
    pthread_once(&once, init_sync);
    float w = fminf(720, fmaxf(480, area.w * 0.5f)), h = area.h - 32;
    if (w > area.w - 32) w = area.w - 32;
    struct nk_rect r = nk_rect(area.x + 16, area.y + 16, w, h);
    if (L.reopen) {
        nk_window_show(ctx, "LogicAnalyser", NK_SHOWN);
        nk_window_set_bounds(ctx, "LogicAnalyser", r);
        nk_window_set_focus(ctx, "LogicAnalyser");
        L.reopen = false;
    }
    L.typing = false;
    if (!nk_begin_titled(ctx, "LogicAnalyser", "Diagnose with the logic analyser", r,
                         NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE |
                         NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR)) {
        L.open = false;
        nk_end(ctx);
        return;
    }
    L.bounds = nk_window_get_bounds(ctx);
    float cw = nk_window_get_content_region(ctx).w - 20;
    bool zx = is_zx48(machine);
    if (L.start_pending && zx) { L.start_pending = false; start(machine); }
    bool running = atomic_load(&L.running);

    pthread_mutex_lock(&L.mu);
    bool asking = L.asking;
    char prompt[600], doing[200];
    snprintf(prompt, sizeof(prompt), "%s", L.prompt);
    snprintf(doing, sizeof(doing), "%s", L.doing);
    int st[NSTAGES];
    memcpy(st, L.stage_st, sizeof(st));
    int cur = L.cur_stage, sign = L.sign;
    bool stopping = L.stop && running;
    pthread_mutex_unlock(&L.mu);

    if (!L.has_run) {
        /* ---- the setup: what to diagnose, and Start */
        if (!zx) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label_colored(ctx, "Put a ZX Spectrum on the bench first (the analyser reads its edge connector)",
                             NK_TEXT_LEFT, COL_FAULT);
        }
        static const char *const modes[] = { "The real Spectrum (analyser)", "Again, from the folder's captures",
                                             "The simulated board, with a fault" };
        static const char *const tips[] = {
            "Capture from the two LogicAnalyzer boards over USB: the Spectrum left running first, then its boot "
            "(you hold its reset button when asked)",
            "Diagnose the captures a run took and saved before (the folder below) again, without the analyser",
            "The simulated board with the fault below as the one under test: to see what the diagnosis says of it" };
        for (int k = 0; k < 3; k++) {
            nk_layout_row_dynamic(ctx, 22, 1);
            if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, tips[k]);
            if (nk_option_label(ctx, modes[k], L.mode == k)) L.mode = k;
        }
        if (L.mode == MODE_REPLAY)
            field(ctx, "Folder", L.folder, sizeof(L.folder), 70, cw,
                  "A folder of captures a run took and saved (diagnosis-NAME)");
        if (L.mode == MODE_ANALYSER) {
            nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
            nk_layout_row_push(ctx, 70);
            nk_label(ctx, "Ports", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, 220);
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "MASTER,OTHER: the board nearest the Spectrum first (a COM port or a serial number); "
                                "empty with one board");
            nk_flags ev = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, L.ports, sizeof(L.ports), nk_filter_default);
            if (ev & NK_EDIT_ACTIVE) L.typing = true;
            nk_layout_row_push(ctx, 120);
            if (nk_button_label(ctx, "Find boards")) {
                L.nbp = la_boards(L.bp, 8, &L.bp_known);
                L.boards[0] = 0;
                if (!L.nbp) snprintf(L.boards, sizeof(L.boards), "No LogicAnalyzer boards found (plugged in?)");
                else if (L.nbp == 1) snprintf(L.ports, sizeof(L.ports), "%s", L.bp[0].port);
                else if (L.nbp == 2) snprintf(L.ports, sizeof(L.ports), "%s,%s", L.bp[0].port, L.bp[1].port);
                else {
                    int m = snprintf(L.boards, sizeof(L.boards), "%d boards found (two are used):", L.nbp);
                    for (int i = 0; i < L.nbp && m < (int)sizeof(L.boards); i++)
                        m += snprintf(L.boards + m, sizeof(L.boards) - m, " %s (serial %s)", L.bp[i].port, L.bp[i].serial);
                }
            }
            nk_layout_row_end(ctx);
            if (L.boards[0]) {
                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label_colored(ctx, L.boards, NK_TEXT_LEFT, COL_DIMTEXT);
            }
            if (L.nbp == 2) boards_rows(ctx, cw);
        } else if (L.mode == MODE_SIMULATE)
            field(ctx, "Fault", L.fault, sizeof(L.fault), 70, cw,
                  "As --fault: IC26:removed, IC2.26:gnd, IC12:data0=1... (empty: the working board)");
        screen_row(ctx, cw, machine);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_bool all = L.all;
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, "After a stage that's wrong, capture the later ones too: more to go on (one that never comes "
                            "ends it)");
        if (nk_checkbox_label(ctx, "After a wrong stage, take the rest too", &all)) L.all = all;
        nk_layout_row_dynamic(ctx, 8, 1);
        nk_spacing(ctx, 1);
        nk_layout_row_static(ctx, 34, 160, 2);
        if (zx && (L.mode != MODE_REPLAY || L.folder[0])) {
            if (color_button(ctx, "Start", COL_GO, nk_rgb(255, 255, 255))) {
            /* The boards as shown, started with: that's the order from now on. */
            if (L.mode == MODE_ANALYSER && L.nbp == 2 && !L.bp_known) {
                char want[200];
                snprintf(want, sizeof(want), "%s,%s", L.bp[0].port, L.bp[1].port);
                if (!strcmp(want, L.ports) && la_boards_remember(L.bp[0].serial, L.bp[1].serial)) L.bp_known = true;
            }
            start(machine);
        }
        } else nk_spacing(ctx, 1);
        if (nk_button_label(ctx, "Close")) L.open = false;
        if (L.picking) {
            struct nk_rect region = nk_window_get_content_region(ctx);
            float ph = region.y + region.h - (ctx->current->layout->at_y + ctx->current->layout->row.height) -
                       ctx->style.window.spacing.y - 4;
            pick_view(ctx, cw, ph < 160 ? 160 : ph);
        }
        nk_end(ctx);
        return;
    }

    /* ---- a run: the stages, what to do now, its text */
    stage_strip(ctx, st, cur, running);

    if (asking) {
        nk_layout_row_dynamic(ctx, 6, 1);
        nk_spacing(ctx, 1);
        nk_layout_row_dynamic(ctx, 40, 1);
        nk_label_colored_wrap(ctx, prompt, COL_HOVER);
        nk_layout_row_static(ctx, 36, 180, 3);
        if (color_button(ctx, "Continue (Enter)", COL_GO, nk_rgb(255, 255, 255))) answer(1);
        if (nk_button_label(ctx, "No")) answer(0);
        if (color_button(ctx, "Stop", COL_STOP, nk_rgb(255, 255, 255))) {
            pthread_mutex_lock(&L.mu);
            L.stop = true;
            pthread_cond_broadcast(&L.cv);
            pthread_mutex_unlock(&L.mu);
        }
        if (nk_input_is_key_pressed(&ctx->input, NK_KEY_ENTER) && !L.typing) answer(1);
    } else if (running) {
        if (sign) {
            /* ARMED (waiting) or TRIGGERED, big, and what to do now. */
            nk_layout_row_dynamic(ctx, 70, 1);
            struct nk_rect b;
            if (nk_widget(&b, ctx)) {
                struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
                struct nk_color fill = sign == 1 ? nk_rgb(200, 20, 20) : nk_rgb(240, 170, 0);
                nk_fill_rect(cv, b, 6, fill);
                nk_stroke_rect(cv, b, 6, 3, sign == 1 ? nk_rgb(255, 90, 80) : nk_rgb(255, 220, 120));
                big_text(cv, ctx, b, sign == 1 ? "ARMED" : "TRIGGERED", sign == 1 ? nk_rgb(255, 255, 255) : nk_rgb(20, 20, 20));
            }
            nk_layout_row_dynamic(ctx, 48, 1);
            if (nk_widget(&b, ctx) && doing[0]) {
                struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
                bool shout = strlen(doing) <= 20;
                if (shout) big_text(cv, ctx, b, doing, ink(COL_HOVER));
                else text_centered(cv, ctx, b, doing, ink(COL_HOVER));
            }
        } else {
            nk_layout_row_dynamic(ctx, 6, 1);
            nk_spacing(ctx, 1);
            char t[220];
            if (doing[0] && !stopping) snprintf(t, sizeof(t), "%s...", doing);
            else
            snprintf(t, sizeof(t), "%s: %s", stage_label[cur],
                     stopping ? "stopping, at its next question or when this capture or simulation ends..."
                              : L.run_mode == MODE_ANALYSER ? "working..." : "simulating and comparing...");
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label_colored(ctx, t, NK_TEXT_LEFT, COL_PULSE);
        }
        nk_layout_row_static(ctx, 34, 160, 1);
        if (!stopping && color_button(ctx, "Stop", COL_STOP, nk_rgb(255, 255, 255))) {
            pthread_mutex_lock(&L.mu);
            L.stop = true;
            pthread_cond_broadcast(&L.cv);
            pthread_mutex_unlock(&L.mu);
        }
    } else {
        /* Done: the verdict, saving it, starting again. */
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label_colored(ctx, L.rc_text, NK_TEXT_LEFT, L.rc == 0 ? COL_TRACE : COL_FAULT);
        if (L.summary[0]) {
            char t[480];
            snprintf(t, sizeof(t), "Outlined on the board: %s", L.summary);
            nk_layout_row_dynamic(ctx, 40, 1);
            nk_label_colored_wrap(ctx, t, COL_FAULT);
        }
        if (L.run_mode != MODE_REPLAY) {
            /* Kept only if saved: the next run starts with an empty folder. */
            nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
            nk_layout_row_push(ctx, 70);
            nk_label(ctx, "Save as", NK_TEXT_LEFT);
            nk_layout_row_push(ctx, fmaxf(120, cw - 70 - 8 - 120 - 8));
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "The folder to keep this run's captures and diagnosis.txt in: type the board's name "
                                "after diagnosis-");
            nk_flags ev = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, L.save, sizeof(L.save), nk_filter_default);
            if (ev & NK_EDIT_ACTIVE) L.typing = true;
            nk_layout_row_push(ctx, 120);
            if (nk_button_label(ctx, "Save")) save_run();
            nk_layout_row_end(ctx);
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label_colored(ctx, L.saved[0] ? L.saved : "Not saved: the next run starts afresh (Save keeps this one)",
                             NK_TEXT_LEFT, !strncmp(L.saved, "Saved", 5) ? COL_TRACE : COL_DIMTEXT);
        }
        screen_row(ctx, cw, machine);
        pthread_mutex_lock(&L.mu);
        bool had_screen = L.screen[0] || (L.text && strstr(L.text, "\nThe screen ("));
        pthread_mutex_unlock(&L.mu);
        if (had_screen && L.rc != 1) {
            nk_layout_row_static(ctx, 30, 330, 1);
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "Put each of the screen's faults (24 at most) on the simulated board and compare every "
                                "stage with the analyser's captures: the fault that does what this board did. A minute or "
                                "two, on half the processors");
            if (nk_button_label(ctx, "Check the screen's faults against the captures")) after_run(check_worker);
        }
        nk_layout_row_static(ctx, 34, 160, 3);
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, "Clear these results and the outlines on the board, and choose what to diagnose next");
        if (color_button(ctx, "New diagnosis", COL_GO, nk_rgb(255, 255, 255))) {
            clear_results();
            set_screen_none();
            L.has_run = false;
        }
        if (L.nmarks) {
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "Close the window to see the board: the outlines stay (open it again from Diagnose)");
            if (nk_button_label(ctx, "Show the board")) L.open = false;
        }
        if (nk_button_label(ctx, "Close")) L.open = false;
    }

    /* What it printed, the newest at the bottom (or a clicked stage's). */
    struct nk_rect region = nk_window_get_content_region(ctx);
    float list_h = region.y + region.h - (ctx->current->layout->at_y + ctx->current->layout->row.height) -
                   ctx->style.window.spacing.y - 4;
    if (list_h < 120) list_h = 120;
    if (L.picking && !running) {
        pick_view(ctx, cw, list_h);
        nk_end(ctx);
        return;
    }
    nk_layout_row_dynamic(ctx, list_h, 1);
    static nk_uint sx, sy;
    if (nk_group_scrolled_offset_begin(ctx, &sx, &sy, "la text", 0)) {
        pthread_mutex_lock(&L.mu);
        const char *t = L.text ? L.text : "";
        size_t tl = L.len;
        long jump = L.jump;
        L.jump = -1;
        float y = 0, jump_y = -1, lh = 16, gap = ctx->style.window.spacing.y;
        for (const char *line = t; line < t + tl; ) {
            const char *end = memchr(line, '\n', (size_t)(t + tl - line));
            size_t len = end ? (size_t)(end - line) : (size_t)(t + tl - line);
            if (jump >= 0 && jump_y < 0 && line - t >= jump) jump_y = y;
            char buf[512];
            snprintf(buf, sizeof(buf), "%.*s", (int)(len < sizeof(buf) - 1 ? len : sizeof(buf) - 1), line);
            const struct nk_user_font *f = ctx->style.font;
            float avail = nk_window_get_content_region(ctx).w - 2 * ctx->style.window.group_padding.x -
                          ctx->style.window.scrollbar_size.x - 8;
            float tw = f->width(f->userdata, f->height, buf, (int)strlen(buf));
            int rows = avail > 50 ? (int)(tw / avail) + 1 : 1;
            if (rows > 6) rows = 6;
            nk_layout_row_dynamic(ctx, lh * rows, 1);
            struct nk_color c = COL_TEXT;
            if (strstr(buf, "FAULT") || strstr(buf, "Wrong too") || strstr(buf, "It never")) c = COL_FAULT;
            else if (!strncmp(buf, "  OK", 4) || !strncmp(buf, "  It agrees", 11)) c = COL_TRACE;
            else if (buf[0] == ' ' && buf[1] == ' ' && isdigit((unsigned char)buf[2])) c = COL_PULSE;
            if (rows > 1) nk_label_colored_wrap(ctx, buf, ink(c));
            else nk_label_colored(ctx, buf, NK_TEXT_LEFT, ink(c));
            y += lh * rows + gap;
            if (!end) break;
            line = end + 1;
        }
        bool more = tl != L.shown_len;
        L.shown_len = tl;
        pthread_mutex_unlock(&L.mu);
        if (jump_y >= 0) sy = (nk_uint)(jump_y > 4 ? jump_y - 4 : 0);
        else if (more) sy = (nk_uint)(y + 100);   /* down to the newest */
        nk_group_end(ctx);
    }
    nk_end(ctx);
}
