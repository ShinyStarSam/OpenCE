/*
KEVIN_VIEW.H

Kevin's view (kevin.enabled): what his eyes see, drawn off screen when a
frame is asked for, for his AI to read (kevin_view.c).
*/

#ifndef __KEVIN_VIEW_H
#define __KEVIN_VIEW_H

struct observer_result;
struct render_window;

/* ---------- prototypes/KEVIN_VIEW.C */

/* asks for a frame of his view: drawn in the next frame shown while he is
alive, and there (kevin_view_frame) a frame or more after that */
void kevin_view_request(void);

/* main.c's main_render, each frame: whether his view is to be drawn this
frame (one has been asked for), and from where (his eyes); his window's
bounds, from the screen's; and the window it is drawn in this frame */
boolean kevin_view_due(struct observer_result *camera);
void kevin_view_bounds(rectangle2d *viewport_bounds, rectangle2d *window_bounds);
void kevin_view_set_window(struct render_window const *window);

/* render.c's render_frame: whether this window is his view, drawn before
the others; and around drawing it */
boolean kevin_view_window(struct render_window const *window);
void kevin_view_begin(void);
void kevin_view_end(rectangle2d const *viewport_bounds);

/* render_objects.c's object_is_first_person_camera: his unit, while his
view is drawn (it is not drawn, as yours is not in your view) */
boolean kevin_view_camera_object(long object_index);

/* the last frame of his view that has arrived (RGBA, rows from the top),
its size and its number (counting from 1); NULL before the first */
unsigned char const *kevin_view_frame(long *width, long *height, unsigned long *frame_index);

#endif
