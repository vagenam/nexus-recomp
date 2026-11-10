/* ps3recomp - on-screen touch controls. See touch_controls.h. */
#include "touch_controls.h"
#include "cellPad.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if !defined(_WIN32)
#include <SDL2/SDL.h>
#define TC_HAVE_SDL 1
#endif

/* Layout. Radii are fractions of the frame height; x positions are fractions
 * of the width (so a wide phone spreads the clusters apart, not the buttons). */
static const tc_button k_layout[] = {
    /* face buttons, bottom right */
    { 0.885f, 0.83f, 0.065f, TC_SHAPE_CROSS,    CELL_PAD_CTRL_CROSS },
    { 0.950f, 0.69f, 0.065f, TC_SHAPE_CIRCLE,   CELL_PAD_CTRL_CIRCLE },
    { 0.820f, 0.69f, 0.065f, TC_SHAPE_SQUARE,   CELL_PAD_CTRL_SQUARE },
    { 0.885f, 0.55f, 0.065f, TC_SHAPE_TRIANGLE, CELL_PAD_CTRL_TRIANGLE },
    /* d-pad, right of the left stick zone */
    { 0.330f, 0.73f, 0.050f, TC_SHAPE_UP,       CELL_PAD_CTRL_UP },
    { 0.330f, 0.91f, 0.050f, TC_SHAPE_DOWN,     CELL_PAD_CTRL_DOWN },
    { 0.285f, 0.82f, 0.050f, TC_SHAPE_LEFT,     CELL_PAD_CTRL_LEFT },
    { 0.375f, 0.82f, 0.050f, TC_SHAPE_RIGHT,    CELL_PAD_CTRL_RIGHT },
    /* shoulders */
    { 0.060f, 0.10f, 0.055f, TC_SHAPE_PILL,     CELL_PAD_CTRL_L1 },
    { 0.060f, 0.26f, 0.055f, TC_SHAPE_PILL,     CELL_PAD_CTRL_L2 },
    { 0.940f, 0.10f, 0.055f, TC_SHAPE_PILL,     CELL_PAD_CTRL_R1 },
    { 0.940f, 0.26f, 0.055f, TC_SHAPE_PILL,     CELL_PAD_CTRL_R2 },
    /* select / start, the settings menu between them (mask 0: not a pad button) */
    { 0.400f, 0.06f, 0.035f, TC_SHAPE_PILL,     CELL_PAD_CTRL_SELECT },
    { 0.600f, 0.06f, 0.035f, TC_SHAPE_PILL,     CELL_PAD_CTRL_START },
    { 0.500f, 0.06f, 0.030f, TC_SHAPE_PILL,     0 },
    /* stick clicks, under the triggers */
    { 0.060f, 0.42f, 0.045f, TC_SHAPE_PILL,     CELL_PAD_CTRL_L3 },
    { 0.940f, 0.42f, 0.045f, TC_SHAPE_PILL,     CELL_PAD_CTRL_R3 },
    /* left stick base (drawn; the zone is the whole lower-left) */
    { 0.130f, 0.72f, 0.140f, TC_SHAPE_STICK,    0 },
};
#define TC_N ((int)(sizeof k_layout / sizeof k_layout[0]))
#define TC_STICK_RANGE 0.12f       /* thumb travel for full deflection (height units) */

static volatile uint32_t s_pressed;
static volatile int   s_stick_on;
static volatile float s_ax, s_ay, s_tx, s_ty;
static volatile int   s_gamepad;

/* The layout at the player's size: RACACIT_TOUCH_SIZE, percent of the
 * default (60-160; positions stay, the buttons and their hit areas grow). */
static tc_button s_layout[TC_N];
static const tc_button* tc_layout(void)
{
    static int done;
    if (!done) {
        float k = 1.0f;
        const char* e = getenv("RACACIT_TOUCH_SIZE");
        if (e && *e) { k = (float)atof(e) / 100.0f; if (!(k >= 0.6f)) k = 0.6f; if (k > 1.6f) k = 1.6f; }
        for (int i = 0; i < TC_N; i++) { s_layout[i] = k_layout[i]; s_layout[i].r *= k; }
        done = 1;
    }
    return s_layout;
}

float touch_controls_opacity(void)
{
    static float op = -1.0f;
    if (op < 0.0f) {
        const char* e = getenv("RACACIT_TOUCH_OPACITY");
        op = (e && *e) ? (float)atof(e) / 100.0f : 1.0f;
        if (!(op >= 0.15f)) op = 0.15f; if (op > 1.0f) op = 1.0f;
    }
    return op;
}

/* Where the frame is on the screen (normalised) and its shape there, from the
 * presenter: touches are mapped into it, as the overlay is drawn in it. */
static volatile float s_fx0 = 0.0f, s_fy0 = 0.0f, s_fx1 = 1.0f, s_fy1 = 1.0f, s_faspect = 0.0f;
void touch_controls_set_frame(float x0, float y0, float x1, float y1, float aspect)
{
    if (!(x1 > x0) || !(y1 > y0) || !(aspect > 0.0f)) return;
    s_fx0 = x0; s_fy0 = y0; s_fx1 = x1; s_fy1 = y1; s_faspect = aspect;
}

int touch_controls_layout(const tc_button** out) { if (out) *out = tc_layout(); return TC_N; }
uint32_t touch_controls_pressed(void) { return s_pressed; }
void touch_controls_set_gamepad(int c) { s_gamepad = c; }
void touch_controls_stick(int* a, float* ax, float* ay, float* tx, float* ty)
{ *a = s_stick_on; *ax = s_ax; *ay = s_ay; *tx = s_tx; *ty = s_ty; }

int touch_controls_active(void)
{
    static int en = -1;
    if (en < 0) {
        const char* e = getenv("RACACIT_TOUCH");
#ifdef __ANDROID__
        en = (e && e[0] == '0') ? 0 : 1;
#else
        en = (e && e[0] == '1') ? 1 : 0;
#endif
    }
    /* The settings menu (rsx_host_ui) takes the touches while it is open. */
    extern int rsx_host_ui_capturing(void);
    return en && !s_gamepad && !rsx_host_ui_capturing();
}

static uint8_t tc_axis(float d)
{
    float v = d / TC_STICK_RANGE;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    int b = (int)lroundf(128.0f + v * 127.0f);
    return (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

int touch_controls_poll(float aspect, uint16_t* buttons,
                        uint8_t* lx, uint8_t* ly, uint8_t* rx, uint8_t* ry)
{
#if TC_HAVE_SDL
    static SDL_FingerID s_lfinger = -1, s_rfinger = -1;
    static float s_rlastx, s_rlasty;
    if (!touch_controls_active()) { s_pressed = 0; s_stick_on = 0; return 0; }
    uint16_t b = 0; uint32_t pressed = 0;
    int any = 0, lseen = 0, rseen = 0;
    if (s_faspect > 0.0f) aspect = s_faspect;
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;
    const float fx0 = s_fx0, fy0 = s_fy0, fw = s_fx1 - s_fx0, fh = s_fy1 - s_fy0;
    for (int d = 0; d < SDL_GetNumTouchDevices(); d++) {
        SDL_TouchID tid = SDL_GetTouchDevice(d);
        for (int f = 0; f < SDL_GetNumTouchFingers(tid); f++) {
            SDL_Finger* fg = SDL_GetTouchFinger(tid, f);
            if (!fg) continue;
            any = 1;
            const float x = (fg->x - fx0) / fw, y = (fg->y - fy0) / fh;   /* in the frame */
            /* Buttons first: distance in height units. */
            int hit = 0;
            const tc_button* lay = tc_layout();
            for (int i = 0; i < TC_N; i++) {
                const tc_button* k = &lay[i];
                if (k->shape == TC_SHAPE_STICK) continue;
                float dx = (x - k->x) * aspect, dy = y - k->y;
                float rr = k->r * 1.25f;                    /* generous hit area */
                if (dx * dx + dy * dy <= rr * rr) { b |= k->mask; pressed |= 1u << i; hit = 1; }
            }
            if (hit) continue;
            if (fg->id == s_lfinger || (s_lfinger == -1 && x < 0.40f && y > 0.33f)) {
                if (s_lfinger != fg->id) { s_lfinger = fg->id; s_ax = x; s_ay = y; }
                s_tx = x; s_ty = y; lseen = 1;
                *lx = tc_axis((x - s_ax) * aspect);
                *ly = tc_axis(y - s_ay);
            } else if (fg->id == s_rfinger || (s_rfinger == -1 && x >= 0.45f)) {
                /* Right stick: deflection follows the drag speed (camera). */
                if (s_rfinger != fg->id) { s_rfinger = fg->id; s_rlastx = x; s_rlasty = y; }
                float dx = (x - s_rlastx) * aspect * 14.0f, dy = (y - s_rlasty) * 14.0f;
                s_rlastx = x; s_rlasty = y; rseen = 1;
                *rx = tc_axis(dx * TC_STICK_RANGE / 0.12f);
                *ry = tc_axis(dy * TC_STICK_RANGE / 0.12f);
            }
        }
    }
    /* The menu button: a new press opens the settings menu, as F1 does on a
     * keyboard (pushed as a key event for the window thread to handle). */
    { static uint32_t s_was; const tc_button* lay = tc_layout();
      for (int i = 0; i < TC_N; i++)
          if (lay[i].shape == TC_SHAPE_PILL && !lay[i].mask && (pressed >> i & 1u) && !(s_was >> i & 1u)) {
              SDL_Event ev; memset(&ev, 0, sizeof ev);
              ev.type = SDL_KEYDOWN; ev.key.state = SDL_PRESSED; ev.key.keysym.sym = SDLK_F1; ev.key.keysym.scancode = SDL_SCANCODE_F1;
              SDL_PushEvent(&ev);
              ev.type = SDL_KEYUP; ev.key.state = SDL_RELEASED; SDL_PushEvent(&ev);
          }
      s_was = pressed; }
    if (!lseen) s_lfinger = -1;
    if (!rseen) s_rfinger = -1;
    s_stick_on = lseen;
    s_pressed = pressed;
    *buttons |= b;
    return any;
#else
    (void)aspect; (void)buttons; (void)lx; (void)ly; (void)rx; (void)ry;
    return 0;
#endif
}
