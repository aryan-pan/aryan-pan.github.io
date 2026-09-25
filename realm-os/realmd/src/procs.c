/* Real processes owned by the player, surfaced as creatures around their home. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "realm.h"

#define MAX_SCAN 1024
#define HOT_CPU 0.7
#define HOT_TICKS 5

struct sample { int pid; unsigned long long ticks; };
static struct sample prev[MAX_SCAN];
static int n_prev;

static const char *const essential[] = {
	"systemd", "(sd-pam)", "dbus-daemon", "dbus-broker", "pipewire", "pipewire-pulse", "wireplumber",
	"realmd", "login", "agetty", "gnome-keyring-d", "Xwayland", "xdg-desktop-por", "at-spi-bus-laun", NULL,
};

static bool is_ancestor(int pid) {
	int cur = getpid();
	for (int depth = 0; cur > 1 && depth < 32; depth++) {
		if (cur == pid) return true;
		char path[64], buf[512];
		snprintf(path, sizeof path, "/proc/%d/stat", cur);
		FILE *f = fopen(path, "r");
		if (!f) return false;
		size_t n = fread(buf, 1, sizeof buf - 1, f);
		fclose(f);
		buf[n] = 0;
		char *rp = strrchr(buf, ')');
		int ppid = 0;
		if (!rp || sscanf(rp + 2, "%*c %d", &ppid) != 1) return false;
		cur = ppid;
	}
	return false;
}

static bool is_essential(int pid, const char *comm) {
	if (pid <= 1 || is_ancestor(pid)) return true;
	for (int i = 0; essential[i]; i++) if (!strcmp(comm, essential[i])) return true;
	return false;
}

static struct creature *find(struct game *g, int pid) {
	for (int i = 0; i < g->n_creatures; i++) if (g->creatures[i].pid == pid) return &g->creatures[i];
	return NULL;
}

struct row { int pid; char comm[32]; long rss_kb; double cpu; };

static int by_rss(const void *a, const void *b) {
	const struct row *x = a, *y = b;
	return (y->rss_kb > x->rss_kb) - (y->rss_kb < x->rss_kb);
}

void procs_scan(struct game *g, double dt) {
	static struct row rows[MAX_SCAN];
	static struct sample next[MAX_SCAN];
	int n_rows = 0, n_next = 0;
	uid_t me = getuid();
	long page_kb = sysconf(_SC_PAGESIZE) / 1024;
	double hz = (double)sysconf(_SC_CLK_TCK);

	DIR *d = opendir("/proc");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && n_rows < MAX_SCAN) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		int pid = atoi(e->d_name);
		char path[64], buf[1024];
		struct stat st;
		snprintf(path, sizeof path, "/proc/%d", pid);
		if (stat(path, &st) || st.st_uid != me) continue;
		snprintf(path, sizeof path, "/proc/%d/stat", pid);
		FILE *f = fopen(path, "r");
		if (!f) continue;
		size_t n = fread(buf, 1, sizeof buf - 1, f);
		fclose(f);
		buf[n] = 0;
		char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
		if (!lp || !rp) continue;
		struct row *r = &rows[n_rows];
		size_t cl = (size_t)(rp - lp - 1);
		if (cl >= sizeof r->comm) cl = sizeof r->comm - 1;
		memcpy(r->comm, lp + 1, cl);
		r->comm[cl] = 0;
		unsigned long long ut = 0, stt = 0;
		long rss = 0;
		/* fields after ')': state(3) ... utime(14) stime(15) ... rss(24) */
		if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
				&ut, &stt, &rss) != 3) continue;
		if (rss == 0) continue; /* kernel threads and zombies */
		r->pid = pid;
		r->rss_kb = rss * page_kb;
		unsigned long long ticks = ut + stt;
		r->cpu = 0;
		for (int i = 0; i < n_prev; i++) if (prev[i].pid == pid) {
			r->cpu = dt > 0 ? (double)(ticks - prev[i].ticks) / hz / dt : 0;
			break;
		}
		next[n_next++] = (struct sample){ pid, ticks };
		n_rows++;
	}
	closedir(d);
	memcpy(prev, next, sizeof(struct sample) * n_next);
	n_prev = n_next;

	qsort(rows, n_rows, sizeof *rows, by_rss);
	/* Busy processes always get a slot, even when their memory is small. */
	for (int i = MAX_CREATURES; i < n_rows; i++) {
		if (rows[i].cpu > HOT_CPU) { struct row t = rows[MAX_CREATURES - 1]; rows[MAX_CREATURES - 1] = rows[i]; rows[i] = t; }
	}
	int take = n_rows < MAX_CREATURES ? n_rows : MAX_CREATURES;

	struct creature fresh[MAX_CREATURES];
	for (int i = 0; i < take; i++) {
		struct row *r = &rows[i];
		struct creature *old = find(g, r->pid);
		struct creature c = old ? *old : (struct creature){ .pid = r->pid, .phase = (r->pid % 628) / 100.0 };
		if (!old) c.x = c.y = -1;
		snprintf(c.comm, sizeof c.comm, "%s", r->comm);
		c.rss_kb = r->rss_kb;
		c.cpu = r->cpu;
		c.protected_ = is_essential(r->pid, r->comm);
		c.hot_ticks = r->cpu > HOT_CPU ? c.hot_ticks + 1 : 0;
		bool huge = g->mem_total_kb && r->rss_kb > g->mem_total_kb / 4;
		bool was = c.runaway;
		c.runaway = !c.protected_ && (c.hot_ticks >= HOT_TICKS || huge);
		if (c.runaway && !was) game_toast(g, "%s (pid %d) is running wild near your home", c.comm, c.pid);
		fresh[i] = c;
	}
	memcpy(g->creatures, fresh, sizeof(struct creature) * take);
	g->n_creatures = take;
}

bool procs_swat(struct game *g, int index) {
	if (index < 0 || index >= g->n_creatures) return false;
	struct creature *c = &g->creatures[index];
	if (c->protected_) { game_toast(g, "%s keeps this installation running; it can't be swatted", c->comm); return false; }
	if (kill(c->pid, SIGTERM) != 0) { game_toast(g, "%s resisted (%s)", c->comm, "permission denied"); return false; }
	if (c->runaway) {
		game_gain_xp(g, 25, c->comm);
		game_quest(g, Q_SWAT);
	} else {
		game_toast(g, "Swatted %s (pid %d)", c->comm, c->pid);
	}
	c->pid = -1;
	c->x = -1000;
	return true;
}
