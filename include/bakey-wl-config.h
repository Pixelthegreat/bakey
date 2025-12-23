#ifndef BAKEY_WL_CONFIG_H
#define BAKEY_WL_CONFIG_H

#include <bakey.h>

#define BAKEY_WL_CONFIG_STRING_SIZE 128

typedef struct bakey_wl_config {
	/*
	 * General window settings
	 */
	size_t width, height;
	bakey_color_t background;
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
	.locale = "en_US.UTF8",
	.shell = "sh",
};

#else
BAKEY_API bakey_wl_config_t bakey_wl_config;
#endif

#endif /* BAKEY_WL_CONFIG_H */
