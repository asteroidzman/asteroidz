#include "avk_blur_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One per capture site per frame. A frame with more blur nodes than this dumps
 * the first few and says so: the point is to look at a source, and sixteen
 * pictures is already more than anybody reads.
 */
#define AZ_BLUR_DUMP_MAX_NOTES 16

struct az_blur_dump_note {
	bool used;
	enum avk_oracle_tap_kind kind;
	size_t cmd_index;
	char tag[32];
	VkFormat format;
	/* Where the crop sits on the output, so the sidecar can say so. */
	struct avk_box capture;
	/* The box the blur's result will be composited into -- the window, for a
	 * live chain. Inside the crop, and the thing a halo is measured against. */
	struct avk_box write;
};

static struct {
	bool checked;
	/* Owned whenever it is set: the dispatch's string belongs to the IPC
	 * message and the environment's belongs to the environment, so both are
	 * copied and this frees its own. */
	char *prefix;
	int budget;
	unsigned seq;
	struct az_blur_dump_note notes[AZ_BLUR_DUMP_MAX_NOTES];
	size_t note_len;
	/* Said once per arming rather than per frame: a format this cannot write
	 * would otherwise print a line for every blur of every armed frame. */
	bool warned_format;
} dump;

void avk_blur_dump_arm(const char *prefix, int frames) {
	dump.checked = true;
	free(dump.prefix);
	dump.prefix = NULL;
	dump.note_len = 0;
	dump.warned_format = false;
	if (prefix == NULL || prefix[0] == '\0') {
		avk_log(AVK_ERROR, "avk blur dump: disarmed");
		return;
	}
	dump.prefix = strdup(prefix);
	if (dump.prefix == NULL) {
		return;
	}
	dump.budget = frames > 0 ? frames : 1;
	/* Fresh numbering per arming; see the header. */
	dump.seq = 0;
	avk_log(AVK_ERROR, "avk blur dump: armed at %s for %d frames -- each one "
		"waits for its own submission before reading it back", dump.prefix,
		dump.budget);
}

bool avk_blur_dump_armed(void) {
	if (!dump.checked) {
		dump.checked = true;
		const char *prefix = getenv("AZ_BLUR_DUMP");
		if (prefix != NULL && prefix[0] != '\0') {
			const char *frames = getenv("AZ_BLUR_DUMP_FRAMES");
			avk_blur_dump_arm(prefix, frames != NULL ? atoi(frames) : 3);
		}
	}
	return dump.prefix != NULL && dump.budget > 0;
}

void avk_blur_dump_note(enum avk_oracle_tap_kind kind, size_t cmd_index,
		const char *tag, VkFormat format, struct avk_box capture,
		struct avk_box write) {
	struct az_blur_dump_note *n = NULL;
	for (size_t i = 0; i < dump.note_len; i++) {
		if (dump.notes[i].kind == kind
				&& dump.notes[i].cmd_index == cmd_index) {
			/* The same site again: this frame's geometry replaces the last
			 * frame's, which is the only way a note left behind by a frame that
			 * never wrote can describe the wrong crop. */
			n = &dump.notes[i];
			break;
		}
	}
	if (n == NULL) {
		if (dump.note_len >= AZ_BLUR_DUMP_MAX_NOTES) {
			avk_log(AVK_ERROR, "avk blur dump: more than %d capture sites this "
				"frame; the rest are not written", AZ_BLUR_DUMP_MAX_NOTES);
			return;
		}
		n = &dump.notes[dump.note_len++];
	}
	*n = (struct az_blur_dump_note){
		.used = true,
		.kind = kind,
		.cmd_index = cmd_index,
		.format = format,
		.capture = capture,
		.write = write,
	};
	snprintf(n->tag, sizeof(n->tag), "%s", tag);
}

/*
 * ONE TEXEL, IN THE IMAGE'S OWN FORMAT, AS 8-BIT RGBA.
 *
 * Channel order comes FROM THE FORMAT, never from a guess: XR24 imports as
 * B8G8R8A8 and a dump written as if it were R8G8B8A8 swaps red and blue --
 * invisible on a grey desktop and a wrong answer everywhere else.
 *
 * The 10-bit packed formats are shifted down to 8, which loses two bits and is
 * said so in the sidecar. It is a picture to look at, not a colour measurement;
 * a run that needs codes wants the oracle's comparison instead.
 */
static bool az_dump_texel(const uint8_t *src, VkFormat format, uint8_t out[4]) {
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
		out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; out[3] = src[3];
		return true;
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
		out[0] = src[2]; out[1] = src[1]; out[2] = src[0]; out[3] = src[3];
		return true;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
		uint32_t v;
		memcpy(&v, src, sizeof(v));
		uint32_t c0 = (v >> 0) & 0x3ff;    /* R for A2B10, B for A2R10 */
		uint32_t c1 = (v >> 10) & 0x3ff;   /* G either way */
		uint32_t c2 = (v >> 20) & 0x3ff;   /* B for A2B10, R for A2R10 */
		uint32_t a = (v >> 30) & 0x3;
		bool abgr = format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		out[0] = (uint8_t)((abgr ? c0 : c2) >> 2);
		out[1] = (uint8_t)(c1 >> 2);
		out[2] = (uint8_t)((abgr ? c2 : c0) >> 2);
		out[3] = (uint8_t)(a * 85);
		return true;
	}
	default:
		return false;
	}
}

/* Whether az_dump_texel() knows this format at all. Asked before a file is
 * opened, so an unwritable format leaves no truncated .pam behind. */
static bool az_dump_format_ok(VkFormat f) {
	const uint8_t zero[4] = { 0, 0, 0, 0 };
	uint8_t out[4];
	return az_dump_texel(zero, f, out);
}

static bool az_dump_format_10bit(VkFormat f) {
	return f == VK_FORMAT_A2B10G10R10_UNORM_PACK32
		|| f == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}

static const char *az_dump_format_name(VkFormat f) {
	switch (f) {
	case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
	case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
	case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
	case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_PACK32";
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10_PACK32";
	default: return "unknown";
	}
}

static bool az_dump_one(const struct az_blur_dump_note *n, const uint8_t *px,
		struct avk_box box, uint32_t stride, unsigned seq) {
	char path[576];
	snprintf(path, sizeof(path), "%s-%u-%s.pam", dump.prefix, seq, n->tag);
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		avk_log(AVK_ERROR, "avk blur dump: cannot write %s", path);
		return false;
	}
	fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\n"
		"TUPLTYPE RGB_ALPHA\nENDHDR\n", box.width, box.height);
	uint8_t *row = malloc((size_t)box.width * 4);
	if (row == NULL) {
		fclose(f);
		return false;
	}
	for (int y = 0; y < box.height; y++) {
		const uint8_t *src = px + (size_t)y * stride;
		for (int x = 0; x < box.width; x++) {
			az_dump_texel(src + (size_t)x * 4, n->format, row + x * 4);
		}
		fwrite(row, 1, (size_t)box.width * 4, f);
	}
	free(row);
	fclose(f);

	/*
	 * The crop's own origin, so a measurement taken in the .pam can be stated
	 * in output coordinates -- and, more to the point, so the blur's write box
	 * can be located inside it without guessing. Both boxes are in OUTPUT
	 * pixels; the image starts at `capture`'s origin.
	 */
	snprintf(path, sizeof(path), "%s-%u-%s.txt", dump.prefix, seq, n->tag);
	f = fopen(path, "w");
	if (f != NULL) {
		fprintf(f,
			"tag %s\n"
			"cmd_index %zu\n"
			"image %d %d\n"
			"capture %d %d %d %d\n"
			"write %d %d %d %d\n"
			"format %s\n"
			"channels rgba8%s\n",
			n->tag, n->cmd_index, box.width, box.height,
			n->capture.x, n->capture.y, n->capture.width, n->capture.height,
			n->write.x, n->write.y, n->write.width, n->write.height,
			az_dump_format_name(n->format),
			az_dump_format_10bit(n->format)
				? " (10-bit source, shifted down 2)" : "");
		fclose(f);
	}
	avk_log(AVK_ERROR, "avk blur dump: %s-%u-%s.pam %dx%d capture %d,%d %dx%d "
		"write %d,%d %dx%d", dump.prefix, seq, n->tag, box.width, box.height,
		n->capture.x, n->capture.y, n->capture.width, n->capture.height,
		n->write.x, n->write.y, n->write.width, n->write.height);
	return true;
}

void avk_blur_dump_write(struct avk_oracle *o) {
	if (dump.prefix == NULL || dump.budget <= 0 || o->mapped == NULL) {
		return;
	}
	/*
	 * THE PRODUCTION RENDER ONLY.
	 *
	 * Under AZ_FRAME_ORACLE every frame is rendered twice through this same
	 * function, and the production pass's slots survive into the reference one
	 * -- so without this the reference render would write the production
	 * pixels a second time under the next sequence number and spend two frames
	 * of the budget on one frame of the session.
	 */
	if (o->pass != AVK_ORACLE_PRODUCTION) {
		return;
	}
	bool wrote = false;
	for (size_t i = 0; i < o->slot_len; i++) {
		const struct avk_oracle_slot *s = &o->slots[i];
		if (!s->valid || s->pass != AVK_ORACLE_PRODUCTION) {
			continue;
		}
		for (size_t j = 0; j < dump.note_len; j++) {
			const struct az_blur_dump_note *n = &dump.notes[j];
			if (!n->used || n->kind != s->kind
					|| n->cmd_index != s->cmd_index) {
				continue;
			}
			if (!az_dump_format_ok(n->format)) {
				if (!dump.warned_format) {
					dump.warned_format = true;
					avk_log(AVK_ERROR, "avk blur dump: %s cannot be written as "
						"8-bit RGBA; nothing captured",
						az_dump_format_name(n->format));
				}
				continue;
			}
			wrote |= az_dump_one(n, (const uint8_t *)o->mapped + s->offset,
				s->box, s->stride, dump.seq);
			break;
		}
	}
	/* The notes describe THIS frame. Whatever was not matched belongs to a
	 * frame that never reached a submission, and carrying it forward would
	 * label the next frame's tap with the last one's geometry. */
	dump.note_len = 0;
	if (!wrote) {
		return;
	}
	dump.seq++;
	if (--dump.budget <= 0) {
		/* Disarmed HERE, not merely exhausted: avk_blur_dump_armed() is what
		 * adds TRANSFER_SRC to the prefix transients and declares the taps, so
		 * leaving it true would go on paying for a capture nothing writes. */
		free(dump.prefix);
		dump.prefix = NULL;
		avk_log(AVK_ERROR, "avk blur dump: budget spent, disarmed");
	}
}
