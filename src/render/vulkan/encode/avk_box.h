/*
 * avk_box -- ISOBMFF box writing, shared by the still and the video container.
 *
 * HEIF and MP4 are the same file format with different boxes in it, so the
 * primitives are the same: a growable buffer, big-endian writes, and a length
 * that is reserved and patched once the box's contents are known. Nesting
 * boxes by hand and counting bytes is how a container ends up four bytes out
 * somewhere in the middle, which no reader reports as anything more useful
 * than a refusal to open the file.
 *
 * Static inline in a header rather than a third translation unit: these are a
 * dozen one-line functions and giving them a .c would mean a library boundary
 * for `write four bytes big-endian`.
 */
#ifndef AVK_BOX_H
#define AVK_BOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── a growable byte buffer, because every box needs its length back ───── */

struct buf {
	uint8_t *data;
	size_t len, cap;
	bool failed;
};

static inline void put(struct buf *b, const void *src, size_t n) {
	if (b->failed) {
		return;
	}
	if (b->len + n > b->cap) {
		size_t want = (b->cap ? b->cap * 2 : 4096);
		while (want < b->len + n) {
			want *= 2;
		}
		uint8_t *grown = realloc(b->data, want);
		if (grown == NULL) {
			b->failed = true;
			return;
		}
		b->data = grown;
		b->cap = want;
	}
	memcpy(b->data + b->len, src, n);
	b->len += n;
}

static inline void u8(struct buf *b, uint8_t v) { put(b, &v, 1); }
static inline void u16(struct buf *b, uint16_t v) {
	uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
	put(b, x, 2);
}
static inline void u32(struct buf *b, uint32_t v) {
	uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
		(uint8_t)(v >> 8), (uint8_t)v};
	put(b, x, 4);
}
static inline void tag(struct buf *b, const char *t) { put(b, t, 4); }

/*
 * A box is a length it does not know yet followed by its contents, so the
 * length is reserved and patched. Nesting boxes by hand and counting bytes is
 * how a container ends up off by four somewhere in the middle.
 */
static inline size_t box_open(struct buf *b, const char *type) {
	size_t at = b->len;
	u32(b, 0);
	tag(b, type);
	return at;
}
static inline void box_close(struct buf *b, size_t at) {
	if (b->failed) {
		return;
	}
	uint32_t len = (uint32_t)(b->len - at);
	b->data[at] = (uint8_t)(len >> 24);
	b->data[at + 1] = (uint8_t)(len >> 16);
	b->data[at + 2] = (uint8_t)(len >> 8);
	b->data[at + 3] = (uint8_t)len;
}
static inline size_t full_box_open(struct buf *b, const char *type, uint8_t version,
		uint32_t flags) {
	size_t at = box_open(b, type);
	u32(b, ((uint32_t)version << 24) | (flags & 0xFFFFFF));
	return at;
}

/* ── Annex B -> NAL units ──────────────────────────────────────────────── */

struct nal {
	const uint8_t *data;
	size_t len;
	int type;
};

#define MAX_NALS 32

/* Split on start codes. Three-byte and four-byte both occur; a splitter that
 * only knows the four-byte form silently merges two NAL units into one. */
static inline size_t split_nals(const uint8_t *s, size_t n, struct nal *out,
		size_t max) {
	size_t count = 0;
	size_t i = 0;
	while (i + 3 < n && count < max) {
		size_t sc = 0;
		if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
			sc = 3;
		} else if (i + 4 < n && s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 0
				&& s[i + 3] == 1) {
			sc = 4;
		} else {
			i++;
			continue;
		}
		size_t start = i + sc;
		size_t j = start;
		while (j + 2 < n) {
			if (s[j] == 0 && s[j + 1] == 0
					&& (s[j + 2] == 1
						|| (j + 3 < n && s[j + 2] == 0 && s[j + 3] == 1))) {
				break;
			}
			j++;
		}
		size_t end = (j + 2 < n) ? j : n;
		if (end > start) {
			out[count].data = s + start;
			out[count].len = end - start;
			out[count].type = (s[start] >> 1) & 0x3F;
			count++;
		}
		i = end;
	}
	return count;
}

#endif /* AVK_BOX_H */
