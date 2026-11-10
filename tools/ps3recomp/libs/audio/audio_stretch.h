/* audio_stretch.h -- pitch-preserving time stretch for the audio output (WSOLA).
 *
 * The game writes its sound on the CPU. A PC that cannot make the 187.5 blocks a
 * second a PS3's SPU always makes has no sound for the difference, and playing
 * what there is with silence between makes the crackle: 5 ms holes, many a
 * second. Here the sound is played a little longer instead: same pitch, no holes.
 * With nothing to stretch (a stall) it carries on from the sound just played, and
 * fades out if that goes on.
 *
 * Overlap-add of 1024-sample Hann frames every 512 samples. At ratio 1 a frame
 * follows the last exactly and the sum is the input, sample for sample. Otherwise
 * each frame is taken near where the ratio says, at the spot that best continues
 * the last one, so waveforms line up in the overlap (no comb filtering, no clicks).
 *
 * All state lives in one struct; no allocation, no locks: one thread only. */
#ifndef AUDIO_STRETCH_H
#define AUDIO_STRETCH_H

#include <math.h>
#include <string.h>

#define AST_FRAME   1024u               /* samples per channel: 21 ms at 48 kHz */
#define AST_HOP     512u                /* output per step */
#define AST_SEARCH  288                 /* how far a frame may move each way: 6 ms */
#define AST_RING    32768u              /* input kept, played and not: 680 ms */
#define AST_MASK    (AST_RING - 1u)
#define AST_DEC     4                   /* the search compares every 4th sample */
#define AST_FADE_STEPS 8u               /* no new input for this many steps (85 ms): fade out */

typedef struct {
    float ring[AST_RING * 2];           /* stereo input */
    float win[AST_FRAME];
    float ola[AST_FRAME * 2];           /* output being summed */
    float out[AST_HOP * 2];             /* a finished hop */
    unsigned out_pos;                   /* samples of out[] already taken; AST_HOP = none left */
    unsigned long long wr;              /* input samples ever written */
    unsigned long long a_prev;          /* where the last frame came from */
    double nom;                         /* where the ratio says the next frame comes from */
    double ratio;                       /* output time / input time */
    float gain;                         /* the fade, 0..1 */
    unsigned dry;                       /* steps with no new input */
    unsigned long long wr_seen;
    int started;
    unsigned long long dry_steps, stretched_steps, steps, fades;   /* for the report */
} audio_stretch;

static void ast_init(audio_stretch* s)
{
    memset(s, 0, sizeof *s);
    const double pi = 3.14159265358979323846;
    /* Periodic Hann: frames AST_HOP apart sum to exactly 1. */
    for (unsigned i = 0; i < AST_FRAME; i++) s->win[i] = (float)(0.5 - 0.5 * cos(2.0 * pi * i / AST_FRAME));
    s->ratio = 1.0; s->gain = 1.0f; s->out_pos = AST_HOP;
}

/* Samples per channel waiting to be played, in time as they will sound at ratio 1. */
static double ast_level(const audio_stretch* s)
{
    if (!s->started) return (double)s->wr;
    const double d = (double)s->wr - s->nom;
    return (d > 0.0 ? d : 0.0) + (double)(AST_HOP - s->out_pos);
}

static void ast_push(audio_stretch* s, const float* stereo, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        const unsigned w = (unsigned)(s->wr & AST_MASK);
        s->ring[w * 2] = stereo[i * 2]; s->ring[w * 2 + 1] = stereo[i * 2 + 1];
        s->wr++;
    }
}

static void ast_set_ratio(audio_stretch* s, double r)
{
    if (fabs(r - 1.0) < 0.002) r = 1.0;
    s->ratio = r;
}

static inline float ast_mono(const audio_stretch* s, unsigned long long p)
{
    const unsigned w = (unsigned)(p & AST_MASK);
    return s->ring[w * 2] + s->ring[w * 2 + 1];
}

/* How well the AST_HOP samples at a match those at b, -1..1. */
static float ast_match(const audio_stretch* s, unsigned long long a, unsigned long long b, int step)
{
    float ab = 0.0f, aa = 1e-9f, bb = 1e-9f;
    for (unsigned i = 0; i < AST_HOP; i += (unsigned)step) {
        const float x = ast_mono(s, a + i), y = ast_mono(s, b + i);
        ab += x * y; aa += x * x; bb += y * y;
    }
    return ab / sqrtf(aa * bb);
}

/* Sum in the next frame and finish a hop. 0 if the input has not filled the first frame. */
static int ast_step(audio_stretch* s)
{
    if (!s->started) {
        if (s->wr < AST_FRAME) return 0;
        s->started = 1;
        s->a_prev = s->wr - AST_FRAME;
        s->nom = (double)s->a_prev;
    } else {
        const unsigned long long natural = s->a_prev + AST_HOP;
        const unsigned long long newest = s->wr - AST_FRAME;      /* the last whole frame */
        /* The past that is still safely in the ring. */
        const unsigned long long oldest = s->wr > AST_RING - AST_FRAME * 2u ? s->wr - (AST_RING - AST_FRAME * 2u) : 0u;
        unsigned long long a;
        if (s->ratio == 1.0 && natural <= newest && natural >= oldest) {
            a = natural;                                          /* exact: no search, no change to the sound */
            s->nom = (double)a;
        } else {
            s->nom += (double)AST_HOP / s->ratio;
            if (s->nom > (double)newest) s->nom = (double)newest;  /* ran out: no debt, reuse what was played */
            if (s->nom < (double)oldest) s->nom = (double)oldest;
            const unsigned long long c = (unsigned long long)s->nom;
            unsigned long long lo = c > oldest + AST_SEARCH ? c - AST_SEARCH : oldest;
            unsigned long long hi = c + AST_SEARCH;
            if (hi > newest) hi = newest;
            /* Dry, a frame right behind the last would repeat it: look further back
             * so the loop is at least a period long. */
            if (c == newest && lo + AST_SEARCH * 2 > hi) lo = hi > oldest + AST_SEARCH * 2 ? hi - AST_SEARCH * 2 : oldest;
            a = c; float best = -2.0f;
            for (unsigned long long p = lo; p <= hi; p += AST_DEC) {
                const float m = ast_match(s, p, natural, AST_DEC);
                if (m > best) { best = m; a = p; }
            }
            for (unsigned long long p = a > lo + AST_DEC ? a - AST_DEC + 1 : lo; p <= a + AST_DEC - 1 && p <= hi; p++) {
                const float m = ast_match(s, p, natural, 2);
                if (m > best) { best = m; a = p; }
            }
            s->stretched_steps++;
        }
        s->a_prev = a;
    }
    /* The fade: only a real stall, not a slow writer. */
    s->steps++;
    if (s->wr == s->wr_seen) { s->dry++; s->dry_steps++; } else s->dry = 0;
    s->wr_seen = s->wr;
    const float want = s->dry >= AST_FADE_STEPS ? 0.0f : 1.0f;
    if (want == 0.0f && s->gain > 0.0f) s->fades++;
    for (unsigned i = 0; i < AST_FRAME; i++) {
        const unsigned w = (unsigned)((s->a_prev + i) & AST_MASK);
        s->ola[i * 2]     += s->win[i] * s->ring[w * 2];
        s->ola[i * 2 + 1] += s->win[i] * s->ring[w * 2 + 1];
    }
    const float g0 = s->gain, dg = (want - g0) / AST_HOP * 2.0f;    /* a whole fade takes 5 ms */
    float g = g0;
    for (unsigned i = 0; i < AST_HOP; i++) {
        if (dg > 0.0f ? g < want : g > want) { g += dg; if (dg > 0.0f ? g > want : g < want) g = want; }
        s->out[i * 2] = s->ola[i * 2] * g; s->out[i * 2 + 1] = s->ola[i * 2 + 1] * g;
    }
    s->gain = g;
    memmove(s->ola, s->ola + AST_HOP * 2, AST_HOP * 2 * sizeof(float));
    memset(s->ola + AST_HOP * 2, 0, AST_HOP * 2 * sizeof(float));
    s->out_pos = 0;
    return 1;
}

/* n samples per channel of output. 0 (and no output) if there is nothing yet at
 * all, which is only before the first frame. */
static int ast_pull(audio_stretch* s, float* stereo, unsigned n)
{
    unsigned got = 0;
    while (got < n) {
        if (s->out_pos >= AST_HOP && !ast_step(s)) {
            if (!got) return 0;
            memset(stereo + got * 2, 0, (n - got) * 2 * sizeof(float));
            return 1;
        }
        unsigned k = AST_HOP - s->out_pos;
        if (k > n - got) k = n - got;
        memcpy(stereo + got * 2, s->out + s->out_pos * 2, k * 2 * sizeof(float));
        s->out_pos += k; got += k;
    }
    return 1;
}

#endif /* AUDIO_STRETCH_H */
