#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "az_identity.h"
#include "ipc-config.h"
#include "ipc-rules.h"
#include "ipc-out.h"

struct ipc_watch_client {
	struct wl_list link;
	int fd;
	struct wl_event_source *source;
	enum ipc_watch_type type;
	struct ipc_out out;
	union {
		struct {
			char name[64];
		} monitor;
		struct {
			uint32_t id;
		} client;
		struct {
			char mon_name[64];
		} tags;
	} target;
};

static struct wl_list watch_clients;

struct ipc_client_state {
	int fd;
	struct wl_event_source *source;
	struct wl_event_loop *loop;
	char *buf;
	size_t buf_len;
	size_t buf_cap;
	struct ipc_out out;
	/* The reply has been queued and the connection closes once it is out. */
	bool closing;
	/* The handler answered nothing and the connection must STAY OPEN.
	 *
	 * For `capture-chord`, whose reply arrives when a key is pressed rather than
	 * when the handler returns. Without it the normal path sees an empty output
	 * queue, concludes the reply is out, and closes -- so the client would get
	 * EOF instead of a chord. */
	bool deferred;
};

static void ipc_remove_watch_client(struct ipc_watch_client *wc);
static void ipc_notify_json_to_fd(int fd, cJSON *json);

/* Answer a request whose handler returned without answering.
 *
 * The other half of `deferred`: queue the reply now, arm the writable handler,
 * and let the ordinary drain-then-close path finish the connection. Nothing here
 * closes the fd itself -- doing so would race whatever is still in the queue,
 * which is the bug the output queue exists to have fixed. */
static void ipc_capture_reply(struct ipc_client_state *c, const char *json) {
	if (!c)
		return;
	if (!ipc_out_append(&c->out, json, strlen(json)) ||
		!ipc_out_append(&c->out, "\n", 1)) {
		return;
	}
	c->deferred = false;
	c->closing = true;
	if (!ipc_out_flush(c->fd, &c->out))
		return;
	wl_event_source_fd_update(c->source, WL_EVENT_WRITABLE | WL_EVENT_HANGUP |
											 WL_EVENT_ERROR);
}

/* Included HERE rather than beside the other ipc-* headers: it calls
 * ipc_capture_reply above and takes an ipc_client_state, so both have to exist
 * first. */
#include "ipc-capture.h"

/* ---------- utility functions ---------- */

static Monitor *monitor_by_name(const char *name) {
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (strcmp(m->wlr_output->name, name) == 0)
			return m;
	}
	return NULL;
}

static Client *client_by_id(uint32_t id) {
	Client *c;
	wl_list_for_each(c, &clients, link) {
		if (c->id == id)
			return c;
	}
	return NULL;
}

static const char *ipc_get_layout_str(void) {
	struct wlr_keyboard *keyboard = &kb_group->wlr_group->keyboard;
	xkb_layout_index_t current = xkb_state_serialize_layout(
		keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
	static char layout[32];
	const char *name = xkb_keymap_layout_get_name(keyboard->keymap, current);
	snprintf(layout, sizeof(layout), "%s", name ? name : "");
	return layout;
}

static cJSON *tags_mask_to_array(uint32_t tagmask) {
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < LENGTH(tags); i++)
		if (tagmask & (1 << i))
			cJSON_AddItemToArray(arr, cJSON_CreateNumber(i + 1));
	return arr;
}

static cJSON *build_tags_json(Monitor *m) {
	cJSON *tags_array = cJSON_CreateArray();
	Client *c = NULL;
	for (int tag = 1; tag <= LENGTH(tags); tag++) {
		int numclients = 0;
		bool is_active = false, is_urgent = false;
		uint32_t tagmask = 1 << (tag - 1);
		if (tagmask & m->tagset[m->seltags])
			is_active = true;
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			if (!(c->tags & tagmask & TAGMASK))
				continue;
			if (c->isurgent)
				is_urgent = true;
			numclients++;
		}
		char tagname[64];
		tag_display_name(m, tag, tagname, sizeof(tagname));
		cJSON *tag_obj = cJSON_CreateObject();
		cJSON_AddNumberToObject(tag_obj, "index", tag);
		cJSON_AddStringToObject(tag_obj, "name", tagname);
		cJSON_AddBoolToObject(tag_obj, "is_active", is_active);
		cJSON_AddBoolToObject(tag_obj, "is_urgent", is_urgent);
		cJSON_AddStringToObject(tag_obj, "layout",
								m->pertag->ltidxs[tag]->symbol);
		cJSON_AddNumberToObject(tag_obj, "client_count", numclients);
		cJSON_AddItemToArray(tags_array, tag_obj);
	}
	return tags_array;
}

static cJSON *monitor_active_client(Monitor *m) {
	cJSON *obj = cJSON_CreateObject();
	if (!m->sel) {
		cJSON_AddNullToObject(obj, "id");
		cJSON_AddNullToObject(obj, "title");
		cJSON_AddNullToObject(obj, "appid");
		return obj;
	}
	Client *c = m->sel;
	cJSON_AddNumberToObject(obj, "id", c->id);
	cJSON_AddStringToObject(obj, "title", client_get_title(c));
	cJSON_AddStringToObject(obj, "appid", client_get_appid(c));
	return obj;
}

static cJSON *monitor_active_tags(Monitor *m) {
	cJSON *arr = cJSON_CreateArray();
	uint32_t tagset;
	if (m->isoverview) {
		cJSON_AddItemToArray(arr, cJSON_CreateNumber(0));
		return arr;
	}
	tagset = m->tagset[m->seltags];
	for (int i = 0; i < LENGTH(tags); i++)
		if (tagset & (1 << i))
			cJSON_AddItemToArray(arr, cJSON_CreateNumber(i + 1));
	return arr;
}

static cJSON *build_client_json(Client *c) {
	cJSON *obj = cJSON_CreateObject();

	cJSON_AddNumberToObject(obj, "id", c->id);
	cJSON_AddNumberToObject(obj, "pid", c->pid);
	cJSON_AddStringToObject(obj, "foreign_toplevel_id",
							c->ext_foreign_toplevel
								? c->ext_foreign_toplevel->identifier
								: "");
	cJSON_AddStringToObject(obj, "title", client_get_title(c));
	cJSON_AddStringToObject(obj, "appid", client_get_appid(c));
	cJSON_AddBoolToObject(obj, "is_xwayland", client_is_x11(c));
	/*
	 * M6A. Whether this window is mid-animation, geometry or opacity.
	 *
	 * Exposed because "when did the animation finish" is a question about TIME
	 * and every other way of asking it answers a different one: a screenshot
	 * compares pixels, and polling geometry until it stops moving cannot tell
	 * a finished animation from a stalled one. ADR-607 statement 3 -- that a
	 * completion happens at the same wall-clock instant whatever the refresh
	 * rate -- needs exactly this bit and nothing more.
	 */
	cJSON_AddBoolToObject(obj, "animating",
		c->animation.running || c->opacity_animation.running);
	/*
	 * PRESENTATION geometry -- where the window actually is on screen right
	 * now, evaluated at the last pass's sample instant (ADR-611).
	 *
	 * Distinct from x/y/width/height above, and the distinction is the whole
	 * point: `x` jumps to the target the moment a move is dispatched, so a
	 * fixture polling it during an animation sees a step function and learns
	 * nothing about motion. Any oracle asking "where was the window while it
	 * moved" -- retarget continuity, fractional placement, damage bounds --
	 * needs this one.
	 */
	cJSON_AddNumberToObject(obj, "anim_x", c->animation.current.x);
	cJSON_AddNumberToObject(obj, "anim_y", c->animation.current.y);
	cJSON_AddNumberToObject(obj, "anim_width", c->animation.current.width);
	cJSON_AddNumberToObject(obj, "anim_height", c->animation.current.height);
	cJSON_AddStringToObject(obj, "icon", c->icon_name ? c->icon_name : "");
	cJSON_AddStringToObject(obj, "monitor",
							c->mon ? c->mon->wlr_output->name : "");
	/*
	 * ── M6B/D6: THE COMPOSITOR'S OWN ANSWER, SO A FIXTURE HAS TWO WITNESSES ──
	 *
	 * `monitor` above is where the LAYOUT put the window. These two are what
	 * the colour policy RESOLVED for its surface, and the whole point is that
	 * they must agree: the frog wrong-display defect was precisely a colour
	 * policy that disagreed with the layout.
	 *
	 * A client-side observation alone cannot prove which output was described
	 * when two outputs are both SDR -- the serialized bytes are identical. So
	 * the fixture reads the compositor's resolved output and identity here and
	 * the client's received tuple over the wire, and asserts they correspond.
	 * Neither observation is sufficient; together they are.
	 */
	{
		struct az_preferred pref;
		az_preferred_resolve(client_surface(c), &pref);
		cJSON_AddStringToObject(obj, "preferred_output",
			pref.mon != NULL ? pref.mon->wlr_output->name : "");
		/* As a string: a 64-bit hash does not survive a JSON double. */
		char idbuf[32];
		snprintf(idbuf, sizeof(idbuf), "%" PRIu64, pref.identity);
		cJSON_AddStringToObject(obj, "preferred_identity", idbuf);
		cJSON_AddBoolToObject(obj, "preferred_hdr", pref.hdr);
		cJSON_AddNumberToObject(obj, "preferred_max_luminance",
			pref.max_luminance);
		cJSON_AddNumberToObject(obj, "preferred_min_luminance",
			pref.min_luminance);
		cJSON_AddNumberToObject(obj, "preferred_max_fall", pref.max_fall);
	}
	cJSON_AddItemToObject(obj, "tags", tags_mask_to_array(c->tags));
	cJSON_AddBoolToObject(obj, "is_focused", c->isfocused);
	cJSON_AddBoolToObject(obj, "is_fullscreen", c->isfullscreen);
	/* "vrr" is the honest per-client answer to "is this app under variable
	 * refresh right now": VRR is an output-wide hardware state, so it's the
	 * committed adaptive-sync status of the client's monitor (same query the
	 * monitor JSON uses, not our own intent bookkeeping). "vrr_only_fullscreen"
	 * is the per-client window rule that makes this app *drive* VRR while
	 * fullscreen -- the "why" behind the flag. */
	cJSON_AddBoolToObject(obj, "vrr",
						  c->mon && c->mon->wlr_output->adaptive_sync_status ==
										WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
	cJSON_AddBoolToObject(obj, "vrr_only_fullscreen",
						  c->vrr_only_fullscreen > 0);
	cJSON_AddBoolToObject(obj, "is_floating", c->isfloating);
	cJSON_AddBoolToObject(obj, "is_maximized", c->ismaximizescreen);
	cJSON_AddBoolToObject(obj, "is_global", c->isglobal);
	cJSON_AddBoolToObject(obj, "is_unglobal", c->isunglobal);
	cJSON_AddBoolToObject(obj, "is_overlay", c->isoverlay);
	cJSON_AddBoolToObject(obj, "is_fakefullscreen", c->isfakefullscreen);
	cJSON_AddBoolToObject(obj, "is_minimized", c->isminimized);
	/* xdg_toplevel.suspended as last configured: true means we have told this
	 * client its content isn't visible and it may stop rendering. Reported
	 * because the state is otherwise unobservable from outside -- it lives
	 * only on the wire, and inferring it from tags re-derives the very
	 * predicate you'd be trying to check. Always false for X11 (no
	 * equivalent) and for clients that bound xdg_wm_base below v6, which must
	 * never be sent the state at all. */
	cJSON_AddBoolToObject(obj, "is_suspended", c->issuspended);
	cJSON_AddBoolToObject(obj, "is_urgent", c->isurgent);
	cJSON_AddBoolToObject(obj, "is_scratchpad", c->is_in_scratchpad);
	cJSON_AddBoolToObject(obj, "is_namedscratchpad", c->isnamedscratchpad);
	cJSON_AddStringToObject(obj, "special_workspace",
							c->special_name ? c->special_name : "");
	cJSON_AddBoolToObject(obj, "pinned", c->ispinned);
	/*
	 * SEMANTIC geometry -- where the compositor has decided this window
	 * belongs. During an animation it is the TARGET and it jumps there the
	 * instant the decision is made; it is not where the window is drawn.
	 */
	cJSON_AddNumberToObject(obj, "x", c->geom.x);
	cJSON_AddNumberToObject(obj, "y", c->geom.y);
	cJSON_AddNumberToObject(obj, "width", c->geom.width);
	cJSON_AddNumberToObject(obj, "height", c->geom.height);
	cJSON_AddNumberToObject(obj, "scroller_proportion",
							(double)c->scroller_proportion);
	return obj;
}

/*
 * ── M11: WHAT IS BEING DONE TO ONE SURFACE ────────────────────────────────
 *
 * Serializes az_surface_intent_resolve()'s snapshot. Everything here is read
 * from production state; nothing is recomputed for display, which is the whole
 * contract of the inspector (see az_intent.h).
 *
 * `role` and `name` are the caller's, because a wl_surface does not know
 * whether it is a toplevel or a panel and the answer matters to whoever is
 * reading.
 */
struct intent_walk {
	cJSON *arr;
	const char *name;
	Client *client;
	const char *toplevel_role;
};

static cJSON *build_surface_intent_json(struct wlr_surface *s,
		const char *role, const char *name, Client *ic);

/* One entry per surface the client actually presents, subsurfaces included;
 * see the call site for what reporting only the toplevel got wrong. */
static void intent_add_surface(struct wlr_surface *s, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	struct intent_walk *w = data;
	const char *role = wlr_subsurface_try_from_wlr_surface(s) != NULL
		? "subsurface" : w->toplevel_role;
	cJSON_AddItemToArray(w->arr,
		build_surface_intent_json(s, role, w->name, w->client));
}

static cJSON *build_surface_intent_json(struct wlr_surface *s,
		const char *role, const char *name, Client *ic) {
	struct az_surface_intent in;
	az_surface_intent_resolve(s, &in);

	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "role", role);
	cJSON_AddStringToObject(o, "name", name != NULL ? name : "");
	/*
	 * THE APP-ID, because it is what a rule MATCHES ON and the title is not.
	 * The dump reported only the title, so reading "this window classed wrong"
	 * and writing the rule to fix it needed a second query against
	 * `get all-clients`. A diagnostic that tells you there is a problem but not
	 * the identifier you need to act on is half a diagnostic. Empty for layer
	 * surfaces, which have a namespace instead and cannot carry window rules.
	 */
	cJSON_AddStringToObject(o, "app_id",
		ic != NULL && client_get_appid(ic) != NULL ? client_get_appid(ic) : "");
	cJSON_AddStringToObject(o, "output",
		in.mon != NULL && in.mon->wlr_output != NULL
			? in.mon->wlr_output->name : "");
	/*
	 * IDENTITY IS A STRING, and that is not cosmetic.
	 *
	 * It is a 64-bit FNV-1a hash. JSON numbers are doubles, so everything above
	 * 2^53 is rounded -- the first live run printed 1.6541738727388557E+19,
	 * which is not the hash and cannot round-trip. A consumer comparing the
	 * rounded value for change would silently miss any change that survives
	 * rounding, which is exactly the job this field exists to do.
	 */
	char ident[24];
	snprintf(ident, sizeof(ident), "0x%016" PRIx64, in.identity);
	cJSON_AddStringToObject(o, "identity", ident);

	cJSON *b = cJSON_AddObjectToObject(o, "buffer");
	cJSON_AddBoolToObject(b, "attached", in.has_buffer);
	if (in.has_buffer) {
		cJSON_AddNumberToObject(b, "width", in.buf_width);
		cJSON_AddNumberToObject(b, "height", in.buf_height);
		cJSON_AddStringToObject(b, "kind",
			in.buf_dmabuf ? "dmabuf" : (in.buf_shm ? "shm" : "other"));
		/* fourcc as its four characters: a bare 875713112 is unreadable, and
		 * this is a diagnostic before it is data. */
		char fourcc[5] = {
			(char)(in.buf_format & 0xff), (char)((in.buf_format >> 8) & 0xff),
			(char)((in.buf_format >> 16) & 0xff),
			(char)((in.buf_format >> 24) & 0xff), 0 };
		cJSON_AddStringToObject(b, "format", in.buf_format ? fourcc : "");
		if (in.buf_dmabuf) {
			char mod[32];
			snprintf(mod, sizeof(mod), "0x%016" PRIx64, in.buf_modifier);
			cJSON_AddStringToObject(b, "modifier", mod);
		}
	}

	/*
	 * SOURCE. `tagged` is the load-bearing field and it is reported first:
	 * essentially every surface on a real desktop is untagged, and reading
	 * "srgb/bt709" without it invites the conclusion that a client declared
	 * sRGB when in fact it declared nothing and ADR-004 answered for it.
	 */
	cJSON *src = cJSON_AddObjectToObject(o, "source");
	cJSON_AddBoolToObject(src, "tagged", in.src.tagged);
	cJSON_AddStringToObject(src, "transfer",
		in.src.tagged ? az_tf_name(in.src.tf) : "(untagged)");
	cJSON_AddStringToObject(src, "primaries",
		in.src.tagged ? az_primaries_name(in.src.primaries) : "(untagged)");
	cJSON_AddNumberToObject(src, "max_cll_nits", in.src.max_cll);

	/*
	 * M12. The class, and WHERE IT CAME FROM -- because "hdr-content" derived
	 * from a PQ description and "hdr-content" because someone wrote a rule are
	 * different facts, and only the second survives the client changing its
	 * mind. The applied multipliers are reported beside it so the class's
	 * effect is visible rather than implied.
	 */
	cJSON *lc = cJSON_AddObjectToObject(o, "luminance");
	cJSON_AddStringToObject(lc, "class", az_lum_class_name(in.lum_class));
	/* WHICH KIND of rule, because they are different files and different
	 * matchers: a window rule matches app-id and title, a layerrule matches a
	 * namespace, and a reader told "window-rule" about a bar would go looking
	 * in the wrong half of the config. Layer surfaces are serialized with no
	 * Client, which is exactly the distinction. */
	cJSON_AddStringToObject(lc, "class_from",
		!in.lum_class_from_rule ? "derived"
			: (ic != NULL ? "window-rule" : "layer-rule"));
	cJSON_AddNumberToObject(lc, "sdr_white_scale", in.lum_rules.sdr_white_scale);
	cJSON_AddNumberToObject(lc, "hdr_gain", in.lum_rules.hdr_gain);

	cJSON *dom = cJSON_AddObjectToObject(o, "domain");
	cJSON_AddStringToObject(dom, "transfer", az_tf_name(in.domain.tf));
	cJSON_AddStringToObject(dom, "primaries",
		az_primaries_name(in.domain.primaries));
	cJSON_AddNumberToObject(dom, "scale", in.domain.scale);
	/* 0 means UNKNOWN, never "this source is black" -- az_lum.h's contract. */
	cJSON_AddNumberToObject(dom, "content_peak_scene", in.domain.content_peak);
	cJSON_AddBoolToObject(dom, "hdr_transfer", az_lum_tf_is_hdr(in.domain.tf));

	cJSON *pr = cJSON_AddObjectToObject(o, "preferred");
	cJSON_AddBoolToObject(pr, "hdr", in.pref.hdr);
	cJSON_AddBoolToObject(pr, "bt2020", in.pref.bt2020);
	cJSON_AddNumberToObject(pr, "max_luminance", in.pref.max_luminance);
	cJSON_AddNumberToObject(pr, "min_luminance", in.pref.min_luminance);
	cJSON_AddNumberToObject(pr, "max_fall", in.pref.max_fall);
	snprintf(ident, sizeof(ident), "0x%016" PRIx64, in.pref.identity);
	cJSON_AddStringToObject(pr, "identity", ident);

	/*
	 * M13. The presentation class and what it actually bought, which is the
	 * half a class name alone does not tell you: "game" is only interesting if
	 * you can see whether it got VRR and whether it may tear. Both are read
	 * from the same predicates the commit path uses.
	 */
	{
		cJSON *pc = cJSON_AddObjectToObject(o, "presentation");
		bool pc_ruled = false;
		enum az_present_class klass = az_present_class_of(ic, &pc_ruled);
		cJSON_AddStringToObject(pc, "class", az_present_class_name(klass));
		/*
		 * EVERY SURFACE ANSWERS, including layer-shell ones, which used to get
		 * no `presentation` object at all. A missing key reads as "unknown",
		 * and the truth for a layer surface is known and structural: window
		 * rules match app-id and title and it has neither, wp-content-type is
		 * a toplevel concern, so it is desktop-ui and cannot be otherwise.
		 * Saying that is better than omitting it -- and a null also made
		 * `select(.presentation.class != "desktop-ui")` match every panel,
		 * which is how this was noticed.
		 */
		cJSON_AddStringToObject(pc, "class_from",
			ic == NULL ? "layer-shell" : (pc_ruled ? "window-rule" : "derived"));
		cJSON_AddBoolToObject(pc, "fullscreen",
			ic != NULL && ic->isfullscreen);
		/*
		 * TWO TEARING FIELDS, because there are two questions and reporting
		 * only the second was wrong.
		 *
		 * `tearing_eligible` is this WINDOW's answer: does it ask to tear.
		 * `tearing_active` is the COMPOSITOR's answer for its output right
		 * now, which additionally requires this window to be the focused one
		 * and the global setting to permit it.
		 *
		 * The first version printed check_tearing_frame_allow() on every row.
		 * That function reads selmon->sel and ignores which window is being
		 * asked about, so every row carried the FOCUSED window's answer --
		 * a global dressed as a per-surface fact, which is the one thing an
		 * inspector must never do.
		 */
		cJSON_AddBoolToObject(pc, "tearing_eligible",
			client_tearing_eligible(ic));
		cJSON_AddBoolToObject(pc, "tearing_active",
			ic != NULL && ic->mon != NULL && selmon != NULL
				&& selmon->sel == ic && check_tearing_frame_allow(ic->mon));
		/* VRR is genuinely per-output, and is named so it cannot be read as a
		 * property of this window. */
		/*
		 * WHAT THE CLIENT COMMITS AT, beside what the output presents at.
		 * Reported as Hz because that is the unit the question is asked in
		 * ("is this 23.976 content arriving as 23.976"), and the two numbers
		 * are only interesting together: a 23.976Hz client on a 144Hz fixed
		 * output cannot be presented evenly, and seeing both is what makes
		 * that visible rather than inferred.
		 */
		if (ic != NULL && ic->commit_interval_n > 0) {
			double mean_ns = (double)ic->commit_interval_sum_ns
				/ (double)ic->commit_interval_n;
			cJSON_AddNumberToObject(pc, "commit_hz",
				mean_ns > 0.0 ? 1.0e9 / mean_ns : 0.0);
			cJSON_AddNumberToObject(pc, "commit_samples",
				(double)ic->commit_interval_n);
		}
		if (in.mon != NULL) {
			/*
			 * ── TWO RATES, AND DIVIDING BY THE WRONG ONE LIES ──────────────
			 *
			 * `vblank_hz` is the panel's scan rate, from the presenter's
			 * observed period. `presented_hz` is how often a frame actually
			 * REACHED the screen. On a fixed output they are the same number.
			 * UNDER VRR THEY ARE NOT: the panel can free-run far faster than
			 * the compositor commits.
			 *
			 * This shipped dividing the client's commit interval by the vblank
			 * period and calling it "vblanks_per_frame". On DP-1 playing a
			 * 23.976fps film it read 4.5, which was taken as evidence that VRR
			 * was NOT following the content and that cadence work was needed.
			 * `get presentation` then showed 240 presented frames in 10
			 * seconds -- exactly 24/s, exactly the content -- so VRR was
			 * following it perfectly and the metric was measuring the panel's
			 * scan rate against the film's frame rate, which is a ratio of two
			 * unrelated things.
			 *
			 * A number that produces a confident wrong conclusion is worse than
			 * no number. The ratio is now against `presented_hz`, where 1.0
			 * means "one presentation per committed frame" -- the thing the
			 * question was actually asking.
			 */
			uint64_t vb = az_presenter_period_ns(in.mon);
			cJSON_AddNumberToObject(pc, "vblank_hz",
				vb > 0 ? 1.0e9 / (double)vb : 0.0);
			/*
			 * ABSENT, NOT ZERO, when the backend has given no presentation
			 * timing -- which is every headless output. A reported 0 reads as
			 * "nothing reached the screen", which is a different and wrong
			 * claim; the absence of the key says "not measured here".
			 */
			uint64_t pres = in.mon->present_interval_ns;
			if (pres > 0) {
				cJSON_AddNumberToObject(pc, "presented_hz",
					1.0e9 / (double)pres);
			}
			if (pres > 0 && ic != NULL && ic->commit_interval_n > 0) {
				double mean_ns = (double)ic->commit_interval_sum_ns
					/ (double)ic->commit_interval_n;
				cJSON_AddNumberToObject(pc, "presents_per_frame",
					mean_ns / (double)pres);
			}
		}
		cJSON_AddBoolToObject(pc, "output_vrr_active",
			in.mon != NULL && in.mon->is_vrr_opening);
	}

	cJSON *rn = cJSON_AddObjectToObject(o, "render");
	/*
	 * M13B. The verdict for THIS surface, evaluated now from production state
	 * -- not the output's last-frame verdict, which would answer about
	 * whichever window happened to be fullscreen. Free: everything but
	 * KMS_REFUSED is decidable without touching the display.
	 */
	enum az_scanout_verdict sv = AZ_SCANOUT_NO_CANDIDATE;
	if (ic != NULL && in.mon != NULL) {
		Client *cand = NULL;
		sv = az_scanout_eligible(in.mon, NULL, &cand);
		/*
		 * Eligible, but for a DIFFERENT window: say so rather than letting this
		 * surface inherit another's verdict.
		 *
		 * `cand != NULL` matters. When there is no candidate at all the
		 * evaluator has already said something precise -- no-candidate, or
		 * not-visible -- and overwriting that because NULL != ic threw the
		 * precise answer away. That is what made not-visible unreachable the
		 * first time it existed.
		 */
		if (cand != NULL && cand != ic) {
			sv = AZ_SCANOUT_NO_CANDIDATE;
		}
	}
	cJSON_AddBoolToObject(rn, "direct_scanout",
		ic != NULL && in.mon != NULL && sv == AZ_SCANOUT_ACCEPTED
			&& in.mon->scanout_verdict == (int32_t)AZ_SCANOUT_ACCEPTED);
	cJSON_AddStringToObject(rn, "scanout", az_scanout_verdict_name(sv));
	cJSON_AddStringToObject(rn, "scanout_why", az_scanout_verdict_why(sv));

	return o;
}

// Available output modes, for a config UI's resolution/refresh picker.
// refresh is wlroots-native millihertz (divide by 1000 for Hz).
static cJSON *build_modes_json(Monitor *m) {
	cJSON *arr = cJSON_CreateArray();
	struct wlr_output_mode *mode;
	wl_list_for_each(mode, &m->wlr_output->modes, link) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddNumberToObject(o, "width", mode->width);
		cJSON_AddNumberToObject(o, "height", mode->height);
		cJSON_AddNumberToObject(o, "refresh", mode->refresh);
		cJSON_AddBoolToObject(o, "current", mode == m->wlr_output->current_mode);
		cJSON_AddBoolToObject(o, "preferred", mode->preferred);
		cJSON_AddItemToArray(arr, o);
	}
	return arr;
}

static cJSON *build_monitor_json(Monitor *m) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddStringToObject(resp, "name", m->wlr_output->name);
	cJSON_AddBoolToObject(resp, "active", m == selmon);
	cJSON_AddBoolToObject(resp, "enabled", m->wlr_output->enabled);
	cJSON_AddBoolToObject(resp, "asleep", (bool)m->asleep);
	cJSON_AddNumberToObject(resp, "x", m->m.x);
	cJSON_AddNumberToObject(resp, "y", m->m.y);
	cJSON_AddNumberToObject(resp, "width", m->m.width);
	cJSON_AddNumberToObject(resp, "height", m->m.height);
	cJSON_AddNumberToObject(resp, "scale", m->wlr_output->scale);
	cJSON_AddNumberToObject(resp, "layout_index",
							m->pertag->ltidxs[m->pertag->curtag] - layouts);
	cJSON_AddStringToObject(resp, "layout_symbol",
							m->pertag->ltidxs[m->pertag->curtag]->symbol);
	cJSON_AddStringToObject(resp, "last_open_surface", m->last_open_surface);
	/* "hdr"/"vrr" report the actual, currently-committed hardware state
	 * (queried straight from wlroots, not our own intent bookkeeping) --
	 * a commit can be pending, rejected, or mid-retrain, so m->hdr / the
	 * configured vrr flag alone can briefly (or, if something's stuck,
	 * indefinitely) disagree with what the display is really doing.
	 * "*_enabled" is the persisted/intended setting (what a config UI
	 * should read/write); "*_capable" is a static hardware capability
	 * check. */
	cJSON_AddBoolToObject(resp, "hdr", m->wlr_output->image_description != NULL);
	cJSON_AddBoolToObject(resp, "hdr_enabled", m->hdr);
	/*
	 * The BASELINE, as a tri-state: -1 nobody has spoken for this output,
	 * 0 explicitly off, 1 explicitly on. hdr_enabled cannot carry it -- it is
	 * a bool, so an unconfigured output and an explicitly-on one are the same
	 * value on the wire.
	 *
	 * That flattening is why a real defect stayed invisible: setmon assigned
	 * the tri-state straight into the effective m->hdr, so every unconfigured
	 * output ran as HDR (-1 is truthy), and IPC reported hdr_enabled=true --
	 * indistinguishable from working correctly. A fixture could see the wrong
	 * PIXELS but not the wrong INTENT.
	 */
	cJSON_AddNumberToObject(resp, "hdr_configured", m->hdr_configured);
	cJSON_AddBoolToObject(resp, "hdr_capable",
						  (m->wlr_output->supported_primaries &
						   WLR_COLOR_NAMED_PRIMARIES_BT2020) &&
							  (m->wlr_output->supported_transfer_functions &
							   WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ));
	/* The bit depth in USE, not the one configured. m->bitdepth is the
	 * config value where 0 means "auto" -- reporting that verbatim answered
	 * "what depth is this output running at?" with 0, which is not a depth.
	 * Same split as hdr/hdr_enabled above: the plain name is what the output
	 * is really doing, *_enabled is what was asked for. */
	cJSON_AddNumberToObject(
		resp, "bitdepth",
		m->wlr_output->render_format == DRM_FORMAT_XRGB2101010 ? 10 : 8);
	cJSON_AddNumberToObject(resp, "bitdepth_enabled", m->bitdepth);
	cJSON_AddNumberToObject(resp, "hdr_max_luminance", m->hdr_max_luminance);
	cJSON_AddNumberToObject(resp, "hdr_min_luminance", m->hdr_min_luminance);
	cJSON_AddNumberToObject(resp, "hdr_max_fall", m->hdr_max_fall);
	cJSON_AddStringToObject(resp, "icc_profile", m->icc_path);
	/*
	 * M6B/G2. WHICH RENDERER IS APPLYING THE PROFILE, and how the output is
	 * leaving the scene. `icc_profile` alone answers "was one configured",
	 * which is the question that was already answerable from the config file;
	 * these two answer the one that is not, and that a headless fixture has no
	 * other way to ask. `color_encode_tf` reading "lut1d" is AVK carrying the
	 * profile itself; "srgb" with a path of "fallback" is SceneFX carrying it.
	 *
	 * M6C adds the third answer: "clut3d" is AVK carrying a profile that does
	 * NOT reduce to a matrix and a curve, as a 65-cube. `icc_shaper` and
	 * `icc_clut` are mutually exclusive by construction and both false with no
	 * profile, so the three states are distinguishable from these two booleans
	 * without consulting the path.
	 */
	cJSON_AddStringToObject(resp, "color_path",
							az_output_path_name(m->color_state.path));
	cJSON_AddStringToObject(resp, "color_encode_tf",
							az_tf_name(m->color_state.encode_tf));
	cJSON_AddBoolToObject(resp, "icc_shaper", m->icc_shaper_ok);
	cJSON_AddBoolToObject(resp, "icc_clut", m->icc_clut != NULL);
	cJSON_AddNumberToObject(resp, "sdr_luminance",
							config.sdr_reference_luminance > 0
								? config.sdr_reference_luminance
								: 203);
	cJSON_AddBoolToObject(resp, "vrr",
						  m->wlr_output->adaptive_sync_status ==
							  WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
	cJSON_AddBoolToObject(resp, "vrr_enabled", m->vrr_global_enable);
	cJSON_AddBoolToObject(resp, "vrr_capable", m->wlr_output->adaptive_sync_supported);
	cJSON_AddItemToObject(resp, "modes", build_modes_json(m));
	if (m->wlr_output->current_mode) {
		cJSON_AddNumberToObject(resp, "mode_width",
								m->wlr_output->current_mode->width);
		cJSON_AddNumberToObject(resp, "mode_height",
								m->wlr_output->current_mode->height);
		cJSON_AddNumberToObject(resp, "mode_refresh",
								m->wlr_output->current_mode->refresh);
	}
	cJSON_AddItemToObject(resp, "tags", build_tags_json(m));
	cJSON_AddItemToObject(resp, "active_tags", monitor_active_tags(m));
	cJSON_AddItemToObject(resp, "active_client", monitor_active_client(m));
	cJSON_AddItemToObject(resp, "keymode", cJSON_CreateString(keymode.mode));
	cJSON_AddItemToObject(resp, "keyboardlayout",
						  cJSON_CreateString(ipc_get_layout_str()));
	cJSON_AddStringToObject(resp, "active_special",
							m->active_special ? m->active_special : "");
	return resp;
}

/* Whether idling is being held off, and by whom.
 *
 * Two flags rather than one, because a client that wants to DRAW this state
 * and a client that wants to change it need different answers. `inhibited` is
 * what the idle notifier was actually told and is the honest answer to "will
 * this machine sleep"; `manual` is the flag `toggle_idle_inhibit` owns, and is
 * the only one a bar's toggle may show as its own -- a pill lit because mpv
 * holds an inhibitor would go dark when the user clicked it, having in fact
 * changed nothing they can see.
 *
 * `portal` is the third answer, to a question the first two cannot settle:
 * WHO. A request that arrives over org.freedesktop.portal.Inhibit has no
 * window and no surface, so there is nothing on screen to point at when the
 * machine will not sleep -- which is exactly the shape of the failure that
 * matters, a laptop awake in a bag with no visible cause. Each entry carries
 * the app that asked and the reason it gave; `flags` is the portal's own
 * bitmask (1 logout, 2 user-switch, 4 suspend, 8 idle), so an entry that
 * appears here without raising `inhibited` is one asteroidz recorded and does
 * not enforce. See src/ipc/inhibit-portal.h. */
static cJSON *build_idle_response(void) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddBoolToObject(resp, "inhibited", idle_inhibited);
	cJSON_AddBoolToObject(resp, "manual", idle_inhibit_manual);

	cJSON *portal = cJSON_AddArrayToObject(resp, "portal");
	const char *app_id, *reason;
	uint32_t flags;
	for (size_t i = 0; inhibit_portal_get(i, &app_id, &reason, &flags); i++) {
		cJSON *entry = cJSON_CreateObject();
		cJSON_AddStringToObject(entry, "app_id", app_id);
		cJSON_AddStringToObject(entry, "reason", reason);
		cJSON_AddNumberToObject(entry, "flags", flags);
		cJSON_AddBoolToObject(entry, "logout", flags & 1);
		cJSON_AddBoolToObject(entry, "user_switch", flags & 2);
		cJSON_AddBoolToObject(entry, "suspend", flags & 4);
		cJSON_AddBoolToObject(entry, "idle", flags & 8);
		cJSON_AddItemToArray(portal, entry);
	}
	return resp;
}

static cJSON *build_all_tags_entry(Monitor *m) {
	cJSON *entry = cJSON_CreateObject();
	cJSON_AddStringToObject(entry, "monitor", m->wlr_output->name);
	cJSON_AddItemToObject(entry, "tags", build_tags_json(m));
	return entry;
}

static cJSON *build_all_tags_response(void) {
	cJSON *arr = cJSON_CreateArray();
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		cJSON_AddItemToArray(arr, build_all_tags_entry(m));
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddItemToObject(resp, "all_tags", arr);
	return resp;
}

static cJSON *build_monitor_tags_response(Monitor *m) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddStringToObject(resp, "monitor", m->wlr_output->name);
	cJSON_AddItemToObject(resp, "tags", build_tags_json(m));
	cJSON_AddItemToObject(resp, "active_tags", monitor_active_tags(m));
	return resp;
}

/* Colours as [r,g,b,a] floats, not "#rrggbbaa".
 *
 * Hex would have to pick a byte order, and the two obvious ones disagree:
 * CSS reads #RRGGBBAA, Qt reads #AARRGGBB, and a string that parses under both
 * conventions but means different things is the kind of bug that shows up as
 * "the bar is slightly the wrong colour" months later. Four floats in the
 * range the compositor already stores them in cannot be misread. */
static void bar_cfg_color(cJSON *o, const char *key, const float c[4]) {
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < 4; i++)
		cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)c[i]));
	cJSON_AddItemToObject(o, key, arr);
}

/* The theme, resolved -- and nothing else any more.
 *
 * This used to carry the bar's whole appearance: its height, its pills, its
 * panel, its popovers, its module lists, its idle timeouts. Sixty-two values
 * the compositor stored, clamped, described and served, and never once read.
 * They were here because the compositor used to DRAW the bar, and its config
 * described one; when the bar moved out of process the config did not follow
 * it, so a program that draws no bar went on being the authority on what a bar
 * looks like. They live in the bar's own config now.
 *
 * What is left is the part that is genuinely the compositor's. The theme is
 * shared -- titlebars, the overview and the bar all draw from it -- and the
 * compositor is the only process that knows what it currently IS, because
 * matugen rewrites it at runtime whenever the wallpaper changes. Sending the
 * file path instead would be two KDL readers that agree until one of them
 * gains a default, and it would still not see a palette written after
 * startup. */
static cJSON *build_bar_config_response(void) {
	cJSON *resp = cJSON_CreateObject();

	/* The shared UI theme, not a bar-private one: titlebars, the overview and
	 * the bar have always drawn from this, and a bar in another process must
	 * keep doing so or it stops matching the desktop on the next matugen run. */
	cJSON *theme = cJSON_CreateObject();
	bar_cfg_color(theme, "fg", config.theme.fg_color);
	bar_cfg_color(theme, "bg", config.theme.bg_color);
	bar_cfg_color(theme, "focus_fg", config.theme.focus_fg_color);
	bar_cfg_color(theme, "focus_bg", config.theme.focus_bg_color);
	bar_cfg_color(theme, "urgent", config.theme.urgent_color);
	bar_cfg_color(theme, "border", config.theme.border_color);
	cJSON_AddNumberToObject(theme, "border_width", config.theme.border_width);
	cJSON_AddNumberToObject(theme, "corner_radius", config.theme.corner_radius);
	cJSON_AddNumberToObject(theme, "padding_x", config.theme.padding_x);
	cJSON_AddNumberToObject(theme, "padding_y", config.theme.padding_y);
	/* The same fallback the renderers use. set_value_default now assigns it, so
	 * this only catches the window between a reload freeing the string and the
	 * new one being parsed -- but reporting "" there told the bar to pick its
	 * own font while every native overlay drew at monospace Bold 16. */
	cJSON_AddStringToObject(theme, "font",
							config.theme.font_desc ? config.theme.font_desc
												   : "monospace Bold 16");
	cJSON_AddItemToObject(resp, "theme", theme);

	return resp;
}

/* The one-shot client whose command is being served right now.
 *
 * handle_command reaches its replies through a dozen paths (`send_static_json`
 * early-outs, the dispatch fast path, the cJSON tail) and threading a client
 * pointer through all of them would be a large diff for no gain: the IPC socket
 * is serviced from the compositor's own event loop, one command at a time, and
 * there is no reentrancy for it to get wrong. */
static struct ipc_client_state *ipc_serving;

static void send_static_json(int fd, const char *json_str) {
	(void)fd;
	if (!ipc_serving)
		return;
	ipc_out_append(&ipc_serving->out, json_str, strlen(json_str));
}

/* The cursor-shape protocol's names, as the protocol spells them.
 *
 * Its own names rather than any of ours: a caller comparing against "pointer"
 * is comparing against wp_cursor_shape_device_v1, which is also what the client
 * asked for, so there is no third vocabulary in between to keep in step. The
 * two DND-only shapes and the resize family are all here for the same reason --
 * a table that covers most of an enum is one somebody has to check. */
static const char *cursor_shape_name(enum wp_cursor_shape_device_v1_shape s) {
	switch (s) {
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT: return "default";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU: return "context-menu";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP: return "help";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER: return "pointer";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS: return "progress";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT: return "wait";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL: return "cell";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR: return "crosshair";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT: return "text";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT: return "vertical-text";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS: return "alias";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY: return "copy";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE: return "move";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP: return "no-drop";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED: return "not-allowed";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB: return "grab";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING: return "grabbing";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE: return "e-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE: return "n-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE: return "ne-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE: return "nw-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE: return "s-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE: return "se-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE: return "sw-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE: return "w-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE: return "ew-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE: return "ns-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE: return "nesw-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE: return "nwse-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE: return "col-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE: return "row-resize";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL: return "all-scroll";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN: return "zoom-in";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT: return "zoom-out";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK: return "dnd-ask";
	case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE: return "all-resize";
	}
	/* 0 is not a member: it is what the tracker holds before any client has
	 * named a shape, and after one attached a surface instead. */
	return "unset";
}

/* Declared here rather than beside ipc_init: `get instance` reports it, and
 * a harness pinning its target needs the endpoint the answer came from. */
static char ipc_socket_path[256];

/* ---------- one-shot command handling ---------- */
static void handle_command(int client_fd, const char *cmd_raw) {
	cJSON *resp = NULL;
	char *json_str = NULL;
	char cmd[1024];

	strncpy(cmd, cmd_raw, sizeof(cmd) - 1);
	cmd[sizeof(cmd) - 1] = '\0';
	for (char *p = cmd; *p; p++)
		if (*p == ',')
			*p = ' ';

	if (strcmp(cmd, "get version") == 0) {
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "version", VERSION);
	} else if (strcmp(cmd, "get instance") == 0) {
		/*
		 * WHICH COMPOSITOR IS ANSWERING. Everything a qualification harness
		 * needs to prove it is talking to the instance it means, in one
		 * round trip, so the check happens BEFORE any measurement rather
		 * than being reconstructed from a log afterwards.
		 *
		 * See az_identity.h for why `build` is the ELF build-id and not the
		 * path or VERSION: a freshly-installed-over binary reads
		 * "/usr/bin/asteroidz (deleted)", which is exactly the moment the
		 * question matters.
		 */
		resp = cJSON_CreateObject();
		cJSON_AddNumberToObject(resp, "pid", (double)getpid());
		cJSON_AddStringToObject(resp, "version", VERSION);
		cJSON_AddStringToObject(resp, "build", az_build_id());
		cJSON_AddStringToObject(resp, "socket", ipc_socket_path);
		char exe[512];
		ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
		exe[n > 0 ? n : 0] = '\0';
		cJSON_AddStringToObject(resp, "exe", exe);
		cJSON_AddStringToObject(resp, "backend",
								wlr_backend_is_headless(backend) ? "headless"
																 : "drm");
		const char *sess = getenv("DESKTOP_SESSION");
		cJSON_AddStringToObject(resp, "session", sess ? sess : "");
		/* One renderer, named rather than selected -- kept in the payload
		 * because instance reports parse it. */
		cJSON_AddStringToObject(resp, "renderer", "avk");
		/* The precondition an M6B live gate needs, from the instance itself
		 * rather than from whichever process happened to answer. */
		cJSON_AddBoolToObject(resp, "validation_enabled",
							  az_avk_validation_enabled());
	} else if (strcmp(cmd, "get cursorpos") == 0) {
		resp = cJSON_CreateObject();
		cJSON_AddNumberToObject(resp, "x", cursor->x);
		cJSON_AddNumberToObject(resp, "y", cursor->y);
		Monitor *m = xytomon(cursor->x, cursor->y);
		if (m)
			cJSON_AddStringToObject(resp, "monitor", m->wlr_output->name);
		else
			cJSON_AddNullToObject(resp, "monitor");
		/* What the pointer currently LOOKS like, which is the only way to check
		 * a client's hover cursors from outside it: the cursor is drawn by the
		 * compositor and does not appear in a screenshot, so a test driving a
		 * bar or a settings window has nothing else to assert against. Tracked
		 * here already -- setcursorshape() and the set_cursor request both
		 * write it -- and simply never reported.
		 *
		 * A client may name a shape or attach a surface of its own, and those
		 * are different answers rather than degrees of one: `shape` is null
		 * when a surface is in use. */
		if (last_cursor.surface) {
			cJSON_AddNullToObject(resp, "cursor-shape");
			cJSON_AddBoolToObject(resp, "cursor-surface", true);
		} else {
			cJSON_AddStringToObject(resp, "cursor-shape",
									cursor_shape_name(last_cursor.shape));
			cJSON_AddBoolToObject(resp, "cursor-surface", false);
		}
	} else if (strcmp(cmd, "get idle") == 0) {
		resp = build_idle_response();
	} else if (strcmp(cmd, "get keymode") == 0) {
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "keymode", keymode.mode);
	} else if (strcmp(cmd, "get keyboardlayout") == 0) {
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "layout", ipc_get_layout_str());
	} else if (strcmp(cmd, "get last_open_surface") == 0 ||
			   strncmp(cmd, "get last_open_surface ", 22) == 0) {
		Monitor *m;
		if (cmd[21] == '\0') { // exactly "get last_open_surface"
			m = selmon;
		} else {
			m = monitor_by_name(cmd + 22);
		}
		if (!m) {
			send_static_json(client_fd, "{\"error\":\"monitor not found\"}\n");
			return;
		}
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "monitor", m->wlr_output->name);
		cJSON_AddStringToObject(resp, "last_open_surface",
								m->last_open_surface);
	} else if (strncmp(cmd, "get monitor ", 12) == 0) {
		Monitor *m = monitor_by_name(cmd + 12);
		if (!m) {
			send_static_json(client_fd, "{\"error\":\"monitor not found\"}\n");
			return;
		}
		resp = build_monitor_json(m);
	} else if (strcmp(cmd, "get focused-client") == 0) {
		if (selmon && selmon->sel) {
			resp = build_client_json(selmon->sel);
		} else {
			send_static_json(client_fd, "{\"error\":\"no focused client\"}\n");
			return;
		}
	} else if (strncmp(cmd, "get client ", 11) == 0) {
		Client *c = client_by_id((uint32_t)atoi(cmd + 11));
		if (!c) {
			send_static_json(client_fd, "{\"error\":\"client not found\"}\n");
			return;
		}
		resp = build_client_json(c);
	} else if (strncmp(cmd, "get tag ", 8) == 0) {
		char mon_name[64];
		int ext_tag_idx;
		if (sscanf(cmd + 8, "%63s %d", mon_name, &ext_tag_idx) != 2) {
			send_static_json(
				client_fd,
				"{\"error\":\"usage: get tag <monitor> <index>\"}\n");
			return;
		}
		int tag_idx = ext_tag_idx - 1;
		Monitor *m = monitor_by_name(mon_name);
		if (!m || tag_idx < 0 || tag_idx >= LENGTH(tags)) {
			send_static_json(client_fd,
							 "{\"error\":\"invalid monitor or tag index\"}\n");
			return;
		}
		uint32_t tagmask = 1 << tag_idx;
		int numclients = 0, focused_client = 0;
		bool is_active = false, is_urgent = false;
		if (tagmask & m->tagset[m->seltags])
			is_active = true;

		Client *c, *focused = focustop(m);
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m || !(c->tags & tagmask))
				continue;
			if (c == focused)
				focused_client = 1;
			if (c->isurgent)
				is_urgent = true;
			numclients++;
		}
		resp = cJSON_CreateObject();
		cJSON_AddStringToObject(resp, "monitor", m->wlr_output->name);
		cJSON_AddNumberToObject(resp, "tag_index", ext_tag_idx);
		cJSON_AddBoolToObject(resp, "is_active", is_active);
		cJSON_AddBoolToObject(resp, "is_urgent", is_urgent);
		cJSON_AddNumberToObject(resp, "client_count", numclients);
		cJSON_AddBoolToObject(resp, "focused_client", focused_client);
	} else if (strcmp(cmd, "get all-clients") == 0) {
		cJSON *arr = cJSON_CreateArray();
		Client *c;
		wl_list_for_each(c, &clients, link)
			cJSON_AddItemToArray(arr, build_client_json(c));
		resp = cJSON_CreateObject();
		cJSON_AddItemToObject(resp, "clients", arr);
	} else if (strcmp(cmd, "get all-monitors") == 0) {
		cJSON *arr = cJSON_CreateArray();
		Monitor *m;
		wl_list_for_each(m, &mons, link)
			cJSON_AddItemToArray(arr, build_monitor_json(m));
		resp = cJSON_CreateObject();
		cJSON_AddItemToObject(resp, "monitors", arr);
	} else if (strcmp(cmd, "get avk-stats") == 0) {
		resp = az_avk_stats_json();
		/*
		 * P4. The last completed tag transition, attached here rather than
		 * inside az_avk_stats_json(): the accumulator lives above the renderer
		 * (it counts frames and damage, which are the compositor's, not
		 * AVK's), and az_avk.h is included before it. One reader, one place.
		 */
		if (resp != NULL) {
			cJSON *tc = cJSON_CreateObject();
			if (tc != NULL) {
				cJSON_AddNumberToObject(tc, "transitions",
					(double)az_tag_cost.transitions);
				cJSON_AddBoolToObject(tc, "valid", az_tag_cost.last.valid);
				if (az_tag_cost.last.valid) {
					cJSON_AddNumberToObject(tc, "duration_ms",
						(double)az_tag_cost.last.duration_ns / 1.0e6);
					cJSON_AddNumberToObject(tc, "frames",
						(double)az_tag_cost.last.frames);
					cJSON_AddNumberToObject(tc, "committed",
						(double)az_tag_cost.last.committed);
					cJSON_AddNumberToObject(tc, "blur_prefix_px",
						(double)az_tag_cost.last.blur_prefix_px);
					cJSON_AddNumberToObject(tc, "blur_rebuilds",
						(double)az_tag_cost.last.blur_rebuilds);
					cJSON_AddNumberToObject(tc, "blur_hits",
						(double)az_tag_cost.last.blur_hits);
					cJSON_AddNumberToObject(tc, "damage_px",
						(double)az_tag_cost.last.damage_px);
					cJSON_AddNumberToObject(tc, "p50_ms",
						az_tag_cost.last.p50_ms);
					cJSON_AddNumberToObject(tc, "p95_ms",
						az_tag_cost.last.p95_ms);
					cJSON_AddNumberToObject(tc, "samples",
						(double)az_tag_cost.last.nsamples);
					cJSON_AddBoolToObject(tc, "truncated",
						az_tag_cost.last.truncated);
				}
				cJSON_AddItemToObject(resp, "tag_cost", tc);
			}
		}
	} else if (strcmp(cmd, "get surface-intent") == 0) {
		/*
		 * ── M11. THE ONE AUTHORITATIVE ANSWER TO "WHAT IS THIS WINDOW?" ────
		 *
		 * Deliberately NOT folded into `get all-clients`: that is a window-
		 * manager view (geometry, tags, focus) consumed by the bar on every
		 * arrange, and colour/presentation state has a different audience and a
		 * different cost. Deliberately NOT folded into `avk-stats` either --
		 * those are the renderer's aggregate counters, and these are per-surface
		 * facts that are not counters at all.
		 *
		 * LAYER SURFACES ARE INCLUDED, and that is not completeness for its own
		 * sake: asteroidz-bar's own wallpaper backdrop is a layer-shell wp-cm
		 * client rendering HDR10, and it was the surface that proved the
		 * preferred-description defect M6B fixed. A dump that showed only
		 * toplevels would have been blind to it.
		 */
		resp = cJSON_CreateObject();
		cJSON *arr = cJSON_AddArrayToObject(resp, "surfaces");
		Client *ic;
		wl_list_for_each(ic, &clients, link) {
			struct wlr_surface *s = client_surface(ic);
			if (s == NULL)
				continue;
			/*
			 * ── SUBSURFACES TOO, OR THIS LIES ABOUT WHOLE CLIENTS ─────────
			 *
			 * This used to report a client's toplevel surface and stop, and
			 * for a client whose content is a SUBSURFACE that is not a
			 * partial answer, it is the wrong one.
			 *
			 * Firefox is exactly that client: its xdg_toplevel carries only
			 * the client-side decoration frame, drawn on the CPU into wl_shm,
			 * while every frame of actual page content goes to a dma-buf
			 * subsurface underneath it. Reporting the toplevel alone made
			 * Firefox read as a pure wl_shm client -- 5128x2734 of it -- and
			 * a whole investigation into "why does Firefox not use dma-buf"
			 * followed from a line of output that was never true. A protocol
			 * trace showed 183 dma-buf attaches on the subsurface the
			 * inspector did not know existed.
			 *
			 * wlr_surface_for_each_surface() walks the surface and its
			 * subsurface tree in the order they compose.
			 */
			struct intent_walk walk = {
				.arr = arr,
				.name = client_get_title(ic),
				.client = ic,
				.toplevel_role = client_is_x11(ic) ? "xwayland" : "toplevel",
			};
			wlr_surface_for_each_surface(s, intent_add_surface, &walk);
		}
		Monitor *im;
		wl_list_for_each(im, &mons, link) {
			for (size_t li = 0; li < LENGTH(im->layers); li++) {
				LayerSurface *l;
				wl_list_for_each(l, &im->layers[li], link) {
					if (l->layer_surface == NULL
							|| l->layer_surface->surface == NULL)
						continue;
					/* NULL: a layer surface is not a Client and has no
					 * presentation class -- window rules match app-id and
					 * title, and it has neither. */
					cJSON_AddItemToArray(arr, build_surface_intent_json(
						l->layer_surface->surface, "layer",
						l->layer_surface->namespace != NULL
							? l->layer_surface->namespace : "", NULL));
				}
			}
		}
		/*
		 * THE OUTPUT HALF. The same numbers the surfaces above were resolved
		 * against -- reported once here rather than repeated per surface, so a
		 * disagreement between "what the output is" and "what a surface was
		 * told" is visible by comparison instead of by inference.
		 */
		cJSON *outs = cJSON_AddArrayToObject(resp, "outputs");
		Monitor *om;
		wl_list_for_each(om, &mons, link) {
			if (om->wlr_output == NULL)
				continue;
			cJSON *e = cJSON_CreateObject();
			cJSON_AddStringToObject(e, "name", om->wlr_output->name);
			cJSON_AddBoolToObject(e, "enabled", om->wlr_output->enabled);
			cJSON_AddBoolToObject(e, "hdr", om->hdr > 0);
			/*
			 * ── A PROFILE THAT IS LOADED IS NOT NECESSARILY APPLIED ────────
			 *
			 * M6B/D3: on an HDR output the profile is deliberately INERT. The
			 * connector carries its own image description, so applying an SDR
			 * characterisation on top would put two transforms on one pixel.
			 *
			 * Reporting only `icc: true` next to `encode_transfer: pq` -- which
			 * is what the first live run did -- makes a reader infer the
			 * profile is in the pipeline. Requiring that inference is the exact
			 * failure this inspector exists to remove, so the applied/not
			 * distinction and its reason are stated.
			 */
			bool icc_present = om->icc_transform != NULL;
			bool icc_applied = om->color_state.encode_tf == AZ_TF_LUT1D
				|| om->color_state.encode_tf == AZ_TF_CLUT3D;
			cJSON_AddBoolToObject(e, "icc", icc_present);
			cJSON_AddBoolToObject(e, "icc_applied", icc_applied);
			if (icc_present && !icc_applied) {
				cJSON_AddStringToObject(e, "icc_why",
					om->hdr > 0
						? "inert: the output presents its own HDR image "
						  "description, and two transforms on one pixel is "
						  "worse than none (M6B/D3)"
						: "loaded but not carried by the encode pass");
			}
			cJSON_AddStringToObject(e, "path",
				az_output_path_name(om->color_state.path));
			cJSON_AddStringToObject(e, "encode_transfer",
				az_tf_name(om->color_state.encode_tf));
			cJSON_AddNumberToObject(e, "ref_nits", om->color_state.ref_nits);
			cJSON_AddNumberToObject(e, "peak_scene",
				om->color_state.peak_scene);
			cJSON_AddNumberToObject(e, "dither_q", om->color_state.dither_q);
			cJSON_AddStringToObject(e, "present_regime",
				az_present_regime_name(om->presenter.regime));
			cJSON_AddNumberToObject(e, "nominal_period_ns",
				(double)om->presenter.nominal_period_ns);
			cJSON_AddNumberToObject(e, "period_ns",
				(double)az_presenter_period_ns(om));
			/* Signed, and a mean over a count that may be 0 -- both reported,
			 * because a mean with no samples is not a small error. */
			cJSON_AddNumberToObject(e, "err_count",
				(double)om->presenter.err_count);
			cJSON_AddNumberToObject(e, "err_mean_ns",
				om->presenter.err_count > 0
					? (double)(om->presenter.err_sum_ns
						/ (int64_t)om->presenter.err_count) : 0);
			cJSON_AddNumberToObject(e, "misses", (double)om->presenter.misses);
			cJSON_AddNumberToObject(e, "prediction_exceeded",
				(double)om->presenter.prediction_exceeded);
			/* M13B: what this OUTPUT did last frame, and how often it has
			 * skipped composition entirely. */
			cJSON_AddStringToObject(e, "scanout_last",
				az_scanout_verdict_name(
					(enum az_scanout_verdict)om->scanout_verdict));
			cJSON_AddNumberToObject(e, "scanout_frames",
				(double)om->scanout_frames);
			cJSON_AddNumberToObject(e, "scanout_changes",
				(double)om->scanout_changes);
			/* What the torn-flip path did, per outcome. `tear_busy_synced` is
			 * the one no test can predict: the state was tearable and the
			 * previous flip simply had not finished, so the frame went out on
			 * the vblank instead of being dropped. */
			cJSON_AddNumberToObject(e, "tear_torn", (double)om->tear_torn);
			cJSON_AddNumberToObject(e, "tear_test_refused",
				(double)om->tear_test_refused);
			cJSON_AddNumberToObject(e, "tear_busy_synced",
				(double)om->tear_busy_synced);
			cJSON_AddNumberToObject(e, "tear_dropped",
				(double)om->tear_dropped);
			cJSON_AddNumberToObject(e, "tear_backoff",
				(double)om->tear_backoff);
			cJSON_AddItemToArray(outs, e);
		}
	} else if (strcmp(cmd, "get presentation") == 0) {
		/*
		 * M6A.1. WHAT PRESENTATION ACTUALLY DID, per output.
		 *
		 * Separate from avk-stats because it is not the renderer's: these are
		 * facts about the display's page flips, and the two answer different
		 * questions. Observed only -- nothing here is a prediction yet.
		 */
		resp = cJSON_CreateObject();
		/* I1: the sampled instant must BE the armed target, every pass. A
		 * non-zero count means something handed animation a different clock --
		 * which is what AZ_BREAK_PRESENT_SAMPLE_NOW does on purpose. */
		cJSON_AddNumberToObject(resp, "sample_passes", (double)az_sample_total);
		cJSON_AddNumberToObject(resp, "sample_not_target",
			(double)az_sample_not_target);
		cJSON *arr = cJSON_AddArrayToObject(resp, "outputs");
		Monitor *pm;
		wl_list_for_each(pm, &mons, link) {
			if (pm->wlr_output == NULL)
				continue;
			cJSON *e = cJSON_CreateObject();
			cJSON_AddStringToObject(e, "name", pm->wlr_output->name);
			cJSON_AddNumberToObject(e, "presented", (double)pm->present_count);
			cJSON_AddNumberToObject(e, "dropped", (double)pm->present_dropped);
			cJSON_AddNumberToObject(e, "no_stamp",
				(double)pm->present_no_stamp);
			cJSON_AddNumberToObject(e, "last_seq",
				(double)pm->present_last_seq);
			/* Both, and never one alone: the nominal figure is what the mode
			 * claims and the observed one is what the display did. DP-1
			 * reports 143.999 Hz and they are not the same number. */
			cJSON_AddNumberToObject(e, "nominal_refresh_mhz",
				(double)pm->wlr_output->refresh);
			/* Per VBLANK, from the sequence delta -- not the gap between
			 * presented frames, which on a damage-driven compositor is an
			 * arrival rate and not a display period. */
			cJSON_AddNumberToObject(e, "observed_interval_us",
				(double)pm->present_interval_ns / 1000.0);
			cJSON_AddNumberToObject(e, "interval_rejected",
				(double)pm->present_interval_rejected);
			/* False means the backend does not count vblanks, and every
			 * sequence-derived figure below is therefore 0 BY ABSENCE rather
			 * than by measurement. The headless backend is such a backend. */
			cJSON_AddBoolToObject(e, "seq_available",
				pm->present_seq_available);
			cJSON *cad = cJSON_AddObjectToObject(e, "cadence");
			cJSON_AddNumberToObject(cad, "x1", (double)pm->present_cadence_1x);
			cJSON_AddNumberToObject(cad, "x2", (double)pm->present_cadence_2x);
			cJSON_AddNumberToObject(cad, "x3plus",
				(double)pm->present_cadence_3x);
			cJSON_AddStringToObject(e, "stamp_clock",
				pm->present_clock == PRESENT_CLOCK_MONOTONIC  ? "monotonic"
				: pm->present_clock == PRESENT_CLOCK_REALTIME ? "realtime"
				: pm->present_clock == PRESENT_CLOCK_NEITHER  ? "neither"
				                                              : "unknown");
			cJSON_AddNumberToObject(e, "stamp_skew_monotonic_us",
				(double)pm->present_skew_mono_ns / 1000.0);
			cJSON_AddNumberToObject(e, "stamp_skew_realtime_us",
				(double)pm->present_skew_real_ns / 1000.0);
			/* The hardware's own next-refresh guess, which under VRR is the
			 * only period the display itself states. */
			cJSON_AddNumberToObject(e, "hw_next_refresh_us",
				(double)pm->present_hw_refresh_ns / 1000.0);
			/* M-8: what ADR-605's t_pipe is seeded from. Both intervals,
			 * because they answer different questions -- arm-to-photons is
			 * what a predictor must add to `now`, commit-to-photons is how
			 * much of it is outside the compositor's control. */
			cJSON *lat = cJSON_AddObjectToObject(e, "latency");
			cJSON_AddNumberToObject(lat, "samples", (double)pm->m8_samples);
			cJSON_AddNumberToObject(lat, "unmatched",
				(double)pm->m8_unmatched);
			if (pm->m8_samples) {
				cJSON_AddNumberToObject(lat, "arm_to_present_mean_us",
					(double)pm->m8_arm_sum_ns / (double)pm->m8_samples / 1e3);
				cJSON_AddNumberToObject(lat, "arm_to_present_min_us",
					(double)pm->m8_arm_min_ns / 1e3);
				cJSON_AddNumberToObject(lat, "arm_to_present_max_us",
					(double)pm->m8_arm_max_ns / 1e3);
				cJSON_AddNumberToObject(lat, "commit_to_present_mean_us",
					(double)pm->m8_commit_sum_ns / (double)pm->m8_samples
						/ 1e3);
				cJSON_AddNumberToObject(lat, "commit_to_present_min_us",
					(double)pm->m8_commit_min_ns / 1e3);
				cJSON_AddNumberToObject(lat, "commit_to_present_max_us",
					(double)pm->m8_commit_max_ns / 1e3);
				/*
				 * Percentiles off the histogram. p10 is the one that matters:
				 * it is commit-to-photons with the display READY, which is
				 * what ADR-605's t_pipe wants -- the queueing case is already
				 * carried by the predictor's P_min floor, so a mean would
				 * count the wait twice. p50/p95 are here so the shape can be
				 * seen rather than inferred from three numbers.
				 */
				static const int want[3] = {10, 50, 95};
				static const char *names[3] = {
					"commit_to_present_p10_us",
					"commit_to_present_p50_us",
					"commit_to_present_p95_us",
				};
				for (int w = 0; w < 3; w++) {
					uint64_t need = pm->m8_samples * (uint64_t)want[w] / 100;
					uint64_t acc = 0;
					int b = 0;
					for (; b < AZ_M8_BUCKETS; b++) {
						acc += pm->m8_hist[b];
						if (acc > need)
							break;
					}
					/* The bucket's upper edge, so the figure is an inclusive
					 * bound rather than a midpoint that no sample had. */
					cJSON_AddNumberToObject(lat, names[w],
						(double)((b + 1) * (int)(AZ_M8_BUCKET_NS / 1000)));
				}
			}
			/*
			 * ADR-601/605. The PRESENTER's view, kept beside the raw
			 * observations rather than replacing them: the counters above say
			 * what the display did, and this says whether the prediction was
			 * any good. Conflating them would leave no way to tell a bad
			 * predictor from a busy display.
			 */
			const struct az_presenter *ps = &pm->presenter;
			cJSON *pr = cJSON_AddObjectToObject(e, "presenter");
			cJSON_AddNumberToObject(pr, "epoch", (double)ps->epoch);
			cJSON_AddStringToObject(pr, "regime",
				az_present_regime_name(ps->regime));
			cJSON_AddStringToObject(pr, "sync",
				ps->sync == AZ_PRESENT_SYNCED ? "synced" : "unsynced");
			cJSON_AddStringToObject(pr, "clock",
				ps->clock == AZ_PRESENT_CLOCK_MONOTONIC ? "monotonic"
				: ps->clock == AZ_PRESENT_CLOCK_FOREIGN ? "foreign"
				                                        : "unknown");
			cJSON_AddNumberToObject(pr, "nominal_period_us",
				(double)ps->nominal_period_ns / 1e3);
			cJSON_AddNumberToObject(pr, "period_us",
				(double)az_presenter_period_ns(pm) / 1e3);
			cJSON_AddNumberToObject(pr, "t_pipe_us",
				(double)ps->t_pipe_ns / 1e3);
			/* The instant THIS output's current pass is aiming at. Two outputs
			 * animating one window aim at different instants, and the gap
			 * between them times the window's speed is how far apart
			 * per-output evaluation would place it on each screen -- the
			 * quantity that decides whether enforcing ADR-611 is a correctness
			 * nicety or a visible seam. */
			cJSON_AddNumberToObject(pr, "armed_target_us",
				(double)ps->last_target_ns / 1e3);
			cJSON_AddNumberToObject(pr, "last_present_us",
				(double)ps->last_present_ns / 1e3);
			cJSON_AddNumberToObject(pr, "accepted",
				(double)ps->presents_accepted);
			cJSON_AddNumberToObject(pr, "discarded_pre_epoch",
				(double)ps->presents_discarded_epoch);
			/*
			 * THE ERROR SERIES. Signed: positive means the frame lit up LATER
			 * than predicted. mean and mean_abs are both here because a
			 * predictor that is early half the time and late half the time has
			 * a mean near zero and is not thereby good.
			 */
			/*
			 * ADR-609. Misses by what the timestamps could PROVE, and an
			 * UNKNOWN that is used honestly rather than as a shrug.
			 *
			 * gpu_ts_available is false until VK_EXT_calibrated_timestamps is
			 * wired, and while it is false GPU_LATE and PRESENTATION_SCHEDULING
			 * are structurally unreachable -- so a series that is entirely
			 * UNKNOWN is a stated limitation and not a mystery. Reporting the
			 * flag beside the counters is what makes the difference readable.
			 */
			cJSON *ms = cJSON_AddObjectToObject(pr, "misses");
			cJSON_AddNumberToObject(ms, "total", (double)ps->misses);
			/* NOT a miss, and the naming is the point: prediction spread that
			 * crossed the tolerance. On VRR this is the whole of what the old
			 * rule was counting. See az_presenter.prediction_exceeded. */
			cJSON_AddNumberToObject(ms, "prediction_exceeded",
				(double)ps->prediction_exceeded);
			cJSON_AddBoolToObject(ms, "gpu_ts_available", false);
			for (int v = AZ_MISS_CPU_LATE; v < AZ_MISS_COUNT; v++) {
				cJSON_AddNumberToObject(ms,
					az_present_verdict_name((enum az_present_verdict)v),
					(double)ps->verdicts[v]);
			}
			cJSON *er = cJSON_AddObjectToObject(pr, "error");
			cJSON_AddNumberToObject(er, "count", (double)ps->err_count);
			if (ps->err_count) {
				cJSON_AddNumberToObject(er, "mean_us",
					(double)ps->err_sum_ns / (double)ps->err_count / 1e3);
				cJSON_AddNumberToObject(er, "mean_abs_us",
					(double)ps->err_abs_sum_ns / (double)ps->err_count / 1e3);
				cJSON_AddNumberToObject(er, "min_us",
					(double)ps->err_min_ns / 1e3);
				cJSON_AddNumberToObject(er, "max_us",
					(double)ps->err_max_ns / 1e3);
			}
			cJSON_AddItemToArray(arr, e);
		}
	} else if (strcmp(cmd, "get dmabuf-feedback") == 0) {
		resp = az_dmabuf_feedback_json();
	} else if (strcmp(cmd, "get cm-stats") == 0) {
		/*
		 * ── THE COLOUR-MANAGEMENT SEND COUNTERS ──────────────────────────
		 *
		 * Both protocol frontends already counted what they emitted and
		 * NOTHING READ EITHER NUMBER: az_wpcm_preferred_sends and
		 * frog_metadata_sends were incremented and never referenced again.
		 * A counter with no reader cannot answer the question it was written
		 * for, which is the one an oracle actually needs -- "was this client
		 * told, or is it reporting the state it assumed at startup?" Those are
		 * the same picture and unrelated defects, and only a send count tells
		 * them apart from outside the compositor.
		 *
		 * `content_metadata_arms` is the third and is a COST counter rather
		 * than a coverage one: it counts how many times a client changing its
		 * declared HDR10 metadata armed a connector update, each of which is a
		 * blocking modeset. Unchanged metadata re-committed on every frame
		 * must leave it flat; that is the entire safety argument for arming
		 * that path at all, and it is checkable here rather than only
		 * reasoned about.
		 */
		resp = cJSON_CreateObject();
		cJSON_AddNumberToObject(resp, "wpcm_preferred_sends",
								(double)az_wpcm_preferred_sends);
		cJSON_AddNumberToObject(resp, "frog_metadata_sends",
								(double)frog_metadata_sends);
		cJSON_AddNumberToObject(resp, "content_metadata_arms",
								(double)az_content_metadata_arms);
		/* Which implementation is answering, so a reader cannot mistake a
		 * zero for "native wp-cm is not the one running". */
		cJSON_AddBoolToObject(resp, "wpcm_native", az_wpcm_global != NULL);
	} else if (strcmp(cmd, "get all-tags") == 0) {
		resp = build_all_tags_response();
	} else if (strcmp(cmd, "get bar-config") == 0) {
		resp = build_bar_config_response();
	} else if (strcmp(cmd, "get config-schema") == 0) {
		resp = build_config_schema_response(NULL);
	} else if (strncmp(cmd, "get config-schema ", 18) == 0) {
		resp = build_config_schema_response(cmd + 18);
	} else if (strcmp(cmd, "get config-schema-digest") == 0) {
		resp = build_config_schema_digest_response();
	} else if (strcmp(cmd, "get config") == 0) {
		resp = build_config_response(NULL);
	} else if (strncmp(cmd, "get config ", 11) == 0) {
		resp = build_config_response(cmd + 11);
	} else if (strcmp(cmd, "get dispatch-actions") == 0) {
		resp = build_dispatch_actions_response();
	} else if (strcmp(cmd, "get window-rule-schema") == 0) {
		resp = build_rule_schema_response();
	} else if (strcmp(cmd, "get window-rules") == 0) {
		resp = build_window_rules_response();
	} else if (strcmp(cmd, "get tag-rule-schema") == 0) {
		resp = build_tag_rule_schema_response();
	} else if (strcmp(cmd, "get tag-rules") == 0) {
		resp = build_tag_rules_response();
	} else if (strcmp(cmd, "get binds") == 0) {
		resp = build_binds_response();
	} else if (strcmp(cmd, "capture-chord") == 0) {
		/* No reply now. The next key press is the reply. */
		if (chord_capture.active) {
			send_static_json(client_fd,
							 "{\"ok\":false,\"error\":\"busy\","
							 "\"detail\":\"another client is capturing\"}\n");
			return;
		}
		chord_capture.active = true;
		chord_capture.client = ipc_serving;
		chord_capture.fd = client_fd;
		if (ipc_serving)
			ipc_serving->deferred = true;
		return;
	} else if (strncmp(cmd, "get tags ", 9) == 0) {
		Monitor *m = monitor_by_name(cmd + 9);
		if (!m) {
			send_static_json(client_fd, "{\"error\":\"monitor not found\"}\n");
			return;
		}
		resp = build_monitor_tags_response(m);
	} else if (strncmp(cmd_raw, "set-tag-rules ", 14) == 0) {
		resp = handle_set_tag_rules(cmd_raw + 14);
	} else if (strncmp(cmd_raw, "set-window-rules ", 17) == 0) {
		resp = handle_set_window_rules(cmd_raw + 17);
	} else if (strncmp(cmd_raw, "set-binds ", 10) == 0) {
		resp = handle_set_binds(cmd_raw + 10);
	} else if (strncmp(cmd_raw, "set-config ", 11) == 0) {
		/* cmd_raw, not cmd: the copy has had every comma turned into a space,
		 * and this body is JSON. */
		resp = handle_set_config(cmd_raw + 11);
	} else if (strncmp(cmd, "dispatch ", 9) == 0) {
		char *dispatch_copy = strdup(cmd_raw + 9);
		char *out = dispatch_copy, *ptr = dispatch_copy;
		int client_id = -1;
		while (*ptr) {
			while (*ptr == ' ' || *ptr == '\t')
				*out++ = *ptr++;
			if (strncmp(ptr, "client,", 7) == 0) {
				char *end;
				long id = strtol(ptr + 7, &end, 10);
				if (id > 0 && end > ptr + 7 && (*end == '\0' || *end == ',')) {
					client_id = (int)id;
					ptr = end;
					if (*ptr == ',')
						ptr++;
					continue;
				}
			}
			*out++ = *ptr++;
		}
		*out = '\0';

		char *tokens[6] = {NULL};
		int token_count = 0;
		char *saveptr;
		char *token = strtok_r(dispatch_copy, ",", &saveptr);
		while (token && token_count < 6) {
			while (*token == ' ' || *token == '\t')
				token++;
			char *end = token + strlen(token) - 1;
			while (end >= token && (*end == ' ' || *end == '\t'))
				*end-- = '\0';
			tokens[token_count++] = token;
			token = strtok_r(NULL, ",", &saveptr);
		}

		Arg arg = {0};
		int32_t (*func)(const Arg *) = parse_func_name(
			token_count > 0 ? tokens[0] : "", &arg,
			token_count > 1 ? tokens[1] : "", token_count > 2 ? tokens[2] : "",
			token_count > 3 ? tokens[3] : "", token_count > 4 ? tokens[4] : "",
			token_count > 5 ? tokens[5] : "");

		if (func && client_id > 0)
			arg.tc = client_by_id((uint32_t)client_id);

		if (func) {
			/* NOTE: "success" means the dispatch NAME parsed and the function
			 * was called -- not that it did anything. The return value is
			 * discarded here, so `set_output_mode` refusing an unsupported
			 * mode, or `toggle_hdr` being overridden by `hdr-mode`, both
			 * answer success:true.
			 *
			 * Not fixed in place because the return conventions differ across
			 * dispatches -- plenty return 0 unconditionally, so reporting
			 * `ret != 0` would report failure for most of the working ones and
			 * break every consumer at once. It needs an audit of all of them
			 * first; until then, the log is the honest channel. */
			func(&arg);
			send_static_json(client_fd, "{\"success\":true}\n");
		} else {
			send_static_json(client_fd, "{\"error\":\"unknown function\"}\n");
		}

		if (arg.v)
			free(arg.v);
		if (arg.v2)
			free(arg.v2);
		if (arg.v3)
			free(arg.v3);
		free(dispatch_copy);
		return; // Fast path exit
	} else {
		send_static_json(client_fd, "{\"error\":\"unknown command\"}\n");
		return;
	}

	if (resp) {
		json_str = cJSON_PrintUnformatted(resp);
		if (json_str) {
			size_t len = strlen(json_str);
			char *msg = malloc(len + 2);
			if (msg) {
				snprintf(msg, len + 2, "%s\n", json_str);
				if (ipc_serving)
					ipc_out_append(&ipc_serving->out, msg, len + 1);
				free(msg);
			}
			free(json_str);
		}
		cJSON_Delete(resp);
	}
	(void)client_fd;
}

/* ---------- watch mode support ---------- */

/* Queue one already-newline-terminated message on a watcher, dropping it if
 * the write fails outright or its backlog has run away. */
static void ipc_watch_send(struct ipc_watch_client *wc, const char *msg,
						   size_t len) {
	if (!ipc_out_append(&wc->out, msg, len) ||
		!ipc_out_flush(wc->fd, &wc->out)) {
		ipc_remove_watch_client(wc);
		return;
	}
	wl_event_source_fd_update(wc->source,
							  WL_EVENT_READABLE | WL_EVENT_HANGUP |
								  WL_EVENT_ERROR |
								  (ipc_out_pending(&wc->out) ? WL_EVENT_WRITABLE
															 : 0));
}

static void ipc_notify_json_to_fd(int fd, cJSON *json) {
	struct ipc_watch_client *wc, *tmp, *found = NULL;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->fd == fd) {
			found = wc;
			break;
		}
	}
	if (!found)
		return;
	char *str = cJSON_PrintUnformatted(json);
	if (!str)
		return;
	size_t len = strlen(str);
	char *msg = malloc(len + 2);
	if (!msg) {
		free(str);
		return;
	}
	snprintf(msg, len + 2, "%s\n", str);
	ipc_watch_send(found, msg, len + 1);
	free(msg);
	free(str);
}

static void ipc_remove_watch_client(struct ipc_watch_client *wc) {
	wl_list_remove(&wc->link);
	wl_event_source_remove(wc->source);
	close(wc->fd);
	ipc_out_reset(&wc->out);
	free(wc);
}

static int ipc_watch_data_handler(int fd, uint32_t mask, void *data) {
	struct ipc_watch_client *wc = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		ipc_remove_watch_client(wc);
		return 0;
	}
	if (mask & WL_EVENT_WRITABLE) {
		if (!ipc_out_flush(fd, &wc->out)) {
			ipc_remove_watch_client(wc);
			return 0;
		}
		if (!ipc_out_pending(&wc->out))
			wl_event_source_fd_update(wc->source, WL_EVENT_READABLE |
													  WL_EVENT_HANGUP |
													  WL_EVENT_ERROR);
	}
	if (mask & WL_EVENT_READABLE) {
		char buf[64];
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			ipc_remove_watch_client(wc);
		}
	}
	return 0;
}

static bool handle_watch_command(int fd, const char *cmd,
								 struct ipc_client_state *client) {
	enum ipc_watch_type type = IPC_WATCH_NONE;
	const char *arg = NULL;
	uint32_t client_id = 0;

	if (strncmp(cmd, "watch monitor ", 14) == 0) {
		type = IPC_WATCH_MONITOR;
		arg = cmd + 14;
	} else if (strcmp(cmd, "watch focused-client") == 0) {
		type = IPC_WATCH_FOCUSED_CLIENT;
	} else if (strncmp(cmd, "watch client ", 13) == 0) {
		type = IPC_WATCH_CLIENT;
		client_id = (uint32_t)atoi(cmd + 13);
	} else if (strncmp(cmd, "watch tags ", 11) == 0) {
		type = IPC_WATCH_TAGS;
		arg = cmd + 11;
	} else if (strcmp(cmd, "watch all-monitors") == 0) {
		type = IPC_WATCH_ALL_MONITORS;
	} else if (strcmp(cmd, "watch all-tags") == 0) {
		type = IPC_WATCH_ALL_TAGS;
	} else if (strcmp(cmd, "watch all-clients") == 0) {
		type = IPC_WATCH_ALL_CLIENTS;
	} else if (strcmp(cmd, "watch config") == 0) {
		type = IPC_WATCH_CONFIG;
	} else if (strcmp(cmd, "watch bar-config") == 0) {
		type = IPC_WATCH_BAR_CONFIG;
	} else if (strcmp(cmd, "watch idle") == 0) {
		type = IPC_WATCH_IDLE;
	} else if (strcmp(cmd, "watch keymode") == 0) {
		type = IPC_WATCH_KEYMODE;
	} else if (strcmp(cmd, "watch keyboardlayout") == 0) {
		type = IPC_WATCH_KB_LAYOUT;
	} else if (strcmp(cmd, "watch last_open_surface") == 0 ||
			   strncmp(cmd, "watch last_open_surface ", 24) == 0) {
		type = IPC_WATCH_LAST_OPEN_SURFACE;
		if (cmd[24] != '\0') { // has argument after the space
			arg = cmd + 24;
		} else {
			arg = NULL; // default to selmon
		}
	}

	if (type == IPC_WATCH_NONE)
		return false;

	struct ipc_watch_client *wc = calloc(1, sizeof(*wc));
	wc->fd = fd;
	wc->type = type;

	if ((type == IPC_WATCH_MONITOR || type == IPC_WATCH_LAST_OPEN_SURFACE) &&
		arg)
		snprintf(wc->target.monitor.name, sizeof(wc->target.monitor.name), "%s",
				 arg);
	else if (type == IPC_WATCH_TAGS && arg)
		snprintf(wc->target.tags.mon_name, sizeof(wc->target.tags.mon_name),
				 "%s", arg);
	else if (type == IPC_WATCH_CLIENT)
		wc->target.client.id = client_id;

	wl_event_source_remove(client->source);
	wc->source = wl_event_loop_add_fd(
		client->loop, fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
		ipc_watch_data_handler, wc);
	wl_list_insert(&watch_clients, &wc->link);

	/* push the initial state */
	cJSON *json = NULL;
	switch (type) {
	case IPC_WATCH_MONITOR: {
		Monitor *m = monitor_by_name(arg);
		if (m)
			json = build_monitor_json(m);
		break;
	}
	case IPC_WATCH_LAST_OPEN_SURFACE: {
		Monitor *m = NULL;
		if (arg) {
			m = monitor_by_name(arg);
		} else {
			m = selmon;
		}
		if (m) {
			json = cJSON_CreateObject();
			cJSON_AddStringToObject(json, "monitor", m->wlr_output->name);
			cJSON_AddStringToObject(json, "last_open_surface",
									m->last_open_surface);
		}
		break;
	}
	case IPC_WATCH_FOCUSED_CLIENT: {
		if (selmon && selmon->sel) {
			json = build_client_json(selmon->sel);
		} else {
			json = cJSON_CreateObject();
			cJSON_AddNullToObject(json, "id");
			cJSON_AddNullToObject(json, "title");
			cJSON_AddNullToObject(json, "appid");
		}
		break;
	}
	case IPC_WATCH_CLIENT: {
		Client *c = client_by_id(client_id);
		if (c)
			json = build_client_json(c);
		break;
	}
	case IPC_WATCH_TAGS: {
		Monitor *m = monitor_by_name(arg);
		if (m)
			json = build_monitor_tags_response(m);
		break;
	}
	case IPC_WATCH_ALL_MONITORS: {
		cJSON *arr = cJSON_CreateArray();
		Monitor *m;
		wl_list_for_each(m, &mons, link)
			cJSON_AddItemToArray(arr, build_monitor_json(m));
		json = cJSON_CreateObject();
		cJSON_AddItemToObject(json, "monitors", arr);
		break;
	}
	case IPC_WATCH_ALL_TAGS: {
		json = build_all_tags_response();
		break;
	}
	case IPC_WATCH_ALL_CLIENTS: {
		cJSON *arr = cJSON_CreateArray();
		Client *c;
		wl_list_for_each(c, &clients, link)
			cJSON_AddItemToArray(arr, build_client_json(c));
		json = cJSON_CreateObject();
		cJSON_AddItemToObject(json, "clients", arr);
		break;
	}
	case IPC_WATCH_CONFIG: {
		/* Everything, once, so the subscriber starts from a known state -- every
		 * push after this one is a diff and a client that joined mid-stream
		 * would otherwise have nothing to apply them to. */
		json = build_config_diff_response("initial", true);
		config_snapshot();
		break;
	}
	case IPC_WATCH_BAR_CONFIG: {
		json = build_bar_config_response();
		break;
	}
	case IPC_WATCH_IDLE: {
		json = build_idle_response();
		break;
	}
	case IPC_WATCH_KEYMODE: {
		json = cJSON_CreateObject();
		cJSON_AddStringToObject(json, "keymode", keymode.mode);
		break;
	}
	case IPC_WATCH_KB_LAYOUT: {
		json = cJSON_CreateObject();
		cJSON_AddStringToObject(json, "layout", ipc_get_layout_str());
		break;
	}
	default:
		break;
	}

	if (json) {
		ipc_notify_json_to_fd(fd, json);
		cJSON_Delete(json);
	}

	/* The one-shot state is discarded: the fd now belongs to the watch client,
	 * which has an output queue of its own. Nothing can be queued on this one
	 * yet, but freeing it without resetting would be a leak the moment
	 * anything ever is. */
	ipc_out_reset(&client->out);
	free(client->buf);
	free(client);
	return true;
}

/* ---------- socket event handling ---------- */
static int ipc_handle_client_data(int fd, uint32_t mask, void *data) {
	struct ipc_client_state *client = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
		goto cleanup;

	/* The rest of a reply that did not fit the socket buffer first time. */
	if (mask & WL_EVENT_WRITABLE) {
		if (!ipc_out_flush(fd, &client->out))
			goto cleanup;
		if (ipc_out_pending(&client->out))
			return 0;
		if (client->closing)
			goto cleanup;
		wl_event_source_fd_update(client->source, WL_EVENT_READABLE |
													  WL_EVENT_HANGUP |
													  WL_EVENT_ERROR);
		return 0;
	}

	if (mask & WL_EVENT_READABLE) {
		size_t available = client->buf_cap - client->buf_len;
		if (available < 4096) {
			size_t new_cap = client->buf_cap ? client->buf_cap * 2 : 8192;
			char *new_buf = realloc(client->buf, new_cap);
			if (!new_buf) {
				wlr_log(WLR_ERROR, "IPC: out of memory");
				goto cleanup;
			}
			client->buf = new_buf;
			client->buf_cap = new_cap;
			available = client->buf_cap - client->buf_len;
		}

		ssize_t n = recv(fd, client->buf + client->buf_len, available - 1, 0);
		if (n <= 0)
			goto cleanup;

		client->buf_len += n;
		client->buf[client->buf_len] = '\0';

		char *nl = memchr(client->buf, '\n', client->buf_len);
		if (!nl) {
			if (client->buf_len > 1024 * 1024)
				goto cleanup;
			return 0;
		}
		*nl = '\0';
		char *cmd = client->buf;

		bool is_watch = handle_watch_command(fd, cmd, client);
		if (is_watch)
			return 0;

		ipc_serving = client;
		client->deferred = false;
		handle_command(fd, cmd);
		ipc_serving = NULL;

		/* A handler that answers later keeps its connection. The consumed
		 * command is dropped from the buffer first, so a second line already in
		 * flight is not re-read as a fresh request. */
		if (client->deferred) {
			size_t consumed = (size_t)(nl - client->buf) + 1;
			memmove(client->buf, client->buf + consumed,
					client->buf_len - consumed);
			client->buf_len -= consumed;
			return 0;
		}

		/* The connection closes when the REPLY IS OUT, not when the handler
		 * returns. Closing here dropped whatever the socket buffer could not
		 * take -- which for everything served until now was nothing, and for
		 * a config schema is most of it. */
		if (!ipc_out_flush(fd, &client->out))
			goto cleanup;
		if (!ipc_out_pending(&client->out))
			goto cleanup;
		client->closing = true;
		wl_event_source_fd_update(client->source,
								  WL_EVENT_WRITABLE | WL_EVENT_HANGUP |
									  WL_EVENT_ERROR);
		return 0;
	}
	return 0;

cleanup:
	/* Before the struct is freed, or a chord captured afterwards would be
	 * written to a file descriptor number that has already been handed to
	 * somebody else. */
	chord_capture_cancel(client);
	close(client->fd);
	wl_event_source_remove(client->source);
	ipc_out_reset(&client->out);
	free(client->buf);
	free(client);
	return 0;
}

static int ipc_handle_connection(int fd, uint32_t mask, void *data) {
	struct wl_event_loop *loop = data;
	int client_fd = accept(fd, NULL, NULL);
	if (client_fd < 0)
		return 0;

	// set O_NONBLOCK
	int flags = fcntl(client_fd, F_GETFL, 0);
	fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
	// set FD_CLOEXEC
	flags = fcntl(client_fd, F_GETFD, 0);
	fcntl(client_fd, F_SETFD, flags | FD_CLOEXEC);

	struct ipc_client_state *client = calloc(1, sizeof(*client));
	client->fd = client_fd;
	client->loop = loop;
	client->source = wl_event_loop_add_fd(
		loop, client_fd, WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
		ipc_handle_client_data, client);
	return 0;
}

/* ---------- external notification interface ---------- */

void ipc_notify_monitor(Monitor *m) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_MONITOR &&
			strcmp(m->wlr_output->name, wc->target.monitor.name) == 0) {
			if (!json_str) {
				cJSON *json = build_monitor_json(m);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_last_surface_ws_name(Monitor *m) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type != IPC_WATCH_LAST_OPEN_SURFACE)
			continue;

		bool match = false;
		if (wc->target.monitor.name[0] == '\0') {
			if (m == selmon)
				match = true;
		} else {
			if (strcmp(m->wlr_output->name, wc->target.monitor.name) == 0)
				match = true;
		}

		if (!match)
			continue;

		if (!json_str) {
			cJSON *json = cJSON_CreateObject();
			cJSON_AddStringToObject(json, "monitor", m->wlr_output->name);
			cJSON_AddStringToObject(json, "last_open_surface",
									m->last_open_surface);
			char *raw = cJSON_PrintUnformatted(json);
			cJSON_Delete(json);
			if (!raw)
				return;
			len = strlen(raw);
			json_str = malloc(len + 2);
			snprintf(json_str, len + 2, "%s\n", raw);
			free(raw);
		}
		ipc_watch_send(wc, json_str, len + 1);
	}
	free(json_str);
}

void ipc_notify_focused_client(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_FOCUSED_CLIENT) {
			if (!json_str) {
				cJSON *json = NULL;
				if (selmon && selmon->sel) {
					json = build_client_json(selmon->sel);
				} else {
					json = cJSON_CreateObject();
					cJSON_AddNullToObject(json, "id");
					cJSON_AddNullToObject(json, "title");
					cJSON_AddNullToObject(json, "appid");
				}
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	free(json_str);
}

void ipc_notify_client(Client *c) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_CLIENT && c->id == wc->target.client.id) {
			if (!json_str) {
				cJSON *json = build_client_json(c);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_tags(Monitor *m) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_TAGS &&
			strcmp(m->wlr_output->name, wc->target.tags.mon_name) == 0) {
			if (!json_str) {
				cJSON *json = build_monitor_tags_response(m);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_all_monitors(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_ALL_MONITORS) {
			if (!json_str) {
				cJSON *arr = cJSON_CreateArray();
				Monitor *m;
				wl_list_for_each(m, &mons, link)
					cJSON_AddItemToArray(arr, build_monitor_json(m));
				cJSON *json = cJSON_CreateObject();
				cJSON_AddItemToObject(json, "monitors", arr);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_all_clients(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_ALL_CLIENTS) {
			if (!json_str) {
				cJSON *arr = cJSON_CreateArray();
				Client *c;
				wl_list_for_each(c, &clients, link)
					cJSON_AddItemToArray(arr, build_client_json(c));
				cJSON *json = cJSON_CreateObject();
				cJSON_AddItemToObject(json, "clients", arr);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_all_tags(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_ALL_TAGS) {
			if (!json_str) {
				cJSON *json = build_all_tags_response();
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

/* Called from reload_config(). A bar in another process cannot see the config
 * being re-read, and polling for it would mean either a lag between the reload
 * and the repaint or a timer that spends all day finding nothing changed. */
void ipc_notify_bar_config(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type != IPC_WATCH_BAR_CONFIG)
			continue;
		if (!json_str) {
			cJSON *json = build_bar_config_response();
			char *raw = cJSON_PrintUnformatted(json);
			cJSON_Delete(json);
			if (!raw)
				return;
			len = strlen(raw);
			json_str = malloc(len + 2);
			snprintf(json_str, len + 2, "%s\n", raw);
			free(raw);
		}
		ipc_watch_send(wc, json_str, len + 1);
	}
	if (json_str)
		free(json_str);
}

/* Push whatever changed since the last push, to anyone watching the config.
 *
 * Called from reload_config, from setoption, and from the write path. The
 * snapshot is updated whether or not anyone is listening: otherwise the first
 * subscriber after a quiet period would be diffed against a state from before
 * however many changes happened while nobody was watching, and would be told
 * about all of them as if they had just occurred.
 *
 * A no-op push is skipped rather than sent as an empty object. A reload that
 * changes nothing -- which is most of them, since matugen fires on every
 * wallpaper change and the palette often lands on the same colours -- should
 * not wake a settings panel to tell it so. */
void ipc_notify_config(const char *reason) {
	bool any = false;
	struct ipc_watch_client *wc;
	wl_list_for_each(wc, &watch_clients, link) {
		if (wc->type == IPC_WATCH_CONFIG) {
			any = true;
			break;
		}
	}
	if (!any) {
		config_snapshot();
		return;
	}

	cJSON *json = build_config_diff_response(reason, false);
	cJSON *count = cJSON_GetObjectItem(json, "count");
	if (!count || count->valueint == 0) {
		cJSON_Delete(json);
		config_snapshot();
		return;
	}
	char *raw = cJSON_PrintUnformatted(json);
	cJSON_Delete(json);
	if (!raw) {
		config_snapshot();
		return;
	}
	size_t len = strlen(raw);
	char *json_str = malloc(len + 2);
	if (json_str) {
		snprintf(json_str, len + 2, "%s\n", raw);
		struct ipc_watch_client *tmp;
		wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
			if (wc->type != IPC_WATCH_CONFIG)
				continue;
			ipc_watch_send(wc, json_str, len + 1);
		}
		free(json_str);
	}
	free(raw);
	config_snapshot();
}

void ipc_notify_idle(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type != IPC_WATCH_IDLE)
			continue;
		if (!json_str) {
			cJSON *json = build_idle_response();
			char *raw = cJSON_PrintUnformatted(json);
			cJSON_Delete(json);
			if (!raw)
				return;
			len = strlen(raw);
			json_str = malloc(len + 2);
			snprintf(json_str, len + 2, "%s\n", raw);
			free(raw);
		}
		ipc_watch_send(wc, json_str, len + 1);
	}
	free(json_str);
}

void ipc_notify_keymode(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_KEYMODE) {
			if (!json_str) {
				cJSON *json = cJSON_CreateObject();
				cJSON_AddStringToObject(json, "keymode", keymode.mode);
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

void ipc_notify_kb_layout(void) {
	char *json_str = NULL;
	size_t len = 0;
	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link) {
		if (wc->type == IPC_WATCH_KB_LAYOUT) {
			if (!json_str) {
				cJSON *json = cJSON_CreateObject();
				cJSON_AddStringToObject(json, "layout", ipc_get_layout_str());
				char *raw = cJSON_PrintUnformatted(json);
				cJSON_Delete(json);
				if (!raw)
					return;
				len = strlen(raw);
				json_str = malloc(len + 2);
				snprintf(json_str, len + 2, "%s\n", raw);
				free(raw);
			}
			ipc_watch_send(wc, json_str, len + 1);
		}
	}
	if (json_str)
		free(json_str);
}

/* ---------- init and cleanup ---------- */
static int ipc_sock_fd = -1;
static struct wl_event_source *ipc_event_source = NULL;

void ipc_init(struct wl_event_loop *event_loop) {
	wl_list_init(&watch_clients);

	const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
	if (!xdg_runtime)
		return;

	snprintf(ipc_socket_path, sizeof(ipc_socket_path), "%s/asteroidz-%d.sock",
			 xdg_runtime, getpid());

	ipc_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ipc_sock_fd < 0)
		return;

	// set FD_CLOEXEC
	int flags = fcntl(ipc_sock_fd, F_GETFD, 0);
	if (flags == -1 || fcntl(ipc_sock_fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
		wlr_log(WLR_ERROR, "failed to set FD_CLOEXEC on IPC socket");
		close(ipc_sock_fd);
		return;
	}
	// set O_NONBLOCK
	flags = fcntl(ipc_sock_fd, F_GETFL, 0);
	if (flags == -1 || fcntl(ipc_sock_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		wlr_log(WLR_ERROR, "failed to set O_NONBLOCK on IPC socket");
		close(ipc_sock_fd);
		return;
	}

	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strncpy(addr.sun_path, ipc_socket_path, sizeof(addr.sun_path) - 1);

	unlink(ipc_socket_path);
	if (bind(ipc_sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(ipc_sock_fd);
		return;
	}
	listen(ipc_sock_fd, 16);

	setenv("ASTEROIDZ_INSTANCE_SIGNATURE", ipc_socket_path, 1);

	ipc_event_source =
		wl_event_loop_add_fd(event_loop, ipc_sock_fd, WL_EVENT_READABLE,
							 ipc_handle_connection, event_loop);
}

void ipc_cleanup(void) {
	if (ipc_event_source)
		wl_event_source_remove(ipc_event_source);
	if (ipc_sock_fd >= 0)
		close(ipc_sock_fd);
	unlink(ipc_socket_path);
	unsetenv("ASTEROIDZ_INSTANCE_SIGNATURE");

	struct ipc_watch_client *wc, *tmp;
	wl_list_for_each_safe(wc, tmp, &watch_clients, link)
		ipc_remove_watch_client(wc);
}