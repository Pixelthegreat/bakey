/*
 * Copyright 2026, Elliot Kohlmyer
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <locale.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <wayland-client.h>
#include <bakey-xdg-shell.h>
#include <xkbcommon/xkbcommon.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define BAKEY_CONFIG_IMPL
#include <bakey-config.h>
#include <bakey-wl-config.h>
#include <bakey-rc.h>
#include <bakey-posix.h>
#include <bakey.h>

#define FPS_CAP 60

static int pty = -1, spty = -1;
static pid_t shell_pid = -1;

static bool display_updated = true;
static bool running = true;
static bool draw = true;
static bool ctrled = false;

static size_t fwidth, fheight;

static size_t cursor_flash_count = 0;
static bool cursor_visible = true;
static bool cursor_updated = true;
static uint64_t cursor_flash_interval = 0;
static uint64_t cursor_flash_time = 0;

/* wayland stuff */
static size_t width, height;
static int display_fd = -1;
static struct wl_display *wl_display;

static struct wl_registry *wl_registry;
static struct wl_compositor *wl_compositor;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_shm *wl_shm;
static struct wl_seat *wl_seat;

static struct wl_surface *wl_surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;
static struct wl_keyboard *wl_keyboard;
static struct wl_callback *wl_callback_frame;

static struct xkb_context *xkb_context;
static struct xkb_keymap *xkb_keymap;
static struct xkb_state *xkb_state;
static int keymap_fd = -1;
static void *keymap_data;
static size_t keymap_size;
static xkb_keysym_t key_held_recent;
static uint32_t key_held_recent_code;
static uint64_t key_repeat_time;
static size_t key_repeat_count;
static uint64_t key_repeat_rate = 25000000;
static uint64_t key_repeat_delay = 500000000;

static struct wl_shm_pool *wl_shm_pool;
static struct wl_buffer *wl_buffer_shm;
static int shm_fd = -1;
static void *shm_data;
static size_t shm_size;

static FT_Library ft_library;
static bool init_ft_library = false;
static FT_Face ft_face;

static bool frame_ready = false;

/* glyph cache */
static uint8_t *glyph_cache;
static size_t glyph_count;
static uint8_t *glyph_write;

/* bakey context */
static bakey_display_t display;

static bakey_result_t term_open(void);
static size_t term_read(void *buffer, size_t count);
static size_t term_write(const void *buffer, size_t count);
static void term_close(void);
static void term_signal(bakey_control_character_t cc);

static bakey_context_t context = {
	.backend = {
		.display = &display,
		.term_open = term_open,
		.term_read = term_read,
		.term_write = term_write,
		.term_close = term_close,
		.term_signal = term_signal,
	},
};

/* open terminal */
static bakey_result_t term_open(void) {

	if (bakey_posix_openpt(&pty, &spty) != BAKEY_RESULT_SUCCESS)
		return BAKEY_RESULT_FAILURE;

	return bakey_posix_tiocswinsz(pty, display.width, display.height,
				      bakey_wl_config.width,
				      bakey_wl_config.height);
}

/* read from terminal */
static size_t term_read(void *buffer, size_t count) {

	int nread = read(pty, buffer, count);
	return nread < 0? 0: (size_t)nread;
}

/* write to terminal */
static size_t term_write(const void *buffer, size_t count) {

	int nwritten = write(pty, buffer, count);
	return nwritten < 0? 0: (size_t)nwritten;
}

/* close terminal */
static void term_close(void) {

	if (spty >= 0) close(spty);
	if (pty >= 0) close(pty);
}

/* send signal to child process */
static void term_signal(bakey_control_character_t cc) {

	int sig;
	switch (cc) {
		case BAKEY_CONTROL_CHARACTER_INTERRUPT: sig = SIGINT; break;
		case BAKEY_CONTROL_CHARACTER_QUIT: sig = SIGQUIT; break;
		case BAKEY_CONTROL_CHARACTER_SUSPEND: sig = SIGTSTP; break;
		default: return;
	}
	killpg(tcgetpgrp(pty), sig);
}

/* load configuration */
#ifdef BAKEY_RC

#define NRCPATHS (sizeof(rcpaths) / sizeof(rcpaths[0]))
static const char *rcpaths[] = {
	"./bakeywlrc",
	"./bakeyrc",
	"/etc/bakeywlrc",
	"/etc/bakeyrc",
	BAKEY_PREFIX "/share/bakeywlrc",
	BAKEY_PREFIX "/share/bakeyrc",
#ifdef DEBUG
	"./bakeysdlrc.default",
#endif
	NULL, /* $HOME/bakeyrc */
};

static bakey_result_t handle_key(const char *key, const char *value) {

	/* window settings */
	if (!strcmp(key, "wl_width"))
		bakey_wl_config.width = (size_t)atoi(value);

	else if (!strcmp(key, "wl_height"))
		bakey_wl_config.height = (size_t)atoi(value);

	else if (!strcmp(key, "wl_background")) {

		if (bakey_rc_read_color(&bakey_wl_config.background, value) != BAKEY_RESULT_SUCCESS)
			return BAKEY_RESULT_FAILURE;
	}

	/* cursor settings */
	else if (!strcmp(key, "wl_cursor_mode")) {

		if (!strcmp(value, "inverted"))
			bakey_wl_config.cursor_mode = BAKEY_WL_CURSOR_MODE_INVERTED;
	}

	else if (!strcmp(key, "wl_cursor_flash_interval"))
		bakey_wl_config.cursor_flash_interval = (float)atof(value);

	else if (!strcmp(key, "wl_cursor_flash_count"))
		bakey_wl_config.cursor_flash_count = (size_t)atoi(value);

	/* font settings */
	else if (!strcmp(key, "wl_font_face"))
		strncpy(bakey_wl_config.font_face, value, BAKEY_WL_CONFIG_STRING_SIZE);

	else if (!strcmp(key, "wl_font_size"))
		bakey_wl_config.font_size = (float)atof(value);

	/* miscellaneous settings */
	else if (!strcmp(key, "wl_locale"))
		strncpy(bakey_wl_config.locale, value, BAKEY_WL_CONFIG_STRING_SIZE);

	else if (!strcmp(key, "wl_shell"))
		strncpy(bakey_wl_config.shell, value, BAKEY_WL_CONFIG_STRING_SIZE);

	return BAKEY_RESULT_SUCCESS;
}

static int load_config(void) {

	char buf[128];
	const char *home = getenv("HOME");

	if (home) {
		
		snprintf(buf, sizeof(buf), BAKEY_PREFIX "/%s/bakeyrc", home);
		rcpaths[NRCPATHS-1] = buf;
	}

	struct stat st;
	for (size_t i = 0; i < NRCPATHS; i++) {

		if (!rcpaths[i]) continue;

		if (stat(rcpaths[i], &st) < 0 ||
		    !S_ISREG(st.st_mode))
			continue;

		if (bakey_rc_load_path(rcpaths[i], handle_key) != BAKEY_RESULT_SUCCESS) {

			fprintf(stderr, "Bakey: %s\n", bakey_get_error());
			return -1;
		}
		break;
	}
	return 0;
}

#endif

/* timer callback */
static void sigh_alrm() {

	draw = true;
}

/* reset cursor state */
static void reset_cursor(void) {

	cursor_flash_time = 0;
	cursor_visible = true;
	cursor_flash_count = bakey_wl_config.cursor_flash_count;
	cursor_updated = true;
}

/* registry listener */
static void registry_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version);
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t id);

static struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* register interface */
static void registry_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {

	if (!strcmp(interface, "wl_compositor"))
		wl_compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
	else if (!strcmp(interface, "xdg_wm_base"))
		xdg_wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
	else if (!strcmp(interface, "wl_shm"))
		wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
	else if (!strcmp(interface, "wl_seat"))
		wl_seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
}

/* remove interface */
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t id) {
}

/* xdg wm base listener */
static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t ping);

static struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

/* respond to ping */
static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t ping) {

	xdg_wm_base_pong(xdg_wm_base, ping);
}

/* xdg surface listener */
static void surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t id);

static struct xdg_surface_listener surface_listener = {
	.configure = surface_configure,
};

/* xdg surface configure */
static void surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t id) {

	xdg_surface_ack_configure(xdg_surface, id);
	frame_ready = true;
}

/* keyboard listener */
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size);
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t id, struct wl_surface *surface, struct wl_array *keys);
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t id, struct wl_surface *surface);
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t id, uint32_t time, uint32_t key, uint32_t state);
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t id, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group);
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int rate, int delay);

static struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

/* provide keymap */
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) {

	if (keymap_fd >= 0 || format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
		return;

	keymap_fd = fd;
	keymap_size = (size_t)size;

	keymap_data = mmap(NULL, keymap_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE, fd, 0);
	if (keymap_data == (void *)-1) {

		keymap_data = NULL;
		return;
	}

	/* create xkb keymap and state */
	xkb_keymap = xkb_keymap_new_from_string(xkb_context,
						keymap_data,
						XKB_KEYMAP_FORMAT_TEXT_V1,
						XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!xkb_keymap) return;

	xkb_state = xkb_state_new(xkb_keymap);
	if (!xkb_state) return;
}

/* enter focus */
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t id, struct wl_surface *surface, struct wl_array *keys) {
}

/* leave focus */
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t id, struct wl_surface *surface) {

	key_held_recent = XKB_KEY_NoSymbol;
}

/* process key */
static void process_key(xkb_keysym_t keysym, uint32_t code, uint32_t state) {

	if (keysym == XKB_KEY_Control_L)
		ctrled = state > 0;

	if (ctrled && state > 0) {

		if (code >= L'a' && code <= L'z')
			code -= L'a';
		if (code >= L'A' && code <= L'Z')
			code -= L'A';
		bakey_send_character(&context, (wchar_t)code);
		return;
	}

	if (!state) return;

	/* generic keys */
	switch (keysym) {

		case XKB_KEY_BackSpace: bakey_send_character(&context, L'\x7f'); return;
		case XKB_KEY_Up: bakey_send_sequence(&context, L"\x1b[A"); return;
		case XKB_KEY_Down: bakey_send_sequence(&context, L"\x1b[B"); return;
		case XKB_KEY_Right: bakey_send_sequence(&context, L"\x1b[C"); return;
		case XKB_KEY_Left: bakey_send_sequence(&context, L"\x1b[D"); return;
	}

	if (code) bakey_send_character(&context, code);
}

/* key press or release */
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t id, uint32_t time, uint32_t key, uint32_t state) {

	if (!xkb_keymap || !xkb_state) return;

	xkb_keysym_t keysym = xkb_state_key_get_one_sym(xkb_state, (xkb_keycode_t)key + 8);
	uint32_t code = xkb_state_key_get_utf32(xkb_state, (xkb_keycode_t)key + 8);

	if (state > 0) {

		key_held_recent = keysym;
		key_held_recent_code = code;
		key_repeat_time = 0;
		key_repeat_count = 0;
	}
	else if (keysym == key_held_recent)
		key_held_recent = XKB_KEY_NoSymbol;

	process_key(keysym, code, state);
	if (context.display_updated) {

		reset_cursor();
		display_updated = true;
	}
}

/* modifiers */
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t id, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {

	xkb_state_update_mask(xkb_state,
			      mods_depressed,
			      mods_latched,
			      mods_locked,
			      0, 0, group);
}

/* repeat rate info */
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int rate, int delay) {

	if (!rate || !delay) return;

	key_repeat_rate = 1000000000 / (uint64_t)rate;
	key_repeat_delay = (uint64_t)delay * 1000000;
}

/* load glyph cache data */
static int load_glyph_cache(void) {

	glyph_count = (size_t)ft_face->num_glyphs;
	glyph_cache = (uint8_t *)malloc(fwidth * fheight * glyph_count * 4);
	glyph_write = (uint8_t *)malloc(fwidth * fheight * 4);

	if (!glyph_cache || !glyph_write) {

		fprintf(stderr, "Can't allocate glyph cache\n");
		return -1;
	}
	memset(glyph_cache, 0, fwidth * fheight * glyph_count * 4);

	/* render glyphs */
	for (size_t i = 1; i < glyph_count; i++) {

		uint8_t *data = glyph_cache + i * fwidth * fheight * 4;

		if (FT_Load_Glyph(ft_face, (FT_UInt)i, FT_LOAD_DEFAULT))
			continue;

		if (FT_Render_Glyph(ft_face->glyph, FT_RENDER_MODE_NORMAL))
			continue;

		size_t x = ft_face->glyph->bitmap_left;
		if (x >= fwidth) continue;

		size_t w = ft_face->glyph->bitmap.width;
		if (x+w > fwidth) w = fwidth - x;

		size_t y = ft_face->size->metrics.y_ppem -
			   ft_face->glyph->bitmap_top;
		if (y >= fheight) continue;

		size_t h = ft_face->glyph->bitmap.rows;
		if (y+h > fheight) h = fheight - y;

		for (size_t py = 0; py < h; py++) {
			for (size_t px = 0; px < w; px++) {

				uint8_t value = ft_face->glyph->bitmap.buffer[py *
					ft_face->glyph->bitmap.pitch + px];

				size_t index = ((y+py) * fwidth + x+px) * 4;

				data[index] = value;
				data[index + 1] = value;
				data[index + 2] = value;
				data[index + 3] = 0xff;
			}
		}
	}
	return 0;
}

/* frame callback listener */
static void frame_done(void *data, struct wl_callback *callback, uint32_t time);

static struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

/* frame ready to be drawn */
static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {

	frame_ready = true;
}

/* resize shared memory file */
static int resize_shm(void) {

	size_t old_size = shm_size;
	shm_size = width * height * 4;

	if (ftruncate(shm_fd, (off_t)shm_size) < 0) {

		perror("ftruncate");
		return -1;
	}

	if (shm_data) munmap(shm_data, old_size);
	shm_data = mmap(NULL, shm_size, PROT_READ | PROT_WRITE,
			MAP_SHARED, shm_fd, 0);
	if (shm_data == (void *)-1) {

		shm_data = NULL;
		perror("mmap");
		return -1;
	}
	return 0;
}

/* dispatch pending messages */
static void dispatch_messages(void) {

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(display_fd, &rfds);

	struct timeval timeout = {
		.tv_sec = 0,
		.tv_usec = 0,
	};
	if (select(display_fd+1, &rfds, NULL, NULL, &timeout) < 0)
		return;

	if (FD_ISSET(display_fd, &rfds))
		wl_display_dispatch(wl_display);
	else wl_display_dispatch_pending(wl_display);
}

/* update frame callback */
static int update_frame(void) {

	if (wl_callback_frame) wl_callback_destroy(wl_callback_frame);

	wl_callback_frame = wl_surface_frame(wl_surface);
	if (!wl_callback_frame) {

		fprintf(stderr, "Can't get Wayland surface frame callback");
		return -1;
	}
	wl_callback_add_listener(wl_callback_frame, &frame_listener, NULL);
	wl_surface_commit(wl_surface);

	wl_display_flush(wl_display);
	return 0;
}

/* fill area of screen */
static void fill_area(size_t x, size_t y, size_t w, size_t h, bakey_color_t color) {

	uint8_t r = (color >> 16) & 0xff;
	uint8_t g = (color >> 8) & 0xff;
	uint8_t b = color & 0xff;

	uint8_t *data = shm_data + (y * width + x) * 4;
	size_t diff = (width - w) * 4;

	for (size_t py = y; py < y+h; py++, data += diff) {
		for (size_t px = x; px < x+w; px++) {

			*data++ = b;
			*data++ = g;
			*data++ = r;
			*data++ = 0xff;
		}
	}
}

/* draw character */
#define BLEND_TABLE_SIZE 16
#define BLEND_TABLE_SHIFT 4

static bakey_color_t blend_bg, blend_fg;
static uint8_t blend_table[BLEND_TABLE_SIZE * 4];

static void draw_character(size_t x, size_t y, int cc, bakey_color_t bg, bakey_color_t fg) {

	FT_UInt index = FT_Get_Char_Index(ft_face, cc);

	if (!index) {

		fill_area(x, y, fwidth, fheight, bg);
		return;
	}

	/* update blend table */
	if (bg != blend_bg || fg != blend_fg) {

		uint8_t bgv[4] = {
			bg & 0xff,
			(bg >> 8) & 0xff,
			(bg >> 16) & 0xff,
			0xff,
		};
		uint8_t fgv[4] = {
			fg & 0xff,
			(fg >> 8) & 0xff,
			(fg >> 16) & 0xff,
			0xff,
		};

		for (uint32_t i = 0; i < sizeof(blend_table); i++) {

			size_t j = i >> 2;
			blend_table[i] = (bgv[i & 0x3] * (BLEND_TABLE_SIZE - j) >> (8 - BLEND_TABLE_SHIFT)) +
					 (fgv[i & 0x3] * j >> (8 - BLEND_TABLE_SHIFT));
		}

		blend_bg = bg;
		blend_fg = fg;
	}

	size_t size = fwidth * fheight * 4;
	const uint8_t *rdata = glyph_cache + (size_t)index * size;

	uint8_t *wdata = glyph_write;
	for (size_t i = 0; i < size; i++, wdata++, rdata++)
		*wdata = blend_table[((*rdata) >> BLEND_TABLE_SHIFT) * 4 + (i & 0x3)];

	wdata = shm_data + (y * width + x) * 4;
	rdata = glyph_write;

	for (size_t i = 0; i < fheight; i++, wdata += width * 4, rdata += fwidth * 4)
		memcpy(wdata, rdata, fwidth * 4);
}

/* draw frame */
#define SWAP_VALUES(a, b) ({\
		a ^= b;\
		b ^= a;\
		a ^= b;\
	})

static bool drawn_background = false;

struct damage {
	size_t left, right, top, bottom;
};

static void draw_frame(struct damage *damage) {

	bool old_drawn_background = drawn_background;
	if (!drawn_background) {

		fill_area(0, 0, width, height, bakey_wl_config.background);
		drawn_background = true;
	}

	damage->left = width;
	damage->right = 0;
	damage->top = height;
	damage->bottom = 0;

	/* draw character cells */
#ifdef BAKEY_WL_TIME_ACCOUNTING
	struct timespec ts_start;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_start);
#endif

	for (size_t y = 0; y < display.height; y++) {
		for (size_t x = 0; x < display.width; x++) {

			size_t position = y * display.width + x;

			if (old_drawn_background &&
			    position != context.position &&
			    position != context.damage.position &&
			    (x < context.damage.x || y < context.damage.y ||
			     x >= context.damage.x + context.damage.width ||
			     y >= context.damage.y + context.damage.height))
				continue;

			size_t dx = x * fwidth;
			size_t dy = y * fheight;

			/* update damage area */
			if (dx < damage->left) damage->left = dx;
			if (dx+fwidth > damage->right) damage->right = dx+fwidth;
			if (dy < damage->top) damage->top = dy;
			if (dy+fheight > damage->bottom) damage->bottom = dy+fheight;

			/* draw character */
			bakey_cell_t *cell = display.cells + (y * display.width) + x;

			bakey_color_t bg = cell->background;
			bakey_color_t fg = cell->foreground;

			if (cell->style & BAKEY_STYLE_INVERSE)
				SWAP_VALUES(bg, fg);

			if (position == context.position && cursor_visible &&
			    bakey_wl_config.cursor_mode == BAKEY_WL_CURSOR_MODE_INVERTED)
				SWAP_VALUES(bg, fg);

			draw_character(dx, dy, cell->character, bg, fg);
		}
	}

	/* fix damage area */
	if (damage->right < damage->left ||
	    damage->bottom < damage->top ||
	    !old_drawn_background) {

		damage->left = 0;
		damage->right = width;
		damage->top = 0;
		damage->bottom = height;
	}

	/* print frame time */
#ifdef BAKEY_WL_TIME_ACCOUNTING
	struct timespec ts_end;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_end);

	uint64_t ns_start = (uint64_t)ts_start.tv_sec * 1000000000 + (uint64_t)ts_start.tv_nsec;
	uint64_t ns_end = (uint64_t)ts_end.tv_sec * 1000000000 + (uint64_t)ts_end.tv_nsec;
	uint64_t ns = ns_end - ns_start;

	printf("%llu\n", ns);
#endif
}

/* run application */
static int run(void) {

#ifdef BAKEY_RC
	if (load_config() < 0)
		return -1;
#endif

	setlocale(LC_ALL, bakey_wl_config.locale);

	struct sigaction action;

	action.sa_handler = sigh_alrm;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	action.sa_restorer = NULL;

	if (sigaction(SIGALRM, &action, NULL)) {

		perror("sigaction");
		return 1;
	}

	/* configure environment */
	setenv("TERM", "vt100-bakey", 1);
#ifdef DEBUG
	static char termpathbuf[PATH_MAX];
	realpath("./terminfo", termpathbuf);

	setenv("TERMINFO", termpathbuf, 1);
#endif

	/* connect to wayland server */
	width = bakey_wl_config.width;
	height = bakey_wl_config.height;

	wl_display = wl_display_connect(NULL);
	if (!wl_display) {

		fprintf(stderr, "Can't connect to Wayland display\n");
		return 1;
	}
	display_fd = wl_display_get_fd(wl_display);

	/* register interfaces */
	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &registry_listener, NULL);

	wl_display_dispatch(wl_display);

	if (!wl_compositor || !xdg_wm_base || !wl_shm || !wl_seat) {

		fprintf(stderr, "Can't register required Wayland interfaces\n");
		return 1;
	}
	xdg_wm_base_add_listener(xdg_wm_base, &wm_base_listener, NULL);

	/* create surface */
	wl_surface = wl_compositor_create_surface(wl_compositor);
	if (!wl_surface) {

		fprintf(stderr, "Can't create Wayland compositor surface\n");
		return 1;
	}

	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, wl_surface);
	if (!xdg_surface) {

		fprintf(stderr, "Can't get XDG surface\n");
		return 1;
	}

	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	if (!xdg_toplevel) {

		fprintf(stderr, "Can't get XDG toplevel\n");
		return 1;
	}

	wl_surface_commit(wl_surface);

	xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
	wl_display_dispatch(wl_display);

	/* open shared memory */
	srand((unsigned int)time(NULL));

	char name[32] = "/";
	for (size_t i = 0; i < sizeof(name)-1; i++)
		name[i] = (char)(rand() % 26 + 'A');
	name[sizeof(name)-1] = 0;

	shm_fd = shm_open(name, O_RDWR | O_EXCL | O_CREAT, 0600);
	if (shm_fd < 0) {

		perror("shm_open");
		return 1;
	}
	shm_unlink(name);

	if (resize_shm() < 0)
		return 1;

	/* get peripherals */
	wl_keyboard = wl_seat_get_keyboard(wl_seat);
	if (!wl_keyboard) {

		fprintf(stderr, "Can't get Wayland keyboard");
		return 1;
	}

	xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkb_context) {

		fprintf(stderr, "Can't create XKB context");
		return 1;
	}

	wl_keyboard_add_listener(wl_keyboard, &keyboard_listener, NULL);

	if (update_frame() < 0) return 1;

	/* initialize freetype */
	if (FT_Init_FreeType(&ft_library)) {

		fprintf(stderr, "Can't initialize FreeType\n");
		return 1;
	}
	init_ft_library = true;

	if (FT_New_Face(ft_library, bakey_wl_config.font_face, 0, &ft_face)) {

		fprintf(stderr, "Can't load FreeType font face\n");
		return 1;
	}

	if (FT_Set_Char_Size(ft_face, 0, (FT_F26Dot6)(bakey_wl_config.font_size * 64.f), 96, 96)) {

		fprintf(stderr, "Can't set FreeType font size\n");
		return 1;
	}

	FT_UInt index = FT_Get_Char_Index(ft_face, L'W');
	FT_Load_Glyph(ft_face, index, FT_LOAD_DEFAULT);
	FT_Render_Glyph(ft_face->glyph, FT_RENDER_MODE_NORMAL);

	fwidth = (size_t)(ft_face->glyph->bitmap_left + ft_face->glyph->bitmap.width);
	fheight = (size_t)(ft_face->glyph->bitmap.rows + ft_face->size->metrics.y_ppem - ft_face->glyph->bitmap_top);

	fwidth = ft_face->glyph->advance.x >> 6;
	fheight = ft_face->size->metrics.height >> 6;

	/* load glyph cache */
	if (load_glyph_cache() < 0)
		return 1;

	/* initialize bakey */
	display.width = width / fwidth;
	display.height = height / fheight;
	display.cells = malloc(sizeof(bakey_cell_t) * display.width * display.height);

	if (bakey_open(&context) != BAKEY_RESULT_SUCCESS) {

		fprintf(stderr, "Bakey: %s\n", bakey_get_error());
		return 1;
	}

	/* create shell process */
	const char *shell_argv[] = {bakey_wl_config.shell, NULL};
	shell_pid = bakey_posix_launch(bakey_wl_config.shell, shell_argv, pty);
	if (shell_pid < 0) {

		fprintf(stderr, "Bakey: %s\n", bakey_get_error());
		return 1;
	}

	/* set frame timer */
	struct itimerval it = {
		.it_interval = {
			.tv_sec = 0,
			.tv_usec = 1000000 / FPS_CAP,
		},
		.it_value = {
			.tv_sec = 0,
			.tv_usec = 1000000 / FPS_CAP,
		},
	};
	setitimer(ITIMER_REAL, &it, NULL);

	/* main loop */
	cursor_flash_interval = (uint64_t)(bakey_wl_config.cursor_flash_interval * 1000000000.f);
	cursor_flash_time = 0;
	cursor_flash_count = bakey_wl_config.cursor_flash_count;

	struct timespec ts_last;
	clock_gettime(CLOCK_REALTIME, &ts_last);

	while (running) {

		/* update terminal attributes */
		struct termios attr;
		if (tcgetattr(pty, &attr) >= 0) {

			if (attr.c_lflag & ECHO)
				context.control.flags |= BAKEY_CONTROL_FLAG_ECHO;
			else context.control.flags &= ~BAKEY_CONTROL_FLAG_ECHO;

			if (attr.c_lflag & ICANON)
				context.control.flags |= BAKEY_CONTROL_FLAG_CANONICAL;
			else context.control.flags &= ~BAKEY_CONTROL_FLAG_CANONICAL;

			if (attr.c_lflag & ISIG)
				context.control.flags |= BAKEY_CONTROL_FLAG_SIGNAL;
			else context.control.flags &= ~BAKEY_CONTROL_FLAG_SIGNAL;

			context.control.cc[BAKEY_CONTROL_CHARACTER_MINIMUM] = (long)attr.c_cc[VMIN];
			context.control.cc[BAKEY_CONTROL_CHARACTER_INTERRUPT] = (long)attr.c_cc[VINTR];
			context.control.cc[BAKEY_CONTROL_CHARACTER_QUIT] = (long)attr.c_cc[VQUIT];
			context.control.cc[BAKEY_CONTROL_CHARACTER_SUSPEND] = (long)attr.c_cc[VSUSP];
		}

		/* update terminal */
		if (bakey_update(&context) != BAKEY_RESULT_SUCCESS)
			return 1;

		if (context.display_updated) {

			reset_cursor();
			display_updated = true;
		}

		if (draw) {

			dispatch_messages();

			/* create shared memory pool and buffer */
			if (!wl_shm_pool) {

				wl_shm_pool = wl_shm_create_pool(wl_shm, shm_fd, shm_size);
				if (!wl_shm_pool) {

					fprintf(stderr, "Can't create Wayland shared memory pool\n");
					return 1;
				}
			}

			if (!wl_buffer_shm) {

				int stride = (int)width * 4;
				wl_buffer_shm = wl_shm_pool_create_buffer(
						wl_shm_pool,
						0,
						(int)width,
						(int)height,
						stride,
						WL_SHM_FORMAT_XRGB8888
						);
				if (!wl_buffer_shm) {

					fprintf(stderr, "Can't create Wayland shared memory buffer\n");
					return 1;
				}
			}

			/* draw frame */
			if ((cursor_updated || display_updated) &&
			    frame_ready) {

				struct damage damage;
				draw_frame(&damage);

				bakey_reset_damage(&context);

				wl_surface_attach(wl_surface, wl_buffer_shm, 0, 0);
				wl_surface_damage(wl_surface,
						  (int32_t)damage.left,
						  (int32_t)damage.top,
						  (int32_t)(damage.right - damage.left),
						  (int32_t)(damage.bottom - damage.top));
				wl_surface_commit(wl_surface);

				if (update_frame() < 0)
					return 1;
				frame_ready = false;
				display_updated = false;
			}
			cursor_updated = false;

			/* get frame time */
			struct timespec ts_current;
			clock_gettime(CLOCK_REALTIME, &ts_current);

			uint64_t ns_last = (uint64_t)ts_last.tv_sec * 1000000000 + (uint64_t)ts_last.tv_nsec;
			uint64_t ns_current = (uint64_t)ts_current.tv_sec * 1000000000 + (uint64_t)ts_current.tv_nsec;

			uint64_t ns_time = ns_current - ns_last;
			ts_last = ts_current;

			cursor_flash_time += ns_time;
			key_repeat_time += ns_time;

			/* update cursor */
			if (cursor_flash_time >= cursor_flash_interval &&
			    cursor_flash_count) {

				cursor_flash_time %= cursor_flash_interval;
				cursor_visible = !cursor_visible;

				cursor_flash_count--;
				cursor_updated = true;
			}

			/* update key repeat */
			uint64_t key_repeat_threshold =
				(!key_repeat_count? key_repeat_delay: key_repeat_rate);

			if (key_held_recent && key_repeat_time >= key_repeat_threshold) {

				key_repeat_time %= key_repeat_threshold;
				key_repeat_count++;

				process_key(key_held_recent, key_held_recent_code, 1);
				if (context.display_updated) {

					reset_cursor();
					display_updated = true;
				}
			}

			draw = false;
		}

		/* manage shell process */
		if (bakey_posix_process_exited(shell_pid))
			running = false;
	}
	return 0;
}

/* clean up resources */
static void cleanup(void) {

	if (shell_pid >= 0) kill(shell_pid, SIGKILL);
	if (context.init) bakey_close(&context);
	if (glyph_write) free(glyph_write);
	if (glyph_cache) free(glyph_cache);
	if (init_ft_library) FT_Done_FreeType(ft_library);
	if (xkb_state) xkb_state_unref(xkb_state);
	if (xkb_keymap) xkb_keymap_unref(xkb_keymap);
	if (keymap_data) munmap(keymap_data, keymap_size);
	if (xkb_context) xkb_context_unref(xkb_context);
	if (shm_data) munmap(shm_data, shm_size);
	if (shm_fd >= 0) close(shm_fd);
	if (wl_display) wl_display_disconnect(wl_display);
}

int main() {

	int code = run();
	cleanup();
	return code;
}
