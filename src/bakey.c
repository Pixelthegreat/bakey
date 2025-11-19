#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <wctype.h>
#include <bakey-config.h>
#include <bakey.h>

#define ESCAPE_SEQUENCE_COMMANDS "ABCDEfFGhHJKlLnmMPrsu@"

#define ERRBUFSZ 1024
static char errbuf[ERRBUFSZ];
static bool errset = false;

/* scroll view */
static void scroll_view(bakey_context_t *context) {

	bakey_display_t *display = context->backend.display;

	size_t size = display->width * display->height;

	if (context->position >= size) {

		memmove(display->cells, display->cells + display->width, (size - display->width) * sizeof(bakey_cell_t));
		for (size_t i = size - display->width; i < size; i++) {

			bakey_cell_t *cell = display->cells + i;

			cell->character = 0;
			cell->background = context->style.background;
			cell->foreground = context->style.foreground;
			cell->style = bakey_config.style;
		}
		context->position = size - display->width;

		if (context->internal.input_pos >= display->width)
			context->internal.input_pos -= display->width;
		else context->internal.input_pos = 0;
	}
}

/* process character */
#define SCROLL_VIEW_NONCANON(context) if (!(context->control.flags & BAKEY_CONTROL_FLAG_CANONICAL)) scroll_view(context)

static void print_character(bakey_context_t *context, wchar_t wc) {

	bakey_display_t *display = context->backend.display;
	if (!wc) return;

	/* print character */
	if (wc == '\n') {

		SCROLL_VIEW_NONCANON(context);
		context->position += display->width - (context->position % display->width);
	}

	else if (wc == '\r')
		context->position -= context->position % display->width;

	else if (wc == '\b') {
		if (context->position) context->position--;
	}

	else if (wc == '\x07') {

		if (context->backend.term_bell)
			context->backend.term_bell();
	}

	else if (wc == '\x1b') {

		context->state = BAKEY_CONTEXT_STATE_ESCAPE_SEQUENCE;
		context->internal.sequence_ready = false;
		context->internal.sequence_pos = 0;
	}

	else if (wc == '\x7f') {

		bakey_cell_t *cell = display->cells + context->position;

		cell->character = 0;
		cell->background = context->style.background;
		cell->foreground = context->style.foreground;
		cell->style = context->style.flags;
	}

	else {

		SCROLL_VIEW_NONCANON(context);

		bakey_cell_t *cell = display->cells + context->position++;

		cell->character = wc;
		cell->background = context->style.background;
		cell->foreground = context->style.foreground;
		cell->style = context->style.flags;
	}

	/* scroll view */
	if (context->control.flags & BAKEY_CONTROL_FLAG_CANONICAL)
		scroll_view(context);
	context->display_updated = true;
}

/* read escape sequence character */
static void interpret_sequence(bakey_context_t *context);

static void read_sequence_character(bakey_context_t *context, wchar_t wc) {

	if (context->internal.sequence_pos > BAKEY_CONTEXT_SEQUENCEBUFSZ - MB_CUR_MAX) {

		context->state = BAKEY_CONTEXT_STATE_NORMAL;
		interpret_sequence(context);
		return;
	}
	char *buf = context->internal.sequencebuf + context->internal.sequence_pos;

	int ibytes = wctomb(buf, wc);
	if (ibytes < 0)
		return;

	size_t nbytes = (size_t)ibytes;
	context->internal.sequence_pos += nbytes;

	/* end of sequence */
	if (strchr(ESCAPE_SEQUENCE_COMMANDS, *buf) ||
	    (context->internal.sequence_pos == 2 &&
	     !memcmp(context->internal.sequencebuf, " 7", 2)) ||
	    (context->internal.sequence_pos == 2 &&
	     !memcmp(context->internal.sequencebuf, " 8", 2))) {

		context->state = BAKEY_CONTEXT_STATE_NORMAL;
		interpret_sequence(context);
	}
}

/* process character */
static void (*character_handlers[])(bakey_context_t *, wchar_t) = {
	[BAKEY_CONTEXT_STATE_NORMAL] = print_character,
	[BAKEY_CONTEXT_STATE_ESCAPE_SEQUENCE] = read_sequence_character,
};

static void process_character(bakey_context_t *context, wchar_t wc) {

	character_handlers[context->state](context, wc);
}

/* get integer */
static size_t to_int(const char *s, int *i) {

	size_t n = 0; *i = 0;
	while (*s >= '0' && *s <= '9') {

		*i = ((*i) * 10) + (int)(*s++ - '0');
		n++;
	}
	return n;
}

/* get next integer */
static size_t get_next_int(const char *s, int *i) {

	size_t n = 0;
	while (*s == ';') n++, s++;

	size_t t = to_int(s, i);
	if (!t) *i = -1;

	return n + t;
}

/* get rgb color value */
static size_t get_next_rgb(const char *s, bakey_color_t *color) {

	int r, g, b;
	size_t n = 0, i = 0;

	i = get_next_int(s, &r);
	if (r < 0 || r >= 256) return 0;
	n += i, s += i;

	i = get_next_int(s, &g);
	if (g < 0 || g >= 256) return 0;
	n += i, s += i;

	i = get_next_int(s, &b);
	if (b < 0 || b >= 256) return 0;
	n += i, s += i;

	*color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
	return n;
}

/* change style */
static void command_m(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	size_t pos = 1;
	while (pos < length) {

		int num;
		pos += get_next_int(sequence + pos, &num);

		if (num < 0) break;

		/* set style modes */
		else if (num == 1)
			context->style.flags |= BAKEY_STYLE_BOLD;

		else if (num == 2 || num == 22)
			context->style.flags &= ~BAKEY_STYLE_BOLD;

		else if (num == 3)
			context->style.flags |= BAKEY_STYLE_ITALIC;

		else if (num == 4)
			context->style.flags |= BAKEY_STYLE_UNDERLINE;

		else if (num == 5)
			context->style.flags |= BAKEY_STYLE_BLINKING;

		else if (num == 6)
			context->style.flags |= BAKEY_STYLE_INVERSE;

		else if (num == 7)
			context->style.flags |= BAKEY_STYLE_HIDDEN;

		else if (num == 8)
			context->style.flags |= BAKEY_STYLE_STRIKETHROUGH;

		/* reset style modes */
		else if (num == 23)
			context->style.flags &= ~BAKEY_STYLE_ITALIC;

		else if (num == 24)
			context->style.flags &= ~BAKEY_STYLE_UNDERLINE;

		else if (num == 25)
			context->style.flags &= ~BAKEY_STYLE_BLINKING;

		else if (num == 26)
			context->style.flags &= ~BAKEY_STYLE_INVERSE;

		else if (num == 27)
			context->style.flags &= ~BAKEY_STYLE_HIDDEN;

		else if (num == 28)
			context->style.flags &= ~BAKEY_STYLE_STRIKETHROUGH;

		/* set foreground color */
		else if (num >= 30 && num <= 37) {

			int offset = context->style.flags & BAKEY_STYLE_BOLD? 8: 0;
			context->style.foreground = bakey_config.colors[num - 30];
		}

		else if (num == 38) {

			pos += get_next_int(sequence + pos, &num);

			/* 256-color palette */
			if (num == 5) {

				pos += get_next_int(sequence + pos, &num);
				if (num < 0 || num >= 256) break;

				context->style.foreground = bakey_config.colors[num];
			}

			/* rgb color */
			else if (num == 2) {

				size_t nread = get_next_rgb(sequence + pos, &context->style.foreground);
				if (!nread) break;
				pos += nread;
			}

			else break;
		}

		/* reset foreground color */
		else if (num == 39) context->style.foreground = bakey_config.foreground;

		/* set background color */
		else if (num >= 40 && num <= 47)
			context->style.background = bakey_config.colors[num - 40];

		else if (num == 48) {

			pos += get_next_int(sequence + pos, &num);

			if (num == 5) {

				pos += get_next_int(sequence + pos, &num);
				if (num < 0 || num >= 256) break;

				context->style.background = bakey_config.colors[num];
			}

			else if (num == 2) {

				size_t nread = get_next_rgb(sequence + pos, &context->style.background);
				if (!nread) break;
				pos += nread;
			}

			else break;
		}

		else if (num == 49) context->style.background = bakey_config.background;

		/* set bright foreground or background color */
		else if (num >= 90 && num <= 97) context->style.foreground = bakey_config.colors[num - 82];

		else if (num >= 100 && num <= 107) context->style.foreground = bakey_config.colors[num - 92];

		/* reset style */
		else if (!num) {

			context->style.background = bakey_config.background;
			context->style.foreground = bakey_config.foreground;
			context->style.flags = bakey_config.style;
		}

		else break;
	}
}

/* move cursor */
static void command_H(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[')
		return;

	size_t pos = 1;

	int y = 1, x = 1;
	pos += get_next_int(sequence + pos, &y);
	if (y < 1) y = 1;

	pos += get_next_int(sequence + pos, &x);
	if (x < 1) x = 1;

	size_t sx = (size_t)x - 1;
	size_t sy = (size_t)y - 1;

	bakey_display_t *display = context->backend.display;

	if (sy >= display->height) sy = display->height - 1;
	if (sx >= display->width) sx = display->width - 1;

	context->position = sy * display->width + sx;
}

/* erase screen */
static void command_J(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	bakey_display_t *display = context->backend.display;

	int num;
	(void)to_int(sequence+1, &num);

	size_t start = context->position;
	size_t end = display->width * display->height;
	switch (num) {

		/* erase from cursor until end of screen */
		case 0:
			break;

		/* erase from cursor to beginning of screen */
		case 1:
			end = start;
			start = 0;
			break;

		/* erase entire screen */
		case 2:
			start = 0;
			break;

		default: return;
	}

	for (size_t i = start; i < end; i++) {

		bakey_cell_t *cell = display->cells + i;

		cell->character = 0;
		cell->background = context->style.background;
		cell->foreground = context->style.foreground;
		cell->style = context->style.flags;
	}
	context->display_updated = true;
}

/* erase line */
static void command_K(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	bakey_display_t *display = context->backend.display;

	int num;
	(void)to_int(sequence+1, &num);

	size_t start = context->position;
	size_t end = context->position + (display->width - (context->position % display->width));
	switch (num) {

		/* erase from cursor until end of line */
		case 0: break;

		/* erase from cursor to beginning of line */
		case 1:
			end = start;
			start = context->position - (context->position % display->width);
			break;

		/* erase entire line */
		case 2:
			start = context->position - (context->position % display->width);
			break;

		default: return;
	}

	for (size_t i = start; i < end; i++) {

		bakey_cell_t *cell = display->cells + i;

		cell->character = 0;
		cell->background = context->style.background;
		cell->foreground = context->style.foreground;
		cell->style = context->style.flags;
	}
	context->display_updated = true;
}

/* various cursor moves */
static void command_A(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	bakey_display_t *display = context->backend.display;

	int num;
	(void)to_int(sequence+1, &num);
	if (num <= 0) num = 1;

	if (context->position >= display->width * (size_t)num)
		context->position -= display->width * (size_t)num;
}

static void command_B(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	bakey_display_t *display = context->backend.display;

	int num;
	(void)to_int(sequence+1, &num);
	if (num <= 0) num = 1;

	if (display->height >= (size_t)num &&
	    context->position < (display->height - (size_t)num) * display->width)
		context->position += (size_t)num * display->width;
}

static void command_C(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	bakey_display_t *display = context->backend.display;

	int num;
	(void)to_int(sequence+1, &num);
	if (num <= 0) num = 1;

	if (display->width >= (size_t)num &&
	    (context->position % display->width) < display->width - (size_t)num)
		context->position += (size_t)num;
}

static void command_D(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	bakey_display_t *display = context->backend.display;

	int num;
	(void)to_int(sequence+1, &num);
	if (num <= 0) num = 1;

	if ((context->position % display->width) >= (size_t)num)
		context->position -= (size_t)num;
}

static void command_E(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	bakey_display_t *display = context->backend.display;

	int num;
	(void)to_int(sequence+1, &num);

	if (display->height >= (size_t)num &&
	    context->position < (display->height - (size_t)num) * display->width) {

		context->position += (size_t)num * display->width;
		context->position -= (context->position % display->width);
	}
}

static void command_F(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	bakey_display_t *display = context->backend.display;

	int num;
	(void)to_int(sequence+1, &num);

	if (context->position >= display->width * (size_t)num) {

		context->position -= display->width * (size_t)num;
		context->position -= (context->position % display->width);
	}
}

static void command_G(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	bakey_display_t *display = context->backend.display;

	int num;
	(void)to_int(sequence+1, &num);

	if ((size_t)num < display->width)
		context->position = (context->position - (context->position % display->width)) + (size_t)num;
}

/* get cursor position */
static void command_n(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence++ != '[' || *sequence != '6') return;

	bakey_display_t *display = context->backend.display;

	size_t ypos = context->position / display->width;
	size_t xpos = context->position % display->width;

	if (ypos >= display->width)
		ypos = display->width - 1;

	char buf[128];
	snprintf(buf, 128, "\e[%zu;%zuR", ypos+1, xpos+1);
	size_t len = strlen(buf);

	if (len+1 < BAKEY_CONTEXT_WRITEBUFSZ) {

		memcpy(context->internal.writebuf + context->internal.write_pos, buf, len);
		context->internal.write_pos += len;
	}
}

/* save cursor position */
static void command_7(bakey_context_t *context, const char *sequence, size_t length) {

	if (context->saved.npositions >= BAKEY_CONTEXT_MAX_SAVED) {

		memmove(context->saved.positions, context->saved.positions + 1, sizeof(size_t) * (BAKEY_CONTEXT_MAX_SAVED - 1));
		context->saved.npositions--;
	}
	context->saved.positions[context->saved.npositions++] = context->position;
}

/* restore cursor position */
static void command_8(bakey_context_t *context, const char *sequence, size_t length) {

	if (context->saved.npositions)
		context->position = context->saved.positions[--context->saved.npositions];
}

/* insert blank characters */
static void command_at(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	int num;
	to_int(sequence + 1, &num);
	if (num < 0) num = 1;

	bakey_display_t *display = context->backend.display;

	size_t remainder = (display->width * display->height) - context->position;
	size_t nchars = remainder < (size_t)num? remainder: (size_t)num;

	if (remainder != nchars)
		memmove(display->cells + context->position + nchars, display->cells + context->position, remainder - nchars);

	for (size_t i = 0; i < nchars; i++) {

		size_t pos = context->position + i;

		display->cells[pos].character = 0;
		display->cells[pos].background = context->style.background;
		display->cells[pos].foreground = context->style.foreground;
		display->cells[pos].style = context->style.flags;
	}
}

/* delete characters */
static void command_P(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	int num;
	to_int(sequence + 1, &num);
	if (num < 0) num = 1;

	bakey_display_t *display = context->backend.display;
	size_t size = display->width * display->height;

	size_t remainder = size - context->position;
	size_t nchars = remainder < (size_t)num? remainder: (size_t)num;

	memmove(display->cells + context->position, display->cells + context->position + nchars, remainder - nchars);

	for (size_t i = size - nchars; i < size; i++) {

		display->cells[i].character = 0;
		display->cells[i].background = context->style.background;
		display->cells[i].foreground = context->style.foreground;
		display->cells[i].style = context->style.flags;
	}
}

/* insert blank lines */
static void command_L(bakey_context_t *context, const char *sequence, size_t length) {

	if (*sequence != '[') return;

	int num;
	to_int(sequence + 1, &num);
	if (num < 0) num = 1;

	bakey_display_t *display = context->backend.display;

	size_t position = (context->position + (display->width - (context->position % display->width)));
	size_t remainder = display->height - position;
	size_t nchars = remainder < (size_t)num * display->width? remainder: (size_t)num * display->width;

	if (remainder != nchars)
		memmove(display->cells + position + nchars, display->cells + position, remainder - nchars);

	for (size_t i = 0; i < nchars; i++) {

		size_t pos = context->position + i;

		display->cells[pos].character = 0;
		display->cells[pos].background = context->style.background;
		display->cells[pos].foreground = context->style.foreground;
		display->cells[pos].style = context->style.flags;
	}
}

/* interpret escape sequence */
static void (*command_handlers[256])(bakey_context_t *, const char *, size_t) = {
	['A'] = command_A,
	['B'] = command_B,
	['C'] = command_C,
	['D'] = command_D,
	['E'] = command_E,
	['F'] = command_F,
	['G'] = command_G,
	['H'] = command_H,
	['f'] = command_H,
	['J'] = command_J,
	['K'] = command_K,
	['L'] = command_L,
	['n'] = command_n,
	['m'] = command_m,
	['P'] = command_P,
	['s'] = command_7,
	['u'] = command_8,
	['@'] = command_at,
};

static void interpret_sequence(bakey_context_t *context) {

	const char *sequence = context->internal.sequencebuf;
	size_t length = context->internal.sequence_pos;

	if (!length) return;
	char command = sequence[--length];

	if (!command_handlers[command])
		return;
	command_handlers[command](context, sequence, length);
}

/* flush input buffer */
static void flush_input(bakey_context_t *context) {

	size_t min = (size_t)context->control.cc[BAKEY_CONTROL_CHARACTER_MINIMUM];
	if (!min) min = 1;

	if (!(context->control.flags & BAKEY_CONTROL_FLAG_CANONICAL) &&
	    context->internal.write_pos >= min) {

		context->backend.term_write(context->internal.writebuf, context->internal.write_pos);
		context->internal.write_pos = 0;
	}

	/* canonical mode */
	else if ((context->control.flags & BAKEY_CONTROL_FLAG_CANONICAL) &&
		  context->internal.write_ready) {

		context->backend.term_write(context->internal.writebuf, context->internal.write_pos);
		context->internal.write_ready = false;
		context->internal.write_pos = 0;
	}
}

/* set error string */
BAKEY_API void bakey_set_error(const char *fmt, ...) {

	va_list args;
	va_start(args, fmt);
	vsnprintf(errbuf, ERRBUFSZ, fmt, args);
	va_end(args);

	errset = true;
}

/* get error string */
BAKEY_API const char *bakey_get_error(void) {

	return errset? errbuf: NULL;
}

/* initialize context */
BAKEY_API bakey_result_t bakey_open(bakey_context_t *context) {

	if (!context || context->init ||
	    !context->backend.display ||
	    !context->backend.term_open ||
	    !context->backend.term_read ||
	    !context->backend.term_write ||
	    !context->backend.term_close) {

		bakey_set_error("Invalid context state");
		return BAKEY_RESULT_FAILURE;
	}

	context->init = true;
	if (context->backend.term_open() != BAKEY_RESULT_SUCCESS) {

		bakey_set_error("Unable to open pseudo terminal");
		return BAKEY_RESULT_FAILURE;
	}

	context->position = 0;
	context->display_updated = false;
	context->state = BAKEY_CONTEXT_STATE_NORMAL;

	context->control.flags = BAKEY_CONTROL_FLAG_ECHO |
				 BAKEY_CONTROL_FLAG_CANONICAL |
				 BAKEY_CONTROL_FLAG_SIGNAL;

	context->control.cc[BAKEY_CONTROL_CHARACTER_MINIMUM] = 1;
	context->control.cc[BAKEY_CONTROL_CHARACTER_INTERRUPT] = 0; /* Ctrl+C */
	context->control.cc[BAKEY_CONTROL_CHARACTER_QUIT] = 0; /* Ctrl+\ */
	context->control.cc[BAKEY_CONTROL_CHARACTER_SUSPEND] = 0; /* Ctrl+Z */

	context->style.background = bakey_config.background;
	context->style.foreground = bakey_config.foreground;
	context->style.flags = bakey_config.style;

	context->saved.npositions = 0;

	context->internal.width = context->backend.display->width;
	context->internal.height = context->backend.display->height;
	context->internal.write_ready = false;
	context->internal.write_pos = 0;
	context->internal.sequence_ready = false;
	context->internal.sequence_pos = 0;
	context->internal.input_pos = 0;

	for (size_t i = 0; i < context->internal.width * context->internal.height; i++) {

		bakey_cell_t *cell = context->backend.display->cells + i;

		cell->character = 0;
		cell->background = bakey_config.background;
		cell->foreground = bakey_config.foreground;
		cell->style = bakey_config.style;
	}
	return BAKEY_RESULT_SUCCESS;
}

/* adjust position to size */
BAKEY_API void bakey_adjust(bakey_context_t *context) {

	if (!context || !context->init) return;

	context->position = 0;
	context->internal.width = context->backend.display->width;
	context->internal.height = context->backend.display->height;
}

/* update context */
BAKEY_API bakey_result_t bakey_update(bakey_context_t *context) {

	if (!context || !context->init) {

		bakey_set_error("Context not initialized");
		return BAKEY_RESULT_FAILURE;
	}
	context->display_updated = false;

	/* read from terminal */
	if (context->backend.term_waiting && !context->backend.term_waiting())
		return BAKEY_RESULT_SUCCESS;

	size_t nread = BAKEY_CONTEXT_READBUFSZ;
	while (nread == BAKEY_CONTEXT_READBUFSZ) {

		nread = context->backend.term_read(context->internal.readbuf,
						   BAKEY_CONTEXT_READBUFSZ);

		size_t pos = 0;
		while (pos < nread) {

			wchar_t wc;
			int nbytes = mbtowc(&wc, context->internal.readbuf + pos, nread - pos);
			if (nbytes <= 0) break;

			process_character(context, wc);
			pos += (size_t)nbytes;
		}
	}

	/* flush input buffer */
	flush_input(context);
	return BAKEY_RESULT_SUCCESS;
}

/* send */
static void add_to_writebuf(bakey_context_t *context, int ch) {

	if (context->internal.write_pos < BAKEY_CONTEXT_WRITEBUFSZ)
		context->internal.writebuf[context->internal.write_pos++] = ch;
}

BAKEY_API void bakey_send_character(bakey_context_t *context, wchar_t wc) {

	if (!context || !context->init || context->internal.write_ready)
		return;

	/* send signal */
	if (context->control.flags & BAKEY_CONTROL_FLAG_SIGNAL) {

		bakey_control_character_t cc = BAKEY_CONTROL_CHARACTER_SIGNAL_FIRST;
		for (; cc < BAKEY_CONTROL_CHARACTER_SIGNAL_LAST+1; cc++) {

			if ((wchar_t)context->control.cc[cc] == wc) {

				if (!context->backend.term_signal)
					return;

				context->backend.term_signal(cc);
				return;
			}
		}
	}

	char *buf = NULL;
	switch (wc) {

		/* newline */
		case '\r':
			if ((context->control.flags & BAKEY_CONTROL_FLAG_ECHO) &&
			    context->internal.write_pos)
				context->position = context->internal.input_pos;

			add_to_writebuf(context, '\r');

			if (context->control.flags & BAKEY_CONTROL_FLAG_CANONICAL)
				context->internal.write_ready = true;
			break;

		/* delete / backspace */
		case '\x7f':
			if (context->control.flags & BAKEY_CONTROL_FLAG_CANONICAL) {

				if (!context->internal.write_pos)
					break;

				size_t nmax = context->internal.write_pos < MB_CUR_MAX?
					      context->internal.write_pos: MB_CUR_MAX;
				buf = context->internal.writebuf + context->internal.write_pos;

				size_t nbytes = 0;
				for (; nbytes < nmax; nbytes++) {

					if (mblen(buf - nbytes, nbytes) > 0)
						break;
				}
				if (!nbytes) break;
				context->internal.write_pos -= nbytes;

				if (context->position &&
				    (context->control.flags & BAKEY_CONTROL_FLAG_ECHO)) {

					print_character(context, '\b');
					print_character(context, '\x7f');
				}
			}
			else add_to_writebuf(context, '\x7f');
			break;

		/* normal character */
		default:
			if (context->internal.write_pos >= BAKEY_CONTEXT_WRITEBUFSZ - MB_CUR_MAX)
				break;
			else if (!context->internal.write_pos)
				context->internal.input_pos = context->position;

			buf = context->internal.writebuf + context->internal.write_pos;
			int nwrite = wctomb(buf, wc);

			if (nwrite <= 0) break;
			context->internal.write_pos += (size_t)nwrite;

			if (context->control.flags & BAKEY_CONTROL_FLAG_ECHO) {

				if (wc == '\x1b') {

					print_character(context, '^');
					print_character(context, '[');
				}
				else print_character(context, wc);
			}
			break;
	}
}

/* send character sequence */
BAKEY_API void bakey_send_sequence(bakey_context_t *context, const wchar_t *wcs) {

	while (*wcs) bakey_send_character(context, *wcs++);
}

/* destroy context */
BAKEY_API void bakey_close(bakey_context_t *context) {

	if (!context || !context->init) return;

	context->backend.term_close();
	context->init = false;
}
