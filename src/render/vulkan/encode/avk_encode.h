/*
 * avk_encode -- H.265 Main 10 encoding of AVK's own composited images.
 *
 * M14A. The compositor is the only thing on this machine that already holds
 * the finished frame on the GPU in the format an HDR encoder wants, and every
 * other capture path is trying to obtain a copy of it: the portal flattens to
 * 8-bit, gpu-screen-recorder's raw-KMS path races explicit sync, and
 * screenshot_ui freezes the output for a full readback per still. Encoding
 * here removes the copy rather than optimising it.
 *
 * ── WHY THE RGB CONVERSION EXTENSION IS LOAD-BEARING ──────────────────────
 *
 * Asked plainly, this driver's encoder accepts P010 -- a 10-bit YUV plane pair
 * -- and nothing else, which would make an RGB->YUV pass the compositor's to
 * write. Asked with VkVideoEncodeProfileRgbConversionInfoVALVE chained into
 * the PROFILE, the same query answers A2R10G10B10, which is exactly
 * DRM_FORMAT_XRGB2101010: the format AVK already renders an HDR output in.
 *
 * That is a property of the profile, not a flag on the session. An encoder
 * that converts is a different video profile from one that does not, and the
 * two answer the format query differently -- so the profile built here is
 * threaded through session creation, the format queries and the DPB, and
 * changing it in one place without the others produces a session that is
 * created successfully and then rejects every picture.
 *
 * ── WHAT THIS IS NOT, YET ─────────────────────────────────────────────────
 *
 * One key frame. No P or B pictures, no reference management beyond the single
 * reconstructed picture the driver insists on, no rate control (fixed QP), no
 * container. That is M14A's whole scope: a still. The video half needs a DPB
 * with real reference tracking and a rate-control mode, and both are easier to
 * add to something that already produces a correct IDR than to debug together.
 */
#ifndef AVK_ENCODE_H
#define AVK_ENCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

struct avk_device;

/*
 * What the picture's colour actually is, which decides two things that must
 * agree: the matrix the conversion applies, and the colour description written
 * into the stream. A player reading the second while the first was different
 * shows a picture that is wrong in a way nobody reports as a bug.
 */
/*
 * HDR10 static metadata: what the DISPLAY can do, not what the picture
 * contains. A tone-mapping player uses it to decide how to fit the second into
 * the first, so the numbers have to come from the monitor rather than from a
 * standard's reference values -- which is exactly what contrib/hdr-record.sh
 * has to fall back on today, because IPC never exposed them.
 *
 * Units are the ones the SEI itself uses: chromaticity in 0.00002 increments,
 * luminance in 0.0001 cd/m2. Everything zero means unknown, and then nothing
 * is written.
 */
struct avk_encode_mastering {
	uint16_t display_primaries_x[3];   /* G, B, R -- the SEI's own order */
	uint16_t display_primaries_y[3];
	uint16_t white_point_x, white_point_y;
	uint32_t max_luminance, min_luminance;
	uint16_t max_content_light_level;
	uint16_t max_frame_average_light_level;
};

enum avk_encode_colour {
	AVK_ENCODE_COLOUR_SDR,     /* BT.709 primaries, sRGB transfer, BT.709 matrix */
	AVK_ENCODE_COLOUR_HDR10,   /* BT.2020 primaries, PQ transfer, BT.2020 NCL */
};

struct avk_encoder {
	struct avk_device *dev;   /* borrowed */

	/* The coded size, which is the requested size rounded up to the
	 * encoder's picture alignment. On this display they are equal --
	 * 3840x2160 lands exactly on 64x16 -- but a 1366x768 laptop panel does
	 * not, and a coded size that silently differs from the source extent is
	 * how an encoder produces a picture with a green strip down one edge. */
	uint32_t width, height;
	uint32_t coded_width, coded_height;

	VkVideoSessionKHR session;
	VkVideoSessionParametersKHR params;
	VkDeviceMemory *session_memory;
	uint32_t session_memory_count;

	/* The profile, kept because nearly every later call needs it back and
	 * rebuilding it at each site is how the RGB conversion gets left off one
	 * of them. */
	VkVideoProfileInfoKHR profile;
	VkVideoEncodeH265ProfileInfoKHR h265_profile;
	VkVideoProfileListInfoKHR profile_list;

	/* What the driver said it could do, kept for the same reason. */
	VkVideoCapabilitiesKHR caps;
	VkVideoEncodeCapabilitiesKHR encode_caps;
	VkVideoEncodeH265CapabilitiesKHR h265_caps;

	VkFormat dpb_format;
	VkFormat src_format;
	StdVideoH265LevelIdc level_idc;
	enum avk_encode_colour colour;
	/* HDR10 static metadata, as the display reported it. Zero means "not
	 * known", and then no SEI is written at all -- an SEI full of guessed
	 * numbers is worse than its absence, because a player believes it. */
	struct avk_encode_mastering mastering;

	/* The reconstructed picture. The encoder writes it whether or not
	 * anything will reference it, so a still needs one even though it has no
	 * second frame to predict from. */
	VkImage dpb_image;
	VkDeviceMemory dpb_memory;
	VkImageView dpb_view;

	/* Where the bitstream lands, and how the driver reports how much of it it
	 * used. The buffer size is a guess by nature -- an encoder is not obliged
	 * to tell you in advance -- so it is generous and the query says what was
	 * actually written. */
	VkBuffer bitstream;
	VkDeviceMemory bitstream_memory;
	VkDeviceSize bitstream_size;
	VkQueryPool feedback;

	VkCommandPool cmd_pool;
	VkCommandBuffer cmd;
	VkFence fence;

	/* The encoder's own input picture, in the only format it will accept.
	 * The caller's RGB image is converted into this rather than handed over
	 * -- see the shader for why the driver's own conversion is not used. */
	VkImage p010_image;
	VkDeviceMemory p010_memory;
	VkImageView p010_view;      /* whole image, for the encoder */
	VkImageView p010_y_view;    /* plane 0 as R16, for the compute write */
	VkImageView p010_uv_view;   /* plane 1 as R16G16, likewise */

	VkDescriptorSetLayout conv_set_layout;
	VkDescriptorPool conv_pool;
	VkDescriptorSet conv_set;
	VkPipelineLayout conv_pipeline_layout;
	VkPipeline conv_pipeline;
	VkCommandPool conv_cmd_pool;   /* on the graphics family */
	VkCommandBuffer conv_cmd;
	VkFence conv_fence;
};

/*
 * Encode one image as a single IDR picture.
 *
 * `src_view` must be a VK_FORMAT_A2R10G10B10_UNORM_PACK32 view of an image at
 * the encoder's coded size, created with VK_IMAGE_USAGE_STORAGE_BIT and
 * readable by the graphics/compute family. It is converted to P010 by a
 * compute pass and the result is what the encoder reads, so the source needs
 * no video usage flags and no profile in its pNext.
 *
 * On success *out is a malloc'd Annex B bitstream -- parameter sets followed
 * by the coded picture -- and the caller owns it.
 */
bool avk_encoder_encode_still(struct avk_encoder *enc, VkImage src,
	VkImageView src_view, VkImageLayout src_layout,
	void **out, size_t *out_len);

/*
 * Create an encoder for one output size. Returns NULL and logs the reason if
 * the device cannot encode, the driver rejects the profile, or the size is
 * outside what the codec allows.
 */
struct avk_encoder *avk_encoder_create(struct avk_device *dev,
	uint32_t width, uint32_t height, enum avk_encode_colour colour,
	const struct avk_encode_mastering *mastering);

void avk_encoder_destroy(struct avk_encoder *enc);

#endif /* AVK_ENCODE_H */
