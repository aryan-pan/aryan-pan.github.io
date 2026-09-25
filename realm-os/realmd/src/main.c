/*
 * realmd — the RealmOS session. A Wayland compositor whose idle state is a
 * game world; sitting at your desk in that world reveals a normal desktop
 * where real Wayland applications run.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <drm_fourcc.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "realm.h"

#define BAR_H 40
#define TICK_MS 33

/* ---------------- cairo-backed wlr_buffer ---------------- */

struct cbuf {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void cbuf_destroy(struct wlr_buffer *b) {
	struct cbuf *c = wl_container_of(b, c, base);
	cairo_surface_destroy(c->surface);
	free(c);
}

static bool cbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct cbuf *c = wl_container_of(b, c, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
	*data = cairo_image_surface_get_data(c->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(c->surface);
	return true;
}

static void cbuf_end(struct wlr_buffer *b) { (void)b; }

static const struct wlr_buffer_impl cbuf_impl = {
	.destroy = cbuf_destroy,
	.begin_data_ptr_access = cbuf_begin,
	.end_data_ptr_access = cbuf_end,
};

static struct cbuf *cbuf_create(int w, int h) {
	struct cbuf *c = calloc(1, sizeof(*c));
	wlr_buffer_init(&c->base, &cbuf_impl, w, h);
	c->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	return c;
}

/* ---------------- compositor state ---------------- */

enum cursor_mode { CURSOR_PASSTHROUGH, CURSOR_MOVE, CURSOR_RESIZE };

struct toplevel {
	struct wl_list link;
	struct wlr_xdg_toplevel *xdg;
	struct wlr_scene_tree *tree;
	struct wlr_box saved;
	bool maximized, placed;
	struct wl_listener map, unmap, destroy, commit;
	struct wl_listener request_move, request_resize, request_maximize, request_fullscreen;
};

struct keyboard {
	struct wl_list link;
	struct wlr_keyboard *wlr;
	struct wl_listener modifiers, key, destroy;
};

struct output {
	struct wl_list link;
	struct wlr_output *wlr;
	struct wl_listener frame, request_state, destroy;
};

static struct {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *layout;
	struct wl_list outputs;

	struct wlr_scene_buffer *world_node, *bar_node;
	struct cbuf *world_buf, *bar_buf;
	struct wlr_scene_tree *desk_tree, *windows_tree;
	struct wlr_scene_rect *dim;
	int w, h;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_list toplevels;
	int placed_count;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	enum cursor_mode cursor_mode;
	struct toplevel *grabbed;
	double grab_x, grab_y;
	struct wlr_box grab_box;
	uint32_t resize_edges;

	struct wlr_seat *seat;
	struct wl_list keyboards;

	struct wl_event_source *timer;
	double last_tick;
	int frame_no;

	struct wl_listener new_output, new_input, new_xdg_surface, new_virtual_keyboard;
	struct wl_listener cursor_motion, cursor_motion_absolute, cursor_button, cursor_axis, cursor_frame;
	struct wl_listener request_cursor, request_set_selection, request_set_primary, request_set_shape;
	struct wl_listener new_decoration;

	struct game game;
	char config[32][2][256];
	int n_config;
} S;

double realm_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------------- configuration ---------------- */

static void config_read(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[600];
	while (fgets(line, sizeof line, f) && S.n_config < 32) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq) continue;
		*eq = 0;
		char *v = eq + 1;
		v[strcspn(v, "\r\n")] = 0;
		int i;
		for (i = 0; i < S.n_config; i++) if (!strcmp(S.config[i][0], line)) break;
		if (i == S.n_config) S.n_config++;
		snprintf(S.config[i][0], 256, "%.255s", line);
		snprintf(S.config[i][1], 256, "%.255s", v);
	}
	fclose(f);
}

const char *realm_config(const char *key, const char *fallback) {
	for (int i = 0; i < S.n_config; i++) if (!strcmp(S.config[i][0], key)) return S.config[i][1];
	return fallback;
}

void realm_launch(const char *what) {
	static const char *defaults[][2] = {
		{"terminal", "foot"}, {"files", "thunar"}, {"editor", "mousepad"}, {"browser", "epiphany-browser"},
	};
	const char *cmd = NULL;
	for (size_t i = 0; i < sizeof defaults / sizeof *defaults; i++)
		if (!strcmp(defaults[i][0], what)) cmd = realm_config(what, defaults[i][1]);
	if (!cmd) cmd = what;
	wlr_log(WLR_INFO, "launching %s", cmd);
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGCHLD, SIG_DFL);
		if (chdir(getenv("HOME") ? getenv("HOME") : "/") != 0) _exit(126);
		execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

int realm_window_count(void) { return wl_list_length(&S.toplevels); }

/* ---------------- focus & seating ---------------- */

static void focus_toplevel(struct toplevel *t) {
	if (!t) return;
	struct wlr_surface *surface = t->xdg->base->surface;
	struct wlr_surface *prev = S.seat->keyboard_state.focused_surface;
	if (prev == surface) return;
	if (prev) {
		struct wlr_xdg_toplevel *p = wlr_xdg_toplevel_try_from_wlr_surface(prev);
		if (p) wlr_xdg_toplevel_set_activated(p, false);
	}
	wlr_scene_node_raise_to_top(&t->tree->node);
	wl_list_remove(&t->link);
	wl_list_insert(&S.toplevels, &t->link);
	wlr_xdg_toplevel_set_activated(t->xdg, true);
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(S.seat);
	if (kb) wlr_seat_keyboard_notify_enter(S.seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
	else wlr_seat_keyboard_notify_enter(S.seat, surface, NULL, 0, NULL);
}

void realm_set_seated(bool seated) {
	struct game *g = &S.game;
	wlr_scene_node_set_enabled(&S.desk_tree->node, seated);
	if (seated) {
		g->mode = MODE_DESK;
		if (!wl_list_empty(&S.toplevels)) {
			struct toplevel *t = wl_container_of(S.toplevels.next, t, link);
			S.seat->keyboard_state.focused_surface = NULL;
			focus_toplevel(t);
		}
	} else {
		g->mode = MODE_WORLD;
		S.cursor_mode = CURSOR_PASSTHROUGH;
		S.grabbed = NULL;
		wlr_seat_keyboard_notify_clear_focus(S.seat);
		wlr_seat_pointer_notify_clear_focus(S.seat);
		wlr_cursor_set_xcursor(S.cursor, S.cursor_mgr, "default");
		game_on_stand(g);
	}
}

static void cycle_windows(void) {
	if (wl_list_length(&S.toplevels) < 2) return;
	struct toplevel *t = wl_container_of(S.toplevels.prev, t, link);
	focus_toplevel(t);
}

static void set_maximized(struct toplevel *t, bool on) {
	if (!t->xdg->base->initialized) return;
	if (on && !t->maximized) {
		wlr_xdg_surface_get_geometry(t->xdg->base, &t->saved);
		t->saved.x = t->tree->node.x;
		t->saved.y = t->tree->node.y;
		wlr_scene_node_set_position(&t->tree->node, 0, BAR_H);
		wlr_xdg_toplevel_set_size(t->xdg, S.w, S.h - BAR_H);
	} else if (!on && t->maximized) {
		wlr_scene_node_set_position(&t->tree->node, t->saved.x, t->saved.y);
		wlr_xdg_toplevel_set_size(t->xdg, t->saved.width, t->saved.height);
	}
	t->maximized = on;
	wlr_xdg_toplevel_set_maximized(t->xdg, on);
}

/* ---------------- keyboard ---------------- */

static void keyboard_modifiers(struct wl_listener *l, void *data) {
	struct keyboard *k = wl_container_of(l, k, modifiers);
	wlr_seat_set_keyboard(S.seat, k->wlr);
	wlr_seat_keyboard_notify_modifiers(S.seat, &k->wlr->modifiers);
}

static bool desk_binding(xkb_keysym_t sym) {
	struct toplevel *top = wl_list_empty(&S.toplevels) ? NULL : wl_container_of(S.toplevels.next, top, link);
	switch (sym) {
	case XKB_KEY_Escape: realm_set_seated(false); return true;
	case XKB_KEY_Return: realm_launch("terminal"); return true;
	case XKB_KEY_e: realm_launch("files"); return true;
	case XKB_KEY_b: realm_launch("browser"); return true;
	case XKB_KEY_Tab: cycle_windows(); return true;
	case XKB_KEY_q: if (top) wlr_xdg_toplevel_send_close(top->xdg); return true;
	case XKB_KEY_f: case XKB_KEY_Up: if (top) set_maximized(top, !top->maximized); return true;
	default: return false;
	}
}

static void keyboard_key(struct wl_listener *l, void *data) {
	struct keyboard *k = wl_container_of(l, k, key);
	struct wlr_keyboard_key_event *ev = data;
	const xkb_keysym_t *syms;
	int n = xkb_state_key_get_syms(k->wlr->xkb_state, ev->keycode + 8, &syms);
	uint32_t mods = wlr_keyboard_get_modifiers(k->wlr);
	bool pressed = ev->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	bool handled = false;

	for (int i = 0; i < n && pressed; i++) {
		xkb_keysym_t s = syms[i];
		if (s >= XKB_KEY_XF86Switch_VT_1 && s <= XKB_KEY_XF86Switch_VT_12) {
			if (S.session) wlr_session_change_vt(S.session, s - XKB_KEY_XF86Switch_VT_1 + 1);
			handled = true;
		} else if (s == XKB_KEY_BackSpace && (mods & WLR_MODIFIER_CTRL) && (mods & WLR_MODIFIER_ALT)) {
			wl_display_terminate(S.display);
			handled = true;
		} else if (S.game.mode == MODE_DESK && (mods & WLR_MODIFIER_LOGO)) {
			handled |= desk_binding(s);
		}
	}
	if (handled) return;

	if (S.game.mode == MODE_DESK) {
		wlr_seat_set_keyboard(S.seat, k->wlr);
		wlr_seat_keyboard_notify_key(S.seat, ev->time_msec, ev->keycode, ev->state);
		return;
	}
	/* Outside the desk, the keyboard drives the character. Use the unshifted
	 * keysym for movement and the shifted one for typed text. */
	xkb_keysym_t sym = n > 0 ? syms[0] : XKB_KEY_NoSymbol;
	if (S.game.mode == MODE_WORLD) sym = xkb_keysym_to_lower(sym);
	game_key(&S.game, sym, pressed);
}

static void keyboard_destroy(struct wl_listener *l, void *data) {
	struct keyboard *k = wl_container_of(l, k, destroy);
	wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->key.link);
	wl_list_remove(&k->destroy.link);
	wl_list_remove(&k->link);
	if (wlr_seat_get_keyboard(S.seat) == k->wlr) {
		struct keyboard *other = wl_list_empty(&S.keyboards) ? NULL : wl_container_of(S.keyboards.next, other, link);
		wlr_seat_set_keyboard(S.seat, other ? other->wlr : NULL);
	}
	free(k);
}

static void add_keyboard(struct wlr_keyboard *wlr, bool set_keymap) {
	struct keyboard *k = calloc(1, sizeof(*k));
	k->wlr = wlr;
	if (set_keymap) {
		struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_rule_names names = {
			.layout = getenv("XKB_DEFAULT_LAYOUT") ? getenv("XKB_DEFAULT_LAYOUT") : realm_config("keyboard_layout", NULL),
		};
		struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
		wlr_keyboard_set_keymap(wlr, km);
		xkb_keymap_unref(km);
		xkb_context_unref(ctx);
		wlr_keyboard_set_repeat_info(wlr, 25, 600);
	}
	k->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&wlr->events.modifiers, &k->modifiers);
	k->key.notify = keyboard_key;
	wl_signal_add(&wlr->events.key, &k->key);
	k->destroy.notify = keyboard_destroy;
	wl_signal_add(&wlr->base.events.destroy, &k->destroy);
	wlr_seat_set_keyboard(S.seat, wlr);
	wl_list_insert(&S.keyboards, &k->link);
	wlr_seat_set_capabilities(S.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

static void new_input(struct wl_listener *l, void *data) {
	struct wlr_input_device *dev = data;
	if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) add_keyboard(wlr_keyboard_from_input_device(dev), true);
	else if (dev->type == WLR_INPUT_DEVICE_POINTER) wlr_cursor_attach_input_device(S.cursor, dev);
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&S.keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(S.seat, caps);
}

static void new_virtual_keyboard(struct wl_listener *l, void *data) {
	struct wlr_virtual_keyboard_v1 *vk = data;
	add_keyboard(&vk->keyboard, false);
}

/* ---------------- pointer ---------------- */

static struct toplevel *toplevel_at(double lx, double ly, struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(&S.windows_tree->node, lx, ly, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	if (!ss) return NULL;
	*surface = ss->surface;
	struct wlr_scene_tree *tree = node->parent;
	while (tree && !tree->node.data) tree = tree->node.parent;
	return tree ? tree->node.data : NULL;
}

static void process_motion(uint32_t time) {
	if (S.game.mode != MODE_DESK) {
		wlr_cursor_set_xcursor(S.cursor, S.cursor_mgr, "default");
		return;
	}
	if (S.cursor_mode == CURSOR_MOVE) {
		wlr_scene_node_set_position(&S.grabbed->tree->node, S.cursor->x - S.grab_x, S.cursor->y - S.grab_y);
		return;
	}
	if (S.cursor_mode == CURSOR_RESIZE) {
		struct toplevel *t = S.grabbed;
		double bx = S.cursor->x - S.grab_x, by = S.cursor->y - S.grab_y;
		int left = S.grab_box.x, right = S.grab_box.x + S.grab_box.width;
		int top = S.grab_box.y, bottom = S.grab_box.y + S.grab_box.height;
		if (S.resize_edges & WLR_EDGE_TOP) { top = by; if (top >= bottom) top = bottom - 1; }
		else if (S.resize_edges & WLR_EDGE_BOTTOM) { bottom = by; if (bottom <= top) bottom = top + 1; }
		if (S.resize_edges & WLR_EDGE_LEFT) { left = bx; if (left >= right) left = right - 1; }
		else if (S.resize_edges & WLR_EDGE_RIGHT) { right = bx; if (right <= left) right = left + 1; }
		struct wlr_box geo;
		wlr_xdg_surface_get_geometry(t->xdg->base, &geo);
		wlr_scene_node_set_position(&t->tree->node, left - geo.x, top - geo.y);
		wlr_xdg_toplevel_set_size(t->xdg, right - left, bottom - top);
		return;
	}
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct toplevel *t = toplevel_at(S.cursor->x, S.cursor->y, &surface, &sx, &sy);
	if (!t) wlr_cursor_set_xcursor(S.cursor, S.cursor_mgr, "default");
	if (surface) {
		wlr_seat_pointer_notify_enter(S.seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(S.seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(S.seat);
	}
}

static void cursor_motion(struct wl_listener *l, void *data) {
	struct wlr_pointer_motion_event *ev = data;
	wlr_cursor_move(S.cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
	process_motion(ev->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *l, void *data) {
	struct wlr_pointer_motion_absolute_event *ev = data;
	wlr_cursor_warp_absolute(S.cursor, &ev->pointer->base, ev->x, ev->y);
	process_motion(ev->time_msec);
}

static void begin_interactive(struct toplevel *t, enum cursor_mode mode, uint32_t edges) {
	S.grabbed = t;
	S.cursor_mode = mode;
	if (mode == CURSOR_MOVE) {
		S.grab_x = S.cursor->x - t->tree->node.x;
		S.grab_y = S.cursor->y - t->tree->node.y;
		return;
	}
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(t->xdg->base, &geo);
	double bx = t->tree->node.x + geo.x + ((edges & WLR_EDGE_RIGHT) ? geo.width : 0);
	double by = t->tree->node.y + geo.y + ((edges & WLR_EDGE_BOTTOM) ? geo.height : 0);
	S.grab_x = S.cursor->x - bx;
	S.grab_y = S.cursor->y - by;
	S.grab_box = geo;
	S.grab_box.x += t->tree->node.x;
	S.grab_box.y += t->tree->node.y;
	S.resize_edges = edges;
}

static void cursor_button(struct wl_listener *l, void *data) {
	struct wlr_pointer_button_event *ev = data;
	struct game *g = &S.game;
	if (g->mode != MODE_DESK) {
		if (ev->state == WLR_BUTTON_PRESSED) game_click(g, S.cursor->x, S.cursor->y, S.w, S.h);
		return;
	}
	if (ev->state == WLR_BUTTON_RELEASED) {
		if (S.cursor_mode != CURSOR_PASSTHROUGH) { S.cursor_mode = CURSOR_PASSTHROUGH; S.grabbed = NULL; return; }
		wlr_seat_pointer_notify_button(S.seat, ev->time_msec, ev->button, ev->state);
		return;
	}
	if (S.cursor->y < BAR_H) {
		game_click(g, S.cursor->x, S.cursor->y, S.w, S.h);
		return;
	}
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct toplevel *t = toplevel_at(S.cursor->x, S.cursor->y, &surface, &sx, &sy);
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(S.seat);
	if (t && kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_LOGO)) {
		focus_toplevel(t);
		begin_interactive(t, CURSOR_MOVE, 0);
		return;
	}
	wlr_seat_pointer_notify_button(S.seat, ev->time_msec, ev->button, ev->state);
	focus_toplevel(t);
}

static void cursor_axis(struct wl_listener *l, void *data) {
	struct wlr_pointer_axis_event *ev = data;
	if (S.game.mode != MODE_DESK) return;
	wlr_seat_pointer_notify_axis(S.seat, ev->time_msec, ev->orientation, ev->delta, ev->delta_discrete, ev->source);
}

static void cursor_frame(struct wl_listener *l, void *data) {
	if (S.game.mode == MODE_DESK) wlr_seat_pointer_notify_frame(S.seat);
}

static void request_cursor(struct wl_listener *l, void *data) {
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	if (S.game.mode == MODE_DESK && S.seat->pointer_state.focused_client == ev->seat_client)
		wlr_cursor_set_surface(S.cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void request_set_shape(struct wl_listener *l, void *data) {
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
	if (S.game.mode == MODE_DESK && S.seat->pointer_state.focused_client == ev->seat_client)
		wlr_cursor_set_xcursor(S.cursor, S.cursor_mgr, wlr_cursor_shape_v1_name(ev->shape));
}

static void request_set_selection(struct wl_listener *l, void *data) {
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(S.seat, ev->source, ev->serial);
}

static void request_set_primary(struct wl_listener *l, void *data) {
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(S.seat, ev->source, ev->serial);
}

/* ---------------- outputs ---------------- */

static void output_frame(struct wl_listener *l, void *data) {
	struct output *o = wl_container_of(l, o, frame);
	struct wlr_scene_output *so = wlr_scene_get_scene_output(S.scene, o->wlr);
	wlr_scene_output_commit(so, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
}

static void output_request_state(struct wl_listener *l, void *data) {
	struct output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *ev = data;
	wlr_output_commit_state(o->wlr, ev->state);
}

static void output_destroy(struct wl_listener *l, void *data) {
	struct output *o = wl_container_of(l, o, destroy);
	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	wl_list_remove(&o->link);
	free(o);
}

static void new_output(struct wl_listener *l, void *data) {
	struct wlr_output *wlr = data;
	wlr_output_init_render(wlr, S.allocator, S.renderer);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr);
	if (mode) wlr_output_state_set_mode(&state, mode);
	wlr_output_commit_state(wlr, &state);
	wlr_output_state_finish(&state);

	struct output *o = calloc(1, sizeof(*o));
	o->wlr = wlr;
	o->frame.notify = output_frame;
	wl_signal_add(&wlr->events.frame, &o->frame);
	o->request_state.notify = output_request_state;
	wl_signal_add(&wlr->events.request_state, &o->request_state);
	o->destroy.notify = output_destroy;
	wl_signal_add(&wlr->events.destroy, &o->destroy);
	wl_list_insert(S.outputs.prev, &o->link);

	struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(S.layout, wlr);
	struct wlr_scene_output *so = wlr_scene_output_create(S.scene, wlr);
	wlr_scene_output_layout_add_output(S.scene_layout, lo, so);
}

/* Keeps the world and bar buffers the size of the first output. */
static void sync_size(void) {
	int w = 1280, h = 720;
	if (!wl_list_empty(&S.outputs)) {
		struct output *o = wl_container_of(S.outputs.next, o, link);
		wlr_output_effective_resolution(o->wlr, &w, &h);
	}
	if (w == S.w && h == S.h && S.world_buf) return;
	S.w = w;
	S.h = h;
	struct cbuf *old_world = S.world_buf, *old_bar = S.bar_buf;
	S.world_buf = cbuf_create(w, h);
	S.bar_buf = cbuf_create(w, BAR_H);
	wlr_scene_buffer_set_buffer(S.world_node, &S.world_buf->base);
	wlr_scene_buffer_set_buffer(S.bar_node, &S.bar_buf->base);
	wlr_scene_rect_set_size(S.dim, w, h);
	if (old_world) wlr_buffer_drop(&old_world->base);
	if (old_bar) wlr_buffer_drop(&old_bar->base);
}

/* ---------------- xdg shell ---------------- */

static void toplevel_map(struct wl_listener *l, void *data) {
	struct toplevel *t = wl_container_of(l, t, map);
	wl_list_insert(&S.toplevels, &t->link);
	if (!t->placed) {
		t->placed = true;
		int n = S.placed_count++ % 8;
		struct wlr_box geo;
		wlr_xdg_surface_get_geometry(t->xdg->base, &geo);
		int x = 60 + n * 36, y = BAR_H + 20 + n * 30;
		if (x + geo.width > S.w) x = S.w - geo.width > 0 ? S.w - geo.width : 0;
		if (y + geo.height > S.h) y = S.h - geo.height > BAR_H ? S.h - geo.height : BAR_H;
		wlr_scene_node_set_position(&t->tree->node, x - geo.x, y - geo.y);
	}
	if (S.game.mode == MODE_DESK) focus_toplevel(t);
	game_on_window_opened(&S.game);
}

static void toplevel_unmap(struct wl_listener *l, void *data) {
	struct toplevel *t = wl_container_of(l, t, unmap);
	if (t == S.grabbed) { S.cursor_mode = CURSOR_PASSTHROUGH; S.grabbed = NULL; }
	wl_list_remove(&t->link);
	if (S.game.mode == MODE_DESK && !wl_list_empty(&S.toplevels)) {
		struct toplevel *next = wl_container_of(S.toplevels.next, next, link);
		focus_toplevel(next);
	}
}

static void toplevel_destroy(struct wl_listener *l, void *data) {
	struct toplevel *t = wl_container_of(l, t, destroy);
	wl_list_remove(&t->map.link);
	wl_list_remove(&t->unmap.link);
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->request_move.link);
	wl_list_remove(&t->request_resize.link);
	wl_list_remove(&t->request_maximize.link);
	wl_list_remove(&t->request_fullscreen.link);
	free(t);
}

static void toplevel_request_move(struct wl_listener *l, void *data) {
	struct toplevel *t = wl_container_of(l, t, request_move);
	if (S.game.mode != MODE_DESK) return;
	if (t->maximized) set_maximized(t, false);
	begin_interactive(t, CURSOR_MOVE, 0);
}

static void toplevel_request_resize(struct wl_listener *l, void *data) {
	struct toplevel *t = wl_container_of(l, t, request_resize);
	struct wlr_xdg_toplevel_resize_event *ev = data;
	if (S.game.mode == MODE_DESK) begin_interactive(t, CURSOR_RESIZE, ev->edges);
}

static void toplevel_request_maximize(struct wl_listener *l, void *data) {
	struct toplevel *t = wl_container_of(l, t, request_maximize);
	if (!t->xdg->base->initialized) return;
	set_maximized(t, t->xdg->requested.maximized);
	wlr_xdg_surface_schedule_configure(t->xdg->base);
}

static void toplevel_request_fullscreen(struct wl_listener *l, void *data) {
	struct toplevel *t = wl_container_of(l, t, request_fullscreen);
	if (!t->xdg->base->initialized) return;
	set_maximized(t, t->xdg->requested.fullscreen);
	wlr_xdg_surface_schedule_configure(t->xdg->base);
}

static void new_xdg_surface(struct wl_listener *l, void *data) {
	struct wlr_xdg_surface *xs = data;
	if (xs->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xs->popup->parent);
		if (!parent || !parent->data) return;
		xs->data = wlr_scene_xdg_surface_create(parent->data, xs);
		return;
	}
	if (xs->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;
	struct toplevel *t = calloc(1, sizeof(*t));
	t->xdg = xs->toplevel;
	t->tree = wlr_scene_xdg_surface_create(S.windows_tree, xs);
	t->tree->node.data = t;
	xs->data = t->tree;
	t->map.notify = toplevel_map;
	wl_signal_add(&xs->surface->events.map, &t->map);
	t->unmap.notify = toplevel_unmap;
	wl_signal_add(&xs->surface->events.unmap, &t->unmap);
	t->destroy.notify = toplevel_destroy;
	wl_signal_add(&xs->events.destroy, &t->destroy);
	t->request_move.notify = toplevel_request_move;
	wl_signal_add(&xs->toplevel->events.request_move, &t->request_move);
	t->request_resize.notify = toplevel_request_resize;
	wl_signal_add(&xs->toplevel->events.request_resize, &t->request_resize);
	t->request_maximize.notify = toplevel_request_maximize;
	wl_signal_add(&xs->toplevel->events.request_maximize, &t->request_maximize);
	t->request_fullscreen.notify = toplevel_request_fullscreen;
	wl_signal_add(&xs->toplevel->events.request_fullscreen, &t->request_fullscreen);
}

static void new_decoration(struct wl_listener *l, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *d = data;
	/* Applications draw their own title bars; the realm draws none. */
	if (d->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(d, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
}

/* ---------------- game loop ---------------- */

static int tick(void *data) {
	double now = realm_now();
	double dt = now - S.last_tick;
	if (dt > 0.1) dt = 0.1;
	S.last_tick = now;
	sync_size();
	game_tick(&S.game, dt);

	bool seated = S.game.mode == MODE_DESK;
	if (!seated || S.frame_no % 3 == 0) {
		cairo_t *cr = cairo_create(S.world_buf->surface);
		game_draw_world(&S.game, cr, S.w, S.h);
		cairo_destroy(cr);
		cairo_surface_flush(S.world_buf->surface);
		wlr_scene_buffer_set_buffer_with_damage(S.world_node, &S.world_buf->base, NULL);
	}
	if (seated && S.frame_no % 3 == 0) {
		cairo_t *cr = cairo_create(S.bar_buf->surface);
		game_draw_bar(&S.game, cr, S.w, BAR_H);
		cairo_destroy(cr);
		cairo_surface_flush(S.bar_buf->surface);
		wlr_scene_buffer_set_buffer_with_damage(S.bar_node, &S.bar_buf->base, NULL);
	}
	S.frame_no++;
	wl_event_source_timer_update(S.timer, TICK_MS);
	return 0;
}

int main(int argc, char *argv[]) {
	wlr_log_init(getenv("REALM_DEBUG") ? WLR_DEBUG : WLR_INFO, NULL);
	char *startup = NULL;
	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		if (c == 's') startup = optarg;
		else { printf("Usage: %s [-s startup command]\n", argv[0]); return 0; }
	}

	config_read("/etc/realm/realm.conf");
	char path[512];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg) snprintf(path, sizeof path, "%s/realm/realm.conf", xdg);
	else snprintf(path, sizeof path, "%s/.config/realm/realm.conf", getenv("HOME") ? getenv("HOME") : "");
	config_read(path);

	struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT };
	sigaction(SIGCHLD, &sa, NULL);

	S.display = wl_display_create();
	S.backend = wlr_backend_autocreate(S.display, &S.session);
	if (!S.backend) { wlr_log(WLR_ERROR, "failed to create backend"); return 1; }
	S.renderer = wlr_renderer_autocreate(S.backend);
	if (!S.renderer) { wlr_log(WLR_ERROR, "failed to create renderer"); return 1; }
	wlr_renderer_init_wl_display(S.renderer, S.display);
	S.allocator = wlr_allocator_autocreate(S.backend, S.renderer);
	if (!S.allocator) { wlr_log(WLR_ERROR, "failed to create allocator"); return 1; }

	wlr_compositor_create(S.display, 5, S.renderer);
	wlr_subcompositor_create(S.display);
	wlr_data_device_manager_create(S.display);
	wlr_primary_selection_v1_device_manager_create(S.display);
	wlr_viewporter_create(S.display);
	wlr_single_pixel_buffer_manager_v1_create(S.display);
	wlr_screencopy_manager_v1_create(S.display);

	S.layout = wlr_output_layout_create();
	wlr_xdg_output_manager_v1_create(S.display, S.layout);
	wl_list_init(&S.outputs);
	S.new_output.notify = new_output;
	wl_signal_add(&S.backend->events.new_output, &S.new_output);

	S.scene = wlr_scene_create();
	S.scene_layout = wlr_scene_attach_output_layout(S.scene, S.layout);
	S.world_node = wlr_scene_buffer_create(&S.scene->tree, NULL);
	S.desk_tree = wlr_scene_tree_create(&S.scene->tree);
	S.dim = wlr_scene_rect_create(S.desk_tree, 1, 1, (float[4]){0.03f, 0.05f, 0.09f, 0.72f});
	S.windows_tree = wlr_scene_tree_create(S.desk_tree);
	S.bar_node = wlr_scene_buffer_create(S.desk_tree, NULL);
	wlr_scene_node_set_enabled(&S.desk_tree->node, false);

	wl_list_init(&S.toplevels);
	S.xdg_shell = wlr_xdg_shell_create(S.display, 3);
	S.new_xdg_surface.notify = new_xdg_surface;
	wl_signal_add(&S.xdg_shell->events.new_surface, &S.new_xdg_surface);
	struct wlr_xdg_decoration_manager_v1 *deco = wlr_xdg_decoration_manager_v1_create(S.display);
	S.new_decoration.notify = new_decoration;
	wl_signal_add(&deco->events.new_toplevel_decoration, &S.new_decoration);

	S.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(S.cursor, S.layout);
	S.cursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), 24);
	S.cursor_motion.notify = cursor_motion;
	wl_signal_add(&S.cursor->events.motion, &S.cursor_motion);
	S.cursor_motion_absolute.notify = cursor_motion_absolute;
	wl_signal_add(&S.cursor->events.motion_absolute, &S.cursor_motion_absolute);
	S.cursor_button.notify = cursor_button;
	wl_signal_add(&S.cursor->events.button, &S.cursor_button);
	S.cursor_axis.notify = cursor_axis;
	wl_signal_add(&S.cursor->events.axis, &S.cursor_axis);
	S.cursor_frame.notify = cursor_frame;
	wl_signal_add(&S.cursor->events.frame, &S.cursor_frame);
	struct wlr_cursor_shape_manager_v1 *shapes = wlr_cursor_shape_manager_v1_create(S.display, 1);
	S.request_set_shape.notify = request_set_shape;
	wl_signal_add(&shapes->events.request_set_shape, &S.request_set_shape);

	wl_list_init(&S.keyboards);
	S.new_input.notify = new_input;
	wl_signal_add(&S.backend->events.new_input, &S.new_input);
	S.seat = wlr_seat_create(S.display, "seat0");
	S.request_cursor.notify = request_cursor;
	wl_signal_add(&S.seat->events.request_set_cursor, &S.request_cursor);
	S.request_set_selection.notify = request_set_selection;
	wl_signal_add(&S.seat->events.request_set_selection, &S.request_set_selection);
	S.request_set_primary.notify = request_set_primary;
	wl_signal_add(&S.seat->events.request_set_primary_selection, &S.request_set_primary);
	struct wlr_virtual_keyboard_manager_v1 *vkm = wlr_virtual_keyboard_manager_v1_create(S.display);
	S.new_virtual_keyboard.notify = new_virtual_keyboard;
	wl_signal_add(&vkm->events.new_virtual_keyboard, &S.new_virtual_keyboard);

	const char *socket = wl_display_add_socket_auto(S.display);
	if (!socket) { wlr_backend_destroy(S.backend); return 1; }
	if (!wlr_backend_start(S.backend)) {
		wlr_backend_destroy(S.backend);
		wl_display_destroy(S.display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_CURRENT_DESKTOP", "RealmOS", true);
	setenv("XDG_SESSION_TYPE", "wayland", true);
	setenv("MOZ_ENABLE_WAYLAND", "1", true);
	setenv("QT_QPA_PLATFORM", "wayland", true);
	setenv("GDK_BACKEND", "wayland", true);

	sync_size();
	game_init(&S.game, S.w, S.h);
	S.last_tick = realm_now();
	S.timer = wl_event_loop_add_timer(wl_display_get_event_loop(S.display), tick, NULL);
	wl_event_source_timer_update(S.timer, TICK_MS);
	wlr_cursor_warp(S.cursor, NULL, S.w / 2.0, S.h / 2.0);
	wlr_cursor_set_xcursor(S.cursor, S.cursor_mgr, "default");

	if (startup && fork() == 0) {
		signal(SIGCHLD, SIG_DFL);
		execl("/bin/sh", "/bin/sh", "-c", startup, (char *)NULL);
		_exit(127);
	}

	wlr_log(WLR_INFO, "realmd running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(S.display);

	character_save(&S.game);
	items_save(&S.game);
	wl_display_destroy_clients(S.display);
	wlr_scene_node_destroy(&S.scene->tree.node);
	wlr_xcursor_manager_destroy(S.cursor_mgr);
	wlr_output_layout_destroy(S.layout);
	wl_display_destroy(S.display);
	return 0;
}
