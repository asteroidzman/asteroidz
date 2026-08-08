// wlsandbox — connects through security-context-v1 and reports the globals the
// compositor is willing to show a sandboxed client.
//
// What it exists to catch: the privileged-global deny list in modern.h is a
// table of interface-name STRINGS compared with strcmp against
// wl_global_get_interface()->name. A name that doesn't match any real global is
// not an error anywhere -- it is simply a line that never fires. One such entry
// ("wlr_export_dmabuf_manager_v1", missing the zwlr_ prefix) sat in that list
// silently handing full-screen capture to every Flatpak, and nothing in the
// build, the tests, or the compositor's own logs could have noticed. Only
// enumerating the registry from the far side of a real security context can.
//
// So this asserts the whole list, not one entry: a test greps the output for
// any interface it expects to be denied.
//
// How it works: create a listening unix socket inside XDG_RUNTIME_DIR, hand it
// to wp_security_context_v1 as the listen fd, then connect a SECOND wl_display
// to that socket by name. The compositor treats everything arriving on it as
// sandboxed.
//
// Output, one line each, flushed:
//   sandboxed <interface> <version>    a global the sandboxed client CAN see
//   done <n>                           end of registry, n globals visible
//
// Usage: wlsandbox [socket_name]      (default: wlsandbox-probe)
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
#include "security-context-v1-client-protocol.h"

static struct wp_security_context_manager_v1 *sec_mgr = NULL;
static int global_count = 0;

static void outer_global(void *data, struct wl_registry *registry, uint32_t name,
						 const char *interface, uint32_t version) {
	(void)data; (void)version;
	if (!strcmp(interface, wp_security_context_manager_v1_interface.name))
		sec_mgr = wl_registry_bind(registry, name,
								   &wp_security_context_manager_v1_interface, 1);
}
static void outer_global_remove(void *data, struct wl_registry *r, uint32_t n) {
	(void)data; (void)r; (void)n;
}
static const struct wl_registry_listener outer_listener = {
	.global = outer_global,
	.global_remove = outer_global_remove,
};

static void inner_global(void *data, struct wl_registry *registry, uint32_t name,
						 const char *interface, uint32_t version) {
	(void)data; (void)registry; (void)name;
	printf("sandboxed %s %u\n", interface, version);
	fflush(stdout);
	global_count++;
}
static void inner_global_remove(void *data, struct wl_registry *r, uint32_t n) {
	(void)data; (void)r; (void)n;
}
static const struct wl_registry_listener inner_listener = {
	.global = inner_global,
	.global_remove = inner_global_remove,
};

int main(int argc, char **argv) {
	const char *sock_name = argc > 1 ? argv[1] : "wlsandbox-probe";
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) {
		fprintf(stderr, "wlsandbox: XDG_RUNTIME_DIR unset\n");
		return 1;
	}

	struct wl_display *outer = wl_display_connect(NULL);
	if (!outer) {
		fprintf(stderr, "wlsandbox: cannot connect to WAYLAND_DISPLAY\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(outer);
	wl_registry_add_listener(registry, &outer_listener, NULL);
	wl_display_roundtrip(outer);

	if (!sec_mgr) {
		fprintf(stderr, "wlsandbox: no wp_security_context_manager_v1\n");
		return 1;
	}

	// the socket the sandboxed client will arrive on. Built straight into
	// sun_path, which is ~108 bytes -- a silently truncated path binds the
	// wrong socket and the connect below then fails for no visible reason.
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int n = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s",
					 runtime_dir, sock_name);
	if (n < 0 || (size_t)n >= sizeof(addr.sun_path)) {
		fprintf(stderr, "wlsandbox: socket path too long for sun_path\n");
		return 1;
	}
	const char *path = addr.sun_path;
	unlink(path);

	int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listen_fd < 0) { perror("socket"); return 1; }
	if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind"); return 1;
	}
	if (listen(listen_fd, 8) < 0) { perror("listen"); return 1; }

	// close_fd: the compositor drops the context when this hits EOF. Held open
	// for the life of this process, so the write end must NOT be closed here.
	int close_fds[2];
	if (pipe(close_fds) < 0) { perror("pipe"); return 1; }

	struct wp_security_context_v1 *ctx =
		wp_security_context_manager_v1_create_listener(sec_mgr, listen_fd,
													  close_fds[0]);
	wp_security_context_v1_set_sandbox_engine(ctx, "org.asteroidz.test");
	wp_security_context_v1_set_app_id(ctx, "org.asteroidz.wlsandbox");
	wp_security_context_v1_set_instance_id(ctx, "wlsandbox-1");
	wp_security_context_v1_commit(ctx);
	wl_display_roundtrip(outer);

	close(listen_fd);
	close(close_fds[0]);

	// now connect as the sandboxed client and enumerate what we're shown
	struct wl_display *inner = wl_display_connect(sock_name);
	if (!inner) {
		fprintf(stderr, "wlsandbox: cannot connect to sandbox socket %s\n", path);
		return 1;
	}
	struct wl_registry *inner_registry = wl_display_get_registry(inner);
	wl_registry_add_listener(inner_registry, &inner_listener, NULL);
	wl_display_roundtrip(inner);

	printf("done %d\n", global_count);
	fflush(stdout);

	wl_display_disconnect(inner);
	close(close_fds[1]);
	wl_display_disconnect(outer);
	unlink(path);
	return 0;
}
