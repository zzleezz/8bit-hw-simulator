/* lapico.c - the LogicAnalyzer boards over USB serial: see lapico.h. */
#include "lapico.h"
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* ------------------------------------------------------------------------ */
/* The serial port                                                           */
/* ------------------------------------------------------------------------ */

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

typedef struct { HANDLE h; } sp_t;

static bool sp_open(const char *port, sp_t *s, char *err, int errlen) {
    char path[128];
    snprintf(path, sizeof(path), port[0] == '\\' ? "%s" : "\\\\.\\%s", port);
    s->h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (s->h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        snprintf(err, errlen, "can't open %s%s", port,
                 e == ERROR_ACCESS_DENIED ? " (in use: is the LogicAnalyzer program open?)" :
                 e == ERROR_FILE_NOT_FOUND ? " (no such port)" : "");
        return false;
    }
    SetupComm(s->h, 1 << 20, 1 << 16);
    DCB dcb = { .DCBlength = sizeof(DCB) };
    GetCommState(s->h, &dcb);
    dcb.BaudRate = 115200;              /* USB: any; the board doesn't care */
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = dcb.fInX = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE; /* the board sends nothing until DTR is up */
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;
    SetCommState(s->h, &dcb);
    PurgeComm(s->h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return true;
}

static void sp_close(sp_t *s) {
    if (s->h != INVALID_HANDLE_VALUE) CloseHandle(s->h);
    s->h = INVALID_HANDLE_VALUE;
}

static bool sp_write(sp_t *s, const void *p, int n) {
    DWORD w = 0;
    return WriteFile(s->h, p, (DWORD)n, &w, NULL) && (int)w == n;
}

/* Up to n bytes: whatever is there, or waits up to ms for the first. */
static int sp_read(sp_t *s, void *p, int n, int ms) {
    COMMTIMEOUTS t = { MAXDWORD, MAXDWORD, (DWORD)(ms > 0 ? ms : 1), 0, 2000 };
    SetCommTimeouts(s->h, &t);
    DWORD r = 0;
    if (!ReadFile(s->h, p, (DWORD)n, &r, NULL)) {
        DWORD e = GetLastError(), errs = 0;
        COMSTAT st;
        ClearCommError(s->h, &errs, &st);
        if (getenv("LP_DEBUG")) fprintf(stderr, "ReadFile: error %lu, comm errors %lx\n", (unsigned long)e, (unsigned long)errs);
        return -1;
    }
    return (int)r;
}

static void sp_sleep(int ms) { Sleep((DWORD)ms); }

static volatile LONG lp_stop;
static BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) { lp_stop = 1; return TRUE; }
    /* The window closed: Windows ends the program when this returns (5 s
     * at most), so wait while the capture stops the boards, or they stay
     * armed and won't answer the next time. */
    if (type == CTRL_CLOSE_EVENT) { lp_stop = 1; Sleep(3000); return TRUE; }
    return FALSE;
}
static void catch_ctrl_c(bool on) { lp_stop = 0; SetConsoleCtrlHandler(on_ctrl, on ? TRUE : FALSE); }

/* The serial ports' class, {4D36E978-E325-11CE-BFC1-08002BE10318}. */
static const GUID ports_class = { 0x4D36E978, 0xE325, 0x11CE, { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 } };

int lp_find(lp_port_t *out, int max) {
    HDEVINFO h = SetupDiGetClassDevsA(&ports_class, NULL, NULL, DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVINFO_DATA d = { .cbSize = sizeof(d) };
    for (DWORD i = 0; n < max && SetupDiEnumDeviceInfo(h, i, &d); i++) {
        char id[512];
        if (!SetupDiGetDeviceInstanceIdA(h, &d, id, sizeof(id), NULL)) continue;
        for (char *q = id; *q; q++) *q = (char)toupper((unsigned char)*q);
        if (!strstr(id, "VID_1209&PID_3020")) continue;
        lp_port_t *p = &out[n];
        memset(p, 0, sizeof(*p));
        HKEY k = SetupDiOpenDevRegKey(h, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        DWORD len = sizeof(p->port) - 1, type = 0;
        LONG rc = RegQueryValueExA(k, "PortName", NULL, &type, (BYTE *)p->port, &len);
        RegCloseKey(k);
        if (rc != ERROR_SUCCESS) continue;
        DEVINST parent;
        char pid[512];
        if (CM_Get_Parent(&parent, d.DevInst, 0) == CR_SUCCESS &&
            CM_Get_Device_IDA(parent, pid, sizeof(pid), 0) == CR_SUCCESS) {
            const char *slash = strrchr(pid, '\\');
            snprintf(p->serial, sizeof(p->serial), "%s", slash ? slash + 1 : pid);
        }
        n++;
    }
    SetupDiDestroyDeviceInfoList(h);
    /* By port number, so the listing doesn't move about. */
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++) {
            int x = atoi(out[a].port + strcspn(out[a].port, "0123456789"));
            int y = atoi(out[b].port + strcspn(out[b].port, "0123456789"));
            if (y < x) { lp_port_t t = out[a]; out[a] = out[b]; out[b] = t; }
        }
    return n;
}

#else /* POSIX */
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

typedef struct { int fd; } sp_t;

static bool sp_open(const char *port, sp_t *s, char *err, int errlen) {
    char path[128];
    snprintf(path, sizeof(path), port[0] == '/' ? "%s" : "/dev/%s", port);
    s->fd = open(path, O_RDWR | O_NOCTTY);
    if (s->fd < 0) { snprintf(err, errlen, "can't open %s", path); return false; }
    struct termios t;
    if (tcgetattr(s->fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(s->fd, TCSANOW, &t);
    }
    int bits = TIOCM_DTR | TIOCM_RTS;
    ioctl(s->fd, TIOCMBIS, &bits);
    tcflush(s->fd, TCIOFLUSH);
    return true;
}

static void sp_close(sp_t *s) { if (s->fd >= 0) close(s->fd); s->fd = -1; }

static bool sp_write(sp_t *s, const void *p, int n) {
    const char *c = p;
    while (n > 0) {
        ssize_t w = write(s->fd, c, (size_t)n);
        if (w <= 0) return false;
        c += w; n -= (int)w;
    }
    return true;
}

static int sp_read(sp_t *s, void *p, int n, int ms) {
    struct pollfd q = { s->fd, POLLIN, 0 };
    int r = poll(&q, 1, ms > 0 ? ms : 1);
    if (r < 0) return -1;
    if (r == 0) return 0;
    ssize_t got = read(s->fd, p, (size_t)n);
    return got < 0 ? -1 : (int)got;
}

static void sp_sleep(int ms) { usleep((useconds_t)ms * 1000); }

static volatile sig_atomic_t lp_stop;
static void on_int(int sig) { (void)sig; lp_stop = 1; }
static void catch_ctrl_c(bool on) { lp_stop = 0; signal(SIGINT, on ? on_int : SIG_DFL); }

/* Linux: /dev/ttyACM*, the USB ids and serial from sysfs. */
int lp_find(lp_port_t *out, int max) {
    int n = 0;
    DIR *d = opendir("/sys/class/tty");
    if (!d) return 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (strncmp(e->d_name, "ttyACM", 6)) continue;
        char path[512], buf[64] = "";
        snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../idVendor", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
        fclose(f);
        if (strncmp(buf, "1209", 4)) continue;
        snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../idProduct", e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
        fclose(f);
        if (strncmp(buf, "3020", 4)) continue;
        lp_port_t *p = &out[n++];
        memset(p, 0, sizeof(*p));
        snprintf(p->port, sizeof(p->port), "/dev/%s", e->d_name);
        snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../serial", e->d_name);
        f = fopen(path, "r");
        if (f) {
            if (fgets(p->serial, sizeof(p->serial), f)) p->serial[strcspn(p->serial, "\r\n")] = 0;
            fclose(f);
        }
    }
    closedir(d);
    return n;
}
#endif

/* ------------------------------------------------------------------------ */
/* The protocol                                                              */
/* ------------------------------------------------------------------------ */

static bool send_packet(sp_t *s, int cmd, const uint8_t *data, int len) {
    uint8_t buf[256];
    int n = 0;
    buf[n++] = 0x55;
    buf[n++] = 0xAA;
    for (int i = -1; i < len; i++) {
        uint8_t b = i < 0 ? (uint8_t)cmd : data[i];
        if (b == 0x55 || b == 0xAA || b == 0xF0) { buf[n++] = 0xF0; buf[n++] = b ^ 0xF0; }
        else buf[n++] = b;
    }
    buf[n++] = 0xAA;
    buf[n++] = 0x55;
    return sp_write(s, buf, n);
}

static double now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
#endif
}

static void trace(const char *fmt, ...);

int (*lp_printf)(const char *fmt, ...) = printf;

/* A line (without the \n), waiting up to ms. */
static bool read_line(sp_t *s, char *out, int len, int ms) {
    int n = 0;
    double end = now_ms() + ms;
    while (now_ms() < end) {
        char c;
        int r = sp_read(s, &c, 1, 100);
        if (r < 0) return false;
        if (r == 0) continue;
        if (c == '\n') { out[n] = 0; return true; }
        if (c != '\r' && n < len - 1) out[n++] = c;
    }
    out[n] = 0;
    return false;
}

static bool hello(sp_t *s, lp_info_t *info, const char *port, char *err, int errlen) {
    memset(info, 0, sizeof(*info));
    if (!send_packet(s, 0, NULL, 0)) { snprintf(err, errlen, "%s: can't write", port); return false; }
    char line[128];
    if (!read_line(s, info->version, sizeof(info->version), 1500)) {
        /* Still armed from a capture nobody stopped (the window closed, the
         * program killed): it only listens for the stop byte, which it
         * looks for once a second. Stop it, throw away what it sends, ask
         * again. */
        trace("%s: no answer, stopping a capture it may still be armed for\n", port);
        uint8_t ff = 0xFF, junk[4096];
        sp_write(s, &ff, 1);
        sp_sleep(1500);
        while (sp_read(s, junk, sizeof(junk), 200) > 0) {}
        if (!send_packet(s, 0, NULL, 0) || !read_line(s, info->version, sizeof(info->version), 3000)) {
            snprintf(err, errlen, "%s: no answer (is it a LogicAnalyzer board? unplug it and plug it in again)", port);
            return false;
        }
    }
    int major = 0, minor = 0;
    const char *v = strrchr(info->version, 'V');
    if (strncmp(info->version, "LOGIC_ANALYZER_", 15) || !v || sscanf(v, "V%d_%d", &major, &minor) != 2) {
        snprintf(err, errlen, "%s: not a LogicAnalyzer board (it said \"%s\")", port, info->version);
        return false;
    }
    if (major < 6) {
        snprintf(err, errlen, "%s: firmware %s is too old (V6_0 or later)", port, info->version);
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!read_line(s, line, sizeof(line), 3000)) { snprintf(err, errlen, "%s: half an answer", port); return false; }
        sscanf(line, "FREQ:%d", &info->max_freq);
        sscanf(line, "BLASTFREQ:%d", &info->blast_freq);
        sscanf(line, "BUFFER:%d", &info->buffer);
        sscanf(line, "CHANNELS:%d", &info->channels);
    }
    if (!info->max_freq || !info->buffer || !info->channels) {
        snprintf(err, errlen, "%s: an answer the board shouldn't give", port);
        return false;
    }
    return true;
}

bool lp_info(const char *port, lp_info_t *info, char *err, int errlen) {
    sp_t s;
    if (!sp_open(port, &s, err, errlen)) return false;
    bool ok = hello(&s, info, port, err, errlen);
    sp_close(&s);
    return ok;
}

/* The firmware's CAPTURE_REQUEST (LogicAnalyzer_Structs.h), as laid out
 * by GCC on the RP2350 and by .NET's marshalling: 48 bytes. */
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static bool start(sp_t *s, const char *port, int type, int trig, int count, unsigned value,
                  const int *chan, int nchan, int freq, int pre, int post, int mode, char *err, int errlen) {
    uint8_t r[48] = { 0 };
    r[0] = (uint8_t)type;
    r[1] = (uint8_t)trig;
    r[2] = (uint8_t)count;
    r[4] = (uint8_t)value;
    r[5] = (uint8_t)(value >> 8);
    for (int k = 0; k < nchan && k < 24; k++) r[6 + k] = (uint8_t)chan[k];
    r[30] = (uint8_t)nchan;
    put32(r + 32, (uint32_t)freq);
    put32(r + 36, (uint32_t)pre);
    put32(r + 40, (uint32_t)post);
    r[44] = 0;                          /* loops */
    r[45] = 0;                          /* measure bursts */
    r[46] = (uint8_t)mode;
    trace("%s: start, trigger type %d", port, type);
    if (!send_packet(s, 1, r, sizeof(r))) { snprintf(err, errlen, "%s: can't write", port); return false; }
    char line[128];
    if (!read_line(s, line, sizeof(line), 10000)) { snprintf(err, errlen, "%s: no answer to the capture", port); return false; }
    if (strcmp(line, "CAPTURE_STARTED")) {
        snprintf(err, errlen, "%s: the board won't start the capture (%s)", port, line);
        return false;
    }
    return true;
}

static double t0;
static void trace(const char *fmt, ...) {
    if (!getenv("LP_DEBUG")) return;
    if (!t0) t0 = now_ms();
    fprintf(stderr, "[%7.0f ms] ", now_ms() - t0);
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}

static int lround_pos(double x) { return (int)(x + 0.5); }

double lp_now_ms(void) { return now_ms(); }
double lp_trigger_ms;

bool lp_capture(const lp_capture_req_t *q, uint8_t **v, int *n, int *pre_out, char *err, int errlen) {
    if (q->ndev < 1 || q->ndev > LP_MAXDEV) { snprintf(err, errlen, "1 to %d boards", LP_MAXDEV); return false; }
    /* Each board's channels (its own numbers, 0-23), in the order asked. */
    int dchan[LP_MAXDEV][24], dn[LP_MAXDEV] = { 0 };
    for (int k = 0; k < q->nchan; k++) {
        int d = q->chan[k] / 24, c = q->chan[k] % 24;
        if (q->chan[k] < 0 || d >= q->ndev || dn[d] >= 24) { snprintf(err, errlen, "channel %d: no board for it", q->chan[k] + 1); return false; }
        dchan[d][dn[d]++] = c;
    }
    if (!dn[0]) { snprintf(err, errlen, "no channel on the master (it has the trigger)"); return false; }
    if (q->trig_type == LP_FAST ? q->trig_base + q->trig_count > 5 : q->trig_base + q->trig_count > 16) {
        snprintf(err, errlen, "the trigger can only be on channels 1-%d", q->trig_type == LP_FAST ? 5 : 16);
        return false;
    }

    sp_t s[LP_MAXDEV];
    lp_info_t info[LP_MAXDEV];
    int nopen = 0;
    bool ok = false;
    uint8_t *raw[LP_MAXDEV] = { 0 };
    int bytes[LP_MAXDEV], mode[LP_MAXDEV];
    for (; nopen < q->ndev; nopen++) {
        if (!sp_open(q->port[nopen], &s[nopen], err, errlen)) goto done;
        if (!hello(&s[nopen], &info[nopen], q->port[nopen], err, errlen)) { sp_close(&s[nopen]); goto done; }
    }
    for (int d = 1; d < q->ndev; d++)
        if (strcmp(info[d].version, info[0].version)) {
            snprintf(err, errlen, "the boards have different firmware: %s %s, %s %s (they must be the same)",
                     q->port[0], info[0].version, q->port[d], info[d].version);
            goto done;
        }
    /* The limits: the firmware's, and his driver's checks of them. */
    int total = 1 << 30, maxf = info[0].max_freq;
    for (int d = 0; d < q->ndev; d++) {
        int top = 0;
        for (int k = 0; k < dn[d]; k++) if (dchan[d][k] > top) top = dchan[d][k];
        mode[d] = top < 8 ? 0 : top < 16 ? 1 : 2;
        bytes[d] = mode[d] == 0 ? 1 : mode[d] == 1 ? 2 : 4;
        int t = info[d].buffer / bytes[d];
        if (t < total) total = t;
        if (info[d].max_freq < maxf) maxf = info[d].max_freq;
    }
    if (q->freq < 1 || q->freq > maxf) { snprintf(err, errlen, "%d Hz: the boards do up to %d", q->freq, maxf); goto done; }
    if (q->pre < 2 || q->pre > total / 10 || q->post < 2 || q->pre + q->post > total) {
        snprintf(err, errlen, "%d before and %d after: the boards hold %d samples (2 to %d before)",
                 q->pre, q->post, total, total / 10);
        goto done;
    }
    /* The trigger's lag, as his driver allows for it: the master's own (a few
     * cycles at its top rate), the others' none to speak of. */
    double period = 1e9 / q->freq, delay = q->trig_type == LP_FAST ? 3 : 5;
    int moff = lround_pos(1e9 / maxf * delay / period + 0.3);
    int soff = lround_pos(delay / period + 0.3);
    /* A timed start: the boards are open and have answered; wait (the
     * last few ms without sleeping: Sleep is coarse). */
    while (q->start_at > 0 && now_ms() < q->start_at) {
        if (q->start_at - now_ms() > 30) sp_sleep(5);
    }
    for (int d = 1; d < q->ndev; d++)
        if (dn[d] && !start(&s[d], q->port[d], LP_EDGE, 24, 0, 0, dchan[d], dn[d], q->freq, q->pre + soff,
                            q->post - soff, mode[d], err, errlen))
            goto cancel;
    if (!start(&s[0], q->port[0], q->trig_type, q->trig_base, q->trig_count, q->trig_value, dchan[0], dn[0],
               q->freq, q->pre + moff, q->post - moff, mode[0], err, errlen))
        goto cancel;

    int timeout = q->timeout > 0 ? q->timeout : 60;
    lp_printf("Armed: waiting up to %d s for the trigger (Ctrl+C to give up)\n", timeout);
    fflush(stdout);
    catch_ctrl_c(true);
    int nsamp = q->pre + q->post;
    /* Read every board as its samples come, all at once (as his driver
     * does, a thread a board): left unread while another is read, a
     * board's data loses a USB packet now and then (64 or 128 bytes short,
     * seen 2026-11-06). Each sends its count, its samples, a byte. */
    int want[LP_MAXDEV], got[LP_MAXDEV] = { 0 }, left = 0;
    uint8_t head[LP_MAXDEV][4];
    for (int d = 0; d < q->ndev; d++) {
        want[d] = dn[d] ? 4 + nsamp * bytes[d] + 1 : 0;
        if (dn[d]) { raw[d] = malloc((size_t)want[d]); left++; }
    }
    double deadline = now_ms() + timeout * 1000.0, after = 0;
    bool triggered = false;
    while (left > 0) {
        if (lp_stop) { snprintf(err, errlen, "given up"); catch_ctrl_c(false); goto cancel; }
        if (!triggered && now_ms() > deadline) {
            snprintf(err, errlen, "no trigger in %d s", timeout);
            catch_ctrl_c(false);
            goto cancel;
        }
        if (triggered && now_ms() > after) {
            int n = 0;
            n += snprintf(err + n, errlen - n, "the samples stopped coming:");
            for (int d = 0; d < q->ndev && n < errlen; d++)
                if (got[d] < want[d]) n += snprintf(err + n, errlen - n, " %s %d bytes short%s", q->port[d], want[d] - got[d],
                                                    got[d] == 0 && d > 0 ? " (is the chain cable on?)" : "");
            catch_ctrl_c(false);
            goto cancel;
        }
        bool any = false;
        for (int d = 0; d < q->ndev; d++) {
            if (got[d] >= want[d]) continue;
            int r = sp_read(&s[d], raw[d] + got[d], want[d] - got[d] > 65536 ? 65536 : want[d] - got[d], left > 1 ? 2 : 50);
            if (r < 0) { snprintf(err, errlen, "%s: can't read", q->port[d]); catch_ctrl_c(false); goto cancel; }
            if (r == 0) continue;
            any = true;
            if (got[d] < 4 && got[d] + r >= 4) {
                memcpy(head[d], raw[d], 4);
                uint32_t len = (uint32_t)head[d][0] | (uint32_t)head[d][1] << 8 | (uint32_t)head[d][2] << 16 | (uint32_t)head[d][3] << 24;
                trace("%s: %u samples coming", q->port[d], len);
                if ((int)len != nsamp) {
                    snprintf(err, errlen, "%s sent %u samples, not %d", q->port[d], len, nsamp);
                    catch_ctrl_c(false);
                    goto cancel;
                }
                if (!triggered) {
                    triggered = true;
                    lp_trigger_ms = now_ms();
                    lp_printf("Triggered: reading the samples\n");
                    fflush(stdout);
                }
            }
            got[d] += r;
            if (got[d] >= want[d]) { left--; trace("%s: all read", q->port[d]); }
        }
        /* Once triggered, give up if nothing more comes for 15 s. */
        if (triggered && (any || after == 0)) after = now_ms() + 15000;
    }
    catch_ctrl_c(false);

    /* Each channel's samples: bit k of a board's sample is its k-th channel. */
    int kd[LP_MAXDEV] = { 0 };
    for (int k = 0; k < q->nchan; k++) {
        int d = q->chan[k] / 24, bit = kd[d]++;
        v[k] = malloc((size_t)nsamp);
        for (int i = 0; i < nsamp; i++) {
            const uint8_t *p = raw[d] + 4 + (size_t)i * bytes[d];
            uint32_t w = p[0] | (bytes[d] > 1 ? (uint32_t)p[1] << 8 : 0) |
                         (bytes[d] > 2 ? (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0);
            v[k][i] = (uint8_t)(w >> bit & 1);
        }
    }
    *n = nsamp;
    *pre_out = q->pre;
    ok = true;
    goto done;

cancel:
    for (int d = 0; d < nopen; d++) { uint8_t ff = 0xFF; sp_write(&s[d], &ff, 1); }
    sp_sleep(1500);                     /* the board looks for it once a second */
done:
    for (int d = 0; d < nopen; d++) sp_close(&s[d]);
    for (int d = 0; d < LP_MAXDEV; d++) free(raw[d]);
    return ok;
}
