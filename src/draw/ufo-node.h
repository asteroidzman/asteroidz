#ifndef ASTEROIDZ_UFO_NODE_H
#define ASTEROIDZ_UFO_NODE_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

/*
 * asteroidz easter egg: at random intervals a classic Asteroids-style vector
 * ship flies across the top of the screen, shoots down a UFO, and blows it up.
 * Purely cosmetic, drawn with cairo into a scene-buffer on a topmost overlay
 * layer so it renders over the bar. Self-contained: owns its own timers.
 */

struct ufo_egg;

/* Fetch, at fire time, the strip (in layout coords) to animate over -- i.e. the
 * bar region of the active output. Return false to skip this fly-by. */
typedef bool (*ufo_geometry_fn)(void *user_data, int *x, int *y, int *w,
								int *h);

struct ufo_egg *ufo_egg_create(struct wl_event_loop *loop,
							   struct wlr_scene_tree *parent,
							   ufo_geometry_fn geometry, void *user_data);

/* Ship/bullet accent colour (0..1), e.g. the focused-border colour. */
void ufo_egg_set_accent(struct ufo_egg *egg, float r, float g, float b);

/* Enable/disable per config; enabling (re)arms the random scheduler. */
void ufo_egg_set_enabled(struct ufo_egg *egg, bool enabled);

/* Trigger a fly-by immediately (e.g. from a dispatcher), ignoring the timer. */
void ufo_egg_trigger(struct ufo_egg *egg);

void ufo_egg_destroy(struct ufo_egg *egg);

#endif
