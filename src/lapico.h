/* lapico.h - talk to Agustin Gimenez Bernad's LogicAnalyzer boards
 * (github.com/gusmanb/logicanalyzer, GPL-3) over USB serial, without his
 * program: find the boards, ask what they are, take a capture.
 *
 * The protocol (from his firmware, Firmware/LogicAnalyzer_V2, and his
 * driver, Software/LogicAnalyzer/SharedDriver, branch version/v6_0):
 * packets 55 AA <command> <data> AA 55, with 55, AA and F0 in between sent
 * as F0 <byte ^ F0>. Command 0 asks for the board (replies: a version line,
 * FREQ:, BLASTFREQ:, BUFFER:, CHANNELS:), 1 starts a capture (a 48 byte
 * request; reply CAPTURE_STARTED or CAPTURE_ERROR), a lone FF byte stops
 * one. When the capture is done the board sends a 32-bit sample count, the
 * samples (1, 2 or 4 bytes each, bit k = the k-th channel asked for) and a
 * byte of burst timestamps (0 here).
 *
 * Chained boards (the 3-pin cable): every board but the first is started
 * first on an edge trigger from the cable (channel 24), then the first,
 * the master, with the real trigger, which only the master can have (its
 * channels 1-16). */
#ifndef LAPICO_H
#define LAPICO_H

#include <stdbool.h>
#include <stdint.h>

#define LP_MAXDEV 5

typedef struct {
    char port[64];              /* COM12, /dev/ttyACM0 */
    char serial[64];            /* the board's USB serial number, if known */
} lp_port_t;

/* The boards plugged in (USB 1209:3020). Returns how many (up to max). */
int lp_find(lp_port_t *out, int max);

typedef struct {
    char version[64];           /* LOGIC_ANALYZER_PICO_2_V6_0 */
    int max_freq, blast_freq, buffer, channels;
} lp_info_t;

/* Ask one board what it is. */
bool lp_info(const char *port, lp_info_t *info, char *err, int errlen);

/* Trigger types, as the firmware numbers them. */
enum { LP_EDGE = 0, LP_COMPLEX = 1, LP_FAST = 2 };

typedef struct {
    const char *port[LP_MAXDEV];/* the master first */
    int ndev;
    int nchan;                  /* channels to capture, 0-based across the boards (24 a board) */
    int chan[LP_MAXDEV * 24];
    int freq;                   /* samples a second */
    int pre, post;              /* samples before and after the trigger */
    int trig_type;              /* LP_COMPLEX or LP_FAST */
    int trig_base, trig_count;  /* the master's channels base..base+count-1 */
    unsigned trig_value;        /* bit k: channel base+k */
    int timeout;                /* seconds to wait for the trigger (0: a minute) */
} lp_capture_req_t;

/* Where "Armed" and "Triggered" are printed (printf unless set). */
extern int (*lp_printf)(const char *fmt, ...);

/* Take a capture. On success *v[k] holds the samples (0/1, *n of them) of
 * req->chan[k], malloc'ed; *pre the samples before the trigger. */
bool lp_capture(const lp_capture_req_t *req, uint8_t **v, int *n, int *pre, char *err, int errlen);

#endif
