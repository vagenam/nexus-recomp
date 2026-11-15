/*
 * host_placement.h -- opting a host thread out of HOST_PLACEMENT's restriction.
 *
 * Ported from infamous2-recomp (PF1):
 * HOST_PLACEMENT (runtime/syscalls/lv2_register.c) gives the frame clock and
 * the SPU workers physical cores of their own and confines every other thread
 * of the process to the remaining cores (the process's default CPU sets).
 * Timing-sensitive or thread-pool helpers (such as the audio mixer) can call
 * ps3_host_unplace_thread() at thread start to run across all system cores
 * when placement is active.
 */
#ifndef PS3_HOST_PLACEMENT_H
#define PS3_HOST_PLACEMENT_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>

static inline void ps3_host_unplace_thread(void)
{
    ULONG n = 0;
    if (!GetProcessDefaultCpuSets(GetCurrentProcess(), NULL, 0, &n) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return;
    if (!n) return;                                  /* no default set: thread runs anywhere already */
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    unsigned char* buf = len ? (unsigned char*)malloc(len) : NULL;
    if (!buf) return;
    if (GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        ULONG ids[256]; ULONG k = 0;
        for (ULONG o = 0; o < len && k < 256; ) {
            const SYSTEM_CPU_SET_INFORMATION* si = (const SYSTEM_CPU_SET_INFORMATION*)(buf + o);
            if (!si->Size) break;
            if (si->Type == CpuSetInformation) ids[k++] = si->CpuSet.Id;
            o += si->Size;
        }
        if (k) SetThreadSelectedCpuSets(GetCurrentThread(), ids, k);
    }
    free(buf);
}
#else
static inline void ps3_host_unplace_thread(void) {}
#endif

#endif /* PS3_HOST_PLACEMENT_H */
