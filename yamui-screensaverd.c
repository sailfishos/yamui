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
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>

#include <linux/input.h>

/*#define DEBUG*/
#include "yamui-tools.h"
#include "minui/minui.h"

#define NBITS(x)		((((x) - 1) / __BITS_PER_LONG) + 1)
#define BIT(arr, bit)		((arr[(bit) / __BITS_PER_LONG] >> \
				 ((bit) % __BITS_PER_LONG)) & 1)

#define DISPLAY_CONTROL		"/sys/class/graphics/fb0/blank"
#define MAX_DEVICES		256
#define DISPLAY_OFF_TIME	25 /* seconds */

const char *app_name = "screensaverd";
sig_atomic_t volatile running = 1;

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
	int ret;

	if (display_state == state_on)
		return 0;

	debugf("Turning display on.");
	display_state = state_on;
	ret = sysfs_write_int(DISPLAY_CONTROL, 0);
#ifdef __arm__
	gr_restore(); /* Qualcomm specific. TODO: implement generic solution. */
#endif /* __arm__ */
	return ret;
}

/* ------------------------------------------------------------------------ */

static int
turn_display_off(void)
{
	if (display_state == state_off)
		return 0;

	debugf("Turning display off.");
	display_state = state_off;
#ifdef __arm__
	gr_save(); /* Qualcomm specific. TODO: implement generic solution. */
#endif /* __arm__ */
	return sysfs_write_int(DISPLAY_CONTROL, 1);
}

/* ------------------------------------------------------------------------ */

static void
signal_handler(int sig UNUSED)
{
	running = 0;
}

/* ------------------------------------------------------------------------ */

int
main(void)
{
	int fds[MAX_DEVICES], num_fds = 0, ret = EXIT_SUCCESS;

	if (open_fds(fds, &num_fds, MAX_DEVICES, check_device_type) == -1)
		return EXIT_FAILURE;

#ifdef __arm__
	/* Qualcomm specific. TODO: implement generic solution. */
	if (gr_init(false)) {
		errorf("Failed gr_init().\n");
		close_fds(fds, num_fds);
		return EXIT_FAILURE;
	}
#endif /* __arm__ */

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
				if (FD_ISSET(fds[i], &rfds)) {
					ret_t r;

					r = handle_events(fds[i], NULL);
					if (r == ret_continue)
						continue;

					ret = get_exit_status(r);
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
#ifdef __arm__
	gr_exit(); /* Qualcomm specific. TODO: implement generic solution. */
#endif /* __arm__ */
	close_fds(fds, num_fds);
	debugf("Terminated");
	return ret;
}
