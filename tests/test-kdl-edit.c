/* Unit tests for src/common/kdl-edit.h.
 *
 * This code rewrites the user's own config file. Everything else in the tree
 * is tested by driving a compositor; this is tested directly, because the
 * failure mode is not "the bar looks wrong", it is a settings file that no
 * longer parses or that quietly lost the comment explaining why an output is
 * configured the way it is.
 *
 * Build/run: meson test -C build  (or ninja -C build test)
 */
#include "../src/common/kdl-edit.h"

#include <assert.h>
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
		printf("        got:  %s\n", got ? got : "(null)");
		printf("        want: %s\n", want);
		failures++;
	}
}

int main(void) {
	/* the common case: a one-line block with x and y already in it */
	{
		const char *in = "output DP-1 { scale 1; x 0; y 0; vrr 1; }\n";
		char *out = kdl_rewrite_output_pos(in, "DP-1", 1920, 200);
		check_str("replaces an existing x and y in place", out,
				  "output DP-1 { scale 1; x 1920; y 200; vrr 1; }\n");
		free(out);
	}

	/* everything that is not x or y must survive byte for byte, comments
	 * included -- this is the whole reason the function is textual */
	{
		const char *in =
			"// Managed by hand. Do not regenerate.\n"
			"output HDMI-A-1 { scale 0.75; width 1920; x 3840; y 0; }\n"
			"// No `hdr` here on purpose.\n"
			"output DP-1 { hdr; x 0; y 0; icc-profile \"/home/x/FI32U.icm\"; }\n";
		char *out = kdl_rewrite_output_pos(in, "DP-1", -1920, 40);
		check_str("leaves comments and every other field untouched", out,
				  "// Managed by hand. Do not regenerate.\n"
				  "output HDMI-A-1 { scale 0.75; width 1920; x 3840; y 0; }\n"
				  "// No `hdr` here on purpose.\n"
				  "output DP-1 { hdr; x -1920; y 40; "
				  "icc-profile \"/home/x/FI32U.icm\"; }\n");
		free(out);
	}

	/* a block that never had a position gets one appended rather than being
	 * silently skipped */
	{
		const char *in = "output DP-2 { scale 1; }\n";
		char *out = kdl_rewrite_output_pos(in, "DP-2", 100, 50);
		check("appends x/y to a block that had none",
			  out && strstr(out, "x 100") && strstr(out, "y 50") &&
				  strstr(out, "scale 1"));
		free(out);
	}

	/* Appending after an UNTERMINATED last entry has to insert a separator:
	 * `refresh 60  x 3409` is a three-argument `refresh` node, not a refresh
	 * rate and a position, so the bare append silently changed what the file
	 * means. */
	{
		const char *in = "output DP-2 { width 1920; height 1080; refresh 60 }\n";
		char *out = kdl_rewrite_output_pos(in, "DP-2", 3409, 0);
		check("separates an appended x/y from an unterminated last entry",
			  out && strstr(out, "60; x 3409") && strstr(out, "y 0"));
		free(out);
	}

	/* multi-line blocks are the other common hand-written shape */
	{
		const char *in = "output DP-1 {\n"
						 "    scale 1\n"
						 "    x 0\n"
						 "    y 0\n"
						 "}\n";
		char *out = kdl_rewrite_output_pos(in, "DP-1", 640, 480);
		check("handles a block written across several lines",
			  out && strstr(out, "x 640") && strstr(out, "y 480") &&
				  strstr(out, "scale 1") && !strstr(out, "x 0\n"));
		free(out);
	}

	/* the name must match exactly: DP-1 is not DP-10, and a prefix match
	 * would move the wrong monitor */
	{
		const char *in = "output DP-10 { x 0; y 0; }\n";
		char *out = kdl_rewrite_output_pos(in, "DP-1", 5, 5);
		check("does not match a longer name by prefix", out == NULL);
		free(out);
	}

	/* a commented-out block is not a block */
	{
		const char *in = "// output DP-1 { x 0; y 0; }\n";
		char *out = kdl_rewrite_output_pos(in, "DP-1", 5, 5);
		check("ignores a commented-out block", out == NULL);
		free(out);
	}

	/* absent entirely: the caller needs NULL so it can try the next file
	 * rather than inventing a rule in the wrong one */
	{
		const char *in = "output DP-1 { x 0; y 0; }\n";
		char *out = kdl_rewrite_output_pos(in, "HDMI-A-1", 5, 5);
		check("returns NULL when the output is not in this file", out == NULL);
		free(out);
	}

	/* an unterminated block is malformed input, not something to guess at */
	{
		const char *in = "output DP-1 { x 0; y 0;\n";
		char *out = kdl_rewrite_output_pos(in, "DP-1", 5, 5);
		check("refuses a block with no closing brace", out == NULL);
		free(out);
	}

	/* nested braces must not end the block early */
	{
		const char *in = "output DP-1 { x 0; extra { a 1 }; y 0; }\ntrailing\n";
		char *out = kdl_rewrite_output_pos(in, "DP-1", 7, 8);
		check("brace-matches past a nested block",
			  out && strstr(out, "x 7") && strstr(out, "y 8") &&
				  strstr(out, "extra { a 1 }") && strstr(out, "trailing"));
		free(out);
	}

	/* a key merely STARTING with x or y is a different key */
	{
		const char *in = "output DP-1 { xwayland-scale 2; x 0; y 0; }\n";
		char *out = kdl_rewrite_output_pos(in, "DP-1", 3, 4);
		check_str("does not mistake a longer key for x", out,
				  "output DP-1 { xwayland-scale 2; x 3; y 4; }\n");
		free(out);
	}

	/* negative positions are ordinary: a monitor left of the origin */
	{
		const char *in = "output DP-1 { x 100; y 100; }\n";
		char *out = kdl_rewrite_output_pos(in, "DP-1", -3840, -1080);
		check_str("writes negative coordinates", out,
				  "output DP-1 { x -3840; y -1080; }\n");
		free(out);
	}

	/* indented, as it would be inside a larger document */
	{
		const char *in = "  output DP-1 { x 0; y 0; }\n";
		char *out = kdl_rewrite_output_pos(in, "DP-1", 1, 2);
		check_str("finds an indented block", out,
				  "  output DP-1 { x 1; y 2; }\n");
		free(out);
	}

	/* ── kdl_rewrite_output_props ────────────────────────────────────────
	 *
	 * The general form, which set_output_mode/scale/vrr/icc all persist
	 * through. A mistake here writes the wrong resolution into a file nobody
	 * reads until their screen comes up wrong. */

	/* several keys at once, all already present */
	{
		const char *in =
			"output DP-1 { scale 1; width 1920; height 1080; refresh 60; }\n";
		const char *keys[] = {"width", "height", "refresh"};
		const char *vals[] = {"2560", "1440", "144"};
		char *out = kdl_rewrite_output_props(in, "DP-1", keys, vals, 3);
		check_str("replaces several keys in one pass", out,
				  "output DP-1 { scale 1; width 2560; height 1440; "
				  "refresh 144; }\n");
		free(out);
	}

	/* a key that is not there yet is APPENDED -- and appending after an
	 * unterminated entry needs a `;` first, or it becomes an argument to it */
	{
		const char *in = "output DP-1 { scale 1 }\n";
		const char *keys[] = {"vrr"};
		const char *vals[] = {"1"};
		char *out = kdl_rewrite_output_props(in, "DP-1", keys, vals, 1);
		check_str("terminates the last entry before appending a new one", out,
				  "output DP-1 { scale 1; vrr 1; }\n");
		free(out);
	}

	/* mixed: one present, one absent */
	{
		const char *in = "output DP-1 { scale 1; x 0; y 0; }\n";
		const char *keys[] = {"scale", "vrr"};
		const char *vals[] = {"1.25", "0"};
		char *out = kdl_rewrite_output_props(in, "DP-1", keys, vals, 2);
		check_str("replaces one key and appends another", out,
				  "output DP-1 { scale 1.25; x 0; y 0; vrr 0; }\n");
		free(out);
	}

	/* a quoted value passes through as given -- the caller owns the quoting */
	{
		const char *in = "output DP-1 { scale 1; }\n";
		const char *keys[] = {"icc-profile"};
		const char *vals[] = {"\"/home/u/p.icm\""};
		char *out = kdl_rewrite_output_props(in, "DP-1", keys, vals, 1);
		check_str("appends a quoted path verbatim", out,
				  "output DP-1 { scale 1; icc-profile \"/home/u/p.icm\"; }\n");
		free(out);
	}

	/* NULL removes. Clearing an ICC profile has to take the entry OUT: an
	 * `icc-profile ""` is a profile whose path is empty, not the absence of
	 * one, and the untransformed pipeline is reachable only by absence. */
	{
		const char *in =
			"output DP-1 { scale 1; icc-profile \"/p.icm\"; vrr 1; }\n";
		const char *keys[] = {"icc-profile"};
		const char *vals[] = {NULL};
		char *out = kdl_rewrite_output_props(in, "DP-1", keys, vals, 1);
		check_str("removes a key rather than emptying it", out,
				  "output DP-1 { scale 1; vrr 1; }\n");
		free(out);
	}

	/* removing something that was never there changes nothing */
	{
		const char *in = "output DP-1 { scale 1; vrr 1; }\n";
		const char *keys[] = {"icc-profile"};
		const char *vals[] = {NULL};
		char *out = kdl_rewrite_output_props(in, "DP-1", keys, vals, 1);
		check_str("removing an absent key is a no-op", out,
				  "output DP-1 { scale 1; vrr 1; }\n");
		free(out);
	}

	/* the multi-line form, which is how monitors.kdl looks once someone has
	 * been editing it by hand */
	{
		const char *in = "output DP-1 {\n"
						 "    // 4K, and it stays 4K\n"
						 "    scale 1\n"
						 "    width 3840\n"
						 "}\n";
		const char *keys[] = {"scale"};
		const char *vals[] = {"1.5"};
		char *out = kdl_rewrite_output_props(in, "DP-1", keys, vals, 1);
		check_str("keeps a multi-line block and its comment", out,
				  "output DP-1 {\n"
				  "    // 4K, and it stays 4K\n"
				  "    scale 1.5\n"
				  "    width 3840\n"
				  "}\n");
		free(out);
	}

	printf("\n%s\n", failures ? "FAILED" : "all kdl-edit tests passed");
	return failures ? 1 : 0;
}
