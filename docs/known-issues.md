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

The encoder side is ready: the reference `pic_type` now uses the authoritative
`enc->poc == 1`, and a failed submit resets the sequence so the next picture is
an IDR rather than a P claiming POC 0. What remains is the policy -- no cadence,
no bound chosen, no promotion. `AZ_ENCODE_INTER` stays and all-intra stays the
default.

## OPEN — a tag switch arranges twice

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.
