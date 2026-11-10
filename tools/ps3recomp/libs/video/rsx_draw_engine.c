/*
 * ps3recomp - platform-neutral RSX draw engine (see rsx_draw_engine.h)
 *
 * The orchestration carried across from libs/video/rsx_live_draw.c, the
 * NV4097 -> D3D12 engine a title has shipped on. Line numbers in the comments
 * below are that file's, so the two can be read side by side; the file itself
 * is vendored and stays exactly as it is.
 *
 * What is deliberately left out, because it is not renderer behaviour a title
 * needs: the shader disk cache, movie mode and its compositor, the a010
 * probe, and every YZ_PERF_PROFILE block.
 */
#include "rsx_draw_engine.h"
#include "rsx_writewatch.h"

#include "rsx_fp_decompiler.h"
#include "rsx_primitives.h"
#include "rsx_restart_cuts.h"
#include "rsx_vertex_formats.h"
#include "rsx_vp_decompiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#else
#include <sys/stat.h>
#endif
#if defined(__AVX2__)
#include <immintrin.h>
#endif

/* Guest memory and the RSX offset resolvers, declared the way every backend
 * that reads guest data declares them (rsx_vertex_fetch.h). */
extern u8* vm_base;
u32 cellGcmResolveLocated(int local, u32 offset);
/* Non-zero only when the offset's page is in the IO table, so a caller can
 * tell IO-mapped main memory from "assume VRAM". */
u32 cellGcmResolveIO(u32 offset);
/* The guest VM's OOB guard: the size a host backed, or 0 for "the whole 32-bit
 * space is backed and no check is needed" (runtime/memory/vm.h). */
extern u32 ppu_vm_size;

/* ------------------------------------------------------------------------- */

/* A Crack in Time carves its shadow maps (depth-only passes, 32x32 up to
 * 1024x1024) out of one pool, and Zolar Forest alone used ~190 distinct
 * (address, size) targets and 60+ zeta addresses: 128 and 64 overflowed,
 * which churned targets every frame and left later shadow maps with no depth
 * buffer at all (a dark box around Ratchet). */
/* Room for what a level churns through between two uses of a target: at
 * 512/256 A Crack in Time filled the lists within minutes (per-object 16x16
 * targets, the shadow pool, haze copies) and a target retired from them that
 * the game still sampled came back as raw VRAM: neon noise over scenery and
 * Ratchet. */
#define ENG_MAX_SURFACES   1024
#define ENG_MAX_ZDEPTHS    512
#define ENG_ADDR_BUCKETS   2048u     /* power of two: the address index below */
#define ENG_MAX_TEXTURES   RSX_DRAW_ENGINE_TEXTURE_CACHE
#define ENG_TEX_INDEX      65536u    /* power of two, >= 4x ENG_MAX_TEXTURES; entries are u16 slot + 1 */
#define ENG_MAX_VTEXTURES  64
#define ENG_MAX_PIPELINES  8192
#define ENG_PIPE_INDEX     16384     /* power of two, > 1.5x pipelines */
#define ENG_VERT_STRIDE    (RSX_DSP_NUM_VERTEX_ATTR * 16u)
#define ENG_INVALID        0xFFFFFFFFu
/* Guest textures larger than this are a misdecoded register, not an image;
 * rsx_live_draw.c rejects the same 4096 bound (2033). */
#define ENG_MAX_TEX_DIM    4096u
/* A surface declaration beyond this is a guest pointer read as clip
 * dimensions. rsx_live_draw.c's surface-guard (2625) preserves the existing
 * target rather than destroying a live one. */
#define ENG_MAX_SURFACE_DIM 8192u

/* Cycle counters for the RSX_ENG_STATS breakdown. Off, they read 0: about a
 * dozen rdtsc per draw was measurable at 6000 draws a frame. */
static int s_eng_prof = -1;
/* Per guest frame: what the render thread did, for the frame-drop report
 * (rsx_draw_engine_frame_mark). */
static struct { u32 draws, tex_ups, pipes, surfs, snaps, zcreates; u64 tex_cyc, pipe_cyc, sink_cyc, create_cyc, snap_cyc; } s_hf;
/* A raw cycle counter: the TSC on x86-64, the virtual counter on ARM64 (its
 * rate differs; every user converts through a measured rate). */
static inline u64 eng_cycles(void)
{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
    u64 v; __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v)); return v;
#else
    return 0;
#endif
}
static inline u64 eng_rdtsc(void)
{
    if (__builtin_expect(s_eng_prof <= 0, 1)) {
        if (s_eng_prof < 0) s_eng_prof = getenv("RSX_ENG_STATS") ? 1 : 0;
        if (!s_eng_prof) return 0;
    }
    return eng_cycles();
}
/* The frame-drop report's few (RSX_HITCH): two a draw and the rare events. It used to
 * turn on all the stage counters above too, a few dozen rdtsc a draw for numbers it never printed. */
static int s_eng_hf = -1;
static inline u64 eng_hf_tsc(void)
{
    if (__builtin_expect(s_eng_hf <= 0, 1)) {
        if (s_eng_hf < 0) s_eng_hf = (getenv("RSX_ENG_STATS") || getenv("RSX_HITCH")) ? 1 : 0;
        if (!s_eng_hf) return 0;
    }
    return eng_cycles();
}
/* Defined with the memory budgets below. */
extern float g_rsx_res_scale;
float rsx_depth_target_scale(u32 w, u32 h);
int rsx_depth_target_is_map(u32 w, u32 h);
#define ENG_VP_CB_BYTES    ((RSX_DSP_NUM_CONSTANTS + 2) * 16u)
/* g.vp_cb changed since it was last handed to the backend / which buffer the
 * backend holds now (the depth-as-colour pass binds its own). */
static int s_vp_cb_changed = 1;
static const void* s_vp_cb_bound;
/* The bytes of g.vp_cb written since the last bind (lo >= hi: none), for a
 * backend that takes only what changed (bind_vs_constants_delta). */
static u32 s_vp_dlo = 0, s_vp_dhi = 0xFFFFFFFFu;
static inline void eng_vp_dirty(u32 lo, u32 hi)
{
    if (s_vp_dlo >= s_vp_dhi) { s_vp_dlo = lo; s_vp_dhi = hi; return; }
    if (lo < s_vp_dlo) s_vp_dlo = lo;
    if (hi > s_vp_dhi) s_vp_dhi = hi;
}
#define ENG_FP_MAX_BYTES   0x10000u

#define M_ALPHA_TEST_ENABLE   0x0304
#define M_ALPHA_FUNC          0x0308
#define M_ALPHA_REF           0x030C
#define M_BLEND_ENABLE        0x0310
#define M_BLEND_SFACTOR       0x0314
#define M_BLEND_DFACTOR       0x0318
#define M_BLEND_COLOR         0x031C
#define M_BLEND_EQUATION      0x0320
#define M_COLOR_MASK          0x0324
#define M_STENCIL_TEST_ENABLE 0x0328
#define M_STENCIL_MASK        0x032C
#define M_STENCIL_FUNC        0x0330
#define M_STENCIL_FUNC_REF    0x0334
#define M_STENCIL_FUNC_MASK   0x0338
#define M_STENCIL_OP_FAIL     0x033C
#define M_STENCIL_OP_ZFAIL    0x0340
#define M_STENCIL_OP_ZPASS    0x0344
#define M_TWO_SIDED_STENCIL   0x0348
#define M_BACK_STENCIL_FUNC   0x0350
#define M_BACK_STENCIL_OP_FAIL  0x035C
#define M_BACK_STENCIL_OP_ZFAIL 0x0360
#define M_BACK_STENCIL_OP_ZPASS 0x0364
#define M_SCISSOR_HORIZONTAL  0x08C0
#define M_SCISSOR_VERTICAL    0x08C4
#define M_DEPTH_FUNC          0x0A6C
#define M_DEPTH_WRITE         0x0A70
#define M_DEPTH_TEST_ENABLE   0x0A74
#define M_ZSTENCIL_CLEAR      0x1D8C

/* ---- module state -------------------------------------------------------- */

typedef struct {
    u32 location, offset;
    u32 w, h;
    rsx_be_format fmt;
    u32 handle;
    int color_written;     /* a draw with colour writes has targeted it */
    u32 draws_since_present, last_clip;   /* RSX_ENG_PRESENT_LOG */
    u64 last_write, last_use;             /* g.surf_serial at the last draw into / bind of it */
    u64 synced_serial;                    /* last_write when guest memory last got its pixels */
    u32 pitch;                            /* SET_SURFACE_PITCH when last bound (0 = unknown) */
    u32 rsx_fmt, raster;                  /* SURFACE_FORMAT colour field and type when last bound */
    u32 ww_epoch;                         /* rsx_ww epoch of the last draw into it */
    u32 ww_check_frame;                   /* g.frames + 1 when ww_stale was last computed */
    u8  ww_stale;                         /* the game wrote its memory after that draw */
    u32 own_snap, own_snap_of;            /* the copy a draw reading this target samples, and of which handle */
    u64 own_snap_at;                      /* last_write when that copy was current */
    u32 copy_h;                           /* a copy target (made by a blit): rows [0, copy_h) hold
                                           * what the GPU copied; 0 = a drawn target, all rows */
    u8  srgb_written;                     /* its last draw ran with the sRGB write (eng_pipeline_get) */
    u32 edge_w, edge_h;                   /* eng_copy_target_edges: the texture size its edges */
    u64 edge_serial;                      /* were last spread for, and last_write then */
} eng_surface;

typedef struct {
    u32 location, offset;
    u32 w, h;
    u32 handle;
    u32 snapshot;          /* sampleable copy, or 0                        */
    int cleared;
    int had_write;
    int snapshot_valid;
    u32 as_color;          /* its D24S8 bytes as an ARGB8 target, or 0      */
    u32 used_w, used_h;    /* the area the last pass using it rendered     */
    u32 as_color_w, as_color_h;
    int as_color_valid;
    u32 as_color_spare, spare_w, spare_h;   /* the one for the other size it is used at */
    u64 last_use;          /* g.surf_serial at the last bind: LRU eviction */
    u64 last_write;        /* g.surf_serial at the last depth-writing draw  */
    u32 snap_w, snap_h;    /* the snapshot texture's size (0 = the used area) */
    u32 ww_epoch;          /* rsx_ww epoch of the last depth write or clear */
    u32 ww_check_frame;    /* g.frames + 1 when ww_stale was last computed */
    u8  ww_stale;          /* the game wrote its memory after that */
    u32 big_frame;         /* g.frames + 1 when a pass last needed more than its shadow passes */
    u32 sh_w, sh_h;        /* the largest shadow-map pass at its address (eng_zdepth_get_alloc) */
    u32 bpp;               /* the zeta's bytes a pixel: 2 (Z16) or 4 (Z24S8); 0 = not drawn yet */
} eng_zdepth;

typedef struct {
    u32 location, offset, format, width, height, pitch, remap, cubemap;
    u32 handle;
    u32 levels;            /* the mip count it was created with */
    u32 span;              /* its byte span at `levels` (eng_texture_span) */
    u32 ww_epoch;          /* rsx_ww epoch its content was last checked in (0 = never) */
    u64 content_hash;
    u32 last_hash_frame;
    u64 last_use_serial;
    u32 last_use_frame;    /* g.frames at the last lookup: never evicted while in use */
    u32 fails;             /* creates that failed in a row: the retry waits 30 << fails frames */
} eng_texture;

typedef struct { u64 key; u32 handle; u8 fixed; u32 used; } eng_pipeline;   /* used: g.frames at the last lookup */

typedef struct { u32 first, count; } eng_batch;

typedef struct {
    u32 location, offset, pitch, width, height;
    int valid;
} eng_display_buffer;

static struct {
    const rsx_draw_backend* be;
    int ready;
    int default_on;
    u32 width, height;

    rsx_dispatch rsx;

    eng_surface surfaces[ENG_MAX_SURFACES];
    u32 n_surfaces;
    eng_zdepth zdepths[ENG_MAX_ZDEPTHS];
    u32 n_zdepths;
    /* (location, offset) -> slots, as chains of slot + 1 (0 ends one). Every
     * draw asks "which target / depth buffer is at this address" once per
     * texture unit, and scanning both lists for it was most of the draw's
     * cost once they held a few hundred entries. Rebuilt when addr_dirty. */
    u16 surf_head[ENG_ADDR_BUCKETS], surf_next[ENG_MAX_SURFACES];
    u16 z_head[ENG_ADDR_BUCKETS], z_next[ENG_MAX_ZDEPTHS];
    int addr_dirty;
    /* Bumped whenever what a texture unit resolves to could change other
     * than through its own registers: a target or depth buffer appears,
     * is first drawn into, or a cached texture is replaced or evicted. */
    u32 bind_epoch;
    eng_texture textures[ENG_MAX_TEXTURES];
    u32 n_textures;
    u16 tex_index[ENG_TEX_INDEX];        /* identity -> textures[] + 1 */
    eng_pipeline pipelines[ENG_MAX_PIPELINES];
    u32 pipe_index[ENG_PIPE_INDEX];      /* key -> pipelines[] + 1 */
    u32 vp_cache_gen, vp_cache_start, vp_cache_instrs;
    u64 vp_cache_hash;
    int vp_uses_bbits;         /* the program has BRB/CLB: key on TRANSFORM_BRANCH_BITS */
    u32 n_pipelines;
    eng_display_buffer display_buffers[8];

    u32 frames;
    u64 texture_use_serial;
    u32 guest_draws;
    u32 last_guest_draws;
    u32 last_present_surface;
    u64 surf_serial;
    u32 last_flip_buffer;

    /* staging: decoded texture levels, the constant blocks, the index list */
    u8* tex_staging;
    u32 tex_staging_cap;
    u8* vp_cb;
    u8* fp_cb;
    u32 fp_cb_cap;
    u32* indices;
    u32 index_cap;

    rsx_fp_constant_block fp_constants;
} g;

/* The decompilers' HLSL. The VP decompiler builds bodies up to 192 KB. */
static char s_vs_hlsl[256 * 1024];
static char s_ps_hlsl[256 * 1024];

/* The draw accumulator: one BEGIN_END group's batches, references and cuts. */
static struct {
    /* Grown as needed: a group may carry any number of DRAW_ARRAYS /
     * DRAW_INDEX_ARRAY ranges (each at most 256 vertices). They were capped
     * at 256 ranges, and whatever came after was dropped without a word. */
    eng_batch* arr;
    u32 n_arr, cap_arr;
    eng_batch* idx;
    u32 n_idx, cap_idx;
    u32 n_packets;

    rsx_vertex_ref* refs;
    u32 n_refs, cap_refs, n_source_refs;
    int refs_remapped;
    rsx_vertex_remap ref_remap;

    u32* cuts;
    u32 n_cuts, cap_cuts;

    u8* verts;
    u64 verts_cap;
    u32 n_verts;

    rsx_vertex_layout_plan layout;
    rsx_vertex_fetch_plan fetch_plan;
    int fetch_ok;

    /* INLINE_ARRAY stream for this group, when the vertices came down the
     * FIFO rather than from a vertex array. */
    const u8* inl;
    u32 inl_bytes;
} dc;

/* ---- guest memory -------------------------------------------------------- */

/* rsx_dsp_* locations are RSX_LOCATION_LOCAL = 0 / _MAIN = 1;
 * cellGcmResolveLocated's argument is the opposite sense (1 = local), and
 * getting that backwards resolves every VRAM object through the IO table. */
static rsx_vertex_guest_ptr_fn s_guest_reader;
static void* s_guest_user;

void rsx_draw_engine_set_guest_memory(rsx_vertex_guest_ptr_fn reader, void* user)
{
    s_guest_reader = reader;
    s_guest_user = user;
}

static const u8* eng_guest_ptr(void* user, u32 location, u32 offset,
                               u32 min_bytes)
{
    (void)user;
    if (!min_bytes || min_bytes > (256u << 20)) return NULL;
    if (s_guest_reader) return s_guest_reader(s_guest_user, location, offset, min_bytes);
    if (!vm_base) return NULL;
    const u32 ea = cellGcmResolveLocated(location == RSX_LOCATION_LOCAL, offset);
    if (!ea) return NULL;
    if (ppu_vm_size && (u64)ea + min_bytes > ppu_vm_size) return NULL;
    return vm_base + ea;
}

/* FNV-1a over 8-byte words (then the tail bytewise): an in-process cache
 * key, so it only has to be stable within a run, and per draw it hashes a
 * vertex program of up to 544 x 16 bytes -- bytewise that was a third of
 * the engine's time per draw. */
static u64 eng_fnv1a(const void* data, u32 n, u64 hash)
{
    const u8* p = (const u8*)data;
    u32 i = 0;
    for (; i + 8 <= n; i += 8) { u64 w; memcpy(&w, p + i, 8); hash ^= w; hash *= 1099511628211ull; }
    /* A 4-byte tail (most callers hash one u32 field) as one step, not four:
     * the render-state key alone was ~120 dependent multiplies a draw. */
    for (; i + 4 <= n; i += 4) { u32 w; memcpy(&w, p + i, 4); hash ^= w; hash *= 1099511628211ull; }
    for (; i < n; i++) { hash ^= p[i]; hash *= 1099511628211ull; }
    return hash;
}

/* xxh64-style 4-lane hash: validating a cached draw hashes its raw guest
 * bytes on every use, so this has to run at memory speed. */
static inline u64 eng_rotl(u64 x, int r) { return (x << r) | (x >> (64 - r)); }
static u64 eng_hash_bytes(const void* data, size_t n, u64 seed)
{
    const u64 P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full;
    const u8* p = (const u8*)data;
    u64 a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1;
    while (n >= 32) {
        u64 w[4]; memcpy(w, p, 32);
        a = eng_rotl(a + w[0] * P2, 31) * P1;
        b = eng_rotl(b + w[1] * P2, 31) * P1;
        c = eng_rotl(c + w[2] * P2, 31) * P1;
        d = eng_rotl(d + w[3] * P2, 31) * P1;
        p += 32; n -= 32;
    }
    u64 h = eng_rotl(a, 1) + eng_rotl(b, 7) + eng_rotl(c, 12) + eng_rotl(d, 18) + n;
    while (n >= 8) { u64 w; memcpy(&w, p, 8); h = eng_rotl(h ^ (eng_rotl(w * P2, 31) * P1), 27) * P1; p += 8; n -= 8; }
    while (n) { h = eng_rotl(h ^ (*p * P1), 11) * P2; p++; n--; }
    h ^= h >> 33; h *= P2; h ^= h >> 29;
    return h;
}

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
/* Content hash for long runs (geometry and texture validation hash their raw
 * guest bytes, megabytes a frame in a city): an XXH3-style accumulator --
 * per 64-byte stripe, (data ^ key) lo*hi 32x32->64 products plus the data
 * with its 64-bit halves swapped, eight 64-bit lanes in four SSE2 registers,
 * scrambled every 1 KB -- about three times the scalar loop's speed. Only
 * ever compared with itself within a run. */
static const u64 s_eng_secret[24] = {
    0xBE4BA423396CFEB8ull, 0x1CAD21F72C81017Cull, 0xDB979083E96DD4DEull, 0x1F67B3B7A4A44072ull,
    0x78E5C0CC4EE679CBull, 0x2172FFCC7DD05A82ull, 0x8E2443F7744608B8ull, 0x4C263A81E69035E0ull,
    0xCB00C391BB52283Cull, 0xA32E531B8B65D088ull, 0x4EF90DA297486471ull, 0xD8ACDEA946EF1938ull,
    0x3F349CE33F76FAA8ull, 0x1D4F0BC7C7BBDCF9ull, 0x3159B4CD4BE0518Aull, 0x647378D9C97E9FC8ull,
    0xC3EBD33483ACC5EAull, 0xEB6313FAFFA081C5ull, 0x49DAF0B751DD0D17ull, 0x9E68D429265516D3ull,
    0xFCA1477D58BE162Bull, 0xCE31D07AD1B8F88Full, 0x280416958F3ACB45ull, 0x7E404BBBCAFBD7AFull };
static u64 eng_hash_long(const void* data, size_t n, u64 seed)
{
    const u8* p = (const u8*)data;
    __m128i acc[4];
    acc[0] = _mm_set_epi64x((long long)0x9E3779B185EBCA87ull, (long long)(seed ^ 0xC2B2AE3D27D4EB4Full));
    acc[1] = _mm_set_epi64x((long long)0x165667B19E3779F9ull, (long long)(seed + 0x27D4EB2F165667C5ull));
    acc[2] = _mm_set_epi64x((long long)0x85EBCA77C2B2AE63ull, (long long)(seed - 0x85EBCA77C2B2AE63ull));
    acc[3] = _mm_set_epi64x((long long)0x27D4EB2F165667C5ull, (long long)(seed ^ 0x9E3779B97F4A7C15ull));
    const __m128i prime = _mm_set1_epi32((int)0x9E3779B1u);
    const size_t stripes = n / 64u;
    for (size_t st = 0; st < stripes; st++) {
        const u8* q = p + st * 64u;
        const u8* k = (const u8*)s_eng_secret + (st & 15u) * 8u;
        for (int i = 0; i < 4; i++) {
            const __m128i d = _mm_loadu_si128((const __m128i*)(q + i * 16));
            const __m128i dk = _mm_xor_si128(d, _mm_loadu_si128((const __m128i*)(k + i * 16)));
            const __m128i prod = _mm_mul_epu32(dk, _mm_shuffle_epi32(dk, _MM_SHUFFLE(0, 3, 0, 1)));
            acc[i] = _mm_add_epi64(acc[i], _mm_add_epi64(prod, _mm_shuffle_epi32(d, _MM_SHUFFLE(1, 0, 3, 2))));
        }
        if ((st & 15u) == 15u)
            for (int i = 0; i < 4; i++) {
                __m128i a = _mm_xor_si128(acc[i], _mm_srli_epi64(acc[i], 47));
                a = _mm_xor_si128(a, _mm_loadu_si128((const __m128i*)((const u8*)s_eng_secret + 128 + i * 16)));
                const __m128i lo = _mm_mul_epu32(a, prime);
                const __m128i hi = _mm_mul_epu32(_mm_shuffle_epi32(a, _MM_SHUFFLE(3, 3, 1, 1)), prime);
                acc[i] = _mm_add_epi64(lo, _mm_slli_epi64(hi, 32));
            }
    }
    u64 lanes[8];
    for (int i = 0; i < 4; i++) _mm_storeu_si128((__m128i*)(lanes + i * 2), acc[i]);
    u64 h = eng_hash_bytes(p + stripes * 64u, n - stripes * 64u, seed ^ (u64)n);
    for (int i = 0; i < 8; i++) h = eng_rotl(h ^ (lanes[i] * 0xC2B2AE3D27D4EB4Full), 27) * 0x9E3779B185EBCA87ull;
    h ^= h >> 33; h *= 0xC2B2AE3D27D4EB4Full; h ^= h >> 29;
    return h;
}
#elif defined(__aarch64__)
#include <arm_neon.h>
/* The same accumulator in NEON (Android/arm64), lane for lane the SSE2 one:
 * vmull_u32 of the narrowed low and high halves is _mm_mul_epu32's lo*hi
 * product, vextq_u64 the 64-bit half swap. Without it arm64 took the scalar
 * loop -- three times slower on megabytes a frame. */
static const u64 s_eng_secret[24] = {
    0xBE4BA423396CFEB8ull, 0x1CAD21F72C81017Cull, 0xDB979083E96DD4DEull, 0x1F67B3B7A4A44072ull,
    0x78E5C0CC4EE679CBull, 0x2172FFCC7DD05A82ull, 0x8E2443F7744608B8ull, 0x4C263A81E69035E0ull,
    0xCB00C391BB52283Cull, 0xA32E531B8B65D088ull, 0x4EF90DA297486471ull, 0xD8ACDEA946EF1938ull,
    0x3F349CE33F76FAA8ull, 0x1D4F0BC7C7BBDCF9ull, 0x3159B4CD4BE0518Aull, 0x647378D9C97E9FC8ull,
    0xC3EBD33483ACC5EAull, 0xEB6313FAFFA081C5ull, 0x49DAF0B751DD0D17ull, 0x9E68D429265516D3ull,
    0xFCA1477D58BE162Bull, 0xCE31D07AD1B8F88Full, 0x280416958F3ACB45ull, 0x7E404BBBCAFBD7AFull };
static u64 eng_hash_long(const void* data, size_t n, u64 seed)
{
    const u8* p = (const u8*)data;
    uint64x2_t acc[4];
    acc[0] = vcombine_u64(vcreate_u64(seed ^ 0xC2B2AE3D27D4EB4Full), vcreate_u64(0x9E3779B185EBCA87ull));
    acc[1] = vcombine_u64(vcreate_u64(seed + 0x27D4EB2F165667C5ull), vcreate_u64(0x165667B19E3779F9ull));
    acc[2] = vcombine_u64(vcreate_u64(seed - 0x85EBCA77C2B2AE63ull), vcreate_u64(0x85EBCA77C2B2AE63ull));
    acc[3] = vcombine_u64(vcreate_u64(seed ^ 0x9E3779B97F4A7C15ull), vcreate_u64(0x27D4EB2F165667C5ull));
    const uint32x2_t prime = vdup_n_u32(0x9E3779B1u);
    const size_t stripes = n / 64u;
    for (size_t st = 0; st < stripes; st++) {
        const u8* q = p + st * 64u;
        const u8* k = (const u8*)s_eng_secret + (st & 15u) * 8u;
        for (int i = 0; i < 4; i++) {
            const uint64x2_t d = vreinterpretq_u64_u8(vld1q_u8(q + i * 16));
            const uint64x2_t dk = veorq_u64(d, vreinterpretq_u64_u8(vld1q_u8(k + i * 16)));
            const uint64x2_t prod = vmull_u32(vmovn_u64(dk), vshrn_n_u64(dk, 32));
            acc[i] = vaddq_u64(acc[i], vaddq_u64(prod, vextq_u64(d, d, 1)));
        }
        if ((st & 15u) == 15u)
            for (int i = 0; i < 4; i++) {
                uint64x2_t a = veorq_u64(acc[i], vshrq_n_u64(acc[i], 47));
                a = veorq_u64(a, vreinterpretq_u64_u8(vld1q_u8((const u8*)s_eng_secret + 128 + i * 16)));
                const uint64x2_t lo = vmull_u32(vmovn_u64(a), prime);
                const uint64x2_t hi = vmull_u32(vshrn_n_u64(a, 32), prime);
                acc[i] = vaddq_u64(lo, vshlq_n_u64(hi, 32));
            }
    }
    u64 lanes[8];
    for (int i = 0; i < 4; i++) vst1q_u64(lanes + i * 2, acc[i]);
    u64 h = eng_hash_bytes(p + stripes * 64u, n - stripes * 64u, seed ^ (u64)n);
    for (int i = 0; i < 8; i++) h = eng_rotl(h ^ (lanes[i] * 0xC2B2AE3D27D4EB4Full), 27) * 0x9E3779B185EBCA87ull;
    h ^= h >> 33; h *= 0xC2B2AE3D27D4EB4Full; h ^= h >> 29;
    return h;
}
#else
#define eng_hash_long eng_hash_bytes
#endif

/* ---- render state -------------------------------------------------------- */

void rsx_draw_engine_decode_render_state(const rsx_dispatch* rsx,
                                         rsx_be_render_state* rs)
{
    memset(rs, 0, sizeof(*rs));
    rs->alpha_test_enable = rsx_dsp_reg(rsx, M_ALPHA_TEST_ENABLE) & 1;
    rs->alpha_func   = rsx_dsp_reg(rsx, M_ALPHA_FUNC);
    /* The RSX takes a compare in either encoding: 0..7 is 0x200..0x207
     * (RPCS3 to_comparison_function accepts both). A Crack in Time draws its
     * teleport and pad-shimmer refraction quads with the test on and func 0,
     * NEVER: the console discards every pixel. Read as "not a compare" (so
     * ALWAYS), each quad landed opaque -- blending is off -- as a hard-edged
     * block of displaced scene. */
    if (rs->alpha_func < 8u) rs->alpha_func |= 0x0200u;
    rs->alpha_ref_raw = rsx_dsp_reg(rsx, M_ALPHA_REF);
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(rsx, &sf);
    rs->alpha_ref_format = sf.color_format;
    rs->rt_fp16 = sf.color_format == RSX_SURFACE_FMT_F_W16Z16Y16X16;
    rs->blend_enable = rsx_dsp_reg(rsx, M_BLEND_ENABLE) & 1;
    const u32 sfac = rsx_dsp_reg(rsx, M_BLEND_SFACTOR);
    const u32 dfac = rsx_dsp_reg(rsx, M_BLEND_DFACTOR);
    const u32 eq   = rsx_dsp_reg(rsx, M_BLEND_EQUATION);
    rs->sf_rgb = sfac & 0xFFFF; rs->sf_a = sfac >> 16;
    rs->df_rgb = dfac & 0xFFFF; rs->df_a = dfac >> 16;
    rs->eq_rgb = eq & 0xFFFF;   rs->eq_a = eq >> 16;
    rs->depth_test  = rsx_dsp_reg(rsx, M_DEPTH_TEST_ENABLE) & 1;
    rs->depth_write = rsx_dsp_reg(rsx, M_DEPTH_WRITE) & 1;
    rs->depth_func  = rsx_dsp_reg(rsx, M_DEPTH_FUNC);
    rs->cull_enable = rsx_dsp_reg(rsx, 0x183C) & 1;
    rs->cull_face   = rsx_dsp_reg(rsx, 0x1830);
    rs->front_face  = rsx_dsp_reg(rsx, 0x1834);
    /* The RAW register: a game-written 0 is a real "write no colour channel"
     * (a depth-prime pass), and rsx_dispatch_init seeds the nv40 reset value
     * so never-written reads as all-on. */
    rs->color_mask  = rsx_dsp_reg(rsx, M_COLOR_MASK);
    rs->stencil_enable    = rsx_dsp_reg(rsx, M_STENCIL_TEST_ENABLE) & 1;
    rs->stencil_two_sided = rsx_dsp_reg(rsx, M_TWO_SIDED_STENCIL) & 1;
    rs->s_func       = rsx_dsp_reg(rsx, M_STENCIL_FUNC);
    rs->s_func_mask  = rsx_dsp_reg(rsx, M_STENCIL_FUNC_MASK) & 0xFF;
    rs->s_write_mask = rsx_dsp_reg(rsx, M_STENCIL_MASK) & 0xFF;
    rs->s_fail       = rsx_dsp_reg(rsx, M_STENCIL_OP_FAIL);
    rs->s_zfail      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZFAIL);
    rs->s_zpass      = rsx_dsp_reg(rsx, M_STENCIL_OP_ZPASS);
    rs->bs_func      = rsx_dsp_reg(rsx, M_BACK_STENCIL_FUNC);
    rs->bs_fail      = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_FAIL);
    rs->bs_zfail     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZFAIL);
    rs->bs_zpass     = rsx_dsp_reg(rsx, M_BACK_STENCIL_OP_ZPASS);
    rs->poly_offset_fill = rsx_dsp_reg(rsx, 0x0A68) & 1;          /* POLY_OFFSET_FILL_ENABLE */
    { const u32 sfac = rsx_dsp_reg(rsx, 0x0A78), bias = rsx_dsp_reg(rsx, 0x0A7C);
      memcpy(&rs->poly_scale, &sfac, 4); memcpy(&rs->poly_bias, &bias, 4); }
    if (!rs->poly_offset_fill || !(rs->poly_scale == rs->poly_scale) || !(rs->poly_bias == rs->poly_bias))
        rs->poly_scale = rs->poly_bias = 0.0f;
    /* What a disabled stage would use is not state: A Crack in Time leaves
     * junk in the blend factor registers while blending is off (0x40/0xFFFF/
     * 0x4D/0x1F...), and every new junk value made a new pipeline -- 8192 in
     * five minutes of Zolar Forest, the table full and draws dropped, a
     * driver compile (a hitch) per pipeline and memory growing all along.
     * Each field reads as the reset value its disabled stage implies. */
    if (!rs->blend_enable) {
        rs->sf_rgb = rs->sf_a = 0x0001u; rs->df_rgb = rs->df_a = 0x0000u;   /* ONE, ZERO */
        rs->eq_rgb = rs->eq_a = 0x8006u;                                    /* FUNC_ADD */
    }
    if (!rs->alpha_test_enable) rs->alpha_func = 0x0207u;                  /* ALWAYS */
    if (!rs->depth_test) { rs->depth_func = 0x0207u; rs->depth_write = 0; }  /* GL: no test, no write */
    if (!rs->cull_enable) rs->cull_face = 0x0405u;                          /* BACK; front_face stays: two-sided stencil reads it */
    if (!rs->stencil_enable) {
        rs->stencil_two_sided = 0;
        rs->s_func = 0x0207u; rs->s_func_mask = rs->s_write_mask = 0xFFu;
        rs->s_fail = rs->s_zfail = rs->s_zpass = 0x1E00u;                    /* KEEP */
    }
    if (!rs->stencil_two_sided) { rs->bs_func = 0; rs->bs_fail = rs->bs_zfail = rs->bs_zpass = 0; }
}

u64 rsx_draw_engine_hash_render_state(const rsx_be_render_state* rs, u64 hash)
{
#define ENG_HASH_FIELD(name) hash = eng_fnv1a(&rs->name, sizeof(rs->name), hash)
    /* The alpha REFERENCE is deliberately absent: it lives in the fragment
     * constant block, so a title animating a fade must not compile a new
     * pipeline per frame. Enable and compare mode still select a variant.
     * Every field is listed by name so struct padding can never be identity
     * (rsx_live_draw.c:3151-3185). */
    ENG_HASH_FIELD(alpha_test_enable);
    ENG_HASH_FIELD(alpha_func);
    ENG_HASH_FIELD(blend_enable);
    ENG_HASH_FIELD(sf_rgb);
    ENG_HASH_FIELD(df_rgb);
    ENG_HASH_FIELD(sf_a);
    ENG_HASH_FIELD(df_a);
    ENG_HASH_FIELD(eq_rgb);
    ENG_HASH_FIELD(eq_a);
    ENG_HASH_FIELD(depth_test);
    ENG_HASH_FIELD(depth_write);
    ENG_HASH_FIELD(depth_func);
    ENG_HASH_FIELD(cull_enable);
    ENG_HASH_FIELD(cull_face);
    ENG_HASH_FIELD(front_face);
    ENG_HASH_FIELD(color_mask);
    ENG_HASH_FIELD(rt_fp16);
    ENG_HASH_FIELD(stencil_enable);
    ENG_HASH_FIELD(stencil_two_sided);
    ENG_HASH_FIELD(s_func);
    ENG_HASH_FIELD(s_func_mask);
    ENG_HASH_FIELD(s_write_mask);
    ENG_HASH_FIELD(s_fail);
    ENG_HASH_FIELD(s_zfail);
    ENG_HASH_FIELD(s_zpass);
    ENG_HASH_FIELD(bs_func);
    ENG_HASH_FIELD(bs_fail);
    ENG_HASH_FIELD(bs_zfail);
    ENG_HASH_FIELD(bs_zpass);
    ENG_HASH_FIELD(poly_offset_fill);
#undef ENG_HASH_FIELD
    return hash;
}

/* ---- topology expansion -------------------------------------------------- */

/* Every expansion is bounded by the restart cuts, so a strip that the guest
 * broke with the sentinel index gets no connecting triangle across the break.
 * Missing that decode is what upstream's aa7fb63 records as "the exploded
 * spiky mesh that occluded the scene". */
u32 rsx_draw_engine_topology_index_count(u32 primitive, u32 source_refs,
                                         const u32* cuts, u32 cut_count)
{
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        return source_refs - source_refs % 3u;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON: {
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 3) total += (count - 2) * 3;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUAD_STRIP: {
        /* Two vertices per quad after the first pair, so a segment of n
         * carries (n - 2) / 2 quads and an odd trailing vertex is dropped. */
        u32 total = 0;
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            (void)begin;
            if (count >= 4) total += ((count - 2u) / 2u) * 6u;
        }
        return total;
    }
    case RSX_PRIMITIVE_QUADS:
        return (source_refs / 4u) * 6u;
    default:
        return 0;
    }
}

static u32 eng_topology_vertex(const u32* occurrence_to_unique, u32 occurrence)
{
    return occurrence_to_unique ? occurrence_to_unique[occurrence] : occurrence;
}

void rsx_draw_engine_write_topology_indices(u32 primitive, u32 source_refs,
                                            const u32* cuts, u32 cut_count,
                                            const u32* occurrence_to_unique,
                                            u32* indices)
{
    u32 write = 0;
    const u32 segments = cut_count + 1;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES: {
        const u32 count = source_refs - source_refs % 3u;
        for (u32 o = 0; o < count; o++)
            indices[write++] = eng_topology_vertex(occurrence_to_unique, o);
        break;
    }
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            /* Odd triangles swap their first two vertices, so the whole strip
             * keeps one winding. */
            for (u32 i = 0; i + 2 < count; i++) {
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 1u - (i & 1u));
                indices[write++] = eng_topology_vertex(
                    occurrence_to_unique, begin + i + 2u);
            }
        }
        break;
    /* A polygon is a convex fan around its own first vertex, which is what
     * both of this tree's other renderers make of one. */
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 3) continue;
            for (u32 i = 1; i + 1 < count; i++) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + i + 1u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 s = 0; s < segments; s++) {
            u32 begin, count;
            rsx_restart_segment_bounds(cuts, cut_count, source_refs, s,
                                       &begin, &count);
            if (count < 4) continue;
            /* Every quad is split along the diagonal between the two pairs,
             * so each one keeps the strip's winding without alternating the
             * way a triangle strip has to; a cut simply starts the pairing
             * again from the segment's own first vertex. This is the
             * expansion the vtable path's emit_vertices performs. */
            for (u32 q = 0; q + 3 < count; q += 2) {
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 1u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 3u);
                indices[write++] = eng_topology_vertex(occurrence_to_unique, begin + q + 2u);
            }
        }
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 q = 0; q < source_refs / 4u; q++) {
            const u32 base = q * 4u;
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 1u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 2u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base + 3u);
            indices[write++] = eng_topology_vertex(occurrence_to_unique, base);
        }
        break;
    default:
        break;
    }
}

/* Does this primitive become an indexed triangle list, and is the index buffer
 * needed at all? rsx_vertex_topology_plan answers that for the reference
 * engine, whose own expansion stops at quads. Quad strips and polygons are
 * this engine's, so their two cases are decided here rather than in the
 * shared helper, which the vendored rsx_live_draw.c also calls. */
static int eng_topology_rebuild(u32 primitive, int refs_remapped, int* indexed)
{
    if (primitive == RSX_PRIMITIVE_QUAD_STRIP ||
        primitive == RSX_PRIMITIVE_POLYGON) {
        (void)refs_remapped;
        *indexed = 1;
        return 1;
    }
    return rsx_vertex_topology_plan(primitive, refs_remapped, indexed);
}

/* ---- surfaces ------------------------------------------------------------ */

static rsx_be_format eng_surface_format(u32 color_format)
{
    switch (color_format & 0x1Fu) {
    case RSX_SURFACE_FMT_F_W16Z16Y16X16: return RSX_BE_FMT_R16G16B16A16F;
    case 0x0C: return RSX_BE_FMT_R32G32B32A32F;
    case 0x0D: return RSX_BE_FMT_R32F;
    default:   return RSX_BE_FMT_R8G8B8A8;
    }
}

/* Decode the guest's own bytes behind a colour surface into host R,G,B,A rows
 * so a freshly created target starts with what the title CPU-initialised it
 * to. Returns the staging pointer, or NULL when the bytes are unreadable or
 * the format is not one the decoder handles. */
static const void* eng_surface_seed(u32 location, u32 offset, u32 w, u32 h,
                                    rsx_be_format fmt, u32* out_row_bytes)
{
    *out_row_bytes = 0;
    if (fmt != RSX_BE_FMT_R8G8B8A8) return NULL;
    /* Only a surface in IO-mapped MAIN memory is seeded. RSX local memory is
     * reserved and not backed by this tree's guest VM -- runtime/memory/vm.h
     * maps the whole space PROT_NONE and commits main memory and the stacks,
     * leaving a runner to commit what its own title needs -- so reading a VRAM
     * surface's backing would fault long before it could help. Both halves are
     * checked: the DMA context says which space the offset is in, and the IO
     * table says the page is really mapped. */
    if (location != RSX_LOCATION_MAIN ||
        (!s_guest_reader && !cellGcmResolveIO(offset))) return NULL;
    rsx_tex_layout tl;
    /* A8R8G8B8 with the LN bit: a surface is linear, never swizzled. */
    rsx_texture_layout(0x85u | 0x20u, w, h, &tl);
    if (!tl.face_bytes || !tl.dst_row_bytes) return NULL;
    const u8* src = eng_guest_ptr(NULL, location, offset, tl.face_bytes);
    if (!src) return NULL;
    const u32 bytes = tl.dst_row_bytes * tl.rows;
    if (g.tex_staging_cap < bytes) {
        u8* n = (u8*)realloc(g.tex_staging, bytes);
        if (!n) return NULL;
        g.tex_staging = n;
        g.tex_staging_cap = bytes;
    }
    rsx_texture_decode(g.tex_staging, tl.dst_row_bytes, src, w, h, &tl,
                       rsx_texture_argb_is_rgba());
    *out_row_bytes = tl.dst_row_bytes;
    return g.tex_staging;
}

static inline u32 eng_addr_bucket(u32 location, u32 offset)
{
    return (u32)(((((u64)location << 32) | offset) * 0x9E3779B97F4A7C15ull) >> 53) & (ENG_ADDR_BUCKETS - 1u);
}
static void eng_addr_index(void)
{
    if (!g.addr_dirty) return;
    g.addr_dirty = 0;
    memset(g.surf_head, 0, sizeof g.surf_head);
    memset(g.z_head, 0, sizeof g.z_head);
    for (u32 i = g.n_surfaces; i-- > 0;) {
        if (!g.surfaces[i].handle) continue;
        const u32 b = eng_addr_bucket(g.surfaces[i].location, g.surfaces[i].offset);
        g.surf_next[i] = g.surf_head[b]; g.surf_head[b] = (u16)(i + 1u);
    }
    for (u32 i = g.n_zdepths; i-- > 0;) {
        const u32 b = eng_addr_bucket(g.zdepths[i].location, g.zdepths[i].offset);
        g.z_next[i] = g.z_head[b]; g.z_head[b] = (u16)(i + 1u);
    }
}
/* The colour targets (with a handle) / depth buffers at an address, in slot
 * order: for (i = eng_surf_first(l, o); i != ENG_INVALID; i = eng_surf_next(i)). */
static inline u32 eng_surf_from(u32 link, u32 location, u32 offset)
{
    for (; link; link = g.surf_next[link - 1u])
        if (g.surfaces[link - 1u].location == location && g.surfaces[link - 1u].offset == offset)
            return link - 1u;
    return ENG_INVALID;
}
static inline u32 eng_surf_first(u32 location, u32 offset)
{
    eng_addr_index();
    return eng_surf_from(g.surf_head[eng_addr_bucket(location, offset)], location, offset);
}
static inline u32 eng_surf_next(u32 i)
{
    return eng_surf_from(g.surf_next[i], g.surfaces[i].location, g.surfaces[i].offset);
}
static inline u32 eng_z_from(u32 link, u32 location, u32 offset)
{
    for (; link; link = g.z_next[link - 1u])
        if (g.zdepths[link - 1u].location == location && g.zdepths[link - 1u].offset == offset)
            return link - 1u;
    return ENG_INVALID;
}
static inline u32 eng_z_first(u32 location, u32 offset)
{
    eng_addr_index();
    return eng_z_from(g.z_head[eng_addr_bucket(location, offset)], location, offset);
}
static inline u32 eng_z_next(u32 i)
{
    return eng_z_from(g.z_next[i], g.zdepths[i].location, g.zdepths[i].offset);
}

/* The target a reader of (address, w x h) means: one of exactly that size
 * if there is one, else the most recently drawn into at that address. w = 0
 * asks for the most recent. */
static u32 eng_surface_find(u32 location, u32 offset, u32 w, u32 h)
{
    u32 best = ENG_INVALID, exact = ENG_INVALID;
    for (u32 i = eng_surf_first(location, offset); i != ENG_INVALID; i = eng_surf_next(i)) {
        const eng_surface* s = &g.surfaces[i];
        if (w && s->w == w && s->h == h &&
            (exact == ENG_INVALID || s->last_write > g.surfaces[exact].last_write)) exact = i;
        if (best == ENG_INVALID || s->last_write > g.surfaces[best].last_write) best = i;
    }
    return exact != ENG_INVALID ? exact : best;
}

/* A target leaving the list (its slot is reused) takes what was drawn into
 * it along: anything that samples its address afterwards finds no target and
 * decodes guest memory -- which never received the drawing, so a backdrop or
 * map rendered once at level load came back minutes later as noise ("neon"
 * garbage on distant scenery after long play). Its pixels are written to its
 * guest memory first, as the RSX would have left them there. Only when that
 * memory still holds what it held then: if the game has written there since,
 * its data wins. Linear 32-bit ARGB targets (the A8R8G8B8 family). */
/* Retired targets whose pixels did NOT reach guest memory, for the check in
 * eng_texture_slot: a texture decoded from such memory is the noise above. */
static struct { u32 loc, off, span, frame; const char* why; } s_retired[64];
static u32 s_n_retired;
static void eng_retired_note(const eng_surface* s, u32 span, const char* why)
{
    s_retired[s_n_retired % 64u].loc = s->location; s_retired[s_n_retired % 64u].off = s->offset;
    s_retired[s_n_retired % 64u].span = span; s_retired[s_n_retired % 64u].frame = g.frames;
    s_retired[s_n_retired % 64u].why = why; s_n_retired++;
}
static const char* eng_retired_hit(u32 loc, u32 off, u32 span)
{
    const u32 n = s_n_retired < 64u ? s_n_retired : 64u;
    for (u32 i = 0; i < n; i++)
        if (s_retired[i].loc == loc && off < s_retired[i].off + s_retired[i].span && s_retired[i].off < off + span)
            return s_retired[i].why;
    return NULL;
}
/* Returns nonzero only when the pixels could not be read back (the target
 * should then stay): 0 when they are in guest memory now, were already, or
 * cannot be put there (the reason is noted). */
static int eng_surface_retire(u32 slot)
{
    eng_surface* s = &g.surfaces[slot];
    static unsigned long long n_wb, n_skip, n_ret;
    if (!s->handle || !s->color_written) return 0;
    /* Guest memory has its last drawing already (a CPU copy or blit read it
     * back): no second readback, each of which waits for the GPU. */
    if (s->synced_serial == s->last_write) return 0;
    const unsigned long long age = (unsigned long long)(g.surf_serial - s->last_use);
    const int swz = s->raster == 2;
    const char* why = NULL;
    u32 pitch = s->pitch ? s->pitch : s->w * 4u;
    if (swz) pitch = s->w * 4u;
    const u32 span = pitch * s->h;
    if (!g.be->readback || s->fmt != RSX_BE_FMT_R8G8B8A8) why = "not RGBA8";
    else if (!(s->rsx_fmt == 4 || s->rsx_fmt == 5 || s->rsx_fmt == 8)) why = "surface format";
    else if (swz && ((s->w & (s->w - 1u)) || (s->h & (s->h - 1u)))) why = "swizzled, not power of two";
    else if (pitch < s->w * 4u || (u64)span > (64u << 20)) why = "pitch";
    u8* dst = why ? NULL : (u8*)eng_guest_ptr(NULL, s->location, s->offset, span);
    if (!why && !dst) why = "unmapped";
    if (++n_ret <= 64 || (n_ret & 63u) == 0)
        fprintf(stderr, "[rsx engine] retiring target %u:0x%08X %ux%u%s (idle %llu binds; %llu retired)%c",
                s->location, s->offset, s->w, s->h, swz ? " swizzled" : "", age, n_ret, 10);
    if (why) { eng_retired_note(s, span ? span : s->w * s->h * 4u, why); return 0; }
    if (rsx_ww_on() && rsx_ww_guest_written_since(dst, span, s->ww_epoch)) { n_skip++; return 0; }
    u8* px = (u8*)malloc((size_t)s->w * s->h * 4u);
    if (!px) return -1;
    /* A failed readback writes nothing: the buffer is not pixels. */
    if (g.be->readback(g.be->user, s->handle, 0, 0, s->w, s->h, px, s->w * 4u)) { free(px); return -1; }
    u32 l2w = 0, l2h = 0;
    while ((1u << l2w) < s->w) l2w++;
    while ((1u << l2h) < s->h) l2h++;
    rsx_ww_self_begin(dst, span);
    for (u32 y = 0; y < s->h; y++)
        for (u32 x = 0; x < s->w; x++) {
            const u8* q = px + ((size_t)y * s->w + x) * 4u;
            u8* d = swz ? dst + (size_t)rsx_swizzle_offset(x, y, l2w, l2h) * 4u
                        : dst + (size_t)y * pitch + x * 4u;
            d[0] = q[3]; d[1] = q[0]; d[2] = q[1]; d[3] = q[2];   /* RGBA -> A,R,G,B */
        }
    rsx_ww_self_end(dst, span);
    free(px);
    if (++n_wb <= 16 || (n_wb & (n_wb - 1)) == 0)
        fprintf(stderr, "[rsx engine] retired target %u:0x%08X %ux%u%s written back to guest memory (%llu so far, %llu skipped: rewritten by the game)%c",
                s->location, s->offset, s->w, s->h, swz ? " (swizzled)" : "", n_wb, n_skip, 10);
    return 0;
}

static u64 s_frame_serial;              /* defined with the frame bookkeeping below */
static u32 eng_surface_span(const eng_surface* s);
static int eng_surface_writes_back(const eng_surface* s);
static int eng_superseded(u32 loc, u32 off, u32 span, u64 last_write, const void* self);
static int eng_surface_overwritten(u32 i);
static int eng_surface_rows_stale(u32 i, u32 y, u32 h);

/* Did the game write guest memory in [p, p+len) since `epoch`? Only the 4 KB
 * pages lying wholly inside count (write-watch is per page): a target that
 * does not start or end on a page shares its first and last page with other
 * data, and the game writing THAT made the target read as overwritten -- its
 * reads decoded memory for a frame (small targets: the moons' landing pads
 * flickered with dark patches, owner v81; log: 128x96 at 0x02C73280 and 16x16
 * ones at 0x02C8xxxx "overwritten by the game"). A span with no whole page
 * cannot be told apart: its GPU contents stand. eng_zdepth_overwritten
 * already did this. */
static int eng_ww_written_inner(const u8* p, u32 len, u32 epoch)
{
    if (!p || !len) return 0;
    const uintptr_t a = ((uintptr_t)p + 4095u) & ~(uintptr_t)4095u;
    const uintptr_t e = ((uintptr_t)p + len) & ~(uintptr_t)4095u;
    if (e <= a) return 0;
    return rsx_ww_watched((const void*)a, (unsigned)(e - a)) &&
           rsx_ww_guest_written_since((const void*)a, (unsigned)(e - a), epoch);
}

/* A written depth buffer dropped while it may still be read: its contents
 * are gone (a static shadow map shown again later comes back empty). */
static void eng_note_lossy_depth(const eng_zdepth* z, const char* why)
{
    static unsigned long long lossy;
    if (++lossy <= 32 || (lossy & 63u) == 0)
        fprintf(stderr, "[rsx mem] depth %u:0x%08X %ux%u dropped (%s; idle %llu binds; %llu so far): its contents are lost%c",
                z->location, z->offset, z->w, z->h, why, (unsigned long long)(g.surf_serial - z->last_use), lossy, 10);
}

/* The 16 least recently bound entries no draw of this frame used, oldest
 * first, as indices into `cand`; how many. */
#define ENG_VICTIM_CANDS 16u
static u32 eng_oldest(const u64* last_use, u32 stride, u32 n, u32 skip, u32* cand)
{
    u32 nc = 0;
    for (u32 i = 0; i < n; i++) {
        const u64 lu = *(const u64*)((const u8*)last_use + (size_t)i * stride);
        if (i == skip || lu > s_frame_serial) continue;
        u32 p;
        if (nc < ENG_VICTIM_CANDS) p = nc++;
        else if (lu < *(const u64*)((const u8*)last_use + (size_t)cand[nc - 1] * stride)) p = nc - 1;
        else continue;
        while (p > 0 && *(const u64*)((const u8*)last_use + (size_t)cand[p - 1] * stride) > lu) { cand[p] = cand[p - 1]; p--; }
        cand[p] = i;
    }
    return nc;
}

/* The slot a full colour-target list gives up, by the budget pass's tiers:
 * among the oldest, superseded or never drawn first, then colour that writes
 * back, then the oldest of the rest -- and never one whose pixels could not
 * be read back. ENG_INVALID: none qualifies (the caller keeps the old LRU). */
static u32 eng_surface_victim(void)
{
    u32 cand[ENG_VICTIM_CANDS];
    const u32 nc = eng_oldest(&g.surfaces[0].last_use, (u32)sizeof g.surfaces[0], g.n_surfaces,
                              g.last_present_surface, cand);
    u64 key[ENG_VICTIM_CANDS];
    for (u32 k = 0; k < nc; k++) {
        const eng_surface* s = &g.surfaces[cand[k]];
        const u64 tier = (!s->color_written || eng_superseded(s->location, s->offset, eng_surface_span(s), s->last_write, s) ||
                          eng_surface_overwritten(cand[k])) ? 0u : eng_surface_writes_back(s) ? 1u : 2u;
        key[k] = (tier << 60) | (s->last_use & ((1ull << 60) - 1u));
    }
    for (u32 a = 1; a < nc; a++)                    /* 16 entries: insertion sort */
        for (u32 b = a; b > 0 && key[b - 1] > key[b]; b--) {
            const u64 tk = key[b]; key[b] = key[b - 1]; key[b - 1] = tk;
            const u32 tc = cand[b]; cand[b] = cand[b - 1]; cand[b - 1] = tc;
        }
    for (u32 k = 0; k < nc; k++) {
        if (!(key[k] >> 60)) return cand[k];       /* loses nothing: no write-back either */
        if (!eng_surface_retire(cand[k])) return cand[k];
    }
    return ENG_INVALID;
}

/* The same for a full depth list: superseded or never written first; a
 * written one only as a last resort, logged. */
static u32 eng_zdepth_victim(void)
{
    u32 cand[ENG_VICTIM_CANDS];
    const u32 nc = eng_oldest(&g.zdepths[0].last_use, (u32)sizeof g.zdepths[0], g.n_zdepths, ENG_INVALID, cand);
    for (u32 k = 0; k < nc; k++) {
        const eng_zdepth* z = &g.zdepths[cand[k]];
        if (!z->had_write || eng_superseded(z->location, z->offset, z->w * z->h * 4u, z->last_write, z)) return cand[k];
    }
    if (!nc) return ENG_INVALID;
    eng_note_lossy_depth(&g.zdepths[cand[0]], "depth list full");
    return cand[0];
}

/* The colour target for this (location, offset, size, format), created when
 * there is none yet. Returns a slot index, or ENG_INVALID.
 * rsx_live_draw.c's surface_get (2605-2690). */
static u32 eng_surface_get(u32 location, u32 offset, u32 want_w, u32 want_h,
                          rsx_be_format want_fmt)
{
    if (!want_w) want_w = g.width;
    if (!want_h) want_h = g.height;

    /* Identity is (address, size, format). A title aliases one block of VRAM
     * as differently sized targets within a frame -- A Crack in Time draws a
     * 960x704 shadow mask at 0x1904100 and later a 240x176 bloom level at the
     * same address -- and reallocating one target on every size change
     * destroyed what the next reader of the other size needed (and re-seeded
     * it from guest memory, every frame). Readers pick by size and recency
     * (eng_surface_find). */
    for (u32 i = eng_surf_first(location, offset); i != ENG_INVALID; i = eng_surf_next(i))
        if (g.surfaces[i].w == want_w &&
            g.surfaces[i].h == want_h && g.surfaces[i].fmt == want_fmt) {
            g.surfaces[i].last_use = ++g.surf_serial;
            return i;
        }

    /* Never create a target because one malformed command decoded a guest
     * pointer as clip dimensions. */
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM) {
        static u32 logs = 0;
        if (logs++ < 8)
            fprintf(stderr, "[rsx engine] rejected implausible surface 0x%X %ux%u%c",
                    offset, want_w, want_h, 10);
        return eng_surface_find(location, offset, 0, 0);
    }
    u32 slot;
    if (g.n_surfaces < ENG_MAX_SURFACES) {
        slot = g.n_surfaces;
    } else {
        slot = eng_surface_victim();
        if (slot == ENG_INVALID) {                 /* least recently bound */
            slot = 0;
            for (u32 i = 1; i < g.n_surfaces; i++)
                if (g.surfaces[i].last_use < g.surfaces[slot].last_use) slot = i;
            eng_surface_retire(slot);
        }
    }

    u32 seed_row = 0;
    const void* seed = eng_surface_seed(location, offset, want_w, want_h,
                                        want_fmt, &seed_row);
    const u64 c0_ = eng_hf_tsc();
    const u32 handle = g.be->color_target_create(g.be->user, want_fmt,
                                                 want_w, want_h, seed, seed_row);
    if (!handle) return ENG_INVALID;

    eng_surface* s = &g.surfaces[slot];
    if (s->handle) g.be->color_target_release(g.be->user, s->handle);
    s_hf.create_cyc += eng_hf_tsc() - c0_;
    s->location = location; s->offset = offset;
    s->w = want_w; s->h = want_h; s->fmt = want_fmt;
    s->handle = handle;
    s->color_written = 0;
    s->last_write = 0;
    s->ww_epoch = 0; s->ww_check_frame = 0; s->ww_stale = 0;   /* a new target: no verdict of the old one */
    s->copy_h = 0;
    s->synced_serial = 0;
    s->own_snap = 0; s->own_snap_of = 0; s->own_snap_at = 0;
    s->last_use = ++g.surf_serial;
    s->draws_since_present = 0;
    if (slot == g.n_surfaces) g.n_surfaces++;
    s_hf.surfs++;
    g.addr_dirty = 1;
    g.bind_epoch++;
    /* RSX_ENG_SURF_LOG=<frame>: every new target from that frame on (400 at most). */
    { static u32 logs = 0; static long from = -2;
      if (from == -2) { const char* e = getenv("RSX_ENG_SURF_LOG"); from = e ? (atol(e) > 0 ? atol(e) : 4000) : -1; }
      if (logs++ < 16 || (from >= 0 && g.frames > (u32)from && logs < 400))
        fprintf(stderr, "[rsx engine] surface %u:0x%08X %ux%u fmt %d%s\n",
                location, offset, want_w, want_h, (int)want_fmt,
                seed ? " (seeded from guest memory)" : ""); }
    return slot;
}

/* The colour targets SET_SURFACE_COLOR_TARGET names, as slots into
 * g.surfaces, with A first. One for an ordinary draw; an MRT set adds B, C
 * and D, each created at target A's size and format, which is the rule the
 * vtable path's target_for binds by. Resolution stops at the first member
 * that cannot be created, as that path stops at the first gap. Returns how
 * many were resolved, or 0. */
/* A 4x square multisampled surface (SURFACE_FORMAT antialias 4 or 5: 2x2
 * samples a pixel) is laid out in memory as a 1x surface twice as wide and
 * twice as high. A Crack in Time draws its half-resolution particles that
 * way: 480x352 at 4x straight over the 960x704 1x image (and its 960x704
 * depth), each particle pixel covering 2x2 -- the electric balls and blade
 * trails of the arena's Hydra, the Great Clock lizards' attacks... v70: such
 * a surface is the 1x one at twice the size, drawn at full resolution
 * (viewport and scissor scaled); before, it was a separate 480x352 image
 * that nothing read, and every particle drawn into it was lost (owner, v69).
 * RSX_ENG_AA4_SPLIT=1 keeps it separate (the old way). */
static u32 eng_aa_scale(void)
{
    static int split = -1;
    if (split < 0) { const char* e = getenv("RSX_ENG_AA4_SPLIT"); split = e && *e == '1'; }
    const u32 aa = (rsx_dsp_reg(&g.rsx, 0x0208u) >> 12) & 0xFu;   /* SURFACE_FORMAT antialias */
    return !split && (aa == 4u || aa == 5u) ? 2u : 1u;
}

static u32 eng_current_target_set(u32 slots[RSX_BE_MAX_COLOR_TARGETS])
{
    rsx_dsp_surface sf;
    rsx_dsp_get_surface(&g.rsx, &sf);
    { const u32 as = eng_aa_scale(); sf.clip_w *= as; sf.clip_h *= as; }
    /* SET_SURFACE_COLOR_TARGET 2 selects B alone; every other value starts at
     * A, and the MRT selectors add B, C and D on top of it. */
    const u32 sel = (sf.color_target == CELL_GCM_SURFACE_TARGET_1) ? 1u : 0u;
    const rsx_be_format fmt = eng_surface_format(sf.color_format);
    const u32 first = eng_surface_get(sf.color_location[sel], sf.color_offset[sel],
                                      sf.clip_w, sf.clip_h, fmt);
    if (first == ENG_INVALID) return 0;
    g.surfaces[first].pitch = sf.color_pitch[sel];
    g.surfaces[first].rsx_fmt = sf.color_format & 0x1Fu;
    g.surfaces[first].copy_h = 0;                 /* drawn into: every row is the GPU's */
    g.surfaces[first].raster = sf.raster_type;
    slots[0] = first;
    u32 n = 1;

    static const u32 mrt_from[3] = {
        CELL_GCM_SURFACE_TARGET_MRT1, CELL_GCM_SURFACE_TARGET_MRT2,
        CELL_GCM_SURFACE_TARGET_MRT3
    };
    for (u32 i = 0; i < 3; i++) {
        if (sf.color_target < mrt_from[i]) break;
        const u32 slot = eng_surface_get(sf.color_location[i + 1],
                                         sf.color_offset[i + 1],
                                         sf.clip_w, sf.clip_h, fmt);
        if (slot == ENG_INVALID) break;
        /* A set naming one buffer twice would attach the same target twice,
         * which no host API allows. */
        int seen = 0;
        for (u32 k = 0; k < n; k++) if (slots[k] == slot) seen = 1;
        if (seen) break;
        slots[n++] = slot;
    }
    return n;
}

static u32 eng_current_surface(void)
{
    u32 slots[RSX_BE_MAX_COLOR_TARGETS];
    return eng_current_target_set(slots) ? slots[0] : ENG_INVALID;
}

/* ---- per-zeta depth ------------------------------------------------------ */

/* One depth target per guest zeta address. Sharing a single resource across a
 * title's shadow, scene and post passes cross-contaminates later depth tests;
 * upstream's ef3271b records that as "the black player-character mass". */
static u32 eng_zdepth_get_alloc(u32 location, u32 offset, u32 rt_w, u32 rt_h);
/* The depth target for this zeta, remembering the area the pass renders:
 * the allocation is at least the canvas, and a reader samples the rendered
 * area in 0..1 -- A Crack in Time draws its scene at 960x704 into a depth
 * buffer the canvas makes 1280x720, and its 64x64 shadow maps likewise. */
static u32 eng_zdepth_get(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    const u32 slot = eng_zdepth_get_alloc(location, offset, rt_w, rt_h);
    if (slot != ENG_INVALID) g.zdepths[slot].last_use = ++g.surf_serial;
    if (slot != ENG_INVALID && rt_w && rt_h) {
        eng_zdepth* z = &g.zdepths[slot];
        const u32 uw = rt_w < z->w ? rt_w : z->w, uh = rt_h < z->h ? rt_h : z->h;
        if (uw != z->used_w || uh != z->used_h) {   /* copies of the old area are stale */
            z->used_w = uw; z->used_h = uh;
            z->snapshot_valid = 0; z->as_color_valid = 0;
        }
    }
    return slot;
}

/* What the shadow maps drawn at their own scale (rsx_depth_target_scale) may
 * take together, each with its snapshot: a sixteenth of the card's video memory,
 * 192-512 MB (about 450 at 7 GB: Zolar's eight 512x512 maps fit); RACACIT_SHADOW_MB
 * sets it. The small maps (under 512 a side) get half of that, so a crowd of
 * them cannot take the hero's and the sun's room.
 * A fight's shadow pool makes dozens of maps a frame. In the owner's v96 Valkyrie
 * fleet fight its 512x512 ones, 32 MB + 16 MB each at 4x, took the process to 7.9
 * GB of a 7 GB budget; every failed allocation then waited for the GPU, 100+ ms
 * frames. Past the cap, or while the card is short (g_rsx_vram_tight), a new map
 * is made one pixel wider: not a power of two, so the backend draws it at the
 * scene's scale, as before v96, and the engine's own accounting (which goes by
 * size) agrees. 1 if a w x h map fits beside the others (bar `skip`). */
static u64 eng_zdepth_bytes(const eng_zdepth* z);
volatile int g_rsx_vram_tight;
extern unsigned g_rsx_vram_budget_mb;
static int eng_shadow_room(u32 w, u32 h, u32 skip)
{
    if (g_rsx_vram_tight) return 0;
    static u64 cap;
    if (!cap) { const char* e = getenv("RACACIT_SHADOW_MB"); const long v = e ? atol(e) : 0;
                u64 mb = v > 0 ? (u64)v : g_rsx_vram_budget_mb / 16u;
                if (v <= 0) { if (mb < 192u) mb = 192u; if (mb > 512u) mb = 512u; }
                cap = mb << 20; }
    const int small_ = w < 512u && h < 512u;
    const double sc = rsx_depth_target_scale(w, h);
    const u64 nb = (u64)((double)w * h * 12.0 * sc * sc);
    u64 all = nb, sm = small_ ? nb : 0;
    for (u32 i = 0; i < g.n_zdepths && all <= cap; i++) {
        const eng_zdepth* z = &g.zdepths[i];
        if (i == skip || !z->handle || rsx_depth_target_scale(z->w, z->h) == g_rsx_res_scale) continue;
        const u64 b = eng_zdepth_bytes(z);
        all += b;
        if (z->w < 512u && z->h < 512u) sm += b;
    }
    return all <= cap && (!small_ || sm <= cap / 2u);
}

static u32 eng_zdepth_get_alloc(u32 location, u32 offset, u32 rt_w, u32 rt_h)
{
    /* The attachment must cover the whole canvas: an early pass declaring a
     * smaller clip than the live viewport otherwise gets a target the host
     * API rejects when a later, larger pass binds it. */
    /* A small pass (a shadow map) gets its own size: a canvas-sized buffer
     * for each of a pool's dozens of maps was most of a gigabyte. */
    /* So does a shadow map the backend draws at its own scale
     * (rsx_depth_target_scale): A Crack in Time's 512x512 hero and sun maps
     * covered a quarter of the canvas and got canvas-sized (1280x720, not a
     * power of two), which kept them at the scene's scale. A later larger
     * pass at the address grows the buffer as for any other. */
    u32 want_w = rt_w, want_h = rt_h;
    /* Every shadow-map size, also where its scale happens to be the scene's. A 1024x1024
     * map from 2x up took the canvas rule below: 1280x1024 of depth, 163 MB at 4x, for each
     * of the pool's addresses. A broken crate or a crowd repacks the pool: over 7 GB within
     * seconds, on any card that had it, then failed allocations and every texture thrown out. */
    const int shadow_size = rt_w && rt_h && rsx_depth_target_is_map(rt_w, rt_h);
    if (!shadow_size && (!rt_w || !rt_h || (u64)rt_w * rt_h * 4u >= (u64)g.width * g.height)) {
        want_w = rt_w > g.width ? rt_w : g.width;
        want_h = rt_h > g.height ? rt_h : g.height;
    }
    if (want_w > ENG_MAX_SURFACE_DIM || want_h > ENG_MAX_SURFACE_DIM)
        return ENG_INVALID;

    u32 slot = ENG_MAX_ZDEPTHS, shrink = 0;
    { const u32 i = eng_z_first(location, offset);
      if (i != ENG_INVALID) {
          eng_zdepth* z = &g.zdepths[i];
          /* The pool repacks: the address of the hero's 512x512 map is
           * drawn at 1024x512 once a few seconds into Zolar, and grown to
           * canvas size the buffer stayed at the scene's scale for the
           * session. A shadow pass takes it back to its own size once no pass
           * has needed more than the shadow passes there for ~2 s (a pass
           * that needs it every frame keeps it big: no churn). */
          if (shadow_size) {
              if (rt_w > z->sh_w) z->sh_w = rt_w;
              if (rt_h > z->sh_h) z->sh_h = rt_h;
          } else if (want_w > z->sh_w || want_h > z->sh_h) {
              z->big_frame = g.frames + 1u;
          }
          shrink = shadow_size && z->w >= want_w && z->h >= want_h &&
                   !rsx_depth_target_is_map(z->w, z->h) && rsx_depth_target_is_map(z->sh_w, z->sh_h) &&
                   g.frames + 1u - z->big_frame > 120u &&
                   (rsx_depth_target_scale(z->sh_w, z->sh_h) <= g_rsx_res_scale || eng_shadow_room(z->sh_w, z->sh_h, i));
          if (!shrink && z->w >= want_w && z->h >= want_h) return i;
          slot = i;
          if (shrink) { want_w = z->sh_w; want_h = z->sh_h; }
          else {
              if (want_w < z->w) want_w = z->w;
              if (want_h < z->h) want_h = z->h;
          }
      } }
    /* Only where the scene's scale saves anything: at 4x it would cost more. */
    if (rsx_depth_target_scale(want_w, want_h) > g_rsx_res_scale && !eng_shadow_room(want_w, want_h, slot)) {
        static unsigned long long n; ++n;
        if (n <= 8 || (n & (n - 1)) == 0)
            fprintf(stderr, "[rsx engine] shadow maps past their memory cap: depth %u:0x%08X %ux%u made at the scene's scale (%llu so far)%c",
                    location, offset, want_w, want_h, n, 10);
        want_w++;
    }
    if (slot == ENG_MAX_ZDEPTHS) {
        if (g.n_zdepths < ENG_MAX_ZDEPTHS) {
            slot = g.n_zdepths;
        } else {
            slot = eng_zdepth_victim();
            if (slot == ENG_INVALID) {             /* least recently bound */
                slot = 0;
                for (u32 i = 1; i < g.n_zdepths; i++)
                    if (g.zdepths[i].last_use < g.zdepths[slot].last_use) slot = i;
            }
            { static u32 logs = 0; if (logs++ < 4)
                fprintf(stderr, "[rsx engine] depth list full: 0x%08X replaces 0x%08X%c",
                        offset, g.zdepths[slot].offset, 10); }
        }
    }

    const u64 c0_ = eng_hf_tsc();
    const u32 handle = g.be->depth_target_create(g.be->user, want_w, want_h);
    if (!handle)
        return (slot < g.n_zdepths && g.zdepths[slot].handle) ? slot : ENG_INVALID;
    { static u32 logs;   /* which depth buffers a level makes, and at what scale */
      if (logs++ < 64)
          fprintf(stderr, "[rsx engine] depth %u:0x%08X %ux%u for a %ux%u pass, x%.2f%s%c", location, offset,
                  want_w, want_h, rt_w, rt_h, (double)rsx_depth_target_scale(want_w, want_h),
                  shrink ? " (back to its shadow maps' size)" : "", 10); }

    eng_zdepth* z = &g.zdepths[slot];
    /* A buffer remade at its own address after this frame wrote it: what was
     * drawn there is gone before anything later in the frame reads it. */
    if (z->handle && z->location == location && z->offset == offset && z->had_write && z->last_write > s_frame_serial) {
        static unsigned long long n; ++n;
        if (n <= 32 || (n & (n - 1)) == 0)
            fprintf(stderr, "[rsx engine] depth %u:0x%08X %ux%u (x%.2f) drawn this frame, remade %ux%u (x%.2f) for a %ux%u pass%s, frame %u (%llu so far)%c",
                    location, offset, z->w, z->h, (double)rsx_depth_target_scale(z->w, z->h), want_w, want_h,
                    (double)rsx_depth_target_scale(want_w, want_h), rt_w, rt_h, shrink ? " (back to its shadow maps' size)" : "",
                    g.frames, n, 10);
    }
    if (z->handle) g.be->depth_target_release(g.be->user, z->handle);
    s_hf.create_cyc += eng_hf_tsc() - c0_; s_hf.zcreates++;
    if (slot >= g.n_zdepths || z->location != location || z->offset != offset) {
        z->sh_w = shadow_size ? rt_w : 0; z->sh_h = shadow_size ? rt_h : 0;
        z->big_frame = shadow_size ? 0u : g.frames + 1u;
        z->bpp = 0;
    }
    z->location = location; z->offset = offset;
    z->w = want_w; z->h = want_h;
    z->handle = handle;
    z->snapshot = 0;
    z->cleared = 0;
    z->had_write = 0;
    z->last_write = 0;
    z->ww_epoch = 0; z->ww_check_frame = 0; z->ww_stale = 0;   /* a new buffer: no verdict of the old one */
    z->snapshot_valid = 0;
    if (z->as_color) g.be->color_target_release(g.be->user, z->as_color);
    z->as_color = 0;
    z->as_color_valid = 0;
    if (z->as_color_spare) g.be->color_target_release(g.be->user, z->as_color_spare);
    z->as_color_spare = 0;
    z->used_w = z->used_h = 0;
    z->last_use = ++g.surf_serial;
    if (slot == g.n_zdepths) g.n_zdepths++;
    g.addr_dirty = 1;
    g.bind_epoch++;
    return slot;
}

/* A depth target read back as a texture, but only once the pass has executed
 * a depth-WRITING draw: a write-enable bit alone does not prove the pass
 * produced a usable depth map, and a clear-only zeta falls through to guest
 * memory instead (rsx_live_draw.c:6016-6018, 6265-6269). */
/* tex_w x tex_h: the size the reader declares, when it is not the area the
 * pass drew (0 = that area). A swizzled shadow map drawn at 512x256 and read
 * as a 512x512 texture is its top half: stretching the drawn area over the
 * whole texture put every lookup on the wrong row. */
static u64 s_snap_builds, s_ascolor_builds, s_memo_hits;
static u32 eng_zdepth_snapshot_tex(u32 slot, u32 tex_w, u32 tex_h)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (!z->handle || !z->had_write) return 0;
    const u32 uw = z->used_w ? z->used_w : z->w, uh = z->used_h ? z->used_h : z->h;
    if (!g.be->depth_snapshot_sized || (tex_w == uw && tex_h == uh)) tex_w = tex_h = 0;
    if (z->snapshot_valid && z->snapshot && z->snap_w == tex_w && z->snap_h == tex_h) return z->snapshot;
    if (!g.be->depth_snapshot) return 0;
    s_snap_builds++; s_hf.snaps++;
    const u64 c0_ = eng_hf_tsc();
    const u32 tex = (tex_w && tex_h)
        ? g.be->depth_snapshot_sized(g.be->user, z->handle, uw, uh, tex_w, tex_h)
        : g.be->depth_snapshot(g.be->user, z->handle, uw, uh);
    s_hf.snap_cyc += eng_hf_tsc() - c0_;
    if (!tex) return 0;
    z->snapshot = tex;
    z->snapshot_valid = 1;
    z->snap_w = tex_w; z->snap_h = tex_h;
    return tex;
}
static u32 eng_zdepth_snapshot(u32 slot)
{
    return eng_zdepth_snapshot_tex(slot, 0, 0);
}

/* A depth buffer sampled with a COLOUR format reads its raw D24S8 words: in
 * A8R8G8B8, a = depth[23:16], r = depth[15:8], g = depth[7:0], b = stencil.
 * PS3 titles reconstruct depth that way -- A Crack in Time's blob shadows and
 * decals read the scene depth at 0x1380000 as ARGB8 to project onto the
 * ground. Its colour twin there is never written (the pass that fills the
 * depth masks colour off), so reading that gave depth 0 and every decal
 * painted its whole quad: the dark box around the player. Built on the GPU
 * from the depth snapshot by one full-screen draw, at most once per change.
 * The 24-bit value is rounded and capped, not "+ 0.5": in float, 1.0 x
 * 16777215 + 0.5 rounds up to 16777216 = 0x1000000, whose low 24 bits are 0 --
 * the far plane read as the nearest depth. A Crack in Time copies its scene
 * depth this way (pass b87682533c764a97 into 0x01380000) for every particle
 * and effect pass after it; over open sky (depth 1.0) the copy said 0, and
 * all of them failed their depth test there: the Aphelion's blaster bolts
 * were invisible until they flew in front of something. */
static const char* const kEngDepthAsColorPS =
"struct PSInput {\n"
"    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
"    float4 fog : FOG;\n"
"    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
"    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
"};\n"
"Texture2D    rsx_tex[16] : register(t0);\n"
"SamplerState rsx_samp[16] : register(s0);\n"
"cbuffer PSConstants : register(b1) {\n"
"    float4 fp_constants[1];\n"
"    float4 fp_alpha;\n"
"};\n"
"float4 main(PSInput input) : SV_TARGET {\n"
"    float2 sz; rsx_tex[0].GetDimensions(sz.x, sz.y);\n"
"    float d = rsx_tex[0].Load(int3(int2(input.position.xy * fp_constants[0].xy * sz), 0)).r;\n"
"    uint z = min((uint)round(saturate(d) * 16777215.0), 16777215u);\n"
"    return float4((float)((z >> 8) & 255u), (float)(z & 255u), 0.0,\n"
"                  (float)((z >> 16) & 255u)) / 255.0;\n"
"}\n";

/* The fragment constant block last handed to the backend (rsx_draw_engine
 * draw path): an identical one is not sent again. Anything else that binds
 * fragment constants resets s_ps_last_n. */
static u8* s_ps_last; static u32 s_ps_last_n, s_ps_last_cap;
static u32 s_d2c_pipe;        /* 0 = not built yet, ENG_INVALID = failed */
static const char* const kEngFixedVS;

static u32 eng_zdepth_as_color(u32 slot)
{
    eng_zdepth* z = &g.zdepths[slot];
    if (z->as_color_valid && z->as_color) return z->as_color;
    s_ascolor_builds++; s_hf.snaps++;
    const u32 snap = eng_zdepth_snapshot(slot);
    if (!snap) return 0;
    z = &g.zdepths[slot];
    const u32 cw = z->used_w ? z->used_w : z->w, ch = z->used_h ? z->used_h : z->h;
    if (z->as_color && (z->as_color_w != cw || z->as_color_h != ch)) {
        /* A depth buffer used at two sizes (a full and a half-size pass over
         * the same memory) alternates: keep the other size's target as a
         * spare instead of a release and a create each time. */
        const u32 sp = z->as_color_spare, sw = z->spare_w, sh = z->spare_h;
        z->as_color_spare = z->as_color; z->spare_w = z->as_color_w; z->spare_h = z->as_color_h;
        z->as_color = 0;
        if (sp && sw == cw && sh == ch) { z->as_color = sp; z->as_color_w = cw; z->as_color_h = ch; }
        else if (sp) g.be->color_target_release(g.be->user, sp);
    }
    if (!z->as_color) {
        z->as_color = g.be->color_target_create(g.be->user, RSX_BE_FMT_R8G8B8A8, cw, ch, NULL, 0);
        if (!z->as_color) return 0;
        z->as_color_w = cw; z->as_color_h = ch;
    }
    rsx_vertex_layout_plan lp;
    rsx_vertex_layout_plan_init(&lp, 1u);
    rsx_be_render_state rs;
    memset(&rs, 0, sizeof rs);
    rs.color_mask = 0x01010101u;
    if (!s_d2c_pipe) {
        s_d2c_pipe = g.be->pipeline_create(g.be->user, kEngFixedVS, kEngDepthAsColorPS, &rs, &lp,
                                           lp.stride, RSX_BE_FMT_R8G8B8A8, 1u);
        if (!s_d2c_pipe) {
            s_d2c_pipe = ENG_INVALID;
            fprintf(stderr, "[rsx engine] depth-as-colour pipeline FAILED; such reads stay guest memory%c", 10);
        }
    }
    if (s_d2c_pipe == ENG_INVALID) return 0;

    static u8 vcb[ENG_VP_CB_BYTES];
    { const float one[4] = { 1, 1, 1, 0 };
      memcpy(vcb + RSX_DSP_NUM_CONSTANTS * 16u, one, sizeof one); }
    /* One over the target's size in the backend's pixels: the shader reads
     * the snapshot texel under each pixel at whatever scale the snapshot is
     * (a shadow map's is not the scene's: rsx_depth_target_scale). */
    const float rs_ = g_rsx_res_scale;
    const float tw_ = rs_ == 1.0f ? (float)cw : (float)(u32)((float)cw * rs_ + 0.5f);
    const float th_ = rs_ == 1.0f ? (float)ch : (float)(u32)((float)ch * rs_ + 0.5f);
    const float pcb[8] = { 1.0f / tw_, 1.0f / th_, 0, 0, 0, 0, 0, 0 };
    u32 tex[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc smp[RSX_BE_MAX_TEXTURES];
    u32 vtex[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsmp[RSX_BE_MAX_VERTEX_TEXTURES];
    memset(tex, 0, sizeof tex); memset(smp, 0, sizeof smp);
    memset(vtex, 0, sizeof vtex); memset(vsmp, 0, sizeof vsmp);
    tex[0] = snap;
    static const float quad[6][4] = {
        { -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { 1, 1, 0, 1 },
        { -1, -1, 0, 1 }, { 1, 1, 0, 1 }, { -1, 1, 0, 1 } };
    g.be->bind_targets(g.be->user, &z->as_color, 1, 0);
    g.be->bind_pipeline(g.be->user, s_d2c_pipe);
    g.be->bind_vs_constants(g.be->user, vcb, ENG_VP_CB_BYTES);
    s_vp_cb_bound = vcb;
    g.be->bind_ps_constants(g.be->user, pcb, sizeof pcb);
    s_ps_last_n = 0;
    g.be->bind_textures(g.be->user, tex, smp, 1u);
    g.be->bind_vertex_textures(g.be->user, vtex, vsmp, 0u);
    g.be->set_viewport(g.be->user, 0.0f, 0.0f, (float)cw, (float)ch);
    g.be->set_scissor(g.be->user, 0, 0, cw, ch);
    g.be->set_stencil_ref(g.be->user, 0);
    g.be->draw(g.be->user, RSX_TOPOLOGY_TRIANGLES, quad, 6, 16, NULL, 0);
    z->as_color_valid = 1;
    /* RSX_ENG_D2C_CHECK=<n>: the first n of these passes read back and
     * compared with the snapshot they were made from (both at the guest's
     * size): how many pixels hold their depth's 24 bits. */
    { static long chk = -1;
      if (chk < 0) { const char* e = getenv("RSX_ENG_D2C_CHECK"); chk = e ? atol(e) : 0; }
      if (chk > 0 && g.be->readback) {
          chk--;
          u8* c = (u8*)malloc((size_t)cw * ch * 4u); float* d = (float*)malloc((size_t)cw * ch * 4u);
          if (c && d && !g.be->readback(g.be->user, z->as_color, 0, 0, cw, ch, c, cw * 4u) &&
              !g.be->readback(g.be->user, snap, 0, 0, cw, ch, d, cw * 4u)) {
              u64 ok = 0, n = (u64)cw * ch;
              for (u64 i = 0; i < n; i++) {
                  const double v = (d[i] < 0 ? 0 : d[i] > 1 ? 1 : d[i]) * 16777215.0 + 0.5;
                  const u32 want = v >= 16777215.0 ? 16777215u : (u32)v;
                  const u32 got = ((u32)c[i * 4 + 3] << 16) | ((u32)c[i * 4 + 0] << 8) | c[i * 4 + 1];
                  if ((got > want ? got - want : want - got) <= 1u) ok++;
              }
              fprintf(stderr, "[rsx engine] depth-as-colour check 0x%08X %ux%u (depth x%.2f): %llu of %llu pixels hold their depth%c",
                      z->offset, cw, ch, (double)rsx_depth_target_scale(z->w, z->h),
                      (unsigned long long)ok, (unsigned long long)n, 10);
          }
          free(c); free(d);
      } }
    return z->as_color;
}

/* ---- textures ------------------------------------------------------------ */

static rsx_be_format eng_texfmt(rsx_texfmt f)
{
    switch (f) {
    case RSX_TEXFMT_R8:              return RSX_BE_FMT_R8;
    case RSX_TEXFMT_R8G8:            return RSX_BE_FMT_R8G8;
    case RSX_TEXFMT_R8G8B8A8:        return RSX_BE_FMT_R8G8B8A8;
    case RSX_TEXFMT_BC1:             return RSX_BE_FMT_BC1;
    case RSX_TEXFMT_BC2:             return RSX_BE_FMT_BC2;
    case RSX_TEXFMT_BC3:             return RSX_BE_FMT_BC3;
    case RSX_TEXFMT_R16:             return RSX_BE_FMT_R16;
    case RSX_TEXFMT_R16G16:          return RSX_BE_FMT_R16G16;
    case RSX_TEXFMT_R16G16F:         return RSX_BE_FMT_R16G16F;
    case RSX_TEXFMT_R16G16B16A16F:   return RSX_BE_FMT_R16G16B16A16F;
    case RSX_TEXFMT_R32F:            return RSX_BE_FMT_R32F;
    case RSX_TEXFMT_R32G32B32A32F:   return RSX_BE_FMT_R32G32B32A32F;
    default:                         return RSX_BE_FMT_R8;
    }
}

/* How many guest bytes a texture occupies: the whole mip chain, times six for
 * a cube map. That is what the content hash has to cover, or a title
 * animating one face or one lower level keeps its stale upload. */
static u32 eng_texture_span(u32 fmt, u32 w, u32 h, u32 levels, u32 pitch, int cube)
{
    if (cube) return rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) * 6u;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    return lv[n - 1].offset + lv[n - 1].tl.face_bytes;
}

/* One hash per cached texture per presented frame. Word-at-a-time FNV over
 * the whole span is deliberately cheap: this is a mutation detector, not a
 * content id (rsx_live_draw.c:2002-2026). */
/* RSX_ENG_STATS: what the texture cache costs (hash vs upload). */
static u64 s_tex_slot_cyc, s_tex_ww_skips, s_geom_ww_skips;
static u64 s_tex_hash_cyc, s_tex_hash_bytes, s_tex_hashes, s_tex_up_cyc, s_tex_ups, s_tex_lookups, s_tex_probe;
static u64 eng_texture_content_hash(u32 location, u32 offset, u32 span,
                                    int* readable)
{
    const u8* src = span ? eng_guest_ptr(NULL, location, offset, span) : NULL;
    if (!src) { *readable = 0; return 0; }
    const u64 t0 = eng_rdtsc();
    const u64 hash = span >= 256 ? eng_hash_long(src, span, 0x746578ull) : eng_hash_bytes(src, span, 0x746578ull);
    s_tex_hash_cyc += eng_rdtsc() - t0;
    s_tex_hash_bytes += span; s_tex_hashes++;
    *readable = 1;
    return hash;
}

static int eng_staging_reserve(u32 bytes)
{
    if (g.tex_staging_cap >= bytes) return 1;
    u8* n = (u8*)realloc(g.tex_staging, bytes);
    if (!n) return 0;
    g.tex_staging = n;
    g.tex_staging_cap = bytes;
    return 1;
}

/* Decode a guest texture out of guest memory and hand every face and level to
 * the backend. Returns the backend handle, or 0. */
/* Texture (re)uploads from guest memory; a movie re-uploads its planes every
 * frame, so this over the stats window is the movie's real frame rate. */
volatile unsigned long g_eng_tex_uploads, g_eng_tex_upload_bytes;
/* Texture quality (RACACIT_TEX_QUALITY: high = as the game ships, medium,
 * low; read at start): how many of a texture's top mip levels to leave out.
 * Only textures that carry a mip chain -- scenery, characters, effects, the
 * ones the game itself shows smaller with distance -- and only while the
 * level kept is at least 128 (medium) or 64 (low) texels on its short side.
 * The HUD, fonts and menus have no mip chain and stay sharp; the art is the
 * game's own, read from its smaller levels, as the PS3 shows it from further
 * away. Medium is a quarter of the texture memory and upload work, low a
 * sixteenth. */
static u32 eng_tex_skip_levels(u32 w, u32 h, u32 nlv)
{
    static int q = -1;
    if (q < 0) {
        const char* e = getenv("RACACIT_TEX_QUALITY");
        q = e && (!strcmp(e, "low") || !strcmp(e, "0")) ? 2 : e && (!strcmp(e, "medium") || !strcmp(e, "1")) ? 1 : 0;
        if (q) fprintf(stderr, "[rsx engine] texture quality %s: mip-mapped textures from level %d%c", q == 2 ? "low" : "medium", q, 10);
    }
    if (!q || nlv < 2) return 0;
    const u32 floor_ = q == 2 ? 64u : 128u;
    u32 k = 0;
    while (k < (u32)q && k + 1u < nlv && (w >> (k + 1u)) >= floor_ && (h >> (k + 1u)) >= floor_) k++;
    return k;
}
static u32 eng_texture_upload(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                              u32 levels, u32 pitch, int cube, u32 remap, u32 into)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, pitch, lv);
    if (!nlv || !lv[0].tl.face_bytes) return 0;
    const u32 faces = cube ? 6u : 1u;
    const u32 face_stride = cube
        ? rsx_texture_cube_face_stride(fmt, w, h, levels, pitch) : 0u;
    const u32 span = eng_texture_span(fmt, w, h, levels, pitch, cube);
    const u8* src = eng_guest_ptr(NULL, location, offset, span);
    if (!src) return 0;
    g_eng_tex_uploads++; g_eng_tex_upload_bytes += span;
    /* ENG_TEXDBG_OFF=<hex>: the first 64 bytes of every upload from that offset. */
    { static long s_off = -2; static int n = 0;
      if (s_off == -2) { const char* e = getenv("ENG_TEXDBG_OFF"); s_off = e ? (long)strtoul(e, 0, 16) : -1; }
      if (s_off >= 0 && offset == (u32)s_off && n < 24) { n++;
          fprintf(stderr, "[eng-tex] upload loc %u off 0x%08X fmt 0x%02X %ux%u:", location, offset, fmt, w, h);
          for (u32 i = 0; i < 64 && i < span; i++) fprintf(stderr, "%s%02X", (i & 3) ? "" : " ", src[i]);
          fprintf(stderr, "\n"); } }
    { static int dbg = -1; if (dbg < 0) dbg = getenv("ENG_TEXDBG") ? 1 : 0;
      static int n = 0;
      if (dbg && n < 40 && w >= 640) { n++;
          u32 p = pitch ? pitch : w;
          fprintf(stderr, "[eng-tex] loc %u off 0x%08X fmt 0x%02X %ux%u pitch %u span %u nlv %u rows:",
                  location, offset, fmt, w, h, pitch, span, nlv);
          for (u32 y = 0; y < h; y += h / 8 ? h / 8 : 1) {
              u32 sum = 0; for (u32 x = 0; x < w && x < p; x++) sum += src[(size_t)y * p + x];
              fprintf(stderr, " %u:%u", y, sum / (w < p ? w : p));
          }
          fprintf(stderr, " | tl rows %u row_bytes %u face_bytes %u\n", lv[0].tl.rows, lv[0].tl.dst_row_bytes, lv[0].tl.face_bytes);
      } }

    /* `into`: the guest rewrote a cached texture -- the same image takes the
     * new bytes. A fresh image and allocation per refresh was ~8 kernel
     * round trips a frame in Zolar Forest (its animated textures). */
    const u32 skip = eng_tex_skip_levels(w, h, nlv);   /* texture quality */
    const u32 handle = into ? into
                            : g.be->texture_create(g.be->user, eng_texfmt(lv[0].tl.fmt),
                                                   lv[skip].w, lv[skip].h, nlv - skip, faces, remap, fmt);
    if (!handle) return 0;

    for (u32 f = 0; f < faces; f++) {
        const u8* face = src + (size_t)f * face_stride;
        for (u32 m = skip; m < nlv; m++) {
            const rsx_tex_layout* tl = &lv[m].tl;
            if (!eng_staging_reserve(tl->dst_row_bytes * tl->rows)) {
                if (!into) g.be->texture_release(g.be->user, handle);
                return 0;
            }
            rsx_texture_decode(g.tex_staging, tl->dst_row_bytes,
                               face + lv[m].offset, lv[m].w, lv[m].h, tl,
                               rsx_texture_argb_is_rgba());
            g.be->texture_upload(g.be->user, handle, f, m - skip, lv[m].w, lv[m].h,
                                 g.tex_staging, tl->dst_row_bytes, tl->rows);
        }
    }
    return handle;
}

/* The cache slot for a texture unit's bytes, keyed on where they are and what
 * the registers say they are. An already-cached entry is re-hashed at most
 * once per presented frame and re-decoded when the guest changed it, which is
 * what makes an animated UI or a video texture update at all. A miss with the
 * cache full evicts the least recently used entry rather than returning
 * nothing: returning white "made the recovered orphanage render as flat
 * green/black geometry" (rsx_live_draw.c:2326-2334). */
static u64 s_tex_evicts;
static u32 eng_tex_index_hash(const u32 id[8])
{
    return (u32)eng_hash_bytes(id, 32, 0x7469ull) & (ENG_TEX_INDEX - 1u);
}
static void eng_tex_index_insert(u32 index)
{
    for (u32 hi = eng_tex_index_hash((const u32*)&g.textures[index]);; hi = (hi + 1u) & (ENG_TEX_INDEX - 1u))
        if (!g.tex_index[hi]) { g.tex_index[hi] = (u16)(index + 1u); return; }
}
/* Evicted textures' identities (a direct-mapped table of their full hashes),
 * so one decoded again after its eviction is counted in the [rsx mem] line:
 * whether the cache's caps cost re-decodes, or only drop the dead. */
static u32 s_tex_gone[8192];
static unsigned long long s_tex_remade;
static u32 eng_tex_id_hash(const u32 id[8])
{
    const u32 h = (u32)eng_hash_bytes(id, 32, 0x676F6E65ull);
    return h ? h : 1u;
}
static void eng_tex_gone(const eng_texture* e)
{
    const u32 h = eng_tex_id_hash((const u32*)e);
    s_tex_gone[h & 8191u] = h;
}
static void eng_tex_index_rebuild(void)
{
    memset(g.tex_index, 0, sizeof g.tex_index);
    for (u32 i = 0; i < g.n_textures; i++) eng_tex_index_insert(i);
}

/* RSX_ENG_VERIFY=<bits>: check the caches' shortcuts against ground truth
 * and log every disagreement. 1 textures the write watch called unchanged,
 * 2 cached geometry the write watch called unchanged, 4 vertex blocks (each
 * reuse against a fresh conversion), 8 texture units a program samples that
 * end up with nothing bound. A mismatch is also repaired (converted again). */
static int eng_verify(int bit)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("RSX_ENG_VERIFY"); v = e ? (int)strtol(e, 0, 0) : 0; }
    return (v & bit) != 0;
}
static int eng_verify_say(const char* what)   /* rate limit: first 40 of each kind, then every 500th */
{
    static unsigned long long n[4];
    const int k = what[0] == 't' ? 0 : what[0] == 'g' ? 1 : what[0] == 'v' ? 2 : 3;
    const unsigned long long c = ++n[k];
    return c <= 40 || c % 500 == 0;
}

static u32 eng_texture_slot_core(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                                 u32 levels, u32 pitch, int cube, u32 remap)
{
    if (!w || !h || w > ENG_MAX_TEX_DIM || h > ENG_MAX_TEX_DIM) return 0;

    s_tex_lookups++;
    /* Identity is the first eight fields of eng_texture, in order. A linear
     * walk of the table here was ~280 compares per lookup, six lookups a draw:
     * most of the texture stage. */
    const u32 id[8] = { location, offset, fmt, w, h, pitch, remap, (u32)(cube != 0) };
    for (u32 hi = eng_tex_index_hash(id);; hi = (hi + 1u) & (ENG_TEX_INDEX - 1u)) {
        const u32 slot = g.tex_index[hi];
        if (!slot) break;
        eng_texture* e = &g.textures[slot - 1u];
        s_tex_probe++;
        if (memcmp(e, id, sizeof id) != 0)
            continue;
        /* No image behind it (its create failed -- out of video memory, the
         * backend's object table full; the backend thread can only say so
         * later): drop it and make it again below. Kept, every draw with it
         * sampled the magenta "nothing bound" texture until the entry was
         * evicted -- for a texture in view all the time, until a restart. */
        if (e->handle && g.be->texture_failed && g.be->texture_failed(g.be->user, e->handle)) {
            g.be->texture_release(g.be->user, e->handle);
            e->handle = 0; e->content_hash = 0;
            if (e->fails < 6u) e->fails++;
            g.bind_epoch++;
            static u32 logs;
            if (logs < 32) { logs++;
                fprintf(stderr, "[rsx engine] texture %u:0x%08X %ux%u fmt 0x%02X has no image (out of video memory?): made again%c",
                        location, offset, w, h, fmt, 10); }
        }
        /* A create that failed is tried again: after 30 frames, twice as long
         * after each further failure (one the backend can never make -- a
         * format it lacks -- settles at every half a minute), at most 4 a
         * frame in all (a failed allocation waits for the GPU). It used to
         * stay failed, the unit unbound, for the entry's life. */
        if (!e->handle) {
            static u32 s_retry_frame, s_retries;
            if (s_retry_frame != g.frames) { s_retry_frame = g.frames; s_retries = 0; }
            if (s_retries < 4u && g.frames - e->last_hash_frame >= (30u << e->fails)) {
                s_retries++;
                e->last_hash_frame = g.frames;
                const u32 span = eng_texture_span(fmt, w, h, levels, pitch, cube);
                int readable = 0;
                const u32 ww_now = rsx_ww_on() ? rsx_ww_epoch() : 0u;
                const u64 hash = span ? eng_texture_content_hash(location, offset, span, &readable) : 0;
                const u32 fresh = span ? eng_texture_upload(location, offset, fmt, w, h, levels, pitch, cube, remap, 0) : 0;
                if (!fresh && e->fails < 6u) e->fails++;
                if (fresh) {
                    e->handle = fresh; e->levels = levels; e->span = span; e->content_hash = hash;
                    e->ww_epoch = readable ? ww_now : 0u;
                    g.bind_epoch++;
                }
            }
        }
        if (e->handle && e->last_hash_frame != g.frames) {
            /* The span is the entry's own unless the mip count changed: the
             * whole mip-chain layout per lookup was ~8% of the RSX thread. */
            const u32 span = (e->levels == levels && e->span)
                ? e->span : eng_texture_span(fmt, w, h, levels, pitch, cube);
            if (!span) return 0;
            /* Nothing in its pages written since it was last checked: the
             * same bytes, no hash (rsx_writewatch.h). */
            if (e->ww_epoch && e->levels == levels) {
                const u8* src = eng_guest_ptr(NULL, location, offset, span);
                if (src && !rsx_ww_written_since(src, span, e->ww_epoch)) {
                    int stale = 0;
                    /* Spot check (default on): about one write-watch skip in
                     * 512, at most 32 a frame, is hashed anyway -- a change
                     * the watch missed shows up as [verify] and is re-decoded
                     * here instead of staying wrong for the rest of the
                     * session. RSX_ENG_VERIFY=1 checks every skip. */
                    static u32 s_spot, s_spot_frame, s_spot_n;
                    if (s_spot_frame != g.frames) { s_spot_frame = g.frames; s_spot_n = 0; }
                    const int spot = ((++s_spot & 511u) == 0) && s_spot_n < 32u;
                    if (spot) s_spot_n++;
                    if (eng_verify(1) || spot) {
                        int rd = 0;
                        const u64 h2 = eng_texture_content_hash(location, offset, span, &rd);
                        if (rd && h2 != e->content_hash) {
                            stale = 1;
                            if (eng_verify_say("tex"))
                                fprintf(stderr, "[verify] texture %u:0x%08X %ux%u fmt 0x%02X span %u changed with no write seen (frame %u, epoch %u)%c",
                                        location, offset, w, h, fmt, span, g.frames, e->ww_epoch, 10);
                        }
                    }
                    if (!stale) {
                        e->last_hash_frame = g.frames;
                        s_tex_ww_skips++;
                        e->last_use_serial = ++g.texture_use_serial; e->last_use_frame = g.frames;
                        return e->handle;
                    }
                }
            }
            int readable = 0;
            const u32 ww_now = rsx_ww_on() ? rsx_ww_epoch() : 0u;
            const u64 hash = eng_texture_content_hash(location, offset, span,
                                                      &readable);
            e->last_hash_frame = g.frames;
            if (readable) e->ww_epoch = ww_now;
            if (readable && hash != e->content_hash) {
                const u64 t0 = eng_hf_tsc();
                const u32 into = (e->levels == levels) ? e->handle : 0u;
                const u32 fresh = eng_texture_upload(location, offset, fmt, w, h,
                                                     levels, pitch, cube, remap, into);
                { const u64 dc_ = eng_hf_tsc() - t0; s_tex_up_cyc += dc_; s_hf.tex_cyc += dc_; } s_tex_ups++; s_hf.tex_ups++;
                if (fresh) {
                    if (fresh != e->handle) g.be->texture_release(g.be->user, e->handle);
                    if (fresh != e->handle) g.bind_epoch++;
                    e->handle = fresh;
                    e->levels = levels;
                    e->span = span;
                    e->content_hash = hash;
                }
            }
        }
        e->last_use_serial = ++g.texture_use_serial; e->last_use_frame = g.frames;
        return e->handle;
    }

    const u32 span = eng_texture_span(fmt, w, h, levels, pitch, cube);
    if (!span) return 0;
    u32 index;
    u32 evicted = 0;
    /* RSX_ENG_TEX_CAP=<n>: a smaller texture cache (testing the eviction path). */
    static u32 s_cap = 0;
    if (!s_cap) { const char* c_ = getenv("RSX_ENG_TEX_CAP"); s_cap = c_ ? (u32)atoi(c_) : 0;
                  if (!s_cap || s_cap > ENG_MAX_TEXTURES) s_cap = ENG_MAX_TEXTURES; }
    if (g.n_textures < s_cap) {
        index = g.n_textures++;
    } else {
        /* The least recently used one that no draw of this frame has bound:
         * one this frame used may sit in a draw still being put together (a
         * unit bound before the one being resolved now). */
        index = ENG_INVALID;
        for (u32 i = 0; i < g.n_textures; i++)
            if (g.textures[i].last_use_frame != g.frames &&
                (index == ENG_INVALID || g.textures[i].last_use_serial < g.textures[index].last_use_serial))
                index = i;
        if (index == ENG_INVALID) return 0;         /* everything is in use: leave the unit unbound */
        evicted = g.textures[index].handle;
        eng_tex_gone(&g.textures[index]);
        s_tex_evicts++;
    }
    { const u32 h = eng_tex_id_hash(id);
      if (s_tex_gone[h & 8191u] == h) { s_tex_gone[h & 8191u] = 0; s_tex_remade++; } }

    { const char* rw = eng_retired_hit(location, offset, span);
      static u32 logs;
      if (rw && logs < 48) { logs++;
          fprintf(stderr, "[rsx engine] texture %u:0x%08X %ux%u fmt 0x%02X decoded from a retired target's memory (%s): noise%c",
                  location, offset, w, h, fmt, rw, 10); } }
    eng_texture e;
    memset(&e, 0, sizeof(e));
    e.location = location; e.offset = offset; e.format = fmt;
    e.width = w; e.height = h; e.pitch = pitch;
    e.remap = remap; e.cubemap = (u32)(cube != 0);
    e.last_hash_frame = g.frames;
    e.last_use_serial = ++g.texture_use_serial; e.last_use_frame = g.frames;
    { int readable = 0;
      const u32 ww_now = rsx_ww_on() ? rsx_ww_epoch() : 0u;
      e.content_hash = eng_texture_content_hash(location, offset, span, &readable);
      if (readable) e.ww_epoch = ww_now; }
    { const u64 t0 = eng_hf_tsc();
      e.handle = eng_texture_upload(location, offset, fmt, w, h, levels, pitch,
                                    cube, remap, 0);
      e.levels = levels;
      e.span = span;
      { const u64 dc_ = eng_hf_tsc() - t0; s_tex_up_cyc += dc_; s_hf.tex_cyc += dc_; } s_tex_ups++; s_hf.tex_ups++; }
    if (e.handle && evicted) g.be->texture_release(g.be->user, evicted);
    if (e.handle || !evicted) {
        g.textures[index] = e;
        if (evicted) { eng_tex_index_rebuild(); g.bind_epoch++; }   /* an identity left: rare */
        else eng_tex_index_insert(index);
    }
    return e.handle;
}

/* ---- parked-VRAM watch ---------------------------------------------------
 * Before a vendor's preview movie, the pause menu and the like, A Crack in
 * Time copies 7.25 MB of RSX local memory (0x0E98FF80..) to main memory with
 * NV0039, and nothing copies it back through NV0039; afterwards level
 * textures can turn into striped noise and stay that way (HANDOFF, open
 * blocker). From every NV0039 copy of >= 1 MB, for RSX_PARK_FLIPS flips
 * (default 1800) this logs "[park]" lines:
 *  - at the copy: each colour target, depth buffer and cached texture
 *    overlapping the source, and whether the copy got its pixels (the copy
 *    reads back only drawn RGBA8 colour; anything else only the GPU has is
 *    copied as whatever guest memory held);
 *  - every 4th flip, when it changed: how many 64 KB chunks of the source
 *    equal the copy -- the source being overwritten, and being restored;
 *  - every texture bound from either range: new, re-decoded or cached; a
 *    cached one is re-hashed every 32 flips and logged STALE when guest
 *    memory no longer matches what was decoded (a write the watch missed);
 *  - other NV0039 copies touching either range;
 *  - (cellGcmSys) methods on subchannels 1-7 and SPU PUTs into the ranges.
 * Only runs inside a window, so it is on by default: the owner's own log
 * then carries the evidence. RSX_PARK_WATCH=0 = off. */
#define ENG_PARK_HITS 128
static struct {
    u32 flips_left;                 /* 0 = not watching */
    u32 src_loc, src_off, dst_loc, dst_off, bytes;
    u32 flip;                       /* flips since the copy */
    u32 eq_chunks;
    int compared;
    u32 n_hits;
    u32 hit_loc[ENG_PARK_HITS], hit_off[ENG_PARK_HITS], hit_check[ENG_PARK_HITS];
    u8  hit_redecodes[ENG_PARK_HITS], hit_stale[ENG_PARK_HITS];
    u32 tex_lines, cmp_lines, copy_lines, stale;
    u32 parks;
} s_park;

static int eng_park_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_PARK_WATCH"); on = !(e && *e == '0'); }
    return on;
}

/* 1 when [off, off + n) at `loc` overlaps the parked source, 2 the copy, 0 neither. */
static int eng_park_range(u32 loc, u32 off, u32 n)
{
    if (!s_park.flips_left) return 0;
    const u64 lo = off, hi = (u64)off + n;
    if (loc == s_park.src_loc && lo < (u64)s_park.src_off + s_park.bytes && hi > s_park.src_off) return 1;
    if (loc == s_park.dst_loc && lo < (u64)s_park.dst_off + s_park.bytes && hi > s_park.dst_off) return 2;
    return 0;
}

/* What a copy out of [off, off + bytes) at `loc` gets from what overlaps it. */
static void eng_park_audit(u32 loc, u32 off, u32 bytes)
{
    const u64 lo = off, hi = (u64)off + bytes;
    u32 lines = 0, nsurf = 0, nstale = 0, nz = 0, nzw = 0, ntex = 0, ntex_recent = 0;
    u64 texb = 0;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->location != loc) continue;
        const int swz = s->raster == 2;
        const u32 pitch = swz ? s->w * 4u : (s->pitch ? s->pitch : s->w * 4u);
        const u64 so = s->offset, se = so + (u64)pitch * s->h;
        if (so >= hi || se <= lo) continue;
        nsurf++;
        const char* st =
            !s->color_written ? "never drawn" :
            s->synced_serial == s->last_write ? "guest memory already has its pixels" :
            !g.be->readback ? "NOT read back (no readback): the copy gets stale bytes" :
            s->fmt != RSX_BE_FMT_R8G8B8A8 ? "NOT read back (not RGBA8): the copy gets stale bytes" :
            !s->rsx_fmt ? "NOT read back (surface format unknown): the copy gets stale bytes" :
            (swz && ((s->w & (s->w - 1u)) || (s->h & (s->h - 1u)))) ? "NOT read back (swizzled, not a power of two): the copy gets stale bytes" :
            pitch < s->w * 4u ? "NOT read back (pitch below width): the copy gets stale bytes" :
            "read back for the copy";
        if (st[0] == 'N') nstale++;
        if (lines++ < 40)
            fprintf(stderr, "[park]   target %u:0x%08X %ux%u fmt %u rsx_fmt 0x%X %s pitch %u (+0x%llX): %s%c",
                    s->location, s->offset, s->w, s->h, (unsigned)s->fmt, s->rsx_fmt, swz ? "swizzled" : "linear",
                    pitch, (unsigned long long)(so > lo ? so - lo : 0), st, 10);
    }
    for (u32 i = 0; i < g.n_zdepths; i++) {
        const eng_zdepth* z = &g.zdepths[i];
        if (!z->handle || z->location != loc) continue;
        const u64 zo = z->offset, ze = zo + (u64)z->w * z->h * 4u;
        if (zo >= hi || ze <= lo) continue;
        nz++; if (z->had_write) nzw++;
        if (lines++ < 40)
            fprintf(stderr, "[park]   depth %u:0x%08X %ux%u (+0x%llX)%s%s: %s%c", z->location, z->offset, z->w, z->h,
                    (unsigned long long)(zo > lo ? zo - lo : 0), z->cleared ? " cleared" : "", z->snapshot ? " snapshot" : "",
                    z->had_write ? "written on the GPU only, NOT read back: the copy gets stale bytes" : "never written", 10);
    }
    for (u32 i = 0; i < g.n_textures; i++) {
        const eng_texture* t = &g.textures[i];
        if (!t->handle || t->location != loc) continue;
        const u64 to = t->offset, te = to + (t->span ? t->span : 1u);
        if (to >= hi || te <= lo) continue;
        ntex++; texb += t->span;
        if (g.frames - t->last_use_frame < 60u) ntex_recent++;
    }
    fprintf(stderr, "[park] source %u:0x%08X+0x%X: %u colour targets (%u NOT read back), %u depth buffers (%u written on the GPU), %u cached textures (%.1f MB, %u used in the last 60 frames)%s%c",
            loc, off, bytes, nsurf, nstale, nz, nzw, ntex, texb / 1048576.0, ntex_recent,
            lines > 40 ? "; target/depth list cut at 40" : "", 10);
}

/* NV0039 is about to copy `bytes` from (src_loc, src_off) to (dst_loc,
 * dst_off); called before the read-back of the source. */
void rsx_draw_engine_park_note(u32 src_loc, u32 src_off, u32 dst_loc, u32 dst_off, u32 bytes)
{
    if (!g.ready || !eng_park_on() || !bytes) return;
    /* libgcm splits one transfer into lines of at most 0x3FFFFF bytes: the
     * next piece continues the park. */
    if (s_park.flips_left && s_park.flip <= 2u && src_loc == s_park.src_loc && dst_loc == s_park.dst_loc &&
        src_off == s_park.src_off + s_park.bytes && dst_off == s_park.dst_off + s_park.bytes) {
        eng_park_audit(src_loc, src_off, bytes);
        s_park.bytes += bytes;
        return;
    }
    if (bytes < (1u << 20)) {
        const int a = eng_park_range(src_loc, src_off, bytes), b = eng_park_range(dst_loc, dst_off, bytes);
        if ((a || b) && s_park.copy_lines < 64) { s_park.copy_lines++;
            fprintf(stderr, "[park] flip %u: NV0039 %u:0x%08X -> %u:0x%08X, %u bytes: %s%s%s%c", s_park.flip,
                    src_loc, src_off, dst_loc, dst_off, bytes,
                    a ? (a == 1 ? "reads the parked source" : "reads the copy") : "", a && b ? ", " : "",
                    b ? (b == 1 ? "writes into the parked source" : "writes into the copy") : "", 10); }
        return;
    }
    static u32 s_flips = 0;
    if (!s_flips) { const char* e = getenv("RSX_PARK_FLIPS"); s_flips = e ? (u32)atoi(e) : 0; if (!s_flips) s_flips = 1800u; }
    const u32 parks = s_park.parks + 1u;
    if (s_park.flips_left)
        fprintf(stderr, "[park] new park replaces the watch of park %u at flip %u%c", s_park.parks, s_park.flip, 10);
    memset(&s_park, 0, sizeof s_park);
    s_park.parks = parks;
    if (parks > 32u) return;                  /* enough evidence for one log */
    fprintf(stderr, "[park] park %u (frame %u): NV0039 %u:0x%08X -> %u:0x%08X, %u bytes; watching %u flips%c",
            parks, g.frames, src_loc, src_off, dst_loc, dst_off, bytes, s_flips, 10);
    eng_park_audit(src_loc, src_off, bytes);
    s_park.flips_left = s_flips;
    s_park.src_loc = src_loc; s_park.src_off = src_off;
    s_park.dst_loc = dst_loc; s_park.dst_off = dst_off;
    s_park.bytes = bytes;
}

/* The park being watched (1, 2, ...), or 0. */
u32 rsx_draw_engine_park_id(void) { return s_park.flips_left ? s_park.parks : 0u; }

/* Once per guest flip. */
static void eng_park_flip(void)
{
    if (!s_park.flips_left) return;
    s_park.flip++;
    if (--s_park.flips_left == 0) {
        fprintf(stderr, "[park] park %u watch ended after %u flips: %u textures bound from the ranges, %u stale cache hits%c",
                s_park.parks, s_park.flip, s_park.n_hits, s_park.stale, 10);
        return;
    }
    if (s_park.flip & 3u) return;
    const u8* a = eng_guest_ptr(NULL, s_park.src_loc, s_park.src_off, s_park.bytes);
    const u8* b = eng_guest_ptr(NULL, s_park.dst_loc, s_park.dst_off, s_park.bytes);
    if (!a || !b) return;
    u32 eq = 0, n = 0, d0 = 0xFFFFFFFFu, d1 = 0;
    for (u32 o = 0; o < s_park.bytes; o += 65536u, n++) {
        const u32 len = s_park.bytes - o < 65536u ? s_park.bytes - o : 65536u;
        if (!memcmp(a + o, b + o, len)) eq++;
        else { if (d0 == 0xFFFFFFFFu) d0 = o; d1 = o + len; }
    }
    if (s_park.compared && eq == s_park.eq_chunks) return;
    if (s_park.cmp_lines++ < 64) {
        if (eq == n) fprintf(stderr, "[park] flip %u: the parked source equals the copy again (all %u chunks)%c", s_park.flip, n, 10);
        else fprintf(stderr, "[park] flip %u: %u of %u 64 KB chunks of the parked source equal the copy; differing +0x%X..+0x%X%c",
                     s_park.flip, eq, n, d0, d1, 10);
    }
    s_park.compared = 1; s_park.eq_chunks = eq;
}

static u32 eng_tex_find(const u32 id[8])        /* textures[] + 1, or 0 */
{
    for (u32 hi = eng_tex_index_hash(id);; hi = (hi + 1u) & (ENG_TEX_INDEX - 1u)) {
        const u32 slot = g.tex_index[hi];
        if (!slot) return 0;
        if (!memcmp(&g.textures[slot - 1u], id, 8u * sizeof(u32))) return slot;
    }
}

/* eng_texture_slot_core, plus the park watch's report for a texture in a
 * watched range. */
static u32 eng_texture_slot(u32 location, u32 offset, u32 fmt, u32 w, u32 h,
                            u32 levels, u32 pitch, int cube, u32 remap)
{
    const int pr = s_park.flips_left ? eng_park_range(location, offset, w * h * 4u) : 0;
    if (!pr) return eng_texture_slot_core(location, offset, fmt, w, h, levels, pitch, cube, remap);
    const u32 id[8] = { location, offset, fmt, w, h, pitch, remap, (u32)(cube != 0) };
    const u32 k0 = eng_tex_find(id);
    const u64 h0 = k0 ? g.textures[k0 - 1u].content_hash : 0;
    const u32 hd0 = k0 ? g.textures[k0 - 1u].handle : 0;
    const u32 r = eng_texture_slot_core(location, offset, fmt, w, h, levels, pitch, cube, remap);
    const u32 k1 = eng_tex_find(id);
    const int redec = k0 && k1 && (g.textures[k1 - 1u].content_hash != h0 || g.textures[k1 - 1u].handle != hd0);
    u32 hi = 0;
    while (hi < s_park.n_hits && !(s_park.hit_loc[hi] == location && s_park.hit_off[hi] == offset)) hi++;
    const int first = hi == s_park.n_hits;
    if (first) {
        if (hi >= ENG_PARK_HITS) return r;
        s_park.n_hits++;
        s_park.hit_loc[hi] = location; s_park.hit_off[hi] = offset;
        s_park.hit_check[hi] = s_park.flip; s_park.hit_redecodes[hi] = 0; s_park.hit_stale[hi] = 0;
    }
    const u32 base = pr == 1 ? s_park.src_off : s_park.dst_off;
    const char* st = !r ? "not bound" : !k0 ? "new: decoded now" : redec ? "memory changed: re-decoded" : "cached";
    int say = first || (redec && s_park.hit_redecodes[hi] < 2u);
    if (redec && s_park.hit_redecodes[hi] < 255u) s_park.hit_redecodes[hi]++;
    /* A cached one: does guest memory still hold what was decoded? */
    if (r && k0 && !redec && k1 && s_park.flip - s_park.hit_check[hi] >= 32u) {
        s_park.hit_check[hi] = s_park.flip;
        const eng_texture* e = &g.textures[k1 - 1u];
        int rd = 0;
        const u32 span = e->span ? e->span : eng_texture_span(fmt, w, h, levels, pitch, cube);
        if (span && eng_texture_content_hash(location, offset, span, &rd) != e->content_hash && rd) {
            s_park.stale++;
            if (s_park.hit_stale[hi] < 2u) { s_park.hit_stale[hi]++; say = 1; st = "STALE: guest memory differs from what was decoded, no write seen"; }
        }
    }
    if (say && s_park.tex_lines < 200u) { s_park.tex_lines++;
        fprintf(stderr, "[park] flip %u: texture %u:0x%08X %ux%u fmt 0x%02X levels %u from the parked %s (+0x%X): %s%c",
                s_park.flip, location, offset, w, h, fmt, levels, pr == 1 ? "source" : "copy",
                offset > base ? offset - base : 0u, st, 10); }
    return r;
}

/* SET_TEXTURE_FILTER's min field is [18:16] (1 NEAREST, 2 LINEAR, then 3..6,
 * the four combinations of a nearest/linear minification with a
 * nearest/linear mip filter) and mag [26:24]; SET_TEXTURE_CONTROL0 carries
 * max LOD at [18:7] and min LOD at [30:19], both 4.8 fixed point. RSX's LOD
 * bias, SET_TEXTURE_FILTER [12:0], is not applied: the reference engine
 * leaves it at zero too. */
static void eng_decode_sampler(u32 filter, u32 wrap, u32 control0,
                               rsx_be_sampler_desc* out)
{
    const u32 minf = (filter >> 16) & 7u;
    const u32 magf = (filter >> 24) & 7u;
    out->min_linear  = (u8)(minf == 2 || minf == 4 || minf == 6);
    out->mag_linear  = (u8)(magf == 2);
    out->mip_present = (u8)(minf >= 3);
    out->mip_linear  = (u8)(minf == 5 || minf == 6);
    out->wrap_s = (u8)(wrap & 0xFu);
    out->wrap_t = (u8)((wrap >> 8) & 0xFu);
    out->wrap_r = (u8)((wrap >> 16) & 0xFu);
    out->min_lod = (float)((control0 >> 19) & 0xFFFu) / 256.0f;
    out->max_lod = out->mip_present ? (float)((control0 >> 7) & 0xFFFu) / 256.0f
                                    : 0.0f;
    if (out->max_lod < out->min_lod) out->max_lod = out->min_lod;
}

/* ---- pipelines ----------------------------------------------------------- */

static u32 eng_vtex_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_VERTEX_TEXTURES; u++) {
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        if (vt.enabled && vt.width && vt.height) mask |= 1u << u;
    }
    return mask;
}

static int eng_is_depth_fmt(u32 format)
{
    const u32 b = format & RSX_TEX_FMT_BASE_MASK & ~(u32)RSX_TEX_FMT_UNNORM;
    return b >= RSX_TEX_FMT_DEPTH24_D8 && b <= RSX_TEX_FMT_DEPTH16_F;
}

/* Units sampling a depth texture with a compare function (TEX_ADDRESS ZFUNC,
 * bits 28-31, not NEVER): the hardware returns (ref OP depth), PCF-filtered,
 * not the depth. A Crack in Time's character shadows are a 64x64 DEPTH16 map
 * read this way; sampled as plain depth, every pixel of the shadow volume
 * read as shadowed -- a dark box around the player. */
static u32 eng_shadow_mask(u8 func[16])
{
    u32 mask = 0;
    for (u32 u = 0; u < 16; u++) {
        func[u] = 0;
        if (u >= RSX_DSP_NUM_TEXTURES) continue;
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        if (!t.enabled || !eng_is_depth_fmt(t.format)) continue;
        const u32 zf = (t.wrap >> 28) & 7u;
        if (!zf) continue;
        func[u] = (u8)zf;
        /* RSX_ENG_SHADOW_FUNC=<n>: every compare uses function n instead
         * (7 = always lit/1, 8 = always 0) -- a diagnostic. */
        { static long ov = -2; if (ov == -2) { const char* e = getenv("RSX_ENG_SHADOW_FUNC"); ov = e ? atol(e) : -1; }
          if (ov >= 0) func[u] = (u8)ov; }
        mask |= 1u << u;
    }
    return mask;
}

/* Units whose format carries RSX_TEX_FMT_UNNORM: the program samples them
 * with texel coordinates (Bink's YUV planes in A Crack in Time's movies), and
 * the HLSL sampler wants 0..1, so the coordinates are divided by the image
 * size in the shader (rsx_fp_apply_unnorm_scale -- same as rsx_live_draw). */
static u32 eng_unnorm_mask(u32 dim[][2])
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        dim[u][0] = dim[u][1] = 0;
        if (!t.enabled || !(t.format & RSX_TEX_FMT_UNNORM) || !t.width || !t.height) continue;
        mask |= 1u << u;
        dim[u][0] = t.width; dim[u][1] = t.height;
    }
    return mask;
}

/* Units whose TEXTURE_ADDRESS unsigned remap (bits 12-15) is BIASED: the
 * unit returns 2x-1 (rsx_fp_biased_mask). */
static u32 eng_biased_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled && ((t.wrap >> 12) & 0xFu) == 1u) mask |= 1u << u;
    }
    return mask;
}

/* TEXTURE_ADDRESS gamma [20:23] (R, G, B, A): the unit converts those
 * channels from sRGB to linear on read. Four bits per unit. */
static u64 eng_gamma_mask(void)
{
    u64 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        if (t.enabled) mask |= (u64)((t.wrap >> 20) & 0xFu) << (u * 4u);
    }
    return mask;
}

static u32 eng_cube_mask(void)
{
    u32 mask = 0;
    for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES; u++) {
        rsx_dsp_texture t;
        rsx_dsp_get_texture(&g.rsx, u, &t);
        /* A cube face is square by construction, so a non-square image
         * cannot be one; the fragment program is compiled against this mask
         * and a texturecube slot filled with a 2D texture is a validation
         * failure, not a wrong pixel. */
        if (t.enabled && t.cubemap && t.width && t.width == t.height)
            mask |= 1u << u;
    }
    return mask;
}

/* Are both of the guest's own programs resident? One answer, used by the
 * layout and by the pipeline, because they must not disagree: the built-in
 * program declares all sixteen inputs, so narrowing the layout for it would
 * leave the pipeline's vertex descriptor short of what the shader reads. */
/* The fragment program eng_guest_programs last read; `serial` changes
 * whenever it had to be read again. */
static struct { int valid; u32 reg, size, epoch, serial; const u8* ptr; } s_fpc;

static int eng_guest_programs(const u8** out_vp, u32* out_vp_instrs,
                              const u8** out_fp, u32* out_fp_size)
{
    const u8* vp_uc = NULL;
    u32 vp_instrs = 0;
    const u32 start = rsx_dsp_vp_start(&g.rsx);
    if (start < RSX_DSP_VP_INSTR) {
        vp_uc = (const u8*)(g.rsx.vp + start * 4);
        /* The program only changes with an upload: size it once per
         * (upload generation, start slot), not on every draw. */
        if (g.vp_cache_gen != g.rsx.vp_gen + 1u || g.vp_cache_start != start) {
            g.vp_cache_instrs = rsx_vp_program_size_instrs(
                vp_uc, (RSX_DSP_VP_INSTR - start) * 16u);
            g.vp_cache_hash = 0;
            g.vp_cache_gen = g.rsx.vp_gen + 1u;
            g.vp_cache_start = start;
        }
        vp_instrs = g.vp_cache_instrs;
    }

    /* A zero SET_SHADER_PROGRAM is "none", not "the program at offset 0": the
     * register carries the location in its low two bits, so a real program can
     * never read back as zero. */
    const u8* fp_uc = NULL;
    u32 fp_size = 0;
    const u32 fp_reg = rsx_dsp_reg(&g.rsx, 0x08E4);
    if (fp_reg) {
        /* The same program register, and none of the program's bytes written
         * since it was sized (rsx_writewatch.h): the same program. Titles patch
         * constants into their fragment programs between draws, and those
         * writes are exactly what makes this re-read. */
        if (s_fpc.valid && s_fpc.reg == fp_reg && s_fpc.ptr &&
            !rsx_ww_written_since(s_fpc.ptr, s_fpc.size, s_fpc.epoch)) {
            fp_uc = s_fpc.ptr; fp_size = s_fpc.size;
        } else {
            const u32 epoch = rsx_ww_on() ? rsx_ww_epoch() : 0u;
            u32 fp_loc = 0;
            const u32 fp_off = rsx_dsp_fragment_program(&g.rsx, &fp_loc);
            fp_uc = eng_guest_ptr(NULL, fp_loc, fp_off, 16);
            fp_size = fp_uc ? rsx_fp_program_size(fp_uc, ENG_FP_MAX_BYTES) : 0;
            if (fp_size) fp_uc = eng_guest_ptr(NULL, fp_loc, fp_off, fp_size);
            if (!fp_uc) fp_size = 0;
            s_fpc.valid = epoch != 0 && fp_size != 0;
            s_fpc.reg = fp_reg; s_fpc.ptr = fp_uc; s_fpc.size = fp_size; s_fpc.epoch = epoch;
            s_fpc.serial++;
        }
    }

    if (out_vp) *out_vp = vp_uc;
    if (out_vp_instrs) *out_vp_instrs = vp_instrs;
    if (out_fp) *out_fp = fp_uc;
    if (out_fp_size) *out_fp_size = fp_size;
    return vp_instrs && fp_size;
}

/* The layout the active vertex program actually reads. An uncertain analysis
 * falls back to all sixteen registers (rsx_live_draw.c:3625-3643). */
static void eng_vertex_layout(rsx_vertex_layout_plan* layout)
{
    u32 mask = 0xFFFFu;
    const u8* uc = NULL;
    u32 instrs = 0;
    if (eng_guest_programs(&uc, &instrs, NULL, NULL)) {
        /* What the vertex program reads changes only with an upload. */
        static u32 c_gen, c_start, c_mask;
        if (c_gen != g.vp_cache_gen || c_start != g.vp_cache_start) {
            rsx_vp_input_analysis analysis = { 0xFFFFu, 0 };
            c_mask = 0xFFFFu;
            if (rsx_vp_analyze_inputs(uc, instrs * 16u, &analysis) == (int)instrs &&
                analysis.exact && analysis.input_mask)
                c_mask = analysis.input_mask;
            c_gen = g.vp_cache_gen; c_start = g.vp_cache_start;
        }
        mask = c_mask;
    }
    rsx_vertex_layout_plan_init(layout, mask);
}

/* What a draw runs before a title has loaded programs of its own.
 *
 * The reference engine has no fallback and drops such a draw, because a title
 * always has both programs resident by the time it draws anything. A general
 * toolkit does not get to assume that: the host harness's fixed-function modes
 * are exactly this case, and so is the first frame of a title that clears and
 * flips before its first SET_TRANSFORM_PROGRAM. It is written in the same HLSL
 * shape the decompilers emit -- ATTRn inputs, the COLOR0/TEXCOORD varyings, the
 * VPConst block and the viewport epilogue -- so it goes through the one
 * pipeline_create the backend already implements rather than needing a second
 * entry point for a built-in shader.
 *
 * Vertex constant rows 0..3 are the transform when the guest programmed them
 * and identity when it did not: an all-zero matrix would collapse every vertex
 * onto the origin. */
static const char* const kEngFixedVS =
"struct VSInput {\n"
"    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;\n"
"    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;\n"
"    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;\n"
"    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;\n"
"};\n"
"struct VSOutput {\n"
"    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
"    float4 fog:FOG;\n"
"    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
"    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
"};\n"
"cbuffer VPConst : register(b0) {\n"
"    float4 vp_c[512];\n"
"    float4 vp_posscale;\n"
"    float4 vp_posoffset;\n"
"};\n"
"VSOutput main(VSInput input) {\n"
"    float4 _p = float4(dot(vp_c[0], input.a0), dot(vp_c[1], input.a0),\n"
"                       dot(vp_c[2], input.a0), dot(vp_c[3], input.a0));\n"
"    float _m = dot(abs(vp_c[0]), 1.0) + dot(abs(vp_c[1]), 1.0)\n"
"             + dot(abs(vp_c[2]), 1.0) + dot(abs(vp_c[3]), 1.0);\n"
"    if (_m == 0.0) _p = input.a0;\n"
"    VSOutput Out;\n"
"    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
"    Out.col0 = input.a3; Out.col1 = float4(0,0,0,1); Out.fog = (float4)0;\n"
"    Out.t0 = input.a8; Out.t1 = (float4)0; Out.t2 = (float4)0; Out.t3 = (float4)0;\n"
"    Out.t4 = (float4)0; Out.t5 = (float4)0; Out.t6 = (float4)0; Out.t7 = (float4)0;\n"
"    return Out;\n"
"}\n";

static const char* const kEngFixedPS =
"struct PSInput {\n"
"    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
"    float4 fog : FOG;\n"
"    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
"    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
"};\n"
"float4 main(PSInput input) : SV_TARGET { return input.col0; }\n";

/* The table is full: retire the least recently used quarter of it -- never
 * a pipeline used in the last ~5 s -- release their backend objects and
 * rebuild the index. A long session across many levels meets far more
 * program and state combinations than the table holds; without this every
 * new one after the 8192nd failed and its draws vanished for good. Returns
 * how many were retired. */
static int eng_pipeline_cmp_u32(const void* a, const void* b)
{
    const u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : x > y;
}
static u32 eng_pipeline_evict(void)
{
    const u32 n = g.n_pipelines, recent = 300u;
    static u32 ages[ENG_MAX_PIPELINES];
    for (u32 i = 0; i < n; i++) ages[i] = g.pipelines[i].used;
    qsort(ages, n, sizeof ages[0], eng_pipeline_cmp_u32);
    const u32 cutoff = ages[n / 4u];                 /* the oldest quarter's newest use */
    u32 keep = 0, gone = 0;
    for (u32 i = 0; i < n; i++) {
        const eng_pipeline p = g.pipelines[i];
        if (p.used <= cutoff && g.frames - p.used > recent) {
            if (p.handle) g.be->pipeline_release(g.be->user, p.handle);
            gone++;
        } else {
            g.pipelines[keep++] = p;
        }
    }
    g.n_pipelines = keep;
    memset(g.pipe_index, 0, sizeof g.pipe_index);
    for (u32 i = 0; i < keep; i++) {
        const u64 k = g.pipelines[i].key;
        for (u32 h = (u32)(k ^ (k >> 29)) & (ENG_PIPE_INDEX - 1u);; h = (h + 1u) & (ENG_PIPE_INDEX - 1u))
            if (!g.pipe_index[h]) { g.pipe_index[h] = i + 1u; break; }
    }
    static u32 said;
    if (said++ < 16)
        fprintf(stderr, "[rsx engine] pipeline table full: retired %u not used for a while (%u kept)%c", gone, keep, 10);
    return gone;
}

/* The pipeline for this draw, built once per distinct key. A negative result
 * is cached too (handle 0), so a program pair that will not translate is not
 * retried on every draw of every frame. */
static u64 s_pstage[5];   /* programs, masks, constants, key, lookup */
/* RSX_FP_DYNAMIC_TEX=0 bakes the texture modes into the programs again. */
static int eng_dyn_tex(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_FP_DYNAMIC_TEX"); on = !(e && *e == '0'); }
    return on;
}
/* The texture-derived modes of the draw eng_pipeline_get last saw: what
 * fp_texp in the fragment constants carries when they are dynamic. */
static u32 s_tm_unnorm, s_tm_biased, s_tm_cube;
static u64 s_tm_gamma;
static u32 s_tm_dim[RSX_DSP_NUM_TEXTURES][2];
#define PSTAGE(i) do { const u64 _n = eng_rdtsc(); s_pstage[i] += _n - _pt; _pt = _n; } while (0)
static u64 s_last_pipe_key;   /* the key eng_pipeline_get last resolved (RSX_ENG_PROBE) */
static int s_srgb_draw;       /* the draw eng_pipeline_get last saw packs its colour as sRGB */
static u32 eng_pipeline_get(const rsx_vertex_layout_plan* layout,
                            const rsx_be_render_state* rs,
                            rsx_be_format rt_fmt, u32 rt_count, int* out_fixed)
{
    u64 _pt = eng_rdtsc();
    *out_fixed = 1;
    const u8* vp_uc = NULL;
    const u8* fp_uc = NULL;
    u32 vp_instrs = 0, fp_size = 0;
    /* No resident program pair means the built-in one; see kEngFixedVS. */
    const int fixed = !eng_guest_programs(&vp_uc, &vp_instrs, &fp_uc, &fp_size);
    PSTAGE(0);

    const u32 fp_ctrl  = rsx_dsp_shader_control(&g.rsx);
    /* The texture-derived masks change only with texture registers: kept
     * until a texture or vertex-texture register takes a new value. */
    static struct { int valid; u32 tgen, vgen, cube, vtex, unnorm, shadow, biased; u64 gamma; u32 dim[RSX_DSP_NUM_TEXTURES][2]; u8 func[16]; } mc;
    if (!mc.valid || mc.tgen != g.rsx.tex_gen_all || mc.vgen != g.rsx.vtex_gen) {
        mc.cube = eng_cube_mask();
        mc.vtex = eng_vtex_mask();
        mc.unnorm = eng_unnorm_mask(mc.dim);
        mc.shadow = eng_shadow_mask(mc.func);
        mc.biased = eng_biased_mask();
        mc.gamma = eng_gamma_mask();
        mc.tgen = g.rsx.tex_gen_all; mc.vgen = g.rsx.vtex_gen; mc.valid = 1;
    }
    const u32 cube_mask = mc.cube;
    const u32 vtex_mask = mc.vtex;
    u32 unnorm_dim[RSX_DSP_NUM_TEXTURES][2];
    memcpy(unnorm_dim, mc.dim, sizeof unnorm_dim);
    const u32 unnorm_mask = mc.unnorm;
    u8 shadow_func[16];
    memcpy(shadow_func, mc.func, sizeof shadow_func);
    const u32 shadow_mask = mc.shadow;
    const u32 biased_mask = mc.biased;
    const u64 gamma_mask = mc.gamma;
    const int dyn_tex = eng_dyn_tex();
    s_tm_unnorm = unnorm_mask; s_tm_biased = biased_mask; s_tm_gamma = gamma_mask; s_tm_cube = cube_mask;
    memcpy(s_tm_dim, unnorm_dim, sizeof s_tm_dim);
    /* SET_SHADER_PACKER (0x1FEC) bit 0: colour outputs are written as sRGB
     * (the RSX's sRGB-write, libgcm's fragment program gamma). A Crack in
     * Time sets it for its colour passes and clears it for the G-buffer, and
     * reads the colour back through gamma-flagged texture units, so the two
     * halves only work together. RSX_FP_SRGB_OUT=0 disables it. */
    static int s_srgb_env = -1;
    if (s_srgb_env < 0) { const char* e = getenv("RSX_FP_SRGB_OUT"); s_srgb_env = !(e && *e == '0'); }
    /* As RPCS3 has it: only 8-bit targets are packed as sRGB, and never a
     * program exporting 32-bit registers (SHADER_CONTROL 0x40) -- A Crack in
     * Time's depth-to-colour copy, whose bytes are depth, not colour. */
    const int srgb_out = s_srgb_env && (rsx_dsp_reg(&g.rsx, 0x1FECu) & 1u) &&
                         rt_fmt == RSX_BE_FMT_R8G8B8A8 && !(fp_ctrl & 0x40u) ? 1 : 0;
    s_srgb_draw = srgb_out;
    PSTAGE(1);

    /* The same fragment program as the last draw (s_fpc.serial): its inline
     * constants and structure are what they were. */
    static u32 c_serial; static int c_ok; static u64 c_fhash;
    const int fp_same = !fixed && c_ok && c_serial == s_fpc.serial && s_fpc.valid && fp_uc == s_fpc.ptr;
    if (!fp_same) {
        g.fp_constants.count = 0;   /* not the 32 KB block: consumers read `count` */
        c_ok = 0;
        if (!fixed && rsx_fp_collect_constants(fp_uc, fp_size, &g.fp_constants) < 0)
            return 0;
    }
    PSTAGE(2);

    /* Identity: the vertex program's own bytes, the fragment program's
     * STRUCTURE (its inline constants are hoisted into the buffered block, so
     * a constant change must not be a new pipeline), the export-width bit of
     * SHADER_CONTROL, the cube and vertex-texture masks, the input layout,
     * and the structural render state. */
    u64 key = 1469598103934665603ull;
    u32 branch_bits = 0;
    if (fixed) {
        static const u32 fixed_tag = 0x4E464958u;   /* "XIFN" */
        key = eng_fnv1a(&fixed_tag, sizeof fixed_tag, key);
    } else {
        if (!g.vp_cache_hash) {
            g.vp_cache_hash = eng_fnv1a(vp_uc, vp_instrs * 16u, 1469598103934665603ull) | 1u;
            g.vp_uses_bbits = rsx_vp_uses_branch_bits(vp_uc, vp_instrs * 16u);
        }
        key = eng_fnv1a(&g.vp_cache_hash, sizeof g.vp_cache_hash, key);
        if (g.vp_uses_bbits) {
            branch_bits = rsx_dsp_reg(&g.rsx, 0x1FF8u);     /* TRANSFORM_BRANCH_BITS */
            key = eng_fnv1a(&branch_bits, sizeof branch_bits, key ^ 0xB8B8ull);
        }
        if (!fp_same) {
            c_fhash = rsx_fp_structural_hash(fp_uc, fp_size, 1469598103934665603ull);
            if (!c_fhash) return 0;
            c_serial = s_fpc.serial; c_ok = s_fpc.valid && fp_uc == s_fpc.ptr;
        }
        key = eng_fnv1a(&c_fhash, sizeof c_fhash, key);
        const u32 fp_ctrl_key = fp_ctrl & 0x4Eu;   /* export width + depth export */
        key = eng_fnv1a(&fp_ctrl_key, sizeof fp_ctrl_key, key);
        key = eng_fnv1a(&cube_mask, sizeof cube_mask, key);
        key = eng_fnv1a(&vtex_mask, sizeof vtex_mask, key);
        if (dyn_tex) {
            static const u32 dyn_tag = 0x44594E54u;   /* "TNYD": a different program text */
            key = eng_fnv1a(&dyn_tag, sizeof dyn_tag, key);
        } else {
            if (unnorm_mask) {
                key = eng_fnv1a(&unnorm_mask, sizeof unnorm_mask, key);
                key = eng_fnv1a(unnorm_dim, sizeof unnorm_dim, key);
            }
            if (biased_mask) key = eng_fnv1a(&biased_mask, sizeof biased_mask, key ^ 0xB1A5ull);
            if (gamma_mask) key = eng_fnv1a(&gamma_mask, sizeof gamma_mask, key ^ 0x6A33ull);
        }
        if (srgb_out) key = eng_fnv1a(&srgb_out, sizeof srgb_out, key ^ 0x5267ull);
        if (shadow_mask) {
            key = eng_fnv1a(&shadow_mask, sizeof shadow_mask, key);
            key = eng_fnv1a(shadow_func, sizeof shadow_func, key);
        }
    }
    key = eng_fnv1a(&layout->mask, sizeof layout->mask, key);
    key = eng_fnv1a(&layout->stride, sizeof layout->stride, key);
    key = eng_fnv1a(&rt_fmt, sizeof rt_fmt, key);
    /* How many colour attachments the pass will have is pipeline identity:
     * a host API matches the two against each other. */
    key = eng_fnv1a(&rt_count, sizeof rt_count, key);
    key = rsx_draw_engine_hash_render_state(rs, key);

    PSTAGE(3);
    /* Open-addressed index (pipelines are never removed). */
    for (u32 h = (u32)(key ^ (key >> 29)) & (ENG_PIPE_INDEX - 1u);; h = (h + 1u) & (ENG_PIPE_INDEX - 1u)) {
        const u32 slot = g.pipe_index[h];
        if (!slot) break;
        if (g.pipelines[slot - 1u].key == key) {
            PSTAGE(4);
            g.pipelines[slot - 1u].used = g.frames;
            s_last_pipe_key = key;
            *out_fixed = g.pipelines[slot - 1u].fixed;
            return g.pipelines[slot - 1u].handle;
        }
    }
    if (g.n_pipelines >= ENG_MAX_PIPELINES && !eng_pipeline_evict()) return 0;

    u32 handle = 0;
    int vi = 1, fi = 1;
    u32 nconst = 0;
    const u64 hp0_ = eng_hf_tsc();
    if (fixed) {
        snprintf(s_vs_hlsl, sizeof s_vs_hlsl, "%s", kEngFixedVS);
        snprintf(s_ps_hlsl, sizeof s_ps_hlsl, "%s", kEngFixedPS);
    } else {
        rsx_vp_branch_bits = branch_bits;
        vi = rsx_vp_decompile_compact_ex(vp_uc, vp_instrs * 16u, vtex_mask,
                                         layout->mask, s_vs_hlsl,
                                         sizeof s_vs_hlsl);
        rsx_vp_branch_bits = 0;
        rsx_fp_shadow_mask = shadow_mask;
        rsx_fp_biased_mask = dyn_tex ? 0u : biased_mask;
        rsx_fp_gamma_mask = dyn_tex ? 0u : gamma_mask;
        rsx_fp_dynamic_tex = dyn_tex;
        rsx_fp_srgb_out = srgb_out;
        memcpy(rsx_fp_shadow_func, shadow_func, sizeof shadow_func);
        fi = rsx_fp_decompile_buffered_ex(fp_uc, fp_size, fp_ctrl, cube_mask,
                                          s_ps_hlsl, sizeof s_ps_hlsl, &nconst);
        rsx_fp_shadow_mask = 0;
        rsx_fp_biased_mask = 0;
        rsx_fp_gamma_mask = 0;
        rsx_fp_dynamic_tex = 0;
        rsx_fp_srgb_out = 0;
        if (fi > 0 && nconst != g.fp_constants.count) fi = -1;
        if (fi > 0 && rs->alpha_test_enable &&
            rsx_fp_apply_alpha_test_buffered(s_ps_hlsl, sizeof s_ps_hlsl,
                                             rs->alpha_func) < 0)
            fi = -1;
        if (fi > 0 && dyn_tex) {
            /* Every 2D unit scales its coordinates by fp_texp: (1,1) unless
             * it is unnormalised, whatever it is bound to this draw. */
            if (rsx_fp_apply_unnorm_scale(s_ps_hlsl, sizeof s_ps_hlsl, 0xFFFFu & ~cube_mask,
                                          NULL, cube_mask) < 0)
                fi = -1;
        } else if (fi > 0 && unnorm_mask &&
            rsx_fp_apply_unnorm_scale(s_ps_hlsl, sizeof s_ps_hlsl, unnorm_mask,
                                      (const u32 (*)[2])unnorm_dim, cube_mask) < 0)
            fi = -1;
    }
    /* RSX_FP_DEBUG_KEY=<hex pipeline key>: that pipeline's fragment program
     * returns its inputs instead of its colour, in 24-pixel screen stripes:
     * TEXCOORD1.xy (fract), unit 0 sampled there (rgb), COLOR0.rgb, and
     * (TEXCOORD0.w, COLOR0.a, unit 0 alpha). Which input breaks a draw, in
     * one picture. */
    { static u64 s_dk = 1;
      static u64 s_dkl[32]; static int s_ndk;
      if (s_dk == 1) { const char* k = getenv("RSX_FP_DEBUG_KEY"); s_dk = 0;
                       for (const char* q = k; q && *q && s_ndk < 32; ) { char* e; const u64 v = strtoull(q, &e, 16);
                           if (e == q) break; s_dkl[s_ndk++] = v; s_dk = v; q = (*e == ',') ? e + 1 : e; } }
      int dk_hit = 0; for (int i = 0; i < s_ndk; i++) if (s_dkl[i] == key) dk_hit = 1;
      if (s_dk && dk_hit && fi > 0) {
          char* at = NULL;
          for (char* q = strstr(s_ps_hlsl, "return _rsx_out;"); q; q = strstr(q + 1, "return _rsx_out;")) at = q;
          static const char dbg[] =
              "{ uint _st = ((uint)(input.position.x / 24.0)) % 4u;\n"
              "  float4 _t = rsx_tex[0].Sample(rsx_samp[0], input.tc1.xy);\n"
              "  _rsx_out = _st == 0u ? float4(frac(input.tc1.xy), 0.0, 1.0) : _st == 1u ? float4(_t.rgb, 1.0) :\n"
              "             _st == 2u ? float4(input.col0.rgb, 1.0) : float4(input.tc0.w, input.col0.a, _t.a, 1.0); }\n";
          const size_t used = strlen(s_ps_hlsl), dl = sizeof dbg - 1;
          if (at && used + dl + 1 < sizeof s_ps_hlsl) {
              memmove(at + dl, at, used - (size_t)(at - s_ps_hlsl) + 1);
              memcpy(at, dbg, dl);
              fprintf(stderr, "[rsx engine] RSX_FP_DEBUG_KEY: pipeline %016llx shows its inputs%c", (unsigned long long)key, 10);
          }
      } }
    /* RSX_ENG_PIPE_LIMIT=<n>: pipelines created after the n-th are treated as
     * failed (their draws dropped, and logged below) -- to bisect which pass
     * breaks a picture. RSX_ENG_PIPE_ONLYSKIP=<n> drops just the n-th. */
    static long s_plimit = -2, s_pskip = -2;
    if (s_plimit == -2) { const char* e = getenv("RSX_ENG_PIPE_LIMIT"); s_plimit = e ? atol(e) : -1;
                          const char* k = getenv("RSX_ENG_PIPE_ONLYSKIP"); s_pskip = k ? atol(k) : -1; }
    /* RSX_ENG_PIPE_SKIPFILE=<file>: drop the pipelines whose keys (hex, one
     * per line, as the log prints them) are listed. Creation order changes
     * from run to run; the key does not. */
    static u64 s_skip[512]; static int s_nskip = -1;
    if (s_nskip < 0) {
        s_nskip = 0;
        const char* sf = getenv("RSX_ENG_PIPE_SKIPFILE");
        FILE* f = (sf && *sf) ? fopen(sf, "r") : NULL;
        if (f) { char ln[64];
                 while (s_nskip < 512 && fgets(ln, sizeof ln, f)) {
                     u64 k = strtoull(ln, 0, 16); if (k) s_skip[s_nskip++] = k; }
                 fclose(f); }
    }
    int key_drop = 0;
    for (int i = 0; i < s_nskip; i++) if (s_skip[i] == key) { key_drop = 1; break; }
    const int dbg_drop = (s_plimit >= 0 && (long)g.n_pipelines >= s_plimit) ||
                         (s_pskip >= 0 && (long)g.n_pipelines == s_pskip) || key_drop;
    /* RSX_ENG_PS_OVERRIDE=<dir>: <dir>/<key>_ps.hlsl, when it exists, is
     * compiled instead of the translated pixel shader of that pipeline -- an
     * edited RSX_ENG_HLSL_DUMP file, to test which term of a shader breaks a
     * picture without changing the translator. */
    { static const char* od = (const char*)-1;
      if (od == (const char*)-1) od = getenv("RSX_ENG_PS_OVERRIDE");
      if (od && *od && fi > 0) {
          char path[1024]; snprintf(path, sizeof path, "%s/%016llx_ps.hlsl", od, (unsigned long long)key);
          FILE* f = fopen(path, "rb");
          if (f) { const size_t n = fread(s_ps_hlsl, 1, sizeof s_ps_hlsl - 1, f); fclose(f); s_ps_hlsl[n] = 0;
                   fprintf(stderr, "[rsx engine] RSX_ENG_PS_OVERRIDE: pipeline %016llx uses %s (%u bytes)%c",
                           (unsigned long long)key, path, (unsigned)n, 10); } } }
    if (vi > 0 && fi > 0 && !dbg_drop)
        handle = g.be->pipeline_create(g.be->user, s_vs_hlsl, s_ps_hlsl, rs,
                                       layout, layout->stride, rt_fmt, rt_count);
    s_hf.pipes++; s_hf.pipe_cyc += eng_hf_tsc() - hp0_;
    /* The first 32 pipelines, and every failure (its own cap): a failed pair
     * drops every draw that uses it, silently, for the rest of the run -- in
     * A Crack in Time the level's resolve/composite passes, so the scene was
     * rendered and never reached the display. */
    { static u32 logs = 0, fails = 0;
      if (logs < 32 || (!handle && fails < 64)) {
        logs++; if (!handle) fails++;
        fprintf(stderr, "[rsx engine] pipeline #%u %016llx: %s vp %d (%u instrs), fp %d (%u bytes, ctrl 0x%X),"
                        " %u/%u constants, rt fmt %d x%u, vtx mask 0x%X stride %u -> %s\n",
                g.n_pipelines, (unsigned long long)key, fixed ? "built-in" : "guest",
                vi, vp_instrs, fi, fp_size, fp_ctrl, nconst, g.fp_constants.count,
                (int)rt_fmt, rt_count, (unsigned)layout->mask, (unsigned)layout->stride,
                handle ? "ok" : "FAILED (draw dropped)"); } }
    if (fp_size <= 64 && s_ps_hlsl[0]) {
        fprintf(stderr, "[pipe-ps] #%u %016llx (ctrl 0x%X):\n%s\n",
                g.n_pipelines, (unsigned long long)key, fp_ctrl, s_ps_hlsl);
    }
    /* RSX_ENG_PIPE_LOG=1: every pipeline built, with each part of its key, to
     * see which part keeps producing new ones. */
    { static int pl = -1; if (pl < 0) { const char* e = getenv("RSX_ENG_PIPE_LOG"); pl = e && *e && *e != '0'; }
      if (pl) {
        char dims[160]; int dn = 0; dims[0] = 0;
        for (u32 u = 0; u < RSX_DSP_NUM_TEXTURES && dn < 140; u++)
            if (unnorm_mask & (1u << u)) dn += snprintf(dims + dn, sizeof dims - dn, " %u:%ux%u", u, unnorm_dim[u][0], unnorm_dim[u][1]);
        u32 sf = 0; for (u32 u = 0; u < 16; u++) if (shadow_mask & (1u << u)) sf = sf * 8u + shadow_func[u];
        fprintf(stderr, "[pipe+] rsf at%u/%X bl%u %X/%X/%X/%X/%X/%X z%u/%u/%X cu%u/%X/%X cm%X st%u/%u %X %X/%X %X/%X/%X b%X/%X/%X/%X%c",
                rs->alpha_test_enable, rs->alpha_func, rs->blend_enable, rs->sf_rgb, rs->df_rgb, rs->sf_a, rs->df_a,
                rs->eq_rgb, rs->eq_a, rs->depth_test, rs->depth_write, rs->depth_func, rs->cull_enable, rs->cull_face,
                rs->front_face, rs->color_mask, rs->stencil_enable, rs->stencil_two_sided, rs->s_func, rs->s_func_mask,
                rs->s_write_mask, rs->s_fail, rs->s_zfail, rs->s_zpass, rs->bs_func, rs->bs_fail, rs->bs_zfail, rs->bs_zpass, 10);
        fprintf(stderr, "[pipe+] #%u %016llx vp %016llx bb %08X fp %016llx ctl %X cube %X vtex %X un %X%s sh %X/%X bi %X ga %llX srgb %d lay %X/%u rt %d x%u rs %016llx %s%c",
                g.n_pipelines, (unsigned long long)key, (unsigned long long)(fixed ? 0 : g.vp_cache_hash), branch_bits,
                (unsigned long long)(fixed ? 0 : c_fhash), fp_ctrl & 0x4Eu, cube_mask, vtex_mask, unnorm_mask, dims,
                shadow_mask, sf, biased_mask, (unsigned long long)gamma_mask, srgb_out,
                (unsigned)layout->mask, (unsigned)layout->stride, (int)rt_fmt, rt_count,
                (unsigned long long)rsx_draw_engine_hash_render_state(rs, 1469598103934665603ull),
                handle ? "ok" : "FAILED", 10);
      } }

    /* RSX_ENG_HLSL_DUMP=<dir>: the shaders of every pipeline built, as
     * p<index>_<key>_vs/_ps.hlsl, to read what a traced draw ran. */
    { static const char* hd = (const char*)-1; static u32 nd = 0;
      if (hd == (const char*)-1) hd = getenv("RSX_ENG_HLSL_DUMP");
      if (hd && *hd && nd < 4000) {
          nd++;
          char path[1024];
          snprintf(path, sizeof path, "%s/p%04u_%016llx_ps.hlsl", hd, g.n_pipelines, (unsigned long long)key);
          FILE* f = fopen(path, "wb"); if (f) { fprintf(f, "// ctrl 0x%08X%c", fp_ctrl, 10); fputs(s_ps_hlsl, f); fclose(f); }
          snprintf(path, sizeof path, "%s/p%04u_%016llx_vs.hlsl", hd, g.n_pipelines, (unsigned long long)key);
          f = fopen(path, "wb"); if (f) { fputs(s_vs_hlsl, f); fclose(f); }
      } }
    for (u32 h = (u32)(key ^ (key >> 29)) & (ENG_PIPE_INDEX - 1u);; h = (h + 1u) & (ENG_PIPE_INDEX - 1u))
        if (!g.pipe_index[h]) { g.pipe_index[h] = g.n_pipelines + 1u; break; }
    s_last_pipe_key = key;
    g.pipelines[g.n_pipelines].key = key;
    g.pipelines[g.n_pipelines].handle = handle;
    g.pipelines[g.n_pipelines].fixed = (u8)fixed;
    g.pipelines[g.n_pipelines].used = g.frames;
    g.n_pipelines++;
    *out_fixed = fixed;
    return handle;
}

/* ---- draw accumulation --------------------------------------------------- */

static void dc_reset(void)
{
    dc.n_arr = dc.n_idx = dc.n_packets = 0;
    dc.n_refs = dc.n_source_refs = dc.n_verts = dc.n_cuts = 0;
    dc.refs_remapped = 0;
    dc.fetch_ok = 1;
    dc.inl = NULL;
    dc.inl_bytes = 0;
}

static int dc_push_ref(u32 vertex_id, u32 base_index)
{
    if (dc.n_refs >= dc.cap_refs) {
        const u32 next = dc.cap_refs ? dc.cap_refs * 2u : 4096u;
        rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(dc.refs,
                                                     (size_t)next * sizeof(*r));
        if (!r) return 0;
        dc.refs = r;
        dc.cap_refs = next;
    }
    dc.refs[dc.n_refs].vertex_id = vertex_id;
    dc.refs[dc.n_refs].base_index = base_index;
    dc.n_refs++;
    return 1;
}

static int dc_reserve_verts(u64 bytes)
{
    if (bytes <= dc.verts_cap) return 1;
    u64 next = dc.verts_cap ? dc.verts_cap : (1u << 20);
    while (next < bytes) next *= 2u;
    u8* v = (u8*)realloc(dc.verts, (size_t)next);
    if (!v) return 0;
    dc.verts = v;
    dc.verts_cap = next;
    return 1;
}

static int dc_reserve_indices(u32 count)
{
    if (count <= g.index_cap) return 1;
    u32 next = g.index_cap ? g.index_cap : 4096u;
    while (next < count) next *= 2u;
    u32* p = (u32*)realloc(g.indices, (size_t)next * sizeof(u32));
    if (!p) return 0;
    g.indices = p;
    g.index_cap = next;
    return 1;
}

/* Turn this group's batches into a reference list, recording a CUT rather
 * than fetching a phantom vertex wherever the restart sentinel appears, then
 * collapse repeated references and fetch each unique vertex once.
 * rsx_live_draw.c's fetch_batches_hoisted (4165-4292). */
static void dc_fetch(const rsx_vertex_layout_plan* layout, int allow_remap)
{
    for (u32 b = 0; b < dc.n_arr && dc.fetch_ok; b++)
        for (u32 i = 0; i < dc.arr[b].count && dc.fetch_ok; i++)
            if (!dc_push_ref(dc.arr[b].first + i, 0)) dc.fetch_ok = 0;

    if (dc.n_idx && dc.fetch_ok) {
        const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 esz = ia.is_u32 ? 4u : 2u;
        const int restart_en = rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
        const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
        /* The index array's location field is a DMA selector, not the
         * rsx_dsp location enum: 0 selects local memory. */
        const u32 ia_loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;

        for (u32 b = 0; b < dc.n_idx && dc.fetch_ok; b++) {
            const u32 first = dc.idx[b].first, count = dc.idx[b].count;
            const u64 start = (u64)ia.offset + (u64)first * esz;
            const u64 bytes = (u64)count * esz;
            const u8* run = NULL;
            if (count && start <= 0xFFFFFFFFull && bytes <= 0xFFFFFFFFull &&
                start + bytes <= 0x100000000ull)
                run = eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes);
            for (u32 i = 0; i < count && dc.fetch_ok; i++) {
                const u8* p = run ? run + (size_t)i * esz
                    : eng_guest_ptr(NULL, ia_loc, ia.offset + (first + i) * esz, esz);
                if (!p) { dc.fetch_ok = 0; break; }
                const u32 index = ia.is_u32
                    ? (((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3])
                    : (u32)((p[0] << 8) | p[1]);
                if (restart_en && index == restart_val) {
                    if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts,
                                              dc.n_refs))
                        dc.fetch_ok = 0;
                    continue;
                }
                if (!dc_push_ref(index, base_index)) dc.fetch_ok = 0;
            }
        }
    }
    if (!dc.fetch_ok) return;

    dc.n_source_refs = dc.n_refs;
    /* Only a topology this engine rebuilds through an index buffer may share
     * repeated references. A line strip or a point list is drawn in the order
     * it arrived, so collapsing duplicates would reorder it. */
    if (allow_remap && dc.n_refs > 1) {
        u32 unique = dc.n_refs;
        if (rsx_vertex_remap_build(&dc.ref_remap, dc.refs, dc.n_refs, &unique)) {
            dc.refs_remapped = unique < dc.n_refs;
            dc.n_refs = unique;
        }
    }

    dc.layout = *layout;
    rsx_vertex_fetch_plan_init(&dc.fetch_plan, &g.rsx, layout, eng_guest_ptr, NULL);
    rsx_vertex_fetch_plan_set_inline(&dc.fetch_plan, &g.rsx, dc.inl, dc.inl_bytes);
    rsx_vertex_fetch_plan_prepare(&dc.fetch_plan, dc.refs, dc.n_refs);
    if (!dc_reserve_verts((u64)dc.n_refs * layout->stride)) {
        dc.fetch_ok = 0;
        return;
    }
    if (layout->stride) {
        if (!rsx_vertex_fetch_many(&dc.fetch_plan, dc.refs, dc.n_refs, dc.verts)) {
            dc.fetch_ok = 0;
            return;
        }
    } else {
        for (u32 i = 0; i < dc.n_refs; i++)
            if (!rsx_vertex_fetch_one(&dc.fetch_plan, &dc.refs[i], NULL)) {
                dc.fetch_ok = 0;
                return;
            }
    }
    dc.n_verts = dc.n_refs;
}

/* ---- geometry cache ------------------------------------------------------ */

/* What a draw hands the backend: converted vertices (one float4 per input
 * slot) and, for a rebuilt topology, the triangle-list indices. */
typedef struct {
    const u8*  verts;
    const u32* indices;     /* NULL = not indexed */
    u32 n_upload;           /* vertices uploaded */
    u32 n_draw;             /* indices (or vertices) drawn */
    u32 gpu;                /* backend geometry block holding all of it (0 = none) */
    int vb;                 /* gpu holds the vertices only (a vertex block); indices are ours */
    int vofs;               /* vb: the indices are elements; block vertex = index + vofs */
    int gpu_indexed;        /* gpu holds indices too (`indices` may be NULL: no CPU copy) */
} eng_geom;


/* A draw is converted once and reused while the guest bytes it was built
 * from are unchanged. Identity is the draw's DESCRIPTION (layout, attribute
 * formats and offsets, batches, index array); validity is a hash of the raw
 * index bytes and the vertex byte ranges the conversion actually read. An
 * entry only stores data on its second identical sighting, so geometry the
 * guest rewrites every frame (skinning, particles) costs one hash, not a
 * copy. In A Crack in Time the per-vertex conversion was half the render
 * thread's time per draw. */
#define ENG_GEOM_SETS    16384u
#define ENG_GEOM_WAYS    4u
#define ENG_GEOM_RANGES  16u
#define ENG_GEOM_BUDGET  (384ull << 20)
typedef struct {
    u64 key, content;
    u32 frame;
    u8  state;              /* 0 empty, 1 seen (no data), 2 data */
    u8  n_ranges;
    u8  rloc[ENG_GEOM_RANGES];
    u32 roff[ENG_GEOM_RANGES], rlen[ENG_GEOM_RANGES];
    u8* verts; u32 vbytes, n_upload;
    u32* indices; u32 n_draw;
    u32 ww_epoch;           /* rsx_ww epoch its content was last checked in (0 = never) */
    u32 gpu;                /* its backend geometry block (rsx_draw_backend.geom_upload), 0 = none */
    u64 icontent;           /* the index bytes' part of `content` (0 = unknown) */
    u32 held;               /* bytes it counts against ENG_GEOM_BUDGET */
    u8  indexed;            /* drawn with indices (held by `gpu` once that is set) */
} eng_geom_entry;
static eng_geom_entry* s_geom;          /* ENG_GEOM_SETS * ENG_GEOM_WAYS */
static u64 s_geom_bytes, s_geom_sweep_frame, s_geom_swept;
static u64 s_geom_hits, s_geom_misses, s_geom_uncached;
static u64 s_geom_miss_new, s_geom_miss_changed, s_geom_miss_promote, s_geom_miss_nocontent, s_geom_miss_bytes;
static int s_geom_on = -1;

static u64 eng_geom_key(const rsx_vertex_layout_plan* layout, u32 prim, int rebuild)
{
    if (dc.inl) return 0;                   /* the FIFO stream is the data */
    u32 hdr[6] = { prim, (u32)rebuild, layout->mask, layout->stride,
                   rsx_dsp_vertex_data_base_offset(&g.rsx), rsx_dsp_reg(&g.rsx, 0x1FC0) };
    u64 h = eng_hash_bytes(hdr, sizeof hdr, 0x6E67656F6Dull);
    for (u32 slot = 0; slot < layout->count; slot++) {
        struct { rsx_dsp_vertex_attr d; float def[4]; } a;
        memset(&a, 0, sizeof a);
        rsx_dsp_get_vertex_attr(&g.rsx, layout->attrs[slot], &a.d);
        rsx_dsp_vertex_default(&g.rsx, layout->attrs[slot], a.def);
        h = eng_hash_bytes(&a, sizeof a, h);
    }
    h = eng_hash_bytes(dc.arr, (size_t)dc.n_arr * sizeof dc.arr[0], h ^ dc.n_arr);
    if (dc.n_idx) {
        rsx_dsp_index_array ia; memset(&ia, 0, sizeof ia);
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 ix[6] = { ia.offset, ia.location, ia.is_u32,
                            rsx_dsp_vertex_data_base_index(&g.rsx),
                            (u32)rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32),
                            rsx_dsp_restart_index(&g.rsx) };
        h = eng_hash_bytes(ix, sizeof ix, h);
        h = eng_hash_bytes(dc.idx, (size_t)dc.n_idx * sizeof dc.idx[0], h ^ dc.n_idx);
    }
    return h | 1u;
}

/* The raw bytes behind a draw: its index runs, then the given vertex ranges.
 * 0 = some range is not readable (never cached). */
/* The index runs' hash on its own, as eng_geom_content starts it (0 =
 * unreadable). The title rewrites its index buffers every frame, so a draw's
 * index pages are nearly always "written" while its vertices are not:
 * comparing this alone saves hashing the vertex ranges. */
static u64 s_geom_last_ih;
static u64 eng_geom_icontent(void)
{
    u64 h = 0x636F6E74656E74ull;
    rsx_dsp_index_array ia;
    rsx_dsp_get_index_array(&g.rsx, &ia);
    const u32 esz = ia.is_u32 ? 4u : 2u;
    const u32 loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
    for (u32 b = 0; b < dc.n_idx; b++) {
        if (!dc.idx[b].count) continue;
        const u64 start = (u64)ia.offset + (u64)dc.idx[b].first * esz;
        const u64 bytes = (u64)dc.idx[b].count * esz;
        if (start + bytes > 0x100000000ull) return 0;
        const u8* p = eng_guest_ptr(NULL, loc, (u32)start, (u32)bytes);
        if (!p) return 0;
        h = bytes >= 256 ? eng_hash_long(p, (size_t)bytes, h) : eng_hash_bytes(p, (size_t)bytes, h);
    }
    return h | 1u;
}

static u64 eng_geom_content(u32 n_ranges, const u8* rloc, const u32* roff, const u32* rlen)
{
    u64 h = 0x636F6E74656E74ull;
    s_geom_last_ih = 0;
    if (dc.n_idx) {
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 esz = ia.is_u32 ? 4u : 2u;
        const u32 loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
        for (u32 b = 0; b < dc.n_idx; b++) {
            if (!dc.idx[b].count) continue;
            const u64 start = (u64)ia.offset + (u64)dc.idx[b].first * esz;
            const u64 bytes = (u64)dc.idx[b].count * esz;
            if (start + bytes > 0x100000000ull) return 0;
            const u8* p = eng_guest_ptr(NULL, loc, (u32)start, (u32)bytes);
            if (!p) return 0;
            h = bytes >= 256 ? eng_hash_long(p, (size_t)bytes, h) : eng_hash_bytes(p, (size_t)bytes, h);
        }
        s_geom_last_ih = h | 1u;
    }
    for (u32 i = 0; i < n_ranges; i++) {
        const u8* p = eng_guest_ptr(NULL, rloc[i], roff[i], rlen[i]);
        if (!p) return 0;
        h = rlen[i] >= 256 ? eng_hash_long(p, rlen[i], h) : eng_hash_bytes(p, rlen[i], h);
    }
    return h | 1u;
}

/* The entry's vertex ranges alone unwritten since it was last checked. */
static int eng_geom_vunwritten(const eng_geom_entry* e)
{
    if (!e->ww_epoch || !rsx_ww_on()) return 0;
    for (u32 i = 0; i < e->n_ranges; i++) {
        const u8* p = eng_guest_ptr(NULL, e->rloc[i], e->roff[i], e->rlen[i]);
        if (!p || rsx_ww_written_since(p, e->rlen[i], e->ww_epoch)) return 0;
    }
    return 1;
}

/* The draw's index runs and the entry's vertex ranges all unwritten since
 * the entry was last checked (rsx_writewatch.h): its content is unchanged
 * without hashing a byte. 0 = cannot tell, hash it. */
static int eng_geom_unwritten(eng_geom_entry* e)
{
    if (!e->ww_epoch || !rsx_ww_on()) return 0;
    /* Writes are stamped once per poll: checked clean in this very epoch,
     * nothing can have been written since. Most hits are draws the frame
     * before already validated, and the range walk below (a pointer resolve
     * and a page scan per range, all cold memory) was ~30% of a draw. */
    const u32 now = rsx_ww_epoch();
    if (e->ww_epoch == now) return 1;
    if (dc.n_idx) {
        rsx_dsp_index_array ia;
        rsx_dsp_get_index_array(&g.rsx, &ia);
        const u32 esz = ia.is_u32 ? 4u : 2u;
        const u32 loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
        for (u32 b = 0; b < dc.n_idx; b++) {
            if (!dc.idx[b].count) continue;
            const u64 start = (u64)ia.offset + (u64)dc.idx[b].first * esz;
            const u64 bytes = (u64)dc.idx[b].count * esz;
            if (start + bytes > 0x100000000ull) return 0;
            const u8* p = eng_guest_ptr(NULL, loc, (u32)start, (u32)bytes);
            if (!p || rsx_ww_written_since(p, (u32)bytes, e->ww_epoch)) return 0;
        }
    }
    for (u32 i = 0; i < e->n_ranges; i++) {
        const u8* p = eng_guest_ptr(NULL, e->rloc[i], e->roff[i], e->rlen[i]);
        if (!p || rsx_ww_written_since(p, e->rlen[i], e->ww_epoch)) return 0;
    }
    e->ww_epoch = now;
    return 1;
}

/* The vertex byte ranges the last dc_fetch read, overlapping ranges of one
 * location merged (interleaved attributes share one). 0 = not describable. */
static u32 eng_geom_ranges(u8* rloc, u32* roff, u32* rlen)
{
    u32 n = 0;
    const rsx_vertex_fetch_plan* fp = &dc.fetch_plan;
    for (u32 slot = 0; slot < dc.layout.count; slot++) {
        const rsx_vertex_fetch_attr* f = &fp->attr[dc.layout.attrs[slot]];
        if (!f->desc.type || !f->desc.size) continue;       /* constant default */
        if (!f->stable_base) return 0;
        const u64 off = (u64)fp->base_offset + f->desc.offset + (u64)f->stable_first * f->stride;
        const u64 len = (u64)(f->stable_last - f->stable_first) * f->stride + f->elem_size;
        if (off + len > 0x100000000ull) return 0;
        u32 i = 0;
        for (; i < n; i++) {
            if (rloc[i] != (u8)f->desc.location) continue;
            const u64 lo = roff[i], hi = (u64)roff[i] + rlen[i];
            if (off > hi || off + len < lo) continue;
            const u64 nlo = off < lo ? off : lo, nhi = off + len > hi ? off + len : hi;
            roff[i] = (u32)nlo; rlen[i] = (u32)(nhi - nlo);
            break;
        }
        if (i == n) {
            if (n == ENG_GEOM_RANGES) return 0;
            rloc[n] = (u8)f->desc.location; roff[n] = (u32)off; rlen[n] = (u32)len; n++;
        }
    }
    return n ? n : 0;
}

/* Convert this draw's vertices and build its indices (the uncached path).
 * Returns 0, or the ENG_DROP code of why the draw cannot be drawn. */
static int eng_geom_build(const rsx_vertex_layout_plan* layout, u32 prim, int rebuild, eng_geom* o)
{
    dc_fetch(layout, rebuild);
    if (!dc.n_verts || !dc.fetch_ok) return dc.n_verts ? 3 : 2;
    int indexed = 0;
    u32 n_draw = dc.n_source_refs;
    if (rebuild) {
        if (!eng_topology_rebuild(prim, dc.refs_remapped, &indexed)) return 4;
        n_draw = indexed
            ? rsx_draw_engine_topology_index_count(prim, dc.n_source_refs, dc.cuts, dc.n_cuts)
            : dc.n_source_refs - dc.n_source_refs % 3u;
    }
    if (!n_draw) return 5;
    if (indexed) {
        if (!dc_reserve_indices(n_draw)) return 5;
        rsx_draw_engine_write_topology_indices(
            prim, dc.n_source_refs, dc.cuts, dc.n_cuts,
            dc.refs_remapped ? dc.ref_remap.occurrence_to_unique : NULL, g.indices);
    }
    o->gpu = 0;
    o->verts = dc.verts;
    o->indices = indexed ? g.indices : NULL;
    o->n_upload = indexed ? dc.n_verts : n_draw;
    o->n_draw = n_draw;
    return 0;
}

static void eng_geom_free(eng_geom_entry* e)
{
    s_geom_bytes -= e->held;
    e->held = 0;
    if (e->gpu && g.be && g.be->geom_release) g.be->geom_release(g.be->user, e->gpu);
    e->gpu = 0;
    free(e->verts); free(e->indices);
    e->verts = NULL; e->indices = NULL; e->vbytes = 0; e->n_draw = 0; e->indexed = 0;
}

/* ---- vertex blocks -------------------------------------------------------
 * The route of an indexed draw whose full key missed. A Crack in Time culls
 * on the SPU and writes each mesh's surviving indices into a ring every
 * frame, so while the camera moves most draws arrive with new index bytes at
 * a new offset over the very same vertices -- and the key above, which
 * covers both, missed for over half the draws of a frame (Krell, running:
 * ~1100 of ~2000), each converted from scratch. A block is keyed by the
 * vertex streams alone and holds every vertex of the element span its draws
 * have referenced, converted once and kept on the GPU; a draw then only reads
 * its indices and rebases them onto the block. The vertex bytes a draw uses
 * are checked against the write watch; a block whose bytes were written is
 * converted again. RSX_ENG_VBLOCK=0 turns it off; =2 sends every draw it
 * can take its way, full-key hits too (a check that blocks draw the same). */
#define ENG_VB_SETS     4096u
#define ENG_VB_WAYS     4u
#define ENG_VB_MAX_SPAN (1u << 18)
#define ENG_VB_BUDGET   (96ull << 20)
typedef struct { u64 key; u32 lo, hi, gpu, epoch, frame, bytes; u8* cpu; } eng_vb_entry;
static eng_vb_entry* s_vb;
static int s_vb_on = -1;
static u32* s_vb_occ; static u32 s_vb_occ_cap;
static u8* s_vb_verts; static u64 s_vb_verts_cap;
static rsx_vertex_ref* s_vb_refs; static u32 s_vb_refs_cap;
static u64 s_vb_held, s_vb_sweep_frame;
static u64 s_vbst[5];   /* RSX_ENG_STATS: eligible, scan, ranges+content, block, sightings */
static u64 s_vb_hits, s_vb_builds, s_vb_grows, s_vb_dirty, s_vb_fallback, s_vb_conv_bytes;

static int eng_vb_eligible(const rsx_vertex_layout_plan* layout, int rebuild)
{
    if (s_vb_on < 0) {
        const char* e = getenv("RSX_ENG_VBLOCK");
        s_vb_on = (e && *e == '2' ? 2 : !(e && *e == '0')) && g.be->draw_cached_vi && g.be->geom_upload && g.be->geom_release && rsx_ww_on();
        if (s_vb_on && e && *e == '2') s_vb_on = 2;
        if (s_vb_on) s_vb = (eng_vb_entry*)calloc(ENG_VB_SETS * ENG_VB_WAYS, sizeof *s_vb);
        if (!s_vb) s_vb_on = 0;
    }
    if (!s_vb_on || dc.inl || !dc.n_idx || dc.n_arr || !rebuild || !layout->stride || !layout->count) return 0;
    for (u32 slot = 0; slot < layout->count; slot++) {
        rsx_dsp_vertex_attr d;
        rsx_dsp_get_vertex_attr(&g.rsx, layout->attrs[slot], &d);
        if (d.type && d.size && d.frequency > 1) return 0;     /* instanced: elements are not indices */
    }
    return 1;
}

/* The draw's indices as vertex elements into s_vb_occ, restart cuts into
 * dc.cuts, and the span they cover. 0 = not readable here. */
static u32 eng_vb_scan(u32* out_lo, u32* out_hi)
{
    const u32 base_index = rsx_dsp_vertex_data_base_index(&g.rsx);
    rsx_dsp_index_array ia;
    rsx_dsp_get_index_array(&g.rsx, &ia);
    const u32 esz = ia.is_u32 ? 4u : 2u;
    const int restart_en = rsx_dsp_restart_index_enabled(&g.rsx, ia.is_u32);
    const u32 restart_val = rsx_dsp_restart_index(&g.rsx);
    const u32 ia_loc = ia.location ? RSX_LOCATION_MAIN : RSX_LOCATION_LOCAL;
    u64 total = 0;
    for (u32 b = 0; b < dc.n_idx; b++) total += dc.idx[b].count;
    if (!total || total > 0x4000000u) return 0;
    if (total > s_vb_occ_cap) {
        u32 next = s_vb_occ_cap ? s_vb_occ_cap : 4096u;
        while (next < total) next *= 2u;
        u32* p = (u32*)realloc(s_vb_occ, (size_t)next * 4u);
        if (!p) return 0;
        s_vb_occ = p; s_vb_occ_cap = next;
    }
    u32 n = 0, lo = 0xFFFFFFFFu, hi = 0;
    dc.n_cuts = 0;
    #define VB_ONE(I) do { \
            const u32 index_ = esz == 4u \
                ? (((u32)p[4 * (I)] << 24) | ((u32)p[4 * (I) + 1] << 16) | ((u32)p[4 * (I) + 2] << 8) | p[4 * (I) + 3]) \
                : (u32)((p[2 * (I)] << 8) | p[2 * (I) + 1]); \
            if (restart_en && index_ == restart_val) { \
                if (!rsx_restart_cut_push(&dc.cuts, &dc.n_cuts, &dc.cap_cuts, n)) return 0; \
            } else { \
                const u32 e_ = (index_ + base_index) & 0x000FFFFFu; \
                s_vb_occ[n++] = e_; \
                if (e_ < lo) lo = e_; \
                if (e_ > hi) hi = e_; \
            } } while (0)
    for (u32 b = 0; b < dc.n_idx; b++) {
        const u32 count = dc.idx[b].count;
        if (!count) continue;
        const u64 start = (u64)ia.offset + (u64)dc.idx[b].first * esz;
        const u64 bytes = (u64)count * esz;
        if (start + bytes > 0x100000000ull) return 0;
        const u8* p = eng_guest_ptr(NULL, ia_loc, (u32)start, (u32)bytes);
        if (!p) return 0;
        u32 i = 0;
#if defined(__AVX2__)
        /* Sixteen 16-bit (eight 32-bit) indices a step: byte-swapped, based,
         * masked, min/max'd. A step with the restart index in it goes one
         * at a time. */
        {
            const __m256i vbase = _mm256_set1_epi32((int)base_index), vmask = _mm256_set1_epi32(0x000FFFFF);
            __m256i vlo = _mm256_set1_epi32(-1), vhi = _mm256_setzero_si256();
            if (esz == 2u) {
                const __m256i sw = _mm256_setr_epi8(1,0,3,2,5,4,7,6,9,8,11,10,13,12,15,14,
                                                    1,0,3,2,5,4,7,6,9,8,11,10,13,12,15,14);
                const int rchk = restart_en && restart_val <= 0xFFFFu;
                const __m256i rv = _mm256_set1_epi16((short)restart_val);
                for (; i + 16u <= count; i += 16u) {
                    const __m256i x = _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(p + 2u * i)), sw);
                    if (rchk) {
                        const __m256i m = _mm256_cmpeq_epi16(x, rv);
                        if (!_mm256_testz_si256(m, m)) { for (u32 k = 0; k < 16u; k++) VB_ONE(i + k); continue; }
                    }
                    const __m256i a = _mm256_and_si256(_mm256_add_epi32(_mm256_cvtepu16_epi32(_mm256_castsi256_si128(x)), vbase), vmask);
                    const __m256i c = _mm256_and_si256(_mm256_add_epi32(_mm256_cvtepu16_epi32(_mm256_extracti128_si256(x, 1)), vbase), vmask);
                    _mm256_storeu_si256((__m256i*)(s_vb_occ + n), a);
                    _mm256_storeu_si256((__m256i*)(s_vb_occ + n + 8u), c);
                    n += 16u;
                    vlo = _mm256_min_epu32(vlo, _mm256_min_epu32(a, c));
                    vhi = _mm256_max_epu32(vhi, _mm256_max_epu32(a, c));
                }
            } else {
                const __m256i sw = _mm256_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12,
                                                    3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
                const __m256i rv = _mm256_set1_epi32((int)restart_val);
                for (; i + 8u <= count; i += 8u) {
                    const __m256i x = _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(p + 4u * i)), sw);
                    if (restart_en) {
                        const __m256i m = _mm256_cmpeq_epi32(x, rv);
                        if (!_mm256_testz_si256(m, m)) { for (u32 k = 0; k < 8u; k++) VB_ONE(i + k); continue; }
                    }
                    const __m256i a = _mm256_and_si256(_mm256_add_epi32(x, vbase), vmask);
                    _mm256_storeu_si256((__m256i*)(s_vb_occ + n), a);
                    n += 8u;
                    vlo = _mm256_min_epu32(vlo, a);
                    vhi = _mm256_max_epu32(vhi, a);
                }
            }
            u32 l8[8], h8[8];
            _mm256_storeu_si256((__m256i*)l8, vlo); _mm256_storeu_si256((__m256i*)h8, vhi);
            for (u32 k = 0; k < 8u; k++) { if (l8[k] < lo) lo = l8[k]; if (h8[k] > hi) hi = h8[k]; }
        }
#endif
        for (; i < count; i++) VB_ONE(i);
    }
    #undef VB_ONE
    *out_lo = lo; *out_hi = hi;
    return n;
}

static int eng_vb_type_ok(u32 type) { return type >= RSX_VTX_TYPE_SNORM16 && type <= RSX_VTX_TYPE_UINT8; }

static u64 eng_vb_key(const rsx_vertex_layout_plan* layout)
{
    const u32 hdr[4] = { layout->mask, layout->stride, rsx_dsp_vertex_data_base_offset(&g.rsx), layout->count };
    u64 h = eng_hash_bytes(hdr, sizeof hdr, 0x76626C6F636Bull);
    for (u32 slot = 0; slot < layout->count; slot++) {
        struct { rsx_dsp_vertex_attr d; float def[4]; } a;
        memset(&a, 0, sizeof a);
        rsx_dsp_get_vertex_attr(&g.rsx, layout->attrs[slot], &a.d);
        /* The default only reaches a vertex for an attribute with no stream
         * (or one the decoder cannot read): a per-object constant colour in
         * it does not make the streamed attributes another block. */
        if (!a.d.type || !a.d.size || !eng_vb_type_ok(a.d.type))
            rsx_dsp_vertex_default(&g.rsx, layout->attrs[slot], a.def);
        h = eng_hash_bytes(&a, sizeof a, h);
    }
    return h | 1u;
}

/* The vertex byte ranges elements [a, b] read, merged as eng_geom_ranges
 * merges them (the same ranges, so the same content hash). With `epoch`,
 * also: none of them written since. 0 = unreadable, or written. */
static u32 eng_vb_ranges(const rsx_vertex_layout_plan* layout, u32 a, u32 b,
                         u8* rloc, u32* roff, u32* rlen, int check, u32 epoch)
{
    u32 n = 0;
    const u32 base = rsx_dsp_vertex_data_base_offset(&g.rsx);
    for (u32 slot = 0; slot < layout->count; slot++) {
        rsx_dsp_vertex_attr d;
        rsx_dsp_get_vertex_attr(&g.rsx, layout->attrs[slot], &d);
        if (!d.type || !d.size) continue;
        const u32 esz = rsx_vertex_attrib_size(d.type, d.size);
        const u32 stride = d.stride;                 /* 0: every vertex reads the one element */
        const u64 off = (u64)base + d.offset + (u64)a * stride;
        const u64 len = (u64)(b - a) * stride + esz;
        if (!esz || off + len > 0x100000000ull) return 0;
        const u8* p = eng_guest_ptr(NULL, d.location, (u32)off, (u32)len);
        if (!p) return 0;
        if (check && rsx_ww_written_since(p, (u32)len, epoch)) return 0;
        u32 i = 0;
        for (; i < n; i++) {
            if (rloc[i] != (u8)d.location) continue;
            const u64 rlo = roff[i], rhi = (u64)roff[i] + rlen[i];
            if (off > rhi || off + len < rlo) continue;
            const u64 nlo = off < rlo ? off : rlo, nhi = off + len > rhi ? off + len : rhi;
            roff[i] = (u32)nlo; rlen[i] = (u32)(nhi - nlo);
            break;
        }
        if (i == n) {
            if (n == ENG_GEOM_RANGES) return 0;
            rloc[n] = (u8)d.location; roff[n] = (u32)off; rlen[n] = (u32)len; n++;
        }
    }
    return n;
}

static void eng_vb_drop(eng_vb_entry* e)
{
    if (e->gpu) g.be->geom_release(g.be->user, e->gpu);
    s_vb_held -= e->bytes;
    free(e->cpu); e->cpu = NULL;
    e->gpu = 0; e->bytes = 0; e->key = 0;
}

/* Every vertex of elements [lo, hi] into s_vb_verts. */
static int eng_vb_convert(const rsx_vertex_layout_plan* layout, u32 lo, u32 hi)
{
    const u32 nv = hi - lo + 1u;
    if (nv > s_vb_refs_cap) {
        u32 next = s_vb_refs_cap ? s_vb_refs_cap : 4096u;
        while (next < nv) next *= 2u;
        rsx_vertex_ref* r = (rsx_vertex_ref*)realloc(s_vb_refs, (size_t)next * sizeof *r);
        if (!r) return 0;
        s_vb_refs = r; s_vb_refs_cap = next;
    }
    for (u32 i = 0; i < nv; i++) { s_vb_refs[i].vertex_id = lo + i; s_vb_refs[i].base_index = 0; }
    const u64 bytes = (u64)nv * layout->stride;
    if (bytes > s_vb_verts_cap) {
        u64 next = s_vb_verts_cap ? s_vb_verts_cap : (1u << 20);
        while (next < bytes) next *= 2u;
        u8* v = (u8*)realloc(s_vb_verts, (size_t)next);
        if (!v) return 0;
        s_vb_verts = v; s_vb_verts_cap = next;
    }
    static rsx_vertex_fetch_plan plan;
    rsx_vertex_fetch_plan_init(&plan, &g.rsx, layout, eng_guest_ptr, NULL);
    rsx_vertex_fetch_plan_prepare(&plan, s_vb_refs, nv);
    return rsx_vertex_fetch_many(&plan, s_vb_refs, nv, s_vb_verts);
}

/* 0 = drawn from a block (o filled), > 0 the ENG_DROP code, -1 = not here:
 * the caller converts it the ordinary way. s_vb_occ / dc.cuts hold the
 * draw's elements and cuts (eng_vb_scan). */
static int eng_vb_get(const rsx_vertex_layout_plan* layout, u32 prim, u32 n_occ, u32 a, u32 b, eng_geom* o)
{
    const u64 key = eng_vb_key(layout);
    eng_vb_entry* set = s_vb + (size_t)(key & (ENG_VB_SETS - 1u)) * ENG_VB_WAYS;
    eng_vb_entry* e = NULL;
    for (u32 w = 0; w < ENG_VB_WAYS; w++) if (set[w].key == key) { e = &set[w]; break; }
    u8 rloc[ENG_GEOM_RANGES]; u32 roff[ENG_GEOM_RANGES], rlen[ENG_GEOM_RANGES];
    int hit = e && e->lo <= a && b <= e->hi && eng_vb_ranges(layout, a, b, rloc, roff, rlen, 1, e->epoch);
    if (hit && eng_verify(4) && e->cpu && eng_vb_convert(layout, a, b)) {
        const u32 st = layout->stride, nb = (b - a + 1u) * st;
        if (memcmp(e->cpu + (size_t)(a - e->lo) * st, s_vb_verts, nb) != 0) {
            u32 first = 0; while (first < nb && e->cpu[(size_t)(a - e->lo) * st + first] == s_vb_verts[first]) first++;
            if (eng_verify_say("vb"))
                fprintf(stderr, "[verify] vertex block %016llX [%u,%u] reused for [%u,%u]: vertex %u slot %u differs from a fresh conversion (frame %u, epoch %u)%c",
                        (unsigned long long)key, e->lo, e->hi, a, b, a + first / st, (first % st) / 16u, g.frames, e->epoch, 10);
            hit = 0;
        }
    }
    if (hit)
        s_vb_hits++;
    else {
        u32 lo = a, hi = b;
        if (e) {
            const u32 ulo = e->lo < a ? e->lo : a, uhi = e->hi > b ? e->hi : b;
            if (uhi - ulo < ENG_VB_MAX_SPAN) { lo = ulo; hi = uhi; }
            if (e->lo <= a && b <= e->hi) s_vb_dirty++; else s_vb_grows++;
        }
        if (hi - lo >= ENG_VB_MAX_SPAN) return -1;
        const u32 now = rsx_ww_epoch();
        if (!eng_vb_ranges(layout, lo, hi, rloc, roff, rlen, 0, 0)) return -1;
        if (!eng_vb_convert(layout, lo, hi)) return -1;
        const u32 vbytes = (hi - lo + 1u) * layout->stride;
        u32 cls = 256u; while (cls < vbytes && cls < 0x80000000u) cls <<= 1;   /* what the heap rounds it to */
        /* Over budget: let go of the blocks no frame has used lately. */
        if (s_vb_held + cls > ENG_VB_BUDGET && s_vb_sweep_frame != g.frames) {
            s_vb_sweep_frame = g.frames;
            for (u32 i = 0; i < ENG_VB_SETS * ENG_VB_WAYS; i++)
                if (s_vb[i].key && s_vb[i].frame + 60u < g.frames && &s_vb[i] != e) eng_vb_drop(&s_vb[i]);
        }
        if (s_vb_held + cls > ENG_VB_BUDGET) return -1;
        const u32 gpu = g.be->geom_upload(g.be->user, s_vb_verts, vbytes, NULL, 0);
        if (!gpu) return -1;
        if (!e) {
            e = &set[0];
            for (u32 w = 0; w < ENG_VB_WAYS; w++) {
                if (!set[w].key) { e = &set[w]; break; }
                if (set[w].frame < e->frame) e = &set[w];
            }
        }
        if (e->key) eng_vb_drop(e);
        e->key = key; e->lo = lo; e->hi = hi; e->gpu = gpu; e->epoch = now; e->bytes = cls;
        if (eng_verify(4)) { e->cpu = (u8*)malloc(vbytes); if (e->cpu) memcpy(e->cpu, s_vb_verts, vbytes); }
        s_vb_held += cls;
        s_vb_builds++; s_vb_conv_bytes += vbytes;
    }
    e->frame = g.frames;

    /* The indices stay elements: the draw's base vertex takes the block's
     * first element off on the GPU. */
    const u32 n_draw = rsx_draw_engine_topology_index_count(prim, n_occ, dc.cuts, dc.n_cuts);
    if (!n_draw) return 5;
    if (prim == RSX_PRIMITIVE_TRIANGLES) o->indices = s_vb_occ;       /* already the list */
    else {
        if (!dc_reserve_indices(n_draw)) return 5;
        rsx_draw_engine_write_topology_indices(prim, n_occ, dc.cuts, dc.n_cuts, s_vb_occ, g.indices);
        o->indices = g.indices;
    }
    o->verts = NULL;
    o->n_upload = e->hi - e->lo + 1u;
    o->n_draw = n_draw;
    o->gpu = e->gpu;
    o->vb = 1;
    o->vofs = -(int)e->lo;
    return 0;
}

/* RSX_ENG_GEOM_CACHE=0 turns the cache off (every draw converted, as before). */
/* RSX_ENG_STATS: the fetch stage split -- layout plan, key, hit check, build. */
static u64 s_gstage[4];
static int eng_geom_get(const rsx_vertex_layout_plan* layout, u32 prim, int rebuild, eng_geom* o)
{
    u64 _gt = eng_rdtsc();
    o->vb = 0; o->gpu_indexed = 0;
    if (s_geom_on < 0) {
        const char* e = getenv("RSX_ENG_GEOM_CACHE");
        s_geom_on = !(e && *e == '0');
        if (s_geom_on) s_geom = (eng_geom_entry*)calloc(ENG_GEOM_SETS * ENG_GEOM_WAYS, sizeof *s_geom);
        if (!s_geom) s_geom_on = 0;
    }
    const u64 key = s_geom_on ? eng_geom_key(layout, prim, rebuild) : 0;
    { const u64 n_ = eng_rdtsc(); s_gstage[1] += n_ - _gt; _gt = n_; }
    if (!key) { s_geom_uncached++; const int r_ = eng_geom_build(layout, prim, rebuild, o);
                s_gstage[3] += eng_rdtsc() - _gt; return r_; }

    eng_geom_entry* set = s_geom + (size_t)(key & (ENG_GEOM_SETS - 1u)) * ENG_GEOM_WAYS;
    eng_geom_entry* e = NULL;
    for (u32 w = 0; w < ENG_GEOM_WAYS; w++) if (set[w].state && set[w].key == key) { e = &set[w]; break; }
    if (s_vb_on == 2 && eng_vb_eligible(layout, rebuild)) e = NULL;
    if (e && e->state == 2) {
        int same = eng_geom_unwritten(e);
        if (same && eng_verify(2) &&
            eng_geom_content(e->n_ranges, e->rloc, e->roff, e->rlen) != e->content) {
            same = 0;
            if (eng_verify_say("geom"))
                fprintf(stderr, "[verify] geometry key %016llX (%u ranges, first %u:0x%08X +%u) changed with no write seen (frame %u)%c",
                        (unsigned long long)key, e->n_ranges, e->n_ranges ? e->rloc[0] : 0u,
                        e->n_ranges ? e->roff[0] : 0u, e->n_ranges ? e->rlen[0] : 0u, g.frames, 10);
        }
        if (same) s_geom_ww_skips++;
        else {
            const u32 ww_now = rsx_ww_on() ? rsx_ww_epoch() : 0u;
            if (e->icontent && dc.n_idx && !eng_verify(2) && eng_geom_vunwritten(e)) {
                same = eng_geom_icontent() == e->icontent;      /* only the indices can differ */
            } else {
                same = eng_geom_content(e->n_ranges, e->rloc, e->roff, e->rlen) == e->content;
                if (same) e->icontent = s_geom_last_ih;
            }
            if (same) e->ww_epoch = ww_now;
        }
        if (!same) goto geom_miss;
        e->frame = g.frames;
        s_geom_hits++;
        s_gstage[2] += eng_rdtsc() - _gt;
        o->verts = e->verts; o->indices = e->indices;
        o->n_upload = e->n_upload; o->n_draw = e->n_draw;
        o->gpu = e->gpu;
        o->gpu_indexed = e->gpu && e->indexed && g.be->draw_cached;
        return 0;
    }

geom_miss:
    s_geom_misses++;
    { const u64 n_ = eng_rdtsc(); s_gstage[2] += n_ - _gt; _gt = n_; }
    u64 _vt = eng_rdtsc();
    #define VBST(i) do { const u64 n_ = eng_rdtsc(); s_vbst[i] += n_ - _vt; _vt = n_; } while (0)
    const int vb_ok = eng_vb_eligible(layout, rebuild);
    VBST(0);
    if (vb_ok) {
        u32 a = 0, b = 0;
        const u32 n_occ = eng_vb_scan(&a, &b);
        VBST(1);
        u8 rloc[ENG_GEOM_RANGES]; u32 roff[ENG_GEOM_RANGES], rlen[ENG_GEOM_RANGES];
        const u32 nr = n_occ ? eng_vb_ranges(layout, a, b, rloc, roff, rlen, 0, 0) : 0;
        /* The full-key entry's sightings go on as on the ordinary route, and
         * a draw seen unchanged twice is still kept whole (a full-key hit
         * reads no index at all); everything before that is a block draw. */
        const u64 content = (e && nr) ? eng_geom_content(nr, rloc, roff, rlen) : 0;
        const int promote = e && e->state == 1 && content && e->content == content;
        VBST(2);
        const int r = (n_occ && !promote) ? eng_vb_get(layout, prim, n_occ, a, b, o) : -1;
        VBST(3);
        if (r >= 0) {
            s_gstage[3] += eng_rdtsc() - _gt;
            if (r || !nr || (e && !content)) return r;
            if (!e) {
                e = &set[0];
                for (u32 w = 0; w < ENG_GEOM_WAYS; w++) {
                    if (!set[w].state) { e = &set[w]; break; }
                    if (set[w].frame < e->frame) e = &set[w];
                }
                if (e->state == 2) eng_geom_free(e);
                e->key = key; e->content = 0; e->icontent = 0; e->state = 1;
                s_geom_miss_new++;
            } else {
                if (e->state == 2) eng_geom_free(e);
                e->content = content; e->icontent = s_geom_last_ih; e->state = 1;
                s_geom_miss_changed++;
            }
            e->frame = g.frames;
            e->ww_epoch = rsx_ww_on() ? rsx_ww_epoch() : 0u;
            e->n_ranges = (u8)nr;
            memcpy(e->rloc, rloc, nr); memcpy(e->roff, roff, nr * 4u); memcpy(e->rlen, rlen, nr * 4u);
            VBST(4);
            return 0;
        }
        if (n_occ && !promote) s_vb_fallback++;
        dc.n_cuts = 0;                      /* the build pushes its own */
    }
    const int r = eng_geom_build(layout, prim, rebuild, o);
    s_gstage[3] += eng_rdtsc() - _gt;
    if (r) return r;
    u8 rloc[ENG_GEOM_RANGES]; u32 roff[ENG_GEOM_RANGES], rlen[ENG_GEOM_RANGES];
    const u32 nr = eng_geom_ranges(rloc, roff, rlen);
    const u32 ww_now = rsx_ww_on() ? rsx_ww_epoch() : 0u;
    s_geom_miss_bytes += (u64)o->n_upload * layout->stride;
    /* First sighting: remember the key only. Ring-buffered dynamic geometry
     * comes at a new address every frame and is never seen again, so hashing
     * it here was wasted; geometry that does come back is hashed on its
     * second sighting and stored on its third. */
    const u64 content = (e && nr) ? eng_geom_content(nr, rloc, roff, rlen) : 0;
    if (!nr || (e && !content)) { s_geom_miss_nocontent++; return 0; }
    if (!e) s_geom_miss_new++; else if (e->content != content) s_geom_miss_changed++; else s_geom_miss_promote++;

    if (!e) {                                   /* first sighting: remember it */
        e = &set[0];
        for (u32 w = 0; w < ENG_GEOM_WAYS; w++) {
            if (!set[w].state) { e = &set[w]; break; }
            if (set[w].frame < e->frame) e = &set[w];
        }
        if (e->state == 2) eng_geom_free(e);
        e->key = key; e->content = 0; e->icontent = 0; e->state = 1;   /* content: not hashed yet */
    } else if (e->content != content) {         /* the guest rewrote it (or: second sighting) */
        if (e->state == 2) eng_geom_free(e);
        e->content = content; e->icontent = s_geom_last_ih; e->state = 1;
    } else {                                    /* seen twice unchanged: keep it */
        e->icontent = s_geom_last_ih;
        const u32 vbytes = o->n_upload * layout->stride;
        const u64 need = (u64)vbytes + (o->indices ? (u64)o->n_draw * 4u : 0);
        /* Full: let go of what no draw has used for five seconds (at most
         * once a second). It used to stop storing anything new for the rest
         * of the session once the budget was reached. */
        if (s_geom_bytes + need > ENG_GEOM_BUDGET && s_geom_sweep_frame + 60u <= g.frames) {
            s_geom_sweep_frame = g.frames;
            for (u32 i = 0; i < ENG_GEOM_SETS * ENG_GEOM_WAYS; i++) {
                eng_geom_entry* x = &s_geom[i];
                if (x->state == 2 && x != e && x->frame + 300u < g.frames) { eng_geom_free(x); x->state = 0; s_geom_swept++; }
            }
        }
        if (s_geom_bytes + need <= ENG_GEOM_BUDGET && vbytes) {
            e->verts = (u8*)malloc(vbytes);
            e->indices = o->indices ? (u32*)malloc((size_t)o->n_draw * 4u) : NULL;
            if (e->verts && (!o->indices || e->indices)) {
                memcpy(e->verts, o->verts, vbytes);
                if (o->indices) memcpy(e->indices, o->indices, (size_t)o->n_draw * 4u);
                e->vbytes = vbytes; e->n_upload = o->n_upload; e->n_draw = o->n_draw;
                e->indexed = o->indices != NULL;
                e->held = (u32)need;
                s_geom_bytes += need;
                e->state = 2;
                /* Once on the GPU side too: every later draw of it copies
                 * nothing, and the CPU copy goes (the upload copies it out --
                 * the backend thread's record or the geometry heap). Kept, it
                 * was the steady private-memory climb of a long session:
                 * +15 MB a minute toward the 384 MB budget, all duplicates. */
                e->gpu = g.be->geom_upload
                    ? g.be->geom_upload(g.be->user, e->verts, vbytes, e->indices,
                                        e->indices ? e->n_draw * 4u : 0u) : 0u;
                if (e->gpu && g.be->draw_cached) {
                    free(e->verts); free(e->indices);
                    e->verts = NULL; e->indices = NULL;
                }
            } else { free(e->verts); free(e->indices); e->verts = NULL; e->indices = NULL; }
        }
    }
    e->frame = g.frames;
    e->ww_epoch = ww_now;
    e->n_ranges = (u8)nr;
    memcpy(e->rloc, rloc, nr); memcpy(e->roff, roff, nr * 4u); memcpy(e->rlen, rlen, nr * 4u);
    return 0;
}

/* ---- sink ---------------------------------------------------------------- */

static void sink_begin(void* user, const rsx_dispatch* r, u32 prim)
{
    (void)user; (void)r; (void)prim;
    dc_reset();
}

/* Room for one more range in *list (n used, *cap held); 0 = out of memory. */
static int eng_batch_reserve(eng_batch** list, u32 n, u32* cap)
{
    if (n < *cap) return 1;
    const u32 next = *cap ? *cap * 2u : 256u;
    eng_batch* grown = (eng_batch*)realloc(*list, (size_t)next * sizeof **list);
    if (!grown) {
        static int warned;
        if (!warned) { warned = 1; fprintf(stderr, "[rsx engine] out of memory for a draw's %u ranges: the rest are dropped%c", n, 10); }
        return 0;
    }
    *list = grown; *cap = next;
    return 1;
}

static void sink_draw_arrays(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (!eng_batch_reserve(&dc.arr, dc.n_arr, &dc.cap_arr)) return;
    dc.arr[dc.n_arr].first = first;
    dc.arr[dc.n_arr].count = count;
    dc.n_arr++;
}

/* INLINE_ARRAY delivers one finished stream rather than a (first, count)
 * range; queue it as an ordinary consecutive batch and let the fetch plan
 * read the vertices out of it (rsx_vertex_fetch_plan_set_inline). */
static void sink_inline_array(void* user, const rsx_dispatch* r,
                              const u8* data, u32 bytes)
{
    (void)user;
    const u32 stride = rsx_vertex_inline_layout(r, NULL);
    if (!stride || bytes < stride) return;
    dc.inl = data;
    dc.inl_bytes = bytes;
    dc.n_packets++;
    if (!eng_batch_reserve(&dc.arr, dc.n_arr, &dc.cap_arr)) return;
    dc.arr[dc.n_arr].first = 0;
    dc.arr[dc.n_arr].count = bytes / stride;
    dc.n_arr++;
}

static void sink_draw_index(void* user, const rsx_dispatch* r, u32 first, u32 count)
{
    (void)user; (void)r;
    dc.n_packets++;
    if (!eng_batch_reserve(&dc.idx, dc.n_idx, &dc.cap_idx)) return;
    dc.idx[dc.n_idx].first = first;
    dc.idx[dc.n_idx].count = count;
    dc.n_idx++;
}

/* Which texture units this draw binds, and from where: a unit naming a
 * registered colour surface samples that live target rather than guest
 * memory (every post-process, shadow and reflection in a PS3 title is a
 * render-to-texture read back through a texture unit), a unit naming a
 * tracked zeta in DEPTH24_D8 samples its snapshot, and everything else is a
 * guest upload (rsx_live_draw.c:5986-6062). */
/* RSX_ENG_TRACE_TARGETS=1: the frame's layout -- draws per colour target set,
 * in first-use order, with the depth buffer and antialias mode each used. */
static struct { u32 slot, n, zslot, aa, count; } s_tt[48];
static u32 s_tt_n = 0;
/* RSX_ENG_TRACE_PER_PIPE=<n>: at most n traced draws per pipeline key, so one
 * busy pipeline (heat-haze quads) does not use up RSX_ENG_TRACE_MAX. */
static int eng_trace_per_pipe_ok(u64 key)
{
    static long per = -2;
    if (per == -2) { const char* e = getenv("RSX_ENG_TRACE_PER_PIPE"); per = e ? atol(e) : -1; }
    if (per < 0) return 1;
    static struct { u64 k; long n; } seen[512]; static int ns;
    for (int i = 0; i < ns; i++) if (seen[i].k == key) return seen[i].n++ < per;
    if (ns < 512) { seen[ns].k = key; seen[ns].n = 1; ns++; return per > 0; }
    return 0;
}

/* RSX_ENG_STATS=1: where draws go. sink_end leaves silently on eight paths, and
 * a frame of thousands of draws that shows nothing looks the same whichever
 * one took them. Counted always (cheap), printed every 120 presents. */
/* RSX_ENG_CLEAR_LOG=1 with RSX_ENG_TRACE_FILE: the clears of the two frames
 * after each trace trigger (targets, depth buffer, values). */
static u32 s_clearlog_from = ~0u;
static u64 s_eng_drop[10];
#define ENG_DROP(i) (s_eng_drop[i]++)
static void eng_stage_report(void);
static void eng_stats_report(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RSX_ENG_STATS") ? 1 : 0;
    if (!on) return;
    static const char* name[10] = { "needs-expansion", "bad-topology", "no-verts", "fetch-failed",
                                    "topology-rebuild", "zero-count", "no-target", "no-pipeline", "submitted",
                                    "alpha-never" };
    fprintf(stderr, "[rsx engine] draws:");
    for (int i = 0; i < 10; i++) { fprintf(stderr, " %s=%llu", name[i], (unsigned long long)s_eng_drop[i]); s_eng_drop[i] = 0; }
    fprintf(stderr, "\n");
    eng_stage_report();
#ifdef PS3RECOMP_RSX_VULKAN
    { extern void rsx_vulkan_engine_stats_report(void); rsx_vulkan_engine_stats_report(); }
#endif
}

/* RSX_ENG_ZSTAT=<period>: one frame in <period>, the values of each depth
 * snapshot a draw samples (read back from the GPU): min / max / mean and how
 * much of it is still the far plane. A diagnostic for shadow compares. */
static u64 s_frame_serial;          /* g.surf_serial when the frame began (eng_present) */
static void eng_zstat(u32 slot, u32 snap, const char* why)
{
    static long per = -1; static u32 last_frame = ~0u, n_this = 0;
    if (per < 0) { const char* e = getenv("RSX_ENG_ZSTAT"); per = e ? atol(e) : 0; }
    if (per <= 0 || g.frames % (u32)per != 0 || !g.be->readback) return;
    static u32 done_off[32];
    if (last_frame != g.frames) { last_frame = g.frames; n_this = 0; }
    const eng_zdepth* z = &g.zdepths[slot];
    for (u32 i = 0; i < n_this; i++) if (done_off[i] == z->offset) return;   /* once per address a frame */
    if (n_this >= 32) return;
    done_off[n_this++] = z->offset;
    const u32 w = z->snap_w ? z->snap_w : (z->used_w ? z->used_w : z->w);
    const u32 h = z->snap_h ? z->snap_h : (z->used_h ? z->used_h : z->h);
    float* px = (float*)malloc((size_t)w * h * 4u);
    if (!px) return;
    memset(px, 0, (size_t)w * h * 4u);
    g.be->readback(g.be->user, snap, 0, 0, w, h, px, w * 4u);
    double mn = 1e30, mx = -1e30, sum = 0; u64 far_ = 0;
    for (u64 i = 0; i < (u64)w * h; i++) { const double v = px[i]; if (v < mn) mn = v; if (v > mx) mx = v; sum += v; if (v >= 0.999999) far_++; }
    fprintf(stderr, "[zstat] frame %u %s 0x%08X %ux%u: min %.6f max %.6f mean %.6f far %.1f%% centre %.6f | used %ux%u alloc %ux%u written %s%c",
            g.frames, why, z->offset, w, h, mn, mx, sum / ((double)w * h), 100.0 * (double)far_ / ((double)w * h),
            px[(size_t)(h / 2) * w + w / 2], z->used_w, z->used_h, z->w, z->h,
            z->last_write > s_frame_serial ? "this frame" : "earlier", 10);
    /* RSX_ENG_ZDUMP=<dir>: and as a PGM image (0 = near, 255 = far). */
    { const char* dir = getenv("RSX_ENG_ZDUMP");
      if (dir && *dir) {
          char path[512]; snprintf(path, sizeof path, "%s/z_%08X_%u_%ux%u.pgm", dir, z->offset, g.frames, w, h);
          FILE* f = fopen(path, "wb");
          if (f) { fprintf(f, "P5 %u %u 255%c", w, h, 10);
                   for (u64 i = 0; i < (u64)w * h; i++) { float v = px[i] < 0 ? 0 : px[i] > 1 ? 1 : px[i]; fputc((int)(v * 255.0f + 0.5f), f); }
                   fclose(f); }
          snprintf(path, sizeof path, "%s/z_%08X_%u_%ux%u.f32", dir, z->offset, g.frames, w, h);
          f = fopen(path, "wb");
          if (f) { fwrite(px, 4, (size_t)w * h, f); fclose(f); } } }
    free(px);
}


/* RSX_ENG_TRACE_PERIOD=<n>: the RSX_ENG_TRACE_* traces only in frames that
 * are multiples of n (lines up with RSX_ENG_PROBE's period). */
static int eng_trace_period_ok(void)
{
    static long per = -1;
    if (per < 0) { const char* e = getenv("RSX_ENG_TRACE_PERIOD"); per = e ? atol(e) : 0; }
    return per <= 0 || g.frames % (u32)per == 0;
}
/* RSX_ENG_TRACE_W=<width>: the trace also follows every target that wide. */
static int eng_trace_width_ok(u32 w)
{
    static long tw = -1;
    if (tw < 0) { const char* e = getenv("RSX_ENG_TRACE_W"); tw = e ? atol(e) : 0; }
    return tw > 0 && w == (u32)tw;
}
static const char* s_tex_src[RSX_BE_MAX_TEXTURES];   /* what each unit was bound from (RSX_ENG_TRACE_*) */
static u64 s_tsub[4], s_tsub_n;   /* texture stage: decode, memo, (rest), vertex units; non-memo units */
typedef struct { rsx_dsp_texture t; u32 handle; rsx_be_sampler_desc samp; u32 frame, epoch; int valid;
                 int rt; u32 slot; u32 gen; int enabled; } eng_unit_memo;
static u32 s_tex_src_slot[RSX_BE_MAX_TEXTURES];   /* the colour target a unit sampled (trace) */
/* Units that sample a colour target whose last draw packed it as sRGB. */
static u32 s_tex_srgb_src;
/* This draw's targets that it also samples, through a copy (sink_bind_textures):
 * its own write leaves that copy current (sink_end). */
static u32 s_own_read[RSX_BE_MAX_COLOR_TARGETS], s_n_own_read;
/* RSX_ENG_SNAP_REUSE=0: a fresh copy for every draw that reads its own target. */
static int eng_snap_reuse(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("RSX_ENG_SNAP_REUSE"); on = !(e && *e == '0'); }
    return on;
}
/* A target or depth buffer a draw SAMPLES is in use as much as one it draws
 * into. Only binds counted before, so a target drawn once and read every
 * frame since (an environment map, a cached UI layer) looked idle: once the
 * game had streamed in enough new targets to fill the list -- a few minutes
 * of play in A Crack in Time -- it was the one recycled, and every read of it
 * from then on decoded raw VRAM instead (stripes, noise, neon). */
static inline void eng_surf_touch(u32 i) { if (i < g.n_surfaces) g.surfaces[i].last_use = ++g.surf_serial; }
/* The game has written this depth buffer's memory since our last depth
 * write or clear into it: on the PS3 that memory now holds the game's bytes
 * (VRAM a shadow map used, reused for streamed level textures), so neither a
 * colour read (depth as colour) nor a depth read may take the stale GPU copy.
 * Only pages wholly inside the buffer count -- a neighbour's writes to a
 * shared edge page must not hide a live shadow map. Once per frame. */
static int eng_zdepth_overwritten(u32 i)
{
    if (i >= g.n_zdepths) return 0;
    eng_zdepth* z = &g.zdepths[i];
    if (!rsx_ww_on() || !z->handle || !z->last_write || !z->ww_epoch) return 0;
    if (z->ww_check_frame == g.frames + 1u) return z->ww_stale;
    z->ww_check_frame = g.frames + 1u;
    const u32 span = z->w * z->h * 4u;
    const u8* p = eng_guest_ptr(NULL, z->location, z->offset, span);
    u8 st = 0;
    if (p) {
        const uintptr_t a = ((uintptr_t)p + 4095u) & ~(uintptr_t)4095u;
        const uintptr_t e = ((uintptr_t)p + span) & ~(uintptr_t)4095u;
        if (e > a && rsx_ww_watched((const void*)a, (unsigned)(e - a)) &&
            rsx_ww_guest_written_since((const void*)a, (unsigned)(e - a), z->ww_epoch)) st = 1;
    }
    if (st && !z->ww_stale) { static unsigned long long n; ++n;
        if (n <= 24 || (n & (n - 1)) == 0)
            fprintf(stderr, "[rsx engine] depth %u:0x%08X %ux%u overwritten by the game since its last write: reads decode memory (%llu so far)%c",
                    z->location, z->offset, z->w, z->h, n, 10); }
    z->ww_stale = st;
    return st;
}

/* A compressed (DXT) texture read resolved to a render target or a depth
 * buffer: no draw produces DXT, so this is a level texture shadowed by a
 * stale GPU object -- the neon/striped signature. Logged per address. */
static void eng_tex_suspect(const rsx_dsp_texture* t, const char* src, u32 w, u32 h)
{
    const u32 f = t->format & 0x9Fu;
    if (f != 0x86u && f != 0x87u && f != 0x88u) return;
    static u32 seen[64]; static u32 ns; static unsigned long long n;
    ++n;
    for (u32 i = 0; i < ns; i++) if (seen[i] == t->offset) return;
    if (ns < 64u) seen[ns++] = t->offset; else return;
    fprintf(stderr, "[tex-suspect] frame %u: DXT texture %u:0x%08X %ux%u fmt 0x%02X bound from a %s (%ux%u) -- %llu such reads%c",
            g.frames, t->location, t->offset, t->width, t->height, t->format, src, w, h, n, 10);
}

static inline void eng_z_touch(u32 i) { if (i < g.n_zdepths) g.zdepths[i].last_use = ++g.surf_serial; }
static eng_unit_memo s_unit_memo[RSX_BE_MAX_TEXTURES];
/* A colour target bigger than the texture a unit declares at its address:
 * the texture is its top-left width x height, which is what the RSX reads
 * (rows at the target's pitch). The fraction goes to fp_texp[u].xy. A Crack
 * in Time copies one screen patch per heat-haze particle into a copy target
 * sized for the largest patch so far; sampled whole, each particle showed a
 * stretched block of the scene with the unwritten rows below it in black. */
static float s_tex_sub[RSX_BE_MAX_TEXTURES][2];
/* A colour target read through Y16_X16 (0x95): the RSX takes each 32-bit
 * texel's bytes as two 16-bit numbers -- A:R and G:B -- where a sampled RGBA8
 * view gives four 8-bit ones. A Crack in Time copies its scene depth (read as
 * colour) into a 512x352 ARGB8 target and its water reads it back this way
 * for the depth under the surface; with four bytes in place of two halves
 * every pixel came out "deep": flat opaque teal. The unit is bound with an
 * identity view and fp_texp[u].z carries the remap for the shader to apply
 * after rebuilding the halves (rsx_texmode): 16 | sel A,R,G,B << 5,8,11,14,
 * each sel 0..3 = component of (Y, X, 0, 1), 4 = zero, 5 = one. */
static u32 s_tex_reint[RSX_BE_MAX_TEXTURES];
static inline u32 eng_tex_reint_code(const rsx_dsp_texture* t, u32 slot)
{
    if ((t->format & 0x9Fu) != 0x95u || slot >= g.n_surfaces || g.surfaces[slot].fmt != RSX_BE_FMT_R8G8B8A8) return 0;
    u8 sel[4];
    rsx_texture_component_remap(t->remap & 0xFFFFu, 0x95u, sel);
    return 16u | ((u32)sel[0] << 5) | ((u32)sel[1] << 8) | ((u32)sel[2] << 11) | ((u32)sel[3] << 14);
}
/* A copy target read as a smaller texture with clamped addressing (CLAMP,
 * CLAMP_TO_EDGE, MIRROR_ONCE*): the RSX clamps at the texture's own edge, the
 * sampler here at the target's, past texels no blit filled. A Crack in Time's
 * Dynamo of Doom copies the scene into 0x0205F100 (464x352 of a 512x512
 * target) and bends it through a lens; the bent reads past the right edge came
 * out a black band. The target's texels past the texture become copies of its
 * last column and row, once per texture size and target write. Only copy
 * targets: what lies past the texture there is no image the game drew. */
static void eng_copy_target_edges(const rsx_dsp_texture* t, u32 slot)
{
    if (slot >= g.n_surfaces || !g.be->blit) return;
    eng_surface* s = &g.surfaces[slot];
    const u32 w = t->width, h = t->height;
    if (s->rsx_fmt || !s->copy_h || !s->handle || !w || !h || h > s->copy_h || w > s->w || h > s->h ||
        (w == s->w && h == s->h)) return;
    if (s->edge_w == w && s->edge_h == h && s->edge_serial == s->last_write) return;
    const u32 ws = t->wrap & 0xFu, wt = (t->wrap >> 8) & 0xFu;
    const int cs = ws == 3u || ws == 5u || ws == 6u || ws == 8u, ct = wt == 3u || wt == 5u || wt == 6u || wt == 8u;
    if (cs && w < s->w) g.be->blit(g.be->user, s->handle, w - 1u, 0, 1, h, s->handle, w, 0, s->w - w, h);
    if (ct && h < s->h) g.be->blit(g.be->user, s->handle, 0, h - 1u, w, 1, s->handle, 0, h, w, s->h - h);
    if (cs && ct && w < s->w && h < s->h)
        g.be->blit(g.be->user, s->handle, w - 1u, h - 1u, 1, 1, s->handle, w, h, s->w - w, s->h - h);
    s->edge_w = w; s->edge_h = h; s->edge_serial = s->last_write;
}

static inline void eng_tex_sub_set(u32 u, const rsx_dsp_texture* t, u32 slot)
{
    eng_copy_target_edges(t, slot);
    float x = 1.0f, y = 1.0f;
    if (slot < g.n_surfaces) {
        const eng_surface* s = &g.surfaces[slot];
        if (t->width && s->w > t->width) x = (float)t->width / (float)s->w;
        if (t->height && s->h > t->height) y = (float)t->height / (float)s->h;
    }
    s_tex_sub[u][0] = x; s_tex_sub[u][1] = y;
    s_tex_reint[u] = eng_tex_reint_code(t, slot);
}
/* The units the draw's fragment program samples; every unit for the built-in
 * program. A title leaves units of earlier passes enabled -- its display
 * buffer, a depth buffer -- and resolving each of those for every draw (a
 * colour target with a depth buffer at its address, a depth buffer read as
 * colour) was most of the texture stage: Axiom, ~3 such units a draw. A unit
 * the program never samples cannot change the picture. */
static u32 eng_fp_tex_mask(void)
{
    const u8* vp = NULL; const u8* fp = NULL; u32 vi = 0, fs = 0;
    if (!eng_guest_programs(&vp, &vi, &fp, &fs)) return 0xFFFFu;
    static u32 c_serial = ~0u, c_mask; static const u8* c_ptr;
    if (c_serial != s_fpc.serial || c_ptr != fp || !s_fpc.valid) {
        c_mask = rsx_fp_texture_mask(fp, fs);
        c_serial = s_fpc.serial; c_ptr = fp;
    }
    return c_mask;
}

static u32 sink_bind_textures(const u32* target_slots, u32 n_targets,
                              u32 current_zslot, u32 fp_mask,
                              u32 textures[RSX_BE_MAX_TEXTURES],
                              rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES])
{
    u32 mask = 0;
    s_tex_srgb_src = 0;
    s_n_own_read = 0;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        s_tex_sub[u][0] = s_tex_sub[u][1] = 1.0f;
        s_tex_reint[u] = 0;
        if (!((fp_mask >> u) & 1u)) continue;
        rsx_dsp_texture t;
        u64 _tt = eng_rdtsc();
        s_tex_src[u] = "";
        /* Registers unchanged since the unit was resolved (tex_gen), and
         * nothing it depended on changed: the same answer, without decoding. */
        { eng_unit_memo* mm = &s_unit_memo[u];
          int own = 0;
          if (mm->valid && mm->rt)
              for (u32 k = 0; k < n_targets; k++) if (target_slots[k] == mm->slot) own = 1;
          if (mm->valid && mm->gen == g.rsx.tex_gen[u] && !own && mm->epoch == g.bind_epoch &&
              (mm->rt || mm->frame == g.frames) && !(mm->rt && eng_surface_overwritten(mm->slot))) {
              s_tsub[1] += eng_rdtsc() - _tt;
              if (!mm->enabled) continue;
              mask |= 1u << u;
              textures[u] = mm->handle; samplers[u] = mm->samp; s_memo_hits++;
              s_tex_src[u] = mm->rt ? "colour target" : "texture cache";
              s_tex_src_slot[u] = mm->slot;
              if (mm->rt) { eng_surf_touch(mm->slot); eng_tex_sub_set(u, &mm->t, mm->slot);
                            if (g.surfaces[mm->slot].srgb_written) s_tex_srgb_src |= 1u << u; }
              continue;
          } }
        rsx_dsp_get_texture(&g.rsx, u, &t);
        { const u64 _n = eng_rdtsc(); s_tsub[0] += _n - _tt; _tt = _n; }
        if (!t.enabled) {
            eng_unit_memo* mm = &s_unit_memo[u];
            mm->valid = 1; mm->enabled = 0; mm->rt = 1; mm->slot = ENG_INVALID;
            mm->gen = g.rsx.tex_gen[u]; mm->epoch = g.bind_epoch;
            mm->t = t; mm->handle = 0;                  /* nothing of an earlier answer survives */
            continue;
        }
        mask |= 1u << u;
        /* The unit resolved to the texture cache last time, with the same
         * registers, this frame, and nothing that could redirect it since:
         * the same texture and sampler. Most draws rebind what the one
         * before bound; resolving every unit again was ~a third of a draw. */
        { eng_unit_memo* mm = &s_unit_memo[u];
          int own = 0;
          if (mm->valid && mm->rt)
              for (u32 k = 0; k < n_targets; k++) if (target_slots[k] == mm->slot) own = 1;
          if (mm->valid && mm->enabled && !own && (mm->rt || mm->frame == g.frames) && mm->epoch == g.bind_epoch &&
              !(mm->rt && eng_surface_overwritten(mm->slot)) && !memcmp(&mm->t, &t, sizeof t)) {
              textures[u] = mm->handle; samplers[u] = mm->samp; s_memo_hits++;
              s_tex_src[u] = mm->rt ? "colour target" : "texture cache";
              s_tex_src_slot[u] = mm->slot;
              if (mm->rt) { eng_surf_touch(mm->slot); eng_tex_sub_set(u, &mm->t, mm->slot);
                            if (g.surfaces[mm->slot].srgb_written) s_tex_srgb_src |= 1u << u; }
              mm->gen = g.rsx.tex_gen[u];
              s_tsub[1] += eng_rdtsc() - _tt;
              continue;
          }
          mm->valid = 0; }
        s_tsub[1] += eng_rdtsc() - _tt; s_tsub_n++;
        eng_decode_sampler(t.filter, t.wrap, t.control0, &samplers[u]);
        samplers[u].border = t.border_color;

        /* A colour read of a depth buffer whose colour twin nothing has drawn
         * into: its raw D24S8 bytes (eng_zdepth_as_color). */
        if (!eng_is_depth_fmt(t.format)) {
            u32 zi = ENG_INVALID;
            for (u32 i = eng_z_first(t.location, t.offset); i != ENG_INVALID; i = eng_z_next(i))
                if (g.zdepths[i].handle && g.zdepths[i].had_write && !eng_zdepth_overwritten(i)) { zi = i; break; }
            if (zi != ENG_INVALID) {
                int twin_drawn = 0;
                for (u32 i = eng_surf_first(t.location, t.offset); i != ENG_INVALID; i = eng_surf_next(i))
                    if (g.surfaces[i].color_written) { twin_drawn = 1; break; }
                if (!twin_drawn) {
                    const u32 ac = eng_zdepth_as_color(zi);
                    if (ac) {
                        const u32 view = g.be->surface_view
                            ? g.be->surface_view(g.be->user, ac, t.remap & 0xFFFFu, t.format) : 0;
                        textures[u] = view ? view : ac;
                        s_tex_src[u] = "depth as colour"; if (g.zdepths[zi].snapshot) eng_zstat(zi, g.zdepths[zi].snapshot, "as-colour");
                        eng_tex_suspect(&t, "depth buffer", g.zdepths[zi].w, g.zdepths[zi].h);
                        eng_z_touch(zi);
                        continue;
                    }
                }
            }
        }

        /* A depth-format read wants whichever was written last at that
         * address: the depth buffer, unless a colour draw came after it. A
         * Crack in Time's shadow passes bind a colour target at each shadow
         * map's own address with colour writes off, and its bloom later
         * reuses the start of the shadow pool as a colour target; picking the
         * colour target there handed the shadow compare a never-written image
         * (depth 0) -- Ratchet's whole shadow volume came out shadowed. */
        if (eng_is_depth_fmt(t.format)) {
            u64 colour_last = 0;
            for (u32 i = eng_surf_first(t.location, t.offset); i != ENG_INVALID; i = eng_surf_next(i))
                if (g.surfaces[i].color_written && g.surfaces[i].last_write > colour_last)
                    colour_last = g.surfaces[i].last_write;
            for (u32 i = eng_z_first(t.location, t.offset); i != ENG_INVALID; i = eng_z_next(i))
                if (current_zslot != i && g.zdepths[i].had_write && g.zdepths[i].last_write > colour_last &&
                    !eng_zdepth_overwritten(i)) {
                    const u32 snap = eng_zdepth_snapshot_tex(i, t.width, t.height);
                    if (snap) { textures[u] = snap; s_tex_src[u] = "depth snapshot (recent)"; eng_zstat(i, snap, "compare"); eng_z_touch(i); }
                    break;
                }
            if (textures[u]) continue;
        }

        int sampled = -1;
        /* A compressed (DXT) texture is never a render target -- no draw
         * produces DXT -- so it decodes memory even when a target sits at its
         * address, as the RSX would. The install movie's subtitle bar (Qwark,
         * gc1_nstal_fmv) is a quad textured with a 16x16 DXT1 at local 0x0,
         * which is display buffer 0: sampled as that target, the bar showed
         * the picture on every other flip (the "black flicker at the bottom";
         * owner, 0.8). Decoded, it is black, as on the PS3. */
        const u32 tfmt = t.format & 0x9Fu;
        const int dxt = tfmt == 0x86u || tfmt == 0x87u || tfmt == 0x88u;
        if (dxt && eng_surface_find(t.location, t.offset, t.width, t.height) != ENG_INVALID)
            eng_tex_suspect(&t, "colour target (decoded from memory instead)", t.width, t.height);
        for (u32 i = dxt ? ENG_INVALID : eng_surface_find(t.location, t.offset, t.width, t.height); i != ENG_INVALID; ) {
            /* Not one this draw writes: reading a target a pass is writing
             * is undefined on every API, and an MRT set writes more than
             * one of them. */
            int own = 0;
            for (u32 k = 0; k < n_targets; k++) if (target_slots[k] == i) own = 1;
            if (own) {
                /* Sample a copy taken now: what the target held before this
                 * draw, which is what an in-place pass (a multisample resolve
                 * into the buffer it reads) needs. Without a copy the texture
                 * cache read guest memory, which the GPU never wrote: black. */
                /* Taken again only when something other than a draw reading
                 * the copy has written the target since. A run of distortion
                 * draws (the Rift Inducer's vortex, the time bomb's sphere)
                 * copied the whole scaled target for every draw: a full-screen
                 * copy and a full GPU barrier each. Those draws now share one
                 * copy, as the RSX's texture cache would give them; anything
                 * else drawn, cleared or blitted into the target takes a new one. */
                eng_surface* sf = &g.surfaces[i];
                u32 snap = 0;
                if (eng_snap_reuse() && sf->own_snap && sf->own_snap_of == sf->handle && sf->own_snap_at == sf->last_write)
                    snap = sf->own_snap;
                else if (g.be->color_snapshot) {
                    snap = g.be->color_snapshot(g.be->user, sf->handle, sf->w, sf->h);
                    sf->own_snap = snap; sf->own_snap_of = sf->handle; sf->own_snap_at = sf->last_write;
                }
                if (snap) {
                    textures[u] = snap; sampled = -2; s_tex_src[u] = "own target copy"; eng_surf_touch(i);
                    if (s_n_own_read < RSX_BE_MAX_COLOR_TARGETS) s_own_read[s_n_own_read++] = i;
                }
                break;
            }
            /* A target the GPU never wrote (created by a pass with colour
             * writes off -- the shadow passes bind one at each shadow map's
             * address -- and never cleared) holds only the guest memory it was
             * seeded with: guest memory is the truth, and once the game streams
             * level textures into that memory the target is a stale RGBA copy
             * of old bytes -- the neon/striped textures after menus, vendors
             * and long play. Use one at that address the GPU did write, or
             * decode memory. */
            if (!g.surfaces[i].last_write) {
                u32 alt = ENG_INVALID;
                for (u32 j = eng_surf_first(t.location, t.offset); j != ENG_INVALID; j = eng_surf_next(j))
                    if (g.surfaces[j].last_write && (alt == ENG_INVALID || g.surfaces[j].last_write > g.surfaces[alt].last_write))
                        alt = j;
                { static unsigned long long n_unw; ++n_unw;
                  if (n_unw <= 24 || (n_unw & (n_unw - 1)) == 0)
                      fprintf(stderr, "[rsx engine] texture %u:0x%08X %ux%u fmt 0x%02X: the target there (%ux%u) was never drawn or cleared -> %s (%llu so far)%c",
                              t.location, t.offset, t.width, t.height, t.format, g.surfaces[i].w, g.surfaces[i].h,
                              alt != ENG_INVALID ? "another target there" : "decoding memory", n_unw, 10); }
                if (alt == ENG_INVALID) { sampled = -1; break; }
                i = alt;
                own = 0;
                for (u32 k = 0; k < n_targets; k++) if (target_slots[k] == i) own = 1;
                if (own) { sampled = -1; break; }
            }
            sampled = eng_surface_overwritten(i) ? -1 : (int)i;
            if (sampled < 0) { static unsigned long long n_ow; ++n_ow;
                if (n_ow <= 24 || (n_ow & (n_ow - 1)) == 0)
                    fprintf(stderr, "[rsx engine] texture %u:0x%08X %ux%u fmt 0x%02X: target there overwritten by the game -> decoding memory (%llu so far)%c",
                            t.location, t.offset, t.width, t.height, t.format, n_ow, 10); }
            break;
        }
        if (sampled == -2) continue;           /* own target: snapshot bound */
        if (sampled >= 0 && eng_is_depth_fmt(t.format)) {
            static long per = -1; if (per < 0) { const char* e = getenv("RSX_ENG_ZSTAT"); per = e ? atol(e) : 0; }
            if (per > 0 && g.frames % (u32)per == 0)
            { fprintf(stderr, "[zstat] frame %u depth-format read of 0x%08X %ux%u bound COLOUR target %ux%u (written %d);",
                        g.frames, t.offset, t.width, t.height, g.surfaces[sampled].w, g.surfaces[sampled].h,
                        g.surfaces[sampled].color_written);
              for (u32 i = eng_z_first(t.location, t.offset); i != ENG_INVALID; i = eng_z_next(i))
                  fprintf(stderr, " zdepth %u %ux%u used %ux%u had_write %d current %d handle %u", i, g.zdepths[i].w, g.zdepths[i].h,
                          g.zdepths[i].used_w, g.zdepths[i].used_h, g.zdepths[i].had_write, i == current_zslot, g.zdepths[i].handle);
              fprintf(stderr, "%c", 10); }
        }
        if (sampled >= 0) {
            /* Each pairing of a target's format with the texture format it is
             * read through, once: a format the view cannot express (two
             * 16-bit halves, a float in four bytes) is a picture bug. */
            { static u32 seen[64]; static int ns;
              const u32 k = ((u32)g.surfaces[sampled].fmt << 24) | ((t.format & 0x9Fu) << 16) | (t.remap & 0xFFFFu);
              int found = 0; for (int i = 0; i < ns; i++) if (seen[i] == k) { found = 1; break; }
              if (!found && ns < 64) { seen[ns++] = k;
                  fprintf(stderr, "[rsx engine] target 0x%08X (%ux%u, be fmt %u) read as texture fmt 0x%02X remap %04X %ux%u%c",
                          g.surfaces[sampled].offset, g.surfaces[sampled].w, g.surfaces[sampled].h, (unsigned)g.surfaces[sampled].fmt,
                          t.format & 0x9Fu, t.remap & 0xFFFFu, t.width, t.height, 10); } }
            /* A reinterpreting read (eng_tex_reint_code) wants the raw bytes. */
            eng_tex_suspect(&t, "colour target", g.surfaces[sampled].w, g.surfaces[sampled].h);
            const int reint = eng_dyn_tex() && eng_tex_reint_code(&t, (u32)sampled) != 0;
            const u32 view = g.be->surface_view
                ? g.be->surface_view(g.be->user, g.surfaces[sampled].handle,
                                     reint ? 0xAAE4u : (t.remap & 0xFFFFu), reint ? 0x85u : t.format)
                : 0;
            textures[u] = view ? view : g.surfaces[sampled].handle;
            s_tex_src[u] = "colour target"; s_tex_src_slot[u] = (u32)sampled;
            if (g.surfaces[sampled].srgb_written) s_tex_srgb_src |= 1u << u;
            eng_surf_touch((u32)sampled);
            eng_tex_sub_set(u, &t, (u32)sampled);
            /* The only target at that address, and no depth buffer there:
             * the same view until a target or depth buffer comes or goes
             * (bind_epoch), whatever frame it is. A title leaves spare units
             * pointing at its display buffer, so most draws resolved several
             * of these each time. */
            if (!eng_is_depth_fmt(t.format) && eng_z_first(t.location, t.offset) == ENG_INVALID &&
                eng_surf_next((u32)sampled) == ENG_INVALID &&
                eng_surf_first(t.location, t.offset) == (u32)sampled) {
                eng_unit_memo* mm = &s_unit_memo[u];
                mm->t = t; mm->handle = textures[u]; mm->samp = samplers[u];
                mm->frame = g.frames; mm->epoch = g.bind_epoch; mm->valid = 1;
                mm->rt = 1; mm->slot = (u32)sampled;
                mm->gen = g.rsx.tex_gen[u]; mm->enabled = 1;
            }
            continue;
        }
        if (eng_is_depth_fmt(t.format)) {
            for (u32 i = eng_z_first(t.location, t.offset); i != ENG_INVALID; i = eng_z_next(i))
                if (current_zslot != i) {
                    const u32 snap = eng_zdepth_snapshot_tex(i, t.width, t.height);
                    if (snap) { textures[u] = snap; s_tex_src[u] = "depth snapshot"; eng_z_touch(i); }
                    break;
                }
            if (textures[u]) continue;
        }
        /* RSX_ENG_DEPTH_TRACE=<period>: one frame in <period>, every depth-
         * format read that found no depth buffer and falls to guest memory. */
        { static long per = -1; if (per < 0) { const char* e = getenv("RSX_ENG_DEPTH_TRACE"); per = e ? atol(e) : 0; }
          if (per > 0 && eng_is_depth_fmt(t.format) && g.frames % (u32)per == 0) {
              u32 nz = 0, wr = 0; for (u32 i = eng_z_first(t.location, t.offset); i != ENG_INVALID; i = eng_z_next(i)) { nz++; wr |= (u32)g.zdepths[i].had_write; }
              fprintf(stderr, "[depth-read] frame %u unit %u %u:0x%08X %ux%u fmt 0x%X zfunc %u -> texture cache (zdepths here %u, written %u, current z %u)%c",
                      g.frames, u, t.location, t.offset, t.width, t.height, t.format, (t.wrap >> 28) & 7u, nz, wr,
                      current_zslot != ENG_INVALID ? g.zdepths[current_zslot].offset : 0u, 10); } }
        /* Always, rarely: a shadow compare that found no depth buffer drawn
         * reads memory the GPU never wrote -- a whole light volume dark. */
        if (eng_is_depth_fmt(t.format)) { static unsigned long long n; ++n;
            if (n <= 16 || (n & (n - 1)) == 0) {
                u32 nz = 0, wr = 0; for (u32 i = eng_z_first(t.location, t.offset); i != ENG_INVALID; i = eng_z_next(i)) { nz++; wr |= (u32)g.zdepths[i].had_write; }
                fprintf(stderr, "[rsx engine] depth read %u:0x%08X %ux%u found no depth buffer drawn there (%u there, written %u) -> memory, frame %u (%llu so far)%c",
                        t.location, t.offset, t.width, t.height, nz, wr, g.frames, n, 10); } }
        s_tex_src[u] = "texture cache";
        { const u64 t0 = eng_rdtsc();
          const u32 epoch0 = g.bind_epoch;
        textures[u] = eng_texture_slot(t.location, t.offset, t.format,
                                       t.width, t.height, t.mipmaps, t.pitch,
                                       (int)t.cubemap, t.remap & 0xFFFFu);
          s_tex_slot_cyc += eng_rdtsc() - t0;
          if (textures[u] && g.bind_epoch == epoch0) {
              eng_unit_memo* mm = &s_unit_memo[u];
              mm->t = t; mm->handle = textures[u]; mm->samp = samplers[u];
              mm->frame = g.frames; mm->epoch = g.bind_epoch; mm->valid = 1;
              mm->rt = 0; mm->slot = ENG_INVALID;
              mm->gen = g.rsx.tex_gen[u]; mm->enabled = 1;
          } }
    }
    return mask;
}

static u32 sink_bind_vertex_textures(
    u32 vtex_mask, u32 textures[RSX_BE_MAX_VERTEX_TEXTURES],
    rsx_be_sampler_desc samplers[RSX_BE_MAX_VERTEX_TEXTURES])
{
    u32 bound = 0;
    for (u32 u = 0; u < RSX_BE_MAX_VERTEX_TEXTURES; u++) {
        if (!((vtex_mask >> u) & 1u)) continue;
        rsx_dsp_vertex_texture vt;
        rsx_dsp_get_vertex_texture(&g.rsx, u, &vt);
        eng_decode_sampler(vt.filter, vt.wrap, vt.control0, &samplers[u]);
        /* A vertex unit has no crossbar, so its remap is the identity. */
        textures[u] = eng_texture_slot(vt.location, vt.offset, vt.format,
                                       vt.width, vt.height, vt.mipmaps,
                                       vt.pitch, 0, 0xAAE4u);
        if (textures[u]) bound |= 1u << u;
    }
    return bound;
}

/* RSX_ENG_STATS=1 also prints where sink_end spends its time: cycles per
 * stage, summed over the draws of 120 presents. */
static u64 s_stage[6];   /* fetch, render-state+targets, textures, pipeline, constants+state, backend */
static u64 s_cstage[4];  /* inside "consts": VS constants, trace checks, PS block, binds */
static u64 s_bsub[5];    /* inside binds: targets+pipeline, VS block, PS block, textures, viewport..bias */
#define BSUB(i) do { const u64 _n = eng_tsc(); s_bsub[i] += _n - _bt; _bt = _n; } while (0)
#define CSTAGE(i) do { const u64 _n = eng_tsc(); s_cstage[i] += _n - _ct; _ct = _n; } while (0)
static u64 s_const_dirty_draws, s_const_dirty_slots;   /* draws that saw new constants, slots copied */
static u64 s_sink_calls;
static inline u64 eng_tsc(void) { return eng_rdtsc(); }
#define STAGE(i) do { const u64 _n = eng_tsc(); s_stage[i] += _n - _st; _st = _n; } while (0)
static void eng_stage_report(void)
{
    static const char* nm[6] = { "fetch", "state", "textures", "pipeline", "consts", "backend" };
    u64 tot = 0; for (int i = 0; i < 6; i++) tot += s_stage[i];
    if (!tot || !s_sink_calls) return;
    fprintf(stderr, "[rsx engine] sink_end %llu calls, %.0f kcycles/draw:", (unsigned long long)s_sink_calls,
            (double)tot / (double)s_sink_calls / 1000.0);
    { static u64 last_tsc; const u64 now = eng_cycles();   /* the share of the window inside sink_end */
      if (last_tsc && now > last_tsc) fprintf(stderr, " [%.0f%% of the thread]", 100.0 * (double)tot / (double)(now - last_tsc));
      last_tsc = now; }
    for (int i = 0; i < 6; i++) { fprintf(stderr, " %s %.0f%%", nm[i], 100.0 * (double)s_stage[i] / (double)tot); s_stage[i] = 0; }
    { static const char* pn[5] = { "programs", "masks", "constants", "key", "lookup" };
      fprintf(stderr, " | pipeline:");
      for (int i = 0; i < 5; i++) { fprintf(stderr, " %s %.0f%%", pn[i], 100.0 * (double)s_pstage[i] / (double)tot); s_pstage[i] = 0; } }
    { static const char* cn[4] = { "vs", "trace", "ps", "binds" };
      fprintf(stderr, " | consts:");
      for (int i = 0; i < 4; i++) { fprintf(stderr, " %s %.0f%%", cn[i], 100.0 * (double)s_cstage[i] / (double)tot); s_cstage[i] = 0; } }
    { static const char* bn[5] = { "tgt+pipe", "vsblk", "psblk", "tex", "vp..bias" };
      fprintf(stderr, " | binds:");
      for (int i = 0; i < 5; i++) { fprintf(stderr, " %s %.0f%%", bn[i], 100.0 * (double)s_bsub[i] / (double)tot); s_bsub[i] = 0; } }
    { static const char* gn[4] = { "layout", "key", "check", "build" };
      fprintf(stderr, " | fetch:");
      for (int i = 0; i < 4; i++) { fprintf(stderr, " %s %.0f%%", gn[i], 100.0 * (double)s_gstage[i] / (double)tot); s_gstage[i] = 0; } }
    fprintf(stderr, "%c", 10);
    fprintf(stderr, "[rsx engine] geometry cache: %llu hits, %llu misses, %llu uncacheable, %.1f MB held, %llu let go%c",
            (unsigned long long)s_geom_hits, (unsigned long long)s_geom_misses,
            (unsigned long long)s_geom_uncached, (double)s_geom_bytes / 1048576.0,
            (unsigned long long)s_geom_swept, 10);
    fprintf(stderr, "[rsx engine] geometry misses: new %llu, changed %llu, promoted %llu, no-content %llu; %.1f MB converted%c",
            (unsigned long long)s_geom_miss_new, (unsigned long long)s_geom_miss_changed,
            (unsigned long long)s_geom_miss_promote, (unsigned long long)s_geom_miss_nocontent,
            (double)s_geom_miss_bytes / 1048576.0, 10);
    fprintf(stderr, "[rsx engine] vertex blocks: %llu hits, %llu builds (%llu grown, %llu rewritten), %llu fell back; %.1f MB converted, %.1f MB held%c",
            (unsigned long long)s_vb_hits, (unsigned long long)s_vb_builds, (unsigned long long)s_vb_grows,
            (unsigned long long)s_vb_dirty, (unsigned long long)s_vb_fallback,
            (double)s_vb_conv_bytes / 1048576.0, (double)s_vb_held / 1048576.0, 10);
    { const double nd = s_vb_hits + s_vb_builds ? (double)(s_vb_hits + s_vb_builds) : 1.0;
      fprintf(stderr, "[rsx engine] vertex block kcycles per draw: eligible %.2f scan %.2f ranges %.2f block %.2f sightings %.2f%c",
              s_vbst[0] / nd / 1e3, s_vbst[1] / nd / 1e3, s_vbst[2] / nd / 1e3, s_vbst[3] / nd / 1e3, s_vbst[4] / nd / 1e3, 10);
      memset(s_vbst, 0, sizeof s_vbst); }
    s_vb_hits = s_vb_builds = s_vb_grows = s_vb_dirty = s_vb_fallback = s_vb_conv_bytes = 0;
    s_geom_miss_new = s_geom_miss_changed = s_geom_miss_promote = s_geom_miss_nocontent = s_geom_miss_bytes = 0;
    s_geom_hits = s_geom_misses = s_geom_uncached = 0;
    fprintf(stderr, "[rsx engine] textures: %llu lookups (%.0f probes each, %u cached), %llu hashes %.1f MB %.1f Mcyc, %llu uploads %.1f Mcyc%c",
            (unsigned long long)s_tex_lookups, s_tex_lookups ? (double)s_tex_probe / (double)s_tex_lookups : 0.0,
            g.n_textures, (unsigned long long)s_tex_hashes, (double)s_tex_hash_bytes / 1048576.0,
            (double)s_tex_hash_cyc / 1e6, (unsigned long long)s_tex_ups, (double)s_tex_up_cyc / 1e6, 10);
    fprintf(stderr, "[rsx engine] texture slots %.1f Mcyc; %u surfaces, %u depth buffers%c",
            (double)s_tex_slot_cyc / 1e6, g.n_surfaces, g.n_zdepths, 10);
    s_tex_slot_cyc = 0;
    fprintf(stderr, "[rsx engine] constants: %llu of the draws saw uploads, %.1f slots each%c",
            (unsigned long long)s_const_dirty_draws,
            s_const_dirty_draws ? (double)s_const_dirty_slots / (double)s_const_dirty_draws : 0.0, 10);
    s_const_dirty_draws = s_const_dirty_slots = 0;
    { extern unsigned long long g_rsx_ww_poll_us, g_rsx_ww_polls, g_rsx_ww_pages;
      fprintf(stderr, "[rsx engine] write watch: %llu polls %.1f ms, %llu pages written; skipped hashing %llu texture and %llu geometry checks%c",
              g_rsx_ww_polls, (double)g_rsx_ww_poll_us / 1000.0, g_rsx_ww_pages,
              (unsigned long long)s_tex_ww_skips, (unsigned long long)s_geom_ww_skips, 10);
      extern unsigned long long g_rsx_ww_heap_us, g_rsx_ww_heap_polls, g_rsx_ww_range_us[3];
      extern unsigned rsx_ww_heap_span_mb(void);
      fprintf(stderr, "[rsx engine] write watch split: %llu heap polls %.1f ms (span %u MB); full polls by range %.1f / %.1f / %.1f ms%c",
              g_rsx_ww_heap_polls, (double)g_rsx_ww_heap_us / 1000.0, rsx_ww_heap_span_mb(), (double)g_rsx_ww_range_us[0] / 1000.0,
              (double)g_rsx_ww_range_us[1] / 1000.0, (double)g_rsx_ww_range_us[2] / 1000.0, 10);
      g_rsx_ww_heap_us = g_rsx_ww_heap_polls = g_rsx_ww_range_us[0] = g_rsx_ww_range_us[1] = g_rsx_ww_range_us[2] = 0;
      g_rsx_ww_polls = g_rsx_ww_poll_us = g_rsx_ww_pages = 0; s_tex_ww_skips = s_geom_ww_skips = 0; }
    fprintf(stderr, "[rsx engine] depth snapshots %llu, depth-as-colour builds %llu, unit memo hits %llu%c",
            (unsigned long long)s_snap_builds, (unsigned long long)s_ascolor_builds, (unsigned long long)s_memo_hits, 10);
    s_snap_builds = s_ascolor_builds = s_memo_hits = 0;
    fprintf(stderr, "[rsx engine] texture stage: decode %.0f Mcyc, memo %.0f Mcyc, vertex units %.0f Mcyc, %llu unmemoised units; cache %u/%u, %llu evictions; %u surfaces, %u depths%c",
            s_tsub[0] / 1e6, s_tsub[1] / 1e6, s_tsub[3] / 1e6, (unsigned long long)s_tsub_n,
            g.n_textures, (unsigned)ENG_MAX_TEXTURES, (unsigned long long)s_tex_evicts, g.n_surfaces, g.n_zdepths, 10);
    s_tex_evicts = 0;
    memset(s_tsub, 0, sizeof s_tsub); s_tsub_n = 0;
    /* RSX_ENG_LIST_DUMP=1: what fills the surface and depth lists (once). */
    { static int done = 0;
      if (!done && getenv("RSX_ENG_LIST_DUMP") && g.n_zdepths >= ENG_MAX_ZDEPTHS && g.n_surfaces >= ENG_MAX_SURFACES) { done = 1;
          for (u32 i = 0; i < g.n_zdepths; i++)
              fprintf(stderr, "[eng-list] z%u %u:0x%08X %ux%u used %ux%u write %u%c", i, g.zdepths[i].location,
                      g.zdepths[i].offset, g.zdepths[i].w, g.zdepths[i].h, g.zdepths[i].used_w, g.zdepths[i].used_h,
                      g.zdepths[i].had_write, 10);
          for (u32 i = 0; i < g.n_surfaces; i++)
              fprintf(stderr, "[eng-list] s%u %u:0x%08X %ux%u fmt %d age %u written %u%c", i, g.surfaces[i].location,
                      g.surfaces[i].offset, g.surfaces[i].w, g.surfaces[i].h, (int)g.surfaces[i].fmt,
                      g.surf_serial - g.surfaces[i].last_use, g.surfaces[i].color_written, 10);
      } }
    s_tex_lookups = s_tex_probe = s_tex_hashes = s_tex_hash_bytes = s_tex_hash_cyc = s_tex_ups = s_tex_up_cyc = 0;
    s_sink_calls = 0;
}

/* Whether any per-draw diagnostic is asked for (RSX_ENG_TRACE_*, _PROBE*,
 * _PPM_AT*, _FRAMELOG_*, _ZPROBE_W): read once. Off -- always, in play -- a
 * draw passes all of them with one branch; their ~40 checks and the target-set
 * count (a search of up to 48 entries) otherwise ran on every draw. */
static int eng_draw_diag(void)
{
    static int on = -1;
    if (on < 0) {
        static const char* const k[] = {
            "RSX_ENG_TRACE_TARGETS", "RSX_ENG_TRACE_OFFSET", "RSX_ENG_TRACE_TEX", "RSX_ENG_TRACE_FILE",
            "RSX_ENG_TRACE_PIPE", "RSX_ENG_TRACE_PRIM", "RSX_ENG_TRACE_W", "RSX_ENG_FRAMELOG_GATE",
            "RSX_ENG_ZPROBE_W", "RSX_ENG_PPM_AT", "RSX_ENG_PPM_AT2", "RSX_ENG_PROBE", "RSX_ENG_PROBE_FILE" };
        on = 0;
        for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++) if (getenv(k[i])) on = 1;
    }
    return on;
}

static void sink_end(void* user, const rsx_dispatch* r)
{
    u64 _st = eng_tsc();
    const u64 hf0_ = eng_hf_tsc();
    s_sink_calls++;
    (void)user; (void)r;
    if (!g.ready || !dc.n_packets) return;

    const u32 prim = g.rsx.current_primitive;
    /* Alpha test on with NEVER: every fragment fails it, and a failed
     * fragment writes no colour, depth or stencil -- the draw leaves no trace
     * (A Crack in Time's refraction quads: teleports, fire haze, the Dynamo
     * flash; rsx_draw_engine_decode_render_state). Not fetched, not drawn. */
    if ((rsx_dsp_reg(&g.rsx, M_ALPHA_TEST_ENABLE) & 1u) && (rsx_dsp_reg(&g.rsx, M_ALPHA_FUNC) & ~0x0200u) == 0u) {
        ENG_DROP(9); return;
    }

    /* Everything that becomes triangles -- lists, strips, fans, quads, quad
     * strips and polygons -- is REBUILT into one triangle list through an
     * index buffer. A host strip topology cannot express a restart cut, which
     * is the whole reason the reference engine rebuilds them, and rebuilding
     * is also what lets repeated references share an uploaded vertex. Points
     * and lines pass through as the guest issued them, and anything left that
     * would need an expansion this engine does not have is dropped rather
     * than drawn as something else. */
    rsx_topology topology = RSX_TOPOLOGY_TRIANGLES;
    int scratch = 0;
    const int rebuild = eng_topology_rebuild(prim, 0, &scratch) != 0;
    if (!rebuild) {
        if (rsx_primitive_needs_expansion(prim)) { ENG_DROP(0); return; }
        topology = rsx_primitive_topology(prim);
        if (topology == RSX_TOPOLOGY_UNSUPPORTED) { ENG_DROP(1); return; }
    }

    rsx_vertex_layout_plan layout;
    { const u64 t_ = eng_rdtsc(); eng_vertex_layout(&layout); s_gstage[0] += eng_rdtsc() - t_; }
    eng_geom geo;
    { const int why = eng_geom_get(&layout, prim, rebuild, &geo);
      STAGE(0);
      if (why) { ENG_DROP(why); return; } }
    const int indexed = geo.indices != NULL || geo.gpu_indexed;
    const u32 n_draw = geo.n_draw;

    rsx_be_render_state rs;
    rsx_draw_engine_decode_render_state(&g.rsx, &rs);
    /* RSX_ENG_DBG=<bits>: neutralise one piece of render state for every draw,
     * to find which one is eating a frame that draws but shows nothing.
     * 1 depth test off, 2 culling off, 8 alpha test off, 16 stencil off,
     * 32 blending off, 64 write all colour channels, 128 depth compare
     * ALWAYS (test on, so depth is still written -- bit 1 stops the writes). */
    { static long s_dbg = -1;
      if (s_dbg < 0) { const char* e = getenv("RSX_ENG_DBG"); s_dbg = e ? strtol(e, 0, 0) : 0; }
      if (s_dbg) {
          if (s_dbg & 1)  rs.depth_test = 0;
          if (s_dbg & 2)  rs.cull_enable = 0;
          if (s_dbg & 8)  rs.alpha_test_enable = 0;
          if (s_dbg & 16) rs.stencil_enable = 0;
          if (s_dbg & 32) rs.blend_enable = 0;
          if (s_dbg & 64) rs.color_mask = 0x01010101u;
          if (s_dbg & 128) rs.depth_func = 0x0207u;   /* depth test ALWAYS: writes kept, every draw's depth seen */
      } }

    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    if (!n_targets) { ENG_DROP(6); return; }
    const u32 target = targets[0];
    /* RSX_ENG_SKIP_TARGET=<hex>[,<hex>...]: drop every draw into those colour
     * targets -- which pass puts a given artefact on screen. */
    { static int n_sk = -1; static u32 sk[8];
      if (n_sk < 0) { n_sk = 0; const char* e = getenv("RSX_ENG_SKIP_TARGET");
                      while (e && *e && n_sk < 8) { sk[n_sk++] = (u32)strtoul(e, (char**)&e, 16); if (*e == ',') e++; else break; } }
      for (int i = 0; i < n_sk; i++) if (g.surfaces[target].offset == sk[i]) { ENG_DROP(6); return; } }
    /* RSX_ENG_SKIP_RANGE=<hex target>:<first>:<last>: drop that target's
     * draws numbered first..last within each frame -- bisects one draw. */
    { static int on = -1; static u32 st, sfirst, slast, n, fr;
      if (on < 0) { const char* e = getenv("RSX_ENG_SKIP_RANGE");
                    on = e && sscanf(e, "%x:%u:%u", &st, &sfirst, &slast) == 3; }
      if (on && g.surfaces[target].offset == st) {
          if (fr != g.frames) { fr = g.frames; n = 0; }
          const u32 k = n++;
          if (k >= sfirst && k <= slast) { ENG_DROP(6); return; }
      } }

    rsx_dsp_surface sf;
    rsx_dsp_viewport vp;
    rsx_dsp_get_surface(&g.rsx, &sf);
    rsx_dsp_get_viewport(&g.rsx, &vp);
    const u32 aas = eng_aa_scale();
    const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                     sf.clip_w * aas, sf.clip_h * aas);
    if (zslot != ENG_INVALID) g.zdepths[zslot].bpp = sf.depth_format == 1 ? 2u : 4u;

    /* Textures before any pipeline or target binding: a cache refresh can
     * replace a resource, and the backend is free to submit while doing it. */
    u32 textures[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samplers[RSX_BE_MAX_TEXTURES];
    u32 vtextures[RSX_BE_MAX_VERTEX_TEXTURES];
    rsx_be_sampler_desc vsamplers[RSX_BE_MAX_VERTEX_TEXTURES];
    memset(textures, 0, sizeof textures);
    memset(samplers, 0, sizeof samplers);
    memset(vtextures, 0, sizeof vtextures);
    memset(vsamplers, 0, sizeof vsamplers);
    STAGE(1);
    static int s_all_units = -1;     /* RSX_ENG_ALL_UNITS=1: resolve every enabled unit, as before */
    if (s_all_units < 0) { const char* e = getenv("RSX_ENG_ALL_UNITS"); s_all_units = e && *e == '1'; }
    const u32 fp_mask_ = s_all_units ? 0xFFFFu : eng_fp_tex_mask();
    const u32 tex_mask = sink_bind_textures(targets, n_targets, zslot, fp_mask_,
                                            textures, samplers);
    if (eng_verify(8) && !s_all_units) {
        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
            if (!((fp_mask_ >> u) & 1u) || textures[u]) continue;
            rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
            static u64 seen[512]; const u64 k = ((u64)t.offset << 20) ^ ((u64)t.format << 8) ^ u ^ ((u64)t.enabled << 63);
            u32 i = (u32)(k % 509u); int known = 0;
            for (u32 j = 0; j < 8; j++) if (seen[(i + j) % 512] == k) { known = 1; break; } else if (!seen[(i + j) % 512]) { seen[(i + j) % 512] = k; break; }
            if (!known)
                fprintf(stderr, "[verify] draw samples unit %u with nothing bound: %s, %u:0x%08X %ux%u fmt 0x%02X (frame %u)%c",
                        u, t.enabled ? "enabled" : "DISABLED unit", t.location, t.offset, t.width, t.height, t.format, g.frames, 10);
        }
    }
    const u64 _tv = eng_rdtsc();
    static u32 vm_gen = ~0u, vm_mask;
    if (vm_gen != g.rsx.vtex_gen) { vm_mask = eng_vtex_mask(); vm_gen = g.rsx.vtex_gen; }
    const u32 vtex_mask = sink_bind_vertex_textures(vm_mask, vtextures,
                                                    vsamplers);
    s_tsub[3] += eng_rdtsc() - _tv;

    /* A guest program pair that will not translate has no fallback: the draw
     * is dropped, as the reference engine drops it. Substituting the built-in
     * program would draw the geometry in the wrong colours, which is harder to
     * see than a missing object and hides the translation failure. */
    STAGE(2);
    int pipeline_is_fixed = 1;
    const u32 pipeline = eng_pipeline_get(&layout, &rs,
                                          eng_surface_format(sf.color_format),
                                          n_targets, &pipeline_is_fixed);
    STAGE(3);
    u64 _ct = _st;
    if (!pipeline) { ENG_DROP(7); return; }
    ENG_DROP(8);                                   /* not a drop: reached the backend */

    /* The transform constant block, then the viewport epilogue the vertex
     * program's tail multiplies by: RSX window coordinates mapped into the
     * host's clip space (rsx_live_draw.c:6143-6156). */
    const float W = sf.clip_w ? (float)sf.clip_w : (float)g.width;
    const float H = sf.clip_h ? (float)sf.clip_h : (float)g.height;
    float xf[8] = { 1, 1, 1, 0, 0, 0, 0, 0 };
    if (vp.scale[0] != 0.0f || vp.translate[0] != 0.0f) {
        xf[0] = vp.scale[0] / (W * 0.5f);
        xf[1] = -(vp.scale[1] / (H * 0.5f));
        xf[2] = vp.scale[2];
        xf[4] = (vp.translate[0] - W * 0.5f) / (W * 0.5f);
        xf[5] = -((vp.translate[1] - H * 0.5f) / (H * 0.5f));
        xf[6] = vp.translate[2];
    }
    /* Polygon offset, constant part: units times the GUEST depth format's
     * step (2^-16 for Z16, 2^-24 for Z24S8), added to window z through the
     * epilogue (z += units*r*w/w). The host depth buffer is D32_SFLOAT, and
     * Vulkan scales a float format's constant bias by 2^(exponent(z)-23) per
     * primitive: on a Z16 shadow map at z ~0.1 that is ~2000x less bias than
     * the RSX applies, and the casters self-shadowed (acne). The slope part
     * stays a depth bias (set_depth_bias below, format independent).
     * RSX_ENG_DBIAS_HOST=1 restores the host bias for both parts. */
    static int s_dbias_host = -1;
    if (s_dbias_host < 0) s_dbias_host = getenv("RSX_ENG_DBIAS_HOST") ? 1 : 0;
    if (!s_dbias_host && rs.poly_bias != 0.0f)
        xf[6] += rs.poly_bias * (sf.depth_format == 1 ? (1.0f / 65536.0f) : (1.0f / 16777216.0f));
    /* Only the slots uploaded since the last draw: g.vp_cb keeps the rest.
     * The whole 8 KB block per draw was most of the RSX thread's memcpy. */
    if (g.rsx.const_dirty_hi > g.rsx.const_dirty_lo) {
        memcpy(g.vp_cb + g.rsx.const_dirty_lo * 16u, g.rsx.constants[g.rsx.const_dirty_lo],
               (g.rsx.const_dirty_hi - g.rsx.const_dirty_lo) * 16u);
        eng_vp_dirty(g.rsx.const_dirty_lo * 16u, g.rsx.const_dirty_hi * 16u);
        s_const_dirty_draws++; s_const_dirty_slots += g.rsx.const_dirty_hi - g.rsx.const_dirty_lo;
        g.rsx.const_dirty_lo = g.rsx.const_dirty_hi = 0;
        s_vp_cb_changed = 1;
    }
    if (memcmp(g.vp_cb + RSX_DSP_NUM_CONSTANTS * 16u, xf, sizeof xf)) {
        memcpy(g.vp_cb + RSX_DSP_NUM_CONSTANTS * 16u, xf, sizeof xf);
        eng_vp_dirty(RSX_DSP_NUM_CONSTANTS * 16u, RSX_DSP_NUM_CONSTANTS * 16u + (u32)sizeof xf);
        s_vp_cb_changed = 1;
    }
    CSTAGE(0);

    if (__builtin_expect(eng_draw_diag(), 0)) {   /* RSX_ENG_TRACE_*: off unless asked for */
    /* RSX_ENG_TRACE_TARGETS=1: per frame, how many draws went to each target
     * set (printed every 120 presents by eng_targets_report). */
    { int found = 0; const u32 aa = (rsx_dsp_reg(&g.rsx, 0x0208u) >> 12) & 0xFu;   /* SURFACE_FORMAT antialias */
      for (u32 i = 0; i < s_tt_n; i++)
          if (s_tt[i].slot == target && s_tt[i].n == n_targets && s_tt[i].zslot == zslot && s_tt[i].aa == aa) { s_tt[i].count++; found = 1; break; }
      if (!found && s_tt_n < 48) { s_tt[s_tt_n].slot = target; s_tt[s_tt_n].n = n_targets; s_tt[s_tt_n].zslot = zslot;
                                   s_tt[s_tt_n].aa = aa; s_tt[s_tt_n].count = 1; s_tt_n++; } }

    /* RSX_ENG_TRACE_OFFSET=<hex>: describe the draws aimed at the colour target
     * at that offset -- what each one samples and with which state -- from
     * present RSX_ENG_TRACE_AFTER on, RSX_ENG_TRACE_MAX lines (default 48).
     * For following a picture from one render target to the next. */
    /* RSX_ENG_TRACE_TEX=<hex>: the same, for every draw (into any target)
     * that samples a texture at that offset -- who reads a buffer. */
    { static long s_to = -2, s_after = 0, s_max = 48, s_tt_off = -1; static long s_tn = 0;
      if (s_to == -2) { const char* e = getenv("RSX_ENG_TRACE_OFFSET"); s_to = e ? (long)strtoul(e, 0, 16) : -1;
                        const char* t = getenv("RSX_ENG_TRACE_TEX");    s_tt_off = t ? (long)strtoul(t, 0, 16) : -1;
                        const char* a = getenv("RSX_ENG_TRACE_AFTER"); if (a) s_after = atol(a);
                        const char* m = getenv("RSX_ENG_TRACE_MAX");   if (m) s_max = atol(m); }
      /* RSX_ENG_TRACE_FILE=<file>: anything written there while the game runs
       * restarts the trace at the next frame (budget RSX_ENG_TRACE_MAX again;
       * the file is emptied once read) -- trace the moment you want, not the
       * level start. */
      { static const char* tf = (const char*)-1; static u32 tchk = ~0u;
        if (tf == (const char*)-1) { tf = getenv("RSX_ENG_TRACE_FILE"); if (tf) s_after = 0x7FFFFFFF; }
        if (tf && g.frames != tchk && g.frames % 15u == 0u) {
            tchk = g.frames;
            FILE* f = fopen(tf, "rb");
            if (f) { const int c = fgetc(f); fclose(f);
                     if (c != EOF) { f = fopen(tf, "wb"); if (f) fclose(f);
                                     s_after = (long)g.frames + 1; s_tn = 0; s_clearlog_from = g.frames + 1u;
                                     fprintf(stderr, "[rsx trace] file: tracing from frame %ld%c", s_after, 10); } } } }
      int samples_it = 0;
      if (s_tt_off >= 0)
          for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
              if (!(tex_mask & (1u << u))) continue;
              rsx_dsp_texture t; rsx_dsp_get_texture(&g.rsx, u, &t);
              if (t.offset == (u32)s_tt_off) { samples_it = 1; break; }
          }
      /* RSX_ENG_TRACE_PIPE=<hex key>: the draws with that pipeline. */
      static u64 s_tpk = 1;
      if (s_tpk == 1) { const char* k = getenv("RSX_ENG_TRACE_PIPE"); s_tpk = k ? strtoull(k, 0, 16) : 0; }
      /* RSX_ENG_TRACE_PRIM=<n>[:<blend sfactor hex>]: the draws of that
       * primitive type (8 = quads: A Crack in Time's particles), optionally
       * only those blending with that source factor. */
      static long s_tprim = -2; static long s_tsf = -1, s_ttg = -1;
      if (s_tprim == -2) { const char* p = getenv("RSX_ENG_TRACE_PRIM"); s_tprim = p ? atol(p) : -1;
                           const char* c = p ? strchr(p, ':') : NULL; if (c && c[1] && c[1] != ':') s_tsf = (long)strtoul(c + 1, 0, 16);
                           const char* c2 = c ? strchr(c + 1, ':') : NULL; if (c2) s_ttg = (long)strtoul(c2 + 1, 0, 16); }
      const int prim_hit = s_tprim >= 0 && (long)prim == s_tprim &&
                           (s_tsf < 0 || (rs.blend_enable && (long)rs.sf_rgb == s_tsf)) &&
                           (s_ttg < 0 || g.surfaces[target].offset == (u32)s_ttg);
      if (((s_to >= 0 && g.surfaces[target].offset == (u32)s_to) || samples_it || eng_trace_width_ok(g.surfaces[target].w) ||
           (s_tpk && s_last_pipe_key == s_tpk) || prim_hit) &&
          (long)g.frames >= s_after && s_tn < s_max && eng_trace_period_ok() && eng_trace_per_pipe_ok(s_last_pipe_key)) {
          s_tn++;
          { u64 pk = 0; for (u32 i = 0; i < g.n_pipelines; i++) if (g.pipelines[i].handle == pipeline) { pk = g.pipelines[i].key; break; }
            fprintf(stderr, "[rsx trace]   pipeline %016llx\n", (unsigned long long)pk); }
          fprintf(stderr, "[rsx trace] frame %u -> %08X %ux%u (x%u targets) prim %u n=%u | blend %u (%X,%X) depth t%u w%u f%X"
                          " alpha %u cull %u mask %08X | vp scale %.1f,%.1f tr %.1f,%.1f clip %ux%u\n",
                  g.frames, g.surfaces[target].offset, g.surfaces[target].w, g.surfaces[target].h, n_targets,
                  (unsigned)prim, n_draw, rs.blend_enable, rs.sf_rgb, rs.df_rgb, rs.depth_test, rs.depth_write,
                  rs.depth_func, rs.alpha_test_enable, rs.cull_enable, rs.color_mask,
                  vp.scale[0], vp.scale[1], vp.translate[0], vp.translate[1], sf.clip_w, sf.clip_h);
          { const u32 cmin = rsx_dsp_reg(&g.rsx, 0x0394), cmax = rsx_dsp_reg(&g.rsx, 0x0398);
            float fmin, fmax; memcpy(&fmin, &cmin, 4); memcpy(&fmax, &cmax, 4);
            fprintf(stderr, "[rsx trace]     vp z scale %.6f tr %.6f | clip min %g max %g | zminmax ctrl %X%c",
                    vp.scale[2], vp.translate[2], fmin, fmax, rsx_dsp_reg(&g.rsx, 0x1D78), 10); }
          /* RSX_ENG_TRACE_REGS=1: every non-zero register below the texture block, to diff two draws' state. */
          { static int tr = -1; if (tr < 0) tr = getenv("RSX_ENG_TRACE_REGS") ? 1 : 0;
            if (tr) { fprintf(stderr, "[rsx trace]     regs");
                      for (u32 m = 0x100u; m < 0x2000u; m += 4u) { if (m >= 0x1A00u && m < 0x1C00u) continue;
                          const u32 v = rsx_dsp_reg(&g.rsx, m); if (v) fprintf(stderr, " %04X=%X", m, v); }
                      fputc(10, stderr); } }
          fprintf(stderr, "[rsx trace]     shader packer %08X (sRGB write %u) | targets", rsx_dsp_reg(&g.rsx, 0x1FECu),
                  rsx_dsp_reg(&g.rsx, 0x1FECu) & 1u);
          for (u32 i = 0; i < n_targets; i++)
              fprintf(stderr, " %08X/fmt %X", g.surfaces[targets[i]].offset, g.surfaces[targets[i]].rsx_fmt);
          fputc(10, stderr);
          for (u32 ai = 0; ai < 16; ai++) {
              rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, ai, &va);
              if (!va.type || !va.size) continue;
              fprintf(stderr, "[rsx trace]     attr%u type %u size %u stride %u freq %u off 0x%08X loc %u%c", ai, va.type, va.size,
                      va.stride, va.frequency, va.offset, va.location, 10);
          }
          { static const u32 cl[] = { 0, 8, 9, 10, 11, 18, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40 };
            for (u32 k = 0; k < sizeof cl / sizeof cl[0]; k++) {
                const float* c = (const float*)g.rsx.constants[cl[k]];
                fprintf(stderr, "[rsx trace]     vc%u %g %g %g %g%c", cl[k], c[0], c[1], c[2], c[3], 10);
            }
            /* RSX_ENG_TRACE_VC=3,4,5,20: further vertex constants by index. */
            { static const char* ex = (const char*)1;
              if (ex == (const char*)1) ex = getenv("RSX_ENG_TRACE_VC");
              /* "all": every non-zero constant, full precision (for running a
               * traced draw's vertex program on the CPU) */
              if (ex && !strcmp(ex, "all"))
                  for (u32 ci = 0; ci < 512; ci++) { const float* c = (const float*)g.rsx.constants[ci];
                      if (c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f || c[3] != 0.0f)
                          fprintf(stderr, "[rsx trace]     vc%u %.9g %.9g %.9g %.9g%c", ci, c[0], c[1], c[2], c[3], 10); }
              for (const char* q = (ex && !strcmp(ex, "all")) ? NULL : ex; q && *q; ) {
                  char* e_; const unsigned long ci = strtoul(q, &e_, 10);
                  if (e_ == q) break;
                  if (ci < 512) { const float* c = (const float*)g.rsx.constants[ci];
                      fprintf(stderr, "[rsx trace]     vc%lu %.9g %.9g %.9g %.9g%c", ci, c[0], c[1], c[2], c[3], 10); }
                  q = *e_ ? e_ + 1 : e_;
              } } }
          /* Every input slot of vertex 0, decoded, and the first raw bytes
           * of its attribute in guest memory. */
          if (geo.verts && layout.stride >= 16)
              for (u32 sl = 0; sl < layout.count; sl++) {
                  const float* fv = (const float*)(geo.verts + sl * 16u);
                  const u32 ai = layout.attrs[sl];
                  rsx_dsp_vertex_attr va; rsx_dsp_get_vertex_attr(&g.rsx, ai, &va);
                  const u8* rb = va.type ? eng_guest_ptr(NULL, va.location,
                      rsx_dsp_vertex_data_base_offset(&g.rsx) + va.offset, 8) : NULL;
                  fprintf(stderr, "[rsx trace]     v0 attr%u = %g %g %g %g  raw %02X %02X %02X %02X %02X %02X %02X %02X%c", ai,
                          fv[0], fv[1], fv[2], fv[3], rb ? rb[0] : 0, rb ? rb[1] : 0, rb ? rb[2] : 0, rb ? rb[3] : 0,
                          rb ? rb[4] : 0, rb ? rb[5] : 0, rb ? rb[6] : 0, rb ? rb[7] : 0, 10);
              }
          if (geo.verts && layout.stride >= 16) {
              const float* fv = (const float*)geo.verts;
              fprintf(stderr, "[rsx trace]     decoded v0 a0: %g %g %g %g | v1 a0: %g %g %g %g%c", fv[0], fv[1], fv[2], fv[3],
                      geo.n_upload > 1 ? ((const float*)(geo.verts + layout.stride))[0] : 0.0f,
                      geo.n_upload > 1 ? ((const float*)(geo.verts + layout.stride))[1] : 0.0f,
                      geo.n_upload > 1 ? ((const float*)(geo.verts + layout.stride))[2] : 0.0f,
                      geo.n_upload > 1 ? ((const float*)(geo.verts + layout.stride))[3] : 0.0f, 10);
          }
          fprintf(stderr, "[rsx trace]     stencil %u func %X ref %02X mask %02X wmask %02X ops %X/%X/%X back %X %X/%X/%X two-sided %u | zeta %08X pitch %u (zslot %u)\n",
                  rs.stencil_enable, rs.s_func, rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu, rs.s_func_mask, rs.s_write_mask, rs.s_fail, rs.s_zfail, rs.s_zpass,
                  rs.bs_func, rs.bs_fail, rs.bs_zfail, rs.bs_zpass,
                  rs.stencil_two_sided, sf.zeta_offset, sf.zeta_pitch, zslot);
          { const u32 sh_ = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL), sv_ = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
            fprintf(stderr, "[rsx trace]     scissor %u,%u %ux%u  shader_window %08X  zeta_fmt %08X%c", sh_ & 0xFFFFu, sv_ & 0xFFFFu,
                    sh_ >> 16, sv_ >> 16, rsx_dsp_reg(&g.rsx, 0x1D88), rsx_dsp_reg(&g.rsx, 0x0208), 10); }
          /* RSX_ENG_TRACE_VERTS=n: the first n decoded vertices of every
           * traced draw (default: 4, and only for the first 6 draws). */
          static int s_trace_verts = -1;
          if (s_trace_verts < 0) { const char* e_ = getenv("RSX_ENG_TRACE_VERTS"); s_trace_verts = e_ ? atoi(e_) : 0; }
          if ((n_draw <= 6 || s_trace_verts > 0) && geo.verts && layout.stride >= 16) {
              const u32 nv = s_trace_verts > 0 ? (u32)s_trace_verts : 4u;
              for (u32 vi = 0; vi < geo.n_upload && vi < nv; vi++) {
                  const float* fv = (const float*)(geo.verts + (u64)vi * layout.stride);
                  fprintf(stderr, "[rsx trace]     v%u (stride %u):", vi, layout.stride);
                  for (u32 k = 0; k < 40 && k * 4 < layout.stride; k++) fprintf(stderr, k % 4 ? " %.9g" : " | %.9g", fv[k]);
                  fprintf(stderr, "\n");
              }
          }
          for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
              if (!(tex_mask & (1u << u))) continue;
              rsx_dsp_texture t;
              rsx_dsp_get_texture(&g.rsx, u, &t);
              const char* src = "texture cache";
              for (u32 i = 0; i < g.n_surfaces; i++)
                  if (g.surfaces[i].handle && g.surfaces[i].location == t.location && g.surfaces[i].offset == t.offset)
                      { src = (i == target) ? "ITS OWN TARGET (not sampled)" : "render target"; break; }
              for (u32 i = 0; i < g.n_zdepths; i++)
                  if (g.zdepths[i].location == t.location && g.zdepths[i].offset == t.offset) { src = "depth buffer"; break; }
              fprintf(stderr, "[rsx trace]     tex%u loc %u offset %08X %ux%u fmt 0x%X pitch %u zfunc %u filter %08X remap %04X wrap %08X border %08X <- %s\n",
                      u, t.location, t.offset, t.width, t.height, t.format, t.pitch, (t.wrap >> 28) & 7u, t.filter,
                      t.remap & 0xFFFFu, t.wrap, t.border_color, s_tex_src[u] && *s_tex_src[u] ? s_tex_src[u] : src);
              if (s_tex_src[u] && !strcmp(s_tex_src[u], "colour target") && s_tex_src_slot[u] < g.n_surfaces)
                  fprintf(stderr, "[rsx trace]       (that target: %ux%u, written %d, surface format 0x%X)%c", g.surfaces[s_tex_src_slot[u]].w,
                          g.surfaces[s_tex_src_slot[u]].h, g.surfaces[s_tex_src_slot[u]].color_written,
                          g.surfaces[s_tex_src_slot[u]].rsx_fmt, 10);
          }
          for (u32 c = 0; c < g.fp_constants.count && c < 20; c++) {
              const float* fc = (const float*)g.fp_constants.values + c * 4u;
              fprintf(stderr, "[rsx trace]     fc%u %g %g %g %g%c", c, fc[0], fc[1], fc[2], fc[3], 10);
          }
          for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++)
              if ((tex_mask >> u) & 1u)
                  fprintf(stderr, "[rsx trace]     texp%u sub %.4f %.4f reint 0x%X%c", u, s_tex_sub[u][0], s_tex_sub[u][1], s_tex_reint[u], 10);
      } }
    }

    CSTAGE(1);
    /* The buffered fragment constants, then fp_alpha: the layout
     * rsx_fp_decompile_buffered_ex compiled the shader against. */
    const u32 nslots = g.fp_constants.count ? g.fp_constants.count : 1u;
    const int dyn_tex = eng_dyn_tex() && !pipeline_is_fixed;
    const u32 fp_bytes = (nslots + 1u + (dyn_tex ? 16u : 0u)) * 16u;
    if (g.fp_cb_cap < fp_bytes) {
        u8* n = (u8*)realloc(g.fp_cb, fp_bytes);
        if (!n) return;
        g.fp_cb = n;
        g.fp_cb_cap = fp_bytes;
    }
    memset(g.fp_cb, 0, fp_bytes);
    if (g.fp_constants.count)
        memcpy(g.fp_cb, g.fp_constants.values, g.fp_constants.count * 16u);
    { float* alpha = (float*)(g.fp_cb + nslots * 16u);
      alpha[0] = rsx_fp_alpha_ref(rs.alpha_ref_raw, rs.alpha_ref_format);
      /* WPOS in guest pixels (rsx_fp_decompiler input_expr): the backend's
       * pixel over the internal resolution and over the 4x pass's doubling. */
      alpha[1] = 1.0f / (g_rsx_res_scale * (float)aas);
      alpha[2] = alpha[3] = 0.0f; }
    if (dyn_tex) {   /* fp_texp[16]: coordinate scale, gamma channel bits, biased */
        float* tp = (float*)(g.fp_cb + (nslots + 1u) * 16u);
        for (u32 u = 0; u < 16u; u++, tp += 4) {
            const int un = ((s_tm_unnorm >> u) & 1u) && !((s_tm_cube >> u) & 1u) &&
                           s_tm_dim[u][0] && s_tm_dim[u][1];
            const int cube = (s_tm_cube >> u) & 1u;
            tp[0] = (un ? 1.0f / (float)s_tm_dim[u][0] : 1.0f) * (cube ? 1.0f : s_tex_sub[u][0]);
            tp[1] = (un ? 1.0f / (float)s_tm_dim[u][1] : 1.0f) * (cube ? 1.0f : s_tex_sub[u][1]);
            u32 gam = (u32)((s_tm_gamma >> (u * 4u)) & 0xFu);
            /* A BIASED (x2-1, signed data) read of a target the last draw
             * packed as sRGB reads it back linear. A Crack in Time leaves the
             * sRGB write on for its normals pass (its frame ends with it set),
             * yet its sun-shadow projections read those normals biased and
             * without gamma: on the console they are linear, and packed here
             * the floor's N.L came out ~0 -- a dark square under every
             * character. A signed read of gamma-packed bytes means nothing,
             * so this cannot change a pass that reads colour. */
            if (((s_tm_biased >> u) & 1u) && ((s_tex_srgb_src >> u) & 1u)) gam |= 7u;
            tp[2] = s_tex_reint[u] ? (float)s_tex_reint[u] : (float)gam;
            tp[3] = (float)((s_tm_biased >> u) & 1u);
        }
    }

    CSTAGE(2);
    u64 _bt = _ct;
    /* The depth attachment is cleared the first time it is bound, so a title
     * that never clears its shadow zeta still tests against something. */
    const u32 depth_handle = (zslot != ENG_INVALID) ? g.zdepths[zslot].handle : 0;
    if (zslot != ENG_INVALID && !g.zdepths[zslot].cleared) {
        g.be->clear_depth_stencil(g.be->user, depth_handle,
                                  RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL,
                                  1.0f, 0);
        g.zdepths[zslot].cleared = 1;
        g.zdepths[zslot].had_write = 0;
    }

    /* A colour pass that tests against a depth buffer the backend draws at a
     * shadow map's scale sends it back to the scene's (vk_eng_depth_demote):
     * which. */
    if (rs.color_mask && (rs.depth_test || rs.stencil_enable) && zslot != ENG_INVALID &&
        rsx_depth_target_scale(g.zdepths[zslot].w, g.zdepths[zslot].h) != g_rsx_res_scale) {
        static u32 logs;
        if (logs++ < 8)
            fprintf(stderr, "[rsx engine] colour pass over shadow-size depth %u:0x%08X %ux%u: colour 0x%08X %ux%u mask %08X, frame %u%c",
                    g.zdepths[zslot].location, g.zdepths[zslot].offset, g.zdepths[zslot].w, g.zdepths[zslot].h,
                    g.surfaces[targets[0]].offset, g.surfaces[targets[0]].w, g.surfaces[targets[0]].h, rs.color_mask, g.frames, 10);
    }
    u32 handles[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i < n_targets; i++) { handles[i] = g.surfaces[targets[i]].handle;
                                          if (rs.color_mask) g.surfaces[targets[i]].srgb_written = (u8)s_srgb_draw; }
    g.be->bind_targets(g.be->user, handles, n_targets, depth_handle);
    g.be->bind_pipeline(g.be->user, pipeline);
    BSUB(0);
    /* The 7.5 KB block only when it changed: the backend keeps what it was
     * last given, and binding it unchanged copied it twice per draw. */
    if (s_vp_cb_changed || s_vp_cb_bound != g.vp_cb) {
        if (s_vp_cb_bound != g.vp_cb) { s_vp_dlo = 0; s_vp_dhi = ENG_VP_CB_BYTES; }
        if (g.be->bind_vs_constants_delta && s_vp_dlo < s_vp_dhi)
            g.be->bind_vs_constants_delta(g.be->user, g.vp_cb, ENG_VP_CB_BYTES, s_vp_dlo,
                                          s_vp_dhi > ENG_VP_CB_BYTES ? ENG_VP_CB_BYTES : s_vp_dhi);
        else
            g.be->bind_vs_constants(g.be->user, g.vp_cb, ENG_VP_CB_BYTES);
        s_vp_cb_changed = 0; s_vp_cb_bound = g.vp_cb;
        s_vp_dlo = 0; s_vp_dhi = 0;
    }
    BSUB(1);
    /* The backend keeps the block it was last given: an identical one (the
     * common case -- the same material draw after draw) is not sent again
     * through the worker's queue. */
    if (fp_bytes != s_ps_last_n || memcmp(s_ps_last, g.fp_cb, fp_bytes) != 0) {
        g.be->bind_ps_constants(g.be->user, g.fp_cb, fp_bytes);
        if (s_ps_last_cap < fp_bytes) { u8* n = (u8*)realloc(s_ps_last, fp_bytes); if (n) { s_ps_last = n; s_ps_last_cap = fp_bytes; } }
        if (s_ps_last_cap >= fp_bytes) { memcpy(s_ps_last, g.fp_cb, fp_bytes); s_ps_last_n = fp_bytes; }
        else s_ps_last_n = 0;
    }
    BSUB(2);
    g.be->bind_textures(g.be->user, textures, samplers, tex_mask);
    g.be->bind_vertex_textures(g.be->user, vtextures, vsamplers, vtex_mask);
    BSUB(3);
    g.be->set_viewport(g.be->user, 0.0f, 0.0f, W * (float)aas, H * (float)aas);

    /* The guest scissor intersected with the surface. The nv40 reset is a
     * full 4096x4096 window and a never-written register reads 0 here, which
     * the w == 0 test treats as no scissor, so only a real game scissor
     * narrows anything (rsx_live_draw.c:6180-6197). */
    {
        u32 sx = 0, sy = 0;
        u32 sw = g.surfaces[target].w, sh = g.surfaces[target].h;
        const u32 h = rsx_dsp_reg(&g.rsx, M_SCISSOR_HORIZONTAL);
        const u32 v = rsx_dsp_reg(&g.rsx, M_SCISSOR_VERTICAL);
        const u32 gx = (h & 0xFFFFu) * aas, gw = (h >> 16) * aas;   /* 4x: in samples (eng_aa_scale) */
        const u32 gy = (v & 0xFFFFu) * aas, gh = (v >> 16) * aas;
        if (gw > 0 && gh > 0) {
            u32 right = sx + sw, bottom = sy + sh;
            if (gx > sx) sx = gx;
            if (gy > sy) sy = gy;
            if (gx + gw < right)  right = gx + gw;
            if (gy + gh < bottom) bottom = gy + gh;
            sw = right > sx ? right - sx : 0;
            sh = bottom > sy ? bottom - sy : 0;
        }
        g.be->set_scissor(g.be->user, sx, sy, sw, sh);
    }
    /* Dynamic, and deliberately outside the pipeline key. */
    g.be->set_stencil_ref(g.be->user,
                          rsx_dsp_reg(&g.rsx, M_STENCIL_FUNC_REF) & 0xFFu);
    /* The depth bias the game puts on its shadow-map and decal passes.
     * Without it every shadow caster self-shadowed: stripes over lit ground. */
    if (g.be->set_depth_bias) g.be->set_depth_bias(g.be->user, s_dbias_host ? rs.poly_bias : 0.0f, rs.poly_scale);
    /* SET_BLEND_COLOR, for CONSTANT_COLOR / CONSTANT_ALPHA factors. A Crack in
     * Time's normal pass writes a material flag into the G-buffer's alpha this
     * way, and its shadow mask pass picks by that flag: world geometry gets
     * max(1 - 2 N.L, shadow), seamless at any angle, characters min(2 N.L,
     * shadow). With the factors read as ONE every pixel counted as a character:
     * wherever N.L < 0.5 (moons, slopes, walls, stairs) each caster's volume and
     * screen tiles showed as a dark box. */
    if (g.be->set_blend_color && rs.blend_enable &&
        (((rs.sf_rgb | rs.df_rgb | rs.sf_a | rs.df_a) & 0x8000u) != 0u)) {
        const u32 bc = rsx_dsp_reg(&g.rsx, M_BLEND_COLOR);
        const float rgba[4] = { (float)((bc >> 16) & 0xFFu) / 255.0f, (float)((bc >> 8) & 0xFFu) / 255.0f,
                                (float)(bc & 0xFFu) / 255.0f, (float)(bc >> 24) / 255.0f };
        g.be->set_blend_color(g.be->user, rgba);
        { static u32 seen[24][3]; static u32 n_seen;   /* each use once, for the logs */
          const u32 fa = (rs.sf_rgb << 16) | rs.df_rgb, fb = (rs.sf_a << 16) | rs.df_a;
          u32 k = 0;
          while (k < n_seen && (seen[k][0] != fa || seen[k][1] != fb || seen[k][2] != bc)) k++;
          if (k == n_seen && n_seen < 24u) {
              seen[n_seen][0] = fa; seen[n_seen][1] = fb; seen[n_seen][2] = bc; n_seen++;
              fprintf(stderr, "[rsx engine] blend constant %08X: colour factors %04X,%04X alpha %04X,%04X, target 0x%08X%s%c",
                      bc, rs.sf_rgb, rs.df_rgb, rs.sf_a, rs.df_a, g.surfaces[target].offset, rs.rt_fp16 ? " (FP16)" : "", 10);
          } }
    }
    BSUB(4);
    CSTAGE(3);

    STAGE(4);
    {
        const u32 cur_tgt_off = g.surfaces[target].offset;
        if (cur_tgt_off == 0x00000000u || cur_tgt_off == 0x00398000u) {
            static u32 s_disp_log_cnt = 0;
            if (s_disp_log_cnt < 40 || (g.frames % 60 == 0)) {
                s_disp_log_cnt++;
                fprintf(stderr, "[disp-draw] f=%u tgt=%u:0x%08X %ux%u pipe=%016llx topo=%u n_draw=%u stride=%u idx=%d (cmask=0x%X dtest=%u dwrite=%u dfunc=0x%X blend=%u sf=0x%X df=0x%X zslot=%u:0x%08X)\n",
                        g.frames, g.surfaces[target].location, cur_tgt_off,
                        g.surfaces[target].w, g.surfaces[target].h,
                        (unsigned long long)s_last_pipe_key,
                        (unsigned)topology, n_draw, layout.stride, indexed,
                        rs.color_mask, rs.depth_test, rs.depth_write, rs.depth_func,
                        rs.blend_enable, rs.sf_rgb, rs.df_rgb,
                        zslot != ENG_INVALID ? g.zdepths[zslot].location : 99,
                        zslot != ENG_INVALID ? g.zdepths[zslot].offset : 0);
                for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
                    if (!(tex_mask & (1u << u))) continue;
                    rsx_dsp_texture tt; rsx_dsp_get_texture(&g.rsx, u, &tt);
                    fprintf(stderr, "   tex[%u]: %u:0x%08X %ux%u fmt=0x%02X wrap=0x%X remap=0x%X src=<%s>\n",
                            u, tt.location, tt.offset, tt.width, tt.height, tt.format, tt.wrap, tt.remap,
                            s_tex_src[u] ? s_tex_src[u] : "?");
                }
                if (geo.verts && layout.stride >= 16) {
                    const float* vf = (const float*)geo.verts;
                    fprintf(stderr, "   v0: %g %g %g %g | v1: %g %g %g %g\n",
                            vf[0], vf[1], vf[2], vf[3],
                            geo.n_upload > 1 ? ((const float*)(geo.verts + layout.stride))[0] : 0.0f,
                            geo.n_upload > 1 ? ((const float*)(geo.verts + layout.stride))[1] : 0.0f,
                            geo.n_upload > 1 ? ((const float*)(geo.verts + layout.stride))[2] : 0.0f,
                            geo.n_upload > 1 ? ((const float*)(geo.verts + layout.stride))[3] : 0.0f);
                }
            }
        }
    }
    g.surfaces[target].draws_since_present++;
    g.surfaces[target].last_clip = (sf.clip_w << 16) | sf.clip_h;
    if (geo.vb)
        g.be->draw_cached_vi(g.be->user, topology, geo.gpu, geo.n_upload, layout.stride, geo.indices, n_draw, geo.vofs);
    else if (geo.gpu && g.be->draw_cached)
        g.be->draw_cached(g.be->user, topology, geo.gpu, geo.n_upload, layout.stride, indexed ? n_draw : 0);
    else
        g.be->draw(g.be->user, topology, geo.verts, geo.n_upload, layout.stride,
                   geo.indices, indexed ? n_draw : 0);
    STAGE(5);
    s_hf.draws++; s_hf.sink_cyc += eng_hf_tsc() - hf0_;
    if (__builtin_expect(eng_draw_diag(), 0)) {   /* the per-draw diagnostics below, all off unless asked for */
    /* RSX_ENG_FRAMELOG_GATE=<file>: when that file appears, the next frame's
     * draws are logged -- target, depth buffer, pipeline and every texture
     * unit with where the engine bound it from -- and the file is removed.
     * One frame of a running game, on demand (no restart into the state). */
    { static const char* gate = (const char*)-1; static u32 chk = ~0u, log_frame = ~0u, nd;
      if (gate == (const char*)-1) gate = getenv("RSX_ENG_FRAMELOG_GATE");
      if (gate && chk != g.frames) {
          chk = g.frames;
          FILE* gf = fopen(gate, "rb");
          if (gf) { fclose(gf); remove(gate); log_frame = g.frames + 1u; nd = 0;
                    fprintf(stderr, "[framelog] armed for frame %u%c", log_frame, 10); }
      }
      if (gate && log_frame != ~0u && g.frames >= log_frame && g.frames < log_frame + 2u && nd < 6000u) {
          char line[2000];
          int p = snprintf(line, sizeof line, "[framelog] f%u d%u prim %u n %u tgt %u:%08X %ux%u fmt %d cm %X z %08X pipe %016llx |",
                           g.frames, nd++, (unsigned)prim, n_draw, g.surfaces[target].location, g.surfaces[target].offset,
                           g.surfaces[target].w, g.surfaces[target].h, (int)g.surfaces[target].fmt, (unsigned)rs.color_mask,
                           zslot != ENG_INVALID ? g.zdepths[zslot].offset : 0u, (unsigned long long)s_last_pipe_key);
          for (u32 u = 0; u < RSX_BE_MAX_TEXTURES && p < (int)sizeof line - 160; u++) {
              if (!(tex_mask & (1u << u))) continue;
              rsx_dsp_texture tt; rsx_dsp_get_texture(&g.rsx, u, &tt);
              p += snprintf(line + p, sizeof line - (size_t)p, " t%u %u:%08X %ux%u f%02X L%u p%u r%04X w%08X b%08X<%s", u, tt.location, tt.offset,
                            tt.width, tt.height, tt.format, tt.mipmaps, tt.pitch, tt.remap & 0xFFFFu, tt.wrap, tt.border_color,
                            s_tex_src[u] && *s_tex_src[u] ? s_tex_src[u] : "?");
          }
          fprintf(stderr, "%s%c", line, 10);
          /* RSX_ENG_FRAMELOG_DUMP=<dir>: each distinct texture of the logged
           * frame as its raw guest bytes (the whole mip chain), for offline
           * decoding: tex_<loc>_<off>_<w>x<h>_f<fmt>_L<levels>_p<pitch>.bin */
          { static const char* dd = (const char*)-1; static u32 seen[512], ns, seen_frame = ~0u;
            if (dd == (const char*)-1) dd = getenv("RSX_ENG_FRAMELOG_DUMP");
            if (dd) {
                if (seen_frame != log_frame) { seen_frame = log_frame; ns = 0; }
                for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
                    if (!(tex_mask & (1u << u))) continue;
                    rsx_dsp_texture tt; rsx_dsp_get_texture(&g.rsx, u, &tt);
                    const u32 key = tt.offset ^ (tt.location << 31) ^ (tt.format << 24);
                    int dup = 0; for (u32 k = 0; k < ns; k++) if (seen[k] == key) { dup = 1; break; }
                    if (dup || ns >= 512u) continue;
                    seen[ns++] = key;
                    const u32 span = eng_texture_span(tt.format & 0x9Fu, tt.width, tt.height, tt.mipmaps ? tt.mipmaps : 1u, tt.pitch, tt.cubemap);
                    const u8* src = span ? eng_guest_ptr(NULL, tt.location, tt.offset, span) : NULL;
                    if (!src) continue;
                    char path[700];
                    snprintf(path, sizeof path, "%s/f%u_tex_%u_%08X_%ux%u_f%02X_L%u_p%u.bin", dd, log_frame, tt.location, tt.offset,
                             tt.width, tt.height, tt.format, tt.mipmaps, tt.pitch);
                    FILE* tf = fopen(path, "wb");
                    if (tf) { fwrite(src, 1, span, tf); fclose(tf); }
                }
            } }
      } }
    /* RSX_ENG_ZPROBE_W=<width>: after each draw into a target that wide (one
     * frame in 300), the minimum of its depth buffer -- did the draw write? */
    { static long zw = -1; if (zw < 0) { const char* e = getenv("RSX_ENG_ZPROBE_W"); zw = e ? atol(e) : 0; }
      if (zw > 0 && g.surfaces[target].w == (u32)zw && g.frames % 300 == 0 && zslot != ENG_INVALID && g.be->readback) {
          eng_zdepth* z = &g.zdepths[zslot];
          const u32 snap = eng_zdepth_snapshot(zslot);
          const u32 w = z->used_w ? z->used_w : z->w, h = z->used_h ? z->used_h : z->h;
          float* px = snap ? (float*)malloc((size_t)w * h * 4u) : NULL;
          if (px) {
              g.be->readback(g.be->user, snap, 0, 0, w, h, px, w * 4u);
              double mn = 2; u64 far_ = 0; for (u64 i = 0; i < (u64)w * h; i++) { if (px[i] < mn) mn = px[i]; if (px[i] >= 0.999999f) far_++; }
              fprintf(stderr, "[zprobe] frame %u 0x%08X n=%u depth t%u w%u f%X cull %u: min %.5f far %.1f%% (had_write %d)%c", g.frames, z->offset, n_draw,
                      rs.depth_test, rs.depth_write, rs.depth_func, rs.cull_enable, mn, 100.0 * far_ / ((double)w * h), z->had_write, 10);
              free(px);
          } else fprintf(stderr, "[zprobe] frame %u 0x%08X n=%u: no snapshot (had_write %d)%c", g.frames, z->offset, n_draw, z->had_write, 10);
      } }
    /* RSX_ENG_PPM_AT=<hex trigger target>:<hex surface>[,<hex surface>...]:<dir>
     * at the first draw into the trigger target in one frame per
     * RSX_ENG_PPM_EVERY (1200), each surface at those addresses as an image --
     * what a pass sees in the buffers it reads. Trigger FFFFFFFF: any 1280-wide
     * target (the display buffers, drawn once the 3D frame is complete). */
    { static int on = -1; static u32 trig, surfs[8], nsurf, last = ~0u, every = 1200; static char dir[400];
      if (on < 0) { const char* e = getenv("RSX_ENG_PPM_AT"); char list[200] = { 0 };
                    on = e && sscanf(e, "%x:%199[0-9A-Fa-fx,]:%399s", &trig, list, dir) == 3;
                    for (char* p = list; on && *p && nsurf < 8; ) { char* q; surfs[nsurf++] = (u32)strtoul(p, &q, 16); p = *q == ',' ? q + 1 : q; if (q == p && *p) break; }
                    const char* ev = getenv("RSX_ENG_PPM_EVERY"); if (ev && atol(ev) > 0) every = (u32)atol(ev); }
      if (on && (trig == 0xFFFFFFFFu ? g.surfaces[target].w == 1280u : g.surfaces[target].offset == trig) &&
          g.frames % every == 0 && last != g.frames && g.be->readback) {
          last = g.frames;
          for (u32 i = 0; i < g.n_surfaces; i++) {
              int want = 0; for (u32 k = 0; k < nsurf; k++) if (g.surfaces[i].offset == surfs[k]) want = 1;
              const u32 surf = g.surfaces[i].offset;
              if (!want || !g.surfaces[i].handle) continue;
              const u32 w = g.surfaces[i].w, h = g.surfaces[i].h;
              u8* img = (u8*)malloc((size_t)w * h * 4u);
              if (!img) break;
              g.be->readback(g.be->user, g.surfaces[i].handle, 0, 0, w, h, img, w * 4u);
              char path[600]; snprintf(path, sizeof path, "%s/at_%u_%08X_%u.ppm", dir, g.frames, surf, i);
              FILE* f = fopen(path, "wb");
              if (f) { fprintf(f, "P6 %u %u 255%c", w, h, 10);
                       for (size_t k = 0; k < (size_t)w * h; k++) fwrite(img + k * 4u, 1, 3, f);
                       fclose(f); }
              snprintf(path, sizeof path, "%s/at_%u_%08X_%u_alpha.pgm", dir, g.frames, surf, i);
              f = fopen(path, "wb");
              if (f) { fprintf(f, "P5 %u %u 255%c", w, h, 10);
                       for (size_t k = 0; k < (size_t)w * h; k++) fputc(img[k * 4u + 3], f);
                       fclose(f); }
              free(img);
          }
      } }
    /* RSX_ENG_PPM_AT2: a second, independent RSX_ENG_PPM_AT (same syntax, files
     * b_*): two moments of one frame, e.g. a pass's output and the final scene. */
    { static int on = -1; static u32 trig, surfs[8], nsurf, last = ~0u, every = 1200; static char dir[400];
      if (on < 0) { const char* e = getenv("RSX_ENG_PPM_AT2"); char list[200] = { 0 };
                    on = e && sscanf(e, "%x:%199[0-9A-Fa-fx,]:%399s", &trig, list, dir) == 3;
                    for (char* p = list; on && *p && nsurf < 8; ) { char* q; surfs[nsurf++] = (u32)strtoul(p, &q, 16); p = *q == ',' ? q + 1 : q; if (q == p && *p) break; }
                    const char* ev = getenv("RSX_ENG_PPM_EVERY"); if (ev && atol(ev) > 0) every = (u32)atol(ev); }
      if (on && (trig == 0xFFFFFFFFu ? g.surfaces[target].w == 1280u : g.surfaces[target].offset == trig) &&
          g.frames % every == 0 && last != g.frames && g.be->readback) {
          last = g.frames;
          for (u32 i = 0; i < g.n_surfaces; i++) {
              int want = 0; for (u32 k = 0; k < nsurf; k++) if (g.surfaces[i].offset == surfs[k]) want = 1;
              const u32 surf = g.surfaces[i].offset;
              if (!want || !g.surfaces[i].handle) continue;
              const u32 w = g.surfaces[i].w, h = g.surfaces[i].h;
              u8* img = (u8*)malloc((size_t)w * h * 4u);
              if (!img) break;
              g.be->readback(g.be->user, g.surfaces[i].handle, 0, 0, w, h, img, w * 4u);
              char path[600]; snprintf(path, sizeof path, "%s/b_%u_%08X_%u.ppm", dir, g.frames, surf, i);
              FILE* f = fopen(path, "wb");
              if (f) { fprintf(f, "P6 %u %u 255%c", w, h, 10);
                       for (size_t k = 0; k < (size_t)w * h; k++) fwrite(img + k * 4u, 1, 3, f);
                       fclose(f); }
              snprintf(path, sizeof path, "%s/b_%u_%08X_%u_alpha.pgm", dir, g.frames, surf, i);
              f = fopen(path, "wb");
              if (f) { fprintf(f, "P5 %u %u 255%c", w, h, 10);
                       for (size_t k = 0; k < (size_t)w * h; k++) fputc(img[k * 4u + 3], f);
                       fclose(f); }
              free(img);
          }
      } }
    /* RSX_ENG_PROBE=<hex target>:<x>:<y>[:<frame period>]: after every draw
     * into that target in one frame per period, its pixel (x, y) read back. */
    /* Optional :<from>:<to>: only frames in that range (a cutscene that
     * always plays at the same frame after the level loads). */
    { static int on = -1; static u32 pt, px_, py_, per = 600, pfrom = 0, pto = 0xFFFFFFFFu;
      if (on < 0) { const char* e = getenv("RSX_ENG_PROBE");
                    on = e && sscanf(e, "%x:%u:%u:%u:%u:%u", &pt, &px_, &py_, &per, &pfrom, &pto) >= 3; if (!per) per = 600; }
      /* RSX_ENG_PROBE_FILE=<file>: "x y [hex target]" written there while the
       * game runs probes that pixel in the next frames (the file is emptied
       * once read) -- a pixel picked from a screenshot of the same run, when
       * the scene does not repeat between runs. "s x y": a SCREEN pixel
       * (display size), scaled into each target -- the scene renders at
       * 960x704 2x-MSAA (1920x704) and its masks at 960x704, so one x,y is a
       * different place in each. */
      static int pscreen; static u32 psx, psy;
      { static const char* pfile = (const char*)-1; static u32 last_chk = ~0u;
        if (pfile == (const char*)-1) pfile = getenv("RSX_ENG_PROBE_FILE");
        if (pfile && g.frames != last_chk && g.frames % 15u == 0u) {
            last_chk = g.frames;
            FILE* f = fopen(pfile, "rb");
            if (f) {
                u32 x = 0, y = 0, t = 0; char head[8] = { 0 };
                int n = fscanf(f, "%7s", head);
                pscreen = n == 1 && head[0] == 's';
                if (pscreen) n = fscanf(f, "%u %u %x", &x, &y, &t);
                else if (n == 1) n = sscanf(head, "%u", &x) == 1 ? 1 + fscanf(f, "%u %x", &y, &t) : 0;
                fclose(f);
                if (n >= 2) {
                    psx = x; psy = y;
                    px_ = x; py_ = y; pt = n >= 3 ? t : 0u; on = 1; per = 1; pfrom = g.frames + 1u; pto = g.frames + 4u;   /* a few presents: not all carry the scene */
                    f = fopen(pfile, "wb"); if (f) fclose(f);
                    fprintf(stderr, "[probe] file: pixel (%u,%u) target %08X in frames %u-%u%c", x, y, pt, pfrom, pto, 10);
                }
            }
        } }
      if (pscreen && g.width && g.height) {
          px_ = (u32)((u64)psx * g.surfaces[target].w / g.width);
          py_ = (u32)((u64)psy * g.surfaces[target].h / g.height);
      }
      if (on && (pt ? g.surfaces[target].offset == pt : (pscreen ? g.surfaces[target].w >= 480u : g.surfaces[target].w >= 900u)) && g.frames % per == 0 && g.frames >= pfrom && g.frames <= pto && g.be->readback &&
          px_ < g.surfaces[target].w && py_ < g.surfaces[target].h) {
          u8 pv[4] = { 0, 0, 0, 0 };
          g.be->readback(g.be->user, g.surfaces[target].handle, px_, py_, 1, 1, pv, 4);
          fprintf(stderr, "[probe] frame %u prim %u n=%u stencil %u -> (%u,%u) = %u %u %u %u pipe %016llx blend %u %X/%X tgt %08X %ux%u%c", g.frames, (unsigned)prim, n_draw,
                  rs.stencil_enable, px_, py_, pv[0], pv[1], pv[2], pv[3], (unsigned long long)s_last_pipe_key,
                  rs.blend_enable, rs.sf_rgb, rs.df_rgb, g.surfaces[target].offset, g.surfaces[target].w, g.surfaces[target].h, 10);
          /* ... and the bound depth buffer's snapshot and as-colour bytes there. */
          if (zslot != ENG_INVALID) {
              eng_zdepth* z = &g.zdepths[zslot];
              float zf = -1.0f; u8 ac[4] = { 0, 0, 0, 0 };
              /* a fresh snapshot: the kept one is the last pass's copy, not this draw's depth */
              { const u32 snap = eng_zdepth_snapshot(zslot);
                if (snap) g.be->readback(g.be->user, snap, px_, py_, 1, 1, &zf, 4);
                else if (z->snapshot) g.be->readback(g.be->user, z->snapshot, px_, py_, 1, 1, &zf, 4); }
              if (z->as_color) g.be->readback(g.be->user, z->as_color, px_, py_, 1, 1, ac, 4);
              fprintf(stderr, "[probe]   zeta 0x%08X snapshot %.7f (x2^24 %.0f) as-colour RGBA %u %u %u %u -> A,R,G %u%c",
                      z->offset, zf, zf * 16777215.0f, ac[0], ac[1], ac[2], ac[3],
                      ((u32)ac[3] << 16) | ((u32)ac[0] << 8) | ac[1], 10);
          }
          /* RSX_ENG_PROBE_UNITS=1: a 24x48 block around (x, y) too; every draw
           * that changes it prints the block's mean colour, its texture units
           * (address, format, remap, where the engine bound each from) and
           * fc0..fc11 -- what a character's colour is made of, frame by frame. */
          { static int pu = -1; static u8 prev[24 * 48 * 4]; static u32 prev_frame = ~0u;
            if (pu < 0) pu = getenv("RSX_ENG_PROBE_UNITS") ? 1 : 0;
            const u32 bw = 24u, bh = 48u, bx = px_ >= bw / 2u ? px_ - bw / 2u : 0u, by = py_ >= bh / 2u ? py_ - bh / 2u : 0u;
            if (pu && bx + bw <= g.surfaces[target].w && by + bh <= g.surfaces[target].h) {
                u8 cur[24 * 48 * 4];
                if (!g.be->readback(g.be->user, g.surfaces[target].handle, bx, by, bw, bh, cur, bw * 4u)) {
                    u32 changed = 0; u64 sr = 0, sg = 0, sb = 0;
                    for (u32 k = 0; k < bw * bh; k++) {
                        const int d = abs((int)cur[k * 4] - (int)prev[k * 4]) + abs((int)cur[k * 4 + 1] - (int)prev[k * 4 + 1]) +
                                      abs((int)cur[k * 4 + 2] - (int)prev[k * 4 + 2]);
                        changed += d > 24; sr += cur[k * 4]; sg += cur[k * 4 + 1]; sb += cur[k * 4 + 2];
                    }
                    if (prev_frame != g.frames || changed >= 8) {
                        fprintf(stderr, "[probe-units] frame %u pipe %016llx: %u px changed, block mean RGB %llu %llu %llu%c", g.frames,
                                (unsigned long long)s_last_pipe_key, changed, (unsigned long long)(sr / (bw * bh)),
                                (unsigned long long)(sg / (bw * bh)), (unsigned long long)(sb / (bw * bh)), 10);
                        for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
                            if (!(tex_mask & (1u << u))) continue;
                            rsx_dsp_texture tt; rsx_dsp_get_texture(&g.rsx, u, &tt);
                            fprintf(stderr, "[probe-units]   tex%u %u:%08X %ux%u fmt 0x%X remap %04X <- %s%c", u, tt.location, tt.offset,
                                    tt.width, tt.height, tt.format, tt.remap & 0xFFFFu,
                                    s_tex_src[u] && *s_tex_src[u] ? s_tex_src[u] : "?", 10);
                        }
                        for (u32 c = 0; c < g.fp_constants.count && c < 12; c++) {
                            const float* fc = (const float*)g.fp_constants.values + c * 4u;
                            fprintf(stderr, "[probe-units]   fc%u %g %g %g %g%c", c, fc[0], fc[1], fc[2], fc[3], 10);
                        }
                    }
                    memcpy(prev, cur, sizeof cur); prev_frame = g.frames;
                }
            } }
          /* RSX_ENG_PROBE_RECT=x,y,w,h: that block too, one character per 1x3
           * pixels ('#' dark, '+' mid, '.' light) -- a shape, not a point. */
          { static int rr = -1; static u32 rx, ry, rw, rh;
            if (rr < 0) { const char* e = getenv("RSX_ENG_PROBE_RECT"); rr = e && sscanf(e, "%u,%u,%u,%u", &rx, &ry, &rw, &rh) == 4; }
            if (rr && rx + rw <= g.surfaces[target].w && ry + rh <= g.surfaces[target].h) {
                u8* blk = (u8*)malloc((size_t)rw * rh * 4u);
                if (blk) {
                    g.be->readback(g.be->user, g.surfaces[target].handle, rx, ry, rw, rh, blk, rw * 4u);
                    for (u32 y = 0; y < rh; y += 3) {
                        char line[512]; u32 n = 0;
                        for (u32 x = 0; x < rw && n < sizeof line - 1; x++) {
                            const u8 v = blk[((size_t)y * rw + x) * 4u];
                            line[n++] = v < 64 ? '#' : v < 192 ? '+' : '.';
                        }
                        line[n] = 0;
                        fprintf(stderr, "[probe-rect] %s%c", line, 10);
                    }
                    free(blk);
                } } }
          /* RSX_ENG_PROBE_PPM=<dir>: the whole target as an image after each
           * probed draw (pr_<frame>_<n>.ppm, R G B), to see a buffer's content. */
          { static const char* pd = (const char*)-1; static u32 pf = ~0u, pn = 0;
            if (pd == (const char*)-1) pd = getenv("RSX_ENG_PROBE_PPM");
            static long pmin = -1;
            if (pmin < 0) { const char* e = getenv("RSX_ENG_PROBE_PPM_MIN"); pmin = e ? atol(e) : 0; }
            if (pf != g.frames) { pf = g.frames; pn = 0; }
            if (pd && *pd && (long)(pn++) >= pmin) {
                pn--;
                const u32 w = g.surfaces[target].w, h = g.surfaces[target].h;
                u8* img = (u8*)malloc((size_t)w * h * 4u);
                if (img) {
                    g.be->readback(g.be->user, g.surfaces[target].handle, 0, 0, w, h, img, w * 4u);
                    char path[600]; snprintf(path, sizeof path, "%s/pr_%u_%u.ppm", pd, g.frames, pn++);
                    FILE* f = fopen(path, "wb");
                    if (f) { fprintf(f, "P6 %u %u 255%c", w, h, 10);
                             for (size_t i = 0; i < (size_t)w * h; i++) fwrite(img + i * 4u, 1, 3, f);
                             fclose(f); }
                    free(img);
                } } }
      } }
    }
    /* Counted only when the draw ran the guest's OWN programs, which is what
     * the test hook means: a draw through the built-in pair is a draw, not
     * evidence that the decompile-translate-compile path worked. */
    if (!pipeline_is_fixed) g.guest_draws++;

    if (zslot != ENG_INVALID && rs.depth_test && rs.depth_write) {
        if (!g.zdepths[zslot].had_write) g.bind_epoch++;
        g.zdepths[zslot].had_write = 1;
        g.zdepths[zslot].last_write = ++g.surf_serial;
        g.zdepths[zslot].ww_epoch = rsx_ww_epoch();
        /* Later readers must see this pass, not a copy taken before it. */
        g.zdepths[zslot].snapshot_valid = 0;
        g.zdepths[zslot].as_color_valid = 0;
    }
    if (rs.color_mask)
        for (u32 i = 0; i < n_targets; i++) {
            if (!g.surfaces[targets[i]].color_written) g.bind_epoch++;
            g.surfaces[targets[i]].color_written = 1;
            g.surfaces[targets[i]].last_write = ++g.surf_serial;
            g.surfaces[targets[i]].ww_epoch = rsx_ww_epoch();
        }
    /* Its own write does not outdate the copy it read (eng_snap_reuse). */
    for (u32 k = 0; k < s_n_own_read; k++) {
        eng_surface* sf = &g.surfaces[s_own_read[k]];
        if (sf->own_snap) sf->own_snap_at = sf->last_write;
    }
    s_n_own_read = 0;
}

static void sink_clear(void* user, const rsx_dispatch* r, u32 mask)
{
    (void)user; (void)r;
    if (!g.ready) return;
    u32 targets[RSX_BE_MAX_COLOR_TARGETS];
    const u32 n_targets = eng_current_target_set(targets);
    { static int cl = -1; if (cl < 0) cl = getenv("RSX_ENG_CLEAR_LOG") ? 1 : 0;
      if (cl && s_clearlog_from != ~0u && g.frames >= s_clearlog_from && g.frames < s_clearlog_from + 2u) {
          rsx_dsp_surface sf_; rsx_dsp_get_surface(&g.rsx, &sf_);
          fprintf(stderr, "[clear] frame %u mask %X colour %08X zs %08X tgt %08X (%u targets, %ux%u) zeta %u:%08X clip %ux%u%c",
                  g.frames, mask, rsx_dsp_clear_color(&g.rsx), rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR),
                  n_targets ? g.surfaces[targets[0]].offset : 0u, n_targets,
                  n_targets ? g.surfaces[targets[0]].w : 0u, n_targets ? g.surfaces[targets[0]].h : 0u,
                  sf_.zeta_location, sf_.zeta_offset, sf_.clip_w, sf_.clip_h, 10); } }
    if (!n_targets) return;

    if (mask & (RSX_CLEAR_COLOR_R | RSX_CLEAR_COLOR_G |
                RSX_CLEAR_COLOR_B | RSX_CLEAR_COLOR_A)) {
        const u32 c = rsx_dsp_clear_color(&g.rsx);
        const float rgba[4] = {
            (float)((c >> 16) & 0xFF) / 255.0f,
            (float)((c >>  8) & 0xFF) / 255.0f,
            (float)( c        & 0xFF) / 255.0f,
            (float)((c >> 24) & 0xFF) / 255.0f,
        };
        /* Every target the set names, not only A: a deferred pass clears its
         * whole G-buffer in one CLEAR_SURFACE, and B, C and D would otherwise
         * keep whatever the last frame left in them. */
        for (u32 i = 0; i < n_targets; i++) {
            g.be->clear_color(g.be->user, g.surfaces[targets[i]].handle, rgba);
            g.surfaces[targets[i]].last_write = ++g.surf_serial;
            g.surfaces[targets[i]].ww_epoch = rsx_ww_epoch();   /* eng_surface_overwritten */
        }
    }
    if (mask & (RSX_CLEAR_DEPTH | RSX_CLEAR_STENCIL)) {
        rsx_dsp_surface sf;
        rsx_dsp_get_surface(&g.rsx, &sf);
        const u32 zslot = eng_zdepth_get(sf.zeta_location, sf.zeta_offset,
                                         sf.clip_w * eng_aa_scale(), sf.clip_h * eng_aa_scale());
        if (zslot == ENG_INVALID) return;
        g.zdepths[zslot].bpp = sf.depth_format == 1 ? 2u : 4u;
        u32 flags = 0;
        if (mask & RSX_CLEAR_DEPTH)   flags |= RSX_BE_CLEAR_DEPTH;
        if (mask & RSX_CLEAR_STENCIL) flags |= RSX_BE_CLEAR_STENCIL;
        /* ZSTENCIL_CLEAR_VALUE is Z24S8: depth24 << 8 | stencil8, with the
         * nv40 reset 0xFFFFFF00 seeded by rsx_dispatch_init, so a stream that
         * never writes it still clears to 1.0 / 0. Read so for Z16 too, not
         * as its low 16 bits (RPCS3's clear_z): A Crack in Time clears its
         * 128x128 Z16 shadow maps with 0xFFFFFF00 (logged below), which the
         * low half makes 0.9961 -- and with the shadow compare's reference
         * clamped to 1.0, the ground past each caster's far plane would then
         * compare as shadowed: the solid slab the clamp exists to prevent. */
        const u32 zs = rsx_dsp_reg(&g.rsx, M_ZSTENCIL_CLEAR);
        const float zc = zs ? (float)(zs >> 8) / 16777215.0f : 1.0f;
        { static u32 seen[16], n_seen;
          const u32 key = (zs & 0xFFFFFFF0u) ^ (sf.depth_format & 7u);
          u32 k = 0;
          while (k < n_seen && seen[k] != key) k++;
          if (k == n_seen && n_seen < 16u) { seen[n_seen++] = key;
              fprintf(stderr, "[rsx engine] depth clear: %s zeta %u:0x%08X %ux%u, value 0x%08X -> %.6f%c",
                      sf.depth_format == 1 ? "Z16" : "Z24S8", sf.zeta_location, sf.zeta_offset,
                      sf.clip_w, sf.clip_h, zs, zc, 10); } }
        g.be->clear_depth_stencil(g.be->user, g.zdepths[zslot].handle, flags, zc,
                                  (u8)(zs & 0xFFu));
        g.zdepths[zslot].cleared = 1;
        if (mask & RSX_CLEAR_DEPTH) {
            /* A clear invalidates the older published depth image; the next
             * texture consumer resolves the newly written contents exactly
             * once. And it IS a write: on the RSX it fills the buffer's memory,
             * and a depth read of that address sees the far plane. A Crack in
             * Time sets up a 128x128 shadow map it may draw nothing into --
             * colour twin at the same address, colour writes off -- and its
             * light pass still samples it; counting only depth-writing draws
             * sent that read to the never-written colour twin (zero = nearest),
             * and the light's whole volume came out shadowed: the dark square
             * under Ratchet. */
            if (!g.zdepths[zslot].had_write) g.bind_epoch++;
            g.zdepths[zslot].had_write = 1;
            g.zdepths[zslot].last_write = ++g.surf_serial;
            g.zdepths[zslot].ww_epoch = rsx_ww_epoch();
            g.zdepths[zslot].snapshot_valid = 0;
            g.zdepths[zslot].as_color_valid = 0;
        }
    }
}

static int s_surface_override = -1; // -1 = auto, 0 = disp, 1 = scene (0x00880000), 2 = ui (0x013C0000), 3 = ram (0x00030000)
static volatile int s_dump_requested = 0;

void rsx_draw_engine_dump_now(void)
{
    s_dump_requested = 1;
}

void rsx_draw_engine_cycle_surface_override(void)
{
    s_surface_override++;
    if (s_surface_override > 3) s_surface_override = -1;
    const char* names[] = {
        "Auto (Display Buffer with Scene fallback)",
        "Force Display Buffer (0x00000000 / 0x00398000)",
        "Force 3D Scene Target (0x00880000)",
        "Force UI / Composite Target (0x013C0000)",
        "Force Host RAM Target (0x00030000)"
    };
    fprintf(stderr, "\n[rsx override] F5 PRESSED -> Presentation Mode: %s (mode %d)\n\n",
            names[s_surface_override + 1], s_surface_override);
}

static void eng_dump_surface_bmp(const char* path, u32 handle, u32 w, u32 h)
{
    if (!g.be->readback || !handle || !w || !h) return;
    u8* px = (u8*)malloc((size_t)w * h * 4u);
    if (!px) return;
    if (g.be->readback(g.be->user, handle, 0, 0, w, h, px, w * 4u) == 0) {
        u64 non_black = 0;
        u64 sum_r = 0, sum_g = 0, sum_b = 0;
        const u64 total_px = (u64)w * h;
        for (u64 i = 0; i < total_px; i++) {
            const u8 r = px[i * 4 + 0];
            const u8 g_c = px[i * 4 + 1];
            const u8 b = px[i * 4 + 2];
            if (r > 4 || g_c > 4 || b > 4) non_black++;
            sum_r += r; sum_g += g_c; sum_b += b;
        }
        FILE* f = fopen(path, "wb");
        if (f) {
            unsigned char bmp_hdr[54] = {
                'B', 'M',           // bfType
                0, 0, 0, 0,         // bfSize (filled below)
                0, 0, 0, 0,         // reserved
                54, 0, 0, 0,        // bfOffBits (54)
                40, 0, 0, 0,        // biSize (40)
                0, 0, 0, 0,         // biWidth (filled below)
                0, 0, 0, 0,         // biHeight (filled below)
                1, 0,               // biPlanes (1)
                32, 0,              // biBitCount (32 bpp)
                0, 0, 0, 0,         // biCompression (0 = BI_RGB)
                0, 0, 0, 0,         // biSizeImage (filled below)
                0, 0, 0, 0,         // biXPelsPerMeter
                0, 0, 0, 0,         // biYPelsPerMeter
                0, 0, 0, 0,         // biClrUsed
                0, 0, 0, 0          // biClrImportant
            };
            uint32_t file_size = 54 + w * h * 4;
            uint32_t img_size = w * h * 4;
            int32_t bw = (int32_t)w;
            int32_t bh = -(int32_t)h; // Negative height for top-down image
            memcpy(bmp_hdr + 2, &file_size, 4);
            memcpy(bmp_hdr + 18, &bw, 4);
            memcpy(bmp_hdr + 22, &bh, 4);
            memcpy(bmp_hdr + 34, &img_size, 4);
            fwrite(bmp_hdr, 1, 54, f);
            u8* row_buf = (u8*)malloc(w * 4u);
            if (row_buf) {
                for (u32 y = 0; y < h; y++) {
                    const u8* src = px + y * w * 4;
                    for (u32 x = 0; x < w; x++) {
                        row_buf[x * 4 + 0] = src[x * 4 + 2]; // B
                        row_buf[x * 4 + 1] = src[x * 4 + 1]; // G
                        row_buf[x * 4 + 2] = src[x * 4 + 0]; // R
                        row_buf[x * 4 + 3] = src[x * 4 + 3]; // A
                    }
                    fwrite(row_buf, 1, w * 4, f);
                }
                free(row_buf);
            }
            fclose(f);
            fprintf(stderr, "[rsx dump] %s: non_black=%.1f%% avg_rgb=(%u,%u,%u)\n",
                    path, (double)non_black * 100.0 / (double)total_px,
                    (u32)(sum_r / total_px), (u32)(sum_g / total_px), (u32)(sum_b / total_px));
        }
    }
    free(px);
}

static void eng_dump_all_surfaces(u32 frame)
{
    if (!g.ready || !g.be->readback) return;
    mkdir("dumps", 0755);
    fprintf(stderr, "[rsx dump] ================= DUMPING SURFACES AT FRAME %u =================\n", frame);
    for (u32 i = 0; i < g.n_surfaces; i++) {
        eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->w < 640 || s->h < 360) continue;
        char path[256];
        snprintf(path, sizeof(path), "dumps/f%04u_surf%u_off%08X_%ux%u.bmp",
                 frame, i, s->offset, s->w, s->h);
        eng_dump_surface_bmp(path, s->handle, s->w, s->h);
    }
    fprintf(stderr, "[rsx dump] ================================================================\n");
}

/* Resolve a flip's buffer id to a REGISTERED surface rather than whatever is
 * currently bound: the live target at that instant is often an offscreen
 * shadow or post-process surface, and copying it presents black despite the
 * scene's draws having executed (rsx_live_draw.c:7557-7570). */
static u32 eng_present_surface(u32 buffer_id)
{
    /* If no new draws occurred since the last present call, show the last frame again
     * to prevent double-flip flicker */
    if (g.surf_serial == s_frame_serial && g.last_present_surface < g.n_surfaces &&
        g.surfaces[g.last_present_surface].handle) {
        return g.last_present_surface;
    }

    static int s_env_checked = 0;
    if (!s_env_checked) {
        s_env_checked = 1;
        const char* m = getenv("RSX_PRESENT_MODE");
        if (m) {
            if (!_stricmp(m, "disp")) s_surface_override = 0;
            else if (!_stricmp(m, "scene")) s_surface_override = 1;
            else if (!_stricmp(m, "ui")) s_surface_override = 2;
            else if (!_stricmp(m, "ram")) s_surface_override = 3;
            else s_surface_override = -1;
        }
    }

    if (buffer_id < 8 && !g.display_buffers[buffer_id].valid) {
        extern int cellGcm_display_buffer_info(u32, u32*, u32*, u32*, u32*);
        u32 off, pitch, w, h;
        if (cellGcm_display_buffer_info(buffer_id, &off, &pitch, &w, &h))
            rsx_draw_engine_set_display_buffer(buffer_id, 0, off, pitch, w, h);
    }

    u32 disp_surf = ENG_INVALID;
    if (buffer_id < 8 && g.display_buffers[buffer_id].valid) {
        const eng_display_buffer* d = &g.display_buffers[buffer_id];
        disp_surf = eng_surface_find(d->location, d->offset, d->width, d->height);
    }

    /* 3D scene target in Ratchet & Clank: Into the Nexus is at local offset 0x00880000 (1280x720) */
    u32 scene_surf = ENG_INVALID;
    u32 ui_surf = ENG_INVALID;
    u32 ram_surf = ENG_INVALID;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        if (!g.surfaces[i].handle || g.surfaces[i].w != 1280u || g.surfaces[i].h != 720u) continue;
        if (g.surfaces[i].location == 0 && g.surfaces[i].offset == 0x00880000u) scene_surf = i;
        else if (g.surfaces[i].location == 0 && g.surfaces[i].offset == 0x013C0000u) ui_surf = i;
        else if (g.surfaces[i].location == 1 && g.surfaces[i].offset == 0x00030000u) ram_surf = i;
    }

    u32 chosen = ENG_INVALID;

    // Check manual override (via F5 or RSX_PRESENT_MODE)
    if (s_surface_override == 0 && disp_surf != ENG_INVALID && g.surfaces[disp_surf].handle)
        chosen = disp_surf;
    else if (s_surface_override == 1 && scene_surf != ENG_INVALID && g.surfaces[scene_surf].handle)
        chosen = scene_surf;
    else if (s_surface_override == 2 && ui_surf != ENG_INVALID && g.surfaces[ui_surf].handle)
        chosen = ui_surf;
    else if (s_surface_override == 3 && ram_surf != ENG_INVALID && g.surfaces[ram_surf].handle)
        chosen = ram_surf;

    if (chosen == ENG_INVALID) {
        // Auto presentation mode:
        // 1. Registered display buffer if it received any draws recently
        if (disp_surf != ENG_INVALID && g.surfaces[disp_surf].handle &&
            (g.surf_serial - g.surfaces[disp_surf].last_write) < 100) {
            chosen = disp_surf;
        }
        // 2. 3D Scene target (0x00880000) if recently drawn
        else if (scene_surf != ENG_INVALID && g.surfaces[scene_surf].handle &&
                 (g.surf_serial - g.surfaces[scene_surf].last_write) < 100) {
            chosen = scene_surf;
        }
        // 3. UI/Composite target (0x013C0000) if recently drawn
        else if (ui_surf != ENG_INVALID && g.surfaces[ui_surf].handle &&
                 (g.surf_serial - g.surfaces[ui_surf].last_write) < 100) {
            chosen = ui_surf;
        }
        // 4. Latest written 1280x720 surface
        else {
            u64 best_write = 0;
            for (u32 i = 0; i < g.n_surfaces; i++) {
                if (g.surfaces[i].handle && g.surfaces[i].w == 1280u && g.surfaces[i].h == 720u) {
                    if (g.surfaces[i].last_write > best_write) {
                        best_write = g.surfaces[i].last_write;
                        chosen = i;
                    }
                }
            }
        }
    }

    if (chosen == ENG_INVALID) {
        if (disp_surf != ENG_INVALID && g.surfaces[disp_surf].handle) chosen = disp_surf;
        else if (scene_surf != ENG_INVALID && g.surfaces[scene_surf].handle) chosen = scene_surf;
        else if (g.last_present_surface < g.n_surfaces && g.surfaces[g.last_present_surface].handle) chosen = g.last_present_surface;
        else chosen = eng_current_surface();
    }

    static u32 s_last_log_frame = 0;
    if (g.frames % 120 == 0 && g.frames != s_last_log_frame) {
        s_last_log_frame = g.frames;
        fprintf(stderr, "[rsx present-choice] frame=%u req_buf=%u disp=%u(off=%08X, age=%llu) scene=%u(off=%08X, age=%llu) ui=%u(off=%08X, age=%llu) -> chosen=%u(off=%08X) [override=%d]\n",
                g.frames, buffer_id,
                disp_surf, disp_surf != ENG_INVALID ? g.surfaces[disp_surf].offset : 0,
                disp_surf != ENG_INVALID ? (unsigned long long)(g.surf_serial - g.surfaces[disp_surf].last_write) : 999999ull,
                scene_surf, scene_surf != ENG_INVALID ? g.surfaces[scene_surf].offset : 0,
                scene_surf != ENG_INVALID ? (unsigned long long)(g.surf_serial - g.surfaces[scene_surf].last_write) : 999999ull,
                ui_surf, ui_surf != ENG_INVALID ? g.surfaces[ui_surf].offset : 0,
                ui_surf != ENG_INVALID ? (unsigned long long)(g.surf_serial - g.surfaces[ui_surf].last_write) : 999999ull,
                chosen, chosen != ENG_INVALID ? g.surfaces[chosen].offset : 0,
                s_surface_override);
    }

    return chosen;
}

static void eng_writeback_small(void)
{
    static long maxpx = -1;
    if (maxpx < 0) { const char* e = getenv("RSX_ENG_WRITEBACK"); maxpx = e ? atol(e) : 0; }
    if (maxpx <= 0 || !g.be->readback) return;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->fmt != RSX_BE_FMT_R8G8B8A8 || !s->color_written) continue;
        if ((u64)s->w * s->h > (u64)maxpx || s->last_write <= s_frame_serial) continue;
        const u32 pitch = s->pitch ? s->pitch : s->w * 4u;
        if (pitch < s->w * 4u) continue;
        u8* dst = (u8*)eng_guest_ptr(NULL, s->location, s->offset, pitch * s->h);
        if (!dst) continue;
        u8* px = (u8*)malloc((size_t)s->w * s->h * 4u);
        if (!px) continue;
        if (g.be->readback(g.be->user, s->handle, 0, 0, s->w, s->h, px, s->w * 4u)) { free(px); continue; }
        rsx_ww_self_begin(dst, pitch * s->h);
        for (u32 y = 0; y < s->h; y++)
            for (u32 x = 0; x < s->w; x++) {
                const u8* q = px + ((size_t)y * s->w + x) * 4u;
                u8* d = dst + (size_t)y * pitch + x * 4u;
                d[0] = q[3]; d[1] = q[0]; d[2] = q[1]; d[3] = q[2];   /* RGBA -> A,R,G,B */
            }
        rsx_ww_self_end(dst, pitch * s->h);
        free(px);
        static int logs;
        if (logs < 12) { logs++; fprintf(stderr, "[rsx engine] writeback %u:0x%08X %ux%u pitch %u%c",
                                         s->location, s->offset, s->w, s->h, pitch, 10); }
    }
}

/* ---------------------------------------------------------------------------
 * Memory budgets (once per frame, from eng_present).
 *
 * The texture cache and the target lists used to evict only when their slot
 * counts filled (4096 / 1024 / 512) with no regard for bytes. A level
 * streams new (address, size, format) identities all the time, so memory
 * climbed until the counts were reached -- +140 MB/min private and +160
 * MB/min GPU at the Zolar start. Each cache now has a byte budget and the
 * texture cache a count budget below its slot count; over it, the least
 * recently used entries no draw of the last second used go first.
 * Textures may go further (anything not used this frame): their bytes are
 * still in guest memory. Targets and depth buffers never do, and below
 * twice their budget only lose-nothing ones go (see the tiers below): a
 * depth buffer has no other copy of its contents. A retired colour target
 * writes its pixels back to guest memory (eng_surface_retire), so a later
 * read of it is not noise.
 *   RSX_ENG_TEX_MB (card/6, 640..2048)  RSX_ENG_TEX_MAX (3584)  RSX_ENG_RT_MB (768)
 *   RSX_ENG_Z_MB (768)    RSX_ENG_MEMLOG=1: the budget line every 120 frames
 * Targets and depth buffers count the GPU's bytes: the backend's images are
 * RACACIT_RES_SCALE times the guest's size each way, and its depth format
 * D32_SFLOAT_S8 takes 8 bytes a pixel. v63 and older counted guest pixels at
 * 4 bytes, so at 2.0x a budget let 8x its size through for depth: an Axiom
 * fight went from 0.9 to 5.2 GB of VRAM with the depth line at "721 MB", and
 * the owner's long v60 session ran out (50 failed image allocations, targets
 * missing in the fight).
 * -------------------------------------------------------------------------*/
float    g_rsx_res_scale = 1.0f;   /* set by the backend: its internal-resolution factor */
unsigned g_rsx_vram_budget_mb = 0; /* set by the backend: the card's video memory budget, 0 = unknown */
unsigned g_rsx_depth_bpp = 4;      /* and its depth images' bytes per pixel */
float    g_rsx_shadow_scale = 0.0f;/* set by the backend: its scale for shadow maps, 0 = the scene's */
static u64 eng_scaled(u64 b) { const double s = g_rsx_res_scale; return s == 1.0 ? b : (u64)((double)b * s * s); }

/* The scale a depth target of the guest's w x h is drawn at. A Crack in
 * Time's shadow maps are small power-of-two depth buffers -- 128x128 per
 * character, 256x256 or 512x256 for the sun -- stretched over the ground, and
 * at the scene's scale their texels stay big enough to see as steps however
 * they are filtered: the "boxy shadows". The backend draws those at
 * g_rsx_shadow_scale instead (RACACIT_SHADOW_RES), up to 2048 pixels a side;
 * the scene's depth (1280x720) and the effects pass's (512x352) are not
 * powers of two and keep the scene's scale. Shared with the backend so the
 * budgets below count the bytes it really allocates. */
int rsx_depth_target_is_map(u32 w, u32 h)
{
    return w >= 32u && h >= 32u && w <= 1024u && h <= 1024u && !(w & (w - 1u)) && !(h & (h - 1u));
}
/* The 2048 a side holds at every scene scale: at 4x a 1024x1024 map was 4096x4096,
 * cleared and copied whole however little of it a caster covered. */
float rsx_depth_target_scale(u32 w, u32 h)
{
    const float rs = g_rsx_res_scale;
    if (!rsx_depth_target_is_map(w, h)) return rs;
    const u32 m = w > h ? w : h;
    float s = g_rsx_shadow_scale > rs ? g_rsx_shadow_scale : rs;
    if ((float)m * s > 2048.0f) s = 2048.0f / (float)m;
    return s;
}
static u64 s_frame_serial_hist[64];          /* s_frame_serial at each of the last 64 frames */

static u64 eng_env_mb(const char* name, u64 def)
{
    const char* e = getenv(name);
    const long v = e ? atol(e) : -1;
    return (v > 0 ? (u64)v : def) << 20;
}

static u64 eng_surface_bytes(const eng_surface* s)
{
    if (!s->last_write) return 0;   /* never drawn or cleared: the backend has no image for it */
    const u64 bpp = s->fmt == RSX_BE_FMT_R16G16B16A16F ? 8u : s->fmt == RSX_BE_FMT_R32G32B32A32F ? 16u : 4u;
    /* A target a draw has read while drawing into it also holds a same-size
     * copy (the backend's snapshot, freed with it). Uncounted, a GitHub #4
     * log at 3x had ~5 GB in the caches' books and 14 GB in use when
     * allocations started failing. */
    const u64 b = (u64)s->w * s->h * bpp;
    return eng_scaled(s->own_snap && s->own_snap_of == s->handle ? b * 2u : b);
}

static u64 eng_zdepth_bytes(const eng_zdepth* z)
{
    /* The buffer and its snapshot at the depth target's scale, the colour
     * copies at the scene's. */
    u64 b = (u64)z->w * z->h * (g_rsx_depth_bpp ? g_rsx_depth_bpp : 4u);
    if (z->snapshot) b += (u64)(z->snap_w ? z->snap_w : z->w) * (z->snap_h ? z->snap_h : z->h) * 4u;
    u64 c = 0;
    if (z->as_color) c += (u64)(z->as_color_w ? z->as_color_w : z->w) * (z->as_color_h ? z->as_color_h : z->h) * 4u;
    if (z->as_color_spare) c += (u64)z->spare_w * z->spare_h * 4u;
    const double ds = rsx_depth_target_scale(z->w, z->h);
    return (ds == g_rsx_res_scale ? eng_scaled(b) : (u64)((double)b * ds * ds)) + eng_scaled(c);
}

/* Bytes of guest memory a colour target covers. */
static u32 eng_surface_span(const eng_surface* s)
{
    const u32 bpp = s->fmt == RSX_BE_FMT_R16G16B16A16F ? 8u : s->fmt == RSX_BE_FMT_R32G32B32A32F ? 16u : 4u;
    const u32 pitch = (s->raster == 2 || !s->pitch) ? s->w * bpp : s->pitch;
    return pitch * s->h;
}

/* eng_surface_retire can put its pixels back into guest memory, where a
 * later read decodes them: dropping it loses nothing. */
static int eng_surface_writes_back(const eng_surface* s)
{
    if (!g.be->readback || s->fmt != RSX_BE_FMT_R8G8B8A8) return 0;
    if (!(s->rsx_fmt == 4 || s->rsx_fmt == 5 || s->rsx_fmt == 8)) return 0;
    const int swz = s->raster == 2;
    if (swz && ((s->w & (s->w - 1u)) || (s->h & (s->h - 1u)))) return 0;
    const u32 pitch = swz ? s->w * 4u : (s->pitch ? s->pitch : s->w * 4u);
    return pitch >= s->w * 4u && (u64)pitch * s->h <= (64u << 20);
}

/* Another live target or depth buffer has drawn over this one's memory since
 * this one was last drawn: on the PS3 those bytes now hold that drawing (a
 * shadow pool repacked, a buffer reused at another size), so this copy can
 * never be read correctly again and dropping it loses nothing. */
static int eng_superseded(u32 loc, u32 off, u32 span, u64 last_write, const void* self)
{
    for (u32 j = 0; j < g.n_surfaces; j++) {
        const eng_surface* o = &g.surfaces[j];
        if ((const void*)o == self || !o->handle || o->location != loc || o->last_write <= last_write) continue;
        if (o->offset < off + span && off < o->offset + eng_surface_span(o)) return 1;
    }
    for (u32 j = 0; j < g.n_zdepths; j++) {
        const eng_zdepth* o = &g.zdepths[j];
        if ((const void*)o == self || !o->handle || o->location != loc || o->last_write <= last_write) continue;
        if (o->offset < off + span && off < o->offset + o->w * o->h * 4u) return 1;
    }
    return 0;
}

/* The game has written this target's memory since our last draw into it:
 * an NV0039 copy, a CPU or SPU write over a region it drew into earlier. On
 * the PS3 that memory now holds the new bytes, so a texture read there must
 * decode them, not sample the stale GPU copy. A Crack in Time saves 7.25 MB
 * of VRAM to main memory when the pause menu opens, draws the menu in it and
 * copies the level back afterwards: every level texture whose address a
 * menu target had taken came back as that target's leftover pixels -- the
 * striped "neon" textures that stayed after a Constructo mod pickup. Once
 * per frame per target (a page scan of its span). */
static int eng_surface_overwritten(u32 i)
{
    if (i >= g.n_surfaces) return 0;
    eng_surface* s = &g.surfaces[i];
    /* Any GPU write counts -- a colour clear too (sink_clear stamps ww_epoch):
     * a target the game only cleared is as stale as a drawn one once the
     * game writes its memory. */
    if (!rsx_ww_on() || !s->handle || !s->last_write || !s->ww_epoch) return 0;
    if (s->ww_check_frame == g.frames + 1u) return s->ww_stale;
    s->ww_check_frame = g.frames + 1u;
    u32 span = eng_surface_span(s);
    /* A copy target holds only the rows its blits filled (copy_h, at the
     * game's pitch); the rest of the power-of-two size it was grown to is not
     * its memory. Clank's time / teleport effects copy 300-450 px patches of
     * the scene into 0x0205F100, the target grew to 512x512 = 1 MB of address
     * space, the game wrote its own data past the patch, and every read
     * decoded memory instead: a black box over the effect (owner, v54). */
    if (!s->rsx_fmt && s->copy_h) {
        const u32 cspan = (s->pitch ? s->pitch : s->w * 4u) * s->copy_h;
        if (cspan < span) span = cspan;
    }
    const u8* p = (const u8*)eng_guest_ptr(NULL, s->location, s->offset, span);
    s->ww_stale = (u8)eng_ww_written_inner(p, span, s->ww_epoch);
    if (s->ww_stale) { static u32 logs; if (logs++ < 16)
        fprintf(stderr, "[rsx engine] target %u:0x%08X %ux%u overwritten by the game since its last draw: reads decode memory%c",
                s->location, s->offset, s->w, s->h, 10); }
    return s->ww_stale;
}

/* The guest bytes a depth buffer's last pass covered (its zeta's pixel size;
 * the pool packs its maps at width x bytes-a-pixel pitch). */
static u32 eng_zdepth_span(const eng_zdepth* z)
{
    const u32 w = z->used_w ? z->used_w : z->w, h = z->used_h ? z->used_h : z->h;
    return w * h * (z->bpp ? z->bpp : 4u);
}

/* Shadow maps the backend draws at their own scale (rsx_depth_target_scale),
 * dropped two frames after another depth buffer was drawn over their bytes.
 * The game's shadow pool repacks every frame -- each caster's map lands at a
 * new address and size -- and the dead sweep below frees the old ones after a
 * second idle: at 4x a second of them is a lot of memory. Breaking one crate
 * in the Great Clock took the depth buffers from 13 to 133 (570 MB); the
 * owner's v96 Valkyrie fleet fight reached 288 (over 1.5 GB), past the
 * budget: 69 ms budget passes and the scene's own depth buffers dropped.
 * On the PS3 a map drawn over is gone, so nothing correct can read one. Their
 * images go to the backend's recycle bin for the next map of their size. */
static u32 eng_zdepth_shadow_sweep(u64 idle_since)
{
    static u8 drop[ENG_MAX_ZDEPTHS];
    u32 ev = 0;
    for (u32 i = 0; i < g.n_zdepths; i++) {
        drop[i] = 0;
        eng_zdepth* z = &g.zdepths[i];
        if (!z->handle || z->last_use > idle_since || !rsx_depth_target_is_map(z->w, z->h)) continue;
        int dead = !z->had_write;
        const u32 a0 = z->offset, a1 = a0 + eng_zdepth_span(z);
        for (u32 j = 0; j < g.n_zdepths && !dead; j++) {
            const eng_zdepth* o = &g.zdepths[j];
            if (j != i && o->handle && o->location == z->location && o->last_write > z->last_write &&
                o->offset < a1 && a0 < o->offset + eng_zdepth_span(o)) dead = 1;
        }
        if (!dead) continue;
        g.be->depth_target_release(g.be->user, z->handle);
        if (z->as_color) g.be->color_target_release(g.be->user, z->as_color);
        if (z->as_color_spare) g.be->color_target_release(g.be->user, z->as_color_spare);
        drop[i] = 1; ev++;
    }
    if (!ev) return 0;
    u32 j = 0;
    for (u32 i = 0; i < g.n_zdepths; i++) if (!drop[i]) { if (j != i) g.zdepths[j] = g.zdepths[i]; j++; }
    memset(&g.zdepths[j], 0, (size_t)(g.n_zdepths - j) * sizeof g.zdepths[0]);
    g.n_zdepths = j;
    g.addr_dirty = 1;
    g.bind_epoch++;
    return ev;
}

typedef struct { u64 key; u32 idx; } eng_lru_ent;
static int eng_lru_cmp(const void* a, const void* b)
{
    const u64 x = ((const eng_lru_ent*)a)->key, y = ((const eng_lru_ent*)b)->key;
    return x < y ? -1 : x > y;
}

/* Set by a backend whose video memory ran out (an image allocation failed
 * even after waiting for the GPU; rsx_vulkan_backend.c). The budgets are
 * fixed sizes, not the card's memory: at a raised internal resolution the
 * targets alone may grow past what it has. The next budget pass then evicts
 * below the budgets, and only what costs nothing to bring back -- textures
 * (decoded again from guest memory) and targets that are stale or written
 * back. */
volatile int g_rsx_vram_pressure;

static void eng_budget_enforce(void)
{
    static u64 tex_budget, rt_budget, z_budget; static u32 tex_max; static int memlog = -1;
    if (memlog < 0) {
        /* Textures: a sixth of the card's video memory, 640 MB to 2 GB (640
         * when unknown -- the old fixed size). Textures are not scaled by the
         * internal resolution, so on a big card they were the cache that ran
         * out: the owner's v93 Great Clock D session filled 640 MB in ~6
         * minutes and evicted 2,122 textures by minute 9, each one decoded
         * and uploaded again when seen next, with ~4 GB of a 7.2 GB card
         * free. RSX_ENG_TEX_MB still sets it outright. */
        { u64 def = 640;
          if (g_rsx_vram_budget_mb) {
              def = g_rsx_vram_budget_mb / 6u;
              if (def < 640) def = 640;
              if (def > 2048) def = 2048;
          }
          tex_budget = eng_env_mb("RSX_ENG_TEX_MB", def);
          fprintf(stderr, "[rsx mem] texture budget %llu MB (card budget %u MB)%c",
                  (unsigned long long)(tex_budget >> 20), g_rsx_vram_budget_mb, 10); }
        /* Real bytes since v64 (eng_scaled): a higher internal resolution
         * gets a bigger budget, by the scale (not its square) -- 1.5 GB each
         * at 2.0x, so a fight's targets are not evicted (and written back,
         * a GPU wait each) all the time, and VRAM stays near 4 GB. */
        { const double sc = g_rsx_res_scale > 1.0f ? g_rsx_res_scale : 1.0;
          rt_budget = (u64)((double)eng_env_mb("RSX_ENG_RT_MB", 768) * sc);
          z_budget = (u64)((double)eng_env_mb("RSX_ENG_Z_MB", 768) * sc); }
        { const char* e = getenv("RSX_ENG_TEX_MAX"); const long v = e ? atol(e) : 0;
          tex_max = v > 0 && v <= ENG_MAX_TEXTURES ? (u32)v : ENG_MAX_TEXTURES / 8u * 7u; }
        memlog = getenv("RSX_ENG_MEMLOG") ? 1 : 0;
    }
    /* eng_present has already moved s_frame_serial to this frame's end; the
     * frame began at the previous entry. Comparing against s_frame_serial
     * excluded nothing: an old second pass evicted targets and depth
     * buffers the frame had just drawn with. */
    const u64 frame_start = s_frame_serial_hist[(g.frames - 1u) & 63u];
    s_frame_serial_hist[g.frames & 63u] = s_frame_serial;
    /* A draw of the last ~second used it after this serial. */
    const u64 recent = s_frame_serial_hist[(g.frames - 60u) & 63u];
    static eng_lru_ent lru[ENG_MAX_TEXTURES > ENG_MAX_SURFACES ? ENG_MAX_TEXTURES : ENG_MAX_SURFACES];
    static unsigned long long n_tex_ev, n_rt_ev, n_z_ev;

    const int pressure = g_rsx_vram_pressure;
    if (pressure) {
        g_rsx_vram_pressure = 0;
        static u32 logs;
        if (logs < 16) { logs++; fprintf(stderr, "[rsx mem] frame %u: video memory ran out: evicting below the budgets%c", g.frames, 10); }
    }

    /* Textures. */
    u64 tex_bytes = 0;
    for (u32 i = 0; i < g.n_textures; i++) if (g.textures[i].handle) tex_bytes += g.textures[i].span;
    if ((tex_bytes > tex_budget || g.n_textures > tex_max || pressure) && g.n_textures) {
        const u64 tgt_b = pressure ? (tex_bytes < tex_budget ? tex_bytes : tex_budget) / 4u * 3u
                                   : tex_budget / 10u * 9u;
        const u32 tgt_n = tex_max / 10u * 9u;
        for (int pass = 0; pass < 2 && (tex_bytes > tgt_b || g.n_textures > tgt_n); pass++) {
            u32 n = 0;
            for (u32 i = 0; i < g.n_textures; i++) {
                const eng_texture* e = &g.textures[i];
                if (e->last_use_frame == g.frames) continue;
                if (pass == 0 && e->last_use_frame + 60u > g.frames) continue;
                lru[n].key = e->last_use_serial; lru[n].idx = i; n++;
            }
            qsort(lru, n, sizeof lru[0], eng_lru_cmp);
            static u8 drop[ENG_MAX_TEXTURES];
            memset(drop, 0, sizeof drop);
            u32 left = g.n_textures, k = 0;
            for (; k < n && (tex_bytes > tgt_b || left > tgt_n); k++) {
                eng_texture* e = &g.textures[lru[k].idx];
                eng_tex_gone(e);
                if (e->handle) { g.be->texture_release(g.be->user, e->handle); tex_bytes -= e->span; }
                drop[lru[k].idx] = 1; left--;
            }
            if (!k) continue;
            u32 j = 0;
            for (u32 i = 0; i < g.n_textures; i++) if (!drop[i]) { if (j != i) g.textures[j] = g.textures[i]; j++; }
            /* The vacated tail still holds copies of entries that moved
             * down; an append must find empty slots there. */
            memset(&g.textures[j], 0, (size_t)(g.n_textures - j) * sizeof g.textures[0]);
            g.n_textures = j;
            n_tex_ev += k;
            eng_tex_index_rebuild();
            g.bind_epoch++;
        }
    }

    /* Colour targets and depth buffers: only what no draw of the last second
     * used, and in tiers. Under the budget's hard limit (twice the budget)
     * only what loses nothing goes: superseded or never drawn, and colour
     * that eng_surface_retire writes back to guest memory. Above it, colour
     * it cannot write back and depth buffers too -- their contents are gone
     * (a static shadow map the game shows again later would come back
     * empty), so each such depth drop is logged. A scan runs at most every
     * 30 frames unless over the hard limit. */
    /* Dead sweep, once a second even under the budgets: the tier-0 ones only
     * (never drawn, or drawn over since), which cost nothing to drop. The
     * shadow-map pool's re-packing leaves hundreds of them: the owner's 9
     * minutes of Great Clock D sat at the 512-buffer cap with ~1.4 GB of
     * depth, most of it dead, until the budget forced a scan. Their images go
     * to the backend's recycle bin for the next target of their size.
     * RSX_ENG_DEAD_SWEEP=0: only over the budgets, as before. */
    static int s_dead_sweep = -1;
    if (s_dead_sweep < 0) { const char* e = getenv("RSX_ENG_DEAD_SWEEP"); s_dead_sweep = !(e && *e == '0'); }
    static u32 s_rt_scan, s_z_scan;
    u64 rt_bytes = 0;
    for (u32 i = 0; i < g.n_surfaces; i++) if (g.surfaces[i].handle) rt_bytes += eng_surface_bytes(&g.surfaces[i]);
    const u64 rt_hard = rt_budget * 2u;
    const int rt_sweep = s_dead_sweep && !pressure && rt_bytes <= rt_budget && g.frames - s_rt_scan >= 60u;
    if ((rt_bytes > rt_budget && (rt_bytes > rt_hard || g.frames - s_rt_scan >= 30u)) || (pressure && rt_bytes) || rt_sweep) {
        s_rt_scan = g.frames;
        const u64 tgt = rt_sweep ? 0u
                      : pressure && rt_bytes / 4u * 3u < rt_budget / 10u * 9u ? rt_bytes / 4u * 3u : rt_budget / 10u * 9u;
        u32 n = 0;
        for (u32 i = 0; i < g.n_surfaces; i++) {
            const eng_surface* s = &g.surfaces[i];
            if (!s->handle || i == g.last_present_surface || s->last_use > recent || s->last_use > frame_start) continue;
            u64 tier;
            if (!s->color_written || eng_superseded(s->location, s->offset, eng_surface_span(s), s->last_write, s) ||
                eng_surface_overwritten(i)) tier = 0;
            else if (rt_sweep) continue;
            else if (eng_surface_writes_back(s)) tier = 1;
            else if (rt_bytes > rt_hard) tier = 2;
            else continue;
            lru[n].key = (tier << 60) | (s->last_use & ((1ull << 60) - 1u)); lru[n].idx = i; n++;
        }
        qsort(lru, n, sizeof lru[0], eng_lru_cmp);
        static u8 drop[ENG_MAX_SURFACES];
        memset(drop, 0, sizeof drop);
        u32 ev = 0;
        for (u32 k = 0; k < n && rt_bytes > tgt; k++) {
            const u32 i = lru[k].idx;
            /* Tier 0 loses nothing and is not written back (a superseded
             * target's pixels are older than what was drawn over them). */
            if ((lru[k].key >> 60) && eng_surface_retire(i)) continue;   /* pixels not saved: keep it */
            rt_bytes -= eng_surface_bytes(&g.surfaces[i]);
            g.be->color_target_release(g.be->user, g.surfaces[i].handle);
            drop[i] = 1; ev++;
        }
        if (ev) {
            u32 j = 0, lp = g.last_present_surface;
            for (u32 i = 0; i < g.n_surfaces; i++) if (!drop[i]) {
                if (i == g.last_present_surface) lp = j;
                if (j != i) g.surfaces[j] = g.surfaces[i];
                j++;
            }
            /* eng_surface_get releases the handle of the slot it appends
             * into: left in the vacated tail, a survivor's copy there was
             * released while the survivor still used it, and the freed
             * handle came straight back as a new object (stale-handle blits,
             * VK_ERROR_DEVICE_LOST). */
            memset(&g.surfaces[j], 0, (size_t)(g.n_surfaces - j) * sizeof g.surfaces[0]);
            g.n_surfaces = j; g.last_present_surface = lp;
            n_rt_ev += ev;
            g.addr_dirty = 1;
            g.bind_epoch++;
        }
    }

    if (s_dead_sweep && g_rsx_shadow_scale > g_rsx_res_scale && !(g.frames & 1u))
        n_z_ev += eng_zdepth_shadow_sweep(s_frame_serial_hist[(g.frames - 2u) & 63u]);
    u64 z_bytes = 0;
    for (u32 i = 0; i < g.n_zdepths; i++) if (g.zdepths[i].handle) z_bytes += eng_zdepth_bytes(&g.zdepths[i]);
    const u64 z_hard = z_budget * 2u;
    const int z_sweep = s_dead_sweep && !pressure && z_bytes <= z_budget && g.frames - s_z_scan >= 60u;
    if ((z_bytes > z_budget && (z_bytes > z_hard || g.frames - s_z_scan >= 30u)) || (pressure && z_bytes) || z_sweep) {
        s_z_scan = g.frames;
        const u64 tgt = z_sweep ? 0u
                      : pressure && z_bytes / 4u * 3u < z_budget / 10u * 9u ? z_bytes / 4u * 3u : z_budget / 10u * 9u;
        u32 n = 0;
        for (u32 i = 0; i < g.n_zdepths; i++) {
            const eng_zdepth* z = &g.zdepths[i];
            if (!z->handle || z->last_use > recent || z->last_use > frame_start) continue;
            u64 tier;
            if (!z->had_write || eng_superseded(z->location, z->offset, z->w * z->h * 4u, z->last_write, z)) tier = 0;
            else if (z_sweep) continue;
            else if (z_bytes > z_hard) tier = 1;
            else continue;
            lru[n].key = (tier << 60) | (z->last_use & ((1ull << 60) - 1u)); lru[n].idx = i; n++;
        }
        qsort(lru, n, sizeof lru[0], eng_lru_cmp);
        static u8 drop[ENG_MAX_ZDEPTHS];
        memset(drop, 0, sizeof drop);
        u32 ev = 0;
        for (u32 k = 0; k < n && z_bytes > tgt; k++) {
            eng_zdepth* z = &g.zdepths[lru[k].idx];
            if (lru[k].key >> 60) eng_note_lossy_depth(z, "over the hard limit");
            z_bytes -= eng_zdepth_bytes(z);
            g.be->depth_target_release(g.be->user, z->handle);
            if (z->as_color) g.be->color_target_release(g.be->user, z->as_color);
            if (z->as_color_spare) g.be->color_target_release(g.be->user, z->as_color_spare);
            drop[lru[k].idx] = 1; ev++;
        }
        if (ev) {
            u32 j = 0;
            for (u32 i = 0; i < g.n_zdepths; i++) if (!drop[i]) { if (j != i) g.zdepths[j] = g.zdepths[i]; j++; }
            /* Same for eng_zdepth_get, which also releases the slot's
             * depth-as-colour targets. */
            memset(&g.zdepths[j], 0, (size_t)(g.n_zdepths - j) * sizeof g.zdepths[0]);
            g.n_zdepths = j;
            n_z_ev += ev;
            g.addr_dirty = 1;
            g.bind_epoch++;
        }
    }

    if ((memlog && g.frames % 120u == 0) || g.frames % 3600u == 0)
        fprintf(stderr, "[rsx mem] frame %u: textures %u (%.0f MB / %.0f), targets %u (%.0f MB / %.0f), depth %u (%.0f MB / %.0f); evicted %llu / %llu / %llu; evicted textures decoded again %llu%c",
                g.frames, g.n_textures, tex_bytes / 1048576.0, tex_budget / 1048576.0,
                g.n_surfaces, rt_bytes / 1048576.0, rt_budget / 1048576.0,
                g.n_zdepths, z_bytes / 1048576.0, z_budget / 1048576.0, n_tex_ev, n_rt_ev, n_z_ev,
                s_tex_remade, 10);
}

static void eng_present(u32 buffer_id)
{
    if (!g.ready) return;
    const u32 target = eng_present_surface(buffer_id);
    if (target == ENG_INVALID) return;

    if (s_dump_requested || g.frames == 60 || g.frames == 120 || g.frames == 300 || g.frames == 600 ||
        g.frames == 900 || g.frames == 1200 || g.frames == 1800 || g.frames == 2400 || g.frames == 3600) {
        s_dump_requested = 0;
        eng_dump_all_surfaces(g.frames);
    }
    if (g.frames % 120 == 0) {
        u32 current = eng_current_surface();
        fprintf(stderr, "[rsx capture] frame=%u buffer=%u selected=%u current=%u draws=%u surfaces=%u\n",
                g.frames, buffer_id, target, current, g.guest_draws, g.n_surfaces);
        for (u32 i = 0; i < g.n_surfaces; ++i)
            fprintf(stderr, "[rsx capture]   surf %u loc=%u offset=%08X size=%ux%u draws=%u last_write=%llu handle=%u\n",
                    i, g.surfaces[i].location, g.surfaces[i].offset,
                    g.surfaces[i].w, g.surfaces[i].h, g.surfaces[i].draws_since_present,
                    (unsigned long long)g.surfaces[i].last_write, g.surfaces[i].handle);
    }
    /* RSX_ENG_PRESENT_LOG=<n>: log present info */
    { static long n_log = -1; static long seen = 0;
      if (n_log < 0) { const char* e = getenv("RSX_ENG_PRESENT_LOG"); n_log = e ? atol(e) : 0; }
      if (n_log && ++seen <= n_log)
          fprintf(stderr, "[rsx present] #%ld frame %u buffer %u -> surface %u off %08X: %u draws into it since, last clip %ux%u%c",
                  seen, g.frames, buffer_id, target, g.surfaces[target].offset,
                  g.surfaces[target].draws_since_present, g.surfaces[target].last_clip >> 16,
                  g.surfaces[target].last_clip & 0xFFFFu, 10); }
    g.surfaces[target].draws_since_present = 0;
    g.last_present_surface = target;
    extern unsigned long long ps3_qpc_us(void);
    const unsigned long long pt0 = ps3_qpc_us();
    eng_writeback_small();
    const unsigned long long pt1 = ps3_qpc_us();
    if (pt1 - pt0 >= 20000ull) { static int n; if (n++ < 100)
        fprintf(stderr, "[present-slow] frame %u: small-target writeback %.1f ms%c", g.frames, (pt1 - pt0) / 1e3, 10); }
    g.be->present(g.be->user, g.surfaces[target].handle);
    g.frames++;
    s_frame_serial = g.surf_serial;
    if (g.frames % 120 == 0) eng_stats_report();
    { static int tt = -1;
      if (tt < 0) { const char* e = getenv("RSX_ENG_TRACE_TARGETS"); tt = e ? atol(e) : 1; }
      if (tt && g.frames % 120 < 2 && s_tt_n) {   /* a flip presents twice; the second has no draws */
          fprintf(stderr, "[rsx targets] frame %u, in first-use order:\n", g.frames);
          for (u32 i = 0; i < s_tt_n; i++) {
              const u32 s = s_tt[i].slot, z = s_tt[i].zslot;
              fprintf(stderr, "[rsx targets]   %5u draws -> %08X %ux%u x%u  depth %08X %ux%u  aa %u\n", s_tt[i].count,
                      g.surfaces[s].offset, g.surfaces[s].w, g.surfaces[s].h, s_tt[i].n,
                      z != ENG_INVALID && z < g.n_zdepths ? g.zdepths[z].offset : 0xFFFFFFFFu,
                      z != ENG_INVALID && z < g.n_zdepths ? g.zdepths[z].w : 0u,
                      z != ENG_INVALID && z < g.n_zdepths ? g.zdepths[z].h : 0u, s_tt[i].aa);
          }
      }
      s_tt_n = 0; }
    { const unsigned long long bt0 = ps3_qpc_us();
      eng_budget_enforce();
      const unsigned long long bt1 = ps3_qpc_us();
      if (bt1 - bt0 >= 20000ull) { static int n; if (n++ < 100)
          fprintf(stderr, "[present-slow] frame %u: memory budget enforcement %.1f ms%c", g.frames, (bt1 - bt0) / 1e3, 10); } }
    g.last_guest_draws = g.guest_draws;
    g.guest_draws = 0;
}

static void sink_flip(void* user, const rsx_dispatch* r, u32 arg)
{
    (void)user; (void)r;
    g.last_flip_buffer = arg & 7u;
    eng_present(g.last_flip_buffer);
}

/* ---- public API ---------------------------------------------------------- */

void rsx_draw_engine_set_backend(const rsx_draw_backend* backend)
{
    g.be = backend;
}

void rsx_draw_engine_set_default(int on)
{
    g.default_on = on;
}

int rsx_draw_engine_enabled(void)
{
    static int cached = -1;
    static int env_seen = 0;
    static int env_on = 0;
    if (!env_seen) {
        const char* e = getenv("PS3RECOMP_RSX_ENGINE");
        env_seen = 1;
        if (e && *e) env_on = (strcmp(e, "dispatch") == 0) ? 1 : -1;
    }
    /* The environment is authoritative in both directions; without it the
     * backend's own default decides. Cached after the first backend has
     * registered, so the FIFO walker pays one compare. */
    const int on = env_on > 0 ? 1 : (env_on < 0 ? 0 : g.default_on);
    if (!g.be) return 0;
    if (cached < 0) cached = on;
    return cached;
}

int rsx_draw_engine_init(u32 width, u32 height)
{
    if (!g.be) return -1;
    g.width  = width  ? width  : 1280;
    g.height = height ? height : 720;
    g.vp_cb = (u8*)calloc(1, ENG_VP_CB_BYTES);
    if (!g.vp_cb) return -1;
    s_vp_cb_changed = 1; s_vp_cb_bound = NULL;   /* a new block may reuse the old address */
    s_vp_dlo = 0; s_vp_dhi = ENG_VP_CB_BYTES;
    if (g.be->init && g.be->init(g.be->user, g.width, g.height) != 0) {
        free(g.vp_cb); g.vp_cb = NULL;
        return -1;
    }
    rsx_dispatch_sink sink;
    memset(&sink, 0, sizeof sink);
    sink.clear            = sink_clear;
    sink.begin            = sink_begin;
    sink.end              = sink_end;
    sink.draw_arrays      = sink_draw_arrays;
    sink.draw_index_array = sink_draw_index;
    sink.inline_array     = sink_inline_array;
    sink.flip             = sink_flip;
    rsx_dispatch_init(&g.rsx, &sink);
    g.ready = 1;
    fprintf(stderr, "[rsx engine] register-file draw engine up (%ux%u)\n",
            g.width, g.height);
    return 0;
}

void rsx_draw_engine_shutdown(void)
{
    if (!g.be) return;
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_SHUTDOWN);
    for (u32 i = 0; i < g.n_textures; i++)
        if (g.textures[i].handle) g.be->texture_release(g.be->user, g.textures[i].handle);
    for (u32 i = 0; i < g.n_pipelines; i++)
        if (g.pipelines[i].handle) g.be->pipeline_release(g.be->user, g.pipelines[i].handle);
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].handle) g.be->color_target_release(g.be->user, g.surfaces[i].handle);
    for (u32 i = 0; i < g.n_zdepths; i++)
        if (g.zdepths[i].handle) g.be->depth_target_release(g.be->user, g.zdepths[i].handle);
    if (g.ready && g.be->shutdown) g.be->shutdown(g.be->user);

    rsx_draw_engine_set_guest_memory(NULL, NULL);
    free(dc.refs); free(dc.cuts); free(dc.verts); free(dc.arr); free(dc.idx);
    rsx_vertex_remap_destroy(&dc.ref_remap);
    if (s_geom) {
        for (u32 i = 0; i < ENG_GEOM_SETS * ENG_GEOM_WAYS; i++) if (s_geom[i].state == 2) eng_geom_free(&s_geom[i]);
        free(s_geom); s_geom = NULL; s_geom_on = -1;
        free(s_vb); s_vb = NULL; s_vb_on = -1; s_vb_held = 0;
    }
    memset(&dc, 0, sizeof dc);
    free(g.tex_staging); free(g.vp_cb); free(g.fp_cb); free(g.indices);
    const rsx_draw_backend* be = g.be;
    memset(&g, 0, sizeof g);
    g.be = be;
}

void rsx_draw_engine_method(u32 method, u32 arg)
{
    if (!g.ready) return;
    /* The subchannel is a binding slot, not an engine selector, and a title
     * whose SPU-built command lists bind NV4097 elsewhere would otherwise
     * store its state in the wrong register bank.  Standard NV4097 methods
     * (< 0x2000) strip the subchannel with & 0x1FFC.  Driver methods
     * (0xE9xx, 0xEBxx) encode the subchannel bits as part of the address;
     * preserve them with the wider mask. */
    u32 m = (method >= 0xE000u) ? (method & 0xFFFCu) : (method & 0x1FFCu);
    rsx_dispatch_method(&g.rsx, m, arg);
}

void rsx_draw_engine_methods(u32 method, const u8* be, u32 count, int incrementing)
{
    if (!g.ready) return;
    for (u32 i = 0; i < count; ) {
        const u32 m = incrementing ? ((method + i * 4u) & 0x1FFCu) : (method & 0x1FFCu);
        if (incrementing && m >= 0x1F00u && m < 0x1F80u) {      /* transform constants */
            const u32 first = (m - 0x1F00u) >> 2;
            u32 n = 32u - first;
            if (n > count - i) n = count - i;
            rsx_dispatch_const_words(&g.rsx, first, be + (size_t)i * 4u, n);
            i += n;
            continue;
        }
        u32 w; memcpy(&w, be + (size_t)i * 4u, 4);
        rsx_dispatch_method(&g.rsx, m, __builtin_bswap32(w));
        i++;
    }
}

void rsx_draw_engine_set_display_buffer(u32 buffer_id, u32 location, u32 offset,
                                        u32 pitch, u32 width, u32 height)
{
    if (buffer_id >= 8) return;
    eng_display_buffer* d = &g.display_buffers[buffer_id];
    d->location = location;
    d->offset = offset;
    d->pitch = pitch;
    d->width = width;
    d->height = height;
    d->valid = width && height;
}

void rsx_draw_engine_flush(void)
{
    if (g.ready && g.be->submit_and_wait)
        g.be->submit_and_wait(g.be->user, RSX_BE_FLUSH_GUEST_REFERENCE);
}

void rsx_draw_engine_present(void)
{
    /* A host that drives the flip itself gets whichever buffer the guest's
     * own last flip named, and display buffer 0 before there has been one:
     * a runner that both consumes 0xE944 and calls here would otherwise
     * re-present a double-buffered title's OTHER scanout. Presenting twice is
     * harmless either way -- the engine never clears the surface, so a second
     * present just blits the same image again. The FIFO walker records the
     * buffer of each flip it decodes in cellGcm's current display buffer id:
     * without it this presented "buffer 0", which A Crack in Time never
     * registers (it uses 1 and 2), so every present fell back to whatever
     * target was bound last -- some frames showed an intermediate buffer. */
    extern u32 cellGcmGetCurrentDisplayBufferId(void);
    const u32 id = cellGcmGetCurrentDisplayBufferId();
    if (id < 8) g.last_flip_buffer = id;
    eng_present(g.last_flip_buffer);
}

/* The colour target whose pixels hold `off` (row pitch `pitch`): a blit may
 * start anywhere inside one. A Crack in Time copies the patch of scene behind
 * each heat-haze particle -- source = scene base + y*pitch + x*4 -- into a
 * small texture. *x, *y receive the position in guest pixels. */
static u32 eng_surface_containing(u32 loc, u32 off, u32 pitch, u32* x, u32* y)
{
    if (!pitch) return ENG_INVALID;
    u32 best = ENG_INVALID;
    for (u32 i = 0; i < g.n_surfaces; i++) {
        const eng_surface* s = &g.surfaces[i];
        if (!s->handle || s->location != loc || s->fmt != RSX_BE_FMT_R8G8B8A8 || !s->color_written) continue;
        const u32 sp = s->pitch ? s->pitch : s->w * 4u;
        if (sp != pitch || off < s->offset) continue;
        const u32 d = off - s->offset, yy = d / sp, xx = (d % sp) / 4u;
        if (yy >= s->h || xx >= s->w) continue;
        if (s->copy_h && yy >= s->copy_h) continue;    /* past what was copied into it */
        if (best == ENG_INVALID || s->last_write > g.surfaces[best].last_write) { best = i; *x = xx; *y = yy; }
    }
    return best;
}

/* Rows [y, y+h) x [x, x+w) of the GPU target that holds `off` (row pitch
 * `pitch`), read back into guest memory as A8R8G8B8 when the GPU has drawn
 * there after the guest last wrote it. For a CPU-side transfer out of a
 * target -- A Crack in Time's swizzled pyramid blits (NV3089 into NV309E) --
 * which otherwise copies whatever stale bytes the guest memory held: one
 * cyan-speckled texture shared by every material that samples the pyramid.
 * Returns 1 when it read back, 0 when guest memory is already current or no
 * target holds the address. */
int rsx_draw_engine_sync_region(u32 loc, u32 off, u32 pitch, u32 x, u32 y, u32 w, u32 h)
{
    if (!g.ready || !g.be->readback || !w || !h) return 0;
    u32 ix = 0, iy = 0;
    u32 cs = eng_surface_find(loc, off, 0, 0);
    if (cs != ENG_INVALID && !(g.surfaces[cs].pitch == pitch || (!g.surfaces[cs].pitch && g.surfaces[cs].w * 4u == pitch))) cs = ENG_INVALID;
    if (cs == ENG_INVALID) cs = eng_surface_containing(loc, off, pitch, &ix, &iy);
    if (cs == ENG_INVALID) return 0;
    eng_surface* s = &g.surfaces[cs];
    if (!s->handle || !s->color_written || s->fmt != RSX_BE_FMT_R8G8B8A8) return 0;
    x += ix; y += iy;
    if (x >= s->w || y >= s->h) return 0;
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (s->copy_h) { if (y >= s->copy_h) return 0; if (y + h > s->copy_h) h = s->copy_h - y; }
    /* The game wrote these rows after the GPU did: guest memory is current,
     * and writing the GPU's older pixels over it would destroy its data. */
    if (eng_surface_rows_stale(cs, y, h)) return 0;
    u8* dst = (u8*)eng_guest_ptr(NULL, s->location, s->offset, pitch * s->h);
    if (!dst) return 0;
    /* Already there: nothing drawn since this region was last synced. */
    if (s->synced_serial == s->last_write) return 0;
    u8* px = (u8*)malloc((size_t)w * h * 4u);
    if (!px) return 0;
    if (g.be->readback(g.be->user, s->handle, x, y, w, h, px, w * 4u)) { free(px); return 0; }
    /* Not the game's write: no target on these pages reads as overwritten
     * by it, this one included (rsx_ww_self_begin). */
    rsx_ww_self_begin(dst + (size_t)y * pitch, pitch * h);
    for (u32 r = 0; r < h; r++)
        for (u32 c = 0; c < w; c++) {
            const u8* q = px + ((size_t)r * w + c) * 4u;
            u8* d = dst + (size_t)(y + r) * pitch + (size_t)(x + c) * 4u;
            d[0] = q[3]; d[1] = q[0]; d[2] = q[1]; d[3] = q[2];   /* RGBA -> A,R,G,B */
        }
    rsx_ww_self_end(dst + (size_t)y * pitch, pitch * h);
    free(px);
    if (x == 0 && y == 0 && w == s->w && h == s->h) s->synced_serial = s->last_write;
    { static unsigned long long n; n++;
      if (n <= 8 || (n & (n - 1)) == 0)
          fprintf(stderr, "[rsx engine] target %u:0x%08X %ux%u read back (%u,%u %ux%u) for a CPU transfer out of it (%llu so far)%c",
                  s->location, s->offset, s->w, s->h, x, y, w, h, n, 10); }
    return 1;
}

/* Every colour target overlapping [off, off+bytes) at `loc` that holds
 * drawing guest memory has not seen, read back into guest memory (A8R8G8B8,
 * linear or swizzled power-of-two): for a CPU copy out of that range
 * (NV0039). Returns how many were synced. */
int rsx_draw_engine_sync_range(u32 loc, u32 off, u32 bytes)
{
    if (!g.ready || !g.be->readback || !bytes) return 0;
    int n = 0;
    /* RSX_ENG_MOVE_LOG=1: what each CPU copy out of VRAM carries -- every
     * target and depth buffer in the range, and whether its pixels reach
     * guest memory first (a copy target, a depth buffer or another format
     * does not: the copy moves stale bytes). */
    { static int ml = -1; if (ml < 0) { const char* e = getenv("RSX_ENG_MOVE_LOG"); ml = e && *e == '1'; }
      static int lines;
      if (ml && lines < 4000) {
          for (u32 i = 0; i < g.n_surfaces; i++) {
              const eng_surface* s = &g.surfaces[i];
              if (!s->handle || s->location != loc) continue;
              const u32 span = eng_surface_span(s);
              if (s->offset >= off + bytes || s->offset + span <= off) continue;
              const char* why = !s->color_written ? "never drawn" : s->fmt != RSX_BE_FMT_R8G8B8A8 ? "NOT SYNCED: format"
                              : !s->rsx_fmt ? "NOT SYNCED: copy target" : s->synced_serial == s->last_write ? "already in memory"
                              : (s->raster == 2 && ((s->w & (s->w - 1u)) || (s->h & (s->h - 1u)))) ? "NOT SYNCED: swizzled NPOT" : "synced";
              fprintf(stderr, "[rsx move] frame %u copy %u:0x%08X+0x%X holds target 0x%08X %ux%u fmt %d rsx_fmt %u raster %u copy_h %u: %s%c",
                      g.frames, loc, off, bytes, s->offset, s->w, s->h, (int)s->fmt, s->rsx_fmt, s->raster, s->copy_h, why, 10); lines++;
          }
          for (u32 j = 0; j < g.n_zdepths; j++) {
              const eng_zdepth* z = &g.zdepths[j];
              if (!z->handle || z->location != loc || z->offset >= off + bytes || z->offset + z->w * z->h * 4u <= off) continue;
              fprintf(stderr, "[rsx move] frame %u copy %u:0x%08X+0x%X holds depth 0x%08X %ux%u%s%c", g.frames, loc, off, bytes,
                      z->offset, z->w, z->h, z->had_write ? ": NOT SYNCED (written on the GPU)" : "", 10); lines++;
          }
      } }
    for (u32 i = 0; i < g.n_surfaces; i++) {
        eng_surface* s = &g.surfaces[i];
        if (!s->handle || !s->color_written || s->location != loc || s->fmt != RSX_BE_FMT_R8G8B8A8) continue;
        if (s->synced_serial == s->last_write || !s->rsx_fmt) continue;
        if (eng_surface_rows_stale(i, 0, s->h)) continue;   /* the game's bytes are newer there */
        const int swz = s->raster == 2;
        if (swz && ((s->w & (s->w - 1u)) || (s->h & (s->h - 1u)))) continue;
        const u32 pitch = swz ? s->w * 4u : (s->pitch ? s->pitch : s->w * 4u);
        const u32 span = pitch * s->h;
        if (s->offset >= off + bytes || s->offset + span <= off || pitch < s->w * 4u) continue;
        u8* dst = (u8*)eng_guest_ptr(NULL, s->location, s->offset, span);
        if (!dst) continue;
        u8* px = (u8*)malloc((size_t)s->w * s->h * 4u);
        if (!px) continue;
        if (g.be->readback(g.be->user, s->handle, 0, 0, s->w, s->h, px, s->w * 4u)) { free(px); continue; }
        u32 l2w = 0, l2h = 0;
        while ((1u << l2w) < s->w) l2w++;
        while ((1u << l2h) < s->h) l2h++;
        rsx_ww_self_begin(dst, span);           /* our own write is not the game's */
        for (u32 y = 0; y < s->h; y++)
            for (u32 x = 0; x < s->w; x++) {
                const u8* q = px + ((size_t)y * s->w + x) * 4u;
                u8* d = swz ? dst + (size_t)rsx_swizzle_offset(x, y, l2w, l2h) * 4u : dst + (size_t)y * pitch + x * 4u;
                d[0] = q[3]; d[1] = q[0]; d[2] = q[1]; d[3] = q[2];
            }
        rsx_ww_self_end(dst, span);
        free(px);
        s->synced_serial = s->last_write;
        n++;
    }
    if (n) { static int logs; if (logs < 16) { logs++;
        fprintf(stderr, "[rsx engine] %d target(s) in %u:0x%08X+0x%X read back for a CPU copy out of it%c", n, loc, off, bytes, 10); } }
    return n;
}

/* A guest flip, `frame_us` after the last: RSX_HITCH=<ms> (default off)
 * prints what the render thread did in every frame longer than that --
 * draws and time inside the sink, texture uploads, new pipelines, new
 * targets, depth/colour snapshots, and the frame-clock thread's drain and
 * present time (the rest of the frame it waited on the game). */
unsigned long long g_rt_drain_total_us, g_rt_present_total_us;
unsigned long long g_rsx_gpu_wait_us;   /* the backend's waits on the GPU (fences), for [hitch] */
void rsx_draw_engine_frame_mark(unsigned long long frame_us)
{
    static long thr = -2;
    if (thr == -2) { const char* e = getenv("RSX_HITCH"); thr = e ? atol(e) : -1; }
    static unsigned long long drain0, pres0;
    /* TSC cycles per microsecond, from the frames themselves. */
    static double s_cyc_per_us; static u64 s_last_tsc;
    { const u64 now = eng_cycles();
      if (s_last_tsc && frame_us > 1000 && frame_us < 1000000) {
          const double est = (double)(now - s_last_tsc) / (double)frame_us;
          s_cyc_per_us = s_cyc_per_us > 0.0 ? s_cyc_per_us * 0.98 + est * 0.02 : est;
      }
      s_last_tsc = now; }
    const unsigned long long drain = g_rt_drain_total_us - drain0, pres = g_rt_present_total_us - pres0;
    static unsigned long long gw0; const unsigned long long gw0_prev = gw0; gw0 = g_rsx_gpu_wait_us;
    drain0 = g_rt_drain_total_us; pres0 = g_rt_present_total_us;
    eng_park_flip();
    if (thr >= 0 && frame_us >= (unsigned long long)thr * 1000ull) {
        if (s_cyc_per_us <= 0.0) s_cyc_per_us = 3000.0;
        static unsigned n;
        if (n++ < 2000)
            fprintf(stderr, "[hitch] frame %u %.1f ms: %u draws, sink %.1f ms, %u tex uploads %.1f ms, %u new pipelines %.1f ms, %u new targets (%u depth) %.1f ms, %u snapshots %.1f ms, GPU waits %.1f ms | drain %.1f ms, present %.1f ms, idle %.1f ms%c",
                    g.frames, frame_us / 1e3, s_hf.draws, s_hf.sink_cyc / s_cyc_per_us / 1e3, s_hf.tex_ups, s_hf.tex_cyc / s_cyc_per_us / 1e3,
                    s_hf.pipes, s_hf.pipe_cyc / s_cyc_per_us / 1e3, s_hf.surfs, s_hf.zcreates, s_hf.create_cyc / s_cyc_per_us / 1e3,
                    s_hf.snaps, s_hf.snap_cyc / s_cyc_per_us / 1e3, (gw0 - gw0_prev) / 1e3,
                    drain / 1e3, pres / 1e3, frame_us > drain + pres ? (frame_us - drain - pres) / 1e3 : 0.0, 10);
    }
    memset(&s_hf, 0, sizeof s_hf);
}

/* Rows [y, y+h) of target i, as its pitch lays them out in guest memory,
 * written by the game since the GPU last wrote the target: there the guest
 * bytes are the current ones and the GPU copy is stale, so an RSX copy out of
 * those rows has to read guest memory -- as the RSX would, since it has no
 * second copy. A Crack in Time compacts its streamed level textures with
 * NV3089 moves (32-pixel rows, 128-byte pitch) through VRAM that menu and
 * vendor targets drew into earlier; without this a move out of such memory
 * could be done on the GPU from the target's leftover pixels. (Hardening:
 * the striped textures themselves were a lifted vcmpequh., see
 * scripts/fix_vmx_alias.py.) */
static int eng_surface_rows_stale(u32 i, u32 y, u32 h)
{
    if (i >= g.n_surfaces || !rsx_ww_on()) return 0;
    const eng_surface* s = &g.surfaces[i];
    if (!s->handle || !s->ww_epoch || s->raster == 2) return 0;
    const u32 sp = s->pitch ? s->pitch : s->w * 4u;
    if (y >= s->h) return 0;
    if (y + h > s->h || y + h < y) h = s->h - y;
    const u64 len = (u64)sp * h;
    if (!len || len > (64u << 20)) return 0;
    const u8* p = (const u8*)eng_guest_ptr(NULL, s->location, s->offset + y * sp, (u32)len);
    return eng_ww_written_inner(p, (u32)len, s->ww_epoch);
}

int rsx_draw_engine_blit(u32 src_loc, u32 src_off, u32 sx, u32 sy, u32 sw, u32 sh,
                         u32 dst_loc, u32 dst_off, u32 dx, u32 dy, u32 dw, u32 dh)
{
    return rsx_draw_engine_blit_pitched(src_loc, src_off, 0, sx, sy, sw, sh, dst_loc, dst_off, 0, dx, dy, dw, dh);
}

int rsx_draw_engine_blit_pitched(u32 src_loc, u32 src_off, u32 src_pitch, u32 sx, u32 sy, u32 sw, u32 sh,
                                 u32 dst_loc, u32 dst_off, u32 dst_pitch, u32 dx, u32 dy, u32 dw, u32 dh)
{
    if (!g.ready || !g.be->blit) return 0;
    /* A depth buffer copied as 32-bit pixels is a depth copy: the bytes are
     * D24S8 either way. A Crack in Time builds its half-res effects pass's
     * depth buffer like this; without the GPU copy that pass depth-tested
     * against nothing and its result showed as a box around the player. */
    for (u32 i = 0; i < g.n_zdepths; i++) {
        if (!g.zdepths[i].handle || g.zdepths[i].location != src_loc || g.zdepths[i].offset != src_off)
            continue;
        if (eng_zdepth_overwritten(i)) return 0;      /* the game's bytes are there now */
        const u32 zd = eng_zdepth_get(dst_loc, dst_off, dx + dw, dy + dh);
        if (zd == ENG_INVALID || zd == i) return 0;
        const u32 sh_ = g.zdepths[i].handle;          /* eng_zdepth_get may move the table */
        if (!g.be->blit(g.be->user, sh_, sx, sy, sw, sh, g.zdepths[zd].handle, dx, dy, dw, dh))
            return 0;
        g.zdepths[zd].cleared = 1;
        if (!g.zdepths[zd].had_write) g.bind_epoch++;
        g.zdepths[zd].had_write = 1;
        g.zdepths[zd].last_write = ++g.surf_serial;
        { static int n = 0; if (n < 3) { n++; fprintf(stderr, "[rsx engine] GPU depth blit %08X %ux%u -> %08X %ux%u%c", src_off, sw, sh, dst_off, dw, dh, 10); } }
        if (src_pitch && src_pitch == dst_pitch && sw * 4u == src_pitch) {
            static unsigned long long n; n++;
            if (n <= 64 || (n & (n - 1)) == 0)
                fprintf(stderr, "[rsx engine] linear move %u:0x%08X -> 0x%08X (%u rows) done on the GPU from depth 0x%08X %ux%u (ww_epoch %u, now %u, last_write %llu) (%llu so far)%c",
                        src_loc, src_off, dst_off, sh, g.zdepths[i].offset, g.zdepths[i].w, g.zdepths[i].h, g.zdepths[i].ww_epoch, rsx_ww_epoch(),
                        (unsigned long long)g.zdepths[i].last_write, n, 10);
        }
        g.zdepths[zd].snapshot_valid = 0;
        g.zdepths[zd].as_color_valid = 0;
        return 1;
    }
    u32 cs = eng_surface_find(src_loc, src_off, 0, 0);
    /* The same address with another row pitch is another image: a stale
     * target there says nothing about these bytes. */
    if (cs != ENG_INVALID && src_pitch && g.surfaces[cs].pitch && g.surfaces[cs].pitch != src_pitch)
        cs = ENG_INVALID;
    if (cs == ENG_INVALID) {                       /* from inside a target */
        u32 ix = 0, iy = 0;
        cs = eng_surface_containing(src_loc, src_off, src_pitch, &ix, &iy);
        if (cs != ENG_INVALID) {
            sx += ix; sy += iy;
            if (sx + sw > g.surfaces[cs].w) sw = g.surfaces[cs].w - sx;
            if (sy + sh > g.surfaces[cs].h) sh = g.surfaces[cs].h - sy;
            src_off = g.surfaces[cs].offset;
            { static int n = 0; if (n < 4) { n++; fprintf(stderr, "[rsx engine] blit source inside target 0x%08X at %u,%u %ux%u%c",
                                                         src_off, sx, sy, sw, sh, 10); } }
        }
    }
    /* A copy target sized up to a power of two claims rows nothing was copied
     * into: those are still only in guest memory. */
    if (cs != ENG_INVALID && g.surfaces[cs].copy_h && sy + sh > g.surfaces[cs].copy_h) {
        static unsigned long long n; n++;
        if (n <= 16 || (n & (n - 1)) == 0)
            fprintf(stderr, "[rsx engine] blit out of copy target %u:0x%08X rows %u+%u, past the %u it holds -- copied from memory (%llu so far)%c",
                    src_loc, g.surfaces[cs].offset, sy, sh, g.surfaces[cs].copy_h, n, 10);
        return 0;
    }
    if (cs != ENG_INVALID && eng_surface_rows_stale(cs, sy, sh)) {
        static unsigned long long n; n++;
        if (n <= 16 || (n & (n - 1)) == 0)
            fprintf(stderr, "[rsx engine] blit out of target %u:0x%08X rows %u+%u: the game has written them since the GPU did -- copied from memory (%llu so far)%c",
                    src_loc, g.surfaces[cs].offset, sy, sh, n, 10);
        return 0;
    }
    u32 cd = eng_surface_find(dst_loc, dst_off, 0, 0);
    /* A copy OUT of a target into memory no target covers: A Crack in Time
     * shrinks the scene into a small texture (128x63 at 0x0205F100) that its
     * heat haze over fires and its water refraction sample. Only the GPU has
     * the source's pixels, so the guest-memory copy the caller also makes
     * copies nothing useful -- the haze drew black boxes and the water came
     * out a flat slab. The destination becomes a target, filled on the GPU,
     * and a texture read at that address then finds it. */
    /* A copy target too small for this copy (the haze copies one patch per
     * particle, each its own size, into one address): grown in its own slot.
     * A new identity per size (32x18, 48x24 ... 144x77 at 0x0205F100) filled
     * the target list within minutes of fire and time effects, and the
     * targets it pushed out came back as noise. */
    if (cs != ENG_INVALID && cd != ENG_INVALID && !g.surfaces[cd].rsx_fmt &&
        (dx + dw > g.surfaces[cd].w || dy + dh > g.surfaces[cd].h) &&
        dx + dw <= ENG_MAX_SURFACE_DIM && dy + dh <= ENG_MAX_SURFACE_DIM) {
        eng_surface* c = &g.surfaces[cd];
        u32 nw = c->w > dx + dw ? c->w : dx + dw, nh = c->h > dy + dh ? c->h : dy + dh;
        { u32 p2 = 16u; while (p2 < nw) p2 <<= 1; nw = p2; }
        { u32 p2 = 16u; while (p2 < nh) p2 <<= 1; nh = p2; }
        const u32 hnew = g.be->color_target_create(g.be->user, RSX_BE_FMT_R8G8B8A8, nw, nh, NULL, 0);
        if (!hnew) return 0;
        g.be->color_target_release(g.be->user, c->handle);
        c->handle = hnew; c->w = nw; c->h = nh;
        c->color_written = 0;
        c->copy_h = 0;                                /* the new image holds nothing yet */
        g.bind_epoch++;
        { static int n = 0; if (n < 8) { n++; fprintf(stderr, "[rsx engine] copy target %u:0x%08X grown to %ux%u%c",
                                                     dst_loc, dst_off, nw, nh, 10); } }
    }
    if (cs != ENG_INVALID && cd == ENG_INVALID && g.surfaces[cs].color_written &&
        g.surfaces[cs].fmt == RSX_BE_FMT_R8G8B8A8 && dw && dh &&
        dx + dw <= ENG_MAX_SURFACE_DIM && dy + dh <= ENG_MAX_SURFACE_DIM) {
        const u32 src_handle = g.surfaces[cs].handle;
        u32 tw = dx + dw, th = dy + dh;
        if (dst_pitch >= 4u && dst_pitch / 4u > tw) tw = dst_pitch / 4u;   /* the whole row, as a reader declares it */
        { u32 p2 = 16u; while (p2 < tw) p2 <<= 1; tw = p2; }              /* settles after a size or two */
        { u32 p2 = 16u; while (p2 < th) p2 <<= 1; th = p2; }
        cd = eng_surface_get(dst_loc, dst_off, tw, th, RSX_BE_FMT_R8G8B8A8);
        if (cd != ENG_INVALID && g.surfaces[cd].handle != src_handle) {
            g.surfaces[cd].pitch = dst_pitch;
            g.surfaces[cd].rsx_fmt = 0;            /* layout unknown: never written back */
            { static int n = 0; if (n < 8) { n++; fprintf(stderr, "[rsx engine] copy target %u:0x%08X %ux%u made for a blit out of 0x%08X%c",
                                                         dst_loc, dst_off, dx + dw, dy + dh, src_off, 10); } }
        }
        if (cd != ENG_INVALID && eng_surface_find(src_loc, src_off, 0, 0) != cs) return 0;   /* the table moved */
    }
    if (cs == ENG_INVALID || cd == ENG_INVALID || cs == cd) return 0;
    /* Readers' cached bindings change only when the target first holds
     * something: one epoch bump per haze particle re-resolved every unit of
     * every later draw. */
    if (!g.surfaces[cd].color_written) g.bind_epoch++;
    g.surfaces[cd].color_written = 1;
    g.surfaces[cd].last_write = ++g.surf_serial;
    g.surfaces[cd].ww_epoch = rsx_ww_epoch();
    { static int n = 0; if (n < 8) { n++; fprintf(stderr, "[rsx engine] GPU colour blit %08X %u,%u %ux%u -> %08X %u,%u %ux%u%c",
                                                  src_off, sx, sy, sw, sh, dst_off, dx, dy, dw, dh, 10); } }
    if (src_pitch && src_pitch == dst_pitch && sw * 4u == src_pitch) {
        static unsigned long long n; n++;
        const eng_surface* c = &g.surfaces[cs];
        if (n <= 64 || (n & (n - 1)) == 0)
            fprintf(stderr, "[rsx engine] linear move %u:0x%08X -> 0x%08X (rows %u+%u) done on the GPU from target 0x%08X %ux%u pitch %u fmt %u copy_h %u (ww_epoch %u, now %u) (%llu so far)%c",
                    src_loc, c->offset + sy * (c->pitch ? c->pitch : c->w * 4u), dst_off, sy, sh, c->offset, c->w, c->h, c->pitch, c->rsx_fmt, c->copy_h,
                    c->ww_epoch, rsx_ww_epoch(), n, 10);
    }
    if (!g.be->blit(g.be->user, g.surfaces[cs].handle, sx, sy, sw, sh,
                    g.surfaces[cd].handle, dx, dy, dw, dh)) return 0;
    if (!g.surfaces[cd].rsx_fmt && dy + dh > g.surfaces[cd].copy_h)
        g.surfaces[cd].copy_h = dy + dh;              /* a copy target: rows it now holds */
    return 1;
}

void rsx_draw_engine_present_buffer(u32 buffer_id)
{
    g.last_flip_buffer = buffer_id & 7u;
    eng_present(g.last_flip_buffer);
}

u32 rsx_draw_engine_guest_draws(void)
{
    return g.last_guest_draws;
}

u32 rsx_draw_engine_readback_center(void)
{
    if (!g.ready || !g.be->readback) return 0;
    const u32 slot = g.last_present_surface;
    if (slot >= g.n_surfaces) return 0;
    const eng_surface* s = &g.surfaces[slot];
    if (!s->handle || s->fmt != RSX_BE_FMT_R8G8B8A8 || !s->w || !s->h) return 0;
    u8 px[4] = { 0, 0, 0, 0 };
    g.be->readback(g.be->user, s->handle, s->w / 2u, s->h / 2u, 1, 1, px, 4);
    /* Decoded rows are R,G,B,A; the guest's own clear colour is A8R8G8B8, and
     * a host comparing the two wants them in the same order. */
    return ((u32)px[3] << 24) | ((u32)px[0] << 16) |
           ((u32)px[1] << 8)  |  (u32)px[2];
}
