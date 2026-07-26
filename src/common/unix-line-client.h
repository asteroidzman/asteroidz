#ifndef ASTEROIDZ_UNIX_LINE_CLIENT_H
#define ASTEROIDZ_UNIX_LINE_CLIENT_H

/* A long-lived unix-socket client that delivers newline-delimited messages on
 * the compositor's own event loop, and reconnects with backoff when the peer
 * is not there.
 *
 * This is the shape every "talk to my session daemon" integration wants: the
 * daemon may not be running yet, may be restarted underneath us, and must
 * never block the compositor while we wait for it. Distinct from
 * async-spawn.h's streaming mode, which owns a CHILD PROCESS -- here the peer
 * has its own lifetime and we are merely a client of it.
 *
 * Every operation is non-blocking. A send to a dead socket is dropped rather
 * than retried: these are commands whose outcome arrives as an event anyway,
 * and queueing them would mean replaying stale intent at a daemon that just
 * came back.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>

/* Called once per complete line, newline stripped. */
typedef void (*UnixLineCb)(const char *line, void *user);
/* Called when the connection is established or lost, so the caller can reflect
 * "daemon not running" without having to poll. */
typedef void (*UnixLineStateCb)(bool connected, void *user);

/* Starting size. The buffer GROWS from here -- see the read handler -- because
 * a line's length is a property of what the peer has to say, not a symptom.
 * discord-voiced's channel snapshot is 19KB for an ordinary account (118 voice
 * channels across 13 servers), and a fixed 16KB buffer silently discarded it
 * every single time: the small status lines arrived, so the module looked
 * connected and healthy, and the join menu was permanently empty. */
#define UNIX_LINE_BUF 16384
/* Refuse to grow past this. A peer emitting an unbounded line IS
 * malfunctioning, and a compositor must not follow it into swap. */
#define UNIX_LINE_MAX (4 * 1024 * 1024)
#define UNIX_LINE_BACKOFF_MIN 500
#define UNIX_LINE_BACKOFF_MAX 8000

typedef struct {
	struct wl_event_loop *loop;
	struct wl_event_source *source;
	struct wl_event_source *retry;
	int fd;
	char path[256];
	char *buf;   /* grown on demand; NULL until the first read */
	size_t cap;
	size_t len;
	UnixLineCb line_cb;
	UnixLineStateCb state_cb;
	void *user;
	int32_t backoff_ms;
	bool connected;
	bool stopped;
} UnixLineClient;

static void unix_line_connect(UnixLineClient *c);

static void unix_line_schedule_retry(UnixLineClient *c);

static void unix_line_drop(UnixLineClient *c) {
	if (c->source) {
		wl_event_source_remove(c->source);
		c->source = NULL;
	}
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
	c->len = 0;
	if (c->connected) {
		c->connected = false;
		if (c->state_cb)
			c->state_cb(false, c->user);
	}
}

static int unix_line_readable(int fd, uint32_t mask, void *data) {
	UnixLineClient *c = data;
	if (mask & WL_EVENT_READABLE) {
		for (;;) {
			if (c->len + 1 >= c->cap) {
				/* Grow, rather than throw the line away. Dropping at a fixed
				 * size makes the failure depend on how much the peer has to
				 * say, which is the worst kind of bug to find: everything
				 * works until someone joins one more server. */
				size_t want = c->cap ? c->cap * 2 : UNIX_LINE_BUF;
				if (want > UNIX_LINE_MAX) {
					wlr_log(WLR_ERROR,
							"unix-line: %s sent more than %d bytes with no "
							"newline; dropping",
							c->path, UNIX_LINE_MAX);
					c->len = 0;
					want = UNIX_LINE_BUF;
				}
				char *grown = realloc(c->buf, want);
				if (!grown) {
					c->len = 0;
					return 0;
				}
				c->buf = grown;
				c->cap = want;
			}
			ssize_t n = read(fd, c->buf + c->len, c->cap - c->len - 1);
			if (n > 0) {
				c->len += (size_t)n;
				c->buf[c->len] = '\0';
				size_t start = 0;
				for (size_t i = 0; i < c->len; i++) {
					if (c->buf[i] != '\n')
						continue;
					c->buf[i] = '\0';
					if (c->line_cb)
						c->line_cb(c->buf + start, c->user);
					start = i + 1;
				}
				if (start) {
					memmove(c->buf, c->buf + start, c->len - start);
					c->len -= start;
				}
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				return 0; /* drained; stay registered */
			break;        /* 0 = peer closed, or a real error */
		}
	}
	/* EOF or hangup: the daemon went away */
	unix_line_drop(c);
	unix_line_schedule_retry(c);
	return 0;
}

static int unix_line_retry_cb(void *data) {
	UnixLineClient *c = data;
	c->retry = NULL;
	if (!c->stopped)
		unix_line_connect(c);
	return 0;
}

static void unix_line_schedule_retry(UnixLineClient *c) {
	if (c->stopped || c->retry)
		return;
	if (!c->retry)
		c->retry = wl_event_loop_add_timer(c->loop, unix_line_retry_cb, c);
	if (c->retry)
		wl_event_source_timer_update(c->retry, c->backoff_ms);
	/* back off toward a ceiling: a daemon the user never runs must not cost a
	 * connect attempt every half second for the life of the session */
	c->backoff_ms = c->backoff_ms >= UNIX_LINE_BACKOFF_MAX
						? UNIX_LINE_BACKOFF_MAX
						: c->backoff_ms * 2;
}

static void unix_line_connect(UnixLineClient *c) {
	if (c->stopped || c->fd >= 0)
		return;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		unix_line_schedule_retry(c);
		return;
	}
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", c->path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 &&
		errno != EINPROGRESS) {
		/* the usual case while the daemon is not running: ENOENT/ECONNREFUSED */
		close(fd);
		unix_line_schedule_retry(c);
		return;
	}
	c->fd = fd;
	c->source = wl_event_loop_add_fd(c->loop, fd, WL_EVENT_READABLE,
									 unix_line_readable, c);
	if (!c->source) {
		close(fd);
		c->fd = -1;
		unix_line_schedule_retry(c);
		return;
	}
	c->connected = true;
	c->backoff_ms = UNIX_LINE_BACKOFF_MIN; /* healthy connect resets it */
	if (c->state_cb)
		c->state_cb(true, c->user);
}

static void unix_line_start(UnixLineClient *c, struct wl_event_loop *loop,
							const char *path, UnixLineCb line_cb,
							UnixLineStateCb state_cb, void *user) {
	memset(c, 0, sizeof(*c));
	c->loop = loop;
	c->fd = -1;
	c->line_cb = line_cb;
	c->state_cb = state_cb;
	c->user = user;
	c->backoff_ms = UNIX_LINE_BACKOFF_MIN;
	snprintf(c->path, sizeof(c->path), "%s", path);
	unix_line_connect(c);
}

static void unix_line_stop(UnixLineClient *c) {
	c->stopped = true;
	if (c->retry) {
		wl_event_source_remove(c->retry);
		c->retry = NULL;
	}
	unix_line_drop(c);
	free(c->buf);
	c->buf = NULL;
	c->cap = c->len = 0;
}

/* Best-effort: a command whose outcome arrives as an event anyway is not worth
 * queueing against a daemon that is not listening. */
static void unix_line_send(UnixLineClient *c, const char *line) {
	if (!c->connected || c->fd < 0 || !line)
		return;
	size_t n = strlen(line);
	if (write(c->fd, line, n) < 0)
		return;
	if (write(c->fd, "\n", 1) < 0)
		return;
}

#endif /* ASTEROIDZ_UNIX_LINE_CLIENT_H */
