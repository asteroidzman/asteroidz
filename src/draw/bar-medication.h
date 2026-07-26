#ifndef ASTEROIDZ_BAR_MEDICATION_H
#define ASTEROIDZ_BAR_MEDICATION_H

/* Medication reminders, reading the same store the waybar plugin writes:
 * $XDG_STATE_HOME/waybar-medication/medications.json.
 *
 * READ ONLY, deliberately. The waybar plugin owns this file -- it adds and
 * edits medications, records taken/skipped/postponed doses, prunes history and
 * rewrites the whole document. Two processes writing one JSON store with no
 * locking is how you lose a dose record, and "did I take it?" is exactly the
 * question this must never get wrong. The bar shows what is due; the popover
 * that takes and skips doses stays in waybar until this owns the store
 * outright.
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
	time_t scheduled;
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

#endif /* ASTEROIDZ_BAR_MEDICATION_H */
