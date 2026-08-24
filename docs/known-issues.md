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

90 coded pictures of 3840x2160 at QP 26, through the real encoder and the real
muxer; `force_key` driven from a /tmp harness, no product change. Everything is
indexed by PICTURES, because that is what the bound is on and the only quantity
the harness controls -- see the next section for why the obvious translation
into seconds is not one. "worst decode" is the pictures that must be decoded to
reach the least favourable seek target: one short of the next recovery point.

DYNAMIC desktop. The first measurement used content that changed every 30
pictures, which put a forced IDR exactly on every scene change at the 30-picture
bound, where a P picture would have been expensive anyway. That flattered the
small bounds badly. Re-measured with a change period of 23, which no tested
bound divides:

    bound          bytes/90 pics   vs unbounded   sync pts   worst decode
    unbounded         3793772          --            1       89 pictures
    60 pictures       4650232         +23%           2       59 pictures
    30 pictures       6250159         +65%           3       29 pictures
    15 pictures       9688475        +155%           6       14 pictures

For the record, the same bounds against the ALIGNED content read +4%, +8% and
+115%. Any future cadence measurement has to avoid letting the content's period
share factors with the bound.

STATIC desktop -- nothing changes, the worst case for IDR overhead, and no
alignment to confound:

    bound          bytes/90 pics   vs unbounded   sync pts   worst decode
    unbounded          472827          --            1       89 pictures
    60 pictures        916318         +85%           2       59 pictures
    30 pictures       1359813        +177%           3       29 pictures
    15 pictures       2690046        +454%           6       14 pictures
    1 (all-intra)    39371400       +8228%          90        0 pictures

The muxer marked every forced IDR as a sync sample in all of these: the `stss`
entry count matched the IRAP count in every row, and all-intra omits `stss`
entirely, which is what its absence means.

### What a cadence can and cannot bound

`AZ_RECORD_FPS` is a CAP on how often a frame is captured, not a rate: a
compositor renders when something changed, so an idle desktop produces no
captures at all. And the MP4 sample durations are MEASURED, with a 1 ms floor
and a **1 s ceiling** -- so a long idle is recorded as exactly one second and
the file's presentation timeline is not wall-clock either. "Every `2 * fps`
captured frames" therefore bounds neither elapsed wall-clock time nor elapsed
presentation time; it is only a picture count wearing a clock's clothes.

So the cadence is defined to bound ONE thing:

    the number of coded pictures that must be decoded to reach a recovery point

That is the quantity the defect is actually about. A lost or malformed picture
destroys the run of pictures that depends on it, and the cost of seeking is the
work of decoding them -- 18000 pictures, about two minutes, for the end of a
ten-minute recording today. It is also the quantity the table above is indexed
by. It needs no clock, no coupling to the configured rate, and no reasoning
about timestamp origins.

It deliberately does NOT bound elapsed presentation time. Under a bound of N
pictures a busy desktop gets a recovery point roughly every N/fps seconds and an
idle one much less often in wall-clock terms -- but an idle stretch is N
near-identical pictures, so the seek COST stays bounded even where the
granularity is coarse. Dense where the picture changes, sparse where it does
not, is the right behaviour for a screen recording.

If elapsed presentation time ever does need bounding, the authoritative source
is already computed: the exact `duration` handed to `avk_mp4_add_sample()`, in
AVK_MP4_TIMESCALE units, which is the timeline a seek operates on and which
already guards against mixing presentation stamps with readback stamps. Two
caveats if it is ever used: it is known only AFTER the picture it describes has
been collected, so an accumulator lags by one picture, and the 1 s ceiling means
it undercounts real idle time. Do not synchronise a frame counter with the
configured FPS instead.

### What the evidence decides, and what it does not

It decides two things.

**Unbounded is out.** One recovery point in the whole recording is the defect;
every other row buys real recovery for a cost that is a small fraction of the
all-intra default already shipping.

**15 and below are out.** Going 30 -> 15 costs +90 points of dynamic overhead
and +277 of static to remove fifteen pictures of decode. The cost per picture
saved rises sharply below 30 while the benefit per picture saved is constant, so
nothing under 30 is worth measuring further.

**It does not decide between 30 and 60.**

    bound          dynamic   static    worst decode
    60 pictures      +23%     +85%     59 pictures
    30 pictures      +65%     +177%    29 pictures

30 costs roughly 2.8x the dynamic overhead and 2.1x the static overhead of 60,
and buys exactly half the worst-case decode. There is no measurement here that
says whether halving decode work is worth two and a half times the bitrate; that
is a preference, and it has to be stated rather than derived. Two criteria that
WOULD each pick one, if someone adopts them:

  - "a static desktop must not cost more than 2x unbounded" selects 60 and only
    60 (+85% against +177%)
  - "worst-case decode must stay under 30 pictures" selects 30 and only 30

Both remain far cheaper than what ships today: against all-intra on static
content, 60 pictures is 44x cheaper and 30 pictures is 29x.

An earlier draft claimed the curve was flat above 60 on the strength of a
90-picture bound measuring identical to unbounded. That was not a measurement:
with 90 pictures encoded and a bound of 90, the forced key would fall at index
90, one past the end, so no IDR was inserted at all and the run WAS the
unbounded run. Nothing above 60 has been measured.

### Prerequisite: the reference pic_type is wrong after any later IDR

`submit_picture()` derives the reference's type as

    .pic_type = enc->frame_index == 1 ? IDR : P

which is true only for the FIRST P of the whole sequence. After a forced IDR at
frame N the next P has `frame_index == N+1`, so the DPB entry is described as a
P picture while the slot actually holds an IDR. That is exactly the defect the
comment above it says was already found and fixed -- fixed only for the single
IDR that was the only one that could exist. A cadence recreates it at every IDR,
so it has to be corrected before any cadence lands.

Latent so far, not observable: files forced to a 30- and 60-picture cadence
decoded with no errors and every frame pixel-correct. Same status the original
instance had.

**The smallest authoritative fix needs no new state: `enc->poc == 1`.**
`encode_frame()` sets `poc = 0` on any key picture and increments it after every
successful submit, so a P picture sees `poc == 1` exactly when the previous
submitted picture was a key frame, and `poc >= 2` otherwise. Verified by
instrumenting `submit_picture()` with an independent ground-truth flag and
comparing on every P picture: 20 P pictures, zero disagreements, across a forced
4-picture cadence, a mid-stream `avk_encoder_reset_sequence()`, and a fresh
encoder. Under all-intra no P picture is ever submitted, so the field is never
consulted (`referenceSlotCount` is 0 for a key); `avk_encoder_drain()` only
collects and does not touch the sequence state.

That equivalence holds on every path that advances the sequence. It does NOT
hold after a failed submit, where `poc` has been zeroed but nothing advanced --
but neither does the current condition, and the entry below says what has to
happen there regardless.

### Prerequisite: a failed submit leaves the sequence unusable

This was dismissed once as "already inconsistent, so not a reason to prefer a
new boolean". That was wrong: it is not merely an indeterminate `pic_type`, it
is a sequence that goes on to emit malformed pictures, and the recorder keeps
recording through it.

`submit_picture()` has six failure returns. In order, and what each has already
changed:

    enc == NULL                  nothing
    load_cmd_api() failed        nothing
    convert_to_p010() failed     the P010 image; logs
    vkBeginCommandBuffer failed  the command buffer was reset; SILENT
    vkEndCommandBuffer failed    all commands recorded, incl. the session
                                 RESET, and enc->session_reset already
                                 latched true; SILENT
    vkQueueSubmit failed         the same, plus enc->last_qp; logs

Two of the six are silent. And `enc->session_reset = true` is latched inside the
reset block, far ahead of the submit -- so if either of the last two fail, the
whole command buffer is discarded, the RESET never executes on the device, and
the flag says it did. The session is then never reset for the rest of its life.

Meanwhile `avk_encoder_encode_frame()` has already applied `enc->poc = 0` for a
key picture BEFORE calling submit, and advances `frame_index`, `poc` and
`dpb_slot` only on success. So after a failed forced IDR:

  - `poc == 0`, `frame_index` unchanged and non-zero, `dpb_slot` unchanged
  - the DPB still holds the last picture that succeeded
  - `enc->submitted` stays false, so nothing tries to collect a phantom packet

The next call is then `key = force_key || frame_index == 0 || all_intra`, which
is FALSE if the caller does not force again -- so a P picture is submitted with
`PicOrderCntVal = 0`, a short-term RPS whose one negative reference resolves to
POC -1, and `ref_std.PicOrderCntVal = enc->poc - 1`, which on the `int32_t` poc
is literally -1. That is a malformed sequence, not an ambiguous one. And
`az_avk_record_frame()` responds to the failure with `rec_dropped++` and a
`return`, then carries on recording into it.

**The smallest behaviour required before a cadence ships**, all of it reusing
state that already exists:

  1. Latch `enc->session_reset` only after `vkQueueSubmit` succeeds, so a
     discarded command buffer does not leave the session permanently
     un-reset.
  2. On any `submit_picture()` failure, call the existing
     `avk_encoder_reset_sequence()`. `frame_index` returns to 0, which makes
     the next picture an IDR through the condition already there, and `poc`
     and `dpb_slot` restart with it. That re-establishes a well-defined
     sequence start whatever was lost -- no new flag.
  3. Log the two silent returns. A drop that says nothing is
     indistinguishable from a frame that was never captured.

This matters more with a cadence than without one: today a recording contains
exactly one key picture and a failure is overwhelmingly likely to be in the P
chain, where the reset above is what recovers it. With forced IDRs, failures
land on key pictures too, which is precisely the case that zeroes `poc`.

### What is NOT being changed yet

No cadence is implemented, no bound is chosen, and inter is not promoted.
`AZ_ENCODE_INTER` stays and all-intra stays the default.

What has to happen, in order, before any of that:

  1. Correct the reference `pic_type` condition to `enc->poc == 1`.
  2. Make a failed submit safe: latch `session_reset` only on success, reset
     the sequence on failure, and log the two silent returns.
  3. Choose 30 or 60 pictures by stating a criterion the measurements above
     can be held against -- the evidence narrows it to those two and does not
     pick between them.
  4. Then force a key every N captured PICTURES, decided from the encoder's own
     sequence state rather than from a clock or the configured capture rate.
  5. Then promote inter and delete the switch.

## OPEN — a tag switch arranges twice

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.
