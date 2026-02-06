/*
 * Copyright 2026, Elliot Kohlmyer
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <locale.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include <cairo.h>
#include <pango/pangocairo.h>

#define BAKEY_CONFIG_IMPL
#include <bakey-config.h>
#include <bakey-sdl-config.h>
#include <bakey-rc.h>
#include <bakey-posix.h>
#include <bakey.h>

#define FPS_CAP 60

static bool init_sdl = false;
static SDL_Window *window = NULL;
static SDL_Surface *surface = NULL;
static cairo_surface_t *cr_surface = NULL;
static cairo_t *cr = NULL;
static PangoLayout *layout = NULL;
static PangoFontDescription *font = NULL;
static size_t width, height, fwidth, fheight;

static size_t cursor_flash_count = 0;
static bool cursor_visible = true;
static bool cursor_updated = true;
static uint32_t cursor_flash_interval = 0;
static uint32_t cursor_flash_time = 0;

static int pty = -1, spty = -1;
static pid_t shell_pid = -1;

static bool display_updated = true;
static bool running = true;
static bool draw = false;
static bool shifted = false;
static bool ctrled = false;

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
				      width, height);
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
	"./bakeysdlrc",
	"./bakeyrc",
	"/etc/bakeysdlrc",
	"/etc/bakeyrc",
	BAKEY_PREFIX "/share/bakeysdlrc",
	BAKEY_PREFIX "/share/bakeyrc",
};

static bakey_result_t handle_key(const char *key, const char *value) {

	/* window settings */
	if (!strcmp(key, "sdl_title"))
		strncpy(bakey_sdl_config.title, value, BAKEY_SDL_CONFIG_STRING_SIZE);

	else if (!strcmp(key, "sdl_width"))
		bakey_sdl_config.width = (size_t)atoi(value);

	else if (!strcmp(key, "sdl_height"))
		bakey_sdl_config.height = (size_t)atoi(value);

	else if (!strcmp(key, "sdl_background")) {

		if (bakey_rc_read_color(&bakey_sdl_config.background, value) != BAKEY_RESULT_SUCCESS)
			return BAKEY_RESULT_FAILURE;
	}

	/* font settings */
	else if (!strcmp(key, "sdl_font_face"))
		strncpy(bakey_sdl_config.font_face, value, BAKEY_SDL_CONFIG_STRING_SIZE);

	else if (!strcmp(key, "sdl_font_size"))
		bakey_sdl_config.font_size = (size_t)atoi(value);

	/* cursor settings */
	else if (!strcmp(key, "sdl_cursor_mode")) {

		if (!strcmp(value, "inverted"))
			bakey_sdl_config.cursor_mode = BAKEY_SDL_CURSOR_MODE_INVERTED;
	}

	else if (!strcmp(key, "sdl_cursor_flash_interval"))
		bakey_sdl_config.cursor_flash_interval = (float)atof(value);

	else if (!strcmp(key, "sdl_cursor_flash_count"))
		bakey_sdl_config.cursor_flash_count = (size_t)atoi(value);

	/* miscellaneous settings */
	else if (!strcmp(key, "sdl_locale"))
		strncpy(bakey_sdl_config.locale, value, BAKEY_SDL_CONFIG_STRING_SIZE);

	else if (!strcmp(key, "sdl_shell"))
		strncpy(bakey_sdl_config.shell, value, BAKEY_SDL_CONFIG_STRING_SIZE);

	else return BAKEY_RESULT_FAILURE;
	return BAKEY_RESULT_SUCCESS;
}

static int load_config(void) {

	struct stat st;
	for (size_t i = 0; i < NRCPATHS; i++) {

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

/* create or recreate surfaces */
static int create_surfaces(void) {

	if (cr) cairo_destroy(cr);
	if (cr_surface) cairo_surface_destroy(cr_surface);
	if (surface) SDL_FreeSurface(surface);

	cr = NULL;
	cr_surface = NULL;
	surface = NULL;

	surface = SDL_CreateRGBSurface(0,
				       width,
				       height,
				       32,
				       0x00ff0000,
				       0x0000ff00,
				       0x000000ff,
				       0);
	if (!surface) {

		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return -1;
	}

	/* create cairo surface */
	cr_surface = cairo_image_surface_create_for_data(
		(unsigned char *)surface->pixels, CAIRO_FORMAT_ARGB32,
		surface->w, surface->h, surface->pitch);
	if (!cr_surface) {

		fprintf(stderr, "Can't create Cairo surface\n");
		return -1;
	}
	cairo_surface_set_device_scale(cr_surface, 1, 1);

	cr = cairo_create(cr_surface);
	if (!cr) {

		fprintf(stderr, "Can't create Cairo context\n");
		return -1;
	}
	return 0;
}

/* interpret key press */
static void process_key(SDL_Event *event) {

	if (event->key.keysym.sym == SDLK_LSHIFT ||
	    event->key.keysym.sym == SDLK_RSHIFT) {

		shifted = (event->type == SDL_KEYDOWN);
		return;
	}
	else if (event->key.keysym.sym == SDLK_LCTRL ||
		 event->key.keysym.sym == SDLK_RCTRL) {

		ctrled = (event->type == SDL_KEYDOWN);
		return;
	}
	else if (event->type != SDL_KEYDOWN)
		return;

	/* control modifier */
	else if (ctrled) {

		wchar_t c = (wchar_t)toupper((int)event->key.keysym.sym);

		if (c >= 'A' && c <= 'Z')
			bakey_send_character(&context, c - 'A' + 1);
		return;
	}

	switch (event->key.keysym.sym) {

		/* generic key */
		case SDLK_BACKSPACE: bakey_send_character(&context, '\x7f'); break;
		case SDLK_TAB: bakey_send_character(&context, '\t'); break;
		case SDLK_RETURN: bakey_send_character(&context, '\r'); break;
		case SDLK_ESCAPE: bakey_send_character(&context, '\x1b'); break;
		case SDLK_UP: bakey_send_sequence(&context, L"\x1b[A"); break;
		case SDLK_DOWN: bakey_send_sequence(&context, L"\x1b[B"); break;
		case SDLK_RIGHT: bakey_send_sequence(&context, L"\x1b[C"); break;
		case SDLK_LEFT: bakey_send_sequence(&context, L"\x1b[D"); break;

		/* printable character */
		default:
			if (event->type != SDL_KEYDOWN)
				break;

			wchar_t wc = (wchar_t)event->key.keysym.sym;
			if (wc < ' ' || wc > '~')
				break;

			wc = tolower(wc);
			if (shifted) {

				/* shift character */
				wc = toupper(wc);
				if (wc >= '0' && wc <= '9')
					wc = L")!@#$%^&*("[wc - '0'];
				else switch (wc) {
					case '-': wc = '_'; break;
					case '=': wc = '+'; break;
					case '[': wc = '{'; break;
					case ']': wc = '}'; break;
					case ';': wc = ':'; break;
					case '\'': wc = '"'; break;
					case ',': wc = '<'; break;
					case '.': wc = '>'; break;
					case '\\': wc = '|'; break;
					case '/': wc = '?'; break;
					case '`': wc = '~'; break;
				}
			}

			bakey_send_character(&context, wc);
			break;
	}
	if (context.display_updated)
		display_updated = true;
}

/* set color to bakey color */
static void set_color(bakey_color_t color) {

	double red = (double)((color >> 16) & 0xff) / 255.0;
	double green = (double)((color >> 8) & 0xff) / 255.0;
	double blue = (double)(color & 0xff) / 255.0;

	cairo_set_source_rgb(cr, red, green, blue);
}

/* draw terminal */
static bool drawn_background = false;

static void draw_terminal(void) {

	bool old_drawn_background = drawn_background;
	if (!drawn_background) {

		set_color(bakey_sdl_config.background);
		cairo_paint(cr);
		drawn_background = true;
	}

	/* draw text grid */
	double dw = (double)fwidth;
	double dh = (double)fheight;

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

			bakey_cell_t *cell = display.cells + (y * display.width) + x;

			double dx = (double)(x * fwidth);
			double dy = (double)(y * fheight);

			bakey_color_t background = cell->background;
			bakey_color_t foreground = cell->foreground;

			if (cell->style & BAKEY_STYLE_INVERSE) {

				bakey_color_t color = background;
				background = foreground;
				foreground = color;
			}

			/* draw background */
			set_color(background);
			cairo_rectangle(cr, dx, dy, dw, dh);
			cairo_fill(cr);

			bakey_color_t color = foreground;

			/* draw cursor */
			if (position == context.position && cursor_visible) {

				switch (bakey_sdl_config.cursor_mode) {

					/* inverted cursor */
					case BAKEY_SDL_CURSOR_MODE_INVERTED:
						color = background;

						set_color(foreground);
						cairo_rectangle(cr, dx, dy, dw, dh);
						cairo_fill(cr);
						break;
				}
			}

			/* draw character */
			if (!cell->character || cell->character == ' ') continue;

			char cbuf[MB_CUR_MAX+1];
			int nch = wctomb(cbuf, cell->character);

			if (nch < 1) {

				cbuf[0] = '?';
				cbuf[1] = 0;
			}
			else cbuf[nch] = 0;

			cairo_move_to(cr, dx, dy);
			set_color(color);

			pango_layout_set_text(layout, cbuf, nch);
			pango_cairo_show_layout(cr, layout);
		}
	}
}

/* handle resize event */
static int handle_resize(SDL_Event *event) {

	width = (size_t)event->window.data1;
	height = (size_t)event->window.data2;

	size_t new_width = width / fwidth;
	size_t new_height = height / fheight;

	if (!new_width || !new_height ||
	    (display.width == new_width &&
	     display.height == new_height))
		return 0;

	if (create_surfaces() < 0)
		return -1;

	display.width = new_width;
	display.height = new_height;

	display.cells = realloc(display.cells,
			sizeof(bakey_cell_t) *
			display.width * display.height);

	bakey_adjust(&context);

	if (bakey_posix_tiocswinsz(pty,
	    display.width, display.height,
	    width, height) < 0)
		return -1;

	drawn_background = false;
	display_updated = true;
	return 0;
}

/* reset cursor state */
static void reset_cursor(void) {

	cursor_flash_time = 0;
	cursor_visible = true;
	cursor_flash_count = bakey_sdl_config.cursor_flash_count;
	cursor_updated = true;
}

/* run application */
static int run(void) {

#ifdef BAKEY_RC
	if (load_config() < 0)
		return 1;
#endif

	/* configure locale and signals */
	setlocale(LC_ALL, bakey_sdl_config.locale);

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

	/* initialize sdl */
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	if (SDL_Init(SDL_INIT_EVERYTHING) < 0) {

		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}
	init_sdl = true;

	/* create window */
	width = bakey_sdl_config.width;
	height = bakey_sdl_config.height;

	window = SDL_CreateWindow(bakey_sdl_config.title,
				  SDL_WINDOWPOS_UNDEFINED,
				  SDL_WINDOWPOS_UNDEFINED,
				  width, height,
				  SDL_WINDOW_ALLOW_HIGHDPI |
				  SDL_WINDOW_RESIZABLE);
	if (!window) {

		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}

	if (create_surfaces() < 0)
		return 1;

	/* load font */
	font = pango_font_description_new();
	pango_font_description_set_family(font, bakey_sdl_config.font_face);
	pango_font_description_set_weight(font, PANGO_WEIGHT_NORMAL);
	pango_font_description_set_absolute_size(font, bakey_sdl_config.font_size * PANGO_SCALE);

	layout = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(layout, font);

	pango_layout_set_text(layout, "W", -1);

	int x, y;
	pango_layout_get_size(layout, &x, &y);

	fwidth = (size_t)PANGO_PIXELS(x);
	fheight = (size_t)PANGO_PIXELS(y);

	/* initialize bakey */
	display.width = width / fwidth;
	display.height = height / fheight;
	display.cells = malloc(sizeof(bakey_cell_t) * display.width * display.height);

	if (bakey_open(&context) != BAKEY_RESULT_SUCCESS) {

		fprintf(stderr, "Bakey: %s\n", bakey_get_error());
		return 1;
	}

	/* create shell process */
	const char *shell_argv[] = {bakey_sdl_config.shell, NULL};
	shell_pid = bakey_posix_launch(bakey_sdl_config.shell, shell_argv, pty);
	if (shell_pid < 0) {

		fprintf(stderr, "Bakey: %s\n", bakey_get_error());
		return 1;
	}

	/* set timer */
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
	cursor_flash_interval = (uint32_t)(bakey_sdl_config.cursor_flash_interval * 1000.f);
	cursor_flash_time = 0;
	cursor_flash_count = bakey_sdl_config.cursor_flash_count;

	uint32_t lftime = SDL_GetTicks();
	SDL_Event event;

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
		if (bakey_update(&context) != BAKEY_RESULT_SUCCESS) {

			fprintf(stderr, "Bakey: %s\n", bakey_get_error());
			return 1;
		}

		if (context.display_updated) {

			reset_cursor();
			display_updated = true;
		}

		if (draw) {

			while (SDL_PollEvent(&event)) {

				if (event.type == SDL_QUIT)
					running = false;

				else if (event.type == SDL_KEYDOWN ||
					 event.type == SDL_KEYUP)
					process_key(&event);

				else if (event.type == SDL_WINDOWEVENT &&
					 event.window.event == SDL_WINDOWEVENT_RESIZED)
					handle_resize(&event);
			}

			/* update terminal view */
			if (cursor_updated || display_updated) {

				draw_terminal();
				bakey_reset_damage(&context);

				SDL_Surface *window_surface = SDL_GetWindowSurface(window);

				SDL_Rect rect = {0, 0, 0, 0};
				SDL_BlitSurface(surface, NULL, window_surface, &rect);
				SDL_UpdateWindowSurface(window);

				display_updated = false;
			}
			cursor_updated = false;

			/* update cursor */
			uint32_t cftime = SDL_GetTicks();
			uint32_t ftime = cftime - lftime;
			lftime = cftime;

			cursor_flash_time += ftime;
			if (cursor_flash_time >= cursor_flash_interval && cursor_flash_count) {

				cursor_flash_time %= cursor_flash_interval;
				cursor_visible = !cursor_visible;
				cursor_flash_count--;
				cursor_updated = true;
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
	if (layout) g_object_unref(G_OBJECT(layout));
	if (font) pango_font_description_free(font);
	if (cr) cairo_destroy(cr);
	if (cr_surface) cairo_surface_destroy(cr_surface);
	if (surface) SDL_FreeSurface(surface);
	if (window) SDL_DestroyWindow(window);
	if (init_sdl) SDL_Quit();
}

int main() {

	int code = run();
	cleanup();
	return code;
}
