/* lacap_rules.c - the diagnosis: what each capture shows (the evidence).
 *
 * AI-generated (Claude Code). */
#include "lacap_int.h"

void ev_init(ev_t *e) {
    memset(e, 0, sizeof(*e));
    e->pramt = e->pramt_ref = e->test_addr = -1;
    e->clk_pct = -1;
    e->rt_addr = -1;
}

/* Another stage's evidence added to E (--la-all). */
void ev_merge(ev_t *e, const ev_t *f) {
    for (int ar = 0; ar < AR_N; ar++) {
        if (!e->nbad[ar] && f->nbad[ar]) e->first[ar] = f->first[ar];
        e->nbad[ar] += f->nbad[ar];
        for (int i = 0; i < 8; i++) { e->bad[ar][i] += f->bad[ar][i]; e->low[ar][i] += f->low[ar][i]; e->high[ar][i] += f->high[ar][i]; }
    }
    /* Not the stuck lines: after the CPU went astray a line can sit still
     * because of where it went, not a fault. */
    e->kb_n += f->kb_n;
    e->kb_all += f->kb_all;
    e->kb_rows |= f->kb_rows;
    for (int k = 0; k < 5; k++) { e->kb_low[k] += f->kb_low[k]; e->kb_rows_low[k] |= f->kb_rows_low[k]; }
    if (e->pramt < 0) { e->pramt = f->pramt; e->pramt_ref = f->pramt_ref; }
    if (e->test_addr < 0 && f->test_addr >= 0) { e->test_addr = f->test_addr; e->test_got = f->test_got; memcpy(e->test_seq, f->test_seq, sizeof(e->test_seq)); }
    e->c_have += f->c_have; e->c_bad += f->c_bad; e->c_bad_win += f->c_bad_win; e->c_good += f->c_good; e->c_good_win += f->c_good_win;
    e->in_reset |= f->in_reset;
    for (int k = 0; k < 14; k++) e->rom_abit[k] += f->rom_abit[k];
    if (f->idle) {
        e->idle = 1;
        e->slow_ms = f->slow_ms; e->rst_pulses = f->rst_pulses; e->int_n = f->int_n; e->int_ref = f->int_ref;
        memcpy(e->held, f->held, sizeof(e->held));
        memcpy(e->lvl, f->lvl, sizeof(e->lvl));
        memcpy(e->idle_where, f->idle_where, sizeof(e->idle_where));
    }
    for (int ar = 0; ar < AR_N; ar++) {
        if (!e->late_n[ar] && f->late_n[ar]) { e->late_addr[ar] = f->late_addr[ar]; e->late_seen[ar] = f->late_seen[ar]; e->late_wrote[ar] = f->late_wrote[ar]; }
        e->late_n[ar] += f->late_n[ar];
        e->late_bits[ar] |= f->late_bits[ar];
    }
    if (e->rt_addr < 0) { e->rt_addr = f->rt_addr; e->rt_got = f->rt_got; }
    if (f->clk_pct >= 0 && (e->clk_pct < 0 || f->clk_pct < e->clk_pct)) e->clk_pct = f->clk_pct;
    if (f->clk_edge) e->clk_edge = f->clk_edge;
    if (!e->clk_stopped) e->clk_stopped = f->clk_stopped;
}

/* Where in the ULA's frame a sample is: in its screen fetch (the first 128
 * T of each of the 192 lines of the screen, from 14335 T after /INT
 * falls), from T_INT, the T of /INT's fall, in real time. */
static bool in_fetch(double t, double t_int) {
    double f = fmod(t - t_int, 69888.0);
    if (f < 0) f += 69888.0;
    f -= 14335.0;
    if (f < 0) return false;
    int line = (int)(f / 224.0);
    return line < 192 && f - line * 224.0 < 128.0;
}

/* The lower RAM's read-backs against the ULA's fetches: a ULA that doesn't
 * hold the CPU off while it fetches (contention) lets their accesses clash,
 * and the bytes written or read then come out wrong. */
static void contention_ev(ev_t *e, const la_capture_t *d, const la_cycle_t *cy, int n) {
    if (!(d->have >> LA_INT & 1) || d->freq <= 0) return;
    int fall = -1;
    for (int i = 1; i < d->n && fall < 0; i++) if (lo(d->s[i], LA_INT) && !lo(d->s[i - 1], LA_INT)) fall = i;
    if (fall < 0) return;
    double per = 3500000.0 / d->freq, t_int = fall * per;
    static short wv[0x4000];
    static unsigned char wwin[0x4000];
    unsigned badbits = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < 0x4000; k++) wv[k] = -1;
        for (int k = 0; k < n; k++) {
            const la_cycle_t *x = &cy[k];
            if (x->addr < 0x4000 || x->addr >= 0x8000) continue;
            unsigned a = x->addr - 0x4000;
            bool win = in_fetch(x->sample * per, t_int);
            if (x->kind == LA_WRITE) { wv[a] = x->data; wwin[a] = win; continue; }
            if ((x->kind != LA_READ && x->kind != LA_FETCH) || wv[a] < 0) continue;
            bool w = win || wwin[a];
            if (x->data != wv[a]) {
                if (pass == 0) { badbits |= (unsigned)(x->data ^ wv[a]); e->c_bad++; e->c_bad_win += w; }
                wv[a] = x->data;
            } else if (pass == 1 && (wv[a] & badbits)) { e->c_good++; e->c_good_win += w; }
        }
    }
    e->c_have = 1;
    printf("Contention check: /INT falls at sample %d; %d wrong read-backs of the lower RAM (%d written or read in a\n"
           "screen fetch), %d right ones with those bits (%d in a fetch)\n", fall, e->c_bad, e->c_bad_win, e->c_good, e->c_good_win);
}

static int area_of(unsigned a) { return a < 0x4000 ? AR_ROM : a < 0x8000 ? AR_LRAM : AR_URAM; }

static void ev_bits(ev_t *e, unsigned addr, unsigned got, unsigned want) {
    int ar = area_of(addr);
    unsigned x = (got ^ want) & 0xFF;
    if (!x) return;
    if (!e->nbad[ar]++) e->first[ar] = (int)addr;
    for (int i = 0; i < 8; i++)
        if (x >> i & 1) {
            e->bad[ar][i]++;
            if (want >> i & 1) e->low[ar][i]++;
            else e->high[ar][i]++;
        }
}

unsigned ev_mask(const ev_t *e, int ar) {
    unsigned m = 0;
    for (int i = 0; i < 8; i++) if (e->bad[ar][i]) m |= 1u << i;
    return m;
}

bool ev_pramt_bad(const ev_t *e) { return e->pramt >= 0 && e->pramt_ref >= 0 && e->pramt != e->pramt_ref; }

/* The frame interrupt, left running: not as often as a working one's. */
bool int_wrong(const ev_t *e) {
    return e->slow_ms && e->int_ref >= 2 && !e->held[LA_INT] && abs(e->int_n - e->int_ref) > 1;
}

bool ev_any(const ev_t *e) {
    for (int s = 0; s < LA_NSIG; s++)                   /* /M1, /RFSH: nothing on a 48K uses them; it runs without */
        if (e->stuck[s] && s != LA_M1 && s != LA_RFSH) return true;
    for (int k = 0; k < 5; k++) if (e->kb_low[k]) return true;
    if (e->kb_all) return true;
    for (int ar = 0; ar < AR_N; ar++) if (e->late_n[ar]) return true;
    if (e->rst_pulses || int_wrong(e)) return true;
    for (int k = 0; k < LA_NSIG; k++) if (e->held[k]) return true;
    return e->nbad[0] || e->nbad[1] || e->nbad[2] || ev_pramt_bad(e);
}

/* The RAM test's result: the last write of P-RAMT (5CB4) in the capture. */
static int pramt_of(const la_cycle_t *cy, int n) {
    int lo_b = -1, hi_b = -1;
    for (int k = 0; k < n; k++)
        if (cy[k].kind == LA_WRITE) {
            if (cy[k].addr == 0x5CB4) lo_b = cy[k].data;
            if (cy[k].addr == 0x5CB5) hi_b = cy[k].data;
        }
    return lo_b >= 0 && hi_b >= 0 ? hi_b << 8 | lo_b : -1;
}

/* A line that never changes in D from the reset (or the start) on. */
/* The CPU is doing something in C (its /MREQ keeps going low). /RESET
 * rises slowly (a capacitor): the Z80 runs well before the analyser sees
 * the line high, so a /RESET that reads low all through, with the CPU
 * busy, wasn't held. */
bool cpu_busy(const la_capture_t *c) {
    if (!(c->have >> LA_MREQ & 1)) return false;
    int k = 0;
    for (int i = 1; i < c->n && k < 50; i++) k += lo(c->s[i], LA_MREQ) && !lo(c->s[i - 1], LA_MREQ);
    return k >= 50;
}

int line_state(const la_capture_t *c, int s) {
    int from = la_start(c);
    if (from >= c->n) return -1;
    uint64_t v0 = c->s[from] >> s & 1;
    for (int i = from + 1; i < c->n; i++) if ((c->s[i] >> s & 1) != v0) return -1;
    return v0 ? 2 : 1;
}

/* How often line SG changes, from the start of the bus; *mostly_high: it's
 * high more than half the time. */
static void line_changes(const la_capture_t *c, int sg, int *changes, int *mostly_high) {
    int from = la_start(c), ch = 0, hi = 0;
    for (int i = from; i < c->n; i++) {
        hi += !lo(c->s[i], sg);
        if (i > from && (c->s[i] >> sg & 1) != (c->s[i - 1] >> sg & 1)) ch++;
    }
    *changes = ch;
    *mostly_high = 2 * hi > c->n - from;
}
void gather(ev_t *e, const la_capture_t *d, const la_capture_t *r) {
    la_cycle_t *cy;
    int n = la_cycles(d, &cy);
    /* The reset's capture: not the cycles the button's bounce cut short
     * (before the last restart). */
    if (n > 0 && (d->trig_addr < 0 || d->trig_addr == 0xFFFF)) {
        int k0 = first_fetch0_n(cy, n, NULL);
        if (k0 > 0) { memmove(cy, cy + k0, (size_t)(n - k0) * sizeof(*cy)); n -= k0; }
    }
    static short last[65536];
    static unsigned char no_ram[65536];
    memset(no_ram, 0, sizeof(no_ram));
    if (r) {
        la_cycle_t *cr;
        int nr = la_cycles(r, &cr);
        no_ram_cells(cr, nr, no_ram);
        free(cr);
    }
    unsigned last_fetch = 0;
    int pend_a = -1, pend_v = 0;
    for (int k = 0; k < 65536; k++) last[k] = -1;
    for (int k = 0; k < n; k++) {
        const la_cycle_t *x = &cy[k];
        if (x->kind == LA_IN && !(x->addr & 1)) {      /* port FE: the keyboard, a half row a bit of A8-A15 low */
            unsigned rows = ~(unsigned)(x->addr >> 8) & 0xFF;
            e->kb_n++;
            e->kb_rows |= rows;
            if (!(x->data & 0x1F)) e->kb_all++;        /* not keys: the read itself */
            else for (int b = 0; b < 5; b++) if (!(x->data >> b & 1)) { e->kb_low[b]++; e->kb_rows_low[b] |= rows; }
        }
        if (rom_wrong(d, x)) {
            ev_bits(e, x->addr, x->data, zx48_rom_image()[x->addr]);
            for (int b = 0; b < 14; b++) e->rom_abit[b] += zx48_rom_image()[x->addr ^ (1u << b)] == x->data;
        }
        if (x->kind == LA_FETCH) last_fetch = x->addr;
        if (x->kind == LA_READ && (last_fetch == 0x11E9 || last_fetch == 0x11EC) && x->addr >= 0x4000) { pend_a = x->addr; pend_v = x->data; }
        else if (x->kind == LA_WRITE && (int)x->addr == pend_a) {
            unsigned cpu_got = (x->data + 1u) & 0xFF, diff = cpu_got ^ (unsigned)pend_v;
            if (diff) {
                int ar = area_of(x->addr);
                if (!e->late_n[ar]++) { e->late_addr[ar] = pend_a; e->late_seen[ar] = pend_v; e->late_wrote[ar] = x->data; }
                e->late_bits[ar] |= diff;
            }
            pend_a = -1;
        }
        /* The RAM test's read-back (11E2-11EE): a byte's first read, not 02. */
        if (x->kind == LA_READ && x->addr >= 0x4000 && last_fetch >= 0x11E2 && last_fetch <= 0x11EE && last[x->addr] < 0 &&
            x->data != 0x02 && e->rt_addr < 0) { e->rt_addr = x->addr; e->rt_got = x->data; }
        if (x->addr < 0x4000 || no_ram[x->addr]) continue;
        if (x->kind == LA_WRITE) last[x->addr] = (short)x->data;
        else if ((x->kind == LA_READ || x->kind == LA_FETCH) && last[x->addr] >= 0 && last[x->addr] != x->data) {
            ev_bits(e, x->addr, x->data, (unsigned)last[x->addr]);
            last[x->addr] = (short)x->data;
        }
    }
    int p = pramt_of(cy, n);
    if (p >= 0) e->pramt = p;
    if (r) {
        la_cycle_t *cr;
        int nr = la_cycles(r, &cr);
        int pr = pramt_of(cr, nr);
        if (pr >= 0) e->pramt_ref = pr;
        free(cr);
    }
    /* The RAM test stopped short: the byte after P-RAMT. It wrote 02 there
     * (the fill), and reads it back first. */
    if (ev_pramt_bad(e) && e->pramt < 0xFFFF && e->pramt >= 0x3FFF) {
        unsigned x = (unsigned)e->pramt + 1;
        for (int k = 0; k < n; k++)
            if (cy[k].kind == LA_READ && cy[k].addr == x) {
                e->test_addr = (int)x;
                e->test_got = cy[k].data;
                ev_bits(e, x, cy[k].data, 0x02);
                /* The test's reads and writes there: 02 read, 01 written, 01 read, 00 written. */
                int len = 0, m = 0;
                for (int j = k; j < n && m < 4 && len < (int)sizeof(e->test_seq) - 16; j++)
                    if (cy[j].addr == x && (cy[j].kind == LA_READ || cy[j].kind == LA_WRITE)) {
                        len += snprintf(e->test_seq + len, sizeof(e->test_seq) - len, "%s%s %02X", m ? ", " : "",
                                        cy[j].kind == LA_READ ? "read" : "wrote", cy[j].data);
                        m++;
                    }
                break;
            }
    }
    contention_ev(e, d, cy, n);
    int rom_ok = 0;                     /* fetches from the ROM that are its bytes: the CPU really runs */
    {
        const uint8_t *rom = zx48_rom_image();
        for (int k = 0; k < n; k++) {
            rom_ok += cy[k].kind == LA_FETCH && cy[k].addr < 0x4000 && rom[cy[k].addr] == cy[k].data;
            e->rom_writes += cy[k].kind == LA_WRITE && cy[k].addr < 0x4000;
        }
    }
    free(cy);
    e->in_reset = (d->have >> LA_RESET & 1) && line_state(d, LA_RESET) == 1 && !cpu_busy(d);
    /* The CPU runs the ROM's code, so it has a clock, whatever the edge
     * connector's CLK pin reads (B8 is the ULA's side of the clock, not
     * the CPU's pin): a note, not a stopped clock. (The analyser's boards
     * swapped read CLK stuck too, but then nothing fetches the ROM right.) */
    bool runs_rom_ok = rom_ok >= 100;
    e->rom_runs = runs_rom_ok;
    if ((d->have >> LA_CLK & 1) && d->freq > 0 && line_state(d, LA_CLK) < 0) {
        int from = la_start(d), rises = 0;
        for (int i = from + 1; i < d->n; i++) rises += !lo(d->s[i], LA_CLK) && lo(d->s[i - 1], LA_CLK);
        double want = (double)(d->n - from) * ZX_FREQ / d->freq;
        if (want > 1000) e->clk_pct = (int)(100.0 * rises / want + 0.5);
    }
    for (int s2 = 0; s2 < LA_NSIG; s2++)
        if (d->have >> s2 & 1) { int st = line_state(d, s2); e->lvl[s2] = st > 0 ? st : 0; }
    /* Lines any running Z80 changes all the time. */
    static const int busy[] = { LA_MREQ, LA_RD, LA_WR, LA_M1, LA_RFSH, LA_CLK };
    for (int s = 0; r && s < LA_NSIG; s++) {
        bool look = (s >= LA_A0 && s <= LA_A15) || (s >= LA_D0 && s <= LA_D7);
        for (int k = 0; k < 6; k++) look |= s == busy[k];
        if (!look || !(d->have >> s & 1) || !(r->have >> s & 1)) continue;
        int st = line_state(d, s);
        if (st > 0 && line_state(r, s) < 0) e->stuck[s] = st;
        else if (st < 0) {
            int cd, hd, cr, hr;
            line_changes(d, s, &cd, &hd);
            line_changes(r, s, &cr, &hr);
            if (cd <= 20 && cr >= 1000) { e->stuck[s] = hd ? 2 : 1; e->nearly[s] = cd; }
        }
    }
    if (runs_rom_ok && e->stuck[LA_CLK]) {
        e->clk_edge = e->stuck[LA_CLK];
        e->stuck[LA_CLK] = 0;
        e->nearly[LA_CLK] = 0;
    }
}

/* Where the CPU was in a capture (taken at a time, not at a stage). */
void where_text(const la_capture_t *c, char *out, int len) {
    la_cycle_t *cy;
    int n = la_cycles(c, &cy), nf = 0, ram = 0, lo_a = 0xFFFF, hi_a = 0, fill = 0, rb = 0, irq = 0;
    for (int k = 0; k < n; k++) {
        if (cy[k].kind != LA_FETCH) continue;
        unsigned a = cy[k].addr;
        nf++;
        if (a >= 0x4000) ram++;
        if ((int)a < lo_a) lo_a = (int)a;
        if ((int)a > hi_a) hi_a = (int)a;
        fill += a >= 0x11DC && a <= 0x11E1;
        rb += a >= 0x11E2 && a <= 0x11EE;
        irq += a >= 0x0038 && a <= 0x0052;
    }
    free(cy);
    int halt = 0, from = la_start(c);
    if (c->have >> LA_HALT & 1)
        for (int i = from; i < c->n; i++) halt += lo(c->s[i], LA_HALT);
    if (!nf && n > 100)
        snprintf(out, len, "making bus cycles (%d) but not one instruction fetch: /M1 doesn't go low with them", n);
    else if (!nf) snprintf(out, len, "no instruction fetches at all: the CPU isn't running (its clock, /RESET, /WAIT or /BUSRQ, its power)");
    else if (halt > (c->n - from) / 2) snprintf(out, len, "halted (HALT low): it ran into a HALT, so it went astray before");
    else if (2 * fill > nf) snprintf(out, len, "still filling the RAM for the RAM test (11DC-11E1)");
    else if (2 * rb > nf) snprintf(out, len, "still reading the RAM back for the RAM test (11E2-11EE)");
    else if (2 * ram > nf) snprintf(out, len, "running code from RAM (%04X-%04X): it went astray", lo_a, hi_a);
    else snprintf(out, len, "fetching from %04X-%04X%s", lo_a, hi_a, irq ? " (and the frame interrupt)" : "");
}

/* A simulated board's capture: with FAULTS, triggered at TRIG (-1 the
 * reset, -2 ARM_MS after the reset, else the address from ARM_MS after
 * power on). */
bool record_sim(const machine_t *m, const char *const *faults, int nfaults, int pre, int post, int trig, int arm_ms,
                       la_capture_t *c, char *err, int errlen) {
    board_t *b = la_new_board(m);
    for (int i = 0; i < nfaults; i++)
        if (!board_apply_fault_spec(b, faults[i])) {
            snprintf(err, errlen, "bad fault spec: %s", faults[i]);
            board_destroy(b);
            return false;
        }
    bool ok = trig == -1 ? la_record(b, pre, post, c, err, errlen)
                         : la_record_at(b, pre, post, trig, arm_ms, trig == -2 ? arm_ms + 1000 : arm_ms + 4000, c, err, errlen);
    board_destroy(b);
    return ok;
}

/* A capture at a time: armed on the reset, then again AT_MS after it (0:
 * at once, no reset) on A0 being high for 60 ns, which a running Z80 shows
 * all the time (or low, if A0 is stuck high). */
bool capture_now(const la_cli_t *o, double at_ms, const char *save, la_capture_t *c, char *err, int errlen) {
    la_cli_t q = *o;
    q.at = at_ms;
    q.capture = save;
    q.tries = 3;
    q.timeout = 5;
    q.pre = 1000;
    q.post = 97000;
    q.trigger = "raw:1,15,1,1";
    if (capture_real(&q, c, err, errlen)) return true;
    if (!strstr(err, "no trigger")) return false;
    printf("Again, for A0 low this time (it was never high)%s\n",
           at_ms > 0 ? ": hold the reset button down, press Enter, and let go once it says Armed" : "");
    if (at_ms > 0 && !la_wait("Hold the reset button down, continue, and let go once it says Armed")) return false;
    q.trigger = "raw:1,15,1,0";
    return capture_real(&q, c, err, errlen);
}

bool want_enter(const char *msg) {
    printf("%s", msg);
    return la_wait(msg);
}

board_t *vboard;     /* the board's parts, for their references */
bool record_idle(const machine_t *m, const char *const *faults, int nfaults, la_capture_t *fast, la_capture_t *slow,
                        char *err, int errlen) {
    la_capture_t c;
    int k = ZX_FREQ * ZX_STEPS_PER_CYCLE / IDLE_SLOW;      /* 14 bus steps a sample */
    if (!record_sim(m, faults, nfaults, 1000 * k, 97000 * k, -3, IDLE_MS, &c, err, errlen)) return false;
    *fast = c;                  /* as the analyser's 1000 + 97000 at 14 MHz */
    fast->pre = c.pre < 2000 ? c.pre : 2000;
    fast->n = fast->pre + 194000 < c.n - (c.pre - fast->pre) ? fast->pre + 194000 : c.n - (c.pre - fast->pre);
    fast->trig_addr = -1;
    fast->s = malloc((size_t)fast->n * sizeof(uint64_t));
    memcpy(fast->s, c.s + (c.pre - fast->pre), (size_t)fast->n * sizeof(uint64_t));
    *slow = c;
    slow->n = c.n / k;
    slow->pre = c.pre / k;
    slow->freq = c.freq / k;
    slow->trig_addr = -1;
    slow->s = malloc((size_t)slow->n * sizeof(uint64_t));
    for (int i = 0; i < slow->n; i++) slow->s[i] = c.s[(size_t)i * k];
    la_free(&c);
    return true;
}

bool low_all(const la_capture_t *c, int sg) {
    if (!(c->have >> sg & 1) || c->n < 2) return false;
    for (int i = 0; i < c->n; i++) if (!lo(c->s[i], sg)) return false;
    return true;
}

static int falls(const la_capture_t *c, int sg) {
    int k = 0;
    if (!(c->have >> sg & 1)) return 0;
    for (int i = 1; i < c->n; i++) k += lo(c->s[i], sg) && !lo(c->s[i - 1], sg);
    return k;
}

/* The slow capture D against a working one's R. */
void slow_ev(ev_t *e, const la_capture_t *d, const la_capture_t *r) {
    e->slow_ms = d->freq > 0 ? (int)(1000.0 * d->n / d->freq + 0.5) : 0;
    static const int ctl[] = { LA_RESET, LA_WAIT, LA_BUSRQ, LA_INT, LA_HALT };
    for (int k = 0; k < 5; k++)
        if ((r->have >> ctl[k] & 1) && low_all(d, ctl[k]) && !low_all(r, ctl[k])) e->held[ctl[k]] = 1;
    if (e->held[LA_RESET] && cpu_busy(d)) e->held[LA_RESET] = 0;     /* the slow rise, not held */
    if (e->held[LA_RESET]) e->in_reset = true;
    else e->rst_pulses = falls(d, LA_RESET);
    if ((d->have >> LA_INT & 1) && (r->have >> LA_INT & 1)) { e->int_n = falls(d, LA_INT); e->int_ref = falls(r, LA_INT); }
    else e->int_ref = 0;
}

/* Not running: the boot's stages can't be taken. */
bool idle_dead(const ev_t *e) {
    int nstuck = 0;
    for (int k = 0; k < LA_NSIG; k++) nstuck += e->stuck[k] != 0;
    return e->in_reset || e->rst_pulses || e->stuck[LA_CLK] || (e->clk_pct >= 0 && e->clk_pct < 30) ||
           e->held[LA_WAIT] || e->held[LA_BUSRQ] || nstuck >= 8;
}
