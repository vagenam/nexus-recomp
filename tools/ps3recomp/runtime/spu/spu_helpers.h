/*
 * ps3recomp - SPU instruction semantics helpers
 *
 * Pure-C, header-only implementation of the per-instruction semantics used
 * by lifter-generated code. Extracted from spu_lifter.py so the helpers
 * have one source of truth and can be unit-tested directly (see
 * runtime/spu/tests/test_spu_helpers.c).
 *
 * Each helper is `static inline u128 spu_<mnemonic>(...)`. Naming, lane
 * widths and big-endian conventions match runtime/spu/spu_context.h.
 */

#ifndef SPU_HELPERS_H
#define SPU_HELPERS_H

#include "spu_context.h"
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
#include <intrin.h>
static inline int spu_clz32(uint32_t x) {
    unsigned long idx;
    if (!x) return 32;
    _BitScanReverse(&idx, x);
    return 31 - (int)idx;
}
#else
static inline int spu_clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
#endif

/* Architectural SPU barrier -- the lifted `sync`/`dsync`/`syncc` emission.
 * SPU ISA v1.2 §13 (+ the per-instruction pages): order all earlier LS and
 * channel accesses before later ones; LS must be consistent "if it were\n * observed by another entity" (Table 13-6 is exactly this runtime's
 * host-thread producer -> SPU consumer handoff). A no-op is neither a
 * compiler nor a CPU barrier -- MSVC at /O2 may reorder plain LS accesses
 * across an empty statement (the PPU twin of this bug is LESSONS #11d). A
 * full fence: the locked exchange forces total order and is opaque to the
 * optimizer. */
#ifdef _MSC_VER
static inline void spu_arch_fence(void) { long spu_fence_tmp = 0; _InterlockedExchange(&spu_fence_tmp, 1); }
#else
static inline void spu_arch_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif

/* SPU byte position -> our _u8 index.
 * Our u128 is HOST-NATIVE little-endian (_u32[i]=SPU word i as a value via
 * spu_ls_read128's big-endian load), so within each 4-byte word the _u8[]
 * bytes are reversed vs SPU big-endian byte order. Any op defined in terms of
 * SPU *byte positions* (quadword byte rotates/shifts, gen-controls, shuffle
 * insertion) must map SPU byte P to our index W(P). Word-aligned operations
 * are unaffected (W is identity modulo the within-word reversal). */
#define SPU_W(P) (((P) & ~3) | (3 - ((P) & 3)))

/* ---- constructors ---- */
static inline u128 spu_splat_u32(uint32_t v) {
    u128 r; r._u32[0]=v; r._u32[1]=v; r._u32[2]=v; r._u32[3]=v; return r;
}
/* Link register value for brsl/bisl/bisled: the SPU writes the return address
 * into the PREFERRED word (word 0) and ZEROS the other three slots (matches
 * RPCS3 v128::from32r, SPUInterpreter.cpp BRSL). Splatting it (the old bug)
 * corrupts any code that saves/reloads/operates on the full 128-bit link. */
/* An SPU instruction the lifter could not translate. It emits a bare comment,
 * so an unsupported opcode on a live path silently does NOTHING and the guest
 * computes garbage from there on -- which surfaces much later as a wild DMA or
 * atomic address with no hint of where it came from. Most of these are data
 * embedded in .text that never executes; this fires only if one actually runs.
 * One line per site. */
static inline void spu_unsupported(uint32_t pc, const char* mnemonic)
{
    static int warned;
    if (warned < 64) {
        warned++;
        fprintf(stderr, "[spu] UNSUPPORTED %s at LS 0x%05X -- executed, "
                        "result is wrong from here\n", mnemonic, pc);
        fflush(stderr);
    }
}

static inline u128 spu_link(uint32_t addr) {
    u128 r; r._u32[0]=addr; r._u32[1]=0; r._u32[2]=0; r._u32[3]=0; return r;
}
/* Preferred-slot scalar: value in word 0, remaining slots ZERO. CBEA's scalar
 * channel convention -- rchcnt returns the count this way (RPCS3 measured
 * {1,0,0,0} where the old splat gave {1,1,1,1}). Same bug class as spu_link. */
static inline u128 spu_pref_u32(uint32_t v) {
    u128 r; r._u32[0]=v; r._u32[1]=0; r._u32[2]=0; r._u32[3]=0; return r;
}
static inline u128 spu_splat_u16(uint16_t v) {
    u128 r; for (int i=0;i<8;i++) r._u16[i]=v; return r;
}
static inline u128 spu_splat_u8(uint8_t v) {
    u128 r; for (int i=0;i<16;i++) r._u8[i]=v; return r;
}
static inline u128 spu_zero(void) { u128 r; memset(&r,0,sizeof r); return r; }

/* ---- integer arithmetic (SIMD) ---- */
static inline u128 spu_a(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+b._u32[i]; return r; }
static inline u128 spu_sf(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]-a._u32[i]; return r; }
static inline u128 spu_ah(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+b._u16[i]; return r; }
static inline u128 spu_sfh(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=b._u16[i]-a._u16[i]; return r; }
static inline u128 spu_ai(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+(uint32_t)imm; return r; }
static inline u128 spu_ahi(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+(uint16_t)imm; return r; }
static inline u128 spu_sfi(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)imm-a._u32[i]; return r; }
static inline u128 spu_sfhi(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)imm-a._u16[i]; return r; }

/* ---- multiply (low halfword of each word × ... -> 32-bit, per SPU mpy) ----
 * Sub-lane indexing assumes a little-endian host (matches the recompiler's
 * target). The "low halfword of word i" in SPU BE semantics is _s16[2i] on
 * an LE host (NOT _s16[2i+1], which would be correct on a BE host). */
static inline u128 spu_mpy(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]; return r; }
/* mpya: 16x16 signed multiply of low halves + add rc (per word, RRR form). */
static inline u128 spu_mpya(u128 a, u128 b, u128 c) { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]+c._s32[i]; return r; }
/* sfx: extended subtract rb-ra-1+carry; carry-in = low bit of old rt (RT is 3rd src). */
static inline u128 spu_sfx(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]+~a._u32[i]+(t._u32[i]&1u); return r; }
/* ---- SPU single precision is not IEEE (CBEA SPU ISA 9.1) ----
 * No NaN or Inf: biased exponent 255 is an ordinary exponent, so the range
 * reaches 2^129 (0x7FFFFFFF ~ 6.8e38). Results round toward zero, saturate to
 * +/-0x7FFFFFFF on overflow and become zero below 2^-126; denormal operands
 * are zero. Taken as IEEE floats, every exponent-255 value is a NaN and
 * poisons whatever touches it, where the SPU would have got a number -- and
 * games rely on it: 1/0 (frest+fi of a zero) is the largest number, and that
 * times zero is zero. A Crack in Time's animation job leaves such values in
 * the unused column of Ratchet's joint matrices; its bounds job multiplies
 * them by zero once he moves, got NaN here, and the hero collapsed to a sliver
 * and stopped moving. Operands are decoded to double (exact), computed there,
 * and encoded back with the SPU's rules -- integer work, no libm. */
static inline double spu_xf_get(uint32_t b)
{
    uint64_t e = (b >> 23) & 0xFFu, u = (uint64_t)(b & 0x80000000u) << 32;
    if (e) u |= ((e + 896u) << 52) | ((uint64_t)(b & 0x7FFFFFu) << 29);   /* 1023 - 127 */
    double d; memcpy(&d, &u, sizeof d); return d;   /* exponent 0: a signed zero */
}
static inline uint32_t spu_xf_put(double d)
{
    uint64_t u; memcpy(&u, &d, sizeof u);
    const uint32_t s = (uint32_t)(u >> 32) & 0x80000000u;
    const int64_t e = (int64_t)((u >> 52) & 0x7FFu) - 896;   /* single-biased */
    if (e <= 0) return s;                                      /* zero, underflow */
    if (e > 255) return s | 0x7FFFFFFFu;                       /* saturate (and inf) */
    return s | ((uint32_t)e << 23) | ((uint32_t)(u >> 29) & 0x7FFFFFu);   /* truncate */
}
/* Fast path: lanes that are neither denormal nor exponent 255 -- operands and
 * result -- mean the same number to the SPU as to IEEE, so the plain float
 * result stands (rounded to nearest where the SPU truncates: one ulp, as
 * before). Only the rest take the exact path; doing every lane exactly cost
 * Zolar Forest a third of its frame rate. */
static inline uint32_t spu_xf_plain(uint32_t b)
{
    const uint32_t e = b & 0x7F800000u;
    return (uint32_t)(e != 0x7F800000u) & ((uint32_t)(e != 0u) | (uint32_t)!(b & 0x7FFFFFu));
}
static inline uint32_t spu_xf_plain4(u128 a)
{
    return spu_xf_plain(a._u32[0]) & spu_xf_plain(a._u32[1]) & spu_xf_plain(a._u32[2]) & spu_xf_plain(a._u32[3]);
}
#define SPU_XF_ARITH2(name, OP) \
    static inline u128 name(u128 a, u128 b) { u128 r; \
        for (int i = 0; i < 4; i++) r._f32[i] = a._f32[i] OP b._f32[i]; \
        if (spu_xf_plain4(a) & spu_xf_plain4(b) & spu_xf_plain4(r)) return r; \
        for (int i = 0; i < 4; i++) r._u32[i] = spu_xf_put(spu_xf_get(a._u32[i]) OP spu_xf_get(b._u32[i])); \
        return r; }
#define SPU_XF_CMP2(name, F) \
    static inline u128 name(u128 a, u128 b) { u128 r; \
        if (spu_xf_plain4(a) & spu_xf_plain4(b)) { \
            for (int i = 0; i < 4; i++) { const float x = a._f32[i], y = b._f32[i]; r._u32[i] = (F) ? 0xFFFFFFFFu : 0u; } \
            return r; } \
        for (int i = 0; i < 4; i++) { const double x = spu_xf_get(a._u32[i]), y = spu_xf_get(b._u32[i]); \
            r._u32[i] = (F) ? 0xFFFFFFFFu : 0u; } \
        return r; }
#define SPU_XF_FMA(name, EXPR) \
    static inline u128 name(u128 a, u128 b, u128 c) { u128 r; \
        for (int i = 0; i < 4; i++) { const double x = a._f32[i], y = b._f32[i], z = c._f32[i]; r._f32[i] = (float)(EXPR); } \
        if (spu_xf_plain4(a) & spu_xf_plain4(b) & spu_xf_plain4(c) & spu_xf_plain4(r)) return r; \
        for (int i = 0; i < 4; i++) { const double x = spu_xf_get(a._u32[i]), y = spu_xf_get(b._u32[i]), \
            z = spu_xf_get(c._u32[i]); r._u32[i] = spu_xf_put(EXPR); } \
        return r; }

/* FI decodes the base and step fields produced by the hardware estimate
 * instructions; its second operand is not an ordinary reciprocal float. The
 * base is an extended-range number: frest of zero gives 0x7FFFFFFF, whose
 * base 0x7FFFFC00 the IEEE version turned into a NaN. */
extern volatile long g_spu_xf_exact[8];   /* SPU_XF_STATS: exact-path calls (spu_drain.c) */
/* ...counted only with SPU_XF_STATS set: every lane bumping one shared line on
 * each extended-range op (millions a second in fights) bounced it between
 * cores for a report nobody asked for. A read of a flag that is never
 * written stays in every core's cache. */
extern int g_spu_xf_stats_on;
#define SPU_XF_COUNT(k) do { if (__builtin_expect(g_spu_xf_stats_on, 0)) g_spu_xf_exact[k]++; } while (0)
static inline __attribute__((noinline)) u128 spu_fi_exact(u128 a, u128 b) {
    u128 r; SPU_XF_COUNT(3);
    for (int i = 0; i < 4; i++) {
        uint32_t rb = b._u32[i];
        uint32_t ra = a._u32[i];
        uint32_t exp = (rb >> 23) & 0xFFu;
        double base = spu_xf_get(rb & 0xFFFFFC00u);
        double step = ldexp((double)(rb & 0x3FFu), (int)exp - 127 - 13);
        if (base < 0.0) step = -step;
        double y = ldexp((double)(ra & 0x7FFFFu), -19);
        r._u32[i] = spu_xf_put(base - step * y);
    }
    return r;
}

#if !(defined(__SSE2__) || defined(_M_X64))
static inline u128 spu_fi(u128 a, u128 b) { return spu_fi_exact(a, b); }
#endif
static inline u128 spu_mpyu(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[i*2]*(uint32_t)b._u16[i*2]; return r; }
static inline u128 spu_mpyi(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int16_t)imm; return r; }

/* ---- bitwise logic (whole 128 bits) ---- */
static inline u128 spu_and(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]&b._u64[0]; r._u64[1]=a._u64[1]&b._u64[1]; return r; }
static inline u128 spu_or(u128 a, u128 b)  { u128 r; r._u64[0]=a._u64[0]|b._u64[0]; r._u64[1]=a._u64[1]|b._u64[1]; return r; }
static inline u128 spu_xor(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]^b._u64[0]; r._u64[1]=a._u64[1]^b._u64[1]; return r; }
static inline u128 spu_nand(u128 a, u128 b){ u128 r; r._u64[0]=~(a._u64[0]&b._u64[0]); r._u64[1]=~(a._u64[1]&b._u64[1]); return r; }
static inline u128 spu_nor(u128 a, u128 b) { u128 r; r._u64[0]=~(a._u64[0]|b._u64[0]); r._u64[1]=~(a._u64[1]|b._u64[1]); return r; }
static inline u128 spu_andc(u128 a, u128 b){ u128 r; r._u64[0]=a._u64[0]&~b._u64[0]; r._u64[1]=a._u64[1]&~b._u64[1]; return r; }
static inline u128 spu_orc(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]|~b._u64[0]; r._u64[1]=a._u64[1]|~b._u64[1]; return r; }
static inline u128 spu_andi(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]&(uint32_t)imm; return r; }
static inline u128 spu_ori(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]|(uint32_t)imm; return r; }
static inline u128 spu_xori(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]^(uint32_t)imm; return r; }

#if defined(__SSSE3__)
#include <tmmintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif
/* ---- count leading zeros / population count per byte ---- */
static inline u128 spu_clz(u128 a)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)spu_clz32(a._u32[i]); return r; }
static inline u128 spu_cntb(u128 a) {
#if defined(__SSSE3__)
    { __m128i x; memcpy(&x, &a, 16);
      const __m128i lut = _mm_setr_epi8(0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4), m = _mm_set1_epi8(0x0F);
      x = _mm_add_epi8(_mm_shuffle_epi8(lut, _mm_and_si128(x, m)),
                       _mm_shuffle_epi8(lut, _mm_and_si128(_mm_srli_epi16(x, 4), m)));
      u128 r; memcpy(&r, &x, 16); return r; }
#elif defined(__aarch64__)
    { u128 r; vst1q_u8(r._u8, vcntq_u8(vld1q_u8(a._u8))); return r; }
#endif
    u128 r; for(int i=0;i<16;i++){ uint8_t v=a._u8[i],c=0; while(v){c+=v&1;v>>=1;} r._u8[i]=c; } return r; }

/* ---- compares: all-ones / all-zeros per lane ---- */
static inline u128 spu_ceq(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]==b._u32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_ceqh(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]==b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_ceqb(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_cgt(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]>b._s32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_cgth(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>b._s16[i])?0xFFFFu:0; return r; }
static inline u128 spu_cgtb(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>b._s8[i])?0xFFu:0; return r; }
static inline u128 spu_clgt(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]>b._u32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_clgth(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_clgtb(u128 a, u128 b){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_ceqi(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]==imm)?0xFFFFFFFFu:0; return r; }
static inline u128 spu_cgti(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]>imm)?0xFFFFFFFFu:0; return r; }
static inline u128 spu_clgti(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]>(uint32_t)imm)?0xFFFFFFFFu:0; return r; }

/* ---- select / shuffle ---- */
static inline u128 spu_selb(u128 a, u128 b, u128 c) {
    u128 r;
    r._u64[0]=(a._u64[0]&~c._u64[0])|(b._u64[0]&c._u64[0]);
    r._u64[1]=(a._u64[1]&~c._u64[1])|(b._u64[1]&c._u64[1]);
    return r;
}
/* shufb special selectors per Cell BE ISA:
 *   sel & 0xE0 == 0xE0 -> 0x80
 *   sel & 0xC0 == 0xC0 -> 0xFF
 *   sel & 0xC0 == 0x80 -> 0x00
 *   otherwise           -> concat{a,b}[sel & 0x1F] */
#if defined(__SSE4_1__)
#include <smmintrin.h>
/* pshufb version: one of the hottest SPU ops (~11% of GH3's FMOD mixer task
 * as the byte loop; much of A Crack in Time's SPU job time, which the main
 * thread waits on every frame). SPU byte P lives at host byte SPU_W(P), the
 * bytes of each 32-bit word reversed. That map is its own inverse, so instead
 * of swapping a, b and c into SPU order and the result back (four pshufb),
 * the control stays in host order and only its source index is mapped:
 * result host byte h = (a|b)[SPU_W(c[h] & 15)]. The special selectors (any
 * control byte >= 0x80) are rare and take a branch. Checked against the byte
 * loop on 5M random a/b/c including every special code. */
static inline u128 spu_shufb(u128 a, u128 b, u128 c) {
    const __m128i sw = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12);
    __m128i A, B, C; memcpy(&A, &a, 16); memcpy(&B, &b, 16); memcpy(&C, &c, 16);
    const __m128i idx = _mm_shuffle_epi8(sw, _mm_and_si128(C, _mm_set1_epi8(0x0F)));
    const __m128i fromb = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
    __m128i r = _mm_blendv_epi8(_mm_shuffle_epi8(A, idx), _mm_shuffle_epi8(B, idx), fromb);
    if (__builtin_expect(_mm_movemask_epi8(C) != 0, 0)) {
        /* Special selectors: 10x -> 0x00, 110 -> 0xFF, 111 -> 0x80. */
        const __m128i m80 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8((char)0x80)), _mm_set1_epi8((char)0x80));
        const __m128i mC0 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8((char)0xC0)), _mm_set1_epi8((char)0xC0));
        const __m128i mE0 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8((char)0xE0)), _mm_set1_epi8((char)0xE0));
        const __m128i special = _mm_blendv_epi8(mC0, _mm_set1_epi8((char)0x80), mE0);
        r = _mm_blendv_epi8(r, special, m80);
    }
    u128 out; memcpy(&out, &r, 16); return out;
}
/* The byte loop stays as the reference (tests/test_spu_shufb.c checks both). */
static inline u128 spu_shufb_ref(u128 a, u128 b, u128 c) {
    /* shufb is defined on SPU byte positions. Our u128 is host-native LE, so SPU
     * byte P lives at _u8[SPU_W(P)]; map every access (the concat source, the
     * control, and the result) through SPU_W so a control supplied as an immediate
     * (ila/il) or an LS-loaded constant -- already in true SPU byte order -- is
     * interpreted correctly. The cbd/chd/cwd/cdd generators below produce true
     * SPU-byte-order selectors to match. */
    uint8_t cat[32];
    for (int j=0;j<16;j++) cat[j]    = a._u8[SPU_W(j)];   /* concat SPU byte j  = a SPU byte j */
    for (int j=0;j<16;j++) cat[16+j] = b._u8[SPU_W(j)];   /* concat SPU byte 16+j = b SPU byte j */
    u128 r;
    for (int t=0;t<16;t++) {                              /* result SPU byte t */
        uint8_t s = c._u8[SPU_W(t)];                      /* control SPU byte t */
        uint8_t v;
        if      ((s & 0xE0)==0xE0) v=0x80;
        else if ((s & 0xC0)==0xC0) v=0xFF;
        else if ((s & 0xC0)==0x80) v=0x00;
        else                       v=cat[s & 0x1F];       /* concat SPU byte (s & 0x1F) */
        r._u8[SPU_W(t)] = v;
    }
    return r;
}
#elif defined(__aarch64__)
#include <arm_neon.h>
/* NEON: the same as the pshufb version, a two-register table lookup. */
static inline u128 spu_shufb(u128 a, u128 b, u128 c) {
    const uint8x16_t sw = { 3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12 };
    const uint8x16_t A = vqtbl1q_u8(vld1q_u8(a._u8), sw), B = vqtbl1q_u8(vld1q_u8(b._u8), sw);
    const uint8x16_t C = vqtbl1q_u8(vld1q_u8(c._u8), sw);
    const uint8x16x2_t AB = { { A, B } };
    uint8x16_t r = vqtbl2q_u8(AB, vandq_u8(C, vdupq_n_u8(0x1F)));
    /* Special selectors: 10x -> 0x00, 110 -> 0xFF, 111 -> 0x80. */
    const uint8x16_t m80 = vtstq_u8(C, vdupq_n_u8(0x80));
    const uint8x16_t mC0 = vceqq_u8(vandq_u8(C, vdupq_n_u8(0xC0)), vdupq_n_u8(0xC0));
    const uint8x16_t mE0 = vceqq_u8(vandq_u8(C, vdupq_n_u8(0xE0)), vdupq_n_u8(0xE0));
    const uint8x16_t special = vbslq_u8(mE0, vdupq_n_u8(0x80), mC0);
    r = vqtbl1q_u8(vbslq_u8(m80, special, r), sw);
    u128 out; vst1q_u8(out._u8, r); return out;
}
static inline u128 spu_shufb_ref(u128 a, u128 b, u128 c) {
    uint8_t cat[32];
    for (int j=0;j<16;j++) cat[j]    = a._u8[SPU_W(j)];
    for (int j=0;j<16;j++) cat[16+j] = b._u8[SPU_W(j)];
    u128 r;
    for (int t=0;t<16;t++) {
        uint8_t s = c._u8[SPU_W(t)], v;
        if      ((s & 0xE0)==0xE0) v=0x80;
        else if ((s & 0xC0)==0xC0) v=0xFF;
        else if ((s & 0xC0)==0x80) v=0x00;
        else                       v=cat[s & 0x1F];
        r._u8[SPU_W(t)] = v;
    }
    return r;
}
#else
static inline u128 spu_shufb(u128 a, u128 b, u128 c) {
    /* shufb is defined on SPU byte positions. Our u128 is host-native LE, so SPU
     * byte P lives at _u8[SPU_W(P)]; map every access (the concat source, the
     * control, and the result) through SPU_W so a control supplied as an immediate
     * (ila/il) or an LS-loaded constant -- already in true SPU byte order -- is
     * interpreted correctly. The cbd/chd/cwd/cdd generators below produce true
     * SPU-byte-order selectors to match. */
    uint8_t cat[32];
    for (int j=0;j<16;j++) cat[j]    = a._u8[SPU_W(j)];   /* concat SPU byte j  = a SPU byte j */
    for (int j=0;j<16;j++) cat[16+j] = b._u8[SPU_W(j)];   /* concat SPU byte 16+j = b SPU byte j */
    u128 r;
    for (int t=0;t<16;t++) {                              /* result SPU byte t */
        uint8_t s = c._u8[SPU_W(t)];                      /* control SPU byte t */
        uint8_t v;
        if      ((s & 0xE0)==0xE0) v=0x80;
        else if ((s & 0xC0)==0xC0) v=0xFF;
        else if ((s & 0xC0)==0x80) v=0x00;
        else                       v=cat[s & 0x1F];       /* concat SPU byte (s & 0x1F) */
        r._u8[SPU_W(t)] = v;
    }
    return r;
}
#define spu_shufb_ref spu_shufb
#endif

/* SSE fast paths for the quadword byte rotates/shifts. `lqd` + `rotqby` is how
 * SPU code does every unaligned scalar load, so these run constantly; the byte
 * loops below were a large share of SPU time. One pshufb with a precomputed
 * control (SPU byte order folded in: host byte h <- host byte
 * SPU_W((SPU_W(h)+sh) & 15)) does the whole thing. */
#if defined(__SSE4_1__) || defined(__aarch64__)
#define SPU_HAS_BYTE_TABLE 1
static const uint8_t spu__rotqby_ctl[16][16] __attribute__((aligned(16))) = {
    {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F},
    {0x07,0x00,0x01,0x02,0x0B,0x04,0x05,0x06,0x0F,0x08,0x09,0x0A,0x03,0x0C,0x0D,0x0E},
    {0x06,0x07,0x00,0x01,0x0A,0x0B,0x04,0x05,0x0E,0x0F,0x08,0x09,0x02,0x03,0x0C,0x0D},
    {0x05,0x06,0x07,0x00,0x09,0x0A,0x0B,0x04,0x0D,0x0E,0x0F,0x08,0x01,0x02,0x03,0x0C},
    {0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x00,0x01,0x02,0x03},
    {0x0B,0x04,0x05,0x06,0x0F,0x08,0x09,0x0A,0x03,0x0C,0x0D,0x0E,0x07,0x00,0x01,0x02},
    {0x0A,0x0B,0x04,0x05,0x0E,0x0F,0x08,0x09,0x02,0x03,0x0C,0x0D,0x06,0x07,0x00,0x01},
    {0x09,0x0A,0x0B,0x04,0x0D,0x0E,0x0F,0x08,0x01,0x02,0x03,0x0C,0x05,0x06,0x07,0x00},
    {0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07},
    {0x0F,0x08,0x09,0x0A,0x03,0x0C,0x0D,0x0E,0x07,0x00,0x01,0x02,0x0B,0x04,0x05,0x06},
    {0x0E,0x0F,0x08,0x09,0x02,0x03,0x0C,0x0D,0x06,0x07,0x00,0x01,0x0A,0x0B,0x04,0x05},
    {0x0D,0x0E,0x0F,0x08,0x01,0x02,0x03,0x0C,0x05,0x06,0x07,0x00,0x09,0x0A,0x0B,0x04},
    {0x0C,0x0D,0x0E,0x0F,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B},
    {0x03,0x0C,0x0D,0x0E,0x07,0x00,0x01,0x02,0x0B,0x04,0x05,0x06,0x0F,0x08,0x09,0x0A},
    {0x02,0x03,0x0C,0x0D,0x06,0x07,0x00,0x01,0x0A,0x0B,0x04,0x05,0x0E,0x0F,0x08,0x09},
    {0x01,0x02,0x03,0x0C,0x05,0x06,0x07,0x00,0x09,0x0A,0x0B,0x04,0x0D,0x0E,0x0F,0x08}
};
static const uint8_t spu__shlqby_ctl[16][16] __attribute__((aligned(16))) = {
    {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F},
    {0x07,0x00,0x01,0x02,0x0B,0x04,0x05,0x06,0x0F,0x08,0x09,0x0A,0x80,0x0C,0x0D,0x0E},
    {0x06,0x07,0x00,0x01,0x0A,0x0B,0x04,0x05,0x0E,0x0F,0x08,0x09,0x80,0x80,0x0C,0x0D},
    {0x05,0x06,0x07,0x00,0x09,0x0A,0x0B,0x04,0x0D,0x0E,0x0F,0x08,0x80,0x80,0x80,0x0C},
    {0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x80,0x80,0x80,0x80},
    {0x0B,0x04,0x05,0x06,0x0F,0x08,0x09,0x0A,0x80,0x0C,0x0D,0x0E,0x80,0x80,0x80,0x80},
    {0x0A,0x0B,0x04,0x05,0x0E,0x0F,0x08,0x09,0x80,0x80,0x0C,0x0D,0x80,0x80,0x80,0x80},
    {0x09,0x0A,0x0B,0x04,0x0D,0x0E,0x0F,0x08,0x80,0x80,0x80,0x0C,0x80,0x80,0x80,0x80},
    {0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80},
    {0x0F,0x08,0x09,0x0A,0x80,0x0C,0x0D,0x0E,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80},
    {0x0E,0x0F,0x08,0x09,0x80,0x80,0x0C,0x0D,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80},
    {0x0D,0x0E,0x0F,0x08,0x80,0x80,0x80,0x0C,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80},
    {0x0C,0x0D,0x0E,0x0F,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80},
    {0x80,0x0C,0x0D,0x0E,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80},
    {0x80,0x80,0x0C,0x0D,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80},
    {0x80,0x80,0x80,0x0C,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80}
};

#if defined(__SSE4_1__)
static inline u128 spu__pshufb_tab(u128 a, const uint8_t* ctl) {
    __m128i A, C; memcpy(&A, &a, 16); C = _mm_load_si128((const __m128i*)ctl);
    A = _mm_shuffle_epi8(A, C); u128 r; memcpy(&r, &A, 16); return r;
}
#else
static inline u128 spu__pshufb_tab(u128 a, const uint8_t* ctl) {   /* index 0x80 reads 0, as pshufb */
    u128 r; vst1q_u8(r._u8, vqtbl1q_u8(vld1q_u8(a._u8), vld1q_u8(ctl))); return r;
}
#endif
#define SPU__ROTQBY(a, sh) spu__pshufb_tab((a), spu__rotqby_ctl[(sh) & 15])
#define SPU__SHLQBY(a, sh) (((sh) >= 16) ? spu_zero() : spu__pshufb_tab((a), spu__shlqby_ctl[(sh) & 15]))
/* rotqmby (shift right by sh bytes, zero fill) and the cbd/chd/cwd/cdd
 * insertion controls, generated from the byte-loop definitions below
 * (scripts: session note in HANDOFF). */
static const uint8_t spu__rotqmby_ctl[16][16] __attribute__((aligned(16))) = {
    {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F},
    {0x01,0x02,0x03,0x80,0x05,0x06,0x07,0x00,0x09,0x0A,0x0B,0x04,0x0D,0x0E,0x0F,0x08},
    {0x02,0x03,0x80,0x80,0x06,0x07,0x00,0x01,0x0A,0x0B,0x04,0x05,0x0E,0x0F,0x08,0x09},
    {0x03,0x80,0x80,0x80,0x07,0x00,0x01,0x02,0x0B,0x04,0x05,0x06,0x0F,0x08,0x09,0x0A},
    {0x80,0x80,0x80,0x80,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B},
    {0x80,0x80,0x80,0x80,0x01,0x02,0x03,0x80,0x05,0x06,0x07,0x00,0x09,0x0A,0x0B,0x04},
    {0x80,0x80,0x80,0x80,0x02,0x03,0x80,0x80,0x06,0x07,0x00,0x01,0x0A,0x0B,0x04,0x05},
    {0x80,0x80,0x80,0x80,0x03,0x80,0x80,0x80,0x07,0x00,0x01,0x02,0x0B,0x04,0x05,0x06},
    {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07},
    {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x01,0x02,0x03,0x80,0x05,0x06,0x07,0x00},
    {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x02,0x03,0x80,0x80,0x06,0x07,0x00,0x01},
    {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x03,0x80,0x80,0x80,0x07,0x00,0x01,0x02},
    {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x00,0x01,0x02,0x03},
    {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x01,0x02,0x03,0x80},
    {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x02,0x03,0x80,0x80},
    {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x03,0x80,0x80,0x80}
};
static const uint8_t spu__cbd_ctl[16][16] __attribute__((aligned(16))) = {
    {0x13,0x12,0x11,0x03,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x03,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x03,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x03,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x03,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x03,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x03,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x03,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x03,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x03,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x03,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x03,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x03},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x03,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x03,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x03,0x1E,0x1D,0x1C}
};
static const uint8_t spu__chd_ctl[8][16] __attribute__((aligned(16))) = {
    {0x13,0x12,0x03,0x02,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x03,0x02,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x03,0x02,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x03,0x02,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x03,0x02,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x03,0x02,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x03,0x02},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x03,0x02,0x1D,0x1C}
};
static const uint8_t spu__cwd_ctl[4][16] __attribute__((aligned(16))) = {
    {0x03,0x02,0x01,0x00,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x03,0x02,0x01,0x00,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x03,0x02,0x01,0x00,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x1B,0x1A,0x19,0x18,0x03,0x02,0x01,0x00}
};
static const uint8_t spu__cdd_ctl[2][16] __attribute__((aligned(16))) = {
    {0x03,0x02,0x01,0x00,0x07,0x06,0x05,0x04,0x1B,0x1A,0x19,0x18,0x1F,0x1E,0x1D,0x1C},
    {0x13,0x12,0x11,0x10,0x17,0x16,0x15,0x14,0x03,0x02,0x01,0x00,0x07,0x06,0x05,0x04}
};
#define SPU__ROTQMBY(a, sh) (((sh) >= 16) ? spu_zero() : spu__pshufb_tab((a), spu__rotqmby_ctl[(sh) & 15]))
static inline u128 spu__cxd_row(const uint8_t* row) { u128 r; memcpy(&r, row, 16); return r; }
#define SPU__CXD(tab, idx) spu__cxd_row((tab)[(idx)])
#endif


/* Quadword bit shifts by 1-7 in SSE2. The u128 holds SPU word 0 first, so
 * with its words reversed it is one little-endian 128-bit integer in SPU
 * bit order: shift both 64-bit halves and carry across with a byte shift,
 * instead of assembling and splitting scalar halves. */
#if defined(__SSE2__)
#include <emmintrin.h>
static inline __m128i spu__q_le(u128 a) { __m128i x; memcpy(&x, &a, 16); return _mm_shuffle_epi32(x, 0x1B); }
static inline u128 spu__q_from_le(__m128i x) { x = _mm_shuffle_epi32(x, 0x1B); u128 r; memcpy(&r, &x, 16); return r; }
static inline u128 spu__qshl(u128 a, int sh) {
    const __m128i v = spu__q_le(a), c = _mm_cvtsi32_si128(sh), c2 = _mm_cvtsi32_si128(64 - sh);
    return spu__q_from_le(_mm_or_si128(_mm_sll_epi64(v, c), _mm_srl_epi64(_mm_slli_si128(v, 8), c2))); }
static inline u128 spu__qrotl(u128 a, int sh) {
    const __m128i v = spu__q_le(a), c = _mm_cvtsi32_si128(sh), c2 = _mm_cvtsi32_si128(64 - sh);
    return spu__q_from_le(_mm_or_si128(_mm_sll_epi64(v, c), _mm_srl_epi64(_mm_shuffle_epi32(v, 0x4E), c2))); }
static inline u128 spu__qshr(u128 a, int sh) {
    const __m128i v = spu__q_le(a), c = _mm_cvtsi32_si128(sh), c2 = _mm_cvtsi32_si128(64 - sh);
    return spu__q_from_le(_mm_or_si128(_mm_srl_epi64(v, c), _mm_sll_epi64(_mm_srli_si128(v, 8), c2))); }
#define SPU_QSHL(a, sh)  return spu__qshl((a), (sh));
#define SPU_QROTL(a, sh) return spu__qrotl((a), (sh));
#define SPU_QSHR(a, sh)  return spu__qshr((a), (sh));
#else
#define SPU_QSHL(a, sh)
#define SPU_QROTL(a, sh)
#define SPU_QSHR(a, sh)
#endif
/* ---- shift / rotate immediate (word lanes) ---- */
static inline u128 spu_shli(u128 a, int sh)  { u128 r; sh&=0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]<<sh); return r; }
static inline u128 spu_shlhi(u128 a, int sh) { u128 r; sh&=0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]<<sh); return r; }
static inline u128 spu_roti(u128 a, int sh)  { u128 r; sh&=31; for(int i=0;i<4;i++) r._u32[i]= sh ? ((a._u32[i]<<sh)|(a._u32[i]>>(32-sh))) : a._u32[i]; return r; }
static inline u128 spu_rothi(u128 a, int sh) { u128 r; sh&=15; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))); return r; }
static inline u128 spu_rotmi(u128 a, int i7)  { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]>>sh); return r; }
static inline u128 spu_rotmai(u128 a, int i7) { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._s32[i]=(sh>31)?(a._s32[i]>>31):(a._s32[i]>>sh); return r; }
static inline u128 spu_rotmhi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
static inline u128 spu_shlqbyi(u128 a, int sh) {
#if defined(SPU_HAS_BYTE_TABLE)
    sh&=0x1F; return SPU__SHLQBY(a, sh);
#endif
    u128 r=spu_zero(); sh&=0x1F; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqbyi(u128 a, int sh) {
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__ROTQBY(a, sh);
#endif
    u128 r; sh&=0x0F; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
/* Bit-level quadword shifts/rotates operate on the WHOLE 128-bit big-endian
 * value. Our _u64[0]/_u64[1] are word-SCRAMBLED (host-LE: _u64[0] = word0 |
 * word1<<32, but SPU's MS half is word0<<32 | word1), so native shifts on them
 * corrupt any bits crossing a 32-bit boundary. Assemble the logical hi/lo halves
 * with _u32[0] as most-significant, shift, then disassemble. */
static inline u128 spu_shlqbii(u128 a, int sh) { sh&=7; if(!sh) return a; SPU_QSHL(a, sh)
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqbii(u128 a, int sh) { sh&=7; if(!sh) return a; SPU_QROTL(a, sh)
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh)|(hi>>(64-sh));
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }

/* ---- single-precision float (4 lanes) ---- */
/* Extended-range single precision throughout (spu_xf_get/put above). A
 * product of two singles is exact in double, so fm is exact before the
 * truncation; fa/fs/fma round once in double first, which differs from the
 * SPU only in rare double-rounding cases. */
#if defined(__SSE2__) || defined(_M_X64)
/* x86: the same results four lanes at a time (IEEE single ops for the fast
 * path, the plain-lane test as integer compares); the exact path is shared. */
#include <emmintrin.h>
static inline __attribute__((always_inline)) int spu_xf_plain_v(__m128i v)
{
    const __m128i e = _mm_and_si128(v, _mm_set1_epi32(0x7F800000));
    const __m128i m = _mm_and_si128(v, _mm_set1_epi32(0x007FFFFF));
    const __m128i z = _mm_setzero_si128();
    const __m128i bad = _mm_or_si128(_mm_cmpeq_epi32(e, _mm_set1_epi32(0x7F800000)),
                                     _mm_andnot_si128(_mm_cmpeq_epi32(m, z), _mm_cmpeq_epi32(e, z)));
    return _mm_movemask_epi8(bad) == 0;
}
/* SPU code runs with DAZ and FTZ set (spu_run_with_halt): denormal operands
 * read as zero and denormal results flush to zero, exactly as on the SPU.
 * What is left to tell the two apart is exponent 255 -- a large finite number
 * to the SPU, Inf/NaN to IEEE -- and an IEEE operation turns any such operand
 * (and any overflow) into an exponent-255 result. So arithmetic checks only
 * its result, compares only their operands: a few instructions where the
 * full test of every operand was most of a float op's cost. The rounding
 * mode is toward zero there too, so the fast results are the SPU's exactly. */
static inline __attribute__((always_inline)) int spu_xf_e255_v(__m128i v)
{
    const __m128i e = _mm_set1_epi32(0x7F800000);
    return _mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(v, e), e)) != 0;
}
#if defined(__AVX2__)
#include <immintrin.h>
/* spu_xf_get / spu_xf_put four lanes at a time (AVX2): the extended-range
 * path without a scalar loop or a call. Same double arithmetic, same rounding
 * mode, so the same results -- it is what the exact paths below run when a
 * vector has an exponent-255 lane or overflows, which some jobs do millions
 * of times a second (frest(0) = 0x7FFFFFFF in a padding lane). */
static inline __attribute__((always_inline)) __m256d spu_xf_get4(__m128i v)
{
    const __m256i u = _mm256_cvtepu32_epi64(v);
    const __m256i e = _mm256_and_si256(_mm256_srli_epi64(u, 23), _mm256_set1_epi64x(0xFF));
    const __m256i sign = _mm256_slli_epi64(_mm256_and_si256(u, _mm256_set1_epi64x(0x80000000)), 32);
    const __m256i bits = _mm256_or_si256(_mm256_or_si256(sign,
        _mm256_slli_epi64(_mm256_add_epi64(e, _mm256_set1_epi64x(896)), 52)),
        _mm256_slli_epi64(_mm256_and_si256(u, _mm256_set1_epi64x(0x7FFFFF)), 29));
    return _mm256_castsi256_pd(_mm256_blendv_epi8(bits, sign, _mm256_cmpeq_epi64(e, _mm256_setzero_si256())));
}
static inline __attribute__((always_inline)) __m128i spu_xf_put4(__m256d d)
{
    const __m256i u = _mm256_castpd_si256(d);
    const __m256i sign = _mm256_and_si256(_mm256_srli_epi64(u, 32), _mm256_set1_epi64x(0x80000000));
    const __m256i e = _mm256_sub_epi64(_mm256_and_si256(_mm256_srli_epi64(u, 52), _mm256_set1_epi64x(0x7FF)),
                                       _mm256_set1_epi64x(896));
    __m256i r = _mm256_or_si256(_mm256_or_si256(sign, _mm256_slli_epi64(e, 23)),
                                _mm256_and_si256(_mm256_srli_epi64(u, 29), _mm256_set1_epi64x(0x7FFFFF)));
    r = _mm256_blendv_epi8(r, sign, _mm256_cmpgt_epi64(_mm256_set1_epi64x(1), e));               /* e <= 0 */
    r = _mm256_blendv_epi8(r, _mm256_or_si256(sign, _mm256_set1_epi64x(0x7FFFFFFF)),
                           _mm256_cmpgt_epi64(e, _mm256_set1_epi64x(255)));                       /* e > 255 */
    return _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(r, _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6)));
}
#endif
/* A result to redo exactly: exponent 255, or +-FLT_MAX -- with rounding toward
 * zero (as the SPU rounds) an IEEE overflow lands there instead of on Inf. */
static inline __attribute__((always_inline)) int spu_xf_bad_v(__m128i v)
{
    const __m128i ab = _mm_and_si128(v, _mm_set1_epi32(0x7FFFFFFF));
    return _mm_movemask_epi8(_mm_cmpgt_epi32(ab, _mm_set1_epi32(0x7F7FFFFE))) != 0;
}

#if defined(__AVX2__)
#include <immintrin.h>
/* The same arithmetic four lanes at a time in double: base is b with its low
 * ten bits cleared (DAZ reads exponent 0 as a signed zero, as spu_xf_get
 * does), step = (b & 0x3FF) * 2^(exp-140) built from its exponent, y =
 * (a & 0x7FFFF) * 2^-19; base - step*y rounds toward zero to single (the SPU
 * mode). b with exponent 255, or a result at FLT_MAX or beyond, goes exact. */
static inline __attribute__((always_inline)) u128 spu_fi(u128 a, u128 b) {
    const __m128i va = _mm_loadu_si128((const __m128i*)a._u32), vb = _mm_loadu_si128((const __m128i*)b._u32);
    if (__builtin_expect(!spu_xf_e255_v(vb), 1)) {
        const __m256d base = _mm256_cvtps_pd(_mm_castsi128_ps(_mm_and_si128(vb, _mm_set1_epi32((int)0xFFFFFC00u))));
        const __m128i ex = _mm_and_si128(_mm_srli_epi32(vb, 23), _mm_set1_epi32(0xFF));
        const __m256d p2 = _mm256_castsi256_pd(_mm256_slli_epi64(
            _mm256_add_epi64(_mm256_cvtepu32_epi64(ex), _mm256_set1_epi64x(1023 - 140)), 52));
        __m256d step = _mm256_mul_pd(_mm256_cvtepi32_pd(_mm_and_si128(vb, _mm_set1_epi32(0x3FF))), p2);
        const __m256d neg = _mm256_cmp_pd(base, _mm256_setzero_pd(), _CMP_LT_OQ);
        step = _mm256_blendv_pd(step, _mm256_sub_pd(_mm256_setzero_pd(), step), neg);
        const __m256d y = _mm256_mul_pd(_mm256_cvtepi32_pd(_mm_and_si128(va, _mm_set1_epi32(0x7FFFF))),
                                        _mm256_set1_pd(1.0 / 524288.0));
        const __m128 vr = _mm256_cvtpd_ps(_mm256_sub_pd(base, _mm256_mul_pd(step, y)));
        if (__builtin_expect(!spu_xf_bad_v(_mm_castps_si128(vr)), 1)) { u128 r; _mm_storeu_ps(r._f32, vr); return r; }
    }
    {   /* extended range: base decoded the SPU's way, result encoded likewise */
        SPU_XF_COUNT(7);
        const __m256d base = spu_xf_get4(_mm_and_si128(vb, _mm_set1_epi32((int)0xFFFFFC00u)));
        const __m128i ex = _mm_and_si128(_mm_srli_epi32(vb, 23), _mm_set1_epi32(0xFF));
        const __m256d p2 = _mm256_castsi256_pd(_mm256_slli_epi64(
            _mm256_add_epi64(_mm256_cvtepu32_epi64(ex), _mm256_set1_epi64x(1023 - 140)), 52));
        __m256d step = _mm256_mul_pd(_mm256_cvtepi32_pd(_mm_and_si128(vb, _mm_set1_epi32(0x3FF))), p2);
        const __m256d neg = _mm256_cmp_pd(base, _mm256_setzero_pd(), _CMP_LT_OQ);
        step = _mm256_blendv_pd(step, _mm256_sub_pd(_mm256_setzero_pd(), step), neg);
        const __m256d y = _mm256_mul_pd(_mm256_cvtepi32_pd(_mm_and_si128(va, _mm_set1_epi32(0x7FFFF))),
                                        _mm256_set1_pd(1.0 / 524288.0));
        u128 r; _mm_storeu_si128((__m128i*)r._u32, spu_xf_put4(_mm256_sub_pd(base, _mm256_mul_pd(step, y))));
        return r;
    }
}
#else
static inline u128 spu_fi(u128 a, u128 b) { return spu_fi_exact(a, b); }
#endif
/* The fast path inlined (a call passes and returns every u128 through memory
 * on Win64); lanes the SPU treats differently from IEEE take the exact path
 * out of line. */
#if defined(__AVX2__)
#define SPU_XF_EXT2(name, V256) \
        { SPU_XF_COUNT(5); const __m256d x = spu_xf_get4(_mm_castps_si128(va)), y = spu_xf_get4(_mm_castps_si128(vb)); \
          _mm_storeu_si128((__m128i*)r._u32, spu_xf_put4(V256)); return r; }
#else
#define SPU_XF_EXT2(name, V256) return name##_exact(a, b);
#endif
#define SPU_XF_ARITH2_V(name, OP, VOP, V256) \
    static inline __attribute__((noinline)) u128 name##_exact(u128 a, u128 b) { u128 r; SPU_XF_COUNT(0); \
        for (int i = 0; i < 4; i++) r._u32[i] = spu_xf_put(spu_xf_get(a._u32[i]) OP spu_xf_get(b._u32[i])); \
        return r; } \
    static inline __attribute__((always_inline)) u128 name(u128 a, u128 b) { u128 r; \
        const __m128 va = _mm_loadu_ps(a._f32), vb = _mm_loadu_ps(b._f32), vr = VOP(va, vb); \
        _mm_storeu_ps(r._f32, vr); \
        if (__builtin_expect(!spu_xf_bad_v(_mm_castps_si128(vr)), 1)) return r; \
        SPU_XF_EXT2(name, V256) }
SPU_XF_ARITH2_V(spu_fa, +, _mm_add_ps, _mm256_add_pd(x, y))
SPU_XF_ARITH2_V(spu_fs, -, _mm_sub_ps, _mm256_sub_pd(x, y))
SPU_XF_ARITH2_V(spu_fm, *, _mm_mul_ps, _mm256_mul_pd(x, y))
#elif defined(__aarch64__)
/* NEON, with the FPCR set as MXCSR is on x86 (spu_run_with_halt: flush to
 * zero, round toward zero): the IEEE op is the SPU's result unless it comes
 * out at exponent 255 or +-FLT_MAX, which go exact. */
#include <arm_neon.h>
static inline __attribute__((always_inline)) int spu_xf_e255_n(uint32x4_t v)
{
    const uint32x4_t e = vdupq_n_u32(0x7F800000u);
    return vmaxvq_u32(vceqq_u32(vandq_u32(v, e), e)) != 0;
}
static inline __attribute__((always_inline)) int spu_xf_bad_n(uint32x4_t v)
{
    return vmaxvq_u32(vcgtq_u32(vandq_u32(v, vdupq_n_u32(0x7FFFFFFFu)), vdupq_n_u32(0x7F7FFFFEu))) != 0;
}
#define SPU_XF_ARITH2_N(name, OP, NOP) \
    static inline __attribute__((noinline)) u128 name##_exact(u128 a, u128 b) { u128 r; SPU_XF_COUNT(0); \
        for (int i = 0; i < 4; i++) r._u32[i] = spu_xf_put(spu_xf_get(a._u32[i]) OP spu_xf_get(b._u32[i])); \
        return r; } \
    static inline __attribute__((always_inline)) u128 name(u128 a, u128 b) { u128 r; \
        const float32x4_t vr = NOP(vld1q_f32(a._f32), vld1q_f32(b._f32)); \
        vst1q_f32(r._f32, vr); \
        if (__builtin_expect(!spu_xf_bad_n(vreinterpretq_u32_f32(vr)), 1)) return r; \
        return name##_exact(a, b); }
SPU_XF_ARITH2_N(spu_fa, +, vaddq_f32)
SPU_XF_ARITH2_N(spu_fs, -, vsubq_f32)
SPU_XF_ARITH2_N(spu_fm, *, vmulq_f32)
#else
SPU_XF_ARITH2(spu_fa, +)
SPU_XF_ARITH2(spu_fs, -)
SPU_XF_ARITH2(spu_fm, *)
#endif
#if defined(__SSE2__) || defined(_M_X64)
/* Compares four lanes at a time when every lane is a normal float or a zero
 * (where IEEE and the SPU agree); the scalar extended-range path otherwise. */
SPU_XF_CMP2(spu_fceq_exact, x == y)
SPU_XF_CMP2(spu_fcgt_exact, x > y)
static inline __attribute__((always_inline)) u128 spu_fceq(u128 a, u128 b) {
    const __m128 va = _mm_loadu_ps(a._f32), vb = _mm_loadu_ps(b._f32);
    if (__builtin_expect(!spu_xf_e255_v(_mm_castps_si128(va)) & !spu_xf_e255_v(_mm_castps_si128(vb)), 1)) {
        u128 r; _mm_storeu_ps(r._f32, _mm_cmpeq_ps(va, vb)); return r; }
    return spu_fceq_exact(a, b); }
static inline __attribute__((always_inline)) u128 spu_fcgt(u128 a, u128 b) {
    const __m128 va = _mm_loadu_ps(a._f32), vb = _mm_loadu_ps(b._f32);
    if (__builtin_expect(!spu_xf_e255_v(_mm_castps_si128(va)) & !spu_xf_e255_v(_mm_castps_si128(vb)), 1)) {
        u128 r; _mm_storeu_ps(r._f32, _mm_cmpgt_ps(va, vb)); return r; }
    return spu_fcgt_exact(a, b); }
#elif defined(__aarch64__)
SPU_XF_CMP2(spu_fceq_exact, x == y)
SPU_XF_CMP2(spu_fcgt_exact, x > y)
static inline __attribute__((always_inline)) u128 spu_fceq(u128 a, u128 b) {
    const uint32x4_t ia = vld1q_u32(a._u32), ib = vld1q_u32(b._u32);
    if (__builtin_expect(!spu_xf_e255_n(ia) & !spu_xf_e255_n(ib), 1)) {
        u128 r; vst1q_u32(r._u32, vceqq_f32(vreinterpretq_f32_u32(ia), vreinterpretq_f32_u32(ib))); return r; }
    return spu_fceq_exact(a, b); }
static inline __attribute__((always_inline)) u128 spu_fcgt(u128 a, u128 b) {
    const uint32x4_t ia = vld1q_u32(a._u32), ib = vld1q_u32(b._u32);
    if (__builtin_expect(!spu_xf_e255_n(ia) & !spu_xf_e255_n(ib), 1)) {
        u128 r; vst1q_u32(r._u32, vcgtq_f32(vreinterpretq_f32_u32(ia), vreinterpretq_f32_u32(ib))); return r; }
    return spu_fcgt_exact(a, b); }
#else
SPU_XF_CMP2(spu_fceq, x == y)
SPU_XF_CMP2(spu_fcgt, x > y)
#endif

/* RI8 scale: float-to-int uses 2^(173-i8), int-to-float 2^(i8-155).
 * The canonical immediates 173 and 155 are identity conversions. Int to
 * float truncates, like every SPU single-precision result. */
/* 2^n as a double, built from its exponent (exactly exp2(n) for the RI8
 * range, -255..255): exp2() per instruction was a libm call in hot loops. */
static inline double spu_pow2i(int n)
{
    if (n < -1022 || n > 1023) return exp2((double)n);
    const uint64_t u = (uint64_t)(n + 1023) << 52;
    double d; memcpy(&d, &u, sizeof d); return d;
}
static inline u128 spu_cflts_exact(u128 a, int i8){ u128 r; SPU_XF_COUNT(4); double f=spu_pow2i(173-i8);
    for(int i=0;i<4;i++){ double v=spu_xf_get(a._u32[i])*f; if(v>2147483647.0)v=2147483647.0; if(v<-2147483648.0)v=-2147483648.0; r._s32[i]=(int32_t)v; } return r; }
static inline u128 spu_cfltu_exact(u128 a, int i8){ u128 r; SPU_XF_COUNT(4); double f=spu_pow2i(173-i8);
    for(int i=0;i<4;i++){ double v=spu_xf_get(a._u32[i])*f; if(v<0)v=0; if(v>4294967295.0)v=4294967295.0; r._u32[i]=(uint32_t)v; } return r; }
#if defined(__AVX2__)
/* Float to integer four lanes at a time: the scale is a power of two (exact
 * in double), then the same clamps and truncation. Exponent 255 goes exact. */
static inline __attribute__((always_inline)) u128 spu_cflts(u128 a, int i8) {
    const __m128i va = _mm_loadu_si128((const __m128i*)a._u32);
    if (__builtin_expect(spu_xf_e255_v(va), 0)) return spu_cflts_exact(a, i8);
    __m256d v = _mm256_mul_pd(_mm256_cvtps_pd(_mm_castsi128_ps(va)), _mm256_set1_pd(spu_pow2i(173 - i8)));
    v = _mm256_min_pd(_mm256_max_pd(v, _mm256_set1_pd(-2147483648.0)), _mm256_set1_pd(2147483647.0));
    u128 r; _mm_storeu_si128((__m128i*)r._u32, _mm256_cvttpd_epi32(v)); return r; }
static inline __attribute__((always_inline)) u128 spu_cfltu(u128 a, int i8) {
    const __m128i va = _mm_loadu_si128((const __m128i*)a._u32);
    if (__builtin_expect(spu_xf_e255_v(va), 0)) return spu_cfltu_exact(a, i8);
    __m256d v = _mm256_mul_pd(_mm256_cvtps_pd(_mm_castsi128_ps(va)), _mm256_set1_pd(spu_pow2i(173 - i8)));
    v = _mm256_floor_pd(_mm256_min_pd(_mm256_max_pd(v, _mm256_setzero_pd()), _mm256_set1_pd(4294967295.0)));
    const __m128i t = _mm256_cvttpd_epi32(_mm256_sub_pd(v, _mm256_set1_pd(2147483648.0)));
    u128 r; _mm_storeu_si128((__m128i*)r._u32, _mm_xor_si128(t, _mm_set1_epi32((int)0x80000000u))); return r; }
#else
static inline u128 spu_cflts(u128 a, int i8) { return spu_cflts_exact(a, i8); }
static inline u128 spu_cfltu(u128 a, int i8) { return spu_cfltu_exact(a, i8); }
#endif
static inline u128 spu_csflt_exact(u128 a, int i8){ u128 r; double f=spu_pow2i(i8-155);
    for(int i=0;i<4;i++) r._u32[i]=spu_xf_put((double)a._s32[i]*f); return r; }
static inline u128 spu_cuflt_exact(u128 a, int i8){ u128 r; double f=spu_pow2i(i8-155);
    for(int i=0;i<4;i++) r._u32[i]=spu_xf_put((double)a._u32[i]*f); return r; }
#if defined(__SSE2__) || defined(_M_X64)
/* Integers below 2^24 convert exactly, and the scale is a power of two, so
 * the product is exact too unless it leaves the normal range: then no
 * rounding mode matters and the SPU's truncation is the IEEE result. Any
 * other lane takes the exact path. */
static inline __attribute__((always_inline)) u128 spu_xf_int_scale(__m128i v, int i8, int is_unsigned, u128 a) {
    (void)a;
    const __m128i lim = _mm_set1_epi32(1 << 24);
    __m128i big;
    if (is_unsigned) big = _mm_or_si128(_mm_cmpgt_epi32(v, _mm_set1_epi32((1 << 24) - 1)), _mm_cmplt_epi32(v, _mm_setzero_si128()));
    else big = _mm_or_si128(_mm_cmpgt_epi32(v, _mm_sub_epi32(lim, _mm_set1_epi32(1))),
                            _mm_cmplt_epi32(v, _mm_sub_epi32(_mm_setzero_si128(), lim)));
    u128 r;
    if (_mm_movemask_epi8(big) != 0 || i8 - 155 < -126 || i8 - 155 > 127) { r._u32[0] = 0xFFFFFFFFu; r._u32[1] = 1u; return r; }
    const __m128 sc = _mm_castsi128_ps(_mm_set1_epi32((i8 - 155 + 127) << 23));
    const __m128 vr = _mm_mul_ps(_mm_cvtepi32_ps(v), sc);
    if (!spu_xf_plain_v(_mm_castps_si128(vr))) { r._u32[0] = 0xFFFFFFFFu; r._u32[1] = 1u; return r; }
    _mm_storeu_ps(r._f32, vr); return r; }
#if defined(__AVX2__)
/* Integers of 2^24 and up, which the path above gives to the exact one, a lane at a
 * time: common in the game's skinning jobs (4% of a busy lane). Through doubles, as
 * the exact path does, all four at once: the int and the power of two are exact in a
 * double, the narrowing rounds to nearest, and a lane that rounded away from zero
 * steps back one (the SPU truncates). A result outside the plain normal range still
 * goes the exact way. */
static inline __attribute__((always_inline)) int spu_xf_wide(__m256d d, u128* r) {
    const __m128 f = _mm256_cvtpd_ps(d);
    const __m256d back = _mm256_cvtps_pd(f);
    const __m256d absm = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
    const __m256i over = _mm256_castpd_si256(_mm256_cmp_pd(_mm256_and_pd(back, absm), _mm256_and_pd(d, absm), _CMP_GT_OQ));
    const __m128i low = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(over, _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6)));
    const __m128i bits = _mm_add_epi32(_mm_castps_si128(f), low);   /* -1 where it went over */
    if (!spu_xf_plain_v(_mm_castps_si128(f)) || !spu_xf_plain_v(bits)) return 0;
    _mm_storeu_si128((__m128i*)r->_u32, bits);
    return 1; }
#endif
static inline __attribute__((always_inline)) u128 spu_csflt(u128 a, int i8) {
    const u128 r = spu_xf_int_scale(_mm_loadu_si128((const __m128i*)a._u32), i8, 0, a);
    /* the miss marker is a NaN pattern, which a plain result can never be */
    if (__builtin_expect(!(r._u32[0] == 0xFFFFFFFFu && r._u32[1] == 1u), 1)) return r;
#if defined(__AVX2__)
    if (i8 - 155 >= -126 && i8 - 155 <= 127) {
        u128 w;
        const __m256d d = _mm256_mul_pd(_mm256_cvtepi32_pd(_mm_loadu_si128((const __m128i*)a._u32)), _mm256_set1_pd(spu_pow2i(i8 - 155)));
        if (spu_xf_wide(d, &w)) return w;
    }
#endif
    return spu_csflt_exact(a, i8); }
static inline __attribute__((always_inline)) u128 spu_cuflt(u128 a, int i8) {
    const u128 r = spu_xf_int_scale(_mm_loadu_si128((const __m128i*)a._u32), i8, 1, a);
    if (__builtin_expect(!(r._u32[0] == 0xFFFFFFFFu && r._u32[1] == 1u), 1)) return r;
    return spu_cuflt_exact(a, i8); }
#else
static inline u128 spu_csflt(u128 a, int i8) { return spu_csflt_exact(a, i8); }
static inline u128 spu_cuflt(u128 a, int i8) { return spu_cuflt_exact(a, i8); }
#endif
/* Fused multiply-add: the product is exact in double, the sum rounds once
 * there, then the SPU truncation. */
#if defined(__SSE2__) || defined(_M_X64)
/* Two lanes per double op: the same double arithmetic as the scalar form. */
/* The four lanes in double: one 256-bit pass with AVX, two 128-bit passes
 * otherwise -- the same arithmetic. x, y, z are the operands as doubles.
 *
 * With FMA3, one native single-precision fused op instead. SPU code runs
 * with round-toward-zero (and DAZ/FTZ), and truncating to double and then
 * to single is truncating once, so the double form computes the exactly
 * rounded fused result -- what vfmadd/vfmsub/vfnmadd give directly. Checked
 * bit-exact, the fast/exact path choice included, on 180M cases per op
 * (random, denormal, exponent-255, FLT_MAX, near-cancelling c = -a*b). The
 * double form was a ~20-cycle convert/mul/add/convert chain per op in the
 * collision jobs' dot products, which the main thread waits on. */
#if defined(__FMA__)
#include <immintrin.h>
#define SPU_XF_FMA_BODY(VEXPR, V256, NATIVE) \
        const __m128 vr = NATIVE;
#elif defined(__AVX__)
#include <immintrin.h>
#define SPU_XF_FMA_BODY(VEXPR, V256, NATIVE) \
        const __m256d x = _mm256_cvtps_pd(va), y = _mm256_cvtps_pd(vb), z = _mm256_cvtps_pd(vc); \
        const __m128 vr = _mm256_cvtpd_ps(V256);
#else
#define SPU_XF_FMA_BODY(VEXPR, V256, NATIVE) \
        __m128d x = _mm_cvtps_pd(va), y = _mm_cvtps_pd(vb), z = _mm_cvtps_pd(vc); \
        const __m128 lo = _mm_cvtpd_ps(VEXPR); \
        x = _mm_cvtps_pd(_mm_movehl_ps(va, va)); y = _mm_cvtps_pd(_mm_movehl_ps(vb, vb)); \
        z = _mm_cvtps_pd(_mm_movehl_ps(vc, vc)); \
        const __m128 vr = _mm_movelh_ps(lo, _mm_cvtpd_ps(VEXPR));
#endif
#if defined(__AVX2__)
#define SPU_XF_EXT3(name, V256) \
        { SPU_XF_COUNT(6); const __m256d x = spu_xf_get4(_mm_castps_si128(va)), y = spu_xf_get4(_mm_castps_si128(vb)), \
                         z = spu_xf_get4(_mm_castps_si128(vc)); \
          _mm_storeu_si128((__m128i*)r._u32, spu_xf_put4(V256)); return r; }
#else
#define SPU_XF_EXT3(name, V256) return name##_exact(a, b, c);
#endif
#define SPU_XF_FMA_V(name, EXPR, VEXPR, V256, NATIVE) \
    static inline __attribute__((noinline)) u128 name##_exact(u128 a, u128 b, u128 c) { u128 r; SPU_XF_COUNT(1); \
        for (int i = 0; i < 4; i++) { const double x = spu_xf_get(a._u32[i]), y = spu_xf_get(b._u32[i]), \
            z = spu_xf_get(c._u32[i]); r._u32[i] = spu_xf_put(EXPR); } \
        return r; } \
    static inline __attribute__((always_inline)) u128 name(u128 a, u128 b, u128 c) { u128 r; \
        const __m128 va = _mm_loadu_ps(a._f32), vb = _mm_loadu_ps(b._f32), vc = _mm_loadu_ps(c._f32); \
        SPU_XF_FMA_BODY(VEXPR, V256, NATIVE) \
        _mm_storeu_ps(r._f32, vr); \
        if (__builtin_expect(!spu_xf_bad_v(_mm_castps_si128(vr)), 1)) return r; \
        SPU_XF_EXT3(name, V256) }
SPU_XF_FMA_V(spu_fma, x * y + z, _mm_add_pd(_mm_mul_pd(x, y), z), _mm256_add_pd(_mm256_mul_pd(x, y), z),
             _mm_fmadd_ps(va, vb, vc))
SPU_XF_FMA_V(spu_fms, x * y - z, _mm_sub_pd(_mm_mul_pd(x, y), z), _mm256_sub_pd(_mm256_mul_pd(x, y), z),
             _mm_fmsub_ps(va, vb, vc))
SPU_XF_FMA_V(spu_fnms, z - x * y, _mm_sub_pd(z, _mm_mul_pd(x, y)), _mm256_sub_pd(z, _mm256_mul_pd(x, y)),
             _mm_fnmadd_ps(va, vb, vc))
#elif defined(__aarch64__)
/* The four lanes in double, two at a time; the product of two singles is
 * exact there, the sum rounds once (toward zero, FPCR), then to single. */
#define SPU_XF_FMA_N(name, EXPR, NEXPR) \
    static inline __attribute__((noinline)) u128 name##_exact(u128 a, u128 b, u128 c) { u128 r; SPU_XF_COUNT(1); \
        for (int i = 0; i < 4; i++) { const double x = spu_xf_get(a._u32[i]), y = spu_xf_get(b._u32[i]), \
            z = spu_xf_get(c._u32[i]); r._u32[i] = spu_xf_put(EXPR); } \
        return r; } \
    static inline __attribute__((always_inline)) u128 name(u128 a, u128 b, u128 c) { u128 r; \
        const float32x4_t va = vld1q_f32(a._f32), vb = vld1q_f32(b._f32), vc = vld1q_f32(c._f32); \
        float64x2_t x = vcvt_f64_f32(vget_low_f32(va)), y = vcvt_f64_f32(vget_low_f32(vb)), z = vcvt_f64_f32(vget_low_f32(vc)); \
        const float32x2_t lo = vcvt_f32_f64(NEXPR); \
        x = vcvt_high_f64_f32(va); y = vcvt_high_f64_f32(vb); z = vcvt_high_f64_f32(vc); \
        const float32x4_t vr = vcvt_high_f32_f64(lo, NEXPR); \
        vst1q_f32(r._f32, vr); \
        if (__builtin_expect(!spu_xf_bad_n(vreinterpretq_u32_f32(vr)), 1)) return r; \
        return name##_exact(a, b, c); }
SPU_XF_FMA_N(spu_fma, x * y + z, vaddq_f64(vmulq_f64(x, y), z))
SPU_XF_FMA_N(spu_fms, x * y - z, vsubq_f64(vmulq_f64(x, y), z))
SPU_XF_FMA_N(spu_fnms, z - x * y, vsubq_f64(z, vmulq_f64(x, y)))
#else
SPU_XF_FMA(spu_fma, x * y + z)
SPU_XF_FMA(spu_fms, x * y - z)
SPU_XF_FMA(spu_fnms, z - x * y)
#endif

/* ---- SPU double-precision (2 doubles/reg; dword i = words 2i(high),2i+1(low)).
 * Reassemble via _u32 -- our u128 is host-LE, so a naive _f64[i] would SWAP the
 * two words. Semantics from RPCS3 SPUInterpreter (DFASM / DFMA / FESD / FRDS). ---- */
static inline double spu__dget(u128 r, int i) {
    uint64_t u = ((uint64_t)r._u32[i*2] << 32) | r._u32[i*2+1];
    double d; memcpy(&d, &u, sizeof d); return d;
}
static inline void spu__dset(u128* r, int i, double d) {
    uint64_t u; memcpy(&u, &d, sizeof u);
    r->_u32[i*2] = (uint32_t)(u >> 32); r->_u32[i*2+1] = (uint32_t)u;
}
static inline u128 spu_dfa(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)+spu__dget(b,i)); return r; }
static inline u128 spu_dfs(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)-spu__dget(b,i)); return r; }
static inline u128 spu_dfm(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)*spu__dget(b,i)); return r; }
/* double FMA family: c = rt (accumulator, 3-register). */
static inline u128 spu_dfma(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)*spu__dget(b,i)+spu__dget(t,i)); return r; }
static inline u128 spu_dfms(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)*spu__dget(b,i)-spu__dget(t,i)); return r; }
static inline u128 spu_dfnms(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(t,i)-spu__dget(a,i)*spu__dget(b,i)); return r; }
static inline u128 spu_dfnma(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, -(spu__dget(a,i)*spu__dget(b,i)+spu__dget(t,i))); return r; }
/* double compares -> per-lane 64-bit mask (RPCS3 stubs these; sane impl here). */
static inline u128 spu_dfceq(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)==spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmeq(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x==y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
/* Singles occupy preferred words 0 and 2 when converting doubleword lanes. */
static inline u128 spu_fesd(u128 a) { u128 r; memset(&r,0,sizeof r); for(int i=0;i<2;i++) spu__dset(&r,i,spu_xf_get(a._u32[i*2])); return r; }
static inline u128 spu_frds(u128 a) { u128 r; memset(&r,0,sizeof r); for(int i=0;i<2;i++) r._f32[i*2]=(float)spu__dget(a,i); return r; }
/* mpyhhu: high-16 x high-16 of each word, unsigned, full 32-bit product. */
static inline u128 spu_mpyhhu(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
/* cgx: extended carry-generate, carry-in = low bit of old rt (RPCS3 CGX). */
static inline u128 spu_cgx(u128 a, u128 b, u128 t){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)(t._u32[i]&1u)+a._u32[i]+b._u32[i])>>32); return r; }

/* ---- remaining SPU ISA ops (RPCS3 SPUInterpreter: EQV/ABSDB/AVGB/MPYHHA/
 * MPYHHAU/DFCGT/DFCMGT/XORBI/DFTSV). Completes the lifter's opcode coverage. ---- */
static inline u128 spu_eqv(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=~(a._u32[i]^b._u32[i]); return r; }
static inline u128 spu_xorbi(u128 a, int32_t imm){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(a._u8[i]^(uint8_t)imm); return r; }
static inline u128 spu_absdb(u128 a, u128 b)  { u128 r; for(int i=0;i<16;i++){ uint8_t x=a._u8[i],y=b._u8[i]; r._u8[i]=(uint8_t)(x>y?x-y:y-x); } return r; }
static inline u128 spu_avgb(u128 a, u128 b)   { u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(((uint32_t)a._u8[i]+(uint32_t)b._u8[i]+1u)>>1); return r; }
/* mpyhha/mpyhhau: high-16 x high-16, ACCUMULATE into rt (3-register). */
static inline u128 spu_mpyhha(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=t._s32[i]+(int32_t)a._s16[2*i+1]*(int32_t)b._s16[2*i+1]; return r; }
static inline u128 spu_mpyhhau(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=t._u32[i]+(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
/* double compare greater (RPCS3 stubs these; sane impl) -> per-lane mask. */
static inline u128 spu_dfcgt(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)>spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmgt(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x>y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
/* dftsv: double test special value -- RPCS3 stubs (fatal); 0 = no special. */
static inline u128 spu_dftsv(u128 a, int32_t imm){ (void)a; (void)imm; u128 r; memset(&r,0,sizeof r); return r; }

/* ---- Phase 2: register-variable shifts/rotates ---- */
static inline u128 spu_shl(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=b._u32[i]&0x3F; r._u32[i]=(sh>31)?0:(a._u32[i]<<sh); } return r; }
static inline u128 spu_shlh(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=b._u16[i]&0x1F; r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]<<sh); } return r; }
static inline u128 spu_rot(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=b._u32[i]&31; r._u32[i]= sh ? ((a._u32[i]<<sh)|(a._u32[i]>>(32-sh))) : a._u32[i]; } return r; }
static inline u128 spu_roth(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=b._u16[i]&15; r._u16[i]= sh ? (uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))) : a._u16[i]; } return r; }
static inline u128 spu_shlqbi(u128 a, u128 b){ int sh=b._u32[0]&7; if(!sh) return a; SPU_QSHL(a, sh)
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqbi(u128 a, u128 b){ int sh=b._u32[0]&7; if(!sh) return a; SPU_QROTL(a, sh)
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh)|(hi>>(64-sh));
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_shlqby(u128 a, u128 b){ int sh=b._u32[0]&0x1F;
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__SHLQBY(a, sh);
#endif
    u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqby(u128 a, u128 b){ int sh=b._u32[0]&0x0F;
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__ROTQBY(a, sh);
#endif
    u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
static inline u128 spu_shlqbybi(u128 a, u128 b){ int sh=(b._u32[0]>>3)&0x1F;
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__SHLQBY(a, sh);
#endif
    u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqbybi(u128 a, u128 b){ int sh=(b._u32[0]>>3)&0x0F;
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__ROTQBY(a, sh);
#endif
    u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }

/* ---- Phase 2: rotmahi ---- */
static inline u128 spu_rotmahi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); return r; }

/* ---- Phase 2: byte/half immediate compares ---- */
static inline u128 spu_ceqbi(u128 a, int32_t imm)  { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==v)?0xFFu:0; return r; }
static inline u128 spu_ceqhi(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]==v)?0xFFFFu:0; return r; }
static inline u128 spu_clgtbi(u128 a, int32_t imm) { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>v)?0xFFu:0; return r; }
static inline u128 spu_clgthi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgthi(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgtbi(u128 a, int32_t imm)  { u128 r; int8_t v=(int8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>v)?0xFFu:0; return r; }

/* ---- Phase 2: misc one-offs ---- */
static inline u128 spu_fscrrd(u128 a) { (void)a; return spu_zero(); }
static inline u128 spu_gb(u128 a) {
    uint32_t v = ((a._u32[0]&1)<<3)|((a._u32[1]&1)<<2)|((a._u32[2]&1)<<1)|(a._u32[3]&1);
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
static inline u128 spu_gbh(u128 a) {
    /* gather LSB of each SPU halfword H into bit (7-H). SPU halfword H maps to
     * our _u16[H^1] (the within-word halfword swap of the value layout). */
    uint32_t v=0; for(int i=0;i<8;i++) v |= ((uint32_t)(a._u16[i^1]&1) << (7-i));
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
/* gather LSB of each SPU byte i into bit (15-i); exact inverse of spu_fsmb.
 * SPU byte i lives at our _u8[SPU_W(i)] (byte-reversed within each word). */
static inline u128 spu_gbb(u128 a) {
    uint32_t v=0; for(int i=0;i<16;i++) v |= ((uint32_t)(a._u8[SPU_W(i)]&1) << (15-i));
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
static inline u128 spu_cg(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)a._u32[i]+(uint64_t)b._u32[i])>>32); return r; }
/* sumb: sum the 4 bytes of each word. Per CBEA, RT halfword 2i (the HIGH half of
 * word i) = sum of RB's 4 bytes of word i; halfword 2i+1 (LOW half) = sum of RA's
 * 4 bytes of word i. Computed on word VALUES so byte order is irrelevant. */
static inline u128 spu_sumb(u128 a, u128 b) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint32_t wa=a._u32[i], wb=b._u32[i];
        uint32_t sa=((wa>>24)&0xFF)+((wa>>16)&0xFF)+((wa>>8)&0xFF)+(wa&0xFF);
        uint32_t sb=((wb>>24)&0xFF)+((wb>>16)&0xFF)+((wb>>8)&0xFF)+(wb&0xFF);
        r._u32[i]=(sb<<16)|sa;
    }
    return r;
}
/* Borrow generate: carry-out of (b + ~a + 1) == (b >= a unsigned ? 1 : 0).
 * The subtract-side sibling of cg; pairs with sf/sfx for extended subtraction. */
static inline u128 spu_bg(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)b._u32[i]+(uint64_t)(~a._u32[i])+1u)>>32); return r; }
static inline u128 spu_addx(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+b._u32[i]+(t._u32[i]&1); return r; }
/* LE host: high half of word i = _s16[2i+1], low half = _s16[2i]. */
static inline u128 spu_mpyh(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._s32[i]=((int32_t)a._s16[2*i+1] * (int32_t)b._s16[2*i]) << 16; return r; }
static inline u128 spu_mpyhh(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[2*i+1] * (int32_t)b._s16[2*i+1]; return r; }
static inline u128 spu_mpys(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++){ int32_t p=(int32_t)a._s16[2*i]*(int32_t)b._s16[2*i]; r._s32[i]=(int16_t)(p>>16); } return r; }
static inline u128 spu_mpyui(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[2*i]*(uint32_t)(uint16_t)imm; return r; }
SPU_XF_CMP2(spu_fcmeq, fabs(x) == fabs(y))
#if defined(__SSE2__) || defined(_M_X64)
SPU_XF_CMP2(spu_fcmgt_exact, fabs(x) > fabs(y))
static inline __attribute__((always_inline)) u128 spu_fcmgt(u128 a, u128 b) {
    const __m128i ia = _mm_loadu_si128((const __m128i*)a._u32), ib = _mm_loadu_si128((const __m128i*)b._u32);
    if (__builtin_expect(spu_xf_e255_v(ia) | spu_xf_e255_v(ib), 0)) return spu_fcmgt_exact(a, b);
    const __m128i m = _mm_set1_epi32(0x7FFFFFFF);
    u128 r; _mm_storeu_ps(r._f32, _mm_cmpgt_ps(_mm_castsi128_ps(_mm_and_si128(ia, m)), _mm_castsi128_ps(_mm_and_si128(ib, m))));
    return r; }
#elif defined(__aarch64__)
SPU_XF_CMP2(spu_fcmgt_exact, fabs(x) > fabs(y))
static inline __attribute__((always_inline)) u128 spu_fcmgt(u128 a, u128 b) {
    const uint32x4_t ia = vld1q_u32(a._u32), ib = vld1q_u32(b._u32);
    if (__builtin_expect(spu_xf_e255_n(ia) | spu_xf_e255_n(ib), 0)) return spu_fcmgt_exact(a, b);
    u128 r; vst1q_u32(r._u32, vcagtq_f32(vreinterpretq_f32_u32(ia), vreinterpretq_f32_u32(ib))); return r; }
#else
SPU_XF_CMP2(spu_fcmgt, fabs(x) > fabs(y))
#endif
static const uint32_t spu_frest_fraction_lut[32] = {
    0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
    0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
    0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
    0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07
};

static const uint32_t spu_frsqest_fraction_lut[64] = {
    0x350160, 0x34E954, 0x2F993D, 0x2F993D, 0x2AA523, 0x2AA523, 0x26190D, 0x26190D,
    0x21E4F9, 0x21E4F9, 0x1E00E9, 0x1E00E9, 0x1A5CD9, 0x1A5CD9, 0x16F8CB, 0x16F8CB,
    0x13CCC0, 0x13CCC0, 0x10CCB3, 0x10CCB3, 0x0E00AA, 0x0E00AA, 0x0B58A1, 0x0B58A1,
    0x08D498, 0x08D498, 0x067491, 0x067491, 0x043089, 0x043089, 0x020C83, 0x020C83,
    0x7FFDF4, 0x7FD1DE, 0x7859C8, 0x783DBA, 0x71559C, 0x71559C, 0x6AE57C, 0x6AE57C,
    0x64F561, 0x64F561, 0x5F7149, 0x5F7149, 0x5A4D33, 0x5A4D33, 0x55811F, 0x55811F,
    0x51050F, 0x51050F, 0x4CC8FE, 0x4CC8FE, 0x48D0F0, 0x48D0F0, 0x4510E4, 0x4510E4,
    0x4180D7, 0x4180D7, 0x3E24CC, 0x3E24CC, 0x3AF4C3, 0x3AF4C3, 0x37E8BA, 0x37E8BA
};

static inline u128 spu_frsqest_scalar(u128 a) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a._u32[i];
        uint32_t exp  = (bits >> 23) & 0xFFu;
        uint32_t frac_idx = (bits >> 18) & 0x3Fu;
        uint32_t rexp = (exp == 0u) ? 0xFFu : (190u - (exp + 1u) / 2u);
        uint32_t fraction = spu_frsqest_fraction_lut[frac_idx];
        uint32_t out = (rexp << 23) | (fraction & 0x7FFFFFu);
        memcpy(&r._f32[i], &out, sizeof out);
    }
    return r;
}
/* frest: reciprocal estimate (refined by the following spu_fi Newton step).
 * Full-precision 1/x is exact after fi and >= HW-estimate accuracy. */
static inline u128 spu_frest_scalar(u128 a) {
    u128 r;
    for (int i = 0; i < 4; i++) {
        uint32_t bits = a._u32[i];
        uint32_t sign = bits & 0x80000000u;
        uint32_t exp  = (bits >> 23) & 0xFFu;
        uint32_t frac_idx = (bits >> 18) & 0x1Fu;
        uint32_t out;
        if (exp == 0u) {
            /* zero or denormal operand: ISA p.215 "1/0 is defined to give the
             * maximum SPU single-precision extended-range fp number", sign of
             * the (zero) input preserved. Same bucket for true denormals
             * (their biased exponent field is also 0). */
            out = sign | 0x7FFFFFFFu;
        } else {
            /* result biased exponent = 253 - input biased exponent (253 =
             * 2*127-1, i.e. reciprocal negates the unbiased exponent),
             * clamped to 0 on underflow (ISA p.215 "Big: if |x|>=2^126,
             * 1/x underflows to zero"). Verified bit-exact against RPCS3's
             * raw 256-entry spu_frest_exponent_lut for all 256 inputs. */
            uint32_t rexp = (exp >= 253u) ? 0u : (253u - exp);
            uint32_t fraction = spu_frest_fraction_lut[frac_idx];
            out = sign | (rexp << 23) | (fraction & 0x7FFFFFu);
        }
        memcpy(&r._f32[i], &out, sizeof out);
    }
    return r;
}

#if defined(__AVX2__)
/* The same table lookups four lanes at a time (AVX2 gathers). */
static inline __attribute__((always_inline)) u128 spu_frsqest(u128 a) {
    const __m128i v = _mm_loadu_si128((const __m128i*)a._u32);
    const __m128i ex = _mm_and_si128(_mm_srli_epi32(v, 23), _mm_set1_epi32(0xFF));
    const __m128i idx = _mm_and_si128(_mm_srli_epi32(v, 18), _mm_set1_epi32(0x3F));
    __m128i rexp = _mm_sub_epi32(_mm_set1_epi32(190), _mm_srli_epi32(_mm_add_epi32(ex, _mm_set1_epi32(1)), 1));
    rexp = _mm_blendv_epi8(rexp, _mm_set1_epi32(0xFF), _mm_cmpeq_epi32(ex, _mm_setzero_si128()));
    const __m128i fr = _mm_i32gather_epi32((const int*)spu_frsqest_fraction_lut, idx, 4);
    u128 r; _mm_storeu_si128((__m128i*)r._u32,
        _mm_or_si128(_mm_slli_epi32(rexp, 23), _mm_and_si128(fr, _mm_set1_epi32(0x7FFFFF)))); return r;
}
static inline __attribute__((always_inline)) u128 spu_frest(u128 a) {
    const __m128i v = _mm_loadu_si128((const __m128i*)a._u32);
    const __m128i sign = _mm_and_si128(v, _mm_set1_epi32((int)0x80000000u));
    const __m128i ex = _mm_and_si128(_mm_srli_epi32(v, 23), _mm_set1_epi32(0xFF));
    const __m128i idx = _mm_and_si128(_mm_srli_epi32(v, 18), _mm_set1_epi32(0x1F));
    __m128i rexp = _mm_sub_epi32(_mm_set1_epi32(253), ex);
    rexp = _mm_andnot_si128(_mm_cmpgt_epi32(ex, _mm_set1_epi32(252)), rexp);          /* exp >= 253: 0 */
    const __m128i fr = _mm_i32gather_epi32((const int*)spu_frest_fraction_lut, idx, 4);
    __m128i out = _mm_or_si128(_mm_or_si128(sign, _mm_slli_epi32(rexp, 23)), _mm_and_si128(fr, _mm_set1_epi32(0x7FFFFF)));
    out = _mm_blendv_epi8(out, _mm_or_si128(sign, _mm_set1_epi32(0x7FFFFFFF)), _mm_cmpeq_epi32(ex, _mm_setzero_si128()));
    u128 r; _mm_storeu_si128((__m128i*)r._u32, out); return r;
}
#else
static inline u128 spu_frsqest(u128 a) { return spu_frsqest_scalar(a); }
static inline u128 spu_frest(u128 a) { return spu_frest_scalar(a); }
#endif

/* ---- Phase 3: sign extension ----
 * LE host: low sub-lane = _u8[2i] / _s16[2i] / _s32[2i] (the byte/half/word
 * at the *lower* storage address). Same caveat as the mpy family. */
static inline u128 spu_xsbh(u128 a) { u128 r; for(int i=0;i<8;i++) r._s16[i] = (int8_t)a._u8[2*i]; return r; }
static inline u128 spu_xshw(u128 a) { u128 r; for(int i=0;i<4;i++) r._s32[i] = (int16_t)a._s16[2*i]; return r; }
/* xswd: rt.doubleword[d] = sign_extend_64(ra.word[2d+1]) -- the ODD word of
 * each doubleword, because that is the low half in big-endian order.
 *
 * This read the EVEN words (2d) and wrote the result through _s64, which is a
 * host little-endian lane: writing it puts the value in _u32[2d] (SPU word 2d,
 * the HIGH half) and the sign fill in _u32[2d+1]. Both halves were therefore
 * wrong, and the common case -- a small positive value -- came out as ZERO.
 *
 * You Don't Know Jack's FMOD mixer hit exactly that: a compiler-generated
 * 64-bit multiply helper received `1 * 0` instead of `1 * 16`, so the mixer's
 * block count was zero and it stopped on its own assert. Unlike its siblings
 * xsbh/xshw, whose source and destination swaps cancel, nothing cancels here.
 * Write the two words explicitly. */
static inline u128 spu_xswd(u128 a) {
    u128 r;
    for (int d = 0; d < 2; d++) {
        int32_t v = (int32_t)a._u32[2*d + 1];
        r._u32[2*d]     = (uint32_t)(v >> 31);   /* sign fill (high word) */
        r._u32[2*d + 1] = (uint32_t)v;           /* value      (low word) */
    }
    return r;
}

/* ---- Phase 3: OR across ---- */
static inline u128 spu_orx(u128 a) {
    u128 r = spu_zero();
    r._u32[0] = a._u32[0] | a._u32[1] | a._u32[2] | a._u32[3];
    return r;
}

/* ---- Phase 3: form-select mask from bits ---- */
static inline u128 spu_fsm(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xF;
    for(int i=0;i<4;i++) r._u32[i] = ((v>>(3-i))&1) ? 0xFFFFFFFFu : 0;
    return r;
}
/* fsmh: per-halfword mask from 8 bits. SPU halfword H <- bit (7-H); store at
 * our _u16[H^1] (within-word halfword swap of the value layout). Per-halfword
 * both bytes are identical so byte order within a halfword is irrelevant, but
 * the halfword ORDER within each word must be swapped. */
static inline u128 spu_fsmh(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xFF;
    for(int i=0;i<8;i++) r._u16[i^1] = ((v>>(7-i))&1) ? 0xFFFFu : 0;
    return r;
}
/* fsmb/fsmbi: per-byte mask from 16 bits. SPU byte position P <- bit (15-P);
 * store at our _u8[SPU_W(P)] (byte-reversed within each word). A bit set at SPU
 * byte P must land at our _u8[SPU_W(P)] so that downstream word arithmetic (e.g.
 * andbi(fsmbi(0x101),-128) building a +0x80 EA offset in the low byte of words
 * 1,3) places the value in the correct byte lane. Raw _u8[P] would put 0x80 in
 * the high byte (bit 31) instead -> wrong DMA EA. */
static inline u128 spu_fsmb(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xFFFF;
    for(int i=0;i<16;i++) r._u8[SPU_W(i)] = ((v>>(15-i))&1) ? 0xFFu : 0;
    return r;
}
static inline u128 spu_fsmbi(int32_t imm) {
    u128 r; uint32_t v = imm & 0xFFFF;
    for(int i=0;i<16;i++) r._u8[SPU_W(i)] = ((v>>(15-i))&1) ? 0xFFu : 0;
    return r;
}

/* ---- Phase 3: constant generators (insertion shuffle patterns) ---- */
/* Generate-controls (cbd/chd/cwd/cdd) — insertion selectors for the (now
 * SPU-byte-correct) shufb. Each builds a control in TRUE SPU byte order: the
 * base selects b's SPU byte t at result SPU byte t (selector 0x10+t), and the
 * insert positions select a's preferred scalar bytes -- SPU byte 3 for a byte
 * (selector 0x03), SPU bytes 2,3 for a halfword (0x02,0x03), SPU bytes 0..3 for
 * a word, 0..7 for a doubleword. Every store is mapped through SPU_W to land at
 * the right host index. Verified to compose correctly with the fixed shufb for
 * both cbd-generated and immediate/LS-constant controls. */
static inline u128 spu_cbd_pos(int pos) {
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__CXD(spu__cbd_ctl, pos & 0xF);
#endif
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    r._u8[SPU_W(pos & 0xF)] = 0x03;                 /* a's preferred byte = SPU byte 3 */
    return r;
}
static inline u128 spu_chd_pos(int pos) {
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__CXD(spu__chd_ctl, (pos & 0xF) >> 1);
#endif
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~1;                       /* SPU halfword byte positions p, p+1 */
    r._u8[SPU_W(p)]   = 0x02;                        /* a SPU byte 2 (hi byte of preferred hw) */
    r._u8[SPU_W(p+1)] = 0x03;                        /* a SPU byte 3 (lo byte) */
    return r;
}
static inline u128 spu_cwd_pos(int pos) {
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__CXD(spu__cwd_ctl, (pos & 0xF) >> 2);
#endif
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~3;
    for(int k=0;k<4;k++) r._u8[SPU_W(p+k)] = (uint8_t)k;   /* a SPU bytes 0..3 (preferred word) */
    return r;
}
static inline u128 spu_cdd_pos(int pos) {
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__CXD(spu__cdd_ctl, (pos & 0xF) >> 3);
#endif
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~7;
    for(int k=0;k<8;k++) r._u8[SPU_W(p+k)] = (uint8_t)k;   /* a SPU bytes 0..7 (preferred dword) */
    return r;
}
static inline u128 spu_cbd(u128 a, int i7){ return spu_cbd_pos((int)a._u32[0]+i7); }
static inline u128 spu_chd(u128 a, int i7){ return spu_chd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cwd(u128 a, int i7){ return spu_cwd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cdd(u128 a, int i7){ return spu_cdd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cbx(u128 a, u128 b){ return spu_cbd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_chx(u128 a, u128 b){ return spu_chd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_cwx(u128 a, u128 b){ return spu_cwd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_cdx(u128 a, u128 b){ return spu_cdd_pos((int)(a._u32[0]+b._u32[0])); }

/* ---- Phase 3: rotate-and-mask family ---- */
static inline u128 spu_rotm(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=(0-b._u32[i])&0x3F; r._u32[i]=(sh>31)?0:(a._u32[i]>>sh); } return r; }
static inline u128 spu_rotma(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++){ uint32_t sh=(0-b._u32[i])&0x3F; r._s32[i]=(sh>31)?(a._s32[i]>>31):(a._s32[i]>>sh); } return r; }
static inline u128 spu_rothm(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=(0-b._u16[i])&0x1F; r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); } return r; }
static inline u128 spu_rothma(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++){ uint32_t sh=(0-b._u16[i])&0x1F; r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); } return r; }
static inline u128 spu_rothmi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
static inline u128 spu_rotqmbi(u128 a, u128 b)   { int sh=(0-(int)b._u32[0])&7; if(!sh) return a; SPU_QSHR(a, sh)
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nlo=(lo>>sh)|(hi<<(64-sh)), nhi=(hi>>sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqmby(u128 a, u128 b)   { int sh=(0-(int)b._u32[0])&0x1F;
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__ROTQMBY(a, sh);
#endif
    u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqmbybi(u128 a, u128 b) { int sh=(0-((int)b._u32[0]>>3))&0x1F;
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__ROTQMBY(a, sh);
#endif
    u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }
static inline u128 spu_rotqmbii(u128 a, int i7)  { int sh=(0-i7)&7; if(!sh) return a; SPU_QSHR(a, sh)
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nlo=(lo>>sh)|(hi<<(64-sh)), nhi=(hi>>sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqmbyi(u128 a, int i7)  { int sh=(0-i7)&0x1F;
#if defined(SPU_HAS_BYTE_TABLE)
    return SPU__ROTQMBY(a, sh);
#endif
    u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r; }

/* ---- Phase 3: halfword/byte immediate logic ---- */
static inline u128 spu_andhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]&v; return r; }
static inline u128 spu_andbi(u128 a, int32_t imm) { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]&v;   return r; }
static inline u128 spu_orhi(u128 a, int32_t imm)  { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]|v; return r; }
static inline u128 spu_orbi(u128 a, int32_t imm)  { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]|v;   return r; }
static inline u128 spu_xorhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]^v; return r; }

/* ---- Phase 3: borrow-generate extended ---- */
static inline u128 spu_bgx(u128 a, u128 b, u128 t) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint64_t s = (uint64_t)b._u32[i] + (uint64_t)(~a._u32[i]) + (uint64_t)(t._u32[i]&1);
        r._u32[i] = (uint32_t)(s >> 32);
    }
    return r;
}

/* ---- Phase 3: mfspr stub ---- */
static inline u128 spu_mfspr(u128 a) { (void)a; return spu_zero(); }

/* ---- immediate loaders ---- */
static inline u128 spu_il(int32_t imm)   { return spu_splat_u32((uint32_t)imm); }
static inline u128 spu_ila(uint32_t i18) { return spu_splat_u32(i18 & 0x3FFFF); }
static inline u128 spu_ilh(uint16_t imm) { return spu_splat_u16(imm); }
static inline u128 spu_ilhu(uint16_t imm){ return spu_splat_u32((uint32_t)imm << 16); }
static inline u128 spu_iohl(u128 a, uint16_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]|(uint32_t)imm; return r; }

#ifdef __cplusplus
}
#endif

#endif /* SPU_HELPERS_H */
