/* lacap.c - logic analyser captures of the ZX Spectrum's edge connector:
 * reading and writing them (.lac, CSV), recording a simulated board, the
 * Z80 bus cycles, comparing two captures. See lacap.h. */
#include "lacap.h"
#include "board.h"
#include "zx48.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------------ */
/* The signals and the channel map                                          */
/* ------------------------------------------------------------------------ */

static const char *const sig_names[LA_NSIG] = {
    "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7",
    "A8", "A9", "A10", "A11", "A12", "A13", "A14", "A15",
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
    "CLK", "/RESET", "/MREQ", "/IORQ", "/RD", "/WR", "/M1", "/RFSH",
    "/HALT", "/WAIT", "/INT", "/NMI", "/BUSRQ", "/BUSACK", "/ROMCS",
    "/IORQGE", "VIDEO", "Y", "U", "V",
};

/* Other names a channel may be given (letters and digits only, see norm()). */
static const struct { int sig; const char *names; } sig_aliases[] = {
    { LA_CLK, "CK CLOCK PHI CPUCLK" },
    { LA_RESET, "RST" },
    { LA_RFSH, "RFRSH REFRESH" },
    { LA_IORQ, "IOREQ" },
    { LA_BUSRQ, "BUSREQ" },
    { LA_IORQGE, "IORQULA" },
};

/* The nets of the simulated boards' edge connectors, where the name is not
 * the signal's own (the Issue 2/1 netlists and the 3B/4B/6A ones differ). */
static const struct { int sig; const char *nets; } sig_nets[] = {
    { LA_CLK, "/CK CLK" },
    { LA_IORQ, "/IORQ /IOREQ" },
    { LA_BUSRQ, "/BUSRQ /BUSREQ" },
    { LA_IORQGE, "/IORQGE /IORQULA" },
    { LA_Y, "Y /Y" },
    { LA_U, "U LKU" },
    { LA_V, "V LKV" },
};

const char *la_signal_name(int sig) { return sig >= 0 && sig < LA_NSIG ? sig_names[sig] : "?"; }

/* Jim's interface, rev 3.2 (liveboxandy/ZX-Spectrum-Logic-Analyser-Interface),
 * channels 1-48 (board A 1-24, board B 25-48): read from the schematic's
 * header pins, NOT yet checked on a real one. -1: nothing (the NC pin A4,
 * the spare channels 6, 7 and 26 on J3). */
static int chan_map[48] = {
    -1, LA_VIDEO, LA_Y, LA_V, LA_U, -1, -1, LA_RESET,
    LA_A0, LA_A0 + 1, LA_A0 + 2, LA_A0 + 3,
    LA_A15, LA_A0 + 14, LA_A0 + 13, LA_A0 + 12, LA_A0 + 11, LA_A0 + 10, LA_A0 + 9, LA_A0 + 8,
    LA_A0 + 7, LA_A0 + 6, LA_A0 + 5, LA_A0 + 4,
    LA_CLK, -1, LA_BUSACK, LA_ROMCS, LA_RFSH, LA_WAIT, LA_BUSRQ, LA_IORQ,
    LA_MREQ, LA_HALT, LA_NMI, LA_IORQGE, LA_M1, LA_WR, LA_RD,
    LA_D7, LA_D0 + 6, LA_D0 + 5, LA_D0 + 4, LA_D0 + 3, LA_D0 + 2, LA_D0 + 1, LA_D0, LA_INT,
};

/* A name with only its letters and digits, in capitals ("~{MREQ}" -> "MREQ"). */
static void norm(const char *in, char *out, int len) {
    int n = 0;
    for (; *in && n < len - 1; in++)
        if (isalnum((unsigned char)*in)) out[n++] = (char)toupper((unsigned char)*in);
    out[n] = 0;
}

static bool in_words(const char *list, const char *word) {
    size_t n = strlen(word);
    for (const char *s = list; *s; ) {
        size_t len = strcspn(s, " ");
        if (len == n && !strncmp(s, word, n)) return true;
        s += len + (s[len] == ' ');
    }
    return false;
}

static int sig_by_norm(const char *w) {
    if (!*w) return -1;
    for (int i = 0; i < LA_NSIG; i++) {
        char c[16];
        norm(sig_names[i], c, sizeof(c));
        if (!strcmp(c, w)) return i;
    }
    for (size_t i = 0; i < sizeof(sig_aliases) / sizeof(sig_aliases[0]); i++)
        if (in_words(sig_aliases[i].names, w)) return sig_aliases[i].sig;
    return -1;
}

/* The signal a channel's name is, or -1: "/MREQ", "MREQ", "nMREQ",
 * "DIN33-/MREQ" (Jim's schematic labels: the part after the last '-'). */
static int sig_by_name(const char *name) {
    if (!name || !*name) return -1;
    char w[64];
    norm(name, w, sizeof(w));
    int s = sig_by_norm(w);
    if (s < 0 && w[0] == 'N') s = sig_by_norm(w + 1);
    const char *dash = strrchr(name, '-');
    if (s < 0 && dash) {
        norm(dash + 1, w, sizeof(w));
        s = sig_by_norm(w);
    }
    return s;
}

bool la_load_map(const char *path, char *err, int errlen) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "can't open %s", path); return false; }
    int map[48];
    for (int i = 0; i < 48; i++) map[i] = -1;
    char line[256];
    int ln = 0;
    while (fgets(line, sizeof(line), f)) {
        ln++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        int ch;
        char name[64];
        if (sscanf(line, "%d %63s", &ch, name) != 2) continue;
        int s = sig_by_name(name);
        if (ch < 1 || ch > 48 || s < 0) {
            snprintf(err, errlen, "%s line %d: %s", path, ln, ch < 1 || ch > 48 ? "channel not 1-48" : "unknown signal");
            fclose(f);
            return false;
        }
        map[ch - 1] = s;
    }
    fclose(f);
    memcpy(chan_map, map, sizeof(map));
    return true;
}

/* ------------------------------------------------------------------------ */
/* Base64                                                                    */
/* ------------------------------------------------------------------------ */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void b64_write(FILE *f, const uint8_t *p, int n) {
    for (int i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        fputc(b64[v >> 18 & 63], f);
        fputc(b64[v >> 12 & 63], f);
        fputc(i + 1 < n ? b64[v >> 6 & 63] : '=', f);
        fputc(i + 2 < n ? b64[v & 63] : '=', f);
    }
}

/* ------------------------------------------------------------------------ */
/* Reading: a small JSON walker for .lac                                     */
/* ------------------------------------------------------------------------ */

typedef struct { const char *p, *end; bool bad; } js_t;

static void js_ws(js_t *j) { while (j->p < j->end && isspace((unsigned char)*j->p)) j->p++; }
static bool js_is(js_t *j, char c) { js_ws(j); if (j->p < j->end && *j->p == c) { j->p++; return true; } return false; }

/* A string into OUT (cut to LEN), or skipped (OUT NULL). */
static bool js_string(js_t *j, char *out, int len) {
    if (!js_is(j, '"')) { j->bad = true; return false; }
    int n = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            c = *j->p++;
            if (c == 'u') { j->p += 4; c = '?'; }
            else if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
        }
        if (out && n < len - 1) out[n++] = c;
    }
    if (out) out[n] = 0;
    if (j->p >= j->end) { j->bad = true; return false; }
    j->p++;
    return true;
}

static void js_skip(js_t *j) {
    js_ws(j);
    if (j->p >= j->end) { j->bad = true; return; }
    char c = *j->p;
    if (c == '"') { js_string(j, NULL, 0); return; }
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        j->p++;
        if (js_is(j, close)) return;
        do {
            if (c == '{') { js_string(j, NULL, 0); if (!js_is(j, ':')) { j->bad = true; return; } }
            js_skip(j);
            if (j->bad) return;
        } while (js_is(j, ','));
        if (!js_is(j, close)) j->bad = true;
        return;
    }
    while (j->p < j->end && !strchr(",}] \t\r\n", *j->p)) j->p++;     /* number, true, null... */
}

static double js_number(js_t *j) {
    js_ws(j);
    char *e;
    double v = strtod(j->p, &e);
    if (e == j->p) { js_skip(j); return 0; }
    j->p = e;
    return v;
}

/* A channel's samples: a base64 string, or an array of numbers. */
static uint8_t *js_samples(js_t *j, int *n) {
    js_ws(j);
    *n = 0;
    if (j->p < j->end && *j->p == '[') {
        int cap = 4096;
        uint8_t *out = malloc((size_t)cap);
        j->p++;
        if (!js_is(j, ']')) {
            do {
                if (*n == cap) out = realloc(out, (size_t)(cap *= 2));
                out[(*n)++] = js_number(j) != 0;
            } while (js_is(j, ','));
            if (!js_is(j, ']')) j->bad = true;
        }
        return out;
    }
    if (j->p >= j->end || *j->p != '"') { js_skip(j); return NULL; }
    j->p++;
    const char *q = j->p;
    while (q < j->end && *q != '"') q++;
    if (q >= j->end) { j->bad = true; return NULL; }
    uint8_t *out = malloc((size_t)((q - j->p) / 4 * 3 + 3));
    unsigned acc = 0;
    int bits = 0;
    for (; j->p < q; j->p++) {
        int v = b64_val(*j->p);
        if (v < 0) continue;    /* '=', '\' of an escaped '/' */
        acc = acc << 6 | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[(*n)++] = (acc >> bits & 0xFF) != 0; }
    }
    j->p = q + 1;
    return out;
}

typedef struct { int number; char name[64]; uint8_t *v; int n; } rawchan_t;

/* The signals of the channels: by name, else by the map. */
static bool la_build(la_capture_t *c, rawchan_t *ch, int nch, char *err, int errlen) {
    for (int i = 0; i < LA_NSIG; i++) c->chan[i] = -1;
    int from[LA_NSIG];
    int n = -1;
    for (int k = 0; k < nch; k++) {
        int s = sig_by_name(ch[k].name);
        if (s < 0 && ch[k].number >= 0 && ch[k].number < 48) s = chan_map[ch[k].number];
        if (s < 0 || !ch[k].v) continue;
        if (c->chan[s] >= 0) {
            fprintf(stderr, "channels %d and %d are both %s: %d used\n", c->chan[s] + 1, ch[k].number + 1,
                    sig_names[s], c->chan[s] + 1);
            continue;
        }
        c->chan[s] = ch[k].number;
        from[s] = k;
        if (n < 0 || ch[k].n < n) n = ch[k].n;
    }
    if (n <= 0) { snprintf(err, errlen, "no channel with samples the signals could be told by"); return false; }
    c->n = n;
    c->have = 0;
    c->s = calloc((size_t)n, sizeof(uint64_t));
    for (int s = 0; s < LA_NSIG; s++) {
        if (c->chan[s] < 0) continue;
        c->have |= 1ull << s;
        const uint8_t *v = ch[from[s]].v;
        for (int i = 0; i < n; i++) c->s[i] |= (uint64_t)(v[i] != 0) << s;
    }
    if (c->pre > n) c->pre = 0;
    return true;
}

static bool read_lac(const char *text, size_t len, la_capture_t *c, char *err, int errlen) {
    js_t j = { text, text + len, false };
    rawchan_t *ch = NULL;
    int nch = 0;
    bool ok = false;
    if (!js_is(&j, '{')) { snprintf(err, errlen, "not a .lac file (no JSON object)"); return false; }
    bool settings = false;
    do {
        char key[64];
        if (!js_string(&j, key, sizeof(key)) || !js_is(&j, ':')) break;
        if (strcmp(key, "Settings") || !js_is(&j, '{')) { js_skip(&j); continue; }
        settings = true;
        do {
            char k2[64];
            if (!js_string(&j, k2, sizeof(k2)) || !js_is(&j, ':')) break;
            if (!strcmp(k2, "Frequency")) c->freq = (int)js_number(&j);
            else if (!strcmp(k2, "PreTriggerSamples")) c->pre = (int)js_number(&j);
            else if (!strcmp(k2, "CaptureChannels") && js_is(&j, '[')) {
                if (js_is(&j, ']')) continue;
                do {
                    if (!js_is(&j, '{')) { j.bad = true; break; }
                    ch = realloc(ch, (size_t)(nch + 1) * sizeof(rawchan_t));
                    rawchan_t *r = &ch[nch++];
                    memset(r, 0, sizeof(*r));
                    r->number = -1;
                    if (!js_is(&j, '}')) {
                        do {
                            char k3[64];
                            if (!js_string(&j, k3, sizeof(k3)) || !js_is(&j, ':')) break;
                            js_ws(&j);
                            if (!strcmp(k3, "ChannelNumber")) r->number = (int)js_number(&j);
                            else if (!strcmp(k3, "ChannelName") && j.p < j.end && *j.p == '"')
                                js_string(&j, r->name, sizeof(r->name));
                            else if (!strcmp(k3, "Samples")) r->v = js_samples(&j, &r->n);
                            else js_skip(&j);
                        } while (!j.bad && js_is(&j, ','));
                        if (!js_is(&j, '}')) j.bad = true;
                    }
                } while (!j.bad && js_is(&j, ','));
                if (!js_is(&j, ']')) j.bad = true;
            }
            else js_skip(&j);
        } while (!j.bad && js_is(&j, ','));
        if (!js_is(&j, '}')) j.bad = true;
    } while (!j.bad && js_is(&j, ','));
    if (j.bad) snprintf(err, errlen, "the .lac file is damaged (JSON at byte %d)", (int)(j.p - text));
    else if (!settings || !nch) snprintf(err, errlen, "no capture channels in the .lac file");
    else ok = la_build(c, ch, nch, err, errlen);
    for (int k = 0; k < nch; k++) free(ch[k].v);
    free(ch);
    return ok;
}

static bool read_csv(char *text, la_capture_t *c, char *err, int errlen) {
    char *line = strtok(text, "\r\n");
    if (!line) { snprintf(err, errlen, "empty CSV file"); return false; }
    rawchan_t *ch = NULL;
    int nch = 0, skip = -1;
    for (char *f = line; ; ) {
        size_t len = strcspn(f, ",;");
        ch = realloc(ch, (size_t)(nch + 1) * sizeof(rawchan_t));
        rawchan_t *r = &ch[nch];
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%.*s", (int)len, f);
        char w[64];
        norm(r->name, w, sizeof(w));
        if (nch == 0 && (!strncmp(w, "TIME", 4) || !strcmp(w, "SAMPLE"))) skip = 0;
        r->number = nch - (skip == 0);
        nch++;
        if (!f[len]) break;
        f += len + 1;
    }
    int cap = 0, n = 0;
    while ((line = strtok(NULL, "\r\n"))) {
        if (n == cap) {
            cap = cap ? cap * 2 : 65536;
            for (int k = 0; k < nch; k++) ch[k].v = realloc(ch[k].v, (size_t)cap);
        }
        char *f = line;
        for (int k = 0; k < nch; k++) {
            ch[k].v[n] = strtod(f, NULL) != 0;
            f += strcspn(f, ",;");
            if (*f) f++;
        }
        n++;
    }
    for (int k = 0; k < nch; k++) ch[k].n = k == skip ? 0 : n;
    if (skip == 0) { free(ch[0].v); ch[0].v = NULL; }
    bool ok = la_build(c, ch, nch, err, errlen);
    for (int k = 0; k < nch; k++) free(ch[k].v);
    free(ch);
    return ok;
}

bool la_read(const char *path, la_capture_t *c, char *err, int errlen) {
    memset(c, 0, sizeof(*c));
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "can't open %s", path); return false; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)len + 1);
    size_t got = fread(text, 1, (size_t)len, f);
    fclose(f);
    text[got] = 0;
    size_t n = strlen(path);
    bool ok = n > 4 && !strcasecmp(path + n - 4, ".csv") ? read_csv(text, c, err, errlen)
                                                          : read_lac(text, got, c, err, errlen);
    free(text);
    return ok;
}

void la_free(la_capture_t *c) { free(c->s); memset(c, 0, sizeof(*c)); }

/* ------------------------------------------------------------------------ */
/* Writing                                                                   */
/* ------------------------------------------------------------------------ */

bool la_write(const char *path, const la_capture_t *c, char *err, int errlen) {
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, errlen, "can't write %s", path); return false; }
    size_t pn = strlen(path);
    /* The channels: the map's, those the capture has. */
    int nout = 0, sig[48], num[48];
    for (int k = 0; k < 48; k++)
        if (chan_map[k] >= 0 && (c->have >> chan_map[k] & 1)) { sig[nout] = chan_map[k]; num[nout++] = k; }
    if (pn > 4 && !strcasecmp(path + pn - 4, ".csv")) {
        for (int k = 0; k < nout; k++) fprintf(f, "%s%s", k ? "," : "", sig_names[sig[k]]);
        fputc('\n', f);
        for (int i = 0; i < c->n; i++) {
            for (int k = 0; k < nout; k++) fprintf(f, "%s%d", k ? "," : "", (int)(c->s[i] >> sig[k] & 1));
            fputc('\n', f);
        }
    } else {
        /* As the LogicAnalyzer program saves one (ExportedCapture). */
        int reset_ch = -1;
        for (int k = 0; k < 48; k++) if (chan_map[k] == LA_RESET) reset_ch = k;
        fprintf(f, "{\"Settings\":{\"Frequency\":%d,\"PreTriggerSamples\":%d,\"PostTriggerSamples\":%d,"
                "\"TotalSamples\":%d,\"LoopCount\":0,\"MeasureBursts\":false,\"CaptureChannels\":[",
                c->freq, c->pre, c->n - c->pre, c->n);
        uint8_t *v = malloc((size_t)c->n + 1);
        for (int k = 0; k < nout; k++) {
            for (int i = 0; i < c->n; i++) v[i] = c->s[i] >> sig[k] & 1;
            fprintf(f, "%s{\"TextualChannelNumber\":\"Channel %d\",\"ChannelNumber\":%d,\"ChannelName\":\"%s\","
                    "\"ChannelColor\":null,\"Hidden\":false,\"Samples\":\"", k ? "," : "", num[k] + 1, num[k],
                    sig_names[sig[k]]);
            b64_write(f, v, c->n);
            fputs("\"}", f);
        }
        free(v);
        fprintf(f, "],\"Bursts\":null,\"TriggerType\":1,\"TriggerChannel\":%d,\"TriggerInverted\":false,"
                "\"TriggerBitCount\":1,\"TriggerPattern\":1},\"Samples\":null,\"SelectedRegions\":null}\n",
                reset_ch < 0 ? 0 : reset_ch);
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) snprintf(err, errlen, "can't write %s", path);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Recording a simulated board                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    int net[LA_NSIG];
    int pre, post;
    uint64_t *ring; int rpos, rcount;   /* before the trigger */
    uint64_t *out; int nout;            /* from the trigger */
    bool reset_prev, triggered, done;
} rec_t;

static void rec_step(board_t *b, void *ctx) {
    rec_t *r = ctx;
    if (r->done) return;
    sim_t *s = &b->sim;
    uint64_t v = 0;
    for (int i = 0; i < LA_NSIG; i++)
        if (r->net[i] >= 0) v |= (uint64_t)(s->nread[s->nets[r->net[i]].root] & 1) << i;
    bool reset = v >> LA_RESET & 1;
    if (!r->triggered && reset && !r->reset_prev) {
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

bool la_record(board_t *b, int pre, int post, la_capture_t *c, char *err, int errlen) {
    memset(c, 0, sizeof(*c));
    if (!board_is_zx48(b)) { snprintf(err, errlen, "the analyser is made for the Spectrum's edge connector"); return false; }
    sim_t *s = &b->sim;
    rec_t r = { .pre = pre, .post = post };
    for (int i = 0; i < LA_NSIG; i++) {
        c->chan[i] = -1;
        r.net[i] = -1;
        char names[64];
        snprintf(names, sizeof(names), "%s", sig_names[i]);
        for (size_t k = 0; k < sizeof(sig_nets) / sizeof(sig_nets[0]); k++)
            if (sig_nets[k].sig == i) snprintf(names, sizeof(names), "%s", sig_nets[k].nets);
        for (char *w = strtok(names, " "); w && r.net[i] < 0; w = strtok(NULL, " ")) {
            int net = sim_find_net(s, w);
            if (net >= 0 && board_net_simulated(b, net)) r.net[i] = net;
        }
        if (r.net[i] < 0) continue;
        c->have |= 1ull << i;
        for (int k = 0; k < 48; k++) if (chan_map[k] == i) c->chan[i] = k;
    }
    if (r.net[LA_RESET] < 0) { snprintf(err, errlen, "no /RESET net on the edge connector"); return false; }
    r.ring = calloc((size_t)(pre > 0 ? pre : 1), sizeof(uint64_t));
    r.out = calloc((size_t)pre + (size_t)post, sizeof(uint64_t));
    if (!board_step_hook(b, rec_step, &r)) {
        free(r.ring); free(r.out);
        snprintf(err, errlen, "the board is busy (edge counting)");
        return false;
    }
    /* Power on is 70 ms of reset (C27): wait up to a second for it. */
    for (int t = 0; !r.done && t < ZX_FREQ; t += 1000) board_run(b, 1000);
    board_step_hook(b, NULL, NULL);
    free(r.ring);
    if (!r.triggered) {
        free(r.out);
        snprintf(err, errlen, "/RESET never went high in the first second: the analyser would not trigger");
        return false;
    }
    c->s = r.out;
    c->n = r.nout;
    c->pre = r.rcount;
    c->freq = ZX_FREQ * ZX_STEPS_PER_CYCLE;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Bus cycles                                                                */
/* ------------------------------------------------------------------------ */

static inline bool lo(uint64_t v, int sig) { return !(v >> sig & 1); }
static inline unsigned addr_of(uint64_t v) { return (unsigned)(v >> LA_A0 & 0xFFFF); }
static inline unsigned data_of(uint64_t v) { return (unsigned)(v >> LA_D0 & 0xFF); }

/* Where /RESET goes high, or 0. */
static int la_start(const la_capture_t *c) {
    if (!(c->have >> LA_RESET & 1)) return 0;
    for (int i = 1; i < c->n; i++)
        if (!lo(c->s[i], LA_RESET) && lo(c->s[i - 1], LA_RESET)) return i;
    return 0;
}

int la_cycles(const la_capture_t *c, la_cycle_t **out) {
    const uint64_t need = 1ull << LA_MREQ | 1ull << LA_IORQ | 1ull << LA_RD | 1ull << LA_WR;
    *out = NULL;
    if ((c->have & need) != need) return 0;
    bool m1 = c->have >> LA_M1 & 1, rfsh = c->have >> LA_RFSH & 1, clk = c->have >> LA_CLK & 1;
    int cap = 1024, n = 0;
    la_cycle_t *cy = malloc((size_t)cap * sizeof(la_cycle_t));
    int t = 0;
    bool in = false, mreq = false, saw_m1 = false, saw_rfsh = false, saw_rd = false, saw_wr = false;
    unsigned rdata = 0xFF, wdata = 0xFF;
    la_cycle_t cur = { 0 };
    int start = la_start(c);
    for (int i = start; i < c->n; i++) {
        uint64_t v = c->s[i];
        if (clk && i > start && (v >> LA_CLK & 1) && lo(c->s[i - 1], LA_CLK)) t++;
        bool act = lo(v, LA_MREQ) || lo(v, LA_IORQ);
        if (act && !in) {
            in = true;
            mreq = lo(v, LA_MREQ);
            saw_m1 = saw_rfsh = saw_rd = saw_wr = false;
            cur = (la_cycle_t){ .addr = (uint16_t)addr_of(v), .sample = i, .t = clk ? t : -1 };
        }
        if (in && act) {
            if (m1 && lo(v, LA_M1)) saw_m1 = true;
            if (rfsh && lo(v, LA_RFSH)) saw_rfsh = true;
            if (lo(v, LA_RD)) { saw_rd = true; rdata = data_of(v); }
            if (lo(v, LA_WR)) { saw_wr = true; wdata = data_of(v); }
            if (saw_m1 && !mreq) rdata = data_of(v);       /* the interrupt vector */
            continue;
        }
        if (!in) continue;
        in = false;
        int kind;
        if (mreq) kind = saw_rfsh ? LA_REFRESH : saw_m1 ? LA_FETCH : saw_wr ? LA_WRITE : saw_rd ? LA_READ : -1;
        else kind = saw_m1 ? LA_INTACK : saw_wr ? LA_OUT : saw_rd ? LA_IN : -1;
        if (kind < 0) continue;
        cur.kind = (uint8_t)kind;
        cur.data = (uint8_t)(kind == LA_WRITE || kind == LA_OUT ? wdata : kind == LA_REFRESH ? 0 : rdata);
        if (n == cap) cy = realloc(cy, (size_t)(cap *= 2) * sizeof(la_cycle_t));
        cy[n++] = cur;
    }
    *out = cy;
    return n;
}

static const char *const kind_names[] = { "fetch", "read", "write", "in", "out", "intack", "refresh" };

static void cycle_text(const la_cycle_t *y, char *out, int len) {
    if (y->kind == LA_REFRESH) snprintf(out, len, "%-7s %04X   ", kind_names[y->kind], y->addr);
    else snprintf(out, len, "%-7s %04X %02X", kind_names[y->kind], y->addr, y->data);
}

/* A fetch or read from the ROM that isn't what the ROM holds. The edge
 * connector's /ROMCS is the ROM's select (low for 0000-3FFF); held high
 * there by an interface (a cartridge, the Interface 1), the ROM is off. */
static bool rom_wrong(const la_capture_t *c, const la_cycle_t *y) {
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

static void print_info(const char *path, const la_capture_t *c, int count) {
    int start = la_start(c);
    printf("%s: %d samples", path, c->n);
    if (c->freq) printf(" at %.3f MHz (%.3f ms)", c->freq / 1e6, c->n * 1e3 / c->freq);
    printf(", /RESET goes high at sample %d\n", start);
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

static int first_fetch0(const la_cycle_t *cy, int n) {
    for (int k = 0; k < n; k++) if (cy[k].kind == LA_FETCH && cy[k].addr == 0) return k;
    return 0;
}

/* Without the refresh cycles (unless asked). */
static int filter(la_cycle_t *cy, int n, bool refresh) {
    int m = 0;
    for (int k = 0; k < n; k++) if (refresh || cy[k].kind != LA_REFRESH) cy[m++] = cy[k];
    return m;
}

static int compare(const char *an, const la_capture_t *a, const char *bn, const la_capture_t *b,
                   int context, bool refresh) {
    la_cycle_t *ca, *cb;
    int na = la_cycles(a, &ca), nb = la_cycles(b, &cb);
    na = filter(ca, na, refresh);
    nb = filter(cb, nb, refresh);
    if (!na || !nb) {
        printf("No bus cycles in %s\n", !na ? an : bn);
        free(ca); free(cb);
        return 1;
    }
    int ia = first_fetch0(ca, na), ib = first_fetch0(cb, nb);
    int ta = ca[ia].t, tb = cb[ib].t;
    printf("A: %s (%d cycles%s)\nB: %s (%d cycles%s)\n", an, na - ia, ia ? ", from its first fetch at 0000" : "",
           bn, nb - ib, ib ? ", from its first fetch at 0000" : "");
    int k = 0, soft = 0, timing = -1;
    for (; ia + k < na && ib + k < nb; k++) {
        const la_cycle_t *x = &ca[ia + k], *y = &cb[ib + k];
        bool same = x->kind == y->kind && x->addr == y->addr && (x->kind == LA_REFRESH || x->data == y->data);
        if (!same && x->kind == y->kind && x->addr == y->addr && (x->kind == LA_IN || x->kind == LA_INTACK)) {
            soft++;             /* a port or the floating bus: may differ */
            continue;
        }
        if (timing < 0 && x->t >= 0 && y->t >= 0 && abs((x->t - ta) - (y->t - tb)) > 1) timing = k;
        if (!same) break;
    }
    if (timing >= 0)
        printf("Timing: from cycle %d the T states differ (A T %d, B T %d from the first fetch)\n", timing,
               ca[ia + timing].t - ta, cb[ib + timing].t - tb);
    if (soft) printf("%d port reads / interrupt acknowledges read differently (ports, the floating bus)\n", soft);
    int rc = 0;
    if (ia + k >= na || ib + k >= nb)
        printf("The same for all %d cycles both have\n", k);
    else {
        rc = 2;
        printf("\nThe first difference, cycle %d:\n", k);
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
    free(ca); free(cb);
    return rc;
}

/* A capture of the simulated board, healthy or with the faults. */
static bool record_board(const la_cli_t *o, la_capture_t *c, char *err, int errlen) {
    board_t *b = board_create(o->machine ? o->machine : &machine_zx48);
    for (int i = 0; i < o->nfaults; i++)
        if (!board_apply_fault_spec(b, o->faults[i])) {
            snprintf(err, errlen, "bad fault spec: %s", o->faults[i]);
            board_destroy(b);
            return false;
        }
    bool ok = la_record(b, o->pre > 0 ? o->pre : 512, o->post > 0 ? o->post : 32000, c, err, errlen);
    board_destroy(b);
    return ok;
}

static const char *board_desc(const la_cli_t *o, char *out, int len) {
    const machine_t *m = o->machine ? o->machine : &machine_zx48;
    int n = snprintf(out, len, "the simulated %s", m->board_name);
    for (int i = 0; i < o->nfaults && n < len; i++) n += snprintf(out + n, len - n, "%s%s", i ? " " : ", ", o->faults[i]);
    return out;
}

int la_cli(const la_cli_t *o) {
    char err[512];
    if (o->map && !la_load_map(o->map, err, sizeof(err))) { fprintf(stderr, "%s\n", err); return 1; }
    int count = o->count > 0 ? o->count : 20;
    la_capture_t c;
    if (o->record) {
        if (!record_board(o, &c, err, sizeof(err))) { fprintf(stderr, "%s\n", err); return 1; }
        if (!la_write(o->record, &c, err, sizeof(err))) { fprintf(stderr, "%s\n", err); la_free(&c); return 1; }
        char d[300];
        printf("%s: %s, %d samples (%d before /RESET went high) at %.0f MHz\n", o->record,
               board_desc(o, d, sizeof(d)), c.n, c.pre, c.freq / 1e6);
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
        bool ok = o->against ? la_read(o->against, &b, err, sizeof(err)) : record_board(o, &b, err, sizeof(err));
        if (!ok) { fprintf(stderr, "%s: %s\n", o->against ? o->against : "recording", err); la_free(&c); return 1; }
        int rc = compare(o->compare, &c, o->against ? o->against : board_desc(o, bn, sizeof(bn)), &b,
                         o->count > 0 ? o->count : 8, o->refresh);
        la_free(&c); la_free(&b);
        return rc;
    }
    return 0;
}
