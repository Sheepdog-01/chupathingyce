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
They are drawn as labelled circles, the stick as a ring with a thumb pad
that follows the finger. input.touch_size and input.touch_opacity scale
them, input.touch_sensitivity the swipe (measured by its length on the
screen, so that a thumb turns the view as far on a tablet as on a phone),
and input.touch_layout moves, resizes or hides them (empty: the usual
layout, DESKTOP_LAYOUT's, or Android's without it).

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
/* (sdl_platform.c) */
SDL_Window *platform_sdl_window(void);

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

static const struct
{
	const char *label;
	/* the SDL gamepad button it presses, or -1 */
	int button;
	/* the SDL axis it pulls (4, 5: the triggers), or -1 */
	int trigger;
	/* Android's place on its 960x540 grid, and size in its logical pixels */
	float x, y, radius;
} kinds[NUMBER_OF_KINDS] =
{
	{ "A / Jump", 0, -1, 856, 447, 36 },
	{ "B / Melee", 1, -1, 913, 377, 32 },
	{ "X / Reload", 2, -1, 794, 377, 34 },
	{ "Y / Weapon", 3, -1, 850, 312, 32 },
	{ "Fire", -1, 5, 915, 239, 39 },
	{ "Grenade", -1, 4, 802, 239, 35 },
	{ "Crouch", 7, -1, 236, 449, 32 },
	{ "Zoom", 8, -1, 691, 449, 32 },
	{ "Light", 9, -1, 360, 490, 27 },
	{ "Gren. type", 10, -1, 438, 490, 29 },
	{ "Pause", 6, -1, 570, 36, 28 },
	{ "Back", 4, -1, 390, 36, 28 },
	{ "Up", 11, -1, 100, 237, 25 },
	{ "Down", 12, -1, 100, 335, 25 },
	{ "Left", 13, -1, 51, 286, 25 },
	{ "Right", 14, -1, 149, 286, 25 },
	{ "Move", -1, -1, 115, 440, 64 },
	{ "Fire", -1, 5, 255, 239, 39 },
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
pixels, the window's units per overlay unit, with the overlay's origin in
the window, and a phone's logical pixels per overlay unit by length on the
screen (the swipe's) */
struct view
{
	float left, width, scale;
	float window_x0, window_y0, window_scale_x, window_scale_y;
	float look_scale;
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
	/* the Hide button's: the controls put away, until Touch is pressed
	(TouchControls.java's visible, for this run) */
	BOOL hidden;
} overlay = { .layout_read_at = (unsigned long)-1, .fade = 1.0f };

static void release_all(void);

/* ---------- the layout */

/* the desktop's usual layout, as input.touch_layout writes one: laid out
on a 3:2 tablet (a Surface Pro), whose picture is taller than a phone's;
empty, Android's */
static const char DESKTOP_LAYOUT[] = "";

/* whether a layout's control is one Android's would take (TouchLayout.java):
the eighteen in their order, then copies of any but the stick, each on the
grid and between half and twice its size */
static BOOL control_valid(const struct control *control, int index)
{
	return control->kind >= 0 && control->kind < NUMBER_OF_KINDS &&
		(index < NUMBER_OF_KINDS ? control->kind == index : control->kind != _kind_stick) &&
		control->x >= 0.0f && control->x <= GRID_WIDTH && control->y >= 0.0f && control->y <= GRID_HEIGHT &&
		control->size >= 0.5f && control->size <= 2.0f;
}

/* a layout as input.touch_layout has it ("kind,x,y,size,shown" for each
control, between semicolons, x and y on the 960x540 grid) into controls;
how many, 0 for none or a mistake */
static int layout_parse(const char *text, struct control *controls)
{
	int count = 0;

	while (*text && count < MAXIMUM_CONTROLS)
	{
		struct control *control = &controls[count];
		int kind, shown, used = 0;

		if (sscanf(text, " %d , %f , %f , %f , %d %n", &kind, &control->x, &control->y, &control->size, &shown,
			&used) != 5)
		{
			return 0;
		}
		control->kind = kind;
		if (!control_valid(control, count))
			return 0;
		control->shown = shown != 0;
		count++;
		text += used;
		if (*text == ';')
			text++;
	}
	return *text || count < NUMBER_OF_KINDS ? 0 : count;
}

/* the usual layout into controls; how many */
static int layout_defaults_into(struct control *controls)
{
	int kind, count = layout_parse(DESKTOP_LAYOUT, controls);

	if (count)
		return count;
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

/* input.touch_layout; empty, or a mistake, gives the usual layout */
static void layout_read(void)
{
	struct control before[MAXIMUM_CONTROLS];
	int before_count = overlay.count;
	const char *text;
	int count;

	if (overlay.layout_read_at == config_changes())
		return;
	overlay.layout_read_at = config_changes();
	memcpy(before, overlay.controls, sizeof(before));
	text = config_string("input.touch_layout");
	count = layout_parse(text, overlay.controls);
	if (*text && !count)
		platform_log("touch: input.touch_layout not understood, using the usual layout");
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
	/* (Android's swipe is in its logical pixels, 540 to a phone's height,
	which held sideways is about 360 dp: 1.5 to a dp. A computer's dp is
	its display scale times 96 pixels to the inch, over 160) */
	{
		int pixels_width = 0, pixels_height = 0, window_width = 0, window_height = 0;
		float display_scale = platform_video_display_scale();
		float pixels_per_unit;

		platform_video_drawable_size(&pixels_width, &pixels_height);
		platform_video_window_size(&window_width, &window_height);
		pixels_per_unit = view->window_scale_y *
			(window_height > 0 && pixels_height > 0 ? (float)pixels_height / (float)window_height : 1.0f);
		if (display_scale <= 0.0f)
			display_scale = 1.0f;
		view->look_scale = pixels_per_unit / (display_scale * 96.0f / 160.0f) * (GRID_HEIGHT / 360.0f);
	}
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

/* the Hide (or Touch) button: at the top, in the middle, as Android's is,
by its size alone */
#define TOGGLE_Y 38.0f
#define TOGGLE_RADIUS 28.0f

static void toggle_place(const struct view *view, float *x, float *y, float *radius)
{
	*x = view->left + view->width * 0.5f;
	*y = TOGGLE_Y * LAYOUT_LINES / GRID_HEIGHT;
	*radius = TOGGLE_RADIUS * view->scale;
}

static void finger_down(const struct view *view, SDL_FingerID id, float x, float y)
{
	struct finger *finger;
	float toggle_x, toggle_y, toggle_radius;
	int control;

	if (overlay.finger_count == MAXIMUM_FINGERS || finger_find(id))
		return;
	/* (Hide puts the controls away, Touch brings them back; while they are
	away no finger plays) */
	toggle_place(view, &toggle_x, &toggle_y, &toggle_radius);
	if (inside(x, y, toggle_x, toggle_y, toggle_radius))
	{
		overlay.hidden = !overlay.hidden;
		release_all();
		return;
	}
	if (overlay.hidden)
		return;
	control = control_at(view, x, y);
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
			float sensitivity = setting_real("input.touch_sensitivity", 0.25f, 8.0f, 2.0f);

			overlay.look[0] += (x - finger->last_x) * view->look_scale * sensitivity;
			overlay.look[1] += (y - finger->last_y) * view->look_scale * sensitivity;
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
	_tool_copy,
	_tool_reset,
	_tool_cancel,
	_tool_save,

	NUMBER_OF_TOOLS
};

#define NO_TOOL (-1)
/* the panel, in the middle of the 640 columns, between the controls'
usual places; its buttons along its bottom */
#define PANEL_X 116.0f
#define PANEL_Y 150.0f
#define PANEL_WIDTH 408.0f
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

/* a layout as input.touch_layout has it into text (MAXIMUM_CONTROLS * 40
long), empty if it is the usual one; whether it is not */
#define LAYOUT_TEXT_SIZE (MAXIMUM_CONTROLS * 40)

static BOOL layout_text(const struct control *controls, int count, char *text)
{
	struct control usual[MAXIMUM_CONTROLS];
	int usual_count = layout_defaults_into(usual);
	size_t used = 0;
	BOOL changed = count != usual_count;
	int index;

	text[0] = 0;
	for (index = 0; index < count; index++)
	{
		const struct control *control = &controls[index];
		int written = snprintf(text + used, LAYOUT_TEXT_SIZE - used, "%d,%.1f,%.1f,%.2f,%d;", control->kind,
			control->x, control->y, control->size, control->shown ? 1 : 0);

		if (written < 0 || (size_t)written >= LAYOUT_TEXT_SIZE - used)
			break;
		used += (size_t)written;
		changed |= index >= usual_count || control->kind != usual[index].kind ||
			fabsf(control->x - usual[index].x) > 0.05f || fabsf(control->y - usual[index].y) > 0.05f ||
			fabsf(control->size - usual[index].size) > 0.001f || control->shown != usual[index].shown;
	}
	if (!changed)
		text[0] = 0;
	return changed;
}

/* the edited layout into input.touch_layout */
static void edit_save(void)
{
	char text[LAYOUT_TEXT_SIZE];

	layout_text(overlay.edit, overlay.edit_count, text);
	if (!config_write("input.touch_layout", text))
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
	case _tool_copy:
		/* (as TouchLayout.java's duplicate: beside it, on its right if
		there is room; never the stick) */
		if (chosen && chosen->kind != _kind_stick && overlay.edit_count < MAXIMUM_CONTROLS)
		{
			struct control *copy = &overlay.edit[overlay.edit_count];
			float radius = kinds[chosen->kind].radius * chosen->size;

			*copy = *chosen;
			copy->shown = TRUE;
			copy->x = chosen->x + radius * 2.0f + 12.0f;
			if (copy->x > GRID_WIDTH - radius)
				copy->x = chosen->x - radius * 2.0f - 12.0f;
			copy->x = fminf(GRID_WIDTH, fmaxf(0.0f, copy->x));
			overlay.chosen = overlay.edit_count++;
		}
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

/* ---------- aiming with the gyroscope (GyroscopeAim.java)

input.touch_gyro turns the view as the device turns, while the controls
show: the gyroscope's rates, turned into the screen's axes by how the
display is rotated from its natural way, in radians, 0.0022 to the look's
pixel as the swipe and the mouse have, times input.touch_gyro_sensitivity.
The sensor is open only while it is used.

Unlike Android's, which integrates each sample (and drops one that comes
more than a tenth of a second after the last), a rate here holds until the
next reading, and is integrated up to each one and at every read of the
look: Windows reports a gyroscope only when its reading changes, at the
driver's own rate (SDL sets none), so readings come seldom and unevenly, and
between them the rate is the last one. A reading older than a second is
taken to be stale, and the rate stops. */

#define LOOK_RADIANS_PER_PIXEL 0.0022f
/* (rates below this are the sensor's noise at rest, which would drift the
view) */
#define GYRO_NOISE 0.01f
#define GYRO_STALE_NS 1000000000ULL
/* (the readings a second, logged while it is used, every few seconds) */
#define GYRO_RATE_LOG_NS 5000000000ULL

static struct
{
	SDL_Sensor *sensor;
	BOOL tried, subsystem;
	int rotation;
	/* the held rates about the screen's axes (GyroscopeAim.java's screenX
	and screenY), until when they are integrated, and the last reading */
	float rate_x, rate_y;
	Uint64 integrated_until, last_reading;
	/* readings since the rate was last logged, and when */
	unsigned int readings;
	Uint64 counted_since;
} gyro = { .rotation = -1 };

/* quarter turns counterclockwise the display is from its natural way (0 to
3, as Android's Surface.ROTATION_*): portrait, landscape (its right side
up), portrait upside down and landscape flipped are a quarter turn apart */
static int display_rotation(void)
{
	SDL_Window *window = platform_sdl_window();
	SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : 0;
	SDL_DisplayOrientation current, natural;
	int turns[5] = { -1, 1, 3, 0, 2 };

	if (!display)
		return 0;
	current = SDL_GetCurrentDisplayOrientation(display);
	natural = SDL_GetNaturalDisplayOrientation(display);
	if (current == SDL_ORIENTATION_UNKNOWN || natural == SDL_ORIENTATION_UNKNOWN)
		return 0;
	return (turns[current] - turns[natural] + 4) % 4;
}

static void gyro_reset(void)
{
	gyro.rate_x = gyro.rate_y = 0.0f;
	gyro.integrated_until = gyro.last_reading = 0;
	gyro.rotation = -1;
}

/* the held rates' turn from where they were integrated to now (on the
clock of SDL_GetTicksNS, which the readings' timestamps are on) */
static void gyro_advance(Uint64 now)
{
	float seconds, sensitivity;

	if (!gyro.integrated_until || now <= gyro.integrated_until)
		return;
	if (now - gyro.last_reading > GYRO_STALE_NS)
		gyro.rate_x = gyro.rate_y = 0.0f;
	seconds = (float)(now - gyro.integrated_until) * 1e-9f;
	sensitivity = setting_real("input.touch_gyro_sensitivity", 0.25f, 8.0f, 2.0f);
	/* (GyroscopeAim.java's delta, and TouchControls.java's look from it) */
	overlay.look[0] += gyro.rate_y * seconds / LOOK_RADIANS_PER_PIXEL * sensitivity;
	overlay.look[1] += gyro.rate_x * seconds / LOOK_RADIANS_PER_PIXEL * sensitivity;
	gyro.integrated_until = now;
}

/* opens the gyroscope when it is wanted (once a time, and only if there is
one), closes it when not */
static void gyro_update(void)
{
	BOOL wanted = overlay.shown && !overlay.hidden && !overlay.editing && config_boolean("input.touch_gyro");

	if (!wanted)
	{
		if (gyro.sensor)
		{
			SDL_CloseSensor(gyro.sensor);
			gyro.sensor = NULL;
		}
		gyro.tried = FALSE;
		gyro_reset();
		return;
	}
	if (gyro.sensor || gyro.tried)
		return;
	gyro.tried = TRUE;
	if (!gyro.subsystem)
		gyro.subsystem = SDL_InitSubSystem(SDL_INIT_SENSOR);
	if (gyro.subsystem)
	{
		int count = 0, index;
		SDL_SensorID *sensors = SDL_GetSensors(&count);

		for (index = 0; sensors && index < count && !gyro.sensor; index++)
		{
			if (SDL_GetSensorTypeForID(sensors[index]) == SDL_SENSOR_GYRO)
				gyro.sensor = SDL_OpenSensor(sensors[index]);
		}
		SDL_free(sensors);
	}
	gyro.readings = 0;
	gyro.counted_since = SDL_GetTicksNS();
	platform_log(gyro.sensor ? "touch: gyroscope aim on (%s)" : "touch: gyroscope aim, but no gyroscope%s",
		gyro.sensor ? SDL_GetSensorName(gyro.sensor) : "");
}

void touch_overlay_sensor(const SDL_SensorEvent *event)
{
	float right, up;
	int rotation;

	if (!gyro.sensor || event->which != SDL_GetSensorID(gyro.sensor))
		return;
	if (!overlay.shown || overlay.hidden || overlay.editing)
	{
		gyro_reset();
		return;
	}
	gyro.readings++;
	if (event->timestamp - gyro.counted_since >= GYRO_RATE_LOG_NS)
	{
		platform_log("touch: gyroscope, %.1f readings a second",
			(double)gyro.readings * 1e9 / (double)(event->timestamp - gyro.counted_since));
		gyro.readings = 0;
		gyro.counted_since = event->timestamp;
	}
	/* (the turn so far, at the rates held until now) */
	gyro_advance(event->timestamp);
	rotation = display_rotation();
	if (rotation != gyro.rotation)
	{
		gyro_reset();
		gyro.rotation = rotation;
	}
	/* (the rates about the screen's right and up, from the device's x and
	y: SDL's axes are Android's, right and up with the device held its
	natural way) */
	switch (rotation)
	{
	default:
		right = event->data[0];
		up = event->data[1];
		break;
	case 1:
		right = -event->data[1];
		up = event->data[0];
		break;
	case 2:
		right = -event->data[0];
		up = -event->data[1];
		break;
	case 3:
		right = event->data[1];
		up = -event->data[0];
		break;
	}
	/* (GyroscopeAim.java's screenX and screenY; its table, kept for
	landscape, the 1 and 3 a phone has, inverts 0 and 2, which a tablet's
	natural landscape gives) */
	gyro.rate_x = fabsf(right) < GYRO_NOISE ? 0.0f : -right;
	gyro.rate_y = fabsf(up) < GYRO_NOISE ? 0.0f : -up;
	gyro.last_reading = event->timestamp;
	if (!gyro.integrated_until)
		gyro.integrated_until = event->timestamp;
}

/* ---------- a layout's file

Android's layout file (TouchLayout.java's exportConfiguration: a Java
properties file, "halo-touch-layout" version 2), so that a layout goes from
the phone to the computer and back: the controls on the same grid, the
look's sensitivity (by its length on a phone's screen, as the desktop's
is), and gyroscope aim. Controls Setup's EXPORT LAYOUT and IMPORT LAYOUT
pick the file with the system's own dialog; where none can be shown (the
Steam Deck's Game Mode: platform_run_is_quiet) it is touch_layout.txt
beside config.toml. */

#define LAYOUT_FILE_NAME "touch_layout.txt"
#define LAYOUT_FILE_LIMIT 65536
#define MESSAGE_MS 4000

enum
{
	_file_idle,
	_file_asking,
	_file_chosen,
	_file_cancelled,
};

static struct
{
	SDL_AtomicInt state;
	BOOL exporting;
	char path[1024];
	/* a line saying what came of it, drawn for a while */
	char message[96];
	Uint64 message_until;
} layout_file;

static void file_message(const char *message)
{
	snprintf(layout_file.message, sizeof(layout_file.message), "%s", message);
	layout_file.message_until = SDL_GetTicks() + MESSAGE_MS;
	platform_log("touch: %s", message);
}

static void layout_export(const char *path)
{
	char text[MAXIMUM_CONTROLS * 220];
	size_t used;
	int index;

	layout_read();
	used = (size_t)snprintf(text, sizeof(text),
		"#Halo touch layout\nformat=halo-touch-layout\nversion=2\nrumble=true\ngyroscope=%s\ncount=%d\n"
		"sensitivity=%g\n", config_boolean("input.touch_gyro") ? "true" : "false", overlay.count,
		setting_real("input.touch_sensitivity", 0.25f, 8.0f, 2.0f) > 4.0f ? 4.0 :
		(double)setting_real("input.touch_sensitivity", 0.25f, 8.0f, 2.0f));
	for (index = 0; index < overlay.count && used < sizeof(text); index++)
	{
		const struct control *control = &overlay.controls[index];

		used += (size_t)snprintf(text + used, sizeof(text) - used,
			"control.%d.type=%d\ncontrol.%d.x=%.1f\ncontrol.%d.y=%.1f\ncontrol.%d.visible=%s\ncontrol.%d.size=%.2f\n",
			index, control->kind, index, control->x, index, control->y, index, control->shown ? "true" : "false",
			index, control->size);
	}
	file_message(used < sizeof(text) && SDL_SaveFile(path, text, used) ? "Touch layout exported." :
		"The touch layout could not be exported.");
}

/* a property's value in a properties file's text, or NULL; at most size */
static const char *property(const char *text, const char *key, char *value, size_t size)
{
	size_t length = strlen(key);
	const char *line;

	for (line = text; *line; line += strcspn(line, "\n"), line += *line == '\n')
	{
		const char *at = line + strspn(line, " \t");
		size_t end;

		if (*at == '#' || *at == '!' || strncmp(at, key, length))
			continue;
		at += length;
		at += strspn(at, " \t");
		if (*at != '=' && *at != ':')
			continue;
		at++;
		at += strspn(at, " \t");
		end = strcspn(at, "\r\n");
		while (end && (at[end - 1] == ' ' || at[end - 1] == '\t'))
			end--;
		if (end >= size)
			return NULL;
		memcpy(value, at, end);
		value[end] = 0;
		return value;
	}
	return NULL;
}

static BOOL property_number(const char *text, const char *key, float *number)
{
	char value[64], *end;

	if (!property(text, key, value, sizeof(value)))
		return FALSE;
	*number = strtof(value, &end);
	return end != value && !*end && isfinite(*number);
}

/* "true" or "false" */
static BOOL property_boolean(const char *text, const char *key, BOOL *flag)
{
	char value[8];

	if (!property(text, key, value, sizeof(value)) || (strcmp(value, "true") && strcmp(value, "false")))
		return FALSE;
	*flag = !strcmp(value, "true");
	return TRUE;
}

/* as TouchLayout.java's importConfiguration takes one: versions 1 and 2,
18 to 64 controls, every one valid; then into config.toml */
static void layout_import(const char *path)
{
	struct control controls[MAXIMUM_CONTROLS];
	char layout[LAYOUT_TEXT_SIZE], value[32], number[32];
	size_t size = 0;
	char *text = SDL_LoadFile(path, &size);
	float count = 0.0f, sensitivity = 0.0f;
	BOOL gyroscope = FALSE, rumble, valid, version_2 = FALSE;
	int index;

	valid = text && size <= LAYOUT_FILE_LIMIT && property(text, "format", value, sizeof(value)) &&
		!strcmp(value, "halo-touch-layout") && property(text, "version", value, sizeof(value)) &&
		(!strcmp(value, "1") || !strcmp(value, "2")) && property_number(text, "count", &count) &&
		count >= NUMBER_OF_KINDS && count <= MAXIMUM_CONTROLS && count == floorf(count) &&
		property_number(text, "sensitivity", &sensitivity) && sensitivity >= 0.25f && sensitivity <= 4.0f;
	if (valid && !strcmp(value, "2"))
	{
		version_2 = TRUE;
		valid = property_boolean(text, "rumble", &rumble) && property_boolean(text, "gyroscope", &gyroscope);
	}
	for (index = 0; valid && index < (int)count; index++)
	{
		struct control *control = &controls[index];
		float kind = -1.0f;
		BOOL shown = TRUE;
		char key[40];

		snprintf(key, sizeof(key), "control.%d.type", index);
		valid = property_number(text, key, &kind) && kind == floorf(kind);
		control->kind = (int)kind;
		snprintf(key, sizeof(key), "control.%d.x", index);
		valid = valid && property_number(text, key, &control->x);
		snprintf(key, sizeof(key), "control.%d.y", index);
		valid = valid && property_number(text, key, &control->y);
		snprintf(key, sizeof(key), "control.%d.visible", index);
		valid = valid && property_boolean(text, key, &shown);
		control->shown = shown;
		control->size = 1.0f;
		snprintf(key, sizeof(key), "control.%d.size", index);
		valid = valid && (!version_2 || property_number(text, key, &control->size)) && control_valid(control, index);
	}
	SDL_free(text);
	if (!valid)
	{
		file_message("That file is not a touch layout.");
		return;
	}
	layout_text(controls, (int)count, layout);
	snprintf(number, sizeof(number), "%g", sensitivity);
	if (config_write("input.touch_layout", layout) && config_write("input.touch_sensitivity", number) &&
		(!version_2 || config_write_boolean("input.touch_gyro", gyroscope)))
	{
		file_message("Touch layout imported.");
	}
	else
	{
		file_message("The touch layout could not be saved.");
	}
}

/* (the system's dialog answers on a thread of its own on some systems) */
static void SDLCALL file_chosen(void *userdata, const char *const *files, int filter)
{
	(void)userdata;
	(void)filter;
	if (files && files[0])
	{
		snprintf(layout_file.path, sizeof(layout_file.path), "%s", files[0]);
		SDL_SetAtomicInt(&layout_file.state, _file_chosen);
	}
	else
	{
		SDL_SetAtomicInt(&layout_file.state, _file_cancelled);
	}
}

static void file_begin(BOOL exporting)
{
	static const SDL_DialogFileFilter filters[] =
	{
		{ "Touch layouts", "txt" },
		{ "All files", "*" },
	};
	char folder[1024];

	if (SDL_GetAtomicInt(&layout_file.state) != _file_idle)
		return;
	layout_file.exporting = exporting;
	config_folder(folder, sizeof(folder));
	snprintf(layout_file.path, sizeof(layout_file.path), "%s%s", folder, LAYOUT_FILE_NAME);
	if (platform_run_is_quiet())
	{
		SDL_SetAtomicInt(&layout_file.state, _file_chosen);
		return;
	}
	SDL_SetAtomicInt(&layout_file.state, _file_asking);
	if (exporting)
		SDL_ShowSaveFileDialog(file_chosen, NULL, platform_sdl_window(), filters, 2, layout_file.path);
	else
		SDL_ShowOpenFileDialog(file_chosen, NULL, platform_sdl_window(), filters, 2, folder, false);
}

/* (on the game's thread, at every read of the controller) */
static void file_update(void)
{
	int state = SDL_GetAtomicInt(&layout_file.state);

	if (state == _file_chosen)
	{
		if (layout_file.exporting)
			layout_export(layout_file.path);
		else
			layout_import(layout_file.path);
	}
	if (state == _file_chosen || state == _file_cancelled)
		SDL_SetAtomicInt(&layout_file.state, _file_idle);
}

void touch_overlay_layout_export(void)
{
	file_begin(TRUE);
}

void touch_overlay_layout_import(void)
{
	file_begin(FALSE);
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
	gyro_update();
	file_update();
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
	/* (the gyroscope's turn up to this read) */
	if (gyro.sensor)
		gyro_advance(SDL_GetTicksNS());
	delta[0] = overlay.look[0];
	delta[1] = overlay.look[1];
	overlay.look[0] = overlay.look[1] = 0.0f;
}

/* ---------- drawing */

#ifdef HALO_GAME_BROWSER

/* a color's alpha scaled by input.touch_opacity (and by fraction: the
labels are drawn at 1 / 0.6, so that they are solid at the default opacity
and fade only below it) */
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

/* a circle and its label, shrunk until it fits */
static void draw_labelled(float x, float y, float radius, const char *label, BOOL held)
{
	unsigned int fill_top = held ? 0xFFFFFF60 : 0x30343880, fill_bottom = held ? 0xC8CCD060 : 0x0C0E1080;
	float size = radius * 0.42f;
	float width = ui_overlay_text_width(UI_FONT_BOLD, size, label);

	circle_gradient(x, y, radius, faded(fill_top, 1.0f), faded(fill_bottom, 1.0f));
	ring(x, y, radius, radius * 0.06f, faded(held ? 0xFFFFFFF0 : 0xFFFFFFA0, 1.0f));
	if (width > radius * 1.7f)
		size *= radius * 1.7f / width;
	ui_overlay_text(UI_FONT_BOLD, size, x, y - size * 0.5f, UI_ALIGN_CENTER, faded(0xFFFFFFFF, 1.0f / 0.6f), label);
}

/* a button: its circle and its label (Android's) */
static void draw_button(int kind, float x, float y, float radius, BOOL held)
{
	draw_labelled(x, y, radius, kinds[kind].label, held);
}

/* the editor: the screen dimmed, the panel, and every control, the hidden
ones faint and the chosen one ringed */
static void edit_render(const struct view *view)
{
	static const char *const labels[NUMBER_OF_TOOLS] =
		{ "SMALLER", "BIGGER", "HIDE", "COPY", "RESET", "CANCEL", "SAVE" };
	int index;

	ui_overlay_layer(2);
	ui_overlay_rect(view->left, 0.0f, view->width, LAYOUT_LINES, 0.0f, 0x000000B0);
	ui_overlay_panel(PANEL_X, PANEL_Y, PANEL_WIDTH, PANEL_HEIGHT, 1);
	ui_overlay_text(UI_FONT_MENU, 16.0f, PANEL_X + PANEL_WIDTH * 0.5f, PANEL_Y + 10.0f, UI_ALIGN_CENTER, 0xFFFFFFFF,
		"TOUCH CONTROLS LAYOUT");
	ui_overlay_text(UI_FONT_REGULAR, 11.0f, PANEL_X + PANEL_WIDTH * 0.5f, PANEL_Y + 34.0f, UI_ALIGN_CENTER,
		0xC8D8F0FF, "Drag a control to move it. Tap one to size, hide or copy it.");
	ui_overlay_text(UI_FONT_REGULAR, 9.0f, PANEL_X + PANEL_WIDTH * 0.5f, PANEL_Y + 50.0f, UI_ALIGN_CENTER,
		0x9FB4D0FF, "The mouse works too. Escape, or B on a controller, cancels.");
	for (index = 0; index < NUMBER_OF_TOOLS; index++)
	{
		float x, y;
		/* (the chosen control's buttons do nothing with none chosen, and the
		stick has no copy) */
		BOOL usable = index > _tool_copy || (overlay.chosen != NO_CONTROL &&
			(index != _tool_copy || overlay.edit[overlay.chosen].kind != _kind_stick));
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
			draw_button(control->kind, x, y, radius, index == overlay.chosen);
		overlay.fade = 1.0f;
		if (index == overlay.chosen)
			ring(x, y, radius + 3.0f, 2.5f, 0xFFD040FF);
	}
	ui_overlay_layer(0);
}

/* the layout file's line, over whatever is up */
static void message_render(void)
{
	float width;

	if (!layout_file.message[0] || SDL_GetTicks() >= layout_file.message_until)
		return;
	width = ui_overlay_text_width(UI_FONT_BOLD, 12.0f, layout_file.message) + 24.0f;
	ui_overlay_layer(3);
	ui_overlay_rect(320.0f - width * 0.5f, 372.0f, width, 24.0f, 6.0f, 0x0A1A30E8);
	ui_overlay_outline(320.0f - width * 0.5f, 372.0f, width, 24.0f, 6.0f, 1.0f, 0x4F9DFFFF);
	ui_overlay_text(UI_FONT_BOLD, 12.0f, 320.0f, 378.0f, UI_ALIGN_CENTER, 0xFFFFFFFF, layout_file.message);
	ui_overlay_layer(0);
}

void touch_overlay_render(void)
{
	struct view view;
	int index;

	message_render();
	if (overlay.editing && view_get(&view))
	{
		edit_render(&view);
		return;
	}
	if (!overlay.shown || !view_get(&view))
		return;
	layout_read();
	ui_overlay_layer(0);
	{
		float x, y, radius;

		toggle_place(&view, &x, &y, &radius);
		draw_labelled(x, y, radius, overlay.hidden ? "Touch" : "Hide", FALSE);
	}
	if (overlay.hidden)
		return;
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
			draw_button(control->kind, x, y, radius, held);
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

void touch_overlay_sensor(const SDL_SensorEvent *event)
{
	(void)event;
}

void touch_overlay_layout_export(void)
{
}

void touch_overlay_layout_import(void)
{
}

#endif
