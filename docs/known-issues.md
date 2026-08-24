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
is lost or malformed has no later IDR to resynchronise on.

This is why `AZ_ENCODE_INTER` still exists and all-intra is still the default
even though M14B's corruption is fixed: inter prediction now produces correct
pictures (11.7x smaller than all-intra on correlated content), but a recording
made of one IDR and a thousand P pictures is not a file anybody should be given
until it has recovery points. Settle this and inter can be promoted and the
switch deleted; the rate control contract it was also waiting on is fixed.

NOTE: it was tempting to say this is what turned the rare M14B corruption into
a wholly green file, via error propagation down the P chain. **That was tested
and not confirmed**: with unrelated photos only the corrupt frame is wrong and
the P pictures after it recover, because the encoder codes them as effectively
intra. Propagation on correlated desktop content is plausible and unproven.

## OPEN — a tag switch arranges twice

A tag switch emits two `anim start`s per client one frame apart (83 of 250 in a
20-round-trip run were superseded before their first tick). Harmless now that a
replaced-but-unsampled segment restarts cleanly, but the second arrange is
redundant work.
