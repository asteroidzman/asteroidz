#ifndef ASTEROIDZ_BAR_MEDICATION_H
#define ASTEROIDZ_BAR_MEDICATION_H

/* Medication reminders, reading the same store the waybar plugin writes:
 * $XDG_STATE_HOME/waybar-medication/medications.json.
 *
 * The bar now OWNS this store: waybar is gone, so the two-writer hazard that
 * kept this read-only is gone with it. Taking, skipping and postponing a dose
 * all write here.
 *
 * Written the way the plugin wrote it, field for field, because the file
 * outlives the program that made it -- a store this rewrites into a shape the
 * plugin could not read would strand the history if anyone ever went back.
 * Saved through a temporary file and rename(), which is atomic on the same
 * filesystem: "did I take it?" is exactly the question a half-written file
 * must never be able to answer wrongly.
 *
 * The schedule is reproduced exactly rather than approximated, because a pill
 * that disagrees with the plugin about what is due is worse than no pill:
 *
 *   frequencyUnit "days"  -- every `frequencyValue` days from startDate, at
 *                            each listed HH:MM
 *   frequencyUnit "hours" -- every `frequencyValue` hours from the first
 *                            listed time, daily
 *
 * A dose is DUE from its scheduled time until six hours later, unless its
 * doseState says taken or skipped.
 *
 * bar_med_reload() is called FROM the module refresh, so it must never call
 * bar_update_all() itself: that re-enters the refresh, which reloads, which
 * updates again -- an infinite recursion that wedged the compositor on the
 * first frame. It only updates state; the digest covers these fields, so the
 * next metrics tick renders any change. Every other module calls
 * bar_update_all() from a bus or timer callback, never from inside a refresh.
 */

#define BAR_MED_DUE_WINDOW (6 * 3600)
#define BAR_MED_MAX 32

typedef struct {
	char name[64];
	char time[8];  /* HH:MM */
	/* the plugin's dose key, medId@YYYY-MM-DDTHH:MM -- carried so a popover
	 * row can act on a specific dose rather than re-deriving it from a name
	 * and a time that two medications could share */
	char key[176];
	char status[16]; /* "", "taken", "skipped" */
	time_t scheduled;
	time_t postponed_until; /* 0 when not postponed */
	bool due;      /* past its time, inside the window, not taken/skipped */
	bool pending;  /* still to come today */
} BarMedDose;

static struct {
	BarMedDose doses[BAR_MED_MAX];
	int32_t ndoses;
	int32_t ndue;
	char next_time[8]; /* HH:MM of the next pending dose, empty if none */
	char due_name[64]; /* the single due dose's name, when there is exactly one */
	time_t mtime;      /* store mtime we last parsed */
	bool have;
} bar_med;

static void bar_med_path(char *out, size_t len) {
	const char *state = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	if (state && *state)
		snprintf(out, len, "%s/waybar-medication/medications.json", state);
	else
		snprintf(out, len, "%s/.local/state/waybar-medication/medications.json",
				 home ? home : "");
}

/* local midnight of a YYYY-MM-DD date string */
static time_t bar_med_midnight(const char *day) {
	struct tm tm = {0};
	if (!day || sscanf(day, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3)
		return 0;
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	tm.tm_isdst = -1;
	return mktime(&tm);
}

static time_t bar_med_at(const char *day, const char *hhmm) {
	struct tm tm = {0};
	int hh = 0, mm = 0;
	if (!day || sscanf(day, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3)
		return 0;
	sscanf(hhmm ? hhmm : "00:00", "%d:%d", &hh, &mm);
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	tm.tm_hour = hh;
	tm.tm_min = mm;
	tm.tm_isdst = -1;
	return mktime(&tm);
}

static const char *bar_med_str(cJSON *o, const char *key, const char *fallback) {
	cJSON *v = cJSON_GetObjectItem(o, key);
	return cJSON_IsString(v) && v->valuestring ? v->valuestring : fallback;
}

/* Does this medication have a dose on `day`? Mirrors scheduled_on(). */
static bool bar_med_scheduled_on(cJSON *med, const char *day,
								 const char *unit, int32_t freq) {
	cJSON *en = cJSON_GetObjectItem(med, "enabled");
	if (cJSON_IsBool(en) && !cJSON_IsTrue(en))
		return false;
	if (strcmp(unit, "hours") == 0)
		return true; /* hourly schedules run every day */
	if (freq <= 1)
		return true;
	const char *start = bar_med_str(med, "startDate", NULL);
	if (!start || !*start)
		return true;
	time_t s = bar_med_midnight(start);
	time_t d = bar_med_midnight(day);
	if (!s || !d)
		return true;
	long days = lround((double)(d - s) / 86400.0);
	return days >= 0 && (days % freq) == 0;
}

static void bar_med_reload(void) {
	char path[512];
	bar_med_path(path, sizeof(path));

	struct stat st;
	if (stat(path, &st) != 0) {
		if (bar_med.have)
			memset(&bar_med, 0, sizeof(bar_med));
		return;
	}
	/* the plugin rewrites the whole document on every action; re-read only
	 * when it actually changed */
	bool changed = st.st_mtime != bar_med.mtime;

	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	char day[16];
	strftime(day, sizeof(day), "%Y-%m-%d", &lt);

	static char cached_day[16];
	if (!changed && bar_med.have && strcmp(cached_day, day) == 0) {
		/* nothing re-read, but due/pending still move with the clock */
		int32_t ndue = 0;
		const char *next = NULL;
		const char *due_name = NULL;
		for (int32_t i = 0; i < bar_med.ndoses; i++) {
			BarMedDose *d = &bar_med.doses[i];
			d->due = d->due &&
					 now - d->scheduled <= BAR_MED_DUE_WINDOW;
			if (!d->due && d->pending && d->scheduled <= now) {
				d->pending = false;
				d->due = true;
			}
			if (d->due) {
				ndue++;
				due_name = d->name;
			}
			if (d->pending && d->scheduled > now && !next)
				next = d->time;
		}
		if (ndue == bar_med.ndue)
			return; /* nothing visible moved */
		bar_med.ndue = ndue;
		snprintf(bar_med.next_time, sizeof(bar_med.next_time), "%s",
				 next ? next : "");
		snprintf(bar_med.due_name, sizeof(bar_med.due_name), "%s",
				 ndue == 1 && due_name ? due_name : "");
		return;
	}
	snprintf(cached_day, sizeof(cached_day), "%s", day);
	bar_med.mtime = st.st_mtime;

	FILE *f = fopen(path, "re");
	if (!f)
		return;
	if (st.st_size <= 0 || st.st_size > 4 * 1024 * 1024) {
		fclose(f);
		return;
	}
	char *buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		fclose(f);
		return;
	}
	size_t got = fread(buf, 1, (size_t)st.st_size, f);
	fclose(f);
	buf[got] = '\0';
	cJSON *root = cJSON_Parse(buf);
	free(buf);
	if (!root)
		return;

	cJSON *meds = cJSON_GetObjectItem(root, "medications");
	cJSON *state = cJSON_GetObjectItem(root, "doseState");

	bar_med.ndoses = 0;
	bar_med.ndue = 0;
	bar_med.next_time[0] = '\0';
	bar_med.due_name[0] = '\0';
	const char *next = NULL;
	const char *due_name = NULL;

	cJSON *med = NULL;
	cJSON_ArrayForEach(med, meds) {
		if (bar_med.ndoses >= BAR_MED_MAX)
			break;
		const char *unit = bar_med_str(med, "frequencyUnit", "days");
		cJSON *fv = cJSON_GetObjectItem(med, "frequencyValue");
		int32_t freq = cJSON_IsNumber(fv) ? (int32_t)fv->valuedouble : 1;
		if (freq < 1)
			freq = 1;
		if (!bar_med_scheduled_on(med, day, unit, freq))
			continue;
		const char *name = bar_med_str(med, "name", "");
		const char *id = bar_med_str(med, "id", "");
		cJSON *times = cJSON_GetObjectItem(med, "times");

		/* the times this medication is actually taken today */
		char slots[BAR_MED_MAX][8];
		int32_t nslots = 0;
		if (strcmp(unit, "hours") == 0) {
			cJSON *first = cJSON_GetArrayItem(times, 0);
			int32_t h0 = 8, m0 = 0;
			if (cJSON_IsString(first) && first->valuestring)
				sscanf(first->valuestring, "%d:%d", &h0, &m0);
			for (int32_t h = h0; h < 24 && nslots < BAR_MED_MAX; h += freq)
				snprintf(slots[nslots++], sizeof(slots[0]), "%02d:%02d", h, m0);
		} else {
			cJSON *t = NULL;
			cJSON_ArrayForEach(t, times) {
				if (nslots >= BAR_MED_MAX)
					break;
				if (cJSON_IsString(t) && t->valuestring)
					snprintf(slots[nslots++], sizeof(slots[0]), "%s",
							 t->valuestring);
			}
		}

		for (int32_t k = 0; k < nslots && bar_med.ndoses < BAR_MED_MAX; k++) {
			/* the plugin's dose key: medId@YYYY-MM-DDTHH:MM */
			char key[160];
			snprintf(key, sizeof(key), "%s@%sT%s", id, day, slots[k]);
			const char *status = "";
			cJSON *st_o = state ? cJSON_GetObjectItem(state, key) : NULL;
			if (st_o)
				status = bar_med_str(st_o, "status", "");
			bool done = strcmp(status, "taken") == 0 ||
						strcmp(status, "skipped") == 0;

			BarMedDose *d = &bar_med.doses[bar_med.ndoses++];
			snprintf(d->name, sizeof(d->name), "%s", name);
			snprintf(d->time, sizeof(d->time), "%s", slots[k]);
			snprintf(d->key, sizeof(d->key), "%s", key);
			snprintf(d->status, sizeof(d->status), "%s", status);
			d->scheduled = bar_med_at(day, slots[k]);
			d->due = !done && now >= d->scheduled &&
					 (now - d->scheduled) <= BAR_MED_DUE_WINDOW;
			d->pending = !done && d->scheduled > now;
			if (d->due) {
				bar_med.ndue++;
				due_name = d->name;
			}
		}
	}
	cJSON_Delete(root);

	/* earliest still-to-come dose */
	time_t best = 0;
	for (int32_t i = 0; i < bar_med.ndoses; i++) {
		BarMedDose *d = &bar_med.doses[i];
		if (d->pending && (!best || d->scheduled < best)) {
			best = d->scheduled;
			next = d->time;
		}
	}
	snprintf(bar_med.next_time, sizeof(bar_med.next_time), "%s",
			 next ? next : "");
	snprintf(bar_med.due_name, sizeof(bar_med.due_name), "%s",
			 bar_med.ndue == 1 && due_name ? due_name : "");
	bar_med.have = true;
}


/* ─── writing ─────────────────────────────────────────────────────────────── */

/* UTC in the exact spelling the plugin used, so a store written by either is
 * readable by both: 2026-07-26T04:12:33.000Z. */
static void bar_med_iso_utc(time_t t, char *out, size_t len) {
	struct tm tm;
	gmtime_r(&t, &tm);
	char base[32];
	strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
	snprintf(out, len, "%s.000Z", base);
}

static cJSON *bar_med_load_doc(void) {
	char path[512];
	bar_med_path(path, sizeof(path));
	FILE *f = fopen(path, "re");
	if (!f)
		return NULL;
	struct stat st;
	if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 ||
		st.st_size > 4 * 1024 * 1024) {
		fclose(f);
		return NULL;
	}
	char *buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)st.st_size, f);
	fclose(f);
	buf[got] = '\0';
	cJSON *root = cJSON_Parse(buf);
	free(buf);
	return root;
}

/* Replace the store atomically: write a sibling temp file, fsync it, rename
 * over the original. A rename within a directory is atomic, so a reader either
 * sees the whole old document or the whole new one -- never a truncated file
 * where a dose has no status. */
static bool bar_med_save_doc(cJSON *root) {
	char path[512], tmp[544];
	bar_med_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	char *text = cJSON_Print(root);
	if (!text)
		return false;
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		free(text);
		return false;
	}
	size_t len = strlen(text);
	bool ok = write(fd, text, len) == (ssize_t)len;
	if (ok)
		ok = fsync(fd) == 0;
	close(fd);
	free(text);
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		wlr_log(WLR_ERROR, "medication: could not save %s", path);
		return false;
	}
	/* force the next reload to re-read rather than trust the cached mtime */
	bar_med.mtime = 0;
	return true;
}

/* Prepend to `history`, capped at 200 entries exactly as the plugin capped it.
 * The history is what "did I take it?" is answered from months later, so it is
 * worth keeping in the same shape and the same order. */
static void bar_med_history_add(cJSON *root, const BarMedDose *d,
								const char *status, const char *taken_at) {
	cJSON *history = cJSON_GetObjectItem(root, "history");
	if (!history || !cJSON_IsArray(history)) {
		history = cJSON_CreateArray();
		if (!history)
			return;
		cJSON_ReplaceItemInObject(root, "history", history);
		if (!cJSON_GetObjectItem(root, "history"))
			cJSON_AddItemToObject(root, "history", history);
	}

	cJSON *entry = cJSON_CreateObject();
	if (!entry)
		return;
	/* medId is the part of the key before '@' */
	const char *at = strchr(d->key, '@');
	char med_id[96];
	size_t idlen = at ? (size_t)(at - d->key) : strlen(d->key);
	if (idlen >= sizeof(med_id))
		idlen = sizeof(med_id) - 1;
	memcpy(med_id, d->key, idlen);
	med_id[idlen] = '\0';

	char sched[40];
	bar_med_iso_utc(d->scheduled, sched, sizeof(sched));
	cJSON_AddStringToObject(entry, "medId", med_id);
	cJSON_AddStringToObject(entry, "name", d->name);
	cJSON_AddStringToObject(entry, "dosage", "");
	cJSON_AddStringToObject(entry, "scheduled", sched);
	if (taken_at)
		cJSON_AddStringToObject(entry, "takenAt", taken_at);
	cJSON_AddStringToObject(entry, "status", status);
	cJSON_InsertItemInArray(history, 0, entry);

	while (cJSON_GetArraySize(history) > 200)
		cJSON_DeleteItemFromArray(history, cJSON_GetArraySize(history) - 1);
}

static cJSON *bar_med_dose_state(cJSON *root) {
	cJSON *ds = cJSON_GetObjectItem(root, "doseState");
	if (!ds || !cJSON_IsObject(ds)) {
		ds = cJSON_CreateObject();
		if (ds)
			cJSON_AddItemToObject(root, "doseState", ds);
	}
	return ds;
}

/* Take or skip. `status` is "taken" or "skipped", the two the plugin wrote. */
static bool bar_med_mark(const BarMedDose *dose, const char *status) {
	if (!dose || !dose->key[0])
		return false;
	cJSON *root = bar_med_load_doc();
	if (!root)
		return false;
	cJSON *ds = bar_med_dose_state(root);
	if (!ds) {
		cJSON_Delete(root);
		return false;
	}

	char at[40];
	bar_med_iso_utc(time(NULL), at, sizeof(at));
	cJSON *st = cJSON_CreateObject();
	if (!st) {
		cJSON_Delete(root);
		return false;
	}
	cJSON_AddStringToObject(st, "status", status);
	if (!strcmp(status, "taken"))
		cJSON_AddStringToObject(st, "takenAt", at);
	cJSON_DeleteItemFromObject(ds, dose->key); /* replaces, like the plugin */
	cJSON_AddItemToObject(ds, dose->key, st);

	bar_med_history_add(root, dose, status,
						!strcmp(status, "taken") ? at : NULL);
	bool ok = bar_med_save_doc(root);
	cJSON_Delete(root);
	return ok;
}

/* Push a dose out by `minutes`, PRESERVING whatever else its state carried.
 * The plugin copied the existing members before adding postponedUntil, and it
 * matters: postponing a dose that was already alerted must not erase the
 * alert record and start it ringing again. */
static bool bar_med_postpone(const BarMedDose *dose, int32_t minutes) {
	if (!dose || !dose->key[0] || minutes <= 0)
		return false;
	cJSON *root = bar_med_load_doc();
	if (!root)
		return false;
	cJSON *ds = bar_med_dose_state(root);
	if (!ds) {
		cJSON_Delete(root);
		return false;
	}

	cJSON *prev = cJSON_GetObjectItem(ds, dose->key);
	cJSON *st = prev ? cJSON_Duplicate(prev, 1) : cJSON_CreateObject();
	if (!st) {
		cJSON_Delete(root);
		return false;
	}
	char until[40];
	bar_med_iso_utc(time(NULL) + minutes * 60, until, sizeof(until));
	cJSON_DeleteItemFromObject(st, "postponedUntil");
	cJSON_AddStringToObject(st, "postponedUntil", until);
	cJSON_DeleteItemFromObject(ds, dose->key);
	cJSON_AddItemToObject(ds, dose->key, st);

	bool ok = bar_med_save_doc(root);
	cJSON_Delete(root);
	return ok;
}

/* The store's own snooze length, so Postpone offers what the user configured
 * rather than a number invented here. */
static int32_t bar_med_snooze_minutes(void) {
	cJSON *root = bar_med_load_doc();
	int32_t mins = 15;
	if (root) {
		cJSON *v = cJSON_GetObjectItem(root, "snoozeMinutes");
		if (cJSON_IsNumber(v) && v->valuedouble > 0)
			mins = (int32_t)v->valuedouble;
		cJSON_Delete(root);
	}
	return mins;
}

#endif /* ASTEROIDZ_BAR_MEDICATION_H */
