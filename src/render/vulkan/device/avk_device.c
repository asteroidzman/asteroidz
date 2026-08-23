/* _GNU_SOURCE for asprintf(), which is what keeps the cache-path building
 * free of fixed-size buffers and the truncation checks they need. */
#define _GNU_SOURCE

#include "avk_device.h"
#include "avk_phys.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── pipeline cache ─────────────────────────────────────────────────────────
 *
 * On disk, keyed by driver, so a driver update does not feed a stale blob to a
 * new compiler. The cache is an optimisation for first-use hitches only; every
 * failure here is a warning, never a hard error -- a compositor that refuses to
 * start because $XDG_CACHE_HOME is read-only would be an absurd trade.
 */
static char *pipeline_cache_path(const struct avk_caps *caps) {
	const char *base = getenv("XDG_CACHE_HOME");
	char dir[PATH_MAX];
	if (base != NULL && base[0] == '/') {
		snprintf(dir, sizeof(dir), "%s/asteroidz", base);
	} else {
		const char *home = getenv("HOME");
		if (home == NULL || home[0] != '/') {
			return NULL;
		}
		snprintf(dir, sizeof(dir), "%s/.cache/asteroidz", home);
	}

	if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
		avk_log(AVK_DEBUG, "pipeline cache: cannot create %s: %s", dir,
			strerror(errno));
		return NULL;
	}

	char *path = NULL;
	if (asprintf(&path, "%s/avk-pipeline-%08x-%08x-%u.bin", dir,
			caps->vendor_id, caps->device_id, (unsigned)caps->driver_id) < 0) {
		return NULL;
	}
	return path;
}

static void create_pipeline_cache(struct avk_device *dev) {
	dev->pipeline_cache_path = pipeline_cache_path(&dev->caps);

	void *data = NULL;
	size_t size = 0;
	if (dev->pipeline_cache_path != NULL) {
		FILE *f = fopen(dev->pipeline_cache_path, "rb");
		if (f != NULL) {
			if (fseek(f, 0, SEEK_END) == 0) {
				long len = ftell(f);
				if (len > 0 && fseek(f, 0, SEEK_SET) == 0) {
					data = malloc((size_t)len);
					if (data != NULL
							&& fread(data, 1, (size_t)len, f) == (size_t)len) {
						size = (size_t)len;
					} else {
						free(data);
						data = NULL;
					}
				}
			}
			fclose(f);
		}
	}

	VkPipelineCacheCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
		.initialDataSize = size,
		.pInitialData = data,
	};
	VkResult res = vkCreatePipelineCache(dev->dev, &info, NULL,
		&dev->pipeline_cache);
	if (res != VK_SUCCESS) {
		/* A corrupt or foreign blob is the likely cause; drop it and retry
		 * empty rather than run with no cache for the rest of the session. */
		avk_log(AVK_WARN, "pipeline cache rejected (%s); starting empty",
			avk_strerror(res));
		info.initialDataSize = 0;
		info.pInitialData = NULL;
		res = vkCreatePipelineCache(dev->dev, &info, NULL,
			&dev->pipeline_cache);
		if (res != VK_SUCCESS) {
			avk_check(res, "vkCreatePipelineCache");
			dev->pipeline_cache = VK_NULL_HANDLE;
		}
	} else if (size > 0) {
		avk_log(AVK_DEBUG, "pipeline cache: loaded %zu bytes from %s", size,
			dev->pipeline_cache_path);
	}
	free(data);
}

void avk_device_save_pipeline_cache(struct avk_device *dev) {
	if (dev->pipeline_cache == VK_NULL_HANDLE
			|| dev->pipeline_cache_path == NULL) {
		return;
	}

	size_t size = 0;
	if (vkGetPipelineCacheData(dev->dev, dev->pipeline_cache, &size, NULL)
			!= VK_SUCCESS || size == 0) {
		return;
	}
	void *data = malloc(size);
	if (data == NULL) {
		return;
	}
	if (vkGetPipelineCacheData(dev->dev, dev->pipeline_cache, &size, data)
			!= VK_SUCCESS) {
		free(data);
		return;
	}

	/* Written to a temporary and renamed: a compositor that is killed
	 * mid-write would otherwise leave a truncated cache that the next start
	 * has to detect and discard. */
	char *tmp = NULL;
	if (asprintf(&tmp, "%s.tmp", dev->pipeline_cache_path) < 0) {
		free(data);
		return;
	}
	FILE *f = fopen(tmp, "wb");
	if (f != NULL) {
		bool ok = fwrite(data, 1, size, f) == size;
		ok = (fclose(f) == 0) && ok;
		if (ok) {
			if (rename(tmp, dev->pipeline_cache_path) == 0) {
				avk_log(AVK_DEBUG, "pipeline cache: wrote %zu bytes to %s",
					size, dev->pipeline_cache_path);
			} else {
				unlink(tmp);
			}
		} else {
			unlink(tmp);
		}
	}
	free(tmp);
	free(data);
}

/* ── device ─────────────────────────────────────────────────────────────── */

struct avk_device *avk_device_create(struct avk_instance *inst, int drm_fd) {
	struct avk_phys *list = NULL;
	uint32_t count = 0;
	if (!avk_phys_enumerate(inst, &list, &count)) {
		return NULL;
	}

	int index = avk_phys_select(list, count, drm_fd);
	avk_phys_log_all(list, count, index);
	if (index < 0) {
		free(list);
		return NULL;
	}

	struct avk_device *dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		free(list);
		avk_log(AVK_ERROR, "allocation failed");
		return NULL;
	}
	dev->instance = inst;
	dev->phys = list[index].handle;
	dev->caps = list[index].caps;
	dev->drm_fd = -1;
	free(list);

	if (drm_fd >= 0) {
		dev->drm_fd = fcntl(drm_fd, F_DUPFD_CLOEXEC, 0);
		if (dev->drm_fd < 0) {
			avk_log(AVK_ERROR, "cannot dup the DRM fd: %s", strerror(errno));
			free(dev);
			return NULL;
		}
	}

	/* ── extensions to enable ──────────────────────────────────────────── */
	const char *exts[16];
	uint32_t ext_count = 0;
	exts[ext_count++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
	exts[ext_count++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
	exts[ext_count++] = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
	exts[ext_count++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME;
	exts[ext_count++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
	if (dev->caps.external_fence_fd) {
		exts[ext_count++] = VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME;
	}
	if (dev->caps.memory_budget) {
		exts[ext_count++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
	}
	if (dev->caps.calibrated_timestamps) {
		exts[ext_count++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
	}

	/* ── video encode (M14) ────────────────────────────────────────────
	 *
	 * All or nothing. A session needs the queue family, VK_KHR_video_queue,
	 * VK_KHR_video_encode_queue and the codec's own extension; enabling a
	 * subset buys nothing and leaves a half-capable device to be discovered
	 * later, at session creation, by code that has already assumed it works.
	 *
	 * The RGB conversion extension is in the same bundle deliberately. Without
	 * it the encoder's input picture is P010 and an RGB->YUV pass becomes the
	 * compositor's to write and to get wrong; the whole reason encoding here
	 * is cheap is that the encoder takes the composited image untouched. */
	bool want_encode = dev->caps.has_video_encode_family
		&& dev->caps.video_queue && dev->caps.video_encode_queue
		&& dev->caps.video_encode_h265 && dev->caps.video_encode_rgb;
	if (want_encode) {
		exts[ext_count++] = VK_KHR_VIDEO_QUEUE_EXTENSION_NAME;
		exts[ext_count++] = VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME;
		exts[ext_count++] = VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME;
		exts[ext_count++] = VK_VALVE_VIDEO_ENCODE_RGB_CONVERSION_EXTENSION_NAME;
	}

	/* ── features to enable ────────────────────────────────────────────── */
	VkPhysicalDeviceVulkan13Features vk13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.synchronization2 = VK_TRUE,
		.dynamicRendering = dev->caps.dynamic_rendering ? VK_TRUE : VK_FALSE,
	};
	VkPhysicalDeviceVulkan12Features vk12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &vk13,
		.timelineSemaphore = VK_TRUE,
	};
	VkPhysicalDeviceVulkan11Features vk11 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
		.pNext = &vk12,
		.samplerYcbcrConversion =
			dev->caps.sampler_ycbcr_conversion ? VK_TRUE : VK_FALSE,
	};
	VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE rgb_conv = {
		.sType =
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_RGB_CONVERSION_FEATURES_VALVE,
		.pNext = &vk11,
		.videoEncodeRgbConversion = VK_TRUE,
	};
	VkPhysicalDeviceFeatures2 features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = want_encode ? (void *)&rgb_conv : (void *)&vk11,
	};

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = dev->caps.graphics_family,
			.queueCount = 1,
			.pQueuePriorities = &priority,
		},
	};
	uint32_t queue_info_count = 1;
	if (want_encode) {
		queue_info[queue_info_count++] = (VkDeviceQueueCreateInfo){
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = dev->caps.video_encode_family,
			.queueCount = 1,
			.pQueuePriorities = &priority,
		};
	}

	VkDeviceCreateInfo device_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features,
		.queueCreateInfoCount = queue_info_count,
		.pQueueCreateInfos = queue_info,
		.enabledExtensionCount = ext_count,
		.ppEnabledExtensionNames = exts,
	};

	VkResult res = vkCreateDevice(dev->phys, &device_info, NULL, &dev->dev);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateDevice");
		goto error;
	}

	vkGetDeviceQueue(dev->dev, dev->caps.graphics_family, 0,
		&dev->graphics_queue);
	if (want_encode) {
		vkGetDeviceQueue(dev->dev, dev->caps.video_encode_family, 0,
			&dev->encode_queue);
		dev->has_encode_queue = dev->encode_queue != VK_NULL_HANDLE;
	}

	/* VK_KHR_external_semaphore_fd is always in the extension list above, so a
	 * NULL here means the loader and the driver disagree about what was
	 * enabled -- worth saying out loud, because the consequence is that AVK
	 * cannot attach a fence to a presented frame and will decline every
	 * output rather than present one unsynchronised. */
	dev->api.vkGetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)
		vkGetDeviceProcAddr(dev->dev, "vkGetSemaphoreFdKHR");
	dev->api.vkImportSemaphoreFdKHR = (PFN_vkImportSemaphoreFdKHR)
		vkGetDeviceProcAddr(dev->dev, "vkImportSemaphoreFdKHR");
	if (dev->api.vkGetSemaphoreFdKHR == NULL ||
			dev->api.vkImportSemaphoreFdKHR == NULL) {
		avk_log(AVK_ERROR, "VK_KHR_external_semaphore_fd was enabled but its "
			"entry points are missing");
	}

	/* ── the device timeline ───────────────────────────────────────────── */
	VkSemaphoreTypeCreateInfo timeline_type = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		.initialValue = 0,
	};
	VkSemaphoreCreateInfo sem_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = &timeline_type,
	};
	res = vkCreateSemaphore(dev->dev, &sem_info, NULL, &dev->timeline);
	if (res == VK_SUCCESS) {
		AVK_LIVE_INC(dev, semaphores);
	}
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateSemaphore (device timeline)");
		goto error;
	}
	/* Starts at 1, not 0: 0 is the value the semaphore already has, so a
	 * resource retired "at 0" would be freed before its submission was even
	 * recorded. Every reserved point is therefore strictly in the future. */
	dev->timeline_next = 1;

	create_pipeline_cache(dev);

	avk_device_name_object(dev, VK_OBJECT_TYPE_SEMAPHORE,
		(uint64_t)dev->timeline, "avk device timeline");
	avk_device_name_object(dev, VK_OBJECT_TYPE_QUEUE,
		(uint64_t)dev->graphics_queue, "avk graphics queue (family %u)",
		dev->caps.graphics_family);
	if (dev->has_encode_queue) {
		avk_device_name_object(dev, VK_OBJECT_TYPE_QUEUE,
			(uint64_t)dev->encode_queue, "avk video encode queue (family %u)",
			dev->caps.video_encode_family);
	}

	return dev;

error:
	if (dev->timeline != VK_NULL_HANDLE) {
		vkDestroySemaphore(dev->dev, dev->timeline, NULL);
	}
	if (dev->dev != VK_NULL_HANDLE) {
		vkDestroyDevice(dev->dev, NULL);
	}
	if (dev->drm_fd >= 0) {
		close(dev->drm_fd);
	}
	free(dev);
	return NULL;
}

/*
 * Wait for the GPU to finish everything.
 *
 * Separated from destruction on purpose. The wait used to live inside
 * avk_device_destroy() alone, which is the LAST thing shutdown calls -- so
 * every caller that needed quiescence before destroying its own resources had
 * either to know that some other finish() happened to wait too, or to do
 * without. Quiescence is a precondition of destruction, not a step inside it,
 * and a precondition a caller cannot invoke is not one they can rely on.
 *
 * This is a shutdown and device-loss facility. A frame path that calls it is
 * a bug -- see docs/architecture.md §5.7 -- and it deliberately
 * does not touch avk.cpu_sync_waits, which counts stalls in the frame path.
 */
void avk_device_wait_idle(struct avk_device *dev) {
	if (dev == NULL || dev->dev == VK_NULL_HANDLE) {
		return;
	}
	vkDeviceWaitIdle(dev->dev);
}

int avk_device_log_live_objects(const struct avk_device *dev, const char *when) {
	if (dev == NULL) {
		return 0;
	}
	const struct avk_live_objects *l = &dev->live;
	struct { const char *name; int64_t n; } classes[] = {
		{ "images", l->images },
		{ "image_views", l->image_views },
		{ "device_memory", l->device_memory },
		{ "buffers", l->buffers },
		{ "samplers", l->samplers },
		{ "pipelines", l->pipelines },
		{ "pipeline_layouts", l->pipeline_layouts },
		{ "descriptor_set_layouts", l->descriptor_set_layouts },
		{ "descriptor_pools", l->descriptor_pools },
		{ "command_pools", l->command_pools },
		{ "semaphores", l->semaphores },
		{ "query_pools", l->query_pools },
		{ "avk_images", l->avk_images },
		{ "avk_uploads", l->avk_uploads },
		{ "retire_entries", l->retire_entries },
	};

	int nonzero = 0;
	char line[512];
	size_t off = 0;
	for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
		if (classes[i].n != 0) {
			nonzero++;
		}
		int n = snprintf(line + off, sizeof(line) - off, "%s%s=%" PRId64,
			off == 0 ? "" : " ", classes[i].name, classes[i].n);
		if (n < 0 || (size_t)n >= sizeof(line) - off) {
			break;
		}
		off += (size_t)n;
	}
	/* Loud when it matters, quiet when it does not: a leak or a double
	 * destruction at shutdown is a defect, and a defect that logs at DEBUG is
	 * a defect nobody reads. */
	avk_log(nonzero == 0 ? AVK_INFO : AVK_ERROR,
		"live objects %s: %s%s", when, line,
		nonzero == 0 ? " (all zero)" : "  <-- NOT ZERO");
	return nonzero;
}

void avk_device_destroy(struct avk_device *dev) {
	if (dev == NULL) {
		return;
	}

	avk_device_save_pipeline_cache(dev);

	/* Belt to az_avk_finish()'s braces. By the time shutdown gets here the
	 * GPU has already been waited for once, before anything was destroyed --
	 * which is the wait that matters. This one covers avk_device_destroy()
	 * being called on its own, as the tests and the device-loss path do. */
	avk_device_wait_idle(dev);

	/* The warm staging buffers are AVK's, not a subsystem's, and the census
	 * below must see zero. */
	avk_staging_cache_drain(dev);

	/* The device's own two children go first, so that what the census below
	 * reports is what AVK's SUBSYSTEMS still hold. Counting the device's own
	 * timeline semaphore as an outstanding object would put a permanent 1 in
	 * a line whose entire value is that every number in it is 0. */
	if (dev->pipeline_cache != VK_NULL_HANDLE) {
		vkDestroyPipelineCache(dev->dev, dev->pipeline_cache, NULL);
	}
	if (dev->timeline != VK_NULL_HANDLE) {
		vkDestroySemaphore(dev->dev, dev->timeline, NULL);
		AVK_LIVE_DEC(dev, semaphores);
	}

	/* What AVK still owns, immediately before the device that owns it goes.
	 * Every count must be zero here. A positive one is a leak; a negative one
	 * is a double destruction, which is a double free of the driver's host
	 * allocation and the reason a shutdown can abort inside glibc with no AVK
	 * frame on the stack. */
	avk_device_log_live_objects(dev, "before vkDestroyDevice");
	if (dev->lifecycle_violations > 0) {
		avk_log(AVK_ERROR, "%" PRIu64 " AVK ownership violations were detected "
			"during this session", dev->lifecycle_violations);
	}

	if (dev->dev != VK_NULL_HANDLE) {
		vkDestroyDevice(dev->dev, NULL);
	}
	if (dev->drm_fd >= 0) {
		close(dev->drm_fd);
	}
	free(dev->pipeline_cache_path);
	free(dev);
}

uint64_t avk_device_timeline_value(struct avk_device *dev) {
	uint64_t value = 0;
	VkResult res = vkGetSemaphoreCounterValue(dev->dev, dev->timeline, &value);
	if (res != VK_SUCCESS) {
		/* Includes VK_ERROR_DEVICE_LOST. Reporting 0 is the safe direction:
		 * nothing is considered retired, so nothing in use gets freed while
		 * device-loss recovery works out what to do. */
		avk_check(res, "vkGetSemaphoreCounterValue");
		return 0;
	}
	return value;
}

uint64_t avk_device_timeline_reserve(struct avk_device *dev) {
	return dev->timeline_next++;
}

bool avk_device_timeline_wait(struct avk_device *dev, uint64_t value,
		uint64_t timeout_ns) {
	VkSemaphoreWaitInfo info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &dev->timeline,
		.pValues = &value,
	};
	VkResult res = vkWaitSemaphores(dev->dev, &info, timeout_ns);
	if (res == VK_TIMEOUT) {
		return false;
	}
	return avk_check(res, "vkWaitSemaphores");
}

void avk_device_name_object(struct avk_device *dev, VkObjectType type,
		uint64_t handle, const char *fmt, ...) {
	if (dev->instance->set_object_name == NULL || handle == 0) {
		return;
	}

	char name[128];
	va_list args;
	va_start(args, fmt);
	vsnprintf(name, sizeof(name), fmt, args);
	va_end(args);

	VkDebugUtilsObjectNameInfoEXT info = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		.objectType = type,
		.objectHandle = handle,
		.pObjectName = name,
	};
	dev->instance->set_object_name(dev->dev, &info);
}

void avk_device_log_caps(const struct avk_device *dev) {
	const struct avk_caps *c = &dev->caps;

	avk_log(AVK_INFO, "avk device: %s", c->device_name);
	avk_log(AVK_INFO, "avk device: driver %s (%s), API %u.%u.%u, "
		"vendor 0x%04x device 0x%04x",
		c->driver_name, c->driver_info,
		VK_API_VERSION_MAJOR(c->api_version),
		VK_API_VERSION_MINOR(c->api_version),
		VK_API_VERSION_PATCH(c->api_version),
		c->vendor_id, c->device_id);

	if (c->has_drm) {
		avk_log(AVK_INFO, "avk device: DRM primary %" PRId64 ":%" PRId64
			", render %" PRId64 ":%" PRId64,
			c->drm_has_primary ? c->drm_primary_major : -1,
			c->drm_has_primary ? c->drm_primary_minor : -1,
			c->drm_has_render ? c->drm_render_major : -1,
			c->drm_has_render ? c->drm_render_minor : -1);
	} else {
		avk_log(AVK_WARN, "avk device: no %s -- this GPU cannot be matched to "
			"a DRM node, so multi-GPU buffer routing is guesswork",
			VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
	}

	avk_log(AVK_INFO, "avk caps: timeline=%d sync2=%d dynamic_rendering=%d "
		"ycbcr=%d",
		c->timeline_semaphore, c->synchronization2, c->dynamic_rendering,
		c->sampler_ycbcr_conversion);
	avk_log(AVK_INFO, "avk caps: dmabuf=%d drm_modifiers=%d foreign_queue=%d "
		"external_mem_fd=%d external_sem_fd=%d external_fence_fd=%d",
		c->external_memory_dma_buf, c->image_drm_format_modifier,
		c->queue_family_foreign, c->external_memory_fd,
		c->external_semaphore_fd, c->external_fence_fd);
	avk_log(AVK_INFO, "avk caps: sync_file import=%d export=%d, "
		"timeline opaque-fd export=%d %s",
		c->semaphore_sync_fd_import, c->semaphore_sync_fd_export,
		c->timeline_opaque_fd_export,
		(c->semaphore_sync_fd_import && c->semaphore_sync_fd_export)
			? "(implicit-sync interop available)"
			: "(NO implicit-sync interop -- fences may need CPU waits)");
	avk_log(AVK_INFO, "avk caps: queues graphics+compute=%u transfer=%s "
		"compute=%s",
		c->graphics_family,
		c->has_dedicated_transfer_family ? "yes" : "none",
		c->has_dedicated_compute_family ? "yes" : "none");
	/* M14. Named piece by piece for the same reason as the colour line: a
	 * device with the queue but no RGB conversion is a different conversation
	 * from one with no encode hardware at all. */
	if (c->has_video_encode_family) {
		avk_log(AVK_INFO, "avk caps: video encode family=%u video_queue=%d "
			"encode_queue=%d h265=%d rgb_conversion=%d",
			c->video_encode_family, c->video_queue, c->video_encode_queue,
			c->video_encode_h265, c->video_encode_rgb);
	} else {
		avk_log(AVK_INFO, "avk caps: no video encode queue on this device");
	}
	/* M5/C5. One line, and it names the missing bit rather than just saying
	 * no -- "fp16=0" on a device that supports the format for sampling but
	 * not for blending is a completely different conversation from "fp16=0"
	 * on a device that does not have the format. */
	avk_log(AVK_INFO, "avk caps: color fp16=%d (features 0x%08x) "
		"rgb10a2=%d (features 0x%08x)",
		c->fp16_attach_blend_sample, (unsigned)c->fp16_optimal_features,
		c->rgb10a2_attach, (unsigned)c->rgb10a2_optimal_features);
	avk_log(AVK_INFO, "avk caps: max 2D image %u, max alloc %" PRIu64 " MiB, "
		"timestamp period %.2f ns, memory_budget=%d",
		c->max_image_dimension_2d,
		c->max_memory_allocation_size / (1024 * 1024),
		(double)c->timestamp_period, c->memory_budget);
}
