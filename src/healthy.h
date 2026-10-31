/* healthy.h - the reference sheet of a healthy board (to-do item 2): what
 * a logic probe shows on every net of the board working normally, for
 * comparing with a broken one on the bench. Measured where a person
 * probes a real machine, once it has settled (frames ATLAS_PROBE_FROM to
 * ATLAS_PROBE_TO, as the atlas's PINS: the probing window's "healthy
 * board" readings are the same), and for a pulsing net how fast (rising
 * edges a second over the last HEALTHY_EDGE_FRAMES frames) and how much of
 * the time it is high. The simulator looks at the nets once per bus step,
 * so nothing faster than half that rate shows (healthy_t.max_hz).
 *
 *   c64-hw-sim --healthy-sheet DIR     a printable page (HTML) per board
 *
 * In the program a pin's tooltip says what the healthy board shows there,
 * and "Healthy board sheet" writes the page of the board on the bench and
 * opens it. Measured in the background the first time it is needed (a few
 * seconds per board), kept until the program ends. */
#ifndef HEALTHY_H
#define HEALTHY_H
#include "board.h"

#define HEALTHY_EDGE_FRAMES 10
#define HEALTHY_MAX_OPTIONS 8

typedef struct {
    uint8_t state;              /* NET_* */
    float hz;                   /* rising edges a second, 0 if none in the window */
    float high;                 /* share of the time driven high, 0..1 */
} healthy_net_t;

typedef struct {
    const machine_t *m;
    int options[HEALTHY_MAX_OPTIONS];
    int nnets;
    healthy_net_t *net;
    double max_hz;              /* the fastest a net can be seen to change */
    double window;              /* seconds the edges were counted over */
} healthy_t;

/* Measure board M built with OPTIONS (machine_option_t values; NULL the
 * defaults). One at a time: others wait. */
healthy_t *healthy_measure(const machine_t *m, const int *options);
void healthy_free(healthy_t *h);
/* What the healthy board shows on NET, in words: "pulsing, 985 kHz, high
 * 50%", "HIGH". */
void healthy_text(const healthy_t *h, board_t *b, int net, char *out, int len);
/* The printable page of board B (built as H was measured). */
bool healthy_write_html(board_t *b, const healthy_t *h, const char *path, char *err, int errlen);

/* For the program: the sheet of board B as it is built, or NULL while it
 * is measured in the background (started by the first call). */
const healthy_t *healthy_get(board_t *b);
/* --healthy-sheet DIR: the page of every board. The exit code. */
int healthy_cli(const char *dir);

#endif
