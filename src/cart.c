/* cart.c - cartridges (.CRT files, raw .rom/.bin images) in the expansion port.
 *
 * A cartridge is modeled for what it is electrically: something that
 * drives pins of the expansion port CN6. It ties /GAME and/or /EXROM to
 * ground (that is how it tells the PLA the memory map it wants), and puts
 * the byte of its ROM on D0-D7 while the PLA pulls /ROML or /ROMH low. The
 * cartridge pins are CN6's pins, so a bad contact on CN6 (a fault on one
 * of its pins) is a bad contact for the cartridge too.
 *
 * Everything else comes from the board. In Ultimax mode (/GAME low, /EXROM
 * high, like the Dead Test cartridge) the cartridge replaces the KERNAL at
 * $E000 and the PLA leaves holes where nothing answers, not even RAM. The
 * VIC reads the cartridge too: during its cycles the CPU lets go of A12-A15
 * and the RP4 pull-ups raise them, so the cartridge sees $F000-$FFFF.
 *
 * Supported: the plain cartridges (CRT hardware type 0: 8K, 16K, Ultimax),
 * which is what the diagnostic cartridges are. Cartridges with bank
 * switching (type 1 and up) are refused with a message.
 *
 * Raw ROM images (.rom, .bin: an EPROM dump, no header) are taken too. The
 * wiring is guessed from the contents, bank by bank (8K each): "CBM80" at
 * offset 4 is an autostart cartridge at $8000 (8K, or 16K when a second
 * bank without signature follows); otherwise a reset vector into
 * $E000-$FFFF is an Ultimax ROM (Dead Test, DesTestMAX). An image of
 * several banks is a DesTestSwitch EPROM: its upper address lines go to a
 * switch, so one bank at a time is seen; flipping it restarts the C64. An
 * 8K image can also go in the KERNAL socket U4 instead of the port. */
#include "c64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { MODE_8K, MODE_16K, MODE_ULTIMAX };
#define RAW_MAX_BANKS 128       /* 1 MB of 8K banks */

struct cart {
    char name[33];
    bool exrom, game;           /* lines the cartridge holds low */
    bool has_roml, has_romh;
    uint8_t roml[8192], romh[8192];
    /* CN6 pins, by function. */
    int pin_roml, pin_romh, pin_game, pin_exrom, pin_a[13], pin_d[8];
    /* A raw image (nbanks > 0; 0 for a .crt): the wiring each 8K bank asks
     * for, the bank the switch selects, and whether the image sits in the
     * KERNAL socket U4 instead of the expansion port. */
    int nbanks, bank;
    bool socket;
    /* A 16K BASIC + KERNAL image (a 27128 for the 250469's U4): one bank,
     * always in U4, its KERNAL the high half. u4 is what a 250469's U4
     * holds: BASIC, then the KERNAL in use. */
    bool basic_kernal;
    uint8_t u4[16384];
    uint8_t mode[RAW_MAX_BANKS];
    uint8_t image[];            /* nbanks * 8K */
};

/* The pad of CN6 for every line the cartridge uses (the expansion port
 * pinout: data on 14-21, address on F-Y). */
static const char *const pad_a[13] = { "Y", "X", "W", "V", "U", "T", "S", "R", "P", "N", "M", "L", "K" };
static const char *const pad_d[8] = { "21", "20", "19", "18", "17", "16", "15", "14" };

static int cn6_pin(board_t *b, const char *pad) { return board_pad_pin(b, C64(b)->p_cn6, pad); }

/* The ROM selected by the PLA drives the data bus; the address is A0-A12
 * (8K). If a fault pulls both lines low, the ROML chip answers: on a real
 * cartridge it depends on how it is wired. */
static void cart_eval(sim_t *s, part_t *p) {
    struct cart *c = p->state;
    const uint8_t *rom = NULL;
    if (c->has_roml && !sim_read(s, c->pin_roml)) rom = c->roml;
    else if (c->has_romh && !sim_read(s, c->pin_romh)) rom = c->romh;
    unsigned v = 0;
    if (rom) {
        unsigned a = 0;
        for (int i = 0; i < 13; i++) a |= (unsigned)sim_read(s, c->pin_a[i]) << i;
        v = rom[a];
    }
    for (int i = 0; i < 8; i++) sim_drive(s, c->pin_d[i], rom ? DRV_BIT((v >> i) & 1) : DRV_Z);
}

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned long be32(const uint8_t *p) {
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

static struct cart *crt_parse(const uint8_t *buf, size_t n, const char *path, char *msg, int msglen);

/* Read a .CRT file (the VICE format). */
static struct cart *crt_load(const char *path, char *msg, int msglen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, msglen, "can't open %s", path); return NULL; }
    /* Not static: boards may plug cartridges in parallel threads. */
    uint8_t *buf = malloc(1 << 20);
    size_t n = buf ? fread(buf, 1, 1 << 20, f) : 0;
    fclose(f);
    struct cart *c = NULL;
    if (n < 0x40 || memcmp(buf, "C64 CARTRIDGE   ", 16))
        snprintf(msg, msglen, "%s is not a .CRT cartridge", path);
    else c = crt_parse(buf, n, path, msg, msglen);
    free(buf);
    return c;
}

static struct cart *crt_parse(const uint8_t *buf, size_t n, const char *path, char *msg, int msglen) {
    unsigned type = be16(buf + 0x16);
    char name[33];
    memcpy(name, buf + 0x20, 32);
    name[32] = 0;
    if (type != 0) {
        snprintf(msg, msglen, "%s: cartridge type %u has bank switching, not supported (only plain "
                 "8K/16K/Ultimax ones)", name, type);
        return NULL;
    }
    struct cart *c = calloc(1, sizeof(*c));
    memcpy(c->name, name, sizeof(c->name));
    if (!c->name[0]) {      /* no name in the file: use the file name */
        const char *base = board_basename(path);
        snprintf(c->name, sizeof(c->name), "%.*s", (int)strcspn(base, "."), base);
    }
    /* The header gives the levels of the two lines: 0 means held low. */
    c->exrom = buf[0x18] == 0;
    c->game = buf[0x19] == 0;
    size_t off = be32(buf + 0x10);
    while (off + 16 <= n && !memcmp(buf + off, "CHIP", 4)) {
        unsigned long len = be32(buf + off + 4);
        unsigned load = be16(buf + off + 12), size = be16(buf + off + 14);
        if (len < 16 || off + 16 + size > n) break;
        const uint8_t *data = buf + off + 16;
        /* $8000: ROML (a 16K image continues into ROMH); $A000 or $E000 and
         * up: ROMH. A smaller chip repeats over the 8K window (its upper
         * address lines are not connected): a 4K ROM at $F000 is also seen
         * at $E000. */
        unsigned m = size < 8192 ? size : 8192;
        if (m && load == 0x8000) {
            for (unsigned i = 0; i < 8192; i++) c->roml[i] = data[i % m];
            c->has_roml = true;
            if (size > 8192) {
                for (unsigned i = 0; i < 8192; i++) c->romh[i] = data[8192 + i % (size - 8192)];
                c->has_romh = true;
            }
        } else if (m && (load == 0xA000 || load >= 0xE000)) {
            unsigned base = load >= 0xE000 ? load - 0xE000 : 0;
            for (unsigned i = 0; i < 8192; i++) c->romh[i] = data[(i + 8192 - base) % m];
            c->has_romh = true;
        }
        off += len;
    }
    if (!c->has_roml && !c->has_romh) {
        snprintf(msg, msglen, "%s: no ROM in the file", c->name);
        free(c);
        return NULL;
    }
    return c;
}

/* ------------------------------------------------------------------------ */
/* Raw ROM images                                                            */
/* ------------------------------------------------------------------------ */

static bool has_cbm80(const uint8_t *d) { return !memcmp(d + 4, "\xC3\xC2\xCD\x38\x30", 5); }
static unsigned vec(const uint8_t *d, unsigned off) { return d[off] | (unsigned)d[off + 1] << 8; }
/* The 6502 vectors at the end of the bank: in Ultimax mode it is seen at
 * $E000-$FFFF, so a reset vector there is the CPU's way in. 'strict' asks
 * for NMI and IRQ there too (telling a C64 ROM from random bytes). */
static bool ultimax_vectors(const uint8_t *d, bool strict) {
    return vec(d, 0x1FFC) >= 0xE000 && (!strict || (vec(d, 0x1FFA) >= 0xE000 && vec(d, 0x1FFE) >= 0xE000));
}
/* A KERNAL: the jump table at $FF81-$FFF3, JMP or JMP () in nearly every
 * slot (Dead Test and the like don't have one). */
static bool kernal_like(const uint8_t *d) {
    int jumps = 0;
    for (unsigned a = 0x1F81; a <= 0x1FF3; a += 3) jumps += d[a] == 0x4C || d[a] == 0x6C;
    return jumps >= 30 && ultimax_vectors(d, true);
}

/* The image without its load address ($8000 or $E000 in front, .prg
 * style: 8K * N + 2 bytes). */
static const uint8_t *raw_strip(const uint8_t *buf, size_t *n) {
    if (*n > 2 && *n % 8192 == 2) { *n -= 2; return buf + 2; }
    return buf;
}

/* How many 8K banks (0: not a C64 ROM size). A 2K or 4K ROM repeats over
 * the 8K, its top address lines not connected: one bank. */
static int raw_banks(size_t n) {
    if (n == 2048 || n == 4096) return 1;
    return n && n % 8192 == 0 && n / 8192 <= RAW_MAX_BANKS ? (int)(n / 8192) : 0;
}

static void raw_mirror(const uint8_t *d, size_t n, uint8_t *out, int nbanks) {
    if (n < 8192) for (unsigned i = 0; i < 8192; i++) out[i] = d[i % n];
    else memcpy(out, d, (size_t)nbanks * 8192);
}

static int bank_mode(const uint8_t *d) {
    if (has_cbm80(d)) return MODE_8K;
    if (ultimax_vectors(d, false)) return MODE_ULTIMAX;
    return MODE_8K;             /* no autostart: a plain ROM at $8000 */
}

/* Two banks, BASIC (no autostart) then a KERNAL: a 27128 burned for the
 * 250469's U4 (BASIC at $A000, the KERNAL at $E000). */
static bool raw_is_basic_kernal(const uint8_t *d, int nbanks) {
    return nbanks == 2 && !has_cbm80(d) && kernal_like(d + 8192);
}

/* Two banks, the first an autostart one, the second neither autostart nor
 * Ultimax: a 16K cartridge (ROML + ROMH), not a switch of two. */
static bool raw_is_16k(const uint8_t *d, int nbanks) {
    return nbanks == 2 && has_cbm80(d) && !has_cbm80(d + 8192) && !ultimax_vectors(d + 8192, true);
}

bool rom_image_is_c64(const uint8_t *buf, size_t n) {
    const uint8_t *d = raw_strip(buf, &n);
    int nb = raw_banks(n);
    if (!nb) return false;
    if (n < 8192 || has_cbm80(d)) return true;
    for (int i = 0; i < nb; i++)
        if (!has_cbm80(d + i * 8192) && !ultimax_vectors(d + i * 8192, true)) return false;
    return true;
}

bool c64_raw_rom_info(const uint8_t *buf, size_t n, char *info, int len) {
    size_t full = n;
    const uint8_t *d = raw_strip(buf, &n);
    int nb = raw_banks(n);
    if (!nb) { snprintf(info, len, "%zu bytes: not 8K banks", full); return false; }
    uint8_t one[8192];
    if (n < 8192) { raw_mirror(d, n, one, 1); d = one; }
    const char *pfx = full != n ? ", load address skipped" : "";
    if (!rom_image_is_c64(buf, full) && n % 16384 == 0) {
        snprintf(info, len, "%zuK, no CBM80 or 6502 vectors: a Spectrum ROM", n / 1024);
        return false;
    }
    if (raw_is_basic_kernal(d, nb)) {
        snprintf(info, len, "BASIC + KERNAL, 16K (for U4 of the 250469)%s", pfx);
        return true;
    }
    if (nb > 1 && !raw_is_16k(d, nb)) {
        int ult = 0;
        for (int i = 0; i < nb; i++) ult += bank_mode(d + i * 8192) == MODE_ULTIMAX;
        snprintf(info, len, "switch ROM: %d banks of 8K (%d Ultimax)%s", nb, ult, pfx);
        return true;
    }
    const char *kind = nb == 2 ? "16K cartridge" : kernal_like(d) ? "KERNAL (for U4)" :
                       has_cbm80(d) ? "8K cartridge" : bank_mode(d) == MODE_ULTIMAX ? "Ultimax ROM" :
                       "8K ROM, no autostart";
    snprintf(info, len, "%s%s", kind, pfx);
    return true;
}

/* Read a raw image: every bank and the wiring it asks for. */
static struct cart *raw_load(const char *path, char *msg, int msglen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(msg, msglen, "can't open %s", path); return NULL; }
    size_t cap = (size_t)RAW_MAX_BANKS * 8192 + 3;
    uint8_t *buf = malloc(cap);
    size_t n = buf ? fread(buf, 1, cap, f) : 0;
    fclose(f);
    const uint8_t *d = raw_strip(buf, &n);
    int nb = raw_banks(n);
    if (!nb) {
        snprintf(msg, msglen, "%s: %zu bytes, a C64 ROM image is 2K, 4K or N x 8K (+2 for a load address)",
                 path, n);
        free(buf);
        return NULL;
    }
    struct cart *c = calloc(1, sizeof(*c) + (size_t)nb * 8192);
    raw_mirror(d, n, c->image, nb);
    free(buf);
    const char *base = path;
    for (const char *q = path; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
    const char *dot = strrchr(base, '.');
    snprintf(c->name, sizeof(c->name), "%.*s", dot ? (int)(dot - base) : 32, base);
    if (raw_is_16k(c->image, nb)) { c->nbanks = 1; c->mode[0] = MODE_16K; }
    else if (raw_is_basic_kernal(c->image, nb)) {
        c->nbanks = 1;
        c->mode[0] = MODE_8K;
        c->basic_kernal = c->socket = true;
        return c;
    } else {
        c->nbanks = nb;
        for (int i = 0; i < nb; i++) c->mode[i] = (uint8_t)bank_mode(c->image + i * 8192);
    }
    /* A KERNAL goes where a KERNAL goes. */
    c->socket = c->nbanks == 1 && c->mode[0] != MODE_16K && kernal_like(c->image);
    return c;
}

/* The selected bank on the cartridge's ROM lines (none in the socket). */
static void raw_select(struct cart *c) {
    const uint8_t *d = c->image + (size_t)c->bank * 8192;
    int mode = c->mode[c->bank];
    c->has_roml = c->has_romh = c->exrom = c->game = false;
    if (c->socket) return;
    if (mode == MODE_ULTIMAX) {
        memcpy(c->romh, d, 8192);
        c->has_romh = c->game = true;
    } else {
        memcpy(c->roml, d, 8192);
        c->has_roml = c->exrom = true;
        if (mode == MODE_16K) { memcpy(c->romh, d + 8192, 8192); c->has_romh = c->game = true; }
    }
}

/* Pin flags and drives for the cartridge, or back to a bare connector. A
 * ROM in the KERNAL socket leaves the connector bare. */
static void plug(board_t *b, struct cart *c) {
    sim_t *s = &b->sim;
    board_config_changed(b);
    /* No evaluation of the old wiring may be left pending: CN6 may lose
     * its eval below (plugged, then moved to U4 before the C64 ran). */
    sim_settle(s);
    part_t *p = &s->parts[C64(b)->p_cn6];
    struct cart *old = p->state;
    struct cart *k = c ? c : old;
    bool in = c && !c->socket;
    int ins[15], nin = 0, outs[10], nout = 0;
    ins[nin++] = k->pin_roml;
    ins[nin++] = k->pin_romh;
    for (int i = 0; i < 13; i++) ins[nin++] = k->pin_a[i];
    for (int i = 0; i < 8; i++) outs[nout++] = k->pin_d[i];
    outs[nout++] = k->pin_game;
    outs[nout++] = k->pin_exrom;
    for (int i = 0; i < nin; i++) if (ins[i] >= 0) s->pins[ins[i]].flags = in ? PIN_IN | PIN_SENSE : 0;
    for (int i = 0; i < nout; i++) if (outs[i] >= 0) {
        s->pins[outs[i]].flags = in ? PIN_OUT : 0;
        s->pins[outs[i]].drive = DRV_Z;
    }
    /* A wire from the line to ground on the cartridge. */
    if (in && c->game && c->pin_game >= 0) s->pins[c->pin_game].drive = DRIVE(S_SUPPLY, 0);
    if (in && c->exrom && c->pin_exrom >= 0) s->pins[c->pin_exrom].drive = DRIVE(S_SUPPLY, 0);
    p->state = c;
    p->state_size = 0;          /* a save state leaves the cartridge alone */
    p->eval = in ? cart_eval : NULL;
    C64(b)->cart = c;
    /* In U4: an 8K KERNAL, or the high half of a BASIC + KERNAL image. On
     * the 250469 U4 is a 27128 with BASIC too: BASIC + that KERNAL, as an
     * owner burns one (stock BASIC unless the image has its own). On the
     * 250407 a BASIC + KERNAL image's BASIC is left out (U3 holds BASIC). */
    const uint8_t *rom = !c || !c->socket ? NULL : c->basic_kernal ? c->image + 8192 : c->image + (size_t)c->bank * 8192;
    if (rom && board_is_250469(b)) {
        if (c->basic_kernal) memcpy(c->u4, c->image, 16384);
        else c64_basic_kernal(c->u4, rom);
        rom = c->u4;
    }
    c64_set_kernal(b, rom);
}

/* What is plugged, and how, for the messages. */
static void describe(const struct cart *c, char *msg, int msglen) {
    if (c->socket) snprintf(msg, msglen, "ROM \"%s\"%s in the KERNAL socket U4", c->name,
                            c->basic_kernal ? " (BASIC + KERNAL)" : "");
    else snprintf(msg, msglen, "cartridge \"%s\": %s%s%s", c->name,
                  c->game && !c->exrom ? "Ultimax" : c->game ? "16K" : "8K",
                  c->has_roml ? ", ROML" : "", c->has_romh ? ", ROMH" : "");
    size_t l = strlen(msg);
    if (c->nbanks > 1 && l < (size_t)msglen)
        snprintf(msg + l, msglen - l, ", bank %d of %d", c->bank, c->nbanks);
}

/* A .crt, or a raw .rom/.bin image. */
bool c64_insert_cart(board_t *b, const char *path, char *msg, int msglen) {
    if (!board_is_c64(b)) { snprintf(msg, msglen, "cartridges go in the C64"); return false; }
    size_t pl = strlen(path);
    bool crt = pl > 4 && !strcasecmp(path + pl - 4, ".crt");
    struct cart *c = crt ? crt_load(path, msg, msglen) : raw_load(path, msg, msglen);
    if (!c) return false;
    if (c->nbanks) raw_select(c);
    c->pin_roml = cn6_pin(b, "11");
    c->pin_romh = cn6_pin(b, "B");
    c->pin_game = cn6_pin(b, "8");
    c->pin_exrom = cn6_pin(b, "9");
    for (int i = 0; i < 13; i++) c->pin_a[i] = cn6_pin(b, pad_a[i]);
    for (int i = 0; i < 8; i++) c->pin_d[i] = cn6_pin(b, pad_d[i]);
    c64_remove_cart(b);
    plug(b, c);
    /* Cartridges go in with the power off: switch the C64 on again. */
    board_rebuild(b);
    board_power_on(b);
    describe(c, msg, msglen);
    return true;
}

void c64_remove_cart(board_t *b) {
    if (!board_is_c64(b)) return;
    struct cart *c = C64(b)->cart;
    if (!c) return;
    plug(b, NULL);
    free(c);
    board_rebuild(b);
    board_power_on(b);
}

const char *c64_cart_name(board_t *b) {
    if (!board_is_c64(b) || !C64(b)->cart) return NULL;
    return ((struct cart *)C64(b)->cart)->name;
}

/* The switch of a ROM made of several banks, and where an 8K image sits.
 * Both are changed with the power off, like the real thing. */
static struct cart *raw_cart(board_t *b) {
    struct cart *c = board_is_c64(b) ? C64(b)->cart : NULL;
    return c && c->nbanks ? c : NULL;
}

int c64_cart_banks(board_t *b) { struct cart *c = raw_cart(b); return c ? c->nbanks : 0; }
int c64_cart_bank(board_t *b) { struct cart *c = raw_cart(b); return c ? c->bank : 0; }

const char *c64_cart_bank_info(board_t *b, int bank) {
    struct cart *c = raw_cart(b);
    if (!c || bank < 0 || bank >= c->nbanks) return "";
    const uint8_t *d = c->image + (size_t)bank * 8192;
    return c->mode[bank] == MODE_ULTIMAX ? "Ultimax, $E000-$FFFF" :
           c->mode[bank] == MODE_16K ? "16K, $8000-$BFFF" :
           has_cbm80(d) ? "8K, $8000-$9FFF, CBM80 autostart" : "8K, $8000-$9FFF, no autostart";
}

static void raw_remount(board_t *b, struct cart *c) {
    raw_select(c);
    plug(b, c);
    board_rebuild(b);
    board_power_on(b);
}

void c64_set_cart_bank(board_t *b, int bank) {
    struct cart *c = raw_cart(b);
    if (!c || bank < 0 || bank >= c->nbanks) return;
    c->bank = bank;
    raw_remount(b, c);
}

bool c64_cart_in_u4(board_t *b) { struct cart *c = raw_cart(b); return c && c->socket; }

/* A BASIC + KERNAL image stays in U4; a 16K cartridge in the port. */
int c64_cart_socket(board_t *b) {
    struct cart *c = raw_cart(b);
    if (!c || c->mode[c->bank] == MODE_16K || c->basic_kernal) return -1;
    return c->socket;
}

void c64_set_cart_socket(board_t *b, bool u4) {
    struct cart *c = raw_cart(b);
    if (!c || c->mode[c->bank] == MODE_16K || c->basic_kernal || c->socket == u4) return;
    c->socket = u4;
    raw_remount(b, c);
}
