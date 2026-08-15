#ifndef AZ_IDENTITY_H
#define AZ_IDENTITY_H

/*
 * ── WHO ANSWERED? ────────────────────────────────────────────────────────
 *
 * A telemetry reading is worth nothing until the instance that produced it is
 * known. That is not hypothetical: an M6B live gate asserted
 * `validation_enabled` as its precondition -- the guard against a vacuous
 * `validation_errors: 0` -- and the assertion PASSED while the session under
 * test had no validation layer at all. `amsg` had answered from a leftover
 * headless test instance, and every headless M6B fixture sets
 * ASTEROIDZ_VK_DEBUG=1, so the wrong respondent reported exactly the value the
 * precondition was looking for. That run's amsg-derived numbers were
 * measurements of a different compositor.
 *
 * The socket fallback that allowed it is CORRECT and stays: ASTEROIDZ_INSTANCE_
 * SIGNATURE goes stale on every restart, so a tool inheriting an old
 * environment must be able to find the live compositor rather than fail
 * silently against a dead socket. What was missing is the other half -- a way
 * for a caller that knows which instance it means to say so, and be refused
 * otherwise.
 *
 * So this reports identity and `amsg --require-*` enforces it. A qualification
 * harness pins its target before it measures anything.
 *
 * ── THE BUILD ID, NOT A PATH AND NOT A VERSION ───────────────────────────
 *
 * `exe` is advisory. A running compositor whose binary has been replaced reads
 * `/usr/bin/asteroidz (deleted)` -- exactly the state a fresh `meson install`
 * leaves, and exactly when "is this the build I think it is?" matters most.
 * VERSION is worse: identical across every build of a release.
 *
 * The ELF build-id is embedded at link time, unique per link, and identifies
 * the code that is RUNNING. It is read from `/proc/self/exe`, which still opens
 * the original inode after the file has been unlinked -- so this answers
 * correctly in precisely the case that defeats the path.
 *
 * (dl_iterate_phdr would be shorter, but it needs _GNU_SOURCE, and defining
 * that project-wide silently changes strerror_r's signature.)
 */

#include <elf.h>
#include <stdio.h>
#include <string.h>

static char az_build_id_buf[41];

static void az_build_id_scan_notes(const unsigned char *buf, size_t len,
								   char *out) {
	size_t off = 0;
	while (off + sizeof(Elf64_Nhdr) <= len) {
		Elf64_Nhdr n;
		memcpy(&n, buf + off, sizeof(n));
		size_t name_sz = (n.n_namesz + 3) & ~3u;
		size_t desc_sz = (n.n_descsz + 3) & ~3u;
		if (off + sizeof(n) + name_sz + desc_sz > len) {
			return;
		}
		const char *name = (const char *)(buf + off + sizeof(n));
		if (n.n_type == NT_GNU_BUILD_ID && n.n_namesz == 4 &&
			memcmp(name, "GNU", 4) == 0) {
			const unsigned char *d = (const unsigned char *)(name + name_sz);
			size_t dl = n.n_descsz > 20 ? 20 : n.n_descsz; /* 40 hex + NUL */
			for (size_t b = 0; b < dl; b++) {
				snprintf(out + b * 2, 3, "%02x", d[b]);
			}
			return;
		}
		off += sizeof(n) + name_sz + desc_sz;
	}
}

/* The running image's build-id as lowercase hex, or "" if it has none.
 * Computed once. */
static const char *az_build_id(void) {
	static int done = 0;
	if (done) {
		return az_build_id_buf;
	}
	done = 1;
	az_build_id_buf[0] = '\0';

	FILE *f = fopen("/proc/self/exe", "rb");
	if (f == NULL) {
		return az_build_id_buf;
	}
	Elf64_Ehdr eh;
	if (fread(&eh, sizeof(eh), 1, f) != 1 ||
		memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
		eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_phentsize != sizeof(Elf64_Phdr)) {
		fclose(f);
		return az_build_id_buf;
	}
	for (Elf64_Half i = 0; i < eh.e_phnum; i++) {
		Elf64_Phdr ph;
		if (fseek(f, (long)(eh.e_phoff + (size_t)i * sizeof(ph)), SEEK_SET) != 0 ||
			fread(&ph, sizeof(ph), 1, f) != 1) {
			break;
		}
		/* Notes are small; anything larger is not a build-id segment and
		 * reading it would be the only unbounded allocation in this file. */
		if (ph.p_type != PT_NOTE || ph.p_filesz == 0 || ph.p_filesz > 65536) {
			continue;
		}
		unsigned char note[65536];
		if (fseek(f, (long)ph.p_offset, SEEK_SET) != 0 ||
			fread(note, 1, ph.p_filesz, f) != ph.p_filesz) {
			continue;
		}
		az_build_id_scan_notes(note, ph.p_filesz, az_build_id_buf);
		if (az_build_id_buf[0] != '\0') {
			break;
		}
	}
	fclose(f);
	return az_build_id_buf;
}

#endif /* AZ_IDENTITY_H */
