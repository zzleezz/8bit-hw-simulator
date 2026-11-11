/* ui_common.h - colors and small drawing helpers shared by the interface
 * files: ui.c (board, monitor, controls, scope) and inspect.c (the chip
 * inspector and the logic diagrams). */
#ifndef UI_COMMON_H
#define UI_COMMON_H
#include "ui.h"
#include "board_data.h"
#include <stdio.h>
#include <string.h>

/* Colors. */
#define COL_PCB       nk_rgb(28, 88, 52)
#define COL_PCB_EDGE  nk_rgb(10, 45, 25)
#define COL_CHIP      nk_rgb(26, 26, 28)
#define COL_CHIP_EDGE nk_rgb(100, 100, 108)
#define COL_SOCKET    nk_rgb(200, 165, 90)
#define COL_RES       nk_rgb(205, 182, 140)
#define COL_LINK      nk_rgb(70, 70, 76)
#define COL_OTHER     nk_rgb(60, 120, 80)
#define COL_TEXT      nk_rgb(235, 235, 225)
#define COL_DIMTEXT   nk_rgb(150, 160, 150)
#define COL_HIGH      nk_rgb(235, 30, 30)     /* a true red: +12V is orange */
#define COL_LOW       nk_rgb(70, 140, 255)
#define COL_PULSE     nk_rgb(250, 210, 60)
#define COL_FLOAT     nk_rgb(160, 160, 160)
#define COL_FIGHT     nk_rgb(255, 60, 255)
#define COL_NONE      nk_rgb(70, 70, 70)
#define COL_FAULT     nk_rgb(255, 150, 0)     /* fault text on the windows */
#define COL_FAULT_MARK nk_rgb(255, 255, 255)   /* a fault on the board: no pin color is white */
#define COL_HOVER     nk_rgb(0, 255, 255)
#define COL_TRACE     nk_rgb(90, 255, 120)
#define COL_HARNESS   nk_rgb(150, 160, 255)
#define COL_NEG5      nk_rgb(170, 255, 0)     /* -5V: not GND's blue */
#define COL_P12       nk_rgb(255, 120, 0)     /* +12V: not +5V's red */

/* The pin colors only: a net on the -5V or the +12V supply (the 4116s'
 * VBB and VDD, the VIC-II's and SID's 12V). To a logic probe they are low
 * and high, like GND and +5V, so they get their own colors. */
#define NET_NEG5      (NET_NONE + 1)
#define NET_P12       (NET_NONE + 2)

/* Light or dark windows (ui_set_theme(), the Theme row of the controls).
 * The board, the monitor, the scope screen and the chips keep their own
 * colors either way: only the windows and the space around them change. */
typedef struct {
    bool light;
    struct nk_color bg;                 /* behind the board and the windows */
    struct nk_color row_sel, row_hover; /* the file browser's list */
    struct nk_color line_hover;         /* a line of the fault list */
} theme_t;
extern theme_t g_theme;

/* A color for text or a line on a window. The colors above are made for
 * a dark one; on a light one, a light grey becomes as dark as it was
 * light, and a bright color is darkened until it reads. Dark colors stay,
 * so ink(ink(c)) == ink(c). */
static inline struct nk_color ink(struct nk_color c) {
    if (!g_theme.light) return c;
    float l = (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255;
    int hi = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    int lo = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    if (hi - lo < 40) {
        if (l <= 0.5f) return c;
        nk_byte v = (nk_byte)((1 - l) * 255);
        return nk_rgba(v, v, v, c.a);
    }
    if (l <= 0.42f) return c;
    float k = 0.42f / l;
    return nk_rgba((int)(c.r * k), (int)(c.g * k), (int)(c.b * k), c.a);
}

/* Colored labels are always on a window. */
#define nk_label_colored(ctx, txt, align, c) nk_label_colored(ctx, txt, align, ink(c))
#define nk_label_colored_wrap(ctx, txt, c) nk_label_colored_wrap(ctx, txt, ink(c))

/* Chip names are colored by role (see ROLE_* in board.h). */
static const struct nk_color role_color[] = {
    [ROLE_RAM]   = { 120, 195, 255, 255 },    /* the 8 DRAMs */
    [ROLE_VRAM]  = { 225, 140, 255, 255 },    /* color RAM */
    [ROLE_ROM]   = { 255, 195, 100, 255 },
    [ROLE_PLA]   = { 255, 110, 140, 255 },
    [ROLE_CHIP]  = { 240, 240, 230, 255 },    /* CPU, VIC, SID, CIAs */
    [ROLE_GLUE]  = { 175, 195, 175, 255 },
    [ROLE_CLOCK] = { 135, 140, 135, 255 },
    [ROLE_OTHER] = { 235, 235, 225, 255 },
};

/* The interface font, three times the size (NULL before the window is up). */
const struct nk_user_font *ui_big_font(void);

static inline float text_width(struct nk_context *ctx, const char *txt) {
    const struct nk_user_font *f = ctx->style.font;
    return f->width(f->userdata, f->height, txt, (int)strlen(txt));
}

/* A button in a color of its own: BG filled, FG the text (the analyser
 * window's Start and Stop, the probing window's readings). */
static inline bool color_button(struct nk_context *ctx, const char *label, struct nk_color bg, struct nk_color fg) {
    struct nk_style_button *b = &ctx->style.button;
    struct nk_color hov = nk_rgb(bg.r + (255 - bg.r) / 5, bg.g + (255 - bg.g) / 5, bg.b + (255 - bg.b) / 5);
    nk_style_push_style_item(ctx, &b->normal, nk_style_item_color(bg));
    nk_style_push_style_item(ctx, &b->hover, nk_style_item_color(hov));
    nk_style_push_style_item(ctx, &b->active, nk_style_item_color(hov));
    nk_style_push_color(ctx, &b->text_normal, fg);
    nk_style_push_color(ctx, &b->text_hover, fg);
    nk_style_push_color(ctx, &b->text_active, fg);
    nk_style_push_color(ctx, &b->border_color, hov);
    bool r = nk_button_label(ctx, label);
    nk_style_pop_color(ctx); nk_style_pop_color(ctx); nk_style_pop_color(ctx); nk_style_pop_color(ctx);
    nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx);
    return r;
}

/* Text centered in a rectangle. */
static inline void text_centered(struct nk_command_buffer *cv, struct nk_context *ctx,
                                 struct nk_rect r, const char *txt, struct nk_color c) {
    const struct nk_user_font *f = ctx->style.font;
    float w = text_width(ctx, txt);
    nk_draw_text(cv, nk_rect(r.x + (r.w - w) / 2, r.y + (r.h - f->height) / 2, w + 2, f->height),
                 txt, (int)strlen(txt), f, nk_rgba(0, 0, 0, 0), c);
}

/* Text starting at x (align 0), ending at x (align 1), vertically
 * centered on y. */
static inline void text_at(struct nk_command_buffer *cv, struct nk_context *ctx, float x, float y,
                           int align, const char *txt, struct nk_color c) {
    const struct nk_user_font *f = ctx->style.font;
    float w = text_width(ctx, txt);
    if (align) x -= w;
    nk_draw_text(cv, nk_rect(x, y - f->height / 2, w + 2, f->height), txt, (int)strlen(txt), f,
                 nk_rgba(0, 0, 0, 0), c);
}

static inline struct nk_color state_color(int st) {
    switch (st) {
    case NET_HIGH: return COL_HIGH;
    case NET_LOW: return COL_LOW;
    case NET_PULSING: return COL_PULSE;
    case NET_FLOATING: return COL_FLOAT;
    case NET_CONTENTION: return COL_FIGHT;
    case NET_NEG5: return COL_NEG5;
    case NET_P12: return COL_P12;
    default: return COL_NONE;
    }
}

static inline const char *state_name(int st) {
    switch (st) {
    case NET_HIGH: return "HIGH";
    case NET_LOW: return "LOW";
    case NET_PULSING: return "PULSING";
    case NET_FLOATING: return "FLOATING";
    case NET_CONTENTION: return "CONTENTION";
    case NET_NEG5: return "-5V";
    case NET_P12: return "+12V";
    default: return "not connected";
    }
}

/* A combo with a tooltip. The tooltip is drawn after the combo: a tooltip
 * is a popup until the end of the frame, and a combo drawn while one is up
 * won't open (pointing at the combo to click it showed the tooltip first). */
static inline int combo_tip(struct nk_context *ctx, const char *const *items, int n, int sel, int item_h,
                            struct nk_vec2 size, const char *tip) {
    struct nk_rect r = nk_widget_bounds(ctx);
    int now = nk_combo(ctx, items, n, sel, item_h, size);
    if (tip && nk_input_is_mouse_hovering_rect(&ctx->input, r)) nk_tooltip(ctx, tip);
    return now;
}

/* "Hide fault markers": the board, the inspector and the fault list don't
 * show what is broken (for practice: find it like on a real board). */
extern bool g_hide_faults;

static inline const char *pin_fault_name(int f) {
    switch (f) {
    case PF_OPEN: return "floating (open)";
    case PF_GND: return "shorted to GND";
    case PF_VCC: return "shorted to +5V";
    case PF_OUT0: return "output dead, stuck low";
    case PF_OUT1: return "output dead, stuck high";
    default: return "working";
    }
}

/* Short description of a pin: "U17.18 /CASRAM (PLA)", "CN4.3 ATN (SERIAL)",
 * "C54.1 (capacitor .1)". */
static inline void pin_desc(board_t *b, int pin, char *buf, size_t len) {
    const sim_t *s = &b->sim;
    const pin_t *p = &s->pins[pin];
    const part_t *part = &s->parts[p->part];
    const char *name = sim_pin_name(s, pin), *func = board_pin_func(b, pin);
    if (part->kind == PART_IC)
        snprintf(buf, len, "%s.%s %s (%s)", part->ref, b->sim.bd->pads[pin].name, name ? name : "",
                 board_part_label(b, p->part));
    else if (func)
        snprintf(buf, len, "%s.%s %s (%s)", part->ref, b->sim.bd->pads[pin].name, func,
                 board_connector_name(b, p->part));
    else if (part->kind != PART_CONNECTOR)
        snprintf(buf, len, "%s.%s (%s %s)", part->ref, b->sim.bd->pads[pin].name, board_part_kind_name(b, p->part),
                 part->name ? part->name : "");
    else
        snprintf(buf, len, "%s.%s %s", part->ref, b->sim.bd->pads[pin].name, name ? name : "");
}

/* "Repair records with U7 faulty (3)": the records window showing the
 * real repairs of chip PART (diagnose.c). True if clicked. */
bool chip_records_button(struct nk_context *ctx, int part);

/* TEXT on one line: a chip label's line breaks ("4164\nbit D0") as spaces. */
static inline const char *one_line(char *out, size_t len, const char *text) {
    snprintf(out, len, "%s", text);
    for (char *c = out; *c; c++) if (*c == '\n') *c = ' ';
    return out;
}

static inline const char *pin_net_name(board_t *b, int pin) {
    int net = b->sim.pins[pin].net;
    return net >= 0 ? b->sim.nets[net].name : "(none)";
}

#endif
