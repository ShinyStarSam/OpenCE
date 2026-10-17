/*
KEVIN_VIEW.C

Kevin's view (kevin.enabled): what his eyes see, for his AI to read
(kevin/BRIDGE_PLAN.md's M10). You see only your own view. His is drawn only
when a frame of it is asked for (kevin_view_request): in the next frame,
from his unit's eyes along his aim, into the top left quarter of the
screen's target before yours, which then covers it, and read back from
there without waiting for the GPU (halo_screen_read_begin); it arrives a
frame or more later.

It has no HUD (he is no player) and not his own body, as your view hasn't
yours. debug.kevin_view_every asks for one every so many seconds, and
debug.kevin_view_directory saves each that arrives as a BMP file.
*/

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "camera/observer.h"
#include "main/main.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "units/units.h"

#include "kevin_bot.h"
#include "kevin_view.h"

#include <stdio.h>
#include <string.h>

/* ---------- constants */

/* views drawn between the lines of how long they take, in debug.txt */
#define KEVIN_VIEW_LOG_COUNT 10

/* his field of view (horizontal, radians) when yours is not known */
#define KEVIN_VIEW_FIELD_OF_VIEW 1.2217305f /* 70 degrees */

/* ---------- prototypes */

unsigned long system_milliseconds(void);

/* ---------- globals */

static struct
{
	boolean settings_read;
	long every_milliseconds;     /* debug.kevin_view_every's, or 0 */
	char directory[256];

	boolean requested;           /* a frame is asked for */
	struct render_window const *window; /* this frame's, or NULL */
	long unit_index;             /* his, while his view is drawn */
	boolean drawing;
	boolean reading;
	unsigned long requested_milliseconds;

	unsigned char const *pixels;
	long width;
	long height;
	unsigned long frame_index;

	/* how long drawing his view takes (the processor's, not the GPU's) */
	LARGE_INTEGER draw_start;
	double draw_seconds;
	double draw_longest_seconds;
	long draw_count;
} kevin_view_globals = { FALSE, 0, "", NULL, NONE };

/* ---------- private code */

static void kevin_view_save(
	void)
{
	char path[320];

	snprintf(path, sizeof(path), "%s/kevin%05lu.bmp", kevin_view_globals.directory, kevin_view_globals.frame_index);
	halo_screen_write_bmp(path, kevin_view_globals.pixels, kevin_view_globals.width, kevin_view_globals.height);

	return;
}

/* takes the frame read, when it has arrived */
static void kevin_view_collect(
	void)
{
	unsigned char const *pixels;
	long width;
	long height;

	if (!kevin_view_globals.reading)
		return;
	pixels = halo_screen_read_end(&width, &height);
	if (!pixels)
		return;
	kevin_view_globals.reading = FALSE;
	kevin_view_globals.pixels = pixels;
	kevin_view_globals.width = width;
	kevin_view_globals.height = height;
	kevin_view_globals.frame_index++;
	if (kevin_view_globals.frame_index == 1)
		error(_error_silent, "kevin: his view arrives, %ldx%ld", width, height);
	if (kevin_view_globals.directory[0])
		kevin_view_save();

	return;
}

/* from his eyes along his aim, as wide as your view */
static boolean kevin_view_camera(
	long unit_index,
	struct observer_result *camera)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct observer_result const *yours = observer_get_camera(0);
	real along;

	memset(camera, 0, sizeof(*camera));
	camera->forward = unit->unit.aiming_vector;
	if (normalize3d(&camera->forward) == 0.f)
		return FALSE;
	/* (up, square to his aim) */
	along = camera->forward.k;
	camera->up.i = -along * camera->forward.i;
	camera->up.j = -along * camera->forward.j;
	camera->up.k = 1.f - along * camera->forward.k;
	if (normalize3d(&camera->up) == 0.f)
		return FALSE;
	unit_get_camera_position(unit_index, &camera->position);
	camera->location = unit->object.location;
	camera->field_of_view = yours && yours->field_of_view > 0.f ? yours->field_of_view : KEVIN_VIEW_FIELD_OF_VIEW;

	return TRUE;
}

/* ---------- public code */

boolean kevin_view_due(
	struct observer_result *camera)
{
	unsigned long now;
	long unit_index;

	kevin_view_globals.window = NULL;
	if (!kevin_view_globals.settings_read)
	{
		kevin_view_globals.settings_read = TRUE;
		kevin_view_globals.every_milliseconds = 1000 * config_integer("debug.kevin_view_every");
		snprintf(kevin_view_globals.directory, sizeof(kevin_view_globals.directory), "%s",
			config_string("debug.kevin_view_directory"));
	}
	kevin_view_collect();
	now = system_milliseconds();
	if (kevin_view_globals.every_milliseconds > 0 &&
		now - kevin_view_globals.requested_milliseconds >= (unsigned long)kevin_view_globals.every_milliseconds)
	{
		kevin_view_request();
	}
	if (!kevin_view_globals.requested || kevin_view_globals.reading)
		return FALSE;
	unit_index = kevin_bot_unit_index();
	if (unit_index == NONE || TEST_FLAG(unit_get(unit_index)->object.damage_flags, _object_dead_bit))
		return FALSE;
	if (!kevin_view_camera(unit_index, camera))
		return FALSE;
	kevin_view_globals.requested = FALSE;
	kevin_view_globals.unit_index = unit_index;

	return TRUE;
}

void kevin_view_request(
	void)
{
	kevin_view_globals.requested = TRUE;
	kevin_view_globals.requested_milliseconds = system_milliseconds();

	return;
}

void kevin_view_bounds(
	rectangle2d *viewport_bounds,
	rectangle2d *window_bounds)
{
	/* the top left quarter of the screen */
	viewport_bounds->x1 = viewport_bounds->x0 + (viewport_bounds->x1 - viewport_bounds->x0) / 2;
	viewport_bounds->y1 = viewport_bounds->y0 + (viewport_bounds->y1 - viewport_bounds->y0) / 2;
	*window_bounds = *viewport_bounds;

	return;
}

void kevin_view_set_window(
	struct render_window const *window)
{
	kevin_view_globals.window = window;

	return;
}

boolean kevin_view_window(
	struct render_window const *window)
{
	return window != NULL && window == kevin_view_globals.window;
}

void kevin_view_begin(
	void)
{
	kevin_view_globals.drawing = TRUE;
	QueryPerformanceCounter(&kevin_view_globals.draw_start);

	return;
}

void kevin_view_end(
	rectangle2d const *viewport_bounds)
{
	LARGE_INTEGER end;
	LARGE_INTEGER frequency;
	double seconds;

	QueryPerformanceCounter(&end);
	QueryPerformanceFrequency(&frequency);
	seconds = frequency.QuadPart > 0 ?
		(double)(end.QuadPart - kevin_view_globals.draw_start.QuadPart) / (double)frequency.QuadPart : 0.0;
	kevin_view_globals.draw_seconds += seconds;
	if (seconds > kevin_view_globals.draw_longest_seconds)
		kevin_view_globals.draw_longest_seconds = seconds;
	if (++kevin_view_globals.draw_count >= KEVIN_VIEW_LOG_COUNT)
	{
		error(_error_silent, "kevin: his view takes %.2f ms to draw (the longest of %ld, %.2f)",
			1000.0 * kevin_view_globals.draw_seconds / kevin_view_globals.draw_count,
			kevin_view_globals.draw_count,
			1000.0 * kevin_view_globals.draw_longest_seconds);
		kevin_view_globals.draw_seconds = 0.0;
		kevin_view_globals.draw_longest_seconds = 0.0;
		kevin_view_globals.draw_count = 0;
	}

	/* (the window stays his until the next frame: the windows after it are
	drawn passing over it) */
	kevin_view_globals.drawing = FALSE;
	kevin_view_globals.reading = halo_screen_read_begin(
		viewport_bounds->x0,
		viewport_bounds->y0,
		viewport_bounds->x1,
		viewport_bounds->y1);

	return;
}

boolean kevin_view_camera_object(
	long object_index)
{
	if (!kevin_view_globals.drawing || object_index == NONE || kevin_view_globals.unit_index == NONE)
		return FALSE;

	/* (his unit, and what it holds: his weapon) */
	return object_index == kevin_view_globals.unit_index ||
		object_get(object_index)->object.parent_object_index == kevin_view_globals.unit_index;
}

unsigned char const *kevin_view_frame(
	long *width,
	long *height,
	unsigned long *frame_index)
{
	*width = kevin_view_globals.width;
	*height = kevin_view_globals.height;
	*frame_index = kevin_view_globals.frame_index;

	return kevin_view_globals.pixels;
}
