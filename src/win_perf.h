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
 * CPU or an older Windows: the calls just become no-ops. */
#ifndef WIN_PERF_H
#define WIN_PERF_H
#ifdef _WIN32

#include <windows.h>
#include <stdlib.h>

/* Steer the CALLING thread to the fastest cores available. Call this from
 * inside the thread itself (thread priority and CPU sets are per-thread). */
static inline void win_boost_current_thread(void) {
    HANDLE th = GetCurrentThread();
    SetThreadPriority(th, THREAD_PRIORITY_HIGHEST);

    THREAD_POWER_THROTTLING_STATE pt = { 0 };
    pt.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = 0;       /* 0 = force throttling OFF for this thread */
    SetThreadInformation(th, ThreadPowerThrottling, &pt, sizeof(pt));

    /* Windows 10 2004+: prefer the CPU sets with the highest
     * EfficiencyClass (the P-cores on a hybrid chip). This is a soft
     * preference, not a hard affinity mask: the thread can still run
     * elsewhere if those cores are busy, it just won't be parked on an
     * E-core by default anymore. */
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len) return;
    SYSTEM_CPU_SET_INFORMATION *info = malloc(len);
    if (!info) return;
    if (GetSystemCpuSetInformation(info, len, &len, GetCurrentProcess(), 0)) {
        /* The entries are walked by their own Size: it may grow in later
         * Windows versions. */
        BYTE best = 0;
        for (ULONG o = 0; o + sizeof(*info) <= len; ) {
            const SYSTEM_CPU_SET_INFORMATION *e = (const void *)((const BYTE *)info + o);
            if (!e->Size) break;
            if (e->Type == CpuSetInformation && e->CpuSet.EfficiencyClass > best) best = e->CpuSet.EfficiencyClass;
            o += e->Size;
        }
        ULONG ids[256];
        ULONG nid = 0;
        for (ULONG o = 0; o + sizeof(*info) <= len && nid < 256; ) {
            const SYSTEM_CPU_SET_INFORMATION *e = (const void *)((const BYTE *)info + o);
            if (!e->Size) break;
            if (e->Type == CpuSetInformation && e->CpuSet.EfficiencyClass == best) ids[nid++] = e->CpuSet.Id;
            o += e->Size;
        }
        if (nid) SetThreadSelectedCpuSets(th, ids, nid);
    }
    free(info);
}

/* No EcoQoS throttling by default for threads this process creates, and
 * nothing else: the threads stay free to run on any core, E-cores
 * included, at full speed. What --build-atlas wants for its many worker
 * threads (see atlas.c). */
static inline void win_unthrottle_process(void) {
    PROCESS_POWER_THROTTLING_STATE pt = { 0 };
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
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
