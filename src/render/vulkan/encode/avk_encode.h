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
	VkVideoEncodeProfileRgbConversionInfoVALVE rgb_profile;
	VkVideoProfileListInfoKHR profile_list;

	/* What the driver said it could do, kept for the same reason. */
	VkVideoCapabilitiesKHR caps;
	VkVideoEncodeCapabilitiesKHR encode_caps;
	VkVideoEncodeH265CapabilitiesKHR h265_caps;
	VkVideoEncodeRgbConversionCapabilitiesVALVE rgb_caps;

	VkFormat dpb_format;
	VkFormat src_format;
};

/*
 * Create an encoder for one output size. Returns NULL and logs the reason if
 * the device cannot encode, the driver rejects the profile, or the size is
 * outside what the codec allows.
 */
struct avk_encoder *avk_encoder_create(struct avk_device *dev,
	uint32_t width, uint32_t height);

void avk_encoder_destroy(struct avk_encoder *enc);

#endif /* AVK_ENCODE_H */
