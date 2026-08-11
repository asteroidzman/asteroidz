#ifndef AVK_INSTANCE_H
#define AVK_INSTANCE_H

#include "../avk.h"

/*
 * The VkInstance, and everything that is a property of the loader rather than
 * of a device: API version, layers, instance extensions, the debug messenger.
 *
 * One per process. asteroidz creates it once at startup and hands it to every
 * avk_device; the tests create and destroy it repeatedly, which is why
 * teardown has to be genuinely complete rather than "the process is exiting
 * anyway".
 */

struct avk_instance {
	VkInstance instance;

	/* What the loader reported, and what we actually asked for -- not the same
	 * number, and the difference matters when a bug report says "1.4". */
	uint32_t loader_api_version;
	uint32_t api_version;

	bool have_debug_utils;
	bool have_layer_settings;
	bool validation_enabled;
	bool sync_validation_enabled;
	bool gpuav_enabled;

	VkDebugUtilsMessengerEXT messenger;

	/* Resolved once here rather than at every call site: these are extension
	 * entry points, so vkGetInstanceProcAddr is the only way to reach them and
	 * a NULL check on the pointer is the only way to know they exist. */
	PFN_vkSetDebugUtilsObjectNameEXT set_object_name;
	PFN_vkCmdBeginDebugUtilsLabelEXT cmd_begin_label;
	PFN_vkCmdEndDebugUtilsLabelEXT cmd_end_label;
	PFN_vkCmdInsertDebugUtilsLabelEXT cmd_insert_label;
	PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger;
};

/*
 * Create the instance.
 *
 * `app_name` is what shows up in RenderDoc and in driver logs; pass something
 * that identifies the process, not the subsystem.
 *
 * Validation is enabled when developer mode is on (ASTEROIDZ_VK_DEBUG) AND the
 * layer is actually installed. A missing layer is a warning, never a failure:
 * the layer is a developer package and refusing to start the compositor
 * because it is absent would make developer mode a footgun.
 */
struct avk_instance *avk_instance_create(const char *app_name);
void avk_instance_destroy(struct avk_instance *inst);

/* Log the instance-level capability table at INFO. Separated from creation so
 * the tests can assert on the struct rather than on the log text. */
void avk_instance_log_caps(const struct avk_instance *inst);

#endif /* AVK_INSTANCE_H */
