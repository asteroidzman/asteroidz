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
 * on its own: see tests/test-kdl-edit.c, which is what a change here must
 * answer to. It writes to the user's config -- it does not get to be "probably
 * right".
 */

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	/* find `output <name>` followed by a brace, ignoring commented lines */
	const char *p = text;
	const char *block = NULL;
	size_t namelen = strlen(name);
	while ((p = strstr(p, "output")) != NULL) {
		/* at the start of a line (bar whitespace)? */
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
		return NULL;
	const char *open = strchr(block, '{');
	if (!open)
		return NULL;
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
		return NULL;

	/* rebuild the block body with x/y replaced (or appended if absent) */
	size_t body_len = (size_t)(close - open - 1);
	char *body = strndup(open + 1, body_len);
	if (!body)
		return NULL;

	char out_body[2048];
	size_t o = 0;
	bool wrote_x = false, wrote_y = false;
	const char *b = body;
	while (*b && o + 64 < sizeof(out_body)) {
		/* a token is `key value;` or `key;`  -- copy verbatim unless it is
		 * x or y, which are replaced with the new value */
		while (*b && (isspace((unsigned char)*b) || *b == ';')) {
			out_body[o++] = *b++;
			if (o + 64 >= sizeof(out_body))
				break;
		}
		if (!*b)
			break;
		const char *tok = b;
		while (*b && !isspace((unsigned char)*b) && *b != ';')
			b++;
		size_t toklen = (size_t)(b - tok);
		bool is_x = toklen == 1 && tok[0] == 'x';
		bool is_y = toklen == 1 && tok[0] == 'y';
		if (is_x || is_y) {
			/* skip the old value */
			while (*b == ' ' || *b == '\t')
				b++;
			while (*b && !isspace((unsigned char)*b) && *b != ';')
				b++;
			o += (size_t)snprintf(out_body + o, sizeof(out_body) - o, "%c %d",
								  is_x ? 'x' : 'y', is_x ? x : y);
			if (is_x)
				wrote_x = true;
			else
				wrote_y = true;
		} else {
			if (o + toklen + 1 >= sizeof(out_body))
				break;
			memcpy(out_body + o, tok, toklen);
			o += toklen;
		}
	}
	/* Appending needs a SEPARATOR, not just a space.
	 *
	 * KDL ends a node at a semicolon or a newline; anything else on the line
	 * is another argument to the node already open. So appending " x 3409"
	 * after `refresh 60` does not add a position -- it turns the refresh rate
	 * into `refresh 60 x 3409`, a three-argument node, and the config stops
	 * meaning what it says. Only a block whose last entry was already
	 * terminated can take a bare append. */
	if (!wrote_x || !wrote_y) {
		size_t last = o;
		while (last > 0 && isspace((unsigned char)out_body[last - 1]))
			last--;
		bool terminated = last == 0 || out_body[last - 1] == ';' ||
						  out_body[last - 1] == '{';
		if (!terminated && last + 2 < sizeof(out_body)) {
			/* right after the last real character, not after the whitespace
			 * that follows it -- `refresh 60 ; x 3409` parses, but a stray
			 * space before the semicolon is not how anyone writes it */
			o = last;
			out_body[o++] = ';';
		}
	}
	if (!wrote_x)
		o += (size_t)snprintf(out_body + o, sizeof(out_body) - o, " x %d;", x);
	if (!wrote_y)
		o += (size_t)snprintf(out_body + o, sizeof(out_body) - o, " y %d;", y);
	out_body[o < sizeof(out_body) ? o : sizeof(out_body) - 1] = '\0';
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

#endif /* ASTEROIDZ_KDL_EDIT_H */
