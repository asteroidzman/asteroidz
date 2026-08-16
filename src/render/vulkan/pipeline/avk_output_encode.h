#ifndef AVK_OUTPUT_ENCODE_H
#define AVK_OUTPUT_ENCODE_H

#include <pixman.h>

#include "avk_pipeline.h"
#include "../command/avk_retire.h"

/* The LUT upload rides the renderer's shared upload ring; only the pointer is
 * needed here, so the definition stays where it belongs. */
struct avk_cmd_ring;

/*
 * THE OUTPUT-ENCODE PASS. Contract C6, ADR-008.
 *
 * The Path-B half of M5's colour architecture, and the reason Path B is not
 * optional: Vulkan has no sRGB variant of a 10-bit or a half-float format, so
 * the `_SRGB` attachment view Path A encodes through CANNOT EXIST for any
 * 10-bit or HDR output (F11). Every one of those needs a working intermediate
 * and a real encode pass, which is this.
 *
 * ── WHAT IT IS, MECHANICALLY ─────────────────────────────────────────────
 *
 * One fullscreen triangle per damage rectangle, sampling a scene-linear FP16
 * intermediate and writing the scanout buffer through output_encode.frag. The
 * shader carries every decision (see the comment at the top of it); this file
 * carries a pipeline layout, a specialisation constant and a scissor loop, and
 * deliberately nothing else.
 *
 * ── THE PIPELINE IS KEYED ON (target format, transfer function) ──────────
 *
 * Dynamic rendering bakes the colour-attachment format into the pipeline, and
 * the encode curve is a specialisation constant so that an SDR output's shader
 * does not merely skip the PQ branch -- it does not contain one. That makes
 * two axes, both known when an output's colour state is derived and neither
 * changing within a frame, so the variants are built lazily and kept.
 *
 * ── ITS OWN PIPELINE LAYOUT, SHARING ONE DESCRIPTOR SET LAYOUT ───────────
 *
 * The push block is 64 bytes and is NOT push.glsl's: that block is exactly the
 * 128-byte guaranteed minimum and already dual-purpose across four pipelines,
 * with no room for a 3x3 matrix and four scalars. But the descriptor set
 * layout IS avk_pipelines' texture set layout, borrowed rather than
 * redeclared, so the intermediate's sampler descriptor comes from
 * avk_pipelines_texture_set() and is cached on the image exactly like every
 * other sampled image in the renderer. That is what makes C6's "no descriptor
 * set creation per frame" true by construction rather than by discipline.
 */

/* Pipelines are built per (format, tf). Two scanout formats and FOUR curves --
 * M6C makes it four -- is the whole space this renderer can reach, so 8 of
 * these 12 slots is the ceiling; the rest is so that a mixed multi-output
 * desktop cannot silently start recompiling on the frame path. */
#define AVK_ENCODE_MAX_VARIANTS 12

/*
 * Which curve a variant encodes with. An enum rather than the `bool pq` this
 * replaces, because M6B adds a third and ADR-000's HLG would be a fourth --
 * the same reason az_tf is an enum and not `is_hdr`.
 *
 * These values ARE the specialisation constant; they must match the
 * AZ_ENCODE_TF_* defines in shader/src/output_encode.frag.
 */
enum avk_encode_tf {
	AVK_ENCODE_TF_SRGB = 0,
	AVK_ENCODE_TF_PQ = 1,
	AVK_ENCODE_TF_LUT1D = 2,
	/*
	 * M6C. The one variant that is a different SHADER MODULE and not only a
	 * different specialisation: its set-1 declaration is a sampler3D. See
	 * output_encode.frag for why a fourth branch in one module would be
	 * VUID-vkCmdDraw-viewType-07752 rather than a tidier design.
	 */
	AVK_ENCODE_TF_CLUT3D = 3,
};

/* Taps in the measured curve. Must match AZ_ICC_CURVE_TAPS and the shader's
 * AZ_ENCODE_LUT_TAPS; a mismatch is a silently skewed curve, not an error. */
#define AVK_ENCODE_LUT_TAPS 256

/*
 * ONE OUTPUT'S ENCODE CONSTANTS, in the order ADR-008 applies them.
 *
 * Derived from az_output_color_state (C3) by the caller, NOT here: this file
 * has no compositor types in it, which is what lets the pipeline be built and
 * driven by a test fixture with a device and no compositor.
 *
 * matrix     row-major, y = M x, as everywhere else in this project.
 * knee       scene units. ADR-009 fixes it at 1.0; it is a parameter because
 *            the interaction between a knee of 1.0 and an SDR ceiling of 1.0
 *            is the thing F5 had to be able to discuss.
 * peak       the output's ceiling in scene units. 1.0 on SDR, which makes the
 *            tone map the identity by construction.
 * anchor     ref_nits / 10000. PQ only -- there is nothing to anchor for an
 *            sRGB output, where scene 1.0 IS electrical 1.0.
 * dither_q   peak-to-peak, in ELECTRICAL units: one output code.
 * origin_x/y where this target's top-left sits in the OUTPUT raster, so the
 *            dither pattern is anchored to the display and cannot phase-shift.
 *            0,0 for the whole-output pass; the field exists so a regional one
 *            cannot be written without confronting the question.
 * tf         the encode transfer function, as the specialisation constant.
 * lut        the measured encode curve, when tf is LUT1D. Owned by the caller
 *            and guaranteed live for the frame; NULL for the analytic curves,
 *            where the spec constant compiles the sample out entirely so the
 *            descriptor is never statically used.
 * clut       M6C's cube, when tf is CLUT3D, on the same terms. A SEPARATE
 *            field from `lut` and not a union: the two are sampled by two
 *            different modules with two different view types, and a single
 *            slot would make "the wrong one is set" a thing the record path
 *            could not notice.
 * clut_dim   the cube's edge in samples. Zero is refused rather than defaulted,
 *            because a wrong dim is a smoothly wrong picture: the coordinate
 *            mapping is (v*(dim-1) + 0.5)/dim, so a dim that disagrees with the
 *            image reads the table at a steadily drifting offset.
 * clut_encoded_domain  AZ_BREAK_CLUT_DOMAIN, plumbed from the object that read
 *            the environment (off the frame path) rather than read here. Makes
 *            the pass sample the cube with the ENCODED value instead of the
 *            linear one -- the domain error this whole path is at risk of.
 */
struct avk_encode_params {
	float matrix[9];
	float knee;
	float peak;
	float anchor;
	float dither_q;
	float origin_x, origin_y;
	enum avk_encode_tf tf;
	struct avk_image *lut;
	struct avk_image *clut;
	uint32_t clut_dim;
	bool clut_encoded_domain;
};

/* Must match the block in shader/src/output_encode.frag exactly. */
struct avk_encode_push {
	float row0[4]; /* xyz matrix row 0, w knee   */
	float row1[4]; /* xyz matrix row 1, w peak   */
	float row2[4]; /* xyz matrix row 2, w anchor */
	float misc[4]; /* x dither quantum, yz origin, w the cLUT edge (negated
	                * under the domain break) */
};
_Static_assert(sizeof(struct avk_encode_push) == 64,
	"the encode push block must match output_encode.frag");

struct avk_encode_variant {
	VkFormat format;
	enum avk_encode_tf tf;
	VkPipeline pipeline;
};

struct avk_output_encode {
	struct avk_device *dev;
	VkPipelineLayout layout;
	VkShaderModule vert, frag;
	/* M6C: the same source with AZ_ENCODE_CLUT defined. One pipeline layout
	 * serves both -- a descriptor set layout carries a descriptor TYPE and not
	 * an image dimensionality, so a combined image sampler is a combined image
	 * sampler whether the view behind it is 2D or 3D. */
	VkShaderModule frag_clut;

	/*
	 * SET 1 -- THE MEASURED CURVE -- OWNS NOTHING HERE, deliberately.
	 *
	 * Its layout is the renderer's texture set layout, the same one set 0
	 * borrows: one binding of a combined image sampler is exactly a LUT, so its
	 * descriptor comes from avk_pipelines_texture_set() and is cached on the
	 * LUT image like every other sampled image in the renderer.
	 *
	 * A set OWNED BY THE PASS was the first design and it is wrong: with two
	 * profiled outputs it has to be re-pointed between frames, and that is a
	 * descriptor update on a set a pending command buffer still holds.
	 */
	struct avk_encode_variant variants[AVK_ENCODE_MAX_VARIANTS];
	uint32_t variant_len;
	/*
	 * Pipeline compiles, cumulative and never reset.
	 *
	 * A pipeline compile on the frame path is a stall of milliseconds, and this
	 * is the only instrument that can tell "the variant was already there" from
	 * "it was built again". It must stop moving after the first frame of each
	 * output; a counter that keeps climbing is a keying bug, not a warm-up.
	 */
	uint64_t compiles;
	uint64_t compile_failures;
};

/*
 * `set_layout` is avk_pipelines.texture_set_layout -- borrowed, not owned, and
 * not destroyed here.
 */
bool avk_output_encode_init(struct avk_output_encode *enc,
	struct avk_device *dev, VkDescriptorSetLayout set_layout);
void avk_output_encode_finish(struct avk_output_encode *enc);

/*
 * The pipeline for this (format, curve), built on first use.
 *
 * VK_NULL_HANDLE if it could not be built or the table is full, in which case
 * the caller must NOT fall back to writing the intermediate's values straight
 * out -- those are scene-linear and would be presented as though they were
 * electrical. The frame is refused instead; see avk_render.c.
 */
VkPipeline avk_output_encode_pipeline(struct avk_output_encode *enc,
	VkFormat format, enum avk_encode_tf tf);

/*
 * ONE ENCODE PASS, as a graph pass record callback.
 *
 * `damage` is in OUTPUT pixels and is the same region the composition segment
 * was scissored to. Every rectangle is one draw of the fullscreen triangle;
 * the attachment is LOADed, so pixels outside it keep the previous frame --
 * which is the whole reason the pass can be damage-scissored at all.
 */
struct avk_encode_pass {
	struct avk_output_encode *enc;
	struct avk_pipelines *pipes;

	struct avk_image *src; /* the scene-linear intermediate */
	VkImageView dst_view;
	/* The layout the GRAPH left the scan-out buffer in: GENERAL for a foreign
	 * image, COLOR_ATTACHMENT_OPTIMAL for one of our own. Taken from the image
	 * rather than assumed, exactly as the composition segment does. */
	VkImageLayout dst_layout;
	uint32_t width, height;

	VkPipeline pipeline;
	struct avk_encode_params params;
	const pixman_region32_t *damage;

	/* Counted where the draw happens, so "the encode ran" is a reading rather
	 * than an inference from a pixel difference. Optional. */
	uint64_t *draws;
	uint64_t *px;
};

void avk_output_encode_record(VkCommandBuffer cb, void *user);

/*
 * ── THE WORKING INTERMEDIATE ──────────────────────────────────────────────
 *
 * PERSISTENT AND PER OUTPUT, not a pooled transient, and the reason is damage.
 *
 * A transient is recycled the moment its frame retires and its backing extent
 * is rounded up to the pool's granularity. Both are wrong here: the encode
 * pass re-encodes only the damaged region, so the intermediate must still hold
 * the previous frame's composite everywhere else, and the fullscreen triangle
 * maps [0,1] of the attachment onto [0,1] of the source, which is only the
 * same rectangle when the two extents are equal.
 *
 * So it is owned by the output, exactly like the M4I blur cache, and replaced
 * through the retire queue at its own last use when the output's shape or
 * format changes.
 */
struct avk_encode_intermediate {
	struct avk_image *image;
	uint64_t image_bytes; /* VkMemoryRequirements::size */
};

struct avk_image *avk_encode_intermediate_get(struct avk_encode_intermediate *w,
	struct avk_device *dev, struct avk_retire_queue *retire, VkFormat format,
	uint32_t width, uint32_t height);

/*
 * ── THE MEASURED CURVE, AS A TEXTURE ──────────────────────────────────────
 *
 * M6B/G2. One 256x1 RGBA16-UNORM image per output holding az_icc_shaper's
 * curve: R, G and B carry the three channels' taps, which is why the shader
 * samples one texture three times taking a different component each time
 * rather than sampling three textures.
 *
 * PER OUTPUT, because a display profile is, and written ONLY when the profile
 * changes. `serial` is the compositor's revision of the shaper and the whole
 * of the validity test: a curve is not a picture, it has no damage and no
 * geometry, and re-uploading one per frame would be 2KB of pure waste on the
 * path M5 spent a milestone keeping clear.
 *
 * SIXTEEN BITS, not the output's eight. The pass dithers against ONE output
 * code, so a curve quantised to the output's own depth would put the table's
 * resolution at the dither's amplitude -- noise around a staircase, rather
 * than noise that resolves a staircase.
 */
struct avk_encode_lut {
	struct avk_image *image;
	uint64_t serial; /* the shaper revision `image` holds; 0 = empty */
};

/*
 * The LUT image for `curve`, uploaded if `serial` differs from what is held.
 *
 * NULL if it could not be built or uploaded, and a caller must read that as
 * "do not encode with LUT1D this frame" rather than "encode without a curve":
 * the LUT1D variant samples a descriptor that would then be unwritten, which
 * is undefined behaviour and not a missing correction.
 */
struct avk_image *avk_encode_lut_get(struct avk_encode_lut *l,
	struct avk_device *dev, struct avk_cmd_ring *ring,
	struct avk_retire_queue *retire,
	const uint16_t curve[3][AVK_ENCODE_LUT_TAPS], uint64_t serial);

/* Same retire contract as the intermediate: NULL `retire` only from teardown,
 * with the device idle. */
void avk_encode_lut_finish(struct avk_encode_lut *l, struct avk_device *dev,
	struct avk_retire_queue *retire);

/*
 * ── M6C: THE cLUT, AS A VOLUME ────────────────────────────────────────────
 *
 * One dim x dim x dim RGBA16-UNORM image per output holding az_icc_clut's grid,
 * sampled trilinearly. Everything the 1D curve's object says applies here word
 * for word -- per output because a display profile is; written only when the
 * profile's serial moves because a table is not a picture; sixteen bits because
 * the pass dithers against one output code -- so the differences are the only
 * things worth stating:
 *
 * R16G16B16A16_UNORM AND NOT R32G32B32A32_SFLOAT, which is what wlroots uses.
 * The cube MUST be filtered linearly (trilinear interpolation is the whole
 * mechanism, not an optimisation), and Vulkan requires
 * SAMPLED_IMAGE_FILTER_LINEAR of the 16-bit UNORM format and does NOT require
 * it of the 32-bit float one. Choosing the format that is guaranteed to filter
 * is choosing the one that cannot silently fall back to point sampling on some
 * other GPU -- which would look like mild banding rather than like a bug.
 *
 * 65^3 x 8 bytes is 2.1MB per profiled output, once.
 *
 * `dim` is held alongside the image because the shader's coordinate mapping
 * needs it and reading it back off the image would be reading it off
 * `extent.width`, which is the same number for the wrong reason.
 */
struct avk_encode_clut {
	struct avk_image *image;
	uint64_t serial; /* the profile revision `image` holds; 0 = empty */
	uint32_t dim;
	/* AZ_BREAK_CLUT_DOMAIN, latched when the table was built so that the frame
	 * path never reads the environment. The pass reads it from the params. */
	bool domain_break;
};

/*
 * The cube image for `grid`, uploaded if `serial` differs from what is held.
 *
 * `grid` is 3 * dim^3 samples, R varying fastest -- az_icc_clut's layout, which
 * is a buffer-to-3D-image copy's layout. NULL on failure, with the same reading
 * as the 1D curve's: "do not encode with CLUT3D this frame", never "encode
 * without a table".
 */
struct avk_image *avk_encode_clut_get(struct avk_encode_clut *l,
	struct avk_device *dev, struct avk_cmd_ring *ring,
	struct avk_retire_queue *retire, const uint16_t *grid, uint32_t dim,
	uint64_t serial);

void avk_encode_clut_finish(struct avk_encode_clut *l, struct avk_device *dev,
	struct avk_retire_queue *retire);

/* With `retire` non-NULL the image is destroyed once the GPU passes its own
 * last use; with it NULL, immediately -- which is only correct from teardown,
 * after the device is idle. */
void avk_encode_intermediate_finish(struct avk_encode_intermediate *w,
	struct avk_device *dev, struct avk_retire_queue *retire);

#endif /* AVK_OUTPUT_ENCODE_H */
