/* lacap_compare.c - bus cycles, and comparing a capture with the simulated board cycle by cycle.
 *
 * AI-generated (Claude Code). */
#include "lacap_int.h"
static inline unsigned data_of(uint64_t v) { return (unsigned)(v >> LA_D0 & 0xFF); }

/* Where /RESET goes high, or 0. On a real Spectrum /RESET rises slowly
 * (an RC): the Z80 is running milliseconds before the analyser's input
 * sees it high. So: the last fetch from 0000 before the rise, if any. */
int la_start(const la_capture_t *c) {
    /* Fired as soon as it was armed (a capture at once), the master board
     * fills its samples before the trigger with 0 (its channels: A0-A15,
     * /RESET), up to about the trigger (the slave's are real): the bus
     * starts after them. Only a run that ends near the trigger, with the
     * slave's lines showing the CPU running in it (held in reset, the
     * master's lines can be 0 too, but the bus is still then). */
    uint64_t mm = 0;
    for (int k = 0; k < 24; k++) if (chan_map[k] >= 0) mm |= 1ull << chan_map[k];
    mm &= c->have;
    int z = 0;
    while (mm && z < c->n && !(c->s[z] & mm)) z++;
    if (z >= c->n || c->pre < 100 || z < c->pre / 2 || z > c->pre + 50) z = 0;
    int busy = 0;
    for (int i = 1; i < z && (c->have >> LA_MREQ & 1); i++) busy += lo(c->s[i], LA_MREQ) && !lo(c->s[i - 1], LA_MREQ);
    if (busy < 8) z = 0;
    if (!(c->have >> LA_RESET & 1)) return z;
    int r = 0;
    for (int i = z + 1; i < c->n && !r; i++)      /* not the fill's end */
        if (!lo(c->s[i], LA_RESET) && lo(c->s[i - 1], LA_RESET)) r = i;
    if (!r) return z;
    /* /RESET rises slowly (a capacitor): the Z80 can be running well
     * before the analyser sees it high. With the CPU busy before that
     * rise, it isn't where the CPU started: the start is earlier. */
    {
        int falls_before = 0;
        for (int i = z + 1; i < r && falls_before < 50 && (c->have >> LA_MREQ & 1); i++)
            falls_before += lo(c->s[i], LA_MREQ) && !lo(c->s[i - 1], LA_MREQ);
        if (falls_before >= 50) return z;
    }
    bool m1 = c->have >> LA_M1 & 1;
    int best = r;
    for (int i = z + 1; i < r; i++)
        if (lo(c->s[i], LA_MREQ) && !lo(c->s[i - 1], LA_MREQ) && addr_of(c->s[i]) == 0 &&
            (!m1 || lo(c->s[i], LA_M1)))
            best = i;
    return best;
}

int la_cycles(const la_capture_t *c, la_cycle_t **out) {
    const uint64_t need = 1ull << LA_MREQ | 1ull << LA_IORQ | 1ull << LA_RD | 1ull << LA_WR;
    *out = NULL;
    if ((c->have & need) != need) return 0;
    bool m1 = c->have >> LA_M1 & 1, rfsh = c->have >> LA_RFSH & 1, clk = c->have >> LA_CLK & 1;
    int cap = 1024, n = 0;
    la_cycle_t *cy = malloc((size_t)cap * sizeof(la_cycle_t));
    int t = 0;
    bool in = false, mreq = false, saw_rd = false, saw_wr = false;
    int n_act = 0, n_m1 = 0, n_rfsh = 0;
    unsigned rdata = 0xFF, wdata = 0xFF;
    la_cycle_t cur = { 0 };
    /* The address: the one held longest in the cycle. Taken at /MREQ's
     * fall alone, it is sometimes still changing there (seen with some
     * ULAs, whose clock is shaped differently): a bit or two from the last. */
    unsigned run_a = 0, best_a = 0;
    int run_n = 0, best_n = 0;
    int start = la_start(c);
    /* /M1 or /RFSH that hardly change while /MREQ goes all the time: a
     * Z80 with that output dead (the 48K doesn't use /M1, and runs with
     * it). Decoded without it: a fetch is a read with a refresh straight
     * after it; a refresh, a memory cycle with neither /RD nor /WR. */
    bool m1_dead = false, rfsh_dead = false;
    if (m1 || rfsh) {
        int cm = 0, cr = 0, cq = 0;
        for (int i = start + 1; i < c->n; i++) {
            uint64_t x = c->s[i] ^ c->s[i - 1];
            cm += x >> LA_M1 & 1; cr += x >> LA_RFSH & 1; cq += x >> LA_MREQ & 1;
        }
        if (m1 && cq > 1000 && cm <= 20) { m1 = false; m1_dead = true; }
        if (rfsh && cq > 1000 && cr <= 20) { rfsh = false; rfsh_dead = true; }
    }
    for (int i = start; i < c->n; i++) {
        uint64_t v = c->s[i];
        if (clk && i > start && (v >> LA_CLK & 1) && lo(c->s[i - 1], LA_CLK)) t++;
        bool act = lo(v, LA_MREQ) || lo(v, LA_IORQ);
        if (act && !in) {
            in = true;
            mreq = lo(v, LA_MREQ);
            saw_rd = saw_wr = false;
            n_act = n_m1 = n_rfsh = 0;
            /* T states by the time since the start when the rate is known:
             * on the real board CLK is high for only about 40 ns as the
             * analyser sees it, so at 14 MHz most of its rises are missed.
             * The CPU clock is 3.5 MHz (ULA contention stretches it, and
             * the simulated capture stretches the same). */
            int tt = c->freq > 0 ? (int)((int64_t)(i - start) * ZX_FREQ / c->freq) : clk ? t : -1;
            cur = (la_cycle_t){ .addr = (uint16_t)addr_of(v), .sample = i, .t = tt };
            run_n = best_n = 0;
        }
        if (in && act) {
            unsigned a = addr_of(v);
            if (run_n && a == run_a) run_n++;
            else { run_a = a; run_n = 1; }
            if (run_n > best_n) { best_n = run_n; best_a = run_a; }
            /* /M1 and /RFSH by most of the cycle: on a real board /RFSH
             * falls a sample or so before the fetch's /MREQ rises. */
            n_act++;
            if (m1 && lo(v, LA_M1)) n_m1++;
            if (rfsh && lo(v, LA_RFSH)) n_rfsh++;
            if (lo(v, LA_RD)) { saw_rd = true; rdata = data_of(v); }
            if (lo(v, LA_WR)) { saw_wr = true; wdata = data_of(v); }
            if (m1 && lo(v, LA_M1) && !mreq) rdata = data_of(v);       /* the interrupt vector */
            continue;
        }
        if (!in) continue;
        in = false;
        bool saw_m1 = 2 * n_m1 > n_act, saw_rfsh = 2 * n_rfsh > n_act;
        int kind;
        if (mreq) kind = saw_rfsh ? LA_REFRESH : saw_m1 ? LA_FETCH : saw_wr ? LA_WRITE : saw_rd ? LA_READ : !rfsh ? LA_REFRESH : -1;
        else kind = saw_m1 ? LA_INTACK : saw_wr ? LA_OUT : saw_rd ? LA_IN : -1;
        if (kind < 0) continue;
        cur.kind = (uint8_t)kind;
        if (best_n) cur.addr = (uint16_t)best_a;
        cur.data = (uint8_t)(kind == LA_WRITE || kind == LA_OUT ? wdata : kind == LA_REFRESH ? 0 : rdata);
        if (n == cap) cy = realloc(cy, (size_t)(cap *= 2) * sizeof(la_cycle_t));
        cy[n++] = cur;
    }
    if (m1_dead || !(c->have >> LA_M1 & 1))
        for (int k = 0; k + 1 < n; k++)
            if (cy[k].kind == LA_READ && cy[k + 1].kind == LA_REFRESH) cy[k].kind = LA_FETCH;
    (void)rfsh_dead;
    *out = cy;
    return n;
}

static const char *const kind_names[] = { "fetch", "read", "write", "in", "out", "intack", "refresh" };

void cycle_text(const la_cycle_t *y, char *out, int len) {
    if (y->kind == LA_REFRESH) snprintf(out, len, "%-7s %04X   ", kind_names[y->kind], y->addr);
    else snprintf(out, len, "%-7s %04X %02X", kind_names[y->kind], y->addr, y->data);
}

/* A fetch or read from the ROM that isn't what the ROM holds. The edge
 * connector's /ROMCS is the ROM's select (low for 0000-3FFF); held high
 * there by an interface (a cartridge, the Interface 1), the ROM is off. */
bool rom_wrong(const la_capture_t *c, const la_cycle_t *y) {
    if ((y->kind != LA_FETCH && y->kind != LA_READ) || y->addr >= 0x4000) return false;
    if (c->have >> LA_ROMCS & 1) {
        /* The select follows /MREQ through gates: low in the cycle's first
         * T state or so (8 samples at 28 MHz, more at faster rates). */
        int span = c->freq > 0 ? c->freq / ZX_FREQ : 8;
        bool sel = false;
        for (int i = y->sample; i < c->n && i <= y->sample + span && !sel; i++) sel = lo(c->s[i], LA_ROMCS);
        if (!sel) return false;
    }
    return zx48_rom_image()[y->addr] != y->data;
}

/* ------------------------------------------------------------------------ */
/* The command line                                                          */
/* ------------------------------------------------------------------------ */

/* An access the ULA can hold the CPU up for: 4000-7FFF, or an even port. */
static bool ula_contended(const la_cycle_t *x) {
    if (x->kind == LA_REFRESH || x->kind == LA_INTACK) return false;
    if (x->kind == LA_IN || x->kind == LA_OUT) return !(x->addr & 1) || (x->addr >= 0x4000 && x->addr < 0x8000);
    return x->addr >= 0x4000 && x->addr < 0x8000;
}

/* The addresses where reads don't give back what was written (BAD[a] = 1):
 * on a working board, where there's no RAM (8000-FFFF on a 16K one, which
 * the ROM's RAM test writes to find where the RAM ends). */
void no_ram_cells(const la_cycle_t *cy, int n, unsigned char *bad) {
    static short last[65536];
    for (int k = 0; k < 65536; k++) last[k] = -1;
    memset(bad, 0, 65536);
    for (int k = 0; k < n; k++) {
        const la_cycle_t *x = &cy[k];
        if (x->addr < 0x4000) continue;
        if (x->kind == LA_WRITE) last[x->addr] = (short)x->data;
        else if ((x->kind == LA_READ || x->kind == LA_FETCH) && last[x->addr] >= 0 && last[x->addr] != x->data)
            bad[x->addr] = 1;
    }
}

/* RAM reads that don't give back what was written there earlier in the
 * same capture: a fault in the RAM (or its buffers, the multiplexers),
 * whatever the machine was doing; not at an address in SKIP (no RAM there:
 * the working board's reads don't either). Prints up to SHOW of them. */
static int ram_readback(const la_cycle_t *cy, int n, int show, const unsigned char *skip) {
    static short last[65536];
    for (int k = 0; k < 65536; k++) last[k] = -1;
    int bad = 0;
    for (int k = 0; k < n; k++) {
        const la_cycle_t *x = &cy[k];
        if (x->addr < 0x4000 || (skip && skip[x->addr])) continue;
        if (x->kind == LA_WRITE) last[x->addr] = (short)x->data;
        else if ((x->kind == LA_READ || x->kind == LA_FETCH) && last[x->addr] >= 0 && last[x->addr] != x->data) {
            if (bad < show)
                printf("  T %d: %s %04X gave %02X, %02X was written there (bits %02X wrong)\n", x->t,
                       kind_names[x->kind], x->addr, x->data, last[x->addr], (x->data ^ last[x->addr]) & 0xFF);
            bad++;
            last[x->addr] = (short)x->data;     /* report each wrong cell once */
        }
    }
    return bad;
}

void print_info(const char *path, const la_capture_t *c, int count) {
    int start = la_start(c);
    printf("%s: %d samples", path, c->n);
    if (c->freq) printf(" at %.3f MHz (%.3f ms)", c->freq / 1e6, c->n * 1e3 / c->freq);
    printf(", the Z80 starts after /RESET at sample %d\n", start);
    if (c->freq && c->freq < 10000000)
        printf("Too slow to tell the bus cycles apart (a T state is 286 ns): "
               "capture at 14285714 Hz or faster; the cycles below are not to be trusted\n");
    printf("Channels:");
    for (int s = 0; s < LA_NSIG; s++) if (c->chan[s] >= 0) printf(" %d=%s", c->chan[s] + 1, sig_names[s]);
    printf("\nNot in the capture:");
    for (int s = 0; s < LA_NSIG; s++) if (c->chan[s] < 0) printf(" %s", sig_names[s]);
    /* What a logic probe would say, from /RESET going high. */
    printf("\n\nFrom /RESET going high (%d samples):\n", c->n - start);
    for (int s = 0; s < LA_NSIG; s++) {
        if (c->chan[s] < 0) continue;
        int rises = 0, highs = 0;
        for (int i = start; i < c->n; i++) {
            highs += c->s[i] >> s & 1;
            rises += i > start && (c->s[i] >> s & 1) && lo(c->s[i - 1], s);
        }
        int total = c->n - start;
        const char *state = rises ? "pulsing" : total && highs == total ? "high" : "low";
        printf("  %-8s %-8s", sig_names[s], state);
        if (rises) printf(" %6d rises, high %3d%%", rises, total ? (int)(100.0 * highs / total + 0.5) : 0);
        printf("\n");
    }
    la_cycle_t *cy;
    int n = la_cycles(c, &cy);
    if (!n) { printf("\nNo bus cycles (needs /MREQ /IORQ /RD /WR)\n"); free(cy); return; }
    int per[7] = { 0 }, wrong = 0;
    for (int k = 0; k < n; k++) { per[cy[k].kind]++; wrong += rom_wrong(c, &cy[k]); }
    printf("\n%d bus cycles:", n);
    for (int k = 0; k < 7; k++) if (per[k]) printf(" %d %s", per[k], kind_names[k]);
    printf("\nROM reads that aren't what the ROM holds: %d\n", wrong);
    int rb = ram_readback(cy, n, 0, NULL);
    printf("RAM reads that don't give back what was written: %d\n", rb);
    if (rb) ram_readback(cy, n, 5, NULL);
    /* The RAM test's result: the ROM keeps the top of the RAM it found
     * good in P-RAMT (5CB4); its last write here is the one that counts. */
    int lo_b = -1, hi_b = -1;
    for (int k = 0; k < n; k++)
        if (cy[k].kind == LA_WRITE) {
            if (cy[k].addr == 0x5CB4) lo_b = cy[k].data;
            if (cy[k].addr == 0x5CB5) hi_b = cy[k].data;
        }
    if (lo_b >= 0 && hi_b >= 0) {
        unsigned top = (unsigned)(hi_b << 8 | lo_b);
        printf("The RAM test: the RAM is good up to %04X (P-RAMT)", top);
        if (top == 0xFFFF) printf(": all 48K passed\n");
        else if (top == 0x7FFF) printf(": it stopped at 8000, the first byte of the upper 32K (or it's a 16K machine)\n");
        else if (top >= 0x8000) printf(": it stopped at %04X, in the upper 32K (8000-FFFF)\n", top + 1);
        else printf(": it stopped at %04X, in the lower 16K (4000-7FFF)\n", top + 1);
    }
    for (int k = 0, shown = 0; k < n && shown < 5; k++)
        if (rom_wrong(c, &cy[k])) {
            printf("  T %d: %s %04X gave %02X, the ROM holds %02X\n", cy[k].t, kind_names[cy[k].kind], cy[k].addr,
                   cy[k].data, zx48_rom_image()[cy[k].addr]);
            shown++;
        }
    printf("\nThe first %d:\n", count < n ? count : n);
    for (int k = 0; k < n && k < count; k++) {
        char tx[32];
        cycle_text(&cy[k], tx, sizeof(tx));
        printf("  T %6d  %s%s\n", cy[k].t, tx, rom_wrong(c, &cy[k]) ? "  <- not the ROM's" : "");
    }
    free(cy);
}

/* The first cycle at ADDR from just before the trigger on, or -1. */
static int at_trigger(const la_cycle_t *cy, int n, const la_capture_t *c, int addr) {
    for (int k = 0; k < n; k++) if (cy[k].addr == addr && cy[k].sample >= c->pre - 16) return k;
    return -1;
}
static bool runs_rom(const la_cycle_t *cy, int n, int k) {
    const uint8_t *rom = zx48_rom_image();
    int seen = 0, right = 0;
    for (int j = k; j < n && seen < 20; j++) {
        if (cy[j].kind != LA_FETCH && cy[j].kind != LA_READ) continue;
        if (cy[j].addr >= 0x4000) continue;
        seen++;
        right += rom[cy[j].addr] == cy[j].data;
    }
    return seen >= 8 && right * 10 >= seen * 9;
}
int first_fetch0_n(const la_cycle_t *cy, int n, int *restarts) {
    int first = -1, use = -1, before = 0, k0 = 0;
    for (int k = 0; k < n && n - k >= BOUNCE_MIN_AFTER; k++)
        if (cy[k].kind == LA_FETCH && cy[k].addr == 0) {
            if (first < 0) first = k;
            if (k > first && runs_rom(cy, n, k)) { use = k; before = k0; }
            k0++;
        }
    if (use < 0) {                      /* no bounce: the first one */
        if (restarts) *restarts = 0;
        return first < 0 ? 0 : first;
    }
    if (restarts) *restarts = before;
    return use;
}

/* Without the refresh cycles (unless asked). */
static int filter(la_cycle_t *cy, int n, bool refresh) {
    int m = 0;
    for (int k = 0; k < n; k++) if (refresh || cy[k].kind != LA_REFRESH) cy[m++] = cy[k];
    return m;
}

int compare(const char *an, const la_capture_t *a, const char *bn, const la_capture_t *b,
                   int context, bool refresh, cmp_out_t *res) {
    la_cycle_t *ca, *cb;
    int na = la_cycles(a, &ca), nb = la_cycles(b, &cb);
    na = filter(ca, na, refresh);
    nb = filter(cb, nb, refresh);
    if (!na || !nb) {
        printf("No bus cycles in %s\n", !na ? an : bn);
        free(ca); free(cb);
        return 1;
    }
    /* Lined up on the reset's first fetch from 0000, or, triggered on
     * another address, on the cycle at the trigger with that address, from
     * a few cycles before it. */
    int align = a->trig_addr >= 0 ? a->trig_addr : b->trig_addr;
    int ia, ib;
    char from[64] = "";
    if (align < 0 || align == 0xFFFF) {
        int ra = 0, rb = 0;
        ia = first_fetch0_n(ca, na, &ra);
        ib = first_fetch0_n(cb, nb, &rb);
        if (ra || rb)
            printf("The CPU started again from 0000 %d time%s just after the reset (the reset button bouncing as\n"
                   "it's let go): compared from the last start\n", ra + rb, ra + rb == 1 ? "" : "s");
        if (ia || ib) snprintf(from, sizeof(from), ", from the %s fetch at 0000", ra || rb ? "last" : "first");
        /* The capture began a cycle or two after the fetch at 0000 (the
         * analyser saw the start late): the working board's from the same
         * cycle as the capture's first. */
        if (!(ca[ia].kind == LA_FETCH && ca[ia].addr == 0) && cb[ib].kind == LA_FETCH && cb[ib].addr == 0)
            for (int k = ib; k < nb && k < ib + 64; k++)
                if (cb[k].kind == ca[ia].kind && cb[k].addr == ca[ia].addr && cb[k].data == ca[ia].data) {
                    ib = k;
                    snprintf(from, sizeof(from), ", from %04X (the capture began after 0000)", ca[ia].addr);
                    break;
                }
    } else {
        ia = at_trigger(ca, na, a, align);
        ib = at_trigger(cb, nb, b, align);
        if (ia < 0 || ib < 0) {
            printf("No bus cycle at %04X at the trigger in %s\n", align, ia < 0 ? an : bn);
            free(ca); free(cb);
            return 1;
        }
        int back = ia < ib ? ia : ib;
        if (back > 40) back = 40;
        if (align == 0x0038) back = 0;  /* what it interrupted is anyone's */
        ia -= back;
        ib -= back;
        snprintf(from, sizeof(from), ", from %d before the trigger's %04X", back, align);
    }
    int ta = ca[ia].t, tb = cb[ib].t;
    printf("A: %s (%d cycles%s)\nB: %s (%d cycles%s)\n", an, na - ia, from, bn, nb - ib, from);
    int k = 0, soft = 0, timing = -1, ndiff = 0, diffs[8], area[4] = { 0 }, moved = 0;
    /* On a frame interrupt: the ROM's routine, 0038 to its RET at 0052 and
     * the two reads of the address it goes back to. After that each goes
     * back to what it was doing when the interrupt came. */
    int lim_a = na, lim_b = nb;
    bool routine = false;
    if (align == 0x0038) {
        for (int j = ia; j < na; j++) if (ca[j].kind == LA_FETCH && ca[j].addr == 0x0052) { lim_a = j + 1; break; }
        for (int j = ib; j < nb; j++) if (cb[j].kind == LA_FETCH && cb[j].addr == 0x0052) { lim_b = j + 1; break; }
        /* the RET's two reads (and the refresh between) */
        for (int n = 0; n < 2 && lim_a < na; lim_a++) if (ca[lim_a].kind == LA_READ) n++;
        for (int n = 0; n < 2 && lim_b < nb; lim_b++) if (cb[lim_b].kind == LA_READ) n++;
        routine = lim_a < na && lim_b < nb;
        if (routine) { na = lim_a; nb = lim_b; }
    }
    for (; ia + k < na && ib + k < nb; k++) {
        /* The same bus cycle (kind and address) is the CPU doing the same;
         * the data can differ under that (a fault in what is read, or a
         * counter, a pushed address). The first cycle that isn't the same
         * ends it. */
        const la_cycle_t *x = &ca[ia + k], *y = &cb[ib + k];
        /* A read or write of RAM at another address is the state too (the
         * stack pointer where it was interrupted): what the CPU does is the
         * code it runs, and a fault on the address lines moves the ROM's
         * fetches as well. */
        if (x->kind == y->kind && x->addr != y->addr && (x->kind == LA_READ || x->kind == LA_WRITE) &&
            x->addr >= 0x4000 && y->addr >= 0x4000) { moved++; continue; }
        if (x->kind != y->kind || x->addr != y->addr) break;
        if (x->kind == LA_REFRESH || x->data == y->data) continue;
        if (x->kind == LA_IN || x->kind == LA_INTACK) { soft++; continue; }   /* a port or the floating bus */
        if (ndiff < 8) diffs[ndiff] = k;
        ndiff++;
        area[x->addr < 0x4000 ? 0 : x->addr < 0x5B00 ? 1 : x->addr < 0x5CC0 ? 2 : 3]++;
    }
    /* Timing, after taking out a steady difference of the clocks (the real
     * Spectrum's crystal against the analyser's): what's left is a real
     * difference in when things happen. */
    /* Gap by gap: the T states from one cycle to the next. Next to an
     * access the ULA can hold up (4000-7FFF, an even port) the gaps differ
     * on any two boots: the reset doesn't reset the ULA, so where in its
     * frame the CPU is differs. Elsewhere a gap that differs is a real
     * difference. The clocks: the sum of those other gaps. */
    long long sa = 0, sb = 0;
    int held = 0;
    for (int j = 1; j < k; j++) {
        const la_cycle_t *x0 = &ca[ia + j - 1], *x = &ca[ia + j], *y0 = &cb[ib + j - 1], *y = &cb[ib + j];
        if (x->t < 0 || x0->t < 0 || y->t < 0 || y0->t < 0) continue;
        int ga = x->t - x0->t, gb = y->t - y0->t;
        if (ula_contended(x0) || ula_contended(x)) { if (abs(ga - gb) >= 2) held++; continue; }
        sa += ga; sb += gb;
        if (timing < 0 && abs(ga - gb) >= 2) timing = j;
    }
    if (sb > 1000 && sa != sb) {
        double ppm = ((double)sa / sb - 1) * 1e6;
        if (fabs(ppm) < 5000)
            printf("Clocks: A runs %+.0f ppm against B (%lld T against %lld T away from the ULA's RAM)\n", -ppm, sa, sb);
    }
    if (timing >= 0)
        printf("Timing: cycle %d comes %d T after the one before in A, %d T in B\n", timing,
               ca[ia + timing].t - ca[ia + timing - 1].t, cb[ib + timing].t - cb[ib + timing - 1].t);
    else if (k > 0)
        printf("Timing: the same, within a T state%s\n", held ? "," : "");
    if (held)
        printf("  but for %d waits next to 4000-7FFF or a port, which the ULA holds the CPU up for where it is in its\n"
               "  frame: that differs on every reset (the ULA isn't reset), so they aren't a fault\n", held);
    if (soft) printf("%d port reads / interrupt acknowledges read differently (ports, the floating bus)\n", soft);
    int rc = 0;
    bool end = ia + k >= na || ib + k >= nb;
    /* A frame interrupt in one and not the other: the ULA makes them, and
     * the reset doesn't reset it, so when they come differs every boot. */
    bool irq = !end && (ca[ia + k].kind == LA_INTACK) != (cb[ib + k].kind == LA_INTACK);
    if (moved) printf("%d RAM reads and writes are at other addresses (the stack pointer where it was: the state)\n", moved);
    if (routine && end)
        printf("The interrupt routine (0038-0052, %d cycles): after its RET each goes back to what it was doing\n", k);
    if (end && !ndiff) printf("The same for all %d cycles both have\n", k);
    else if (end) printf("The CPU does the same for all %d cycles both have\n", k);
    else printf("The CPU does the same for %d cycles\n", k);
    if (ndiff) {
        if (area[0]) rc = 2;            /* the ROM read differently: a fault; RAM: the state */
        /* Where: a ROM read that differs is a fault; the system variables
         * (FRAMES, the last key...) and the stack (pushed return addresses)
         * differ on any two machines captured at different times. */
        printf("The data differ in %d of them:", ndiff);
        const char *an_[4] = { "ROM (0000-3FFF: a fault)", "the screen (4000-5AFF)",
                               "the system variables (5B00-5CBF: counters, keys)", "the rest of the RAM (5CC0-FFFF: the stack, programs)" };
        for (int z = 0, first = 1; z < 4; z++)
            if (area[z]) { printf("%s %d in %s", first ? "" : ",", area[z], an_[z]); first = 0; }
        printf("\n%s:\n", ndiff > 8 ? "The first 8" : "They are");
        printf("  %-6s %-9s %-16s   %-9s %s\n", "cycle", "A T", "A", "B T", "B");
        for (int d = 0; d < ndiff && d < 8; d++) {
            int j = diffs[d];
            char tx[32], ty[32];
            cycle_text(&ca[ia + j], tx, sizeof(tx));
            cycle_text(&cb[ib + j], ty, sizeof(ty));
            printf("  %-6d %-9d %-16s   %-9d %s%s\n", j, ca[ia + j].t - ta, tx, cb[ib + j].t - tb, ty,
                   rom_wrong(a, &ca[ia + j]) ? "  <- A's isn't the ROM's" : rom_wrong(b, &cb[ib + j]) ? "  <- B's isn't the ROM's" : "");
        }
    }
    /* Where both read back wrong there's no RAM (a 16K machine's upper half). */
    static unsigned char nra[65536], nrb[65536];
    no_ram_cells(ca + ia, na - ia, nra);
    no_ram_cells(cb + ib, nb - ib, nrb);
    int both = 0;
    for (int k = 0x4000; k < 65536; k++) both += nra[k] && nrb[k];
    if (both) printf("%d addresses read back wrong on both: no RAM there on this machine (not a fault)\n", both);
    int rba = ram_readback(ca + ia, na - ia, 0, nrb), rbb = ram_readback(cb + ib, nb - ib, 0, nra);
    if (rba) { printf("%s: %d RAM reads don't give back what was written there:\n", an, rba); ram_readback(ca + ia, na - ia, 5, nrb); rc = 2; }
    if (rbb) { printf("%s: %d RAM reads don't give back what was written there:\n", bn, rbb); ram_readback(cb + ib, nb - ib, 5, nra); rc = 2; }
    if (ndiff && !area[0] && (end || irq) && !rba && !rbb)
        printf("Only RAM data differ (counters, the stack's pushed registers): the state the machines were in, not a fault\n");
    if (irq) {
        printf("\nFrom cycle %d %s takes a frame interrupt and %s doesn't yet: the ULA makes them 50 times a second,\n"
               "and the reset doesn't reset it, so when they come differs on every reset. Not a fault; compared up to there:\n",
               k, ca[ia + k].kind == LA_INTACK ? "A" : "B", ca[ia + k].kind == LA_INTACK ? "B" : "A");
        printf("  %-6s %-9s %-16s   %-9s %s\n", "cycle", "A T", "A", "B T", "B");
        for (int j = k - 2 < 0 ? 0 : k - 2; j <= k + 1 && ia + j < na && ib + j < nb; j++) {
            char tx[32], ty[32];
            cycle_text(&ca[ia + j], tx, sizeof(tx));
            cycle_text(&cb[ib + j], ty, sizeof(ty));
            printf("%s %-6d %-9d %-16s   %-9d %s\n", j == k ? ">" : " ", j, ca[ia + j].t - ta, tx, cb[ib + j].t - tb, ty);
        }
    } else if (!end) {
        rc = 2;
        printf("\nThe first difference in what the CPU does, cycle %d:\n", k);
        printf("  %-6s %-9s %-16s   %-9s %s\n", "cycle", "A T", "A", "B T", "B");
        for (int j = k - context < 0 ? 0 : k - context; j <= k + 2 && ia + j < na && ib + j < nb; j++) {
            char tx[32], ty[32];
            cycle_text(&ca[ia + j], tx, sizeof(tx));
            cycle_text(&cb[ib + j], ty, sizeof(ty));
            printf("%s %-6d %-9d %-16s   %-9d %s\n", j == k ? ">" : " ", j, ca[ia + j].t - ta, tx, cb[ib + j].t - tb, ty);
        }
        const la_cycle_t *x = &ca[ia + k];
        if (rom_wrong(a, x)) printf("A read %02X at %04X: the ROM holds %02X\n", x->data, x->addr, zx48_rom_image()[x->addr]);
        const la_cycle_t *y = &cb[ib + k];
        if (rom_wrong(b, y)) printf("B read %02X at %04X: the ROM holds %02X\n", y->data, y->addr, zx48_rom_image()[y->addr]);
    }
    if (res) {
        *res = (cmp_out_t){ .rc = rc, .diverged = !end && !irq, .cycle = k };
        if (!end) { res->a = ca[ia + k]; res->b = cb[ib + k]; }
    }
    free(ca); free(cb);
    return rc;
}

/* A capture of the simulated board, healthy or with the faults. */
/* Samples (at the simulation's 28 MHz) to record to cover capture A, and a
 * little more: 0 if A's rate isn't known. */
int sim_length(const la_capture_t *a) {
    if (a->freq <= 0) return 0;
    return (int)((double)(a->n - a->pre) * (ZX_FREQ * ZX_STEPS_PER_CYCLE) / a->freq) + 4000;
}
