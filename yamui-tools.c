/*
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

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <linux/input.h>

#include "yamui-tools.h"

extern const char *app_name;

/* ------------------------------------------------------------------------ */

static void
print_common(const char *format, va_list ap)
{
	printf("[%s] ", app_name);
	vprintf(format, ap);
}

/* ------------------------------------------------------------------------ */

/* Info printing with printf-style format and application name prefix. */
void
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

/* perror() with printf-style format and application name prefix. */
void
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

/* Open all /dev/input/event* files. */
int
open_fds(int fds[], int *num, int max_num, device_filter_t device_filter)
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

		if (device_filter && device_filter(fds[*num], name) == -1) {
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

void
close_fds(int fds[], int num)
{
	int i;

	for (i = 0; i < num; i++)
		close(fds[i]);
}

/* ------------------------------------------------------------------------ */

/* Returns:
 * ret_success	- Power key was pressed, terminate main loop.
 * ret_continue	- Some other key was pressed or released, continue main loop.
 * ret_failure	- Error happens, terminate the application. */
ret_t
handle_events(int fd, event_handler_t event_handler)
{
	size_t i;
	ssize_t rv;
	ret_t ret;
	struct input_event buf[EVENTS_BUF_SIZE];

	/* Read and ignore event data if OK. */
	rv = read(fd, (void *)buf, sizeof(buf));
	if (rv < 0) {
		errorf("Error on read");
		return ret_failure;
	} else if (rv == 0) {
		infof("Unexpected EOF on read");
		return ret_failure;
	} else if (rv % sizeof(struct input_event)) {
		infof("Read incomplete input_event structure");
		return ret_failure;
	}

	if (event_handler)
		for (i = 0; i < rv / sizeof(struct input_event); i++)
			if ((ret = event_handler(&buf[i])) != ret_continue)
				return ret;

	return ret_continue;
}

/* ------------------------------------------------------------------------ */

/* Map functions return status to main() exit status. */
int
get_exit_status(ret_t r)
{
	return (r == ret_success) ? EXIT_SUCCESS : EXIT_FAILURE;
}
