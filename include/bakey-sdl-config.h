/*
 * Copyright 2026, Elliot Kohlmyer
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BAKEY_SDL_CONFIG_H
#define BAKEY_SDL_CONFIG_H

#include <bakey.h>

typedef enum bakey_sdl_cursor_mode {
	BAKEY_SDL_CURSOR_MODE_INVERTED = 0,

	BAKEY_SDL_CURSOR_MODE_COUNT,
} bakey_sdl_cursor_mode_t;

#define BAKEY_SDL_CONFIG_STRING_SIZE 128

typedef struct bakey_sdl_config {
	/*
	 * General window settings
	 */
	char title[BAKEY_SDL_CONFIG_STRING_SIZE];
	size_t width, height;
	bakey_color_t background;
	/*
	 * Font settings
	 */
	char font_face[BAKEY_SDL_CONFIG_STRING_SIZE];
	float font_size;
	/*
	 * Cursor settings
	 */
	bakey_sdl_cursor_mode_t cursor_mode;
	float cursor_flash_interval;
	size_t cursor_flash_count;
	/*
	 * Miscellaneous settings
	 */
	char locale[BAKEY_SDL_CONFIG_STRING_SIZE];
	char shell[BAKEY_SDL_CONFIG_STRING_SIZE];
} bakey_sdl_config_t;

/*
 * Define BAKEY_CONFIG_IMPL in exactly one source file
 * to include the Bakey configuration in the build.
 */
#ifdef BAKEY_CONFIG_IMPL

bakey_sdl_config_t bakey_sdl_config = {
	.title = "Bakey",
	.width = 640,
	.height = 408,
	.background = 0x0e0f10,
	.font_face = "DejaVu Sans Mono",
	.font_size = 14,
	.cursor_mode = BAKEY_SDL_CURSOR_MODE_INVERTED,
	.cursor_flash_interval = 0.625f,
	.cursor_flash_count = 8,
	.locale = "en_US.UTF8",
	.shell = "sh",
};

#else
BAKEY_API bakey_sdl_config_t bakey_sdl_config;
#endif

#endif /* BAKEY_SDL_CONFIG_H */
