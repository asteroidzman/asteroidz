/* Unit tests for src/common/kdl-write.h.
 *
 * This code rewrites the user's own config file at arbitrary nested paths, on
 * behalf of a settings UI that can touch every option there is. The failure
 * mode is not "the screen looks wrong" -- it is a 460-line hand-maintained
 * config that no longer parses, or that quietly lost the comment explaining why
 * a setting is what it is. So it is tested directly, and the assertions are
 * byte-exact: "it still parses" is not enough when the whole promise is that
 * everything not being edited survives untouched.
 *
 * Build/run: meson test -C build  (or ninja -C build test)
 */
#include "../src/common/kdl-write.h"

#include <stdio.h>

static int failures;

static void check(const char *what, bool ok) {
	printf("%s %s\n", ok ? "ok    " : "FAIL  ", what);
	if (!ok)
		failures++;
}

static void check_str(const char *what, const char *got, const char *want) {
	bool ok = got && !strcmp(got, want);
	printf("%s %s\n", ok ? "ok    " : "FAIL  ", what);
	if (!ok) {
		printf("        got:  [%s]\n", got ? got : "(null)");
		printf("        want: [%s]\n", want);
		failures++;
	}
}

/* Parse, run the body, free. Every mutator needs a parse of the same text.
 *
 * Variadic because the bodies contain array initialisers: `{"0.25", "0.1"}` is
 * several macro arguments as far as the preprocessor is concerned. */
#define WITH_DOC(text, ...)                                                    \
	do {                                                                       \
		KdlDocument doc = {0};                                                 \
		char err[256] = "";                                                    \
		if (!kdl_parse((text), &doc, err, sizeof(err))) {                       \
			printf("FAIL   input did not parse: %s\n", err);                   \
			failures++;                                                        \
		} else {                                                               \
			__VA_ARGS__                                                        \
		}                                                                      \
		kdl_free(&doc);                                                        \
	} while (0)

static const char *A1(const char *v) { return v; }

int main(void) {
	/* ---------- locating ---------- */

	{
		const char *in = "layout {\n\tborder { width 3; color 0x111111ff }\n}\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "layout/border/width");
			check("locates a nested path", n && !strcmp(n->name, "width"));
			check("...and its argument came through",
				  n && n->n_args == 1 && !strcmp(n->args[0].value, "3"));
			check("a path that is not there is not invented",
				  kdl_locate_path(&doc, "layout/border/radius") == NULL);
			check("a PREFIX of a real path is not a match",
				  kdl_locate_path(&doc, "layout/border") != NULL &&
					  kdl_locate_path(&doc, "layout/border/width/extra") ==
						  NULL);
		});
	}

	/* Later declarations win at parse time, so the writer must edit the LAST
	 * one -- editing the first produces a change the user cannot see happen. */
	{
		const char *in = "layout { border { width 3 } }\n"
						 "layout { border { width 9 } }\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "layout/border/width");
			check("a duplicated path resolves to the LAST declaration",
				  n && n->n_args == 1 && !strcmp(n->args[0].value, "9"));
		});
	}

	/* The bare-name fallback the config front-end uses, and which the user's
	 * own config relies on: `misc { border_radius 9 }` where the canonical
	 * spelling is a bare top-level key. */
	{
		const char *in = "misc { border_radius 9; gappih 16 }\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_key(&doc, "border_radius");
			check("finds a key by bare name at any depth",
				  n && n->n_args == 1 && !strcmp(n->args[0].value, "9"));
			check("a key that is nowhere is not found",
				  kdl_locate_key(&doc, "nonexistent_key") == NULL);
		});
	}

	/* ---------- set_args: the common case ---------- */

	{
		const char *in = "layout {\n"
						 "\t// Border width. 0 disables it entirely.\n"
						 "\tborder { width 3; color 0x111111ff }\n"
						 "}\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "layout/border/width");
			const char *args[] = {"7"};
			char *out = kdl_edit_set_args(in, n, args, 1);
			check_str("replaces a value in a nested block, in place", out,
					  "layout {\n"
					  "\t// Border width. 0 disables it entirely.\n"
					  "\tborder { width 7; color 0x111111ff }\n"
					  "}\n");
			free(out);
		});
	}

	/* A key at a NON-canonical path is edited where it actually lives, not
	 * duplicated at the canonical one. This is the exact shape of the user's
	 * config, and getting it wrong means two declarations of one key with the
	 * appended copy silently winning. */
	{
		const char *in = "misc { border_radius 9; gappih 16 }\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_key(&doc, "border_radius");
			const char *args[] = {"12"};
			char *out = kdl_edit_set_args(in, n, args, 1);
			check_str("edits a non-canonically-placed key where it is", out,
					  "misc { border_radius 12; gappih 16 }\n");
			free(out);
		});
	}

	/* A node with no argument takes one without a second code path. */
	{
		const char *in = "effects { blur { enable } }\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "effects/blur/enable");
			const char *args[] = {"0"};
			char *out = kdl_edit_set_args(in, n, args, 1);
			check_str("gives an argument to a bare node", out,
					  "effects { blur { enable 0 } }\n");
			free(out);
		});
	}

	/* Several arguments at once -- an animation bezier is four numbers, and
	 * they have to go together or the curve is briefly nonsense. */
	{
		const char *in = "animations { curve 0.1 0.2 0.3 0.4 }\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "animations/curve");
			const char *args[] = {"0.25", "0.1", "0.25", "1.0"};
			char *out = kdl_edit_set_args(in, n, args, 4);
			check_str("replaces a whole multi-argument value", out,
					  "animations { curve 0.25 0.1 0.25 1.0 }\n");
			free(out);
		});
	}

	/* Quoting is the caller's business, so a value with a space round-trips
	 * exactly as handed over. */
	{
		const char *in = "theme { font \"monospace Bold 16\" }\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "theme/font");
			const char *args[] = {"\"Ubuntu 17\""};
			char *out = kdl_edit_set_args(in, n, args, 1);
			check_str("replaces a quoted string value", out,
					  "theme { font \"Ubuntu 17\" }\n");
			free(out);
		});
	}

	/* Anything on the same line that is not this node's arguments survives. */
	{
		const char *in = "\tborder { width 3 }  // 0 disables\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "border/width");
			const char *args[] = {"1"};
			char *out = kdl_edit_set_args(in, n, args, 1);
			check_str("a trailing comment on the same line survives", out,
					  "\tborder { width 1 }  // 0 disables\n");
			free(out);
		});
	}

	/* A node carrying properties before its arguments is refused rather than
	 * mangled -- rewriting args around a prop needs a policy nobody has asked
	 * for, and silently eating the prop is not it. */
	{
		const char *in = "bind key=a 1 2\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "bind");
			const char *args[] = {"9"};
			char *out = kdl_edit_set_args(in, n, args, 1);
			check("refuses a node whose properties precede its arguments",
				  out == NULL);
			free(out);
		});
	}

	/* ---------- set_path: creating what is missing ---------- */

	/* The new entry lands INSIDE the block the user already has, not as a
	 * second block of the same name at the end of the file. */
	{
		const char *in = "effects {\n"
						 "\t// Frosted glass behind floating windows.\n"
						 "\tblur { enable 1; radius 6 }\n"
						 "}\n";
		WITH_DOC(in, {
			const char *args[] = {"24"};
			char *out = kdl_edit_set_path(in, &doc, "effects/shadow/size",
										  args, 1);
			check_str("creates a missing sub-block inside its parent", out,
					  "effects {\n"
					  "\t// Frosted glass behind floating windows.\n"
					  "\tblur { enable 1; radius 6 }\n"
					  "\tshadow { size 24 }\n"
					  "}\n");
			free(out);
		});
	}

	/* Same thing in a four-space document: the indent is detected, not assumed.
	 * A config that suddenly mixes tabs into space-indented blocks reads as
	 * corrupted even when it parses. */
	{
		const char *in = "effects {\n"
						 "    blur { enable 1 }\n"
						 "}\n";
		WITH_DOC(in, {
			const char *args[] = {"24"};
			char *out = kdl_edit_set_path(in, &doc, "effects/shadow/size",
										  args, 1);
			check_str("matches four-space indentation", out,
					  "effects {\n"
					  "    blur { enable 1 }\n"
					  "    shadow { size 24 }\n"
					  "}\n");
			free(out);
		});
	}

	/* An unterminated last entry gets its separator, or the new entry becomes
	 * more arguments to the entry above it. `enable 1 shadow { size 24 }` is a
	 * three-argument `enable`, and the config stops meaning what it says. */
	{
		const char *in = "effects { blur { enable 1 } }\n";
		WITH_DOC(in, {
			const char *args[] = {"24"};
			char *out = kdl_edit_set_path(in, &doc, "effects/shadow/size",
										  args, 1);
			/* `}` already terminates the preceding node, so no `;` is added. */
			check("appends into a one-line block",
				  out && strstr(out, "shadow { size 24 }") != NULL);
			check("...and the block still parses", ({
					  KdlDocument d2 = {0};
					  char e2[256] = "";
					  bool ok = out && kdl_parse(out, &d2, e2, sizeof(e2));
					  kdl_free(&d2);
					  ok;
				  }));
			free(out);
		});
	}

	{
		const char *in = "effects { blur { enable 1; radius 6 } }\n";
		WITH_DOC(in, {
			const char *args[] = {"8"};
			char *out =
				kdl_edit_set_path(in, &doc, "effects/blur/passes", args, 1);
			check("an unterminated last entry gets a separator",
				  out && strstr(out, "radius 6;") != NULL);
			check("...and the new entry is its own node", ({
					  KdlDocument d2 = {0};
					  char e2[256] = "";
					  bool ok = out && kdl_parse(out, &d2, e2, sizeof(e2));
					  const KdlNode *r =
						  ok ? kdl_locate_path(&d2, "effects/blur/radius")
							 : NULL;
					  bool good = r && r->n_args == 1 &&
								  !strcmp(r->args[0].value, "6");
					  kdl_free(&d2);
					  good;
				  }));
			free(out);
		});
	}

	/* Nothing to hang it off: a whole declaration at the end of the file. */
	{
		const char *in = "misc { gappih 16 }\n";
		WITH_DOC(in, {
			const char *args[] = {"0x445566ff"};
			char *out =
				kdl_edit_set_path(in, &doc, "layout/border/color", args, 1);
			check_str("appends a new top-level path at the end", out,
					  "misc { gappih 16 }\n"
					  "layout { border { color 0x445566ff } }\n");
			free(out);
		});
	}

	/* set_path on a path that DOES exist is an in-place edit, not an append.
	 * Getting this wrong gives two declarations and a growing config file. */
	{
		const char *in = "effects {\n\tblur { radius 6 }\n}\n";
		WITH_DOC(in, {
			const char *args[] = {"9"};
			char *out =
				kdl_edit_set_path(in, &doc, "effects/blur/radius", args, 1);
			check_str("an existing path is edited, never appended", out,
					  "effects {\n\tblur { radius 9 }\n}\n");
			free(out);
		});
	}

	/* An empty block still gets a readable first entry. */
	{
		const char *in = "effects {\n}\n";
		WITH_DOC(in, {
			const char *args[] = {"1"};
			char *out =
				kdl_edit_set_path(in, &doc, "effects/blur/enable", args, 1);
			check_str("fills an empty block", out,
					  "effects {\n\tblur { enable 1 }\n}\n");
			free(out);
		});
	}

	/* ---------- remove ---------- */

	{
		const char *in = "layout {\n"
						 "\tborder { width 3 }\n"
						 "\ttitlebar { enable 1 }\n"
						 "}\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "layout/titlebar");
			char *out = kdl_edit_remove(in, n, false);
			check_str("removing a block takes its whole line", out,
					  "layout {\n"
					  "\tborder { width 3 }\n"
					  "}\n");
			free(out);
		});
	}

	/* A node's explanation goes with it. Left behind, those two lines would sit
	 * above `width` and describe the wrong setting. */
	{
		const char *in = "layout {\n"
						 "\t// Server-side titlebars. Off by default because\n"
						 "\t// they eat content height.\n"
						 "\ttitlebar { enable 1 }\n"
						 "\tborder { width 3 }\n"
						 "}\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "layout/titlebar");
			char *out = kdl_edit_remove(in, n, true);
			check_str("removal takes the comment lines above it", out,
					  "layout {\n"
					  "\tborder { width 3 }\n"
					  "}\n");
			free(out);
		});
	}

	/* ...but only when asked, and never a comment belonging to a neighbour. */
	{
		const char *in = "layout {\n"
						 "\tborder { width 3 }  // resting border\n"
						 "\ttitlebar { enable 1 }\n"
						 "}\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "layout/titlebar");
			char *out = kdl_edit_remove(in, n, true);
			check_str("a neighbour's trailing comment is not claimed", out,
					  "layout {\n"
					  "\tborder { width 3 }  // resting border\n"
					  "}\n");
			free(out);
		});
	}

	{
		const char *in = "border { width 3; color 0x111111ff }\n";
		WITH_DOC(in, {
			const KdlNode *n = kdl_locate_path(&doc, "border/width");
			char *out = kdl_edit_remove(in, n, false);
			check("removing one entry of a one-line block leaves the others",
				  out && strstr(out, "color 0x111111ff") != NULL &&
					  strstr(out, "width") == NULL);
			check("...and it still parses", ({
					  KdlDocument d2 = {0};
					  char e2[256] = "";
					  bool ok = out && kdl_parse(out, &d2, e2, sizeof(e2));
					  kdl_free(&d2);
					  ok;
				  }));
			free(out);
		});
	}

	/* ---------- append_path: the override escape hatch ---------- */

	{
		const char *in = "source \"./colors.kdl\"\n";
		WITH_DOC(in, {
			const char *args[] = {"0x445566ff"};
			char *out = kdl_edit_append_path(
				in, "layout/border/color", args, 1,
				"// Set by the settings app. Overrides ./colors.kdl, which\n"
				"// matugen regenerates -- this wins because it is later.");
			check("append lands after the source line",
				  out && strstr(out, "source \"./colors.kdl\"") <
							 strstr(out, "0x445566ff"));
			check("...carrying its explanation",
				  out && strstr(out, "matugen regenerates") != NULL);
			check("...and parsing to the value that was asked for", ({
					  KdlDocument d2 = {0};
					  char e2[256] = "";
					  bool ok = out && kdl_parse(out, &d2, e2, sizeof(e2));
					  const KdlNode *c =
						  ok ? kdl_locate_path(&d2, "layout/border/color")
							 : NULL;
					  bool good = c && c->n_args == 1 &&
								  !strcmp(c->args[0].value, "0x445566ff");
					  kdl_free(&d2);
					  good;
				  }));
			free(out);
		});
	}

	/* ---------- indent detection ---------- */

	{
		char unit[16];
		kdl_detect_indent("a {\n\tb 1\n}\n", unit, sizeof(unit));
		check_str("detects a tab indent", unit, "\t");
		kdl_detect_indent("a {\n    b 1\n}\n", unit, sizeof(unit));
		check_str("detects a four-space indent", unit, "    ");
		kdl_detect_indent("a {\n  b 1\n}\n", unit, sizeof(unit));
		check_str("detects a two-space indent", unit, "  ");
		/* A leading comment block must not be mistaken for the indent unit. */
		kdl_detect_indent("// header\n//   continued\na {\n\tb 1\n}\n", unit,
						  sizeof(unit));
		check_str("a comment block is not the indent unit", unit, "\t");
		kdl_detect_indent("a 1\nb 2\n", unit, sizeof(unit));
		check_str("falls back to a tab when nothing is indented", unit, "\t");
	}

	/* ---------- the real config, edited many times over ---------- */

	/* The corpus check. Every edit must leave a document that re-parses AND
	 * that reads back the value just written -- "it parses" alone would pass on
	 * a writer that dropped the edit on the floor. */
	{
		FILE *f = fopen("assets/config.kdl", "r");
		if (!f)
			f = fopen("../assets/config.kdl", "r");
		if (!f) {
			printf("ok     (skipped corpus: assets/config.kdl not found)\n");
		} else {
			fseek(f, 0, SEEK_END);
			long sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			char *text = malloc((size_t)sz + 1);
			size_t rd = fread(text, 1, (size_t)sz, f);
			text[rd] = '\0';
			fclose(f);

			static const char *paths[] = {
				"layout/border/width",   "layout/border/color",
				"effects/blur/radius",   "effects/blur/passes",
				"effects/shadow/size",   "theme/corner-radius",
				"misc/border_radius",    "overview/gaps",
				"animations/window-open/duration",
			};
			char *cur = strdup(text);
			bool all_ok = true;
			unsigned seed = 12345;
			for (int i = 0; i < 500 && all_ok; i++) {
				seed = seed * 1103515245u + 12345u;
				const char *path = paths[(seed >> 16) % (sizeof(paths) /
														 sizeof(paths[0]))];
				char val[32];
				snprintf(val, sizeof(val), "%u", (seed >> 8) % 64);

				KdlDocument d = {0};
				char e[256] = "";
				if (!kdl_parse(cur, &d, e, sizeof(e))) {
					printf("        iteration %d: no longer parses: %s\n", i,
						   e);
					all_ok = false;
					kdl_free(&d);
					break;
				}
				const char *args[] = {A1(val)};
				char *next = kdl_edit_set_path(cur, &d, path, args, 1);
				kdl_free(&d);
				if (!next) {
					printf("        iteration %d: set_path(%s) failed\n", i,
						   path);
					all_ok = false;
					break;
				}
				free(cur);
				cur = next;

				/* Read it back through a fresh parse. */
				KdlDocument d2 = {0};
				if (!kdl_parse(cur, &d2, e, sizeof(e))) {
					printf("        iteration %d: write broke the parse: %s\n",
						   i, e);
					all_ok = false;
					kdl_free(&d2);
					break;
				}
				const KdlNode *n = kdl_locate_path(&d2, path);
				if (!n || n->n_args != 1 || strcmp(n->args[0].value, val)) {
					printf("        iteration %d: %s did not read back as %s\n",
						   i, path, val);
					all_ok = false;
				}
				kdl_free(&d2);
			}
			check("500 edits to the shipped config all parse and read back",
				  all_ok);

			/* And the file has not grown a duplicate block per edit. */
			int layout_blocks = 0;
			for (const char *p = cur; (p = strstr(p, "\nlayout ")); p++)
				layout_blocks++;
			check("...without appending a duplicate block each time",
				  layout_blocks <= 2);

			free(cur);
			free(text);
		}
	}

	printf("\n%s\n", failures ? "FAILED" : "all kdl-write tests passed");
	return failures ? 1 : 0;
}
