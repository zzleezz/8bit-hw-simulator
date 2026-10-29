/* atlas.h - the fault atlas: what the monitor shows for every fault of a
 * catalog, simulated ahead of time, so a picture of a real broken machine
 * (a USB capture card screenshot) can be matched against it, and the
 * matching faults put on the live board with one click.
 *
 *   c64-hw-sim --build-atlas=quick|full [--atlas FILE] [--threads N]
 *   c64-hw-sim --atlas-report [--atlas FILE] [--atlas-png DIR]
 *   c64-hw-sim --atlas-diff OTHER [--atlas FILE]
 *
 * The build runs every fault of the catalog headless, on a pool of worker
 * threads (one board each: all the state of a board is in its board_t),
 * booting normally, with no cartridge: a machine well enough to run the
 * Dead Test cartridge or the ZX Spectrum Diagnostics ROM says itself what
 * is wrong. (The file and the reports still know those modes: an atlas
 * built before has them.)
 *
 * THE FILE (fault_atlas.bin) is a log of chunks, appended as runs finish
 * and flushed one by one, so the build can be stopped at any moment
 * (Ctrl+C) and run again: it skips the faults already in the file, and a
 * chunk cut short by the stop is dropped. Every chunk is
 *
 *   u32 tag, u32 length, payload, u32 FNV-1a of the payload
 *
 * after a header "HWATLAS" + version. Two kinds of chunks:
 *
 *   OUTC  an OUTCOME: what a run ended up showing (thumbnail, fingerprint,
 *         behavior over time), written once for all the faults that give
 *         the same outcome: five pins that all give the same black screen
 *         are one outcome with five causes.
 *   FALT  a FAULT run: machine, cartridge mode, the fault spec (what
 *         board_apply_fault_spec() takes, space separated if several), a
 *         description for people, and the key of its outcome.
 *   NETS  the names of a machine's nets, in the order PINS has them,
 *         written at the start of every build (the last one counts).
 *   PINS  what a logic probe showed on every net in the last second of a
 *         fault run (NET_LOW... of board.h, a byte each), for "Diagnose by
 *         probing" (probe.h). An atlas built before has none: the build
 *         runs those faults again for their PINS only.
 *
 * Two runs have the same outcome when they ended on the same pictures in
 * the same way: the set of different pictures of their last 64 frames (so
 * a blinking cursor matches whatever its phase when the run ended), or for
 * a flash code (Dead Test) the code and the flashing picture, or snow.
 * The fixed size headers are written as they are in memory (little endian,
 * no pointers): see the static asserts in atlas.c. */
#ifndef ATLAS_H
#define ATLAS_H
#include <stdint.h>
#include <stdbool.h>

/* The boards (a machine of the atlas is a board: the Spectrum's Issues
 * 3B, 2, 1 and 4B have faults and outcomes of their own, the C64's 250407
 * and 250469 too). New ones go at the end: the number is in the file. */
enum { ATLAS_C64, ATLAS_ZX48, ATLAS_ZX48I2, ATLAS_ZX48I1, ATLAS_ZX48I4B, ATLAS_ZX48I6A, ATLAS_C64C, ATLAS_NMACHINES };
/* What kind of machine a board is: ATLAS_C64 (either C64) or ATLAS_ZX48 (any Spectrum),
 * for what its pictures look like (palette, layout) and the hints written
 * for it. */
struct machine;
int atlas_family(int machine);
/* The board of an atlas machine, and the atlas machine of a board (-1 if
 * none). */
const struct machine *atlas_machine(int machine);
int atlas_machine_of(const struct machine *m);
/* Do two boards name their parts alike, so a fault of one ("U7.24:vcc")
 * can go on the other? The Spectrum issues do; the C64's 250407 and
 * 250469 don't (the 250469's U7 is the VIC). */
bool atlas_same_refs(int a, int b);
/* Cartridge modes. */
enum { MODE_BOOT, MODE_DEADTEST, MODE_ZXDIAG, ATLAS_NMODES };
/* Why a run ended before its last frame (see atlas.c, "Early out"). */
enum { STOP_END, STOP_NO_SIGNAL, STOP_CPU_DEAD, STOP_STATIC, STOP_FLASH_CODE };
/* What the CPU was doing at the end of a run. */
enum { CPU_RUNNING, CPU_RESET, CPU_STOPPED, CPU_JAMMED };
/* Thumbnail encodings. */
enum { THUMB_NONE, THUMB_RLE };

#define ATLAS_CELLS_W 40        /* character cells (the C64's 40x25 at most) */
#define ATLAS_CELLS_H 25
#define ATLAS_LAGS    32        /* autocorrelation lags 1..32 */
/* The logic probe readings (PINS) of a machine still running: frames
 * FROM to TO (5-6 s after power on, once it has settled). Readings taken
 * otherwise have another PINS version, and don't count. */
#define ATLAS_PROBE_FROM   250
#define ATLAS_PROBE_TO     300
#define ATLAS_PINS_VERSION 2

/* The fingerprint of a picture, made to compare with a captured picture
 * once that is brought to the machine's resolution and palette: every
 * pixel is a palette index (4 bits), the BORDER is what is outside the
 * active display (the C64's 320x200, the Spectrum's 256x192). */
typedef struct {
    uint8_t border_hist[16];    /* share of the border in each color, 0..255 */
    uint8_t active_hist[16];    /* the same for the active display */
    uint8_t cells_w, cells_h;   /* 40x25 (C64), 32x24 (Spectrum) */
    /* Per 8x8 cell: bits 0-3 its most frequent color, bits 4-5 the number
     * of colors in it minus 1 (up to 4): 0 = a plain cell. */
    uint8_t cells[ATLAS_CELLS_H][ATLAS_CELLS_W];
    /* Spatial periodicity of the active display: autocorrelation of the
     * column profile (how many pixels of each column aren't the background)
     * and of the row profile, lags 1..32 (0..255), and the strongest peak:
     * 8 for text, the width of jail bars, the height of attribute stripes. */
    uint8_t hcorr[ATLAS_LAGS], vcorr[ATLAS_LAGS];
    uint8_t hperiod, vperiod, hstrength, vstrength;
} atlas_fp_t;

/* How the picture behaved over time. */
typedef struct {
    uint8_t no_signal;          /* snow: no sync at all */
    uint8_t nseries;            /* complete flash series seen */
    uint8_t series[8];          /* flashes in each of them */
    uint16_t flash_lit;         /* frames a flash stays lit (average) */
    uint16_t flash_period;      /* frames from a flash to the next one */
    uint8_t distinct;           /* different pictures in the last 64 frames */
    uint8_t changes;            /* frames of the last 64 that changed the picture */
    uint16_t blink_period;      /* frames between changes when regular, else 0 */
} atlas_temporal_t;

/* OUTC payload: this, then 'len' bytes of thumbnail and 'alt_len' of the
 * second one (the lit picture of a flash code). Thumbnails are the
 * picture at half resolution, 4 bit palette indices, run length coded:
 * a byte (run - 1) << 4 | index, where a run field of 15 is followed by a
 * LEB128 varint of the run minus 16. */
typedef struct {
    uint64_t key;
    uint8_t machine, mode, enc, alt_enc;
    uint16_t w, h;
    uint32_t len, alt_len;
    atlas_temporal_t t;
    atlas_fp_t fp;
} atlas_outcome_hdr_t;

/* FALT payload: this, then the spec and the description (no zeros). */
typedef struct {
    uint64_t outcome;
    uint8_t machine, mode, stop, cpu;
    uint16_t frames;            /* frames run */
    uint16_t spec_len, desc_len;
    uint16_t pad[3];            /* zero */
} atlas_fault_hdr_t;

/* An atlas read back. */
typedef struct {
    atlas_outcome_hdr_t h;
    uint8_t *thumb, *alt;
    int *faults, nfaults;       /* the runs that gave it (atlas_t.faults) */
} atlas_outcome_t;

typedef struct {
    atlas_fault_hdr_t h;
    char *spec, *desc;
    int outcome;                /* in atlas_t.outcomes, -1 if missing */
    uint8_t *nets; int nnets;   /* its PINS: the probe state of each net of NETS, NULL if none */
} atlas_fault_t;

typedef struct {
    atlas_outcome_t *outcomes; int noutcomes;
    atlas_fault_t *faults; int nfaults;
    char **net_names[ATLAS_NMACHINES]; int nnet_names[ATLAS_NMACHINES];
    long valid_end;             /* file offset after the last good chunk */
} atlas_t;

/* Read an atlas (false if the file isn't one; a missing file is an empty
 * atlas). Chunks after a damaged one are ignored. */
bool atlas_load(atlas_t *a, const char *path, bool *exists);
void atlas_free(atlas_t *a);
/* Decode a thumbnail (or the flash one) into h.w * h.h palette indices. */
bool atlas_decode_thumb(const atlas_outcome_t *o, bool alt, uint8_t *out);
/* The 16 colors of a machine (RGBA8, as the monitor draws them). */
const uint32_t *atlas_palette(int machine);
const char *atlas_machine_id(int machine);
const char *atlas_mode_name(int mode);
/* A short label for an outcome: "Solid black screen", "Flash code: 8"... */
void atlas_outcome_label(const atlas_outcome_t *o, char *buf, int len);

typedef struct {
    const char *tier;           /* "quick" or "full" */
    const char *path;           /* the atlas file */
    const char *carts;          /* where the diagnostic cartridges are */
    int threads;                /* 0: one per logical processor */
} atlas_build_t;

/* The command line tools (exit codes). */
int atlas_build(const atlas_build_t *o);
int atlas_report(const char *path, const char *png_dir);
int atlas_diff(const char *path, const char *other);

/* ---- Matching a picture of a real machine (atlas_match.c) ---------------
 *
 *   c64-hw-sim --match shot.png [--top N] [--causes N] [--machine=c64|zx48]
 *              [--cart=normal|diag] [--match-debug DIR] [--atlas FILE]
 *   c64-hw-sim --match-test [N] [--match-dir DIR] [--seed S]
 *
 * The picture (PNG, BMP, JPG, TGA, GIF) is brought to the machine's own
 * resolution and palette: the capture card's black bars cut off, the
 * active display found (where the border ends, or where it should be),
 * the colors corrected and snapped to the palette, cell by cell. Then it
 * is compared with every outcome of the atlas: border and background
 * color, which character cells are drawn and how, stripes and their
 * period, the picture itself. */
/* A real repair's picture (repairs.h), matched as the outcomes are: its
 * screenshot as the matcher read it when it was recorded (half size
 * palette indices of MACHINE, the size of an outcome's thumbnail). */
typedef struct {
    int machine;
    const uint8_t *thumb;
    float snow;                 /* share of noise: above a half, a picture of no signal */
} atlas_real_t;

/* How a screenshot is read (the normalized picture a repair record
 * keeps): one more when hypotheses(), fit_colors() or normalize() change,
 * and the records read with an older one are read again from their
 * stored screenshot. */
#define ATLAS_READ_VERSION 2     /* 2: a frame of one color cut off (analyze()) */

typedef struct {
    const char *path;           /* the atlas */
    const char *image;          /* its name, and the file unless image_data is given */
    const unsigned char *image_data;    /* the picture's file in memory, IMAGE_LEN bytes */
    int image_len;
    int top;                    /* groups to list */
    int causes;                 /* causes listed per group, -1 all */
    int machine;                /* -1: either */
    int cart;                   /* -1: any, 0: normal boot, 1: diagnostic cartridge */
    const char *debug_dir;      /* write the normalized pictures there */
    const char *carts;
    const atlas_real_t *reals;  /* real repairs to score too (of the machine's family) */
    int nreals;
} atlas_match_opts_t;

int atlas_match(const atlas_match_opts_t *o);

/* The same for the interface: the groups as data. */
typedef struct {
    char spec[96];              /* what board_apply_fault_spec() takes, space separated; "" healthy */
    char desc[700];
} atlas_cause_t;

typedef struct {
    int machine, mode;
    float score;                /* 0..1 */
    char label[256];
    bool healthy;               /* the healthy board gives this picture too */
    int same_as;                /* an earlier group with the very same picture, or -1 */
    bool flash;                 /* a flash code: a screenshot can't count flashes */
    int w, h;
    uint32_t *thumb;            /* the simulated picture, w x h RGBA (malloc'd) */
    int ncauses;
    atlas_cause_t *causes;      /* malloc'd */
} atlas_group_t;

#define ATLAS_MAX_GROUPS 8
#define ATLAS_MAX_REALS 8
typedef struct {
    char error[300];            /* set when it failed */
    char summary[300];          /* the picture, and how it was read */
    int w, h;
    uint32_t *picture;          /* the screenshot, at most 400 wide, RGBA (malloc'd) */
    int ngroups;
    atlas_group_t groups[ATLAS_MAX_GROUPS];
    /* The screenshot as read (the reading the best group came from): what
     * a repair record keeps. Half size palette indices, malloc'd. */
    uint8_t *norm;
    int norm_machine, norm_w, norm_h;
    float norm_snow;
    /* The real repairs (atlas_match_opts_t.reals) most alike, best first. */
    int nreals;
    struct { int index; float score; } reals[ATLAS_MAX_REALS];
} atlas_result_t;

/* Match o->image, the best o->top groups (at most ATLAS_MAX_GROUPS);
 * false with r->error set if it can't. */
bool atlas_match_run(const atlas_match_opts_t *o, atlas_result_t *r);
void atlas_result_free(atlas_result_t *r);
/* A picture file in memory (a repair record's screenshot) made small, at
 * most MAXW wide, RGBA (malloc'd), as atlas_result_t.picture; NULL if it
 * can't be read. */
uint32_t *atlas_small_picture(const unsigned char *data, int len, int maxw, int *w, int *h);
/* The synthetic capture card test: COUNT different screens of the atlas,
 * run again at full size, made to look like a bad composite capture
 * (written to OUT_DIR), matched: where the true group ranks. */
int atlas_match_test(const atlas_match_opts_t *o, int count, const char *out_dir, unsigned seed);
/* Where the diagnostic cartridges are (the ZX Diagnostics font). */
void atlas_set_carts(const char *dir);
/* The text read on a thumbnail (atlas_decode_thumb), line by line with
 * '\n' between lines; the number of characters read. */
int atlas_read_text(int machine, int mode, const uint8_t *thumb, char *out, int len);

/* Shared by atlas.c and atlas_match.c. */
void atlas_init_shared(void);          /* tables the threads share */
const char *atlas_mode_id(int mode);   /* "boot", "deadtest", "zxdiag" */
const char *atlas_color_name(int machine, int index);
void atlas_active_area(int machine, int *x, int *y, int *w, int *h);
void atlas_screen_size(int machine, int *w, int *h);
/* Full size palette indices -> the thumbnail's half size. */
void atlas_half(const uint8_t *idx, int w, int h, uint8_t *out);
/* Run one fault the way the build does; its last picture (and, for a
 * flash code, the lit one) at full size, malloc'd. */
bool atlas_run_fault(int machine, int mode, const char *spec, const char *carts, uint64_t *key, uint32_t **frame,
                     uint32_t **lit, char *err, int errlen);

/* The CPU's threads (at least 1). */
int atlas_logical_processors(void);

#endif
