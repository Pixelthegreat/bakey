/*
 * Copyright 2026, Elliot Kohlmyer
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <locale.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define BAKEY_CONFIG_IMPL
#include <bakey-config.h>
#include <bakey-sdl-config.h>
#include <bakey-rc.h>
#include <bakey-posix.h>
#include <bakey.h>

#define FPS_CAP 60

#define FONT_TEXTURE_DIV 96

static bool init_sdl = false;
static SDL_Window *window = NULL;
static SDL_Texture *font_texture = NULL;
static SDL_Renderer *renderer = NULL;
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

static FT_Library ft_library;
static bool init_ft_library = false;
static FT_Face ft_face;

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
#ifdef DEBUG
	"./bakeysdlrc.default",
#endif
	NULL, /* $HOME/bakeyrc */
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
		bakey_sdl_config.font_size = (float)atof(value);

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

	return BAKEY_RESULT_SUCCESS;
}

static int load_config(void) {

	char buf[128];
	const char *home = getenv("HOME");

	if (home) {
		
		snprintf(buf, sizeof(buf), "/%s/bakeyrc", home);
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

/* load font texture */
static int load_font_texture(void) {

	size_t theight = (size_t)ft_face->num_glyphs / FONT_TEXTURE_DIV + 1;
	size_t twidth = FONT_TEXTURE_DIV;

	SDL_Surface *surface = SDL_CreateRGBSurface(0,
		twidth * fwidth, theight * fheight,
		32, 0x000000ff, 0x0000ff00, 0x00ff0000,
		0xff000000);

	if (!surface) {

		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return -1;
	}

	/* render glyphs */
	SDL_LockSurface(surface);

	for (FT_UInt i = 1; i < ft_face->num_glyphs; i++) {

		FT_UInt fy = i / FONT_TEXTURE_DIV;
		FT_UInt fx = i % FONT_TEXTURE_DIV;

		uint8_t *data = (uint8_t *)surface->pixels + fy * fheight *
				surface->pitch + fx * fwidth * 4;

		if (FT_Load_Glyph(ft_face, i, FT_LOAD_DEFAULT))
			continue;

		if (FT_Render_Glyph(ft_face->glyph, FT_RENDER_MODE_NORMAL))
			continue;

		/* determine area to copy */
		size_t x = ft_face->glyph->bitmap_left;
		if (x >= fwidth) continue;

		size_t w = ft_face->glyph->bitmap.width;
		if (x+w > fwidth) w = fwidth - x;

		size_t y = ft_face->size->metrics.y_ppem -
			   ft_face->glyph->bitmap_top;
		if (y >= fheight) continue;

		size_t h = ft_face->glyph->bitmap.rows;
		if (y+h > fheight) h = fheight - y;

		/* copy data */
		for (size_t py = 0; py < h; py++) {
			for (size_t px = 0; px < w; px++) {

				uint8_t value = ft_face->glyph->bitmap.buffer[py *
					ft_face->glyph->bitmap.pitch + px];

				size_t index = (y+py) * surface->pitch + (x+px) * 4;

				data[index] = 0xff;
				data[index + 1] = 0xff;
				data[index + 2] = 0xff;
				data[index + 3] = value;
			}
		}
	}

	SDL_UnlockSurface(surface);

	font_texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_FreeSurface(surface);

	if (!font_texture) {

		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return -1;
	}
	return 0;
}

/* timer callback */
static void sigh_alrm() {

	draw = true;
}

/* interpret key press */
static void reset_cursor(void);

static void process_key(SDL_Event *event) {

	if (event->type == SDL_TEXTINPUT) {

		wchar_t wc = 1;
		size_t pos = 0;
		for (; pos < SDL_TEXTINPUTEVENT_TEXT_SIZE && wc; pos++) {

			int nch = mbtowc(&wc, event->text.text+pos,
					 SDL_TEXTINPUTEVENT_TEXT_SIZE - pos);
			if (nch <= 0) continue;
			pos += (size_t)nch-1;

			if (wc) bakey_send_character(&context, wc);
		}

		return;
	}
	else if (event->key.keysym.sym == SDLK_LSHIFT ||
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

		/* control modifier */
		default:
			if (ctrled) {

				SDL_Keycode sym = event->key.keysym.sym;
				if (sym >= SDLK_a && sym <= SDLK_z) {

					sym -= SDLK_a-1;
					bakey_send_character(&context, (wchar_t)sym);
				}
			}
			break;
	}

	if (context.display_updated) {

		reset_cursor();
		display_updated = true;
	}
}

/* set color to bakey color */
static void set_color(bakey_color_t color, bool set_texture_mod) {

	if (!set_texture_mod) {

		SDL_SetRenderDrawColor(renderer,
			(uint8_t)((color >> 16) & 0xff),
			(uint8_t)((color >> 8) & 0xff),
			(uint8_t)(color & 0xff),
			0xff);
	}
	else {
		SDL_SetTextureColorMod(font_texture,
			(uint8_t)((color >> 16) & 0xff),
			(uint8_t)((color >> 8) & 0xff),
			(uint8_t)(color & 0xff));
	}
}

/* draw terminal */
static bool drawn_background = false;

struct damage {
	size_t left, right, top, bottom;
};

#define SWAP_VALUES(a, b) ({\
		a ^= b;\
		b ^= a;\
		a ^= b;\
	})

static void draw_terminal(struct damage *damage) {

	bool old_drawn_background = drawn_background;
	if (!drawn_background) {

		set_color(bakey_sdl_config.background, false);
		SDL_RenderClear(renderer);
		drawn_background = true;
	}

	damage->left = width;
	damage->right = 0;
	damage->top = height;
	damage->bottom = 0;

	/* draw text grid */
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
			FT_UInt index = FT_Get_Char_Index(ft_face, cell->character);

			size_t dx = x * fwidth;
			size_t dy = y * fheight;
			size_t sx = ((size_t)index % FONT_TEXTURE_DIV) * fwidth;
			size_t sy = ((size_t)index / FONT_TEXTURE_DIV) * fheight;

			/* update damage area */
			if (dx < damage->left) damage->left = dx;
			if (dx+fwidth > damage->right) damage->right = dx+fwidth;
			if (dy < damage->top) damage->top = dy;
			if (dy+fheight > damage->bottom) damage->bottom = dy+fheight;

			/* determine colors */
			bakey_color_t background = cell->background;
			bakey_color_t foreground = cell->foreground;

			if (cell->style & BAKEY_STYLE_INVERSE)
				SWAP_VALUES(background, foreground);

			if (position == context.position && cursor_visible) {

				switch (bakey_sdl_config.cursor_mode) {

					/* inverted cursor */
					case BAKEY_SDL_CURSOR_MODE_INVERTED:
						SWAP_VALUES(background, foreground);
						break;
				}
			}

			/* draw character */
			SDL_Rect dest = {
				(int)dx, (int)dy,
				(int)fwidth, (int)fheight,
			};
			SDL_Rect source = {
				(int)sx, (int)sy,
				(int)fwidth, (int)fheight,
			};

			set_color(background, false);
			SDL_RenderFillRect(renderer, &dest);

			if (index) {

				set_color(foreground, true);
				SDL_RenderCopy(renderer, font_texture, &source, &dest);
			}
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
static int run(int argc, const char **argv) {

	bakey_posix_options_t options;

	if (bakey_posix_parse_arguments(&options, argc, argv) != BAKEY_RESULT_SUCCESS)
		return 1;
	if (options.flags & BAKEY_POSIX_OPTION_FLAG_HELP)
		return 0;

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

	if (sigaction(SIGALRM, &action, NULL)) {

		perror("sigaction");
		return 1;
	}

	/* configure environment */
	setenv("TERM", "vt100-bakey", 1);

	const char *terminfo_path = NULL;
#ifdef DEBUG
	terminfo_path = "./terminfo";
#endif
	if (options.terminfo_path)
		terminfo_path = options.terminfo_path;

	static char termpathbuf[PATH_MAX];
	if (terminfo_path) {

		realpath(terminfo_path, termpathbuf);
		setenv("TERMINFO", termpathbuf, 1);
	}

	setenv("COLORS", "256", 1);
	setenv("COLORTERM", "truecolor", 1);

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
				  SDL_WINDOW_RESIZABLE |
				  SDL_WINDOW_HIDDEN);
	if (!window) {

		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}

	renderer = SDL_CreateRenderer(window, -1, 0);
	if (!renderer) {

		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}

	SDL_ShowWindow(window);

	/* initialize freetype */
	if (FT_Init_FreeType(&ft_library)) {

		fprintf(stderr, "Can't initialize FreeType\n");
		return 1;
	}
	init_ft_library = true;

	if (FT_New_Face(ft_library, bakey_sdl_config.font_face, 0, &ft_face)) {

		fprintf(stderr, "Can't load FreeType font face\n");
		return 1;
	}

	if (FT_Set_Char_Size(ft_face, 0, (FT_F26Dot6)(bakey_sdl_config.font_size * 64.f), 96, 96)) {

		fprintf(stderr, "Can't set FreeType font size\n");
		return 1;
	}

	FT_UInt index = FT_Get_Char_Index(ft_face, L'W');
	FT_Load_Glyph(ft_face, index, FT_LOAD_DEFAULT);
	FT_Render_Glyph(ft_face->glyph, FT_RENDER_MODE_NORMAL);

	fwidth = ft_face->glyph->advance.x >> 6;
	fheight = ft_face->size->metrics.height >> 6;

	/* load font texture */
	if (load_font_texture() < 0)
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
					 event.type == SDL_KEYUP ||
					 event.type == SDL_TEXTINPUT)
					process_key(&event);

				else if (event.type == SDL_WINDOWEVENT &&
					 event.window.event == SDL_WINDOWEVENT_RESIZED)
					handle_resize(&event);
			}

			/* update terminal view */
			if (cursor_updated || display_updated) {

				struct damage damage;
				draw_terminal(&damage);

				bakey_reset_damage(&context);
				SDL_RenderPresent(renderer);

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
	if (renderer) SDL_DestroyRenderer(renderer);
	if (window) SDL_DestroyWindow(window);
	if (init_sdl) SDL_Quit();
}

int main(int argc, const char **argv) {

	int code = run(argc, argv);
	cleanup();
	return code;
}
