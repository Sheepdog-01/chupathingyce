/*
TOUCH_OVERLAY.H

The on-screen touch controls of the desktop builds (touch_overlay.c): a
stick, buttons and a swipe to look, drawn over a game by code, as a
controller for player 1. They are what Android's are
(port/android/app/.../TouchControls.java), with the same layout, and they
reach the game the same way: touch_input.c's touch_input_controls and
touch_input_look read them as Android's host_touch_* functions are read.
On Android every function here does nothing (the app draws its own).

Main thread only, as the rest of touch_input.c.
*/

#ifndef TOUCH_OVERLAY_H
#define TOUCH_OVERLAY_H

#include <SDL3/SDL_events.h>

/* a finger of the touchscreen, in a game; nonzero if the controls took it
(they show), zero if it is the menus' (touch_menu.c) */
int touch_overlay_event(unsigned int type, const SDL_TouchFingerEvent *finger);

/* every finger let go (the window lost the focus) */
void touch_overlay_cancel(void);

/* where the game is, at every read of port 0: touch_input.c's
_touch_scene_* bits */
void touch_overlay_scene(int scene);

/* the controls' controller: the SDL axes (left x, y, right x, y, left
trigger, right trigger), then the SDL button bits, each press held for at
least a frame */
void touch_overlay_read(int *state);

/* the view swipe since the last read, in Android's logical pixels (a 540th
of the picture's height) with input.touch_sensitivity applied */
void touch_overlay_look_read(float *delta);

/* draws the controls when they show (ui_widget.c's render_ui_widgets, as
the code-drawn screens are drawn) */
void touch_overlay_render(void);

/* opens the layout's editor over the menus (Controls Setup's EDIT LAYOUT:
menu_functions.c); while it is open it takes every finger, and the menus
take no input (ui_widget.c) */
void touch_overlay_edit_begin(void);
int touch_overlay_editing(void);

#endif
