/*
 * avk_mp4 -- write an HEVC recording as an MP4 anything can play.
 *
 * The encoder produces one Annex B packet per picture. That is a stream, not a
 * file: nothing on a desktop opens a raw .h265, and the pictures carry no
 * timing at all, so even a player that decodes one has no idea what rate to
 * show it at.
 *
 * ── WHY NOT ACCUMULATE AND WRITE AT THE END ───────────────────────────────
 *
 * A recording has no length known in advance, and holding every frame of a
 * 4K60 capture in memory to write the file afterwards is minutes of recording
 * before it becomes gigabytes of RSS. So sample DATA is appended to the file
 * as it arrives and only the sample TABLE is kept -- sixteen bytes a frame,
 * which is a few megabytes an hour and is what the header at the end is built
 * from.
 *
 * That ordering is the one thing an MP4 writer has to decide, and it has a
 * consequence worth stating: the file is unplayable until it is closed,
 * because the index lives at the end. A recording that is killed rather than
 * stopped leaves an mdat nothing can read. Recovering one is a job for a
 * repair tool, not for this.
 *
 * ── WHAT IT DOES NOT DO ───────────────────────────────────────────────────
 *
 * One video track, no audio, no fragments, no edit lists, no B pictures. A
 * screen recording needs none of them, and every one is a box to get subtly
 * wrong.
 */
#ifndef AVK_MP4_H
#define AVK_MP4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct avk_heif_colour;

struct avk_mp4_sample {
	uint32_t size;
	uint32_t duration;   /* in timescale units */
	bool sync;
};

struct avk_mp4 {
	FILE *f;
	char *path;          /* owned */
	uint32_t width, height;
	uint32_t timescale;  /* units per second; 1000 means milliseconds */

	/* The decoder configuration, taken from the first picture's parameter
	 * sets. They belong in the sample description and NOT in the samples, so
	 * they are stripped out of every packet on the way past. */
	uint8_t *vps, *sps, *pps;
	size_t vps_len, sps_len, pps_len;

	struct avk_mp4_sample *samples;
	size_t sample_len, sample_cap;
	uint64_t mdat_start;     /* file offset of the mdat payload */
	uint64_t mdat_size_at;   /* file offset of its 64-bit length field */
	uint64_t mdat_bytes;

	/* Copied rather than pointed at: the caller's colour description is
	 * usually a stack local at open time and the file is written at close. */
	uint16_t primaries, transfer, matrix;
	bool full_range;
	bool has_mastering;
	uint16_t master_x[3], master_y[3];
	uint16_t white_x, white_y;
	uint32_t max_luminance, min_luminance;
	uint16_t max_cll, max_fall;
};

/*
 * Open a recording. `timescale` is the units a duration is expressed in;
 * 1000 makes durations milliseconds, which is enough for any display rate and
 * keeps the arithmetic legible in a debugger.
 */
struct avk_mp4 *avk_mp4_open(const char *path, uint32_t width,
	uint32_t height, uint32_t timescale, const struct avk_heif_colour *colour);

/*
 * Append one encoded picture. `annexb` is exactly what the encoder returned;
 * parameter sets are lifted out of it on the first sample and dropped from
 * every one after.
 */
bool avk_mp4_add_sample(struct avk_mp4 *m, const uint8_t *annexb, size_t len,
	uint32_t duration);

/* Write the index and close. The file is not playable before this. */
bool avk_mp4_close(struct avk_mp4 *m);

/* Abandon a recording: close the file and remove it. */
void avk_mp4_abort(struct avk_mp4 *m);

#endif /* AVK_MP4_H */
