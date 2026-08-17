#ifndef AZ_PRESENT_INTENT_H
#define AZ_PRESENT_INTENT_H

/*
 * ── WHAT IS THIS WINDOW'S FRAME FOR? ──────────────────────────────────────
 *
 * M13. Not a performance mode. A performance mode asks "how much may I switch
 * off", and the answer here is always "nothing": colour management, HDR and
 * explicit synchronisation are never traded for latency, because a fast wrong
 * pixel is still wrong. What a class selects is WHEN a frame should appear,
 * which is a different axis entirely.
 *
 *   DESKTOP_UI  smoothness and a predictable cadence. Presentation-time
 *               animation, no tearing. This is M6A's existing default, and it
 *               is named here rather than changed.
 *   GAME        lowest practical latency: tearing where the client asks for it,
 *               VRR where the display supports it, minimal queue depth.
 *   VIDEO       never tears, because a torn frame in a film is a worse
 *               artifact than a late one.
 *
 *               CADENCE-FOLLOWING IS NOT IMPLEMENTED. The intent -- present
 *               23.976fps content at the presenter opportunity nearest the
 *               client's target rather than pacing it like desktop animation
 *               -- is what this class is FOR, and it is not what this class
 *               currently DOES. Classification and the no-tear rule landed
 *               first because they are what the tearing path needed; the
 *               cadence half is M13's remaining presenter work. Said here
 *               rather than implied, because a class that names an intent it
 *               does not yet act on is exactly the kind of thing that gets
 *               believed.
 *
 * ── CLASSIFICATION USES INTENT, NEVER EXECUTABLE NAMES ────────────────────
 *
 * wp-content-type is a client declaring what it is, which is the honest signal.
 * A window rule overrides it, because the operator knows things the client does
 * not. An app-id list would be neither: it guesses at intent from identity and
 * is wrong for every application not on it.
 *
 * FULLSCREEN IS NOT EVIDENCE OF CLASS, and that is a judgement worth stating.
 * A fullscreen browser is not a game. Fullscreen gates whether some policies
 * APPLY -- VRR is pointless for a windowed game sharing an output with a
 * blinking cursor -- but it never decides what the content IS.
 *
 * Placed between modern.h (which reads wp-content-type) and tearing.h (which
 * consumed that protocol directly and now consumes this), so both halves of the
 * tearing decision come from one predicate.
 */

enum az_present_class {
	AZ_PRESENT_CLASS_DESKTOP_UI = 0,
	AZ_PRESENT_CLASS_GAME,
	AZ_PRESENT_CLASS_VIDEO,
	AZ_PRESENT_CLASS_COUNT,
};

static inline const char *az_present_class_name(enum az_present_class c) {
	switch (c) {
	case AZ_PRESENT_CLASS_DESKTOP_UI: return "desktop-ui";
	case AZ_PRESENT_CLASS_GAME:       return "game";
	case AZ_PRESENT_CLASS_VIDEO:      return "video";
	case AZ_PRESENT_CLASS_COUNT:      break;
	}
	return "?";
}

/* A parser, like az_lum_class_from_name() and for the same reason: a window
 * rule spells the class as a string. An unrecognised spelling returns false and
 * the caller keeps what it derived -- a typo must not become a policy. */
static inline bool az_present_class_from_name(const char *s,
		enum az_present_class *out) {
	if (s == NULL || *s == '\0' || out == NULL) {
		return false;
	}
	for (int i = 0; i < (int)AZ_PRESENT_CLASS_COUNT; i++) {
		if (strcmp(s, az_present_class_name((enum az_present_class)i)) == 0) {
			*out = (enum az_present_class)i;
			return true;
		}
	}
	return false;
}

/*
 * THE CLASS FOR A CLIENT. Rule first, then what the client declared, then the
 * default that changes nothing.
 *
 * `from_rule` tells an observer which happened, because "game because the
 * client said so" and "game because someone wrote a rule" behave differently
 * when the client changes its mind.
 */
static inline enum az_present_class az_present_class_of(Client *c,
		bool *from_rule) {
	if (from_rule != NULL) {
		*from_rule = false;
	}
	if (c == NULL) {
		return AZ_PRESENT_CLASS_DESKTOP_UI;
	}
	enum az_present_class ruled;
	if (az_present_class_from_name(c->presentation_class, &ruled)) {
		if (from_rule != NULL) {
			*from_rule = true;
		}
		return ruled;
	}
	if (client_content_type_is_game(c)) {
		return AZ_PRESENT_CLASS_GAME;
	}
	if (client_content_type_is_video(c)) {
		return AZ_PRESENT_CLASS_VIDEO;
	}
	return AZ_PRESENT_CLASS_DESKTOP_UI;
}

#endif /* AZ_PRESENT_INTENT_H */
