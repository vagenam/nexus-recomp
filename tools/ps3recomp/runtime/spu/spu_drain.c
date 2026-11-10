/* SPU_DRAIN trampoline model -- runtime state + stub hooks (faithful-adopt).
 *
 * Milestone 1 lands the execution-model plumbing; the per-transfer hooks
 * (lockstep gate, flight recorder, SPURS task-launch) are inert stubs here and
 * gain real bodies in their own milestones. Keeping them as real out-of-line
 * functions (not macros) means the SPU_DRAIN/SPU_RET call sites are already in
 * place -- later milestones only replace the bodies. */
#include "spu_context.h"
#include "../platform/ps3_setjmp.h"   /* setjmp/longjmp that never unwind (Windows) */

/* See spu_context.h `irq_frame`. */
typedef struct spu_irq_frame {
    jmp_buf               env;
    struct spu_irq_frame* prev;
    uint32_t              depth;
    int                   image_id;
} spu_irq_frame;

/* Pending cross-function transfer target for this host thread's SPU context. */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;
SPU_THREAD_LOCAL uint32_t g_spu_pch[8];
SPU_THREAD_LOCAL unsigned g_spu_pch_n;

/* SPU_TRACE_RING=1: per SPU thread, the last 64 control transfers the runtime
 * sees (drain start / return / resume-at-entry / restart, indirect branches),
 * printed by spu_tr_dump when the lane dies on an unresolved branch. For the
 * A Crack in Time level-14 freeze: CollNarrow's handler ran with the
 * dispatcher's frame already popped -- a continuation run twice? */
#include <stdio.h>
#include <stdlib.h>
typedef struct { char k; uint32_t pc, ret, r0, r1, sp0, depth; int img; } spu_tr_ev;
static SPU_THREAD_LOCAL spu_tr_ev s_tr[64];
static SPU_THREAD_LOCAL unsigned s_tr_n;
static int spu_tr_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPU_TRACE_RING"); on = (e && *e == '1') ? 1 : 0; }
    return on;
}
void spu_tr_note(spu_context* ctx, char k, uint32_t ret, uint32_t sp0)
{
    if (!spu_tr_on()) return;
    spu_tr_ev* e = &s_tr[s_tr_n++ & 63u];
    e->k = k; e->pc = (uint32_t)ctx->pc & SPU_LS_MASK; e->ret = ret & SPU_LS_MASK;
    e->r0 = ctx->gpr[0]._u32[0] & SPU_LS_MASK; e->r1 = ctx->gpr[1]._u32[0];
    e->sp0 = sp0; e->depth = ctx->host_depth; e->img = ctx->image_id;
}
void spu_tr_dump(spu_context* ctx)
{
    if (!spu_tr_on()) return;
    fprintf(stderr, "[spu-trace] lane %u img=%d, last transfers (oldest first; D=drain start, R=drain return, "
                    "E=drain resume at lifted entry, X=restart, B=indirect branch):\n",
            (unsigned)ctx->spu_id, ctx->image_id);
    const unsigned n = s_tr_n < 64u ? s_tr_n : 64u;
    for (unsigned i = s_tr_n - n; i != s_tr_n; i++) {
        const spu_tr_ev* e = &s_tr[i & 63u];
        fprintf(stderr, "   %c pc=%05X ret=%05X r0=%05X r1=%05X sp0=%05X depth=%u img=%d\n",
                e->k, e->pc, e->ret, e->r0, e->r1, e->sp0, e->depth, e->img);
    }
    fflush(stderr);
}

/* yz_lockstep_tick now has its real body in spu_lockstep.c (milestone 2). */

/* SPURS task-launch interception at a trampoline hop (milestone: SPURS kernel).
 * No-op until then. */

/* PM flow trace (SPURS_PM_FLOW=1): record every cross-function transfer of ONE
 * policy-module run (the ctx spurs_policy.c arms) so the post-claim decision
 * path can be reconstructed offline. Written by the drain-loop hook below;
 * armed/dumped by spu_run_policy_module. */
uint32_t          g_pm_flow_buf[8192];
volatile unsigned g_pm_flow_n = 0;
void* volatile    g_pm_flow_ctx = 0;

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Sync-model ring-drain: the wwsjob PM's job ring drains only when jobs mark
 * their record's "done" bit (0x8000 in the record's first halfword @LS 0xDF0).
 * On hardware the async load-completion INTERRUPT sets it; our loads complete
 * synchronously, so a job whose code never got a ring slot stays "pending"
 * forever and the ring deadlocks (scan re-bails at LS 0x2318). When we observe
 * that bail spin, the in-flight loads HAVE in fact completed in our model, so
 * set the done bit on the pending records -- the faithful sync equivalent of
 * the completion interrupt. Env SPU_JOBDRAIN (default off while validating). */
extern void spu_halt(spu_context*);
/* Last 32 drain steps on this host thread (pc), for post-mortems of a run that
 * ended where it should not have (spurs_policy.c, [pm-end]). */
static _Thread_local uint32_t t_recent[32];
static _Thread_local uint32_t t_recent_n;
unsigned spu_recent_pcs(uint32_t* out, unsigned max)
{
    unsigned n = t_recent_n < 32 ? t_recent_n : 32, k = n < max ? n : max;
    for (unsigned i = 0; i < k; i++) out[i] = t_recent[(t_recent_n - k + i) & 31];
    return k;
}

/* SPU_JOB_STATS=1: igPm job run times per GUID, every 5 s. A job runs from
 * the first step at LS 0x100-0x13F (its entry after the GUID header) until
 * the lane is back in igPm's runtime (LS 0x3F000 and up). */
extern unsigned ps3_tl_tix(void);
/* PS3_SAMPLE_JOB=<guid hex>: the job each sampled lane is running (0 =
 * none), by the lane's sampler slot (PS3_SAMPLE_SPURS=all registration;
 * t_samp_slot = -1 for threads the sampler does not take). The sampler keeps
 * only samples taken inside that job -- runtime helpers, channel waits and
 * float slow paths included, which the code-address map cannot attribute. */
volatile uint32_t g_samp_lane_job[8];
_Thread_local int t_samp_slot = -1;
/* Lock-line wait time inside the job this lane is running (spu_lr_wait adds
 * to it): SPU_JOB_STATS reports it next to the run time -- how much of a
 * job's time is waiting on another lane rather than its own code. */
_Thread_local unsigned long long t_spu_job_wait_us;
static void spu_job_stats(spu_context* ctx)
{
    static int on = -1;
    if (on < 0) on = (getenv("SPU_JOB_STATS") ? 1 : 0) | (getenv("PS3_TIMELINE") ? 2 : 0) |
                     (getenv("PS3_SAMPLE_JOB") ? 4 : 0);
    if (!on) return;
    static _Thread_local int active; static _Thread_local unsigned long long t0;
    static _Thread_local uint8_t guid[4];
    const uint32_t pc = (uint32_t)ctx->pc & SPU_LS_MASK;
    extern unsigned long long ps3_qpc_us(void);
    if (!active) {
        if (pc < 0x100u || pc >= 0x140u) return;
        active = 1; t0 = ps3_qpc_us(); memcpy(guid, ctx->ls + 0x100, 4); t_spu_job_wait_us = 0;
        if (t_samp_slot >= 0 && t_samp_slot < 8)
            g_samp_lane_job[t_samp_slot] = (uint32_t)guid[0] << 24 | (uint32_t)guid[1] << 16 | (uint32_t)guid[2] << 8 | guid[3];
        if (on & 2) { extern void ps3_tl_at(unsigned long long, char, uint32_t, uint32_t);
            ps3_tl_at(t0, 'J', (uint32_t)guid[0] << 24 | (uint32_t)guid[1] << 16 | (uint32_t)guid[2] << 8 | guid[3],
                      ps3_tl_tix()); }
        return;
    }
    if (pc < 0x3F000u) return;
    active = 0;
    if (t_samp_slot >= 0 && t_samp_slot < 8) g_samp_lane_job[t_samp_slot] = 0;
    const unsigned long long d = ps3_qpc_us() - t0;
    typedef struct { uint32_t g; unsigned long long n, us, max, wus; } ent;
    static ent e[128]; static int ne; static volatile long mu; static unsigned long long last;
    const uint32_t g = (uint32_t)guid[0] << 24 | (uint32_t)guid[1] << 16 | (uint32_t)guid[2] << 8 | guid[3];
    if (on & 2) { extern void ps3_tl_at(unsigned long long, char, uint32_t, uint32_t); ps3_tl_at(t0 + d, 'j', g, (uint32_t)d); }
    if (!(on & 1)) return;
    while (__sync_lock_test_and_set(&mu, 1)) { }
    int k = 0;
    for (; k < ne; k++) if (e[k].g == g) break;
    if (k == ne && ne < 128) { e[ne].g = g; e[ne].n = e[ne].us = e[ne].max = e[ne].wus = 0; ne++; }
    if (k < ne) { e[k].n++; e[k].us += d; e[k].wus += t_spu_job_wait_us; if (d > e[k].max) e[k].max = d; }
    const unsigned long long now = ps3_qpc_us();
    if (!last) last = now;
    if (now - last > 5000000ull) {
        last = now;
        fprintf(stderr, "[job-stats] 5 s, by total run time:%c", 10);
        for (int t = 0; t < 14; t++) {
            int b = -1;
            for (int j = 0; j < ne; j++) if (e[j].n && (b < 0 || e[j].us > e[b].us)) b = j;
            if (b < 0) break;
            fprintf(stderr, "   %08x: %llu runs, %.1f ms total, %.0f us avg, %.2f ms max, %.1f ms in lock-line waits%c", e[b].g, e[b].n,
                    e[b].us / 1e3, (double)e[b].us / (double)e[b].n, e[b].max / 1e3, e[b].wus / 1e3, 10);
            e[b].n = 0;
        }
        ne = 0;
    }
    __sync_lock_release(&mu);
}

volatile long g_spu_xf_exact[8];
int g_spu_xf_stats_on = -1;
/* SPU_XF_STATS=1: calls into the exact (extended-range) float paths per
 * second -- arithmetic, fma family, fi, float-to-int -- every 5 s. */
static void spu_xf_stats(void)
{
    if (g_spu_xf_stats_on < 0) g_spu_xf_stats_on = getenv("SPU_XF_STATS") ? 1 : 0;
    if (!g_spu_xf_stats_on) return;
    extern unsigned long long ps3_qpc_us(void);
    static unsigned long long last; const unsigned long long now = ps3_qpc_us();
    if (!last) { last = now; return; }
    if (now - last < 5000000ull) return;
    last = now;
    fprintf(stderr, "[xf-exact] per s: scalar arith %ld fma %ld fi %ld cflt %ld; vector arith %ld fma %ld fi %ld%c",
            g_spu_xf_exact[0] / 5, g_spu_xf_exact[1] / 5, g_spu_xf_exact[3] / 5, g_spu_xf_exact[4] / 5,
            g_spu_xf_exact[5] / 5, g_spu_xf_exact[6] / 5, g_spu_xf_exact[7] / 5, 10);
    for (int i = 0; i < 8; i++) g_spu_xf_exact[i] = 0;
}

static void spu_task_launch_check_diag(spu_context* ctx, void* fn);

/* Every trampoline step lands here. The diagnostics below are each env-gated,
 * but a dozen separate gates were as costly as the job code they wrapped
 * while a fight's batch ran (spu_task_launch_check 3.8% next to the jobs'
 * own ~10%), so they are decided once: with none of them set, only the two
 * checks that change behaviour run. */
void spu_task_launch_check(spu_context* ctx, void* fn)
{
    static int s_diag = -1;
    if (__builtin_expect(s_diag < 0, 0)) {
        static const char* const k_env[] = {
            "SPU_JOB_STATS", "PS3_TIMELINE", "PS3_SAMPLE_JOB", "SPU_XF_STATS", "SPU_RECENT_PCS",
            "SPU_NO_LS0_END", "SPU_EXITTRACE", "SPU_STEPTRACE", "SPURS_PM_FLOW", "SPU_JOBDRAIN" };
        int d = 0;
        for (unsigned i = 0; i < sizeof k_env / sizeof k_env[0]; i++) if (getenv(k_env[i])) d = 1;
        s_diag = d;
    }
    if (__builtin_expect(s_diag, 0)) { spu_task_launch_check_diag(ctx, fn); return; }
    extern void spu_check_stack_reset(spu_context*, void (*)(spu_context*));
    if (ctx->host_depth && !ctx->policy_mode)
        spu_check_stack_reset(ctx, (void (*)(spu_context*))fn);
    /* A job branching to LS 0 is finished (see the diagnostic version). */
    if (ctx->steps++ && (ctx->pc & SPU_LS_MASK) == 0 && !ctx->policy_mode && ctx->image_id > 0) {
        static int _n = 0;
        if ((_n < 8 && (_n++, 1)))
            fprintf(stderr, "[spurs-job] img=%d branched to LS 0 -- job complete\n", ctx->image_id);
        spu_halt(ctx);
        return;
    }
    if (g_pm_flow_ctx == (void*)ctx && g_pm_flow_n < 8192)
        g_pm_flow_buf[g_pm_flow_n++] = ctx->pc;
}

static void spu_task_launch_check_diag(spu_context* ctx, void* fn)
{
    spu_job_stats(ctx);
    spu_xf_stats();
    /* Every trampoline step lands here, so the diagnostics stay off the path
     * unless asked for: SPU_RECENT_PCS=1 keeps the post-mortem ring. */
    static int s_recent = -1;
    if (s_recent < 0) s_recent = getenv("SPU_RECENT_PCS") ? 1 : 0;
    if (s_recent) t_recent[t_recent_n++ & 31] = (uint32_t)ctx->pc & SPU_LS_MASK;
    extern void spu_check_stack_reset(spu_context*, void (*)(spu_context*));
    if (ctx->host_depth && !ctx->policy_mode)   /* its own first test, without the call */
        spu_check_stack_reset(ctx, (void (*)(spu_context*))fn);
    /* A SPURS job returning to LS 0 is finished -- its crt tail-jumps to the
     * resident job manager, and with the job loaded at 0 that lands on its own
     * entry. Planting a return address in r0 catches the jobs that get there
     * via `bi $r0`, but some branch to 0 DIRECTLY, which the lifter turns into
     * a plain trampoline that never reaches spu_indirect_branch. The drain sees
     * every step, so catch it here too. Step 0 is the real entry. */
    static int s_no_ls0 = -1;
    if (s_no_ls0 < 0) s_no_ls0 = getenv("SPU_NO_LS0_END") ? 1 : 0;
    /* SPU_EXITTRACE=<img>: remember the last PCs this image executed and dump
     * them when the job ends. "Why did it exit here" is a question about the
     * path taken, and a forward trace from the entry never reaches far enough
     * to show it. */
    static int64_t s_et = -2;
    if (s_et == -2) { const char* e = getenv("SPU_EXITTRACE");
                      s_et = e ? strtol(e, 0, 0) : -1; }
    static uint32_t s_ring[64]; static uint32_t s_ri;
    if (s_et >= 0 && ctx->image_id == s_et)
        s_ring[s_ri++ & 63] = (uint32_t)ctx->pc & SPU_LS_MASK;

    if (!s_no_ls0 && ctx->steps++ && (ctx->pc & SPU_LS_MASK) == 0 &&
        !ctx->policy_mode && ctx->image_id > 0) {
        static int _n = 0;
        if ((_n < 8 && (_n++, 1)))
            fprintf(stderr, "[spurs-job] img=%d branched to LS 0 -- job complete\n",
                    ctx->image_id);
        if (s_et >= 0 && ctx->image_id == s_et) {
            fprintf(stderr, "[spu-exit] img=%d last PCs:", ctx->image_id);
            for (int i = 32; i >= 1; i--)
                fprintf(stderr, " %05X", s_ring[(s_ri - i) & 63]);
            fputc(10, stderr); fflush(stderr);
        }
        spu_halt(ctx);
        return;
    }
    /* SPU_STEPTRACE=<img>: pc and the argument registers at every trampoline
     * step. This runs on each drain iteration, so it is the finest-grained
     * view of a register file changing under a running job. */
    { static int64_t s_t = -2;
      if (s_t == -2) { const char* e = getenv("SPU_STEPTRACE"); s_t = e ? strtol(e,0,0) : -1; }
      static int64_t s_from = -2;
      if (s_from == -2) { const char* e = getenv("SPU_STEPTRACE_FROM");
                          s_from = e ? strtol(e,0,16) : -1; }
      static int s_armed = 0;
      if (s_t >= 0 && ctx->image_id == s_t) {
          if (s_from >= 0 && !s_armed &&
              ((uint32_t)ctx->pc & SPU_LS_MASK) == (uint32_t)s_from) s_armed = 1;
          static int n = 0;
          if ((s_from < 0 || s_armed) && n++ < 40) {
              fprintf(stderr, "[step] pc=0x%05X r1=0x%05X r2=0x%08X r3=0x%08X r4=0x%08X\n",
                      (uint32_t)ctx->pc & SPU_LS_MASK, ctx->gpr[1]._u32[0],
                      ctx->gpr[2]._u32[0], ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0]);
              fflush(stderr);
          }
      } }
    if (g_pm_flow_ctx == (void*)ctx && g_pm_flow_n < 8192)
        g_pm_flow_buf[g_pm_flow_n++] = ctx->pc;

    static int s_on = -1;
    if (s_on < 0) s_on = getenv("SPU_JOBDRAIN") ? 1 : 0;
    if (!s_on || ctx->image_id != 2 || !ctx->policy_mode) return;

    /* 0x2318 = the type-2 load's ring-full bail (resets scan, retries). */
    if ((ctx->pc & 0x3FFFF) != 0x2318) return;

    static unsigned s_bail = 0;
    if (++s_bail < 4096) return;      /* only after a real spin, not one pass */
    s_bail = 0;

    /* Set bit 31 (the 0x8000 first-halfword done flag) on each pending record
     * the release sweep scans (16 slots, 4-byte stride at LS 0xDF0), for any
     * record that is non-empty and not already done. */
    int marked = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t* r = &ctx->ls[0xDF0 + i * 4];
        uint32_t w = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
                     ((uint32_t)r[2] << 8) | r[3];
        if (w == 0 || (w & 0x80000000u)) continue;
        w |= 0x80000000u;
        r[0] = (uint8_t)(w >> 24); r[1] = (uint8_t)(w >> 16);
        r[2] = (uint8_t)(w >> 8);  r[3] = (uint8_t)w;
        marked++;
    }
    static int _n = 0;
    if (marked && (_n < 12 && (_n++, 1)))
        fprintf(stderr, "[pm-jobdrain] ring-full spin: marked %d pending records done\n", marked);
}

/* Restore the active image after a lifted call bracket. The brsl/bisl emission
 * saves image_id in a call-site local and hands it back here so an image
 * adopted inside the callee cannot leak into the caller's continuation. A
 * persistent LS-0xA00 workload module (module_img_a00) is re-applied since the
 * plain restore would undo it. */
void spu_img_restore(spu_context* ctx, int32_t saved_img)
{
    ctx->image_id = ctx->module_img_a00 ? ctx->module_img_a00 : saved_img;
}

/* spu_ch_wake now has its real body in spu_channels.c (milestone 3). */

/* --- SPU interrupt dispatch (drain-loop hook) ------------------------------
 * Called when int_enable && (event_status & event_mask): architectural SPU
 * interrupt. srr0 <- the interrupted continuation (ctx->pc already holds the
 * pending transfer target), interrupts disable, and control vectors through
 * the guest-planted branch instruction at LS 0 (the WWS jobmanager's entry
 * writes `bra 0xA2C` there; its handler drives the DMA-list job pipeline and
 * returns via iret). If LS 0 holds no br/bra, the interrupt is NOT taken --
 * we log once and resume normal flow rather than jump into zeros. */
#include <stdio.h>
#include <stdlib.h>

/* ---- Deferred DMA-list stall-and-notify delivery --------------------------
 * Real MFC hardware processes a queued list command (the WWS job manager's
 * barriered null list, MFC_GETLB) ASYNCHRONOUSLY: the stall-and-notify -- and
 * the interrupt it raises -- land after the SPU has run on past the code that
 * armed it. We execute the list synchronously inside `wrch MFC_Cmd`, so the
 * interrupt was delivered before the job manager had stored
 * g_WwsJob_loadJobState = kReadCommands (LS 0x12A0).
 *
 * Its interrupt handler gates the Load->Run advance on BOTH the ch25 stall mask
 * carrying tag 0 (kLoadJob_readCommands) AND loadJobState == kReadCommands. With
 * the state still kNone it correctly declined to advance -- and because
 * MFC_RdListStallStat is "accumulative, clear on read" that single stall was
 * consumed, so no later interrupt could ever advance it: the job never ran and
 * LBP's loading thread deadlocked on a completion that never came.
 *
 * Holding the interrupt off for N drain ticks was tried and DISPROVEN: a probe
 * at the stall site shows loadJobState is ALREADY 0 when the GETLB is issued, so
 * the arming store never ran on this path at all -- it is not an interrupt-timing
 * race. (The pm takes trychangefreetoloadjob's early-exit `ceqhi jobHeader,1 /
 * brhz cond1,Exit` to LS 0x2D30, skipping the Load setup.) Default OFF; kept
 * env-gated for experiments: SPU_SN_DEFER=<n> ticks (0 = immediate, faithful
 * to the current synchronous list execution). */
void* volatile   g_sn_defer_ctx = 0;
volatile unsigned g_sn_defer     = 0;

unsigned spu_sn_defer_ticks(void)
{
    static int s_n = -1;
    if (s_n < 0) { const char* e = getenv("SPU_SN_DEFER"); s_n = e ? atoi(e) : 0; }
    return (unsigned)(s_n < 0 ? 0 : s_n);
}

/* ---- Interrupt register preservation --------------------------------------
 * Hardware contract: an SPU interrupt handler preserves every register (the
 * WWS jobmanager guarantees it with a save/restore shim it installs at the
 * TOP of LS, ~0x3FEC0 -- interrupthandlerasm.spu.s). That shim lies OUTSIDE
 * the PM image our lifter processed, so the lifted handler runs WITHOUT the
 * save/restore and eats the interrupted code's live registers. Measured
 * mid-GetBufferTag: the fsmbi mask registers (r13/r15) arrived zeroed at the
 * packing selbs, the BufferTag's lanes collapsed, the job saw a NULL buffer
 * and bailed -- LBP's loading freeze.
 *
 * Enforce the contract host-side: snapshot the full GPR file when the
 * interrupt is taken, restore it when the handler irets (detected in
 * spu_indirect_branch: pc == saved srr0 with interrupts re-enabled -- during
 * the handler int_enable stays 0, so the first enabled dispatch at srr0 IS
 * the irete). State the handler legitimately publishes lives in LS/channels
 * and is untouched. Per-ctx slots; no nesting (interrupts stay disabled
 * until iret). */
static void spu_irq_regs_save(spu_context* ctx)
{
    ctx->irq_saved = 1;
    ctx->irq_resume_pc = ctx->srr0;
    memcpy(ctx->irq_gpr, ctx->gpr, sizeof ctx->irq_gpr);
}

/* Called from spu_indirect_branch on every dispatch. Restores + clears when
 * the iret lands. Returns 1 if a restore happened (diagnostic). */
/* Drop any saved register file belonging to this context. The save slots are
 * keyed by RAW POINTER, and an spu_context is typically a stack local -- so a
 * later, unrelated run lands on the same address and inherits the earlier
 * context's registers wholesale. A fresh run must never do that. */
void spu_irq_regs_forget(spu_context* ctx)
{
    ctx->irq_saved = 0;
}

int spu_irq_regs_maybe_restore(spu_context* ctx)
{
    {
        if (ctx->irq_saved) {
            if ((ctx->pc & SPU_LS_MASK) == (ctx->irq_resume_pc & SPU_LS_MASK) &&
                ctx->int_enable) {
                memcpy(ctx->gpr, ctx->irq_gpr, sizeof ctx->irq_gpr);
                ctx->irq_saved = 0;
                /* The iret completed below the frame that took the interrupt:
                 * abandon the handler's host frames and resume there. */
                spu_irq_frame* f = (spu_irq_frame*)ctx->irq_frame;
                if (f && ctx->host_depth <= f->depth)
                    ctx->irq_frame = f->prev;      /* returned to its loop the ordinary way */
                if (f && ctx->host_depth > f->depth) {
                    { static int s_it = -1; if (s_it < 0) s_it = getenv("SPU_IRQTRACE") ? 1 : 0;
                      static int _n = 0;
                      if (s_it && (_n < 200 && (_n++, 1)))
                          fprintf(stderr, "[irq] IRET at depth %u unwinds to taking frame depth %u (srr0=0x%05X)\n",
                                  ctx->host_depth, f->depth, ctx->pc & SPU_LS_MASK); }
                    longjmp(f->env, 1);
                }
                if (!f && ctx->host_depth > 0) {
                    /* Taken by the top-level driver loop (depth 0), which
                     * keeps no frame: the driver's own restart re-enters at
                     * srr0 with the host stack empty, which is that frame. */
                    { static int s_it = -1; if (s_it < 0) s_it = getenv("SPU_IRQTRACE") ? 1 : 0;
                      static int _n = 0;
                      if (s_it && (_n < 200 && (_n++, 1)))
                          fprintf(stderr, "[irq] IRET at depth %u unwinds to the driver (srr0=0x%05X)\n",
                                  ctx->host_depth, ctx->pc & SPU_LS_MASK); }
                    extern void spu_restart_dispatch(spu_context*);
                    spu_restart_dispatch(ctx);
                }
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

void (*spu_take_interrupt(spu_context* ctx,
                          void (*tf)(spu_context*)))(spu_context*)
{
    /* Stall-and-notify still settling: let the SPU run on (see above). */
    if (g_sn_defer && g_sn_defer_ctx == (void*)ctx) {
        if (--g_sn_defer == 0) g_sn_defer_ctx = 0;
        return tf;
    }
    const uint8_t* v = ctx->ls;      /* vector word at LS 0, big-endian */
    uint32_t w = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
                 ((uint32_t)v[2] << 8) | v[3];
    uint32_t op9 = w >> 23;          /* top 9 opcode bits */
    uint32_t target;
    if (op9 == 0x060) {              /* bra i16 (absolute) */
        target = ((w >> 7) & 0xFFFF) << 2;
    } else if (op9 == 0x064) {       /* br i16 (relative to LS 0) */
        target = (((w >> 7) & 0xFFFF) << 2) & SPU_LS_MASK;
    } else {
        static int _w = 0;
        if ((_w < 4 && (_w++, 1)))
            fprintf(stderr, "[spu-int] pending (st=0x%X mask=0x%X) but LS0 word "
                    "0x%08X is no branch -- not taken\n",
                    ctx->event_status, ctx->event_mask, w);
        return tf;
    }
    ctx->srr0 = ctx->pc;             /* resume point for iret */
    ctx->int_enable = 0;
    spu_irq_regs_save(ctx);          /* hardware contract: handler preserves regs */
    { static int s_it = -1; if (s_it < 0) s_it = getenv("SPU_IRQTRACE") ? 1 : 0;
      if (s_it) { static int _n = 0; if ((_n < 200 && (_n++, 1)))
        fprintf(stderr, "[irq] TAKE srr0=0x%05X depth=%d r13=%08X r15=%08X\n",
                ctx->srr0 & SPU_LS_MASK, ctx->host_depth,
                ctx->gpr[13]._u32[0], ctx->gpr[15]._u32[0]); } }
    ctx->pc = target & SPU_LS_MASK;
    { extern volatile int g_spu_irqfollow_n; if (g_spu_irqfollow_n == 0) g_spu_irqfollow_n = 1; }
    { static int _n = 0;
      if ((_n < 16 && (_n++, 1))) {
          /* WWS Load->Run diagnosis: the job-manager interrupt handler advances
           * g_WwsJob_loadJobState kReadCommands(1)->kExecuteCommands(2) only if
           * (a) the ch25 stall mask has tag 0 (kLoadJob_readCommands) AND
           * (b) loadJobState's HALFWORDS compare equal to 1 (ceqhi).
           * Log both at interrupt entry so we can see which test fails. */
          const uint8_t* js = ctx->ls + 0x12A0;   /* g_WwsJob_loadJobState quadword */
          fprintf(stderr, "[spu-int] TAKEN img=%d events=0x%X&0x%X vector->0x%05X "
                  "(srr0=0x%05X) stallstat=0x%X parked=0x%X loadJobState@12A0=%02X%02X%02X%02X "
                  "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n", ctx->image_id,
                  ctx->event_status, ctx->event_mask, ctx->pc, ctx->srr0,
                  ctx->list_stall_stat, ctx->list_stall_mask,
                  js[0],js[1],js[2],js[3], js[4],js[5],js[6],js[7],
                  js[8],js[9],js[10],js[11], js[12],js[13],js[14],js[15]); } }
    return spu_indirect_branch;
}

/* See the declaration in spu_context.h. */
void spu_depth_guard(spu_context* ctx)
{
    static uint32_t s_max = 0;
    if (!s_max) {
        const char* e = getenv("SPU_HOST_DEPTH_MAX");
        s_max = e ? (uint32_t)strtoul(e, 0, 0) : 2000u;
    }

    /* Ring of recent drain sites. When the depth trips, the useful question is
     * not "where are we" but "what cycle got us here" -- one pc names a point,
     * a ring names the loop. Per thread: one shared ring was a cache line
     * every SPU thread wrote on every lifted call. */
    enum { RING = 24 };
    static _Thread_local uint32_t s_ring[RING];
    static _Thread_local uint32_t s_n;
    s_ring[s_n % RING] = ((uint32_t)ctx->pc & SPU_LS_MASK);
    s_n++;

    if (ctx->host_depth < s_max) return;
    static int s_reported = 0;
    if (s_reported < 1) {   /* one report: five SPU threads all trip together */
        s_reported++;
        fprintf(stderr, "[spu-depth] img=%d pc=0x%05X lr=0x%05X host_depth=%u"
                        " -- lifted call recursion, halting the SPU\n",
                ctx->image_id, (uint32_t)ctx->pc & SPU_LS_MASK,
                ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->host_depth);
        fprintf(stderr, "[spu-depth] recent drain sites (oldest first):");
        uint32_t start = (s_n > RING) ? (s_n - RING) : 0;
        for (uint32_t k = start; k < s_n; k++)
            fprintf(stderr, " %05X", s_ring[k % RING]);
        fprintf(stderr, "\n");
        fflush(stderr);
    }
    { extern void spu_halt(spu_context*); spu_halt(ctx); }
}

/* See spu_context.h. Env-gated: SPU_TAILRET=1. */
int spu_tailret_enabled(void)
{
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("SPU_TAILRET"); s_on = (e && e[0] == 0x31) ? 1 : 0; }
    return s_on;
}

/* A return may use any register, not only r0. Running that branch target
 * inside this drain executes the caller's continuation twice (once here,
 * once when its real host frame resumes), including its stack adjustment.
 *
 * One kind of "wrong" pc is safe to run here, and must be: a lifted ENTRY
 * that has no host frame of its own. Every WWS job module starts with
 *
 *     ila $r0, entry+0xAC ; a $r0, $r0, $r126 ; br body
 *
 * so the body ends `bi $r0` at a point the entry function tail-jumped to and
 * whose host frame is therefore gone: the continuation exists only as the
 * lifted function seeded at that address. Unwinding instead (the restart
 * below) leaves the manager unable to resume at ITS return point, which is a
 * plain r0 drain with no entry, and the job ends on a garbage r0. So when the
 * callee comes back on a lifted entry, run it in this drain until the pc
 * reaches return_pc; only a pc with no lifted code behind it is unwound. */
typedef void (*spu_drain_fn)(spu_context*);
extern spu_drain_fn spu_lookup(uint32_t addr, int image_id);

void spu_drain_call(spu_context* ctx, uint32_t return_pc)
{
    spu_depth_guard(ctx);
    /* racacit: a callee really returns with the caller's r1 restored. Code
     * merely passing through return_pc's LS address (a job image loaded over
     * the caller -- A Crack in Time's igPm jobs overwrite the policy module at
     * LS 0xA00..0xC30) must not be taken for the return. */
    const uint32_t sp0 = ctx->gpr[1]._u32[0];
    /* ...but the callee may not have finished when we get here: a lifted
     * function that ends in a trampoline (0x1750 -> 0x17A0 -> 0x17E0, A Crack
     * in Time's DMA helper) returns to us with ITS frame still on r1, so sp0 is
     * the callee's stack pointer and the real return -- r1 restored ABOVE it --
     * never matched. The drain then fell off into spu_restart_dispatch and the
     * whole policy module ended mid-job; igPm re-ran the job forever and Great
     * Clock's level load never finished. A return is the return pc reached at
     * or above the entry frame; code passing through deeper is not. */
#define SPU_DRAIN_AT_RETURN() (((ctx->pc & SPU_LS_MASK) == (return_pc & SPU_LS_MASK)) && ctx->gpr[1]._u32[0] >= sp0)
    /* The frame an interrupt taken in THIS loop returns to. It stays armed
     * after the handler's first host function returns -- the handler keeps
     * running as trampolines of this loop -- until its iret fires or the
     * loop exits, whichever first. */
    spu_irq_frame f;
    f.prev = 0; f.depth = 0; f.image_id = 0;
    spu_tr_note(ctx, 'D', return_pc, sp0);
#define SPU_DRAIN_POP_IRQ() do { if (ctx->irq_frame == &f) ctx->irq_frame = f.prev; ctx->drain_ret_pc = 0; ctx->drain_ret_sp = 0; } while (0)
    for (;;) {
        while (g_spu_trampoline_fn) {
            if (g_spu_trampoline_fn == spu_indirect_branch && SPU_DRAIN_AT_RETURN()) {
                g_spu_trampoline_fn = 0;
                SPU_DRAIN_POP_IRQ();
                spu_tr_note(ctx, 'R', return_pc, sp0);
                return;
            }
            void (*fn)(spu_context*) = g_spu_trampoline_fn;
            g_spu_trampoline_fn = 0;
            ctx->drain_ret_pc = return_pc & SPU_LS_MASK;   /* re-set: a nested drain changed it */
            ctx->drain_ret_sp = sp0;
            yz_lockstep_tick(ctx);
            spu_task_launch_check(ctx, (void*)fn);
            if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) {
                void (*vf)(spu_context*) = spu_take_interrupt(ctx, fn);
                if (!ctx->int_enable) {   /* taken (a take clears the enable; a deferral leaves it) */
                    /* Interrupt taken here: this loop is the frame the iret
                     * comes back to, however deep the handler leaves from. */
                    if (ctx->irq_frame != &f) {
                        f.prev = (spu_irq_frame*)ctx->irq_frame;
                        f.depth = ctx->host_depth;
                        f.image_id = ctx->image_id;
                        ctx->irq_frame = &f;
                    }
                    if (setjmp(f.env) == 0) {
                        vf(ctx);
                    } else {
                        /* iret fired deeper: registers restored, pc = srr0 */
                        ctx->host_depth = f.depth;
                        ctx->image_id = f.image_id;
                        g_spu_trampoline_fn = spu_indirect_branch;
                        SPU_DRAIN_POP_IRQ();
                    }
                    continue;
                }
            }
            fn(ctx);
        }
        if (SPU_DRAIN_AT_RETURN()) {
            SPU_DRAIN_POP_IRQ();
            spu_tr_note(ctx, 'R', return_pc, sp0);
            return;
        }
        {
            uint32_t pc = ctx->pc & SPU_LS_MASK;
            spu_drain_fn fn = ctx->resident_ovl ? spu_lookup(pc, ctx->resident_ovl) : 0;
            if (!fn) { extern spu_drain_fn spu_lookup_ctx(const spu_context*, uint32_t, int);
                       fn = spu_lookup_ctx(ctx, pc, ctx->image_id); }
            if (fn) {
                /* Capped per image: the WWS lanes (img 100) used up a global cap
                 * of 8 at boot, hiding every later image's resumes. Ids run to
                 * 543 (game/spu/spu_jobs.c): at 64 slots img 100 and 101 still
                 * shared bucket 0. */
                { enum { DR_MAX_IMG = 1024, DR_PER_IMG = 8 };
                  static int _n[DR_MAX_IMG];
                  const unsigned im = (ctx->image_id >= 0 && ctx->image_id < DR_MAX_IMG) ? (unsigned)ctx->image_id : 0;
                  if (_n[im] < DR_PER_IMG && (_n[im]++, 1))
                    fprintf(stderr, "[spu] drain-resume return_pc=0x%05X at lifted entry 0x%05X img=%d depth=%d ovl=%d r0=0x%05X r1=0x%05X sp0=0x%05X\n",
                            (unsigned)(return_pc & SPU_LS_MASK), pc, ctx->image_id, ctx->host_depth,
                            (int)ctx->resident_ovl, ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->gpr[1]._u32[0], sp0); }
                spu_tr_note(ctx, 'E', return_pc, sp0);
                g_spu_trampoline_fn = spu_indirect_branch;
                continue;
            }
        }
        {
            extern void spu_restart_dispatch(spu_context*);
            SPU_DRAIN_POP_IRQ();
            spu_tr_note(ctx, 'X', return_pc, sp0);
            spu_restart_dispatch(ctx);
            return;
        }
    }
#undef SPU_DRAIN_POP_IRQ
#undef SPU_DRAIN_AT_RETURN
}
