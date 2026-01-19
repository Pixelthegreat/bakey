/*
 * Copyright 2026, Elliot Kohlmyer
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BAKEY_H
#define BAKEY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define BAKEY_API extern

typedef enum bakey_result {
	BAKEY_RESULT_SUCCESS = 0,
	BAKEY_RESULT_FAILURE,

	BAKEY_RESULT_COUNT,
} bakey_result_t;

/*
 * Format for a color value: 0x00RRGGBB
 */
typedef uint32_t bakey_color_t;

/*
 * Style attributes
 */
typedef enum bakey_style {
	BAKEY_STYLE_NORMAL = 0,

	BAKEY_STYLE_ITALIC = 0x1,
	BAKEY_STYLE_BOLD = 0x2,
	BAKEY_STYLE_UNDERLINE = 0x4,
	BAKEY_STYLE_BLINKING = 0x8,
	BAKEY_STYLE_INVERSE = 0x10,
	BAKEY_STYLE_HIDDEN = 0x20,
	BAKEY_STYLE_STRIKETHROUGH = 0x40,
} bakey_style_t;

/*
 * Terminal display and terminal context
 */
typedef struct bakey_cell {
	wchar_t character;
	bakey_color_t background, foreground;
	bakey_style_t style;
} bakey_cell_t;

typedef struct bakey_display {
	size_t width, height;
	bakey_cell_t *cells;
} bakey_display_t;

typedef enum bakey_context_state {
	BAKEY_CONTEXT_STATE_NORMAL = 0,
	BAKEY_CONTEXT_STATE_ESCAPE_SEQUENCE,

	BAKEY_CONTEXT_STATE_COUNT,
} bakey_context_state_t;

typedef enum bakey_control_flag {
	BAKEY_CONTROL_FLAG_ECHO = 0x1,
	BAKEY_CONTROL_FLAG_CANONICAL = 0x2,
	BAKEY_CONTROL_FLAG_SIGNAL = 0x4,
} bakey_control_flag_t;

typedef enum bakey_control_character {
	BAKEY_CONTROL_CHARACTER_MINIMUM = 0,

	/*
	 * Signal characters
	 */
	BAKEY_CONTROL_CHARACTER_INTERRUPT,
	BAKEY_CONTROL_CHARACTER_QUIT,
	BAKEY_CONTROL_CHARACTER_SUSPEND,

	BAKEY_CONTROL_CHARACTER_SIGNAL_FIRST = BAKEY_CONTROL_CHARACTER_INTERRUPT,
	BAKEY_CONTROL_CHARACTER_SIGNAL_LAST = BAKEY_CONTROL_CHARACTER_SUSPEND,

	BAKEY_CONTROL_CHARACTER_COUNT,
} bakey_control_character_t;

#define BAKEY_CONTEXT_READBUFSZ 1024
#define BAKEY_CONTEXT_WRITEBUFSZ 4096
#define BAKEY_CONTEXT_SEQUENCEBUFSZ 1024

#define BAKEY_CONTEXT_MAX_SAVED 32

typedef struct bakey_context {
	bool init;
	size_t position;
	size_t old_position;
	size_t scroll_start; /* start of scroll region */
	size_t scroll_end; /* end of scroll region */
	bool display_updated;
	bakey_context_state_t state;
	/*
	 * Values set by the backend
	 */
	struct {
		bakey_display_t *display;
		bakey_result_t (*term_open)(void);
		bool (*term_waiting)(void);
		size_t (*term_read)(void *, size_t);
		size_t (*term_write)(const void *, size_t);
		void (*term_close)(void);
		void (*term_bell)(void);
		void (*term_signal)(bakey_control_character_t);
	} backend;
	/*
	 * Terminal control values
	 */
	struct {
		bakey_control_flag_t flags;
		long cc[BAKEY_CONTROL_CHARACTER_COUNT];
	} control;
	/*
	 * Active style values
	 */
	struct {
		bakey_color_t background, foreground;
		bakey_style_t flags;
	} style;
	/*
	 * Saved values
	 */
	struct {
		size_t positions[BAKEY_CONTEXT_MAX_SAVED];
		size_t npositions;
	} saved;
	/*
	 * Internal context values
	 */
	struct {
		size_t input_pos;
		size_t width, height;
		bool write_ready;
		size_t write_pos;
		bool sequence_ready;
		size_t sequence_pos;
		char readbuf[BAKEY_CONTEXT_READBUFSZ];
		char writebuf[BAKEY_CONTEXT_WRITEBUFSZ];
		char sequencebuf[BAKEY_CONTEXT_SEQUENCEBUFSZ]; /* escape sequences */
	} internal;
} bakey_context_t;

/*
 * Set the error string
 */
BAKEY_API void bakey_set_error(const char *fmt, ...);

/*
 * Get the error string or NULL if there is no error
 */
BAKEY_API const char *bakey_get_error(void);

/*
 * Initialize/open terminal
 *
 * The specified context must have the appropriate
 * values set by the backend beforehand:
 *   - display
 *   - term_open
 *   - term_read
 *   - term_write
 *   - term_close
 * If 'term_waiting' is set, bakey_update will use it
 * determine if it should read from the terminal.
 *
 * If 'term_bell' is set, it will be called whenever
 * the ASCII bell (BEL) character is sent.
 *
 * If 'term_signal' is set, it will be called whenever
 * one of the signal control characters is sent (i.e.
 * Ctrl+C, Ctrl+Z, etc).
 */
BAKEY_API bakey_result_t bakey_open(bakey_context_t *context);

/*
 * Adjust the internal state according to the newly
 * determined size
 */
BAKEY_API void bakey_adjust(bakey_context_t *context);

/*
 * Update terminal state
 */
BAKEY_API bakey_result_t bakey_update(bakey_context_t *context);

/*
 * Send an input character to the terminal
 */
BAKEY_API void bakey_send_character(bakey_context_t *context, wchar_t wc);

/*
 * Send a series of input characters to the terminal
 *
 * This is basically the same as multiple calls to
 * bakey_send_character.
 */
BAKEY_API void bakey_send_sequence(bakey_context_t *context, const wchar_t *wcs);

/*
 * Close terminal
 */
BAKEY_API void bakey_close(bakey_context_t *context);

#endif /* BAKEY_H */
