/*
 * ps3recomp - Timer and time syscalls (implementation)
 */

#include "sys_timer.h"
#include "sys_event.h"
#include "../memory/vm.h"
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#ifndef _WIN32
#include "../platform/win32_compat.h"   /* Interlocked*, Sleep (the timeline ring) */
#endif
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_timer_info g_sys_timers[SYS_TIMER_MAX];

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

int64_t lv2_usec_deadline(uint64_t usec)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    return now.QuadPart +
        (int64_t)((usec * (uint64_t)s_qpc_freq.QuadPart) / 1000000ULL);
}

int lv2_deadline_passed(int64_t deadline)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart >= deadline;
}
#endif

/* ---------------------------------------------------------------------------
 * The guest clocks' origin.
 *
 * Absolute host time is time since the HOST booted: after two weeks of uptime
 * it is ~1.3 million seconds, where a 32-bit float only holds steps of 0.125 s
 * (a float has 24 bits of mantissa: 2^20 s and up go in eighths). A Crack in
 * Time keeps a float seconds clock off the timebase and animates the Axiom
 * City cranes, their containers, the spinning platforms and the ship's
 * landing from it: they moved 8 times a second, and Ratchet riding a
 * platform with them. A PS3's clock counts from power-on, so the guest's
 * clocks here count from its first read, 30 s in. All of them subtract the
 * same origin, so they still share one epoch. RACACIT_GUEST_CLOCK=host keeps
 * absolute host time (the old behaviour).
 * -----------------------------------------------------------------------*/
#define PS3_GUEST_CLOCK_LEAD_S 30u
static int guest_clock_host(void)
{
    static int s = -1;
    if (s < 0) { const char* e = getenv("RACACIT_GUEST_CLOCK"); s = e && strcmp(e, "host") == 0; }
    return s;
}
#ifdef _WIN32
uint64_t ps3_guest_clock_epoch_qpc(void)
{
    static volatile LONG64 s_epoch = -1;
    LONG64 e = s_epoch;
    if (e >= 0) return (uint64_t)e;
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONG64 lead = (LONG64)PS3_GUEST_CLOCK_LEAD_S * s_qpc_freq.QuadPart;
    e = guest_clock_host() || now.QuadPart <= lead ? 0 : now.QuadPart - lead;
    const LONG64 was = InterlockedCompareExchange64(&s_epoch, e, -1);   /* the first caller's wins */
    return (uint64_t)(was >= 0 ? was : e);
}
#else
uint64_t ps3_guest_clock_epoch_ns(void)
{
    static volatile int64_t s_epoch = -1;
    int64_t e = __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE);
    if (e >= 0) return (uint64_t)e;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    const int64_t lead = (int64_t)PS3_GUEST_CLOCK_LEAD_S * 1000000000ll;
    e = guest_clock_host() || now <= lead ? 0 : now - lead;
    int64_t expect = -1;
    if (!__atomic_compare_exchange_n(&s_epoch, &expect, e, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) e = expect;
    return (uint64_t)e;
}
#endif

/* ---------------------------------------------------------------------------
 * PPU timebase (mftb/mftbu) -- THE guest clock.
 *
 * One global monotonic counter scaled to the PS3 timebase (79.8 MHz),
 * anchored at first use. The old lifter emission was a per-call-site
 * static that advanced 16667 ticks PER READ -- not a clock at all: every
 * guest timing loop (media pacers, throttles, profilers) computed garbage
 * from it, and each call site had a PRIVATE counter that only moved when
 * polled. Overflow-safe split multiply (rem < qpf, so
 * rem * 79.8e6 < ~8e14 << 2^63).
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static uint64_t tb_from_qpc(uint64_t d)
{
    const uint64_t q = (uint64_t)s_qpc_freq.QuadPart;
    return (d / q) * PS3_TIMEBASE_FREQ + (d % q) * PS3_TIMEBASE_FREQ / q;
}
#endif

#if defined(_WIN32) && defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#include <x86intrin.h>
#define PS3_TB_TSC 1
/* The timebase off the TSC. A Crack in Time reads it ~7 million times a
 * second (its profiler scopes and timed spin loops: ~120K reads a frame), and
 * QPC plus two 64-bit divisions a read was ~10% of the main thread. With an
 * invariant TSC -- what QPC itself runs on there -- a read is the TSC scaled
 * by a ratio measured against QPC: calibrated over the first 50 ms, then
 * re-anchored on the QPC reading once a second over an ever longer baseline,
 * and never stepping back. PS3_TB_QPC=1 reads QPC every time, as before. */
typedef struct { uint64_t tsc0, tb0, mul; } tb_anchor;   /* tb = tb0 + (tsc - tsc0) * mul / 2^32 */
static tb_anchor s_tba[2];
static volatile long s_tba_cur = -1;                      /* -1: not calibrated yet */
static volatile long s_tba_busy;
static volatile uint64_t s_tba_next;                      /* tsc of the next re-anchor */
static uint64_t s_tba_cal_tsc, s_tba_cal_qpc;
static int s_tsc_ok = -1;

static inline uint64_t tb_at(const tb_anchor* a, uint64_t t)
{
    return a->tb0 + (uint64_t)(((unsigned __int128)(t - a->tsc0) * a->mul) >> 32);
}

static uint64_t tb_slow(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint64_t t = __rdtsc();
    const uint64_t qpc = (uint64_t)now.QuadPart - ps3_guest_clock_epoch_qpc(), tbq = tb_from_qpc(qpc);
    if (s_tsc_ok < 0) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        s_tsc_ok = __get_cpuid(0x80000007u, &a, &b, &c, &d) && (d & (1u << 8)) && !getenv("PS3_TB_QPC");
    }
    const long cur = s_tba_cur;
    if (!s_tsc_ok) return tbq;
    if (InterlockedCompareExchange(&s_tba_busy, 1, 0) != 0)          /* another thread re-anchors */
        return cur >= 0 ? tb_at(&s_tba[cur], t) : tbq;
    uint64_t r = tbq;
    if (!s_tba_cal_tsc) { s_tba_cal_tsc = t; s_tba_cal_qpc = qpc; }
    else {
        const uint64_t dq = qpc - s_tba_cal_qpc, qf = (uint64_t)s_qpc_freq.QuadPart;
        const uint64_t dts = t - s_tba_cal_tsc;
        if (dq >= qf / 20u && dts) {
            const uint64_t dtb = tbq - tb_from_qpc(s_tba_cal_qpc);
            const uint64_t mul = (uint64_t)(((unsigned __int128)dtb << 32) / dts);
            if (cur >= 0) { const uint64_t est = tb_at(&s_tba[cur], t);
                /* PS3_TB_CHECK=1: how far the TSC reading had drifted from QPC. */
                static int chk = -1; if (chk < 0) chk = getenv("PS3_TB_CHECK") ? 1 : 0;
                if (chk) fprintf(stderr, "[tb] re-anchor: TSC reading %+lld ticks from QPC%c", (long long)(est - tbq), 10);
                if (est > r) r = est; }
            const long nxt = cur >= 0 ? 1 - cur : 0;
            s_tba[nxt].tsc0 = t; s_tba[nxt].tb0 = r; s_tba[nxt].mul = mul;
            s_tba_next = t + (uint64_t)(((unsigned __int128)dts * qf) / dq);   /* a second on */
            InterlockedExchange(&s_tba_cur, nxt);
        }
    }
    InterlockedExchange(&s_tba_busy, 0);
    return r;
}
#endif

uint64_t ppu_timebase_now(void)
{
#ifdef _WIN32
    /* QPC from the one guest origin (ps3_guest_clock_epoch_qpc), NOT anchored
     * at each clock's own first call: every guest-visible clock (this
     * timebase, sys_time_get_system_time, cellAudio port timestamps, GCM
     * report timestamps) must share one epoch. Anchoring each one separately
     * put the audio timestamps hours away from the game's own clock on
     * Windows. The TSC reading below is that same clock, anchored on QPC
     * readings. */
    ensure_qpc_init();
#ifdef PS3_TB_TSC
    { const long cur = s_tba_cur;
      if (cur >= 0) {
          const uint64_t t = __rdtsc();
          if (t < s_tba_next) return tb_at(&s_tba[cur], t);
      }
      return tb_slow(); }
#else
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return tb_from_qpc((uint64_t)now.QuadPart - ps3_guest_clock_epoch_qpc());
#endif
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec - ps3_guest_clock_epoch_ns();
    return (ns / 1000000000ull) * PS3_TIMEBASE_FREQ +
           (ns % 1000000000ull) * PS3_TIMEBASE_FREQ / 1000000000ull;
#endif
}

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

static void write_be64(uint32_t addr, uint64_t val)
{
    uint64_t* p = (uint64_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 56) & 0xFFULL) |
          ((val >> 40) & 0xFF00ULL) |
          ((val >> 24) & 0xFF0000ULL) |
          ((val >>  8) & 0xFF000000ULL) |
          ((val <<  8) & 0xFF00000000ULL) |
          ((val << 24) & 0xFF0000000000ULL) |
          ((val << 40) & 0xFF000000000000ULL) |
          ((val << 56) & 0xFF00000000000000ULL);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * PS3_TIMELINE=<presents>: one window of cross-thread events on one clock.
 *
 * Profiles say which thread is busy; they cannot say who waits on whom. From
 * the given present on, events are recorded (FIFO kicks, label writes, flips,
 * presents, drains, guest usleep wait spans per call site) until the buffer
 * fills, then printed in time order as "[tl] +us ev a b". 'P' (present) both
 * counts toward arming and is recorded.
 * -----------------------------------------------------------------------*/
#define PS3_TL_N 8192
static struct { unsigned long long t; char ev; uint32_t a, b; } s_tl[PS3_TL_N];
static volatile long s_tl_n = 0;
static long s_tl_arm = -1, s_tl_presents = 0;
static volatile long s_tl_dumped = 0;

static int ps3_tl_cmp(const void* x, const void* y)
{
    const unsigned long long a = ((const unsigned long long*)x)[0];
    const unsigned long long b = ((const unsigned long long*)y)[0];
    return a < b ? -1 : a > b;
}

/* PS3_TIMELINE_RING=<lr hex>[:<min us>[:<dumps>]] (with PS3_TIMELINE=1, which
 * turns the event sources on): record all the time into a ring instead of one
 * window, and print the wait and the 30 ms before it when a main-thread wait at
 * <lr> ('Q', racacit_wait_end) took >= min us (default 3000) -- up to <dumps>
 * times (default 4), 2 s apart. A fight's long waits come at random moments;
 * a window armed by present count misses them. "[tl-ring]" heads each dump. */
#define PS3_TL_RING 32768
static struct { unsigned long long t; char ev; uint32_t a, b; } s_tlr[PS3_TL_RING], s_tlr_cp[PS3_TL_RING];
static volatile long s_tlr_n = 0, s_tlr_busy = 0;
static int ps3_tl_ring(unsigned long long t, char ev, uint32_t a, uint32_t b)
{
    static int on = -1; static uint32_t lr, min_us; static long left; static unsigned long long last;
    if (on < 0) {
        const char* e = getenv("PS3_TIMELINE_RING");
        on = 0;
        if (e && *e) { char* p; lr = (uint32_t)strtoul(e, &p, 16); min_us = 3000; left = 4;
                       if (*p == ':') { min_us = (uint32_t)strtoul(p + 1, &p, 10); if (*p == ':') left = atol(p + 1); }
                       on = 1; }
    }
    if (!on) return 0;
    const long i = InterlockedIncrement(&s_tlr_n) - 1;
    const long k = i & (PS3_TL_RING - 1);
    s_tlr[k].t = t; s_tlr[k].ev = ev; s_tlr[k].a = a; s_tlr[k].b = b;
    if (ev == 'Q' && a == lr && b >= min_us && left > 0 && t - last > 2000000ull &&
        !InterlockedExchange(&s_tlr_busy, 1)) {
        left--; last = t;
        Sleep(2);                            /* let in-flight writers finish */
        memcpy(s_tlr_cp, s_tlr, sizeof s_tlr_cp);
        qsort(s_tlr_cp, PS3_TL_RING, sizeof s_tlr_cp[0], ps3_tl_cmp);
        const unsigned long long lo = t > 30000ull ? t - 30000ull : 0, hi = t + b + 2000ull;
        fprintf(stderr, "[tl-ring] wait lr %08X %u us; events from 30 ms before it (+us from there)%c", lr, b, 10);
        for (long j = 0; j < PS3_TL_RING; j++)
            if (s_tlr_cp[j].t >= lo && s_tlr_cp[j].t <= hi)
                fprintf(stderr, "[tl] +%-8llu %c %08X %08X%c", s_tlr_cp[j].t - lo, s_tlr_cp[j].ev,
                        s_tlr_cp[j].a, s_tlr_cp[j].b, 10);
        fprintf(stderr, "[tl-ring] end%c", 10);
        fflush(stderr);
        InterlockedExchange(&s_tlr_busy, 0);
    }
    return 1;
}

void ps3_tl_at(unsigned long long t, char ev, uint32_t a, uint32_t b)
{
    if (s_tl_arm < 0) { const char* e = getenv("PS3_TIMELINE"); s_tl_arm = e ? atol(e) : 0; }
    if (!s_tl_arm) return;
    if (ps3_tl_ring(t, ev, a, b)) return;
    if (ev == 'P') s_tl_presents++;
    if (s_tl_presents < s_tl_arm || s_tl_dumped) return;
    long i = InterlockedIncrement(&s_tl_n) - 1;
    if (i < PS3_TL_N) {
        s_tl[i].t = t; s_tl[i].ev = ev; s_tl[i].a = a; s_tl[i].b = b;
        if (i == PS3_TL_N - 1 && !InterlockedExchange(&s_tl_dumped, 1)) {
            Sleep(50);                       /* let in-flight writers finish */
            qsort(s_tl, PS3_TL_N, sizeof s_tl[0], ps3_tl_cmp);
            const unsigned long long t0 = s_tl[0].t;
            for (long k = 0; k < PS3_TL_N; k++)
                fprintf(stderr, "[tl] +%-8llu %c %08X %08X%c", s_tl[k].t - t0, s_tl[k].ev,
                        s_tl[k].a, s_tl[k].b, 10);
            fflush(stderr);
        }
    }
}
/* A small number per thread for timeline events (1, 2, ... in first-event order). */
unsigned ps3_tl_tix(void)
{
    static volatile long n;
    static _Thread_local unsigned ix;
    if (!ix) ix = (unsigned)InterlockedIncrement(&n);
    return ix;
}
void ps3_tl(char ev, uint32_t a, uint32_t b)
{
    extern unsigned long long ps3_qpc_us(void);
    if (s_tl_arm == 0) return;
    ps3_tl_at(ps3_qpc_us(), ev, a, b);
}

#ifdef _WIN32
/* Under 16 CPU threads the runtime stops spinning where it can: see the callers. */
int racacit_few_cpus(void)
{
    static int s_few = -1;
    if (s_few < 0) {
        DWORD_PTR pm = 0, sm = 0; unsigned n = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) for (; pm; pm >>= 1) n += (unsigned)(pm & 1u);
        s_few = n > 0 && n < 16;
    }
    return s_few;
}

/* A run of short polls from one place: 1 if this one really slept. */
static int usleep_real_sleep(uint32_t lr, uint64_t usec, int64_t now)
{
    static long s_spin = -2;
    if (s_spin == -2) { const char* e = getenv("RACACIT_USLEEP_SPIN_US");
                        s_spin = e ? atol(e) : (racacit_few_cpus() ? 100 : -1); }
    if (s_spin < 0) return 0;
    static __thread uint32_t t_lr; static __thread int64_t t_first, t_last; static __thread HANDLE t_tm; static __thread int t_tried;
    const int64_t per_us = s_qpc_freq.QuadPart / 1000000;
    if (lr != t_lr || now - t_last > 200 * per_us) { t_lr = lr; t_first = now; }
    if ((now - t_first) < s_spin * per_us) { t_last = now + (int64_t)usec * per_us; return 0; }
    if (!t_tried) { t_tried = 1; t_tm = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS); }
    LARGE_INTEGER due; due.QuadPart = -(LONGLONG)usec * 10;
    if (t_tm && SetWaitableTimer(t_tm, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(t_tm, INFINITE);
    else Sleep(1);
    LARGE_INTEGER t; QueryPerformanceCounter(&t); t_last = t.QuadPart;
    return 1;
}
#endif

/* ---------------------------------------------------------------------------
 * sys_timer_usleep
 *
 * r3 = microseconds
 * -----------------------------------------------------------------------*/
int64_t sys_timer_usleep(ppu_context* ctx)
{
    uint64_t usec = LV2_ARG_U64(ctx, 0);
    /* Timeline: consecutive sleeps from one call site on one thread are one
     * wait span, recorded when the thread does something else ('W' site us). */
    if (s_tl_arm) {
        extern unsigned long long ps3_qpc_us(void);
        static _Thread_local uint32_t w_lr;
        static _Thread_local unsigned long long w_t0, w_t1;
        const unsigned long long now = ps3_qpc_us();
        const uint32_t lr = (uint32_t)ctx->lr;
        if (lr != w_lr || now - w_t1 > 500) {
            if (w_lr) ps3_tl_at(w_t0, 'W', w_lr, (uint32_t)(w_t1 - w_t0));
            w_lr = lr; w_t0 = now;
        }
        w_t1 = now + usec;
    }
    { extern unsigned long long ps3_qpc_us(void);
      static int n=0; if (n < 60 && (n++, 1))
        fprintf(stderr, "[WAIT] t=%lluus timer_usleep(%llu us) lr=0x%08llX cia=0x%08llX\n", ps3_qpc_us(),
        (unsigned long long)usec, (unsigned long long)ctx->lr, (unsigned long long)ctx->cia); }
    /* PS3_USLEEP_REGS=<lr hex>: every 2048th usleep from that call site, print
     * r29/r31 and the 64-bit word r29 points at (the "what is it waiting on"
     * handle for most poll loops). */
    { static long s_ur = -1; static unsigned s_un = 0;
      if (s_ur < 0) { const char* e = getenv("PS3_USLEEP_REGS"); s_ur = e ? (long)strtoul(e, 0, 16) : 0; }
      if (s_ur > 0 && ((uint32_t)ctx->lr == (uint32_t)s_ur || (uint32_t)ctx->cia == (uint32_t)s_ur) && (s_un++ & 255) == 0) {
        /* The guest callers, from the stack's back chain: the LR save word
         * sits 16 bytes into each caller's frame. */
        uint32_t c[3] = { 0, 0, 0 }, sp = (uint32_t)ctx->gpr[1];
        for (int k = 0; k < 3 && sp && sp < 0x40000000u; k++) {
            sp = (uint32_t)__builtin_bswap64(*(uint64_t*)(vm_base + sp));
            if (!sp || sp >= 0x40000000u) break;
            c[k] = (uint32_t)__builtin_bswap64(*(uint64_t*)(vm_base + sp + 16));
        }
        fprintf(stderr, "[USLEEP-REGS] lr=%08X r29=%08llX *r29=%016llX r31=%08llX r28=%08llX callers %08X %08X %08X\n",
                (uint32_t)ctx->lr, (unsigned long long)ctx->gpr[29],
                (unsigned long long)__builtin_bswap64(*(uint64_t*)(vm_base + (uint32_t)ctx->gpr[29])),
                (unsigned long long)ctx->gpr[31], (unsigned long long)ctx->gpr[28], c[0], c[1], c[2]);
      } }
    /* PS3_POLLTOP=<seconds>: name the usleep poll sites.
     *
     * A title that is "hung" is usually not blocked on anything -- every thread
     * is spinning on sys_timer_usleep waiting for a flag, and [BLOCKSUM] only
     * says "syscall 141, 12s". This histograms the callers over a window and
     * prints the top ones, so the answer is a guest address rather than a
     * count. Keyed on ctx->lr: a host-stack backtrace through lifted code has
     * no unwind tables and invents chains (see the Guitar Hero III writeup),
     * and lr is the only guest chain that survives.
     *
     * Deliberately unlocked: the counts are a diagnostic, and a lost increment
     * under a race cannot change which site is at the top of a 30k-per-second
     * poll. */
    { static long s_pt = -1;
      if (s_pt < 0) { const char* e = getenv("PS3_POLLTOP");
                      s_pt = e ? (long)strtoul(e, 0, 10) : 0;
                      if (s_pt < 0) s_pt = 0; }
      if (s_pt > 0) {
        enum { PT_MAX = 128 };
        static uint32_t pt_lr[PT_MAX]; static uint32_t pt_n[PT_MAX];
        static uint64_t pt_us[PT_MAX]; static int pt_used = 0;
        static uint32_t pt_tid[PT_MAX];
        static unsigned long long pt_win = 0;
        uint32_t lr = (uint32_t)ctx->lr;
        int i = 0;
        for (; i < pt_used; i++) if (pt_lr[i] == lr) break;
        if (i == pt_used && pt_used < PT_MAX) {
            pt_lr[pt_used] = lr;
            /* The thread matters as much as the site: "who is waiting" is the
             * half that says whether a poll is the engine idling or the main
             * thread stuck. */
            pt_tid[pt_used] = (uint32_t)ctx->thread_id;
            pt_used++;
        }
        if (i < PT_MAX) { pt_n[i]++; pt_us[i] += usec; }
        { extern unsigned long long ps3_qpc_us(void);
          unsigned long long now = ps3_qpc_us();
          if (!pt_win) pt_win = now;
          else if (now - pt_win >= (unsigned long long)s_pt * 1000000ull) {
            fprintf(stderr, "--- usleep poll sites (last %lds) ---\n", s_pt);
            for (int k = 0; k < 8; k++) {
              int best = -1;
              for (int j = 0; j < pt_used; j++)
                if (pt_n[j] && (best < 0 || pt_n[j] > pt_n[best])) best = j;
              if (best < 0) break;
              fprintf(stderr, "  %8u calls  %6llums slept  tid=%-3u lr=0x%08X\n",
                      pt_n[best], (unsigned long long)(pt_us[best] / 1000),
                      pt_tid[best], pt_lr[best]);
              pt_n[best] = 0;
            }
            fflush(stderr);
            for (int j = 0; j < pt_used; j++) { pt_n[j] = 0; pt_us[j] = 0; }
            pt_win = now;
          } } } }

    /* PS3_WAIT_OBJ=<lr-hex>: when a usleep spin is reached from this return
     * address, dump the registers and the object they point at. A poll loop
     * tells you WHERE it is spinning; this tells you WHAT it is spinning on,
     * which is the part you actually need to find who never releases it. */
    { static long s_wo = -1;
      if (s_wo < 0) { const char* e = getenv("PS3_WAIT_OBJ");
                      s_wo = e ? (long)strtoul(e, 0, 16) : 0; }
      if (s_wo && (uint32_t)ctx->lr == (uint32_t)s_wo) {
        static int _n = 0;
        if ((_n < 8 && (_n++, 1))) {
          /* Dump the words at EVERY plausible object register, not just r29.
           * Which register holds the object is per-title -- r29 was right for
           * the title this was written for and is a spin COUNTER in Guitar
           * Hero III, where the object is in r30/r31. Printing one guess makes
           * the tool silently report the wrong memory as "the thing it waits
           * on", which is worse than printing nothing. */
          fprintf(stderr, "[wait-obj] lr=0x%08X sleep=%lluus", (uint32_t)ctx->lr,
                  (unsigned long long)usec);
          static const int regs[] = { 28, 29, 30, 31, 3 };
          for (unsigned ri = 0; ri < sizeof regs / sizeof regs[0]; ri++) {
            const int r = regs[ri];
            const uint32_t o = (uint32_t)ctx->gpr[r];
            fprintf(stderr, "  r%d=0x%08X", r, o);
            /* Only deref something that looks like a guest pointer: a small
             * integer is a count, and dumping "memory at 4" is noise. */
            if (vm_base && o >= 0x10000u) {
              fprintf(stderr, "[");
              for (int w = 0; w < 4; w++) {
                const uint8_t* q = vm_base + o + (unsigned)w * 4u;
                uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                             ((uint32_t)q[2] << 8)  | (uint32_t)q[3];
                fprintf(stderr, "%s%08X", w ? " " : "", v);
              }
              fprintf(stderr, "]");
            }
          }
          fprintf(stderr, "\n"); fflush(stderr);
        } } }

    /* POLLSITE: resolve the host chain of the 1ms poller (LBP bringup) to
     * guest functions -- names the stage that is starving. */
    { static int _ps = -1; if (_ps < 0) _ps = getenv("POLLSITE") ? 12 : 0;
      if (_ps > 0 && usec == 1000) { _ps--;
        extern void ppu_log_host_chain(const char*);
        ppu_log_host_chain("usleep1ms"); } }

    /* A Crack in Time stops its 14 sound-stream threads (after an autosave,
     * around area changes) by setting a flag and polling every 100 ms until
     * each one acknowledges (func_00997840, called from 0x00999868). One poll
     * that comes too early cost the main thread a whole 100 ms: the hitch while
     * playing. Poll every 1 ms there instead; the loop re-checks its flag. */
    if (usec == 100000 && (uint32_t)ctx->lr == 0x0099986Cu) usec = 1000;

#ifdef _WIN32
    /* Use high-resolution sleep via waitable timer for better precision */
    if (usec >= 1000) {
        HANDLE timer = CreateWaitableTimerW(NULL, TRUE, NULL);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -((LONGLONG)usec * 10); /* 100ns units, negative = relative */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
            CloseHandle(timer);
        } else {
            Sleep((DWORD)(usec / 1000));
        }
    } else if (usec > 0) {
        /* Sub-millisecond sleep. Sleep()/waitable timers round up to the
         * ~0.5-1 ms scheduler quantum, far too coarse for a guest that relies
         * on accurate microsecond pacing -- a short usleep() spin waiting on
         * another thread to make progress otherwise collapses to a single yield
         * and busy-spins at full speed (so the wait is effectively a no-op).
         * Busy-wait to a precise QPC deadline, yielding inside the spin
         * (SwitchToThread) so a sibling host thread the guest may be waiting on
         * still gets the core. Mirrors RPCS3's "Usleep Only" TSC busy-tail.
         *
         * With few CPU threads that costs more than it gives: the game's main
         * thread polls its SPU jobs and its frame limiter this way (usleep 30-50 us,
         * 8,000 calls a second), holding a whole CPU thread, at raised priority,
         * while the SPU lanes it is waiting for sit behind it at normal priority.
         * Under 16 CPU threads, a run of polls from one place spins for 100 us (most
         * hand-offs land in that) and then really sleeps. RACACIT_USLEEP_SPIN_US
         * sets the 100; a negative value always spins. */
        ensure_qpc_init();
        LARGE_INTEGER qpc_start, qpc_now;
        QueryPerformanceCounter(&qpc_start);
        if (usleep_real_sleep((uint32_t)ctx->lr, usec, qpc_start.QuadPart)) return CELL_OK;
        const int64_t qpc_deadline = qpc_start.QuadPart +
            (int64_t)((usec * (uint64_t)s_qpc_freq.QuadPart) / 1000000ULL);
        do {
            SwitchToThread();
            QueryPerformanceCounter(&qpc_now);
        } while (qpc_now.QuadPart < qpc_deadline);
    }
#else
    if (usec > 0) {
        struct timespec ts;
        ts.tv_sec  = (time_t)(usec / 1000000);
        ts.tv_nsec = (long)((usec % 1000000) * 1000);
        nanosleep(&ts, NULL);
    }
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_sleep
 *
 * r3 = seconds
 * -----------------------------------------------------------------------*/
int64_t sys_timer_sleep(ppu_context* ctx)
{
    uint32_t sec = LV2_ARG_U32(ctx, 0);

#ifdef _WIN32
    Sleep(sec * 1000);
#else
    sleep(sec);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_current_time
 *
 * r3 = pointer to receive seconds (u64*)
 * r4 = pointer to receive nanoseconds (u64*)
 * -----------------------------------------------------------------------*/
int64_t sys_time_get_current_time(ppu_context* ctx)
{
    uint32_t sec_addr  = LV2_ARG_PTR(ctx, 0);
    uint32_t nsec_addr = LV2_ARG_PTR(ctx, 1);

    uint64_t sec, nsec;

#ifdef _WIN32
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    /* Convert QPC to seconds + nanoseconds */
    sec  = (uint64_t)(now.QuadPart / s_qpc_freq.QuadPart);
    uint64_t remainder = (uint64_t)(now.QuadPart % s_qpc_freq.QuadPart);
    nsec = (remainder * 1000000000ULL) / (uint64_t)s_qpc_freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    sec  = (uint64_t)ts.tv_sec;
    nsec = (uint64_t)ts.tv_nsec;
#endif

    if (sec_addr != 0) {
        write_be64(sec_addr, sec);
    }
    if (nsec_addr != 0) {
        write_be64(nsec_addr, nsec);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_timebase_frequency
 *
 * Returns the PS3 timebase frequency in r3.
 * -----------------------------------------------------------------------*/
int64_t sys_time_get_timebase_frequency(ppu_context* ctx)
{
    (void)ctx;
    return (int64_t)PS3_TIMEBASE_FREQ;
}

/* ---------------------------------------------------------------------------
 * Periodic timer thread
 * -----------------------------------------------------------------------*/

/* Forward declaration */
static void timer_send_event(sys_timer_info* t);

#ifdef _WIN32
static DWORD WINAPI timer_thread_proc(LPVOID param)
{
    sys_timer_info* t = (sys_timer_info*)param;

    while (t->running) {
        DWORD ms = (DWORD)(t->period_usec / 1000);
        if (ms == 0) ms = 1;

        DWORD result = WaitForSingleObject(t->stop_event, ms);
        if (result == WAIT_OBJECT_0) {
            break; /* stop signaled */
        }

        if (t->running) {
            timer_send_event(t);
        }
    }

    return 0;
}
#else
static void* timer_thread_proc(void* param)
{
    sys_timer_info* t = (sys_timer_info*)param;

    pthread_mutex_lock(&t->mtx);
    while (!t->stop_flag) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(t->period_usec / 1000000);
        ts.tv_nsec += (long)((t->period_usec % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        int rc = pthread_cond_timedwait(&t->cv, &t->mtx, &ts);
        if (t->stop_flag) break;

        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&t->mtx);
            timer_send_event(t);
            pthread_mutex_lock(&t->mtx);
        }
    }
    pthread_mutex_unlock(&t->mtx);

    return NULL;
}
#endif

static void timer_send_event(sys_timer_info* t)
{
    if (t->event_queue_id <= 0 || t->event_queue_id > SYS_EVENT_QUEUE_MAX)
        return;

    sys_event_queue_info* q = &g_sys_event_queues[t->event_queue_id - 1];
    if (!q->active)
        return;

    sys_event_t evt;
    evt.source = t->source;
    evt.data1  = t->data1;
    evt.data2  = 0;
    evt.data3  = 0;

    /* Push event (ignore if queue full) */
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif

    if (q->count < q->capacity) {
        q->buffer[q->tail] = evt;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
#ifdef _WIN32
        WakeConditionVariable(&q->not_empty);
#else
        pthread_cond_signal(&q->not_empty);
#endif
    }

#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
}

/* ---------------------------------------------------------------------------
 * sys_timer_create
 *
 * r3 = pointer to receive timer ID (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_timer_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);

    int slot = -1;
    for (int i = 0; i < SYS_TIMER_MAX; i++) {
        if (!g_sys_timers[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_timer_info* t = &g_sys_timers[slot];
    memset(t, 0, sizeof(*t));
    t->active  = 1;
    t->running = 0;
    t->event_queue_id = 0;

#ifdef _WIN32
    t->stop_event    = NULL;
    t->thread_handle = NULL;
    t->timer_handle  = NULL;
#else
    pthread_mutex_init(&t->mtx, NULL);
    pthread_cond_init(&t->cv, NULL);
    t->stop_flag = 0;
#endif

    uint32_t timer_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, timer_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_destroy
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_destroy(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Stop if running */
    if (t->running) {
        t->running = 0;
#ifdef _WIN32
        if (t->stop_event) SetEvent(t->stop_event);
        if (t->thread_handle) {
            WaitForSingleObject(t->thread_handle, 3000);
            CloseHandle(t->thread_handle);
        }
        if (t->stop_event) CloseHandle(t->stop_event);
#else
        pthread_mutex_lock(&t->mtx);
        t->stop_flag = 1;
        pthread_cond_signal(&t->cv);
        pthread_mutex_unlock(&t->mtx);
        pthread_join(t->thread, NULL);
        pthread_mutex_destroy(&t->mtx);
        pthread_cond_destroy(&t->cv);
#endif
    }

    t->active = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_connect_event_queue
 *
 * r3 = timer_id
 * r4 = queue_id
 * r5 = source
 * r6 = data1
 * -----------------------------------------------------------------------*/
int64_t sys_timer_connect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);
    uint32_t queue_id = LV2_ARG_U32(ctx, 1);
    uint64_t source   = LV2_ARG_U64(ctx, 2);
    uint64_t data1    = LV2_ARG_U64(ctx, 3);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = (int32_t)queue_id;
    t->source         = source;
    t->data1          = data1;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_disconnect_event_queue
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_disconnect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_start
 *
 * r3 = timer_id
 * r4 = base_time (absolute start time, 0 = now)
 * r5 = period_usec
 * -----------------------------------------------------------------------*/
int64_t sys_timer_start(ppu_context* ctx)
{
    uint32_t timer_id    = LV2_ARG_U32(ctx, 0);
    /* uint64_t base_time = LV2_ARG_U64(ctx, 1); -- ignored for now */
    uint64_t period      = LV2_ARG_U64(ctx, 2);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (t->running)
        return (int64_t)(int32_t)CELL_EBUSY;

    if (period == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    t->period_usec = period;
    t->running     = 1;

#ifdef _WIN32
    t->stop_event    = CreateEventA(NULL, TRUE, FALSE, NULL);
    t->thread_handle = CreateThread(NULL, 0, timer_thread_proc, t, 0, NULL);
#else
    t->stop_flag = 0;
    pthread_create(&t->thread, NULL, timer_thread_proc, t);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_stop
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_stop(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (!t->running)
        return CELL_OK;

    t->running = 0;

#ifdef _WIN32
    if (t->stop_event) SetEvent(t->stop_event);
    if (t->thread_handle) {
        WaitForSingleObject(t->thread_handle, 3000);
        CloseHandle(t->thread_handle);
        t->thread_handle = NULL;
    }
    if (t->stop_event) {
        CloseHandle(t->stop_event);
        t->stop_event = NULL;
    }
#else
    pthread_mutex_lock(&t->mtx);
    t->stop_flag = 1;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mtx);
    pthread_join(t->thread, NULL);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 *
 * NOTE: There are syscall number collisions in the existing table:
 *   SYS_TIMER_USLEEP (141) == SYS_EVENT_FLAG_WAIT (141)
 *   SYS_TIMER_SLEEP  (142) == SYS_EVENT_FLAG_TRYWAIT (142)
 *   SYS_TIME_GET_CURRENT_TIME (145) == SYS_EVENT_FLAG_CANCEL (145)
 * The event flag handlers are registered separately and will win.
 * Timer sleep/time functions should be called via wrapper functions
 * or fixed syscall numbers should be assigned.
 * For now, we register the timer-specific syscalls that don't collide.
 * -----------------------------------------------------------------------*/
void sys_timer_init(lv2_syscall_table* tbl)
{
    memset(g_sys_timers, 0, sizeof(g_sys_timers));

#ifdef _WIN32
    ensure_qpc_init();
#endif

    lv2_syscall_register(tbl, SYS_TIMER_CREATE,                   sys_timer_create);
    lv2_syscall_register(tbl, SYS_TIMER_DESTROY,                  sys_timer_destroy);
    lv2_syscall_register(tbl, SYS_TIMER_START,                    sys_timer_start);
    lv2_syscall_register(tbl, SYS_TIMER_STOP,                     sys_timer_stop);
    lv2_syscall_register(tbl, SYS_TIMER_CONNECT_EVENT_QUEUE,      sys_timer_connect_event_queue);
    lv2_syscall_register(tbl, SYS_TIMER_DISCONNECT_EVENT_QUEUE,   sys_timer_disconnect_event_queue);

    /* These have conflicting numbers with event flag syscalls.
     * Register them here -- last registration wins. If events are
     * registered after timers, the event handlers will override.
     * The runtime should dispatch timer_usleep/sleep/get_current_time
     * via direct function calls instead. */
    lv2_syscall_register(tbl, SYS_TIME_GET_TIMEBASE_FREQUENCY, sys_time_get_timebase_frequency);

    /* Register these but be aware of collisions */
    lv2_syscall_register(tbl, SYS_TIMER_USLEEP,            sys_timer_usleep);
    lv2_syscall_register(tbl, SYS_TIMER_SLEEP,             sys_timer_sleep);
    lv2_syscall_register(tbl, SYS_TIME_GET_CURRENT_TIME,   sys_time_get_current_time);
}
