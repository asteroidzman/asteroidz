#ifndef ASTEROIDZ_BIND_SOURCE_H
#define ASTEROIDZ_BIND_SOURCE_H

/* What each bind and each window rule was WRITTEN as, recorded while it is read.
 *
 * A keybind editor cannot work from the parsed structures, and the reason is not
 * inconvenience -- it is that the parse is lossy in exactly the places an editor
 * needs. `KeyBinding` holds a function POINTER and a resolved `Arg`:
 *
 *     focus_stack next     has become    func = focusstack, arg.i = NEXT
 *     tag_silent 3         has become    func = tagsilent,  arg.ui = 1 << 2
 *
 * There is no way back. Two dispatch names can share a function, an `Arg` is a
 * union whose meaning depends on which function reads it, and `1 << 2` is a
 * plausible value for four different fields. Reconstructing "tag_silent 3" from
 * that is guesswork, and a keybind editor that guesses rewrites the user's
 * config with something that merely behaves the same today.
 *
 * The same is true of `ConfigWinRule`: `tags 4` becomes a bitmask, a regex
 * becomes a string that no longer says whether the user wrote `^kitty$` or
 * `kitty`, and the tri-states cannot distinguish "the file said 0" from "the file
 * said nothing" once anything else has coerced them.
 *
 * So the raw strings are kept as they are read, alongside the byte span of the
 * node they came from. Editing then means replacing that span -- the same
 * parse-to-LOCATE, edit-bytes-to-MUTATE split kdl-write.h is built on, and for
 * the same reason: it never round-trips a hand-maintained config through a
 * serialiser and never loses a comment.
 *
 * `editable` is the honest part. A bind written in the legacy `bind=` line form,
 * or one of the kinds with no KDL block handler at all (axisbind, switchbind,
 * gesturebind), has no node and no span. Those are still listed -- a keybind
 * editor that pretends they do not exist is worse than one that shows them
 * greyed out -- but they cannot be rewritten, and saying so is better than
 * mangling them.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BIND_SOURCE_MAX 512
#define BIND_SOURCE_ARGS 5

typedef struct {
	/* "bind", "mousebind", "axisbind", "switchbind", "gesturebind" */
	char kind[16];
	/* The chord exactly as written -- "Super+Shift+Q" from KDL, or the
	 * "Super+Shift,Q" halves of a legacy line rejoined. What a UI shows. */
	char chord[96];
	char mods[128];
	char key[64];
	/* The dispatch name and its arguments, unresolved. `parse_func_name` turns
	 * these into a pointer and a union; these are what the file says. */
	char action[64];
	char args[BIND_SOURCE_ARGS][96];
	int32_t n_args;
	/* The keymode block it was declared in. Two binds can share a chord in
	 * different modes, so a UI listing them without this shows duplicates. */
	char mode[28];
	bool keysym, lock, release, pass;
	int32_t file; /* index into config_files[], -1 if not from a file */
	int32_t line;
	size_t span_start, span_end;
	bool editable;
} BindSource;

static BindSource bind_sources[BIND_SOURCE_MAX];
static int32_t nbind_sources;

/* How many of the five argument slots the file actually filled.
 *
 * The bind format pads every missing argument with "0", so a zero-argument
 * dispatch arrives here with five of them. Storing all five would have a keybind
 * editor offering five boxes for `kill_client` -- and, worse, writing five zeros
 * back into the config when the rule was saved. Trailing "0"s are dropped;
 * interior ones are kept, because `resize,0,20` means what it says. */
static int32_t bind_source_arity(const char *const *args, int32_t cap) {
	int32_t n = 0;
	for (int32_t i = 0; i < cap; i++)
		if (args[i] && args[i][0] && strcmp(args[i], "0"))
			n = i + 1;
	return n;
}

/* The chord node being applied right now.
 *
 * A static rather than a parameter, for the same reason config_src_ctx is one:
 * the bind branch of parse_option is reached from the KDL walker, from the
 * legacy line reader and from `dispatch set_option`, and only the first has a
 * node to offer. One hook in the branch catches all three, and the caller that
 * knows more sets this first. */
static struct {
	const KdlNode *node;
	int32_t file;
} bind_src_ctx = {.file = -1};

static void bind_source_reset(void) {
	nbind_sources = 0;
	bind_src_ctx.node = NULL;
	bind_src_ctx.file = -1;
}

/* Record a bind as it was written.
 *
 * `mods` and `key` are the halves parse_option split out; `chord` is rebuilt
 * from them when there is no node to take it from verbatim. Args are stored
 * un-trimmed of their placeholder "0"s by the caller passing n_args -- the bind
 * format pads missing arguments with "0", and storing five of them for a
 * zero-argument dispatch would have a UI offering five empty boxes. */
static void bind_source_note(const char *kind, const char *mods,
							 const char *key, const char *action,
							 const char *const *args, int32_t n_args,
							 const char *mode, const KeyBinding *kb) {
	if (nbind_sources >= BIND_SOURCE_MAX)
		return;
	BindSource *b = &bind_sources[nbind_sources++];
	memset(b, 0, sizeof(*b));
	snprintf(b->kind, sizeof(b->kind), "%s", kind);
	snprintf(b->mods, sizeof(b->mods), "%s", mods ? mods : "");
	snprintf(b->key, sizeof(b->key), "%s", key ? key : "");
	snprintf(b->action, sizeof(b->action), "%s", action ? action : "");
	snprintf(b->mode, sizeof(b->mode), "%s", mode ? mode : "");
	if (n_args > BIND_SOURCE_ARGS)
		n_args = BIND_SOURCE_ARGS;
	for (int32_t i = 0; i < n_args; i++)
		snprintf(b->args[i], sizeof(b->args[i]), "%s", args[i] ? args[i] : "");
	b->n_args = n_args;

	if (kb) {
		b->keysym = kb->keysymcode.type == KEY_TYPE_SYM;
		b->lock = kb->islockapply;
		b->release = kb->isreleaseapply;
		b->pass = kb->ispassapply;
	}

	if (bind_src_ctx.node) {
		const KdlNode *n = bind_src_ctx.node;
		snprintf(b->chord, sizeof(b->chord), "%s", n->name);
		b->file = bind_src_ctx.file;
		b->line = n->span.line;
		b->span_start = n->span.start;
		b->span_end = n->span.end;
		b->editable = true;
	} else {
		/* No node: a legacy line, or a runtime set_option. Rebuild the chord so
		 * a UI has something to show, and say it cannot be rewritten. */
		if (b->mods[0] && strcmp(b->mods, "none"))
			snprintf(b->chord, sizeof(b->chord), "%s+%s", b->mods, b->key);
		else
			snprintf(b->chord, sizeof(b->chord), "%s", b->key);
		b->file = -1;
		b->editable = false;
	}
}

/* ---------- window rules ---------- */

typedef struct {
	int32_t file;
	int32_t line;
	size_t span_start, span_end;
	bool editable;
} RuleOrigin;

static RuleOrigin rule_origins[BIND_SOURCE_MAX];
static int32_t nrule_origins;

static struct {
	const KdlNode *node;
	int32_t file;
} rule_src_ctx = {.file = -1};

static void rule_source_reset(void) {
	nrule_origins = 0;
	rule_src_ctx.node = NULL;
	rule_src_ctx.file = -1;
}

/* Indexed by position in config.window_rules[], so the two arrays are appended
 * to in lockstep. Called from the windowrule branch right after the count is
 * bumped; an early return there leaves both untouched, which is what keeps them
 * aligned when a rule fails to parse. */
static void rule_source_note(void) {
	if (nrule_origins >= BIND_SOURCE_MAX)
		return;
	RuleOrigin *o = &rule_origins[nrule_origins++];
	memset(o, 0, sizeof(*o));
	if (rule_src_ctx.node) {
		const KdlNode *n = rule_src_ctx.node;
		o->file = rule_src_ctx.file;
		o->line = n->span.line;
		o->span_start = n->span.start;
		o->span_end = n->span.end;
		o->editable = true;
	} else {
		o->file = -1;
		o->editable = false;
	}
}

static const RuleOrigin *rule_source_at(int32_t index) {
	if (index < 0 || index >= nrule_origins)
		return NULL;
	return &rule_origins[index];
}

#endif /* ASTEROIDZ_BIND_SOURCE_H */
