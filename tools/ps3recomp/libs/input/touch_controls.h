/*
 * ps3recomp - on-screen touch controls (phones without a gamepad)
 *
 * A fixed DualShock-style layout in normalised screen coordinates (x and y in
 * 0..1 of the presented frame). cellPad merges the touches into port 0; the
 * Vulkan backend draws the same layout over the frame, so what is drawn and
 * what is hit always agree.
 *
 *   left third, lower 2/3  floating left stick (anchors where the thumb lands)
 *   right half, upper part right stick (drag; camera)
 *   bottom-right           cross / circle / square / triangle
 *   bottom-left of centre  d-pad
 *   top corners            L1 L2 / R1 R2, L3 / R3 below
 *   top centre             SELECT, the settings menu, START
 *
 * RACACIT_TOUCH_SIZE=<percent> scales the buttons (60-160), and
 * RACACIT_TOUCH_OPACITY=<percent> fades the overlay (15-100).
 *
 * Enabled by default on Android, elsewhere with RACACIT_TOUCH=1; RACACIT_TOUCH=0
 * turns it off. It is also hidden while a real gamepad is connected.
 */
#ifndef PS3RECOMP_TOUCH_CONTROLS_H
#define PS3RECOMP_TOUCH_CONTROLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { TC_SHAPE_CROSS, TC_SHAPE_CIRCLE, TC_SHAPE_SQUARE, TC_SHAPE_TRIANGLE,
       TC_SHAPE_UP, TC_SHAPE_DOWN, TC_SHAPE_LEFT, TC_SHAPE_RIGHT,
       TC_SHAPE_PILL, TC_SHAPE_STICK };

typedef struct {
    float    x, y;      /* centre, normalised (x of width, y of height)  */
    float    r;         /* radius as a fraction of the frame HEIGHT      */
    int      shape;
    uint16_t mask;      /* CELL_PAD_CTRL_* bit (0 for the sticks)        */
} tc_button;

/* 1 when the overlay should be drawn and polled this frame. */
int  touch_controls_active(void);
/* The layout; returns the count. Same array every call. */
int  touch_controls_layout(const tc_button** out);
/* Read the current touches. Fills buttons (CELL_PAD_CTRL_* mask) and the
 * stick bytes (128 = centre) only for what touches are doing; returns 1 when
 * any finger is down. `aspect` = frame width / height. */
int  touch_controls_poll(float aspect, uint16_t* buttons,
                         uint8_t* lx, uint8_t* ly, uint8_t* rx, uint8_t* ry);
/* For the overlay: bitmask of layout indices currently pressed, and the left
 * stick (anchor + thumb, normalised; valid when *active). */
uint32_t touch_controls_pressed(void);
void touch_controls_stick(int* active, float* ax, float* ay, float* tx, float* ty);
/* The presenter: where the frame is on the screen (normalised x0,y0-x1,y1)
 * and its shape there (width / height in pixels). Touches are mapped into it. */
void touch_controls_set_frame(float x0, float y0, float x1, float y1, float aspect);
/* The overlay's opacity, 0.15-1 (RACACIT_TOUCH_OPACITY). */
float touch_controls_opacity(void);
/* Gamepad presence, reported by the pad backend (hides the overlay). */
void touch_controls_set_gamepad(int connected);

#ifdef __cplusplus
}
#endif
#endif
