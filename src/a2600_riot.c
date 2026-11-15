/* MOS 6532 RAM, I/O and timer. Pinout and register decoding follow the
 * MPS 6532 data sheet: https://retrosix.wiki/uploads/U9%20-%20RIOT%20%286532%29-1.pdf
 */
#include "a2600_riot.h"

enum {
    RIOT_IRQ = 25, RIOT_RES = 34, RIOT_RW, RIOT_RS, RIOT_CS2, RIOT_CS1, RIOT_PHI2,
    RIOT_PA7 = 15
};
enum { RIOT_EDGE_FLAG = 0x40, RIOT_TIMER_FLAG = 0x80 };

static const int riot_address[] = { 7, 6, 5, 4, 3, 2, 40 };
static const int riot_data[] = { 33, 32, 31, 30, 29, 28, 27, 26 };
static const int riot_pa[] = { 8, 9, 10, 11, 12, 13, 14, 15 };
static const int riot_pb[] = { 24, 23, 22, 21, 19, 18, 17, 16 };

typedef struct {
    uint8_t ram[A2600_RIOT_RAM_SIZE];
    uint8_t out_a, ddr_a, out_b, ddr_b;
    uint8_t timer, flags, irq_enable, read_flags;
    uint16_t interval, clocks_to_tick;
    bool phi2, pa7, positive_edge;
} riot_state_t;

static bool riot_selected(sim_t *s, part_t *p) {
    return pin_in(s, p, RIOT_CS1) && !pin_in(s, p, RIOT_CS2);
}

static unsigned riot_ram_address(sim_t *s, part_t *p) {
    unsigned addr = part_read_bus(s, p, riot_address, A2600_RIOT_RAM_ADDR_BITS);
    if (p->mfault == MF_ADDR) addr &= ~(1u << p->mf_bit);
    return addr;
}

static uint8_t riot_ram_read(sim_t *s, part_t *p, const riot_state_t *st) {
    unsigned addr = riot_ram_address(s, p);
    uint8_t val = st->ram[addr];
    if (p->mfault == MF_FLIP && addr == p->mf_addr) val ^= p->mf_val;
    if (p->mfault == MF_DATA) {
        uint8_t bit = 1u << p->mf_bit;
        val = (val & ~bit) | (p->mf_val ? bit : 0);
    }
    return val;
}

static void riot_drive_port(sim_t *s, part_t *p, const int *pins,
                            uint8_t ddr, uint8_t out, bool push_pull) {
    for (unsigned i = 0; i < 8; i++) {
        unsigned bit = 1u << i;
        drive_t drive = DRV_PULLUP;
        if (ddr & bit) {
            if (!(out & bit)) drive = DRV_LO;
            else if (push_pull) drive = DRV_HI;
        }
        pin_out(s, p, pins[i], drive);
    }
}

/* A lifted or unconnected leg still sees its internal driver/pull-up. */
static unsigned riot_read_port_bit(sim_t *s, part_t *p, int number, unsigned internal) {
    int pi = part_pin(p, number);
    if (pi < 0 || s->pins[pi].root < 0 || s->pins[pi].fault == PF_OPEN) return internal & 1;
    return sim_read(s, pi);
}

static uint8_t riot_read_port(sim_t *s, part_t *p, const int *pins,
                              uint8_t ddr, uint8_t out, bool read_latch) {
    uint8_t result = 0;
    unsigned internal = (uint8_t)(out | ~ddr);
    for (int i = 0; i < 8; i++) {
        unsigned bit = (read_latch && (ddr & (1u << i))) ? (out >> i) & 1
                     : riot_read_port_bit(s, p, pins[i], internal >> i);
        result |= bit << i;
    }
    return result;
}

static uint8_t riot_read_register(sim_t *s, part_t *p, const riot_state_t *st) {
    switch (part_read_bus(s, p, riot_address, 3)) {
    case 0: return riot_read_port(s, p, riot_pa, st->ddr_a, st->out_a, false);
    case 1: return st->ddr_a;
    case 2: return riot_read_port(s, p, riot_pb, st->ddr_b, st->out_b, true);
    case 3: return st->ddr_b;
    case 4:
    case 6: return st->timer;
    default: return st->phi2 ? st->read_flags : st->flags;
    }
}

static void riot_eval(sim_t *s, part_t *p) {
    const riot_state_t *st = p->state;
    bool enabled = pin_in(s, p, RIOT_RES);
    riot_drive_port(s, p, riot_pa, enabled ? st->ddr_a : 0, st->out_a, false);
    riot_drive_port(s, p, riot_pb, enabled ? st->ddr_b : 0, st->out_b, true);
    pin_out(s, p, RIOT_IRQ, enabled && (st->flags & st->irq_enable) ? DRV_LO : DRV_Z);

    bool read = enabled && pin_in(s, p, RIOT_PHI2) && pin_in(s, p, RIOT_RW) && riot_selected(s, p);
    uint8_t val = 0;
    if (read) val = pin_in(s, p, RIOT_RS) ? riot_read_register(s, p, st) : riot_ram_read(s, p, st);
    part_drive_bus(s, p, riot_data, 8, val, read);
}

static void riot_write_register(riot_state_t *st, unsigned addr, uint8_t val) {
    if (!(addr & 4)) {
        switch (addr & 3) {
        case 0: st->out_a = val; break;
        case 1: st->ddr_a = val; break;
        case 2: st->out_b = val; break;
        case 3: st->ddr_b = val; break;
        }
    } else if (addr & 16) {
        static const uint16_t intervals[] = { 1, 8, 64, 1024 };
        st->interval = intervals[addr & 3];
        st->timer = val;
        /* First decrement is one clock after the load, independent of the divider. */
        st->clocks_to_tick = 1;
        st->flags &= ~RIOT_TIMER_FLAG;
        st->irq_enable = (st->irq_enable & RIOT_EDGE_FLAG) | ((addr & 8) ? RIOT_TIMER_FLAG : 0);
    } else {
        st->positive_edge = (addr & 1) != 0;
        st->irq_enable = (st->irq_enable & RIOT_TIMER_FLAG) | ((addr & 2) ? RIOT_EDGE_FLAG : 0);
    }
}

/* Returns true on the clock that sets the timer flag. An INTIM read on
 * this clock leaves it set. After underflow the timer decrements every
 * PHI2 until a timer read clears the flag and restores the divider. */
static bool riot_tick_timer(riot_state_t *st) {
    if (st->flags & RIOT_TIMER_FLAG) {
        st->timer--;
        return false;
    }
    if (--st->clocks_to_tick != 0) return false;
    st->clocks_to_tick = st->interval;
    if (st->timer-- != 0) return false;
    st->flags |= RIOT_TIMER_FLAG;
    return true;
}

static void riot_read_side_effects(riot_state_t *st, unsigned addr, bool underflow) {
    if (!(addr & 4)) return;
    if (addr & 1) {
        st->flags &= ~RIOT_EDGE_FLAG;
        return;
    }
    st->irq_enable = (st->irq_enable & RIOT_EDGE_FLAG) | ((addr & 8) ? RIOT_TIMER_FLAG : 0);
    if (!underflow && (st->flags & RIOT_TIMER_FLAG)) {
        st->flags &= ~RIOT_TIMER_FLAG;
        /* Approximate the resumed phase by waiting a whole interval. */
        st->clocks_to_tick = st->interval;
    }
}

static void riot_commit(sim_t *s, part_t *p) {
    riot_state_t *st = p->state;
    bool phi2 = pin_in(s, p, RIOT_PHI2);
    bool rising = phi2 && !st->phi2;
    st->phi2 = phi2;

    if (!pin_in(s, p, RIOT_RES)) {
        st->out_a = st->ddr_a = st->out_b = st->ddr_b = 0;
        st->flags = st->irq_enable = 0;
        st->positive_edge = false;
        st->pa7 = riot_read_port_bit(s, p, RIOT_PA7, 1);
        return;
    }
    if (!rising) return;

    bool underflow = riot_tick_timer(st);
    /* Sample PA7 once per PHI2, including when it is an output. */
    unsigned pa7_drive = (uint8_t)(st->out_a | ~st->ddr_a) >> 7;
    bool pa7 = riot_read_port_bit(s, p, RIOT_PA7, pa7_drive);
    if (pa7 != st->pa7 && pa7 == st->positive_edge) st->flags |= RIOT_EDGE_FLAG;
    st->pa7 = pa7;
    st->read_flags = st->flags;

    if (riot_selected(s, p)) {
        bool reg = pin_in(s, p, RIOT_RS);
        bool read = pin_in(s, p, RIOT_RW);
        if (reg) {
            unsigned addr = part_read_bus(s, p, riot_address, 5);
            if (read) riot_read_side_effects(st, addr, underflow);
            else riot_write_register(st, addr, (uint8_t)part_read_bus(s, p, riot_data, 8));
        } else if (!read) {
            st->ram[riot_ram_address(s, p)] = (uint8_t)part_read_bus(s, p, riot_data, 8);
        }
    }
    sim_mark_dirty(s, (int)(p - s->parts));
}

static void riot_reset(sim_t *s, part_t *p) {
    riot_state_t *st = p->state;
    /* Deterministic cold-start values for RAM and the unprogrammed timer. */
    *st = (riot_state_t){ .timer = 0xff, .interval = 1024, .clocks_to_tick = 1024, .pa7 = true };
}

const part_type_t a2600_riot_type = { "6532 RIOT",
    "GND:P A5:M A4:M A3:M A2:M A1:M A0:M PA0:X PA1:X PA2:X "
    "PA3:X PA4:X PA5:X PA6:X PA7:X PB7:X PB6:X PB5:X PB4:X VCC:P "
    "PB3:X PB2:X PB1:X PB0:X /IRQ:O D7:B D6:B D5:B D4:B D3:B "
    "D2:B D1:B D0:B /RES:M R/W:M RS:M /CS2:M CS1:M PHI2:M A6:M",
    riot_eval, riot_commit, riot_reset, sizeof(riot_state_t), TECH_NMOS };
