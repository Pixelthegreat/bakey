/*
 * Copyright 2026, Elliot Kohlmyer
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Simple program for testing different escape
 * sequences
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static void printf_flush(const char *fmt, ...) {

	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);

	fflush(stdout);
}

/*
 * Scroll region test one
 *
 * Expected output:
 *  A
 *  C
 *  D
 */
static void test_scroll1(void) {

	printf_flush("\x1b[H\x1b[2J\x1b[2;3rA\nB\nC\nD");

	getchar();
	printf_flush("\x1b[r");
}

/*
 * Scroll region test two
 *
 * Expected output:
 *  D
 *  A
 *  B
 */
static void test_scroll2(void) {

	printf_flush("\x1b[H\x1b[2J\x1b[2;4r\nA\nB\nC\x1b[2;1H\x1b[1TD");

	getchar();
	printf_flush("\x1b[r");
}

int main(int argc, const char **argv) {

	const char *test_name = argv[1];
	if (!test_name) {

		fprintf(stderr, "%s: Invalid arguments\nUsage: %s <test name>\n",
			argv[0], argv[0]);
		return 1;
	}

	if (!strcmp(test_name, "scroll1")) test_scroll1();
	else if (!strcmp(test_name, "scroll2")) test_scroll2();
	else getchar();

	return 0;
}
