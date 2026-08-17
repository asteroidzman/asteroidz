# The five-feature program — dependency audit, order, and numbering

Written against HEAD `7899303d` ("the scene graph is ours, and scenefx is
gone"), from the code, not from the milestone summaries. Per the operator's
mid-audit correction, this document carries no falsifier apparatus and no
fixture design: each milestone ends in a short "demonstrably done" statement,
observable live or in one smoke test. The analysis — what exists, what depends
on what, what order the tree forces — is the deliverable.

Four facts found in the tree carry most of this document's weight:

1. **`~/.config/asteroidz/monitors.kdl` has its note honoured**: DP-1 runs
   `icc-profile "/home/ralf/FI32U.icm"; hdr 1; vrr 1; max-luminance 400` live,
   on AVK, simultaneously. M6B's definition of done held. The machine this
   program targets is a real HDR+ICC+VRR output next to a plain 8-bit SDR one —
   the exact two-monitor case Features 1 and 2 describe.
2. **Direct scanout is dead code at HEAD.** `scene_entry_try_direct_scanout()`
   (src/scene/wlr_scene.c:3233) has no caller; `wlr_scene_output_build_state()`
   is declared (src/scene/include/scene/wlr_scene.h:1248) and **defined
   nowhere**; `wlr_scene_output_commit()` (wlr_scene.c:3408), the only thing
   that would call either, is itself unreachable — every frame goes through
   `az_output_build_frame()` → `az_avk_build_frame()`, which never attempts
   scanout ("absent rather than half present", az_avk.h:4885). Feature 3's
   "direct-scanout audit" is therefore a **revival**, not an audit, and both
   `no-scanout` removal comments in config.kdl are currently vacuous: the races
   they warn about cannot occur because no scanout occurs at all.
3. **Feature 1's central type already exists.** `struct az_lum_domain`
   (src/render/az_lum.h) *is* a per-window luminance domain: per-source
   transfer function, primaries, a single resolved scale, a content peak from
   MaxCLL — resolved at commit, never per frame, consumed per draw by
   `az_avk_lum_of()` (az_avk.h:3039). The per-window rules (`sdr_white_scale`,
   `hdr_gain`, src/config/rule-schema.h:275-284) exist. What does not exist is
   the named *class* and its automatic derivation.
4. **Feature 2 is mostly already built, as architecture rather than feature.**
   az_output_color.h's opening comment is its specification: "a window
   straddling DP-1 (HDR) and HDMI-A-1 (SDR) is one scene intent rendered
   twice, with the same domains and two of these." `scene_ref_nits` is
   documented as a scene property precisely so two outputs cannot disagree
   about reference white (az_output_color.h:88-92); both outputs derive their
   mapping independently from the same scene-linear source by construction.
   Nothing is interpolated because nothing needs to be. What remains of
   Feature 2 is qualification and any discontinuity it *finds*, not a build.

---

## The questions

**Q1 — Which existing M5/M6 types does the program reuse?**

| need | existing symbol | file |
|---|---|---|
| per-surface colour/luminance intent | `az_lum_domain`, `az_lum_source_desc`, `az_lum_rules`, `az_lum_resolve()` | src/render/az_lum.h |
| per-output mapping | `az_output_desc`, `az_output_color_state`, `az_output_color_derive()`, `az_output_may_drive()` | src/render/az_output_color.h |
| effective output + preferred description | `az_surface_effective_output()`, `az_preferred_resolve()`, the identity-hash re-send pattern | src/render/az_preferred.h |
| presentation timing authority | `az_presenter` (embedded in Monitor), `az_present_regime` FIXED/VRR, the error series, miss verdicts | src/present/az_presenter.h + \_impl.h |
| source-description slot both protocols share | `az_cm_surface_description()` consumers, `az_cm_caps` single capability list | src/ext-protocol/az_cm_caps.h, wp-color-management.h (native), frog-color-management.h |
| content classification inputs | `wlr_content_type_v1` (wired), `client_content_type_is_game/is_video()` used by `check_tearing_frame_allow()` | src/ext-protocol/tearing.h |
| per-window policy knobs | `force_tearing`, `vrr_only_fullscreen`, `noscanout`, `force_hdr`, `sdr_white_scale`, `hdr_gain` | src/config/rule-schema.h |
| single frame seam | `az_output_build_frame()`, `az_output_color_transform()` | src/render/az_output.h |
| structured refusal precedent | `refused_why` in `az_avk_output_supported()` (az_avk.h:4893) | src/render/az_avk.h |
| diagnostics channel | `amsg get avk-stats` → `az_avk_stats_json()` (src/ipc/ipc.h:801) | src/ipc/ |
| capture protocol backbone | `wlr_ext_image_copy_capture_manager_v1` + session listener, `wlr_screencopy_manager_v1`, `active_capture_count`, `privacy_shield` | src/asteroidz.c:1799-1801, 11070-11071 |
| capture readback precedent | `az_avk_capture_frame()` (test capture), `screenshot_ui rawhdr` readback | az_avk.h:4602, src/dispatch/bind_define.h:3232 |
| scanout raw material (orphaned) | `scene_entry_try_direct_scanout()`, `color_management_is_scanout_allowed()`, `wlr_scene_buffer_set_prevent_scanout()` | src/scene/wlr_scene.c |
| HDR10 metadata forwarding | `mon_hdr_scanout_candidate()`, `mon_content_metadata()`, identity-gated modeset | src/asteroidz.c:6023-6152 |

The cross-cutting "per-surface presentation intent object" the brief asks for
is these things *referenced*, not a new stored type — see D2.

**Q2 — Which features depend on which, and in which direction?**

- A single **classifier** (from image description, content-type, fullscreen
  state, window rule) is shared input to Feature 1 (luminance class) and
  Feature 3 (presentation class). Neither owns it; the intent resolver does.
- Feature 2 depends on Feature 1 only in the sense that "continuity of what?"
  is answered by the domain; the two-mapping machinery it needs shipped in
  M5/M6B. It is downstream of 1 and of nothing else.
- Feature 4's SDR_APPEARANCE mode *is* the per-output tone map applied at a
  named stage, so it consumes Feature 1's stage naming; its timestamps come
  from the M6A presenter, which exists. It does not depend on Feature 3.
- Feature 5 observes everything but depends on nothing: every field it would
  print for the current compositor already exists in some struct today. It is
  upstream in *usefulness* — every later milestone's live demonstration reads
  its output — and downstream of none.
- Feature 3's scanout revival is the only KMS-facing work and is the only
  place the gamescope live-observation question becomes testable again (see
  Q8): at HEAD the removed `no-scanout` rules are untested by vacuity.

So the true graph is: intent resolver → {1, 3} → {2-as-qualification, 4},
with 5 attachable at any point and cheapest first.

**Q3 — Protocol / renderer / KMS work per feature.**

| feature | protocol | renderer | KMS/output | wlroots change |
|---|---|---|---|---|
| 1 domains | none new (wp-cm/frog/content-type wired) | small: per-domain tone-map params on existing per-draw path | none | none |
| 2 continuity | none | none (qualification; fixes only if found) | none | none |
| 3 classes | none new (tearing + content-type exist) | scanout revival inside the AVK frame seam | plane commit + the mpv wedge hazard | none — scanout code is already ours (scene absorbed) |
| 4 capture | ext-image-copy-capture session semantics; colour signalling audit (the protocol cannot express PQ metadata today — stated, not papered over) | stage taps + readback ring | none | none |
| 5 inspector | none (amsg/IPC) | none (reads existing state) | none | none |

Nothing requires patching wlroots. Feature 3 would have — scanout logic used
to be wlroots'/scenefx's — but the scene absorption at HEAD made it our code.

**Q4 — Independently implementable and testable?** 5 alone; 1 alone (the
resolver is pure, az_lum.h's whole design); 3's scanout is independent of 1;
4 needs 1's stage names to mean anything. 2 is not independently *buildable*
because there is nearly nothing to build.

**Q5 — Realistically useful on this hardware?** Demonstrable here: 1 (mpv
`force_hdr` beside SDR terminals on DP-1), 2 (drag across DP-1 HDR/ICC ↔
HDMI-A-1 SDR — the exact configured pair), 3 GAME/VIDEO (gamescope + steam
rules exist, `allow-tearing 2`, `force_tearing` on war.exe/aska.exe, VRR live
on DP-1, mpv at 23.976 on a 144 Hz VRR panel can be cadence-followed), 4
SDR_APPEARANCE and HDR10 (rawhdr precedent; DP-1 is a real PQ sink), 5
(trivially). Speculative here: SDR_EXTENDED (no wide-gamut-SDR source exists
on this machine — the class enum reserves the name, nothing more is built),
Vulkan Video encode (audit-then-maybe stands; RADV supports it, demand not yet
shown), SCENE_LINEAR/EXR (developer capture; cheap because the working image
already exists, but no named consumer yet — it rides along, it does not drive).

**Q6 — Order** — D1. **Q7 — Numbering** — D7. **Q8 — Priority check** — next.

---

## Q8 — Open correctness items, with verdicts

- **F10, wp-cm on XWayland: CLOSED 2026-08-17.** `commitx11()` now calls
  `mon_content_metadata_changed()` under the same `!c->iskilling` gate and in
  the same before-any-early-out position as `commitnotify()`. The gap was
  exactly one call: the preferred-description half is sent from `mapnotify()`
  and `setmon()`, both of which XWayland already shares. Not headlessly
  observable — the callee early-returns unless the output is HDR *and* the
  surface is its scanout candidate — so the headless evidence is that
  `commitx11` still runs (9 configures, XWayland on its own display, no
  fallback frame, no abort); the propagation itself is M11's live line.
  *Original finding, for the record:* `commitnotify()` (asteroidz.c:5278) calls
  `mon_content_metadata_changed()`; the XWayland path installs `commitx11()`
  (asteroidz.c:12824) which does geometry only. An XWayland client attaching a
  wp-cm description gets no metadata forwarding and no preferred re-send.
  Feature 1's classification rides the same commit path, so building on it
  while this hole exists hides a defect under a feature — the standing rule.
  It stops blocking when the X11 commit path reaches the same two calls the
  xdg path reaches. It is the first work item of the first milestone.
- **Gamescope / no-scanout removals (config.kdl, 2026-08-16): DOES-NOT-BLOCK
  now, BLOCKS M13's scanout closure.** Both hazards — gamescope's GPU-write
  race (believed fixed by the acquire-fence work, `5ae7ed09`) and mpv's
  KMS-wedge on scanout *transition* (explicitly NOT covered by that work, per
  the config comment) — are unobservable at HEAD because nothing scans out.
  The moment M13 revives scanout, both return as live questions and must be
  re-verified in M13's live session, mpv tag-away/tag-back included. A quiet
  live session between now and then proves nothing about either.
- **M3.5 P0/P1/P2: CLOSED.** Fixed at `d9cc7bc`, `2eaec70`, `5a1c711`
  (docs/vulkan-journey.md:657-674). Not blockers.
- **M4A large-radius flicker: CLOSED in M4B** (docs/avk-effects.md:100-157,
  border mask fix; the fixture that failed by design now passes). The still-open
  M4 items are M4F.2C's 180°/270° transform findings, recorded as not affecting
  `normal` — which is both live monitors. DOES-NOT-BLOCK.
- **Scene absorption qualified by smoke test only: recorded as known state.**
  Per the operator's correction, no suite run is scheduled here. Two concrete
  code facts found by this audit are recorded with it: the declared-but-
  undefined `wlr_scene_output_build_state` and the unreachable
  `wlr_scene_output_commit`/`scene_entry_try_direct_scanout` — link-alive only
  by luck of the linker. M13 either revives that code or deletes it; the
  no-back-compat rule forbids leaving it ambient either way.
- **HDR pending-commit fold-in always fails on DP-1** (every HDR transition
  falls back to retrain): DOES-NOT-BLOCK, but M13's GAME class touches HDR
  metadata correctness on the same path; it inherits the known state.
- **`VK_EXT_calibrated_timestamps` unwired**: GPU_LATE / PRESENTATION_SCHEDULING
  verdicts unreachable (az_presenter.h:122-127, by design, stated). Not a
  blocker; M11's inspector reports `gpu_ts_available` honestly rather than
  hiding the gap.

---

## Decisions

**D1 — The order is 5 → 1 (with 2 folded in) → 3 → 4, and the code forces
each move.** The operator's 1→2→3→4→5 is refused on three findings:

- *5 first*: the inspector needs no new policy — every field it prints exists
  (Q1) — and every later milestone's live demonstration is read through it.
  This machine's standing instruments are expensive (screenshot_ui freezes a
  frame; polling nearly froze the box once) while `amsg get avk-stats` is the
  proven cheap channel. Building the observer before the observed also gives
  the intent resolver its first consumer, which is how the resolver's shape
  gets validated without inventing scenes.
- *2 demoted from milestone to qualification*: fact 4 above. The invariants
  Feature 2 asks for are already load-bearing architecture; a milestone whose
  build content is near-zero is a name, not work. Its live drag test closes
  inside M12, where the domain policy it would qualify actually changes.
- *3 after 1, not after 2*: the classifier is shared (Q2), and 3's real risk —
  scanout revival against the recorded KMS-wedge hazard — deserves the
  inspector's rejection reporting live from day one, not bolted on after.
- *4 last*: SDR_APPEARANCE capture is meaningless until the per-domain mapping
  it captures is the shipped one, and its timestamps (M6A presenter) already
  exist, so nothing is lost by waiting.

**D2 — The per-surface intent object is a resolver and a snapshot, not a new
stored type.** The pattern is `az_preferred_resolve()`: a pure function that
aggregates, on demand and on generation change, references to what already
exists — `az_lum_domain` (source side), luminance class (new, M12),
presentation class (new, M13), `c->mon` → `az_output_color_state` (output
side), content type, presenter regime/prediction. Plus an identity hash so
frontends and the inspector re-send/re-print only on change — the exact
mechanism az_preferred.h already proves. Storing a parallel authoritative
object would be the duplicate policy engine the brief itself forbids; the
brief's "converging toward" instinct is right and its "object" wording is the
part to resist.

**D3 — M11: the intent resolver and the inspector's dump, plus the F10 fix.**
Scope: (a) close F10 (the X11 commit path reaches
`mon_content_metadata_changed()` and the preferred re-send, exactly as xdg
does); (b) the D2 resolver; (c) one authoritative structured dump — `amsg get
surface-intent` beside `avk-stats`, JSON, per-surface: identity, buffer
(dmabuf/shm, dims, format, modifier, bpc), colour (source tf/primaries/
description, domain, mastering max/MaxCLL/MaxFALL), presentation (regime,
predicted vs actual, error, cadence), render (composited-vs-scanout — today
honestly always "composited: scanout not implemented" — blur/opacity, tone-map
and encode parameters from `az_output_color_state`), output (path, encode_tf,
peak, ICC form, `validation_enabled`, `gpu_ts_available`); (d) an output-level
section reusing `az_output_path_name()`/`az_tf_name()`. No overlays yet:
overlays are M13+ options, the dump is the milestone. The inspector reads
production structs; it re-derives nothing.

> **M11 BUILT 2026-08-17.** (a) F10 closed. (b) `az_intent.h` —
> `az_surface_intent_resolve()`, a pure resolver in the `az_preferred_resolve()`
> shape, storing nothing. (c) `amsg get surface-intent`, covering toplevels,
> XWayland and layer-shell surfaces, plus the output section. One extraction
> came out of it that was not in the plan: the wlroots-colour → `az_lum_source_desc`
> switch lived inside the renderer's `az_avk_lum_of()`, keyed on a scene buffer
> the inspector cannot reach. It is now `az_source_desc.h`, read by both — the
> alternative was a second copy of the translation, which is how the protocol
> frontends drifted before `az_preferred.h` existed.
>
> Headless: 10/10, including that the dump's colour path agrees with the
> compositor's own log rather than being recomputed, and that a surface's
> identity is stable while it merely renders. **Gap stated rather than papered
> over: no headless client tags a surface**, so every surface in that run was
> untagged — the branch that reads a real PQ description is exercised only by
> the live line below, and "correct" and "always reports untagged" are
> indistinguishable in the headless run.

**D4 — M12: luminance domain classes, in scene-linear, with continuity
qualification (Feature 2's remains) closing it.** The class enum — SDR_UI,
SDR_NORMAL, SDR_EXTENDED (reserved, unpopulated on this machine), HDR_CONTENT
— derives event-driven at the places `az_lum_resolve()` already runs: from the
committed image description, content-type, and window rule override, in that
order, never required for correctness (untagged stays exactly ADR-004's
default). The class maps to policy through the existing knobs: a class is a
named default for `az_lum_rules` plus a tone-map intent consumed by the
existing per-output mapping — before encode, in scene-linear, because that is
the only place the pipeline applies anything. No 203 anywhere new:
`config.sdr_reference_luminance` and `AZ_SCENE_REF_DEFAULT` remain the only
anchors. Blur/translucency interaction costs nothing new — M5 composition is
already scene-linear FP16, so an HDR highlight behind a translucent SDR_UI
terminal already influences the blur in light units. Closure includes the
Feature-2 live drag: a window dragged DP-1↔HDMI-A-1, and a straddling window,
observed for reference-white/hue jumps; any discontinuity found is a finding
against the derive table, not a new subsystem.

> **M12 BUILD HALF VERIFIED LIVE 2026-08-17**, on DP-1 at reference 280.
>
> - mpv playing HDR: `hdr-content`, `class_from: derived`, scale 35.714 =
>   10000/280 — the automatic derivation, with no rule written. This is the
>   branch no headless run could reach: nothing in `contrib/` tags a surface.
> - kitty with `luminance-domain "sdr-ui"`: `class_from: window-rule`, scale
>   0.725 = 203/280 — BT.2408 diffuse white held against a brighter desktop
>   reference.
> - Fourteen other surfaces — terminals, both wallpapers, both bars and their
>   panels — all `sdr-normal` / `derived` / scale 1. The "untagged defaults to
>   SDR_NORMAL" decision, doing what it was chosen for: had the default been
>   SDR_UI, all fourteen would have dimmed on an upgrade nobody asked for.
>
> An earlier reading showed mpv as `sdr-normal`/1 and looked like a regression;
> it was mpv not playing. Worth recording because the inspector was correct and
> the instinct to distrust it first was wrong — the check that settled it was
> that the M12 commit's diff touches no line of the source-reading path.
>
> **THE STRADDLING CASE, WATCHED 2026-08-17.** One surface spanning both
> outputs: a single domain (scale 35.714, `hdr-content`) against two
> independently derived mappings — HDMI-A-1 `A-direct-srgb`/srgb/`peak_scene`
> 1.0, DP-1 `B-encode`/pq/`peak_scene` 1.4286. Nothing interpolated and nothing
> sampled from one output onto the other, which is Feature 2's founding
> invariant shown rather than argued. Operator: highlights roll off smoothly on
> both halves, no clipping.
>
> **WHAT THAT DOES AND DOES NOT PROVE.** mpv's `content_peak_scene` is 1.4286
> and DP-1's `peak_scene` is *also* 1.4286 — both are 400/280, the content's
> MaxCLL and the panel rule's max-luminance coinciding. So on DP-1 the rolloff
> is an IDENTITY, and a smooth DP-1 half is what would be seen whether the curve
> worked or was absent. Only the HDMI-A-1 half is evidence: there the same
> highlights compressed 1.4286 → 1.0, ~30% off peak, without flattening. Had
> the content's MaxCLL been below 400, both halves would have been no-ops and
> the whole observation would have proven nothing — the equal-by-coincidence
> trap again, and this time it was avoided by the panel's ceiling rather than by
> design.
>
> Both outputs run `ref_nits` 280, so an SDR source's scale — which carries no
> reference term (ADR-003) — is identical across the seam by construction.

**D5 — M13: presentation classes and the scanout revival.** DESKTOP_UI is
M6A's default, named. GAME and VIDEO become values in the intent snapshot,
classified from content-type/fullscreen/rule (never executable names), and
they *parameterize the existing presenter and commit path* — VRR
(`vrr_only_fullscreen` generalized), tearing (existing `check_tearing_frame_allow`
consuming the class), queue depth, and cadence-following presentation for
VIDEO (present at the timestamp the presenter predicts nearest the client's
target, which the error series can then judge). Colour management and sync are
never disabled by any class. The scanout revival: port the orphaned scene
decision logic into the AVK frame seam as an explicit pre-composition step in
`az_avk_build_frame()`, returning a **structured rejection reason** (extending
the `refused_why` precedent into an enum the M11 dump prints) — "HDR source
needs compositor tone mapping", "modifier not scanout-capable", "effects
active on this surface", "output ICC carried by encode pass". Delete
`wlr_scene_output_commit` and the undefined `build_state` declaration in the
same commit. The M13 live session re-tests both no-scanout hazards (Q8);
gamescope (asteroidz → gamescope → Steam → Proton) is the qualification
target and gets no compositor-specific hack. VRR is not built, it is audited:
DP-1 already runs `vrr 1` with the presenter's VRR regime measured live —
what M13 adds is per-class policy over a capability that demonstrably works.

> **M13A LANDED 2026-08-17 — classification, tearing, VRR. NOT cadence.**
>
> Built: the three classes, derived from wp-content-type with a
> `presentation-class` window rule overriding; tearing routed through the class
> instead of reading wp-content-type in a second place; VRR generalised so a
> GAME window gets it fullscreen without `vrr-only-fullscreen` named per app.
>
> **A crash was found and fixed on the way, not by this milestone's design but
> by its first fixture.** `allow_tearing` plus any window asking to tear called
> `apply_tear_state()`, which has aborted since 13254aad removed the SceneFX
> composition it routed to — a crash reachable from configuration alone, which
> M13 would have widened since a game no longer needs a rule to be classed as
> one. Tearing now goes through `az_output_build_frame()` with
> `tearing_page_flip`, tested and downgraded to a synced present if refused.
>
> **VIDEO's cadence-following is NOT implemented**, and the first version of
> this text, the rule help and the enum member all said it was. Today `video`
> means exactly one thing: never tear. Presenting 23.976fps content at the
> opportunity nearest the client's target is the reason the class exists and is
> M13's remaining presenter work. Corrected in every place that claimed it.
>
> Live: mpv reports `video (derived)` — so the automatic path does reach real
> clients, and the video-never-tears fix was live rather than theoretical, since
> mpv was in the class that could tear under `allow_tearing 1`. VRR remains
> unverified; a headless output has no adaptive sync and no fullscreen game has
> been run since.
>
> **VERIFIED LIVE 2026-08-17.** mpv `video (derived)` — the automatic path
> reaches real clients. gamescope does NOT declare wp-content-type, so games
> need a rule in practice; with `presentation-class "game"` on the game, the
> live dump reads `game fs=true vrr=true`, which closes the VRR generalisation:
> a GAME window fullscreen gets adaptive sync without `vrr-only-fullscreen`
> named per application.
>
> **A SEGFAULT REACHED THE LIVE SESSION, AND A CHECK THAT WOULD HAVE CAUGHT IT
> WAS ALREADY IN THE TREE.** `rule_format()` dereferences every RULE_ENUM field
> as a `const char *`; M12's `luminance_domain` and M13A's `presentation_class`
> were declared `char[16]`, so it read the value's own first eight characters as
> a pointer. Latent until a rule set one — an all-zero array reads as NULL —
> then fatal on any IPC read of the window rules, which something on the desktop
> does periodically.
>
> `asteroidz -S` writes every field through the real parser and reads it back
> through `rule_format`. Run against the pre-fix build it dumps core; it was
> never run. Every smoke passed because all of them read these fields through
> the RENDERER and none through the IPC surface that formats them. The lesson is
> not "write more fixtures" — it is that adding a row to `rule_schema[]` has an
> existing check attached to it, and schema changes must run `-S` and
> `check-rule-schema.py` before install.
>
> **VIDEO CADENCE: MEASURED, AND NOT NEEDED ON THIS HARDWARE.** Live on DP-1
> with a 23.976fps film: mpv commits at **23.9377 Hz** over 622 samples (0.16%
> off nominal, dropping nothing), and `get presentation` counts **240 presented
> frames in 10 seconds — exactly 24.0/s**. The compositor already paces to the
> content; DP-1's global VRR is doing precisely what the VIDEO class would have
> been built to do. No mechanism is warranted here, and the class keeps its one
> real effect: never tear.
>
> Revival condition, so this does not run forever: a fixed-refresh output
> carrying video where `presents_per_frame` is a non-integer, or an operator
> complaint about judder with a capture. HDMI-A-1 at 60Hz is the candidate —
> 23.976 into 60 is 2.5 presentations per frame and no compositor-side
> scheduling fixes that; only a mode change does.
>
> **THE INSTRUMENT PRODUCED A CONFIDENT WRONG ANSWER FIRST, AND ALMOST WON.**
> Its `vblanks_per_frame` divided the client's commit interval by the panel's
> observed *vblank* period. Under VRR those are unrelated: the panel free-runs
> while the compositor commits sparsely. It read 4.517 on a perfectly-paced
> film, which was written up as "refutes the hypothesis with real data" and as
> justification for building cadence scheduling. `get presentation` — counting
> frames that actually reached the screen — said 24.0/s and settled it the other
> way. Now `presents_per_frame` divides by `presented_hz`, where 1.0 means the
> compositor is pacing to the client, and `presented_hz` is ABSENT rather than
> zero where the backend gives no timing, which is every headless output.
>
> The lesson is the M12 one again in a new place: a metric that cannot be wrong
> in an obvious way is more dangerous than a missing one. This one survived
> because a second, independent counter disagreed with it.
>
> **M13B LANDED AND VERIFIED LIVE 2026-08-17.** DP-1 presenting a fullscreen mpv:
> `accepted`, and `scanout_frames` climbing 774 → 902 in five seconds (~25.6/s,
> the film's own cadence). Every frame going straight to the display with no
> composition pass. The first time this compositor has bypassed composition at
> all; `fallback_frames` 0 and no crash.
>
> Every refusal carries a verdict and a sentence, which was the point: the scene
> version returned `SCANOUT_INELIGIBLE` from eleven places. The acquire fence is
> attached, which is what the gamescope no-scanout rule was working around.
>
> **Three bugs, and two of them ignored an answer that already existed.**
> The effects check tested the per-window OVERRIDE flags (`isnoradius`,
> `isnoshadow`) rather than whether an effect is drawn — with no rule set they
> read as "enabled" and refused every candidate forever. The scanout path did not
> clear `pending_commit_damage`, so the output would have re-scanned-out at
> maximum rate with the content paused. And the ICC check tested whether a
> profile was LOADED rather than whether the encode pass CARRIES one —
> refusing DP-1, whose profile is inert under HDR, using a distinction M11 had
> already added to the inspector as `icc_applied` and documented. That is twice
> in one day of reimplementing past an existing answer; the other was
> `asteroidz -S`.
>
> **THE TRANSITION HAZARD DID NOT REPRODUCE.** mpv — the application with the
> recorded KMS wedge on scanout *transition* — was tagged away from and back
> while scanning out. No wedge, and scanout re-engaged on its own: 1483 → 1610
> frames in five seconds after the round-trip. The dangerous direction, scanout →
> composition → scanout, works.
>
> That is ONE round-trip, not a soak, and the honest scope is that: the recorded
> wedge may have been intermittent or conditional on an HDR transition happening
> at the same moment, and a single clean pass cannot distinguish "fixed" from
> "not triggered". What it does establish is that the path is not wedged by
> construction. The gamescope arm — a fullscreen game scanning out, with the
> explicit-sync race the acquire fence now addresses — remains untested.

**M13 IS CLOSED.** A, B, and the cadence question answered by measurement and
declined.


> **M14 IS FUTURE WORK, NOT CANCELLED — deferred 2026-08-17 with its ground
> audited.** What the audit established, so the next attempt does not re-derive
> it:
>
> - **The demand is real and the current path is genuinely bad.**
>   `contrib/hdr-record.sh` records HDR at **1 frame per second**, and says why in
>   its own header: `screenshot_ui,rawhdr` freezes the output and does a full
>   readback per call, and a faster poll "already made a real system nearly
>   unresponsive once". HDR recording today is a slideshow by necessity.
> - **Most of the plumbing exists.** ext-image-copy-capture sessions are wired
>   (asteroidz.c:1833), `active_capture_count` and the privacy shield work, and
>   `az_avk_capture_frame()` does readback. What is missing is a STREAM with
>   named stages.
> - **M13B changed the problem.** A scanned-out frame has no composited image to
>   tap, so the stream must either force composition for its duration or read the
>   client buffer and SAY which it did. That is a new contract, not an
>   implementation detail.
> - **It is the largest remaining build and the only one with no cheap live
>   check.** Verifying it needs a capture client consuming frames, and none
>   exists in `contrib/`. Every other milestone this program shipped was checked
>   with one `amsg` query.
>
> The audit already paid for itself: it found that direct scanout defeated
> `privacy_shield`, a hole M13B had opened hours earlier (177b158e).

**D6 — M14: the capture stream.** A stream API first, not an encoder:
ext-image-copy-capture sessions backed by AVK stage taps, with the stage
**named in the API** — SCENE_LINEAR (the Path-B working image), SDR_APPEARANCE
(post-tone-map, pre-encode, the intentional SDR rendition), HDR10 (final
encoded PQ + the metadata `mon_content_metadata()` already computes). No
unnamed intermediates. Timestamps are the presenter's presentation timeline,
not recorder wake time. Readback follows the architecture's own rule
(§5.7: "capture may block, the frame path may not") via the existing retire/
staging machinery; the `rawhdr` path retires into this rather than surviving
beside it. Vulkan Video encode: one audit work item measuring zero-copy
complexity on RADV, build only if the audit says cheap — demand on this
machine is real (hdr-record.sh exists because the operator records HDR) but
the encoder half is not yet shown to need to live in the compositor.
Permissions ride the existing screencopy/portal path and `privacy_shield`;
AVK owning the image grants nothing.

**D7 — Numbering: the five-feature program is M11–M14; M7 and M8 stand; M9
and M10 are closed out of order.** The architecture table's M7 (client-compat
matrix) and M8 (device-loss recovery + performance) remain real, unstarted
work — M13's gamescope qualification and M6A's instruments subsume *parts* of
each, and the table rows gain a note to that effect, but neither row is
consumed by this program and neither is renumbered. M9 is complete at
`7899303d`; M10 is complete in a stronger form than written — GLES was not
"demoted to recovery renderer", it was deleted, because the no-fallback rule
made a recovery renderer a contradiction. The five features therefore take
fresh numbers with no collision: **M11 intent+inspector, M12 luminance
domains, M13 presentation classes+scanout, M14 capture**. Feature 2 has no
number because it is not a milestone (D1). On docs placement: `docs/m7-next/`
is a misnomer twice over (it holds M6B/M6C, and M7 means client-compat);
program docs belong in per-milestone directories following the house pattern —
`docs/m11-intent/` onward — with `docs/m7-next/` renamed or its contents
re-homed under `docs/m6-color/` in a future commit. Nothing is moved by this
audit; this file lives where it was ordered to.

**D8 — Explicitly out, with revival conditions:** adaptive/content-histogram
tone mapping (out by the brief and by §5.22's 2-code measurement; revived by a
named live complaint with a capture); SDR_EXTENDED population (revived by a
wide-gamut-SDR source actually appearing on this machine); Vulkan Video encode
beyond the M14 audit (revived by the audit's own numbers); developer overlays
beyond the dump (revived after M13, when scanout eligibility gives an overlay
something to show that the dump cannot); a VRR subsystem beyond per-class
policy (the backend's VRR already works; nothing more is built on assumption).

**D9 — Demonstrably done, per milestone.**

- **M11**: `amsg get surface-intent` on the live session shows mpv's real PQ
  description, domain and mastering values, the terminal's untagged default,
  DP-1's LUT-carrying encode state and HDMI-A-1's Path A — and an XWayland
  wp-cm test client's metadata change visibly propagates (F10 closed).

> **CLOSED 2026-08-17**, with one correction to this line and one gap left open.
>
> **"DP-1's LUT-carrying encode state" was wrong, and this document contradicted
> itself.** D3 makes an ICC profile INERT on an HDR output — the connector
> presents its own image description, and stacking an SDR characterisation on
> top is two transforms on one pixel. So DP-1 with `hdr 1` carries no LUT and
> never will; it reads `path: B-encode`, `encode_transfer: pq`, `icc: true`,
> `icc_applied: false`. The acceptance line was written against an expectation
> the decision section had already refused three sections earlier. Live output
> is correct; the gate was not.
>
> Live, on DP-1 and HDMI-A-1: mpv tagged `pq`/`bt2020`, `max_cll` 400, on a
> 10-bit `AB30` dmabuf; `domain.scale` 35.714 = 10000/280 against the operator's
> `sdr_reference_luminance` of 280; `peak_scene` 1.4286 = 400/280; preferred
> carrying the rule's real 400/0.4/250 rather than a default; HDMI-A-1 on
> `A-direct-srgb`; DP-1 in VRR with **0 misses** and 66 `prediction_exceeded`,
> which is ADR-605's distinction holding in the field.
>
> **Still unverified: the XWayland half.** F10's code path is closed and the
> commit path runs, but no XWayland client carrying a wp-cm description has been
> observed propagating a metadata change live — headless cannot (the callee
> needs an HDR output and scanout candidacy), and no such client was to hand.
> It is a narrower claim than "F10 closed" and is recorded as the narrower one.
>
> Two defects the dump found in itself on first live use are at `a38f3cd5`: a
> 64-bit identity emitted as a JSON double (lossy above 2^53), and a loaded
> profile reported without saying whether it was applied.
- **M12**: on DP-1, HDR mpv beside an SDR_UI terminal with the terminal's
  white at the restrained anchor and highlights untouched; the same window
  dragged to HDMI-A-1 and back, and straddling, with no visible reference-white
  or hue jump beyond the panels' physical difference; the dump names each
  surface's class and why.

> **CLOSED 2026-08-17.** Every clause met, live. The dead rules are live, the
> class derives and overrides, the straddling case holds, and the drag shows no
> jump on SDR windows with mpv's diffuse content holding across the boundary.
>
> **The evidence is not of equal weight, and the record should not pretend it
> is.**
>
> *Strong.* mpv's diffuse content holding across the drag crosses two genuinely
> different output paths — `B-encode`/PQ on DP-1, `A-direct-srgb`/sRGB on
> HDMI-A-1 — and lands at the same apparent brightness. That is decode →
> scene-linear → per-output encode agreeing with itself through two different
> encodes, which is the whole architecture in one observation. Also strong:
> HDMI-A-1 compressing 1.4286 → 1.0 without flattening.
>
> *Weak, by construction.* "No jump on SDR windows" could hardly have failed.
> `sdr_reference_luminance` is a single global, so both outputs necessarily
> report `ref_nits` 280, and an SDR source's scale carries no reference term
> (ADR-003). The observation confirms nothing was violated; it could not have
> discovered a violation. A real test would need per-output references, which do
> not exist and are not proposed — recording the limit is cheaper than building
> the capability to test it.
>
> *Untravelled.* `AZ_LUM_CLASS_SDR_EXTENDED` derives from BT.2020 primaries with
> an SDR transfer, a combination nothing on this machine has produced. Live code
> on a path never walked; its rules are the default, so the exposure is a name
> in a dump and no luminance change.

- **M13**: a fullscreen game classified GAME with VRR active and the dump
  showing scanout ACCEPTED (or the named reason it is not); gamescope runs a
  Proton title without artifacts through the tag-away/tag-back mpv sequence
  that used to wedge DP-1; 23.976 mpv playback shows cadence-following
  presentation in the presenter's own error series.
- **M14**: a capture client pulls an HDR10 stream from DP-1 that an HDR-aware
  player renders correctly, an SDR_APPEARANCE stream that matches what an SDR
  screenshot should look like (not truncated PQ), each frame stamped with its
  presentation time; the stage is named in the session, and the frame path's
  timing (presenter error series) does not degrade while capturing.

---

## Where the operator's framing was wrong, said plainly

1. **Feature 2 is not a feature at HEAD.** The independence of the two
   per-output mappings from one shared scene-linear intent is the *founding
   invariant* of az_lum.h and az_output_color.h, written down and shipped in
   M5/M6B. Asking for it as milestone #2 re-orders the program around work
   that is already done; what remains is one live drag test and honesty about
   physically unavoidable differences. The interpolation question the brief
   carefully hedges ("never interpolate raw ICC LUT outputs") dissolves:
   nothing is interpolated anywhere in the design.
2. **"Direct scanout audit" presumes scanout exists to audit.** At HEAD it is
   an orphaned function and an undefined symbol. The honest name for Feature
   3's scanout half is a revival inside AVK's own frame seam — which the scene
   absorption made possible without touching wlroots, and which reopens both
   no-scanout hazards the config comments think are "under test" but are in
   fact untestable by vacuity today.
3. **The order buried the cheapest, most enabling feature last.** The
   inspector needs nothing, validates the shared resolver, and is the
   instrument every other milestone's "demonstrably done" line is read
   through. Building it fifth means qualifying M12/M13 with screenshots and
   log-grepping on a machine where both are known to be expensive.
4. **"A per-surface presentation intent object" is right as a concept and
   wrong as a type.** The state already has owners — az_lum, az_preferred,
   the presenter, the output colour state. The convergence point is a pure
   resolver plus an identity hash, the pattern az_preferred.h already proves;
   a stored object would be the parallel policy engine the same paragraph
   forbids.
5. **Five features are four milestones.** One dissolves into qualification
   (2), and none of the remaining four is small enough to pair — the numbering
   in D7 reflects the work, not the brief's enumeration.
