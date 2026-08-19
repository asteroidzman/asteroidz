#define _POSIX_C_SOURCE 200809L

#include "avk_scene.h"

#include <stdlib.h>
#include <string.h>

void avk_scene_init(struct avk_scene *scene) {
	memset(scene, 0, sizeof(*scene));
	pixman_region32_init(&scene->damage);
}

void avk_scene_finish(struct avk_scene *scene) {
	for (size_t i = 0; i < scene->len; i++) {
		if (scene->cmds[i].has_clip) {
			pixman_region32_fini(&scene->cmds[i].clip);
		}
		if (scene->cmds[i].has_opaque) {
			pixman_region32_fini(&scene->cmds[i].opaque);
		}
	}
	free(scene->cmds);
	free(scene->gradient_colors);
	pixman_region32_fini(&scene->damage);
	memset(scene, 0, sizeof(*scene));
}

struct avk_cmd *avk_scene_add(struct avk_scene *scene, enum avk_cmd_type type) {
	if (scene->len == scene->cap) {
		/* Starts at 64 because a desktop frame is tens of commands, not
		 * three: growing from 1 would realloc six times on the first frame
		 * and then never again, which is all cost and no benefit. */
		size_t cap = scene->cap == 0 ? 64 : scene->cap * 2;
		struct avk_cmd *cmds = realloc(scene->cmds, cap * sizeof(*cmds));
		if (cmds == NULL) {
			avk_log(AVK_ERROR, "scene: out of memory adding a command");
			return NULL;
		}
		scene->cmds = cmds;
		scene->cap = cap;
	}

	struct avk_cmd *cmd = &scene->cmds[scene->len++];
	memset(cmd, 0, sizeof(*cmd));
	cmd->type = type;
	cmd->opacity = 1.0f;
	cmd->color[3] = 1.0f;
	/*
	 * M5/C2. THE UNTAGGED DOMAIN, not a zeroed one.
	 *
	 * memset above leaves scale = 0, and the resolver's contract is that
	 * `scale > 0` ALWAYS -- a consumer is entitled to multiply by it. Zero is
	 * not a neutral default here, it is a black surface, and it would be the
	 * value carried by every command whose site does not fill this in: rects,
	 * shadows, blur results, and the output cursor, which has no colour
	 * description to resolve from.
	 *
	 * Defaulting HERE rather than at each site means a command type added
	 * later cannot forget. Sites with a real source description overwrite it.
	 */
	cmd->lum = az_lum_domain_untagged();
	return cmd;
}

bool avk_cmd_set_opaque(struct avk_cmd *cmd, const pixman_region32_t *region) {
	if (!cmd->has_opaque) {
		pixman_region32_init(&cmd->opaque);
		cmd->has_opaque = true;
	}
	return pixman_region32_copy(&cmd->opaque, (pixman_region32_t *)region);
}

bool avk_cmd_set_clip(struct avk_cmd *cmd, const pixman_region32_t *region) {
	if (!cmd->has_clip) {
		pixman_region32_init(&cmd->clip);
		cmd->has_clip = true;
	}
	return pixman_region32_copy(&cmd->clip, (pixman_region32_t *)region);
}

bool avk_cmd_set_gradient(struct avk_scene *scene, struct avk_cmd *cmd,
		enum avk_gradient_type type, float degree, bool blend,
		const float origin[2], const float *colors, uint32_t count) {
	if (type == AVK_GRADIENT_NONE || colors == NULL || count == 0) {
		return false;
	}

	if (scene->gradient_color_len + count > scene->gradient_color_cap) {
		/* Geometric, and starting at a size a real frame usually fits inside:
		 * asteroidz draws at most a focused window's two-stop border and the
		 * overview's two five-stop vignettes, so 64 colours covers an ordinary
		 * desktop without ever growing. */
		uint32_t cap = scene->gradient_color_cap == 0 ? 64
			: scene->gradient_color_cap * 2;
		while (cap < scene->gradient_color_len + count) {
			cap *= 2;
		}
		float *grown = realloc(scene->gradient_colors,
			(size_t)cap * 4 * sizeof(float));
		if (grown == NULL) {
			avk_log(AVK_ERROR, "scene: out of memory packing %u gradient "
				"colours", count);
			return false;
		}
		scene->gradient_colors = grown;
		scene->gradient_color_cap = cap;
	}

	memcpy(scene->gradient_colors + (size_t)scene->gradient_color_len * 4,
		colors, (size_t)count * 4 * sizeof(float));

	cmd->gradient = (struct avk_gradient){
		.type = type,
		.degree = degree,
		.blend = blend,
		.origin = { origin != NULL ? origin[0] : 0.5f,
			origin != NULL ? origin[1] : 0.5f },
		.color_offset = scene->gradient_color_len,
		.color_count = count,
	};
	scene->gradient_color_len += count;
	return true;
}
