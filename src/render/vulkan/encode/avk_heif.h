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
};

/*
 * Wrap `annexb` (parameter sets, optional SEI, one coded picture) as a HEIF
 * still. On success *out is a malloc'd file image the caller owns.
 */
bool avk_heif_wrap(const uint8_t *annexb, size_t annexb_len,
	uint32_t width, uint32_t height, const struct avk_heif_colour *colour,
	void **out, size_t *out_len);

#endif /* AVK_HEIF_H */
