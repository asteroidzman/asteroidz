#define _GNU_SOURCE

#include "avk_dmabuf.h"

#include "../image/avk_upload.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── diagnostics ────────────────────────────────────────────────────────────
 *
 * Every failure path funnels through here. The rule from
 * docs/vulkan-native-architecture.md §5.2 is that a failed import must never
 * be silent, and must never leave anyone guessing what the buffer was -- so
 * the log line carries the format name, the modifier name, and every plane's
 * fd, offset and stride. That is the difference between "Electron is blank"
 * and "Electron handed us AR24 / MOD_INVALID, 1 plane, stride 15360, and GBM
 * could not import it".
 */
static void log_attribs(enum avk_log_level level,
		const struct avk_dmabuf_attributes *a, const char *what) {
	char fmt_name[64], mod_name[80];
	avk_drm_format_name(a->format, fmt_name, sizeof(fmt_name));
	avk_drm_modifier_name(a->modifier, mod_name, sizeof(mod_name));

	avk_log(level, "dmabuf %s: %" PRIi32 "x%" PRIi32 " %s modifier %s, "
		"%d plane(s)", what, a->width, a->height, fmt_name, mod_name,
		a->n_planes);
	for (int i = 0; i < a->n_planes; i++) {
		struct stat st;
		off_t size = -1;
		if (fstat(a->fd[i], &st) == 0) {
			size = st.st_size;
		}
		avk_log(level, "  plane %d: fd=%d offset=%" PRIu32 " stride=%" PRIu32
			" fd_size=%lld", i, a->fd[i], a->offset[i], a->stride[i],
			(long long)size);
	}
}

/* ── does this driver's GBM recover modifiers? ──────────────────────────────
 *
 * Allocate a buffer with an explicitly tiled modifier, export it, re-import it
 * through the legacy implicit path, and see whether the modifier survives the
 * round trip. That is exactly what rung 1 needs to do to a client buffer, so
 * doing it once at startup with our own buffer answers the question honestly
 * for whatever driver we are on.
 *
 * On Mesa/radeonsi the answer is no: the round trip returns
 * DRM_FORMAT_MOD_INVALID even though the buffer demonstrably has a tiled
 * layout. That is why this is a probe and not an assumption -- other drivers
 * may differ, and hard-coding either answer would be exactly the vendor
 * branching this design is supposed to avoid.
 */
static bool probe_modifier_recovery(struct avk_dmabuf_importer *importer) {
	if (importer->gbm == NULL) {
		return false;
	}

	const struct avk_format_caps *caps =
		avk_format_table_find(&importer->table, DRM_FORMAT_XRGB8888);
	if (caps == NULL) {
		return false;
	}

	/* Only tiled, single-plane candidates: LINEAR would round-trip trivially
	 * and prove nothing, and a multi-plane modifier is not what the legacy
	 * path handles anyway. */
	uint64_t candidates[32];
	uint32_t candidate_count = 0;
	for (uint32_t i = 0; i < caps->texture_mod_count && candidate_count < 32;
			i++) {
		const struct avk_modifier_caps *m = &caps->texture_mods[i];
		if (m->modifier != DRM_FORMAT_MOD_LINEAR && m->plane_count == 1) {
			candidates[candidate_count++] = m->modifier;
		}
	}
	if (candidate_count == 0) {
		return false;
	}

	struct gbm_bo *bo = gbm_bo_create_with_modifiers2(importer->gbm, 64, 64,
		DRM_FORMAT_XRGB8888, candidates, candidate_count,
		GBM_BO_USE_RENDERING);
	if (bo == NULL) {
		return false;
	}

	uint64_t real = gbm_bo_get_modifier(bo);
	uint32_t stride = gbm_bo_get_stride(bo);
	int fd = gbm_bo_get_fd(bo);
	bool recovered = false;

	if (fd >= 0 && real != DRM_FORMAT_MOD_INVALID) {
		struct gbm_import_fd_data data = {
			.fd = fd,
			.width = 64,
			.height = 64,
			.stride = stride,
			.format = DRM_FORMAT_XRGB8888,
		};
		struct gbm_bo *reimported = gbm_bo_import(importer->gbm,
			GBM_BO_IMPORT_FD, &data, GBM_BO_USE_RENDERING);
		if (reimported != NULL) {
			recovered = gbm_bo_get_modifier(reimported) == real;
			gbm_bo_destroy(reimported);
		}
	}

	if (fd >= 0) {
		close(fd);
	}
	gbm_bo_destroy(bo);

	if (recovered) {
		avk_log(AVK_INFO, "avk dmabuf: this driver's GBM recovers implicit "
			"modifiers -- buffers with no stated modifier import zero-copy");
	} else {
		avk_log(AVK_INFO, "avk dmabuf: this driver's GBM does not recover "
			"implicit modifiers; buffers with no stated modifier take the "
			"copy path");
	}
	return recovered;
}

/* ── importer lifetime ──────────────────────────────────────────────────── */

bool avk_dmabuf_importer_init(struct avk_dmabuf_importer *importer,
		struct avk_device *dev) {
	memset(importer, 0, sizeof(*importer));
	importer->dev = dev;

	if (!avk_format_table_init(&importer->table, dev)) {
		return false;
	}
	avk_format_table_log(&importer->table);

	if (dev->drm_fd >= 0) {
		importer->gbm = gbm_create_device(dev->drm_fd);
		if (importer->gbm == NULL) {
			/* Said once, here, rather than once per implicit buffer. Without
			 * GBM there is no recovery AND no copy path, so implicit buffers
			 * cannot be imported at all. */
			avk_log(AVK_WARN, "could not open a GBM device on the render "
				"node: buffers that arrive with an implicit modifier cannot "
				"be imported at all");
		}
	} else {
		avk_log(AVK_WARN, "importer has no DRM fd: implicit-modifier handling "
			"is unavailable");
	}

	importer->gbm_recovers_modifiers = probe_modifier_recovery(importer);

	avk_retire_init(&importer->retire);
	if (!avk_cmd_ring_init(&importer->upload_ring, dev, "avk upload")) {
		avk_format_table_finish(&importer->table);
		if (importer->gbm != NULL) {
			gbm_device_destroy(importer->gbm);
		}
		return false;
	}

	return true;
}

void avk_dmabuf_importer_collect(struct avk_dmabuf_importer *importer) {
	avk_retire_collect(&importer->retire, importer->dev);
}

void avk_dmabuf_importer_finish(struct avk_dmabuf_importer *importer) {
	if (importer->dev != NULL) {
		/* Uploads may still be in flight; teardown is the one place a wait is
		 * the right answer. */
		vkDeviceWaitIdle(importer->dev->dev);
		avk_retire_finish(&importer->retire, importer->dev);
		avk_cmd_ring_finish(&importer->upload_ring);
	}
	if (importer->gbm != NULL) {
		gbm_device_destroy(importer->gbm);
		importer->gbm = NULL;
	}
	avk_format_table_finish(&importer->table);
	importer->dev = NULL;
}

/* ── rung 1: recover the modifier ───────────────────────────────────────── */

uint64_t avk_dmabuf_recover_modifier(struct avk_dmabuf_importer *importer,
		const struct avk_dmabuf_attributes *attribs) {
	if (importer->gbm == NULL || !importer->gbm_recovers_modifiers) {
		return DRM_FORMAT_MOD_INVALID;
	}

	/*
	 * GBM_BO_IMPORT_FD, not FD_MODIFIER.
	 *
	 * The whole point is that we do not know the modifier; the FD_MODIFIER
	 * path requires us to state one, which is the question rather than the
	 * answer. The plain FD path is the legacy implicit-modifier import, and
	 * asking the resulting bo for its modifier is what makes the driver tell
	 * us the tiling it inferred from the buffer's own metadata.
	 *
	 * Single-plane only: the legacy import has no multi-plane form, and a
	 * multi-plane buffer arriving with no modifier is not something any
	 * current client produces. If one ever does, it takes the copy path
	 * rather than a wrong guess.
	 */
	if (attribs->n_planes != 1) {
		avk_log(AVK_DEBUG, "implicit-modifier recovery skipped: %d planes",
			attribs->n_planes);
		return DRM_FORMAT_MOD_INVALID;
	}

	struct gbm_import_fd_data data = {
		.fd = attribs->fd[0],
		.width = (uint32_t)attribs->width,
		.height = (uint32_t)attribs->height,
		.stride = attribs->stride[0],
		.format = attribs->format,
	};

	/* RENDERING only, and NOT SCANOUT. Measured: adding GBM_BO_USE_SCANOUT
	 * makes radeonsi refuse the import outright for a buffer that was
	 * allocated for rendering, because the usage is a constraint on the
	 * layout rather than a hint about our intentions. Asking for more than
	 * we need turns a working import into a failure. */
	struct gbm_bo *bo = gbm_bo_import(importer->gbm, GBM_BO_IMPORT_FD, &data,
		GBM_BO_USE_RENDERING);
	if (bo == NULL) {
		avk_log(AVK_DEBUG, "implicit-modifier recovery: GBM refused the "
			"import");
		return DRM_FORMAT_MOD_INVALID;
	}

	uint64_t modifier = gbm_bo_get_modifier(bo);
	gbm_bo_destroy(bo);

	if (modifier == DRM_FORMAT_MOD_INVALID) {
		avk_log(AVK_DEBUG, "implicit-modifier recovery: GBM imported the "
			"buffer but reports no modifier either");
	}
	return modifier;
}

/* ── the import itself ──────────────────────────────────────────────────── */

static bool find_memory_type(struct avk_device *dev, uint32_t type_bits,
		uint32_t *out) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(dev->phys, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if (type_bits & (1u << i)) {
			*out = i;
			return true;
		}
	}
	return false;
}

/*
 * Create and bind the VkImage for a dma-buf whose modifier is known.
 *
 * The plane-layout array is what makes this an import rather than an
 * allocation: it hands Vulkan the producer's own offsets and strides, so the
 * image describes memory that already exists rather than memory Vulkan would
 * have laid out itself.
 */
static struct avk_image *import_with_modifier(
		struct avk_dmabuf_importer *importer,
		const struct avk_dmabuf_attributes *attribs, uint64_t modifier,
		bool for_render, const struct avk_format_caps *caps) {
	struct avk_device *dev = importer->dev;
	const struct avk_drm_format *fmt = caps->format;

	const struct avk_modifier_caps *mod =
		avk_format_caps_find_modifier(caps, modifier, for_render);
	if (mod == NULL) {
		char mod_name[80];
		avk_drm_modifier_name(modifier, mod_name, sizeof(mod_name));
		avk_log(AVK_DEBUG, "modifier %s is not in the %s capability set",
			mod_name, for_render ? "render" : "texture");
		return NULL;
	}

	/* The driver's plane count for this modifier is authoritative. A buffer
	 * claiming a different number of planes is malformed, and importing it
	 * anyway reads memory that is not there. */
	if ((uint32_t)attribs->n_planes != mod->plane_count) {
		avk_log(AVK_ERROR, "dmabuf claims %d planes but this modifier has %u",
			attribs->n_planes, mod->plane_count);
		return NULL;
	}

	if ((uint32_t)attribs->width > mod->max_extent.width
			|| (uint32_t)attribs->height > mod->max_extent.height) {
		avk_log(AVK_ERROR, "dmabuf is %" PRIi32 "x%" PRIi32 ", larger than "
			"the %ux%u this format/modifier supports",
			attribs->width, attribs->height, mod->max_extent.width,
			mod->max_extent.height);
		return NULL;
	}

	/*
	 * Disjoint: planes living in different buffer objects, which needs one
	 * VkDeviceMemory per plane and VK_IMAGE_CREATE_DISJOINT_BIT.
	 *
	 * Detected by inode, not by comparing fd numbers -- a producer can hand
	 * over the same buffer as several dup()ed descriptors, which are
	 * different fds pointing at one object, and treating that as disjoint
	 * would allocate several bindings for memory that is one allocation.
	 */
	bool disjoint = false;
	if (attribs->n_planes > 1) {
		struct stat first;
		if (fstat(attribs->fd[0], &first) != 0) {
			avk_log(AVK_ERROR, "cannot fstat plane 0");
			return NULL;
		}
		for (int i = 1; i < attribs->n_planes; i++) {
			struct stat other;
			if (fstat(attribs->fd[i], &other) != 0) {
				avk_log(AVK_ERROR, "cannot fstat plane %d", i);
				return NULL;
			}
			if (other.st_ino != first.st_ino) {
				disjoint = true;
				break;
			}
		}
	}
	if (disjoint && !mod->supports_disjoint) {
		avk_log(AVK_ERROR, "buffer has planes in separate allocations but "
			"this format/modifier does not support disjoint images");
		return NULL;
	}

	struct avk_image *image = calloc(1, sizeof(*image));
	if (image == NULL) {
		avk_log(AVK_ERROR, "allocation failed");
		return NULL;
	}
	image->dev = dev;
	image->format = fmt->vk;
	image->drm_format = attribs->format;
	image->extent = (VkExtent2D){ (uint32_t)attribs->width,
		(uint32_t)attribs->height };
	image->modifier = modifier;
	image->plane_count = (uint32_t)attribs->n_planes;
	image->disjoint = disjoint;
	image->has_alpha = fmt->has_alpha;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkSubresourceLayout plane_layouts[AVK_MAX_PLANES] = {0};
	for (int i = 0; i < attribs->n_planes; i++) {
		plane_layouts[i].offset = attribs->offset[i];
		plane_layouts[i].rowPitch = attribs->stride[i];
		/* size/arrayPitch/depthPitch must be zero for an import: the driver
		 * derives them from the modifier. Setting them is a validation
		 * error, and a plausible-looking one to write by accident. */
	}

	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
		.sType =
			VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.drmFormatModifier = modifier,
		.drmFormatModifierPlaneCount = (uint32_t)attribs->n_planes,
		.pPlaneLayouts = plane_layouts,
	};
	VkExternalMemoryImageCreateInfo external_info = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext = &modifier_info,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};

	VkFormat view_formats[2] = { fmt->vk, fmt->vk_srgb };
	VkImageFormatListCreateInfo format_list = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
		.pNext = &external_info,
		.viewFormatCount = 2,
		.pViewFormats = view_formats,
	};

	VkImageCreateInfo image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = mod->srgb_mutable ? (const void *)&format_list
			: (const void *)&external_info,
		.flags = (disjoint ? VK_IMAGE_CREATE_DISJOINT_BIT : 0)
			| (mod->srgb_mutable ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0),
		.imageType = VK_IMAGE_TYPE_2D,
		.format = fmt->vk,
		.extent = { (uint32_t)attribs->width, (uint32_t)attribs->height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = for_render
			? (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
				| VK_IMAGE_USAGE_SAMPLED_BIT
				| VK_IMAGE_USAGE_TRANSFER_SRC_BIT
				| VK_IMAGE_USAGE_TRANSFER_DST_BIT)
			: (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult res = vkCreateImage(dev->dev, &image_info, NULL, &image->image);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateImage (dmabuf import)");
		free(image);
		return NULL;
	}

	/* ── memory ────────────────────────────────────────────────────────── */
	uint32_t memory_count = disjoint ? (uint32_t)attribs->n_planes : 1;
	VkBindImageMemoryInfo binds[AVK_MAX_PLANES] = {0};
	VkBindImagePlaneMemoryInfo plane_binds[AVK_MAX_PLANES] = {0};

	static const VkImageAspectFlagBits plane_aspects[AVK_MAX_PLANES] = {
		VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
		VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
	};

	for (uint32_t i = 0; i < memory_count; i++) {
		/* The fd is DUPLICATED. vkAllocateMemory with a dma-buf handle takes
		 * ownership of the descriptor it is given and closes it, so handing
		 * over the caller's fd would close a descriptor the caller still
		 * owns -- an EBADF somewhere else entirely, much later. */
		int fd = fcntl(attribs->fd[i], F_DUPFD_CLOEXEC, 0);
		if (fd < 0) {
			avk_log(AVK_ERROR, "cannot dup plane %u's fd", i);
			goto error;
		}

		VkMemoryFdPropertiesKHR fd_props = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
		};
		PFN_vkGetMemoryFdPropertiesKHR get_fd_props =
			(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev->dev,
				"vkGetMemoryFdPropertiesKHR");
		if (get_fd_props == NULL) {
			avk_log(AVK_ERROR, "vkGetMemoryFdPropertiesKHR is missing");
			close(fd);
			goto error;
		}
		res = get_fd_props(dev->dev,
			VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props);
		if (res != VK_SUCCESS) {
			avk_check(res, "vkGetMemoryFdPropertiesKHR");
			close(fd);
			goto error;
		}

		/* Intersect what the fd can be imported as with what the image
		 * requires. Using either alone picks a memory type the other side
		 * rejects. */
		VkImagePlaneMemoryRequirementsInfo plane_req = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
			.planeAspect = plane_aspects[i],
		};
		VkImageMemoryRequirementsInfo2 req_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
			.pNext = disjoint ? &plane_req : NULL,
			.image = image->image,
		};
		VkMemoryRequirements2 reqs = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
		};
		vkGetImageMemoryRequirements2(dev->dev, &req_info, &reqs);

		uint32_t type_bits =
			reqs.memoryRequirements.memoryTypeBits & fd_props.memoryTypeBits;
		uint32_t type_index = 0;
		if (type_bits == 0 || !find_memory_type(dev, type_bits, &type_index)) {
			avk_log(AVK_ERROR, "no memory type satisfies both the image "
				"(0x%x) and the imported fd (0x%x)",
				reqs.memoryRequirements.memoryTypeBits,
				fd_props.memoryTypeBits);
			close(fd);
			goto error;
		}

		VkMemoryDedicatedAllocateInfo dedicated = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
			.image = image->image,
		};
		VkImportMemoryFdInfoKHR import_info = {
			.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
			/* Dedicated only when the image owns the whole allocation.
			 * A disjoint image's planes are not each a dedicated allocation
			 * for the image as a whole. */
			.pNext = disjoint ? NULL : (const void *)&dedicated,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
			.fd = fd,
		};
		VkMemoryAllocateInfo alloc_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.pNext = &import_info,
			.allocationSize = reqs.memoryRequirements.size,
			.memoryTypeIndex = type_index,
		};

		res = vkAllocateMemory(dev->dev, &alloc_info, NULL,
			&image->memory[i]);
		if (res != VK_SUCCESS) {
			avk_check(res, "vkAllocateMemory (dmabuf import)");
			/* On failure Vulkan did NOT take the fd, so we still own it. */
			close(fd);
			goto error;
		}
		image->memory_count++;

		binds[i] = (VkBindImageMemoryInfo){
			.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
			.image = image->image,
			.memory = image->memory[i],
			.memoryOffset = 0,
		};
		if (disjoint) {
			plane_binds[i] = (VkBindImagePlaneMemoryInfo){
				.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
				.planeAspect = plane_aspects[i],
			};
			binds[i].pNext = &plane_binds[i];
		}
	}

	res = vkBindImageMemory2(dev->dev, memory_count, binds);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkBindImageMemory2");
		goto error;
	}

	avk_device_name_object(dev, VK_OBJECT_TYPE_IMAGE, (uint64_t)image->image,
		"client buffer %ux%u", image->extent.width, image->extent.height);

	return image;

error:
	avk_image_destroy(dev, image);
	return NULL;
}

/* ── rung 2: copy through a driver-detiled mapping ──────────────────────────
 *
 * The buffer's layout is unknown to US. It is not unknown to the driver that
 * allocated it: gbm_bo_map() with GBM_BO_TRANSFER_READ hands back a linear
 * view, detiling on the way if it has to. So the pixels can be read correctly
 * without anyone ever guessing at a modifier.
 *
 * This costs a read of the whole surface plus an upload, per import. It is the
 * slow path and it is meant to be -- but a slow window is a window, and the
 * alternative on this rung is a blank one.
 */

static struct avk_image *import_by_copy(struct avk_dmabuf_importer *importer,
		const struct avk_dmabuf_attributes *attribs,
		const struct avk_format_caps *caps) {
	struct avk_device *dev = importer->dev;
	const struct avk_drm_format *fmt = caps->format;

	if (importer->gbm == NULL) {
		return NULL;
	}
	if (attribs->n_planes != 1) {
		avk_log(AVK_DEBUG, "copy path: only single-plane buffers, got %d",
			attribs->n_planes);
		return NULL;
	}

	if (avk_format_bytes_per_pixel(fmt->vk) == 0) {
		avk_log(AVK_DEBUG, "copy path: no single bytes-per-pixel for this "
			"format");
		return NULL;
	}

	struct gbm_import_fd_data data = {
		.fd = attribs->fd[0],
		.width = (uint32_t)attribs->width,
		.height = (uint32_t)attribs->height,
		.stride = attribs->stride[0],
		.format = attribs->format,
	};
	struct gbm_bo *bo = gbm_bo_import(importer->gbm, GBM_BO_IMPORT_FD, &data,
		GBM_BO_USE_RENDERING);
	if (bo == NULL) {
		avk_log(AVK_DEBUG, "copy path: GBM would not import the buffer");
		return NULL;
	}

	uint32_t map_stride = 0;
	void *map_data = NULL;
	void *pixels = gbm_bo_map(bo, 0, 0, (uint32_t)attribs->width,
		(uint32_t)attribs->height, GBM_BO_TRANSFER_READ, &map_stride,
		&map_data);
	if (pixels == NULL) {
		avk_log(AVK_DEBUG, "copy path: gbm_bo_map failed");
		gbm_bo_destroy(bo);
		return NULL;
	}

	struct avk_upload *staging = NULL;

	/* The destination and the copy itself are avk_upload's job -- the same
	 * code the SHM client path uses, so there is one implementation of
	 * "CPU pixels onto the GPU" rather than two that drift. */
	struct avk_image *image = avk_upload_image_create(dev, attribs->format,
		(uint32_t)attribs->width, (uint32_t)attribs->height,
		AVK_IMAGE_DMABUF_COPIED);
	if (image == NULL) {
		goto out;
	}

	/* One-shot staging: this bo is about to be unmapped and destroyed, so the
	 * buffer is dead as soon as the GPU has read it. Retired, not freed --
	 * the copy has not necessarily happened yet. */
	staging = calloc(1, sizeof(*staging));
	if (staging == NULL) {
		goto out;
	}
	uint64_t timeline = avk_upload_image_write(dev, &importer->upload_ring,
		staging, image, pixels, map_stride, (uint32_t)attribs->height, NULL, 0);
	if (timeline == 0) {
		goto out;
	}
	avk_retire_push(&importer->retire, dev, timeline, avk_upload_retire,
		staging);
	staging = NULL;

	gbm_bo_unmap(bo, map_data);
	gbm_bo_destroy(bo);
	return image;

out:
	if (staging != NULL) {
		avk_upload_retire(dev, staging);
	}
	if (image != NULL) {
		avk_image_destroy(dev, image);
	}
	gbm_bo_unmap(bo, map_data);
	gbm_bo_destroy(bo);
	return NULL;
}

struct avk_image *avk_dmabuf_import(struct avk_dmabuf_importer *importer,
		const struct avk_dmabuf_attributes *attribs, bool for_render) {
	if (attribs->n_planes <= 0 || attribs->n_planes > AVK_MAX_PLANES) {
		avk_log(AVK_ERROR, "dmabuf has %d planes, which is not a number of "
			"planes a buffer can have", attribs->n_planes);
		importer->imports_failed++;
		return NULL;
	}

	const struct avk_format_caps *caps =
		avk_format_table_find(&importer->table, attribs->format);
	if (caps == NULL) {
		char fmt_name[64];
		avk_drm_format_name(attribs->format, fmt_name, sizeof(fmt_name));
		avk_log(AVK_ERROR, "cannot import %s: this device advertises no "
			"importable modifiers for it", fmt_name);
		log_attribs(AVK_ERROR, attribs, "rejected");
		importer->imports_failed++;
		return NULL;
	}

	/* ── rung 0: the modifier was stated ───────────────────────────────── */
	if (attribs->modifier != DRM_FORMAT_MOD_INVALID) {
		struct avk_image *image = import_with_modifier(importer, attribs,
			attribs->modifier, for_render, caps);
		if (image != NULL) {
			image->origin = AVK_IMAGE_DMABUF_EXPLICIT;
			/*
			 * A buffer we will SAMPLE already has pixels in it, put there by an
			 * owner outside this device, so its first barrier must come from
			 * GENERAL -- UNDEFINED tells the driver the contents may be
			 * discarded.
			 *
			 * A buffer we will RENDER INTO has no contents worth keeping, and
			 * claiming GENERAL for it is actively wrong: the driver never gets
			 * the UNDEFINED -> layout transition in which it initialises an
			 * image's compression metadata. Leave it UNDEFINED and let the
			 * first frame do that properly.
			 */
			if (!for_render) {
				image->layout = VK_IMAGE_LAYOUT_GENERAL;
			}
			importer->imports_explicit++;
			return image;
		}
		avk_log(AVK_ERROR, "import failed for a buffer with an explicit "
			"modifier -- there is no fallback for this, because the tiling "
			"was known and still could not be used");
		log_attribs(AVK_ERROR, attribs, "rejected");
		importer->imports_failed++;
		return NULL;
	}

	/* ── rung 1: recover it ────────────────────────────────────────────── */
	avk_log(AVK_DEBUG, "buffer arrived with an implicit modifier; attempting "
		"recovery");
	uint64_t recovered = avk_dmabuf_recover_modifier(importer, attribs);
	if (recovered != DRM_FORMAT_MOD_INVALID) {
		struct avk_image *image = import_with_modifier(importer, attribs,
			recovered, for_render, caps);
		if (image != NULL) {
			char mod_name[80];
			avk_drm_modifier_name(recovered, mod_name, sizeof(mod_name));
			avk_log(AVK_DEBUG, "recovered implicit modifier as %s; imported "
				"zero-copy", mod_name);
			image->origin = AVK_IMAGE_DMABUF_RECOVERED;
			if (!for_render) {
				image->layout = VK_IMAGE_LAYOUT_GENERAL;
			}
			importer->imports_recovered++;
			return image;
		}
	}

	/* ── rung 2: copy ──────────────────────────────────────────────────── */
	struct avk_image *copied = import_by_copy(importer, attribs, caps);
	if (copied != NULL) {
		avk_log(AVK_DEBUG, "imported an implicit-modifier buffer by copying "
			"through a driver-detiled mapping");
		importer->imports_copied++;
		return copied;
	}

	/* ── rung 3: say so, loudly and completely ─────────────────────────── */
	avk_log(AVK_ERROR, "cannot import this buffer by any route");
	log_attribs(AVK_ERROR, attribs, "rejected");
	avk_log(AVK_ERROR, "  device: %s (%s)", importer->dev->caps.device_name,
		importer->dev->caps.driver_name);
	avk_log(AVK_ERROR, "  tried: explicit modifier (buffer had none); "
		"modifier recovery (%s); copy through gbm_bo_map (%s)",
		importer->gbm == NULL ? "no GBM device"
			: (importer->gbm_recovers_modifiers ? "failed"
				: "unsupported by this driver"),
		importer->gbm == NULL ? "no GBM device" : "failed");
	importer->imports_failed++;
	return NULL;
}

void avk_dmabuf_importer_log_stats(
		const struct avk_dmabuf_importer *importer) {
	avk_log(AVK_INFO, "avk dmabuf: %" PRIu64 " explicit, %" PRIu64
		" recovered-implicit, %" PRIu64 " copied, %" PRIu64 " failed",
		importer->imports_explicit, importer->imports_recovered,
		importer->imports_copied, importer->imports_failed);
}
