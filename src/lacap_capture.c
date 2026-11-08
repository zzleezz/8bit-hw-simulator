/* lacap_capture.c - the boot's stages, and capturing them from the analyser.
 *
 * AI-generated (Claude Code). */
#include "lacap_int.h"

/* When the CPU went another way: at the capture's first bus cycle or after N.
 * The text is in one static buffer: use it before the next call (not two in
 * one printf). */
const char *went_when(int cycle) {
    static char b[64];
    if (cycle <= 0) return "from the capture's first bus cycle";
    snprintf(b, sizeof(b), "after %d bus cycle%s", cycle, cycle == 1 ? "" : "s");
    return b;
}

/* A stage's name as the window's boxes say it ("RAM test", not "ramcheck"). */
const char *stage_plain(const char *name) {
    static const char *const from[] = { "idle", "reset", "ramcheck", "ramdone", "copyright", "main", "interrupt" };
    static const char *const to[] = { "Idle", "Reset", "RAM test", "RAM done", "Copyright", "Main loop", "Interrupt" };
    for (int k = 0; k < 7; k++) if (!strcmp(name, from[k])) return to[k];
    return name;
}

const la_stage_t *stage_named(const char *t) {
    for (const la_stage_t *s = la_stages; s->name; s++) if (!strcasecmp(s->name, t)) return s;
    return NULL;
}

/* --la-trigger: a stage's name, addr=XXXX (all 16 address lines), or
 * raw:TYPE,BASE,COUNT,VALUE. Sets *addr to the address (-1 for raw). Not
 * A15 alone for the reset: while the button is held the Z80's address
 * lines float, and A15 and A14 drift high (live1.lac). */
static bool pick_trigger(const char *t, lp_capture_req_t *q, int *addr, char *err, int errlen) {
    unsigned a, ty, base, count, value;
    *addr = -1;
    if (!t) t = "reset";
    const la_stage_t *st = stage_named(t);
    char buf[32];
    if (st) { snprintf(buf, sizeof(buf), "addr=%04X", st->addr); t = buf; }
    if (sscanf(t, "addr=%x", &a) == 1) {
        long v = addr_pattern(a & 0xFFFF);
        if (v < 0) { snprintf(err, errlen, "an address trigger needs A0-A15 on channels 1-16"); return false; }
        q->trig_type = LP_COMPLEX; q->trig_base = 0; q->trig_count = 16; q->trig_value = (unsigned)v;
        *addr = (int)(a & 0xFFFF);
        return true;
    }
    if (sscanf(t, "raw:%u,%u,%u,%u", &ty, &base, &count, &value) == 4) {
        q->trig_type = (int)ty; q->trig_base = (int)base; q->trig_count = (int)count; q->trig_value = value;
        return true;
    }
    int n = snprintf(err, errlen, "--la-trigger %s: a stage (", t);
    for (const la_stage_t *s = la_stages; s->name && n < errlen; s++) n += snprintf(err + n, errlen - n, "%s%s", s == la_stages ? "" : ", ", s->name);
    if (n < errlen) snprintf(err + n, errlen - n, "), addr=XXXX or raw:TYPE,BASE,COUNT,VALUE");
    return false;
}

/* Was it the address, or a glitch while the lines changed? A bus cycle
 * holds its address for 2 T or more (8 samples at 14 MHz); a glitch lasts
 * a few ns. Wants it for 3 samples in a row near the trigger. */
static bool real_trigger(const la_capture_t *c, int addr) {
    int run = 0, need = c->freq > 0 && c->freq < 10000000 ? 1 : 3;
    for (int i = c->pre - 8; i < c->pre + 40 && i < c->n; i++) {
        if (i < 0) continue;
        run = (int)(c->s[i] >> LA_A0 & 0xFFFF) == addr ? run + 1 : 0;
        if (run >= need) return true;
    }
    return false;
}

bool capture_real(const la_cli_t *o, la_capture_t *c, char *err, int errlen) {
    char ports[LP_MAXDEV][64];
    lp_capture_req_t q = { 0 };
    q.ndev = pick_ports(o->ports, ports, LP_MAXDEV, err, errlen);
    if (!q.ndev) return false;
    for (int d = 0; d < q.ndev; d++) q.port[d] = ports[d];
    for (int k = 0; k < 24 * q.ndev && k < 48; k++) if (chan_map[k] >= 0) q.chan[q.nchan++] = k;
    q.freq = o->freq > 0 ? o->freq : 14285714;
    q.pre = o->pre > 0 ? o->pre : 1000;
    q.post = o->post > 0 ? o->post : 97000;
    q.timeout = o->timeout;
    int addr;
    if (!pick_trigger(o->trigger, &q, &addr, err, errlen)) return false;
    const la_stage_t *st = stage_named(o->trigger ? o->trigger : "reset");
    /* --la-at MS: arm MS after the reset (seen on FFFF) whatever the
     * trigger; with an address the CPU is at all the time, it fires at
     * once, and the capture shows where the boot is MS after the reset. */
    la_stage_t at_stage;
    if (o->at > 0) {
        at_stage = (la_stage_t){ "the time", addr, o->at, "armed again a set time after the reset" };
        st = &at_stage;
        if (addr >= 0) printf("Armed on the reset, then again %.1f ms after it, for %04X\n", o->at, addr);
        else printf("Armed on the reset, then again %.1f ms after it\n", o->at);
    }
    else if (st) printf("Trigger: %s\n", st->what);
    else if (addr >= 0) printf("Trigger: the address bus showing %04X\n", addr);
    if (st && st->addr != 0x0038)
        printf("Hold the Spectrum's reset button down now, and let go once it says Armed.\n");
    if (st == &at_stage)
        printf("(armed for the reset first, then again by itself %.1f ms after it)\n", st->ms);
    printf("Capturing %d channels from %s%s%s at %d Hz, %d samples before the trigger and %d after\n", q.nchan,
           ports[0], q.ndev > 1 ? " (master) and " : "", q.ndev > 1 ? ports[1] : "", q.freq, q.pre, q.post);
    lp_printf = pico_out;
    int tries = o->tries > 0 ? o->tries : 5;
    bool boot = st && st->addr != 0x0038;
    /* A stage after the reset: armed on the reset first (FFFF, the RAM
     * test's first write, which the bus can't show before), then again
     * MARGIN ms before the stage is due. Armed for the whole boot, the RAM
     * test's writes to every address could show the stage's address for an
     * instant as the lines change (ramcheck, ramdone 2026-11-06). */
    /* Armed once from the reset: with the zx firmware the trigger needs its
     * address for 75 ns, and a glitch can't fire it. Timed (armed again
     * a set time after the reset) only for --la-at. */
    bool timed = st == &at_stage;
    double margin = st == &at_stage ? 0 : 20;
    for (int attempt = 1; ; attempt++) {
        if (attempt > 1 && boot) {
            /* The stage comes once a boot: the next try needs a reset. */
            printf("Hold the Spectrum's reset button down, then press Enter here (keep holding the button)\n");
            char line[16];
            (void)line;
            if (!la_wait("Hold the Spectrum's reset button down, then continue (keep holding the button)")) {
                snprintf(err, errlen, "no key pressed");
                return false;
            }
            printf("Let go of the reset button once it says Armed.\n");
        }
        uint8_t *v[LP_MAXDEV * 24] = { 0 };
        int n = 0, pre = 0;
        if (timed) {
            lp_capture_req_t r = q;
            r.pre = 100; r.post = 1000;
            long pv = addr_pattern(getenv("LA_FIRST_ADDR") ? (unsigned)strtoul(getenv("LA_FIRST_ADDR"), NULL, 16) : 0xFFFFu); /* LA_FIRST_ADDR: a test without a reset */
            r.trig_type = LP_COMPLEX; r.trig_base = 0; r.trig_count = 16; r.trig_value = (unsigned)pv;
            bool got = lp_capture(&r, v, &n, &pre, err, errlen);
            if (!got) return false;
            /* The Spectrum starting, or the address lines floating while the
             * button is held down (they can show FFFF)? Started, the ROM's
             * RAM fill (11DC-11E1) follows FFFF within a few us. */
            la_capture_t rc;
            rawchan_t ch[LP_MAXDEV * 24];
            for (int k = 0; k < r.nchan; k++) {
                ch[k].number = r.chan[k];
                snprintf(ch[k].name, sizeof(ch[k].name), "%s", la_signal_name(chan_map[r.chan[k]]));
                ch[k].v = v[k];
                ch[k].n = n;
            }
            memset(&rc, 0, sizeof(rc));
            rc.freq = r.freq;
            rc.pre = pre;
            bool built = la_build(&rc, ch, r.nchan, err, errlen);
            for (int k = 0; k < r.nchan; k++) { free(v[k]); v[k] = NULL; }
            if (!built) return false;
            int fill = 0;
            for (int i = rc.pre; i < rc.n; i++) {
                unsigned a = addr_of(rc.s[i]);
                if (a >= 0x11DC && a <= 0x11E1 && lo(rc.s[i], LA_M1) && lo(rc.s[i], LA_MREQ)) fill++;
            }
            la_free(&rc);
            if (fill < 8 && !getenv("LA_FIRST_ADDR")) {
                if (attempt >= tries) { snprintf(err, errlen, "%d times it saw the address lines floating, not the reset", tries); return false; }
                printf("That wasn't the Spectrum starting (the address lines float while the button is held down): "
                       "trying again (%d of %d)\n", attempt + 1, tries);
                continue;
            }
            /* The reset's FFFF came this long before its samples did: its 1000
             * after it, and the USB (about a ms). */
            double t_reset = lp_trigger_ms - 1000.0 * 1000 / q.freq - 1;
            q.start_at = t_reset + st->ms - margin;
            double wait = q.start_at - lp_now_ms();
            printf("The reset seen: now waiting for %s (armed again by itself in %.0f ms, %.0f ms before it's due)\n",
                   st->name, wait, margin);
            if (wait < 50) { snprintf(err, errlen, "the reset came in %.0f ms too late to arm for %s", -wait, st->name); return false; }
            q.timeout = 3;
        }
        if (timed) lp_printf = pico_quiet;  /* not "Armed" again: the button is let go already */
        bool got = lp_capture(&q, v, &n, &pre, err, errlen);
        lp_printf = pico_out;
        if (got && timed) printf("Got %s\n", st->name);
        q.start_at = 0;
        if (!got && timed && strstr(err, "no trigger") && st != &at_stage) {
            /* Armed too late: the stage came sooner, or never: a fault
             * before it (the RAM test ends early on bad RAM). Once more
             * armed sooner, in case it's the PC's timing. */
            if (margin >= 80 || attempt >= tries) {
                snprintf(err, errlen, "%s didn't come when it does on a working Spectrum (%.0f ms after the reset): it came "
                         "sooner or not at all, so something before it went wrong%s", st->name, st->ms,
                         st->addr == 0x5C7B ? " (the RAM test ends early if the RAM has a fault: capture ramcheck)" :
                         ": capture an earlier stage");
                return false;
            }
            margin *= 4;
            printf("%s didn't come: trying again armed %.0f ms before it (%d of %d)\n", st->name, margin, attempt + 1, tries);
            continue;
        }
        if (!got) return false;
        rawchan_t ch[LP_MAXDEV * 24];
        for (int k = 0; k < q.nchan; k++) {
            ch[k].number = q.chan[k];
            snprintf(ch[k].name, sizeof(ch[k].name), "%s", la_signal_name(chan_map[q.chan[k]]));
            ch[k].v = v[k];
            ch[k].n = n;
        }
        memset(c, 0, sizeof(*c));
        c->freq = q.freq;
        c->pre = pre;
        c->trig_addr = addr;
        bool ok = la_build(c, ch, q.nchan, err, errlen);
        for (int k = 0; k < q.nchan; k++) free(v[k]);
        if (!ok) return false;
        /* Fired the moment it armed: the board fills the samples before the
         * trigger with 0 (all 40 lines low, which a running Z80 never
         * shows). The Spectrum was past the stage already. */
        bool instant = c->pre > 0 && c->n > 0 && c->s[0] == 0 && c->s[c->pre / 2] == 0;
        /* Or BASIC was running already: the keyboard loop (10A8) before the
         * trigger comes after every stage of the boot. */
        bool basic = false;
        for (int i = 0; boot && !instant && i < c->pre && i < c->n && !basic; i++)
            basic = addr_of(c->s[i]) == 0x10A8 && lo(c->s[i], LA_M1) && lo(c->s[i], LA_MREQ);
        if ((instant || basic) && boot && st != &at_stage) {
            la_free(c);
            if (attempt >= tries) { snprintf(err, errlen, "the Spectrum wasn't reset while it was armed, %d times", tries); return false; }
            printf("%s: the Spectrum was past that stage already (the reset button wasn't held down when it armed)\n"
                   "Trying again (%d of %d)\n", instant ? "It fired as soon as it was armed" : "BASIC was running when it fired",
                   attempt + 1, tries);
            continue;
        }
        if (addr < 0 || real_trigger(c, addr)) return true;
        /* Kept, to see what the glitch was: NAME-glitchN.lac */
        if (o->capture) {
            char gp[1024], e2[256];
            const char *dot = strrchr(o->capture, '.');
            int stem = dot ? (int)(dot - o->capture) : (int)strlen(o->capture);
            snprintf(gp, sizeof(gp), "%.*s-glitch%d.lac", stem, o->capture, attempt);
            if (la_write(gp, c, e2, sizeof(e2))) printf("(kept as %s)\n", gp);
        }
        la_free(c);
        if (attempt >= tries) {
            snprintf(err, errlen, "%d glitches on the address lines and not once %04X itself: given up", tries, addr);
            return false;
        }
        printf("That was a glitch on the address lines, not %04X itself: trying again (%d of %d)\n", addr, attempt + 1, tries);
        if (!lp_zx_firmware)
            printf("(the boards' stock firmware fires on glitches a few ns long; the zx firmware doesn't: see la-firmware)\n");
    }
}

bool record_board(const la_cli_t *o, int post, int trig_addr, la_capture_t *c, char *err, int errlen) {
    board_t *b = la_new_board(o->machine ? o->machine : &machine_zx48);
    for (int i = 0; i < o->nfaults; i++)
        if (!board_apply_fault_spec(b, o->faults[i])) {
            snprintf(err, errlen, "bad fault spec: %s", o->faults[i]);
            board_destroy(b);
            return false;
        }
    if (o->cart && !board_load(b, o->cart, false, err, errlen)) {
        board_destroy(b);
        return false;
    }
    /* Before the trigger: the analyser's 1000 samples at 14 MHz are 70 us,
     * 2000 here at 28 MHz. The stages are up to 1.8 s after power on. */
    int pre = o->pre > 0 && !o->capture ? o->pre : trig_addr >= 0 ? 2000 : 512;
    post = post > 0 ? post : o->post > 0 ? o->post : 32000;
    /* A frame interrupt: once it's idle in BASIC (the main loop is 1.8 s
     * after power on), as a running Spectrum is when it's captured. */
    int arm = trig_addr == 0x0038 ? 2000 : 0;

    bool ok = trig_addr >= 0 ? la_record_at(b, pre, post, trig_addr, arm, arm + 4000, c, err, errlen)
                             : la_record(b, pre, post, c, err, errlen);
    board_destroy(b);
    return ok;
}

/* --la-record --la-trigger: the address of a stage or addr=XXXX, else -1. */
int trigger_addr_of(const char *t) {
    const la_stage_t *st = stage_named(t);
    unsigned a;
    if (st) return st->addr;
    if (sscanf(t, "addr=%x", &a) == 1) return (int)(a & 0xFFFF);
    return -1;
}

const char *board_desc(const la_cli_t *o, char *out, int len) {
    const machine_t *m = o->machine ? o->machine : &machine_zx48;
    int n = snprintf(out, len, "the simulated %s", m->board_name);
    for (int i = 0; i < o->nfaults && n < len; i++) n += snprintf(out + n, len - n, "%s%s", i ? " " : ", ", o->faults[i]);
    return out;
}


/* ------------------------------------------------------------------------ */
/* --la-diagnose: the boot stage by stage, and what's wrong                  */
/* ------------------------------------------------------------------------ */

/* The stages in order, and how much to capture of each (the analyser's
 * samples at 14 MHz): before the RAM test's end, enough to see the byte
 * it failed on. */
const diag_plan_t diag_plan[] = {
    { "reset", 1000, 97000 }, { "ramcheck", 1000, 97000 }, { "ramdone", 9000, 89000 },
    { "copyright", 1000, 97000 }, { "main", 1000, 97000 }, { "interrupt", 1000, 97000 },
};
