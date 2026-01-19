/*
 * Copyright 2026, Elliot Kohlmyer
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <bakey.h>
#include <bakey-posix.h>

/* open pseudo-terminal pair */
BAKEY_API bakey_result_t bakey_posix_openpt(int *p_pty, int *p_spty) {

	*p_pty = posix_openpt(O_RDWR | O_NOCTTY);
	if (*p_pty < 0) {

		bakey_set_error("posix_openpt: %s", strerror(errno));
		return BAKEY_RESULT_FAILURE;
	}
	if (grantpt(*p_pty) < 0) {

		bakey_set_error("grantpt: %s", strerror(errno));
		return BAKEY_RESULT_FAILURE;
	}
	if (unlockpt(*p_pty) < 0) {

		bakey_set_error("unlockpt: %s", strerror(errno));
		return BAKEY_RESULT_FAILURE;
	}

	/* open slave */
	const char *path = ptsname(*p_pty);
	*p_spty = open(path, O_RDWR);

	if (*p_spty < 0) {

		bakey_set_error("open: %s", strerror(errno));
		return BAKEY_RESULT_FAILURE;
	}
	return BAKEY_RESULT_SUCCESS;
}

/* set terminal window size */
BAKEY_API bakey_result_t bakey_posix_tiocswinsz(int pty, size_t width, size_t height,
						size_t pixel_width, size_t pixel_height) {

	struct winsize winsize = {
		.ws_row = (unsigned short)height,
		.ws_col = (unsigned short)width,
		.ws_xpixel = (unsigned short)pixel_width,
		.ws_ypixel = (unsigned short)pixel_height,
	};
	if (ioctl(pty, TIOCSWINSZ, &winsize) < 0) {

		bakey_set_error("ioctl(TIOCSWINSZ): %s", strerror(errno));
		return BAKEY_RESULT_FAILURE;
	}
	return BAKEY_RESULT_SUCCESS;
}

/* launch child process with pseudo-terminal */
BAKEY_API pid_t bakey_posix_launch(const char *prog, const char **argv, int pty) {

	pid_t pid = fork();
	if (pid < 0) {

		bakey_set_error("fork: %s", strerror(errno));
		return -1;
	}
	else if (!pid) {

		/* open pseudo-terminal as stdin, stdout and stderr */
		const char *path = ptsname(pty);
		if (setsid() < 0) {

			perror("setsid");
			_exit(1);
		}

		int mpty = open(path, O_RDWR);
		if (mpty < 0) {

			perror("open");
			_exit(1);
		}

		dup2(mpty, 0);
		dup2(mpty, 1);
		dup2(mpty, 2);
		close(mpty);

		/* run program */
		if (execvp(prog, (char *const *)argv) < 0) {

			perror("execvp");
			_exit(1);
		}
		_exit(0);
	}
	else return pid;
}

/* check if process exited */
BAKEY_API bool bakey_posix_process_exited(pid_t pid) {

	int wstatus;
	return waitpid(pid, &wstatus, WCONTINUED | WNOHANG) == pid &&
	       WIFEXITED(wstatus);
}
