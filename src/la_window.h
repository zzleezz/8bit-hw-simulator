/* la_window.h - "Diagnose with the logic analyser": the window. Runs
 * --la-diagnose (lacap.c) in a thread of its own: the text comes into the
 * window as it goes, its "press Enter" questions are buttons, and the
 * chips the verdict names are outlined on the board. Three ways: from the
 * analyser on the real Spectrum, again from a folder of captures taken
 * before, or the simulated board with a fault as the one under test.
 *
 * AI-generated (Claude Code). */
#ifndef LA_WINDOW_H
#define LA_WINDOW_H
#include "ui_common.h"

void la_window_open(void);
bool la_window_is_open(void);
/* For tests and screenshots (--la-window): open it and start at once,
 * SPEC "replay:FOLDER" or "simulate:FAULT|FOLDER" (FAULT may be empty);
 * "demo:ask", "demo:armed", "demo:triggered" show how a run on the
 * analyser looks, without one; anything else only opens it. */
void la_window_open_test(const char *spec);
void la_window_close(void);
/* The window on the screen, so the board below ignores the mouse. */
bool la_window_bounds(struct nk_rect *r);
/* A text field has the keyboard (keys aren't the machine's then). */
bool la_window_wants_keyboard(void);
/* How part REF is named by the last verdict, on MACHINE's board: 0 not,
 * 1 a suspect, 2 the first thing to look at. */
int la_window_mark(const machine_t *machine, const char *ref);
/* Draw the window, if open, over AREA; MACHINE is the board on the bench
 * (a Spectrum's is diagnosed: its netlist names the chips). */
void la_window(struct nk_context *ctx, struct nk_rect area, const machine_t *machine);
/* A run still going stops at its next question (and the thread is left). */
void la_window_shutdown(void);

#endif
