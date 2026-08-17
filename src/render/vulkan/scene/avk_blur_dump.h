#ifndef AVK_BLUR_DUMP_H
#define AVK_BLUR_DUMP_H

/*
 * ── THE BLUR SOURCE DUMP ──────────────────────────────────────────────────
 *
 * Write a live blur's SOURCE image to disk, so what the blur is about to spread
 * can be LOOKED AT instead of reasoned about.
 *
 * A backdrop blur's source is the scene so far -- the commands below the blur
 * node, replayed into an image the chain then samples. Every question about the
 * halo around a floating window (is the hole filled, with what, does the fill
 * reach far enough, is that wallpaper the CURRENT wallpaper) is a question about
 * the contents of that image at that moment, and nothing downstream of it can
 * answer them: by the time anything is on screen the blur has already averaged
 * the evidence away. This project has closed two bugs with it -- the shadow-blur
 * glow, and the background cache serving a wallpaper several rotations old.
 *
 * ── WHAT AVK HAS TO CAPTURE, AND WHAT IT DOES NOT ─────────────────────────
 *
 * SceneFX's fx_vk captured the same image TWICE: once as copied, and again
 * after blur_exclude_from_source() had patched a synthetic fill over the
 * window's own pixels so they would not appear in their own shadow. AVK has no
 * such stage and this does not invent one. Its source is the scene PREFIX --
 * commands [0, k) for the blur at index k -- so the window is not in its own
 * source to begin with and there is nothing to patch out. See the darken-clamp
 * comment in avk_blur.h, which argues the same divergence from the other side.
 *
 * What AVK does have is TWO PRODUCERS of a blur source, and both are captured
 * because a dump that silently covered only one would answer the cache question
 * with the live path's picture:
 *
 *   live<k>       the per-node prefix transient, after the segment has replayed
 *                 scene[0, k) into it and BEFORE the dual-Kawase chain runs.
 *   cache-plain   the monitor background cache image, after the [0, prefix_end)
 *   cache-dark    replay and before the chain that blurs it in place. One tag
 *                 per kind, because the two are separate images with separate
 *                 identities and conflating them is exactly the defect that
 *                 made the dark image serve last week's wallpaper.
 *
 * A cache tag is ABSENT on a frame that HIT the cache, and that absence is
 * itself the finding: nothing rebuilt, so the pixels on screen came from an
 * image built some earlier frame. The count of files is data.
 *
 * ── HOW IT READS THE IMAGE ────────────────────────────────────────────────
 *
 * Through avk_oracle_tap(): a copy declared as a GRAPH PASS, so the barriers
 * around it -- including waiting for the segment's colour writes to land before
 * the transfer reads them -- are the graph's to derive and this file contains no
 * synchronisation of its own. There is exactly one readback path in AVK and this
 * is a second consumer of it, not a second implementation.
 *
 * ── PAM, NOT PNG, AND NOT PPM ─────────────────────────────────────────────
 *
 * Binary PAM: a short ASCII header and raw RGBA, which needs no decoder, no
 * zlib and no filter reconstruction (the harness has already had one
 * hand-written PNG reader produce a failure that read like "the two images are
 * different sizes"). RGBA rather than the oracle's RGB PPM because the source is
 * premultiplied and the ALPHA IS PART OF THE EVIDENCE: a hole filled with
 * something fully transparent is identical to one filled correctly once alpha
 * has been thrown away.
 *
 * Beside each image a `.txt` sidecar names the boxes, in OUTPUT coordinates,
 * that the image is a crop of -- so a measurement taken in the .pam can be
 * stated in screen coordinates and the window's own box located inside it
 * without guessing.
 *
 * ── IT MUST STOP ON ITS OWN ───────────────────────────────────────────────
 *
 * Every armed frame ends in a wait on that frame's submission before the mapped
 * memory is read. That is a real stall, and a forgotten arming would otherwise
 * turn a live session into a slideshow -- so the arming carries a FRAME BUDGET
 * and disarms itself when it is spent.
 *
 * Off unless armed. Two ways to arm, and both exist for a reason:
 *
 *   amsg dispatch dump_blur_source "/tmp/blur,3"    a running session, at the
 *                                                   moment the artefact is on
 *                                                   screen -- which is the only
 *                                                   moment it can be captured
 *   AZ_BLUR_DUMP=/tmp/blur AZ_BLUR_DUMP_FRAMES=3    a headless harness, which
 *                                                   has to arm BEFORE the
 *                                                   compositor draws its first
 *                                                   frame and has no session to
 *                                                   dispatch into yet
 */

#include "avk_oracle.h"

#include <stdbool.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

/*
 * Arm for `frames` frames writing `prefix`-<n>-<tag>.pam, or disarm with a NULL
 * or empty prefix. `frames` below 1 is taken as 1.
 *
 * Numbering restarts at 0 on every arming, so a second look does not overwrite
 * the first one's files and leave two runs interleaved in one directory.
 */
void avk_blur_dump_arm(const char *prefix, int frames);

/* Whether a blur source should be tapped this frame. Reads AZ_BLUR_DUMP once,
 * so an unarmed run costs one comparison. */
bool avk_blur_dump_armed(void);

/*
 * Record where a tap's image sits on screen, keyed by the same (kind,
 * cmd_index) the tap was declared with.
 *
 * The tap knows the image and the crop; only the caller knows what the crop IS
 * -- which output pixels it covers and which box the blur will actually write.
 * Called immediately after a successful avk_oracle_tap().
 */
void avk_blur_dump_note(enum avk_oracle_tap_kind kind, size_t cmd_index,
	const char *tag, VkFormat format, struct avk_box capture,
	struct avk_box write);

/*
 * Write every noted tap of the frame that has just completed, and spend one
 * frame of the budget if anything was written.
 *
 * THE CALLER MUST HAVE WAITED for the submission that recorded the taps: this
 * reads host-visible memory a transfer wrote, and reading it early shows the
 * previous frame's contents rather than nothing, which is the failure that
 * looks like a rendering bug.
 */
void avk_blur_dump_write(struct avk_oracle *o);

#endif /* AVK_BLUR_DUMP_H */
