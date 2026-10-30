/* atlas_match.c - matching a picture of a real machine against the fault
 * atlas (see atlas.h): reading the text on a screen, the labels of the
 * outcomes, loading and normalizing a capture card picture, the
 * similarity score, --match and the synthetic capture card test.
 *
 * Everything here works on the THUMBNAIL picture of the atlas: palette
 * indices at half the machine's resolution, so a character cell is 4x4.
 * A captured picture is brought to the machine's full resolution and
 * palette first, then halved the same way the thumbnails were. */
#include "atlas.h"
#include "board.h"
#include "probe_db.h"
#include "c64.h"
#include "zx48.h"
#include "png.h"
#include "stb/stb_image.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0777)
#endif

static void appendf(char *buf, int len, const char *fmt, ...) {
    int at = (int)strlen(buf);
    if (at >= len - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + at, (size_t)(len - at), fmt, ap);
    va_end(ap);
}

static int popcount16(unsigned v) { return __builtin_popcount(v & 0xFFFF); }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The thumbnail: size and active display (half of the machine's). */
typedef struct { int w, h, ax, ay, aw, ah, cw, ch; } layout_t;

static layout_t layout(int machine) {
    layout_t L;
    int x, y, w, h, sw, sh;
    atlas_active_area(machine, &x, &y, &w, &h);
    atlas_screen_size(machine, &sw, &sh);
    L.w = sw / 2; L.h = sh / 2;
    L.ax = x / 2; L.ay = y / 2; L.aw = w / 2; L.ah = h / 2;
    L.cw = w / 8; L.ch = h / 8;
    return L;
}

/* ------------------------------------------------------------------------ */
/* Reading the text on a screen                                              */
/* ------------------------------------------------------------------------ */

/* A glyph at half size: a 4x4 mask, bit (y * 4 + x), of the 2x2 blocks
 * the thumbnail draws in the ink color. Which color a 2x2 block gets
 * depends on which one is the background of the whole picture (a tie
 * goes to the other one), so every glyph has two masks: ink on a tie
 * (two pixels of four are enough) and paper on a tie (three needed). */
enum { CLS_LETTER, CLS_DIGIT, CLS_PUNCT, CLS_GRAPHIC };
typedef struct { uint16_t m2, m3; char ch; uint8_t cls; } glyph_t;
typedef struct { glyph_t g[128]; int n; } font_t;
enum { FONT_C64, FONT_DEADTEST, FONT_ZX, FONT_ZXDIAG, NFONTS };
static font_t fonts[NFONTS];
static bool fonts_ready;
static char carts_dir[512] = "carts";

void atlas_set_carts(const char *dir) { snprintf(carts_dir, sizeof(carts_dir), "%s", dir ? dir : "carts"); }

static uint16_t half_mask(const uint8_t *g, int need) {
    uint16_t m = 0;
    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++) {
            int n = 0;
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 2; x++) n += (g[by * 2 + y] >> (7 - bx * 2 - x)) & 1;
            if (n >= need) m |= (uint16_t)(1u << (by * 4 + bx));
        }
    return m;
}

static int char_class(int c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return CLS_LETTER;
    if (c >= '0' && c <= '9') return CLS_DIGIT;
    return CLS_PUNCT;
}

static void add_glyph(font_t *f, const uint8_t *g, char ch, int cls) {
    glyph_t *G = &f->g[f->n++];
    G->m2 = half_mask(g, 2);
    G->m3 = half_mask(g, 3);
    G->ch = ch;
    G->cls = (uint8_t)cls;
}

/* An ASCII font of characters 32-127 (the Spectrum's ROMs). 127 is the
 * copyright sign, kept as 0x7F. */
static void ascii_font(font_t *f, const uint8_t *data) {
    for (int c = 33; c < 128; c++) add_glyph(f, data + (c - 32) * 8, (char)c, c == 127 ? CLS_PUNCT : char_class(c));
}

/* A C64 font in screen code order: 1-26 the letters, 32-63 as ASCII,
 * graphic characters from GRAPHICS on. */
static void screen_code_font(font_t *f, const uint8_t *data, int n, int graphics) {
    for (int sc = 1; sc < n; sc++) {
        if (sc == 32) continue;
        char ch = sc <= 26 ? (char)('A' + sc - 1) : sc == 27 ? '[' : sc == 28 ? '#' : sc == 29 ? ']' : sc == 30 ? '^'
                : sc == 31 ? '<' : (char)sc;
        if (sc >= graphics) add_glyph(f, data + sc * 8, '~', CLS_GRAPHIC);
        else add_glyph(f, data + sc * 8, ch, char_class(ch));
    }
    add_glyph(f, data, '@', CLS_PUNCT);
}

/* N bytes at OFFSET of a cartridge file, if it is there. */
static bool cart_bytes(const char *name, long offset, uint8_t *out, int n) {
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", carts_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = !fseek(f, offset, SEEK_SET) && fread(out, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static void init_fonts(void) {
    if (fonts_ready) return;
    fonts_ready = true;
    /* C64: screen codes 0-127 of the upper case set (128-255 are the same
     * reversed). 64-127 are the graphic characters. */
    screen_code_font(&fonts[FONT_C64], c64_char_rom(), 128, 64);
    /* Dead Test draws with a font of its own (the VIC reads it from the
     * cartridge, in Ultimax mode): screen codes 0-57 at 0xAD8 of its ROM,
     * after the 80 bytes of CRT headers; 58-63 are test patterns. */
    static uint8_t dt[64 * 8];
    if (cart_bytes("deadtest-781220.crt", 80 + 0xAD8, dt, sizeof(dt)) && dt[3 * 8] == 0x7E && dt[3 * 8 + 1] == 0x42)
        screen_code_font(&fonts[FONT_DEADTEST], dt, 64, 58);
    ascii_font(&fonts[FONT_ZX], zx48_rom_image() + 0x3D00);
    /* The ZX Diagnostics ROM has an 8x8 font of its own at 0x3C00 (its
     * reports use a narrow one, too small to read on a thumbnail). */
    static uint8_t zd[96 * 8];
    if (cart_bytes("zx-diagnostics-v0.37.rom", 0x3C00, zd, sizeof(zd))) ascii_font(&fonts[FONT_ZXDIAG], zd);
}

/* A character cell read: what it shows, as a letter (and as a digit when
 * a digit looks the same at half size: 0/O, 1/I, 6/G...). */
typedef struct {
    char ch, digit;             /* 0: nothing matched; ' ' a plain cell */
    uint8_t dist;
} cell_read_t;

typedef struct {
    int cw, ch;
    uint8_t bg;                 /* of the active display */
    cell_read_t c[ATLAS_CELLS_H][ATLAS_CELLS_W];
} screen_read_t;

static int glyph_dist(const glyph_t *g, uint16_t m) {
    uint16_t n = (uint16_t)~m;
    int d = popcount16(m ^ g->m2), e;
    if ((e = popcount16(m ^ g->m3)) < d) d = e;
    if ((e = popcount16(n ^ g->m2)) < d) d = e;
    if ((e = popcount16(n ^ g->m3)) < d) d = e;
    return d;
}

static void read_cell(int machine, int mode, uint16_t m, int maxd, cell_read_t *r) {
    r->ch = r->digit = 0;
    r->dist = 255;
    if (!m) { r->ch = r->digit = ' '; r->dist = 0; return; }
    int fl[2] = { atlas_family(machine) == ATLAS_C64 ? FONT_C64 : mode == MODE_ZXDIAG ? FONT_ZXDIAG : FONT_ZX,
                  mode == MODE_DEADTEST ? FONT_DEADTEST : mode == MODE_ZXDIAG ? FONT_ZX : -1 };
    int best = 99, bestcls = 9, bestdig = 99;
    for (int k = 0; k < 2; k++) {
        if (fl[k] < 0) continue;
        const font_t *f = &fonts[fl[k]];
        for (int i = 0; i < f->n; i++) {
            const glyph_t *g = &f->g[i];
            int d = glyph_dist(g, m);
            /* The same distance: a letter rather than a digit rather than
             * punctuation rather than a graphic character. */
            if (d < best || (d == best && g->cls < bestcls)) { best = d; bestcls = g->cls; r->ch = g->ch; }
            if (g->cls == CLS_DIGIT && d < bestdig) { bestdig = d; r->digit = g->ch; }
        }
    }
    if (best > maxd) { r->ch = r->digit = 0; return; }
    r->dist = (uint8_t)best;
    if (bestdig > best) r->digit = r->ch;
}

static int argmax_u32(const uint32_t *v, int n) {
    int b = 0;
    for (int i = 1; i < n; i++) if (v[i] > v[b]) b = i;
    return b;
}

/* Read every character cell of a thumbnail. A cell's ink is what differs
 * from its paper: the background of the screen if the cell has some,
 * else its most frequent color. */
static void read_screen(int machine, int mode, const uint8_t *img, int maxd, screen_read_t *s) {
    init_fonts();
    layout_t L = layout(machine);
    uint32_t hist[16] = { 0 };
    for (int y = 0; y < L.ah; y++)
        for (int x = 0; x < L.aw; x++) hist[img[(L.ay + y) * L.w + L.ax + x]]++;
    s->bg = (uint8_t)argmax_u32(hist, 16);
    s->cw = L.cw;
    s->ch = L.ch;
    for (int cy = 0; cy < L.ch; cy++)
        for (int cx = 0; cx < L.cw; cx++) {
            const uint8_t *p = &img[(L.ay + cy * 4) * L.w + L.ax + cx * 4];
            uint32_t ch[16] = { 0 };
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) ch[p[y * L.w + x]]++;
            int paper = ch[s->bg] ? s->bg : argmax_u32(ch, 16);
            uint16_t m = 0;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    if (p[y * L.w + x] != paper) m |= (uint16_t)(1u << (y * 4 + x));
            read_cell(machine, mode, m, maxd, &s->c[cy][cx]);
        }
}

/* The lines of text of a screen read, as a person would copy them: a
 * word that is mostly digits is read as digits, cells that match no
 * character are '?', graphic characters '~'. Only lines with at least
 * three letters or digits and not much else. */
static int text_lines(const screen_read_t *s, char *out, int len) {
    out[0] = 0;
    int total = 0;
    for (int cy = 0; cy < s->ch; cy++) {
        char line[ATLAS_CELLS_W * 3 + 1];
        int n = 0, alnum = 0, junk = 0, other = 0;
        for (int cx = 0; cx < s->cw; ) {
            const cell_read_t *c = &s->c[cy][cx];
            if (c->ch == ' ') { line[n++] = ' '; cx++; continue; }
            int end = cx, digits = 0, cells = 0;
            while (end < s->cw && s->c[cy][end].ch != ' ') {
                const cell_read_t *e = &s->c[cy][end];
                cells++;
                digits += e->digit >= '0' && e->digit <= '9';
                end++;
            }
            bool numeric = 2 * digits >= cells;
            for (; cx < end; cx++) {
                const cell_read_t *e = &s->c[cy][cx];
                char ch = !e->ch ? '?' : numeric && e->digit >= '0' && e->digit <= '9' ? e->digit : e->ch;
                if (ch == 0x7F) { line[n++] = '('; line[n++] = 'c'; ch = ')'; }
                line[n++] = ch;
                if (ch == '?' || ch == '~') junk++;
                else if (char_class(ch) != CLS_PUNCT) alnum++;
                else other++;
            }
        }
        while (n && line[n - 1] == ' ') n--;
        line[n] = 0;
        int lead = 0;
        while (line[lead] == ' ') lead++;
        if (alnum < 3 || 3 * junk > alnum + other + junk) continue;
        appendf(out, len, "%s%s", out[0] ? "\n" : "", line + lead);
        total += alnum;
    }
    return total;
}

int atlas_read_text(int machine, int mode, const uint8_t *thumb, char *out, int len) {
    screen_read_t s;
    read_screen(machine, mode, thumb, 0, &s);
    return text_lines(&s, out, len);
}

/* ------------------------------------------------------------------------ */
/* Labels                                                                    */
/* ------------------------------------------------------------------------ */

/* Dead Test's flash codes, from its manual: the bad RAM chip. */
static const char *const deadtest_chip[9] = { "", "U12", "U24", "U11", "U23", "U10", "U22", "U9", "U21" };

/* Case-insensitive search. */
static const char *find_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (!strncasecmp(p, needle, n)) return p;
    return NULL;
}

/* NEEDLE in the text read, a letter in four allowed to differ (at half
 * size Y reads as X, T as R...), within a line, not across lines. */
static const char *find_fuzzy(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    int letters = 0;
    for (size_t i = 0; i < n; i++) letters += needle[i] != ' ';
    for (const char *p = hay; *p; p++) {
        int bad = 0;
        size_t i;
        for (i = 0; i < n && p[i] && p[i] != '\n'; i++) {
            bool same = toupper((unsigned char)p[i]) == toupper((unsigned char)needle[i]);
            if (!same && (needle[i] == ' ' || p[i] == ' ' || ++bad > letters / 4)) break;
        }
        if (i == n) return p;
    }
    return NULL;
}

/* The line of TEXT around P, in BUF. */
static void line_at(const char *text, const char *p, char *buf, int len) {
    const char *a = p, *b = p;
    while (a > text && a[-1] != '\n') a--;
    while (*b && *b != '\n') b++;
    snprintf(buf, (size_t)len, "%.*s", (int)(b - a), a);
}

/* The colors the text of a screen is in: how many different ones color
 * at least three character cells (the C64 without its color RAM writes in
 * every color). */
static int ink_colors(int machine, const uint8_t *img) {
    layout_t L = layout(machine);
    uint32_t ah[16] = { 0 }, cells[16] = { 0 };
    for (int y = 0; y < L.ah; y++)
        for (int x = 0; x < L.aw; x++) ah[img[(L.ay + y) * L.w + L.ax + x]]++;
    int bg = argmax_u32(ah, 16), n = 0;
    for (int cy = 0; cy < L.ch; cy++)
        for (int cx = 0; cx < L.cw; cx++) {
            bool seen[16] = { false };
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    int v = img[(L.ay + cy * 4 + y) * L.w + L.ax + cx * 4 + x];
                    if (v != bg && !seen[v]) { seen[v] = true; cells[v]++; }
                }
        }
    for (int i = 0; i < 16; i++) n += cells[i] >= 3;
    return n;
}

/* The ZX Diagnostics report: the narrow print can't be read on a
 * thumbnail, but each line ends with its verdict, PASS in green or FAIL
 * in red, in the last cells of the line. */
static void diag_verdicts(const uint8_t *img, int *pass, int *fail) {
    layout_t L = layout(ATLAS_ZX48);
    *pass = *fail = 0;
    /* The report is printed on white paper. */
    uint32_t ah[16] = { 0 };
    for (int y = 0; y < L.ah; y++)
        for (int x = 0; x < L.aw; x++) ah[img[(L.ay + y) * L.w + L.ax + x]]++;
    if ((argmax_u32(ah, 16) & 7) != 7) return;
    for (int cy = 0; cy < L.ch; cy++) {
        int green = 0, red = 0;
        for (int y = 0; y < 4; y++)
            for (int x = L.aw - 6 * 4; x < L.aw; x++) {
                int v = img[(L.ay + cy * 4 + y) * L.w + L.ax + x] & 7;
                green += v == 4;
                red += v == 2;
            }
        /* A verdict is a few strokes, not a filled block. */
        if (green >= 4 && green <= 40 && red < 4) (*pass)++;
        if (red >= 4 && red <= 40 && green < 4) (*fail)++;
    }
}

/* What the text says, for people; false if nothing known. */
static bool text_headline(int machine, int mode, const char *text, const uint8_t *img, char *buf, int len) {
    char line[160];
    const char *p;
    if (atlas_family(machine) == ATLAS_C64) {
        if (find_fuzzy(text, "OUT OF MEMORY")) { appendf(buf, len, "?OUT OF MEMORY ERROR"); return true; }
        if (find_fuzzy(text, "DEAD TEST")) {
            appendf(buf, len, "Dead Test results screen");
            if ((p = find_ci(text, "BAD"))) {
                line_at(text, p, line, sizeof(line));
                appendf(buf, len, ": \"%s\"", line);
            } else appendf(buf, len, ", nothing reported BAD");
            return true;
        }
        bool ready = find_fuzzy(text, "READY") != NULL;
        if ((p = find_fuzzy(text, "BYTES FREE"))) {
            /* The number before "BASIC BYTES FREE". */
            const char *q = p;
            while (q > text && q[-1] != '\n' && !(q[-1] >= '0' && q[-1] <= '9')) q--;
            const char *d = q;
            while (d > text && d[-1] >= '0' && d[-1] <= '9') d--;
            appendf(buf, len, "%s", ready ? "BASIC READY screen" : "BASIC banner, no READY prompt");
            if (q > d) {
                appendf(buf, len, ", %.*s BYTES FREE", (int)(q - d), d);
                if ((int)(q - d) != 5 || strncmp(d, "38911", 5)) appendf(buf, len, " (a healthy C64 says 38911)");
            }
        } else if (ready) appendf(buf, len, "READY prompt, no BASIC banner");
        else if (find_fuzzy(text, "COMMODORE")) appendf(buf, len, "BASIC banner, no READY prompt");
        else if ((p = find_fuzzy(text, "ERROR"))) {
            line_at(text, p, line, sizeof(line));
            appendf(buf, len, "BASIC error: \"%s\"", line);
        } else return false;
        if (ink_colors(machine, img) >= 4) appendf(buf, len, ", text in random colors");
        return true;
    }
    if (mode == MODE_ZXDIAG) {
        int pass, fail;
        diag_verdicts(img, &pass, &fail);
        if (!pass && !fail) return false;
        appendf(buf, len, "ZX Diagnostics report: ");
        if (fail) appendf(buf, len, "%d test%s FAIL (red), %d PASS", fail, fail == 1 ? "" : "s", pass);
        else appendf(buf, len, "every test PASS (%d)", pass);
        return true;
    }
    if (find_fuzzy(text, "Sinclair") || find_fuzzy(text, "Research") || find_ci(text, "1982")) {
        appendf(buf, len, "Sinclair copyright message (it booted)");
        return true;
    }
    return false;
}

/* Bars: the runs of columns (or rows) of the active display that are
 * mostly not the background. In BUF how many, how wide and how far
 * apart, in the machine's pixels; false if there are none. */
static bool bar_runs(const uint8_t *img, layout_t L, int bg, bool vertical, char *buf, int len) {
    int n = vertical ? L.aw : L.ah, other = vertical ? L.ah : L.aw;
    int runs = 0, width = 0, first = -1, last = -1;
    bool in = false;
    for (int i = 0; i < n; i++) {
        int k = 0;
        for (int j = 0; j < other; j++)
            k += img[(L.ay + (vertical ? j : i)) * L.w + L.ax + (vertical ? i : j)] != bg;
        bool on = 2 * k > other;
        if (on && !in) { runs++; if (first < 0) first = i; last = i; }
        width += on;
        in = on;
    }
    if (!runs) return false;
    buf[0] = 0;
    /* Thumbnail pixels are two of the machine's. */
    if (runs == 1) appendf(buf, len, "one, %d pixels wide", 2 * width);
    else appendf(buf, len, "%d of them, %d pixels wide, every %d pixels", runs, 2 * width / runs,
                 2 * (last - first) / (runs - 1));
    return true;
}

/* What the picture looks like, without text. */
static int structure_label(const atlas_outcome_t *o, const uint8_t *img, char *buf, int len) {
    int m = o->h.machine;
    layout_t L = layout(m);
    screen_read_t s;
    read_screen(m, o->h.mode, img, 0, &s);
    uint32_t bh[16] = { 0 }, ah[16] = { 0 };
    for (int y = 0; y < L.h; y++)
        for (int x = 0; x < L.w; x++) {
            bool in = x >= L.ax && x < L.ax + L.aw && y >= L.ay && y < L.ay + L.ah;
            (in ? ah : bh)[img[y * L.w + x]]++;
        }
    int bd = argmax_u32(bh, 16), ad = argmax_u32(ah, 16);
    int cells = L.cw * L.ch, drawn = 0, glyphs = 0, pairs = 0;
    bool seen_pair[256] = { 0 };
    uint16_t masks[ATLAS_CELLS_H * ATLAS_CELLS_W];
    for (int cy = 0; cy < L.ch; cy++)
        for (int cx = 0; cx < L.cw; cx++) {
            const uint8_t *p = &img[(L.ay + cy * 4) * L.w + L.ax + cx * 4];
            uint32_t ch[16] = { 0 };
            uint16_t mk = 0;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    ch[p[y * L.w + x]]++;
                    if (p[y * L.w + x] != ad) mk |= (uint16_t)(1u << (y * 4 + x));
                }
            int paper = argmax_u32(ch, 16), ink = paper;
            for (int i = 0; i < 16; i++) if (i != paper && ch[i] && (ink == paper || ch[i] > ch[ink])) ink = i;
            if (!seen_pair[paper << 4 | ink]) { seen_pair[paper << 4 | ink] = true; pairs++; }
            if (mk) {
                masks[drawn++] = mk;
                glyphs += s.c[cy][cx].ch && s.c[cy][cx].ch != ' ' && s.c[cy][cx].ch != '~';
            }
        }
    /* How alike neighbor pixels of the active display are, across and down. */
    long same_r = 0, same_d = 0, n_r = 0, n_d = 0;
    for (int y = 0; y < L.ah; y++)
        for (int x = 0; x < L.aw; x++) {
            uint8_t v = img[(L.ay + y) * L.w + L.ax + x];
            if (x + 1 < L.aw) { same_r += v == img[(L.ay + y) * L.w + L.ax + x + 1]; n_r++; }
            if (y + 1 < L.ah) { same_d += v == img[(L.ay + y + 1) * L.w + L.ax + x]; n_d++; }
        }
    double fr = n_r ? (double)same_r / n_r : 1, fd = n_d ? (double)same_d / n_d : 1;
    int pct = (100 * drawn + cells / 2) / cells;
    /* The most common drawn cell. */
    int common = 0;
    for (int i = 0; i < drawn && i < 200; i++) {
        int k = 0;
        for (int j = 0; j < drawn; j++) k += masks[j] == masks[i];
        if (k > common) common = k;
    }
    const char *bn = atlas_color_name(m, bd), *an = atlas_color_name(m, ad);
    bool uniform_border = bh[bd] >= 0.97 * (double)(L.w * L.h - L.aw * L.ah);
    if (!drawn) {
        if (bd == ad && uniform_border) appendf(buf, len, "Solid %s screen", bn);
        else appendf(buf, len, "Blank %s screen, %s border", an, bn);
        return 0;
    }
    char bars[96];
    if (drawn * 10 >= cells * 3 && fd > 0.97 && bar_runs(img, L, ad, true, bars, sizeof(bars)))
        appendf(buf, len, "Vertical bars: %s", bars);
    else if (drawn * 10 >= cells * 3 && fr > 0.97 && bar_runs(img, L, ad, false, bars, sizeof(bars)))
        appendf(buf, len, "Horizontal stripes: %s", bars);
    else if (atlas_family(m) == ATLAS_ZX48 && drawn * 2 >= cells && pairs >= 12)
        appendf(buf, len, "Attribute garbage: random color blocks over %d%% of the screen", pct);
    else if (drawn * 2 >= cells && common * 5 >= drawn * 2)
        appendf(buf, len, "Repeating pattern over %d%% of the screen", pct);
    else if (drawn * 10 >= cells * 3)
        appendf(buf, len, "%s over %d%% of the screen", glyphs * 5 >= drawn * 3 ? "Random characters" : "Garbled character cells", pct);
    else appendf(buf, len, "%s on %d%% of the cells", glyphs * 5 >= drawn * 3 ? "Unreadable characters" : "Garbage", pct ? pct : 1);
    appendf(buf, len, " (%s border, %s screen)", bn, an);
    return pct;
}

void atlas_outcome_label(const atlas_outcome_t *o, char *buf, int len) {
    const atlas_outcome_hdr_t *h = &o->h;
    buf[0] = 0;
    if (h->t.no_signal) { appendf(buf, len, "No video signal (snow)"); return; }
    if (h->t.nseries) {
        int n = h->t.series[0];
        appendf(buf, len, "%s: %d flash%s", h->mode == MODE_DEADTEST ? "Dead Test" : "Flash code", n, n == 1 ? "" : "es");
        if (h->mode == MODE_DEADTEST && n >= 1 && n <= 8) appendf(buf, len, " = bad RAM chip %s", deadtest_chip[n]);
        if (h->t.nseries >= 2) appendf(buf, len, ", repeating");
        return;
    }
    int n = h->w * h->h;
    uint8_t *img = malloc((size_t)n);
    if (!atlas_decode_thumb(o, false, img)) { free(img); appendf(buf, len, "(no picture)"); return; }
    char text[2048];
    atlas_read_text(h->machine, h->mode, img, text, sizeof(text));
    if (!text_headline(h->machine, h->mode, text, img, buf, len)) {
        /* Text, unless it is just what garbage happens to look like. */
        if (structure_label(o, img, buf, len) < 30 && text[0]) {
            char line[120];
            line_at(text, text, line, sizeof(line));
            appendf(buf, len, ", text \"%s\"", line);
        }
    }
    free(img);
    if (h->t.distinct > 4) appendf(buf, len, ", animated");
    else if (h->t.distinct > 1 && h->t.blink_period >= 8 && h->t.blink_period <= 50)
        appendf(buf, len, ", blinking (%d frames)", h->t.blink_period);
    else if (h->t.distinct > 1) appendf(buf, len, ", changing");
}

/* ------------------------------------------------------------------------ */
/* Colors                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct { float r, g, b; } rgb_t;
typedef struct { float L, a, b; } lab_t;

static float lin_lut[256];
static bool lut_ready;

static float srgb_lin(float c) {
    int i = (int)(c + 0.5f);
    return lin_lut[i < 0 ? 0 : i > 255 ? 255 : i];
}

static float lab_f(float t) { return t > 0.008856f ? cbrtf(t) : 7.787f * t + 16.0f / 116.0f; }

static lab_t to_lab(rgb_t c) {
    float R = srgb_lin(c.r), G = srgb_lin(c.g), B = srgb_lin(c.b);
    float X = (0.4124f * R + 0.3576f * G + 0.1805f * B) / 0.95047f;
    float Y = 0.2126f * R + 0.7152f * G + 0.0722f * B;
    float Z = (0.0193f * R + 0.1192f * G + 0.9505f * B) / 1.08883f;
    float fx = lab_f(X), fy = lab_f(Y), fz = lab_f(Z);
    return (lab_t){ 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz) };
}

static float lab_d(lab_t x, lab_t y) {
    float dl = x.L - y.L, da = x.a - y.a, db = x.b - y.b;
    return sqrtf(dl * dl + da * da + db * db);
}

/* A machine's palette as RGB and Lab, and how alike two of its colors
 * look (1 the same, 0 nothing alike). */
typedef struct {
    rgb_t rgb[16];
    lab_t lab[16];
    float sim[16][16];
} pal_t;
static pal_t pals[ATLAS_NMACHINES];

static void init_colors(void) {
    if (lut_ready) return;
    for (int i = 0; i < 256; i++) {
        float c = i / 255.0f;
        lin_lut[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    lut_ready = true;
    for (int m = 0; m < ATLAS_NMACHINES; m++) {
        const uint32_t *p = atlas_palette(m);
        pal_t *P = &pals[m];
        for (int i = 0; i < 16; i++) {
            P->rgb[i] = (rgb_t){ (float)(p[i] & 0xFF), (float)((p[i] >> 8) & 0xFF), (float)((p[i] >> 16) & 0xFF) };
            P->lab[i] = to_lab(P->rgb[i]);
        }
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 16; j++) {
                float s = 1 - lab_d(P->lab[i], P->lab[j]) / 50;
                P->sim[i][j] = s > 0 ? s * s : 0;
                /* A Spectrum color and its BRIGHT one differ by brightness
                 * only, which a capture card gets wrong by as much. */
                if (atlas_family(m) == ATLAS_ZX48 && (i & 7) == (j & 7) && P->sim[i][j] < 0.9f) P->sim[i][j] = 0.9f;
            }
    }
}

static int nearest_lab(const pal_t *P, lab_t c, float *dist) {
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i < 16; i++) {
        float d = lab_d(c, P->lab[i]);
        if (d < bd) { bd = d; best = i; }
    }
    if (dist) *dist = bd;
    return best;
}

/* ------------------------------------------------------------------------ */
/* The captured picture                                                      */
/* ------------------------------------------------------------------------ */

typedef struct { int w, h; rgb_t *px; } image_t;
typedef struct { float x, y, w, h; } rectf_t;

static bool load_image(const char *path, const atlas_match_opts_t *op, image_t *im, char *err, int errlen) {
    int n;
    unsigned char *d = op && op->image_data ? stbi_load_from_memory(op->image_data, op->image_len, &im->w, &im->h, &n, 3)
                                            : stbi_load(path, &im->w, &im->h, &n, 3);
    if (!d) { snprintf(err, (size_t)errlen, "%s", stbi_failure_reason()); return false; }
    if (im->w < 16 || im->h < 16) {      /* an icon, not a screen (and too small to measure) */
        snprintf(err, (size_t)errlen, "the picture is only %dx%d pixels", im->w, im->h);
        stbi_image_free(d);
        return false;
    }
    im->px = malloc((size_t)im->w * im->h * sizeof(rgb_t));
    for (int i = 0; i < im->w * im->h; i++) im->px[i] = (rgb_t){ d[3 * i], d[3 * i + 1], d[3 * i + 2] };
    stbi_image_free(d);
    return true;
}

static float luma(rgb_t c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }

static float rgb_d(rgb_t x, rgb_t y) {
    float dr = x.r - y.r, dg = x.g - y.g, db = x.b - y.b;
    return sqrtf(dr * dr + dg * dg + db * db);
}

/* What the picture shows, before knowing the machine:
 *
 * The FRAME: bars of one flat color, not black, on both sides (or top and
 * bottom) of a picture of TV proportions: a 4:3 capture pillarboxed into
 * 16:9 by a card or a program that fills with white or grey. Cut off
 * first; what follows looks inside it.
 *
 * The SIGNAL: the picture without the capture card's black bars (columns
 * and rows as dark as the darkest corner, from the edges in). Their
 * color is the black level of the card, the reference to correct the
 * colors with. A black border can't be told from the bars: the signal
 * then ends where the picture starts.
 *
 * The BORDER color: the median of a thin ring inside the signal. A photo
 * of a screen is lit unevenly (brighter in the middle, darker at the
 * corners), so the border is looked for against the border's color NEAR
 * each pixel: the ring's color along each edge, blended between them (a
 * Coons patch). A ring point far from the median (content at the edge) is
 * the median. A capture card's flat border gives the median everywhere.
 *
 * Where the border ENDS: the longest run of rows (then columns) most of
 * whose pixels aren't the border color: the active display, when its
 * background isn't the border color.
 *
 * The CONTENT: the bounding box of whatever isn't the border color (text
 * on a screen the color of its border).
 *
 * SNOW: how much of the signal is noise, blocks of about a character
 * whose brightness varies well above the quietest blocks. Snow covers
 * the whole signal, border and all; a screen of garbage leaves its border
 * plain, so it never gets near. 0 under 80%, 1 from 95%. */
typedef struct {
    int w, h;                   /* the whole picture */
    int x0, y0, x1, y1;
    int fx0, fy0, fx1, fy1;     /* inside the frame (the whole picture without one) */
    bool frame, bars;
    rgb_t black, border;
    bool has_edge, has_content;
    float snow;
    rectf_t edge, content;
} scene_t;

static rgb_t median_rgb(const image_t *im, int x0, int y0, int x1, int y1, int ring) {
    static int hist[3][256];
    memset(hist, 0, sizeof(hist));
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (ring && x >= x0 + ring && x < x1 - ring && y >= y0 + ring && y < y1 - ring) continue;
            rgb_t c = im->px[y * im->w + x];
            hist[0][(int)clampf(c.r, 0, 255)]++;
            hist[1][(int)clampf(c.g, 0, 255)]++;
            hist[2][(int)clampf(c.b, 0, 255)]++;
            n++;
        }
    float v[3] = { 0 };
    for (int k = 0; k < 3; k++) {
        int acc = 0;
        for (int i = 0; i < 256; i++) {
            acc += hist[k][i];
            if (2 * acc >= n) { v[k] = (float)i; break; }
        }
    }
    return (rgb_t){ v[0], v[1], v[2] };
}

/* The longest run of P[i] > 0.5 (normalized to its inside), with the
 * edges moved by how much of the rows just outside it are covered. */
static bool longest_run(const float *p, int n, int min_len, float *a, float *b) {
    /* A run goes on over short dips: lines of text in the border's color. */
    int best0 = -1, best1 = -1, gap = n / 12;
    for (int i = 0; i < n; ) {
        if (p[i] <= 0.5f) { i++; continue; }
        int j = i;
        for (;;) {
            while (j < n && p[j] > 0.5f) j++;
            int k = j;
            while (k < n && k - j <= gap && p[k] <= 0.5f) k++;
            if (k < n && k - j <= gap && k > j) j = k;
            else break;
        }
        if (j - i > best1 - best0) { best0 = i; best1 = j; }
        i = j;
    }
    if (best0 < 0 || best1 - best0 < min_len) return false;
    double in = 0;
    int k = 0;
    for (int i = best0 + (best1 - best0) / 4; i < best1 - (best1 - best0) / 4; i++) { in += p[i]; k++; }
    float full = k ? (float)(in / k) : 1;
    *a = (float)best0 - (best0 > 0 ? clampf(p[best0 - 1] / full, 0, 1) : 0);
    *b = (float)best1 + (best1 < n ? clampf(p[best1] / full, 0, 1) : 0);
    return true;
}

static bool bar_line(const image_t *im, int x, int y, int dx, int dy, int n, float black_l);

/* A line of the frame: hardly a pixel away from color C. */
static bool frame_line(const image_t *im, int x, int y, int dx, int dy, int n, rgb_t c) {
    int off = 0;
    for (int i = 0; i < n; i++) off += rgb_d(im->px[(y + i * dy) * im->w + x + i * dx], c) > 24;
    return off * 100 < n;
}

/* Bars of the frame across (side by side, ACROSS) or down: the same flat
 * color, not black, on both sides; what they leave of TV
 * proportions; and the picture inside not that color along the other two
 * edges (else it is a border, the machine's own, filling the picture). */
static bool find_frame(const image_t *im, bool across, int *a0, int *a1) {
    int W = im->w, H = im->h, n = across ? W : H, m = across ? H : W;
    #define LINE(i) (across ? frame_line(im, (i), 0, 0, 1, H, c) : frame_line(im, 0, (i), 1, 0, W, c))
    rgb_t c = across ? median_rgb(im, 0, 0, 2, H, 0) : median_rgb(im, 0, 0, W, 2, 0);
    rgb_t c2 = across ? median_rgb(im, W - 2, 0, W, H, 0) : median_rgb(im, 0, H - 2, W, H, 0);
    if (luma(c) < 70 || rgb_d(c, c2) > 30) { return false; }     /* black bars: analyze()'s own */
    int i0 = 0, i1 = n;
    while (i0 < n && LINE(i0)) i0++;
    while (i1 > i0 && LINE(i1 - 1)) i1--;
    #undef LINE
    int wa = i0, wb = n - i1;
    /* Not always centered (a card's offset): the narrower a third of the other at least. */
    if (wa < n / 25 || wb < n / 25 || 3 * (wa < wb ? wa : wb) < (wa > wb ? wa : wb)) return false;
    float aspect = across ? (float)(i1 - i0) / H : (float)W / (i1 - i0);
    if (aspect < 1.15f || aspect > 1.5f) return false;
    /* Inside, the first and last lines the other way: not the frame's color. */
    int same = 0;
    for (int k = i0; k < i1; k++) {
        same += rgb_d(across ? im->px[k] : im->px[k * W], c) <= 24;
        same += rgb_d(across ? im->px[(m - 1) * W + k] : im->px[k * W + W - 1], c) <= 24;
    }
    if (same > (i1 - i0)) return false;     /* half of them or more */
    *a0 = i0;
    *a1 = i1;
    return true;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float snow_share(const image_t *im, const scene_t *sc) {
    int sw = sc->x1 - sc->x0, sh = sc->y1 - sc->y0;
    int bs = sw / 48 > 4 ? sw / 48 : 4, bw = sw / bs, bh = sh / bs;
    if (bw < 4 || bh < 4) return 0;
    float *sd = malloc((size_t)bw * bh * sizeof(float));
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            double s = 0, s2 = 0;
            for (int y = 0; y < bs; y++)
                for (int x = 0; x < bs; x++) {
                    float l = luma(im->px[(sc->y0 + by * bs + y) * im->w + sc->x0 + bx * bs + x]);
                    s += l; s2 += l * l;
                }
            double n = bs * bs, mean = s / n;
            sd[by * bw + bx] = (float)sqrt(fmax(0, s2 / n - mean * mean));
        }
    float *sorted = malloc((size_t)bw * bh * sizeof(float));
    memcpy(sorted, sd, (size_t)bw * bh * sizeof(float));
    qsort(sorted, (size_t)(bw * bh), sizeof(float), cmp_float);
    float floor_sd = sorted[bw * bh / 20];      /* the quietest blocks: the card's own noise */
    int noisy = 0;
    for (int i = 0; i < bw * bh; i++) noisy += sd[i] > floor_sd + 8;
    free(sd);
    free(sorted);
    /* Snow: noise everywhere, so its quietest blocks are noisy too. */
    float frac = (float)noisy / (bw * bh);
    if (floor_sd > 12) frac = 1;
    float everywhere = clampf((frac - 0.8f) / 0.15f, 0, 1);
    if (everywhere <= 0) return 0;
    /* And no line repeats another: a fine pattern (a RAM test's fill, a
     * screen of garbage with a black border lost in the bars) repeats at
     * its period, snow at none. How much two lines K apart differ, at the
     * best K from one line of the machine to 16, against two unrelated
     * pixels (sqrt 2 times the spread). */
    double s = 0, s2 = 0;
    int n = 0;
    for (int y = sc->y0; y < sc->y1; y += 3)
        for (int x = sc->x0; x < sc->x1; x += 3) {
            float l = luma(im->px[y * im->w + x]);
            s += l; s2 += l * l; n++;
        }
    double sigma = sqrt(fmax(1, s2 / n - (s / n) * (s / n)));
    int k0 = sh / 288 > 1 ? sh / 288 : 1;
    double best = 1e9;
    for (int k = k0 * 2; k <= k0 * 16 && k < sh / 2; k++) {
        double d = 0;
        int m = 0;
        for (int y = sc->y0; y + k < sc->y1; y += 3)
            for (int x = sc->x0; x < sc->x1; x += 3) {
                d += fabsf(luma(im->px[y * im->w + x]) - luma(im->px[(y + k) * im->w + x]));
                m++;
            }
        if (m && d / m < best) best = d / m;
    }
    /* Mean |a - b| of two independent normal values: 2 sigma / sqrt(pi). */
    double r = best / (2 * sigma / sqrt(3.141592653589793));
    return everywhere * clampf((float)(r - 0.6) / 0.2f, 0, 1);
}

/* The border's color near each point of the signal (see above). */
typedef struct {
    int x0, y0, w, h;
    rgb_t *top, *bottom, *left, *right;
} border_ref_t;

static void border_ref_make(const image_t *im, const scene_t *sc, int ring, border_ref_t *br) {
    br->x0 = sc->x0; br->y0 = sc->y0; br->w = sc->x1 - sc->x0; br->h = sc->y1 - sc->y0;
    br->top = malloc((size_t)br->w * sizeof(rgb_t));
    br->bottom = malloc((size_t)br->w * sizeof(rgb_t));
    br->left = malloc((size_t)br->h * sizeof(rgb_t));
    br->right = malloc((size_t)br->h * sizeof(rgb_t));
    /* Each point: the median of a ring-sized block, every few pixels (the
     * lighting changes slowly), held in between. */
    int step = ring / 2 > 1 ? ring / 2 : 1;
    for (int i = 0; i < br->w; i += step) {
        int a = sc->x0 + i - ring, b = sc->x0 + i + ring;
        if (a < sc->x0) a = sc->x0;
        if (b > sc->x1) b = sc->x1;
        rgb_t t = median_rgb(im, a, sc->y0, b, sc->y0 + ring, 0), u = median_rgb(im, a, sc->y1 - ring, b, sc->y1, 0);
        if (rgb_d(t, sc->border) > 60) t = sc->border;
        if (rgb_d(u, sc->border) > 60) u = sc->border;
        for (int k = i; k < i + step && k < br->w; k++) { br->top[k] = t; br->bottom[k] = u; }
    }
    for (int i = 0; i < br->h; i += step) {
        int a = sc->y0 + i - ring, b = sc->y0 + i + ring;
        if (a < sc->y0) a = sc->y0;
        if (b > sc->y1) b = sc->y1;
        rgb_t l = median_rgb(im, sc->x0, a, sc->x0 + ring, b, 0), r = median_rgb(im, sc->x1 - ring, a, sc->x1, b, 0);
        if (rgb_d(l, sc->border) > 60) l = sc->border;
        if (rgb_d(r, sc->border) > 60) r = sc->border;
        for (int k = i; k < i + step && k < br->h; k++) { br->left[k] = l; br->right[k] = r; }
    }
}

static void border_ref_free(border_ref_t *br) {
    free(br->top); free(br->bottom); free(br->left); free(br->right);
}

/* The border's color at X, Y of the picture. */
static rgb_t border_at(const border_ref_t *br, int x, int y) {
    int i = x - br->x0, j = y - br->y0;
    float u = br->w > 1 ? (float)i / (float)(br->w - 1) : 0, v = br->h > 1 ? (float)j / (float)(br->h - 1) : 0;
    const rgb_t *T = br->top, *B = br->bottom, *L = br->left, *R = br->right;
    rgb_t tl = T[0], tr = T[br->w - 1], bl = B[0], bra = B[br->w - 1];
    rgb_t c;
    #define COONS(ch) ((1 - v) * T[i].ch + v * B[i].ch + (1 - u) * L[j].ch + u * R[j].ch - \
                       ((1 - u) * (1 - v) * tl.ch + u * (1 - v) * tr.ch + (1 - u) * v * bl.ch + u * v * bra.ch))
    c.r = COONS(r); c.g = COONS(g); c.b = COONS(b);
    #undef COONS
    return c;
}

static void analyze(const image_t *im, scene_t *sc) {
    memset(sc, 0, sizeof(*sc));
    sc->w = im->w;
    sc->h = im->h;
    int W = im->w, H = im->h;
    /* The bars, from the edges in: columns and rows as dark as the darkest
     * flat corner, hardly a bright pixel. Again inside what is left, with
     * its own corners: a capture in a frame of its own (a 4:3 picture
     * pillarboxed into 1080p) has the card's bars, pure black, around the
     * signal's own black. The black level is that of the innermost bars. */
    sc->x0 = 0; sc->y0 = 0; sc->x1 = W; sc->y1 = H;
    int f0, f1;
    if (find_frame(im, true, &f0, &f1)) { sc->x0 = f0; sc->x1 = f1; sc->frame = true; }
    else if (find_frame(im, false, &f0, &f1)) { sc->y0 = f0; sc->y1 = f1; sc->frame = true; }
    sc->fx0 = sc->x0; sc->fy0 = sc->y0; sc->fx1 = sc->x1; sc->fy1 = sc->y1;
    for (int pass = 0; pass < 3; pass++) {
        int RX0 = sc->x0, RY0 = sc->y0, RW = sc->x1 - sc->x0, RH = sc->y1 - sc->y0;
        int cs = RW / 50 > 4 ? RW / 50 : 4, rs = RH / 50 > 4 ? RH / 50 : 4;
        float black_l = 1e9f;
        rgb_t black = { 0, 0, 0 };
        for (int k = 0; k < 4; k++) {
            int x0 = k & 1 ? sc->x1 - cs : RX0, y0 = k & 2 ? sc->y1 - rs : RY0;
            double s = 0, s2 = 0, r = 0, g = 0, b = 0;
            for (int y = y0; y < y0 + rs; y++)
                for (int x = x0; x < x0 + cs; x++) {
                    rgb_t c = im->px[y * W + x];
                    float l = luma(c);
                    s += l; s2 += l * l; r += c.r; g += c.g; b += c.b;
                }
            int n = cs * rs;
            double mean = s / n, sd = sqrt(fmax(0, s2 / n - mean * mean));
            /* Black: flat, dark and without color (a dark red border is no bar). */
            rgb_t avg = { (float)(r / n), (float)(g / n), (float)(b / n) };
            float spread = fmaxf(avg.r, fmaxf(avg.g, avg.b)) - fminf(avg.r, fminf(avg.g, avg.b));
            if (sd < 20 && spread < 30 && mean < black_l) { black_l = (float)mean; black = avg; }
        }
        if (black_l >= 70) break;
        int x0 = RX0, x1 = sc->x1, y0 = RY0, y1 = sc->y1;
        while (x0 < x1 && bar_line(im, x0, RY0, 0, 1, RH, black_l)) x0++;
        while (x1 > x0 && bar_line(im, x1 - 1, RY0, 0, 1, RH, black_l)) x1--;
        while (y0 < y1 && bar_line(im, RX0, y0, 1, 0, RW, black_l)) y0++;
        while (y1 > y0 && bar_line(im, RX0, y1 - 1, 1, 0, RW, black_l)) y1--;
        if (x1 - x0 < W / 8 || y1 - y0 < H / 8) break;
        bool cut = x0 > RX0 + 1 || y0 > RY0 + 1 || x1 < sc->x1 - 1 || y1 < sc->y1 - 1;
        if (!cut) break;
        sc->x0 = x0; sc->x1 = x1; sc->y0 = y0; sc->y1 = y1;
        sc->bars = true;
        sc->black = black;
    }
    int sw = sc->x1 - sc->x0, sh = sc->y1 - sc->y0;
    int ring = (sw < sh ? sw : sh) * 3 / 100;
    if (ring < 2) ring = 2;
    sc->border = median_rgb(im, sc->x0, sc->y0, sc->x1, sc->y1, ring);
    border_ref_t br;
    border_ref_make(im, sc, ring, &br);

    /* Where the border ends. */
    float *rows = calloc((size_t)sh, sizeof(float)), *cols = calloc((size_t)sw, sizeof(float));
    const float thr = 28;       /* white and bright white, washed out, are ~40 apart */
    int cx0 = sc->x0 + sw / 5, cx1 = sc->x1 - sw / 5;
    for (int y = 0; y < sh; y++) {
        int k = 0;
        for (int x = cx0; x < cx1; x++) k += rgb_d(im->px[(sc->y0 + y) * W + x], border_at(&br, x, sc->y0 + y)) > thr;
        rows[y] = (float)k / (float)(cx1 - cx0);
    }
    float ey0, ey1, ex0, ex1;
    if (longest_run(rows, sh, sh * 2 / 5, &ey0, &ey1)) {
        int ry0 = sc->y0 + (int)ey0 + (int)(ey1 - ey0) / 10, ry1 = sc->y0 + (int)ey1 - (int)(ey1 - ey0) / 10;
        for (int x = 0; x < sw; x++) {
            int k = 0;
            for (int y = ry0; y < ry1; y++) k += rgb_d(im->px[y * W + sc->x0 + x], border_at(&br, sc->x0 + x, y)) > thr;
            cols[x] = ry1 > ry0 ? (float)k / (float)(ry1 - ry0) : 0;
        }
        if (longest_run(cols, sw, sw * 2 / 5, &ex0, &ex1)) {
            sc->has_edge = true;
            sc->edge = (rectf_t){ sc->x0 + ex0, sc->y0 + ey0, ex1 - ex0, ey1 - ey0 };
        }
    }
    /* What isn't the border color: clearly not (noise is no content), on
     * more than 1% of a row or column, away from the very edge of the
     * signal (smeared into the bars). */
    const float cthr = 50;
    int mx = sw / 50, my = sh / 50;
    int bx0 = sw, bx1 = -1, by0 = sh, by1 = -1;
    for (int y = my; y < sh - my; y++) {
        int k = 0;
        for (int x = mx; x < sw - mx; x++)
            k += rgb_d(im->px[(sc->y0 + y) * W + sc->x0 + x], border_at(&br, sc->x0 + x, sc->y0 + y)) > cthr;
        if (k > 2 && k * 100 > sw) { if (y < by0) by0 = y; by1 = y; }
    }
    for (int x = mx; x < sw - mx; x++) {
        int k = 0;
        for (int y = my; y < sh - my; y++)
            k += rgb_d(im->px[(sc->y0 + y) * W + sc->x0 + x], border_at(&br, sc->x0 + x, sc->y0 + y)) > cthr;
        if (k > 2 && k * 100 > sh) { if (x < bx0) bx0 = x; bx1 = x; }
    }
    if (bx1 >= bx0 && by1 >= by0) {
        sc->has_content = true;
        sc->content = (rectf_t){ (float)(sc->x0 + bx0), (float)(sc->y0 + by0), (float)(bx1 - bx0 + 1), (float)(by1 - by0 + 1) };
    }
    free(rows);
    free(cols);
    border_ref_free(&br);
    sc->snow = snow_share(im, sc);
}

static bool bar_line(const image_t *im, int x, int y, int dx, int dy, int n, float black_l) {
    double s = 0;
    int bright = 0;
    for (int i = 0; i < n; i++) {
        float l = luma(im->px[(y + i * dy) * im->w + x + i * dx]);
        s += l;
        bright += l > black_l + 40;
    }
    return s / n < black_l + 16 && bright * 100 < n;
}

/* ------------------------------------------------------------------------ */
/* Normalizing: the picture at the machine's resolution and palette          */
/* ------------------------------------------------------------------------ */

/* The active display's share of a full PAL capture (52 us of a line, 288
 * lines): 320 C64 pixels at 7.88 MHz, 256 Spectrum pixels at 7 MHz. */
static const float pal_share[2][2] = { { 0.78f, 0.694f }, { 0.70f, 0.667f } };

typedef struct {
    int machine;
    rectf_t active;             /* in the picture */
    char how[64];
    uint8_t *full, *half;       /* palette indices */
    float gain[3], off[3], fit; /* the color correction, its mean error (dE) */
    float snow;                 /* share of cells that are noise */
    /* The average color of every character cell, before snapping to the
     * palette: a line too thin for the composite signal, smeared into
     * its paper, still tints its cell. */
    lab_t means[ATLAS_CELLS_H][ATLAS_CELLS_W];
    uint8_t lined[ATLAS_CELLS_H][ATLAS_CELLS_W];   /* cells with only a faint line (see quantize()) */
    /* No border in the picture, and no black bars it could have gone
     * with: its color (black, put there) is no evidence either way. */
    bool no_border;
} norm_t;

/* The color correction of a capture: observed = gain * true + offset,
 * per channel, the same over the whole picture, so it is found once per
 * machine from the picture's main colors: the flat parts of the signal
 * (pixels like their neighbors) gathered into a few clusters, weighted by
 * their size.
 *
 * Gains are searched over a grid, each set of gains scored by how near
 * the corrected clusters fall to the palette, plus a pull toward no
 * correction (stronger against a tint than against a brightness error); the best is refined by least squares on the clusters
 * assigned to their nearest palette color. A search, not only the
 * refinement: started from no correction, a strong tint (white seen as
 * pink) settles on wrong colors (light grey). The offset is the black
 * level of the bars when there are bars, else searched too (a lift of
 * the blacks). With one color only, the pull decides: a tinted white
 * stays a tint of white. */
typedef struct { float gain[3], off[3], err; } colorfit_t;

#define MAXCL 16
typedef struct { rgb_t c; float w; } cluster_t;

static int clusters(const image_t *im, const scene_t *sc, cluster_t *cl) {
    static float sum[512][4];
    memset(sum, 0, sizeof(sum));
    int W = im->w;
    double total = 0;
    for (int y = sc->y0; y + 1 < sc->y1; y++)
        for (int x = sc->x0; x + 1 < sc->x1; x++) {
            rgb_t c = im->px[y * W + x];
            if (rgb_d(c, im->px[y * W + x + 1]) > 30 || rgb_d(c, im->px[(y + 1) * W + x]) > 30) continue;
            int b = ((int)clampf(c.r, 0, 255) >> 5) << 6 | ((int)clampf(c.g, 0, 255) >> 5) << 3 | ((int)clampf(c.b, 0, 255) >> 5);
            sum[b][0] += c.r; sum[b][1] += c.g; sum[b][2] += c.b; sum[b][3] += 1;
            total++;
        }
    int n = 0;
    for (int b = 0; b < 512; b++) {
        if (sum[b][3] < 0.003 * total) continue;
        cluster_t t = { { sum[b][0] / sum[b][3], sum[b][1] / sum[b][3], sum[b][2] / sum[b][3] }, (float)(sum[b][3] / total) };
        int at = n < MAXCL ? n++ : MAXCL - 1;
        if (at == MAXCL - 1 && cl[at].w >= t.w) continue;
        cl[at] = t;
        for (int i = at; i > 0 && cl[i].w > cl[i - 1].w; i--) { cluster_t s = cl[i]; cl[i] = cl[i - 1]; cl[i - 1] = s; }
    }
    return n;
}

static float fit_cost(const pal_t *P, const cluster_t *cl, int n, const float *g, const float *o, int *assign) {
    float e = 0;
    for (int i = 0; i < n; i++) {
        rgb_t t = { (cl[i].c.r - o[0]) / g[0], (cl[i].c.g - o[1]) / g[1], (cl[i].c.b - o[2]) / g[2] };
        float d;
        int k = nearest_lab(P, to_lab(t), &d);
        if (assign) assign[i] = k;
        e += cl[i].w * d;
    }
    return e;
}

/* Up to MAXFITS corrections, the best first: the best one, and the best
 * ones that see the two main colors of the picture as other palette
 * colors, when they cost not much more. (A white screen a bit dark, or a
 * light grey one a bit bright? The C64 has four greys; the picture alone
 * can't always tell.) Each is tried. */
#define MAXFITS 3

static void refine_fit(const pal_t *P, const cluster_t *cl, int n, float wsum, colorfit_t *f) {
    int assign[MAXCL];
    for (int round = 0; round < 3; round++) {
        fit_cost(P, cl, n, f->gain, f->off, assign);
        for (int ch = 0; ch < 3; ch++) {
            double sxy = 0, sxx = 0;
            for (int i = 0; i < n; i++) {
                const rgb_t *pc = &P->rgb[assign[i]];
                float x = ch == 0 ? pc->r : ch == 1 ? pc->g : pc->b;
                float y = (ch == 0 ? cl[i].c.r : ch == 1 ? cl[i].c.g : cl[i].c.b) - f->off[ch];
                sxy += cl[i].w * x * y;
                sxx += cl[i].w * x * x;
            }
            /* Least squares through the offset, pulled toward gain 1. */
            double lg = 0.1 * wsum * 128 * 128;
            f->gain[ch] = clampf((float)((sxy + lg) / (sxx + lg)), 0.55f, 1.4f);
        }
    }
    f->err = fit_cost(P, cl, n, f->gain, f->off, NULL) / wsum;
}

static int fit_colors(const image_t *im, const scene_t *sc, int m, colorfit_t *fits) {
    const pal_t *P = &pals[m];
    cluster_t cl[MAXCL];
    int n = clusters(im, sc, cl);
    float wsum = 0;
    for (int i = 0; i < n; i++) wsum += cl[i].w;
    if (!n) {
        fits[0] = (colorfit_t){ { 1, 1, 1 }, { 0, 0, 0 }, 0 };
        return 1;
    }
    /* The best fit for each way of seeing the two main colors. */
    struct { int a0, a1; float cost; colorfit_t f; } cand[64];
    int nc = 0;
    const float lifts[4] = { 0, 15, 30, 45 };
    for (int l = 0; l < (sc->bars ? 1 : 4); l++) {
        float o[3];
        if (sc->bars) { o[0] = sc->black.r; o[1] = sc->black.g; o[2] = sc->black.b; }
        else o[0] = o[1] = o[2] = lifts[l];
        for (int a = 0; a < 17; a++)
            for (int b = 0; b < 17; b++)
                for (int c = 0; c < 17; c++) {
                    float g[3] = { 0.55f + 0.05f * a, 0.55f + 0.05f * b, 0.55f + 0.05f * c };
                    /* A brightness error (the same gain on every channel) is
                     * likelier than a tint (gains apart). */
                    float mean = (g[0] + g[1] + g[2]) / 3, tint = 0;
                    for (int ch = 0; ch < 3; ch++) tint += (g[ch] - mean) * (g[ch] - mean);
                    float prior = 60 * tint + 10 * (mean - 1) * (mean - 1) + 0.05f * o[0];
                    int assign[MAXCL];
                    float cost = fit_cost(P, cl, n, g, o, assign) / wsum + prior;
                    int a1 = n > 1 ? assign[1] : -1, k;
                    for (k = 0; k < nc && (cand[k].a0 != assign[0] || cand[k].a1 != a1); k++) {}
                    if (k == nc) {
                        if (nc == 64) continue;
                        cand[nc++] = (typeof(cand[0])){ assign[0], a1, 1e30f, { { 0 }, { 0 }, 0 } };
                    }
                    if (cost < cand[k].cost) {
                        cand[k].cost = cost;
                        memcpy(cand[k].f.gain, g, sizeof(g));
                        memcpy(cand[k].f.off, o, sizeof(o));
                    }
                }
    }
    /* The cheapest, and the others within 3 dE of it. */
    for (int i = 1; i < nc; i++)
        for (int j = i; j > 0 && cand[j].cost < cand[j - 1].cost; j--) {
            typeof(cand[0]) t = cand[j]; cand[j] = cand[j - 1]; cand[j - 1] = t;
        }
    int nf = 0;
    for (int i = 0; i < nc && nf < MAXFITS && cand[i].cost <= cand[0].cost + 3; i++) {
        fits[nf] = cand[i].f;
        refine_fit(P, cl, n, wsum, &fits[nf]);
        nf++;
    }
    return nf;
}

static void apply_fit(const colorfit_t *f, rgb_t *px, int n) {
    for (int i = 0; i < n; i++) {
        px[i].r = clampf((px[i].r - f->off[0]) / f->gain[0], 0, 255);
        px[i].g = clampf((px[i].g - f->off[1]) / f->gain[1], 0, 255);
        px[i].b = clampf((px[i].b - f->off[2]) / f->gain[2], 0, 255);
    }
}

/* The picture at the machine's resolution, ACT (in the picture) taken as
 * its active display: every pixel the average of the picture under it.
 * What falls outside the signal is unknown (the card shows less border
 * than the monitor picture has); with WHOLE, only what falls outside the
 * picture: its black bars are then taken for a black border. */
static void resample(const image_t *im, const scene_t *sc, int m, rectf_t act, bool whole, rgb_t *out, uint8_t *known) {
    int x0 = whole ? sc->fx0 : sc->x0, y0 = whole ? sc->fy0 : sc->y0, x1 = whole ? sc->fx1 : sc->x1,
        y1 = whole ? sc->fy1 : sc->y1;
    int ax, ay, aw, ah, sw, sh;
    atlas_active_area(m, &ax, &ay, &aw, &ah);
    atlas_screen_size(m, &sw, &sh);
    float kx = act.w / aw, ky = act.h / ah;
    for (int y = 0; y < sh; y++) {
        float v0 = act.y + (y - ay) * ky, v1 = v0 + ky;
        float cv0 = fmaxf(v0, (float)y0), cv1 = fminf(v1, (float)y1);
        for (int x = 0; x < sw; x++) {
            int i = y * sw + x;
            float u0 = act.x + (x - ax) * kx, u1 = u0 + kx;
            float cu0 = fmaxf(u0, (float)x0), cu1 = fminf(u1, (float)x1);
            known[i] = 0;
            if (cu1 - cu0 < 0.5f * kx || cv1 - cv0 < 0.5f * ky) continue;
            double r = 0, g = 0, b = 0, wsum = 0;
            for (int yy = (int)floorf(cv0); yy < (int)ceilf(cv1); yy++) {
                float wy = fminf(cv1, (float)yy + 1) - fmaxf(cv0, (float)yy);
                if (wy <= 0) continue;
                for (int xx = (int)floorf(cu0); xx < (int)ceilf(cu1); xx++) {
                    float wx = fminf(cu1, (float)xx + 1) - fmaxf(cu0, (float)xx);
                    if (wx <= 0) continue;
                    rgb_t c = im->px[yy * im->w + xx];
                    float wgt = wx * wy;
                    r += wgt * c.r; g += wgt * c.g; b += wgt * c.b; wsum += wgt;
                }
            }
            if (wsum <= 0) continue;
            out[i] = (rgb_t){ (float)(r / wsum), (float)(g / wsum), (float)(b / wsum) };
            known[i] = 1;
        }
    }
}

/* Snap to the palette. The border pixel by pixel; the active display cell
 * by cell, as two colors: the cell's most frequent one (paper) and the
 * one that best explains the rest as a mix of the two (ink: a stroke the
 * composite signal smeared into its paper), a pixel being ink when it is
 * nearer the ink than the paper along the mix. */
static void quantize(int m, const rgb_t *px, const uint8_t *known, uint8_t *idx,
                     uint8_t lined[ATLAS_CELLS_H][ATLAS_CELLS_W]) {
    const pal_t *P = &pals[m];
    int ax, ay, aw, ah, sw, sh;
    atlas_active_area(m, &ax, &ay, &aw, &ah);
    atlas_screen_size(m, &sw, &sh);
    uint8_t *near = malloc((size_t)sw * sh);
    for (int i = 0; i < sw * sh; i++) near[i] = known[i] ? (uint8_t)nearest_lab(P, to_lab(px[i]), NULL) : 0xFF;
    uint32_t bh[16] = { 0 };
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            int i = y * sw + x;
            bool in = x >= ax && x < ax + aw && y >= ay && y < ay + ah;
            idx[i] = near[i];
            if (!in && known[i]) bh[near[i]]++;
        }
    uint32_t act[16] = { 0 };
    for (int cy = 0; cy < ah / 8; cy++)
        for (int cx = 0; cx < aw / 8; cx++) {
            int base = (ay + cy * 8) * sw + ax + cx * 8;
            uint32_t h[16] = { 0 };
            int nk = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int i = base + y * sw + x;
                    if (known[i]) { h[near[i]]++; nk++; }
                }
            if (!nk) continue;
            int paper = argmax_u32(h, 16), ink = paper;
            rgb_t pp = P->rgb[paper];
            float bestcost = 1e30f;
            for (int k = 0; k < 16; k++) {
                rgb_t pk = P->rgb[k];
                float dx = pk.r - pp.r, dy = pk.g - pp.g, dz = pk.b - pp.b, len2 = dx * dx + dy * dy + dz * dz;
                if (len2 < 1) continue;
                float cost = 0;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        int i = base + y * sw + x;
                        if (!known[i]) continue;
                        rgb_t c = px[i];
                        float t = clampf(((c.r - pp.r) * dx + (c.g - pp.g) * dy + (c.b - pp.b) * dz) / len2, 0, 1);
                        float er = c.r - (pp.r + t * dx), eg = c.g - (pp.g + t * dy), eb = c.b - (pp.b + t * dz);
                        cost += sqrtf(er * er + eg * eg + eb * eb);
                    }
                if (cost < bestcost) { bestcost = cost; ink = k; }
            }
            rgb_t pk = P->rgb[ink];
            float dx = pk.r - pp.r, dy = pk.g - pp.g, dz = pk.b - pp.b, len2 = dx * dx + dy * dy + dz * dz;
            /* How far along the mix each pixel is. Ink from halfway between
             * the cell's typical pixel and its inkiest one, between 0.3 and
             * 0.45: a thin line the composite signal smeared to a third of
             * its color is still a line, the smear of a stroke next door
             * (less) is not. */
            float t[64], sorted[64];
            int nt = 0;
            for (int k = 0; k < 64; k++) {
                int i = base + (k >> 3) * sw + (k & 7);
                rgb_t c = px[i];
                t[k] = len2 >= 1 ? ((c.r - pp.r) * dx + (c.g - pp.g) * dy + (c.b - pp.b) * dz) / len2 : 0;
                if (known[i]) sorted[nt++] = t[k];
            }
            qsort(sorted, (size_t)nt, sizeof(float), cmp_float);
            float thr = fminf(0.45f, fmaxf(0.3f, (sorted[nt / 2] + sorted[nt - 1]) / 2));
            /* A one pixel line through the cell, which the composite signal
             * smears to a quarter of its color or less (red on black: the
             * ROM's RAM test pattern): its column (row) of 8 averaged is
             * above the noise, so it is ink when it stands out from the
             * cell and is inkier than the columns on both sides (the smear
             * of a stroke next door never is), all along. Only in a cell
             * with nothing stronger: in one with text, a stroke and its
             * smear are already told apart above. */
            float colt[8] = { 0 }, rowt[8] = { 0 }, colm[8], rowm[8];
            bool coll[8] = { 0 }, rowl[8] = { 0 }, strong = false;
            for (int k = 0; k < 64; k++) strong |= t[k] > 0.45f;
            if (nt == 64 && len2 >= 1 && !strong) {
                for (int j = 0; j < 8; j++) colm[j] = rowm[j] = 1e9f;
                for (int k = 0; k < 64; k++) {
                    colt[k & 7] += t[k] / 8; rowt[k >> 3] += t[k] / 8;
                    colm[k & 7] = fminf(colm[k & 7], t[k]); rowm[k >> 3] = fminf(rowm[k >> 3], t[k]);
                }
                float cmin = 1e9f, rmin = 1e9f;
                for (int j = 0; j < 8; j++) { cmin = fminf(cmin, colt[j]); rmin = fminf(rmin, rowt[j]); }
                float lthr = fmaxf(0.18f, fminf(cmin, rmin) + 0.15f);
                for (int j = 0; j < 8; j++) {
                    coll[j] = colt[j] > lthr && colm[j] > colt[j] / 2 && (j == 0 || colt[j] > colt[j - 1]) &&
                              (j == 7 || colt[j] > colt[j + 1]);
                    rowl[j] = rowt[j] > lthr && rowm[j] > rowt[j] / 2 && (j == 0 || rowt[j] > rowt[j - 1]) &&
                              (j == 7 || rowt[j] > rowt[j + 1]);
                }
                /* Such a line keeps more of its hue than of its color: a
                 * red one comes out a dim, reddish grey, which the mix
                 * above takes for white. Its ink is the color of the
                 * hue it has, when it has one. */
                float lr = 0, lg = 0, lb = 0, nl = 0;
                for (int k = 0; k < 64; k++) {
                    if (!coll[k & 7] && !rowl[k >> 3]) continue;
                    int i = base + (k >> 3) * sw + (k & 7);
                    lr += px[i].r; lg += px[i].g; lb += px[i].b; nl++;
                }
                lined[cy][cx] = nl > 0;
                if (nl) {
                    lab_t lc = to_lab((rgb_t){ lr / nl, lg / nl, lb / nl }), lp = P->lab[paper];
                    float da = lc.a - lp.a, db = lc.b - lp.b, c = sqrtf(da * da + db * db), bestcos = 0.8f;
                    if (c > 6)
                        for (int k = 0; k < 16; k++) {
                            float ka = P->lab[k].a - lp.a, kb = P->lab[k].b - lp.b, kc = sqrtf(ka * ka + kb * kb);
                            if (k == paper || kc < 15) continue;
                            float cs = (da * ka + db * kb) / (c * kc);
                            if (cs > bestcos + 1e-4f) { bestcos = cs; ink = k; }
                        }
                }
            }
            for (int k = 0; k < 64; k++) {
                int i = base + (k >> 3) * sw + (k & 7);
                if (!known[i]) { idx[i] = 0xFF; continue; }
                /* Noise alone (the card's, a few units once averaged)
                 * never makes ink, even between two close colors. */
                bool line = (coll[k & 7] || rowl[k >> 3]) && t[k] > 0.1f;
                idx[i] = (uint8_t)((t[k] > thr && rgb_d(px[i], pp) > 24) || line ? ink : paper);
                act[idx[i]]++;
            }
        }
    int bd = argmax_u32(bh, 16), bg = argmax_u32(act, 16);
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            int i = y * sw + x;
            bool in = x >= ax && x < ax + aw && y >= ay && y < ay + ah;
            if (idx[i] == 0xFF) idx[i] = (uint8_t)(in ? bg : bd);
        }
    /* The outermost pixels of the active display in the border's color are
     * the border, a pixel off (the edges are found to a pixel or so), when
     * the screen's background is just inside: the background. (Something
     * drawn up to the edge goes on inside.) So is one that is a blend of
     * the two (a blurred edge, half border): its cell may have snapped it
     * to another color along the way (bright white for white). */
    if (bg != bd) {
        rgb_t pg = P->rgb[bg], pb = P->rgb[bd];
        float ex = pb.r - pg.r, ey = pb.g - pg.g, ez = pb.b - pg.b, el2 = ex * ex + ey * ey + ez * ez;
        for (int y = ay; y < ay + ah; y++)
            for (int x = ax; x < ax + aw; x++) {
                int dx = x < ax + 2 ? 3 : x >= ax + aw - 2 ? -3 : 0, dy = y < ay + 2 ? 3 : y >= ay + ah - 2 ? -3 : 0;
                int i = y * sw + x;
                if (!(dx || dy) || idx[i] == bg || idx[(y + dy) * sw + x + dx] != bg) continue;
                bool blend = false;
                if (known[i] && el2 >= 1) {
                    rgb_t c = px[i];
                    float t = ((c.r - pg.r) * ex + (c.g - pg.g) * ey + (c.b - pg.b) * ez) / el2;
                    float er = c.r - (pg.r + t * ex), eg = c.g - (pg.g + t * ey), eb = c.b - (pg.b + t * ez);
                    blend = t > 0 && t < 1.25f && sqrtf(er * er + eg * eg + eb * eb) < 30;
                }
                if (idx[i] == bd || blend) idx[i] = (uint8_t)bg;
            }
    }
    free(near);
}

/* The average color of each character cell of the active display: of the
 * corrected picture where it is known, of the palette color put there
 * where it isn't. */
static void cell_means(int m, const rgb_t *px, const uint8_t *known, const uint8_t *idx,
                       lab_t means[ATLAS_CELLS_H][ATLAS_CELLS_W]) {
    const pal_t *P = &pals[m];
    int ax, ay, aw, ah, sw, sh;
    atlas_active_area(m, &ax, &ay, &aw, &ah);
    atlas_screen_size(m, &sw, &sh);
    for (int cy = 0; cy < ah / 8; cy++)
        for (int cx = 0; cx < aw / 8; cx++) {
            float r = 0, g = 0, b = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int i = (ay + cy * 8 + y) * sw + ax + cx * 8 + x;
                    rgb_t c = known[i] ? px[i] : P->rgb[idx[i] & 15];
                    r += c.r; g += c.g; b += c.b;
                }
            means[cy][cx] = to_lab((rgb_t){ r / 64, g / 64, b / 64 });
        }
}

static void normalize(const image_t *im, const scene_t *sc, int m, const colorfit_t *fit, rectf_t act, bool whole,
                      const char *how, norm_t *nm) {
    int sw, sh;
    atlas_screen_size(m, &sw, &sh);
    memset(nm, 0, sizeof(*nm));
    nm->machine = m;
    nm->active = act;
    snprintf(nm->how, sizeof(nm->how), "%s", how);
    rgb_t *px = malloc((size_t)sw * sh * sizeof(rgb_t));
    uint8_t *known = malloc((size_t)sw * sh);
    resample(im, sc, m, act, whole, px, known);
    nm->snow = sc->snow;
    if (!sc->bars) {
        int ax, ay, aw, ah, nb = 0;
        atlas_active_area(m, &ax, &ay, &aw, &ah);
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++)
                nb += known[y * sw + x] && !(x >= ax && x < ax + aw && y >= ay && y < ay + ah);
        nm->no_border = nb * 100 < (sw * sh - aw * ah);     /* under 1% of it */
    }
    apply_fit(fit, px, sw * sh);
    for (int ch = 0; ch < 3; ch++) { nm->gain[ch] = fit->gain[ch]; nm->off[ch] = fit->off[ch]; }
    nm->fit = fit->err;
    nm->full = malloc((size_t)sw * sh);
    nm->half = malloc((size_t)(sw / 2) * (sh / 2));
    quantize(m, px, known, nm->full, nm->lined);
    cell_means(m, px, known, nm->full, nm->means);
    atlas_half(nm->full, sw, sh, nm->half);
    free(px);
    free(known);
}

static void norm_free(norm_t *nm) {
    free(nm->full);
    free(nm->half);
    nm->full = nm->half = NULL;
}

/* Where the active display may be, for machine M: where the border ends;
 * the signal being the monitor picture of the simulator; the signal being
 * a full PAL capture; the signal being only the active display (a black
 * border cut off with the bars); and, when there is only text on a screen
 * the color of its border, the text's box put at the active display's
 * top left or bottom left corner. */
static int hypotheses(const scene_t *sc, int m, rectf_t *r, const char **how, bool *whole) {
    int ax, ay, aw, ah, sw, sh, n = 0;
    atlas_active_area(m, &ax, &ay, &aw, &ah);
    atlas_screen_size(m, &sw, &sh);
    const float *share = pal_share[atlas_family(m)];
    float W = (float)(sc->x1 - sc->x0), H = (float)(sc->y1 - sc->y0);
    rectf_t native = { sc->x0 + W * ax / sw, sc->y0 + H * ay / sh, W * aw / sw, H * ah / sh };
    rectf_t pal = { 0, 0, W * share[0], H * share[1] };
    pal.x = sc->x0 + (W - pal.w) / 2;
    pal.y = sc->y0 + (H - pal.h) / 2;
    if (sc->has_edge) { r[n] = sc->edge; how[n++] = "where the border ends"; }
    r[n] = native; how[n++] = "monitor picture proportions";
    r[n] = pal; how[n++] = "PAL capture proportions";
    /* Not when the content sits in the middle with a margin all round:
     * that margin is a border, there to be read, not cut off (text on a
     * screen the color of its border sits in a corner). */
    float ml = sc->content.x - sc->x0, mr = sc->x1 - (sc->content.x + sc->content.w);
    float mt = sc->content.y - sc->y0, mb = sc->y1 - (sc->content.y + sc->content.h);
    bool framed = sc->has_content && ml > W * 0.08f && mr > W * 0.08f && mt > H * 0.08f && mb > H * 0.08f &&
                  ml < mr * 2 && mr < ml * 2 && mt < mb * 2 && mb < mt * 2;
    if (!framed) { r[n] = (rectf_t){ (float)sc->x0, (float)sc->y0, W, H }; how[n++] = "the signal is the active display"; }
    /* A black border went with the bars: the proportions of the whole
     * picture instead. */
    if (sc->bars) {
        float fx = (float)sc->fx0, fy = (float)sc->fy0, iw = (float)(sc->fx1 - sc->fx0), ih = (float)(sc->fy1 - sc->fy0);
        r[n] = (rectf_t){ fx + iw * ax / sw, fy + ih * ay / sh, iw * aw / sw, ih * ah / sh };
        whole[n] = true;
        how[n++] = "monitor proportions of the whole picture";
        r[n] = (rectf_t){ fx + iw * (1 - share[0]) / 2, fy + ih * (1 - share[1]) / 2, iw * share[0], ih * share[1] };
        whole[n] = true;
        how[n++] = "PAL proportions of the whole picture";
    }
    if (sc->has_content && !sc->has_edge) {
        const rectf_t *c = &sc->content;
        rectf_t sz[2] = { native, pal };
        for (int k = 0; k < 2; k++) {
            if (c->w > sz[k].w * 1.05f || c->h > sz[k].h * 1.05f) continue;
            r[n] = (rectf_t){ c->x, c->y, sz[k].w, sz[k].h }; how[n++] = k ? "text at top left (PAL)" : "text at top left";
            r[n] = (rectf_t){ c->x, c->y + c->h - sz[k].h, sz[k].w, sz[k].h };
            how[n++] = k ? "text at bottom left (PAL)" : "text at bottom left";
        }
        if (c->w > native.w * 0.6f && c->h > native.h * 0.6f) { r[n] = *c; how[n++] = "the content is the active display"; }
    }
    /* The same rectangle twice is one hypothesis. */
    int k = 0;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < k && !dup; j++)
            dup = whole[i] == whole[j] && fabsf(r[i].x - r[j].x) < 1 && fabsf(r[i].y - r[j].y) < 1 && fabsf(r[i].w - r[j].w) < 1 &&
                  fabsf(r[i].h - r[j].h) < 1;
        if (!dup) { r[k] = r[i]; whole[k] = whole[i]; how[k++] = how[i]; }
    }
    return k;
}

/* ------------------------------------------------------------------------ */
/* Features and the score                                                    */
/* ------------------------------------------------------------------------ */

#define NLAGS 16

typedef struct {
    int machine;
    const uint8_t *img;         /* the thumbnail picture */
    uint8_t border, bg;         /* bg: what the cells are measured against */
    uint8_t bg_own;             /* the active display's most frequent color */
    float share[16];            /* of the active display in each color */
    int drawn;
    uint8_t dens[ATLAS_CELLS_H][ATLAS_CELLS_W];     /* pixels not bg, 0..16 */
    uint8_t ink[ATLAS_CELLS_H][ATLAS_CELLS_W];
    uint16_t mask[ATLAS_CELLS_H][ATLAS_CELLS_W];
    lab_t mean[ATLAS_CELLS_H][ATLAS_CELLS_W];       /* average color of each cell */
    lab_t qmean[ATLAS_CELLS_H][ATLAS_CELLS_W];      /* the same of the thumbnail (a capture: snapped, where lined) */
    float hc[NLAGS], vc[NLAGS];
    float fit;                  /* a capture: how far its colors are from the palette (dE) */
    bool no_border;             /* a capture: see norm_t */
} feat_t;

static void autocorr(const int *p, int n, float *out) {
    double mean = 0, den = 0;
    for (int i = 0; i < n; i++) mean += p[i];
    mean /= n;
    for (int i = 0; i < n; i++) den += (p[i] - mean) * (p[i] - mean);
    for (int lag = 1; lag <= NLAGS; lag++) {
        double s = 0;
        if (den > 0)
            for (int i = 0; i + lag < n; i++) s += (p[i] - mean) * (p[i + lag] - mean);
        out[lag - 1] = den > 0 && s > 0 ? (float)(s / den) : 0;
    }
}

/* The features of a thumbnail picture, its cells measured against the
 * background BG (-1: its own most frequent color). A screen half black,
 * half white has no clear background: a capture is measured against the
 * background of each outcome it is compared with. */
static void features(int m, const uint8_t *img, int bg, feat_t *f) {
    const pal_t *P = &pals[m];
    layout_t L = layout(m);
    memset(f, 0, sizeof(*f));
    f->machine = m;
    f->img = img;
    uint32_t bh[16] = { 0 }, ah[16] = { 0 };
    for (int y = 0; y < L.h; y++)
        for (int x = 0; x < L.w; x++) {
            bool in = x >= L.ax && x < L.ax + L.aw && y >= L.ay && y < L.ay + L.ah;
            (in ? ah : bh)[img[y * L.w + x]]++;
        }
    f->border = (uint8_t)argmax_u32(bh, 16);
    f->bg_own = (uint8_t)argmax_u32(ah, 16);
    f->bg = (uint8_t)(bg >= 0 ? bg : f->bg_own);
    for (int i = 0; i < 16; i++) f->share[i] = (float)ah[i] / (float)(L.aw * L.ah);
    int col[256] = { 0 }, row[256] = { 0 };
    for (int cy = 0; cy < L.ch; cy++)
        for (int cx = 0; cx < L.cw; cx++) {
            uint32_t h[16] = { 0 };
            uint16_t mk = 0;
            float r = 0, g = 0, b = 0;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    int px = L.ax + cx * 4 + x, py = L.ay + cy * 4 + y;
                    uint8_t v = img[py * L.w + px];
                    r += P->rgb[v].r; g += P->rgb[v].g; b += P->rgb[v].b;
                    if (v != f->bg) {
                        mk |= (uint16_t)(1u << (y * 4 + x));
                        h[v]++;
                        col[px - L.ax]++;
                        row[py - L.ay]++;
                    }
                }
            f->mask[cy][cx] = mk;
            f->dens[cy][cx] = (uint8_t)popcount16(mk);
            f->ink[cy][cx] = mk ? (uint8_t)argmax_u32(h, 16) : f->bg;
            f->mean[cy][cx] = f->qmean[cy][cx] = to_lab((rgb_t){ r / 16, g / 16, b / 16 });
            f->drawn += f->dens[cy][cx] >= 2;
        }
    autocorr(col, L.aw, f->hc);
    autocorr(row, L.ah, f->vc);
}

enum { SC_BORDER, SC_BG, SC_CELLS, SC_GLYPHS, SC_STRIPES, SC_PICTURE, SC_MEANS, SC_COLORS, NSC };
static const float sc_weight[NSC] = { 0.12f, 0.12f, 0.20f, 0.10f, 0.08f, 0.18f, 0.12f, 0.08f };
static const char *const sc_names[NSC] = { "border", "background", "cells", "glyphs", "stripes", "picture", "tint",
                                           "colors" };

static float score(const feat_t *a, const feat_t *b, float *parts) {
    const pal_t *P = &pals[a->machine];
    layout_t L = layout(a->machine);
    bool nb = a->no_border || b->no_border;
    parts[SC_BORDER] = nb ? 0.5f : P->sim[a->border][b->border];
    /* The background: the same color, or as much of the capture in the
     * outcome's background color as in its own (a half and half screen). */
    float share = 0;
    for (int i = 0; i < 16; i++) share += a->share[i] * P->sim[i][b->bg];
    parts[SC_BG] = fmaxf(P->sim[a->bg_own][b->bg], clampf(share / fmaxf(a->share[a->bg_own], 1e-3f), 0, 1));
    int both = 0, either = 0, npat = 0;
    double dsum = 0, dmax = 0, pat = 0, mean = 0;
    for (int cy = 0; cy < L.ch; cy++)
        for (int cx = 0; cx < L.cw; cx++) {
            int da = a->dens[cy][cx], db = b->dens[cy][cx];
            bool x = da >= 2, y = db >= 2;
            both += x && y;
            either += x || y;
            dsum += abs(da - db);
            dmax += da > db ? da : db;
            if (x && y) {
                pat += 0.5 * (1 - popcount16(a->mask[cy][cx] ^ b->mask[cy][cx]) / 16.0) +
                       0.5 * P->sim[a->ink[cy][cx]][b->ink[cy][cx]];
                npat++;
            }
            /* The tint of the cell as captured, or as snapped to the
             * palette and halved like the thumbnail (which makes a one
             * pixel line twice as strong): the nearer. */
            float dm = fminf(lab_d(a->mean[cy][cx], b->mean[cy][cx]), lab_d(a->qmean[cy][cx], b->qmean[cy][cx]));
            mean += fmaxf(0, 1 - dm / 40);
        }
    /* Which cells are drawn (overlap of the two sets), and how much ink
     * differs. Both with a floor of a tenth of the screen, so a few faint
     * cells (the edges a pixel off, smeared border) don't sink a blank
     * screen. */
    double floor_cells = 0.1 * L.cw * L.ch;
    double J = (both + floor_cells) / (either + floor_cells);
    double D = 1 - dsum / (16 * fmax(dmax / 16, floor_cells));
    parts[SC_CELLS] = (float)(0.5 * J + 0.5 * clampf((float)D, 0, 1));
    parts[SC_GLYPHS] = npat ? (float)(pat / npat) : either ? 0 : 1;
    parts[SC_MEANS] = (float)(mean / (L.cw * L.ch));
    double dp = 0;
    for (int i = 0; i < NLAGS; i++) dp += fabsf(a->hc[i] - b->hc[i]) + fabsf(a->vc[i] - b->vc[i]);
    parts[SC_STRIPES] = (float)(1 - dp / (2 * NLAGS));
    /* The pictures pixel by pixel, a pixel off sideways forgiven (the
     * composite signal smears and shifts things sideways). */
    /* Without a border, the active display alone. */
    double g = 0;
    int px0 = nb ? L.ax : 0, py0 = nb ? L.ay : 0, px1 = nb ? L.ax + L.aw : L.w, py1 = nb ? L.ay + L.ah : L.h;
    for (int y = py0; y < py1; y++) {
        const uint8_t *ra = a->img + y * L.w, *rb = b->img + y * L.w;
        for (int x = px0; x < px1; x++) {
            float s = P->sim[ra[x]][rb[x]];
            if (s < 1 && x > 0 && P->sim[ra[x]][rb[x - 1]] > s) s = P->sim[ra[x]][rb[x - 1]];
            if (s < 1 && x + 1 < L.w && P->sim[ra[x]][rb[x + 1]] > s) s = P->sim[ra[x]][rb[x + 1]];
            g += s;
        }
    }
    parts[SC_PICTURE] = (float)(g / ((px1 - px0) * (py1 - py0)));
    /* The palette of the machine explains the colors (once corrected):
     * what tells a C64 from a Spectrum when the layouts are alike. */
    parts[SC_COLORS] = clampf(1 - (a->fit - 4) / 16, 0, 1);
    float total = 0;
    for (int i = 0; i < NSC; i++) total += sc_weight[i] * parts[i];
    return total;
}

/* ------------------------------------------------------------------------ */
/* Matching                                                                  */
/* ------------------------------------------------------------------------ */

/* The atlas, ready to match against: every outcome's pictures decoded and
 * their features, and which outcomes show the same picture. */
typedef struct {
    atlas_t a;
    uint8_t **img[2];           /* [0] the last picture, [1] the lit one of a flash code */
    feat_t *f[2];
    uint64_t *pic;              /* hash of the pictures, 0 for snow */
    uint64_t *one[2];           /* hash of each picture alone, 0 if none */
    /* How likely each fault is (probe_fault_weight()), and whether it is
     * on the ULA's side of the lower RAM (a Spectrum's: see ula_suspect). */
    float *fw;
    uint8_t *ula_side;
} ctx_t;

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

/* Errors go to ERR (NULL: printed). */
static void report_error(char *err, int len, const char *fmt, ...) {
    char msg[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (err && len > 0) {
        size_t n = strlen(msg) < (size_t)len - 1 ? strlen(msg) : (size_t)len - 1;
        memcpy(err, msg, n);
        err[n] = 0;
    } else if (!err) fprintf(stderr, "%s\n", msg);
}

/* The ULA as a suspect. The atlas has the ULA at its pins only, but a
 * ULA failing inside gives the screens of the faults between it and the
 * lower RAM (the RAM, the resistors on its address lines, its own pins):
 * those screens name it too, as likely as the ULA fails. */
#define ULA_SUSPECT_DESC "ULA failed inside: the simulator has the ULA at its pins only and can't show " \
    "this, but a ULA failing inside gives screens like this one (it drives the lower RAM)"
#define ULA_WEIGHT 5        /* probe_part_weight()'s, for the Spectrum's ULA */

/* How much a screen's likelihood moves its rank: a screen whose causes
 * are 10 times likelier gains 0.7 points (of 100), so it only decides
 * between screens about as alike to the picture (C64HW_MATCH_LIKELY: 0
 * turns it off). */
static float likely_k(void) {
    static float k = -1;
    if (k < 0) {
        const char *e = getenv("C64HW_MATCH_LIKELY");
        k = e ? (float)atof(e) : 0.003f;
    }
    return k;
}

static int ula_part(board_t *b) {
    for (int i = 0; i < b->sim.nparts; i++) {
        const char *l = board_part_label(b, i);
        if (b->sim.parts[i].kind == PART_IC && l && !strcmp(l, "ULA")) return i;
    }
    return -1;
}

/* The weights of the atlas's faults, and which are on the ULA's side. */
static void ctx_weights(ctx_t *c) {
    int nf = c->a.nfaults;
    c->fw = malloc(((size_t)nf + 1) * sizeof(float));
    c->ula_side = calloc((size_t)nf + 1, 1);
    for (int i = 0; i < nf; i++) c->fw[i] = 1;
    for (int m = 0; m < ATLAS_NMACHINES; m++) {
        bool any = false;
        for (int i = 0; i < nf && !any; i++) any = c->a.faults[i].h.machine == m;
        if (!any) continue;
        board_t *b = board_create(atlas_machine(m));
        sim_t *s = &b->sim;
        int ula = atlas_family(m) == ATLAS_ZX48 ? ula_part(b) : -1;
        /* The nets of the ULA's lower RAM pins: /CAS, /WE, A0-A6, D0-D7, /RAS. */
        static const int ula_ram_pins[] = { 1, 4, 5, 6, 7, 8, 9, 10, 11, 18, 21, 22, 25, 27, 29, 30, 31, 35 };
        bool *ram_net = calloc((size_t)s->nnets + 1, sizeof(bool));
        for (size_t k = 0; ula >= 0 && k < sizeof(ula_ram_pins) / sizeof(ula_ram_pins[0]); k++) {
            int pin = s->parts[ula].pinmap[ula_ram_pins[k]];
            if (pin >= 0 && s->pins[pin].net >= 0) ram_net[s->pins[pin].net] = true;
        }
        for (int i = 0; i < nf; i++) {
            const atlas_fault_t *f = &c->a.faults[i];
            if (f->h.machine != m || !f->spec[0]) continue;
            c->fw[i] = probe_fault_weight(b, f->spec);
            for (const char *sp = f->spec; ula >= 0 && *sp && !c->ula_side[i]; ) {
                size_t len = strcspn(sp, " "), rl = strcspn(sp, ".:");
                char ref[16];
                snprintf(ref, sizeof(ref), "%.*s", (int)(rl < len ? rl : len), sp);
                int part = sim_find_part(s, ref);
                if (part >= 0) {
                    const part_t *p = &s->parts[part];
                    bool side = part == ula || board_part_role(b, part) == ROLE_VRAM;
                    for (int k = 0; k < p->npins && !side; k++) {
                        int net = s->pins[p->first_pin + k].net;
                        side = net >= 0 && ram_net[net];
                    }
                    c->ula_side[i] = side;
                }
                sp += len + (sp[len] == ' ');
            }
        }
        free(ram_net);
        board_destroy(b);
    }
}

/* A screen's likelihood: its likeliest cause's (not the sum: hundreds of
 * faults give the healthy screen, which says nothing of a broken one). */
static float outcome_weight(const ctx_t *c, const atlas_outcome_t *o) {
    float w = 0;
    for (int k = 0; k < o->nfaults; k++) w = fmaxf(w, c->fw[o->faults[k]]);
    return w > 0 ? w : 1;
}

static bool ula_suspect(const ctx_t *c, const atlas_outcome_t *o) {
    for (int k = 0; k < o->nfaults; k++) if (c->ula_side[o->faults[k]]) return true;
    return false;
}

/* A screen's causes as listed: the healthy board first, then the
 * likeliest, the ULA among them if it is a suspect (-1 in ORDER). Their
 * number. */
static int list_causes(const ctx_t *c, const atlas_outcome_t *o, int *order) {
    int n = 0;
    for (int k = 0; k < o->nfaults; k++) order[n++] = o->faults[k];
    if (ula_suspect(c, o)) order[n++] = -1;
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0; k--) {
            int x = order[k], y = order[k - 1];
            bool hx = x >= 0 && !c->a.faults[x].spec[0], hy = y >= 0 && !c->a.faults[y].spec[0];
            float wx = x >= 0 ? c->fw[x] : ULA_WEIGHT, wy = y >= 0 ? c->fw[y] : ULA_WEIGHT;
            if (!(hx > hy || (hx == hy && wx > wy))) break;
            order[k] = y;
            order[k - 1] = x;
        }
    return n;
}

static bool ctx_load(ctx_t *c, const char *path, char *err, int errlen) {
    memset(c, 0, sizeof(*c));
    bool exists;
    if (!atlas_load(&c->a, path, &exists) || !exists || !c->a.noutcomes) {
        report_error(err, errlen, "%s: %s", path,
                     !exists ? "no such file (build it with --build-atlas)" : "not an atlas, or empty");
        atlas_free(&c->a);
        return false;
    }
    init_colors();
    init_fonts();
    int n = c->a.noutcomes;
    for (int k = 0; k < 2; k++) {
        c->img[k] = calloc((size_t)n, sizeof(uint8_t *));
        c->f[k] = calloc((size_t)n, sizeof(feat_t));
    }
    c->pic = calloc((size_t)n, sizeof(uint64_t));
    for (int k = 0; k < 2; k++) c->one[k] = calloc((size_t)n, sizeof(uint64_t));
    for (int i = 0; i < n; i++) {
        const atlas_outcome_t *o = &c->a.outcomes[i];
        if (o->h.t.no_signal) continue;
        uint64_t h = fnv(0xCBF29CE484222325ull, &o->h.machine, 1);
        for (int k = 0; k < 2; k++) {
            if ((k ? o->h.alt_enc : o->h.enc) != THUMB_RLE) continue;
            c->img[k][i] = malloc((size_t)o->h.w * o->h.h);
            if (!atlas_decode_thumb(o, k, c->img[k][i])) { free(c->img[k][i]); c->img[k][i] = NULL; continue; }
            features(o->h.machine, c->img[k][i], -1, &c->f[k][i]);
            h = fnv(h, c->img[k][i], (size_t)o->h.w * o->h.h);
            c->one[k][i] = fnv(fnv(0xCBF29CE484222325ull, &o->h.machine, 1), c->img[k][i], (size_t)o->h.w * o->h.h);
        }
        c->pic[i] = h;
    }
    ctx_weights(c);
    return true;
}

static void ctx_free(ctx_t *c) {
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < c->a.noutcomes; i++) free(c->img[k][i]);
        free(c->img[k]);
        free(c->f[k]);
    }
    free(c->pic);
    free(c->one[0]);
    free(c->one[1]);
    free(c->fw);
    free(c->ula_side);
    atlas_free(&c->a);
}

typedef struct {
    int outcome;
    float score, parts[NSC];
    int hyp;                    /* the normalization that gave it */
    float rank;                 /* the score, moved by the likelihood (likely_k()) */
} cand_t;

typedef struct {
    int w, h;
    scene_t sc;
    norm_t norms[2 * 12 * MAXFITS];
    int nnorms;
    cand_t *c;
    int nc;
    cand_t *rc;                 /* the real repairs: .outcome is the index in op->reals */
    int nrc;
} match_t;

static const atlas_t *cand_atlas;

/* Best rank first (the score, moved by how likely the causes are); the
 * same (to 0.1%): the group with more causes first. */
static int cmp_cand(const void *x, const void *y) {
    const cand_t *a = x, *b = y;
    int sa = (int)lroundf(a->rank * 1000), sb = (int)lroundf(b->rank * 1000);
    if (sa != sb) return sa < sb ? 1 : -1;
    int na = cand_atlas->outcomes[a->outcome].nfaults, nb = cand_atlas->outcomes[b->outcome].nfaults;
    if (na != nb) return nb - na;
    return a->outcome - b->outcome;
}

static int cmp_real(const void *x, const void *y) {
    const cand_t *a = x, *b = y;
    if (a->score != b->score) return a->score < b->score ? 1 : -1;
    return a->outcome - b->outcome;
}

static bool outcome_wanted(const atlas_outcome_t *o, const atlas_match_opts_t *op) {
    if (op->machine >= 0 && o->h.machine != op->machine) return false;
    if (op->cart >= 0 && (o->h.mode != MODE_BOOT) != (op->cart == 1)) return false;
    return true;
}

/* The features of a capture as normalized, against background BG. */
static void norm_features(const norm_t *nm, int bg, feat_t *f) {
    features(nm->machine, nm->half, bg, f);
    memcpy(f->mean, nm->means, sizeof(f->mean));
    /* The tint as snapped only where a faint line was found: elsewhere
     * the capture's own. */
    for (int cy = 0; cy < ATLAS_CELLS_H; cy++)
        for (int cx = 0; cx < ATLAS_CELLS_W; cx++)
            if (!nm->lined[cy][cx]) f->qmean[cy][cx] = nm->means[cy][cx];
    f->fit = nm->fit;
    f->no_border = nm->no_border;
}

/* Normalize the picture every way it may be, score every outcome against
 * every normalization of its machine, keep the best of each outcome. */
static bool match_picture(ctx_t *c, const char *path, const atlas_match_opts_t *op, match_t *M, char *err,
                          int errlen) {
    memset(M, 0, sizeof(*M));
    image_t im;
    char why[256];
    if (!load_image(path, op, &im, why, sizeof(why))) {
        report_error(err, errlen, "%s: can't read the picture (%s)", path, why);
        return false;
    }
    M->w = im.w;
    M->h = im.h;
    analyze(&im, &M->sc);
    /* Against every board (no machine asked for): normalized once per
     * family (the Spectrum issues share the screen and the palette), each
     * board of it scored against that; the normalizations would run out
     * before the last boards otherwise. */
    bool by_family = op->machine < 0;
    for (int m = 0; m < ATLAS_NMACHINES; m++) {
        if (op->machine >= 0 && m != op->machine) continue;
        if (by_family && atlas_family(m) != m) continue;
        bool any = false;       /* a board the atlas has nothing of (not built yet) */
        for (int i = 0; i < c->a.noutcomes && !any; i++) {
            int om = c->a.outcomes[i].h.machine;
            any = (by_family ? atlas_family(om) : om) == m;
        }
        if (!any) continue;
        rectf_t r[16];
        const char *how[16];
        bool whole[16] = { false };
        int n = hypotheses(&M->sc, m, r, how, whole);
        colorfit_t fits[MAXFITS];
        int nf = fit_colors(&im, &M->sc, m, fits);
        for (int f = 0; f < nf; f++)
            for (int k = 0; k < n && M->nnorms < (int)(sizeof(M->norms) / sizeof(M->norms[0])); k++)
                normalize(&im, &M->sc, m, &fits[f], r[k], whole[k], how[k], &M->norms[M->nnorms++]);
    }
    free(im.px);
    /* The features of each normalization against each background asked
     * for, made when first asked. */
    feat_t *(*nf)[16] = calloc((size_t)M->nnorms, sizeof(*nf));
    M->c = calloc((size_t)c->a.noutcomes, sizeof(cand_t));
    for (int i = 0; i < c->a.noutcomes; i++) {
        const atlas_outcome_t *o = &c->a.outcomes[i];
        if (!outcome_wanted(o, op)) continue;
        cand_t best = { i, -1, { 0 }, -1, 0 };
        int om = by_family ? atlas_family(o->h.machine) : o->h.machine;
        for (int k = 0; k < M->nnorms; k++) {
            if (M->norms[k].machine != om) continue;
            cand_t t = { i, 0, { 0 }, k, 0 };
            if (o->h.t.no_signal) {
                t.score = M->norms[k].snow;
                for (int p = 0; p < NSC; p++) t.parts[p] = t.score;
            } else {
                float parts[NSC];
                for (int a = 0; a < 2; a++) {
                    if (!c->img[a][i]) continue;
                    int bg = c->f[a][i].bg;
                    if (!nf[k][bg]) {
                        nf[k][bg] = malloc(sizeof(feat_t));
                        norm_features(&M->norms[k], bg, nf[k][bg]);
                    }
                    float s = score(nf[k][bg], &c->f[a][i], parts);
                    /* A noisy picture isn't a clean one. */
                    s *= 1 - M->norms[k].snow;
                    if (s > t.score) { t.score = s; memcpy(t.parts, parts, sizeof(parts)); }
                }
            }
            if (t.score > best.score) best = t;
        }
        if (best.score >= 0) M->c[M->nc++] = best;
    }
    /* The real repairs, the same way: against the readings of a board of
     * their family (the Spectrum issues share the screen). */
    M->rc = calloc((size_t)op->nreals + 1, sizeof(cand_t));
    for (int i = 0; i < op->nreals; i++) {
        const atlas_real_t *rl = &op->reals[i];
        feat_t rf;
        if (rl->snow <= 0.5f) features(rl->machine, rl->thumb, -1, &rf);
        cand_t best = { i, -1, { 0 }, -1, 0 };
        for (int k = 0; k < M->nnorms; k++) {
            if (atlas_family(M->norms[k].machine) != atlas_family(rl->machine)) continue;
            cand_t t = { i, 0, { 0 }, k, 0 };
            if (rl->snow > 0.5f) t.score = M->norms[k].snow;
            else {
                if (!nf[k][rf.bg]) {
                    nf[k][rf.bg] = malloc(sizeof(feat_t));
                    norm_features(&M->norms[k], rf.bg, nf[k][rf.bg]);
                }
                t.score = score(nf[k][rf.bg], &rf, t.parts) * (1 - M->norms[k].snow);
            }
            if (t.score > best.score) best = t;
        }
        if (best.score >= 0) M->rc[M->nrc++] = best;
    }
    for (int k = 0; k < M->nnorms; k++)
        for (int bg = 0; bg < 16; bg++) free(nf[k][bg]);
    free(nf);
    for (int k = 0; k < M->nc; k++)
        M->c[k].rank = M->c[k].score + likely_k() * logf(outcome_weight(c, &c->a.outcomes[M->c[k].outcome]));
    cand_atlas = &c->a;
    qsort(M->c, (size_t)M->nc, sizeof(cand_t), cmp_cand);
    qsort(M->rc, (size_t)M->nrc, sizeof(cand_t), cmp_real);
    return true;
}

static void match_free(match_t *M) {
    for (int k = 0; k < M->nnorms; k++) norm_free(&M->norms[k]);
    free(M->c);
    free(M->rc);
}

static void write_indexed_png(int m, const uint8_t *idx, int w, int h, const char *path) {
    const uint32_t *pal = atlas_palette(m);
    uint32_t *px = malloc((size_t)w * h * sizeof(uint32_t));
    for (int i = 0; i < w * h; i++) px[i] = pal[idx[i] & 15];
    png_write(path, px, w, h);
    free(px);
}

static bool has_healthy(const atlas_t *a, const atlas_outcome_t *o) {
    for (int k = 0; k < o->nfaults; k++) if (!a->faults[o->faults[k]].spec[0]) return true;
    return false;
}

int atlas_match(const atlas_match_opts_t *op) {
    atlas_set_carts(op->carts);
    ctx_t c;
    if (!ctx_load(&c, op->path, NULL, 0)) return 1;
    match_t M;
    if (!match_picture(&c, op->image, op, &M, NULL, 0)) { ctx_free(&c); return 1; }
    const scene_t *sc = &M.sc;
    printf("%s: %dx%d picture", op->image, M.w, M.h);
    if (sc->frame) printf(", a frame cut off: the picture is %dx%d at %d,%d", sc->fx1 - sc->fx0, sc->fy1 - sc->fy0, sc->fx0,
                          sc->fy0);
    if (sc->bars) printf(", black bars cut off: the signal is %dx%d at %d,%d", sc->x1 - sc->x0, sc->y1 - sc->y0, sc->x0, sc->y0);
    printf("\n");
    if (!M.nc) { printf("nothing in the atlas to compare with (--machine / --cart)\n"); match_free(&M); ctx_free(&c); return 1; }
    const norm_t *bn = &M.norms[M.c[0].hyp >= 0 ? M.c[0].hyp : 0];
    printf("best fit: %s, active display %.0fx%.0f at %.0f,%.0f (%s), color gain %.2f/%.2f/%.2f offset %+.0f/%+.0f/%+.0f, "
           "fit error %.1f dE\n\n",
           atlas_machine_id(bn->machine), bn->active.w, bn->active.h, bn->active.x, bn->active.y, bn->how, bn->gain[0],
           bn->gain[1], bn->gain[2], bn->off[0], bn->off[1], bn->off[2], bn->fit);
    if (op->debug_dir) {
        printf("scene: border RGB %.0f,%.0f,%.0f  black %.0f,%.0f,%.0f  snow %.2f", sc->border.r, sc->border.g,
               sc->border.b, sc->black.r, sc->black.g, sc->black.b, sc->snow);
        if (sc->has_edge) printf("  edge %.0fx%.0f at %.0f,%.0f", sc->edge.w, sc->edge.h, sc->edge.x, sc->edge.y);
        if (sc->has_content)
            printf("  content %.0fx%.0f at %.0f,%.0f", sc->content.w, sc->content.h, sc->content.x, sc->content.y);
        printf("\n");
        for (int k = 0; k < M.nnorms; k++) {
            int sw, sh;
            atlas_screen_size(M.norms[k].machine, &sw, &sh);
            char p[700];
            snprintf(p, sizeof(p), "%s/norm-%s-%d.png", op->debug_dir, atlas_machine_id(M.norms[k].machine), k);
            write_indexed_png(M.norms[k].machine, M.norms[k].full, sw, sh, p);
            printf("  %s: %s, %.0fx%.0f at %.0f,%.0f, snow %.2f\n", p, M.norms[k].how, M.norms[k].active.w,
                   M.norms[k].active.h, M.norms[k].active.x, M.norms[k].active.y, M.norms[k].snow);
            /* Against #1, or the match C64HW_MATCH_VS names (why one loses). */
            const char *vs_env = getenv("C64HW_MATCH_VS");
            int vs = vs_env ? atoi(vs_env) : 1;
            if (vs < 1 || vs > M.nc) vs = 1;
            int oi = M.c[vs - 1].outcome;
            const atlas_outcome_t *o = &c.a.outcomes[oi];
            if (atlas_family(o->h.machine) == atlas_family(M.norms[k].machine) && c.img[0][oi]) {
                feat_t f;
                float parts[NSC];
                norm_features(&M.norms[k], c.f[0][oi].bg, &f);
                float s = score(&f, &c.f[0][oi], parts);
                printf("      vs #%d: %.3f  ", vs, s);
                for (int q = 0; q < NSC; q++) printf("%s %.2f ", sc_names[q], parts[q]);
                printf(" (drawn cells %d vs %d)\n", f.drawn, c.f[0][oi].drawn);
            }
        }
        printf("\n");
    }
    int top = op->top > 0 ? op->top : 5;
    for (int r = 0; r < M.nc && r < top; r++) {
        const cand_t *cd = &M.c[r];
        const atlas_outcome_t *o = &c.a.outcomes[cd->outcome];
        char label[256];
        atlas_outcome_label(o, label, sizeof(label));
        printf("#%d  %5.1f%%  %s, %s: %s%s\n", r + 1, 100 * cd->score, atlas_machine_id(o->h.machine),
               atlas_mode_name(o->h.mode), label, has_healthy(&c.a, o) ? "   <== the healthy board" : "");
        for (int q = 0; q < r; q++)
            if (c.pic[M.c[q].outcome] && c.pic[M.c[q].outcome] == c.pic[cd->outcome]) {
                printf("     (the same picture as #%d: one screenshot can't tell them apart)\n", q + 1);
                break;
            }
        if (o->h.t.nseries) printf("     (a flash code: the number of flashes takes a video, not a screenshot)\n");
        printf("     ");
        for (int p = 0; p < NSC; p++) printf("%s %.2f%s", sc_names[p], cd->parts[p], p + 1 < NSC ? "  " : "\n");
        int *order = malloc(((size_t)o->nfaults + 1) * sizeof(int)), nc = list_causes(&c, o, order);
        printf("     %d cause%s, likeliest first (likelihood %.1f):\n", nc, nc == 1 ? "" : "s", outcome_weight(&c, o));
        int shown = op->causes < 0 ? nc : op->causes;
        for (int k = 0; k < nc && k < shown; k++) {
            if (order[k] < 0) {
                printf("       %-14s %s\n", "ULA (inside)", ULA_SUSPECT_DESC);
                continue;
            }
            const atlas_fault_t *ft = &c.a.faults[order[k]];
            /* The chip notes of a description are several lines. */
            printf("       %-14s ", ft->spec[0] ? ft->spec : "(healthy)");
            for (const char *d = ft->desc; *d; d++) {
                if (*d == '\n') fputs(", ", stdout);
                else putchar(*d);
            }
            putchar('\n');
        }
        if (nc > shown) printf("       ... %d more (--causes all lists them)\n", nc - shown);
        free(order);
        printf("\n");
    }
    match_free(&M);
    ctx_free(&c);
    return 0;
}

/* The screenshot itself, made small for the interface: every pixel the
 * average of the block under it, at most MAXW wide. */
static uint32_t *small_picture(const atlas_match_opts_t *op, int maxw, int *w, int *h) {
    image_t im;
    char why[256];
    if (!load_image(op->image, op, &im, why, sizeof(why))) return NULL;
    int k = (im.w + maxw - 1) / maxw;
    if (k < 1) k = 1;
    *w = im.w / k;
    *h = im.h / k;
    uint32_t *out = malloc((size_t)*w * *h * sizeof(uint32_t));
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++) {
            float r = 0, g = 0, b = 0;
            for (int j = 0; j < k; j++)
                for (int i = 0; i < k; i++) {
                    rgb_t c = im.px[(y * k + j) * im.w + x * k + i];
                    r += c.r; g += c.g; b += c.b;
                }
            float n = (float)(k * k);
            out[y * *w + x] = 0xFF000000u | (uint32_t)(b / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(r / n);
        }
    free(im.px);
    return out;
}

uint32_t *atlas_small_picture(const unsigned char *data, int len, int maxw, int *w, int *h) {
    atlas_match_opts_t o = { .image = "", .image_data = data, .image_len = len };
    return data && len > 0 ? small_picture(&o, maxw, w, h) : NULL;
}

bool atlas_match_run(const atlas_match_opts_t *op, atlas_result_t *r) {
    memset(r, 0, sizeof(*r));
    atlas_set_carts(op->carts);
    ctx_t c;
    if (!ctx_load(&c, op->path, r->error, sizeof(r->error))) return false;
    match_t M;
    if (!match_picture(&c, op->image, op, &M, r->error, sizeof(r->error))) { ctx_free(&c); return false; }
    r->picture = small_picture(op, 400, &r->w, &r->h);
    const char *base = strrchr(op->image, '/'), *base2 = strrchr(op->image, '\\');
    if (base2 > base) base = base2;
    base = base ? base + 1 : op->image;
    if (!M.nc) {
        snprintf(r->error, sizeof(r->error), "Nothing in %s to compare %s with", op->path, base);
        match_free(&M);
        ctx_free(&c);
        return false;
    }
    const norm_t *bn = &M.norms[M.c[0].hyp >= 0 ? M.c[0].hyp : 0];
    snprintf(r->summary, sizeof(r->summary), "%s: %dx%d%s. Read as a %s screen (geometry: %s).", base, M.w, M.h,
             M.sc.frame ? ", the frame around it cut off" : M.sc.bars ? ", capture card bars cut off" : "", atlas_family(bn->machine) == ATLAS_C64 ? "C64" : "ZX Spectrum",
             bn->how);
    layout_t BL = layout(bn->machine);
    r->norm_machine = bn->machine;
    r->norm_w = BL.w;
    r->norm_h = BL.h;
    r->norm_snow = bn->snow;
    r->norm = malloc((size_t)BL.w * BL.h);
    memcpy(r->norm, bn->half, (size_t)BL.w * BL.h);
    for (int k = 0; k < M.nrc && r->nreals < ATLAS_MAX_REALS; k++) {
        r->reals[r->nreals].index = M.rc[k].outcome;
        r->reals[r->nreals++].score = M.rc[k].score;
    }
    int top = op->top > 0 ? op->top : 5;
    if (top > ATLAS_MAX_GROUPS) top = ATLAS_MAX_GROUPS;
    for (int k = 0; k < M.nc && r->ngroups < top; k++) {
        const atlas_outcome_t *o = &c.a.outcomes[M.c[k].outcome];
        atlas_group_t *g = &r->groups[r->ngroups];
        g->machine = o->h.machine;
        g->mode = o->h.mode;
        g->score = M.c[k].score;
        atlas_outcome_label(o, g->label, sizeof(g->label));
        g->healthy = has_healthy(&c.a, o);
        g->flash = o->h.t.nseries > 0;
        g->same_as = -1;
        for (int q = 0; q < k && g->same_as < 0; q++)
            if (c.pic[M.c[q].outcome] && c.pic[M.c[q].outcome] == c.pic[M.c[k].outcome]) g->same_as = q;
        /* The picture: the lit one of a flash code, else the last one;
         * snow has none. */
        const uint8_t *img = c.img[1][M.c[k].outcome] ? c.img[1][M.c[k].outcome] : c.img[0][M.c[k].outcome];
        if (img) {
            const uint32_t *pal = atlas_palette(o->h.machine);
            g->w = o->h.w;
            g->h = o->h.h;
            g->thumb = malloc((size_t)g->w * g->h * sizeof(uint32_t));
            for (int i = 0; i < g->w * g->h; i++) g->thumb[i] = pal[img[i] & 15];
        }
        int *order = malloc(((size_t)o->nfaults + 1) * sizeof(int));
        g->ncauses = list_causes(&c, o, order);
        g->causes = calloc((size_t)g->ncauses, sizeof(atlas_cause_t));
        for (int f = 0; f < g->ncauses; f++) {
            if (order[f] < 0) {
                g->causes[f].suspect = true;
                snprintf(g->causes[f].spec, sizeof(g->causes[f].spec), "ULA (inside)");
                snprintf(g->causes[f].desc, sizeof(g->causes[f].desc), "%s", ULA_SUSPECT_DESC);
                continue;
            }
            const atlas_fault_t *ft = &c.a.faults[order[f]];
            snprintf(g->causes[f].spec, sizeof(g->causes[f].spec), "%s", ft->spec);
            /* The chip notes of a description are several lines. */
            char *d = g->causes[f].desc;
            int n = 0;
            for (const char *s = ft->desc; *s && n < (int)sizeof(g->causes[f].desc) - 3; s++) {
                if (*s == '\n') { d[n++] = ','; d[n++] = ' '; }
                else d[n++] = *s;
            }
            d[n] = 0;
        }
        free(order);
        r->ngroups++;
    }
    match_free(&M);
    ctx_free(&c);
    return true;
}

void atlas_result_free(atlas_result_t *r) {
    for (int k = 0; k < r->ngroups; k++) {
        free(r->groups[k].thumb);
        free(r->groups[k].causes);
    }
    free(r->picture);
    free(r->norm);
    memset(r, 0, sizeof(*r));
}

/* ------------------------------------------------------------------------ */
/* The synthetic capture card test                                           */
/* ------------------------------------------------------------------------ */

static uint64_t rng_state;
static double rnd(void) {          /* 0..1 */
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (double)(rng_state >> 11) / 9007199254740992.0;
}
static double gauss(void) {
    double u = rnd() + 1e-12, v = rnd();
    return sqrt(-2 * log(u)) * cos(6.283185307179586 * v);
}

/* What a cheap USB composite capture card does to the monitor picture:
 * a bit more border (a real monitor shows more of it than the simulator),
 * scaled into 720x576 with black bars (not the same scale across and
 * down), luma and much more chroma smeared sideways, brightness and tint
 * off by 15%, contrast washed out, noise. */
typedef struct { int pad[4], sx, sy, sw, sh; double bright, tint[3], contrast, lift, noise; } degrade_t;

static uint32_t *degrade(const uint32_t *src, int w, int h, degrade_t *d, int *ow, int *oh) {
    const int W = 720, H = 576;
    d->pad[0] = 6 + (int)(rnd() * 20);       /* left, right, top, bottom */
    d->pad[1] = 6 + (int)(rnd() * 20);
    d->pad[2] = 4 + (int)(rnd() * 14);
    d->pad[3] = 4 + (int)(rnd() * 14);
    d->sw = 620 + (int)(rnd() * 90);
    d->sh = 520 + (int)(rnd() * 50);
    d->sx = (int)(rnd() * (W - d->sw));
    d->sy = (int)(rnd() * (H - d->sh));
    d->bright = rnd() < 0.5 ? 0.85 : 1.15;
    int up = (int)(rnd() * 3), down = (up + 1 + (int)(rnd() * 2)) % 3;
    for (int k = 0; k < 3; k++) d->tint[k] = k == up ? 1.15 : k == down ? 0.85 : 1.0;
    d->contrast = 0.85;
    d->lift = 10 + rnd() * 15;
    d->noise = 8;
    int ew = w + d->pad[0] + d->pad[1], eh = h + d->pad[2] + d->pad[3];
    /* RGB planes, the picture with its border extended (edge pixels repeated). */
    float *pl[3];
    for (int k = 0; k < 3; k++) pl[k] = calloc((size_t)W * H, sizeof(float));
    for (int y = 0; y < d->sh; y++)
        for (int x = 0; x < d->sw; x++) {
            /* Bilinear from the extended picture. */
            float u = (x + 0.5f) * ew / d->sw - 0.5f, v = (y + 0.5f) * eh / d->sh - 0.5f;
            int x0 = (int)floorf(u), y0 = (int)floorf(v);
            float fx = u - x0, fy = v - y0;
            float acc[3] = { 0 };
            for (int j = 0; j < 4; j++) {
                int xx = x0 + (j & 1), yy = y0 + (j >> 1);
                float wgt = ((j & 1) ? fx : 1 - fx) * ((j >> 1) ? fy : 1 - fy);
                int sx = xx - d->pad[0], sy = yy - d->pad[2];
                sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx;
                sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
                uint32_t c = src[sy * w + sx];
                acc[0] += wgt * (c & 0xFF);
                acc[1] += wgt * ((c >> 8) & 0xFF);
                acc[2] += wgt * ((c >> 16) & 0xFF);
            }
            for (int k = 0; k < 3; k++) pl[k][(d->sy + y) * W + d->sx + x] = acc[k];
        }
    /* Composite: Y smeared a little, U and V a lot, sideways. */
    float *Y = malloc((size_t)W * H * sizeof(float)), *U = malloc((size_t)W * H * sizeof(float)),
          *V = malloc((size_t)W * H * sizeof(float)), *tmp = malloc((size_t)W * sizeof(float));
    for (int i = 0; i < W * H; i++) {
        float r = pl[0][i], g = pl[1][i], b = pl[2][i];
        Y[i] = 0.299f * r + 0.587f * g + 0.114f * b;
        U[i] = 0.492f * (b - Y[i]);
        V[i] = 0.877f * (r - Y[i]);
    }
    float *planes[3] = { Y, U, V };
    const float sig[3] = { 0.9f, 2.5f, 2.5f };
    for (int k = 0; k < 3; k++) {
        float kern[17], ks = 0;
        for (int i = -8; i <= 8; i++) ks += kern[i + 8] = expf(-(float)(i * i) / (2 * sig[k] * sig[k]));
        for (int y = 0; y < H; y++) {
            float *row = planes[k] + y * W;
            for (int x = 0; x < W; x++) {
                float s = 0;
                for (int i = -8; i <= 8; i++) {
                    int xx = x + i < 0 ? 0 : x + i >= W ? W - 1 : x + i;
                    s += kern[i + 8] * row[xx];
                }
                tmp[x] = s / ks;
            }
            memcpy(row, tmp, (size_t)W * sizeof(float));
        }
    }
    uint32_t *out = malloc((size_t)W * H * sizeof(uint32_t));
    for (int i = 0; i < W * H; i++) {
        float rgb[3] = { Y[i] + V[i] / 0.877f, 0, Y[i] + U[i] / 0.492f };
        rgb[1] = (Y[i] - 0.299f * rgb[0] - 0.114f * rgb[2]) / 0.587f;
        uint32_t c = 0xFF000000u;
        for (int k = 0; k < 3; k++) {
            double v = ((rgb[k] - 128) * d->contrast + 128 + d->lift) * d->bright * d->tint[k] + gauss() * d->noise;
            c |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5)) << (8 * k);
        }
        out[i] = c;
    }
    for (int k = 0; k < 3; k++) free(pl[k]);
    free(Y); free(U); free(V); free(tmp);
    *ow = W;
    *oh = H;
    return out;
}

typedef struct {
    int outcome;
    const char *spec;
    int machine, mode;
    bool ok, same_key, reuse;
    uint32_t *frame, *lit;
    char err[256];
    const char *carts;
} rerun_t;

static void *rerun_worker(void *arg) {
    rerun_t *r = arg;
    uint64_t key = 0;
    r->ok = atlas_run_fault(r->machine, r->mode, r->spec, r->carts, &key, &r->frame, &r->lit, r->err, sizeof(r->err));
    r->same_key = r->ok;
    return (void *)(uintptr_t)key;
}

int atlas_match_test(const atlas_match_opts_t *op, int count, const char *out_dir, unsigned seed) {
    atlas_set_carts(op->carts);
    setvbuf(stdout, NULL, _IONBF, 0);
    ctx_t c;
    if (!ctx_load(&c, op->path, NULL, 0)) return 1;
    atlas_init_shared();
    if (count <= 0) count = 20;
    make_dir(out_dir);

    /* COUNT different pictures: round robin over machine and cartridge
     * mode, the outcomes with the most causes first, a picture already
     * taken skipped. */
    int *pick = malloc((size_t)count * sizeof(int)), npick = 0;
    int *lists[ATLAS_NMACHINES * ATLAS_NMODES], nl[ATLAS_NMACHINES * ATLAS_NMODES] = { 0 };
    for (int l = 0; l < ATLAS_NMACHINES * ATLAS_NMODES; l++) lists[l] = malloc(((size_t)c.a.noutcomes + 1) * sizeof(int));
    for (int i = 0; i < c.a.noutcomes; i++) {
        int l = c.a.outcomes[i].h.machine * ATLAS_NMODES + c.a.outcomes[i].h.mode;
        lists[l][nl[l]++] = i;
    }
    for (int l = 0; l < ATLAS_NMACHINES * ATLAS_NMODES; l++)      /* most causes first */
        for (int i = 1; i < nl[l]; i++)
            for (int j = i; j > 0 && c.a.outcomes[lists[l][j]].nfaults > c.a.outcomes[lists[l][j - 1]].nfaults; j--) {
                int t = lists[l][j]; lists[l][j] = lists[l][j - 1]; lists[l][j - 1] = t;
            }
    int at[ATLAS_NMACHINES * ATLAS_NMODES] = { 0 };
    for (bool any = true; any && npick < count; ) {
        any = false;
        for (int l = 0; l < ATLAS_NMACHINES * ATLAS_NMODES && npick < count; l++) {
            while (at[l] < nl[l]) {
                int i = lists[l][at[l]++];
                if (!outcome_wanted(&c.a.outcomes[i], op)) continue;
                bool dup = false;
                for (int k = 0; k < npick && !dup; k++)
                    dup = c.a.outcomes[pick[k]].h.machine == c.a.outcomes[i].h.machine &&
                          (c.pic[i] ? c.pic[pick[k]] == c.pic[i] : c.a.outcomes[pick[k]].h.t.no_signal);
                if (dup) continue;
                pick[npick++] = i;
                any = true;
                break;
            }
        }
    }
    for (int l = 0; l < ATLAS_NMACHINES * ATLAS_NMODES; l++) free(lists[l]);

    /* Run each one's first fault again, all at once, for its picture at
     * full size. */
    printf("Synthetic capture card test: %d different screens of %s, seed %u\n", npick, op->path, seed);
    /* A capture already in OUT_DIR is used again as it is (delete the
     * folder for new ones). */
    rerun_t *rr = calloc((size_t)npick, sizeof(rerun_t));
    pthread_t *tid = malloc((size_t)npick * sizeof(pthread_t));
    char (*paths)[700] = malloc((size_t)npick * sizeof(*paths));
    int nrun = 0;
    for (int k = 0; k < npick; k++) {
        const atlas_outcome_t *o = &c.a.outcomes[pick[k]];
        snprintf(paths[k], sizeof(paths[k]), "%s/capture-%02d-%s-%s.png", out_dir, k + 1, atlas_machine_id(o->h.machine),
                 atlas_mode_id(o->h.mode));
        rr[k] = (rerun_t){ pick[k], c.a.faults[o->faults[0]].spec, o->h.machine, o->h.mode, false, false, false, NULL,
                           NULL, "", op->carts };
        FILE *f = fopen(paths[k], "rb");
        rr[k].reuse = f != NULL;
        if (f) fclose(f);
        else nrun++;
    }
    if (nrun) printf("running %d of their faults again for full size pictures...\n", nrun);
    if (nrun < npick) printf("%d captures already in %s used again\n", npick - nrun, out_dir);
    /* As many at a time as the CPU has threads (each is a whole board); one
     * whose thread won't start runs here. */
    int wave = atlas_logical_processors();
    bool *started = calloc((size_t)npick + 1, sizeof(bool));
    for (int first = 0; first < npick; first += wave) {
        int last = first + wave < npick ? first + wave : npick;
        void **ret = calloc((size_t)npick + 1, sizeof(void *));
        for (int k = first; k < last; k++)
            if (!rr[k].reuse) started[k] = pthread_create(&tid[k], NULL, rerun_worker, &rr[k]) == 0;
        for (int k = first; k < last; k++) {
            if (rr[k].reuse) { rr[k].ok = rr[k].same_key = true; continue; }
            if (started[k]) pthread_join(tid[k], &ret[k]);
            else ret[k] = rerun_worker(&rr[k]);
            rr[k].same_key = rr[k].ok && (uint64_t)(uintptr_t)ret[k] == c.a.outcomes[pick[k]].h.key;
        }
        free(ret);
    }
    free(started);
    free(tid);

    int top1 = 0, top3 = 0, etop1 = 0, etop3 = 0, n = 0;
    printf("\n  #  screen (true group)                                              true rank  same-picture rank  "
           "#1 match\n");
    for (int k = 0; k < npick; k++) {
        const atlas_outcome_t *o = &c.a.outcomes[pick[k]];
        char label[256];
        atlas_outcome_label(o, label, sizeof(label));
        if (!rr[k].ok) { printf(" %2d  %s: rerun failed: %s\n", k + 1, label, rr[k].err); continue; }
        const char *path = paths[k];
        if (!rr[k].reuse) {
            int sw, sh, ow, oh;
            atlas_screen_size(o->h.machine, &sw, &sh);
            rng_state = 0x9E3779B97F4A7C15ull * (seed + 1) + (uint64_t)(k + 1) * 0xBF58476D1CE4E5B9ull;
            degrade_t d;
            uint32_t *pic = degrade(rr[k].lit ? rr[k].lit : rr[k].frame, sw, sh, &d, &ow, &oh);
            png_write(path, pic, ow, oh);
            free(pic);
        }
        /* Against the screen's own board, as the window matches against
         * the board on the bench: the Spectrum's issues have the same
         * pictures. */
        atlas_match_opts_t own = *op;
        if (own.machine < 0) own.machine = o->h.machine;
        match_t M;
        if (!match_picture(&c, path, &own, &M, NULL, 0)) continue;
        int rank = 0, erank = 0;
        for (int r = 0; r < M.nc; r++) {
            if (!rank && M.c[r].outcome == pick[k]) rank = r + 1;
            /* The same picture: one of the group's pictures is the very
             * one shown (the lit one of a flash code). */
            uint64_t shown_pic = c.one[o->h.alt_enc == THUMB_RLE ? 1 : 0][pick[k]];
            int q = M.c[r].outcome;
            bool same = q == pick[k] || (shown_pic && (c.one[0][q] == shown_pic || c.one[1][q] == shown_pic));
            if (!erank && same) erank = r + 1;
        }
        n++;
        top1 += rank == 1; top3 += rank >= 1 && rank <= 3;
        etop1 += erank == 1; etop3 += erank >= 1 && erank <= 3;
        char top[256];
        atlas_outcome_label(&c.a.outcomes[M.c[0].outcome], top, sizeof(top));
        char shown[64];
        snprintf(shown, sizeof(shown), "%s %s", atlas_machine_id(o->h.machine), atlas_mode_id(o->h.mode));
        printf(" %2d  %-9s %-55.55s  %3d (%4.1f%%)  %3d               %s%.60s (%.1f%%)\n", k + 1, shown, label, rank,
               rank ? 100 * M.c[rank - 1].score : 0, erank, erank == 1 ? "= " : "", erank == 1 ? "" : top,
               100 * M.c[0].score);
        if (!rr[k].same_key) printf("     (warning: the rerun didn't give the atlas's outcome again)\n");
        match_free(&M);
    }
    printf("\n%d screens: the true group #1 for %d, in the top 3 for %d.\n", n, top1, top3);
    printf("Counting a group with the very same picture as right (one screenshot can't tell them apart):\n"
           "  #1 for %d, top 3 for %d.\nThe degraded pictures are in %s\n", etop1, etop3, out_dir);
    for (int k = 0; k < npick; k++) { free(rr[k].frame); free(rr[k].lit); }
    free(paths);
    free(rr);
    free(pick);
    ctx_free(&c);
    return 0;
}
