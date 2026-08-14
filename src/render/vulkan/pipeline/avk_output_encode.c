#define _POSIX_C_SOURCE 200809L

#include "avk_output_encode.h"

#include <stdlib.h>
#include <string.h>

#include "../avk.h"
#include "../device/avk_device.h"
#include "../image/avk_image.h"
#include "../image/avk_upload.h"

#include "output_encode_vert.spv.h"
#include "output_encode_frag.spv.h"

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
	VkPipelineLayoutCreateInfo li = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &set_layout,
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
	if (enc->vert == VK_NULL_HANDLE || enc->frag == VK_NULL_HANDLE) {
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
	if (enc->vert != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, enc->vert, NULL);
		enc->vert = VK_NULL_HANDLE;
	}
	if (enc->frag != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, enc->frag, NULL);
		enc->frag = VK_NULL_HANDLE;
	}
	if (enc->layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(dev->dev, enc->layout, NULL);
		enc->layout = VK_NULL_HANDLE;
	}
	enc->dev = NULL;
}

static VkPipeline encode_build(struct avk_output_encode *enc, VkFormat format,
		bool pq) {
	struct avk_device *dev = enc->dev;

	/* AZ_ENCODE_TF, constant_id 0. The other curve is not compiled in at all,
	 * which is the point: "there is no PQ encode in an SDR pipeline" is a
	 * stronger statement than "the branch is not taken". */
	int32_t tf = pq ? 1 : 0;
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
			.module = enc->frag,
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
		"avk pipeline output_encode %s fmt=%d", pq ? "pq" : "srgb", (int)format);
	return pipeline;
}

VkPipeline avk_output_encode_pipeline(struct avk_output_encode *enc,
		VkFormat format, bool pq) {
	if (enc == NULL || enc->dev == NULL || enc->frag == VK_NULL_HANDLE) {
		return VK_NULL_HANDLE;
	}
	for (uint32_t i = 0; i < enc->variant_len; i++) {
		if (enc->variants[i].format == format && enc->variants[i].pq == pq) {
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
			pq ? "pq" : "srgb");
		return VK_NULL_HANDLE;
	}
	VkPipeline pipeline = encode_build(enc, format, pq);
	/* Recorded even when it is VK_NULL_HANDLE: a format that cannot compile
	 * must fail once, not once per frame. */
	enc->variants[enc->variant_len++] = (struct avk_encode_variant){
		.format = format,
		.pq = pq,
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
