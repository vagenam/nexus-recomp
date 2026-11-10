/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput (no extra dependencies)
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows.
 */

#include <time.h>
#include "cellPad.h"
#include "touch_controls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX */
#include "../video/rsx_host_ui.h"                 /* Settings > Controller: triggers, rumble, light bar, motion */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL2  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
    /* Sixaxis (10-bit, 512 = level): accelerometer X/Y/Z, gyro (yaw). */
    int  has_motion;
    u16  sensor_x, sensor_y, sensor_z, sensor_g;
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];
/* Per-port "a report has arrived since your last read" flag; see cellPadGetData. */
static int s_data_fresh[PAD_MAX_HOST_PORTS];

#if PAD_BACKEND_SDL2
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static int s_sdl_inited = 0;
#endif

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

static u8 pad_xinput_stick_to_u8(short raw, short deadzone)
{
    float normalized;
    if (raw > deadzone)
        normalized = (float)(raw - deadzone) / (float)(32767 - deadzone);
    else if (raw < -deadzone)
        normalized = (float)(raw + deadzone) / (float)(32767 - deadzone);
    else
        normalized = 0.0f;

    /* Map -1.0..1.0 to 0..255 with center at 128 */
    int val = (int)(normalized * 127.0f) + 128;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* Keyboard fallback for port 0.
 *
 * The only backend here is XInput, so on a machine with no controller plugged
 * in a title gets a pad that is reported present and never presses anything --
 * it can be watched but not played. That is what The Simpsons Arcade Game hit:
 * it reached its attract loop and no input existed to start a game.
 *
 * Only fills in for port 0, only when XInput found nothing there, so a real
 * controller always wins and nothing changes for a port that has one. Keys are
 * read only while a window of THIS process is in the foreground, so typing in
 * another application does not drive the game. Set PAD_NO_KEYBOARD=1 to
 * disable it entirely.
 *
 * Arrows = d-pad, Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1,
 * 1/2 = L2/R2, Enter = START, Tab = SELECT. The left stick mirrors the d-pad
 * so a title that reads the stick instead is playable too. */
#ifdef _WIN32
static int pad_host_window_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void pad_poll_keyboard(void)
{
    static int off = -1;
    if (off < 0) off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
    if (off) return;

    PadHostState* hs = &s_host_state[0];
    if (!pad_host_window_focused()) {
        /* Release everything on focus loss, or a key held while alt-tabbing
         * would stay down forever. */
        hs->buttons = 0;
        hs->analog_lx = hs->analog_ly = 128;
        hs->analog_rx = hs->analog_ry = 128;
        hs->connected = 1;
        return;
    }

    static const struct { int vk; u16 btn; } map[] = {
        { VK_UP,     CELL_PAD_CTRL_UP },      { VK_DOWN,  CELL_PAD_CTRL_DOWN },
        { VK_LEFT,   CELL_PAD_CTRL_LEFT },    { VK_RIGHT, CELL_PAD_CTRL_RIGHT },
        { 'Z',       CELL_PAD_CTRL_CROSS },   { 'X',      CELL_PAD_CTRL_CIRCLE },
        { 'A',       CELL_PAD_CTRL_SQUARE },  { 'S',      CELL_PAD_CTRL_TRIANGLE },
        { 'Q',       CELL_PAD_CTRL_L1 },      { 'W',      CELL_PAD_CTRL_R1 },
        { '1',       CELL_PAD_CTRL_L2 },      { '2',      CELL_PAD_CTRL_R2 },
        { VK_RETURN, CELL_PAD_CTRL_START },   { VK_TAB,   CELL_PAD_CTRL_SELECT },
    };

    u16 btns = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (GetAsyncKeyState(map[i].vk) & 0x8000) btns |= map[i].btn;

    hs->buttons   = btns;
    hs->connected = 1;
    hs->analog_lx = (u8)((btns & CELL_PAD_CTRL_LEFT) ? 0 :
                         (btns & CELL_PAD_CTRL_RIGHT) ? 255 : 128);
    hs->analog_ly = (u8)((btns & CELL_PAD_CTRL_UP) ? 0 :
                         (btns & CELL_PAD_CTRL_DOWN) ? 255 : 128);
    hs->analog_rx = hs->analog_ry = 128;
    hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
    hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);

    { static int said = 0;
      if (!said && btns) { said = 1;
          printf("[cellPad] keyboard fallback active on port 0 (no XInput device)\n");
          fflush(stdout); } }
}
#endif

static void pad_poll_xinput(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        DWORD result = XInputGetState((DWORD)i, &state);
        if (result != ERROR_SUCCESS) {
            s_host_state[i].connected = 0;
            continue;
        }

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* */
        u16 btns = 0;
        if (gp->wButtons & XINPUT_GAMEPAD_BACK)           btns |= CELL_PAD_CTRL_SELECT;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= CELL_PAD_CTRL_L3;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= CELL_PAD_CTRL_R3;
        if (gp->wButtons & XINPUT_GAMEPAD_START)          btns |= CELL_PAD_CTRL_START;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP)        btns |= CELL_PAD_CTRL_UP;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= CELL_PAD_CTRL_RIGHT;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= CELL_PAD_CTRL_DOWN;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= CELL_PAD_CTRL_LEFT;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)     btns |= CELL_PAD_CTRL_L2;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD)    btns |= CELL_PAD_CTRL_R2;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= CELL_PAD_CTRL_L1;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= CELL_PAD_CTRL_R1;
        if (gp->wButtons & XINPUT_GAMEPAD_Y)              btns |= CELL_PAD_CTRL_TRIANGLE;
        if (gp->wButtons & XINPUT_GAMEPAD_B)              btns |= CELL_PAD_CTRL_CIRCLE;
        if (gp->wButtons & XINPUT_GAMEPAD_A)              btns |= CELL_PAD_CTRL_CROSS;
        if (gp->wButtons & XINPUT_GAMEPAD_X)              btns |= CELL_PAD_CTRL_SQUARE;

        s_host_state[i].buttons = btns;

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0). Reflect about
         * 128 (256 - x), NOT 255 - x: the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START and can make the guest self-exit.
         * (via sagemono, PR #42) */
        s_host_state[i].analog_lx = pad_xinput_stick_to_u8(gp->sThumbLX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ly = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbLY, PAD_STICK_DEADZONE));
        s_host_state[i].analog_rx = pad_xinput_stick_to_u8(gp->sThumbRX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ry = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbRY, PAD_STICK_DEADZONE));

        /* Triggers */
        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

        /* Pressure-sensitive buttons: XInput has digital only, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }

}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init */
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

#endif /* PAD_BACKEND_XINPUT */

/* ---------------------------------------------------------------------------
 * SDL2 backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

static u8 pad_sdl_axis_to_u8(int raw)
{
    /* SDL axis: -32768..32767 -> 0..255 with center at 128 */
    int val = ((raw + 32768) * 255) / 65535;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* ---------------------------------------------------------------------------
 * DualSense / DualShock 4 extras (SDL's HID driver; main.cpp sets the hints)
 *
 *   - player LED / number and the light bar colour
 *     (RACACIT_DS_LIGHTBAR=r,g,b; default Ratchet orange)
 *   - the motion sensors as the PS3 Sixaxis reports them
 *   - "Fire button = R2": R1 and R2 trade places -- R2 fires, the game's
 *     R2 moves to R1; L2 strafes and L1 looks as on the PS3 (v52-v69 traded
 *     L1/L2 too); see the poll below for quick select and hoverboots
 *     (RACACIT_DS_SWAP=0 keeps the PS3 layout; RACACIT_FIRE_ON_R2=1
 *     swaps on any controller, =0 on none)
 *   - DualSense adaptive triggers (RACACIT_DS_TRIGGERS=0 turns them off):
 *     swapped, R2 is a weapon trigger -- resistance that gives way with a
 *     click -- and L2 a light, even resistance; unswapped, both triggers
 *     have a light resistance.
 * -----------------------------------------------------------------------*/
/* Game state the layout follows: the quick select ring is open (TRIANGLE
 * held, set by the poll below); Ratchet is on hoverboots (game/stubs.cpp,
 * from the hoverboots' own sounds). */
volatile int g_racacit_ctx_quickselect, g_racacit_ctx_hoverboots;
static int pad_swap(SDL_GameController* gc)
{
    static int force = -2;
    if (force == -2) { const char* e = getenv("RACACIT_FIRE_ON_R2"); force = e ? (*e == '1') : -1; }
    if (force >= 0) return force;
    rsx_host_options o; rsx_host_ui_options(&o);           /* "Fire button", live: every controller */
    return o.ds_swap && gc;
}

/* DualSense trigger effect blocks (11 bytes each; the formats SDL's
 * testgamecontroller and the community trigger-effect documentation use). */
static void ds5_feedback(Uint8* d, int position, int strength)    /* resistance from position (0-9) on */
{
    memset(d, 0, 11);
    if (strength < 1) { d[0] = 0x05; return; }                     /* off */
    unsigned force = (unsigned)(strength - 1) & 7u, active = 0;
    unsigned long long zones = 0;
    for (int i = position; i < 10; i++) { zones |= (unsigned long long)force << (3 * i); active |= 1u << i; }
    d[0] = 0x21;
    d[1] = (Uint8)(active & 0xFF); d[2] = (Uint8)(active >> 8);
    d[3] = (Uint8)zones; d[4] = (Uint8)(zones >> 8); d[5] = (Uint8)(zones >> 16); d[6] = (Uint8)(zones >> 24);
}
static void ds5_weapon(Uint8* d, int start, int end, int strength)   /* gives way at `end` (2-8) */
{
    memset(d, 0, 11);
    unsigned zones = (1u << start) | (1u << end);
    d[0] = 0x25; d[1] = (Uint8)(zones & 0xFF); d[2] = (Uint8)(zones >> 8); d[3] = (Uint8)(strength - 1);
}
static int s_kick[PAD_MAX_HOST_PORTS];                /* R2 vibration amplitude now (0 = resistance) */
static unsigned long long s_kick_ms[PAD_MAX_HOST_PORTS];
/* Vibration inside the trigger from `position` (0-9) on: amplitude 1-8,
 * frequency in Hz. */
static void ds5_vibration(Uint8* d, int position, int amplitude, int frequency)
{
    memset(d, 0, 11);
    if (amplitude < 1 || frequency < 1) { d[0] = 0x05; return; }
    unsigned active = 0; unsigned long long zones = 0;
    for (int i = position; i < 10; i++) { zones |= (unsigned long long)((amplitude - 1) & 7) << (3 * i); active |= 1u << i; }
    d[0] = 0x26;
    d[1] = (Uint8)(active & 0xFF); d[2] = (Uint8)(active >> 8);
    d[3] = (Uint8)zones; d[4] = (Uint8)(zones >> 8); d[5] = (Uint8)(zones >> 16); d[6] = (Uint8)(zones >> 24);
    d[9] = (Uint8)frequency;
}
/* Every trigger effect goes out through ds5_send, which remembers the block
 * last sent per trigger (R2, L2) for ds5_readback and logs a failed send. */
static Uint8 s_ds_want[2][11];                        /* [0] = 0: nothing sent yet */
static void ds5_send(SDL_GameController* gc, Uint8* st)
{
    if (st[0] & 0x04) memcpy(s_ds_want[0], st + 10, 11);
    if (st[0] & 0x08) memcpy(s_ds_want[1], st + 21, 11);
    if (SDL_GameControllerSendEffect(gc, st, 47) != 0) {
        static int n; if (n++ < 10) fprintf(stderr, "[ds-trigger] effect not sent: %s%c", SDL_GetError(), 10);
    }
}
/* Readback (owner, v54: "the adaptive triggers don't seem to be active at
 * all"). The controller reports the trigger effect it is running in its input
 * report: packet byte 47 (after the report id; Bluetooth 0x31 reports have one
 * more header byte), R2 in the low nibble, L2 in the high one, 0 = none --
 * measured on the owner's DualSense over USB: weapon 0x25 -> 2, vibration 0x26
 * -> 3, feedback 0x21 -> 1, off 0x05 -> 0, each held through rumble, LED,
 * speaker-path reports and the USB audio stream. A second HID handle reads it
 * four times a second; an effect we set that the controller is not running
 * (another program or a reset cleared it) is sent again. Logged: the first
 * confirmation per trigger, each re-send (first 20, then every 100th). */
static void ds5_readback(SDL_GameController* gc)
{
    static int state;                                 /* 0 not opened yet, 1 open, -1 unavailable */
    static SDL_hid_device* h;
    static Uint64 next;
    static int miss[2], confirmed[2];
    static unsigned resent;
    const Uint64 now = SDL_GetTicks64();
    if (now < next) return;
    next = now + 250;
    if (!state) {
        state = -1;
        struct SDL_hid_device_info* all = SDL_hid_enumerate(0x054C, 0);
        for (struct SDL_hid_device_info* d = all; d && !h; d = d->next)
            if (d->product_id == 0x0CE6 || d->product_id == 0x0DF2) h = SDL_hid_open_path(d->path, 0);
        SDL_hid_free_enumeration(all);
        if (h) state = 1;
        fprintf(stderr, "[ds-trigger] readback %s%c", h ? "on (second HID handle)" : "unavailable: no DualSense HID device opened", 10);
        if (!h) return;
    }
    if (state < 0) return;
    Uint8 r[80], last[80]; int n, got = 0;
    while ((n = SDL_hid_read_timeout(h, r, sizeof r, 0)) > 0) { memcpy(last, r, (size_t)n); got = n; }
    if (n < 0) { SDL_hid_close(h); h = NULL; state = 0; next = now + 2000; return; }   /* unplugged: reopen later */
    const int o = got && last[0] == 0x31 ? 2 : got && last[0] == 0x01 ? 1 : -1;
    if (o < 0 || got < o + 48) return;
    const Uint8 fx = last[o + 47];
    for (int t = 0; t < 2; t++) {
        const int running = (t ? fx >> 4 : fx & 15) != 0;
        const int want = s_ds_want[t][0] && s_ds_want[t][0] != 0x05;
        if (!want || running) {
            if (want && !confirmed[t]) { confirmed[t] = 1;
                fprintf(stderr, "[ds-trigger] the controller runs the %s effect (0x%02X)%c", t ? "L2" : "R2", s_ds_want[t][0], 10); }
            miss[t] = 0;
            continue;
        }
        if (++miss[t] < 2) continue;                  /* two looks in a row: not just a send in flight */
        miss[t] = 0; resent++;
        Uint8 st[47]; memset(st, 0, sizeof st);
        st[0] = t ? 0x08 : 0x04;
        memcpy(st + (t ? 21 : 10), s_ds_want[t], 11);
        ds5_send(gc, st);
        if (resent <= 20 || resent % 100 == 0)
            fprintf(stderr, "[ds-trigger] %s effect 0x%02X was not running on the controller (reported 0x%02X): sent again (%u so far)%c",
                    t ? "L2" : "R2", s_ds_want[t][0], fx, resent, 10);
    }
}
/* ---- Weapon-aware triggers (DualSense, Rift Apart layout) ------------------
 * Driven by the game's own weapon events, which game/stubs.cpp reads from its
 * sound starts (A Crack in Time names them: wep_<weapon>_..., the equip and
 * out-of-ammo sounds) and the equipped-weapon id:
 *   - the fire trigger rests in the equipped weapon's profile -- a heavy pull
 *     that snaps for shotguns and launchers, a crisp click at mid-travel for
 *     pistols and blasters, a wall that gives way for throwables, a firm even
 *     resistance for beams, a short click for deployables (v55: firmer; the
 *     light ones read as no effect at all);
 *   - every shot kicks inside the trigger, timed to the shot itself rather
 *     than to the rumble (which a hit or an explosion also drives);
 *   - out of ammo, the trigger goes slack until the next shot or weapon.
 * Until the first event arrives the old rumble-driven kick stays in charge. */
enum { DSW_MEDIUM, DSW_LIGHT, DSW_HEAVY, DSW_THROW, DSW_BEAM, DSW_DEPLOY, DSW_N };
static volatile int      s_dsw_prof = DSW_MEDIUM, s_dsw_empty = 0, s_dsw_events = 0;
static volatile unsigned s_dsw_shots = 0;
volatile int             g_racacit_ds_pad = 0;     /* a DualSense with weapon triggers is in use */
void racacit_ds_weapon_profile(int p) { s_dsw_prof = (p >= 0 && p < DSW_N) ? p : DSW_MEDIUM; s_dsw_empty = 0; s_dsw_events = 1; }
void racacit_ds_weapon_shot(void)     { s_dsw_empty = 0; s_dsw_shots++; s_dsw_events = 1; }
void racacit_ds_weapon_empty(void)    { s_dsw_empty = 1; s_dsw_events = 1; }
static volatile unsigned s_dsw_lshots = 0;
void racacit_ds_weapon_shot_left(void) { s_dsw_lshots++; s_dsw_events = 1; }   /* a missile: L2 kicks */
static void ds5_weapon(Uint8* d, int start, int end, int strength);
static void ds5_feedback(Uint8* d, int position, int strength);
static void ds5_vibration(Uint8* d, int position, int amplitude, int frequency);
/* Settings > Controls > Trigger strength (v70, 10-100%): scales the resting
 * resistance and every kick; 100% is the strongest the trigger motors give
 * ("the weapon effects themselves should have a real punch with the adaptive
 * triggers, at an appropriate volume based on what the user has applied",
 * owner, v69). */
static int dsw_scale(int v)
{
    rsx_host_options o; rsx_host_ui_options(&o);
    const int r = (v * o.ds_trig_str + 50) / 100;
    return r < 1 ? 1 : r;
}
static void dsw_rest(Uint8* d, int prof, int empty)
{
    if (empty) { memset(d, 0, 11); d[0] = 0x05; return; }           /* slack */
    switch (prof) {
    case DSW_LIGHT:  ds5_weapon(d, 4, 6, dsw_scale(6)); break;      /* a crisp click at mid-travel */
    case DSW_HEAVY:  ds5_weapon(d, 2, 7, dsw_scale(8)); break;      /* long hard pull, snaps late */
    case DSW_THROW:  ds5_weapon(d, 4, 6, dsw_scale(7)); break;      /* a wall at mid-travel */
    case DSW_BEAM:   ds5_feedback(d, 2, dsw_scale(5)); break;       /* firm, even, all the way */
    case DSW_DEPLOY: ds5_weapon(d, 3, 5, dsw_scale(7)); break;
    default:         ds5_weapon(d, 3, 6, dsw_scale(8)); break;
    }
}
/* The kick per shot: amplitude (1-8, scaled), frequency (lower = a deeper
 * thump), length. v70: every kind at or near the motors' maximum, lower and
 * longer (v55-v69: 4-8, 25-60 Hz, 45-130 ms -- "no real punch", owner). */
static const struct { int amp, freq, ms; } k_dsw_kick[DSW_N] = {
    { 8, 28, 90 }, { 7, 38, 60 }, { 8, 18, 160 }, { 8, 24, 110 }, { 6, 45, 100 }, { 6, 32, 70 } };
static volatile int s_dsw_reapply;   /* pad_ds_apply re-sent the base triggers: the weapon's profile again */
static void dsw_poll(int port, SDL_GameController* gc)
{
    static int applied = -1, applied_empty = -1, kicking = 0;
    static unsigned seen = 0; static Uint64 until = 0;
    { rsx_host_options o; rsx_host_ui_options(&o); if (!o.ds_triggers) return; }   /* "Adaptive triggers", live */
    if (s_dsw_reapply) { s_dsw_reapply = 0; applied = -1; }
    if (port != 0 || !s_dsw_events) return;
    const Uint64 now = SDL_GetTicks64();
    const int prof = s_dsw_prof, empty = s_dsw_empty;
    Uint8 st[47];
    { static unsigned lseen = 0; static int lkick = 0; static Uint64 luntil = 0;   /* L2: a missile's kick, then the aim resistance */
      if (s_dsw_lshots != lseen) {
          lseen = s_dsw_lshots;
          memset(st, 0, sizeof st); st[0] = 0x08;
          ds5_vibration(&st[21], 2, dsw_scale(8), 22);
          ds5_send(gc, st);
          lkick = 1; luntil = now + 140;
      } else if (lkick && now >= luntil) {
          lkick = 0;
          memset(st, 0, sizeof st); st[0] = 0x08;
          ds5_feedback(&st[21], 2, dsw_scale(5));
          ds5_send(gc, st);
      } }
    if (s_dsw_shots != seen) {                                     /* a shot: kick inside the trigger */
        seen = s_dsw_shots;
        memset(st, 0, sizeof st); st[0] = 0x04;
        ds5_vibration(&st[10], 2, dsw_scale(k_dsw_kick[prof].amp), k_dsw_kick[prof].freq);
        ds5_send(gc, st);
        kicking = 1; until = now + (Uint64)k_dsw_kick[prof].ms;
        return;
    }
    if (kicking && now < until) return;
    if (kicking || prof != applied || empty != applied_empty) {    /* back to (or into) the resting profile */
        if (prof != applied || empty != applied_empty) {
            static int dbg = -1; if (dbg < 0) { const char* e = getenv("RACACIT_DS_DEBUG"); dbg = e && *e == '1'; }
            if (dbg) fprintf(stderr, "[ds-trigger] rest profile %d%s (kicks so far %u)%c", prof, empty ? " EMPTY" : "", seen, 10);
        }
        kicking = 0; applied = prof; applied_empty = empty;
        memset(st, 0, sizeof st); st[0] = 0x04;
        dsw_rest(&st[10], prof, empty);
        ds5_send(gc, st);
    }
}

/* Which button layout the player is on, for UI that names buttons (the
 * game's button prompts): 0 = PS3 layout, 1 = DualSense in the Rift Apart
 * layout (R2/L2 fire/aim; the game's R1<->R2 and L1<->L2 swapped -- see
 * pad_swap), 2 = an Xbox-style controller (same swap, Xbox glyphs wanted).
 * Port 0's controller; 0 with none. */
/* The same, as of the last poll, for other threads (game/button_prompts.cpp):
 * only the pad thread touches the SDL controllers. */
volatile int g_racacit_pad_layout;
int racacit_pad_layout(void)
{
    /* RACACIT_TEST_LAYOUT=<n>: that layout whatever is plugged in -- an agent
     * run checks the swapped prompts and ring badges without the owner's pad. */
    { static int force = -2;
      if (force == -2) { const char* e = getenv("RACACIT_TEST_LAYOUT"); force = e && *e ? atoi(e) : -1; }
      if (force >= 0) return force; }
    SDL_GameController* gc = s_sdl_controllers[0];
    int r, why;   /* why: 0 no controller, 1 not attached, 2 Fire button = R1, 3 swapped */
    if (!gc) { r = 0; why = 0; }
    else if (!SDL_GameControllerGetAttached(gc)) { r = 0; why = 1; }
    else if (!pad_swap(gc)) { r = 0; why = 2; }
    else {
        const SDL_GameControllerType t = SDL_GameControllerGetType(gc);
        r = (t == SDL_CONTROLLER_TYPE_PS5 || t == SDL_CONTROLLER_TYPE_PS4 || t == SDL_CONTROLLER_TYPE_PS3) ? 1 : 2; why = 3;
    }
    /* Port 0 empty or reading as detached for a moment (a USB/Bluetooth
     * hiccup, a re-enumeration) keeps the layout it had for 3 s, so the
     * prompts do not flip on a blip. */
    static int last = -1, n; static Uint64 gone_since;
    if (why < 2 && last > 0) {
        const Uint64 now = SDL_GetTicks64();
        if (!gone_since) gone_since = now;
        if (now - gone_since < 3000) return last;
    } else gone_since = 0;
    /* Every change, with its reason (the owner's v66: the layout went to 0
     * during a level load and firing stayed on R1). */
    if (r != last && n < 100) {
        static const char* const k_why[4] = { "no controller on port 0", "port 0's controller reads as detached",
                                              "Fire button is R1 (PS3 layout)", "Fire button is R2" };
        fprintf(stderr, "[cellPad] layout %d: %s%c", r, k_why[why], 10);
        last = r; n++;
    }
    return r;
}

static void ds5_send_triggers(SDL_GameController* gc, int clear)
{
    Uint8 st[47];                      /* SDL's DS5EffectsState_t */
    memset(st, 0, sizeof st);
    st[0] = 0x04 | 0x08;               /* right and left trigger effect */
    if (clear) { st[10] = 0x05; st[21] = 0x05; }
    else {
        /* Firm: a weapon trigger that holds to a little past half and snaps,
         * and a solid aim trigger -- a weak resistance read as a faint buzz. */
        if (pad_swap(gc)) { ds5_weapon(&st[10], 3, 6, 8); ds5_feedback(&st[21], 2, 5); }
        else { ds5_feedback(&st[10], 2, 5); ds5_feedback(&st[21], 3, 4); }
    }
    ds5_send(gc, st);
}
/* DualSense speaker: the controller's audio device (cellAudio.c, USB) plays
 * through the built-in speaker instead of only the headset jack -- output
 * path 3 (speaker takes the right channel), speaker volume and preamp gain.
 * RACACIT_DS_SPEAKER=0 or RACACIT_DS_DEVICE=0 (agent test runs: cellAudio.c
 * opens no controller audio) leave the audio path alone. The volume byte stays at
 * 0x64, the Linux driver's value: the controller's usable range is only
 * about 0x3D-0x64, so v42-v44's 65 sat near the bottom of it and the speaker
 * was close to silent. RACACIT_DS_SPK_VOL is a percentage now, applied to
 * the samples (cellAudio.c); RACACIT_DS_SPK_HW overrides the raw byte and
 * RACACIT_DS_SPK_GAIN the preamp (0-7, default 2). The layout:
 * hid-playstation's speaker patches. */
static void ds5_speaker_route(SDL_GameController* gc)
{
    const char* e = getenv("RACACIT_DS_SPEAKER");
    if (e && *e == '0') return;
    e = getenv("RACACIT_DS_DEVICE");
    if (e && *e == '0') return;
    int vol = 0x64, gain = 2;
    { const char* v = getenv("RACACIT_DS_SPK_HW"); if (v) vol = atoi(v); }
    { const char* g = getenv("RACACIT_DS_SPK_GAIN"); if (g) gain = atoi(g); }
    if (vol < 0) vol = 0; if (vol > 255) vol = 255; if (gain < 0) gain = 0; if (gain > 7) gain = 7;
    Uint8 st[47];
    memset(st, 0, sizeof st);
    st[0] = 0x80 | 0x20;               /* audio control, speaker volume */
    st[1] = 0x80;                      /* audio control 2 (preamp) */
    st[5] = (Uint8)vol;
    st[7] = 3 << 4;                    /* output path: speaker */
    st[37] = (Uint8)gain;
    SDL_GameControllerSendEffect(gc, st, sizeof st);
}
static void pad_ds_clear_all(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++)
        if (s_sdl_controllers[i] && SDL_GameControllerGetType(s_sdl_controllers[i]) == SDL_CONTROLLER_TYPE_PS5) {
            ds5_send_triggers(s_sdl_controllers[i], 1);
            /* ...and the audio path back to the headset jack: the speaker
             * would otherwise keep playing whatever Windows sends there. */
            const char* e = getenv("RACACIT_DS_SPEAKER"), *d = getenv("RACACIT_DS_DEVICE");
            if (!(e && *e == '0') && !(d && *d == '0')) {
                Uint8 st[47]; memset(st, 0, sizeof st);
                st[0] = 0x80; st[7] = 0;
                SDL_GameControllerSendEffect(s_sdl_controllers[i], st, sizeof st);
            }
        }
}
/* The light bar: Ratchet orange (RACACIT_DS_LIGHTBAR=r,g,b), or dark with
 * Settings > Controller > Light bar off. */
static void pad_ds_lightbar(SDL_GameController* gc)
{
    if (!gc || !SDL_GameControllerHasLED(gc)) return;
    int r = 255, g = 96, b = 0;
    { const char* e = getenv("RACACIT_DS_LIGHTBAR"); if (e) sscanf(e, "%d,%d,%d", &r, &g, &b); }
    rsx_host_options o; rsx_host_ui_options(&o);
    if (!o.ds_plus) return;             /* plain pad: SDL's own player colour */
    if (!o.ds_lightbar) r = g = b = 0;
    SDL_GameControllerSetLED(gc, (Uint8)r, (Uint8)g, (Uint8)b);
}
/* Settings > Controller, live: the trigger resistance (or none) for the
 * layout in use, and the light bar, sent again whenever one of them changed
 * (or `force`, at connect). Port 0's weapon events (game/stubs.cpp) follow. */
static void pad_ds_apply(int port, SDL_GameController* gc, int force)
{
    static int a_trig[PAD_MAX_HOST_PORTS], a_swap[PAD_MAX_HOST_PORTS], a_light[PAD_MAX_HOST_PORTS];
    if (port < 0 || port >= PAD_MAX_HOST_PORTS || !gc) return;
    rsx_host_options o; rsx_host_ui_options(&o);
    const int swap = pad_swap(gc);
    if (force || o.ds_triggers != a_trig[port] || swap != a_swap[port]) {
        ds5_send_triggers(gc, !o.ds_triggers);
        s_kick[port] = 0;
        if (port == 0) s_dsw_reapply = 1;
        if (port == 0) g_racacit_ds_pad = o.ds_triggers && swap;
    }
    if (force || o.ds_lightbar != a_light[port]) pad_ds_lightbar(gc);
    a_trig[port] = o.ds_triggers; a_swap[port] = swap; a_light[port] = o.ds_lightbar;
}
/* The DualSense Plus mod (rsx_host_ui.h ds_plus): without it a DualSense
 * gets no DualSense output at all -- no trigger effects, speaker route or
 * light bar colour -- and reads like any other controller. */
static int pad_ds_plus(void)
{
    rsx_host_options o; rsx_host_ui_options(&o);
    return o.ds_plus;
}
static void pad_ds_setup(int port, SDL_GameController* gc)
{
    if (!gc) return;
    SDL_GameControllerSetPlayerIndex(gc, port);
    pad_ds_lightbar(gc);
    s_host_state[port].has_motion = 0;
    if (SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL) && SDL_GameControllerHasSensor(gc, SDL_SENSOR_GYRO)) {
        SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_ACCEL, SDL_TRUE);
        SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_GYRO, SDL_TRUE);
        s_host_state[port].has_motion = 1;
    }
    if (SDL_GameControllerGetType(gc) == SDL_CONTROLLER_TYPE_PS5 && pad_ds_plus()) {
        static int once;
        if (!once) { once = 1; atexit(pad_ds_clear_all); }   /* do not leave the triggers stiff */
        pad_ds_apply(port, gc, 1);
        ds5_speaker_route(gc);
    }
    /* How SDL reads it: its GUID's driver byte (SDL_joystick.c). */
    const Uint8 drv = SDL_JoystickGetGUID(SDL_GameControllerGetJoystick(gc)).data[14];
    fprintf(stderr, "[cellPad] controller %d: %s (%s)%s%s%c", port, SDL_GameControllerName(gc) ? SDL_GameControllerName(gc) : "?",
            drv == 'x' ? "XInput" : drv == 'h' ? "HIDAPI" : drv == 'r' ? "Raw Input" : drv == 'w' ? "Windows.Gaming.Input" :
            drv == 'v' ? "virtual" : "DirectInput",
            s_host_state[port].has_motion ? ", motion" : "",
            SDL_GameControllerGetType(gc) == SDL_CONTROLLER_TYPE_PS5 ? ", adaptive triggers" : "", 10);
}
static u16 pad_sensor10(float v)
{
    int x = (int)(v + 512.5f);
    return (u16)(x < 0 ? 0 : x > 1023 ? 1023 : x);
}
static void pad_ds_motion(int port, SDL_GameController* gc)
{
    PadHostState* hs = &s_host_state[port];
    hs->sensor_x = 512; hs->sensor_y = 399; hs->sensor_z = 512; hs->sensor_g = 512;
    if (!hs->has_motion) return;
    { rsx_host_options o; rsx_host_ui_options(&o); if (!o.ds_motion) return; }   /* "Motion sensors": a pad at rest */
    float a[3] = { 0, 0, 0 }, w[3] = { 0, 0, 0 };
    if (SDL_GameControllerGetSensorData(gc, SDL_SENSOR_ACCEL, a, 3) != 0) return;
    SDL_GameControllerGetSensorData(gc, SDL_SENSOR_GYRO, w, 3);
    /* Sixaxis: ~113 counts per g (at rest Y reads 512 - 113); the gyro ~123
     * counts per 90 deg/s about the vertical axis. SDL: m/s^2 and rad/s. */
    const float g = SDL_STANDARD_GRAVITY;
    hs->sensor_x = pad_sensor10(-a[0] / g * 113.0f);
    hs->sensor_y = pad_sensor10(-a[1] / g * 113.0f);
    hs->sensor_z = pad_sensor10(-a[2] / g * 113.0f);
    hs->sensor_g = pad_sensor10(-w[1] * 57.2958f * 123.0f / 90.0f);
}

static void pad_poll_sdl_fallbacks(void);


/* A joystick SDL has no controller mapping for (an unknown USB or Bluetooth
 * pad): the common DirectInput layout -- four face buttons, two shoulders,
 * back/start, the sticks on axes 0-3, a hat for the d-pad. Only for devices
 * that look like gamepads, and only when no mapped controller is there to
 * take the port. A line in gamecontrollerdb.txt (see pad_load_mappings)
 * replaces the guess. */
static int pad_generic_mapping(int d)
{
    static SDL_JoystickID tried[16]; static int ntried;   /* each device is looked at once */
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(d);
    for (int i = 0; i < ntried; i++) if (tried[i] == id) return SDL_IsGameController(d);
    tried[ntried++ % 16] = id;
    if (ntried > 16) ntried = 16;
    const SDL_JoystickType t = SDL_JoystickGetDeviceType(d);
    if (t == SDL_JOYSTICK_TYPE_WHEEL || t == SDL_JOYSTICK_TYPE_FLIGHT_STICK || t == SDL_JOYSTICK_TYPE_THROTTLE ||
        t == SDL_JOYSTICK_TYPE_GUITAR || t == SDL_JOYSTICK_TYPE_DRUM_KIT || t == SDL_JOYSTICK_TYPE_DANCE_PAD) return 0;
    SDL_Joystick* j = SDL_JoystickOpen(d);
    if (!j) return 0;
    const int na = SDL_JoystickNumAxes(j), nb = SDL_JoystickNumButtons(j), nh = SDL_JoystickNumHats(j);
    int ok = 0;
    if (na >= 2 && nb >= 4) {
        char guid[64], map[768];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guid, sizeof guid);
        const char* name = SDL_JoystickName(j);
        int n = snprintf(map, sizeof map, "%s,%s,a:b0,b:b1,x:b2,y:b3,leftx:a0,lefty:a1", guid, "Generic gamepad");
        if (nb >= 6)  n += snprintf(map + n, sizeof map - (size_t)n, ",leftshoulder:b4,rightshoulder:b5");
        if (nb >= 8)  n += snprintf(map + n, sizeof map - (size_t)n, ",back:b6,start:b7");
        if (nb >= 10) n += snprintf(map + n, sizeof map - (size_t)n, ",leftstick:b8,rightstick:b9");
        if (na >= 4)  n += snprintf(map + n, sizeof map - (size_t)n, ",rightx:a2,righty:a3");
        if (na >= 6)  n += snprintf(map + n, sizeof map - (size_t)n, ",lefttrigger:a4,righttrigger:a5");
        if (nh >= 1)  n += snprintf(map + n, sizeof map - (size_t)n, ",dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8");
        ok = SDL_GameControllerAddMapping(map) >= 0;
        fprintf(stderr, "[cellPad] \"%s\" has no controller mapping: a generic layout (%d axes, %d buttons, %d hats)%s; "
                        "a line for it in gamecontrollerdb.txt next to RaCACiT.exe sets the real one%c",
                name ? name : "?", na, nb, nh, ok ? "" : " could not be applied", 10);
    }
    SDL_JoystickClose(j);
    return ok && SDL_IsGameController(d);
}

/* Controllers take the first free port in the order they arrive, whatever
 * SDL's device index -- a wheel, a tablet or a virtual device at index 0 used
 * to leave port 0 (the one the game reads) empty. A controller that goes away
 * lets the ones after it move up, so the player always has port 0. Scanned
 * when the device count changes, and once a second. */
static void pad_sdl_attach(void)
{
    static int last_n = -1; static Uint64 next;
    const int n = SDL_NumJoysticks();
    const Uint64 now = SDL_GetTicks64();
    if (n == last_n && now < next) return;
    last_n = n; next = now + 1000;
    for (int pass = 0; pass < 2; pass++) {            /* mapped controllers first, then a generic guess */
        for (int d = 0; d < n; d++) {
            const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(d);
            int open = 0, port = -1;
            for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
                if (s_sdl_controllers[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_sdl_controllers[i])) == id) open = 1;
                if (!s_sdl_controllers[i] && port < 0) port = i;
            }
            if (open || port < 0) continue;
            if (!SDL_IsGameController(d)) {
                if (pass == 0 || s_sdl_controllers[0] || !pad_generic_mapping(d)) continue;
            }
            SDL_GameController* gc = SDL_GameControllerOpen(d);
            if (!gc) continue;
            s_sdl_controllers[port] = gc;
            pad_ds_setup(port, gc);   /* logs "controller <port>: <name>" */
        }
    }
}

/* Select on every controller: held 0.6 s it opens the settings menu (owner:
 * "no way to access the recomp settings menu" on an Xbox pad -- Windows' Game
 * Bar takes the Xbox button; v82: on the DualSense too, beside its touchpad).
 * So the game sees Select only once it is known to be a tap: on release, as a
 * short press. Held into the menu, the game never sees it. A press that starts
 * with the menu open is the menu's. */
#define PAD_SELECT_HOLD_MS 600
#define PAD_SELECT_TAP_MS  80
static int pad_select(int port, SDL_GameController* gc)
{
    const int down = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK);
    static int state[PAD_MAX_HOST_PORTS];             /* 0 up, 1 down (undecided), 2 down (the menu's) */
    static Uint64 since[PAD_MAX_HOST_PORTS], tap_until[PAD_MAX_HOST_PORTS];
    const Uint64 now = SDL_GetTicks64();
    if (down) {
        if (state[port] == 0) { state[port] = rsx_host_ui_capturing() ? 2 : 1; since[port] = now; }
        else if (state[port] == 1 && now - since[port] >= PAD_SELECT_HOLD_MS) { state[port] = 2; rsx_host_ui_request_menu(); }
    } else {
        if (state[port] == 1) tap_until[port] = now + PAD_SELECT_TAP_MS;
        state[port] = 0;
    }
    return now < tap_until[port];
}

static void pad_poll_sdl2(void)
{
    SDL_GameControllerUpdate();
    pad_sdl_attach();
    /* Detached controllers: closed, and the ones after them move up a port. */
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++)
        if (s_sdl_controllers[i] && !SDL_GameControllerGetAttached(s_sdl_controllers[i])) {
            SDL_GameControllerClose(s_sdl_controllers[i]);
            s_sdl_controllers[i] = NULL;
            for (int k = i; k + 1 < PAD_MAX_HOST_PORTS; k++) {
                s_sdl_controllers[k] = s_sdl_controllers[k + 1];
                if (s_sdl_controllers[k]) pad_ds_setup(k, s_sdl_controllers[k]);
            }
            s_sdl_controllers[PAD_MAX_HOST_PORTS - 1] = NULL;
            i--;
        }
    g_racacit_pad_layout = racacit_pad_layout();

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc) {   /* no controller: a neutral pad (nothing held from one just unplugged); touch and keys add to it */
            PadHostState* hs = &s_host_state[i];
            memset(hs, 0, sizeof *hs);
            hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
            hs->sensor_x = hs->sensor_z = hs->sensor_g = 512; hs->sensor_y = 399;
            continue;
        }

        s_host_state[i].connected = 1;

        u16 btns = 0;
        if (pad_select(i, gc))                                                    btns |= CELL_PAD_CTRL_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))     btns |= CELL_PAD_CTRL_L3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    btns |= CELL_PAD_CTRL_R3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))         btns |= CELL_PAD_CTRL_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))       btns |= CELL_PAD_CTRL_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    btns |= CELL_PAD_CTRL_RIGHT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     btns |= CELL_PAD_CTRL_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     btns |= CELL_PAD_CTRL_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  btns |= CELL_PAD_CTRL_L1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btns |= CELL_PAD_CTRL_R1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))             btns |= CELL_PAD_CTRL_TRIANGLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))             btns |= CELL_PAD_CTRL_CIRCLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))             btns |= CELL_PAD_CTRL_CROSS;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))             btns |= CELL_PAD_CTRL_SQUARE;

        /* Triggers via axis */
        int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 3000) btns |= CELL_PAD_CTRL_L2;
        if (rt > 3000) btns |= CELL_PAD_CTRL_R2;
        /* Fire on R2 (pad_swap): R1 and R2 trade places -- R2 fires (the
         * game's R1), R1 throws the wrench, boosts, drops from ledges (the
         * game's R2). The left side stays as on the PS3 since v70: L2 strafes,
         * L1 looks / scopes / centres the camera (v52-v69 traded L1 and L2
         * too, which put strafe on L1; owner, v69: "strafe button being l2
         * ... look at l1"). Two places where L2 does more:
         *   - quick select open: L2 / R2 page back / forward (the game's L1 /
         *     R1; "l2 and r2 for quick select");
         *   - on hoverboots: L2 accelerates too (the game's R2; "hoverboot l2").
         * game/button_prompts.cpp names the buttons in the text to match. */
        /* The quick select ring is open while TRIANGLE is held (the game's
         * default; a tap does not open it): held 150 ms = open. */
        if (i == 0) { static Uint64 tri_ms;
            if (!(btns & CELL_PAD_CTRL_TRIANGLE)) { tri_ms = 0; g_racacit_ctx_quickselect = 0; }
            else { const Uint64 nw = SDL_GetTicks64(); if (!tri_ms) tri_ms = nw;
                   g_racacit_ctx_quickselect = nw - tri_ms >= 150; } }
        if (pad_swap(gc)) {
            const int l1 = (btns & CELL_PAD_CTRL_L1) != 0, l2 = (btns & CELL_PAD_CTRL_L2) != 0;
            const int r1 = (btns & CELL_PAD_CTRL_R1) != 0, r2 = (btns & CELL_PAD_CTRL_R2) != 0;
            btns &= (u16)~(CELL_PAD_CTRL_L1 | CELL_PAD_CTRL_L2 | CELL_PAD_CTRL_R1 | CELL_PAD_CTRL_R2);
            int gl1 = l1, gl2 = l2, gr2 = r1;
            if (g_racacit_ctx_quickselect) { gl1 = l1 || l2; gl2 = 0; }
            else if (g_racacit_ctx_hoverboots && l2) { gr2 = 1; gl2 = 0; }
            if (gl1) btns |= CELL_PAD_CTRL_L1;
            if (gl2) btns |= CELL_PAD_CTRL_L2;
            if (r2) btns |= CELL_PAD_CTRL_R1;
            if (gr2) btns |= CELL_PAD_CTRL_R2;
            lt = gl2 ? lt : 0;
            rt = gr2 ? 32767 : 0;
        }

        s_host_state[i].buttons = btns;

        /* Analog sticks */
        s_host_state[i].analog_lx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        s_host_state[i].analog_ly = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
        s_host_state[i].analog_rx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
        s_host_state[i].analog_ry = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));

        /* Triggers */
        s_host_state[i].trigger_l2 = pad_sdl_trigger_to_u8(lt);
        s_host_state[i].trigger_r2 = pad_sdl_trigger_to_u8(rt);

        /* Pressure: SDL has digital buttons, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
        pad_ds_motion(i, gc);
        const int ds_plus = SDL_GameControllerGetType(gc) == SDL_CONTROLLER_TYPE_PS5 && pad_ds_plus();
        if (ds_plus) pad_ds_apply(i, gc, 0);
        if (ds_plus && pad_swap(gc)) { dsw_poll(i, gc); if (i == 0 && g_racacit_ds_pad) ds5_readback(gc); }
        if (!s_dsw_events && s_kick[i] && SDL_GetTicks64() - s_kick_ms[i] > 150) {   /* the game stopped rumbling: the weapon trigger back */
            s_kick[i] = 0;
            Uint8 st[47]; memset(st, 0, sizeof st);
            st[0] = 0x04;
            ds5_weapon(&st[10], 3, 6, 8);
            ds5_send(gc, st);
        }
    }
    pad_poll_sdl_fallbacks();
}

/* Keyboard and mouse on port 0, alongside a controller (whichever moves wins
 * per stick; buttons add up). Bindings are the game's buttons by name,
 * RACACIT_BIND_<BUTTON>=<keys> in settings.txt: SDL key names ("W", "Space",
 * "Left Shift", "Return", "F"), Mouse1-Mouse5, WheelUp/WheelDown, several
 * separated by commas, "none" for nothing. Buttons: CROSS CIRCLE SQUARE
 * TRIANGLE L1 R1 L2 R2 L3 R3 START SELECT UP DOWN LEFT RIGHT, the stick
 * directions LS_UP LS_DOWN LS_LEFT LS_RIGHT (and RS_* for the camera keys),
 * and WALK (held: the left stick at half). The mouse turns the camera once it
 * is captured (click in the window; rsx_host_ui.c). On/off, mouse speed and
 * invert are in Settings > Controls (RACACIT_KBM, RACACIT_MOUSE_SENS,
 * RACACIT_MOUSE_INVERT_Y, live); PAD_NO_KEYBOARD=1 turns it off for the run.
 * Keys pressed pause scripted test input like a controller does. */
enum { KB_CROSS, KB_CIRCLE, KB_SQUARE, KB_TRIANGLE, KB_L1, KB_R1, KB_L2, KB_R2, KB_L3, KB_R3, KB_START, KB_SELECT,
       KB_UP, KB_DOWN, KB_LEFT, KB_RIGHT, KB_LS_UP, KB_LS_DOWN, KB_LS_LEFT, KB_LS_RIGHT,
       KB_RS_UP, KB_RS_DOWN, KB_RS_LEFT, KB_RS_RIGHT, KB_WALK, KB_N };
static const struct { const char* name; u16 btn; const char* def; } k_kb[KB_N] = {
    { "CROSS",    CELL_PAD_CTRL_CROSS,    "Space,Return" },
    { "CIRCLE",   CELL_PAD_CTRL_CIRCLE,   "E,Backspace" },
    { "SQUARE",   CELL_PAD_CTRL_SQUARE,   "F" },
    { "TRIANGLE", CELL_PAD_CTRL_TRIANGLE, "Q" },
    { "L1",       CELL_PAD_CTRL_L1,       "Mouse3" },          /* strafe / aim (right mouse button) */
    { "R1",       CELL_PAD_CTRL_R1,       "Mouse1" },          /* fire (left mouse button) */
    { "L2",       CELL_PAD_CTRL_L2,       "Left Ctrl" },
    { "R2",       CELL_PAD_CTRL_R2,       "Left Shift" },
    { "L3",       CELL_PAD_CTRL_L3,       "V" },
    { "R3",       CELL_PAD_CTRL_R3,       "Mouse2,B" },        /* middle mouse button */
    { "START",    CELL_PAD_CTRL_START,    "Escape,P" },
    { "SELECT",   CELL_PAD_CTRL_SELECT,   "Tab,M" },
    { "UP",       CELL_PAD_CTRL_UP,       "Up,1" },
    { "DOWN",     CELL_PAD_CTRL_DOWN,     "Down,3" },
    { "LEFT",     CELL_PAD_CTRL_LEFT,     "Left,4" },
    { "RIGHT",    CELL_PAD_CTRL_RIGHT,    "Right,2" },
    { "LS_UP",    0, "W" },
    { "LS_DOWN",  0, "S" },
    { "LS_LEFT",  0, "A" },
    { "LS_RIGHT", 0, "D" },
    { "RS_UP",    0, "I" },
    { "RS_DOWN",  0, "K" },
    { "RS_LEFT",  0, "J" },
    { "RS_RIGHT", 0, "L" },
    { "WALK",     0, "Left Alt" },
};
/* An input: a scancode (< 512), or 512 + mouse button 1-5, or 520/521 the wheel. */
#define KB_MAX_KEYS 4
static int s_kb_keys[KB_N][KB_MAX_KEYS];
static int s_kbm_on = -1;             /* allowed this run (PAD_NO_KEYBOARD) */
extern volatile int g_racacit_kbm_on;   /* rsx_host_ui.c: capture the mouse on a click */

static int kb_parse_one(const char* t)
{
    if (!SDL_strcasecmp(t, "none") || !*t) return -1;
    if (!SDL_strncasecmp(t, "Mouse", 5) && t[5] >= '1' && t[5] <= '5' && !t[6]) return 512 + (t[5] - '0');
    if (!SDL_strcasecmp(t, "WheelUp")) return 520;
    if (!SDL_strcasecmp(t, "WheelDown")) return 521;
    const SDL_Scancode sc = SDL_GetScancodeFromName(t);
    return sc == SDL_SCANCODE_UNKNOWN ? -1 : (int)sc;
}
static void kb_init(int ignore_real)
{
    if (s_kbm_on >= 0) return;
    const char* nk = getenv("PAD_NO_KEYBOARD");
    s_kbm_on = !(nk && *nk && *nk != '0');
    for (int k = 0; k < KB_N; k++) {
        char env[48], list[256];
        snprintf(env, sizeof env, "RACACIT_BIND_%s", k_kb[k].name);
        const char* v = getenv(env);
        snprintf(list, sizeof list, "%s", v && *v ? v : k_kb[k].def);
        for (int i = 0; i < KB_MAX_KEYS; i++) s_kb_keys[k][i] = -1;
        int n = 0;
        for (char* tok = list; tok && *tok && n < KB_MAX_KEYS; ) {
            char* c = strchr(tok, ',');
            if (c) *c = 0;
            while (*tok == ' ') tok++;
            char* end = tok + strlen(tok); while (end > tok && end[-1] == ' ') *--end = 0;
            const int key = kb_parse_one(tok);
            if (key >= 0) s_kb_keys[k][n++] = key;
            else if (*tok && SDL_strcasecmp(tok, "none")) fprintf(stderr, "[cellPad] %s: unknown key \"%s\"%c", env, tok, 10);
            tok = c ? c + 1 : NULL;
        }
    }
    if (s_kbm_on && !ignore_real) fprintf(stderr, "[cellPad] keyboard and mouse ready (click in the window for the mouse camera; Settings > Controls)%c", 10);
}

/* After the controllers (and PAD_IGNORE_REAL, under which only the test
 * hook's keys count: rsx_host_ui_test_key). */
static void pad_poll_kbm(int ignore_real)
{
    kb_init(ignore_real);
    rsx_host_options opt; rsx_host_ui_options(&opt);
    g_racacit_kbm_on = s_kbm_on && opt.kbm && !ignore_real;
    if (!s_kbm_on || !opt.kbm || !SDL_WasInit(SDL_INIT_VIDEO)) return;
    int dx = 0, dy = 0, wheel = 0; unsigned mb = 0;
    const int captured = rsx_host_ui_mouse_take(&dx, &dy, &wheel, &mb);
    /* Keys only while the game window has the keyboard: typing elsewhere does
     * not play, and nothing stays held across an Alt+Tab. */
    const Uint8* ks = !ignore_real && SDL_GetKeyboardFocus() ? SDL_GetKeyboardState(NULL) : NULL;
    const int test = rsx_host_ui_test_key(-1);
    int held[KB_N] = { 0 };
    if (ks || test)
        for (int k = 0; k < KB_N; k++)
            for (int i = 0; i < KB_MAX_KEYS && s_kb_keys[k][i] >= 0; i++) {
                const int key = s_kb_keys[k][i];
                if (test && rsx_host_ui_test_key(key)) held[k] = 1;
                else if (key < 512) { if (ks && ks[key]) held[k] = 1; }
                else if (key < 520) { if (mb & (1u << (key - 513))) held[k] = 1; }
                else if ((key == 520 && wheel > 0) || (key == 521 && wheel < 0)) held[k] = 1;
            }
    PadHostState* hs = &s_host_state[0];
    hs->connected = 1;   /* with no controller the keyboard is the pad (pad_poll_sdl2 left it neutral) */
    u16 btns = 0;
    for (int k = 0; k < KB_N; k++) if (held[k] && k_kb[k].btn) btns |= k_kb[k].btn;
    hs->buttons |= btns;
    if (btns & CELL_PAD_CTRL_L2) hs->trigger_l2 = 255;
    if (btns & CELL_PAD_CTRL_R2) hs->trigger_r2 = 255;
    #define KB_PRESS(f, b) if (btns & (b)) hs->f = 255
    KB_PRESS(press_cross, CELL_PAD_CTRL_CROSS); KB_PRESS(press_circle, CELL_PAD_CTRL_CIRCLE);
    KB_PRESS(press_square, CELL_PAD_CTRL_SQUARE); KB_PRESS(press_triangle, CELL_PAD_CTRL_TRIANGLE);
    KB_PRESS(press_up, CELL_PAD_CTRL_UP); KB_PRESS(press_down, CELL_PAD_CTRL_DOWN);
    KB_PRESS(press_left, CELL_PAD_CTRL_LEFT); KB_PRESS(press_right, CELL_PAD_CTRL_RIGHT);
    KB_PRESS(press_l1, CELL_PAD_CTRL_L1); KB_PRESS(press_r1, CELL_PAD_CTRL_R1);
    #undef KB_PRESS
    /* Left stick: the movement keys, diagonals at the same length, WALK = half. */
    {
        float x = (float)(held[KB_LS_RIGHT] - held[KB_LS_LEFT]), y = (float)(held[KB_LS_DOWN] - held[KB_LS_UP]);
        if (x || y) {
            const float len = sqrtf(x * x + y * y), m = held[KB_WALK] ? 0.5f : 1.0f;
            hs->analog_lx = (u8)(128.0f + 127.0f * m * x / len);
            hs->analog_ly = (u8)(128.0f + 127.0f * m * y / len);
        }
    }
    /* Right stick: the mouse, else the camera keys. The mouse's speed over the
     * poll becomes a deflection, starting past the game's stick dead zone so
     * a slow move still turns; smoothed over a few polls (mouse reports come
     * in bursts). */
    {
        static Uint64 last; static float sx, sy;
        const Uint64 now = SDL_GetPerformanceCounter();
        float dt = last ? (float)((double)(now - last) * 1000.0 / (double)SDL_GetPerformanceFrequency()) : 16.0f;   /* ms */
        last = now;
        if (dt < 1.0f) dt = 1.0f;
        if (dt > 50.0f) dt = 50.0f;
        float vx = 0.0f, vy = 0.0f;
        if (captured && (dx || dy)) {
            const float k = 0.11f * (float)opt.mouse_sens / 100.0f, dz = 0.22f;
            float ax = fabsf((float)dx) / dt * k, ay = fabsf((float)dy) / dt * k;
            if (ax > 0.0f) ax = dz + (1.0f - dz) * (ax > 1.0f ? 1.0f : ax);
            if (ay > 0.0f) ay = dz + (1.0f - dz) * (ay > 1.0f ? 1.0f : ay);
            vx = dx < 0 ? -ax : ax; vy = dy < 0 ? -ay : ay;
            if (opt.mouse_invert) vy = -vy;
        }
        sx += (vx - sx) * 0.6f; sy += (vy - sy) * 0.6f;
        if (fabsf(sx) < 0.02f) sx = 0.0f;
        if (fabsf(sy) < 0.02f) sy = 0.0f;
        float rx = sx, ry = sy;
        const float kx = (float)(held[KB_RS_RIGHT] - held[KB_RS_LEFT]), ky = (float)(held[KB_RS_DOWN] - held[KB_RS_UP]);
        if (!rx && !ry && (kx || ky)) { rx = kx * 0.8f; ry = ky * 0.8f; }
        if (rx || ry) {
            hs->analog_rx = (u8)(128.0f + 127.0f * (rx > 1.0f ? 1.0f : rx < -1.0f ? -1.0f : rx));
            hs->analog_ry = (u8)(128.0f + 127.0f * (ry > 1.0f ? 1.0f : ry < -1.0f ? -1.0f : ry));
        }
    }
}

static void pad_poll_sdl_fallbacks(void)
{
    /* On-screen touch controls drive port 0 while no gamepad is attached. */
    touch_controls_set_gamepad(s_sdl_controllers[0] != NULL);
    if (touch_controls_active()) {
        static float s_aspect = 0.0f;
        if (s_aspect <= 0.0f) {
            SDL_Rect r;
            if (SDL_GetDisplayBounds(0, &r) == 0 && r.h > 0) {
                s_aspect = (float)r.w / (float)r.h;
                if (s_aspect < 1.0f) s_aspect = 1.0f / s_aspect;   /* landscape */
            } else s_aspect = 16.0f / 9.0f;
        }
        PadHostState* hs = &s_host_state[0];
        u16 b = 0; u8 lx = 128, ly = 128, rx = 128, ry = 128;
        touch_controls_poll(s_aspect, &b, &lx, &ly, &rx, &ry);
        hs->buttons = b;
        hs->analog_lx = lx; hs->analog_ly = ly; hs->analog_rx = rx; hs->analog_ry = ry;
        hs->trigger_l2 = (b & CELL_PAD_CTRL_L2) ? 255 : 0;
        hs->trigger_r2 = (b & CELL_PAD_CTRL_R2) ? 255 : 0;
        hs->press_cross    = (b & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        hs->press_circle   = (b & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        hs->press_square   = (b & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        hs->press_triangle = (b & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        hs->press_up = (b & CELL_PAD_CTRL_UP) ? 255 : 0;  hs->press_down  = (b & CELL_PAD_CTRL_DOWN)  ? 255 : 0;
        hs->press_left = (b & CELL_PAD_CTRL_LEFT) ? 255 : 0; hs->press_right = (b & CELL_PAD_CTRL_RIGHT) ? 255 : 0;
        hs->press_l1 = (b & CELL_PAD_CTRL_L1) ? 255 : 0;  hs->press_r1 = (b & CELL_PAD_CTRL_R1) ? 255 : 0;
    }
}

/* Extra controller mappings: gamecontrollerdb.txt (SDL's format, e.g. the
 * community SDL_GameControllerDB file) next to the exe, in the working folder,
 * or RACACIT_CONTROLLER_DB=<file>. SDL_GAMECONTROLLERCONFIG works too. SDL
 * already knows Xbox, PlayStation, Switch, Stadia, Steam and many more. */
static void pad_load_mappings(void)
{
    char path[1024]; int n = 0;
    const char* e = getenv("RACACIT_CONTROLLER_DB");
    if (e && *e) n += SDL_GameControllerAddMappingsFromFile(e) > 0;
    char* base = SDL_GetBasePath();
    if (base) { snprintf(path, sizeof path, "%sgamecontrollerdb.txt", base); SDL_free(base);
                const int k = SDL_GameControllerAddMappingsFromFile(path); if (k > 0) { n++; fprintf(stderr, "[cellPad] %d controller mappings from %s%c", k, path, 10); } }
    const int k = SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
    if (k > 0) fprintf(stderr, "[cellPad] %d controller mappings from gamecontrollerdb.txt%c", k, 10);
    (void)n;
}

static void pad_init_backend(void)
{
    /* Face buttons by position on every controller: the bottom one is Cross,
     * as on a PlayStation pad -- also on Nintendo pads, whose labels are the
     * other way round. Before any controller is opened. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    /* Xbox pads (and Steam Input's virtual ones) through XInput, not Raw
     * Input: Raw Input delivers buttons as WM_INPUT to a helper window of the
     * thread that started the subsystem -- this guest thread, which never
     * pumps messages -- so an Xbox pad read its sticks and triggers (polled
     * XInput/WGI) but no button, ever (owner, v79/0.8: "not working at all";
     * scripts/dev/pad_probe: 0 button changes this way, buttons with
     * SDL_JOYSTICK_RAWINPUT=0). XInput is polled, from any thread. Before the
     * subsystem starts; rsx_vulkan_backend.c vk_window_open sets it too. */
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    /* Controllers are read here only, by this poll (SDL_GameControllerUpdate):
     * SDL otherwise reads them in every SDL_PollEvent too, and the window's
     * event pump runs on the FIFO walker -- the thread that feeds the GPU did
     * HID reads and waited on SDL's joystick lock against this one (owner,
     * 0.9.6: a 0.4-1.4 s freeze with the walker in the pump, waiting in the
     * kernel). Events (the settings menu's) still come from this poll. The
     * discovery window is this thread's, so hotplug is unchanged. */
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_SENSORS, "0");
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
        s_sdl_inited = 1;
        pad_load_mappings();
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));
    pad_sdl_attach();
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (s_sdl_controllers[i]) {
            SDL_GameControllerClose(s_sdl_controllers[i]);
            s_sdl_controllers[i] = NULL;
        }
    }
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

/* Re-poll the host at most every PAD_POLL_INTERVAL_MS; serve cached state in
 * between.
 *
 * XInputGetState is a driver call, and on a slot with nothing plugged in it is
 * an expensive one: measured at 97.6 us for a sweep of the 7 host ports on a
 * machine with no controller attached. cellPadGetData ran that sweep on EVERY
 * call, and a guest that polls the pad from a spin loop -- The Simpsons Arcade
 * Game does, in its input stage -- therefore spends its whole main thread
 * inside the input driver.
 *
 * 4 ms is 250 Hz, well above the 60 Hz a title can actually observe (and above
 * a real DualShock 3's own report rate), so no game can tell the difference,
 * including one sampling for button-press edges. */
#define PAD_POLL_INTERVAL_MS 4

static unsigned long long pad_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

/* A person picked up the controller: scripted test input (PAD_FILE,
 * PAD_SCRIPT, PAD_STICK, PAD_SWEEP, PAD_AUTOPRESS) stands down until the
 * real pads have been left alone for 20 s, so a test run never steers the
 * player into a wall. Only ports with a real device count -- injected stick
 * values sit in the same host state. */
#define PAD_HUMAN_HOLD_MS 20000ull
static unsigned long long s_human_ms;
static void pad_note_human(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        const PadHostState* hs = &s_host_state[i];
        if (!hs->connected) continue;
        if (hs->buttons || hs->trigger_l2 > 30 || hs->trigger_r2 > 30 ||
            abs((int)hs->analog_lx - 128) > 40 || abs((int)hs->analog_ly - 128) > 40 ||
            abs((int)hs->analog_rx - 128) > 40 || abs((int)hs->analog_ry - 128) > 40) {
            const unsigned long long now = pad_now_ms();
            if (!s_human_ms || now - s_human_ms > PAD_HUMAN_HOLD_MS) {
                printf("[cellPad] controller in use -- scripted input paused (buttons %04X, L2 %d R2 %d, sticks %d,%d %d,%d)\n",
                       (unsigned)hs->buttons, hs->trigger_l2, hs->trigger_r2, hs->analog_lx, hs->analog_ly,
                       hs->analog_rx, hs->analog_ry);
                fflush(stdout);
            }
            s_human_ms = now;
            return;
        }
    }
}
static int pad_scripted_ok(void)
{
    return !s_human_ms || pad_now_ms() - s_human_ms > PAD_HUMAN_HOLD_MS;
}

/* While the host settings menu is open (libs/video/rsx_host_ui.h) the game
 * sees an idle pad, and keeps seeing one after the menu closes until every
 * button is up: the Cross that chose "Resume" must not also jump. */
extern int rsx_host_ui_capturing(void);
static void pad_ui_capture(void)
{
    static int hold = 0;
    const int cap = rsx_host_ui_capturing();
    if (cap) hold = 1;
    if (!hold) return;
    int any = 0;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        PadHostState* hs = &s_host_state[i];
        if (hs->buttons || hs->trigger_l2 > 30 || hs->trigger_r2 > 30) any = 1;
        hs->buttons = 0;
        hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
        hs->trigger_l2 = hs->trigger_r2 = 0;
        hs->press_right = hs->press_left = hs->press_up = hs->press_down = 0;
        hs->press_triangle = hs->press_circle = hs->press_cross = hs->press_square = 0;
        hs->press_l1 = hs->press_r1 = 0;
    }
    if (!cap && !any) hold = 0;
}

static void pad_poll_backend(void)
{
    /* Benign race: two threads may poll in the same tick. That costs one extra
     * sweep, never a missed or stale-forever read. */
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    s_last_ms = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
#if PAD_BACKEND_XINPUT
    if (!s_host_state[0].connected) pad_poll_keyboard();
#endif
    /* PAD_IGNORE_REAL=1 (agent test runs, scripts/dev/play.sh): the real
     * controller reads as idle, so scripted input drives alone -- a pad left
     * lying on its sticks (2026-10-22: both pushed fully over) held every
     * test run at the title. */
    static int ign = -1;
    if (ign < 0) { const char* e = getenv("PAD_IGNORE_REAL"); ign = e && *e == '1'; }
    { if (ign) for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
          const int c = s_host_state[i].connected;
          memset(&s_host_state[i], 0, sizeof s_host_state[i]);
          s_host_state[i].connected = c;
          s_host_state[i].analog_lx = s_host_state[i].analog_ly = s_host_state[i].analog_rx = s_host_state[i].analog_ry = 128;
          s_host_state[i].sensor_x = s_host_state[i].sensor_y = s_host_state[i].sensor_z = s_host_state[i].sensor_g = 512;
      } }
#if PAD_BACKEND_SDL2
    pad_poll_kbm(ign);
#endif
    pad_note_human();
    pad_ui_capture();
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    /* The bound is the 127-slot pad table, not the 7 ports: larger requests
     * are accepted and clamped. Resistance: Fall of Man asks for 8; rejected,
     * the library stayed closed, every GetInfo failed and the title never
     * polled the pad -- stuck on "Press the START button". */
    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));
    /* Sticks rest at 128, not 0: a zeroed state is both sticks hard up-left,
     * which with no host controller read as a constant deflection -- menus
     * scrolled on their own (A Crack in Time's main menu and subtitle prompt
     * cycled every few frames). */
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        s_host_state[i].analog_lx = s_host_state[i].analog_ly = 128;
        s_host_state[i].analog_rx = s_host_state[i].analog_ry = 128;
    }

    pad_init_backend();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_shutdown_backend();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. The
 * vm_write helpers from ppu_memory.h translate + byte-swap. (cellPad was
 * written for host pointers and was never actually invoked until its NIDs
 * were fixed.) These used to be redeclared here with a 64-bit address
 * parameter, which conflicts with the header now that it is included. */

/* START presses seen on port 0 (see the latch at the end of cellPadGetData).
 * ps3_pad_take_start_press() reports whether one happened since the last call. */
static volatile unsigned g_pad_start_presses, g_pad_start_taken;
int ps3_pad_take_start_press(void)
{
    unsigned n = __atomic_load_n(&g_pad_start_presses, __ATOMIC_RELAXED);
    if (n == g_pad_start_taken) return 0;
    g_pad_start_taken = n;
    return 1;
}
/* Forget any press so far (a feature arming itself ignores older presses). */
void ps3_pad_clear_start_press(void) { g_pad_start_taken = __atomic_load_n(&g_pad_start_presses, __ATOMIC_RELAXED); }
/* ms (ps3_qpc_us / 1000) when port 0's fire button (R1 as the title sees it)
 * last went down; 0 = never. */
volatile unsigned long long g_racacit_fire_ms;
volatile int g_racacit_fire_held;                     /* ...and whether it is down now */

/* MEM_FILE=<path> (dev tool, next to PAD_FILE): guest-memory commands, one
 * per line; the file is emptied once read, results are [mem] log lines.
 *   dump <host file> [lo hi]  every committed, readable guest range in
 *                             [lo, hi) (default 0 - 0x40000000) into one file:
 *                             per range u32 guest address, u32 size (host
 *                             order), then the bytes as the guest sees them
 *   peek <addr> [n]           n big-endian words, hex and as floats
 *   poke <addr> <u32>         write a big-endian word
 *   pokef <addr> <float>      write a big-endian float
 *   freeze <addr> <u32>       write it again on every poll (16 at most);
 *   freeze off                clears them all
 * For gameplay repros: find Ratchet's position or health with
 * scripts/dev/memscan.py over dumps, then move him or keep him alive. */
static void pad_mem_write32(u32 a, u32 v)
{
    uint8_t* p = vm_base + a;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static u32 pad_mem_read32(u32 a)
{
    const uint8_t* p = vm_base + a;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static void pad_mem_commands(void)
{
    static int on = -1; static char path[512];
    static u32 frz_a[16], frz_v[16]; static int frz_n;
    static volatile long busy;
    if (on < 0) { const char* e = getenv("MEM_FILE"); on = e && *e;
                  if (on) { strncpy(path, e, sizeof path - 1); path[sizeof path - 1] = 0; } }
    if (!on || __sync_lock_test_and_set(&busy, 1)) return;
    for (int i = 0; i < frz_n; i++) pad_mem_write32(frz_a[i], frz_v[i]);
    char line[600] = "";
    FILE* f = fopen(path, "rb");
    if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f);
             if (line[0] && (f = fopen(path, "wb"))) fclose(f); }
    char cmd[16] = "", arg[520] = ""; unsigned a = 0, b = 0;
    if (line[0] && sscanf(line, "%15s", cmd) == 1) {
        if (!strcmp(cmd, "peek") && sscanf(line, "%*s %i %i", (int*)&a, (int*)&b) >= 1) {
            if (b == 0 || b > 64) b = b ? 64 : 1;
            for (u32 i = 0; i < b; i++) { const u32 v = pad_mem_read32(a + i * 4); float fl; memcpy(&fl, &v, 4);
                printf("[mem] %08X: %08X %g\n", a + i * 4, v, fl); }
        } else if (!strcmp(cmd, "poke") && sscanf(line, "%*s %i %i", (int*)&a, (int*)&b) == 2) {
            pad_mem_write32(a, b); printf("[mem] poke %08X = %08X\n", a, b);
        } else if (!strcmp(cmd, "pokef") && sscanf(line, "%*s %i %519s", (int*)&a, arg) == 2) {
            const float fl = strtof(arg, NULL); u32 v; memcpy(&v, &fl, 4);
            pad_mem_write32(a, v); printf("[mem] poke %08X = %g\n", a, fl);
        } else if (!strcmp(cmd, "freeze")) {
            if (strstr(line, "off")) { frz_n = 0; printf("[mem] freeze off\n"); }
            else if (sscanf(line, "%*s %i %i", (int*)&a, (int*)&b) == 2 && frz_n < 16) {
                frz_a[frz_n] = a; frz_v[frz_n++] = b; printf("[mem] freeze %08X = %08X\n", a, b); }
        } else if (!strcmp(cmd, "dump") && sscanf(line, "%*s %519s", arg) == 1) {
            unsigned lo = 0, hi = 0x40000000u;
            sscanf(line, "%*s %*s %i %i", (int*)&lo, (int*)&hi);
#ifdef _WIN32
            FILE* o = fopen(arg, "wb"); unsigned n = 0; unsigned long long bytes = 0;
            for (unsigned long long g = lo; o && g < hi; ) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + g, &mi, sizeof mi)) break;
                const unsigned long long end = (unsigned long long)((uint8_t*)mi.BaseAddress - vm_base) + mi.RegionSize;
                const unsigned long long e = end < hi ? end : hi;
                const DWORD pr = mi.Protect & 0xFF;
                if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && pr != PAGE_NOACCESS && pr != PAGE_EXECUTE) {
                    const u32 ga = (u32)g, sz = (u32)(e - g);
                    fwrite(&ga, 4, 1, o); fwrite(&sz, 4, 1, o); fwrite(vm_base + g, 1, sz, o);
                    n++; bytes += sz;
                }
                g = e;
            }
            if (o) fclose(o);
            printf("[mem] dump %s: %u ranges, %llu MB\n", arg, n, bytes >> 20);
#else
            printf("[mem] dump: Windows only\n");
#endif
        } else printf("[mem] ? %s", line);
        fflush(stdout);
    }
    __sync_lock_release(&busy);
}

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();
    if (port_no == 0) pad_mem_commands();

    /* YDKJ_INJECT_PAD: the game's wait loop polls the pad (cellPadSetActDirect /
     * GetData) -- if it's parked on a "press button" prompt after loading, no
     * host input means it waits forever. Inject a real button PULSE (CROSS+START+
     * CIRCLE) so it advances. Legit input simulation, not forged pixels. */
    /* NOTE: cache getenv() -- this runs on every poll from multiple threads, and the
     * uncached version crashed deterministically (varying fault address = race). Every
     * other probe in this tree uses the cached `static int x=-1` pattern; match it. */
    static int  s_inj   = -1;
    static u16  s_btn   = 0x0008u;   /* default START */
    static long s_delay = 3000;
    if (s_inj < 0) {
        s_inj = getenv("YDKJ_INJECT_PAD") ? 1 : 0;
        const char* be = getenv("YDKJ_PAD_BTN");   if (be) s_btn   = (u16)strtoul(be,0,0);
        const char* de = getenv("YDKJ_PAD_DELAY"); if (de) s_delay = strtol(de,0,0);
    }
    /* Gate on WALL-CLOCK, not poll count: the game polls the pad hundreds of thousands
     * of times inside an early wait loop, so any count-based delay fires during init
     * (crash: guest ctr=0x0005F8A0 r3=0 -- button handler on an unconstructed object).
     * YDKJ_PAD_DELAY is now SECONDS to wait before the first injected press. */
    if (s_inj && port_no < PAD_MAX_HOST_PORTS) {
        static long _pc = 0; _pc++;
        /* Cheap counter gate FIRST: clock() is a slow call and this poll loop is hammered
         * by the guest from multiple threads; sampling it every poll perturbs timing. Only
         * consult the clock once every 256 polls, and never after arming. */
        static int     s_armed = 0;
        static clock_t s_t0    = 0;
        if (!s_armed) {
            if ((_pc & 0xFF) == 0) {
                if (!s_t0) s_t0 = clock();
                else if ((double)(clock() - s_t0) / (double)CLOCKS_PER_SEC >= (double)s_delay) s_armed = 1;
            }
            goto skip_inject;
        }
        u16 btn = s_btn; long delay = 0;
        /* Delay past init, then a single clean pulse every ~180 calls (press 15, release 165). */
        if (_pc > delay) {
            long ph = (_pc - delay) % 180;
            if (ph < 15) s_host_state[port_no].buttons |= btn;
            else         s_host_state[port_no].buttons &= ~btn;
            s_host_state[port_no].connected = 1;
        }
    }
skip_inject: ;

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; s_host_state[0] is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &s_host_state[port_no];
    u32 setting = s_port_setting[port_no];

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* button[0] is reserved and 0 on hardware; button[1] carries the pad mode
     * in its high nibble (0x7) and len/2 in its low one. GH3 drops any packet
     * whose button[0] is non-zero -- with len (8) there, every press was
     * received and thrown away, and "Press any button" never advanced. */
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(hs->buttons & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((hs->buttons >> 8) & 0xFF);

    /* PAD_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    { static int s_ap = -1; static unsigned s_apn = 0;
      if (s_ap < 0) s_ap = getenv("PAD_AUTOPRESS") ? 1 : 0;
      if (s_ap && port_no == 0 && pad_scripted_ok()) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
      } }

    /* PAD_SCRIPT="<sec>:<mask>,<sec>:<mask>,..." -- press a NAMED button at a
     * given wall-clock second, each held ~250 ms. PAD_AUTOPRESS only pulses
     * CROSS and START, which is enough to clear a "press start" screen but not
     * to navigate a menu: You Don't Know Jack needs DOWN to move off its name
     * field before CROSS means anything. Masks are CELL_PAD_CTRL_* packed as
     * (DIGITAL2 << 8) | DIGITAL1: START 0x0008, UP 0x0010, DOWN 0x0040,
     * LEFT 0x0080, RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000.
     * Legit input simulation on the same footing as PAD_AUTOPRESS/PAD_STICK. */
    { static int s_sc = -1;
      static struct { double t; unsigned mask; } ev[32]; static int n_ev = 0;
      static unsigned long long t0 = 0;   /* not ULONGLONG: this builds on POSIX too */
      if (s_sc < 0) {
          s_sc = 0;
          const char* e = getenv("PAD_SCRIPT");
          if (e && *e) {
              s_sc = 1; t0 = GetTickCount64();
              const char* p = e;
              while (*p && n_ev < 32) {
                  double t = strtod(p, (char**)&p);
                  if (*p == ':') p++;
                  unsigned m = (unsigned)strtoul(p, (char**)&p, 0);
                  ev[n_ev].t = t; ev[n_ev].mask = m; n_ev++;
                  while (*p == ',' || *p == ' ') p++;
              }
              printf("[cellPad] PAD_SCRIPT: %d event(s)\n", n_ev);
          }
      }
      if (s_sc && port_no == 0 && pad_scripted_ok()) {
          double now = (double)(GetTickCount64() - t0) / 1000.0;
          /* Latch, do not window. A fixed time window is missed whenever the
           * title stops polling the pad for longer than the window (it does
           * exactly that on a loading screen), so the press silently never
           * happens. Fire on the first poll at or after the event time and
           * hold it for a fixed number of polls, which is what a real press
           * looks like regardless of poll rate. */
          static int fired[32], held[32];
          for (int i = 0; i < n_ev; i++) {
              if (!fired[i] && now >= ev[i].t) { fired[i] = 1; held[i] = 40; }
              if (held[i] > 0) {
                  held[i]--;
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(ev[i].mask & 0xFF);
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((ev[i].mask >> 8) & 0xFF);
                  static int said[32];
                  if (!said[i]) { said[i] = 1;
                      printf("[cellPad] PAD_SCRIPT t=%.1fs press 0x%04X\n", ev[i].t, ev[i].mask); }
              }
          }
      } }

    /* PAD_FILE=<path> -- closed-loop input. Each poll, if the file holds a
     * button mask (same encoding as PAD_SCRIPT), press it for a fixed number
     * of polls and empty the file. A driver outside the process can then look
     * at the log or a frame dump, decide which screen the title is on, and
     * write the next button -- which a fixed time schedule cannot do, because
     * a menu system with several screens wanders when driven blind.
     *
     *   echo 0x0008 > pad.txt    # START
     *   echo 0x4000 > pad.txt    # CROSS
     *   echo 0x0040 4 > pad.txt  # DOWN for 4 polls (menus auto-repeat a
     *                            # long hold; the default is 40)
     */
    { static int s_pf = -1; static char pf_path[512];
      static unsigned held_mask = 0; static int held_n = 0;
      /* Optional sticks: "mask polls lx ly rx ry" (0-255, 128 = centred) holds
       * the analog sticks with the buttons -- walking needs the left stick.
       * A mask of 0 with a stick moves without pressing anything. */
      static int held_stick = 0; static unsigned char held_ax[4];
      /* "a ; b ; c": up to 8 steps run back to back with no release between
       * them -- a hold-then-select (TRIANGLE ring, then stick, then let go)
       * cannot survive the gap between two separate commands. */
      static char q_steps[8][40]; static int q_n = 0, q_i = 0;
      if (s_pf < 0) { const char* e = getenv("PAD_FILE");
                      s_pf = (e && *e) ? 1 : 0;
                      if (s_pf) { strncpy(pf_path, e, sizeof pf_path - 1); pf_path[sizeof pf_path - 1] = 0; } }
      if (s_pf && port_no == 0 && !pad_scripted_ok()) {
          /* A person has the pad: drop any hold, and take (ignore) the
           * driver's commands so it does not stall waiting on the file. */
          if (held_stick) hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
          held_n = 0; held_stick = 0;
          FILE* w = fopen(pf_path, "rb");
          if (w) { const int c = fgetc(w); fclose(w); if (c != EOF && (w = fopen(pf_path, "wb"))) fclose(w); }
      } else if (s_pf && port_no == 0) {
          if (held_n <= 0) {
              if (held_stick) {                 /* a stick hold just ended: centre */
                  held_stick = 0;
                  hs->analog_lx = hs->analog_ly = hs->analog_rx = hs->analog_ry = 128;
              }
              FILE* f = q_i < q_n ? NULL : fopen(pf_path, "rb");
              char buf[256]; buf[0] = 0;
              int have = 0;
              if (q_i < q_n) { strncpy(buf, q_steps[q_i++], sizeof buf - 1); buf[sizeof buf - 1] = 0; have = 1; }
              else if (f && fgets(buf, sizeof buf, f)) {
                  have = 1;
                  if (strchr(buf, ';')) {                       /* a sequence: queue the rest */
                      q_n = 0; q_i = 0;
                      for (char* t = strtok(buf, ";"); t && q_n < 8; t = strtok(NULL, ";")) {
                          strncpy(q_steps[q_n], t, sizeof q_steps[0] - 1); q_steps[q_n][sizeof q_steps[0] - 1] = 0; q_n++; }
                      if (q_n) { strncpy(buf, q_steps[0], sizeof buf - 1); buf[sizeof buf - 1] = 0; q_i = 1; }
                  }
              }
              if (have) {
                  {
                      int lx = -1, ly = 128, rx = 128, ry = 128;
                      unsigned m = 0; int n = 0;
                      const int got = sscanf(buf, "%i %i %i %i %i %i", (int*)&m, &n, &lx, &ly, &rx, &ry);
                      if (got >= 1 && (m || got >= 3)) {
                          held_mask = m; held_n = n > 0 ? n : 40;
                          held_stick = got >= 3;
                          if (held_stick) { held_ax[0] = (unsigned char)lx; held_ax[1] = (unsigned char)ly;
                                            held_ax[2] = (unsigned char)rx; held_ax[3] = (unsigned char)ry; }
                          printf("[cellPad] PAD_FILE press 0x%04X%s\n", m, held_stick ? " +stick" : ""); fflush(stdout); }
                  }
              }
              if (f) {
                  fclose(f);
                  if (held_n > 0) { FILE* w = fopen(pf_path, "wb"); if (w) fclose(w); }
              }
          }
          if (held_n > 0) { held_n--;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held_mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held_mask >> 8) & 0xFF);
              if (held_stick) {
                  hs->analog_lx = held_ax[0]; hs->analog_ly = held_ax[1];
                  hs->analog_rx = held_ax[2]; hs->analog_ry = held_ax[3];
                  hs->connected = 1;
              } }
      } }

#ifdef __ANDROID__
    /* The dual-screen companion's taps (game/android_companion.cpp): a short
     * queue of holds -- buttons plus optional sticks for N polls each -- on
     * port 0, merged over whatever the player is pressing. A quick-select tap
     * is TRIANGLE held with the left stick toward the slot, then released. */
    if (port_no == 0) {
        extern int racacit_companion_step(unsigned* mask, int* sticks, unsigned char ax[4]);
        unsigned cm = 0; int cs = 0; unsigned char cax[4];
        if (racacit_companion_step(&cm, &cs, cax)) {
            data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(cm & 0xFF);
            data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((cm >> 8) & 0xFF);
            if (cs) { hs->analog_lx = cax[0]; hs->analog_ly = cax[1]; hs->analog_rx = cax[2]; hs->analog_ry = cax[3]; }
        }
    }
#endif

    /* Analog sticks */
    /* PAD_STICK="lx,ly,rx,ry" (0-255, 128 = centred): hold the analog sticks at
     * fixed positions. Rubber Ducky drives its camera from the sticks and never
     * moves on its own, so with no host pad the view is frozen wherever it
     * started -- which can leave the subject of the demo off-screen or too far
     * away to make out. Legit input simulation, same footing as YDKJ_INJECT_PAD. */
    { static int s_st = -1;
      static unsigned char s_v[4] = {128,128,128,128};
      /* Optional 5th/6th fields: the seconds the hold starts and ends, so the menus
       * before gameplay are not driven by it (PAD_STICK="128,0,128,128,300,320"). */
      static double s_from = 0, s_until = 1e30; static unsigned long long s_t0 = 0;
      if (!s_t0) s_t0 = GetTickCount64();
      if (s_st < 0) { const char* e = getenv("PAD_STICK");
        s_st = e ? 1 : 0;
        if (e) { int a=128,b=128,c=128,d=128;
                 sscanf(e, "%d,%d,%d,%d,%lf,%lf", &a,&b,&c,&d,&s_from,&s_until);
                 s_v[0]=(unsigned char)a; s_v[1]=(unsigned char)b;
                 s_v[2]=(unsigned char)c; s_v[3]=(unsigned char)d; } }
      if (s_st && pad_scripted_ok() && (double)(GetTickCount64() - s_t0) / 1000.0 >= s_from && (double)(GetTickCount64() - s_t0) / 1000.0 < s_until) { hs->analog_lx = s_v[0]; hs->analog_ly = s_v[1];
                  hs->analog_rx = s_v[2]; hs->analog_ry = s_v[3];
                  hs->connected = 1; }
      /* PAD_SWEEP=<seconds per step>: walk the sticks through a fixed set of
       * deflections instead of holding one. Guessing a single stick position and
       * re-running costs ~5 minutes a try; a sweep answers "can input move the
       * camera anywhere useful" in one run, and the frame dumps say which step
       * paid off. Steps: centre, each left-stick direction, then each right. */
      { static int sw = -1; static clock_t t0 = 0;
        if (sw < 0) { const char* e = getenv("PAD_SWEEP");
                      sw = e ? atoi(e) : 0; if (sw < 0) sw = 0; }
        if (sw && pad_scripted_ok()) {
            static const unsigned char steps[9][4] = {
                {128,128,128,128},
                {128,  0,128,128}, {128,255,128,128},
                {  0,128,128,128}, {255,128,128,128},
                {128,128,128,  0}, {128,128,128,255},
                {128,128,  0,128}, {128,128,255,128},
            };
            if (!t0) t0 = clock();
            long el = (long)((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            int k = (int)((el / (sw > 0 ? sw : 1)) % 9);
            hs->analog_lx = steps[k][0]; hs->analog_ly = steps[k][1];
            hs->analog_rx = steps[k][2]; hs->analog_ry = steps[k][3];
            hs->connected = 1;
            { static int lastk = -1;
              if (k != lastk) { lastk = k;
                fprintf(stderr, "[PADSWEEP] step %d: l=(%u,%u) r=(%u,%u)%c", k,
                        steps[k][0], steps[k][1], steps[k][2], steps[k][3], 10); } }
        } } }
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
        /* A button whose digital bit is set reads full pressure. Injected
         * input (PAD_FILE/PAD_SCRIPT/PAD_AUTOPRESS, keyboard) only sets the
         * digital bits, and the host values above zeroed any pressure it had
         * written -- so a title keying its menus off the pressure words (GH3,
         * Tornado Outbreak's title screen) never saw the press. */
        static const struct { u8 word; u8 bit; u8 off; } pm[] = {
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x20, CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x80, CELL_PAD_BTN_OFFSET_PRESS_LEFT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x10, CELL_PAD_BTN_OFFSET_PRESS_UP },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x40, CELL_PAD_BTN_OFFSET_PRESS_DOWN },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x10, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x20, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x40, CELL_PAD_BTN_OFFSET_PRESS_CROSS },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x80, CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x04, CELL_PAD_BTN_OFFSET_PRESS_L1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x08, CELL_PAD_BTN_OFFSET_PRESS_R1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x01, CELL_PAD_BTN_OFFSET_PRESS_L2 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x02, CELL_PAD_BTN_OFFSET_PRESS_R2 } };
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] < 255)
                data->button[pm[k].off] = 255;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 399; /* gravity */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 512;
#if PAD_BACKEND_SDL2
        if (hs->has_motion) {                                 /* a DualSense / DualShock 4 */
            data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = hs->sensor_x;
            data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = hs->sensor_y;
            data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = hs->sensor_z;
            data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = hs->sensor_g;
        }
#endif
    }
    }  /* end connected block */

emit:
    /* START edge latch for host-side features (A Crack in Time's "Start skips
     * any cutscene"): count presses of START on port 0 as the title sees them. */
    if (port_no == 0) {
        static int s_prev_start;
        int now = (data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] & CELL_PAD_CTRL_START) != 0;
        if (now && !s_prev_start) __atomic_add_fetch(&g_pad_start_presses, 1, __ATOMIC_RELAXED);
        s_prev_start = now;
        /* ...and when fire (R1 as the title sees it) last went down, for the
         * DualSense routing's "what does this gun play" log (game/stubs.cpp). */
        static int s_prev_fire;
        const int fire = (data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] & 0x08) != 0;
        if (fire && !s_prev_fire) { extern unsigned long long ps3_qpc_us(void); g_racacit_fire_ms = ps3_qpc_us() / 1000ull; }
        s_prev_fire = fire;
        g_racacit_fire_held = fire;
    }
    /* PAD_DEBUG=1: once a second, what port 0 actually handed the title. */
    { static int s_pd = -1; if (s_pd < 0) s_pd = getenv("PAD_DEBUG") ? 1 : 0;
      if (s_pd && port_no == 0) { static unsigned long long s_t = 0; unsigned long long t = pad_now_ms();
        if (t - s_t >= 1000) { s_t = t;
          fprintf(stderr, "[pad-dbg] len=%d d1=%02X d2=%02X rx=%u ry=%u lx=%u ly=%u setting=0x%X\n",
                  (int)data->len, data->button[2], data->button[3], data->button[4], data->button[5],
                  data->button[6], data->button[7], s_port_setting[0]); } } }
    /* Button change logger: log immediately whenever any button state changes on port 0 */
    if (data->len && port_no == 0) {
        static u16 s_last_buttons = 0xFFFF;
        u16 cur_btns = ((u16)data->button[2] << 8) | data->button[3];
        if (cur_btns != s_last_buttons) {
            s_last_buttons = cur_btns;
            char desc[128] = "";
            if (cur_btns & 0x0040) strcat(desc, "CROSS ");
            if (cur_btns & 0x0020) strcat(desc, "CIRCLE ");
            if (cur_btns & 0x0080) strcat(desc, "SQUARE ");
            if (cur_btns & 0x0010) strcat(desc, "TRIANGLE ");
            if (cur_btns & 0x0008) strcat(desc, "START ");
            if (cur_btns & 0x0001) strcat(desc, "SELECT ");
            if (cur_btns & 0x1000) strcat(desc, "UP ");
            if (cur_btns & 0x4000) strcat(desc, "DOWN ");
            if (cur_btns & 0x8000) strcat(desc, "LEFT ");
            if (cur_btns & 0x2000) strcat(desc, "RIGHT ");
            if (cur_btns & 0x0400) strcat(desc, "L1 ");
            if (cur_btns & 0x0800) strcat(desc, "R1 ");
            if (cur_btns & 0x0100) strcat(desc, "L2 ");
            if (cur_btns & 0x0200) strcat(desc, "R2 ");
            fprintf(stderr, "[pad] buttons: 0x%04X (%s)\n", cur_btns, desc[0] ? desc : "NONE");
        }
    }
    /* A non-zero len means "here is a CHANGE packet" -- the constant is literally
     * named CELL_PAD_LEN_CHANGE_DEFAULT. Hardware reports len = 0 once the port
     * has nothing new, and titles DRAIN on that: The Simpsons Arcade Game's
     * input stage is
     *
     *     while (cellPadGetData(port, &d) == CELL_OK && d.len > 0) { ... }
     *
     * (func_0013CFA8, loop 0x13D648 -> 0x13D040). Reporting a change on every
     * call made that loop infinite: the main thread pegged one core inside
     * libpad and the title never got past its first rendered screen.
     *
     * So hand out one packet per host report and report len = 0 for any read
     * after that until the next one arrives (pad_poll_backend sets the flag when
     * it actually re-polls, every PAD_POLL_INTERVAL_MS). That is what the
     * hardware does: a pad streams reports at a fixed rate whether or not
     * anything changed, and libpad buffers them.
     *
     * Reporting only on CHANGE instead is wrong and was tried: a HELD button
     * produces exactly one packet, so a title that reads button[] only when
     * len > 0 sees the press for a single frame and then sees nothing. In this
     * game that came out as jump being the only control that seemed to work and
     * attack firing once.
     *
     * PAD_ALWAYS_CHANGE=1 restores the old always-a-packet behaviour. */
    {
        static int always = -1;
        if (always < 0) always = getenv("PAD_ALWAYS_CHANGE") ? 1 : 0;
        if (!always && port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;   /* nothing new: end the drain */
            else                        s_data_fresh[port_no] = 0;
        }
    }
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[127]     +0x10A u16 product_id[127] +0x208 u8 status[127]
 *   (CELL_MAX_PADS / PSL1GHT MAX_PADS = 127 -- NOT 7; see below)
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    /* Present a virtual pad on port 0 when nothing physical is attached, the
     * same as cellPadGetInfo2 does.
     *
     * The two entry points disagreed: GetInfo2 carried the fallback and GetInfo
     * did not, so which one a title imports decided whether it believed a
     * controller existed. The Orange Box's chooser imports GetInfo (v1) and
     * never GetInfo2, so it saw zero pads, opened "SIXAXIS wireless controller
     * not detected", took the auto-OK, and opened it again -- forever, before
     * it ever finished booting. Both entry points describe the same hardware
     * and have to answer the same way. */
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected || i == 0) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    /* CellPadInfo (v1) sizes its arrays by CELL_MAX_PADS = 127, not by the 7
     * ports: vendor_id[127] @0x0C, product_id[127] @0x10A, status[127] @0x208.
     * Packed for 7 ports, status landed at 0x28 and GH3 -- which reads
     * info+0x208+port -- saw port 0 disconnected and never polled the pad. */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  (s_host_state[i].connected || i == 0));
        vm_write16(gaddr + 0x00C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x10A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        vm_write8(gaddr + 0x208 + i, on ? CELL_PAD_STATUS_CONNECTED : 0);
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if (s_host_state[i].connected || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    /* copy to guest big-endian (CellPadInfo2 is all u32 fields) */
    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

/* cellPadPeriphGetInfo -- the peripheral-class view of the pad ports (NID
 * 0x4CC9B68D). LBP polls this during boot; leaving it unresolved returned
 * CELL_OK with the caller's CellPadPeriphInfo left as uninitialised stack
 * garbage, so the game read junk port_status/pclass_type for all 7 ports.
 *
 * Layout (SDK cell/pad/pad_codes.h) -- all u32, so the whole struct marshals
 * big-endian in one sweep like cellPadGetInfo2:
 *   max_connect, now_connect, system_info,
 *   port_status[7], port_setting[7], device_capability[7],
 *   device_type[7], pclass_type[7], pclass_profile[7]
 * We report a standard DUALSHOCK-class pad (pclass_type STANDARD = 0, no
 * profile bits) mirroring the ports cellPadGetInfo2 already reports. */
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if ((i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            info->pclass_type[i]       = CELL_PAD_PCLASS_TYPE_STANDARD;
            info->pclass_profile[i]    = 0;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if ((_n < 2 && (_n++, 1)))
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

/* cellPadInfoPressMode (NID 0x0E2DFAAD) / cellPadSetPressMode (NID 0xF83F8182)
 *
 * Press mode makes the digital buttons report an analog 0..255 pressure in the
 * extended part of CellPadData. A DS3 supports it, and the virtual pad reported
 * by cellPadGetInfo/GetInfo2 already advertises CELL_PAD_CAPABILITY_PRESS_MODE,
 * so these have to agree with that claim rather than fall through to the
 * unresolved-NID path.
 *
 * Info returns 1 when the port supports press mode; Set toggles it, recorded in
 * the same s_port_setting word cellPadSetPortSetting uses so a title that reads
 * the setting back sees what it asked for. */
s32 cellPadInfoPressMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetPressMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_PRESS_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_PRESS_ON;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        vm_write32(info_ea + i * 4, 0);

    /* Report standard DualShock 3 capabilities */
    vm_write32(info_ea, CELL_PAD_CAPABILITY_PS3_CONFORMITY
                        | CELL_PAD_CAPABILITY_PRESS_MODE
                        | CELL_PAD_CAPABILITY_SENSOR_MODE
                        | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                        | CELL_PAD_CAPABILITY_ACTUATOR);

    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);

#if PAD_BACKEND_XINPUT
    /* Map to XInput vibration */
    if (port_no < PAD_MAX_HOST_PORTS && s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257);
        vib.wRightMotorSpeed = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257);
        XInputSetState((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL2
    /* DualSense, Rift Apart layout: while the fire trigger is held and the game
     * rumbles (a shot, a hit), the trigger itself kicks -- a vibration in R2
     * scaled by the rumble -- and goes back to the weapon resistance when
     * the rumble stops. Sent only when that state changes. */
    if (port_no < PAD_MAX_HOST_PORTS && s_sdl_controllers[port_no] &&
        SDL_GameControllerGetType(s_sdl_controllers[port_no]) == SDL_CONTROLLER_TYPE_PS5 &&
        pad_ds_plus() && pad_swap(s_sdl_controllers[port_no])) {
        s_kick_ms[port_no] = SDL_GetTicks64();
        rsx_host_options o; rsx_host_ui_options(&o);
        const int s_trig_off = !o.ds_triggers;
        /* large motor 0-255; the small one is on/off */
        const int lg = param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE], sm = param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] ? 255 : 0;
        const int m = lg > sm ? lg : sm;
        const int firing = (s_host_state[port_no].buttons & CELL_PAD_CTRL_R1) != 0;
        const int want = (!s_trig_off && firing && m) ? 1 + (m * 7) / 255 : 0;   /* amplitude 1-8, 0 = none */
        if (!s_dsw_events && want != s_kick[port_no]) {
            s_kick[port_no] = want;
            Uint8 st[47]; memset(st, 0, sizeof st);
            st[0] = 0x04;                                   /* right trigger only */
            if (want) ds5_vibration(&st[10], 2, want, 35);
            else ds5_weapon(&st[10], 3, 6, 8);
            ds5_send(s_sdl_controllers[port_no], st);
        }
    }
    {   /* Audio haptics (v57, cellAudio.c): on a DualSense the game's rumble
         * becomes tones on the actuators instead -- SDL's rumble would turn
         * the DualSense's audio haptics off while it runs. */
        extern volatile int g_racacit_ds_haptics, g_racacit_rumble_lg, g_racacit_rumble_sm;
        extern volatile unsigned long long g_racacit_rumble_ms;
        if (port_no == 0 && g_racacit_ds_haptics && s_sdl_controllers[0] &&
            SDL_GameControllerGetType(s_sdl_controllers[0]) == SDL_CONTROLLER_TYPE_PS5) {
            g_racacit_rumble_lg = param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE];
            g_racacit_rumble_sm = param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL];
            g_racacit_rumble_ms = SDL_GetTicks64();
            return CELL_OK;
        }
    }
    if (port_no < PAD_MAX_HOST_PORTS && s_sdl_controllers[port_no]) {
        rsx_host_options o; rsx_host_ui_options(&o);   /* "Rumble" strength, 0 = off */
        unsigned k = (unsigned)o.ds_rumble * 257u / 100u;
        { const char* e = getenv("PAD_IGNORE_REAL"); if (e && *e == '1') k = 0; }   /* agent runs: the owner's pad stays still */
        SDL_GameControllerRumble(
            s_sdl_controllers[port_no],
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * k),
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * k),
            100 /* duration ms */
        );
        /* Impulse triggers (Xbox One / Series), R2 firing: like the DualSense
         * kick above, the fire trigger rumbles while held and the game rumbles. */
        if (o.ds_triggers && pad_swap(s_sdl_controllers[port_no]) &&
            SDL_GameControllerHasRumbleTriggers(s_sdl_controllers[port_no])) {
            const unsigned lg = param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE], sm = param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] ? 255u : 0u;
            const int firing = (s_host_state[port_no].buttons & CELL_PAD_CTRL_R1) != 0;
            SDL_GameControllerRumbleTriggers(s_sdl_controllers[port_no], 0, (Uint16)(firing ? (lg > sm ? lg : sm) * k / 2u : 0u), 100);
        }
    }
#endif

    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}
