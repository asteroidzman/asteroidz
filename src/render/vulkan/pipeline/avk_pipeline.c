#define _POSIX_C_SOURCE 200809L

#include "avk_pipeline.h"

#include <stdlib.h>
#include <string.h>

#include "quad_vert.spv.h"
#include "quad_frag.spv.h"
#include "texture_frag.spv.h"
#include "gradient_frag.spv.h"
#include "shadow_frag.spv.h"
#include "blur_down_frag.spv.h"
#include "blur_up_frag.spv.h"
#include "blur_soft_frag.spv.h"

/* Sets per descriptor pool. Enough that an ordinary desktop -- a few dozen
 * surfaces -- never allocates a second one, small enough that a pool is not a
 * meaningful allocation on its own. */
#define AVK_SETS_PER_POOL 256

static VkShaderModule create_module(struct avk_device *dev,
		const uint32_t *code, size_t size, const char *name) {
	VkShaderModuleCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = code,
	};
	VkShaderModule module = VK_NULL_HANDLE;
	VkResult res = vkCreateShaderModule(dev->dev, &info, NULL, &module);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateShaderModule");
		return VK_NULL_HANDLE;
	}
	avk_device_name_object(dev, VK_OBJECT_TYPE_SHADER_MODULE,
		(uint64_t)module, "avk shader %s", name);
	return module;
}

/*
 * THE THREE BLEND MODES, and there are exactly three because each answers a
 * different question about what the destination already holds.
 *
 * AZ_BLEND_OVER    premultiplied source-over. Every compositing primitive: the
 *                  destination is the frame so far and this draw goes on top.
 *
 * AZ_BLEND_REPLACE no blend, straight write. A blur's down and up passes do not
 *                  composite, they RESAMPLE -- their destination is a transient
 *                  whose previous contents are another window's blur from three
 *                  frames ago, and blending against that is not "slightly
 *                  wrong", it is the previous frame leaking through. Replacing
 *                  also means the transient needs no clear, which is a
 *                  full-target write per level saved.
 *
 * AZ_BLEND_DARKEN  min() on rgb, and the source's own alpha. This is the
 *                  reference's darken clamp (blur2.comp:158,
 *                  `color.rgb = min(color.rgb, src.rgb)`) expressed as fixed
 *                  function, because a fragment shader cannot read the
 *                  attachment it writes and the reference's imageLoad is a
 *                  compute-path affordance AVK does not have.
 *
 *                  Blending runs AFTER the fragment shader, so the post-effects
 *                  are folded in first -- exactly the order blur2.comp uses
 *                  (effects at :132, clamp at :158). Min/max blend ops IGNORE
 *                  their blend factors per spec, so there is one behaviour here
 *                  and no factor to get subtly wrong; the alpha op is a real ADD
 *                  with (ONE, ZERO) so alpha still comes from the blur untouched,
 *                  which is what `color.rgb = ...` in the reference means.
 */
enum az_blend_mode {
	AZ_BLEND_OVER,
	AZ_BLEND_REPLACE,
	AZ_BLEND_DARKEN,
};

static bool create_pipeline_ex(struct avk_pipelines *pipes, VkFormat format,
		VkShaderModule vert, VkShaderModule frag, const char *name,
		enum az_blend_mode mode, VkPipeline *out) {
	struct avk_device *dev = pipes->dev;

	VkPipelineShaderStageCreateInfo stages[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vert,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = frag,
			.pName = "main",
		},
	};

	/* No vertex bindings at all: the quad comes from gl_VertexIndex. */
	VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
	};
	VkPipelineViewportStateCreateInfo viewport = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo raster = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		/* No culling. A flipped output transform reverses winding, and a
		 * culled quad is an invisible window with no error anywhere. */
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	/*
	 * Premultiplied source-over, and only this.
	 *
	 * Wayland surfaces are premultiplied by protocol, AVK premultiplies its
	 * own rect colours before they reach the shader, and opacity scales the
	 * whole premultiplied vec4. One convention everywhere means there is
	 * never a question about which side of the multiply a given colour is on
	 * -- which is where straight-vs-premultiplied bugs live, and they always
	 * look like "the edges are too dark".
	 */
	VkPipelineColorBlendAttachmentState blend = {
		.blendEnable = mode != AZ_BLEND_REPLACE ? VK_TRUE : VK_FALSE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
			| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	if (mode == AZ_BLEND_DARKEN) {
		blend.colorBlendOp = VK_BLEND_OP_MIN;
		/* Written out even though VK_BLEND_OP_MIN ignores them, so nobody has to
		 * remember that it does when reading this. */
		blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blend.alphaBlendOp = VK_BLEND_OP_ADD;
		blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	}
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
		.layout = pipes->layout,
	};

	VkResult res = vkCreateGraphicsPipelines(dev->dev, dev->pipeline_cache, 1,
		&info, NULL, out);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateGraphicsPipelines");
		return false;
	}
	AVK_LIVE_INC(dev, pipelines);
	avk_device_name_object(dev, VK_OBJECT_TYPE_PIPELINE, (uint64_t)*out,
		"avk pipeline %s", name);
	return true;
}

/* The blending form, which is what every compositing primitive wants. */
static bool create_pipeline(struct avk_pipelines *pipes, VkFormat format,
		VkShaderModule vert, VkShaderModule frag, const char *name,
		VkPipeline *out) {
	return create_pipeline_ex(pipes, format, vert, frag, name, AZ_BLEND_OVER,
		out);
}

static bool create_sampler(struct avk_device *dev, VkFilter filter,
		VkSampler *out, const char *name) {
	VkSamplerCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = filter,
		.minFilter = filter,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		/* CLAMP_TO_EDGE, not REPEAT: a source crop that lands a hair outside
		 * the surface must take the edge texel, not wrap round to the
		 * opposite side of the window. */
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
	};
	VkResult res = vkCreateSampler(dev->dev, &info, NULL, out);
	if (res != VK_SUCCESS) {
		return avk_check(res, "vkCreateSampler");
	}
	AVK_LIVE_INC(dev, samplers);
	avk_device_name_object(dev, VK_OBJECT_TYPE_SAMPLER, (uint64_t)*out,
		"avk sampler %s", name);
	return true;
}

bool avk_pipelines_init(struct avk_pipelines *pipes, struct avk_device *dev,
		VkFormat format) {
	memset(pipes, 0, sizeof(*pipes));
	pipes->dev = dev;

	VkDescriptorSetLayoutBinding binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo set_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	VkResult res = vkCreateDescriptorSetLayout(dev->dev, &set_layout_info,
		NULL, &pipes->texture_set_layout);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateDescriptorSetLayout");
		goto error;
	}
	AVK_LIVE_INC(dev, descriptor_set_layouts);

	VkDescriptorSetLayoutBinding grad_binding = {
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo grad_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &grad_binding,
	};
	res = vkCreateDescriptorSetLayout(dev->dev, &grad_layout_info, NULL,
		&pipes->gradient_set_layout);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreateDescriptorSetLayout (gradient)");
		goto error;
	}
	AVK_LIVE_INC(dev, descriptor_set_layouts);

	VkPushConstantRange range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT
			| VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(struct avk_push_constants),
	};
	VkDescriptorSetLayout set_layouts[2] = {
		pipes->texture_set_layout,     /* set 0 -- the sampled surface */
		pipes->gradient_set_layout,    /* set 1 -- the frame's gradient data */
	};
	VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 2,
		.pSetLayouts = set_layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &range,
	};
	res = vkCreatePipelineLayout(dev->dev, &layout_info, NULL,
		&pipes->layout);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkCreatePipelineLayout");
		goto error;
	}
	AVK_LIVE_INC(dev, pipeline_layouts);

	if (!create_sampler(dev, VK_FILTER_NEAREST, &pipes->nearest, "nearest")
			|| !create_sampler(dev, VK_FILTER_LINEAR, &pipes->linear,
				"linear")) {
		goto error;
	}

	VkShaderModule vert = create_module(dev, quad_vert_spv,
		sizeof(quad_vert_spv), "quad.vert");
	VkShaderModule rect_frag = create_module(dev, quad_frag_spv,
		sizeof(quad_frag_spv), "quad.frag");
	VkShaderModule texture_frag = create_module(dev, texture_frag_spv,
		sizeof(texture_frag_spv), "texture.frag");
	VkShaderModule gradient_frag = create_module(dev, gradient_frag_spv,
		sizeof(gradient_frag_spv), "gradient.frag");
	VkShaderModule shadow_frag = create_module(dev, shadow_frag_spv,
		sizeof(shadow_frag_spv), "shadow.frag");
	VkShaderModule blur_down_frag = create_module(dev, blur_down_frag_spv,
		sizeof(blur_down_frag_spv), "blur_down.frag");
	VkShaderModule blur_up_frag = create_module(dev, blur_up_frag_spv,
		sizeof(blur_up_frag_spv), "blur_up.frag");
	VkShaderModule blur_soft_frag = create_module(dev, blur_soft_frag_spv,
		sizeof(blur_soft_frag_spv), "blur_soft.frag");

	bool ok = vert != VK_NULL_HANDLE && rect_frag != VK_NULL_HANDLE
		&& texture_frag != VK_NULL_HANDLE && gradient_frag != VK_NULL_HANDLE
		&& shadow_frag != VK_NULL_HANDLE
		&& create_pipeline(pipes, format, vert, rect_frag, "rect",
			&pipes->rect)
		&& create_pipeline(pipes, format, vert, texture_frag, "texture",
			&pipes->texture)
		/* The same two shaders, blending off. Bit-exact for alpha-1 fragments
		 * because AZ_BLEND_OVER's destination factor is (1 - srcAlpha) = 0
		 * there; see the note on rect_opaque. */
		&& create_pipeline_ex(pipes, format, vert, rect_frag, "rect_opaque",
			AZ_BLEND_REPLACE, &pipes->rect_opaque)
		&& create_pipeline_ex(pipes, format, vert, texture_frag,
			"texture_opaque", AZ_BLEND_REPLACE, &pipes->texture_opaque)
		&& create_pipeline(pipes, format, vert, gradient_frag, "gradient",
			&pipes->gradient)
		&& create_pipeline(pipes, format, vert, shadow_frag, "shadow",
			&pipes->shadow)
		&& blur_down_frag != VK_NULL_HANDLE && blur_up_frag != VK_NULL_HANDLE
		&& blur_soft_frag != VK_NULL_HANDLE
		/* REPLACING, not blending -- see create_pipeline_ex(). */
		&& create_pipeline_ex(pipes, format, vert, blur_down_frag, "blur_down",
			AZ_BLEND_REPLACE, &pipes->blur_down)
		&& create_pipeline_ex(pipes, format, vert, blur_up_frag, "blur_up",
			AZ_BLEND_REPLACE, &pipes->blur_up)
		/* The identical shader, clamped against its own destination. */
		&& create_pipeline_ex(pipes, format, vert, blur_up_frag,
			"blur_up_darken", AZ_BLEND_DARKEN, &pipes->blur_up_darken)
		/* A composite, so it blends like every other composite. */
		&& create_pipeline(pipes, format, vert, blur_soft_frag, "blur_soft",
			&pipes->blur_soft);

	/* Modules are only needed while the pipelines are being created; the
	 * driver has compiled what it needs by the time vkCreateGraphicsPipelines
	 * returns. */
	if (vert != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, vert, NULL);
	}
	if (rect_frag != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, rect_frag, NULL);
	}
	if (texture_frag != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, texture_frag, NULL);
	}
	if (gradient_frag != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, gradient_frag, NULL);
	}
	if (shadow_frag != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, shadow_frag, NULL);
	}
	if (blur_down_frag != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, blur_down_frag, NULL);
	}
	if (blur_soft_frag != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, blur_soft_frag, NULL);
	}
	if (blur_up_frag != VK_NULL_HANDLE) {
		vkDestroyShaderModule(dev->dev, blur_up_frag, NULL);
	}

	if (!ok) {
		goto error;
	}
	return true;

error:
	avk_pipelines_finish(pipes);
	return false;
}

void avk_pipelines_finish(struct avk_pipelines *pipes) {
	if (pipes->dev == NULL) {
		return;
	}
	VkDevice dev = pipes->dev->dev;

	if (pipes->rect != VK_NULL_HANDLE) {
		vkDestroyPipeline(dev, pipes->rect, NULL);
		AVK_LIVE_DEC(pipes->dev, pipelines);
	}
	if (pipes->texture != VK_NULL_HANDLE) {
		vkDestroyPipeline(dev, pipes->texture, NULL);
		AVK_LIVE_DEC(pipes->dev, pipelines);
	}
	if (pipes->rect_opaque != VK_NULL_HANDLE) {
		vkDestroyPipeline(dev, pipes->rect_opaque, NULL);
		AVK_LIVE_DEC(pipes->dev, pipelines);
	}
	if (pipes->texture_opaque != VK_NULL_HANDLE) {
		vkDestroyPipeline(dev, pipes->texture_opaque, NULL);
		AVK_LIVE_DEC(pipes->dev, pipelines);
	}
	if (pipes->blur_down != VK_NULL_HANDLE) {
		vkDestroyPipeline(dev, pipes->blur_down, NULL);
		AVK_LIVE_DEC(pipes->dev, pipelines);
	}
	if (pipes->blur_up_darken != VK_NULL_HANDLE) {
		vkDestroyPipeline(dev, pipes->blur_up_darken, NULL);
		AVK_LIVE_DEC(pipes->dev, pipelines);
	}
	if (pipes->blur_soft != VK_NULL_HANDLE) {
		vkDestroyPipeline(dev, pipes->blur_soft, NULL);
		AVK_LIVE_DEC(pipes->dev, pipelines);
	}
	if (pipes->blur_up != VK_NULL_HANDLE) {
		vkDestroyPipeline(dev, pipes->blur_up, NULL);
		AVK_LIVE_DEC(pipes->dev, pipelines);
	}
	if (pipes->shadow != VK_NULL_HANDLE) {
		vkDestroyPipeline(dev, pipes->shadow, NULL);
		AVK_LIVE_DEC(pipes->dev, pipelines);
	}
	if (pipes->gradient != VK_NULL_HANDLE) {
		vkDestroyPipeline(dev, pipes->gradient, NULL);
		AVK_LIVE_DEC(pipes->dev, pipelines);
	}
	if (pipes->layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(dev, pipes->layout, NULL);
		AVK_LIVE_DEC(pipes->dev, pipeline_layouts);
	}
	if (pipes->texture_set_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(dev, pipes->texture_set_layout, NULL);
		AVK_LIVE_DEC(pipes->dev, descriptor_set_layouts);
	}
	if (pipes->gradient_set_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(dev, pipes->gradient_set_layout, NULL);
		AVK_LIVE_DEC(pipes->dev, descriptor_set_layouts);
	}
	if (pipes->nearest != VK_NULL_HANDLE) {
		vkDestroySampler(dev, pipes->nearest, NULL);
		AVK_LIVE_DEC(pipes->dev, samplers);
	}
	if (pipes->linear != VK_NULL_HANDLE) {
		vkDestroySampler(dev, pipes->linear, NULL);
		AVK_LIVE_DEC(pipes->dev, samplers);
	}
	for (uint32_t i = 0; i < pipes->pool_count; i++) {
		vkDestroyDescriptorPool(dev, pipes->pools[i], NULL);
		AVK_LIVE_DEC(pipes->dev, descriptor_pools);
	}
	free(pipes->pools);
	memset(pipes, 0, sizeof(*pipes));
}

static bool add_pool(struct avk_pipelines *pipes) {
	VkDescriptorPoolSize size = {
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = AVK_SETS_PER_POOL,
	};
	VkDescriptorPoolCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = AVK_SETS_PER_POOL,
		.poolSizeCount = 1,
		.pPoolSizes = &size,
	};
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkResult res = vkCreateDescriptorPool(pipes->dev->dev, &info, NULL, &pool);
	if (res != VK_SUCCESS) {
		return avk_check(res, "vkCreateDescriptorPool");
	}
	AVK_LIVE_INC(pipes->dev, descriptor_pools);

	VkDescriptorPool *pools = realloc(pipes->pools,
		(pipes->pool_count + 1) * sizeof(*pools));
	if (pools == NULL) {
		vkDestroyDescriptorPool(pipes->dev->dev, pool, NULL);
		AVK_LIVE_DEC(pipes->dev, descriptor_pools);
		return false;
	}
	pipes->pools = pools;
	pipes->pools[pipes->pool_count++] = pool;
	pipes->sets_left_in_current_pool = AVK_SETS_PER_POOL;
	return true;
}

VkDescriptorSet avk_pipelines_texture_set(struct avk_pipelines *pipes,
		struct avk_image *image, bool linear) {
	uint32_t slot = linear ? 1 : 0;
	if (image->sampler_set[slot] != VK_NULL_HANDLE) {
		return image->sampler_set[slot];
	}

	if (image->view == VK_NULL_HANDLE) {
		VkImageViewCreateInfo view_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = image->image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = image->format,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
		};
		VkResult res = vkCreateImageView(pipes->dev->dev, &view_info, NULL,
			&image->view);
		if (res != VK_SUCCESS) {
			avk_check(res, "vkCreateImageView");
			return VK_NULL_HANDLE;
		}
		AVK_LIVE_INC(pipes->dev, image_views);
	}

	if (pipes->sets_left_in_current_pool == 0 && !add_pool(pipes)) {
		return VK_NULL_HANDLE;
	}

	VkDescriptorSetAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = pipes->pools[pipes->pool_count - 1],
		.descriptorSetCount = 1,
		.pSetLayouts = &pipes->texture_set_layout,
	};
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkResult res = vkAllocateDescriptorSets(pipes->dev->dev, &alloc, &set);
	if (res != VK_SUCCESS) {
		avk_check(res, "vkAllocateDescriptorSets");
		return VK_NULL_HANDLE;
	}
	pipes->sets_left_in_current_pool--;

	VkDescriptorImageInfo image_info = {
		.sampler = linear ? pipes->linear : pipes->nearest,
		.imageView = image->view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image_info,
	};
	vkUpdateDescriptorSets(pipes->dev->dev, 1, &write, 0, NULL);

	image->sampler_set[slot] = set;
	return set;
}
