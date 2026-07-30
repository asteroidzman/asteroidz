#ifndef ASTEROIDZ_IPC_CAPTURE_H
#define ASTEROIDZ_IPC_CAPTURE_H

/* `capture-chord`: press a key combination, get it back as a chord string.
 *
 * A keybind editor that makes you TYPE "Super+Shift+Q" is asking you to know
 * three things it could have found out: which modifier names this compositor
 * accepts, what the key is called (`Return`, not `Enter`; `XF86AudioRaiseVolume`,
 * not `VolumeUp`), and whether the thing you pressed is even nameable. Pressing
 * the keys answers all three at once.
 *
 * It has to happen HERE and not in the client. The compositor takes bindings
 * before the focused surface sees them, so a settings window trying to read the
 * chord itself would receive everything EXCEPT the combinations that are already
 * bound -- which is precisely the set you reach for when rebinding. Super+Q would
 * close a window instead of being captured.
 *
 * A deferred reply, not a watch. The request arrives, nothing is sent, and the
 * reply goes out when a key is pressed -- so `amsg capture-chord` blocks until you
 * press something and then prints it, and a client's ordinary request/reply code
 * works unchanged. That the reply can be deferred at all is a property of the
 * output queue: replies are queued and the connection closes when the queue
 * drains, not when the handler returns.
 *
 * Modelled on the screenshot overlay's exclusive keyboard grab, which is the
 * other place the compositor swallows keys wholesale.
 */

static struct {
	bool active;
	int32_t fd;
	/* Which client is waiting. Compared on cleanup, because a file descriptor
	 * number is reused the moment it is closed -- writing to `fd` after the
	 * requester vanished would deliver a chord to whoever inherited it. */
	struct ipc_client_state *client;
} chord_capture;

static void chord_capture_cancel(struct ipc_client_state *c) {
	if (chord_capture.active && chord_capture.client == c) {
		chord_capture.active = false;
		chord_capture.client = NULL;
		chord_capture.fd = -1;
	}
}

/* Is this keysym a bare modifier?
 *
 * Pressing Super and holding it is not a chord, it is the beginning of one, so
 * capture keeps waiting. Without this the first modifier down would be captured
 * instantly and every chord would come out as "Super".
 */
static bool chord_sym_is_modifier(xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
	case XKB_KEY_Hyper_L:
	case XKB_KEY_Hyper_R:
	case XKB_KEY_ISO_Level3_Shift:
	case XKB_KEY_ISO_Level5_Shift:
	case XKB_KEY_Caps_Lock:
	case XKB_KEY_Num_Lock:
		return true;
	default:
		return false;
	}
}

/* Format a modifier mask and a keysym as the chord a `binds` block wants.
 *
 * The names are the ones kdl_mod_name normalises to, and the key name is
 * whatever xkb calls it -- which is exactly what parse_key reads back, since it
 * looks the name up with xkb_keysym_from_name. So the string this produces
 * round-trips through the parser by construction rather than by a table that has
 * to be kept in step.
 */
static void chord_format(uint32_t mods, xkb_keysym_t sym, uint32_t keycode,
						 char *out, size_t cap) {
	const struct {
		uint32_t bit;
		const char *name;
	} names[] = {
		{WLR_MODIFIER_LOGO, "SUPER"},
		{WLR_MODIFIER_CTRL, "CTRL"},
		{WLR_MODIFIER_ALT, "ALT"},
		{WLR_MODIFIER_SHIFT, "SHIFT"},
		{WLR_MODIFIER_MOD3, "HYPER"},
	};
	size_t o = 0;
	out[0] = '\0';
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (mods & names[i].bit)
			o += (size_t)snprintf(out + o, cap - o, "%s+", names[i].name);

	char key[128];
	if (xkb_keysym_get_name(sym, key, sizeof(key)) <= 0)
		key[0] = '\0';
	if (!key[0] || !strcmp(key, "NoSymbol"))
		/* Nameable keys are the overwhelming majority, but a key with no keysym
		 * under the current layout still has a keycode -- and `code:N` is a
		 * spelling parse_key already accepts, so it stays capturable instead of
		 * being an error the user cannot act on. */
		snprintf(out + o, cap - o, "code:%u", keycode);
	else
		snprintf(out + o, cap - o, "%s", key);
}

/* Called from keypress() before anything else looks at the key. Returns true if
 * the event was consumed, which the caller must honour -- a captured Super+Q that
 * also ran kill_client would be a keybind editor that closes the window you are
 * editing from. */
static bool chord_capture_handle(uint32_t state, uint32_t mods,
								 xkb_keysym_t sym, uint32_t keycode) {
	if (!chord_capture.active)
		return false;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		/* Swallowed anyway. Letting the release through would deliver a key-up
		 * with no matching key-down to whatever has focus, which some toolkits
		 * treat as a stuck modifier. */
		return true;
	if (chord_sym_is_modifier(sym))
		return true;

	cJSON *resp = cJSON_CreateObject();
	if (sym == XKB_KEY_Escape && !mods) {
		/* The way out. Unmodified Escape only, so Shift+Escape and Super+Escape
		 * are still capturable chords. */
		cJSON_AddBoolToObject(resp, "ok", false);
		cJSON_AddStringToObject(resp, "error", "cancelled");
	} else {
		char chord[256];
		chord_format(mods, sym, keycode, chord, sizeof(chord));
		cJSON_AddBoolToObject(resp, "ok", true);
		cJSON_AddStringToObject(resp, "chord", chord);
	}

	char *text = cJSON_PrintUnformatted(resp);
	cJSON_Delete(resp);
	if (text) {
		ipc_capture_reply(chord_capture.client, text);
		free(text);
	}
	chord_capture.active = false;
	chord_capture.client = NULL;
	chord_capture.fd = -1;
	return true;
}

#endif /* ASTEROIDZ_IPC_CAPTURE_H */
