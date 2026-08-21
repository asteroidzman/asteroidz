#define _GNU_SOURCE
#include "render/vulkan/encode/avk_heif.h"

#include <stdlib.h>
#include <string.h>

#include "render/vulkan/avk.h"

/* ── a growable byte buffer, because every box needs its length back ───── */

struct buf {
	uint8_t *data;
	size_t len, cap;
	bool failed;
};

static void put(struct buf *b, const void *src, size_t n) {
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

static void u8(struct buf *b, uint8_t v) { put(b, &v, 1); }
static void u16(struct buf *b, uint16_t v) {
	uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
	put(b, x, 2);
}
static void u32(struct buf *b, uint32_t v) {
	uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
		(uint8_t)(v >> 8), (uint8_t)v};
	put(b, x, 4);
}
static void tag(struct buf *b, const char *t) { put(b, t, 4); }

/*
 * A box is a length it does not know yet followed by its contents, so the
 * length is reserved and patched. Nesting boxes by hand and counting bytes is
 * how a container ends up off by four somewhere in the middle.
 */
static size_t box_open(struct buf *b, const char *type) {
	size_t at = b->len;
	u32(b, 0);
	tag(b, type);
	return at;
}
static void box_close(struct buf *b, size_t at) {
	if (b->failed) {
		return;
	}
	uint32_t len = (uint32_t)(b->len - at);
	b->data[at] = (uint8_t)(len >> 24);
	b->data[at + 1] = (uint8_t)(len >> 16);
	b->data[at + 2] = (uint8_t)(len >> 8);
	b->data[at + 3] = (uint8_t)len;
}
static size_t full_box_open(struct buf *b, const char *type, uint8_t version,
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
static size_t split_nals(const uint8_t *s, size_t n, struct nal *out,
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

/*
 * hvcC -- the decoder configuration record.
 *
 * This is where the VPS, SPS and PPS live in a HEIF file, and the profile and
 * level beside them are not decoration. libheif configures its decoder from
 * this record rather than from the SPS, so a record claiming profile 0 at
 * level 0 produces a file whose metadata reads perfectly and whose image will
 * not decode -- which is exactly what the first version of this did.
 */
static void write_hvcc(struct buf *b, const struct nal *nals, size_t count,
		uint8_t level_idc) {
	size_t at = box_open(b, "hvcC");
	u8(b, 1);                    /* configurationVersion */
	/* profile_space 0, tier 0, profile_idc 2 (Main 10). Zeros here are not
	 * "unspecified": a decoder configured from this record rather than from
	 * the SPS is told the stream is profile 0 at level 0, and refuses it. */
	u8(b, 2);
	u32(b, 0x20000000);          /* compatible with profile 2 */
	/* general_constraint_indicator_flags: progressive source, frame only. */
	u8(b, 0x90);
	for (int i = 0; i < 5; i++) {
		u8(b, 0);
	}
	u8(b, level_idc);
	u16(b, 0xF000);              /* min_spatial_segmentation_idc */
	u8(b, 0xFC);                 /* parallelismType */
	u8(b, 0xFC | 1);             /* chromaFormat 4:2:0 */
	u8(b, 0xF8 | 2);             /* bitDepthLumaMinus8 = 2 */
	u8(b, 0xF8 | 2);             /* bitDepthChromaMinus8 = 2 */
	u16(b, 0);                   /* avgFrameRate */
	/* constantFrameRate 0, numTemporalLayers 1, temporalIdNested 1,
	 * lengthSizeMinusOne 3 (four-byte lengths) */
	u8(b, (0 << 6) | (1 << 3) | (1 << 2) | 3);

	/* One array per parameter set type, in VPS, SPS, PPS order. */
	static const int want[3] = {32, 33, 34};
	uint8_t arrays = 0;
	for (int k = 0; k < 3; k++) {
		for (size_t i = 0; i < count; i++) {
			if (nals[i].type == want[k]) {
				arrays++;
				break;
			}
		}
	}
	u8(b, arrays);
	for (int k = 0; k < 3; k++) {
		size_t n = 0;
		for (size_t i = 0; i < count; i++) {
			if (nals[i].type == want[k]) {
				n++;
			}
		}
		if (n == 0) {
			continue;
		}
		u8(b, (uint8_t)(0x80 | want[k]));   /* array_completeness | type */
		u16(b, (uint16_t)n);
		for (size_t i = 0; i < count; i++) {
			if (nals[i].type != want[k]) {
				continue;
			}
			u16(b, (uint16_t)nals[i].len);
			put(b, nals[i].data, nals[i].len);
		}
	}
	box_close(b, at);
}

bool avk_heif_wrap(const uint8_t *annexb, size_t annexb_len,
		uint32_t width, uint32_t height,
		const struct avk_heif_colour *colour, void **out, size_t *out_len) {
	/* The level, by luma sample count, matching what the encoder wrote into
	 * the SPS. general_level_idc is thirty times the level number. */
	uint64_t samples = (uint64_t)width * height;
	uint8_t level_idc = samples <= 552960 ? 93
		: samples <= 2228224 ? 123
		: samples <= 8912896 ? 153 : 183;
	if (annexb == NULL || out == NULL || out_len == NULL) {
		return false;
	}
	struct nal nals[MAX_NALS];
	size_t count = split_nals(annexb, annexb_len, nals, MAX_NALS);
	if (count == 0) {
		avk_log(AVK_ERROR, "heif: no NAL units in the stream");
		return false;
	}

	/* The picture data: everything that is not a parameter set, length
	 * prefixed. The SEI travels with the picture rather than into hvcC. */
	struct buf mdat_payload = {0};
	bool have_picture = false;
	for (size_t i = 0; i < count; i++) {
		int t = nals[i].type;
		if (t == 32 || t == 33 || t == 34) {
			continue;
		}
		u32(&mdat_payload, (uint32_t)nals[i].len);
		put(&mdat_payload, nals[i].data, nals[i].len);
		if (t <= 31) {
			have_picture = true;
		}
	}
	if (!have_picture || mdat_payload.failed) {
		free(mdat_payload.data);
		avk_log(AVK_ERROR, "heif: the stream carries no coded picture");
		return false;
	}

	struct buf f = {0};

	size_t at = box_open(&f, "ftyp");
	tag(&f, "heic");
	u32(&f, 0);
	tag(&f, "mif1");
	tag(&f, "heic");
	box_close(&f, at);

	/* The meta box describes the item; the mdat holds its bytes. iloc has to
	 * name the mdat's absolute file offset, which is not known until the meta
	 * box is finished -- so the offset field is patched afterwards rather
	 * than guessed. */
	size_t iloc_offset_field = 0;
	size_t meta_at = full_box_open(&f, "meta", 0, 0);
	{
		size_t h = full_box_open(&f, "hdlr", 0, 0);
		u32(&f, 0);
		tag(&f, "pict");
		u32(&f, 0); u32(&f, 0); u32(&f, 0);
		u8(&f, 0);
		box_close(&f, h);

		size_t p = full_box_open(&f, "pitm", 0, 0);
		u16(&f, 1);
		box_close(&f, p);

		size_t il = full_box_open(&f, "iloc", 0, 0);
		/* offset_size 4, length_size 4. Declaring length_size 0 and then
		 * writing a four-byte length is not a size mismatch a parser
		 * reports -- it reads the extent as unbounded and hands the decoder
		 * four bytes of length as though they were picture data. */
		u8(&f, (4 << 4) | 4);
		u8(&f, (4 << 4) | 0);    /* base_offset_size 4, reserved */
		u16(&f, 1);              /* item_count */
		u16(&f, 1);              /* item_ID */
		u16(&f, 0);              /* data_reference_index */
		u32(&f, 0);              /* base_offset, patched below */
		iloc_offset_field = f.len - 4;
		u16(&f, 1);              /* extent_count */
		u32(&f, 0);              /* extent_offset */
		u32(&f, (uint32_t)mdat_payload.len);
		box_close(&f, il);

		size_t ii = full_box_open(&f, "iinf", 0, 0);
		u16(&f, 1);
		{
			size_t ie = full_box_open(&f, "infe", 2, 0);
			u16(&f, 1);          /* item_ID */
			u16(&f, 0);          /* item_protection_index */
			tag(&f, "hvc1");
			put(&f, "", 1);      /* item_name, empty and NUL terminated */
			box_close(&f, ie);
		}
		box_close(&f, ii);

		size_t ip = box_open(&f, "iprp");
		{
			size_t co = box_open(&f, "ipco");
			write_hvcc(&f, nals, count, level_idc);
			{
				size_t sp = full_box_open(&f, "ispe", 0, 0);
				u32(&f, width);
				u32(&f, height);
				box_close(&f, sp);
			}
			{
				/* nclx: the same colour description the bitstream carries.
				 * A viewer is entitled to read either. */
				size_t cl = box_open(&f, "colr");
				tag(&f, "nclx");
				u16(&f, colour->primaries);
				u16(&f, colour->transfer);
				u16(&f, colour->matrix);
				u8(&f, colour->full_range ? 0x80 : 0x00);
				box_close(&f, cl);
			}
			box_close(&f, co);

			size_t ma = full_box_open(&f, "ipma", 0, 0);
			u32(&f, 1);          /* entry_count */
			u16(&f, 1);          /* item_ID */
			u8(&f, 3);           /* association_count */
			u8(&f, 0x80 | 1);    /* essential | hvcC */
			u8(&f, 0x80 | 2);    /* essential | ispe */
			u8(&f, 3);           /* colr, not essential */
			box_close(&f, ma);
		}
		box_close(&f, ip);
	}
	box_close(&f, meta_at);

	size_t mdat_at = box_open(&f, "mdat");
	size_t payload_at = f.len;
	put(&f, mdat_payload.data, mdat_payload.len);
	box_close(&f, mdat_at);
	free(mdat_payload.data);

	if (f.failed) {
		free(f.data);
		return false;
	}
	/* The patch iloc was waiting for. */
	f.data[iloc_offset_field] = (uint8_t)(payload_at >> 24);
	f.data[iloc_offset_field + 1] = (uint8_t)(payload_at >> 16);
	f.data[iloc_offset_field + 2] = (uint8_t)(payload_at >> 8);
	f.data[iloc_offset_field + 3] = (uint8_t)payload_at;

	*out = f.data;
	*out_len = f.len;
	return true;
}
