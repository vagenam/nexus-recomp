/* fast_libc_win64.c -- memcpy, memmove, memset, memcmp and sqrt for the Windows build.
 *
 * zig's toolchain supplies these from its own compiler runtime, built for a baseline
 * x86-64: memcmp is a byte-at-a-time loop, memset a scalar one, sqrt a software
 * square root. The renderer compares and clears constant blocks and vertex data on
 * every draw through them. The linker takes these definitions over zig's (weak) ones.
 *
 * AVX2 is safe here: racacit_entry refuses CPUs without it before anything runs, and
 * the lifted code and runtime need it anyway. Built with -fno-builtin, so the compiler
 * cannot turn a loop below back into a call to itself. */
#if defined(_WIN32) && defined(__x86_64__) && !defined(_MSC_VER)
#include <immintrin.h>
#include <stddef.h>
#include <stdint.h>

#define FL_TARGET __attribute__((target("avx2,bmi")))
/* Not _mm256_zeroupper(): that intrinsic counts as clobbering every vector register, and
 * on Windows xmm6-15 are callee-saved, so each call saved ten of them on entry, even a
 * 16-byte memset. As plain assembly it costs what it should. */
#define FL_VZU() __asm__ volatile("vzeroupper")
typedef uint16_t __attribute__((aligned(1), may_alias)) fl_u16;
typedef uint32_t __attribute__((aligned(1), may_alias)) fl_u32;
typedef uint64_t __attribute__((aligned(1), may_alias)) fl_u64;

/* Overlap-safe: every path up to 128 bytes loads everything before it stores. */
FL_TARGET static inline void* fl_move(void* dv, const void* sv, size_t n)
{
    char* d = (char*)dv; const char* s = (const char*)sv;
    if (n <= 16) {
        if (n >= 8) { const uint64_t a = *(const fl_u64*)s, b = *(const fl_u64*)(s + n - 8); *(fl_u64*)d = a; *(fl_u64*)(d + n - 8) = b; return dv; }
        if (n >= 4) { const uint32_t a = *(const fl_u32*)s, b = *(const fl_u32*)(s + n - 4); *(fl_u32*)d = a; *(fl_u32*)(d + n - 4) = b; return dv; }
        if (n >= 2) { const uint16_t a = *(const fl_u16*)s, b = *(const fl_u16*)(s + n - 2); *(fl_u16*)d = a; *(fl_u16*)(d + n - 2) = b; return dv; }
        if (n) *d = *s;
        return dv;
    }
    if (n <= 32) {
        const __m128i a = _mm_loadu_si128((const __m128i*)s), b = _mm_loadu_si128((const __m128i*)(s + n - 16));
        _mm_storeu_si128((__m128i*)d, a); _mm_storeu_si128((__m128i*)(d + n - 16), b);
        return dv;
    }
    if (n <= 64) {
        const __m256i a = _mm256_loadu_si256((const __m256i*)s), b = _mm256_loadu_si256((const __m256i*)(s + n - 32));
        _mm256_storeu_si256((__m256i*)d, a); _mm256_storeu_si256((__m256i*)(d + n - 32), b);
        FL_VZU();
        return dv;
    }
    if (n <= 128) {
        const __m256i a = _mm256_loadu_si256((const __m256i*)s), b = _mm256_loadu_si256((const __m256i*)(s + 32));
        const __m256i c = _mm256_loadu_si256((const __m256i*)(s + n - 64)), e = _mm256_loadu_si256((const __m256i*)(s + n - 32));
        _mm256_storeu_si256((__m256i*)d, a); _mm256_storeu_si256((__m256i*)(d + 32), b);
        _mm256_storeu_si256((__m256i*)(d + n - 64), c); _mm256_storeu_si256((__m256i*)(d + n - 32), e);
        FL_VZU();
        return dv;
    }
    if ((uintptr_t)d - (uintptr_t)s >= n) {
        /* Forward: the destination is before the source, or clear of it. */
        const __m256i t1 = _mm256_loadu_si256((const __m256i*)(s + n - 64)), t0 = _mm256_loadu_si256((const __m256i*)(s + n - 32));
        size_t i = 0;
        const size_t end = n - 64;
        for (; i + 64 <= end; i += 64) {
            const __m256i a = _mm256_loadu_si256((const __m256i*)(s + i)), b = _mm256_loadu_si256((const __m256i*)(s + i + 32));
            _mm256_storeu_si256((__m256i*)(d + i), a); _mm256_storeu_si256((__m256i*)(d + i + 32), b);
        }
        if (i < end) {
            const __m256i a = _mm256_loadu_si256((const __m256i*)(s + i)), b = _mm256_loadu_si256((const __m256i*)(s + i + 32));
            _mm256_storeu_si256((__m256i*)(d + i), a); _mm256_storeu_si256((__m256i*)(d + i + 32), b);
        }
        _mm256_storeu_si256((__m256i*)(d + n - 64), t1); _mm256_storeu_si256((__m256i*)(d + n - 32), t0);
    } else {
        /* Backward: the destination overlaps the source from above. */
        const __m256i h0 = _mm256_loadu_si256((const __m256i*)s), h1 = _mm256_loadu_si256((const __m256i*)(s + 32));
        size_t i = n;
        for (; i >= 64 + 64; i -= 64) {
            const __m256i a = _mm256_loadu_si256((const __m256i*)(s + i - 32)), b = _mm256_loadu_si256((const __m256i*)(s + i - 64));
            _mm256_storeu_si256((__m256i*)(d + i - 32), a); _mm256_storeu_si256((__m256i*)(d + i - 64), b);
        }
        if (i > 64) {
            const __m256i a = _mm256_loadu_si256((const __m256i*)(s + i - 32)), b = _mm256_loadu_si256((const __m256i*)(s + i - 64));
            _mm256_storeu_si256((__m256i*)(d + i - 32), a); _mm256_storeu_si256((__m256i*)(d + i - 64), b);
        }
        _mm256_storeu_si256((__m256i*)d, h0); _mm256_storeu_si256((__m256i*)(d + 32), h1);
    }
    FL_VZU();
    return dv;
}

FL_TARGET void* memmove(void* d, const void* s, size_t n) { return fl_move(d, s, n); }
FL_TARGET void* memcpy(void* d, const void* s, size_t n)  { return fl_move(d, s, n); }

FL_TARGET void* memset(void* dv, int c, size_t n)
{
    char* d = (char*)dv;
    if (n <= 16) {
        const uint64_t v = 0x0101010101010101ULL * (uint8_t)c;
        if (n >= 8) { *(fl_u64*)d = v; *(fl_u64*)(d + n - 8) = v; return dv; }
        if (n >= 4) { *(fl_u32*)d = (uint32_t)v; *(fl_u32*)(d + n - 4) = (uint32_t)v; return dv; }
        if (n >= 2) { *(fl_u16*)d = (uint16_t)v; *(fl_u16*)(d + n - 2) = (uint16_t)v; return dv; }
        if (n) *d = (char)c;
        return dv;
    }
    if (n <= 32) {
        const __m128i v = _mm_set1_epi8((char)c);
        _mm_storeu_si128((__m128i*)d, v); _mm_storeu_si128((__m128i*)(d + n - 16), v);
        return dv;
    }
    const __m256i v = _mm256_set1_epi8((char)c);
    for (size_t i = 0; i + 32 < n; i += 32) _mm256_storeu_si256((__m256i*)(d + i), v);
    _mm256_storeu_si256((__m256i*)(d + n - 32), v);
    FL_VZU();
    return dv;
}

FL_TARGET int memcmp(const void* av, const void* bv, size_t n)
{
    const unsigned char* a = (const unsigned char*)av; const unsigned char* b = (const unsigned char*)bv;
    if (n >= 32) {
        do {
            const unsigned m = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)a), _mm256_loadu_si256((const __m256i*)b)));
            if (m != 0xFFFFFFFFu) {
                const unsigned k = (unsigned)_tzcnt_u32(~m);
                const int r = (int)a[k] - (int)b[k];
                FL_VZU();
                return r;
            }
            a += 32; b += 32; n -= 32;
        } while (n >= 32);
        FL_VZU();
    }
    if (n >= 16) {
        const unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)a), _mm_loadu_si128((const __m128i*)b)));
        if (m != 0xFFFFu) { const unsigned k = (unsigned)_tzcnt_u32(~m); return (int)a[k] - (int)b[k]; }
        a += 16; b += 16; n -= 16;
    }
    if (n >= 8) {
        const uint64_t x = *(const fl_u64*)a ^ *(const fl_u64*)b;
        if (x) { const unsigned k = (unsigned)(_tzcnt_u64(x) >> 3); return (int)a[k] - (int)b[k]; }
        a += 8; b += 8; n -= 8;
    }
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}

FL_TARGET double sqrt(double x) { const __m128d v = _mm_set_sd(x); return _mm_cvtsd_f64(_mm_sqrt_sd(v, v)); }
FL_TARGET float sqrtf(float x)  { const __m128 v = _mm_set_ss(x); return _mm_cvtss_f32(_mm_sqrt_ss(v)); }
#endif
