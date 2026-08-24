#define _POSIX_C_SOURCE 200809L

#include "avk_output_encode.h"

#include <drm_fourcc.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "../avk.h"
#include "../device/avk_device.h"
#include "../image/avk_image.h"
#include "../image/avk_upload.h"

#include "output_encode_vert.spv.h"
#include "output_encode_frag.spv.h"
#include "output_encode_clut_frag.spv.h"

/* For object names and log lines only. */
static const char *encode_tf_name(enum avk_encode_tf tf) {
	switch (tf) {
	case AVK_ENCODE_TF_SRGB:   return "srgb";
	case AVK_ENCODE_TF_PQ:     return "pq";
	case AVK_ENCODE_TF_LUT1D:  return "lut1d";
	case AVK_ENCODE_TF_CLUT3D: return "clut3d";
	}
	return "?";
}

static VkShaderModule encode_module(struct avk_device *dev,
		const uint32_t *code, size_t size, const char *name) {
	VkShaderModuleCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = code,
	};
	VkShaderModule module = VK_NULL_HANDLE;
	VkResult res = vkCreateShaderModule(dev->dev, &info, NULL, &module);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateShaderModule (output encode)");
		return VK_NULL_HANDLE;
	}
	avk_device_name_object(dev, VK_OBJECT_TYPE_SHADER_MODULE,
		(uint64_t)module, "avk shader %s", name);
	return module;
}

bool avk_output_encode_init(struct avk_output_encode *enc,
		struct avk_device *dev, VkDescriptorSetLayout set_layout) {
	memset(enc, 0, sizeof(*enc));
	enc->dev = dev;

	/*
	 * ITS OWN LAYOUT, 64 bytes, VERTEX | FRAGMENT.
	 *
	 * The vertex stage does not read the block -- its position is a function of
	 * gl_VertexIndex alone. The range covers it anyway because a pipeline layout
	 * whose push range excludes a stage is a layout that cannot later be given
	 * one without every pipeline built from it being recreated, and the cost of
	 * the wider range is exactly zero.
	 */
	VkPushConstantRange range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT
			| VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(struct avk_encode_push),
	};
	/*
	 * ── SET 1 IS THE MEASURED CURVE, AND IT IS THE SAME LAYOUT AS SET 0 ───
	 *
	 * Declared for every variant and sampled by one, and every variant BINDS it
	 * -- see the record function for why "the spec constant eliminates the
	 * sample, so it needs no binding" is wrong and what validation said about it.
	 * Declaring it uniformly keeps ONE pipeline layout rather than two that must
	 * be kept in step.
	 *
	 * The renderer's texture set layout again, rather than a layout this pass
	 * declares -- one binding of a combined image sampler is exactly what a LUT
	 * needs, and reusing it means the LUT's descriptor comes from
	 * avk_pipelines_texture_set() like every other sampled image: allocated
	 * once, cached on the image, written once, never rewritten. A set this pass
	 * owned and re-pointed per output would be a descriptor update on a set a
	 * pending command buffer still holds. See the record function.
	 */
	VkDescriptorSetLayout sets[2] = { set_layout, set_layout };
	VkPipelineLayoutCreateInfo li = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 2,
		.pSetLayouts = sets,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &range,
	};
	if (!avk_check(vkCreatePipelineLayout(dev->dev, &li, NULL, &enc->layout),
			"vkCreatePipelineLayout (output encode)")) {
		return false;
	}
	enc->vert = encode_module(dev, output_encode_vert_spv,
		sizeof(output_encode_vert_spv), "output_encode.vert");
	enc->frag = encode_module(dev, output_encode_frag_spv,
		sizeof(output_encode_frag_spv), "output_encode.frag");
	enc->frag_clut = encode_module(dev, output_encode_clut_frag_spv,
		sizeof(output_encode_clut_frag_spv), "output_encode.frag [clut]");
	if (enc->vert == VK_NULL_HANDLE || enc->frag == VK_NULL_HANDLE
			|| enc->frag_clut == VK_NULL_HANDLE) {
		avk_output_encode_finish(enc);
		return false;
	}
	return true;
}

void avk_output_encode_finish(struct avk_output_encode *enc) {
	if (enc == NULL || enc->dev == NULL) {
		return;
	}
	struct avk_device *dev = enc->dev;
	for (uint32_t i = 0; i < enc->variant_len; i++) {
		if (enc->variants[i].pipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(dev->dev, enc->variants[i].pipeline, NULL);
			AVK_LIVE_DEC(dev, pipelines);
		}
	}
	enc->variant_len = 0;
	/* Nothing to tear down for set 1: its layout is the renderer's, its
	 * sampler is the renderer's, and its descriptors live on the LUT images
	 * and die with them (avk_encode_lut_finish). */
	if (enc->vert != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, enc->vert, NULL);
		enc->vert = VK_NULL_HANDLE;
	}
	if (enc->frag != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, enc->frag, NULL);
		enc->frag = VK_NULL_HANDLE;
	}
	if (enc->frag_clut != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, enc->frag_clut, NULL);
		enc->frag_clut = VK_NULL_HANDLE;
	}
	if (enc->layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(dev->dev, enc->layout, NULL);
		enc->layout = VK_NULL_HANDLE;
	}
	enc->dev = NULL;
}

static VkPipeline encode_build(struct avk_output_encode *enc, VkFormat format,
		enum avk_encode_tf tf_in) {
	struct avk_device *dev = enc->dev;

	/* AZ_ENCODE_TF, constant_id 0. The other curve is not compiled in at all,
	 * which is the point: "there is no PQ encode in an SDR pipeline" is a
	 * stronger statement than "the branch is not taken". */
	int32_t tf = (int32_t)tf_in;
	VkSpecializationMapEntry entry = {
		.constantID = 0,
		.offset = 0,
		.size = sizeof(tf),
	};
	VkSpecializationInfo spec = {
		.mapEntryCount = 1,
		.pMapEntries = &entry,
		.dataSize = sizeof(tf),
		.pData = &tf,
	};
	VkPipelineShaderStageCreateInfo stages[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = enc->vert,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			/*
			 * M6C is the one variant that changes the MODULE. The spec constant
			 * is still supplied -- the module still declares it -- and is still
			 * the honest value, so a dump of this pipeline says CLUT3D rather
			 * than leaving a 0 that reads as sRGB.
			 */
			.module = tf_in == AVK_ENCODE_TF_CLUT3D ? enc->frag_clut
				: enc->frag,
			.pName = "main",
			.pSpecializationInfo = &spec,
		},
	};

	VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	/* A LIST, not a strip: this is one oversized triangle, drawn with nothing
	 * bound, and a strip topology would be a lie about a three-vertex draw. */
	VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo viewport = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	/*
	 * NO BLENDING. Composition finished in the intermediate; this pass
	 * REPLACES. Blending here would mix an electrical value with whatever the
	 * previous frame left in the scanout buffer -- which on an undamaged pixel
	 * is the same colour, so the defect would be invisible everywhere except
	 * exactly where something changed.
	 */
	VkPipelineColorBlendAttachmentState blend = {
		.blendEnable = VK_FALSE,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
			| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend,
	};
	VkDynamicState dynamic[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};
	VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dynamic,
	};
	VkPipelineRenderingCreateInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = 1,
		.pColorAttachmentFormats = &format,
	};
	VkGraphicsPipelineCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &rendering,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport,
		.pRasterizationState = &raster,
		.pMultisampleState = &multisample,
		.pColorBlendState = &blend_state,
		.pDynamicState = &dynamic_state,
		.layout = enc->layout,
	};

	VkPipeline pipeline = VK_NULL_HANDLE;
	VkResult res = vkCreateGraphicsPipelines(dev->dev, dev->pipeline_cache, 1,
		&info, NULL, &pipeline);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateGraphicsPipelines (output encode)");
		enc->compile_failures++;
		return VK_NULL_HANDLE;
	}
	AVK_LIVE_INC(dev, pipelines);
	enc->compiles++;
	avk_device_name_object(dev, VK_OBJECT_TYPE_PIPELINE, (uint64_t)pipeline,
		"avk pipeline output_encode %s fmt=%d", encode_tf_name(tf_in),
		(int)format);
	return pipeline;
}

VkPipeline avk_output_encode_pipeline(struct avk_output_encode *enc,
		VkFormat format, enum avk_encode_tf tf) {
	if (enc == NULL || enc->dev == NULL || enc->frag == VK_NULL_HANDLE) {
		return VK_NULL_HANDLE;
	}
	for (uint32_t i = 0; i < enc->variant_len; i++) {
		if (enc->variants[i].format == format && enc->variants[i].tf == tf) {
			return enc->variants[i].pipeline;
		}
	}
	if (enc->variant_len >= AVK_ENCODE_MAX_VARIANTS) {
		/*
		 * Loud, and then nothing. Evicting one would put a pipeline compile on
		 * the frame path at whatever cadence the outputs alternate at, which is
		 * a millisecond-scale stall repeating forever -- worse than a refused
		 * frame, and much harder to recognise.
		 */
		avk_log(AVK_ERROR, "avk encode: %d pipeline variants already built; "
			"no room for format %d %s", AVK_ENCODE_MAX_VARIANTS, (int)format,
			encode_tf_name(tf));
		return VK_NULL_HANDLE;
	}
	VkPipeline pipeline = encode_build(enc, format, tf);
	/* Recorded even when it is VK_NULL_HANDLE: a format that cannot compile
	 * must fail once, not once per frame. */
	enc->variants[enc->variant_len++] = (struct avk_encode_variant){
		.format = format,
		.tf = tf,
		.pipeline = pipeline,
	};
	return pipeline;
}

void avk_output_encode_record(VkCommandBuffer cb, void *user) {
	struct avk_encode_pass *p = user;

	VkRenderingAttachmentInfo color = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = p->dst_view,
		/*
		 * The layout the graph put the scan-out buffer in. GENERAL for a
		 * foreign image and COLOR_ATTACHMENT_OPTIMAL for one of our own -- the
		 * same rule the composition segment follows, asked of the image rather
		 * than assumed here.
		 */
		.imageLayout = p->dst_layout,
		/*
		 * LOAD. The pass writes only the damaged rectangles, and every other
		 * pixel of the scanout buffer must survive as the previous frame left
		 * it. DONT_CARE here would be a full-screen flicker on every partial
		 * frame -- which is most of them.
		 */
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	};
	VkRenderingInfo info = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = { { 0, 0 }, { p->width, p->height } },
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color,
	};
	vkCmdBeginRendering(cb, &info);

	VkViewport vp = { 0, 0, (float)p->width, (float)p->height, 0.0f, 1.0f };
	vkCmdSetViewport(cb, 0, 1, &vp);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);

	/*
	 * NEAREST. This is a 1:1 blit of an intermediate whose extent equals the
	 * output's, and linear filtering at 1:1 is not a no-op once a destination
	 * lands on a half pixel -- it is a soft desktop nobody can point at.
	 */
	VkDescriptorSet set = avk_pipelines_texture_set(p->pipes, p->src, false);
	if (set == VK_NULL_HANDLE) {
		vkCmdEndRendering(cb);
		return;
	}
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->enc->layout,
		0, 1, &set, 0, NULL);
	/*
	 * ── SET 1, THE MEASURED CURVE, FROM THE SHARED PER-IMAGE CACHE ────────
	 *
	 * Bound only for the variant that samples it: the analytic variants
	 * eliminate the sample at specialisation, so the descriptor is not
	 * statically used there and binding it would be work with no reader.
	 *
	 * LINEAR, unlike set 0 above, and for the opposite reason. The intermediate
	 * is blitted 1:1 and interpolating it is a soft desktop; the curve is
	 * sampled BETWEEN taps by construction -- that is what a 256-tap table for
	 * a continuous function means -- and taking the nearest tap would quantise
	 * the encode to 256 steps, which is coarser than the output it feeds.
	 *
	 * A SET PER IMAGE, NOT A SET PER PASS, and the difference is a real hazard
	 * rather than a style. The first version of this owned one descriptor set
	 * and rewrote it whenever the LUT differed from the last one bound. With
	 * two profiled outputs that rewrite lands while the other output's frame is
	 * still pending -- vkUpdateDescriptorSets on a set bound to a command
	 * buffer in the pending state, which is undefined behaviour and which
	 * validation reports only when it happens to catch the overlap. The
	 * renderer already solves this for every other sampled image by caching the
	 * set ON the image, where it is written exactly once, and this is one more
	 * sampled image.
	 */
	/*
	 * SET 1 IS BOUND FOR EVERY VARIANT, INCLUDING THE ONES THAT NEVER SAMPLE IT.
	 *
	 * The first version bound it only for LUT1D, on the reasoning that the
	 * specialisation constant eliminates the sample in the analytic variants so
	 * the descriptor is not statically used there. THAT REASONING IS WRONG, and
	 * validation says so directly:
	 *
	 *   VUID-vkCmdDraw-None-08600: the VkPipeline [output_encode srgb]
	 *   statically uses descriptor set 1, but ... not compatible with the
	 *   VkPipelineLayout bound with vkCmdBindDescriptorSets
	 *
	 * "Statically uses" is a property of the SPIR-V, not of what the driver
	 * folds away: the module declares the sampler and references it inside a
	 * branch, and that is a static use whether or not the branch survives
	 * specialisation. So every Path-B frame -- every HDR frame on DP-1 included,
	 * with no profile anywhere in sight -- was drawing with set 1 unbound.
	 *
	 * The fixture was green throughout. It rendered correct pixels because the
	 * driver does eliminate the branch; the pass was illegal regardless.
	 */
	VkDescriptorSet lut_set = set;
	if (p->params.tf == AVK_ENCODE_TF_LUT1D
			|| p->params.tf == AVK_ENCODE_TF_CLUT3D) {
		/*
		 * M6C shares this branch, and shares LINEAR with it for a stronger
		 * reason than the 1D curve has: trilinear interpolation IS how a 65-cube
		 * describes a continuous transform. NEAREST here would quantise the
		 * whole display characterisation to 33 steps per axis, which is visible
		 * banding rather than a rounding error.
		 *
		 * The cube ALSO needs its dim to be non-zero, checked here rather than
		 * defaulted: (v*(dim-1) + 0.5)/dim with the wrong dim reads the table at
		 * a drifting offset, which is a smooth and entirely plausible picture.
		 */
		struct avk_image *table = p->params.tf == AVK_ENCODE_TF_CLUT3D
			? p->params.clut : p->params.lut;
		bool usable = table != NULL && (p->params.tf != AVK_ENCODE_TF_CLUT3D
			|| p->params.clut_dim >= 2);
		lut_set = usable
			? avk_pipelines_texture_set(p->pipes, table, true)
			: VK_NULL_HANDLE;
		if (lut_set == VK_NULL_HANDLE) {
			/* REFUSE THE PASS. The variant's sample would read an unwritten
			 * descriptor; skipping the draw leaves the previous frame on the
			 * screen, which is what every other failure in this pass does and
			 * is the only outcome that is not undefined. */
			vkCmdEndRendering(cb);
			return;
		}
	}
	/* For the analytic variants this is set 0's own set again -- layout
	 * compatible, never sampled, and one vkCmdBindDescriptorSets per pass. A
	 * second pipeline layout would avoid the bind and would have to be kept in
	 * step with this one for the life of the pass; the bind is cheaper in both
	 * senses. */
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
		p->enc->layout, 1, 1, &lut_set, 0, NULL);

	struct avk_encode_push pc = {0};
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			/* Row-major, and unrolled into three vec4s rather than pushed as a
			 * mat3: GLSL's mat3 constructor is column-major, and a transposed
			 * 709->2020 matrix still renders a plausible picture. */
			(r == 0 ? pc.row0 : r == 1 ? pc.row1 : pc.row2)[c] =
				p->params.matrix[r * 3 + c];
		}
	}
	pc.row0[3] = p->params.knee;
	pc.row1[3] = p->params.peak;
	pc.row2[3] = p->params.anchor;
	pc.misc[0] = p->params.dither_q;
	pc.misc[1] = p->params.origin_x;
	pc.misc[2] = p->params.origin_y;
		pc.misc[3] = (float)p->params.clut_dim;
	vkCmdPushConstants(cb, p->enc->layout,
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
		sizeof(pc), &pc);

	/*
	 * ONE DRAW PER DAMAGE RECTANGLE, scissored.
	 *
	 * Damage is a scissor and never geometry: the triangle always covers the
	 * whole viewport, so its interpolated uv is the same function of position
	 * for every rectangle and a two-rectangle region cannot end up sampling two
	 * different parts of the intermediate.
	 */
	int n = 0;
	const pixman_box32_t *boxes = p->damage != NULL
		? pixman_region32_rectangles((pixman_region32_t *)p->damage, &n) : NULL;
	for (int i = 0; i < n; i++) {
		int32_t x0 = boxes[i].x1 < 0 ? 0 : boxes[i].x1;
		int32_t y0 = boxes[i].y1 < 0 ? 0 : boxes[i].y1;
		int32_t x1 = boxes[i].x2 > (int32_t)p->width
			? (int32_t)p->width : boxes[i].x2;
		int32_t y1 = boxes[i].y2 > (int32_t)p->height
			? (int32_t)p->height : boxes[i].y2;
		if (x1 <= x0 || y1 <= y0) {
			continue;
		}
		VkRect2D scissor = {
			{ x0, y0 },
			{ (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) },
		};
		vkCmdSetScissor(cb, 0, 1, &scissor);
		vkCmdDraw(cb, 3, 1, 0, 0);
		if (p->draws != NULL) {
			(*p->draws)++;
		}
		if (p->px != NULL) {
			*p->px += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
		}
	}
	vkCmdEndRendering(cb);
}

/* ── the working intermediate ─────────────────────────────────────────────── */

/*
 * COLOR_ATTACHMENT because the whole scene composites into it; SAMPLED because
 * the encode pass reads it; TRANSFER_SRC because the frame oracle and the
 * capture path read it back, and because -- unlike a pooled transient -- this
 * image is not keyed on its usage, so there is no second allocation for a
 * debug path to diverge into.
 */
#define AVK_ENCODE_INTERMEDIATE_USAGE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT \
	| VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)

static struct avk_image *intermediate_create(struct avk_device *dev,
		VkFormat format, uint32_t width, uint32_t height,
		VkDeviceSize *out_bytes) {
	struct avk_image *image = avk_image_alloc(dev);
	if (image == NULL) {
		return NULL;
	}
	image->format = format;
	image->extent = (VkExtent2D){ width, height };
	image->plane_count = 1;
	image->has_alpha = true;
	image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
	/* Owned: nothing outside this device can see it, so the graph must not try
	 * to acquire it from VK_QUEUE_FAMILY_FOREIGN_EXT. */
	image->origin = AVK_IMAGE_OWNED;

	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = AVK_ENCODE_INTERMEDIATE_USAGE,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (!avk_check(vkCreateImage(dev->dev, &info, NULL, &image->image),
			"vkCreateImage (encode intermediate)")) {
		free(image);
		return NULL;
	}
	AVK_LIVE_INC(dev, images);

	VkMemoryRequirements reqs;
	vkGetImageMemoryRequirements(dev->dev, image->image, &reqs);
	uint32_t type = 0;
	if (!avk_find_memory_type(dev, reqs.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
		avk_log(AVK_ERROR, "avk encode: no device-local memory type");
		avk_image_destroy(dev, image);
		return NULL;
	}
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = reqs.size,
		.memoryTypeIndex = type,
	};
	if (!avk_check(vkAllocateMemory(dev->dev, &alloc, NULL, &image->memory[0]),
			"vkAllocateMemory (encode intermediate)")) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	AVK_LIVE_INC(dev, device_memory);
	image->memory_count = 1;
	if (!avk_check(vkBindImageMemory(dev->dev, image->image, image->memory[0],
			0), "vkBindImageMemory (encode intermediate)")) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	VkImageViewCreateInfo vi = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1,
		},
	};
	if (!avk_check(vkCreateImageView(dev->dev, &vi, NULL, &image->view),
			"vkCreateImageView (encode intermediate)")) {
		avk_image_destroy(dev, image);
		return NULL;
	}
	AVK_LIVE_INC(dev, image_views);
	*out_bytes = reqs.size;
	return image;
}

struct avk_image *avk_encode_intermediate_get(struct avk_encode_intermediate *w,
		struct avk_device *dev, struct avk_retire_queue *retire,
		VkFormat format, uint32_t width, uint32_t height) {
	if (w == NULL || dev == NULL || width == 0 || height == 0) {
		return NULL;
	}
	struct avk_image *img = w->image;
	if (img != NULL && (img->format != format || img->extent.width != width
			|| img->extent.height != height)) {
		/* Wrong shape. Retired against ITS OWN last use -- the exact point past
		 * which no submitted command buffer can still be sampling it -- and the
		 * slot is cleared first, so nothing below can fail with ownership in two
		 * places at once. */
		w->image = NULL;
		w->image_bytes = 0;
		avk_retire_push(retire, dev, img->last_use, avk_image_destroy, img);
		img = NULL;
	}
	if (img == NULL) {
		VkDeviceSize bytes = 0;
		img = intermediate_create(dev, format, width, height, &bytes);
		if (img == NULL) {
			return NULL;
		}
		w->image = img;
		w->image_bytes = (uint64_t)bytes;
	}
	return img;
}

void avk_encode_intermediate_finish(struct avk_encode_intermediate *w,
		struct avk_device *dev, struct avk_retire_queue *retire) {
	if (w == NULL) {
		return;
	}
	struct avk_image *img = w->image;
	w->image = NULL;
	w->image_bytes = 0;
	if (img == NULL) {
		return;
	}
	if (retire != NULL) {
		avk_retire_push(retire, dev, img->last_use, avk_image_destroy, img);
	} else {
		avk_image_destroy(dev, img);
	}
}

struct avk_image *avk_encode_lut_get(struct avk_encode_lut *l,
		struct avk_device *dev, struct avk_cmd_ring *ring,
		struct avk_retire_queue *retire,
		const uint16_t curve[3][AVK_ENCODE_LUT_TAPS], uint64_t serial) {
	if (l == NULL || dev == NULL || ring == NULL || curve == NULL) {
		return NULL;
	}
	/*
	 * Serial 0 means "no profile", and it is refused rather than treated as a
	 * curve to upload: an all-zero table encodes every scene value to black,
	 * which is a picture and would therefore not look like a missing profile.
	 */
	if (serial == 0) {
		return NULL;
	}
	if (l->image != NULL && l->serial == serial) {
		return l->image;
	}

	if (l->image == NULL) {
		/* ABGR16161616 is VK_FORMAT_R16G16B16A16_UNORM, and the DRM name's
		 * component order is memory order ascending -- so R is written first
		 * and lands in the shader's .r. The one place this file has an opinion
		 * about channel order, stated because getting it backwards produces a
		 * picture with the red and blue curves swapped, which on a
		 * near-neutral profile is almost invisible. */
		l->image = avk_upload_image_create(dev, DRM_FORMAT_ABGR16161616,
			AVK_ENCODE_LUT_TAPS, 1, AVK_IMAGE_OWNED);
		if (l->image == NULL) {
			return NULL;
		}
		avk_device_name_object(dev, VK_OBJECT_TYPE_IMAGE,
			(uint64_t)l->image->image, "avk encode lut");
	}

	/* Interleaved on the stack: 2KB, once per profile change. */
	uint16_t texels[AVK_ENCODE_LUT_TAPS * 4];
	for (uint32_t i = 0; i < AVK_ENCODE_LUT_TAPS; i++) {
		texels[i * 4 + 0] = curve[0][i];
		texels[i * 4 + 1] = curve[1][i];
		texels[i * 4 + 2] = curve[2][i];
		/* Opaque. Nothing reads it -- the shader takes .r/.g/.b -- but a
		 * zero alpha in a sampled image is the kind of thing a later reader
		 * would have to go and check. */
		texels[i * 4 + 3] = 65535;
	}

	struct avk_upload *staging = calloc(1, sizeof(*staging));
	if (staging == NULL) {
		return NULL;
	}
	AVK_LIVE_INC(dev, avk_uploads);
	/*
	 * ORDERED BEHIND WHATEVER LAST SAMPLED IT. A profile change on a running
	 * desktop overwrites a texture that frames still in flight are reading, and
	 * the copy would otherwise race the fragment shader -- a write-after-read
	 * hazard that shows up as one torn frame at the moment the profile changes
	 * and is therefore nearly unobservable. One timeline wait, no CPU wait.
	 */
	VkSemaphoreSubmitInfo wait = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = dev->timeline,
		.value = l->image->last_use,
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	};
	uint32_t wait_count = l->image->last_use > 0 ? 1 : 0;
	uint64_t timeline = avk_upload_image_write(dev, ring, staging, l->image,
		texels, (uint32_t)sizeof(texels), 1, &wait, wait_count);
	if (timeline == 0) {
		avk_upload_retire(dev, staging);
		/* The image is kept but its serial is NOT advanced, so the next frame
		 * tries again rather than encoding through a table that was never
		 * written. */
		return NULL;
	}
	if (retire != NULL) {
		avk_retire_push(retire, dev, timeline, avk_upload_retire, staging);
	} else {
		avk_upload_retire(dev, staging);
	}
	l->image->content_seq++;
	l->serial = serial;
	avk_log(AVK_DEBUG, "avk encode: uploaded a %d-tap measured curve (serial "
		"%" PRIu64 ")", AVK_ENCODE_LUT_TAPS, serial);
	return l->image;
}

void avk_encode_lut_finish(struct avk_encode_lut *l, struct avk_device *dev,
		struct avk_retire_queue *retire) {
	if (l == NULL) {
		return;
	}
	struct avk_image *img = l->image;
	l->image = NULL;
	l->serial = 0;
	if (img == NULL) {
		return;
	}
	if (retire != NULL) {
		avk_retire_push(retire, dev, img->last_use, avk_image_destroy, img);
	} else {
		avk_image_destroy(dev, img);
	}
}

/* ── M6C: the cLUT ────────────────────────────────────────────────────────── */

struct avk_image *avk_encode_clut_get(struct avk_encode_clut *l,
		struct avk_device *dev, struct avk_cmd_ring *ring,
		struct avk_retire_queue *retire, const uint16_t *grid, uint32_t dim,
		uint64_t serial) {
	if (l == NULL || dev == NULL || ring == NULL || grid == NULL || dim < 2) {
		return NULL;
	}
	/* Serial 0 means "no profile", refused rather than uploaded, for the 1D
	 * curve's reason: an all-zero cube is a picture (black) and would therefore
	 * not look like a missing profile. */
	if (serial == 0) {
		return NULL;
	}
	if (l->image != NULL && l->serial == serial && l->dim == dim) {
		return l->image;
	}
	/* A profile whose cube changed SHAPE is a different image, not a different
	 * upload. Retired against its own last use, slot cleared first, exactly as
	 * the intermediate does when the output's extent changes. */
	if (l->image != NULL && l->dim != dim) {
		struct avk_image *old = l->image;
		l->image = NULL;
		l->serial = 0;
		l->dim = 0;
		if (retire != NULL) {
			avk_retire_push(retire, dev, old->last_use, avk_image_destroy, old);
		} else {
			avk_image_destroy(dev, old);
		}
	}

	if (l->image == NULL) {
		/* Same format and the same reason as the 1D curve: ABGR16161616 is
		 * VK_FORMAT_R16G16B16A16_UNORM, whose DRM name is memory order
		 * ascending, so R is written first and lands in the shader's .r. */
		l->image = avk_upload_image_create_3d(dev, DRM_FORMAT_ABGR16161616,
			dim, AVK_IMAGE_OWNED);
		if (l->image == NULL) {
			return NULL;
		}
		l->dim = dim;
		avk_device_name_object(dev, VK_OBJECT_TYPE_IMAGE,
			(uint64_t)l->image->image, "avk encode clut");
	}

	size_t texels = (size_t)dim * dim * dim;
	uint16_t *staging_texels = calloc(texels * 4, sizeof(uint16_t));
	if (staging_texels == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < texels; i++) {
		staging_texels[i * 4 + 0] = grid[i * 3 + 0];
		staging_texels[i * 4 + 1] = grid[i * 3 + 1];
		staging_texels[i * 4 + 2] = grid[i * 3 + 2];
		/* Opaque. Nothing reads it; a zero alpha in a sampled image is the kind
		 * of thing a later reader would have to go and check. */
		staging_texels[i * 4 + 3] = 65535;
	}

	struct avk_upload *staging = calloc(1, sizeof(*staging));
	if (staging == NULL) {
		free(staging_texels);
		return NULL;
	}
	AVK_LIVE_INC(dev, avk_uploads);
	/* Ordered behind whatever last sampled it, for the 1D curve's reason: a
	 * profile change on a running desktop overwrites a texture that frames still
	 * in flight are reading. */
	VkSemaphoreSubmitInfo wait = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = dev->timeline,
		.value = l->image->last_use,
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	};
	uint32_t wait_count = l->image->last_use > 0 ? 1 : 0;
	/* One row of the cube, and dim*dim rows of them. The copy takes the SLICE
	 * height and the depth from the image itself (avk_upload.c), which is what
	 * makes one write path serve a plane and a volume. */
	uint64_t timeline = avk_upload_image_write(dev, ring, staging, l->image,
		staging_texels, dim * 4 * (uint32_t)sizeof(uint16_t), dim * dim,
		&wait, wait_count);
	free(staging_texels);
	if (timeline == 0) {
		avk_upload_retire(dev, staging);
		/* Image kept, serial NOT advanced: the next frame tries again rather
		 * than encoding through a cube that was never written. */
		return NULL;
	}
	if (retire != NULL) {
		avk_retire_push(retire, dev, timeline, avk_upload_retire, staging);
	} else {
		avk_upload_retire(dev, staging);
	}
	l->image->content_seq++;
	l->serial = serial;
	avk_log(AVK_DEBUG, "avk encode: uploaded a %u^3 cLUT (%zu KB, serial "
		"%" PRIu64 ")", dim, texels * 8 / 1024, serial);
	return l->image;
}

void avk_encode_clut_finish(struct avk_encode_clut *l, struct avk_device *dev,
		struct avk_retire_queue *retire) {
	if (l == NULL) {
		return;
	}
	struct avk_image *img = l->image;
	l->image = NULL;
	l->serial = 0;
	l->dim = 0;
	if (img == NULL) {
		return;
	}
	if (retire != NULL) {
		avk_retire_push(retire, dev, img->last_use, avk_image_destroy, img);
	} else {
		avk_image_destroy(dev, img);
	}
}
