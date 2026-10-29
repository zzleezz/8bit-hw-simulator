/* repairs.c - real repair records (see repairs.h). */
#define _CRT_RAND_S             /* rand_s(): the system's random numbers */
#include "repairs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REPAIRS_MAGIC "HWREPR"
#define REPAIRS_VERSION 1
#define TAG_DSET 0x54455344u        /* "DSET" */
#define TAG_RECD 0x44434552u        /* "RECD" */
#define TAG_SETR 0x52544553u        /* "SETR" */
#define TAG_NORM 0x4d524f4eu        /* "NORM" */

static uint32_t fnv32(const uint8_t *p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

/* ------------------------------------------------------------------------ */
/* Payloads: little endian numbers, strings and blobs with their length      */
/* ------------------------------------------------------------------------ */

typedef struct { uint8_t *p; size_t n, cap; } buf_t;

static void put(buf_t *b, const void *v, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, v, n);
    b->n += n;
}
static void put_u8(buf_t *b, uint8_t v) { put(b, &v, 1); }
static void put_u16(buf_t *b, uint16_t v) { uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(b, x, 2); }
static void put_u32(buf_t *b, uint32_t v) { put_u16(b, (uint16_t)v); put_u16(b, (uint16_t)(v >> 16)); }
static void put_u64(buf_t *b, uint64_t v) { put_u32(b, (uint32_t)v); put_u32(b, (uint32_t)(v >> 32)); }
static void put_str(buf_t *b, const char *s) {
    size_t n = strlen(s);
    if (n > 0xFFFF) n = 0xFFFF;
    put_u16(b, (uint16_t)n);
    put(b, s, n);
}
static void put_blob(buf_t *b, const uint8_t *p, uint32_t n) { put_u32(b, n); if (n) put(b, p, n); }

/* Reading: past the end, everything reads 0 and 'bad' is set. */
typedef struct { const uint8_t *p; size_t n, at; bool bad; } rd_t;

static const uint8_t *take(rd_t *r, size_t n) {
    if (r->bad || n > r->n - r->at) { r->bad = true; return NULL; }
    const uint8_t *p = r->p + r->at;
    r->at += n;
    return p;
}
static uint8_t get_u8(rd_t *r) { const uint8_t *p = take(r, 1); return p ? p[0] : 0; }
static uint16_t get_u16(rd_t *r) { const uint8_t *p = take(r, 2); return p ? (uint16_t)(p[0] | p[1] << 8) : 0; }
static uint32_t get_u32(rd_t *r) { uint32_t lo = get_u16(r); return lo | (uint32_t)get_u16(r) << 16; }
static uint64_t get_u64(rd_t *r) { uint64_t lo = get_u32(r); return lo | (uint64_t)get_u32(r) << 32; }
static void get_str(rd_t *r, char *out, size_t len) {
    uint16_t n = get_u16(r);
    const uint8_t *p = take(r, n);
    size_t k = p ? (n < len - 1 ? n : len - 1) : 0;
    if (k) memcpy(out, p, k);
    out[k] = 0;
}
static uint8_t *get_blob(rd_t *r, uint32_t *len) {
    uint32_t n = get_u32(r);
    const uint8_t *p = take(r, n);
    *len = p ? n : 0;
    if (!p || !n) return NULL;
    uint8_t *v = malloc(n);
    memcpy(v, p, n);
    return v;
}

/* ------------------------------------------------------------------------ */
/* Records                                                                   */
/* ------------------------------------------------------------------------ */

static void encode_record(buf_t *b, const repair_t *r) {
    put_u64(b, r->dataset);
    put_u32(b, r->number);
    put_u32(b, r->version);
    put_u64(b, (uint64_t)r->when);
    put_str(b, r->machine);
    put_u8(b, r->certainty);
    put_u8(b, (uint8_t)r->nchips);
    for (int i = 0; i < r->nchips; i++) {
        put_str(b, r->chips[i].ref);
        put_str(b, r->chips[i].what);
    }
    put_str(b, r->notes);
    put_str(b, r->shot_name);
    put_blob(b, r->shot, r->shot_len);
    put_u8(b, (uint8_t)r->norm_machine);
    put_u16(b, r->norm_w);
    put_u16(b, r->norm_h);
    put_u16(b, (uint16_t)(r->snow * 65535));
    put_blob(b, r->norm, r->norm ? (uint32_t)r->norm_w * r->norm_h : 0);
    put_u8(b, r->flags);
    put_u16(b, r->read_version);
}

static bool decode_record(rd_t *rd, repair_t *r) {
    memset(r, 0, sizeof(*r));
    r->dataset = get_u64(rd);
    r->number = get_u32(rd);
    r->version = get_u32(rd);
    r->when = (int64_t)get_u64(rd);
    get_str(rd, r->machine, sizeof(r->machine));
    r->certainty = get_u8(rd);
    int n = get_u8(rd);
    for (int i = 0; i < n; i++) {
        repair_chip_t c;
        get_str(rd, c.ref, sizeof(c.ref));
        get_str(rd, c.what, sizeof(c.what));
        if (r->nchips < REPAIR_MAX_CHIPS) r->chips[r->nchips++] = c;
    }
    get_str(rd, r->notes, sizeof(r->notes));
    get_str(rd, r->shot_name, sizeof(r->shot_name));
    r->shot = get_blob(rd, &r->shot_len);
    r->norm_machine = get_u8(rd);
    r->norm_w = get_u16(rd);
    r->norm_h = get_u16(rd);
    r->snow = get_u16(rd) / 65535.0f;
    uint32_t nl;
    r->norm = get_blob(rd, &nl);
    /* Added after the first records (read with version 1). */
    r->read_version = 1;
    if (rd->at < rd->n) r->flags = get_u8(rd);
    if (rd->at < rd->n) r->read_version = get_u16(rd);
    if (rd->bad || nl != (uint32_t)r->norm_w * r->norm_h || r->certainty >= REPAIR_NCERTAINTY) {
        free(r->shot);
        free(r->norm);
        return false;
    }
    return true;
}

static void free_record(repair_t *r) {
    free(r->shot);
    free(r->norm);
    r->shot = r->norm = NULL;
}

/* A record read, or added: a newer version of one already there replaces it. */
static void keep_record(repairs_t *rs, repair_t *r) {
    for (int i = 0; i < rs->n; i++)
        if (rs->r[i].dataset == r->dataset && rs->r[i].number == r->number) {
            if (r->version >= rs->r[i].version) {
                r->off = rs->r[i].off;      /* a setting of this file */
                free_record(&rs->r[i]);
                rs->r[i] = *r;
            } else free_record(r);
            return;
        }
    rs->r = realloc(rs->r, (size_t)(rs->n + 1) * sizeof(repair_t));
    rs->r[rs->n++] = *r;
}

static void keep_dataset(repairs_t *rs, uint64_t id, const char *owner) {
    for (int i = 0; i < rs->nsets; i++)
        if (rs->sets[i].id == id) { snprintf(rs->sets[i].owner, sizeof(rs->sets[i].owner), "%s", owner); return; }
    rs->sets = realloc(rs->sets, (size_t)(rs->nsets + 1) * sizeof(repair_dataset_t));
    rs->sets[rs->nsets].id = id;
    snprintf(rs->sets[rs->nsets++].owner, sizeof(rs->sets[0].owner), "%s", owner);
}

/* ------------------------------------------------------------------------ */
/* The file                                                                  */
/* ------------------------------------------------------------------------ */

void repairs_load(repairs_t *rs, const char *path) {
    memset(rs, 0, sizeof(*rs));
    snprintf(rs->path, sizeof(rs->path), "%s", path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(size > 0 ? (size_t)size : 1);
    size_t n = fread(d, 1, size > 0 ? (size_t)size : 0, f);
    fclose(f);
    if (n < 8 || memcmp(d, REPAIRS_MAGIC, 6) || d[7] > REPAIRS_VERSION) {
        snprintf(rs->error, sizeof(rs->error), "%.200s is not a repair records file (or from a newer version)", path);
        rs->foreign = true;
        free(d);
        return;
    }
    /* The own dataset: the first one the file names (the file is made
     * with it). */
    size_t off = 8;
    while (off + 12 <= n) {
        uint32_t tag, len, sum;
        memcpy(&tag, d + off, 4);
        memcpy(&len, d + off + 4, 4);
        if (len > n - off - 12) break;
        const uint8_t *p = d + off + 8;
        memcpy(&sum, p + len, 4);
        if (sum != fnv32(p, len)) break;
        rd_t rd = { p, len, 0, false };
        if (tag == TAG_DSET) {
            uint64_t id = get_u64(&rd);
            char owner[64];
            get_str(&rd, owner, sizeof(owner));
            if (!rd.bad) {
                if (!rs->own.id) rs->own.id = id;
                if (id == rs->own.id) snprintf(rs->own.owner, sizeof(rs->own.owner), "%s", owner);
                keep_dataset(rs, id, owner);
            }
        } else if (tag == TAG_RECD) {
            repair_t r;
            if (decode_record(&rd, &r)) keep_record(rs, &r);
        } else if (tag == TAG_NORM) {
            uint64_t ds = get_u64(&rd);
            uint32_t num = get_u32(&rd), ver = get_u32(&rd);
            uint16_t rv = get_u16(&rd);
            int m = get_u8(&rd), w = get_u16(&rd), h = get_u16(&rd);
            float snow = get_u16(&rd) / 65535.0f;
            uint32_t nl;
            uint8_t *norm = get_blob(&rd, &nl);
            repair_t *to = NULL;
            for (int i = 0; i < rs->n; i++)
                if (rs->r[i].dataset == ds && rs->r[i].number == num && rs->r[i].version == ver) to = &rs->r[i];
            if (!rd.bad && to && nl == (uint32_t)w * h && rv >= to->read_version) {
                free(to->norm);
                to->norm = norm;
                to->norm_machine = m;
                to->norm_w = (uint16_t)w;
                to->norm_h = (uint16_t)h;
                to->snow = snow;
                to->read_version = rv;
            } else free(norm);
        } else if (tag == TAG_SETR) {
            uint64_t ds = get_u64(&rd);
            uint32_t num = get_u32(&rd);
            uint8_t off_ = get_u8(&rd);
            for (int i = 0; !rd.bad && i < rs->n; i++)
                if (rs->r[i].dataset == ds && rs->r[i].number == num) rs->r[i].off = off_ != 0;
        }
        off += 12 + len;
    }
    free(d);
}

void repairs_free(repairs_t *rs) {
    for (int i = 0; i < rs->n; i++) free_record(&rs->r[i]);
    free(rs->r);
    free(rs->sets);
    memset(rs, 0, sizeof(*rs));
}

/* Add chunks to the file (made, with its header, if it isn't there). */
static bool append(repairs_t *rs, uint32_t tag, const buf_t *payload) {
    if (rs->foreign) {
        snprintf(rs->error, sizeof(rs->error), "%.200s is not a repair records file: left as it is", rs->path);
        return false;
    }
    FILE *f = fopen(rs->path, "ab");
    if (!f) {
        snprintf(rs->error, sizeof(rs->error), "can't write %.250s", rs->path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    buf_t all = { 0 };
    if (ftell(f) == 0) {
        put(&all, REPAIRS_MAGIC, 6);
        put_u8(&all, 0);
        put_u8(&all, REPAIRS_VERSION);
    }
    put_u32(&all, tag);
    put_u32(&all, (uint32_t)payload->n);
    put(&all, payload->p, payload->n);
    put_u32(&all, fnv32(payload->p, payload->n));
    bool ok = fwrite(all.p, 1, all.n, f) == all.n;
    ok = fclose(f) == 0 && ok;
    free(all.p);
    if (!ok) snprintf(rs->error, sizeof(rs->error), "can't write %.250s", rs->path);
    return ok;
}

/* A random 64 bit ID: the system's random numbers, else the time and the
 * address of a variable. Never 0. */
static uint64_t random_id(void) {
    uint64_t v = 0;
#ifdef _WIN32
    unsigned lo, hi;
    if (rand_s(&lo) == 0 && rand_s(&hi) == 0) v = (uint64_t)hi << 32 | lo;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(&v, sizeof(v), 1, f) != 1) v = 0; fclose(f); }
#endif
    if (!v) v = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)&v;
    return v ? v : 1;
}

static bool write_dataset(repairs_t *rs) {
    buf_t b = { 0 };
    put_u64(&b, rs->own.id);
    put_str(&b, rs->own.owner);
    bool ok = append(rs, TAG_DSET, &b);
    free(b.p);
    if (ok) keep_dataset(rs, rs->own.id, rs->own.owner);
    return ok;
}

bool repairs_set_owner(repairs_t *rs, const char *owner) {
    snprintf(rs->own.owner, sizeof(rs->own.owner), "%s", owner);
    if (!rs->own.id) rs->own.id = random_id();
    return write_dataset(rs);
}

bool repairs_add(repairs_t *rs, repair_t *rec) {
    rs->error[0] = 0;
    if (!rs->own.id) {
        rs->own.id = random_id();
        if (!write_dataset(rs)) { rs->own.id = 0; return false; }
    }
    uint32_t number = 1;
    for (int i = 0; i < rs->n; i++)
        if (rs->r[i].dataset == rs->own.id && rs->r[i].number >= number) number = rs->r[i].number + 1;
    rec->dataset = rs->own.id;
    rec->number = number;
    rec->version = 1;
    if (!rec->when) rec->when = (int64_t)time(NULL);
    buf_t b = { 0 };
    encode_record(&b, rec);
    bool ok = append(rs, TAG_RECD, &b);
    free(b.p);
    if (!ok) return false;
    keep_record(rs, rec);
    memset(rec, 0, sizeof(*rec));
    return true;
}

bool repairs_update(repairs_t *rs, int index) {
    rs->error[0] = 0;
    repair_t *r = &rs->r[index];
    if (r->dataset != rs->own.id) {
        snprintf(rs->error, sizeof(rs->error), "only the records of your own dataset can be changed");
        return false;
    }
    r->version++;
    buf_t b = { 0 };
    encode_record(&b, r);
    bool ok = append(rs, TAG_RECD, &b);
    free(b.p);
    if (!ok) r->version--;
    return ok;
}

bool repairs_set_reading(repairs_t *rs, int index, uint16_t read_version, int machine, int w, int h, float snow,
                         uint8_t *norm) {
    rs->error[0] = 0;
    repair_t *r = &rs->r[index];
    /* Read the same as before: nothing to keep. */
    if (r->norm && r->norm_machine == machine && r->norm_w == w && r->norm_h == h &&
        !memcmp(r->norm, norm, (size_t)w * h) && r->read_version == read_version) {
        free(norm);
        return true;
    }
    buf_t b = { 0 };
    put_u64(&b, r->dataset);
    put_u32(&b, r->number);
    put_u32(&b, r->version);
    put_u16(&b, read_version);
    put_u8(&b, (uint8_t)machine);
    put_u16(&b, (uint16_t)w);
    put_u16(&b, (uint16_t)h);
    put_u16(&b, (uint16_t)(snow * 65535));
    put_blob(&b, norm, (uint32_t)w * h);
    bool ok = append(rs, TAG_NORM, &b);
    free(b.p);
    /* Kept in memory even if the file can't take it: read again next time. */
    free(r->norm);
    r->norm = norm;
    r->norm_machine = machine;
    r->norm_w = (uint16_t)w;
    r->norm_h = (uint16_t)h;
    r->snow = snow;
    r->read_version = read_version;
    return ok;
}

bool repairs_set_off(repairs_t *rs, int index, bool off) {
    rs->error[0] = 0;
    repair_t *r = &rs->r[index];
    buf_t b = { 0 };
    put_u64(&b, r->dataset);
    put_u32(&b, r->number);
    put_u8(&b, off);
    bool ok = append(rs, TAG_SETR, &b);
    free(b.p);
    if (ok) r->off = off;
    return ok;
}

bool repairs_merge(repairs_t *rs, const char *path, int *added, int *updated, int *skipped) {
    *added = *updated = *skipped = 0;
    rs->error[0] = 0;
    repairs_t o;
    repairs_load(&o, path);
    if (o.foreign || o.error[0]) {
        snprintf(rs->error, sizeof(rs->error), "%.290s", o.error);
        repairs_free(&o);
        return false;
    }
    /* The file's own dataset first (the first one named is its own):
     * merged into an empty file, the others' mustn't become it. */
    if (!rs->own.id) {
        rs->own.id = random_id();
        if (!write_dataset(rs)) { rs->own.id = 0; repairs_free(&o); return false; }
    }
    bool ok = true;
    for (int i = 0; ok && i < o.nsets; i++) {
        if (o.sets[i].id == rs->own.id) continue;
        const char *here = NULL;
        for (int k = 0; k < rs->nsets; k++)
            if (rs->sets[k].id == o.sets[i].id) here = rs->sets[k].owner;
        if (here && !strcmp(here, o.sets[i].owner)) continue;
        buf_t b = { 0 };
        put_u64(&b, o.sets[i].id);
        put_str(&b, o.sets[i].owner);
        ok = append(rs, TAG_DSET, &b);
        free(b.p);
        if (ok) keep_dataset(rs, o.sets[i].id, o.sets[i].owner);
    }
    for (int i = 0; ok && i < o.n; i++) {
        repair_t *r = &o.r[i];
        int at = -1;
        for (int k = 0; k < rs->n; k++)
            if (rs->r[k].dataset == r->dataset && rs->r[k].number == r->number) at = k;
        if (at >= 0 && rs->r[at].version >= r->version) { (*skipped)++; continue; }
        buf_t b = { 0 };
        encode_record(&b, r);
        ok = append(rs, TAG_RECD, &b);
        free(b.p);
        if (!ok) break;
        (*(at >= 0 ? updated : added))++;
        r->off = false;
        keep_record(rs, r);                 /* takes its buffers */
        r->shot = r->norm = NULL;
    }
    repairs_free(&o);
    return ok;
}

bool repairs_export(repairs_t *rs, const char *path, bool all) {
    rs->error[0] = 0;
    buf_t f = { 0 };
    put(&f, REPAIRS_MAGIC, 6);
    put_u8(&f, 0);
    put_u8(&f, REPAIRS_VERSION);
    /* A chunk into F. */
    #define CHUNK(tag, payload) do { \
        put_u32(&f, tag); put_u32(&f, (uint32_t)(payload).n); put(&f, (payload).p, (payload).n); \
        put_u32(&f, fnv32((payload).p, (payload).n)); } while (0)
    /* The own dataset first: in the file made, it is the first one named. */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < rs->nsets; i++) {
            bool own = rs->sets[i].id == rs->own.id;
            if (own != (pass == 0) || (!all && !own)) continue;
            buf_t b = { 0 };
            put_u64(&b, rs->sets[i].id);
            put_str(&b, rs->sets[i].owner);
            CHUNK(TAG_DSET, b);
            free(b.p);
        }
    int n = 0;
    for (int i = 0; i < rs->n; i++) {
        if (!all && rs->r[i].dataset != rs->own.id) continue;
        buf_t b = { 0 };
        encode_record(&b, &rs->r[i]);
        CHUNK(TAG_RECD, b);
        free(b.p);
        n++;
    }
    #undef CHUNK
    FILE *out = fopen(path, "wb");
    bool ok = out && fwrite(f.p, 1, f.n, out) == f.n;
    if (out) ok = fclose(out) == 0 && ok;
    free(f.p);
    if (!ok) snprintf(rs->error, sizeof(rs->error), "can't write %.250s", path);
    else if (!n) snprintf(rs->error, sizeof(rs->error), "no records to export (the file has none)");
    return ok;
}

const char *repairs_owner(const repairs_t *rs, uint64_t dataset) {
    for (int i = 0; i < rs->nsets; i++)
        if (rs->sets[i].id == dataset) return rs->sets[i].owner;
    return "";
}

const char *repairs_certainty_name(int certainty) {
    static const char *const names[] = { "replaced, not sure which was bad", "swapping it fixed it",
                                         "tested bad in a chip tester" };
    return certainty >= 0 && certainty < REPAIR_NCERTAINTY ? names[certainty] : "?";
}
