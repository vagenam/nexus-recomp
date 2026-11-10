/*
 * ps3recomp - the draw engine's backend on its own thread (default; RSX_BACKEND_THREAD=0 = off)
 *
 * The render thread walks the FIFO, decodes state, converts geometry and
 * resolves textures for every draw, then records it into the host API. In a
 * draw-heavy scene (A Crack in Time's Axiom City: ~6600 draws a frame) that
 * thread is the frame rate, and the host-API half -- command recording, the
 * driver, descriptor and constant copies -- is about a fifth of it.
 *
 * On by default; RSX_BACKEND_THREAD=0 keeps the backend on the render thread.
 * This proxy implements rsx_draw_backend over a real one. Every call is
 * appended to a single-producer/single-consumer command ring (its pointed-to
 * data copied in) and a worker thread replays them, in order, into the real
 * backend. Calls that return a handle get a VIRTUAL handle at once; the
 * worker records which real handle each became (s_v2r) and translates every
 * handle in later commands. Views and snapshots, which the real backend
 * reuses per (target, size), are cached by key so the same virtual handle
 * keeps naming them. Readbacks, the only calls whose result the engine reads,
 * wait until the worker has drained the ring and then run on the caller's
 * thread (the worker is idle; nothing else touches the backend then).
 * Presents are throttled to two queued frames.
 */
#include "rsx_draw_engine.h"
#include "../../runtime/platform/win32_compat.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ring --------------------------------------------------------------- */

#define PX_RING_BYTES   (64u << 20)
#define PX_BIG          (256u << 10)     /* payloads above this live in a heap block */
#define PX_ALIGN(n)     (((n) + 15u) & ~15u)

enum {
    OP_PAD = 1, OP_TEX_CREATE, OP_TEX_UPLOAD, OP_TEX_RELEASE, OP_COLOR_CREATE, OP_COLOR_RELEASE,
    OP_VIEW, OP_DEPTH_CREATE, OP_DEPTH_RELEASE, OP_DSNAP, OP_DSNAP_SIZED, OP_CSNAP, OP_BLIT,
    OP_PIPE_CREATE, OP_PIPE_RELEASE, OP_BIND_TARGETS, OP_BIND_PIPE, OP_VS_CONST, OP_PS_CONST,
    OP_BIND_TEX, OP_BIND_VTEX, OP_VIEWPORT, OP_SCISSOR, OP_STENCIL_REF, OP_DRAW, OP_CLEAR_COLOR,
    OP_CLEAR_DS, OP_PRESENT, OP_GEOM_UPLOAD, OP_GEOM_RELEASE, OP_DRAW_CACHED, OP_SUBMIT, OP_VS_DELTA,
    OP_DRAW_CACHED_VI, OP_DEPTH_BIAS, OP_BLEND_COLOR
};

typedef struct { u32 op, bytes; } px_hdr;   /* bytes: the whole record, aligned */

static const rsx_draw_backend* s_real;
static u8* s_ring;
/* Monotonic byte positions, each on its own cache line: the worker reads
 * s_w and writes s_r, the caller the other way round. Both sides publish in
 * batches (px_publish, the worker every PX_R_BATCH bytes), because a shared
 * line bounced per command cost more than the commands. */
static _Alignas(64) _Atomic unsigned long long s_w;
static _Alignas(64) _Atomic unsigned long long s_r;
static _Alignas(64) _Atomic int s_worker_idle;
static _Atomic int s_quit;
static _Alignas(64) _Atomic int s_presents;   /* queued, not yet replayed */
static _Alignas(64) unsigned long long s_wl;  /* caller: written, maybe not yet published */
static unsigned long long s_r_seen;           /* caller: the last s_r it read */
static u32 s_unpub;                           /* caller: commands since the last publish */
#define PX_PUB_CMDS  48u
#define PX_PUB_BYTES (64u << 10)
#define PX_R_BATCH   (256u << 10)
static SRWLOCK s_lk = SRWLOCK_INIT;
static CONDITION_VARIABLE s_cv_work = CONDITION_VARIABLE_INIT;   /* worker: something to do */
static CONDITION_VARIABLE s_cv_room = CONDITION_VARIABLE_INIT;   /* producer: room / idle / presents */
static int s_thread;

/* ---- virtual handles ---------------------------------------------------- */

#define PX_MAX_V (1u << 20)
static u32* s_v2r;                 /* written by the worker (or by the caller at a sync) */
static u32* s_free;  static u32 s_nfree;
static u32 s_next_v = 1;

static u32 v_alloc(void)
{
    if (s_nfree) return s_free[--s_nfree];
    if (s_next_v >= PX_MAX_V) { fprintf(stderr, "[be-proxy] out of virtual handles%c", 10); return 0; }
    return s_next_v++;
}
/* Bumped whenever a handle is freed: the state caches below (bind calls
 * that repeat what the worker already holds are not queued) forget what they
 * sent, since a freed number may come back naming another object. */
static u32 s_bind_gen = 1;
static void v_free(u32 v) { if (v && v < PX_MAX_V && s_nfree < PX_MAX_V) s_free[s_nfree++] = v; s_bind_gen++; }
static inline u32 tr(u32 v) { return (v && v < PX_MAX_V) ? s_v2r[v] : 0; }
/* Set by the worker when a texture_create gave no real texture (out of video
 * memory, say), cleared by its release. The engine already holds the virtual
 * handle; without this it sampled nothing through it -- the magenta "nothing
 * bound" image -- for as long as the texture stayed cached. */
static volatile u8* s_vfail;

/* Caller-side caches: (owner, a, b) -> virtual handle. Views by (surface,
 * remap, format), snapshots by (target, w, h); dropped with their owner. */
typedef struct { u32 owner, a, b, kind, v; } px_key;
#define PX_KEYS 65536u
static px_key* s_keys;  static u32 s_nkeys;
static u32 key_get(u32 kind, u32 owner, u32 a, u32 b, int* fresh)
{
    *fresh = 0;
    for (u32 i = 0; i < s_nkeys; i++) {
        const px_key* k = &s_keys[i];
        if (k->kind == kind && k->owner == owner && k->a == a && k->b == b) return k->v;
    }
    if (s_nkeys >= PX_KEYS) return 0;
    const u32 v = v_alloc();
    if (!v) return 0;
    s_keys[s_nkeys++] = (px_key){ owner, a, b, kind, v };
    *fresh = 1;
    return v;
}
static void key_drop_owner(u32 owner)
{
    for (u32 i = 0; i < s_nkeys; ) {
        if (s_keys[i].owner == owner) { v_free(s_keys[i].v); s_keys[i] = s_keys[--s_nkeys]; }
        else i++;
    }
}

/* RSX_BACKEND_THREAD_STATS=1: where each side waits, every 240 presents. */
static int s_stats = -1;
static unsigned long long st_sync, st_sync_us, st_room, st_room_us, st_thr, st_thr_us, st_busy_us, st_bytes, st_cmds;
static unsigned long long px_us(void)
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (unsigned long long)(c.QuadPart * 1000000.0 / (double)f.QuadPart);
}

/* ---- present pacing (RACACIT_PRESENT_PACE=0 off) ------------------------
 * A frame reaches the window when the render thread has drained it, and
 * that drain varies with the frame: in Axiom City the game kicks a frame
 * every 16.67 ms (its own limiter) and the drain takes 12.6-16.9 ms, so the
 * presents came 12.7-20.9 ms apart and a 60 or 120 Hz display showed some
 * frames for one refresh more than others -- a judder on moving platforms
 * at a steady 60 fps. Each present now waits until one average frame period
 * after the previous one (the period learnt from when frames become ready),
 * so frames ready early are held and presents land evenly; a late frame
 * goes at once. The hold is at most the spread of the drain times. */
static int s_pace = -1, s_pace_stats = -1;
static unsigned long long pc_last_ready, pc_last_present;
static double pc_period;                       /* EMA of ready-to-ready, us */
static unsigned long long pc_hist[6], pc_hold_us, pc_n, pc_t0;
static void px_wait_until(unsigned long long t)
{
#ifdef _WIN32
    /* A high-resolution waitable timer (Windows 10 1803+) sleeps to within a
     * few hundred us: wake 250 us early and spin the rest. Without it the
     * Sleep(0) loop below burned a core for most of every held present. */
    static HANDLE tmr = (HANDLE)-1;
    if (tmr == (HANDLE)-1) tmr = CreateWaitableTimerExW(NULL, NULL, 0x00000002u /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                                        TIMER_ALL_ACCESS);
#endif
    for (;;) {
        const unsigned long long now = px_us();
        if (now >= t) return;
        const unsigned long long left = t - now;
#ifdef _WIN32
        if (tmr && left > 400) {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((left - 250) * 10);   /* relative, 100 ns units */
            if (SetWaitableTimerEx(tmr, &due, 0, NULL, NULL, NULL, 0)) { WaitForSingleObject(tmr, INFINITE); continue; }
        }
#endif
        if (left > 2000) Sleep((DWORD)((left - 1500) / 1000));
        else if (left > 300) Sleep(0);
        /* the last 0.3 ms: spin on the clock */
    }
}
/* Live from Settings > Graphics > Frame pacing (rsx_host_ui.c); -1 = not set
 * there, RACACIT_PRESENT_PACE decides. */
volatile int g_racacit_present_pace = -1;
static void px_pace_present(u32 surface)
{
    static u32 pc_last_surface; static unsigned long long pc_last_call;
    if (s_pace < 0) { const char* e = getenv("RACACIT_PRESENT_PACE"); s_pace = (e && *e == '1') ? 1 : 0;
                      s_pace_stats = getenv("RACACIT_PACE_STATS") ? 1 : 0; }
    if (g_racacit_present_pace >= 0) s_pace = g_racacit_present_pace;
    if (!s_pace && !s_pace_stats) return;
    const unsigned long long ready = px_us();
    /* Each flip is presented twice (the FIFO's flip word and the present
     * thread's own present, rsx_draw_engine_present_buffer): the second, the
     * same image, goes straight through. It can come half a frame behind
     * (~8 ms in space and on Krell Canyon's flybys): with the old 4 ms
     * window it counted as a frame, the learnt period fell to 8.4 ms and
     * pacing switched itself off -- uneven frames, ships judder while they
     * land or fly past. The game alternates its display buffers, so a new
     * frame never comes on the surface just shown: same surface within
     * 14 ms is the repeat. */
    const int dup = surface == pc_last_surface && ready - pc_last_call < 14000ull;
    pc_last_surface = surface; pc_last_call = ready;
    if (dup) return;
    /* (pc_last_call is moved to after the hold below: the duplicate comes
     * behind the held present.) */
    if (pc_last_ready) {
        const double d = (double)(ready - pc_last_ready);
        if (d > 4000.0 && d < 100000.0) pc_period = pc_period > 0.0 ? pc_period + (d - pc_period) / 32.0 : d;
    }
    pc_last_ready = ready;
    unsigned long long at = ready;
    /* Paced only for a steady game between 25 and 100 fps, and only by up
     * to half a period: a frame further ahead means the schedule is stale
     * (a load, a menu) and it goes now, starting a new one. */
    if (s_pace && pc_last_present && pc_period >= 10000.0 && pc_period <= 40000.0) {
        /* A game near 60 fps paces itself to 60 Hz: use that, not the
         * noisy average, or the schedule runs early and holds nothing. */
        const double per = pc_period > 15000.0 && pc_period < 18500.0 ? 1e6 / 60.0
                         : pc_period > 30000.0 && pc_period < 36000.0 ? 1e6 / 30.0 : pc_period;
        const unsigned long long due = pc_last_present + (unsigned long long)per;
        if (due > ready && due - ready <= (unsigned long long)(per / 2.0)) at = due;
    }
    if (at > ready) px_wait_until(at);
    const unsigned long long now = px_us();
    if (s_pace_stats > 0) {
        if (pc_last_present) {
            const unsigned long long iv = now - pc_last_present;
            pc_hist[iv < 15000 ? 0 : iv < 16000 ? 1 : iv < 17500 ? 2 : iv < 20000 ? 3 : iv < 25000 ? 4 : 5]++;
        }
        pc_hold_us += now - ready; pc_n++;
        if (!pc_t0) pc_t0 = now;
        if (now - pc_t0 >= 5000000ull) {
            fprintf(stderr, "[pace] present intervals <15 %llu, 15-16 %llu, 16-17.5 %llu, 17.5-20 %llu, 20-25 %llu, >25 %llu; "
                    "period %.2f ms, held %.2f ms avg%c", pc_hist[0], pc_hist[1], pc_hist[2], pc_hist[3], pc_hist[4], pc_hist[5],
                    pc_period / 1e3, pc_n ? pc_hold_us / 1e3 / (double)pc_n : 0.0, 10);
            memset(pc_hist, 0, sizeof pc_hist); pc_hold_us = pc_n = 0; pc_t0 = now;
        }
    }
    pc_last_present = now; pc_last_call = now;
}

/* ---- producer side ------------------------------------------------------ */

static void px_wake_worker(void)
{
    if (atomic_load_explicit(&s_worker_idle, memory_order_acquire)) {
        AcquireSRWLockExclusive(&s_lk);
        WakeAllConditionVariable(&s_cv_work);
        ReleaseSRWLockExclusive(&s_lk);
    }
}

/* Room for `n` contiguous bytes at the write position (a PAD record first if
 * the ring end is closer); returns where to write. */
/* Hand everything written so far to the worker. */
static void px_publish(void)
{
    s_unpub = 0;
    if (atomic_load_explicit(&s_w, memory_order_relaxed) == s_wl) return;
    atomic_store(&s_w, s_wl);          /* seq_cst: ordered before the idle check */
    px_wake_worker();
}

static u8* px_reserve(u32 n)
{
    for (;;) {
        const unsigned long long w = s_wl;
        const u32 off = (u32)(w % PX_RING_BYTES);
        const u32 tail = PX_RING_BYTES - off;
        const u32 need = (tail < n) ? tail + n : n;
        if (PX_RING_BYTES - (w - s_r_seen) < need)
            s_r_seen = atomic_load_explicit(&s_r, memory_order_acquire);
        if (PX_RING_BYTES - (w - s_r_seen) >= need) {
            if (tail < n) {                          /* pad to the end, start over */
                px_hdr* h = (px_hdr*)(s_ring + off);
                h->op = OP_PAD; h->bytes = tail;
                s_wl = w + tail;
                continue;
            }
            return s_ring + off;
        }
        px_publish();
        const unsigned long long t0 = s_stats > 0 ? px_us() : 0;
        AcquireSRWLockExclusive(&s_lk);
        if (PX_RING_BYTES - (s_wl - atomic_load(&s_r)) < need)
            SleepConditionVariableSRW(&s_cv_room, &s_lk, 1, 0);
        ReleaseSRWLockExclusive(&s_lk);
        if (s_stats > 0) { st_room++; st_room_us += px_us() - t0; }
    }
}
static inline void px_commit(u8* rec, u32 n)
{
    (void)rec;
    const unsigned long long pub = atomic_load_explicit(&s_w, memory_order_relaxed);
    s_wl += n;
    if (++s_unpub >= PX_PUB_CMDS || s_wl - pub >= PX_PUB_BYTES) px_publish();
}

/* A payload: inline when small (returns the inline copy's offset in the
 * record), or a heap block whose pointer the record carries. */
typedef struct { u32 inline_off, size; void* heap; } px_blob;

static u8* px_begin(u32 op, u32 fixed, const void* const* datas, const u32* sizes, u32 nblob, px_blob* blobs, u32* total)
{
    u32 n = PX_ALIGN((u32)sizeof(px_hdr) + fixed);
    for (u32 i = 0; i < nblob; i++) {
        blobs[i].size = sizes[i]; blobs[i].heap = NULL; blobs[i].inline_off = 0;
        if (!datas[i] || !sizes[i]) continue;
        if (sizes[i] > PX_BIG) {
            blobs[i].heap = malloc(sizes[i]);
            if (blobs[i].heap) memcpy(blobs[i].heap, datas[i], sizes[i]);
        } else { blobs[i].inline_off = n; n = PX_ALIGN(n + sizes[i]); }
    }
    u8* rec = px_reserve(n);
    px_hdr* h = (px_hdr*)rec; h->op = op; h->bytes = n;
    for (u32 i = 0; i < nblob; i++)
        if (blobs[i].inline_off) memcpy(rec + blobs[i].inline_off, datas[i], sizes[i]);
    *total = n;
    return rec;
}
static const void* px_blob_ptr(const u8* rec, const px_blob* b)
{
    if (!b->size) return NULL;
    return b->heap ? b->heap : (b->inline_off ? rec + b->inline_off : NULL);
}

/* Records (after the header). */
typedef struct { u32 v; rsx_be_format fmt; u32 w, h, mips, faces, remap, rsx_fmt; } r_tex_create;
typedef struct { u32 v, face, mip, w, h, row_bytes, rows; px_blob data; } r_tex_upload;
typedef struct { u32 v; } r_release;
typedef struct { u32 v; rsx_be_format fmt; u32 w, h, seed_row; px_blob seed; } r_color_create;
typedef struct { u32 v, surface, remap, rsx_format; } r_view;
typedef struct { u32 v, w, h; } r_depth_create;
typedef struct { u32 v, target, w, h, tw, th; } r_snap;
typedef struct { u32 src, sx, sy, sw, sh, dst, dx, dy, dw, dh; } r_blit;
typedef struct { u32 v; rsx_be_render_state rs; rsx_vertex_layout_plan layout; u32 stride; rsx_be_format rt_fmt; u32 rt_count; px_blob vs, ps; } r_pipe;
typedef struct { u32 count, depth, s[RSX_BE_MAX_COLOR_TARGETS]; } r_targets;
typedef struct { u32 v; } r_one;
typedef struct { float units, factor; } r_bias;
typedef struct { float c[4]; } r_blend_c;
typedef struct { u32 bytes; px_blob data; } r_const;
typedef struct { u32 bytes, lo, hi; px_blob data; } r_const_delta;
typedef struct { u32 mask, t[RSX_BE_MAX_TEXTURES]; rsx_be_sampler_desc s[RSX_BE_MAX_TEXTURES]; } r_tex;
typedef struct { float x, y, w, h; } r_viewport;
typedef struct { u32 x, y, w, h; } r_scissor;
typedef struct { rsx_topology topo; u32 vcount, stride, icount; px_blob v, i; } r_draw;
typedef struct { u32 v; float rgba[4]; } r_clear_color;
typedef struct { u32 v, flags; float depth; u8 stencil; } r_clear_ds;
typedef struct { u32 v; px_blob vtx, idx; } r_geom;
typedef struct { rsx_topology topo; u32 geom, vcount, stride, icount; } r_draw_cached;
typedef struct { rsx_topology topo; u32 geom, vcount, stride, icount; int vofs; px_blob i; } r_draw_cached_vi;

#define PX_SIMPLE(OPC_, T, ...) do { u32 n_ = PX_ALIGN((u32)(sizeof(px_hdr) + sizeof(T))); \
        u8* rec_ = px_reserve(n_); px_hdr* h_ = (px_hdr*)rec_; h_->op = (OPC_); h_->bytes = n_; \
        T* b_ = (T*)(rec_ + sizeof(px_hdr)); *b_ = (T){ __VA_ARGS__ }; px_commit(rec_, n_); } while (0)

/* ---- worker -------------------------------------------------------------- */

static unsigned long long s_blit_fails;
static u8* s_vs_blk; static u32 s_vs_cap;


static void px_replay(const u8* rec)
{
    const px_hdr* h = (const px_hdr*)rec;
    const u8* b = rec + sizeof(px_hdr);
    void* u = s_real->user;
    switch (h->op) {
    case OP_PAD: break;
    case OP_TEX_CREATE: { const r_tex_create* a = (const void*)b;
        s_v2r[a->v] = s_real->texture_create(u, a->fmt, a->w, a->h, a->mips, a->faces, a->remap, a->rsx_fmt);
        s_vfail[a->v] = !s_v2r[a->v]; break; }
    case OP_TEX_UPLOAD: { const r_tex_upload* a = (const void*)b;
        s_real->texture_upload(u, tr(a->v), a->face, a->mip, a->w, a->h, px_blob_ptr(rec, &a->data), a->row_bytes, a->rows);
        free(a->data.heap); break; }
    case OP_TEX_RELEASE: { const r_release* a = (const void*)b; s_real->texture_release(u, tr(a->v)); s_v2r[a->v] = 0; s_vfail[a->v] = 0; break; }
    case OP_COLOR_CREATE: { const r_color_create* a = (const void*)b;
        s_v2r[a->v] = s_real->color_target_create(u, a->fmt, a->w, a->h, px_blob_ptr(rec, &a->seed), a->seed_row);
        free(a->seed.heap); break; }
    case OP_COLOR_RELEASE: { const r_release* a = (const void*)b; s_real->color_target_release(u, tr(a->v)); s_v2r[a->v] = 0; break; }
    case OP_VIEW: { const r_view* a = (const void*)b;
        s_v2r[a->v] = s_real->surface_view(u, tr(a->surface), a->remap, a->rsx_format); break; }
    case OP_DEPTH_CREATE: { const r_depth_create* a = (const void*)b; s_v2r[a->v] = s_real->depth_target_create(u, a->w, a->h); break; }
    case OP_DEPTH_RELEASE: { const r_release* a = (const void*)b; s_real->depth_target_release(u, tr(a->v)); s_v2r[a->v] = 0; break; }
    case OP_DSNAP: { const r_snap* a = (const void*)b; s_v2r[a->v] = s_real->depth_snapshot(u, tr(a->target), a->w, a->h); break; }
    case OP_DSNAP_SIZED: { const r_snap* a = (const void*)b;
        s_v2r[a->v] = s_real->depth_snapshot_sized(u, tr(a->target), a->w, a->h, a->tw, a->th); break; }
    case OP_CSNAP: { const r_snap* a = (const void*)b; s_v2r[a->v] = s_real->color_snapshot(u, tr(a->target), a->w, a->h); break; }
    case OP_BLIT: { const r_blit* a = (const void*)b;
        if (!s_real->blit(u, tr(a->src), a->sx, a->sy, a->sw, a->sh, tr(a->dst), a->dx, a->dy, a->dw, a->dh)) s_blit_fails++;
        break; }
    case OP_PIPE_CREATE: { const r_pipe* a = (const void*)b;
        s_v2r[a->v] = s_real->pipeline_create(u, (const char*)px_blob_ptr(rec, &a->vs), (const char*)px_blob_ptr(rec, &a->ps),
                                              &a->rs, &a->layout, a->stride, a->rt_fmt, a->rt_count);
        free(a->vs.heap); free(a->ps.heap); break; }
    case OP_PIPE_RELEASE: { const r_release* a = (const void*)b; s_real->pipeline_release(u, tr(a->v)); s_v2r[a->v] = 0; break; }
    case OP_BIND_TARGETS: { const r_targets* a = (const void*)b; u32 s[RSX_BE_MAX_COLOR_TARGETS];
        for (u32 i = 0; i < a->count && i < RSX_BE_MAX_COLOR_TARGETS; i++) s[i] = tr(a->s[i]);
        s_real->bind_targets(u, s, a->count, tr(a->depth)); break; }
    case OP_BIND_PIPE: { const r_one* a = (const void*)b; s_real->bind_pipeline(u, tr(a->v)); break; }
    case OP_VS_CONST: { const r_const* a = (const void*)b;
        /* The real backend keeps a pointer to the block until the next bind:
         * a copy that lives that long, which a delta then patches. */
        const void* src = px_blob_ptr(rec, &a->data);
        if (a->bytes > s_vs_cap) { free(s_vs_blk); s_vs_blk = (u8*)calloc(1, a->bytes); s_vs_cap = s_vs_blk ? a->bytes : 0; }
        if (s_vs_blk && src) memcpy(s_vs_blk, src, a->bytes);
        s_real->bind_vs_constants(u, src ? s_vs_blk : NULL, a->bytes);
        free(a->data.heap); break; }
    case OP_VS_DELTA: { const r_const_delta* a = (const void*)b;
        const void* src = px_blob_ptr(rec, &a->data);
        if (s_vs_blk && a->bytes <= s_vs_cap && src) memcpy(s_vs_blk + a->lo, src, a->hi - a->lo);
        s_real->bind_vs_constants(u, s_vs_blk, a->bytes);
        free(a->data.heap); break; }
    case OP_PS_CONST: { const r_const* a = (const void*)b;
        s_real->bind_ps_constants(u, px_blob_ptr(rec, &a->data), a->bytes); free(a->data.heap); break; }
    case OP_BIND_TEX: case OP_BIND_VTEX: { const r_tex* a = (const void*)b; u32 t[RSX_BE_MAX_TEXTURES];
        for (u32 i = 0; i < RSX_BE_MAX_TEXTURES; i++) t[i] = tr(a->t[i]);
        if (h->op == OP_BIND_TEX) s_real->bind_textures(u, t, a->s, a->mask);
        else s_real->bind_vertex_textures(u, t, a->s, a->mask);
        break; }
    case OP_VIEWPORT: { const r_viewport* a = (const void*)b; s_real->set_viewport(u, a->x, a->y, a->w, a->h); break; }
    case OP_SCISSOR: { const r_scissor* a = (const void*)b; s_real->set_scissor(u, a->x, a->y, a->w, a->h); break; }
    case OP_STENCIL_REF: { const r_one* a = (const void*)b; s_real->set_stencil_ref(u, a->v); break; }
    case OP_DEPTH_BIAS: { const r_bias* a = (const void*)b; s_real->set_depth_bias(u, a->units, a->factor); break; }
    case OP_BLEND_COLOR: { const r_blend_c* a = (const void*)b; s_real->set_blend_color(u, a->c); break; }
    case OP_DRAW: { const r_draw* a = (const void*)b;
        s_real->draw(u, a->topo, px_blob_ptr(rec, &a->v), a->vcount, a->stride, (const u32*)px_blob_ptr(rec, &a->i), a->icount);
        free(a->v.heap); free(a->i.heap); break; }
    case OP_CLEAR_COLOR: { const r_clear_color* a = (const void*)b; s_real->clear_color(u, tr(a->v), a->rgba); break; }
    case OP_CLEAR_DS: { const r_clear_ds* a = (const void*)b; s_real->clear_depth_stencil(u, tr(a->v), a->flags, a->depth, a->stencil); break; }
    case OP_PRESENT: { const r_one* a = (const void*)b; px_pace_present(a->v); s_real->present(u, tr(a->v));
        atomic_fetch_sub(&s_presents, 1);
        AcquireSRWLockExclusive(&s_lk); WakeAllConditionVariable(&s_cv_room); ReleaseSRWLockExclusive(&s_lk);
        break; }
    case OP_GEOM_UPLOAD: { const r_geom* a = (const void*)b;
        s_v2r[a->v] = s_real->geom_upload(u, px_blob_ptr(rec, &a->vtx), a->vtx.size, (const u32*)px_blob_ptr(rec, &a->idx), a->idx.size);
        free(a->vtx.heap); free(a->idx.heap); break; }
    case OP_GEOM_RELEASE: { const r_release* a = (const void*)b; s_real->geom_release(u, tr(a->v)); s_v2r[a->v] = 0; break; }
    case OP_DRAW_CACHED: { const r_draw_cached* a = (const void*)b;
        s_real->draw_cached(u, a->topo, tr(a->geom), a->vcount, a->stride, a->icount); break; }
    case OP_DRAW_CACHED_VI: { const r_draw_cached_vi* a = (const void*)b;
        s_real->draw_cached_vi(u, a->topo, tr(a->geom), a->vcount, a->stride, (const u32*)px_blob_ptr(rec, &a->i), a->icount, a->vofs);
        free(a->i.heap); break; }
    case OP_SUBMIT: { const r_one* a = (const void*)b; if (s_real->submit_and_wait) s_real->submit_and_wait(u, a->v); break; }
    default: break;
    }
}

static void px_worker_loop(void);
#ifdef _WIN32
static DWORD WINAPI px_worker(LPVOID p) { (void)p; px_worker_loop(); return 0; }
#else
static void* px_worker(void* p) { (void)p; px_worker_loop(); return NULL; }
#endif
static void px_worker_loop(void)
{
    /* PS3_SAMPLE_BE=1: the sampler profiles this thread, as PS3_SAMPLE_RSX does the walker. */
    { extern void ps3_samp_only_tid(unsigned long tid); if (getenv("PS3_SAMPLE_BE")) ps3_samp_only_tid(GetCurrentThreadId()); }
    { char _hb[160]; snprintf(_hb, sizeof _hb, "[hosttid] %lu rsx backend worker%c", (unsigned long)GetCurrentThreadId(), 10); fputs(_hb, stderr); }
    unsigned long long r = atomic_load_explicit(&s_r, memory_order_relaxed), w = r, r_pub = r;
    for (;;) {
        if (r == w) w = atomic_load_explicit(&s_w, memory_order_acquire);
        if (r == w) {
            if (r_pub != r) {                                    /* caught up: show it */
                atomic_store_explicit(&s_r, r, memory_order_release); r_pub = r;
                AcquireSRWLockExclusive(&s_lk); WakeAllConditionVariable(&s_cv_room); ReleaseSRWLockExclusive(&s_lk);
            }
            /* Briefly spin (batches arrive microseconds apart), then sleep. Under 16 CPU
             * threads, more briefly: this was 40% of the worker's time, a CPU the others need. */
            static int s_spin = -1;
            if (s_spin < 0) { extern int racacit_few_cpus(void); s_spin = racacit_few_cpus() ? 300 : 2000; }
            for (int i = 0; i < s_spin && r == w; i++) { YieldProcessor(); w = atomic_load_explicit(&s_w, memory_order_acquire); }
            if (r == w) {
                AcquireSRWLockExclusive(&s_lk);
                atomic_store(&s_worker_idle, 1);   /* seq_cst: before re-reading s_w */
                WakeAllConditionVariable(&s_cv_room);              /* a sync may be waiting */
                while (atomic_load(&s_w) == r && !atomic_load(&s_quit))
                    SleepConditionVariableSRW(&s_cv_work, &s_lk, 5, 0);
                atomic_store_explicit(&s_worker_idle, 0, memory_order_release);
                ReleaseSRWLockExclusive(&s_lk);
                if (atomic_load(&s_quit) && atomic_load(&s_w) == r) return;
                w = atomic_load_explicit(&s_w, memory_order_acquire);
                continue;
            }
        }
        const u8* rec = s_ring + (u32)(r % PX_RING_BYTES);
        const u32 n = ((const px_hdr*)rec)->bytes;
        if (s_stats > 0) {
            const unsigned long long t0 = px_us();
            px_replay(rec);
            const unsigned long long d = px_us() - t0;
            st_busy_us += d; st_bytes += n; st_cmds++;
            if (d >= 20000) fprintf(stderr, "[be-proxy] slow call: op %u took %.1f ms%c", ((const px_hdr*)rec)->op, d / 1e3, 10);
        } else px_replay(rec);
        r += n;
        if (r - r_pub >= PX_R_BATCH) {
            atomic_store_explicit(&s_r, r, memory_order_release); r_pub = r;
            if ((r / (4u << 20)) != ((r - n) / (4u << 20))) {   /* every 4 MB: let a waiting producer in */
                AcquireSRWLockExclusive(&s_lk); WakeAllConditionVariable(&s_cv_room); ReleaseSRWLockExclusive(&s_lk);
            }
        }
    }
}

/* Wait until every queued command has been replayed and the worker sleeps. */
static void px_sync(void)
{
    const unsigned long long t0 = s_stats > 0 ? px_us() : 0;
    if (s_stats > 0) st_sync++;
    px_publish();
    AcquireSRWLockExclusive(&s_lk);
    WakeAllConditionVariable(&s_cv_work);
    while (atomic_load(&s_r) != s_wl || !atomic_load(&s_worker_idle))
        SleepConditionVariableSRW(&s_cv_room, &s_lk, 1, 0);
    ReleaseSRWLockExclusive(&s_lk);
    if (s_stats > 0) st_sync_us += px_us() - t0;
}

/* ---- the proxy's rsx_draw_backend ---------------------------------------- */

static int p_init(void* user, u32 w, u32 h)
{
    (void)user;
    const int rc = s_real->init ? s_real->init(s_real->user, w, h) : 0;
    if (rc) return rc;
    if (!s_thread) {
#ifdef _WIN32
        DWORD tid; HANDLE th = CreateThread(NULL, 1u << 20, px_worker, NULL, 0, &tid);
        s_thread = th != NULL;
        if (th) CloseHandle(th);
#else
        pthread_t th; s_thread = pthread_create(&th, NULL, px_worker, NULL) == 0;
        if (s_thread) pthread_detach(th);
#endif
        fprintf(stderr, "[be-proxy] backend thread %s%c", s_thread ? "running" : "FAILED", 10);
    }
    return 0;
}
static void p_shutdown(void* user)
{
    (void)user;
    px_sync();
    atomic_store(&s_quit, 1);
    px_wake_worker();
    if (s_real->shutdown) s_real->shutdown(s_real->user);
}
static void p_submit(void* user, u32 reason) { (void)user; PX_SIMPLE(OP_SUBMIT, r_one, reason); }
static u32 p_tex_create(void* user, rsx_be_format fmt, u32 w, u32 h, u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)user; const u32 v = v_alloc(); if (!v) return 0;
    PX_SIMPLE(OP_TEX_CREATE, r_tex_create, v, fmt, w, h, mips, faces, remap, rsx_fmt);
    return v;
}
static void p_tex_upload(void* user, u32 tex, u32 face, u32 mip, u32 w, u32 h, const void* src, u32 row_bytes, u32 rows)
{
    (void)user;
    const void* d[1] = { src }; const u32 sz[1] = { src ? row_bytes * rows : 0u }; px_blob bl[1]; u32 n;
    u8* rec = px_begin(OP_TEX_UPLOAD, sizeof(r_tex_upload), d, sz, 1, bl, &n);
    *(r_tex_upload*)(rec + sizeof(px_hdr)) = (r_tex_upload){ tex, face, mip, w, h, row_bytes, rows, bl[0] };
    px_commit(rec, n);
}
static void p_tex_release(void* user, u32 tex) { (void)user; if (!tex) return; PX_SIMPLE(OP_TEX_RELEASE, r_release, tex); v_free(tex); }
static int p_tex_failed(void* user, u32 tex) { (void)user; return tex && tex < PX_MAX_V && s_vfail[tex]; }
static u32 p_color_create(void* user, rsx_be_format fmt, u32 w, u32 h, const void* seed, u32 seed_row)
{
    (void)user; const u32 v = v_alloc(); if (!v) return 0;
    const void* d[1] = { seed }; const u32 sz[1] = { seed ? seed_row * h : 0u }; px_blob bl[1]; u32 n;
    u8* rec = px_begin(OP_COLOR_CREATE, sizeof(r_color_create), d, sz, 1, bl, &n);
    *(r_color_create*)(rec + sizeof(px_hdr)) = (r_color_create){ v, fmt, w, h, seed_row, bl[0] };
    px_commit(rec, n);
    return v;
}
static void p_color_release(void* user, u32 s)
{
    (void)user; if (!s) return;
    PX_SIMPLE(OP_COLOR_RELEASE, r_release, s);
    key_drop_owner(s);             /* its views and snapshot went with it */
    v_free(s);
}
static u32 p_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user; int fresh;
    const u32 v = key_get(1u, surface, remap, rsx_format, &fresh);
    if (v && fresh) PX_SIMPLE(OP_VIEW, r_view, v, surface, remap, rsx_format);
    return v;
}
static u32 p_depth_create(void* user, u32 w, u32 h)
{
    (void)user; const u32 v = v_alloc(); if (!v) return 0;
    PX_SIMPLE(OP_DEPTH_CREATE, r_depth_create, v, w, h);
    return v;
}
static void p_depth_release(void* user, u32 d)
{
    (void)user; if (!d) return;
    PX_SIMPLE(OP_DEPTH_RELEASE, r_release, d);
    key_drop_owner(d);
    v_free(d);
}
/* A snapshot is retaken on every call (its content is the target's now);
 * the key only keeps one virtual handle per (target, size). */
static u32 p_dsnap(void* user, u32 depth, u32 w, u32 h)
{
    (void)user; int fresh; const u32 v = key_get(2u, depth, w, h, &fresh);
    if (v) PX_SIMPLE(OP_DSNAP, r_snap, v, depth, w, h, w, h);
    return v;
}
static u32 p_dsnap_sized(void* user, u32 depth, u32 w, u32 h, u32 tw, u32 th)
{
    (void)user; int fresh; const u32 v = key_get(3u, depth, tw, th, &fresh);
    if (v) PX_SIMPLE(OP_DSNAP_SIZED, r_snap, v, depth, w, h, tw, th);
    return v;
}
static u32 p_csnap(void* user, u32 color, u32 w, u32 h)
{
    (void)user; int fresh; const u32 v = key_get(4u, color, w, h, &fresh);
    if (v) PX_SIMPLE(OP_CSNAP, r_snap, v, color, w, h, w, h);
    return v;
}
/* Queued, reported as done: the engine only uses the result to skip its
 * guest-memory fallback, and its own checks have passed by then. */
static int p_blit(void* user, u32 src, u32 sx, u32 sy, u32 sw, u32 sh, u32 dst, u32 dx, u32 dy, u32 dw, u32 dh)
{
    (void)user; if (!src || !dst) return 0;
    PX_SIMPLE(OP_BLIT, r_blit, src, sx, sy, sw, sh, dst, dx, dy, dw, dh);
    return 1;
}
static u32 p_pipe_create(void* user, const char* vs, const char* ps, const rsx_be_render_state* rs,
                         const rsx_vertex_layout_plan* layout, u32 stride, rsx_be_format rt_fmt, u32 rt_count)
{
    (void)user; if (!vs || !ps || !rs || !layout) return 0;
    const u32 v = v_alloc(); if (!v) return 0;
    const void* d[2] = { vs, ps }; const u32 sz[2] = { (u32)strlen(vs) + 1u, (u32)strlen(ps) + 1u };
    px_blob bl[2]; u32 n;
    u8* rec = px_begin(OP_PIPE_CREATE, sizeof(r_pipe), d, sz, 2, bl, &n);
    r_pipe* a = (r_pipe*)(rec + sizeof(px_hdr));
    a->v = v; a->rs = *rs; a->layout = *layout; a->stride = stride; a->rt_fmt = rt_fmt; a->rt_count = rt_count;
    a->vs = bl[0]; a->ps = bl[1];
    px_commit(rec, n);
    return v;
}
static void p_pipe_release(void* user, u32 p) { (void)user; if (!p) return; PX_SIMPLE(OP_PIPE_RELEASE, r_release, p); v_free(p); }
static void p_bind_targets(void* user, const u32* s, u32 count, u32 depth)
{
    (void)user; r_targets t; memset(&t, 0, sizeof t);
    t.count = count > RSX_BE_MAX_COLOR_TARGETS ? RSX_BE_MAX_COLOR_TARGETS : count; t.depth = depth;
    for (u32 i = 0; i < t.count; i++) t.s[i] = s ? s[i] : 0;
    static r_targets last; static u32 last_gen;
    if (last_gen == s_bind_gen && !memcmp(&last, &t, sizeof t)) return;
    last = t; last_gen = s_bind_gen;
    u32 n = PX_ALIGN((u32)(sizeof(px_hdr) + sizeof t)); u8* rec = px_reserve(n);
    ((px_hdr*)rec)->op = OP_BIND_TARGETS; ((px_hdr*)rec)->bytes = n;
    memcpy(rec + sizeof(px_hdr), &t, sizeof t); px_commit(rec, n);
}
static void p_bind_pipe(void* user, u32 p)
{
    (void)user;
    static u32 last, last_gen;
    if (last_gen == s_bind_gen && last == p) return;
    last = p; last_gen = s_bind_gen;
    PX_SIMPLE(OP_BIND_PIPE, r_one, p);
}
static void p_const(u32 op, const void* data, u32 bytes)
{
    const void* d[1] = { data }; const u32 sz[1] = { data ? bytes : 0u }; px_blob bl[1]; u32 n;
    u8* rec = px_begin(op, sizeof(r_const), d, sz, 1, bl, &n);
    *(r_const*)(rec + sizeof(px_hdr)) = (r_const){ bytes, bl[0] };
    px_commit(rec, n);
}
/* A full block, then deltas against it: valid only after a full bind of
 * the same size, which the engine always makes first. */
static u32 s_vs_full_bytes;
static void p_vs_const(void* user, const void* data, u32 bytes)
{
    (void)user; p_const(OP_VS_CONST, data, bytes); s_vs_full_bytes = data ? bytes : 0;
}
static void p_vs_delta(void* user, const void* data, u32 bytes, u32 lo, u32 hi)
{
    if (!data || bytes != s_vs_full_bytes || lo >= hi || hi > bytes) { p_vs_const(user, data, bytes); return; }
    const void* d[1] = { (const u8*)data + lo }; const u32 sz[1] = { hi - lo }; px_blob bl[1]; u32 n;
    u8* rec = px_begin(OP_VS_DELTA, sizeof(r_const_delta), d, sz, 1, bl, &n);
    *(r_const_delta*)(rec + sizeof(px_hdr)) = (r_const_delta){ bytes, lo, hi, bl[0] };
    px_commit(rec, n);
}
static void p_ps_const(void* user, const void* data, u32 bytes) { (void)user; p_const(OP_PS_CONST, data, bytes); }
/* Field by field: the descriptor has a padding byte nobody initialises,
 * so comparing bytes never matched. */
static int samp_eq(const rsx_be_sampler_desc* a, const rsx_be_sampler_desc* b)
{
    return a->min_linear == b->min_linear && a->mag_linear == b->mag_linear && a->mip_linear == b->mip_linear &&
           a->mip_present == b->mip_present && a->wrap_s == b->wrap_s && a->wrap_t == b->wrap_t &&
           a->wrap_r == b->wrap_r && a->min_lod == b->min_lod && a->max_lod == b->max_lod && a->border == b->border;
}
static void p_tex(u32 op, const u32* t, const rsx_be_sampler_desc* s, u32 mask)
{
    /* Only the units in the mask matter, to the worker and to the
     * comparison with the set it already holds. */
    static r_tex last[2]; static u32 last_gen[2];
    const int k = op == OP_BIND_VTEX;
    if (last_gen[k] == s_bind_gen && last[k].mask == mask) {
        int same = 1;
        for (u32 m = mask; m && same; m &= m - 1) {
            const u32 u = (u32)__builtin_ctz(m);
            if ((t ? t[u] : 0u) != last[k].t[u]) same = 0;
            else if (s && !samp_eq(&s[u], &last[k].s[u])) same = 0;
        }
        if (same) return;
    }
    r_tex a; memset(&a, 0, sizeof a);
    a.mask = mask;
    for (u32 m = mask; m; m &= m - 1) {
        const u32 u = (u32)__builtin_ctz(m);
        if (t) a.t[u] = t[u];
        if (s) a.s[u] = s[u];
    }
    last[k] = a; last_gen[k] = s_bind_gen;
    u32 n = PX_ALIGN((u32)(sizeof(px_hdr) + sizeof(r_tex))); u8* rec = px_reserve(n);
    ((px_hdr*)rec)->op = op; ((px_hdr*)rec)->bytes = n;
    memcpy(rec + sizeof(px_hdr), &a, sizeof a);
    px_commit(rec, n);
}
static void p_bind_tex(void* user, const u32* t, const rsx_be_sampler_desc* s, u32 mask) { (void)user; p_tex(OP_BIND_TEX, t, s, mask); }
static void p_bind_vtex(void* user, const u32* t, const rsx_be_sampler_desc* s, u32 mask) { (void)user; p_tex(OP_BIND_VTEX, t, s, mask); }
/* Plain values the worker keeps until replaced: sent only on change. */
static void p_viewport(void* user, float x, float y, float w, float h)
{
    (void)user;
    static float l[4]; static int sent;
    if (sent && l[0] == x && l[1] == y && l[2] == w && l[3] == h) return;
    sent = 1; l[0] = x; l[1] = y; l[2] = w; l[3] = h;
    PX_SIMPLE(OP_VIEWPORT, r_viewport, x, y, w, h);
}
static void p_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    static u32 l[4]; static int sent;
    if (sent && l[0] == x && l[1] == y && l[2] == w && l[3] == h) return;
    sent = 1; l[0] = x; l[1] = y; l[2] = w; l[3] = h;
    PX_SIMPLE(OP_SCISSOR, r_scissor, x, y, w, h);
}
static void p_stencil_ref(void* user, u32 ref)
{
    (void)user;
    static u32 l; static int sent;
    if (sent && l == ref) return;
    sent = 1; l = ref;
    PX_SIMPLE(OP_STENCIL_REF, r_one, ref);
}
/* Sent only when it changes: almost every draw has none. */
static void p_depth_bias(void* user, float units, float factor)
{
    (void)user;
    static float lu = 0.0f, lf = 0.0f; static int sent = 0;
    if (sent && lu == units && lf == factor) return;
    sent = 1; lu = units; lf = factor;
    PX_SIMPLE(OP_DEPTH_BIAS, r_bias, units, factor);
}
static void p_blend_color(void* user, const float* rgba)
{
    (void)user;
    static float l[4]; static int sent;
    if (sent && !memcmp(l, rgba, sizeof l)) return;
    sent = 1; memcpy(l, rgba, sizeof l);
    PX_SIMPLE(OP_BLEND_COLOR, r_blend_c, { rgba[0], rgba[1], rgba[2], rgba[3] });
}
static void p_draw(void* user, rsx_topology topo, const void* v, u32 vcount, u32 stride, const u32* idx, u32 icount)
{
    (void)user;
    const void* d[2] = { v, idx }; const u32 sz[2] = { v ? vcount * stride : 0u, idx ? icount * 4u : 0u };
    px_blob bl[2]; u32 n;
    u8* rec = px_begin(OP_DRAW, sizeof(r_draw), d, sz, 2, bl, &n);
    *(r_draw*)(rec + sizeof(px_hdr)) = (r_draw){ topo, vcount, stride, icount, bl[0], bl[1] };
    px_commit(rec, n);
}
static void p_clear_color(void* user, u32 s, const float rgba[4])
{
    (void)user; PX_SIMPLE(OP_CLEAR_COLOR, r_clear_color, s, { rgba[0], rgba[1], rgba[2], rgba[3] });
}
static void p_clear_ds(void* user, u32 d, u32 flags, float depth, u8 stencil)
{
    (void)user; PX_SIMPLE(OP_CLEAR_DS, r_clear_ds, d, flags, depth, stencil);
}
static void p_present(void* user, u32 s)
{
    (void)user;
    atomic_fetch_add(&s_presents, 1);
    PX_SIMPLE(OP_PRESENT, r_one, s);
    px_publish();
    /* No more than two frames queued behind the worker. */
    if (atomic_load(&s_presents) > 2) {
        const unsigned long long t0 = s_stats > 0 ? px_us() : 0;
        AcquireSRWLockExclusive(&s_lk);
        while (atomic_load(&s_presents) > 2) SleepConditionVariableSRW(&s_cv_room, &s_lk, 2, 0);
        ReleaseSRWLockExclusive(&s_lk);
        if (s_stats > 0) { st_thr++; st_thr_us += px_us() - t0; }
    }
    static unsigned n_presents; static unsigned long long t_last;
    if (s_stats > 0 && ++n_presents % 240u == 0) {
        const unsigned long long now = px_us(), span = t_last ? now - t_last : 1;
        t_last = now;
        fprintf(stderr, "[be-proxy] 240 presents in %.2f s: worker busy %.0f%% (%llu cmds, %.1f MB); "
                "front waited: sync %llux %.1f ms, ring %llux %.1f ms, throttle %llux %.1f ms; blits failed %llu%c",
                span / 1e6, 100.0 * st_busy_us / span, st_cmds, st_bytes / 1048576.0,
                st_sync, st_sync_us / 1e3, st_room, st_room_us / 1e3, st_thr, st_thr_us / 1e3, s_blit_fails, 10);
        st_sync = st_sync_us = st_room = st_room_us = st_thr = st_thr_us = st_busy_us = st_bytes = st_cmds = 0;
    }
}
static int p_readback(void* user, u32 s, u32 x, u32 y, u32 w, u32 h, void* out, u32 pitch)
{
    (void)user;
    px_sync();                     /* the worker sleeps until the next command */
    return s_real->readback(s_real->user, tr(s), x, y, w, h, out, pitch);
}
static u32 p_geom_upload(void* user, const void* v, u32 vbytes, const u32* idx, u32 ibytes)
{
    (void)user; const u32 gv = v_alloc(); if (!gv) return 0;
    const void* d[2] = { v, idx }; const u32 sz[2] = { v ? vbytes : 0u, idx ? ibytes : 0u }; px_blob bl[2]; u32 n;
    u8* rec = px_begin(OP_GEOM_UPLOAD, sizeof(r_geom), d, sz, 2, bl, &n);
    *(r_geom*)(rec + sizeof(px_hdr)) = (r_geom){ gv, bl[0], bl[1] };
    px_commit(rec, n);
    return gv;
}
static void p_geom_release(void* user, u32 g) { (void)user; if (!g) return; PX_SIMPLE(OP_GEOM_RELEASE, r_release, g); v_free(g); }
static void p_draw_cached(void* user, rsx_topology topo, u32 geom, u32 vcount, u32 stride, u32 icount)
{
    (void)user; PX_SIMPLE(OP_DRAW_CACHED, r_draw_cached, topo, geom, vcount, stride, icount);
}
static void p_draw_cached_vi(void* user, rsx_topology topo, u32 geom, u32 vcount, u32 stride, const u32* idx, u32 icount, int vofs)
{
    (void)user;
    const void* d[1] = { idx }; const u32 sz[1] = { idx ? icount * 4u : 0u };
    px_blob bl[1]; u32 n;
    u8* rec = px_begin(OP_DRAW_CACHED_VI, sizeof(r_draw_cached_vi), d, sz, 1, bl, &n);
    *(r_draw_cached_vi*)(rec + sizeof(px_hdr)) = (r_draw_cached_vi){ topo, geom, vcount, stride, icount, vofs, bl[0] };
    px_commit(rec, n);
}

static rsx_draw_backend s_proxy;

const rsx_draw_backend* rsx_be_proxy_wrap(const rsx_draw_backend* real)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_BACKEND_THREAD"); on = !(e && *e == '0'); }
    if (!on || !real) return real;
    s_ring = (u8*)malloc(PX_RING_BYTES);
    s_v2r = (u32*)calloc(PX_MAX_V, sizeof(u32));
    s_free = (u32*)malloc(PX_MAX_V * sizeof(u32));
    s_keys = (px_key*)malloc(PX_KEYS * sizeof(px_key));
    s_vfail = (volatile u8*)calloc(PX_MAX_V, 1);
    if (!s_ring || !s_v2r || !s_free || !s_keys || !s_vfail) {
        fprintf(stderr, "[be-proxy] out of memory: backend stays on the render thread%c", 10);
        return real;
    }
    s_real = real;
    { const char* e = getenv("RSX_BACKEND_THREAD_STATS"); s_stats = e && *e && *e != '0'; }
    s_proxy = (rsx_draw_backend){
        .user = NULL, .init = p_init, .shutdown = p_shutdown, .submit_and_wait = p_submit,
        .texture_create = p_tex_create, .texture_upload = p_tex_upload, .texture_release = p_tex_release,
        .texture_failed = p_tex_failed,
        .color_target_create = p_color_create, .color_target_release = p_color_release,
        .surface_view = real->surface_view ? p_view : NULL,
        .depth_target_create = p_depth_create, .depth_target_release = p_depth_release,
        .depth_snapshot = real->depth_snapshot ? p_dsnap : NULL,
        .color_snapshot = real->color_snapshot ? p_csnap : NULL,
        .blit = real->blit ? p_blit : NULL,
        .pipeline_create = p_pipe_create, .pipeline_release = p_pipe_release,
        .bind_targets = p_bind_targets, .bind_pipeline = p_bind_pipe,
        .bind_vs_constants = p_vs_const, .bind_ps_constants = p_ps_const,
        .bind_textures = p_bind_tex, .bind_vertex_textures = p_bind_vtex,
        .set_viewport = p_viewport, .set_scissor = p_scissor, .set_stencil_ref = p_stencil_ref,
        .draw = p_draw, .clear_color = p_clear_color, .clear_depth_stencil = p_clear_ds,
        .present = p_present, .readback = real->readback ? p_readback : NULL,
        .depth_snapshot_sized = real->depth_snapshot_sized ? p_dsnap_sized : NULL,
        .geom_upload = real->geom_upload ? p_geom_upload : NULL,
        .geom_release = real->geom_release ? p_geom_release : NULL,
        .draw_cached = real->draw_cached ? p_draw_cached : NULL,
        .draw_cached_vi = real->draw_cached_vi ? p_draw_cached_vi : NULL,
        .bind_vs_constants_delta = p_vs_delta,
        .set_depth_bias = real->set_depth_bias ? p_depth_bias : NULL,
        .set_blend_color = real->set_blend_color ? p_blend_color : NULL,
    };
    fprintf(stderr, "[be-proxy] the draw backend runs on its own thread (RSX_BACKEND_THREAD)%c", 10);
    return &s_proxy;
}
