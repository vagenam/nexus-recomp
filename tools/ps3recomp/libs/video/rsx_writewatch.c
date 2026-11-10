/* See rsx_writewatch.h. */
#include "rsx_writewatch.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern uint8_t* vm_base;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define WW_PAGE_SHIFT 12u
#define WW_SPACE      0x100000000ull                    /* the 4 GB guest space */
#define WW_PAGES      (unsigned)(WW_SPACE >> WW_PAGE_SHIFT)
#define WW_BUF        65536u                             /* 256 MB of pages */

/* The guest ranges that hold what the RSX reads: main memory and RSX local
 * memory. Polled in 256 MB pieces so one call can never overflow WW_BUF
 * (with WRITE_WATCH_FLAG_RESET an overflowing call would lose pages). */
static const struct { uint32_t ea; uint32_t size; } s_ranges[] = {
    { 0x20000000u, 0x10000000u },        /* the title's heap: vertex/index/texture data */
    { 0x40000000u, 0x10000000u },
    { 0xC0000000u, 0x10000000u },        /* RSX local memory */
};
/* The 256 MB pieces above, as a mask of ea >> 28 (where A Crack in Time's
 * draws read from, RSX_WW_LOG=1). Anything outside them is not polled -- and
 * so always reported as written, i.e. hashed as before. Each polled piece
 * costs; all 4 GB was ~1.3 ms a pass. */
#define WW_POLLED_MASK 0x1014u
unsigned long long g_rsx_ww_poll_us, g_rsx_ww_polls, g_rsx_ww_pages;   /* RSX_ENG_STATS */
unsigned long long g_rsx_ww_heap_us, g_rsx_ww_heap_polls, g_rsx_ww_range_us[3];   /* ...split: heap-only polls; full polls per range */

static int       s_on = -1;
static unsigned  s_epoch = 1;
static unsigned* s_page;          /* epoch at which each page was last seen written */
static unsigned* s_self;          /* ...by the engine itself (rsx_ww_self_end) */
static void**    s_buf;

int rsx_ww_on(void)
{
    if (s_on >= 0) return s_on;
    s_on = 0;
    const char* e = getenv("RACACIT_NO_WRITE_WATCH");
    if ((e && *e && *e != '0') || !vm_base) return 0;
    s_page = (unsigned*)calloc(WW_PAGES, sizeof *s_page);
    s_self = (unsigned*)calloc(WW_PAGES, sizeof *s_self);
    s_buf = (void**)malloc(WW_BUF * sizeof *s_buf);
    if (!s_page || !s_self || !s_buf) return 0;
    /* Probe: GetWriteWatch fails on memory not reserved with MEM_WRITE_WATCH. */
    ULONG_PTR n = WW_BUF; DWORD gran = 0;
    if (GetWriteWatch(0, vm_base + 0x10000u, 0x10000u, s_buf, &n, &gran) != 0) {
        fprintf(stderr, "[rsx ww] guest memory has no write watch: cached data is hashed instead%c", 10);
        return 0;
    }
    s_on = 1;
    fprintf(stderr, "[rsx ww] write watch on: cached geometry and textures are rechecked only when written%c", 10);
    return 1;
}

static void ww_poll_ranges(unsigned nr);
void rsx_ww_poll(void) { ww_poll_ranges(sizeof s_ranges / sizeof s_ranges[0]); }
/* Only s_ranges[0], the heap, and of it only the span full polls have seen
 * written (in Axiom a fraction of the 256 MB; the scan's cost is the span). A
 * page written outside it, or in another range, is stamped by the next full
 * poll instead, with a later epoch: still after any check made in between, as
 * it was without this poll. */
static uint64_t s_heap_lo = ~0ull, s_heap_hi;
unsigned rsx_ww_heap_span_mb(void) { return s_heap_lo < s_heap_hi ? (unsigned)((s_heap_hi - s_heap_lo) >> 20) : 0u; }
void rsx_ww_poll_heap(void) { ww_poll_ranges(1); }

static void ww_poll_ranges(unsigned nr)
{
    if (!rsx_ww_on()) return;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    s_epoch++;
    for (unsigned r = 0; r < nr; r++) {
        ULONG_PTR n = WW_BUF; DWORD gran = 0;
        uint8_t* base = vm_base + s_ranges[r].ea;
        SIZE_T size = s_ranges[r].size;
        if (nr == 1) {
            if (s_heap_lo >= s_heap_hi) break;
            base = vm_base + s_heap_lo; size = (SIZE_T)(s_heap_hi - s_heap_lo);
        }
        LARGE_INTEGER r0; QueryPerformanceCounter(&r0);
        const UINT gw = GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, size, s_buf, &n, &gran);
        if (nr > 1) { LARGE_INTEGER r1; QueryPerformanceCounter(&r1);
                      g_rsx_ww_range_us[r] += (unsigned long long)((r1.QuadPart - r0.QuadPart) * 1000000 / f.QuadPart); }
        if (gw != 0)
            continue;   /* not committed (or not watched) there: nothing to report */
        for (ULONG_PTR i = 0; i < n; i++) {
            const uint64_t off = (uint64_t)((uint8_t*)s_buf[i] - vm_base);
            if (off < WW_SPACE) s_page[off >> WW_PAGE_SHIFT] = s_epoch;
            if (r == 0 && nr > 1) {
                if (off < s_heap_lo) s_heap_lo = off;
                if (off + (1u << WW_PAGE_SHIFT) > s_heap_hi) s_heap_hi = off + (1u << WW_PAGE_SHIFT);
            }
        }
        g_rsx_ww_pages += n;
        { static unsigned long long per[32]; static unsigned polls; static int lg = -1; per[r] += n;
          if (lg < 0) lg = getenv("RSX_WW_LOG") ? 1 : 0;
          if (lg && ++polls % 2000 == 0)
              for (unsigned k = 0; k < sizeof s_ranges / sizeof s_ranges[0]; k++)
                  if (per[k]) fprintf(stderr, "[rsx ww] range 0x%08X: %llu pages written%c", s_ranges[k].ea, per[k], 10); }
    }
    QueryPerformanceCounter(&t1);
    g_rsx_ww_poll_us += (unsigned long long)((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
    g_rsx_ww_polls++;
    if (nr == 1) { g_rsx_ww_heap_us += (unsigned long long)((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart); g_rsx_ww_heap_polls++; }
}

unsigned int rsx_ww_epoch(void) { return s_epoch; }

/* 1 when every page of [host, host+len) is in a polled region, so that
 * rsx_ww_written_since's answer is a real one (outside them it is always 1). */
int rsx_ww_watched(const void* host, unsigned int len)
{
    if (!s_on || !host || !len) return 0;
    const uint64_t off = (uint64_t)((const uint8_t*)host - vm_base);
    if (off >= WW_SPACE || off + len > WW_SPACE) return 0;
    return ((WW_POLLED_MASK >> (off >> 28)) & 1u) && ((WW_POLLED_MASK >> ((off + len - 1u) >> 28)) & 1u);
}

static int ww_since(const void* host, unsigned int len, unsigned int epoch, int with_self);
int rsx_ww_written_since(const void* host, unsigned int len, unsigned int epoch)
{
    return ww_since(host, len, epoch, 1);
}

/* The same, counting only writes that are not the engine's own: whether the
 * game rewrote a GPU target's memory. Caches of guest bytes (textures,
 * geometry) ask rsx_ww_written_since -- for them a write-back is new bytes. */
int rsx_ww_guest_written_since(const void* host, unsigned int len, unsigned int epoch)
{
    return ww_since(host, len, epoch, 0);
}

static int ww_since(const void* host, unsigned int len, unsigned int epoch, int with_self)
{
    if (!s_on || !host || !len) return 1;
    const uint64_t off = (uint64_t)((const uint8_t*)host - vm_base);
    static int s_log = -1;
    if (s_log < 0) s_log = getenv("RSX_WW_LOG") ? 1 : 0;
    if (s_log) { static unsigned long long seen[16]; static unsigned long long n;
      if (off < WW_SPACE) seen[off >> 28]++;
      if (++n % 2000000 == 0)
          for (unsigned k = 0; k < 16; k++)
              if (seen[k]) fprintf(stderr, "[rsx ww] checks in 0x%X0000000: %llu%c", k, seen[k], 10); }
    if (off >= WW_SPACE || off + len > WW_SPACE) return 1;
    if (!((WW_POLLED_MASK >> (off >> 28)) & 1u) || !((WW_POLLED_MASK >> ((off + len - 1u) >> 28)) & 1u))
        return 1;
    const unsigned first = (unsigned)(off >> WW_PAGE_SHIFT);
    const unsigned last  = (unsigned)((off + len - 1u) >> WW_PAGE_SHIFT);
    for (unsigned p = first; p <= last; p++)
        if (s_page[p] > epoch || (with_self && s_self[p] > epoch)) return 1;
    return 0;
}

/* The engine's own writes into guest memory -- a GPU target read back for a
 * CPU copy out of it, a retired target written back -- are not the game's.
 * The watch is per 4 KB page, and every other target, depth buffer and
 * texture on those pages read as rewritten: A Crack in Time packs 16x16
 * effect targets 1 KB apart and copies out of a pair of them (moving its VRAM
 * heap), and the neighbours were then decoded from memory that held none of
 * their drawing.
 * begin takes the pages' pending writes into the coming epoch (what the next
 * poll would have recorded); end clears the pages' state, which by then
 * holds only the engine's write, and records that write for the caches only
 * (rsx_ww_guest_written_since does not see it). A guest write that lands
 * between the two is lost with it: the window is one target's copy loop.
 * Same thread as rsx_ww_poll (the FIFO walker). */
static int ww_pages_of(const void* host, unsigned int len, uint8_t** base, SIZE_T* size)
{
    if (!s_on || !rsx_ww_watched(host, len)) return 0;
    const uint64_t off = (uint64_t)((const uint8_t*)host - vm_base);
    const uint64_t pg = (uint64_t)1 << WW_PAGE_SHIFT;
    const uint64_t a = off & ~(pg - 1u), b = (off + len + pg - 1u) & ~(pg - 1u);
    *base = vm_base + a; *size = (SIZE_T)(b - a);
    return 1;
}

void rsx_ww_self_begin(const void* host, unsigned int len)
{
    uint8_t* base; SIZE_T size;
    if (!ww_pages_of(host, len, &base, &size)) return;
    void* buf[256];
    const SIZE_T most = (SIZE_T)256 << WW_PAGE_SHIFT;   /* buf can never overflow */
    for (SIZE_T done = 0; done < size; done += most) {
        ULONG_PTR n = 256; DWORD gran = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base + done, size - done < most ? size - done : most,
                          buf, &n, &gran) != 0) continue;
        for (ULONG_PTR i = 0; i < n; i++)
            s_page[(uint64_t)((uint8_t*)buf[i] - vm_base) >> WW_PAGE_SHIFT] = s_epoch + 1u;
    }
}

void rsx_ww_self_end(const void* host, unsigned int len)
{
    uint8_t* base; SIZE_T size;
    if (!ww_pages_of(host, len, &base, &size)) return;
    ResetWriteWatch(base, size);
    const unsigned first = (unsigned)((uint64_t)(base - vm_base) >> WW_PAGE_SHIFT);
    for (unsigned p = first; p < first + (unsigned)(size >> WW_PAGE_SHIFT); p++) s_self[p] = s_epoch + 1u;
}

#else   /* no write watch: every reader hashes */
unsigned rsx_ww_heap_span_mb(void) { return 0; }
unsigned long long g_rsx_ww_poll_us, g_rsx_ww_polls, g_rsx_ww_pages;   /* RSX_ENG_STATS */
unsigned long long g_rsx_ww_heap_us, g_rsx_ww_heap_polls, g_rsx_ww_range_us[3];
int          rsx_ww_watched(const void* host, unsigned int len) { (void)host; (void)len; return 0; }
int          rsx_ww_on(void) { return 0; }
void         rsx_ww_poll(void) {}
void         rsx_ww_poll_heap(void) {}
unsigned int rsx_ww_epoch(void) { return 0; }
int          rsx_ww_written_since(const void* host, unsigned int len, unsigned int epoch)
{ (void)host; (void)len; (void)epoch; return 1; }
int          rsx_ww_guest_written_since(const void* host, unsigned int len, unsigned int epoch)
{ (void)host; (void)len; (void)epoch; return 1; }
void         rsx_ww_self_begin(const void* host, unsigned int len) { (void)host; (void)len; }
void         rsx_ww_self_end(const void* host, unsigned int len) { (void)host; (void)len; }
#endif
