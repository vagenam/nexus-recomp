/*
 * ps3recomp - SPU channel + indirect-branch runtime glue
 *
 * Implements the externs the SPU lifter (tools/spu_lifter.py) emits:
 *   - spu_rdch / spu_rchcnt / spu_wrch : SPU channel access. MFC channels are
 *     routed to the DMA engine (spu_dma.h); mailboxes, signal notification,
 *     events and the decrementer use the spu_context channel fields.
 *   - spu_indirect_branch : resolves ctx->pc to a lifted spu_func_* via a
 *     registry that generated code populates by calling spu_recomp_register().
 *
 * The MFC engine state is kept per spu_context here (spu_context.h does not
 * embed one), in a small lazily-populated registry.
 */

#include "spu_dma.h"
#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define SPU_CSR_GET() ((unsigned long long)_mm_getcsr())
#define SPU_CSR_SET(v) _mm_setcsr((unsigned)(v))
#define SPU_CSR_SPU(v) ((v) | 0xE040u)                 /* DAZ, FTZ, round toward zero */
#elif defined(__aarch64__)
/* FPCR: FZ (24) flushes denormal operands and results, RMode (23:22) = 3
 * rounds toward zero -- the SPU's single precision, as MXCSR does on x86.
 * Without it every float op here rounded to nearest where the SPU truncates. */
static inline unsigned long long spu_fpcr_get(void) { unsigned long long v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline void spu_fpcr_set(unsigned long long v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
#define SPU_CSR_GET() spu_fpcr_get()
#define SPU_CSR_SET(v) spu_fpcr_set(v)
#define SPU_CSR_SPU(v) ((v) | (1ull << 24) | (3ull << 22))
#else
#define SPU_CSR_GET() 0ull
#define SPU_CSR_SET(v) ((void)(v))
#define SPU_CSR_SPU(v) (v)
#endif
#include "spu_coherency.h" /* lock-line lock + PPU-write coherence bitmap */
#include "spu_helpers.h"   /* spu_splat_u32 / spu_ls_read128 (SMC microstep) */
#include "spu_lockstep.h"
#include "spu_interp.h"    /* spu_lifted_fn / spu_lifted_lookup -- defined below */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../platform/ps3_setjmp.h"   /* setjmp/longjmp that never unwind (Windows) */

/* Byte-range inequality, 8 bytes a step. The toolchain's memcmp (zig's
 * compiler_rt) compares a byte at a time, and the overlay checks below run it
 * over whole function bodies on every indirect branch into a relocatable
 * module: 9% of the WWS job thread. Only equality is ever asked here. */
static inline int spu_mem_ne(const void* a, const void* b, size_t n)
{
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    while (n >= 32) {
        uint64_t x[4], y[4];
        memcpy(x, p, 32); memcpy(y, q, 32);
        if ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) return 1;
        p += 32; q += 32; n -= 32;
    }
    while (n >= 8) {
        uint64_t x, y; memcpy(&x, p, 8); memcpy(&y, q, 8);
        if (x != y) return 1;
        p += 8; q += 8; n -= 8;
    }
    while (n) { if (*p++ != *q++) return 1; n--; }
    return 0;
}
#ifndef _WIN32
#include <sched.h>   /* sched_yield */
#ifndef _WIN32
#include <unistd.h>  /* sysconf */
#endif
#endif
#include <time.h>
#include "../platform/win32_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The SPU decrementer ticks at the PS3 timebase, 79.8 MHz -- the same clock
 * sys_time_get_timebase_frequency reports to the PPU. Titles calibrate real
 * delays against it, so the rate has to be right, not merely non-zero. */
#define SPU_DECREMENTER_HZ  79800000ull

static uint64_t spu_host_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER s_freq;
    LARGE_INTEGER c;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&c);
    /* Split the divide to keep the multiply from overflowing: QPC counters are
     * large enough that (count * 1e9) wraps a u64 within hours of uptime. */
    return (uint64_t)(c.QuadPart / s_freq.QuadPart) * 1000000000ull
         + (uint64_t)(c.QuadPart % s_freq.QuadPart) * 1000000000ull
           / (uint64_t)s_freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* ---------------------------------------------------------------------------
 * Clean SPU job abort (longjmp). The `br .` halt idiom and other terminal
 * spins can't be escaped by setting a status flag (lifted code never checks
 * it), so spu_halt() longjmps back to spu_run_with_halt() in the dispatcher.
 * -----------------------------------------------------------------------*/
#if defined(_MSC_VER)
#  define SPU_TLS __declspec(thread)
#else
#  define SPU_TLS __thread
#endif
/* Called after an SPU atomic commits a 128-byte line, with the lock-line lock
 * released. cellSpurs sets it so a PPU blocked in cellSpursEventFlagWait wakes
 * when a task sets the flag, instead of on its next 2 ms poll. */
void (*g_spu_line_commit_hook)(uint32_t line) = 0;
/* SPU_LS_WATCH=0x1BE80[,0x927D80...]: watch up to 4 16-byte LS lines (reads
 * and writes) in every SPU image. See spu_ls_watch_hit2 for the fast path. */
#define SPU_WATCH_MAX 4
int g_spu_ls_watch_n = -1;
static unsigned s_spu_ls_watch[SPU_WATCH_MAX];
unsigned* spu_ls_watch_list(int* out_n)
{
    if (g_spu_ls_watch_n < 0) {
        int n = 0; const char* e = getenv("SPU_LS_WATCH");
        while (e && *e && n < SPU_WATCH_MAX) {
            s_spu_ls_watch[n++] = (unsigned)strtoul(e, (char**)&e, 0) & ~0xFu;
            while (*e == ',' || *e == ' ') e++;
        }
        g_spu_ls_watch_n = n;
    }
    *out_n = g_spu_ls_watch_n;
    return s_spu_ls_watch;
}
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{
    int n; spu_ls_watch_list(&n);
    if (!n) return;
    uint32_t a = lsa & (SPU_LS_MASK & ~0xFu);
    for (int i = 0; i < g_spu_ls_watch_n; i++) {
        if (s_spu_ls_watch[i] == a) {
            fprintf(stderr, "[spu-watch %s 0x%05X pc=0x%05X lr=0x%05X] "
                "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                is_write ? "WR" : "rd", a, pc, lr,
                p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
            fflush(stderr);
            break;
        }
    }
}

static SPU_TLS jmp_buf s_spu_halt_env;
static SPU_TLS int     s_spu_halt_armed = 0;

/* A non-local return replaces the old host call chain without changing
 * any architectural registers, local store, or pending guest work. */
void spu_restart_dispatch(spu_context* ctx)
{
    g_spu_trampoline_fn = 0;
    if (s_spu_halt_armed) longjmp(s_spu_halt_env, 2);
    fprintf(stderr, "[spu] non-local return outside execution driver at 0x%05X\n", ctx->pc);
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

/* SPU->PPU outbound-mailbox delivery hook. The SPU writing WrOutMbox /
 * WrOutIntrMbox must wake PPU code blocked on the SPURS event queue bound to
 * the SPU thread group (e.g. cellSpursInitialize). lv2_register.c installs a
 * handler that maps spu_group_id -> connected event queue and pushes an event.
 * NULL until installed (plain SPU jobs with no PPU listener stay a no-op). */
int (*g_spu_user_event_hook)(spu_context*, uint32_t) = NULL;
void (*g_spu_out_mbox_hook)(uint32_t group_id, uint32_t spu_id,
                            int is_intr, uint32_t value) = 0;

/* Every MFC PUT an SPU completes, if anyone is listening. Installed by the GCM
 * layer (libs/video/cellGcmSys.c), which needs it to find the RSX pushbuffer:
 * a title whose SPUs build the command stream never tells libgcm where that
 * stream is, so the only evidence is the DMA that writes it. NULL until
 * installed, so a title with no SPU-built FIFO pays nothing. */
void (*g_spu_put_hook)(uint32_t ea, uint32_t size) = 0;

/* Every MFC PUT, for cellAudio (libs/audio/cellAudio.c): it learns when the
 * game has finished writing a block of its audio port. */
void (*g_audio_put_hook)(uint32_t ea, uint32_t lsa, uint32_t size) = 0;

void spu_halt(spu_context* ctx)
{
    /* Register/context dump at halt-asserts: the WWS job's parameter guard
     * (heqi 0x1650 in job_wws_7BD900) checks (r12 bit0) & (r9 16-aligned) &
     * (r15 != 0) -- job descriptor/ABI values our execute path must supply.
     * Dump the low registers + the job's parameter area so a firing assert
     * names exactly which input is wrong. */
    { static int _n = 0;
      if (_n < 6 && ctx->status == SPU_STATUS_STOPPED_BY_HALT) { _n++;
        fprintf(stderr, "[spu-halt] img=%d pc=0x%05X regs:", ctx->image_id,
                ctx->pc & SPU_LS_MASK);
        for (int r = 3; r <= 16; r++)
            fprintf(stderr, " r%d=%08X", r, ctx->gpr[r]._u32[0]);
        fprintf(stderr, " r80=%08X r85=%08X\n",
                ctx->gpr[80]._u32[0], ctx->gpr[85]._u32[0]);
        fflush(stderr);
      } }
    if (s_spu_halt_armed) { s_spu_halt_armed = 0; longjmp(s_spu_halt_env, 1); }
}

/* Diagnostic: dump the taskset-policy scheduler's working tables (LS 0x2700..)
 * at func_00000E60 entry, to reverse why it computes "no runnable task". Env
 * YDKJ_E60. Called from the lifted taskset policy. */
void spu_dbg_e60(spu_context* ctx)
{
    static int s_e = -1; if (s_e < 0) s_e = getenv("YDKJ_E60") ? 1 : 0;
    if (!s_e) return;
    static int _d = 0; if (_d++ >= 6) return;
    const uint8_t* L = ctx->ls;
    #define RD(o) (((uint32_t)L[(o)]<<24)|((uint32_t)L[(o)+1]<<16)|((uint32_t)L[(o)+2]<<8)|L[(o)+3])
    fprintf(stderr, "[E60] r20=%08X r24=%08X | run2700=%08X ready2710=%08X pend2720=%08X en2730=%08X sig2740=%08X wait2750=%08X x2770=%08X\n",
            ctx->gpr[20]._u32[0], ctx->gpr[24]._u32[0],
            RD(0x2700), RD(0x2710), RD(0x2720), RD(0x2730), RD(0x2740), RD(0x2750), RD(0x2770));
    #undef RD
    fflush(stderr);
}

/* SPU `stop` / stop-and-signal. A real SPU stop HALTS the core; the previous
 * lifted emission only did `status=...; return;`, which unwound ONE frame and
 * let the caller's service loop keep running -- so a SPURS policy doing
 * stop-and-signal in a loop spun millions of times instead of yielding. Halt the
 * host SPU thread (longjmp to spu_run_with_halt) so the job stops AT the stop,
 * with ctx->status + the outbound mailbox value available for the dispatcher to
 * service. Env YDKJ_STOP_NOHALT restores the old (looping) behavior for A/B. */
void spu_stop(spu_context* ctx)
{
    /* A lifted `stop` sets status and RETURNS to its caller -- for a SPURS
     * policy this is stop-and-signal: it returns up into the kernel/policy
     * service loop, which continues (i.e. the SPU "resumes" past the stop). That
     * resume-by-return behavior is correct; the previous spin was caused by the
     * lack of (a) a PPU listener for the outbound mailbox and (b) mailbox
     * backpressure -- both handled in the channel write path, not here. So by
     * default DO NOT halt. Env YDKJ_STOP_HALT forces a hard halt (longjmp) for
     * A/B experiments (it makes the kernel terminate, which the game restarts). */
    static int s_halt = -1;
    if (s_halt < 0) s_halt = getenv("YDKJ_STOP_HALT") ? 1 : 0;
    ctx->status = SPU_STATUS_STOPPED_BY_STOP;
    if (s_halt) spu_halt(ctx);
}

/* Run a lifted SPU entry with a halt landing pad. Returns 1 if the job halted
 * (via spu_halt), 0 if it returned normally. */
int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx)
{
    int halted = 0;
    /* Job-args probe: image-1 (the WWS job binary) entries arrive at the crt
     * with r3/r4 zeroed in SOME runs while the jm2 runner sets them -- count
     * and stamp every image-1 run here to find the argless caller. */
    if (ctx->image_id == 1) {
        static int _n = 0;
        if (_n < 12) { _n++;
            fprintf(stderr, "[rwh] #%d img=1 ctx=%p r3=%08X r4=%08X entry=%p\n",
                    _n, (void*)ctx, ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                    (void*)entry);
            fflush(stderr); }
    }
    /* Also discard any saved register file keyed to this context pointer:
     * contexts are stack locals, so the address recurs and a stale save from
     * an earlier run would be restored over this job's arguments. */
    { extern void spu_irq_regs_forget(spu_context*); spu_irq_regs_forget(ctx); }
    ctx->steps = 0;   /* fresh run: step 0 is the entry */
    /* The host stack is empty here by definition, so no lifted caller frame
     * is live. A persistent context (a SPURS policy module re-entered every
     * scheduling quantum) otherwise carries the depth its last quantum
     * halted at: two frames per quantum for an idle job manager, and the
     * recursion guard halted the SPU at 2000 after a few minutes of idling. */
    ctx->host_depth = 0;
    ctx->irq_frame = 0;
    ctx->drain_ret_pc = 0;
    /* Nest-safe: a job run synchronously from inside another SPU's execution
     * re-enters here on the same host thread. The halt target is per-thread,
     * so save the outer one and put it back on the way out -- otherwise the
     * outer run's next halt longjmps into this (by then dead) frame, and a
     * stack-local job context skips its coherency unregister (GH3: crashes in
     * spu_coh_notify_write on a freed stack). */
    jmp_buf outer_env; const int outer_armed = s_spu_halt_armed;
    if (outer_armed) memcpy(outer_env, s_spu_halt_env, sizeof(jmp_buf));
    s_spu_halt_armed = 1;
    g_spu_trampoline_fn = 0;                        /* no stale transfer pending */
    /* Lockstep gate (env SPU_LOCKSTEP, default off): join the round-robin
     * ring and BLOCK until this ctx holds the run token, so only one lifted SPU
     * executes at a time. No-op when unarmed. The thread-local halt env above
     * makes a token pause/resume mid-run safe. */
    yz_lockstep_register(ctx);
    /* DAZ + FTZ + round toward zero while SPU code runs: the SPU's own
     * denormal handling and truncating single precision, which
     * spu_helpers.h's float fast paths rely on. The caller's mode comes
     * back on the way out (a halt's longjmp restores the setjmp-time one). */
    const unsigned long long spu_csr_saved = SPU_CSR_GET();
    switch (setjmp(s_spu_halt_env)) {
    case 1:
        halted = 1;
        g_spu_trampoline_fn = 0;
        break;
    case 2:
        SPU_CSR_SET(SPU_CSR_SPU(spu_csr_saved));
        /* A one-way guest stack reset invalidates every lifted host caller.
         * Re-enter its target with the same guest state on the driver stack. */
        ctx->host_depth = 0;
        g_spu_trampoline_fn = 0;
        spu_indirect_branch(ctx);
        SPU_DRAIN(ctx);
        break;
    default:
        /* SPU_DRAIN trampoline model: the top-level entry runs until its first
         * cross-function tail transfer, which sets g_spu_trampoline_fn and
         * returns; the drain loop re-enters each queued target until the SPU
         * halts (stop -> longjmp) or the trampoline empties. Nested brsl/bisl
         * calls drain inside their own call brackets (see the lifter). */
        SPU_CSR_SET(SPU_CSR_SPU(spu_csr_saved));
        entry(ctx);
        { static int _e = 0; static int s_aw = -1;
          if (s_aw < 0) s_aw = getenv("SPU_ARGWATCH") ? 1 : 0;
          if (s_aw && _e < 8) { _e++;
              fprintf(stderr, "[argwatch] after entry(): img=%d pc=0x%05X "
                      "r2=0x%08X r3=0x%08X r4=0x%08X\n", ctx->image_id,
                      (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[2]._u32[0],
                      ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0]);
              fflush(stderr); } }
        SPU_DRAIN(ctx);
    }
    SPU_CSR_SET(spu_csr_saved);
    yz_lockstep_unregister(ctx);   /* leave the ring; hand the token onward */
    s_spu_halt_armed = outer_armed;
    if (outer_armed) memcpy(s_spu_halt_env, outer_env, sizeof(jmp_buf));
    return halted;
}

/* ===========================================================================
 * Per-context MFC engine registry
 *
 * One MFC engine per live SPU context, in a small table. A thread group starts
 * all of its threads at once and each one arrives here on its first DMA, so the
 * table is written concurrently by as many host threads as the group has.
 *
 * Two rules keep that safe without putting a lock on the lookup, which every
 * MFC channel access goes through:
 *
 *   - A slot's owner field is written only by the thread that owns the context
 *     in it: the claim below, and the release when that context is done. Every
 *     reader is comparing the field against ITS OWN context pointer, so a read
 *     that races a claim either matches (its own slot, which only it can have
 *     written) or does not, and can never be handed another context's engine.
 *   - Choosing which free slot to claim is done under a lock. It used to be a
 *     scan followed by a store, and two threads that scanned before either
 *     stored both took the same slot and then shared one engine's queue and tag
 *     state -- one SPU's tag wait satisfied by the other SPU's transfer.
 * ===========================================================================*/
#define SPU_MAX_CONTEXTS 8

typedef struct {
    spu_context* volatile ctx;
    mfc_engine   mfc;
} spu_mfc_slot;

static spu_mfc_slot s_mfc_slots[SPU_MAX_CONTEXTS];
static SRWLOCK      s_mfc_claim_lock = SRWLOCK_INIT;

static mfc_engine* mfc_for(spu_context* ctx)
{
    for (int i = 0; i < SPU_MAX_CONTEXTS; i++)
        if (s_mfc_slots[i].ctx == ctx)
            return &s_mfc_slots[i].mfc;

    /* No slot yet: take one. The engine is initialized before the slot is
     * published, so it is never visible to anyone in a half-reset state. */
    mfc_engine* e = NULL;
    AcquireSRWLockExclusive(&s_mfc_claim_lock);
    for (int i = 0; i < SPU_MAX_CONTEXTS && !e; i++) {
        if (s_mfc_slots[i].ctx != NULL) continue;
        mfc_engine_init(&s_mfc_slots[i].mfc);
        s_mfc_slots[i].ctx = ctx;
        e = &s_mfc_slots[i].mfc;
    }
    if (!e) {
        /* Out of slots: fall back to a shared engine (correct for single-SPU).
         * Reachable only with more than SPU_MAX_CONTEXTS contexts running at
         * the same time, which is more SPUs than the machine has -- and the
         * sharing is silent, so say it once. Its one-time init is inside the
         * lock as well, or two threads arriving together would each memset an
         * engine the other is already using. */
        static mfc_engine fallback;
        static int fallback_init = 0;
        if (!fallback_init) {
            mfc_engine_init(&fallback);
            fallback_init = 1;
            fprintf(stderr, "[SPU] more than %d contexts hold an MFC engine at "
                    "once; the rest share one\n", SPU_MAX_CONTEXTS);
            fflush(stderr);
        }
        e = &fallback;
    }
    ReleaseSRWLockExclusive(&s_mfc_claim_lock);
    return e;
}

/* Give the slot `ctx` holds back to the table, called when its context is done.
 * Slots used to be claimed and never released, so a title with more SPU
 * contexts over its life than there are slots ran the rest of them on the
 * shared fallback engine -- and by then it is not a fresh engine but whatever
 * tag and queue state the previous owners left in it.
 *
 * Only the owner calls this, and only after its SPU has stopped, so the engine
 * is idle. The next claimant re-initializes it. A context that never issued a
 * DMA holds no slot and this is a scan that finds nothing. */
void spu_mfc_release(spu_context* ctx)
{
    if (!ctx) return;
    AcquireSRWLockExclusive(&s_mfc_claim_lock);
    for (int i = 0; i < SPU_MAX_CONTEXTS; i++) {
        if (s_mfc_slots[i].ctx != ctx) continue;
        s_mfc_slots[i].ctx = NULL;
        break;
    }
    ReleaseSRWLockExclusive(&s_mfc_claim_lock);
}

/* ===========================================================================
 * Atomic reservation (GETLLAR / PUTLLC / PUTLLUC) -- real lock-line semantics
 *
 * Multiple SPU kernel threads (the SPURS workload runtime runs several SPUs on
 * one shared lock-free queue) issue GETLLAR/PUTLLC on the SAME 128-byte lines.
 * Without honoring the reservation, two SPUs both "claim" the same slot and the
 * queue corrupts (observed: the 2nd claim returns garbage [1,1,1,1] -> the SPU
 * traps). PUTLLC must FAIL when the line changed since GETLLAR. We implement the
 * compare-and-swap under one global lock across all SPU host threads.
 * ===========================================================================*/
extern uint8_t* vm_base;

/* The spinlock guarding all atomic line ops now lives in spu_coherency.c as
 * spu_lockline_lock/unlock. It used to be a file-static here, which serialized
 * SPU against SPU but left the PPU free to store into a line in the middle of
 * a PUTLLC's compare-and-commit -- the update the SPU was about to make would
 * be written over, and the SPU would never learn the line had changed. Same
 * lock, same critical sections, now shared with the PPU store paths. */

/* PUTLLCs that hit the WATCHED sync line (g_barrier_sync_watch) -- isolates the
 * work-run on the stalled job queue from the dozen idle jobmanager instances. */
volatile unsigned g_spu_putllc_sync_hit = 0;

/* Lowest LS address treated as "not job code". Jobs load at 0 and
 * their stacks top out well below this; a branch at or above it with no
 * lifted code is a runaway, not a service call. */
#define SPU_JM2_KERNEL_BASE 0x15000u

/* Link-register value handed to a SPURS job so its final `bi $r0` lands
 * somewhere we recognise. The real job manager passes a return address into
 * itself; we have no manager, so we pass this and treat arriving here as job
 * completion. Chosen above the context/descriptor we park near the top of LS. */
#define SPU_JOB_RETURN_LS   0x3FF00u

/* SPU_LINEWATCH=<hex EA>: log every SPU atomic commit (PUTLLC/PUTLLUC) to that
 * 128-byte line with words 0..3 and 12..15 -- the PPU write watch cannot see
 * SPU-side stores, so a flag the PPU sets that "vanishes" needs this half. */
/* racacit: the guest EA a PPU wait is spinning on (0 = none, game/stubs.cpp
 * sets it). An SPU PUT / PUTLLC / PUTLLUC covering it records which image
 * wrote it and when -- "who sets this flag" for a spin the PPU write watch
 * cannot answer. */
volatile uint32_t g_racacit_watch_ea;
volatile int g_racacit_watch_img = -1;
volatile unsigned long long g_racacit_watch_t;
void racacit_watch_hit(const spu_context* ctx, uint32_t ea, uint32_t size)
{
    const uint32_t w = g_racacit_watch_ea;
    if (!w || w < ea || w - ea >= size) return;
    extern unsigned long long ps3_qpc_us(void);
    g_racacit_watch_img = ctx->image_id;
    g_racacit_watch_t = ps3_qpc_us();
}

static void spu_linewatch_note(const spu_context* ctx, uint32_t ea, const uint8_t* ls, const char* what)
{
    if (g_racacit_watch_ea) racacit_watch_hit(ctx, ea, MFC_ATOMIC_LINE);
    static int64_t s_lw = -2;
    if (s_lw == -2) { const char* e = getenv("SPU_LINEWATCH"); s_lw = e ? (int64_t)(strtoul(e, 0, 16) & ~127u) : -1; }
    if (s_lw < 0 || (ea & ~127u) != (uint32_t)s_lw) return;
    static int n, nmax = -1; if (nmax < 0) { const char* m = getenv("SPU_LINEWATCH_MAX"); nmax = m ? atoi(m) : 400; }
    if (n++ >= nmax) return;
    uint32_t w[16];
    for (int i = 0; i < 16; i++) { uint32_t t; memcpy(&t, ls + i * 4, 4); w[i] = __builtin_bswap32(t); }
    fprintf(stderr, "[linewatch] %s img=%d spu=%u pc=0x%05X ea=0x%08X w0-3=%08X %08X %08X %08X w12-15=%08X %08X %08X %08X\n",
            what, ctx->image_id, (unsigned)(ctx->spu_id & 7u), (unsigned)(ctx->pc & SPU_LS_MASK), ea,
            w[0], w[1], w[2], w[3], w[12], w[13], w[14], w[15]);
}

/* Returns 1 if `cmd` is an atomic line op and was handled here, else 0. */
/* racacit: idle backoff for SPU lock-line polling. A policy module with no
 * work (igPm, the WWS job manager) re-reads its queue lines in a tight
 * GETLLAR loop. On a PS3 that burns a dedicated SPU; here every SPU is a host
 * thread, and five or six of them polling flat out starved the PPU threads
 * (Bink decoded at well under 1 fps on two cores). When a thread keeps
 * reading the same few lines and their contents have not changed, yield and
 * then sleep briefly -- 20 us growing to 200 us -- and drop straight back to
 * full speed the moment any line it reads changes. SPU_NO_IDLE_BACKOFF=1
 * turns it off. */
static void spu_getllar_idle_backoff(const spu_context* ctx, uint32_t ea, const uint8_t* line)
{
    static int s_off = -1;
    if (s_off < 0) { const char* e = getenv("SPU_NO_IDLE_BACKOFF"); s_off = (e && *e && *e != '0') ? 1 : 0; }
    if (s_off) return;
    static SPU_THREAD_LOCAL uint32_t t_ea[4];
    static SPU_THREAD_LOCAL uint64_t t_h[4];
    static SPU_THREAD_LOCAL unsigned t_next, t_idle;
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < MFC_ATOMIC_LINE; i += 8) {
        uint64_t w; memcpy(&w, line + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    int same = 0;
    for (int i = 0; i < 4; i++)
        if (t_ea[i] == ea) { same = (t_h[i] == h); t_h[i] = h; goto found; }
    t_ea[t_next & 3] = ea; t_h[t_next & 3] = h; t_next++;
found:
    if (!same) { t_idle = 0; return; }
    /* An SPU that has polled unchanged lines for ~5 s is waiting on something
     * that is not coming: name the program, the PC and the line once (and
     * again every ~3 min), so a stalled job is identifiable from the log. */
    if (t_idle == 30000u || (t_idle > 30000u && (t_idle & 0xFFFFFu) == 0)) {
        uint32_t w[8];
        for (int i = 0; i < 8; i++) { uint32_t t; memcpy(&t, line + i * 4, 4); w[i] = __builtin_bswap32(t); }
        fprintf(stderr, "[spu-idle] img=%d pc=0x%05X lr=0x%05X polling ea=0x%08X (+%u lines) for %u polls; line: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                ctx->image_id, (unsigned)(ctx->pc & SPU_LS_MASK), ctx->gpr[0]._u32[0] & SPU_LS_MASK, ea,
                t_next > 4 ? 4u : t_next, t_idle, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        /* SPU_IDLE_DUMP=1: also the polling program's registers and the top of
         * its local store (igPm keeps its queue state at LS 0x3FE00..0x3FFFF). */
        if (getenv("SPU_IDLE_DUMP")) {
            fprintf(stderr, "[spu-idle]   regs:");
            static const int rr[] = { 3,4,5,6,7,8,9,10,11,80,81,83,84,85,86,87,88,89 };
            for (unsigned i = 0; i < sizeof rr / sizeof rr[0]; i++)
                fprintf(stderr, " r%d=%08X", rr[i], ctx->gpr[rr[i]]._u32[0]);
            fprintf(stderr, "\n");
            for (uint32_t a = 0x3FE00; a < 0x40000; a += 0x40) {
                fprintf(stderr, "[spu-idle]   LS %05X:", a);
                for (uint32_t j = 0; j < 0x40; j += 4) {
                    uint32_t t; memcpy(&t, ctx->ls + a + j, 4);
                    fprintf(stderr, " %08X", __builtin_bswap32(t));
                }
                fprintf(stderr, "\n");
            }
        }
    }
    /* How long to spin before giving the core away scales with the host:
     * with cores to spare (8+), a waiting SPU keeps spinning far longer, so
     * SPU<->SPU and PPU->SPU hand-offs (Bink's two decode lanes meet every
     * slice) see sub-microsecond latency instead of a 50-100 us timer sleep;
     * on a small host the waiting thread must step aside for the one it is
     * waiting on. */
    static int s_spin = 0, s_yield = 0;
    if (!s_spin) {
#ifdef _WIN32
        SYSTEM_INFO si; GetSystemInfo(&si); long ncpu = (long)si.dwNumberOfProcessors;
#else
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#endif
        s_spin  = ncpu >= 8 ? 20000 : 256;
        s_yield = ncpu >= 8 ? 40000 : 1024;
    }
    if (++t_idle < (unsigned)s_spin) {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
        return;
    }
    if (t_idle < (unsigned)s_yield) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
        return;
    }
    unsigned over = t_idle - (unsigned)s_yield;
    unsigned us = 20u << ((over >> 9) > 3 ? 3 : (over >> 9));   /* 20..160 */
    if (us > 200) us = 200;
#ifdef _WIN32
    /* NOT Sleep(1): that is 1-2 ms on Windows, and every PPU->SPU job
     * hand-off then waited that long once a worker had gone idle -- the game
     * fell to ~3 fps with the CPU idle the moment its SPU load got light
     * (first seen at the install movie, then for good). A high-resolution
     * waitable timer (Windows 10 1803+) sleeps for tens of microseconds;
     * without one, just yield. */
    {
        static SPU_THREAD_LOCAL HANDLE t_hrt;
        static SPU_THREAD_LOCAL int t_hrt_tried;
        if (!t_hrt_tried) {
            t_hrt_tried = 1;
            t_hrt = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                           TIMER_ALL_ACCESS);
        }
        if (t_hrt) {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(us > 50 ? 50 : us) * 10;   /* 100 ns units */
            if (SetWaitableTimer(t_hrt, &due, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(t_hrt, INFINITE);
                return;
            }
        }
        SwitchToThread();
    }
#else
    struct timespec d = { 0, (long)us * 1000L };
    nanosleep(&d, NULL);
#endif
}

/* RACACIT_LINE_RING=<hex>[,<hex>]: the last 512 changes to one or two
 * 128-byte guest lines, from every side that goes through the runtime --
 * SPU GETLLAR (when the line differs from the last entry), PUTLLC ok/fail,
 * PUTLLUC, DMA puts into it, PPU stwcx. and coherent stores -- with time,
 * who, pc/lr and the line's first 64 bytes afterwards. racacit_ring_dump()
 * prints it (the stall watch calls it): the interleaving behind a lost
 * update, which no single snapshot can show. */
static uint32_t s_ring_ea[2]; static int s_ring_on = -1;
typedef struct { unsigned long long t; uint32_t ea, id, pc; char op; uint8_t line[64]; } ring_ent;
static ring_ent s_ring[512]; static volatile long s_ring_n;
static int ring_on(void)
{
    if (s_ring_on < 0) {
        const char* e = getenv("RACACIT_LINE_RING"); s_ring_on = 0;
        if (e && *e) { char* end = 0; s_ring_ea[0] = (uint32_t)strtoul(e, &end, 16) & ~127u;
                       s_ring_ea[1] = (end && *end == ',') ? (uint32_t)strtoul(end + 1, 0, 16) & ~127u : s_ring_ea[0];
                       s_ring_on = 1; }
    }
    return s_ring_on;
}
void racacit_ring_note(char op, uint32_t id, uint32_t pc, uint32_t ea, int only_if_changed)
{
    if (!ring_on()) return;
    const uint32_t l = ea & ~127u;
    if (l != s_ring_ea[0] && l != s_ring_ea[1]) return;
    extern uint8_t* vm_base;
    if (only_if_changed) {   /* a polling GETLLAR: only when the line moved since the last entry for it */
        const long n = s_ring_n;
        for (long k = n - 1; k >= 0 && k >= n - 512; k--) {
            const ring_ent* r = &s_ring[k & 511];
            if (r->ea == l) { if (!memcmp(r->line, vm_base + l, 64)) return; break; }
        }
    }
    extern unsigned long long ps3_qpc_us(void);
    const long i = InterlockedIncrement(&s_ring_n) - 1;
    ring_ent* r = &s_ring[i & 511];
    r->t = ps3_qpc_us(); r->ea = l; r->id = id; r->pc = pc; r->op = op;
    memcpy(r->line, vm_base + l, 64);
}
void racacit_ring_dump(void)
{
    if (!ring_on()) return;
    const long n = s_ring_n;
    fprintf(stderr, "[line-ring] %ld events on %08X / %08X, the last %d:%c", n, s_ring_ea[0], s_ring_ea[1], n < 512 ? (int)n : 512, 10);
    for (long k = n > 512 ? n - 512 : 0; k < n; k++) {
        const ring_ent* r = &s_ring[k & 511];
        char w[16 * 9 + 1]; int p = 0;
        for (int j = 0; j < 16; j++)
            p += snprintf(w + p, sizeof w - p, "%02X%02X%02X%02X ", r->line[j*4], r->line[j*4+1], r->line[j*4+2], r->line[j*4+3]);
        fprintf(stderr, "[line-ring] %llu %c id=%u pc=0x%05X %08X: %s%c", r->t, r->op, r->id, r->pc, r->ea, w, 10);
    }
}

/* PUTLLC's commit: the bytes of the 128-byte line where the SPU's new copy
 * (ls) differs from its reservation snapshot (old), and no others. See the
 * call site. 16 bytes at a time; an unchanged chunk is skipped, a fully
 * changed one stored whole, a mixed one byte by byte. */
static void spu_putllc_commit_changed(uint8_t* mem, const uint8_t* ls, const uint8_t* old)
{
    for (uint32_t i = 0; i < MFC_ATOMIC_LINE; i += 16u) {
        uint64_t n0, n1, o0, o1;
        memcpy(&n0, ls + i, 8); memcpy(&n1, ls + i + 8, 8);
        memcpy(&o0, old + i, 8); memcpy(&o1, old + i + 8, 8);
        const uint64_t d0 = n0 ^ o0, d1 = n1 ^ o1;
        if (!(d0 | d1)) continue;
        int all = 1;
        for (int k = 0; k < 8; k++) if (!((d0 >> (k * 8)) & 0xFFu) || !((d1 >> (k * 8)) & 0xFFu)) { all = 0; break; }
        if (all) { memcpy(mem + i, ls + i, 16); continue; }
        for (uint32_t k = 0; k < 16u; k++)
            if (ls[i + k] != old[i + k]) ((volatile uint8_t*)mem)[i + k] = ls[i + k];
    }
}

static int spu_mfc_atomic(spu_context* ctx, uint32_t cmd)
{
    /* Classify FIRST. Every MFC_Cmd write lands here, and only the switch at
     * the bottom used to filter -- so the uncommitted-EA guard below reported
     * ORDINARY DMA as `[spu-atomic]`. A plain put (0x20) to a garbage EA read
     * as an atomic spin, which is a genuinely misleading place to start
     * debugging from. Non-atomic commands belong to the DMA engine. */
    if (cmd != MFC_GETLLAR_CMD && cmd != MFC_PUTLLC_CMD &&
        cmd != MFC_PUTLLUC_CMD && cmd != MFC_PUTQLLUC_CMD)
        return 0;

    uint32_t ea  = ctx->mfc_eal & ~(uint32_t)(MFC_ATOMIC_LINE - 1);
    uint32_t lsa = ctx->mfc_lsa & SPU_LS_MASK;
    uint8_t* ls  = &ctx->ls[lsa];
    uint8_t* mem = vm_base + ea;

    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_n = 0; static uint32_t s_lastea = 0;
        if ((++s_n % 2000000) == 0 || ea != s_lastea) {
          if ((s_n % 2000000) == 0)
            fprintf(stderr, "[atomcnt] %llu atomic ops; last cmd=0x%X ea=0x%08X\n",
                    (unsigned long long)s_n, cmd, ea);
          s_lastea = ea; } } }
    /* SPU_ATOMTRACE=1 keeps the old 40-line cap; =<N> raises it. A busy image
     * (FMOD) exhausts 40 before a quieter one issues its first atomic, which
     * reads as "that image never does atomics" when it simply never got a line. */
    { static int s_at = -1;
      if (s_at < 0) { const char* e = getenv("SPU_ATOMTRACE");
                      int v = e ? atoi(e) : 0; s_at = e ? (v > 1 ? v : 40) : 0; }
      if (s_at) { static int _a=0; if (_a++ < s_at)
        fprintf(stderr, "[atom] cmd=0x%02X ea=0x%08X (img=%d)\n", cmd, ea, ctx->image_id); } }
    /* cri task (img22) atomic on the taskset: dump the loaded bitset line so we can
     * see if the task reads MY taskset (0x4005E000) with my READY bit, or elsewhere. */
    { static int s_ct=-1; if(s_ct<0) s_ct=getenv("SPU_ATOMTRACE")?1:0;
      if(s_ct && ctx->image_id==22 && cmd==0xD0 && mfc_ea_range_committed(ea,16)) {
        static int _c=0; if((_c < 24 && (_c++, 1))){
          uint8_t* m=vm_base+ea;
          #define BW(o) (((uint32_t)m[o]<<24)|((uint32_t)m[o+1]<<16)|((uint32_t)m[o+2]<<8)|m[o+3])
          fprintf(stderr,"[cri-atom] GETLLAR ea=0x%08X line[0..0x30]: %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X\n",
            ea, BW(0),BW(4),BW(8),BW(0xC), BW(0x10),BW(0x14),BW(0x18),BW(0x1C), BW(0x20),BW(0x24),BW(0x28),BW(0x2C));
          #undef BW
        } } }
    /* YDKJ_CRI_R4: dump the CellSpursTaskset bitsets when the policy atomically
     * touches my taskset (0x0F000000), to watch the task-activation state machine
     * (why task0 isn't selected+first-run). running@0 ready@0x10 pending@0x20
     * enabled@0x30 signalled@0x40 waiting@0x50 (each 16B; word0 = MSB, task0=bit127). */
    { static int s_td = -1; if (s_td < 0) s_td = (getenv("YDKJ_CRI_CHAIN") && getenv("SPU_ATOMTRACE")) ? 1 : 0;
      if (s_td && ea >= 0x0F000000u && ea < 0x0F001900u) {
        extern uint8_t* vm_base;
        static int _t=0; if (vm_base && (_t < 24 && (_t++, 1))) {
            uint8_t* t = vm_base + 0x0F000000u;
            #define TW(o) (((uint32_t)t[o]<<24)|((uint32_t)t[o+1]<<16)|((uint32_t)t[o+2]<<8)|t[o+3])
            fprintf(stderr, "[tset] %s run=%08X rdy=%08X pnd=%08X ena=%08X sig=%08X wait=%08X | wid=%08X last=%02X\n",
                    cmd==0xD0?"GET":cmd==0xB4?"PUT":"?", TW(0x00), TW(0x10), TW(0x20), TW(0x30), TW(0x40), TW(0x50), TW(0x74), t[0x73]);
            #undef TW
        }
      } }

    /* Guard atomic line ops against an uncommitted/garbage EA (e.g. a SPURS
     * policy computing a lock-line address from an incomplete instance context).
     * Same rationale as the DMA EA guard: a bad guest atomic must not segfault
     * the host. GETLLAR returns a zeroed line (no reservation); PUTLLC fails. */
    if (!mfc_ea_range_committed(ea, MFC_ATOMIC_LINE)) {
        static int s_w = 0;
        if ((s_w < 16 && (s_w++, 1)))
            fprintf(stderr, "[spu-atomic] cmd=0x%X ea=0x%08X pc=0x%05X img=%d uncommitted -- skipped\n",
                    cmd, ea, (uint32_t)ctx->pc & SPU_LS_MASK, ctx->image_id);
        if (cmd == MFC_GETLLAR_CMD) {
            memset(ls, 0, MFC_ATOMIC_LINE);
            ctx->resv_ea = ea; ctx->resv_valid = 0; ctx->atomic_stat = 4;
        } else {
            ctx->atomic_stat = 1;   /* PUTLLC failure (line "moved") */
        }
        return 1;
    }

    /* SPU_ATOM_EA=<hex>: log every lock-line atomic touching that EA`s 128B line.
     * SPU_ATOM_FULL=1 additionally dumps 32 bytes of the line before and after.
     *
     * Built into ONE buffer and written with a single fprintf: several SPU host
     * threads and the PPU all write stderr, so a dump split across many fprintf
     * calls comes out shredded and unreadable exactly when it matters. */
    { static int s_ae = -1; static uint32_t s_aea;
      if (s_ae < 0) { const char* e = getenv("SPU_ATOM_EA");
        s_aea = e ? (uint32_t)strtoul(e, 0, 16) & ~127u : 0; s_ae = s_aea ? 1 : 0;
        if (s_ae == 1) { extern uint32_t g_barrier_sync_watch;
            g_barrier_sync_watch = s_aea; } /* arm PUTLLC OK/FAIL verdict log */ }
      static int s_po = -1; if (s_po < 0) s_po = getenv("SPU_ATOM_EA_PUTONLY") ? 1 : 0;   /* racacit */
      if (s_ae == 1 && ((uint32_t)ea & ~127u) == s_aea && !(s_po && cmd == MFC_GETLLAR_CMD)) {
          static int _n = 0, s_cap = -1;
          /* SPU_ATOM_EA_MAX=<n>: lines to print (default 48, 0 = unlimited). */
          if (s_cap < 0) { const char* m = getenv("SPU_ATOM_EA_MAX"); s_cap = m ? atoi(m) : 48; }
          if (!s_cap || _n++ < s_cap) {
              extern uint8_t* vm_base;
              const uint8_t* r = vm_base + ((uint32_t)ea & ~127u);
              static int s_af = -1;
              if (s_af < 0) { const char* f = getenv("SPU_ATOM_FULL"); s_af = f ? (atoi(f) > 1 ? 2 : 1) : 0; }
              char buf[1024]; int p = 0;
              /* lr (gpr[0]) as well as pc: these atomics sit in generic helpers,
               * so pc names the HELPER and only lr names the real caller. */
              p += snprintf(buf + p, sizeof buf - p,
                            "[atom-ea] cmd=0x%X img=%d pc=0x%05X lr=0x%05X ea=0x%08X",
                            cmd, ctx->image_id, (uint32_t)ctx->pc & SPU_LS_MASK,
                            ctx->gpr[0]._u32[0] & SPU_LS_MASK, (uint32_t)ea);
              int nb = s_af == 2 ? 128 : s_af ? 64 : 8;   /* 64 covers +0x30 pendingRecv; =2 the whole line */
              p += snprintf(buf + p, sizeof buf - p, " RAM=");
              for (int i = 0; i < nb && p < (int)sizeof buf - 4; i++)
                  p += snprintf(buf + p, sizeof buf - p, "%02X%s", r[i],
                                (i % 4 == 3) ? " " : "");
              if (cmd == MFC_PUTLLC_CMD) {
                  p += snprintf(buf + p, sizeof buf - p, " STORE=");
                  for (int i = 0; i < nb && p < (int)sizeof buf - 4; i++)
                      p += snprintf(buf + p, sizeof buf - p, "%02X%s", ls[i],
                                    (i % 4 == 3) ? " " : "");
              }
              fprintf(stderr, "%s\n", buf); fflush(stderr);
          }
      } }
    /* Bink sync-line atomic trace (armed by the PPU-side producer probe). */
    { extern uint32_t g_barrier_sync_watch;
      uint32_t b = g_barrier_sync_watch;
      if (b && ea >= (b & ~127u) && ea < ((b + 0xC0 + 127) & ~127u)) {
          static int _n = 0;
          if ((_n < 64 && (_n++, 1)))
              fprintf(stderr, "[sync-atomic] %s img=%d pc=0x%05X ea=0x%08X\n",
                      cmd == MFC_GETLLAR_CMD ? "GETLLAR" :
                      cmd == MFC_PUTLLC_CMD ? "PUTLLC" : "PUTLLUC",
                      ctx->image_id, (uint32_t)ctx->pc, ea);
      } }
    switch (cmd) {
    case MFC_GETLLAR_CMD:
        /* Lockstep tick: a guest GETLLAR..PUTLLC poll loop is intra-function
         * (all gotos), so it never crosses the SPU_DRAIN tick site -- without a
         * tick here a polling SPU would hold the run token forever while the
         * peer that must WRITE the line waits for it (canersaka ticks the
         * GETLLAR fast+slow paths for exactly this reason). */
        yz_lockstep_tick(ctx);
        spu_lockline_lock();
        /* Tell the PPU store paths this line is live, so a store into it goes
         * through the lock and raises SPU_EVENT_LR here instead of landing
         * unannounced. Nothing else about this transaction changes. */
        spu_coh_reserve(ctx, ea);

        /* ONE read of guest memory, then the snapshot from that copy.
         *
         * This used to memcpy from `mem` twice -- once to the local store, once
         * to resv_line -- and a PPU store landing BETWEEN the two reads gave the
         * SPU a stale line and a fresh snapshot. That is a permanent deadlock,
         * and it is the one this port spent a session chasing: the SPU compares
         * its (stale) copy, finds produced == consumed, and sleeps on
         * MFC_LLR_LOST_EVENT; spu_resv_lost_poll then compares memory against a
         * snapshot that ALREADY has the new value, finds no difference, and
         * never raises the event. Measured at a freeze: mirror 0x9E, snapshot
         * 0x9F, memory 0x9F, and the SPU polling rchcnt 1.6 billion times.
         *
         * Hardware cannot produce that state: GETLLAR is a single atomic
         * 128-byte read, so the reserved data and the reservation come from the
         * same instant. Copying the snapshot from `ls` restores that property,
         * and a store that lands after the read now leaves BOTH stale -- which
         * is what makes the lost-reservation event fire. */
        memcpy(ls, mem, MFC_ATOMIC_LINE);              /* line -> local store */
        /* Snapshot from `ls`, NOT a second read of `mem`. Hardware GETLLAR is a
         * single atomic 128-byte read, so the reserved data and the reservation
         * come from the same instant. Reading guest memory twice lets a PPU store
         * land between them, leaving the SPU a STALE line and a FRESH snapshot --
         * and then both halves of the wait fail: the SPU compares its stale copy,
         * sees produced == consumed and sleeps on MFC_LLR_LOST_EVENT, while the
         * reservation poll compares memory against a snapshot that already holds
         * the new value, finds no difference, and never raises the event. Nobody
         * wakes anybody. Snapshotting from `ls` makes a later store leave BOTH
         * stale, which is exactly what makes the lost-reservation event fire. */
        memcpy(ctx->resv_line, ls, MFC_ATOMIC_LINE);   /* snapshot for compare */
        /* MFC_RdAtomicStat after a GETLLAR reads 4 (GETLLAR complete), not 0:
         * Havok's SPU allocator (GH3 collide task, LS 0xA66C) re-issues the
         * GETLLAR until that bit is set, and spun forever on 0. PUTLLUC is 2. */
        ctx->resv_ea = ea; ctx->resv_valid = 1; ctx->atomic_stat = 4;
        ctx->dbg_getllar++;
        racacit_ring_note('g', ctx->spu_id, (uint32_t)ctx->pc & SPU_LS_MASK, ea, 1);
        /* A GETLLAR re-issued millions of times on one line is an SPU spin-wait
         * that nobody is releasing (a ticket lock, a counter barrier). Say so
         * once per line with the line's head, so the stuck word is visible
         * without a per-function probe. */
        { static SPU_THREAD_LOCAL uint32_t t_ea; static SPU_THREAD_LOCAL unsigned long t_n;
          if (ea != t_ea) { t_ea = ea; t_n = 0; }
          else if (++t_n == 4000000) {
              const uint8_t* m = vm_base + ea;
              fprintf(stderr, "[spu-spin] img=%d pc=0x%05X lr=0x%05X GETLLAR ea=0x%08X x4M; line:"
                              " %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                      ctx->image_id, (unsigned)(ctx->pc & SPU_LS_MASK), ctx->gpr[0]._u32[0] & SPU_LS_MASK, ea,
                      m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15]);
          } }
        spu_lockline_unlock();
        spu_getllar_idle_backoff(ctx, ea, ls);
        /* SPU_LLARWATCH=<hex EA>: every GETLLAR of that line, with the LSA it
         * used and the first word as it lands in BOTH places.
         *
         * The deadlock this settles: at a freeze the reservation snapshot holds
         * the fresh produced value (0xC9) while the SPU's local-store mirror
         * still holds the stale one (0xC8) -- and the compare reads the mirror.
         * Both memcpys above copy from the same `mem`, so that can only happen
         * if `lsa` was not where the SPU asked. Reading ctx->mfc_lsa from a
         * later poll cannot prove it (by then the SPU has issued other DMA and
         * overwritten the field, which is exactly the compare-two-moments
         * mistake that has cost this port ten retractions). So log it HERE, at
         * the GETLLAR, with the word that actually landed. */
        { static int s_lw2 = -2; static uint32_t s_lwea;
          if (s_lw2 == -2) { const char* e6 = getenv("SPU_LLARWATCH");
                             s_lw2 = e6 ? 1 : 0;
                             s_lwea = e6 ? (uint32_t)strtoul(e6, 0, 16) & ~127u : 0u; }
          if (s_lw2 && (ea & ~127u) == s_lwea) {
              static unsigned long ln;
              /* A histogram of the LSAs this line's GETLLARs use, not a
               * sample of them. resv_line has exactly one writer -- this
               * GETLLAR -- and it copies to &ls[mfc_lsa] from the same source,
               * so a snapshot that is fresh while LS 0x10800 is stale can only
               * mean some GETLLAR ran with a DIFFERENT mfc_lsa. Sampling every
               * 256th hit would miss exactly those. Count them all and print
               * every LSA seen. */
              { static uint32_t seen[8]; static unsigned long cnt[8]; static int ns;
                int f = -1;
                for (int z = 0; z < ns; z++) if (seen[z] == lsa) { f = z; break; }
                if (f < 0 && ns < 8) { f = ns; seen[ns] = lsa; cnt[ns] = 0; ns++; }
                if (f >= 0) cnt[f]++;
                if ((ln % 4096) == 0) {
                    fprintf(stderr, "[llar-lsa] %lu GETLLARs on 0x%08X;", ln, ea);
                    for (int z = 0; z < ns; z++)
                        fprintf(stderr, " lsa=0x%05X x%lu", seen[z], cnt[z]);
                    fprintf(stderr, "\n"); fflush(stderr);
                } }
              if (++ln <= 8 || (ln % 256) == 0) {
                  const uint8_t* q = (const uint8_t*)ls;
                  const uint32_t landed = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16)
                                        | ((uint32_t)q[2] << 8) | q[3];
                  const uint8_t* m8 = (const uint8_t*)mem;
                  const uint32_t frommem = ((uint32_t)m8[0] << 24) | ((uint32_t)m8[1] << 16)
                                         | ((uint32_t)m8[2] << 8) | m8[3];
                  const uint8_t* mir = ctx->ls + 0x10800u;
                  const uint32_t mirw = ((uint32_t)mir[0] << 24) | ((uint32_t)mir[1] << 16)
                                      | ((uint32_t)mir[2] << 8) | mir[3];
                  fprintf(stderr, "[llar] n=%lu spu%u pc=0x%05X lsa=0x%05X"
                                  " ea=0x%08X mem=0x%08X landed=0x%08X"
                                  " mirror[0x10800]=0x%08X r90=0x%08X\n",
                          ln, (unsigned)(ctx->spu_id & 7u),
                          (uint32_t)ctx->pc & SPU_LS_MASK, lsa, ea,
                          frommem, landed, mirw, ctx->gpr[90]._u32[0]);
                  fflush(stderr);
              }
          } }
        return 1;

    case MFC_PUTLLC_CMD:
        { extern uint32_t g_barrier_sync_watch;
          uint32_t b = g_barrier_sync_watch;
          if (b && (ea & ~127u) == (b & ~127u)) g_spu_putllc_sync_hit++; }
        spu_lockline_lock();
        if (ctx->resv_valid && ctx->resv_ea == ea &&
            spu_mem_ne(mem, ctx->resv_line, MFC_ATOMIC_LINE) == 0) {
            { extern void spu_lockguard_check(spu_context*, uint32_t, const uint8_t*, uint32_t, const char*);
              spu_lockguard_check(ctx, ea, ls, MFC_ATOMIC_LINE, "PUTLLC"); }
            /* Commit only the bytes this SPU changed. The bytes it did not
             * change equal the snapshot (the compare just proved it), so
             * writing them changes nothing -- except when a PPU store races
             * in: a PPU store that checked the reservation bitmap just before
             * this SPU's GETLLAR takes the unlocked path, and if it lands
             * between the compare above and this commit, a whole-line memcpy
             * wrote the SPU's stale copy over it. On hardware that PUTLLC
             * would have failed; here the PPU's write was simply lost -- a job
             * submission gone, the main thread waiting forever for a record no
             * SPU would ever see (level 14, 2026-10-19: every lane idle-polling
             * the job queue 0x00F3CF00 while the main thread spun on its
             * record). Writing only the changed bytes leaves such a store
             * intact unless it hit a byte this SPU changed too. */
            spu_putllc_commit_changed(mem, ls, ctx->resv_line);
            spu_linewatch_note(ctx, ea, ls, "PUTLLC");
            /* A committing PUTLLC is a line write like any other, so every
             * PEER reservation on it is lost and its SPU takes SPU_EVENT_LR.
             * Silent, this is the same lost update the PPU half was added to
             * close, with an SPU on the writing side: a peer is never told to
             * re-read, so it goes on polling a line it believes it still owns,
             * and a peer parked on RdEventStat sleeps through the commit it
             * was waiting for.
             *
             * Our own reservation is dropped FIRST so the notify does not
             * raise a self-LR: hardware CONSUMES the reservation on a
             * successful PUTLLC, it does not report it lost. Already under the
             * lock-line lock, which is what spu_coh_notify_write expects. */
            ctx->resv_valid = 0;
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 100000) == 1)
                      /* ...and the first four words of the line being written.
                       * A succeeding PUTLLC that repeats millions of times is a
                       * livelock, and the only useful question is what the SPU
                       * keeps seeing there -- the count alone cannot answer it. */
                      { uint32_t w[8] = {0,0,0,0,0,0,0,0};
                        for (int _i = 0; _i < 8; _i++) {
                            uint32_t t = 0;
                            memcpy(&t, (const unsigned char*)ctx->ls + ((lsa + _i*4) & SPU_LS_MASK), 4);
                            w[_i] = __builtin_bswap32(t);
                        }
                        fprintf(stderr, "[putllc-ok] %llu: img=%d ctx=%p ea=0x%08X"
                                        " line=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                                n, ctx->image_id, (void*)ctx, ea,
                                w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]); } } }
            spu_coh_notify_write(ea);
            ctx->atomic_stat = 0;                      /* PUTLLC_SUCCESS */
        } else {
            ctx->atomic_stat = 1;                      /* PUTLLC_FAILURE -> retry */
            /* SPU_PUTLLC_WHY=1: which of the three conditions failed.
             * A PUTLLC retry loop that never succeeds is a livelock, and
             * "failed" alone cannot distinguish a lost reservation from a line
             * another agent is rewriting -- opposite causes, opposite fixes. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) {
                  static unsigned long long n_noresv, n_ea, n_line;
                  if (!ctx->resv_valid)            n_noresv++;
                  else if (ctx->resv_ea != ea)     n_ea++;
                  else                             n_line++;
                  static unsigned long long total;
                  if ((++total % 500000) == 0) {
                      fprintf(stderr, "[putllc-why] %llu fails: no-reservation=%llu"
                                      " ea-mismatch=%llu line-changed=%llu"
                                      " (last ea=0x%08X img=%d resv_ea=0x%08X"
                                      " getllar=%llu)\n",
                              total, n_noresv, n_ea, n_line, ea, ctx->image_id,
                              ctx->resv_ea, (unsigned long long)ctx->dbg_getllar);
                      fflush(stderr);
                  }
              } }
        }
        { extern uint32_t g_barrier_sync_watch;
          uint32_t b = g_barrier_sync_watch;
          if (b && ea >= (b & ~127u) && ea < ((b + 0xC0 + 127) & ~127u)) {
              static int _n = 0;
              if ((_n < 96 && (_n++, 1)) && ctx->atomic_stat == 0) {
                  uint32_t sn = ((uint32_t)ctx->ls[0x1C8]<<24)|((uint32_t)ctx->ls[0x1C9]<<16)|
                                ((uint32_t)ctx->ls[0x1CA]<<8)|ctx->ls[0x1CB];
                  extern uint8_t* vm_base;
                  const uint8_t* r = vm_base + b + 0x70;   /* row 3 (sync+0x40+16*3) */
                  fprintf(stderr, "[sync-atomic] PUTLLC-OK spuNum=%u ea=+0x%X lanes@row3={%u %u %u %u %u %u %u}\n",
                          sn, ea - b,
                          (r[2]<<8)|r[3],(r[4]<<8)|r[5],(r[6]<<8)|r[7],(r[8]<<8)|r[9],
                          (r[10]<<8)|r[11],(r[12]<<8)|r[13],(r[14]<<8)|r[15]);
              }
          } }
        /* SPU_ATOM_EA: the [atom-ea] line above prints every ATTEMPT, before
         * the compare. Say which of them committed, or a retried failure reads
         * as a lost update. */
        { static int s_v = -1; static uint32_t s_vl;
          if (s_v < 0) { const char* e = getenv("SPU_ATOM_EA");
                         s_vl = e ? (uint32_t)strtoul(e, 0, 16) & ~127u : 0; s_v = s_vl ? 1 : 0; }
          if (s_v && (ea & ~127u) == s_vl)
              fprintf(stderr, "[atom-ea] PUTLLC %s lr=0x%05X ea=0x%08X\n",
                      ctx->atomic_stat ? "FAIL" : "OK",
                      ctx->gpr[0]._u32[0] & SPU_LS_MASK, ea); }
        racacit_ring_note(ctx->atomic_stat ? 'p' : 'P', ctx->spu_id, (uint32_t)ctx->pc & SPU_LS_MASK, ea, 0);
        ctx->resv_valid = 0;                           /* reservation consumed */
        spu_lockline_unlock();
        if (ctx->atomic_stat == 0 && g_spu_line_commit_hook) g_spu_line_commit_hook(ea & ~127u);
        return 1;

    case MFC_PUTLLUC_CMD:
    case MFC_PUTQLLUC_CMD:
        spu_lockline_lock();
        { extern void spu_lockguard_check(spu_context*, uint32_t, const uint8_t*, uint32_t, const char*);
          spu_lockguard_check(ctx, ea, ls, MFC_ATOMIC_LINE, "PUTLLUC"); }
        memcpy(mem, ls, MFC_ATOMIC_LINE);              /* unconditional store */
        spu_linewatch_note(ctx, ea, ls, "PUTLLUC");
        /* Unconditional, so it invalidates EVERY reservation on the line --
         * this SPU's included, which is why the notify runs before the
         * bookkeeping below rather than after it. A peer left holding a
         * reservation here would commit a PUTLLC against a snapshot this store
         * has already overwritten. */
        spu_coh_notify_write(ea);
        ctx->resv_valid = 0; ctx->atomic_stat = 2;   /* PUTLLUC complete */
        racacit_ring_note('U', ctx->spu_id, (uint32_t)ctx->pc & SPU_LS_MASK, ea, 0);
        spu_lockline_unlock();
        if (g_spu_line_commit_hook) g_spu_line_commit_hook(ea & ~127u);
        return 1;

    default:
        return 0;
    }
}

static int channel_is_mfc(uint32_t ch)
{
    switch (ch) {
    case MFC_WrMSSyncReq: case MFC_RdTagMask:  case MFC_LSA:
    case MFC_EAH:         case MFC_EAL:         case MFC_Size:
    case MFC_TagID:       case MFC_Cmd:         case MFC_WrTagMask:
    case MFC_WrTagUpdate: case MFC_RdTagStat:   case MFC_RdListStallStat:
    case MFC_WrListStallAck: case MFC_RdAtomicStat:
        return 1;
    default:
        return 0;
    }
}

/* ===========================================================================
 * Channel write
 * ===========================================================================*/
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value)
{
    /* SPU_CHHIST=1: which channels a job actually touches, and how often. A
     * worker that never issues an MFC command is either not being handed its
     * work descriptor or is waiting on a channel we never satisfy; the channel
     * mix distinguishes those. */
    { static int s_c = -1; if (s_c < 0) { const char* e = getenv("SPU_CHHIST"); s_c = e ? (atoi(e) > 0 ? atoi(e) : 2000) : 0; }
      if (s_c) { static unsigned long long w[128]; static unsigned long long n;
          w[channel & 127]++;
          if ((++n % (unsigned long long)s_c) == 0) { fprintf(stderr, "[chw] %llu writes:%c", n, 10);
              for (int i = 0; i < 128; i++) if (w[i])
                  fprintf(stderr, "   wrch ch%-3d %llu%c", i, w[i], 10); } } }
    uint32_t v = value._u32[0];  /* channel writes use the preferred slot */

    if (channel_is_mfc(channel)) {
        /* Atomic line ops (GETLLAR/PUTLLC/...) need real reservation semantics,
         * not the plain GET/PUT the DMA engine would do. */
        if (channel == MFC_Cmd && spu_mfc_atomic(ctx, v))
            return;
        mfc_channel_write(mfc_for(ctx), ctx, channel, v);
        /* MFC tag-status-update EVENT producer (channel-stall milestone). Our
         * DMA completes synchronously, so the instant the program arms tag
         * notification (MFC_WrTagUpdate with a non-zero mode = any/all), the
         * tags in MFC_WrTagMask are already complete -- pend the tag event
         * (bit 0, MFC_TAG_STATUS_UPDATE_EVENT) when the SPU has it enabled, and
         * wake any host thread blocked in rdch SPU_RdEventStat. This is the
         * long-missing producer for event_status (previously only ever cleared
         * via WrEventAck -> RdEventStat waits could never complete). */
        if (channel == MFC_WrTagUpdate && v != 0 && (ctx->event_mask & 0x1u)) {
            ctx->event_status |= 0x1u;
            spu_ch_wake(ctx);
        }
        return;
    }

    switch (channel) {
    case SPU_WrOutMbox:
        /* A raw SPU's outbound mailbox is one entry deep and wrch stalls while
         * it is full. Buffering instead let two replies queue up, and a PPU
         * that tests bit 0 of the count -- Tornado Outbreak's boot handshake,
         * func_000D5A10 -- read count 2 as empty and spun forever (an
         * intermittent boot hang). Raw SPUs only: their reader is the PPU
         * polling the MMIO window, whereas SPURS-thread mailboxes are drained
         * by paths that do not wake a stalled writer. A raw SPU's local store
         * is the guest window, not its own buffer. */
        if (ctx->ls != ctx->ls_store)
            while (ctx->ch_out_mbox.count >= SPU_MBOX_DEPTH && ctx->status != SPU_STATUS_STOPPED)
                Sleep(0);
        spu_channel_write(&ctx->ch_out_mbox, v);
        /* SPU_DBG_MBOX=1: depth after the write. The image-1 SPUs publish their
         * LS work buffer here (LS 0x11D4) and then poll it, and the guest sets
         * class-2 mask 0x3 -- which excludes the plain-mailbox bit 0x10 -- so
         * this write raises no interrupt and the PPU must POLL to see it. If the
         * depth climbs and stays, nobody is polling and the message is stranded.
         * That is the difference between "the PPU is slow" and "the PPU never
         * looks", which no other probe here distinguishes. */
        { static int _d = -1; if (_d < 0) _d = getenv("SPU_DBG_MBOX") ? 1 : 0;
          if (_d) { static unsigned long _n = 0; if (++_n <= 24)
            fprintf(stderr, "[spu-outmbox] spu=%X wrote 0x%08X depth=%u\n",
                    ctx->spu_id, v, (unsigned)ctx->ch_out_mbox.count); } }
        { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_MBOXTRACE") ? 1 : 0;
          if (s_t) fprintf(stderr, "[spu-mbox] OUT  grp=0x%X spu=0x%X val=0x%08X\n",
                           ctx->spu_group_id, ctx->spu_id, v); }
        /* Plain mailbox data is consumed by the following interrupt request. */
        break;
    case SPU_WrOutIntrMbox:
        { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_MBOXTRACE") ? 1 : 0;
          if (s_t) fprintf(stderr, "[spu-mbox] INTR grp=0x%X spu=0x%X val=0x%08X\n",
                           ctx->spu_group_id, ctx->spu_id, v); }
        if (g_spu_user_event_hook && g_spu_user_event_hook(ctx, v)) break;
        spu_channel_write(&ctx->ch_out_intr_mbox, v);
        if (g_spu_out_mbox_hook) g_spu_out_mbox_hook(ctx->spu_group_id, ctx->spu_id, 1, v);
        break;
    case SPU_WrDec:          ctx->decrementer = v;
                             ctx->dec_base_ns = spu_host_ns();              break;
    case SPU_WrEventMask:    ctx->event_mask = v;                           break; /* WrEventMask */
    case SPU_WrEventAck:
        /* Under the lock-line lock, because every producer of the LR bit sets
         * it under that lock: the PPU coherent store, and a peer SPU's PUTLLC,
         * PUTLLUC or plain PUT. A bare read-modify-write here can read
         * event_status, have a concurrent |= SPU_EVENT_LR land in between, and
         * write the stale value back. The edge is then gone, and an SPU that
         * has just acknowledged the events it read goes back to sleep believing
         * it still holds a reservation it has already lost, which is the parked
         * SPURS kernel this whole mechanism exists to wake. */
        spu_lockline_lock();
        ctx->event_status &= ~v;
        spu_lockline_unlock();
        break;
    case SPU_WrSRR0:         ctx->srr0 = v;                                 break;
    default:
        /* Unknown / unhandled channel write -- ignore (matches a no-op SPU). */
        break;
    }
}

/* ===========================================================================
 * Channel-stall contract (faithful-adopt, from canersaka's fork).
 *
 * A blocking spu_rdch on an empty read channel parks the SPU host thread on a
 * per-SPU CV until a producer (mailbox/signal write, event raise) calls
 * spu_ch_wake -- never fabricating a value. Under SPU_LOCKSTEP the wait
 * releases the run token first (block_begin) so a peer SPU that must post the
 * awaited data is never blocked on this ctx. A 10 ms re-poll is the missed-wake
 * safety net. Opt-in (env SPU_CH_BLOCK=1), default OFF while the producers
 * (mailbox/signal writes, MFC tag-status event raise) are being wired -- until
 * then blocking a read whose producer is missing would hang, so default stays
 * legacy non-blocking with zero regression, exactly like the lockstep gate.
 * ===========================================================================*/
/* Set by runtime/spu/spu_raw.c once a raw SPU exists. A raw SPU runs on its own
 * host thread against a PPU that pokes its mailboxes by MMIO, so an empty
 * RdInMbox must PARK, not fabricate a zero -- there is no scheduler above it to
 * retry. Off unless a raw SPU is actually created, so no other port changes. */
int g_spu_force_ch_block = 0;

static int yz_ch_block(void)
{
    static int v = -1;
    if (v < 0) v = getenv("SPU_CH_BLOCK") ? 1 : 0;
    return v || g_spu_force_ch_block;
}

/* Would rdch complete right now? Plain reads + the 10 ms re-poll cover cross-
 * thread visibility (x86 TSO + the wait syscall's barrier); the s43 atomics are
 * a later refinement. */
/* MFC_LLR_LOST_EVENT (0x400) -- the only producer for it.
 *
 * The SPU "wait until another processor touches this variable" idiom is: GETLLAR
 * the 128-byte line, set the event mask to Lr, then block in
 * `rdch SPU_RdEventStat` until the reservation is lost. Nothing ever raised that
 * bit, so an SPU using it waited forever -- ps1_netemu's GPU core (SPU 4) parks
 * at pc=0x0A5E8 with evmask=0x400 exactly here while the PPU spins waiting on the
 * SPU. Deadlock, and the reason the PS1 core never starts.
 *
 * Losing a reservation means the reserved line changed, and GETLLAR already
 * snapshots it, so compare against that snapshot rather than hooking every store.
 * ponytail: polled, not store-hooked -- one 128-byte memcmp per 10 ms poll on an
 * already-blocked SPU, and nothing at all on the PPU store path. The ceiling is
 * one poll of wake latency, and an A-B-A write that restores the same bytes
 * between polls is missed; hook the writers if either ever matters.
 */
static void spu_resv_lost_poll(spu_context* ctx)
{
    if (!(ctx->event_mask & 0x400u) || !ctx->resv_valid) return;
    if (!vm_base || ctx->resv_ea == 0) return;
    if (spu_mem_ne(vm_base + ctx->resv_ea, ctx->resv_line, 128) != 0) {
        ctx->resv_valid = 0;
        ctx->event_status |= 0x400u;
    }
}

static int spu_ch_ready(spu_context* ctx, uint32_t channel)
{
    if (channel == SPU_RdEventStat) spu_resv_lost_poll(ctx);
    switch (channel) {
    case SPU_RdInMbox:      return ctx->rcv_evt_n != 0 || ctx->ch_in_mbox.count != 0;
    case SPU_RdSigNotify1:  return ctx->ch_sig_notify[0].count != 0;
    case SPU_RdSigNotify2:  return ctx->ch_sig_notify[1].count != 0;
    case SPU_RdEventStat:   return (ctx->event_status & ctx->event_mask) != 0;
    default:                return 1;   /* non-blocking channels: always ready */
    }
}

/* Signal the per-SPU wait CV so a blocked spu_rdch re-checks its predicate.
 * MUST be called by every producer AFTER it makes a read predicate true. */
void spu_ch_wake(spu_context* ctx)
{
    if (!ctx) return;
    WakeAllConditionVariable((CONDITION_VARIABLE*)&ctx->ch_wait_cv);
}

/* Block the calling SPU host thread until `channel` is readable. */
static void spu_ch_wait(spu_context* ctx, uint32_t channel, const char* op)
{
    if (!yz_ch_block() || spu_ch_ready(ctx, channel)) return;

    { static unsigned long bn = 0; unsigned long n = ++bn;
      if (n <= 50 || (n % 512) == 0)
        fprintf(stderr, "[ch-block] spu=%X pc=0x%05X op=%s ch=%u evstat=0x%X evmask=0x%X\n",
                ctx->spu_id, ctx->pc & SPU_LS_MASK, op, channel,
                ctx->event_status, ctx->event_mask); }

    ctx->status = SPU_STATUS_WAITING_CHANNEL;
    yz_lockstep_block_begin(ctx);          /* release the run token before the OS wait */
    {
        unsigned long long start = GetTickCount64(), next_hb = 2000;
        while (!spu_ch_ready(ctx, channel)) {
            AcquireSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
            if (!spu_ch_ready(ctx, channel))
                SleepConditionVariableSRW((CONDITION_VARIABLE*)&ctx->ch_wait_cv,
                                          (SRWLOCK*)&ctx->ch_wait_lock, 10, 0);
            ReleaseSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
            unsigned long long waited = GetTickCount64() - start;
            if (waited >= next_hb) {
                fprintf(stderr, "[ch-wait] spu=%X pc=0x%05X ch=%u waited=%llums evstat=0x%X evmask=0x%X resv[valid=%d ea=0x%08X]\n",
                        ctx->spu_id, ctx->pc & SPU_LS_MASK, channel, waited,
                        ctx->event_status, ctx->event_mask,
                        ctx->resv_valid, ctx->resv_ea);
                fflush(stderr);
                next_hb = ((waited / 2000) + 1) * 2000;
            }
        }
    }
    yz_lockstep_block_end(ctx);            /* rejoin the rotation, reacquire the token */
    ctx->status = SPU_STATUS_RUNNING;
}

/* `rdch SPU_RdEventStat` with only the lock-line-lost event pending on a
 * held reservation: the "sleep until someone writes this line" idiom. Without
 * SPU_CH_BLOCK the read returned at once, so an SPURS job-chain lane with
 * nothing to do went straight back to GETLLAR -- five lanes doing that
 * through the one lock-line lock took a third of every lane's time and
 * slowed the lanes that had work (A Crack in Time's job manager, LS 0x270).
 *
 * Wait here instead, bounded: a short spin on the line itself (no lock),
 * then the channel CV, which every writer that kills a reservation signals
 * (spu_coh_notify_write*). Returning without the event after the bound is
 * what the read did before, so the guest's retry loop is unchanged; only its
 * rate drops. SPU_LR_WAIT_US=<us> (default 2000; 0 = off). */
/* SPU_LR_WAITLOG=1: total time spent in lock-line waits per (line, image, pc),
 * the top 10 every 5 s -- which line a slow job sits on. */
static void spu_lr_waitlog(spu_context* ctx, unsigned long long us)
{
    static int on = -1; if (on < 0) on = getenv("SPU_LR_WAITLOG") ? 1 : 0;
    if (!on) return;
    typedef struct { uint32_t ea, pc; int img; unsigned long long us, n; uint32_t mask; } ent;
    static ent e[256]; static int ne; static SRWLOCK lk = SRWLOCK_INIT; static unsigned long long last;
    extern unsigned long long ps3_qpc_us(void);
    const uint32_t ea = ctx->resv_ea & ~127u, pc = (uint32_t)(ctx->pc & SPU_LS_MASK);
    AcquireSRWLockExclusive(&lk);
    int k = 0;
    for (; k < ne; k++) if (e[k].ea == ea && e[k].pc == pc && e[k].img == ctx->image_id) break;
    if (k == ne && ne < 256) { e[ne].ea = ea; e[ne].pc = pc; e[ne].img = ctx->image_id; e[ne].us = e[ne].n = 0; e[ne].mask = ctx->event_mask; ne++; }
    if (k < ne) { e[k].us += us; e[k].n++; }
    const unsigned long long now = ps3_qpc_us();
    if (!last) last = now;
    if (now - last > 5000000ull) {
        last = now;
        fprintf(stderr, "[lr-waitlog] 5 s:%c", 10);
        for (int t = 0; t < 10; t++) {
            int b = -1;
            for (int j = 0; j < ne; j++) if (e[j].n && (b < 0 || e[j].us > e[b].us)) b = j;
            if (b < 0) break;
            fprintf(stderr, "   line %08X img %d pc 0x%05X mask 0x%X: %llu waits, %.1f ms total, %.0f us avg%c",
                    e[b].ea, e[b].img, e[b].pc, e[b].mask, e[b].n, e[b].us / 1e3, (double)e[b].us / (double)e[b].n, 10);
            e[b].n = 0;
        }
        ne = 0;
    }
    ReleaseSRWLockExclusive(&lk);
}

/* The lanes' spin before sleeping (us), live from Settings > Graphics > CPU
 * usage (rsx_host_ui.c): 30 efficient, 200 maximum. */
volatile int g_racacit_spu_spin_us = 30;
static void spu_lr_wait(spu_context* ctx)
{
    static long s_us = -1;
    /* 200 us: a SPURS kernel parked here re-polls its workloads at least
     * that often. A Crack in Time posts job-chain work through stores that
     * reach the line without a lock-line notify; at the old 2 ms bound the
     * job sat that long before an SPU picked it up, while the main thread
     * spun on its completion (Zolar: 86% -> 93% of frames on time). */
    if (s_us < 0) { const char* e = getenv("SPU_LR_WAIT_US"); s_us = e ? atol(e) : 200; }
    if (s_us <= 0 || !(ctx->event_mask & 0x400u)) return;
    if ((ctx->event_status & ctx->event_mask) || !ctx->resv_valid) return;
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    /* SPU_LR_SPIN_US=<us>: how long to spin on the line before sleeping on
     * the channel CV (woken by any writer that kills the reservation). It was
     * 200, the whole bound: a parked lane never slept, and the six lanes held
     * six cores at 100% doing nothing -- the game took ~8 cores standing in
     * Axiom City, starving the main thread on 4- and 6-core CPUs. At 30 the
     * hand-offs still land in the spin (fight frame rate and frame waits
     * unchanged, measured A/B) and Axiom takes 3.6-4.4 cores standing, ~5 in
     * a fight. 0 sleeps at once and slows fights. */
    static long s_spin_us = -2;
    if (s_spin_us == -2) { const char* e = getenv("SPU_LR_SPIN_US"); s_spin_us = e ? atol(e) : -1; }
    const long spin_us = s_spin_us >= 0 ? s_spin_us : g_racacit_spu_spin_us;   /* Settings > Graphics > CPU usage */
    const LONGLONG spin = f.QuadPart * spin_us / 1000000;
    const LONGLONG lim = f.QuadPart * s_us / 1000000;
    extern void ps3_tl_at(unsigned long long t, char ev, uint32_t a, uint32_t b);
    extern unsigned long long ps3_qpc_us(void);
    const unsigned long long tl0 = ps3_qpc_us();
extern unsigned ps3_tl_tix(void);
extern _Thread_local unsigned long long t_spu_job_wait_us;   /* spu_drain.c, SPU_JOB_STATS */
#define SPU_LR_WAIT_DONE() do { const unsigned long long d_ = ps3_qpc_us() - tl0; t_spu_job_wait_us += d_;         if (d_ >= 20) { ps3_tl_at(tl0, 'w', (ctx->resv_ea & ~127u) | (ps3_tl_tix() & 127u), (uint32_t)d_); ps3_tl_at(tl0, 'x', (uint32_t)(ctx->pc & SPU_LS_MASK), (uint32_t)d_); }         spu_lr_waitlog(ctx, d_); } while (0)
    /* SPU_LR_STATS=1: how the waits end, every 5 s -- in the spin, woken by a
     * writer's notify, found changed only by polling after a sleep (a writer
     * that does not notify: up to 1 ms late), or at the bound. */
    static int s_st = -1; if (s_st < 0) s_st = getenv("SPU_LR_STATS") ? 1 : 0;
    static volatile long s_n[4]; static unsigned long long s_last;
    int slept = 0;
#define SPU_LR_STAT(k) do { if (s_st) { InterlockedIncrement(&s_n[k]); const unsigned long long n_ = ps3_qpc_us();         if (!s_last) s_last = n_; else if (n_ - s_last > 5000000ull) { s_last = n_;         fprintf(stderr, "[lr-wait] spin %ld notified %ld polled-late %ld bound %ld%c", s_n[0], s_n[1], s_n[2], s_n[3], 10);         s_n[0] = s_n[1] = s_n[2] = s_n[3] = 0; } } } while (0)
    for (;;) {
        if ((ctx->event_status & ctx->event_mask) || !ctx->resv_valid) {
            SPU_LR_STAT(slept ? 1 : 0); SPU_LR_WAIT_DONE(); return; }
        spu_resv_lost_poll(ctx);
        if ((ctx->event_status & ctx->event_mask) || !ctx->resv_valid) {
            SPU_LR_STAT(slept ? 2 : 0); SPU_LR_WAIT_DONE(); return; }
        QueryPerformanceCounter(&t);
        if (t.QuadPart - t0.QuadPart >= lim) {
            SPU_LR_STAT(3); SPU_LR_WAIT_DONE();
            /* SPU_LR_STATS: which lines the waits that time out are on. */
            if (s_st) { static uint32_t ea_[16]; static unsigned long cnt_[16]; static unsigned long tot_;
                const uint32_t ea = ctx->resv_ea & ~127u; int k = 0;
                for (; k < 16; k++) if (ea_[k] == ea || !cnt_[k]) break;
                if (k < 16) { ea_[k] = ea; cnt_[k]++; }
                if ((++tot_ & 1023u) == 0) {
                    fprintf(stderr, "[lr-bound] lines:");
                    for (int j = 0; j < 16 && cnt_[j]; j++) fprintf(stderr, " %08X:%lu", ea_[j], cnt_[j]);
                    fprintf(stderr, " (img %d pc 0x%05X)%c", ctx->image_id, (unsigned)(ctx->pc & SPU_LS_MASK), 10);
                } }
            return; }
        if (t.QuadPart - t0.QuadPart < spin) { YieldProcessor(); continue; }
        yz_lockstep_block_begin(ctx);
        AcquireSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
        if (!(ctx->event_status & ctx->event_mask) && ctx->resv_valid)
            SleepConditionVariableSRW((CONDITION_VARIABLE*)&ctx->ch_wait_cv,
                                      (SRWLOCK*)&ctx->ch_wait_lock, 1, 0);
        ReleaseSRWLockExclusive((SRWLOCK*)&ctx->ch_wait_lock);
        yz_lockstep_block_end(ctx);
        slept = 1;
    }
}

/* ===========================================================================
 * Channel read (returns value in the preferred word slot)
 * ===========================================================================*/
u128 spu_rdch(spu_context* ctx, uint32_t channel)
{
    { static int s_c = -1; if (s_c < 0) { const char* e = getenv("SPU_CHHIST"); s_c = e ? (atoi(e) > 0 ? atoi(e) : 2000) : 0; }
      if (s_c) { static unsigned long long r[128]; static unsigned long long n;
          r[channel & 127]++;
          if ((++n % (unsigned long long)s_c) == 0) { fprintf(stderr, "[chr] %llu reads:%c", n, 10);
              for (int i = 0; i < 128; i++) if (r[i])
                  fprintf(stderr, "   rdch ch%-3d %llu%c", i, r[i], 10); } } }
    /* Block (never fabricate) on an empty producer-fed read channel (opt-in
     * SPU_CH_BLOCK). RdEventStat now has a producer (the MFC tag-status event
     * raise above), but only block it when the SPU has actually enabled events
     * (event_mask != 0) -- a masked-off read must return 0 immediately, not
     * park forever. RdInMbox/RdSigNotify park on their PPU producers. */
    if (channel == SPU_RdInMbox || channel == SPU_RdSigNotify1 || channel == SPU_RdSigNotify2)
        spu_ch_wait(ctx, channel, "rdch");
    else if (channel == SPU_RdEventStat && ctx->event_mask != 0) {
        if (yz_ch_block()) spu_ch_wait(ctx, channel, "rdch");
        else spu_lr_wait(ctx);
    }

    uint32_t v = 0;

    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_c[10] = {0}; static uint64_t s_tot = 0;
        int b = (channel==SPU_RdInMbox)?0:(channel==SPU_RdSigNotify1)?1:(channel==SPU_RdSigNotify2)?2:
                (channel==SPU_RdDec)?3:(channel==SPU_RdEventStat)?4:(channel==SPU_RdEventMask)?5:
                (channel==MFC_RdTagStat)?6:(channel==MFC_RdAtomicStat)?7:(channel==SPU_RdMachStat)?8:9;
        s_c[b]++;
        if ((++s_tot % 2000000) == 0)
          fprintf(stderr, "[rdch] InMbox=%llu Sig1=%llu Sig2=%llu Dec=%llu EvStat=%llu EvMask=%llu TagStat=%llu AtomStat=%llu MachStat=%llu other=%llu\n",
            (unsigned long long)s_c[0],(unsigned long long)s_c[1],(unsigned long long)s_c[2],(unsigned long long)s_c[3],
            (unsigned long long)s_c[4],(unsigned long long)s_c[5],(unsigned long long)s_c[6],(unsigned long long)s_c[7],
            (unsigned long long)s_c[8],(unsigned long long)s_c[9]); } }

    if (channel_is_mfc(channel)) {
        v = mfc_channel_read(mfc_for(ctx), ctx, channel);
        return spu_make_preferred_u32(v);
    }

    switch (channel) {
    case SPU_RdInMbox:
        if (ctx->rcv_evt_n > 0) { v = ctx->rcv_evt[ctx->rcv_evt_i++]; ctx->rcv_evt_n--; }
        else v = spu_channel_read(&ctx->ch_in_mbox);
        break;
    case SPU_RdSigNotify1:  v = spu_channel_read(&ctx->ch_sig_notify[0]); break;
    case SPU_RdSigNotify2:  v = spu_channel_read(&ctx->ch_sig_notify[1]); break;
    /* The decrementer must actually DECREMENT. Returning the latched WrDec
     * value verbatim makes every `rdch $ch8` deadline poll spin forever: the
     * elapsed-time term is always zero, so the deadline never passes. LBP's
     * wwsjob SPURS policy module hangs on exactly that at LS 0x2D68:
     *
     *     2d6c:  lqa   $2, 0x1530     ; deadline
     *     2d74:  rdch  $5, $ch8       ; now  <- always read 0
     *     2d78:  sf    $6, $2, $5     ; now - deadline
     *     2d80:  cgti  $5, $6, 0
     *     2d88:  binz  $2, $0         ; leave once it goes positive
     *
     * It never leaves, never reaches its first DMA, and blocks the workload.
     * Wraparound on subtract is deliberate -- it is what hardware does, and the
     * `cgti > 0` test above is written to tolerate it. */
    case SPU_RdDec: {
        /* Never armed by a WrDec: stamp the base on first read rather than
         * measuring from epoch 0, which would subtract the host's entire
         * uptime and hand back a wild value to code that has every right to
         * read the decrementer before writing it. */
        if (!ctx->dec_base_ns) ctx->dec_base_ns = spu_host_ns();
        uint64_t elapsed = spu_host_ns() - ctx->dec_base_ns;
        uint32_t ticks   = (uint32_t)((elapsed * SPU_DECREMENTER_HZ)
                                      / 1000000000ull);
        v = ctx->decrementer - ticks;
        break;
    }
    case SPU_RdEventMask:   v = ctx->event_mask;                        break;
    case SPU_RdEventStat:   spu_resv_lost_poll(ctx); v = ctx->event_status; break;
    case SPU_RdMachStat:    v = (ctx->status == SPU_STATUS_RUNNING) ? 1 : 0; break;
    case SPU_RdSRR0:        v = ctx->srr0;                              break;
    default:
        v = 0;
        break;
    }
    return spu_make_preferred_u32(v);
}

/* ===========================================================================
 * Channel count (rchcnt) -- how many entries can be read/written right now
 * ===========================================================================*/
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel)
{
    /* SPU_WHOPOLLS=<n>: rchcnt/park accounting PER SPU, printed every n calls
     * with the pc each SPU is sitting at.
     *
     * SPU_CHHIST sums every SPU into one histogram, which is exactly the wrong
     * shape for a deadlock: it showed rchcnt ch0/ch4 climbing past 1.7 billion
     * while MFC_WrTagUpdate (ch23) stayed frozen, i.e. somebody polls forever
     * and somebody else stopped doing DMA -- without saying whether that is one
     * SPU or two. The freeze under investigation is the PPU-side spin in
     * func_000D2298 waiting for spu4 to advance a counter at 0x002DF008, so
     * which SPU is parked, and on what, is the whole question. */
    { static int s_w = -1;
      if (s_w < 0) { const char* e = getenv("SPU_WHOPOLLS");
        s_w = e ? (atoi(e) > 0 ? atoi(e) : 4000000) : 0; }
      if (s_w) { static unsigned long long c[8][40]; static unsigned long long n;
          static uint32_t lastpc[8], lastst[8], lastmask[8];
          static uint32_t lastsig[8][2], lastmb[8], lastrea[8];
          static int lastrv[8], lastrd[8];
          static uint32_t lastline[8][8], lastls[8][8];
          static uint32_t lastlsa[8], lastr90[8];
          const unsigned sp = (unsigned)(ctx->spu_id & 7u);
          if (channel < 40u) c[sp][channel]++;
          lastpc[sp] = (uint32_t)ctx->pc & SPU_LS_MASK;
          lastst[sp] = ctx->event_status; lastmask[sp] = ctx->event_mask;
          lastsig[sp][0] = ctx->ch_sig_notify[0].count;
          lastsig[sp][1] = ctx->ch_sig_notify[1].count;
          lastmb[sp] = ctx->ch_in_mbox.count;
          lastrv[sp] = ctx->resv_valid; lastrea[sp] = ctx->resv_ea;
          lastlsa[sp] = ctx->mfc_lsa & SPU_LS_MASK;
          lastr90[sp] = ctx->gpr[90]._u32[0];
          lastrd[sp] = (ctx->resv_valid && vm_base && ctx->resv_ea)
                     ? (spu_mem_ne(vm_base + ctx->resv_ea, ctx->resv_line, 128) != 0)
                     : -1;
          { static int s_ld2 = -2; static uint32_t s_lo2;
            if (s_ld2 == -2) { const char* e5 = getenv("SPU_LSDUMP");
                               s_ld2 = e5 ? 1 : 0;
                               s_lo2 = e5 ? (uint32_t)strtoul(e5,0,16) : 0u; }
            if (s_ld2 && ctx->ls)
                for (int z = 0; z < 8; z++) {
                    const uint8_t* l8 = ctx->ls + ((s_lo2 + z * 4) & SPU_LS_MASK);
                    lastls[sp][z] = ((uint32_t)l8[0] << 24) | ((uint32_t)l8[1] << 16)
                                  | ((uint32_t)l8[2] << 8) | l8[3];
                } }
          for (int z = 0; z < 8; z++) {
              const uint8_t* b8 = ctx->resv_line + z * 4;
              lastline[sp][z] = ((uint32_t)b8[0] << 24) | ((uint32_t)b8[1] << 16)
                              | ((uint32_t)b8[2] << 8) | b8[3];
          }
          if ((++n % (unsigned long long)s_w) == 0) {
              fprintf(stderr, "[whopolls] %llu rchcnt calls\n", n);
              for (unsigned q = 0; q < 8; q++) {
                  int any = 0;
                  for (unsigned k = 0; k < 40; k++) if (c[q][k]) any = 1;
                  if (!any) continue;
                  /* The channel state each poll is actually testing. Counts
                   * alone cannot say whether a poll returns 0 or 1, and that is
                   * the difference between "the SPU is not being told" and "the
                   * SPU is told and ignores it". */
                  fprintf(stderr, "   spu%u pc=0x%05X ev[st=%08X mask=%08X]"
                                  " sig[%u %u] inmbox=%u", q, lastpc[q],
                          lastst[q], lastmask[q], lastsig[q][0], lastsig[q][1],
                          lastmb[q]);
                  /* The reservation is the other half of an evmask=0x400 wait:
                   * spu_resv_lost_poll returns immediately unless resv_valid,
                   * so a cleared reservation is indistinguishable from "nobody
                   * wrote the line" in event_status alone -- and they are
                   * different bugs. diff says whether the line has actually
                   * changed under the snapshot right now. */
                  /* mfc_lsa and r90 alongside the reservation. GETLLAR
                   * copies the line to &ls[mfc_lsa] and to resv_line from the
                   * same source, so a snapshot that disagrees with the local
                   * store can only mean mfc_lsa was not what the SPU asked for
                   * -- and the SPU computes both that LSA and the address its
                   * compare reads from r90. Printing the two together is what
                   * decides it. */
                  fprintf(stderr, " resv[v=%d ea=0x%08X diff=%d]"
                                  " mfc_lsa=0x%05X r90=0x%08X",
                          lastrv[q], lastrea[q], lastrd[q],
                          lastlsa[q], lastr90[q]);
                  /* And the line itself. "The reservation is intact and the
                   * line has not changed" still does not say what the SPU is
                   * waiting FOR; the words do. Snapshot side, so it is exactly
                   * what the SPU last read. */
                  /* SPU_LSDUMP=<hex LS offset>: 8 words of this SPU's local
                   * store. The reserved line says what the SPU can SEE; its LS
                   * says what it BELIEVES. For the spu4 deadlock those are the
                   * two numbers to compare -- its consumed counter is staged at
                   * LS 0x10888 (the counter PUT's lsa 0x10880, +8). */
                  { static int s_ld = -2; static uint32_t s_lo;
                    if (s_ld == -2) { const char* e4 = getenv("SPU_LSDUMP");
                                      s_ld = e4 ? 1 : 0;
                                      s_lo = e4 ? (uint32_t)strtoul(e4,0,16) : 0u; }
                    if (s_ld) {
                        fprintf(stderr, " ls[0x%05X:", s_lo);
                        for (int z = 0; z < 8; z++)
                            fprintf(stderr, " %08X", lastls[q][z]);
                        fprintf(stderr, "]");
                    } }
                  if (lastrv[q]) {
                      fprintf(stderr, " line[");
                      for (int z = 0; z < 8; z++)
                          fprintf(stderr, "%s%08X", z ? " " : "", lastline[q][z]);
                      fprintf(stderr, "]");
                  }
                  for (unsigned k = 0; k < 40; k++) if (c[q][k])
                      fprintf(stderr, " ch%u=%llu", k, c[q][k]);
                  fprintf(stderr, "\n");
              }
              fflush(stderr);
          } } }
    /* SPU_CHHIST also covers rchcnt. It used to instrument only wrch/rdch,
     * which is the one place a parked persistent worker is guaranteed NOT to
     * appear: park_on_empty_inmbox halts from inside THIS function, so a
     * worker that polls rchcnt and parks produced a completely empty channel
     * histogram and read as 'never touches a channel'. */
    { static int s_c = -1;
      if (s_c < 0) { const char* e = getenv("SPU_CHHIST");
        s_c = e ? (atoi(e) > 0 ? atoi(e) : 2000) : 0; }
      if (s_c) { static unsigned long long c[128]; static unsigned long long n;
          c[channel & 127]++;
          if ((++n % (unsigned long long)s_c) == 0) {
              fprintf(stderr, "[chc] %llu rchcnt:%c", n, 10);
              for (int i = 0; i < 128; i++) if (c[i])
                  fprintf(stderr, "   rchcnt ch%-3d %llu%c", i, c[i], 10);
              fflush(stderr); } } }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_cnt[8] = {0}; static uint64_t s_total = 0;
        int b = (channel==SPU_RdInMbox)?0:(channel==SPU_RdEventStat)?1:(channel==SPU_RdSigNotify1)?2:
                (channel==SPU_RdSigNotify2)?3:(channel==MFC_RdTagStat)?4:(channel==SPU_WrOutMbox)?5:
                (channel==SPU_WrOutIntrMbox)?6:7;
        s_cnt[b]++;
        if ((++s_total % 2000000) == 0)
          fprintf(stderr, "[pollcnt] InMbox=%llu EvStat=%llu Sig1=%llu Sig2=%llu TagStat=%llu OutMbox=%llu OutIntr=%llu other=%llu (ch last=%u)\n",
                  (unsigned long long)s_cnt[0],(unsigned long long)s_cnt[1],(unsigned long long)s_cnt[2],
                  (unsigned long long)s_cnt[3],(unsigned long long)s_cnt[4],(unsigned long long)s_cnt[5],
                  (unsigned long long)s_cnt[6],(unsigned long long)s_cnt[7], channel); } }
    switch (channel) {
    case SPU_RdInMbox:
        /* Synchronous persistent-worker park: after its handshake the SPU polls
         * the inbound mailbox for PPU commands; if empty and parking is armed,
         * halt (longjmp out of spu_run_with_halt) instead of spinning forever. */
        /* A pending sys_spu_thread_receive_event reply counts as readable
         * mailbox words: the worker checks rchcnt to decide whether its reply
         * has arrived, and parking on an "empty" inbox that actually holds the
         * reply strands it. */
        if (ctx->park_on_empty_inmbox && ctx->rcv_evt_n == 0 &&
            ctx->ch_in_mbox.count == 0) {
            extern void spu_halt(spu_context*);
            spu_halt(ctx);
        }
        return (uint32_t)ctx->rcv_evt_n + ctx->ch_in_mbox.count;           /* readable */
    /* Free slots, CLAMPED. The queue behind each channel is SPU_CHANNEL_CAP
     * deep -- deeper than the hardware depth reported here -- so `count` can
     * exceed the depth and a bare subtraction underflows, telling the SPU it
     * has four billion free slots. That silently converts a bounded mailbox
     * into an unbounded one for any title that polls before writing, which is
     * every SPU-heavy one. */
    case SPU_WrOutMbox:
        return ctx->ch_out_mbox.count >= SPU_MBOX_DEPTH
             ? 0u : (uint32_t)(SPU_MBOX_DEPTH - ctx->ch_out_mbox.count);
    case SPU_WrOutIntrMbox:
        return ctx->ch_out_intr_mbox.count >= SPU_INTR_MBOX_DEPTH
             ? 0u : (uint32_t)(SPU_INTR_MBOX_DEPTH - ctx->ch_out_intr_mbox.count);
    case SPU_RdSigNotify1:   return ctx->ch_sig_notify[0].count;
    case SPU_RdSigNotify2:   return ctx->ch_sig_notify[1].count;
    case MFC_Cmd:            return MFC_QUEUE_DEPTH - mfc_for(ctx)->queue_count;
    /* An enabled event pending, or 0 -- NOT the `default: 1` this used to fall
     * through to. The SPU idiom is `rchcnt SPU_RdEventStat; brnz -> rdch`: it
     * asks whether an event is pending and only commits to the BLOCKING read if
     * the answer is yes. Answering 1 unconditionally lured it into a read that
     * then parked forever, because rdch correctly blocks while
     * (event_status & event_mask) == 0 -- rchcnt and rdch disagreed.
     *
     * ps1_netemu's audio SPU does exactly this at LS 0x8934 and parked at
     * 0x0A5E8 waiting for MFC_LLR_LOST_EVENT on a line nothing was ever going
     * to touch; on hardware it would simply have fallen through to 0x893C and
     * kept working. Same condition as spu_ch_ready's case, including the
     * lost-reservation poll, so the two now agree by construction. */
    case SPU_RdEventStat:
        spu_resv_lost_poll(ctx);
        return (ctx->event_status & ctx->event_mask) != 0;
    case MFC_RdTagStat:      return 1;  /* synchronous: status always ready */
    /* Stall-and-notify status is a single-value channel: count is 1 while a
     * newly-stalled tag is pending to be read, else 0. The wwsjob interrupt
     * handler gates on this (rchcnt $ch25; brz IhcExit) -- returning the old
     * default of 1 made it enter on spurious interrupts with an empty stat,
     * running IhcLoop on a zero mask (clz(0) -> bogus tagId/ack). */
    case MFC_RdListStallStat:
        { static int s_it = -1; if (s_it < 0) s_it = getenv("SPU_IRQTRACE") ? 1 : 0;
          static int _n = 0;
          if (s_it && (_n < 64 && (_n++, 1)))
              fprintf(stderr, "[irq] rchcnt ListStallStat stat=0x%X img=%d pc=0x%05X\n",
                      ctx->list_stall_stat, ctx->image_id, ctx->pc & 0x3FFFF); }
        return ctx->list_stall_stat ? 1 : 0;
    default:                 return 1;  /* default: channel ready */
    }
}

/* ===========================================================================
 * Indirect-branch dispatch + function registry
 * ===========================================================================*/
typedef void (*spu_fn)(spu_context*);

typedef struct {
    uint32_t addr;
    spu_fn   fn;
    int      image_id;   /* which recompiled image this function belongs to */
    uint32_t sig0, sig1; /* racacit: LS words seen at the first policy-mode dispatch */
    int      sig_set;
    int      ovl;        /* racacit: 1 + index into s_ovl for a content-matched overlay */
    uint32_t len;        /* racacit: overlay entry: bytes to its next entry (capped) */
} spu_reg_entry;

/* racacit: content-matched overlay images. A SPURS job binary is DMA'd into a
 * fixed LS slot that many different jobs share (A Crack in Time's igPm puts
 * every job at LS 0x100), so neither the address nor the context's image id
 * says which program is resident. Each lifted job image registers its GUID
 * header (the four `ila $r2,...` words every job starts with) and the code
 * words at each lifted entry; a lookup serves an overlay function only when
 * BOTH are what the local store holds right now. */
/* guid: the 16 bytes at base + anchor that identify the image in local store.
 * anchor is 0 for a job at its header; a module a job loads above itself has
 * its header overwritten once it is in place, so it is told by its code. */
typedef struct { int image_id; uint32_t base; uint32_t anchor; uint8_t guid[16];
                 const uint8_t* bytes; uint32_t size; int reloc; } spu_ovl_desc;
#define SPU_OVL_MAX 512
static spu_ovl_desc s_ovl[SPU_OVL_MAX];
static int s_ovl_count = 0;

/* One entry per lifted SPU function across EVERY registered image, and a game
 * that lifts its whole SPU workload set has a lot of them: Yakuza: Dead Souls
 * registers ~170k (cri_audio alone is ~35k, gs_task ~9k, the job binaries ~11k).
 * At 65536 the registry silently truncated every image registered past the cap
 * -- and the SPURS job-chain policy, the job binaries and the Edge geometry
 * task (gs_task) register LAST, so their functions were dropped wholesale and
 * every indirect branch into them fell through to a branch-to-0. Size it for
 * the real workload; the overflow is now loud (see spu_register_function). */
#define SPU_FN_REGISTRY_MAX 262144
static spu_reg_entry s_registry[SPU_FN_REGISTRY_MAX];
static uint32_t s_registry_count = 0;

/* Accessor for the sampling profiler in ppu_loader.cpp. Its address->function
 * map is built from the PPU table alone, which leaves the SPU-lifted bodies as
 * unowned gaps -- and an [entry, next_entry) extent then charges every SPU
 * sample to whichever PPU function happens to precede it. That produced a
 * confident and completely wrong "99% of guest time in one PPU function".
 * Handing the SPU host pointers over lets the map cover both, so the gaps are
 * real entries and the extents mean something. */
uint32_t spu_registry_size(void) { return s_registry_count; }

/* racacit: which recompiled image registry entry i belongs to (the sampler
 * keys SPU samples by image + LS address; ids come from spu_begin_image in
 * game/spu/spu_jobs.c and friends). -1 if out of range. */
int spu_registry_image(uint32_t i)
{
    return i < s_registry_count ? s_registry[i].image_id : -1;
}

int spu_registry_entry(uint32_t i, void** host, uint32_t* ls_addr)
{
    if (i >= s_registry_count) return 0;
    if (host)    *host    = (void*)s_registry[i].fn;
    if (ls_addr) *ls_addr = s_registry[i].addr;
    return 1;
}

/* Hash index over the registry. spu_lookup runs on EVERY guest indirect
 * branch -- with the Bink decoder live that is millions of dispatches per
 * second, and the old linear scan (~950 binkspu entries walked per branch)
 * dominated movie playback. Buckets chain in REGISTRATION ORDER (tail
 * append) so the first-registered-match-wins semantics of the linear scan
 * are preserved exactly; the image-id wildcard rules stay in the bucket
 * walk, which is 1-3 entries (same LS addr across overlapping images).
 * Registration is startup-single-threaded; lookups treat the index as
 * read-only. Chain links store index+1 so zero-init means "empty". */
#define SPU_FN_HASH_SIZE 131072   /* power of two, ~= MAX/2 -> load factor ~2 */
static uint32_t s_hash_head[SPU_FN_HASH_SIZE];
static uint32_t s_hash_tail[SPU_FN_HASH_SIZE];
static uint32_t s_hash_next[SPU_FN_REGISTRY_MAX];

static inline uint32_t spu_fn_hash(uint32_t addr)
{
    return ((addr >> 2) * 2654435761u) & (SPU_FN_HASH_SIZE - 1);
}

/* Image currently being registered. SPURS images (kernel/policy/job) overlap in
 * LS, so each registers under a distinct id via spu_begin_image() before calling
 * its (prefixed) spu_recomp_register(). Single-image callers leave it 0. */
static int s_reg_image = 0;
void spu_begin_image(int image_id) { s_reg_image = image_id; }

void spu_register_function(uint32_t addr, spu_fn fn)
{
    if (s_registry_count >= SPU_FN_REGISTRY_MAX) {
        /* Silent truncation here dropped whole late-registered SPU images and
         * cost a multi-round hunt (the branch-to-0 looked like a lifter/overlay
         * bug). Never again: say so, once, loudly, with the number to raise the
         * cap to. */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr,
                    "[spu] FATAL: SPU function registry full at %u entries "
                    "(SPU_FN_REGISTRY_MAX=%u) -- image %d function 0x%05X and all "
                    "later registrations DROPPED. Raise SPU_FN_REGISTRY_MAX.\n",
                    s_registry_count, (unsigned)SPU_FN_REGISTRY_MAX,
                    s_reg_image, addr);
            fflush(stderr);
        }
        return;
    }
    uint32_t i = s_registry_count;
    s_registry[i].addr = addr;
    s_registry[i].fn = fn;
    s_registry[i].image_id = s_reg_image;
    s_registry_count = i + 1;
    uint32_t h = spu_fn_hash(addr);
    s_hash_next[i] = 0;
    if (s_hash_head[h] == 0)
        s_hash_head[h] = i + 1;
    else
        s_hash_next[s_hash_tail[h] - 1] = i + 1;
    s_hash_tail[h] = i + 1;
}

/* Report a CROSS-IMAGE match: the registry served a function some other image
 * registered at this address, because one side's image_id was the 0 wildcard.
 *
 * This is a real hazard, reported with evidence by @canersaka (#59): SPURS job
 * chains load job binaries into descriptor-assigned LS slots, so the same binary
 * can sit at different addresses on different rounds. If the lift was fixed-base
 * and the exact match misses, the wildcard can serve ANOTHER image's function at
 * that address -- the job then runs the wrong program end to end, returns
 * cleanly, and the only symptom is something three layers downstream that never
 * happens. Their words: one log line would have saved a day.
 *
 * Logging, not refusing. Refusing the wildcard outright was measured and does
 * NOT survive contact: a dormant task image can register spans overlapping the
 * resident kernel's low LS range, so a legitimate service-to-kernel yield gets
 * falsely rejected. Telling resident from dormant needs residency tracking the
 * registry does not have. So this reports and keeps going.
 *
 * Deduped per (addr, from, to): spu_lookup runs millions of times a second, and
 * a substitution that repeats every dispatch would bury the log it is meant to
 * make readable. */
static void spu_report_cross_image(uint32_t addr, int want, int got)
{
    enum { SEEN_MAX = 64 };
    static struct { uint32_t addr; int want, got; } seen[SEEN_MAX];
    static unsigned n_seen = 0;
    for (unsigned i = 0; i < n_seen; i++)
        if (seen[i].addr == addr && seen[i].want == want && seen[i].got == got)
            return;
    if (n_seen < SEEN_MAX) {
        seen[n_seen].addr = addr; seen[n_seen].want = want; seen[n_seen].got = got;
        n_seen++;
    }
    fprintf(stderr,
            "[spu] CROSS-IMAGE dispatch: LS 0x%05X requested by image %d, served "
            "by image %d's function (id-0 wildcard). If this job was loaded at a "
            "runtime-chosen base, it may be running the WRONG program -- see "
            "issue #59.\n", addr, want, got);
}

spu_fn spu_lookup(uint32_t addr, int image_id)   /* exported: clang-built fast-path dispatch (spu_dispatch_mt.c) needs it */
{
    /* Match the context's active image; image_id 0 (context or entry) matches
     * any, for back-compat with single-image contexts. */
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->ovl) continue;   /* content-matched overlays: spu_lookup_ctx only */
        if (e->addr == addr &&
            (image_id == 0 || e->image_id == 0 || e->image_id == image_id)) {
            /* One compare on the return path; the report itself is rare. */
            if (e->image_id != image_id)
                spu_report_cross_image(addr, image_id, e->image_id);
            return e->fn;
        }
    }
    return NULL;
}

/* racacit: content-checked lookup for policy-module contexts. HLE SPURS keeps
 * one context per (workload, lane) and the PM's jobs are DMA'd over the PM's
 * own code (A Crack in Time's igPm: job images land at LS 0x100.. and cover the
 * PM at 0xA00..0xC30). An address-only registry then serves the PM's lifted
 * function for the job's instructions. Record the code words the first time a
 * PM-context dispatch hits an entry -- the PM is intact on its own entry path
 * -- and refuse the lifted function when the local store no longer holds them,
 * so the interpreter runs what is really there. Signatures are sealed from the
 * pristine PM image (spu_registry_seal_image); unsealed entries are trusted. */
/* racacit: SPU_DUMP_JOBS=<dir>. A policy-module context that misses every
 * lifted image is running code nobody lifted -- typically a job binary that
 * only exists at run time (Bink's SPU decoder is shipped compressed and
 * unpacked by the PPU). Write the whole local store once per job GUID (the 16
 * bytes at LS 0x100, where igPm loads jobs) so it can be lifted offline with
 * scripts/lift_spu_jobs.py --from-dump. */
static void spu_dump_unknown_job(const spu_context* ctx, uint32_t addr)
{
    static const char* dir = NULL; static int init = 0;
    static uint8_t seen[64][16]; static int nseen = 0;
    static volatile int mu = 0;
    if (!init) { dir = getenv("SPU_DUMP_JOBS"); init = 1; }
    if (!dir || !*dir) return;
    const uint8_t* g = ctx->ls + 0x100;
    while (__sync_lock_test_and_set(&mu, 1)) { }
    for (int i = 0; i < nseen; i++)
        if (!memcmp(seen[i], g, 16)) { __sync_lock_release(&mu); return; }
    if (nseen < 64) memcpy(seen[nseen++], g, 16);
    else { __sync_lock_release(&mu); return; }
    char path[1024];
    snprintf(path, sizeof path, "%s/job_%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x_pc%05X.ls",
             dir, g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15],
             addr & SPU_LS_MASK);
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(ctx->ls, 1, SPU_LS_SIZE, f); fclose(f); }
    fprintf(stderr, "[spu-dump] unlifted job in img %d at LS 0x%05X -> %s\n", ctx->image_id, addr, f ? path : "(open failed)");
    __sync_lock_release(&mu);
}

/* SPU_MISS_DIAG: why an address the interpreter was entered at matched no
 * lifted function -- every registry entry at that address, and for an
 * overlay entry whether its anchor GUID and its first two code words are in
 * local store. "no entry" = the lift never produced that function. */
void spu_lookup_explain(const spu_context* ctx, uint32_t addr)
{
    static int on = -1;
    if (on < 0) on = getenv("SPU_MISS_DIAG") ? 1 : 0;
    if (!on || !ctx) return;
    /* SPU_MISS_DUMP=<dir>: also the whole local store at that moment, so the
     * module behind the entry can be found by a long match offline. */
    { static const char* dir = (const char*)1; static int n = 0;
      if (dir == (const char*)1) dir = getenv("SPU_MISS_DUMP");
      if (dir && *dir && n < 400) {
          n++;
          char path[1024];
          snprintf(path, sizeof path, "%s/miss_%02x%02x%02x%02x_%05X.ls", dir,
                   ctx->ls[0x100], ctx->ls[0x101], ctx->ls[0x102], ctx->ls[0x103], addr & SPU_LS_MASK);
          FILE* f = fopen(path, "wb");
          if (f) { fwrite(ctx->ls, 1, SPU_LS_SIZE, f); fclose(f); }
      } }
    const uint8_t* q = ctx->ls + (addr & SPU_LS_MASK);
    const uint32_t w0 = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
    const uint32_t w1 = ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) | ((uint32_t)q[6] << 8) | q[7];
    fprintf(stderr, "[miss-diag] img=%d LS 0x%05X words %08X %08X:", ctx->image_id, addr, w0, w1);
    int n = 0;
    for (uint32_t k = s_hash_head[spu_fn_hash(addr)]; k; k = s_hash_next[k - 1]) {
        const spu_reg_entry* e = &s_registry[k - 1];
        if (e->addr != addr) continue;
        n++;
        if (e->ovl) {
            const spu_ovl_desc* o = &s_ovl[e->ovl - 1];
            fprintf(stderr, " [ovl img %d base 0x%05X guid %s sig %s", o->image_id, o->base,
                    memcmp(ctx->ls + o->base + o->anchor, o->guid, 16) ? "MISS" : "ok",
                    (w0 == e->sig0 && w1 == e->sig1) ? "ok" : "MISS");
            /* ...and, for a matching entry, the first body word that differs
             * (a patched module stays in the interpreter). */
            if (w0 == e->sig0 && w1 == e->sig1 && e->len > 8) {
                const uint8_t* lb = o->bytes + (e->addr - o->base);
                for (uint32_t b = 0; b + 4 <= e->len; b += 4)
                    if (memcmp(q + b, lb + b, 4)) {
                        fprintf(stderr, " body len 0x%X differs at +0x%X: ls %02X%02X%02X%02X lifted %02X%02X%02X%02X",
                                e->len, b, q[b], q[b+1], q[b+2], q[b+3], lb[b], lb[b+1], lb[b+2], lb[b+3]);
                        break;
                    }
            }
            fprintf(stderr, "]");
        } else {
            fprintf(stderr, " [img %d sig_set %d sig %s]", e->image_id, e->sig_set,
                    (w0 == e->sig0 && w1 == e->sig1) ? "ok" : "MISS");
        }
    }
    if (!n) fprintf(stderr, " no entry");
    fprintf(stderr, "%c", 10);
}

static spu_fn spu_lookup_fixed(const spu_context* ctx, uint32_t addr, int image_id)
{
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        spu_reg_entry* e = &s_registry[n - 1];
        if (e->addr != addr) continue;
        if (e->ovl) {
            /* Overlay entry: matched by content, whatever the context's image. */
            if (!ctx || addr + 8 > SPU_LS_SIZE) continue;
            const spu_ovl_desc* o = &s_ovl[e->ovl - 1];
            if (spu_mem_ne(ctx->ls + o->base + o->anchor, o->guid, 16) != 0) continue;
            const uint8_t* q = ctx->ls + addr;
            uint32_t w0 = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
            uint32_t w1 = ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) | ((uint32_t)q[6] << 8) | q[7];
            if (w0 != e->sig0 || w1 != e->sig1) continue;
            /* The whole body, not just its first two words: a job may patch
             * a module after loading it (A Crack in Time's job 4360b202 does,
             * at 0x29C00), and serving the unpatched lift fed that job garbage
             * that wiped local store. A patched body runs in the interpreter. */
            if (e->len > 8 && spu_mem_ne(q, o->bytes + (e->addr - o->base), e->len) != 0) {
                static int dbg = -1; if (dbg < 0) dbg = getenv("SPU_BODY_DIAG") ? 1 : 0;   /* which word differs */
                if (dbg) { static uint32_t told[64]; static unsigned nt;
                    int seen = 0; for (unsigned t = 0; t < nt && t < 64; t++) if (told[t] == addr) seen = 1;
                    if (!seen && nt < 64) { told[nt++] = addr;
                        const uint8_t* lb = o->bytes + (e->addr - o->base);
                        for (uint32_t b = 0; b + 4 <= e->len; b += 4) if (memcmp(q + b, lb + b, 4)) {
                            fprintf(stderr, "[body-diag] img %d entry 0x%05X len 0x%X first diff +0x%X ls %02X%02X%02X%02X lifted %02X%02X%02X%02X%c",
                                    o->image_id, addr, e->len, b, q[b], q[b+1], q[b+2], q[b+3], lb[b], lb[b+1], lb[b+2], lb[b+3], 10);
                            break; } } }
                continue;
            }
            { static unsigned char s_said[SPU_OVL_MAX];
              if (!s_said[e->ovl - 1]) { s_said[e->ovl - 1] = 1;
                  fprintf(stderr, "[spu-ovl] lifted overlay image %d resident (first hit LS 0x%05X, ctx img %d)\n",
                          o->image_id, addr, image_id); } }
            return e->fn;
        }
        if (!(image_id == 0 || e->image_id == 0 || e->image_id == image_id)) continue;
        if (ctx && ctx->policy_mode && addr + 8 <= SPU_LS_SIZE && e->sig_set) {
            const uint8_t* q = ctx->ls + addr;
            uint32_t w0 = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
            uint32_t w1 = ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) | ((uint32_t)q[6] << 8) | q[7];
            if (w0 != e->sig0 || w1 != e->sig1) {
                static int _n = 0;
                if ((_n < 8 && (_n++, 1)))
                    fprintf(stderr, "[spu] img=%d LS 0x%05X no longer holds its lifted code "
                            "(job overlay) -- trying overlays / interpreting\n", image_id, addr);
                continue;      /* an overlay registered later may own it */
            }
        }
        if (e->image_id != image_id)
            spu_report_cross_image(addr, image_id, e->image_id);
        return e->fn;
    }
    return NULL;
}

/* ---- relocatable overlays (spu_lifter.py --reloc) -------------------------
 * A module a job loads at a base of its own choosing matches nothing by
 * address. Index its functions by their first two code words instead; a
 * candidate puts the module at base' = addr - (entry - lift base), and is
 * served when the module's anchor bytes are at base' + anchor and the code
 * from the entry on is the module's own. The lifted code then runs with
 * ls_delta = base' - lift base. */
#define SPU_RL_HASH 8192u
static uint32_t s_rl_head[SPU_RL_HASH];
static uint32_t s_rl_next[SPU_FN_REGISTRY_MAX];
static volatile int s_rl_built = 0, s_rl_any = 0, s_rl_mu = 0;

static inline uint32_t spu_rl_hash(uint32_t a, uint32_t b)
{
    return ((a * 2654435761u) ^ (b * 2246822519u)) >> 19;   /* 13 bits */
}

/* SPU_NO_RELOC=1: serve relocatable images only at their lift base (as
 * before --reloc), to tell a relocation bug from anything else. */
void spu_mark_overlay_relocatable(int image_id)
{
    if (getenv("SPU_NO_RELOC")) return;
    for (int i = 0; i < s_ovl_count; i++)
        if (s_ovl[i].image_id == image_id) { s_ovl[i].reloc = 1; s_rl_any = 1; }
}

static void spu_rl_build(void)
{
    while (__sync_lock_test_and_set(&s_rl_mu, 1)) { }
    if (!s_rl_built) {
        for (uint32_t i = 0; i < s_registry_count; i++) {
            const spu_reg_entry* e = &s_registry[i];
            if (!e->ovl || !s_ovl[e->ovl - 1].reloc || !e->sig_set) continue;
            const uint32_t h = spu_rl_hash(e->sig0, e->sig1);
            s_rl_next[i] = s_rl_head[h];
            s_rl_head[h] = i + 1;
        }
        __sync_synchronize();
        s_rl_built = 1;
    }
    __sync_lock_release(&s_rl_mu);
}

static spu_fn spu_lookup_reloc(const spu_context* ctx, uint32_t addr, uint32_t* delta)
{
    if (!s_rl_built) spu_rl_build();
    const uint8_t* q = ctx->ls + addr;
    const uint32_t w0 = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
    const uint32_t w1 = ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) | ((uint32_t)q[6] << 8) | q[7];
    /* Verified (overlay, base) pairs: the anchor and entry words are checked
     * on every hit, the longer code compare only once per pair. */
    static _Thread_local struct { int ovl; uint32_t base; } cache[8];
    static _Thread_local unsigned cache_i;
    for (uint32_t n = s_rl_head[spu_rl_hash(w0, w1)]; n; n = s_rl_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->sig0 != w0 || e->sig1 != w1) continue;
        const spu_ovl_desc* o = &s_ovl[e->ovl - 1];
        const uint32_t off = e->addr - o->base;
        if (addr < off) continue;
        const uint32_t base = addr - off;
        if (base + o->size > SPU_LS_SIZE) continue;
        if (spu_mem_ne(ctx->ls + base + o->anchor, o->guid, 16) != 0) continue;
        if (e->len > 8 && spu_mem_ne(ctx->ls + addr, o->bytes + off, e->len) != 0) {  /* patched body */
            static int dbg = -1; if (dbg < 0) dbg = getenv("SPU_BODY_DIAG") ? 1 : 0;
            if (dbg) { static uint32_t told[64]; static unsigned nt; int seen = 0;
                for (unsigned t = 0; t < nt && t < 64; t++) if (told[t] == addr) seen = 1;
                if (!seen && nt < 64) { told[nt++] = addr; unsigned nd = 0; uint32_t first = 0, last = 0;
                    for (uint32_t b = 0; b + 4 <= e->len; b += 4) if (memcmp(ctx->ls + addr + b, o->bytes + off + b, 4)) { if (!nd) first = b; last = b; nd++; }
                    fprintf(stderr, "[body-diag] reloc img %d entry 0x%05X len 0x%X: %u word(s) differ, +0x%X..+0x%X%c", o->image_id, addr, e->len, nd, first, last, 10); } }
            continue;
        }
        int known = 0;
        for (unsigned c = 0; c < 8; c++)
            if (cache[c].ovl == e->ovl && cache[c].base == base) { known = 1; break; }
        if (!known) {
            const uint32_t aspan = o->size - o->anchor < 256u ? o->size - o->anchor : 256u;
            if (spu_mem_ne(ctx->ls + base + o->anchor, o->bytes + o->anchor, aspan) != 0) continue;
            const unsigned c = cache_i++ & 7u;
            cache[c].ovl = e->ovl; cache[c].base = base;
            static int said[SPU_OVL_MAX];
            if (said[e->ovl - 1] < 4) { said[e->ovl - 1]++;
                fprintf(stderr, "[spu-ovl] relocatable image %d at LS 0x%05X (lifted at 0x%05X), entry 0x%05X%c",
                        o->image_id, base, o->base, addr, 10); }
        }
        *delta = base - o->base;
        return e->fn;
    }
    return NULL;
}

spu_fn spu_lookup_ctx(const spu_context* ctx, uint32_t addr, int image_id)
{
    spu_fn fn = spu_lookup_fixed(ctx, addr, image_id);
    if (fn) {
        if (ctx) ((spu_context*)ctx)->ls_delta = 0;
        return fn;
    }
    if (ctx && s_rl_any && addr + 8 <= SPU_LS_SIZE) {
        uint32_t delta = 0;
        fn = spu_lookup_reloc(ctx, addr, &delta);
        if (fn) { ((spu_context*)ctx)->ls_delta = delta; return fn; }
    }
    if (ctx && ctx->policy_mode && (addr < 0x3F000u || getenv("SPU_DUMP_STUBS")))   /* 0x3Fxxx: PM-generated stubs */
        spu_dump_unknown_job(ctx, addr);
    return NULL;
}

/* racacit: declare image `image_id` (already registered) a content-matched
 * overlay loaded at LS `base`; `bytes`/`size` is the image as DMA'd there. */
void spu_register_overlay_image_at(int image_id, uint32_t base, const uint8_t* bytes, uint32_t size, uint32_t anchor);
void spu_register_overlay_image(int image_id, uint32_t base, const uint8_t* bytes, uint32_t size)
{
    spu_register_overlay_image_at(image_id, base, bytes, size, 0);
}

/* As above, identified by the 16 bytes at `anchor` into the image. */
void spu_register_overlay_image_at(int image_id, uint32_t base, const uint8_t* bytes, uint32_t size, uint32_t anchor)
{
    if (s_ovl_count >= SPU_OVL_MAX || size < anchor + 16 || base + size > SPU_LS_SIZE) return;
    /* SPU_NO_SUB=1: leave the job sub-modules (images 400+) to the
     * interpreter -- to tell a lifting problem in them from anything else. */
    if (image_id >= 400 && getenv("SPU_NO_SUB")) return;
    /* SPU_NO_JOBS=1: the same for the content-matched jobs (images 200-399). */
    if (image_id >= 200 && image_id < 400 && getenv("SPU_NO_JOBS")) return;
    spu_ovl_desc* o = &s_ovl[s_ovl_count];
    o->image_id = image_id; o->base = base; o->anchor = anchor;
    memcpy(o->guid, bytes + anchor, 16);
    o->bytes = bytes; o->size = size; o->reloc = 0;
    int idx = ++s_ovl_count, n = 0;
    for (uint32_t i = 0; i < s_registry_count; i++) {
        spu_reg_entry* e = &s_registry[i];
        if (e->image_id != image_id || e->addr < base || e->addr + 8 > base + size) continue;
        const uint8_t* q = bytes + (e->addr - base);
        e->sig0 = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
        e->sig1 = ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) | ((uint32_t)q[6] << 8) | q[7];
        e->sig_set = 1; e->ovl = idx; n++;
    }
    /* Body lengths: each entry runs to the next entry of this image (or the
     * image end), capped -- what the lookup compares before serving it. */
    { uint32_t cnt = 0;
      for (uint32_t i = 0; i < s_registry_count; i++) if (s_registry[i].ovl == idx) cnt++;
      uint32_t* ix = cnt ? (uint32_t*)malloc(cnt * sizeof *ix) : NULL;
      if (ix) {
          uint32_t k = 0;
          for (uint32_t i = 0; i < s_registry_count; i++) if (s_registry[i].ovl == idx) ix[k++] = i;
          for (uint32_t x = 1; x < cnt; x++) {          /* insertion sort by addr */
              const uint32_t v = ix[x]; uint32_t y = x;
              while (y && s_registry[ix[y - 1]].addr > s_registry[v].addr) { ix[y] = ix[y - 1]; y--; }
              ix[y] = v;
          }
          for (uint32_t x = 0; x < cnt; x++) {
              spu_reg_entry* e = &s_registry[ix[x]];
              uint32_t end = base + size;
              for (uint32_t y = x + 1; y < cnt; y++)
                  if (s_registry[ix[y]].addr > e->addr) { end = s_registry[ix[y]].addr; break; }
              const uint32_t len = end > e->addr ? end - e->addr : 0;
              e->len = len > 1024u ? 1024u : len;
          }
          free(ix);
      } }
    (void)n;
}

/* racacit: record the code words of every lifted entry of `image_id` inside
 * [lo, hi) from a local store that is known to hold that image's code (the
 * policy module right after it is copied in). spu_lookup_ctx compares against
 * these, so a job image DMA'd over the PM later is never mistaken for it. */
void spu_registry_seal_image(int image_id, const uint8_t* ls, uint32_t lo, uint32_t hi)
{
    for (uint32_t i = 0; i < s_registry_count; i++) {
        spu_reg_entry* e = &s_registry[i];
        if (e->image_id != image_id || e->addr < lo || e->addr + 8 > hi) continue;
        const uint8_t* q = ls + e->addr;
        e->sig0 = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
        e->sig1 = ((uint32_t)q[4] << 24) | ((uint32_t)q[5] << 16) | ((uint32_t)q[6] << 8) | q[7];
        e->sig_set = 1;
    }
}

/* Which image registered a function at this LS address, or -1 if none did.
 *
 * Deliberately NOT spu_lookup(addr, 0): that call means "dispatch here in a
 * context with no image", so it reports a cross-image substitution for every
 * hit in a real image. This is a question about the registry, not a dispatch,
 * and it runs once per SPU thread start. First match wins, in registration
 * order, so it agrees with which function spu_lookup would actually serve. */
int spu_image_of_function(uint32_t addr)
{
    for (uint32_t n = s_hash_head[spu_fn_hash(addr)]; n; n = s_hash_next[n - 1]) {
        const spu_reg_entry* e = &s_registry[n - 1];
        if (e->addr == addr) return e->image_id;
    }
    return -1;
}

/* Does a lifted function exist at this LS address (any image)? Lets the lv2
 * layer decide between real SPU execution and the PPU-fallback paths. */
int spu_have_function(uint32_t addr)
{
    return spu_image_of_function(addr) >= 0;
}

/* The pure interpreter's fast-path "is this LSA already lifted?" probe
 * (spu_interp.c, rejoin path). It IS the registry lookup, so overlay eviction and
 * self-modifying-code invalidation are honored automatically. Lived in
 * spu_fn_registry.c before the registry was consolidated into this TU. */
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa)
{
    return (spu_lifted_fn)spu_lookup_ctx(ctx, lsa, ctx ? ctx->image_id : 0);
}

/* ---- Swappable code overlays (see spu_context.resident_ovl) --------------
 * A title registers each runtime-streamed overlay's SOURCE content EA with
 * the image id its lifted functions were registered under. The MFC GET path
 * marks that overlay resident in the streaming context; dispatch retries a
 * primary-image miss against the resident overlay's registry. */
typedef struct { uint32_t src_ea; int image_id; uint8_t sig[16]; int has_sig; uint32_t span; } spu_ovl_src;
/* 6 FMOD codec/DSP overlays + up to 89 WWS job-code modules (all stream into
 * the same job code buffer at LS 0x4000, dispatched by content signature). */
#define SPU_OVL_SRC_MAX 128
static spu_ovl_src s_ovl_src[SPU_OVL_SRC_MAX];
static int s_ovl_src_count = 0;

void spu_overlay_register_source(uint32_t content_ea, int image_id)
{
    if (s_ovl_src_count < SPU_OVL_SRC_MAX) {
        s_ovl_src[s_ovl_src_count].src_ea = content_ea;
        s_ovl_src[s_ovl_src_count].image_id = image_id;
        s_ovl_src[s_ovl_src_count].has_sig = 0;
        s_ovl_src_count++;
    }
}

/* Register a bounded code image that may coexist with other streamed images.
 * Its functions must be translated at the local-store addresses used by the title. */
void spu_overlay_register_region(uint32_t content_ea, uint32_t span, int image_id)
{
    if (!span || span > SPU_LS_SIZE || s_ovl_src_count >= SPU_OVL_SRC_MAX) return;
    spu_overlay_register_source(content_ea, image_id);
    s_ovl_src[s_ovl_src_count - 1].span = span;
}

/* Register one-way runtime entries that replace the guest call stack.
 * Matching the resolved function, not just its LS address, avoids affecting
 * unrelated overlays using the same address. Registration precedes execution. */
static struct { uint32_t entry; int image_id; } s_stack_reset[16];
static unsigned s_stack_reset_count;
void spu_register_stack_reset_entry(uint32_t entry, int image_id)
{
    if (s_stack_reset_count < 16) {
        s_stack_reset[s_stack_reset_count].entry = entry & SPU_LS_MASK;
        s_stack_reset[s_stack_reset_count++].image_id = image_id;
    }
}

/* Also called before direct trampoline transfers: generated calls can enter
 * a stack-switching routine without going through spu_indirect_branch. */
void spu_check_stack_reset(spu_context* ctx, void (*fn)(spu_context*))
{
    if (!ctx->host_depth || !s_spu_halt_armed || ctx->policy_mode) return;
    for (unsigned i = 0; i < s_stack_reset_count; ++i)
        if (ctx->pc == s_stack_reset[i].entry &&
            fn == spu_lookup(ctx->pc, s_stack_reset[i].image_id)) {
            g_spu_trampoline_fn = 0;
            longjmp(s_spu_halt_env, 2);
        }
}

/* SPURS taskset TASK entries (see spu_context.resident_task). A taskset can hold
 * several tasks whose lifts share the SAME LS base -- the co-resident task-code
 * region -- so no LS address identifies which task owns it. The title registers
 * each task's ELF ENTRY point with the image id its functions were registered
 * under; spu_indirect_branch adopts that image the moment the policy branches
 * into the entry from outside the region. This is what lets each such task be
 * registered under its own real image id instead of the id-0 wildcard, so a
 * dormant co-resident task can no longer shadow the one the policy launched. */
#define SPU_TASK_ENTRY_MAX 16
static struct { uint32_t entry; int image_id; } s_task_entry[SPU_TASK_ENTRY_MAX];
static int s_task_entry_count = 0;
void spu_taskset_register_task_entry(uint32_t entry, int image_id)
{
    if (s_task_entry_count < SPU_TASK_ENTRY_MAX) {
        s_task_entry[s_task_entry_count].entry = entry & SPU_LS_MASK;
        s_task_entry[s_task_entry_count].image_id = image_id;
        s_task_entry_count++;
    }
}
static int spu_taskset_task_image(uint32_t entry)
{
    for (int i = 0; i < s_task_entry_count; i++)
        if (s_task_entry[i].entry == entry) return s_task_entry[i].image_id;
    return 0;
}
/* Resume PCs and even fresh entry PCs overlap between task ELFs. The
 * taskset policy's TaskInfo identifies which ELF it has actually loaded. */
static struct { uint32_t elf_ea; int image_id, policy_image_id; }
    s_task_elf[SPU_TASK_ENTRY_MAX];
static int s_task_elf_count;
void spu_taskset_register_task_elf(uint32_t elf_ea, int image_id, int policy_image_id)
{
    if (s_task_elf_count < SPU_TASK_ENTRY_MAX) {
        s_task_elf[s_task_elf_count].elf_ea = elf_ea & ~7u;
        s_task_elf[s_task_elf_count].image_id = image_id;
        s_task_elf[s_task_elf_count++].policy_image_id = policy_image_id;
    }
}
static int spu_taskset_resident_image(const spu_context* ctx)
{
    const uint8_t* p = ctx->ls + 0x2794; /* SpursTasksetContext.taskInfo.elf */
    uint32_t elf = (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                    ((uint32_t)p[2] << 8) | p[3]) & ~7u;
    for (int i = 0; i < s_task_elf_count; ++i)
        if (s_task_elf[i].elf_ea == elf &&
            s_task_elf[i].policy_image_id == ctx->resident_ovl)
            return s_task_elf[i].image_id;
    return 0;
}
/* Lowest LS address of the shared task-code region: the taskset tasks all lift
 * at LS 0x3000 (below it is the SPURS kernel/policy/context, 0x290..0x2FFF). */
#define SPU_TASKSET_TASK_LO 0x3000u

/* Content-signature variant: FMOD COPIES codec overlays to the heap before
 * streaming them into the swap slot, so the source EA is unknowable ahead of
 * time -- match the first 16 bytes of the streamed content instead. */
void spu_overlay_register_sig(const uint8_t sig[16], int image_id)
{
    if (s_ovl_src_count < SPU_OVL_SRC_MAX) {
        s_ovl_src[s_ovl_src_count].src_ea = 0;
        s_ovl_src[s_ovl_src_count].image_id = image_id;
        memcpy(s_ovl_src[s_ovl_src_count].sig, sig, 16);
        s_ovl_src[s_ovl_src_count].has_sig = 1;
        s_ovl_src_count++;
    }
}

/* Called from the MFC GET path after the copy: ls points at the JUST-COPIED
 * bytes. EA match first (exact, cheap), then content signature for sizeable
 * chunks (overlay bodies are >= 0x500 bytes). */
/* SPU_DUMP_OVL=<dir>[,<lsa>]: write out every distinct code body streamed into
 * LS <lsa> (default 0x5000, the job code buffer), so job bodies the title
 * keeps in its own data can be lifted. A body larger than one DMA arrives as
 * consecutive GETs (contiguous EA and LS); those are appended to the same
 * file. One file per source EA: ovl_<ea>.bin. */
static void spu_ovl_dump(spu_context* ctx, uint32_t ea, uint32_t lsa, const uint8_t* ls, uint32_t size)
{
    static int s_on = -1; static char s_dir[400]; static uint32_t s_lsa = 0x5000;
    if (s_on < 0) { const char* e = getenv("SPU_DUMP_OVL"); s_on = e && *e;
        if (s_on) { snprintf(s_dir, sizeof s_dir, "%s", e);
                    char* c = strchr(s_dir, ','); if (c) { *c = 0; s_lsa = (uint32_t)strtoul(c + 1, 0, 0); } } }
    if (!s_on) return;
    static uint32_t s_seen[256]; static unsigned s_n;
    static SPU_TLS uint32_t t_src, t_next_ea, t_next_lsa;
    FILE* f = NULL; char path[512];
    if (lsa == s_lsa && size >= 256) {
        for (unsigned i = 0; i < s_n; i++) if (s_seen[i] == ea) { t_src = 0; return; }
        if (s_n < 256) s_seen[s_n++] = ea;
        t_src = ea;
        snprintf(path, sizeof path, "%s/ovl_%08X.bin", s_dir, ea);
        f = fopen(path, "wb");
        fprintf(stderr, "[spu-ovl-dump] img=%d body ea=0x%08X size=%u -> %s\n", ctx->image_id, ea, size, path);
    } else if (t_src && ea == t_next_ea && lsa == t_next_lsa) {
        snprintf(path, sizeof path, "%s/ovl_%08X.bin", s_dir, t_src);
        f = fopen(path, "ab");
    } else { t_src = 0; return; }
    if (f) { fwrite(ls, 1, size, f); fclose(f); }
    t_next_ea = ea + size; t_next_lsa = lsa + size;
}

/* SPU_LOCKGUARD=<hex ea>: the word at that EA is a ticket lock (serving,next
 * halfwords). Log any SPU write that moves either halfword by anything other
 * than 0 or +1 -- a stale whole-line store rolling the lock back. `src` is the
 * new bytes for [ea, ea+size). */
void spu_lockguard_check(spu_context* ctx, uint32_t ea, const uint8_t* src, uint32_t size, const char* what)
{
    static int64_t s_g = -2;
    if (s_g == -2) { const char* e = getenv("SPU_LOCKGUARD"); s_g = e ? (int64_t)strtoul(e, 0, 16) : -1; }
    if (s_g < 0 || !vm_base) return;
    uint32_t g = (uint32_t)s_g;
    if (g < ea || g + 4 > ea + size) return;
    const uint8_t* o = vm_base + g; const uint8_t* n = src + (g - ea);
    unsigned os = (o[0] << 8) | o[1], on = (o[2] << 8) | o[3];
    unsigned ns = (n[0] << 8) | n[1], nn = (n[2] << 8) | n[3];
    int bad = !(ns == os || ns == ((os + 1) & 0xFFFF)) || !(nn == on || nn == ((on + 1) & 0xFFFF));
    if (bad) { static int c; if ((c < 32 && (c++, 1)))
        fprintf(stderr, "[lockguard] %s img=%d pc=0x%05X ea=0x%08X size=%u: %04X/%04X -> %04X/%04X\n",
                what, ctx ? ctx->image_id : -1, ctx ? (unsigned)(ctx->pc & SPU_LS_MASK) : 0u,
                ea, size, os, on, ns, nn); }
}

void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size)
{
    uint32_t lsa = (uint32_t)(ls - ctx->ls);
    spu_ovl_dump(ctx, ea, lsa, ls, size);
    /* PS3_TIMELINE: a GUID-headed code module (a job or sub-module; its first
     * word is 0x4.....02) arriving in local store -- 'M' guid word, lsa | lane. */
    { static int tl = -1; if (tl < 0) tl = getenv("PS3_TIMELINE") ? 1 : 0;
      if (tl && size >= 256) {
          const uint32_t w = (uint32_t)ls[0] << 24 | (uint32_t)ls[1] << 16 | (uint32_t)ls[2] << 8 | ls[3];
          if ((w & 0xF00000FFu) == 0x40000002u) {
              extern void ps3_tl(char ev, uint32_t a, uint32_t b); extern unsigned ps3_tl_tix(void);
              ps3_tl('M', w, (lsa & ~15u) | (ps3_tl_tix() & 15u));
          } } }
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (!ctx->resident_code[slot].image_id) continue;
        uint32_t base = ctx->resident_code[slot].lsa;
        uint32_t end = base + ctx->resident_code[slot].size;
        if (lsa < end && lsa + size > base &&
            (lsa < base || ea != ctx->resident_code[slot].source_ea + (lsa - base)))
            ctx->resident_code[slot].image_id = 0;
    }
    for (int i = 0; i < s_ovl_src_count; i++) {
        const spu_ovl_src* o = &s_ovl_src[i];
        int hit = o->has_sig ? (size >= 512 && memcmp(ls, o->sig, 16) == 0)
                             : (o->src_ea == ea);
        if (hit) {
            if (o->span) {
                if (lsa + o->span > SPU_LS_SIZE) return;
                for (unsigned slot = 0; slot < 4; ++slot) {
                    if (ctx->resident_code[slot].image_id && ctx->resident_code[slot].lsa != lsa)
                        continue;
                    ctx->resident_code[slot].lsa = lsa;
                    ctx->resident_code[slot].size = o->span;
                    ctx->resident_code[slot].source_ea = ea;
                    ctx->resident_code[slot].image_id = o->image_id;
                    return;
                }
                fprintf(stderr, "[spu-ovl] no free resident code span for image %d\n", o->image_id);
                return;
            }
            if (ctx->resident_ovl != o->image_id) {
                ctx->resident_ovl = o->image_id;
                { static int _n = 0; if ((_n < 32 && (_n++, 1)))
                    fprintf(stderr, "[spu-ovl] img=%d streamed overlay src=0x%08X "
                            "-> LS 0x%05X size=%u -> resident ovl image %d%s\n",
                            ctx->image_id, ea, (uint32_t)(ls - ctx->ls), size,
                            ctx->resident_ovl, o->has_sig ? " (sig match)" : ""); }
            }
            return;
        }
    }
}

/* Match a 16-byte module header against the registered content signatures.
 * Returns the overlay image id, or 0 if none. Used by the WWS job-code dispatch
 * path: the job manager streams a module into the code buffer via a DMA LIST
 * (per-element GETs the single-transfer note_get hook doesn't see as one >=512
 * chunk), so resident_ovl is set at branch-in time by matching the buffer head. */
int spu_overlay_match_sig(const uint8_t hdr[16])
{
    for (int i = 0; i < s_ovl_src_count; i++) {
        const spu_ovl_src* o = &s_ovl_src[i];
        if (o->has_sig && memcmp(hdr, o->sig, 16) == 0)
            return o->image_id;
    }
    return 0;
}

/* HLE of the taskset Policy Module's task-syscall entry (LS 0xA70). A SPURS task
 * (e.g. the cri_mpv task, image 22) reads syscallAddr from its SpursTasksetContext
 * (LS 0x27C4) and branches to it to perform a task syscall (EXIT/YIELD/WAIT/POLL).
 * The real kernel has the PM code resident at 0xA70; we don't, so we plant 0xA70 as
 * syscallAddr (in the cri dispatch) and INTERCEPT a branch to it here to HLE the
 * syscall. num = r3&0xF (0x10 bit = the "2" variant), args in r4. Adopted from the
 * JonathanDC64/ps3recomp fork (aaea4158) which uses this to run SPURS tasks clean. */
/* Write the event bits a PPU-side Set owed this wait object into guest memory,
 * at object+0x00 (events) and every object+0x30 slot (pendingRecv).
 *
 * Called from the WAIT_SIGNAL handler immediately before control returns to the
 * guest. Doing it at Set time loses a race: the guest reads object+0x30 the
 * instant it resumes, and a task that took the latched-wake path was never
 * parked for the Set to target. Here the guest demonstrably has not read yet.
 *
 * ponytail: fills all 16 slots rather than the one the guest picked -- the slot
 * index is derived from state we do not model, and a wrong slot delivers
 * nothing. Narrow it if a title ever cares which slot fired. */
static void spu_ef_deliver_owed(spu_context* ctx, uint32_t wobj)
{
    (void)ctx;
    static int s_od = -1;
    if (s_od < 0) s_od = getenv("SPURS_EF_OBJ_DELIVER") ? 1 : 0;
    if (!s_od || !wobj || !vm_base) return;
    extern uint16_t spu_ef_bits_take(uint32_t);
    uint16_t bits = spu_ef_bits_take(wobj);
    if (!bits) return;
    uint8_t* m = vm_base + wobj;
    m[0] = (uint8_t)(bits >> 8); m[1] = (uint8_t)bits;          /* +0x00 events */
    for (uint32_t s = 0; s < 16; s++) {                          /* +0x30 slots */
        m[0x30 + 2*s]     = (uint8_t)(bits >> 8);
        m[0x30 + 2*s + 1] = (uint8_t)bits;
    }
    { static int _n = 0; if ((_n < 8 && (_n++, 1)))
        fprintf(stderr, "[spu] delivered owed bits=0x%04X to object 0x%08X \n",
                (unsigned)bits, wobj); }
}

void spu_spurs_taskset_syscall(spu_context* ctx)   /* non-static: also called by the pure interpreter (spu_interp.c) */
{
    uint32_t raw = ctx->gpr[3]._u32[0];
    uint32_t num = raw & 0x0F;
    /* SPU_SYSCALL_IMG=<n>: every syscall of image n, uncapped (the 24-line
     * budget is spent by FMOD's WAIT_SIGNAL loop long before a later task). */
    static int s_si = -2;
    if (s_si == -2) { const char* e = getenv("SPU_SYSCALL_IMG"); s_si = e ? atoi(e) : -1; }
    { static int _n = 0; if ((_n < 24 && (_n++, 1)) || ctx->image_id == s_si)
        fprintf(stderr, "[spu] SPURS taskset syscall num=%u (raw=0x%X args=0x%08X) image=%d link/r0=0x%05X wobj@2FDC=0x%02X%02X%02X%02X\n",
                num, raw, ctx->gpr[4]._u32[0], ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                ctx->ls[0x2FDC], ctx->ls[0x2FDD], ctx->ls[0x2FDE], ctx->ls[0x2FDF]); }
    /* The task-API argument is passed in LOCAL STORE at 0x2FD0, not in r4: the
     * caller does shufb(arg,...) -> stqd 0x2FD0 and only then sets r3 = number
     * (see func_00026DE0 / func_000272AC in image 22). Logging r4 shows caller
     * leftovers and hides what the task actually asked for. */
    { static int s_sa = -1; if (s_sa < 0) s_sa = getenv("YDKJ_SYSCALL_ARG") ? 1 : 0;
      if (s_sa) { static int _n = 0; if ((_n < 40 && (_n++, 1))) {
        const uint8_t* a = &ctx->ls[0x2FD0];
        fprintf(stderr, "[spu] syscall num=%u LS[0x2FD0]=%02X%02X%02X%02X %02X%02X%02X%02X"
                        " %02X%02X%02X%02X %02X%02X%02X%02X img=%d\n",
                num, a[0],a[1],a[2],a[3], a[4],a[5],a[6],a[7],
                a[8],a[9],a[10],a[11], a[12],a[13],a[14],a[15], ctx->image_id);
        fflush(stderr); } } }
    /* NOTE (YDKJ cri_mpv): the cri task's BOOTSTRAP (func_00003040) calls the
     * task-API syscall and EXPECTS IT TO RETURN, then branches to the real task
     * entry (0x3050). Halting on num=0 here kills the task at bootstrap before it
     * runs. So for image 22 we DON'T halt on num=0 -- we return so the bootstrap
     * continues to the decode entry. (A genuine end-of-task EXIT would re-enter and
     * spin; if that happens, gate a real halt after the task has done work.)
     * For non-cri images keep the fork's EXIT=halt semantics. Env YDKJ_CRI_EXIT_HALT
     * forces the old halt behaviour for comparison. */
    /* The cri bootstrap calls EXIT once and EXPECTS IT TO RETURN, then branches to
     * the real task entry -- so image 22 cannot halt on the first one. But a task
     * that has since done work and calls EXIT again means it: returning there makes
     * it re-enter and spin (observed: 6x num=0 from link 0x26E18 once the task is
     * woken). Honour the first call, halt on the rest. Each task runs on its own
     * host thread, so a thread-local count is per task. */
    static _Thread_local int s_exit_seen = 0;
    if (num == 0 && ctx->image_id == 22 && !getenv("YDKJ_CRI_EXIT_HALT")) {
        if (s_exit_seen++ == 0) { ctx->gpr[3]._u32[0] = 0; return; }   /* bootstrap */
        { static int _n = 0; if ((_n < 8 && (_n++, 1)))
            fprintf(stderr, "[spu] cri task EXIT #%d -- halting (was spinning)\n",
                    s_exit_seen); }
        ctx->status = SPU_STATUS_STOPPED_BY_STOP;
        spu_halt(ctx);
        return;
    }
    if (num == 0 && (ctx->image_id != 22 || getenv("YDKJ_CRI_EXIT_HALT"))) {
        ctx->status = SPU_STATUS_STOPPED_BY_STOP;
        spu_halt(ctx);          /* longjmp out to spu_run_with_halt; post-run writes exit code */
        return;
    }
    /* WAIT_SIGNAL(2): REAL semantics (RPCS3 spursTasksetProcessSyscall) --
     * consume the task's bit in the guest taskset's `signalled` bitset, or
     * SLEEP this task's host thread until _cellSpursSendSignal /
     * cellSpursEventFlagSet delivers one (the FMOD mixer's flag-A wait path:
     * the task registers its wait slot in the flag struct with atomics, then
     * syscalls WAIT_SIGNAL; the PPU-side Set satisfies the slot and signals).
     * Returning "success" here without waiting made the task spin on empty
     * work state forever -- the LBP boot deadlock. taskset/taskId come from
     * the SpursTasksetContext this runtime planted at LS 0x2700. */
    if (num == 2) {
        uint32_t ts  = ((uint32_t)ctx->ls[0x27BC] << 24) | ((uint32_t)ctx->ls[0x27BD] << 16) |
                       ((uint32_t)ctx->ls[0x27BE] << 8)  |  (uint32_t)ctx->ls[0x27BF];
        uint32_t tid = ((uint32_t)ctx->ls[0x27D4] << 24) | ((uint32_t)ctx->ls[0x27D5] << 16) |
                       ((uint32_t)ctx->ls[0x27D6] << 8)  |  (uint32_t)ctx->ls[0x27D7];
        /* The guest names its wait OBJECT in the task-API argument at LS 0x2FD0
         * (low nibble is flags). It reads its received event bits from
         * object+0x30, so this is both who to wake and where to deliver. */
        uint32_t wobj = (((uint32_t)ctx->ls[0x2FDC] << 24) |
                         ((uint32_t)ctx->ls[0x2FDD] << 16) |
                         ((uint32_t)ctx->ls[0x2FDE] << 8)  |
                          (uint32_t)ctx->ls[0x2FDF]) & ~0xFu;
        /* SPURS_EF_SPU_REPLY=1 -- DIAGNOSTIC BISECT, NOT A FIX.
         *
         * The guest task is supposed to set its SPU->PPU flag (object+0x100)
         * when a work cycle completes; it never does, so the PPU blocks on that
         * flag forever and can never enqueue the next unit of work. That is a
         * cycle: we cannot see what the PPU would do next without breaking it.
         *
         * On RE-ENTRY to WAIT_SIGNAL for the same object the previous cycle has
         * finished, so set the flag on the task`s behalf and watch where the PPU
         * goes. This FABRICATES guest state -- the repo`s own history says faked
         * success is the most expensive kind of bug (see the unresolved-NID note
         * in ppu_hle.cpp), so it stays opt-in and must never become the default.
         * If the PPU advances, the answer is what the task must do to earn it. */
        { static int s_rp = -1;
          if (s_rp < 0) { const char* e = getenv("SPURS_EF_SPU_REPLY");
                          s_rp = e ? atoi(e) : 0; }   /* 2 = reply on EVERY entry */
          if (s_rp && wobj) {
              static uint32_t seen[8]; static int seen_n = 0;
              int again = 0;
              for (int i = 0; i < seen_n; i++) if (seen[i] == wobj) { again = 1; break; }
              if (s_rp >= 2) again = 1;   /* force: answer "would the PPU move at all?" */
              if (!again) { if (seen_n < 8) seen[seen_n++] = wobj; }
              else {
                  extern void spurs_ef_set_from_spu(uint32_t, uint16_t);
                  spurs_ef_set_from_spu(wobj + 0x100u, 1);
                  static int _n = 0;
                  if ((_n < 8 && (_n++, 1)))
                      fprintf(stderr, "[spu] SPU-REPLY: set flag 0x%08X for object "
                                      "0x%08X (cycle complete)\n", wobj + 0x100u, wobj);
              }
          } }

        if (ts) {
            extern int spu_taskset_wait_signal(uint32_t, uint32_t);
            /* Park = OS-level wait on this SPU's host thread. Under the lockstep
             * gate the thread HOLDS the global run token here; parking without
             * releasing it starves every other lifted SPU (observed: FMOD task 1
             * parks in WAIT_SIGNAL holding the token -> the mixer never runs
             * again -> the PPU audio pump blocks forever on flag 0x94F600). */
            /* Publish that this task is parked so a PPU-side event-flag Set can
             * reach it even though it registered no wait slot in the flag. */
            extern void spu_taskset_parked_add(uint32_t, uint32_t, uint32_t);
            extern void spu_taskset_parked_del(uint32_t, uint32_t);
            extern int spu_taskset_consume_wake(uint32_t);
            if (spu_taskset_consume_wake(ts)) {   /* a Set beat us to the park */
                spu_ef_deliver_owed(ctx, wobj);
                ctx->gpr[3]._u32[0] = 0;
                return;
            }
            yz_lockstep_block_begin(ctx);
            spu_taskset_parked_add(ts, tid, wobj);
            spu_taskset_wait_signal(ts, tid);
            spu_taskset_parked_del(ts, tid);
            yz_lockstep_block_end(ctx);
            spu_ef_deliver_owed(ctx, wobj);
        }
        ctx->gpr[3]._u32[0] = 0;
        return;
    }
    /* EXIT(0, cri bootstrap)/YIELD(1)/POLL(3)/RECV_WKL_FLAG(4):
     * report success and resume (return -> lifted caller continues at its link). */
    ctx->gpr[3]._u32[0] = 0;
}

/* ---------------------------------------------------------------------------
 * Micro-interpreter for RUNTIME-GENERATED stub code (SMC).
 *
 * The WWS jobmanager's interrupt handler GENERATES a register save/restore
 * stub above the static image (LS 0x3FEC0: a chain of stqd/lqd plus a `bi`
 * terminator, written at interrupt entry) and calls it. No lift can exist for
 * bytes that only come into being at runtime, so when the indirect dispatch
 * finds no registered function we interpret the LIVE LS bytes directly for
 * the small instruction set such stubs use, and exit back into lifted code at
 * the first branch (trampoline re-dispatch). Returns 1 if it executed to a
 * branch, 0 on an unknown opcode (caller falls through to BRANCH-TO-0). */
static int spu_smc_microstep(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK & ~3u;
    for (int steps = 0; steps < 4096; steps++) {
        uint32_t w = ((uint32_t)ctx->ls[pc] << 24) | ((uint32_t)ctx->ls[pc+1] << 16) |
                     ((uint32_t)ctx->ls[pc+2] << 8) | ctx->ls[pc+3];
        uint32_t op11 = w >> 21, op9 = w >> 23, op8 = w >> 24, op7 = w >> 25;
        uint32_t rt = w & 0x7F, ra = (w >> 7) & 0x7F, rb = (w >> 14) & 0x7F;
        int32_t  i10 = (int32_t)(w << 8) >> 22;               /* bits 14-23 sext */
        int32_t  i16 = (int32_t)(int16_t)((w >> 7) & 0xFFFF);
        (void)rb;

        if (op11 == 0x1A8 || op11 == 0x1A9 || op11 == 0x1AA || op11 == 0x1AB) {
            /* bi / bisl / iret / bisled (+E/D interrupt bits) */
            if (op11 == 0x1AB &&                       /* bisled: only on event */
                (ctx->event_status & ctx->event_mask) == 0) { pc += 4; continue; }
            if (w & 0x40000) ctx->int_enable = 1;
            else if (w & 0x80000) ctx->int_enable = 0;
            uint32_t tgt = (op11 == 0x1AA) ? ctx->srr0
                                           : ctx->gpr[ra]._u32[0];
            if (op11 == 0x1A9 || op11 == 0x1AB)
                ctx->gpr[rt] = spu_splat_u32(pc + 4);
            ctx->pc = tgt & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op11 >= 0x128 && op11 <= 0x12B) {
            /* biz / binz / bihz / bihnz rt,ra (+E/D bits, taken-only) */
            uint32_t cv = ctx->gpr[rt]._u32[0];
            int taken;
            switch (op11) {
            case 0x128: taken = (cv == 0); break;                     /* biz  */
            case 0x129: taken = (cv != 0); break;                     /* binz */
            case 0x12A: taken = ((cv & 0xFFFF) == 0); break;          /* bihz */
            default:    taken = ((cv & 0xFFFF) != 0); break;          /* bihnz*/
            }
            if (!taken) { pc += 4; continue; }
            if (w & 0x40000) ctx->int_enable = 1;
            else if (w & 0x80000) ctx->int_enable = 0;
            ctx->pc = ctx->gpr[ra]._u32[0] & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op9 == 0x064 || op9 == 0x060 || op9 == 0x066 || op9 == 0x062) {
            /* br / bra / brsl / brasl */
            uint32_t tgt = (op9 == 0x060 || op9 == 0x062)
                         ? ((uint32_t)i16 << 2)
                         : (pc + ((uint32_t)i16 << 2));
            if (op9 == 0x066 || op9 == 0x062)
                ctx->gpr[rt] = spu_splat_u32(pc + 4);
            ctx->pc = tgt & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op9 == 0x040 || op9 == 0x042 || op9 == 0x044 || op9 == 0x046) {
            /* brz / brnz / brhz / brhnz rt,label (relative) */
            uint32_t cv = ctx->gpr[rt]._u32[0];
            int taken;
            switch (op9) {
            case 0x040: taken = (cv == 0); break;
            case 0x042: taken = (cv != 0); break;
            case 0x044: taken = ((cv & 0xFFFF) == 0); break;
            default:    taken = ((cv & 0xFFFF) != 0); break;
            }
            if (!taken) { pc += 4; continue; }
            ctx->pc = (pc + ((uint32_t)i16 << 2)) & SPU_LS_MASK & ~3u;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }
        if (op8 == 0x24) {                                    /* stqd rt,i10(ra) */
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            spu_ls_write128(ctx, a, ctx->gpr[rt]);
            pc += 4; continue;
        }
        if (op8 == 0x34) {                                    /* lqd rt,i10(ra) */
            uint32_t a = (ctx->gpr[ra]._u32[0] + ((uint32_t)i10 << 4)) & SPU_LS_MASK & ~15u;
            ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x041 || op9 == 0x061) {                   /* stqa / lqa (abs) */
            /* I16 is a WORD address (LSA >> 2), as for every RI16 form -- it
             * was shifted by 4, so `lqa $r77,0xA40` in the WWS job manager's
             * interrupt entry read LS 0x2900 and the save stub it generates
             * from that word came out as garbage (0x549F6937 at 0x3FEC0). */
            uint32_t a = (((w >> 7) & 0xFFFF) << 2) & SPU_LS_MASK & ~15u;
            if (op9 == 0x041) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op9 == 0x047 || op9 == 0x067) {                   /* stqr / lqr (rel) */
            uint32_t a = (pc + ((uint32_t)i16 << 2)) & SPU_LS_MASK & ~15u;
            if (op9 == 0x047) spu_ls_write128(ctx, a, ctx->gpr[rt]);
            else              ctx->gpr[rt] = spu_ls_read128(ctx, a);
            pc += 4; continue;
        }
        if (op8 == 0x44) {                                    /* xori rt,ra,i10 */
            /* Runtime-generated job stubs toggle their own instruction words.
             * Use the signed RI10 immediate, just like statically lifted xori. */
            ctx->gpr[rt] = spu_xori(ctx->gpr[ra], i10);
            pc += 4; continue;
        }
        if (op8 == 0x1C) {                                    /* ai rt,ra,i10 */
            u128 r = ctx->gpr[ra];
            for (int k = 0; k < 4; k++) r._u32[k] += (uint32_t)i10;
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op11 == 0x040 || op11 == 0x0C0) {                 /* sf / a (word) */
            u128 x = ctx->gpr[ra], y = ctx->gpr[rb], r;
            for (int k = 0; k < 4; k++)
                r._u32[k] = (op11 == 0x040) ? y._u32[k] - x._u32[k]
                                            : x._u32[k] + y._u32[k];
            ctx->gpr[rt] = r; pc += 4; continue;
        }
        if (op9 == 0x081) { ctx->gpr[rt] = spu_splat_u32((uint32_t)i16); pc += 4; continue; }  /* il  */
        if (op9 == 0x082) { ctx->gpr[rt] = spu_splat_u32(((w >> 7) & 0xFFFF) << 16); pc += 4; continue; } /* ilhu */
        if (op9 == 0x0C1) { u128 r = ctx->gpr[rt];            /* iohl */
            for (int k = 0; k < 4; k++) r._u32[k] |= (w >> 7) & 0xFFFF;
            ctx->gpr[rt] = r; pc += 4; continue; }
        if (op7 == 0x21)  { ctx->gpr[rt] = spu_splat_u32((w >> 7) & 0x3FFFF); pc += 4; continue; } /* ila */
        if (op11 == 0x201 || op11 == 0x001) { pc += 4; continue; }  /* nop/lnop */
        if (op11 == 0x002 || op11 == 0x003) { pc += 4; continue; }  /* sync/dsync */
        /* Branch hints, all no-ops for execution: hbr is the 11-bit-opcode RR
         * form (0x1AC), hbra and hbrr are the 7-bit-opcode RI18 form (0x08,
         * 0x09) -- the same op7 group as ila (0x21) above, NOT op9. Testing
         * op9 here never matched either (hbrr 0x12033296 has op9 0x24), so a
         * runtime-generated stub carrying a branch hint decoded to UNKNOWN,
         * the microstep bailed, and the SPU fell into branch-to-0. */
        /* sagemono reached the same fix independently in #166, from a different
         * witness (hbrr 0x1200048C rather than 0x12033296). Same line, same
         * diagnosis, arrived at separately -- which is about as good as
         * corroboration gets for a decode bug. */
        if (op11 == 0x1AC || op7 == 0x08 || op7 == 0x09) { pc += 4; continue; } /* hbr/hbra/hbrr */

        /* Stepped onto lifted code: run that instead of interpreting it. A
         * job module is entered at its 16-byte GUID header -- four `ila`
         * the microstepper runs -- and falls through into its first lifted
         * function; the interpreter only rejoins lifted code at a BRANCH, so
         * handed this pc it interpreted the whole module. A Crack in Time's
         * job 43593402 calls the module it loads at 0x10D00 this way every
         * frame (its inner loop at 0x10D40). */
        if (spu_lookup_ctx(ctx, pc, ctx->image_id)) {
            ctx->pc = pc;
            g_spu_trampoline_fn = spu_indirect_branch;
            return 1;
        }

        /* Hand off to the real interpreter rather than giving up.
         *
         * This microstepper knows a dozen opcodes -- enough for the
         * runtime-generated stubs it was written for -- and spu_interp.c is a
         * full, selftested SPU interpreter that was already sitting here
         * unused on this path. Bailing on the first ordinary instruction is
         * strictly worse than running it.
         *
         * It matters for overlay code: a SPURS job policy module DMAs its job
         * bodies into local store and branches to them, so there is no lifted
         * body to find and the fallback IS the execution path. Guitar Hero
         * III`s job kernel dies here on an `ilh` at LS 0x6638 -- a perfectly
         * ordinary instruction the interpreter handles -- and with it the
         * decompression job it was running never completes. */
        { static int _n = 0;
          if ((_n < 8 && (_n++, 1)))
              fprintf(stderr, "[spu-smc] microstep img=%d pc=0x%05X word 0x%08X not in the"
                      " microstep set -- handing to the interpreter (steps=%d)\n",
                      ctx->image_id, pc, w, steps); }
        ctx->pc = pc;
        /* Stop at the drain's return point too: it sits mid-function, so it is
         * no lifted entry, and a job's return would otherwise run on through
         * the caller's code (GH3's song-freeze; see drain_ret_pc). */
        {
            uint32_t at = spu_interp_run_until(ctx, pc, ctx->drain_ret_pc);
            /* The interpreter hands back at the first LIFTED entry it reaches
             * (rejoin). Nothing dispatches that entry unless a transfer is
             * queued: the drain saw an empty trampoline and ended the run, so
             * the RaC WWS job manager's interrupt handler -- microstepped in at
             * LS 0xA2C, rejoining its own lifted loop at 0xA60 -- was abandoned
             * on every interrupt and the synth job never left kReadCommands.
             * Queue the rejoin; the drain's own return point is left to the
             * call bracket that is waiting for it. */
            if (at && ctx->status == SPU_STATUS_RUNNING &&
                (ctx->pc & SPU_LS_MASK) != (ctx->drain_ret_pc & SPU_LS_MASK))
                g_spu_trampoline_fn = spu_indirect_branch;
            return at ? 1 : 0;
        }
    }
    { static int _n = 0; if ((_n < 4 && (_n++, 1)))
        fprintf(stderr, "[spu-smc] microstep img=%d runaway (4096 steps from 0x%05X)\n",
                ctx->image_id, ctx->pc & SPU_LS_MASK); }
    return 0;
}

/* SPU_IRQFOLLOW=<img>: after that image takes an interrupt, log the next 300
 * indirect dispatches (pc, lr, int_enable, depth) -- where a handler goes. */
volatile int g_spu_irqfollow_n = 0;
/* A call through a function pointer into local store that holds nothing.
 * A Crack in Time's CollNarrow job (B8A980, GUID 43593402) calls its
 * shape-pair handlers through a table at LS 0x14F2C (index at 0x14F76), from
 * 0x0FCC8 (`bisl $0,$47`); a zero entry means "no handler" and the job skips
 * the call itself. In the owner's v44 Vorselon fight an entry pointed at
 * LS 0x18AE4, which held zeros: the empty word decodes as `stop 0`, the lane
 * halted mid-job, the job's completion never came and the main thread waited
 * for it forever (the freeze). Treat it the way the game treats a null
 * handler -- return to the caller -- and say so in the log with the bytes
 * around the call's argument, so the stale entry can be traced. Only for a
 * `bisl` whose target register holds this pc, below the stack area, with a
 * whole empty quadword there; SPU_EMPTY_CALL_HALT=1 restores the halt. */
static int spu_skip_call_into_empty_ls(spu_context* ctx, uint32_t p0)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPU_EMPTY_CALL_HALT"); on = !(e && *e == '1'); }
    if (!on || p0 + 16 > 0x3E000u) return 0;
    for (uint32_t k = 0; k < 16; k++) if (ctx->ls[p0 + k]) return 0;
    const uint32_t lr = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    if (lr < 4 || lr + 4 > SPU_LS_SIZE || lr - 4 == p0) return 0;
    const uint8_t* q = ctx->ls + lr - 4;
    const uint32_t c = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
    if ((c >> 21) != 0x1A9u || (c & 0x7Fu) != 0u) return 0;                    /* bisl $0,$ra */
    if ((ctx->gpr[(c >> 7) & 0x7Fu]._u32[0] & SPU_LS_MASK) != p0) return 0;
    static int told = 0;
    if (told < 16) {
        told++;
        fprintf(stderr, "[spu] img=%d: call from LS 0x%05X into empty local store 0x%05X skipped "
                "(as for a null handler; the lane would have halted and the game frozen) r3=0x%05X%c",
                ctx->image_id, lr - 4, p0, ctx->gpr[3]._u32[0] & SPU_LS_MASK, 10);
        const uint32_t a = ((ctx->gpr[3]._u32[0] & SPU_LS_MASK) & ~15u) >= 0x40u ? ((ctx->gpr[3]._u32[0] & SPU_LS_MASK) & ~15u) - 0x40u : 0u;
        for (uint32_t r = a; r < a + 0x80u && r + 16 <= SPU_LS_SIZE; r += 16) {
            fprintf(stderr, "      LS[0x%05X]:", r);
            for (uint32_t o = 0; o < 16; o += 4)
                fprintf(stderr, " %02X%02X%02X%02X", ctx->ls[r+o], ctx->ls[r+o+1], ctx->ls[r+o+2], ctx->ls[r+o+3]);
            fprintf(stderr, "%c", 10);
        }
        /* where the empty stretch begins and ends: what was meant to be there */
        uint32_t lo = p0, hi = p0;
        while (lo >= 16 && !ctx->ls[lo - 1]) lo--;
        while (hi < SPU_LS_SIZE && !ctx->ls[hi]) hi++;
        fprintf(stderr, "      empty LS 0x%05X-0x%05X%c", lo, hi, 10);
        fflush(stderr);
    }
    ctx->pc = lr;
    g_spu_trampoline_fn = 0;
    return 1;
}

void spu_indirect_branch(spu_context* ctx)
{
    { static int s_f = -2; if (s_f == -2) { const char* e = getenv("SPU_IRQFOLLOW"); s_f = e ? atoi(e) : -1; }
      if (s_f >= 0 && ctx->image_id == s_f && g_spu_irqfollow_n > 0 && g_spu_irqfollow_n < 300) {
          g_spu_irqfollow_n++;
          fprintf(stderr, "[irqf] pc=0x%05X lr=0x%05X int=%d depth=%u r1=0x%05X tramp_ret=0x%05X\n",
                  ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->int_enable,
                  ctx->host_depth, ctx->gpr[1]._u32[0], ctx->drain_ret_pc); } }
    /* Real SPU bi/bisl mask the target to the 256 KB local store; the high bits
     * of a computed pointer (e.g. a packed handle like 0x7a028803) are ignored.
     * Without this, any indirect branch through such a value fails the lookup
     * and falls into branch-to-0. All lifted funcs live below SPU_LS_SIZE, so
     * masking is a no-op for already-valid targets. */
    ctx->pc &= SPU_LS_MASK;
    { extern void spu_tr_note(spu_context*, char, uint32_t, uint32_t);
      spu_tr_note(ctx, 'B', ctx->drain_ret_pc, ctx->drain_ret_sp); }
    /* Interrupt-return register restore (see spu_drain.c spu_irq_regs_save):
     * the WWS handler's save/restore shim lives at unlifted top-of-LS, so we
     * enforce the preserve-all-registers hardware contract here -- the first
     * interrupts-enabled dispatch at the saved srr0 is the irete. */
    { extern int spu_irq_regs_maybe_restore(spu_context*);
      if (spu_irq_regs_maybe_restore(ctx)) {
          static int s_it = -1; if (s_it < 0) s_it = getenv("SPU_IRQTRACE") ? 1 : 0;
          static int _r = 0;
          if (s_it && (_r < 200 && (_r++, 1)))
              fprintf(stderr, "[irq] IRET restore at srr0=0x%05X\n", ctx->pc & SPU_LS_MASK);
      } }
    /* SPURS kernel services (policy-module runs only): the HLE kernel plants
     * these two reserved addresses as exitToKernelAddr / selectWorkloadAddr in
     * the SpursKernelContext (spurs_policy.c). */
    if (ctx->policy_mode) {
        extern _Thread_local volatile unsigned g_spurs_pm_polls; extern volatile unsigned g_spurs_pm_exited;
        if (ctx->pc == SPURS_PM_EXIT_TO_KERNEL_LS) {
            /* Module exit: the workload returned to the kernel (drained/yield).
             * Print gated: fires once per policy run = thousands/sec. */
            g_spurs_pm_exited = 1;
            { static int s_t = -1; if (s_t < 0) s_t = getenv("SPURS_PM_TRACE") ? 1 : 0;
              if (s_t) { static unsigned long _n = 0; unsigned long n = ++_n;
                if (n <= 64 || (n & 0xFFF) == 0)
                    fprintf(stderr, "[spurs-pm] exit-to-kernel#%lu (r3=0x%08X polls=%u)\n",
                            n, ctx->gpr[3]._u32[0], g_spurs_pm_polls); } }
            ctx->status = SPU_STATUS_STOPPED_BY_STOP;
            spu_halt(ctx);
            return;
        }
        if (ctx->pc == SPURS_PM_SELECT_WORKLOAD_LS) {
            /* cellSpursModulePoll: report "no contention — keep running".
             * (One virtual SPU per workload here, so nothing ever preempts.) */
            unsigned n = ++g_spurs_pm_polls;
            /* Capped per process, not per run: the counter restarts with every
             * policy run, and the WWS audio module runs ~1900 times a second,
             * so "the first 4 of each run" was a line per run in the
             * unbuffered log. */
            { static int _shown = 0;
              if (_shown < 8 && n <= 4) { _shown++;
                fprintf(stderr, "[spurs-pm] poll #%u (r3=0x%08X) -> continue\n",
                        n, ctx->gpr[3]._u32[0]); } }
            ctx->gpr[3] = spu_make_preferred_u32(0);
            ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
            return;
        }
    }
    /* Synthetic HLE tasks have no resident taskset policy at 0xA70. Real
     * tasksets use the SAME syscall address, so the API table alone is not
     * evidence that this is an HLE context. LLE tasks must enter Sony's policy
     * to yield/select workloads instead of parking an entire SPU host thread.
     * Image 22 is the legacy standalone HLE CRI task runner. */
    if (ctx->pc == SPURS_TASKSET_PM_SYSCALL_LS) {
        uint32_t sc = ((uint32_t)ctx->ls[0x27C4] << 24) | ((uint32_t)ctx->ls[0x27C5] << 16)
                    | ((uint32_t)ctx->ls[0x27C6] << 8)  | ctx->ls[0x27C7];
        /* A task dispatched standalone (no policy module in this local store)
         * runs on the SpursTasksetContext spu_workload.c plants, whose
         * syscallAddr is this address; nothing is lifted at 0xA70 for it, so
         * the only alternative to the HLE is the synthesised-stop exit below
         * -- which is how LBP's audio tasks (images 6/7) ended on their first
         * syscall and left the loading thread waiting on their event flag. */
        int lifted = spu_lookup(SPURS_TASKSET_PM_SYSCALL_LS, ctx->image_id) ||
            (ctx->resident_task && spu_lookup(SPURS_TASKSET_PM_SYSCALL_LS, ctx->resident_task)) ||
            (ctx->resident_ovl  && spu_lookup(SPURS_TASKSET_PM_SYSCALL_LS, ctx->resident_ovl));
        int standalone = !ctx->policy_mode && sc == SPURS_TASKSET_PM_SYSCALL_LS && !lifted;
        if (ctx->image_id == 22 || standalone ||
            (ctx->policy_mode && sc == SPURS_TASKSET_PM_SYSCALL_LS)) {
            spu_spurs_taskset_syscall(ctx);
            ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
            return;
        }
    }
    /* YDKJ_CRI_R4: the taskset policy entry (LS 0xA00, image 23) writes r4 into
     * SpursTasksetContext.taskset @LS 0x27B8 (per RPCS3 cellSpursSpu.cpp). Our
     * kernel->policy handoff doesn't convey the taskset EA, so the policy DMAs
     * the taskset from garbage -> waiting!=0 -> wrong resume path -> savedContextLr=0.
     * Inject r4 = taskset EA (0x0F000000) at the policy entry dispatch (this is the
     * exact point before the entry reads r4, after the kernel's arg setup). */
    if (ctx->image_id == SPU_RESOLVER_ONLY_IMAGE_ID && !getenv("YDKJ_NO_CRI_R4")) {
        static int s_r4 = -1; if (s_r4 < 0) s_r4 = getenv("YDKJ_CRI_CHAIN") ? 1 : 0;
        if (s_r4) {
            /* Force ctxt->taskset @LS 0x27B8 = the REAL game taskset EA on every
             * image-23 branch, so the policy's atomic reads + context-EA computation
             * use the actual taskset (the r4 handoff sets it to garbage 0x0000FFFF via
             * a path we can't intercept). Was hardcoded 0x0F000000, which mismatched
             * the game's real taskset (0x45F1B000) -> policy DMA'd garbage. */
            extern uint32_t g_ydkj_real_taskset_ea;
            uint32_t ts = g_ydkj_real_taskset_ea ? g_ydkj_real_taskset_ea : 0x0F000000u;
            ctx->ls[0x27B8]=0x00; ctx->ls[0x27B9]=0x00; ctx->ls[0x27BA]=0x00; ctx->ls[0x27BB]=0x00;
            ctx->ls[0x27BC]=(uint8_t)(ts>>24); ctx->ls[0x27BD]=(uint8_t)(ts>>16); ctx->ls[0x27BE]=(uint8_t)(ts>>8); ctx->ls[0x27BF]=(uint8_t)ts;
            if (ctx->pc == 0xA00u) { static int _n=0; if ((_n < 4 && (_n++, 1)))
                fprintf(stderr, "[cri-r4] policy entry pc=0xA00: forced ctxt->taskset LS[0x27B8]=0x0F000000\n"); }
        }
    }
    /* SPU_IBCOV: image-3 (Bink SPU) PC-page coverage. Track which 0x1000-byte LS
     * pages the task's indirect branches land in; dump the set periodically. If
     * coverage stays in the kernel/wait region (~0x13xxx) the decode routine never
     * runs; if it spans a wide high range, decode executes but doesn't output. */
    { static int s_cov = -1; if (s_cov < 0) s_cov = getenv("SPU_IBCOV") ? 1 : 0;
      if (s_cov && ctx->image_id == 3) {
        static uint8_t pages[64] = {0};   /* 64 pages * 0x1000 = 256KB LS */
        static uint64_t hits = 0;
        uint32_t pg = (ctx->pc & 0x3FFFF) >> 12;
        int newp = 0;
        if (pg < 64 && !pages[pg]) { pages[pg] = 1; newp = 1; }
        ++hits;
        if (newp || (hits % 200000) == 0) {
            char line[400]; int p = snprintf(line, sizeof line, "[ibcov3] hits=%llu tgt=0x%05X pages:", (unsigned long long)hits, ctx->pc & 0x3FFFF);
            for (int i = 0; i < 64; i++) if (pages[i]) p += snprintf(line+p, sizeof(line)-p, " 0x%X", i<<12);
            fprintf(stderr, "%s\n", line); }
      } }
    { static int s_t = -1; if (s_t < 0) s_t = getenv("SPU_POLLTRACE") ? 1 : 0;
      if (s_t) { static uint64_t s_n = 0; static uint32_t s_last = 0; static uint64_t s_run = 0;
        if (ctx->pc == s_last) s_run++; else { s_last = ctx->pc; s_run = 1; }
        if ((++s_n % 2000000) == 0)
          fprintf(stderr, "[ibranch] %llu indirect branches; current target=0x%05X run=%llu\n",
                  (unsigned long long)s_n, ctx->pc, (unsigned long long)s_run); } }
    /* Policy-entry trace: the SPURS policy at LS 0xA00 branches on
     * r8 = word at LS[r6] (must be 32 for the path that sets the dispatch ptr
     * LS[0x780]). Log r3..r6 + the word the kernel handed it, to see why the
     * wrong branch is taken. Env YDKJ_POLTRACE. */
    if (ctx->pc == 0xA00u) {
        static int64_t pt=-2; if (pt==-2){ const char* e=getenv("YDKJ_POLTRACE"); pt=e?1:0; }
        if (pt) { static int _p=0; if ((_p < 8 && (_p++, 1))) {
            /* re-lifted policy entry uses r80 (kernel-set context base): r44=LS[r80+0xC0] */
            uint32_t r80=ctx->gpr[80]._u32[0] & SPU_LS_MASK;
            const uint8_t* q = ctx->ls + ((r80+0xC0)&SPU_LS_MASK);
            uint32_t ctxw = ((uint32_t)q[0]<<24)|((uint32_t)q[1]<<16)|((uint32_t)q[2]<<8)|q[3];
            fprintf(stderr, "[POLTRACE] policy@0xA00 r3=%08X r4=%08X r80=%08X  LS[r80+0xC0]=%08X\n",
                ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], r80, ctxw);
            fflush(stderr);
        } }
    }
    /* Resolve both fresh launches and mid-function resumes from TaskInfo.
     * A scheduler call temporarily leaves the task region, but its return PC
     * is usually not an ELF entry. Prefer the policy's selected ELF so tasks
     * sharing an entry address cannot shadow one another. Keep the legacy
     * entry-only mapping for runners that have not registered ELF metadata. */
    if (ctx->pc < SPU_TASKSET_TASK_LO) {
        ctx->resident_task = 0;
    } else {
        int ti = spu_taskset_resident_image(ctx);
        if (ti && !ctx->resident_task && ctx->host_depth &&
            s_spu_halt_armed && !ctx->policy_mode) {
            /* The policy restored a task's guest registers and branches to
             * its saved PC. Its host frames still belong to the scheduler or
             * an earlier task invocation; none can satisfy this task's return.
             * Resume at depth zero so SPU_RET follows the restored guest link. */
            ctx->resident_task = ti;
            g_spu_trampoline_fn = 0;
            longjmp(s_spu_halt_env, 2);
        }
        if (!ti && !ctx->resident_task) ti = spu_taskset_task_image(ctx->pc);
        if (ti) ctx->resident_task = ti;
    }
    /* The launched task owns the shared task-code region: resolve it there FIRST
     * so a co-resident task at the same LS base cannot shadow it. (resident_task
     * is 0 outside the region, so this only ever fires for genuine task code.) */
    spu_fn fn = NULL;
    int code_owner = 0;
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (ctx->resident_code[slot].image_id && ctx->pc >= ctx->resident_code[slot].lsa &&
            ctx->pc - ctx->resident_code[slot].lsa < ctx->resident_code[slot].size) {
            code_owner = ctx->resident_code[slot].image_id;
            fn = spu_lookup(ctx->pc, code_owner);
            break;
        }
    }
    if (!code_owner && ctx->resident_task)
        fn = spu_lookup(ctx->pc, ctx->resident_task);
    /* Resident overlay next: streamed code overwrote that LS range, so its lift
     * is the truth there -- the base image's stale bytes at the same addresses
     * may also be registered (historical junk lifts) and must lose. */
    if (!code_owner && !fn && ctx->resident_ovl) fn = spu_lookup(ctx->pc, ctx->resident_ovl);
    if (!code_owner && !fn) fn = spu_lookup_ctx(ctx, ctx->pc, ctx->image_id);
    /* Inside a resident task's code region with no lift of its own there: a
     * module the task loaded into its region (A Crack in Time's job 43593402
     * loads one at 0x10D00 and runs its inner loop there). Content-matched
     * overlays verify their bytes against local store, so they are safe to
     * try; without this every such module ran in the interpreter -- the
     * biggest single cost of Zolar Forest while Ratchet moved. */
    if (code_owner && !fn) fn = spu_lookup_ctx(ctx, ctx->pc, ctx->image_id);
    /* The job returned through the link register we planted: it is finished.
     * Its outermost frame ends in `bi $r0`, and r0 was 0 -- so without this the
     * return landed on LS 0, which is the job's OWN entry, and it ran a second
     * lap with dead registers: re-reading its parameters through a now-zero
     * context pointer and spinning on a DMA to what were really its own
     * opcodes. The work is already done by the time it returns. */
    if (!ctx->policy_mode && ctx->pc == SPU_JOB_RETURN_LS) {
        static int _n = 0;
        if ((_n < 4 && (_n++, 1)))
            fprintf(stderr, "[spurs-job] img=%d returned to the job manager \n",
                    ctx->image_id);
        spu_halt(ctx);
        return;
    }
    /* Branch into high LS with no lifted code there.
     *
     * This was added believing the jobs called a resident SPURS job-manager
     * kernel at 0x16100 / 0x18160 / 0x1A100. That was WRONG. Those "absolute
     * branches" were the disassembler decoding ASCII as instructions: the
     * bytes behind one of them are 30 2C 20 43, i.e. "0, C" from the string
     * "ch0, F:0:400, 100, Channel 0 level, %". Every job embeds the same
     * parameter-description text, which is why the same phantom target
     * appeared in 9 of 12 unrelated images and looked like a shared ABI.
     *
     * The guard is kept because branching into unlifted high local store is
     * still an error worth stopping at rather than executing whatever is
     * there -- but it is a backstop, not a kernel interface. */
    /* SPU exit: the CRT does not branch to a `stop` in the image -- it BUILDS one
     * in memory and jumps to it. ps1_netemu's GPU core ends with
     *
     *     ori $r80,$r3,0 / andi $r80,$r80,255 / iohl $r80,0x2000
     *     stqd $r80,0x10($r1) / sync / ai $r3,$r1,16 / bi $r3
     *
     * i.e. `stop (0x2000 | status)` assembled onto the stack and executed there.
     * That target is a stack address with no lifted code, so the unlifted-branch
     * guard below called a clean exit "branched into unlifted LS 0x3FFB0 -- ending
     * the job" and reported stop_code 0, which reads as a runaway and sent a long
     * chase after phantom memory corruption. Decode the target word instead: a
     * `stop` is opcode 0 in the top 11 bits, so this is unambiguous. */
    if (!fn) {
        uint32_t p0 = ctx->pc & SPU_LS_MASK;
        if (p0 + 3 < SPU_LS_SIZE) {
            uint32_t w = ((uint32_t)ctx->ls[p0] << 24) | ((uint32_t)ctx->ls[p0+1] << 16) |
                         ((uint32_t)ctx->ls[p0+2] << 8) | ctx->ls[p0+3];
            if ((w >> 21) == 0u) {                 /* stop / stopd */
                if (w == 0u && spu_skip_call_into_empty_ls(ctx, p0)) return;
                ctx->stop_code = w & 0x3FFFu;
                static int _n = 0;
                if ((_n < 8 && (_n++, 1)))
                    fprintf(stderr, "[spu] img=%d exit: synthesised stop 0x%04X at LS 0x%05X\n",
                            ctx->image_id, ctx->stop_code, p0);
                { extern void spu_tr_dump(spu_context*); spu_tr_dump(ctx); }
                spu_halt(ctx);
                return;
            }
        }
    }

    if (!fn && !ctx->policy_mode && ctx->pc >= SPU_JM2_KERNEL_BASE) {
        static uint32_t seen[16]; static int n_seen = 0;
        int known = 0;
        for (int i = 0; i < n_seen; i++) if (seen[i] == ctx->pc) { known = 1; break; }
        if (!known && n_seen < 16) {
            seen[n_seen++] = ctx->pc;
            fprintf(stderr, "[spu] img=%d branched into unlifted LS 0x%05X "
                    "(lr=0x%05X) -- ending the job\n",
                    ctx->image_id, ctx->pc, ctx->gpr[0]._u32[0] & SPU_LS_MASK);
            { fprintf(stderr, "      last dispatched PCs (oldest first):");
              for (unsigned q = 0; q < 8; q++) {
                  unsigned idx = (g_spu_pch_n + q) & 7u;
                  if (g_spu_pch_n > q || g_spu_pch[idx]) fprintf(stderr, " 0x%05X", g_spu_pch[idx]);
              }
              fprintf(stderr, "%c", 10); }
            /* Is there real code at the target, or is the pc garbage? Eight
             * words at the target and at the return address separate "the lift
             * missed a function" from "this branch should never have happened". */
            { uint32_t a[2]; a[0] = ctx->pc & SPU_LS_MASK;
              a[1] = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
              for (int k = 0; k < 2; k++) {
                  fprintf(stderr, "      LS[0x%05X]:", a[k]);
                  for (uint32_t o = 0; o < 32 && a[k] + o + 3 < SPU_LS_SIZE; o += 4)
                      fprintf(stderr, " %02X%02X%02X%02X",
                              ctx->ls[a[k]+o], ctx->ls[a[k]+o+1],
                              ctx->ls[a[k]+o+2], ctx->ls[a[k]+o+3]);
                  fprintf(stderr, "\n");
              } }
            fflush(stderr);
        }
        /* SPU_INTERP_UNLIFTED=1: if real code is present at the target, run it
         * through the interpreter instead of ending the job. No lift can exist
         * for a module the title loads at runtime -- You Don't Know Jack's FMOD
         * mixer relocates a DSP plugin into local store and calls it -- and the
         * interpreter rejoins the compiled path as soon as it reaches a lifted
         * address, which is what the plugin's return does. Opt-in so titles
         * that reach here on a genuinely bad pc keep the loud stop. */
        { uint32_t p = ctx->pc & SPU_LS_MASK;
          uint32_t w0 = ((uint32_t)ctx->ls[p] << 24) | ((uint32_t)ctx->ls[p+1] << 16) |
                        ((uint32_t)ctx->ls[p+2] << 8) | ctx->ls[p+3];
          static int s_iu = -1;
          if (s_iu < 0) { const char* e = getenv("SPU_INTERP_UNLIFTED"); s_iu = e ? 1 : 0; }
          if (s_iu && w0) {
              static int _n = 0;
              if ((_n < 8 && (_n++, 1)))
                  fprintf(stderr, "[spu] img=%d interpreting unlifted LS 0x%05X "
                          "(runtime-loaded code)\n", ctx->image_id, p);
              spu_interp_run(ctx, p);
              return;
          } }
        spu_halt(ctx);
        return;
    }
    /* WWS job-code overlay match: the PM (image 2) streams a job module into the
     * code buffer (base = LS[0x1320]) and branches to base+entryOffset. Its 16-byte
     * ila header identifies which of the 89 lifted job modules it is; match it and
     * mark that overlay resident so the module's lifted functions dispatch. The
     * module arrives via a DMA LIST, so the single-GET note_get hook never fired. */
    if (!fn && ctx->image_id == 2 && ctx->pc >= 0x4000 && ctx->pc < 0x3E000) {
        uint32_t codeBase = ((uint32_t)ctx->ls[0x1320] << 24) | ((uint32_t)ctx->ls[0x1321] << 16) |
                            ((uint32_t)ctx->ls[0x1322] << 8) | ctx->ls[0x1323];
        if (codeBase + 16 <= SPU_LS_SIZE) {
            extern int spu_overlay_match_sig(const uint8_t hdr[16]);
            int img = spu_overlay_match_sig(&ctx->ls[codeBase]);
            if (img) {
                if (ctx->resident_ovl != img) {
                    ctx->resident_ovl = img;
                    static int _n = 0; if ((_n < 32 && (_n++, 1)))
                        fprintf(stderr, "[spu-ovl] WWS job module resident: codeBase=0x%05X "
                                "pc=0x%05X -> overlay image %d\n", codeBase, ctx->pc & SPU_LS_MASK, img);
                }
                fn = spu_lookup(ctx->pc, img);
            }
        }
    }
    /* WWS job-code fallback: the jobmanager PM (image 2) stages each job's
     * code into an LS buffer and branches into it. The staged blobs are
     * lifted as their own images at their LS residence addresses (image 1 =
     * job_wws_7BD900 at the 0x14400-region buffer); the PM's own table can
     * never contain them. On a policy-module miss inside the job-buffer
     * range, retry against the job image before falling to the interpreter. */
    /* WWS job-code fallback -- but ONLY when real code is staged at the target.
     * A PM job whose code buffer is empty (the {0xA400,0} null-code placeholder)
     * must NOT resolve to image-1's unrelated function that happens to share the
     * file offset: that ran garbage, the job did no work, and its ring slot
     * never advanced -> the loader's job ring deadlocked. An empty buffer falls
     * through to the null-job handling below. */
    if (!fn && ctx->image_id == 2 && ctx->policy_mode &&
        ctx->pc >= 0x4000 && ctx->pc < 0x3E000) {
        uint32_t w0 = ((uint32_t)ctx->ls[ctx->pc] << 24) | ((uint32_t)ctx->ls[ctx->pc+1] << 16) |
                      ((uint32_t)ctx->ls[ctx->pc+2] << 8) | ctx->ls[ctx->pc+3];
        if (w0 != 0)
            fn = spu_lookup(ctx->pc, 1);
    }
    /* Null-code PM job (empty buffer): return control to the dispatcher's own
     * host-call bracket so its post-job continuation runs the completion
     * bookkeeping -- exactly as a real job that did nothing and returned.
     * (Falling to the SMC microstep would log a spurious BRANCH-TO-0 on the
     * 0x00000000 word; this is the same outcome, quiet.) */
    if (!fn && ctx->image_id == 2 && ctx->policy_mode &&
        ctx->pc >= 0x4000 && ctx->pc < 0x3E000 &&
        ((uint32_t)ctx->ls[ctx->pc] | ctx->ls[ctx->pc+1] |
         ctx->ls[ctx->pc+2] | ctx->ls[ctx->pc+3]) == 0) {
        static int _n = 0;
        if ((_n < 8 && (_n++, 1))) {
            /* Ground-truth dump of the WWS code-buffer resolution at dispatch
             * (wwsjob-rosetta offsets). jobEntry = lsaJobCodeBuffer + [buf+0x10];
             * lsaJobCodeBuffer = Gbt_bufferPageNum<<10 from the RunJob command.
             * If the code buffer really is 0x4000 but empty -> the code LOAD is
             * the bug; if lsaJobCodeBuffer should be 0x14400 -> the RunJob
             * buffer-set resolution is. Dump both + the bufferSetArray rows. */
            uint32_t lsaCode = (uint32_t)ctx->ls[0x1330]<<24 | ctx->ls[0x1331]<<16 |
                               ctx->ls[0x1332]<<8 | ctx->ls[0x1333];
            uint32_t runJob  = (uint32_t)ctx->ls[0x12E0]<<24 | ctx->ls[0x12E1]<<16 |
                               ctx->ls[0x12E2]<<8 | ctx->ls[0x12E3];
            const uint8_t* bs = ctx->ls + 0xDF0;   /* g_WwsJob_bufferSetArray */
            fprintf(stderr, "[pm-jobres] pc=0x%05X lsaJobCodeBuffer(0x1330)=0x%08X "
                    "runJobNum(0x12E0)=0x%08X code@14400:%02X%02X%02X%02X\n"
                    "           bufferSetArray[0xDF0]: %02X%02X%02X%02X %02X%02X%02X%02X "
                    "%02X%02X%02X%02X %02X%02X%02X%02X\n",
                    ctx->pc & SPU_LS_MASK, lsaCode, runJob,
                    ctx->ls[0x14400],ctx->ls[0x14401],ctx->ls[0x14402],ctx->ls[0x14403],
                    bs[0],bs[1],bs[2],bs[3], bs[4],bs[5],bs[6],bs[7],
                    bs[8],bs[9],bs[10],bs[11], bs[12],bs[13],bs[14],bs[15]);
        }
        return;   /* enclosing SPU_DRAIN bracket resumes the dispatcher */
    }
    /* JOB-BODY EXECUTION probe: the WWS job manager enters a staged job via
     * `bie pJobCode` (changeloadtorunjob.spu), an INDIRECT branch to
     * lsaJobCodeBuffer + entryOffset. A successful dispatch with the pc inside
     * the job code buffer and a jobmod overlay resident (image ids 200+) is the
     * proof the lifted job module actually runs. Env SPU_JOBEXEC=1. */
    /* SPU_JOBTRACE=1: log EVERY dispatcher entry while a job module (overlay
     * id >= 200) is resident -- the job's cross-function control-flow trail.
     * The pm-flow drain hook can't see a job that performs no channel ops
     * (LBP's frozen job runs as a complete no-op: entry, no DMAs, return);
     * this names its early-out path instead. */
    { static int s_jt = -1;
      if (s_jt < 0) { const char* e = getenv("SPU_JOBTRACE"); s_jt = e ? 1 : 0; }
      if (s_jt && ctx->resident_ovl >= 200) { static int _n = 0;
        uint32_t p = ctx->pc & SPU_LS_MASK;
        /* Full trail, but only start logging once the job body proper begins
         * (pc in the job code buffer >= 0x4000) so the cap isn't spent on the
         * PM code the job calls back into -- and keep logging thereafter. */
        static int _armed = 0;
        if (p >= 0x4000) _armed = 1;
        if (_armed && (_n < 1500 && (_n++, 1))) {
            fprintf(stderr, "[jobtrace] pc=0x%05X lr=0x%05X%s",
                    p, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                    fn ? "" : " (no lift)");
            /* JobApi entries: args r3-r5. Post-job return 0x3308: result r3 +
             * the job's saved GetBufferTag result r80. */
            if (p == 0x2F30 || p == 0x1700 || p == 0x1770 || p == 0x17C8)
                fprintf(stderr, " args r3=%08X.%08X r4=%08X r5=%08X",
                        ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
                        ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0]);
            if (p == 0x3308 || p == 0x3258)
                fprintf(stderr, " r3=%08X.%08X.%08X.%08X r80=%08X.%08X",
                        ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1],
                        ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3],
                        ctx->gpr[80]._u32[0], ctx->gpr[80]._u32[1]);
            /* Mid-GetBufferTag: r8 = the set/buf id bytes computed at 0x2F30
             * entry; if wrong here, something clobbered it ACROSS the
             * GetLogicalBuffer call (interrupt delivery?). */
            if (p == 0x2F6C)
                fprintf(stderr, " r8=%08X r10=%08X r13=%08X r15=%08X int=%d",
                        ctx->gpr[8]._u32[0], ctx->gpr[10]._u32[0],
                        ctx->gpr[13]._u32[0], ctx->gpr[15]._u32[0],
                        ctx->int_enable);
            if (p == 0x3030)   /* final BufferTag in r7 (result) */
                fprintf(stderr, " BufferTag r7=%08X.%08X",
                        ctx->gpr[7]._u32[0], ctx->gpr[7]._u32[1]);
            fprintf(stderr, "\n");
        }
      } }
    /* LBP_HLE_JOBDONE=1: HLE the WWS job COMPLETION. Our lifted SPURS PM never
     * reaches the store/DecrementDependency phase (the multi-SPU barrier never
     * converges), so a job's completion word -- passed by the PPU as param[4],
     * an EA in the 0x470Axxxx pool, embedded in the job's command list at LS
     * ~0xC00 -- is never written, and the PPU JobManagerWorker (guest-fn
     * 0x521BD4) spins on it forever (loading freeze). When a job body actually
     * dispatches (overlay >=200, entry 0x4A40), scan its staged command list
     * for the completion EA and write it nonzero, unblocking the worker. This
     * fakes ONLY the done-signal (not the job's data DMA), so it tests how far
     * the boot gets once loading stops deadlocking. */
    if (ctx->pc == 0x4A40 && ctx->resident_ovl >= 200) {
        static int s_hd = -1;
        if (s_hd < 0) { const char* e = getenv("LBP_HLE_JOBDONE"); s_hd = e ? 1 : 0; }
        if (s_hd) {
            extern uint8_t* vm_base;
            int wrote = 0;
            for (uint32_t o = 0xC00; o + 4 <= 0xE80 && wrote < 4; o += 4) {
                uint32_t w = (ctx->ls[o]<<24)|(ctx->ls[o+1]<<16)|(ctx->ls[o+2]<<8)|ctx->ls[o+3];
                if ((w & 0xFFFF0000u) == 0x470A0000u && vm_base) {
                    /* write nonzero to the completion word in main RAM (BE) */
                    if (vm_base[w]==0 && vm_base[w+1]==0 && vm_base[w+2]==0 && vm_base[w+3]==0) {
                        vm_base[w+3] = 1;
                        static int _n = 0; if ((_n < 24 && (_n++, 1)))
                            fprintf(stderr, "[hle-jobdone] wrote completion 0x%08X=1 (job overlay %d)\n",
                                    w, ctx->resident_ovl);
                        wrote++;
                    }
                }
            }
        }
    }
    if (fn && ctx->pc >= 0x4000 && ctx->resident_ovl >= 200) {
        static int s_je = -1;
        if (s_je < 0) { const char* e = getenv("SPU_JOBEXEC"); s_je = e ? 1 : 0; }
        if (s_je) { static int _n = 0; if ((_n < 24 && (_n++, 1)))
            fprintf(stderr, "[jobexec] DISPATCH job code pc=0x%05X overlay=%d "
                    "runJobNum=0x%02X%02X%02X%02X\n",
                    ctx->pc & SPU_LS_MASK, ctx->resident_ovl,
                    ctx->ls[0x12E0], ctx->ls[0x12E1], ctx->ls[0x12E2], ctx->ls[0x12E3]);
          /* SPU_JOBEXEC_DUMP=<path>: write the full 256KB LS the first time a job
           * entry (pc==codeBuffer+entryOffset) dispatches, to diff vs the RPCS3
           * oracle SPU4 dump (why our job bails where the oracle's runs). */
          static int s_dumped = 0;
          if (!s_dumped && ctx->pc == 0x4A40) {
              const char* dp = getenv("SPU_JOBEXEC_DUMP");
              if (dp) { FILE* f = fopen(dp, "wb");
                  if (f) { fwrite(ctx->ls, 1, SPU_LS_SIZE, f); fclose(f);
                      fprintf(stderr, "[jobexec] dumped LS at job entry -> %s\n", dp); }
                  s_dumped = 1; } } }
    }
    if (fn) {
        spu_check_stack_reset(ctx, fn);
        /* MUSTTAIL: a guest loop that iterates through an indirect branch (the
         * Bink decoder's per-command dispatch does) must not grow the host
         * stack -- a plain call here leaked a resolver+callee frame per
         * iteration and blew the thread stack ~4k iterations into the first
         * really-decoding movie frame (silent 0x80000001 death). */
#if defined(__clang__)
        __attribute__((musttail)) return fn(ctx);
#else
        fn(ctx);
        return;
#endif
    }
    /* wwsjob JOB-CODE entry probe: the PM stages each job's code into a
     * buffer above its static image (0x3700..) and branches into it. No lift
     * exists at those pcs (the code arrives at runtime), so the branch lands
     * here. Log the entry + leading bytes -- the bytes identify WHICH job
     * blob was staged (match against lifted job images for dispatch). */
    /* Staged-job "return to kernel": a jm2-protocol job binary ends by
     * jumping to LS 0 (the jm2 kernel's home on real hardware). Inside the
     * wwsjob PM's LS, address 0 holds the interrupt VECTOR instead -- letting
     * the jump proceed runs the interrupt handler as the job's continuation
     * and wedges the pipeline (lanes freeze mid-queue). The PM called the job
     * with a return link in r0 (0x31C8 sets 0x3258); route the kernel-return
     * there: the PM resumes its post-job flow, which is exactly what the jm2
     * kernel hand-back does on hardware. */
    if (ctx->image_id == 2 && ctx->policy_mode && ctx->pc == 0 &&
        (ctx->gpr[0]._u32[0] & SPU_LS_MASK) >= 0xA00 &&
        (ctx->gpr[0]._u32[0] & SPU_LS_MASK) < 0x3700) {
        static int _n = 0;
        if ((_n < 8 && (_n++, 1)))
            fprintf(stderr, "[wws-jobret] staged job returned to kernel (LS 0); "
                    "resuming PM at link 0x%05X\n", ctx->gpr[0]._u32[0] & SPU_LS_MASK);
        ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
        spu_indirect_branch(ctx);      /* re-dispatch at the rewritten pc */
        return;
    }

    /* A SPURS *job* that branches back to LS 0 has RETURNED TO THE JOB MANAGER.
     * That is how a jm2 job signals completion: its crt tail-jumps to the
     * resident manager, which on hardware loads the next job and sets up r3/r4
     * afresh. We load the job at LS 0 and have no manager, so the jump lands on
     * the job's OWN entry and it runs a second lap -- with dead registers.
     *
     * Tokyo Jungle showed exactly that: lap one is correct at every step and
     * does the real work, then the crt jumps to 0 and lap two re-reads its
     * parameters through a now-zero context pointer (hence the reads of LS 0x20
     * and the DMA addresses that were really its own opcodes) and spins forever.
     * The work was already finished before the jump; the second lap is noise.
     *
     * The initial entry is called directly, not through here, so any arrival at
     * LS 0 in this path is a re-entry. Treat it as job end. */
    if (!ctx->policy_mode && ctx->pc == 0 && ctx->image_id > 0) {
        static int _n = 0;
        if ((_n < 8 && (_n++, 1)))
            fprintf(stderr, "[spurs-job] img=%d returned to the job manager "
                    "(branch to LS 0) -- job complete\n", ctx->image_id);
        spu_halt(ctx);
        return;
    }
    if (ctx->image_id == 2 &&
        ((ctx->pc >= 0x3700 && ctx->pc < 0x3FE80) ||
         (ctx->pc < 0xA00 && ctx->pc != SPURS_PM_EXIT_TO_KERNEL_LS &&
          ctx->pc != SPURS_PM_SELECT_WORKLOAD_LS))) {
        static int _n = 0;
        if (_n < 12) {
            _n++;
            const uint8_t* p = ctx->ls + (ctx->pc & SPU_LS_MASK);
            fprintf(stderr, "[wws-jobentry] pc=0x%05X lr=0x%05X bytes:"
                    " %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
                    ctx->pc & SPU_LS_MASK, ctx->gpr[0]._u32[0] & SPU_LS_MASK,
                    p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                    p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
            /* Companion state: the OTHER load target (the real 0x100-byte
             * loads land at 0x14400 -- possibly the actual job code, with the
             * entry computed from the wrong slot's base) + the job records
             * the entry math reads (0x1320 base block, 0x1440 batch record). */
            { const uint8_t* q = ctx->ls + 0x14400;
              const uint8_t* r1 = ctx->ls + 0x1320;
              const uint8_t* r2 = ctx->ls + 0x1440;
              fprintf(stderr, "[wws-jobentry]   LS14400: %02X%02X%02X%02X %02X%02X%02X%02X"
                      "  LS1320: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X"
                      "  LS1440: %02X%02X%02X%02X %02X%02X%02X%02X\n",
                      q[0],q[1],q[2],q[3], q[4],q[5],q[6],q[7],
                      r1[0],r1[1],r1[2],r1[3], r1[4],r1[5],r1[6],r1[7],
                      r1[8],r1[9],r1[10],r1[11], r1[12],r1[13],r1[14],r1[15],
                      r2[0],r2[1],r2[2],r2[3], r2[4],r2[5],r2[6],r2[7]); }
            fflush(stderr);
        }
    }
    /* No lifted function at this PC: it may be RUNTIME-GENERATED code (the
     * WWS jobmanager writes a save/restore stub above its static
     * image and calls it). Interpret the live LS bytes; on success the next
     * branch re-enters lifted code via the trampoline. */
    if (spu_smc_microstep(ctx))
        return;
    /* Cap the unresolved-branch log PER IMAGE: a global cap let one noisy
     * image (the FMOD mixer's overlay calls) exhaust it and silently hide
     * every other image's misses -- LBP's loading jobs skipped their command
     * handlers for a whole session without a single log line. */
    { enum { BT0_MAX_IMG = 1024, BT0_PER_IMG = 12 };   /* image ids run to 543 */
      static int _bt0[BT0_MAX_IMG];
      unsigned img = (ctx->image_id >= 0 && ctx->image_id < BT0_MAX_IMG)
                     ? (unsigned)ctx->image_id : 0;
      if (_bt0[img]++ < BT0_PER_IMG)
        fprintf(stderr, "[SPU] BRANCH-TO-0 unresolved pc=0x%05X image=%d lr=0x%05X\n",
                ctx->pc, ctx->image_id, ctx->gpr[0]._u32[0] & SPU_LS_MASK); }
    { extern void spu_tr_dump(spu_context*); spu_tr_dump(ctx); }
    /* One-shot: the FMOD null-handler DSP node carries a PPU descriptor EA at
     * node+0x14 (observed 0x93C3C0). Dump it to identify which plugin/unit
     * type never got its SPU code streamed (env SPU_DSPDESC=<hex ea>). */
    { static int _d = -1; static uint32_t _ea = 0;
      if (_d < 0) { const char* e = getenv("SPU_DSPDESC");
        _ea = e ? (uint32_t)strtoul(e, 0, 16) : 0; _d = _ea ? 1 : 0; }
      if (_d == 1) { _d = 2;
        extern uint8_t* vm_base;
        fprintf(stderr, "[dspdesc] RAM[0x%08X]:", _ea);
        for (int k = 0; k < 0x60; k += 4)
            fprintf(stderr, " %02X%02X%02X%02X", vm_base[_ea+k], vm_base[_ea+k+1],
                    vm_base[_ea+k+2], vm_base[_ea+k+3]);
        fprintf(stderr, "\n"); fflush(stderr); } }
    /* SPU_MISS_DUMP_IMG=<n>: reserve the deep-dump budget for image n's misses
     * (the global 2-shot budget was always consumed by an earlier image's
     * misses, hiding the one under investigation). Unset = old behavior. */
    { static int s_img = -2; static uint32_t s_pcmin = 0;
      if (s_img == -2) { const char* e = getenv("SPU_MISS_DUMP_IMG"); s_img = e ? atoi(e) : -1;
        const char* p = getenv("SPU_MISS_DUMP_PCMIN"); s_pcmin = p ? (uint32_t)strtoul(p,0,0) : 0; }
      static int _n=0;
      uint32_t misspc = ctx->pc & SPU_LS_MASK;
      if ((s_img < 0 || ctx->image_id == s_img) && misspc >= s_pcmin && (_n < 2 && (_n++, 1))) {
        fprintf(stderr, "[SPU] branch-to-0 lr=0x%05X r1=0x%05X\n",
                ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->gpr[1]._u32[0] & SPU_LS_MASK);
#ifdef _WIN32
        void* frames[24]; unsigned short fn = RtlCaptureStackBackTrace(0, 24, frames, NULL);
        char* base = (char*)GetModuleHandleA(NULL);
        fprintf(stderr, "[SPU] host bt RVAs:");
        for (unsigned short i = 0; i < fn; i++)
            fprintf(stderr, " 0x%zX", (size_t)((char*)frames[i] - base));
        fprintf(stderr, "\n");
#endif
        /* State-diff oracle: dump full LS + all GPRs at the branch-to-0 so it can
         * be compared byte-for-byte against the RPCS3 savestate LS of the same
         * (cri_mpv) task. Path from YDKJ_SPU_LSDUMP, else ./recomp_spu_ls.bin. */
        const char* dp = getenv("YDKJ_SPU_LSDUMP");
        if (!dp || !*dp) dp = "recomp_spu_ls.bin";
        FILE* lf = fopen(dp, "wb");
        if (lf) { fwrite(ctx->ls, 1, SPU_LS_SIZE, lf); fclose(lf);
                  fprintf(stderr, "[SPU] dumped 256KB LS -> %s\n", dp); }
        fprintf(stderr, "[SPU] image_id=%d  GPR dump (r0..r127, hi64:lo64 of each quadword, preferred slot = _u32[0]):\n", ctx->image_id);
        for (int g = 0; g < 128; g++) {
            fprintf(stderr, " r%-3d=%08X %08X %08X %08X", g,
                    ctx->gpr[g]._u32[0], ctx->gpr[g]._u32[1],
                    ctx->gpr[g]._u32[2], ctx->gpr[g]._u32[3]);
            if ((g & 1) == 1) fprintf(stderr, "\n");
        }
        fprintf(stderr, "\n");
        /* echo the dispatch chain values the way func_00026DE0 computes them */
        { uint32_t bec0 = ctx->ls[0xBEC0]<<24 | ctx->ls[0xBEC1]<<16 | ctx->ls[0xBEC2]<<8 | ctx->ls[0xBEC3];
          fprintf(stderr, "[SPU] LS[0xBEC0].w0=0x%08X  LS[0x2d4e0:16]=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
            bec0,
            ctx->ls[0x2d4e0],ctx->ls[0x2d4e1],ctx->ls[0x2d4e2],ctx->ls[0x2d4e3],
            ctx->ls[0x2d4e4],ctx->ls[0x2d4e5],ctx->ls[0x2d4e6],ctx->ls[0x2d4e7],
            ctx->ls[0x2d4e8],ctx->ls[0x2d4e9],ctx->ls[0x2d4ea],ctx->ls[0x2d4eb],
            ctx->ls[0x2d4ec],ctx->ls[0x2d4ed],ctx->ls[0x2d4ee],ctx->ls[0x2d4ef]); }
    } }
    ctx->status = SPU_STATUS_STOPPED_BY_HALT;
}

/* ===========================================================================
 * Execution trace (for §3 validation: diff vs RPCS3 SPU interpreter)
 *
 * When the lifter is invoked with --trace, every emitted instruction is
 * surrounded by spu_trace_pc(ctx, PC) before execution and spu_trace_rt(
 * ctx, RT) after, for instructions whose destination is the rt slot. The
 * output is one line per event:
 *
 *     <PC-5hex>                          - PC about to execute
 *       r<rt> <hi-64hex> <lo-64hex>      - register written, post-state
 *
 * Direct to stderr by default; call spu_trace_init(path) once at startup
 * to redirect to a file. The format is intentionally minimal and stable
 * so a small converter can line it up against an RPCS3.log SPU trace.
 * ===========================================================================*/
static FILE* s_trace_fp = NULL;

void spu_trace_init(const char* path)
{
    if (!path || !*path) { s_trace_fp = stderr; return; }
    s_trace_fp = fopen(path, "w");
    if (!s_trace_fp) s_trace_fp = stderr;
}

/* Bounded by design: a --trace lift emits one call PER INSTRUCTION, so an image
 * that spins (a persistent SPURS policy module polling its job queue never
 * returns) would otherwise write stderr until the disk fills. The cap keeps the
 * useful part -- the entry path plus enough revolutions to show what the loop
 * tests -- and the tail IS the loop. SPU_TRACE_MAX=0 restores unbounded.
 * SPU_TRACE_FILE redirects off stderr so the trace does not interleave with the
 * boot log. */
/* Trace output is OPT-IN at runtime: a --trace lift bakes the calls into the
 * generated C forever, so a trace-lifted image shipped in a title build (LBP's
 * job kernel, kept from the WWS verification) would otherwise spew the
 * per-instruction log into every boot. Enable with SPU_TRACE=1 (stderr) or
 * SPU_TRACE_FILE=<path>. */
static long long s_trace_left = -1;     /* -1 uninit, -2 unbounded, 0 off */
/* Set when spu_trace_pc suppressed its line (gates below): the paired
 * spu_trace_rt call for the same instruction must also stay silent. */
static SPU_THREAD_LOCAL int s_trace_suppress;

void spu_trace_pc(spu_context* ctx, uint32_t pc)
{
    (void)ctx;
    /* SPU_TRACE_AT_WALL=1: hold the trace until the PPU's job-barrier probe
     * arms g_barrier_sync_watch, so the budget covers the post-ticket claim/
     * stage pass instead of boot-time idle polling. */
    { static int s_aw = -1;
      if (s_aw < 0) s_aw = getenv("SPU_TRACE_AT_WALL") ? 1 : 0;
      if (s_aw) { extern uint32_t g_barrier_sync_watch;
                  if (!g_barrier_sync_watch) { s_trace_suppress = 1; return; } } }
    /* SPU_TRACE_FLOWCTX=1: trace ONLY the run the SPURS_PM_FLOW tracer armed
     * (the stalled queue's spu0 dispatch) -- post-wall idle dispatches of the
     * other 13 jobmanager wids otherwise exhaust the budget in under a second. */
    { static int s_fc = -1;
      if (s_fc < 0) s_fc = getenv("SPU_TRACE_FLOWCTX") ? 1 : 0;
      if (s_fc) { extern void* volatile g_pm_flow_ctx;
                  if (g_pm_flow_ctx != (void*)ctx) { s_trace_suppress = 1; return; } } }
    /* SPU_TRACE_IMG=<n>: trace only contexts running image n (e.g. 1 = the
     * WWS job binary, to catch the crt corrupting the job's r3/r4 args). */
    { static int64_t s_ti = -2;
      if (s_ti == -2) { const char* e = getenv("SPU_TRACE_IMG");
                        s_ti = e ? atoll(e) : -1; }
      if (s_ti >= 0 && ctx->image_id != (int)s_ti) { s_trace_suppress = 1; return; } }
    s_trace_suppress = 0;
    if (s_trace_left < 0) {
        if (s_trace_left == -1) {
            const char* p = getenv("SPU_TRACE_FILE");
            if ((!p || !*p) && !getenv("SPU_TRACE")) { s_trace_left = 0; return; }
            if (p && *p && !s_trace_fp) spu_trace_init(p);
            const char* m = getenv("SPU_TRACE_MAX");
            s_trace_left = (m && *m) ? atoll(m) : 200000;
            if (s_trace_left == 0) s_trace_left = -2;   /* -2 = unbounded */
        }
    }
    if (!s_trace_fp) s_trace_fp = stderr;
    if (s_trace_left == 0) return;
    if (s_trace_left > 0 && --s_trace_left == 0) {
        fprintf(s_trace_fp, "-- SPU_TRACE_MAX reached; trace stopped --\n");
        fflush(s_trace_fp);
        return;
    }
    fprintf(s_trace_fp, "%05X\n", pc & SPU_LS_MASK);
}

void spu_trace_rt(spu_context* ctx, uint32_t rt)
{
    if (s_trace_suppress) return;          /* paired _pc line was gated out */
    if (s_trace_left == 0) return;         /* tracing off/stopped (see _pc) */
    if (!s_trace_fp) s_trace_fp = stderr;
    u128 v = ctx->gpr[rt & 0x7F];
    fprintf(s_trace_fp, "  r%-3u %016llX %016llX\n",
            (unsigned)(rt & 0x7F),
            (unsigned long long)v._u64[0],
            (unsigned long long)v._u64[1]);
}

#ifdef __cplusplus
}
#endif
/* racacit: see spu_ls_write128. Names the lifted function (by its LS entry
 * and image) whose code issued the store, from the caller's host address:
 * the registry entry with the nearest host start at or below it. */
__attribute__((noinline)) void spu_ls_fill_guard(spu_context* ctx, uint32_t lsa)
{
    static volatile long n = 0;
    if (n >= 6) return;
    n++;
    const uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    uintptr_t best = 0; uint32_t best_ls = 0; int best_img = -1;
    for (uint32_t i = 0; i < s_registry_count; i++) {
        const uintptr_t h = (uintptr_t)s_registry[i].fn;
        if (h <= ra && h > best) { best = h; best_ls = s_registry[i].addr; best_img = s_registry[i].image_id; }
    }
    fprintf(stderr, "[ls-fill] img=%d delta=0x%X store 0x8000.. at LS 0x%05X from lifted fn img %d LS 0x%05X (+0x%llX host) r3=%08X r4=%08X r5=%08X r6=%08X%c",
            ctx->image_id, ctx->ls_delta, lsa, best_img, best_ls, (unsigned long long)(ra - best),
            ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0], ctx->gpr[6]._u32[0], 10);
    fflush(stderr);
}

/* racacit: the last 128 DMA transfers of this SPU thread, printed on demand
 * (spu_dma_ring_dump) -- which transfer was supposed to fill a buffer that a
 * job then read garbage from. */
static _Thread_local struct { uint32_t cmd, lsa, ea, size; } s_dring[128];
static _Thread_local uint32_t s_dring_i;
void spu_dma_ring_note(uint32_t cmd, uint32_t lsa, uint32_t ea, uint32_t size)
{
    const uint32_t k = s_dring_i++ & 127u;
    s_dring[k].cmd = cmd; s_dring[k].lsa = lsa; s_dring[k].ea = ea; s_dring[k].size = size;
}
void spu_dma_ring_dump(const char* why)
{
    fprintf(stderr, "[dma-ring] %s: last transfers, oldest first%c", why, 10);
    for (uint32_t t = 0; t < 128; t++) {
        const uint32_t k = (s_dring_i + t) & 127u;
        if (!s_dring[k].size && !s_dring[k].cmd) continue;
        fprintf(stderr, "[dma-ring]   cmd=0x%02X lsa=0x%05X ea=0x%08X size=%u%c",
                s_dring[k].cmd, s_dring[k].lsa, s_dring[k].ea, s_dring[k].size, 10);
    }
}
