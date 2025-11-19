#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <bakey.h>
#include <bakey-config.h>
#include <bakey-rc.h>

#define IS_DIGIT(c) (((c) >= '0') && ((c) <= '9'))
#define IS_IDENT(c) ((((c) >= 'a') && ((c) <= 'z')) || (((c) >= 'A') && ((c) <= 'Z')) || (c) == '_')
#define IS_IDENT_A(c) (IS_IDENT(c) || IS_DIGIT(c))
#define IS_WHITESPACE(c) (strchr(whitespace, (c))? 1: 0)
#define IS_WHITESPACE_NL(c) (strchr(whitespace_nl, (c))? 1: 0)
#define TO_LOWER(c) (((c) >= 'A' && (c) <= 'Z')? (c) + 32: (c))

static const char *whitespace = " \t\r";
static const char *whitespace_nl = " \t\r\n";

/* handle a key */
static bakey_result_t base_handle_key(const char *key, const char *value, bakey_rc_handle_key_t handle_key) {

	/* background */
	if (!strcmp(key, "background")) {

		if (bakey_rc_read_color(&bakey_config.background, value) != BAKEY_RESULT_SUCCESS)
			return BAKEY_RESULT_FAILURE;
	}

	/* foreground */
	else if (!strcmp(key, "foreground")) {

		if (bakey_rc_read_color(&bakey_config.foreground, value) != BAKEY_RESULT_SUCCESS)
			return BAKEY_RESULT_FAILURE;
	}

	/* 256-color palette */
	else if (!strncmp(key, "color", 5)) {

		int index = atoi(key + 5);
		if (index < 0 || index >= 256)
			return BAKEY_RESULT_SUCCESS;

		if (bakey_rc_read_color(bakey_config.colors + index, value) != BAKEY_RESULT_SUCCESS)
			return BAKEY_RESULT_FAILURE;
	}

	/* style flags */
	else if (!strcmp(key, "style"))
		bakey_config.style = (bakey_style_t)atoi(value);

	/* other */
	else if (!handle_key || handle_key(key, value) != BAKEY_RESULT_SUCCESS)
		return BAKEY_RESULT_FAILURE;

	return BAKEY_RESULT_SUCCESS;
}

/* interpret color value */
BAKEY_API bakey_result_t bakey_rc_read_color(bakey_color_t *color, const char *value) {

	if (*value != '#')
		return BAKEY_RESULT_SUCCESS;
	value++;
	*color = 0;

	const char *hex = "0123456789abcdef";
	const char *offset = NULL;
	while (*value) {

		char cc = *value++;
		offset = strchr(hex, cc);
		if (!offset) continue;

		*color = ((*color) << 4) + (bakey_color_t)(((size_t)offset - (size_t)hex) & 0xf);
	}

	return BAKEY_RESULT_SUCCESS;
}

/* load config from file */
BAKEY_API bakey_result_t bakey_rc_load(FILE *fp, bakey_rc_handle_key_t handle_key) {

	int line = 1;
	int cc = fgetc(fp);

	char key[128];
	char value[128];
	size_t keysz = 0, valuesz = 0;

	while (cc != EOF) {

		/* comment */
		if (cc == '#') {
			while (cc != EOF && cc != '\n')
				cc = fgetc(fp);
		}

		/* whitespace */
		else if (IS_WHITESPACE_NL(cc)) {

			if (cc == '\n') line++;
			cc = fgetc(fp);
		}

		/* key value pair */
		else if (IS_IDENT(cc)) {

			keysz = 0;
			valuesz = 0;

			while (IS_IDENT_A(cc)) {

				if (keysz < sizeof(key)-1)
					key[keysz++] = (char)cc;
				cc = fgetc(fp);
			}
			key[keysz] = 0;

			/* equals */
			while (IS_WHITESPACE(cc))
				cc = fgetc(fp);

			if (cc != '=') {

				bakey_set_error("Line %d: Expected '='", line);
				return BAKEY_RESULT_FAILURE;
			}
			cc = fgetc(fp);

			while (IS_WHITESPACE(cc))
				cc = fgetc(fp);

			/* value */
			while (cc != EOF && cc != '\n') {

				if (valuesz < sizeof(value)-1)
					value[valuesz++] = (char)cc;
				cc = fgetc(fp);
			}
			value[valuesz] = 0;

			/* handle key */
			if (base_handle_key(key, value, handle_key) != BAKEY_RESULT_SUCCESS) {

				if (!bakey_get_error())
					bakey_set_error("Line %d: Unrecognized key '%s'", line, key);
				return BAKEY_RESULT_FAILURE;
			}
		}

		/* error */
		else {

			bakey_set_error("Line %d: Unrecognized character '%c'", cc);
			return BAKEY_RESULT_FAILURE;
		}
	}
	return BAKEY_RESULT_SUCCESS;
}

/* load config from path */
BAKEY_API bakey_result_t bakey_rc_load_path(const char *path, bakey_rc_handle_key_t handle_key) {

	FILE *fp = fopen(path, "rb");
	if (!fp) {

		bakey_set_error("Can't open '%s': %s", path, strerror(errno));
		return BAKEY_RESULT_FAILURE;
	}
	bakey_result_t result = bakey_rc_load(fp, handle_key);
	fclose(fp);

	return result;
}
