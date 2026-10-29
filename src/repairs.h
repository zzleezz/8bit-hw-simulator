/* repairs.h - real repair records (to-do item 7): a screenshot of a broken
 * machine and the chips that were found faulty, kept in a file of their
 * own (repairs.bin next to the program: the atlas gets rebuilt), matched by
 * "Diagnose from screenshot" beside the simulated pictures.
 *
 * The file is a chunk log like the atlas (atlas.c): an 8 byte header,
 * then chunks of a tag, a length, the payload and its FNV-1a sum, only
 * ever added to; a damaged chunk ends the reading, the ones before it
 * stay. DSET: a dataset (its random ID and its owner's name: the last one
 * written counts). RECD: a record. A record is the dataset's ID and a
 * number (unique, so datasets can be merged) and a version (a newer one
 * of the same record replaces it: an edit, or a deletion, which stays as
 * a record flagged deleted so a merge doesn't bring the old one back).
 * SETR: a record left out of the diagnosis in this file only (not
 * exported). NORM: a record's screenshot read again by a newer matcher
 * (ATLAS_READ_VERSION), kept here so it isn't read at each start. */
#ifndef REPAIRS_H
#define REPAIRS_H
#include <stdbool.h>
#include <stdint.h>

#define REPAIR_MAX_CHIPS 16

/* How sure the record is that its chips were the fault. */
enum { REPAIR_UNSURE, REPAIR_SWAP_FIXED, REPAIR_TESTED_BAD, REPAIR_NCERTAINTY };

#define REPAIR_DELETED 1        /* repair_t.flags: taken back by its owner */

typedef struct {
    char ref[16];               /* U7, IC5 */
    char what[64];              /* what it is on that board: "CPU: 6510" (board numbers differ) */
} repair_chip_t;

typedef struct {
    uint64_t dataset;           /* who made it */
    uint32_t number, version;
    int64_t when;               /* time() it was recorded */
    char machine[16];           /* machine_t.id of the board: "c64", "zx48i2" */
    uint8_t certainty;          /* REPAIR_* */
    int nchips;
    repair_chip_t chips[REPAIR_MAX_CHIPS];
    char notes[1024];
    char shot_name[128];        /* the screenshot's file name */
    uint8_t *shot;              /* the file itself (PNG, JPG, BMP), malloc'd */
    uint32_t shot_len;
    /* As the matcher read the screenshot (atlas_result_t.norm), what it
     * is matched with: half size palette indices of NORM_MACHINE. */
    int norm_machine;
    uint16_t norm_w, norm_h;
    uint8_t *norm;
    float snow;
    uint8_t flags;              /* REPAIR_DELETED */
    uint16_t read_version;      /* the ATLAS_READ_VERSION NORM was read with */
    bool off;                   /* left out of the diagnosis (here only) */
} repair_t;

typedef struct {
    uint64_t id;                /* random, made with the dataset: never changes */
    char owner[64];             /* a display name, changeable */
} repair_dataset_t;

/* The records of the file: the own dataset's and any merged in. */
typedef struct {
    char path[2200];
    repair_dataset_t own;       /* id 0 until the first record is saved */
    repair_dataset_t *sets;     /* every dataset the file names */
    int nsets;
    repair_t *r;
    int n;
    char error[300];            /* why the file couldn't be read or written, "" if it could */
    bool foreign;               /* the file is something else: never written to */
} repairs_t;

/* Read the records of PATH (a missing file: none). */
void repairs_load(repairs_t *rs, const char *path);
void repairs_free(repairs_t *rs);
/* Add a record of the own dataset (it gets its dataset, number and
 * version here; the record's buffers are taken over) and write it to the
 * file. False with rs->error set if it couldn't be written. */
bool repairs_add(repairs_t *rs, repair_t *rec);
/* A record of the own dataset changed (its notes, certainty, chips, or
 * flags): written as its next version. */
bool repairs_update(repairs_t *rs, int index);
/* A record's screenshot read again: its new reading (taken over: NORM is
 * malloc'd) kept, and written to this file if it changed. */
bool repairs_set_reading(repairs_t *rs, int index, uint16_t read_version, int machine, int w, int h, float snow,
                         uint8_t *norm);
/* Leave a record out of the diagnosis, or put it back (in this file). */
bool repairs_set_off(repairs_t *rs, int index, bool off);
/* Add another file's records: the new ones, and newer versions of ones
 * here (the rest skipped); its datasets come with them. */
bool repairs_merge(repairs_t *rs, const char *path, int *added, int *updated, int *skipped);
/* Write a records file to share: the own dataset's records (deleted ones
 * too, so a merge takes them back), or with ALL every record here. */
bool repairs_export(repairs_t *rs, const char *path, bool all);
/* A record shown and matched: not deleted. */
static inline bool repair_live(const repair_t *r) { return !(r->flags & REPAIR_DELETED); }
/* Change the own dataset's owner name (written to the file). */
bool repairs_set_owner(repairs_t *rs, const char *owner);
/* A dataset's owner name, "" if unknown. */
const char *repairs_owner(const repairs_t *rs, uint64_t dataset);
const char *repairs_certainty_name(int certainty);

#endif
