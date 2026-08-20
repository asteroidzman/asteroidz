#define _GNU_SOURCE
#include "render/vulkan/encode/avk_encode.h"

#include <stdlib.h>
#include <string.h>

#include "render/vulkan/device/avk_device.h"
#include "render/vulkan/device/avk_instance.h"
#include "render/vulkan/avk.h"

/* Entry points. Every one of these comes from a device extension, so
 * vkGetDeviceProcAddr is the only way to reach them and a NULL check is the
 * only way to know the loader agreed the extension was enabled. */
struct encode_api {
	PFN_vkCreateVideoSessionKHR create_session;
	PFN_vkDestroyVideoSessionKHR destroy_session;
	PFN_vkGetVideoSessionMemoryRequirementsKHR get_session_memory;
	PFN_vkBindVideoSessionMemoryKHR bind_session_memory;
	PFN_vkCreateVideoSessionParametersKHR create_params;
	PFN_vkDestroyVideoSessionParametersKHR destroy_params;
};

static bool load_api(struct avk_device *dev, struct encode_api *api) {
	VkDevice d = dev->dev;
	api->create_session = (PFN_vkCreateVideoSessionKHR)
		vkGetDeviceProcAddr(d, "vkCreateVideoSessionKHR");
	api->destroy_session = (PFN_vkDestroyVideoSessionKHR)
		vkGetDeviceProcAddr(d, "vkDestroyVideoSessionKHR");
	api->get_session_memory = (PFN_vkGetVideoSessionMemoryRequirementsKHR)
		vkGetDeviceProcAddr(d, "vkGetVideoSessionMemoryRequirementsKHR");
	api->bind_session_memory = (PFN_vkBindVideoSessionMemoryKHR)
		vkGetDeviceProcAddr(d, "vkBindVideoSessionMemoryKHR");
	api->create_params = (PFN_vkCreateVideoSessionParametersKHR)
		vkGetDeviceProcAddr(d, "vkCreateVideoSessionParametersKHR");
	api->destroy_params = (PFN_vkDestroyVideoSessionParametersKHR)
		vkGetDeviceProcAddr(d, "vkDestroyVideoSessionParametersKHR");
	return api->create_session && api->destroy_session
		&& api->get_session_memory && api->bind_session_memory
		&& api->create_params && api->destroy_params;
}

static uint32_t align_up(uint32_t v, uint32_t a) {
	return a == 0 ? v : ((v + a - 1) / a) * a;
}

/*
 * Build the profile once, in one place.
 *
 * The RGB conversion struct sits in the profile's pNext beside the codec's
 * own, and its presence changes what every subsequent query answers -- so a
 * caller that rebuilds this by hand and omits it gets a session that creates
 * cleanly and then refuses A2R10G10B10 pictures.
 */
static void build_profile(struct avk_encoder *enc) {
	enc->h265_profile = (VkVideoEncodeH265ProfileInfoKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR,
		.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10,
	};
	enc->rgb_profile = (VkVideoEncodeProfileRgbConversionInfoVALVE){
		.sType =
			VK_STRUCTURE_TYPE_VIDEO_ENCODE_PROFILE_RGB_CONVERSION_INFO_VALVE,
		.pNext = &enc->h265_profile,
		.performEncodeRgbConversion = VK_TRUE,
	};
	enc->profile = (VkVideoProfileInfoKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
		.pNext = &enc->rgb_profile,
		.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR,
		.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
		.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR,
		.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR,
	};
	enc->profile_list = (VkVideoProfileListInfoKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
		.profileCount = 1,
		.pProfiles = &enc->profile,
	};
}

static bool query_caps(struct avk_encoder *enc) {
	struct avk_device *dev = enc->dev;
	PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR get_caps =
		(PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)vkGetInstanceProcAddr(
			dev->instance->instance,
			"vkGetPhysicalDeviceVideoCapabilitiesKHR");
	if (get_caps == NULL) {
		avk_log(AVK_ERROR, "encode: vkGetPhysicalDeviceVideoCapabilitiesKHR "
			"is missing");
		return false;
	}

	/* The codec-specific struct is NOT optional here. RADV segfaults rather
	 * than skipping it when it is absent from the chain -- not an error
	 * return, a crash -- which is worth a line of comment because the
	 * signature gives no hint that the pNext is mandatory. */
	enc->h265_caps = (VkVideoEncodeH265CapabilitiesKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR,
	};
	/* The conversion's own limits: which colour models, ranges and chroma
	 * sitings the driver will perform. The session below has to name one of
	 * each, so they are queried here rather than assumed. */
	enc->rgb_caps = (VkVideoEncodeRgbConversionCapabilitiesVALVE){
		.sType =
			VK_STRUCTURE_TYPE_VIDEO_ENCODE_RGB_CONVERSION_CAPABILITIES_VALVE,
		.pNext = &enc->h265_caps,
	};
	enc->encode_caps = (VkVideoEncodeCapabilitiesKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR,
		.pNext = &enc->rgb_caps,
	};
	enc->caps = (VkVideoCapabilitiesKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
		.pNext = &enc->encode_caps,
	};
	VkResult r = get_caps(dev->phys, &enc->profile, &enc->caps);
	if (r != VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: the driver rejects H.265 Main 10 with RGB "
			"conversion (VkResult %d)", (int)r);
		return false;
	}
	return true;
}

/* Which image format the driver wants for a given role under this profile. */
static bool pick_format(struct avk_encoder *enc, VkImageUsageFlags usage,
		VkFormat *out, const char *role) {
	struct avk_device *dev = enc->dev;
	PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR get_fmts =
		(PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR)vkGetInstanceProcAddr(
			dev->instance->instance,
			"vkGetPhysicalDeviceVideoFormatPropertiesKHR");
	if (get_fmts == NULL) {
		return false;
	}
	VkPhysicalDeviceVideoFormatInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
		.pNext = &enc->profile_list,
		.imageUsage = usage,
	};
	uint32_t n = 0;
	if (get_fmts(dev->phys, &info, &n, NULL) != VK_SUCCESS || n == 0) {
		avk_log(AVK_ERROR, "encode: no %s format under this profile", role);
		return false;
	}
	VkVideoFormatPropertiesKHR *props = calloc(n, sizeof(*props));
	if (props == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		props[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
	}
	if (get_fmts(dev->phys, &info, &n, props) != VK_SUCCESS) {
		free(props);
		return false;
	}
	/* Prefer the one AVK already renders into, so the source image can be
	 * handed over rather than converted. Otherwise take the first offered
	 * and let the caller find out it has work to do. */
	VkFormat chosen = props[0].format;
	for (uint32_t i = 0; i < n; i++) {
		if (props[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32
				&& props[i].imageTiling == VK_IMAGE_TILING_OPTIMAL) {
			chosen = props[i].format;
			break;
		}
	}
	free(props);
	*out = chosen;

	/* A format is only an encode input UNDER A PROFILE. Asked plainly,
	 * A2R10G10B10 does not carry VIDEO_ENCODE_INPUT and the validation layer
	 * says so at vkCreateImageView; asked with the profile list chained, the
	 * answer can differ. Logged rather than assumed, because which of those
	 * two is true decides whether a complaint about the view is a real defect
	 * or a layer that cannot see the profile. */
	VkVideoProfileListInfoKHR list_for_fmt = enc->profile_list;
	VkFormatProperties3 fmt3 = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
		.pNext = &list_for_fmt,
	};
	VkFormatProperties2 fmt2 = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &fmt3,
	};
	vkGetPhysicalDeviceFormatProperties2(dev->phys, chosen, &fmt2);
	avk_log(AVK_INFO, "encode: %s format %d optimal features 0x%llx "
		"(encode_input=%d)", role, (int)chosen,
		(unsigned long long)fmt3.optimalTilingFeatures,
		(fmt3.optimalTilingFeatures
			& VK_FORMAT_FEATURE_2_VIDEO_ENCODE_INPUT_BIT_KHR) ? 1 : 0);
	return true;
}

static bool bind_session_memory(struct avk_encoder *enc,
		const struct encode_api *api) {
	struct avk_device *dev = enc->dev;
	uint32_t count = 0;
	if (api->get_session_memory(dev->dev, enc->session, &count, NULL)
			!= VK_SUCCESS) {
		return false;
	}
	if (count == 0) {
		return true; /* a session that needs no memory of its own is legal */
	}
	VkVideoSessionMemoryRequirementsKHR *reqs = calloc(count, sizeof(*reqs));
	VkBindVideoSessionMemoryInfoKHR *binds = calloc(count, sizeof(*binds));
	enc->session_memory = calloc(count, sizeof(*enc->session_memory));
	if (reqs == NULL || binds == NULL || enc->session_memory == NULL) {
		free(reqs);
		free(binds);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		reqs[i].sType =
			VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
	}
	if (api->get_session_memory(dev->dev, enc->session, &count, reqs)
			!= VK_SUCCESS) {
		free(reqs);
		free(binds);
		return false;
	}

	VkPhysicalDeviceMemoryProperties mem_props;
	vkGetPhysicalDeviceMemoryProperties(dev->phys, &mem_props);

	bool ok = true;
	for (uint32_t i = 0; i < count && ok; i++) {
		uint32_t type = UINT32_MAX;
		for (uint32_t t = 0; t < mem_props.memoryTypeCount; t++) {
			if (reqs[i].memoryRequirements.memoryTypeBits & (1u << t)) {
				type = t;
				break;
			}
		}
		if (type == UINT32_MAX) {
			avk_log(AVK_ERROR, "encode: no memory type for session bind %u",
				reqs[i].memoryBindIndex);
			ok = false;
			break;
		}
		VkMemoryAllocateInfo alloc = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = reqs[i].memoryRequirements.size,
			.memoryTypeIndex = type,
		};
		if (vkAllocateMemory(dev->dev, &alloc, NULL,
				&enc->session_memory[i]) != VK_SUCCESS) {
			avk_log(AVK_ERROR, "encode: cannot allocate %llu bytes for the "
				"video session",
				(unsigned long long)reqs[i].memoryRequirements.size);
			ok = false;
			break;
		}
		enc->session_memory_count = i + 1;
		binds[i] = (VkBindVideoSessionMemoryInfoKHR){
			.sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR,
			.memoryBindIndex = reqs[i].memoryBindIndex,
			.memory = enc->session_memory[i],
			.memoryOffset = 0,
			.memorySize = reqs[i].memoryRequirements.size,
		};
	}
	if (ok && api->bind_session_memory(dev->dev, enc->session, count, binds)
			!= VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: vkBindVideoSessionMemoryKHR failed");
		ok = false;
	}
	free(reqs);
	free(binds);
	return ok;
}

/*
 * The parameter sets.
 *
 * H.265 carries its geometry in a VPS/SPS/PPS triple that the decoder reads
 * before any picture, and the encoder needs them at session-parameters time
 * rather than per-frame. The values below are the smallest legal set that
 * describes "one 10-bit 4:2:0 intra picture at this size": no temporal layers,
 * no tiles, one slice, no reference reordering.
 */
static bool create_parameters(struct avk_encoder *enc,
		const struct encode_api *api) {
	StdVideoH265ProfileTierLevel ptl = {
		.flags = {
			.general_tier_flag = 0,
			.general_progressive_source_flag = 1,
			.general_frame_only_constraint_flag = 1,
		},
		.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10,
		.general_level_idc = enc->h265_caps.maxLevelIdc,
	};
	StdVideoH265DecPicBufMgr dpb_mgr = {
		.max_latency_increase_plus1 = {1},
		.max_dec_pic_buffering_minus1 = {0},
		.max_num_reorder_pics = {0},
	};
	StdVideoH265VideoParameterSet vps = {
		.flags = {
			.vps_temporal_id_nesting_flag = 1,
			.vps_sub_layer_ordering_info_present_flag = 1,
		},
		.vps_video_parameter_set_id = 0,
		.vps_max_sub_layers_minus1 = 0,
		.pDecPicBufMgr = &dpb_mgr,
		.pProfileTierLevel = &ptl,
	};
	StdVideoH265SequenceParameterSet sps = {
		.flags = {
			.sps_temporal_id_nesting_flag = 1,
			.sps_sub_layer_ordering_info_present_flag = 1,
			.amp_enabled_flag = 1,
			.sample_adaptive_offset_enabled_flag = 1,
			.sps_temporal_mvp_enabled_flag = 0,
			.strong_intra_smoothing_enabled_flag = 1,
		},
		.chroma_format_idc = STD_VIDEO_H265_CHROMA_FORMAT_IDC_420,
		.pic_width_in_luma_samples = enc->coded_width,
		.pic_height_in_luma_samples = enc->coded_height,
		.sps_video_parameter_set_id = 0,
		.sps_max_sub_layers_minus1 = 0,
		.sps_seq_parameter_set_id = 0,
		/* 10-bit: 8 + 2. */
		.bit_depth_luma_minus8 = 2,
		.bit_depth_chroma_minus8 = 2,
		.log2_max_pic_order_cnt_lsb_minus4 = 4,
		/* 64x64 CTBs: min 8 (3), diff 3 -> max 64. */
		.log2_min_luma_coding_block_size_minus3 = 0,
		.log2_diff_max_min_luma_coding_block_size = 3,
		.log2_min_luma_transform_block_size_minus2 = 0,
		.log2_diff_max_min_luma_transform_block_size = 3,
		.max_transform_hierarchy_depth_inter = 0,
		.max_transform_hierarchy_depth_intra = 0,
		.pProfileTierLevel = &ptl,
		.pDecPicBufMgr = &dpb_mgr,
	};
	StdVideoH265PictureParameterSet pps = {
		.flags = {
			.sign_data_hiding_enabled_flag = 0,
			.cu_qp_delta_enabled_flag = 1,
			.entropy_coding_sync_enabled_flag = 0,
			.uniform_spacing_flag = 1,
		},
		.pps_pic_parameter_set_id = 0,
		.pps_seq_parameter_set_id = 0,
		.sps_video_parameter_set_id = 0,
		.init_qp_minus26 = 0,
		.diff_cu_qp_delta_depth = 0,
	};

	VkVideoEncodeH265SessionParametersAddInfoKHR add = {
		.sType =
			VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR,
		.stdVPSCount = 1, .pStdVPSs = &vps,
		.stdSPSCount = 1, .pStdSPSs = &sps,
		.stdPPSCount = 1, .pStdPPSs = &pps,
	};
	VkVideoEncodeH265SessionParametersCreateInfoKHR h265_create = {
		.sType =
			VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR,
		.maxStdVPSCount = 1,
		.maxStdSPSCount = 1,
		.maxStdPPSCount = 1,
		.pParametersAddInfo = &add,
	};
	VkVideoSessionParametersCreateInfoKHR create = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
		.pNext = &h265_create,
		.videoSession = enc->session,
	};
	VkResult r = api->create_params(enc->dev->dev, &create, NULL,
		&enc->params);
	if (r != VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: vkCreateVideoSessionParametersKHR failed "
			"(VkResult %d)", (int)r);
		return false;
	}
	return true;
}


/* ── the resources one encode needs ───────────────────────────────────── */

static bool find_memory(struct avk_device *dev, uint32_t bits,
		VkMemoryPropertyFlags want, uint32_t *out) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(dev->phys, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((bits & (1u << i))
				&& (props.memoryTypes[i].propertyFlags & want) == want) {
			*out = i;
			return true;
		}
	}
	return false;
}

const VkVideoProfileListInfoKHR *avk_encoder_profile_list(
		const struct avk_encoder *enc) {
	return &enc->profile_list;
}

/*
 * The DPB.
 *
 * An intra-only still references nothing, but the encoder still writes its
 * reconstructed picture somewhere, and vkCmdEncodeVideoKHR wants a setup slot
 * to write it into. Declining to provide one does not save the allocation --
 * it fails the command.
 */
static bool create_dpb(struct avk_encoder *enc) {
	struct avk_device *dev = enc->dev;
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &enc->profile_list,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = enc->dpb_format,
		.extent = {enc->coded_width, enc->coded_height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(dev->dev, &info, NULL, &enc->dpb_image) != VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: cannot create the DPB image");
		return false;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev->dev, enc->dpb_image, &req);
	uint32_t type = 0;
	if (!find_memory(dev, req.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		return false;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	};
	if (vkAllocateMemory(dev->dev, &alloc, NULL, &enc->dpb_memory)
			!= VK_SUCCESS) {
		return false;
	}
	if (vkBindImageMemory(dev->dev, enc->dpb_image, enc->dpb_memory, 0)
			!= VK_SUCCESS) {
		return false;
	}
	VkImageViewCreateInfo view = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = enc->dpb_image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = enc->dpb_format,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	return vkCreateImageView(dev->dev, &view, NULL, &enc->dpb_view)
		== VK_SUCCESS;
}

static bool create_bitstream(struct avk_encoder *enc) {
	struct avk_device *dev = enc->dev;
	/* Generous, and host-visible so the result can be read without a second
	 * copy. A 4K intra picture at a sane QP is a few megabytes; 64 is room to
	 * be wrong by an order of magnitude and still not truncate. */
	enc->bitstream_size = 64u * 1024u * 1024u;
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = &enc->profile_list,
		.size = enc->bitstream_size,
		.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (vkCreateBuffer(dev->dev, &info, NULL, &enc->bitstream) != VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: cannot create the bitstream buffer");
		return false;
	}
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev->dev, enc->bitstream, &req);
	uint32_t type = 0;
	if (!find_memory(dev, req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
			| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type)) {
		avk_log(AVK_ERROR, "encode: no host-visible memory for the bitstream");
		return false;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = type,
	};
	if (vkAllocateMemory(dev->dev, &alloc, NULL, &enc->bitstream_memory)
			!= VK_SUCCESS) {
		return false;
	}
	return vkBindBufferMemory(dev->dev, enc->bitstream,
		enc->bitstream_memory, 0) == VK_SUCCESS;
}

/*
 * The feedback query.
 *
 * An encoder does not report how many bytes it wrote through the API that
 * launched it; the count comes back through a query of its own, and without
 * one the bitstream buffer holds a correct picture of unknown length -- which
 * is the same as holding nothing.
 */
static bool create_feedback(struct avk_encoder *enc) {
	/* The query pool takes a profile, but its pNext chain admits only a
	 * documented list of structures and VkVideoEncodeProfileRgbConversionInfoVALVE
	 * is not on it -- so this is the profile with the conversion struct
	 * unhooked, not enc->profile. The codec and bit depth are what the query
	 * pool actually needs to know. */
	VkVideoEncodeH265ProfileInfoKHR query_h265 = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR,
		.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10,
	};
	VkVideoProfileInfoKHR query_profile = enc->profile;
	query_profile.pNext = &query_h265;
	VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedback = {
		.sType =
			VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR,
		.pNext = &query_profile,
		.encodeFeedbackFlags =
			VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR
			| VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR,
	};
	VkQueryPoolCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.pNext = &feedback,
		.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR,
		.queryCount = 1,
	};
	if (vkCreateQueryPool(enc->dev->dev, &info, NULL, &enc->feedback)
			!= VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: cannot create the encode feedback query");
		return false;
	}
	return true;
}

static bool create_commands(struct avk_encoder *enc) {
	struct avk_device *dev = enc->dev;
	VkCommandPoolCreateInfo pool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = dev->caps.video_encode_family,
	};
	if (vkCreateCommandPool(dev->dev, &pool, NULL, &enc->cmd_pool)
			!= VK_SUCCESS) {
		return false;
	}
	VkCommandBufferAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = enc->cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (vkAllocateCommandBuffers(dev->dev, &alloc, &enc->cmd) != VK_SUCCESS) {
		return false;
	}
	VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	return vkCreateFence(dev->dev, &fence, NULL, &enc->fence) == VK_SUCCESS;
}

struct avk_encoder *avk_encoder_create(struct avk_device *dev,
		uint32_t width, uint32_t height) {
	if (dev == NULL || !dev->has_encode_queue) {
		avk_log(AVK_ERROR, "encode: this device has no video encode queue");
		return NULL;
	}
	struct encode_api api;
	if (!load_api(dev, &api)) {
		avk_log(AVK_ERROR, "encode: the video encode entry points are missing "
			"even though the extensions were enabled");
		return NULL;
	}

	struct avk_encoder *enc = calloc(1, sizeof(*enc));
	if (enc == NULL) {
		return NULL;
	}
	enc->dev = dev;
	enc->width = width;
	enc->height = height;

	build_profile(enc);
	if (!query_caps(enc)) {
		goto fail;
	}

	/* The coded size is the source size rounded up to the encoder's own
	 * granularity. Getting this wrong does not fail -- it encodes, and the
	 * edge of the picture is whatever was in memory past the image. */
	enc->coded_width = align_up(width, enc->caps.pictureAccessGranularity.width);
	enc->coded_height =
		align_up(height, enc->caps.pictureAccessGranularity.height);
	if (enc->coded_width > enc->caps.maxCodedExtent.width
			|| enc->coded_height > enc->caps.maxCodedExtent.height) {
		avk_log(AVK_ERROR, "encode: %ux%u (coded %ux%u) exceeds the codec's "
			"maximum %ux%u", width, height, enc->coded_width,
			enc->coded_height, enc->caps.maxCodedExtent.width,
			enc->caps.maxCodedExtent.height);
		goto fail;
	}

	if (!pick_format(enc, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR,
			&enc->src_format, "encode source")) {
		goto fail;
	}
	if (!pick_format(enc, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
			&enc->dpb_format, "DPB")) {
		goto fail;
	}

	/*
	 * Having asked the profile to convert RGB, the session must say HOW --
	 * and RADV segfaults inside vkCreateVideoSessionKHR rather than returning
	 * an error when this struct is absent, which is the second time this
	 * extension's structs have turned out to be load-bearing rather than
	 * optional (the first was the codec capabilities chain).
	 *
	 * BT.2020 because the whole point is HDR10, whose matrix coefficients are
	 * BT.2020 non-constant luminance. Full range because AVK's composited
	 * image is full-range RGB and narrowing it here would crush the ends of
	 * the scale for nothing.
	 */
	if (!(enc->rgb_caps.rgbModels
			& VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_YCBCR_2020_BIT_VALVE)) {
		avk_log(AVK_ERROR, "encode: the driver will not convert RGB to "
			"BT.2020, which is what HDR10 is");
		goto fail;
	}
	VkVideoEncodeSessionRgbConversionCreateInfoVALVE rgb_session = {
		.sType =
			VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_RGB_CONVERSION_CREATE_INFO_VALVE,
		.rgbModel = VK_VIDEO_ENCODE_RGB_MODEL_CONVERSION_YCBCR_2020_BIT_VALVE,
		.rgbRange =
			(enc->rgb_caps.rgbRanges
				& VK_VIDEO_ENCODE_RGB_RANGE_COMPRESSION_FULL_RANGE_BIT_VALVE)
			? VK_VIDEO_ENCODE_RGB_RANGE_COMPRESSION_FULL_RANGE_BIT_VALVE
			: VK_VIDEO_ENCODE_RGB_RANGE_COMPRESSION_NARROW_RANGE_BIT_VALVE,
		.xChromaOffset =
			(enc->rgb_caps.xChromaOffsets
				& VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_COSITED_EVEN_BIT_VALVE)
			? VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_COSITED_EVEN_BIT_VALVE
			: VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_MIDPOINT_BIT_VALVE,
		.yChromaOffset =
			(enc->rgb_caps.yChromaOffsets
				& VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_MIDPOINT_BIT_VALVE)
			? VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_MIDPOINT_BIT_VALVE
			: VK_VIDEO_ENCODE_RGB_CHROMA_OFFSET_COSITED_EVEN_BIT_VALVE,
	};
	VkVideoSessionCreateInfoKHR session_info = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
		.pNext = &rgb_session,
		.queueFamilyIndex = dev->caps.video_encode_family,
		.pVideoProfile = &enc->profile,
		.pictureFormat = enc->src_format,
		.maxCodedExtent = (VkExtent2D){enc->coded_width, enc->coded_height},
		.referencePictureFormat = enc->dpb_format,
		/* One slot and one reference: a still needs the reconstructed
		 * picture the encoder writes and nothing beyond it. */
		.maxDpbSlots = 1,
		.maxActiveReferencePictures = 1,
		.pStdHeaderVersion = &enc->caps.stdHeaderVersion,
	};
	VkResult r = api.create_session(dev->dev, &session_info, NULL,
		&enc->session);
	if (r != VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: vkCreateVideoSessionKHR failed "
			"(VkResult %d)", (int)r);
		goto fail;
	}
	if (!bind_session_memory(enc, &api)) {
		goto fail;
	}
	if (!create_parameters(enc, &api)) {
		goto fail;
	}
	if (!create_dpb(enc) || !create_bitstream(enc) || !create_feedback(enc)
			|| !create_commands(enc)) {
		goto fail;
	}

	avk_log(AVK_INFO, "encode: H.265 Main 10 session %ux%u (coded %ux%u), "
		"source format %d, DPB format %d, %u memory bind(s)",
		enc->width, enc->height, enc->coded_width, enc->coded_height,
		(int)enc->src_format, (int)enc->dpb_format,
		enc->session_memory_count);
	return enc;

fail:
	avk_encoder_destroy(enc);
	return NULL;
}

void avk_encoder_destroy(struct avk_encoder *enc) {
	if (enc == NULL) {
		return;
	}
	struct avk_device *dev = enc->dev;
	struct encode_api api;
	if (dev != NULL && dev->dev != VK_NULL_HANDLE && load_api(dev, &api)) {
		if (enc->params != VK_NULL_HANDLE) {
			api.destroy_params(dev->dev, enc->params, NULL);
		}
		if (enc->session != VK_NULL_HANDLE) {
			api.destroy_session(dev->dev, enc->session, NULL);
		}
		for (uint32_t i = 0; i < enc->session_memory_count; i++) {
			if (enc->session_memory[i] != VK_NULL_HANDLE) {
				vkFreeMemory(dev->dev, enc->session_memory[i], NULL);
			}
		}
		if (enc->fence != VK_NULL_HANDLE) {
			vkDestroyFence(dev->dev, enc->fence, NULL);
		}
		if (enc->cmd_pool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(dev->dev, enc->cmd_pool, NULL);
		}
		if (enc->feedback != VK_NULL_HANDLE) {
			vkDestroyQueryPool(dev->dev, enc->feedback, NULL);
		}
		if (enc->bitstream != VK_NULL_HANDLE) {
			vkDestroyBuffer(dev->dev, enc->bitstream, NULL);
		}
		if (enc->bitstream_memory != VK_NULL_HANDLE) {
			vkFreeMemory(dev->dev, enc->bitstream_memory, NULL);
		}
		if (enc->dpb_view != VK_NULL_HANDLE) {
			vkDestroyImageView(dev->dev, enc->dpb_view, NULL);
		}
		if (enc->dpb_image != VK_NULL_HANDLE) {
			vkDestroyImage(dev->dev, enc->dpb_image, NULL);
		}
		if (enc->dpb_memory != VK_NULL_HANDLE) {
			vkFreeMemory(dev->dev, enc->dpb_memory, NULL);
		}
	}
	free(enc->session_memory);
	free(enc);
}

/* ── encoding one picture ──────────────────────────────────────────────── */

struct encode_cmd_api {
	PFN_vkCmdBeginVideoCodingKHR begin;
	PFN_vkCmdEndVideoCodingKHR end;
	PFN_vkCmdControlVideoCodingKHR control;
	PFN_vkCmdEncodeVideoKHR encode;
	PFN_vkGetEncodedVideoSessionParametersKHR get_params;
};

static bool load_cmd_api(struct avk_device *dev, struct encode_cmd_api *a) {
	VkDevice d = dev->dev;
	a->begin = (PFN_vkCmdBeginVideoCodingKHR)
		vkGetDeviceProcAddr(d, "vkCmdBeginVideoCodingKHR");
	a->end = (PFN_vkCmdEndVideoCodingKHR)
		vkGetDeviceProcAddr(d, "vkCmdEndVideoCodingKHR");
	a->control = (PFN_vkCmdControlVideoCodingKHR)
		vkGetDeviceProcAddr(d, "vkCmdControlVideoCodingKHR");
	a->encode = (PFN_vkCmdEncodeVideoKHR)
		vkGetDeviceProcAddr(d, "vkCmdEncodeVideoKHR");
	a->get_params = (PFN_vkGetEncodedVideoSessionParametersKHR)
		vkGetDeviceProcAddr(d, "vkGetEncodedVideoSessionParametersKHR");
	return a->begin && a->end && a->control && a->encode && a->get_params;
}

/*
 * The parameter sets, as bytes.
 *
 * A coded picture on its own decodes to nothing: the VPS/SPS/PPS that describe
 * its geometry are carried separately and have to be prepended. The driver
 * emits them for us rather than us assembling the NAL units by hand, which is
 * the difference between this being a few lines and being a bitstream writer.
 */
static uint8_t *encoded_parameters(struct avk_encoder *enc,
		const struct encode_cmd_api *api, size_t *out_len) {
	VkVideoEncodeH265SessionParametersGetInfoKHR h265_get = {
		.sType =
			VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_KHR,
		.writeStdVPS = VK_TRUE,
		.writeStdSPS = VK_TRUE,
		.writeStdPPS = VK_TRUE,
	};
	VkVideoEncodeSessionParametersGetInfoKHR get = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
		.pNext = &h265_get,
		.videoSessionParameters = enc->params,
	};
	VkVideoEncodeSessionParametersFeedbackInfoKHR feedback = {
		.sType =
			VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,
	};
	size_t len = 0;
	if (api->get_params(enc->dev->dev, &get, &feedback, &len, NULL)
			!= VK_SUCCESS || len == 0) {
		avk_log(AVK_ERROR, "encode: the driver reports no parameter set bytes");
		return NULL;
	}
	uint8_t *buf = malloc(len);
	if (buf == NULL) {
		return NULL;
	}
	if (api->get_params(enc->dev->dev, &get, &feedback, &len, buf)
			!= VK_SUCCESS) {
		free(buf);
		return NULL;
	}
	*out_len = len;
	return buf;
}

bool avk_encoder_encode_still(struct avk_encoder *enc, VkImage src,
		VkImageView src_view, VkImageLayout src_layout,
		void **out, size_t *out_len) {
	if (enc == NULL || out == NULL || out_len == NULL) {
		return false;
	}
	struct avk_device *dev = enc->dev;
	struct encode_cmd_api api;
	if (!load_cmd_api(dev, &api)) {
		avk_log(AVK_ERROR, "encode: the encode command entry points are "
			"missing");
		return false;
	}

	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkResetCommandBuffer(enc->cmd, 0);
	if (vkBeginCommandBuffer(enc->cmd, &begin) != VK_SUCCESS) {
		return false;
	}

	/* Both pictures have to be in their video layouts before the coding scope
	 * opens; a layout transition is not allowed inside one. */
	VkImageMemoryBarrier2 barriers[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.srcAccessMask = 0,
			.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
			.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
			.oldLayout = src_layout,
			.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = src,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.srcAccessMask = 0,
			.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
			.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = enc->dpb_image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		},
	};
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 2,
		.pImageMemoryBarriers = barriers,
	};
	vkCmdPipelineBarrier2(enc->cmd, &dep);
	vkCmdResetQueryPool(enc->cmd, enc->feedback, 0, 1);

	VkVideoPictureResourceInfoKHR dpb_resource = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
		.codedExtent = {enc->coded_width, enc->coded_height},
		.baseArrayLayer = 0,
		.imageViewBinding = enc->dpb_view,
	};
	/* slotIndex -1 in BeginCoding means "reserve this slot, it is not active
	 * yet". The same resource comes back with slotIndex 0 as the encode's
	 * setup slot, which is what activates it. */
	VkVideoReferenceSlotInfoKHR begin_slot = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
		.slotIndex = -1,
		.pPictureResource = &dpb_resource,
	};
	VkVideoBeginCodingInfoKHR begin_coding = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
		.videoSession = enc->session,
		.videoSessionParameters = enc->params,
		.referenceSlotCount = 1,
		.pReferenceSlots = &begin_slot,
	};
	api.begin(enc->cmd, &begin_coding);

	/* A session must be reset before its first picture, and the rate control
	 * mode is set in the same control command. DISABLED means the QP in the
	 * slice header is used as given, which is what a still wants: one picture
	 * has no bitrate to converge on. */
	VkVideoEncodeRateControlInfoKHR rate = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR,
		.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR,
	};
	VkVideoCodingControlInfoKHR control = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
		.pNext = &rate,
		.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR
			| VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR,
	};
	api.control(enc->cmd, &control);

	StdVideoEncodeH265SliceSegmentHeader slice_header = {
		.flags = {
			.first_slice_segment_in_pic_flag = 1,
			.slice_sao_luma_flag = 1,
			.slice_sao_chroma_flag = 1,
		},
		.slice_type = STD_VIDEO_H265_SLICE_TYPE_I,
		.slice_segment_address = 0,
		.MaxNumMergeCand = 5,
		.slice_qp_delta = 0,
	};
	/* 26 is H.265's neutral QP -- init_qp_minus26 is 0 in the PPS, so this is
	 * what the slice inherits. Visually near-transparent on a desktop still
	 * and small enough that a 4K picture is a few megabytes. */
	const uint32_t qp = 26;
	VkVideoEncodeH265NaluSliceSegmentInfoKHR nalu = {
		.sType =
			VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_KHR,
		.constantQp = (int32_t)qp,
		.pStdSliceSegmentHeader = &slice_header,
	};
	StdVideoEncodeH265ReferenceListsInfo ref_lists = {
		.num_ref_idx_l0_active_minus1 = 0,
		.num_ref_idx_l1_active_minus1 = 0,
	};
	memset(ref_lists.RefPicList0, STD_VIDEO_H265_NO_REFERENCE_PICTURE,
		sizeof(ref_lists.RefPicList0));
	memset(ref_lists.RefPicList1, STD_VIDEO_H265_NO_REFERENCE_PICTURE,
		sizeof(ref_lists.RefPicList1));
	StdVideoEncodeH265PictureInfo std_pic = {
		.flags = {
			.IrapPicFlag = 1,
			.is_reference = 1,
			.no_output_of_prior_pics_flag = 0,
		},
		.pic_type = STD_VIDEO_H265_PICTURE_TYPE_IDR,
		.sps_video_parameter_set_id = 0,
		.pps_seq_parameter_set_id = 0,
		.pps_pic_parameter_set_id = 0,
		.PicOrderCntVal = 0,
		.TemporalId = 0,
		.pRefLists = &ref_lists,
	};
	VkVideoEncodeH265PictureInfoKHR h265_pic = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR,
		.naluSliceSegmentEntryCount = 1,
		.pNaluSliceSegmentEntries = &nalu,
		.pStdPictureInfo = &std_pic,
	};
	VkVideoPictureResourceInfoKHR src_resource = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
		.codedExtent = {enc->coded_width, enc->coded_height},
		.baseArrayLayer = 0,
		.imageViewBinding = src_view,
	};
	/* The reconstructed picture has to describe itself in the codec's own
	 * terms as well as Vulkan's: without VkVideoEncodeH265DpbSlotInfoKHR on
	 * the setup slot the encode command is invalid, and the driver accepts it
	 * anyway and writes a picture from whatever it made of the slot. */
	StdVideoEncodeH265ReferenceInfo std_ref = {
		.pic_type = STD_VIDEO_H265_PICTURE_TYPE_IDR,
		.PicOrderCntVal = 0,
		.TemporalId = 0,
	};
	VkVideoEncodeH265DpbSlotInfoKHR h265_slot = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR,
		.pStdReferenceInfo = &std_ref,
	};
	VkVideoReferenceSlotInfoKHR setup_slot = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
		.pNext = &h265_slot,
		.slotIndex = 0,
		.pPictureResource = &dpb_resource,
	};
	VkVideoEncodeInfoKHR encode_info = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR,
		.pNext = &h265_pic,
		.dstBuffer = enc->bitstream,
		.dstBufferOffset = 0,
		.dstBufferRange = enc->bitstream_size,
		.srcPictureResource = src_resource,
		.pSetupReferenceSlot = &setup_slot,
	};

	vkCmdBeginQuery(enc->cmd, enc->feedback, 0, 0);
	api.encode(enc->cmd, &encode_info);
	vkCmdEndQuery(enc->cmd, enc->feedback, 0);

	VkVideoEndCodingInfoKHR end_coding = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,
	};
	api.end(enc->cmd, &end_coding);

	if (vkEndCommandBuffer(enc->cmd) != VK_SUCCESS) {
		return false;
	}
	vkResetFences(dev->dev, 1, &enc->fence);
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &enc->cmd,
	};
	if (vkQueueSubmit(dev->encode_queue, 1, &submit, enc->fence)
			!= VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: vkQueueSubmit to the encode queue failed");
		return false;
	}
	if (vkWaitForFences(dev->dev, 1, &enc->fence, VK_TRUE, 5000000000ULL)
			!= VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: the encode did not finish within 5s");
		return false;
	}

	/* offset then bytes-written, in the order the flags were declared. */
	uint32_t results[2] = {0, 0};
	if (vkGetQueryPoolResults(dev->dev, enc->feedback, 0, 1, sizeof(results),
			results, sizeof(uint32_t),
			VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: no feedback from the encode query");
		return false;
	}
	uint32_t offset = results[0], written = results[1];
	if (written == 0) {
		avk_log(AVK_ERROR, "encode: the encoder wrote no bytes");
		return false;
	}

	size_t hdr_len = 0;
	uint8_t *hdr = encoded_parameters(enc, &api, &hdr_len);
	if (hdr == NULL) {
		return false;
	}

	void *mapped = NULL;
	if (vkMapMemory(dev->dev, enc->bitstream_memory, 0, VK_WHOLE_SIZE, 0,
			&mapped) != VK_SUCCESS) {
		free(hdr);
		return false;
	}
	uint8_t *stream = malloc(hdr_len + written);
	if (stream == NULL) {
		vkUnmapMemory(dev->dev, enc->bitstream_memory);
		free(hdr);
		return false;
	}
	memcpy(stream, hdr, hdr_len);
	memcpy(stream + hdr_len, (const uint8_t *)mapped + offset, written);
	vkUnmapMemory(dev->dev, enc->bitstream_memory);
	free(hdr);

	avk_log(AVK_INFO, "encode: %u bytes of picture + %zu of parameter sets "
		"at QP %u", written, hdr_len, qp);
	*out = stream;
	*out_len = hdr_len + written;
	return true;
}
