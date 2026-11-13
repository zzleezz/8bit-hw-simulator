/* win_perf.h - Windows-only: keep hot threads off an Efficiency core.
 *
 * On a hybrid Intel CPU (P-cores + E-cores), Windows 11's scheduler leans
 * on two signals to decide where a thread runs: its priority, and whether
 * it is flagged for "EcoQoS" power throttling (on by default for most
 * threads, which biases them toward E-cores). A CPU-bound simulation
 * thread wants neither: top priority, throttling off, and (if the API is
 * there) explicitly steered to the CPU sets with the highest
 * EfficiencyClass, which is how Windows itself tells P-cores from E-cores
 * apart on a given chip. None of this changes behavior on a non-hybrid
 * CPU or an older Windows: the calls just become no-ops.
 *
 * The Windows 8/10 functions used here (SetThreadInformation,
 * SetProcessInformation, GetSystemCpuSetInformation,
 * SetThreadSelectedCpuSets) are looked up in kernel32.dll at run time
 * instead of linked directly, and the few structures and constants they
 * need are declared here. Two reasons: MinGW headers only declare these
 * functions when _WIN32_WINNT >= 0x0A00, and several MinGW toolchains
 * (MSYS2, TDM-GCC, older mingw-w64) default to an older value, so the
 * build failed there with "implicit declaration of function"; and a
 * direct link would stop the .exe from even starting on Windows 7. */
#ifndef WIN_PERF_H
#define WIN_PERF_H
#ifdef _WIN32

#include <windows.h>
#include <stdlib.h>

/* Same layout as the Windows SDK's THREAD/PROCESS_POWER_THROTTLING_STATE. */
typedef struct { ULONG Version, ControlMask, StateMask; } win_perf_throttle_state;
#define WIN_PERF_THROTTLE_VERSION        1u /* *_POWER_THROTTLING_CURRENT_VERSION */
#define WIN_PERF_THROTTLE_EXECUTION_SPEED 1u /* *_POWER_THROTTLING_EXECUTION_SPEED */
#define WIN_PERF_THREAD_POWER_THROTTLING  3  /* THREAD_INFORMATION_CLASS ThreadPowerThrottling */
#define WIN_PERF_PROCESS_POWER_THROTTLING 4  /* PROCESS_INFORMATION_CLASS ProcessPowerThrottling */

/* The leading part of SYSTEM_CPU_SET_INFORMATION that is read here. The
 * real entry is longer; entries are walked by their own Size field. */
typedef struct {
    DWORD Size;
    DWORD Type;                 /* 0 = CpuSetInformation */
    DWORD Id;
    WORD  Group;
    BYTE  LogicalProcessorIndex, CoreIndex, LastLevelCacheIndex, NumaNodeIndex;
    BYTE  EfficiencyClass;
} win_perf_cpu_set;

typedef BOOL (WINAPI *win_perf_set_info_fn)(HANDLE, int, LPVOID, DWORD);
typedef BOOL (WINAPI *win_perf_get_cpu_sets_fn)(void *, ULONG, PULONG, HANDLE, ULONG);
typedef BOOL (WINAPI *win_perf_set_thread_cpu_sets_fn)(HANDLE, const ULONG *, ULONG);

/* Look up a kernel32 export; NULL when this Windows doesn't have it. The
 * cast goes through a generic function pointer to keep
 * -Wcast-function-type quiet. */
static inline void (*win_perf_proc(const char *name))(void) {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    return k32 ? (void (*)(void))GetProcAddress(k32, name) : NULL;
}

/* Steer the CALLING thread to the fastest cores available. Call this from
 * inside the thread itself (thread priority and CPU sets are per-thread). */
static inline void win_boost_current_thread(void) {
    HANDLE th = GetCurrentThread();
    SetThreadPriority(th, THREAD_PRIORITY_HIGHEST);

    /* Windows 10 1709+: force EcoQoS throttling OFF for this thread
     * (StateMask 0 with the execution-speed bit in ControlMask). */
    win_perf_set_info_fn set_thread_info =
        (win_perf_set_info_fn)win_perf_proc("SetThreadInformation");
    if (set_thread_info) {
        win_perf_throttle_state pt = { WIN_PERF_THROTTLE_VERSION, WIN_PERF_THROTTLE_EXECUTION_SPEED, 0 };
        set_thread_info(th, WIN_PERF_THREAD_POWER_THROTTLING, &pt, sizeof(pt));
    }

    /* Windows 10 2004+: prefer the CPU sets with the highest
     * EfficiencyClass (the P-cores on a hybrid chip). This is a soft
     * preference, not a hard affinity mask: the thread can still run
     * elsewhere if those cores are busy, it just won't be parked on an
     * E-core by default anymore. */
    win_perf_get_cpu_sets_fn get_cpu_sets =
        (win_perf_get_cpu_sets_fn)win_perf_proc("GetSystemCpuSetInformation");
    win_perf_set_thread_cpu_sets_fn set_cpu_sets =
        (win_perf_set_thread_cpu_sets_fn)win_perf_proc("SetThreadSelectedCpuSets");
    if (!get_cpu_sets || !set_cpu_sets) return;

    ULONG len = 0;
    get_cpu_sets(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len) return;
    BYTE *info = malloc(len);
    if (!info) return;
    if (get_cpu_sets(info, len, &len, GetCurrentProcess(), 0)) {
        /* The entries are walked by their own Size: it may grow in later
         * Windows versions. */
        BYTE best = 0;
        for (ULONG o = 0; o + sizeof(win_perf_cpu_set) <= len; ) {
            const win_perf_cpu_set *e = (const void *)(info + o);
            if (!e->Size) break;
            if (e->Type == 0 && e->EfficiencyClass > best) best = e->EfficiencyClass;
            o += e->Size;
        }
        ULONG ids[256];
        ULONG nid = 0;
        for (ULONG o = 0; o + sizeof(win_perf_cpu_set) <= len && nid < 256; ) {
            const win_perf_cpu_set *e = (const void *)(info + o);
            if (!e->Size) break;
            if (e->Type == 0 && e->EfficiencyClass == best) ids[nid++] = e->Id;
            o += e->Size;
        }
        if (nid) set_cpu_sets(th, ids, nid);
    }
    free(info);
}

/* No EcoQoS throttling by default for threads this process creates, and
 * nothing else: the threads stay free to run on any core, E-cores
 * included, at full speed. What --build-atlas wants for its many worker
 * threads (see atlas.c). Windows 10 1709+; a no-op before that. */
static inline void win_unthrottle_process(void) {
    win_perf_set_info_fn set_process_info =
        (win_perf_set_info_fn)win_perf_proc("SetProcessInformation");
    if (!set_process_info) return;
    win_perf_throttle_state pt = { WIN_PERF_THROTTLE_VERSION, WIN_PERF_THROTTLE_EXECUTION_SPEED, 0 };
    set_process_info(GetCurrentProcess(), WIN_PERF_PROCESS_POWER_THROTTLING, &pt, sizeof(pt));
}

/* Process-wide baseline: a higher scheduling priority class, and no
 * EcoQoS throttling by default for threads this process creates. Call
 * once at startup, from any thread. */
static inline void win_boost_process(void) {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    win_unthrottle_process();
}

#endif /* _WIN32 */
#endif /* WIN_PERF_H */
