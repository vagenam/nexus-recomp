/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Backend selection:
 *   - Windows default: WASAPI (via mmdeviceapi)
 *   - Everywhere else / if PS3RECOMP_AUDIO_USE_SDL2 defined: SDL2 audio
 *
 * Define PS3RECOMP_AUDIO_USE_SDL2 to force SDL2 backend on Windows.
 */

#include "../../runtime/memory/vm.h"   /* vm_commit */
#include "cellAudio.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read64, vm_write32 */
#include "audio_stretch.h"                  /* the output: no holes when the game is short */
#include "ds_player.h"                      /* the DualSense speaker's own player */
#include "../video/rsx_host_ui.h"           /* Settings: controller speaker on/off, volume */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

/* Event-queue push/lookup (runtime/syscalls/sys_event.c). Forward-declared
 * rather than including sys_event.h to avoid pulling PPU context/syscall
 * table types into the HLE audio module. */
extern int      sys_event_queue_push_by_id(uint32_t queue_id,
                                            uint64_t source, uint64_t data1,
                                            uint64_t data2,  uint64_t data3);
extern uint32_t sys_event_find_queue_by_key(uint64_t key);
extern uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size);

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_AUDIO_USE_SDL2)
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#elif defined(_WIN32)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  1
#else
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  /* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
  #ifdef __cplusplus
    #define GUID_SECT
  #else
    #define GUID_SECT
  #endif
  #ifndef DEFINE_AUDIO_GUID
    #ifdef INITGUID
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #else
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #endif
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);

  /* Map to the names used in the code */
  #define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
  #define IID_IAudioClient         ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    u64                  pub_index;      /* the position the game sees (audio_pub_tick) */
    u64                  write_index;    /* where the game is writing */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Timestamp anchor for cellAudioGetPortTimestamp (microseconds, set at init) */
static u64            s_audio_start_us = 0;
static volatile unsigned long g_audio_push_ok, g_audio_push_fail, g_audio_blocks_nz, g_audio_blocks;

/* Mixing thread */
static unsigned long long g_audio_dropped;   /* frames the device had no room for */
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
#ifndef _WIN32
/* Is s_mix_thread a thread that exists and has not been joined? The Win32
 * branch asks that of the HANDLE itself, which it nulls after closing;
 * pthread_t has no such value, so carry the flag. */
static int           s_mix_thread_live = 0;
#endif
static mutex_t       s_audio_mutex;

/* Output mix buffer (stereo, one block worth) */
volatile unsigned long long g_racacit_wav8_blocks;   /* AUDIO_WAV8 blocks written ([ds-line] "cap" time) */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 2];

/* The game's synth thread (func_00992418, syn_tick_timer_thread) writes while
 * its write position differs from the port's read index, and treats "equal" as
 * "ring full, nothing to do": it then sleeps a 60 Hz tick. So the read index has
 * to keep moving, in real time, as on the PS3. v89-v96 held it when the next
 * block was not finished, to wait for the game; the game saw "equal" and waited
 * for the reader, until the device queue ran out. Then 8 blocks of fade and a
 * burst of silent reads to refill the queue knocked the two out of step again:
 * (32 - 2) x 5.33 ms of hold + 43 ms = the exact 203 ms silences of a tester's
 * Ryzen 5 2600 (70% of the time silent, for good, once a fight had grown the
 * queue to its 32 blocks). Now:
 *   - pub_index, the index the game sees, and the period events run on a clock
 *     (audio_pub_tick), never held and never in a burst;
 *   - read_index is the playback cursor: it plays each block the game finished,
 *     in order, and just waits for the next;
 *   - what it reads goes through a time stretch (audio_stretch.h) to the device.
 * A PC that cannot make 187.5 blocks a second has less sound than time. Silence
 * for the difference was the crackle left in v98: 5 ms holes, one block each, a
 * dozen a second at 15 fps (GTX 1650 tester's videos). Now the sound plays longer,
 * same pitch, as far as it must; at full supply it is untouched, sample for sample.
 * A port the game does not fill by SPU PUT (no s_port_done_seen) reads on as it
 * always did, with pub_index = read_index. */
#define AUDIO_DEV_BLOCKS 4u         /* in the device's queue: 21 ms */
static u32 s_q_target = 6;          /* blocks wanted in the stretcher beyond its frame: 6 (32 ms), +2 per dropout with sound, up to 24 (128 ms) */
static volatile unsigned s_finished[CELL_AUDIO_PORT_MAX];   /* blocks the game has finished, ever */
static unsigned s_pub_steps;        /* for the 30 s report */
static int s_pub_mode;              /* the above is on (a device is open; not RACACIT_AUDIO_NOWAIT) */
static u8  s_port_sound[CELL_AUDIO_PORT_MAX];   /* the last block read from the port held sound */
static volatile u8 s_blk_done[CELL_AUDIO_PORT_MAX][32];   /* the game finished writing the block */
static volatile u8 s_port_done_seen[CELL_AUDIO_PORT_MAX];
static float s_last_out[2];         /* the last frame sent to the device */
static int   s_splice;              /* the next block does not follow the last one (a jump) */
static unsigned s_late_skips, s_late_fades;   /* for the 30 s report */

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

#if AUDIO_BACKEND_SDL2

static SDL_AudioDeviceID s_sdl_audio_dev = 0;

/* ---- DualSense speaker (USB) ---------------------------------------------
 * A DualSense on USB is also a 4-channel audio device ("Speakers (Wireless
 * Controller)"): channels 1-2 feed the headset jack / built-in speaker,
 * 3-4 the haptic actuators. cellPad switches the controller's audio path to
 * the speaker (ds5_speaker_route, cellPad.c). What plays there is
 * s_ds_buffer, mono, filled each block by the controller's own player
 * (ds_player.c; the game picks the sounds -- game/stubs.cpp, ppu_fs.cpp).
 * Settings menu / settings.txt: RACACIT_DS_SPEAKER (on/off) and
 * RACACIT_DS_SPK_VOL (percent, default 49), applied live after the limiter
 * on a loudness curve (50% sounds about half as loud);
 * RACACIT_DS_SPEAKER_GAIN scales the drive into the limiter. RACACIT_DS_DEVICE=0 never
 * opens the device (agent test runs). Its own clock: a block that does not
 * fit is dropped, never waited for. */
static SDL_AudioDeviceID s_ds_dev = 0;
static int               s_ds_nch = 4;
static float             s_ds_buffer[CELL_AUDIO_BLOCK_SAMPLES];
static float             s_ds_hbuffer[CELL_AUDIO_BLOCK_SAMPLES];   /* SR: the felt sounds (v57 haptics) */
volatile int             g_racacit_ds_haptics;   /* audio haptics on: cellPad.c sends no rumble, it comes here */
static int               s_ds_haptic_dev;        /* the controller's audio has the actuator channels (3-4) */
volatile int             g_racacit_rumble_lg, g_racacit_rumble_sm;   /* the game's motors (cellPadSetActDirect) */
volatile unsigned long long g_racacit_rumble_ms;                     /* ... when last set (SDL ticks) */
static float             s_ds_gain = 2.0f;   /* at 100% */
static FILE*             s_ds_raw;           /* RACACIT_DS_RAW: the player's speaker output, f32 mono (tests) */

static void ds_audio_open(void)
{
    /* RACACIT_DS_RAW=<file>: the player runs and its speaker output is written
     * there (raw f32 mono, 48 kHz, one block per mixed block -- next to
     * AUDIO_WAV8's TV channels), with or without a controller. */
    { const char* e = getenv("RACACIT_DS_RAW");
      if (e && *e && (s_ds_raw = fopen(e, "wb")) != NULL) {
          ds_player_enable(1);
          fprintf(stderr, "[cellAudio] DualSense player capture: %s%c", e, 10); } }
    { const char* e = getenv("RACACIT_DS_DEVICE"); if (e && *e == '0') return; }
    {   /* Speaker and haptics are the DualSense Plus mod's (rsx_host_ui.h
         * ds_plus): without it the controller's audio device is left alone. */
        rsx_host_options o; rsx_host_ui_options(&o);
        if (!o.ds_plus) { fprintf(stderr, "[cellAudio] DualSense speaker and haptics: off (the DualSense Plus mod is not on)%c", 10); return; }
    }
    const int n = SDL_GetNumAudioDevices(0);
    const char* pick = NULL;
    for (int i = 0; i < n; i++) {
        const char* nm = SDL_GetAudioDeviceName(i, 0);
        if (!nm) continue;
        fprintf(stderr, "[cellAudio] output device %d: %s%c", i, nm, 10);
        if (!pick && strstr(nm, "Wireless Controller")) pick = nm;
    }
    if (!pick) {
        fprintf(stderr, "[cellAudio] DualSense speaker: no controller audio device (USB only)%c", 10);
        return;
    }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = CELL_AUDIO_SAMPLE_RATE; want.format = AUDIO_F32SYS;
    want.channels = 4; want.samples = CELL_AUDIO_BLOCK_SAMPLES;
    s_ds_dev = SDL_OpenAudioDevice(pick, 0, &want, &have, SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    if (!s_ds_dev) { fprintf(stderr, "[cellAudio] DualSense speaker: open failed: %s%c", SDL_GetError(), 10); return; }
    s_ds_nch = have.channels ? have.channels : 2;
    if (s_ds_nch > 8) { SDL_CloseAudioDevice(s_ds_dev); s_ds_dev = 0; return; }
    { const char* e = getenv("RACACIT_DS_SPEAKER_GAIN"); if (e) s_ds_gain = (float)atof(e); }
    ds_player_enable(1);
    /* Audio haptics (v57), on with the controller's audio unless
     * RACACIT_DS_HAPTICS=0: channels 3-4 drive the actuators. The game's
     * rumble is played there as tones (ds_audio_submit) and no longer sent
     * as rumble -- the DualSense's rumble emulation turns audio haptics off
     * while it runs. */
    s_ds_haptic_dev = s_ds_nch >= 4;
    { rsx_host_options o; rsx_host_ui_options(&o); g_racacit_ds_haptics = s_ds_haptic_dev && o.ds_haptics; }
    SDL_PauseAudioDevice(s_ds_dev, 0);
    { rsx_host_options o; rsx_host_ui_options(&o);
      fprintf(stderr, "[cellAudio] DualSense speaker: \"%s\" open, %d channels, %d Hz, %u-sample buffer; speaker %s, volume %d%%%c",
              pick, s_ds_nch, have.freq, (unsigned)have.samples, o.ds_speaker ? "on" : "off", o.ds_volume, 10); }
    fprintf(stderr, "[cellAudio] DualSense haptics: %s%c", g_racacit_ds_haptics ? "on (weapons, impacts, explosions and the game's rumble on the actuators; Settings > Controller)" : "off (rumble)", 10);
}

/* One biquad (RBJ cookbook), direct form 1. */
typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; } ds_biquad;
static void ds_biquad_set(ds_biquad* q, int highpass, float hz)
{
    const float w = 2.0f * 3.14159265f * hz / (float)CELL_AUDIO_SAMPLE_RATE, c = cosf(w), al = sinf(w) / (2.0f * 0.7071f);
    const float a0 = 1.0f + al;
    q->b0 = (highpass ? (1.0f + c) : (1.0f - c)) / 2.0f / a0;
    q->b1 = (highpass ? -(1.0f + c) : (1.0f - c)) / a0;
    q->b2 = q->b0;
    q->a1 = -2.0f * c / a0; q->a2 = (1.0f - al) / a0;
    q->x1 = q->x2 = q->y1 = q->y2 = 0.0f;
}
static float ds_biquad_run(ds_biquad* q, float x)
{
    const float y = q->b0 * x + q->b1 * q->x1 + q->b2 * q->x2 - q->a1 * q->y1 - q->a2 * q->y2;
    q->x2 = q->x1; q->x1 = x; q->y2 = q->y1; q->y1 = y;
    return y;
}
static float ds_soft(float v) { return v / (1.0f + fabsf(v)); }   /* soft limit, ~linear when quiet */

/* The speaker is tiny: the centre's bass (a gunshot's thump is most of its
 * energy) only distorts it, so it is high-passed at RACACIT_DS_SPEAKER_HZ
 * (default 200), then peak-limited to 0.9: the gain drops at once to hold a
 * peak and recovers over ~80 ms, so a shot keeps its shape -- v42-v47's
 * v/(1+|v|) curve halved every peak and bent the waveform (a shot at 1.0
 * came out at 0.5, distorted). RACACIT_DS_HAPTICS=1 sends that low band
 * to the haptic actuators (channels 3-4) instead -- audio-driven haptics, the
 * way Rift Apart's are made; off by default (it shares the actuators with the
 * game's rumble). */
static void ds_audio_submit(void)
{
    if (!ds_player_enabled()) return;
    ds_player_render(s_ds_buffer, s_ds_hbuffer, CELL_AUDIO_BLOCK_SAMPLES);
    if (s_ds_raw) fwrite(s_ds_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES, s_ds_raw);
    if (!s_ds_dev) return;
    /* Settings, live (F1 menu or settings.txt): the speaker's on/off and
     * volume; haptic feedback on/off and its strength (100% = v57-v64's
     * fixed level, RACACIT_DS_HAPTIC_GAIN); the game's rumble strength (its
     * tones on the actuators here, the motors in cellPad.c with haptics off). */
    float vol, hscale, rscale; int haptics;
    { rsx_host_options o; rsx_host_ui_options(&o);
      /* The volume scales what the limiter lets through, on a loudness
       * curve: before it (v42-v66) most sounds already sat in the limiter
       * at any setting above ~20%, so the slider "seemingly does nothing"
       * and everything stayed "far too loud" (owner, v66). The drive into
       * the limiter is fixed (RACACIT_DS_SPEAKER_GAIN, half of it). */
      vol = o.ds_speaker ? powf((float)o.ds_volume / 100.0f, 1.66f) : 0.0f;
      haptics = s_ds_haptic_dev && o.ds_haptics;
      hscale = (float)o.ds_haptic_vol / 100.0f;
      rscale = (float)o.ds_rumble / 100.0f; }
    g_racacit_ds_haptics = haptics;   /* cellPad.c: rumble to the motors when off */
    static ds_biquad hp, lp, lp2, hhp; static int init; static float env, rel, hgain;
    if (!init) {
        init = 1;
        const char* e = getenv("RACACIT_DS_SPEAKER_HZ"); const float hz = e ? (float)atof(e) : 200.0f;
        ds_biquad_set(&hp, 1, hz > 20.0f ? hz : 200.0f);
        ds_biquad_set(&lp, 0, 300.0f); ds_biquad_set(&lp2, 0, 300.0f);   /* the actuators' band: ~20-300 Hz */
        ds_biquad_set(&hhp, 1, 20.0f);
        e = getenv("RACACIT_DS_HAPTIC_GAIN"); hgain = e ? (float)atof(e) : 3.0f;
        rel = expf(-1.0f / (0.080f * (float)CELL_AUDIO_SAMPLE_RATE));
    }
    /* The game's rumble as tones (v57): the large motor a 40 Hz thump, the
     * small one a 150 Hz buzz, at the motor's strength; a setting older than
     * 150 ms counts as off (the game resends while it rumbles). */
    static double ph_lg, ph_sm;
    float r_lg = 0.0f, r_sm = 0.0f;
    if (haptics && SDL_GetTicks64() - g_racacit_rumble_ms < 150) {
        r_lg = 0.8f * (float)g_racacit_rumble_lg / 255.0f; r_sm = g_racacit_rumble_sm ? 0.35f : 0.0f;
    }
    const float ceil_ = 0.9f;
    /* v (speaker) and h (haptics) per input sample; [0] is the previous
     * block's last sample, for the resampler below. */
    static float v[CELL_AUDIO_BLOCK_SAMPLES + 1], hv[CELL_AUDIO_BLOCK_SAMPLES + 1];
    v[0] = v[CELL_AUDIO_BLOCK_SAMPLES]; hv[0] = hv[CELL_AUDIO_BLOCK_SAMPLES];
    float in_peak = 0.0f, out_peak = 0.0f, min_gain = 1.0f, hap_peak = 0.0f;
    for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
        const float x = s_ds_buffer[s] * s_ds_gain * 0.5f;
        const float y = ds_biquad_run(&hp, x), a = fabsf(y);
        env = a > env ? a : env * rel;
        const float g = env > ceil_ ? ceil_ / env : 1.0f;
        v[s + 1] = y * g * vol;
        if (fabsf(x) > in_peak) in_peak = fabsf(x);
        if (fabsf(v[s + 1]) > out_peak) out_peak = fabsf(v[s + 1]);
        if (g < min_gain) min_gain = g;
        if (haptics) {
            const float hx = ds_biquad_run(&lp2, ds_biquad_run(&lp, ds_biquad_run(&hhp, s_ds_hbuffer[s]))) * hgain;
            ph_lg += 40.0 / CELL_AUDIO_SAMPLE_RATE; ph_sm += 150.0 / CELL_AUDIO_SAMPLE_RATE;
            if (ph_lg >= 1.0) ph_lg -= 1.0; if (ph_sm >= 1.0) ph_sm -= 1.0;
            const float rx = r_lg * sinf(6.2831853f * (float)ph_lg) + r_sm * sinf(6.2831853f * (float)ph_sm);
            /* The strengths scale what the limiter lets through: a shot is
             * loud enough to sit in its knee, where a gain before it changed
             * almost nothing (the actuators are audible -- "gun volume is
             * not affected and far too loud", v64). */
            float hh = 0.95f * (hscale * ds_soft(1.5f * hx) + rscale * ds_soft(1.5f * rx));
            if (hh > 0.95f) hh = 0.95f; if (hh < -0.95f) hh = -0.95f;
            hv[s + 1] = hh;
            if (fabsf(hv[s + 1]) > hap_peak) hap_peak = fabsf(hv[s + 1]);
        } else hv[s + 1] = 0.0f;
    }
    const Uint32 frame = (Uint32)(s_ds_nch * sizeof(float));
    const Uint32 queued = SDL_GetQueuedAudioSize(s_ds_dev) / frame;
    /* The controller plays on its own clock, the game's audio follows the
     * main output's: a few percent apart. v42-v54 dropped a whole block when
     * more than 6 were queued -- the owner's v54 session dropped 12222 blocks
     * in 36 min (~210 per 30 s, a gap every 150 ms) once the two clocks drifted
     * apart. Now the block is resampled (linear) at a rate nudged by the queue
     * depth, keeping ~3.5 blocks queued; only a runaway queue still drops. */
    static double phase = 1.0;                       /* read position in v[] */
    static double drift;                             /* learned clock ratio - 1 (slow integral) */
    const double qb = (double)queued / CELL_AUDIO_BLOCK_SAMPLES, err = qb - 3.5;
    drift += 2e-5 * err;
    if (drift < -0.05) drift = -0.05; if (drift > 0.05) drift = 0.05;
    double step = 1.0 + drift + 0.008 * err;
    if (step < 0.94) step = 0.94; if (step > 1.06) step = 1.06;
    float o[(CELL_AUDIO_BLOCK_SAMPLES * 107 / 100 + 4) * 8];
    u32 n_out = 0;
    while (phase <= (double)CELL_AUDIO_BLOCK_SAMPLES && n_out < CELL_AUDIO_BLOCK_SAMPLES * 107 / 100 + 2) {
        const u32 i = (u32)phase; const float f = (float)(phase - i);
        const float a = i < CELL_AUDIO_BLOCK_SAMPLES ? v[i] + (v[i + 1] - v[i]) * f : v[i];
        float* d = o + n_out * s_ds_nch;
        for (int c = 0; c < s_ds_nch; c++) d[c] = 0.0f;
        d[0] = a;                                    /* both of the jack's channels: the speaker takes R */
        if (s_ds_nch > 1) d[1] = a;
        if (haptics && s_ds_nch >= 4) {
            const float hh = i < CELL_AUDIO_BLOCK_SAMPLES ? hv[i] + (hv[i + 1] - hv[i]) * f : hv[i];
            d[2] = hh; d[3] = hh;
        }
        n_out++; phase += step;
    }
    phase -= (double)CELL_AUDIO_BLOCK_SAMPLES;
    const int drop = queued > CELL_AUDIO_BLOCK_SAMPLES * 16;   /* runaway only (~85 ms ahead) */
    /* What reached the speaker, every 30 s with sound, in log.txt: the
     * share of blocks that carried any, the loudest sample out (1.0 = full,
     * after the volume) and in (before the filter and limiter), how often
     * the limiter cut more than 3 dB, blocks dropped (queue full; "with
     * sound" = audible losses), times the queue had run dry, and the queue
     * depth range in blocks. */
    static unsigned rep_blocks, rep_loud, rep_lines, rep_drop, rep_drop_loud, rep_dry, rep_lim;
    static float rep_peak, rep_in;
    static Uint32 rep_qmin = 0xFFFFFFFFu, rep_qmax;
    const int loud = out_peak > 0.003f;
    if (loud) rep_loud++;
    if (min_gain < 0.708f) rep_lim++;
    static unsigned rep_hap; static float rep_hpeak;
    if (hap_peak > 0.02f) rep_hap++;
    if (hap_peak > rep_hpeak) rep_hpeak = hap_peak;
    if (out_peak > rep_peak) rep_peak = out_peak;
    if (in_peak > rep_in) rep_in = in_peak;
    if (drop) { rep_drop++; if (loud) rep_drop_loud++; }
    if (queued == 0) rep_dry++;
    if (queued < rep_qmin) rep_qmin = queued;
    if (queued > rep_qmax) rep_qmax = queued;
    if (++rep_blocks >= 30u * CELL_AUDIO_SAMPLE_RATE / CELL_AUDIO_BLOCK_SAMPLES) {
        static int dbg = -1; if (dbg < 0) { const char* e = getenv("RACACIT_DS_DEBUG"); dbg = e && *e == '1'; }
        if ((rep_loud || rep_hap || rep_drop || dbg) && rep_lines < 200) { rep_lines++;
            fprintf(stderr, "[cellAudio] DualSense speaker: sound in %u%% of the last 30 s, peak %.2f (in %.2f, limited in %u blocks); dropped %u blocks (%u with sound), ran dry %u, queue %.1f-%.1f blocks, clock %+.2f%%; haptics in %u%%, peak %.2f%c",
                    rep_loud * 100u / rep_blocks, rep_peak, rep_in, rep_lim, rep_drop, rep_drop_loud, rep_dry,
                    rep_qmin / (double)CELL_AUDIO_BLOCK_SAMPLES, rep_qmax / (double)CELL_AUDIO_BLOCK_SAMPLES, drift * 100.0,
                    rep_hap * 100u / rep_blocks, rep_hpeak, 10); }
        rep_hap = 0; rep_hpeak = 0.0f;
        rep_blocks = rep_loud = rep_drop = rep_drop_loud = rep_dry = rep_lim = 0; rep_peak = rep_in = 0.0f;
        rep_qmin = 0xFFFFFFFFu; rep_qmax = 0;
    }
    if (drop) return;
    SDL_QueueAudio(s_ds_dev, o, n_out * frame);
}

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = CELL_AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = CELL_AUDIO_BLOCK_SAMPLES;
    want.callback = NULL; /* We'll use SDL_QueueAudio */

    s_sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_sdl_audio_dev == 0) {
        printf("[cellAudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(s_sdl_audio_dev, 0); /* Start playback */
    fprintf(stderr, "[cellAudio] output: %d Hz, %d channels, %u-sample buffer%c", have.freq, have.channels, (unsigned)have.samples, 10);
    ds_audio_open();
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_sdl_audio_dev) {
        SDL_CloseAudioDevice(s_sdl_audio_dev);
        s_sdl_audio_dev = 0;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    ds_audio_submit();   /* the controller's player (and its test capture) */
    if (s_sdl_audio_dev) {
        SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                       num_samples * 2 * sizeof(float));
    }
}

/* SDL queues without bound: keep AUDIO_DEV_BLOCKS ahead of the device. */
static int audio_backend_room(void)
{
    if (!s_sdl_audio_dev) return -1;
    return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float)) <
           CELL_AUDIO_BLOCK_SAMPLES * AUDIO_DEV_BLOCKS ? CELL_AUDIO_BLOCK_SAMPLES : 0;
}

static void audio_backend_wait(void) { SDL_Delay(1); }

static u32 audio_backend_queued_samples(void)
{
    if (s_sdl_audio_dev) {
        return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float));
    }
    return 0;
}

static audio_stretch s_st;

/* What the stretcher holds in blocks, beyond the frame it needs in hand. */
static double audio_st_blocks(void)
{
    const double d = ast_level(&s_st) - AST_FRAME;
    return d > 0.0 ? d / CELL_AUDIO_BLOCK_SAMPLES : 0.0;
}

#endif /* AUDIO_BACKEND_SDL2 */

#if AUDIO_BACKEND_WASAPI

static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;

static int audio_backend_init(void)
{
    HRESULT hr;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
        return -1;
    }

    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender,
                                                      eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }

    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void**)&s_wasapi_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to activate audio client: 0x%08lX\n", hr);
        return -1;
    }

    WAVEFORMATEX wfx;
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* Request event-driven mode with a ~20ms buffer */
    REFERENCE_TIME buf_duration = 200000; /* 20ms in 100ns units */
    s_wasapi_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    hr = s_wasapi_client->lpVtbl->Initialize(
        s_wasapi_client,
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        buf_duration, 0, &wfx, NULL);

    if (FAILED(hr)) {
        /* Fallback: try without event callback */
        hr = s_wasapi_client->lpVtbl->Initialize(
            s_wasapi_client,
            AUDCLNT_SHAREMODE_SHARED,
            0, buf_duration, 0, &wfx, NULL);
        if (FAILED(hr)) {
            printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
            s_wasapi_client->lpVtbl->Release(s_wasapi_client);
            s_wasapi_client = NULL;
            CloseHandle(s_wasapi_event);
            s_wasapi_event = NULL;
            return -1;
        }
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    } else {
        s_wasapi_client->lpVtbl->SetEventHandle(s_wasapi_client, s_wasapi_event);
    }

    s_wasapi_client->lpVtbl->GetBufferSize(s_wasapi_client, &s_wasapi_buf_frames);

    hr = s_wasapi_client->lpVtbl->GetService(
        s_wasapi_client, &IID_IAudioRenderClient, (void**)&s_wasapi_render);
    if (FAILED(hr)) {
        printf("[cellAudio] Failed to get render client: 0x%08lX\n", hr);
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
        return -1;
    }

    s_wasapi_client->lpVtbl->Start(s_wasapi_client);
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    }
    if (s_wasapi_render) {
        s_wasapi_render->lpVtbl->Release(s_wasapi_render);
        s_wasapi_render = NULL;
    }
    if (s_wasapi_client) {
        s_wasapi_client->lpVtbl->Release(s_wasapi_client);
        s_wasapi_client = NULL;
    }
    if (s_wasapi_event) {
        CloseHandle(s_wasapi_event);
        s_wasapi_event = NULL;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (!s_wasapi_render) return;

    UINT32 padding = 0;
    s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available) {
        g_audio_dropped += num_samples - available;
        num_samples = available;
    }
    if (num_samples == 0) return;

    BYTE* buf = NULL;
    HRESULT hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        memcpy(buf, stereo_samples, num_samples * 2 * sizeof(float));
        s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples, 0);
    }
}

/* Frames the device can take right now; -1 with no device open. */
static int audio_backend_room(void)
{
    if (!s_wasapi_client) return -1;
    UINT32 padding = 0;
    const HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr)) {
        /* Device gone (AUDCLNT_E_DEVICE_INVALIDATED on a default-device
         * change, sleep, ...): pace on the clock rather than spin. */
        static int _n = 0; if ((_n < 4 && (_n++, 1)))
            fprintf(stderr, "[cellAudio] GetCurrentPadding failed 0x%08lX -- clock pacing%c", (unsigned long)hr, 10);
        return -1;
    }
    return (int)(s_wasapi_buf_frames - padding);
}

/* Block until the device has consumed some of its buffer. */
static void audio_backend_wait(void)
{
    if (s_wasapi_event) WaitForSingleObject(s_wasapi_event, 10);
    else Sleep(1);
}


#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Guest PCM is BIG-ENDIAN float32; the port buffer is a raw vm_base view, so
 * every sample must be byte-swapped before use. Reading it as a host float
 * turned the whole mix into denormal noise -- silence after clipping (LBP
 * had a verified end-to-end pipeline with no audible output). */
static inline float ld_be_f32(const float* p)
{
    u32 v;
    memcpy(&v, p, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* Host monotonic microseconds -- the same epoch as sys_time_get_system_time
 * and the PPU timebase (CLOCK_MONOTONIC / QPC from the guest clock origin,
 * sys_timer.c). */
#ifdef _WIN32
extern uint64_t ps3_guest_clock_epoch_qpc(void);
#else
extern uint64_t ps3_guest_clock_epoch_ns(void);
#endif
static u64 audio_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    const u64 q = (u64)c.QuadPart - ps3_guest_clock_epoch_qpc();
    return (q / (u64)f.QuadPart) * 1000000ULL +
           (q % (u64)f.QuadPart) * 1000000ULL / (u64)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec - ps3_guest_clock_epoch_ns()) / 1000ULL;
#endif
}

/* Mix one block from all active ports into s_mix_buffer (stereo float) */
static void audio_mix_one_block(void)
{
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));
#if AUDIO_BACKEND_SDL2
    memset(s_ds_buffer, 0, sizeof(s_ds_buffer));
    memset(s_ds_hbuffer, 0, sizeof(s_ds_hbuffer));
#endif

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        /* Diagnostic: a port that HOLDS PCM but was never started is inaudible
         * by design -- flag it once so "no PortStart" vs "no data" is decidable
         * from the log (LBP's Bink movie audio port sat exactly there). */
        if (port->in_use && !port->running && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                  port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) if (port->buffer[s] != 0.0f) { nz = 1; break; }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level <= 0.0f) level = 1.0f;
        if (level > 1.0f) level = 1.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        const int by_put = s_pub_mode && s_port_done_seen[p] && block_idx < 32;
        if (by_put && !s_blk_done[p][block_idx]) continue;   /* the game has not finished it: nothing from this port */
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;

        /* AUDIO_PEAK diag: which blocks of this port's WHOLE ring hold data,
         * vs which block the read cursor is on -- distinguishes "writer never
         * writes" from "writer and reader cycle out of phase". */
        /* AUDIO_PEAK=1: a ring map every 400 blocks. AUDIO_PEAK=<n> (n > 1):
         * a map for EVERY block from mix #n for 96 blocks, with the host time,
         * so the writer's position relative to the read cursor is visible. */
        { static long s_pk = -1; if (s_pk < 0) { const char* e = getenv("AUDIO_PEAK"); s_pk = e ? atol(e) : 0; }
          if (s_pk) { static _Thread_local unsigned _c; ++_c;
            static long s_pkn = -1; if (s_pkn < 0) { const char* e2 = getenv("AUDIO_PEAK_N"); s_pkn = e2 ? atol(e2) : 96; }
            if (s_pk > 1 ? (_c >= (unsigned long)s_pk && _c < (unsigned long)(s_pk + s_pkn)) : (_c % 400) == 0) {
                char map[36]; u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16)
                        if (bp[s2] != 0.0f) { nz = 1; break; }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] t=%llu port %d ridx=%llu blk=%u ring=[%s]\n",
                        (unsigned long long)audio_now_us(), p, (unsigned long long)port->read_index, block_idx, map);
            } } }

        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++) {
            float left, right;
            if (nch >= 2) {
                left  = ld_be_f32(&src[s * nch + 0]) * level;
                right = ld_be_f32(&src[s * nch + 1]) * level;
            } else {
                /* Mono: duplicate to both channels */
                left = right = ld_be_f32(&src[s]) * level;
            }

            /* If 7.1, mix center and other channels into stereo */
            if (nch == 8) {
                float center = ld_be_f32(&src[s * 8 + 2]) * level * 0.707f;
                float lfe    = ld_be_f32(&src[s * 8 + 3]) * level * 0.5f;
                float rl     = ld_be_f32(&src[s * 8 + 4]) * level * 0.5f;
                float rr     = ld_be_f32(&src[s * 8 + 5]) * level * 0.5f;
                float sl     = ld_be_f32(&src[s * 8 + 6]) * level * 0.3f;
                float sr     = ld_be_f32(&src[s * 8 + 7]) * level * 0.3f;
                left  += center + lfe + rl + sl;
                right += center + lfe + rr + sr;
            }

            s_mix_buffer[s * 2 + 0] += left;
            s_mix_buffer[s * 2 + 1] += right;
        }

        /* AUDIO_WAV8=<file>: port 0 as the game wrote it, raw f32le, all its
         * channels (7.1: L R C LFE RL RR SL SR) -- which sounds sit where. */
        if (p == 0) { static FILE* _w8 = (FILE*)-1;
          if (_w8 == (FILE*)-1) { const char* e = getenv("AUDIO_WAV8"); _w8 = e ? fopen(e, "wb") : NULL; }
          if (_w8) { float t[CELL_AUDIO_BLOCK_SAMPLES * 8];
            const u32 n = CELL_AUDIO_BLOCK_SAMPLES * (nch > 8 ? 8 : nch);
            for (u32 s2 = 0; s2 < n; s2++) t[s2] = ld_be_f32(&src[s2]);
            fwrite(t, sizeof(float), n, _w8); g_racacit_wav8_blocks++; } }

        /* Firmware clears a ring-buffer block after consuming it. Besides
         * preventing stale PCM from replaying when a producer stalls, the
         * cleared block is an observable consumption signal for producers. */
        {
            int nz = 0;
            for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 7)
                if (src[s2] != 0.0f) { nz = 1; break; }
            s_port_sound[p] = (u8)nz;
            if (p == 0) { g_audio_blocks++; g_audio_blocks_nz += nz; }
        }
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));
        if (block_idx < 32) s_blk_done[p][block_idx] = 0;

        /* Keep the monotonic counter for tags/timestamps, but publish the
         * ring slot to the guest. SurMixer multiplies this value by the block
         * size directly; an unbounded counter walks beyond the audio buffer. */
        port->read_index++;
        if (!by_put) {
            port->pub_index = port->read_index;
            if (port->read_idx_addr)
                vm_write64((u32)port->read_idx_addr, port->pub_index % nblock);
        }
    }

    mutex_unlock(&s_audio_mutex);

    /* Clip to [-1.0, 1.0] */
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
        if (s_mix_buffer[i] > 1.0f) s_mix_buffer[i] = 1.0f;
        if (s_mix_buffer[i] < -1.0f) s_mix_buffer[i] = -1.0f;
    }

    /* After a jump in the game's ring, the new block does not
     * continue the last sample played: glide from that sample into it over
     * 2 ms instead of a step (a click). */
    if (s_splice) {
        for (u32 i = 0; i < 96; i++) {
            const float g = (float)i / 96.0f;
            s_mix_buffer[i * 2]     = s_mix_buffer[i * 2] * g     + s_last_out[0] * (1.0f - g);
            s_mix_buffer[i * 2 + 1] = s_mix_buffer[i * 2 + 1] * g + s_last_out[1] * (1.0f - g);
        }
        s_splice = 0;
    }
    s_last_out[0] = s_mix_buffer[(CELL_AUDIO_BLOCK_SAMPLES - 1) * 2];
    s_last_out[1] = s_mix_buffer[(CELL_AUDIO_BLOCK_SAMPLES - 1) * 2 + 1];
}

/* The game writes each port block in four SPU PUTs: three scratch passes of
 * its synth buses (planar, not audio yet), then the finished interleaved block
 * from LS 0x12000. Reading a block between them played the scratch data --
 * clicks at 32-sample steps when the game ran late. Once a port has had such a
 * final PUT, a block counts as written only after its own; a port the game
 * fills some other way falls back to "not empty". */
static void audio_put_hook(uint32_t ea, uint32_t lsa, uint32_t size)
{
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        const AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->port_addr) continue;
        const u32 base = (u32)port->port_addr, bb = CELL_AUDIO_BLOCK_SAMPLES * (u32)port->param.nChannel * 4u;
        if (ea < base || ea >= base + port->buf_size || size != bb) continue;
        const u32 blk = (ea - base) / bb;
        if (blk < 32) {
            s_finished[p]++;
#ifdef _WIN32
            /* The thread that runs the game's audio job is its SPU, real-time on
             * the PS3. At normal priority, behind the game's boosted PPU threads,
             * it lost whole time slices (125-185 blocks a second of 187.5 under
             * load). HIGHEST was worse on 6 threads: it starved the work the job
             * itself waits on (20-37 fps against 33-47, fewer blocks). Above
             * normal, level with those threads. RACACIT_AUDIO_BOOST=0/1/2. */
            { static int s_b; if (!s_b) { s_b = 1; const char* e = getenv("RACACIT_AUDIO_BOOST"); const int lv = e ? atoi(e) : 1;
                if (lv) SetThreadPriority(GetCurrentThread(), lv >= 2 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL);
                fprintf(stderr, "[cellAudio] audio writer thread %lu, boost %d%c", (unsigned long)GetCurrentThreadId(), lv, 10); } }
#endif
            s_blk_done[p][blk] = 1; s_port_done_seen[p] = 1;
            static int _first = 0;
            if (_first < 10) {
                _first++;
                fprintf(stderr, "[cellAudio] audio_put_hook hit: port=%d blk=%u lsa=0x%X size=%u (total finished=%u)\n",
                        p, blk, lsa, size, s_finished[p]);
            }
        }
        return;
    }
}

/* Before reading: is there anything to play from the ports the game fills by
 * PUT? Read blocks are cleared, so a finished block is an unread one, and the
 * game writes in order: the first finished block after the cursor is the next
 * to play (the cursor has to move only when a port starts, or if the writer
 * lapped it). *skipped = blocks passed. Late = a port has none finished, and no
 * other port wants mixing. *paced = nothing here sets the pace (a port not filled
 * by PUT, or no port running): mix to keep the output fed. */
#if AUDIO_BACKEND_SDL2
static int audio_game_late(u32* skipped, int* paced)
{
    *skipped = 0;
    int late = 0, other = 0, put = 0;
    mutex_lock(&s_audio_mutex);
    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        if (!port->in_use || !port->running || !port->buffer) continue;
        if (!s_port_done_seen[p]) { other = 1; continue; }
        put = 1;
        const u32 nblock = (u32)port->param.nBlock;
        const u32 cur = (u32)(port->read_index % nblock);
        if (cur >= 32 || s_blk_done[p][cur]) continue;
        u32 d = 1;
        while (d < nblock && !s_blk_done[p][(cur + d) % nblock]) d++;
        if (d == nblock) { late = 1; continue; }
        port->read_index += d;
        if (d > *skipped) *skipped = d;
        s_splice = 1;
    }
    mutex_unlock(&s_audio_mutex);
    *paced = other || !put;
    return late && !other;
}

/* Everything the game has finished goes to the stretcher at once. */
static void audio_feed_stretcher(void)
{
    for (int n = 0; n < 16; n++) {
        u32 skipped = 0; int paced = 0;
        const int late = audio_game_late(&skipped, &paced);
        if (skipped) s_late_skips++;
        if (late || (paced && audio_st_blocks() >= (double)s_q_target)) return;
        audio_mix_one_block();
        ds_audio_submit();
        ast_push(&s_st, s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
    }
}
#endif

/* Every 30 s, if the sound was ever short: how much was stretched, how often it
 * stopped altogether, and what the game made of the 187.5 blocks a second it
 * should finish (under that = the CPU is short). A quiet minute brings the queue
 * back down: one rough moment (a level load) must not leave the latency for good. */
static void audio_late_report(void)
{
#if AUDIO_BACKEND_SDL2
    static u64 t0; static unsigned f0, quiet; static unsigned long long st0, n0, fades0;
    const u64 t = audio_now_us();
    if (s_st.fades != fades0) {   /* the output stopped: more margin, if there was sound to lose */
        s_late_fades += (unsigned)(s_st.fades - fades0); fades0 = s_st.fades;
        int sound = 0;
        for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) { sound |= s_port_sound[p]; s_port_sound[p] = 0; }
        if (sound && s_q_target < 24) {
            s_q_target += 2;
            fprintf(stderr, "[cellAudio] the game fell behind the audio: keeping %u blocks (%u ms) in hand from now on%c",
                    s_q_target, s_q_target * CELL_AUDIO_BLOCK_SAMPLES * 1000u / CELL_AUDIO_SAMPLE_RATE, 10);
        }
    }
    if (!t0) { t0 = t; f0 = s_finished[0]; s_pub_steps = 0; }
    if (t - t0 < 30000000ull) return;
    const double sec = (double)(t - t0) / 1000000.0;
    const unsigned long long st = s_st.stretched_steps - st0, n = s_st.steps - n0;
    if (s_late_fades || s_late_skips || st)
        fprintf(stderr, "[cellAudio] in the last 30 s: stretched %u%% of the time, %u dropouts, %u jumps, game finished %.1f blocks/s, read index %.1f/s (in hand %u blocks)%c",
                n ? (unsigned)(st * 100u / n) : 0u, s_late_fades, s_late_skips, (s_finished[0] - f0) / sec, s_pub_steps / sec, s_q_target, 10);
    quiet = s_late_fades ? 0 : quiet + 1;
    if (quiet >= 2 && s_q_target > 6) { s_q_target -= 2; quiet = 0; }
    s_late_skips = s_late_fades = 0; s_pub_steps = 0;
    f0 = s_finished[0]; st0 = s_st.stretched_steps; n0 = s_st.steps;
    t0 = t;
#endif
}

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

static void audio_notify_event_queues(void)
{
    /* AUDIO_STATS=1: every ~5 s, how many mixed port-0 blocks held PCM and
     * how many period events reached the game. */
    { static int st = -1; if (st < 0) st = getenv("AUDIO_STATS") ? 1 : 0;
      if (st && g_audio_blocks >= 940) {
          fprintf(stderr, "[audio-stats] blocks=%lu with-pcm=%lu (%lu%%) events ok=%lu fail=%lu\n",
                  g_audio_blocks, g_audio_blocks_nz, g_audio_blocks_nz * 100 / g_audio_blocks,
                  g_audio_push_ok, g_audio_push_fail);
          g_audio_blocks = g_audio_blocks_nz = g_audio_push_ok = g_audio_push_fail = 0;
      } }
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue). */
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) continue;
        uint32_t qid = sys_event_find_queue_by_key(s_notify_queues[i].key);
        if (qid) {
            int rc = sys_event_queue_push_by_id(qid, s_notify_queues[i].key,
                                       0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0);
            if (rc == 0) g_audio_push_ok++; else g_audio_push_fail++;
        } else g_audio_push_fail++;
    }
}

/* The clock of what the game sees: one step per block in real time, with the
 * period event its frame waits on. The step leans 1% to keep what is buffered
 * (device queue + stretcher + finished unread blocks) near the target: the device's clock
 * is not this one, and a bigger target fills without silence. The game only
 * writes sooner or later. A ring still entirely unread
 * holds the step, or the writer would overwrite the block about to play. */
#if AUDIO_BACKEND_SDL2
static void audio_pub_tick(void)
{
    static double t_next, ema;
    const double block_us = CELL_AUDIO_BLOCK_SAMPLES * 1000000.0 / CELL_AUDIO_SAMPLE_RATE;
    const double now = (double)audio_now_us();
    if (t_next == 0.0 || now - t_next > 40000.0) t_next = now;   /* this thread stalled: no burst */
    while (now >= t_next) {
        u32 unread = 0; int any = 0, max_lag = -1000;
        mutex_lock(&s_audio_mutex);
        for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
            AudioPortSlot* port = &s_ports[p];
            if (!port->in_use || !port->running || !port->buffer) continue;
            const u32 nblock = (u32)port->param.nBlock;
            if (!s_port_done_seen[p]) {
                port->pub_index++;
                if (port->read_idx_addr)
                    vm_write64((u32)port->read_idx_addr, port->pub_index % nblock);
                continue;
            }
            u32 done = 0;
            for (u32 b = 0; b < nblock && b < 32; b++) done += s_blk_done[p][b];
            any = 1; if (done > unread) unread = done;
            if (done >= nblock) continue;
            /* A whole lap ahead of the writer reads to it as "equal": full,
             * nothing to do, just when it is furthest behind. Wait for it at 6
             * (lag counts finished blocks; some of those are on their way), but
             * never for long: it is not this index the game may wait on. */
            { static unsigned s_steps[CELL_AUDIO_PORT_MAX], s_base[CELL_AUDIO_PORT_MAX]; static u8 s_hold[CELL_AUDIO_PORT_MAX];
              const int lag = (int)(s_steps[p] - (s_finished[p] - s_base[p]));
              if (lag >= 6 && s_hold[p] < 16) { s_hold[p]++; if (lag > max_lag) max_lag = lag; continue; }
              if (s_hold[p] >= 16 || lag < -32) s_base[p] = s_finished[p] - s_steps[p];
              s_hold[p] = 0;
              s_steps[p]++;
              if (lag > max_lag) max_lag = lag; }
            s_pub_steps++;
            port->pub_index++;
            if (port->read_idx_addr)
                vm_write64((u32)port->read_idx_addr, port->pub_index % nblock);
        }
        mutex_unlock(&s_audio_mutex);
        double step = block_us;
        if (any) {
            const double level = (double)audio_backend_queued_samples() / CELL_AUDIO_BLOCK_SAMPLES + audio_st_blocks() + unread;
            const double want = (double)s_q_target + AUDIO_DEV_BLOCKS;
            ema += (level - ema) / 32.0;
            if (ema < want - 2.0 && max_lag < 0) step *= 0.99;
            else if (ema > want + 5.0) step *= 1.01;
        }
        t_next += step;
        audio_notify_event_queues();
    }
}
#endif

/* AUDIO_WAV=<file>: append the final mix as raw f32le stereo 48 kHz
 * (ffmpeg -f f32le -ar 48000 -ac 2 -i <file>). */
static void audio_wav_tap(const float* stereo)
{
    static FILE* _wf = (FILE*)-1;
    if (_wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); _wf = e ? fopen(e, "wb") : NULL; }
    if (_wf) fwrite(stereo, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 2, _wf);
}

#if AUDIO_BACKEND_SDL2
/* One block from the stretcher to the device, at the ratio the level asks for.
 * The game writes in bursts of about 3 blocks and the stretcher moves in steps
 * of 2, nothing to act on: stretching starts when under a block is left and then
 * fills all the way back to the target, so at full supply the ratio is exactly 1.
 * A short dip barely stretches; a lasting shortage settles where it must. */
static int audio_output_stretched(void)
{
    static float out[CELL_AUDIO_BLOCK_SAMPLES * 2];
    static double smooth = 1.0;
    static int filling;
    const double have = audio_st_blocks(), err = (double)s_q_target - have;
    if (have < 1.0) filling = 1; else if (err <= 0.0) filling = 0;
    double r = 1.0;
    if (filling) r = 1.02 + (err > 0.0 ? err : 0.0) * 0.1;
    else if (err < -20.0) r = 0.92;
    else if (err < -6.0) r = 0.97;
    if (r > 1.8) r = 1.8;
    smooth += (r - smooth) * 0.1;
    if (r == 1.0 && fabs(smooth - 1.0) < 0.01) smooth = 1.0;
    ast_set_ratio(&s_st, smooth);
    if (!ast_pull(&s_st, out, CELL_AUDIO_BLOCK_SAMPLES)) return 0;
    audio_wav_tap(out);
    if (s_sdl_audio_dev) SDL_QueueAudio(s_sdl_audio_dev, out, sizeof out);
    return 1;
}
#endif

#ifdef _WIN32
static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");
    /* It carries the clock the game's synth runs on (audio_pub_tick) and does
     * little else: a late wake here is a late read index for the whole game. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#if AUDIO_BACKEND_SDL2
    ast_init(&s_st);
#endif

    /* Pace to the device clock. The PS3 consumes one block every 5.33 ms;
     * the old Sleep(2)/Sleep(5) loop keyed on a queue depth the 1056-frame
     * WASAPI buffer can never reach, so it mixed ~225 blocks/s and the device
     * dropped the surplus: 20% of the audio thrown away, and a guest mixer
     * (GH3's FMOD) that could not keep ahead of the read index played gaps.
     * With no device, pace on the performance counter instead. */
    LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    unsigned long long blocks = 0;
    while (s_mix_thread_running) {
        const int room = audio_backend_room();
#if AUDIO_BACKEND_SDL2
        /* RACACIT_AUDIO_NOWAIT=1: read on and publish as before v89 (for A/B tests). */
        { static int nowait = -1;
          if (nowait < 0) nowait = getenv("RACACIT_AUDIO_NOWAIT") != NULL;
          s_pub_mode = room >= 0 && !nowait;
          if (s_pub_mode) audio_pub_tick(); }
#endif
#if AUDIO_BACKEND_SDL2
        if (s_pub_mode) {
            audio_feed_stretcher();
            audio_late_report();
            while (audio_backend_room() > 0 && audio_output_stretched()) {}
            audio_backend_wait();
            continue;
        }
#endif
        if (room >= 0 && room < CELL_AUDIO_BLOCK_SAMPLES) { audio_backend_wait(); continue; }
        if (room < 0) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const long long due = q0.QuadPart +
                (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
            if (now.QuadPart < due) { Sleep(1); continue; }
            /* More than 50 ms behind (device just went away, a debugger
             * stop): drop the backlog instead of mixing it in a burst. */
            if (now.QuadPart - due > qf.QuadPart / 20)
                q0.QuadPart = now.QuadPart -
                    (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
        }
        blocks++;
        /* Mix and submit one block */
        audio_mix_one_block();
        /* AUDIO_PEAK=1: report the mixed block's peak amplitude periodically so
         * "is any port producing sound" is answerable (LBP Bink movie audio). */
        { static int _ap = -1; if (_ap < 0) _ap = getenv("AUDIO_PEAK") ? 1 : 0;
          if (_ap) { static unsigned _n = 0; float pk = 0.0f;
            for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) {
                float a = s_mix_buffer[i]; if (a < 0) a = -a; if (a > pk) pk = a; }
            if ((++_n % 200) == 0 || (pk > 0.001f && _n < 40))
                fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", _n, pk); } }
        audio_wav_tap(s_mix_buffer);
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        /* AUDIO_RATE=1: blocks mixed vs real time, and frames dropped. */
        { static int on = -1; if (on < 0) on = getenv("AUDIO_RATE") ? 1 : 0;
          if (on) { static ULONGLONG t0; static unsigned n, z; static unsigned long long d0;
            ULONGLONG now = GetTickCount64(); if (!t0) t0 = now; n++;
            { int nz = 0; for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * 2; i++) if (s_mix_buffer[i] != 0.0f) { nz = 1; break; }
              if (!nz) z++; }
            if (now - t0 >= 5000) {
                fprintf(stderr, "[audio-rate] %.1f blocks/s (real time 187.5), silent %.1f%%, dropped %.0f frames/s, device buf %u%c",
                        n * 1000.0 / (now - t0), n ? 100.0 * z / n : 0.0, (g_audio_dropped - d0) * 1000.0 / (now - t0),
                #if AUDIO_BACKEND_WASAPI
                        (unsigned)s_wasapi_buf_frames, 10);
#else
                        0u, 10);
#endif
                n = z = 0; d0 = g_audio_dropped; t0 = now; } } }

        /* Notify event queues */
        audio_notify_event_queues();
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return 0;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    while (s_mix_thread_running) {
        audio_mix_one_block();
        /* AUDIO_WAV=<file>: the final mix as raw f32le stereo 48 kHz (as on Windows). */
        { static FILE* _wf = (FILE*)-1;
          if (_wf == (FILE*)-1) { const char* e = getenv("AUDIO_WAV"); _wf = e ? fopen(e, "wb") : NULL; }
          if (_wf) { fwrite(s_mix_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * 2, _wf); fflush(_wf); } }
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        audio_notify_event_queues();

        u32 queued = audio_backend_queued_samples();
        if (queued > CELL_AUDIO_BLOCK_SAMPLES * 4) {
            usleep(5000);
        } else {
            usleep(2000);
        }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;
    { extern void (*g_audio_put_hook)(uint32_t, uint32_t, uint32_t);
      g_audio_put_hook = audio_put_hook; }

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
    s_mix_thread_live = 1;
#endif
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    /* Same guard. Init leaves s_audio_initialized set even when the mixing
     * thread failed to start ("continuing silently" -- a title without audio
     * still runs), so Quit reaches here with s_mix_thread never assigned.
     * pthread_t has no null value to test, and joining an unset one, or
     * joining the same one twice, is undefined rather than an error the way
     * WaitForSingleObject on a null HANDLE is. */
    if (s_mix_thread_live) {
        pthread_join(s_mix_thread, NULL);
        s_mix_thread_live = 0;
    }
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    /* PS3_NO_AUDIO=1: report that the audio library is unavailable. Middleware
     * that drives its mixer through SPU jobs will otherwise block the whole boot
     * waiting on work we cannot yet complete; failing here lets a title take its
     * silent path and keep going. Diagnostic first, workaround second. */
    if (getenv("PS3_NO_AUDIO")) {
        printf("[cellAudio] PS3_NO_AUDIO -- reporting audio unavailable\n");
        return (s32)CELL_AUDIO_ERROR_NOT_INIT;
    }
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0) {
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");
        /* Don't fail -- games should still run without audio */
    }


    /* Port timestamps (cellAudioGetPortTimestamp) count from here: the same
     * guest clock origin as the timebase and audio_now_us, not raw host time. */
    s_audio_start_us = audio_now_us();
    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();

    /* Port buffers live in the guest vm_base arena (bump-allocated, not host
     * malloc) -- just drop the references; never free() them. */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* param / portNum are GUEST addresses; translate, and read the BE u64 param
     * fields via vm_read64 (a raw param->nChannel faults / is host-endian). */
    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    u64 nch  = vm_read64(param_ea + 0);   /* nChannel */
    u64 nblk = vm_read64(param_ea + 8);   /* nBlock   */
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    /* Store the decoded (host-endian) param values, not the raw BE guest bytes. */
    memset(&port->param, 0, sizeof(port->param));
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->read_index  = 0;
    port->pub_index   = 0;
    port->write_index = 0;
    s_port_done_seen[(int)(port - s_ports)] = 0;
    memset((void*)s_blk_done[(int)(port - s_ports)], 0, sizeof s_blk_done[0]);

    /* Allocate the audio buffer in GUEST memory so portAddr/readIndexAddr are
     * real guest addresses (the game's mixer -- FMOD here -- writes PCM there
     * and validates them as 1 MB-aligned guest pointers; a host pointer trips
     * its "invalid parameter" check). Bump from a free main-memory window; the
     * host-side `buffer` is just the vm_base view of the same bytes. */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    /* Keep audio outside sys_memory (0x40000000..0x50000000), the HLE heap
     * (0x50000000..0x58000000), and sys_vm (0x60000000..0x70000000).
     * Reserve two 1-MB pages per port so reopening reuses its own storage. */
    u32 guest_buf = 0x58000000u + (u32)found * 0x200000u;
    u32 guest_ridx = guest_buf + 0x100000u;
    if (vm_commit(guest_buf, 0x200000u) != CELL_OK) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    port->buffer = (float*)(vm_base + guest_buf);     /* host view of guest buffer */
    memset(port->buffer, 0, port->buf_size);
    vm_write64(guest_ridx, 0);                        /* read index counter (u64, see below) */

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);   /* guest out-param */

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* buffer points INTO the guest vm_base arena (reserved per port in PortOpen),
     * not a host malloc -- do NOT free() it (that corrupts the host heap).
     * The guest window is reclaimed wholesale when vm_base is released. */
    s_ports[portNum].buffer = NULL;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    { static int st = -1; if (st < 0) st = getenv("AUDIO_STATS") ? 1 : 0;
      static unsigned long n; if (st && (n++ < 20 || (n % 2000) == 0))
          fprintf(stderr, "[audio-api] GetPortBlockTag(port=%u block=%llu) #%lu ridx=%llu\n", portNum,
                  (unsigned long long)blockNo, n, (unsigned long long)s_ports[portNum < CELL_AUDIO_PORT_MAX ? portNum : 0].read_index); }
    uint32_t tag_ea = (uint32_t)(uintptr_t)tag;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!tag_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    u64 nblk = port->param.nBlock ? port->param.nBlock : 1;
    if (blockNo >= nblk) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PARAM;
    }
    u64 t = port->pub_index + blockNo - (port->pub_index % nblk);
    if (blockNo < port->pub_index % nblk) t += nblk;
    mutex_unlock(&s_audio_mutex);

    vm_write64(tag_ea, t);
    return CELL_OK;
}

s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    { static int st = -1; if (st < 0) st = getenv("AUDIO_STATS") ? 1 : 0;
      static unsigned long n; if (st && (n++ < 20 || (n % 2000) == 0))
          fprintf(stderr, "[audio-api] GetPortTimestamp(port=%u tag=%llu) #%lu ridx=%llu\n", portNum,
                  (unsigned long long)tag, n, (unsigned long long)s_ports[portNum < CELL_AUDIO_PORT_MAX ? portNum : 0].read_index); }
    uint32_t stamp_ea = (uint32_t)(uintptr_t)stamp;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!stamp_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    u64 global_counter = s_ports[portNum].pub_index;
    mutex_unlock(&s_audio_mutex);
    if (tag > global_counter)
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;

    u64 t = s_audio_start_us + tag * 256000000ULL / 48000ULL;
    vm_write64(stamp_ea, t);
    return CELL_OK;
}

/* cellAudioCreateNotifyEventQueue(sys_event_queue_t* id, u64* key)
 *
 * Creates the queue the game will block on for audio-period events, and
 * registers it for notification in one step -- the game never calls
 * SetNotifyEventQueue for this one.
 *
 * This was missing entirely. YDKJ imports it, got the unimplemented-NID path,
 * and then blocked on an id nothing had created: sys_event_queue_receive
 * answered CELL_ESRCH every time and the title logged 1.7 MILLION
 * "ERROR - LIBAUDIO DROPOUT" lines in a 60 s run.
 *
 * Both params are GUEST addresses and both are big-endian out-values; the id
 * is 32-bit and the key 64-bit, so they cannot be written with a plain store. */
s32 cellAudioCreateNotifyEventQueue(u32* id, u64* key)
{
    uint32_t id_ea  = (uint32_t)(uintptr_t)id;
    uint32_t key_ea = (uint32_t)(uintptr_t)key;

    printf("[cellAudio] CreateNotifyEventQueue(id_ea=0x%08X key_ea=0x%08X)\n",
           id_ea, key_ea);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (!id_ea || !key_ea)
        return CELL_AUDIO_ERROR_PARAM;

    /* A distinct key per queue: the notify path resolves queues BY KEY, and the
     * game creates its other queues with key 0, so reusing 0 here would make
     * find_queue_by_key ambiguous and post audio events to whichever matched. */
    static uint64_t s_next_key = 0x8000000000000001ull;
    uint64_t k = s_next_key++;

    uint32_t qid = sys_event_queue_create_direct(k, 8);
    if (!qid)
        return CELL_AUDIO_ERROR_EVENT_QUEUE;

    mutex_lock(&s_audio_mutex);
    int slot = -1;
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { mutex_unlock(&s_audio_mutex); return CELL_AUDIO_ERROR_PARAM; }
    s_notify_queues[slot].in_use = 1;
    s_notify_queues[slot].key    = k;
    mutex_unlock(&s_audio_mutex);

    vm_write32(id_ea, qid);
    vm_write64(key_ea, k);
    printf("[cellAudio] CreateNotifyEventQueue -> id=%u key=0x%llX\n",
           qid, (unsigned long long)k);
    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    /* `config` is a GUEST address; write the BE struct field-by-field at its
     * guest offsets (8-byte aligned u64s). */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    /* CellAudioPortConfig (cell/audio.h). sys_addr_t is uintptr_t, which on the
     * 32-bit PPU ABI is FOUR bytes -- not eight. Writing the two sys_addr_t
     * fields as u64 shifted every field past it and left the game reading the
     * high (zero) half of readIndexAddr as its pointer:
     *   sys_addr_t readIndexAddr;  u32 @  0
     *   uint32_t   status;         u32 @  4
     *   uint64_t   nChannel;       u64 @  8
     *   uint64_t   nBlock;         u64 @ 16
     *   uint32_t   portSize;       u32 @ 24
     *   sys_addr_t portAddr;       u32 @ 28      -> 32 bytes */
    vm_write32(cfg +  0, (u32)port->read_idx_addr);                          /* readIndexAddr */
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);           /* status */
    vm_write64(cfg +  8, port->param.nChannel);                             /* nChannel */
    vm_write64(cfg + 16, port->param.nBlock);                               /* nBlock */
    vm_write32(cfg + 24, port->buf_size);                                   /* portSize */
    vm_write32(cfg + 28, (u32)port->port_addr);                             /* portAddr */

    { static int _n = 0; if ((_n < 24 && (_n++, 1)))
        fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                portNum, port->running ? "RUN" : "READY",
                (u32)port->port_addr, (u32)port->read_idx_addr); }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !status)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_CLOSE);
    } else if (s_ports[portNum].running) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_RUN);
    } else {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_READY);
    }

    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}
