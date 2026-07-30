#ifndef ASTEROIDZ_KDL_WRITE_H
#define ASTEROIDZ_KDL_WRITE_H

/* Surgical edits to a KDL config file at an arbitrary nested path.
 *
 * kdl-edit.h does this for one shape -- `output NAME { … }` -- by scanning for
 * the block as text. That was right for one feature and does not generalise: a
 * settings app writes `layout/border/width`, `effects/blur/radius`,
 * `animations/window-open/duration`, and a text scan for a node called `width`
 * finds the first one anywhere in the file.
 *
 * So this module PARSES TO LOCATE AND EDITS BYTES TO MUTATE. The real parser
 * hands back a byte span per node (KdlSpan in config/kdl.h), which means the
 * writer never has to reason about comments, `/-` slashdash, `;` terminators,
 * line continuations or nesting -- the thing that already knows how to read
 * KDL does the reading. And because only the located bytes are replaced, every
 * other byte survives: the config this writes to is 460 hand-maintained lines
 * with comments explaining why individual settings are what they are, and
 * regenerating it from the parsed tree would be correct and still destroy all
 * of that.
 *
 * Pure text handling with no compositor types, so it is unit-testable on its
 * own: see tests/test-kdl-write.c, which is what a change here must answer to.
 * It writes to the user's config -- it does not get to be "probably right".
 */

#include "../config/kdl.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- locating ---------- */

/* The LAST node named `name` among `nodes`.
 *
 * Last, not first, and this is the whole reason the search exists: KDL applies
 * declarations in order and asteroidz's `source` directive is applied in place,
 * so a key set in config.kdl and again in a sourced colors.kdl is won by
 * colors.kdl. The node whose value the compositor is actually using is the last
 * one, and it is therefore the only one worth editing -- rewriting an earlier
 * declaration produces a change the user cannot see take effect. */
static const KdlNode *kdl_last_named(const KdlNode *nodes, size_t n,
									 const char *name) {
	const KdlNode *found = NULL;
	for (size_t i = 0; i < n; i++)
		if (nodes[i].name && !strcmp(nodes[i].name, name))
			found = &nodes[i];
	return found;
}

/* Walk a `a/b/c` path from the document root. `*out_depth` is how many
 * components matched, so a caller that means to CREATE the rest knows where to
 * start. */
static const KdlNode *kdl_locate_prefix(const KdlDocument *doc,
										const char *path, size_t *out_depth) {
	const KdlNode *cur = NULL;
	const KdlNode *nodes = doc->nodes;
	size_t n = doc->n_nodes;
	size_t depth = 0;

	const char *seg = path;
	while (*seg) {
		const char *slash = strchr(seg, '/');
		size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
		char comp[128];
		if (len >= sizeof(comp))
			break;
		memcpy(comp, seg, len);
		comp[len] = '\0';

		const char *hit_name = comp;
		const KdlNode *hit = kdl_last_named(nodes, n, hit_name);
		if (!hit)
			break;
		cur = hit;
		depth++;
		nodes = hit->children;
		n = hit->n_children;
		if (!slash)
			break;
		seg = slash + 1;
	}
	if (out_depth)
		*out_depth = depth;
	return cur;
}

/* The node at exactly `path`, or NULL. */
static const KdlNode *kdl_locate_path(const KdlDocument *doc,
									  const char *path) {
	size_t want = 1;
	for (const char *p = path; *p; p++)
		if (*p == '/')
			want++;
	size_t got = 0;
	const KdlNode *n = kdl_locate_prefix(doc, path, &got);
	return (n && got == want) ? n : NULL;
}

static const KdlNode *kdl_locate_key_in(const KdlNode *nodes, size_t n,
										const char *key) {
	const KdlNode *found = NULL;
	for (size_t i = 0; i < n; i++) {
		if (nodes[i].name && !strcmp(nodes[i].name, key))
			found = &nodes[i];
		const KdlNode *deeper =
			kdl_locate_key_in(nodes[i].children, nodes[i].n_children, key);
		if (deeper)
			found = deeper;
	}
	return found;
}

/* A node by its BARE NAME, at any depth, last match wins.
 *
 * The config front-end resolves a leaf by looking its full path up in the
 * option table and falling back to the node's own name (see kdl_leaf), which is
 * why `misc { border_radius 9 }` works even though the canonical spelling of
 * that key is a bare top-level `border_radius`. The user's config leans on that
 * fallback heavily, so a writer that only understood canonical paths would
 * append a second declaration at the top level instead of editing the one that
 * is actually in force -- two declarations of the same key, with the appended
 * one winning by position. */
static const KdlNode *kdl_locate_key(const KdlDocument *doc, const char *key) {
	return kdl_locate_key_in(doc->nodes, doc->n_nodes, key);
}

/* ---------- formatting helpers ---------- */

/* The document's indentation unit, so machine-written lines read like the
 * hand-written ones around them. Nobody trusts a config that suddenly mixes
 * tabs into four-space blocks. */
static void kdl_detect_indent(const char *text, char *out, size_t cap) {
	snprintf(out, cap, "\t"); /* the repo's own style, and the fallback */
	const char *p = text;
	while (*p) {
		if (*p == '\n') {
			const char *q = p + 1;
			size_t tabs = 0, spaces = 0;
			while (*q == '\t' || *q == ' ') {
				if (*q == '\t')
					tabs++;
				else
					spaces++;
				q++;
			}
			/* An indented line that actually carries a node, not a blank one
			 * and not a continuation of a comment block. */
			if ((tabs || spaces) && *q && *q != '\n' && *q != '\r' &&
				!(q[0] == '/' && (q[1] == '/' || q[1] == '*'))) {
				if (tabs && !spaces)
					snprintf(out, cap, "\t");
				else if (spaces && !tabs)
					snprintf(out, cap, "%*s", (int)(spaces > 8 ? 4 : spaces),
							 "");
				return;
			}
		}
		p++;
	}
}

/* The leading whitespace of the line `off` sits on. */
static void kdl_line_indent(const char *text, size_t off, char *out,
							size_t cap) {
	size_t ls = off;
	while (ls > 0 && text[ls - 1] != '\n')
		ls--;
	size_t n = 0;
	while (ls + n < off && (text[ls + n] == ' ' || text[ls + n] == '\t') &&
		   n + 1 < cap)
		n++;
	memcpy(out, text + ls, n);
	out[n] = '\0';
}

/* Splice `ins` over `[from, to)` of `text`. */
static char *kdl_splice(const char *text, size_t from, size_t to,
						const char *ins) {
	size_t tlen = strlen(text);
	if (from > tlen || to > tlen || from > to)
		return NULL;
	size_t ilen = strlen(ins);
	char *out = malloc(tlen - (to - from) + ilen + 1);
	if (!out)
		return NULL;
	memcpy(out, text, from);
	memcpy(out + from, ins, ilen);
	memcpy(out + from + ilen, text + to, tlen - to + 1);
	return out;
}

/* `a b c` from an argv, each element already quoted as it should appear. */
static bool kdl_join_args(const char *const *args, size_t nargs, char *out,
						  size_t cap) {
	size_t o = 0;
	for (size_t i = 0; i < nargs; i++) {
		int w = snprintf(out + o, cap - o, "%s%s", i ? " " : "", args[i]);
		if (w < 0 || (size_t)w >= cap - o)
			return false;
		o += (size_t)w;
	}
	out[o] = '\0';
	return true;
}

/* ---------- mutating ---------- */

/* Replace one node's POSITIONAL ARGUMENTS, leaving its name, its properties,
 * its children, its comment and its indentation byte for byte alone. This is
 * the common case -- one value changing -- and it must not be able to disturb
 * anything else on the line.
 *
 * The region replaced starts just after the node NAME rather than at the first
 * argument, so a node that has no arguments yet takes one by the same code
 * path: `enable` becomes `enable 1` with the separator written, instead of
 * needing a second function for "insert an argument where there was none". */
static char *kdl_edit_set_args(const char *text, const KdlNode *node,
							   const char *const *args, size_t nargs) {
	if (!node)
		return NULL;
	/* A property sitting inside the region would be replaced along with the
	 * arguments. Preserving prop placement while rewriting args around it needs
	 * a policy nobody has asked for, so refuse rather than guess. */
	if (node->span.props_start != KDL_NO_OFFSET &&
		node->span.props_start < node->span.args_end)
		return NULL;

	char joined[1024];
	if (!kdl_join_args(args, nargs, joined, sizeof(joined)))
		return NULL;
	char ins[1088];
	snprintf(ins, sizeof(ins), "%s%s", nargs ? " " : "", joined);
	return kdl_splice(text, node->span.name_end, node->span.args_end, ins);
}

/* Nest `rest` into `name { name { … } }` form, innermost carrying the args. */
static bool kdl_render_nested(const char *rest, const char *joined, char *out,
							  size_t cap) {
	size_t o = 0;
	size_t depth = 0;
	const char *seg = rest;
	while (*seg) {
		const char *slash = strchr(seg, '/');
		size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
		int w;
		if (slash)
			w = snprintf(out + o, cap - o, "%.*s { ", (int)len, seg);
		else
			w = snprintf(out + o, cap - o, "%.*s%s%s", (int)len, seg,
						 *joined ? " " : "", joined);
		if (w < 0 || (size_t)w >= cap - o)
			return false;
		o += (size_t)w;
		if (!slash)
			break;
		depth++;
		seg = slash + 1;
	}
	for (size_t i = 0; i < depth; i++) {
		int w = snprintf(out + o, cap - o, " }");
		if (w < 0 || (size_t)w >= cap - o)
			return false;
		o += (size_t)w;
	}
	out[o] = '\0';
	return true;
}

/* Set `path` to `args`, creating whatever part of the path is missing.
 *
 * Editing in place when the node exists; otherwise inserting into the deepest
 * ancestor that DOES exist, so a new `effects/blur/radius` lands inside the
 * `effects { }` block the user already has rather than as a second `effects`
 * block at the end of the file. With no ancestor at all it goes to the end of
 * the document, which is the only place a whole new top-level block can go
 * without choosing a spot in someone else's file for them.
 *
 * `doc` must be a parse of `text`. Returns a new document, or NULL. */
static char *kdl_edit_set_path(const char *text, const KdlDocument *doc,
							   const char *path, const char *const *args,
							   size_t nargs) {
	const KdlNode *exact = kdl_locate_path(doc, path);
	if (exact)
		return kdl_edit_set_args(text, exact, args, nargs);

	char joined[1024];
	if (!kdl_join_args(args, nargs, joined, sizeof(joined)))
		return NULL;

	size_t depth = 0;
	const KdlNode *anc = kdl_locate_prefix(doc, path, &depth);

	/* Skip `depth` components; what is left has to be created. */
	const char *rest = path;
	for (size_t i = 0; i < depth; i++) {
		const char *slash = strchr(rest, '/');
		if (!slash)
			return NULL; /* depth cannot exceed the component count */
		rest = slash + 1;
	}

	char unit[16];
	kdl_detect_indent(text, unit, sizeof(unit));

	char body[2048];
	if (!kdl_render_nested(rest, joined, body, sizeof(body)))
		return NULL;

	/* No ancestor, or an ancestor with no `{ }` to put anything in: append a
	 * whole top-level declaration at the end of the document. */
	if (!anc || anc->span.body_open == KDL_NO_OFFSET) {
		char full[2176];
		if (!kdl_render_nested(path, joined, full, sizeof(full)))
			return NULL;
		size_t tlen = strlen(text);
		char ins[2240];
		snprintf(ins, sizeof(ins), "%s%s\n",
				 (tlen && text[tlen - 1] == '\n') ? "" : "\n", full);
		return kdl_splice(text, tlen, tlen, ins);
	}

	/* Insert as the last entry of the ancestor's body.
	 *
	 * After the body's last non-whitespace byte, not immediately before the
	 * `}`: whatever whitespace was holding the brace off its content -- a
	 * newline and an indent, usually -- stays where it was, so the block closes
	 * the way it closed before and the new line is indented like its siblings.
	 * Inserting straight before the `}` produced `…radius 8}` on a block whose
	 * every other entry sat on its own line. */
	char outer[32];
	kdl_line_indent(text, anc->span.start, outer, sizeof(outer));

	size_t last = anc->span.body_close;
	while (last > anc->span.body_open + 1 &&
		   isspace((unsigned char)text[last - 1]))
		last--;

	bool empty_body = (last == anc->span.body_open + 1);
	char ins[2304];
	if (empty_body) {
		/* `effects { }` has no sibling to copy, so give the first entry a line
		 * of its own -- which is what the block will look like once it has more
		 * than one thing in it. */
		snprintf(ins, sizeof(ins), "\n%s%s%s", outer, unit, body);
	} else {
		bool terminated = text[last - 1] == ';' || text[last - 1] == '}';
		snprintf(ins, sizeof(ins), "%s\n%s%s%s", terminated ? "" : ";", outer,
				 unit, body);
	}
	return kdl_splice(text, last, last, ins);
}

/* Delete a node, and by default the comment lines directly above it.
 *
 * Those comments are the node's explanation; orphaning them above an unrelated
 * setting is a worse outcome than losing them with the thing they describe.
 * Only lines that are ENTIRELY a comment are taken -- a trailing `// why` on
 * a neighbouring line belongs to that neighbour. */
static char *kdl_edit_remove(const char *text, const KdlNode *node,
							 bool with_comments) {
	if (!node)
		return NULL;
	size_t from = node->span.start;
	size_t to = node->span.end;

	/* Take the terminator, and the rest of the line if nothing else is on it. */
	while (text[to] == ' ' || text[to] == '\t')
		to++;
	if (text[to] == ';')
		to++;

	/* Was this node alone on its line? Then take the line. */
	size_t ls = from;
	while (ls > 0 && text[ls - 1] != '\n')
		ls--;
	bool alone = true;
	for (size_t i = ls; i < from; i++)
		if (!isspace((unsigned char)text[i])) {
			alone = false;
			break;
		}
	if (alone) {
		size_t after = to;
		while (text[after] == ' ' || text[after] == '\t' || text[after] == '\r')
			after++;
		if (text[after] == '\n' || text[after] == '\0') {
			from = ls;
			to = text[after] == '\n' ? after + 1 : after;
		}
	}

	if (with_comments && from == ls) {
		/* Walk back over whole-line comments immediately above. */
		for (;;) {
			if (from == 0)
				break;
			size_t pls = from - 1; /* the '\n' ending the previous line */
			if (text[pls] != '\n')
				break;
			size_t pstart = pls;
			while (pstart > 0 && text[pstart - 1] != '\n')
				pstart--;
			size_t q = pstart;
			while (q < pls && (text[q] == ' ' || text[q] == '\t'))
				q++;
			if (!(q + 1 < pls && text[q] == '/' && text[q + 1] == '/'))
				break;
			from = pstart;
		}
	}

	return kdl_splice(text, from, to, "");
}

/* Append a top-level declaration at the very end of the document, with a
 * comment saying who wrote it and why.
 *
 * The escape hatch for a key whose value currently comes from a file the
 * compositor must not edit -- a matugen-generated palette, say. `source` is
 * applied in place and later declarations win, so a block appended after the
 * `source` line shadows it. Refusing to write such a key forever is a dead end
 * and writing it into the generated file is worse (it is regenerated); this
 * makes the shadowing explicit and visible in the file. */
static char *kdl_edit_append_path(const char *text, const char *path,
								  const char *const *args, size_t nargs,
								  const char *why) {
	char joined[1024];
	if (!kdl_join_args(args, nargs, joined, sizeof(joined)))
		return NULL;
	char full[2176];
	if (!kdl_render_nested(path, joined, full, sizeof(full)))
		return NULL;
	size_t tlen = strlen(text);
	char ins[2560];
	snprintf(ins, sizeof(ins), "%s%s%s%s\n",
			 (tlen && text[tlen - 1] == '\n') ? "\n" : "\n\n",
			 why ? why : "", why ? "\n" : "", full);
	return kdl_splice(text, tlen, tlen, ins);
}

#endif /* ASTEROIDZ_KDL_WRITE_H */
