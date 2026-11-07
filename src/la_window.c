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

#define MAX_MARKS 48

enum { MODE_ANALYSER, MODE_REPLAY, MODE_SIMULATE };

static struct {
    bool open, reopen, typing;
    struct nk_rect bounds;
    int mode;
    char folder[512], ports[128], fault[128];
    bool all;
    bool start_pending;             /* --la-window: start on the next frame */
    char boards[400];               /* "Find boards": what was found */

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
} L = { .all = true, .answer = -1 };

static pthread_once_t once = PTHREAD_ONCE_INIT;
static void init_sync(void) {
    pthread_mutex_init(&L.mu, NULL);
    pthread_cond_init(&L.cv, NULL);
}

/* ---- from the diagnosis's thread ---------------------------------------- */

static void text_hook(const char *t) {
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
    snprintf(L.prompt, sizeof(L.prompt), "%s", prompt && prompt[0] ? prompt : "Continue when ready");
    size_t k = strlen(L.prompt);
    while (k && (L.prompt[k - 1] == '\n' || L.prompt[k - 1] == ' ')) L.prompt[--k] = 0;
    L.answer = -1;
    L.asking = true;
    while (L.answer < 0 && !L.stop) pthread_cond_wait(&L.cv, &L.mu);
    bool go = L.answer == 1 && !L.stop;
    L.asking = false;
    pthread_mutex_unlock(&L.mu);
    text_hook(go ? "  [continued]\n" : "  [no]\n");
    return go;
}

/* Is REF a part of board B? */
static bool is_part(board_t *b, const char *ref) {
    for (int i = 0; i < b->sim.nparts; i++)
        if (!strcmp(b->sim.parts[i].ref, ref)) return true;
    return false;
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

/* The chips the verdict names: after "What to look at", the items'
 * references (IC26, TR6, D2...), item 1's the first to look at; not the
 * notes (a line nothing on the board uses). */
static void find_marks(const char *text) {
    L.nmarks = 0;
    L.summary[0] = 0;
    const char *v = strstr(text, "What to look at");
    if (!v) return;
    board_t *b = board_create(L.m);
    if (!b) return;
    int item = 0;
    bool note = false;
    for (const char *line = v; *line; ) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        int num;
        if (sscanf(line, "  %d.", &num) == 1) { item = num; note = false; }
        else if (!strncmp(line, "  Note:", 7)) note = true;
        else if (len && line[0] != ' ') { if (line != v) break; }
        if (item && !note)
            for (size_t i = 0; i < len; ) {
                if (isupper((unsigned char)line[i]) && (i == 0 || !isalnum((unsigned char)line[i - 1]))) {
                    size_t j = i;
                    while (j < len && isupper((unsigned char)line[j])) j++;
                    size_t d = j;
                    while (d < len && isdigit((unsigned char)line[d])) d++;
                    if (d > j && d - i < 12 && (d >= len || !isalnum((unsigned char)line[d]))) {
                        char ref[16];
                        snprintf(ref, sizeof(ref), "%.*s", (int)(d - i), line + i);
                        if (is_part(b, ref)) add_mark(ref, item == 1 ? 2 : 1);
                    }
                    i = d > i ? d : i + 1;
                } else i++;
            }
        if (!end) break;
        line = end + 1;
    }
    board_destroy(b);
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

static void *worker(void *arg) {
    (void)arg;
    la_cli_t o;
    memset(&o, 0, sizeof(o));
    static char folder[512], ports[128], fault[128];
    static const char *faults[1];
    snprintf(folder, sizeof(folder), "%s", L.folder);
    snprintf(ports, sizeof(ports), "%s", L.ports);
    snprintf(fault, sizeof(fault), "%s", L.fault);
    o.diagnose = folder;
    o.machine = L.m;
    o.all = L.all;
    if (L.mode == MODE_ANALYSER) o.ports = ports[0] ? ports : NULL;
    else if (L.mode == MODE_REPLAY) o.replay = true;
    else {
        o.simulate = true;
        if (fault[0]) { faults[0] = fault; o.faults = faults; o.nfaults = 1; }
    }
    la_text_hook = text_hook;
    la_ask_hook = ask_hook;
    int rc = la_cli(&o);
    la_text_hook = NULL;
    la_ask_hook = NULL;
    pthread_mutex_lock(&L.mu);
    L.rc = rc;
    snprintf(L.rc_text, sizeof(L.rc_text), "%s",
             L.stop ? "Stopped." : rc == 0 ? "Done: no fault found." : rc == 2 ? "Done: the verdict is above." :
             "It stopped before a verdict (see the last lines above).");
    char *copy = L.text ? strdup(L.text) : NULL;
    pthread_mutex_unlock(&L.mu);
    if (copy) { find_marks(copy); free(copy); }
    atomic_store(&L.running, false);
    return NULL;
}

/* ---- the window ----------------------------------------------------------- */

static bool is_zx48(const machine_t *m) {
    return m == &machine_zx48 || m == &machine_zx48i2 || m == &machine_zx48i1 || m == &machine_zx48i4b ||
           m == &machine_zx48i6a;
}

static void start(const machine_t *m) {
    pthread_once(&once, init_sync);
    if (atomic_load(&L.running)) return;
    if (L.started_once) pthread_join(L.th, NULL);
    pthread_mutex_lock(&L.mu);
    L.len = 0;
    if (L.text) L.text[0] = 0;
    L.shown_len = 0;
    L.stop = false;
    L.asking = false;
    L.answer = -1;
    L.rc_text[0] = 0;
    L.nmarks = 0;
    L.summary[0] = 0;
    L.m = m;
    pthread_mutex_unlock(&L.mu);
    atomic_store(&L.running, true);
    if (pthread_create(&L.th, NULL, worker, NULL) != 0) {
        atomic_store(&L.running, false);
        snprintf(L.rc_text, sizeof(L.rc_text), "Couldn't start it (no thread).");
        return;
    }
    L.started_once = true;
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
    if (!L.folder[0]) {
        /* Next to the captures diagnose.bat takes, if that folder is here. */
        struct stat st;
        if (!stat("../Logic Analyser", &st) && (st.st_mode & S_IFDIR))
            snprintf(L.folder, sizeof(L.folder), "../Logic Analyser/diagnosis-board");
        else snprintf(L.folder, sizeof(L.folder), "diagnosis-board");
    }
}
bool la_window_is_open(void) { return L.open; }

void la_window_open_test(const char *spec) {
    la_window_open();
    if (!strncmp(spec, "replay:", 7)) {
        L.mode = MODE_REPLAY;
        snprintf(L.folder, sizeof(L.folder), "%s", spec + 7);
    } else if (!strncmp(spec, "simulate:", 9)) {
        L.mode = MODE_SIMULATE;
        const char *c = strchr(spec + 9, '|');
        if (!c) return;
        snprintf(L.fault, sizeof(L.fault), "%.*s", (int)(c - spec - 9), spec + 9);
        snprintf(L.folder, sizeof(L.folder), "%s", c + 1);
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

    nk_layout_row_dynamic(ctx, 52, 1);
    char head[300];
    if (zx)
        snprintf(head, sizeof(head), "The %s on the bench: its boot captured stage by stage over the edge connector "
                 "(Jim's interface, two LogicAnalyzer boards) and compared with the simulated one.", machine->board_name);
    else snprintf(head, sizeof(head), "For a ZX Spectrum: put one on the bench (the analyser reads its edge connector).");
    nk_label_colored_wrap(ctx, head, zx ? COL_TEXT : COL_FAULT);

    /* What to diagnose. */
    nk_layout_row_dynamic(ctx, 22, 1);
    static const char *const modes[] = { "The real Spectrum (analyser)", "Again, from the folder's captures",
                                         "The simulated board, with a fault" };
    static const char *const tips[] = {
        "Capture from the two LogicAnalyzer boards over USB: the Spectrum left running first, then its boot "
        "(you hold its reset button when asked)",
        "Diagnose the captures a run took before (in the folder below) again, without the analyser",
        "The simulated board with the fault below as the one under test: to see what the diagnosis says of it" };
    for (int k = 0; k < 3; k++) {
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, tips[k]);
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_option_label(ctx, modes[k], L.mode == k) && !running) L.mode = k;
    }
    field(ctx, "Folder", L.folder, sizeof(L.folder), 70, cw,
          "Where the captures and diagnosis.txt go (or, again, where they are)");
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
            lp_port_t p[8];
            int n = lp_find(p, 8), m = 0;
            m += snprintf(L.boards, sizeof(L.boards), n ? "Found:" : "No LogicAnalyzer boards found (plugged in?)");
            for (int i = 0; i < n && m < (int)sizeof(L.boards); i++)
                m += snprintf(L.boards + m, sizeof(L.boards) - m, " %s (serial %s)", p[i].port, p[i].serial);
            if (n == 2 && !L.ports[0]) snprintf(L.ports, sizeof(L.ports), "%s,%s", p[0].port, p[1].port);
        }
        nk_layout_row_end(ctx);
        if (L.boards[0]) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label_colored(ctx, L.boards, NK_TEXT_LEFT, COL_DIMTEXT);
        }
    } else if (L.mode == MODE_SIMULATE)
        field(ctx, "Fault", L.fault, sizeof(L.fault), 70, cw,
              "As --fault: IC26:removed, IC2.26:gnd, IC12:data0=1... (empty: the working board)");
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_bool all = L.all;
    if (nk_widget_is_hovered(ctx))
        nk_tooltip(ctx, "After a stage that's wrong, capture the later ones too: more to go on (one that never comes "
                        "ends it)");
    if (nk_checkbox_label(ctx, "After a wrong stage, take the rest too", &all) && !running) L.all = all;

    nk_layout_row_dynamic(ctx, 28, 4);
    if (!running) {
        if (zx && L.folder[0] && nk_button_label(ctx, "Start")) start(machine);
    } else if (nk_button_label(ctx, "Stop")) {
        pthread_mutex_lock(&L.mu);
        L.stop = true;
        pthread_cond_broadcast(&L.cv);
        pthread_mutex_unlock(&L.mu);
    }
    if (nk_button_label(ctx, "Close")) L.open = false;
    if (!running && L.nmarks) {
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, "Close the window to see the board: the outlines stay (open it again from Diagnose)");
        if (nk_button_label(ctx, "Show the board")) L.open = false;
        if (nk_button_label(ctx, "Clear outlines")) L.nmarks = 0;
    }

    /* Its question, with the answers. */
    pthread_mutex_lock(&L.mu);
    bool asking = L.asking;
    char prompt[600];
    snprintf(prompt, sizeof(prompt), "%s", L.prompt);
    bool stopping = L.stop && running;
    pthread_mutex_unlock(&L.mu);
    if (asking) {
        nk_layout_row_dynamic(ctx, 40, 1);
        nk_label_colored_wrap(ctx, prompt, COL_HOVER);
        nk_layout_row_static(ctx, 30, 220, 2);
        if (nk_button_label(ctx, "Continue (Enter)")) answer(1);
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "As typing n and Enter: no");
        if (nk_button_label(ctx, "No")) answer(0);
        if (nk_input_is_key_pressed(&ctx->input, NK_KEY_ENTER) && !L.typing) answer(1);
    } else if (running) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label_colored(ctx, stopping ? "Stopping: at its next question, or when this capture or simulation ends..."
                                       : "Running...", NK_TEXT_LEFT, COL_PULSE);
    } else if (L.rc_text[0]) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label_colored(ctx, L.rc_text, NK_TEXT_LEFT, L.rc == 0 ? COL_TRACE : COL_FAULT);
        if (L.summary[0]) {
            char s[480];
            snprintf(s, sizeof(s), "Outlined on the board: %s", L.summary);
            nk_layout_row_dynamic(ctx, 40, 1);
            nk_label_colored_wrap(ctx, s, COL_FAULT);
        }
    }

    /* What it printed, the newest at the bottom. */
    struct nk_rect region = nk_window_get_content_region(ctx);
    float list_h = region.y + region.h - (ctx->current->layout->at_y + ctx->current->layout->row.height) -
                   ctx->style.window.spacing.y - 4;
    if (list_h < 120) list_h = 120;
    nk_layout_row_dynamic(ctx, list_h, 1);
    static nk_uint sx, sy;
    if (nk_group_scrolled_offset_begin(ctx, &sx, &sy, "la text", 0)) {
        pthread_mutex_lock(&L.mu);
        const char *t = L.text ? L.text : "";
        size_t tl = L.len;
        int lines = 0;
        float lh = 16;
        for (const char *line = t; line < t + tl; ) {
            const char *end = memchr(line, '\n', (size_t)(t + tl - line));
            size_t len = end ? (size_t)(end - line) : (size_t)(t + tl - line);
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
            else if (!strncmp(buf, "  OK", 4)) c = COL_TRACE;
            else if (buf[0] == ' ' && buf[1] == ' ' && isdigit((unsigned char)buf[2])) c = COL_PULSE;
            if (rows > 1) nk_label_colored_wrap(ctx, buf, ink(c));
            else nk_label_colored(ctx, buf, NK_TEXT_LEFT, ink(c));
            lines += rows;
            if (!end) break;
            line = end + 1;
        }
        bool more = tl != L.shown_len;
        L.shown_len = tl;
        pthread_mutex_unlock(&L.mu);
        if (more) sy = (nk_uint)(lines * (lh + ctx->style.window.spacing.y) + 100);   /* down to the newest */
        nk_group_end(ctx);
    }
    nk_end(ctx);
}
