/*
 * Copyright 2026, Elliot Kohlmyer
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bakey configuration file support / extension library
 *
 * For an overview of the configuration format, see
 * ./bakeysdlrc
 */
#ifndef BAKEY_RC_H
#define BAKEY_RC_H

#include <stdio.h>
#include <bakey.h>

/*
 * Function called when a key is not recognized by the
 * main parser
 *
 * Should return BAKEY_RESULT_SUCCESS if the key is
 * recognized, BAKEY_RESULT_FAILURE otherwise.
 */
typedef bakey_result_t (*bakey_rc_handle_key_t)(const char *, const char *);

/*
 * Interpret a color value
 */
BAKEY_API bakey_result_t bakey_rc_read_color(bakey_color_t *color, const char *value);

/*
 * Load and parse a configuration
 */
BAKEY_API bakey_result_t bakey_rc_load(FILE *fp, bakey_rc_handle_key_t handle_key);

/*
 * Same as bakey_rc_load, but also opens the file
 * instead of requesting an open file pointer
 */
BAKEY_API bakey_result_t bakey_rc_load_path(const char *path, bakey_rc_handle_key_t handle_key);

#endif /* BAKEY_RC_H */
