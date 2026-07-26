#ifndef ASTEROIDZ_BAR_VPN_H
#define ASTEROIDZ_BAR_VPN_H

/* Defined in bar-popover.h, which is included after this file -- the two are
 * one translation unit, so a forward declaration is all this needs. */
static void bar_popover_vpn_countries_arrived(void);

/* NordVPN connection state, from the `nordvpn` CLI.
 *
 * Icon only, with the COLOUR carrying the state -- the same redesign the
 * sysinfo and discord pills follow:
 *
 *   connected     theme accent
 *   connecting    amber
 *   disconnected  foreground, dimmed (an unlit indicator)
 *   no CLI/daemon urgent
 *
 * Polled rather than event-driven, because the CLI is all there is: nordvpn
 * exposes no bus interface and no socket a client may subscribe to. The poll
 * is an async spawn on the event loop and runs on the shared metrics timer, so
 * it costs one short-lived child every `interval` seconds and never blocks --
 * the waybar plugin does the same thing for the same reason.
 *
 * A missing binary degrades to the urgent icon rather than an error: the
 * module is opt-in, and someone who configures it without nordvpn installed
 * should see that plainly rather than have the bar stay silent.
 */

typedef enum {
	BAR_VPN_DISCONNECTED = 0,
	BAR_VPN_CONNECTING,
	BAR_VPN_CONNECTED,
	BAR_VPN_UNAVAILABLE, /* no CLI, or a daemon that answered nothing usable */
} BarVpnState;

#define BAR_VPN_MAX_COUNTRIES 24

static struct {
	BarVpnState state;
	char country[64];
	char server[96];
	char ip[64];
	char uptime[64];
	/* country list for the popover, from `nordvpn countries` */
	char countries[BAR_VPN_MAX_COUNTRIES][48];
	int32_t ncountries;
	bool in_flight;
	bool have;
} bar_vpn;

/* Pull "Key: value" out of the CLI's listing. It bullets some lines with
 * "- ", and spells the separator with and without a leading space. */
static void bar_vpn_field(const char *text, const char *key, char *out,
						  size_t len) {
	if (len)
		out[0] = '\0';
	if (!text || !key)
		return;
	size_t klen = strlen(key);
	for (const char *p = text; p && *p;) {
		const char *eol = strchr(p, '\n');
		size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
		const char *k = p;
		while (linelen && (*k == '-' || *k == ' ' || *k == '\t')) {
			k++;
			linelen--;
		}
		if (linelen > klen && strncasecmp(k, key, klen) == 0) {
			const char *colon = memchr(k, ':', linelen);
			if (colon) {
				const char *v = colon + 1;
				while (*v == ' ' || *v == '\t')
					v++;
				size_t vlen = linelen - (size_t)(v - k);
				while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == ' '))
					vlen--;
				if (vlen >= len)
					vlen = len - 1;
				memcpy(out, v, vlen);
				out[vlen] = '\0';
				return;
			}
		}
		p = eol ? eol + 1 : NULL;
	}
}

static void bar_vpn_on_status(const char *out, size_t len, void *user) {
	(void)len;
	(void)user;
	bar_vpn.in_flight = false;

	BarVpnState was = bar_vpn.state;
	char st[64];
	bar_vpn_field(out, "Status", st, sizeof(st));
	if (!st[0]) {
		/* No Status line at all: either the binary is missing (we got nothing
		 * on stdout) or the daemon is not answering. Both are "cannot tell
		 * you", which is worth showing rather than reporting Disconnected. */
		bar_vpn.state = BAR_VPN_UNAVAILABLE;
		bar_vpn.country[0] = bar_vpn.server[0] = bar_vpn.ip[0] = '\0';
		bar_vpn.uptime[0] = '\0';
	} else if (strcasecmp(st, "Connected") == 0) {
		bar_vpn.state = BAR_VPN_CONNECTED;
		bar_vpn_field(out, "Country", bar_vpn.country, sizeof(bar_vpn.country));
		bar_vpn_field(out, "Hostname", bar_vpn.server, sizeof(bar_vpn.server));
		if (!bar_vpn.server[0])
			bar_vpn_field(out, "Server", bar_vpn.server, sizeof(bar_vpn.server));
		bar_vpn_field(out, "IP", bar_vpn.ip, sizeof(bar_vpn.ip));
		bar_vpn_field(out, "Uptime", bar_vpn.uptime, sizeof(bar_vpn.uptime));
	} else if (strcasecmp(st, "Connecting") == 0 ||
			   strcasecmp(st, "Reconnecting") == 0) {
		bar_vpn.state = BAR_VPN_CONNECTING;
	} else {
		bar_vpn.state = BAR_VPN_DISCONNECTED;
		bar_vpn.country[0] = bar_vpn.server[0] = bar_vpn.ip[0] = '\0';
		bar_vpn.uptime[0] = '\0';
	}
	bar_vpn.have = true;
	if (was != bar_vpn.state)
		bar_update_all();
}

static void bar_vpn_on_countries(const char *out, size_t len, void *user) {
	(void)len;
	(void)user;
	bar_vpn.ncountries = 0;
	/* the CLI prints a comma/whitespace separated list, sometimes in columns */
	const char *p = out;
	while (*p && bar_vpn.ncountries < BAR_VPN_MAX_COUNTRIES) {
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',' || *p == '\r')
			p++;
		const char *start = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != ',' &&
			   *p != '\r')
			p++;
		size_t n = (size_t)(p - start);
		if (n == 0)
			continue;
		if (n >= sizeof(bar_vpn.countries[0]))
			n = sizeof(bar_vpn.countries[0]) - 1;
		/* the CLI decorates its output with box characters on some versions */
		if (start[0] == '-' || start[0] == '|')
			continue;
		memcpy(bar_vpn.countries[bar_vpn.ncountries], start, n);
		bar_vpn.countries[bar_vpn.ncountries][n] = '\0';
		bar_vpn.ncountries++;
	}

	/* Redraw the menu that asked for this.
	 *
	 * The list is fetched when the popover opens and arrives a subprocess
	 * later, so the FIRST open always rendered before it -- one "Quick
	 * Connect" row and no countries -- and only a second open showed the
	 * real menu. Nothing was broken except that nobody told the popover its
	 * content had turned up. */
	if (bar_vpn.ncountries > 0)
		bar_popover_vpn_countries_arrived();
}

static void bar_vpn_poll(void) {
	if (bar_vpn.in_flight)
		return;
	char *const argv[] = {"nordvpn", "status", NULL};
	bar_vpn.in_flight = async_spawn(event_loop, argv, bar_vpn_on_status, NULL);
	if (!bar_vpn.in_flight && !bar_vpn.have) {
		/* fork failed outright -- no binary, no PATH */
		bar_vpn.state = BAR_VPN_UNAVAILABLE;
		bar_vpn.have = true;
	}
}

static void bar_vpn_fetch_countries(void) {
	if (bar_vpn.ncountries > 0)
		return;
	char *const argv[] = {"nordvpn", "countries", NULL};
	async_spawn(event_loop, argv, bar_vpn_on_countries, NULL);
}

static void bar_vpn_connect(const char *country) {
	if (country && *country) {
		char *const argv[] = {"nordvpn", "connect", (char *)country, NULL};
		async_spawn(event_loop, argv, NULL, NULL);
	} else {
		char *const argv[] = {"nordvpn", "connect", NULL};
		async_spawn(event_loop, argv, NULL, NULL);
	}
	/* the next poll reports the outcome; show intent immediately so the click
	 * visibly did something on a link that takes seconds to come up */
	bar_vpn.state = BAR_VPN_CONNECTING;
	bar_update_all();
}

static void bar_vpn_disconnect(void) {
	char *const argv[] = {"nordvpn", "disconnect", NULL};
	async_spawn(event_loop, argv, NULL, NULL);
	bar_vpn.state = BAR_VPN_DISCONNECTED;
	bar_update_all();
}

#endif /* ASTEROIDZ_BAR_VPN_H */
