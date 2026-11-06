/* lacap.c - logic analyser captures of the ZX Spectrum's edge connector:
 * reading and writing them (.lac, CSV), recording a simulated board, the
 * Z80 bus cycles, comparing two captures. See lacap.h. */
#include "lacap.h"
#include "board.h"
#include "zx48.h"
#include "lapico.h"
#include <stdarg.h>
#include <stdio.h>

/* Everything printed goes to the --la-log file as well. */
static FILE *la_logf;
static int la_out(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    int n = vprintf(fmt, a);
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
static int quiet_out(const char *fmt, ...) { (void)fmt; return 0; }
static void la_err(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    if (la_logf) { fprintf(la_logf, "%s\n", msg); fflush(la_logf); }
}
#define printf la_out
#include <ctype.h>
#include <stdio.h>
#include <math.h>
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

/* Jim's interface, channels 1-48 (the master board 1-24, the other 25-48),
 * as the channel names in Jim's video have them; checked on a real one
 * (2026-11-06: an Issue 2, every bus cycle read right). -1: not a signal
 * here (18, 19, 47 unused; 20-24, named /MTR /DWR /DRD /OE2 /OE1 in the
 * video, aren't on the edge connector). The schematic, read before the
 * kit was built, had a different order: it was wrong. */
static int chan_map[48] = {
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

/* The trigger value for the address A on the master's channels 1-16, or
 * -1 if they aren't the address bus; and back. */
static long addr_pattern(unsigned a) {
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

/* ------------------------------------------------------------------------ */
/* Recording a simulated board                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    int net[LA_NSIG];
    int pre, post;
    int trig_addr;                      /* -1: /RESET going high; else the address bus becoming this */
    long long step, arm_step;           /* bus steps since power on; the address trigger counts from arm_step */
    bool after_reset;                   /* /RESET has gone high (address triggers count from then) */
    unsigned prev_addr;
    uint64_t *ring; int rpos, rcount;   /* before the trigger */
    uint64_t *out; int nout;            /* from the trigger */
    bool reset_prev, triggered, done;
} rec_t;

static void rec_step(board_t *b, void *ctx) {
    rec_t *r = ctx;
    if (r->done) return;
    sim_t *s = &b->sim;
    r->step++;
    uint64_t v = 0;
    for (int i = 0; i < LA_NSIG; i++)
        if (r->net[i] >= 0) v |= (uint64_t)(s->nread[s->nets[r->net[i]].root] & 1) << i;
    bool reset = v >> LA_RESET & 1;
    bool fire;
    if (r->trig_addr < 0) fire = reset && !r->reset_prev;
    else {
        /* As the analyser's pattern trigger: the address lines alone,
         * whatever the bus is doing, from the first match after reset. */
        unsigned a = (unsigned)(v >> LA_A0 & 0xFFFF);
        fire = r->after_reset && r->step >= r->arm_step && a == (unsigned)r->trig_addr && r->prev_addr != a;
        r->prev_addr = a;
    }
    if (reset && !r->reset_prev) r->after_reset = true;
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
static void find_nets(board_t *b, int net[LA_NSIG], la_capture_t *c) {
    sim_t *s = &b->sim;
    for (int i = 0; i < LA_NSIG; i++) {
        if (c) c->chan[i] = -1;
        net[i] = -1;
        char names[64];
        snprintf(names, sizeof(names), "%s", sig_names[i]);
        for (size_t k = 0; k < sizeof(sig_nets) / sizeof(sig_nets[0]); k++)
            if (sig_nets[k].sig == i) snprintf(names, sizeof(names), "%s", sig_nets[k].nets);
        for (char *w = strtok(names, " "); w && net[i] < 0; w = strtok(NULL, " ")) {
            int n = sim_find_net(s, w);
            if (n >= 0 && board_net_simulated(b, n)) net[i] = n;
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
        if (trig_addr < 0) snprintf(err, errlen, "/RESET never went high in %d ms: the analyser would not trigger", max_ms);
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

/* ------------------------------------------------------------------------ */
/* The boot's timeline: when the ROM gets where                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    int net[LA_NSIG];
    long long step;                     /* bus steps since power on */
    long long reset_at;                 /* the step /RESET went high, or -1 */
    bool prev_fetch, prev_read, reset_prev;
    float *first_fetch, *first_read;    /* ms after /RESET went high, a 64K table each; -1 never */
    uint32_t *fetches;
    uint32_t ints;                      /* fetches from 0038 */
    /* For the analyser's address trigger: when each address is first on
     * the bus for 8 steps or more (2 analyser samples at 14 MHz), and when
     * it could first be seen for an instant as the bus changes (the lines
     * don't switch together: line_early). ms after /RESET; -1 never. */
    float *held_first, *glitch_first;
    unsigned cur_addr; int cur_hold;
    /* The same, from when the analyser is armed for a stage (--la-capture
     * arms TL_ARMED ms before it's due, on the reset's time) to just after. */
    int nw;
    float w_from[8], w_to[8], *w_held[8], *w_glitch[8];
} tl_t;

#define TL_ARMED 40.0f

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

/* Pairs of addresses already looked at (a set), for glitch_mark. */
#define PAIRS_BITS 22
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

static const char *board_desc(const la_cli_t *o, char *out, int len);

static int timeline(const la_cli_t *o) {
    board_t *b = board_create(o->machine ? o->machine : &machine_zx48);
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

/* ------------------------------------------------------------------------ */
/* Bus cycles                                                                */
/* ------------------------------------------------------------------------ */

static inline bool lo(uint64_t v, int sig) { return !(v >> sig & 1); }
static inline unsigned addr_of(uint64_t v) { return (unsigned)(v >> LA_A0 & 0xFFFF); }
static inline unsigned data_of(uint64_t v) { return (unsigned)(v >> LA_D0 & 0xFF); }

/* Where /RESET goes high, or 0. On a real Spectrum /RESET rises slowly
 * (an RC): the Z80 is running milliseconds before the analyser's input
 * sees it high. So: the last fetch from 0000 before the rise, if any. */
static int la_start(const la_capture_t *c) {
    if (!(c->have >> LA_RESET & 1)) return 0;
    int r = 0;
    for (int i = 1; i < c->n && !r; i++)
        if (!lo(c->s[i], LA_RESET) && lo(c->s[i - 1], LA_RESET)) r = i;
    if (!r) return 0;
    bool m1 = c->have >> LA_M1 & 1;
    int best = r;
    for (int i = 1; i < r; i++)
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
    int start = la_start(c);
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
        }
        if (in && act) {
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

/* An access the ULA can hold the CPU up for: 4000-7FFF, or an even port. */
static bool ula_contended(const la_cycle_t *x) {
    if (x->kind == LA_REFRESH || x->kind == LA_INTACK) return false;
    if (x->kind == LA_IN || x->kind == LA_OUT) return !(x->addr & 1) || (x->addr >= 0x4000 && x->addr < 0x8000);
    return x->addr >= 0x4000 && x->addr < 0x8000;
}

/* RAM reads that don't give back what was written there earlier in the
 * same capture: a fault in the RAM (or its buffers, the multiplexers),
 * whatever the machine was doing. Prints up to SHOW of them. */
static int ram_readback(const la_cycle_t *cy, int n, int show) {
    static short last[65536];
    for (int k = 0; k < 65536; k++) last[k] = -1;
    int bad = 0;
    for (int k = 0; k < n; k++) {
        const la_cycle_t *x = &cy[k];
        if (x->addr < 0x4000) continue;
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

static void print_info(const char *path, const la_capture_t *c, int count) {
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
    int rb = ram_readback(cy, n, 0);
    printf("RAM reads that don't give back what was written: %d\n", rb);
    if (rb) ram_readback(cy, n, 5);
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
    /* Lined up on the reset's first fetch from 0000, or, triggered on
     * another address, on the cycle at the trigger with that address, from
     * a few cycles before it. */
    int align = a->trig_addr >= 0 ? a->trig_addr : b->trig_addr;
    int ia, ib;
    char from[64] = "";
    if (align < 0 || align == 0xFFFF) {
        ia = first_fetch0(ca, na);
        ib = first_fetch0(cb, nb);
        if (ia || ib) snprintf(from, sizeof(from), ", from the first fetch at 0000");
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
    int k = 0, soft = 0, timing = -1, ndiff = 0, diffs[8], area[4] = { 0 };
    for (; ia + k < na && ib + k < nb; k++) {
        /* The same bus cycle (kind and address) is the CPU doing the same;
         * the data can differ under that (a fault in what is read, or a
         * counter, a pushed address). The first cycle that isn't the same
         * ends it. */
        const la_cycle_t *x = &ca[ia + k], *y = &cb[ib + k];
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
    int rba = ram_readback(ca + ia, na - ia, 0), rbb = ram_readback(cb + ib, nb - ib, 0);
    if (rba) { printf("%s: %d RAM reads don't give back what was written there:\n", an, rba); ram_readback(ca + ia, na - ia, 5); rc = 2; }
    if (rbb) { printf("%s: %d RAM reads don't give back what was written there:\n", bn, rbb); ram_readback(cb + ib, nb - ib, 5); rc = 2; }
    if (ndiff && !area[0] && end && !rba && !rbb)
        printf("Only RAM data differ (counters, the stack's pushed registers): the state the machines were in, not a fault\n");
    if (!end) {
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
    free(ca); free(cb);
    return rc;
}

/* A capture of the simulated board, healthy or with the faults. */
/* Samples (at the simulation's 28 MHz) to record to cover capture A, and a
 * little more: 0 if A's rate isn't known. */
static int sim_length(const la_capture_t *a) {
    if (a->freq <= 0) return 0;
    return (int)((double)(a->n - a->pre) * (ZX_FREQ * ZX_STEPS_PER_CYCLE) / a->freq) + 4000;
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

/* --la-ports "A,B": each a port (COM12) or a board's USB serial number. */
static int pick_ports(const char *list, char ports[][64], int max, char *err, int errlen) {
    lp_port_t p[LP_MAXDEV * 2];
    int found = lp_find(p, LP_MAXDEV * 2);
    if (!list) {
        if (found == 1) { snprintf(ports[0], 64, "%s", p[0].port); return 1; }
        int n = snprintf(err, errlen, found ? "%d boards: say which is the master with --la-ports MASTER,OTHER (a port or a serial number):" :
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
            if (!strcasecmp(p[i].serial, w)) snprintf(ports[n], 64, "%s", p[i].port);
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
    { "reset",     0xFFFF,    0.031, "the reset: the RAM test's first write (FFFF), about 110 T after it" },
    { "ramcheck",  0x11E2,  449.419, "the RAM test starts reading the RAM back, from 4000 up (11E2, 0.45 s after the reset)" },
    { "ramdone",   0x11EF, 1650.562, "the RAM test is over (11EF, 1.65 s after the reset; sooner if the RAM has a fault)" },
    { "copyright", 0x1539, 1714.319, "the copyright message (1539, 1.71 s after the reset)" },
    { "main",      0x12A9, 1728.527, "the editor's main loop starts (12A9, 1.73 s after the reset)" },
    { "interrupt", 0x0038,       -1, "a frame interrupt (50 a second, while the ROM's interrupt routine is used)" },
    { NULL, 0, 0, NULL }
};

static const la_stage_t *stage_named(const char *t) {
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

static bool capture_real(const la_cli_t *o, la_capture_t *c, char *err, int errlen) {
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
    if (o->at > 0 && addr >= 0) {
        at_stage = (la_stage_t){ "the time", addr, o->at, "armed again a set time after the reset" };
        st = &at_stage;
        printf("Armed on the reset, then again %.1f ms after it, for %04X\n", o->at, addr);
    }
    else if (st) printf("Trigger: %s\n", st->what);
    else if (addr >= 0) printf("Trigger: the address bus showing %04X\n", addr);
    if (st && st->addr != 0x0038)
        printf("Hold the Spectrum's reset button down now, and let go once it says Armed.\n");
    if (st == &at_stage)
        printf("(armed for the reset first, then again by itself %.1f ms after it)\n", st->ms);
    printf("Capturing %d channels from %s%s%s at %d Hz, %d samples before the trigger and %d after\n", q.nchan,
           ports[0], q.ndev > 1 ? " (master) and " : "", q.ndev > 1 ? ports[1] : "", q.freq, q.pre, q.post);
    lp_printf = la_out;
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
            if (!fgets(line, sizeof(line), stdin)) { snprintf(err, errlen, "no key pressed"); return false; }
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
        if (timed) lp_printf = quiet_out;  /* not "Armed" again: the button is let go already */
        bool got = lp_capture(&q, v, &n, &pre, err, errlen);
        lp_printf = la_out;
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

static bool record_board(const la_cli_t *o, int post, int trig_addr, la_capture_t *c, char *err, int errlen) {
    board_t *b = board_create(o->machine ? o->machine : &machine_zx48);
    for (int i = 0; i < o->nfaults; i++)
        if (!board_apply_fault_spec(b, o->faults[i])) {
            snprintf(err, errlen, "bad fault spec: %s", o->faults[i]);
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
static int trigger_addr_of(const char *t) {
    const la_stage_t *st = stage_named(t);
    unsigned a;
    if (st) return st->addr;
    if (sscanf(t, "addr=%x", &a) == 1) return (int)(a & 0xFFFF);
    return -1;
}

static const char *board_desc(const la_cli_t *o, char *out, int len) {
    const machine_t *m = o->machine ? o->machine : &machine_zx48;
    int n = snprintf(out, len, "the simulated %s", m->board_name);
    for (int i = 0; i < o->nfaults && n < len; i++) n += snprintf(out + n, len - n, "%s%s", i ? " " : ", ", o->faults[i]);
    return out;
}

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
            rc = compare(o->capture, &c, board_desc(o, bn, sizeof(bn)), &b, count < 8 ? count : 8, o->refresh);
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
                         o->count > 0 ? o->count : 8, o->refresh);
        la_free(&c); la_free(&b);
        return rc;
    }
    return 0;
}
