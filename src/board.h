/* board.h - a board: the netlist of a machine and its parts, and what
 * every machine has in common (faults, logic probe, oscilloscope, labels,
 * the RESET button). What makes a machine (the cores of its big chips, its
 * bus cycle, its keyboard, what it can load, its video) is described by a
 * machine_t: see c64.c. */
#ifndef BOARD_H
#define BOARD_H
#include "sim.h"
#include "parts.h"

#define SCOPE_SAMPLES   65536           /* scope ring buffer (power of two) */

/* What a pin's net looked like over the last frame (logic probe colors). */
enum { NET_LOW, NET_HIGH, NET_PULSING, NET_FLOATING, NET_CONTENTION, NET_NONE };
/* Scope sample bits. */
#define SC_LEVEL    1
#define SC_FLOATING 2
#define SC_FIGHT    4
/* What the pin itself drives (its leg, not the net): high, low, or neither
 * (off, or an input). */
#define SC_OWN_HI   8
#define SC_OWN_LO   16

#define BOARD_MAX_BRIDGES 16            /* solder bridges between neighbor legs */

/* The role of a chip, to color its name. */
enum { ROLE_RAM, ROLE_VRAM, ROLE_ROM, ROLE_PLA, ROLE_CHIP, ROLE_GLUE, ROLE_CLOCK, ROLE_OTHER };

typedef struct board board_t;

/* A chip of the board: its model, a short name drawn on the chip, a few
 * short lines shown under the name when zooming in ('\n' separated), its
 * role and a one line description. */
typedef struct {
    const char *ref;
    const part_type_t *type;
    const char *label;
    int role;
    const char *info;
    const char *detail;
} part_def_t;

/* A connector: its name and what each pin is for, "PAD=FUNCTION ..."
 * ('_' is shown as a space). */
typedef struct { const char *ref, *name, *pins; } conn_def_t;

/* A kind of file a machine takes (see machine_t.file_kind). */
typedef struct {
    const char *type;           /* "program", "cartridge", "snapshot" */
    const char *action;         /* the button that opens it: "Load and RUN" */
    bool norun;                 /* it can also be loaded without running it */
    int role;                   /* ROLE_* to color it, -1 for plain text */
} file_kind_t;

/* A choice of how a board is built, as it varies between real boards of
 * the same issue: which chips are fitted, which version of a chip. Its
 * name, a line about it, and its values (the first is the default). */
typedef struct {
    const char *id;             /* for --option ID=VALUE: "ram" */
    const char *name;           /* "RAM" */
    const char *help;
    const char *const *values;  /* "48K", "16K" */
    const char *const *value_ids;   /* for --option: "48k", "16k" */
    int nvalues;
} machine_option_t;

typedef struct machine {
    const char *id;             /* "c64" (--machine) */
    const char *name;           /* "Commodore 64" */
    const char *board_name;     /* "C64 ASSY 250407" */
    /* Boards of the same machine (the Spectrum's issues): the short name
     * of this one ("Issue 2", NULL if the machine has only one board), and
     * what the machine's code needs to know about it. */
    const char *issue;
    const void *variant;
    size_t size;                /* of the machine's state, which starts with a board_t */
    const board_data_t *data;
    sim_desc_t sim;
    double cpu_hz;              /* sim.cycle counts these */
    int cycles_per_frame;       /* a video frame */
    int steps_per_cycle;        /* board_sample() calls per cycle (scope time base) */
    int screen_w, screen_h;     /* the picture of board_monitor() */
    uint64_t ready_cycles;      /* from power on to the prompt (BASIC ready) */
    const part_def_t *parts; int nparts;
    const conn_def_t *conns; int nconns;
    const char *link_resistors; /* resistors that are just wires: "RP1 RP2" */
    const char *crystals;       /* other parts that can be broken: "Y1" */
    /* For the continuity test, two pin parts a meter sees through that
     * aren't resistors or links: diodes and solder jumpers (reference
     * prefixes, "CR" for CR1, CR2...), and the supplies that aren't rails
     * of the simulation (net names). */
    const char *diodes, *jumpers, *supplies;
    const char *keys_help;      /* how the host keyboard maps to the machine's */
    const char *reset_help;     /* where the RESET button is */
    /* Set up the cores, models and wiring (board_create() did the rest). */
    void (*create)(board_t *b);
    void (*destroy)(board_t *b);
    /* Switched on: the cores start over (board_power_on() does the rest). */
    void (*power_on)(board_t *b);
    void (*run)(board_t *b, int cycles);
    /* Joins of nets for board_rebuild(): pressed keys, dongles. */
    void (*joins)(board_t *b);
    /* Keyboard: machine key codes, and the host key (a sokol keycode) or
     * the host character that types each. */
    void (*key_down)(board_t *b, int key);
    void (*key_up)(board_t *b, int key);
    void (*frame_update)(board_t *b, uint32_t us);
    int (*host_key)(int keycode);
    int (*host_char)(int c);
    void (*monitor)(board_t *b, uint32_t *out);
    /* Files: the title of the browser, what a file is for this machine
     * (NULL if it isn't one it takes), with a line about it, and whether
     * it can't be used (bad). */
    const char *files_title;
    const file_kind_t *(*file_kind)(const char *path, char *info, int len, bool *bad);
    /* Cartridges: the connector they plug into, the name of the one
     * plugged (NULL if none), pulling it out (the machine restarts). */
    const char *cart_slot;
    const char *(*cart_name)(board_t *b);
    void (*remove_cart)(board_t *b);
    /* A ROM image of several banks (a DesTestSwitch EPROM), one seen at a
     * time: how many (0 if none), the one the switch selects, a line about
     * a bank, and flipping the switch (the machine restarts). Optional. */
    int (*cart_banks)(board_t *b);
    int (*cart_bank)(board_t *b);
    const char *(*cart_bank_info)(board_t *b, int bank);
    void (*set_cart_bank)(board_t *b, int bank);
    /* Load a file (program, cartridge...): false and a message if not. */
    bool (*load)(board_t *b, const char *path, bool run, char *msg, int msglen);
    const char *(*cpu_status)(board_t *b);              /* "running", "in reset"... */
    void (*cpu_regs)(board_t *b, char *buf, int len);    /* registers, for reports */
    void (*print_text)(board_t *b);                     /* --text: read the screen */
    /* After a save state was loaded into the board: put back what of the
     * machine's state is not the machine (a cache it allocated), from the
     * board as it was just before. Optional. */
    void (*state_loaded)(board_t *b, const board_t *before);
    /* Where a CPU address lives in a RAM chip (part): its chip address,
     * for a DRAM (row << 8) | column as the board wires it, or -1 if the
     * chip doesn't hold that address. Optional. */
    int (*ram_cell)(board_t *b, int part, uint16_t addr);
    /* How this board is built (see machine_option_t): the choices, the
     * value of one, and changing it (the machine restarts). Optional. */
    const machine_option_t *options; int noptions;
    int (*get_option)(board_t *b, int option);
    void (*set_option)(board_t *b, int option, int value);
    /* The label of a chip when it depends on the options (NULL: the one
     * of 'parts'). Optional. */
    const part_def_t *(*part_def)(board_t *b, int part);
} machine_t;

extern const machine_t machine_c64, machine_c64c, machine_zx48, machine_zx48i2, machine_zx48i1, machine_zx48i4b, machine_zx48i6a;

struct board {
    sim_t sim;
    const machine_t *m;
    /* Video, as the machine's video chip drew it (screen_w x screen_h). */
    uint32_t *screen;
    /* The monitor without a video signal: its snow generator (per board,
     * so boards running in parallel threads don't share it), and whether
     * the last board_monitor() picture was snow. */
    uint32_t snow_seed;
    bool no_signal;
    /* Oscilloscope: one sample per bus step for the selected pin, and for
     * a second one (scope_pin2 >= 0: the other leg of a solder bridge). */
    int scope_pin, scope_pin2;
    uint8_t scope[SCOPE_SAMPLES], scope2[SCOPE_SAMPLES];
    uint32_t scope_pos;
    /* Solder bridges: pairs of pins (indices) shorted together, joined by
     * board_rebuild(). A fault like the others: Repair all removes them. */
    int bridge[BOARD_MAX_BRIDGES][2], nbridges;
    /* Self-check oracle for tests (C64HW_CHECK, see the machines). */
    bool check;
    uint64_t check_errors;
    /* Logic probe statistics per net, reset by the UI every frame. */
    uint32_t *st_hi, *st_lo, *st_float, *st_fight;
    int st_countdown;
    /* Also sampled when pin_stats is set (the UI does): what each pin
     * itself drives, which can differ from its net (a lifted leg, two
     * outputs fighting, an open collector), and how often each product
     * term of the PLA is true (p_pla < 0: no PLA). Reading them changes
     * nothing simulated. */
    bool pin_stats;
    uint32_t *pst_hi, *pst_lo, *pst_off, *pst_weak;
    int p_pla;
    bool pla_super;         /* p_pla is the 250469's Super PLA */
    uint32_t pla_hits[64], pla_samples;
    /* The RESET button: a switch from /RESET to GND on a connector, like a
     * reset cartridge (pins set by the machine, -1 if none). */
    bool reset_button;
    int pin_reset, pin_reset_gnd;
    /* Connectors: what each pin is for ("ATN"), NULL for other parts. */
    const char **pin_func;
    char **conn_strings;
    /* Supplies, as wired on the healthy board (see board_rebuild): per
     * pin, the net a chip's power pin gets its supply from (-1: not a power
     * pin) and what that net was (RAIL_*); the main ground and +5V nets. */
    int *pwr_src;
    uint8_t *pwr_rail;
    int gnd_net, vcc_net;
    /* Which board this is, set up how: a new number for every board and
     * every change of what is plugged in (cartridge, ROM in U4, harness).
     * A save state only loads into the board it was taken from. */
    uint32_t config_id;
    /* The file of what is plugged in (a cartridge, a ROM in U4), "" if
     * nothing: a save file carries it. */
    char plugged[260];
};

board_t *board_create(const machine_t *m);
void board_destroy(board_t *b);
void board_power_on(board_t *b);
/* What is plugged in changed (cartridge, ROM in U4, harness): save states
 * taken before don't fit anymore. */
void board_config_changed(board_t *b);

/* Save states: the whole running machine (chips, memories, every net and
 * pin, the faults), kept in memory. Loading puts the board back exactly as
 * it was, and works only on the same board, set up the same way (else
 * false). */
typedef struct board_state board_state_t;
board_state_t *board_save_state(board_t *b);
bool board_load_state(board_t *b, const board_state_t *st);
void board_free_state(board_state_t *st);
/* Save files (.hwstate): a save state on disk, with notes kept with it
 * (text, "" for none) and a copy of what is plugged in. Loading makes a
 * new board, set up as the saved one was, and puts the state in it; NULL
 * and a message if the file can't be read, or was made by a version of
 * the simulator that lays the machine out differently. */
bool board_save_file(board_t *b, const char *path, const char *notes, char *err, int errlen);
board_t *board_load_file(const char *path, char *notes, int noteslen, char *err, int errlen);
bool board_is_state_file(const char *path);
/* The file's name without its folders (either slash). */
const char *board_basename(const char *path);
/* For machine_t.file_kind: what a save file is (NULL if the path isn't
 * one). */
const file_kind_t *board_state_file_kind(const char *path, char *info, int len, bool *bad);
/* Rebuild the electrical network after changing faults, links or what
 * a part drives. */
void board_rebuild(board_t *b);
void board_run(board_t *b, int cycles);
/* For the machines: after every bus step, the scope and the statistics. */
void board_sample(board_t *b);

/* Faults. Every change rebuilds the electrical network. */
void board_set_pin_fault(board_t *b, int pin, int fault);
void board_set_removed(board_t *b, int part, bool removed);
void board_set_rfault(board_t *b, int part, int rfault);
void board_clear_faults(board_t *b);
/* A fault inside a memory chip (MF_*, see sim.h). */
typedef struct { uint8_t kind, bit, val; uint32_t addr; } mem_fault_t;
/* Put one on a memory chip (MF_OK repairs it). */
void board_set_mem_fault(board_t *b, int part, const mem_fault_t *mf);
/* Read the text form of one (after the ':' of a spec, see below): false
 * if it isn't one, or doesn't fit the chip. */
bool board_parse_mem_fault(board_t *b, int part, const char *what, mem_fault_t *mf);
/* Its text form (the spec after the ':'), and what it means for people. */
void board_mem_fault_spec(const mem_fault_t *mf, char *out, int len);
void board_mem_fault_desc(board_t *b, int part, const mem_fault_t *mf, char *out, int len);
/* Fault specs: U17:removed  U17.18:gnd  U17.18:vcc  U17.18:open  R51:open
 * R51:short  RESET:pressed, and a solder bridge between two neighbor legs
 * of a DIP chip: U13.11-12:short (U13.11-12:ok removes it; U13.8-9 is
 * refused: the two ends of the rows face each other across the chip).
 * A dead output, stuck low or high inside the chip: U14.3:out0 U14.3:out1.
 * Memory chips (chip addresses in hex, a DRAM's is (row << 8) | column):
 *   U4:data3=1     data bit D3 reads 1 at every address
 *   U4:addr5       address line A5 dead inside the chip
 *   U4:flip=1F2A^04  bit 2 of the byte at 1F2A reads inverted (^ the bits,
 *                  1 if left out: a DRAM has one)
 *   U21:flip@05F4  the same at a CPU address, in a RAM chip that holds it
 *                  (see machine_t.ram_cell)
 *   U21:weak       a DRAM with weak cells
 *   U4:ok          the memory chip repaired */
bool board_apply_fault_spec(board_t *b, const char *spec);
/* Solder bridge between two pins (indices): add (on) or remove it. False
 * if there's no room for another. */
bool board_set_bridge(board_t *b, int pin_a, int pin_b, bool on);
/* The bridge between two pins (either order), or -1. */
int board_find_bridge(board_t *b, int pin_a, int pin_b);
/* A pin's physical neighbor in a DIP package (dir -1 or +1 in pin
 * numbers) on the same side of the chip: -1 at the ends of a row (pins
 * N/2 and N/2+1 face each other across the chip) and for non-DIP parts. */
int board_dip_neighbor(board_t *b, int pin, int dir);
/* A bridge in words: "U13 74LS257 pins 11-12 shorted: A5 and A6 bridged". */
void board_bridge_desc(board_t *b, int pin_a, int pin_b, char *out, int len);
/* Press or release the RESET button (also as the spec "RESET:pressed" /
 * "RESET:released"). It is not a fault: Repair all doesn't release it. */
void board_set_reset_button(board_t *b, bool pressed);
bool board_part_simulated(board_t *b, int part);
/* Short name drawn on a chip ("RAM1", "PLA"...), the lines shown under it
 * when zooming in (model, address range), a one line description, and its
 * role (to color the label). */
const char *board_part_label(board_t *b, int part);
const char *board_part_info(board_t *b, int part);
const char *board_part_detail(board_t *b, int part);
int board_part_role(board_t *b, int part);
/* What a part is, from its reference: "capacitor", "ferrite bead",
 * "resistor", "transistor", "chip"... */
const char *board_part_kind_name(board_t *b, int part);
/* Can a part body be shorted (a resistor, a capacitor, a diode, any two
 * pin part; not a link, which is a wire already, nor a chip)? */
bool board_part_can_short(board_t *b, int part);
/* The pin of a part by its pad name ("18", "B"; "D" or "4" for the
 * cassette port's "D,4"), or -1. */
int board_pad_pin(board_t *b, int part, const char *pad);
/* Connectors: name ("SERIAL") and the function of a pin ("ATN"), or NULL. */
const char *board_connector_name(board_t *b, int part);
const char *board_pin_func(board_t *b, int pin);
/* Is a net part of the simulated circuit (not just power or analog)? */
bool board_net_simulated(board_t *b, int net);

/* For the continuity test (a meter on the bench): the pairs of legs of a
 * part with a resistor, a wire, a diode or a capacitor between them (a
 * resistor pack has several), and what each is. A meter beeps through a
 * link (ferrite bead, zero ohm link, jumper, fuse, coil), and through any
 * two pin part shorted; never through a capacitor (open at DC). Returns
 * how many (at most max). */
enum { PASS_LINK, PASS_RESISTOR, PASS_DIODE, PASS_CAP };
typedef struct { int pin_a, pin_b, kind; } passive_t;
int board_passives(board_t *b, int part, passive_t *out, int max);
/* Is a net a supply (+5V, GND, +12V, 9V AC...)? RAIL_GND for a ground,
 * RAIL_VCC for the others, RAIL_NONE if not. */
int board_net_supply(board_t *b, int net);

/* Keyboard (machine key codes), loading files, video: see machine_t. */
void board_key_down(board_t *b, int key);
void board_key_up(board_t *b, int key);
void board_frame_update(board_t *b, uint32_t us);
bool board_load(board_t *b, const char *path, bool run, char *msg, int msglen);
const char *board_cart_name(board_t *b);
void board_remove_cart(board_t *b);
int board_cart_banks(board_t *b);
int board_cart_bank(board_t *b);
const char *board_cart_bank_info(board_t *b, int bank);
void board_set_cart_bank(board_t *b, int bank);
/* A raw ROM image (.rom, .bin) made for the C64 (8K banks with CBM80 or
 * 6502 vectors at the top) rather than the Spectrum (cart.c). */
bool rom_image_is_c64(const uint8_t *buf, size_t n);
void board_monitor(board_t *b, uint32_t *out);

/* Logic probe: what a net did since the last board_stats_reset(). */
int board_net_state(board_t *b, int net);
void board_stats_reset(board_t *b);
/* What a pin itself drove (NET_HIGH/LOW/PULSING, or NET_FLOATING if off). */
int board_pin_drive_state(board_t *b, int pin);

#endif
