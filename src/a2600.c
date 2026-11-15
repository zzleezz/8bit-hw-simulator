/* Atari 2600A PAL digital bench. CPU, RIOT, TIA and cartridge communicate
 * through simulated pins. The oscillator and power-on reset are digital
 * circuit equivalents. See docs/atari2600-pal.md. */
#include "board.h"
#include "a2600_riot.h"
#include "a2600_tia.h"
#include "m6502.h"
#include "sokol_headers.h"
#include <stdio.h>
#include <string.h>

#define PAL_OSC_HZ 3546894.0
#define PAL_CYCLES_PER_FRAME (76 * 312)

enum { A2600_UP, A2600_DOWN, A2600_LEFT, A2600_RIGHT, A2600_FIRE, A2600_SELECT };

typedef struct {
    unsigned size;
    char name[64];
    uint8_t data[4096];
} a2600_cart_t;

typedef struct {
    board_t b;
    m6502_t cpu;
    uint64_t cpu_bus;
    uint8_t cpu_data;
    bool phi0, in_reset, waiting;
    unsigned keys;
    int cpu_part, riot_part, tia_part, cart_part, oscillator, por, controls[2];
} a2600_t;

static const int atari_cpu_addr[] = { 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17 };
static const int atari_cpu_data[] = { 25, 24, 23, 22, 21, 20, 19, 18 };
static const int atari_cart_addr[] = { 8, 7, 6, 5, 4, 3, 2, 1, 22, 21, 19, 20 };
static const int atari_cart_data[] = { 9, 10, 11, 13, 14, 15, 16, 17 };

static void atari_cart_eval(sim_t *s, part_t *p) {
    const a2600_cart_t *cart = p->state;
    bool enabled = cart->size && pin_in(s, p, 18);
    unsigned data = 0;
    if (enabled) {
        unsigned addr = part_read_bus(s, p, atari_cart_addr, 12);
        data = cart->data[addr & (cart->size - 1)];
    }
    part_drive_bus(s, p, atari_cart_data, 8, data, enabled);
}

static const part_type_t atari_cpu_type = {
    .name = "MOS 6507",
    .pins = "/RES:m GND:P RDY:m VCC:P A0:O A1:O A2:O A3:O A4:O A5:O A6:O A7:O A8:O A9:O "
            "A10:O A11:O A12:O D7:B D6:B D5:B D4:B D3:B D2:B D1:B D0:B R/W:O PHI0:m PHI2:O"
};

static const part_type_t atari_cart_type = {
    .name = "Cartridge",
    .pins = "A7:M A6:M A5:M A4:M A3:M A2:M A1:M A0:M D0:O D1:O D2:O GND:P "
            "D3:O D4:O D5:O D6:O D7:O A12:M A10:M A11:M A9:M A8:M VCC:P GND:P",
    .eval = atari_cart_eval, .state_size = sizeof(a2600_cart_t)
};

static const part_type_t atari_source_type = { .name = "Digital source", .pins = "GND:P VCC:P OUT:O" };

static const part_def_t atari_parts[] = {
    { "A200", &atari_cpu_type, "6507", ROLE_CHIP, "CPU", "NMOS 6502 core\n13 address lines" },
    { "A202", &a2600_riot_type, "6532 RIOT", ROLE_RAM, "RAM, I/O and timer", "128 bytes of RAM\nTwo 8-bit ports" },
    { "A201", &a2600_tia_type, "PAL TIA", ROLE_CHIP, "Television Interface Adaptor", "C011903\nVideo timing and inputs" },
    { "CART", &atari_cart_type, "Cartridge", ROLE_ROM, "24-pin cartridge connection", "Linear 2 or 4 KiB ROM" },
    { "CLOCK", &atari_source_type, "PAL OSC", ROLE_CLOCK, "Digital oscillator equivalent", "3.546894 MHz" },
    { "POR", &atari_source_type, "Power-on reset", ROLE_CLOCK, "Digital reset equivalent", "Active low at power on" }
};

static const conn_def_t atari_conns[] = {
    { "CART", "Cartridge", "1=A7 2=A6 3=A5 4=A4 5=A3 6=A2 7=A1 8=A0 9=D0 10=D1 11=D2 12=GND "
                            "13=D3 14=D4 15=D5 16=D6 17=D7 18=A12 19=A10 20=A11 21=A9 22=A8 23=+5V 24=GND" },
    { "JOY0", "Left controller", "1=Up 2=Down 3=Left 4=Right 5=Paddle_1 6=Fire 7=+5V 8=GND 9=Paddle_0" },
    { "JOY1", "Right controller", "1=Up 2=Down 3=Left 4=Right 5=Paddle_3 6=Fire 7=+5V 8=GND 9=Paddle_2" },
    { "SW_RESET", "Game reset", "1=Game_Reset 2=GND" },
    { "SW_SELECT", "Game select", "1=Game_Select 2=GND" }
};

static int atari_rail(const char *net) {
    if (!strcmp(net, "GND")) return RAIL_GND;
    if (!strcmp(net, "+5V")) return RAIL_VCC;
    return RAIL_NONE;
}

static int atari_kind(const char *ref) {
    if (ref[0] == 'A') return PART_IC;
    if (!strcmp(ref, "RP")) return PART_RESISTOR;
    if (!strncmp(ref, "JOY", 3) || !strncmp(ref, "SW_", 3) || !strcmp(ref, "CART"))
        return PART_CONNECTOR;
    return PART_OTHER;
}

static void atari_create(board_t *b) {
    a2600_t *a = (a2600_t *)b;
    sim_t *s = &b->sim;
    a->cpu_part = sim_find_part(s, "A200");
    a->riot_part = sim_find_part(s, "A202");
    a->tia_part = sim_find_part(s, "A201");
    a->cart_part = sim_find_part(s, "CART");
    a->oscillator = sim_find_part(s, "CLOCK");
    a->por = sim_find_part(s, "POR");
    a->controls[0] = sim_find_part(s, "JOY0");
    a->controls[1] = sim_find_part(s, "SW_SELECT");
    int reset = sim_find_part(s, "SW_RESET");
    b->pin_reset = part_pin(&s->parts[reset], 1);
    b->pin_reset_gnd = part_pin(&s->parts[reset], 2);

    for (int i = 0; i < 13; i++)
        sim_set_bus_bit(s, s->pins[part_pin(&s->parts[a->cpu_part], atari_cpu_addr[i])].net, i);
    for (int i = 0; i < 8; i++)
        sim_set_bus_bit(s, s->pins[part_pin(&s->parts[a->cpu_part], atari_cpu_data[i])].net, 16 + i);
}

static void atari_power_on(board_t *b) {
    a2600_t *a = (a2600_t *)b;
    a->cpu_bus = m6502_init(&a->cpu, &(m6502_desc_t){0});
    a->cpu_data = 0;
    a->phi0 = false;
    a->in_reset = true;
    a->waiting = false;
    b->no_signal = true;
    memset(b->screen, 0, A2600_SCREEN_W * A2600_SCREEN_H * sizeof(*b->screen));
}

static void atari_cpu_clock(a2600_t *a, part_t *cpu) {
    sim_t *s = &a->b.sim;
    if (cpu->dead) return;
    bool phi0 = pin_in(s, cpu, 27);
    if (a->phi0 == phi0) return;
    a->phi0 = phi0;

    /* Fetch the previous read before PHI2 falls. Hold write signals through
     * the falling edge, when the TIA latches its registers. */
    if (!phi0 && (a->cpu_bus & M6502_RW))
        a->cpu_data = (uint8_t)part_read_bus(s, cpu, atari_cpu_data, 8);
    if (phi0) {
        a->in_reset = !pin_in(s, cpu, 1);
        a->waiting = !pin_in(s, cpu, 3);
        if (a->in_reset) {
            a->cpu_bus = M6502_RW | M6502_RES;
        } else {
            uint64_t pins = a->cpu_bus & ~M6502_RDY;
            if (a->waiting) pins |= M6502_RDY;
            M6502_SET_DATA(pins, a->cpu_data);
            a->cpu_bus = m6502_tick(&a->cpu, pins);
        }
        bool read = (a->cpu_bus & M6502_RW) != 0;
        part_drive_bus(s, cpu, atari_cpu_addr, 13, M6502_GET_ADDR(a->cpu_bus), true);
        pin_out(s, cpu, 26, DRV_BIT(read));
        part_drive_bus(s, cpu, atari_cpu_data, 8, M6502_GET_DATA(a->cpu_bus), !read);
        sim_settle(s);
    }
    pin_out(s, cpu, 28, DRV_BIT(phi0));
}

static void atari_run(board_t *b, int cycles) {
    a2600_t *a = (a2600_t *)b;
    sim_t *s = &b->sim;
    part_t *cpu = &s->parts[a->cpu_part], *tia = &s->parts[a->tia_part];
    part_t *osc = &s->parts[a->oscillator], *por = &s->parts[a->por];
    for (int cycle = 0; cycle < cycles; cycle++, s->cycle++) {
        pin_out(s, por, 3, DRV_BIT(s->cycle >= 8));
        for (int phase = 0; phase < 6; phase++) {
            pin_out(s, osc, 3, DRV_BIT(!(phase & 1)));
            sim_settle(s);
            a2600_tia_clock(s, tia);
            sim_settle(s);
            atari_cpu_clock(a, cpu);
            sim_settle(s);
            sim_commit(s);
            sim_settle(s);
            a2600_tia_video(s, tia);
            board_sample(b);
        }
    }
}

static void atari_close(sim_t *s, const part_t *p, int signal, int ground) {
    const pin_t *a = &s->pins[part_pin(p, signal)], *b = &s->pins[part_pin(p, ground)];
    if (!p->removed && a->fault != PF_OPEN && b->fault != PF_OPEN) sim_join(s, a->net, b->net);
}

static void atari_joins(board_t *b) {
    a2600_t *a = (a2600_t *)b;
    static const int joystick_pins[] = { 1, 2, 3, 4, 6 };
    for (int i = 0; i < 5; i++)
        if (a->keys & (1u << i)) atari_close(&b->sim, &b->sim.parts[a->controls[0]], joystick_pins[i], 8);
    if (a->keys & (1u << A2600_SELECT)) atari_close(&b->sim, &b->sim.parts[a->controls[1]], 1, 2);
}

static void atari_key(board_t *b, int key, bool pressed) {
    if (key <= 0 || key > A2600_SELECT + 1) return;
    a2600_t *a = (a2600_t *)b;
    unsigned bit = 1u << (key - 1), previous = a->keys;
    if (pressed) a->keys |= bit;
    else a->keys &= ~bit;
    if (a->keys != previous) board_rebuild(b);
}

static void atari_key_down(board_t *b, int key) { atari_key(b, key, true); }
static void atari_key_up(board_t *b, int key) { atari_key(b, key, false); }

static int atari_host_key(int key) {
    switch (key) {
        case SAPP_KEYCODE_UP: return A2600_UP + 1;
        case SAPP_KEYCODE_DOWN: return A2600_DOWN + 1;
        case SAPP_KEYCODE_LEFT: return A2600_LEFT + 1;
        case SAPP_KEYCODE_RIGHT: return A2600_RIGHT + 1;
        case SAPP_KEYCODE_SPACE: return A2600_FIRE + 1;
        case SAPP_KEYCODE_TAB: return A2600_SELECT + 1;
        default: return 0;
    }
}

static void atari_monitor(board_t *b, uint32_t *out) {
    a2600_t *a = (a2600_t *)b;
    a2600_tia_monitor(b, &b->sim.parts[a->tia_part], out);
}

static a2600_cart_t *atari_cart(board_t *b) { return b->sim.parts[((a2600_t *)b)->cart_part].state; }

static const file_kind_t atari_cart_file = { "cartridge", "Plug in cartridge", false, ROLE_ROM };

static const file_kind_t *atari_file_kind(const char *path, char *info, int len, bool *bad) {
    const file_kind_t *state = board_state_file_kind(path, info, len, bad);
    if (state) return state;
    const char *ext = strrchr(board_basename(path), '.');
    if (!ext || (strcasecmp(ext, ".a26") && strcasecmp(ext, ".bin"))) return NULL;
    FILE *f = fopen(path, "rb");
    long size = -1;
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
        fclose(f);
    }
    *bad = size != 2048 && size != 4096;
    snprintf(info, len, *bad ? "Expected a linear 2 or 4 KiB ROM" : "Linear %ld KiB ROM", size / 1024);
    return &atari_cart_file;
}

static bool atari_load(board_t *b, const char *path, bool run, char *msg, int len) {
    a2600_cart_t image = {0};
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, len, "Can't open %s", path); return false; }
    size_t size = fread(image.data, 1, sizeof(image.data), f);
    bool valid = (size == 2048 || size == 4096) && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (!valid) { snprintf(msg, len, "%s: expected a linear 2 or 4 KiB ROM", path); return false; }
    image.size = (unsigned)size;
    snprintf(image.name, sizeof(image.name), "%s", board_basename(path));
    *atari_cart(b) = image;
    board_config_changed(b);
    board_power_on(b);
    snprintf(msg, len, "Plugged in %s (%u KiB)", image.name, image.size / 1024);
    return true;
}

static const char *atari_cart_name(board_t *b) {
    const a2600_cart_t *cart = atari_cart(b);
    return cart->size ? cart->name : NULL;
}

static void atari_remove_cart(board_t *b) {
    memset(atari_cart(b), 0, sizeof(a2600_cart_t));
    board_config_changed(b);
    board_power_on(b);
}

static const char *atari_cpu_status(board_t *b) {
    a2600_t *a = (a2600_t *)b;
    if (b->sim.parts[a->cpu_part].dead) return "unpowered";
    if (a->in_reset) return "in reset";
    return a->waiting ? "waiting (RDY low)" : "running";
}

static void atari_cpu_regs(board_t *b, char *buf, int len) {
    m6502_t *cpu = &((a2600_t *)b)->cpu;
    snprintf(buf, len, "PC=%04X A=%02X X=%02X Y=%02X SP=%02X P=%02X",
             m6502_pc(cpu), m6502_a(cpu), m6502_x(cpu), m6502_y(cpu), m6502_s(cpu), m6502_p(cpu));
}

static void atari_print_text(board_t *b) {
    char text[80];
    atari_cpu_regs(b, text, sizeof(text));
    puts(text);
}

static int atari_ram_cell(board_t *b, int part, uint16_t addr) {
    return part == ((a2600_t *)b)->riot_part && (addr & 0x1280) == 0x0080 ? addr & 0x7f : -1;
}

const machine_t machine_a2600pal = {
    .id = "a2600pal", .name = "Atari 2600 PAL", .board_name = "Atari 2600A PAL digital bench",
    .size = sizeof(a2600_t), .data = &bd_a2600pal,
    .sim = { atari_rail, atari_kind, PAL_OSC_HZ / 3000000.0 },
    .cpu_hz = PAL_OSC_HZ / 3, .cycles_per_frame = PAL_CYCLES_PER_FRAME, .steps_per_cycle = 6,
    .screen_w = A2600_SCREEN_W, .screen_h = A2600_SCREEN_H,
    .parts = atari_parts, .nparts = sizeof(atari_parts) / sizeof(*atari_parts),
    .conns = atari_conns, .nconns = sizeof(atari_conns) / sizeof(*atari_conns),
    .keys_help = "Arrows: left joystick. Space: fire. Tab: game select.",
    .reset_help = "Close the Atari GAME RESET switch on the RIOT port",
    .create = atari_create, .power_on = atari_power_on, .run = atari_run, .joins = atari_joins,
    .key_down = atari_key_down, .key_up = atari_key_up, .host_key = atari_host_key,
    .monitor = atari_monitor, .files_title = "Atari 2600 cartridges", .file_kind = atari_file_kind,
    .cart_slot = "CART", .cart_name = atari_cart_name, .remove_cart = atari_remove_cart,
    .load = atari_load, .cpu_status = atari_cpu_status, .cpu_regs = atari_cpu_regs,
    .print_text = atari_print_text, .ram_cell = atari_ram_cell
};
