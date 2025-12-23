#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pty.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <wayland-client.h>
#include <bakey-xdg-shell.h>
#include <xkbcommon/xkbcommon.h>

#define BAKEY_CONFIG_IMPL
#include <bakey-config.h>
#include <bakey-wl-config.h>
#include <bakey-rc.h>
#include <bakey.h>

#define FPS_CAP 60

static int pty = -1, spty = -1;
static pid_t shell_pid = -1;

static bool display_updated = true;
static bool running = true;
static bool draw = true;
static bool shifted = false;
static bool ctrled = false;

static size_t fwidth = 9, fheight = 16;

/* wayland stuff */
static size_t width, height;
static int display_fd = -1;
static struct wl_display *wl_display;

static struct wl_registry *wl_registry;
static struct wl_compositor *wl_compositor;
static struct xdg_wm_base *xdg_wm_base;
static struct wl_shm *wl_shm;
static struct wl_seat *wl_seat;

static struct wl_surface *wl_surface;
static struct xdg_surface *xdg_surface;
static struct xdg_toplevel *xdg_toplevel;
static struct wl_keyboard *wl_keyboard;
static struct wl_callback *wl_callback_frame;

static struct xkb_context *xkb_context;
static struct xkb_keymap *xkb_keymap;
static struct xkb_state *xkb_state;
static int keymap_fd = -1;
static void *keymap_data;
static size_t keymap_size;

static struct wl_shm_pool *wl_shm_pool;
static struct wl_buffer *wl_buffer_shm;
static int shm_fd = -1;
static void *shm_data;
static size_t shm_size;

static bool frame_ready = false;

/* bakey context */
static bakey_display_t display;

static bakey_result_t term_open(void);
static size_t term_read(void *buffer, size_t count);
static size_t term_write(const void *buffer, size_t count);
static void term_close(void);
static void term_signal(bakey_control_character_t cc);

static bakey_context_t context = {
	.backend = {
		.display = &display,
		.term_open = term_open,
		.term_read = term_read,
		.term_write = term_write,
		.term_close = term_close,
		.term_signal = term_signal,
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

	/* open slave */
	const char *path = ptsname(pty);
	spty = open(path, O_RDWR);

	if (spty < 0) {

		perror("open");
		return BAKEY_RESULT_FAILURE;
	}

	/* set window size */
	struct winsize winsize = {
		.ws_row = (unsigned short)display.height,
		.ws_col = (unsigned short)display.width,
		.ws_xpixel = width,
		.ws_ypixel = height,
	};
	if (ioctl(pty, TIOCSWINSZ, &winsize) < 0) {

		perror("ioctl(TIOCSWINSZ)");
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

/* send signal to child process */
static void term_signal(bakey_control_character_t cc) {

	int sig;
	switch (cc) {
		case BAKEY_CONTROL_CHARACTER_INTERRUPT: sig = SIGINT; break;
		case BAKEY_CONTROL_CHARACTER_QUIT: sig = SIGQUIT; break;
		case BAKEY_CONTROL_CHARACTER_SUSPEND: sig = SIGTSTP; break;
		default: return;
	}
	kill(shell_pid, sig);
}

/* load configuration */
#ifdef BAKEY_RC

#define NRCPATHS (sizeof(rcpaths) / sizeof(rcpaths[0]))
static const char *rcpaths[] = {
	"./bakeywlrc",
	"./bakeyrc",
	"/etc/bakeywlrc",
	"/etc/bakeyrc",
	BAKEY_PREFIX "/share/bakeywlrc",
	BAKEY_PREFIX "/share/bakeyrc",
};

static bakey_result_t handle_key(const char *key, const char *value) {

	/* window settings */
	if (!strcmp(key, "wl_width"))
		bakey_wl_config.width = (size_t)atoi(value);

	else if (!strcmp(key, "wl_height"))
		bakey_wl_config.height = (size_t)atoi(value);

	else if (!strcmp(key, "wl_background")) {

		if (bakey_rc_read_color(&bakey_wl_config.background, value) != BAKEY_RESULT_SUCCESS)
			return BAKEY_RESULT_FAILURE;
	}

	/* miscellaneous settings */
	else if (!strcmp(key, "wl_locale"))
		strncpy(bakey_wl_config.locale, value, BAKEY_WL_CONFIG_STRING_SIZE);

	else if (!strcmp(key, "wl_shell"))
		strncpy(bakey_wl_config.shell, value, BAKEY_WL_CONFIG_STRING_SIZE);

	else return BAKEY_RESULT_FAILURE;
	return BAKEY_RESULT_SUCCESS;
}

static int load_config(void) {

	struct stat st;
	for (size_t i = 0; i < NRCPATHS; i++) {

		if (stat(rcpaths[i], &st) < 0 ||
		    !S_ISREG(st.st_mode))
			continue;

		if (bakey_rc_load_path(rcpaths[i], handle_key) != BAKEY_RESULT_SUCCESS) {

			fprintf(stderr, "Bakey: %s\n", bakey_get_error());
			return -1;
		}
		break;
	}
	return 0;
}

#endif

/* timer callback */
static void sigh_alrm() {

	draw = true;
}

/* registry listener */
static void registry_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version);
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t id);

static struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* register interface */
static void registry_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {

	if (!strcmp(interface, "wl_compositor"))
		wl_compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
	else if (!strcmp(interface, "xdg_wm_base"))
		xdg_wm_base = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
	else if (!strcmp(interface, "wl_shm"))
		wl_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
	else if (!strcmp(interface, "wl_seat"))
		wl_seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
}

/* remove interface */
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t id) {
}

/* xdg surface listener */
static void surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t id);

static struct xdg_surface_listener surface_listener = {
	.configure = surface_configure,
};

/* xdg surface configure */
static void surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t id) {

	xdg_surface_ack_configure(xdg_surface, id);
	frame_ready = true;
}

/* keyboard listener */
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size);
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t id, struct wl_surface *surface, struct wl_array *keys);
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t id, struct wl_surface *surface);
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t id, uint32_t time, uint32_t key, uint32_t state);
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t id, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group);
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int rate, int delay);

static struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

/* provide keymap */
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd, uint32_t size) {

	if (keymap_fd >= 0 || format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
		return;

	keymap_fd = fd;
	keymap_size = (size_t)size;

	keymap_data = mmap(NULL, keymap_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE, fd, 0);
	if (keymap_data == (void *)-1) {

		keymap_data = NULL;
		return;
	}

	/* create xkb keymap and state */
	xkb_keymap = xkb_keymap_new_from_string(xkb_context,
						keymap_data,
						XKB_KEYMAP_FORMAT_TEXT_V1,
						XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!xkb_keymap) return;

	xkb_state = xkb_state_new(xkb_keymap);
	if (!xkb_state) return;
}

/* enter focus */
static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t id, struct wl_surface *surface, struct wl_array *keys) {
}

/* leave focus */
static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t id, struct wl_surface *surface) {
}

/* key press or release */
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t id, uint32_t time, uint32_t key, uint32_t state) {

	if (!xkb_keymap || !xkb_state) return;

	xkb_keysym_t keysym = xkb_state_key_get_one_sym(xkb_state, (xkb_keycode_t)key + 8);

	if (keysym == XKB_KEY_Escape)
		running = false;
}

/* modifiers */
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t id, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
}

/* repeat rate info */
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int rate, int delay) {
}

/* frame callback listener */
static void frame_done(void *data, struct wl_callback *callback, uint32_t time);

static struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

/* frame ready to be drawn */
static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {

	frame_ready = true;
}

/* resize shared memory file */
static int resize_shm(void) {

	size_t old_size = shm_size;
	shm_size = width * height * 4;

	if (ftruncate(shm_fd, (off_t)shm_size) < 0) {

		perror("ftruncate");
		return -1;
	}

	if (shm_data) munmap(shm_data, old_size);
	shm_data = mmap(NULL, shm_size, PROT_READ | PROT_WRITE,
			MAP_SHARED, shm_fd, 0);
	if (shm_data == (void *)-1) {

		shm_data = NULL;
		perror("mmap");
		return -1;
	}
	return 0;
}

/* dispatch pending messages */
static void dispatch_messages(void) {

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(display_fd, &rfds);

	struct timeval timeout = {
		.tv_sec = 0,
		.tv_usec = 0,
	};
	if (select(display_fd+1, &rfds, NULL, NULL, &timeout) < 0)
		return;

	if (FD_ISSET(display_fd, &rfds))
		wl_display_dispatch(wl_display);
	else wl_display_dispatch_pending(wl_display);
}

/* update frame callback */
static int update_frame(void) {

	if (wl_callback_frame) wl_callback_destroy(wl_callback_frame);

	wl_callback_frame = wl_surface_frame(wl_surface);
	if (!wl_callback_frame) {

		fprintf(stderr, "Can't get Wayland surface frame callback");
		return -1;
	}
	wl_callback_add_listener(wl_callback_frame, &frame_listener, NULL);
	wl_surface_commit(wl_surface);

	wl_display_flush(wl_display);
	return 0;
}

/* draw frame */
static void draw_frame(void) {

	memset(shm_data, 0xff, shm_size);
}

/* run application */
static int run(void) {

#ifdef BAKEY_RC
	if (load_config() < 0)
		return -1;
#endif

	setlocale(LC_ALL, bakey_wl_config.locale);

	struct sigaction action;

	action.sa_handler = sigh_alrm;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	action.sa_restorer = NULL;

	if (sigaction(SIGALRM, &action, NULL)) {

		perror("sigaction");
		return 1;
	}

	/* connect to wayland server */
	width = bakey_wl_config.width;
	height = bakey_wl_config.height;

	wl_display = wl_display_connect(NULL);
	if (!wl_display) {

		fprintf(stderr, "Can't connect to Wayland display\n");
		return 1;
	}
	display_fd = wl_display_get_fd(wl_display);

	/* register interfaces */
	wl_registry = wl_display_get_registry(wl_display);
	wl_registry_add_listener(wl_registry, &registry_listener, NULL);

	wl_display_dispatch(wl_display);

	if (!wl_compositor || !xdg_wm_base || !wl_shm || !wl_seat) {

		fprintf(stderr, "Can't register required Wayland interfaces\n");
		return 1;
	}

	/* create surface */
	wl_surface = wl_compositor_create_surface(wl_compositor);
	if (!wl_surface) {

		fprintf(stderr, "Can't create Wayland compositor surface\n");
		return 1;
	}

	xdg_surface = xdg_wm_base_get_xdg_surface(xdg_wm_base, wl_surface);
	if (!xdg_surface) {

		fprintf(stderr, "Can't get XDG surface\n");
		return 1;
	}

	xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
	if (!xdg_toplevel) {

		fprintf(stderr, "Can't get XDG toplevel\n");
		return 1;
	}

	wl_surface_commit(wl_surface);

	xdg_surface_add_listener(xdg_surface, &surface_listener, NULL);
	wl_display_dispatch(wl_display);

	/* open shared memory */
	srand((unsigned int)time(NULL));

	char name[32] = "/";
	for (size_t i = 0; i < sizeof(name)-1; i++)
		name[i] = (char)(rand() % 26 + 'A');
	name[sizeof(name)-1] = 0;

	shm_fd = shm_open(name, O_RDWR | O_EXCL | O_CREAT, 0600);
	if (shm_fd < 0) {

		perror("shm_open");
		return 1;
	}
	shm_unlink(name);

	if (resize_shm() < 0)
		return 1;

	/* get peripherals */
	wl_keyboard = wl_seat_get_keyboard(wl_seat);
	if (!wl_keyboard) {

		fprintf(stderr, "Can't get Wayland keyboard");
		return 1;
	}

	xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkb_context) {

		fprintf(stderr, "Can't create XKB context");
		return 1;
	}

	wl_keyboard_add_listener(wl_keyboard, &keyboard_listener, NULL);

	if (update_frame() < 0) return 1;

	/* initialize bakey */
	display.width = width / fwidth;
	display.height = height / fheight;
	display.cells = malloc(sizeof(bakey_cell_t) * display.width * display.height);

	if (bakey_open(&context) != BAKEY_RESULT_SUCCESS) {

		fprintf(stderr, "Bakey: %s\n", bakey_get_error());
		return 1;
	}

	/* set frame timer */
	struct itimerval it = {
		.it_interval = {
			.tv_sec = 0,
			.tv_usec = 1000000 / FPS_CAP,
		},
		.it_value = {
			.tv_sec = 0,
			.tv_usec = 1000000 / FPS_CAP,
		},
	};
	setitimer(ITIMER_REAL, &it, NULL);

	/* main loop */
	while (running) {

		/* update terminal attributes */
		struct termios attr;
		if (tcgetattr(pty, &attr) >= 0) {

			if (attr.c_lflag & ECHO)
				context.control.flags |= BAKEY_CONTROL_FLAG_ECHO;
			else context.control.flags &= ~BAKEY_CONTROL_FLAG_ECHO;

			if (attr.c_lflag & ICANON)
				context.control.flags |= BAKEY_CONTROL_FLAG_CANONICAL;
			else context.control.flags &= ~BAKEY_CONTROL_FLAG_CANONICAL;

			if (attr.c_lflag & ISIG)
				context.control.flags |= BAKEY_CONTROL_FLAG_SIGNAL;
			else context.control.flags &= ~BAKEY_CONTROL_FLAG_SIGNAL;

			context.control.cc[BAKEY_CONTROL_CHARACTER_MINIMUM] = (long)attr.c_cc[VMIN];
			context.control.cc[BAKEY_CONTROL_CHARACTER_INTERRUPT] = (long)attr.c_cc[VINTR];
			context.control.cc[BAKEY_CONTROL_CHARACTER_QUIT] = (long)attr.c_cc[VQUIT];
			context.control.cc[BAKEY_CONTROL_CHARACTER_SUSPEND] = (long)attr.c_cc[VSUSP];
		}

		/* update terminal */
		if (bakey_update(&context) != BAKEY_RESULT_SUCCESS)
			return 1;

		if (context.display_updated)
			display_updated = true;

		if (draw) {

			dispatch_messages();

			/* create shared memory pool and buffer */
			if (!wl_shm_pool) {

				wl_shm_pool = wl_shm_create_pool(wl_shm, shm_fd, shm_size);
				if (!wl_shm_pool) {

					fprintf(stderr, "Can't create Wayland shared memory pool\n");
					return 1;
				}
			}

			if (!wl_buffer_shm) {

				int stride = (int)width * 4;
				wl_buffer_shm = wl_shm_pool_create_buffer(
						wl_shm_pool,
						0,
						(int)width,
						(int)height,
						stride,
						WL_SHM_FORMAT_XRGB8888
						);
				if (!wl_buffer_shm) {

					fprintf(stderr, "Can't create Wayland shared memory buffer\n");
					return 1;
				}
			}

			/* draw frame */
			if (display_updated && frame_ready) {

				draw_frame();

				wl_surface_attach(wl_surface, wl_buffer_shm, 0, 0);
				wl_surface_damage(wl_surface, 0, 0, (int)width, (int)height);
				wl_surface_commit(wl_surface);

				if (update_frame() < 0)
					return 1;
				frame_ready = false;
				display_updated = false;
			}
		}
	}
	return 0;
}

/* clean up resources */
static void cleanup(void) {

	if (shell_pid >= 0) kill(shell_pid, SIGKILL);
	if (context.init) bakey_close(&context);
	if (xkb_state) xkb_state_unref(xkb_state);
	if (xkb_keymap) xkb_keymap_unref(xkb_keymap);
	if (keymap_data) munmap(keymap_data, keymap_size);
	if (xkb_context) xkb_context_unref(xkb_context);
	if (shm_data) munmap(shm_data, shm_size);
	if (shm_fd >= 0) close(shm_fd);
	if (wl_display) wl_display_disconnect(wl_display);
}

int main() {

	int code = run();
	cleanup();
	return code;
}
