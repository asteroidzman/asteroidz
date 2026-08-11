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
	}
	free(scene->cmds);
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
	return cmd;
}

bool avk_cmd_set_clip(struct avk_cmd *cmd, const pixman_region32_t *region) {
	if (!cmd->has_clip) {
		pixman_region32_init(&cmd->clip);
		cmd->has_clip = true;
	}
	return pixman_region32_copy(&cmd->clip, (pixman_region32_t *)region);
}
