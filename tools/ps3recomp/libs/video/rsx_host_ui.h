/*
 * ps3recomp - host settings menu and on-screen display (RaCACiT)
 *
 * The in-game "Settings" overlay a PC port is expected to have: display mode
 * (windowed / borderless / exclusive fullscreen), window size, VSync, aspect
 * ratio, upscaling filter and sharpness, brightness, internal resolution,
 * texture filtering and an FPS counter. Opened with F1 or Esc on the
 * keyboard, the PS / Guide button or the touchpad on a controller, or L3+R3
 * pressed together. F11 or Alt+Enter toggles fullscreen at any time.
 *
 * Split in two:
 *   - this module (platform-neutral C + SDL2): the option values, loading
 *     them from the environment (settings.txt is loaded into it at start) and
 *     writing them back to the file named by RACACIT_SETTINGS_FILE, input,
 *     window-mode changes, the font atlas (CPU side) and the menu layout as a
 *     list of textured, coloured quads in screen pixels.
 *   - the Vulkan backend: the GPU side -- the atlas texture, the pass that
 *     draws those quads, the upscaling/letterbox pass, present mode and the
 *     sampler quality options (rsx_vulkan_backend.c).
 *
 * Threads: rsx_host_ui_event() and rsx_host_ui_attach() run on the thread
 * that owns the window and pumps its events; rsx_host_ui_build(),
 * rsx_host_ui_on_present() and rsx_host_ui_options() on the render worker;
 * rsx_host_ui_capturing() from the pad library on guest threads. Shared state
 * is behind one lock.
 */
#ifndef PS3RECOMP_RSX_HOST_UI_H
#define PS3RECOMP_RSX_HOST_UI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RSX_UI_DISPLAY_WINDOWED = 0, RSX_UI_DISPLAY_BORDERLESS = 1, RSX_UI_DISPLAY_EXCLUSIVE = 2 };
enum { RSX_UI_VSYNC_MAILBOX = 0, RSX_UI_VSYNC_FIFO = 1, RSX_UI_VSYNC_IMMEDIATE = 2 };
enum { RSX_UI_UPSCALE_BILINEAR = 0, RSX_UI_UPSCALE_SHARP = 1, RSX_UI_UPSCALE_NEAREST = 2 };

typedef struct rsx_host_options {
    int   display_mode;   /* RSX_UI_DISPLAY_*                                  */
    int   win_w, win_h;   /* windowed size, 0 = default                        */
    int   vsync;          /* RSX_UI_VSYNC_*                                     */
    int   keep_aspect;    /* 1: 16:9 with black bars, 0: stretch to the window */
    int   upscale;        /* RSX_UI_UPSCALE_*                                   */
    int   sharpness;      /* 0..10 (sharp upscaling only)                      */
    int   brightness;     /* -5..+5, 0 = as the game draws it                  */
    float res_scale;      /* internal resolution asked for (applies at start)  */
    int   aniso;          /* 0 (off), 2, 4, 8, 16                              */
    int   trilinear;      /* 0 / 1                                             */
    int   fps;            /* 0 off, 1 FPS counter                              */
    int   discord;        /* 1: Discord status on (when the build has an app) */
    int   skip_clank;     /* 1: a load into a Great Clock (Clank) level goes on to the next level (testing) */
    int   armor_look;     /* 0: the armor worn; 1-4: Holoflux..Hyperflux's look, the worn armor's protection -- picked in the game's Skins menu (game/stubs.cpp) */
    int   ds_speaker;     /* 1: the DualSense speaker plays (cellAudio.c, ds_player.c) */
    int   ds_volume;      /* its volume, 0..100 percent                       */
    int   ds_haptics;     /* 1: weapons, impacts and rumble felt on the actuators (cellAudio.c) */
    int   ds_haptic_vol;  /* their strength, 0..100 percent                   */
    int   ds_rumble;      /* the game's rumble, 0 (off)..100 percent (cellPad.c, cellAudio.c) */
    int   ds_triggers;    /* 1: adaptive trigger resistance and kicks (cellPad.c) */
    int   ds_trig_str;    /* their strength, 10..100 percent (v70)            */
    int   ds_swap;        /* 1: R2 fires, L2 aims (Rift Apart layout); 0: PS3 layout */
    int   ds_lightbar;    /* 1: light bar lit                                  */
    int   ds_motion;      /* 1: the motion sensors reach the game               */
    int   ds_plus;        /* 1: the DualSense Plus mod is on (RACACIT_DUALSENSE_PLUS=1, the launcher's
                           * mod sets it): speaker, haptics, adaptive triggers, light bar, motion and
                           * Fire on R2 with its prompts. 0 (default): every controller, a DualSense
                           * too, is a plain pad -- PS3 layout, the game's own prompts, rumble.
                           * rsx_host_ui_options() then reads ds_speaker, ds_haptics, ds_triggers,
                           * ds_swap, ds_lightbar and ds_motion as 0; the stored values stay. */
    int   tex_quality;    /* 0 high (as shipped), 1 medium, 2 low: top mip levels left out (applies at start) */
    int   cpu_mode;       /* 0 efficient (SPU lanes sleep after a short spin), 1 maximum (they spin) */
    int   present_pace;   /* 1: presents held to an even 60 Hz (rsx_be_proxy.c) */
    int   kbm;            /* 1: keyboard and mouse play (cellPad.c)            */
    int   mouse_sens;     /* mouse camera speed, percent (100 = default)       */
    int   mouse_invert;   /* 1: mouse up looks down                            */
    int   fov;            /* field of view, percent of the game's own (100 = as shipped, 75..150):
                           * the follow camera's angle (game/stubs.cpp racacit_follow_fov) */
    int   cam_dist;       /* the follow camera's distance behind Ratchet, percent of the
                           * game's (100 = as shipped, 50..200; racacit_follow_dist) */
    unsigned gen;          /* bumped on every change                           */
    unsigned swap_gen;     /* bumped when the swapchain must be rebuilt        */
    unsigned sampler_gen;  /* bumped when texture filtering changed            */
} rsx_host_options;

/* One vertex of the overlay: screen pixels, atlas UV, RGBA8 (R in the low byte). */
typedef struct rsx_ui_vtx { float x, y, u, v; uint32_t rgba; } rsx_ui_vtx;

/* CPU font atlas: R8 coverage. Glyph 127 is a solid block (for rectangles). */
typedef struct rsx_ui_glyph { short x, y, w, h, xoff, adv; } rsx_ui_glyph;
typedef struct rsx_ui_font {
    int px;                 /* the size it was built for (0 = none)  */
    int line_h, ascent;
    int atlas_w, atlas_h;
    unsigned char* pixels;  /* atlas_w * atlas_h                     */
    rsx_ui_glyph g[96];     /* characters 32..127                    */
    unsigned gen;           /* bumped whenever pixels change         */
} rsx_ui_font;

/* Read the options from the environment (settings.txt has been loaded into
 * it by then). Called once, before the window opens. Idempotent. */
void rsx_host_ui_load_options(void);

/* Snapshot of the current options (any thread). */
void rsx_host_ui_options(rsx_host_options* out);
void rsx_host_ui_set_armor_look(int look);   /* the Skins menu's armor entries; saved to settings.txt */

/* The window, once created (event thread). `max_aniso`: the device's limit,
 * `res_scale_active`: the internal resolution this run uses. */
struct SDL_Window;
void rsx_host_ui_attach(struct SDL_Window* win, float max_aniso, float res_scale_active);

/* Initial window size and flags the backend should create the window with. */
void rsx_host_ui_initial_window(int* w, int* h, unsigned* sdl_flags);

/* One SDL event (event thread). Returns 1 when the user chose "Quit game". */
union SDL_Event;
int rsx_host_ui_event(const union SDL_Event* e);

/* True while the menu owns the controls: the game then sees an idle pad.
 * Stays true after closing until every button has been released. */
int rsx_host_ui_capturing(void);

/* Open the menu at the next event pump (cellPad.c, guest threads: Select held
 * on a controller without a touchpad). */
void rsx_host_ui_request_menu(void);

/* Keyboard and mouse for the game (cellPad.c, guest threads). A click in the
 * window captures the mouse (relative mode) while cellPad has set
 * g_racacit_kbm_on; the menu, the save screen and focus loss release it.
 * mouse_take: motion, wheel and buttons (bit 0 = left ... 4) since the last
 * call; returns 1 while captured. test_key: 1 when the test hook holds `key`
 * (a scancode, or 512 + mouse button 1-5) -- or, for -1, any key. */
extern volatile int g_racacit_kbm_on;
int rsx_host_ui_mouse_take(int* dx, int* dy, int* wheel, unsigned* buttons);
int rsx_host_ui_test_key(int key);

/* Anything to draw this frame (menu open or the FPS counter on)? */
int rsx_host_ui_visible(void);

/* A frame went to the screen (render worker): feeds the FPS counter. */
void rsx_host_ui_on_present(void);

/* The font for a screen `screen_h` pixels tall: rebuilds `f` when the size
 * changed (returns 1 then: the GPU copy must be refreshed). */
int rsx_host_ui_font_for(rsx_ui_font* f, int screen_w, int screen_h);

/* Lay the overlay out for a sw x sh target: fills up to `cap` vertices
 * (triangle list) and returns how many. `f` must be the font from
 * rsx_host_ui_font_for for the same size. */
int rsx_host_ui_build(const rsx_ui_font* f, int sw, int sh, rsx_ui_vtx* out, int cap);

/* ---- Save data screen ------------------------------------------------------
 * The PS3 shows its own save list when a title asks for one (cellSaveData's
 * List calls); this is the native stand-in: the game's saves with their
 * ICON0 art, location and play time, a "New save" row when the title offers
 * one, overwrite and delete with confirmation. It runs on the guest thread
 * that made the call and blocks it until the player chooses, the way the
 * system dialog does; the game keeps rendering behind it and sees an idle
 * pad meanwhile. */
typedef struct rsx_save_entry {
    char dir[32];                 /* directory name                      */
    char title[128];              /* PARAM.SFO TITLE                     */
    char subtitle[128];           /* SUB_TITLE: A Crack in Time's place  */
    char detail[256];             /* DETAIL: its play time               */
    char when[40];                /* last written, local time            */
    long long mtime;              /* for sorting                         */
    const unsigned char* icon;    /* RGBA8 icon_w x icon_h, or NULL      */
    int icon_w, icon_h;
} rsx_save_entry;

enum { RSX_SAVE_PICK_UNAVAILABLE = -3, RSX_SAVE_PICK_NEW = -2, RSX_SAVE_PICK_CANCEL = -1 };
#define RSX_SAVE_MODE_DELETE 2   /* is_save value: the delete list (cellSaveDataDelete2) */

/* `is_save`: a save (overwrite asks first) or a load. `allow_new`: offer
 * "New save". `initial`: the row to start on (an entry index, or
 * RSX_SAVE_PICK_NEW). `del`, when not NULL, removes a save (returns 0 on
 * success). Returns the chosen entry's index, RSX_SAVE_PICK_NEW,
 * RSX_SAVE_PICK_CANCEL, or RSX_SAVE_PICK_UNAVAILABLE when there is no window
 * (or RACACIT_SAVE_UI=0): the caller then chooses on its own. Entries must
 * stay valid until it returns. */
int rsx_host_ui_save_pick(int is_save, rsx_save_entry* entries, int n, int allow_new, int initial,
                          int (*del)(const char* dir));

/* The save screen is up: the render loop keeps presenting while it is,
 * even if the game stops flipping. */
int rsx_host_ui_modal(void);

/* Trophies (sceNpTrophy.c sends them): the game's set as the Settings menu's
 * Trophies page lists it, and a notice in the corner when one unlocks. Text
 * is what the overlay's font can draw (ASCII; accents folded). */
#define RSX_UI_TROPHY_MAX 60
typedef struct rsx_trophy {
    int id, grade, hidden;        /* grade: 1 platinum, 2 gold, 3 silver, 4 bronze */
    long long when;               /* unlocked at (unix seconds), 0 = locked */
    char name[128];
    char detail[256];
} rsx_trophy;
void rsx_host_ui_trophy_set(const char* title, const rsx_trophy* t, int n);
/* One trophy unlocked: the list updated; `notify` shows the notice. */
void rsx_host_ui_trophy_unlocked(int id, long long when, int notify);
/* A trophy's icon (RGBA8), scaled into the sheet's trophy cells. */
void rsx_host_ui_trophy_icon(int id, const unsigned char* rgba, int w, int h);

/* The overlay's image sheet (RGBA8, RSX_UI_SHEET_W x RSX_UI_SHEET_H): the
 * save icons (rows 0-791) and the trophy icons (96x96 cells from row 800,
 * ten to a row). The GPU atlas is the font (as white, coverage in alpha) with
 * this sheet below it; rsx_host_ui_atlas_size gives its size. Returns the
 * sheet's generation (bumped when its pixels change) and copies the pixels
 * into `out` when it is not NULL. */
#define RSX_UI_SHEET_W 1024
#define RSX_UI_SHEET_H (800 + 6 * 96)
#define RSX_UI_TROPHY_SHEET_Y 800
#define RSX_UI_TROPHY_CELL 96
unsigned rsx_host_ui_sheet(unsigned char* out);
void rsx_host_ui_atlas_size(const rsx_ui_font* f, int* w, int* h);

/* Test hook (event thread): RACACIT_UI_FILE=<file> holds commands, one run
 * per call (at most every 50 ms) and removed from the file: "key F1|F11|ESC|
 * UP|DOWN|LEFT|RIGHT|ENTER|TAB|PGUP|PGDN", "pad GUIDE|A|B|Y|START|UP|DOWN|
 * LEFT|RIGHT|LB|RB", "hold <ms> <key name|Mouse1-5>" (held for the game's
 * keyboard bindings), "mouse <dx> <dy>" (captured-mouse motion), "shot
 * <path>" (the next composed frame as a PPM), "wait <ms>". */
void rsx_host_ui_poll_script(void);

/* A pending "shot" request: copies its path into `out` (render worker). */
int rsx_host_ui_take_shot(char* out, int n);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_HOST_UI_H */
