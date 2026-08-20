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
	}
	free(enc->session_memory);
	free(enc);
}
