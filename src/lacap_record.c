/* lacap_record.c - recording a simulated board as the analyser would, and the boot's timeline.
 *
 * AI-generated (Claude Code). */
#include "lacap_int.h"

void rec_step(board_t *b, void *ctx) {
    rec_t *r = ctx;
    if (r->done) return;
    sim_t *s = &b->sim;
    r->step++;
    uint64_t v = 0;
    for (int i = 0; i < LA_NSIG; i++)
        if (r->net[i] >= 0) v |= (uint64_t)(s->nread[s->nets[r->net[i]].root] & 1) << i;
    bool reset = v >> LA_RESET & 1;
    bool fire;
    if (r->trig_addr == -2) fire = r->after_reset && r->step - r->reset_step >= r->arm_step;
    else if (r->trig_addr == -3) fire = r->step >= r->arm_step;     /* from power on, /RESET or not */
    else if (r->trig_addr < 0) fire = reset && !r->reset_prev;
    else {
        /* As the analyser's pattern trigger: the address lines alone,
         * whatever the bus is doing, from the first match after reset. */
        unsigned a = (unsigned)(v >> LA_A0 & 0xFFFF);
        fire = r->after_reset && r->step >= r->arm_step && a == (unsigned)r->trig_addr && r->prev_addr != a;
        r->prev_addr = a;
        /* While /RESET is low the real Z80's address lines float: the first
         * address after it is a change, even if the simulation held it. */
        if (reset && !r->reset_prev) r->prev_addr = 0x10000;
    }
    if (reset && !r->reset_prev && !r->after_reset) { r->after_reset = true; r->reset_step = r->step; }
    if (!r->triggered && fire) {
        r->triggered = true;
        int start = r->rcount < r->pre ? 0 : r->rpos;
        for (int k = 0; k < r->rcount; k++) r->out[r->nout++] = r->ring[(start + k) % r->pre];
    }
    r->reset_prev = reset;
    if (r->triggered) {
        r->out[r->nout++] = v;
        if (r->nout >= r->rcount + r->post) r->done = true;
    } else if (r->pre > 0) {
        r->ring[r->rpos] = v;
        r->rpos = (r->rpos + 1) % r->pre;
        if (r->rcount < r->pre) r->rcount++;
    }
}

/* The edge connector's nets for the signals (-1: not there); the capture's
 * channels and signals from the map. */
void find_nets(board_t *b, int net[LA_NSIG], la_capture_t *c) {
    sim_t *s = &b->sim;
    for (int i = 0; i < LA_NSIG; i++) {
        if (c) c->chan[i] = -1;
        net[i] = -1;
        char names[64];
        snprintf(names, sizeof(names), "%s", sig_names[i]);
        for (size_t k = 0; k < sig_nets_n; k++)
            if (sig_nets[k].sig == i) snprintf(names, sizeof(names), "%s", sig_nets[k].nets);
        /* (Not strtok: boards are recorded on several threads at once.) */
        for (char *w = names; *w && net[i] < 0; ) {
            while (*w == ' ') w++;
            char *e = w + strcspn(w, " ");
            bool last = !*e;
            *e = 0;
            int n = *w ? sim_find_net(s, w) : -1;
            if (n >= 0 && board_net_simulated(b, n)) net[i] = n;
            if (last) break;
            w = e + 1;
        }
        if (net[i] < 0 || !c) continue;
        c->have |= 1ull << i;
        for (int k = 0; k < 48; k++) if (chan_map[k] == i) c->chan[i] = k;
    }
}

bool la_record(board_t *b, int pre, int post, la_capture_t *c, char *err, int errlen) {
    return la_record_at(b, pre, post, -1, 0, 1000, c, err, errlen);
}

bool la_record_at(board_t *b, int pre, int post, int trig_addr, int arm_ms, int max_ms, la_capture_t *c, char *err, int errlen) {
    memset(c, 0, sizeof(*c));
    if (!board_is_zx48(b)) { snprintf(err, errlen, "the analyser is made for the Spectrum's edge connector"); return false; }
    rec_t r = { .pre = pre, .post = post, .trig_addr = trig_addr,
                .arm_step = (long long)arm_ms * (ZX_FREQ * ZX_STEPS_PER_CYCLE / 1000) };
    find_nets(b, r.net, c);
    if (r.net[LA_RESET] < 0) { snprintf(err, errlen, "no /RESET net on the edge connector"); return false; }
    r.ring = calloc((size_t)(pre > 0 ? pre : 1), sizeof(uint64_t));
    r.out = calloc((size_t)pre + (size_t)post, sizeof(uint64_t));
    if (!board_step_hook(b, rec_step, &r)) {
        free(r.ring); free(r.out);
        snprintf(err, errlen, "the board is busy (edge counting)");
        return false;
    }
    /* Power on is 70 ms of reset (C27); then up to max_ms in all. */
    long long limit = (long long)ZX_FREQ * max_ms / 1000;
    for (long long t = 0; !r.done && t < limit; t += 1000) board_run(b, 1000);
    board_step_hook(b, NULL, NULL);
    free(r.ring);
    if (!r.triggered) {
        free(r.out);
        if (trig_addr == -2) snprintf(err, errlen, "it didn't run to %d ms after the reset", arm_ms);
        else if (trig_addr < 0) snprintf(err, errlen, "/RESET never went high in %d ms: the analyser would not trigger", max_ms);
        else snprintf(err, errlen, "the address bus never showed %04X from %d to %d ms after power on", trig_addr, arm_ms, max_ms);
        return false;
    }
    if (!r.done) {
        snprintf(err, errlen, "the simulation stopped %d ms after power on, before all the samples after the trigger", max_ms);
        free(r.out);
        return false;
    }
    c->s = r.out;
    c->n = r.nout;
    c->pre = r.rcount;
    c->trig_addr = trig_addr;
    c->freq = ZX_FREQ * ZX_STEPS_PER_CYCLE;
    return true;
}

/* How early each address line switches when the address changes, falling
 * [0] and rising [1]: of the samples caught in the middle of a change on a
 * real Issue 2 (2400 of them, 4 captures), the share where it had switched
 * already. 96% of line pairs kept to this order: A15/A14 falling and A11
 * rising first, A6/A0 rising and A15/A14 rising last. */
static const float line_early[16][2] = {
    { .203f, .037f }, { .343f, .380f }, { .612f, .632f }, { .452f, .455f },
    { .593f, .663f }, { .490f, .547f }, { .106f, .022f }, { .392f, .438f },
    { .630f, .439f }, { .770f, .245f }, { .974f, .794f }, { .871f, .996f },
    { .554f, .839f }, { .868f, .873f }, { .994f, .164f }, { 1.0f, .138f },
};
static uint64_t *pairs_seen;
static bool pair_new(unsigned a, unsigned b) {
    uint64_t key = ((uint64_t)a << 16 | b) + 1;
    size_t mask = ((size_t)1 << PAIRS_BITS) - 1, h = (size_t)((key * 0x9E3779B97F4A7C15ull) >> 20) & mask;
    for (;;) {
        if (!pairs_seen[h]) { pairs_seen[h] = key; return true; }
        if (pairs_seen[h] == key) return false;
        h = (h + 1) & mask;
    }
}

/* What the bus can show for an instant going from P to A: the lines that
 * change, switched in the order above. At each point in that order, the
 * lines that switch at about the same time (within 0.15) either way. */
static float glitch_band = 0.05f;
static void glitch_mark(float *glitch_first, unsigned p, unsigned a, float now) {
    int ln[16], m = 0;
    float e[16];
    for (int k = 0; k < 16; k++)
        if ((p ^ a) >> k & 1) { ln[m] = k; e[m] = line_early[k][a >> k & 1]; m++; }
    for (int i = 0; i < m; i++)         /* earliest first */
        for (int j = i + 1; j < m; j++)
            if (e[j] > e[i]) { float t = e[i]; e[i] = e[j]; e[j] = t; int u = ln[i]; ln[i] = ln[j]; ln[j] = u; }
    for (int cut = 1; cut < m; cut++) {
        float c = (e[cut - 1] + e[cut]) / 2;
        unsigned fixed = p;
        int un[16], nu = 0;
        for (int i = 0; i < m; i++) {
            if (fabsf(e[i] - c) < glitch_band) un[nu++] = ln[i];
            else if (e[i] > c) fixed ^= 1u << ln[i];
        }
        if (nu > 8) nu = 8;
        for (int sub = 0; sub < 1 << nu; sub++) {
            unsigned x = fixed;
            for (int u = 0; u < nu; u++) if (sub >> u & 1) x ^= 1u << un[u];
            if (x != p && x != a && glitch_first[x] < 0) glitch_first[x] = now;
        }
    }
}

static void tl_step(board_t *b, void *ctx) {
    tl_t *t = ctx;
    sim_t *s = &b->sim;
    t->step++;
#define TL(sig) (s->nread[s->nets[t->net[sig]].root] & 1)
    bool reset = TL(LA_RESET);
    if (reset && !t->reset_prev && t->reset_at < 0) t->reset_at = t->step;
    t->reset_prev = reset;
    if (t->reset_at < 0) return;
    bool mreq = !TL(LA_MREQ), m1 = !TL(LA_M1), rd = !TL(LA_RD);
    bool fetch = mreq && m1, read = mreq && rd && !m1 && TL(LA_RFSH);
    if ((fetch && !t->prev_fetch) || (read && !t->prev_read)) {
        unsigned a = 0;
        for (int i = 0; i < 16; i++) a |= (unsigned)TL(LA_A0 + i) << i;
        float ms = (float)((t->step - t->reset_at) / (ZX_FREQ * ZX_STEPS_PER_CYCLE / 1000.0));
        if (fetch) {
            if (t->first_fetch[a] < 0) t->first_fetch[a] = ms;
            t->fetches[a]++;
        } else if (t->first_read[a] < 0) t->first_read[a] = ms;
    }
    unsigned a = 0;
    for (int i = 0; i < 16; i++) a |= (unsigned)TL(LA_A0 + i) << i;
#undef TL
    float now = (float)((t->step - t->reset_at) / (ZX_FREQ * ZX_STEPS_PER_CYCLE / 1000.0));
    if (a != t->cur_addr) {
        if (pair_new(t->cur_addr, a)) glitch_mark(t->glitch_first, t->cur_addr, a, now);
        for (int w = 0; w < t->nw; w++)
            if (now >= t->w_from[w] && now <= t->w_to[w]) glitch_mark(t->w_glitch[w], t->cur_addr, a, now);
        t->cur_addr = a;
        t->cur_hold = 0;
    }
    if (++t->cur_hold == 8) {
        if (t->held_first[a] < 0) t->held_first[a] = now;
        for (int w = 0; w < t->nw; w++)
            if (now >= t->w_from[w] && now <= t->w_to[w] && t->w_held[w][a] < 0) t->w_held[w][a] = now;
    }
    t->prev_fetch = fetch;
    t->prev_read = read;
}

/* A trigger address for a stage at time T (ms) that the analyser can't
 * see before it: on the bus (held) at T or soon after, not held and not
 * possible as a glitch before that. A ROM address (below 4000, not a
 * refresh's 3Fxx) unless none; -1 if none in 0.2 ms. */
static int safe_trigger(const float *held, const float *glitch, float at) {
    int best = -1;
    float bt = 0;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (unsigned x = 0; x < 65536; x++) {
            float h = held[x], g = glitch[x];
            if (h < at - 0.01f || h > at + 0.2f) continue;
            if (g >= 0 && g <= h) continue;
            if (!pass && (x >= 0x4000 || (x & 0xFF00) == 0x3F00)) continue;
            if (best < 0 || h < bt) { best = (int)x; bt = h; }
        }
    return best;
}

static const struct { uint16_t addr; bool read; const char *what; } milestones[] = {
    { 0x0000, false, "START: the reset" },
    { 0x11CB, false, "START/NEW" },
    { 0x11DC, false, "RAM-FILL: the RAM test writes 02 from FFFF down" },
    { 0x11E2, false, "RAM-READ: it checks the RAM, from 4000 up" },
    { 0x11EF, false, "RAM-DONE: the RAM test is over" },
    { 0x1219, false, "RAM-SET: the system variables" },
    { 0x0038, false, "MASK-INT: the first frame interrupt" },
    { 0x1539, true,  "the copyright message is read" },
    { 0x12A9, false, "MAIN-1: the editor's main loop" },
    { 0x10A8, false, "KEY-INPUT" },
};

int timeline(const la_cli_t *o) {
    board_t *b = la_new_board(o->machine ? o->machine : &machine_zx48);
    for (int i = 0; i < o->nfaults; i++)
        if (!board_apply_fault_spec(b, o->faults[i])) { fprintf(stderr, "bad fault spec: %s\n", o->faults[i]); board_destroy(b); return 1; }
    if (!board_is_zx48(b)) { fprintf(stderr, "the timeline is for the Spectrum\n"); board_destroy(b); return 1; }
    tl_t t = { .reset_at = -1 };
    find_nets(b, t.net, NULL);
    const int need[] = { LA_RESET, LA_MREQ, LA_M1, LA_RD, LA_RFSH };
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++)
        if (t.net[need[i]] < 0) { fprintf(stderr, "no %s net\n", sig_names[need[i]]); board_destroy(b); return 1; }
    t.first_fetch = malloc(65536 * sizeof(float));
    t.first_read = malloc(65536 * sizeof(float));
    t.fetches = calloc(65536, sizeof(uint32_t));
    t.held_first = malloc(65536 * sizeof(float));
    pairs_seen = calloc((size_t)1 << PAIRS_BITS, sizeof(uint64_t));
    if (getenv("LA_GLITCH_BAND")) glitch_band = (float)atof(getenv("LA_GLITCH_BAND"));
    t.glitch_first = malloc(65536 * sizeof(float));
    for (int a = 0; a < 65536; a++) t.first_fetch[a] = t.first_read[a] = t.held_first[a] = t.glitch_first[a] = -1;
    for (const la_stage_t *st = la_stages; st->name && t.nw < 8; st++) {
        if (st->ms <= 1) continue;
        int w = t.nw++;
        t.w_from[w] = (float)st->ms - TL_ARMED;
        t.w_to[w] = (float)st->ms + 1;
        t.w_held[w] = malloc(65536 * sizeof(float));
        t.w_glitch[w] = malloc(65536 * sizeof(float));
        for (int a = 0; a < 65536; a++) t.w_held[w][a] = t.w_glitch[w][a] = -1;
    }
    int ms = o->ms > 0 ? o->ms : 3000;
    board_step_hook(b, tl_step, &t);
    long long limit = (long long)ZX_FREQ * ms / 1000;
    for (long long n = 0; n < limit; n += 1000) board_run(b, 1000);
    board_step_hook(b, NULL, NULL);
    char d[300];
    printf("%s, %d ms from power on; /RESET went high at %.1f ms\n", board_desc(o, d, sizeof(d)), ms,
           t.reset_at < 0 ? -1.0 : t.reset_at / (ZX_FREQ * ZX_STEPS_PER_CYCLE / 1000.0));
    printf("  %-9s %-6s %-10s %s\n", "ms", "addr", "fetches", "");
    for (size_t i = 0; i < sizeof(milestones) / sizeof(milestones[0]); i++) {
        unsigned a = milestones[i].addr;
        float f = milestones[i].read ? t.first_read[a] : t.first_fetch[a];
        if (f < 0) printf("  %-9s %04X   %-10s %s\n", "never", a, "", milestones[i].what);
        else printf("  %-9.3f %04X   %-10u %s\n", f, a, milestones[i].read ? 0u : t.fetches[a], milestones[i].what);
    }
    printf("Frame interrupts (0038 fetches): %u\n", t.fetches[0x38]);
    /* The stages' triggers: would the analyser fire before the stage? */
    printf("\nThe --la-trigger stages (an address can show for an instant when the bus changes):\n");
    int w = 0;
    for (const la_stage_t *st = la_stages; st->name; st++) {
        unsigned x = (unsigned)st->addr;
        if (x == 0x0038) continue;      /* any time: the first is all right */
        const float *held = t.held_first, *glitch = t.glitch_first;
        char armed[64] = "armed from the reset";
        if (st->ms > 1 && w < t.nw) {
            held = t.w_held[w]; glitch = t.w_glitch[w]; w++;
            snprintf(armed, sizeof(armed), "armed %.0f ms before", TL_ARMED);
        }
        float h = held[x], g = glitch[x];
        printf("  %-10s %04X  on the bus at %.3f ms (%s)", st->name, x, h, armed);
        if (h < 0) printf(": not in that time");
        else if (g >= 0 && g < h) {
            printf(", but possible as a glitch from %.3f ms", g);
            int sx = safe_trigger(held, glitch, h);
            if (sx >= 0) printf(": %04X is safe (on the bus at %.3f ms)", sx, held[sx]);
            else printf(": no safe address near it");
        } else printf(": safe");
        printf("\n");
    }
    for (int k = 0; k < t.nw; k++) { free(t.w_held[k]); free(t.w_glitch[k]); }
    /* LA_GLITCH_ADDRS=11E2,4000: when those are on the bus, and as a glitch */
    if (getenv("LA_GLITCH_ADDRS")) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", getenv("LA_GLITCH_ADDRS"));
        for (char *w = strtok(buf, ","); w; w = strtok(NULL, ",")) {
            unsigned x = (unsigned)strtoul(w, NULL, 16) & 0xFFFF;
            printf("  %04X on the bus at %.3f ms, a glitch from %.3f ms\n", x, t.held_first[x], t.glitch_first[x]);
        }
    }
    free(t.held_first); free(t.glitch_first);
    free(pairs_seen); pairs_seen = NULL;
    free(t.first_fetch); free(t.first_read); free(t.fetches);
    board_destroy(b);
    return 0;
}
