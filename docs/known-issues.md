# Known issues

Open defects with what has been established and, as importantly, what has been
*ruled out*. The point of this file is that the next attempt starts where the
last one stopped instead of re-deriving it.

## OPEN — a recording is one IDR and an unbounded chain of P pictures

`az_avk_record_frame()` passes `force_key = false` unconditionally and there is
no IDR period anywhere, so the only key picture a recording contains is frame 0.
The file cannot be seeked and has no recovery point: reaching the end of a
ten-minute 30 fps recording means decoding all 18000 pictures, about two minutes
at the ~143 fps this machine decodes 4K.

The container is not implicated. `avk_mp4_add_sample()` derives `sync` from the
IRAP NAL types and writes `stss` from it; measured, the entry count matches the
IRAP count in every configuration, and all-intra omits `stss` entirely. Only the
cadence is missing.

**A cadence bounds pictures, not time.** `AZ_RECORD_FPS` is a cap on capture
frequency, not a rate -- an idle desktop renders nothing -- and MP4 sample
durations are measured with a 1 s ceiling, so the file's timeline is not
wall-clock either. A count tied to the configured fps would bound neither. The
quantity to bound is the pictures that must be decoded to reach a recovery
point, which is what a lost picture destroys and what a seek costs.

### Measured cost

90 pictures of 3840x2160 at QP 26 through the real encoder and muxer, forced
from a /tmp harness. Overheads are byte-exact against the unbounded run. The
dynamic set changes content every 23 pictures, coprime with every bound tested --
a period sharing factors with the bound puts every IDR on a scene change and
flatters it.

    bound          dynamic   static    worst decode
    unbounded         --       --      89 pictures
    60 pictures      +23%     +94%     59 pictures
    30 pictures      +65%    +188%     29 pictures
    15 pictures     +155%    +469%     14 pictures

All-intra, the current default, costs +8227% on the static set, so every bound
above is a large net win against what ships today.

**The evidence narrows the choice; it does not make it.** Unbounded is out, and
so is anything at or below 15, where cost per picture saved climbs sharply while
the benefit stays constant. Between 30 and 60 it decides nothing: 30 costs 2.0x
(static) to 2.8x (dynamic) the overhead of 60 and buys exactly half the
worst-case decode. Picking one requires a stated criterion, not more measurement.

### Prerequisites before a cadence ships

**The reference `pic_type` is wrong after any later IDR.** `submit_picture()`
uses `enc->frame_index == 1`, true only for the first P of the sequence, so
after a forced IDR the DPB entry is described as a P picture while the slot
holds an IDR. The authoritative condition needs no new state: `enc->poc == 1`.
`poc` is zeroed on any key picture and incremented after each successful submit,
so on a P it means exactly "the previous submitted picture was a key". Verified
against an independent ground-truth flag: 20 P pictures, no disagreement, across
a forced cadence, a mid-stream `avk_encoder_reset_sequence()` and a fresh
encoder. Under all-intra no P is submitted and the field is never consulted.

**A failed submit leaves the sequence unusable.** `enc->session_reset` is
latched inside the reset block, ahead of the submit, so a discarded command
buffer leaves the session permanently un-reset while the flag says otherwise.
And `encode_frame()` zeroes `poc` for a key picture before submitting and
advances nothing on failure, so the next unforced picture is a P with
`PicOrderCntVal = 0` whose reference resolves to POC -1 -- malformed, not
ambiguous -- and the recorder counts a drop and records on into it. Three fixes,
all reusing existing state:

  - latch `session_reset` only after `vkQueueSubmit` succeeds
  - call `avk_encoder_reset_sequence()` on any `submit_picture()` failure, so
    the next picture is an IDR through the `frame_index == 0` condition already
    there
  - log the two silent failure returns (`vkBeginCommandBuffer`,
    `vkEndCommandBuffer`)

This matters more with a cadence than without: today a failure almost always
lands in the P chain, but forced IDRs put failures on key pictures, which is the
case that zeroes `poc`.

Nothing is implemented: no cadence, no bound chosen, no promotion.
`AZ_ENCODE_INTER` stays and all-intra stays the default.

## OPEN — a tag switch arranges twice

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.
