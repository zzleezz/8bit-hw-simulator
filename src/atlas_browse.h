/* atlas_browse.h - "Browse the atlas": the pictures of the fault atlas
 * (atlas.h) looked through by what the screen shows, rather than by
 * matching a screenshot. Every picture a board's faults give, in a grid,
 * narrowed by its characteristics (the kind of picture, the border and
 * screen colors, how it behaves, what the CPU was doing, a chip one of
 * its causes is on), each choice saying how many pictures it leaves, and
 * by probe readings taken on the real machine. A picture picked shows its
 * causes, each ready to put on the board, and can be taken to "Diagnose
 * by probing". */
#ifndef ATLAS_BROWSE_H
#define ATLAS_BROWSE_H
#include "ui_common.h"

/* Open the window on the pictures of MACHINE (the board on the bench),
 * as it was left (the atlas is diagnose_atlas_file()). */
void atlas_browse_open(int machine);
/* Hide it (the filters and the picture picked are kept). */
void atlas_browse_close(void);
/* Show the causes of the Nth picture listed (0 the first), for
 * --browse-atlas=N. */
void atlas_browse_pick(int n);
bool atlas_browse_is_open(void);
/* The pin field has the keyboard (keys aren't the machine's then). */
bool atlas_browse_wants_keyboard(void);
/* The window on the screen, so the board below ignores the mouse. */
bool atlas_browse_bounds(struct nk_rect *r);

/* What the user asked for in the window. */
typedef struct {
    bool apply;                 /* put a fault on the board */
    int machine;                /* the board (ATLAS_*) */
    char spec[96];              /* space separated fault specs, "" healthy */
    bool probe;                 /* "Diagnose by probing" narrowed to LABEL */
    char label[256];            /* atlas_outcome_label() of the picture */
} atlas_browse_action_t;

/* Draw the window, if open, over AREA. B is the board on the bench (its
 * pins are the ones probed), MACHINE which one it is: another one, the
 * window shows that one's pictures. */
void atlas_browse_window(struct nk_context *ctx, struct nk_rect area, board_t *b, int machine,
                         atlas_browse_action_t *act);
void atlas_browse_shutdown(void);

#endif
