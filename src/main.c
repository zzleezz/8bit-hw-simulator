/* main.c - c64-hw-sim entry point.
 *
 *   c64-hw-sim [program.prg | cartridge.crt | image.rom | image.bin] [--harness]
 *       [--fault SPEC]... [--machine c64|c64c|zx48|zx48i2|zx48i1|zx48i4b|zx48i6a]
 *       [--board 250407|250469|issue3b|issue4b|issue6a|issue2|issue1]
 *       [--option ID=VALUE]... [--bank N] [--socket u4|port]
 *       Graphical simulator. The optional .prg is loaded and RUN after boot;
 *       a .crt cartridge or a raw .rom/.bin ROM image is plugged into the
 *       expansion port before power on (a raw image picks the machine it is
 *       made for), --harness plugs the 586220 diagnostic harness. --bank
 *       sets the switch of a ROM of several banks (DesTestSwitch), --socket
 *       puts an 8K C64 image in the KERNAL socket U4 or the expansion port
 *       (default: U4 for a KERNAL, the port for anything else).
 *       --board picks the C64's board (c64 is the ASSY 250407, c64c the
 *       250469, with the Super PLA) or the Spectrum's (zx48 is the Issue 3B,
 *       zx48i2 the Issue 2, zx48i1 the Issue 1, a 16K machine, zx48i4b the
 *       Issue 4B, zx48i6a the Issue 6A);
 *       --option sets how it is built: ram=48k|16k (3B, 4B, 6A and 2), ula=5c112|6c001 (Issue 2) or
 *       ula=5c102|5c112|6c001 (Issue 1).
 *
 *   c64-hw-sim --headless FRAMES [--shot out.png] [--text] [--fault SPEC]... [prg]
 *       Run without a window for FRAMES video frames (50 per second), then
 *       save what the monitor shows (--shot) and/or print the text read
 *       from the picture (--text). Handy for tests. Environment:
 *       --nets also lists the nets that had contentions, and who drives them.
 *       --fault-after N applies the faults after N frames (on a running C64).
 *       --at N:SPEC applies one fault at frame N; SPEC "repair" repairs all,
 *       "save" saves the state and "load" loads it back (a test of save states);
 *       "load:FILE" loads a program, snapshot or cartridge then;
 *       "save:FILE" saves it to a save file.
 *       --flashes counts the series of full screen flashes (how Dead Test
 *       reports a bad RAM chip), and says if the screen is lit at the end.
 *       C64HW_TYPE="text"  type this after boot (\n is RETURN)
 *       C64HW_CHECK=1      self-check oracle for the healthy board
 *
 *   --load-state FILE
 *       Start with the board of a save file (.hwstate), as it was saved.
 *
 *   --screenshot out.png [--frames N]
 *       In graphical mode: save the whole window after N frames and quit.
 *   --record N:DIR [--frames COUNT]
 *       In graphical mode: from emulated frame N, save the window every 4
 *       emulated frames (12.5 per second) as DIR/00000.png..., COUNT times,
 *       then quit. Made to turn into a video. --at works here too.
 *
 *   For screenshots and tests of the interface (graphical mode):
 *   --inspect REF       open the chip inspector on REF ("U8")
 *   --continuity REF.PIN   continuity test on a pin ("U12.14"), as shift-click
 *   --view X,Y,ZOOM     center the board on X,Y mm, ZOOM times the full view
 *   --mouse X,Y         keep the mouse at X,Y (window pixels): hovering
 *   --down X,Y@F, --up X,Y@F   press / release the left button at UI frame F
 *   --wheel X,Y,DY@F    turn the mouse wheel DY notches (+ up) at X,Y at UI frame F
 *   --key NAME@F        press a key at UI frame F: up down pgup pgdn home
 *                       end enter shift-enter esc f12 backspace ctrl-o
 *   --chars TEXT@F      type TEXT at UI frame F
 *   --browse            open the file browser
 *   --diagnose PICTURE  diagnose a screenshot (as if dropped on the window)
 *   --probe             open "Diagnose by probing"
 *   --browse-atlas[=N]  open "Browse the atlas" (its Nth picture picked)
 *   --ui-stats          print the interface's frame times every second
 *
 * Fault SPEC examples: U17:removed  U17.18:gnd  U21.14:vcc  U9.2:open
 *                      CN6.B:open  R51:open  RP4:short  RESET:pressed
 *                      U13.11-12:short (solder bridge between two neighbor legs)
 *                      U18.15:out0 (an output dead inside the chip: out0/out1)
 *                      U21:data0=0  U4:addr12  U4:flip=047E^01  U21:flip@05F4
 *                      U21:weak (inside a ROM or RAM chip, see board.h)
 *
 *   --build-atlas=quick|full [--atlas FILE] [--threads N] [--carts DIR]
 *       Run a catalog of faults on every board, booting normally (no
 *       diagnostic cartridge), on N worker threads (default: every logical
 *       processor), into the fault atlas (default fault_atlas.bin; see
 *       atlas.h). Stopped and run again, it carries on where it was.
 *       Both tiers include solder bridges between neighbor legs: quick on
 *       the CPU, PLA/ULA, RAMs, ROMs and multiplexers, full on every chip.
 *   --atlas-report [--atlas FILE] [--atlas-png DIR]
 *       List the atlas: every outcome and the faults that give it; with
 *       --atlas-png, write the thumbnails as PNGs too.
 *   --atlas-diff OTHER [--atlas FILE]
 *       The faults whose outcome differs between two atlases.
 *   --match PICTURE [--top N] [--causes N|all] [--machine=c64|zx48|zx48i2|...]
 *           [--cart=normal|diag|any] [--match-debug DIR] [--atlas FILE]
 *       Match a picture of a real broken machine (a capture card screenshot:
 *       PNG, BMP, JPG, TGA, GIF) against the atlas: the N groups of faults
 *       that best give that picture, with their confidence and causes.
 *       Only the normal boot groups unless --cart says otherwise.
 *       --match-debug writes the picture as normalized to the machine.
 *   --match-test [N] [--match-dir DIR] [--seed S]
 *       The synthetic capture card test: N (20) different screens of the
 *       atlas made to look like a bad composite capture, then matched.
 *   --probe-test [N] [--seed S] [--probe-noise PCT] [--atlas FILE]
 *       "Diagnose by probing" tested: N (20) faults of the atlas at random
 *       on each machine, put on a board and probed where the tool says,
 *       PCT% of the readings taken wrong on purpose (see probe_db.h). */
#include "sokol_headers.h"
#include "atlas.h"
#include "probe_db.h"
#include "board.h"
#include "c64.h"
#include "zx48.h"
#include "png.h"
#include "diagnose.h"
#include "healthy.h"
#include "probe.h"
#include "ui.h"
#include "win_perf.h"
#include "icon_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef _WIN32
#include <mmsystem.h>
#endif

static struct {
    const char *prg;
    const char *crt;
    bool harness, flashes;
    int bank;                   /* --bank: the switch of a ROM of several banks */
    int socket;                 /* --socket: 1 U4, 0 the port, -1 as detected */
    const char *shot;
    const char *screenshot;
    int headless_frames;
    int frames;
    const char *faults[64];
    int nfaults;
    const char *scope;
    bool text, nets;
    int fault_after;        /* apply the faults after this many frames */
    struct { int frame; const char *spec; } at[64];   /* --at FRAME:SPEC */
    int nat;
    const char *record;     /* --record N:DIR */
    int record_from;
    const char *inspect;
    const char *load_state;
    const char *continuity; /* --continuity REF.PIN */
    bool view, mouse;
    float view_x, view_y, view_zoom, mouse_x, mouse_y;
    struct { int frame; bool down; float x, y; } clicks[16];
    int nclicks;
    struct { int frame; float x, y, dy; } wheels[16];
    int nwheels;
    struct { int frame; char what[1024]; bool chars; size_t sent; } keys[16];
    int nkeys;
    bool browse;
    const machine_t *machine;
    const char *options[8]; int noptions;   /* --option ID=VALUE */
    /* The fault atlas (atlas.h). */
    const char *atlas_tier, *atlas_path, *atlas_png, *atlas_diff, *carts;
    bool atlas_report;
    int threads;
    /* Matching a picture against it (atlas_match.c). */
    const char *match, *match_debug, *match_dir, *diagnose;
    const char *diagnose_text, *add_repair, *chips, *notes, *owner, *repairs;
    const char *merge_repairs, *export_repairs;
    bool list_repairs, export_all, reread_repairs, repair_gaps;
    const char *healthy_sheet;  /* --healthy-sheet DIR */
    int certainty;
    bool browse_atlas;
    int browse_pick;
    int match_test, top, causes, match_machine, match_cart;
    int probe_test, probe_noise;      /* probe_db.h */
    bool probe;
    unsigned seed;
} args = { .frames = 60, .atlas_path = "fault_atlas.bin", .carts = "carts", .causes = 8, .match_machine = -1,
            .match_cart = 0, .match_dir = "match_test", .seed = 1, .socket = -1, .certainty = 1 };

static board_t *board;

/* The machines, for --machine. */
static const machine_t *const machines[] = { &machine_c64, &machine_c64c, &machine_zx48, &machine_zx48i2,
                                             &machine_zx48i1, &machine_zx48i4b, &machine_zx48i6a };

static const machine_t *machine_named(const char *id) {
    for (size_t k = 0; k < sizeof(machines) / sizeof(machines[0]); k++)
        if (!strcmp(machines[k]->id, id)) return machines[k];
    return NULL;
}

/* --option ID=VALUE on the board (false if the board has no such choice). */
static bool apply_option(board_t *b, const char *spec) {
    const machine_t *m = b->m;
    size_t n = strcspn(spec, "=");
    for (int o = 0; spec[n] && o < m->noptions; o++) {
        if (strlen(m->options[o].id) != n || strncmp(m->options[o].id, spec, n)) continue;
        for (int v = 0; v < m->options[o].nvalues; v++)
            if (!strcasecmp(m->options[o].value_ids[v], spec + n + 1)) {
                m->set_option(b, o, v);
                return true;
            }
    }
    return false;
}

static void apply_faults(void) {
    for (int i = 0; i < args.nfaults; i++)
        if (!board_apply_fault_spec(board, args.faults[i]))
            fprintf(stderr, "bad fault spec: %s\n", args.faults[i]);
}

/* A raw .rom/.bin image for the Spectrum: banks of 16K without the marks
 * of a C64 image (CBM80, 6502 vectors). */
static bool raw_rom_is_zx(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t cap = 1024 * 1024 + 3;
    uint8_t *buf = malloc(cap);
    size_t n = buf ? fread(buf, 1, cap, f) : 0;
    fclose(f);
    bool zx = n && n < cap && n % 16384 == 0 && !rom_image_is_c64(buf, n);
    free(buf);
    return zx;
}

/* Plug the cartridge given on the command line. */
static void insert_cart(void) {
    char msg[256];
    if (args.harness) board_set_harness(board, true);
    if (!args.crt) return;
    if (!board_load(board, args.crt, false, msg, sizeof(msg))) { fprintf(stderr, "%s\n", msg); return; }
    if (args.bank > 0 && args.bank < board_cart_banks(board)) {
        board_set_cart_bank(board, args.bank);
        snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), ", switched to bank %d", args.bank);
    }
    if (args.socket >= 0 && board_is_c64(board) && c64_cart_socket(board) >= 0 &&
        c64_cart_socket(board) != args.socket) {
        c64_set_cart_socket(board, args.socket);
        snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), ", moved to %s", args.socket ? "U4" : "the port");
    }
    printf("%s\n", msg);
}

/* --flashes: Dead Test reports a bad RAM chip by flashing the whole
 * screen N times, then a pause, then again. A lit screen for less than a
 * second is a flash; a dark pause longer than 0.8 s starts a new series. */
static struct { bool lit; int lit_since, dark_since, cur, n, series[64]; } F;

static bool screen_lit(void) {
    int w = board->m->screen_w, h = board->m->screen_h;
    uint32_t *px = malloc((size_t)w * h * sizeof(uint32_t));
    board_monitor(board, px);
    uint32_t c = px[(h / 2) * w + w / 2];
    free(px);
    return ((c & 0xFF) * 299 + ((c >> 8) & 0xFF) * 587 + ((c >> 16) & 0xFF) * 114) / 1000 > 128;
}

static void flash_frame(int f) {
    bool lit = screen_lit();
    if (lit && !F.lit) F.lit_since = f;
    if (!lit && F.lit) {
        if (f - F.lit_since < 50) {
            if (F.cur && F.lit_since - F.dark_since > 40 && F.n < 64) { F.series[F.n++] = F.cur; F.cur = 0; }
            F.cur++;
        }
        F.dark_since = f;
    }
    F.lit = lit;
}

static void flash_report(void) {
    if (F.cur && F.n < 64) F.series[F.n++] = F.cur;
    printf("flash series:");
    for (int i = 0; i < F.n; i++) printf(" %d", F.series[i]);
    printf("%s\nscreen at the end: %s\n", F.n ? "" : " none", screen_lit() ? "lit" : "dark");
}

static int run_headless(void) {
    char msg[256];
    insert_cart();
    if (!args.fault_after) apply_faults();
    clock_t t0 = clock();
    const char *type = getenv("C64HW_TYPE");     /* text to type after boot */
    int typed = 0, next_type = 0;
    const machine_t *m = board->m;
    int ready = (int)(m->ready_cycles / (uint64_t)m->cycles_per_frame);
    board_state_t *saved = NULL;
    for (int f = 0; f < args.headless_frames; f++) {
        if (args.fault_after && f == args.fault_after) apply_faults();
        for (int i = 0; i < args.nat; i++) {        /* scheduled faults/repairs */
            if (args.at[i].frame != f) continue;
            if (!strcmp(args.at[i].spec, "repair")) board_clear_faults(board);
            else if (!strcmp(args.at[i].spec, "save")) { board_free_state(saved); saved = board_save_state(board); }
            else if (!strncmp(args.at[i].spec, "save:", 5)) {
                char err[256];
                if (board_save_file(board, args.at[i].spec + 5, "", err, sizeof(err)))
                    printf("saved to %s at frame %d\n", args.at[i].spec + 5, f);
                else fprintf(stderr, "can't save %s: %s\n", args.at[i].spec + 5, err);
            }
            else if (!strcmp(args.at[i].spec, "load")) {
                if (!board_load_state(board, saved)) fprintf(stderr, "no save state to load at frame %d\n", f);
            }
            else if (!strncmp(args.at[i].spec, "load:", 5)) {
                board_load(board, args.at[i].spec + 5, true, msg, sizeof(msg));
                printf("frame %d: %s\n", f, msg);
            }
            else if (!board_apply_fault_spec(board, args.at[i].spec))
                fprintf(stderr, "bad fault spec: %s\n", args.at[i].spec);
        }
        board_run(board, m->cycles_per_frame);
        board_frame_update(board, 20000);
        if (args.flashes) flash_frame(f);
        /* Once BASIC is ready: load the program and type. */
        if (args.prg && f == ready) {
            board_load(board, args.prg, true, msg, sizeof(msg));
            printf("%s\n", msg);
        }
        /* A key every 4 frames; the same key again after 12, as the
         * Spectrum's ROM takes a key pressed again within 5 frames of its
         * release for the same key held down. */
        if (type && f >= ready + 10 && f >= next_type && f % 4 == 0 && type[typed]) {
            int c = type[typed++];
            next_type = f + (type[typed] == c ? 12 : 4);
            if (c == '\n') c = m->host_key(SAPP_KEYCODE_ENTER);
            else if (c >= 'a' && c <= 'z') c = m->host_char(c);
            board_key_down(board, c);
            board_key_up(board, c);
        }
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    double emulated = args.headless_frames * (double)m->cycles_per_frame / m->cpu_hz;
    printf("emulated %.2f s in %.2f s (%.0f%% of real time)\n", emulated, secs, 100.0 * emulated / secs);
    char regs[128];
    m->cpu_regs(board, regs, sizeof(regs));
    printf("CPU: %s\n", regs);
    if (board->check) printf("self-check errors: %llu\n", (unsigned long long)board->check_errors);
    board_free_state(saved);
    if (args.flashes) flash_report();
    if (args.text) m->print_text(board);
    if (args.nets) {    /* nets that spent time in contention, and who fights */
        sim_t *s = &board->sim;
        for (int i = 0; i < s->nnets; i++) {
            uint32_t tot = board->st_hi[i] + board->st_lo[i] + board->st_float[i] + board->st_fight[i];
            if (!board->st_fight[i] || s->nets[i].root != i) continue;
            printf("contention %3u%% on net %s:", (unsigned)(100ull * board->st_fight[i] / tot), s->nets[i].name);
            const net_t *n = &s->nets[i];
            for (int k = 0; k < n->npins; k++) {
                const pin_t *pp = &s->pins[n->pins[k]];
                const char *pname = sim_pin_name(s, n->pins[k]);
                if (pp->eff) printf(" %s.%d(%s)=%s%d", s->parts[pp->part].ref, pp->number, pname ? pname : "",
                                    (pp->eff >> 1) == S_SUPPLY ? "supply" : (pp->eff >> 1) == S_STRONG ? "" :
                                    (pp->eff >> 1) == S_RES ? "res" : "weak",
                                    pp->eff & 1);
            }
            printf("\n");
        }
    }
    if (args.shot) {
        uint32_t *pixels = malloc((size_t)m->screen_w * m->screen_h * sizeof(uint32_t));
        board_monitor(board, pixels);
        png_write(args.shot, pixels, m->screen_w, m->screen_h);
        free(pixels);
        printf("screen saved to %s\n", args.shot);
    }
    return 0;
}

static void init(void) {
    sg_setup(&(sg_desc){ .environment = sglue_environment(), .logger.func = slog_func });
    stm_setup();
    snk_setup(&(snk_desc_t){ .dpi_scale = sapp_dpi_scale(), .max_vertices = 400000,
                             .logger.func = slog_func });
    insert_cart();
    apply_faults();
    diagnose_init(args.atlas_path, args.carts);
    diagnose_repairs_file(args.repairs);
    ui_init(board, args.prg, args.scope);
    if (args.inspect) ui_inspect(args.inspect);
    if (args.continuity) ui_continuity(args.continuity);
    if (args.load_state) ui_load_state(args.load_state);
    if (args.browse) ui_browse();
    if (args.probe) probe_open();
    if (args.diagnose) ui_diagnose(args.diagnose);     /* after: probing takes what it shows */
    if (args.browse_atlas) ui_browse_atlas(args.browse_pick);
    if (args.view) ui_view(args.view_x, args.view_y, args.view_zoom);
}

/* A synthetic mouse event, as if from the window (tests, screenshots). */
static void synth_mouse_event(sapp_event_type type, float x, float y) {
    sapp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.mouse_button = SAPP_MOUSEBUTTON_LEFT;
    ev.mouse_x = x;
    ev.mouse_y = y;
    ui_event(&ev);
}

static void frame(void) {
    static int count, recorded, next_record = -1, ui_frames;
    static bool done[64];
    if (args.mouse) synth_mouse_event(SAPP_EVENTTYPE_MOUSE_MOVE, args.mouse_x, args.mouse_y);
    for (int i = 0; i < args.nclicks; i++) {
        if (args.clicks[i].frame != ui_frames) continue;
        synth_mouse_event(SAPP_EVENTTYPE_MOUSE_MOVE, args.clicks[i].x, args.clicks[i].y);
        synth_mouse_event(args.clicks[i].down ? SAPP_EVENTTYPE_MOUSE_DOWN : SAPP_EVENTTYPE_MOUSE_UP,
                    args.clicks[i].x, args.clicks[i].y);
    }
    for (int i = 0; i < args.nwheels; i++) {
        if (args.wheels[i].frame != ui_frames) continue;
        synth_mouse_event(SAPP_EVENTTYPE_MOUSE_MOVE, args.wheels[i].x, args.wheels[i].y);
        sapp_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = SAPP_EVENTTYPE_MOUSE_SCROLL;
        ev.mouse_x = args.wheels[i].x;
        ev.mouse_y = args.wheels[i].y;
        ev.scroll_y = args.wheels[i].dy;
        ui_event(&ev);
    }
    for (int i = 0; i < args.nkeys; i++) {
        sapp_event ev;
        memset(&ev, 0, sizeof(ev));
        if (args.keys[i].chars) {
            /* nuklear takes 16 bytes of text a frame: the rest goes in the
             * next frames, as if typed. */
            const char *what = args.keys[i].what;
            if (args.keys[i].frame > ui_frames || !what[args.keys[i].sent]) continue;
            for (int n = 0; n < 8 && what[args.keys[i].sent]; n++) {
                ev.type = SAPP_EVENTTYPE_CHAR;
                ev.char_code = (uint32_t)what[args.keys[i].sent++];
                ui_event(&ev);
            }
            continue;
        }
        if (args.keys[i].frame != ui_frames) continue;
        static const struct { const char *name; sapp_keycode code; uint32_t mod; } names[] = {
            { "up", SAPP_KEYCODE_UP, 0 }, { "down", SAPP_KEYCODE_DOWN, 0 },
            { "pgup", SAPP_KEYCODE_PAGE_UP, 0 }, { "pgdn", SAPP_KEYCODE_PAGE_DOWN, 0 },
            { "home", SAPP_KEYCODE_HOME, 0 }, { "end", SAPP_KEYCODE_END, 0 },
            { "enter", SAPP_KEYCODE_ENTER, 0 }, { "shift-enter", SAPP_KEYCODE_ENTER, SAPP_MODIFIER_SHIFT },
            { "esc", SAPP_KEYCODE_ESCAPE, 0 }, { "f12", SAPP_KEYCODE_F12, 0 }, { "backspace", SAPP_KEYCODE_BACKSPACE, 0 },
            { "ctrl-o", SAPP_KEYCODE_O, SAPP_MODIFIER_CTRL },
        };
        for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
            if (strcmp(names[k].name, args.keys[i].what)) continue;
            ev.key_code = names[k].code;
            ev.modifiers = names[k].mod;
            ev.type = SAPP_EVENTTYPE_KEY_DOWN;
            ui_event(&ev);
            ev.type = SAPP_EVENTTYPE_KEY_UP;
            ui_event(&ev);
        }
    }
    ui_frames++;
    ui_frame();
    int f = ui_emulated_frame();
    for (int i = 0; i < args.nat; i++)          /* scheduled faults/repairs */
        if (!done[i] && f >= args.at[i].frame) { ui_apply_fault(args.at[i].spec); done[i] = true; }
    if (args.screenshot && ++count >= args.frames) {
        ui_save_screenshot(args.screenshot);
        sapp_request_quit();
    }
    if (args.record && f >= args.record_from && f >= next_record) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%05d.png", args.record, recorded++);
        ui_save_screenshot(path);
        next_record = (next_record < 0 ? f : next_record) + 4;
        if (recorded >= args.frames) sapp_request_quit();
    }
}

static void cleanup(void) {
    ui_shutdown();
    snk_shutdown();
    sg_shutdown();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* Linked -mwindows, so a double-click opens no console window. Run
     * from a command prompt, borrow that prompt's console for the
     * --headless, --atlas-report, --match... output; a redirect (> file, a pipe)
     * already gave us real handles, keep those. */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
        if (!out || out == INVALID_HANDLE_VALUE) freopen("CONOUT$", "w", stdout);
        if (!err || err == INVALID_HANDLE_VALUE) freopen("CONOUT$", "w", stderr);
    }
#endif
    bool atlas = false;
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--build-atlas", 13) || !strncmp(argv[i], "--atlas-", 8) || !strncmp(argv[i], "--match", 7))
            atlas = true;
#ifdef _WIN32
    /* The atlas builder's workers must spread over every logical
     * processor, E-cores included: no P-core steering, no high priority
     * class (28 busy threads would starve the desktop), just no EcoQoS
     * throttling, so a worker on an E-core runs at its full speed. */
    if (atlas) win_unthrottle_process();
    else {
    /* Keep the whole process (this thread, the simulation thread spun up
     * by ui_init()) off the Efficiency cores and off a coarse system
     * timer: see win_perf.h. timeBeginPeriod(1) also tightens Sleep()
     * (and the usleep() the simulation thread paces itself with) from
     * Windows' default ~15.6 ms tick down to ~1 ms, which matters once a
     * video frame is a few ms (120/144 Hz monitors). */
    win_boost_process();
    win_boost_current_thread();
    timeBeginPeriod(1);
    }
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless") && i + 1 < argc) args.headless_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) args.shot = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) args.screenshot = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) args.frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scope") && i + 1 < argc) args.scope = argv[++i];
        else if (!strcmp(argv[i], "--continuity") && i + 1 < argc) args.continuity = argv[++i];
        else if (!strcmp(argv[i], "--fault-after") && i + 1 < argc) args.fault_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) args.load_state = argv[++i];
        else if (!strcmp(argv[i], "--at") && i + 1 < argc && args.nat < 64) {
            const char *a = argv[++i], *colon = strchr(a, ':');
            if (colon) { args.at[args.nat].frame = atoi(a); args.at[args.nat++].spec = colon + 1; }
        }
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            const char *a = argv[++i], *colon = strchr(a, ':');
            args.record_from = colon ? atoi(a) : 0;
            args.record = colon ? colon + 1 : a;
        }
        else if (!strcmp(argv[i], "--inspect") && i + 1 < argc) args.inspect = argv[++i];
        else if (!strcmp(argv[i], "--view") && i + 1 < argc)
            args.view = sscanf(argv[++i], "%f,%f,%f", &args.view_x, &args.view_y, &args.view_zoom) == 3;
        else if (!strcmp(argv[i], "--mouse") && i + 1 < argc)
            args.mouse = sscanf(argv[++i], "%f,%f", &args.mouse_x, &args.mouse_y) == 2;
        else if ((!strcmp(argv[i], "--down") || !strcmp(argv[i], "--up")) && i + 1 < argc && args.nclicks < 16) {
            bool down = !strcmp(argv[i], "--down");
            if (sscanf(argv[++i], "%f,%f@%d", &args.clicks[args.nclicks].x, &args.clicks[args.nclicks].y,
                       &args.clicks[args.nclicks].frame) == 3)
                args.clicks[args.nclicks++].down = down;
        }
        else if (!strcmp(argv[i], "--wheel") && i + 1 < argc && args.nwheels < 16) {
            if (sscanf(argv[++i], "%f,%f,%f@%d", &args.wheels[args.nwheels].x, &args.wheels[args.nwheels].y,
                       &args.wheels[args.nwheels].dy, &args.wheels[args.nwheels].frame) == 4)
                args.nwheels++;
        }
        else if (!strcmp(argv[i], "--harness")) args.harness = true;
        else if (!strcmp(argv[i], "--bank") && i + 1 < argc) args.bank = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc) args.socket = !strcmp(argv[++i], "u4");
        else if (!strcmp(argv[i], "--browse")) args.browse = true;
        else if ((!strcmp(argv[i], "--key") || !strcmp(argv[i], "--chars")) && i + 1 < argc && args.nkeys < 16) {
            bool chars = !strcmp(argv[i], "--chars");
            const char *a = argv[++i], *at = strrchr(a, '@');
            if (at) {
                snprintf(args.keys[args.nkeys].what, sizeof(args.keys[0].what), "%.*s", (int)(at - a), a);
                args.keys[args.nkeys].frame = atoi(at + 1);
                args.keys[args.nkeys++].chars = chars;
            }
        }
        else if (!strcmp(argv[i], "--machine") && i + 1 < argc) {
            const char *id = argv[++i];
            args.machine = machine_named(id);
            if (!args.machine) { fprintf(stderr, "unknown machine %s\n", id); return 1; }
        }
        else if (!strcmp(argv[i], "--board") && i + 1 < argc) {
            const char *id = argv[++i];
            args.machine = !strcmp(id, "250407") ? &machine_c64 : !strcmp(id, "250469") ? &machine_c64c :
                           !strcasecmp(id, "issue2") ? &machine_zx48i2 : !strcasecmp(id, "issue3b") ? &machine_zx48 :
                           !strcasecmp(id, "issue1") ? &machine_zx48i1 : !strcasecmp(id, "issue4b") ? &machine_zx48i4b :
                           !strcasecmp(id, "issue6a") ? &machine_zx48i6a : NULL;
            if (!args.machine) {
                fprintf(stderr, "unknown board %s (250407, 250469, issue3b, issue4b, issue6a, issue2, issue1)\n", id);
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--option") && i + 1 < argc && args.noptions < 8) args.options[args.noptions++] = argv[++i];
        else if (!strcmp(argv[i], "--build-atlas")) args.atlas_tier = "quick";
        else if (!strncmp(argv[i], "--build-atlas=", 14)) args.atlas_tier = argv[i] + 14;
        else if (!strcmp(argv[i], "--atlas") && i + 1 < argc) args.atlas_path = argv[++i];
        else if (!strcmp(argv[i], "--atlas-report")) args.atlas_report = true;
        else if (!strcmp(argv[i], "--atlas-png") && i + 1 < argc) args.atlas_png = argv[++i];
        else if (!strcmp(argv[i], "--atlas-diff") && i + 1 < argc) args.atlas_diff = argv[++i];
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) args.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--match") && i + 1 < argc) args.match = argv[++i];
        else if (!strcmp(argv[i], "--diagnose") && i + 1 < argc) args.diagnose = argv[++i];
        else if (!strcmp(argv[i], "--diagnose-text") && i + 1 < argc) args.diagnose_text = argv[++i];
        else if (!strcmp(argv[i], "--add-repair") && i + 1 < argc) args.add_repair = argv[++i];
        else if (!strcmp(argv[i], "--chips") && i + 1 < argc) args.chips = argv[++i];
        else if (!strcmp(argv[i], "--notes") && i + 1 < argc) args.notes = argv[++i];
        else if (!strcmp(argv[i], "--owner") && i + 1 < argc) args.owner = argv[++i];
        else if (!strcmp(argv[i], "--repairs") && i + 1 < argc) args.repairs = argv[++i];
        else if (!strcmp(argv[i], "--merge-repairs") && i + 1 < argc) args.merge_repairs = argv[++i];
        else if (!strcmp(argv[i], "--export-repairs") && i + 1 < argc) args.export_repairs = argv[++i];
        else if (!strcmp(argv[i], "--all")) args.export_all = true;
        else if (!strcmp(argv[i], "--list-repairs")) args.list_repairs = true;
        else if (!strcmp(argv[i], "--reread-repairs")) args.reread_repairs = true;
        else if (!strcmp(argv[i], "--repair-gaps")) args.repair_gaps = true;
        else if (!strcmp(argv[i], "--healthy-sheet") && i + 1 < argc) args.healthy_sheet = argv[++i];
        else if (!strcmp(argv[i], "--certainty") && i + 1 < argc) args.certainty = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ui-stats")) ui_print_stats(true);
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) {
            if (!ui_set_theme(argv[++i])) {
                fprintf(stderr, "--theme %s: the choices are light, dark and system\n", argv[i]);
                return 1;
            }
        }
        else if (!strncmp(argv[i], "--browse-atlas", 14)) {
            args.browse_atlas = true;
            args.browse_pick = argv[i][14] == '=' ? atoi(argv[i] + 15) : -1;
        }
        else if (!strcmp(argv[i], "--match-test")) {
            args.match_test = 20;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') args.match_test = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--probe-test")) {
            args.probe_test = 20;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') args.probe_test = atoi(argv[++i]);
        }
        else if (!strcmp(argv[i], "--probe")) args.probe = true;
        else if (!strcmp(argv[i], "--probe-noise") && i + 1 < argc) args.probe_noise = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--match-dir") && i + 1 < argc) args.match_dir = argv[++i];
        else if (!strcmp(argv[i], "--match-debug") && i + 1 < argc) args.match_debug = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) args.seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--top") && i + 1 < argc) args.top = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--causes") && i + 1 < argc) {
            const char *a = argv[++i];
            args.causes = !strcmp(a, "all") ? -1 : atoi(a);
        }
        else if (!strncmp(argv[i], "--machine=", 10) || !strncmp(argv[i], "--cart=", 7)) {
            bool mach = argv[i][2] == 'm';
            const char *v = strchr(argv[i], '=') + 1;
            int *dst = mach ? &args.match_machine : &args.match_cart;
            *dst = mach ? (machine_named(v) ? atlas_machine_of(machine_named(v)) : -2)
                        : (!strcmp(v, "normal") ? 0 : !strcmp(v, "diag") ? 1 : !strcmp(v, "any") ? -1 : -2);
            if (*dst == -2) {
                fprintf(stderr, "%s: %s\n", argv[i], mach ? "the machines are c64, c64c, zx48, zx48i2, zx48i1, zx48i4b and zx48i6a" : "the choices are normal, diag and any");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--carts") && i + 1 < argc) args.carts = argv[++i];
        else if (!strcmp(argv[i], "--flashes")) args.flashes = true;
        else if (!strcmp(argv[i], "--text")) args.text = true;
        else if (!strcmp(argv[i], "--nets")) args.nets = true;
        else if (!strcmp(argv[i], "--fault") && i + 1 < argc && args.nfaults < 64) args.faults[args.nfaults++] = argv[++i];
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
        else {
            /* Cartridges go in before power on, the rest is loaded once the
             * machine is ready. Spectrum files pick the Spectrum; a raw ROM
             * image the machine it is made for. */
            size_t n = strlen(argv[i]);
            const char *ext = n > 4 ? argv[i] + n - 4 : "";
            bool raw = !strcasecmp(ext, ".rom") || !strcasecmp(ext, ".bin");
            if (!strcasecmp(ext, ".crt") || raw) args.crt = argv[i];
            else args.prg = argv[i];
            if (!args.machine && raw && raw_rom_is_zx(argv[i])) args.machine = &machine_zx48;
            if (!args.machine && (!strcasecmp(ext, ".sna") || !strcasecmp(ext, ".z80")))
                args.machine = &machine_zx48;
        }
    }
    if (args.atlas_tier)
        return atlas_build(&(atlas_build_t){ .tier = args.atlas_tier, .path = args.atlas_path,
                                             .carts = args.carts, .threads = args.threads });
    if (args.atlas_diff) return atlas_diff(args.atlas_path, args.atlas_diff);
    if (args.probe_test) {
        atlas_init_shared();
        return probe_test(args.atlas_path, args.probe_test, args.seed, args.probe_noise);
    }
    if (args.match || args.match_test) {
        atlas_match_opts_t mo = { .path = args.atlas_path, .image = args.match, .top = args.top, .causes = args.causes,
                                  .machine = args.match_machine, .cart = args.match_cart,
                                  .debug_dir = args.match_debug, .carts = args.carts };
        if (args.machine && mo.machine < 0) mo.machine = atlas_machine_of(args.machine);
        return args.match_test ? atlas_match_test(&mo, args.match_test, args.match_dir, args.seed) : atlas_match(&mo);
    }
    if (args.atlas_report || args.atlas_png) return atlas_report(args.atlas_path, args.atlas_png);
    if (args.healthy_sheet) return healthy_cli(args.healthy_sheet);
    if (args.merge_repairs || args.export_repairs || args.list_repairs || args.reread_repairs || args.repair_gaps) {
        atlas_init_shared();
        diagnose_init(args.atlas_path, args.carts);
        diagnose_repairs_file(args.repairs);
        return diagnose_cli_records(args.list_repairs ? "" : NULL, args.merge_repairs, args.export_repairs,
                                    args.export_all, args.reread_repairs, args.repair_gaps);
    }
    if (args.diagnose_text || args.add_repair) {
        if (args.add_repair && !args.chips) { fprintf(stderr, "--add-repair needs --chips REF,REF...\n"); return 1; }
        atlas_init_shared();
        diagnose_init(args.atlas_path, args.carts);
        diagnose_repairs_file(args.repairs);
        return diagnose_cli(args.add_repair ? args.add_repair : args.diagnose_text,
                            atlas_machine_of(args.machine ? args.machine : &machine_c64),
                            args.add_repair ? args.chips : NULL, args.certainty, args.notes, args.owner);
    }
    (void)atlas;
    board = board_create(args.machine ? args.machine : &machine_c64);
    for (int i = 0; i < args.noptions; i++)
        if (!apply_option(board, args.options[i])) {
            fprintf(stderr, "%s has no option %s\n", board->m->board_name, args.options[i]);
            return 1;
        }
    if (args.load_state && args.headless_frames > 0) {
        char err[256];
        if (!board_state_file_signed(args.load_state))
            fprintf(stderr, "warning: %s wasn't saved on this PC (or was changed since): loaded as asked, "
                    "but only load files you trust\n", args.load_state);
        board_t *loaded = board_load_file(args.load_state, NULL, 0, err, sizeof(err));
        if (!loaded) {
            fprintf(stderr, "%s: %s\n", args.load_state, err);
            return 1;
        }
        board_destroy(board);
        board = loaded;
    }
    if (args.headless_frames > 0) return run_headless();
    sapp_run(&(sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = ui_event,
        .width = 1600,
        .height = 1000,
        .window_title = "c64-hw-sim - C64 ASSY 250407",
        .enable_dragndrop = true,
        .enable_clipboard = true,     /* paste a path in the file browser */
        .high_dpi = false,
        /* The board icon (the .exe's own is in src/icon.rc), not sokol's. */
        .icon.images = {
            { .width = icon_images[0].w, .height = icon_images[0].h, .pixels = { icon_images[0].rgba, icon_images[0].size } },
            { .width = icon_images[1].w, .height = icon_images[1].h, .pixels = { icon_images[1].rgba, icon_images[1].size } },
            { .width = icon_images[2].w, .height = icon_images[2].h, .pixels = { icon_images[2].rgba, icon_images[2].size } },
        },
        .logger.func = slog_func,
    });
    return 0;
}
