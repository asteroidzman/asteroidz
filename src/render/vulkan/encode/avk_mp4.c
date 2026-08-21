#define _GNU_SOURCE
#include "render/vulkan/encode/avk_mp4.h"

#include "render/vulkan/avk.h"
#include "render/vulkan/encode/avk_box.h"
#include "render/vulkan/encode/avk_heif.h"

/* ── Annex B in, length-prefixed out ────────────────────────────────────── */

static bool keep_nal(int type) {
	/* 32/33/34 are VPS/SPS/PPS and belong in the sample description; 35 is an
	 * access unit delimiter and carries nothing a container needs. Everything
	 * else, SEI included, travels with the picture. */
	return type != 32 && type != 33 && type != 34 && type != 35;
}

static void store_param(uint8_t **dst, size_t *dst_len, const uint8_t *src,
		size_t len) {
	if (*dst != NULL) {
		return;   /* the first one wins; later pictures repeat them */
	}
	*dst = malloc(len);
	if (*dst == NULL) {
		return;
	}
	memcpy(*dst, src, len);
	*dst_len = len;
}

/* ── open ───────────────────────────────────────────────────────────────── */

struct avk_mp4 *avk_mp4_open(const char *path, uint32_t width,
		uint32_t height, uint32_t timescale,
		const struct avk_heif_colour *colour) {
	if (path == NULL || width == 0 || height == 0 || timescale == 0) {
		return NULL;
	}
	struct avk_mp4 *m = calloc(1, sizeof(*m));
	if (m == NULL) {
		return NULL;
	}
	m->f = fopen(path, "wb");
	if (m->f == NULL) {
		avk_log(AVK_ERROR, "mp4: cannot create %s", path);
		free(m);
		return NULL;
	}
	m->path = strdup(path);
	m->width = width;
	m->height = height;
	m->timescale = timescale;
	if (colour != NULL) {
		m->primaries = colour->primaries;
		m->transfer = colour->transfer;
		m->matrix = colour->matrix;
		m->full_range = colour->full_range;
		m->has_mastering = colour->has_mastering;
		for (int i = 0; i < 3; i++) {
			m->master_x[i] = colour->master_x[i];
			m->master_y[i] = colour->master_y[i];
		}
		m->white_x = colour->white_x;
		m->white_y = colour->white_y;
		m->max_luminance = colour->max_luminance;
		m->min_luminance = colour->min_luminance;
		m->max_cll = colour->max_cll;
		m->max_fall = colour->max_fall;
	}

	struct buf f = {0};
	size_t at = box_open(&f, "ftyp");
	tag(&f, "isom");
	u32(&f, 0x200);
	tag(&f, "isom");
	tag(&f, "iso2");
	tag(&f, "mp41");
	tag(&f, "hvc1");
	box_close(&f, at);

	/*
	 * A 64-bit mdat, always. The 32-bit form saves eight bytes and caps a
	 * recording at 4GB, which a 4K capture reaches in minutes -- and it fails
	 * by writing a length field that wrapped, not by refusing.
	 */
	u32(&f, 1);           /* size 1 means "the real size is the 64-bit one" */
	tag(&f, "mdat");
	/* WHERE the length lives, recorded rather than computed later. The first
	 * version seeked to a hardcoded offset 8 -- which is inside ftyp, not
	 * mdat -- and produced a file whose every box was correct and which no
	 * player would open: "moov atom not found", because the mdat claimed a
	 * length of zero and swallowed the index. */
	size_t size_field = f.len;
	u32(&f, 0);           /* largesize, high word, patched at close */
	u32(&f, 0);           /* largesize, low word */

	if (f.failed || fwrite(f.data, f.len, 1, m->f) != 1) {
		free(f.data);
		avk_mp4_abort(m);
		return NULL;
	}
	m->mdat_size_at = size_field;
	m->mdat_start = f.len;   /* payload begins right after the header */
	free(f.data);
	return m;
}

/* ── samples ────────────────────────────────────────────────────────────── */

bool avk_mp4_add_sample(struct avk_mp4 *m, const uint8_t *annexb, size_t len,
		uint32_t duration) {
	if (m == NULL || m->f == NULL || annexb == NULL) {
		return false;
	}
	struct nal nals[32];
	size_t count = split_nals(annexb, len, nals, 32);
	if (count == 0) {
		avk_log(AVK_ERROR, "mp4: a packet with no NAL units in it");
		return false;
	}

	struct buf payload = {0};
	bool sync = false;
	for (size_t i = 0; i < count; i++) {
		switch (nals[i].type) {
		case 32: store_param(&m->vps, &m->vps_len, nals[i].data,
			nals[i].len); continue;
		case 33: store_param(&m->sps, &m->sps_len, nals[i].data,
			nals[i].len); continue;
		case 34: store_param(&m->pps, &m->pps_len, nals[i].data,
			nals[i].len); continue;
		default: break;
		}
		if (!keep_nal(nals[i].type)) {
			continue;
		}
		/* 16..21 are the IRAP types; a sample containing one can be seeked
		 * to, and stss lists exactly those. A recording whose stss is wrong
		 * plays correctly and seeks to the wrong place. */
		if (nals[i].type >= 16 && nals[i].type <= 21) {
			sync = true;
		}
		u32(&payload, (uint32_t)nals[i].len);
		put(&payload, nals[i].data, nals[i].len);
	}
	if (payload.failed || payload.len == 0) {
		free(payload.data);
		avk_log(AVK_ERROR, "mp4: the packet carried no coded picture");
		return false;
	}

	if (m->sample_len == m->sample_cap) {
		size_t want = m->sample_cap ? m->sample_cap * 2 : 256;
		struct avk_mp4_sample *grown = realloc(m->samples,
			want * sizeof(*grown));
		if (grown == NULL) {
			free(payload.data);
			return false;
		}
		m->samples = grown;
		m->sample_cap = want;
	}
	if (fwrite(payload.data, payload.len, 1, m->f) != 1) {
		free(payload.data);
		avk_log(AVK_ERROR, "mp4: short write on %s", m->path);
		return false;
	}
	m->samples[m->sample_len++] = (struct avk_mp4_sample){
		.size = (uint32_t)payload.len,
		.duration = duration,
		.sync = sync,
	};
	m->mdat_bytes += payload.len;
	free(payload.data);
	return true;
}

/* ── the index ──────────────────────────────────────────────────────────── */

static void write_hvcc(struct buf *b, const struct avk_mp4 *m,
		uint8_t level_idc) {
	size_t at = box_open(b, "hvcC");
	u8(b, 1);
	u8(b, 2);                    /* profile_space 0, tier 0, Main 10 */
	u32(b, 0x20000000);
	u8(b, 0x90);
	for (int i = 0; i < 5; i++) {
		u8(b, 0);
	}
	u8(b, level_idc);
	u16(b, 0xF000);
	u8(b, 0xFC);
	u8(b, 0xFC | 1);             /* 4:2:0 */
	u8(b, 0xF8 | 2);             /* 10-bit luma */
	u8(b, 0xF8 | 2);             /* 10-bit chroma */
	u16(b, 0);
	u8(b, (0 << 6) | (1 << 3) | (1 << 2) | 3);   /* 4-byte NAL lengths */

	const struct { const uint8_t *p; size_t len; int type; } sets[3] = {
		{m->vps, m->vps_len, 32},
		{m->sps, m->sps_len, 33},
		{m->pps, m->pps_len, 34},
	};
	uint8_t arrays = 0;
	for (int i = 0; i < 3; i++) {
		if (sets[i].p != NULL) {
			arrays++;
		}
	}
	u8(b, arrays);
	for (int i = 0; i < 3; i++) {
		if (sets[i].p == NULL) {
			continue;
		}
		u8(b, (uint8_t)(0x80 | sets[i].type));
		u16(b, 1);
		u16(b, (uint16_t)sets[i].len);
		put(b, sets[i].p, sets[i].len);
	}
	box_close(b, at);
}

static void write_stbl(struct buf *b, const struct avk_mp4 *m,
		uint8_t level_idc, uint64_t chunk_offset) {
	size_t stbl = box_open(b, "stbl");

	size_t stsd = full_box_open(b, "stsd", 0, 0);
	u32(b, 1);
	{
		size_t e = box_open(b, "hvc1");
		for (int i = 0; i < 6; i++) {
			u8(b, 0);
		}
		u16(b, 1);               /* data_reference_index */
		u16(b, 0); u16(b, 0);    /* pre_defined, reserved */
		u32(b, 0); u32(b, 0); u32(b, 0);
		u16(b, (uint16_t)m->width);
		u16(b, (uint16_t)m->height);
		u32(b, 0x00480000);      /* 72 dpi */
		u32(b, 0x00480000);
		u32(b, 0);
		u16(b, 1);               /* frame_count */
		for (int i = 0; i < 32; i++) {
			u8(b, 0);            /* compressorname */
		}
		u16(b, 0x0018);          /* depth */
		u16(b, 0xFFFF);          /* pre_defined = -1 */
		write_hvcc(b, m, level_idc);
		{
			/* The same colour description the bitstream carries. A player
			 * that reads the container and never the VUI must reach the same
			 * conclusion, or the recording is HDR in one and SDR in the
			 * other depending on which was consulted. */
			size_t cl = box_open(b, "colr");
			tag(b, "nclx");
			u16(b, m->primaries);
			u16(b, m->transfer);
			u16(b, m->matrix);
			u8(b, m->full_range ? 0x80 : 0x00);
			box_close(b, cl);
		}
		if (m->has_mastering) {
			size_t md = box_open(b, "mdcv");
			for (int i = 0; i < 3; i++) {
				u16(b, m->master_x[i]);
				u16(b, m->master_y[i]);
			}
			u16(b, m->white_x);
			u16(b, m->white_y);
			u32(b, m->max_luminance);
			u32(b, m->min_luminance);
			box_close(b, md);

			size_t cli = box_open(b, "clli");
			u16(b, m->max_cll);
			u16(b, m->max_fall);
			box_close(b, cli);
		}
		box_close(b, e);
	}
	box_close(b, stsd);

	/* stts, run-length encoded: a recording at a steady rate is one entry,
	 * and one that dropped frames is a handful. Writing one entry per sample
	 * would work and would be a megabyte an hour of nothing. */
	size_t stts = full_box_open(b, "stts", 0, 0);
	size_t stts_count_at = b->len;
	u32(b, 0);
	uint32_t runs = 0;
	for (size_t i = 0; i < m->sample_len;) {
		uint32_t d = m->samples[i].duration;
		size_t j = i;
		while (j < m->sample_len && m->samples[j].duration == d) {
			j++;
		}
		u32(b, (uint32_t)(j - i));
		u32(b, d);
		runs++;
		i = j;
	}
	if (!b->failed) {
		b->data[stts_count_at] = (uint8_t)(runs >> 24);
		b->data[stts_count_at + 1] = (uint8_t)(runs >> 16);
		b->data[stts_count_at + 2] = (uint8_t)(runs >> 8);
		b->data[stts_count_at + 3] = (uint8_t)runs;
	}
	box_close(b, stts);

	/* stss: which samples can be seeked to. Omitted entirely when every
	 * sample is a sync sample, which is what the spec means by its absence --
	 * writing one that lists all of them says the same thing at length. */
	uint32_t syncs = 0;
	for (size_t i = 0; i < m->sample_len; i++) {
		if (m->samples[i].sync) {
			syncs++;
		}
	}
	if (syncs > 0 && syncs < m->sample_len) {
		size_t stss = full_box_open(b, "stss", 0, 0);
		u32(b, syncs);
		for (size_t i = 0; i < m->sample_len; i++) {
			if (m->samples[i].sync) {
				u32(b, (uint32_t)(i + 1));   /* 1-based */
			}
		}
		box_close(b, stss);
	}

	/* One chunk holding every sample: the data was written contiguously as it
	 * arrived, so there is exactly one run of it. */
	size_t stsc = full_box_open(b, "stsc", 0, 0);
	u32(b, 1);
	u32(b, 1);                          /* first_chunk */
	u32(b, (uint32_t)m->sample_len);    /* samples_per_chunk */
	u32(b, 1);                          /* sample_description_index */
	box_close(b, stsc);

	size_t stsz = full_box_open(b, "stsz", 0, 0);
	u32(b, 0);                          /* sizes differ, so they are listed */
	u32(b, (uint32_t)m->sample_len);
	for (size_t i = 0; i < m->sample_len; i++) {
		u32(b, m->samples[i].size);
	}
	box_close(b, stsz);

	/* co64 rather than stco: the payload starts at 16 bytes and a long
	 * recording puts later chunks past 4GB. There is one chunk here so it
	 * cannot happen yet, and the format that cannot express the problem is
	 * the wrong one to grow into. */
	size_t co64 = full_box_open(b, "co64", 0, 0);
	u32(b, 1);
	u32(b, (uint32_t)(chunk_offset >> 32));
	u32(b, (uint32_t)chunk_offset);
	box_close(b, co64);

	box_close(b, stbl);
}

bool avk_mp4_close(struct avk_mp4 *m) {
	if (m == NULL) {
		return false;
	}
	bool ok = false;
	if (m->f == NULL || m->sample_len == 0 || m->sps == NULL) {
		avk_log(AVK_ERROR, "mp4: nothing was recorded");
		goto out;
	}

	uint64_t total = 0;
	for (size_t i = 0; i < m->sample_len; i++) {
		total += m->samples[i].duration;
	}
	uint64_t luma = (uint64_t)m->width * m->height;
	uint8_t level_idc = luma <= 552960 ? 93
		: luma <= 2228224 ? 123
		: luma <= 8912896 ? 153 : 183;

	struct buf b = {0};
	size_t moov = box_open(&b, "moov");
	{
		size_t mvhd = full_box_open(&b, "mvhd", 0, 0);
		u32(&b, 0); u32(&b, 0);          /* creation, modification */
		u32(&b, m->timescale);
		u32(&b, (uint32_t)total);
		u32(&b, 0x00010000);             /* rate 1.0 */
		u16(&b, 0x0100);                 /* volume 1.0 */
		u16(&b, 0);
		u32(&b, 0); u32(&b, 0);
		/* the identity matrix, which every reader expects to find here */
		u32(&b, 0x00010000); u32(&b, 0); u32(&b, 0);
		u32(&b, 0); u32(&b, 0x00010000); u32(&b, 0);
		u32(&b, 0); u32(&b, 0); u32(&b, 0x40000000);
		for (int i = 0; i < 6; i++) {
			u32(&b, 0);                  /* pre_defined */
		}
		u32(&b, 2);                      /* next_track_ID */
		box_close(&b, mvhd);

		size_t trak = box_open(&b, "trak");
		{
			/* flags 3: enabled and in the movie. A track that is neither is
			 * present in the file and played by nothing. */
			size_t tkhd = full_box_open(&b, "tkhd", 0, 3);
			u32(&b, 0); u32(&b, 0);
			u32(&b, 1);                  /* track_ID */
			u32(&b, 0);
			u32(&b, (uint32_t)total);
			u32(&b, 0); u32(&b, 0);
			u16(&b, 0); u16(&b, 0);      /* layer, alternate_group */
			u16(&b, 0); u16(&b, 0);      /* volume, reserved */
			u32(&b, 0x00010000); u32(&b, 0); u32(&b, 0);
			u32(&b, 0); u32(&b, 0x00010000); u32(&b, 0);
			u32(&b, 0); u32(&b, 0); u32(&b, 0x40000000);
			u32(&b, m->width << 16);     /* 16.16 fixed point */
			u32(&b, m->height << 16);
			box_close(&b, tkhd);

			size_t mdia = box_open(&b, "mdia");
			{
				size_t mdhd = full_box_open(&b, "mdhd", 0, 0);
				u32(&b, 0); u32(&b, 0);
				u32(&b, m->timescale);
				u32(&b, (uint32_t)total);
				u16(&b, 0x55C4);         /* language: und */
				u16(&b, 0);
				box_close(&b, mdhd);

				size_t hdlr = full_box_open(&b, "hdlr", 0, 0);
				u32(&b, 0);
				tag(&b, "vide");
				u32(&b, 0); u32(&b, 0); u32(&b, 0);
				put(&b, "avk", 4);       /* name, NUL terminated */
				box_close(&b, hdlr);

				size_t minf = box_open(&b, "minf");
				{
					size_t vmhd = full_box_open(&b, "vmhd", 0, 1);
					u16(&b, 0);
					u16(&b, 0); u16(&b, 0); u16(&b, 0);
					box_close(&b, vmhd);

					size_t dinf = box_open(&b, "dinf");
					size_t dref = full_box_open(&b, "dref", 0, 0);
					u32(&b, 1);
					size_t url = full_box_open(&b, "url ", 0, 1);
					box_close(&b, url);
					box_close(&b, dref);
					box_close(&b, dinf);

					write_stbl(&b, m, level_idc, m->mdat_start);
				}
				box_close(&b, minf);
			}
			box_close(&b, mdia);
		}
		box_close(&b, trak);
	}
	box_close(&b, moov);

	if (b.failed || fwrite(b.data, b.len, 1, m->f) != 1) {
		free(b.data);
		avk_log(AVK_ERROR, "mp4: could not write the index");
		goto out;
	}
	free(b.data);

	/* The mdat length, now that it is known. */
	uint64_t mdat_size = m->mdat_bytes + 16;
	uint8_t sz[8];
	for (int i = 0; i < 8; i++) {
		sz[i] = (uint8_t)(mdat_size >> (56 - i * 8));
	}
	if (fseek(m->f, (long)m->mdat_size_at, SEEK_SET) != 0
			|| fwrite(sz, sizeof(sz), 1, m->f) != 1) {
		avk_log(AVK_ERROR, "mp4: could not patch the mdat length");
		goto out;
	}
	avk_log(AVK_INFO, "mp4: %s, %zu frames, %llu bytes of video", m->path,
		m->sample_len, (unsigned long long)m->mdat_bytes);
	ok = true;

out:
	if (m->f != NULL) {
		fclose(m->f);
	}
	free(m->path);
	free(m->vps);
	free(m->sps);
	free(m->pps);
	free(m->samples);
	free(m);
	return ok;
}

void avk_mp4_abort(struct avk_mp4 *m) {
	if (m == NULL) {
		return;
	}
	if (m->f != NULL) {
		fclose(m->f);
	}
	if (m->path != NULL) {
		remove(m->path);
		free(m->path);
	}
	free(m->vps);
	free(m->sps);
	free(m->pps);
	free(m->samples);
	free(m);
}
