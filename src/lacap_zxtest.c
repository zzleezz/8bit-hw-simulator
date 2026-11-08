/* lacap_zxtest.c - --la-diagnose DIR --la-zxtest: a Spectrum running our
 * test ROM (zxtest/zxtest.asm) from a cartridge, read off the edge
 * connector by the logic analyser.
 *
 * The ROM loops for ever through nine tests (and, from version 5, three
 * more results inside tests 3-5), each writing its result to
 * RES+4n in the ROM area (status 00 OK / 01 FAIL / 02 none, bits, then an
 * address or count, high byte first): the analyser is armed on that
 * address any time, no reset. The ROM judges its results itself (its
 * counts were checked on every simulated issue and a real Issue 2), so
 * no working board is simulated to compare with; tests 0 and 1 are read
 * from the bus itself (the data and address lines). What a result says
 * is put into parts with the board's own map (which chip holds which bit).
 *
 * AI-generated (Claude Code). */
#include "lacap_int.h"
#include "filedialog.h"

#define ZT_N 12                 /* the most results (the arrays): 9-11 come within tests 3-5 (version 5) */

/* The ROM's constants, from its table at 00A0 ("ZTAB", version 4 on: see
 * zxtest.asm); an image without one (versions 1-3) had these. */
static struct {
    int version, ntests;
    unsigned mark;              /* test n's start: a write of n to MARK+n */
    unsigned res;               /* test n's result: four writes at RES+4n */
    unsigned alive;             /* test 0's 20 writes; the reads below it are the image's */
    unsigned csum;              /* the ROM's stored sum (not used here) */
    unsigned t1;                /* test 1's page: its LD A,(nn) operands */
    int frame, cont, nocont, ntol;
} zt = { 3, 9, 0x3FA0, 0x3FC0, 0x3F80, 0x3F7E, 0x0300, 2912, 2209, 2254, 8 };

static void zt_consts(const uint8_t *rom) {
    if (memcmp(rom + 0xA0, "ZTAB", 4)) {
        zt.version = rom[0x77] - '0';
        return;
    }
    const uint8_t *t = rom + 0xA4;
#define W(i) (unsigned)(t[2 + 2 * (i)] | t[3 + 2 * (i)] << 8)
    zt.version = t[0];
    zt.ntests = t[1] < ZT_N ? t[1] : ZT_N;
    zt.mark = W(0); zt.res = W(1); zt.alive = W(2); zt.csum = W(3); zt.t1 = W(4);
    zt.frame = (int)W(5); zt.cont = (int)W(6); zt.nocont = (int)W(7); zt.ntol = (int)W(8);
#undef W
}

static const char *const zt_name[ZT_N] = {
    "alive (the data lines)", "the address lines", "the ROM's sum", "the lower RAM (4000-7FFF)",
    "the upper RAM (8000-FFFF)", "the ULA's ports (keyboard)", "the interrupts", "contention (the ULA's timing)",
    "the upper RAM's refresh", "the lower RAM's address lines", "the upper RAM's address lines", "the EAR input" };

/* The order the results are judged and listed in: each bank's address
 * lines before its march (an address line wrong fails the march too). */
static const int zt_order[ZT_N] = { 0, 1, 2, 9, 3, 10, 4, 5, 11, 6, 7, 8 };
/* The captures: the first from test 0's start (tests 0 and 1 and their
 * results fit in it), then each result, in the order the ROM writes them
 * (one turn of its loop can give them all). A capture's stage box in the
 * window is its test's: 9 and 10 (the address lines) go with the RAM
 * tests they run in, 11 (EAR) with the ULA's ports. Results 9-11 came with
 * version 5: an older image's captures are the first eight. */
#define ZT_NCAP 11
static const struct { int test, box; } zt_cap[ZT_NCAP] = {
    { 0, 0 }, { 2, 1 }, { 9, 2 }, { 3, 2 }, { 10, 3 }, { 4, 3 }, { 5, 4 }, { 11, 4 }, { 6, 5 }, { 7, 6 }, { 8, 7 } };
static int cap_test(int k) { return zt_cap[k].test; }
static int cap_trig(int k) { return k == 0 ? (int)zt.mark : (int)(zt.res + 4 * cap_test(k)); }
static void cap_file(const char *dir, int k, char *out, int len) {
    if (k == 0) snprintf(out, len, "%s/zxtest-start.lac", dir);
    else snprintf(out, len, "%s/zxtest%d.lac", dir, cap_test(k));
}

/* The test ROM's image: the cartridge named on the command line, else
 * zxtest.rom in the carts folder (here, or beside the program). */
static bool zt_rom(const la_cli_t *o, uint8_t *rom, char *path, int plen) {
    char exe[600] = "";
    filedialog_exe_dir(exe, sizeof(exe));
    char cand[6][800];
    int n = 0;
    if (o->cart) snprintf(cand[n++], sizeof(cand[0]), "%s", o->cart);
    snprintf(cand[n++], sizeof(cand[0]), "%s/zxtest.rom", o->carts ? o->carts : "carts");
    if (exe[0]) {
        snprintf(cand[n++], sizeof(cand[0]), "%s/carts/zxtest.rom", exe);
        snprintf(cand[n++], sizeof(cand[0]), "%s/zxtest/zxtest.rom", exe);
    }
    snprintf(cand[n++], sizeof(cand[0]), "zxtest/zxtest.rom");
    for (int i = 0; i < n; i++) {
        FILE *f = fopen(cand[i], "rb");
        if (!f) continue;
        size_t got = fread(rom, 1, 16384, f);
        fclose(f);
        if (got == 16384 && !memcmp(rom + 0x70, "ZXTEST", 6)) {
            snprintf(path, plen, "%.799s", cand[i]);
            return true;
        }
    }
    return false;
}

/* An older version V of the test ROM, zxtest-vV.rom beside the image at
 * PATH (kept so that captures of a cartridge not yet reprogrammed are
 * still compared byte for byte): into ROM, its path into OUT. */
static bool zt_old_rom(const char *path, int v, uint8_t *rom, char *out, int olen) {
    char dir[900];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
    if (bs > sl) sl = bs;
    if (sl) *sl = 0;
    else snprintf(dir, sizeof(dir), ".");
    snprintf(out, olen, "%.800s/zxtest-v%d.rom", dir, v);
    FILE *f = fopen(out, "rb");
    if (!f) return false;
    static uint8_t buf[16384];
    size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (got != sizeof(buf) || memcmp(buf + 0x70, "ZXTEST", 6) || buf[0x77] - '0' != v) return false;
    memcpy(rom, buf, sizeof(buf));
    return true;
}

/* Test N's four result bytes: the first write to RES+4N followed by
 * writes to the next three (a fetch or read there is not a result: a lost
 * CPU sliding through memory shows those addresses too). */
static bool zt_result(const la_cycle_t *cy, int n, int test, uint8_t r[4]) {
    unsigned a = zt.res + 4 * test;
    for (int i = 0; i < n; i++) {
        if (cy[i].kind != LA_WRITE || cy[i].addr != a) continue;
        int got = 0;
        for (int j = i; j < n && got < 4; j++) {
            if (cy[j].kind != LA_WRITE) continue;
            if (cy[j].addr != a + got) break;
            r[got++] = cy[j].data;
        }
        if (got == 4) return true;
    }
    return false;
}

/* The first cycle that isn't the master board's zero-filled start (the
 * address all 0 before the trigger when it fired at once). */
static int zt_first(const la_cycle_t *cy, int n) {
    int i = 0;
    while (i < n && cy[i].addr == 0) i++;
    return i;
}

typedef struct {
    bool have;
    int alive_seen, alive_wrong;    /* test 0's writes found, wrong */
    unsigned d_wrong, d_low, d_high; /* data bits wrong; read 0 for a 1, 1 for a 0 */
    int walk_seen;                  /* test 1's 32 reads found */
    unsigned a_wrong, a_low, a_high;
    int rom_ok, rom_bad;            /* reads of the cartridge right, wrong */
    unsigned rom_bits;
    int rom_first;
} zt_bus_t;

/* Tests 0 and 1 off the bus, and every read of the cartridge. */
static void zt_bus(const la_cycle_t *cy, int n, const uint8_t *rom, zt_bus_t *b) {
    memset(b, 0, sizeof(*b));
    b->rom_first = -1;
    int i0 = zt_first(cy, n);
    for (int i = i0; i < n; i++) {
        /* test 1's reads are of the address lines, not the ROM's bytes:
         * LD A,(nn) at 0300-03FF reads its operands from there */
#define T1(x) ((x) >= zt.t1 && (x) < zt.t1 + 0x100)
        bool walk = cy[i].kind == LA_READ && !T1(cy[i].addr) && i >= 2 && cy[i - 1].kind == LA_READ &&
                    T1(cy[i - 1].addr) && cy[i - 2].kind == LA_READ && T1(cy[i - 2].addr);
        if (walk) continue;
        if ((cy[i].kind == LA_FETCH || cy[i].kind == LA_READ) && cy[i].addr < zt.alive) {
            if (rom[cy[i].addr] == cy[i].data) b->rom_ok++;
            else {
                if (!b->rom_bad++) b->rom_first = cy[i].addr;
                b->rom_bits |= rom[cy[i].addr] ^ cy[i].data;
            }
        }
    }
    /* test 0: after its marker, the writes to 3F80-3F93 */
    static const uint8_t alive[20] = { 0x00, 0xFF, 0x55, 0xAA, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
                                       0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F };
    int s0 = -1, s1 = -1, e1 = -1;
    for (int i = i0; i < n; i++) {
        if (cy[i].kind != LA_WRITE) continue;
        if (cy[i].addr == zt.mark && s0 < 0) s0 = i;
        if (cy[i].addr == zt.mark + 1 && s0 >= 0 && s1 < 0) s1 = i;
        if (cy[i].addr == zt.res + 4 && s1 >= 0 && e1 < 0) e1 = i;
    }
    if (s0 < 0) return;
    b->have = true;
    int k = 0;
    for (int i = s0 + 1; i < n && k < 20; i++) {
        if (cy[i].kind != LA_WRITE || cy[i].addr == zt.mark) continue;
        if (cy[i].addr == zt.res) break;    /* test 0's result: past its writes */
        /* the k-th write of test 0 (where it went is test 1's business) */
        uint8_t want = alive[k], got = cy[i].data;
        b->alive_seen++;
        if (got != want) {
            b->alive_wrong++;
            b->d_wrong |= got ^ want;
            b->d_low |= want & ~got;
            b->d_high |= ~want & got;
        }
        k++;
    }
    if (s1 < 0) return;
    /* test 1: LD A,(nn) 32 times: fetch, two operand reads, the data read */
    int reads = 0;
    for (int i = s1 + 1; i < n && (e1 < 0 || i < e1); i++) {
        if (cy[i].kind == LA_FETCH && T1(cy[i].addr) && cy[i].data == 0x3A) reads = 0;
        if (cy[i].kind != LA_READ) continue;
        if (++reads != 3) continue;
        int w = b->walk_seen++;
        if (w >= 32) break;
        unsigned want = w < 16 ? 1u << w : 0xFFFFu ^ (1u << (w - 16)), got = cy[i].addr;
        b->a_wrong |= got ^ want;
        b->a_low |= want & ~got;
        b->a_high |= ~want & got & 0xFFFF;
    }
}

static void bits_list(unsigned m, int nbits, const char *pre, char *out, int len) {
    int n = 0;
    out[0] = 0;
    for (int i = 0; i < nbits && n < len; i++)
        if (m >> i & 1) n += snprintf(out + n, len - n, "%s%s%d", n ? " " : "", pre, i);
}

/* The board as built has no upper RAM (an Issue 1, or the 16K option). */
static bool zt_16k(const machine_t *m) {
    if (!vboard) return false;
    zx48_t *z = (zx48_t *)vboard;
    if (z->uram_part[0] < 0) return true;
    char o[160];
    options_text(m, o, sizeof(o));
    return strstr(o, "16K") != NULL;
}

/* A RAM test's verdict: the bad bits as chips. */
/* The data bits each bank's address lines test (9, 10) showed bad in
 * (zt_addr, judged before the march). */
static unsigned zt_addr_bits[2];

static int nbits(unsigned m) {
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

static int zt_ram(int item, bool upper, const uint8_t r[4]) {
    const char *bank = upper ? "upper RAM" : "lower RAM", *chips = upper ? "URAM" : "LRAM";
    unsigned bits = r[1], addr = r[2] << 8 | r[3];
    char b[64], buf[400], res[200];
    bits_list(bits, 8, "D", b, sizeof(b));
    /* An address line wrong fails the march in the same bits: said above */
    if (r[0] == 1 && bits && !(bits & ~zt_addr_bits[upper])) {
        printf("  %d. The %s's march fails in the same bits (%s, the first at %04X): explained by its\n"
               "     address lines above.\n", ++item, bank, b, addr);
        return item;
    }
    int nb = 0;
    for (int i = 0; i < 8; i++) nb += bits >> i & 1;
    if (nb >= 3) {
        bank_parts(bank, chips, buf, sizeof(buf));
        data_resistors(chips, res, sizeof(res));
        printf("  %d. The %s as a whole: bits %s wrong (the first at %04X). Not one chip: what all its chips\n"
               "     share: %s%s%s\n", ++item, bank, b, addr, buf[0] ? buf : "its address multiplexers and select",
               res[0] ? "; and for the data, the resistors to the CPU's data lines: " : "", res);
        return item;
    }
    for (int i = 0; i < 8; i++) {
        if (!(bits >> i & 1)) continue;
        char lab[8];
        snprintf(lab, sizeof(lab), "%s%d", chips, i);
        const char *ic = ref_of(lab);
        printf("  %d. %s, the %s's bit D%d: it didn't keep what was written (the first wrong at %04X).\n", ++item,
               ic ? ic : "the chip", bank, i, addr);
        if (!upper) {
            data_resistors_of(chips, 1u << i, res, sizeof(res));
            if (res[0]) printf("     (Or %s, the resistor between it and the CPU's D%d.)\n", res, i);
        }
    }
    return item;
}

/* The captures' test ROM version (test 0's result): version 3 added test 8
 * and test 4's two bytes read back when it finds no upper RAM; version 5
 * results 9-11. */
static int zt_capver;


/* A 74LS157's input pin for a section's output Y: A (the column) or B
 * (the row); 0 if Y isn't one. */
static int mux_input(int y, bool row) {
    static const int sec[4][3] = { {4, 2, 3}, {7, 5, 6}, {9, 11, 10}, {12, 14, 13} };
    for (int k = 0; k < 4; k++) if (sec[k][0] == y) return sec[k][row ? 2 : 1];
    return 0;
}

/* Results 9 and 10: a bank's address lines that alias (r[1] the data bits
 * it showed in, r[2] r[3] the lines). The board's wiring says which pin of
 * the chips each line reaches, as the row or the column, and from which
 * multiplexer output: a pin with both its lines wrong is that pin's path;
 * one line of the two, that line's way into the multiplexer. */
static int zt_addr(int item, bool upper, const uint8_t r[4]) {
    if (r[0] != 1) return item;
    const char *bank = upper ? "upper RAM" : "lower RAM", *chips = upper ? "URAM" : "LRAM";
    unsigned lines = (unsigned)(r[2] << 8 | r[3]), bits = r[1], left = lines;
    zt_addr_bits[upper] = bits;
    char ls[120], bs[64];
    bits_list(lines, 16, "A", ls, sizeof(ls));
    bits_list(bits, 8, "D", bs, sizeof(bs));
    zx48_addr_pin_t ap[8];
    int np = vboard ? zx48_ram_addr_pins(vboard, upper, ap) : 0;
    const sim_t *s = vboard ? &vboard->sim : NULL;
    if (nbits(bits) >= 3) {
        printf("  %d. The %s's address line%s %s %s reach its chips as %s own: writing one address changed\n"
               "     another too, in bits %s (every chip). What all the chips share:\n", ++item, bank,
               nbits(lines) == 1 ? "" : "s", ls, nbits(lines) == 1 ? "doesn't" : "don't", nbits(lines) == 1 ? "its" : "their", bs);
        for (int i = 0; i < np; i++) {
            bool rw = ap[i].row >= 0 && (lines >> ap[i].row & 1), cl = ap[i].col >= 0 && (lines >> ap[i].col & 1);
            if (!rw && !cl) continue;
            const char *mux = ap[i].mux_part >= 0 ? s->parts[ap[i].mux_part].ref : NULL;
            if (rw && cl) {
                printf("     - the chips' A%d (pin %d) gets A%d as the row and A%d as the column, both wrong: that pin's\n"
                       "       path, %s%s%d (the multiplexer's output), a resistor in series if any, or the trace\n",
                       i, ap[i].pin, ap[i].row, ap[i].col, mux ? mux : "", mux ? " pin " : "pin ", ap[i].mux_pin);
                left &= ~(1u << ap[i].row) & ~(1u << ap[i].col);
            } else {
                int a = rw ? ap[i].row : ap[i].col, in = ap[i].mux_part >= 0 ? mux_input(ap[i].mux_pin, rw) : 0;
                printf("     - A%d only (the %s on the chips' A%d; the %s, A%d, is right): A%d's way into the multiplexer,\n"
                       "       %s%s%d, or the trace to it from the CPU's A%d (the CPU drives A%d right: test 1)\n", a,
                       rw ? "row" : "column", i, rw ? "column" : "row", rw ? ap[i].col : ap[i].row, a, mux ? mux : "",
                       mux ? " pin " : "the multiplexer's input pin ", in, a, a);
                left &= ~(1u << a);
            }
        }
        /* several outputs of one multiplexer: that chip */
        for (int i = 0; i < np; i++) {
            int mp = ap[i].mux_part, cnt = 0, first = -1;
            if (mp < 0) continue;
            for (int j = 0; j < np; j++) {
                bool bad = ap[j].mux_part == mp && ap[j].row >= 0 && ap[j].col >= 0 && (lines >> ap[j].row & 1) && (lines >> ap[j].col & 1);
                if (bad && first < 0) first = j;
                cnt += bad;
            }
            if (cnt >= 2 && first == i)
                printf("     - %d of these from %s: more likely %s itself (its +5V or GND, its select, pin 1, or its\n"
                       "       enable, pin 15), or not fitted\n", cnt, s->parts[mp].ref, s->parts[mp].ref);
        }
        if (left) {
            bits_list(left, 16, "A", ls, sizeof(ls));
            printf("     - %s: not on the chips' address pins by this board's wiring: the select logic of the bank\n", ls);
        }
        return item;
    }
    /* One or two bits: those chips' own address pins */
    for (int b = 0; b < 8; b++) {
        if (!(bits >> b & 1)) continue;
        char lab[8], pins[160] = "";
        snprintf(lab, sizeof(lab), "%s%d", chips, b);
        const char *ic = ref_of(lab);
        int n = 0;
        for (int i = 0; i < np; i++) {
            bool rw = ap[i].row >= 0 && (lines >> ap[i].row & 1), cl = ap[i].col >= 0 && (lines >> ap[i].col & 1);
            if (rw || cl) n += snprintf(pins + n, sizeof(pins) - n, "%sA%d (pin %d)", n ? ", " : "", i, ap[i].pin);
        }
        printf("  %d. %s, the %s's bit D%d: address line%s %s alias%s in this chip only. Its address pin%s %s: the\n"
               "     leg (not soldered, or broken inside), or its trace.\n", ++item, ic ? ic : "the chip", bank, b,
               nbits(lines) == 1 ? "" : "s", ls, nbits(lines) == 1 ? "es" : "", n && strchr(pins, ',') ? "s" : "",
               n ? pins : "(not found on this board's wiring)");
    }
    return item;
}

/* What test N's result says, as numbered items from ITEM. */
static int zt_verdict(int item, int test, const uint8_t r[4], const machine_t *m) {
    char buf[600], b[64];
    unsigned v16 = r[2] << 8 | r[3];
    switch (test) {
    case 2:
        printf("  %d. The ROM's sum is %04X, not what the cartridge holds: reads of the cartridge come back wrong.\n"
               "     The cartridge's contacts (clean the edge connector), its ROM, or the address and data lines\n"
               "     between the CPU and the edge connector.\n", ++item, v16);
        return item;
    case 3: return zt_ram(item, false, r);
    case 9: return zt_addr(item, false, r);
    case 10: return zt_addr(item, true, r);
    case 11: {
        /* bits 0-3: bit 6 read 1 after OUT 00, 08, 10, 18 */
        const char *ula = ref_of("ULA");
        printf("  %d. The EAR input reads %d after OUT 00 and %d after OUT 18 (IN FE gave %02X and %02X), not 0 and 1, the\n"
               "     same on every issue with nothing in the EAR socket. Take any lead out of the EAR socket. The ULA\n"
               "     (%s) reads EAR on the pin it drives the speaker from: that pin, the resistors and capacitor between\n"
               "     it, the EAR and MIC sockets and the speaker, or a short on them.\n", ++item, r[1] & 1, r[1] >> 3 & 1,
               r[2], r[3], ula ? ula : "IC1");
        return item;
    }
    case 4:
        if (r[0] == 2) {
            if (zt_16k(m)) return item;
            /* Version 3 on: +2 +3 = what 8000 and 8001 read back (55 and AA
             * written). FF FF: nothing drives the bus there. Else the bits
             * wrong in both: one or two of them are chips stuck. */
            unsigned bad = (0x55u ^ r[2]) | (0xAAu ^ r[3]);
            int nb = 0;
            for (int i = 0; i < 8; i++) nb += bad >> i & 1;
            if (zt_capver >= 3 && v16 != 0xFFFF && nb <= 2) {
                uint8_t f[4] = { 1, (uint8_t)bad, 0x80, 0x00 };
                printf("  %d. No upper RAM found at first sight: 8000 and 8001 read back %02X %02X, not 55 AA, so the march\n"
                       "     didn't run. Bits wrong in both:\n", ++item, r[2], r[3]);
                return zt_ram(item, true, f);
            }
            bank_parts("upper RAM", "URAM", buf, sizeof(buf));
            char got[80] = "nothing written there";
            if (zt_capver >= 3)
                snprintf(got, sizeof(got), "%02X %02X, not 55 AA%s", r[2], r[3],
                         v16 == 0xFFFF ? ": nothing drives the data bus there" : "");
            printf("  %d. No upper RAM found at all (8000 and 8001 read back %s). A 16K Spectrum has\n"
                   "     none: if this one is a 48K, the upper RAM as a whole: %s\n", ++item, got,
                   buf[0] ? buf : "its select and multiplexers");
            return item;
        }
        return zt_ram(item, true, r);
    case 8: {
        /* The march passed it: what was lost, was lost unread for ~1.6 s */
        unsigned bits = r[1];
        int nb = 0;
        for (int i = 0; i < 8; i++) nb += bits >> i & 1;
        bits_list(bits, 8, "D", b, sizeof(b));
        if (nb >= 3) {
            bank_parts("upper RAM", "URAM", buf, sizeof(buf));
            printf("  %d. The upper RAM forgets what it holds when nothing reads it for about 1.6 s (bits %s, the first\n"
                   "     at %04X), though it passed the march: its refresh. The CPU (IC2) makes it (/RFSH, pin 28, with\n"
                   "     /MREQ and the row on A0-A6); what passes it to the upper RAM's /RAS: %s\n", ++item, b, v16,
                   buf[0] ? buf : "its select logic");
            return item;
        }
        for (int i = 0; i < 8; i++) {
            if (!(bits >> i & 1)) continue;
            char lab[8];
            snprintf(lab, sizeof(lab), "URAM%d", i);
            const char *ic = ref_of(lab);
            printf("  %d. %s, the upper RAM's bit D%d: it passed the march but lost what it held after about 1.6 s\n"
                   "     unread (the first wrong at %04X): it leaks faster than the refresh keeps it.\n", ++item,
                   ic ? ic : "the chip", i, v16);
        }
        return item;
    }
    case 5: {
        bits_list(r[1], 5, "KB", b, sizeof(b));
        char rows[64];
        int nr = 0;
        for (int i = 0; i < 8; i++) nr += r[2] >> i & 1;
        /* rows are A8-A15: bit i = A(8+i) */
        int n = 0;
        rows[0] = 0;
        for (int i = 0; i < 8; i++) if (r[2] >> i & 1) n += snprintf(rows + n, sizeof(rows) - n, "%sA%d", n ? " " : "", 8 + i);
        const char *ula = ref_of("ULA");
        if (nr == 8)
            printf("  %d. Keyboard line %s reads low on every row, no key pressed: the line itself is held low. The\n"
                   "     ULA (%s) that reads it, or the keyboard's ribbon/membrane shorting it (unplug the keyboard and\n"
                   "     run again: still low = the ULA or the board).\n", ++item, b, ula ? ula : "IC1");
        else
            printf("  %d. Keyboard line %s reads low on row%s %s, no key pressed: a key held down or shorted there\n"
                   "     (the membrane), or that row's diode (the rows are A8-A15 through diodes).\n", ++item, b,
                   nr == 1 ? "" : "s", rows);
        return item;
    }
    case 6:
        if ((r[1] & 4) && v16 < 100) {
            line_chips(LA_INT, buf, sizeof(buf));
            printf("  %d. /INT is held low: an interrupt came at once each time it was let in (%d loop turn%s between\n"
                   "     two, not %d). The ULA makes it, once a frame (IC1 pin 12, through R26): R26 open (the CPU's pin\n"
                   "     then floats low), the line shorted to 0 V, or the ULA.%s%s\n", ++item, v16,
                   v16 == 1 ? "" : "s", zt.frame, buf[0] ? " On the line: " : "", buf);
            return item;
        }
        if (r[1] & 1) {
            line_chips(LA_INT, buf, sizeof(buf));
            printf("  %d. No frame interrupt came (IM 1): /INT never went low, or the CPU doesn't take it. The ULA\n"
                   "     makes it (IC1 pin 12, through R26).%s%s\n", ++item, buf[0] ? " On the line: " : "", buf);
        }
        if ((r[1] & 2) && !(r[1] & 1))
            printf("  %d. IM 1 interrupts came but no IM 2 one: the CPU (IC2) taking the interrupt's vector off the\n"
                   "     data bus.\n", ++item);
        if (r[1] & 4)
            printf("  %d. The frame is the wrong length: %d loop turns between interrupts, not %d (each turn is 24 clock\n"
                   "     cycles: a frame is 69888). The ULA's clock (the crystal X1, the ULA) or the frame counter in the ULA.\n",
                   ++item, v16, zt.frame);
        return item;
    case 7: {
        const char *ula = ref_of("ULA");
        if (r[0] == 2) return item;
        if (r[1] & 1) {
            printf("  %d. The interrupts stopped coming for the contention test (they came for the interrupt test).\n", ++item);
            return item;
        }
        printf("  %d. Contention is wrong: %d loop turns a frame with reads of 4000, not %d (with no contention at\n"
               "     all it would be %d). The ULA (%s) holds the CPU's clock while it fetches the screen: its\n"
               "     timing with the CPU, or the clock's path to the CPU.\n", ++item, v16, zt.cont, zt.nocont, ula ? ula : "IC1");
        return item;
    }
    }
    return item;
}

int zxtest_diagnose(const la_cli_t *o) {
    const machine_t *m = o->machine ? o->machine : &machine_zx48;
    const char *dir = o->diagnose;
    char path[1024], err[512], rompath[900];
    static uint8_t rom[16384];
    bool own = !la_logf;
    if (!zt_rom(o, rom, rompath, sizeof(rompath))) {
        fprintf(stderr, "The test ROM (zxtest.rom) isn't in carts/ or zxtest/ here or beside the program: name it on the command line\n");
        return 1;
    }
    zt_consts(rom);
    int img_ver = zt.version;
    char oldpath[1000] = "";        /* the older image the captures were compared with */
    mkdir1(dir);
    if (!o->replay)
        for (int k = 0; k < ZT_NCAP; k++) { cap_file(dir, k, path, sizeof(path)); remove(path); }
    if (!la_logf) {
        snprintf(path, sizeof(path), "%s/diagnosis.txt", dir);
        la_logf = fopen(path, "w");
    }
    char opts[160];
    options_text(m, opts, sizeof(opts));
    printf("Diagnosing a %s%s running the test ROM (%s), %s\n", m->board_name, opts, rompath,
           o->replay ? "from the captures in the folder" : o->simulate ? "simulated" : "with the logic analyser");
    if (o->simulate) {
        printf("The board under test: the simulated %s", m->board_name);
        for (int i = 0; i < o->nfaults; i++) printf("%s%s", i ? " " : ", with ", o->faults[i]);
        printf("\n");
    }
    if (!o->replay && !o->simulate)
        printf("The Spectrum just needs to be running the test ROM (the cartridge's slot for it): no reset. It goes\n"
               "round its tests every 6 s or so; each capture waits for its test.\n");
    vboard = la_new_board(m);
    zt_addr_bits[0] = zt_addr_bits[1] = 0;
    uint8_t res[ZT_N][4];
    bool got[ZT_N] = { 0 };
    zt_bus_t bus = { 0 };
    bool have_bus = false;
    int ncap = 0;
    char where[200] = "";
    /* Simulated: one board left running, each capture waiting for its test
     * as the analyser does on a real one (not a power on per capture: the
     * last test comes about 5 s in). */
    board_t *sim = NULL;
    if (o->simulate) {
        la_sim_cart = rompath;
        sim = sim_board(m, o->faults, o->nfaults, err, sizeof(err));
        if (!sim) {
            la_sim_cart = NULL;
            la_err(err);
            board_destroy(vboard);
            vboard = NULL;
            if (own && la_logf) { fclose(la_logf); la_logf = NULL; }
            return 1;
        }
    }
    bool boxbad[8] = { 0 }, shown_res[ZT_N] = { 0 };
    int ncaps = zt.ntests > 9 ? ZT_NCAP : 8, shown = 0;
    for (int k = 0; k < ZT_NCAP; k++) {
        la_capture_t c;
        bool ok;
        cap_file(dir, k, path, sizeof(path));
        int t = cap_test(k), box = zt_cap[k].box;
        /* Results 9-11: only from version 5 on (the image here, and the
         * cartridge's own as test 0 said); a folder replayed from before
         * them hasn't their files. */
        if (t >= 9) {
            int v = got[0] ? (res[0][3] ? res[0][3] : 1) : img_ver;
            FILE *f = o->replay ? fopen(path, "rb") : NULL;
            if (f) fclose(f);
            if (zt.ntests <= 9 || v < 5 || (o->replay && !f)) continue;
        }
        /* this box's last capture? (then its state is final) */
        bool last = true;
        for (int j = k + 1; j < ZT_NCAP; j++) if (zt_cap[j].box == box) last = false;
        printf("\n%d of %d: %s\n", ++shown, ncaps, k == 0 ? "tests 0 and 1 (the data and address lines)" : zt_name[t]);
        la_status(LA_EV_STAGE, box, LA_ST_RUN);
        if (o->replay) ok = la_read(path, &c, err, sizeof(err));
        else if (o->simulate)
            ok = la_record_at(sim, 2000, k == 0 ? 194000 : 20000, cap_trig(k), 0, 8000, &c, err, sizeof(err));
        else {
            la_cli_t q = *o;
            char trig[16];
            snprintf(trig, sizeof(trig), "addr=%04X", cap_trig(k));
            q.trigger = trig; q.capture = path; q.pre = 1000; q.post = 97000; q.at = 0; q.timeout = 15; q.diagnose = NULL;
            ok = capture_real(&q, &c, err, sizeof(err));
        }
        if (!ok) {
            boxbad[box] = true;
            la_status(LA_EV_STAGE, box, LA_ST_FAULT);
            printf("  Not seen: %s\n", err);
            if (!o->replay && !strstr(err, "no trigger") && !strstr(err, "never showed")) { la_err(err); break; }
            continue;
        }
        if (!o->replay && !la_write(path, &c, err, sizeof(err))) la_err(err);
        ncap++;
        la_cycle_t *cy;
        int n = la_cycles(&c, &cy);
        /* Every result in it not had yet: the capture's own, and 9-11,
         * which come early in the next test's capture or late in their
         * own (version 5 on: the image's NTESTS). */
        bool now[ZT_N] = { 0 };
        for (int tt = 0; tt < zt.ntests; tt++)
            if (!got[tt] && zt_result(cy, n, tt, res[tt])) got[tt] = now[tt] = true;
        if (k == 0) {
            /* the cartridge holds an older version: its own image, if kept */
            int v = got[0] ? (res[0][3] ? res[0][3] : 1) : img_ver;
            if (v != img_ver && !zt_old_rom(rompath, v, rom, oldpath, sizeof(oldpath))) oldpath[0] = 0;
            zt_bus(cy, n, rom, &bus);
            have_bus = bus.have;
        }
        if (!got[t] && !where[0]) where_text(&c, where, sizeof(where));
        free(cy);
        la_free(&c);
        if (k == 0)
            printf("  %s\n", !have_bus ? "test 0's start not in the capture" :
                   bus.alive_wrong || bus.walk_seen < 32 || bus.a_wrong || bus.rom_bad ? "WRONG (below)" : "OK: every data and address line both ways");
        /* The capture's own tests, and the results 9-11 it held too. */
        for (int tt = 0; tt < ZT_N; tt++) {
            bool own = k == 0 ? tt <= 1 : tt == t;
            if (!own && !(tt >= 9 && now[tt])) continue;
            if (shown_res[tt]) {             /* 9-11: in an earlier capture */
                if (own) printf("  Test %d: given above (the capture before held it)\n", tt);
                continue;
            }
            shown_res[tt] = got[tt];
            if (!got[tt]) { printf("  Test %d: no result written%s\n", tt, k == 0 && tt == 0 && have_bus ? " (its writes were)" : ""); continue; }
            printf("  Test %d: %s  %02X %02X%02X\n", tt, res[tt][0] == 0 ? "OK  " : res[tt][0] == 1 ? "FAIL" : "none",
                   res[tt][1], res[tt][2], res[tt][3]);
        }
        bool bad = k == 0 ? !have_bus || bus.alive_wrong || bus.a_wrong || bus.rom_bad || !got[0] || !got[1] || res[0][0] == 1 || res[1][0] == 1
                          : !got[t] || res[t][0] == 1;
        boxbad[box] |= bad;
        if (boxbad[box]) la_status(LA_EV_STAGE, box, LA_ST_FAULT);
        else if (last) la_status(LA_EV_STAGE, box, LA_ST_OK);
    }
    la_sim_cart = NULL;
    if (sim) board_destroy(sim);

    /* The verdict */
    printf("\n");
    int item = 0;
    bool none = true;
    for (int t = 0; t < ZT_N; t++) none &= !got[t];
    if (!ncap || (none && !have_bus)) {
        printf("No test of the test ROM was seen%s. Is the cartridge in, set to the test ROM, and the Spectrum on?\n"
               "If it is: the CPU can't run it. Diagnose the boot (without the cartridge) first.%s%s\n",
               ncap ? " (the captures came, but no result was written)" : "", where[0] ? "\nWhere the CPU was: " : "", where);
    } else {
        /* Captures of another version of the test ROM than the image here:
         * compared with that version's image if it was kept, else its
         * bytes can't be (its results still say what they say). */
        int cap_ver = got[0] ? (res[0][3] ? res[0][3] : 1) : img_ver;
        zt_capver = cap_ver;
        int ntests = cap_ver >= 5 ? ZT_N : cap_ver >= 3 ? 9 : 8;    /* test 8 came with version 3, 9-11 with 5 */
        if (have_bus && cap_ver != img_ver && oldpath[0])
            printf("(These captures are of version %d of the test ROM, compared with %s; %s is\n"
                   "version %d: program the cartridge with it when convenient.)\n\n", cap_ver, oldpath, rompath, img_ver);
        else if (have_bus && cap_ver != img_ver) {
            printf("(These captures are of version %d of the test ROM, %s is version %d: the cartridge's reads aren't\n"
                   "compared with it. Program the cartridge with the newer one.)\n\n", cap_ver, rompath, img_ver);
            bus.rom_bad = 0;
        }
        /* Is there anything to say? */
        bool any = (have_bus && (bus.rom_bad || bus.alive_wrong || bus.a_wrong));
        for (int t = 0; t < ntests; t++)
            any |= !got[t] || res[t][0] == 1 || (t == 4 && res[t][0] == 2 && !zt_16k(m));
        if (any) printf("What to look at:\n");
        if (have_bus && bus.rom_bad) {
            char b[64];
            bits_list(bus.rom_bits, 8, "D", b, sizeof(b));
            printf("  %d. Reads of the cartridge come back wrong (%d of %d, bits %s, the first at %04X): the data lines\n"
                   "     between the edge connector and the CPU, or the cartridge's contacts.\n", ++item, bus.rom_bad,
                   bus.rom_bad + bus.rom_ok, b, bus.rom_first);
        }
        if (have_bus && bus.alive_wrong) {
            for (int i = 0; i < 8; i++) {
                if (!(bus.d_wrong >> i & 1)) continue;
                char buf[600];
                line_chips(LA_D0 + i, buf, sizeof(buf));
                const char *how = (bus.d_low >> i & 1) && !(bus.d_high >> i & 1) ? "never goes high" :
                                  (bus.d_high >> i & 1) && !(bus.d_low >> i & 1) ? "never goes low" : "is wrong both ways";
                printf("  %d. Data line D%d %s when the CPU writes (%d of 20 writes wrong): shorted or stuck, or a chip\n"
                       "     on it holding it: %s\n", ++item, i, how, bus.alive_wrong, buf[0] ? buf : "-");
            }
        }
        if (have_bus && bus.a_wrong) {
            for (int i = 0; i < 16; i++) {
                if (!(bus.a_wrong >> i & 1)) continue;
                char buf[600];
                line_chips(LA_A0 + i, buf, sizeof(buf));
                const char *how = (bus.a_low >> i & 1) && !(bus.a_high >> i & 1) ? "never goes high" :
                                  (bus.a_high >> i & 1) && !(bus.a_low >> i & 1) ? "never goes low" : "is wrong both ways";
                printf("  %d. Address line A%d %s: shorted or stuck, or a chip on it holding it: %s\n", ++item, i, how,
                       buf[0] ? buf : "-");
            }
        }
        /* Test 7 counts interrupts too: with test 6 failed, its count says
         * nothing more. With no contention at all the ULA and the CPU use
         * the lower RAM at once: that RAM fails as a whole too. */
        bool int_bad = got[6] && res[6][0] == 1;
        bool cont_bad = !int_bad && got[7] && res[7][0] == 1;
        int nb3 = 0;
        for (int i = 0; got[3] && i < 8; i++) nb3 += res[3][1] >> i & 1;
        bool cont_first = cont_bad && res[3][0] == 1 && nb3 >= 3;
        if (cont_first) item = zt_verdict(item, 7, res[7], m);
        for (int o = 2; o < ZT_N; o++) {
            int t = zt_order[o];
            if (!got[t] || (t == 7 && (int_bad || cont_first))) continue;
            if (t == 3 && cont_first && res[3][0] == 1)
                printf("     The lower RAM fails as a whole as well: with the contention wrong, the CPU and the ULA use it\n"
                       "     at the same time. Look at that first; if the RAM still fails with it right:\n");
            if (res[t][0] == 1 || (t == 4 && res[t][0] == 2)) item = zt_verdict(item, t, res[t], m);
        }
        /* The first test with no result: the program stopped there (or
         * before: the ones before it may be missing too). */
        int stop = -1;
        for (int o = 0; o < ZT_N && stop < 0; o++) if (zt_order[o] < ntests && !got[zt_order[o]]) stop = zt_order[o];
        if (stop >= 0) {
            printf("  %d. No result from test %d (%s)%s: the test ROM doesn't get there, or not every turn. It hangs\n"
                   "     or crashes in it or before it.%s%s\n", ++item, stop, zt_name[stop],
                   (stop >= 3 && stop <= 4) || stop == 10 ? ", which runs from the RAM's results" : "",
                   where[0] ? " Where the CPU was: " : "", where);
        }
        if (!any) {
            bool k16 = got[4] && res[4][0] == 2;
            printf("Nothing to look at: every test passed%s. The data and address lines go both ways, the ROM reads right, all of\n"
                   "  the RAM keeps every bit both ways%s, the keyboard lines are high with no key\n"
                   "  pressed, the frame interrupt and contention are timed as a working one's.\n"
                   "  Not tested here: the picture (the ULA's own reads of the screen), the sound and the tape%s.\n",
                   k16 ? " (no upper RAM: a 16K)" : "", ntests > 9 ? " (each address line its own)" : "",
                   ntests > 9 ? " (only the EAR input's level)" : "");
            if (ntests > 9 && got[11])
                printf("  The EAR input after OUT 00, 08, 10, 18: %d %d %d %d (08 and 10 differ between issues; a lead in the\n"
                       "  EAR socket changes them).\n", res[11][1] & 1, res[11][1] >> 1 & 1, res[11][1] >> 2 & 1,
                       res[11][1] >> 3 & 1);
        }
    }
    board_destroy(vboard);
    vboard = NULL;
    if (own && la_logf) { fclose(la_logf); la_logf = NULL; }
    return item ? 2 : 0;
}
