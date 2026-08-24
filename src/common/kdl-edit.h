#ifndef ASTEROIDZ_KDL_EDIT_H
#define ASTEROIDZ_KDL_EDIT_H

/* Surgical edits to a KDL config file, as text.
 *
 * Not a KDL writer. The config is hand-maintained as well as machine-written,
 * and a round trip through a parser and back out loses every comment, every
 * choice of whitespace, and anything the parser does not model -- which for a
 * settings file someone lives in is a far worse outcome than the feature that
 * prompted the write is worth. So the functions here find the bytes that hold
 * one value, replace those, and copy the rest through untouched.
 *
 * Pure string manipulation with no compositor types, so it is unit-testable
 * on its own, which is what a change here must
 * answer to. It writes to the user's config -- it does not get to be "probably
 * right".
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Find one `output NAME { … }` block, returning its opening brace and the
 * matching close, or false if it is not there.
 *
 * Comment-aware only to the extent it needs to be: a line whose `output` is
 * preceded by anything other than whitespace is skipped, which covers `//
 * output DP-1 …` -- the form the file this writes to actually uses to explain
 * why a setting is absent. */
static bool kdl_find_output_block(const char *text, const char *name,
								  const char **out_open,
								  const char **out_close) {
	const char *p = text;
	const char *block = NULL;
	size_t namelen = strlen(name);
	while ((p = strstr(p, "output")) != NULL) {
		const char *ls = p;
		while (ls > text && ls[-1] != '\n')
			ls--;
		bool commented = false;
		for (const char *q = ls; q < p; q++) {
			if (!isspace((unsigned char)*q)) {
				commented = true;
				break;
			}
		}
		const char *after = p + 6;
		while (*after == ' ' || *after == '\t')
			after++;
		if (!commented && !strncmp(after, name, namelen) &&
			(after[namelen] == ' ' || after[namelen] == '\t' ||
			 after[namelen] == '{')) {
			block = after + namelen;
			break;
		}
		p += 6;
	}
	if (!block)
		return false;
	const char *open = strchr(block, '{');
	if (!open)
		return false;
	/* brace-match so a nested block cannot end the search early */
	int32_t depth = 0;
	const char *close = NULL;
	for (const char *q = open; *q; q++) {
		if (*q == '{')
			depth++;
		else if (*q == '}' && --depth == 0) {
			close = q;
			break;
		}
	}
	if (!close)
		return false;
	*out_open = open;
	*out_close = close;
	return true;
}

/* Set (or remove) any number of `key value` entries inside one
 * `output NAME { … }` block, returning a newly allocated document or NULL if
 * the block is not there.
 *
 * `vals[i] == NULL` REMOVES that key, which is how an ICC profile is cleared:
 * writing `icc-profile ""` would be a profile whose path is the empty string,
 * and the way back to the untransformed pipeline is for the entry not to be
 * there at all.
 *
 * Values are passed pre-formatted, so a caller decides for itself whether
 * something is `0.75`, `1920` or a quoted path. */
static char *kdl_rewrite_output_props(const char *text, const char *name,
									  const char *const *keys,
									  const char *const *vals, size_t nkeys) {
	const char *open, *close;
	if (!kdl_find_output_block(text, name, &open, &close))
		return NULL;

	size_t body_len = (size_t)(close - open - 1);
	char *body = strndup(open + 1, body_len);
	if (!body)
		return NULL;

	char out_body[4096];
	size_t o = 0;
	bool *wrote = calloc(nkeys ? nkeys : 1, sizeof(bool));
	if (!wrote) {
		free(body);
		return NULL;
	}

	const char *b = body;
	while (*b && o + 128 < sizeof(out_body)) {
		while (*b && (isspace((unsigned char)*b) || *b == ';')) {
			out_body[o++] = *b++;
			if (o + 128 >= sizeof(out_body))
				break;
		}
		if (!*b)
			break;
		const char *tok = b;
		while (*b && !isspace((unsigned char)*b) && *b != ';')
			b++;
		size_t toklen = (size_t)(b - tok);

		size_t hit = nkeys;
		for (size_t i = 0; i < nkeys; i++) {
			if (strlen(keys[i]) == toklen && !strncmp(tok, keys[i], toklen)) {
				hit = i;
				break;
			}
		}

		if (hit == nkeys) {
			if (o + toklen + 1 >= sizeof(out_body))
				break;
			memcpy(out_body + o, tok, toklen);
			o += toklen;
			continue;
		}

		/* skip whatever value the old entry carried */
		while (*b == ' ' || *b == '\t')
			b++;
		while (*b && !isspace((unsigned char)*b) && *b != ';')
			b++;

		if (vals[hit]) {
			o += (size_t)snprintf(out_body + o, sizeof(out_body) - o, "%s %s",
								  keys[hit], vals[hit]);
		} else {
			/* Removed. Give back the SPACES that led up to it and eat the
			 * separator that followed -- not the other way round, and not the
			 * separator in front of it: that one terminates the entry BEFORE
			 * this one, and swallowing it welded two entries together
			 * (`scale 1 vrr 1`, a two-argument scale). On a multi-line block
			 * the same rewind takes the indentation with it and the whole line
			 * disappears cleanly. */
			while (o > 0 && (out_body[o - 1] == ' ' || out_body[o - 1] == '\t'))
				o--;
			while (*b == ' ' || *b == '\t')
				b++;
			if (*b == ';')
				b++;
		}
		wrote[hit] = true;
	}

	/* Appending needs a SEPARATOR, not just a space.
	 *
	 * KDL ends a node at a semicolon or a newline; anything else on the line
	 * is another argument to the node already open. So appending " x 3409"
	 * after `refresh 60` does not add a position -- it turns the refresh rate
	 * into `refresh 60 x 3409`, a three-argument node, and the config stops
	 * meaning what it says. Only a block whose last entry was already
	 * terminated can take a bare append. */
	bool any_append = false;
	for (size_t i = 0; i < nkeys; i++)
		if (!wrote[i] && vals[i])
			any_append = true;
	char trailing[64] = "";
	if (any_append) {
		size_t last = o;
		while (last > 0 && isspace((unsigned char)out_body[last - 1]))
			last--;
		/* Whatever whitespace closed the block goes back on AFTER the
		 * appends, so `{ scale 1; }` gains `{ scale 1; vrr 1; }` rather than
		 * `{ scale 1;  vrr 1;}` -- the entry landing outside the space that
		 * was holding the brace off. Machine-written lines have to read like
		 * the hand-written ones around them or nobody trusts the file. */
		size_t tw = o - last;
		if (tw < sizeof(trailing)) {
			memcpy(trailing, out_body + last, tw);
			trailing[tw] = '\0';
			o = last;
		}
		bool terminated = last == 0 || out_body[last - 1] == ';' ||
						  out_body[last - 1] == '{';
		if (!terminated && o + 2 < sizeof(out_body))
			out_body[o++] = ';';
	}
	for (size_t i = 0; i < nkeys; i++) {
		if (wrote[i] || !vals[i])
			continue;
		o += (size_t)snprintf(out_body + o, sizeof(out_body) - o, " %s %s;",
							  keys[i], vals[i]);
	}
	if (trailing[0] && o + strlen(trailing) < sizeof(out_body))
		o += (size_t)snprintf(out_body + o, sizeof(out_body) - o, "%s",
							  trailing);
	out_body[o < sizeof(out_body) ? o : sizeof(out_body) - 1] = '\0';
	free(wrote);
	free(body);

	size_t head = (size_t)(open + 1 - text);
	size_t tail_len = strlen(close);
	char *doc = malloc(head + strlen(out_body) + tail_len + 1);
	if (!doc)
		return NULL;
	memcpy(doc, text, head);
	memcpy(doc + head, out_body, strlen(out_body));
	memcpy(doc + head + strlen(out_body), close, tail_len + 1);
	return doc;
}

/* Rewrite the `x` and `y` of one `output NAME { … }` block in `text`, in
 * place, returning a newly allocated document or NULL if the block is not
 * there.
 *
 * SURGICAL on purpose: it edits the two numbers and leaves every other byte
 * alone. The alternative -- regenerating the block from the Monitor -- would
 * silently drop whatever the compositor does not model back into KDL, and
 * every comment around it. The file this writes to is hand-maintained as well
 * as machine-written (the user's monitors.kdl explains in a comment why HDR is
 * absent from one output), and losing that to a monitor drag would be an
 * unforgivable trade for a feature this small. */
static char *kdl_rewrite_output_pos(const char *text, const char *name,
									 int32_t x, int32_t y) {
	char xs[32], ys[32];
	snprintf(xs, sizeof(xs), "%d", x);
	snprintf(ys, sizeof(ys), "%d", y);
	const char *keys[] = {"x", "y"};
	const char *vals[] = {xs, ys};
	return kdl_rewrite_output_props(text, name, keys, vals, 2);
}

#endif /* ASTEROIDZ_KDL_EDIT_H */
