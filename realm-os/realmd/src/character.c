/* The character: identity, hardware-derived attributes and the save file. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "realm.h"

uint32_t fnv1a(const char *s) {
	uint32_t h = 2166136261u;
	for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
	return h;
}

/* Keeps printable ASCII without protocol separators. */
void sanitize(char *s, size_t max) {
	size_t j = 0;
	for (size_t i = 0; s[i] && j + 1 < max; i++) {
		unsigned char ch = s[i];
		if (ch >= 32 && ch != 127 && ch != '\t') s[j++] = ch;
	}
	s[j] = 0;
}

static void mkdirs(const char *path) {
	char tmp[700];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
	}
	mkdir(tmp, 0755);
}

/* The install id is derived from machine-id without exposing it. */
static void install_id(char out[ID_LEN]) {
	char mid[128] = "";
	FILE *f = fopen("/etc/machine-id", "r");
	if (f) { if (!fgets(mid, sizeof mid, f)) mid[0] = 0; fclose(f); }
	mid[strcspn(mid, "\n")] = 0;
	if (!mid[0]) snprintf(mid, sizeof mid, "%ld-%d", (long)time(NULL), getpid());
	char a[160], b[160];
	snprintf(a, sizeof a, "realm-a:%s", mid);
	snprintf(b, sizeof b, "realm-b:%s", mid);
	snprintf(out, ID_LEN, "%08x%08x", fnv1a(a), fnv1a(b));
}

static void probe_hardware(struct character *c) {
	c->cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (c->cores < 1) c->cores = 1;
	long kb = 0;
	FILE *f = fopen("/proc/meminfo", "r");
	if (f) { if (fscanf(f, "MemTotal: %ld kB", &kb) != 1) kb = 0; fclose(f); }
	c->mem_gb = (int)((kb + 512 * 1024) / (1024 * 1024));
	if (c->mem_gb < 1) c->mem_gb = 1;
	c->laptop = access("/sys/class/power_supply/BAT0", F_OK) == 0 || access("/sys/class/power_supply/BAT1", F_OK) == 0;
}

const char *character_class(const struct character *c) {
	if (c->laptop) return "Wanderer";
	if (c->cores >= 12) return "Forge Knight";
	if (c->mem_gb >= 16) return "Archivist";
	if (c->cores >= 6) return "Artificer";
	return "Tinkerer";
}

void character_stats(const struct character *c, int out[4]) {
	int per = c->screen_w / 120;
	out[0] = c->cores > 64 ? 64 : c->cores;             /* STR */
	out[1] = c->mem_gb > 64 ? 64 : c->mem_gb;           /* INT */
	out[2] = per < 1 ? 1 : per > 40 ? 40 : per;         /* PER */
	out[3] = c->laptop ? 14 : 8;                        /* AGI */
}

static void save_path(struct game *g, char *out, size_t n) {
	snprintf(out, n, "%s/character", g->data_dir);
}

void character_load(struct game *g) {
	struct character *c = &g->me;
	const char *xdg = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
	snprintf(g->home, sizeof g->home, "%s", home);
	if (xdg) snprintf(g->data_dir, sizeof g->data_dir, "%s/realm", xdg);
	else snprintf(g->data_dir, sizeof g->data_dir, "%s/.local/share/realm", home);
	mkdirs(g->data_dir);
	char dir[600];
	snprintf(dir, sizeof dir, "%s/Inventory", home);
	mkdirs(dir);
	snprintf(dir, sizeof dir, "%s/Outbox", home);
	mkdirs(dir);

	int sw = c->screen_w, sh = c->screen_h;
	memset(c, 0, sizeof *c);
	c->screen_w = sw;
	c->screen_h = sh;
	probe_hardware(c);
	install_id(c->id);
	const char *override = getenv("REALM_ID");
	if (override) { snprintf(c->id, ID_LEN, "%s", override); sanitize(c->id, ID_LEN); }

	char path[700];
	save_path(g, path, sizeof path);
	FILE *f = fopen(path, "r");
	if (!f) { g->mode = MODE_CREATE; c->color = (int)(fnv1a(c->id) % 8); c->created = time(NULL); return; }
	char line[256];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		const char *v = eq + 1;
		if (!strcmp(line, "name")) { snprintf(c->name, NAME_LEN, "%s", v); sanitize(c->name, NAME_LEN); }
		else if (!strcmp(line, "color")) c->color = atoi(v) & 7;
		else if (!strcmp(line, "xp")) c->xp = atol(v);
		else if (!strcmp(line, "quests")) c->quests = (uint32_t)strtoul(v, NULL, 10);
		else if (!strcmp(line, "created")) c->created = atol(v);
	}
	fclose(f);
	g->mode = c->name[0] ? MODE_WORLD : MODE_CREATE;
}

void character_save(struct game *g) {
	if (g->mode == MODE_CREATE) return;
	char path[700], tmp[710];
	save_path(g, path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "name=%s\ncolor=%d\nxp=%ld\nquests=%u\ncreated=%ld\n", g->me.name, g->me.color, g->me.xp, g->me.quests, g->me.created);
	fclose(f);
	rename(tmp, path);
}
