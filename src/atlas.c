/* atlas.c - the fault atlas (see atlas.h): the catalog of faults, running
 * them headless on worker threads, the fingerprint of a picture, and the
 * atlas file.
 *
 * Threads. Every run has its own board_t, and everything a board does is
 * in it. What boards share is read only once built: the board tables, the
 * PLA lookup table (built here, before the workers start), the palettes.
 * The workers only take the lock to append their result to the file. They
 * don't call win_boost_current_thread(): the scheduler is free to put them
 * on every logical processor, E-cores included. */
#include "atlas.h"
#include "board.h"
#include "c64.h"
#include "zx48.h"
#include "png.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* The file stores these as they are in memory. */
_Static_assert(sizeof(atlas_outcome_hdr_t) == 1144, "atlas_outcome_hdr_t layout changed");
_Static_assert(sizeof(atlas_fault_hdr_t) == 24, "atlas_fault_hdr_t layout changed");

#define ATLAS_MAGIC   "HWATLAS"     /* + a version byte */
#define ATLAS_VERSION 1
#define TAG_OUTCOME   0x4354554Fu   /* "OUTC" */
#define TAG_FAULT     0x544C4146u   /* "FALT" */
#define TAG_NETS      0x5354454Eu   /* "NETS" */
#define TAG_PINS      0x534E4950u   /* "PINS" */
#define TAG_LEGS      0x5347454Cu   /* "LEGS" */
#define MAX_LEGS      8

static const machine_t *const atlas_machines[ATLAS_NMACHINES] = {
    &machine_c64, &machine_zx48, &machine_zx48i2, &machine_zx48i1, &machine_zx48i4b, &machine_zx48i6a, &machine_c64c };

int atlas_family(int machine) { return machine == ATLAS_C64 || machine == ATLAS_C64C ? ATLAS_C64 : ATLAS_ZX48; }

const machine_t *atlas_machine(int machine) { return atlas_machines[machine]; }

bool atlas_same_refs(int a, int b) { return a == b || (atlas_family(a) == ATLAS_ZX48 && atlas_family(b) == ATLAS_ZX48); }

int atlas_machine_of(const machine_t *m) {
    for (int i = 0; i < ATLAS_NMACHINES; i++)
        if (atlas_machines[i] == m) return i;
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Small tools                                                               */
/* ------------------------------------------------------------------------ */

static uint64_t fnv64(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
#define FNV64_INIT 0xCBF29CE484222325ull

/* A picture's hash, a word at a time (every frame of every run). */
static uint64_t hash_pixels(const uint32_t *px, int n) {
    uint64_t h = FNV64_INIT;
    for (int i = 0; i < n; i++) h = (h ^ px[i]) * 0x100000001B3ull;
    return h;
}

static uint32_t fnv32(const uint8_t *p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

static void appendf(char *buf, int len, const char *fmt, ...) {
    int at = (int)strlen(buf);
    if (at >= len - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + at, (size_t)(len - at), fmt, ap);
    va_end(ap);
}

/* A growing byte buffer. */
typedef struct { uint8_t *p; size_t n, cap; } buf_t;

static void buf_put(buf_t *b, const void *data, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}
static void buf_byte(buf_t *b, uint8_t v) { buf_put(b, &v, 1); }

/* A set of 64 bit keys (open addressing; 0 is stored as 1). */
typedef struct { uint64_t *v; size_t cap, n; } hset_t;

static bool hset_add(hset_t *s, uint64_t k) {
    if (!k) k = 1;
    if (2 * (s->n + 1) > s->cap) {
        hset_t t = { calloc(s->cap ? s->cap * 2 : 1024, sizeof(uint64_t)), s->cap ? s->cap * 2 : 1024, 0 };
        for (size_t i = 0; i < s->cap; i++) if (s->v[i]) hset_add(&t, s->v[i]);
        free(s->v);
        *s = t;
    }
    size_t i = (size_t)(k * 0x9E3779B97F4A7C15ull) & (s->cap - 1);
    while (s->v[i]) {
        if (s->v[i] == k) return false;
        i = (i + 1) & (s->cap - 1);
    }
    s->v[i] = k;
    s->n++;
    return true;
}

static bool hset_has(const hset_t *s, uint64_t k) {
    if (!k) k = 1;
    if (!s->cap) return false;
    size_t i = (size_t)(k * 0x9E3779B97F4A7C15ull) & (s->cap - 1);
    while (s->v[i]) {
        if (s->v[i] == k) return true;
        i = (i + 1) & (s->cap - 1);
    }
    return false;
}

static uint64_t fault_key(int machine, int mode, const char *spec) {
    uint8_t mm[2] = { (uint8_t)machine, (uint8_t)mode };
    return fnv64(fnv64(FNV64_INIT, mm, 2), spec, strlen(spec));
}

static double now_seconds(void) {
#ifdef _WIN32
    return GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

int atlas_logical_processors(void) {
#ifdef _WIN32
    int n = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n > 0 ? n : 1;
}

/* ------------------------------------------------------------------------ */
/* Machines, modes, palettes                                                 */
/* ------------------------------------------------------------------------ */

const char *atlas_machine_id(int machine) { return atlas_machines[machine]->id; }

const char *atlas_mode_name(int mode) {
    return mode == MODE_BOOT ? "Normal boot" : mode == MODE_DEADTEST ? "Dead Test cartridge" : "ZX Diagnostics ROM";
}
static const char *const mode_ids[ATLAS_NMODES] = { "boot", "deadtest", "zxdiag" };

/* How each cartridge mode runs. MIN_FRAME: no early out on a dead CPU
 * before it (the normal boot keeps the screen blank while the ROM tests
 * the RAM, ~100-130 frames, which is not a dead machine). STATIC: frames
 * of an unchanging picture that end a long run (0: never: on a normal
 * boot some Spectrum pictures hold still for a second, then change again),
 * not before
 * STATIC_FROM: Dead Test keeps the screen black for 8-12 s while it tests
 * the RAM before showing anything. FLASH_CODES: count the flashes as a
 * code (Dead Test's RAM error flash code); elsewhere a flashing screen
 * (the Spectrum's RAM FAIL) is just a picture that changes. The ZX
 * Diagnostics ROM tests the lower RAM, then asks for the model: the "1"s
 * typed choose the 48K tests (as in tests/zx-diag.sh). */
static const struct {
    int max_frames, min_frame, static_frames, static_from;
    bool flash_codes;
    const char *cart;
    const char *type;
} modes[ATLAS_NMODES] = {
    { 200, 130, 0, 0, false, NULL, NULL },
    { 1500, 100, 150, 700, true, "deadtest-781220.crt", NULL },
    { 3000, 1000, 250, 1000, false, "zx-diagnostics-v0.37.rom",
      "111111111111111111111111111111111111111111111111111111111111" },
};

static bool mode_fits(int machine, int mode) {
    return mode == MODE_BOOT || (mode == MODE_DEADTEST) == (atlas_family(machine) == ATLAS_C64);
}

/* The active display (the rest of the picture is the border), per family. */
static const struct { int x, y, w, h; } active_area[2] = {
    { 32, 36, 320, 200 },   /* C64: 40x25 characters */
    { 48, 48, 256, 192 },   /* Spectrum: 32x24 */
};

static uint32_t palettes[ATLAS_NMACHINES][16];
static bool palettes_ready;

static void init_palettes(void) {
    if (palettes_ready) return;
    chips_range_t r = m6569_palette();
    memcpy(palettes[ATLAS_C64], r.ptr, sizeof(palettes[ATLAS_C64]));
    for (int i = 0; i < 16; i++) palettes[ATLAS_ZX48][i] = ula_color(i & 7, i & 8);
    for (int m = 0; m < ATLAS_NMACHINES; m++)
        if (m != atlas_family(m)) memcpy(palettes[m], palettes[atlas_family(m)], sizeof(palettes[m]));
    palettes_ready = true;
}

const uint32_t *atlas_palette(int machine) {
    init_palettes();
    return palettes[machine];
}

const char *atlas_mode_id(int mode) { return mode_ids[mode]; }

void atlas_active_area(int machine, int *x, int *y, int *w, int *h) {
    *x = active_area[atlas_family(machine)].x;
    *y = active_area[atlas_family(machine)].y;
    *w = active_area[atlas_family(machine)].w;
    *h = active_area[atlas_family(machine)].h;
}

void atlas_screen_size(int machine, int *w, int *h) {
    *w = atlas_machines[machine]->screen_w;
    *h = atlas_machines[machine]->screen_h;
}

void atlas_init_shared(void) {
    pla_logic(0);
    png_init();
    init_palettes();
}

static const char *const color_names[2][16] = {
    { "black", "white", "red", "cyan", "purple", "green", "blue", "yellow", "orange", "brown",
      "light red", "dark grey", "grey", "light green", "light blue", "light grey" },
    { "black", "blue", "red", "magenta", "green", "cyan", "yellow", "white", "black", "bright blue",
      "bright red", "bright magenta", "bright green", "bright cyan", "bright yellow", "bright white" },
};

const char *atlas_color_name(int machine, int index) { return color_names[atlas_family(machine)][index & 15]; }

/* The palette index nearest to a monitor color (exact for the colors the
 * chips draw; the black and white picture of a C64 without its color
 * clock falls on the greys). */
static int nearest_color(int machine, uint32_t c) {
    const uint32_t *pal = palettes[machine];
    int best = 0;
    long bestd = -1;
    for (int i = 0; i < 16; i++) {
        if (pal[i] == c) return i;
        long dr = (long)(c & 0xFF) - (pal[i] & 0xFF), dg = (long)((c >> 8) & 0xFF) - ((pal[i] >> 8) & 0xFF),
             db = (long)((c >> 16) & 0xFF) - ((pal[i] >> 16) & 0xFF);
        long d = dr * dr + dg * dg + db * db;
        if (bestd < 0 || d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static void to_indices(int machine, const uint32_t *px, int n, uint8_t *idx) {
    uint32_t last = px[0];
    int li = nearest_color(machine, last);
    for (int i = 0; i < n; i++) {
        if (px[i] != last) { last = px[i]; li = nearest_color(machine, last); }
        idx[i] = (uint8_t)li;
    }
}

/* ------------------------------------------------------------------------ */
/* Fingerprint and thumbnail                                                 */
/* ------------------------------------------------------------------------ */

/* Autocorrelation of a profile at lags 1..ATLAS_LAGS, and its strongest
 * local peak from lag 2 on (lag 1 is always high on anything smooth). */
static void autocorr(const int *p, int n, uint8_t *out, uint8_t *period, uint8_t *strength) {
    double mean = 0, den = 0, r[ATLAS_LAGS + 2] = { 0 };
    for (int i = 0; i < n; i++) mean += p[i];
    mean /= n;
    for (int i = 0; i < n; i++) den += (p[i] - mean) * (p[i] - mean);
    *period = *strength = 0;
    if (den <= 0) return;
    for (int lag = 1; lag <= ATLAS_LAGS + 1 && lag < n; lag++) {
        double s = 0;
        for (int i = 0; i + lag < n; i++) s += (p[i] - mean) * (p[i + lag] - mean);
        r[lag] = s / den < 0 ? 0 : s / den;
    }
    for (int lag = 1; lag <= ATLAS_LAGS; lag++) out[lag - 1] = (uint8_t)(r[lag] * 255 + 0.5);
    double best = 0.05;
    for (int lag = 2; lag <= ATLAS_LAGS; lag++)
        if (r[lag] > r[lag - 1] && r[lag] >= r[lag + 1] && r[lag] > best) {
            best = r[lag];
            *period = (uint8_t)lag;
            *strength = (uint8_t)(r[lag] * 255 + 0.5);
        }
}

static void fingerprint(int machine, const uint8_t *idx, int w, int h, atlas_fp_t *fp) {
    memset(fp, 0, sizeof(*fp));
    int ax = active_area[atlas_family(machine)].x, ay = active_area[atlas_family(machine)].y;
    int aw = active_area[atlas_family(machine)].w, ah = active_area[atlas_family(machine)].h;
    uint32_t bh[16] = { 0 }, act[16] = { 0 }, nb = 0, na = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool inside = x >= ax && x < ax + aw && y >= ay && y < ay + ah;
            if (inside) { act[idx[y * w + x]]++; na++; } else { bh[idx[y * w + x]]++; nb++; }
        }
    int bg = 0;
    for (int i = 0; i < 16; i++) {
        fp->border_hist[i] = (uint8_t)(nb ? (bh[i] * 255 + nb / 2) / nb : 0);
        fp->active_hist[i] = (uint8_t)(na ? (act[i] * 255 + na / 2) / na : 0);
        if (act[i] > act[bg]) bg = i;
    }
    fp->cells_w = (uint8_t)(aw / 8);
    fp->cells_h = (uint8_t)(ah / 8);
    for (int cy = 0; cy < fp->cells_h; cy++)
        for (int cx = 0; cx < fp->cells_w; cx++) {
            int count[16] = { 0 }, colors = 0, dom = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) count[idx[(ay + cy * 8 + y) * w + ax + cx * 8 + x]]++;
            for (int i = 0; i < 16; i++) {
                if (count[i]) colors++;
                if (count[i] > count[dom]) dom = i;
            }
            fp->cells[cy][cx] = (uint8_t)(dom | ((colors > 4 ? 3 : colors - 1) << 4));
        }
    int *col = calloc((size_t)aw, sizeof(int)), *row = calloc((size_t)ah, sizeof(int));
    for (int y = 0; y < ah; y++)
        for (int x = 0; x < aw; x++)
            if (idx[(ay + y) * w + ax + x] != bg) { col[x]++; row[y]++; }
    autocorr(col, aw, fp->hcorr, &fp->hperiod, &fp->hstrength);
    autocorr(row, ah, fp->vcorr, &fp->vperiod, &fp->vstrength);
    free(col);
    free(row);
}

/* Half resolution: the most frequent color of every 2x2 block; on a tie,
 * not the background, so one pixel wide strokes (text) survive. */
void atlas_half(const uint8_t *idx, int w, int h, uint8_t *out) {
    uint32_t hist[16] = { 0 };
    for (int i = 0; i < w * h; i++) hist[idx[i]]++;
    int bg = 0;
    for (int i = 1; i < 16; i++) if (hist[i] > hist[bg]) bg = i;
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            const uint8_t *p = &idx[2 * y * w + 2 * x];
            uint8_t v[4] = { p[0], p[1], p[w], p[w + 1] };
            int best = 0, bestn = 0;
            for (int i = 0; i < 4; i++) {
                int n = (v[0] == v[i]) + (v[1] == v[i]) + (v[2] == v[i]) + (v[3] == v[i]);
                if (n > bestn || (n == bestn && v[best] == bg && v[i] != bg)) { best = i; bestn = n; }
            }
            out[y * (w / 2) + x] = v[best];
        }
}

static void rle_encode(const uint8_t *idx, int n, buf_t *out) {
    for (int i = 0; i < n; ) {
        int run = 1;
        while (i + run < n && idx[i + run] == idx[i]) run++;
        if (run <= 15) buf_byte(out, (uint8_t)((run - 1) << 4 | idx[i]));
        else {
            buf_byte(out, (uint8_t)(0xF0 | idx[i]));
            for (unsigned v = (unsigned)(run - 16); ; v >>= 7) {
                if (v < 0x80) { buf_byte(out, (uint8_t)v); break; }
                buf_byte(out, (uint8_t)(0x80 | (v & 0x7F)));
            }
        }
        i += run;
    }
}

static bool rle_decode(const uint8_t *p, size_t len, uint8_t *out, int n) {
    size_t i = 0;
    int at = 0;
    while (at < n) {
        if (i >= len) return false;
        uint8_t b = p[i++];
        int run = (b >> 4) + 1;
        if (run == 16) {
            unsigned v = 0;
            for (int shift = 0; ; shift += 7) {
                if (i >= len || shift > 28) return false;
                uint8_t c = p[i++];
                v |= (unsigned)(c & 0x7F) << shift;
                if (!(c & 0x80)) break;
            }
            run = 16 + (int)v;
        }
        if (at + run > n) return false;
        memset(out + at, b & 15, (size_t)run);
        at += run;
    }
    return i == len;
}

/* A thumbnail of a picture, checked by decoding it again. */
static bool make_thumb(int machine, const uint32_t *px, int w, int h, buf_t *out) {
    uint8_t *idx = malloc((size_t)w * h), *half = malloc((size_t)(w / 2) * (h / 2)),
            *check = malloc((size_t)(w / 2) * (h / 2));
    to_indices(machine, px, w * h, idx);
    atlas_half(idx, w, h, half);
    rle_encode(half, (w / 2) * (h / 2), out);
    bool ok = rle_decode(out->p, out->n, check, (w / 2) * (h / 2)) &&
              !memcmp(check, half, (size_t)(w / 2) * (h / 2));
    free(idx); free(half); free(check);
    return ok;
}

bool atlas_decode_thumb(const atlas_outcome_t *o, bool alt, uint8_t *out) {
    int enc = alt ? o->h.alt_enc : o->h.enc;
    if (enc != THUMB_RLE) return false;
    return rle_decode(alt ? o->alt : o->thumb, alt ? o->h.alt_len : o->h.len, out, o->h.w * o->h.h);
}

/* ------------------------------------------------------------------------ */
/* One run                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint8_t machine, mode;
    char spec[96];              /* space separated fault specs, "" healthy */
    char desc[700];
    double cost;                /* estimated, to run the longest first */
    int order;
    bool pins_only;             /* in the file already but for its PINS */
} job_t;

typedef struct {
    bool bad;                   /* a spec or the cartridge didn't load */
    char err[300];
    int stop, cpu, frames;
    atlas_outcome_hdr_t oh;
    buf_t thumb, alt;
    bool keep;                  /* hand over the last pictures (full size) */
    uint32_t *frame, *lit;      /* lit: NULL but for a flash code */
    uint8_t *nets; int nnets;   /* the logic probe at the end (PINS) */
    int legs[MAX_LEGS], nlegs;  /* the open chip legs (LEGS) */
    uint8_t leg_state[MAX_LEGS];
    char leg_name[MAX_LEGS][16];
} result_t;

/* A JAM opcode keeps the 6502 fetching forever (only a reset gets it out);
 * a Z80 halted with interrupts off is stuck the same way. */
static int cpu_state(board_t *b) {
    if (board_is_c64(b)) {
        c64_t *c = C64(b);
        if (!c->cpu_running) return CPU_STOPPED;
        if (c->cpu_in_reset) return CPU_RESET;
        unsigned op = c->cpu.IR >> 3;
        if ((op & 0x0F) == 0x02 && (op < 0x80 || (op & 0x10))) return CPU_JAMMED;
        return CPU_RUNNING;
    }
    zx48_t *z = ZX(b);
    if (!z->cpu_running) return CPU_STOPPED;
    if (z->in_reset) return CPU_RESET;
    if ((z->cpu_pins & Z80_HALT) && !z->cpu.iff1) return CPU_JAMMED;
    return CPU_RUNNING;
}

static int luma(uint32_t c) {
    return (int)(((c & 0xFF) * 299 + ((c >> 8) & 0xFF) * 587 + ((c >> 16) & 0xFF) * 114) / 1000);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Run a fault the way "--headless" does (cartridge in, faults on, run),
 * watching the picture every frame.
 *
 * Early out. A run ends before its last frame when nothing more can
 * happen: snow for a second (no video at all: the video chip is dead);
 * a picture that hasn't changed for a second while the CPU is stopped,
 * held in reset, jammed or halted with interrupts off; on the long
 * cartridge runs, a picture that hasn't changed for STATIC frames, or a
 * flash code seen twice the same. Never before MIN_FRAME but for snow.
 *
 * Flashes are counted like main.c's --flashes: the middle of the screen
 * lit for less than a second is a flash, a dark pause of more than 0.8 s
 * starts a new series. */
static void run_job(const job_t *j, const char *carts, result_t *r) {
    const machine_t *m = atlas_machines[j->machine];
    int mode = j->mode;
    board_t *b = board_create(m);
    char msg[256];
    if (modes[mode].cart) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", carts, modes[mode].cart);
        if (!board_load(b, path, false, msg, sizeof(msg))) {
            r->bad = true;
            snprintf(r->err, sizeof(r->err), "%s", msg);
            board_destroy(b);
            return;
        }
    }
    for (const char *s = j->spec; *s; ) {
        size_t len = strcspn(s, " ");
        char one[64];
        snprintf(one, sizeof(one), "%.*s", (int)len, s);
        if (len && !board_apply_fault_spec(b, one)) {
            r->bad = true;
            snprintf(r->err, sizeof(r->err), "bad fault spec %s", one);
            board_destroy(b);
            return;
        }
        s += len + (s[len] == ' ');
    }
    /* Open chip legs: what the chip drives on them is counted too. */
    r->nlegs = atlas_open_legs(b, j->spec, r->legs, MAX_LEGS);
    if (r->nlegs) b->pin_stats = true;
    uint32_t lwin[MAX_LEGS][3] = { { 0 } };

    int w = m->screen_w, h = m->screen_h, n = w * h;
    uint32_t *px = malloc((size_t)n * sizeof(uint32_t)), *lit_px = malloc((size_t)n * sizeof(uint32_t));
    int ready = (int)(m->ready_cycles / (uint64_t)m->cycles_per_frame);
    const char *type = modes[mode].type;
    int typed = 0, next_type = 0;
    uint64_t ring[64], prev = 0;
    int same = 0, snow = 0;
    int change_at[8], nchanges = 0;
    /* Flash counting. */
    bool lit = false, have_lit = false;
    int lit_since = 0, dark_since = 0, cur = 0, nseries = 0, series[16];
    int flashes = 0, last_start = -1;
    long lit_total = 0, gap_total = 0, ngaps = 0;
    int stop = STOP_END, f;
    /* The logic probe of every net counts over the last 25 to 50 frames:
     * the statistics start over every 25, the window before kept. */
    int nnets = b->sim.nnets;
    uint32_t *win = calloc((size_t)nnets * 4, sizeof(uint32_t));
    for (f = 0; f < modes[mode].max_frames; f++) {
        board_run(b, m->cycles_per_frame);
        board_frame_update(b, 20000);
        if (f % 25 == 24) {
            size_t sz = (size_t)nnets * sizeof(uint32_t);
            memcpy(win, b->st_hi, sz);
            memcpy(win + nnets, b->st_lo, sz);
            memcpy(win + 2 * nnets, b->st_float, sz);
            memcpy(win + 3 * nnets, b->st_fight, sz);
            for (int k = 0; k < r->nlegs; k++) {
                lwin[k][0] = b->pst_hi[r->legs[k]];
                lwin[k][1] = b->pst_lo[r->legs[k]];
                lwin[k][2] = b->pst_off[r->legs[k]];
            }
            board_stats_reset(b);
        }
        if (type && f >= ready + 10 && f >= next_type && f % 4 == 0 && type[typed]) {
            int c = type[typed++];
            next_type = f + (type[typed] == c ? 12 : 4);
            board_key_down(b, c);
            board_key_up(b, c);
        }
        board_monitor(b, px);
        uint64_t hash = hash_pixels(px, n);
        if (f && hash == prev) same++;
        else if (f) {
            same = 0;
            change_at[nchanges++ & 7] = f;
        }
        prev = ring[f & 63] = hash;
        snow = b->no_signal ? snow + 1 : 0;
        bool l = !b->no_signal && luma(px[(h / 2) * w + w / 2]) > 128;
        if (l) { memcpy(lit_px, px, (size_t)n * sizeof(uint32_t)); have_lit = true; }
        if (l && !lit) lit_since = f;
        if (!l && lit) {
            if (f - lit_since < 50) {
                if (cur && lit_since - dark_since > 40) {
                    if (nseries < 16) series[nseries++] = cur;
                    cur = 0;
                    last_start = -1;
                }
                cur++;
                flashes++;
                lit_total += f - lit_since;
                if (last_start >= 0) { gap_total += lit_since - last_start; ngaps++; }
                last_start = lit_since;
            }
            dark_since = f;
        }
        lit = l;
        int done = f + 1, cpu = cpu_state(b);
        if (snow >= 50) { stop = STOP_NO_SIGNAL; break; }
        if (done >= modes[mode].min_frame && same >= 50 && cpu != CPU_RUNNING) { stop = STOP_CPU_DEAD; break; }
        if (modes[mode].static_frames && done >= modes[mode].static_from && same >= modes[mode].static_frames) {
            stop = STOP_STATIC;
            break;
        }
        if (modes[mode].flash_codes && nseries >= 2 && series[nseries - 1] == series[nseries - 2]) {
            stop = STOP_FLASH_CODE;
            break;
        }
    }
    if (!modes[mode].flash_codes) nseries = 0;      /* flashing, but not a code */
    int frames = stop == STOP_END ? modes[mode].max_frames : f + 1;
    r->stop = stop;
    r->frames = frames;
    r->cpu = cpu_state(b);

    /* Behavior over time. */
    atlas_outcome_hdr_t *oh = &r->oh;
    memset(oh, 0, sizeof(*oh));
    atlas_temporal_t *t = &oh->t;
    t->no_signal = b->no_signal;
    t->nseries = (uint8_t)(nseries < 8 ? nseries : 8);
    for (int i = 0; i < t->nseries; i++) t->series[i] = (uint8_t)series[i];
    t->flash_lit = (uint16_t)(flashes ? lit_total / flashes : 0);
    t->flash_period = (uint16_t)(ngaps ? gap_total / ngaps : 0);
    /* The pictures of the last 64 frames, or only the last one when it has
     * stayed the same for a second (so an early out and a full run of the
     * same still picture agree). */
    int window = same >= 50 ? 1 : frames < 64 ? frames : 64, ndistinct = 0, changes = 0;
    uint64_t last[64];
    for (int i = 0; i < window; i++) last[i] = ring[(frames - window + i) & 63];
    for (int i = 1; i < window; i++) changes += last[i] != last[i - 1];
    qsort(last, (size_t)window, sizeof(uint64_t), cmp_u64);
    for (int i = 0; i < window; i++) if (!i || last[i] != last[i - 1]) last[ndistinct++] = last[i];
    t->distinct = (uint8_t)ndistinct;
    t->changes = (uint8_t)changes;
    if (nchanges >= 3 && window > 1) {     /* regular changes: blinking */
        int k = nchanges < 8 ? nchanges : 8, gaps[7] = { 0 }, ng = 0;
        for (int i = 1; i < k; i++) gaps[ng++] = change_at[(nchanges - k + i) & 7] - change_at[(nchanges - k + i - 1) & 7];
        bool regular = true;
        for (int i = 1; i < ng; i++) if (abs(gaps[i] - gaps[0]) > 1) regular = false;
        if (regular) t->blink_period = (uint16_t)gaps[0];
    }

    /* The outcome key. */
    uint8_t mm[2] = { j->machine, j->mode };
    uint64_t key = fnv64(FNV64_INIT, mm, 2);
    if (t->no_signal) key = fnv64(key, "snow", 4);
    else if (nseries && have_lit) {
        uint64_t lh = hash_pixels(lit_px, n);
        key = fnv64(key, "flash", 5);
        key = fnv64(key, &series[0], sizeof(series[0]));
        key = fnv64(key, &lh, sizeof(lh));
    } else if (ndistinct <= 4) key = fnv64(key, last, (size_t)ndistinct * sizeof(uint64_t));
    else {
        key = fnv64(key, "anim", 4);
        key = fnv64(key, &prev, sizeof(prev));
    }
    oh->key = key;
    oh->machine = j->machine;
    oh->mode = j->mode;
    oh->w = (uint16_t)(w / 2);
    oh->h = (uint16_t)(h / 2);
    if (!t->no_signal) {
        uint8_t *idx = malloc((size_t)n);
        to_indices(j->machine, px, n, idx);
        fingerprint(j->machine, idx, w, h, &oh->fp);
        free(idx);
        oh->enc = THUMB_RLE;
        if (!make_thumb(j->machine, px, w, h, &r->thumb)) {
            r->bad = true;
            snprintf(r->err, sizeof(r->err), "thumbnail does not decode back");
        }
        oh->len = (uint32_t)r->thumb.n;
        if (nseries && have_lit) {
            oh->alt_enc = THUMB_RLE;
            if (!make_thumb(j->machine, lit_px, w, h, &r->alt)) {
                r->bad = true;
                snprintf(r->err, sizeof(r->err), "flash thumbnail does not decode back");
            }
            oh->alt_len = (uint32_t)r->alt.n;
        }
    }
    if (r->keep) {
        r->frame = px;
        r->lit = oh->alt_enc == THUMB_RLE ? lit_px : NULL;
        if (!r->lit) free(lit_px);
    } else {
        free(px);
        free(lit_px);
    }
    /* The logic probe (PINS). A machine still running when the pictures
     * are done runs on, and is probed where a person probes a real one:
     * once it has settled, frames ATLAS_PROBE_FROM to ATLAS_PROBE_TO (on
     * a C64 the first seconds are the RAM test and the start of BASIC). A
     * run that ended early (dead, stuck) has settled already: its last 25
     * to 50 frames count. */
    r->nnets = nnets;
    r->nets = malloc((size_t)nnets);
    if (stop == STOP_END && mode == MODE_BOOT && !r->keep) {
        for (int k = frames; k < ATLAS_PROBE_TO; k++) {
            if (k == ATLAS_PROBE_FROM) board_stats_reset(b);
            board_run(b, m->cycles_per_frame);
            board_frame_update(b, 20000);
        }
    } else
        for (int i = 0; i < nnets; i++) {
            b->st_hi[i] += win[i];
            b->st_lo[i] += win[nnets + i];
            b->st_float[i] += win[2 * nnets + i];
            b->st_fight[i] += win[3 * nnets + i];
        }
    if (!(stop == STOP_END && mode == MODE_BOOT && !r->keep))
        for (int k = 0; k < r->nlegs; k++) {
            b->pst_hi[r->legs[k]] += lwin[k][0];
            b->pst_lo[r->legs[k]] += lwin[k][1];
            b->pst_off[r->legs[k]] += lwin[k][2];
        }
    for (int i = 0; i < nnets; i++) r->nets[i] = (uint8_t)board_net_state(b, i);
    for (int k = 0; k < r->nlegs; k++) {
        const sim_t *s = &b->sim;
        r->leg_state[k] = (uint8_t)board_pin_drive_state(b, r->legs[k]);
        snprintf(r->leg_name[k], sizeof(r->leg_name[k]), "%s.%s", s->parts[s->pins[r->legs[k]].part].ref,
                 s->bd->pads[r->legs[k]].name);
    }
    free(win);
    board_destroy(b);
}

bool atlas_run_fault(int machine, int mode, const char *spec, const char *carts, uint64_t *key, uint32_t **frame,
                     uint32_t **lit, char *err, int errlen) {
    job_t j;
    memset(&j, 0, sizeof(j));
    j.machine = (uint8_t)machine;
    j.mode = (uint8_t)mode;
    snprintf(j.spec, sizeof(j.spec), "%s", spec);
    result_t r;
    memset(&r, 0, sizeof(r));
    r.keep = true;
    run_job(&j, carts, &r);
    free(r.thumb.p);
    free(r.nets);
    free(r.alt.p);
    if (r.bad) {
        snprintf(err, (size_t)errlen, "%s", r.err);
        free(r.frame);
        free(r.lit);
        return false;
    }
    *key = r.oh.key;
    *frame = r.frame;
    *lit = r.lit;
    return true;
}

/* ------------------------------------------------------------------------ */
/* The catalog                                                               */
/* ------------------------------------------------------------------------ */

/* The scenarios of tests/faults.sh and tests/zx-faults.sh, with what the
 * repair guides say they show. */
static const struct { int machine; const char *name, *expected, *specs; } scenarios[] = {
    { ATLAS_C64, "healthy", "READY screen, 38911 BYTES FREE", "" },
    { ATLAS_C64, "cpu_removed", "black screen", "U7:removed" },
    { ATLAS_C64, "pla_removed", "black screen", "U17:removed" },
    { ATLAS_C64, "kernal_removed", "black screen", "U4:removed" },
    { ATLAS_C64, "basic_removed", "screen cleared to blue, no banner text", "U3:removed" },
    { ATLAS_C64, "char_removed", "boots, cursor blinks, garbage characters", "U5:removed" },
    { ATLAS_C64, "colorram_removed", "text in random flickering colors", "U6:removed" },
    { ATLAS_C64, "cia1_removed", "READY screen but no blinking cursor", "U1:removed" },
    { ATLAS_C64, "cia2_removed", "READY screen, looks normal", "U2:removed" },
    { ATLAS_C64, "sid_removed", "normal READY screen", "U18:removed" },
    { ATLAS_C64, "vic_removed", "no picture at all (no sync)", "U19:removed" },
    { ATLAS_C64, "dram_d0_removed", "black screen", "U21:removed" },
    { ATLAS_C64, "dram_d0_q_gnd", "black screen (D0 stuck low for everybody)", "U21.14:gnd" },
    { ATLAS_C64, "dram_d7_q_vcc", "black screen (D7 stuck high for everybody)", "U12.14:vcc" },
    { ATLAS_C64, "ls373_removed", "CPU works, screen garbage (VIC addresses)", "U26:removed" },
    { ATLAS_C64, "cd4066_removed", "colors don't follow: power-on color garbage", "U16:removed" },
    { ATLAS_C64, "cia1_cs_high", "READY but no cursor, keyboard dead", "U15.12:vcc" },
    { ATLAS_C64, "ls7406_removed", "black screen", "U8:removed" },
    { ATLAS_C64, "ls257_removed", "black screen or wrong BYTES FREE", "U13:removed" },
    { ATLAS_C64, "crystal_broken", "black and white picture", "Y1:open" },
    { ATLAS_C64, "vco_removed", "no picture at all (no sync)", "U31:removed" },
    { ATLAS_C64, "timer556_removed", "black screen (reset held or missing)", "U20:removed" },
    { ATLAS_C64, "pla_casram_high", "black screen", "U17.18:vcc" },
    { ATLAS_C64, "pla_charom_high", "boots, garbage characters", "U17.15:vcc" },
    { ATLAS_C64, "game_to_gnd", "black screen (Ultimax mode, no cartridge)", "CN6.8:gnd" },
    { ATLAS_C64, "sid_d3_gnd", "black screen (SID shorts the data bus)", "U18.18:gnd" },
    { ATLAS_C64, "cpu_a15_open", "black screen", "U7.23:open" },
    { ATLAS_C64, "dram_d0_din_open", "black screen or garbage (RAM bit 0 can't be written)", "U21.2:open" },
    { ATLAS_C64, "ls258_removed", "CPU works, VIC shows wrong memory (garbage screen)", "U14:removed" },
    { ATLAS_C64, "ls08_removed", "black screen (RDY/AEC/color RAM select lost)", "U27:removed" },
    { ATLAS_C64, "ls139_removed", "VIC/CIA/SID never selected: black screen, CPU runs", "U15:removed" },
    /* The 250469's (no tests/ script yet): what a repairer expects, written
     * before running them. */
    { ATLAS_C64C, "healthy", "READY screen, 38911 BYTES FREE", "" },
    { ATLAS_C64C, "cpu_removed", "black screen", "U6:removed" },
    { ATLAS_C64C, "superpla_removed", "black screen", "U8:removed" },
    { ATLAS_C64C, "rom_removed", "black screen (BASIC and the KERNAL gone)", "U4:removed" },
    { ATLAS_C64C, "char_removed", "boots, cursor blinks, garbage characters", "U5:removed" },
    { ATLAS_C64C, "colorram_removed", "text in random flickering colors", "U19:removed" },
    { ATLAS_C64C, "cia1_removed", "READY screen but no blinking cursor", "U1:removed" },
    { ATLAS_C64C, "cia2_removed", "READY screen, looks normal", "U2:removed" },
    { ATLAS_C64C, "sid_removed", "normal READY screen", "U9:removed" },
    { ATLAS_C64C, "vic_removed", "no picture at all (no sync)", "U7:removed" },
    { ATLAS_C64C, "ram_low_removed", "black screen", "U10:removed" },
    { ATLAS_C64C, "ram_high_removed", "black screen", "U11:removed" },
    { ATLAS_C64C, "ram_d0_gnd", "black screen (D0 stuck low for everybody)", "U10.2:gnd" },
    { ATLAS_C64C, "ram_d7_vcc", "black screen (D7 stuck high for everybody)", "U11.17:vcc" },
    { ATLAS_C64C, "cd4066_removed", "colors don't follow: power-on color garbage", "U21:removed" },
    { ATLAS_C64C, "ls14_removed", "black screen (reset held: the 74LS06's input floats high)", "U23:removed" },
    { ATLAS_C64C, "ls08_removed", "black screen (no CPU AEC and RDY, no ROM select)", "U3:removed" },
    { ATLAS_C64C, "crystal_broken", "no picture at all (the 8701 makes both clocks from it)", "Y1:open" },
    { ATLAS_C64C, "clock_removed", "no picture at all (no sync)", "U20:removed" },
    { ATLAS_C64C, "por_cap_short", "black screen (reset held)", "C43:short" },
    { ATLAS_C64C, "pla_casram_high", "black screen", "U8.22:vcc" },
    { ATLAS_C64C, "pla_charrom_high", "boots, garbage characters", "U8.13:vcc" },
    { ATLAS_C64C, "game_to_gnd", "black screen (Ultimax mode, no cartridge)", "CN6.8:gnd" },
    { ATLAS_C64C, "sid_d3_gnd", "black screen (SID shorts the data bus)", "U9.18:gnd" },
    { ATLAS_C64C, "cpu_a15_open", "black screen", "U6.23:open" },
    { ATLAS_ZX48, "healthy", "the copyright message, (c) 1982 Sinclair Research Ltd", "" },
    { ATLAS_ZX48, "lram_ic10", "no copyright: the RAM test fails at 16384, its fill pattern stays on screen", "IC10:removed" },
    { ATLAS_ZX48, "lram_ic6", "no copyright: the RAM test fails at 16384", "IC6:removed" },
    { ATLAS_ZX48, "uram_ic15", "the copyright message (it boots as a 16K Spectrum)", "IC15:removed" },
    { ATLAS_ZX48, "no_m5v", "no -5V: the 4116s don't work, no copyright, a broken picture", "D12:open" },
    { ATLAS_ZX48, "no_m5v_4b", "no -5V: the 4116s don't work, no copyright, a broken picture", "D18:open" },
    { ATLAS_ZX48, "no_dcdc", "no +12V and no -5V: same as a dead lower RAM", "TR4:open" },
    { ATLAS_ZX48, "no_rom", "black border, the power-on garbage of the screen RAM", "IC5:removed" },
    { ATLAS_ZX48, "no_cpu", "black border, the power-on garbage of the screen RAM", "IC2:removed" },
    { ATLAS_ZX48, "no_ula", "no picture: snow", "IC1:removed" },
    { ATLAS_ZX48, "no_crystal", "no clock: no picture, the CPU stopped", "X1:open" },
    { ATLAS_ZX48, "no_clock_tr3", "no CPU clock: black border, garbage", "TR3:open" },
    { ATLAS_ZX48, "mux_ic3", "scrambled characters and colours, or no boot", "IC3:removed" },
    { ATLAS_ZX48, "uram_link", "wrong RAM type link: the upper RAM fails, a 16K Spectrum", "TI-3:open" },
};

static const char *const role_names[] = { "RAM", "video RAM", "ROM", "address decoder", "chip", "glue logic",
                                          "clock", "other" };

/* The one chip output on the net of PIN, -1 if none or several (a bus).
 * A net stuck there is that chip's output first: dead inside, or its pin
 * shorted, and the faults are named after it. */
static int net_driver(board_t *b, int pin) {
    sim_t *s = &b->sim;
    int net = pin >= 0 ? s->pins[pin].net : -1;
    if (net < 0) return -1;
    const net_t *n = &s->nets[s->nets[net].root];
    int drv = -1;
    for (int k = 0; k < n->npins; k++) {
        int q = n->pins[k];
        const part_t *qp = &s->parts[s->pins[q].part];
        if (!(s->pins[q].flags & PIN_OUT) || qp->kind != PART_IC || !s->pins[q].number || qp->removed) continue;
        if (drv >= 0) return -1;
        drv = q;
    }
    return drv;
}

/* What a fault spec means, for people: the chip, the pin, the fault, and
 * what the chip does on the board. */
static void describe_spec(board_t *b, const char *spec, char *out, int len) {
    sim_t *s = &b->sim;
    char ref[16] = { 0 }, pad[8] = { 0 }, pad2[8] = { 0 }, what[16] = { 0 };
    if (sscanf(spec, "%15[^.:].%7[^-:]-%7[^:]:%15s", ref, pad, pad2, what) == 4) {     /* a solder bridge */
        int part = sim_find_part(s, ref);
        int pa = part >= 0 ? board_pad_pin(b, part, pad) : -1, pb = part >= 0 ? board_pad_pin(b, part, pad2) : -1;
        if (pa < 0 || pb < 0) { appendf(out, len, "%s", spec); return; }
        char d[160];
        board_bridge_desc(b, pa, pb, d, sizeof(d));
        appendf(out, len, "%s", d);
        const char *info = board_part_info(b, part);
        if (info && *info) appendf(out, len, " - [%s] %s", role_names[board_part_role(b, part)], info);
        return;
    }
    if (sscanf(spec, "%15[^.:].%7[^:]:%15s", ref, pad, what) != 3) {
        pad[0] = 0;
        if (sscanf(spec, "%15[^:]:%15s", ref, what) != 2) { appendf(out, len, "%s", spec); return; }
    }
    int part = sim_find_part(s, ref);
    if (part < 0) { appendf(out, len, "%s", spec); return; }
    const part_t *p = &s->parts[part];
    const char *label = board_part_label(b, part), *info = board_part_info(b, part);
    bool ic = p->kind == PART_IC;
    if (pad[0]) {
        int pin = board_pad_pin(b, part, pad);
        const char *pname = pin >= 0 ? sim_pin_name(s, pin) : NULL;
        int net = pin >= 0 ? s->pins[pin].net : -1;
        const char *nname = net >= 0 ? s->nets[net].name : "?";
        char where[96];
        snprintf(where, sizeof(where), "%s %s pin %s%s%s%s", ref, label, pad, pname ? " (" : "", pname ? pname : "",
                 pname ? ")" : "");
        if (!strcmp(what, "open")) appendf(out, len, "%s open: the leg doesn't reach net %s", where, nname);
        else if (!strcmp(what, "out0") || !strcmp(what, "out1"))
            appendf(out, len, "%s: output dead inside the chip, stuck %s (net %s)", where,
                    what[3] == '1' ? "high" : "low", nname);
        else {
            bool gnd = !strcmp(what, "gnd");
            appendf(out, len, "net %s stuck %s: %s shorted to %s", nname, gnd ? "low" : "high", where,
                    gnd ? "GND" : "+5V");
            if (net_driver(b, pin) == pin) appendf(out, len, ", or the output dead inside the chip");
            const net_t *root = net >= 0 ? &s->nets[s->nets[net].root] : NULL;
            int shown = 0;
            for (int k = 0; root && k < root->npins; k++) {
                int q = root->pins[k];
                const part_t *qp = &s->parts[s->pins[q].part];
                if (q == pin || qp->kind != PART_IC || !s->pins[q].number) continue;
                if (shown == 8) { appendf(out, len, ", ..."); break; }
                const char *qn = sim_pin_name(s, q);
                appendf(out, len, "%s%s.%d%s%s%s", shown ? ", " : "; also on the net: ", qp->ref, s->pins[q].number,
                        qn ? "(" : "", qn ? qn : "", qn ? ")" : "");
                shown++;
            }
        }
    } else if (!strcmp(what, "removed")) appendf(out, len, "%s %s removed (or dead)", ref, label);
    else if (part_memory(p, NULL, NULL) && strcmp(what, "open") && strcmp(what, "short")) {
        mem_fault_t mf;
        char d[160];
        if (!board_parse_mem_fault(b, part, what, &mf)) { appendf(out, len, "%s", spec); return; }
        board_mem_fault_desc(b, part, &mf, d, sizeof(d));
        appendf(out, len, "%s %s %s", ref, label, d);
        if (!strncmp(what, "flip@", 5)) appendf(out, len, ", CPU address $%.4s", what + 5);
    } else {
        int rf = RF_OPEN;
        for (int k = RF_OPEN; k <= RF_WEAK; k++) if (!strcmp(what, board_rfault_spec(k))) rf = k;
        appendf(out, len, "%s %s %s", ref, ic ? label : p->name ? p->name : "", board_rfault_name(b, part, rf));
        if (p->npins == 2) {
            int n0 = s->pins[p->first_pin].net, n1 = s->pins[p->first_pin + 1].net;
            if (n0 >= 0 && n1 >= 0) appendf(out, len, " (between %s and %s)", s->nets[n0].name, s->nets[n1].name);
        }
    }
    if (info && *info) appendf(out, len, " - [%s] %s", role_names[board_part_role(b, part)], info);
}

typedef struct { job_t *v; int n, cap; } joblist_t;

typedef struct {
    joblist_t *list;
    hset_t *seen;               /* fault keys, and stuck nets done */
    board_t *b;
    int machine, mode;
} catalog_t;

static void add_job(catalog_t *c, const char *spec, const char *prefix) {
    if (!hset_add(c->seen, fault_key(c->machine, c->mode, spec))) return;
    joblist_t *L = c->list;
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 256;
        L->v = realloc(L->v, (size_t)L->cap * sizeof(job_t));
    }
    job_t *j = &L->v[L->n];
    memset(j, 0, sizeof(*j));
    j->machine = (uint8_t)c->machine;
    j->mode = (uint8_t)c->mode;
    j->order = L->n++;
    snprintf(j->spec, sizeof(j->spec), "%s", spec);
    if (prefix) appendf(j->desc, sizeof(j->desc), "%s", prefix);
    if (!*spec) appendf(j->desc, sizeof(j->desc), "healthy board, no fault");
    for (const char *s = spec; *s; ) {
        size_t len = strcspn(s, " ");
        char one[64];
        snprintf(one, sizeof(one), "%.*s", (int)len, s);
        if (s != spec) appendf(j->desc, sizeof(j->desc), "; ");
        describe_spec(c->b, one, j->desc, sizeof(j->desc));
        s += len + (s[len] == ' ');
    }
    j->cost = (c->mode == MODE_BOOT ? ATLAS_PROBE_TO : modes[c->mode].max_frames) *
              (atlas_family(c->machine) == ATLAS_ZX48 ? 1.25 : 1.0);
}

/* Which pins of a part a rule takes. */
enum { SEL_SIGNAL, SEL_OUT, SEL_BUS, SEL_DATA, SEL_DRAM_DQ, SEL_SHARED_OUT };
/* What a rule does to them. */
enum { DO_OPEN = 1, DO_GND = 2, DO_VCC = 4, DO_OUT = 8 };

static bool signal_pin(sim_t *s, int pin) {
    const pin_t *p = &s->pins[pin];
    return p->net >= 0 && p->number > 0 && !(p->flags & PIN_POWER) && !s->nets[s->nets[p->net].root].rail;
}

/* An output on a net another chip drives too (a bus): an output dead
 * inside the chip fights the others there, unlike a short to a supply.
 * On a net it drives alone the two look the same: one run (gnd/vcc). */
static bool shared_output(sim_t *s, int pin) {
    if (!(s->pins[pin].flags & PIN_OUT)) return false;
    const net_t *n = &s->nets[s->pins[pin].root];
    for (int k = 0; k < n->npins; k++) {
        int q = n->pins[k];
        if (q != pin && (s->pins[q].flags & PIN_OUT) && s->parts[s->pins[q].part].kind == PART_IC) return true;
    }
    return false;
}

static bool selected(sim_t *s, int pin, int sel) {
    if (!signal_pin(s, pin)) return false;
    const char *name = sim_pin_name(s, pin);
    bool bus = name && (name[0] == 'A' || name[0] == 'D') && name[1] >= '0' && name[1] <= '9';
    switch (sel) {
    case SEL_OUT:     return (s->pins[pin].flags & PIN_OUT) != 0;
    case SEL_BUS:     return bus;
    case SEL_DATA:    return bus && name[0] == 'D';
    case SEL_DRAM_DQ: return s->pins[pin].number == 2 || s->pins[pin].number == 14;   /* Din, Dout */
    case SEL_SHARED_OUT: return shared_output(s, pin);
    default:          return true;
    }
}

/* Faults on the selected pins of a part. A short to GND or +5V puts the
 * whole net there, whichever pin it is on: one run per net, named after
 * the chip driving the net when only one does (net_driver()). */
static void add_pins(catalog_t *c, int part, int sel, int what) {
    sim_t *s = &c->b->sim;
    if (part < 0) return;
    const part_t *p = &s->parts[part];
    for (int k = 0; k < p->npins; k++) {
        int pin = p->first_pin + k;
        if (!selected(s, pin, sel)) continue;
        char spec[64];
        if (what & DO_OPEN) {
            snprintf(spec, sizeof(spec), "%s.%d:open", p->ref, s->pins[pin].number);
            add_job(c, spec, NULL);
        }
        for (int level = 0; level < 2; level++) {
            if (!(what & (level ? DO_VCC : DO_GND))) continue;
            uint64_t netkey = fault_key(c->machine, c->mode, "net") ^ ((uint64_t)s->nets[s->pins[pin].net].root << 1 | level);
            if (!hset_add(c->seen, netkey)) continue;
            int drv = net_driver(c->b, pin), at = drv >= 0 ? drv : pin;
            snprintf(spec, sizeof(spec), "%s.%d:%s", s->parts[s->pins[at].part].ref, s->pins[at].number,
                     level ? "vcc" : "gnd");
            add_job(c, spec, NULL);
        }
        for (int level = 0; (what & DO_OUT) && level < 2; level++) {
            snprintf(spec, sizeof(spec), "%s.%d:out%d", p->ref, s->pins[pin].number, level);
            add_job(c, spec, NULL);
        }
    }
}

static bool sim_ic(board_t *b, int part) {
    return b->sim.parts[part].kind == PART_IC && board_part_simulated(b, part);
}

/* A supply: a rail, or a net with a chip's power pin on it (+12V, -5V). */
static bool power_net(sim_t *s, int root) {
    if (s->nets[root].rail) return true;
    for (int k = 0; k < s->nets[root].npins; k++) if (s->pins[s->nets[root].pins[k]].flags & PIN_POWER) return true;
    return false;
}

/* Solder bridges between the neighbor legs of a chip (pin, pin + 1 on the
 * same side of the DIP), but not: legs already on the same net, a leg
 * that goes nowhere, two supplies (+5V to GND is a dead short, not a
 * symptom), or a net the simulation doesn't model (analog). */
static void add_bridges(catalog_t *c, int part) {
    board_t *b = c->b;
    sim_t *s = &b->sim;
    if (part < 0 || !sim_ic(b, part)) return;
    const part_t *p = &s->parts[part];
    for (int k = 0; k < p->npins; k++) {
        int pa = p->first_pin + k, pb = board_dip_neighbor(b, pa, +1);
        if (pb < 0) continue;
        int ra = s->pins[pa].root, rb = s->pins[pb].root;
        if (ra < 0 || rb < 0 || ra == rb) continue;
        if (s->nets[ra].npins < 2 || s->nets[rb].npins < 2) continue;
        if (power_net(s, ra) && power_net(s, rb)) continue;
        if (!board_net_simulated(b, ra) || !board_net_simulated(b, rb)) continue;
        char spec[32];
        snprintf(spec, sizeof(spec), "%s.%d-%d:short", p->ref, s->pins[pa].number, s->pins[pb].number);
        add_job(c, spec, NULL);
    }
}

static void add_removed(catalog_t *c) {
    for (int i = 0; i < c->b->sim.nparts; i++) {
        if (!sim_ic(c->b, i)) continue;
        char spec[32];
        snprintf(spec, sizeof(spec), "%s:removed", c->b->sim.parts[i].ref);
        add_job(c, spec, NULL);
    }
}

/* One bad cell in the RAM, at CPU addresses where it shows: in every chip
 * holding the address (each bit of a 2114). And corrupted ROM bytes, in
 * text the machine shows when it starts. */
static const struct { int machine; uint16_t addr; } ram_flips[] = {
    { ATLAS_C64, 0x05F4 },      /* the middle of the screen */
    { ATLAS_C64, 0x01F8 },      /* the stack */
    { ATLAS_C64, 0xD831 },      /* the color of the C of COMMODORE */
    { ATLAS_ZX48, 0x53E2 },     /* a pixel row of the copyright line */
    { ATLAS_ZX48, 0xFF4E },     /* the machine stack, under RAMTOP */
};
static const struct { int machine; const char *ref; uint16_t addr; uint8_t bits; } rom_flips[] = {
    { ATLAS_C64, "U4", 0x047E, 0x01 },      /* COMMODORE: BOMMODORE */
    { ATLAS_C64C, "U4", 0x247E, 0x01 },     /* the same, in the 250469's BASIC and KERNAL ROM */
    { ATLAS_C64, "U5", 0x0093, 0x10 },      /* the letter R */
    { ATLAS_ZX48, "IC5", 0x1540, 0x20 },    /* Sinclair: sinclair */
};

/* Does a corrupted ROM byte go in this chip of this board: one of the
 * board's own, or else one of its family's (the 250469's U4 has its own). */
static bool rom_flip_fits(int machine, size_t k, const char *ref) {
    if (strcmp(rom_flips[k].ref, ref)) return false;
    if (rom_flips[k].machine == machine) return true;
    if (rom_flips[k].machine != atlas_family(machine)) return false;
    for (size_t j = 0; j < sizeof(rom_flips) / sizeof(rom_flips[0]); j++)
        if (rom_flips[j].machine == machine && !strcmp(rom_flips[j].ref, ref)) return false;
    return true;
}

/* Faults inside the memory chips: a dead data bit (the whole chip, on a
 * DRAM), dead address lines, weak DRAM cells, and the bad cells and
 * bytes above. */
static void add_memory(catalog_t *c) {
    board_t *b = c->b;
    sim_t *s = &b->sim;
    char spec[48];
    for (int i = 0; i < s->nparts; i++) {
        int bits;
        bool dram;
        int lines = sim_ic(b, i) ? part_memory(&s->parts[i], &bits, &dram) : 0;
        if (!lines) continue;
        const char *ref = s->parts[i].ref;
        for (int bit = 0; bit < bits; bit++)
            for (int v = 0; v < 2; v++) {
                snprintf(spec, sizeof(spec), "%s:data%d=%d", ref, bit, v);
                add_job(c, spec, NULL);
            }
        for (int a = 0; a < lines; a++) {
            snprintf(spec, sizeof(spec), "%s:addr%d", ref, a);
            add_job(c, spec, NULL);
        }
        if (dram) {
            snprintf(spec, sizeof(spec), "%s:weak", ref);
            add_job(c, spec, NULL);
        }
        for (size_t k = 0; b->m->ram_cell && k < sizeof(ram_flips) / sizeof(ram_flips[0]); k++) {
            if (ram_flips[k].machine != atlas_family(c->machine) || b->m->ram_cell(b, i, ram_flips[k].addr) < 0) continue;
            for (int bit = 0; bit < bits; bit++) {
                if (bits == 1) snprintf(spec, sizeof(spec), "%s:flip@%04X", ref, ram_flips[k].addr);
                else snprintf(spec, sizeof(spec), "%s:flip@%04X^%02X", ref, ram_flips[k].addr, 1u << bit);
                add_job(c, spec, NULL);
            }
        }
        for (size_t k = 0; k < sizeof(rom_flips) / sizeof(rom_flips[0]); k++)
            if (rom_flip_fits(c->machine, k, ref)) {
                snprintf(spec, sizeof(spec), "%s:flip=%04X^%02X", ref, rom_flips[k].addr, rom_flips[k].bits);
                add_job(c, spec, NULL);
            }
    }
}

/* Do all the parts of a fault spec exist on this board? (The scenarios
 * are written for a family: the 3B's TI-3 link isn't on the Issue 2.) */
static bool spec_fits(board_t *b, const char *spec) {
    for (const char *s = spec; *s; ) {
        size_t len = strcspn(s, " "), ref = strcspn(s, ".:");
        char one[32];
        snprintf(one, sizeof(one), "%.*s", (int)(ref < len ? ref : len), s);
        if (sim_find_part(&b->sim, one) < 0) return false;
        s += len + (s[len] == ' ');
    }
    return true;
}

/* A board's own scenarios, or else its family's (the Spectrum issues
 * share the 3B's; the 250469 has its own: its references differ). */
static int scenario_machine(int machine) {
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
        if (scenarios[i].machine == machine) return machine;
    return atlas_family(machine);
}

static void add_scenarios(catalog_t *c) {
    int sm = scenario_machine(c->machine);
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (scenarios[i].machine != sm || !spec_fits(c->b, scenarios[i].specs)) continue;
        char prefix[200];
        snprintf(prefix, sizeof(prefix), "%s %s (expected: %s): ",
                 sm == ATLAS_C64C ? "250469 scenario" : sm == ATLAS_C64 ? "tests/faults.sh" : "tests/zx-faults.sh",
                 scenarios[i].name, scenarios[i].expected);
        add_job(c, scenarios[i].specs, prefix);
    }
}

/* The parts the rules talk about. */
typedef struct { int cpu, pla_ula, dram[16], ndram, mux[4], nmux; } hot_t;

static void hot_parts(board_t *b, int machine, hot_t *h) {
    memset(h, 0, sizeof(*h));
    if (machine == ATLAS_C64C) {     /* the Super PLA, the two 41464s */
        h->cpu = C64(b)->p_cpu;
        h->pla_ula = sim_find_part(&b->sim, "U8");
        for (int i = 0; i < 2; i++) h->dram[h->ndram++] = C64(b)->dram4_part[i];
    } else if (machine == ATLAS_C64) {
        h->cpu = C64(b)->p_cpu;
        h->pla_ula = b->p_pla;
        for (int i = 0; i < 8; i++) h->dram[h->ndram++] = C64(b)->dram_part[i];
    } else {
        h->cpu = ZX(b)->p_cpu;
        h->pla_ula = ZX(b)->p_ula;
        for (int i = 0; i < 8; i++) h->dram[h->ndram++] = ZX(b)->lram_part[i];
        for (int i = 0; i < 8; i++) if (ZX(b)->uram_part[i] >= 0) h->dram[h->ndram++] = ZX(b)->uram_part[i];
    }
    for (int i = 0; i < b->sim.nparts && h->nmux < 4; i++) {
        const char *name = b->sim.parts[i].name;
        if (sim_ic(b, i) && name &&
            (strstr(name, "74LS257") || strstr(name, "74LS258") || strstr(name, "74LS157") || strstr(name, "ZX8401")))
            h->mux[h->nmux++] = i;
    }
}

static void catalog_mode(catalog_t *c, bool full) {
    board_t *b = c->b;
    sim_t *s = &b->sim;
    hot_t h;
    hot_parts(b, c->machine, &h);
    bool zx = atlas_family(c->machine) == ATLAS_ZX48;
    if (c->mode == MODE_BOOT) {
        add_scenarios(c);
        add_job(c, "", NULL);
        add_removed(c);
        add_memory(c);
        if (full) {
            /* Every single fault: pins open, nets stuck, passives, crystals. */
            for (int i = 0; i < s->nparts; i++) if (sim_ic(b, i)) add_pins(c, i, SEL_SIGNAL, DO_OPEN);
            for (int i = 0; i < s->nparts; i++) if (sim_ic(b, i)) add_pins(c, i, SEL_SIGNAL, DO_GND | DO_VCC);
            for (int i = 0; i < s->nparts; i++) {
                const part_t *p = &s->parts[i];
                if ((p->kind != PART_RESISTOR && p->kind != PART_LINK) || !board_part_simulated(b, i)) continue;
                char spec[32];
                snprintf(spec, sizeof(spec), "%s:open", p->ref);
                add_job(c, spec, NULL);
                snprintf(spec, sizeof(spec), "%s:short", p->ref);
                add_job(c, spec, NULL);
            }
            const char *list = b->m->crystals;
            while (list && *list) {
                size_t len = strcspn(list, " ");
                char ref[24], spec[32];
                snprintf(ref, sizeof(ref), "%.*s", (int)len, list);
                int part = sim_find_part(s, ref);
                if (part >= 0) {
                    snprintf(spec, sizeof(spec), "%s:open", ref);
                    add_job(c, spec, NULL);
                    /* A transistor's junctions, and weak (TR4, TR5). */
                    for (int k = RF_BE_SHORT; k <= RF_WEAK; k++) {
                        if (!board_rfault_fits(b, part, k)) continue;
                        snprintf(spec, sizeof(spec), "%s:%s", ref, board_rfault_spec(k));
                        add_job(c, spec, NULL);
                    }
                }
                list += len + (list[len] == ' ');
            }
            /* Solder bridges between neighbor legs, on every chip. */
            for (int i = 0; i < s->nparts; i++) add_bridges(c, i);
            /* Outputs dead inside the chip, on the busses. */
            for (int i = 0; i < s->nparts; i++) if (sim_ic(b, i)) add_pins(c, i, SEL_SHARED_OUT, DO_OUT);
        } else {
            /* The parts that fail most on the bench. */
            add_pins(c, h.cpu, SEL_BUS, DO_GND);
            add_pins(c, h.cpu, SEL_DATA, DO_VCC);
            for (int i = 0; i < h.ndram; i++) add_pins(c, h.dram[i], SEL_DRAM_DQ, DO_OPEN);
            add_pins(c, h.pla_ula, SEL_OUT, zx ? DO_GND : DO_GND | DO_VCC);
            for (int i = 0; i < h.nmux; i++) add_pins(c, h.mux[i], SEL_OUT, DO_GND | DO_VCC);
            /* Solder bridges on the chips that get resoldered or socketed
             * most: CPU, PLA/ULA, RAMs, ROMs, multiplexers. */
            add_bridges(c, h.cpu);
            add_bridges(c, h.pla_ula);
            for (int i = 0; i < h.ndram; i++) add_bridges(c, h.dram[i]);
            for (int i = 0; i < h.nmux; i++) add_bridges(c, h.mux[i]);
            for (int i = 0; i < s->nparts; i++) {
                int role = board_part_role(b, i);
                if (role == ROLE_RAM || role == ROLE_VRAM || role == ROLE_ROM) add_bridges(c, i);
            }
        }
        return;
    }
    /* The diagnostic cartridges: the faults they are made to find. */
    add_job(c, "", NULL);
    if (!full) {
        for (int i = 0; i < h.ndram; i++) {
            char spec[32];
            snprintf(spec, sizeof(spec), "%s:removed", s->parts[h.dram[i]].ref);
            add_job(c, spec, NULL);
        }
        if (!zx) {
            char spec[32];
            snprintf(spec, sizeof(spec), "%s:removed", s->parts[h.pla_ula].ref);
            add_job(c, spec, NULL);
            add_pins(c, h.cpu, SEL_DATA, DO_GND);
        }
        return;
    }
    add_removed(c);
    for (int i = 0; i < h.ndram; i++) add_pins(c, h.dram[i], zx ? SEL_DRAM_DQ : SEL_SIGNAL, DO_OPEN);
    add_pins(c, h.cpu, SEL_DATA, DO_GND | DO_VCC);
    if (!zx) add_pins(c, h.pla_ula, SEL_SIGNAL, DO_OPEN);
    add_pins(c, h.pla_ula, SEL_OUT, zx ? DO_GND : DO_GND | DO_VCC);
}

/* ------------------------------------------------------------------------ */
/* The file                                                                  */
/* ------------------------------------------------------------------------ */

static void write_chunk(FILE *f, uint32_t tag, const buf_t *payload) {
    uint32_t hdr[2] = { tag, (uint32_t)payload->n }, sum = fnv32(payload->p, payload->n);
    buf_t all = { 0 };
    buf_put(&all, hdr, sizeof(hdr));
    buf_put(&all, payload->p, payload->n);
    buf_put(&all, &sum, sizeof(sum));
    fwrite(all.p, 1, all.n, f);     /* one write per chunk */
    free(all.p);
}

static void write_outcome(FILE *f, const atlas_outcome_hdr_t *h, const buf_t *thumb, const buf_t *alt) {
    buf_t p = { 0 };
    buf_put(&p, h, sizeof(*h));
    if (thumb->n) buf_put(&p, thumb->p, thumb->n);
    if (alt->n) buf_put(&p, alt->p, alt->n);
    write_chunk(f, TAG_OUTCOME, &p);
    free(p.p);
}

static void write_fault(FILE *f, const job_t *j, const result_t *r) {
    atlas_fault_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.outcome = r->oh.key;
    h.machine = j->machine;
    h.mode = j->mode;
    h.stop = (uint8_t)r->stop;
    h.cpu = (uint8_t)r->cpu;
    h.frames = (uint16_t)r->frames;
    h.spec_len = (uint16_t)strlen(j->spec);
    h.desc_len = (uint16_t)strlen(j->desc);
    buf_t p = { 0 };
    buf_put(&p, &h, sizeof(h));
    buf_put(&p, j->spec, h.spec_len);
    buf_put(&p, j->desc, h.desc_len);
    write_chunk(f, TAG_FAULT, &p);
    free(p.p);
}

/* PINS: the key of the fault (fault_key()), the number of nets, when the
 * readings were taken (ATLAS_PINS_VERSION), then the probe state of each. */
typedef struct { uint64_t fault; uint16_t nnets, version, pad[2]; } pins_hdr_t;
_Static_assert(sizeof(pins_hdr_t) == 16, "pins_hdr_t layout changed");

static void write_pins(FILE *f, const job_t *j, const result_t *r) {
    pins_hdr_t h = { fault_key(j->machine, j->mode, j->spec), (uint16_t)r->nnets, ATLAS_PINS_VERSION, { 0 } };
    buf_t p = { 0 };
    buf_put(&p, &h, sizeof(h));
    buf_put(&p, r->nets, (size_t)r->nnets);
    write_chunk(f, TAG_PINS, &p);
    free(p.p);
}

/* LEGS: the key of the fault, the number of legs, the PINS version they
 * were taken by, then for each its state, the length of its name and the
 * name ("U7.3"). */
static void write_legs(FILE *f, const job_t *j, const result_t *r) {
    pins_hdr_t h = { fault_key(j->machine, j->mode, j->spec), (uint16_t)r->nlegs, ATLAS_PINS_VERSION, { 0 } };
    buf_t p = { 0 };
    buf_put(&p, &h, sizeof(h));
    for (int k = 0; k < r->nlegs; k++) {
        uint8_t hd[2] = { r->leg_state[k], (uint8_t)strlen(r->leg_name[k]) };
        buf_put(&p, hd, 2);
        buf_put(&p, r->leg_name[k], hd[1]);
    }
    write_chunk(f, TAG_LEGS, &p);
    free(p.p);
}

int atlas_open_legs(board_t *b, const char *spec, int *pins, int max) {
    int n = 0;
    for (const char *s = spec; *s && n < max; ) {
        size_t len = strcspn(s, " ");
        char ref[16] = { 0 }, pad[8] = { 0 }, what[16] = { 0 }, one[64];
        snprintf(one, sizeof(one), "%.*s", (int)len, s);
        if (sscanf(one, "%15[^.:].%7[^:]:%15s", ref, pad, what) == 3 && !strcmp(what, "open")) {
            int part = sim_find_part(&b->sim, ref), pin = part >= 0 ? board_pad_pin(b, part, pad) : -1;
            if (pin >= 0 && b->sim.parts[part].kind == PART_IC) pins[n++] = pin;
        }
        s += len + (s[len] == ' ');
    }
    return n;
}

/* NETS: the machine, a zero, the number of nets, their names (each ended
 * by a zero). */
static void write_net_names(FILE *f, int machine) {
    board_t *b = board_create(atlas_machines[machine]);
    uint8_t mh[2] = { (uint8_t)machine, 0 };
    uint16_t n = (uint16_t)b->sim.nnets;
    buf_t p = { 0 };
    buf_put(&p, mh, 2);
    buf_put(&p, &n, 2);
    for (int i = 0; i < n; i++) {
        const char *name = b->sim.nets[i].name ? b->sim.nets[i].name : "";
        buf_put(&p, name, strlen(name) + 1);
    }
    write_chunk(f, TAG_NETS, &p);
    free(p.p);
    board_destroy(b);
}

static char *dup_n(const uint8_t *p, size_t n) {
    char *s = malloc(n + 1);
    memcpy(s, p, n);
    s[n] = 0;
    return s;
}

static int cmp_outcome_key(const void *a, const void *b) {
    const atlas_outcome_t *x = a, *y = b;
    return x->h.key < y->h.key ? -1 : x->h.key > y->h.key;
}

/* The PINS chunks while loading: by fault key, then in the order written. */
typedef struct { uint64_t key; int order, n, len; uint8_t *v; } pins_rec_t;

static int cmp_pins(const void *a, const void *b) {
    const pins_rec_t *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->order - y->order;
}

static int find_outcome(const atlas_t *a, uint64_t key) {
    int lo = 0, hi = a->noutcomes - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a->outcomes[mid].h.key == key) return mid;
        if (a->outcomes[mid].h.key < key) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

bool atlas_load(atlas_t *a, const char *path, bool *exists) {
    memset(a, 0, sizeof(*a));
    FILE *f = fopen(path, "rb");
    if (exists) *exists = f != NULL;
    if (!f) return true;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(size > 0 ? (size_t)size : 1);
    size_t n = fread(d, 1, (size_t)size, f);
    fclose(f);
    if (n < 8 || memcmp(d, ATLAS_MAGIC, 7) || d[7] != ATLAS_VERSION) { free(d); return false; }
    int cap_o = 0, cap_f = 0, cap_p = 0, npins = 0, cap_l = 0, nlegs = 0;
    pins_rec_t *pins = NULL, *legs = NULL;      /* LEGS: n legs, v the payload after the header */
    size_t off = 8;
    while (off + 12 <= n) {
        uint32_t tag, len, sum;
        memcpy(&tag, d + off, 4);
        memcpy(&len, d + off + 4, 4);
        if (len > n - off - 12) break;
        const uint8_t *p = d + off + 8;
        memcpy(&sum, p + len, 4);
        if (sum != fnv32(p, len)) break;
        if (tag == TAG_OUTCOME && len >= sizeof(atlas_outcome_hdr_t)) {
            atlas_outcome_t o;
            memset(&o, 0, sizeof(o));
            memcpy(&o.h, p, sizeof(o.h));
            if (sizeof(o.h) + (size_t)o.h.len + o.h.alt_len != len) break;
            o.thumb = (uint8_t *)dup_n(p + sizeof(o.h), o.h.len);
            o.alt = (uint8_t *)dup_n(p + sizeof(o.h) + o.h.len, o.h.alt_len);
            if (a->noutcomes == cap_o) a->outcomes = realloc(a->outcomes, (size_t)(cap_o = cap_o * 2 + 64) * sizeof(o));
            a->outcomes[a->noutcomes++] = o;
        } else if (tag == TAG_FAULT && len >= sizeof(atlas_fault_hdr_t)) {
            atlas_fault_t ft;
            memset(&ft, 0, sizeof(ft));
            memcpy(&ft.h, p, sizeof(ft.h));
            if (sizeof(ft.h) + (size_t)ft.h.spec_len + ft.h.desc_len != len) break;
            ft.spec = dup_n(p + sizeof(ft.h), ft.h.spec_len);
            ft.desc = dup_n(p + sizeof(ft.h) + ft.h.spec_len, ft.h.desc_len);
            if (a->nfaults == cap_f) a->faults = realloc(a->faults, (size_t)(cap_f = cap_f * 2 + 256) * sizeof(ft));
            a->faults[a->nfaults++] = ft;
        } else if (tag == TAG_PINS && len >= sizeof(pins_hdr_t)) {
            pins_hdr_t ph;
            memcpy(&ph, p, sizeof(ph));
            if (sizeof(ph) + (size_t)ph.nnets != len) break;
            if (ph.version != ATLAS_PINS_VERSION) { off += 12 + len; continue; }     /* taken otherwise */
            if (npins == cap_p) pins = realloc(pins, (size_t)(cap_p = cap_p * 2 + 256) * sizeof(*pins));
            pins[npins].key = ph.fault;
            pins[npins].n = ph.nnets;
            pins[npins].order = npins;
            pins[npins++].v = (uint8_t *)dup_n(p + sizeof(ph), ph.nnets);
        } else if (tag == TAG_LEGS && len >= sizeof(pins_hdr_t)) {
            pins_hdr_t lh;
            memcpy(&lh, p, sizeof(lh));
            if (lh.version != ATLAS_PINS_VERSION) { off += 12 + len; continue; }
            if (nlegs == cap_l) legs = realloc(legs, (size_t)(cap_l = cap_l * 2 + 256) * sizeof(*legs));
            legs[nlegs].key = lh.fault;
            legs[nlegs].n = lh.nnets;
            legs[nlegs].order = nlegs;
            legs[nlegs].len = (int)(len - sizeof(lh));
            legs[nlegs++].v = (uint8_t *)dup_n(p + sizeof(lh), len - sizeof(lh));
        } else if (tag == TAG_NETS && len >= 4 && p[0] < ATLAS_NMACHINES) {
            int m = p[0];
            uint16_t nn;
            memcpy(&nn, p + 2, 2);
            char **names = calloc(nn ? nn : 1, sizeof(char *));
            size_t at = 4;
            int got = 0;
            while (got < nn && at < len) {
                size_t l = strnlen((const char *)p + at, len - at);
                if (at + l >= len) break;
                names[got++] = dup_n(p + at, l);
                at += l + 1;
            }
            if (got != nn) {
                for (int i = 0; i < got; i++) free(names[i]);
                free(names);
                break;
            }
            for (int i = 0; i < a->nnet_names[m]; i++) free(a->net_names[m][i]);
            free(a->net_names[m]);
            a->net_names[m] = names;
            a->nnet_names[m] = nn;
        }
        off += 12 + len;
    }
    a->valid_end = (long)off;
    free(d);
    /* A fault run again (C64HW_ATLAS_REDO): the last one written counts. */
    hset_t seen = { 0 }, used = { 0 };
    bool *drop = calloc((size_t)a->nfaults + 1, sizeof(bool));
    for (int i = a->nfaults - 1; i >= 0; i--)
        drop[i] = !hset_add(&seen, fault_key(a->faults[i].h.machine, a->faults[i].h.mode, a->faults[i].spec));
    int nf = 0;
    for (int i = 0; i < a->nfaults; i++) {
        if (drop[i]) { free(a->faults[i].spec); free(a->faults[i].desc); continue; }
        a->faults[nf++] = a->faults[i];
        hset_add(&used, a->faults[i].h.outcome);
    }
    a->nfaults = nf;
    free(drop);
    free(seen.v);
    /* Outcomes by key, and every fault linked to its outcome. A picture
     * no fault gives any more (all its faults run again) goes. */
    if (a->noutcomes) qsort(a->outcomes, (size_t)a->noutcomes, sizeof(atlas_outcome_t), cmp_outcome_key);
    int no = 0;
    for (int i = 0; i < a->noutcomes; i++) {
        atlas_outcome_t *o = &a->outcomes[i];
        if (!hset_has(&used, o->h.key) || (no && a->outcomes[no - 1].h.key == o->h.key)) {
            free(o->thumb);
            free(o->alt);
            continue;
        }
        a->outcomes[no++] = *o;
    }
    a->noutcomes = no;
    free(used.v);
    for (int i = 0; i < a->nfaults; i++) {
        atlas_fault_t *ft = &a->faults[i];
        ft->outcome = find_outcome(a, ft->h.outcome);
        if (ft->outcome < 0) continue;
        atlas_outcome_t *o = &a->outcomes[ft->outcome];
        o->faults = realloc(o->faults, (size_t)(o->nfaults + 1) * sizeof(int));
        o->faults[o->nfaults++] = i;
    }
    /* Every fault linked to its PINS (the last one written counts). */
    if (npins) {
        qsort(pins, (size_t)npins, sizeof(*pins), cmp_pins);
        for (int i = 0; i < a->nfaults; i++) {
            atlas_fault_t *ft = &a->faults[i];
            uint64_t k = fault_key(ft->h.machine, ft->h.mode, ft->spec);
            int lo = 0, hi = npins - 1, at = -1;
            while (lo <= hi) {      /* the last of equal keys */
                int mid = (lo + hi) / 2;
                if (pins[mid].key <= k) { if (pins[mid].key == k) at = mid; lo = mid + 1; }
                else hi = mid - 1;
            }
            if (at < 0) continue;
            ft->nets = (uint8_t *)dup_n(pins[at].v, pins[at].n);
            ft->nnets = pins[at].n;
        }
        for (int i = 0; i < npins; i++) free(pins[i].v);
    }
    free(pins);
    /* And to its LEGS (the last one written counts). */
    if (nlegs) {
        qsort(legs, (size_t)nlegs, sizeof(*legs), cmp_pins);
        for (int i = 0; i < a->nfaults; i++) {
            atlas_fault_t *ft = &a->faults[i];
            uint64_t k = fault_key(ft->h.machine, ft->h.mode, ft->spec);
            int lo = 0, hi = nlegs - 1, at = -1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (legs[mid].key <= k) { if (legs[mid].key == k) at = mid; lo = mid + 1; }
                else hi = mid - 1;
            }
            if (at < 0) continue;
            ft->has_legs = true;
            ft->legs = calloc((size_t)legs[at].n + 1, sizeof(*ft->legs));
            const uint8_t *v = legs[at].v;
            int pos = 0;
            for (int l = 0; l < legs[at].n && pos + 2 <= legs[at].len; l++) {
                int nl = v[pos + 1];
                if (pos + 2 + nl > legs[at].len) break;
                ft->legs[ft->nlegs].state = v[pos];
                snprintf(ft->legs[ft->nlegs].pin, sizeof(ft->legs[0].pin), "%.*s", nl, (const char *)v + pos + 2);
                ft->nlegs++;
                pos += 2 + nl;
            }
        }
        for (int i = 0; i < nlegs; i++) free(legs[i].v);
    }
    free(legs);
    return true;
}

void atlas_free(atlas_t *a) {
    for (int i = 0; i < a->noutcomes; i++) {
        free(a->outcomes[i].thumb);
        free(a->outcomes[i].alt);
        free(a->outcomes[i].faults);
    }
    for (int i = 0; i < a->nfaults; i++) {
        free(a->faults[i].spec);
        free(a->faults[i].desc);
        free(a->faults[i].nets);
        free(a->faults[i].legs);
    }
    for (int m = 0; m < ATLAS_NMACHINES; m++) {
        for (int i = 0; i < a->nnet_names[m]; i++) free(a->net_names[m][i]);
        free(a->net_names[m]);
    }
    free(a->outcomes);
    free(a->faults);
    memset(a, 0, sizeof(*a));
}

/* ------------------------------------------------------------------------ */
/* Reports (the labels are in atlas_match.c)                                */
/* ------------------------------------------------------------------------ */

static const char *const stop_names[] = { "ran to the end", "no signal", "CPU dead", "static picture",
                                          "flash code repeated" };
static const char *const cpu_names[] = { "running", "in reset", "stopped", "jammed" };

static const atlas_t *sort_atlas;

static int cmp_outcome_size(const void *a, const void *b) {
    const atlas_outcome_t *x = &sort_atlas->outcomes[*(const int *)a], *y = &sort_atlas->outcomes[*(const int *)b];
    if (x->nfaults != y->nfaults) return y->nfaults - x->nfaults;
    return *(const int *)a - *(const int *)b;
}

static bool has_healthy(const atlas_t *a, const atlas_outcome_t *o) {
    for (int k = 0; k < o->nfaults; k++) if (!a->faults[o->faults[k]].spec[0]) return true;
    return false;
}

/* The outcomes of one machine and mode, most causes first (a fresh
 * array, 'n' of them). */
static int *ranked(const atlas_t *a, int machine, int mode, int *n, int *runs) {
    int *v = malloc(((size_t)a->noutcomes + 1) * sizeof(int));
    *n = *runs = 0;
    for (int i = 0; i < a->noutcomes; i++)
        if (a->outcomes[i].h.machine == machine && a->outcomes[i].h.mode == mode) {
            v[(*n)++] = i;
            *runs += a->outcomes[i].nfaults;
        }
    sort_atlas = a;
    qsort(v, (size_t)*n, sizeof(int), cmp_outcome_size);
    return v;
}

/* max_groups / max_causes < 0: all; descs: a line per cause. */
static void print_groups(const atlas_t *a, int max_groups, int max_causes, bool descs) {
    for (int m = 0; m < ATLAS_NMACHINES; m++)
        for (int mode = 0; mode < ATLAS_NMODES; mode++) {
            int n, runs;
            int *v = ranked(a, m, mode, &n, &runs);
            if (n) printf("\n== %s, %s: %d runs, %d different outcomes\n", atlas_machine_id(m), atlas_mode_name(mode),
                          runs, n);
            for (int g = 0; g < n && (max_groups < 0 || g < max_groups); g++) {
                const atlas_outcome_t *o = &a->outcomes[v[g]];
                char label[256];
                atlas_outcome_label(o, label, sizeof(label));
                printf("  #%-3d %3d cause%s  %s%s\n", g + 1, o->nfaults, o->nfaults == 1 ? " " : "s", label,
                       has_healthy(a, o) ? "   <== the healthy board" : "");
                if (descs) {
                    uint8_t *img = malloc((size_t)o->h.w * o->h.h);
                    char text[2048] = "";
                    if (atlas_decode_thumb(o, false, img)) atlas_read_text(o->h.machine, o->h.mode, img, text, sizeof(text));
                    free(img);
                    for (char *line = text; *line; ) {
                        char *nl = strchr(line, '\n');
                        int n = nl ? (int)(nl - line) : (int)strlen(line);
                        printf("         text: \"%.*s\"\n", n, line);
                        line += n + (nl != NULL);
                    }
                    for (int k = 0; k < o->nfaults && (max_causes < 0 || k < max_causes); k++) {
                        const atlas_fault_t *ft = &a->faults[o->faults[k]];
                        printf("         %-14s %s  [%s at frame %d, CPU %s]\n", ft->spec[0] ? ft->spec : "(none)", ft->desc,
                               stop_names[ft->h.stop < 5 ? ft->h.stop : 0], ft->h.frames, cpu_names[ft->h.cpu & 3]);
                    }
                    if (max_causes >= 0 && o->nfaults > max_causes) printf("         ... %d more\n", o->nfaults - max_causes);
                    continue;
                }
                printf("        ");
                for (int k = 0; k < o->nfaults && (max_causes < 0 || k < max_causes); k++)
                    printf("%s%s", k ? ", " : "", a->faults[o->faults[k]].spec[0] ? a->faults[o->faults[k]].spec : "(healthy)");
                if (max_causes >= 0 && o->nfaults > max_causes) printf(", ... +%d more", o->nfaults - max_causes);
                printf("\n");
            }
            if (max_groups >= 0 && n > max_groups) printf("  ... %d more outcomes (--atlas-report lists them all)\n", n - max_groups);
            free(v);
        }
}

static bool write_thumb_png(const atlas_outcome_t *o, bool alt, const char *path) {
    int n = o->h.w * o->h.h;
    uint8_t *idx = malloc((size_t)n);
    uint32_t *px = malloc((size_t)n * sizeof(uint32_t));
    bool ok = atlas_decode_thumb(o, alt, idx);
    const uint32_t *pal = atlas_palette(o->h.machine);
    for (int i = 0; ok && i < n; i++) px[i] = pal[idx[i]];
    if (ok) ok = png_write(path, px, o->h.w, o->h.h);
    free(idx);
    free(px);
    return ok;
}

int atlas_report(const char *path, const char *png_dir) {
    atlas_t a;
    bool exists;
    if (!atlas_load(&a, path, &exists) || !exists) {
        fprintf(stderr, "%s: %s\n", path, exists ? "not an atlas" : "no such file");
        return 1;
    }
    printf("%s: %d fault runs, %d different outcomes\n", path, a.nfaults, a.noutcomes);
    print_groups(&a, -1, -1, true);
    if (png_dir) {
        /* One PNG per outcome (and its flash picture), with an index. */
        char ipath[600];
        snprintf(ipath, sizeof(ipath), "%s/index.txt", png_dir);
        FILE *idx = fopen(ipath, "w");
        if (!idx) { fprintf(stderr, "can't write %s\n", ipath); atlas_free(&a); return 1; }
        int written = 0;
        for (int m = 0; m < ATLAS_NMACHINES; m++)
            for (int mode = 0; mode < ATLAS_NMODES; mode++) {
                int n, runs;
                int *v = ranked(&a, m, mode, &n, &runs);
                for (int g = 0; g < n; g++) {
                    const atlas_outcome_t *o = &a.outcomes[v[g]];
                    char label[256], file[700];
                    atlas_outcome_label(o, label, sizeof(label));
                    fprintf(idx, "%s-%s-%03d  %d causes  %s\n", atlas_machine_id(m), mode_ids[mode], g + 1, o->nfaults, label);
                    for (int k = 0; k < o->nfaults; k++) fprintf(idx, "    %s\n", a.faults[o->faults[k]].spec);
                    snprintf(file, sizeof(file), "%s/%s-%s-%03d.png", png_dir, atlas_machine_id(m), mode_ids[mode], g + 1);
                    written += write_thumb_png(o, false, file);
                    snprintf(file, sizeof(file), "%s/%s-%s-%03d-lit.png", png_dir, atlas_machine_id(m), mode_ids[mode], g + 1);
                    written += o->h.alt_enc == THUMB_RLE && write_thumb_png(o, true, file);
                }
                free(v);
            }
        fclose(idx);
        printf("\n%d thumbnails written to %s (see index.txt)\n", written, png_dir);
    }
    atlas_free(&a);
    return 0;
}

/* Two atlases built from different versions of the simulator (or with a
 * different number of threads, which must not change anything): the
 * faults whose outcome changed. */
int atlas_diff(const char *path, const char *other) {
    atlas_t a, b;
    bool ea, eb;
    if (!atlas_load(&a, path, &ea) || !ea || !atlas_load(&b, other, &eb) || !eb) {
        fprintf(stderr, "can't read both %s and %s\n", path, other);
        return 1;
    }
    int same = 0, changed = 0, only_a = 0;
    for (int i = 0; i < a.nfaults; i++) {
        const atlas_fault_t *x = &a.faults[i];
        const atlas_fault_t *y = NULL;
        for (int k = 0; k < b.nfaults && !y; k++)
            if (b.faults[k].h.machine == x->h.machine && b.faults[k].h.mode == x->h.mode && !strcmp(b.faults[k].spec, x->spec))
                y = &b.faults[k];
        if (!y) { only_a++; continue; }
        if (x->h.outcome == y->h.outcome) { same++; continue; }
        changed++;
        char la[256] = "?", lb[256] = "?";
        if (x->outcome >= 0) atlas_outcome_label(&a.outcomes[x->outcome], la, sizeof(la));
        if (y->outcome >= 0) atlas_outcome_label(&b.outcomes[y->outcome], lb, sizeof(lb));
        printf("%s %s %s:\n    %s\n -> %s\n", atlas_machine_id(x->h.machine), mode_ids[x->h.mode], x->spec[0] ? x->spec : "(healthy)", la, lb);
    }
    printf("%d faults with the same outcome, %d changed, %d only in %s, %d in %s\n", same, changed, only_a, path,
           b.nfaults, other);
    atlas_free(&a);
    atlas_free(&b);
    return changed ? 2 : 0;
}

/* ------------------------------------------------------------------------ */
/* The build                                                                 */
/* ------------------------------------------------------------------------ */

static struct {
    job_t *jobs;
    int njobs;
    const char *carts;
    int next, active;           /* atomic */
    pthread_mutex_t lock;       /* the rest */
    FILE *f;
    hset_t outcomes;
    int done, failed, new_outcomes;
    double cost_done;
} P;

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        int i = __atomic_fetch_add(&P.next, 1, __ATOMIC_SEQ_CST);
        if (i >= P.njobs) break;
        __atomic_fetch_add(&P.active, 1, __ATOMIC_SEQ_CST);
        result_t r;
        memset(&r, 0, sizeof(r));
        const job_t *j = &P.jobs[i];
        run_job(j, P.carts, &r);
        pthread_mutex_lock(&P.lock);
        if (r.bad) {
            fprintf(stderr, "\n%s %s %s: %s\n", atlas_machine_id(j->machine), mode_ids[j->mode], j->spec, r.err);
            P.failed++;
        } else {
            if (!j->pins_only && hset_add(&P.outcomes, r.oh.key)) {
                write_outcome(P.f, &r.oh, &r.thumb, &r.alt);
                P.new_outcomes++;
            }
            if (!j->pins_only) write_fault(P.f, j, &r);
            write_pins(P.f, j, &r);
            if (r.nlegs) write_legs(P.f, j, &r);
            fflush(P.f);
        }
        P.done++;
        P.cost_done += j->cost;
        pthread_mutex_unlock(&P.lock);
        __atomic_fetch_sub(&P.active, 1, __ATOMIC_SEQ_CST);
        free(r.thumb.p);
        free(r.nets);
        free(r.alt.p);
    }
    return NULL;
}

static int cmp_job_cost(const void *a, const void *b) {
    const job_t *x = a, *y = b;
    if (x->cost != y->cost) return x->cost < y->cost ? 1 : -1;
    return x->order - y->order;
}

static void fmt_time(double s, char *buf, int len) {
    int t = (int)(s + 0.5);
    if (t >= 3600) snprintf(buf, len, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(buf, len, "%02d:%02d", t / 60, t % 60);
}

static void progress(int threads, double t0, double total_cost, bool tty, bool final) {
    pthread_mutex_lock(&P.lock);
    int done = P.done;
    double cost_done = P.cost_done;
    pthread_mutex_unlock(&P.lock);
    int active = __atomic_load_n(&P.active, __ATOMIC_SEQ_CST);
    double el = now_seconds() - t0;
    char bar[31], tel[16], teta[16] = "--:--";
    int fill = P.njobs ? 30 * done / P.njobs : 30;
    for (int i = 0; i < 30; i++) bar[i] = i < fill ? '#' : '-';
    bar[30] = 0;
    fmt_time(el, tel, sizeof(tel));
    if (cost_done > 0 && done < P.njobs) fmt_time(el * (total_cost - cost_done) / cost_done, teta, sizeof(teta));
    printf("%s[%s] %d/%d  %2d/%d threads busy  elapsed %s  ETA %s   %s", tty ? "\r" : "", bar, done, P.njobs, active,
           threads, tel, final ? "00:00" : teta, tty && !final ? "" : "\n");
    fflush(stdout);
}

/* C64HW_ATLAS_REDO="IC5 JROM1 IC5.20 IC5.27": run again, though the atlas
 * has them, the faults on these parts or on the nets of these pins (as
 * the board's links join them, and one series resistor further), after
 * a change to how those parts work. The loader keeps the last run. */
static void redo_marks(board_t *b, const char *list, hset_t *parts, hset_t *roots) {
    sim_t *s = &b->sim;
    for (const char *t = list; *t; ) {
        size_t len = strcspn(t, " ");
        char one[32], ref[16], pad[8];
        snprintf(one, sizeof(one), "%.*s", (int)len, t);
        t += len + (t[len] == ' ');
        if (sscanf(one, "%15[^.].%7s", ref, pad) == 2) {
            int part = sim_find_part(s, ref), pin = part >= 0 ? board_pad_pin(b, part, pad) : -1;
            if (pin >= 0 && s->pins[pin].root >= 0) hset_add(roots, (uint64_t)s->pins[pin].root + 1);
        } else if (*one) {
            int part = sim_find_part(s, one);
            if (part >= 0) hset_add(parts, (uint64_t)part + 1);
        }
    }
    for (int i = 0; i < s->nparts; i++) {
        const part_t *p = &s->parts[i];
        if (p->kind != PART_RESISTOR || !p->state) continue;
        const resistor_state_t *st = p->state;
        for (int k = 0; k < st->nel; k++) {
            const element_t *e = &st->el[k];
            if (e->type != EL_SERIES && e->type != EL_DELAY) continue;
            uint64_t ra = (uint64_t)s->pins[e->pin_a].root + 1, rb = (uint64_t)s->pins[e->pin_b].root + 1;
            if (hset_has(roots, ra)) hset_add(roots, rb);
            else if (hset_has(roots, rb)) hset_add(roots, ra);
        }
    }
}

static bool redo_touches(board_t *b, const char *spec, const hset_t *parts, const hset_t *roots) {
    sim_t *s = &b->sim;
    for (const char *t = spec; *t; ) {
        size_t len = strcspn(t, " ");
        char one[48], ref[16] = { 0 }, pad[2][8] = { { 0 } };
        snprintf(one, sizeof(one), "%.*s", (int)len, t);
        t += len + (t[len] == ' ');
        int n = sscanf(one, "%15[^.:].%7[^-:]-%7[^:]", ref, pad[0], pad[1]);
        int part = n >= 1 ? sim_find_part(s, ref) : -1;
        if (part < 0) continue;
        if (hset_has(parts, (uint64_t)part + 1)) return true;
        const part_t *p = &s->parts[part];
        if (n == 1) {       /* the whole part: removed, open, shorted */
            for (int k = 0; k < p->npins; k++)
                if (hset_has(roots, (uint64_t)s->pins[p->first_pin + k].root + 1)) return true;
        } else {
            for (int j = 0; j < n - 1; j++) {
                int pin = board_pad_pin(b, part, pad[j]);
                if (pin >= 0 && hset_has(roots, (uint64_t)s->pins[pin].root + 1)) return true;
            }
        }
    }
    return false;
}

/* C64HW_ATLAS_PRUNE=1: the normal boot faults no longer in the catalog
 * (a pin found not to be an output: its "out0"/"out1" faults go) are cut
 * out of the file, their FALT, PINS and LEGS chunks; the rest is copied as
 * it is. The old file is kept as PATH.prune-bak. How many were cut, -1 if
 * the file couldn't be rewritten. */
static int prune_file(const char *path, const hset_t *catalog) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(size > 0 ? (size_t)size : 1);
    size_t n = fread(d, 1, (size_t)size, f);
    fclose(f);
    char tmp[1100], bak[1100];
    snprintf(tmp, sizeof(tmp), "%s.prune-tmp", path);
    snprintf(bak, sizeof(bak), "%s.prune-bak", path);
    FILE *o = fopen(tmp, "wb");
    if (!o || n < 8) { free(d); if (o) fclose(o); return -1; }
    fwrite(d, 1, 8, o);
    hset_t gone = { 0 };
    int cut = 0;
    for (size_t off = 8; off + 12 <= n; ) {
        uint32_t tag, len;
        memcpy(&tag, d + off, 4);
        memcpy(&len, d + off + 4, 4);
        if (len > n - off - 12) break;
        const uint8_t *p = d + off + 8;
        bool drop = false;
        if (tag == TAG_FAULT && len >= sizeof(atlas_fault_hdr_t)) {
            atlas_fault_hdr_t h;
            memcpy(&h, p, sizeof(h));
            char *spec = dup_n(p + sizeof(h), h.spec_len);
            uint64_t k = fault_key(h.machine, h.mode, spec);
            if (h.mode == MODE_BOOT && !hset_has(catalog, k)) {
                drop = true;
                if (hset_add(&gone, k)) { printf("  cut out: %s %s\n", atlas_machine_id(h.machine), spec); cut++; }
            }
            free(spec);
        } else if ((tag == TAG_PINS || tag == TAG_LEGS) && len >= sizeof(pins_hdr_t)) {
            pins_hdr_t h;
            memcpy(&h, p, sizeof(h));
            drop = hset_has(&gone, h.fault);
        }
        if (!drop) fwrite(d + off, 1, 12 + len, o);
        off += 12 + len;
    }
    free(d);
    free(gone.v);
    if (fclose(o)) return -1;
    remove(bak);
    if (rename(path, bak) || rename(tmp, path)) return -1;
    return cut;
}

int atlas_build(const atlas_build_t *o) {
    setvbuf(stdout, NULL, _IONBF, 0);       /* progress shows as it goes, even into a file */
    bool full = !strcmp(o->tier, "full");
    if (!full && strcmp(o->tier, "quick")) {
        fprintf(stderr, "--build-atlas=%s: the tiers are quick and full\n", o->tier);
        return 1;
    }
    /* What the boards share, built before any thread runs. */
    atlas_init_shared();
    atlas_set_carts(o->carts);

    /* What is already in the file: skipped. A chunk cut short when the
     * last build was stopped is cut off. */
    atlas_t old;
    bool exists;
    if (!atlas_load(&old, o->path, &exists)) {
        fprintf(stderr, "%s is not an atlas file\n", o->path);
        return 1;
    }
    /* DONE: in the file with their PINS. RAN: in it without (an atlas
     * built before PINS), or with PINS of other nets than the board has
     * now (a board data change: probe_db_load drops them), or with an open
     * chip leg and no LEGS (an atlas built before LEGS): run again for
     * those only. */
    hset_t done = { 0 }, ran = { 0 }, legged = { 0 };
    memset(&P, 0, sizeof(P));
    P.carts = o->carts;
    for (int i = 0; i < old.nfaults; i++)
        hset_add(old.faults[i].nets ? &done : &ran,
                 fault_key(old.faults[i].h.machine, old.faults[i].h.mode, old.faults[i].spec));
    for (int i = 0; i < old.nfaults; i++)
        if (old.faults[i].has_legs) hset_add(&legged, fault_key(old.faults[i].h.machine, old.faults[i].h.mode, old.faults[i].spec));
    for (int i = 0; i < old.noutcomes; i++) hset_add(&P.outcomes, old.outcomes[i].h.key);
    if (exists) {
        FILE *f = fopen(o->path, "r+b");
        if (!f) { fprintf(stderr, "can't write %s\n", o->path); return 1; }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        if (size > old.valid_end) {
            printf("%s: dropping %ld bytes of an interrupted write\n", o->path, size - old.valid_end);
            fflush(f);
            if (ftruncate(fileno(f), old.valid_end)) { fprintf(stderr, "can't truncate %s\n", o->path); return 1; }
        }
        fclose(f);
    }
    P.f = fopen(o->path, exists ? "ab" : "wb");
    if (!P.f) { fprintf(stderr, "can't write %s\n", o->path); return 1; }
    if (!exists) {
        fwrite(ATLAS_MAGIC, 1, 7, P.f);
        fputc(ATLAS_VERSION, P.f);
        fflush(P.f);
    }

    /* The catalog. */
    joblist_t all = { 0 };
    const char *redo_list = getenv("C64HW_ATLAS_REDO");
    hset_t redo = { 0 };
    printf("Fault atlas, %s tier -> %s\n", o->tier, o->path);
    if (redo_list) printf("  running again the faults on: %s\n", redo_list);
    /* C64HW_ATLAS_REDO_FAULTS=FILE: these faults run again, a line each:
     * the board's id and the fault ("zx48 IC23.8:open"). */
    hset_t redo_faults = { 0 };
    const char *redo_file = getenv("C64HW_ATLAS_REDO_FAULTS");
    if (redo_file) {
        FILE *rf = fopen(redo_file, "r");
        if (!rf) { fprintf(stderr, "can't read %s\n", redo_file); return 1; }
        char line[256], mid[32], spec[200];
        int nr = 0;
        while (fgets(line, sizeof(line), rf))
            if (sscanf(line, "%31s %199[^\r\n]", mid, spec) == 2)
                for (int m = 0; m < ATLAS_NMACHINES; m++)
                    if (!strcmp(mid, atlas_machine_id(m))) { hset_add(&redo_faults, fault_key(m, MODE_BOOT, spec)); nr++; }
        fclose(rf);
        printf("  running again %d faults listed in %s\n", nr, redo_file);
    }
    for (int m = 0; m < ATLAS_NMACHINES; m++) {
        board_t *b = board_create(atlas_machines[m]);
        hset_t rparts = { 0 }, rroots = { 0 };
        if (redo_list) redo_marks(b, redo_list, &rparts, &rroots);
        for (int mode = 0; mode < ATLAS_NMODES; mode++) {
            /* Normal boot only (see atlas.h); the cartridge modes stay
             * in the table, for the atlases built with them. */
            if (!mode_fits(m, mode) || mode != MODE_BOOT) continue;
            hset_t seen = { 0 };
            catalog_t c = { &all, &seen, b, m, mode };
            int before = all.n;
            catalog_mode(&c, full);
            for (int i = 0; i < old.nfaults; i++) {
                const atlas_fault_t *f = &old.faults[i];
                if (f->h.machine == m && f->h.mode == mode && f->nets && f->nnets != b->sim.nnets)
                    hset_add(&ran, fault_key(m, mode, f->spec));
            }
            int todo = 0;
            for (int i = before; i < all.n; i++) {
                uint64_t k = fault_key(m, mode, all.v[i].spec);
                int lp[MAX_LEGS];
                if (hset_has(&done, k) && !hset_has(&legged, k) && atlas_open_legs(b, all.v[i].spec, lp, MAX_LEGS))
                    hset_add(&ran, k);
                if (redo_list && redo_touches(b, all.v[i].spec, &rparts, &rroots)) hset_add(&redo, k);
                if (hset_has(&redo_faults, k)) hset_add(&redo, k);
                todo += !hset_has(&done, k) || hset_has(&ran, k) || hset_has(&redo, k);
            }
            printf("  %-5s %-20s %5d faults, %5d to run\n", atlas_machine_id(m), atlas_mode_name(mode), all.n - before, todo);
            free(seen.v);
        }
        free(rparts.v);
        free(rroots.v);
        board_destroy(b);
    }
    if (getenv("C64HW_ATLAS_PRUNE")) {
        /* Against the full catalog only: the quick one would cut the rest. */
        if (!full) { fprintf(stderr, "C64HW_ATLAS_PRUNE needs --build-atlas=full\n"); return 1; }
        hset_t cat = { 0 };
        for (int i = 0; i < all.n; i++) hset_add(&cat, fault_key(all.v[i].machine, all.v[i].mode, all.v[i].spec));
        fclose(P.f);
        int cut = prune_file(o->path, &cat);
        free(cat.v);
        if (cut < 0) { fprintf(stderr, "can't rewrite %s\n", o->path); return 1; }
        printf("%d faults no longer in the catalog cut out of %s (the old file: %s.prune-bak)\n", cut, o->path, o->path);
        P.f = fopen(o->path, "ab");
        if (!P.f) { fprintf(stderr, "can't write %s\n", o->path); return 1; }
    }
    P.jobs = malloc(((size_t)all.n + 1) * sizeof(job_t));
    double total_cost = 0;
    for (int i = 0; i < all.n; i++) {
        uint64_t k = fault_key(all.v[i].machine, all.v[i].mode, all.v[i].spec);
        if (!hset_has(&done, k) || hset_has(&ran, k) || hset_has(&redo, k)) {
            all.v[i].pins_only = !hset_has(&redo, k) && hset_has(&ran, k);
            P.jobs[P.njobs++] = all.v[i];
            total_cost += all.v[i].cost;
        }
    }
    free(all.v);
    free(done.v);
    free(ran.v);
    free(legged.v);
    free(redo.v);
    free(redo_faults.v);
    atlas_free(&old);
    /* C64HW_ATLAS_TODO=1: only list the faults still to run. */
    if (getenv("C64HW_ATLAS_TODO")) {
        for (int i = 0; i < P.njobs; i++)
            printf("%s %s\n", atlas_machine_id(P.jobs[i].machine), P.jobs[i].spec);
        free(P.jobs);
        return 0;
    }
    qsort(P.jobs, (size_t)P.njobs, sizeof(job_t), cmp_job_cost);   /* the longest first */

    int threads = o->threads > 0 ? o->threads : atlas_logical_processors();
    if (threads > P.njobs) threads = P.njobs;
    double t0 = now_seconds();
    if (P.njobs) {
        /* The net names first: the PINS that follow are in their order
         * (the last NETS of a board counts). */
        for (int m = 0; m < ATLAS_NMACHINES; m++) write_net_names(P.f, m);
        fflush(P.f);
        printf("%d runs on %d worker threads (%d logical processors)\n", P.njobs, threads, atlas_logical_processors());
        pthread_mutex_init(&P.lock, NULL);
        pthread_t *tid = malloc((size_t)threads * sizeof(pthread_t));
        for (int i = 0; i < threads; i++) pthread_create(&tid[i], NULL, worker, NULL);
        bool tty = isatty(fileno(stdout));
        double last_line = t0;
        for (;;) {
#ifdef _WIN32
            Sleep(250);
#else
            usleep(250000);
#endif
            pthread_mutex_lock(&P.lock);
            bool finished = P.done == P.njobs;
            pthread_mutex_unlock(&P.lock);
            if (finished) break;
            if (tty || now_seconds() - last_line >= 15) {
                progress(threads, t0, total_cost, tty, false);
                last_line = now_seconds();
            }
        }
        for (int i = 0; i < threads; i++) pthread_join(tid[i], NULL);
        free(tid);
        progress(threads, t0, total_cost, tty, true);
        pthread_mutex_destroy(&P.lock);
    } else printf("Nothing to run: every fault of the tier is in %s\n", o->path);
    fclose(P.f);
    char tel[16];
    fmt_time(now_seconds() - t0, tel, sizeof(tel));
    printf("%d runs in %s, %d new outcomes, %d failed\n", P.done, tel, P.new_outcomes, P.failed);
    free(P.jobs);
    free(P.outcomes.v);

    /* What the atlas holds now. */
    atlas_t a;
    if (atlas_load(&a, o->path, NULL)) {
        printf("\n%s: %d fault runs, %d different outcomes", o->path, a.nfaults, a.noutcomes);
        print_groups(&a, 10, 8, false);
        atlas_free(&a);
    }
    return P.failed ? 2 : 0;
}
