/* diagnose.h - "Diagnose from screenshot": a picture of a broken machine's
 * screen (a USB capture card's screenshot, dropped on the window or
 * picked with the button of the controls) matched against the fault atlas
 * (atlas.h): the screenshot, the groups of faults that give the most
 * alike picture, their simulated pictures and causes, and for every cause
 * a button that puts it on the board. */
#ifndef DIAGNOSE_H
#define DIAGNOSE_H
#include "ui_common.h"

/* The atlas and where the diagnostic cartridges are (--atlas, --carts). */
void diagnose_init(const char *atlas_path, const char *carts);
/* The repair records file (--repairs; default repairs.bin next to the
 * program). Before the first diagnosis. */
void diagnose_repairs_file(const char *path);
/* Where the atlas is (next to the program when not where we run from). */
void diagnose_atlas_file(char *out, int len);
/* A picture the matcher takes (.png .jpg .jpeg .bmp). */
bool diagnose_is_picture(const char *path);
/* Match a picture against the faults of MACHINE (ATLAS_C64, ATLAS_ZX48:
 * the one on the bench) and show the results (or why it couldn't). */
void diagnose_open(const char *path, int machine);
/* The machine on the bench changed: the picture matched again against
 * that one's faults (nothing if no picture, or already that machine). */
void diagnose_set_machine(int machine);
void diagnose_close(void);
/* Results to show again ("Show the last diagnosis"). */
bool diagnose_has_results(void);
void diagnose_show(void);
bool diagnose_is_open(void);
/* The labels of the pictures that match the screenshot best (the first
 * group's, and any as good within a few percent), at most MAX: what
 * "Diagnose by probing" narrows its faults to. 0 if no results. */
int diagnose_best_labels(char (*labels)[256], int max);
/* The window on the screen, so the board below ignores the mouse. */
bool diagnose_bounds(struct nk_rect *r);

/* Recording a real repair (repairs.h): its window is open while the
 * faulty chips are clicked on the board, which then toggles them (REF,
 * and WHAT it is on that board) instead of opening the fault menu. */
bool diagnose_recording(void);
void diagnose_record_toggle(const char *ref, const char *what);
bool diagnose_record_marked(const char *ref);
void diagnose_record_cancel(void);
/* The window of all the records (look, leave out, edit, delete, merge,
 * export), and where it is. */
void diagnose_records_open(int bench);
/* The records with chip REF faulty, on boards of BENCH's family: how
 * many, and the records window showing only them. */
int diagnose_records_of_chip(int machine, const char *ref);
void diagnose_records_open_chip(int bench, const char *ref);
void diagnose_records_close(void);
bool diagnose_records_bounds(struct nk_rect *r);
/* The file asked for by diagnose_action_t.records_file, picked: merged
 * from (1), or exported to (2 one's own, 3 all). */
void diagnose_records_file(int what, const char *path);
/* Without a window (--list-repairs, --merge-repairs, --export-repairs,
 * --reread-repairs: every stored screenshot read again):
 * 0, or 1 if it failed. */
int diagnose_cli_records(const char *list, const char *merge, const char *export_to, bool all, bool reread);
/* What the repair records say of each part of B (a MACHINE board): a
 * factor in BOOST (B's nparts), 1 for none. A chip found faulty on real
 * boards of the family counts a little, more for each record; one in a
 * record that matched the last screenshot of this board, a lot. A line
 * saying which in NOTE. The number of parts moved up. */
int diagnose_record_boost(board_t *b, int machine, float *boost, char *note, int len);
/* One more each time the records, or the matches of the last screenshot,
 * change (diagnose_record_boost() then gives something else). */
unsigned diagnose_records_serial(void);
/* Typing in the record's notes or name: the keys aren't the machine's. */
bool diagnose_wants_keyboard(void);

/* What the user asked for in the window. */
typedef struct {
    bool apply;                 /* put a fault on the board */
    int machine;                /* ATLAS_C64, ATLAS_ZX48 */
    char spec[96];              /* space separated fault specs, "" healthy */
    bool pick;                  /* open another screenshot */
    bool probe;                 /* narrow the best matches down by probing */
    int bench;                  /* put this machine on the bench (and match against it), -1 none */
    int records_file;           /* ask for a file: 1 to merge, 2 to export one's own records, 3 all */
} diagnose_action_t;

/* Draw the window, if open, over 'area'. */
void diagnose_window(struct nk_context *ctx, struct nk_rect area, diagnose_action_t *act);
/* Without a window (--diagnose-text, --add-repair): PICTURE diagnosed on
 * MACHINE and printed, real repair records too; with CHIPS ("IC5,IC6"),
 * that repair recorded first. 0, or 1 if it failed. */
int diagnose_cli(const char *picture, int machine, const char *chips, int certainty, const char *notes,
                 const char *owner);
void diagnose_shutdown(void);

#endif
