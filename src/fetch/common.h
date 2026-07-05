pid_t getparentprocess(pid_t p) {
	uint32_t v = 0;

	FILE *f;
	char buf[256];
	snprintf(buf, sizeof(buf) - 1, "/proc/%u/stat", (unsigned)p);

	if (!(f = fopen(buf, "r")))
		return 0;

	// check fscanf's return value to make sure 1 field was read successfully
	if (fscanf(f, "%*u %*s %*c %u", &v) != 1) {
		fclose(f);
		return 0;
	}

	fclose(f);

	return (pid_t)v;
}

int32_t isdescprocess(pid_t p, pid_t c) {
	while (p != c && c != 0)
		c = getparentprocess(c);

	return (int32_t)c;
}

void get_layout_abbr(char *abbr, const char *full_name) {
	// clear the output buffer
	abbr[0] = '\0';

	// 1. try to look it up in the mapping table
	for (int32_t i = 0; layout_mappings[i].full_name != NULL; i++) {
		if (strcmp(full_name, layout_mappings[i].full_name) == 0) {
			strcpy(abbr, layout_mappings[i].abbr);
			return;
		}
	}

	// 2. try to extract from the name and lowercase it
	const char *open = strrchr(full_name, '(');
	const char *close = strrchr(full_name, ')');
	if (open && close && close > open) {
		uint32_t len = close - open - 1;
		if (len > 0 && len <= 4) {
			// extract and lowercase
			for (uint32_t j = 0; j < len; j++) {
				abbr[j] = tolower(open[j + 1]);
			}
			abbr[len] = '\0';
			return;
		}
	}

	// 3. take the first 2-3 letters and lowercase them
	uint32_t j = 0;
	for (uint32_t i = 0; full_name[i] != '\0' && j < 3; i++) {
		if (isalpha(full_name[i])) {
			abbr[j++] = tolower(full_name[i]);
		}
	}
	abbr[j] = '\0';

	// make sure we have at least 2 characters
	if (j >= 2) {
		return;
	}

	// 4. fallback: lowercase the first letter
	if (j == 1) {
		abbr[1] = full_name[1] ? tolower(full_name[1]) : '\0';
		abbr[2] = '\0';
	} else {
		// 5. final fallback: return "xx"
		strcpy(abbr, "xx");
	}
}

Client *xytoclient(double x, double y) {
	Client *c = NULL, *tmp = NULL;
	wl_list_for_each_safe(c, tmp, &clients, link) {
		if (VISIBLEON(c, c->mon) && c->animation.current.x <= x &&
			c->animation.current.y <= y &&
			c->animation.current.x + c->animation.current.width >= x &&
			c->animation.current.y + c->animation.current.height >= y) {
			return c;
		}
	}
	return NULL;
}

static bool layer_ignores_focus(LayerSurface *l) {
	if (!l || !l->layer_surface)
		return true;
	struct wlr_surface *s = l->layer_surface->surface;
	return !pixman_region32_not_empty(&s->input_region) ||
		   l->layer_surface->current.keyboard_interactive ==
			   ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
}

void xytonode(double x, double y, struct wlr_surface **psurface, Client **pc,
			  LayerSurface **pl, AsteroidzGroupBar **gb, double *nx, double *ny) {
	struct wlr_scene_node *node = NULL, *pnode = NULL;
	struct wlr_surface *surface = NULL;
	Client *c = NULL;
	LayerSurface *l = NULL;
	AsteroidzGroupBar *groupbar = NULL;
	int32_t layer;
	Client *ovc = NULL;

	if (psurface)
		*psurface = NULL;
	if (pc)
		*pc = NULL;
	if (pl)
		*pl = NULL;
	if (gb)
		*gb = NULL;

	for (layer = NUM_LAYERS - 1; layer >= 0; layer--) {
		if (layer == LyrFadeOut || layer == LyrScreenshot)
			continue;

		node = wlr_scene_node_at(&layers[layer]->node, x, y, nx, ny);
		if (!node)
			continue;

		if (node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_surface *scene_surface =
				wlr_scene_surface_try_from_buffer(
					wlr_scene_buffer_from_node(node));
			if (scene_surface) {
				surface = scene_surface->surface;
			}
		}

		if (layer == LyrIMPopup) {
			c = NULL;
			l = NULL;
		} else {
			void *data = NULL;
			for (pnode = node; pnode; pnode = &pnode->parent->node) {
				if (pnode->data) {
					data = pnode->data;
					break;
				}
			}

			if (data) {
				Client *temp_c = (Client *)data;
				if (temp_c->type == LayerShell) {
					l = (LayerSurface *)temp_c;
				} else if (temp_c->type == GroupBar) {
					groupbar = (AsteroidzGroupBar *)temp_c;
				} else if (temp_c->type == XDGShell || temp_c->type == X11) {
					c = temp_c;
				}
			}
		}

		if (node->type == WLR_SCENE_NODE_RECT) {
			if (c) {
				surface = client_surface(c);
			}
		}

		break;
	}

	if (psurface)
		*psurface = surface;
	if (pc)
		*pc = c;
	if (pl)
		*pl = l;
	if (gb)
		*gb = groupbar;

	if (selmon && selmon->isoverview && config.ov_no_resize) {
		ovc = xytoclient(x, y);

		bool is_below = false;
		if (l && l->layer_surface) {
			is_below = (l->layer_surface->current.layer ==
							ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
						l->layer_surface->current.layer ==
							ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
		}

		if (ovc && (!l || layer_ignores_focus(l) || is_below)) {
			if (pc)
				*pc = ovc;
			if (psurface)
				*psurface = ovc ? client_surface(ovc) : NULL;
			if (pl)
				*pl = NULL;
			if (gb)
				*gb = NULL;
		}
	}
}