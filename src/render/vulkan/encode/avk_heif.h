/*
 * avk_heif -- wrap one HEVC picture as a HEIF still.
 *
 * The encoder produces an Annex B elementary stream, which is not a file
 * anything opens. HEIF is the container a single HEVC picture belongs in, and
 * it is what an HDR screenshot has to be if it is going to be a screenshot
 * rather than a thing that needs ffmpeg explained to it.
 *
 * ── WHY THIS IS NOT JUST THE BYTES IN A BOX ───────────────────────────────
 *
 * Two conversions are unavoidable, and both are the kind that produce a file
 * that almost works:
 *
 *  1. HEIF stores NAL units LENGTH-PREFIXED, four big-endian bytes each. Annex
 *     B start codes must be stripped, not merely skipped past.
 *  2. The parameter sets do NOT go in the picture data. They go in an `hvcC`
 *     decoder configuration record inside the item's property list, and a
 *     decoder that finds them in the sample data instead sees a picture with
 *     no geometry.
 *
 * The SEI stays with the picture: it describes the pictures that follow it,
 * and in a still that is the one sample.
 *
 * Deliberately minimal. One image item, no thumbnail, no Exif, no rotation, no
 * alpha, no tiling. A screenshot needs none of those and each is another box
 * to get subtly wrong.
 */
#ifndef AVK_HEIF_H
#define AVK_HEIF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The colour description, mirroring the stream's own so a viewer that reads
 * the container rather than the bitstream reaches the same conclusion. */
struct avk_heif_colour {
	uint16_t primaries;
	uint16_t transfer;
	uint16_t matrix;
	bool full_range;
	/* HDR10 static metadata, as CONTAINER boxes.
	 *
	 * The same numbers already ride in the bitstream as SEI, and that is not
	 * enough: a HEIF reader looks at the item's properties, not inside the
	 * coded picture. libheif reported "no mastering metadata" on a file whose
	 * stream carried it, which is how a viewer ends up tone mapping against
	 * defaults it had no need to guess. Zero means absent, and then no box is
	 * written. */
	bool has_mastering;
	uint16_t master_x[3], master_y[3];   /* G, B, R in 0.00002 units */
	uint16_t white_x, white_y;
	uint32_t max_luminance;              /* 0.0001 cd/m2 */
	uint32_t min_luminance;
	uint16_t max_cll, max_fall;          /* cd/m2 */
};

/*
 * Wrap `annexb` (parameter sets, optional SEI, one coded picture) as a HEIF
 * still. On success *out is a malloc'd file image the caller owns.
 */
bool avk_heif_wrap(const uint8_t *annexb, size_t annexb_len,
	uint32_t width, uint32_t height, const struct avk_heif_colour *colour,
	void **out, size_t *out_len);

#endif /* AVK_HEIF_H */
