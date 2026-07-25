#ifndef ASTEROIDZ_ASYNC_SPAWN_H
#define ASTEROIDZ_ASYNC_SPAWN_H

/* Run a command and get its stdout back, without ever blocking the event
 * loop.
 *
 * The bar modules that wrap a CLI (weather via curl, and anything else later)
 * must not do what the waybar plugins they replace do: `volume.c` alone calls
 * g_spawn_command_line_sync five times per interaction. In a bar process that
 * is an invisible stutter; on the compositor's own event loop it is a dropped
 * frame and an input hitch. So: fork, keep the read end of a pipe in the
 * event loop, and deliver the output in a callback when the child closes it.
 *
 * The child gets PR_SET_PDEATHSIG so it cannot outlive the compositor, and is
 * NOT reaped here -- asteroidz already installs a global SIGCHLD handler that
 * reaps every child, and racing it would just produce ECHILD noise.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <wayland-server-core.h>

/* out is NUL-terminated; len excludes the terminator. out is freed after the
 * callback returns, so copy anything you keep. A child that produced nothing
 * still fires the callback, with len 0. */
typedef void (*AsyncSpawnCb)(const char *out, size_t len, void *user);

typedef struct {
	struct wl_event_source *source;
	int fd;
	char *buf;
	size_t len, cap;
	AsyncSpawnCb cb;
	void *user;
} AsyncSpawn;

static void async_spawn_finish(AsyncSpawn *as) {
	if (as->source)
		wl_event_source_remove(as->source);
	if (as->fd >= 0)
		close(as->fd);
	if (as->buf)
		as->buf[as->len] = '\0';
	if (as->cb)
		as->cb(as->buf ? as->buf : "", as->len, as->user);
	free(as->buf);
	free(as);
}

static int async_spawn_readable(int fd, uint32_t mask, void *data) {
	AsyncSpawn *as = data;

	if (mask & WL_EVENT_READABLE) {
		for (;;) {
			if (as->len + 4096 + 1 > as->cap) {
				size_t cap = as->cap ? as->cap * 2 : 8192;
				/* a runaway child must not be able to grow this without
				 * bound; 1 MiB is far more than any status command emits */
				if (cap > 1024 * 1024)
					break;
				char *b = realloc(as->buf, cap);
				if (!b)
					break;
				as->buf = b;
				as->cap = cap;
			}
			ssize_t n = read(fd, as->buf + as->len, 4096);
			if (n > 0) {
				as->len += (size_t)n;
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				return 0; /* drained for now, stay registered */
			break;        /* 0 = EOF, or a real error */
		}
	}

	/* EOF or hangup: the child is done talking */
	async_spawn_finish(as);
	return 0;
}

/* argv must be NULL-terminated. Returns false if the child could not be
 * started, in which case the callback never fires. */
static bool async_spawn(struct wl_event_loop *loop, char *const argv[],
						AsyncSpawnCb cb, void *user) {
	int fds[2];
	if (pipe(fds) < 0)
		return false;

	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return false;
	}
	if (pid == 0) {
		/* die with the compositor rather than lingering as an orphan */
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		/* stderr to /dev/null: a failing curl must not spam the session log */
		int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		setsid();
		execvp(argv[0], argv);
		_exit(127);
	}

	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);

	AsyncSpawn *as = calloc(1, sizeof(*as));
	if (!as) {
		close(fds[0]);
		return false;
	}
	as->fd = fds[0];
	as->cb = cb;
	as->user = user;
	as->source = wl_event_loop_add_fd(loop, fds[0], WL_EVENT_READABLE,
									  async_spawn_readable, as);
	if (!as->source) {
		close(fds[0]);
		free(as);
		return false;
	}
	return true;
}

#endif /* ASTEROIDZ_ASYNC_SPAWN_H */
