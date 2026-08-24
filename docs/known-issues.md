# Known issues

Open defects with what has been established and, as importantly, what has been
*ruled out*. The point of this file is that the next attempt starts where the
last one stopped instead of re-deriving it.

## OPEN — a recording is one IDR and an unbounded chain of P pictures

`az_avk_record_frame()` is the only caller of `avk_encoder_encode_frame()` and
passes `force_key = false` unconditionally; the only key frame a recording ever
contains is frame 0, because `key` is `force_key || frame_index == 0 ||
all_intra`. There is no IDR period anywhere in the encoder or the recorder.

A file like that cannot be seeked and has no recovery point: any picture that
is lost or malformed has no later IDR to resynchronise on. 4K decode runs about
143 fps here, so seeking to the end of a ten-minute 30 fps recording means
decoding all 18000 pictures -- roughly two minutes.

**The container is NOT the problem.** `avk_mp4_add_sample()` derives `sync` from
the NAL type (16..21 are the IRAP types), writes `stss` listing exactly those,
and omits `stss` entirely when every sample is one -- which is what its absence
means. Measured: all-intra has no `stss`, unbounded inter has an `stss` with
exactly ONE entry, and with forced IDRs the entry count matches the IRAP count
every time. The muxer already tells the truth; the cadence is what is missing.

### Measured cost of a cadence

90 frames of 3840x2160, QP 26, the real encoder and the real muxer; `force_key`
driven from a /tmp harness, no product change. The recorder captures at 30 fps
by default (`AZ_RECORD_FPS`, 1..240, `rec_interval_ns = 1e9 / fps`), so 90
frames is 3.00 s and 60 frames is 2.0 s.

DYNAMIC desktop -- content changes every 30 frames:

    cadence                  bytes/90f    Mbit/s   sync pts  worst seek
    unbounded inter (1 IDR)  2694480      7.2      1         whole file
    IDR every 60f (2.0s)     2800871      7.5      2         1.97 s
    IDR every 30f (1.0s)     2927451      7.8      3         0.97 s
    IDR every 15f (0.5s)     5826742      15.5     6         0.47 s
    all-intra (shipping)     86601180     230.9    90        0.00 s

STATIC desktop -- nothing changes, the worst case for IDR overhead:

    cadence                  bytes/90f    Mbit/s   sync pts  worst seek
    unbounded inter (1 IDR)  472827       1.3      1         whole file
    IDR every 60f (2.0s)     916318       2.4      2         1.97 s
    IDR every 30f (1.0s)     1359813      3.6      3         0.97 s
    IDR every 15f (0.5s)     2690046      7.2      6         0.47 s
    all-intra (shipping)     39371400     105.0    90        0.00 s

### The cadence the evidence supports: 2 s

Against unbounded inter a 2 s cadence costs +4% on changing content and +85%
on a static desktop -- the static figure being the honest worst case. Against
**what ships today** it is 31x (dynamic) to 44x (static) cheaper: 7.5 against
230.9 Mbit/s, 2.4 against 105.0. So adopting it is a large net win over the
current default rather than a regression.

2 s is the knee. Halving to 1 s buys one second of seek for roughly double the
static overhead (+177% against +85%); halving again to 0.5 s costs +454%. It is
also the conventional keyframe interval for screen capture.

It must be expressed as `2 * fps` FRAMES, not a literal 60: `AZ_RECORD_FPS` is
settable from 1 to 240, so a fixed constant would mean 60 s at 1 fps and 0.25 s
at 240. Note also that the MP4 sample durations are MEASURED, not nominal --
the configured rate is a capture cap, not a promise about spacing.

### Prerequisite: the reference pic_type is wrong after any later IDR

`submit_picture()` derives the reference's type as

    .pic_type = enc->frame_index == 1 ? IDR : P

which is true only for the FIRST P of the whole sequence. After a forced IDR at
frame N the next P has `frame_index == N+1`, so the DPB entry is described as a
P picture while the slot actually holds an IDR. That is exactly the defect the
comment above it says was already found and fixed -- fixed only for the single
IDR that was the only one that could exist. A cadence recreates it at every
IDR, so the condition has to become "the previous picture was an IDR" before
any cadence lands.

Latent, not yet observable: files forced to a 30- and 60-frame cadence decoded
with no errors and every frame pixel-correct. Same status the original instance
had.

### What is NOT being changed yet

No cadence is implemented and inter is not promoted. `AZ_ENCODE_INTER` stays
and all-intra stays the default. The intended change, when it is made, is:
fix the `pic_type` condition, force a key frame every `2 * fps` captured
frames, then promote inter and delete the switch.

## OPEN — a tag switch arranges twice

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.
