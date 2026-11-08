/* lacap_file.c - the signals and the channel map, and reading and writing captures (.lac, CSV).
 *
 * AI-generated (Claude Code). */
#include "lacap_int.h"

/* ------------------------------------------------------------------------ */
/* The signals and the channel map                                          */
/* ------------------------------------------------------------------------ */

const char *const sig_names[LA_NSIG] = {
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
const sig_nets_t sig_nets[] = {
    { LA_CLK, "/CK CLK" },
    { LA_IORQ, "/IORQ /IOREQ" },
    { LA_BUSRQ, "/BUSRQ /BUSREQ" },
    { LA_IORQGE, "/IORQGE /IORQULA" },
    { LA_Y, "Y /Y" },
    { LA_U, "U LKU" },
    { LA_V, "V LKV" },
};
const size_t sig_nets_n = sizeof(sig_nets) / sizeof(sig_nets[0]);

const char *la_signal_name(int sig) { return sig >= 0 && sig < LA_NSIG ? sig_names[sig] : "?"; }

/* Jim's interface, channels 1-48 (the master board 1-24, the other 25-48),
 * as the channel names in Jim's video have them; checked on a real one
 * (2026-11-06: an Issue 2, every bus cycle read right). -1: not a signal
 * here (18, 19, 47 unused; 20-24, named /MTR /DWR /DRD /OE2 /OE1 in the
 * video, aren't on the edge connector). The schematic, read before the
 * kit was built, had a different order: it was wrong. */
int chan_map[48] = {
    LA_A15, LA_A0 + 14, LA_A0 + 13, LA_A0 + 12, LA_A0 + 11, LA_A0 + 10, LA_A0 + 9, LA_A0 + 8,
    LA_A0 + 7, LA_A0 + 6, LA_A0 + 5, LA_A0 + 4, LA_A0 + 3, LA_A0 + 2, LA_A0 + 1, LA_A0,
    LA_RESET, -1, -1, -1, -1, -1, -1, -1,
    LA_M1, LA_WR, LA_RD, LA_D7, LA_D0 + 6, LA_D0 + 5, LA_D0 + 4, LA_D0 + 3,
    LA_D0 + 2, LA_D0 + 1, LA_D0, LA_INT, LA_IORQGE, LA_NMI, LA_HALT, LA_MREQ,
    LA_IORQ, LA_BUSRQ, LA_WAIT, LA_RFSH, LA_ROMCS, LA_BUSACK, -1, LA_CLK,
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
        uint8_t *out = xmalloc((size_t)cap);
        j->p++;
        if (!js_is(j, ']')) {
            do {
                if (*n == cap) out = xrealloc(out, (size_t)(cap *= 2));
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
    uint8_t *out = xmalloc((size_t)((q - j->p) / 4 * 3 + 3));
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

/* The signals of the channels: by name, else by the map. */
bool la_build(la_capture_t *c, rawchan_t *ch, int nch, char *err, int errlen) {
    for (int i = 0; i < LA_NSIG; i++) c->chan[i] = -1;
    int from[LA_NSIG];
    int n = -1;
    /* Named channels first; the map then gives only channels that have no
     * name, and only signals no name has given (a channel named something
     * else, /MTR say, is left out). */
    int sig[256];
    for (int k = 0; k < nch && k < 256; k++) sig[k] = sig_by_name(ch[k].name);
    for (int pass = 0; pass < 2; pass++)
    for (int k = 0; k < nch && k < 256; k++) {
        int s = sig[k];
        if (pass == 1) {
            if (s >= 0 || ch[k].name[0] || ch[k].number < 0 || ch[k].number >= 48) continue;
            s = chan_map[ch[k].number];
            if (s >= 0 && c->chan[s] >= 0) continue;
        } else if (s < 0) continue;
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
    c->s = xcalloc((size_t)n, sizeof(uint64_t));
    for (int s = 0; s < LA_NSIG; s++) {
        if (c->chan[s] < 0) continue;
        c->have |= 1ull << s;
        const uint8_t *v = ch[from[s]].v;
        for (int i = 0; i < n; i++) c->s[i] |= (uint64_t)(v[i] != 0) << s;
    }
    if (c->pre > n) c->pre = 0;
    return true;
}

/* The trigger value for the address A on the master's channels 1-16, or
 * -1 if they aren't the address bus; and back. */
long addr_pattern(unsigned a) {
    unsigned v = 0;
    for (int k = 0; k < 16; k++) {
        int s = chan_map[k];
        if (s < LA_A0 || s > LA_A15) return -1;
        v |= (a >> (s - LA_A0) & 1u) << k;
    }
    return (long)v;
}
static int pattern_addr(unsigned v) {
    unsigned a = 0;
    for (int k = 0; k < 16; k++) {
        int s = chan_map[k];
        if (s < LA_A0 || s > LA_A15) return -1;
        a |= (v >> k & 1u) << (s - LA_A0);
    }
    return (int)a;
}

static bool read_lac(const char *text, size_t len, la_capture_t *c, char *err, int errlen) {
    js_t j = { text, text + len, false };
    int ttype = -1, tchan = -1, tcount = -1;
    long tpat = -1;
    c->trig_addr = -1;
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
            else if (!strcmp(k2, "TriggerType")) ttype = (int)js_number(&j);
            else if (!strcmp(k2, "TriggerChannel")) tchan = (int)js_number(&j);
            else if (!strcmp(k2, "TriggerBitCount")) tcount = (int)js_number(&j);
            else if (!strcmp(k2, "TriggerPattern")) tpat = (long)js_number(&j);
            else if (!strcmp(k2, "CaptureChannels") && js_is(&j, '[')) {
                if (js_is(&j, ']')) continue;
                do {
                    if (!js_is(&j, '{')) { j.bad = true; break; }
                    ch = xrealloc(ch, (size_t)(nch + 1) * sizeof(rawchan_t));
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
    /* A 16 bit pattern on channels 1-16: the address it was triggered on. */
    if (ok && ttype == 1 && tchan == 0 && tcount == 16 && tpat >= 0) c->trig_addr = pattern_addr((unsigned)tpat);
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
        ch = xrealloc(ch, (size_t)(nch + 1) * sizeof(rawchan_t));
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
            for (int k = 0; k < nch; k++) ch[k].v = xrealloc(ch[k].v, (size_t)cap);
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
    char *text = xmalloc((size_t)len + 1);
    size_t got = fread(text, 1, (size_t)len, f);
    fclose(f);
    text[got] = 0;
    size_t n = strlen(path);
    c->trig_addr = -1;
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
        uint8_t *v = xmalloc((size_t)c->n + 1);
        for (int k = 0; k < nout; k++) {
            for (int i = 0; i < c->n; i++) v[i] = c->s[i] >> sig[k] & 1;
            fprintf(f, "%s{\"TextualChannelNumber\":\"Channel %d\",\"ChannelNumber\":%d,\"ChannelName\":\"%s\","
                    "\"ChannelColor\":null,\"Hidden\":false,\"Samples\":\"", k ? "," : "", num[k] + 1, num[k],
                    sig_names[sig[k]]);
            b64_write(f, v, c->n);
            fputs("\"}", f);
        }
        free(v);
        long pat = c->trig_addr >= 0 ? addr_pattern((unsigned)c->trig_addr) : -1;
        if (pat >= 0)
            fprintf(f, "],\"Bursts\":null,\"TriggerType\":1,\"TriggerChannel\":0,\"TriggerInverted\":false,"
                    "\"TriggerBitCount\":16,\"TriggerPattern\":%ld},\"Samples\":null,\"SelectedRegions\":null}\n", pat);
        else
            fprintf(f, "],\"Bursts\":null,\"TriggerType\":1,\"TriggerChannel\":%d,\"TriggerInverted\":false,"
                    "\"TriggerBitCount\":1,\"TriggerPattern\":1},\"Samples\":null,\"SelectedRegions\":null}\n",
                    reset_ch < 0 ? 0 : reset_ch);
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) snprintf(err, errlen, "can't write %s", path);
    return ok;
}
