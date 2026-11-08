/* lacap_diagnose.c - the diagnosis stage by stage, the screen with it, and the check of the screen's faults.
 *
 * AI-generated (Claude Code). */
#include "lacap_int.h"
/* ---- The screen (--la-screen): a picture of the monitor, matched against
 * the fault atlas (atlas.h), with what the bus showed. ---- */

/* Whether the D0-D7 at LINE[I] (to J) is a data bit, not a diode: it
 * follows "bit", "bits", "line(s)", "pin(s)" or "gave", perhaps after
 * other bits ("bits D1 D2 D3", "pins D0-D4", "bit D7"). */
static bool data_bit_word(const char *line, size_t i, size_t j) {
    if (j - i != 2 || line[i] != 'D' || line[i + 1] < '0' || line[i + 1] > '7') return false;
    size_t k = i;
    for (;;) {
        while (k > 0 && (line[k - 1] == ' ' || line[k - 1] == ',' || line[k - 1] == '-')) k--;
        size_t e = k;
        while (k > 0 && isalnum((unsigned char)line[k - 1])) k--;
        if (k == e) return false;
        size_t n = e - k;
        if (n == 2 && line[k] == 'D' && line[k + 1] >= '0' && line[k + 1] <= '7') continue;
        static const char *words[] = { "bit", "bits", "line", "lines", "pin", "pins", "gave" };
        for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); w++)
            if (strlen(words[w]) == n && !strncmp(line + k, words[w], n)) return true;
        return false;
    }
}

static int part_index(board_t *b, const char *ref) {
    for (int i = 0; b && i < b->sim.nparts; i++)
        if (!strcmp(b->sim.parts[i].ref, ref)) return i;
    return -1;
}

static const char *const parts_heads[] = { "What to look at", "From the screen, what to look at",
                                           "From the check, what to look at" };

int la_text_parts(const struct machine *m, const char *text, char (*ref)[16], int *level, int max) {
    board_t *b = la_new_board(m);
    if (!b) return 0;
    int n = 0, sec = 0, first_sec = -1, item = 0, kind = 0;
    bool in = false, note = false, taken = false;
    for (const char *line = text; line && *line; ) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        int head = -1;
        for (int h = 0; h < 3; h++) if (!strncmp(line, parts_heads[h], strlen(parts_heads[h]))) head = h;
        int num;
        if (head >= 0) { in = true; sec++; item = 0; note = false; kind = head; }
        else if (in) {
            if (sscanf(line, "  %d.", &num) == 1) {
                item = num;
                note = false;
                taken = false;
                if (item == 1 && first_sec < 0) first_sec = sec;
            } else if (!strncmp(line, "  Note:", 7)) note = true;
            else if (len && line[0] != ' ') in = false;
            /* The check's items: the fault's own part only (its description
             * names nets and the parts on them). */
            if (in && item && !note && !(kind == 2 && taken))
                for (size_t i = 0; i < len; ) {
                    if (isupper((unsigned char)line[i]) && (i == 0 || !isalnum((unsigned char)line[i - 1]))) {
                        size_t j = i;
                        while (j < len && isupper((unsigned char)line[j])) j++;
                        size_t d = j;
                        while (d < len && isdigit((unsigned char)line[d])) d++;
                        if (d > j && d - i < 12 && (d >= len || !isalnum((unsigned char)line[d])) && !data_bit_word(line, i, d)) {
                            char r[16];
                            snprintf(r, sizeof(r), "%.*s", (int)(d - i), line + i);
                            if (part_index(b, r) >= 0 && !(kind == 2 && taken)) {
                                taken = true;
                                int lev = sec == first_sec && item == 1 ? 2 : 1, k;
                                for (k = 0; k < n && strcmp(ref[k], r); k++) {}
                                if (k < n) { if (lev > level[k]) level[k] = lev; }
                                else if (n < max) { snprintf(ref[n], 16, "%s", r); level[n++] = lev; }
                            }
                        }
                        i = d > i ? d : i + 1;
                    } else i++;
                }
        }
        if (!end) break;
        line = end + 1;
    }
    board_destroy(b);
    return n;
}
static void spec_parts(const atlas_cause_t *c, const char *ula, spart_t *p, int *np, int max, int order) {
    const char *s = c->spec;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        size_t k = strcspn(s, ".: ");
        char r[16];
        snprintf(r, sizeof(r), "%.*s", (int)(k < 15 ? k : 15), s);
        if (c->suspect) snprintf(r, sizeof(r), "%s", ula ? ula : "ULA");
        int i;
        for (i = 0; i < *np && strcmp(p[i].ref, r); i++) {}
        if (i < *np) p[i].n++;
        else if (*np < max) { snprintf(p[*np].ref, sizeof(p[0].ref), "%s", r); p[*np].n = 1; p[*np].first = order; (*np)++; }
        if (c->suspect) break;
        s += strcspn(s, " ");
    }
}

/* Faults the bus would most likely have shown, the boot being right: on the
 * CPU, the ROM or a RAM chip (the ROM's RAM test reads all of it). */
static bool bus_covers(const char *ref) {
    const char *cpu = ref_of("CPU"), *rom = ref_of("ROM");
    if ((cpu && !strcmp(ref, cpu)) || (rom && !strcmp(ref, rom))) return true;
    int i = part_index(vboard, ref);
    const char *l = i >= 0 ? board_part_label(vboard, i) : NULL;
    return l && (!strncmp(l, "LRAM", 4) || !strncmp(l, "URAM", 4));
}

/* The screen's part of the verdict: what the picture O->SCREEN matches,
 * against what the bus named (BUS_TEXT: what was printed, its "What to
 * look at"), BUS_OK when it found nothing wrong. 1: the screen is wrong
 * (it names suspects), 0: a working board's picture, -1: no use. */
static int screen_part(const la_cli_t *o, const machine_t *m, const char *bus_text, bool bus_ok) {
    const char *img = o->screen, *nm = img;
    for (const char *p = img; *p; p++) if (*p == '/' || *p == '\\') nm = p + 1;
    int am = atlas_machine_of(m);
    atlas_match_opts_t mo = { .path = o->atlas ? o->atlas : "fault_atlas.bin", .image = img, .top = ATLAS_MAX_GROUPS,
                              .causes = -1, .machine = am, .cart = 0, .carts = o->carts ? o->carts : "carts" };
    atlas_result_t r;
    memset(&r, 0, sizeof(r));
    bool ok = am >= 0 && atlas_match_run(&mo, &r);
    if ((!ok || !r.ngroups) && am >= 0 && atlas_family(am) != am) {    /* a board the atlas has no pictures of */
        atlas_result_free(&r);
        memset(&r, 0, sizeof(r));
        mo.machine = atlas_family(am);
        ok = atlas_match_run(&mo, &r);
    }
    printf("\nThe screen (%s): ", nm);
    if (!ok || !r.ngroups) {
        printf("can't use it: %s\n", r.error[0] ? r.error : "nothing in the atlas is like it");
        atlas_result_free(&r);
        return -1;
    }
    /* The groups as alike as the best (one picture, several ways to it). */
    float best = r.groups[0].score;
    bool healthy = r.groups[0].healthy;
    int nf = 0, np = 0;
    spart_t sp[96];
    vboard = la_new_board(m);
    const char *ula = ref_of("ULA");
    for (int g = 0; g < r.ngroups; g++) {
        const atlas_group_t *gr = &r.groups[g];
        if (gr->score < best - 0.03f || gr->healthy) continue;
        for (int c = 0; c < gr->ncauses; c++) {
            if (!gr->causes[c].spec[0]) continue;
            spec_parts(&gr->causes[c], ula, sp, &np, 96, nf++);
        }
    }
    if (r.seen[0]) printf("it shows %c%s.\n  The nearest simulated screen", tolower((unsigned char)r.seen[0]), r.seen + 1);
    else printf("the nearest simulated screen");
    printf(" (%.0f%% alike%s): \"%s\"\n", best * 100,
           best < 0.8f ? ", a rough match: take it as a hint" : "", r.groups[0].label);
    int rc = 1;
    if (healthy || !np) {
        printf("  A working board shows that too: the screen tells nothing more here.\n");
        rc = 0;
        goto out;
    }
    /* Most faults first; the bus being right, the parts it would have
     * shown last. */
    for (int i = 1; i < np; i++)
        for (int j = i; j > 0; j--) {
            bool cj = bus_ok && bus_covers(sp[j].ref), ci = bus_ok && bus_covers(sp[j - 1].ref);
            bool before = cj != ci ? !cj : sp[j].n != sp[j - 1].n ? sp[j].n > sp[j - 1].n : sp[j].first < sp[j - 1].first;
            if (!before) break;
            spart_t t = sp[j]; sp[j] = sp[j - 1]; sp[j - 1] = t;
        }
    printf("  In the atlas, that picture comes from %d fault%s on %d part%s.\n", nf, nf == 1 ? "" : "s", np, np == 1 ? "" : "s");
    char bref[32][16];
    int blev[32], nb = bus_ok || !bus_text ? 0 : la_text_parts(m, bus_text, bref, blev, 32);
    if (nb) {
        char agree[200] = "", named[200] = "";
        int na = 0, a = 0, b = 0;
        for (int k = 0; k < nb; k++) {
            b += snprintf(named + b, sizeof(named) - b > 0 ? sizeof(named) - b : 0, "%s%s", k ? ", " : "", bref[k]);
            for (int i = 0; i < np; i++)
                if (!strcmp(sp[i].ref, bref[k])) {
                    a += snprintf(agree + a, sizeof(agree) - a > 0 ? sizeof(agree) - a : 0, "%s%s", na ? ", " : "", bref[k]);
                    na++;
                    break;
                }
        }
        if (na)
            printf("  It agrees with the bus: %s %s among them (a fault there gives this picture as well).\n", agree,
                   na == 1 ? "is" : "are");
        else {
            printf("  None of them is what the bus names (%s). The bus is the more direct of the two; both together\n"
                   "  can be two faults, or a fault the atlas doesn't have.\n", named);
            printf("From the screen, what to look at as well:\n");
            for (int i = 0; i < np && i < 4; i++) {
                int pi = part_index(vboard, sp[i].ref);
                const char *l = pi >= 0 ? board_part_label(vboard, pi) : NULL;
                printf("  %d. %s%s%s%s: %d of those faults\n", i + 1, sp[i].ref, l ? " (" : "", l ? l : "", l ? ")" : "", sp[i].n);
            }
        }
        goto out;
    }
    printf("From the screen, what to look at, most likely first%s:\n", bus_ok ? "" : " (the bus names no part)");
    int shown = 0, ncov = 0;
    for (int i = 0; i < np && shown < 6; i++) {
        if (bus_ok && bus_covers(sp[i].ref)) { ncov++; continue; }
        int pi = part_index(vboard, sp[i].ref);
        const char *l = pi >= 0 ? board_part_label(vboard, pi) : NULL;
        bool inside = ula && !strcmp(sp[i].ref, ula);
        printf("  %d. %s%s%s%s: %d of those faults%s\n", ++shown, sp[i].ref, l ? " (" : "", l ? l : "", l ? ")" : "", sp[i].n,
               inside ? " (at its pins; and inside it, which the atlas can't show)" : "");
    }
    if (!shown) printf("  None but the parts below.\n");
    if (bus_ok) {
        char cov[400];
        int c = 0;
        for (int i = 0; i < np; i++) {
            if (!bus_covers(sp[i].ref)) continue;
            int pi = part_index(vboard, sp[i].ref);
            const char *l = pi >= 0 ? board_part_label(vboard, pi) : NULL;
            if (c < (int)sizeof(cov))
                c += snprintf(cov + c, sizeof(cov) - c, "%s%s%s%s%s", c ? ", " : "", sp[i].ref, l ? " (" : "", l ? l : "", l ? ")" : "");
            ncov++;
        }
        if (c)
            printf("  Note: less likely, as the bus is right and would most likely have shown a fault on them: %s.\n", cov);
    } else if (np > shown) printf("  ... and %d more part%s\n", np - shown, np - shown == 1 ? "" : "s");
    (void)ncov;
out:
    board_destroy(vboard);
    vboard = NULL;
    atlas_result_free(&r);
    return rc;
}

int la_screen_again(const la_cli_t *o, const char *bus_text, bool bus_ok) {
    if (!o->screen || !o->machine) return -1;
    return screen_part(o, o->machine, bus_text, bus_ok);
}

static void multi_step(board_t *b, void *ctx) {
    rec_t *r = ctx;
    for (int s = 0; r[s].pre >= 0; s++) rec_step(b, &r[s]);
}

/* The board with SPEC ("" none) recorded for all N stages at once, as
 * record_board() does each (from power on, each on its trigger): one boot
 * for all, as the simulation goes the same way every time. GOT[s]: stage s
 * came. False if SPEC can't be put on the board. */
static bool record_stages(const machine_t *m, const char *spec, const la_capture_t *d, int n, la_capture_t *out, bool *got) {
    board_t *b = la_new_board(m);
    if (spec[0] && !board_apply_fault_spec(b, spec)) { board_destroy(b); return false; }
    rec_t r[DIAG_NSTAGES + 1];
    memset(r, 0, sizeof(r));
    long long limit_ms = 0;
    for (int s = 0; s < n; s++) {
        int trig = d[s].trig_addr, pre = trig >= 0 ? 2000 : 512, post = sim_length(&d[s]);
        if (post <= 0) post = 32000;
        int arm = trig == 0x0038 ? 2000 : 0, max_ms = trig >= 0 ? arm + 4000 : 1000;
        if (max_ms > limit_ms) limit_ms = max_ms;
        r[s] = (rec_t){ .pre = pre, .post = post, .trig_addr = trig,
                        .arm_step = (long long)arm * (ZX_FREQ * ZX_STEPS_PER_CYCLE / 1000) };
        memset(&out[s], 0, sizeof(out[s]));
        find_nets(b, r[s].net, &out[s]);
        r[s].ring = xcalloc((size_t)pre, sizeof(uint64_t));
        r[s].out = xcalloc((size_t)pre + (size_t)post, sizeof(uint64_t));
    }
    r[n].pre = -1;
    bool hooked = board_step_hook(b, multi_step, r);
    long long limit = (long long)ZX_FREQ * limit_ms / 1000;
    for (long long t = 0; hooked && t < limit; t += 1000) {
        bool all = true;
        for (int s = 0; s < n && all; s++) all = r[s].done;
        if (all) break;
        board_run(b, 1000);
    }
    if (hooked) board_step_hook(b, NULL, NULL);
    board_destroy(b);
    for (int s = 0; s < n; s++) {
        free(r[s].ring);
        got[s] = r[s].triggered && r[s].done;
        if (!got[s]) { free(r[s].out); out[s].s = NULL; continue; }
        out[s].s = r[s].out;
        out[s].n = r[s].nout;
        out[s].pre = r[s].rcount;
        out[s].trig_addr = r[s].trig_addr;
        out[s].freq = ZX_FREQ * ZX_STEPS_PER_CYCLE;
    }
    return true;
}

static void *check_thread(void *arg) {
    check_t *k = arg;
    for (;;) {
        pthread_mutex_lock(&k->mu);
        int j = k->next++;
        pthread_mutex_unlock(&k->mu);
        if (j >= k->nc) break;
        cand_t *c = &k->c[j];
        la_capture_t r[DIAG_NSTAGES];
        bool got[DIAG_NSTAGES];
        bool ok = record_stages(k->m, c->spec, k->d, k->nst, r, got);
        pthread_mutex_lock(&k->mu);
        if (!ok) c->bad = true;
        for (int s = 0; ok && s < k->nst; s++) {
            if (!got[s]) continue;      /* it never gets there: that stage differs */
            cmp_out_t co = { 0 };
            bool was = la_screen;
            la_screen = false;          /* the comparisons go to the log */
            printf("\n==== check: %s, %s\n", c->spec[0] ? c->spec : "a working board", k->path[s]);
            compare(k->path[s], &k->d[s], c->spec[0] ? c->spec : "a working board", &r[s], 8, false, &co);
            la_screen = was;
            if (co.rc == 0) c->same++;
            else c->cycles += co.cycle;
            la_free(&r[s]);
        }
        k->done++;
        la_status(LA_EV_PROGRESS, k->done, k->total);
        pthread_mutex_unlock(&k->mu);
    }
    return NULL;
}

static int check_threads(void) {
    const char *e = getenv("LA_JOBS");
    int n = e ? atoi(e) : (atlas_logical_processors() + 1) / 2;
    return n < 1 ? 1 : n > 64 ? 64 : n;
}

/* The faults the screen O->SCREEN matches (the likeliest of each group
 * first, then the next of each...), at most MAX; the ULA inside apart. */
static int screen_candidates(const la_cli_t *o, const machine_t *m, cand_t *c, int max) {
    int am = atlas_machine_of(m);
    atlas_match_opts_t mo = { .path = o->atlas ? o->atlas : "fault_atlas.bin", .image = o->screen, .top = ATLAS_MAX_GROUPS,
                              .causes = -1, .machine = am, .cart = 0, .carts = o->carts ? o->carts : "carts" };
    atlas_result_t r;
    memset(&r, 0, sizeof(r));
    bool ok = am >= 0 && atlas_match_run(&mo, &r);
    if ((!ok || !r.ngroups) && am >= 0 && atlas_family(am) != am) {
        atlas_result_free(&r);
        memset(&r, 0, sizeof(r));
        mo.machine = atlas_family(am);
        ok = atlas_match_run(&mo, &r);
    }
    int n = 0;
    if (ok && r.ngroups && !r.groups[0].healthy) {
        float best = r.groups[0].score;
        for (int depth = 0; n < max; depth++) {
            bool any = false;
            for (int g = 0; g < r.ngroups && n < max; g++) {
                const atlas_group_t *gr = &r.groups[g];
                if (gr->score < best - 0.03f || gr->healthy) continue;
                int seen = 0;
                for (int q = 0; q < gr->ncauses; q++) {
                    const atlas_cause_t *a = &gr->causes[q];
                    if (a->suspect || !a->spec[0]) continue;
                    if (seen++ != depth) continue;
                    any = true;
                    int dup = 0;
                    for (int i = 0; i < n && !dup; i++) dup = !strcmp(c[i].spec, a->spec);
                    if (dup) break;
                    memset(&c[n], 0, sizeof(c[n]));
                    snprintf(c[n].spec, sizeof(c[n].spec), "%s", a->spec);
                    const char *dd = a->desc;
                    size_t l = strcspn(dd, "\n");
                    snprintf(c[n].desc, sizeof(c[n].desc), "%.*s", (int)(l < 300 ? l : 300), dd);
                    n++;
                    break;
                }
            }
            if (!any) break;
        }
    }
    atlas_result_free(&r);
    return n;
}

/* With no screen: the faults of the parts the bus's verdict names (its
 * "What to look at", the first item's parts first), from the atlas: each
 * part's removal first, then its data bits, then what goes wrong inside it
 * (an address line, a flipped bit, weak outputs), then its pins (open,
 * shorted, stuck); a part at a time, the first item's parts two at a turn,
 * at most MAX. TEXT: the diagnosis (diagnosis.txt). */
static int bus_candidates(const la_cli_t *o, const machine_t *m, const char *text, cand_t *c, int max) {
    char ref[32][16];
    int level[32];
    /* Only the bus's own verdict: not a screen's or an earlier check's. */
    size_t tl = strlen(text);
    const char *cut = strstr(text, "\nThe screen (");
    const char *cut2 = strstr(text, "\nChecked against the captures:");
    if (cut2 && (!cut || cut2 < cut)) cut = cut2;
    char *own = xmalloc(tl + 1);
    size_t ol = cut ? (size_t)(cut - text) : tl;
    memcpy(own, text, ol);
    own[ol] = 0;
    int np = la_text_parts(m, own, ref, level, 32);
    free(own);
    if (!np) return 0;
    int order[32], no = 0;
    for (int lev = 2; lev >= 1; lev--)
        for (int i = 0; i < np; i++) if (level[i] == lev) order[no++] = i;
    atlas_t a;
    memset(&a, 0, sizeof(a));
    bool exists = false;
    int am = atlas_machine_of(m);
    if (am < 0 || !atlas_load(&a, o->atlas ? o->atlas : "fault_atlas.bin", &exists) || !a.nfaults) {
        atlas_free(&a);
        return 0;
    }
    /* Each part's faults, best first: the board's own atlas machine, else
     * one with the same parts. */
    int *fl[32], nf[32], lev[32];
    for (int k = 0; k < no; k++) {
        const char *r = ref[order[k]];
        size_t rl = strlen(r);
        fl[k] = xmalloc((size_t)a.nfaults * sizeof(int));
        nf[k] = 0;
        lev[k] = level[order[k]];
        for (int pass = 0; pass < 2 && !nf[k]; pass++)
            for (int rank = 0; rank < 4; rank++)
                for (int f = 0; f < a.nfaults; f++) {
                    const atlas_fault_t *x = &a.faults[f];
                    if (x->h.mode != 0 || !x->spec) continue;
                    if (pass == 0 ? x->h.machine != am : !atlas_same_refs(x->h.machine, am) || x->h.machine == am) continue;
                    if (strncmp(x->spec, r, rl) || !strchr(".:", x->spec[rl]) || strchr(x->spec, ' ')) continue;
                    int fr = strstr(x->spec, ":removed") ? 0 : strstr(x->spec, ":data") ? 1
                           : x->spec[rl] == ':' ? 2 : 3;      /* "IC6:addr2" inside; "IC6.3:open" a pin */
                    if (fr != rank) continue;
                    int dup = 0;
                    for (int i = 0; i < nf[k] && !dup; i++) dup = !strcmp(a.faults[fl[k][i]].spec, x->spec);
                    if (!dup) fl[k][nf[k]++] = f;
                }
    }
    int n = 0, used[32] = { 0 };
    for (;;) {
        bool any = false;
        for (int k = 0; k < no && n < max; k++)
            for (int t = 0; t < (lev[k] == 2 ? 2 : 1) && n < max && used[k] < nf[k]; t++) {
                any = true;
                const atlas_fault_t *x = &a.faults[fl[k][used[k]++]];
                int dup = 0;
                for (int i = 0; i < n && !dup; i++) dup = !strcmp(c[i].spec, x->spec);
                if (dup) continue;
                memset(&c[n], 0, sizeof(c[n]));
                snprintf(c[n].spec, sizeof(c[n].spec), "%s", x->spec);
                const char *dd = x->desc ? x->desc : "";
                size_t l = strcspn(dd, "\n");
                snprintf(c[n].desc, sizeof(c[n].desc), "%.*s", (int)(l < 300 ? l : 300), dd);
                n++;
            }
        if (!any || n >= max) break;
    }
    for (int k = 0; k < no; k++) free(fl[k]);
    atlas_free(&a);
    return n;
}

/* The folder's diagnosis.txt, whole (NULL if none). */
static char *folder_text(const char *dir) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/diagnosis.txt", dir);
    if (la_logf) fflush(la_logf);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    char *t = xmalloc((size_t)len + 1);
    size_t got = fread(t, 1, (size_t)len, f);
    fclose(f);
    t[got] = 0;
    return t;
}

static const char *folder_screen(const char *dir, char *out, int len);
int la_check(const la_cli_t *o) {
    const machine_t *m = o->machine ? o->machine : &machine_zx48;
    const char *dir = o->diagnose;
    char sp[1100];
    la_cli_t q = *o;
    if (!q.screen && dir) q.screen = folder_screen(dir, sp, sizeof(sp));
    o = &q;
    if (!dir) return -1;
    static cand_t c[CHECK_MAX + 1];
    int max = o->check > 0 && o->check < CHECK_MAX ? o->check : CHECK_MAX;
    /* c[0]: a working board, what the others are measured against. */
    memset(&c[0], 0, sizeof(c[0]));
    /* The screen's faults; with no screen, those of the parts the bus's
     * verdict names. */
    bool from_bus = !o->screen;
    int nc = 0;
    if (!from_bus) nc = screen_candidates(o, m, c + 1, max);
    else {
        char *t = folder_text(dir);
        if (t) nc = bus_candidates(o, m, t, c + 1, max);
        free(t);
    }
    const char *whose = from_bus ? "the verdict's parts" : "the screen";
    if (!nc) {
        if (from_bus)
            printf("\nChecked against the captures: the verdict names no parts with faults to check (or there's no\n"
                   "diagnosis.txt, or no atlas).\n");
        else
            printf("\nChecked against the captures: the screen has no faults to check (a working board's picture, or\n"
                   "none read).\n");
        return 0;
    }
    nc++;
    /* The stages the analyser captured (not those that never came). */
    la_capture_t d[DIAG_NSTAGES];
    static char paths[DIAG_NSTAGES][1100];     /* static for the size; one diagnosis runs at a time */
    const char *pp[DIAG_NSTAGES];
    int nst = 0;
    char err[300];
    for (int i = 0; i < DIAG_NSTAGES; i++) {
        const la_stage_t *st = stage_named(diag_plan[i].stage);
        snprintf(paths[nst], sizeof(paths[0]), "%s/%s.lac", dir, st->name);
        if (!la_read(paths[nst], &d[nst], err, sizeof(err))) continue;
        pp[nst] = paths[nst];
        nst++;
    }
    if (!nst) { printf("\nChecked against the captures: no captures in %s to check with.\n", dir); return 0; }
    int nt = check_threads();
    if (nt > nc) nt = nc;
    printf("\nChecked against the captures: %d fault%s of %s, each put on the simulated board and\n"
           "its %d stage%s compared with the analyser's (%d at a time).\n", nc - 1, nc == 2 ? "" : "s", whose, nst,
           nst == 1 ? "" : "s", nt);
    check_t k = { .m = m, .c = c, .nc = nc, .d = d, .path = pp, .nst = nst, .total = nc };
    pthread_mutex_init(&k.mu, NULL);
    pthread_t th[64];
    int started = 0;
    for (int i = 0; i < nt; i++) if (pthread_create(&th[started], NULL, check_thread, &k) == 0) started++;
    if (!started) check_thread(&k);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
    pthread_mutex_destroy(&k.mu);
    for (int i = 0; i < nst; i++) la_free(&d[i]);
    cand_t w = c[0];                    /* a working board */
    printf("  A working board: %d of %d stage%s the same as the captures", w.same, nst, nst == 1 ? "" : "s");
    if (w.same < nst) printf(", the others the same for %ld cycles", w.cycles);
    printf(".\n");
    /* Most stages the same first; then the most cycles the same where they differ. */
    for (int i = 2; i < nc; i++)
        for (int j = i; j > 1; j--) {
            cand_t *a = &c[j], *b = &c[j - 1];
            bool before = a->bad != b->bad ? !a->bad : a->same != b->same ? a->same > b->same : a->cycles > b->cycles;
            if (!before) break;
            cand_t t = *a; *a = *b; *b = t;
        }
    int better = 0;
    for (int i = 1; i < nc; i++)
        better += !c[i].bad && (c[i].same > w.same || (c[i].same == w.same && c[i].cycles > w.cycles));
    if (!better) {
        printf("  None of %s faults does more of what the board did than a working board: they don't\n"
               "  explain the captures.\n", from_bus ? "the verdict's parts'" : "the screen's");
        return 0;
    }
    printf("From the check, what to look at, most likely first:\n");
    int shown = 0;
    for (int i = 1; i < nc && shown < 5; i++) {
        if (c[i].bad || !(c[i].same > w.same || (c[i].same == w.same && c[i].cycles > w.cycles))) break;
        if (c[i].same < c[1].same - 1) break;     /* well behind the best */
        printf("  %d. %s: %d of %d stages the same", ++shown, c[i].spec, c[i].same, nst);
        if (c[i].same < nst) printf(", the others for %ld cycles", c[i].cycles);
        printf(" (%s)\n", c[i].desc);
    }
    if (c[1].same == nst)
        printf("  Note: the first does just what the board did in every stage the analyser took.\n");
    return 1;
}

/* A picture of the screen saved with the captures (the window's "screen.png"). */
static const char *folder_screen(const char *dir, char *out, int len) {
    static const char *const ext[] = { "png", "jpg", "jpeg", "bmp" };
    for (int i = 0; i < 4; i++) {
        snprintf(out, (size_t)len, "%s/screen.%s", dir, ext[i]);
        FILE *f = fopen(out, "rb");
        if (f) { fclose(f); return out; }
    }
    return NULL;
}

static int diagnose_run(const la_cli_t *o);
int diagnose(const la_cli_t *o) {
    bool own = !la_logf;
    la_cli_t q = *o;
    char sp[1100];
    if (!q.screen && q.replay) q.screen = folder_screen(q.diagnose, sp, sizeof(sp));
    vtext_n = 0;
    if (vtext) vtext[0] = 0;
    vtext_on = q.screen != NULL;
    int rc = diagnose_run(&q);
    vtext_on = false;
    /* With no screen, only after a fault (a working board's verdict names no parts). */
    if (q.check && (q.screen || rc == 2) && la_check(&q) > 0 && rc == 0) rc = 2;
    if (own && la_logf) { fclose(la_logf); la_logf = NULL; }
    return rc;
}
static int diagnose_run(const la_cli_t *o) {
    const machine_t *m = o->machine ? o->machine : &machine_zx48;
    const char *dir = o->diagnose;
    char path[1024], err[512], bn[300];
    mkdir1(dir);
    /* Captured or simulated afresh: an earlier run's files in the folder go
     * (one left over would be read as this run's by a replay). */
    if (!o->replay) {
        static const char *const idle_files[] = { "idle.lac", "idle-slow.lac", "diagnosis.txt" };
        for (int i = 0; i < 3; i++) {
            snprintf(path, sizeof(path), "%s/%s", dir, idle_files[i]);
            remove(path);
        }
        for (int i = 0; i < DIAG_NSTAGES; i++) {
            const la_stage_t *st = stage_named(diag_plan[i].stage);
            snprintf(path, sizeof(path), "%s/%s.lac", dir, st->name);
            remove(path);
            snprintf(path, sizeof(path), "%s/%s-instead.lac", dir, st->name);
            remove(path);
        }
    }
    if (!la_logf) {
        snprintf(path, sizeof(path), "%s/diagnosis.txt", dir);
        la_logf = fopen(path, "w");
    }
    const char *how = o->replay ? "from the captures in the folder" :
                      o->simulate ? "of the simulated board" : "with the logic analyser";
    char opts_txt[160];
    options_text(m, opts_txt, sizeof(opts_txt));
    printf("Diagnosing a %s%s, stage by stage, %s\n", m->board_name, opts_txt, how);
    if (o->simulate) {
        printf("The board under test: the simulated %s", m->board_name);
        for (int i = 0; i < o->nfaults; i++) printf("%s%s", i ? " " : ", with ", o->faults[i]);
        printf("\n");
    }
    if (!o->replay && !o->simulate)
        printf("Each stage after the reset is once a boot: you hold the Spectrum's reset button, press Enter here, and let go\n"
               "once it says Armed. The captures and a log go in %s\n", dir);
    la_cli_t ref = *o;
    ref.faults = NULL; ref.nfaults = 0; ref.capture = NULL; ref.pre = ref.post = 0; ref.machine = m;
    ev_t e;
    ev_init(&e);
    cmp_out_t co = { 0 };
    char where[200] = "";
    const char *fail_stage = NULL;
    bool missing = false;
    int passed = 0;
    ev_t ie;
    la_status(LA_EV_STAGE, 0, LA_ST_RUN);
    int idle = idle_stage(o, m, dir, &ie);
    la_status(LA_EV_STAGE, 0, idle == 0 ? LA_ST_OK : idle > 0 ? LA_ST_FAULT : LA_ST_SKIP);
    if (idle == 2)
        for (int i = 1; i <= DIAG_NSTAGES; i++) la_status(LA_EV_STAGE, i, LA_ST_SKIP);
    if (idle == 2) { e = ie; fail_stage = "idle"; snprintf(where, sizeof(where), "%s", ie.idle_where); }
    for (int i = 0; i < DIAG_NSTAGES && idle != 2; i++) {
        const la_stage_t *st = stage_named(diag_plan[i].stage);
        int pre = diag_plan[i].pre, post = diag_plan[i].post;
        bool boot = st->addr != 0x0038;
        la_capture_t d, r;
        bool got = false, instead = false;
        char ipath[1024];
        snprintf(path, sizeof(path), "%s/%s.lac", dir, st->name);
        snprintf(ipath, sizeof(ipath), "%s/%s-instead.lac", dir, st->name);
        printf("\n%d of %d, %s: %s\n", i + 1, DIAG_NSTAGES, stage_plain(st->name), st->what);
        la_status(LA_EV_STAGE, i + 1, LA_ST_RUN);
        double at = i == 0 ? 0 : st->ms + 5;    /* when it's past due */
        if (o->replay) {
            got = la_read(path, &d, err, sizeof(err));
            if (!got) instead = la_read(ipath, &d, err, sizeof(err));
            if (!got && !instead) {
                la_status(LA_EV_STAGE, i + 1, LA_ST_SKIP);
                if (fail_stage) printf("  Not in the folder (that run stopped at the first fault): the rest is from the stages before\n");
                else printf("  Not in the folder (%s): the diagnosis stops here\n", path);
                break;
            }
        } else if (o->simulate) {
            int arm = boot ? 0 : 2000;
            got = record_sim(m, o->faults, o->nfaults, pre * 2, post * 2, st->addr, arm, &d, err, sizeof(err));
            if (!got && i == 0) got = record_sim(m, o->faults, o->nfaults, pre * 2, post * 2, 0x0000, 0, &d, err, sizeof(err));
            if (!got) instead = record_sim(m, o->faults, o->nfaults, 2000, post * 2, -2, i == 0 ? 1 : (int)at, &d, err, sizeof(err));
            if (!got && !instead) { la_err(err); if (fail_stage) { printf(STOP_MSG); break; } return 1; }
        } else {
            la_cli_t q = *o;
            q.trigger = st->name; q.capture = path; q.pre = pre; q.post = post; q.at = 0;
            q.timeout = boot ? 15 : 5;
            q.diagnose = NULL;
            if (boot && !want_enter("Hold the Spectrum's reset button down, then press Enter here (keep holding the button)\n")) {
                if (fail_stage) break;
                return 1;
            }
            if (!got) got = capture_real(&q, &d, err, sizeof(err));
            if (!got && strstr(err, "no trigger") && boot &&
                want_enter("It didn't come. Once more? Hold the reset button, press Enter (or type n, Enter if you did let go and\n"
                           "it still didn't come)\n"))
                got = capture_real(&q, &d, err, sizeof(err));
            if (!got && !strstr(err, "no trigger") && !strstr(err, "no key")) { la_err(err); if (fail_stage) { printf(STOP_MSG); break; } return 1; }
            if (!got && i == 0) {
                printf("The reset's FFFF never showed: now for its first fetch, 0000\n");
                if (!want_enter("Hold the reset button down, press Enter, and let go once it says Armed\n")) { if (fail_stage) break; return 1; }
                q.trigger = "addr=0000";
                got = capture_real(&q, &d, err, sizeof(err));
                if (got) d.trig_addr = 0x0000;
            }
            if (!got) {
                if (i == 0) printf("Now what it's doing at all: let it run (switched on, reset button not held)\n");
                else printf("Now what it's doing when %s should have come: hold the reset button down, press Enter, and\n"
                            "let go once it says Armed\n", st->name);
                if (!want_enter(i == 0 ? "Press Enter\n" : "")) { if (fail_stage) break; return 1; }
                instead = capture_now(o, at, ipath, &d, err, sizeof(err));
                if (!instead) { la_err(err); if (fail_stage) { printf(STOP_MSG); break; } return 1; }
            }
            if (!la_write(got ? path : ipath, &d, err, sizeof(err))) la_err(err);
        }
        if ((o->simulate) && !la_write(got ? path : ipath, &d, err, sizeof(err))) la_err(err);
        /* A working board's, to compare with. */
        bool have_r = got ? record_board(&ref, sim_length(&d), d.trig_addr, &r, err, sizeof(err))
                          : record_sim(m, NULL, 0, 2000, 194000, -2, i == 0 ? 1 : (int)at, &r, err, sizeof(err));
        if (!have_r) { la_err(err); la_free(&d); return 1; }
        /* Taken while the button was still held (the lines float then)?
         * Once more, if it's the analyser. */
        if (got && boot && !o->replay && !o->simulate && (d.have >> LA_RESET & 1) && line_state(&d, LA_RESET) == 1 && !cpu_busy(&d) &&
            want_enter("/RESET was low all through: that was while the button was held. Once more: hold it, press Enter,\n"
                       "and let go as soon as it says Armed (or type n, Enter to go on with this one)\n")) {
            la_capture_t d2;
            la_cli_t q = *o;
            q.trigger = st->name; q.capture = path; q.pre = pre; q.post = post; q.at = 0; q.timeout = 15; q.diagnose = NULL;
            if (capture_real(&q, &d2, err, sizeof(err))) {
                la_free(&d);
                d = d2;
                if (!la_write(path, &d, err, sizeof(err))) la_err(err);
            }
        }
        ev_t se;
        ev_init(&se);
        cmp_out_t sco = { 0 };
        la_screen = false;
        printf("\n==== %s: %s\n", st->name, got ? path : ipath);
        if (got) compare(got ? path : ipath, &d, board_desc(&ref, bn, sizeof(bn)), &r, 8, o->refresh, &sco);
        gather(&se, &d, &r);
        if (instead) where_text(&d, where, sizeof(where));
        int clk_st = d.have >> LA_CLK & 1 ? line_state(&d, LA_CLK) : -1;
        la_screen = true;
        la_free(&d);
        la_free(&r);
        bool bad = instead || sco.rc == 2 || ev_any(&se);
        la_status(LA_EV_STAGE, i + 1, !bad ? LA_ST_OK : instead ? LA_ST_NEVER : LA_ST_FAULT);
        if (!bad) {
            if (!fail_stage) passed++;
            else ev_merge(&e, &se);     /* --la-all: what a later stage shows as well */
            printf("  OK: the same as a working one%s\n",
                   se.pramt == 0xFFFF ? " (the RAM test passed all 48K)" : se.pramt == 0x7FFF ? " (the RAM test passed 16K)" : "");
            continue;
        }
        if (instead && clk_st > 0) {
            printf("  CLK is stuck %s in it: the clock stopped\n", clk_st == 1 ? "low" : "high");
            if (fail_stage && !e.stuck[LA_CLK]) e.clk_stopped = clk_st;
        }
        if (fail_stage) {               /* --la-all: a later stage that's wrong too */
            if (!instead) ev_merge(&e, &se);
            if (instead) printf("  It never came either. When it should have, the CPU was %s\n", where);
            else if (sco.diverged) printf("  Wrong too: the CPU went another way %s\n", went_when(sco.cycle));
            else printf("  Wrong too: it got there, but the bus shows something wrong\n");
            if (instead) break;
            continue;
        }
        e = se;
        co = sco;
        fail_stage = st->name;
        missing = instead;
        if (instead) printf("  It never came. When it should have, the CPU was %s\n", where);
        else if (sco.diverged) printf("  FAULT: the CPU went another way %s\n", went_when(sco.cycle));
        else printf("  FAULT: it got there, but the bus shows something wrong\n");
        /* --la-all: on to the rest (not after a stage that never came:
         * the later ones won't either). */
        if (!o->all || instead) break;
        printf("  (--la-all: capturing the rest as well)\n");
    }
    printf("\n");
    la_status(LA_EV_VERDICT, 0, 0);
    if (idle == 1) {
        if (!fail_stage) { e = ie; fail_stage = "idle"; snprintf(where, sizeof(where), "%s", ie.idle_where); }
        else ev_merge(&e, &ie);     /* not its stuck lines: left running, a faulty board's in other code */
    }
    if (!fail_stage) {
        if (idle == 0) printf("Left running before the boot, it does as a working one: the clock, /RESET and the frame interrupt.\n");
        if (passed == DIAG_NSTAGES) {
            vboard = la_new_board(m);
            const char *u = ref_of("ULA");
            char ula[32];
            snprintf(ula, sizeof(ula), "the ULA (%s)", u ? u : "IC1");
            board_destroy(vboard);
            vboard = NULL;
            char bname[240], otx[160];
            options_text(m, otx, sizeof(otx));
            snprintf(bname, sizeof(bname), "%s%s", m->board_name, otx);
            printf("All %d stages are the same as a working %s: the CPU, the ROM and the start of BASIC are good, and\n"
                   "the RAM passes the ROM's own test.\n", passed, bname);
            /* The same at every pass: the window shows it behind "More". */
            la_status(LA_EV_NOTE, 1, 0);
            printf("That test is weak: it writes only 02, 01 and 00 to each byte, so bits D2-D7 never hold a 1, and a\n"
                   "RAM chip for one of them that can't hold a 1 (or fails only on some patterns) passes it, and the\n"
                   "machine can boot. A full RAM test (ZX Diagnostics) finds those.\n");
            if (!o->screen)
                printf("This doesn't test the picture, the keyboard, the sound or the tape.\n"
                       "If the picture is wrong all the same (the copyright message garbled, wrong colours): the CPU wrote\n"
                       "the screen right and the RAM keeps it, so it's the ULA's video side: %s reads the screen over its own\n"
                       "address and data lines to the lower RAM, which the edge connector doesn't show. Diagnose a\n"
                       "screenshot of it: the atlas has the ULA's outputs shorted and its pins bridged, though not every\n"
                       "fault inside the chip.\n", ula);
            la_status(LA_EV_NOTE, 0, 0);
            if (o->screen)
                printf("A wrong picture is then the ULA's video side: %s reads the screen\n"
                       "over its own lines to the lower RAM, which the edge connector doesn't show.\n", ula);
        }
        if (o->screen && screen_part(o, m, vtext, true) > 0) return 2;
        return 0;
    }
    if (!strcmp(fail_stage, "idle") && idle == 2)
        printf("The boot's stages weren't taken: they need a running CPU, and left running this one isn't.\n");
    else if (!strcmp(fail_stage, "idle"))
        printf("The boot's %d stages are the same as a working one's, but left running it isn't.\n", passed);
    else
        printf("The boot goes wrong at %s (%d stage%s before it %s good).\n", stage_plain(fail_stage), passed, passed == 1 ? "" : "s",
               passed == 1 ? "is" : "are");
    verdict(&e, m, fail_stage, missing, &co, where);
    la_status(LA_EV_FOLDER, 0, 0);
    printf("\nThe captures and the whole comparison: %s (diagnosis.txt)\n", dir);
    if (o->screen) screen_part(o, m, vtext, false);
    return 2;
}
