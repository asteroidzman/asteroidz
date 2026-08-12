/*
 * avk -- asteroidz's own Vulkan engine.
 *
 * This is not a wlr_renderer. It does not include one, it cannot be passed to
 * one, and nothing under src/render/vulkan/ may include a wlroots renderer,
 * EGL or GLES header -- meson enforces that with an include check, because the
 * whole point of this subsystem is that its design is not shaped by an
 * abstraction built for GLES. See docs/vulkan-native-architecture.md.
 *
 * The layer is deliberately compositor-agnostic: its inputs are DRM file
 * descriptors, dmabuf file descriptors and pixels. That is what makes it
 * testable without a compositor -- tests/test-avk-core.c opens a render node
 * and drives the whole thing with no Wayland display anywhere.
 */

#ifndef AVK_H
#define AVK_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/*
 * An integer rectangle, in whatever pixel space its holder documents.
 *
 * Here rather than in avk_scene.h because it is the vocabulary two subsystems
 * that do not know about each other both need: the scene describes where a
 * command lands, and the graph describes which part of a resource a pass
 * touches. Neither should have to include the other to say "rectangle".
 */
struct avk_box {
	int32_t x, y, width, height;
};

/* ── logging ────────────────────────────────────────────────────────────────
 *
 * avk has its own log rather than calling wlr_log, so that a) the layer does
 * not depend on wlroots for something as incidental as printing, and b) the
 * tests can run it without a compositor. The compositor installs a handler at
 * startup that forwards to wlr_log, so in a real session there is still one
 * log with one format.
 */
enum avk_log_level {
	AVK_SILENT = 0,
	AVK_ERROR,
	AVK_WARN,
	AVK_INFO,
	AVK_DEBUG,
};

typedef void (*avk_log_fn)(enum avk_log_level level, const char *fmt,
	va_list args);

/* NULL restores the built-in stderr handler. */
void avk_log_set_handler(avk_log_fn fn);
/* Messages above this level are dropped. Defaults to AVK_INFO, or AVK_DEBUG
 * when ASTEROIDZ_VK_DEBUG is set in the environment. */
void avk_log_set_level(enum avk_log_level level);
enum avk_log_level avk_log_get_level(void);
void avk_log(enum avk_log_level level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

/* How many validation errors the debug messenger has reported.
 *
 * A number rather than a log line, because "was this run clean?" should be
 * answerable by a test and by `amsg get avk-stats` without anyone grepping. */
uint64_t avk_validation_errors(void);
void avk_validation_error_count(void);

/* Whether developer mode is on (ASTEROIDZ_VK_DEBUG=1): validation layers,
 * object names, debug labels. Read once, at first use. */
bool avk_debug_enabled(void);

/* Human-readable VkResult. Vulkan's own error codes are the first thing you
 * want in a bug report and the last thing anyone can remember the number of. */
const char *avk_strerror(VkResult res);

/* Log `msg` with the decoded result and return false, so call sites can read
 *     if (res != VK_SUCCESS) return avk_check(res, "vkCreateFoo");
 * without four lines of formatting each time. Always returns false. */
bool avk_check(VkResult res, const char *msg);

#endif /* AVK_H */
