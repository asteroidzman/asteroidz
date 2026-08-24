#ifndef ASTEROIDZ_IPC_OUT_H
#define ASTEROIDZ_IPC_OUT_H

/* Queued writes for the IPC socket.
 *
 * A reply that may not fit in the socket buffer in one go.
 *
 * Every IPC fd is O_NONBLOCK, so send() writes what fits in SO_SNDBUF (208KB
 * on AF_UNIX by default, but a value the kernel is free to choose) and returns
 * THAT COUNT -- not an error. The code this replaces passed the return to a
 * `< 0` test and dropped the rest on the floor, so a reply larger than the
 * buffer reached the client with its tail missing and nothing was logged
 * anywhere: what the client saw was JSON ending mid-string. Nothing served at
 * the time was big enough to hit it, which is exactly why it had to be fixed
 * before the first big response existed rather than after the first bug
 * report.
 *
 * So a reply is queued, flushed as far as the socket will take it, and the fd
 * re-armed for WL_EVENT_WRITABLE with whatever is left. A one-shot client is
 * closed when the queue drains, not when its handler returns.
 *
 * Pure string-and-socket handling with no compositor types, so it is
 * unit-testable on its own against a socketpair whose SO_SNDBUF has been shrunk
 * to force the partial write that a normal-sized reply never triggers: see
 * a client that never reads, which is what a change here must answer to. */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

/* A watcher that has stopped reading must not be able to grow this without
 * bound -- `watch config` pushes on every reload, and a wedged settings app
 * would otherwise take the compositor's memory with it. Past the cap the
 * client is dropped, which is the same outcome it would get from any other
 * write error. */
#define IPC_OUT_MAX (4u * 1024u * 1024u)

struct ipc_out {
	char *buf;
	size_t len; /* bytes queued */
	size_t off; /* bytes already handed to the kernel */
};

static inline void ipc_out_reset(struct ipc_out *o) {
	free(o->buf);
	o->buf = NULL;
	o->len = o->off = 0;
}

static inline bool ipc_out_pending(const struct ipc_out *o) {
	return o->off < o->len;
}

/* Copy `len` bytes onto the queue. Ownership of `msg` stays with the caller. */
static inline bool ipc_out_append(struct ipc_out *o, const char *msg,
								  size_t len) {
	if (len == 0)
		return true;
	/* Fully-drained queue: start over rather than grow past what was sent. */
	if (o->off > 0 && o->off == o->len) {
		free(o->buf);
		o->buf = NULL;
		o->len = o->off = 0;
	}
	if (o->len - o->off + len > IPC_OUT_MAX)
		return false;
	char *nb = realloc(o->buf, o->len + len);
	if (!nb)
		return false;
	o->buf = nb;
	memcpy(o->buf + o->len, msg, len);
	o->len += len;
	return true;
}

/* Hand the kernel as much as it will take. False means the peer is gone.
 *
 * MSG_NOSIGNAL because a write to a socket whose peer has closed raises
 * SIGPIPE, and nothing in this compositor installs a handler for it -- a client
 * that exits between asking and reading would take the whole session down.
 * EPIPE comes back through the return value instead, where the caller already
 * handles it. */
static inline bool ipc_out_flush(int fd, struct ipc_out *o) {
	while (o->off < o->len) {
		ssize_t n = send(fd, o->buf + o->off, o->len - o->off, MSG_NOSIGNAL);
		if (n > 0) {
			o->off += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return true; /* socket full; the rest goes out on WRITABLE */
		if (n < 0 && errno == EINTR)
			continue;
		return false;
	}
	ipc_out_reset(o);
	return true;
}

#endif /* ASTEROIDZ_IPC_OUT_H */
