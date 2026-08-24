# Known issues

Open defects with what has been established and, as importantly, what has been
*ruled out*. The point of this file is that the next attempt starts where the
last one stopped instead of re-deriving it.

## OPEN — a scanned-out fullscreen client records nothing

Found while validating inter on the live output. With mpv fullscreen and direct
scan-out active (`surface-intent` showed `scanout_frames=19141` on DP-1), a
35-second recording produced *"record: DP-1 captured no frames; discarding"* --
nothing was composited, so there was no frame for `az_avk_record_frame()` to
encode. The same content windowed recorded 944 frames.

Pre-existing and unrelated to the encoder: the recorder can only encode what the
compositor composites. But the only symptom is that one line at stop, after the
recording has already been thrown away, and scan-out is exactly what a fullscreen
video player gets. Either the recording should suppress scan-out on the output it
is capturing, or refusing should be loud at `record_start` rather than silent
until `record_stop`.

## OPEN — a tag switch arranges twice

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.
