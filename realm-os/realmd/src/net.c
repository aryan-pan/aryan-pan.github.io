/*
 * Realm networking over UDP. Without configuration every installation on the
 * local network broadcasts to the others; with `server=host:port` in
 * realm.conf all traffic goes through a relay (see server/realm-relay.py).
 *
 * Packets are tab-separated text: TYPE, sender id, then type-specific fields.
 *   S id name color lvl class x y sitting dir moving   position, 10 Hz
 *   C id name color text                                chat
 *   I id item x y name holder_name                      a file lying in the world, 1 Hz
 *   P id item                                           request to pick an item up
 *   G id item target name base64                        item handed to target
 *   R id item                                           item no longer in the world
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "realm.h"

#define REALM_PORT 4777
#define PKT_MAX 65000

static int sock = -1;
static struct sockaddr_in dest;
static bool relay;
static double last_rx;

bool net_online(void) { return sock >= 0 && realm_now() - last_rx < 5; }
const char *net_mode(void) { return sock < 0 ? "offline" : relay ? "realm server" : "local network"; }

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const unsigned char *in, size_t len, char *out) {
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = in[i] << 16 | (i + 1 < len ? in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
		out[o++] = b64chars[v >> 18 & 63];
		out[o++] = b64chars[v >> 12 & 63];
		out[o++] = i + 1 < len ? b64chars[v >> 6 & 63] : '=';
		out[o++] = i + 2 < len ? b64chars[v & 63] : '=';
	}
	out[o] = 0;
	return o;
}

static size_t b64_decode(const char *in, unsigned char *out, size_t max) {
	uint32_t v = 0;
	int bits = 0;
	size_t o = 0;
	for (; *in && *in != '='; in++) {
		const char *p = strchr(b64chars, *in);
		if (!p) continue;
		v = v << 6 | (uint32_t)(p - b64chars);
		bits += 6;
		if (bits >= 8) { bits -= 8; if (o < max) out[o++] = (v >> bits) & 0xff; }
	}
	return o;
}

void net_init(struct game *g) {
	(void)g;
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) return;
	fcntl(sock, F_SETFL, O_NONBLOCK);
	int one = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);

	const char *server = getenv("REALM_SERVER");
	if (!server) server = realm_config("server", "");
	memset(&dest, 0, sizeof dest);
	dest.sin_family = AF_INET;
	if (server && *server) {
		char host[256];
		snprintf(host, sizeof host, "%s", server);
		int port = REALM_PORT;
		char *colon = strrchr(host, ':');
		if (colon) { *colon = 0; port = atoi(colon + 1); }
		struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
		if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
			dest.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
			dest.sin_port = htons(port);
			relay = true;
			freeaddrinfo(res);
			return;
		}
		fprintf(stderr, "realmd: cannot resolve realm server %s, using the local network\n", server);
	}
	struct sockaddr_in bind_addr = { .sin_family = AF_INET, .sin_port = htons(REALM_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (bind(sock, (struct sockaddr *)&bind_addr, sizeof bind_addr) < 0) {
		fprintf(stderr, "realmd: cannot bind UDP %d: %s\n", REALM_PORT, strerror(errno));
		close(sock);
		sock = -1;
		return;
	}
	dest.sin_port = htons(REALM_PORT);
	const char *bcast = realm_config("broadcast", "255.255.255.255");
	inet_pton(AF_INET, bcast, &dest.sin_addr);
}

static void sendf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sendf(const char *fmt, ...) {
	if (sock < 0) return;
	static char buf[PKT_MAX];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n <= 0 || n >= (int)sizeof buf) return;
	sendto(sock, buf, n, 0, (struct sockaddr *)&dest, sizeof dest);
}

void net_send_state(struct game *g) {
	sendf("S\t%s\t%s\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d", g->me.id, g->me.name, g->me.color, game_level(g->me.xp),
		character_class(&g->me), (int)g->px, (int)g->py, g->mode == MODE_DESK, g->dir, g->moving);
}

void net_send_chat(struct game *g, const char *text) {
	sendf("C\t%s\t%s\t%d\t%s", g->me.id, g->me.name, g->me.color, text);
}

void net_announce_items(struct game *g) {
	for (int i = 0; i < g->n_items; i++) {
		struct item *it = &g->items[i];
		if (it->npc || strcmp(it->holder, g->me.id)) continue;
		sendf("I\t%s\t%s\t%d\t%d\t%s\t%s", g->me.id, it->id, (int)it->x, (int)it->y, it->name, g->me.name);
	}
}

void net_request_pickup(struct game *g, struct item *it) {
	sendf("P\t%s\t%s", g->me.id, it->id);
}

void net_item_removed(struct game *g, const char *item_id) {
	sendf("R\t%s\t%s", g->me.id, item_id);
}

static int split(char *s, char **f, int max) {
	int n = 0;
	f[n++] = s;
	for (char *p = s; *p && n < max; p++) if (*p == '\t') { *p = 0; f[n++] = p + 1; }
	return n;
}

static struct peer *peer_get(struct game *g, const char *id) {
	for (int i = 0; i < g->n_peers; i++) if (!strcmp(g->peers[i].id, id)) return &g->peers[i];
	if (g->n_peers >= MAX_PEERS) return NULL;
	struct peer *p = &g->peers[g->n_peers++];
	memset(p, 0, sizeof *p);
	snprintf(p->id, ID_LEN, "%s", id);
	return p;
}

static int item_index(struct game *g, const char *id) {
	for (int i = 0; i < g->n_items; i++) if (!strcmp(g->items[i].id, id)) return i;
	return -1;
}

static void chat_push(struct game *g, const char *name, int color, const char *text) {
	if (g->n_chat == 8) {
		memmove(g->chat_log[0], g->chat_log[1], sizeof g->chat_log[0] * 7);
		memmove(g->chat_color, g->chat_color + 1, sizeof(int) * 7);
		g->n_chat = 7;
	}
	snprintf(g->chat_log[g->n_chat], sizeof g->chat_log[0], "%s: %s", name, text);
	g->chat_color[g->n_chat++] = color & 7;
}

void net_poll(struct game *g) {
	if (sock < 0) return;
	static char buf[PKT_MAX + 1];
	static unsigned char bin[MAX_ITEM_BYTES];
	for (int budget = 0; budget < 256; budget++) {
		ssize_t n = recv(sock, buf, PKT_MAX, 0);
		if (n <= 0) break;
		buf[n] = 0;
		char *f[12];
		int nf = split(buf, f, 12);
		if (nf < 2 || strlen(f[1]) >= ID_LEN || !strcmp(f[1], g->me.id)) continue;
		last_rx = realm_now();
		char type = f[0][0];
		if (type == 'S' && nf >= 11) {
			struct peer *p = peer_get(g, f[1]);
			if (!p) continue;
			snprintf(p->name, NAME_LEN, "%s", f[2]);
			sanitize(p->name, NAME_LEN);
			p->color = atoi(f[3]) & 7;
			p->lvl = atoi(f[4]);
			snprintf(p->cls, NAME_LEN, "%s", f[5]);
			double nx = atof(f[6]), ny = atof(f[7]);
			if (p->last_seen == 0) { p->x = nx; p->y = ny; }
			p->x = nx; p->y = ny;
			p->sitting = atoi(f[8]);
			p->dir = atoi(f[9]);
			p->moving = atoi(f[10]);
			p->last_seen = realm_now();
			house_add(g, p->id, p->name, p->color);
		} else if (type == 'C' && nf >= 5) {
			char text[128];
			snprintf(text, sizeof text, "%s", f[4]);
			sanitize(text, sizeof text);
			chat_push(g, f[2], atoi(f[3]), text);
			struct peer *p = peer_get(g, f[1]);
			if (p) { snprintf(p->bubble, sizeof p->bubble, "%s", text); p->bubble_until = g->t + 6; }
		} else if (type == 'I' && nf >= 7) {
			int i = item_index(g, f[2]);
			if (i < 0) {
				if (g->n_items >= MAX_ITEMS || strlen(f[2]) >= ID_LEN) continue;
				i = g->n_items++;
				memset(&g->items[i], 0, sizeof g->items[i]);
				snprintf(g->items[i].id, ID_LEN, "%s", f[2]);
			}
			struct item *it = &g->items[i];
			if (it->data) continue; /* never let a peer rewrite an item we hold */
			snprintf(it->holder, ID_LEN, "%s", f[1]);
			snprintf(it->holder_name, NAME_LEN, "%s", f[6]);
			snprintf(it->name, sizeof it->name, "%s", f[5]);
			sanitize(it->name, sizeof it->name);
			it->x = atof(f[3]);
			it->y = atof(f[4]);
			it->last_seen = realm_now();
		} else if (type == 'P' && nf >= 3) {
			int i = item_index(g, f[2]);
			if (i < 0 || !g->items[i].data || g->items[i].npc) continue;
			struct item *it = &g->items[i];
			static char enc[MAX_ITEM_BYTES * 4 / 3 + 8];
			b64_encode(it->data, it->len, enc);
			sendf("G\t%s\t%s\t%s\t%s\t%s", g->me.id, it->id, f[1], it->name, enc);
			net_item_removed(g, it->id);
			struct peer *p = peer_get(g, f[1]);
			game_toast(g, "%s picked up your %s", p && p->name[0] ? p->name : "someone", it->name);
			item_remove_at(g, i);
			items_save(g);
		} else if (type == 'G' && nf >= 6) {
			if (strcmp(f[3], g->me.id)) continue;
			size_t len = b64_decode(f[5], bin, sizeof bin);
			struct peer *p = peer_get(g, f[1]);
			item_receive(g, f[4], bin, len, p && p->name[0] ? p->name : "another install");
			int i = item_index(g, f[2]);
			if (i >= 0) item_remove_at(g, i);
		} else if (type == 'R' && nf >= 3) {
			int i = item_index(g, f[2]);
			if (i >= 0 && !g->items[i].data) item_remove_at(g, i);
		}
	}
}
