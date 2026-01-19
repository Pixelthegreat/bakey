/*
 * Copyright 2026, Elliot Kohlmyer
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bakey posix utility helper library
 */
#ifndef BAKEY_POSIX_H
#define BAKEY_POSIX_H

#include <sys/types.h>
#include <bakey.h>

/*
 * Open a psuedo-terminal pair
 */
BAKEY_API bakey_result_t bakey_posix_openpt(int *p_pty, int *p_spty);

/*
 * Set pseudo-terminal window size
 */
BAKEY_API bakey_result_t bakey_posix_tiocswinsz(int pty, size_t width, size_t height,
						size_t pixel_width, size_t pixel_height);

/*
 * Launch child process
 */
BAKEY_API pid_t bakey_posix_launch(const char *prog, const char **argv, int pty);

/*
 * Get status of child process
 */
BAKEY_API bool bakey_posix_process_exited(pid_t pid);

#endif /* BAKEY_POSIX_H */
