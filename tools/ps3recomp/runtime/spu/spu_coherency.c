/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store.
 */

#include <stdlib.h>
#include "spu_coherency.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * spinlock, same semantics, now shared. _InterlockedExchange is a clang-cl/MSVC
 * intrinsic (no runtime library symbol needed); elsewhere use the C11
 * equivalent, which lowers to the same LL/SC or lock-xchg on every target.
 * ===========================================================================*/
#if defined(_WIN32)
#include <windows.h>   /* SwitchToThread, WaitOnAddress */
static volatile long g_lockline = 0;
static volatile long g_lockline_waiters = 0;
typedef BOOL (WINAPI* woa_fn)(volatile VOID*, PVOID, SIZE_T, DWORD);
typedef VOID (WINAPI* wba_fn)(PVOID);
static woa_fn s_woa; static wba_fn s_wba; static volatile int s_ll_init;
static int s_ll_spin = 4096;
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, yield after a while, and then really wait. The bare exchange loop let
 * GH3's four idle job-policy pollers saturate the line and delay every other SPU
 * atomic -- FMOD's mixer task among them, 25-40 ms at a time.
 *
 * This was under _MSC_VER until v99, and the Windows build is zig's clang: it got
 * the bare exchange loop after all. With more threads than CPUs a holder that loses
 * its CPU kept all the others hammering the line until it got one back (Windows'
 * time slice: 15+ ms): 48 fps on 6 threads and 1-5 on 4, game audio at 6-14 blocks a
 * second of its 187.5, with every CPU flat out. Waiting frees the CPU the holder needs. */
void spu_lockline_lock(void)
{
    if (!_InterlockedExchange(&g_lockline, 1)) return;
    if (!s_ll_init) {
        extern int racacit_few_cpus(void);
        HMODULE m = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");
        if (m) { s_woa = (woa_fn)(void*)GetProcAddress(m, "WaitOnAddress"); s_wba = (wba_fn)(void*)GetProcAddress(m, "WakeByAddressSingle"); }
        if (racacit_few_cpus()) s_ll_spin = 128;
        s_ll_init = 1;
    }
    unsigned spins = 0, rounds = 0;
    do {
        while (g_lockline) {
            YieldProcessor();
            if (++spins < (unsigned)s_ll_spin) continue;
            spins = 0;
            if (++rounds < 3 || !s_woa) { SwitchToThread(); continue; }
            long held = 1;
            _InterlockedIncrement(&g_lockline_waiters);
            if (g_lockline) s_woa(&g_lockline, &held, sizeof held, 1);
            _InterlockedDecrement(&g_lockline_waiters);
        }
    } while (_InterlockedExchange(&g_lockline, 1));
}
void spu_lockline_unlock(void)
{
    _InterlockedExchange(&g_lockline, 0);
    if (g_lockline_waiters && s_wba) s_wba((PVOID)&g_lockline);
}
/* The short locks' wait: on a plain read, with a pause; a holder off its CPU gets a yield. */
void spu_spin_wait(volatile long* lock)
{
    unsigned spins = 0;
    while (*lock) {
        YieldProcessor();
        if (++spins >= 1024u) { spins = 0; SwitchToThread(); }
    }
}
#else
#include <stdatomic.h>
static atomic_flag g_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
#endif

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
#define SPU_COH_LINE_SHIFT  7                                  /* 128-byte lines */
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)                 /* 4 MiB */

unsigned char spu_coh_bitmap_g[SPU_COH_BITMAP_SZ];   /* read inline by spu_coh_is_reserved (header) */
#define s_coh_bitmap spu_coh_bitmap_g

/* Zero until the first GETLLAR anywhere. Keeps the PPU fast path off the
 * bitmap entirely for a title that runs no SPU code, and keeps every existing
 * test that stores through vm_write* paying one compare against a hot global.
 * Written once, under the lock; read unlocked, where a stale zero costs at most
 * one missed event on the very first reservation of the run. */
volatile int spu_coh_armed_g;
#define s_coh_armed spu_coh_armed_g

/* The SPU contexts that have reserved a line. A Crack in Time has more than
 * 8 (five job-chain lanes, the kernels' policy-module contexts, transient job
 * contexts), and a context past the end never received a lock-line event. */
#define SPU_COH_MAX_CTX 32
static spu_context* s_coh_ctxs[SPU_COH_MAX_CTX];
/* One past the highest slot ever used: the notify walks stop there (the
 * PPU and the RSX thread notify on every store to a reserved line). */
static volatile int s_coh_hi;

unsigned long g_spu_lr_raise = 0;

/* For the [mem] log line (game/main.cpp): lines marked in the bitmap now, and
 * writes spu_coh_notify_write was told of. A line stays marked once any SPU
 * reserved it, and every PPU store into a marked line takes the locked path:
 * if this count keeps climbing through a long session, so does that cost. */
unsigned long g_spu_coh_lines = 0;
unsigned long g_spu_coh_writes = 0;
unsigned long g_ppu_coh_stores = 0;   /* of those, PPU stores (ppu_loader.cpp; the rest are SPU atomics) */

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    if (!(s_coh_bitmap[line >> 3] & (1u << (line & 7)))) g_spu_coh_lines++;
    s_coh_bitmap[line >> 3] |= (unsigned char)(1u << (line & 7));
    s_coh_armed = 1;

    if (!ctx) return;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        if (s_coh_ctxs[i] == ctx) return;
        if (s_coh_ctxs[i] == NULL) { s_coh_ctxs[i] = ctx; if (i + 1 > s_coh_hi) s_coh_hi = i + 1; return; }
    }
    /* More live SPU contexts than the registry holds. The ones already in it
     * still get their events; this one would silently never wake, so say so. */
    { static int warned = 0;
      if (!warned) { warned = 1;
          fprintf(stderr, "[spu-coh] more than %d reserving SPU contexts -- "
                          "spu=0x%X will not receive lock-line events\n",
                  SPU_COH_MAX_CTX, ctx->spu_id);
          fflush(stderr); } }
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv_valid, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    /* Under the lock-line lock, which every walker of s_coh_ctxs holds: a
     * notifier that loaded this pointer just before the NULL store would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    for (int i = 0; i < SPU_COH_MAX_CTX; i++)
        if (s_coh_ctxs[i] == ctx) { s_coh_ctxs[i] = NULL; break; }
    spu_lockline_unlock();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    spu_lockline_lock();
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        uintptr_t c = (uintptr_t)s_coh_ctxs[i];
        if (c >= lo && c < hi) {
            static int _n = 0;
            if ((_n < 16 && (_n++, 1)))
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, s_coh_ctxs[i]->image_id);
            s_coh_ctxs[i] = NULL;
        }
    }
    spu_lockline_unlock();
}

/* racacit: hand a line a title marked with spu_coh_reserve(NULL, ...) back to
 * the PPU's lock-free store path (game/stubs.cpp, the collision records) --
 * unless an SPU holds a reservation on it right now; its next GETLLAR marks it
 * again anyway. Lock-line lock held. */
void spu_coh_unreserve_line(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    for (int i = 0, hi = s_coh_hi; i < hi; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (c && c->resv_valid && (c->resv_ea & ~127u) == line) return;
    }
    line >>= SPU_COH_LINE_SHIFT;
    if (s_coh_bitmap[line >> 3] & (1u << (line & 7))) g_spu_coh_lines--;
    s_coh_bitmap[line >> 3] &= (unsigned char)~(1u << (line & 7));
}

/* Hot paths use the inline spu_coh_is_reserved_fast (spu_coherency.h). */
int spu_coh_is_reserved(uint32_t addr) { return spu_coh_is_reserved_fast(addr); }

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    uint32_t line = ea & ~127u;
    for (int i = 0, hi = s_coh_hi; i < hi; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c || (const void*)c == self) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
    }
}

/* racacit: title hooks on SPU DMA of one guest structure (game/stubs.cpp, the
 * A Crack in Time collision command record). NULL for every other title.
 *   g_coh_write_hook     -- every write spu_coh_notify_write is told of: a PPU
 *                           store / stwcx. / vector store, or an SPU atomic, into
 *                           a reserved line (DMA PUTs use the _except variant
 *                           and are not reported). Lock-line lock held.
 *   g_spu_get_copy_hook  -- a DMA GET; returns 1 if it made the copy itself.
 *   g_spu_put_skip_hook  -- a DMA PUT into a reserved line, lock held; returns 1
 *                           to drop the copy (the transfer still completes). */
void (*g_coh_write_hook)(uint32_t ea) = 0;
int  (*g_spu_get_copy_hook)(void* spu, const uint8_t* ls, uint32_t lsa, uint32_t ea, uint32_t size,
                            uint8_t* dst, const uint8_t* src) = 0;
int  (*g_spu_put_skip_hook)(void* spu, const uint8_t* ls, uint32_t lsa, uint32_t ea, uint32_t size) = 0;

void spu_coh_notify_write(uint32_t ea)
{
    g_spu_coh_writes++;
    if (g_coh_write_hook) g_coh_write_hook(ea);
    uint32_t line = ea & ~127u;
    for (int i = 0, hi = s_coh_hi; i < hi; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;          /* reservation lost, PUTLLC must fail */
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            g_spu_lr_raise++;
            /* The event_status store has to be visible before the wake. The
             * waiter re-polls its predicate on a timeout as well, so a
             * straggling store costs latency and not the wakeup itself. */
            spu_ch_wake(c);
        }
    }
}

/* racacit: wake every parked SPU now. A Crack in Time posts its SPU jobs
 * with plain stores (no lock-line notify reaches the idle SPURS kernels),
 * then the main thread waits on them -- about nine submit/wait round trips
 * per view per frame (func_004DFDF0). An idle lane only noticed the new work
 * when its spu_lr_wait bound ran out (SPU_LR_WAIT_US), so every round trip
 * paid that bound. Called at the entry of the game's job/batch waits
 * (patch_lift step "spu kick"): each parked lane gets a reservation-lost
 * event, re-reads its workload state at once and finds the job. A spurious
 * LR event is harmless -- the kernel re-polls and parks again.
 * MEASURED 2026-10-18 (Galacton, standing still, 2x2 runs, order swapped):
 * no effect -- frame-waits stayed ~135 ms/s and frame times moved only with
 * run order. The waits are the jobs' own run time (igMobyPreRender ~1.4 ms a
 * frame), not lane pickup. So it is OFF by default; SPU_KICK_ON_WAIT=1 turns
 * it on. */
unsigned long g_spu_kicks = 0;
void racacit_spu_kick(void)
{
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("SPU_KICK_ON_WAIT"); s_on = e ? atoi(e) != 0 : 0; }
    if (!s_on) return;
    spu_lockline_lock();
    for (int i = 0, hi = s_coh_hi; i < hi; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c || !c->resv_valid) continue;
        c->event_status |= SPU_EVENT_LR;
        c->resv_valid = 0;
        g_spu_lr_raise++;
        spu_ch_wake(c);
    }
    spu_lockline_unlock();
    g_spu_kicks++;
}

#ifdef __cplusplus
}
#endif
