/* lacap.c - logic analyser captures of the ZX Spectrum's edge connector.
 * This file: the output (the window or the terminal, and the log), the
 * board's options, the analysers' order, and the command line. The rest:
 *   lacap_file.c      the signals, reading and writing captures (.lac, CSV)
 *   lacap_record.c    recording a simulated board; the boot's timeline
 *   lacap_compare.c   Z80 bus cycles; comparing two captures
 *   lacap_capture.c   the boot's stages; capturing them from the analyser
 *   lacap_rules.c     the diagnosis: what each capture shows
 *   lacap_verdict.c   the diagnosis: what to look at, in words
 *   lacap_diagnose.c  the diagnosis stage by stage; the screen; the check
 * lacap_int.h is what they share; lacap.h is for the rest of the program. */
#include "lacap_int.h"

/* Everything printed goes to the --la-log file as well. */
FILE *la_logf;
bool la_screen = true;           /* false: to the log alone */
void (*la_text_hook)(const char *text);
bool (*la_ask_hook)(const char *prompt);
void (*la_status_hook)(int kind, int stage, int state);
void *la_alloc_fail(size_t n) {
    fprintf(stderr, "c64-hw-sim: out of memory (wanted %zu bytes for the logic analyser)\n", n);
    exit(3);
}
void la_status(int kind, int stage, int state) {
    if (la_status_hook) la_status_hook(kind, stage, state);
}
/* What the diagnosis printed (for the parts its verdict names): kept
 * while VTEXT_ON. */
char *vtext;
size_t vtext_n, vtext_cap;
bool vtext_on;
static void vtext_add(const char *fmt, va_list a) {
    char buf[8192];
    int n = vsnprintf(buf, sizeof(buf), fmt, a);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    if (vtext_n + (size_t)n + 1 > vtext_cap) {
        size_t cap = (vtext_cap ? vtext_cap * 2 : 16384) + (size_t)n;
        char *p = xrealloc(vtext, cap);
        if (!p) return;
        vtext = p;
        vtext_cap = cap;
    }
    memcpy(vtext + vtext_n, buf, (size_t)n + 1);
    vtext_n += (size_t)n;
}
int la_out(const char *fmt, ...) {
    va_list a;
    int n = 0;
    if (vtext_on && la_screen) {
        va_start(a, fmt);
        vtext_add(fmt, a);
        va_end(a);
    }
    va_start(a, fmt);
    if (la_screen && la_text_hook) {
        char buf[8192];
        n = vsnprintf(buf, sizeof(buf), fmt, a);
        la_text_hook(buf);
    } else if (la_screen) n = vprintf(fmt, a);
    va_end(a);
    if (la_logf) {
        va_start(a, fmt);
        vfprintf(la_logf, fmt, a);
        va_end(a);
        fflush(la_logf);
    }
    fflush(stdout);
    return n;
}
/* What lapico.c prints as it captures: "Armed..." and "Triggered..." told
 * to the window as well (pico_quiet: told, not printed). */
static void pico_note(const char *t) {
    if (!strncmp(t, "Armed", 5)) la_status(LA_EV_ARMED, 0, 0);
    else if (!strncmp(t, "Triggered", 9)) la_status(LA_EV_TRIGGERED, 0, 0);
}
int pico_out(const char *fmt, ...) {
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    pico_note(buf);
    return la_out("%s", buf);
}
int pico_quiet(const char *fmt, ...) {
    char buf[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    pico_note(buf);
    return 0;
}
void la_err(const char *msg) {
    if (la_text_hook) { la_text_hook(msg); la_text_hook("\n"); }
    else fprintf(stderr, "%s\n", msg);
    if (la_logf) { fprintf(la_logf, "%s\n", msg); fflush(la_logf); }
}

/* Wait for the user (Enter): true to go on, false for no / stopped. PROMPT
 * is what was asked (printed already), or NULL. */
bool la_wait(const char *prompt) {
    if (la_ask_hook) return la_ask_hook(prompt);
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) return false;
    return line[0] != 'n' && line[0] != 'N';
}

/* ------------------------------------------------------------------------ */
/* Capturing from the analyser itself                                        */
/* ------------------------------------------------------------------------ */

static void list_devices(void) {
    lp_port_t p[LP_MAXDEV * 2];
    int n = lp_find(p, LP_MAXDEV * 2);
    if (!n) { printf("No LogicAnalyzer boards found (USB 1209:3020)\n"); return; }
    for (int i = 0; i < n; i++) {
        lp_info_t info;
        char err[256];
        printf("%-8s serial %-18s ", p[i].port, p[i].serial[0] ? p[i].serial : "?");
        if (lp_info(p[i].port, &info, err, sizeof(err)))
            printf("%s, %d channels, up to %d Hz, %d byte buffer, %s\n", info.version, info.channels, info.max_freq, info.buffer,
                   info.zx ? "zx firmware (no glitch triggers)" : "stock firmware");
        else
            printf("%s\n", err);
    }
}

/* The board's options (Controls: RAM 16K, chips fitted...), "ID=VALUE" as
 * --option takes them: every simulated board the diagnosis makes has them,
 * so a 16K board is compared with a working 16K one. */
static char la_opt[8][64];
static int la_nopt;

void la_set_options(const char *const *opts, int n) {
    la_nopt = 0;
    for (int i = 0; i < n && la_nopt < 8; i++)
        if (opts[i] && opts[i][0]) snprintf(la_opt[la_nopt++], sizeof(la_opt[0]), "%s", opts[i]);
}

static bool la_apply_option(board_t *b, const char *spec) {
    const machine_t *m = b->m;
    size_t n = strcspn(spec, "=");
    for (int o = 0; spec[n] && o < m->noptions; o++) {
        if (strlen(m->options[o].id) != n || strncmp(m->options[o].id, spec, n)) continue;
        for (int v = 0; v < m->options[o].nvalues; v++)
            if (!strcasecmp(m->options[o].value_ids[v], spec + n + 1)) {
                if (m->get_option(b, o) != v) m->set_option(b, o, v);
                return true;
            }
    }
    return false;
}

board_t *la_new_board(const machine_t *m) {
    board_t *b = board_create(m);
    for (int i = 0; b && i < la_nopt; i++) la_apply_option(b, la_opt[i]);
    return b;
}

/* "(RAM 16K)": the options not at their default, for the header. */
void options_text(const machine_t *m, char *out, int len) {
    out[0] = 0;
    board_t *b = la_new_board(m);
    if (!b) return;
    int k = 0;
    for (int o = 0; o < m->noptions; o++) {
        int v = m->get_option(b, o);
        if (v > 0 && v < m->options[o].nvalues)
            k += snprintf(out + k, (size_t)(len - k), "%s%s %s", k ? ", " : " (", m->options[o].name, m->options[o].values[v]);
        if (k >= len - 2) break;
    }
    if (k) snprintf(out + k, (size_t)(len - k), ")");
    board_destroy(b);
}

/* Which board is the master, by its USB serial number (the COM ports
 * change from PC to PC): la-boards.txt beside the program. */
static void boards_file(char *out, int len) {
    char dir[1024];
    if (filedialog_exe_dir(dir, sizeof(dir))) snprintf(out, (size_t)len, "%s/la-boards.txt", dir);
    else snprintf(out, (size_t)len, "la-boards.txt");
}

int la_boards(lp_port_t *p, int max, bool *known) {
    int n = 0;
    const char *fake = getenv("LA_FAKE_BOARDS");    /* for tests: "COM4=SERIAL,COM5=SERIAL" */
    if (fake && *fake) {
        for (const char *s = fake; *s && n < max; ) {
            size_t len = strcspn(s, ",");
            char w[130];
            snprintf(w, sizeof(w), "%.*s", (int)(len < 129 ? len : 129), s);
            char *eq = strchr(w, '=');
            if (eq) *eq = 0;
            snprintf(p[n].port, sizeof(p[n].port), "%.63s", w);
            snprintf(p[n].serial, sizeof(p[n].serial), "%.63s", eq ? eq + 1 : "");
            n++;
            s += len + (s[len] == ',');
        }
    } else n = lp_find(p, max);
    *known = false;
    char path[1100], line[200], master[64] = "";
    boards_file(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "master ", 7)) sscanf(line + 7, "%63s", master);
        fclose(f);
    }
    for (int i = 0; master[0] && i < n; i++)
        if (p[i].serial[0] && !strcasecmp(p[i].serial, master)) {
            lp_port_t t = p[0]; p[0] = p[i]; p[i] = t;
            *known = true;
            break;
        }
    return n;
}

bool la_boards_remember(const char *master, const char *slave) {
    if (!master || !master[0]) return false;
    char path[1100];
    boards_file(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# The logic analyser's boards, by USB serial number: the master is the one nearest\n"
               "# the Spectrum (channels 1-24). Written by c64-hw-sim (\"Swap\" in its window).\n"
               "master %s\nslave %s\n", master, slave && slave[0] ? slave : "?");
    fclose(f);
    return true;
}

/* --la-ports "A,B": each a port (COM12) or a board's USB serial number.
 * Not given: the boards found, the master first as remembered. */
int pick_ports(const char *list, char ports[][64], int max, char *err, int errlen) {
    lp_port_t p[LP_MAXDEV * 2];
    bool known = false;
    int found = la_boards(p, LP_MAXDEV * 2, &known);
    if (!list) {
        if (found == 1) { snprintf(ports[0], 64, "%s", p[0].port); return 1; }
        if (found > 1 && known) {
            int k = 0;
            for (; k < found && k < max; k++) snprintf(ports[k], 64, "%.63s", p[k].port);
            static char said[64];          /* once a run, not at every capture */
            if (strcmp(said, p[0].serial)) {
                printf("The boards: %s is the master (serial %s, as remembered in la-boards.txt)\n", p[0].port, p[0].serial);
                snprintf(said, sizeof(said), "%s", p[0].serial);
            }
            return k;
        }
        int n = snprintf(err, errlen, found ? "%d boards: say which is the master with --la-ports MASTER,OTHER (a port or a serial number),\n"
                         "or once with Swap in the logic analyser window (remembered in la-boards.txt):" :
                         "no LogicAnalyzer boards found", found);
        for (int i = 0; i < found && n < errlen; i++) n += snprintf(err + n, errlen - n, " %s=%s", p[i].port, p[i].serial);
        return 0;
    }
    int n = 0;
    for (const char *s = list; *s && n < max; ) {
        size_t len = strcspn(s, ",");
        char w[64];
        snprintf(w, sizeof(w), "%.*s", (int)len, s);
        snprintf(ports[n], 64, "%s", w);
        for (int i = 0; i < found; i++)
            if (!strcasecmp(p[i].serial, w)) snprintf(ports[n], 64, "%.63s", p[i].port);
        n++;
        s += len + (s[len] == ',');
    }
    return n;
}

/* The stages of a 48K's boot to trigger at, from the simulated Issue 2's
 * timeline (--la-timeline): after the reset button is let go, ms after it.
 * The address is first on the bus at the stage; but as the lines change
 * they show other addresses for a few ns, and the stock firmware's trigger
 * fires on those (the RAM test's writes can fake almost any address):
 * these need the zx firmware (la-firmware), whose trigger wants 75 ns. */
const la_stage_t la_stages[] = {
    { "reset",     0xFFFF,    0.031, "the reset: the RAM test's first write (FFFF), about 110 clock cycles after it" },
    { "ramcheck",  0x11E2,  449.419, "the RAM test starts reading the RAM back, from 4000 up (11E2, 0.45 s after the reset)" },
    { "ramdone",   0x11EF, 1650.562, "the RAM test is over (11EF, 1.65 s after the reset; sooner if the RAM has a fault)" },
    { "copyright", 0x1539, 1714.319, "the copyright message (1539, 1.71 s after the reset)" },
    { "main",      0x12A9, 1728.527, "the editor's main loop starts (12A9, 1.73 s after the reset)" },
    { "interrupt", 0x0038,       -1, "a frame interrupt (50 a second, while the ROM's interrupt routine is used)" },
    { NULL, 0, 0, NULL }
};

int la_cli(const la_cli_t *o) {
    char err[512];
    if (o->bootsel) {                   /* --la-bootsel [--la-ports P1,P2] */
        char ports[LP_MAXDEV][64];
        int n = pick_ports(o->ports, ports, LP_MAXDEV, err, sizeof(err));
        if (n <= 0) { la_err(err); return 1; }
        int rc = 0;
        for (int i = 0; i < n; i++) {
            if (lp_bootsel(ports[i], err, sizeof(err))) printf("%s: restarted into USB drive mode (a drive named RP2350)\n", ports[i]);
            else { la_err(err); rc = 1; }
        }
        return rc;
    }
    if (o->log && !(la_logf = fopen(o->log, "w"))) fprintf(stderr, "can't write %s\n", o->log);
    if (o->map && !la_load_map(o->map, err, sizeof(err))) { la_err(err); return 1; }
    int count = o->count > 0 ? o->count : 20;
    la_capture_t c;
    if (o->devices) { list_devices(); return 0; }
    if (o->diagnose) return diagnose(o);
    if (o->timeline) return timeline(o);
    if (o->capture) {
        if (!capture_real(o, &c, err, sizeof(err))) { la_err(err); return 1; }
        if (!la_write(o->capture, &c, err, sizeof(err))) { la_err(err); la_free(&c); return 1; }
        printf("Saved %s\n\n", o->capture);
        print_info(o->capture, &c, count);
        int rc = 0;
        if (o->machine) {               /* --board: compare with the simulated one */
            la_capture_t b;
            char bn[300];
            printf("\n");
            if (!record_board(o, sim_length(&c), c.trig_addr, &b, err, sizeof(err))) { la_err(err); la_free(&c); return 1; }
            rc = compare(o->capture, &c, board_desc(o, bn, sizeof(bn)), &b, count < 8 ? count : 8, o->refresh, NULL);
            la_free(&b);
        }
        la_free(&c);
        return rc;
    }
    if (o->record) {
        if (!record_board(o, 0, o->trigger ? trigger_addr_of(o->trigger) : -1, &c, err, sizeof(err))) { la_err(err); return 1; }
        if (!la_write(o->record, &c, err, sizeof(err))) { la_err(err); la_free(&c); return 1; }
        char d[300];
        char when[32] = "/RESET went high";
        if (c.trig_addr >= 0) snprintf(when, sizeof(when), "the trigger at %04X", c.trig_addr);
        printf("%s: %s, %d samples (%d before %s) at %.0f MHz\n", o->record,
               board_desc(o, d, sizeof(d)), c.n, c.pre, when, c.freq / 1e6);
        la_free(&c);
        return 0;
    }
    if (o->info || o->cycles) {
        const char *path = o->info ? o->info : o->cycles;
        if (!la_read(path, &c, err, sizeof(err))) { fprintf(stderr, "%s: %s\n", path, err); return 1; }
        if (o->info) print_info(path, &c, count);
        else {
            la_cycle_t *cy;
            int n = la_cycles(&c, &cy);
            for (int k = 0; k < n && (o->count <= 0 || k < o->count); k++) {
                char tx[32];
                cycle_text(&cy[k], tx, sizeof(tx));
                printf("%6d %8d %s%s\n", cy[k].t, cy[k].sample, tx, rom_wrong(&c, &cy[k]) ? "  <- not the ROM's" : "");
            }
            free(cy);
        }
        la_free(&c);
        return 0;
    }
    if (o->compare) {
        la_capture_t b;
        if (!la_read(o->compare, &c, err, sizeof(err))) { fprintf(stderr, "%s: %s\n", o->compare, err); return 1; }
        char bn[300];
        bool ok = o->against ? la_read(o->against, &b, err, sizeof(err)) : record_board(o, sim_length(&c), c.trig_addr, &b, err, sizeof(err));
        if (!ok) { fprintf(stderr, "%s: %s\n", o->against ? o->against : "recording", err); la_free(&c); return 1; }
        int rc = compare(o->compare, &c, o->against ? o->against : board_desc(o, bn, sizeof(bn)), &b,
                         o->count > 0 ? o->count : 8, o->refresh, NULL);
        la_free(&c); la_free(&b);
        return rc;
    }
    return 0;
}
