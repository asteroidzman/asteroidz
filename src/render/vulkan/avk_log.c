#define _POSIX_C_SOURCE 200809L

#include "avk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static avk_log_fn log_handler = NULL;
static enum avk_log_level log_level = AVK_INFO;
static bool log_level_set = false;

static int debug_enabled = -1;

bool avk_debug_enabled(void) {
	if (debug_enabled < 0) {
		const char *env = getenv("ASTEROIDZ_VK_DEBUG");
		/* Present-and-not-"0" rather than strcmp("1"): people write =true,
		 * =yes and =on, and a developer-mode switch that silently ignores
		 * three of the four spellings is a switch that reads as broken. */
		debug_enabled = env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
		if (debug_enabled && !log_level_set) {
			log_level = AVK_DEBUG;
		}
	}
	return debug_enabled == 1;
}

static const char *level_name(enum avk_log_level level) {
	switch (level) {
	case AVK_ERROR: return "ERROR";
	case AVK_WARN:  return "WARN ";
	case AVK_INFO:  return "INFO ";
	case AVK_DEBUG: return "DEBUG";
	default:        return "?????";
	}
}

static void default_handler(enum avk_log_level level, const char *fmt,
		va_list args) {
	fprintf(stderr, "[avk %s] ", level_name(level));
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
}

void avk_log_set_handler(avk_log_fn fn) {
	log_handler = fn;
}

void avk_log_set_level(enum avk_log_level level) {
	log_level = level;
	log_level_set = true;
}

enum avk_log_level avk_log_get_level(void) {
	/* Touch the env probe so ASTEROIDZ_VK_DEBUG raises the default level even
	 * if nothing has called avk_debug_enabled() yet. */
	avk_debug_enabled();
	return log_level;
}

void avk_log(enum avk_log_level level, const char *fmt, ...) {
	if (level > avk_log_get_level()) {
		return;
	}

	va_list args;
	va_start(args, fmt);
	if (log_handler != NULL) {
		log_handler(level, fmt, args);
	} else {
		default_handler(level, fmt, args);
	}
	va_end(args);
}

const char *avk_strerror(VkResult res) {
	switch (res) {
	case VK_SUCCESS:                        return "VK_SUCCESS";
	case VK_NOT_READY:                      return "VK_NOT_READY";
	case VK_TIMEOUT:                        return "VK_TIMEOUT";
	case VK_EVENT_SET:                      return "VK_EVENT_SET";
	case VK_EVENT_RESET:                    return "VK_EVENT_RESET";
	case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
	case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
	case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
	case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_FRAGMENTED_POOL:          return "VK_ERROR_FRAGMENTED_POOL";
	case VK_ERROR_UNKNOWN:                  return "VK_ERROR_UNKNOWN";
	case VK_ERROR_OUT_OF_POOL_MEMORY:       return "VK_ERROR_OUT_OF_POOL_MEMORY";
	case VK_ERROR_INVALID_EXTERNAL_HANDLE:  return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
	case VK_ERROR_FRAGMENTATION:            return "VK_ERROR_FRAGMENTATION";
	case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS:
		return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
	case VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT:
		return "VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT";
	default:                                return "VkResult(unrecognised)";
	}
}

bool avk_check(VkResult res, const char *msg) {
	if (res == VK_SUCCESS) {
		return true;
	}
	avk_log(AVK_ERROR, "%s: %s (%d)", msg, avk_strerror(res), (int)res);
	return false;
}
