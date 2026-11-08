/* lacap_int.h - what the lacap*.c files share (types, and the functions and
 * tables one uses of another). Not for the rest of the program: lacap.h is.
 * Made by splitting lacap.c (2026-11-08).
 *
 * AI-generated (Claude Code). */
#ifndef LACAP_INT_H
#define LACAP_INT_H
#include "lacap.h"
#include "board.h"
#include "zx48.h"
#include "lapico.h"
#include "atlas.h"
#include "filedialog.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

typedef struct { int sig; const char *nets; } sig_nets_t;

/* ------------------------------------------------------------------------ */
/* Reading: a small JSON walker for .lac                                     */
/* ------------------------------------------------------------------------ */

typedef struct { const char *p, *end; bool bad; } js_t;

typedef struct { int number; char name[64]; uint8_t *v; int n; } rawchan_t;

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

/* Pairs of addresses already looked at (a set), for glitch_mark. */
#define PAIRS_BITS 22

/* ------------------------------------------------------------------------ */
/* Bus cycles                                                                */
/* ------------------------------------------------------------------------ */

static inline bool lo(uint64_t v, int sig) { return !(v >> sig & 1); }
static inline unsigned addr_of(uint64_t v) { return (unsigned)(v >> LA_A0 & 0xFFFF); }

/* The reset's first fetch from 0000. The button bounces as it's let go
 * (for a few ms): the CPU starts again from 0000 each time, and a working
 * ROM never goes back there in the boot's first ms. So it's the last
 * restart that runs the ROM's code right after it (a CPU going astray
 * reaches 0000 too, on garbage: not a bounce) and leaves enough to compare
 * (300 bus cycles); the restarts before it: *restarts (may be NULL). */
#define BOUNCE_MIN_AFTER 300

/* What compare() found. */
typedef struct {
    int rc;                     /* 0 the same (or only the state), 2 a fault */
    bool diverged;              /* the CPU went another way (not an interrupt's timing) */
    int cycle;                  /* there */
    la_cycle_t a, b;
} cmp_out_t;
typedef struct { const char *stage; int pre, post; } diag_plan_t;
#define DIAG_NSTAGES 6

enum { AR_ROM, AR_LRAM, AR_URAM, AR_N };

/* What a capture of the board under test shows against a working one's. */
typedef struct {
    int bad[AR_N][8], low[AR_N][8], high[AR_N][8];  /* reads with the bit wrong; of them 0 for 1, 1 for 0 */
    int nbad[AR_N], first[AR_N];                    /* reads with a bit wrong, the first one's address */
    int stuck[LA_NSIG];                             /* 1 low, 2 high all through, where a working one's changes */
    int nearly[LA_NSIG];                            /* of them: but for a few moments (how many changes) */
    int lvl[LA_NSIG];                               /* the line in the capture: 1 low, 2 high all through, 0 not */
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
    int clk_edge;                                   /* CLK read stuck (1 low, 2 high) while the CPU ran the ROM right */
    bool rom_runs;                                  /* the CPU fetched the ROM's own bytes (100 or more) */
    int rom_writes;                                 /* writes to 0000-3FFF (the ROM: a stack there) */
    int idle_still[LA_NSIG];                        /* left running, the ROM running: never changed (1 low, 2 high) */
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

#ifdef _WIN32
#define mkdir1(p) _mkdir(p)
#else
#define mkdir1(p) mkdir(p, 0777)
#endif

/* Before the boot: the Spectrum left running, no reset: a capture at once
 * (on A0 high, else low, which always comes), at 14 MHz (the bus) and at
 * 2 MHz (49 ms: /RESET and the frame interrupt, every 20 ms). With /RESET
 * broken the boot's stages can't trigger; this can. */
#define IDLE_SLOW 2000000
#define IDLE_MS 2000            /* the simulation: idle in BASIC from 1.8 s after power on */

#define STOP_MSG "  Couldn't capture it: the diagnosis goes on what the stages before show\n"

/* The parts a fault SPEC is on ("IC13:data0=1 R26:removed" -> IC13 R26; a
 * bridge "IC7.5-6:short" -> IC7; the ULA inside -> ULA's ref). */
typedef struct { char ref[16]; int n, first; } spart_t;

/* ---- The check (--la-check): the screen's faults put on the simulated
 * board one by one, each stage recorded and compared with the analyser's
 * capture of it. The fault that does what the real board did, stage by
 * stage, is the one. ---- */

#define CHECK_MAX 32
typedef struct {
    char spec[96], desc[320];
    int same;                   /* stages the same as the captures */
    long cycles;                /* where the others differ: cycles the same before it */
    bool bad;                   /* couldn't be simulated */
} cand_t;

typedef struct {
    const machine_t *m;
    cand_t *c; int nc;
    la_capture_t *d; const char *const *path; int nst;
    int next;                   /* the next job: candidate * nst + stage */
    pthread_mutex_t mu;
    int done, total;
} check_t;

extern FILE *la_logf;
extern bool la_screen;
extern char *vtext;
extern size_t vtext_n, vtext_cap;
extern bool vtext_on;
extern const char *const sig_names[LA_NSIG];
extern const sig_nets_t sig_nets[];
extern const size_t sig_nets_n;
extern int chan_map[48];
extern const la_stage_t la_stages[];
extern const diag_plan_t diag_plan[];
extern board_t *vboard;

void la_status(int kind, int stage, int state);
int la_out(const char *fmt, ...);
int pico_out(const char *fmt, ...);
int pico_quiet(const char *fmt, ...);
void la_err(const char *msg);
bool la_wait(const char *prompt);
const char *la_signal_name(int sig);
bool la_load_map(const char *path, char *err, int errlen);
bool la_build(la_capture_t *c, rawchan_t *ch, int nch, char *err, int errlen);
long addr_pattern(unsigned a);
bool la_read(const char *path, la_capture_t *c, char *err, int errlen);
void la_free(la_capture_t *c);
bool la_write(const char *path, const la_capture_t *c, char *err, int errlen);
void rec_step(board_t *b, void *ctx);
void find_nets(board_t *b, int net[LA_NSIG], la_capture_t *c);
bool la_record(board_t *b, int pre, int post, la_capture_t *c, char *err, int errlen);
bool la_record_at(board_t *b, int pre, int post, int trig_addr, int arm_ms, int max_ms, la_capture_t *c, char *err, int errlen);
int timeline(const la_cli_t *o);
int la_start(const la_capture_t *c);
int la_cycles(const la_capture_t *c, la_cycle_t **out);
void cycle_text(const la_cycle_t *y, char *out, int len);
bool rom_wrong(const la_capture_t *c, const la_cycle_t *y);
void no_ram_cells(const la_cycle_t *cy, int n, unsigned char *bad);
void print_info(const char *path, const la_capture_t *c, int count);
int first_fetch0_n(const la_cycle_t *cy, int n, int *restarts);
int compare(const char *an, const la_capture_t *a, const char *bn, const la_capture_t *b,
                   int context, bool refresh, cmp_out_t *res);
int sim_length(const la_capture_t *a);
board_t *la_new_board(const machine_t *m);
void options_text(const machine_t *m, char *out, int len);
int pick_ports(const char *list, char ports[][64], int max, char *err, int errlen);
const char *went_when(int cycle);
const char *stage_plain(const char *name);
const la_stage_t *stage_named(const char *t);
bool capture_real(const la_cli_t *o, la_capture_t *c, char *err, int errlen);
bool record_board(const la_cli_t *o, int post, int trig_addr, la_capture_t *c, char *err, int errlen);
int trigger_addr_of(const char *t);
const char *board_desc(const la_cli_t *o, char *out, int len);
void ev_init(ev_t *e);
void ev_merge(ev_t *e, const ev_t *f);
unsigned ev_mask(const ev_t *e, int ar);
bool ev_pramt_bad(const ev_t *e);
bool int_wrong(const ev_t *e);
bool ev_any(const ev_t *e);
bool cpu_busy(const la_capture_t *c);
int line_state(const la_capture_t *c, int s);
void gather(ev_t *e, const la_capture_t *d, const la_capture_t *r);
void where_text(const la_capture_t *c, char *out, int len);
board_t *sim_board(const machine_t *m, const char *const *faults, int nfaults, char *err, int errlen);
bool record_sim(const machine_t *m, const char *const *faults, int nfaults, int pre, int post, int trig, int arm_ms,
                       la_capture_t *c, char *err, int errlen);
bool capture_now(const la_cli_t *o, double at_ms, const char *save, la_capture_t *c, char *err, int errlen);
bool want_enter(const char *msg);
const char *ref_of(const char *label);
int verdict(const ev_t *e, const machine_t *m, const char *stage, bool missing, const cmp_out_t *co, const char *where);
bool record_idle(const machine_t *m, const char *const *faults, int nfaults, la_capture_t *fast, la_capture_t *slow,
                        char *err, int errlen);
bool low_all(const la_capture_t *c, int sg);
void slow_ev(ev_t *e, const la_capture_t *d, const la_capture_t *r);
bool idle_dead(const ev_t *e);
int idle_stage(const la_cli_t *o, const machine_t *m, const char *dir, ev_t *ie);
/* The parts serving a RAM bank as a whole; the resistors to its chips'
 * data lines; the chips on a signal's line (lacap_verdict.c). */
void bank_parts(const char *bank, const char *chips, char *out, int len);
void data_resistors(const char *chips, char *out, int len);
void data_resistors_of(const char *chips, unsigned mask, char *out, int len);
int line_chips(int sig, char *out, int len);
/* A ROM image plugged into the boards record_sim makes (NULL: none). */
extern const char *la_sim_cart;
/* --la-diagnose --la-zxtest: the test ROM's results (lacap_zxtest.c). */
int zxtest_diagnose(const la_cli_t *o);
int diagnose(const la_cli_t *o);

/* Allocation that can't come back empty: out of memory, it says so and
 * stops, rather than crash later on a NULL (a capture is 5 MB, the
 * timeline's tables a few more). */
void *la_alloc_fail(size_t n);
static inline void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); return p ? p : la_alloc_fail(n); }
static inline void *xcalloc(size_t k, size_t n) { void *p = calloc(k ? k : 1, n ? n : 1); return p ? p : la_alloc_fail(k * n); }
static inline void *xrealloc(void *q, size_t n) { void *p = realloc(q, n ? n : 1); return p ? p : la_alloc_fail(n); }

/* What the diagnosis prints goes to the window or the log (la_out). */
#define printf la_out

#endif
