/* Unit tests for src/ipc/ipc-out.h.
 *
 * The bug this code exists to fix is invisible at any size the IPC socket has
 * ever been asked to carry: every fd is O_NONBLOCK, and send() on a nonblocking
 * socket writes what fits in SO_SNDBUF and RETURNS THAT COUNT. A `< 0` test
 * therefore reports success on a partial write, and the tail is simply lost --
 * silently, with the fd closed straight afterwards. Every reply served before
 * `get config-schema` fit in the buffer, so nothing ever exercised it.
 *
 * So the tests below shrink SO_SNDBUF on a real socketpair until a partial
 * write is guaranteed, and then check that every byte arrives anyway. Asserting
 * against a normal-sized socket would pass on the broken code, which is the
 * only thing that would make these tests worthless.
 *
 * Build/run: meson test -C build  (or ninja -C build test)
 */
#include "../src/ipc/ipc-out.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static int failures;

static void check(const char *what, bool ok) {
	printf("%s %s\n", ok ? "ok    " : "FAIL  ", what);
	if (!ok)
		failures++;
}

/* A socketpair whose send side is deliberately too small, and nonblocking, so
 * it behaves exactly like an IPC client that is not reading fast enough. */
static void make_pair(int sv[2], int sndbuf) {
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		perror("socketpair");
		exit(1);
	}
	setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
	int rcvbuf = sndbuf;
	setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	int fl = fcntl(sv[0], F_GETFL, 0);
	fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);
}

/* Read exactly n bytes, draining as the writer flushes. Returns bytes read. */
static size_t drain(int fd, char *out, size_t n) {
	size_t got = 0;
	while (got < n) {
		ssize_t r = recv(fd, out + got, n - got, 0);
		if (r <= 0)
			break;
		got += (size_t)r;
	}
	return got;
}

int main(void) {
	/* The whole point: a payload far larger than the socket buffer arrives
	 * intact, across as many flushes as it takes. */
	{
		const size_t N = 512 * 1024;
		char *payload = malloc(N);
		for (size_t i = 0; i < N; i++)
			payload[i] = (char)('a' + (i % 26));

		int sv[2];
		make_pair(sv, 4096);

		struct ipc_out o = {0};
		check("append accepts a payload larger than the socket buffer",
			  ipc_out_append(&o, payload, N));

		/* First flush cannot possibly complete: the buffer is 4K. */
		check("first flush succeeds", ipc_out_flush(sv[0], &o));
		check("...and reports the rest still pending", ipc_out_pending(&o));
		check("...having written less than the whole payload", o.off < N);

		/* Now alternate reading and flushing, as the event loop would. */
		char *got = malloc(N);
		size_t have = 0;
		int spins = 0;
		while (ipc_out_pending(&o) && spins++ < 10000) {
			ssize_t r = recv(sv[1], got + have, N - have, 0);
			if (r > 0)
				have += (size_t)r;
			if (!ipc_out_flush(sv[0], &o)) {
				check("flush kept succeeding", false);
				break;
			}
		}
		check("the queue drains", !ipc_out_pending(&o));
		have += drain(sv[1], got + have, N - have);

		check("every byte arrives", have == N);
		check("and in the right order", have == N && !memcmp(got, payload, N));

		free(got);
		free(payload);
		close(sv[0]);
		close(sv[1]);
		ipc_out_reset(&o);
	}

	/* A small reply -- everything the IPC served before this change -- still
	 * goes out in one flush and leaves nothing queued. */
	{
		int sv[2];
		make_pair(sv, 4096);
		struct ipc_out o = {0};
		const char *msg = "{\"success\":true}\n";
		ipc_out_append(&o, msg, strlen(msg));
		check("a small reply flushes completely", ipc_out_flush(sv[0], &o));
		check("...leaving nothing pending", !ipc_out_pending(&o));
		check("...and freeing the queue", o.buf == NULL);

		char buf[64] = {0};
		ssize_t r = recv(sv[1], buf, sizeof(buf) - 1, 0);
		check("...and arriving verbatim",
			  r == (ssize_t)strlen(msg) && !strcmp(buf, msg));
		close(sv[0]);
		close(sv[1]);
		ipc_out_reset(&o);
	}

	/* Several messages queued before anything is flushed -- a watcher notified
	 * twice in one event-loop cycle -- concatenate in order. */
	{
		int sv[2];
		make_pair(sv, 4096);
		struct ipc_out o = {0};
		ipc_out_append(&o, "one\n", 4);
		ipc_out_append(&o, "two\n", 4);
		ipc_out_append(&o, "three\n", 6);
		ipc_out_flush(sv[0], &o);
		char buf[64] = {0};
		recv(sv[1], buf, sizeof(buf) - 1, 0);
		check("queued messages keep their order",
			  !strcmp(buf, "one\ntwo\nthree\n"));
		close(sv[0]);
		close(sv[1]);
		ipc_out_reset(&o);
	}

	/* Appending after a full drain must reuse the queue rather than grow it
	 * forever: a long-lived watcher appends thousands of times. */
	{
		int sv[2];
		make_pair(sv, 65536);
		struct ipc_out o = {0};
		char sink[256];
		for (int i = 0; i < 1000; i++) {
			ipc_out_append(&o, "tick\n", 5);
			ipc_out_flush(sv[0], &o);
			recv(sv[1], sink, sizeof(sink), 0);
		}
		check("a drained queue is reused, not grown", o.len <= 5);
		close(sv[0]);
		close(sv[1]);
		ipc_out_reset(&o);
	}

	/* The runaway guard: a client that never reads is refused, not fed. */
	{
		int sv[2];
		make_pair(sv, 4096);
		struct ipc_out o = {0};
		size_t chunk = 256 * 1024;
		char *big = calloc(1, chunk);
		bool refused = false;
		for (int i = 0; i < 64; i++) {
			if (!ipc_out_append(&o, big, chunk)) {
				refused = true;
				break;
			}
			ipc_out_flush(sv[0], &o); /* peer never reads; fills up */
		}
		check("a backlog past IPC_OUT_MAX is refused", refused);
		check("...and the queue never exceeded the cap",
			  o.len - o.off <= IPC_OUT_MAX);
		free(big);
		close(sv[0]);
		close(sv[1]);
		ipc_out_reset(&o);
	}

	/* A peer that has gone away is reported, not signalled. Without
	 * MSG_NOSIGNAL this raises SIGPIPE, and nothing in the compositor installs
	 * a handler -- a client exiting between asking and reading would take the
	 * whole session down. If this test dies rather than fails, that is the
	 * flag having been dropped. */
	{
		int sv[2];
		make_pair(sv, 4096);
		close(sv[1]);
		struct ipc_out o = {0};
		char blob[8192];
		memset(blob, 'x', sizeof(blob));
		ipc_out_append(&o, blob, sizeof(blob));
		bool ok = ipc_out_flush(sv[0], &o);
		check("a dead peer is reported through the return value", !ok);
		close(sv[0]);
		ipc_out_reset(&o);
	}

	/* Reset is safe on a queue that was never used. */
	{
		struct ipc_out o = {0};
		ipc_out_reset(&o);
		check("reset on an empty queue is a no-op", o.buf == NULL && o.len == 0);
	}

	printf("\n%s\n", failures ? "FAILED" : "all ipc-out tests passed");
	return failures ? 1 : 0;
}
