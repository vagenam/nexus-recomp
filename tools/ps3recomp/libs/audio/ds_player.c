/*
 * ds_player -- the DualSense speaker's own sound player.
 *
 * The game side decides what the controller plays (game/stubs.cpp: by a sound
 * instance's name; runtime/ppu/ppu_fs.cpp: by a dialogue line's speaker) and
 * hands the whole sound over: a SCREAM bank sound, which runs here from its
 * grain script on the player's own clock, or a line's PS-ADPCM data. This
 * file decodes, resamples and mixes them (48 kHz mono) into the controller's
 * speaker and actuator buffers; cellAudio.c (ds_audio_submit) pulls a block at
 * a time. Nothing is taken from the game's own mix. v42-v62 copied the synth
 * voices whose sample addresses had been marked, and samples shared between
 * sounds, reused voices and stream rings let other sounds through (owner,
 * v62: "random noises ... nothing is consistent").
 *
 * SCREAM banks ("klBS", 989snd's SFX block, version 3). In memory every
 * offset below is already an address:
 *   [bank+0x1C] sounds, 12 bytes: vol, vol group, pan s16, grain count,
 *               instance limit, flags s16, first grain
 *   grains, 8 bytes: opcode (type in the top byte, 24-bit argument), then
 *               the delay in ticks (240 Hz) before the next grain
 *   [bank+0x34] + argument of a tone grain, 24 bytes: priority, vol (s8;
 *               negative = register -vol-1), center note (s8; negative = a
 *               48 kHz note), center fine (1/128 semitone), pan s16, map
 *               low/high, bend down/up (semitones), ADSR1, ADSR2, flags,
 *               sample address, size
 * A tone plays at base * 2^((60 - |note| + fine/128 + bend) / 12) Hz, base
 * 48000 for a negative note, 44100 for a positive one (989snd's
 * PS1Note2Pitch). The sign of the fine part is measured: 14 pistol shots in
 * a capture matched the TV's copy at 1.04-1.135x (mean 1.085) with it
 * subtracted -- 1.08x is 2 x 84/128 of a semitone. Samples are PS-ADPCM, 16-byte frames: predictor << 4 |
 * shift, flags (1 end, 2 repeat, 4 loop start), 28 samples.
 */
#include "ds_player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t* vm_base;

#define DS_RATE     48000
#define DS_TICK_HZ  240
#define DS_VOICES   64
#define DS_SCRIPTS  32
#define DS_GRAINS   96
#define DS_MAX_DATA (4u << 20)

typedef struct {
    int      used;
    uint8_t* data;
    uint32_t nfr, fr;          /* frames, next frame to decode */
    int      loop_fr;          /* the last loop-start frame seen, -1 none */
    int      last;             /* the frame decoded last had the end flag */
    int32_t  h1, h2;           /* ADPCM history */
    int16_t  buf[28];
    int      bi;               /* next sample of buf; 28 = decode a frame */
    float    s0, s1;           /* the samples the output lies between */
    double   phase, step;      /* position between s0 and s1; input samples per output sample */
    double   base;             /* step before the script's pitch bend */
    int      up, down;         /* the tone's bend range, semitones */
    int      ended;            /* no more input: s1 was the last sample */
    float    spk, felt;        /* levels */
    float    env, fade;        /* 1 = full; per-sample decrement once released */
    uint32_t delay;            /* output samples before it starts */
    uint32_t owner;
    uint32_t handle;           /* the game's SCREAM sound it belongs to, 0 unknown */
    int      dead;             /* checks that found that sound gone */
    int      script;           /* the script that started it, -1 */
    uint32_t age;              /* output samples played */
    uint32_t line;             /* a dialogue line's token, 0 otherwise */
    uint32_t read_fr, margin;  /* frames of it the game has read; how far past that it may run */
    uint32_t held;             /* output samples spent holding for the game's reads */
    float    gate;             /* 1 = playing, 0 = holding */
    char     name[32];
} ds_voice;

typedef struct {
    int      used;
    uint32_t bank, tab;        /* bank, tone table */
    uint32_t g[DS_GRAINS][2];
    int      ng, pc;
    int32_t  countdown;        /* ticks before the next grain */
    int      play_left, skip_after;   /* a RAND_PLAY / PLAY_CYCLE choice being played */
    int      loop_pc, stop_pc; /* LOOP_START; the release part (after ON_STOP_MARKER), -1 none */
    int      stopping, waiting, parked;   /* parked: at ON_STOP_MARKER, ends with its voices */
    uint32_t ticks;            /* age */
    float    bend;             /* -1..1 of the tone's bend range */
    int      reg[4];
    float    vol, spk, felt;
    float    lift;             /* the whole sound brought up (sound_lift) */
    uint32_t owner;
    uint32_t handle;
    int      dead;
    char     name[48];
} ds_script;

static ds_voice  s_v[DS_VOICES];
static ds_script s_s[DS_SCRIPTS];
static volatile long s_mu;
static volatile int  s_on;
static double s_tick_acc;
static uint32_t s_snd;         /* SCREAM's globals (ds_player_bind) */
static uint32_t s_check;       /* output samples since the last handle check */
static uint64_t s_clock;       /* output samples rendered (debug) */
static uint32_t s_rng = 0x9E3779B9u;

static void lock(void)   { while (__sync_lock_test_and_set(&s_mu, 1)) { } }
static void unlock(void) { __sync_lock_release(&s_mu); }
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
static float frand(void) { return (float)(rnd() & 0xFFFFFF) / 8388608.0f - 1.0f; }   /* -1..1 */

static int ea_ok(uint32_t ea, uint32_t n) { return vm_base && ea >= 0x10000u && ea < 0x40000000u && n <= 0x40000000u - ea; }
static uint32_t g32(uint32_t ea) { const uint8_t* p = vm_base + ea; return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void ds_player_enable(int on) { s_on = on; }

/* RACACIT_DS_DEBUG=1: voice starts and stops in the log (tests). */
static int dbg(void) { static int d = -1; if (d < 0) { const char* e = getenv("RACACIT_DS_DEBUG"); d = e && *e == '1'; } return d; }
int  ds_player_enabled(void) { return s_on; }

/* Each sound name once in the log, with what it played. */
static void say_once(const char* what, const char* name, const char* detail)
{
    static char seen[512][48]; static int n; static volatile long mu;
    while (__sync_lock_test_and_set(&mu, 1)) { }
    int i = 0;
    while (i < n && strcmp(seen[i], name)) i++;
    const int fresh = i == n && n < 512;
    if (fresh) snprintf(seen[n++], sizeof seen[0], "%s", name);
    __sync_lock_release(&mu);
    if (fresh) fprintf(stderr, "[ds] %s %s%s%c", what, name, detail, 10);
}

/* ---- voices ---------------------------------------------------------------- */

static const int k_coef[5][2] = { { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 } };

static int decode_frame(ds_voice* v)
{
    if (v->last || v->fr >= v->nfr) return 0;
    const uint8_t* f = v->data + (size_t)v->fr * 16u;
    const int pr = f[0] >> 4, sh = f[0] & 15, fl = f[1];
    const int c1 = pr < 5 ? k_coef[pr][0] : 0, c2 = pr < 5 ? k_coef[pr][1] : 0;
    if (fl & 4) v->loop_fr = (int)v->fr;
    for (int i = 0; i < 14; i++) {
        for (int k = 0; k < 2; k++) {
            int s = (f[2 + i] >> (k * 4)) & 15;
            if (s >= 8) s -= 16;
            int32_t x = (sh <= 12 ? (s << 12) >> sh : 0) + ((v->h1 * c1 + v->h2 * c2) >> 6);
            if (x > 32767) x = 32767; if (x < -32768) x = -32768;
            v->h2 = v->h1; v->h1 = x;
            v->buf[i * 2 + k] = (int16_t)x;
        }
    }
    v->bi = 0;
    if (fl & 1) {   /* end: repeat from the loop start, or this was the last frame */
        if ((fl & 2) && v->loop_fr >= 0) v->fr = (uint32_t)v->loop_fr;
        else v->last = 1;
    } else v->fr++;
    return 1;
}

static int next_sample(ds_voice* v, float* out)
{
    if (v->bi >= 28 && !decode_frame(v)) return 0;
    *out = (float)v->buf[v->bi++] * (1.0f / 32768.0f);
    return 1;
}

/* A bank sample's loudness, for the ceiling (v69) and the lift below: its loudest ~40 ms
 * (64 frames) and ~400 ms (640 frames) RMS, from the first 4096 frames (~2.4 s at 48 kHz),
 * once per sample (cached by address). */
static void sample_loudness(const uint8_t* src, uint32_t n, float* r40, float* r400)
{
    static struct { const uint8_t* src; uint32_t n, head; float r40, r400; } cache[1024];   /* head: a level reloads banks */
    const unsigned h = (unsigned)(((uintptr_t)src >> 4) ^ n) & 1023u;
    uint32_t head; memcpy(&head, src + 2, 4);
    if (cache[h].src != src || cache[h].n != n || cache[h].head != head) {
        ds_voice t; memset(&t, 0, sizeof t);
        t.data = (uint8_t*)src; t.nfr = n < 4096u ? n : 4096u; t.bi = 28;
        static double fs[640]; memset(fs, 0, sizeof fs);
        double w40 = 0.0, w400 = 0.0, b40 = 0.0, b400 = 0.0;
        for (uint32_t f = 0; f < t.nfr && !t.last && decode_frame(&t); f++) {
            double ss = 0.0;
            for (int i = 0; i < 28; i++) { const double x = t.buf[i] / 32768.0; ss += x * x; }
            w40 += ss - fs[(f + 576u) % 640u];      /* the frame 64 ago */
            w400 += ss - fs[f % 640u];
            fs[f % 640u] = ss;
            if (w40 > b40) b40 = w40;
            if (w400 > b400) b400 = w400;
            if (t.fr <= f) break;                   /* looped back: measured once through */
        }
        cache[h].src = src; cache[h].n = n; cache[h].head = head;
        cache[h].r40 = (float)sqrt(b40 / (64.0 * 28.0)); cache[h].r400 = (float)sqrt(b400 / (640.0 * 28.0));
    }
    *r40 = cache[h].r40; *r400 = cache[h].r400;
}

/* The ceiling (v69): the gain that brings a sample's loudest ~40 ms down to
 * RACACIT_DS_VOICE_RMS (default 0.20) and, since 0.9.5, its loudest ~400 ms down to
 * RACACIT_DS_VOICE_RMS400 (default 0.10), never up. Banks mix samples cut at very different
 * levels, and every routed sound reached the limiter at its sample's own level ("there needs
 * to be a clean baseline for how loud certain volumes of weapons can be", owner, v68). 40 ms
 * alone let a long roar stand at the level of a short pop and sound far louder: shotgun and
 * Dynamo 0.058-0.069 over 400 ms against 0.03-0.04 for a pop ("some weapons being too loud",
 * owner, 0.9.4). */
static float voice_ceiling(const uint8_t* src, uint32_t n)
{
    static float ref = -1.0f, ref4;
    if (ref < 0.0f) { const char* e = getenv("RACACIT_DS_VOICE_RMS"); ref = e ? (float)atof(e) : 0.20f;
                      e = getenv("RACACIT_DS_VOICE_RMS400"); ref4 = e ? (float)atof(e) : 0.10f; }
    if (ref <= 0.0f) return 1.0f;
    float r40, r400; sample_loudness(src, n, &r40, &r400);
    float g = r40 > ref ? ref / r40 : 1.0f;
    if (ref4 > 0.0f && r400 * g > ref4) g = ref4 / r400;
    return g;
}

/* A sample's frames: whole frames up to and including the first end frame, or `max` bytes. */
static uint32_t sample_frames(const uint8_t* src, uint32_t max)
{
    uint32_t n = 0;
    for (uint32_t f = 0; (f + 1u) * 16u <= max && (f + 1u) * 16u <= DS_MAX_DATA; f++) {
        n = f + 1u;
        if (src[f * 16u + 1] & 1) break;   /* the end frame (a loop's too: it jumps back) */
    }
    return n;
}

/* A voice for the frames at `src` (whole frames up to and including the
 * first end frame, or `max` bytes); the data is copied. -1 if none is free.
 * A bank sound's voice (script >= 0) is held to its sample's ceiling, on
 * the speaker and the actuators. */
static void voice_free(ds_voice* v);
static int voice_start(const uint8_t* src, uint32_t max, uint32_t rate, float spk, float felt,
                       uint32_t owner, int script, uint32_t delay, int up, int down, const char* name)
{
    if (rate < 1000u || rate > 192000u || max < 16) return -1;
    const uint32_t n = sample_frames(src, max);
    if (!n) return -1;
    if (script >= 0) {
        const float g = voice_ceiling(src, n);
        spk *= g; felt *= g;
        if (dbg() && g != 1.0f) { static int nl; if (nl++ < 200)
            fprintf(stderr, "[ds-dbg] %s: sample %p held to its ceiling, x%.2f%c", name ? name : "?", (const void*)src, g, 10); }
    }
    int k = -1;
    for (int i = 0; i < DS_VOICES; i++) if (!s_v[i].used) { k = i; break; }
    /* Full: an effect makes room -- one already fading, else the oldest. A
     * line's voice is never taken. The RYNO V's rockets alone held all 64
     * voices, and its 1812 Overture (a line) found none ("player full",
     * 0.9.7); a new sound now takes the oldest one's place instead of being
     * dropped. */
    if (k < 0) {
        uint64_t best = 0;
        for (int i = 0; i < DS_VOICES; i++) {
            if (!s_v[i].used || s_v[i].line) continue;
            const uint64_t score = (uint64_t)s_v[i].age + (s_v[i].fade > 0.0f ? (1ull << 40) : 0ull) + 1u;
            if (score > best) { best = score; k = i; }
        }
        if (k >= 0) voice_free(&s_v[k]);
    }
    if (k < 0) return -1;
    uint8_t* d = (uint8_t*)malloc((size_t)n * 16u);
    if (!d) return -1;
    memcpy(d, src, (size_t)n * 16u);
    ds_voice* v = &s_v[k];
    memset(v, 0, sizeof *v);
    v->data = d; v->nfr = n; v->loop_fr = -1; v->bi = 28;
    v->step = v->base = (double)rate / DS_RATE;
    v->up = up; v->down = down;
    v->spk = spk; v->felt = felt; v->env = 1.0f;
    v->owner = owner; v->script = script; v->delay = delay;
    snprintf(v->name, sizeof v->name, "%s", name ? name : "");
    if (!next_sample(v, &v->s0)) v->s0 = 0.0f;
    if (!next_sample(v, &v->s1)) { v->s1 = 0.0f; v->ended = 1; }
    v->used = 1;
    return k;
}

static void voice_free(ds_voice* v) { free(v->data); memset(v, 0, sizeof *v); }

/* Key off: a short fade (the synth's release is not modelled). */
static void voice_release(ds_voice* v) { if (v->fade == 0.0f) v->fade = 1.0f / (0.030f * DS_RATE); }

/* ---- SCREAM scripts ---------------------------------------------------------- */

static void say_grain(int type)
{
    static uint8_t said[256];
    if (said[type & 255]) return;
    said[type & 255] = 1;
    fprintf(stderr, "[ds] grain type 0x%02X not played (first seen)%c", type, 10);
}

static void tone_start(ds_script* s, uint32_t arg)
{
    const uint32_t t = s->tab + arg;
    if (!ea_ok(t, 24)) return;
    const uint8_t* r = vm_base + t;
    int vol = (int8_t)r[1];
    if (vol < 0) { const int i = -vol - 1; vol = i < 4 && s->reg[i] > 0 ? s->reg[i] : 127; }
    const int cn = (int8_t)r[2], fine = r[3] & 0x7F;
    const int down = r[8], up = r[9];
    const double semis = 60.0 - (cn < 0 ? -cn : cn) + fine / 128.0;
    const uint32_t rate = (uint32_t)((cn < 0 ? 48000.0 : 44100.0) * pow(2.0, semis / 12.0) + 0.5);
    const uint32_t ea = g32(t + 16), size = g32(t + 20);
    const uint32_t max = size && size < DS_MAX_DATA ? size : DS_MAX_DATA;
    if (!ea_ok(ea, max)) return;
    /* 0.707: a centred voice's share of the TV's L/R, so a stereo pair (two
     * tones panned +-15 degrees) sums like the TV's channels do. */
    const float lv = s->vol * s->lift * (float)(vol > 127 ? 127 : vol) / 127.0f * 0.707f;
    const int k = voice_start(vm_base + ea, max, rate, lv * s->spk, lv * s->felt, s->owner, (int)(s - s_s), 0, up, down, s->name);
    if (dbg()) { static int nl; if (nl++ < 400)
        fprintf(stderr, "[ds-dbg] %s tone +%X: %08X %u Hz vol %.2f (%u frames)%s%c", s->name, arg, ea, rate, lv,
                k >= 0 ? s_v[k].nfr : 0, k < 0 ? " NOT STARTED" : "", 10); }
    if (k >= 0) s_v[k].handle = s->handle;
    if (k >= 0 && s->bend != 0.0f) {
        const float st = s->bend >= 0.0f ? s->bend * (float)up : s->bend * (float)down;
        s_v[k].step = s_v[k].base * pow(2.0, st / 12.0);
    }
}

/* A pitch bend moves the script's playing voices too (989snd bends a
 * sound's voices: the Dynamo's charging whine rises by ADD_PB in a loop). */
static void bend_apply(ds_script* s)
{
    for (int i = 0; i < DS_VOICES; i++) {
        ds_voice* v = &s_v[i];
        if (!v->used || v->script != (int)(s - s_s)) continue;
        const float st = s->bend >= 0.0f ? s->bend * (float)v->up : s->bend * (float)v->down;
        v->step = v->base * pow(2.0, st / 12.0);
    }
}

static int script_start(uint32_t bank, uint32_t sound, float vol, float spk, float felt,
                        uint32_t owner, uint32_t handle, const char* name, int depth);

/* STARTCHILDSOUND: 32 bytes at the tone table + argument {vol s32, pan s32,
 * register settings, sound s32, name[16]}. */
static void child_start(ds_script* s, uint32_t arg, int depth)
{
    const uint32_t p = s->tab + arg;
    if (depth > 3 || !ea_ok(p, 32)) return;
    const int32_t vol = (int32_t)g32(p), snd = (int32_t)g32(p + 12);
    const float cv = vol > 0 && vol <= 127 ? (float)vol / 127.0f : 1.0f;
    if (snd >= 0) script_start(s->bank, (uint32_t)snd, s->vol * cv, s->spk, s->felt, s->owner, s->handle, s->name, depth + 1);
}

/* One grain; returns 0 when the script ends. */
static int grain_run(ds_script* s, int depth)
{
    const uint32_t op = s->g[s->pc][0];
    const int type = (int)(op >> 24), a = (op >> 16) & 0xFF, b = (op >> 8) & 0xFF, c = op & 0xFF;
    const uint32_t arg = op & 0xFFFFFF;
    switch (type) {
    case 0x01: tone_start(s, arg); break;               /* TONE */
    case 0x05: child_start(s, arg, depth); break;       /* STARTCHILDSOUND */
    case 0x14: break;                                   /* CONTROL_NULL */
    case 0x15: s->loop_pc = s->pc + 1; break;           /* LOOP_START */
    case 0x16: if (!s->stopping && s->loop_pc >= 0) { s->pc = s->loop_pc - 1; } break;   /* LOOP_END */
    case 0x17: break;                                   /* LOOP_CONTINUE */
    case 0x18: return 0;                                /* STOP */
    case 0x19: case 0x27: {                             /* RAND_PLAY / PLAY_CYCLE: one of a groups of b */
        if (a < 1 || b < 1) break;
        int k;
        if (type == 0x19) k = (int)(rnd() % (uint32_t)a);
        else {   /* the next one each time this sound plays */
            static struct { uint32_t key; int n; } cyc[64]; static unsigned nc;
            const uint32_t key = s->g[0][0] ^ (s->bank * 2654435761u) ^ (uint32_t)s->pc;
            unsigned i = 0; while (i < nc && cyc[i].key != key) i++;
            if (i == nc) { i = nc < 64 ? nc++ : (unsigned)(rnd() % 64); cyc[i].key = key; cyc[i].n = 0; }
            k = cyc[i].n++ % a;
        }
        s->pc += k * b;                       /* the grain after this one is the chosen group's first */
        s->play_left = b;
        s->skip_after = (a - 1 - k) * b;
        return 1; }
    case 0x30: {   /* RAND_PLAY with a 24-byte record {groups, per group, ?, no-repeat depth}.
                    * Not a tone (its sample is 0xFFFFFFFF in all 88 uses): read as one it
                    * started nothing and every group after it played at once -- the
                    * Constructo Shotgun fired 12 shots together into the limiter. */
        const uint32_t t = s->tab + arg;
        if (!ea_ok(t, 4)) break;
        const int ga = vm_base[t], gb = vm_base[t + 1];
        if (ga < 1 || gb < 1) break;
        int hist = vm_base[t + 3] < 4 ? vm_base[t + 3] : 3;
        if (hist > ga - 1) hist = ga - 1;
        static struct { uint32_t key; int last[4]; } rp[64]; static unsigned nr;
        const uint32_t key = t ^ (s->bank * 2654435761u);
        unsigned i = 0; while (i < nr && rp[i].key != key) i++;
        if (i == nr) { i = nr < 64 ? nr++ : (unsigned)(rnd() % 64); rp[i].key = key; for (int h = 0; h < 4; h++) rp[i].last[h] = -1; }
        int k = 0;
        for (int tries = 0; tries < 16; tries++) {   /* not one of the last `hist` picks */
            k = (int)(rnd() % (uint32_t)ga);
            int seen = 0; for (int h = 0; h < hist; h++) if (rp[i].last[h] == k) seen = 1;
            if (!seen) break;
        }
        for (int h = 3; h > 0; h--) rp[i].last[h] = rp[i].last[h - 1];
        rp[i].last[0] = k;
        s->pc += k * gb;
        s->play_left = gb;
        s->skip_after = (ga - 1 - k) * gb;
        return 1; }
    case 0x1A: s->countdown += arg ? (int32_t)(rnd() % (arg + 1u)) : 0; break;   /* RAND_DELAY */
    case 0x1B: s->bend = frand() * (float)a / 127.0f; break;            /* RAND_PB: the tones that follow */
    case 0x1C: s->bend = (float)(int8_t)a / 127.0f; bend_apply(s); break;   /* PB */
    case 0x1D: s->bend += (float)(int8_t)a / 127.0f;                      /* ADD_PB */
               if (s->bend > 1.0f) s->bend = 1.0f; if (s->bend < -1.0f) s->bend = -1.0f;
               bend_apply(s); break;
    case 0x1E: if (a < 4) s->reg[a] = (int8_t)b; break;                  /* SET_REGISTER */
    case 0x1F: if (a < 4) s->reg[a] = b <= c ? b + (int)(rnd() % (uint32_t)(c - b + 1)) : b; break;   /* SET_REGISTER_RAND */
    case 0x20: if (a < 4) s->reg[a]++; break;                            /* INC_REGISTER */
    case 0x21: if (a < 4) s->reg[a]--; break;                            /* DEC_REGISTER */
    case 0x23: break;                                                    /* MARKER */
    case 0x26: s->waiting = 1; break;                                    /* WAIT_FOR_ALL_VOICES */
    case 0x29: case 0x2A:                                                /* KEY_OFF_VOICES / KILL_VOICES */
        for (int i = 0; i < DS_VOICES; i++) if (s_v[i].used && s_v[i].script == (int)(s - s_s)) voice_release(&s_v[i]);
        break;
    case 0x2B: if (!s->stopping) { s->parked = 1; return 1; } break;     /* ON_STOP_MARKER: what follows plays on stop */
    default: say_grain(type); break;
    }
    return 1;
}

/* A script ends; its voices play on, no longer tied to the slot (it is
 * reused). `kill`: they fade now too. */
static void script_end(ds_script* s, int kill)
{
    for (int i = 0; i < DS_VOICES; i++) {
        if (!s_v[i].used || s_v[i].script != (int)(s - s_s)) continue;
        s_v[i].script = -1;
        if (kill) voice_release(&s_v[i]);
    }
    s->used = 0;
}

static int script_voices(const ds_script* s)
{
    for (int i = 0; i < DS_VOICES; i++) if (s_v[i].used && s_v[i].script == (int)(s - s_s)) return 1;
    return 0;
}

static void script_tick(ds_script* s)
{
    /* Loops run as long as the game lets the sound play; a minute is a
     * missed stop -- 20 s for a sound the game gave no handle for (only its
     * stop hook can end it, and some stops take paths no hook sees: the
     * owner's v66 "the speaker is still looping sounds"). */
    if (++s->ticks > (s->handle ? 60u : 20u) * DS_TICK_HZ) { script_end(s, 1); return; }
    if (s->parked) { if (!script_voices(s)) s->used = 0; return; }
    if (s->waiting) { if (script_voices(s)) return; s->waiting = 0; }
    if (s->countdown > 0 && --s->countdown > 0) return;
    for (int guard = 0; guard < 64; guard++) {
        if (s->pc >= s->ng) { script_end(s, 0); return; }
        const int pc = s->pc;
        if (!grain_run(s, 0)) { script_end(s, 0); return; }
        if (s->parked) return;
        if (s->play_left > 0 && --s->play_left == 0) { s->pc += s->skip_after; s->skip_after = 0; }
        const uint32_t delay = s->g[pc][1];
        s->pc++;
        if (s->waiting) return;
        if (delay) { s->countdown = (int32_t)(delay > 100000u ? 100000u : delay); return; }
    }
    s->countdown = 1;   /* a zero-delay loop: one pass per tick */
}

/* A quiet sound brought up, as one sound: the gain that takes its loudest tone, after the
 * ceiling, up to the ceiling itself, 40 ms and 400 ms, at most RACACIT_DS_LIFT_MAX (default 6). Only for the
 * names in RACACIT_DS_LIFT (stubs.cpp list syntax without ^): what the player does, not the
 * hums and tails that are meant to sit low. The Constructo Pistol's shot reached the speaker
 * at a sixth of the other guns' ("some being too quiet", owner, 0.9.4). A tone at a time (v77)
 * would have raised a shot's quiet layers as far as its main one. Never a looping sound: a
 * loop is meant to sit under everything, and the names catch some (the Spiral's "_hit_*_rolling"). */
static float sound_lift(const ds_script* s, const char* name)
{
    static const char* list = (const char*)-1; static float ref = -1.0f, ref4, cap;
    if (list == (const char*)-1) {
        const char* e = getenv("RACACIT_DS_LIFT");
        list = e ? e : "ui_upgrade_,zoni,fire,firing,shot,throw,launch,zap,explo,blast,shockwave,_hit";
        e = getenv("RACACIT_DS_VOICE_RMS"); ref = e ? (float)atof(e) : 0.20f;
        e = getenv("RACACIT_DS_VOICE_RMS400"); ref4 = e ? (float)atof(e) : 0.10f;
        e = getenv("RACACIT_DS_LIFT_MAX"); cap = e ? (float)atof(e) : 6.0f;
    }
    int on = 0;
    if (name && *list)
        for (const char* q = list; *q && !on; ) {
            const char* c = strchr(q, ','); const size_t len = c ? (size_t)(c - q) : strlen(q);
            for (const char* t = name; *t && !on; t++) if (!strncmp(t, q, len)) on = 1;
            q += len + (c ? 1 : 0);
        }
    if (!on || ref <= 0.0f) return 1.0f;
    float best = 0.0f, best4 = 0.0f;
    for (int i = 0; i < s->ng; i++) if ((s->g[i][0] >> 24) == 0x15) return 1.0f;   /* LOOP_START */
    for (int i = 0; i < s->ng; i++) {
        if ((s->g[i][0] >> 24) != 0x01) continue;
        const uint32_t t = s->tab + (s->g[i][0] & 0xFFFFFFu);
        if (!ea_ok(t, 24)) continue;
        const uint32_t ea = g32(t + 16), size = g32(t + 20), max = size && size < DS_MAX_DATA ? size : DS_MAX_DATA;
        if (!ea_ok(ea, max)) continue;
        const uint32_t n = sample_frames(vm_base + ea, max);
        if (!n) continue;
        if (vm_base[ea + (n - 1u) * 16u + 1] & 2) return 1.0f;   /* a sample that repeats */
        float r40, r400; sample_loudness(vm_base + ea, n, &r40, &r400);
        const int v = (int8_t)vm_base[t + 1];
        const float k = voice_ceiling(vm_base + ea, n) * (float)(v < 0 ? 127 : v) / 127.0f;
        if (r40 * k > best) best = r40 * k;
        if (r400 * k > best4) best4 = r400 * k;
    }
    if (best <= 0.0f) return 1.0f;
    float g = ref / best;
    if (ref4 > 0.0f && best4 > 0.0f && ref4 / best4 < g) g = ref4 / best4;
    if (g > cap) g = cap;
    return g > 1.0f ? g : 1.0f;
}

static int script_start(uint32_t bank, uint32_t sound, float vol, float spk, float felt,
                        uint32_t owner, uint32_t handle, const char* name, int depth)
{
    if (!ea_ok(bank, 0x40) || g32(bank) != 0x6B6C4253u) return -1;      /* "klBS" */
    if (sound >= (g32(bank + 0x14) & 0x7FFFu)) return -1;
    const uint32_t rec = g32(bank + 0x1C) + sound * 12u;
    if (!ea_ok(rec, 12)) return -1;
    const uint32_t ng = vm_base[rec + 4], gp = g32(rec + 8);
    if (!ng || !ea_ok(gp, ng * 8u)) return -1;
    int k = -1;
    for (int i = 0; i < DS_SCRIPTS; i++) if (!s_s[i].used) { k = i; break; }
    if (k < 0) return -1;
    ds_script* s = &s_s[k];
    memset(s, 0, sizeof *s);
    s->bank = bank; s->tab = g32(bank + 0x34);
    s->ng = (int)(ng < DS_GRAINS ? ng : DS_GRAINS);
    for (int i = 0; i < s->ng; i++) { s->g[i][0] = g32(gp + i * 8u); s->g[i][1] = g32(gp + i * 8u + 4); }
    s->loop_pc = s->stop_pc = -1;
    for (int i = 0; i < s->ng; i++) if ((s->g[i][0] >> 24) == 0x2B) { s->stop_pc = i + 1; break; }
    s->vol = vol * (float)(vm_base[rec] > 127 ? 127 : vm_base[rec]) / 127.0f;
    s->spk = spk; s->felt = felt; s->owner = owner; s->handle = handle;
    snprintf(s->name, sizeof s->name, "%s", name ? name : "");
    s->lift = sound_lift(s, s->name);
    s->used = 1;
    /* The first grains run now: the sound starts with the game's. */
    script_tick(s);
    (void)depth;
    return k;
}

/* ---- the API -------------------------------------------------------------------- */

/* A game sound instance starting again (the game reuses them) ends what it
 * played before -- that sound is over in the game even when no stop reached
 * us. Not the sounds of this same start (one start can begin several). */
static void owner_restart(uint32_t owner)
{
    if (!owner) return;
    for (int i = 0; i < DS_SCRIPTS; i++)
        if (s_s[i].used && s_s[i].owner == owner && s_s[i].ticks > DS_TICK_HZ / 10u) script_end(&s_s[i], 1);
    for (int j = 0; j < DS_VOICES; j++)
        if (s_v[j].used && s_v[j].owner == owner && s_v[j].script < 0 && s_v[j].age > DS_RATE / 10u) voice_release(&s_v[j]);
}

void ds_player_scream(uint32_t bank_ea, uint32_t sound, float spk, float felt, uint32_t owner, const char* name)
{
    if (!s_on || (spk <= 0.0f && felt <= 0.0f)) return;
    lock();
    owner_restart(owner);
    const int k = script_start(bank_ea, sound, 1.0f, spk, felt, owner, 0, name, 0);
    unlock();
    char d[64];
    snprintf(d, sizeof d, " (bank %08X sound %u)%s%s", bank_ea, sound, spk > 0.0f ? "" : " -- actuators only",
             k < 0 ? " -- NOT played: bank unreadable or player full" : "");
    say_once("plays", name && *name ? name : "?", d);
}

void ds_player_adpcm(const uint8_t* data, uint32_t bytes, uint32_t rate, float spk, float felt, uint32_t owner,
                     uint32_t delay_ms, const char* name)
{
    if (!s_on || !data || bytes < 16) return;
    lock();
    owner_restart(owner);
    const int k = voice_start(data, bytes, rate, spk, felt, owner, -1, (uint32_t)((uint64_t)delay_ms * DS_RATE / 1000u), 0, 0, name);
    unlock();
    char d[64];
    snprintf(d, sizeof d, " (%u bytes at %u Hz)%s", bytes, rate, k < 0 ? " -- NOT played: player full" : "");
    say_once("plays line", name && *name ? name : "?", d);
}

/* ---- dialogue lines ------------------------------------------------------------ */

/* The game's streams that are paused now. Every stream is paused for a moment at each start,
 * so only one still paused after 150 ms counts: then the game has paused its sound (a pause
 * menu), and the controller's lines hold until it resumes them. */
#define DS_STREAMS 16
#define DS_PAUSE_DEBOUNCE (DS_RATE * 3u / 20u)
static struct { uint32_t stream; uint64_t at; } s_paused[DS_STREAMS];
static volatile int s_lines_paused;
static uint64_t s_pause_at;    /* s_clock when they began to hold */
static uint32_t s_line_serial;

static void pause_clear(void) { memset(s_paused, 0, sizeof s_paused); s_lines_paused = 0; }

/* Once per render block. */
static void pause_update(void)
{
    int on = 0;
    for (int i = 0; i < DS_STREAMS; i++) if (s_paused[i].stream && s_clock - s_paused[i].at >= DS_PAUSE_DEBOUNCE) on = 1;
    if (on != s_lines_paused) {
        if (on) s_pause_at = s_clock;
        s_lines_paused = on;
        if (dbg()) fprintf(stderr, "[ds-pause] lines %s at %.2f s%c", on ? "hold" : "go on", s_clock / (double)DS_RATE, 10);
    }
}

void ds_player_stream_pause(uint32_t stream, int on)
{
    lock();
    int k = -1, free_k = -1;
    for (int i = 0; i < DS_STREAMS; i++) {
        if (s_paused[i].stream == stream) k = i;
        else if (!s_paused[i].stream && free_k < 0) free_k = i;
    }
    if (on && k < 0 && free_k >= 0) { s_paused[free_k].stream = stream; s_paused[free_k].at = s_clock; }
    else if (!on && k >= 0) s_paused[k].stream = 0;
    unlock();
}

uint32_t ds_player_line(const uint8_t* data, uint32_t bytes, uint32_t rate, float spk, uint32_t delay_ms, const char* name)
{
    if (!s_on || !data || bytes < 16) return 0;
    lock();
    pause_clear();        /* the game is starting speech: nothing of its is held */
    const uint32_t token = 0x80000000u | (++s_line_serial & 0x7FFFFFFFu);
    const int k = voice_start(data, bytes, rate, spk, 0.0f, 0u, -1, (uint32_t)((uint64_t)delay_ms * DS_RATE / 1000u), 0, 0, name);
    if (k >= 0) { s_v[k].line = token; s_v[k].gate = 1.0f; s_v[k].margin = rate * 2u / 28u; }
    unlock();
    char d[64];
    snprintf(d, sizeof d, " (%u bytes at %u Hz)%s", bytes, rate, k < 0 ? " -- NOT played: player full" : "");
    say_once("plays line", name && *name ? name : "?", d);
    return token;
}

void ds_player_line_read(uint32_t token, uint32_t bytes)
{
    if (!token) return;
    const uint32_t fr = bytes / 16u;
    lock();
    int moved = 0;
    for (int j = 0; j < DS_VOICES; j++)
        if (s_v[j].used && s_v[j].line == token && fr > s_v[j].read_fr) { s_v[j].read_fr = fr; moved = 1; }
    /* A line the game goes on reading, 2 s into a pause, is not paused: that pause was some
     * other stream's. */
    if (moved && s_lines_paused && s_clock - s_pause_at > 2u * DS_RATE) {
        pause_clear();
        fprintf(stderr, "[ds-pause] the game is still reading a paused line: playing on%c", 10);
    }
    if (moved && dbg()) { static int nl; if (nl++ < 400) for (int j = 0; j < DS_VOICES; j++) if (s_v[j].used && s_v[j].line == token)
        fprintf(stderr, "[ds-dbg] %s read %u of %u frames, playing frame %u%c", s_v[j].name, fr, s_v[j].nfr, s_v[j].fr, 10); }
    unlock();
}

void ds_player_stop_lines(int all)
{
    lock();
    for (int j = 0; j < DS_VOICES; j++) {
        ds_voice* v = &s_v[j];
        if (!v->used || !v->line) continue;
        if (!all && (strstr(v->name, "_zurk_") || strstr(v->name, "_zoni_") || strstr(v->name, "mus_ryno_"))) continue;   /* weapons' lines */
        if (dbg()) fprintf(stderr, "[ds-dbg] %s stopped by the game at frame %u of %u, %.2f s%c", v->name, v->fr, v->nfr, s_clock / (double)DS_RATE, 10);
        voice_release(v);
    }
    if (all) pause_clear();
    unlock();
}

void ds_player_stop(uint32_t owner)
{
    if (!owner) return;
    lock();
    for (int i = 0; i < DS_SCRIPTS; i++) {
        ds_script* s = &s_s[i];
        if (!s->used || s->owner != owner || s->stopping) continue;
        if (dbg()) { static int nl; if (nl++ < 300) fprintf(stderr, "[ds-dbg] stop %s (instance %08X)%s%c", s->name, owner,
                                                            s->stop_pc >= 0 ? ": release part" : "", 10); }
        /* Voices playing now fade; a release part (after ON_STOP_MARKER) plays on. */
        for (int j = 0; j < DS_VOICES; j++) if (s_v[j].used && s_v[j].script == i) voice_release(&s_v[j]);
        if (s->stop_pc >= 0 && s->stop_pc < s->ng) {
            s->stopping = 1; s->parked = 0; s->waiting = 0; s->play_left = 0;
            s->pc = s->stop_pc; s->countdown = 0;
        }
        else s->used = 0;
    }
    for (int j = 0; j < DS_VOICES; j++) if (s_v[j].used && s_v[j].owner == owner && s_v[j].script < 0) voice_release(&s_v[j]);
    unlock();
}

/* SCREAM's own handle check (func_00982858, without its lock): a handle is
 * type << 24 | slot << 16 | serial; the slot's handler holds the handle while
 * the sound lives. Type 5 has its own table of 0x1DC-byte handlers, the
 * others a {.., count +8, stride +0xC, first +0x10} per type. Anything
 * unreadable counts as alive. */
static int scream_alive(uint32_t h)
{
    if (!s_snd) return 1;
    const uint32_t g = s_snd, type = (h >> 24) & 0x1Fu, slot = (h >> 16) & 0xFFu;
    if (!ea_ok(g - 0x8000u, 0x8000u)) return 1;
    uint32_t e;
    if (type == 5) e = g32(g - 0x7FE4u) + slot * 0x1DCu;
    else {
        const uint32_t t = g32(g - 0x7FFCu) + type * 4u;
        if (!ea_ok(t, 4)) return 1;
        const uint32_t p = g32(t);
        if (!p) return 0;
        if (!ea_ok(p, 0x14)) return 1;
        if ((int32_t)g32(p + 8) <= (int32_t)slot) return 0;
        e = slot * g32(p + 0xC) + g32(p + 0x10);
    }
    if (!e) return 0;
    return !ea_ok(e, 4) || g32(e) == h;
}

void ds_player_bind(uint32_t owner, uint32_t handle, uint32_t snd_globals)
{
    if (!owner || !handle) return;
    lock();
    s_snd = snd_globals;
    for (int i = 0; i < DS_SCRIPTS; i++) if (s_s[i].used && s_s[i].owner == owner && !s_s[i].handle) s_s[i].handle = handle;
    for (int j = 0; j < DS_VOICES; j++) if (s_v[j].used && s_v[j].owner == owner && !s_v[j].handle) s_v[j].handle = handle;
    unlock();
}

/* Every 20 ms: sounds whose game copy is gone (twice in a row -- the read is
 * not under SCREAM's lock) end. The game stops some sounds by paths no hook
 * sees: the Dynamo's rolling loop played on for 20 s+ after the ball was gone
 * (v63 test). */
static void check_handles(void)
{
    for (int i = 0; i < DS_SCRIPTS; i++) {
        ds_script* s = &s_s[i];
        if (!s->used || !s->handle) continue;
        if (scream_alive(s->handle)) { s->dead = 0; continue; }
        if (++s->dead < 2) continue;
        if (dbg()) { static int nl; if (nl++ < 300) fprintf(stderr, "[ds-dbg] game's sound gone: %s (handle %08X)%c", s->name, s->handle, 10); }
        script_end(s, 1);
    }
    for (int j = 0; j < DS_VOICES; j++) {
        ds_voice* v = &s_v[j];
        if (!v->used || v->fade > 0.0f) continue;
        /* A looping voice its script let go of, with no game handle to check:
         * 8 s, then it ends (nothing routed to the controller loops longer
         * on its own); any voice ends after 2 minutes (a missed stop). */
        if ((!v->handle && v->script < 0 && v->loop_fr >= 0 && v->age > 8u * DS_RATE) || v->age > 120u * DS_RATE) {
            if (dbg()) { static int nl; if (nl++ < 300) fprintf(stderr, "[ds-dbg] %s voice ended at %.1fs (no game handle)%c", v->name, v->age / (double)DS_RATE, 10); }
            voice_release(v);
            continue;
        }
        if (!v->handle) continue;
        if (scream_alive(v->handle)) { v->dead = 0; continue; }
        if (++v->dead < 2) continue;
        if (dbg()) { static int nl; if (nl++ < 300) fprintf(stderr, "[ds-dbg] game's sound gone: %s voice (%.1fs%s, %.2fs of data left) at %.2fs%c", v->name,
                                                            v->age / (double)DS_RATE, v->loop_fr >= 0 ? " loop" : "",
                                                            (v->nfr - v->fr) * 28.0 / DS_RATE / v->step, s_clock / (double)DS_RATE, 10); }
        voice_release(v);
    }
}

void ds_player_stop_all(void)
{
    lock();
    for (int i = 0; i < DS_SCRIPTS; i++) if (s_s[i].used) script_end(&s_s[i], 1);
    for (int j = 0; j < DS_VOICES; j++) if (s_v[j].used) voice_release(&s_v[j]);
    unlock();
}

void ds_player_render(float* speaker, float* felt, unsigned n)
{
    if (!s_on) return;
    lock();
    if ((s_check += n) >= DS_RATE / 50u) { s_check = 0; check_handles(); }
    pause_update();
    for (unsigned o = 0; o < n; o++) {
        s_tick_acc += (double)DS_TICK_HZ / DS_RATE;
        if (s_tick_acc >= 1.0) {
            s_tick_acc -= 1.0;
            for (int i = 0; i < DS_SCRIPTS; i++) if (s_s[i].used) script_tick(&s_s[i]);
        }
        float sp = 0.0f, fe = 0.0f;
        for (int i = 0; i < DS_VOICES; i++) {
            ds_voice* v = &s_v[i];
            if (!v->used) continue;
            if (v->delay) { v->delay--; continue; }
            float lg = 1.0f;
            if (v->line) {
                /* Paused with the game, or past what it has read (it read some, then stopped).
                 * A 20 ms fade each way; the line stays where it is. */
                const int hold = s_lines_paused || (v->read_fr && v->fr > v->read_fr + v->margin && v->fr < v->nfr);
                if (hold) {
                    v->gate -= 1.0f / (0.020f * DS_RATE);
                    if (v->gate <= 0.0f) {
                        v->gate = 0.0f;
                        /* Only a line the game stopped reading ends: a pause can last. */
                        if (!v->held && dbg()) fprintf(stderr, "[ds-dbg] %s holds at frame %u (read %u), %s, %.2f s%c", v->name, v->fr, v->read_fr,
                                                        s_lines_paused ? "paused" : "past what the game has read", s_clock / (double)DS_RATE, 10);
                        if (++v->held > 30u * DS_RATE && !s_lines_paused) { voice_free(v); }
                        continue;
                    }
                } else if (v->gate < 1.0f) {
                    if (v->held && dbg()) fprintf(stderr, "[ds-dbg] %s goes on, %.2f s%c", v->name, s_clock / (double)DS_RATE, 10);
                    v->gate += 1.0f / (0.020f * DS_RATE); if (v->gate > 1.0f) v->gate = 1.0f; v->held = 0;
                }
                lg = v->gate;
            }
            const float x = (v->s0 + (v->s1 - v->s0) * (float)v->phase) * v->env * lg;
            sp += x * v->spk; fe += x * v->felt;
            v->phase += v->step;
            while (v->phase >= 1.0) {
                v->phase -= 1.0;
                v->s0 = v->s1;
                if (v->ended || !next_sample(v, &v->s1)) {
                    if (v->ended) { voice_free(v); break; }
                    v->s1 = 0.0f; v->ended = 1;
                }
            }
            if (v->used && v->fade > 0.0f) { v->env -= v->fade; if (v->env <= 0.0f) voice_free(v); }
        }
        if (speaker) speaker[o] += sp;
        if (felt) felt[o] += fe;
    }
    for (int i = 0; i < DS_VOICES; i++) if (s_v[i].used && !(s_v[i].line && s_v[i].gate <= 0.0f)) s_v[i].age += n;
    s_clock += n;
    if (dbg()) {   /* every 10 s: what still plays (a stuck voice shows up here) */
        static uint32_t clk; clk += n;
        if (clk >= 10u * DS_RATE) { clk = 0;
            char b[600]; int o = snprintf(b, sizeof b, "[ds-dbg] playing:");
            for (int i = 0; i < DS_VOICES && o < 560; i++) if (s_v[i].used)
                o += snprintf(b + o, sizeof b - o, " %s(%.1fs%s%s)", s_v[i].name, s_v[i].age / (double)DS_RATE,
                              s_v[i].loop_fr >= 0 ? " loop" : "", s_v[i].spk > 0.0f ? "" : " felt");
            for (int i = 0; i < DS_SCRIPTS && o < 560; i++) if (s_s[i].used)
                o += snprintf(b + o, sizeof b - o, " [script %s pc %d/%d%s]", s_s[i].name, s_s[i].pc, s_s[i].ng, s_s[i].parked ? " parked" : "");
            fprintf(stderr, "%s%c", b, 10); } }
    unlock();
}
