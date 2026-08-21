#define _GNU_SOURCE
#include "render/vulkan/encode/avk_encode.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "render/vulkan/device/avk_device.h"
#include "render/vulkan/device/avk_instance.h"
#include "render/vulkan/avk.h"

#include "rgb_to_p010_comp.spv.h"

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

static int64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
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
	enc->profile = (VkVideoProfileInfoKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
		.pNext = &enc->h265_profile,
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
	enc->encode_caps = (VkVideoEncodeCapabilitiesKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR,
		.pNext = &enc->h265_caps,
	};
	enc->caps = (VkVideoCapabilitiesKHR){
		.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
		.pNext = &enc->encode_caps,
	};
	VkResult r = get_caps(dev->phys, &enc->profile, &enc->caps);
	if (r == VK_SUCCESS) {
		/* The block sizes the SPS has to agree with. A parameter set that
		 * describes a geometry the encoder did not use produces a valid
		 * bitstream that decodes to garbage, with nothing failing anywhere. */
		avk_log(AVK_INFO, "encode: ctbSizes 0x%x transformBlockSizes 0x%x "
			"maxLevelIdc %d qp %d..%d",
			(unsigned)enc->h265_caps.ctbSizes,
			(unsigned)enc->h265_caps.transformBlockSizes,
			(int)enc->h265_caps.maxLevelIdc,
			enc->h265_caps.minQp, enc->h265_caps.maxQp);
	}
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
	/* Optimal tiling, which is the only one worth having for an image the
	 * GPU both writes and encodes from. */
	VkFormat chosen = props[0].format;
	for (uint32_t i = 0; i < n; i++) {
		if (props[i].imageTiling == VK_IMAGE_TILING_OPTIMAL) {
			chosen = props[i].format;
			break;
		}
	}
	free(props);
	*out = chosen;

	/* Whether this format can be an encode input at all. VkFormatProperties2
	 * takes no video profile -- a profile list in its pNext is invalid, which
	 * is worth stating because the answer therefore describes the format
	 * plainly and not under any profile. */
	VkFormatProperties3 fmt3 = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
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
		/* NOT h265_caps.maxLevelIdc, which this driver reports as 0 --
		 * LEVEL_IDC_1_0, whose limit is 176x144. Writing that into a stream
		 * carrying 3840x2160 declares a picture the level forbids, and
		 * nothing rejects it: the encoder encodes, the headers are written,
		 * and the decoder is left to reconcile two things that cannot both be
		 * true. The level is derived from the picture instead. */
		.general_level_idc = enc->level_idc,
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
	/*
	 * The colour description. Without it every player has to guess, and the
	 * guess is BT.709 SDR -- so an HDR10 picture is shown as though its PQ
	 * signal were sRGB, which is not subtly wrong, it is unwatchable.
	 *
	 * The three numbers are the H.273 code points: 9 is BT.2020 primaries,
	 * 16 is SMPTE ST 2084 (PQ), 9 again is BT.2020 non-constant luminance as
	 * a matrix. video_full_range_flag says the samples span 0..1023 rather
	 * than 64..940, which is what the conversion produces.
	 */
	StdVideoH265SequenceParameterSetVui vui = {
		.flags = {
			.video_signal_type_present_flag = 1,
			.video_full_range_flag = 1,
			.colour_description_present_flag = 1,
		},
		.video_format = 5,   /* unspecified */
		.colour_primaries = enc->colour == AVK_ENCODE_COLOUR_HDR10 ? 9 : 1,
		.transfer_characteristics =
			enc->colour == AVK_ENCODE_COLOUR_HDR10 ? 16 : 13,
		.matrix_coeffs = enc->colour == AVK_ENCODE_COLOUR_HDR10 ? 9 : 1,
	};
	StdVideoH265SequenceParameterSet sps = {
		.flags = {
			.vui_parameters_present_flag = 1,
			.sps_temporal_id_nesting_flag = 1,
			.sps_sub_layer_ordering_info_present_flag = 1,
			.conformance_window_flag =
				(enc->coded_width != enc->width
					|| enc->coded_height != enc->height) ? 1u : 0u,
			.amp_enabled_flag = 1,
			.sample_adaptive_offset_enabled_flag = 1,
			.sps_temporal_mvp_enabled_flag = 0,
			.strong_intra_smoothing_enabled_flag = 1,
		},
		.chroma_format_idc = STD_VIDEO_H265_CHROMA_FORMAT_IDC_420,
		.pic_width_in_luma_samples = enc->coded_width,
		.pic_height_in_luma_samples = enc->coded_height,
		/*
		 * THE CONFORMANCE WINDOW, which is how the coded size stops being the
		 * displayed size.
		 *
		 * A picture is coded at the encoder's alignment -- 1080 becomes 1088,
		 * 1366 becomes 1408 -- and without this the decoder shows every
		 * padded row and column. It does not look like corruption: ffprobe
		 * reports the recording as 1920x1088 and the extra rows hold whatever
		 * the conversion left there, which on a screen capture is a strip of
		 * the picture's own edge repeated.
		 *
		 * The offsets are in CHROMA samples for 4:2:0, so a difference of
		 * eight luma rows is four here. Halving twice, or not at all, is the
		 * mistake this arithmetic invites.
		 */
		.conf_win_right_offset = (enc->coded_width - enc->width) / 2,
		.conf_win_bottom_offset = (enc->coded_height - enc->height) / 2,
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
		/* A 64x64 CTB cannot hold a single 32x32 transform, so a depth of 0
		 * asks for a TU tree that cannot describe the CTB the encoder is
		 * using. One split is the minimum that is consistent. */
		.max_transform_hierarchy_depth_inter = 1,
		.max_transform_hierarchy_depth_intra = 1,
		.pProfileTierLevel = &ptl,
		.pDecPicBufMgr = &dpb_mgr,
		.pSequenceParameterSetVui = &vui,
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
		.arrayLayers = AVK_ENCODE_DPB_SLOTS,
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
	/* One view per layer: a picture resource names a view and a layer, and
	 * giving each slot its own view keeps the two from disagreeing. */
	for (uint32_t i = 0; i < AVK_ENCODE_DPB_SLOTS; i++) {
		VkImageViewCreateInfo view = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = enc->dpb_image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = enc->dpb_format,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.baseArrayLayer = i,
				.layerCount = 1,
			},
		};
		if (vkCreateImageView(dev->dev, &view, NULL, &enc->dpb_view[i])
				!= VK_SUCCESS) {
			return false;
		}
	}
	return true;
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
	VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedback = {
		.sType =
			VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR,
		.pNext = &enc->profile,
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


/* ── RGB -> P010, because the driver's own conversion does not work ─────── */

/*
 * The encoder's input picture.
 *
 * P010 is a two-plane format and a compute shader cannot write a multi-planar
 * image directly, so the image is created MUTABLE and each plane gets a view
 * in a plain single-plane format of the same bit width: plane 0 as R16, plane
 * 1 as R16G16. That is the sanctioned way to write one, and it is why
 * EXTENDED_USAGE is set -- STORAGE is not a usage P010 itself supports, only
 * the per-plane formats do.
 */
/* The RGB copy the conversion reads. STORAGE because a compute shader loads
 * from it, TRANSFER_DST because the caller's picture is copied into it. */
static bool create_rgb(struct avk_encoder *enc) {
	struct avk_device *dev = enc->dev;
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
		.extent = {enc->coded_width, enc->coded_height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_STORAGE_BIT
			| VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(dev->dev, &info, NULL, &enc->rgb_image) != VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: cannot create the RGB staging image");
		return false;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev->dev, enc->rgb_image, &req);
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
	if (vkAllocateMemory(dev->dev, &alloc, NULL, &enc->rgb_memory)
			!= VK_SUCCESS
			|| vkBindImageMemory(dev->dev, enc->rgb_image, enc->rgb_memory, 0)
				!= VK_SUCCESS) {
		return false;
	}
	VkImageViewCreateInfo vinfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = enc->rgb_image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_A2R10G10B10_UNORM_PACK32,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	return vkCreateImageView(dev->dev, &vinfo, NULL, &enc->rgb_view)
		== VK_SUCCESS;
}

static bool create_p010(struct avk_encoder *enc) {
	struct avk_device *dev = enc->dev;
	uint32_t families[2] = {dev->caps.graphics_family,
		dev->caps.video_encode_family};
	bool split = families[0] != families[1];
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &enc->profile_list,
		.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT
			| VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = enc->src_format,
		.extent = {enc->coded_width, enc->coded_height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR
			| VK_IMAGE_USAGE_STORAGE_BIT
			/* So the converted picture can be read back and checked. Without
			 * it the only evidence about the conversion is the bitstream,
			 * which is the thing under suspicion. */
			| VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		/* Written on the graphics family and read on the encode family.
		 * CONCURRENT rather than an ownership transfer: this image is written
		 * once and read once per still, so the transfer would cost two more
		 * barriers and a second submission to save a compression mode that a
		 * single-use intermediate never benefits from. */
		.sharingMode = split ? VK_SHARING_MODE_CONCURRENT
			: VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = split ? 2 : 0,
		.pQueueFamilyIndices = split ? families : NULL,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (vkCreateImage(dev->dev, &info, NULL, &enc->p010_image) != VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: cannot create the P010 input image");
		return false;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(dev->dev, enc->p010_image, &req);
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
	if (vkAllocateMemory(dev->dev, &alloc, NULL, &enc->p010_memory)
			!= VK_SUCCESS
			|| vkBindImageMemory(dev->dev, enc->p010_image,
				enc->p010_memory, 0) != VK_SUCCESS) {
		return false;
	}

	struct {
		VkImageView *view;
		VkFormat format;
		VkImageAspectFlags aspect;
		VkImageUsageFlags usage;
	} views[] = {
		{&enc->p010_view, enc->src_format, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR},
		{&enc->p010_y_view, VK_FORMAT_R16_UNORM, VK_IMAGE_ASPECT_PLANE_0_BIT,
			VK_IMAGE_USAGE_STORAGE_BIT},
		{&enc->p010_uv_view, VK_FORMAT_R16G16_UNORM,
			VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_USAGE_STORAGE_BIT},
	};
	for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
		/* Each view declares the ONE usage it is for. Without this the view
		 * inherits both, and a plane view carrying VIDEO_ENCODE_SRC is
		 * invalid for the same reason the RGB view was. */
		VkImageViewUsageCreateInfo usage = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
			.usage = views[i].usage,
		};
		VkImageViewCreateInfo vinfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.pNext = &usage,
			.image = enc->p010_image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = views[i].format,
			.subresourceRange = {views[i].aspect, 0, 1, 0, 1},
		};
		if (vkCreateImageView(dev->dev, &vinfo, NULL, views[i].view)
				!= VK_SUCCESS) {
			avk_log(AVK_ERROR, "encode: cannot create P010 view %zu", i);
			return false;
		}
	}
	return true;
}

/* Must match the block in rgb_to_p010.comp. The padding is not decoration:
 * a vec4 is 16-byte aligned in a push-constant block, so the two ints cannot
 * simply be followed by floats. */
struct conv_push {
	int32_t width, height;
	int32_t pad0, pad1;
	float kr, kg, kb, pad2;
	float cb_div, cr_div;
};

static bool create_conversion(struct avk_encoder *enc) {
	struct avk_device *dev = enc->dev;
	VkDescriptorSetLayoutBinding bindings[3] = {
		{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
			VK_SHADER_STAGE_COMPUTE_BIT, NULL},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
			VK_SHADER_STAGE_COMPUTE_BIT, NULL},
		{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
			VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	};
	VkDescriptorSetLayoutCreateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 3,
		.pBindings = bindings,
	};
	if (vkCreateDescriptorSetLayout(dev->dev, &set_info, NULL,
			&enc->conv_set_layout) != VK_SUCCESS) {
		return false;
	}
	VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3};
	VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	if (vkCreateDescriptorPool(dev->dev, &pool_info, NULL, &enc->conv_pool)
			!= VK_SUCCESS) {
		return false;
	}
	VkDescriptorSetAllocateInfo set_alloc = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = enc->conv_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &enc->conv_set_layout,
	};
	if (vkAllocateDescriptorSets(dev->dev, &set_alloc, &enc->conv_set)
			!= VK_SUCCESS) {
		return false;
	}

	VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
		sizeof(struct conv_push)};
	VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &enc->conv_set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &range,
	};
	if (vkCreatePipelineLayout(dev->dev, &layout_info, NULL,
			&enc->conv_pipeline_layout) != VK_SUCCESS) {
		return false;
	}
	VkShaderModuleCreateInfo mod_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = sizeof(rgb_to_p010_comp_spv),
		.pCode = rgb_to_p010_comp_spv,
	};
	VkShaderModule module;
	if (vkCreateShaderModule(dev->dev, &mod_info, NULL, &module)
			!= VK_SUCCESS) {
		return false;
	}
	VkComputePipelineCreateInfo pipe_info = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = module,
			.pName = "main",
		},
		.layout = enc->conv_pipeline_layout,
	};
	VkResult r = vkCreateComputePipelines(dev->dev, VK_NULL_HANDLE, 1,
		&pipe_info, NULL, &enc->conv_pipeline);
	vkDestroyShaderModule(dev->dev, module, NULL);
	if (r != VK_SUCCESS) {
		avk_log(AVK_ERROR, "encode: cannot create the RGB->P010 pipeline");
		return false;
	}

	VkCommandPoolCreateInfo cpool = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = dev->caps.graphics_family,
	};
	if (vkCreateCommandPool(dev->dev, &cpool, NULL, &enc->conv_cmd_pool)
			!= VK_SUCCESS) {
		return false;
	}
	VkCommandBufferAllocateInfo cb = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = enc->conv_cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (vkAllocateCommandBuffers(dev->dev, &cb, &enc->conv_cmd)
			!= VK_SUCCESS) {
		return false;
	}
	VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	return vkCreateFence(dev->dev, &fence, NULL, &enc->conv_fence)
		== VK_SUCCESS;
}

/* Run the conversion on the graphics queue and wait for it. */
static bool convert_to_p010(struct avk_encoder *enc, VkImage src,
		VkImageLayout src_layout, int32_t src_x, int32_t src_y) {
	VkImageView src_view = enc->rgb_view;
	struct avk_device *dev = enc->dev;
	VkDescriptorImageInfo images[3] = {
		{VK_NULL_HANDLE, src_view, VK_IMAGE_LAYOUT_GENERAL},
		{VK_NULL_HANDLE, enc->p010_y_view, VK_IMAGE_LAYOUT_GENERAL},
		{VK_NULL_HANDLE, enc->p010_uv_view, VK_IMAGE_LAYOUT_GENERAL},
	};
	VkWriteDescriptorSet writes[3];
	for (int i = 0; i < 3; i++) {
		writes[i] = (VkWriteDescriptorSet){
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = enc->conv_set,
			.dstBinding = (uint32_t)i,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.pImageInfo = &images[i],
		};
	}
	vkUpdateDescriptorSets(dev->dev, 3, writes, 0, NULL);

	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkResetCommandBuffer(enc->conv_cmd, 0);
	if (vkBeginCommandBuffer(enc->conv_cmd, &begin) != VK_SUCCESS) {
		return false;
	}
	/* Both planes to GENERAL, which is what a storage write needs. The whole
	 * image is transitioned in one barrier: a per-plane transition would need
	 * VK_IMAGE_ASPECT_PLANE_n and separate barriers for no benefit. */
	VkImageMemoryBarrier2 to_general = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = enc->p010_image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &to_general,
	};
	vkCmdPipelineBarrier2(enc->conv_cmd, &dep);

	/* The caller's picture into ours. Both layouts are restored on the way
	 * out: the source goes back to what it was, because an image borrowed for
	 * a screenshot must not come back in a different state, and the staging
	 * copy goes to GENERAL for the compute read. */
	VkImageMemoryBarrier2 in[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
			.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
			.oldLayout = src_layout,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = src,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
			.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = enc->rgb_image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		},
	};
	VkDependencyInfo in_dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 2,
		.pImageMemoryBarriers = in,
	};
	vkCmdPipelineBarrier2(enc->conv_cmd, &in_dep);
	VkImageCopy copy = {
		.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		.srcOffset = {src_x, src_y, 0},
		.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		.extent = {enc->width, enc->height, 1},
	};
	vkCmdCopyImage(enc->conv_cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		enc->rgb_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	VkImageMemoryBarrier2 out_b[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
			.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			.newLayout = src_layout,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = src,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
			.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = enc->rgb_image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		},
	};
	VkDependencyInfo out_dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 2,
		.pImageMemoryBarriers = out_b,
	};
	vkCmdPipelineBarrier2(enc->conv_cmd, &out_dep);

	vkCmdBindPipeline(enc->conv_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
		enc->conv_pipeline);
	vkCmdBindDescriptorSets(enc->conv_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
		enc->conv_pipeline_layout, 0, 1, &enc->conv_set, 0, NULL);
	/* The luma coefficients and the difference scalings that go with them.
	 * BT.2020 and BT.709 differ in both, and the pair used here is the pair
	 * named in the SPS -- see create_parameters. */
	struct conv_push push = {
		.width = (int32_t)enc->coded_width,
		.height = (int32_t)enc->coded_height,
	};
	if (enc->colour == AVK_ENCODE_COLOUR_HDR10) {
		push.kr = 0.2627f; push.kg = 0.6780f; push.kb = 0.0593f;
		push.cb_div = 1.8814f; push.cr_div = 1.4746f;
	} else {
		push.kr = 0.2126f; push.kg = 0.7152f; push.kb = 0.0722f;
		push.cb_div = 1.8556f; push.cr_div = 1.5748f;
	}
	vkCmdPushConstants(enc->conv_cmd, enc->conv_pipeline_layout,
		VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	/* One invocation per chroma sample: half the luma extent in each axis,
	 * rounded up, over an 8x8 local size. */
	uint32_t gx = ((enc->coded_width + 1) / 2 + 7) / 8;
	uint32_t gy = ((enc->coded_height + 1) / 2 + 7) / 8;
	vkCmdDispatch(enc->conv_cmd, gx, gy, 1);

	if (vkEndCommandBuffer(enc->conv_cmd) != VK_SUCCESS) {
		return false;
	}
	vkResetFences(dev->dev, 1, &enc->conv_fence);
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &enc->conv_cmd,
	};
	if (vkQueueSubmit(dev->graphics_queue, 1, &submit, enc->conv_fence)
			!= VK_SUCCESS) {
		return false;
	}
	return vkWaitForFences(dev->dev, 1, &enc->conv_fence, VK_TRUE,
		5000000000ULL) == VK_SUCCESS;
}

/* ── HDR10 static metadata, as SEI NAL units ───────────────────────────── */

/*
 * Vulkan Video writes the parameter sets and the coded picture. It does not
 * write SEI, so the two messages HDR10 is defined by are ours to assemble:
 * mastering display colour volume (137) and content light level info (144).
 *
 * Both are short, fixed-layout, big-endian payloads. The only subtlety is that
 * a NAL payload may not contain 00 00 00, 00 00 01, 00 00 02 or 00 00 03, so
 * an escape byte is inserted -- a luminance of 0x00000100 would otherwise end
 * the NAL unit early and take the rest of the picture with it.
 */
struct sei_writer {
	uint8_t *buf;
	size_t len, cap;
	int zeros;
};

static void sei_raw(struct sei_writer *w, uint8_t b) {
	if (w->len < w->cap) {
		w->buf[w->len++] = b;
	}
}

/* Payload bytes, with emulation prevention applied. */
static void sei_byte(struct sei_writer *w, uint8_t b) {
	if (w->zeros >= 2 && b <= 3) {
		sei_raw(w, 0x03);
		w->zeros = 0;
	}
	sei_raw(w, b);
	w->zeros = (b == 0) ? w->zeros + 1 : 0;
}

static void sei_u16(struct sei_writer *w, uint16_t v) {
	sei_byte(w, (uint8_t)(v >> 8));
	sei_byte(w, (uint8_t)v);
}

static void sei_u32(struct sei_writer *w, uint32_t v) {
	for (int i = 24; i >= 0; i -= 8) {
		sei_byte(w, (uint8_t)(v >> i));
	}
}

static bool mastering_known(const struct avk_encode_mastering *m) {
	return m->max_luminance != 0 || m->white_point_x != 0
		|| m->display_primaries_x[0] != 0;
}

/*
 * Both messages as one prefix-SEI NAL unit, Annex B framed. NULL when there is
 * nothing worth saying: a player believes an SEI, so guessed numbers are worse
 * than none.
 */
static uint8_t *build_hdr_sei(const struct avk_encoder *enc, size_t *out_len) {
	*out_len = 0;
	if (enc->colour != AVK_ENCODE_COLOUR_HDR10
			|| !mastering_known(&enc->mastering)) {
		return NULL;
	}
	const struct avk_encode_mastering *m = &enc->mastering;
	uint8_t *buf = malloc(128);
	if (buf == NULL) {
		return NULL;
	}
	struct sei_writer w = {.buf = buf, .cap = 128};
	/* Start code, then the two-byte NAL header: type 39 (PREFIX_SEI_NUT),
	 * layer 0, nuh_temporal_id_plus1 1. The header is not subject to
	 * emulation prevention. */
	sei_raw(&w, 0); sei_raw(&w, 0); sei_raw(&w, 0); sei_raw(&w, 1);
	sei_raw(&w, (uint8_t)(39 << 1)); sei_raw(&w, 1);
	w.zeros = 0;

	/* mastering_display_colour_volume: 24 bytes, primaries in G, B, R. */
	sei_byte(&w, 137);
	sei_byte(&w, 24);
	for (int i = 0; i < 3; i++) {
		sei_u16(&w, m->display_primaries_x[i]);
		sei_u16(&w, m->display_primaries_y[i]);
	}
	sei_u16(&w, m->white_point_x);
	sei_u16(&w, m->white_point_y);
	sei_u32(&w, m->max_luminance);
	sei_u32(&w, m->min_luminance);

	/* content_light_level_info: 4 bytes. */
	sei_byte(&w, 144);
	sei_byte(&w, 4);
	sei_u16(&w, m->max_content_light_level);
	sei_u16(&w, m->max_frame_average_light_level);

	/* rbsp_trailing_bits: for a byte-aligned payload, one stop bit. */
	sei_byte(&w, 0x80);
	*out_len = w.len;
	return buf;
}

struct avk_encoder *avk_encoder_create(struct avk_device *dev,
		uint32_t width, uint32_t height, enum avk_encode_colour colour,
		const struct avk_encode_mastering *mastering) {
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
	enc->colour = colour;
	if (mastering != NULL) {
		enc->mastering = *mastering;
	}

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
	/* H.265 levels, by luma sample count. 5.1 is the first that admits
	 * 3840x2160; below that the picture is larger than the level allows. */
	uint32_t samples = enc->coded_width * enc->coded_height;
	enc->level_idc = samples <= 552960 ? STD_VIDEO_H265_LEVEL_IDC_3_1
		: samples <= 2228224 ? STD_VIDEO_H265_LEVEL_IDC_4_1
		: samples <= 8912896 ? STD_VIDEO_H265_LEVEL_IDC_5_1
		: STD_VIDEO_H265_LEVEL_IDC_6_1;

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

	VkVideoSessionCreateInfoKHR session_info = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
		.queueFamilyIndex = dev->caps.video_encode_family,
		.pVideoProfile = &enc->profile,
		.pictureFormat = enc->src_format,
		.maxCodedExtent = (VkExtent2D){enc->coded_width, enc->coded_height},
		.referencePictureFormat = enc->dpb_format,
		/* Two slots, one active reference: the picture being reconstructed
		 * and the one before it that a P picture predicts from. A still uses
		 * only the first and costs the second nothing but its allocation. */
		.maxDpbSlots = AVK_ENCODE_DPB_SLOTS,
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
			|| !create_commands(enc) || !create_rgb(enc)
			|| !create_p010(enc) || !create_conversion(enc)) {
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
		for (uint32_t i = 0; i < AVK_ENCODE_DPB_SLOTS; i++) {
			if (enc->dpb_view[i] != VK_NULL_HANDLE) {
				vkDestroyImageView(dev->dev, enc->dpb_view[i], NULL);
			}
		}
		if (enc->dpb_image != VK_NULL_HANDLE) {
			vkDestroyImage(dev->dev, enc->dpb_image, NULL);
		}
		if (enc->dpb_memory != VK_NULL_HANDLE) {
			vkFreeMemory(dev->dev, enc->dpb_memory, NULL);
		}
		if (enc->conv_fence != VK_NULL_HANDLE) {
			vkDestroyFence(dev->dev, enc->conv_fence, NULL);
		}
		if (enc->conv_cmd_pool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(dev->dev, enc->conv_cmd_pool, NULL);
		}
		if (enc->conv_pipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(dev->dev, enc->conv_pipeline, NULL);
		}
		if (enc->conv_pipeline_layout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(dev->dev, enc->conv_pipeline_layout, NULL);
		}
		if (enc->conv_pool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(dev->dev, enc->conv_pool, NULL);
		}
		if (enc->conv_set_layout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(dev->dev, enc->conv_set_layout, NULL);
		}
		if (enc->p010_view != VK_NULL_HANDLE) {
			vkDestroyImageView(dev->dev, enc->p010_view, NULL);
		}
		if (enc->p010_y_view != VK_NULL_HANDLE) {
			vkDestroyImageView(dev->dev, enc->p010_y_view, NULL);
		}
		if (enc->p010_uv_view != VK_NULL_HANDLE) {
			vkDestroyImageView(dev->dev, enc->p010_uv_view, NULL);
		}
		if (enc->p010_image != VK_NULL_HANDLE) {
			vkDestroyImage(dev->dev, enc->p010_image, NULL);
		}
		if (enc->p010_memory != VK_NULL_HANDLE) {
			vkFreeMemory(dev->dev, enc->p010_memory, NULL);
		}
		if (enc->rgb_view != VK_NULL_HANDLE) {
			vkDestroyImageView(dev->dev, enc->rgb_view, NULL);
		}
		if (enc->rgb_image != VK_NULL_HANDLE) {
			vkDestroyImage(dev->dev, enc->rgb_image, NULL);
		}
		if (enc->rgb_memory != VK_NULL_HANDLE) {
			vkFreeMemory(dev->dev, enc->rgb_memory, NULL);
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

static bool encode_picture(struct avk_encoder *enc, VkImage src,
		VkImageLayout src_layout, int32_t src_x, int32_t src_y, bool key,
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

	/* The conversion runs on the graphics queue and is waited for, so the
	 * encode below sees a finished P010 picture. One submission each rather
	 * than a semaphore between them: a still is not on a frame budget, and
	 * two queues chained by a fence is easier to reason about than two
	 * chained by a timeline nothing else uses. */
	int64_t t0 = now_ns();
	if (!convert_to_p010(enc, src, src_layout, src_x, src_y)) {
		avk_log(AVK_ERROR, "encode: the RGB->P010 conversion failed");
		return false;
	}
	enc->last_convert_ns = now_ns() - t0;
	int64_t t1 = now_ns();

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
			.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
			.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = enc->p010_image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		},
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			.srcAccessMask = 0,
			.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
			.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
			/* UNDEFINED discards the contents, which is right exactly once.
			 * On every later picture the other layer holds the reference the
			 * encoder is about to predict from, and discarding it produces a
			 * P picture predicted from nothing -- which still encodes, still
			 * decodes, and is visibly wrong. */
			.oldLayout = enc->session_reset
				? VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR
				: VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = enc->dpb_image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
				AVK_ENCODE_DPB_SLOTS},
		},
	};
	VkDependencyInfo dep = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 2,
		.pImageMemoryBarriers = barriers,
	};
	vkCmdPipelineBarrier2(enc->cmd, &dep);
	vkCmdResetQueryPool(enc->cmd, enc->feedback, 0, 1);

	/* This picture is reconstructed into `slot`; the one before it lives in
	 * `ref_slot` and is what a P picture predicts from. */
	const uint32_t slot = enc->dpb_slot;
	const uint32_t ref_slot = (slot + 1) % AVK_ENCODE_DPB_SLOTS;

	VkVideoPictureResourceInfoKHR dpb_resource = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
		.codedExtent = {enc->coded_width, enc->coded_height},
		.baseArrayLayer = 0,
		.imageViewBinding = enc->dpb_view[slot],
	};
	VkVideoPictureResourceInfoKHR ref_resource = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
		.codedExtent = {enc->coded_width, enc->coded_height},
		.baseArrayLayer = 0,
		.imageViewBinding = enc->dpb_view[ref_slot],
	};
	StdVideoEncodeH265ReferenceInfo ref_std = {
		.pic_type = STD_VIDEO_H265_PICTURE_TYPE_P,
		.PicOrderCntVal = enc->poc - 1,
	};
	VkVideoEncodeH265DpbSlotInfoKHR ref_h265 = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR,
		.pStdReferenceInfo = &ref_std,
	};

	/*
	 * BeginCoding lists every slot the scope will touch. A slot that is not
	 * yet active is named with slotIndex -1; the reference of a P picture is
	 * named with its real index, because it has to be found. Getting this
	 * backwards does not fail -- the encoder predicts from a slot holding
	 * nothing and the picture comes out as noise that still decodes.
	 */
	VkVideoReferenceSlotInfoKHR begin_slots[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
			.slotIndex = -1,
			.pPictureResource = &dpb_resource,
		},
		{
			.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
			.pNext = &ref_h265,
			.slotIndex = (int32_t)ref_slot,
			.pPictureResource = &ref_resource,
		},
	};
	VkVideoBeginCodingInfoKHR begin_coding = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
		.videoSession = enc->session,
		.videoSessionParameters = enc->params,
		.referenceSlotCount = key ? 1u : 2u,
		.pReferenceSlots = begin_slots,
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
	/* RESET is a session-lifetime thing, not a per-picture one: issuing it
	 * again mid-sequence throws away the DPB the next P picture is about to
	 * predict from. */
	if (!enc->session_reset) {
		VkVideoCodingControlInfoKHR control = {
			.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
			.pNext = &rate,
			.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR
				| VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR,
		};
		api.control(enc->cmd, &control);
		enc->session_reset = true;
	}

	StdVideoEncodeH265SliceSegmentHeader slice_header = {
		.flags = {
			.first_slice_segment_in_pic_flag = 1,
			.slice_sao_luma_flag = 1,
			.slice_sao_chroma_flag = 1,
			/* A P slice states its own reference count rather than
			 * inheriting the PPS default, which is what lets one picture
			 * reference exactly one. */
			.num_ref_idx_active_override_flag = key ? 0u : 1u,
		},
		.slice_type = key ? STD_VIDEO_H265_SLICE_TYPE_I
			: STD_VIDEO_H265_SLICE_TYPE_P,
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
	if (!key) {
		/* L0[0] is a DPB SLOT INDEX, not a picture order count. The two are
		 * both small integers and are not interchangeable. */
		ref_lists.RefPicList0[0] = (uint8_t)ref_slot;
	}
	StdVideoH265ShortTermRefPicSet rps = {
		.num_negative_pics = 1,
		.num_positive_pics = 0,
		.delta_poc_s0_minus1 = {0},      /* delta -1: the previous picture */
		.used_by_curr_pic_s0_flag = 1,   /* and this picture does use it */
	};
	StdVideoEncodeH265PictureInfo std_pic = {
		.flags = {
			.IrapPicFlag = key ? 1u : 0u,
			.is_reference = 1,
			.no_output_of_prior_pics_flag = 0,
		},
		.pic_type = key ? STD_VIDEO_H265_PICTURE_TYPE_IDR
			: STD_VIDEO_H265_PICTURE_TYPE_P,
		.sps_video_parameter_set_id = 0,
		.pps_seq_parameter_set_id = 0,
		.pps_pic_parameter_set_id = 0,
		/* An IDR resets the count; everything after it counts from there. */
		.PicOrderCntVal = key ? 0 : enc->poc,
		.TemporalId = 0,
		.pRefLists = &ref_lists,
		/* THE REFERENCE, AS THE BITSTREAM DECLARES IT.
		 *
		 * RefPicList0 above is a DPB slot index and tells the ENCODER what to
		 * predict from. It says nothing to a decoder, which has no idea what
		 * the encoder's slots were. The short-term reference picture set is
		 * what travels in the slice header, and without it a decoder reports
		 * "zero refs for a frame with P or B slices" and drops every picture
		 * after the first -- while the NAL unit types still read as a correct
		 * sequence of one IDR and nine P frames.
		 *
		 * One negative reference at delta -1: the picture immediately before
		 * this one. delta_poc_s0_minus1 is the delta MINUS ONE, so 0 means 1.
		 */
		.pShortTermRefPicSet = key ? NULL : &rps,
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
		.imageViewBinding = enc->p010_view,
	};
	/* The reconstructed picture has to describe itself in the codec's own
	 * terms as well as Vulkan's: without VkVideoEncodeH265DpbSlotInfoKHR on
	 * the setup slot the encode command is invalid, and the driver accepts it
	 * anyway and writes a picture from whatever it made of the slot. */
	StdVideoEncodeH265ReferenceInfo std_ref = {
		.pic_type = key ? STD_VIDEO_H265_PICTURE_TYPE_IDR
			: STD_VIDEO_H265_PICTURE_TYPE_P,
		.PicOrderCntVal = key ? 0 : enc->poc,
		.TemporalId = 0,
	};
	VkVideoEncodeH265DpbSlotInfoKHR h265_slot = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR,
		.pStdReferenceInfo = &std_ref,
	};
	VkVideoReferenceSlotInfoKHR setup_slot = {
		.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
		.pNext = &h265_slot,
		.slotIndex = (int32_t)slot,
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
		.referenceSlotCount = key ? 0u : 1u,
		.pReferenceSlots = key ? NULL : &begin_slots[1],
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

	enc->last_encode_ns = now_ns() - t1;

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
	/* After the parameter sets and before the picture, which is where a
	 * prefix SEI belongs: it describes the pictures that follow it. */
	size_t sei_len = 0;
	uint8_t *sei = build_hdr_sei(enc, &sei_len);

	void *mapped = NULL;
	if (vkMapMemory(dev->dev, enc->bitstream_memory, 0, VK_WHOLE_SIZE, 0,
			&mapped) != VK_SUCCESS) {
		free(hdr);
		return false;
	}
	uint8_t *stream = malloc(hdr_len + sei_len + written);
	if (stream == NULL) {
		vkUnmapMemory(dev->dev, enc->bitstream_memory);
		free(hdr);
		free(sei);
		return false;
	}
	memcpy(stream, hdr, hdr_len);
	if (sei_len > 0) {
		memcpy(stream + hdr_len, sei, sei_len);
	}
	memcpy(stream + hdr_len + sei_len, (const uint8_t *)mapped + offset,
		written);
	vkUnmapMemory(dev->dev, enc->bitstream_memory);
	free(hdr);
	free(sei);

	avk_log(AVK_INFO, "encode: %u bytes of picture + %zu of parameter sets "
		"+ %zu of HDR10 SEI at QP %u", written, hdr_len, sei_len, qp);
	*out = stream;
	*out_len = hdr_len + sei_len + written;
	return true;
}


/* ── the two ways in ───────────────────────────────────────────────────── */

void avk_encoder_reset_sequence(struct avk_encoder *enc) {
	if (enc == NULL) {
		return;
	}
	enc->frame_index = 0;
	enc->poc = 0;
	enc->dpb_slot = 0;
}

bool avk_encoder_encode_still(struct avk_encoder *enc, VkImage src,
		VkImageLayout src_layout, int32_t src_x, int32_t src_y,
		void **out, size_t *out_len) {
	if (enc == NULL) {
		return false;
	}
	avk_encoder_reset_sequence(enc);
	return encode_picture(enc, src, src_layout, src_x, src_y, true, out,
		out_len);
}

bool avk_encoder_encode_frame(struct avk_encoder *enc, VkImage src,
		VkImageLayout src_layout, int32_t src_x, int32_t src_y,
		bool force_key, void **out, size_t *out_len) {
	if (enc == NULL) {
		return false;
	}
	/* The first picture of a sequence has nothing to predict from, so it is
	 * an IDR whether or not one was asked for. */
	bool key = force_key || enc->frame_index == 0;
	if (key) {
		enc->poc = 0;
	}
	if (!encode_picture(enc, src, src_layout, src_x, src_y, key, out,
			out_len)) {
		return false;
	}
	/* Advance only on success: a failed picture must not consume the slot the
	 * next one will reference, or the sequence predicts from a gap. */
	enc->frame_index++;
	enc->poc++;
	enc->dpb_slot = (enc->dpb_slot + 1) % AVK_ENCODE_DPB_SLOTS;
	return true;
}
