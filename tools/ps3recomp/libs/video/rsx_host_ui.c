/*
 * ps3recomp - host settings menu and on-screen display. See rsx_host_ui.h.
 */
#include "rsx_host_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#if defined(__has_include)
#  if __has_include(<SDL2/SDL.h>)
#    define RSX_UI_HAVE_SDL 1
#  endif
#endif

#ifdef RSX_UI_HAVE_SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

/* ---------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------*/
static SDL_SpinLock s_lock;
#define LOCK()   SDL_AtomicLock(&s_lock)
#define UNLOCK() SDL_AtomicUnlock(&s_lock)

static rsx_host_options s_opt;
static int   s_loaded;
static SDL_Window* s_win;
static float s_max_aniso = 16.0f;
static float s_rs_active = 1.0f;
static int   s_texq_active = -1;      /* texture quality the engine started with */
static int   s_page;                  /* 0 General, 1 Graphics, 2 Controls */
/* Read live by the SPU lanes (spu_channels.c) and the present pacing
 * (rsx_be_proxy.c); set from the options here. */
extern volatile int g_racacit_spu_spin_us;
extern volatile int g_racacit_present_pace;
static int   s_last_full = RSX_UI_DISPLAY_BORDERLESS;   /* what F11 returns to */

static volatile int s_open;          /* menu showing */
static int   s_sel;                  /* selected row */
static int   s_quit_armed;           /* "Quit game" pressed once */
static unsigned long long s_dirty_ms;   /* unsaved changes since (0 = none) */

/* Rows of the last layout, in target pixels, for the mouse. */
static struct { int x0, x1, y0[32], y1[32], n; float sx, sy; int tx0[4], tx1[4], ty0, ty1; } s_hit;

/* Trophies (rsx_host_ui_trophy_*): the set, the Trophies page's list and
 * the notices waiting to be shown. Under the lock. */
static struct {
    char title[128];
    int n;
    rsx_trophy t[RSX_UI_TROPHY_MAX];
    int has_icon[RSX_UI_TROPHY_MAX];
    int sel, top, vis;               /* the page's selected row, first row shown, rows that fit */
    int show_hidden;
    int pop[16];                     /* notices, oldest first */
    volatile int pop_n;
    unsigned long long pop_t0;       /* the first one's start; 0 = not shown yet */
} s_tr;
#define TR_PAGE 2

/* FPS counter */
static double s_ft[128];             /* recent frame intervals, ms */
static unsigned s_ft_n;
static unsigned long long s_last_present;
static char  s_fps_text[64];
static unsigned long long s_fps_next;

static unsigned long long now_ms(void) { return (unsigned long long)SDL_GetTicks64(); }

/* ---------------------------------------------------------------------------
 * Options: environment in, settings file out
 * -------------------------------------------------------------------------*/

static const int k_win[][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 },
                                { 3200, 1800 }, { 3840, 2160 } };
#define N_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

static int env_int(const char* k, int def) { const char* v = getenv(k); return (v && *v) ? atoi(v) : def; }
static int env_is(const char* k, const char* s) { const char* v = getenv(k); return v && !SDL_strcasecmp(v, s); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void rsx_host_ui_load_options(void)
{
    if (s_loaded) return;
    s_loaded = 1;
    rsx_host_options o;
    memset(&o, 0, sizeof o);
    o.display_mode = RSX_UI_DISPLAY_WINDOWED;
    { const char* v = getenv("PS3RECOMP_VK_FULLSCREEN"); if (v && *v && *v != '0') o.display_mode = RSX_UI_DISPLAY_BORDERLESS; }
    if (env_is("RACACIT_DISPLAY_MODE", "windowed"))   o.display_mode = RSX_UI_DISPLAY_WINDOWED;
    if (env_is("RACACIT_DISPLAY_MODE", "borderless")) o.display_mode = RSX_UI_DISPLAY_BORDERLESS;
    if (env_is("RACACIT_DISPLAY_MODE", "fullscreen")) o.display_mode = RSX_UI_DISPLAY_EXCLUSIVE;
    { const char* v = getenv("RACACIT_WINDOW_SIZE"); int w = 0, h = 0;
      if (v && sscanf(v, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 200 && w <= 16384 && h <= 16384) { o.win_w = w; o.win_h = h; } }
    o.vsync = RSX_UI_VSYNC_MAILBOX;
    if (env_is("RACACIT_PRESENT_MODE", "fifo"))      o.vsync = RSX_UI_VSYNC_FIFO;
    if (env_is("RACACIT_PRESENT_MODE", "immediate")) o.vsync = RSX_UI_VSYNC_IMMEDIATE;
    o.keep_aspect = !env_is("RACACIT_ASPECT", "stretch");
    o.upscale = RSX_UI_UPSCALE_BILINEAR;
    o.sharpness  = 0;
    o.brightness = 0;
    o.res_scale = 1.0f;
    o.aniso = 0;
    o.trilinear = 0;
    o.fps = env_int("RACACIT_FPS", 0) ? 1 : 0;
    o.discord = 0;
    o.skip_clank = 0;
    o.armor_look = 0;
    { const char* v = getenv("RACACIT_DS_SPEAKER"); o.ds_speaker = !(v && *v == '0'); }
    o.ds_volume = clampi(env_int("RACACIT_DS_SPK_VOL", 49), 0, 100);
    o.ds_haptics    = env_int("RACACIT_DS_HAPTICS", 1) ? 1 : 0;
    o.ds_haptic_vol = clampi(env_int("RACACIT_DS_HAPTIC_VOL", 50), 0, 100);
    o.ds_rumble     = clampi(env_int("RACACIT_DS_RUMBLE", 100), 0, 100);
    o.ds_triggers   = env_int("RACACIT_DS_TRIGGERS", 1) ? 1 : 0;
    o.ds_trig_str   = clampi(env_int("RACACIT_DS_TRIG_STR", 100), 10, 100);
    o.ds_swap       = env_int("RACACIT_DS_SWAP", 1) ? 1 : 0;
    o.ds_lightbar   = env_int("RACACIT_DS_LIGHT", 1) ? 1 : 0;
    o.ds_motion     = env_int("RACACIT_DS_MOTION", 1) ? 1 : 0;
    o.ds_plus       = env_int("RACACIT_DUALSENSE_PLUS", 0) ? 1 : 0;
    fprintf(stderr, "[settings] DualSense Plus: %s%c", o.ds_plus ? "on (DualSense extras, Fire on R2)"
                                                    : "off (every controller a plain pad, PS3 layout)", 10);
    o.tex_quality   = 0;
    o.cpu_mode      = env_int("RACACIT_CPU_MODE", 0);
    o.present_pace  = 0;
    o.kbm           = env_int("RACACIT_KBM", 1) ? 1 : 0;
    { const char* v = getenv("RACACIT_MOUSE_SENS"); const double m = v && *v ? atof(v) : 1.0;
      o.mouse_sens = clampi((int)(m * 100.0 + 0.5), 10, 500); }
    o.mouse_invert  = env_int("RACACIT_MOUSE_INVERT_Y", 0) ? 1 : 0;
    o.fov           = 100;
    o.cam_dist      = 100;
    s_texq_active   = 0;
    g_racacit_spu_spin_us = o.cpu_mode ? 200 : 30;
    g_racacit_present_pace = 0;
    o.gen = 1;
    if (o.display_mode != RSX_UI_DISPLAY_WINDOWED) s_last_full = o.display_mode;
    LOCK(); s_opt = o; UNLOCK();
#ifdef _WIN32
    /* Real pixels on high-DPI screens: without this Windows renders the game
     * at 100% and stretches the window, blurring everything. */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
}

/* Without the DualSense Plus mod the DualSense extras read as off for
 * everyone (pad, audio, prompts); the menu keeps editing the stored values
 * (s_opt), so turning the mod back on brings the player's choices back. */
static void ds_plus_mask(rsx_host_options* o)
{
    if (o->ds_plus) return;
    o->ds_speaker = o->ds_haptics = o->ds_triggers = o->ds_swap = o->ds_lightbar = o->ds_motion = 0;
}

void rsx_host_ui_options(rsx_host_options* out)
{
    rsx_host_ui_load_options();
    LOCK(); *out = s_opt; UNLOCK();
    ds_plus_mask(out);
}

/* The armor look picked in the game's own Extras > Skins menu (game/stubs.cpp):
 * kept in settings.txt like the menu's options, written at once -- the
 * menu's own changes wait for the next input event, which a game closed
 * right after the pick may never get. */
static void settings_save(void);
void rsx_host_ui_set_armor_look(int look)
{
    rsx_host_ui_load_options();
    if (look < 0 || look > 4) look = 0;
    LOCK();
    const int changed = s_opt.armor_look != look;
    if (changed) { s_opt.armor_look = look; s_opt.gen++; }
    UNLOCK();
    if (changed) settings_save();
}

/* One KEY=value per option, in settings.txt's own wording. */
static void settings_lines(const rsx_host_options* o, char kv[][96], int* n)
{
    int k = 0;
    snprintf(kv[k++], 96, "RACACIT_DISPLAY_MODE=%s", o->display_mode == RSX_UI_DISPLAY_EXCLUSIVE ? "fullscreen"
                                                   : o->display_mode == RSX_UI_DISPLAY_BORDERLESS ? "borderless" : "windowed");
    if (o->win_w && o->win_h) snprintf(kv[k++], 96, "RACACIT_WINDOW_SIZE=%dx%d", o->win_w, o->win_h);
    snprintf(kv[k++], 96, "RACACIT_PRESENT_MODE=%s", o->vsync == RSX_UI_VSYNC_FIFO ? "fifo"
                                                   : o->vsync == RSX_UI_VSYNC_IMMEDIATE ? "immediate" : "mailbox");
    snprintf(kv[k++], 96, "RACACIT_ASPECT=%s", o->keep_aspect ? "keep" : "stretch");
    snprintf(kv[k++], 96, "RACACIT_FPS=%d", o->fps);
    snprintf(kv[k++], 96, "RACACIT_CPU_MODE=%d", o->cpu_mode);
    snprintf(kv[k++], 96, "RACACIT_DS_SPEAKER=%d", o->ds_speaker);
    snprintf(kv[k++], 96, "RACACIT_DS_SPK_VOL=%d", o->ds_volume);
    snprintf(kv[k++], 96, "RACACIT_DS_HAPTICS=%d", o->ds_haptics);
    snprintf(kv[k++], 96, "RACACIT_DS_HAPTIC_VOL=%d", o->ds_haptic_vol);
    snprintf(kv[k++], 96, "RACACIT_DS_RUMBLE=%d", o->ds_rumble);
    snprintf(kv[k++], 96, "RACACIT_DS_TRIGGERS=%d", o->ds_triggers);
    snprintf(kv[k++], 96, "RACACIT_DS_TRIG_STR=%d", o->ds_trig_str);
    snprintf(kv[k++], 96, "RACACIT_DS_SWAP=%d", o->ds_swap);
    snprintf(kv[k++], 96, "RACACIT_DS_LIGHT=%d", o->ds_lightbar);
    snprintf(kv[k++], 96, "RACACIT_DS_MOTION=%d", o->ds_motion);
    snprintf(kv[k++], 96, "RACACIT_KBM=%d", o->kbm);
    snprintf(kv[k++], 96, "RACACIT_MOUSE_SENS=%.2f", o->mouse_sens / 100.0);
    snprintf(kv[k++], 96, "RACACIT_MOUSE_INVERT_Y=%d", o->mouse_invert);
    *n = k;
}

/* Rewrite the settings file with the current values: each key replaces its
 * own line (or the commented "#KEY=" template line of the shipped file), so
 * the file's comments and other settings stay where they are; keys it never
 * had are appended under a header. Written beside and renamed over, so a
 * crash mid-write cannot leave half a file. One writer at a time: the menu
 * saves from the event thread, the Skins menu's armor look from the game's. */
static void settings_save_file(void);
static void settings_save(void)
{
    static SDL_SpinLock save_lock;
    SDL_AtomicLock(&save_lock);
    settings_save_file();
    SDL_AtomicUnlock(&save_lock);
}
static void settings_save_file(void)
{
    const char* path = getenv("RACACIT_SETTINGS_FILE");
    if (!path || !*path) return;
    rsx_host_options o;
    LOCK(); o = s_opt; UNLOCK();
    char kv[40][96]; int nkv = 0;
    settings_lines(&o, kv, &nkv);

    char* text = NULL; size_t len = 0;
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < (1 << 20)) {
            text = (char*)malloc((size_t)sz + 1);
            if (text) { len = fread(text, 1, (size_t)sz, f); text[len] = 0; }
        }
        fclose(f);
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* out = fopen(tmp, "wb");
    if (!out) { free(text); return; }
    int done[sizeof kv / sizeof kv[0]] = {0};
    const char* nl = (text && strstr(text, "\r\n")) ? "\r\n" : "\n";
    for (char* p = text; p && *p; ) {
        char* e = strchr(p, '\n');
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        size_t body = ln; if (body && p[body - 1] == '\r') body--;
        int replaced = 0;
        for (int i = 0; i < nkv && !replaced; i++) {
            const size_t kl = (size_t)(strchr(kv[i], '=') - kv[i]) + 1;   /* "KEY=" */
            const char* q = p; size_t qb = body;
            if (qb && *q == '#') { q++; qb--; }
            if (qb >= kl && !strncmp(q, kv[i], kl)) {
                /* The first line for a key takes the value; later duplicates go. */
                if (!done[i]) fprintf(out, "%s%s", kv[i], nl);
                else if (*p == '#') fwrite(p, 1, body, out), fputs(nl, out);
                done[i] = 1; replaced = 1;
            }
        }
        if (!replaced) { fwrite(p, 1, body, out); if (e) fputs(nl, out); }
        p = e ? e + 1 : p + ln;
    }
    int header = 0;
    for (int i = 0; i < nkv; i++)
        if (!done[i]) {
            if (!header++) fprintf(out, "%s# --- Set from the in-game Settings menu (F1 / Esc) ---%s", nl, nl);
            fprintf(out, "%s%s", kv[i], nl);
        }
    fclose(out);
    free(text);
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) remove(tmp);
#else
    if (rename(tmp, path) != 0) remove(tmp);
#endif
}

/* ---------------------------------------------------------------------------
 * Window
 * -------------------------------------------------------------------------*/
static void desktop_size(int* w, int* h)
{
    SDL_DisplayMode dm;
    const int di = s_win ? SDL_GetWindowDisplayIndex(s_win) : 0;
    if (SDL_GetDesktopDisplayMode(di < 0 ? 0 : di, &dm) == 0 && dm.w > 0) { *w = dm.w; *h = dm.h; }
    else { *w = 1920; *h = 1080; }
}

/* The default window: the largest preset that leaves room around it. */
static void default_window(int* w, int* h)
{
    int dw, dh; desktop_size(&dw, &dh);
    *w = 1280; *h = 720;
    for (int i = 0; i < N_OF(k_win); i++)
        if (k_win[i][0] <= dw * 3 / 4 && k_win[i][1] <= dh * 3 / 4) { *w = k_win[i][0]; *h = k_win[i][1]; }
}

void rsx_host_ui_initial_window(int* w, int* h, unsigned* flags)
{
    rsx_host_ui_load_options();
    rsx_host_options o; rsx_host_ui_options(&o);
    if (o.win_w && o.win_h) { *w = o.win_w; *h = o.win_h; }
    else default_window(w, h);
    unsigned f = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (o.display_mode == RSX_UI_DISPLAY_BORDERLESS) f |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (o.display_mode == RSX_UI_DISPLAY_EXCLUSIVE)  f |= SDL_WINDOW_FULLSCREEN;
    *flags = f;
}

static void apply_window(void)
{
    if (!s_win) return;
    rsx_host_options o; rsx_host_ui_options(&o);
    if (o.display_mode == RSX_UI_DISPLAY_WINDOWED) {
        SDL_SetWindowFullscreen(s_win, 0);
        int w = o.win_w, h = o.win_h;
        if (!w || !h) default_window(&w, &h);
        SDL_SetWindowSize(s_win, w, h);
        SDL_SetWindowPosition(s_win, SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(s_win)),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(SDL_GetWindowDisplayIndex(s_win)));
    } else if (o.display_mode == RSX_UI_DISPLAY_BORDERLESS) {
        SDL_SetWindowFullscreen(s_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        /* Exclusive at the desktop's own mode: no monitor mode switch. */
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(s_win), &dm) == 0)
            SDL_SetWindowDisplayMode(s_win, &dm);
        SDL_SetWindowFullscreen(s_win, SDL_WINDOW_FULLSCREEN);
    }
    LOCK(); s_opt.swap_gen++; UNLOCK();
}

static void update_cursor(void)
{
    rsx_host_options o; rsx_host_ui_options(&o);
    SDL_ShowCursor((s_open || o.display_mode == RSX_UI_DISPLAY_WINDOWED) ? SDL_ENABLE : SDL_DISABLE);
}

void rsx_host_ui_attach(SDL_Window* win, float max_aniso, float res_scale_active)
{
    rsx_host_ui_load_options();
    s_win = win;
    s_max_aniso = max_aniso;
    s_rs_active = res_scale_active;
    if (win) {
        rsx_host_options o; rsx_host_ui_options(&o);
        if (o.display_mode == RSX_UI_DISPLAY_EXCLUSIVE) apply_window();
        update_cursor();
    }
}

/* ---------------------------------------------------------------------------
 * Menu model
 * -------------------------------------------------------------------------*/
enum { K_HEAD, K_OPT, K_ACT, K_GAP };
enum { R_DISPLAY = 1, R_WINSIZE, R_VSYNC, R_ASPECT, R_UPSCALE, R_SHARP, R_BRIGHT,
       R_RES, R_ANISO, R_TRI, R_DSSPK, R_DSVOL, R_DSHAP, R_DSHVOL, R_DSRUMBLE, R_DSTRIG, R_DSSWAP,
       R_DSLIGHT, R_DSMOTION, R_FPS, R_DISCORD, R_SKIPCLANK, R_RESUME, R_QUIT,
       R_PRESET, R_TEXQ, R_PACE, R_CPU, R_KBM, R_MSENS, R_MINV, R_DSTRIGSTR, R_TROPHIES,
       R_FOV, R_CAMDIST };
typedef struct { int kind, id; const char* label; } ui_row;
/* Three pages: General, Controls, Trophies */
static const ui_row k_rows_general[] = {
    { K_HEAD, 0, "Display" },
    { K_OPT, R_DISPLAY, "Display mode" },
    { K_OPT, R_WINSIZE, "Window size" },
    { K_OPT, R_VSYNC,   "VSync" },
    { K_OPT, R_ASPECT,  "Aspect ratio" },
    { K_OPT, R_FPS,     "FPS counter" },
    { K_HEAD, 0, "Performance" },
    { K_OPT, R_CPU,     "CPU profile" },
    { K_GAP, 0, "" },
    { K_ACT, R_RESUME,  "Resume" },
    { K_ACT, R_QUIT,    "Quit game" },
};
static const ui_row k_rows_controls[] = {
    { K_HEAD, 0, "Controller" },
    { K_OPT, R_DSSWAP,  "Fire button" },
    { K_OPT, R_DSRUMBLE, "Rumble" },
    { K_OPT, R_DSTRIG,  "Adaptive triggers" },
    { K_OPT, R_DSTRIGSTR, "Trigger strength" },
    { K_HEAD, 0, "DualSense" },
    { K_OPT, R_DSSPK,   "Controller speaker" },
    { K_OPT, R_DSVOL,   "Speaker volume" },
    { K_OPT, R_DSHAP,   "Haptic feedback" },
    { K_OPT, R_DSHVOL,  "Haptic strength" },
    { K_OPT, R_DSLIGHT, "Light bar" },
    { K_OPT, R_DSMOTION, "Motion sensors" },
    { K_HEAD, 0, "Keyboard and mouse" },
    { K_OPT, R_KBM,     "Play with keyboard and mouse" },
    { K_OPT, R_MSENS,   "Mouse sensitivity" },
    { K_OPT, R_MINV,    "Invert mouse Y" },
    { K_GAP, 0, "" },
    { K_ACT, R_RESUME,  "Resume" },
    { K_ACT, R_QUIT,    "Quit game" },
};
/* The Trophies page draws its own list (tr_build); its one row only keeps the
 * row-based code (selection, adjust) on something harmless. */
static const ui_row k_rows_trophies[] = {
    { K_OPT, R_TROPHIES, "Trophies" },
};
#define N_PAGES 3
static const char* const k_page_names[N_PAGES] = { "General", "Controls", "Trophies" };
static const ui_row* page_rows(int p, int* n)
{
    static ui_row f[2][N_OF(k_rows_controls) + N_OF(k_rows_general)];
    static int fn[2] = { -1, -1 };
    if (p == TR_PAGE) { *n = N_OF(k_rows_trophies); return k_rows_trophies; }
    if (p < 0 || p > 1) p = 0;
    if (fn[p] < 0) {
        rsx_host_ui_load_options();
        LOCK(); const int plus = s_opt.ds_plus; UNLOCK();
        const ui_row* src = p == 1 ? k_rows_controls : k_rows_general;
        const int ns = p == 1 ? N_OF(k_rows_controls) : N_OF(k_rows_general);
        int k = 0;
        for (int i = 0; i < ns; i++) {
            const int id = src[i].kind == K_OPT ? src[i].id : 0;
            if (!plus && (id == R_DSSWAP || id == R_DSTRIG || id == R_DSTRIGSTR || id == R_DSSPK || id == R_DSVOL ||
                          id == R_DSHAP || id == R_DSHVOL || id == R_DSLIGHT || id == R_DSMOTION ||
                          (src[i].kind == K_HEAD && !strcmp(src[i].label, "DualSense")))) continue;
#ifdef __ANDROID__
            if (id == R_DISPLAY || id == R_WINSIZE || id == R_DISCORD || id == R_DSSPK || id == R_DSVOL ||
                id == R_DSHAP || id == R_DSHVOL || id == R_MSENS || id == R_MINV) continue;
#endif
            f[p][k++] = src[i];
        }
        fn[p] = k;
    }
    *n = fn[p]; return f[p];
}
static const ui_row* rows(void) { int n; return page_rows(s_page, &n); }
static int n_rows(void) { int n; page_rows(s_page, &n); return n; }
/* Height of the longest page (head and row heights given), so the panel is
 * sized once and does not move when the page changes. */
static float rows_height(float head, float row)
{
    float best = 0.0f;
    for (int p = 0; p < N_PAGES; p++) {
        int n; const ui_row* r = page_rows(p, &n);
        float h = 0.0f;
        for (int i = 0; i < n; i++) h += r[i].kind == K_HEAD ? head : r[i].kind == K_GAP ? row * 0.5f : row;
        if (h > best) best = h;
    }
    return best;
}
#define N_ROWS n_rows()
#define k_rows rows()

static int row_enabled(int i, const rsx_host_options* o)
{
    if (k_rows[i].kind == K_HEAD || k_rows[i].kind == K_GAP) return 0;
    if (k_rows[i].id == R_WINSIZE) return o->display_mode == RSX_UI_DISPLAY_WINDOWED;
    if (k_rows[i].id == R_SHARP) return o->upscale == RSX_UI_UPSCALE_SHARP;
    if (k_rows[i].id == R_DSVOL) return o->ds_speaker;
    if (k_rows[i].id == R_DSHVOL) return o->ds_haptics;
    if (k_rows[i].id == R_DSTRIGSTR) return o->ds_triggers;
    if (k_rows[i].id == R_MSENS || k_rows[i].id == R_MINV) return o->kbm;
    return 1;
}

static void value_text(int id, const rsx_host_options* o, char* b, size_t n)
{
    b[0] = 0;
    switch (id) {
    case R_DISPLAY: snprintf(b, n, "%s", o->display_mode == RSX_UI_DISPLAY_EXCLUSIVE ? "Exclusive fullscreen"
                                       : o->display_mode == RSX_UI_DISPLAY_BORDERLESS ? "Borderless fullscreen" : "Windowed"); break;
    case R_WINSIZE: {
        int w = o->win_w, h = o->win_h;
        if (!w && s_win) SDL_GetWindowSize(s_win, &w, &h);
        snprintf(b, n, "%d x %d", w, h); break; }
    case R_VSYNC:   snprintf(b, n, "%s", o->vsync == RSX_UI_VSYNC_FIFO ? "On"
                                       : o->vsync == RSX_UI_VSYNC_IMMEDIATE ? "Off (allow tearing)" : "Off (no tearing)"); break;
    case R_ASPECT:  snprintf(b, n, "%s", o->keep_aspect ? "16:9" : "Stretch to window"); break;
    case R_FPS:     snprintf(b, n, "%s", o->fps ? "On" : "Off"); break;
    case R_CPU:     snprintf(b, n, "%s", o->cpu_mode == 1 ? "Xeon / Multi-core" : "General (Default)"); break;
    case R_UPSCALE: snprintf(b, n, "%s", o->upscale == RSX_UI_UPSCALE_SHARP ? "Sharp (bicubic + sharpen)"
                                       : o->upscale == RSX_UI_UPSCALE_NEAREST ? "Nearest (pixelated)" : "Bilinear"); break;
    case R_SHARP:   snprintf(b, n, "%d%%", o->sharpness * 10); break;
    case R_DSSPK:   snprintf(b, n, "%s", o->ds_speaker ? "On" : "Off"); break;
    case R_DSVOL:   snprintf(b, n, "%d%%", o->ds_volume); break;
    case R_DSHAP:   snprintf(b, n, "%s", o->ds_haptics ? "On" : "Off (rumble motors)"); break;
    case R_DSHVOL:  snprintf(b, n, "%d%%", o->ds_haptic_vol); break;
    case R_DSRUMBLE: if (o->ds_rumble) snprintf(b, n, "%d%%", o->ds_rumble); else snprintf(b, n, "Off"); break;
    case R_DSTRIG:  snprintf(b, n, "%s", o->ds_triggers ? "On" : "Off"); break;
    case R_DSTRIGSTR: snprintf(b, n, "%d%%", o->ds_trig_str); break;
    case R_DSSWAP:  snprintf(b, n, "%s", o->ds_swap ? "R2 (Rift Apart layout)" : "R1 (PS3 layout)"); break;
    case R_DSLIGHT: snprintf(b, n, "%s", o->ds_lightbar ? "On" : "Off"); break;
    case R_DSMOTION: snprintf(b, n, "%s", o->ds_motion ? "On" : "Off"); break;
    case R_KBM:     snprintf(b, n, "%s", o->kbm ? "On" : "Off"); break;
    case R_MSENS:   snprintf(b, n, "%d%%", o->mouse_sens); break;
    case R_MINV:    snprintf(b, n, "%s", o->mouse_invert ? "On" : "Off"); break;
    case R_QUIT:    if (s_quit_armed) snprintf(b, n, "Press again to quit"); break;
    default: break;
    }
}

static int cycle(int v, int n, int dir) { return ((v + dir) % n + n) % n; }

/* Change the option on row `id` one step in `dir` (-1 / +1). */
static void adjust(int id, int dir)
{
    int window = 0;
    LOCK();
    rsx_host_options* o = &s_opt;
    switch (id) {
    case R_DISPLAY:
        o->display_mode = cycle(o->display_mode, 3, dir);
        if (o->display_mode != RSX_UI_DISPLAY_WINDOWED) s_last_full = o->display_mode;
        window = 1; break;
    case R_WINSIZE: {
        int dw, dh; UNLOCK(); desktop_size(&dw, &dh); LOCK();
        int fit = 0;   /* presets that fit on the desktop */
        while (fit < N_OF(k_win) && k_win[fit][0] <= dw && k_win[fit][1] <= dh) fit++;
        if (!fit) fit = 1;
        int cur = -1, w = o->win_w, h = o->win_h;
        if (!w && s_win) { UNLOCK(); SDL_GetWindowSize(s_win, &w, &h); LOCK(); }
        for (int i = 0; i < fit; i++) if (k_win[i][0] == w && k_win[i][1] == h) cur = i;
        if (cur < 0) { cur = 0; for (int i = 0; i < fit; i++) if (k_win[i][0] < w) cur = i; if (dir < 0) cur++; }
        cur = cycle(cur, fit, dir);
        o->win_w = k_win[cur][0]; o->win_h = k_win[cur][1];
        window = 1; break; }
    case R_VSYNC:   o->vsync = cycle(o->vsync, 3, dir); o->swap_gen++; break;
    case R_ASPECT:  o->keep_aspect = !o->keep_aspect; break;
    case R_FPS:     o->fps = !o->fps; break;
    case R_CPU:
        o->cpu_mode = !o->cpu_mode;
        g_racacit_spu_spin_us = o->cpu_mode ? 200 : 30;
        break;
    case R_UPSCALE: o->upscale = cycle(o->upscale, 3, dir); break;
    case R_SHARP:   o->sharpness = clampi(o->sharpness + dir, 0, 10); break;
    case R_DSSPK:   o->ds_speaker = !o->ds_speaker; break;
    case R_DSVOL:   o->ds_volume = clampi(o->ds_volume + dir * 5, 0, 100); break;
    case R_DSHAP:   o->ds_haptics = !o->ds_haptics; break;
    case R_DSHVOL:  o->ds_haptic_vol = clampi(o->ds_haptic_vol + dir * 5, 0, 100); break;
    case R_DSRUMBLE: o->ds_rumble = clampi(o->ds_rumble + dir * 10, 0, 100); break;
    case R_DSTRIG:  o->ds_triggers = !o->ds_triggers; break;
    case R_DSTRIGSTR: o->ds_trig_str = clampi(o->ds_trig_str + dir * 10, 10, 100); break;
    case R_DSSWAP:  o->ds_swap = !o->ds_swap; break;
    case R_DSLIGHT: o->ds_lightbar = !o->ds_lightbar; break;
    case R_DSMOTION: o->ds_motion = !o->ds_motion; break;
    case R_KBM:     o->kbm = !o->kbm; break;
    case R_MSENS:   o->mouse_sens = clampi(o->mouse_sens + dir * (o->mouse_sens + dir * 5 > 100 ? 25 : 10), 10, 500); break;
    case R_MINV:    o->mouse_invert = !o->mouse_invert; break;
    default: UNLOCK(); return;
    }
    o->gen++;
    UNLOCK();
    if (window) { apply_window(); update_cursor(); }
    s_dirty_ms = now_ms();
    {   /* every change in log.txt: what a player changed, for bug reports */
        const char* label = "?";
        for (int p = 0; p < N_PAGES && !strcmp(label, "?"); p++) {
            int n; const ui_row* r = page_rows(p, &n);
            for (int i = 0; i < n; i++) if (r[i].kind == K_OPT && r[i].id == id) { label = r[i].label; break; }
        }
        rsx_host_options cur; rsx_host_ui_options(&cur);
        char val[96]; value_text(id, &cur, val, sizeof val);
        fprintf(stderr, "[settings] %s: %s%c", label, val, 10);
    }
}

static void mouse_capture(int on);
static int s_mcap, s_mcap_resume;   /* see "Keyboard and mouse for the game" */
static const char* s_menu_src = "?";   /* what opened or closed it, for the log */
static void open_menu(int open)
{
    if (open == s_open) return;
    s_open = open;
    s_quit_armed = 0;
    fprintf(stderr, "[settings] menu %s (%s)%c", open ? "opened" : "closed", s_menu_src, 10);
    s_menu_src = "?";
    if (open) { s_mcap_resume = s_mcap; mouse_capture(0); }
    else if (s_mcap_resume) { s_mcap_resume = 0; LOCK(); const int on = s_opt.kbm; UNLOCK(); if (on) mouse_capture(1); }
    if (open) {
        /* First selectable row. */
        rsx_host_options o; rsx_host_ui_options(&o);
        if (s_sel <= 0 || s_sel >= N_ROWS || !row_enabled(s_sel, &o))
            for (s_sel = 0; s_sel < N_ROWS && !row_enabled(s_sel, &o); s_sel++) {}
    } else if (s_dirty_ms) {
        settings_save(); s_dirty_ms = 0;
    }
    update_cursor();
}

static void move_sel(int dir)
{
    rsx_host_options o; rsx_host_ui_options(&o);
    for (int k = 0, i = s_sel; k < N_ROWS; k++) {
        i = cycle(i, N_ROWS, dir);
        if (row_enabled(i, &o)) { s_sel = i; break; }
    }
    s_quit_armed = 0;
}

/* Switch to page `page` (0 General, 1 Graphics, 2 Controls; wraps) at its
 * first selectable row. */
static void set_page(int page)
{
    page = (page % N_PAGES + N_PAGES) % N_PAGES;
    if (page == s_page) return;
    s_page = page;
    rsx_host_options o; rsx_host_ui_options(&o);
    for (s_sel = 0; s_sel < N_ROWS && !row_enabled(s_sel, &o); s_sel++) {}
    s_quit_armed = 0;
}

/* Enter / Cross / click on the selected row. Returns 1 for quit. */
static int activate(int dir)
{
    if (s_sel < 0 || s_sel >= N_ROWS) return 0;
    const int id = k_rows[s_sel].id;
    if (id == R_RESUME) { open_menu(0); return 0; }
    if (id == R_QUIT) {
        if (s_quit_armed) { if (s_dirty_ms) { settings_save(); s_dirty_ms = 0; } return 1; }
        s_quit_armed = 1; return 0;
    }
    adjust(id, dir);
    return 0;
}

static void toggle_fullscreen(void)
{
    LOCK();
    s_opt.display_mode = s_opt.display_mode == RSX_UI_DISPLAY_WINDOWED ? s_last_full : RSX_UI_DISPLAY_WINDOWED;
    s_opt.gen++;
    UNLOCK();
    apply_window();
    update_cursor();
    settings_save();
}

static volatile int s_sv_active;      /* the save screen is up */
static int sv_event(const SDL_Event* e);

int rsx_host_ui_capturing(void) { return s_open || s_sv_active; }
int rsx_host_ui_modal(void) { return s_sv_active; }

int rsx_host_ui_visible(void)
{
    if (s_open || s_sv_active || s_tr.pop_n) return 1;
    LOCK(); const int f = s_opt.fps; UNLOCK();
    return f;
}

/* ---------------------------------------------------------------------------
 * Input
 * -------------------------------------------------------------------------*/
static int mouse_row(int wx, int wy)
{
    LOCK();
    const float x = (float)wx * s_hit.sx, y = (float)wy * s_hit.sy;
    int r = -1;
    if (x >= (float)s_hit.x0 && x < (float)s_hit.x1)
        for (int i = 0; i < s_hit.n && i < 32; i++)
            if (y >= (float)s_hit.y0[i] && y < (float)s_hit.y1[i]) { r = i; break; }
    UNLOCK();
    return r;
}

/* ---------------------------------------------------------------------------
 * Keyboard and mouse for the game (cellPad.c reads them)
 * -------------------------------------------------------------------------*/
volatile int g_racacit_kbm_on;      /* set by cellPad: keyboard and mouse play */
/* s_mcap: the cursor is captured (relative mouse mode); s_mcap_resume: it
 * was when the menu opened, so it is again when the menu closes. */
static int s_mdx, s_mdy, s_mwheel;  /* since the last take, under the lock */
static unsigned s_mbtn, s_mbtn_tap; /* buttons held / pressed since the last take */
static int s_mtest;                 /* the test hook moved the mouse */
#define TEST_KEYS 8
static struct { int key; unsigned long long until; } s_tkey[TEST_KEYS];
static volatile int s_tkey_n;       /* test keys set (a cheap "none" for the pad poll) */

static void mouse_capture(int on)
{
#ifdef __ANDROID__
    on = 0;   /* touch screens: the touch controls, no cursor */
#endif
    if (!s_win || on == s_mcap) return;
    s_mcap = on;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    LOCK(); s_mdx = s_mdy = s_mwheel = 0; s_mbtn = s_mbtn_tap = 0; UNLOCK();
    if (!on) update_cursor();
}

int rsx_host_ui_mouse_take(int* dx, int* dy, int* wheel, unsigned* buttons)
{
    LOCK();
    *dx = s_mdx; *dy = s_mdy; *wheel = s_mwheel; *buttons = s_mbtn | s_mbtn_tap;
    s_mdx = s_mdy = s_mwheel = 0; s_mbtn_tap = 0;
    const int r = s_mcap || s_mtest;
    s_mtest = 0;
    UNLOCK();
    return r;
}

int rsx_host_ui_test_key(int key)
{
    if (!s_tkey_n) return 0;
    const unsigned long long t = now_ms();
    int r = 0, n = 0;
    LOCK();
    for (int i = 0; i < TEST_KEYS; i++)
        if (s_tkey[i].until) {
            if (t >= s_tkey[i].until) { s_tkey[i].until = 0; continue; }
            n++;
            if (key < 0 || s_tkey[i].key == key) r = 1;
        }
    s_tkey_n = n;
    UNLOCK();
    return r;
}

/* One press, one step: the same controller button down again within 120 ms
 * is the same press. Since v80 the owner's controller delivered 2-4 down
 * events per press (the touchpad opened and closed the menu at once, and
 * d-pad moves skipped 3-4 rows); the game reads the pad's state, only these
 * screens read events. Until 0.9.7 only the Settings menu had this, and the
 * save list skipped rows on a DualSense (owner, 0.9.7: "it will just skip
 * over certain options"). The first repeats are logged with their spacing
 * and device. */
static int pad_repeat_press(const SDL_Event* e)
{
    static Uint64 s_last[SDL_CONTROLLER_BUTTON_MAX]; static SDL_JoystickID s_last_id[SDL_CONTROLLER_BUTTON_MAX];
    static int s_logged;
    const int b = e->cbutton.button;
    if (b < 0 || b >= SDL_CONTROLLER_BUTTON_MAX) return 0;
    const Uint64 t = SDL_GetTicks64();
    if (s_last[b] && t - s_last[b] < 120) {
        if (s_logged < 12) { s_logged++;
            fprintf(stderr, "[settings] controller button %d down again after %llu ms (device %d, before %d): same press, ignored%c",
                    b, (unsigned long long)(t - s_last[b]), (int)e->cbutton.which, (int)s_last_id[b], 10); }
        return 1;
    }
    s_last[b] = t; s_last_id[b] = e->cbutton.which;
    return 0;
}

/* The Trophies page: move the selection, scrolling with it. */
static void tr_move(int d)
{
    LOCK();
    if (s_tr.n > 0) {
        s_tr.sel = clampi(s_tr.sel + d, 0, s_tr.n - 1);
        const int vis = s_tr.vis > 0 ? s_tr.vis : 1;
        if (s_tr.sel < s_tr.top) s_tr.top = s_tr.sel;
        if (s_tr.sel >= s_tr.top + vis) s_tr.top = s_tr.sel - vis + 1;
    }
    UNLOCK();
}
static void tr_toggle_hidden(void)
{
    LOCK(); s_tr.show_hidden = !s_tr.show_hidden; const int on = s_tr.show_hidden; UNLOCK();
    fprintf(stderr, "[settings] Trophies: hidden trophies' details %s%c", on ? "shown" : "hidden", 10);
}

int rsx_host_ui_event(const SDL_Event* e)
{
    if (s_mcap && (s_sv_active || s_open)) mouse_capture(0);
    if (e->type == SDL_CONTROLLERBUTTONDOWN && pad_repeat_press(e)) return 0;
    /* The save screen owns the input while it is up (F11 still works). */
    if (s_sv_active) {
        if (e->type == SDL_KEYDOWN && !e->key.repeat &&
            (e->key.keysym.sym == SDLK_F11 || (e->key.keysym.sym == SDLK_RETURN && (e->key.keysym.mod & KMOD_ALT)))) {
            toggle_fullscreen(); return 0;
        }
        if (e->type != SDL_WINDOWEVENT) return sv_event(e);
    }
    /* Unsaved changes made outside the menu (a dragged window size). */
    if (!s_open && s_dirty_ms && now_ms() - s_dirty_ms > 1000) { settings_save(); s_dirty_ms = 0; }

    switch (e->type) {
    case SDL_KEYDOWN: {
        const SDL_Keycode k = e->key.keysym.sym;
        if (k == SDLK_F11 || (k == SDLK_RETURN && (e->key.keysym.mod & KMOD_ALT))) {
            if (!e->key.repeat) toggle_fullscreen();
            return 0;
        }
        /* Esc is the game's START (its pause menu) while the keyboard plays; F1 is this menu. */
        if (k == SDLK_F1 || (k == SDLK_ESCAPE && !s_open && !g_racacit_kbm_on)) {
            if (!e->key.repeat) { s_menu_src = k == SDLK_F1 ? "F1" : "Esc"; open_menu(!s_open); }
            return 0; }
        if (k == SDLK_F5 && !e->key.repeat) {
            extern void rsx_draw_engine_cycle_surface_override(void);
            rsx_draw_engine_cycle_surface_override();
            return 0;
        }
        if (k == SDLK_F6 && !e->key.repeat) {
            extern void rsx_draw_engine_dump_now(void);
            rsx_draw_engine_dump_now();
            return 0;
        }
        if (!s_open) return 0;
        if (s_page == TR_PAGE) {
            switch (k) {
            case SDLK_UP: case SDLK_w: tr_move(-1); return 0;
            case SDLK_DOWN: case SDLK_s: tr_move(+1); return 0;
            case SDLK_HOME: tr_move(-1000); return 0;
            case SDLK_END: tr_move(+1000); return 0;
            case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_h:
                if (!e->key.repeat) tr_toggle_hidden();
                return 0;
            default: break;
            }
        }
        switch (k) {
        case SDLK_ESCAPE: case SDLK_BACKSPACE: open_menu(0); break;
        case SDLK_TAB: set_page(s_page + ((e->key.keysym.mod & KMOD_SHIFT) ? -1 : 1)); break;
        case SDLK_q: case SDLK_PAGEUP: set_page(s_page - 1); break;
        case SDLK_e: case SDLK_PAGEDOWN: set_page(s_page + 1); break;
        case SDLK_UP:   case SDLK_w: move_sel(-1); break;
        case SDLK_DOWN: case SDLK_s: move_sel(+1); break;
        case SDLK_LEFT: case SDLK_a: if (k_rows[s_sel].kind == K_OPT) adjust(k_rows[s_sel].id, -1); break;
        case SDLK_RIGHT: case SDLK_d: if (k_rows[s_sel].kind == K_OPT) adjust(k_rows[s_sel].id, +1); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: if (!e->key.repeat) return activate(+1);
        default: break;
        }
        return 0; }
    case SDL_CONTROLLERBUTTONDOWN: {
        const int b = e->cbutton.button;
        /* One press, one toggle: a second down event within 0.4 s is the same
         * press. The owner's DualSense (v81 log) sent two touchpad downs per
         * click, so the menu opened and closed at once. */
        if (b == SDL_CONTROLLER_BUTTON_GUIDE || b == SDL_CONTROLLER_BUTTON_TOUCHPAD) {
            static Uint64 s_toggle_ms;
            const Uint64 t = SDL_GetTicks64();
            if (s_toggle_ms && t - s_toggle_ms < 400) return 0;
            s_toggle_ms = t;
            s_menu_src = b == SDL_CONTROLLER_BUTTON_GUIDE ? "PS / Guide button" : "touchpad";
            open_menu(!s_open); return 0; }
        if (b == SDL_CONTROLLER_BUTTON_LEFTSTICK || b == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
            SDL_GameController* gc = SDL_GameControllerFromInstanceID(e->cbutton.which);
            const int other = b == SDL_CONTROLLER_BUTTON_LEFTSTICK ? SDL_CONTROLLER_BUTTON_RIGHTSTICK
                                                                   : SDL_CONTROLLER_BUTTON_LEFTSTICK;
            if (gc && SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)other)) { s_menu_src = "L3+R3"; open_menu(!s_open); return 0; }
        }
        if (!s_open) return 0;
        if (s_page == TR_PAGE) {
            switch (b) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:   tr_move(-1); return 0;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: tr_move(+1); return 0;
            case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_Y: tr_toggle_hidden(); return 0;
            default: break;
            }
        }
        switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    move_sel(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  move_sel(+1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  if (k_rows[s_sel].kind == K_OPT) adjust(k_rows[s_sel].id, -1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (k_rows[s_sel].kind == K_OPT) adjust(k_rows[s_sel].id, +1); break;
        case SDL_CONTROLLER_BUTTON_A: return activate(+1);
        case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_START: open_menu(0); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  set_page(s_page - 1); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: set_page(s_page + 1); break;
        default: break;
        }
        return 0; }
    case SDL_CONTROLLERAXISMOTION: {
        /* The left stick steps like the d-pad, once per push. */
        static int held[2];
        if (e->caxis.axis != SDL_CONTROLLER_AXIS_LEFTX && e->caxis.axis != SDL_CONTROLLER_AXIS_LEFTY) return 0;
        const int a = e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY;
        const int v = e->caxis.value, dir = v > 20000 ? 1 : v < -20000 ? -1 : 0;
        if (abs(v) < 12000) held[a] = 0;
        if (!s_open || !dir || held[a]) return 0;
        held[a] = 1;
        if (s_page == TR_PAGE) { if (a) tr_move(dir); return 0; }
        if (a) move_sel(dir);
        else if (k_rows[s_sel].kind == K_OPT) adjust(k_rows[s_sel].id, dir);
        return 0; }
    case SDL_MOUSEMOTION: {
        if (s_mcap) { LOCK(); s_mdx += e->motion.xrel; s_mdy += e->motion.yrel; UNLOCK(); return 0; }
        if (!s_open) return 0;
        const int r = mouse_row(e->motion.x, e->motion.y);
        rsx_host_options o; rsx_host_ui_options(&o);
        if (r >= 0 && r < N_ROWS && row_enabled(r, &o) && r != s_sel) { s_sel = r; s_quit_armed = 0; }
        return 0; }
    case SDL_MOUSEBUTTONUP:
        if (e->button.button >= 1 && e->button.button <= 5) { LOCK(); s_mbtn &= ~(1u << (e->button.button - 1)); UNLOCK(); }
        return 0;
    case SDL_MOUSEBUTTONDOWN: {
        if (!s_open) {
            /* A click in the game window captures the mouse for the camera (the
             * click itself does nothing else); Alt+Tab, F1 or the save screen
             * let it go. */
            if (e->button.which == SDL_TOUCH_MOUSEID || e->button.button < 1 || e->button.button > 5) return 0;
            if (!s_mcap) { if (g_racacit_kbm_on) mouse_capture(1); return 0; }
            LOCK(); s_mbtn |= 1u << (e->button.button - 1); s_mbtn_tap |= 1u << (e->button.button - 1); UNLOCK();
            return 0;
        }
        {   /* a page tab */
            LOCK();
            const float x = (float)e->button.x * s_hit.sx, y = (float)e->button.y * s_hit.sy;
            int t = -1;
            if (y >= (float)s_hit.ty0 && y < (float)s_hit.ty1)
                for (int k = 0; k < N_PAGES; k++) if (x >= (float)s_hit.tx0[k] && x < (float)s_hit.tx1[k]) t = k;
            UNLOCK();
            if (t >= 0) { set_page(t); return 0; }
        }
        const int r = mouse_row(e->button.x, e->button.y);
        rsx_host_options o; rsx_host_ui_options(&o);
        if (r < 0 || r >= N_ROWS || !row_enabled(r, &o)) return 0;
        s_sel = r;
        return activate(e->button.button == SDL_BUTTON_RIGHT ? -1 : +1); }
    case SDL_MOUSEWHEEL:
        if (s_mcap) { LOCK(); s_mwheel += e->wheel.y; UNLOCK(); return 0; }
        if (s_open && s_page == TR_PAGE) { if (e->wheel.y) tr_move(e->wheel.y > 0 ? -3 : 3); return 0; }
        if (s_open && k_rows[s_sel].kind == K_OPT && e->wheel.y) adjust(k_rows[s_sel].id, e->wheel.y > 0 ? 1 : -1);
        return 0;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) mouse_capture(0);
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED && s_win &&
            !(SDL_GetWindowFlags(s_win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MAXIMIZED))) {
            /* A window dragged to a new size keeps it next time. */
            int w = e->window.data1, h = e->window.data2;
            LOCK();
            if (s_opt.display_mode == RSX_UI_DISPLAY_WINDOWED && (s_opt.win_w != w || s_opt.win_h != h)) {
                s_opt.win_w = w; s_opt.win_h = h; s_opt.gen++;
                s_dirty_ms = now_ms();
            }
            UNLOCK();
        }
        return 0;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------------
 * FPS counter
 * -------------------------------------------------------------------------*/
void rsx_host_ui_on_present(void)
{
    const unsigned long long t = SDL_GetPerformanceCounter();
    static double freq;
    if (!freq) freq = (double)SDL_GetPerformanceFrequency();
    if (s_last_present) {
        s_ft[s_ft_n % N_OF(s_ft)] = (double)(t - s_last_present) * 1000.0 / freq;
        s_ft_n++;
    }
    s_last_present = t;
    const unsigned long long ms = now_ms();
    if (ms < s_fps_next) return;
    s_fps_next = ms + 500;
    /* Over the last second's frames: rate, average and worst frame time. */
    double sum = 0.0, worst = 0.0; unsigned n = 0;
    for (unsigned i = 0; i < N_OF(s_ft) && i < s_ft_n && sum < 1000.0; i++) {
        const double d = s_ft[(s_ft_n - 1 - i) % N_OF(s_ft)];
        sum += d; n++; if (d > worst) worst = d;
    }
    char b[64];
    if (n && sum > 0.0) snprintf(b, sizeof b, "%.0f FPS   %.1f ms   (worst %.0f ms)", n * 1000.0 / sum, sum / n, worst);
    else snprintf(b, sizeof b, "-- FPS");
    LOCK(); memcpy(s_fps_text, b, sizeof b); UNLOCK();
    if (s_win) {
        char title[128];
        if (n && sum > 0.0)
            snprintf(title, sizeof title, "Ratchet & Clank: Into the Nexus - %.0f FPS (%.1f ms)", n * 1000.0 / sum, sum / n);
        else
            snprintf(title, sizeof title, "Ratchet & Clank: Into the Nexus");
        SDL_SetWindowTitle(s_win, title);
    }
}

/* ---------------------------------------------------------------------------
 * Font
 *
 * Windows: Segoe UI rasterised by GDI at exactly the size drawn, grey
 * anti-aliasing, into an R8 atlas. Elsewhere (and if GDI fails): a built-in
 * 5x7 pixel font, scaled up by whole pixels.
 * -------------------------------------------------------------------------*/
static const unsigned char k_font5x7[95][7] = {
    {0,0,0,0,0,0,0},{4,4,4,4,4,0,4},{10,10,10,0,0,0,0},{10,10,31,10,31,10,10},
    {4,15,20,14,5,30,4},{24,25,2,4,8,19,3},{12,18,20,8,21,18,13},{12,4,8,0,0,0,0},
    {2,4,8,8,8,4,2},{8,4,2,2,2,4,8},{0,4,21,14,21,4,0},{0,4,4,31,4,4,0},
    {0,0,0,0,12,4,8},{0,0,0,31,0,0,0},{0,0,0,0,0,12,12},{0,1,2,4,8,16,0},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},
    {2,6,10,18,31,2,2},{31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14},{14,17,17,15,1,2,12},{0,12,12,0,12,12,0},{0,12,12,0,12,4,8},
    {2,4,8,16,8,4,2},{0,0,31,0,31,0,0},{8,4,2,1,2,4,8},{14,17,1,2,4,0,4},
    {14,17,1,13,21,21,14},{14,17,17,17,31,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {28,18,17,17,17,18,28},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},
    {17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},
    {16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,17,25,21,19,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},
    {31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},
    {17,17,10,4,10,17,17},{17,17,17,10,4,4,4},{31,1,2,4,8,16,31},{14,8,8,8,8,8,14},
    {0,16,8,4,2,1,0},{14,2,2,2,2,2,14},{4,10,17,0,0,0,0},{0,0,0,0,0,0,31},
    {8,4,2,0,0,0,0},{0,0,14,1,15,17,15},{16,16,22,25,17,17,30},{0,0,14,16,16,17,14},
    {1,1,13,19,17,17,15},{0,0,14,17,31,16,14},{6,9,8,28,8,8,8},{0,15,17,17,15,1,14},
    {16,16,22,25,17,17,17},{4,0,12,4,4,4,14},{2,0,6,2,2,18,12},{16,16,18,20,24,20,18},
    {12,4,4,4,4,4,14},{0,0,26,21,21,17,17},{0,0,22,25,17,17,17},{0,0,14,17,17,17,14},
    {0,0,30,17,30,16,16},{0,0,13,19,15,1,1},{0,0,22,25,16,16,16},{0,0,14,16,14,1,30},
    {8,8,28,8,8,9,6},{0,0,17,17,17,19,13},{0,0,17,17,17,10,4},{0,0,17,17,21,21,10},
    {0,0,17,10,4,10,17},{0,0,17,17,15,1,14},{0,0,31,2,4,8,31},{2,4,4,8,4,4,2},
    {4,4,4,4,4,4,4},{8,4,4,2,4,4,8},{0,0,8,21,2,0,0},
};

static int font_alloc(rsx_ui_font* f, int aw, int ah)
{
    unsigned char* p = (unsigned char*)calloc((size_t)aw * (size_t)ah, 1);
    if (!p) return -1;
    free(f->pixels);
    f->pixels = p; f->atlas_w = aw; f->atlas_h = ah;
    return 0;
}

/* Glyph 127: a solid 4x4 block in the atlas' top-left corner, its centre the
 * UV every rectangle samples. */
static void font_block(rsx_ui_font* f)
{
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) f->pixels[y * f->atlas_w + x] = 255;
    f->g[95] = (rsx_ui_glyph){ 0, 0, 4, 4, 0, 0 };
}

static int font_build_pixel(rsx_ui_font* f, int px)
{
    int s = px / 9; if (s < 1) s = 1;              /* 7 rows + spacing in px */
    const int cw = 6 * s, ch = 9 * s, per_row = 16;
    if (font_alloc(f, 8 + per_row * cw, 4 + 6 * ch + 8)) return -1;
    font_block(f);
    for (int c = 0; c < 95; c++) {
        const int gx = 8 + (c % per_row) * cw, gy = 4 + (c / per_row) * ch;
        for (int r = 0; r < 7; r++)
            for (int b = 0; b < 5; b++)
                if (k_font5x7[c][r] & (16 >> b))
                    for (int yy = 0; yy < s; yy++)
                        memset(&f->pixels[(gy + s + r * s + yy) * f->atlas_w + gx + b * s], 255, (size_t)s);
        f->g[c] = (rsx_ui_glyph){ (short)gx, (short)gy, (short)cw, (short)ch, 0, (short)cw };
    }
    f->line_h = ch; f->ascent = 8 * s;
    return 0;
}

#ifdef __ANDROID__
/* Android: the glyphs drawn by the platform's own text engine (Roboto Medium,
 * through JNI: game/android_companion.cpp, HostFont.java), laid out as the GDI
 * ones are. The 5x7 fallback read as wide block capitals -- the save screen
 * looked stretched out. */
extern int racacit_android_font(int px, int* aw, int* ah, int* line_h, int* ascent,
                                short glyph[95][6], unsigned char** pixels);
static int font_build_android(rsx_ui_font* f, int px)
{
    int aw = 0, ah = 0, lh = 0, asc = 0; short g[95][6]; unsigned char* pix = NULL;
    if (racacit_android_font(px, &aw, &ah, &lh, &asc, g, &pix) != 0 || !pix) return -1;
    if (font_alloc(f, aw, ah)) { free(pix); return -1; }
    memcpy(f->pixels, pix, (size_t)aw * (size_t)ah);
    free(pix);
    for (int c = 0; c < 95; c++) f->g[c] = (rsx_ui_glyph){ g[c][0], g[c][1], g[c][2], g[c][3], g[c][4], g[c][5] };
    font_block(f);
    f->line_h = lh; f->ascent = asc;
    return 0;
}
#endif

#ifdef _WIN32
static int font_build_gdi(rsx_ui_font* f, int px)
{
    int ok = -1;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return -1;
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    HBITMAP bmp = NULL;
    if (font) {
        HGDIOBJ oldf = SelectObject(dc, font);
        TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
        const int lh = tm.tmHeight, pad = px / 6 + 2;
        ABC abc[95];
        if (!GetCharABCWidthsW(dc, 32, 126, abc)) {
            for (int c = 0; c < 95; c++) { SIZE sz; WCHAR ch = (WCHAR)(32 + c);
                GetTextExtentPoint32W(dc, &ch, 1, &sz); abc[c].abcA = 0; abc[c].abcB = (UINT)sz.cx; abc[c].abcC = 0; }
        }
        /* Pack the cells into rows of a 1024-wide atlas. */
        const int aw = 1024;
        int x = 8, y = 0, rows_h = lh + 2;
        int gx[95], gy[95], gw[95];
        for (int c = 0; c < 95; c++) {
            const int w = (int)abc[c].abcB + 2 * pad;
            if (x + w > aw) { x = 8; y += rows_h; }
            gx[c] = x; gy[c] = y; gw[c] = w; x += w;
        }
        const int ah = y + rows_h + 4;
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = aw; bi.bmiHeader.biHeight = -ah;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp && bits && font_alloc(f, aw, ah) == 0) {
            HGDIOBJ oldb = SelectObject(dc, bmp);
            memset(bits, 0, (size_t)aw * ah * 4);
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkMode(dc, TRANSPARENT);
            for (int c = 0; c < 95; c++) {
                WCHAR ch = (WCHAR)(32 + c);
                TextOutW(dc, gx[c] + pad - abc[c].abcA, gy[c] + 1, &ch, 1);
                f->g[c] = (rsx_ui_glyph){ (short)gx[c], (short)gy[c], (short)gw[c], (short)(lh + 2),
                                          (short)(abc[c].abcA - pad),
                                          (short)(abc[c].abcA + (int)abc[c].abcB + abc[c].abcC) };
            }
            GdiFlush();
            const unsigned char* s = (const unsigned char*)bits;
            for (int i = 0; i < aw * ah; i++) f->pixels[i] = s[i * 4 + 1];   /* green: grey AA */
            font_block(f);
            f->line_h = lh + 2; f->ascent = tm.tmAscent + 1;
            SelectObject(dc, oldb);
            ok = 0;
        }
        SelectObject(dc, oldf);
        DeleteObject(font);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    return ok;
}
#endif

/* The UI text size for a screen height: 1/44 of it, smaller when the menu at
 * its tightest (a line a row, the layout's 0.78 squeeze) would not fit --
 * Segoe UI's line is ~1.45 of its size. */
static int ui_px(int sh)
{
    int px = (sh + 22) / 44;
    float lines = 3.5f;                                  /* title and footer */
    lines += rows_height(1.15f, 1.0f);                   /* the longest page */
    const int fit = (int)((float)sh * 0.94f / (lines * 1.45f));
    if (px > fit) px = fit;
    return px < 12 ? 12 : px > 72 ? 72 : px;
}

int rsx_host_ui_font_for(rsx_ui_font* f, int sw, int sh)
{
    (void)sw;
    const int px = ui_px(sh);
    if (f->px == px && f->pixels) return 0;
    int r = -1;
#ifdef _WIN32
    r = font_build_gdi(f, px);
#elif defined(__ANDROID__)
    r = font_build_android(f, px);
#endif
    if (r) r = font_build_pixel(f, px);
    if (r) return 0;
    f->px = px;
    f->gen++;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Layout
 * -------------------------------------------------------------------------*/
typedef struct { rsx_ui_vtx* v; int n, cap; const rsx_ui_font* f; float aw, ah; } ui_out;

void rsx_host_ui_atlas_size(const rsx_ui_font* f, int* w, int* h)
{
    *w = f->atlas_w > RSX_UI_SHEET_W ? f->atlas_w : RSX_UI_SHEET_W;
    *h = f->atlas_h + RSX_UI_SHEET_H;
}

static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24); }

static void quad(ui_out* o, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c)
{
    if (o->n + 6 > o->cap) return;
    rsx_ui_vtx* v = o->v + o->n;
    v[0] = (rsx_ui_vtx){ x0, y0, u0, v0, c }; v[1] = (rsx_ui_vtx){ x1, y0, u1, v0, c };
    v[2] = (rsx_ui_vtx){ x1, y1, u1, v1, c }; v[3] = v[0]; v[4] = v[2];
    v[5] = (rsx_ui_vtx){ x0, y1, u0, v1, c };
    o->n += 6;
}

static void rect(ui_out* o, float x0, float y0, float x1, float y1, uint32_t c)
{
    const float u = 2.0f / o->aw, v = 2.0f / o->ah;   /* inside the block */
    quad(o, x0, y0, x1, y1, u, v, u, v, c);
}

static float text_w(const rsx_ui_font* f, const char* s)
{
    float w = 0.0f;
    for (; *s; s++) { const unsigned c = (unsigned char)*s; if (c >= 32 && c < 127) w += (float)f->g[c - 32].adv; }
    return w;
}

/* `y` is the line's top. */
static void text(ui_out* o, float x, float y, const char* s, uint32_t c)
{
    const rsx_ui_font* f = o->f;
    const float iw = 1.0f / o->aw, ih = 1.0f / o->ah;
    for (; *s; s++) {
        const unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch >= 127) continue;
        const rsx_ui_glyph* g = &f->g[ch - 32];
        if (ch != ' ')
            quad(o, x + g->xoff, y, x + g->xoff + g->w, y + g->h,
                 g->x * iw, g->y * ih, (g->x + g->w) * iw, (g->y + g->h) * ih, c);
        x += (float)g->adv;
    }
}

/* ---------------------------------------------------------------------------
 * Save data screen (rsx_host_ui_save_pick)
 *
 * Styled after the game's own pause menu: a dark navy panel, cyan headings
 * and highlight. The rows show what the game wrote into each save: its ICON0
 * (the location's art), SUB_TITLE (the location) and DETAIL (time played).
 * -------------------------------------------------------------------------*/
#define SV_MAX 64
#define SV_ICON_W 240
#define SV_ICON_H 132
#define SV_ICONS ((RSX_UI_SHEET_W / SV_ICON_W) * (RSX_UI_TROPHY_SHEET_Y / SV_ICON_H))   /* above the trophy cells */
static unsigned char s_sheet[RSX_UI_SHEET_W * RSX_UI_SHEET_H * 4];
static volatile unsigned s_sheet_gen = 1;

static struct {
    int is_save, allow_new, n;
    rsx_save_entry e[SV_MAX];
    int gone[SV_MAX];             /* deleted while the screen was up         */
    int icon[SV_MAX];             /* sheet slot, -1 = none                   */
    int sel, top, vis;            /* selected row, first row shown, rows that fit */
    int confirm, yes;             /* 1 overwrite, 2 delete; Yes selected     */
    volatile int done, result;
    int (*del)(const char*);
    char msg[96]; unsigned long long msg_ms;
    /* mouse: rows on screen and the confirm buttons, in target pixels */
    int hx0, hx1, hy0[16], hy1[16], hrow[16], hn;
    int bx0[2], bx1[2], by0, by1;
} s_sv;

unsigned rsx_host_ui_sheet(unsigned char* out)
{
    const unsigned g = s_sheet_gen;
    SDL_MemoryBarrierAcquire();
    if (out) memcpy(out, s_sheet, sizeof s_sheet);
    return g;
}

/* Rows: "New save" first when offered, then the saves still there. */
static int sv_rows(int* map, int cap)
{
    int k = 0;
    if (s_sv.allow_new && k < cap) map[k++] = RSX_SAVE_PICK_NEW;
    for (int i = 0; i < s_sv.n && k < cap; i++) if (!s_sv.gone[i]) map[k++] = i;
    return k;
}

static void sv_keep_visible(int nrows)
{
    if (s_sv.sel >= nrows) s_sv.sel = nrows - 1;
    if (s_sv.sel < 0) s_sv.sel = 0;
    const int vis = s_sv.vis > 0 ? s_sv.vis : 4;
    if (s_sv.sel < s_sv.top) s_sv.top = s_sv.sel;
    if (s_sv.sel >= s_sv.top + vis) s_sv.top = s_sv.sel - vis + 1;
    if (s_sv.top > nrows - vis) s_sv.top = nrows - vis;
    if (s_sv.top < 0) s_sv.top = 0;
}

static void sv_finish(int result) { s_sv.result = result; SDL_MemoryBarrierRelease(); s_sv.done = 1; }

/* Enter / Cross on the selected row (or in the confirm box). */
static void sv_activate(void)
{
    int map[SV_MAX + 1];
    const int nrows = sv_rows(map, SV_MAX + 1);
    if (s_sv.confirm) {
        const int what = s_sv.confirm, yes = s_sv.yes;
        s_sv.confirm = 0;
        if (!yes || s_sv.sel >= nrows) return;
        const int i = map[s_sv.sel];
        if (what == 1) { sv_finish(i); return; }
        if (what == 2 && i >= 0) {
            if (s_sv.del && s_sv.del(s_sv.e[i].dir) == 0) {
                s_sv.gone[i] = 1;
                snprintf(s_sv.msg, sizeof s_sv.msg, "Save deleted");
            } else snprintf(s_sv.msg, sizeof s_sv.msg, "Could not delete that save");
            s_sv.msg_ms = now_ms();
            sv_keep_visible(sv_rows(map, SV_MAX + 1));
        }
        return;
    }
    if (s_sv.sel < 0 || s_sv.sel >= nrows) return;
    const int i = map[s_sv.sel];
    if (s_sv.is_save == RSX_SAVE_MODE_DELETE) { if (i >= 0 && s_sv.del) { s_sv.confirm = 2; s_sv.yes = 0; } return; }
    if (i == RSX_SAVE_PICK_NEW || !s_sv.is_save) { sv_finish(i); return; }
    s_sv.confirm = 1; s_sv.yes = 0;          /* overwrite: ask, No first */
}

static void sv_delete(void)
{
    int map[SV_MAX + 1];
    const int nrows = sv_rows(map, SV_MAX + 1);
    if (s_sv.confirm || !s_sv.del || s_sv.sel < 0 || s_sv.sel >= nrows || map[s_sv.sel] < 0) return;
    s_sv.confirm = 2; s_sv.yes = 0;
}

static void sv_back(void)
{
    if (s_sv.confirm) { s_sv.confirm = 0; return; }
    sv_finish(RSX_SAVE_PICK_CANCEL);
}

static void sv_move(int dir)
{
    if (s_sv.confirm) { s_sv.yes = !s_sv.yes; return; }
    int map[SV_MAX + 1];
    const int nrows = sv_rows(map, SV_MAX + 1);
    if (!nrows) return;
    s_sv.sel = ((s_sv.sel + dir) % nrows + nrows) % nrows;
    sv_keep_visible(nrows);
}

static int sv_hit(int wx, int wy, int* button)
{
    const float x = (float)wx * s_hit.sx, y = (float)wy * s_hit.sy;
    *button = -1;
    if (s_sv.confirm) {
        for (int b = 0; b < 2; b++)
            if (x >= s_sv.bx0[b] && x < s_sv.bx1[b] && y >= s_sv.by0 && y < s_sv.by1) *button = b;
        return -1;
    }
    if (x < s_sv.hx0 || x >= s_sv.hx1) return -1;
    for (int i = 0; i < s_sv.hn; i++)
        if (y >= s_sv.hy0[i] && y < s_sv.hy1[i]) return s_sv.hrow[i];
    return -1;
}

static int sv_event(const SDL_Event* e)
{
    LOCK();
    switch (e->type) {
    case SDL_KEYDOWN:
        switch (e->key.keysym.sym) {
        case SDLK_UP: case SDLK_w: sv_move(-1); break;
        case SDLK_DOWN: case SDLK_s: sv_move(+1); break;
        case SDLK_LEFT: case SDLK_RIGHT: case SDLK_a: case SDLK_d: if (s_sv.confirm) s_sv.yes = !s_sv.yes; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: if (!e->key.repeat) sv_activate(); break;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: if (!e->key.repeat) sv_back(); break;
        case SDLK_DELETE: case SDLK_x: if (!e->key.repeat) sv_delete(); break;
        default: break;
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sv_move(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sv_move(+1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (s_sv.confirm) s_sv.yes = !s_sv.yes; break;
        case SDL_CONTROLLER_BUTTON_A: sv_activate(); break;
        case SDL_CONTROLLER_BUTTON_B: sv_back(); break;
        case SDL_CONTROLLER_BUTTON_Y: sv_delete(); break;
        default: break;
        }
        break;
    case SDL_CONTROLLERAXISMOTION: {
        static int held[2];
        if (e->caxis.axis != SDL_CONTROLLER_AXIS_LEFTX && e->caxis.axis != SDL_CONTROLLER_AXIS_LEFTY) break;
        const int a = e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY;
        const int v = e->caxis.value, dir = v > 20000 ? 1 : v < -20000 ? -1 : 0;
        if (abs(v) < 12000) held[a] = 0;
        if (!dir || held[a]) break;
        held[a] = 1;
        if (a) sv_move(dir); else if (s_sv.confirm) s_sv.yes = !s_sv.yes;
        break; }
    case SDL_MOUSEMOTION: {
        int b; const int r = sv_hit(e->motion.x, e->motion.y, &b);
        if (s_sv.confirm) { if (b >= 0) s_sv.yes = (b == 0); }
        else if (r >= 0) s_sv.sel = r;
        break; }
    case SDL_MOUSEBUTTONDOWN: {
        int b; const int r = sv_hit(e->button.x, e->button.y, &b);
        if (s_sv.confirm) { if (b >= 0) { s_sv.yes = (b == 0); sv_activate(); } }
        else if (r >= 0) { s_sv.sel = r; if (e->button.button == SDL_BUTTON_RIGHT) sv_delete(); else sv_activate(); }
        break; }
    case SDL_MOUSEWHEEL:
        if (e->wheel.y) sv_move(e->wheel.y > 0 ? -1 : +1);
        break;
    default: break;
    }
    UNLOCK();
    return 0;
}

/* `s` cut to fit `maxw` pixels, with "..." when it was cut. */
static void fit_text(const rsx_ui_font* f, const char* s, float maxw, char* out, size_t n)
{
    snprintf(out, n, "%s", s);
    if (text_w(f, out) <= maxw) return;
    size_t len = strlen(out);
    while (len > 0) {
        out[--len] = 0;
        char t[300]; snprintf(t, sizeof t, "%s...", out);
        if (text_w(f, t) <= maxw) { snprintf(out, n, "%s", t); return; }
    }
}

static void sv_build(ui_out* o, int sw, int sh)
{
    const rsx_ui_font* f = o->f;
    const float px = (float)f->px, lh = (float)f->line_h;
    const uint32_t cyan = rgba(90, 205, 255, 255), white = rgba(240, 244, 250, 255), grey = rgba(150, 160, 178, 255);
    LOCK();
    int map[SV_MAX + 1];
    const int nrows = sv_rows(map, SV_MAX + 1);

    rect(o, 0, 0, (float)sw, (float)sh, rgba(0, 4, 14, 170));
    const float pad = px * 1.1f;
    float pw = px * 36.0f; if (pw > sw * 0.94f) pw = sw * 0.94f;
    const float title_h = lh * 2.4f, foot_h = lh * 2.2f;
    float row_h = lh * 2.9f;
    /* As many rows as fit in 82% of the screen, at most five. */
    int vis = (int)(((float)sh * 0.82f - pad * 2.0f - title_h - foot_h) / row_h);
    if (vis > 5) vis = 5;
    if (vis < 1) vis = 1;
    s_sv.vis = vis;
    sv_keep_visible(nrows);
    const int shown = nrows < vis ? (nrows ? nrows : 1) : vis;
    const float ph = pad + title_h + shown * row_h + foot_h + pad * 0.5f;
    const float x0 = ((float)sw - pw) * 0.5f, y0 = ((float)sh - ph) * 0.5f, x1 = x0 + pw;

    rect(o, x0, y0, x1, y0 + ph, rgba(8, 16, 34, 238));
    rect(o, x0, y0, x1, y0 + px * 0.16f, cyan);
    rect(o, x0, y0 + ph - px * 0.08f, x1, y0 + ph, rgba(90, 205, 255, 120));
    text(o, x0 + pad, y0 + pad * 0.7f, s_sv.is_save == RSX_SAVE_MODE_DELETE ? "DELETE SAVE DATA"
                                       : s_sv.is_save ? "SAVE GAME" : "LOAD GAME", cyan);
    {
        char b[96];
        const int cnt = nrows - (s_sv.allow_new ? 1 : 0);
        snprintf(b, sizeof b, "%d save%s", cnt, cnt == 1 ? "" : "s");
        text(o, x1 - pad - text_w(f, b), y0 + pad * 0.7f, b, grey);
    }
    rect(o, x0 + pad, y0 + pad * 0.7f + lh * 1.35f, x1 - pad, y0 + pad * 0.7f + lh * 1.35f + px * 0.06f, rgba(90, 205, 255, 70));

    float y = y0 + pad + title_h;
    const float ih = row_h - px * 0.7f, iw = ih * (float)SV_ICON_W / (float)SV_ICON_H;
    s_sv.hx0 = (int)x0; s_sv.hx1 = (int)x1; s_sv.hn = 0;
    if (!nrows) text(o, x0 + pad, y + (row_h - lh) * 0.5f, "No save data", grey);
    for (int r = s_sv.top; r < nrows && r < s_sv.top + vis; r++) {
        const int i = map[r], sel = (r == s_sv.sel);
        if (s_sv.hn < 16) { s_sv.hy0[s_sv.hn] = (int)y; s_sv.hy1[s_sv.hn] = (int)(y + row_h); s_sv.hrow[s_sv.hn] = r; s_sv.hn++; }
        if (sel) {
            rect(o, x0 + pad * 0.5f, y + px * 0.1f, x1 - pad * 0.5f, y + row_h - px * 0.1f, rgba(30, 120, 200, 200));
            rect(o, x0 + pad * 0.5f, y + px * 0.1f, x0 + pad * 0.5f + px * 0.22f, y + row_h - px * 0.1f, cyan);
        }
        const float ix = x0 + pad * 1.1f, iy = y + (row_h - ih) * 0.5f;
        const float tx = ix + iw + pad * 0.8f, tw = x1 - pad - tx;
        char b[300];
        if (i == RSX_SAVE_PICK_NEW) {
            rect(o, ix, iy, ix + iw, iy + ih, rgba(20, 40, 70, 255));
            rect(o, ix + iw * 0.5f - px * 0.12f, iy + ih * 0.25f, ix + iw * 0.5f + px * 0.12f, iy + ih * 0.75f, cyan);
            rect(o, ix + iw * 0.5f - ih * 0.25f, iy + ih * 0.5f - px * 0.12f, ix + iw * 0.5f + ih * 0.25f, iy + ih * 0.5f + px * 0.12f, cyan);
            text(o, tx, y + (row_h - lh) * 0.5f, "New Save Data", white);
        } else {
            const int slot = s_sv.icon[i];
            if (slot >= 0) {
                const int cols = RSX_UI_SHEET_W / SV_ICON_W;
                const float ax = (float)((slot % cols) * SV_ICON_W), ay = (float)(f->atlas_h + (slot / cols) * SV_ICON_H);
                quad(o, ix, iy, ix + iw, iy + ih, (ax + 0.5f) / o->aw, (ay + 0.5f) / o->ah,
                     (ax + SV_ICON_W - 0.5f) / o->aw, (ay + SV_ICON_H - 0.5f) / o->ah, rgba(255, 255, 255, 255));
            } else rect(o, ix, iy, ix + iw, iy + ih, rgba(20, 40, 70, 255));
            const rsx_save_entry* se = &s_sv.e[i];
            const float ty = y + (row_h - lh * 2.1f) * 0.5f;
            const float dw = text_w(f, se->when);
            fit_text(f, se->subtitle[0] ? se->subtitle : se->dir, tw - dw - pad, b, sizeof b);
            text(o, tx, ty, b, white);
            text(o, x1 - pad - dw, ty, se->when, sel ? white : grey);
            fit_text(f, se->detail[0] ? se->detail : se->title, tw, b, sizeof b);
            text(o, tx, ty + lh * 1.1f, b, sel ? rgba(200, 235, 255, 255) : cyan);
        }
        y += row_h;
    }
    if (s_sv.top > 0) text(o, x1 - pad * 0.9f - text_w(f, "^"), y0 + pad + title_h - lh * 0.6f, "^", cyan);
    if (s_sv.top + vis < nrows) text(o, x1 - pad * 0.9f - text_w(f, "v"), y - lh * 0.9f, "v", cyan);

    const char* foot = s_sv.is_save == RSX_SAVE_MODE_DELETE ? "Enter / Cross  delete      Esc / Circle  back"
                     : s_sv.del ? "Enter / Cross  select      Del / Triangle  delete      Esc / Circle  back"
                                : "Enter / Cross  select      Esc / Circle  back";
    text(o, x0 + (pw - text_w(f, foot)) * 0.5f, y0 + ph - foot_h * 0.5f - lh * 0.5f - pad * 0.2f, foot, grey);

    if (s_sv.msg[0] && now_ms() - s_sv.msg_ms < 2500) {
        const float mw = text_w(f, s_sv.msg) + pad * 2.0f, my = y0 - lh * 2.0f;
        rect(o, ((float)sw - mw) * 0.5f, my, ((float)sw + mw) * 0.5f, my + lh * 1.5f, rgba(8, 16, 34, 238));
        text(o, ((float)sw - mw) * 0.5f + pad, my + lh * 0.25f, s_sv.msg, cyan);
    }

    if (s_sv.confirm && s_sv.sel < nrows && map[s_sv.sel] >= 0) {
        const rsx_save_entry* se = &s_sv.e[map[s_sv.sel]];
        const char* q = s_sv.confirm == 1 ? "Overwrite this save?" : "Delete this save?";
        char b[300]; fit_text(f, se->subtitle[0] ? se->subtitle : se->dir, pw * 0.6f, b, sizeof b);
        float cw = text_w(f, q); if (text_w(f, b) > cw) cw = text_w(f, b);
        cw += pad * 3.0f; if (cw < px * 16.0f) cw = px * 16.0f;
        const float chh = lh * 5.6f, cx0 = ((float)sw - cw) * 0.5f, cy0 = ((float)sh - chh) * 0.5f;
        rect(o, x0, y0, x1, y0 + ph, rgba(0, 0, 0, 120));
        rect(o, cx0, cy0, cx0 + cw, cy0 + chh, rgba(10, 22, 44, 250));
        rect(o, cx0, cy0, cx0 + cw, cy0 + px * 0.14f, s_sv.confirm == 2 ? rgba(240, 90, 70, 255) : cyan);
        text(o, cx0 + (cw - text_w(f, q)) * 0.5f, cy0 + lh * 0.7f, q, white);
        text(o, cx0 + (cw - text_w(f, b)) * 0.5f, cy0 + lh * 1.8f, b, cyan);
        const float bw = px * 5.5f, gap = px * 1.2f, by = cy0 + lh * 3.4f;
        const char* lab[2] = { "Yes", "No" };
        s_sv.by0 = (int)by; s_sv.by1 = (int)(by + lh * 1.4f);
        for (int k = 0; k < 2; k++) {
            const float bx = (float)sw * 0.5f + (k == 0 ? -(bw + gap * 0.5f) : gap * 0.5f);
            const int on = (k == 0) == (s_sv.yes != 0);
            rect(o, bx, by, bx + bw, by + lh * 1.4f, on ? rgba(30, 120, 200, 230) : rgba(30, 40, 60, 230));
            text(o, bx + (bw - text_w(f, lab[k])) * 0.5f, by + lh * 0.2f, lab[k], on ? white : grey);
            s_sv.bx0[k] = (int)bx; s_sv.bx1[k] = (int)(bx + bw);
        }
    }
    UNLOCK();

    if (s_win) {   /* mouse coordinates arrive in window points */
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(s_win, &ww, &wh);
        SDL_Vulkan_GetDrawableSize(s_win, &dw, &dh);
        LOCK();
        s_hit.sx = ww > 0 ? (float)dw / (float)ww : 1.0f;
        s_hit.sy = wh > 0 ? (float)dh / (float)wh : 1.0f;
        UNLOCK();
    }
}

/* An icon into its sheet cell, scaled to fit (area average) and flattened
 * over the row background. */
static void sv_put_icon(int slot, const unsigned char* px, int w, int h)
{
    const int cols = RSX_UI_SHEET_W / SV_ICON_W;
    const int cx = (slot % cols) * SV_ICON_W, cy = (slot / cols) * SV_ICON_H;
    for (int y = 0; y < SV_ICON_H; y++) {
        const int sy0 = y * h / SV_ICON_H, sy1 = (y + 1) * h / SV_ICON_H > sy0 ? (y + 1) * h / SV_ICON_H : sy0 + 1;
        for (int x = 0; x < SV_ICON_W; x++) {
            const int sx0 = x * w / SV_ICON_W, sx1 = (x + 1) * w / SV_ICON_W > sx0 ? (x + 1) * w / SV_ICON_W : sx0 + 1;
            unsigned acc[4] = { 0, 0, 0, 0 }, cnt = 0;
            for (int yy = sy0; yy < sy1 && yy < h; yy++)
                for (int xx = sx0; xx < sx1 && xx < w; xx++) {
                    const unsigned char* p = px + ((size_t)yy * w + xx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; cnt++;
                }
            unsigned char* d = s_sheet + ((size_t)(cy + y) * RSX_UI_SHEET_W + cx + x) * 4;
            if (!cnt) cnt = 1;
            const unsigned a = acc[3] / cnt;
            d[0] = (unsigned char)((acc[0] / cnt) * a / 255 + 20 * (255 - a) / 255);
            d[1] = (unsigned char)((acc[1] / cnt) * a / 255 + 40 * (255 - a) / 255);
            d[2] = (unsigned char)((acc[2] / cnt) * a / 255 + 70 * (255 - a) / 255);
            d[3] = 255;
        }
    }
}

int rsx_host_ui_save_pick(int is_save, rsx_save_entry* entries, int n, int allow_new, int initial,
                          int (*del)(const char* dir))
{
    { const char* v = getenv("RACACIT_SAVE_UI"); if (v && *v == '0') return RSX_SAVE_PICK_UNAVAILABLE; }
    if (!s_win || !entries || n < 0) return RSX_SAVE_PICK_UNAVAILABLE;
    if (n > SV_MAX) n = SV_MAX;
    LOCK();
    memset(&s_sv, 0, sizeof s_sv);
    s_sv.is_save = is_save; s_sv.allow_new = allow_new; s_sv.n = n; s_sv.del = del;
    for (int i = 0; i < n; i++) {
        s_sv.e[i] = entries[i];
        s_sv.e[i].icon = NULL;
        s_sv.icon[i] = -1;
        if (i < SV_ICONS && entries[i].icon && entries[i].icon_w > 0 && entries[i].icon_h > 0) {
            sv_put_icon(i, entries[i].icon, entries[i].icon_w, entries[i].icon_h);
            s_sv.icon[i] = i;
        }
    }
    int map[SV_MAX + 1];
    const int nrows = sv_rows(map, SV_MAX + 1);
    for (int r = 0; r < nrows; r++) if (map[r] == initial) s_sv.sel = r;
    s_sv.vis = 4;
    sv_keep_visible(nrows);
    SDL_MemoryBarrierRelease();
    s_sheet_gen++;
    s_sv_active = 1;
    UNLOCK();
    fprintf(stderr, "[save ui] %s screen: %d saves%s%c", is_save == RSX_SAVE_MODE_DELETE ? "delete" : is_save ? "save" : "load",
            n, allow_new ? " + new" : "", 10);
    while (!s_sv.done) SDL_Delay(10);
    SDL_MemoryBarrierAcquire();
    const int r = s_sv.result;
    LOCK(); s_sv_active = 0; UNLOCK();
    fprintf(stderr, "[save ui] chose %d%c", r, 10);
    return r;
}

/* ---------------------------------------------------------------------------
 * Trophies: the Settings menu's Trophies page and the notice when one unlocks
 * -------------------------------------------------------------------------*/
void rsx_host_ui_trophy_set(const char* title, const rsx_trophy* t, int n)
{
    if (n > RSX_UI_TROPHY_MAX) n = RSX_UI_TROPHY_MAX;
    if (n < 0) n = 0;
    LOCK();
    snprintf(s_tr.title, sizeof s_tr.title, "%s", title ? title : "");
    for (int i = 0; i < n; i++) s_tr.t[i] = t[i];
    s_tr.n = n;
    if (s_tr.sel >= n) s_tr.sel = n ? n - 1 : 0;
    UNLOCK();
}

void rsx_host_ui_trophy_unlocked(int id, long long when, int notify)
{
    LOCK();
    for (int i = 0; i < s_tr.n; i++)
        if (s_tr.t[i].id == id) {
            s_tr.t[i].when = when ? when : 1;
            if (notify && s_tr.pop_n < N_OF(s_tr.pop)) { s_tr.pop[s_tr.pop_n] = i; s_tr.pop_n = s_tr.pop_n + 1; }
            break;
        }
    UNLOCK();
}

void rsx_host_ui_trophy_icon(int id, const unsigned char* px, int w, int h)
{
    if (id < 0 || id >= RSX_UI_TROPHY_MAX || !px || w <= 0 || h <= 0) return;
    const int C = RSX_UI_TROPHY_CELL, cols = RSX_UI_SHEET_W / C;
    const int cx = (id % cols) * C, cy = RSX_UI_TROPHY_SHEET_Y + (id / cols) * C;
    if (cy + C > RSX_UI_SHEET_H) return;
    LOCK();
    for (int y = 0; y < C; y++) {
        const int sy0 = y * h / C, sy1 = (y + 1) * h / C > sy0 ? (y + 1) * h / C : sy0 + 1;
        for (int x = 0; x < C; x++) {
            const int sx0 = x * w / C, sx1 = (x + 1) * w / C > sx0 ? (x + 1) * w / C : sx0 + 1;
            unsigned acc[4] = { 0, 0, 0, 0 }, cnt = 0;
            for (int yy = sy0; yy < sy1 && yy < h; yy++)
                for (int xx = sx0; xx < sx1 && xx < w; xx++) {
                    const unsigned char* p = px + ((size_t)yy * w + xx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; cnt++;
                }
            if (!cnt) cnt = 1;
            unsigned char* d = s_sheet + ((size_t)(cy + y) * RSX_UI_SHEET_W + cx + x) * 4;
            const unsigned a = acc[3] / cnt;   /* over the panel's colour */
            d[0] = (unsigned char)((acc[0] / cnt) * a / 255 + 16 * (255 - a) / 255);
            d[1] = (unsigned char)((acc[1] / cnt) * a / 255 + 18 * (255 - a) / 255);
            d[2] = (unsigned char)((acc[2] / cnt) * a / 255 + 26 * (255 - a) / 255);
            d[3] = 255;
        }
    }
    s_tr.has_icon[id] = 1;
    SDL_MemoryBarrierRelease();
    s_sheet_gen++;
    UNLOCK();
}

static uint32_t tr_grade_color(int g)
{
    return g == 1 ? rgba(186, 214, 240, 255) : g == 2 ? rgba(240, 192, 64, 255)
         : g == 3 ? rgba(200, 204, 214, 255) : rgba(212, 132, 62, 255);
}
static const char* tr_grade_name(int g)
{
    return g == 1 ? "Platinum" : g == 2 ? "Gold" : g == 3 ? "Silver" : g == 4 ? "Bronze" : "";
}

/* A trophy's icon at (x, y), side s; dimmed while locked. Under the lock. */
static void tr_icon(ui_out* o, int i, float x, float y, float s, int locked, int show)
{
    const rsx_ui_font* f = o->f;
    const int id = s_tr.t[i].id;
    if (show && id >= 0 && id < RSX_UI_TROPHY_MAX && s_tr.has_icon[id]) {
        const int C = RSX_UI_TROPHY_CELL, cols = RSX_UI_SHEET_W / C;
        const float ax = (float)((id % cols) * C), ay = (float)(f->atlas_h + RSX_UI_TROPHY_SHEET_Y + (id / cols) * C);
        quad(o, x, y, x + s, y + s, (ax + 0.5f) / o->aw, (ay + 0.5f) / o->ah, (ax + C - 0.5f) / o->aw, (ay + C - 0.5f) / o->ah,
             locked ? rgba(95, 95, 105, 255) : rgba(255, 255, 255, 255));
    } else {
        rect(o, x, y, x + s, y + s, rgba(34, 38, 52, 255));
        const char* q = "?";
        text(o, x + (s - text_w(f, q)) * 0.5f, y + (s - (float)f->line_h) * 0.5f, q, rgba(150, 150, 165, 255));
    }
    rect(o, x, y + s - (float)f->px * 0.14f, x + s, y + s, tr_grade_color(s_tr.t[i].grade));   /* the grade, as a stripe */
}

/* The notice: top right, slides in, stays 5 s, fades. */
static void tr_popup(ui_out* o, int sw, int sh)
{
    (void)sh;
    const rsx_ui_font* f = o->f;
    const float px = (float)f->px, lh = (float)f->line_h;
    LOCK();
    if (!s_tr.pop_n) { UNLOCK(); return; }
    const unsigned long long t = now_ms();
    if (!s_tr.pop_t0) s_tr.pop_t0 = t;
    const unsigned long long age = t - s_tr.pop_t0;
    const unsigned long long life = 5600;
    if (age >= life) {   /* the next one */
        for (int k = 1; k < s_tr.pop_n; k++) s_tr.pop[k - 1] = s_tr.pop[k];
        s_tr.pop_n = s_tr.pop_n - 1;
        s_tr.pop_t0 = s_tr.pop_n ? t : 0;
        UNLOCK();
        return;
    }
    const int i = s_tr.pop[0];
    const rsx_trophy* tr = &s_tr.t[i];
    char head[64], name[160];
    snprintf(head, sizeof head, "%s trophy unlocked", tr_grade_name(tr->grade));
    const float isz = lh * 2.3f, pad = px * 0.6f;
    float tw = text_w(f, head);
    const float maxw = (float)sw * 0.42f;
    fit_text(f, tr->name, maxw, name, sizeof name);
    if (text_w(f, name) > tw) tw = text_w(f, name);
    const float pw = pad + isz + pad + tw + pad * 1.4f, ph = isz + pad * 2.0f;
    float slide = 0.0f, alpha = 1.0f;
    if (age < 280) { const float k = 1.0f - (float)age / 280.0f; slide = k * k * (pw + px); }
    if (age > life - 450) alpha = (float)(life - age) / 450.0f;
    const float x1 = (float)sw - px * 0.8f + slide, x0 = x1 - pw, y0 = px * 0.8f;
    const int a = (int)(alpha * 255.0f);
    rect(o, x0, y0, x1, y0 + ph, rgba(16, 18, 26, (int)(alpha * 236.0f)));
    rect(o, x0, y0, x0 + px * 0.16f, y0 + ph, (tr_grade_color(tr->grade) & 0x00FFFFFFu) | ((uint32_t)a << 24));
    {   /* the icon, faded with the panel */
        const int id = tr->id;
        if (id >= 0 && id < RSX_UI_TROPHY_MAX && s_tr.has_icon[id]) {
            const int C = RSX_UI_TROPHY_CELL, cols = RSX_UI_SHEET_W / C;
            const float ax = (float)((id % cols) * C), ay = (float)(f->atlas_h + RSX_UI_TROPHY_SHEET_Y + (id / cols) * C);
            quad(o, x0 + pad, y0 + pad, x0 + pad + isz, y0 + pad + isz, (ax + 0.5f) / o->aw, (ay + 0.5f) / o->ah,
                 (ax + C - 0.5f) / o->aw, (ay + C - 0.5f) / o->ah, rgba(255, 255, 255, a));
        } else rect(o, x0 + pad, y0 + pad, x0 + pad + isz, y0 + pad + isz, (tr_grade_color(tr->grade) & 0x00FFFFFFu) | ((uint32_t)a << 24));
    }
    const float tx = x0 + pad + isz + pad;
    const float ty = y0 + pad + (isz - lh * 2.0f) * 0.5f;
    text(o, tx, ty, head, (tr_grade_color(tr->grade) & 0x00FFFFFFu) | ((uint32_t)a << 24));
    text(o, tx, ty + lh, name, rgba(240, 240, 245, a));
    UNLOCK();
}

/* The Trophies page, in the menu's content area [x0, x1) x [y, y1). */
static void tr_page(ui_out* o, float x0, float x1, float y, float y1, float pad)
{
    const rsx_ui_font* f = o->f;
    const float px = (float)f->px, lh = (float)f->line_h;
    const uint32_t white = rgba(235, 235, 240, 255), grey = rgba(150, 150, 160, 255), accent = rgba(242, 140, 40, 255);
    LOCK();
    const int n = s_tr.n;
    if (!n) {
        UNLOCK();
        text(o, x0 + pad, y + lh * 0.5f, "No trophy set: the disc's PS3_GAME\\TROPDIR is missing from the game folder.", grey);
        return;
    }
    int got = 0, all[5] = { 0 }, gotg[5] = { 0 }, pts = 0, pts_all = 0;
    static const int k_pts[5] = { 0, 180, 90, 30, 15 };   /* the console's weights for the progress */
    for (int i = 0; i < n; i++) {
        const int g = s_tr.t[i].grade >= 1 && s_tr.t[i].grade <= 4 ? s_tr.t[i].grade : 0;
        all[g]++; pts_all += g == 1 ? 0 : k_pts[g];
        if (s_tr.t[i].when) { got++; gotg[g]++; pts += g == 1 ? 0 : k_pts[g]; }
    }
    const int pct = pts_all ? pts * 100 / pts_all : got * 100 / n;
    char b[200];
    /* Title, count and progress */
    snprintf(b, sizeof b, "%d of %d unlocked   %d%%", got, n, pct);
    const float bw = text_w(f, b);
    char title[128]; fit_text(f, s_tr.title, x1 - x0 - pad * 3.0f - bw, title, sizeof title);
    text(o, x0 + pad, y, title, white);
    text(o, x1 - pad - bw, y, b, accent);
    y += lh * 1.15f;
    rect(o, x0 + pad, y, x1 - pad, y + px * 0.32f, rgba(40, 44, 58, 255));
    rect(o, x0 + pad, y, x0 + pad + (x1 - x0 - pad * 2.0f) * (float)pct / 100.0f, y + px * 0.32f, accent);
    y += px * 0.6f;
    {   /* per grade */
        float gx = x0 + pad;
        for (int g = 1; g <= 4; g++) {
            snprintf(b, sizeof b, "%s %d/%d", tr_grade_name(g), gotg[g], all[g]);
            rect(o, gx, y + lh * 0.3f, gx + px * 0.5f, y + lh * 0.3f + px * 0.5f, tr_grade_color(g));
            text(o, gx + px * 0.8f, y, b, grey);
            gx += px * 0.8f + text_w(f, b) + px * 1.6f;
        }
    }
    y += lh * 1.35f;
    /* The list */
    const float row_h = lh * 2.25f, isz = row_h - px * 0.45f;
    int vis = (int)((y1 - y) / row_h);
    if (vis < 1) vis = 1;
    s_tr.vis = vis;
    if (s_tr.sel >= n) s_tr.sel = n - 1;
    if (s_tr.top > s_tr.sel) s_tr.top = s_tr.sel;
    if (s_tr.sel >= s_tr.top + vis) s_tr.top = s_tr.sel - vis + 1;
    if (s_tr.top > n - vis) s_tr.top = n - vis > 0 ? n - vis : 0;
    for (int r = 0; r < vis && s_tr.top + r < n; r++) {
        const int i = s_tr.top + r;
        const rsx_trophy* t = &s_tr.t[i];
        const float ry = y + r * row_h;
        const int locked = !t->when, secret = locked && t->hidden && !s_tr.show_hidden;
        if (i == s_tr.sel) rect(o, x0 + pad * 0.5f, ry, x1 - pad * 0.5f - px * 0.6f, ry + row_h - px * 0.1f, rgba(242, 140, 40, 60));
        tr_icon(o, i, x0 + pad, ry + px * 0.18f, isz, locked, !secret);
        const float tx = x0 + pad + isz + px * 0.8f, right = x1 - pad - px * 0.6f;
        const char* gname = tr_grade_name(t->grade);
        const float gw = text_w(f, gname);
        char nm[160], dt[300], when[48];
        fit_text(f, secret ? "Hidden trophy" : t->name, right - tx - gw - px, nm, sizeof nm);
        text(o, tx, ry + px * 0.12f, nm, locked ? rgba(175, 175, 185, 255) : white);
        text(o, right - gw, ry + px * 0.12f, gname, tr_grade_color(t->grade));
        if (t->when > 1) {
            const time_t tt = (time_t)t->when;
            struct tm lt;
#ifdef _WIN32
            localtime_s(&lt, &tt);
#else
            localtime_r(&tt, &lt);
#endif
            strftime(when, sizeof when, "%Y-%m-%d", &lt);
        } else snprintf(when, sizeof when, "%s", locked ? "Locked" : "Unlocked");
        const float ww = text_w(f, when);
        fit_text(f, secret ? "Its details show once it is unlocked." : t->detail, right - tx - ww - px, dt, sizeof dt);
        text(o, tx, ry + px * 0.12f + lh, dt, grey);
        text(o, right - ww, ry + px * 0.12f + lh, when, locked ? rgba(110, 110, 120, 255) : accent);
    }
    if (n > vis) {   /* scroll bar */
        const float sx = x1 - pad * 0.5f - px * 0.35f, h = y1 - y;
        rect(o, sx, y, sx + px * 0.25f, y1, rgba(40, 44, 58, 255));
        rect(o, sx, y + h * (float)s_tr.top / (float)n, sx + px * 0.25f, y + h * (float)(s_tr.top + vis) / (float)n, accent);
    }
    UNLOCK();
}

int rsx_host_ui_build(const rsx_ui_font* f, int sw, int sh, rsx_ui_vtx* out, int cap)
{
    if (!f || !f->pixels || sw <= 0 || sh <= 0) return 0;
    ui_out o = { out, 0, cap, f, 1.0f, 1.0f };
    { int aw, ah; rsx_host_ui_atlas_size(f, &aw, &ah); o.aw = (float)aw; o.ah = (float)ah; }
    rsx_host_options opt; rsx_host_ui_options(&opt);
    const float px = (float)f->px, lh = (float)f->line_h;

    if (s_sv_active) { sv_build(&o, sw, sh); return o.n; }
    if (opt.fps && !s_open) {
        char b[64]; LOCK(); memcpy(b, s_fps_text, sizeof b); UNLOCK();
        if (!b[0]) snprintf(b, sizeof b, "-- FPS");
        const float x = px * 0.6f, y = px * 0.5f, w = text_w(f, b);
        rect(&o, x - px * 0.4f, y - px * 0.15f, x + w + px * 0.4f, y + lh + px * 0.1f, rgba(0, 0, 0, 150));
        text(&o, x, y, b, rgba(255, 230, 120, 255));
    }
    if (!s_open) { tr_popup(&o, sw, sh); return o.n; }

    /* Dim the game, then a centred panel. */
    rect(&o, 0, 0, (float)sw, (float)sh, rgba(0, 0, 0, 120));
#ifdef __ANDROID__
    const char* l1 = s_page == TR_PAGE ? (s_tr.show_hidden ? "Up/Down  scroll     Cross  hide the hidden trophies' details"
                                                           : "Up/Down  scroll     Cross  show the hidden trophies' details")
                                       : "Up/Down  choose     Left/Right  change     Cross  select";
    const char* l2 = "L1 R1 / tap a tab  page     Circle  close";
#else
    const int kbm_row = s_sel >= 0 && s_sel < N_ROWS && (k_rows[s_sel].id == R_KBM || k_rows[s_sel].id == R_MSENS || k_rows[s_sel].id == R_MINV);
    const char* l1 = s_page == TR_PAGE ? (s_tr.show_hidden ? "Up/Down  scroll     Enter / Cross  hide the hidden trophies' details"
                                                           : "Up/Down  scroll     Enter / Cross  show the hidden trophies' details")
                   : kbm_row ? "Click in the game to turn the camera with the mouse; F1 opens this menu"
                             : "Up/Down  choose     Left/Right  change     Enter / Cross  select";
    const char* l2 = "Tab / L1 R1  page     Esc / Circle  close     F11  fullscreen";
#endif
    /* Spacing, in lines; squeezed (never the text) when the screen is short. */
    float row_k = 1.30f, head_k = 1.45f, title_k = 1.9f, foot_k = 2.6f;
    const float pad = px * 1.1f;
    float units = title_k + foot_k;
    units += rows_height(head_k, row_k);   /* the longest page: switching pages does not move the panel */
    const float avail = (float)sh * 0.96f - pad * 1.5f;
    if (units * lh > avail) {
        const float k = avail / (units * lh) < 0.78f ? 0.78f : avail / (units * lh);
        row_k *= k; head_k *= k; title_k *= k; foot_k *= k;
    }
    const float row_h = lh * row_k, head_h = lh * head_k;
    const float content_h = rows_height(head_h, row_h);
    const float title_h = lh * title_k, foot_h = lh * foot_k;
    float pw = px * 30.0f;
    { const float need = (text_w(f, l1) > text_w(f, l2) ? text_w(f, l1) : text_w(f, l2)) + pad * 2.0f;
      if (pw < need) pw = need; }
    if (s_page == TR_PAGE) {   /* both footers of the Trophies page: the panel stays put when it changes */
        static const char* const both[2] = { "Up/Down  scroll     Enter / Cross  show the hidden trophies' details",
                                             "Up/Down  scroll     Enter / Cross  hide the hidden trophies' details" };
        for (int k = 0; k < 2; k++) if (pw < text_w(f, both[k]) + pad * 2.0f) pw = text_w(f, both[k]) + pad * 2.0f;
    }
    {   /* the title and every page tab */
        float need = pad * 2.0f + text_w(f, "Settings") + px * 1.6f;
        for (int k = 0; k < N_PAGES; k++) need += text_w(f, k_page_names[k]) + px * 1.4f;
        if (pw < need) pw = need;
    }
    if (pw > sw * 0.96f) pw = sw * 0.96f;
    const float ph = pad + title_h + content_h + foot_h + pad * 0.5f;
    const float x0 = ((float)sw - pw) * 0.5f, y0 = ((float)sh - ph) * 0.5f, x1 = x0 + pw;
    rect(&o, x0, y0, x1, y0 + ph, rgba(16, 18, 26, 236));
    rect(&o, x0, y0, x1, y0 + px * 0.18f, rgba(242, 140, 40, 255));            /* accent bar */
    text(&o, x0 + pad, y0 + pad * 0.6f, "Settings", rgba(255, 178, 80, 255));
    {   /* the page tabs */
        const char* const* tabs = k_page_names;
        float tx = x0 + pad + text_w(f, "Settings") + px * 1.6f;
        const float ty = y0 + pad * 0.6f;
        LOCK(); s_hit.ty0 = (int)(ty - px * 0.2f); s_hit.ty1 = (int)(ty + lh + px * 0.2f); UNLOCK();
        for (int k = 0; k < N_PAGES; k++) {
            const float tw = text_w(f, tabs[k]);
            if (k == s_page) {
                rect(&o, tx - px * 0.4f, ty - px * 0.15f, tx + tw + px * 0.4f, ty + lh + px * 0.05f, rgba(242, 140, 40, 230));
                text(&o, tx, ty, tabs[k], rgba(20, 16, 10, 255));
            } else text(&o, tx, ty, tabs[k], rgba(200, 200, 210, 255));
            LOCK(); s_hit.tx0[k] = (int)(tx - px * 0.4f); s_hit.tx1[k] = (int)(tx + tw + px * 0.4f); UNLOCK();
            tx += tw + px * 1.4f;
        }
        const char* sub = "Changes apply at once and are saved";
        if (tx + text_w(f, sub) < x1 - pad)
            text(&o, x1 - pad - text_w(f, sub), y0 + pad * 0.6f, sub, rgba(150, 150, 160, 255));
    }

    float y = y0 + pad + title_h;
    const float foot_y = y + content_h;   /* the footer stays put on a shorter page */
    LOCK(); s_hit.x0 = (int)x0; s_hit.x1 = (int)x1; s_hit.n = s_page == TR_PAGE ? 0 : N_ROWS; UNLOCK();
    if (s_page == TR_PAGE) tr_page(&o, x0, x1, y, foot_y - lh * 0.3f, pad);
    char val[96];
    for (int i = 0; i < N_ROWS && s_page != TR_PAGE; i++) {
        const int kind = k_rows[i].kind;
        const float h = kind == K_HEAD ? head_h : kind == K_GAP ? row_h * 0.5f : row_h;
        LOCK(); if (i < 32) { s_hit.y0[i] = (int)y; s_hit.y1[i] = (int)(y + h); } UNLOCK();
        if (kind == K_HEAD) {
            text(&o, x0 + pad, y + (h - lh) * 0.75f, k_rows[i].label, rgba(242, 140, 40, 255));
            rect(&o, x0 + pad, y + h - px * 0.12f, x1 - pad, y + h - px * 0.06f, rgba(242, 140, 40, 90));
        } else if (kind != K_GAP) {
            const int en = row_enabled(i, &opt), sel = (i == s_sel);
            const float ty = y + (h - lh) * 0.5f;
            if (sel) rect(&o, x0 + pad * 0.5f, y + px * 0.06f, x1 - pad * 0.5f, y + h - px * 0.06f, rgba(242, 140, 40, 230));
            const uint32_t lc = sel ? rgba(20, 16, 10, 255) : en ? rgba(235, 235, 240, 255) : rgba(110, 110, 120, 255);
            const uint32_t vc = sel ? rgba(20, 16, 10, 255) : en ? rgba(255, 205, 140, 255) : rgba(110, 110, 120, 255);
            text(&o, x0 + pad, ty, k_rows[i].label, lc);
            value_text(k_rows[i].id, &opt, val, sizeof val);
            if (val[0]) {
                float vw = text_w(f, val), vx = x1 - pad - vw;
                if (kind == K_OPT && sel) {
                    const float aw = text_w(f, ">");
                    text(&o, x1 - pad - aw, ty, ">", vc);
                    vx -= aw + px * 0.6f;
                    text(&o, vx - px * 0.6f - text_w(f, "<"), ty, "<", vc);
                }
                text(&o, vx, ty, val, vc);
            }
        }
        y += h;
    }
    text(&o, x0 + (pw - text_w(f, l1)) * 0.5f, foot_y + foot_h * 0.17f, l1, rgba(150, 150, 160, 255));
    text(&o, x0 + (pw - text_w(f, l2)) * 0.5f, foot_y + foot_h * 0.17f + lh, l2, rgba(150, 150, 160, 255));

    /* Mouse coordinates arrive in window points; the layout is in pixels. */
    if (s_win) {
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(s_win, &ww, &wh);
        SDL_Vulkan_GetDrawableSize(s_win, &dw, &dh);
        LOCK();
        s_hit.sx = ww > 0 ? (float)dw / (float)ww : 1.0f;
        s_hit.sy = wh > 0 ? (float)dh / (float)wh : 1.0f;
        UNLOCK();
    }
    tr_popup(&o, sw, sh);
    return o.n;
}

/* ---------------------------------------------------------------------------
 * Test hook (RACACIT_UI_FILE)
 * -------------------------------------------------------------------------*/
static char s_shot_req[512];
/* "burst <n> <dir>": the next n presented frames, <dir>/b<k>_<host us>_<flip>.ppm
 * (host microseconds at the present, the game's flip count) -- how far a moving
 * object travels from one frame to the next. */
static char s_burst_dir[480];
static int s_burst_n, s_burst_k;

int rsx_host_ui_take_shot(char* out, int n)
{
    LOCK();
    int r = s_shot_req[0] != 0;
    if (r) { snprintf(out, (size_t)n, "%s", s_shot_req); s_shot_req[0] = 0; }
    else if (s_burst_n > 0) {
        extern unsigned long long ps3_qpc_us(void);
        extern unsigned cellGcm_flip_request_count(void);
        snprintf(out, (size_t)n, "%s/b%04d_%llu_%u.ppm", s_burst_dir, s_burst_k++, ps3_qpc_us(), cellGcm_flip_request_count());
        s_burst_n--; r = 1;
    }
    UNLOCK();
    return r;
}

static volatile int s_menu_req;
void rsx_host_ui_request_menu(void) { s_menu_req = 1; }

void rsx_host_ui_poll_script(void)
{
    if (s_menu_req) { s_menu_req = 0; if (!s_open) { s_menu_src = "Select held"; open_menu(1); } }
    static const char* path = (const char*)-1;
    if (path == (const char*)-1) path = getenv("RACACIT_UI_FILE");
    if (!path || !*path) return;
    static unsigned long long next;
    const unsigned long long t = now_ms();
    if (t < next) return;
    next = t + 50;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char first[600] = "", rest[8192]; size_t nrest = 0;
    if (!fgets(first, sizeof first, f)) first[0] = 0;
    nrest = fread(rest, 1, sizeof rest - 1, f);
    fclose(f);
    if (!first[0]) return;
    if ((f = fopen(path, "wb"))) { fwrite(rest, 1, nrest, f); fclose(f); }
    char cmd[16] = "", arg[580] = "";
    if (sscanf(first, "%15s %579[^\r\n]", cmd, arg) < 1) return;
    SDL_Event e; memset(&e, 0, sizeof e);
    if (!strcmp(cmd, "key")) {
        static const struct { const char* n; SDL_Keycode k; } keys[] = {
            { "F1", SDLK_F1 }, { "F11", SDLK_F11 }, { "ESC", SDLK_ESCAPE }, { "UP", SDLK_UP }, { "DOWN", SDLK_DOWN },
            { "LEFT", SDLK_LEFT }, { "RIGHT", SDLK_RIGHT }, { "ENTER", SDLK_RETURN }, { "TAB", SDLK_TAB },
            { "PGUP", SDLK_PAGEUP }, { "PGDN", SDLK_PAGEDOWN } };
        for (int i = 0; i < N_OF(keys); i++)
            if (!strcmp(arg, keys[i].n)) { e.type = SDL_KEYDOWN; e.key.keysym.sym = keys[i].k; rsx_host_ui_event(&e); }
    } else if (!strcmp(cmd, "pad")) {
        static const struct { const char* n; int b; } pads[] = {
            { "GUIDE", SDL_CONTROLLER_BUTTON_GUIDE }, { "A", SDL_CONTROLLER_BUTTON_A }, { "B", SDL_CONTROLLER_BUTTON_B },
            { "Y", SDL_CONTROLLER_BUTTON_Y },
            { "START", SDL_CONTROLLER_BUTTON_START }, { "UP", SDL_CONTROLLER_BUTTON_DPAD_UP },
            { "DOWN", SDL_CONTROLLER_BUTTON_DPAD_DOWN }, { "LEFT", SDL_CONTROLLER_BUTTON_DPAD_LEFT },
            { "RIGHT", SDL_CONTROLLER_BUTTON_DPAD_RIGHT }, { "LB", SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
            { "RB", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER } };
        for (int i = 0; i < N_OF(pads); i++)
            if (!strcmp(arg, pads[i].n)) { e.type = SDL_CONTROLLERBUTTONDOWN; e.cbutton.which = -1;
                                           e.cbutton.button = (Uint8)pads[i].b; rsx_host_ui_event(&e); }
    } else if (!strcmp(cmd, "shot")) {
        LOCK(); snprintf(s_shot_req, sizeof s_shot_req, "%s", arg); UNLOCK();
    } else if (!strcmp(cmd, "burst")) {
        int bn = 0; char dir[480] = "";
        if (sscanf(arg, "%d %479s", &bn, dir) == 2) { LOCK(); snprintf(s_burst_dir, sizeof s_burst_dir, "%s", dir); s_burst_n = bn; s_burst_k = 0; UNLOCK(); }
    } else if (!strcmp(cmd, "hold")) {
        /* "hold <ms> <key>": a key (SDL name) or Mouse1-5 held for the game. */
        int ms = 0, nn = 0;
        if (sscanf(arg, "%d %n", &ms, &nn) >= 1 && arg[nn]) {
            const char* name = arg + nn;
            int key = -1;
            if (!SDL_strncasecmp(name, "Mouse", 5) && name[5] >= '1' && name[5] <= '5' && !name[6]) key = 512 + (name[5] - '0');
            else { const SDL_Scancode sc = SDL_GetScancodeFromName(name); if (sc != SDL_SCANCODE_UNKNOWN) key = (int)sc; }
            if (key >= 0) {
                LOCK();
                int slot = 0;
                for (int i = 0; i < TEST_KEYS; i++) if (!s_tkey[i].until || s_tkey[i].key == key) { slot = i; break; }
                s_tkey[slot].key = key; s_tkey[slot].until = t + (unsigned long long)(ms > 0 ? ms : 1);
                s_tkey_n++;
                UNLOCK();
            } else fprintf(stderr, "[ui script] hold: unknown key \"%s\"%c", name, 10);
        }
    } else if (!strcmp(cmd, "mouse")) {
        /* "mouse <dx> <dy>": camera motion as from a captured mouse. */
        int dx = 0, dy = 0;
        if (sscanf(arg, "%d %d", &dx, &dy) == 2) { LOCK(); s_mdx += dx; s_mdy += dy; s_mtest = 1; UNLOCK(); }
    } else if (!strcmp(cmd, "wait")) {
        next = t + (unsigned long long)atoi(arg);
    }
}

#else  /* no SDL: options from the environment only, no menu */

static rsx_host_options s_opt;
void rsx_host_ui_load_options(void)
{
    s_opt.res_scale = 1.0f; s_opt.keep_aspect = 1; s_opt.discord = 1;
    { const char* v = getenv("RACACIT_DS_SPEAKER"); s_opt.ds_speaker = !(v && *v == '0'); }
    { const char* v = getenv("RACACIT_DS_SPK_VOL"); int p = v && *v ? atoi(v) : 49; s_opt.ds_volume = p < 0 ? 0 : p > 100 ? 100 : p; }
    { const char* v = getenv("RACACIT_DS_HAPTIC_VOL"); int p = v && *v ? atoi(v) : 50; s_opt.ds_haptic_vol = p < 0 ? 0 : p > 100 ? 100 : p; }
    { const char* v = getenv("RACACIT_DS_RUMBLE"); int p = v && *v ? atoi(v) : 100; s_opt.ds_rumble = p < 0 ? 0 : p > 100 ? 100 : p; }
    { const char* v = getenv("RACACIT_DS_HAPTICS"); s_opt.ds_haptics = !(v && *v == '0'); }
    { const char* v = getenv("RACACIT_DS_TRIGGERS"); s_opt.ds_triggers = !(v && *v == '0'); }
    { const char* v = getenv("RACACIT_DS_TRIG_STR"); int p = v && *v ? atoi(v) : 100; s_opt.ds_trig_str = p < 10 ? 10 : p > 100 ? 100 : p; }
    { const char* v = getenv("RACACIT_DS_SWAP"); s_opt.ds_swap = !(v && *v == '0'); }
    { const char* v = getenv("RACACIT_DS_LIGHT"); s_opt.ds_lightbar = !(v && *v == '0'); }
    { const char* v = getenv("RACACIT_DS_MOTION"); s_opt.ds_motion = !(v && *v == '0'); }
    { const char* v = getenv("RACACIT_DUALSENSE_PLUS"); s_opt.ds_plus = v && *v == '1'; }
    { const char* v = getenv("RACACIT_KBM"); s_opt.kbm = !(v && *v == '0'); }
    { const char* v = getenv("RACACIT_MOUSE_SENS"); s_opt.mouse_sens = v && atof(v) > 0.0 ? (int)(atof(v) * 100.0 + 0.5) : 100; }
    { const char* v = getenv("RACACIT_MOUSE_INVERT_Y"); s_opt.mouse_invert = v && *v == '1'; }
    { const char* v = getenv("RACACIT_FOV"); int p = v && *v ? atoi(v) : 100; s_opt.fov = p < 75 ? 75 : p > 150 ? 150 : p; }
    { const char* v = getenv("RACACIT_CAM_DIST"); int p = v && *v ? atoi(v) : 100; s_opt.cam_dist = p < 50 ? 50 : p > 200 ? 200 : p; }
}
void rsx_host_ui_options(rsx_host_options* out)
{
    *out = s_opt;
    if (!out->ds_plus) out->ds_speaker = out->ds_haptics = out->ds_triggers = out->ds_swap = out->ds_lightbar = out->ds_motion = 0;
}
void rsx_host_ui_attach(struct SDL_Window* w, float a, float r) { (void)w; (void)a; (void)r; }
void rsx_host_ui_initial_window(int* w, int* h, unsigned* f) { *w = 1280; *h = 720; *f = 0; }
int  rsx_host_ui_event(const union SDL_Event* e) { (void)e; return 0; }
int  rsx_host_ui_capturing(void) { return 0; }
void rsx_host_ui_request_menu(void) {}
int  rsx_host_ui_visible(void) { return 0; }
void rsx_host_ui_on_present(void) {}
int  rsx_host_ui_font_for(rsx_ui_font* f, int w, int h) { (void)f; (void)w; (void)h; return 0; }
int  rsx_host_ui_build(const rsx_ui_font* f, int sw, int sh, rsx_ui_vtx* out, int cap)
{ (void)f; (void)sw; (void)sh; (void)out; (void)cap; return 0; }
void rsx_host_ui_poll_script(void) {}
int  rsx_host_ui_take_shot(char* out, int n) { (void)out; (void)n; return 0; }
int  rsx_host_ui_save_pick(int s, rsx_save_entry* e, int n, int a, int i, int (*d)(const char*))
{ (void)s; (void)e; (void)n; (void)a; (void)i; (void)d; return RSX_SAVE_PICK_UNAVAILABLE; }
int  rsx_host_ui_modal(void) { return 0; }
volatile int g_racacit_kbm_on;
int  rsx_host_ui_mouse_take(int* dx, int* dy, int* wheel, unsigned* buttons) { *dx = *dy = *wheel = 0; *buttons = 0; return 0; }
int  rsx_host_ui_test_key(int key) { (void)key; return 0; }
unsigned rsx_host_ui_sheet(unsigned char* out) { (void)out; return 0; }
void rsx_host_ui_atlas_size(const rsx_ui_font* f, int* w, int* h) { *w = f->atlas_w; *h = f->atlas_h; }
void rsx_host_ui_trophy_set(const char* t, const rsx_trophy* tr, int n) { (void)t; (void)tr; (void)n; }
void rsx_host_ui_trophy_unlocked(int id, long long when, int notify) { (void)id; (void)when; (void)notify; }
void rsx_host_ui_trophy_icon(int id, const unsigned char* px, int w, int h) { (void)id; (void)px; (void)w; (void)h; }

#endif
