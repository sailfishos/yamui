/*
 * Simple screen saver daemon. Turns off the display after idle timeout.
 * Turns the display on on any event from /dev/input/event* files.
 * On exit turns the display on.
 *
 * Copyright (C) 2015 Jolla Ltd.
 * Contact: Igor Zhbanov <igor.zhbanov@jolla.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _BSD_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>

#define UNUSED			__attribute__((unused))
#define NBITS(x)		((((x) - 1) / __BITS_PER_LONG) + 1)
#define BIT(arr, bit)		((arr[(bit) / __BITS_PER_LONG] >> \
				 ((bit) % __BITS_PER_LONG)) & 1)

/*#define DEBUG*/

#define DISPLAY_CONTROL		"/sys/class/graphics/fb0/blank"
#define DEV_INPUT_DIR		"/dev/input"
#define LOG_NAME		"screensaverd"
#define EVENT_PREFIX		"event"
#define MAX_DEVICES		256
#define DISPLAY_OFF_TIME	25 /* seconds */
#define EVENTS_BUF_SIZE		512 /* events */
#define EXIT_SUCCESS		0
#define EXIT_FAILURE		1

sig_atomic_t volatile running = 1;

/* ------------------------------------------------------------------------ */

static void
print_common(const char *format, va_list ap)
{
	printf("[%s] ", LOG_NAME);
	vprintf(format, ap);
}

/* ------------------------------------------------------------------------ */

static void
infof(const char *format, ...) __attribute__ ((format (printf, 1, 2)));

/* Info printing with printf-style format and application name prefix. */
static void
infof(const char *format, ...)
{
	va_list ap;
	int old_errno = errno;

	va_start(ap, format);
	print_common(format, ap);
	printf("\n");
	va_end(ap);
	errno = old_errno; /* Just in case. */
}

/* ------------------------------------------------------------------------ */

#ifdef DEBUG

#define debugf	infof

#else /* !DEBUG */

static void
debugf(const char *format, ...) __attribute__ ((format (printf, 1, 2)))
				UNUSED;

static __inline__ void
debugf(const char *format UNUSED, ...)
{

}

#endif /* !DEBUG */

/* ------------------------------------------------------------------------ */

static void
errorf(const char *format, ...) __attribute__ ((format (printf, 1, 2)));

/* perror() with printf-style format and application name prefix. */
static void
errorf(const char *format, ...)
{
	va_list ap;
	int old_errno = errno;

	va_start(ap, format);
	print_common(format, ap);
	printf(": %s\n", strerror(old_errno));
	va_end(ap);
	errno = old_errno; /* Just in case. */
}

/* ------------------------------------------------------------------------ */

/* Check for input device type. Returns 0 if button or touchscreen. */
static int
check_device_type(int fd, const char *name)
{
	unsigned long bits[EV_MAX][NBITS(KEY_MAX)];

	memset(bits, '\0', sizeof(bits));
	if (ioctl(fd, EVIOCGBIT(0, EV_MAX), bits[0]) == -1) {
		errorf("ioctl(, EVIOCGBIT(0, ), ) error on event device %s",
		       name);
		return -1;
	}

	if (BIT(bits[0], EV_ABS)) {
		if (ioctl(fd, EVIOCGBIT(EV_ABS, KEY_MAX), bits[EV_ABS]) == -1)
			errorf("ioctl(, EVIOCGBIT(EV_ABS, ), ) error on event"
			       " device %s", name);
		else if (BIT(bits[EV_ABS], ABS_MT_POSITION_X) &&
			 BIT(bits[EV_ABS], ABS_MT_POSITION_Y)) {
			debugf("Device %s supports multi-touch events.", name);
			return 0;
		}
	}

	if (BIT(bits[0], EV_KEY)) {
		if (ioctl(fd, EVIOCGBIT(EV_KEY, KEY_MAX), bits[EV_KEY]) == -1)
			errorf("ioctl(, EVIOCGBIT(EV_KEY, ), ) error on event"
			       " device %s", name);
		else if (BIT(bits[EV_KEY], KEY_POWER)		||
			 BIT(bits[EV_KEY], KEY_VOLUMEDOWN)	||
			 BIT(bits[EV_KEY], KEY_VOLUMEUP)	||
			 BIT(bits[EV_KEY], KEY_OK)		||
			 BIT(bits[EV_KEY], KEY_ENTER)) {
			debugf("Device %s supports needed key events.", name);
			return 0;
		}
	}

	debugf("Skipping unsupported device %s.", name);
	return -1;
}

/* ------------------------------------------------------------------------ */

static void
close_fds(int fds[], int num)
{
	int i;

	for (i = 0; i < num; i++)
		close(fds[i]);
}

/* ------------------------------------------------------------------------ */

/* Open all /dev/input/event* files. */
static int
open_fds(int fds[], int *num, int max_num)
{
	DIR *dir;
	struct dirent *d;

	if (!(dir = opendir(DEV_INPUT_DIR))) {
		errorf("Can't open directory %s", DEV_INPUT_DIR);
		return -1;
	}

	while ((d = readdir(dir))) {
		char name[256];

		if (strncmp(d->d_name, EVENT_PREFIX, strlen(EVENT_PREFIX)))
			continue; /* Not /dev/input/event* file */

		snprintf(name, sizeof(name), "%s/%s", DEV_INPUT_DIR,
			 d->d_name);
		debugf("Processing input ivents file %s", name);
		if ((fds[*num] = open(name, O_RDONLY)) == -1) {
			errorf("Can't open input device %s", name);
			continue;
		}

		if (check_device_type(fds[*num], name) == -1) {
			close(fds[*num]);
			continue;
		}

		if (++*num >= max_num)
			break;
	}

	closedir(dir);
	if (!*num) {
		infof("No suitable input events devices found.");
		return -1;
	}

	return 0;
}

/* ------------------------------------------------------------------------ */

static int
sysfs_write_int(const char *fname, int val)
{
	FILE *f;

	if (!(f = fopen(fname, "w"))) {
		errorf("Can't open \"%s\" for writing", fname);
		return -1;
	}

	fprintf(f, "%d\n", val);
	fclose(f);
	return 0;
}

/* ------------------------------------------------------------------------ */

typedef enum {
	state_unknown = -1,
	state_off,
	state_on
} display_state_t;

static display_state_t display_state = state_unknown;

static int
turn_display_on(void)
{
	if (display_state == state_on)
		return 0;

	debugf("Turning display on.");
	display_state = state_on;
	return sysfs_write_int(DISPLAY_CONTROL, 0);
}

/* ------------------------------------------------------------------------ */

static int
turn_display_off(void)
{
	if (display_state == state_off)
		return 0;

	debugf("Turning display off.");
	display_state = state_off;
	return sysfs_write_int(DISPLAY_CONTROL, 1);
}

/* ------------------------------------------------------------------------ */

static int
handle_events(int fd)
{
	int rv;
	struct input_event buf[EVENTS_BUF_SIZE];

	/* Read and ignore event data if OK. */
	rv = read(fd, (void *)buf, sizeof(buf));
	if (rv < 0) {
		errorf("Error on read");
		return -1;
	} else if (rv == 0) {
		infof("Unexpected EOF on read");
		return -1;
	}

	return 0;
}

/* ------------------------------------------------------------------------ */

void
signal_handler(int sig UNUSED)
{
	running = 0;
}

/* ------------------------------------------------------------------------ */

int
main(void)
{
	int fds[MAX_DEVICES], num_fds = 0, ret = EXIT_SUCCESS;

	if (open_fds(fds, &num_fds, MAX_DEVICES) == -1)
		return EXIT_FAILURE;

	debugf("Started");
	signal(SIGINT,  signal_handler);
	signal(SIGTERM, signal_handler);

	while (running) { /* Main loop */
		int i, rv, max_fd = 0;
		fd_set rfds;
		struct timeval tv;

		FD_ZERO(&rfds);
		for (i = 0; i < num_fds; i++) {
			FD_SET(fds[i], &rfds);
			if (fds[i] > max_fd)
				max_fd = fds[i];
		}

		/* Wait up to 25 seconds. */
		tv.tv_sec  = DISPLAY_OFF_TIME;
		tv.tv_usec = 0;

		rv = select(max_fd + 1, &rfds, NULL, NULL, &tv);
		if (rv > 0) {
			for (i = 0; i < num_fds; i++)
				if (FD_ISSET(fds[i], &rfds) &&
				    handle_events(fds[i]) == -1) {
					running = 0;
					break;
				}

			turn_display_on();
		} else if (rv == 0) /* Timeout */
			turn_display_off();
		else { /* Error or signal */
			if (errno != EINTR) {
				errorf("Error on select()");
				ret = EXIT_FAILURE;
			}

			break;
		}
	}

	turn_display_on();
	close_fds(fds, num_fds);
	debugf("Terminated");
	return ret;
}
