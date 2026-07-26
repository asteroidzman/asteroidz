#ifndef ASTEROIDZ_BAR_DISCORD_H
#define ASTEROIDZ_BAR_DISCORD_H

/* Discord voice status, from the standalone `discord-voiced` daemon.
 *
 * ALL Discord, audio and tokio work stays in that daemon; this is a thin IPC
 * client exactly as the waybar plugin is. It reads newline-JSON events from
 * $XDG_RUNTIME_DIR/discord-voiced.sock and writes newline-JSON commands back.
 * Nothing here touches songbird, DAVE or an audio thread -- running that
 * inside a bar process has crashed a session before, and running it inside the
 * COMPOSITOR would take the whole desktop with it.
 *
 * With no daemon the pill reads "Offline" and the client retries with backoff,
 * so the module is harmless on a machine that never runs it.
 *
 * Events consumed (the daemon's protocol):
 *   status   state=idle|connecting|connected, channel=<id>, muted=<bool>
 *   channels guilds[] + channels[], for the join popover
 *   ready    username
 *   ptt      active=<bool>, push-to-talk held
 *   error    text, a hard failure worth showing (e.g. an expired token)
 */

#include "../common/unix-line-client.h"

#define BAR_DV_MAX_CHANNELS 24

typedef enum {
	BAR_DV_OFFLINE = 0,
	BAR_DV_IDLE,
	BAR_DV_CONNECTING,
	BAR_DV_CONNECTED,
} BarDvState;

typedef struct {
	char id[48];
	char guild[48];
	char name[64];
	char guild_name[64];
	int32_t people;
} BarDvChannel;

static struct {
	BarDvState state;
	char channel_id[48];
	char channel_name[64];
	char username[64];
	char error[128];
	bool muted;
	bool ptt_active;
	BarDvChannel channels[BAR_DV_MAX_CHANNELS];
	int32_t nchannels;
	bool started;
} bar_dv;

static UnixLineClient bar_dv_client;

static BarDvState bar_dv_state_from(const char *s) {
	if (!s)
		return BAR_DV_OFFLINE;
	if (!strcmp(s, "idle"))
		return BAR_DV_IDLE;
	if (!strcmp(s, "connecting"))
		return BAR_DV_CONNECTING;
	if (!strcmp(s, "connected"))
		return BAR_DV_CONNECTED;
	return BAR_DV_OFFLINE;
}

static const char *bar_dv_json_str(cJSON *o, const char *key,
								   const char *fallback) {
	cJSON *v = cJSON_GetObjectItem(o, key);
	return cJSON_IsString(v) && v->valuestring ? v->valuestring : fallback;
}

/* Resolve the joined channel's display name from the last channels snapshot,
 * so the pill can say where you are rather than printing an opaque id. */
static void bar_dv_resolve_channel_name(void) {
	bar_dv.channel_name[0] = '\0';
	if (!bar_dv.channel_id[0])
		return;
	for (int32_t i = 0; i < bar_dv.nchannels; i++) {
		if (strcmp(bar_dv.channels[i].id, bar_dv.channel_id) == 0) {
			snprintf(bar_dv.channel_name, sizeof(bar_dv.channel_name), "%s",
					 bar_dv.channels[i].name);
			return;
		}
	}
}

static void bar_dv_on_line(const char *line, void *user) {
	(void)user;
	cJSON *root = cJSON_Parse(line);
	if (!root)
		return;
	const char *ev = bar_dv_json_str(root, "event", NULL);
	if (!ev) {
		cJSON_Delete(root);
		return;
	}

	if (!strcmp(ev, "status")) {
		bar_dv.state = bar_dv_state_from(bar_dv_json_str(root, "state", NULL));
		snprintf(bar_dv.channel_id, sizeof(bar_dv.channel_id), "%s",
				 bar_dv_json_str(root, "channel", ""));
		cJSON *m = cJSON_GetObjectItem(root, "muted");
		bar_dv.muted = cJSON_IsTrue(m);
		/* mute is only meaningful while actually in a channel */
		if (bar_dv.state != BAR_DV_CONNECTED)
			bar_dv.muted = false;
		bar_dv.error[0] = '\0'; /* a real status means the daemon is healthy */
		bar_dv_resolve_channel_name();
	} else if (!strcmp(ev, "channels")) {
		cJSON *guilds = cJSON_GetObjectItem(root, "guilds");
		cJSON *chans = cJSON_GetObjectItem(root, "channels");
		bar_dv.nchannels = 0;
		cJSON *c = NULL;
		cJSON_ArrayForEach(c, chans) {
			if (bar_dv.nchannels >= BAR_DV_MAX_CHANNELS)
				break;
			BarDvChannel *out = &bar_dv.channels[bar_dv.nchannels];
			snprintf(out->id, sizeof(out->id), "%s",
					 bar_dv_json_str(c, "id", ""));
			snprintf(out->guild, sizeof(out->guild), "%s",
					 bar_dv_json_str(c, "guild", ""));
			snprintf(out->name, sizeof(out->name), "%s",
					 bar_dv_json_str(c, "name", ""));
			cJSON *people = cJSON_GetObjectItem(c, "people");
			out->people = cJSON_IsArray(people) ? cJSON_GetArraySize(people)
												: 0;
			/* the guild's display name, for the popover's row prefix */
			out->guild_name[0] = '\0';
			cJSON *g = NULL;
			cJSON_ArrayForEach(g, guilds) {
				if (strcmp(bar_dv_json_str(g, "id", ""), out->guild) == 0) {
					snprintf(out->guild_name, sizeof(out->guild_name), "%s",
							 bar_dv_json_str(g, "name", ""));
					break;
				}
			}
			if (out->id[0] && out->name[0])
				bar_dv.nchannels++;
		}
		bar_dv_resolve_channel_name();
	} else if (!strcmp(ev, "ready")) {
		snprintf(bar_dv.username, sizeof(bar_dv.username), "%s",
				 bar_dv_json_str(root, "username", ""));
		bar_dv.error[0] = '\0';
	} else if (!strcmp(ev, "ptt")) {
		bar_dv.ptt_active = cJSON_IsTrue(cJSON_GetObjectItem(root, "active"));
	} else if (!strcmp(ev, "error")) {
		snprintf(bar_dv.error, sizeof(bar_dv.error), "%s",
				 bar_dv_json_str(root, "text", ""));
	}
	cJSON_Delete(root);
	bar_update_all();
}

static void bar_dv_on_state(bool connected, void *user) {
	(void)user;
	if (connected)
		return;
	/* The daemon is gone: everything we knew is stale. Showing the last
	 * channel would claim a voice connection that no longer exists. */
	bar_dv.state = BAR_DV_OFFLINE;
	bar_dv.channel_id[0] = '\0';
	bar_dv.channel_name[0] = '\0';
	bar_dv.username[0] = '\0';
	bar_dv.muted = false;
	bar_dv.ptt_active = false;
	bar_dv.nchannels = 0;
	bar_update_all();
}

static void bar_dv_start(void) {
	if (bar_dv.started)
		return;
	bar_dv.started = true;
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char path[256];
	snprintf(path, sizeof(path), "%s/discord-voiced.sock",
			 rt && *rt ? rt : "/tmp");
	unix_line_start(&bar_dv_client, event_loop, path, bar_dv_on_line,
					bar_dv_on_state, NULL);
}

static void bar_dv_finish(void) {
	if (!bar_dv.started)
		return;
	unix_line_stop(&bar_dv_client);
	bar_dv.started = false;
}

static void bar_dv_send(const char *cmd) {
	unix_line_send(&bar_dv_client, cmd);
}

/* What the pill says: where you are if connected, otherwise what the daemon is
 * doing. Matches the waybar plugin's wording so the two read identically while
 * both are installed. */
static const char *bar_dv_label(void) {
	if (bar_dv.error[0])
		return "Error";
	switch (bar_dv.state) {
	case BAR_DV_CONNECTED:
		return bar_dv.channel_name[0] ? bar_dv.channel_name : "Connected";
	case BAR_DV_CONNECTING:
		return "Connecting";
	case BAR_DV_IDLE:
		return "Idle";
	default:
		return "Offline";
	}
}

#endif /* ASTEROIDZ_BAR_DISCORD_H */
