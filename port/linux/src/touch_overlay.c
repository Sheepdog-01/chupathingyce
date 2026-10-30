/*
TOUCH_OVERLAY.C

The on-screen touch controls of the desktop builds (touch_overlay.h).

The controls are Android's (port/android/app/.../TouchLayout.java and
TouchControls.java): the same eighteen, at the same places, the same sizes
and doing the same things, so a layout means the same on both. Their
places are kept as Android keeps them, on a 960x540 grid stretched over the
picture, and their sizes in Android's logical pixels; here they are drawn
in the overlay's 480 lines (ui_overlay.c), the picture's height, with the
margins of a wide picture beyond its 640 columns.

A finger that goes down on a control holds it until it is lifted; a finger
on a button that moves away also turns the view (fire and aim with one
thumb), as does any finger that goes down away from the controls. The
stick's finger moves the stick within its ring.

They show in a game (no menu or cinematic up), with input.touch_controls
"on", or "auto" (the default) while the touchscreen was the device used
last: a desktop with a touchscreen does not show them until it is touched,
and a key, the mouse or a controller hides them again (xinput_sdl.c).
input.touch_theme draws them with the controller's own button icons
("xbox") or as plain labelled circles ("simple"); input.touch_size,
input.touch_opacity and input.touch_sensitivity scale them, and
input.touch_layout moves, resizes or hides them (empty: Android's layout).

Controls Setup's EDIT LAYOUT (menu_functions.c) opens the layout's editor
over the menus (touch_overlay_edit_begin): every control drawn where it
is, the hidden ones faint, and a panel in the middle. A finger on a control
chooses it and drags it; the panel's buttons make the chosen one smaller
or bigger, hide or show it, put the usual layout back, or leave, saving
input.touch_layout or not. The editor takes the touchscreen, and the menus
take no input, until it is left.
*/

#include "touch_overlay.h"

#include <string.h>

#ifdef HALO_DESKTOP_TOUCH

#include "platform.h"
#include "sdl_platform.h"
#include "port_config.h"
#ifdef HALO_GAME_BROWSER
#include "ui_overlay.h"
#endif

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* (d3d8_gl.c) */
long halo_screen_width(void);
BOOL halo_ui_point_to_window(short x, short y, float *window_x, float *window_y);
/* (xinput_sdl.c) */
BOOL platform_input_touch_last(void);

/* touch_input.c's _touch_scene_* bits */
enum
{
	_scene_known = 1 << 0,
	_scene_menus = 1 << 1,
	_scene_on = 1 << 2,
	_scene_off = 1 << 3,
};

/* the controls' kinds, in TouchLayout.java's order (a layout names them by
it); the stick is _kind_stick */
enum
{
	_kind_a, _kind_b, _kind_x, _kind_y, _kind_fire, _kind_grenade, _kind_crouch, _kind_zoom,
	_kind_light, _kind_grenade_type, _kind_pause, _kind_back, _kind_up, _kind_down, _kind_left,
	_kind_right, _kind_stick, _kind_fire_left,

	NUMBER_OF_KINDS
};

#define NO_ICON (-1)

static const struct
{
	const char *label;
	/* the SDL gamepad button it presses, or -1 */
	int button;
	/* the SDL axis it pulls (4, 5: the triggers), or -1 */
	int trigger;
	/* the controller's icon the "xbox" theme draws it with (ui_overlay.h),
	or NO_ICON: its label */
	int icon;
	/* Android's place on its 960x540 grid, and size in its logical pixels */
	float x, y, radius;
} kinds[NUMBER_OF_KINDS] =
{
#ifdef HALO_GAME_BROWSER
#define ICON(name) name
#else
#define ICON(name) NO_ICON
#endif
	{ "A / Jump", 0, -1, ICON(UI_BUTTON_A), 856, 447, 36 },
	{ "B / Melee", 1, -1, ICON(UI_BUTTON_B), 913, 377, 32 },
	{ "X / Reload", 2, -1, ICON(UI_BUTTON_X), 794, 377, 34 },
	{ "Y / Weapon", 3, -1, ICON(UI_BUTTON_Y), 850, 312, 32 },
	{ "Fire", -1, 5, ICON(UI_BUTTON_RIGHT_TRIGGER), 915, 239, 39 },
	{ "Grenade", -1, 4, ICON(UI_BUTTON_LEFT_TRIGGER), 802, 239, 35 },
	{ "Crouch", 7, -1, NO_ICON, 236, 449, 32 },
	{ "Zoom", 8, -1, NO_ICON, 691, 449, 32 },
	{ "Light", 9, -1, ICON(UI_BUTTON_LEFT_SHOULDER), 360, 490, 27 },
	{ "Gren. type", 10, -1, ICON(UI_BUTTON_RIGHT_SHOULDER), 438, 490, 29 },
	{ "Pause", 6, -1, ICON(UI_BUTTON_START), 570, 36, 28 },
	{ "Back", 4, -1, ICON(UI_BUTTON_BACK), 390, 36, 28 },
	{ "Up", 11, -1, NO_ICON, 100, 237, 25 },
	{ "Down", 12, -1, NO_ICON, 100, 335, 25 },
	{ "Left", 13, -1, NO_ICON, 51, 286, 25 },
	{ "Right", 14, -1, NO_ICON, 149, 286, 25 },
	{ "Move", -1, -1, NO_ICON, 115, 440, 64 },
	{ "Fire", -1, 5, ICON(UI_BUTTON_RIGHT_TRIGGER), 255, 239, 39 },
#undef ICON
};

#define GRID_WIDTH 960.0f
#define GRID_HEIGHT 540.0f
#define LAYOUT_LINES 480.0f
#define MAXIMUM_CONTROLS 64
/* the stick's ring catches a finger a little beyond it, and the stick
rests within this much of its center */
#define STICK_REACH 1.28f
#define STICK_DEAD_ZONE 0.12f
/* a button's finger that moves this far (logical pixels) turns the view */
#define BUTTON_AIM_SLOP 10.0f
/* a press shorter than this still reaches the game, which reads the
controller once a frame (as port/android/host/host_touch.c holds it) */
#define TAP_HOLD_MS 60
#define NUMBER_OF_BUTTONS 15
#define NUMBER_OF_INPUTS (NUMBER_OF_BUTTONS + 2)

/* a control of the layout: its kind, its place on the grid, its size (a
scale of its kind's) and whether it shows */
struct control
{
	int kind;
	float x, y, size;
	BOOL shown;
};

/* a finger the controls hold: the control it went down on (or NO_CONTROL: it
turns the view), whether it turns the view, where it went down and where it
was last */
#define NO_CONTROL (-1)
#define MAXIMUM_FINGERS 10
struct finger
{
	SDL_FingerID id;
	int control;
	BOOL looks;
	float down_x, down_y, last_x, last_y;
};

/* the picture as the controls lay out over it: its left edge and width in
the overlay's units (480 to its height), the scale from Android's logical
pixels, and the window's units per overlay unit, with the overlay's origin
in the window */
struct view
{
	float left, width, scale;
	float window_x0, window_y0, window_scale_x, window_scale_y;
};

static struct
{
	struct control controls[MAXIMUM_CONTROLS];
	int count;
	unsigned long layout_read_at;
	BOOL layout_known;
	struct finger fingers[MAXIMUM_FINGERS];
	int finger_count;
	int scene;
	BOOL shown;
	/* the controller they make (touch_overlay_read) */
	int axes[6];
	unsigned int buttons;
	Uint64 pressed_ms[NUMBER_OF_INPUTS];
	float look[2];
	/* the layout being edited (touch_overlay_edit_begin): a copy of it, the
	control chosen, the finger dragging it and its offset from the control's
	center (on the grid), and the panel's button a finger is on */
	BOOL editing;
	struct control edit[MAXIMUM_CONTROLS];
	int edit_count;
	int chosen;
	BOOL dragging;
	SDL_FingerID drag_finger;
	float grab_x, grab_y;
	int tool;
	SDL_FingerID tool_finger;
	/* what is drawn next is this solid (the editor's hidden controls) */
	float fade;
} overlay = { .layout_read_at = (unsigned long)-1, .fade = 1.0f };

static void release_all(void);

/* ---------- the layout */

/* Android's layout into controls; how many */
static int layout_defaults_into(struct control *controls)
{
	int kind;

	for (kind = 0; kind < NUMBER_OF_KINDS; kind++)
	{
		controls[kind].kind = kind;
		controls[kind].x = kinds[kind].x;
		controls[kind].y = kinds[kind].y;
		controls[kind].size = 1.0f;
		controls[kind].shown = TRUE;
	}
	return NUMBER_OF_KINDS;
}

static void layout_defaults(void)
{
	overlay.count = layout_defaults_into(overlay.controls);
}

/* input.touch_layout: "kind,x,y,size,shown" for each control, between
semicolons, x and y on the 960x540 grid; empty, or a mistake, gives
Android's layout */
static void layout_read(void)
{
	struct control before[MAXIMUM_CONTROLS];
	int before_count = overlay.count;
	const char *text;
	int count = 0;

	if (overlay.layout_read_at == config_changes())
		return;
	overlay.layout_read_at = config_changes();
	memcpy(before, overlay.controls, sizeof(before));
	text = config_string("input.touch_layout");
	while (*text && count < MAXIMUM_CONTROLS)
	{
		struct control *control = &overlay.controls[count];
		int kind, shown, used = 0;

		if (sscanf(text, " %d , %f , %f , %f , %d %n", &kind, &control->x, &control->y, &control->size, &shown,
			&used) != 5 || kind < 0 || kind >= NUMBER_OF_KINDS || control->size < 0.5f || control->size > 2.0f)
		{
			if (*text)
				platform_log("touch: input.touch_layout not understood, using the default layout");
			count = 0;
			break;
		}
		control->kind = kind;
		control->shown = shown != 0;
		count++;
		text += used;
		if (*text == ';')
			text++;
	}
	if (count)
		overlay.count = count;
	else
		layout_defaults();
	/* (a finger holds a control by its place in the layout: a layout that
	changed under it lets everything go) */
	if (overlay.count != before_count || memcmp(before, overlay.controls, sizeof(before)))
		release_all();
}

/* ---------- where things are */

static float setting_real(const char *name, float minimum, float maximum, float fallback)
{
	double value = config_real(name);

	return value >= minimum && value <= maximum ? (float)value : fallback;
}

/* FALSE while the picture's place is not known (no window yet) */
static BOOL view_get(struct view *view)
{
	float x0, y0, x1, y1;
	long screen_width = halo_screen_width();

	if (screen_width <= 0 || !halo_ui_point_to_window(0, 0, &x0, &y0) ||
		!halo_ui_point_to_window(640, 480, &x1, &y1) || x1 == x0 || y1 == y0)
	{
		return FALSE;
	}
	view->width = (float)screen_width;
	view->left = -(view->width - 640.0f) * 0.5f;
	/* (Android's logical pixel: its view's scale, min(width / 960, height
	/ 540) of its pixels, in the overlay's) */
	view->scale = fminf(view->width / GRID_WIDTH, LAYOUT_LINES / GRID_HEIGHT);
	view->window_x0 = x0;
	view->window_y0 = y0;
	view->window_scale_x = (x1 - x0) / 640.0f;
	view->window_scale_y = (y1 - y0) / 480.0f;
	return TRUE;
}

static void control_place(const struct view *view, const struct control *control, float *x, float *y,
	float *radius)
{
	*x = view->left + control->x * view->width / GRID_WIDTH;
	*y = control->y * LAYOUT_LINES / GRID_HEIGHT;
	*radius = kinds[control->kind].radius * control->size * view->scale *
		setting_real("input.touch_size", 0.5f, 2.0f, 1.0f);
}

/* a finger's place in the overlay's units */
static void finger_place(const struct view *view, const SDL_TouchFingerEvent *finger, float *x, float *y)
{
	int width = 0, height = 0;

	platform_video_window_size(&width, &height);
	*x = (finger->x * (float)width - view->window_x0) / view->window_scale_x;
	*y = (finger->y * (float)height - view->window_y0) / view->window_scale_y;
}

static BOOL inside(float x, float y, float center_x, float center_y, float radius)
{
	return (x - center_x) * (x - center_x) + (y - center_y) * (y - center_y) <= radius * radius;
}

/* ---------- the controller they make */

static int input_bits(const int *axes, unsigned int buttons)
{
	return (int)(buttons & ((1u << NUMBER_OF_BUTTONS) - 1)) | (axes[4] ? 1 << NUMBER_OF_BUTTONS : 0) |
		(axes[5] ? 1 << (NUMBER_OF_BUTTONS + 1) : 0);
}

/* the buttons and triggers the fingers hold; a new press is timed, so that
a tap reaches the game however short */
static void publish(void)
{
	int before = input_bits(overlay.axes, overlay.buttons), after, index;
	Uint64 now = SDL_GetTicks();

	overlay.buttons = 0;
	overlay.axes[4] = overlay.axes[5] = 0;
	for (index = 0; index < overlay.finger_count; index++)
	{
		int control = overlay.fingers[index].control;
		int kind;

		if (control < 0)
			continue;
		kind = overlay.controls[control].kind;
		if (kinds[kind].button >= 0)
			overlay.buttons |= 1u << kinds[kind].button;
		if (kinds[kind].trigger >= 0)
			overlay.axes[kinds[kind].trigger] = 32767;
	}
	after = input_bits(overlay.axes, overlay.buttons);
	for (index = 0; index < NUMBER_OF_INPUTS; index++)
	{
		if ((after & ~before) & (1 << index))
			overlay.pressed_ms[index] = now;
	}
}

static void stick_move(const struct view *view, int control, float x, float y)
{
	float center_x, center_y, radius, dx, dy, length;

	control_place(view, &overlay.controls[control], &center_x, &center_y, &radius);
	dx = (x - center_x) / radius;
	dy = (y - center_y) / radius;
	length = sqrtf(dx * dx + dy * dy);
	if (length < STICK_DEAD_ZONE)
		dx = dy = 0.0f;
	else if (length > 1.0f)
	{
		dx /= length;
		dy /= length;
	}
	overlay.axes[0] = (int)lroundf(dx * 32767.0f);
	overlay.axes[1] = (int)lroundf(dy * 32767.0f);
}

/* every finger let go: the stick back to its center, nothing held */
static void release_all(void)
{
	overlay.finger_count = 0;
	memset(overlay.axes, 0, sizeof(overlay.axes));
	overlay.look[0] = overlay.look[1] = 0.0f;
	publish();
}

/* ---------- the fingers */

/* the control a finger at x, y goes down on, or NO_CONTROL; the stick only if no
other finger holds it */
static int control_at(const struct view *view, float x, float y)
{
	int index, stick = NO_CONTROL;

	for (index = 0; index < overlay.count; index++)
	{
		float center_x, center_y, radius;

		if (!overlay.controls[index].shown)
			continue;
		control_place(view, &overlay.controls[index], &center_x, &center_y, &radius);
		if (overlay.controls[index].kind == _kind_stick)
		{
			if (stick == NO_CONTROL && inside(x, y, center_x, center_y, radius * STICK_REACH))
				stick = index;
			continue;
		}
		if (inside(x, y, center_x, center_y, radius))
			return index;
	}
	if (stick != NO_CONTROL)
	{
		for (index = 0; index < overlay.finger_count; index++)
		{
			if (overlay.fingers[index].control == stick)
				return NO_CONTROL;
		}
	}
	return stick;
}

static BOOL looking(void)
{
	int index;

	for (index = 0; index < overlay.finger_count; index++)
	{
		if (overlay.fingers[index].looks)
			return TRUE;
	}
	return FALSE;
}

static struct finger *finger_find(SDL_FingerID id)
{
	int index;

	for (index = 0; index < overlay.finger_count; index++)
	{
		if (overlay.fingers[index].id == id)
			return &overlay.fingers[index];
	}
	return NULL;
}

static void finger_down(const struct view *view, SDL_FingerID id, float x, float y)
{
	struct finger *finger;
	int control = control_at(view, x, y);

	if (overlay.finger_count == MAXIMUM_FINGERS || finger_find(id))
		return;
	/* (away from the controls, one finger turns the view; another does
	nothing) */
	if (control == NO_CONTROL && looking())
		return;
	finger = &overlay.fingers[overlay.finger_count++];
	finger->id = id;
	finger->control = control;
	finger->looks = control == NO_CONTROL;
	finger->down_x = finger->last_x = x;
	finger->down_y = finger->last_y = y;
	if (control != NO_CONTROL && overlay.controls[control].kind == _kind_stick)
		stick_move(view, control, x, y);
	publish();
}

static void finger_move(const struct view *view, SDL_FingerID id, float x, float y)
{
	struct finger *finger = finger_find(id);

	if (!finger)
		return;
	if (finger->control != NO_CONTROL && overlay.controls[finger->control].kind == _kind_stick)
	{
		stick_move(view, finger->control, x, y);
	}
	else
	{
		/* a button's finger that moves away turns the view too, if none
		does yet */
		if (!finger->looks && !looking() &&
			hypotf(x - finger->down_x, y - finger->down_y) > BUTTON_AIM_SLOP * view->scale)
		{
			finger->looks = TRUE;
			finger->last_x = finger->down_x;
			finger->last_y = finger->down_y;
		}
		if (finger->looks)
		{
			float sensitivity = setting_real("input.touch_sensitivity", 0.25f, 4.0f, 1.0f);

			overlay.look[0] += (x - finger->last_x) / view->scale * sensitivity;
			overlay.look[1] += (y - finger->last_y) / view->scale * sensitivity;
		}
	}
	finger->last_x = x;
	finger->last_y = y;
}

static void finger_up(SDL_FingerID id)
{
	struct finger *finger = finger_find(id);

	if (!finger)
		return;
	if (finger->control != NO_CONTROL && overlay.controls[finger->control].kind == _kind_stick)
		overlay.axes[0] = overlay.axes[1] = 0;
	*finger = overlay.fingers[--overlay.finger_count];
	publish();
}

/* whether the controls show now: in a game, and by input.touch_controls
(the scene's bits) and the device used last */
static BOOL shown_now(void)
{
	if (!(overlay.scene & _scene_known) || (overlay.scene & (_scene_menus | _scene_off)))
		return FALSE;
#ifdef HALO_GAME_BROWSER
	if (!ui_overlay_available())
		return FALSE;
#else
	return FALSE;
#endif
	return (overlay.scene & _scene_on) || platform_input_touch_last();
}

/* ---------- the layout's editor */

enum
{
	_tool_smaller,
	_tool_bigger,
	_tool_hide,
	_tool_reset,
	_tool_cancel,
	_tool_save,

	NUMBER_OF_TOOLS
};

#define NO_TOOL (-1)
/* the panel, in the middle of the 640 columns, between the controls'
usual places; its buttons along its bottom */
#define PANEL_X 150.0f
#define PANEL_Y 150.0f
#define PANEL_WIDTH 340.0f
#define PANEL_HEIGHT 100.0f
#define TOOL_WIDTH 52.0f
#define TOOL_HEIGHT 24.0f
#define TOOL_GAP 4.0f
#define SIZE_STEP 0.1f

static void tool_rect(int tool, float *x, float *y)
{
	float row = NUMBER_OF_TOOLS * TOOL_WIDTH + (NUMBER_OF_TOOLS - 1) * TOOL_GAP;

	*x = PANEL_X + (PANEL_WIDTH - row) * 0.5f + (float)tool * (TOOL_WIDTH + TOOL_GAP);
	*y = PANEL_Y + PANEL_HEIGHT - TOOL_HEIGHT - 10.0f;
}

static int tool_at(float x, float y)
{
	int tool;

	for (tool = 0; tool < NUMBER_OF_TOOLS; tool++)
	{
		float left, top;

		tool_rect(tool, &left, &top);
		if (x >= left && x < left + TOOL_WIDTH && y >= top && y < top + TOOL_HEIGHT)
			return tool;
	}
	return NO_TOOL;
}

/* the edited control under x, y, the hidden ones too (the last drawn, on
top, first), or NO_CONTROL */
static int edit_control_at(const struct view *view, float x, float y)
{
	int index;

	for (index = overlay.edit_count - 1; index >= 0; index--)
	{
		float center_x, center_y, radius;

		control_place(view, &overlay.edit[index], &center_x, &center_y, &radius);
		if (inside(x, y, center_x, center_y, radius))
			return index;
	}
	return NO_CONTROL;
}

static void grid_place(const struct view *view, float x, float y, float *grid_x, float *grid_y)
{
	*grid_x = (x - view->left) * GRID_WIDTH / view->width;
	*grid_y = y * GRID_HEIGHT / LAYOUT_LINES;
}

/* the edited layout as input.touch_layout has it; empty if it is the usual
one */
static void edit_save(void)
{
	char text[MAXIMUM_CONTROLS * 40];
	struct control usual[NUMBER_OF_KINDS];
	size_t used = 0;
	BOOL changed = overlay.edit_count != layout_defaults_into(usual);
	int index;

	text[0] = 0;
	for (index = 0; index < overlay.edit_count; index++)
	{
		const struct control *control = &overlay.edit[index];
		int written = snprintf(text + used, sizeof(text) - used, "%d,%.1f,%.1f,%.2f,%d;", control->kind,
			control->x, control->y, control->size, control->shown ? 1 : 0);

		if (written < 0 || (size_t)written >= sizeof(text) - used)
			break;
		used += (size_t)written;
		changed |= index >= NUMBER_OF_KINDS || control->kind != usual[index].kind ||
			fabsf(control->x - usual[index].x) > 0.05f || fabsf(control->y - usual[index].y) > 0.05f ||
			fabsf(control->size - 1.0f) > 0.001f || !control->shown;
	}
	if (!config_write("input.touch_layout", changed ? text : ""))
		platform_log("touch: could not save input.touch_layout");
}

static void edit_tool(int tool)
{
	struct control *chosen = overlay.chosen != NO_CONTROL ? &overlay.edit[overlay.chosen] : NULL;

	switch (tool)
	{
	case _tool_smaller:
	case _tool_bigger:
		if (chosen)
		{
			float size = chosen->size + (tool == _tool_bigger ? SIZE_STEP : -SIZE_STEP);

			chosen->size = fminf(2.0f, fmaxf(0.5f, roundf(size * 20.0f) / 20.0f));
		}
		break;
	case _tool_hide:
		if (chosen)
			chosen->shown = !chosen->shown;
		break;
	case _tool_reset:
		overlay.edit_count = layout_defaults_into(overlay.edit);
		overlay.chosen = NO_CONTROL;
		break;
	case _tool_save:
		edit_save();
		overlay.editing = FALSE;
		break;
	case _tool_cancel:
		overlay.editing = FALSE;
		break;
	}
}

/* a finger (or the mouse) at x, y in the overlay's units: a down on one of
the panel's buttons presses it, one on a control chooses it and drags it
(one at a time); a button acts when it is let go of on it */
static void edit_pointer(const struct view *view, unsigned int type, SDL_FingerID id, float x, float y)
{
	switch (type)
	{
	case SDL_EVENT_FINGER_DOWN:
		if (!overlay.dragging && overlay.tool == NO_TOOL)
		{
			int control;

			/* (the panel's buttons first: a control left over them must not
			keep SAVE or CANCEL from being pressed) */
			if ((overlay.tool = tool_at(x, y)) != NO_TOOL)
			{
				overlay.tool_finger = id;
			}
			else if ((control = edit_control_at(view, x, y)) != NO_CONTROL)
			{
				float grid_x, grid_y;

				grid_place(view, x, y, &grid_x, &grid_y);
				overlay.chosen = control;
				overlay.dragging = TRUE;
				overlay.drag_finger = id;
				overlay.grab_x = overlay.edit[control].x - grid_x;
				overlay.grab_y = overlay.edit[control].y - grid_y;
			}
		}
		break;
	case SDL_EVENT_FINGER_MOTION:
		if (overlay.dragging && id == overlay.drag_finger)
		{
			struct control *control = &overlay.edit[overlay.chosen];
			float grid_x, grid_y;

			grid_place(view, x, y, &grid_x, &grid_y);
			control->x = fminf(GRID_WIDTH, fmaxf(0.0f, grid_x + overlay.grab_x));
			control->y = fminf(GRID_HEIGHT, fmaxf(0.0f, grid_y + overlay.grab_y));
		}
		break;
	case SDL_EVENT_FINGER_UP:
		if (overlay.dragging && id == overlay.drag_finger)
		{
			overlay.dragging = FALSE;
		}
		else if (overlay.tool != NO_TOOL && id == overlay.tool_finger)
		{
			if (tool_at(x, y) == overlay.tool)
				edit_tool(overlay.tool);
			overlay.tool = NO_TOOL;
		}
		break;
	}
}

static int edit_event(unsigned int type, const SDL_TouchFingerEvent *finger)
{
	struct view view;
	float x, y;

	if (type == SDL_EVENT_FINGER_CANCELED)
	{
		overlay.dragging = FALSE;
		overlay.tool = NO_TOOL;
		return 1;
	}
	if (!view_get(&view))
		return 1;
	finger_place(&view, finger, &x, &y);
	edit_pointer(&view, type, finger->fingerID, x, y);
	return 1;
}

/* (the mouse, as a finger of its own) */
#define MOUSE_FINGER ((SDL_FingerID)-1)

void touch_overlay_edit_mouse(unsigned int type, float window_x, float window_y)
{
	struct view view;

	if (!overlay.editing || !view_get(&view))
		return;
	edit_pointer(&view, type, MOUSE_FINGER, (window_x - view.window_x0) / view.window_scale_x,
		(window_y - view.window_y0) / view.window_scale_y);
}

void touch_overlay_edit_cancel(void)
{
	if (overlay.editing)
		edit_tool(_tool_cancel);
}

int touch_overlay_touched_last(void)
{
	return platform_input_touch_last();
}

void touch_overlay_edit_begin(void)
{
	layout_read();
	memcpy(overlay.edit, overlay.controls, sizeof(overlay.controls));
	overlay.edit_count = overlay.count;
	overlay.chosen = NO_CONTROL;
	overlay.dragging = FALSE;
	overlay.tool = NO_TOOL;
	overlay.editing = TRUE;
	release_all();
	platform_log("touch: editing the layout");
}

int touch_overlay_editing(void)
{
	return overlay.editing;
}

int touch_overlay_event(unsigned int type, const SDL_TouchFingerEvent *finger)
{
	struct view view;
	float x, y;

	if (overlay.editing)
		return edit_event(type, finger);
	if (type == SDL_EVENT_FINGER_CANCELED)
	{
		release_all();
		return overlay.shown;
	}
	/* (a finger that went down while the menus had the screen stays theirs:
	only a down decides, and a finger the controls do not hold is not
	theirs) */
	if (type == SDL_EVENT_FINGER_DOWN)
	{
		overlay.shown = shown_now();
		if (!overlay.shown)
			return 0;
	}
	else if (!finger_find(finger->fingerID))
	{
		return 0;
	}
	if (!view_get(&view))
		return overlay.shown;
	layout_read();
	finger_place(&view, finger, &x, &y);
	switch (type)
	{
	case SDL_EVENT_FINGER_DOWN:
		finger_down(&view, finger->fingerID, x, y);
		break;
	case SDL_EVENT_FINGER_MOTION:
		finger_move(&view, finger->fingerID, x, y);
		break;
	case SDL_EVENT_FINGER_UP:
		finger_up(finger->fingerID);
		break;
	}
	return 1;
}

void touch_overlay_cancel(void)
{
	release_all();
}

void touch_overlay_scene(int scene)
{
	BOOL shown;

	overlay.scene = scene;
	shown = shown_now();
	/* (hidden: what the fingers held is let go, and a menu or cinematic
	that comes up takes them) */
	if (overlay.shown && !shown)
		release_all();
	overlay.shown = shown;
}

void touch_overlay_read(int *state)
{
	Uint64 now = SDL_GetTicks();
	int index;

	memcpy(state, overlay.axes, sizeof(overlay.axes));
	state[6] = (int)overlay.buttons;
	for (index = 0; index < NUMBER_OF_INPUTS; index++)
	{
		if (!overlay.pressed_ms[index] || now - overlay.pressed_ms[index] >= TAP_HOLD_MS)
			continue;
		if (index < NUMBER_OF_BUTTONS)
			state[6] |= 1 << index;
		else if (!state[4 + index - NUMBER_OF_BUTTONS])
			state[4 + index - NUMBER_OF_BUTTONS] = 32767;
	}
}

void touch_overlay_look_read(float *delta)
{
	delta[0] = overlay.look[0];
	delta[1] = overlay.look[1];
	overlay.look[0] = overlay.look[1] = 0.0f;
}

/* ---------- drawing */

#ifdef HALO_GAME_BROWSER

/* a color's alpha scaled by input.touch_opacity (and by fraction: the icons
and labels are drawn at 1 / 0.6, so that they are solid at the default
opacity and fade only below it) */
static unsigned int faded(unsigned int color, float fraction)
{
	/* (the editor shows the controls at the default opacity, so that they
	can be seen to be placed) */
	float opacity = overlay.editing ? 0.6f : setting_real("input.touch_opacity", 0.1f, 1.0f, 0.6f);
	unsigned int alpha = (unsigned int)lroundf((float)(color & 0xFF) * opacity * fraction * overlay.fade);

	return (color & 0xFFFFFF00u) | (alpha > 255 ? 255 : alpha);
}

static void circle(float x, float y, float radius, unsigned int color)
{
	ui_overlay_rect(x - radius, y - radius, radius * 2.0f, radius * 2.0f, radius, color);
}

static void circle_gradient(float x, float y, float radius, unsigned int top, unsigned int bottom)
{
	ui_overlay_gradient(x - radius, y - radius, radius * 2.0f, radius * 2.0f, radius, top, bottom);
}

static void ring(float x, float y, float radius, float thickness, unsigned int color)
{
	ui_overlay_outline(x - radius, y - radius, radius * 2.0f, radius * 2.0f, radius, thickness, color);
}

/* the stick: its ring, and the island the finger moves, lit from above */
static void draw_stick(float x, float y, float radius)
{
	float island = radius * 0.45f;
	float island_x = x + (float)overlay.axes[0] / 32767.0f * (radius - island * 0.5f);
	float island_y = y + (float)overlay.axes[1] / 32767.0f * (radius - island * 0.5f);

	circle_gradient(x, y, radius, faded(0x2A2E33A0, 1.0f), faded(0x0E1013A0, 1.0f));
	ring(x, y, radius, radius * 0.04f, faded(0xFFFFFF90, 1.0f));
	ring(x, y, radius * 0.62f, radius * 0.02f, faded(0xFFFFFF30, 1.0f));
	circle_gradient(island_x, island_y, island, faded(0xECEFF2E0, 1.0f), faded(0x8E959CE0, 1.0f));
	/* (the island's grip: a few faint rings) */
	ring(island_x, island_y, island * 0.72f, island * 0.06f, faded(0x00000030, 1.0f));
	ring(island_x, island_y, island * 0.44f, island * 0.06f, faded(0x00000024, 1.0f));
	ring(island_x, island_y, island, island * 0.06f, faded(0xFFFFFF60, 1.0f));
}

static void draw_button(int kind, float x, float y, float radius, BOOL held, BOOL icons)
{
	unsigned int fill_top = held ? 0xFFFFFF60 : 0x30343880, fill_bottom = held ? 0xC8CCD060 : 0x0C0E1080;

	circle_gradient(x, y, radius, faded(fill_top, 1.0f), faded(fill_bottom, 1.0f));
	ring(x, y, radius, radius * 0.06f, faded(held ? 0xFFFFFFF0 : 0xFFFFFFA0, 1.0f));
	if (icons && kinds[kind].icon != NO_ICON)
	{
		float size = radius * 1.25f;
		float width = ui_overlay_button_width(kinds[kind].icon, size);

		ui_overlay_button(kinds[kind].icon, size, x - width * 0.5f, y - size * 0.5f, faded(0xFFFFFFFF, 1.0f / 0.6f));
	}
	else
	{
		/* (a label in two lines' room: shrunk until it fits the circle) */
		float size = radius * 0.42f;
		float width = ui_overlay_text_width(UI_FONT_BOLD, size, kinds[kind].label);

		if (width > radius * 1.7f)
			size *= radius * 1.7f / width;
		ui_overlay_text(UI_FONT_BOLD, size, x, y - size * 0.5f, UI_ALIGN_CENTER, faded(0xFFFFFFFF, 1.0f / 0.6f),
			kinds[kind].label);
	}
}

/* the editor: the screen dimmed, the panel, and every control, the hidden
ones faint and the chosen one ringed */
static void edit_render(const struct view *view, BOOL icons)
{
	static const char *const labels[NUMBER_OF_TOOLS] = { "SMALLER", "BIGGER", "HIDE", "RESET", "CANCEL", "SAVE" };
	int index;

	ui_overlay_layer(2);
	ui_overlay_rect(view->left, 0.0f, view->width, LAYOUT_LINES, 0.0f, 0x000000B0);
	ui_overlay_panel(PANEL_X, PANEL_Y, PANEL_WIDTH, PANEL_HEIGHT, 1);
	ui_overlay_text(UI_FONT_MENU, 16.0f, PANEL_X + PANEL_WIDTH * 0.5f, PANEL_Y + 10.0f, UI_ALIGN_CENTER, 0xFFFFFFFF,
		"TOUCH CONTROLS LAYOUT");
	ui_overlay_text(UI_FONT_REGULAR, 11.0f, PANEL_X + PANEL_WIDTH * 0.5f, PANEL_Y + 34.0f, UI_ALIGN_CENTER,
		0xC8D8F0FF, "Drag a control to move it. Tap one to size or hide it.");
	ui_overlay_text(UI_FONT_REGULAR, 9.0f, PANEL_X + PANEL_WIDTH * 0.5f, PANEL_Y + 50.0f, UI_ALIGN_CENTER,
		0x9FB4D0FF, "The mouse works too. Escape, or B on a controller, cancels.");
	for (index = 0; index < NUMBER_OF_TOOLS; index++)
	{
		float x, y;
		/* (the chosen control's buttons do nothing with none chosen) */
		BOOL usable = index > _tool_hide || overlay.chosen != NO_CONTROL;
		const char *label = labels[index];

		if (index == _tool_hide && overlay.chosen != NO_CONTROL && !overlay.edit[overlay.chosen].shown)
			label = "SHOW";
		tool_rect(index, &x, &y);
		ui_overlay_rect(x, y, TOOL_WIDTH, TOOL_HEIGHT, 4.0f, overlay.tool == index ? 0x3A6AA0F0 : 0x14243CE0);
		ui_overlay_outline(x, y, TOOL_WIDTH, TOOL_HEIGHT, 4.0f, 1.0f, usable ? 0x7FB2FFC0 : 0x7FB2FF40);
		ui_overlay_text(UI_FONT_BOLD, 10.0f, x + TOOL_WIDTH * 0.5f, y + 7.0f, UI_ALIGN_CENTER,
			usable ? 0xFFFFFFFF : 0xFFFFFF50, label);
	}
	ui_overlay_layer(3);
	for (index = 0; index < overlay.edit_count; index++)
	{
		const struct control *control = &overlay.edit[index];
		float x, y, radius;

		control_place(view, control, &x, &y, &radius);
		overlay.fade = control->shown ? 1.0f : 0.35f;
		if (control->kind == _kind_stick)
			draw_stick(x, y, radius);
		else
			draw_button(control->kind, x, y, radius, index == overlay.chosen, icons);
		overlay.fade = 1.0f;
		if (index == overlay.chosen)
			ring(x, y, radius + 3.0f, 2.5f, 0xFFD040FF);
	}
	ui_overlay_layer(0);
}

void touch_overlay_render(void)
{
	struct view view;
	BOOL icons;
	int index;

	if (overlay.editing && view_get(&view))
	{
		edit_render(&view, strcmp(config_string("input.touch_theme"), "simple") != 0);
		return;
	}
	if (!overlay.shown || !view_get(&view))
		return;
	layout_read();
	icons = strcmp(config_string("input.touch_theme"), "simple") != 0;
	ui_overlay_layer(0);
	for (index = 0; index < overlay.count; index++)
	{
		const struct control *control = &overlay.controls[index];
		float x, y, radius;
		BOOL held = FALSE;
		int finger;

		if (!control->shown)
			continue;
		control_place(&view, control, &x, &y, &radius);
		for (finger = 0; finger < overlay.finger_count; finger++)
			held |= overlay.fingers[finger].control == index;
		if (control->kind == _kind_stick)
			draw_stick(x, y, radius);
		else
			draw_button(control->kind, x, y, radius, held, icons);
	}
}

#else

void touch_overlay_render(void)
{
}

#endif

#else

/* (Android, whose app draws its own controls, TouchControls.java, and the
consoles) */
int touch_overlay_event(unsigned int type, const SDL_TouchFingerEvent *finger)
{
	(void)type;
	(void)finger;
	return 0;
}

void touch_overlay_cancel(void)
{
}

void touch_overlay_scene(int scene)
{
	(void)scene;
}

void touch_overlay_read(int *state)
{
	memset(state, 0, sizeof(int) * 7);
}

void touch_overlay_look_read(float *delta)
{
	delta[0] = delta[1] = 0.0f;
}

void touch_overlay_render(void)
{
}

void touch_overlay_edit_begin(void)
{
}

int touch_overlay_editing(void)
{
	return 0;
}

void touch_overlay_edit_cancel(void)
{
}

void touch_overlay_edit_mouse(unsigned int type, float window_x, float window_y)
{
	(void)type;
	(void)window_x;
	(void)window_y;
}

int touch_overlay_touched_last(void)
{
	return 0;
}

#endif
