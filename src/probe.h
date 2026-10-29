/* probe.h - "Diagnose by probing": the window. Readings taken with a logic
 * probe on the real broken machine go in one by one; the faults of the
 * atlas that fit them stay on the list, and the window says which pin to
 * probe next (the matching is probe_db.h). */
#ifndef PROBE_H
#define PROBE_H
#include "ui_common.h"

void probe_open(void);
void probe_close(void);
bool probe_is_open(void);
/* The window on the screen, so the board below ignores the mouse. */
bool probe_bounds(struct nk_rect *r);
/* The pin field has the keyboard (keys aren't the machine's then). */
bool probe_wants_keyboard(void);
/* The pin it asks to probe now, to mark on the board (-1 none). */
int probe_marked_pin(void);
/* What the screen shows, from a screenshot diagnosis: the faults narrowed
 * to those giving one of the N pictures LABELS (atlas_outcome_label()), of
 * the machine on the bench; the readings are kept. */
void probe_screens_from_screenshot(const char (*labels)[256], int n);

/* What the user asked for in the window. */
typedef struct {
    bool apply;                 /* put a fault on the board */
    char spec[96];              /* space separated fault specs, "" healthy */
    int show_pin;               /* go to this pin on the board, -1 none */
} probe_action_t;

/* Draw the window, if open, over AREA, for board B (MACHINE: ATLAS_C64 or
 * ATLAS_ZX48). A pin clicked on the board (B's scope pin changing) is the
 * pin to take a reading of. */
void probe_window(struct nk_context *ctx, struct nk_rect area, board_t *b, int machine, probe_action_t *act);
void probe_shutdown(void);

#endif
