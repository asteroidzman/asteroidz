#ifndef AZ_DMABUF_CAPS_H
#define AZ_DMABUF_CAPS_H

/*
 * Turning the capability model into a linux-dmabuf global.
 *
 * The model itself -- which format/modifier pairs are advertisable, and which
 * device -- lives in az_dmabuf_model.h, which depends on nothing but AVK's
 * format table and wlr_drm_format_set. That split is not tidiness: it is what
 * lets the rule be driven with SYNTHETIC capabilities
 * and assert the answer, instead of a test that can only ever confirm what
 * this machine's Mesa happens to report today.
 */

#include "az_dmabuf_model.h"


/* Kept so `amsg get dmabuf-feedback` can report what was advertised without
 * rebuilding it, and so the subset invariants can be checked against the
 * thing clients were actually told rather than against a fresh computation
 * that might differ. */
static struct {
	bool active;
	dev_t main_device;
	struct wlr_drm_format_set composition;
	uint32_t pairs;
} az_dmabuf_advertised;

/*
 * Build the linux-dmabuf global from AVK's capabilities.
 *
 * wlroots implements the protocol and compiles the feedback; every number in
 * it comes from here. wlr_linux_dmabuf_v1_create() takes a caller-owned
 * default feedback, so no fork is needed for this half -- only the per-surface
 * half needed a SceneFX hook, because wlr_linux_dmabuf_feedback_v1_init_with_
 * options() asserts a renderer and offers no way to pass a format set.
 */
static struct wlr_linux_dmabuf_v1 *az_dmabuf_create_from_avk(
		struct wl_display *display) {
	if (!avk.active) {
		return NULL;
	}
	dev_t main_device;
	if (!az_dmabuf_main_device(avk.device, &main_device)) {
		wlr_log(WLR_ERROR, "dmabuf: AVK has no DRM device to advertise");
		return NULL;
	}

	/*
	 * The size the advertisement has to hold up to.
	 *
	 * The device's own 2D limit, not the largest output: feedback is built
	 * once, at start-up, before any output exists and long before the user
	 * plugs in a bigger monitor -- and a client is entitled to allocate any
	 * size the device permits regardless of what is on screen. Anything
	 * smaller here would be a bar that a later output could walk past.
	 */
	const VkExtent2D required = {
		.width = avk.device->caps.max_image_dimension_2d,
		.height = avk.device->caps.max_image_dimension_2d,
	};
	struct wlr_drm_format_set composition;
	if (!az_dmabuf_composition_formats(&avk.importer.table, required,
			&composition)) {
		return NULL;
	}

	struct wlr_linux_dmabuf_feedback_v1 feedback = {0};
	feedback.main_device = main_device;
	struct wlr_linux_dmabuf_feedback_v1_tranche *tranche =
		wlr_linux_dmabuf_feedback_add_tranche(&feedback);
	if (tranche == NULL) {
		wlr_drm_format_set_finish(&composition);
		return NULL;
	}
	tranche->target_device = main_device;
	tranche->flags = 0;   /* composition, not scanout */
	/* The tranche owns a copy: the feedback is finished below, and
	 * az_dmabuf_advertised keeps its own for reporting. */
	for (size_t i = 0; i < composition.len; i++) {
		const struct wlr_drm_format *f = &composition.formats[i];
		for (size_t m = 0; m < f->len; m++) {
			if (!wlr_drm_format_set_add(&tranche->formats, f->format,
					f->modifiers[m])) {
				wlr_linux_dmabuf_feedback_v1_finish(&feedback);
				wlr_drm_format_set_finish(&composition);
				return NULL;
			}
		}
	}

	struct wlr_linux_dmabuf_v1 *linux_dmabuf =
		wlr_linux_dmabuf_v1_create(display, 5, &feedback);
	wlr_linux_dmabuf_feedback_v1_finish(&feedback);
	if (linux_dmabuf == NULL) {
		wlr_drm_format_set_finish(&composition);
		return NULL;
	}

	uint32_t pairs = 0;
	for (size_t i = 0; i < composition.len; i++) {
		pairs += (uint32_t)composition.formats[i].len;
	}
	az_dmabuf_advertised.active = true;
	az_dmabuf_advertised.main_device = main_device;
	az_dmabuf_advertised.composition = composition;   /* ownership moves here */
	az_dmabuf_advertised.pairs = pairs;

	wlr_log(WLR_INFO, "dmabuf: feedback source: AVK -- main device %u:%u, "
		"%zu formats, %" PRIu32 " explicit format/modifier pairs "
		"(MOD_INVALID deliberately absent)",
		major(main_device), minor(main_device), composition.len, pairs);

	/* SceneFX builds per-surface and scanout tranches; without this it would
	 * fall back to `.main_renderer = output->renderer`, which is the GLES
	 * compatibility renderer and the whole bug. */
	wlr_scene_set_linux_dmabuf_capabilities(scene, main_device, &composition);
	return linux_dmabuf;
}

/*
 * Everything needed to check the subset invariants from outside:
 *
 *     advertised_composition  subset of  AVK_importable
 *     advertised_scanout      subset of  AVK_importable  intersect  KMS_scanout
 *
 * All three sets are reported rather than the verdict, so a test asserts the
 * relationship itself instead of trusting the compositor's own opinion of it.
 */
static void az_dmabuf_add_format_set(cJSON *parent, const char *key,
		const struct wlr_drm_format_set *set) {
	cJSON *arr = cJSON_CreateArray();
	for (size_t i = 0; i < set->len; i++) {
		const struct wlr_drm_format *f = &set->formats[i];
		for (size_t m = 0; m < f->len; m++) {
			char pair[96], name[64];
			avk_drm_format_name(f->format, name, sizeof(name));
			snprintf(pair, sizeof(pair), "%s:0x%016" PRIx64, name,
				f->modifiers[m]);
			cJSON_AddItemToArray(arr, cJSON_CreateString(pair));
		}
	}
	cJSON_AddItemToObject(parent, key, arr);
}

static cJSON *az_dmabuf_feedback_json(void) {
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "source",
		az_dmabuf_advertised.active ? "avk" : "wlr_renderer");
	if (!az_dmabuf_advertised.active) {
		return o;
	}
	char dev[32];
	snprintf(dev, sizeof(dev), "%u:%u",
		major(az_dmabuf_advertised.main_device),
		minor(az_dmabuf_advertised.main_device));
	cJSON_AddStringToObject(o, "main_device", dev);
	cJSON_AddNumberToObject(o, "advertised_pairs",
		(double)az_dmabuf_advertised.pairs);
	cJSON_AddNumberToObject(o, "withheld_pairs",
		(double)az_dmabuf_withheld_pairs);
	cJSON_AddNumberToObject(o, "avk_texture_pairs_probed",
		(double)avk.importer.table.texture_pair_count);
	/*
	 * How many of AVK's importable pairs carry a size condition the protocol
	 * cannot express. Reported rather than asserted, so a test can check the
	 * rule -- "none of these is in advertised_composition" -- against the set
	 * clients were actually told about.
	 */
	cJSON_AddNumberToObject(o, "size_restricted_pairs",
		(double)az_dmabuf_size_restricted_pairs);
	cJSON_AddNumberToObject(o, "required_extent",
		(double)avk.device->caps.max_image_dimension_2d);
	cJSON *restricted = cJSON_CreateArray();
	for (uint32_t i = 0; i < avk.importer.table.count; i++) {
		const struct avk_format_caps *caps = &avk.importer.table.formats[i];
		for (uint32_t m = 0; m < caps->texture_mod_count; m++) {
			const struct avk_modifier_caps *mc = &caps->texture_mods[m];
			if (mc->max_extent.width >= avk.device->caps.max_image_dimension_2d
					&& mc->max_extent.height
						>= avk.device->caps.max_image_dimension_2d) {
				continue;
			}
			char pair[110], name[64];
			avk_drm_format_name(caps->format->drm, name, sizeof(name));
			snprintf(pair, sizeof(pair), "%s:0x%016" PRIx64 ":%ux%u", name,
				mc->modifier, mc->max_extent.width, mc->max_extent.height);
			cJSON_AddItemToArray(restricted, cJSON_CreateString(pair));
		}
	}
	cJSON_AddItemToObject(o, "avk_size_restricted", restricted);
	az_dmabuf_add_format_set(o, "advertised_composition",
		&az_dmabuf_advertised.composition);

	/* What AVK will actually accept, rebuilt from the table right now -- not a
	 * copy of what was advertised, or the subset check would be comparing a
	 * set with itself. */
	cJSON *imp = cJSON_CreateArray();
	for (uint32_t i = 0; i < avk.importer.table.count; i++) {
		const struct avk_format_caps *caps = &avk.importer.table.formats[i];
		for (uint32_t m = 0; m < caps->texture_mod_count; m++) {
			char pair[96], name[64];
			avk_drm_format_name(caps->format->drm, name, sizeof(name));
			snprintf(pair, sizeof(pair), "%s:0x%016" PRIx64, name,
				caps->texture_mods[m].modifier);
			cJSON_AddItemToArray(imp, cJSON_CreateString(pair));
		}
	}
	cJSON_AddItemToObject(o, "avk_importable", imp);

	/* Per-output scanout capability, straight from the output. */
	cJSON *outs = cJSON_CreateArray();
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output == NULL) {
			continue;
		}
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "name", m->wlr_output->name);
		const struct wlr_drm_format_set *sc =
			wlr_output_get_primary_formats(m->wlr_output, WLR_BUFFER_CAP_DMABUF);
		if (sc != NULL) {
			az_dmabuf_add_format_set(e, "kms_scanout", sc);
			struct wlr_drm_format_set inter = {0};
			if (wlr_drm_format_set_intersect(&inter, sc,
					&az_dmabuf_advertised.composition)) {
				az_dmabuf_add_format_set(e, "advertised_scanout", &inter);
				wlr_drm_format_set_finish(&inter);
			}
		} else {
			cJSON_AddNullToObject(e, "kms_scanout");
		}
		cJSON_AddItemToArray(outs, e);
	}
	cJSON_AddItemToObject(o, "outputs", outs);
	return o;
}

static void az_dmabuf_caps_finish(void) {
	if (!az_dmabuf_advertised.active) {
		return;
	}
	wlr_drm_format_set_finish(&az_dmabuf_advertised.composition);
	az_dmabuf_advertised.active = false;
}


#endif /* AZ_DMABUF_CAPS_H */
