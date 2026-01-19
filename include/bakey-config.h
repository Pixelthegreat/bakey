/*
 * Copyright 2026, Elliot Kohlmyer
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BAKEY_CONFIG_H
#define BAKEY_CONFIG_H

#include <bakey.h>

typedef struct bakey_config {
	/*
	 * The terminal color palette; The first sixteen
	 * colors should correspond to the standard sixteen
	 * color ansi palette.
	 */
	bakey_color_t background, foreground;
	bakey_color_t colors[256];
	/*
	 * Style flags
	 */
	bakey_style_t style;
} bakey_config_t;

/*
 * Define BAKEY_CONFIG_IMPL in exactly one source file
 * to include the Bakey configuration in the build.
 */
#ifdef BAKEY_CONFIG_IMPL

bakey_config_t bakey_config = {
	.background = 0x0e0f10,
	.foreground = 0x888a85,
	.colors = {
		/* black */
		[0]  = 0x2e3436,
		[8]  = 0x555753,
		/* red */
		[1]  = 0xcc0000,
		[9]  = 0xef2929,
		/* green */
		[2]  = 0x4e9a06,
		[10] = 0x8ae234,
		/* yellow */
		[3]  = 0xc4a000,
		[11] = 0xfce94f,
		/* blue */
		[4]  = 0x3465a4,
		[12] = 0x729fcf,
		/* magenta */
		[5]  = 0x75507b,
		[13] = 0xad7fa8,
		/* cyan */
		[6]  = 0x06989a,
		[14] = 0x34e2e2,
		/* white */
		[7]  = 0xd3d7cf,
		[15] = 0xeeeeec,
	},
	.style = 0,
};

#else
BAKEY_API bakey_config_t bakey_config;
#endif

#endif /* BAKEY_CONFIG_H */
