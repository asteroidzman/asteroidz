/* Compact KDL (kdl.dev) parser for asteroidz config files.
 *
 * Covers the subset the config needs: nodes with positional arguments and
 * key=value properties, `{ }` children, line comments, C-style (nesting)
 * block comments, `/-` slashdash, `\`-newline continuation, `;`/newline node
 * terminators, quoted strings (with the common escapes), bare identifiers,
 * numbers, and the `true`/`false`/`null` keywords. Type annotations `(type)`
 * are parsed and ignored. Values are kept as their decoded string form
 * (numbers verbatim, bools as "true"/"false", strings unescaped); the config
 * front-end maps those onto the existing key=value option machinery.
 *
 * Not supported (unused by the config): raw strings, non-decimal number
 * bases, and unicode escapes beyond the BMP.
 */
#ifndef ASTEROIDZ_KDL_H
#define ASTEROIDZ_KDL_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

typedef enum {
	KDL_STRING,
	KDL_NUMBER,
	KDL_BOOL,
	KDL_NULL,
} KdlValueType;

typedef struct {
	char *name; /* property name, or NULL for a positional argument */
	char *value;
	KdlValueType type;
} KdlEntry;

typedef struct KdlNode {
	char *name;
	KdlEntry *args;
	size_t n_args;
	KdlEntry *props;
	size_t n_props;
	struct KdlNode *children;
	size_t n_children;
} KdlNode;

typedef struct {
	KdlNode *nodes;
	size_t n_nodes;
} KdlDocument;

typedef struct {
	const char *p;
	int line;
	char err[256];
	bool failed;
} KdlParser;

static bool kdl_parse_nodes(KdlParser *ps, KdlNode **out_nodes,
							size_t *out_count, bool in_children);

static void kdl_node_free(KdlNode *n) {
	free(n->name);
	for (size_t i = 0; i < n->n_args; i++) {
		free(n->args[i].name);
		free(n->args[i].value);
	}
	free(n->args);
	for (size_t i = 0; i < n->n_props; i++) {
		free(n->props[i].name);
		free(n->props[i].value);
	}
	free(n->props);
	for (size_t i = 0; i < n->n_children; i++)
		kdl_node_free(&n->children[i]);
	free(n->children);
}

static void kdl_free(KdlDocument *doc) {
	if (!doc || !doc->nodes)
		return;
	for (size_t i = 0; i < doc->n_nodes; i++)
		kdl_node_free(&doc->nodes[i]);
	free(doc->nodes);
	doc->nodes = NULL;
	doc->n_nodes = 0;
}

static void kdl_fail(KdlParser *ps, const char *msg) {
	if (!ps->failed) {
		snprintf(ps->err, sizeof(ps->err), "line %d: %s", ps->line, msg);
		ps->failed = true;
	}
}

static bool kdl_is_bare(char c) {
	/* KDL identifier chars: anything printable that isn't a delimiter */
	return c && !isspace((unsigned char)c) && c != '{' && c != '}' &&
		   c != '(' && c != ')' && c != '=' && c != '"' && c != ';' &&
		   c != '/' && c != '\\';
}

/* consume whitespace, comments and `\`-newline continuations. Returns true if
 * a node-terminating newline (or ';') was crossed. */
static bool kdl_skip_ws(KdlParser *ps, bool stop_at_newline) {
	bool newline = false;
	for (;;) {
		char c = *ps->p;
		if (c == '\0')
			break;
		if (c == '\n') {
			if (stop_at_newline) {
				newline = true;
				break; /* leave the newline for the next (consuming) skip */
			}
			ps->line++;
			ps->p++;
		} else if (c == ' ' || c == '\t' || c == '\r') {
			ps->p++;
		} else if (c == ';') {
			if (stop_at_newline) {
				newline = true;
				break;
			}
			ps->p++;
		} else if (c == '\\') {
			/* line continuation: backslash then (ws) newline */
			const char *q = ps->p + 1;
			while (*q == ' ' || *q == '\t' || *q == '\r')
				q++;
			if (*q == '\n') {
				ps->line++;
				ps->p = q + 1;
			} else {
				break; /* stray backslash: let caller handle */
			}
		} else if (c == '/' && ps->p[1] == '/') {
			ps->p += 2;
			while (*ps->p && *ps->p != '\n')
				ps->p++;
		} else if (c == '/' && ps->p[1] == '*') {
			ps->p += 2;
			int depth = 1;
			while (*ps->p && depth > 0) {
				if (ps->p[0] == '/' && ps->p[1] == '*') {
					depth++;
					ps->p += 2;
				} else if (ps->p[0] == '*' && ps->p[1] == '/') {
					depth--;
					ps->p += 2;
				} else {
					if (*ps->p == '\n')
						ps->line++;
					ps->p++;
				}
			}
		} else {
			break;
		}
	}
	return newline;
}

/* parse a "(type)" annotation if present; the type is discarded */
static void kdl_skip_type(KdlParser *ps) {
	if (*ps->p == '(') {
		ps->p++;
		while (*ps->p && *ps->p != ')')
			ps->p++;
		if (*ps->p == ')')
			ps->p++;
	}
}

/* parse a quoted or bare scalar into a freshly-allocated string; sets *type */
static char *kdl_parse_scalar(KdlParser *ps, KdlValueType *type) {
	kdl_skip_type(ps);
	char *buf = NULL;
	size_t len = 0, cap = 0;
#define KDL_PUSH(ch)                                                           \
	do {                                                                       \
		if (len + 1 >= cap) {                                                  \
			cap = cap ? cap * 2 : 32;                                          \
			buf = realloc(buf, cap);                                           \
		}                                                                      \
		buf[len++] = (ch);                                                     \
	} while (0)

	if (*ps->p == '"') {
		ps->p++;
		while (*ps->p && *ps->p != '"') {
			char c = *ps->p++;
			if (c == '\\') {
				char e = *ps->p++;
				switch (e) {
				case 'n': KDL_PUSH('\n'); break;
				case 't': KDL_PUSH('\t'); break;
				case 'r': KDL_PUSH('\r'); break;
				case '"': KDL_PUSH('"'); break;
				case '\\': KDL_PUSH('\\'); break;
				case '/': KDL_PUSH('/'); break;
				case 'b': KDL_PUSH('\b'); break;
				case 'f': KDL_PUSH('\f'); break;
				case 'u': {
					/* \u{XXXX} or \uXXXX -> keep simple: skip braces, emit
					 * codepoint's low byte(s) as UTF-8 for BMP */
					if (*ps->p == '{')
						ps->p++;
					unsigned cp = 0;
					while (isxdigit((unsigned char)*ps->p)) {
						char h = *ps->p++;
						cp = cp * 16 +
							 (h <= '9' ? h - '0'
									   : (tolower(h) - 'a' + 10));
					}
					if (*ps->p == '}')
						ps->p++;
					if (cp < 0x80) {
						KDL_PUSH((char)cp);
					} else if (cp < 0x800) {
						KDL_PUSH((char)(0xC0 | (cp >> 6)));
						KDL_PUSH((char)(0x80 | (cp & 0x3F)));
					} else {
						KDL_PUSH((char)(0xE0 | (cp >> 12)));
						KDL_PUSH((char)(0x80 | ((cp >> 6) & 0x3F)));
						KDL_PUSH((char)(0x80 | (cp & 0x3F)));
					}
					break;
				}
				default: KDL_PUSH(e); break;
				}
			} else {
				if (c == '\n')
					ps->line++;
				KDL_PUSH(c);
			}
		}
		if (*ps->p != '"') {
			kdl_fail(ps, "unterminated string");
			free(buf);
			return NULL;
		}
		ps->p++;
		if (type)
			*type = KDL_STRING;
	} else {
		/* bare word / number / keyword */
		while (kdl_is_bare(*ps->p))
			KDL_PUSH(*ps->p++);
		if (len == 0) {
			free(buf);
			return NULL;
		}
		buf[len] = '\0';
		if (type) {
			if (strcmp(buf, "true") == 0 || strcmp(buf, "false") == 0)
				*type = KDL_BOOL;
			else if (strcmp(buf, "null") == 0)
				*type = KDL_NULL;
			else if (isdigit((unsigned char)buf[0]) || buf[0] == '-' ||
					 buf[0] == '+')
				*type = KDL_NUMBER;
			else
				*type = KDL_STRING;
		}
	}
	KDL_PUSH('\0');
	len--; /* not counting the NUL for the return */
#undef KDL_PUSH
	return buf;
}

static void kdl_add_entry(KdlEntry **arr, size_t *n, char *name, char *value,
						  KdlValueType type) {
	*arr = realloc(*arr, (*n + 1) * sizeof(KdlEntry));
	(*arr)[*n].name = name;
	(*arr)[*n].value = value;
	(*arr)[*n].type = type;
	(*n)++;
}

/* parse a single node starting at ps->p (name already known to be present) */
static bool kdl_parse_one_node(KdlParser *ps, KdlNode *node) {
	memset(node, 0, sizeof(*node));
	kdl_skip_type(ps);
	KdlValueType t;
	node->name = kdl_parse_scalar(ps, &t);
	if (!node->name) {
		kdl_fail(ps, "expected node name");
		return false;
	}

	for (;;) {
		bool nl = kdl_skip_ws(ps, true);
		if (nl || *ps->p == '\0')
			break;
		if (*ps->p == '}')
			break;
		if (*ps->p == '{') {
			ps->p++;
			if (!kdl_parse_nodes(ps, &node->children, &node->n_children, true))
				return false;
			if (*ps->p != '}') {
				kdl_fail(ps, "expected '}'");
				return false;
			}
			ps->p++;
			break; /* children end the node */
		}
		/* slashdash: comment out the next arg/prop/children */
		if (ps->p[0] == '/' && ps->p[1] == '-') {
			ps->p += 2;
			kdl_skip_ws(ps, true);
			if (*ps->p == '{') {
				ps->p++;
				KdlNode *tmp = NULL;
				size_t tn = 0;
				kdl_parse_nodes(ps, &tmp, &tn, true);
				for (size_t i = 0; i < tn; i++)
					kdl_node_free(&tmp[i]);
				free(tmp);
				if (*ps->p == '}')
					ps->p++;
			} else {
				char *disc = kdl_parse_scalar(ps, &t);
				if (*ps->p == '=') {
					ps->p++;
					char *dv = kdl_parse_scalar(ps, &t);
					free(dv);
				}
				free(disc);
			}
			continue;
		}

		/* look ahead: is this "name=value" (property) or a bare arg? */
		const char *save = ps->p;
		int save_line = ps->line;
		char *first = kdl_parse_scalar(ps, &t);
		if (!first) {
			kdl_fail(ps, "expected argument");
			return false;
		}
		if (*ps->p == '=') {
			ps->p++;
			KdlValueType vt;
			char *val = kdl_parse_scalar(ps, &vt);
			if (!val) {
				free(first);
				kdl_fail(ps, "expected property value");
				return false;
			}
			kdl_add_entry(&node->props, &node->n_props, first, val, vt);
		} else {
			(void)save;
			(void)save_line;
			kdl_add_entry(&node->args, &node->n_args, NULL, first, t);
		}
	}
	return true;
}

static bool kdl_parse_nodes(KdlParser *ps, KdlNode **out_nodes,
							size_t *out_count, bool in_children) {
	KdlNode *nodes = NULL;
	size_t count = 0;
	for (;;) {
		kdl_skip_ws(ps, false);
		if (*ps->p == '\0')
			break;
		if (*ps->p == '}') {
			if (in_children)
				break;
			kdl_fail(ps, "unexpected '}'");
			break;
		}
		/* slashdash a whole node */
		if (ps->p[0] == '/' && ps->p[1] == '-') {
			ps->p += 2;
			kdl_skip_ws(ps, false);
			KdlNode tmp;
			if (!kdl_parse_one_node(ps, &tmp))
				break;
			kdl_node_free(&tmp);
			continue;
		}
		nodes = realloc(nodes, (count + 1) * sizeof(KdlNode));
		if (!kdl_parse_one_node(ps, &nodes[count])) {
			count++;
			break;
		}
		count++;
		if (ps->failed)
			break;
	}
	*out_nodes = nodes;
	*out_count = count;
	return !ps->failed;
}

/* Parse a whole KDL document. On failure returns false and fills err. */
static bool kdl_parse(const char *text, KdlDocument *out, char *errbuf,
					  size_t errlen) {
	KdlParser ps = {.p = text, .line = 1, .failed = false};
	ps.err[0] = '\0';
	bool ok = kdl_parse_nodes(&ps, &out->nodes, &out->n_nodes, false);
	if (!ok && errbuf)
		snprintf(errbuf, errlen, "%s", ps.err);
	if (!ok)
		kdl_free(out);
	return ok;
}

#endif /* ASTEROIDZ_KDL_H */
