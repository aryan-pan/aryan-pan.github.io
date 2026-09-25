#ifndef REALM_H
#define REALM_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>
#include <xkbcommon/xkbcommon.h>

/* ---------- world geometry ---------- */

#define TILE 32
#define MAP_W 112
#define MAP_H 112
#define HOUSE_W 8
#define HOUSE_H 7
#define MAX_HOUSES 64
#define MAX_PEERS 48
#define MAX_ITEMS 96
#define MAX_CREATURES 24
#define MAX_NPCS 3
#define ID_LEN 17
#define NAME_LEN 24
#define MAX_ITEM_BYTES (32 * 1024)

enum mode { MODE_CREATE, MODE_WORLD, MODE_CHAT, MODE_DESK };

struct house {
	char id[ID_LEN];
	char name[NAME_LEN];
	int color;
	int x, y; /* tile coordinates of the top-left corner */
};

struct peer {
	char id[ID_LEN];
	char name[NAME_LEN];
	char cls[NAME_LEN];
	int color, lvl, dir;
	bool sitting, moving;
	double x, y, walk;
	double last_seen;
	char bubble[128];
	double bubble_until;
};

struct item {
	char id[ID_LEN];
	char holder[ID_LEN];      /* installation that stores the file */
	char holder_name[NAME_LEN];
	char name[64];
	double x, y;
	double last_seen;
	unsigned char *data;      /* only set for items this installation holds */
	size_t len;
	bool npc;
};

struct creature {
	int pid;
	char comm[32];
	long rss_kb;
	double cpu;
	int hot_ticks;
	bool runaway, protected_, seen;
	double x, y, phase;
};

struct npc {
	char id[ID_LEN];
	char name[NAME_LEN];
	int color;
	double x, y, tx, ty, wait, stuck, walk, next_say, next_drop;
	bool has_target, moving;
	int dir;
	const char *const *lines;
	char bubble[128];
	double bubble_until;
};

struct character {
	char id[ID_LEN];
	char name[NAME_LEN];
	int color;
	long xp;
	uint32_t quests;
	long created;
	int cores, mem_gb, screen_w, screen_h;
	bool laptop;
};

struct game {
	enum mode mode;
	struct character me;
	char data_dir[512];
	char home[512];

	uint8_t terrain[MAP_W * MAP_H];
	cairo_surface_t *ground;   /* pre-rendered terrain */
	bool ground_dirty;
	struct house houses[MAX_HOUSES];
	int n_houses;

	double px, py, walk;
	int dir;
	bool moving;
	bool k_up, k_down, k_left, k_right;
	bool has_target;
	double tx, ty;
	double cam_x, cam_y;
	double t;

	struct peer peers[MAX_PEERS];
	int n_peers;
	struct item items[MAX_ITEMS];
	int n_items;
	struct creature creatures[MAX_CREATURES];
	int n_creatures;
	struct npc npcs[MAX_NPCS];

	char chat_input[160];
	char chat_log[8][200];
	int chat_color[8];
	int n_chat;
	char bubble[128];
	double bubble_until;

	char toast[4][160];
	double toast_until[4];

	/* nearest interaction target */
	int near_kind;
	int near_index;
	char near_label[160];
	int confirm_pid;
	double confirm_until;

	struct { double x0, x1; char action[16]; } bar_btn[8];
	int n_bar_btn;

	double proc_timer, outbox_timer, net_timer, announce_timer;
	long mem_total_kb;
};

/* game.c */
void game_init(struct game *g, int screen_w, int screen_h);
void game_tick(struct game *g, double dt);
void game_draw_world(struct game *g, cairo_t *cr, int w, int h);
void game_draw_bar(struct game *g, cairo_t *cr, int w, int h);
bool game_key(struct game *g, xkb_keysym_t sym, bool pressed);
void game_click(struct game *g, double sx, double sy, int w, int h);
void game_toast(struct game *g, const char *fmt, ...);
void game_gain_xp(struct game *g, int xp, const char *why);
void game_quest(struct game *g, int quest);
void game_on_stand(struct game *g);
void game_on_window_opened(struct game *g);
int game_level(long xp);

enum quest { Q_SIT, Q_WINDOW, Q_DROP, Q_LOOT, Q_SWAT, Q_SQUARE, Q_VISIT, Q_CHAT, Q_COUNT };
extern const char *const quest_names[Q_COUNT];
extern const int quest_xp[Q_COUNT];
extern const double palette[8][3];
extern const char *const palette_names[8];

/* character.c */
void character_load(struct game *g);
void character_save(struct game *g);
const char *character_class(const struct character *c);
void character_stats(const struct character *c, int out[4]);

/* procs.c */
void procs_scan(struct game *g, double dt);
bool procs_swat(struct game *g, int index);

/* net.c */
void net_init(struct game *g);
void net_poll(struct game *g);
void net_send_state(struct game *g);
void net_send_chat(struct game *g, const char *text);
void net_announce_items(struct game *g);
void net_request_pickup(struct game *g, struct item *it);
void net_item_removed(struct game *g, const char *item_id);
bool net_online(void);
const char *net_mode(void);

/* items (game.c) */
struct item *item_add_local(struct game *g, const char *name, const unsigned char *data, size_t len, double x, double y, bool npc);
void item_receive(struct game *g, const char *name, const unsigned char *data, size_t len, const char *from_name);
void items_save(struct game *g);
void items_load(struct game *g);
void item_remove_at(struct game *g, int index);
struct house *house_add(struct game *g, const char *id, const char *name, int color);

/* main.c: hooks the game uses to drive the compositor */
void realm_set_seated(bool seated);
void realm_launch(const char *what);
int realm_window_count(void);
double realm_now(void);
const char *realm_config(const char *key, const char *fallback);

/* helpers */
uint32_t fnv1a(const char *s);
void sanitize(char *s, size_t max);

#endif
