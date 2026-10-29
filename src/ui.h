/* ui.h - the graphical interface: board view, monitor, scope, controls. */
#ifndef UI_H
#define UI_H
#include "sokol_headers.h"
#include "board.h"

void ui_init(board_t *b, const char *prg, const char *scope_pin);
/* Put another machine on the bench (the current board goes away). */
void ui_set_machine(const machine_t *m);
void ui_frame(void);
void ui_event(const sapp_event *ev);
void ui_save_screenshot(const char *path);
int ui_emulated_frame(void);
void ui_inspect(const char *ref);
void ui_continuity(const char *spec);
void ui_browse(void);
/* Put the board of a save file (.hwstate) on the bench, with its notes. */
void ui_load_state(const char *path);
void ui_view(float cx, float cy, float zoom);
void ui_apply_fault(const char *spec);
void ui_diagnose(const char *path);
/* Print the interface's frame times every second (--ui-stats). */
void ui_print_stats(bool on);
/* Open "Browse the atlas" at start, the Nth picture picked (-1 none). */
void ui_browse_atlas(int pick);
void ui_shutdown(void);
/* Light or dark windows: "light", "dark" or "system" (the default: as the
 * system is set, followed while running). False for a name it doesn't know. */
bool ui_set_theme(const char *name);
/* Is the system set to light windows? (sokol_impl.c: it asks the OS.) */
bool ui_system_light(void);

#endif
