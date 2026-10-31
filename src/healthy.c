/* healthy.c - the reference sheet of a healthy board (see healthy.h). */
#include "healthy.h"
#include "atlas.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t measuring = PTHREAD_MUTEX_INITIALIZER;

static void options_of(board_t *b, int *options) {
    memset(options, 0, HEALTHY_MAX_OPTIONS * sizeof(int));
    for (int i = 0; i < b->m->noptions && i < HEALTHY_MAX_OPTIONS; i++) options[i] = b->m->get_option(b, i);
}

healthy_t *healthy_measure(const machine_t *m, const int *options) {
    pthread_mutex_lock(&measuring);         /* the edge counter is one board at a time */
    board_t *b = board_create(m);
    for (int i = 0; options && i < m->noptions && i < HEALTHY_MAX_OPTIONS; i++)
        if (m->get_option(b, i) != options[i]) m->set_option(b, i, options[i]);
    healthy_t *h = calloc(1, sizeof(*h));
    h->m = m;
    options_of(b, h->options);
    int n = h->nnets = b->sim.nnets;
    h->net = calloc((size_t)n + 1, sizeof(healthy_net_t));
    uint32_t *rises = calloc((size_t)n + 1, sizeof(uint32_t)), *highs = calloc((size_t)n + 1, sizeof(uint32_t));
    uint32_t steps = 0;
    for (int k = 0; k < ATLAS_PROBE_TO; k++) {
        if (k == ATLAS_PROBE_FROM) board_stats_reset(b);
        if (k == ATLAS_PROBE_TO - HEALTHY_EDGE_FRAMES) board_edges_start(b);
        board_run(b, m->cycles_per_frame);
        board_frame_update(b, 20000);
    }
    steps = board_edges_stop(b, rises, highs);
    double sample_hz = m->cpu_hz * m->steps_per_cycle;
    h->max_hz = sample_hz / 2;
    h->window = steps / sample_hz;
    for (int i = 0; i < n; i++) {
        h->net[i].state = (uint8_t)board_net_state(b, i);
        h->net[i].hz = steps ? (float)(rises[i] / h->window) : 0;
        h->net[i].high = steps ? (float)highs[i] / steps : 0;
    }
    free(rises);
    free(highs);
    board_destroy(b);
    pthread_mutex_unlock(&measuring);
    return h;
}

void healthy_free(healthy_t *h) {
    if (!h) return;
    free(h->net);
    free(h);
}

static void freq_text(double hz, char *out, int len) {
    if (hz >= 1e6) snprintf(out, (size_t)len, "%.3g MHz", hz / 1e6);
    else if (hz >= 1e3) snprintf(out, (size_t)len, "%.3g kHz", hz / 1e3);
    else snprintf(out, (size_t)len, "%.3g Hz", hz);
}

void healthy_text(const healthy_t *h, board_t *b, int net, char *out, int len) {
    if (net < 0 || net >= h->nnets) { snprintf(out, (size_t)len, "not connected"); return; }
    int rail = board_net_supply(b, net);
    if (rail == RAIL_GND) { snprintf(out, (size_t)len, "ground (0 V)"); return; }
    if (rail != RAIL_NONE) { snprintf(out, (size_t)len, "supply (%s)", b->sim.nets[net].name); return; }
    if (!board_net_simulated(b, net)) { snprintf(out, (size_t)len, "not simulated (analog)"); return; }
    const healthy_net_t *n = &h->net[net];
    switch (n->state) {
    case NET_HIGH: snprintf(out, (size_t)len, "HIGH"); return;
    case NET_LOW: snprintf(out, (size_t)len, "LOW"); return;
    case NET_FLOATING: snprintf(out, (size_t)len, "FLOATING (nothing drives it)"); return;
    case NET_CONTENTION: snprintf(out, (size_t)len, "two outputs fighting"); return;
    case NET_PULSING: break;
    default: snprintf(out, (size_t)len, "-"); return;
    }
    if (n->hz <= 0) {
        snprintf(out, (size_t)len, "PULSING, now and then (slower than %.0f Hz)", 1 / h->window);
        return;
    }
    char f[32];
    freq_text(n->hz, f, sizeof(f));
    snprintf(out, (size_t)len, "PULSING, %s%s, high %.0f%%", f,
             n->hz >= 0.9 * h->max_hz ? " (as fast as the simulator shows)" : "", 100 * n->high);
}

/* ---- The page ------------------------------------------------------------ */

static void html_text(FILE *f, const char *s) {
    for (; s && *s; s++) {
        if (*s == '&') fputs("&amp;", f);
        else if (*s == '<') fputs("&lt;", f);
        else if (*s == '>') fputs("&gt;", f);
        else if (*s == '"') fputs("&quot;", f);
        else if (*s == '\n') fputc(' ', f);
        else fputc(*s, f);
    }
}

static const char *state_class(const healthy_t *h, board_t *b, int net) {
    if (net < 0 || board_net_supply(b, net) != RAIL_NONE || !board_net_simulated(b, net)) return "pw";
    switch (h->net[net].state) {
    case NET_HIGH: return "hi";
    case NET_LOW: return "lo";
    case NET_PULSING: return "pu";
    case NET_FLOATING: return "fl";
    default: return "fi";
    }
}

bool healthy_write_html(board_t *b, const healthy_t *h, const char *path, char *err, int errlen) {
    FILE *f = fopen(path, "w");
    if (!f) { snprintf(err, (size_t)errlen, "can't write %s", path); return false; }
    const sim_t *s = &b->sim;
    const machine_t *m = b->m;
    char when[32] = "";
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) strftime(when, sizeof(when), "%Y-%m-%d", tm);
    fputs("<!doctype html>\n<html><head><meta charset=\"utf-8\">\n<title>", f);
    html_text(f, m->board_name);
    fputs(" healthy board sheet</title>\n<style>\n"
          ":root{--ink:#1c1c1c;--dim:#666;--line:#ccc;--bg:#fff;--hi:#b3261e;--lo:#1a56c4;--pu:#9a6a00;--fl:#777}\n"
          "@media (prefers-color-scheme:dark){:root{--ink:#e8e8e2;--dim:#a0a8a0;--line:#444;--bg:#1b1d1b;"
          "--hi:#ff7a66;--lo:#7aa8ff;--pu:#f0c040;--fl:#aaa}}\n"
          "body{font:13px/1.35 system-ui,sans-serif;color:var(--ink);background:var(--bg);margin:16px}\n"
          "h1{font-size:20px;margin:0 0 4px}p{margin:4px 0;color:var(--dim);max-width:60em}\n"
          ".chips{columns:2 30em;column-gap:24px;margin-top:12px}\n"
          ".chip{break-inside:avoid;margin:0 0 14px}\n"
          "h2{font-size:14px;margin:0 0 2px}h2 span{font-weight:normal;color:var(--dim)}\n"
          "table{border-collapse:collapse;width:100%}td{border-top:1px solid var(--line);padding:1px 6px 1px 0;"
          "vertical-align:top}\n"
          "td.n{width:2.5em;text-align:right;color:var(--dim)}td.net{color:var(--dim)}\n"
          ".hi{color:var(--hi)}.lo{color:var(--lo)}.pu{color:var(--pu)}.fl{color:var(--fl)}.pw{color:var(--dim)}\n"
          "@media print{body{margin:0;font-size:10px}:root{--ink:#000;--bg:#fff}}\n"
          "</style></head><body>\n<h1>", f);
    html_text(f, m->board_name);
    fputs(": the healthy board</h1>\n<p>", f);
    fputs("What a logic probe shows on each pin of a working board, ", f);
    fprintf(f, "%.0f to %.0f seconds after power on (once it has settled, the time a probe reading is taken on "
            "the bench), as the simulator measured it", ATLAS_PROBE_FROM * 0.02, ATLAS_PROBE_TO * 0.02);
    for (int i = 0; i < m->noptions && i < HEALTHY_MAX_OPTIONS; i++) {
        fputs(i ? ", " : " (", f);
        html_text(f, m->options[i].name);
        fputs(": ", f);
        html_text(f, m->options[i].values[h->options[i]]);
        if (i == m->noptions - 1) fputs(")", f);
    }
    fprintf(f, ". How fast a pulsing pin is: its rising edges a second over the last %.2f s. ", h->window);
    char fmax[32];
    freq_text(h->max_hz, fmax, sizeof(fmax));
    fprintf(f, "The simulator looks at every pin once per bus step, so it shows nothing faster than %s; a "
            "signal that fast on the real board can read slower here. ", fmax);
    fputs("A real probe or scope reading differs a little (timing, levels); what counts is the kind of "
          "reading. Pins that are analog on the real board (video outputs, an oscillator a chip runs "
          "inside it, sound) show what the simulator makes of them, not what a probe sees there. Made ", f);
    fprintf(f, "%s by c64-hw-sim (AI-generated software, written with Claude).</p>\n<div class=\"chips\">\n", when);
    for (int p = 0; p < s->nparts; p++) {
        const part_t *part = &s->parts[p];
        if (part->kind != PART_IC && part->kind != PART_CONNECTOR) continue;
        fputs("<div class=\"chip\"><h2>", f);
        html_text(f, part->ref);
        fputs(" <span>", f);
        const char *label = part->kind == PART_IC ? board_part_label(b, p) : board_connector_name(b, p);
        html_text(f, label ? label : "");
        const char *det = part->kind == PART_IC ? board_part_detail(b, p) : NULL;
        if (det && *det) {
            fputs(": ", f);
            char one[160];
            snprintf(one, sizeof(one), "%.*s", (int)strcspn(det, "\n"), det);
            html_text(f, one);
        }
        fputs("</span></h2>\n<table>\n", f);
        for (int k = 0; k < part->npins; k++) {
            int pin = part->first_pin + k, net = s->pins[pin].net;
            const char *name = part->kind == PART_IC ? sim_pin_name(s, pin) : board_pin_func(b, pin);
            char what[200];
            healthy_text(h, b, net, what, sizeof(what));
            fputs("<tr><td class=\"n\">", f);
            html_text(f, s->bd->pads[pin].name);
            fputs("</td><td>", f);
            html_text(f, name ? name : "");
            fputs("</td><td class=\"net\">", f);
            html_text(f, net >= 0 ? s->nets[net].name : "");
            fprintf(f, "</td><td class=\"%s\">", state_class(h, b, net));
            html_text(f, what);
            fputs("</td></tr>\n", f);
        }
        fputs("</table></div>\n", f);
    }
    fputs("</div>\n</body></html>\n", f);
    if (fclose(f) != 0) { snprintf(err, (size_t)errlen, "can't write %s", path); return false; }
    return true;
}

/* ---- In the program ------------------------------------------------------- */

static struct {
    pthread_mutex_t lock;
    healthy_t **done; int ndone;
    bool busy;                          /* a measurement running */
    const machine_t *m;                 /* ... of this */
    int options[HEALTHY_MAX_OPTIONS];
} H = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void *worker(void *arg) {
    (void)arg;
    healthy_t *h = healthy_measure(H.m, H.options);
    pthread_mutex_lock(&H.lock);
    H.done = realloc(H.done, (size_t)(H.ndone + 1) * sizeof(*H.done));
    H.done[H.ndone++] = h;
    H.busy = false;
    pthread_mutex_unlock(&H.lock);
    return NULL;
}

const healthy_t *healthy_get(board_t *b) {
    int options[HEALTHY_MAX_OPTIONS];
    options_of(b, options);
    const healthy_t *found = NULL;
    pthread_mutex_lock(&H.lock);
    for (int i = 0; i < H.ndone && !found; i++)
        if (H.done[i]->m == b->m && !memcmp(H.done[i]->options, options, sizeof(options))) found = H.done[i];
    if (!found && !H.busy) {
        H.busy = true;
        H.m = b->m;
        memcpy(H.options, options, sizeof(options));
        pthread_t tid;
        if (pthread_create(&tid, NULL, worker, NULL) == 0) pthread_detach(tid);
        else H.busy = false;
    }
    pthread_mutex_unlock(&H.lock);
    return found;
}

int healthy_cli(const char *dir) {
    static const machine_t *const boards[] = { &machine_c64, &machine_c64c, &machine_zx48i1, &machine_zx48i2,
                                               &machine_zx48, &machine_zx48i4b, &machine_zx48i6a };
    int bad = 0;
    for (size_t i = 0; i < sizeof(boards) / sizeof(boards[0]); i++) {
        clock_t t0 = clock();
        healthy_t *h = healthy_measure(boards[i], NULL);
        board_t *b = board_create(boards[i]);
        char path[1100], err[1200];
        snprintf(path, sizeof(path), "%s/%s-healthy.html", dir, boards[i]->id);
        if (healthy_write_html(b, h, path, err, sizeof(err)))
            printf("%s: %s (%.1f s)\n", boards[i]->board_name, path, (double)(clock() - t0) / CLOCKS_PER_SEC);
        else {
            fprintf(stderr, "%s\n", err);
            bad = 1;
        }
        board_destroy(b);
        healthy_free(h);
    }
    return bad;
}
