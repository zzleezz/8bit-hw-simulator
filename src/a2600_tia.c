/* PAL TIA digital model for the first diagnostic cartridge.
 *
 * Bus timing, WSYNC and the playfield follow Atari's Stella Programmer's
 * Guide (https://alienbill.com/2600/101/docs/stella.html). PAL pin differences
 * and clock rates follow the chip pinouts and clock table in the 2K6 specs
 * (https://www.astro.physik.uni-potsdam.de/~mwendt/2600scene/specs.html).
 * The monitor uses an approximate palette and samples the digital pins
 * before the analogue video mixer and modulator. */
#include "a2600_tia.h"
#include <string.h>

enum {
    TIA_SYNC = 2, TIA_RDY = 3, TIA_PHI0 = 4, TIA_COL = 9, TIA_OSC = 11,
    TIA_CS3 = 21, TIA_CS2 = 22, TIA_CS1 = 23, TIA_CS0 = 24, TIA_RW = 25, TIA_PHI2 = 26,
    TIA_D6 = 33, TIA_D7 = 34,
    TIA_HBLANK = 68, TIA_HSYNC_ON = 16, TIA_HSYNC_OFF = 32,
    TIA_BURST_ON = 36, TIA_BURST_OFF = 52,
    TIA_SYNC_TIMEOUT = 3 * (A2600_CLOCKS_PER_LINE / 3),
};

static const int tia_luma_pins[] = {7, 5, 6};
static const int tia_address_pins[] = {32, 31, 30, 29, 28, 27};
static const int tia_data_pins[] = {14, 15, 16, 17, 18, 19, TIA_D6, TIA_D7};
static const int tia_input_pins[] = {40, 39, 38, 37, 36, 35};

typedef struct {
    uint8_t color[4];    /* P0, P1, playfield, background */
    uint8_t playfield[3], ctrlpf, vblank;
    uint8_t cpu_phase, chroma_phase, rsync_delay;
    int hclock, y;
    bool osc, phi2, sample_pixel;
    bool vsync;
    bool trigger_low[2];
    bool sync_level, have_sync, frame_started, frame_sync;
    uint8_t burst_levels;
    uint64_t last_sync;
    uint32_t frame[A2600_SCREEN_W * A2600_SCREEN_H];
} a2600_tia_state_t;

/* A7 and A12 are the two chip selects driven by the motherboard bus.
 * The other two chip selects are tied to their active levels. */
static bool a2600_tia_selected(sim_t *s, part_t *p) {
    return !pin_in(s, p, TIA_CS3) && !pin_in(s, p, TIA_CS0) &&
           !pin_in(s, p, TIA_CS2) && pin_in(s, p, TIA_CS1);
}

static void a2600_tia_eval(sim_t *s, part_t *p) {
    a2600_tia_state_t *t = p->state;
    drive_t d6 = DRV_Z, d7 = DRV_Z;
    if (pin_in(s, p, TIA_PHI2) && pin_in(s, p, TIA_RW) &&
        a2600_tia_selected(s, p)) {
        unsigned addr = part_read_bus(s, p, tia_address_pins, 4);
        if (addr < 8) {
            /* With only a playfield there are no object collisions. */
            d7 = DRV_LO;
            if (addr != 6) d6 = DRV_LO;  /* CXBLPF has only bit 7. */
        } else if (addr < 14) {
            int input = addr - 8;
            int level = pin_in(s, p, tia_input_pins[input]);
            if (input >= 4 && (t->vblank & 0x40))
                level = !t->trigger_low[input - 4];
            d7 = DRV_BIT(level);
        }
    }
    pin_out(s, p, TIA_D6, d6);
    pin_out(s, p, TIA_D7, d7);
}

static unsigned a2600_tia_pixel(const a2600_tia_state_t *t) {
    if (t->hclock < TIA_HBLANK || (t->vblank & 2) || t->vsync) return 0;
    int x = t->hclock - TIA_HBLANK;
    int cell = (x / 4) % 20;
    if (x >= 80 && (t->ctrlpf & 1)) cell = 19 - cell;
    unsigned pf;
    if (cell < 4) pf = t->playfield[0] >> (4 + cell);
    else if (cell < 12) pf = t->playfield[1] >> (11 - cell);
    else pf = t->playfield[2] >> (cell - 12);
    if (!(pf & 1)) return t->color[3];
    if ((t->ctrlpf & 6) == 2) return t->color[x >= 80];
    return t->color[2];
}

static bool a2600_tia_burst(const a2600_tia_state_t *t) {
    return !t->vsync && t->hclock >= TIA_BURST_ON && t->hclock < TIA_BURST_OFF;
}

static void a2600_tia_drive_video(sim_t *s, part_t *p) {
    const a2600_tia_state_t *t = p->state;
    unsigned color = a2600_tia_pixel(t);
    bool hsync = t->hclock >= TIA_HSYNC_ON && t->hclock < TIA_HSYNC_OFF;
    pin_out(s, p, TIA_SYNC, hsync != t->vsync ? DRV_LO : DRV_Z);
    for (int bit = 0; bit < 3; bit++)
        pin_out(s, p, tia_luma_pins[bit], (color & (2u << bit)) ? DRV_Z : DRV_LO);

    /* One digital sample of the PAL subcarrier at each oscillator edge.
     * The actual PALS/PALI phase circuit and CADJ trim are analogue. */
    unsigned hue = color >> 4;
    bool chroma = a2600_tia_burst(t) || (hue >= 2 && hue < 14);
    uint8_t phase = t->chroma_phase + (a2600_tia_burst(t) ? 0 : 16 * hue);
    pin_out(s, p, TIA_COL, chroma && (phase & 0x80) ? DRV_LO : DRV_Z);
}

static void a2600_tia_commit(sim_t *s, part_t *p) {
    a2600_tia_state_t *t = p->state;
    bool phi2 = pin_in(s, p, TIA_PHI2);
    bool write = t->phi2 && !phi2 && !pin_in(s, p, TIA_RW) &&
                 a2600_tia_selected(s, p);
    t->phi2 = phi2;
    if (write) {
        unsigned addr = part_read_bus(s, p, tia_address_pins, 6);
        unsigned data = part_read_bus(s, p, tia_data_pins, 8);
        switch (addr) {
        case 0x00:
            t->vsync = (data & 2) != 0;
            break;
        case 0x01:
            t->vblank = data;
            for (int i = 0; i < 4; i++)
                pin_out(s, p, tia_input_pins[i], (data & 0x80) ? DRV_LO : DRV_Z);
            break;
        case 0x02:
            pin_out(s, p, TIA_RDY, DRV_LO);
            break;
        case 0x03:
            /* Restart after a short TIA counter cycle. The near-edge
             * interaction with PHI0 is approximated in this model. */
            t->rsync_delay = 3;
            break;
        case 0x06: case 0x07: case 0x08: case 0x09:
            t->color[addr - 6] = data & 0xFE;
            break;
        case 0x0A:
            t->ctrlpf = data;
            break;
        case 0x0D: case 0x0E: case 0x0F:
            t->playfield[addr - 0x0D] = data;
            break;
        default:
            break;
        }
        a2600_tia_drive_video(s, p);
    }

    for (int i = 0; i < 2; i++) {
        bool latched = (t->vblank & 0x40) &&
                       (t->trigger_low[i] || !pin_in(s, p, tia_input_pins[i + 4]));
        if (latched == t->trigger_low[i]) continue;
        t->trigger_low[i] = latched;
        sim_mark_dirty(s, (int)(p - s->parts));
    }
}

static void a2600_tia_reset(sim_t *s, part_t *p) {
    a2600_tia_state_t *t = p->state;
    memset(t, 0, sizeof *t);
    t->cpu_phase = 5;
    t->hclock = A2600_CLOCKS_PER_LINE - 1;
    for (int i = 0; i < 4; i++) pin_out(s, p, tia_input_pins[i], DRV_Z);
    pin_out(s, p, TIA_RDY, DRV_Z);
    pin_out(s, p, TIA_PHI0, DRV_LO);
    a2600_tia_drive_video(s, p);
}

const part_type_t a2600_tia_type = {
    "C011903 PAL TIA",
    "GND:P CSYNC:O RDY:O PHI0:O LUM1:O LUM2:O LUM0:O PALI:A COL:O CADJ:A OSC:m PALS:A AUD:A "
    "D0:B D1:B D2:B D3:B D4:B D5:B VCC:P /CS3:M /CS2:M CS1:M /CS0:M R/W:M PHI2:M "
    "A5:m A4:m A3:M A2:M A1:M A0:M D6:B D7:B "
    "INPT5:M INPT4:M INPT3:X INPT2:X INPT1:X INPT0:X",
    a2600_tia_eval, a2600_tia_commit, a2600_tia_reset, sizeof(a2600_tia_state_t), TECH_NMOS
};

void a2600_tia_clock(sim_t *s, part_t *p) {
    a2600_tia_state_t *t = p->state;
    t->sample_pixel = false;
    if (p->dead) return;
    bool osc = pin_in(s, p, TIA_OSC);
    if (osc == t->osc) return;
    t->osc = osc;
    t->cpu_phase = (t->cpu_phase + 1) % 6;
    pin_out(s, p, TIA_PHI0, DRV_BIT(t->cpu_phase < 3));
    t->chroma_phase += 160;  /* 4.433618 MHz at a 2 x 3.546894 MHz sample rate */
    if (osc) {
        bool restart = t->rsync_delay && --t->rsync_delay == 0;
        if (++t->hclock == A2600_CLOCKS_PER_LINE || restart) {
            t->hclock = 0;
            if (t->y < A2600_SCREEN_H) t->y++;
            t->burst_levels = 0;
            pin_out(s, p, TIA_RDY, DRV_Z);
        }
        t->sample_pixel = true;
    }
    a2600_tia_drive_video(s, p);
}

static bool a2600_tia_net_high(const sim_t *s, const part_t *p, int pin) {
    int root = s->pins[part_pin(p, pin)].root;
    return root >= 0 && s->nets[root].level;
}

static unsigned a2600_tia_clip(int c) {
    if (c < 0) return 0;
    if (c > 255) return 255;
    return (unsigned)c;
}

static uint32_t a2600_tia_rgb(unsigned hue, unsigned luma) {
    /* Signed RGB offsets approximate the PAL hue ordering documented in
     * the 2K6 specs. Actual hues depend on console and display adjustments. */
    static const int8_t pal[16][3] = {
        {0, 0, 0}, {0, 0, 0}, {49, 19, -84}, {12, 34, -84},
        {86, -16, -58}, {-63, 49, -71}, {90, -30, -27}, {-83, 44, -9},
        {84, -40, 31}, {-83, 30, 58}, {50, -42, 77}, {-65, 9, 87},
        {12, -26, 91}, {-31, -12, 91}, {0, 0, 0}, {0, 0, 0},
    };
    int y = luma * 255 / 7;
    unsigned r = a2600_tia_clip(y + pal[hue][0]);
    unsigned g = a2600_tia_clip(y + pal[hue][1]);
    unsigned b = a2600_tia_clip(y + pal[hue][2]);
    return 0xFF000000u | b << 16 | g << 8 | r;
}

void a2600_tia_video(sim_t *s, part_t *p) {
    if (p->dead) return;
    board_t *b = p->user;
    a2600_tia_state_t *t = p->state;
    bool sync = a2600_tia_net_high(s, p, TIA_SYNC);
    if (sync != t->sync_level &&
        (t->hclock == TIA_HSYNC_ON || t->hclock == TIA_HSYNC_OFF)) {
        t->last_sync = s->cycle;
        t->have_sync = true;
    }
    t->sync_level = sync;

    if (!t->vsync) t->frame_sync = false;
    if (t->vsync && !t->frame_sync && !sync && t->have_sync &&
        s->cycle - t->last_sync < TIA_SYNC_TIMEOUT) {
        if (t->frame_started) memcpy(b->screen, t->frame, sizeof t->frame);
        memset(t->frame, 0, sizeof t->frame);
        t->y = 0;
        t->frame_started = t->frame_sync = true;
    }

    bool col = a2600_tia_net_high(s, p, TIA_COL);
    if (a2600_tia_burst(t)) t->burst_levels |= col ? 2 : 1;
    if (!t->sample_pixel || !t->frame_started || t->y >= A2600_SCREEN_H ||
        t->hclock < TIA_HBLANK || !sync) return;
    unsigned luma = 0;
    for (int bit = 0; bit < 3; bit++)
        if (a2600_tia_net_high(s, p, tia_luma_pins[bit])) luma |= 1u << bit;
    unsigned hue = t->burst_levels == 3 ? a2600_tia_pixel(t) >> 4 : 0;
    t->frame[t->y * A2600_SCREEN_W + t->hclock - TIA_HBLANK] = a2600_tia_rgb(hue, luma);
}

void a2600_tia_monitor(board_t *b, part_t *p, uint32_t *out) {
    const a2600_tia_state_t *t = p->state;
    b->no_signal = p->dead || !t->have_sync ||
                   b->sim.cycle - t->last_sync >= TIA_SYNC_TIMEOUT;
    for (int i = 0; i < A2600_SCREEN_W * A2600_SCREEN_H; i++) {
        uint32_t color = b->screen[i] | 0xFF000000u;
        if (b->no_signal) {
            b->snow_seed = b->snow_seed * 1103515245u + 12345u;
            uint32_t gray = (b->snow_seed >> 24) * 3 / 4;
            color = 0xFF000000u | gray * 0x010101u;
        }
        out[i] = color;
    }
}
