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
static bool la_screen = true;           /* false: to the log alone */
static int la_out(const char *fmt, ...) {
    va_list a;
    int n = 0;
    va_start(a, fmt);
    if (la_screen) n = vprintf(fmt, a);
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
    long long reset_step;               /* when /RESET went high (trig_addr -2 counts from it) */
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
        if (mreq) kind = saw_rfsh ? LA_REFRESH : saw_m1 ? LA_FETCH : saw_wr ? LA_WRITE : saw_rd ? LA_READ : -1;
        else kind = saw_m1 ? LA_INTACK : saw_wr ? LA_OUT : saw_rd ? LA_IN : -1;
        if (kind < 0) continue;
        cur.kind = (uint8_t)kind;
        if (best_n) cur.addr = (uint16_t)best_a;
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

/* What compare() found. */
typedef struct {
    int rc;                     /* 0 the same (or only the state), 2 a fault */
    bool diverged;              /* the CPU went another way (not an interrupt's timing) */
    int cycle;                  /* there */
    la_cycle_t a, b;
} cmp_out_t;

static int compare(const char *an, const la_capture_t *a, const char *bn, const la_capture_t *b,
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
    int rba = ram_readback(ca + ia, na - ia, 0), rbb = ram_readback(cb + ib, nb - ib, 0);
    if (rba) { printf("%s: %d RAM reads don't give back what was written there:\n", an, rba); ram_readback(ca + ia, na - ia, 5); rc = 2; }
    if (rbb) { printf("%s: %d RAM reads don't give back what was written there:\n", bn, rbb); ram_readback(cb + ib, nb - ib, 5); rc = 2; }
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


/* ------------------------------------------------------------------------ */
/* --la-diagnose: the boot stage by stage, and what's wrong                  */
/* ------------------------------------------------------------------------ */

/* The stages in order, and how much to capture of each (the analyser's
 * samples at 14 MHz): before the RAM test's end, enough to see the byte
 * it failed on. */
static const struct { const char *stage; int pre, post; } diag_plan[] = {
    { "reset", 1000, 97000 }, { "ramcheck", 1000, 97000 }, { "ramdone", 9000, 89000 },
    { "copyright", 1000, 97000 }, { "main", 1000, 97000 }, { "interrupt", 1000, 97000 },
};
#define DIAG_NSTAGES 6

enum { AR_ROM, AR_LRAM, AR_URAM, AR_N };

/* What a capture of the board under test shows against a working one's. */
typedef struct {
    int bad[AR_N][8], low[AR_N][8], high[AR_N][8];  /* reads with the bit wrong; of them 0 for 1, 1 for 0 */
    int nbad[AR_N], first[AR_N];                    /* reads with a bit wrong, the first one's address */
    int stuck[LA_NSIG];                             /* 1 low, 2 high all through, where a working one's changes */
    int pramt, pramt_ref;                           /* the RAM test's result (P-RAMT), -1 not seen */
    /* Wrong ROM reads that are the ROM's byte with address bit k the
     * other way: the ROM sees A<k> otherwise than the edge connector. */
    int rom_abit[14];
    /* The RAM test's first read of a byte, seen in the capture, that
     * wasn't 02 (the fill): where and what. */
    int rt_addr, rt_got;
    /* The RAM test's DEC (HL): the byte written isn't one less than the one
     * the analyser saw read, so the CPU read something else: those bits
     * changed late in the read (per bank: the bits, how often, the first). */
    unsigned late_bits[3];
    int late_n[3], late_addr[3], late_seen[3], late_wrote[3];
    int test_addr, test_got;                        /* the byte it stopped on and its first read (02 was written) */
    char test_seq[64];                              /* what the test did there: "read 02, wrote 01, read 11" */
    /* The lower RAM's wrong bytes against the ULA's screen fetch (from
     * the /INT in the capture): those written or read while it fetched. */
    int c_have, c_bad, c_bad_win, c_good, c_good_win;
    bool in_reset;                                  /* /RESET low all through: taken while it was held */
    int clk_pct;                                    /* CLK rises seen, % of 3.5 MHz (-1 not known) */
    int clk_stopped;                                /* a later capture: CLK stuck (1 low, 2 high), the CPU stopped */
    /* The keyboard (port FE reads): how many, which half rows were read
     * (bit r: A8+r low), and per line KB0-KB4 the half rows it read low on
     * (a key down; a working board on the bench has none). */
    int kb_n, kb_low[5], kb_all;                    /* kb_all: reads with all five lines low at once */
    unsigned kb_rows, kb_rows_low[5];
    /* Left running, before the boot (the idle capture, no reset). */
    int idle;                                       /* 1: there was one */
    int slow_ms;                                    /* its slow capture's length (0: none) */
    int rst_pulses;                                 /* /RESET going low in it */
    int int_n, int_ref;                             /* /INT going low in it; in a working one's */
    int held[LA_NSIG];                              /* 1: low all through, where a working one's isn't */
    char idle_where[200];                           /* where the CPU was */
} ev_t;

static void ev_init(ev_t *e) {
    memset(e, 0, sizeof(*e));
    e->pramt = e->pramt_ref = e->test_addr = -1;
    e->clk_pct = -1;
    e->rt_addr = -1;
}

/* Another stage's evidence added to E (--la-all). */
static void ev_merge(ev_t *e, const ev_t *f) {
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
        memcpy(e->idle_where, f->idle_where, sizeof(e->idle_where));
    }
    for (int ar = 0; ar < AR_N; ar++) {
        if (!e->late_n[ar] && f->late_n[ar]) { e->late_addr[ar] = f->late_addr[ar]; e->late_seen[ar] = f->late_seen[ar]; e->late_wrote[ar] = f->late_wrote[ar]; }
        e->late_n[ar] += f->late_n[ar];
        e->late_bits[ar] |= f->late_bits[ar];
    }
    if (e->rt_addr < 0) { e->rt_addr = f->rt_addr; e->rt_got = f->rt_got; }
    if (f->clk_pct >= 0 && (e->clk_pct < 0 || f->clk_pct < e->clk_pct)) e->clk_pct = f->clk_pct;
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

static unsigned ev_mask(const ev_t *e, int ar) {
    unsigned m = 0;
    for (int i = 0; i < 8; i++) if (e->bad[ar][i]) m |= 1u << i;
    return m;
}

static bool ev_pramt_bad(const ev_t *e) { return e->pramt >= 0 && e->pramt_ref >= 0 && e->pramt != e->pramt_ref; }

/* The frame interrupt, left running: not as often as a working one's. */
static bool int_wrong(const ev_t *e) {
    return e->slow_ms && e->int_ref >= 2 && !e->held[LA_INT] && abs(e->int_n - e->int_ref) > 1;
}

static bool ev_any(const ev_t *e) {
    for (int s = 0; s < LA_NSIG; s++) if (e->stuck[s]) return true;
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
static int line_state(const la_capture_t *c, int s) {
    int from = la_start(c);
    if (from >= c->n) return -1;
    uint64_t v0 = c->s[from] >> s & 1;
    for (int i = from + 1; i < c->n; i++) if ((c->s[i] >> s & 1) != v0) return -1;
    return v0 ? 2 : 1;
}

/* The evidence in D (the board under test) against R (a working one). */
static void gather(ev_t *e, const la_capture_t *d, const la_capture_t *r) {
    la_cycle_t *cy;
    int n = la_cycles(d, &cy);
    static short last[65536];
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
        if (x->addr < 0x4000) continue;
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
    free(cy);
    e->in_reset = (d->have >> LA_RESET & 1) && line_state(d, LA_RESET) == 1;
    if ((d->have >> LA_CLK & 1) && d->freq > 0 && line_state(d, LA_CLK) < 0) {
        int from = la_start(d), rises = 0;
        for (int i = from + 1; i < d->n; i++) rises += !lo(d->s[i], LA_CLK) && lo(d->s[i - 1], LA_CLK);
        double want = (double)(d->n - from) * ZX_FREQ / d->freq;
        if (want > 1000) e->clk_pct = (int)(100.0 * rises / want + 0.5);
    }
    /* Lines any running Z80 changes all the time. */
    static const int busy[] = { LA_MREQ, LA_RD, LA_WR, LA_M1, LA_RFSH, LA_CLK };
    for (int s = 0; r && s < LA_NSIG; s++) {
        bool look = (s >= LA_A0 && s <= LA_A15) || (s >= LA_D0 && s <= LA_D7);
        for (int k = 0; k < 6; k++) look |= s == busy[k];
        if (!look || !(d->have >> s & 1) || !(r->have >> s & 1)) continue;
        int st = line_state(d, s);
        if (st > 0 && line_state(r, s) < 0) e->stuck[s] = st;
    }
}

/* Where the CPU was in a capture (taken at a time, not at a stage). */
static void where_text(const la_capture_t *c, char *out, int len) {
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
    if (!nf) snprintf(out, len, "no instruction fetches at all: the CPU isn't running (its clock, /RESET, /WAIT or /BUSRQ, its power)");
    else if (halt > (c->n - from) / 2) snprintf(out, len, "halted (HALT low): it ran into a HALT, so it went astray before");
    else if (2 * fill > nf) snprintf(out, len, "still filling the RAM for the RAM test (11DC-11E1)");
    else if (2 * rb > nf) snprintf(out, len, "still reading the RAM back for the RAM test (11E2-11EE)");
    else if (2 * ram > nf) snprintf(out, len, "running code from RAM (%04X-%04X): it went astray", lo_a, hi_a);
    else snprintf(out, len, "fetching from %04X-%04X%s", lo_a, hi_a, irq ? " (and the frame interrupt)" : "");
}

/* A simulated board's capture: with FAULTS, triggered at TRIG (-1 the
 * reset, -2 ARM_MS after the reset, else the address from ARM_MS after
 * power on). */
static bool record_sim(const machine_t *m, const char *const *faults, int nfaults, int pre, int post, int trig, int arm_ms,
                       la_capture_t *c, char *err, int errlen) {
    board_t *b = board_create(m);
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
static bool capture_now(const la_cli_t *o, double at_ms, const char *save, la_capture_t *c, char *err, int errlen) {
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
    if (at_ms > 0) { char line[16]; if (!fgets(line, sizeof(line), stdin)) return false; }
    q.trigger = "raw:1,15,1,0";
    return capture_real(&q, c, err, errlen);
}

static bool want_enter(const char *msg) {
    printf("%s", msg);
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) return false;
    return line[0] != 'n' && line[0] != 'N';
}

static board_t *vboard;     /* the board's parts, for their references */

static const char *ref_of(const char *label) {
    for (int i = 0; vboard && i < vboard->sim.nparts; i++) {
        const char *l = board_part_label(vboard, i);
        if (l && !strcmp(l, label)) return vboard->sim.parts[i].ref;
    }
    return NULL;
}

/* The parts that serve one bank of RAM as a whole (their description
 * says "lower RAM" or "upper RAM"), but its chips. */
static void bank_parts(const char *bank, const char *chips, char *out, int len) {
    int n = 0;
    out[0] = 0;
    for (int i = 0; vboard && i < vboard->sim.nparts && n < len; i++) {
        const char *d = board_part_detail(vboard, i), *l = board_part_label(vboard, i);
        if (!d || !strstr(d, bank) || (l && !strncmp(l, chips, 4))) continue;
        n += snprintf(out + n, len - n, "%s%s (%s)", n ? ", " : "", vboard->sim.parts[i].ref, l ? l : "?");
    }
}

/* The resistors between the CPU's data lines and the chips labelled
 * CHIPS* (the lower RAM's: the ULA's side of the bus). */
static void data_resistors(const char *chips, char *out, int len) {
    sim_t *s = &vboard->sim;
    int n = 0;
    out[0] = 0;
    for (int bit = 0; bit < 8; bit++) {
        char nm[4];
        snprintf(nm, sizeof(nm), "D%d", bit);
        int dn = sim_find_net(s, nm);
        if (dn < 0) continue;
        int droot = s->nets[dn].root;
        for (int i = 0; i < s->nparts && n < len; i++) {
            const part_t *r = &s->parts[i];
            if (r->npins != 2 || strcmp(board_part_kind_name(vboard, i), "resistor")) continue;
            int r0 = s->pins[r->first_pin].root, r1 = s->pins[r->first_pin + 1].root;
            int other = r0 == droot ? r1 : r1 == droot ? r0 : -1;
            if (other < 0 || board_net_supply(vboard, other) != RAIL_NONE) continue;    /* a pull-up */
            bool to_chip = false;       /* the other side reaches a CHIPS chip */
            for (int j = 0; j < s->nparts && !to_chip; j++) {
                const char *l = board_part_label(vboard, j);
                if (!l || strncmp(l, chips, strlen(chips))) continue;
                for (int k = 0; k < s->parts[j].npins && !to_chip; k++) to_chip = s->pins[s->parts[j].first_pin + k].root == other;
            }
            if (to_chip) n += snprintf(out + n, len - n, "%s%s", n ? " " : "", r->ref);
        }
    }
}

static int z80_pin(int s) {
    static const int a[16] = { 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 1, 2, 3, 4, 5 };
    static const int d[8] = { 14, 15, 12, 8, 7, 9, 10, 13 };
    if (s >= LA_A0 && s <= LA_A15) return a[s - LA_A0];
    if (s >= LA_D0 && s <= LA_D7) return d[s - LA_D0];
    switch (s) {
    case LA_MREQ: return 19; case LA_RD: return 21; case LA_WR: return 22; case LA_M1: return 27; case LA_RFSH: return 28;
    case LA_CLK: return 6; case LA_INT: return 16; case LA_NMI: return 17; case LA_HALT: return 18;
    case LA_WAIT: return 24; case LA_BUSRQ: return 25; case LA_RESET: return 26;
    default: return 0;
    }
}

/* The pin of part REF on the signal's line (0: none). */
static int pin_on(const char *ref, int sig) {
    if (!vboard || !ref) return 0;
    int net[LA_NSIG];
    find_nets(vboard, net, NULL);
    if (net[sig] < 0) return 0;
    sim_t *s = &vboard->sim;
    int root = s->nets[net[sig]].root;
    for (int i = 0; i < s->nparts; i++) {
        if (strcmp(s->parts[i].ref, ref)) continue;
        for (int j = 0; j < s->parts[i].npins; j++)
            if (s->pins[s->parts[i].first_pin + j].root == root) return s->pins[s->parts[i].first_pin + j].number;
    }
    return 0;
}

static const char *bits_way(const ev_t *e, int ar, int bit) {
    if (!e->high[ar][bit]) return "reads 0 where it should be 1";
    if (!e->low[ar][bit]) return "reads 1 where it should be 0";
    return "reads wrong both ways";
}

static void bits_text(unsigned m, char *out, int len) {
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < 8 && n < len; i++) if (m >> i & 1) n += snprintf(out + n, len - n, "%sD%d", n ? " " : "", i);
}

static int popcount8(unsigned m) { int c = 0; for (; m; m &= m - 1) c++; return c; }

/* The bank's multiplexers that carry CPU address line A<k> (on its net, or
 * through a resistor from it). */
static void mux_for_addr(const char *bank, const char *chips, int k, char *out, int len) {
    sim_t *s = &vboard->sim;
    char nm[8];
    snprintf(nm, sizeof(nm), "A%d", k);
    int an = sim_find_net(s, nm);
    int n = 0;
    out[0] = 0;
    if (an < 0) return;
    int roots[8], nr = 0;
    roots[nr++] = s->nets[an].root;
    for (int i = 0; i < s->nparts && nr < 8; i++) {        /* through a resistor */
        const part_t *r = &s->parts[i];
        if (r->npins != 2 || strcmp(board_part_kind_name(vboard, i), "resistor")) continue;
        int r0 = s->pins[r->first_pin].root, r1 = s->pins[r->first_pin + 1].root;
        if (r0 == roots[0] && board_net_supply(vboard, r1) == RAIL_NONE) roots[nr++] = r1;
        else if (r1 == roots[0] && board_net_supply(vboard, r0) == RAIL_NONE) roots[nr++] = r0;
    }
    for (int i = 0; i < s->nparts && n < len; i++) {
        const char *d = board_part_detail(vboard, i), *l = board_part_label(vboard, i);
        if (!d || !strstr(d, bank) || !l || strcmp(l, "MUX")) continue;
        bool on = false;
        for (int j = 0; j < s->parts[i].npins && !on; j++)
            for (int q = 0; q < nr && !on; q++) on = s->pins[s->parts[i].first_pin + j].root == roots[q];
        if (on) n += snprintf(out + n, len - n, "%s%s", n ? " or " : "", s->parts[i].ref);
    }
    (void)chips;
}

/* What it points at, most likely first. */
static int verdict(const ev_t *e, const machine_t *m, const char *stage, bool missing, const cmp_out_t *co, const char *where) {
    vboard = board_create(m);
    const char *cpu = ref_of("CPU"), *rom = ref_of("ROM"), *ula = ref_of("ULA");
    if (!cpu) cpu = "the CPU";
    if (!rom) rom = "the ROM";
    int n = 0;
    char buf[400], bits[64], res[200];
    bool idle_st = !strcmp(stage, "idle");
    if (e->in_reset && !idle_st)
        printf("\nNote: /RESET was low all through that capture: it was taken while the reset button was held (or /RESET\n"
               "is stuck low), when the CPU drives nothing; what it shows isn't the boot.\n");
    printf("\nWhat to look at, most likely first:\n");
    /* Most of the lines dead: the CPU isn't running at all. */
    int nstuck = 0, nctl = 0;
    for (int s = 0; s < LA_NSIG; s++)
        if (e->stuck[s]) { nstuck++; nctl += s == LA_MREQ || s == LA_RD || s == LA_WR || s == LA_M1 || s == LA_RFSH; }
    if (e->rst_pulses) {
        printf("  %d. /RESET goes low %d time%s in %d ms, left running with nobody at the button: the CPU keeps\n"
               "     restarting (a working one's stays high). Something pulls /RESET low now and then: the CPU (%s pin 26:\n"
               "     a faulty Z80 can short its input), the reset circuit (its capacitor and resistor), the button, or the\n"
               "     5 V supply dipping. Pull the CPU and watch pin 26's socket hole: it should stay at 5 V.\n", ++n,
               e->rst_pulses, e->rst_pulses == 1 ? "" : "s", e->slow_ms, cpu);
        goto done;
    }
    if (e->in_reset && idle_st && !e->stuck[LA_CLK]) {
        printf("  %d. /RESET is low all through, left running with nobody at the button, so the CPU never starts.\n"
               "     Something holds /RESET low: the CPU (%s pin 26: a faulty Z80 can short its input), the reset\n"
               "     circuit (its capacitor and resistor) or the button. Pull the CPU and measure pin 26's socket hole:\n"
               "     it should rise to 5 V.\n", ++n, cpu);
        goto done;
    }
    if (e->in_reset && !e->stuck[LA_CLK]) {
        printf("  %d. /RESET is low all through, so the CPU never starts. If the reset button wasn't held then (it asks\n"
               "     once more when it sees this), something holds /RESET low: the CPU (%s pin 26: a faulty Z80 can\n"
               "     short its input), the reset circuit (its capacitor and resistor) or the button. Pull the CPU and\n"
               "     measure pin 26's socket hole: it should rise to 5 V.\n", ++n, cpu);
        goto done;
    }
    if (e->stuck[LA_CLK])
        printf("  %d. No clock: CLK (%s's pin 6) is stuck %s. It comes from %s (the ULA, from the 14 MHz crystal): the ULA, the\n"
               "     crystal, or a short on the line. Nothing runs without it.\n", ++n, cpu, e->stuck[LA_CLK] == 1 ? "low" : "high",
               ula ? ula : "ULA");
    if (!e->stuck[LA_CLK] && e->clk_pct >= 0 && e->clk_pct < 30)
        printf("  %d. The CPU's clock is wrong: the analyser sees only %d%% of CLK's pulses (a working ULA's: about 90%%),\n"
               "     so they are too narrow or too weak, from %s (the ULA). A CPU on a clock like that misreads and\n"
               "     miswrites: what follows may all come from it.\n", ++n, e->clk_pct, ula ? ula : "the ULA");
    if (e->clk_stopped)
        printf("  %d. Later the clock stopped: CLK stuck %s, so the CPU stopped. It comes from %s (the ULA): a ULA that\n"
               "     fails as it warms up does this.\n", ++n, e->clk_stopped == 1 ? "low" : "high", ula ? ula : "the ULA");
    if (!e->stuck[LA_CLK] && ((e->clk_pct >= 0 && e->clk_pct < 30) || e->clk_stopped)) {
        unsigned all = ev_mask(e, AR_ROM) | ev_mask(e, AR_LRAM) | ev_mask(e, AR_URAM);
        if (all) {
            bits_text(all, bits, sizeof(bits));
            printf("     What the bus shows on that clock (look at it again once the clock is right): bits %s read wrong\n"
                   "     (ROM %d, lower RAM %d, upper RAM %d reads)", bits, e->nbad[AR_ROM], e->nbad[AR_LRAM], e->nbad[AR_URAM]);
            if (ev_pramt_bad(e)) printf("; the RAM test stopped at %04X", e->pramt + 1);
            printf(".\n");
        }
        goto done;
    }
    if (nstuck >= 8 && e->stuck[LA_CLK]) {
        printf("     So the CPU isn't running (%d of the lines a running Z80 changes never change): that follows from the clock.\n",
               nstuck - 1);
        goto done;
    }
    if (e->held[LA_WAIT] || e->held[LA_BUSRQ]) {
        int sg = e->held[LA_WAIT] ? LA_WAIT : LA_BUSRQ;
        printf("  %d. %s (%s pin %d) is low all through: the CPU %s. It's the CPU's input, from the\n"
               "     edge connector (anything plugged in?) and a pull-up: a short to 0 V on the line, or the CPU itself.\n",
               ++n, la_signal_name(sg), cpu, z80_pin(sg),
               sg == LA_WAIT ? "waits for ever in its bus cycle" : "hands the bus over and stops");
        goto done;
    }
    if (nstuck >= 8) {
        printf("  %d. The CPU isn't running: %d of the lines a running Z80 changes all the time never change. Check what it\n"
               "     needs: its clock (pin 6), /RESET (pin 26) high, /WAIT (24) and /BUSRQ (25) high, 5 V on pin 11, 0 V on\n"
               "     pin 29; then the CPU (%s) itself.\n", ++n, nstuck, cpu);
        goto done;
    }
    /* A line that doesn't change: stuck, whatever drives it. */
    for (int s = 0; s < LA_NSIG; s++) {
        if (!e->stuck[s] || s == LA_CLK) continue;
        printf("  %d. %s is stuck %s all through, where a working board's changes all the time: a short to %s, or the line\n"
               "     broken. It's %s's pin %d; every chip on the line is a suspect, and the line's tracks.\n",
               ++n, la_signal_name(s), e->stuck[s] == 1 ? "low" : "high", e->stuck[s] == 1 ? "0 V (or another line)" : "5 V (or another line)",
               cpu, z80_pin(s));
    }
    /* Left running: the frame interrupt (the ULA's, every 20 ms) and HALT. */
    {
        int ip = pin_on(ula, LA_INT);
        char up[48];
        if (ip) snprintf(up, sizeof(up), "%s pin %d", ula ? ula : "the ULA", ip);
        else snprintf(up, sizeof(up), "%s", ula ? ula : "the ULA");
        if (e->held[LA_INT])
            printf("  %d. /INT (%s pin 16) is low all through, left running: a working one's goes low for 32 T every 20 ms.\n"
                   "     It comes from the ULA (%s): the ULA, or a short to 0 V on the line.\n", ++n, cpu, up);
        else if (int_wrong(e) && !e->int_n)
            printf("  %d. No frame interrupt: /INT (%s pin 16) never goes low in %d ms, left running; a working one's does\n"
                   "     %d times (every 20 ms). It comes from the ULA (%s): the ULA, or the line broken. Without it\n"
                   "     the keyboard isn't read.\n", ++n, cpu, e->slow_ms, e->int_ref, up);
        else if (int_wrong(e))
            printf("  %d. The frame interrupt: /INT goes low %d times in %d ms, left running, a working one's %d (every 20 ms).\n"
                   "     It comes from the ULA (%s): its timing, or something else pulling the line.\n", ++n,
                   e->int_n, e->slow_ms, e->int_ref, up);
    }
    /* The keyboard: a line low on every half row is no key (nobody holds
     * a whole column down): the line or the ULA's input. Else the keys. */
    static const char *const keys[8][5] = {
        { "CAPS SHIFT", "Z", "X", "C", "V" }, { "A", "S", "D", "F", "G" }, { "Q", "W", "E", "R", "T" },
        { "1", "2", "3", "4", "5" }, { "0", "9", "8", "7", "6" }, { "P", "O", "I", "U", "Y" },
        { "ENTER", "L", "K", "J", "H" }, { "SPACE", "SYMBOL SHIFT", "M", "N", "B" } };
    static const int ula_kb_pin[5] = { 19, 20, 23, 24, 26 };
    for (int k = 0; k < 5; k++) {
        if (!e->kb_low[k]) continue;
        unsigned rows = e->kb_rows_low[k];
        /* Three rows or more (the ROM's KEY-SCAN gives up after three keys). */
        if (popcount8(rows) >= 3) {
            char col[96];
            int m = 0;
            for (int r = 0; r < 8; r++) m += snprintf(col + m, sizeof(col) - m, "%s%s", r ? " " : "", keys[r][k]);
            printf("  %d. Keyboard line KB%d reads low on every half row (%d reads): as if %s were all held\n"
                   "     down, which no hand does. The line is shorted to 0 V (the keyboard connector, the membrane's\n"
                   "     track, a bridge to its neighbour) or the ULA's KB%d input (%s pin %d) is.\n",
                   ++n, k, e->kb_low[k], col, k, ula ? ula : "the ULA", ula_kb_pin[k]);
        } else {
            for (int r = 0; r < 8; r++)
                if (rows >> r & 1)
                    printf("  %d. The key %s reads as held down (half row A%d, line KB%d): a key stuck or the membrane\n"
                           "     shorted there%s\n", ++n, keys[r][k], 8 + r, k, "");
        }
    }
    unsigned mr = ev_mask(e, AR_ROM), ml = ev_mask(e, AR_LRAM), mu = ev_mask(e, AR_URAM);
    unsigned shared = (mr & ml) | (mr & mu) | (ml & mu);
    /* The ROM's wrong reads are its own bytes from the address with one
     * bit the other way: that address line, not the data. */
    int abit = -1;
    for (int k = 0; k < 14; k++)
        if (e->rom_abit[k] >= 8 && 2 * e->rom_abit[k] >= e->nbad[AR_ROM] && (abit < 0 || e->rom_abit[k] > e->rom_abit[abit])) abit = k;
    if (abit >= 0) {
        printf("  %d. Address line A%d: %d of the %d wrong ROM reads are the ROM's own byte from the address with A%d the\n"
               "     other way. So the ROM sees A%d otherwise than the edge connector does: the line is weak or floating\n"
               "     (in between 0 and 5 V), not stuck. Its driver is the CPU (%s pin %d): a faulty CPU's output does this;\n"
               "     else a break in the track to the ROM (%s) or a chip loading the line.\n", ++n, abit, e->rom_abit[abit],
               e->nbad[AR_ROM], abit, abit, cpu, z80_pin(LA_A0 + abit), rom);
        printf("     The wrong data that follows (bits %s from the ROM and the RAM) all comes from it.\n",
               (bits_text(mr | ml | mu, bits, sizeof(bits)), bits));
        goto done;
    }
    /* The RAM test's first bad byte reads 00 and lies 2^k above the bank's
     * start: the same cell as the start (tested and left at 00), so the
     * bank's address line A<k> doesn't get through. */
    int alias_ar = -1, alias_k = -1;
    if (e->rt_addr >= 0x4000 && e->rt_got == 0x00) {
        unsigned base = e->rt_addr < 0x8000 ? 0x4000 : 0x8000, off = (unsigned)e->rt_addr - base;
        if (off && !(off & (off - 1)))
            for (int k = 0; k < 15; k++) if (off == 1u << k) { alias_k = k; alias_ar = base == 0x4000 ? AR_LRAM : AR_URAM; }
    }
    if (alias_k >= 0) {
        const char *bank = alias_ar == AR_LRAM ? "lower RAM" : "upper RAM", *chips = alias_ar == AR_LRAM ? "LRAM" : "URAM";
        unsigned base = alias_ar == AR_LRAM ? 0x4000 : 0x8000;
        char mx[64];
        mux_for_addr(bank, chips, alias_k, mx, sizeof(mx));
        printf("  %d. The %s's address line A%d: the RAM test found %04X reading 00, which is what it had just left\n"
               "     at %04X: those two are the same cell, so A%d doesn't get through to the %s's chips. Its\n"
               "     multiplexer%s%s%s, or the line from it to all the chips (a break, a short to the line next to it).\n",
               ++n, bank, alias_k, e->rt_addr, base, alias_k, bank, mx[0] ? " (" : "", mx, mx[0] ? ")" : "");
        printf("     The wrong bytes later on (the system variables there) come from it.\n");
        if (alias_ar == AR_LRAM) ml = 0; else mu = 0;
        shared &= ~0u;
        shared = (mr & ml) | (mr & mu) | (ml & mu);
    }
    for (int i = 0; i < 8; i++) {
        if (!(shared >> i & 1) || e->stuck[LA_D0 + i]) continue;
        printf("  %d. Data line D%d is wrong from %s%s%s: not one chip, the line itself (%s's pin %d) or a chip that\n"
               "     holds it (a short, a chip driving it when it shouldn't)\n", ++n, i,
               mr >> i & 1 ? "the ROM" : "", mr >> i & 1 && (ml | mu) >> i & 1 ? " and " : "", (ml | mu) >> i & 1 ? "the RAM" : "",
               cpu, z80_pin(LA_D0 + i));
    }
    unsigned stuckd = 0;
    for (int i = 0; i < 8; i++) if (e->stuck[LA_D0 + i]) stuckd |= 1u << i;
    mr &= ~(shared | stuckd); ml &= ~(shared | stuckd); mu &= ~(shared | stuckd);
    /* A stuck control or address line: the wrong bytes follow from it. */
    bool addr_stuck = false;
    for (int i = 0; i < 16; i++) addr_stuck |= e->stuck[LA_A0 + i] != 0;
    if (nctl || addr_stuck) { mr = ml = mu = 0; shared = 0; }
    if (mr) {
        bits_text(mr, bits, sizeof(bits));
        if (popcount8(mr) >= 3)
            printf("  %d. %s, the ROM: %d reads with bits %s wrong (the first at %04X): the chip, or its address lines or\n"
                   "     select (/ROMCS)\n", ++n, rom, e->nbad[AR_ROM], bits, e->first[AR_ROM]);
        else
            for (int i = 0; i < 8; i++)
                if (mr >> i & 1)
                    printf("  %d. %s, the ROM: its D%d %s (%d read%s, the first at %04X)\n", ++n, rom, i, bits_way(e, AR_ROM, i),
                           e->bad[AR_ROM][i], e->bad[AR_ROM][i] == 1 ? "" : "s", e->first[AR_ROM]);
    }
    /* The lower RAM wrong only where the ULA was fetching the screen: its
     * timing (contention), whichever chips show it. */
    bool timing = e->c_have && e->c_bad >= 2 && e->c_bad_win == e->c_bad && e->c_good > e->c_good_win && ml;
    if (timing) {
        bits_text(ml, bits, sizeof(bits));
        printf("  %d. %s, the ULA: its timing with the CPU (contention). All %d wrong bytes of the lower RAM were written\n"
               "     or read while the ULA was fetching the screen; of the %d that read back right (with those bits), %d\n"
               "     %s outside it. A working ULA holds the CPU's clock then; this one lets their accesses clash. The bits\n"
               "     that lose (%s) are the chips that are slowest, which show it first:\n", ++n, ula ? ula : "the ULA", e->c_bad,
               e->c_good, e->c_good - e->c_good_win, e->c_good - e->c_good_win == 1 ? "was" : "were", bits);
    }

    for (int ar = AR_LRAM; ar <= AR_URAM; ar++) {
        if (!e->late_n[ar]) continue;
        const char *bank = ar == AR_LRAM ? "lower RAM" : "upper RAM", *chips = ar == AR_LRAM ? "LRAM" : "URAM";
        for (int i = 0; i < 8; i++) {
            if (!(e->late_bits[ar] >> i & 1)) continue;
            char lab[8];
            snprintf(lab, sizeof(lab), "%s%d", chips, i);
            const char *ic = ref_of(lab);
            printf("  %d. %s, the %s's bit D%d: the CPU read it otherwise than the analyser saw. The RAM test read %02X\n"
                   "     at %04X (as the analyser saw it) but wrote back %02X, one less than %02X: what the CPU got. So D%d\n"
                   "     changes late in the read: a slow or failing chip (or one at the edge of its timing), or one not\n"
                   "     there at all (its pin floats and reads either way).\n", ++n,
                   ic ? ic : "the chip", bank, i, e->late_seen[ar], e->late_addr[ar], e->late_wrote[ar], (e->late_wrote[ar] + 1) & 0xFF, i);
            if (ar == AR_LRAM) ml &= ~(1u << i); else mu &= ~(1u << i);
        }
    }
    for (int ar = AR_LRAM; ar <= AR_URAM; ar++) {
        unsigned mm = ar == AR_LRAM ? ml : mu;
        const char *bank = ar == AR_LRAM ? "lower RAM" : "upper RAM", *chips = ar == AR_LRAM ? "LRAM" : "URAM";
        const char *range = ar == AR_LRAM ? "4000-7FFF" : "8000-FFFF";
        if (!mm) continue;
        bits_text(mm, bits, sizeof(bits));
        if (popcount8(mm) >= 3 && !(timing && ar == AR_LRAM)) {
            bank_parts(bank, chips, buf, sizeof(buf));
            data_resistors(chips, res, sizeof(res));
            printf("  %d. The %s (%s) as a whole: bits %s wrong (the first at %04X). Not one chip: what all its\n"
                   "     chips share: %s%s%s\n", ++n, bank, range, bits, e->first[ar], buf[0] ? buf : "its address multiplexers and select",
                   res[0] ? "; and for the data, the resistors to the CPU's data lines: " : "", res);
            if (ar == AR_LRAM && !e->c_have)
                printf("     The ULA's timing with the CPU (contention) can make this too; it can't be told here (no /INT in the\n"
                       "     capture to place the ULA's screen fetch). Run again with --la-all: the later stages show more.\n");
            continue;
        }
        int reads = 0;
        for (int i = 0; i < 8; i++) {
            if (!(mm >> i & 1)) continue;
            char lab[8];
            snprintf(lab, sizeof(lab), "%s%d", chips, i);
            const char *ic = ref_of(lab);
            reads += e->bad[ar][i];
            if (timing && ar == AR_LRAM) {
                printf("     %s (bit D%d, %d wrong read%s)\n", ic ? ic : "the chip", i, e->bad[ar][i], e->bad[ar][i] == 1 ? "" : "s");
                continue;
            }
            printf("  %d. %s, the %s's bit D%d (%s): it %s (%d read%s, the first at %04X)\n", ++n, ic ? ic : "the chip", bank, i,
                   range, bits_way(e, ar, i), e->bad[ar][i], e->bad[ar][i] == 1 ? "" : "s", e->first[ar]);
        }
        if (timing && ar == AR_LRAM) continue;
        if (reads <= 2) {
            /* One byte wrong can be the bank's addressing as well as a chip. */
            bank_parts(bank, chips, buf, sizeof(buf));
            if (buf[0])
                printf("  %d. Or what the %s's chips share, if they are all addressed wrong (only %d wrong read%s to go on):\n"
                       "     %s\n", ++n, bank, reads, reads == 1 ? "" : "s", buf);
        }
    }
    if (ev_pramt_bad(e)) {
        printf("     The RAM test (the ROM's own, at the switch on) stopped at %04X, in the %s; a working one's goes to %04X.\n",
               e->pramt + 1, e->pramt + 1 >= 0x8000 ? "upper RAM" : "lower RAM", e->pramt_ref);
        if (e->test_addr >= 0)
            printf("     There it %s; a working one's reads 02, writes 01, reads 01, writes 00.\n", e->test_seq);
        if (!ml && !mu && e->test_addr < 0)
            printf("  %d. The %s, from %04X: which bit isn't in the capture (it holds the last %d us before the test ended)\n", ++n,
                   e->pramt + 1 >= 0x8000 ? "upper RAM" : "lower RAM", e->pramt + 1, 9000 * 1000 / 14285);
    }
    if (e->kb_all)
        printf("  %d. %d keyboard read%s (port FE) gave all five lines low at once: not keys, the read itself. The ULA\n"
               "     (%s) answers it: its port read, or its data pins D0-D4, or its timing with the CPU.\n", ++n, e->kb_all,
               e->kb_all == 1 ? "" : "s", ula ? ula : "the ULA");
    if (e->held[LA_HALT]) {
        bool above = n > 0;
        printf("  %d. Left running, the CPU halts (/HALT, %s pin 18, low all through): it ran into a HALT%s. So it\n"
               "     went astray before that%s.\n", ++n, cpu, e->slow_ms && !e->int_n ? " with no interrupt to wake it" : "",
               above ? ", from what's above" : ": the boot's stages show where");
    }
    if (!n) {
        if (idle_st)
            printf("  No one part shows: left running, the CPU was %s.\n", where && where[0] ? where : "somewhere else");
        else if (missing)
            printf("  No one part shows in the captures: %s never came. When it should have, the CPU was %s.\n"
                   "  So it went astray after the stage before passed: the captures are in the folder, and the log has them.\n",
                   stage, where && where[0] ? where : "somewhere else");
        else if (co && co->diverged) {
            char tx[32], ty[32];
            cycle_text(&co->a, tx, sizeof(tx));
            cycle_text(&co->b, ty, sizeof(ty));
            printf("  No one part shows: at %s the CPU went another way, %s where a working one does %s.\n"
                   "  Look at the cycles before it in the log: the byte it read last decides where it goes.\n", stage, tx, ty);
        }
    }
done:
    board_destroy(vboard);
    vboard = NULL;
    return n;
}

#ifdef _WIN32
#include <direct.h>
#define mkdir1(p) _mkdir(p)
#else
#include <sys/stat.h>
#define mkdir1(p) mkdir(p, 0777)
#endif

/* Before the boot: the Spectrum left running, no reset: a capture at once
 * (on A0 high, else low, which always comes), at 14 MHz (the bus) and at
 * 2 MHz (49 ms: /RESET and the frame interrupt, every 20 ms). With /RESET
 * broken the boot's stages can't trigger; this can. */
#define IDLE_SLOW 2000000
#define IDLE_MS 2000            /* the simulation: idle in BASIC from 1.8 s after power on */
static bool record_idle(const machine_t *m, const char *const *faults, int nfaults, la_capture_t *fast, la_capture_t *slow,
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

static bool low_all(const la_capture_t *c, int sg) {
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
static void slow_ev(ev_t *e, const la_capture_t *d, const la_capture_t *r) {
    e->slow_ms = d->freq > 0 ? (int)(1000.0 * d->n / d->freq + 0.5) : 0;
    static const int ctl[] = { LA_RESET, LA_WAIT, LA_BUSRQ, LA_INT, LA_HALT };
    for (int k = 0; k < 5; k++)
        if ((r->have >> ctl[k] & 1) && low_all(d, ctl[k]) && !low_all(r, ctl[k])) e->held[ctl[k]] = 1;
    if (e->held[LA_RESET]) e->in_reset = true;
    else e->rst_pulses = falls(d, LA_RESET);
    if ((d->have >> LA_INT & 1) && (r->have >> LA_INT & 1)) { e->int_n = falls(d, LA_INT); e->int_ref = falls(r, LA_INT); }
    else e->int_ref = 0;
}

/* Not running: the boot's stages can't be taken. */
static bool idle_dead(const ev_t *e) {
    int nstuck = 0;
    for (int k = 0; k < LA_NSIG; k++) nstuck += e->stuck[k] != 0;
    return e->in_reset || e->rst_pulses || e->stuck[LA_CLK] || (e->clk_pct >= 0 && e->clk_pct < 30) ||
           e->held[LA_WAIT] || e->held[LA_BUSRQ] || nstuck >= 8;
}

/* 0: as a working one, 1: something wrong, 2: not running, -1: none. */
static int idle_stage(const la_cli_t *o, const machine_t *m, const char *dir, ev_t *ie) {
    char fpath[1024], spath[1024], err[512];
    snprintf(fpath, sizeof(fpath), "%s/idle.lac", dir);
    snprintf(spath, sizeof(spath), "%s/idle-slow.lac", dir);
    ev_init(ie);
    printf("\nFirst, idle: what it does left running, before any reset (the clock, /RESET, the frame interrupt, the CPU)\n");
    la_capture_t f, sl;
    bool have_slow = false;
    if (o->replay) {
        if (!la_read(fpath, &f, err, sizeof(err))) {
            printf("  Not in the folder (taken before there was this stage): on to the boot\n");
            return -1;
        }
        have_slow = la_read(spath, &sl, err, sizeof(err));
    } else if (o->simulate) {
        if (!record_idle(m, o->faults, o->nfaults, &f, &sl, err, sizeof(err))) { la_err(err); return -1; }
        have_slow = true;
        if (!la_write(fpath, &f, err, sizeof(err)) || !la_write(spath, &sl, err, sizeof(err))) la_err(err);
    } else {
        la_cli_t q = *o;
        q.diagnose = NULL;
        bool reuse = false;
        if (la_read(fpath, &f, err, sizeof(err))) {
            have_slow = la_read(spath, &sl, err, sizeof(err));
            reuse = want_enter("idle.lac is in the folder from before: press Enter to use it, or type n, Enter to capture it again\n");
            if (!reuse) { la_free(&f); if (have_slow) la_free(&sl); have_slow = false; }
        }
        for (int t = 0; !reuse; t++) {
            if (t == 0 && !want_enter("Leave the Spectrum switched on and running, don't touch the reset button or the keys, and press Enter\n"))
                return -1;
            if (!capture_now(&q, 0, fpath, &f, err, sizeof(err))) {
                la_err(err);
                printf("  Couldn't capture it: on to the boot\n");
                return -1;
            }
            printf("Now the same for 49 ms, slower (the frame interrupt is every 20 ms)\n");
            q.freq = IDLE_SLOW;
            have_slow = capture_now(&q, 0, spath, &sl, err, sizeof(err));
            q.freq = o->freq;
            if (!have_slow) { la_err(err); printf("  (without it: not the frame interrupt)\n"); }
            if (!la_write(fpath, &f, err, sizeof(err))) la_err(err);
            if (have_slow && !la_write(spath, &sl, err, sizeof(err))) la_err(err);
            bool rlow = (f.have >> LA_RESET & 1) && low_all(&f, LA_RESET) && (!have_slow || low_all(&sl, LA_RESET));
            if (t == 0 && rlow &&
                want_enter("/RESET was low all through: was the reset button held? Let go of it and press Enter to take it again\n"
                           "(or type n, Enter to go on with this one)\n")) {
                la_free(&f);
                if (have_slow) la_free(&sl);
                have_slow = false;
                continue;
            }
            break;
        }
    }
    la_capture_t rf, rs;
    if (!record_idle(m, NULL, 0, &rf, &rs, err, sizeof(err))) {
        la_err(err);
        la_free(&f);
        if (have_slow) la_free(&sl);
        return -1;
    }
    gather(ie, &f, &rf);
    where_text(&f, ie->idle_where, sizeof(ie->idle_where));
    if (have_slow) slow_ev(ie, &sl, &rs);
    ie->idle = 1;
    la_free(&f); la_free(&rf); la_free(&rs);
    if (have_slow) la_free(&sl);
    /* What it shows. */
    if (ie->stuck[LA_CLK]) printf("  CLK: stuck %s\n", ie->stuck[LA_CLK] == 1 ? "low" : "high");
    else if (ie->clk_pct >= 0) printf("  CLK: running (the analyser sees %d%% of its pulses)\n", ie->clk_pct);
    if (ie->in_reset) printf("  /RESET: low all through\n");
    else if (ie->rst_pulses) printf("  /RESET: goes low %d time%s in %d ms\n", ie->rst_pulses, ie->rst_pulses == 1 ? "" : "s", ie->slow_ms);
    else printf("  /RESET: high\n");
    if (ie->held[LA_INT]) printf("  /INT: low all through\n");
    else if (ie->slow_ms) printf("  /INT: low %d time%s in %d ms (a working one's: %d, every 20 ms)\n", ie->int_n, ie->int_n == 1 ? "" : "s",
                                 ie->slow_ms, ie->int_ref);
    for (int k = 0; k < LA_NSIG; k++)
        if (ie->held[k] && k != LA_RESET && k != LA_INT) printf("  %s: low all through\n", la_signal_name(k));
    printf("  The CPU: %s\n", ie->idle_where);
    bool dead = idle_dead(ie), bad = dead || ev_any(ie);
    if (!bad && (strstr(ie->idle_where, "astray") || strstr(ie->idle_where, "not running"))) bad = true;
    if (!bad) { printf("  OK: the same as a working one\n"); return 0; }
    if (dead) { printf("  FAULT: it isn't running, so the boot's stages can't be taken (they need a reset and a running CPU)\n"); return 2; }
    printf("  FAULT: something's wrong (below); the boot's stages next show more\n");
    return 1;
}

#define STOP_MSG "  Couldn't capture it: the diagnosis goes on what the stages before show\n"
static int diagnose(const la_cli_t *o) {
    const machine_t *m = o->machine ? o->machine : &machine_zx48;
    const char *dir = o->diagnose;
    char path[1024], err[512], bn[300];
    mkdir1(dir);
    if (!la_logf) {
        snprintf(path, sizeof(path), "%s/diagnosis.txt", dir);
        la_logf = fopen(path, "w");
    }
    const char *how = o->replay ? "from the captures in the folder" :
                      o->simulate ? "of the simulated board" : "with the logic analyser";
    printf("Diagnosing a %s, stage by stage, %s\n", m->board_name, how);
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
    int idle = idle_stage(o, m, dir, &ie);
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
        printf("\n%d of %d, %s: %s\n", i + 1, DIAG_NSTAGES, st->name, st->what);
        double at = i == 0 ? 0 : st->ms + 5;    /* when it's past due */
        if (o->replay) {
            got = la_read(path, &d, err, sizeof(err));
            if (!got) instead = la_read(ipath, &d, err, sizeof(err));
            if (!got && !instead) {
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
            /* Captured on an earlier run into the same folder: use it again? */
            la_capture_t old;
            if (la_read(path, &old, err, sizeof(err))) {
                char msg[200];
                snprintf(msg, sizeof(msg), "%s.lac is in the folder from before: press Enter to use it, or type n, Enter to capture "
                         "it again\n", st->name);
                if (want_enter(msg)) { d = old; got = true; }
                else la_free(&old);
            }
            if (!got && boot && !want_enter("Hold the Spectrum's reset button down, then press Enter here (keep holding the button)\n")) {
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
        if (got && boot && !o->replay && !o->simulate && (d.have >> LA_RESET & 1) && line_state(&d, LA_RESET) == 1 &&
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
            else if (sco.diverged) printf("  Wrong too: the CPU went another way (cycle %d)\n", sco.cycle);
            else printf("  Wrong too: it got there, but the bus shows something wrong\n");
            if (instead) break;
            continue;
        }
        e = se;
        co = sco;
        fail_stage = st->name;
        missing = instead;
        if (instead) printf("  It never came. When it should have, the CPU was %s\n", where);
        else if (sco.diverged) printf("  FAULT: the CPU went another way (cycle %d)\n", sco.cycle);
        else printf("  FAULT: it got there, but the bus shows something wrong\n");
        /* --la-all: on to the rest (not after a stage that never came:
         * the later ones won't either). */
        if (!o->all || instead) break;
        printf("  (--la-all: capturing the rest as well)\n");
    }
    printf("\n");
    if (idle == 1) {
        if (!fail_stage) { e = ie; fail_stage = "idle"; snprintf(where, sizeof(where), "%s", ie.idle_where); }
        else ev_merge(&e, &ie);
    }
    if (!fail_stage) {
        if (idle == 0) printf("Left running before the boot, it does as a working one: the clock, /RESET and the frame interrupt.\n");
        if (passed == DIAG_NSTAGES) {
            vboard = board_create(m);
            const char *u = ref_of("ULA");
            char ula[32];
            snprintf(ula, sizeof(ula), "the ULA (%s)", u ? u : "IC1");
            board_destroy(vboard);
            vboard = NULL;
            printf("All %d stages are the same as a working %s: the CPU, the ROM, all of the RAM (the ROM's RAM test)\n"
                   "and the start of BASIC are good. This doesn't test the picture, the keyboard, the sound or the tape.\n"
                   "If the picture is wrong all the same (the copyright message garbled, wrong colours): the CPU wrote\n"
                   "the screen right and the RAM keeps it, so it's the ULA's video side: %s reads the screen over its own\n"
                   "address and data lines to the lower RAM, which the edge connector doesn't show. Diagnose a\n"
                   "screenshot of it: the atlas has the ULA's outputs shorted and its pins bridged, though not every\n"
                   "fault inside the chip.\n",
                   passed, m->board_name, ula);
        }
        return 0;
    }
    if (!strcmp(fail_stage, "idle") && idle == 2)
        printf("Left running, it isn't working, so the boot's stages weren't taken.\n");
    else if (!strcmp(fail_stage, "idle"))
        printf("The boot's %d stages are the same as a working one's, but left running it isn't.\n", passed);
    else
        printf("The boot goes wrong at %s (%d stage%s before it %s good).\n", fail_stage, passed, passed == 1 ? "" : "s",
               passed == 1 ? "is" : "are");
    verdict(&e, m, fail_stage, missing, &co, where);
    printf("\nThe captures and the whole comparison: %s (diagnosis.txt)\n", dir);
    return 2;
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
