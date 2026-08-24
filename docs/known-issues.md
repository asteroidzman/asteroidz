# Known issues

Open defects with what has been established and, as importantly, what has been
*ruled out*. The point of this file is that the next attempt starts where the
last one stopped instead of re-deriving it.

## OPEN — inter prediction is correct but is not the default

Recordings are all-intra, where every picture is a sync sample and there is no
dependency chain at all. `AZ_ENCODE_INTER=1` selects inter, which produces
correct pictures and now starts a new sequence every 60 SUBMITTED pictures, so
no chain exceeds 59 and a recording has somewhere to seek to and somewhere to
resynchronise after a lost picture. The bound is in pictures, taken from the
encoder's own `frame_index`: `AZ_RECORD_FPS` is a cap on capture frequency and
the MP4 sample durations are measured, so neither bounds a chain. Measured at
4K, inter with that cadence is 7.0x smaller than all-intra on detailed changing
content and about 43x smaller on a static desktop.

Two things block promoting it to the default, both about the recorder rather
than the encoder:

  - **The real recording path has never run with inter.** `record_start`
    refuses a headless output (`hdr_capable` is false there), so every
    measurement so far drives `avk_encoder_*` directly and bypasses
    `az_avk_record_frame()` -- the rate cap, the presentation stamps, the
    measured sample durations, and the collect-a-frame-later deferral.
  - **The GPU encode duration of an inter picture at 4K is unmeasured.** The
    deferral is built around roughly 21 ms per picture. `last_encode_ns` does
    not answer this: it times command-buffer recording and submission, not the
    encode. Inter adds motion estimation, and a harness whose inter-picture gap
    is a 33 MB host upload says nothing about the compositor's.

Until both are answered on a real HDR output, all-intra stays the default and
`AZ_ENCODE_INTER` stays. Long runs are also untested: 150 pictures here against
18000 in a ten-minute recording, though `poc` restarts every 60 and the two DPB
slots simply alternate.

## OPEN — a tag switch arranges twice

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.
