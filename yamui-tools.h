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

#ifndef _YAMUI_TOOLS_H_
#define _YAMUI_TOOLS_H_

#include <stdio.h>

#include <linux/input.h>

#define UNUSED	__attribute__((unused))

void infof(const char *format, ...)  __attribute__ ((format (printf, 1, 2)));
void errorf(const char *format, ...) __attribute__ ((format (printf, 1, 2)));

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

#define DEV_INPUT_DIR	"/dev/input"
#define EVENT_PREFIX	"event"
#define EVENTS_BUF_SIZE	512 /* events */

typedef int (*device_filter_t)(int fd, const char *name);
int open_fds(int fds[], int *num, int max_num, device_filter_t device_filter);
void close_fds(int fds[], int num);

typedef enum {
	ret_success,
	ret_failure,
	ret_continue
} ret_t;

int get_exit_status(ret_t r);

typedef ret_t (*event_handler_t)(const struct input_event *ev);
ret_t handle_events(int fd, event_handler_t event_handler);

#endif /* _YAMUI_TOOLS_H_ */
