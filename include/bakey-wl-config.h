#ifndef BAKEY_WL_CONFIG_H
#define BAKEY_WL_CONFIG_H

#include <bakey.h>

typedef enum bakey_wl_cursor_mode {
	BAKEY_WL_CURSOR_MODE_INVERTED = 0,

	BAKEY_WL_CURSOR_MODE_COUNT,
} bakey_wl_cursor_mode_t;

#define BAKEY_WL_CONFIG_STRING_SIZE 128

typedef struct bakey_wl_config {
	/*
	 * General window settings
	 */
	size_t width, height;
	bakey_color_t background;
	/*
	 * Font settings
	 */
	char font_face[BAKEY_WL_CONFIG_STRING_SIZE];
	float font_size;
	/*
	 * Cursor settings
	 */
	bakey_wl_cursor_mode_t cursor_mode;
	float cursor_flash_interval;
	size_t cursor_flash_count;
	/*
	 * Miscellneous settings
	 */
	char locale[BAKEY_WL_CONFIG_STRING_SIZE];
	char shell[BAKEY_WL_CONFIG_STRING_SIZE];
} bakey_wl_config_t;

/*
 * Define BAKEY_CONFIG_IMPL in exactly one source file
 * to include the Bakey configuration in the build.
 */
#ifdef BAKEY_CONFIG_IMPL

bakey_wl_config_t bakey_wl_config = {
	.width = 640,
	.height = 408,
	.background = 0x0e0f10,
	.font_face = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
	.font_size = 10.7f,
	.cursor_mode = BAKEY_WL_CURSOR_MODE_INVERTED,
	.cursor_flash_interval = 0.625f,
	.cursor_flash_count = 8,
	.locale = "en_US.UTF8",
	.shell = "sh",
};

#else
BAKEY_API bakey_wl_config_t bakey_wl_config;
#endif

#endif /* BAKEY_WL_CONFIG_H */
