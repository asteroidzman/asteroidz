#define _POSIX_C_SOURCE 200809L

#include "avk_phys.h"

#include "avk_color_caps.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

struct ext_flag {
	const char *name;
	size_t offset;      /* offset of the bool in struct avk_caps */
	bool required;
};

#define CAP_OFFSET(field) offsetof(struct avk_caps, field)

/*
 * Required means: without this, avk cannot do its job at all, and saying so at
 * selection time is far better than failing on the first client buffer.
 *
 *   external_memory_fd + external_memory_dma_buf : import client buffers
 *   image_drm_format_modifier                    : import them *correctly*,
 *                                                  with the producer's tiling
 *   external_semaphore_fd                        : explicit sync without a
 *                                                  CPU wait
 *   queue_family_foreign                         : ownership transfers for
 *                                                  images another device wrote
 */
static const struct ext_flag device_extensions[] = {
	{ VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		CAP_OFFSET(external_memory_fd), true },
	{ VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		CAP_OFFSET(external_memory_dma_buf), true },
	{ VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
		CAP_OFFSET(external_semaphore_fd), true },
	{ VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
		CAP_OFFSET(image_drm_format_modifier), true },
	{ VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
		CAP_OFFSET(queue_family_foreign), true },

	{ VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
		CAP_OFFSET(external_fence_fd), false },
	{ VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
		CAP_OFFSET(memory_budget), false },
	{ VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
		CAP_OFFSET(calibrated_timestamps), false },
	{ VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
		CAP_OFFSET(pipeline_executable_properties), false },
};

static bool *cap_bool(struct avk_caps *caps, size_t offset) {
	return (bool *)((char *)caps + offset);
}

static void query_queue_families(VkPhysicalDevice phys, struct avk_caps *caps) {
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, NULL);
	if (count == 0) {
		return;
	}
	VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
	if (families == NULL) {
		return;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families);

	bool found_graphics = false;
	for (uint32_t i = 0; i < count; i++) {
		VkQueueFlags flags = families[i].queueFlags;
		if (families[i].queueCount == 0) {
			continue;
		}

		/* GRAPHICS *and* COMPUTE on one family. Blur is a compute shader and
		 * composition is graphics; putting them on one queue means no
		 * ownership transfers and no cross-queue semaphores for what is
		 * really one frame's worth of work. Vulkan guarantees any
		 * graphics-capable family also supports transfer. */
		if (!found_graphics && (flags & VK_QUEUE_GRAPHICS_BIT)
				&& (flags & VK_QUEUE_COMPUTE_BIT)) {
			caps->graphics_family = i;
			/* Read here rather than beside timestampPeriod, because it is a
			 * property of THIS family and not of the device: a family can
			 * report 0 on a device whose period is perfectly good. */
			caps->timestamp_valid_bits = families[i].timestampValidBits;
			found_graphics = true;
		}

		/* Dedicated = no graphics. On AMD this is the SDMA/async-compute
		 * ring, which is what makes an upload overlap a frame rather than
		 * queue behind it. */
		if (!caps->has_dedicated_transfer_family
				&& (flags & VK_QUEUE_TRANSFER_BIT)
				&& !(flags & VK_QUEUE_GRAPHICS_BIT)
				&& !(flags & VK_QUEUE_COMPUTE_BIT)) {
			caps->has_dedicated_transfer_family = true;
			caps->transfer_family = i;
		}
		if (!caps->has_dedicated_compute_family
				&& (flags & VK_QUEUE_COMPUTE_BIT)
				&& !(flags & VK_QUEUE_GRAPHICS_BIT)) {
			caps->has_dedicated_compute_family = true;
			caps->compute_family = i;
		}
	}

	if (!found_graphics) {
		/* Signalled by leaving graphics_family at its calloc'd 0 and letting
		 * the caller's reject check catch it -- see avk_phys_enumerate. */
		caps->graphics_family = UINT32_MAX;
	}

	free(families);
}

static void query_external_sync(VkPhysicalDevice phys, struct avk_caps *caps) {
	/* Binary semaphore <-> sync_file. This is the interop that lets a client's
	 * implicit-sync dmabuf fence become a GPU-side wait instead of a CPU
	 * block, and lets our render completion become an IN_FENCE_FD for KMS. */
	VkPhysicalDeviceExternalSemaphoreInfo sem_info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	VkExternalSemaphoreProperties sem_props = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
	};
	vkGetPhysicalDeviceExternalSemaphoreProperties(phys, &sem_info,
		&sem_props);
	caps->semaphore_sync_fd_import = (sem_props.externalSemaphoreFeatures
		& VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
	caps->semaphore_sync_fd_export = (sem_props.externalSemaphoreFeatures
		& VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;

	/* Timeline semaphore as an opaque FD -- the handoff to a DRM syncobj
	 * timeline. The semaphore *type* has to be chained into the query or the
	 * driver answers about a binary semaphore, which is a different question
	 * with a different answer. */
	VkSemaphoreTypeCreateInfo type_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	};
	VkPhysicalDeviceExternalSemaphoreInfo timeline_info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
		.pNext = &type_info,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
	};
	VkExternalSemaphoreProperties timeline_props = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
	};
	vkGetPhysicalDeviceExternalSemaphoreProperties(phys, &timeline_info,
		&timeline_props);
	caps->timeline_opaque_fd_export = (timeline_props.externalSemaphoreFeatures
		& VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;
}

/*
 * M5 (contract C5): the two device-level colour questions, asked once.
 *
 * These are FORMAT questions, not extension questions, which is why they are
 * probed rather than inferred from a driver name: R16G16B16A16_SFLOAT is core
 * Vulkan and every driver supports it for something, and the something differs.
 * ADR-001 picks the working format by CRITERIA, and this is criterion (a).
 *
 * Nothing branches on the answers yet -- C5 is pure information, by design.
 * What it buys is that the day the intermediate is wired up, "does this device
 * support the working format" is a recorded fact from startup rather than a
 * question asked for the first time inside a frame.
 */
static void query_color_formats(VkPhysicalDevice phys, struct avk_caps *caps) {
	VkFormatProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
	};
	vkGetPhysicalDeviceFormatProperties2(phys, VK_FORMAT_R16G16B16A16_SFLOAT,
		&props);
	caps->fp16_optimal_features =
		props.formatProperties.optimalTilingFeatures;
	caps->fp16_attach_blend_sample =
		avk_fp16_working_format_ok(caps->fp16_optimal_features);

	props = (VkFormatProperties2){
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
	};
	vkGetPhysicalDeviceFormatProperties2(phys,
		VK_FORMAT_A2B10G10R10_UNORM_PACK32, &props);
	caps->rgb10a2_optimal_features =
		props.formatProperties.optimalTilingFeatures;
	caps->rgb10a2_attach =
		avk_rgb10a2_attach_ok(caps->rgb10a2_optimal_features);
}

static void query_caps(VkPhysicalDevice phys, struct avk_caps *caps) {
	memset(caps, 0, sizeof(*caps));

	/* ── extensions ────────────────────────────────────────────────────── */
	uint32_t ext_count = 0;
	VkExtensionProperties *exts = NULL;
	if (vkEnumerateDeviceExtensionProperties(phys, NULL, &ext_count, NULL)
			== VK_SUCCESS && ext_count > 0) {
		exts = calloc(ext_count, sizeof(*exts));
		if (exts != NULL && vkEnumerateDeviceExtensionProperties(phys, NULL,
				&ext_count, exts) != VK_SUCCESS) {
			ext_count = 0;
		}
	}

	bool has_drm_ext = false;
	for (uint32_t i = 0; i < ext_count; i++) {
		const char *name = exts[i].extensionName;
		for (size_t j = 0; j < sizeof(device_extensions)
				/ sizeof(device_extensions[0]); j++) {
			if (strcmp(name, device_extensions[j].name) == 0) {
				*cap_bool(caps, device_extensions[j].offset) = true;
			}
		}
		if (strcmp(name, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0) {
			has_drm_ext = true;
		}
	}
	free(exts);

	/* ── properties ────────────────────────────────────────────────────── */
	VkPhysicalDeviceDrmPropertiesEXT drm_props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
	};
	VkPhysicalDeviceDriverProperties driver_props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
		.pNext = has_drm_ext ? &drm_props : NULL,
	};
	VkPhysicalDeviceMaintenance3Properties maint3 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES,
		.pNext = &driver_props,
	};
	VkPhysicalDeviceProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &maint3,
	};
	vkGetPhysicalDeviceProperties2(phys, &props);

	snprintf(caps->device_name, sizeof(caps->device_name), "%s",
		props.properties.deviceName);
	snprintf(caps->driver_name, sizeof(caps->driver_name), "%s",
		driver_props.driverName);
	snprintf(caps->driver_info, sizeof(caps->driver_info), "%s",
		driver_props.driverInfo);
	caps->driver_id = driver_props.driverID;
	caps->vendor_id = props.properties.vendorID;
	caps->device_id = props.properties.deviceID;
	caps->device_type = props.properties.deviceType;
	caps->api_version = props.properties.apiVersion;

	caps->max_image_dimension_2d = props.properties.limits.maxImageDimension2D;
	caps->max_memory_allocation_size = maint3.maxMemoryAllocationSize;
	caps->timestamp_period = props.properties.limits.timestampPeriod;
	caps->non_coherent_atom_size =
		props.properties.limits.nonCoherentAtomSize;
	caps->optimal_buffer_copy_offset_alignment =
		props.properties.limits.optimalBufferCopyOffsetAlignment;
	caps->optimal_buffer_copy_row_pitch_alignment =
		props.properties.limits.optimalBufferCopyRowPitchAlignment;

	caps->has_drm = has_drm_ext;
	if (has_drm_ext) {
		caps->drm_has_primary = drm_props.hasPrimary;
		caps->drm_has_render = drm_props.hasRender;
		caps->drm_primary_major = drm_props.primaryMajor;
		caps->drm_primary_minor = drm_props.primaryMinor;
		caps->drm_render_major = drm_props.renderMajor;
		caps->drm_render_minor = drm_props.renderMinor;
	}

	/* ── features ──────────────────────────────────────────────────────── */
	VkPhysicalDeviceVulkan13Features vk13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
	};
	VkPhysicalDeviceVulkan12Features vk12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &vk13,
	};
	VkPhysicalDeviceVulkan11Features vk11 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
		.pNext = &vk12,
	};
	VkPhysicalDeviceFeatures2 features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &vk11,
	};
	vkGetPhysicalDeviceFeatures2(phys, &features);

	caps->timeline_semaphore = vk12.timelineSemaphore;
	caps->synchronization2 = vk13.synchronization2;
	caps->dynamic_rendering = vk13.dynamicRendering;
	caps->sampler_ycbcr_conversion = vk11.samplerYcbcrConversion;

	query_queue_families(phys, caps);
	query_external_sync(phys, caps);
	query_color_formats(phys, caps);
}

static const char *find_reject_reason(const struct avk_caps *caps) {
	if (caps->api_version < VK_API_VERSION_1_3) {
		return "device API version below 1.3";
	}
	if (caps->graphics_family == UINT32_MAX) {
		return "no queue family with both graphics and compute";
	}
	if (!caps->timeline_semaphore) {
		return "no timeline semaphores";
	}
	if (!caps->synchronization2) {
		return "no synchronization2";
	}
	for (size_t i = 0; i < sizeof(device_extensions)
			/ sizeof(device_extensions[0]); i++) {
		if (!device_extensions[i].required) {
			continue;
		}
		if (!*cap_bool((struct avk_caps *)caps, device_extensions[i].offset)) {
			return device_extensions[i].name;
		}
	}
	return NULL;
}

bool avk_phys_enumerate(struct avk_instance *inst, struct avk_phys **out,
		uint32_t *count) {
	*out = NULL;
	*count = 0;

	uint32_t n = 0;
	VkResult res = vkEnumeratePhysicalDevices(inst->instance, &n, NULL);
	if (res != VK_SUCCESS) {
		return avk_check(res, "vkEnumeratePhysicalDevices");
	}
	if (n == 0) {
		avk_log(AVK_ERROR, "no Vulkan physical devices");
		return false;
	}

	VkPhysicalDevice *handles = calloc(n, sizeof(*handles));
	struct avk_phys *list = calloc(n, sizeof(*list));
	if (handles == NULL || list == NULL) {
		free(handles);
		free(list);
		avk_log(AVK_ERROR, "allocation failed");
		return false;
	}

	res = vkEnumeratePhysicalDevices(inst->instance, &n, handles);
	if (res != VK_SUCCESS) {
		free(handles);
		free(list);
		return avk_check(res, "vkEnumeratePhysicalDevices");
	}

	for (uint32_t i = 0; i < n; i++) {
		list[i].handle = handles[i];
		query_caps(handles[i], &list[i].caps);
		list[i].reject_reason = find_reject_reason(&list[i].caps);
	}

	free(handles);
	*out = list;
	*count = n;
	return true;
}

/* Both the render node and the card node are accepted: the compositor holds
 * whichever the session gave it, and a device is the same device either way. */
static bool drm_matches(const struct avk_caps *caps, int64_t major,
		int64_t minor) {
	if (!caps->has_drm) {
		return false;
	}
	if (caps->drm_has_render && caps->drm_render_major == major
			&& caps->drm_render_minor == minor) {
		return true;
	}
	if (caps->drm_has_primary && caps->drm_primary_major == major
			&& caps->drm_primary_minor == minor) {
		return true;
	}
	return false;
}

int avk_phys_select(const struct avk_phys *list, uint32_t count, int drm_fd) {
	if (drm_fd >= 0) {
		struct stat st;
		if (fstat(drm_fd, &st) != 0) {
			avk_log(AVK_ERROR,
				"cannot fstat the DRM fd, so the GPU cannot be identified");
			return -1;
		}
		int64_t want_major = (int64_t)major(st.st_rdev);
		int64_t want_minor = (int64_t)minor(st.st_rdev);

		for (uint32_t i = 0; i < count; i++) {
			if (!drm_matches(&list[i].caps, want_major, want_minor)) {
				continue;
			}
			if (list[i].reject_reason != NULL) {
				avk_log(AVK_ERROR,
					"the GPU on DRM node %" PRId64 ":%" PRId64 " (%s) cannot "
					"be used: %s",
					want_major, want_minor, list[i].caps.device_name,
					list[i].reject_reason);
				return -1;
			}
			return (int)i;
		}

		/* Deliberately a hard failure rather than "fall back to device 0".
		 * Falling back is how a compositor ends up rendering on the iGPU
		 * while presenting on the dGPU, with every client buffer arriving
		 * cross-device and nothing in the log saying so. */
		avk_log(AVK_ERROR,
			"no Vulkan device claims DRM node %" PRId64 ":%" PRId64,
			want_major, want_minor);
		for (uint32_t i = 0; i < count; i++) {
			if (!list[i].caps.has_drm) {
				avk_log(AVK_ERROR,
					"  %s: driver does not implement %s, so its DRM node is "
					"unknowable",
					list[i].caps.device_name,
					VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
			}
		}
		return -1;
	}

	/* No node given: tests only. Discrete first, then anything usable. */
	int fallback = -1;
	for (uint32_t i = 0; i < count; i++) {
		if (list[i].reject_reason != NULL) {
			continue;
		}
		if (list[i].caps.device_type
				== VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			return (int)i;
		}
		if (fallback < 0) {
			fallback = (int)i;
		}
	}
	return fallback;
}

static const char *device_type_name(VkPhysicalDeviceType type) {
	switch (type) {
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
	case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
	default:                                     return "other";
	}
}

void avk_phys_log_all(const struct avk_phys *list, uint32_t count,
		int selected) {
	for (uint32_t i = 0; i < count; i++) {
		const struct avk_caps *c = &list[i].caps;
		char drm[64] = "drm=unknown";
		if (c->has_drm) {
			snprintf(drm, sizeof(drm),
				"drm=%" PRId64 ":%" PRId64 "/%" PRId64 ":%" PRId64,
				c->drm_has_primary ? c->drm_primary_major : -1,
				c->drm_has_primary ? c->drm_primary_minor : -1,
				c->drm_has_render ? c->drm_render_major : -1,
				c->drm_has_render ? c->drm_render_minor : -1);
		}
		avk_log(AVK_INFO, "avk GPU%u%s: %s [%s, %s, %s] %s%s%s",
			i, (int)i == selected ? " (selected)" : "",
			c->device_name, c->driver_name,
			device_type_name(c->device_type), drm,
			list[i].reject_reason ? "UNUSABLE: " : "usable",
			list[i].reject_reason ? list[i].reject_reason : "",
			"");
	}
}
