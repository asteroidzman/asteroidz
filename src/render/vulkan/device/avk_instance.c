#define _POSIX_C_SOURCE 200809L

#include "avk_instance.h"

#include <stdlib.h>
#include <string.h>

/*
 * Vulkan 1.3 is the floor.
 *
 * This is a deliberate baseline, not an oversight. 1.3 makes synchronization2,
 * dynamic rendering and the 1.2 timeline-semaphore/descriptor-indexing set
 * core, which removes roughly a dozen "is the extension there, and if so is it
 * the KHR or the core entry point" branches from every file below this one.
 * Every driver this compositor targets -- RADV, ANV, NVIDIA 525+ -- has shipped
 * 1.3 since 2022. Detecting a capability is right when the answer varies in
 * practice; branching on 1.1-vs-1.3 in 2026 is carrying dead code.
 *
 * Anything genuinely optional (memory budget, calibrated timestamps, the DRM
 * property extension) is detected per-device in avk_device.c instead.
 */
#define AVK_API_VERSION VK_API_VERSION_1_3

static const char *VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

static bool has_layer(const VkLayerProperties *layers, uint32_t count,
		const char *name) {
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(layers[i].layerName, name) == 0) {
			return true;
		}
	}
	return false;
}

static bool has_extension(const VkExtensionProperties *exts, uint32_t count,
		const char *name) {
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(exts[i].extensionName, name) == 0) {
			return true;
		}
	}
	return false;
}

static VkBool32 VKAPI_PTR debug_messenger_cb(
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT types,
		const VkDebugUtilsMessengerCallbackDataEXT *data,
		void *user_data) {
	(void)types;
	(void)user_data;

	enum avk_log_level level = AVK_DEBUG;
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		level = AVK_ERROR;
		avk_validation_error_count();
	} else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		level = AVK_WARN;
	} else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
		level = AVK_INFO;
	}

	avk_log(level, "validation: %s: %s",
		data->pMessageIdName ? data->pMessageIdName : "?",
		data->pMessage ? data->pMessage : "");

	/* VK_FALSE: never abort the call. A validation error should make the log
	 * loud, not take the desktop down -- the whole reason developer mode is
	 * usable as a daily session is that it complains instead of dying. */
	return VK_FALSE;
}

static void fill_messenger_info(VkDebugUtilsMessengerCreateInfoEXT *info) {
	*info = (VkDebugUtilsMessengerCreateInfoEXT){
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
		.messageType =
			VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
		.pfnUserCallback = debug_messenger_cb,
	};
	if (avk_log_get_level() >= AVK_DEBUG) {
		info->messageSeverity |=
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
	}
}

struct avk_instance *avk_instance_create(const char *app_name) {
	VkResult res;

	uint32_t loader_version = VK_API_VERSION_1_0;
	res = vkEnumerateInstanceVersion(&loader_version);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkEnumerateInstanceVersion");
		return NULL;
	}

	if (loader_version < AVK_API_VERSION) {
		avk_log(AVK_ERROR,
			"Vulkan loader reports %u.%u.%u; asteroidz requires at least "
			"%u.%u. Install a newer Vulkan loader/driver, or run the GLES "
			"renderer.",
			VK_API_VERSION_MAJOR(loader_version),
			VK_API_VERSION_MINOR(loader_version),
			VK_API_VERSION_PATCH(loader_version),
			VK_API_VERSION_MAJOR(AVK_API_VERSION),
			VK_API_VERSION_MINOR(AVK_API_VERSION));
		return NULL;
	}

	struct avk_instance *inst = calloc(1, sizeof(*inst));
	if (inst == NULL) {
		avk_log(AVK_ERROR, "allocation failed");
		return NULL;
	}
	inst->loader_api_version = loader_version;
	inst->api_version = AVK_API_VERSION;

	/* ── what is available ─────────────────────────────────────────────── */
	uint32_t ext_count = 0;
	VkExtensionProperties *exts = NULL;
	res = vkEnumerateInstanceExtensionProperties(NULL, &ext_count, NULL);
	if (res == VK_SUCCESS && ext_count > 0) {
		exts = calloc(ext_count, sizeof(*exts));
		if (exts == NULL) {
			avk_log(AVK_ERROR, "allocation failed");
			free(inst);
			return NULL;
		}
		res = vkEnumerateInstanceExtensionProperties(NULL, &ext_count, exts);
		if (res != VK_SUCCESS) {
			ext_count = 0;
		}
	}

	uint32_t layer_count = 0;
	VkLayerProperties *layers = NULL;
	res = vkEnumerateInstanceLayerProperties(&layer_count, NULL);
	if (res == VK_SUCCESS && layer_count > 0) {
		layers = calloc(layer_count, sizeof(*layers));
		if (layers != NULL) {
			res = vkEnumerateInstanceLayerProperties(&layer_count, layers);
			if (res != VK_SUCCESS) {
				layer_count = 0;
			}
		} else {
			layer_count = 0;
		}
	}

	inst->have_debug_utils =
		has_extension(exts, ext_count, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	inst->have_layer_settings =
		has_extension(exts, ext_count, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);

	const bool want_debug = avk_debug_enabled();
	const bool layer_present =
		layers != NULL && has_layer(layers, layer_count, VALIDATION_LAYER);

	if (want_debug && !layer_present) {
		avk_log(AVK_WARN,
			"ASTEROIDZ_VK_DEBUG is set but %s is not installed; continuing "
			"without validation (install the Vulkan validation layers)",
			VALIDATION_LAYER);
	}
	inst->validation_enabled = want_debug && layer_present;

	/* ── what we ask for ───────────────────────────────────────────────── */
	const char *enabled_exts[4];
	uint32_t enabled_ext_count = 0;
	if (inst->have_debug_utils) {
		/* Enabled whenever present, not only in developer mode: object names
		 * and command-buffer labels are what make a RenderDoc capture or a
		 * driver-side crash report legible, and they cost nothing when
		 * nothing is listening. */
		enabled_exts[enabled_ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	}
	if (inst->validation_enabled && inst->have_layer_settings) {
		enabled_exts[enabled_ext_count++] =
			VK_EXT_LAYER_SETTINGS_EXTENSION_NAME;
	}

	const char *enabled_layers[1];
	uint32_t enabled_layer_count = 0;
	if (inst->validation_enabled) {
		enabled_layers[enabled_layer_count++] = VALIDATION_LAYER;
	}

	/* Synchronization validation is the one that finds the bugs this
	 * subsystem is most likely to have -- a missing barrier renders correctly
	 * on the machine it was written on and corrupts on the next driver. It is
	 * on by default in developer mode. GPU-assisted validation is much slower
	 * and off unless asked for by name. */
	const VkBool32 vk_true = VK_TRUE;
	const char *gpuav_env = getenv("ASTEROIDZ_VK_DEBUG");
	const bool want_gpuav = gpuav_env != NULL && strstr(gpuav_env, "gpuav");

	VkLayerSettingEXT settings[2];
	uint32_t setting_count = 0;
	if (inst->validation_enabled && inst->have_layer_settings) {
		settings[setting_count++] = (VkLayerSettingEXT){
			.pLayerName = VALIDATION_LAYER,
			.pSettingName = "validate_sync",
			.type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
			.valueCount = 1,
			.pValues = &vk_true,
		};
		inst->sync_validation_enabled = true;
		if (want_gpuav) {
			settings[setting_count++] = (VkLayerSettingEXT){
				.pLayerName = VALIDATION_LAYER,
				.pSettingName = "gpuav_enable",
				.type = VK_LAYER_SETTING_TYPE_BOOL32_EXT,
				.valueCount = 1,
				.pValues = &vk_true,
			};
			inst->gpuav_enabled = true;
		}
	}

	VkLayerSettingsCreateInfoEXT settings_info = {
		.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
		.settingCount = setting_count,
		.pSettings = settings,
	};

	/* Chained into the create info so the messenger is live for the instance
	 * creation itself -- validation errors raised while creating the instance
	 * are otherwise reported to nobody. */
	VkDebugUtilsMessengerCreateInfoEXT messenger_info;
	fill_messenger_info(&messenger_info);

	const void *chain = NULL;
	if (setting_count > 0) {
		settings_info.pNext = chain;
		chain = &settings_info;
	}
	if (inst->have_debug_utils) {
		messenger_info.pNext = chain;
		chain = &messenger_info;
	}

	VkApplicationInfo app_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = app_name ? app_name : "asteroidz",
		.pEngineName = "avk",
		.apiVersion = AVK_API_VERSION,
	};

	VkInstanceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pNext = chain,
		.pApplicationInfo = &app_info,
		.enabledLayerCount = enabled_layer_count,
		.ppEnabledLayerNames = enabled_layers,
		.enabledExtensionCount = enabled_ext_count,
		.ppEnabledExtensionNames = enabled_exts,
	};

	res = vkCreateInstance(&create_info, NULL, &inst->instance);
	free(exts);
	free(layers);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateInstance");
		free(inst);
		return NULL;
	}

	if (inst->have_debug_utils) {
		inst->set_object_name = (PFN_vkSetDebugUtilsObjectNameEXT)
			vkGetInstanceProcAddr(inst->instance,
				"vkSetDebugUtilsObjectNameEXT");
		inst->cmd_begin_label = (PFN_vkCmdBeginDebugUtilsLabelEXT)
			vkGetInstanceProcAddr(inst->instance,
				"vkCmdBeginDebugUtilsLabelEXT");
		inst->cmd_end_label = (PFN_vkCmdEndDebugUtilsLabelEXT)
			vkGetInstanceProcAddr(inst->instance,
				"vkCmdEndDebugUtilsLabelEXT");
		inst->cmd_insert_label = (PFN_vkCmdInsertDebugUtilsLabelEXT)
			vkGetInstanceProcAddr(inst->instance,
				"vkCmdInsertDebugUtilsLabelEXT");
		inst->destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)
			vkGetInstanceProcAddr(inst->instance,
				"vkDestroyDebugUtilsMessengerEXT");

		PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
			(PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
				inst->instance, "vkCreateDebugUtilsMessengerEXT");
		if (create_messenger != NULL) {
			/* Reset pNext: the chained copy above borrowed it for instance
			 * creation, and a standalone messenger must not inherit the
			 * layer-settings link. */
			messenger_info.pNext = NULL;
			res = create_messenger(inst->instance, &messenger_info, NULL,
				&inst->messenger);
			if (res != VK_SUCCESS) {
				avk_check(res, "vkCreateDebugUtilsMessengerEXT");
				inst->messenger = VK_NULL_HANDLE;
			}
		}
	}

	return inst;
}

void avk_instance_destroy(struct avk_instance *inst) {
	if (inst == NULL) {
		return;
	}
	if (inst->messenger != VK_NULL_HANDLE && inst->destroy_messenger != NULL) {
		inst->destroy_messenger(inst->instance, inst->messenger, NULL);
	}
	if (inst->instance != VK_NULL_HANDLE) {
		vkDestroyInstance(inst->instance, NULL);
	}
	free(inst);
}

void avk_instance_log_caps(const struct avk_instance *inst) {
	avk_log(AVK_INFO, "avk instance: requesting Vulkan %u.%u, loader has %u.%u.%u",
		VK_API_VERSION_MAJOR(inst->api_version),
		VK_API_VERSION_MINOR(inst->api_version),
		VK_API_VERSION_MAJOR(inst->loader_api_version),
		VK_API_VERSION_MINOR(inst->loader_api_version),
		VK_API_VERSION_PATCH(inst->loader_api_version));
	avk_log(AVK_INFO, "avk instance: debug_utils=%s validation=%s "
		"sync_validation=%s gpu_assisted=%s",
		inst->have_debug_utils ? "yes" : "no",
		inst->validation_enabled ? "on" : "off",
		inst->sync_validation_enabled ? "on" : "off",
		inst->gpuav_enabled ? "on" : "off");
}
