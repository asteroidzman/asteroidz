#ifndef ASTEROIDZ_TRACY_H
#define ASTEROIDZ_TRACY_H

/* asteroidz's side of the Tracy instrumentation.
 *
 * The client itself comes from the in-tree asteroidz-scenefx subproject, which
 * already ships include/render/tracy.h and already calls TRACY_MARK_FRAME from
 * wlr_scene_output_commit -- so asteroidz's zones land inside frame boundaries
 * that are delimited for us, and both halves of a frame report into the same
 * client. Building the library as a subproject rather than resolving it as an
 * external package is what makes that possible: its Tracy client used to be a
 * private static copy that asteroidz could not reach.
 *
 * Why not just use scenefx's macros: TRACY_ZONE_START hardcodes the context
 * variable name `ctx`, which is fine for one zone per scope but cannot nest,
 * and render_monitor wants an outer zone with inner ones inside it. These take
 * the variable name explicitly. Zones are also the only thing asteroidz needs
 * from that header -- the GPU-zone macros there are bound to the GLES timer
 * query path and do nothing under WLR_RENDERER=vulkan.
 *
 * All of this compiles to nothing without -Dtracy=true. */

#ifdef TRACY_ENABLE

#include <stdint.h>
#include <string.h>
#include <tracy/TracyC.h>

/* Zones. `var` is a plain local declared by AZ_ZONE, so an outer and an inner
 * zone in the same function just use different names. */
#define AZ_ZONE(var, name) TracyCZoneN(var, name, 1)
#define AZ_ZONE_C(var, name, color) TracyCZoneNC(var, name, color, 1)
#define AZ_ZONE_END(var) TracyCZoneEnd(var)

/* Attach the monitor/module/client this zone was for. Without it every frame
 * looks alike in the timeline and a multi-monitor trace is unreadable. */
#define AZ_ZONE_TEXT(var, str) \
	do { \
		const char *az__s = (str); \
		if (az__s) \
			TracyCZoneText(var, az__s, strlen(az__s)); \
	} while (0)
#define AZ_ZONE_VALUE(var, v) TracyCZoneValue(var, (uint64_t)(v))

/* Plots. These are the point of instrumenting the render-late scheduler: it is
 * a feedback loop, and a loop is read as a curve over frames, not as a set of
 * durations. */
#define AZ_PLOT(name, v) TracyCPlotF(name, (double)(v))
#define AZ_PLOT_INT(name, v) TracyCPlotI(name, (int64_t)(v))
#define AZ_PLOT_CONFIG(name, fmt, step, fill, color) \
	TracyCPlotConfig(name, fmt, step, fill, color)

#define AZ_MESSAGE(str) \
	do { \
		const char *az__m = (str); \
		if (az__m) \
			TracyCMessage(az__m, strlen(az__m)); \
	} while (0)

/* True only while a viewer is attached. TRACY_ON_DEMAND means nothing is
 * collected until then, so anything that costs real work to produce (a
 * formatted string, a walk over clients) belongs behind this. */
#define AZ_PROFILING TracyCIsConnected

#else /* !TRACY_ENABLE */

#define AZ_ZONE(var, name) ((void)0)
#define AZ_ZONE_C(var, name, color) ((void)0)
#define AZ_ZONE_END(var) ((void)0)
#define AZ_ZONE_TEXT(var, str) ((void)0)
#define AZ_ZONE_VALUE(var, v) ((void)0)
#define AZ_PLOT(name, v) ((void)0)
#define AZ_PLOT_INT(name, v) ((void)0)
#define AZ_PLOT_CONFIG(name, fmt, step, fill, color) ((void)0)
#define AZ_MESSAGE(str) ((void)0)
#define AZ_PROFILING 0

#endif /* TRACY_ENABLE */

/* Plot names. Kept together so the set is greppable and the viewer's plot
 * list stays stable between runs. */
#define AZ_PLOT_RENDER_MS "render+commit ms"
#define AZ_PLOT_LATE_FRAC "render-late frac"
#define AZ_PLOT_LATE_DELAY "render-late delay ms"
#define AZ_PLOT_LATE_SLIP "render-late vblank slip"

#endif /* ASTEROIDZ_TRACY_H */
