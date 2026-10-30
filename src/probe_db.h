/* probe_db.h - "Diagnose by probing": readings taken with a logic probe on
 * a real broken machine, matched against what the probe showed under every
 * fault of the fault atlas (its PINS chunks, see atlas.h), and the pin to
 * probe next.
 *
 * The faults that show the very same thing on every net are one GROUP: no
 * probe on the board can tell them apart. A group's COST is how badly it
 * fits the readings (0 for each reading it agrees with, up to 1 for each it
 * doesn't: see probe_cost()); the groups still IN THE RUNNING are those
 * within PROBE_TOLERANCE of the best, so one misread pin doesn't throw the
 * true fault out. The next pin to probe is the one whose reading splits
 * the groups in the running most evenly (the most information), counting
 * every group once: a group of ten faults on one net is still one answer.
 *
 * A reading on the solder side (the pad) is of the NET a pin is on. One
 * on a chip's leg, above the board, is of the net too, unless that leg is
 * open (lifted, a bad joint or socket): then it shows what the chip drives
 * there, or floats for an input (the atlas's LEGS). That tells an open leg
 * apart from a healthy board, which its pad alone often can't.
 *
 * Not every fault is as likely: on the bench the PLA fails most, then the
 * CIAs, the CPU, the RAM and the ROMs, and the maker printed on a chip
 * moves it (MT RAM and MOS logic fail a lot, a PLA not made by MOS
 * hardly). Every fault has a WEIGHT for that (probe_part_weight()): with
 * a STRATEGY other than PROBE_FEWEST, groups that fit as well are listed
 * the likeliest first, and the pin to probe next splits the likely faults
 * rather than all of them alike. PROBE_BENCH also starts the way a repair
 * does (reset and clock at the CPU, then the PLA outputs: bench_pins in
 * probe_db.c): reset and clock always, the others skipped when they
 * can't tell the suspects apart. A net found only at a connector (the
 * Spectrum's keyboard rows) is suggested only if it tells clearly more
 * than a chip's leg. C64HW_PROBE_TRACE=1 makes --probe-test print every
 * pin it probes. */
#ifndef PROBE_DB_H
#define PROBE_DB_H
#include "atlas.h"
#include "board.h"

/* What the probe shows. PULSING: any activity, even rare. FLOATING: no
 * light, or a dim one (an in-between level), nothing driving it. BAD:
 * active, but not reaching a proper low or high on a scope (two chips
 * fighting, a weak output: the atlas's NET_CONTENTION). */
enum { PROBE_LOW, PROBE_HIGH, PROBE_PULSING, PROBE_FLOATING, PROBE_BAD, PROBE_NREADINGS };

/* One reading wrong is forgiven (--probe-test: with 10% of the readings
 * wrong, the true fault stays in the running 94-99% of the time against
 * 60-85% with 0.75, at ~3 more probes; 2.0 does no better). */
#define PROBE_TOLERANCE 1.25f

/* How the next pin is picked (see above). */
enum { PROBE_BENCH, PROBE_LIKELY, PROBE_FEWEST, PROBE_NSTRATEGIES };
const char *probe_strategy_name(int strategy);

/* What is printed on a chip of the real machine. */
enum { MARK_UNKNOWN, MARK_MOS, MARK_MT, MARK_OTHER };
/* Which marks matter for a part: MARK_MOS (the PLA, logic chips),
 * MARK_MT (RAM), or MARK_UNKNOWN (none: the weight doesn't depend on the
 * maker). */
int probe_part_markable(board_t *b, int part);
/* How likely a part is to fail on the bench, marked MARK (1: a logic
 * chip, 10: the PLA). */
float probe_part_weight(board_t *b, int part, int mark);
/* How likely a fault spec (the atlas's, "R17:short") is, on board B with
 * no chip markings known: its likeliest part's weight, less for a leg or a
 * bridge (1 for the healthy board). */
float probe_fault_weight(board_t *b, const char *spec);

typedef struct {
    int pin;                    /* the pin probed (a pin index of the board) */
    int net;                    /* its net */
    int reading;                /* PROBE_* */
    bool leg;                   /* on the chip's leg (else on the pad, the solder side) */
} probe_reading_t;

typedef struct {
    int *faults, nfaults;       /* in atlas.faults */
    uint8_t *nets;              /* NET_* of every net of the board, NET_NONE if unknown */
    /* The open chip legs of its faults (pins of the board, in order) and
     * what the probe shows on each leg (NET_*). */
    int *legs; uint8_t *leg_state; int nlegs;
    bool healthy;               /* the healthy board is in it */
    float cost;                 /* set by probe_rank() */
    int mismatches;             /* readings it disagrees with (cost >= 0.5) */
    int nmatch;                 /* its faults that give the screen picked (all if none) */
    float weight;               /* the WEIGHT of those faults, summed */
} probe_group_t;

typedef struct {
    atlas_t atlas;
    int machine;                /* ATLAS_C64, ATLAS_ZX48... */
    /* Where the readings come from: MACHINE, or another board of the same
     * family when the atlas has none of this one yet (the Issue 2 gets
     * the Issue 3B's: the same circuit, the nets named alike). */
    int source;
    int nnets;                  /* the board's */
    probe_group_t *groups; int ngroups;
    int nfaults;                /* with probe readings, in all the groups */
    int healthy;                /* the healthy board's group, -1 if none */
    /* What the screen shows: the pictures of the atlas, by label (the
     * outcomes alike enough to have the same label are one), most faults
     * first; each fault's (in atlas.faults, -1 none). Each picture is of a
     * KIND ("Vertical bars") and has COLORS ("white border, black screen",
     * "" if the label doesn't say), so the list can be narrowed in steps:
     * both index KINDS and COLORSETS, most faults first. */
    struct probe_screen { char label[256]; int nfaults, kind, colors; } *screens; int nscreens;
    struct probe_kind { char name[64]; int nfaults; } *kinds, *colorsets; int nkinds, ncolorsets;
    int *fault_screen;
    bool *screen_on;            /* the pictures picked (any of them fits) */
    bool screen_any;            /* none picked: any picture */
    int screen;                 /* the one picture picked, -1 if several or any */
    float *fault_weight;        /* each fault's WEIGHT (in atlas.faults) */
    int strategy;               /* PROBE_* */
    bool on_legs;               /* readings are taken on the chips' legs (else on the pads) */
    bool have_legs;             /* the atlas has LEGS for this board */
} probe_db_t;

/* Load the faults of MACHINE with probe readings from the atlas at PATH,
 * their nets matched to board B's (by name). False with ERR set if there
 * are none. */
bool probe_db_load(probe_db_t *db, const char *path, int machine, board_t *b, char *err, int errlen);
void probe_db_free(probe_db_t *db);

/* How badly a net the simulation shows as NET_STATE fits a READING:
 * 0 (the same) to 1 (nothing alike). */
float probe_cost(int net_state, int reading);
/* What a probe would show for a NET_* state (PROBE_*), -1 for NET_NONE. */
int probe_reading_of(int net_state);
const char *probe_reading_name(int reading);

/* Only the faults that give screen picture S (db->screens), -1 any. */
void probe_db_set_screen(probe_db_t *db, int s);
/* Only the faults that give one of the pictures ON (one per screen; NULL:
 * any). */
void probe_db_set_screens(probe_db_t *db, const bool *on);
/* The weights of the faults, with the chips of B marked MARKS (MARK_*, one
 * per part; NULL: none marked). Each group's faults get listed the
 * likeliest first (the healthy board still first). */
/* MARKS (or NULL) the chip markings, BOOST (or NULL) a factor per part
 * from the repair records (diagnose_record_boost()). */
void probe_db_set_weights(probe_db_t *db, board_t *b, const uint8_t *marks, const float *boost);
/* A fault of the atlas that gives the screen picked. */
bool probe_fault_shown(const probe_db_t *db, int fault);
/* The KIND of picture a screen label (atlas_outcome_label()) is of, and
 * its COLORS ("" if the label doesn't say): "Vertical bars: 32 of them,
 * ... (white border, black screen), animated" is "Vertical bars" in
 * "white border, black screen". The atlas browser sorts by them too. */
void probe_screen_kind(const char *label, char *kind, int klen, char *colors, int clen);

/* What group G shows where reading R was taken (NET_*). */
int probe_group_state(const probe_group_t *g, const probe_reading_t *r);

/* Rank the groups on READINGS: ORDER gets every group index, the best
 * first; the number in the running is returned. Groups with no fault
 * giving the screen picked come last, out of the running. */
int probe_rank(probe_db_t *db, const probe_reading_t *readings, int n, int *order);

/* The net to probe next among the first NRUNNING groups of ORDER (none
 * of READINGS'), and on it the PIN that is easiest to find (an IC pin, on
 * the smallest chip), or an open leg some of them have; *LEG: probe the
 * chip's leg (db->on_legs, or a leg only its chip side tells apart),
 * else the pad. COUNTS[r]: the groups that would show reading r there;
 * *BENCH (if not NULL): a step of the bench procedure, 0 if not. -1 if
 * nothing tells them apart. */
int probe_suggest(const probe_db_t *db, board_t *b, const probe_reading_t *readings, int n, const int *order,
                  int nrunning, int *pin, bool *leg, int counts[PROBE_NREADINGS], int *bench);
/* The bench procedure's step K (1...): what it checks, NULL past the end. */
const char *probe_bench_step(int machine, int k);

/* The easiest pin to probe a net on (see probe_suggest()), -1 if none. */
int probe_pin_for_net(board_t *b, int net);

/* --probe-test: COUNT faults of the atlas at random on each machine, run
 * on a board, probed where probe_suggest() says (NOISE_PCT: readings
 * taken wrong on purpose) until the groups in the running can't be told
 * apart: where the true fault ends up, for each strategy on the same
 * faults. Faults are picked as likely as their WEIGHT (none marked), the
 * way they come to the bench. The exit code. */
int probe_test(const char *atlas_path, int count, unsigned seed, int noise_pct);

#endif
