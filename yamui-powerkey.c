/*
 * Power key handler. Waits for all event devices providing KEY_POWER events.
 * Exits on power key pressed for desired time or after receiving of SIGTERM.
 * Returns:
 *   0 - Power key was pressed,
 *   1 - signal was received,
 *   2 - error.
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
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>

#include <linux/input.h>

/*#define DEBUG*/
#include "yamui-tools.h"

#define NBITS(x)		((((x) - 1) / __BITS_PER_LONG) + 1)
#define BIT(arr, bit)		((arr[(bit) / __BITS_PER_LONG] >> \
				 ((bit) % __BITS_PER_LONG)) & 1)

#define MAX_DEVICES		256
#define DEFAULT_DURATION	3 /* seconds */

/* EXIT_SUCCESS and EXIT_FAILURE are defined in <stdlib.h>. */
#define EXIT_SIGNAL		2

const char *app_name = "powerkey";
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

	if (BIT(bits[0], EV_KEY)) {
		if (ioctl(fd, EVIOCGBIT(EV_KEY, KEY_MAX), bits[EV_KEY]) == -1)
			errorf("ioctl(, EVIOCGBIT(EV_KEY, ), ) error on event"
			       " device %s", name);
		else if (BIT(bits[EV_KEY], KEY_POWER)) {
			debugf("Device %s supports needed key events.", name);
			return 0;
		}
	}

	debugf("Skipping unsupported device %s.", name);
	return -1;
}

/* ------------------------------------------------------------------------ */

static int duration = DEFAULT_DURATION;
static struct timeval key_tv;

typedef enum {
	key_up,
	key_down,
	key_long_press
} key_state_t;

static key_state_t power_key_state = key_up;

static struct timeval *
get_timeout_value(void)
{
	if (power_key_state ==  key_down)
		return &key_tv;
	else /* key_up or key_long_press */
		return NULL; /* Wait forever */
}

/* ------------------------------------------------------------------------ */

static void
set_timeout_value(int sec)
{
	key_tv.tv_sec  = sec;
	key_tv.tv_usec = 0;
}

/* ------------------------------------------------------------------------ */

static void
reset_timeout_value(void)
{
	key_tv.tv_sec  = duration;
	key_tv.tv_usec = 0;
}

/* ------------------------------------------------------------------------ */

typedef enum {
	key_ev_up,
	key_ev_down
} key_ev_t; /* Why <linux/input.h> still doesn't define it for us? */

/* Returns:
 * ret_success	- Power key was pressed, terminate main loop.
 * ret_continue	- Some other key was pressed or released, continue main loop.
 */
static ret_t
handle_event(const struct input_event *ev)
{
	if (ev->type != EV_KEY || ev->code != KEY_POWER) {
		/* We are not recalculating timeout value in case of
		 * "interrupted" key_down state because select() properly
		 * updates timeout value on return. This behavior of select()
		 * is Linux-specific, and on other platforms you have to
		 * recalculate timeout value by your own. */
		return ret_continue; /* Ignore other events and keys */
	}

	if (power_key_state == key_up) {
		if (ev->value == key_ev_down) {
			debugf("New state: key_down");
			power_key_state = key_down;
			reset_timeout_value();
		} /* Else key_ev_up.
		   * This can happen with multiple Power keys.
		   * Ignore and keep timeout unchanged. */
	} else if (power_key_state == key_down) {
		if (ev->value == key_ev_up) {
			debugf("New state: key_up");
			power_key_state = key_up;
		} /* Else key_ev_down.
		   * This can happen with multiple Power keys. */
	} else { /* key_long_press */
		if (ev->value == key_ev_up)
			return ret_success;
		/* Else key_ev_down.
		 * This can happen with multiple Power keys. */
	}

	return ret_continue;
}

/* ------------------------------------------------------------------------ */

bool wait_key_up = false; /* Wait for key release event. */

/* Returns:
 * ret_success	- Power key was pressed, terminate main loop.
 * ret_continue	- Some other key was pressed or released, continue main loop.
 * ret_failure	- Error happens, terminate the application. */
static ret_t
handle_timeout(void)
{
	if (power_key_state != key_down) { /* Should't be here. */
		infof("Internal error: timeout in unexpected state: %d.\n",
		      power_key_state);
		return ret_failure;
	}

	if (!wait_key_up)
		return ret_success;

	debugf("New state: key_long_press");
	power_key_state = key_long_press;
	return ret_continue;
}

/* ------------------------------------------------------------------------ */

static void
signal_handler(int sig UNUSED)
{
	running = 0;
}

/* ------------------------------------------------------------------------ */

static void
usage(void)
{
	printf("Usage: yamui-%s [-d <key-press-duration>] [-u]\n", app_name);
	printf("-d <key-press-duration>\tThe Power key press period "
	       "in seconds before exit,\n");
	printf("\t\t\tdefault value: %d seconds\n", DEFAULT_DURATION);
	printf("-u\t\t\tExit on the key release event\n\n");
	printf("Return status:\n");
	printf("%d - Power key was pressed,\n", EXIT_SUCCESS);
	printf("%d - error happens,\n", EXIT_FAILURE);
	printf("%d - signal received.\n", EXIT_SIGNAL);
}

/* ------------------------------------------------------------------------ */

int
main(int argc, char *argv[])
{
	int opt, fds[MAX_DEVICES], num_fds = 0, ret = EXIT_SIGNAL;

	while ((opt = getopt(argc, argv, "d:hu")) != -1) {
		switch (opt) {
		case 'd':
			duration = atoi(optarg);
			if ((duration = atoi(optarg)) < 1) {
				printf("Duration value must be positive.\n");
				usage();
				return EXIT_FAILURE;
			}

			break;
		case 'u':
			wait_key_up = true;
			break;
		case 'h':
		default:
			usage();
			return EXIT_FAILURE;
		}
	}

	if (optind < argc) {
		usage();
		return EXIT_FAILURE;
	}

	if (open_fds(fds, &num_fds, MAX_DEVICES, check_device_type) == -1)
		return EXIT_FAILURE;

	debugf("Started");
	signal(SIGINT,  signal_handler);
	signal(SIGTERM, signal_handler);
	set_timeout_value(duration);

	while (running) { /* Main loop */
		int i, rv, max_fd = 0;
		fd_set rfds;
		struct timeval *tv;

		FD_ZERO(&rfds);
		for (i = 0; i < num_fds; i++) {
			FD_SET(fds[i], &rfds);
			if (fds[i] > max_fd)
				max_fd = fds[i];
		}

		tv = get_timeout_value();
		rv = select(max_fd + 1, &rfds, NULL, NULL, tv);
		if (rv > 0) {
			for (i = 0; i < num_fds; i++)
				if (FD_ISSET(fds[i], &rfds)) {
					ret_t r;

					r = handle_events(fds[i],
							  handle_event);
					if (r == ret_continue)
						continue;

					ret = get_exit_status(r);
					running = 0;
					break;
				}
		} else if (rv == 0) { /* Timeout */
			ret_t r;

			if ((r = handle_timeout()) == ret_continue)
				continue;

			ret = get_exit_status(r);
			break;
		} else { /* Error or signal */
			if (errno != EINTR) {
				errorf("Error on select()");
				ret = EXIT_FAILURE;
			}

			break;
		}
	}

	close_fds(fds, num_fds);
	debugf("Terminated");
	return ret;
}
