/*
 * Copyright 2026, Elliot Kohlmyer
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pty.h>

#define BAKEY_CONFIG_IMPL
#include <bakey-config.h>
#include <bakey.h>

static int pty = -1, spty = -1; /* pty master and slave */
static bool running = true;

/* bakey context */
#define WIDTH 128
#define HEIGHT 128

static bakey_cell_t cells[WIDTH * HEIGHT];

static bakey_display_t display = {
	.width = WIDTH, .height = HEIGHT,
	.cells = cells,
};

static bakey_result_t term_open(void);
static size_t term_read(void *buffer, size_t count);
static size_t term_write(const void *buffer, size_t count);
static void term_close(void);

static bakey_context_t context = {
	.backend = {
		.display = &display,
		.term_open = term_open,
		.term_read = term_read,
		.term_write = term_write,
		.term_close = term_close,
	},
};

/* open terminal */
static bakey_result_t term_open(void) {

	pty = posix_openpt(O_RDWR | O_NOCTTY);
	if (pty < 0) {

		perror("posix_openpt");
		return BAKEY_RESULT_FAILURE;
	}
	if (grantpt(pty) < 0) {

		perror("grantpt");
		return BAKEY_RESULT_FAILURE;
	}
	if (unlockpt(pty) < 0) {

		perror("unlockpt");
		return BAKEY_RESULT_FAILURE;
	}
	const char *path = ptsname(pty);
	printf("Opened pseudo-terminal at '%s'\n", path);

	/* open slave */
	spty = open(path, O_RDWR);
	if (spty < 0) {

		perror("open");
		return BAKEY_RESULT_FAILURE;
	}
	return BAKEY_RESULT_SUCCESS;
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

/* interrupt signal handler */
static void sigh_int() {

	running = false;
}

/* run application */
static int run(void) {

	signal(SIGINT, sigh_int);
	if (bakey_open(&context) != BAKEY_RESULT_SUCCESS)
		return 1;

	while (running) {
		if (bakey_update(&context) != BAKEY_RESULT_SUCCESS)
			return 1;
	}
	return 0;
}

/* clean up resources */
static void cleanup(void) {

	bakey_close(&context);
}

int main() {

	int code = run();
	cleanup();
	return code;
}
