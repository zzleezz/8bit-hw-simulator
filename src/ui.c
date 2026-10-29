/* ui.c - the graphical interface.
 *
 * Left: the motherboard, drawn from the KiCad geometry. Chips are dark
 * boxes with their name, pins are small squares colored like a logic probe
 * would show them (see the legend). Mouse wheel zooms, dragging pans; the
 * more you zoom, the more the chips show: pin numbers, then the gates of
 * the logic chips (drawn by inspect.c) or the pin names. Clicking a pin
 * selects it for the oscilloscope and opens a menu to break it; clicking a
 * chip lets you pull it out of its socket or open the inspector; clicking
 * a resistor or a ferrite bead lets you open or short it.
 *
 * Right: what the monitor shows, the oscilloscope and the controls. */
#include "ui_common.h"
#include "atlas.h"
#include "c64.h"
#include "diagnose.h"
#include "atlas_browse.h"
#include "thumbtex.h"
#include "probe.h"
#include "filedialog.h"
#include "inspect.h"
#include "browser.h"
#include "zx48.h"
#include "png.h"
#include "win_perf.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define GL_SILENCE_DEPRECATION
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#define RIGHT_W   470           /* width of the right column */
#define MON_W     448           /* monitor picture size on screen */
#define MON_H     311
#define SCOPE_H   250

typedef struct { float x0, y0, x1, y1; } box_t;

/* What a continuity test reaches (tier 1: beeps, 2: through a resistor or
 * a diode, 3: broken open, 4: a capacitor, open at DC). A hop is a passive
 * on the way, from its near leg to its far one, and what that reaches: a
 * supply (a badge), a net (for a capacitor: only named, not followed), or
 * nothing new (-1). Wires join pads (tier colored): copper, dashed
 * (to a tier 2 net), or the body of a passive. The readout is words, each
 * placed on a line when drawn; items are pads.
 *
 * Faults are hops too: a solder bridge between two legs (the meter beeps
 * across it), and a pin shorted to GND or +5V (its net beeps to the
 * supply, and the supply to its net). AT is the shorted pin. */
enum { HOP_PASSIVE, HOP_BRIDGE, HOP_SHORT };
typedef struct { int pin; uint8_t tier; } cont_pad_t;
typedef struct {
    int part, leg_in, leg_out, kind; uint8_t tier; bool open; int supply, net;
    uint8_t what; int at;   /* HOP_* */
} cont_hop_t;
typedef struct { int a, b; uint8_t tier; bool dashed, body; } cont_wire_t;
typedef struct { char text[48]; uint8_t tier; bool item; } cont_word_t;
typedef struct {
    uint8_t *tier;          /* per net */
    int *via;               /* per net reached: the hop that reached it, -1 for the net of the pin */
    cont_pad_t *pads; int npads;
    cont_hop_t *hops; int nhops;
    cont_wire_t *wires; int nwires;
    cont_word_t *words; int nwords;
    struct { int line; float x; } *pos;
} cont_t;

static struct {
    board_t *b;
    /* Board view: screen = origin + mm * zoom (origin relative to the view). */
    float zoom, ox, oy;
    bool fitted;
    /* The view was zoomed or panned since the last fit (else a resized
     * window fits it again); the size it was fitted to; the last click on
     * the bare board (a second one soon after, close by: fit again). */
    bool view_moved;
    float fit_w, fit_h;
    double last_click;
    float last_click_x, last_click_y;
    /* The save state slot (NULL: empty), and when it was saved. */
    board_state_t *saved;
    char saved_note[48];
    /* Repair notes, kept in save files; the text box has the keyboard.
     * The save file last saved or loaded, and the dialogs to ask for one
     * (after the frame, as want_pick). */
    char notes[4096];
    bool notes_typing;
    char state_path[4096];
    bool want_save_file, want_open_file;
    bool want_browse;           /* "Browse the atlas...": opened after the frame (the first reads the atlas) */
    char diag_drop[4096];       /* a screenshot dropped on the window, matched after the frame */
    /* Screenshot diagnoses (and re-matches) made, and the one "Diagnose by
     * probing" last took its screen from: opening it takes a newer one. */
    int diag_serial, diag_taken;
    /* The help window. */
    bool help_open;
    bool want_dump;             /* F12: the interface state into ui-debug.log */
    bool want_esc;              /* Esc: close the window on top, in the next frame */
    struct nk_rect help_bounds;
    /* The font turned a quarter turn, for vertical labels: its glyphs, its
     * picture and the size of the unturned one. */
    struct nk_font *vfont;
    nk_handle vfont_tex;
    int vfont_w, vfont_h;
    box_t box[SIM_MAX_PARTS];   /* body of each part, in mm */
    int hover_pin, hover_part;
    bool dragging, drag_moved, drag_beep;  /* beep: the click is a continuity test */
    float drag_x, drag_y;
    /* Fault menu (a small window at the mouse, in screen coordinates). */
    bool menu_open;
    int menu_pin, menu_part, menu_frame;
    bool menu_mem;          /* the memory faults list of a chip menu is open */
    float menu_x, menu_y;
    /* The RESET button kept pressed (the checkbox next to it). */
    bool reset_hold;
    /* Requested board view (--view): center in mm and zoom over the fit. */
    bool view_set;
    float view_cx, view_cy, view_zoom;
    /* Logic probe state of every net, refreshed every frame. */
    uint8_t *net_state;
    /* Monitor. */
    sg_image img;
    sg_view view;
    sg_sampler smp;
    snk_image_t snk_img;
    uint32_t *monitor;          /* screen_w x screen_h of the machine */
    uint64_t monitor_cycle;     /* sim cycle when it was last drawn */
    /* Emulation. */
    bool paused, warp;
    double last_time, speed;
    uint64_t frame;
    char autoload[4096];    /* a program to load once BASIC is ready */
    bool autoload_run;
    char pending_load[4096];    /* a file for the machine switch_to puts on the bench */
    bool pending_run;
    /* Scope. */
    int timebase;
    bool trigger;
    /* What the last program or cartridge load said. */
    char status[512];
    /* A machine chosen in the controls, put on the bench after the frame;
     * the Spectrum and the C64 boards last on the bench (their buttons'). */
    const machine_t *switch_to, *last_zx, *last_c64;
    /* A fault put on the board from a diagnosis: the view goes to its part
     * (once the board is drawn), which is outlined for a while. */
    bool focus_pending;
    int focus_part, focus_pin, focus_pin2;     /* pin2: the other leg of a bridge */
    double focus_until;         /* stm_sec() */
    char focus_note[240];
    /* "Diagnose screenshot..." clicked: the system's dialog, after the frame. */
    bool want_pick;
    bool want_paste;            /* "Paste screenshot" or Ctrl+V: the clipboard's picture, after the frame */
    int want_records_file;      /* a repair records file to pick (diagnose_action_t.records_file) */
    char diag_status[300];      /* what the last diagnosis did */
    /* The machine on the bench changed: the screenshot is matched again,
     * after the frame (its pictures are textures the frame draws). */
    bool diag_rematch;
    /* Continuity test, like a meter's beeper on a trace (shift-click or
     * middle click a pin): the copper net of that pin (-1: none), from the
     * pin clicked (-1: no test), what it reaches, and the faults it was
     * worked out with (see continuity_trace()). */
    int active_continuity_net, cont_pin;
    uint32_t cont_sig;
    cont_t cont;
} U;

bool g_hide_faults;

/* ------------------------------------------------------------------------ */
/* Light or dark                                                             */
/* ------------------------------------------------------------------------ */

enum { THEME_SYSTEM, THEME_LIGHT, THEME_DARK };
static const char *const theme_names[] = { "system", "light", "dark" };
static int theme_choice = THEME_SYSTEM;

static const theme_t theme_dark = {
    false, { 32, 34, 36, 255 }, { 45, 70, 120, 255 }, { 40, 44, 52, 255 }, { 60, 62, 66, 255 } };
static const theme_t theme_light = {
    true, { 214, 216, 220, 255 }, { 175, 200, 240, 255 }, { 222, 226, 232, 255 }, { 218, 220, 225, 255 } };
theme_t g_theme = theme_dark;

/* nuklear's own colors for the light windows (nuklear's defaults are the
 * dark ones). */
static const struct nk_color light_style[NK_COLOR_COUNT] = {
    [NK_COLOR_TEXT] = { 30, 30, 32, 255 },
    [NK_COLOR_WINDOW] = { 238, 239, 241, 255 },
    [NK_COLOR_HEADER] = { 222, 224, 228, 255 },
    [NK_COLOR_BORDER] = { 185, 188, 194, 255 },
    [NK_COLOR_BUTTON] = { 226, 228, 232, 255 },
    [NK_COLOR_BUTTON_HOVER] = { 210, 213, 219, 255 },
    [NK_COLOR_BUTTON_ACTIVE] = { 195, 199, 207, 255 },
    [NK_COLOR_TOGGLE] = { 200, 203, 209, 255 },
    [NK_COLOR_TOGGLE_HOVER] = { 185, 189, 197, 255 },
    [NK_COLOR_TOGGLE_CURSOR] = { 60, 62, 66, 255 },
    [NK_COLOR_SELECT] = { 226, 228, 232, 255 },
    [NK_COLOR_SELECT_ACTIVE] = { 190, 205, 230, 255 },
    [NK_COLOR_SLIDER] = { 215, 218, 223, 255 },
    [NK_COLOR_SLIDER_CURSOR] = { 150, 154, 162, 255 },
    [NK_COLOR_SLIDER_CURSOR_HOVER] = { 130, 134, 142, 255 },
    [NK_COLOR_SLIDER_CURSOR_ACTIVE] = { 110, 114, 122, 255 },
    [NK_COLOR_PROPERTY] = { 226, 228, 232, 255 },
    [NK_COLOR_EDIT] = { 250, 250, 251, 255 },
    [NK_COLOR_EDIT_CURSOR] = { 30, 30, 32, 255 },
    [NK_COLOR_COMBO] = { 226, 228, 232, 255 },
    [NK_COLOR_CHART] = { 215, 218, 223, 255 },
    [NK_COLOR_CHART_COLOR] = { 120, 124, 132, 255 },
    [NK_COLOR_CHART_COLOR_HIGHLIGHT] = { 255, 0, 0, 255 },
    [NK_COLOR_SCROLLBAR] = { 226, 228, 232, 255 },
    [NK_COLOR_SCROLLBAR_CURSOR] = { 175, 179, 187, 255 },
    [NK_COLOR_SCROLLBAR_CURSOR_HOVER] = { 155, 159, 167, 255 },
    [NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = { 135, 139, 147, 255 },
    [NK_COLOR_TAB_HEADER] = { 222, 224, 228, 255 },
    [NK_COLOR_KNOB] = { 215, 218, 223, 255 },
    [NK_COLOR_KNOB_CURSOR] = { 150, 154, 162, 255 },
    [NK_COLOR_KNOB_CURSOR_HOVER] = { 130, 134, 142, 255 },
    [NK_COLOR_KNOB_CURSOR_ACTIVE] = { 110, 114, 122, 255 },
};

bool ui_set_theme(const char *name) {
    for (int i = 0; i < 3; i++)
        if (!strcmp(name, theme_names[i])) { theme_choice = i; return true; }
    return false;
}

/* At the start of a frame: the theme chosen, the system's asked once a
 * second (it may change while running). */
static void apply_theme(struct nk_context *ctx) {
    static double asked;
    static bool system_light, applied, applied_light;
    double now = stm_sec(stm_now());
    if (theme_choice == THEME_SYSTEM && (!asked || now - asked >= 1)) {
        system_light = ui_system_light();
        asked = now;
    }
    bool light = theme_choice == THEME_SYSTEM ? system_light : theme_choice == THEME_LIGHT;
    if (applied && light == applied_light) return;
    g_theme = light ? theme_light : theme_dark;
    nk_style_from_table(ctx, light ? light_style : NULL);
    applied = true;
    applied_light = light;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static void switch_machine(const machine_t *m);
static void attach_board(board_t *b);
static sim_t *S(void) { return &U.b->sim; }
/* The machine on the bench, as the atlas names it: screenshots are only
 * matched against its faults. */
static int bench_machine(void) { return atlas_machine_of(U.b->m); }
static const board_data_t *BD(void) { return U.b->sim.bd; }
static const machine_t *M(void) { return U.b->m; }

/* A pin of a chip or a connector (square pads); the others are the round
 * pads of the passives. Every pad can be clicked alike. */
static bool pin_on_chip(int pin) {
    const part_t *p = &S()->parts[S()->pins[pin].part];
    return p->kind == PART_IC || p->kind == PART_CONNECTOR;
}

/* ------------------------------------------------------------------------ */
/* Board geometry                                                            */
/* ------------------------------------------------------------------------ */

/* The body of a part, from its pads: DIP chips get a body between the two
 * pin rows, everything else a box around the pads. */
static void compute_boxes(void) {
    sim_t *s = S();
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, pad = 0;
        for (int k = 0; k < p->npins; k++) {
            const bd_pad_t *bp = &BD()->pads[p->first_pin + k];
            if (bp->x < x0) x0 = bp->x;
            if (bp->y < y0) y0 = bp->y;
            if (bp->x > x1) x1 = bp->x;
            if (bp->y > y1) y1 = bp->y;
            if (bp->w > pad) pad = bp->w;
            if (bp->h > pad) pad = bp->h;
        }
        if (p->kind == PART_IC) {
            if (x1 - x0 >= y1 - y0) U.box[i] = (box_t){ x0 - 1.4f, y0 + 0.9f, x1 + 1.4f, y1 - 0.9f };
            else U.box[i] = (box_t){ x0 + 0.9f, y0 - 1.4f, x1 - 0.9f, y1 + 1.4f };
        } else {
            float m = pad / 2 + 0.4f;
            U.box[i] = (box_t){ x0 - m, y0 - m, x1 + m, y1 + m };
        }
        /* Big pads at the edge (the Spectrum's TV socket) would push the
         * box off the board: keep it inside. */
        const float *o = BD()->outline, in = 0.4f;
        box_t *bx = &U.box[i];
        if (bx->x0 < o[0] + in) bx->x0 = o[0] + in;
        if (bx->y0 < o[1] + in) bx->y0 = o[1] + in;
        if (bx->x1 > o[2] - in) bx->x1 = o[2] - in;
        if (bx->y1 > o[3] - in) bx->y1 = o[3] - in;
    }
}

static struct nk_rect to_screen(struct nk_rect area, float x0, float y0, float x1, float y1) {
    return nk_rect(area.x + U.ox + x0 * U.zoom, area.y + U.oy + y0 * U.zoom,
                   (x1 - x0) * U.zoom, (y1 - y0) * U.zoom);
}

static void fit_view(struct nk_rect area) {
    float bw = BD()->outline[2] - BD()->outline[0], bh = BD()->outline[3] - BD()->outline[1];
    U.zoom = fminf(area.w / bw, area.h / bh) * 0.98f;
    U.ox = (area.w - bw * U.zoom) / 2 - BD()->outline[0] * U.zoom;
    U.oy = (area.h - bh * U.zoom) / 2 - BD()->outline[1] * U.zoom;
    U.fitted = true;
    U.view_moved = false;
    U.fit_w = area.w;
    U.fit_h = area.h;
}

/* Pin under the mouse (any pad: chips, connectors, passives), or -1. */
static int pick_pin(float mx, float my) {
    sim_t *s = S();
    for (int i = 0; i < s->npins; i++) {
        const bd_pad_t *bp = &BD()->pads[i];
        float hw = fmaxf(bp->w / 2, 0.7f), hh = fmaxf(bp->h / 2, 0.7f);
        if (fabsf(mx - bp->x) <= hw && fabsf(my - bp->y) <= hh) return i;
    }
    return -1;
}

/* Part under the mouse: chips first, then the rest (the smallest box, so a
 * capacitor beside a big part is still found). */
static int pick_part(float mx, float my) {
    sim_t *s = S();
    for (int pass = 0; pass < 2; pass++) {
        int best = -1;
        float best_area = 1e30f;
        for (int i = 0; i < s->nparts; i++) {
            if ((s->parts[i].kind == PART_IC) != (pass == 0)) continue;
            box_t *b = &U.box[i];
            float area = (b->x1 - b->x0) * (b->y1 - b->y0);
            if (mx >= b->x0 && mx <= b->x1 && my >= b->y0 && my <= b->y1 && area < best_area) { best = i; best_area = area; }
        }
        if (best >= 0) return best;
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Board view                                                                */
/* ------------------------------------------------------------------------ */

/* The name of a chip ("RAM1", "PLA", "VRAM"...), colored by role. The
 * chips stand vertical and at normal zoom they are too narrow for a word,
 * so the letters are stacked. When zooming in, the name goes horizontal and
 * the reference, the model and the address range appear under it, as many
 * lines as fit. */
static void draw_chip_name(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect r, int i) {
    part_t *p = &S()->parts[i];
    const char *label = board_part_label(U.b, i);
    struct nk_color tc = p->removed ? COL_SOCKET : role_color[board_part_role(U.b, i)];
    struct nk_color dim = p->removed ? COL_SOCKET : COL_DIMTEXT;
    float fh = ctx->style.font->height;
    int n = (int)strlen(label);

    if (text_width(ctx, label) < r.w - 4) {
        /* Horizontal: collect the lines that fit in the width. */
        const char *lines[8];
        char buf[8][32];
        struct nk_color col[8];
        int nl = 0;
        lines[nl] = label; col[nl++] = tc;
        if (text_width(ctx, p->ref) < r.w - 4) { lines[nl] = p->ref; col[nl++] = dim; }
        /* The info lines. One too wide is wrapped at a space or inside an
         * address range ("$0000-$FFFF" becomes "$0000-" and "$FFFF"); if a
         * piece still doesn't fit, the rest of that line is left out. */
        const char *info = p->unfitted ? "(not fitted)" : p->removed ? "(removed)" : board_part_info(U.b, i);
        while (*info && nl < 8) {
            size_t len = strcspn(info, "\n");
            snprintf(buf[nl], sizeof(buf[nl]), "%.*s", (int)len, info);
            for (int k = (int)strlen(buf[nl]) - 1; k > 0 && text_width(ctx, buf[nl]) >= r.w - 4; k--) {
                if (buf[nl][k] == ' ') { buf[nl][k] = 0; len = (size_t)k + 1; }
                else if (buf[nl][k] == '$' && buf[nl][k - 1] == '-') { buf[nl][k] = 0; len = (size_t)k; }
            }
            if (text_width(ctx, buf[nl]) >= r.w - 4) len = strcspn(info, "\n");
            else { lines[nl] = buf[nl]; col[nl++] = dim; }
            info += len;
            if (*info == '\n') info++;
        }
        /* As many lines as the height allows (at least the name). */
        int fit = (int)((r.h - 4) / fh);
        if (fit < 1) fit = 1;
        if (nl > fit) nl = fit;
        float y = r.y + (r.h - nl * fh) / 2;
        for (int k = 0; k < nl; k++, y += fh)
            text_centered(cv, ctx, nk_rect(r.x, y, r.w, fh), lines[k], col[k]);
    } else if (r.h >= n * fh * 0.9f + 8) {
        /* Vertical: one letter under the other. */
        float step = fh * 0.9f, y = r.y + (r.h - n * step) / 2;
        for (int k = 0; k < n; k++, y += step) {
            char ch[2] = { label[k], 0 };
            text_centered(cv, ctx, nk_rect(r.x, y, r.w, step), ch, tc);
        }
    } else if (text_width(ctx, p->ref) < r.w - 2 && r.h >= fh) {
        text_centered(cv, ctx, r, p->ref, dim);
    }
}

/* Pad squares big enough to write the pin number inside (see draw_pins). */
static bool numbers_in_pads(struct nk_context *ctx) {
    return 1.6f * U.zoom * 0.85f >= text_width(ctx, "40") + 4;
}

/* Inside a chip, depending on the zoom: the name; then also the pin
 * numbers along the edges; zooming in more (numbers move into the pads)
 * the gates of the logic chips, or the pin names of the others, with the
 * name above the chip. */
static void draw_chip_inside(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area,
                             struct nk_rect r, int i) {
    part_t *p = &S()->parts[i];
    float fh = ctx->style.font->height, pitch = 2.54f * U.zoom;
    bool vertical = r.h > r.w;
    if (!vertical || pitch < fh) { draw_chip_name(cv, ctx, r, i); return; }
    /* Where each pin enters the body. */
    pinpos_t pos[PIN_NUMBERS] = {0};
    for (int num = 1; num <= p->npins && num < PIN_NUMBERS; num++) {
        int pin = part_pin(p, num);
        if (pin < 0) continue;
        float px = area.x + U.ox + BD()->pads[pin].x * U.zoom, py = area.y + U.oy + BD()->pads[pin].y * U.zoom;
        int side = px < r.x + r.w / 2 ? -1 : 1;
        pos[num] = (pinpos_t){ side < 0 ? r.x : r.x + r.w, py, side, true };
    }
    char lab[48];
    snprintf(lab, sizeof(lab), "%s %s", p->ref, board_part_label(U.b, i));
    struct nk_color tc = p->removed ? COL_SOCKET : role_color[board_part_role(U.b, i)];
    if (numbers_in_pads(ctx)) {
        if (!p->removed && chip_diagram(cv, ctx, U.b, U.net_state, i, r, pos, pitch)) {
            text_centered(cv, ctx, nk_rect(r.x - 40, r.y - fh - 2, r.w + 80, fh), lab, tc);
            return;
        }
        /* Pin names along the edges, if they fit in half the body. */
        float name_w = 0;
        for (int num = 1; num <= p->npins && num < PIN_NUMBERS; num++) {
            const char *name = pos[num].valid ? sim_pin_name(S(), part_pin(p, num)) : NULL;
            if (name && text_width(ctx, name) > name_w) name_w = text_width(ctx, name);
        }
        if (name_w + 6 < r.w / 2) {
            for (int num = 1; num <= p->npins && num < PIN_NUMBERS; num++) {
                const char *name = pos[num].valid ? sim_pin_name(S(), part_pin(p, num)) : NULL;
                if (name) text_at(cv, ctx, pos[num].x - pos[num].side * 4, pos[num].y, pos[num].side > 0, name, COL_DIMTEXT);
            }
            float center = r.w - 2 * (name_w + 8);
            if (center > 12) draw_chip_name(cv, ctx, nk_rect(r.x + name_w + 8, r.y, center, r.h), i);
            else text_centered(cv, ctx, nk_rect(r.x - 40, r.y - fh - 2, r.w + 80, fh), lab, tc);
            return;
        }
        draw_chip_name(cv, ctx, r, i);
        return;
    }
    /* Pin numbers along the edges, the name in what is left in the middle. */
    float num_w = text_width(ctx, "40") + 4, center = r.w - 2 * num_w;
    if (center < 10) { draw_chip_name(cv, ctx, r, i); return; }
    for (int num = 1; num <= p->npins && num < PIN_NUMBERS; num++) {
        if (!pos[num].valid) continue;
        char txt[8];
        snprintf(txt, sizeof(txt), "%d", num);
        text_at(cv, ctx, pos[num].x - pos[num].side * 2, pos[num].y, pos[num].side > 0, txt, COL_DIMTEXT);
    }
    draw_chip_name(cv, ctx, nk_rect(r.x + num_w, r.y, center, r.h), i);
}

/* A passive broken open (a diagonal) or shorted (a bar across, pad to pad). */
static void body_fault(struct nk_command_buffer *cv, struct nk_rect r, int rfault) {
    if (rfault == RF_OK || g_hide_faults) return;
    nk_stroke_rect(cv, r, 2, 2, COL_FAULT);
    if (rfault == RF_OPEN) nk_stroke_line(cv, r.x, r.y, r.x + r.w, r.y + r.h, 2, COL_FAULT);
    else if (r.w >= r.h) nk_stroke_line(cv, r.x, r.y + r.h / 2, r.x + r.w, r.y + r.h / 2, 3, COL_FAULT);
    else nk_stroke_line(cv, r.x + r.w / 2, r.y, r.x + r.w / 2, r.y + r.h, 3, COL_FAULT);
}

/* Does a rectangle on screen cover one of the part's pads? */
static bool covers_pad(struct nk_rect area, const part_t *p, struct nk_rect t) {
    for (int k = 0; k < p->npins; k++) {
        const bd_pad_t *bp = &BD()->pads[p->first_pin + k];
        float x = area.x + U.ox + bp->x * U.zoom, y = area.y + U.oy + bp->y * U.zoom;
        float hw = fmaxf(bp->w * U.zoom * 0.85f, 2.0f) / 2, hh = fmaxf(bp->h * U.zoom * 0.85f, 2.0f) / 2;
        if (x + hw > t.x && x - hw < t.x + t.w && y + hh > t.y && y - hh < t.y + t.h) return true;
    }
    return false;
}

/* Whether t (on screen) lands on a part other than p: where a name
 * beside a part may go. */
static bool covers_part(struct nk_rect area, const part_t *p, struct nk_rect t) {
    sim_t *s = S();
    for (int i = 0; i < s->nparts; i++) {
        if (&s->parts[i] == p) continue;
        const box_t *b = &U.box[i];
        struct nk_rect r = to_screen(area, b->x0, b->y0, b->x1, b->y1);
        if (r.x + r.w > t.x && r.x < t.x + t.w && r.y + r.h > t.y && r.y < t.y + t.h) return true;
    }
    return false;
}

/* Text read from bottom to top, in r (font height wide, text width high):
 * each letter from the turned font (make_vertical_font). A letter at x
 * along the line and y down from its top lands at r.x + y, bottom - x. */
static void text_vertical(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect r,
                          const char *txt, struct nk_color c) {
    if (!U.vfont) return;
    float scale = ctx->style.font->height / U.vfont->info.height, x = 0, bottom = r.y + r.h;
    for (const char *t = txt; *t; t++) {
        const struct nk_font_glyph *g = nk_font_find_glyph(U.vfont, (nk_rune)(unsigned char)*t);
        /* Where the letter is in the unturned picture, then in the turned one. */
        float u0 = g->u0 * U.vfont_w, u1 = g->u1 * U.vfont_w, v0 = g->v0 * U.vfont_h, v1 = g->v1 * U.vfont_h;
        struct nk_image img = nk_subimage_handle(U.vfont_tex, (nk_ushort)U.vfont_h, (nk_ushort)U.vfont_w,
                                                 nk_rect(v0, U.vfont_w - u1, v1 - v0, u1 - u0));
        nk_draw_image(cv, nk_rect(r.x + g->y0 * scale, bottom - x - g->x1 * scale,
                                  (g->y1 - g->y0) * scale, (g->x1 - g->x0) * scale), &img, c);
        x += g->xadvance * scale;
    }
}

/* The reference of a part without a name of its own (a resistor, a
 * capacitor, a transistor, a jumper): along the part, inside it if it fits
 * between the pads (in the color given), else beside it, dim (above a
 * lying part, right of a standing one, read from the bottom like the
 * silkscreen; a lying part's name goes left, right or below instead if
 * above it lands on another part, a link's left first; a standing part's
 * goes left if right lands on one). Beside it only once zoomed in a little
 * (5 pixels a mm), and only for a part as long as its name: zoomed out,
 * the dense corners would be a pile of text. */
static void draw_ref(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area,
                     struct nk_rect r, const part_t *p, struct nk_color inside) {
    float tw = text_width(ctx, p->ref), fh = ctx->style.font->height;
    bool standing = r.h > r.w;
    float along = standing ? r.h : r.w, across = standing ? r.w : r.h;
    struct nk_rect in = standing ? nk_rect(r.x + (r.w - fh) / 2, r.y + (r.h - tw) / 2, fh, tw)
                                 : nk_rect(r.x + (r.w - tw) / 2, r.y + (r.h - fh) / 2, tw, fh);
    if (along > tw + 4 && across > fh * 0.6f && !covers_pad(area, p, in)) {
        if (standing) text_vertical(cv, ctx, in, p->ref, inside);
        else text_centered(cv, ctx, r, p->ref, inside);
    } else if (U.zoom < 5 || tw > along) {
        return;
    } else if (standing) {
        struct nk_rect right = nk_rect(r.x + r.w + 2, r.y + (r.h - tw) / 2, fh, tw);
        struct nk_rect left = nk_rect(r.x - fh - 2, right.y, fh, tw);
        bool flip = covers_part(area, p, right) && !covers_part(area, p, left);
        text_vertical(cv, ctx, flip ? left : right, p->ref, COL_DIMTEXT);
    } else {
        float y = r.y + (r.h - fh) / 2;
        struct nk_rect at[4] = {
            nk_rect(r.x + (r.w - tw) / 2, r.y - fh - 1, tw, fh),    /* above */
            nk_rect(r.x - tw - 3, y, tw, fh),                       /* left */
            nk_rect(r.x + r.w + 3, y, tw, fh),                      /* right */
            nk_rect(r.x + (r.w - tw) / 2, r.y + r.h + 1, tw, fh),   /* below */
        };
        /* A link's name goes left first: they come in columns (the video links). */
        static const int order[2][4] = { { 0, 1, 2, 3 }, { 1, 0, 2, 3 } };
        bool link = p->kind == PART_LINK || strstr(BD()->comps[p - S()->parts].footprint, "Link");
        const int *o = order[link];
        int k = 0;
        while (k < 4 && covers_part(area, p, at[o[k]])) k++;
        text_centered(cv, ctx, at[o[k < 4 ? k : 0]], p->ref, COL_DIMTEXT);
    }
}

/* A label beside a part: the board's color on the board, readable on the
 * space around it when it is off (above a connector at the edge). */
static void board_label(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area,
                        struct nk_rect r, const char *txt, struct nk_color c) {
    struct nk_rect pcb = to_screen(area, BD()->outline[0], BD()->outline[1], BD()->outline[2], BD()->outline[3]);
    float x = r.x + r.w / 2, y = r.y + r.h / 2;
    bool on = x >= pcb.x && x <= pcb.x + pcb.w && y >= pcb.y && y <= pcb.y + pcb.h;
    text_centered(cv, ctx, r, txt, on ? c : ink(c));
}

static void draw_part(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area, int i) {
    sim_t *s = S();
    part_t *p = &s->parts[i];
    box_t *b = &U.box[i];
    struct nk_rect r = to_screen(area, b->x0, b->y0, b->x1, b->y1);
    bool sim = board_part_simulated(U.b, i);
    switch (p->kind) {
    case PART_IC: {
        if (p->removed) {
            nk_stroke_rect(cv, r, 0, 2, COL_SOCKET);
        } else {
            /* The memories and the PLA get an edge in their role color, so
             * they stand out. */
            int role = board_part_role(U.b, i);
            bool mem = role == ROLE_RAM || role == ROLE_VRAM || role == ROLE_ROM || role == ROLE_PLA;
            nk_fill_rect(cv, r, 2, COL_CHIP);
            nk_stroke_rect(cv, r, 2, mem ? 2 : 1, mem ? role_color[role] : COL_CHIP_EDGE);
        }
        /* Pin 1 marker. */
        int p1 = part_pin(p, 1);
        if (p1 >= 0) {
            float px = area.x + U.ox + BD()->pads[p1].x * U.zoom, py = area.y + U.oy + BD()->pads[p1].y * U.zoom;
            float cx = fminf(fmaxf(px, r.x + 6), r.x + r.w - 6), cy = fminf(fmaxf(py, r.y + 6), r.y + r.h - 6);
            nk_fill_circle(cv, nk_rect(cx - 3, cy - 3, 6, 6), p->removed ? COL_SOCKET : COL_CHIP_EDGE);
        }
        draw_chip_inside(cv, ctx, area, r, i);
        break;
    }
    case PART_RESISTOR:
    case PART_LINK:
        if (!sim) { nk_stroke_rect(cv, r, 2, 1, COL_OTHER); body_fault(cv, r, p->rfault); draw_ref(cv, ctx, area, r, p, COL_DIMTEXT); break; }
        nk_fill_rect(cv, r, 2, p->kind == PART_LINK ? COL_LINK : COL_RES);
        body_fault(cv, r, p->rfault);
        draw_ref(cv, ctx, area, r, p, p->kind == PART_LINK ? COL_TEXT : nk_rgb(40, 30, 20));
        break;
    case PART_CONNECTOR: {
        /* A cartridge in the expansion port: a board under the pins. */
        const char *cart = M()->cart_slot && !strcmp(p->ref, M()->cart_slot) ? board_cart_name(U.b) : NULL;
        if (board_is_c64(U.b) && c64_cart_in_u4(U.b)) cart = NULL;     /* it sits in U4 */
        /* A dongle of the 586220 harness: an outline around the connector. */
        static const char *const harness_cn[] = { "CN1", "CN2", "CN3", "CN4", "CN8", "CN9" };
        bool dongle = false;
        for (size_t k = 0; board_harness(U.b) && k < sizeof(harness_cn) / sizeof(harness_cn[0]); k++)
            if (!strcmp(p->ref, harness_cn[k])) dongle = true;
        if (dongle) nk_stroke_rect(cv, nk_rect(r.x - 4, r.y - 4, r.w + 8, r.h + 8), 3, 2, COL_HARNESS);
        if (cart) {
            nk_fill_rect(cv, nk_rect(r.x - 6, r.y - 6, r.w + 12, r.h + 12), 3, nk_rgb(60, 50, 40));
            nk_stroke_rect(cv, nk_rect(r.x - 6, r.y - 6, r.w + 12, r.h + 12), 3, 2, COL_SOCKET);
        }
        nk_stroke_rect(cv, r, 0, 1, COL_DIMTEXT);
        /* "CN4 SERIAL" if it fits, else just "CN4". */
        char lab[96];
        const char *cname = board_connector_name(U.b, i);
        if (cart) snprintf(lab, sizeof(lab), "%s cartridge: %s", p->ref, cart);
        else snprintf(lab, sizeof(lab), "%s %s%s", p->ref, cname ? cname : "", dongle ? " + harness" : "");
        if (!cart && (!cname || text_width(ctx, lab) > r.w + 30))
            snprintf(lab, sizeof(lab), "%s%s", p->ref, dongle ? " + harness" : "");
        if (cart)       /* always shown: a cartridge is easy to forget */
            board_label(cv, ctx, area, nk_rect(r.x - 100, r.y - 22, r.w + 200, 14), lab, COL_SOCKET);
        else if (dongle)
            board_label(cv, ctx, area, nk_rect(r.x - 60, r.y - 18, r.w + 120, 14), lab, COL_HARNESS);
        else if (r.w + 30 > text_width(ctx, lab))
            board_label(cv, ctx, area, nk_rect(r.x - 15, r.y - 14, r.w + 30, 14), lab, COL_DIMTEXT);
        break;
    }
    default:
        if (sim) nk_fill_rect(cv, r, 2, nk_rgb(170, 170, 180));    /* the crystal */
        else nk_stroke_rect(cv, r, 2, 1, COL_OTHER);
        body_fault(cv, r, p->rfault);
        draw_ref(cv, ctx, area, r, p, sim ? nk_rgb(40, 40, 50) : COL_DIMTEXT);
        break;
    }
}

static bool pin_bridged(int pin) {
    for (int i = 0; i < U.b->nbridges; i++) if (U.b->bridge[i][0] == pin || U.b->bridge[i][1] == pin) return true;
    return false;
}

/* The faults put on a part, for people ("pulled out; pin 15 output dead,
 * stuck low; ...") into out (NULL: just count them), and how many. */
static int part_faults(int part, char *out, int len) {
    sim_t *s = S();
    const part_t *p = &s->parts[part];
    int n = 0;
    if (out) out[0] = 0;
#define ADD(...) do { if (out) { size_t l_ = strlen(out); \
        if (l_ + 3 < (size_t)len) snprintf(out + l_, (size_t)len - l_, "%s", n ? "; " : ""); \
        l_ = strlen(out); snprintf(out + l_, (size_t)len - l_, __VA_ARGS__); } n++; } while (0)
    if (p->removed && !p->unfitted) ADD("pulled out");
    if (p->rfault != RF_OK)
        ADD("%s", p->rfault == RF_SHORT ? "shorted" :
                  !strcmp(board_part_kind_name(U.b, part), "switch") ? "switched off" : "open");
    if (p->mfault != MF_OK) {
        char d[160];
        mem_fault_t mf = { p->mfault, p->mf_bit, p->mf_val, p->mf_addr };
        board_mem_fault_desc(U.b, part, &mf, d, sizeof(d));
        ADD("%s", d);
    }
    for (int k = 0; k < p->npins; k++) {
        const pin_t *pin = &s->pins[p->first_pin + k];
        if (pin->fault != PF_OK) ADD("pin %s %s", BD()->pads[p->first_pin + k].name, pin_fault_name(pin->fault));
    }
    for (int i = 0; i < U.b->nbridges; i++) {
        int pa = U.b->bridge[i][0], pb = U.b->bridge[i][1];
        if (s->pins[pa].part == part)
            ADD("pins %d-%d bridged", s->pins[pa].number, s->pins[pb].number);
    }
#undef ADD
    return n;
}

/* A part with a fault on it: a red outline and a "!" badge in a corner,
 * big enough to see on the whole board. Faults are easy to forget. */
static void fault_badges(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area) {
    if (g_hide_faults) return;
    sim_t *s = S();
    for (int i = 0; i < s->nparts; i++) {
        if (!part_faults(i, NULL, 0)) continue;
        const box_t *b = &U.box[i];
        struct nk_rect r = to_screen(area, b->x0, b->y0, b->x1, b->y1);
        nk_stroke_rect(cv, nk_rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6), 3, 2, COL_FAULT);
        float rad = 8;
        struct nk_rect c = nk_rect(r.x + r.w - rad, r.y - rad, 2 * rad, 2 * rad);
        nk_fill_circle(cv, c, COL_FAULT);
        nk_stroke_circle(cv, c, 1.5f, nk_rgb(0, 0, 0));
        text_centered(cv, ctx, c, "!", nk_rgb(255, 255, 255));
    }
}

/* A solder bridge: a blob of solder from one pad to the other. */
static void draw_bridge(struct nk_command_buffer *cv, struct nk_rect area, int pa, int pb, float width) {
    const bd_pad_t *a = &BD()->pads[pa], *c = &BD()->pads[pb];
    float ax = area.x + U.ox + a->x * U.zoom, ay = area.y + U.oy + a->y * U.zoom;
    float cx = area.x + U.ox + c->x * U.zoom, cy = area.y + U.oy + c->y * U.zoom;
    nk_stroke_line(cv, ax, ay, cx, cy, width + 3, COL_FAULT);
    nk_stroke_line(cv, ax, ay, cx, cy, width, nk_rgb(200, 200, 205));
}

static void draw_pins(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area) {
    sim_t *s = S();
    float ctx_font_h = ctx->style.font->height;
    int hover_root = U.hover_pin >= 0 ? s->pins[U.hover_pin].root : -2;
    for (int i = 0; i < U.b->nbridges; i++) {
        const bd_pad_t *bp = &BD()->pads[U.b->bridge[i][0]];
        draw_bridge(cv, area, U.b->bridge[i][0], U.b->bridge[i][1], fmaxf(fminf(bp->w, bp->h) * U.zoom * 0.45f, 2.0f));
    }
    for (int i = 0; i < s->npins; i++) {
        const bd_pad_t *bp = &BD()->pads[i];
        const pin_t *p = &s->pins[i];
        float sz = fmaxf(fminf(bp->w, bp->h) * U.zoom * 0.85f, 2.0f);
        float x = area.x + U.ox + bp->x * U.zoom, y = area.y + U.oy + bp->y * U.zoom;
        struct nk_rect r = nk_rect(x - sz / 2, y - sz / 2, sz, sz);
        bool chip = pin_on_chip(i), removed = s->parts[p->part].removed;
        struct nk_color c = p->net < 0 ? COL_NONE : state_color(U.net_state[p->net]);
        if (removed) c = nk_rgb(90, 80, 60);
        /* A passive's pad (round): its net's state, plain solder if the net
         * isn't simulated (analog). */
        if (!chip && p->net >= 0 && !board_net_simulated(U.b, p->net)) c = nk_rgb(150, 140, 110);
        nk_fill_rect(cv, r, chip ? 0 : sz / 2, c);
        if (!g_hide_faults && (p->fault != PF_OK || pin_bridged(i))) {
            nk_stroke_rect(cv, nk_rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4), 0, 2, COL_FAULT);
            if (p->fault == PF_OPEN) {
                nk_stroke_line(cv, r.x, r.y, r.x + r.w, r.y + r.h, 1.5f, nk_rgb(0, 0, 0));
                nk_stroke_line(cv, r.x + r.w, r.y, r.x, r.y + r.h, 1.5f, nk_rgb(0, 0, 0));
            }
        }
        /* The pin number (or letter) inside the pad, when it fits. */
        if (!removed && sz >= ctx_font_h - 1 && sz >= text_width(ctx, bp->name) + 3)
            text_centered(cv, ctx, r, bp->name, nk_rgb(0, 0, 0));
        if (p->root == hover_root && p->root >= 0)
            nk_stroke_rect(cv, nk_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2), 0, 1.5f, COL_HOVER);
        if (i == U.b->scope_pin || i == U.b->scope_pin2)
            nk_stroke_rect(cv, nk_rect(r.x - 3, r.y - 3, r.w + 6, r.h + 6), 0, 2, COL_TRACE);
    }
}

/* The pins on the scope: one, or both legs of a solder bridge (pin2 >= 0). */
static void select_pins(int pin, int pin2) {
    U.b->scope_pin = pin;
    U.b->scope_pin2 = pin >= 0 ? pin2 : -1;
    U.b->scope_pos = 0;
    memset(U.b->scope, 0, sizeof(U.b->scope));
    memset(U.b->scope2, 0, sizeof(U.b->scope2));
}

/* One pin on the scope (the same one again keeps its second channel). */
static void select_pin(int pin) { select_pins(pin, pin == U.b->scope_pin ? U.b->scope_pin2 : -1); }

/* ------------------------------------------------------------------------ */
/* Continuity test                                                           */
/* ------------------------------------------------------------------------ */

/* Like a meter's beeper on the bench. Tier 1 (yellow): the copper net of
 * the pin, and the nets joined to it by links (ferrite beads, zero ohm
 * links, jumpers, fuses, coils), which a meter beeps through. Tier 2
 * (cyan): the nets on the other side of a resistor or a diode on those,
 * and what links join to them; not further. A supply on the other side
 * (a pull-up, a pull-down) gets a badge, and isn't followed: every chip
 * of the board is on it. Neither is anything from a supply clicked on.
 * A passive broken open (or with a leg open) stops the test (orange); a
 * shorted part is a wire. A capacitor on a tier 1 net (violet) is shown
 * with what is on its far leg, but not followed: a meter doesn't beep
 * through it, and the far leg is mostly ground. Worked out again when a
 * fault changes. */
#define COL_BEEP nk_rgb(255, 255, 0)
#define COL_THRU nk_rgb(0, 220, 255)
#define COL_CAP  nk_rgb(200, 130, 255)

static struct nk_color tier_color(int tier) {
    return tier == 1 ? COL_BEEP : tier == 2 ? COL_THRU : tier == 3 ? COL_FAULT : tier == 4 ? COL_CAP : COL_TEXT;
}

static void cont_free(void) {
    cont_t *c = &U.cont;
    free(c->tier); free(c->via); free(c->pads); free(c->hops); free(c->wires); free(c->words); free(c->pos);
    memset(c, 0, sizeof(*c));
}

/* Faults that change what the test reaches. */
static uint32_t cont_signature(void) {
    sim_t *s = S();
    uint32_t h = 2166136261u;
    for (int i = 0; i < s->npins; i++) h = (h ^ s->pins[i].fault) * 16777619u;
    for (int i = 0; i < s->nparts; i++) h = (h ^ (uint32_t)(s->parts[i].rfault | s->parts[i].removed << 4)) * 16777619u;
    h = (h ^ (uint32_t)U.b->nbridges) * 16777619u;
    for (int i = 0; i < U.b->nbridges; i++)
        h = (h ^ (uint32_t)(U.b->bridge[i][0] * 65599 + U.b->bridge[i][1])) * 16777619u;
    return h;
}

/* The pads of a net joined by a minimum spanning tree (Prim, n^2: a few
 * hundred pads on the ground net). */
static void cont_mst(const int *pins, int n, uint8_t tier) {
    if (n < 2) return;
    float *dist = malloc(sizeof(float) * n);
    int *from = malloc(sizeof(int) * n);
    for (int k = 0; k < n; k++) { dist[k] = 1e30f; from[k] = 0; }
    dist[0] = -1;           /* in the tree */
    for (int last = 0, w = 0; w < n - 1; w++) {
        const bd_pad_t *a = &BD()->pads[pins[last]];
        int best = -1;
        for (int k = 0; k < n; k++) {
            if (dist[k] < 0) continue;
            const bd_pad_t *c = &BD()->pads[pins[k]];
            float d = (a->x - c->x) * (a->x - c->x) + (a->y - c->y) * (a->y - c->y);
            if (d < dist[k]) { dist[k] = d; from[k] = last; }
            if (best < 0 || dist[k] < dist[best]) best = k;
        }
        U.cont.wires[U.cont.nwires++] = (cont_wire_t){ pins[from[best]], pins[best], tier, false, false };
        dist[best] = -1;
        last = best;
    }
    free(dist);
    free(from);
}

/* "FB1/R42": the passives from the net of the pin to the far leg of a hop
 * (a bridge "U13.11-12", a shorted pin "U7.3"). */
static void cont_chain(int hop, char *buf, size_t len) {
    const cont_hop_t *h = &U.cont.hops[hop];
    int before = U.cont.via[S()->pins[h->leg_in].net];
    char prev[64] = "", me[48];
    if (before >= 0) cont_chain(before, prev, sizeof(prev));
    const char *ref = S()->parts[h->part].ref;
    if (h->what == HOP_BRIDGE) snprintf(me, sizeof(me), "%s.%s-%s", ref, BD()->pads[h->leg_in].name, BD()->pads[h->leg_out].name);
    else if (h->what == HOP_SHORT) snprintf(me, sizeof(me), "%s.%s", ref, BD()->pads[h->at].name);
    else snprintf(me, sizeof(me), "%s", ref);
    snprintf(buf, len, "%s%s%s", prev, prev[0] ? "/" : "", me);
}

static void cont_word(uint8_t tier, bool item, const char *fmt, ...) {
    cont_t *c = &U.cont;
    cont_word_t *w = &c->words[c->nwords++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->text, sizeof(w->text), fmt, ap);
    va_end(ap);
    w->tier = tier;
    w->item = item;
}

/* The pads of a net as list items ("U12.14,"), but one (the leg it was
 * reached by). */
static void cont_net_words(int net, int skip, uint8_t tier) {
    sim_t *s = S();
    int first = U.cont.nwords;
    for (int k = 0; k < U.cont.npads; k++) {
        int pin = U.cont.pads[k].pin;
        if (s->pins[pin].net != net || pin == skip) continue;
        const pin_t *p = &s->pins[pin];
        cont_word(p->fault == PF_OPEN ? 3 : tier, true, "%s.%s%s,", s->parts[p->part].ref, BD()->pads[pin].name,
                  p->fault == PF_OPEN ? " (open)" : p->fault == PF_GND ? " (to GND)" : p->fault == PF_VCC ? " (to +5V)" : "");
    }
    if (U.cont.nwords == first) cont_word(tier, true, "(no other pads),");
    char *t = U.cont.words[U.cont.nwords - 1].text;
    t[strlen(t) - 1] = 0;   /* no comma after the last */
}

/* Through a passive, from its near leg (on a net reached) to the far one,
 * at a tier: the far net is reached too (queued), unless the passive is
 * broken open, or the net is a supply, or was reached already. */
static void cont_hop(const passive_t *pv, int part, int what, int at, int leg_in, int leg_out, int tier, int *queue,
                     int *qt) {
    sim_t *s = S();
    cont_t *c = &U.cont;
    int nout = s->pins[leg_out].net;
    if (nout == s->pins[leg_in].net) return;
    const part_t *p = &s->parts[part];
    cont_hop_t *h = &c->hops[c->nhops];
    *h = (cont_hop_t){ part, leg_in, leg_out, p->rfault == RF_SHORT || what ? PASS_LINK : pv->kind, (uint8_t)tier, false,
                       -1, -1, (uint8_t)what, at };
    h->open = (!what && p->rfault == RF_OPEN) || s->pins[leg_in].fault == PF_OPEN || s->pins[leg_out].fault == PF_OPEN;
    if (h->kind == PASS_CAP) {      /* named, never followed */
        h->tier = 4;
        if (!h->open && nout >= 0) *(board_net_supply(U.b, nout) ? &h->supply : &h->net) = nout;
    } else if (!h->open && nout >= 0 && board_net_supply(U.b, nout)) {
        h->supply = nout;
    } else if (!h->open && nout >= 0 && !c->tier[nout]) {
        c->tier[nout] = (uint8_t)tier;
        c->via[nout] = c->nhops;
        h->net = nout;
        queue[(*qt)++] = nout;
    }
    c->nhops++;
}

static void continuity_trace(void) {
    cont_free();
    int net_a = U.active_continuity_net;
    if (U.cont_pin < 0 || net_a < 0) return;
    sim_t *s = S();
    cont_t *c = &U.cont;
    /* Every passive of the board, with the part it belongs to, and whether
     * a meter beeps through it (a shorted resistor is a wire). */
    typedef struct { passive_t pv; int part; bool link; int what, at; } ppv_t;
    ppv_t *pv = malloc(sizeof(ppv_t) * ((size_t)s->nparts * 16 + BOARD_MAX_BRIDGES + s->npins + 1));
    int npv = 0;
    for (int i = 0; i < s->nparts; i++) {
        passive_t tmp[16];
        int n = board_passives(U.b, i, tmp, 16);
        for (int k = 0; k < n; k++)
            pv[npv++] = (ppv_t){ tmp[k], i, tmp[k].kind == PASS_LINK || s->parts[i].rfault == RF_SHORT, HOP_PASSIVE, -1 };
    }
    /* The faults a meter beeps through: solder bridges, and pins shorted
     * to a supply (a link from the pin to a pad of the supply's net). */
    for (int i = 0; i < U.b->nbridges; i++) {
        int a = U.b->bridge[i][0], b = U.b->bridge[i][1];
        pv[npv++] = (ppv_t){ { a, b, PASS_LINK }, s->pins[a].part, true, HOP_BRIDGE, -1 };
    }
    int gnd_pad = -1, vcc_pad = -1;
    for (int i = 0; i < s->npins && (gnd_pad < 0 || vcc_pad < 0); i++) {
        if (s->pins[i].net == U.b->gnd_net && gnd_pad < 0) gnd_pad = i;
        if (s->pins[i].net == U.b->vcc_net && vcc_pad < 0) vcc_pad = i;
    }
    for (int i = 0; i < s->npins; i++) {
        int to = s->pins[i].fault == PF_GND ? gnd_pad : s->pins[i].fault == PF_VCC ? vcc_pad : -1;
        if (to >= 0 && s->pins[i].net >= 0) pv[npv++] = (ppv_t){ { i, to, PASS_LINK }, s->pins[i].part, true, HOP_SHORT, i };
    }
    c->tier = calloc(s->nnets, 1);
    c->via = malloc(sizeof(int) * s->nnets);
    c->hops = malloc(sizeof(cont_hop_t) * (npv + 1));
    bool *used = calloc(npv + 1, 1);
    int *queue = malloc(sizeof(int) * s->nnets), qh = 0, qt = 0;
    c->tier[net_a] = 1;
    c->via[net_a] = -1;
    queue[qt++] = net_a;
    for (int t = 1; t <= 2; t++) {
        if (t == 2) {
            if (board_net_supply(U.b, net_a)) break;
            /* The resistors and diodes on the tier 1 nets. */
            for (int k = 0; k < npv; k++) {
                if (used[k] || pv[k].link) continue;
                int a = pv[k].pv.pin_a, b = pv[k].pv.pin_b, na = s->pins[a].net, nb = s->pins[b].net;
                if (na >= 0 && c->tier[na] == 1) cont_hop(&pv[k].pv, pv[k].part, pv[k].what, pv[k].at, a, b, 2, queue, &qt), used[k] = true;
                else if (nb >= 0 && c->tier[nb] == 1) cont_hop(&pv[k].pv, pv[k].part, pv[k].what, pv[k].at, b, a, 2, queue, &qt), used[k] = true;
            }
        }
        /* The links from each net reached, in turn. */
        while (qh < qt) {
            int n = queue[qh++];
            for (int k = 0; k < npv; k++) {
                if (used[k] || !pv[k].link) continue;
                int a = pv[k].pv.pin_a, b = pv[k].pv.pin_b;
                if (s->pins[a].net == n) cont_hop(&pv[k].pv, pv[k].part, pv[k].what, pv[k].at, a, b, t, queue, &qt), used[k] = true;
                else if (s->pins[b].net == n) cont_hop(&pv[k].pv, pv[k].part, pv[k].what, pv[k].at, b, a, t, queue, &qt), used[k] = true;
            }
        }
    }
    free(pv);
    free(used);

    /* The pads reached. */
    c->pads = malloc(sizeof(cont_pad_t) * s->npins);
    for (int i = 0; i < s->npins; i++) {
        int n = s->pins[i].net;
        if (n >= 0 && c->tier[n]) c->pads[c->npads++] = (cont_pad_t){ i, c->tier[n] };
    }
    /* Wires: copper within a tier 1 net, dashed from the leg of the passive
     * to every pad of a tier 2 net, and through the body of each passive. */
    c->wires = malloc(sizeof(cont_wire_t) * (s->npins + c->nhops + 1));
    int *pins = malloc(sizeof(int) * s->npins);
    for (int q = 0; q < qt; q++) {
        int n = queue[q], np = 0;
        for (int k = 0; k < c->npads; k++) if (s->pins[c->pads[k].pin].net == n) pins[np++] = c->pads[k].pin;
        if (c->tier[n] == 1) { cont_mst(pins, np, 1); continue; }
        int from = c->hops[c->via[n]].leg_out;
        for (int k = 0; k < np; k++)
            if (pins[k] != from) c->wires[c->nwires++] = (cont_wire_t){ from, pins[k], 2, true, false };
    }
    free(pins);
    for (int k = 0; k < c->nhops; k++) {
        cont_hop_t *h = &c->hops[k];
        if (h->what == HOP_SHORT) {
            if (h->supply >= 0) h->leg_out = h->at;     /* the badge, by the pin */
            continue;
        }
        c->wires[c->nwires++] = (cont_wire_t){ h->leg_in, h->leg_out, h->open ? 3 : h->tier, false, true };
    }

    /* The words of the readout: "Beep [D4]: Direct: U7.40, FB1.1 | Via
     * FB1/R42 [/IRQ]: U17.12 | Via R12: +5V (pull-up)". */
    c->words = malloc(sizeof(cont_word_t) * (s->npins + 4 * c->nhops + 8));
    c->pos = malloc(sizeof(c->pos[0]) * (s->npins + 4 * c->nhops + 8));
    cont_word(1, false, "Beep [%s]:", s->nets[net_a].name);
    cont_word(0, false, "Direct:");
    cont_net_words(net_a, -1, 1);
    for (int k = 0; k < c->nhops; k++) {
        const cont_hop_t *h = &c->hops[k];
        char chain[64];
        cont_chain(k, chain, sizeof(chain));
        if (h->what == HOP_SHORT && h->supply >= 0) {
            cont_word(h->tier, false, "| %s shorted to", chain);
            cont_word(h->tier, true, "%s", board_net_supply(U.b, h->supply) == RAIL_GND ? "GND" : s->nets[h->supply].name);
        } else if (h->what == HOP_SHORT && h->net >= 0) {
            cont_word(h->tier, false, "| Shorted at %s [%s]:", chain, s->nets[h->net].name);
            cont_net_words(h->net, -1, h->tier);
        } else if (h->what == HOP_BRIDGE && !h->open && h->net >= 0) {
            cont_word(h->tier, false, "| Bridge %s [%s]:", chain, s->nets[h->net].name);
            cont_net_words(h->net, h->leg_out, h->tier);
        } else if (h->what == HOP_BRIDGE && !h->open && h->supply >= 0) {
            cont_word(h->tier, false, "| Bridge %s:", chain);
            cont_word(h->tier, true, "%s", board_net_supply(U.b, h->supply) == RAIL_GND ? "GND" : s->nets[h->supply].name);
        } else if (h->open) {
            cont_word(3, false, "| %s open", chain);
        } else if (h->supply >= 0) {
            const char *name = s->nets[h->supply].name;
            bool gnd = board_net_supply(U.b, h->supply) == RAIL_GND;
            cont_word(h->tier, false, "| Via %s:", chain);
            if (h->kind == PASS_RESISTOR) cont_word(h->tier, true, "%s (%s)", name, gnd ? "pull-down" : "pull-up");
            else if (h->kind == PASS_DIODE) cont_word(h->tier, true, "%s (diode)", name);
            else if (h->kind == PASS_CAP) cont_word(h->tier, true, "%s (%s)", name, gnd ? "decoupling" : "capacitor");
            else cont_word(h->tier, true, "%s", name);
        } else if (h->net >= 0 && h->kind == PASS_CAP) {
            cont_word(h->tier, false, "| Via %s:", chain);
            cont_word(h->tier, true, "%s (capacitor)", s->nets[h->net].name);
        } else if (h->net >= 0) {
            cont_word(h->tier, false, "| Via %s [%s]:", chain, s->nets[h->net].name);
            cont_net_words(h->net, h->leg_out, h->tier);
        }
    }
    free(queue);
}

/* Start a continuity test on the net of a pin (-1 ends it). */
static void set_continuity(int pin) {
    U.cont_pin = pin;
    U.active_continuity_net = pin >= 0 ? S()->pins[pin].net : -1;
    U.cont_sig = cont_signature();
    continuity_trace();
}

static void dashed_line(struct nk_command_buffer *cv, float x0, float y0, float x1, float y1, float w,
                        struct nk_color col) {
    float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    if (len < 1) return;
    float dx = (x1 - x0) / len, dy = (y1 - y0) / len;
    for (float d = 0; d < len; d += 10) {
        float e = fminf(d + 6, len);
        nk_stroke_line(cv, x0 + dx * d, y0 + dy * d, x0 + dx * e, y0 + dy * e, w, col);
    }
}

/* The continuity test on the board: the wires, a ring on every pad
 * reached, the badges of the supplies, and the readout along the bottom of
 * the view. */
static void continuity_mark(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area) {
    if (U.cont_pin < 0) return;
    uint32_t sig = cont_signature();
    if (sig != U.cont_sig) { U.cont_sig = sig; continuity_trace(); }
    cont_t *c = &U.cont;
    const struct nk_color dark = nk_rgba(0, 0, 0, 200);
    /* The interface's vertex indices are 16 bit: a whole frame stays under
     * 65536 vertices. A ring is ~90 of them, a square ~16: a big net (the
     * ground, a rail) gets squares and no dark edges. */
    bool many = c->npads + c->nhops > 80;
    float pulse = 0.5f + 0.5f * sinf((float)stm_sec(stm_now()) * 5), fh = ctx->style.font->height;
#define SX(pin) (area.x + U.ox + BD()->pads[pin].x * U.zoom)
#define SY(pin) (area.y + U.oy + BD()->pads[pin].y * U.zoom)
    for (int w = 0; w < c->nwires; w++) {
        const cont_wire_t *cw = &c->wires[w];
        float ax = SX(cw->a), ay = SY(cw->a), bx = SX(cw->b), by = SY(cw->b);
        struct nk_color col = tier_color(cw->tier);
        if (cw->dashed) {
            if (!many) dashed_line(cv, ax, ay, bx, by, 4, dark);
            dashed_line(cv, ax, ay, bx, by, 2, col);
        } else {
            float wd = cw->body ? 4 : 2;
            if (!many) nk_stroke_line(cv, ax, ay, bx, by, wd + 2, dark);
            nk_stroke_line(cv, ax, ay, bx, by, wd, col);
        }
    }
    for (int k = 0; k <= c->npads + c->nhops; k++) {
        /* The pads reached, then the far legs of the passives broken open. */
        int pin, tier;
        if (k < c->npads) { pin = c->pads[k].pin; tier = c->pads[k].tier; }
        else if (k < c->npads + c->nhops && c->hops[k - c->npads].open) { pin = c->hops[k - c->npads].leg_out; tier = 3; }
        else if (k < c->npads + c->nhops && c->hops[k - c->npads].kind == PASS_CAP) { pin = c->hops[k - c->npads].leg_out; tier = 4; }
        else if (k == c->npads + c->nhops && c->npads == 0) { pin = U.cont_pin; tier = 1; }    /* not connected */
        else continue;
        const bd_pad_t *bp = &BD()->pads[pin];
        float x = SX(pin), y = SY(pin);
        float rad = fmaxf(fminf(bp->w, bp->h) * U.zoom * 0.6f, 3) + 3 + 2 * pulse;
        if (S()->pins[pin].fault == PF_OPEN) tier = 3;
        struct nk_rect rr = nk_rect(x - rad, y - rad, 2 * rad, 2 * rad);
        if (many) {
            nk_stroke_rect(cv, rr, 0, 2, tier_color(tier));
        } else {
            nk_stroke_circle(cv, nk_rect(rr.x - 1, rr.y - 1, rr.w + 2, rr.h + 2), 4, dark);
            nk_stroke_circle(cv, rr, 2.5f, tier_color(tier));
        }
        if (pin == U.cont_pin)  /* where the probe is */
            nk_stroke_circle(cv, nk_rect(x - rad - 4, y - rad - 4, 2 * rad + 8, 2 * rad + 8), 2, nk_rgb(255, 255, 255));
    }
    /* "+5V" or "GND" next to the leg of a pull-up or a pull-down. */
    for (int k = 0; k < c->nhops; k++) {
        const cont_hop_t *h = &c->hops[k];
        if (h->supply < 0) continue;
        const char *name = board_net_supply(U.b, h->supply) == RAIL_GND ? "GND" : S()->nets[h->supply].name;
        float x = SX(h->leg_out), y = SY(h->leg_out), tw = text_width(ctx, name);
        /* On the side away from the near leg. */
        float side = SX(h->leg_out) >= SX(h->leg_in) ? 1 : -1;
        struct nk_rect r = nk_rect(side > 0 ? x + 8 : x - 8 - tw - 8, y - fh / 2 - 2, tw + 8, fh + 4);
        nk_fill_rect(cv, r, 3, nk_rgba(0, 0, 0, 220));
        nk_stroke_rect(cv, r, 3, 1.5f, tier_color(h->tier));
        text_at(cv, ctx, r.x + 4, y, 0, name, tier_color(h->tier));
    }
#undef SX
#undef SY
    /* The readout, wrapped to the view: at most a few lines, the rest
     * counted. */
    enum { MAXLINES = 6 };
    float maxw = area.w - 36, sp = text_width(ctx, " ");
    char more[48] = "", none[96];
    int nw = c->nwords, line = 0, last = -1;
    if (U.active_continuity_net < 0 || !nw) {
        char pd[48];
        const pin_t *p = &S()->pins[U.cont_pin];
        snprintf(pd, sizeof(pd), "%s.%s", S()->parts[p->part].ref, BD()->pads[U.cont_pin].name);
        snprintf(none, sizeof(none), "Beep: %s is not connected to anything", pd);
    } else {
        float x = 0;
        for (int k = 0; k < nw; k++) {
            float w = text_width(ctx, c->words[k].text);
            if (x > 0 && x + w > maxw) {
                if (line + 1 >= MAXLINES) break;
                line++;
                x = 0;
            }
            c->pos[k].line = line;
            c->pos[k].x = x;
            x += w + sp;
            last = k;
        }
        /* Out of room: "... and 60 more" at the end of the last line. */
        while (last < nw - 1) {
            int left = 0;
            for (int k = last + 1; k < nw; k++) left += c->words[k].item;
            snprintf(more, sizeof(more), "... and %d more", left);
            if (last < 0 || c->pos[last].line < line ||
                c->pos[last].x + text_width(ctx, c->words[last].text) + sp + text_width(ctx, more) <= maxw)
                break;
            last--;
        }
    }
    const char *hint = "yellow: beeps (copper, links)   cyan: via a resistor or diode   violet: capacitor (not followed)"
                       "   orange: open   shift-click a pad, the bare board to stop";
    float bw = text_width(ctx, hint);
    for (int k = 0; k <= last; k++) bw = fmaxf(bw, c->pos[k].x + text_width(ctx, c->words[k].text));
    if (more[0] && last >= 0) bw = fmaxf(bw, c->pos[last].x + text_width(ctx, c->words[last].text) + sp + text_width(ctx, more));
    int nl = last >= 0 ? line + 1 : 1;
    if (last < 0 && !more[0]) bw = fmaxf(bw, text_width(ctx, none));
    float bh = (nl + 1) * fh + 10;
    struct nk_rect br = nk_rect(area.x + 6, area.y + area.h - bh - 6, fminf(bw + 12, area.w - 12), bh);
    nk_fill_rect(cv, br, 3, nk_rgba(0, 0, 0, 215));
    nk_stroke_rect(cv, br, 3, 1, COL_BEEP);
    if (last < 0 && !more[0]) {
        text_at(cv, ctx, br.x + 6, br.y + 5 + fh * 0.5f, 0, none, COL_BEEP);
    } else {
        for (int k = 0; k <= last; k++)
            text_at(cv, ctx, br.x + 6 + c->pos[k].x, br.y + 5 + fh * (c->pos[k].line + 0.5f), 0, c->words[k].text,
                    tier_color(c->words[k].tier));
        if (more[0]) {
            float mx = last >= 0 ? c->pos[last].x + text_width(ctx, c->words[last].text) + sp : 0;
            text_at(cv, ctx, br.x + 6 + mx, br.y + 5 + fh * (line + 0.5f), 0, more, COL_DIMTEXT);
        }
    }
    text_at(cv, ctx, br.x + 6, br.y + 5 + fh * (nl + 0.5f), 0, hint, COL_DIMTEXT);
}

static void board_input(struct nk_context *ctx, struct nk_rect area) {
    struct nk_input *in = &ctx->input;
    float mx = (in->mouse.pos.x - area.x - U.ox) / U.zoom;
    float my = (in->mouse.pos.y - area.y - U.oy) / U.zoom;
    bool inside = nk_input_is_mouse_hovering_rect(in, area) && nk_window_is_hovered(ctx);
    struct nk_rect ir;     /* the inspector and the browser float above the board */
    if (inspect_bounds(&ir) && nk_input_is_mouse_hovering_rect(in, ir)) inside = false;
    if (browser_bounds(&ir) && nk_input_is_mouse_hovering_rect(in, ir)) inside = false;
    if (diagnose_bounds(&ir) && nk_input_is_mouse_hovering_rect(in, ir)) inside = false;
    if (diagnose_records_bounds(&ir) && nk_input_is_mouse_hovering_rect(in, ir)) inside = false;
    if (probe_bounds(&ir) && nk_input_is_mouse_hovering_rect(in, ir)) inside = false;
    if (atlas_browse_bounds(&ir) && nk_input_is_mouse_hovering_rect(in, ir)) inside = false;
    if (U.help_open && nk_input_is_mouse_hovering_rect(in, U.help_bounds)) inside = false;
    U.hover_pin = U.hover_part = -1;
    if (U.menu_open) return;
    if (inside) {
        U.hover_pin = pick_pin(mx, my);
        if (U.hover_pin < 0) U.hover_part = pick_part(mx, my);
        /* Zoom around the mouse. */
        if (in->mouse.scroll_delta.y != 0) {
            float f = powf(1.15f, in->mouse.scroll_delta.y);
            float nz = fminf(fmaxf(U.zoom * f, 1.0f), 60.0f);
            U.ox = in->mouse.pos.x - area.x - mx * nz;
            U.oy = in->mouse.pos.y - area.y - my * nz;
            U.zoom = nz;
            U.view_moved = true;
        }
        bool middle = nk_input_is_mouse_pressed(in, NK_BUTTON_MIDDLE);
        if (nk_input_is_mouse_pressed(in, NK_BUTTON_LEFT) || nk_input_is_mouse_pressed(in, NK_BUTTON_RIGHT) || middle) {
            /* Shift or the middle button: the continuity test. */
            U.drag_beep = middle || in->keyboard.keys[NK_KEY_SHIFT].down;
            U.dragging = true;
            U.drag_moved = false;
            U.drag_x = in->mouse.pos.x;
            U.drag_y = in->mouse.pos.y;
        }
    }
    if (U.dragging) {
        if (fabsf(in->mouse.pos.x - U.drag_x) + fabsf(in->mouse.pos.y - U.drag_y) > 4) U.drag_moved = true;
        if (U.drag_moved) { U.ox += in->mouse.delta.x; U.oy += in->mouse.delta.y; U.view_moved = true; }
        bool released = nk_input_is_mouse_released(in, NK_BUTTON_LEFT) || nk_input_is_mouse_released(in, NK_BUTTON_RIGHT) ||
                        nk_input_is_mouse_released(in, NK_BUTTON_MIDDLE);
        if (released) {
            U.dragging = false;
            /* A click (not a drag): select the pin and open the fault menu,
             * or test its continuity; on the bare board, stop the test,
             * and a double click there fits the board in the view. */
            if (!U.drag_moved && inside && U.drag_beep) {
                set_continuity(U.hover_pin);
            } else if (!U.drag_moved && inside && U.hover_pin < 0 && U.hover_part < 0) {
                set_continuity(-1);
                double now = stm_sec(stm_now());
                float x = in->mouse.pos.x, y = in->mouse.pos.y;
                if (now - U.last_click < 0.4 && fabsf(x - U.last_click_x) + fabsf(y - U.last_click_y) < 8) {
                    fit_view(area);
                    now = 0;    /* a third click starts over */
                }
                U.last_click = now;
                U.last_click_x = x;
                U.last_click_y = y;
            } else if (!U.drag_moved && inside && diagnose_recording()) {
                /* Recording a repair: the chip clicked is faulty (or not
                 * any more). */
                int part = U.hover_part >= 0 ? U.hover_part : S()->pins[U.hover_pin].part;
                const char *det = board_part_detail(U.b, part);
                char what[64];
                snprintf(what, sizeof(what), "%s%s%.*s", board_part_label(U.b, part), det && *det ? ": " : "",
                         det ? (int)strcspn(det, "\n,") : 0, det ? det : "");
                diagnose_record_toggle(S()->parts[part].ref, what);
            } else if (!U.drag_moved && inside) {
                U.menu_open = true;
                U.menu_mem = false;
                U.menu_pin = U.hover_pin;
                U.menu_part = U.hover_part;
                U.menu_x = in->mouse.pos.x + 10;
                U.menu_y = in->mouse.pos.y + 10;
                U.menu_frame = (int)U.frame;
                if (U.hover_pin >= 0) select_pin(U.hover_pin);
            }
        }
    }
}

/* A radio option that applies an action when chosen. */
static bool option(struct nk_context *ctx, const char *label, bool active) {
    return nk_option_label(ctx, label, active) && !active;
}

/* The faults the menu offers inside a memory chip, with their names. One
 * bad cell (or corrupted byte) goes at a random address. */
#define MAX_MEM_CHOICES 40
static int mem_fault_choices(int part, mem_fault_t *mf, char (*label)[48]) {
    int bits, n = 0;
    bool dram;
    int lines = part_memory(&S()->parts[part], &bits, &dram);
    if (!lines) return 0;
    mf[n] = (mem_fault_t){ MF_OK, 0, 0, 0 };
    snprintf(label[n++], 48, "working");
    for (int b = 0; b < bits; b++)
        for (int v = 0; v < 2; v++) {
            mf[n] = (mem_fault_t){ MF_DATA, (uint8_t)b, (uint8_t)v, 0 };
            if (bits == 1) snprintf(label[n++], 48, "dead: reads %d everywhere", v);
            else snprintf(label[n++], 48, "D%d dead: reads %d everywhere", b, v);
        }
    for (int a = 0; a < lines; a++) {
        mf[n] = (mem_fault_t){ MF_ADDR, (uint8_t)a, 0, 0 };
        snprintf(label[n++], 48, "address line A%d dead inside", a);
    }
    unsigned r = (unsigned)rand() * 2654435761u >> 8;
    uint32_t addr = dram ? ((r >> 8) % (1u << lines)) << 8 | (r % (1u << lines)) : r % (1u << lines);
    mf[n] = (mem_fault_t){ MF_FLIP, 0, (uint8_t)(1u << ((r >> 20) % (unsigned)bits)), addr };
    snprintf(label[n++], 48, bits == 8 ? "one corrupted byte (random)" : "one bad cell (random)");
    if (dram) {
        mf[n] = (mem_fault_t){ MF_WEAK, 0, 0, 0 };
        snprintf(label[n++], 48, "weak cells (1 in 64 forget)");
    }
    return n;
}

/* The menu of a pin or a part, at the mouse. A window of its own, so it
 * opens from the board and from the inspector alike. */
static void fault_menu(struct nk_context *ctx) {
    if (!U.menu_open) return;
    sim_t *s = S();
    char title[128], info[128] = "";
    int rows;
    bool show_gnd = true, show_vcc = true, show_out = false, can_short = false, faultable = true;
    mem_fault_t mem[MAX_MEM_CHOICES];
    char mem_label[MAX_MEM_CHOICES][48];
    int nmem = 0;
    if (U.menu_pin >= 0) {
        const pin_t *pp = &s->pins[U.menu_pin];
        int kind = s->parts[pp->part].kind, rail = pp->root >= 0 ? s->nets[pp->root].rail : RAIL_NONE;
        pin_desc(U.b, U.menu_pin, title, sizeof(title));
        /* A chip's power pin can only be lifted; a pad already on ground
         * (or +5V) can't be shorted to it. */
        show_gnd = !(pp->flags & PIN_POWER) && rail != RAIL_GND;
        show_vcc = !(pp->flags & PIN_POWER) && rail != RAIL_VCC;
        /* A connector pin on a rail or on a part of the board that isn't
         * simulated (video output, power supply): nothing to break. */
        if (kind == PART_CONNECTOR) {
            if (pp->net < 0 || !board_net_simulated(U.b, pp->net)) {
                faultable = false;
                snprintf(info, sizeof(info), "not simulated (analog or power part)");
            } else if (rail) {
                faultable = false;
                snprintf(info, sizeof(info), "power rail: break the chips' power pins");
            }
        } else if (kind != PART_IC) {
            /* A passive's pad: its leg, like a chip's. */
            if (pp->net < 0) {
                faultable = false;
                snprintf(info, sizeof(info), "not connected");
            } else if (rail || board_net_supply(U.b, pp->net)) {
                snprintf(info, sizeof(info), "on a supply: a short is a dead short (it pulls the supply down)");
            } else if (!board_net_simulated(U.b, pp->net)) {
                snprintf(info, sizeof(info), "analog net (not simulated): continuity only");
            }
        }
        show_out = faultable && kind == PART_IC && (pp->flags & PIN_OUT) && !(pp->flags & PIN_POWER);
        rows = !faultable ? 1 : 2 + show_gnd + show_vcc + 2 * show_out + (info[0] != 0);
        /* A solder bridge to each neighbor leg of a chip. */
        if (faultable)
            for (int dir = -1; dir <= 1; dir += 2) rows += board_dip_neighbor(U.b, U.menu_pin, dir) >= 0;
    } else {
        part_t *p = &s->parts[U.menu_part];
        if (p->kind == PART_IC)
            snprintf(title, sizeof(title), "%s %s", p->ref, board_part_label(U.b, U.menu_part));
        else
            snprintf(title, sizeof(title), "%s %s %s", p->ref, board_part_kind_name(U.b, U.menu_part), p->name);
        can_short = board_part_can_short(U.b, U.menu_part);
        if (p->kind == PART_IC) nmem = mem_fault_choices(U.menu_part, mem, mem_label);
        rows = p->kind == PART_IC ? 4 + (nmem > 0) : 2 + can_short;     /* IC: + its repair records */
    }
    float mem_h = nmem && U.menu_mem ? fminf((float)nmem, 12) * 24 + 8 : 0;
    float w = fmaxf(nmem ? 340 : U.menu_pin < 0 ? 300 : 260, fmaxf(text_width(ctx, title), text_width(ctx, info)) + 30);
    float h = 46 + rows * 24 + mem_h + (U.menu_pin >= 0 ? 22 : 0);
    float x = fminf(U.menu_x, sapp_widthf() - w - 4), y = fminf(U.menu_y, sapp_heightf() - h - 4);
    struct nk_rect r = nk_rect(x, y, w, h);
    nk_window_set_bounds(ctx, "Menu", r);
    if (nk_begin(ctx, "Menu", r, NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, title, NK_TEXT_LEFT);
        if (U.menu_pin >= 0) {
            int pin = U.menu_pin, f = s->pins[pin].fault;
            char net[96];
            snprintf(net, sizeof(net), "net %s", pin_net_name(U.b, pin));
            nk_label_colored(ctx, net, NK_TEXT_LEFT, COL_DIMTEXT);
            nk_layout_row_dynamic(ctx, 22, 1);
            if (!faultable) {
                nk_label_colored(ctx, info, NK_TEXT_LEFT, COL_DIMTEXT);
            } else {
                if (info[0]) nk_label_colored(ctx, info, NK_TEXT_LEFT, COL_DIMTEXT);
                if (option(ctx, "Working", f == PF_OK)) { board_set_pin_fault(U.b, pin, PF_OK); U.menu_open = false; }
                if (option(ctx, "Floating (open, lifted leg)", f == PF_OPEN)) { board_set_pin_fault(U.b, pin, PF_OPEN); U.menu_open = false; }
                if (show_gnd && option(ctx, "Shorted to GND", f == PF_GND)) { board_set_pin_fault(U.b, pin, PF_GND); U.menu_open = false; }
                if (show_vcc && option(ctx, "Shorted to +5V", f == PF_VCC)) { board_set_pin_fault(U.b, pin, PF_VCC); U.menu_open = false; }
                if (show_out && option(ctx, "Output dead inside the chip, stuck low", f == PF_OUT0)) {
                    board_set_pin_fault(U.b, pin, PF_OUT0); U.menu_open = false;
                }
                if (show_out && option(ctx, "Output dead inside the chip, stuck high", f == PF_OUT1)) {
                    board_set_pin_fault(U.b, pin, PF_OUT1); U.menu_open = false;
                }
                for (int dir = -1; dir <= 1; dir += 2) {
                    int nb = board_dip_neighbor(U.b, pin, dir);
                    if (nb < 0) continue;
                    char lab[64];
                    const char *nn = sim_pin_name(s, nb);
                    snprintf(lab, sizeof(lab), "Solder bridge to pin %d%s%s%s", s->pins[nb].number, nn ? " (" : "",
                             nn ? nn : "", nn ? ")" : "");
                    nk_bool on = board_find_bridge(U.b, pin, nb) >= 0;
                    if (nk_checkbox_label(ctx, lab, &on)) {
                        board_set_bridge(U.b, pin, nb, on);
                        if (on) select_pins(pin, nb);
                        U.menu_open = false;
                    }
                }
            }
        } else {
            int part = U.menu_part;
            part_t *p = &s->parts[part];
            nk_layout_row_dynamic(ctx, 22, 1);
            if (p->unfitted) {
                nk_label_colored(ctx, "Not fitted on this board (an option in the controls)", NK_TEXT_LEFT, COL_DIMTEXT);
            } else if (p->kind == PART_IC) {
                if (option(ctx, "In its socket", !p->removed)) { board_set_removed(U.b, part, false); U.menu_open = false; }
                if (option(ctx, "Pulled out of the socket", p->removed)) { board_set_removed(U.b, part, true); U.menu_open = false; }
                /* Faults inside a memory chip: a list that opens in the menu. */
                if (nmem) {
                    int cur = 0;
                    for (int k = 1; k < nmem; k++)
                        if (mem[k].kind == p->mfault && (p->mfault == MF_FLIP || p->mfault == MF_WEAK ||
                                                         (mem[k].bit == p->mf_bit && mem[k].val == p->mf_val)))
                            cur = k;
                    char lab[80];
                    snprintf(lab, sizeof(lab), "Inside the chip: %.47s %s", mem_label[cur], U.menu_mem ? "-" : "+");
                    if (nk_button_label(ctx, lab)) U.menu_mem = !U.menu_mem;
                    if (U.menu_mem) {
                        nk_layout_row_dynamic(ctx, mem_h - 4, 1);
                        if (nk_group_begin(ctx, "memfaults", NK_WINDOW_BORDER)) {
                            nk_layout_row_dynamic(ctx, 20, 1);
                            for (int k = 0; k < nmem; k++)
                                if (option(ctx, mem_label[k], k == cur)) {
                                    board_set_mem_fault(U.b, part, &mem[k]);
                                    U.menu_open = false;
                                }
                            nk_group_end(ctx);
                        }
                        nk_layout_row_dynamic(ctx, 22, 1);
                    }
                }
                if (nk_button_label(ctx, "Inspect: pins, gates, live states")) { inspect_open(U.b, part); U.menu_open = false; }
                if (chip_records_button(ctx, part)) U.menu_open = false;
            } else {
                int f = p->rfault;
                const char *what = board_part_kind_name(U.b, part);
                bool crystal = !strcmp(what, "crystal"), cap = !strcmp(what, "capacitor"), sw = !strcmp(what, "switch");
                if (option(ctx, sw ? "On" : "Working", f == RF_OK)) { board_set_rfault(U.b, part, RF_OK); U.menu_open = false; }
                if (option(ctx, sw ? "Off" : crystal ? "Broken (no oscillation)" : cap ? "Open (no effect at DC)" :
                                p->kind != PART_OTHER ? "Open" : "Broken", f == RF_OPEN)) {
                    board_set_rfault(U.b, part, RF_OPEN); U.menu_open = false;
                }
                if (can_short && option(ctx, crystal ? "Shorted (no oscillation)" :
                                             !strcmp(what, "transistor") ? "Shorted (collector to emitter)" :
                                             "Shorted (its two nets joined)", f == RF_SHORT)) {
                    board_set_rfault(U.b, part, RF_SHORT); U.menu_open = false;
                }
            }
        }
    }
    nk_end(ctx);
    if ((int)U.frame == U.menu_frame) nk_window_set_focus(ctx, "Menu");
    /* Clicking outside the menu closes it. */
    if ((int)U.frame > U.menu_frame + 1 && nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) &&
        !nk_input_is_mouse_hovering_rect(&ctx->input, r))
        U.menu_open = false;
}

/* The tooltip next to the mouse on the board: what the pin or part is. */
static void board_tooltip(struct nk_context *ctx) {
    if (U.menu_open || U.dragging) return;
    char tip[512];
    if (U.hover_pin >= 0) {
        char pd[128];
        pin_desc(U.b, U.hover_pin, pd, sizeof(pd));
        int net = S()->pins[U.hover_pin].net;
        bool sim = board_net_simulated(U.b, net);
        int f = g_hide_faults ? PF_OK : S()->pins[U.hover_pin].fault;
        snprintf(tip, sizeof(tip), "%s   net %s: %s%s%s", pd, pin_net_name(U.b, U.hover_pin),
                 !sim ? "not simulated" : net >= 0 ? state_name(U.net_state[net]) : "-",
                 f != PF_OK ? "   leg " : "", f != PF_OK ? pin_fault_name(f) : "");
    } else if (U.hover_part >= 0) {
        const part_t *hp = &S()->parts[U.hover_part];
        bool shown = !g_hide_faults;
        if (hp->kind == PART_IC)
            snprintf(tip, sizeof(tip), "%s %s: %s", hp->ref, board_part_label(U.b, U.hover_part),
                     board_part_detail(U.b, U.hover_part));
        else if (hp->kind == PART_CONNECTOR)
            snprintf(tip, sizeof(tip), "%s %s", hp->ref, board_part_label(U.b, U.hover_part));
        else
            snprintf(tip, sizeof(tip), "%s %s %s%s   click: break or short it", hp->ref,
                     board_part_kind_name(U.b, U.hover_part), hp->name,
                     !shown || hp->rfault == RF_OK ? "" : hp->rfault == RF_SHORT ? " (shorted)" :
                     !strcmp(board_part_kind_name(U.b, U.hover_part), "switch") ? " (off)" : " (open)");
        /* A chip's faults (a passive says it above). */
        char faults[240];
        if (shown && hp->kind != PART_RESISTOR && hp->kind != PART_LINK && hp->kind != PART_OTHER &&
            part_faults(U.hover_part, faults, sizeof(faults))) {
            size_t l = strlen(tip);
            snprintf(tip + l, sizeof(tip) - l, "   FAULT: %s", faults);
        }
    } else {
        return;
    }
    nk_tooltip(ctx, tip);
}

/* The part (and pin) a diagnosis put a fault on, outlined and pulsing for
 * a while, with what was done to it. */
static void focus_mark(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area) {
    double now = stm_sec(stm_now());
    if (U.focus_part < 0 || now > U.focus_until) return;
    float pulse = 0.5f + 0.5f * sinf((float)now * 6);
    const box_t *fb = &U.box[U.focus_part];
    struct nk_rect br = to_screen(area, fb->x0, fb->y0, fb->x1, fb->y1);
    float g = 3 + 5 * pulse;
    nk_stroke_rect(cv, nk_rect(br.x - g, br.y - g, br.w + 2 * g, br.h + 2 * g), 4, 3, COL_FAULT);
    for (int i = 0; i < 2; i++) {
        int pin = i ? U.focus_pin2 : U.focus_pin;
        if (pin < 0) continue;
        const bd_pad_t *bp = &BD()->pads[pin];
        struct nk_rect pr = to_screen(area, bp->x - bp->w / 2, bp->y - bp->h / 2, bp->x + bp->w / 2, bp->y + bp->h / 2);
        float k = 4 + 4 * pulse;
        nk_stroke_circle(cv, nk_rect(pr.x - k, pr.y - k, pr.w + 2 * k, pr.h + 2 * k), 3, COL_FAULT);
    }
    if (U.focus_note[0]) {
        float fh = ctx->style.font->height, tw = text_width(ctx, U.focus_note);
        struct nk_rect tr = nk_rect(br.x, br.y - g - fh - 10, tw + 12, fh + 6);
        tr.x = fmaxf(area.x + 4, fminf(tr.x, area.x + area.w - tr.w - 4));     /* in the view */
        nk_fill_rect(cv, tr, 3, nk_rgba(0, 0, 0, 200));
        text_at(cv, ctx, tr.x + 6, tr.y + tr.h / 2, 0, U.focus_note, COL_FAULT);
    }
}

/* The pin "Diagnose by probing" asks for: a pulsing cyan ring, and a tag. */
static void probe_mark(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect area) {
    int pin = probe_marked_pin();
    if (pin < 0) return;
    float pulse = 0.5f + 0.5f * sinf((float)stm_sec(stm_now()) * 5);
    const bd_pad_t *bp = &BD()->pads[pin];
    struct nk_rect pr = to_screen(area, bp->x - bp->w / 2, bp->y - bp->h / 2, bp->x + bp->w / 2, bp->y + bp->h / 2);
    float k = 5 + 5 * pulse;
    nk_stroke_circle(cv, nk_rect(pr.x - k, pr.y - k, pr.w + 2 * k, pr.h + 2 * k), 3, COL_HOVER);
    char tag[40];
    snprintf(tag, sizeof(tag), "probe %s.%s", S()->parts[S()->pins[pin].part].ref, bp->name);
    float fh = ctx->style.font->height, tw = text_width(ctx, tag);
    struct nk_rect tr = nk_rect(pr.x + pr.w + k + 4, pr.y + pr.h / 2 - fh / 2 - 3, tw + 12, fh + 6);
    nk_fill_rect(cv, tr, 3, nk_rgba(0, 0, 0, 200));
    text_at(cv, ctx, tr.x + 6, tr.y + tr.h / 2, 0, tag, COL_HOVER);
}

static void board_window(struct nk_context *ctx, float w, float h) {
    nk_window_set_bounds(ctx, "Board", nk_rect(0, 0, w, h));
    if (nk_begin(ctx, "Board", nk_rect(0, 0, w, h), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND)) {
        struct nk_rect area = nk_window_get_content_region(ctx);
        struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
        /* Fit at the start, and again when the window is resized while
         * the board is still as fitted. */
        if (!U.fitted || (!U.view_moved && (area.w != U.fit_w || area.h != U.fit_h))) fit_view(area);
        if (U.view_set) {       /* --view: center on a point of the board */
            U.zoom *= U.view_zoom;
            U.ox = area.w / 2 - U.view_cx * U.zoom;
            U.oy = area.h / 2 - U.view_cy * U.zoom;
            U.view_set = false;
            U.view_moved = true;
        }
        if (U.focus_pending) {  /* a diagnosis put a fault there: zoom in on it */
            float bw = BD()->outline[2] - BD()->outline[0], bh = BD()->outline[3] - BD()->outline[1];
            const box_t *fb = &U.box[U.focus_part];
            U.zoom = fminf(area.w / bw, area.h / bh) * 0.98f * 3;
            U.ox = area.w / 2 - (fb->x0 + fb->x1) / 2 * U.zoom;
            U.oy = area.h / 2 - (fb->y0 + fb->y1) / 2 * U.zoom;
            U.focus_pending = false;
            U.view_moved = true;
        }
        board_input(ctx, area);
        nk_push_scissor(cv, area);
        nk_fill_rect(cv, area, 0, g_theme.bg);
        struct nk_rect pcb = to_screen(area, BD()->outline[0], BD()->outline[1], BD()->outline[2], BD()->outline[3]);
        nk_fill_rect(cv, pcb, 8, COL_PCB);
        nk_stroke_rect(cv, pcb, 8, 2, COL_PCB_EDGE);
        sim_t *s = S();
        for (int pass = 0; pass < 2; pass++)          /* passives below, chips above */
            for (int i = 0; i < s->nparts; i++)
                if ((s->parts[i].kind == PART_IC) == (pass == 1)) draw_part(cv, ctx, area, i);
        draw_pins(cv, ctx, area);
        fault_badges(cv, ctx, area);
        if (U.hover_part >= 0) {
            box_t *b = &U.box[U.hover_part];
            nk_stroke_rect(cv, to_screen(area, b->x0, b->y0, b->x1, b->y1), 2, 2, COL_HOVER);
        }
        if (diagnose_recording())       /* the chips picked as faulty for a repair record */
            for (int i = 0; i < s->nparts; i++)
                if (diagnose_record_marked(s->parts[i].ref)) {
                    box_t *b = &U.box[i];
                    nk_stroke_rect(cv, to_screen(area, b->x0 - 0.8f, b->y0 - 0.8f, b->x1 + 0.8f, b->y1 + 0.8f), 3, 3,
                                   COL_FAULT);
                }
        continuity_mark(cv, ctx, area);
        focus_mark(cv, ctx, area);
        probe_mark(cv, ctx, area);
        board_tooltip(ctx);
    }
    nk_end(ctx);
}

/* ------------------------------------------------------------------------ */
/* Monitor                                                                   */
/* ------------------------------------------------------------------------ */

static void monitor_window(struct nk_context *ctx, float x, float y) {
    /* The simulated video chip only produces a new picture 50/60 times a
     * second; redrawing the pixel buffer and re-uploading the whole
     * texture to the GPU on every UI frame is pure waste on a 120/144 Hz
     * monitor, where ui_frame() itself runs 2-3x more often than that.
     * Skip both unless the board actually reached a new video frame.
     * While the cycle stands still (paused) redraw every time, so a fault
     * injected while paused shows up at once. */
    uint64_t cycle = U.b->sim.cycle, per = (uint64_t)M()->cycles_per_frame;
    if (cycle / per != U.monitor_cycle / per || cycle == U.monitor_cycle || !U.monitor_cycle) {
        board_monitor(U.b, U.monitor);
        size_t bytes = (size_t)M()->screen_w * M()->screen_h * sizeof(uint32_t);
        sg_update_image(U.img, &(sg_image_data){ .mip_levels[0] = { U.monitor, bytes } });
    }
    U.monitor_cycle = cycle;
    struct nk_rect r = nk_rect(x, y, RIGHT_W, MON_H + 44);
    nk_window_set_bounds(ctx, "Monitor", r);
    if (nk_begin(ctx, "Monitor", r, NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        /* The picture with its own proportions (square pixels). */
        float sw = (float)M()->screen_w, sh = (float)M()->screen_h;
        float scale = fminf(MON_W / sw, MON_H / sh), w = sw * scale;
        nk_layout_row_begin(ctx, NK_STATIC, MON_H, 2);
        nk_layout_row_push(ctx, (MON_W - w) / 2);
        nk_spacing(ctx, 1);
        nk_layout_row_push(ctx, w);
        nk_image(ctx, nk_image_handle(snk_nkhandle(U.snk_img)));
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

/* ------------------------------------------------------------------------ */
/* Oscilloscope                                                              */
/* ------------------------------------------------------------------------ */

/* Time bases: a whole screen is 16 to 4096 cycles (C64), always named
 * in microseconds per division. */
static const int timebase_cycles[] = { 16, 64, 256, 1024, 4096 };
#define NTIMEBASES 5

static const char *timebase_name(int i) {
    static char names[NTIMEBASES][24];
    double us = timebase_cycles[i] * 1e6 / M()->cpu_hz / 10;
    snprintf(names[i], sizeof(names[i]), us < 10 ? "%.1f us/div" : "%.0f us/div", us);
    return names[i];
}

/* One channel of the scope: the net of the pin, and when two channels are
 * shown (both legs of a solder bridge), what each leg drives itself
 * (amber), to see who fights whom. */
static void draw_trace(struct nk_command_buffer *cv, struct nk_rect r, const uint8_t *buf, uint32_t start,
                       uint32_t span, bool own) {
    float y_hi = r.y + r.h * 0.2f, y_lo = r.y + r.h * 0.8f, y_mid = r.y + r.h * 0.5f;
    float prev_y = -1, prev_own = -1;
    int cols = (int)r.w;
    for (int x = 0; x < cols; x++) {
        uint32_t i0 = start + (uint32_t)((uint64_t)span * x / cols);
        uint32_t i1 = start + (uint32_t)((uint64_t)span * (x + 1) / cols);
        if (i1 <= i0) i1 = i0 + 1;
        bool hi = false, lo = false, fl = false, fight = false, own_hi = false, own_lo = false;
        for (uint32_t i = i0; i < i1; i++) {
            uint8_t v = buf[i & (SCOPE_SAMPLES - 1)];
            if (v & SC_FIGHT) fight = true;
            if (v & SC_FLOATING) fl = true;
            else if (v & SC_LEVEL) hi = true;
            else lo = true;
            if (v & SC_OWN_HI) own_hi = true;
            if (v & SC_OWN_LO) own_lo = true;
        }
        float px = r.x + x;
        if (fight) {
            nk_stroke_line(cv, px, y_hi, px, y_lo, 1, COL_FIGHT);
            prev_y = y_mid;
        } else if (hi && lo) {
            nk_stroke_line(cv, px, y_hi, px, y_lo, 1, COL_TRACE);
            prev_y = -1;
        } else if (hi || lo) {
            float y = hi ? y_hi : y_lo;
            if (prev_y >= 0 && prev_y != y) nk_stroke_line(cv, px, prev_y, px, y, 1, COL_TRACE);
            nk_stroke_line(cv, px, y, px + 1, y, 1.5f, COL_TRACE);
            prev_y = y;
        } else if (fl) {
            /* Floating: dashed line in the middle, nobody drives the net. */
            if ((x / 3) % 2 == 0) nk_stroke_line(cv, px, y_mid, px + 1, y_mid, 1.5f, COL_PULSE);
            prev_y = -1;
        }
        if (!own) continue;
        struct nk_color amber = nk_rgb(255, 170, 40);
        float d = r.h * 0.07f;
        if (own_hi && own_lo) {
            nk_stroke_line(cv, px, y_hi - d, px, y_lo + d, 1, amber);
            prev_own = -1;
        } else if (own_hi || own_lo) {
            float y = own_hi ? y_hi - d : y_lo + d;
            if (prev_own >= 0 && prev_own != y) nk_stroke_line(cv, px, prev_own, px, y, 1, amber);
            nk_stroke_line(cv, px, y, px + 1, y, 1, amber);
            prev_own = y;
        } else {
            prev_own = -1;      /* the leg drives nothing: an input, or off */
        }
    }
}

static void draw_scope(struct nk_command_buffer *cv, struct nk_context *ctx, struct nk_rect r) {
    board_t *b = U.b;
    nk_fill_rect(cv, r, 0, nk_rgb(8, 16, 10));
    /* Graticule: 10 x 4 divisions. */
    for (int i = 0; i <= 10; i++) {
        float x = r.x + r.w * i / 10;
        nk_stroke_line(cv, x, r.y, x, r.y + r.h, 1, nk_rgb(30, 60, 36));
    }
    for (int i = 0; i <= 4; i++) {
        float y = r.y + r.h * i / 4;
        nk_stroke_line(cv, r.x, y, r.x + r.w, y, 1, nk_rgb(30, 60, 36));
    }
    if (b->scope_pin < 0) return;
    uint32_t end = b->scope_pos;
    uint32_t span = (uint32_t)timebase_cycles[U.timebase] * (uint32_t)M()->steps_per_cycle;
    uint32_t avail = end < SCOPE_SAMPLES ? end : SCOPE_SAMPLES;
    if (avail < span) return;
    uint32_t start = end - span;
    /* Trigger on a rising edge, placed at 10% of the screen. */
    if (U.trigger) {
        uint32_t pre = span / 10, oldest = end - avail + 1;
        for (uint32_t t = end - span + pre; t > oldest + pre; t--) {
            uint8_t a = b->scope[(t - 1) & (SCOPE_SAMPLES - 1)], c = b->scope[t & (SCOPE_SAMPLES - 1)];
            if (!(a & (SC_LEVEL | SC_FLOATING)) && (c & SC_LEVEL) && !(c & SC_FLOATING)) {
                start = t - pre;
                break;
            }
        }
    }
    if (b->scope_pin2 < 0) {
        draw_trace(cv, r, b->scope, start, span, false);
        return;
    }
    /* Both legs of a bridge, one above the other, on the same time base. */
    for (int ch = 0; ch < 2; ch++) {
        struct nk_rect half = nk_rect(r.x, r.y + r.h / 2 * ch, r.w, r.h / 2);
        draw_trace(cv, half, ch ? b->scope2 : b->scope, start, span, true);
        int pin = ch ? b->scope_pin2 : b->scope_pin;
        const char *pn = sim_pin_name(S(), pin);
        char lab[64];
        snprintf(lab, sizeof(lab), "%s.%d%s%s%s", S()->parts[S()->pins[pin].part].ref, S()->pins[pin].number,
                 pn ? " " : "", pn ? pn : "", ch ? "" : "   amber: what each leg drives");
        text_at(cv, ctx, half.x + 4, half.y + ctx->style.font->height / 2 + 2, 0, lab, COL_DIMTEXT);
    }
    nk_stroke_line(cv, r.x, r.y + r.h / 2, r.x + r.w, r.y + r.h / 2, 1, nk_rgb(60, 110, 70));
}

static void scope_window(struct nk_context *ctx, float x, float y, float h) {
    struct nk_rect r = nk_rect(x, y, RIGHT_W, h);
    nk_window_set_bounds(ctx, "Oscilloscope", r);
    if (nk_begin(ctx, "Oscilloscope", r, NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        char label[160] = "click a pin on the board";
        if (U.b->scope_pin >= 0 && U.b->scope_pin2 >= 0) {
            const sim_t *s = S();
            int pa = U.b->scope_pin, pb = U.b->scope_pin2;
            snprintf(label, sizeof(label), "%s pins %d + %d   net %s", s->parts[s->pins[pa].part].ref, s->pins[pa].number,
                     s->pins[pb].number, pin_net_name(U.b, pa));
        } else if (U.b->scope_pin >= 0) {
            char pd[96];
            pin_desc(U.b, U.b->scope_pin, pd, sizeof(pd));
            snprintf(label, sizeof(label), "%s   net %s", pd, pin_net_name(U.b, U.b->scope_pin));
        }
        nk_layout_row_begin(ctx, NK_STATIC, 22, 3);
        nk_layout_row_push(ctx, 250);
        nk_label(ctx, label, NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 110);
        const char *names[NTIMEBASES];
        for (int i = 0; i < NTIMEBASES; i++) names[i] = timebase_name(i);
        U.timebase = nk_combo(ctx, names, NTIMEBASES, U.timebase, 20, nk_vec2(120, 140));
        nk_layout_row_push(ctx, 80);
        nk_bool trig = U.trigger;
        nk_checkbox_label(ctx, "trigger", &trig);
        U.trigger = trig;
        nk_layout_row_end(ctx);
        struct nk_rect area = nk_window_get_content_region(ctx);
        float plot_h = area.y + area.h - (ctx->current->layout->at_y + 30) - 4;
        nk_layout_row_dynamic(ctx, plot_h > 40 ? plot_h : 40, 1);
        struct nk_rect plot;
        if (nk_widget(&plot, ctx) != NK_WIDGET_INVALID) draw_scope(nk_window_get_canvas(ctx), ctx, plot);
    }
    nk_end(ctx);
}

/* ------------------------------------------------------------------------ */
/* Controls                                                                  */
/* ------------------------------------------------------------------------ */

/* The file's name, not its whole path (for the narrow panel). */
static const char *base_name(const char *path) { return board_basename(path); }

/* A save file: its board replaces the one on the bench (the board must be
 * locked), its notes the notes. */
static bool load_state_file(const char *path) {
    char err[256], notes[sizeof(U.notes)];
    board_t *nb = board_load_file(path, notes, sizeof(notes), err, sizeof(err));
    if (!nb) {
        snprintf(U.status, sizeof(U.status), "%s: %s", base_name(path), err);
        return false;
    }
    inspect_open(U.b, -1);
    board_t *old = U.b;
    attach_board(nb);
    board_destroy(old);
    U.diag_rematch = true;
    U.autoload[0] = 0;
    snprintf(U.notes, sizeof(U.notes), "%s", notes);
    snprintf(U.state_path, sizeof(U.state_path), "%s", path);
    snprintf(U.status, sizeof(U.status), "%s loaded: %s at %.2f s", base_name(path), nb->m->board_name,
             (double)nb->sim.cycle / nb->m->cpu_hz);
    browser_add_recent(path);
    return true;
}

static void save_state_file(const char *path) {
    char err[256];
    if (board_save_file(U.b, path, U.notes, err, sizeof(err))) {
        snprintf(U.state_path, sizeof(U.state_path), "%s", path);
        snprintf(U.status, sizeof(U.status), "Saved to %.200s at %.2f s", base_name(path),
                 (double)U.b->sim.cycle / M()->cpu_hz);
    } else {
        snprintf(U.status, sizeof(U.status), "Not saved: %s", err);
    }
}

/* Load a program or plug a cartridge (what the machine can take). A file
 * loaded goes on the list of recent ones. */
static bool load_prg(const char *path, bool run) {
    if (board_is_state_file(path)) return load_state_file(path);
    size_t n = strlen(path);
    bool ok, bad = false, other_bad = true;
    char info[128];
    const file_kind_t *fk = M()->file_kind(path, info, sizeof(info), &bad);
    /* A file for the other machine (a Spectrum ROM dropped on the C64, a
     * C64 one on the Spectrum): that machine goes on the bench after the
     * frame, and gets the file then. */
    const machine_t *other = board_is_c64(U.b) ? U.last_zx : U.last_c64;
    if ((!fk || bad) && other->file_kind(path, info, sizeof(info), &other_bad) && !other_bad) {
        U.switch_to = other;
        snprintf(U.pending_load, sizeof(U.pending_load), "%s", path);
        U.pending_run = run;
        return true;
    }
    if (fk && fk->role == ROLE_ROM) {       /* cartridges and ROMs go in with the power off */
        ok = board_load(U.b, path, run, U.status, sizeof(U.status));
        if (ok) select_pin(U.b->scope_pin);
    } else if (U.b->sim.cycle < M()->ready_cycles) {
        /* Still booting: BASIC's cold start would wipe the program and
         * the RUN typed for it. Load it when BASIC is up. */
        snprintf(U.autoload, sizeof(U.autoload), "%s", path);
        U.autoload_run = run;
        snprintf(U.status, sizeof(U.status), "%s: loading when BASIC is ready", path);
        ok = true;
    } else {
        ok = board_load(U.b, path, run, U.status, sizeof(U.status));
    }
    if (ok) browser_add_recent(path);
    /* The panel is narrow: the file's name, not its whole path. */
    const char *base = base_name(path), *at = strstr(U.status, path);
    if (base != path && at) {
        char tail[sizeof(U.status)];
        snprintf(tail, sizeof(tail), "%s", at + n);
        snprintf(U.status + (at - U.status), sizeof(U.status) - (size_t)(at - U.status), "%s%s", base, tail);
    }
    return ok;
}

/* Zoom in on a fault of the fault list: its part outlined and pulsing for
 * a few seconds with what is wrong, its pin (or the legs of a bridge) on
 * the scope. */
static void show_fault(int part, int pin, int pin2, const char *what) {
    if (pin >= 0) select_pins(pin, pin2);
    U.focus_part = part;
    U.focus_pin = pin;
    U.focus_pin2 = pin2;
    snprintf(U.focus_note, sizeof(U.focus_note), "%s", what);
    U.focus_pending = true;
    U.focus_until = stm_sec(stm_now()) + 4;
}

/* How many faults the board has (the lines of the fault list). */
static int fault_count(void) {
    sim_t *s = S();
    int n = U.b->nbridges;
    for (int i = 0; i < s->npins; i++) n += s->pins[i].fault != PF_OK;
    for (int i = 0; i < s->nparts; i++)
        n += ((s->parts[i].removed && !s->parts[i].unfitted) || s->parts[i].rfault != RF_OK) + (s->parts[i].mfault != MF_OK);
    return n;
}

/* A line of the fault list, as wide as the panel leaves it next to its
 * repair button and wrapped if it doesn't fit: click it to see the fault
 * on the board. True if its repair button was clicked. */
static bool fault_line(struct nk_context *ctx, const char *line, int part, int pin, int pin2) {
    const float button_w = 64, gap = 30;
    float text_w = fmaxf(120, nk_window_get_content_region_size(ctx).x - button_w - gap);
    int lines = (int)ceilf(text_width(ctx, line) / (text_w - 10));
    lines = lines < 1 ? 1 : lines > 3 ? 3 : lines;
    float fh = ctx->style.font->height;
    nk_layout_row_begin(ctx, NK_STATIC, lines * (fh + 4) + 4, 2);
    nk_layout_row_push(ctx, text_w);
    struct nk_rect wb = nk_widget_bounds(ctx);
    const struct nk_input *in = &ctx->input;
    bool hovered = nk_input_is_mouse_hovering_rect(in, wb);
    if (hovered) {
        nk_fill_rect(nk_window_get_canvas(ctx), wb, 2, g_theme.line_hover);
        nk_tooltip(ctx, "click: show it on the board");
    }
    nk_label_wrap(ctx, line);
    if (hovered && nk_input_is_mouse_released(in, NK_BUTTON_LEFT) &&
        nk_input_has_mouse_click_in_rect(in, NK_BUTTON_LEFT, wb))
        show_fault(part, pin, pin2, line);
    nk_layout_row_push(ctx, button_w);
    bool repair = nk_button_label(ctx, "repair");
    nk_layout_row_end(ctx);
    return repair;
}

static void fault_list(struct nk_context *ctx) {
    sim_t *s = S();
    char line[200];
    int count = fault_count();
    if (g_hide_faults) {        /* practice: find them on the board */
        nk_layout_row_dynamic(ctx, 20, 1);
        snprintf(line, sizeof(line), count == 1 ? "1 fault, hidden" : "%d faults, hidden", count);
        nk_label_colored(ctx, count ? line : "none: the board is healthy", NK_TEXT_LEFT, COL_DIMTEXT);
        return;
    }
    for (int i = 0; i < s->npins; i++) {
        if (s->pins[i].fault == PF_OK) continue;
        char pd[96];
        pin_desc(U.b, i, pd, sizeof(pd));
        snprintf(line, sizeof(line), "%s: %s", pd, pin_fault_name(s->pins[i].fault));
        if (fault_line(ctx, line, s->pins[i].part, i, -1)) board_set_pin_fault(U.b, i, PF_OK);
    }
    for (int i = 0; i < U.b->nbridges; i++) {
        int pa = U.b->bridge[i][0], pb = U.b->bridge[i][1];
        snprintf(line, sizeof(line), "%s %s pins %d-%d: solder bridge", s->parts[s->pins[pa].part].ref,
                 board_part_label(U.b, s->pins[pa].part), s->pins[pa].number, s->pins[pb].number);
        if (fault_line(ctx, line, s->pins[pa].part, pa, pb)) { board_set_bridge(U.b, pa, pb, false); break; }
    }
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        bool pulled = p->removed && !p->unfitted;
        if (!pulled && p->rfault == RF_OK) continue;
        snprintf(line, sizeof(line), "%s %s: %s", p->ref, board_part_label(U.b, i),
                 pulled ? "pulled out" : p->rfault == RF_SHORT ? "shorted" :
                 !strcmp(board_part_kind_name(U.b, i), "switch") ? "switched off" : "open");
        if (fault_line(ctx, line, i, -1, -1)) {
            if (pulled) board_set_removed(U.b, i, false);
            else board_set_rfault(U.b, i, RF_OK);
        }
    }
    for (int i = 0; i < s->nparts; i++) {
        part_t *p = &s->parts[i];
        if (p->mfault == MF_OK) continue;
        char d[160];
        mem_fault_t mf = { p->mfault, p->mf_bit, p->mf_val, p->mf_addr };
        board_mem_fault_desc(U.b, i, &mf, d, sizeof(d));
        snprintf(line, sizeof(line), "%s %s: %s", p->ref, board_part_label(U.b, i), d);
        if (fault_line(ctx, line, i, -1, -1)) board_set_mem_fault(U.b, i, &(mem_fault_t){ MF_OK, 0, 0, 0 });
    }
    if (!count) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label_colored(ctx, "none: the board is healthy", NK_TEXT_LEFT, COL_DIMTEXT);
    }
}

/* What the screenshot matched best, to "Diagnose by probing". */
static void probe_take_diagnosis(void) {
    U.diag_taken = U.diag_serial;
    char labels[ATLAS_MAX_GROUPS][256];
    int n = diagnose_best_labels(labels, ATLAS_MAX_GROUPS);
    if (n) probe_screens_from_screenshot((const char (*)[256])labels, n);
}

/* A screenshot diagnosed: the results shown, and if probing is under way,
 * its faults narrowed to what the screenshot shows (behind the results). */
static void lock_board(void);
static void unlock_board(void);

/* Match a screenshot (the board must NOT be locked: matching takes a
 * moment, and reads no board; the simulation goes on meanwhile). */
static void diagnose_picture(const char *path) {
    lock_board();
    U.diag_status[0] = 0;
    int machine = bench_machine();
    unlock_board();
    diagnose_open(path, machine);
    lock_board();
    U.diag_serial++;
    if (probe_is_open()) probe_take_diagnosis();
    unlock_board();
}

/* A fault picked in a diagnosis, put on the board: the machine of the
 * picture, with no cartridge and no harness (the atlas boots without),
 * every other fault repaired, then switched off and on to watch it happen.
 * The view goes to the part, outlined; a pin goes on the scope. */
static void apply_diagnosis(int machine, const char *spec) {
    switch_machine(atlas_machine(machine));
    board_t *b = U.b;
    if (board_cart_name(b)) board_remove_cart(b);
    if (board_is_c64(b) && board_harness(b)) board_set_harness(b, false);
    board_clear_faults(b);
    char first[64] = "", bad[64] = "";
    for (const char *s = spec; *s; ) {
        size_t len = strcspn(s, " ");
        char one[64];
        snprintf(one, sizeof(one), "%.*s", (int)len, s);
        if (len && !first[0]) snprintf(first, sizeof(first), "%s", one);
        if (len && !board_apply_fault_spec(b, one)) snprintf(bad, sizeof(bad), "%s", one);
        s += len + (s[len] == ' ');
    }
    board_power_on(b);
    U.paused = U.reset_hold = false;
    U.autoload[0] = 0;
    /* Where it is. */
    char ref[16] = "", pad[8] = "", pad2[8] = "", what[16] = "";
    if (sscanf(first, "%15[^.:].%7[^-:]-%7[^:]:%15s", ref, pad, pad2, what) != 4) {
        pad2[0] = 0;
        if (sscanf(first, "%15[^.:].%7[^:]:%15s", ref, pad, what) != 3) {
            pad[0] = 0;
            if (sscanf(first, "%15[^:]:%15s", ref, what) != 2) ref[0] = 0;
        }
    }
    U.focus_part = ref[0] ? sim_find_part(S(), ref) : -1;
    U.focus_pin = U.focus_part >= 0 && pad[0] ? board_pad_pin(b, U.focus_part, pad) : -1;
    U.focus_pin2 = U.focus_part >= 0 && pad2[0] ? board_pad_pin(b, U.focus_part, pad2) : -1;
    /* A bridge: both legs on the scope, to see them fight. */
    if (U.focus_pin >= 0 && U.focus_pin2 >= 0) select_pins(U.focus_pin, U.focus_pin2);
    else select_pins(U.focus_pin >= 0 ? U.focus_pin : b->scope_pin, -1);
    U.focus_note[0] = 0;
    if (U.focus_part >= 0 && U.focus_pin >= 0 && U.focus_pin2 >= 0) {
        board_bridge_desc(b, U.focus_pin, U.focus_pin2, U.focus_note, sizeof(U.focus_note));
        U.focus_pending = true;
        U.focus_until = stm_sec(stm_now()) + 30;
    } else if (U.focus_part >= 0) {
        U.focus_pin2 = -1;
        const char *how = !strcmp(what, "removed") ? "pulled out" : !strcmp(what, "open") ? "open"
                        : !strcmp(what, "gnd") ? "shorted to GND" : !strcmp(what, "vcc") ? "shorted to +5V"
                        : !strcmp(what, "short") ? "shorted" : !strcmp(what, "out0") ? pin_fault_name(PF_OUT0)
                        : !strcmp(what, "out1") ? pin_fault_name(PF_OUT1) : what;
        char mem_how[120];
        mem_fault_t mf;
        if (U.focus_pin < 0 && board_parse_mem_fault(b, U.focus_part, what, &mf) && mf.kind != MF_OK) {
            board_mem_fault_desc(b, U.focus_part, &mf, mem_how, sizeof(mem_how));
            how = mem_how;
        }
        const char *pname = U.focus_pin >= 0 ? sim_pin_name(S(), U.focus_pin) : NULL;
        if (U.focus_pin >= 0)
            snprintf(U.focus_note, sizeof(U.focus_note), "%s %s pin %s%s%s%s: %s", ref, board_part_label(b, U.focus_part),
                     pad, pname ? " (" : "", pname ? pname : "", pname ? ")" : "", how);
        else snprintf(U.focus_note, sizeof(U.focus_note), "%s %s: %s", ref, board_part_label(b, U.focus_part), how);
        U.focus_pending = true;
        U.focus_until = stm_sec(stm_now()) + 30;
    }
    if (bad[0]) snprintf(U.diag_status, sizeof(U.diag_status), "Couldn't apply %s on the %s", bad, M()->name);
    else if (!spec[0]) snprintf(U.diag_status, sizeof(U.diag_status), "Repaired, %s power cycled", M()->name);
    else snprintf(U.diag_status, sizeof(U.diag_status), "Applied %s, power cycled%s", spec,
                  U.focus_pin2 >= 0 ? "; both legs on the scope" : U.focus_pin >= 0 ? "; pin on the scope" : "");
    diagnose_close();
}

/* "Show on board" in Diagnose by probing: zoom in on a pin, on the scope. */
static void show_pin(int pin) {
    select_pin(pin);
    U.focus_part = S()->pins[pin].part;
    U.focus_pin = pin;
    U.focus_pin2 = -1;
    U.focus_note[0] = 0;
    U.focus_pending = true;
    U.focus_until = stm_sec(stm_now()) + 3;
}

static void legend_item(struct nk_context *ctx, struct nk_color c, const char *txt) {
    nk_layout_row_push(ctx, 12);
    struct nk_rect r;
    if (nk_widget(&r, ctx) != NK_WIDGET_INVALID)
        nk_fill_rect(nk_window_get_canvas(ctx), nk_rect(r.x, r.y + 4, 10, 10), 0, c);
    nk_layout_row_push(ctx, 72);
    nk_label(ctx, txt, NK_TEXT_LEFT);
}

/* The help window: how to use the board, opened by the Help button. */
static void help_head(struct nk_context *ctx, const char *txt) {
    nk_layout_row_dynamic(ctx, 8, 1);
    nk_spacing(ctx, 1);
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label_colored(ctx, txt, NK_TEXT_LEFT, COL_HOVER);
}

static void help_line(struct nk_context *ctx, const char *txt) {
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label(ctx, txt, NK_TEXT_LEFT);
}

static void help_color(struct nk_context *ctx, struct nk_color c, const char *txt) {
    nk_layout_row_begin(ctx, NK_STATIC, 18, 2);
    nk_layout_row_push(ctx, 16);
    struct nk_rect r;
    if (nk_widget(&r, ctx) != NK_WIDGET_INVALID)
        nk_fill_rect(nk_window_get_canvas(ctx), nk_rect(r.x + 2, r.y + 4, 10, 10), 0, c);
    nk_layout_row_push(ctx, 600);
    nk_label(ctx, txt, NK_TEXT_LEFT);
    nk_layout_row_end(ctx);
}

static void help_window(struct nk_context *ctx, float bw, float h) {
    static bool shown;
    if (!U.help_open) { shown = false; return; }
    float ww = 660, wh = fminf(h - 80, 700);
    struct nk_rect r = nk_rect(fmaxf((bw - ww) / 2, 0), 40, ww, wh);
    if (!shown) {           /* opened (again): in the middle of the board, on top */
        nk_window_show(ctx, "Help", NK_SHOWN);
        nk_window_set_bounds(ctx, "Help", r);
        nk_window_set_focus(ctx, "Help");
        shown = true;
    }
    if (nk_begin(ctx, "Help", r, NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE)) {
        U.help_bounds = nk_window_get_bounds(ctx);
        help_head(ctx, "Credits");
        help_line(ctx, "Based on c64-hw-simulator by Salvatore Sanfilippo (antirez), the original");
        help_line(ctx, "  project: github.com/antirez/c64-hw-simulator");
        help_line(ctx, "ZX Spectrum board layouts (Issues 1, 2, 3B, 4B and 6A) from the KiCad projects");
        help_line(ctx, "  of liveboxandy: github.com/liveboxandy");
        help_line(ctx, "Chip cores by floooh, C64 boards by bwack, light theme by Henry; more in the README.");
        help_head(ctx, "The board");
        help_line(ctx, "Mouse wheel: zoom around the pointer.  Drag: move the board.");
        help_line(ctx, "Double-click the bare board: fit the whole board in the view again.");
        help_line(ctx, "Zoom in on a chip for its pin numbers, then its gates or pin names.");
        help_line(ctx, "Point at a pad or a part: what it is, its net and level (under the controls).");
        help_head(ctx, "Breaking things");
        help_line(ctx, "Click a pad: it goes on the oscilloscope, and a menu lets you make it");
        help_line(ctx, "  float (a lifted leg), short it to GND or +5V, or bridge it to a neighbor.");
        help_line(ctx, "  An output can also die inside the chip, stuck low or high: on a bus the");
        help_line(ctx, "  other chips fight it (a short to GND or +5V always wins).");
        help_line(ctx, "Click a chip: pull it out of its socket, or open the inspector");
        help_line(ctx, "  (its pins, gates and live states), or see the repair records with it faulty.");
        help_line(ctx, "A ROM or RAM chip: \"Inside the chip\" breaks it inside: a dead data bit (a");
        help_line(ctx, "  whole 4164), a dead address line, one bad cell or byte, or weak DRAM cells.");
        help_line(ctx, "Click a resistor, a ferrite bead, a capacitor or the crystal: open or short it.");
        help_line(ctx, "Faults (in the controls) lists them all: repair one there, or Repair all.");
        help_line(ctx, "  Click a fault in the list to zoom in on it. A part with a fault has a red");
        help_line(ctx, "  outline and a \"!\" badge; point at it to read its faults. \"Hide fault");
        help_line(ctx, "  markers\" hides all that, to practise finding a fault like on a real board.");
        help_head(ctx, "Continuity (beep) test");
        help_line(ctx, "Shift-click a pad (or middle-click it): what it is joined to, like the beep");
        help_line(ctx, "  of a meter. Click the bare board to stop. Solder bridges and pins shorted");
        help_line(ctx, "  to GND or +5V beep too.");
        help_color(ctx, COL_BEEP, "yellow: beeps (copper, links)");
        help_color(ctx, COL_THRU, "cyan: through a resistor or a diode");
        help_color(ctx, COL_CAP, "violet: a capacitor (not followed further)");
        help_color(ctx, COL_FAULT, "orange: broken open");
        help_head(ctx, "Pad colors (a logic probe)");
        help_color(ctx, COL_HIGH, "high");
        help_color(ctx, COL_LOW, "low");
        help_color(ctx, COL_PULSE, "pulsing");
        help_color(ctx, COL_FLOAT, "floating: nothing drives it");
        help_color(ctx, COL_FIGHT, "fight: driven high and low at once");
        help_head(ctx, "The machine");
        help_line(ctx, "Power cycle: off and on again (the faults stay).  Pause: stop the clock.");
        help_line(ctx, "RESET: hold the button, or tick \"keep it pressed\".");
        help_line(ctx, "Warp: run as fast as the PC can (no effect below 100% speed).");
        help_line(ctx, "Save state: keep the machine as it is now, faults included (one slot, in");
        help_line(ctx, "  memory until you quit). Load state: back to it at once, faults and all.");
        help_line(ctx, "  It doesn't load after a cartridge, ROM or the harness went in or out.");
        help_line(ctx, "The keyboard types on the machine's keyboard.");
        help_line(ctx, M()->keys_help);
        help_line(ctx, "Ctrl+O, or drop a file on the window: load a program, cartridge or snapshot.");
        help_line(ctx, "Save to file: the board as it is, faults, cartridge and repair notes, in a .hwstate file.");
        help_line(ctx, "The mouse wheel scrolls the window under the mouse. Esc closes the window on top.");
        help_line(ctx, "Interface stuck? Press F12: what it was doing goes into ui-debug.log.");
        help_line(ctx, "Drop a screenshot (.png, .jpg, .bmp): diagnose what could cause it.");
        help_head(ctx, "Diagnose by probing (a real broken machine)");
        help_line(ctx, "Switch the real machine on, wait a few seconds, and pick what its screen shows.");
        help_line(ctx, "Probe the pin the window asks for (ringed in cyan on the board), on the solder");
        help_line(ctx, "  side, and click what your logic probe shows. Each reading narrows the list.");
        help_line(ctx, "Click any pin on the board, or type one (U7.3), to probe that one instead.");
        help_line(ctx, "Apply & Inspect puts a suspect on the board here, to compare with yours.");
        help_head(ctx, "Browse the atlas");
        help_line(ctx, "Every picture the fault atlas has of this board, most causes first. Narrow");
        help_line(ctx, "  them down by kind, border and screen color, behavior, CPU state or chip:");
        help_line(ctx, "  each choice says how many pictures it leaves. Probe readings too: type a");
        help_line(ctx, "  pin (IC2.26), pick what the probe shows, Add. Click a picture for its");
        help_line(ctx, "  causes: Apply & Inspect one, or narrow them down by probing.");
    } else {
        U.help_open = false;    /* closed */
    }
    nk_end(ctx);
}

/* Which board of the machine (the Spectrum's issues, the C64's
 * assemblies), and how it is built (machine_option_t): a drop-down each. A
 * new board goes on the bench after the frame; an option restarts the
 * machine. */
static void board_choice(struct nk_context *ctx, board_t *b) {
    static const machine_t *const zx_boards[] = { &machine_zx48i1, &machine_zx48i2, &machine_zx48,
                                                  &machine_zx48i4b, &machine_zx48i6a };
    static const machine_t *const c64_boards[] = { &machine_c64, &machine_c64c };
    const machine_t *m = b->m;
    if (!m->issue) return;
    bool zx = board_is_zx48(b);
    const machine_t *const *boards = zx ? zx_boards : c64_boards;
    int nb = zx ? (int)(sizeof(zx_boards) / sizeof(zx_boards[0])) : (int)(sizeof(c64_boards) / sizeof(c64_boards[0]));
    nk_layout_row_dynamic(ctx, 24, 1 + m->noptions < 3 ? 1 + m->noptions : 3);   /* more go on a row below */
    char names[8][32];
    const char *issues[8];
    int cur = 0;
    for (int i = 0; i < nb; i++) {
        snprintf(names[i], sizeof(names[i]), zx ? "%s" : "ASSY %s", boards[i]->issue);
        issues[i] = names[i];
        if (boards[i] == m) cur = i;
    }
    if (nk_widget_is_hovered(ctx))
        nk_tooltip(ctx, zx ? "Which issue of the board is on the bench" :
                             "Which board is on the bench: the 250407 (breadbin) or the 250469 (C64C, Super PLA)");
    int pick = nk_combo(ctx, issues, nb, cur, 20, nk_vec2(140, 20.0f * nb + 20));
    if (pick != cur) U.switch_to = boards[pick];
    for (int o = 0; o < m->noptions; o++) {
        const machine_option_t *opt = &m->options[o];
        char text[8][32];
        const char *items[8];
        int n = opt->nvalues < 8 ? opt->nvalues : 8;
        for (int v = 0; v < n; v++) {
            snprintf(text[v], sizeof(text[v]), "%s %s", opt->name, opt->values[v]);
            items[v] = text[v];
        }
        int was = m->get_option(b, o);
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, opt->help);
        int now = nk_combo(ctx, items, n, was, 20, nk_vec2(170, 20.0f * n + 20));
        if (now != was) {
            m->set_option(b, o, now);
            select_pin(b->scope_pin);
        }
    }
}

static void controls_window(struct nk_context *ctx, float x, float y, float h) {
    struct nk_rect r = nk_rect(x, y, RIGHT_W, h);
    nk_window_set_bounds(ctx, "Controls", r);
    if (nk_begin(ctx, "Controls", r, NK_WINDOW_TITLE | NK_WINDOW_BORDER)) {
        board_t *b = U.b;
        /* The machine on the bench: a click puts the other one there (the
         * Spectrum board last on the bench). */
        const machine_t *const machines[] = { U.last_c64, U.last_zx };
        nk_layout_row_dynamic(ctx, 24, 2);
        for (int i = 0; i < 2; i++) {
            bool cur = board_is_zx48(b) == (i == 1);
            if (nk_widget_is_hovered(ctx) && !cur)
                nk_tooltip(ctx, "Put this machine's board on the bench (the other one is switched off)");
            if (nk_select_label(ctx, machines[i]->name, NK_TEXT_CENTERED, cur) && !cur) U.switch_to = machines[i];
        }
        board_choice(ctx, b);
        nk_layout_row_dynamic(ctx, 26, 4);
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Switch the machine off and on again: a restart (the faults stay)");
        if (nk_button_label(ctx, "Power cycle")) { board_power_on(b); select_pin(b->scope_pin); }
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Undo every fault");
        if (nk_button_label(ctx, "Repair all")) board_clear_faults(b);
        if (nk_button_label(ctx, U.paused ? "Resume" : "Pause")) U.paused = !U.paused;
        nk_bool warp = U.warp;
        nk_checkbox_label(ctx, "Warp", &warp);
        U.warp = warp;
        /* RESET: pressed while the mouse holds it, or kept pressed. It
         * shorts /RESET to GND on the user port, like a reset cartridge:
         * the CPU, the CIAs and the SID reset, the VIC (no reset pin) and
         * the RAM don't. */
        nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
        nk_layout_row_push(ctx, 140);
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, M()->reset_help);
        nk_button_push_behavior(ctx, NK_BUTTON_REPEATER);
        bool down = nk_button_label(ctx, b->reset_button ? "RESET pressed" : "RESET");
        nk_button_pop_behavior(ctx);
        nk_layout_row_push(ctx, 150);
        nk_bool hold = U.reset_hold;
        nk_checkbox_label(ctx, "keep it pressed", &hold);
        U.reset_hold = hold;
        nk_layout_row_push(ctx, 140);
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "How to use the board: mouse, faults, colors, keys");
        if (nk_button_label(ctx, "Help")) U.help_open = !U.help_open;
        nk_layout_row_end(ctx);
        if ((down || U.reset_hold) != b->reset_button) board_set_reset_button(b, down || U.reset_hold);
        /* The save state slot: one, in memory. */
        nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
        nk_layout_row_push(ctx, 140);
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Keep the machine as it is now, faults and all (one slot, until you quit)");
        if (nk_button_label(ctx, "Save state")) {
            board_free_state(U.saved);
            U.saved = board_save_state(b);
            snprintf(U.saved_note, sizeof(U.saved_note), "saved at %.2f s", (double)b->sim.cycle / M()->cpu_hz);
        }
        nk_layout_row_push(ctx, 140);
        if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Put the machine back as it was saved, faults and all");
        if (nk_button_label(ctx, "Load state") && U.saved) {
            if (board_load_state(b, U.saved)) select_pin(b->scope_pin);
            else snprintf(U.saved_note, sizeof(U.saved_note), "can't load: plugged in since");
        }
        nk_layout_row_push(ctx, 160);
        nk_label_colored(ctx, U.saved ? U.saved_note : "nothing saved", NK_TEXT_LEFT, COL_DIMTEXT);
        nk_layout_row_end(ctx);
        /* Save files, and the notes kept in them. */
        nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
        nk_layout_row_push(ctx, 140);
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, "Save the machine as it is now to a file, faults, cartridge and notes too");
        if (nk_button_label(ctx, "Save to file...")) U.want_save_file = true;
        nk_layout_row_push(ctx, 140);
        if (nk_widget_is_hovered(ctx))
            nk_tooltip(ctx, "Load a saved board (or drop a .hwstate file on the window)");
        if (nk_button_label(ctx, "Load file...")) U.want_open_file = true;
        nk_layout_row_end(ctx);
        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label_colored(ctx, "Repair notes (kept in the save file):", NK_TEXT_LEFT, COL_DIMTEXT);
        nk_layout_row_dynamic(ctx, 84, 1);
        nk_flags nev = nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX, U.notes, sizeof(U.notes), nk_filter_default);
        U.notes_typing = (nev & NK_EDIT_ACTIVE) != 0;
        /* A ROM of several banks (DesTestSwitch): its switch, at hand
         * like RESET. Flipping it switches the machine off and on. */
        int nbanks = board_cart_banks(b);
        if (nbanks > 1) {
            int cur = board_cart_bank(b), cols = nbanks < 4 ? nbanks : 4;
            nk_layout_row_dynamic(ctx, 22, cols + 1);
            for (int k = 0; k < nbanks; k++) {
                if (k % cols == 0) nk_label_colored(ctx, k ? "" : "ROM bank:", NK_TEXT_LEFT, COL_SOCKET);
                char lab[16], t[128];
                snprintf(lab, sizeof(lab), "Bank %d", k);
                snprintf(t, sizeof(t), "Flip the switch to bank %d (the machine restarts): %s", k,
                         board_cart_bank_info(b, k));
                if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, t);
                if (nk_select_label(ctx, lab, NK_TEXT_CENTERED, k == cur) && k != cur) {
                    board_set_cart_bank(b, k);
                    select_pin(b->scope_pin);
                }
            }
        }
        char info[200];
        snprintf(info, sizeof(info), "speed %.0f%%   time %.2f s   CPU %s",
                 U.speed * 100, (double)b->sim.cycle / M()->cpu_hz, M()->cpu_status(b));
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, info, NK_TEXT_LEFT);

        /* What is under the mouse. */
        char hover[220] = "";
        if (U.hover_pin >= 0) {
            char pd[96];
            pin_desc(U.b, U.hover_pin, pd, sizeof(pd));
            int net = S()->pins[U.hover_pin].net;
            snprintf(hover, sizeof(hover), "%s  net %s  %s", pd, pin_net_name(U.b, U.hover_pin),
                     net >= 0 ? state_name(U.net_state[net]) : "");
        } else if (U.hover_part >= 0) {
            const part_t *hp = &S()->parts[U.hover_part];
            if (hp->kind == PART_IC)
                snprintf(hover, sizeof(hover), "%s %s: %s", hp->ref, board_part_label(b, U.hover_part),
                         board_part_detail(b, U.hover_part));
            else
                snprintf(hover, sizeof(hover), "%s  %s (click to break it)", hp->ref,
                         board_part_label(b, U.hover_part));
        }
        /* Two lines, so a long pin description wraps instead of being cut. */
        nk_layout_row_dynamic(ctx, 34, 1);
        if (hover[0]) nk_label_colored_wrap(ctx, hover, COL_HOVER);
        else nk_label_colored(ctx, "point at a pad or a part for its details",
                              NK_TEXT_LEFT, COL_DIMTEXT);

        nk_layout_row_begin(ctx, NK_STATIC, 18, 10);
        legend_item(ctx, COL_HIGH, "high");
        legend_item(ctx, COL_LOW, "low");
        legend_item(ctx, COL_PULSE, "pulsing");
        legend_item(ctx, COL_FLOAT, "floating");
        legend_item(ctx, COL_FIGHT, "fight");
        nk_layout_row_end(ctx);

        /* The count in the heading, in red: seen even with the list closed. */
        int nfaults = fault_count();
        char fhead[48];
        snprintf(fhead, sizeof(fhead), nfaults ? "Faults (%d)" : "Faults", nfaults);
        if (nfaults) nk_style_push_color(ctx, &ctx->style.tab.text, ink(COL_FAULT));
        bool fopen = nk_tree_push(ctx, NK_TREE_TAB, fhead, NK_MAXIMIZED);
        if (nfaults) nk_style_pop_color(ctx);
        if (fopen) {
            nk_layout_row_dynamic(ctx, 20, 1);
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "Practice: the board, the inspector and this list stop showing what is broken. "
                                "Find it with the probe and the scope, like on a real board");
            nk_bool hide = g_hide_faults;
            if (nk_checkbox_label(ctx, "Hide fault markers (practice)", &hide)) g_hide_faults = hide;
            fault_list(ctx);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Diagnose", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 26, 1);
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "Probe the real broken machine with a logic probe: the faults that fit your readings, "
                                "and which pin to probe next");
            nk_layout_row_dynamic(ctx, 26, 2);
            if (nk_button_label(ctx, "Diagnose by probing...")) {
                /* A screenshot diagnosed since: the screen it shows. */
                if (diagnose_has_results() && U.diag_taken != U.diag_serial) probe_take_diagnosis();
                probe_open();
            }
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "Look through every picture the fault atlas has of this board, narrowed by what the "
                                "screen shows (kind, colors, behavior...), and the faults behind each");
            if (nk_button_label(ctx, "Browse the atlas...")) U.want_browse = true;
            nk_layout_row_dynamic(ctx, 26, 2);
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "Pick a screenshot of the broken machine's screen (PNG, JPG or BMP, from a USB capture "
                                "card): the faults that give that picture, ready to put on the board");
            if (nk_button_label(ctx, "Diagnose screenshot...")) U.want_pick = true;
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "Diagnose the picture on the clipboard (Ctrl+V): a screenshot copied from the capture "
                                "program, or a picture file copied in the file manager");
            if (nk_button_label(ctx, "Paste screenshot")) U.want_paste = true;
            if (diagnose_has_results()) {
                nk_layout_row_dynamic(ctx, 26, 1);
                if (nk_button_label(ctx, "Show the last diagnosis")) diagnose_show();
            }
            nk_layout_row_dynamic(ctx, 26, 1);
            if (nk_widget_is_hovered(ctx))
                nk_tooltip(ctx, "Repairs of real machines you (or others) recorded: the screenshot and the chips "
                                "that were faulty. Look, leave out, edit, delete, merge, export");
            if (nk_button_label(ctx, "Repair records...")) diagnose_records_open(bench_machine());
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label_colored(ctx, U.diag_status[0] ? U.diag_status : "or drop a .png, .jpg or .bmp screenshot on the window, or Ctrl+V",
                             NK_TEXT_LEFT, U.diag_status[0] ? COL_FAULT : COL_DIMTEXT);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Programs and cartridges", NK_MAXIMIZED)) {
            /* The loopback harness of the 586220 diagnostic cartridge. */
            if (board_is_c64(b)) {
                nk_layout_row_dynamic(ctx, 22, 1);
                if (nk_widget_is_hovered(ctx))
                    nk_tooltip(ctx, "Dongles on the user, cassette, serial, control ports and keyboard that loop "
                                    "them back, for the Diagnostic 586220 cartridge (the C64 is switched off and on)");
                nk_bool harness = board_harness(b);
                if (nk_checkbox_label(ctx, "586220 test harness plugged in", &harness) && harness != board_harness(b)) {
                    board_set_harness(b, harness);
                    select_pin(b->scope_pin);
                }
            }
            const char *cart = board_cart_name(b);
            if (cart) {
                char line[96], slot[32] = "";
                int sp = M()->cart_slot ? sim_find_part(S(), M()->cart_slot) : -1;
                const char *cname = sp >= 0 ? board_connector_name(b, sp) : NULL;
                for (int k = 0; cname && cname[k] && k < 31; k++) slot[k] = (char)tolower((unsigned char)cname[k]);
                if (board_is_c64(b) && c64_cart_in_u4(b)) snprintf(line, sizeof(line), "ROM in the KERNAL socket U4: %s", cart);
                else snprintf(line, sizeof(line), "Cartridge in the %s: %s", slot[0] ? slot : "slot", cart);
                nk_layout_row_begin(ctx, NK_STATIC, 22, 2);
                nk_layout_row_push(ctx, 330);
                nk_label_colored(ctx, line, NK_TEXT_LEFT, COL_SOCKET);
                nk_layout_row_push(ctx, 80);
                if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Pull the cartridge out (the C64 is switched off and on)");
                if (nk_button_label(ctx, "remove")) { board_remove_cart(b); select_pin(b->scope_pin); }
                nk_layout_row_end(ctx);
            }
            /* An 8K raw ROM image: in the expansion port or in place of the
             * KERNAL. */
            int socket = board_is_c64(b) ? c64_cart_socket(b) : -1;
            if (socket >= 0) {
                static const char *const where[2] = { "Cartridge (Ultimax / 8K)", "U4 KERNAL socket" };
                static const char *const tip[2] = {
                    "Plug the ROM into the expansion port, wired as it asks (Ultimax at $E000 or 8K at $8000)",
                    "Put the ROM in the KERNAL socket U4, in place of the KERNAL (the cartridge port stays empty)" };
                nk_layout_row_dynamic(ctx, 22, 2);
                for (int k = 0; k < 2; k++) {
                    if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, tip[k]);
                    if (nk_select_label(ctx, where[k], NK_TEXT_CENTERED, socket == k) && socket != k) {
                        c64_set_cart_socket(b, k == 1);
                        select_pin(b->scope_pin);
                    }
                }
            }
            nk_layout_row_dynamic(ctx, 26, 1);
            if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, "Browse for a file to load (Ctrl+O or Cmd+O)");
            if (nk_button_label(ctx, M()->files_title)) browser_open();
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label_colored(ctx, U.status[0] ? U.status : "or drop a file on the window",
                             NK_TEXT_LEFT, COL_DIMTEXT);
            /* The recent ones, one click away. */
            if (browser_recent_count()) nk_label_colored(ctx, "recent (click to load again):", NK_TEXT_LEFT, COL_DIMTEXT);
            for (int i = 0; i < browser_recent_count(); i++) {
                const char *path = browser_recent(i);
                char label[300];
                snprintf(label, sizeof(label), "  %s", base_name(path));
                if (nk_widget_is_hovered(ctx)) nk_tooltip(ctx, path);
                if (nk_select_label(ctx, label, NK_TEXT_LEFT, 0)) {
                    char copy[4096];
                    snprintf(copy, sizeof(copy), "%s", path);
                    load_prg(copy, true);
                    break;
                }
            }
            nk_tree_pop(ctx);
        }
        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label_colored(ctx, M()->keys_help, NK_TEXT_LEFT, COL_DIMTEXT);
        /* Light or dark windows. */
        static const char *const theme_labels[] = { "As the system", "Light", "Dark" };
        nk_layout_row_begin(ctx, NK_STATIC, 22, 4);
        nk_layout_row_push(ctx, 60);
        nk_label(ctx, "Theme:", NK_TEXT_LEFT);
        for (int i = 0; i < 3; i++) {
            nk_layout_row_push(ctx, i ? 80 : 130);
            if (nk_select_label(ctx, theme_labels[i], NK_TEXT_CENTERED, theme_choice == i) && theme_choice != i)
                theme_choice = i;
        }
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
}

/* ------------------------------------------------------------------------ */
/* The simulation thread                                                     */
/* ------------------------------------------------------------------------ */

/* The simulation runs in its own thread, so it can use a whole CPU core.
 * The interface takes the lock only while it looks at or changes the
 * board; when it is waiting for the lock the simulation steps aside. */
static pthread_t sim_thread;
static pthread_mutex_t sim_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool ui_waiting, sim_quit;

static void lock_board(void) {
    atomic_store(&ui_waiting, true);
    pthread_mutex_lock(&sim_lock);
    atomic_store(&ui_waiting, false);
}

static void unlock_board(void) { pthread_mutex_unlock(&sim_lock); }

/* ---- Watching the interface ---------------------------------------------
 * What the interface thread is doing (STAGE, since when), and how long its
 * frames take. A thread of its own watches it (not the simulation's,
 * which waits on the board lock the interface may be holding): a stage
 * that lasts over
 * STALL_SECS is written to ui-debug.log, with how long it lasted once
 * over. F12 writes the windows' state there too (when the interface seems
 * stuck but still draws). --ui-stats prints the frame times every second. */
enum { STAGE_IDLE, STAGE_FRAME_LOCK, STAGE_BUILD, STAGE_RENDER, STAGE_EVENT_LOCK, STAGE_EVENT, STAGE_DIALOG,
       NSTAGES };
static const char *const stage_names[NSTAGES] = {
    "between frames (the window system isn't asking for frames)", "frame: waiting for the board lock",
    "frame: building the interface", "frame: rendering (and waiting for the screen)",
    "event: waiting for the board lock", "event: handling it", "in the file dialog" };
#define STALL_SECS 1.5
#define UI_LOG "ui-debug.log"
static atomic_int ui_stage;
static _Atomic double ui_stage_since;
static bool ui_stats_print;
static struct {
    double lock, build, render, interval;   /* this second's worst, ms */
    double sum_interval; int frames;
    double last_frame, second_start;
    char last_second[160];                  /* the last second's summary */
} FS;

static void set_stage(int stage) {
    atomic_store(&ui_stage_since, stm_sec(stm_now()));
    atomic_store(&ui_stage, stage);
}

static void ui_log(const char *fmt, ...) {
    FILE *f = fopen(UI_LOG, "a");
    if (!f) return;
    time_t now = time(NULL);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "%s  ", when);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* From the watching thread, 4 times a second: a stage lasting too long. */
static void watch_ui(void) {
    static int stalled = -1;
    static double stalled_since;
    int stage = atomic_load(&ui_stage);
    double since = atomic_load(&ui_stage_since), now = stm_sec(stm_now());
    if (since == 0) return;
    if (stalled >= 0 && (stage != stalled || since != stalled_since)) {
        ui_log("interface stall over: %s, %.1f s", stage_names[stalled], now - stalled_since);
        stalled = -1;
    }
    if (stalled < 0 && now - since > STALL_SECS) {
        stalled = stage;
        stalled_since = since;
        ui_log("interface stalled %.1f s so far: %s (last second: %s)", now - since, stage_names[stage],
               FS.last_second);
    }
}

/* A frame's times, ms. */
static void frame_times(double lock, double build, double render) {
    double now = stm_sec(stm_now());
    double interval = FS.last_frame ? (now - FS.last_frame) * 1000 : 0;
    FS.last_frame = now;
    if (lock > FS.lock) FS.lock = lock;
    if (build > FS.build) FS.build = build;
    if (render > FS.render) FS.render = render;
    if (interval > FS.interval) FS.interval = interval;
    FS.sum_interval += interval;
    FS.frames++;
    if (now - FS.second_start >= 1.0) {
        snprintf(FS.last_second, sizeof(FS.last_second),
                 "%d frames, %.1f ms apart (worst %.0f); worst lock wait %.1f, build %.1f, render %.1f ms",
                 FS.frames, FS.sum_interval / FS.frames, FS.interval, FS.lock, FS.build, FS.render);
        if (ui_stats_print) fprintf(stderr, "ui: %s\n", FS.last_second);
        FS.lock = FS.build = FS.render = FS.interval = FS.sum_interval = 0;
        FS.frames = 0;
        FS.second_start = now;
    }
}

/* F12: the windows as nuklear has them, and the mouse buttons as it
 * thinks they are. */
static void dump_ui_state(struct nk_context *ctx) {
    FILE *f = fopen(UI_LOG, "a");
    if (!f) return;
    time_t now = time(NULL);
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(f, "%s  F12: interface state\n  %s\n", when, FS.last_second);
    fprintf(f, "  mouse at %.0f,%.0f; buttons held (as nuklear thinks): left %d right %d middle %d\n",
            ctx->input.mouse.pos.x, ctx->input.mouse.pos.y, ctx->input.mouse.buttons[NK_BUTTON_LEFT].down,
            ctx->input.mouse.buttons[NK_BUTTON_RIGHT].down, ctx->input.mouse.buttons[NK_BUTTON_MIDDLE].down);
    int n = 0;
    for (struct nk_window *w = ctx->begin; w && n < 64; w = w->next, n++)
        fprintf(f, "  window %-14s %4.0f,%4.0f %4.0fx%-4.0f%s%s%s%s%s%s%s\n", w->name_string, w->bounds.x,
                w->bounds.y, w->bounds.w, w->bounds.h, w == ctx->active ? " ACTIVE" : "",
                w == ctx->end ? " END" : "", (w->flags & NK_WINDOW_ROM) ? " read-only" : "",
                (w->flags & NK_WINDOW_HIDDEN) ? " hidden" : "", (w->flags & NK_WINDOW_CLOSED) ? " closed" : "",
                (w->flags & NK_WINDOW_BACKGROUND) ? " background" : "",
                w->popup.active ? " popup-open" : "");
    if (ctx->end && ctx->end->next) fprintf(f, "  BROKEN: the window list goes on past its end\n");
    fclose(f);
}

#define SIM_CHUNK 1000      /* cycles run while holding the lock */

static pthread_t watch_thread;

static void *watch_main(void *arg) {
    (void)arg;
    while (!atomic_load(&sim_quit)) {
        watch_ui();
        usleep(250000);
    }
    return NULL;
}

static void *sim_main(void *arg) {
    (void)arg;
#ifdef _WIN32
    /* This thread does all the heavy lifting (see SIM_CHUNK below): keep
     * it off an Efficiency core, see win_perf.h. */
    win_boost_current_thread();
#endif
    double ref_time = stm_sec(stm_now());
    uint64_t ref_cycle = 0;
    int frame_cycles = 0;
    while (!atomic_load(&sim_quit)) {
        pthread_mutex_lock(&sim_lock);
        bool running = !U.paused, warp = U.warp;
        if (running) {
            board_run(U.b, SIM_CHUNK);
            /* Once per emulated video frame: keyboard housekeeping. */
            if ((frame_cycles += SIM_CHUNK) >= U.b->m->cycles_per_frame) {
                frame_cycles -= U.b->m->cycles_per_frame;
                board_frame_update(U.b, 20000);
            }
        }
        uint64_t cycle = U.b->sim.cycle;
        double hz = U.b->m->cpu_hz;
        pthread_mutex_unlock(&sim_lock);
        while (atomic_load(&ui_waiting)) sched_yield();
        /* Never run faster than a real C64 (unless warp), never try to
         * catch up when slower. Far ahead is the clock jumping (a save
         * state loaded), not running fast: start over from there. */
        double now = stm_sec(stm_now());
        if (!running) { usleep(10000); ref_time = now; ref_cycle = cycle; continue; }
        double ahead = (double)(cycle - ref_cycle) / hz - (now - ref_time);
        if (warp || cycle < ref_cycle || ahead < -0.05 || ahead > 0.1) { ref_time = now; ref_cycle = cycle; }
        else if (ahead > 0.002) usleep((useconds_t)(ahead * 1e6));
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Frame                                                                     */
/* ------------------------------------------------------------------------ */

/* Speed of the simulation (fraction of a real C64), and the program to
 * load once BASIC is ready. */
static void update_status(void) {
    static uint64_t last_cycle;
    double now = stm_sec(stm_now());
    double dt = U.last_time ? now - U.last_time : 0;
    uint64_t cycle = U.b->sim.cycle;
    if (dt > 0 && cycle >= last_cycle) {
        double sp = U.paused ? 0 : (double)(cycle - last_cycle) / (dt * M()->cpu_hz);
        U.speed = U.speed * 0.9 + sp * 0.1;
    }
    U.last_time = now;
    last_cycle = cycle;
    if (U.autoload[0] && cycle >= M()->ready_cycles) {
        char path[sizeof(U.autoload)];
        snprintf(path, sizeof(path), "%s", U.autoload);
        U.autoload[0] = 0;
        load_prg(path, U.autoload_run);
    }
}

void ui_frame(void) {
    double t0 = stm_sec(stm_now());
    set_stage(STAGE_FRAME_LOCK);
    lock_board();
    set_stage(STAGE_BUILD);
    double t1 = stm_sec(stm_now());
    update_status();
    /* Logic probe state for the pin colors, from this frame's samples. */
    for (int i = 0; i < S()->nnets; i++) U.net_state[i] = (uint8_t)board_net_state(U.b, i);

    struct nk_context *ctx = snk_new_frame();
    apply_theme(ctx);
    /* Make checked checkboxes visible with the default style. */
    ctx->style.checkbox.cursor_normal = nk_style_item_color(ink(COL_TRACE));
    ctx->style.checkbox.cursor_hover = nk_style_item_color(ink(COL_TRACE));
    float W = sapp_widthf(), H = sapp_heightf();
    float bw = W - RIGHT_W;
    board_window(ctx, bw, H);
    monitor_window(ctx, bw, 0);
    float y = MON_H + 44;
    float ctrl_h = H - y - SCOPE_H;
    controls_window(ctx, bw, y, ctrl_h);
    scope_window(ctx, bw, y + ctrl_h, SCOPE_H);
    int clicked = inspect_window(ctx, U.b, U.net_state, nk_rect(0, 0, bw, H));
    if (clicked >= 0) {         /* a pin clicked in the inspector */
        select_pin(clicked);
        U.menu_open = true;
        U.menu_mem = false;
        U.menu_pin = clicked;
        U.menu_part = -1;
        U.menu_x = ctx->input.mouse.pos.x + 10;
        U.menu_y = ctx->input.mouse.pos.y + 10;
        U.menu_frame = (int)U.frame;
    }
    bool run = true;
    const char *picked = browser_window(ctx, nk_rect(0, 0, bw, H), &run);
    if (picked) {
        char path[4096];
        snprintf(path, sizeof(path), "%s", picked);
        if (load_prg(path, run)) browser_close();
        else browser_error(U.status);
    }
    diagnose_action_t da;
    diagnose_window(ctx, nk_rect(0, 0, bw, H), &da);
    if (da.pick) U.want_pick = true;
    if (da.records_file) U.want_records_file = da.records_file;
    if (da.probe) {             /* the best matches, narrowed down by probing */
        probe_take_diagnosis();
        diagnose_close();
        probe_open();
    }
    if (da.bench >= 0) U.switch_to = atlas_machine(da.bench);
    atlas_browse_action_t ab;
    atlas_browse_window(ctx, nk_rect(0, 0, bw, H), U.b, bench_machine(), &ab);
    if (ab.probe) {             /* one picture's faults, narrowed down by probing */
        char labels[1][256];
        snprintf(labels[0], sizeof(labels[0]), "%s", ab.label);
        probe_screens_from_screenshot((const char (*)[256])labels, 1);
        U.diag_taken = U.diag_serial;           /* this picture, not the screenshot's */
        atlas_browse_close();
        probe_open();
    }
    help_window(ctx, bw, H);
    probe_action_t pa;
    probe_window(ctx, nk_rect(0, 0, bw, H), U.b, bench_machine(), &pa);
    if (pa.show_pin >= 0) show_pin(pa.show_pin);
    fault_menu(ctx);
    /* Start the statistics over (the drawing above read this frame's). */
    board_stats_reset(U.b);
    if (U.switch_to) {
        switch_machine(U.switch_to);
        U.switch_to = NULL;
        if (U.pending_load[0]) {
            char path[sizeof(U.pending_load)];
            snprintf(path, sizeof(path), "%s", U.pending_load);
            U.pending_load[0] = 0;
            load_prg(path, U.pending_run);
        }
    }
    if (da.apply) apply_diagnosis(da.machine, da.spec);     /* may switch machines too */
    if (ab.apply) {
        apply_diagnosis(ab.machine, ab.spec);
        atlas_browse_close();
    }
    if (pa.apply) apply_diagnosis(bench_machine(), pa.spec);
    if (U.want_esc) {           /* the topmost of the windows Esc closes */
        U.want_esc = false;
        const char *top = NULL;
        for (struct nk_window *w = ctx->begin; w; w = w->next) {
            static const char *const closable[] = { "Menu", "Help", "Inspector", "Diagnose", "AtlasBrowse", "Probe",
                                                    "RecordRepair", "RepairRecords" };
            if (w->flags & (NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED)) continue;
            for (size_t i = 0; i < sizeof(closable) / sizeof(closable[0]); i++)
                if (!strcmp(w->name_string, closable[i])) top = closable[i];
        }
        if (!top) {}
        else if (!strcmp(top, "Menu")) U.menu_open = false;
        else if (!strcmp(top, "Help")) U.help_open = false;
        else if (!strcmp(top, "Inspector")) inspect_open(U.b, -1);
        else if (!strcmp(top, "Diagnose")) diagnose_close();
        else if (!strcmp(top, "AtlasBrowse")) atlas_browse_close();
        else if (!strcmp(top, "RecordRepair")) diagnose_record_cancel();
        else if (!strcmp(top, "RepairRecords")) diagnose_records_close();
        else probe_close();
    }
    if (U.want_dump) {
        U.want_dump = false;
        dump_ui_state(ctx);
        snprintf(U.status, sizeof(U.status), "The interface's state went into %s", UI_LOG);
    }
    unlock_board();
    double t2 = stm_sec(stm_now());
    set_stage(STAGE_RENDER);

    sg_begin_pass(&(sg_pass){ .action = { .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                              .clear_value = { g_theme.bg.r / 255.0f, g_theme.bg.g / 255.0f,
                                               g_theme.bg.b / 255.0f, 1.0f } } },
                              .swapchain = sglue_swapchain() });
    snk_render(sapp_width(), sapp_height());
    sg_end_pass();
    sg_commit();
    thumbtex_collect();         /* pictures let go of this frame, drawn now */
    U.frame++;
    set_stage(STAGE_IDLE);
    frame_times((t1 - t0) * 1000, (t2 - t1) * 1000, (stm_sec(stm_now()) - t2) * 1000);
    /* The system's file dialog waits for the user: not with the board
     * locked (the simulation goes on), not in the middle of a frame. */
    if (U.diag_rematch) {
        U.diag_rematch = false;
        lock_board();
        int machine = bench_machine();
        unlock_board();
        diagnose_set_machine(machine);
        lock_board();
        U.diag_serial++;
        if (probe_is_open()) probe_take_diagnosis();
        unlock_board();
    }
    if (U.diag_drop[0]) {
        char path[sizeof(U.diag_drop)];
        snprintf(path, sizeof(path), "%s", U.diag_drop);
        U.diag_drop[0] = 0;
        diagnose_picture(path);
    }
    if (U.want_browse) {        /* the atlas browser reads no board: not locked */
        U.want_browse = false;
        atlas_browse_open(bench_machine());
    }
    if (U.want_save_file || U.want_open_file) {
        bool save = U.want_save_file;
        U.want_save_file = U.want_open_file = false;
        char path[4096];
        set_stage(STAGE_DIALOG);
        int got = filedialog_state(save, path, sizeof(path));
        set_stage(STAGE_IDLE);
        if (got == FILEDIALOG_PICKED) {
            lock_board();
            if (save) save_state_file(path);
            else load_state_file(path);
            unlock_board();
        } else if (got == FILEDIALOG_NONE) {
            snprintf(U.status, sizeof(U.status), "No file dialog here (install zenity or kdialog)%s",
                     save ? "" : ": drop the .hwstate file on the window");
        }
    }
    if (U.want_records_file) {
        int what = U.want_records_file;
        U.want_records_file = 0;
        char path[4096];
        set_stage(STAGE_DIALOG);
        int got = filedialog_repairs(what != 1, path, sizeof(path));
        set_stage(STAGE_IDLE);
        if (got == FILEDIALOG_PICKED) {
            lock_board();
            diagnose_records_file(what, path);
            unlock_board();
        }
    }
    if (U.want_paste) {
        U.want_paste = false;
        char path[4096];
        int got = filedialog_paste_picture(path, sizeof(path));
        if (got == FILEDIALOG_PICKED) diagnose_picture(path);
        else if (got == FILEDIALOG_NONE) {
            snprintf(U.diag_status, sizeof(U.diag_status),
                     "Can't read the clipboard here (install wl-paste or xclip): drop the screenshot on the window");
        } else {
            snprintf(U.diag_status, sizeof(U.diag_status), "No picture on the clipboard: copy the screenshot first");
        }
    }
    if (U.want_pick) {
        U.want_pick = false;
        char path[4096];
        set_stage(STAGE_DIALOG);
        int got = filedialog_open_picture(path, sizeof(path));
        set_stage(STAGE_IDLE);
        if (got == FILEDIALOG_PICKED) diagnose_picture(path);
        else if (got == FILEDIALOG_NONE) {
            snprintf(U.diag_status, sizeof(U.diag_status),
                     "No file dialog here (install zenity or kdialog): drop the screenshot on the window");
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Events                                                                    */
/* ------------------------------------------------------------------------ */


static void handle_event(const sapp_event *ev);

void ui_event(const sapp_event *ev) {
    /* Mouse moves only feed nuklear. Waiting on the simulation for each one
     * backs up the event queue and the pointer lags. */
    if (ev->type == SAPP_EVENTTYPE_MOUSE_MOVE) {
        snk_handle_event(ev);
        return;
    }
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && ev->key_code == SAPP_KEYCODE_F12) {
        U.want_dump = true;     /* in the next frame, with the windows as drawn */
        return;
    }
    int was = atomic_load(&ui_stage);
    double since = atomic_load(&ui_stage_since);
    set_stage(STAGE_EVENT_LOCK);
    lock_board();
    set_stage(STAGE_EVENT);
    handle_event(ev);
    unlock_board();
    /* Events come between frames, or within one (sokol calls back from
     * inside the window system's calls). */
    atomic_store(&ui_stage_since, since);
    atomic_store(&ui_stage, was);
}

static void handle_event(const sapp_event *ev) {
    snk_handle_event(ev);
    if (ev->type == SAPP_EVENTTYPE_FILES_DROPPED && sapp_get_num_dropped_files() > 0) {
        const char *path = sapp_get_dropped_file_path(0);
        /* A screenshot is diagnosed, anything else loaded. */
        if (diagnose_is_picture(path)) {
            browser_close();
            snprintf(U.diag_drop, sizeof(U.diag_drop), "%s", path);     /* after the frame, unlocked */
        } else if (load_prg(path, true)) browser_close();
        return;
    }
    /* Esc closes the window on top (the file browser takes its own keys);
     * with none open it is the machine's (the Spectrum's BREAK). */
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && ev->key_code == SAPP_KEYCODE_ESCAPE && !browser_is_open() &&
        !probe_wants_keyboard() && !U.notes_typing && !atlas_browse_wants_keyboard() && !diagnose_wants_keyboard() &&
        (diagnose_is_open() || diagnose_recording() || diagnose_records_bounds(&(struct nk_rect){ 0 }) || atlas_browse_is_open() || probe_is_open() || U.help_open || U.menu_open ||
         inspect_current() >= 0)) {
        U.want_esc = true;
        return;
    }
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && ev->key_code == SAPP_KEYCODE_O &&
        (ev->modifiers & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SUPER))) {
        browser_open();
        return;
    }
    /* A key released always reaches the C64, or a key held down while the
     * browser opened (Shift, say) would stay pressed. */
    if (ev->type == SAPP_EVENTTYPE_KEY_UP && M()->host_key(ev->key_code))
        board_key_up(U.b, M()->host_key(ev->key_code));
    /* While the browser is open the keys are for it, not for the C64. Its
     * find field is the only text field: the rest of the time the keyboard
     * is the C64's. (What snk_handle_event() returns is no help: it says
     * the mouse is over a window, and here it always is.) */
    if (browser_is_open()) { browser_key(ev); return; }
    if (probe_wants_keyboard()) return;     /* typing a pin name */
    if (U.notes_typing) return;             /* typing notes */
    if (atlas_browse_wants_keyboard()) return;  /* typing a pin name */
    if (diagnose_wants_keyboard()) return;      /* typing a repair record's notes */
    /* Ctrl+V: the clipboard's screenshot diagnosed (no text field is being
     * typed in, and the machine has no use for it). */
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN && ev->key_code == SAPP_KEYCODE_V && !ev->key_repeat &&
        (ev->modifiers & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SUPER))) {
        U.want_paste = true;
        return;
    }
    if (ev->type == SAPP_EVENTTYPE_CHAR) {
        int c = (int)ev->char_code;
        if (c < 32 || c >= 127 || (ev->modifiers & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SUPER))) return;
        c = M()->host_char(c);
        board_key_down(U.b, c);
        board_key_up(U.b, c);
    } else if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) {
        int k = M()->host_key(ev->key_code);
        if (k) board_key_down(U.b, k);
    }
}

/* ------------------------------------------------------------------------ */
/* Setup                                                                     */
/* ------------------------------------------------------------------------ */

/* Everything of the interface that depends on the board: at start, and
 * when switching machines. */
static void attach_board(board_t *b) {
    U.b = b;
    if (board_is_zx48(b)) U.last_zx = b->m;
    else U.last_c64 = b->m;
    U.hover_pin = U.hover_part = -1;
    U.menu_open = U.dragging = U.reset_hold = false;
    U.fitted = false;           /* the new board fills the view */
    board_free_state(U.saved);  /* it was the other board's */
    U.saved = NULL;
    set_continuity(-1);         /* nets are numbered per board */
    free(U.net_state);
    U.net_state = calloc(b->sim.nnets, 1);
    b->pin_stats = true;        /* what each pin drives, for the inspector */
    compute_boxes();
    /* The monitor picture has the size of the machine's screen. */
    free(U.monitor);
    U.monitor = calloc((size_t)b->m->screen_w * b->m->screen_h, sizeof(uint32_t));
    U.monitor_cycle = 0;               /* force the first draw to upload */
    if (U.img.id) {             /* this frame may draw it still: freed after (thumbtex.h) */
        thumbtex_t old = { .img = U.img, .view = U.view, .snk = U.snk_img, .live = true };
        thumbtex_free(&old);
    }
    browser_set_machine(b->m);
    char title[128];
    snprintf(title, sizeof(title), "c64-hw-sim - %s", b->m->board_name);
    sapp_set_window_title(title);
    U.img = sg_make_image(&(sg_image_desc){ .width = b->m->screen_w, .height = b->m->screen_h,
                                            .pixel_format = SG_PIXELFORMAT_RGBA8,
                                            .usage.dynamic_update = true });
    U.view = sg_make_view(&(sg_view_desc){ .texture.image = U.img });
    U.snk_img = snk_make_image(&(snk_image_desc_t){ .texture_view = U.view, .sampler = U.smp });
}

/* Switch to another machine: its board replaces the one on the bench
 * (the board must be locked). */
static void switch_machine(const machine_t *m) {
    if (U.b->m == m) return;
    inspect_open(U.b, -1);
    board_t *old = U.b;
    attach_board(board_create(m));
    board_destroy(old);
    U.diag_rematch = true;
    U.autoload[0] = 0;
    U.status[0] = 0;
}

void ui_set_machine(const machine_t *m) {
    lock_board();
    switch_machine(m);
    unlock_board();
}

/* The interface font a second time, its picture turned a quarter turn
 * counterclockwise: the letters of vertical labels (see text_vertical). */
static void make_vertical_font(void) {
    static struct nk_font_atlas atlas;
    nk_font_atlas_init_default(&atlas);
    nk_font_atlas_begin(&atlas);
    int w = 0, h = 0;
    const uint32_t *px = nk_font_atlas_bake(&atlas, &w, &h, NK_FONT_ATLAS_RGBA32);
    if (!px || w <= 0 || h <= 0) return;
    /* Pixel (x, y) goes to (y, w - 1 - x): the picture is h wide, w high. */
    uint32_t *rot = malloc(sizeof(uint32_t) * (size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) rot[(size_t)(w - 1 - x) * h + y] = px[(size_t)y * w + x];
    sg_image img = sg_make_image(&(sg_image_desc){
        .width = h, .height = w, .pixel_format = SG_PIXELFORMAT_RGBA8,
        .data.mip_levels[0] = { .ptr = rot, .size = sizeof(uint32_t) * (size_t)w * h } });
    free(rot);
    sg_sampler smp = sg_make_sampler(&(sg_sampler_desc){ .min_filter = SG_FILTER_NEAREST, .mag_filter = SG_FILTER_NEAREST,
                                                         .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE });
    snk_image_t si = snk_make_image(&(snk_image_desc_t){
        .texture_view = sg_make_view(&(sg_view_desc){ .texture = { .image = img } }), .sampler = smp });
    U.vfont_tex = snk_nkhandle(si);
    U.vfont_w = w;
    U.vfont_h = h;
    nk_font_atlas_end(&atlas, U.vfont_tex, NULL);
    nk_font_atlas_cleanup(&atlas);
    U.vfont = atlas.default_font;
}

void ui_init(board_t *b, const char *prg, const char *scope_pin) {
    U.trigger = true;
    make_vertical_font();
    U.timebase = 1;
    U.last_zx = &machine_zx48;
    U.last_c64 = &machine_c64;
    if (prg) { snprintf(U.autoload, sizeof(U.autoload), "%s", prg); U.autoload_run = true; }
    U.smp = sg_make_sampler(&(sg_sampler_desc){ .min_filter = SG_FILTER_LINEAR, .mag_filter = SG_FILTER_LINEAR,
                                                .wrap_u = SG_WRAP_CLAMP_TO_EDGE, .wrap_v = SG_WRAP_CLAMP_TO_EDGE });
    attach_board(b);
    browser_init();
    if (scope_pin) {    /* --scope REF.PIN */
        char ref[16];
        int num;
        if (sscanf(scope_pin, "%15[^.].%d", ref, &num) == 2) {
            int part = sim_find_part(&b->sim, ref);
            if (part >= 0 && part_pin(&b->sim.parts[part], num) >= 0) select_pin(part_pin(&b->sim.parts[part], num));
        }
    }
    pthread_create(&sim_thread, NULL, sim_main, NULL);
    pthread_create(&watch_thread, NULL, watch_main, NULL);
}

void ui_shutdown(void) {
    atomic_store(&sim_quit, true);
    pthread_join(sim_thread, NULL);
    pthread_join(watch_thread, NULL);
    diagnose_shutdown();
    atlas_browse_shutdown();
    probe_shutdown();
    thumbtex_shutdown();
}

void ui_print_stats(bool on) { ui_stats_print = on; }

/* Diagnose a screenshot at start (--diagnose). */
void ui_diagnose(const char *path) { diagnose_picture(path); }

void ui_browse_atlas(int pick) {
    lock_board();
    atlas_browse_open(bench_machine());
    atlas_browse_pick(pick);
    unlock_board();
}

bool chip_records_button(struct nk_context *ctx, int part) {
    const char *ref = S()->parts[part].ref;
    int n = diagnose_records_of_chip(bench_machine(), ref);
    char lab[64];
    if (n) snprintf(lab, sizeof(lab), "Repair records with %s faulty (%d)...", ref, n);
    else snprintf(lab, sizeof(lab), "Repair records with %s faulty: none", ref);
    if (nk_widget_is_hovered(ctx))
        nk_tooltip(ctx, "Repairs of real machines (yours and merged ones) that found this chip faulty, on any board "
                        "of this kind");
    if (!nk_button_label(ctx, lab)) return false;
    diagnose_records_open_chip(bench_machine(), ref);
    return true;
}

/* Open the inspector on a chip by reference ("U8"), for --inspect. */
void ui_inspect(const char *ref) {
    lock_board();
    int part = sim_find_part(S(), ref);
    if (part >= 0) inspect_open(U.b, part);
    else fprintf(stderr, "no part %s\n", ref);
    unlock_board();
}

/* Continuity test on a pin ("U12.14", "CN4.3"), for --continuity. */
void ui_continuity(const char *spec) {
    lock_board();
    char ref[16], pad[8];
    int part = -1, pin = -1;
    if (sscanf(spec, "%15[^.].%7s", ref, pad) == 2 && (part = sim_find_part(S(), ref)) >= 0)
        pin = board_pad_pin(U.b, part, pad);
    if (pin >= 0) set_continuity(pin);
    else fprintf(stderr, "no pin %s\n", spec);
    unlock_board();
}

/* Open the file browser (for --browse). */
void ui_browse(void) { browser_open(); }

void ui_load_state(const char *path) {
    lock_board();
    if (!load_state_file(path)) fprintf(stderr, "%s\n", U.status);
    unlock_board();
}

/* Center the board view on (cx, cy) mm, zoom times the "fit" zoom. */
void ui_view(float cx, float cy, float zoom) {
    U.view_set = true;
    U.view_cx = cx;
    U.view_cy = cy;
    U.view_zoom = zoom;
}

/* Emulated video frames since power on (50 per second). */
int ui_emulated_frame(void) {
    lock_board();
    int f = (int)(U.b->sim.cycle / (uint64_t)U.b->m->cycles_per_frame);
    unlock_board();
    return f;
}

/* Apply a fault spec ("U5:removed", "U6.17:open"...), or "repair" to
 * repair everything, as if done with the mouse. */
void ui_apply_fault(const char *spec) {
    lock_board();
    if (!strcmp(spec, "repair")) board_clear_faults(U.b);
    else if (!strcmp(spec, "save")) {           /* the save state slot, as the buttons */
        board_free_state(U.saved);
        U.saved = board_save_state(U.b);
        snprintf(U.saved_note, sizeof(U.saved_note), "saved at %.2f s", (double)U.b->sim.cycle / M()->cpu_hz);
    } else if (!strcmp(spec, "load")) {
        if (!U.saved || !board_load_state(U.b, U.saved)) fprintf(stderr, "no save state to load\n");
    } else if (!strncmp(spec, "save:", 5)) save_state_file(spec + 5);
    else if (!strncmp(spec, "load:", 5)) load_prg(spec + 5, true);
    else if (!board_apply_fault_spec(U.b, spec)) fprintf(stderr, "bad fault spec: %s\n", spec);
    unlock_board();
}

void ui_save_screenshot(const char *path) {
    int w = sapp_width(), h = sapp_height();
    uint32_t *px = malloc((size_t)w * h * 4), *flip = malloc((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) flip[y * w + x] = px[(h - 1 - y) * w + x] | 0xFF000000u;
    png_write(path, flip, w, h);
    free(px);
    free(flip);
}
