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
 * standard's reference values -- which is what an external recorder has to
 * fall back on, because IPC never exposed them.
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

/*
 * The staging format the conversion reads, which is 10-bit because P010 is.
 * A source in this format is copied into it; anything else the encoder accepts
 * is blitted, which converts rather than reinterprets. See convert_to_p010.
 */
#define AVK_ENCODE_RGB_FORMAT VK_FORMAT_A2R10G10B10_UNORM_PACK32

/*
 * The picture formats the encoder will take. Exactly the two AVK composites
 * an output into: 10-bit for an HDR output or a 10-bit SDR one, 8-bit for an
 * ordinary SDR one. Asked before a recording opens rather than discovered one
 * dropped frame at a time.
 */
static inline bool avk_encode_source_supported(VkFormat f) {
	return f == AVK_ENCODE_RGB_FORMAT || f == VK_FORMAT_B8G8R8A8_UNORM;
}

enum avk_encode_colour {
	AVK_ENCODE_COLOUR_SDR,     /* BT.709 primaries, sRGB transfer, BT.709 matrix */
	AVK_ENCODE_COLOUR_HDR10,   /* BT.2020 primaries, PQ transfer, BT.2020 NCL */
};

/* Two: one being written, one being referenced. A longer GOP would want more,
 * but nothing here predicts from further back than the previous picture. */
#define AVK_ENCODE_DPB_SLOTS 2

/* How often a sequence starts again: every 60th SUBMITTED picture is an IDR,
 * so no picture depends on a chain longer than 59. In pictures rather than
 * seconds because that is what a seek costs and what a lost picture destroys;
 * the capture rate is a cap, not a cadence. */
#define AVK_ENCODE_IDR_PERIOD 60

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
	/*
	 * The DPB, as ONE image with two array layers rather than two images.
	 *
	 * A still needs a single reconstructed picture; a sequence needs the
	 * previous one kept while the next is written, so the two alternate. One
	 * allocation with baseArrayLayer selecting between them is how the video
	 * extensions expect a DPB to be laid out, and it means the slot index and
	 * the layer index are the same number -- which removes the mapping most
	 * likely to be got wrong.
	 */
	VkImage dpb_image;
	VkDeviceMemory dpb_memory;
	VkImageView dpb_view[AVK_ENCODE_DPB_SLOTS];

	/*
	 * Where in the sequence we are. A still resets these and never looks at
	 * them again; a recording is entirely made of them.
	 *
	 * `frame_index` counts pictures, `poc` is the H.265 picture order count
	 * the decoder reconstructs display order from, and `dpb_slot` is the
	 * layer the NEXT picture will be reconstructed into -- so the one before
	 * it, which the next P picture references, is the other one.
	 */
	/* How long the last picture spent in each of the two GPU waits. Kept
	 * because "recording costs frame time" is a certainty and the split
	 * between these is what decides which wait is worth removing first. */
	int64_t last_convert_ns;
	int64_t last_encode_ns;
	/* What was still left to wait for when the picture was collected, a whole
	 * frame after it was submitted. Near zero means the encode overlapped the
	 * frame completely, which is the point of collecting late. */
	int64_t last_collect_ns;
	/* A picture is in flight on the encode queue and its bitstream has not
	 * been taken yet. Nothing may reuse the P010 image or the bitstream
	 * buffer until it has. */
	bool submitted;
	uint32_t last_qp;

	uint64_t frame_index;
	int32_t poc;
	uint32_t dpb_slot;
	bool session_reset;   /* the RESET control is a once-per-session thing */

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
	/* An owned RGB copy of whatever the caller hands over. A scanout image is
	 * a colour attachment and does not carry STORAGE usage, so the conversion
	 * cannot read it directly; one GPU-to-GPU copy into an image that does is
	 * cheaper than requiring every caller to allocate differently, and keeps
	 * the picture off the CPU either way. */
	VkImage rgb_image;
	VkDeviceMemory rgb_memory;
	VkImageView rgb_view;

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
 * `src` must be VK_FORMAT_A2R10G10B10_UNORM_PACK32 and created with
 * VK_IMAGE_USAGE_TRANSFER_SRC_BIT -- which a scanout target already is.
 * `src_layout` is whatever it is in now; it is restored before returning,
 * because a caller's image is not the encoder's to leave in a different state
 * than it found it.
 *
 * (src_x, src_y) is the top-left of the region to encode, and the region's
 * size is the encoder's own -- so a selection is captured by building an
 * encoder the size of the selection rather than by encoding the screen and
 * cropping afterwards, which would encode ten times the pixels to throw most
 * of them away.
 *
 * On success *out is a malloc'd Annex B bitstream -- parameter sets, the HDR10
 * SEI if the metadata is known, then the coded picture -- and the caller owns
 * it.
 */
bool avk_encoder_encode_still(struct avk_encoder *enc, VkImage src,
	VkImageLayout src_layout, VkFormat src_format, int32_t src_x,
	int32_t src_y, void **out, size_t *out_len);

/*
 * One picture of a SEQUENCE.
 *
 * The first is an IDR and carries the parameter sets; the rest are P pictures
 * predicting from the one before, and carry only themselves. So the caller
 * must keep every packet in order -- unlike a still, a later frame is not a
 * file on its own and cannot be decoded without the ones before it.
 *
 * `force_key` starts a new IDR mid-sequence, which is what makes a recording
 * seekable and what a dropped frame recovers through.
 */
/*
 * Submit one picture, and return the PREVIOUS one.
 *
 * Pipelined deliberately: the encode is ~21ms of GPU time for a 4K picture and
 * waiting for it made the compositor's frames three times later than its own
 * budget. So a call submits this frame and hands back the frame before it,
 * which has had the whole gap between the two to finish.
 *
 * *out is therefore NULL on the first call of a sequence, and one picture is
 * always outstanding -- `avk_encoder_drain` takes it. A caller that forgets is
 * a caller whose recording is one frame short.
 */
bool avk_encoder_encode_frame(struct avk_encoder *enc, VkImage src,
	VkImageLayout src_layout, VkFormat src_format, int32_t src_x,
	int32_t src_y, bool force_key, void **out, size_t *out_len);

/* Take the last submitted picture, if one is outstanding. */
bool avk_encoder_drain(struct avk_encoder *enc, void **out, size_t *out_len);

/* Forget the sequence: the next picture is an IDR again. */
void avk_encoder_reset_sequence(struct avk_encoder *enc);

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
