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

/* The bus cycles of a capture, from /RESET going high (or the start).
 * Returns how many (*out malloc'ed). */
int la_cycles(const la_capture_t *c, la_cycle_t **out);

/* The command line: --la-info, --la-cycles, --la-record, --la-compare,
 * --la-devices, --la-capture (from the analyser itself: lapico.h). */
typedef struct {
    const char *info, *cycles, *record, *compare, *against, *map;
    bool devices;               /* --la-devices: list the boards */
    const char *capture;        /* --la-capture FILE */
    const char *ports;          /* --la-ports MASTER,OTHER (ports or serial numbers) */
    const char *trigger;        /* --la-trigger reset | addr=XXXX | raw:T,B,C,V */
    int freq;                   /* --la-freq HZ */
    int timeout;                /* --la-timeout S */
    const struct machine *machine;
    const char *const *faults; int nfaults;
    int count;                  /* --la-count: cycles to list / context */
    int pre, post;              /* --la-pre, --la-post */
    bool refresh;               /* --la-refresh: compare refresh cycles too */
} la_cli_t;
int la_cli(const la_cli_t *o);

#endif
