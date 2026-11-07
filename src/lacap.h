/* lacap.h - logic analyser captures of the ZX Spectrum's edge connector.
 *
 * The analyser: HappyLittleDiodes' (Jim's) ZX Spectrum interface with two
 * of Agustin Gimenez Bernad's LogicAnalyzer boards (github.com/gusmanb/
 * logicanalyzer) chained as one 48 channel device. Its program saves a
 * capture as .lac (JSON: each channel's samples, one byte 0/1 per sample,
 * base64) or CSV (a header of channel names, one row of 0/1 per sample).
 *
 * Here: read those, record the same from a simulated board (a capture of
 * the board on the bench as the analyser would take it), cut a capture
 * into Z80 bus cycles, and compare two captures cycle by cycle from the
 * reset. Groundwork for to-do.md section 10, made before the real
 * analyser was built: the channel map is read from Jim's schematic and
 * still to be checked on the real thing. */
#ifndef LACAP_H
#define LACAP_H

#include <stdbool.h>
#include <stdint.h>

/* The signals of the edge connector the analyser can see. */
enum {
    LA_A0, LA_A15 = LA_A0 + 15,
    LA_D0, LA_D7 = LA_D0 + 7,
    LA_CLK, LA_RESET, LA_MREQ, LA_IORQ, LA_RD, LA_WR, LA_M1, LA_RFSH,
    LA_HALT, LA_WAIT, LA_INT, LA_NMI, LA_BUSRQ, LA_BUSACK, LA_ROMCS,
    LA_IORQGE, LA_VIDEO, LA_Y, LA_U, LA_V,
    LA_NSIG
};

/* A capture: per sample, a bit per signal (bit LA_xxx). */
typedef struct {
    uint64_t *s;                /* the samples */
    int n;                      /* how many */
    uint64_t have;              /* the signals the capture has */
    int freq;                   /* samples per second (0: not known) */
    int pre;                    /* samples before the trigger */
    int trig_addr;              /* triggered on the address bus showing this, or -1 (a reset; not known) */
    int chan[LA_NSIG];          /* each signal's channel (0-47), or -1 */
} la_capture_t;

/* A bus cycle of the Z80, as seen on the edge connector. */
enum { LA_FETCH, LA_READ, LA_WRITE, LA_IN, LA_OUT, LA_INTACK, LA_REFRESH };
typedef struct {
    uint8_t kind;               /* LA_FETCH... */
    uint8_t data;
    uint16_t addr;
    int sample;                 /* where it starts */
    int t;                      /* in T states (CLK rises) since /RESET went high */
} la_cycle_t;

const char *la_signal_name(int sig);

/* The default channel map (Jim's interface, rev 3.2), or one read from a
 * file: lines "CHANNEL NAME" (channel 1-48, as the analyser's program
 * numbers them), # starts a comment. Used when a capture's channels have
 * no names the signals can be told by. */
bool la_load_map(const char *path, char *err, int errlen);

/* Read a capture (.lac or .csv). */
bool la_read(const char *path, la_capture_t *c, char *err, int errlen);
/* Write one (.lac or .csv by the name), with the channel map's channels. */
bool la_write(const char *path, const la_capture_t *c, char *err, int errlen);
void la_free(la_capture_t *c);

/* Record the simulated board as the analyser would: from power on, every
 * bus step (28 MHz, 8 per T state), triggered on /RESET going high, PRE
 * samples before and POST after. B is a board_t (a Spectrum). */
struct board;
bool la_record(struct board *b, int pre, int post, la_capture_t *c, char *err, int errlen);
/* The same, triggered as the analyser is on the address bus showing
 * TRIG_ADDR (from /RESET going high and ARM_MS after power on; -1: on
 * /RESET going high), the simulation run up to MAX_MS from power on. */
bool la_record_at(struct board *b, int pre, int post, int trig_addr, int arm_ms, int max_ms, la_capture_t *c, char *err, int errlen);

/* The bus cycles of a capture, from /RESET going high (or the start).
 * Returns how many (*out malloc'ed). */
int la_cycles(const la_capture_t *c, la_cycle_t **out);

/* The command line: --la-info, --la-cycles, --la-record, --la-compare,
 * --la-devices, --la-capture (from the analyser itself: lapico.h). */
typedef struct {
    const char *info, *cycles, *record, *compare, *against, *map;
    bool devices;               /* --la-devices: list the boards */
    bool timeline;              /* --la-timeline: when the simulated boot gets where */
    int ms;                     /* --la-ms: how long to run it */
    const char *capture;        /* --la-capture FILE */
    const char *ports;          /* --la-ports MASTER,OTHER (ports or serial numbers) */
    const char *trigger;        /* --la-trigger reset | addr=XXXX | raw:T,B,C,V */
    int freq;                   /* --la-freq HZ */
    int timeout;                /* --la-timeout S */
    double at;                  /* --la-at MS: armed on the reset, then again MS after it */
    bool bootsel;               /* --la-bootsel: restart the boards (--la-ports) into USB drive mode */
    const char *diagnose;       /* --la-diagnose DIR: the boot stage by stage, and what's wrong */
    bool replay;                /* --la-replay: from DIR's captures, not the analyser */
    bool simulate;              /* --la-simulate: the simulated board with --fault as the one under test */
    bool all;                   /* --la-all: after a stage that's wrong, capture the rest too */
    int tries;                  /* --la-tries N: re-arm after a glitch this often */
    const char *log;            /* --la-log FILE: a copy of what's printed */
    const struct machine *machine;
    const char *const *faults; int nfaults;
    int count;                  /* --la-count: cycles to list / context */
    int pre, post;              /* --la-pre, --la-post */
    bool refresh;               /* --la-refresh: compare refresh cycles too */
} la_cli_t;
int la_cli(const la_cli_t *o);

/* The stages a capture can be triggered at (--la-trigger NAME): the
 * address, and what it is. NULL name ends the list. */
/* For the GUI (la_window.c), set before la_cli(): where the text goes
 * (else stdout and stderr), and how it asks the user to go on (else Enter
 * on stdin): true to go on, false for no (or stopped). PROMPT is what it
 * asked (shown already), or NULL. */
extern void (*la_text_hook)(const char *text);
extern bool (*la_ask_hook)(const char *prompt);
/* And how far it is (from its thread): LA_EV_STAGE with STAGE 0 (idle) to
 * 6 (the boot's six) and its STATE; LA_EV_ARMED and LA_EV_TRIGGERED as
 * the analyser arms and fires. */
enum { LA_EV_STAGE, LA_EV_ARMED, LA_EV_TRIGGERED };
enum { LA_ST_NONE, LA_ST_RUN, LA_ST_OK, LA_ST_FAULT, LA_ST_NEVER, LA_ST_SKIP };
extern void (*la_status_hook)(int kind, int stage, int state);
typedef struct { const char *name; int addr; double ms; const char *what; } la_stage_t;
extern const la_stage_t la_stages[];

#endif
