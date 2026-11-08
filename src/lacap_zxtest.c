/* lacap_zxtest.c - --la-diagnose DIR --la-zxtest: a Spectrum running our
 * test ROM (zxtest/zxtest.asm) from a cartridge, read off the edge
 * connector by the logic analyser.
 *
 * The ROM loops for ever through eight tests, each writing its result to
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

#define ZT_MARK 0x3FA0          /* test n's start: a write of n to MARK+n */
#define ZT_RES 0x3FC0           /* test n's result: four writes at RES+4n */
#define ZT_ALIVE 0x3F80         /* test 0's 20 writes */
#define ZT_N 8
#define ZT_FRAME 2912           /* zxtest.asm's FRAME_N, CONT_N */
#define ZT_CONT 2209
#define ZT_NOCONT 2254          /* the contended loop with no contention */

static const char *const zt_name[ZT_N] = {
    "alive (the data lines)", "the address lines", "the ROM's sum", "the lower RAM (4000-7FFF)",
    "the upper RAM (8000-FFFF)", "the ULA's ports (keyboard)", "the interrupts", "contention (the ULA's timing)" };

/* The captures: the first from test 0's start (tests 0 and 1 and their
 * results fit in it), then each result of tests 2-7. */
#define ZT_NCAP 7
static int cap_test(int k) { return k == 0 ? 0 : k + 1; }
static int cap_trig(int k) { return k == 0 ? ZT_MARK : ZT_RES + 4 * cap_test(k); }
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

/* Test N's four result bytes: the first write to RES+4N followed by
 * writes to the next three (a fetch or read there is not a result: a lost
 * CPU sliding through memory shows those addresses too). */
static bool zt_result(const la_cycle_t *cy, int n, int test, uint8_t r[4]) {
    unsigned a = ZT_RES + 4 * test;
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
        if ((cy[i].kind == LA_FETCH || cy[i].kind == LA_READ) && cy[i].addr < 0x3F80) {
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
        if (cy[i].addr == ZT_MARK && s0 < 0) s0 = i;
        if (cy[i].addr == ZT_MARK + 1 && s0 >= 0 && s1 < 0) s1 = i;
        if (cy[i].addr == ZT_RES + 4 && s1 >= 0 && e1 < 0) e1 = i;
    }
    if (s0 < 0) return;
    b->have = true;
    int k = 0;
    for (int i = s0 + 1; i < n && k < 20; i++) {
        if (cy[i].kind != LA_WRITE || cy[i].addr == ZT_MARK) continue;
        if (cy[i].addr == ZT_RES) break;    /* test 0's result: past its writes */
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
        if (cy[i].kind == LA_FETCH && cy[i].addr >= 0x0300 && cy[i].addr < 0x0400 && cy[i].data == 0x3A) reads = 0;
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
static int zt_ram(int item, bool upper, const uint8_t r[4]) {
    const char *bank = upper ? "upper RAM" : "lower RAM", *chips = upper ? "URAM" : "LRAM";
    unsigned bits = r[1], addr = r[2] << 8 | r[3];
    char b[64], buf[400], res[200];
    bits_list(bits, 8, "D", b, sizeof(b));
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
    case 4:
        if (r[0] == 2) {
            if (zt_16k(m)) return item;
            bank_parts("upper RAM", "URAM", buf, sizeof(buf));
            printf("  %d. No upper RAM found at all (8000-8001 read back nothing written there). A 16K Spectrum has\n"
                   "     none: if this one is a 48K, the upper RAM as a whole: %s\n", ++item, buf[0] ? buf : "its select and multiplexers");
            return item;
        }
        return zt_ram(item, true, r);
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
                   v16 == 1 ? "" : "s", ZT_FRAME, buf[0] ? " On the line: " : "", buf);
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
                   ++item, v16, ZT_FRAME);
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
               "     timing with the CPU, or the clock's path to the CPU.\n", ++item, v16, ZT_CONT, ZT_NOCONT, ula ? ula : "IC1");
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
               "round its tests every 4.5 s or so; each capture waits for its test.\n");
    vboard = la_new_board(m);
    uint8_t res[ZT_N][4];
    bool got[ZT_N] = { 0 };
    zt_bus_t bus = { 0 };
    bool have_bus = false;
    int ncap = 0;
    char where[200] = "";
    if (o->simulate) la_sim_cart = rompath;
    for (int k = 0; k < ZT_NCAP; k++) {
        la_capture_t c;
        bool ok;
        cap_file(dir, k, path, sizeof(path));
        int t = cap_test(k);
        printf("\n%d of %d: %s\n", k + 1, ZT_NCAP, k == 0 ? "tests 0 and 1 (the data and address lines)" : zt_name[t]);
        la_status(LA_EV_STAGE, k, LA_ST_RUN);
        if (o->replay) ok = la_read(path, &c, err, sizeof(err));
        else if (o->simulate)
            ok = record_sim(m, o->faults, o->nfaults, 2000, k == 0 ? 194000 : 20000, cap_trig(k), 0, &c, err, sizeof(err));
        else {
            la_cli_t q = *o;
            char trig[16];
            snprintf(trig, sizeof(trig), "addr=%04X", cap_trig(k));
            q.trigger = trig; q.capture = path; q.pre = 1000; q.post = 97000; q.at = 0; q.timeout = 15; q.diagnose = NULL;
            ok = capture_real(&q, &c, err, sizeof(err));
        }
        if (!ok) {
            la_status(LA_EV_STAGE, k, LA_ST_FAULT);
            printf("  Not seen: %s\n", err);
            if (!o->replay && !strstr(err, "no trigger") && !strstr(err, "never showed")) { la_err(err); break; }
            continue;
        }
        if (!o->replay && !la_write(path, &c, err, sizeof(err))) la_err(err);
        ncap++;
        la_cycle_t *cy;
        int n = la_cycles(&c, &cy);
        if (k == 0) {
            zt_bus(cy, n, rom, &bus);
            have_bus = bus.have;
            for (int tt = 0; tt < 2; tt++) got[tt] = zt_result(cy, n, tt, res[tt]);
        } else got[t] = zt_result(cy, n, t, res[t]);
        if (!got[t] && !where[0]) where_text(&c, where, sizeof(where));
        free(cy);
        la_free(&c);
        if (k == 0)
            printf("  %s\n", !have_bus ? "test 0's start not in the capture" :
                   bus.alive_wrong || bus.walk_seen < 32 || bus.a_wrong || bus.rom_bad ? "WRONG (below)" : "OK: every data and address line both ways");
        for (int tt = k == 0 ? 0 : t; tt <= t + (k == 0); tt++) {
            if (!got[tt]) { printf("  Test %d: no result written%s\n", tt, k == 0 && tt == 0 && have_bus ? " (its writes were)" : ""); continue; }
            printf("  Test %d: %s  %02X %02X%02X\n", tt, res[tt][0] == 0 ? "OK  " : res[tt][0] == 1 ? "FAIL" : "none",
                   res[tt][1], res[tt][2], res[tt][3]);
        }
        bool bad = k == 0 ? !have_bus || bus.alive_wrong || bus.a_wrong || bus.rom_bad || !got[0] || !got[1] || res[0][0] == 1 || res[1][0] == 1
                          : !got[t] || res[t][0] == 1;
        la_status(LA_EV_STAGE, k, bad ? LA_ST_FAULT : LA_ST_OK);
    }
    la_sim_cart = NULL;

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
        /* Is there anything to say? */
        bool any = (have_bus && (bus.rom_bad || bus.alive_wrong || bus.a_wrong));
        for (int t = 0; t < ZT_N; t++)
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
        for (int t = 2; t < ZT_N; t++) {
            if (!got[t] || (t == 7 && (int_bad || cont_first))) continue;
            if (t == 3 && cont_first && res[3][0] == 1)
                printf("     The lower RAM fails as a whole as well: with the contention wrong, the CPU and the ULA use it\n"
                       "     at the same time. Look at that first; if the RAM still fails with it right:\n");
            if (res[t][0] == 1 || (t == 4 && res[t][0] == 2)) item = zt_verdict(item, t, res[t], m);
        }
        /* The first test with no result: the program stopped there (or
         * before: the ones before it may be missing too). */
        int stop = -1;
        for (int t = 0; t < ZT_N && stop < 0; t++) if (!got[t]) stop = t;
        if (stop >= 0) {
            printf("  %d. No result from test %d (%s)%s: the test ROM doesn't get there, or not every turn. It hangs\n"
                   "     or crashes in it or before it.%s%s\n", ++item, stop, zt_name[stop],
                   stop >= 3 && stop <= 4 ? ", which runs from the RAM's results" : "",
                   where[0] ? " Where the CPU was: " : "", where);
        }
        if (!any) {
            bool k16 = got[4] && res[4][0] == 2;
            printf("Nothing to look at: every test passed%s. The data and address lines go both ways, the ROM reads right, all of\n"
                   "  the RAM keeps every bit both ways, the keyboard lines are high with no key pressed, the frame\n"
                   "  interrupt and contention are timed as a working one's.\n"
                   "  Not tested here: the picture (the ULA's own reads of the screen), the sound and the tape.\n",
                   k16 ? " (no upper RAM: a 16K)" : "");
        }
    }
    board_destroy(vboard);
    vboard = NULL;
    if (own && la_logf) { fclose(la_logf); la_logf = NULL; }
    return item ? 2 : 0;
}
