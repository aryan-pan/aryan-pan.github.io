/* The world: terrain, homes, NPCs, items, creatures, the HUD and the desk bar. */
#define _GNU_SOURCE
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "realm.h"

enum { WATER, SAND, GRASS, TREE, STONE, FLOWER, KERNEL };
enum { NEAR_NONE, NEAR_DESK, NEAR_LOCKED, NEAR_ITEM, NEAR_CREATURE };

#define CX (MAP_W / 2)
#define CY (MAP_H / 2)
#define SPEED 170.0

const double palette[8][3] = {
	{0.88, 0.39, 0.31}, {0.91, 0.64, 0.23}, {0.42, 0.75, 0.44}, {0.25, 0.71, 0.70},
	{0.36, 0.55, 0.94}, {0.60, 0.42, 0.94}, {0.88, 0.44, 0.69}, {0.85, 0.83, 0.78},
};
const char *const palette_names[8] = { "ember", "amber", "moss", "teal", "sky", "violet", "rose", "bone" };
const char *const quest_names[Q_COUNT] = {
	"Sit down at your desk", "Open an application", "Drop a file into the world (~/Outbox)",
	"Pick up a file someone left", "Swat a runaway process", "Visit Kernel Square",
	"Step inside another installation's home", "Say something in chat (T)",
};
const int quest_xp[Q_COUNT] = { 20, 15, 20, 25, 30, 15, 20, 10 };

static const char *const npc_lines[3][5] = {
	{ "anyone else get a runaway process today?", "kernel square is busy tonight", "left a recipe near my place", "btw I use arch", "leveled up by fixing my own cron jobs" },
	{ "stable is a lifestyle", "same desk since 2019", "if something runs hot, swat it", "dropped a poem by the path", "good morning, realm" },
	{ "rebuilt my house from a config file", "the kernel is humming", "who keeps leaving files by the lake?", "reproducible homes or bust", "hello from the south road" },
};
static const char *const npc_names[3] = { "mira@arch", "ollie@debian", "sable@nixos" };
static const int npc_colors[3] = { 3, 1, 5 };
static const char *const npc_files[][2] = {
	{ "recipe.txt", "Ramen for one install\n- 1 packet noodles\n- 1 egg\n- scallions\nBoil 3 min. Eat at desk.\n" },
	{ "poem.txt", "the fan spins down at dusk\na cursor blinks in the dark\nsomeone logs back in\n" },
	{ "map-notes.md", "# notes\n- Kernel Square sits at the centre\n- stone roads lead out in four directions\n" },
	{ "dotfiles.conf", "set -o vi\nalias ll='ls -la'\nexport REALM=home\n" },
};

/* ---------------- helpers ---------------- */

static uint32_t rng_state;
static double rnd(void) {
	uint32_t t = (rng_state += 0x6D2B79F5u);
	t = (t ^ (t >> 15)) * (t | 1u);
	t ^= t + (t ^ (t >> 7)) * (t | 61u);
	return (t ^ (t >> 14)) / 4294967296.0;
}

static double frand(void) { return rand() / (double)RAND_MAX; }

int game_level(long xp) { return (int)floor(sqrt(xp / 25.0)) + 1; }
static long level_xp(int lvl) { return 25L * (lvl - 1) * (lvl - 1); }

void game_toast(struct game *g, const char *fmt, ...) {
	memmove(g->toast[1], g->toast[0], sizeof g->toast[0] * 3);
	memmove(g->toast_until + 1, g->toast_until, sizeof(double) * 3);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g->toast[0], sizeof g->toast[0], fmt, ap);
	va_end(ap);
	g->toast_until[0] = g->t + 5;
}

void game_gain_xp(struct game *g, int xp, const char *why) {
	int before = game_level(g->me.xp);
	g->me.xp += xp;
	if (why) game_toast(g, "+%d XP  %s", xp, why);
	int after = game_level(g->me.xp);
	if (after > before) game_toast(g, "Level up! %s is now level %d", g->me.name, after);
	character_save(g);
}

void game_quest(struct game *g, int q) {
	if (g->mode == MODE_CREATE || (g->me.quests & (1u << q))) return;
	g->me.quests |= 1u << q;
	char why[96];
	snprintf(why, sizeof why, "Quest: %s", quest_names[q]);
	game_gain_xp(g, quest_xp[q], why);
}

/* ---------------- terrain ---------------- */

struct vnoise { int scale, gw; float *v; };

static void vn_init(struct vnoise *n, int scale, uint32_t seed) {
	n->scale = scale;
	n->gw = MAP_W / scale + 3;
	n->v = malloc(sizeof(float) * n->gw * n->gw);
	rng_state = seed;
	for (int i = 0; i < n->gw * n->gw; i++) n->v[i] = (float)rnd();
}

static double vn(const struct vnoise *n, double x, double y) {
	double fx = x / n->scale, fy = y / n->scale;
	int ix = (int)fx, iy = (int)fy;
	double tx = fx - ix, ty = fy - iy;
	tx = tx * tx * (3 - 2 * tx);
	ty = ty * ty * (3 - 2 * ty);
	double a = n->v[iy * n->gw + ix], b = n->v[iy * n->gw + ix + 1];
	double c = n->v[(iy + 1) * n->gw + ix], d = n->v[(iy + 1) * n->gw + ix + 1];
	return a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty;
}

static void generate(struct game *g) {
	struct vnoise elev, detail, flora;
	vn_init(&elev, 18, 11);
	vn_init(&detail, 6, 23);
	vn_init(&flora, 4, 37);
	for (int y = 0; y < MAP_H; y++) for (int x = 0; x < MAP_W; x++) {
		double d = hypot((x - CX) / (double)CX, (y - CY) / (double)CY);
		double e = vn(&elev, x, y) * 0.65 + vn(&detail, x, y) * 0.35 - fmax(0, d - 0.62) * 1.8;
		int t;
		if (e < 0.3) t = WATER;
		else if (e < 0.35) t = SAND;
		else { double f = vn(&flora, x, y); t = f > 0.66 ? TREE : f < 0.22 ? FLOWER : GRASS; }
		double pd = hypot(x - CX + 0.5, y - CY + 0.5);
		if (pd < 7.5) t = STONE;
		if ((fabs(x - CX + 0.5) < 1.1 || fabs(y - CY + 0.5) < 1.1) && pd < 46) t = STONE;
		g->terrain[y * MAP_W + x] = (uint8_t)t;
	}
	for (int y = CY - 1; y <= CY; y++) for (int x = CX - 1; x <= CX; x++) g->terrain[y * MAP_W + x] = KERNEL;
	free(elev.v);
	free(detail.v);
	free(flora.v);
	g->ground_dirty = true;
}

/* ---------------- homes ---------------- */

static bool houses_overlap(const struct house *a, const struct house *b) {
	return a->x < b->x + HOUSE_W + 3 && b->x < a->x + HOUSE_W + 3 && a->y < b->y + HOUSE_H + 3 && b->y < a->y + HOUSE_H + 3;
}

struct house *house_add(struct game *g, const char *id, const char *name, int color) {
	for (int i = 0; i < g->n_houses; i++) if (!strcmp(g->houses[i].id, id)) {
		struct house *h = &g->houses[i];
		snprintf(h->name, NAME_LEN, "%s", name);
		h->color = color & 7;
		return h;
	}
	if (g->n_houses >= MAX_HOUSES) return NULL;
	struct house h = { .color = color & 7 };
	snprintf(h.id, ID_LEN, "%s", id);
	snprintf(h.name, NAME_LEN, "%s", name);
	uint32_t hv = fnv1a(id);
	double ang = (hv % 3600) / 3600.0 * 2 * M_PI;
	double dist = 17 + ((hv >> 12) % 22);
	for (int tries = 0; tries < 40; tries++) {
		int x = (int)lround(CX + cos(ang) * dist - HOUSE_W / 2.0);
		int y = (int)lround(CY + sin(ang) * dist - HOUSE_H / 2.0);
		h.x = x < 4 ? 4 : x > MAP_W - HOUSE_W - 4 ? MAP_W - HOUSE_W - 4 : x;
		h.y = y < 4 ? 4 : y > MAP_H - HOUSE_H - 6 ? MAP_H - HOUSE_H - 6 : y;
		bool near_plaza = h.x < CX + 9 && h.x + HOUSE_W > CX - 9 && h.y < CY + 9 && h.y + HOUSE_H > CY - 9;
		bool clash = false;
		for (int i = 0; i < g->n_houses; i++) clash |= houses_overlap(&g->houses[i], &h);
		if (!near_plaza && !clash) break;
		ang += 0.45;
	}
	for (int y = h.y - 1; y <= h.y + HOUSE_H + 1; y++) for (int x = h.x - 1; x <= h.x + HOUSE_W; x++)
		if (g->terrain[y * MAP_W + x] != STONE) g->terrain[y * MAP_W + x] = GRASS;
	for (int y = h.y + HOUSE_H; y <= h.y + HOUSE_H + 2; y++) {
		g->terrain[y * MAP_W + h.x + 3] = STONE;
		g->terrain[y * MAP_W + h.x + 4] = STONE;
	}
	g->houses[g->n_houses] = h;
	g->ground_dirty = true;
	return &g->houses[g->n_houses++];
}

static struct house *my_house(struct game *g) {
	for (int i = 0; i < g->n_houses; i++) if (!strcmp(g->houses[i].id, g->me.id)) return &g->houses[i];
	return NULL;
}

enum { HT_FLOOR, HT_WALL, HT_DOOR, HT_DESK, HT_BED, HT_PLANT };

static int house_tile(int lx, int ly) {
	if (ly == HOUSE_H - 1 && (lx == 3 || lx == 4)) return HT_DOOR;
	if (lx == 0 || lx == HOUSE_W - 1 || ly == 0 || ly == HOUSE_H - 1) return HT_WALL;
	if (ly == 1 && (lx == 3 || lx == 4)) return HT_DESK;
	if (lx == 6 && (ly == 1 || ly == 2)) return HT_BED;
	if (lx == 1 && ly == 1) return HT_PLANT;
	return HT_FLOOR;
}

static struct house *house_at(struct game *g, int tx, int ty) {
	for (int i = 0; i < g->n_houses; i++) {
		struct house *h = &g->houses[i];
		if (tx >= h->x && tx < h->x + HOUSE_W && ty >= h->y && ty < h->y + HOUSE_H) return h;
	}
	return NULL;
}

static void desk_spot(const struct house *h, double *x, double *y) { *x = (h->x + 4) * TILE; *y = (h->y + 2) * TILE + 14; }
static void door_spot(const struct house *h, double *x, double *y) { *x = (h->x + 4) * TILE; *y = (h->y + HOUSE_H + 1) * TILE + 8; }

static bool solid(struct game *g, int tx, int ty) {
	if (tx < 0 || ty < 0 || tx >= MAP_W || ty >= MAP_H) return true;
	struct house *h = house_at(g, tx, ty);
	if (h) { int k = house_tile(tx - h->x, ty - h->y); return k != HT_FLOOR && k != HT_DOOR; }
	int t = g->terrain[ty * MAP_W + tx];
	return t == WATER || t == TREE || t == KERNEL;
}

static bool blocked(struct game *g, double x, double y) {
	const double r = 9;
	return solid(g, (int)floor((x - r) / TILE), (int)floor((y - r) / TILE)) || solid(g, (int)floor((x + r) / TILE), (int)floor((y - r) / TILE)) ||
		solid(g, (int)floor((x - r) / TILE), (int)floor((y + r) / TILE)) || solid(g, (int)floor((x + r) / TILE), (int)floor((y + r) / TILE));
}

static void move_body(struct game *g, double *x, double *y, double dx, double dy) {
	if (dx != 0 && !blocked(g, *x + dx, *y)) *x += dx;
	if (dy != 0 && !blocked(g, *x, *y + dy)) *y += dy;
}

/* ---------------- items ---------------- */

static void item_paths(struct game *g, const char *id, char *meta, char *data, size_t n) {
	snprintf(meta, n, "%s/items/%s.meta", g->data_dir, id);
	snprintf(data, n, "%s/items/%s.data", g->data_dir, id);
}

struct item *item_add_local(struct game *g, const char *name, const unsigned char *data, size_t len, double x, double y, bool npc) {
	if (g->n_items >= MAX_ITEMS) return NULL;
	struct item *it = &g->items[g->n_items++];
	memset(it, 0, sizeof *it);
	snprintf(it->id, ID_LEN, "%08x%08x", (unsigned)rand(), (unsigned)rand());
	snprintf(it->holder, ID_LEN, "%s", npc ? "npc" : g->me.id);
	snprintf(it->holder_name, NAME_LEN, "%s", npc ? "an npc" : g->me.name);
	snprintf(it->name, sizeof it->name, "%s", name);
	it->x = x;
	it->y = y;
	it->npc = npc;
	it->data = malloc(len ? len : 1);
	memcpy(it->data, data, len);
	it->len = len;
	it->last_seen = realm_now();
	return it;
}

void item_remove_at(struct game *g, int i) {
	struct item *it = &g->items[i];
	if (it->data && !it->npc) {
		char meta[700], data[700];
		item_paths(g, it->id, meta, data, sizeof meta);
		unlink(meta);
		unlink(data);
	}
	free(it->data);
	g->items[i] = g->items[--g->n_items];
}

void items_save(struct game *g) {
	char dir[600];
	snprintf(dir, sizeof dir, "%s/items", g->data_dir);
	mkdir(dir, 0755);
	for (int i = 0; i < g->n_items; i++) {
		struct item *it = &g->items[i];
		if (!it->data || it->npc) continue;
		char meta[700], data[700];
		item_paths(g, it->id, meta, data, sizeof meta);
		FILE *f = fopen(meta, "w");
		if (!f) continue;
		fprintf(f, "%s\n%d\n%d\n", it->name, (int)it->x, (int)it->y);
		fclose(f);
		f = fopen(data, "wb");
		if (f) { fwrite(it->data, 1, it->len, f); fclose(f); }
	}
}

void items_load(struct game *g) {
	char dir[600];
	snprintf(dir, sizeof dir, "%s/items", g->data_dir);
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		char *dot = strstr(e->d_name, ".meta");
		if (!dot || dot - e->d_name >= ID_LEN) continue;
		char id[ID_LEN];
		snprintf(id, sizeof id, "%.*s", (int)(dot - e->d_name), e->d_name);
		char meta[700], data[700], name[64] = "";
		item_paths(g, id, meta, data, sizeof meta);
		FILE *f = fopen(meta, "r");
		if (!f) continue;
		int x = 0, y = 0;
		if (!fgets(name, sizeof name, f) || fscanf(f, "%d\n%d", &x, &y) != 2) { fclose(f); continue; }
		fclose(f);
		name[strcspn(name, "\n")] = 0;
		static unsigned char buf[MAX_ITEM_BYTES];
		f = fopen(data, "rb");
		if (!f) continue;
		size_t len = fread(buf, 1, sizeof buf, f);
		fclose(f);
		struct item *it = item_add_local(g, name, buf, len, x, y, false);
		if (it) snprintf(it->id, ID_LEN, "%s", id);
	}
	closedir(d);
}

/* Writes a received file into ~/Inventory under a name that does not collide. */
void item_receive(struct game *g, const char *raw_name, const unsigned char *data, size_t len, const char *from) {
	char name[64];
	snprintf(name, sizeof name, "%s", raw_name);
	sanitize(name, sizeof name);
	for (char *p = name; *p; p++) if (*p == '/') *p = '_';
	while (name[0] == '.') memmove(name, name + 1, strlen(name));
	if (!name[0]) snprintf(name, sizeof name, "found-file");
	char path[800];
	snprintf(path, sizeof path, "%s/Inventory/%s", g->home, name);
	for (int n = 2; access(path, F_OK) == 0 && n < 100; n++)
		snprintf(path, sizeof path, "%s/Inventory/%d-%s", g->home, n, name);
	FILE *f = fopen(path, "wb");
	if (!f) { game_toast(g, "Could not store %s in ~/Inventory", name); return; }
	fwrite(data, 1, len, f);
	fclose(f);
	game_toast(g, "Picked up %s from %s -> ~/Inventory", name, from);
	game_gain_xp(g, 8, NULL);
	game_quest(g, Q_LOOT);
}

/* Files placed in ~/Outbox leave this machine and appear outside the door. */
static void scan_outbox(struct game *g) {
	static char warned[64];
	char dir[600];
	snprintf(dir, sizeof dir, "%s/Outbox", g->home);
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	struct house *h = my_house(g);
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char path[900];
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(path, &st) || !S_ISREG(st.st_mode)) continue;
		if (st.st_size > MAX_ITEM_BYTES) {
			if (strncmp(warned, e->d_name, sizeof warned - 1)) {
				snprintf(warned, sizeof warned, "%.63s", e->d_name);
				game_toast(g, "%s is too heavy to carry into the world (32 KB max)", e->d_name);
			}
			continue;
		}
		static unsigned char buf[MAX_ITEM_BYTES];
		FILE *f = fopen(path, "rb");
		if (!f) continue;
		size_t len = fread(buf, 1, sizeof buf, f);
		fclose(f);
		double x, y;
		if (g->mode == MODE_DESK && h) { door_spot(h, &x, &y); x += (frand() - 0.5) * 70; y += frand() * 20; }
		else { x = g->px + 22; y = g->py + 12; }
		if (!item_add_local(g, e->d_name, buf, len, x, y, false)) break;
		unlink(path);
		items_save(g);
		game_toast(g, "%s is now lying outside your door", e->d_name);
		game_gain_xp(g, 4, NULL);
		game_quest(g, Q_DROP);
	}
	closedir(d);
}

/* ---------------- NPC installations ---------------- */

static void npcs_init(struct game *g) {
	for (int i = 0; i < MAX_NPCS; i++) {
		struct npc *n = &g->npcs[i];
		memset(n, 0, sizeof *n);
		snprintf(n->id, ID_LEN, "npc-%d", i);
		snprintf(n->name, NAME_LEN, "%s", npc_names[i]);
		n->color = npc_colors[i];
		n->lines = npc_lines[i];
		struct house *h = house_add(g, n->id, n->name, n->color);
		door_spot(h, &n->x, &n->y);
		n->wait = frand() * 3;
		n->next_say = 12 + frand() * 30;
		n->next_drop = 25 + frand() * 40;
		n->dir = 1;
	}
}

static void chat_line(struct game *g, const char *name, int color, const char *text) {
	if (g->n_chat == 8) {
		memmove(g->chat_log[0], g->chat_log[1], sizeof g->chat_log[0] * 7);
		memmove(g->chat_color, g->chat_color + 1, sizeof(int) * 7);
		g->n_chat = 7;
	}
	snprintf(g->chat_log[g->n_chat], sizeof g->chat_log[0], "%s: %s", name, text);
	g->chat_color[g->n_chat++] = color & 7;
}

static void npcs_update(struct game *g, double dt) {
	for (int i = 0; i < MAX_NPCS; i++) {
		struct npc *n = &g->npcs[i];
		if (!n->has_target) {
			n->moving = false;
			n->wait -= dt;
			if (n->wait <= 0) {
				for (int k = 0; k < 20; k++) {
					double tx = n->x + (frand() - 0.5) * TILE * 22, ty = n->y + (frand() - 0.5) * TILE * 22;
					if (!blocked(g, tx, ty)) { n->tx = tx; n->ty = ty; n->has_target = true; break; }
				}
				n->wait = 1 + frand() * 4;
			}
		} else {
			double dx = n->tx - n->x, dy = n->ty - n->y, d = hypot(dx, dy);
			if (d < 6) { n->has_target = false; continue; }
			double sp = 70 * dt, ox = n->x, oy = n->y;
			move_body(g, &n->x, &n->y, dx / d * sp, dy / d * sp);
			n->moving = true;
			n->walk += dt;
			if (dx != 0) n->dir = dx > 0 ? 1 : -1;
			if (hypot(n->x - ox, n->y - oy) < sp * 0.3) { n->stuck += dt; if (n->stuck > 1) { n->has_target = false; n->stuck = 0; } }
			else n->stuck = 0;
		}
		n->next_say -= dt;
		if (n->next_say <= 0) {
			n->next_say = 40 + frand() * 50;
			const char *line = n->lines[rand() % 5];
			chat_line(g, n->name, n->color, line);
			snprintf(n->bubble, sizeof n->bubble, "%s", line);
			n->bubble_until = g->t + 5;
		}
		n->next_drop -= dt;
		if (n->next_drop <= 0) {
			n->next_drop = 80 + frand() * 80;
			int npc_items = 0;
			for (int k = 0; k < g->n_items; k++) npc_items += g->items[k].npc;
			if (npc_items < 3) {
				int f = rand() % 4;
				struct item *it = item_add_local(g, npc_files[f][0], (const unsigned char *)npc_files[f][1], strlen(npc_files[f][1]), n->x + 14, n->y + 10, true);
				if (it) snprintf(it->holder_name, NAME_LEN, "%s", n->name);
			}
		}
	}
}

/* ---------------- setup ---------------- */

void game_init(struct game *g, int screen_w, int screen_h) {
	memset(g, 0, sizeof *g);
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	g->me.screen_w = screen_w;
	g->me.screen_h = screen_h;
	character_load(g);
	FILE *f = fopen("/proc/meminfo", "r");
	if (f) { if (fscanf(f, "MemTotal: %ld kB", &g->mem_total_kb) != 1) g->mem_total_kb = 0; fclose(f); }

	generate(g);
	if (g->mode == MODE_CREATE) {
		char host[64] = "";
		gethostname(host, sizeof host);
		host[16] = 0;
		sanitize(host, sizeof host);
		snprintf(g->me.name, NAME_LEN, "%s", host[0] ? host : "wanderer");
	}
	struct house *h = house_add(g, g->me.id, g->me.name, g->me.color);
	npcs_init(g);
	double dx, dy;
	desk_spot(h, &dx, &dy);
	g->px = dx;
	g->py = dy + 30;
	g->dir = 1;
	items_load(g);
	net_init(g);
	procs_scan(g, 1);
	if (g->mode == MODE_WORLD) game_toast(g, "Welcome back, %s. Your desk is right in front of you.", g->me.name);
}

/* ---------------- input ---------------- */

static void interact(struct game *g) {
	switch (g->near_kind) {
	case NEAR_DESK: {
		struct house *h = my_house(g);
		desk_spot(h, &g->px, &g->py);
		g->has_target = false;
		realm_set_seated(true);
		game_quest(g, Q_SIT);
		break;
	}
	case NEAR_LOCKED:
		game_toast(g, "%s", "That desk only answers to its owner.");
		break;
	case NEAR_ITEM: {
		struct item *it = &g->items[g->near_index];
		if (it->data) {
			bool mine = !strcmp(it->holder, g->me.id);
			char from[NAME_LEN];
			snprintf(from, sizeof from, "%s", it->holder_name);
			if (mine) {
				item_receive(g, it->name, it->data, it->len, "the ground");
			} else {
				item_receive(g, it->name, it->data, it->len, from);
			}
			item_remove_at(g, g->near_index);
			if (mine) items_save(g);
		} else {
			net_request_pickup(g, it);
			game_toast(g, "Reaching for %s...", it->name);
		}
		break;
	}
	case NEAR_CREATURE: {
		struct creature *c = &g->creatures[g->near_index];
		if (!c->runaway && !c->protected_ && (g->confirm_pid != c->pid || g->t > g->confirm_until)) {
			g->confirm_pid = c->pid;
			g->confirm_until = g->t + 2.5;
			game_toast(g, "%s (pid %d) is behaving. Press E again to end it anyway.", c->comm, c->pid);
			break;
		}
		procs_swat(g, g->near_index);
		g->confirm_pid = 0;
		break;
	}
	}
}

static void type_char(char *buf, size_t max, xkb_keysym_t sym) {
	char utf[8] = "";
	xkb_keysym_to_utf8(sym, utf, sizeof utf);
	size_t n = strlen(buf);
	if (utf[0] >= 32 && utf[0] < 127 && !utf[1] && n + 1 < max) { buf[n] = utf[0]; buf[n + 1] = 0; }
}

bool game_key(struct game *g, xkb_keysym_t sym, bool pressed) {
	if (g->mode == MODE_CREATE) {
		if (!pressed) return true;
		size_t n = strlen(g->me.name);
		if (sym == XKB_KEY_Return && n) {
			g->mode = MODE_WORLD;
			house_add(g, g->me.id, g->me.name, g->me.color);
			character_save(g);
			game_toast(g, "%s joins the realm. Walk up to your desk and press E.", g->me.name);
		} else if (sym == XKB_KEY_BackSpace && n) g->me.name[n - 1] = 0;
		else if (sym == XKB_KEY_Left || sym == XKB_KEY_Right) {
			g->me.color = (g->me.color + (sym == XKB_KEY_Right ? 1 : 7)) % 8;
			house_add(g, g->me.id, g->me.name, g->me.color);
		} else if (n < 16 && ((sym >= XKB_KEY_a && sym <= XKB_KEY_z) || (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) ||
				(sym >= XKB_KEY_0 && sym <= XKB_KEY_9) || sym == XKB_KEY_minus || sym == XKB_KEY_underscore)) {
			type_char(g->me.name, 17, sym);
		}
		return true;
	}
	if (g->mode == MODE_CHAT) {
		if (!pressed) return true;
		size_t n = strlen(g->chat_input);
		if (sym == XKB_KEY_Return) {
			if (n) {
				net_send_chat(g, g->chat_input);
				chat_line(g, g->me.name, g->me.color, g->chat_input);
				snprintf(g->bubble, sizeof g->bubble, "%s", g->chat_input);
				g->bubble_until = g->t + 6;
				game_quest(g, Q_CHAT);
			}
			g->chat_input[0] = 0;
			g->mode = MODE_WORLD;
		} else if (sym == XKB_KEY_Escape) {
			g->chat_input[0] = 0;
			g->mode = MODE_WORLD;
		} else if (sym == XKB_KEY_BackSpace) {
			if (n) g->chat_input[n - 1] = 0;
		} else {
			type_char(g->chat_input, 120, sym);
		}
		return true;
	}
	if (g->mode != MODE_WORLD) return false;
	switch (sym) {
	case XKB_KEY_w: case XKB_KEY_Up: g->k_up = pressed; g->has_target = false; return true;
	case XKB_KEY_s: case XKB_KEY_Down: g->k_down = pressed; g->has_target = false; return true;
	case XKB_KEY_a: case XKB_KEY_Left: g->k_left = pressed; g->has_target = false; return true;
	case XKB_KEY_d: case XKB_KEY_Right: g->k_right = pressed; g->has_target = false; return true;
	case XKB_KEY_e: case XKB_KEY_space: if (pressed) interact(g); return true;
	case XKB_KEY_t: case XKB_KEY_Return:
		if (pressed) { g->mode = MODE_CHAT; g->k_up = g->k_down = g->k_left = g->k_right = false; }
		return true;
	default: return false;
	}
}

void game_click(struct game *g, double sx, double sy, int w, int h) {
	if (g->mode == MODE_DESK) {
		for (int i = 0; i < g->n_bar_btn; i++) {
			if (sx < g->bar_btn[i].x0 || sx > g->bar_btn[i].x1) continue;
			if (!strcmp(g->bar_btn[i].action, "stand")) realm_set_seated(false);
			else realm_launch(g->bar_btn[i].action);
			return;
		}
		return;
	}
	if (g->mode != MODE_WORLD) return;
	if (g->near_kind && fabs(sx - w / 2.0) < 220 && sy > h - 110 && sy < h - 60) { interact(g); return; }
	g->tx = sx + g->cam_x;
	g->ty = sy + g->cam_y;
	g->has_target = true;
}

void game_on_stand(struct game *g) {
	g->k_up = g->k_down = g->k_left = g->k_right = false;
	g->py += 30;
}

void game_on_window_opened(struct game *g) { game_quest(g, Q_WINDOW); }

/* ---------------- simulation ---------------- */

static void update_creatures(struct game *g, double dt) {
	struct house *h = my_house(g);
	if (!h) return;
	double hx = (h->x + HOUSE_W / 2.0) * TILE, hy = (h->y + HOUSE_H / 2.0) * TILE;
	for (int i = 0; i < g->n_creatures; i++) {
		struct creature *c = &g->creatures[i];
		if (c->pid < 0) continue;
		if (c->runaway) {
			if (c->x < 0) { c->x = hx + (frand() - 0.5) * 80; c->y = hy + (frand() - 0.5) * 60; }
			double gx = g->mode == MODE_DESK ? hx : g->px, gy = g->mode == MODE_DESK ? hy : g->py;
			double dx = gx - c->x, dy = gy - c->y, d = hypot(dx, dy);
			if (d > 30) { c->x += dx / d * 50 * dt; c->y += dy / d * 50 * dt; }
			c->x += sin(g->t * 3 + c->phase) * 20 * dt;
		} else {
			double r = 90 + (i % 4) * 26, sp = 0.35 + (i % 3) * 0.12;
			c->x = hx + cos(g->t * sp + c->phase) * r;
			c->y = hy + sin(g->t * sp + c->phase) * r * 0.7;
		}
	}
}

static void find_interaction(struct game *g) {
	int kind = NEAR_NONE, index = -1;
	double best = 1e9;
	char label[160] = "";
	if (g->mode == MODE_WORLD) {
		for (int i = 0; i < g->n_creatures; i++) {
			struct creature *c = &g->creatures[i];
			if (c->pid < 0) continue;
			double d = hypot(c->x - g->px, c->y - g->py) - (c->runaway ? 20 : 0);
			if (d < 34 && d < best) {
				best = d; kind = NEAR_CREATURE; index = i;
				if (c->protected_) snprintf(label, sizeof label, "%s (pid %d) - essential, can't be swatted", c->comm, c->pid);
				else snprintf(label, sizeof label, "Swat %s (pid %d, %ld MB)", c->comm, c->pid, c->rss_kb / 1024);
			}
		}
		for (int i = 0; i < g->n_items; i++) {
			struct item *it = &g->items[i];
			double d = hypot(it->x - g->px, it->y - g->py);
			if (d < 40 && d < best) {
				best = d; kind = NEAR_ITEM; index = i;
				if (!strcmp(it->holder, g->me.id)) snprintf(label, sizeof label, "Pick up %s", it->name);
				else snprintf(label, sizeof label, "Pick up %s (from %s)", it->name, it->holder_name);
			}
		}
		for (int i = 0; i < g->n_houses; i++) {
			double dx, dy;
			desk_spot(&g->houses[i], &dx, &dy);
			double d = hypot(dx - g->px, dy - 14 - g->py) + 10;
			if (d < 66 && d < best) {
				best = d; index = i;
				bool mine = !strcmp(g->houses[i].id, g->me.id);
				kind = mine ? NEAR_DESK : NEAR_LOCKED;
				if (mine) snprintf(label, sizeof label, "%s", "Sit at your desk");
				else snprintf(label, sizeof label, "%s's desk", g->houses[i].name);
			}
		}
	}
	g->near_kind = kind;
	g->near_index = index;
	snprintf(g->near_label, sizeof g->near_label, "%s", label);
}

void game_tick(struct game *g, double dt) {
	g->t += dt;
	if (g->mode == MODE_WORLD) {
		double dx = (g->k_right - g->k_left), dy = (g->k_down - g->k_up);
		if (dx == 0 && dy == 0 && g->has_target) {
			double tx = g->tx - g->px, ty = g->ty - g->py, d = hypot(tx, ty);
			if (d < 4) g->has_target = false; else { dx = tx / d; dy = ty / d; }
		}
		double len = hypot(dx, dy);
		g->moving = len > 0;
		if (len > 0) {
			double ox = g->px, oy = g->py, sp = SPEED * dt;
			move_body(g, &g->px, &g->py, dx / len * sp, dy / len * sp);
			if (g->has_target && ox == g->px && oy == g->py) g->has_target = false;
			if (dx != 0) g->dir = dx > 0 ? 1 : -1;
			g->walk += dt;
		}
	} else {
		g->moving = false;
	}

	int ptx = (int)(g->px / TILE), pty = (int)(g->py / TILE);
	if (g->mode != MODE_CREATE) {
		if (hypot(ptx - CX, pty - CY) < 8) game_quest(g, Q_SQUARE);
		struct house *in = house_at(g, ptx, pty);
		if (in && strcmp(in->id, g->me.id)) game_quest(g, Q_VISIT);
	}

	update_creatures(g, dt);
	npcs_update(g, dt);
	net_poll(g);

	g->proc_timer += dt;
	if (g->proc_timer >= 1) { procs_scan(g, g->proc_timer); g->proc_timer = 0; }
	g->outbox_timer += dt;
	if (g->outbox_timer >= 1 && g->mode != MODE_CREATE) { scan_outbox(g); g->outbox_timer = 0; }
	g->net_timer += dt;
	if (g->net_timer >= 0.1 && g->mode != MODE_CREATE) { net_send_state(g); g->net_timer = 0; }
	g->announce_timer += dt;
	if (g->announce_timer >= 1) {
		net_announce_items(g);
		g->announce_timer = 0;
		double now = realm_now();
		for (int i = 0; i < g->n_peers; i++) if (now - g->peers[i].last_seen > 5) g->peers[i--] = g->peers[--g->n_peers];
		for (int i = 0; i < g->n_items; i++) if (!g->items[i].data && now - g->items[i].last_seen > 5) item_remove_at(g, i--);
	}

	find_interaction(g);
}

/* ---------------- drawing helpers ---------------- */

static void set_rgb(cairo_t *cr, const double c[3], double a) { cairo_set_source_rgba(cr, c[0], c[1], c[2], a); }

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static void font(cairo_t *cr, double size, bool bold) {
	cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
}

static double text_w(cairo_t *cr, const char *s) {
	cairo_text_extents_t e;
	cairo_text_extents(cr, s, &e);
	return e.x_advance;
}

static void text(cairo_t *cr, const char *s, double x, double y) {
	cairo_move_to(cr, x, y);
	cairo_show_text(cr, s);
}

/* Centered label on a translucent pill. */
static void label(cairo_t *cr, const char *s, double x, double y, double size, double br, double bg, double bb, double ba) {
	font(cr, size, true);
	double w = text_w(cr, s) + 12;
	cairo_set_source_rgba(cr, br, bg, bb, ba);
	rounded(cr, x - w / 2, y - size - 2, w, size + 8, 5);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	text(cr, s, x - w / 2 + 6, y + 1);
}

static void speech(cairo_t *cr, const char *s, double x, double y) {
	char buf[64];
	snprintf(buf, sizeof buf, "%.46s%s", s, strlen(s) > 46 ? "..." : "");
	font(cr, 12, false);
	double w = text_w(cr, buf) + 18;
	cairo_set_source_rgb(cr, 1, 1, 1);
	rounded(cr, x - w / 2, y - 22, w, 24, 9);
	cairo_fill(cr);
	cairo_move_to(cr, x - 5, y + 2);
	cairo_line_to(cr, x + 5, y + 2);
	cairo_line_to(cr, x, y + 8);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.11, 0.13, 0.19);
	text(cr, buf, x - w / 2 + 9, y - 6);
}

static uint32_t tile_hash(int x, int y) {
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static const double ground_colors[7][3] = {
	{0.15, 0.33, 0.50}, {0.85, 0.77, 0.56}, {0.36, 0.60, 0.30}, {0.33, 0.56, 0.27},
	{0.56, 0.56, 0.60}, {0.36, 0.60, 0.30}, {0.56, 0.56, 0.60},
};

static void render_ground(struct game *g) {
	if (!g->ground) g->ground = cairo_image_surface_create(CAIRO_FORMAT_RGB24, MAP_W * TILE, MAP_H * TILE);
	cairo_t *cr = cairo_create(g->ground);
	for (int y = 0; y < MAP_H; y++) for (int x = 0; x < MAP_W; x++) {
		int t = g->terrain[y * MAP_W + x];
		double px = x * TILE, py = y * TILE;
		uint32_t v = tile_hash(x, y);
		set_rgb(cr, ground_colors[t], 1);
		cairo_rectangle(cr, px, py, TILE, TILE);
		cairo_fill(cr);
		if (t == GRASS || t == TREE || t == FLOWER) {
			cairo_set_source_rgba(cr, 1, 1, 1, 0.05);
			if (v & 1) { cairo_rectangle(cr, px + (v >> 3) % 24, py + (v >> 8) % 24, 3, 6); cairo_fill(cr); }
		}
		if (t == FLOWER) {
			static const double fc[3][3] = { {0.95, 0.89, 0.48}, {0.94, 0.60, 0.75}, {1, 1, 1} };
			set_rgb(cr, fc[v % 3], 1);
			cairo_rectangle(cr, px + (v >> 4) % 26 + 2, py + (v >> 9) % 26 + 2, 4, 4);
			cairo_rectangle(cr, px + (v >> 13) % 26 + 2, py + (v >> 17) % 26 + 2, 4, 4);
			cairo_fill(cr);
		} else if (t == STONE || t == KERNEL) {
			cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
			cairo_set_line_width(cr, 1);
			cairo_rectangle(cr, px + 0.5, py + 0.5, TILE - 1, TILE - 1);
			cairo_stroke(cr);
		}
	}
	for (int y = 0; y < MAP_H; y++) for (int x = 0; x < MAP_W; x++) {
		if (g->terrain[y * MAP_W + x] != TREE) continue;
		double px = x * TILE + 16, py = y * TILE + 16;
		uint32_t v = tile_hash(y, x);
		cairo_set_source_rgb(cr, 0.36, 0.25, 0.15);
		cairo_rectangle(cr, px - 3, py + 2, 6, 12);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.18, (v & 1) ? 0.42 : 0.38, 0.20);
		cairo_arc(cr, px, py - 2, 17, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
		cairo_arc(cr, px - 5, py - 8, 7, 0, 2 * M_PI);
		cairo_fill(cr);
	}
	cairo_destroy(cr);
	g->ground_dirty = false;
}

static void draw_house(struct game *g, cairo_t *cr, const struct house *h, int w, int hh) {
	double px = h->x * TILE, py = h->y * TILE;
	if (px > g->cam_x + w + TILE || py > g->cam_y + hh + TILE || px + HOUSE_W * TILE < g->cam_x - TILE || py + HOUSE_H * TILE < g->cam_y - TILE) return;
	for (int ly = 0; ly < HOUSE_H; ly++) for (int lx = 0; lx < HOUSE_W; lx++) {
		int k = house_tile(lx, ly);
		double x = px + lx * TILE, y = py + ly * TILE;
		if (k == HT_WALL) {
			cairo_set_source_rgb(cr, 0.42, 0.31, 0.23);
			cairo_rectangle(cr, x, y, TILE, TILE);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, 0.49, 0.36, 0.27);
			cairo_rectangle(cr, x + 2, y + 2, TILE - 4, TILE - 12);
			cairo_fill(cr);
			if (ly == 0) { set_rgb(cr, palette[h->color], 1); cairo_rectangle(cr, x, y, TILE, 6); cairo_fill(cr); }
			continue;
		}
		if ((lx + ly) % 2) cairo_set_source_rgb(cr, 0.77, 0.60, 0.42); else cairo_set_source_rgb(cr, 0.74, 0.58, 0.39);
		cairo_rectangle(cr, x, y, TILE, TILE);
		cairo_fill(cr);
		if (k == HT_DOOR) { cairo_set_source_rgb(cr, 0.54, 0.42, 0.29); cairo_rectangle(cr, x, y + 26, TILE, 6); cairo_fill(cr); }
	}
	set_rgb(cr, palette[h->color], 0.33);
	cairo_rectangle(cr, px + 2 * TILE + 8, py + 3 * TILE + 4, 4 * TILE - 16, 2 * TILE - 8);
	cairo_fill(cr);

	double dx = px + 3 * TILE, dy = py + TILE;
	cairo_set_source_rgb(cr, 0.35, 0.24, 0.15);
	cairo_rectangle(cr, dx + 2, dy + 6, 2 * TILE - 4, TILE - 6);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.13, 0.13, 0.13);
	cairo_rectangle(cr, dx + 16, dy - 4, 32, 20);
	cairo_fill(cr);
	bool active = false;
	if (!strcmp(h->id, g->me.id)) active = g->mode == MODE_DESK;
	else for (int i = 0; i < g->n_peers; i++) if (!strcmp(g->peers[i].id, h->id)) active = g->peers[i].sitting;
	if (active) cairo_set_source_rgb(cr, 0.35, 0.80 + sin(g->t * 6) * 0.08, 0.95);
	else cairo_set_source_rgb(cr, 0.18, 0.29, 0.42);
	cairo_rectangle(cr, dx + 19, dy - 1, 26, 14);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.23, 0.23, 0.23);
	cairo_rectangle(cr, dx + 28, dy + 16, 8, 4);
	cairo_fill(cr);

	cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
	cairo_rectangle(cr, px + 6 * TILE + 3, py + TILE + 3, TILE - 6, 2 * TILE - 6);
	cairo_fill(cr);
	set_rgb(cr, palette[h->color], 1);
	cairo_rectangle(cr, px + 6 * TILE + 3, py + TILE + 20, TILE - 6, 2 * TILE - 23);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.48, 0.29, 0.16);
	cairo_rectangle(cr, px + TILE + 10, py + TILE + 18, 12, 10);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.25, 0.54, 0.27);
	cairo_arc(cr, px + TILE + 16, py + TILE + 14, 10, 0, 2 * M_PI);
	cairo_fill(cr);

	char name[64];
	if (!strcmp(h->id, g->me.id)) snprintf(name, sizeof name, "your home");
	else snprintf(name, sizeof name, "%s's home", h->name);
	label(cr, name, px + HOUSE_W * TILE / 2.0, py - 8, 11, 0, 0, 0, 0.55);
}

static void draw_kernel(struct game *g, cairo_t *cr) {
	double x = CX * TILE, y = CY * TILE;
	cairo_pattern_t *p = cairo_pattern_create_radial(x, y, 4, x, y, 90);
	cairo_pattern_add_color_stop_rgba(p, 0, 0.71, 0.55, 1, 0.35 + sin(g->t * 2) * 0.1);
	cairo_pattern_add_color_stop_rgba(p, 1, 0.71, 0.55, 1, 0);
	cairo_set_source(cr, p);
	cairo_rectangle(cr, x - 90, y - 90, 180, 180);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	cairo_set_source_rgb(cr, 0.44, 0.44, 0.47);
	cairo_rectangle(cr, x - TILE, y - TILE, 2 * TILE, 2 * TILE);
	cairo_fill(cr);
	double bob = sin(g->t * 2) * 3;
	cairo_set_source_rgb(cr, 0.72, 0.61, 1);
	cairo_move_to(cr, x, y - 46 + bob);
	cairo_line_to(cr, x + 16, y - 6 + bob);
	cairo_line_to(cr, x, y + 10 + bob);
	cairo_line_to(cr, x - 16, y - 6 + bob);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.4);
	cairo_move_to(cr, x, y - 46 + bob);
	cairo_line_to(cr, x + 6, y - 8 + bob);
	cairo_line_to(cr, x, y + 4 + bob);
	cairo_close_path(cr);
	cairo_fill(cr);
	label(cr, "Kernel Square", x, y - 58, 12, 0.16, 0.08, 0.31, 0.7);
}

static void draw_item(struct game *g, cairo_t *cr, const struct item *it) {
	double bob = sin(g->t * 3 + it->x) * 2, x = it->x - 8, y = it->y - 8 + bob;
	cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
	cairo_save(cr);
	cairo_translate(cr, it->x, it->y + 10);
	cairo_scale(cr, 9, 3);
	cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_fill(cr);
	if (it->npc) cairo_set_source_rgb(cr, 0.42, 0.48, 0.54); else cairo_set_source_rgb(cr, 0.24, 0.35, 0.60);
	cairo_rectangle(cr, x, y, 16, 16);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.87, 0.90, 0.93);
	cairo_rectangle(cr, x + 3, y, 10, 6);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, x + 3, y + 9, 10, 6);
	cairo_fill(cr);
	if (hypot(it->x - g->px, it->y - g->py) < 160) label(cr, it->name, it->x, y - 6, 10, 0, 0, 0, 0.5);
}

static void draw_creature(struct game *g, cairo_t *cr, const struct creature *c) {
	if (c->pid < 0) return;
	double mb = c->rss_kb / 1024.0;
	if (c->runaway) {
		double r = 10 + fmin(20, mb / 60) + sin(g->t * 6 + c->phase) * 2;
		cairo_set_source_rgba(cr, 0.86, 0.24, 0.24, 0.25);
		cairo_arc(cr, c->x, c->y, r + 6, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 0.85, 0.28, 0.23);
		cairo_arc(cr, c->x, c->y, r, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_rectangle(cr, c->x - 5, c->y - 3, 3, 3);
		cairo_rectangle(cr, c->x + 2, c->y - 3, 3, 3);
		cairo_fill(cr);
		char s[80];
		snprintf(s, sizeof s, "%s  %.0f%% cpu", c->comm, c->cpu * 100);
		label(cr, s, c->x, c->y - r - 8, 10, 0.47, 0.08, 0.08, 0.8);
		return;
	}
	double r = 3 + fmin(7, sqrt(mb) / 3);
	const double *col = palette[g->me.color];
	set_rgb(cr, col, 0.25);
	cairo_arc(cr, c->x, c->y, r * 2.3, 0, 2 * M_PI);
	cairo_fill(cr);
	if (c->protected_) set_rgb(cr, col, 1); else cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_arc(cr, c->x, c->y, r, 0, 2 * M_PI);
	cairo_fill(cr);
	if (hypot(c->x - g->px, c->y - g->py) < 110) label(cr, c->comm, c->x, c->y - r - 8, 9, 0, 0, 0, 0.45);
}

struct body { double x, y, walk; int color, dir, lvl; bool moving, sitting, npc, you; const char *name, *bubble; };

static void draw_body(struct game *g, cairo_t *cr, const struct body *b) {
	double bob = b->moving ? fabs(sin(b->walk * 12)) * 3 : 0;
	double x = b->x, y = b->y - bob;
	cairo_set_source_rgba(cr, 0, 0, 0, 0.28);
	cairo_save(cr);
	cairo_translate(cr, b->x, b->y + 11);
	cairo_scale(cr, 10, 4);
	cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_fill(cr);
	set_rgb(cr, palette[b->color], 1);
	rounded(cr, x - 9, y - 6, 18, 18, 5);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.95, 0.83, 0.70);
	cairo_arc(cr, x, y - 13, 8, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.16, 0.16, 0.16);
	double ex = b->dir * 2;
	cairo_rectangle(cr, x - 3 + ex, y - 14, 2, 3);
	cairo_rectangle(cr, x + 2 + ex, y - 14, 2, 3);
	cairo_fill(cr);
	char tag[96];
	snprintf(tag, sizeof tag, "%s  Lv%d%s%s", b->name, b->lvl, b->npc ? "  npc" : "", b->sitting ? "  at desk" : "");
	if (b->you) label(cr, tag, b->x, y - 28, 11, 0.08, 0.16, 0.31, 0.85);
	else label(cr, tag, b->x, y - 28, 11, 0, 0, 0, 0.55);
	if (b->bubble) speech(cr, b->bubble, b->x, y - 48);
}

static int by_y(const void *a, const void *b) {
	double d = ((const struct body *)a)->y - ((const struct body *)b)->y;
	return (d > 0) - (d < 0);
}

/* ---------------- HUD ---------------- */

static void panel(cairo_t *cr, double x, double y, double w, double h) {
	cairo_set_source_rgba(cr, 0.06, 0.08, 0.12, 0.78);
	rounded(cr, x, y, w, h, 10);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
	cairo_set_line_width(cr, 1);
	rounded(cr, x + 0.5, y + 0.5, w - 1, h - 1, 10);
	cairo_stroke(cr);
}

static void draw_minimap(struct game *g, cairo_t *cr, double x, double y, double size, int w, int h) {
	static cairo_surface_t *mini;
	static int mini_houses = -1;
	if (!mini) mini = cairo_image_surface_create(CAIRO_FORMAT_RGB24, MAP_W, MAP_H);
	if (mini_houses != g->n_houses || g->ground_dirty) {
		static const double mc[7][3] = {
			{0.15, 0.33, 0.50}, {0.85, 0.77, 0.56}, {0.36, 0.60, 0.30}, {0.16, 0.38, 0.19},
			{0.56, 0.56, 0.60}, {0.36, 0.60, 0.30}, {0.72, 0.61, 1},
		};
		cairo_t *m = cairo_create(mini);
		for (int ty = 0; ty < MAP_H; ty++) for (int tx = 0; tx < MAP_W; tx++) {
			set_rgb(m, mc[g->terrain[ty * MAP_W + tx]], 1);
			cairo_rectangle(m, tx, ty, 1, 1);
			cairo_fill(m);
		}
		for (int i = 0; i < g->n_houses; i++) {
			set_rgb(m, palette[g->houses[i].color], 1);
			cairo_rectangle(m, g->houses[i].x, g->houses[i].y, HOUSE_W, HOUSE_H);
			cairo_fill(m);
		}
		cairo_destroy(m);
		mini_houses = g->n_houses;
	}
	panel(cr, x - 6, y - 6, size + 12, size + 12);
	double s = size / MAP_W;
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, s, s);
	cairo_set_source_surface(cr, mini, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
	cairo_paint(cr);
	cairo_restore(cr);
	for (int i = 0; i < MAX_NPCS; i++) {
		cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
		cairo_arc(cr, x + g->npcs[i].x / TILE * s, y + g->npcs[i].y / TILE * s, 2, 0, 2 * M_PI);
		cairo_fill(cr);
	}
	for (int i = 0; i < g->n_peers; i++) {
		set_rgb(cr, palette[g->peers[i].color], 1);
		cairo_arc(cr, x + g->peers[i].x / TILE * s, y + g->peers[i].y / TILE * s, 3, 0, 2 * M_PI);
		cairo_fill(cr);
	}
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_arc(cr, x + g->px / TILE * s, y + g->py / TILE * s, 4, 0, 2 * M_PI);
	cairo_fill(cr);
	set_rgb(cr, palette[g->me.color], 1);
	cairo_arc(cr, x + g->px / TILE * s, y + g->py / TILE * s, 2.5, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, x + g->cam_x / TILE * s, y + g->cam_y / TILE * s, w / (double)TILE * s, h / (double)TILE * s);
	cairo_stroke(cr);
}

static void draw_toasts(struct game *g, cairo_t *cr, double cx, double y) {
	for (int i = 0; i < 4; i++) {
		if (g->toast_until[i] < g->t || !g->toast[i][0]) continue;
		double a = fmin(1, (g->toast_until[i] - g->t) * 2);
		font(cr, 13, false);
		double w = text_w(cr, g->toast[i]) + 28;
		cairo_set_source_rgba(cr, 0.06, 0.08, 0.12, 0.85 * a);
		rounded(cr, cx - w / 2, y, w, 30, 15);
		cairo_fill(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, a);
		text(cr, g->toast[i], cx - w / 2 + 14, y + 20);
		y += 36;
	}
}

static void draw_hud(struct game *g, cairo_t *cr, int w, int h) {
	int lvl = game_level(g->me.xp);
	long lo = level_xp(lvl), hi = level_xp(lvl + 1);
	int st[4];
	character_stats(&g->me, st);
	char s[200];

	panel(cr, 16, 16, 300, 92);
	set_rgb(cr, palette[g->me.color], 1);
	cairo_arc(cr, 40, 44, 12, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	font(cr, 16, true);
	text(cr, g->me.name, 62, 42);
	font(cr, 12, false);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
	snprintf(s, sizeof s, "Level %d %s", lvl, character_class(&g->me));
	text(cr, s, 62, 59);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
	rounded(cr, 30, 72, 272, 7, 3.5);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.55, 0.78, 1);
	rounded(cr, 30, 72, fmax(7, 272.0 * (g->me.xp - lo) / (double)(hi - lo)), 7, 3.5);
	cairo_fill(cr);
	font(cr, 11, false);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.65);
	snprintf(s, sizeof s, "STR %d  INT %d  PER %d  AGI %d     %ld / %ld XP", st[0], st[1], st[2], st[3], g->me.xp - lo, hi - lo);
	text(cr, s, 30, 97);

	double ms = 170;
	draw_minimap(g, cr, w - ms - 22, 22, ms, w, h);

	int open = 0;
	for (int q = 0; q < Q_COUNT; q++) open += !(g->me.quests & (1u << q));
	double qy = 22 + ms + 20;
	int shown = open > 4 ? 4 : open;
	panel(cr, w - 330, qy, 314, 36 + (shown ? shown : 1) * 20);
	font(cr, 12, true);
	cairo_set_source_rgb(cr, 1, 1, 1);
	snprintf(s, sizeof s, "Quests  %d/%d", Q_COUNT - open, Q_COUNT);
	text(cr, s, w - 316, qy + 22);
	font(cr, 12, false);
	int row = 0;
	for (int q = 0; q < Q_COUNT && row < 4; q++) {
		if (g->me.quests & (1u << q)) continue;
		cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
		text(cr, quest_names[q], w - 316, qy + 44 + row * 20);
		snprintf(s, sizeof s, "+%d", quest_xp[q]);
		cairo_set_source_rgb(cr, 0.55, 0.78, 1);
		text(cr, s, w - 50, qy + 44 + row * 20);
		row++;
	}
	if (!open) { cairo_set_source_rgba(cr, 1, 1, 1, 0.8); text(cr, "All quests complete.", w - 316, qy + 44); }

	double cy = h - 40 - g->n_chat * 18;
	font(cr, 13, false);
	for (int i = 0; i < g->n_chat; i++) {
		double tw = text_w(cr, g->chat_log[i]);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
		rounded(cr, 14, cy + i * 18 - 13, tw + 12, 17, 4);
		cairo_fill(cr);
		set_rgb(cr, palette[g->chat_color[i]], 1);
		cairo_set_source_rgba(cr, 0.8 + palette[g->chat_color[i]][0] * 0.2, 0.8 + palette[g->chat_color[i]][1] * 0.2, 0.8 + palette[g->chat_color[i]][2] * 0.2, 1);
		text(cr, g->chat_log[i], 20, cy + i * 18);
	}
	if (g->mode == MODE_CHAT) {
		panel(cr, 14, h - 36, 420, 26);
		cairo_set_source_rgb(cr, 1, 1, 1);
		snprintf(s, sizeof s, "Say: %s%s", g->chat_input, fmod(g->t, 1) < 0.5 ? "_" : "");
		text(cr, s, 24, h - 18);
	}

	if (g->near_kind) {
		font(cr, 15, false);
		snprintf(s, sizeof s, "[E]  %s", g->near_label);
		double tw = text_w(cr, s) + 36;
		panel(cr, w / 2.0 - tw / 2, h - 100, tw, 36);
		cairo_set_source_rgb(cr, 1, 1, 1);
		text(cr, s, w / 2.0 - tw / 2 + 18, h - 77);
	}

	font(cr, 11, false);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
	snprintf(s, sizeof s, "WASD / click to move    E interact    T chat    realm: %s, %d online", net_mode(), 1 + g->n_peers);
	text(cr, s, w - text_w(cr, s) - 18, h - 14);

	draw_toasts(g, cr, w / 2.0, 20);
}

static void draw_create(struct game *g, cairo_t *cr, int w, int h) {
	cairo_set_source_rgba(cr, 0.03, 0.04, 0.08, 0.82);
	cairo_paint(cr);
	double cx = w / 2.0, y = h / 2.0 - 190;
	int st[4];
	character_stats(&g->me, st);
	char s[200];
	cairo_set_source_rgb(cr, 0.72, 0.61, 1);
	font(cr, 14, true);
	const char *k = "R E A L M O S";
	text(cr, k, cx - text_w(cr, k) / 2, y);
	cairo_set_source_rgb(cr, 1, 1, 1);
	font(cr, 30, true);
	k = "A new installation has entered the realm";
	text(cr, k, cx - text_w(cr, k) / 2, y + 48);
	font(cr, 15, false);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
	snprintf(s, sizeof s, "This machine has %d CPU cores, %d GB of memory and a %dx%d display.", g->me.cores, g->me.mem_gb, g->me.screen_w, g->me.screen_h);
	text(cr, s, cx - text_w(cr, s) / 2, y + 88);
	snprintf(s, sizeof s, "That makes it %s %s:  STR %d   INT %d   PER %d   AGI %d", strchr("AEIOU", character_class(&g->me)[0]) ? "an" : "a", character_class(&g->me), st[0], st[1], st[2], st[3]);
	text(cr, s, cx - text_w(cr, s) / 2, y + 112);

	panel(cr, cx - 220, y + 150, 440, 150);
	font(cr, 13, false);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
	text(cr, "Name this installation", cx - 196, y + 180);
	font(cr, 26, true);
	cairo_set_source_rgb(cr, 1, 1, 1);
	snprintf(s, sizeof s, "%s%s", g->me.name, fmod(g->t, 1) < 0.5 ? "_" : " ");
	text(cr, s, cx - 196, y + 216);
	font(cr, 13, false);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
	text(cr, "Colour  (Left / Right)", cx - 196, y + 250);
	for (int i = 0; i < 8; i++) {
		double sx = cx - 196 + i * 30 + 10, sy = y + 276;
		set_rgb(cr, palette[i], 1);
		cairo_arc(cr, sx, sy, 10, 0, 2 * M_PI);
		cairo_fill(cr);
		if (i == g->me.color) {
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_set_line_width(cr, 2.5);
			cairo_arc(cr, sx, sy, 14, 0, 2 * M_PI);
			cairo_stroke(cr);
		}
	}
	font(cr, 15, true);
	cairo_set_source_rgb(cr, 0.55, 0.78, 1);
	k = "Press Enter to begin";
	text(cr, k, cx - text_w(cr, k) / 2, y + 340);
}

void game_draw_world(struct game *g, cairo_t *cr, int w, int h) {
	if (g->ground_dirty) render_ground(g);
	double cx = g->px - w / 2.0, cy = g->py - h / 2.0;
	double mw = MAP_W * TILE - w, mh = MAP_H * TILE - h;
	g->cam_x = round(cx < 0 ? 0 : cx > mw ? mw : cx);
	g->cam_y = round(cy < 0 ? 0 : cy > mh ? mh : cy);

	cairo_set_source_rgb(cr, 0.11, 0.24, 0.37);
	cairo_paint(cr);
	cairo_save(cr);
	cairo_translate(cr, -g->cam_x, -g->cam_y);
	cairo_set_source_surface(cr, g->ground, 0, 0);
	cairo_rectangle(cr, g->cam_x, g->cam_y, w, h);
	cairo_fill(cr);

	int x0 = (int)(g->cam_x / TILE), y0 = (int)(g->cam_y / TILE);
	int x1 = x0 + w / TILE + 2, y1 = y0 + h / TILE + 2;
	cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
	for (int ty = y0; ty < y1 && ty < MAP_H; ty++) for (int tx = x0; tx < x1 && tx < MAP_W; tx++) {
		if (g->terrain[ty * MAP_W + tx] != WATER) continue;
		double o = sin(g->t * 1.5 + tx * 0.7 + ty * 0.3) * 6;
		cairo_rectangle(cr, tx * TILE + 8 + o, ty * TILE + 12 + tile_hash(tx, ty) % 10, 10, 2);
	}
	cairo_fill(cr);

	for (int i = 0; i < g->n_houses; i++) draw_house(g, cr, &g->houses[i], w, h);
	draw_kernel(g, cr);
	for (int i = 0; i < g->n_items; i++) draw_item(g, cr, &g->items[i]);
	for (int i = 0; i < g->n_creatures; i++) draw_creature(g, cr, &g->creatures[i]);

	struct body bodies[MAX_PEERS + MAX_NPCS + 1];
	int nb = 0;
	for (int i = 0; i < MAX_NPCS; i++) {
		struct npc *n = &g->npcs[i];
		bodies[nb++] = (struct body){ n->x, n->y, n->walk, n->color, n->dir, 3, n->moving, false, true, false, n->name, n->bubble_until > g->t ? n->bubble : NULL };
	}
	for (int i = 0; i < g->n_peers; i++) {
		struct peer *p = &g->peers[i];
		bodies[nb++] = (struct body){ p->x, p->y, g->t, p->color, p->dir ? p->dir : 1, p->lvl, p->moving, p->sitting, false, false, p->name, p->bubble_until > g->t ? p->bubble : NULL };
	}
	if (g->mode != MODE_CREATE)
		bodies[nb++] = (struct body){ g->px, g->py, g->walk, g->me.color, g->dir, game_level(g->me.xp), g->moving, g->mode == MODE_DESK, false, true, g->me.name, g->bubble_until > g->t ? g->bubble : NULL };
	qsort(bodies, nb, sizeof *bodies, by_y);
	for (int i = 0; i < nb; i++) draw_body(g, cr, &bodies[i]);

	if (g->has_target && g->mode == MODE_WORLD) {
		cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
		cairo_set_line_width(cr, 2);
		cairo_arc(cr, g->tx, g->ty, 8 + sin(g->t * 8) * 2, 0, 2 * M_PI);
		cairo_stroke(cr);
	}
	cairo_restore(cr);

	if (g->mode == MODE_CREATE) draw_create(g, cr, w, h);
	else if (g->mode != MODE_DESK) draw_hud(g, cr, w, h);
}

/* ---------------- desk bar ---------------- */

static double bar_button(struct game *g, cairo_t *cr, double x, const char *label_text, const char *action, bool accent) {
	font(cr, 13, accent);
	double w = text_w(cr, label_text) + 24;
	if (accent) cairo_set_source_rgba(cr, 0.55, 0.78, 1, 0.22); else cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
	rounded(cr, x, 6, w, 28, 8);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 1, 1, 1);
	text(cr, label_text, x + 12, 25);
	if (g->n_bar_btn < 8) {
		g->bar_btn[g->n_bar_btn].x0 = x;
		g->bar_btn[g->n_bar_btn].x1 = x + w;
		snprintf(g->bar_btn[g->n_bar_btn].action, sizeof g->bar_btn[0].action, "%s", action);
		g->n_bar_btn++;
	}
	return x + w + 6;
}

void game_draw_bar(struct game *g, cairo_t *cr, int w, int h) {
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.05, 0.07, 0.11, 0.94);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
	cairo_rectangle(cr, 0, h - 1, w, 1);
	cairo_fill(cr);

	g->n_bar_btn = 0;
	double x = 8;
	x = bar_button(g, cr, x, "Stand up", "stand", true);
	x += 8;
	x = bar_button(g, cr, x, "Terminal", "terminal", false);
	x = bar_button(g, cr, x, "Files", "files", false);
	x = bar_button(g, cr, x, "Editor", "editor", false);
	x = bar_button(g, cr, x, "Browser", "browser", false);

	char s[200];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	long used_kb = 0;
	for (int i = 0; i < g->n_creatures; i++) used_kb += g->creatures[i].rss_kb;
	snprintf(s, sizeof s, "%s  Lv%d    %d online    %02d:%02d", g->me.name, game_level(g->me.xp), 1 + g->n_peers, tm.tm_hour, tm.tm_min);
	font(cr, 13, false);
	double sw = text_w(cr, s);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
	text(cr, s, w - sw - 16, 25);
	set_rgb(cr, palette[g->me.color], 1);
	cairo_arc(cr, w - sw - 28, 20, 6, 0, 2 * M_PI);
	cairo_fill(cr);

	double right = w - sw - 50;
	for (int i = 0; i < 4; i++) {
		if (g->toast_until[i] < g->t || !g->toast[i][0]) continue;
		font(cr, 12, false);
		double tw = text_w(cr, g->toast[i]);
		if (x + 20 < right - tw) {
			cairo_set_source_rgb(cr, 0.55, 0.78, 1);
			text(cr, g->toast[i], right - tw, 25);
		}
		break;
	}
	(void)used_kb;
}
