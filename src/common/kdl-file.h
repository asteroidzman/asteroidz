#ifndef ASTEROIDZ_KDL_FILE_H
#define ASTEROIDZ_KDL_FILE_H

/* Reading and replacing a whole config file, atomically.
 *
 * Split out of output_persist (src/action/output.h), which was the first and
 * only thing that wrote to the user's config. The settings-app write path needs
 * exactly the same read-slurp / .tmp / fsync / rename sequence, and two copies
 * of "do not leave the user without a config that loads" is one copy too many.
 *
 * Pure stdio with no compositor types, so it stays unit-testable alongside
 * kdl-write.h.
 */

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The whole file as a NUL-terminated string, or NULL. Caller frees. */
static char *kdl_file_slurp(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long sz = ftell(f);
	if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	char *text = malloc((size_t)sz + 1);
	if (!text) {
		fclose(f);
		return NULL;
	}
	size_t rd = fread(text, 1, (size_t)sz, f);
	text[rd] = '\0';
	fclose(f);
	return text;
}

/* Replace `path` with `text`.
 *
 * Written to a temporary and renamed over, never in place: a config truncated
 * by a crash mid-write is a compositor that does not come back, and the first
 * caller of this ran on a mode set -- exactly when the machine is most likely
 * to be unhappy. The directory is fsync'd as well as the file, because rename
 * durability is a property of the DIRECTORY entry; without it a crash can leave
 * the new contents on disk with the old name still pointing at nothing.
 *
 * `keep_backup` leaves the previous contents at `<path>.bak`, overwritten each
 * time. A settings app applies several changes at once and "undo the last
 * apply" should not require the user to have thought about it first -- they are
 * already doing this by hand, judging by the config.kdl.bak.* files next to the
 * real one. */
static bool kdl_file_replace(const char *path, const char *text,
							 bool keep_backup) {
	if (!path || !text)
		return false;

	if (keep_backup) {
		char bak[1100];
		snprintf(bak, sizeof(bak), "%s.bak", path);
		char *prev = kdl_file_slurp(path);
		if (prev) {
			FILE *b = fopen(bak, "wb");
			if (b) {
				size_t plen = strlen(prev);
				/* Best-effort: failing to keep a backup is not a reason to
				 * refuse the write the user asked for. */
				(void)(fwrite(prev, 1, plen, b) == plen);
				fclose(b);
			}
			free(prev);
		}
	}

	char tmp[1100];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	bool ok = false;
	FILE *w = fopen(tmp, "wb");
	if (w) {
		size_t len = strlen(text);
		ok = fwrite(text, 1, len, w) == len;
		if (ok && fflush(w) != 0)
			ok = false;
		if (ok && fsync(fileno(w)) != 0)
			ok = false;
		if (fclose(w) != 0)
			ok = false;
	}
	if (ok && rename(tmp, path) != 0)
		ok = false;
	if (!ok) {
		unlink(tmp);
		return false;
	}

	/* fsync the containing directory so the rename itself is durable. */
	char dir[1100];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		int dfd = open(dir[0] ? dir : "/", O_RDONLY | O_DIRECTORY);
		if (dfd >= 0) {
			(void)fsync(dfd);
			close(dfd);
		}
	}
	return true;
}

#endif /* ASTEROIDZ_KDL_FILE_H */
