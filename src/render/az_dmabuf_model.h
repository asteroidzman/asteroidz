#ifndef AZ_DMABUF_MODEL_H
#define AZ_DMABUF_MODEL_H

/*
 * What the compositor tells clients it can consume.
 *
 * THE OWNERSHIP RULE
 *
 *     wlroots implements the protocol; AVK determines the GPU capabilities.
 *
 * Before this existed, linux-dmabuf feedback was built by
 * wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw) and by SceneFX
 * passing `.main_renderer = output->renderer` -- both of which are the GLES2
 * COMPATIBILITY renderer in AVK mode. So a client asked the compositor what it
 * could consume, was answered from a renderer that composites nothing, and
 * allocated accordingly. The subsystem that then had to import the buffer was
 * never consulted.
 *
 * That is not a cosmetic inconsistency. The two sets are not the same set:
 * they are produced by different drivers' code paths with different usage
 * flags, and a format/modifier pair GLES can sample is not evidence that AVK
 * can import one. Every pair advertised from the wrong source is either a
 * missed zero-copy opportunity or a buffer the compositor must reject after
 * the client has already allocated it.
 *
 * avk_format_table is the right source and has said so in its own header since
 * it was written: every entry there is PROBED with
 * vkGetPhysicalDeviceImageFormatProperties2, with the external-memory handle
 * type chained in, rather than inferred from what the driver enumerates. This
 * file turns that table into the wlr_drm_format_set the protocol wants.
 *
 * WHAT IS DELIBERATELY NOT ADVERTISED
 *
 *   DRM_FORMAT_MOD_INVALID. It is never in the table -- no driver enumerates
 *   it -- and it must never be added. It means "the allocator did not say",
 *   which is a thing a compositor must COPE with, not a thing to ask a client
 *   for. avk_dmabuf.c has a three-rung ladder for buffers that arrive that way
 *   (recover the real modifier, else copy through a mapping); advertising
 *   MOD_INVALID as though it were a modifier would tell conforming clients to
 *   take the slow path on purpose.
 *
 *   Render-only modifiers. The table tracks texture and render capability
 *   separately because they are different questions with different usage
 *   flags. Client content is SAMPLED, so only texture_mods belong here. A
 *   modifier that is renderable but not samplable would be a promise the
 *   sampler cannot keep.
 *
 *   Formats whose colour handling is not implemented. See az_dmabuf_advertise
 *   below: import capability and correct interpretation are separate claims,
 *   and only the second one is a promise to the user.
 */

#include <wlr/render/drm_format_set.h>

#ifdef AZ_HAVE_VULKAN

/*
 * Should this format be advertised at all?
 *
 * Import capability is necessary and not sufficient. AVK can import a planar
 * YCbCr buffer -- the table probes it and the sampler has a conversion -- but
 * the compositor's colour path does not yet handle the range/matrix/transfer
 * questions that make the result CORRECT rather than merely present. M5 owns
 * that. Advertising it now would be advertising a bug.
 *
 * 10-bit RGB is the opposite case and stays: importing it is exactly what the
 * SDR path already does with the output's own 10-bit buffers, and the format's
 * interpretation is not in question. HDR *semantics* are M5's; a 10-bit buffer
 * is not an HDR claim.
 */
static bool az_dmabuf_format_advertisable(const struct avk_drm_format *fmt,
		const char **why_not) {
	if (fmt->is_ycbcr) {
		*why_not = "YCbCr: importable, but the colour path does not yet "
			"implement range/matrix/transfer correctly (M5)";
		return false;
	}
	*why_not = NULL;
	return true;
}

/*
 * The composition set: every format/modifier pair AVK has proved it can
 * import and sample.
 *
 * Returns false having logged, and leaves `out` finished, on failure. A
 * compositor that cannot describe its own capabilities must not fall back to
 * describing someone else's.
 */
static uint32_t az_dmabuf_withheld_pairs;

static bool az_dmabuf_composition_formats(const struct avk_format_table *table,
		struct wlr_drm_format_set *out) {
	*out = (struct wlr_drm_format_set){0};

	uint32_t pairs = 0, skipped_formats = 0, skipped_pairs = 0;
	for (uint32_t i = 0; i < table->count; i++) {
		const struct avk_format_caps *caps = &table->formats[i];
		const char *why_not = NULL;
		if (!az_dmabuf_format_advertisable(caps->format, &why_not)) {
			char name[64];
			avk_drm_format_name(caps->format->drm, name, sizeof(name));
			wlr_log(WLR_INFO, "dmabuf: not advertising %s -- %s", name, why_not);
			skipped_formats++;
			skipped_pairs += caps->texture_mod_count;
			continue;
		}
		for (uint32_t m = 0; m < caps->texture_mod_count; m++) {
			uint64_t mod = caps->texture_mods[m].modifier;
			/* Belt and braces: the table cannot contain it, and if it ever
			 * did, this is the line that stops it reaching a client. */
			if (mod == DRM_FORMAT_MOD_INVALID) {
				continue;
			}
			if (!wlr_drm_format_set_add(out, caps->format->drm, mod)) {
				wlr_log(WLR_ERROR, "dmabuf: failed to build the composition "
					"format set");
				wlr_drm_format_set_finish(out);
				return false;
			}
			pairs++;
		}
	}

	if (pairs == 0) {
		wlr_log(WLR_ERROR, "dmabuf: AVK reports no importable format/modifier "
			"pairs at all; refusing to advertise an empty set");
		wlr_drm_format_set_finish(out);
		return false;
	}

	/* The reverse check, stated rather than left to arithmetic: what AVK can
	 * import and we chose not to advertise. Every withheld pair should have a
	 * reason logged immediately above; a gap here that nobody can account for
	 * means the compositor is quietly narrower than its engine. */
	wlr_log(WLR_INFO, "dmabuf: composition set from AVK: %zu formats, %"
		PRIu32 " format/modifier pairs advertised; %" PRIu32 " formats / %"
		PRIu32 " pairs importable but withheld (see the lines above), out of "
		"%" PRIu32 " texture pairs AVK probed",
		out->len, pairs, skipped_formats, skipped_pairs,
		table->texture_pair_count);
	az_dmabuf_withheld_pairs = skipped_pairs;
	return true;
}

/*
 * The device clients should allocate on: the one AVK actually renders with.
 *
 * Taken from the AVK device's own DRM node rather than from the compatibility
 * renderer's. On a single-GPU machine those are the same number and the
 * distinction looks academic; this machine has two (Navi31 discrete, Raphael
 * integrated), and "which GPU should the client allocate on" is precisely the
 * question a client asks feedback in order to avoid a cross-device copy.
 * Answering it from a renderer that does no compositing is answering a
 * different question.
 */
static bool az_dmabuf_main_device(struct avk_device *dev, dev_t *out) {
	if (dev == NULL || dev->drm_fd < 0) {
		return false;
	}
	struct stat st;
	if (fstat(dev->drm_fd, &st) != 0) {
		wlr_log(WLR_ERROR, "dmabuf: cannot stat AVK's DRM fd: %s",
			strerror(errno));
		return false;
	}
	*out = st.st_rdev;
	return true;
}


#endif /* AZ_HAVE_VULKAN */

#endif /* AZ_DMABUF_MODEL_H */
