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

/* Streaming variant: called once per complete line, with the newline stripped,
 * while the child keeps running. Used for subscribe-style commands that never
 * exit -- `pactl subscribe` reports every mixer change on stdout, which is one
 * long-lived process instead of a poll that forks a child per tick for state
 * that changes a few times an hour. */
typedef void (*AsyncSpawnLineCb)(const char *line, void *user);

typedef struct AsyncSpawn {
	struct wl_event_source *source;
	int fd;
	/* Write end of the child's stdin, or -1 when it was not asked for.
	 *
	 * Only streaming children get one, and only when they ask: it is what makes
	 * a plugin two-way, so the compositor can hand it a click rather than the
	 * plugin only ever talking at us. A child that never reads it costs one
	 * unused fd, which is why it is opt-in rather than always. */
	int in_fd;
	pid_t pid;
	char *buf;
	size_t len, cap;
	AsyncSpawnCb cb;
	/* set instead of cb for a streaming child; the two are exclusive */
	AsyncSpawnLineCb line_cb;
	void *user;
	/* Optional back-pointer to the caller's handle. A streaming child can die
	 * on its own (the daemon it talks to restarts, someone kills it), which
	 * frees this struct -- so it clears the caller's pointer on the way out
	 * rather than leaving them holding freed memory. */
	struct AsyncSpawn **owner;
} AsyncSpawn;

static void async_spawn_finish(AsyncSpawn *as) {
	if (as->owner)
		*as->owner = NULL;
	if (as->in_fd >= 0) {
		close(as->in_fd);
		as->in_fd = -1;
	}
	if (as->source)
		wl_event_source_remove(as->source);
	if (as->fd >= 0)
		close(as->fd);
	if (as->buf)
		as->buf[as->len] = '\0';
	/* a streaming child has already delivered everything it produced, line by
	 * line; there is no whole-output callback to fire for it */
	if (as->cb)
		as->cb(as->buf ? as->buf : "", as->len, as->user);
	free(as->buf);
	free(as);
}

/* Hand every complete line in the buffer to the callback and drop it, keeping
 * whatever partial line is left for the next read. */
static void async_spawn_drain_lines(AsyncSpawn *as) {
	size_t start = 0;
	for (size_t i = 0; i < as->len; i++) {
		if (as->buf[i] != '\n')
			continue;
		as->buf[i] = '\0';
		as->line_cb(as->buf + start, as->user);
		start = i + 1;
	}
	if (start == 0) {
		/* No newline yet. A child that never emits one would grow the buffer
		 * without bound, so treat an absurdly long "line" as garbage and drop
		 * it rather than letting the cap check below kill the stream. */
		if (as->len > 64 * 1024)
			as->len = 0;
		return;
	}
	memmove(as->buf, as->buf + start, as->len - start);
	as->len -= start;
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
				if (as->line_cb) {
					as->buf[as->len] = '\0';
					async_spawn_drain_lines(as);
				}
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

/* argv must be NULL-terminated. Returns NULL if the child could not be
 * started, in which case no callback ever fires. Exactly one of cb / line_cb
 * should be set: cb for a command that runs to completion, line_cb for one
 * that streams. */
static AsyncSpawn *async_spawn_run(struct wl_event_loop *loop,
								   char *const argv[], AsyncSpawnCb cb,
								   AsyncSpawnLineCb line_cb, void *user,
								   bool with_stdin) {
	int fds[2];
	if (pipe(fds) < 0)
		return NULL;
	int in[2] = {-1, -1};
	if (with_stdin && pipe(in) < 0) {
		close(fds[0]);
		close(fds[1]);
		return NULL;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		if (in[0] >= 0) {
			close(in[0]);
			close(in[1]);
		}
		return NULL;
	}
	if (pid == 0) {
		/* die with the compositor rather than lingering as an orphan */
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		if (in[0] >= 0) {
			close(in[1]);
			dup2(in[0], STDIN_FILENO);
			close(in[0]);
		}
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
	if (in[0] >= 0) {
		close(in[0]);
		/* Non-blocking on our side too: a child that stops reading must cost
		 * us a dropped event, never a stalled compositor. */
		fcntl(in[1], F_SETFL, O_NONBLOCK);
		fcntl(in[1], F_SETFD, FD_CLOEXEC);
	}

	AsyncSpawn *as = calloc(1, sizeof(*as));
	if (!as) {
		close(fds[0]);
		if (in[1] >= 0)
			close(in[1]);
		return NULL;
	}
	as->fd = fds[0];
	as->in_fd = in[1];
	as->pid = pid;
	as->cb = cb;
	as->line_cb = line_cb;
	as->user = user;
	as->source = wl_event_loop_add_fd(loop, fds[0], WL_EVENT_READABLE,
									  async_spawn_readable, as);
	if (!as->source) {
		close(fds[0]);
		if (as->in_fd >= 0)
			close(as->in_fd);
		free(as);
		return NULL;
	}
	return as;
}

/* Send one line to a streaming child's stdin.
 *
 * Best effort by design. SIGPIPE is ignored process-wide, so a child that has
 * exited gives EPIPE rather than killing us, and a child that has stopped
 * reading gives EAGAIN -- both are dropped rather than retried or queued. The
 * events this carries are user actions: a click that arrives late is worse
 * than one that does not arrive, and a compositor that blocks on a wedged
 * plugin is worse than either. Lines are far under PIPE_BUF, so a write that
 * succeeds is never partial. */
static bool async_spawn_send(AsyncSpawn *as, const char *line) {
	if (!as || as->in_fd < 0 || !line)
		return false;
	size_t len = strlen(line);
	if (!len || len > 4000)
		return false;
	char buf[4096];
	memcpy(buf, line, len);
	buf[len] = '\n';
	ssize_t n = write(as->in_fd, buf, len + 1);
	return n == (ssize_t)(len + 1);
}

/* Collect a command's whole output and deliver it once, when it exits. */
static bool async_spawn(struct wl_event_loop *loop, char *const argv[],
						AsyncSpawnCb cb, void *user) {
	return async_spawn_run(loop, argv, cb, NULL, user, false) != NULL;
}

/* Start a long-lived command and deliver its output a line at a time. The
 * returned handle stays valid until the child exits or async_spawn_stop is
 * called; pass `owner` so it is nulled either way. */
static AsyncSpawn *async_spawn_lines(struct wl_event_loop *loop,
									 char *const argv[], AsyncSpawnLineCb cb,
									 void *user, AsyncSpawn **owner) {
	AsyncSpawn *as = async_spawn_run(loop, argv, NULL, cb, user, false);
	if (as && owner) {
		as->owner = owner;
		*owner = as;
	}
	return as;
}

static void async_spawn_stop(AsyncSpawn *as) {
	if (!as)
		return;
	/* the child is its own session leader (setsid), so signal the process
	 * directly -- there is no group of ours to signal */
	if (as->pid > 0)
		kill(as->pid, SIGTERM);
	/* NOT reaped here: asteroidz's global SIGCHLD handler reaps every child,
	 * and racing it just produces ECHILD noise. */
	async_spawn_finish(as);
}

#endif /* ASTEROIDZ_ASYNC_SPAWN_H */
