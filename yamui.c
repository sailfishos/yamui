/*
 * Copyright (c) 2014 - 2023 Jolla Ltd.
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

#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdbool.h>
#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>

#include <sys/signalfd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <gio/gio.h>

#include <systemd/sd-daemon.h>

#include "os-update.h"
#include "minui/minui.h"

#define IMAGES_MAX      30

/* ========================================================================= *
 * Logging
 * ========================================================================= */

#define VERBOSE 0
#define PFIX "yamui: "

#define log_emit(TAG, FMT, ARGS...) do {\
	fprintf(stderr, PFIX TAG "%s(): " FMT "\n", __func__, ## ARGS);\
	fflush(stderr);\
} while (0)

#define log_err(  FMT, ARGS...)      log_emit("E: ", FMT, ## ARGS)

#if VERBOSE
# define log_debug(FMT, ARGS...)     log_emit("D: ", FMT, ## ARGS)
#else
# define log_debug(FMT, ARGS...)     do {} while (0)
#endif

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * DISPLAY
 * ------------------------------------------------------------------------- */

static void display_acquire            (void);
static void display_release            (void);
static bool display_is_acquired        (void);
static void display_set_updates_enabled(bool enabled);
static void display_set_blanked        (bool blanked);
static bool display_can_be_drawn       (void);

/* ------------------------------------------------------------------------- *
 * SYSTEMBUS
 * ------------------------------------------------------------------------- */

static bool systembus_is_available           (void);
static void systembus_probe_socket           (void);
static void systembus_socket_monitor_event_cb(GFileMonitor *mon, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data);
static void systembus_quit_socket_monitor    (void);
static bool systembus_init_socket_monitor    (void);

/* ------------------------------------------------------------------------- *
 * MAINLOOP
 * ------------------------------------------------------------------------- */

static void mainloop_run (void);
static void mainloop_stop(void);

/* ------------------------------------------------------------------------- *
 * SIGNALS
 * ------------------------------------------------------------------------- */

static gboolean signals_iowatch_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr);
static bool     signals_init      (void);
static void     signals_quit      (void);

/* ------------------------------------------------------------------------- *
 * UNIX_SERVER
 * ------------------------------------------------------------------------- */

static bool     unix_server_handle_client(void);
static gboolean unix_server_iowatch_cb   (GIOChannel *chn, GIOCondition cnd, gpointer aptr);
static bool     unix_server_addr         (struct sockaddr_un *sa, socklen_t *sa_len);
static bool     unix_server_init         (void);
static void     unix_server_quit         (void);

/* ------------------------------------------------------------------------- *
 * UNIX_CLIENT
 * ------------------------------------------------------------------------- */

static bool unix_client_terminate_server(void);

/* ------------------------------------------------------------------------- *
 * COMPOSITOR
 * ------------------------------------------------------------------------- */

static void      compositor_method_call_cb  (GDBusConnection *connection, const gchar *sender, const gchar *object_path, const gchar *interface_name, const gchar *method_name, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data);
static GVariant *compositor_get_property_cb (GDBusConnection *connection, const gchar *sender, const gchar *object_path, const gchar *interface_name, const gchar *property_name, GError **error, gpointer user_data);
static gboolean  compositor_set_property_cb (GDBusConnection *connection, const gchar *sender, const gchar *object_path, const gchar *interface_name, const gchar *property_name, GVariant *value, GError **error, gpointer user_data);
static void      compositor_connected_cb    (GDBusConnection *connection, const gchar *name, gpointer user_data);
static void      compositor_name_acquired_cb(GDBusConnection *connection, const gchar *name, gpointer user_data);
static void      compositor_name_lost_cb    (GDBusConnection *connection, const gchar *name, gpointer user_data);
static gboolean  compositor_connect_cb      (gpointer aptr);
static void      compositor_schedule_connect(void);
static void      compositor_cancel_connect  (void);
static void      compositor_disconnect      (void);
static bool      compositor_init            (void);
static void      compositor_quit            (void);

/* ------------------------------------------------------------------------- *
 * APP
 * ------------------------------------------------------------------------- */

static void     app_notify_systemd          (void);
static void     app_on_enable_from_dbus     (void);
static void     app_add_image               (const char *filename);
static void     app_flush_images            (void);
static void     app_draw_ui                 (void);
static void     app_draw_text               (void);
static void     app_draw_single_image_cb    (void);
static void     app_start_single_image      (void);
static void     app_draw_progress_bar_cb    (void);
static gboolean app_update_progress_bar_cb  (gpointer aptr);
static void     app_start_progress_bar      (void);
static void     app_draw_animate_images_cb  (void);
static gboolean app_update_animate_images_cb(gpointer aptr);
static void     app_start_animate_images    (void);
static gboolean app_start_cb                (gpointer aptr);
static gboolean app_stop_cb                 (gpointer aptr);
static void     app_print_short_help        (void);
static void     app_print_long_help         (void);

/* ------------------------------------------------------------------------- *
 * MAIN
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[]);

/* ========================================================================= *
 * DISPLAY
 * ========================================================================= */

static bool display_acquired = false;
static bool display_released = false;
static bool display_enabled  = false;
static bool display_blanked  = false;

/** Acquire display
 *
 * Note: Tries for real only once, and only if display_release()
 *       has not been called yet.
 */
static void
display_acquire(void)
{
	if (!display_acquired && !display_released) {
		display_acquired = true;
		if (gr_init(true) == -1) {
			log_err("gr_init() failed");
			display_release();
			mainloop_stop();
		}
		else {
			gr_color(0, 0, 0, 255);
			gr_clear();
		}
	}
}

/** Release display
 *
 * Note: Blocks future display_acquire() calls
 */
static void
display_release(void)
{
	if (!display_released) {
		display_released = true;
		freeLogo();
		gr_exit();
	}
}

/** Predicate for: app has the display
 */
static bool
display_is_acquired(void)
{
	return display_acquired && !display_released;
}

/** Allow / deny UI from being drawn
 *
 * Called based on setUpdatesEnabled() dbus method calls from mce.
 */
static void
display_set_updates_enabled(bool enabled)
{
	if (enabled)
		display_acquire();

	if (!display_is_acquired())
		enabled = false;

	if (display_enabled != enabled) {
		if ((display_enabled = enabled)) {
			display_set_blanked(false);
			app_draw_ui();
		}
		else {
			display_set_blanked(true);
		}
	}
}

/** Blank / unblank display
 *
 * Used for synchronizing display power state with
 * setUpdatesEnabled() dbus method calls from mce.
 */
static void
display_set_blanked(bool blanked)
{
	if (display_is_acquired()) {
		if (display_blanked != blanked)
			gr_fb_blank((display_blanked = blanked));
	}
}

/** Predicate for: UI can draw
 */
static bool
display_can_be_drawn(void)
{
	return display_is_acquired() && display_enabled && !display_blanked;
}

/* ========================================================================= *
 * SYSTEMBUS
 * ========================================================================= */

/** Path to D-Bus SystemBus socket */
#define SYSTEMBUS_SOCKET_PATH "/run/dbus/system_bus_socket"

static bool          systembus_socket_exists    = false;
static GFileMonitor *systembus_socket_monitor   = NULL;
static gulong        systembus_monitor_event_id = 0;

/** Predicate for: systembus connect can be attempted
 */
static bool
systembus_is_available(void)
{
	return systembus_socket_exists;
}

/** Probe and react to systembus socket availability changes
 */
static void
systembus_probe_socket(void)
{
	bool socket_exists = (access(SYSTEMBUS_SOCKET_PATH, F_OK) == 0);
	if (systembus_socket_exists != socket_exists) {
		log_debug("systembus_socket_exists: %s -> %s",
			  systembus_socket_exists ? "true" : "false",
			  socket_exists           ? "true" : "false");

		if ((systembus_socket_exists = socket_exists))
			compositor_schedule_connect();
		else
			mainloop_stop();
	}
}

/** Callback for handling systembus socket monitor events
 */
static void
systembus_socket_monitor_event_cb(GFileMonitor *mon,
				  GFile *file,
				  GFile *other_file,
				  GFileMonitorEvent event_type,
				  gpointer user_data)
{
	(void)mon;
	(void)file;
	(void)other_file;
	(void)event_type;
	(void)user_data;

	systembus_probe_socket();
}

/** Stop monitoring systembus socket
 */
static void
systembus_quit_socket_monitor(void)
{
	if (systembus_socket_monitor) {
		if (systembus_monitor_event_id) {
			g_signal_handler_disconnect(systembus_socket_monitor,
						    systembus_monitor_event_id),
				systembus_monitor_event_id = 0;
		}
		g_object_unref(systembus_socket_monitor),
			systembus_socket_monitor = NULL;
	}
}

/** Start monitoring systembus socket
 */
static bool
systembus_init_socket_monitor(void)
{
	bool               ack   = false;
	GFileMonitor      *mon   = NULL;
	GFile             *file  = NULL;
	GFileMonitorFlags  flags = G_FILE_MONITOR_WATCH_MOVES;
	GError            *err   = NULL;
	gulong             id    = 0;

	if (!(file = g_file_new_for_path(SYSTEMBUS_SOCKET_PATH))) {
		log_err("%s: failed to create file object",
			SYSTEMBUS_SOCKET_PATH);
		goto cleanup;
	}

	if (!(mon = g_file_monitor_file(file, flags, NULL, &err))) {
		log_err("%s: failed to create monitor object: %s",
			SYSTEMBUS_SOCKET_PATH, err->message);
		goto cleanup;
	}

	if (!(id = g_signal_connect(G_OBJECT(mon), "changed",
				    G_CALLBACK(systembus_socket_monitor_event_cb),
				    NULL))) {
		log_err("%s: failed to subscribe monitor sginals",
			SYSTEMBUS_SOCKET_PATH);
		goto cleanup;
	}

	systembus_socket_monitor = mon, mon = NULL;
	systembus_monitor_event_id = id, id = 0;
	ack = true;

	systembus_probe_socket();

cleanup:
	if (id)
		g_signal_handler_disconnect(mon, id);
	if (mon)
		g_object_unref(mon);
	if (file)
		g_object_unref(file);
	g_clear_error(&err);

	return ack;
}

/* ========================================================================= *
 * MAINLOOP
 * ========================================================================= */

static GMainLoop *mainloop_handle = NULL;

/** Run glib mainloop
 */
static void
mainloop_run(void)
{
	mainloop_handle = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(mainloop_handle);
	g_main_loop_unref(mainloop_handle),
		mainloop_handle = NULL;
}

/** Stop glib mainloop
 *
 * Note: If glib mainloop is not running, makes immediate exit without
 *       returning to the caller.
 */
static void
mainloop_stop(void)
{
	if (!mainloop_handle)
		_exit(EXIT_FAILURE);
	g_main_loop_quit(mainloop_handle);
}

/* ========================================================================= *
 * SIGNALS
 * ========================================================================= */

static int   signals_signal_fd  = -1;
static guint signals_iowatch_id =  0;

/** Callback for handling signalfd events
 */
static gboolean
signals_iowatch_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
	(void)chn;
	(void)cnd;
	(void)aptr;

	/* Acknowledge the signal as received */
	struct signalfd_siginfo si = {};
	if (read(signals_signal_fd, &si, sizeof si) == -1)
		log_err("Could not read signal fd: %m");
	else
		log_err("Caught signal %u: %s",
			(unsigned)si.ssi_signo, strsignal(si.ssi_signo));

	/* Request exit from mainloop */
	mainloop_stop();

	/* Remove iowatch from bookkeeping, restore default signal handlers,
	 * and tell glib that the iowatch can be removed */
	signals_iowatch_id = 0;
	signals_quit();
	return G_SOURCE_REMOVE;
}

/** Setup async signal handling
 *
 * Uses signalfd for forwarding signal processing in mainloop context.
 */
static bool
signals_init(void)
{
	bool success = false;

	int          fd  = -1;
	GIOChannel  *chn = NULL;
	guint        wid = 0;
	GIOCondition cnd = G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL;

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);

	if ((fd = signalfd(-1, &mask, 0)) == -1) {
		log_err("Could not create signal fd");
		goto cleanup;
	}

	if (!(chn = g_io_channel_unix_new(fd))) {
		log_err("Could not create signal fd io channel");
		goto cleanup;
	}

	if (!(wid = g_io_add_watch(chn, cnd, signals_iowatch_cb, NULL))) {
		log_err("Could not create add signal fd io watch");
		goto cleanup;
	}

	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
		log_err("Could not block signals");
		goto cleanup;
	}

	signals_signal_fd = fd, fd = -1;
	signals_iowatch_id = wid, wid = 0;
	success = true;
cleanup:
	if (wid)
		g_source_remove(wid);
	if (chn)
		g_io_channel_unref(chn);
	if (fd != -1)
		close(fd);

	return success;
}

/** Restore default signal handling
 */
static void
signals_quit(void)
{
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);

	if (sigprocmask(SIG_UNBLOCK, &mask, NULL) == -1)
		log_err("Could not unblock signals");

	if (signals_iowatch_id) {
		g_source_remove(signals_iowatch_id),
			signals_iowatch_id = 0;
	}

	if (signals_signal_fd != -1) {
		close(signals_signal_fd),
			signals_signal_fd = -1;
	}
}

/* ========================================================================= *
 * UNIX_SERVER
 * ========================================================================= */

static const char unix_server_path[]     = "@yamuisplash";
static int        unix_server_socket_fd  = -1;
static guint      unix_server_iowatch_id = 0;
static int        unix_server_client_fd  = -1;

/** Handle client connecting to unix socket
 */
static bool
unix_server_handle_client(void)
{
	if (unix_server_socket_fd == -1)
		goto cleanup;
	struct sockaddr_un sa = { };
	socklen_t sa_len = sizeof sa;
	int fd = accept(unix_server_socket_fd, (struct sockaddr *)&sa, &sa_len);
	if (fd == -1) {
		log_err("%s: accept(): %m", unix_server_path);
		goto cleanup;
	}
	/* What we want to happen is: client gets eof when this
	 * process is terminated. File descriptors are intentionally
	 * leaked and not explicitly closed to achieve this. */
	unix_server_client_fd = fd;
	log_debug("%s: server terminate requested", unix_server_path);
cleanup:
	return unix_server_client_fd != -1;
}

/** I/O watch callback for handling connects to server socket
 */
static gboolean
unix_server_iowatch_cb(GIOChannel *chn, GIOCondition cnd, gpointer aptr)
{
	(void)chn;
	(void)aptr;

	if (cnd & ~G_IO_IN) {
		unix_server_iowatch_id = 0;
		mainloop_stop();
		return G_SOURCE_REMOVE;
	}

	if (unix_server_handle_client())
		mainloop_stop();
	return G_SOURCE_CONTINUE;
}

static bool
unix_server_addr(struct sockaddr_un *sa, socklen_t *sa_len)
{
	socklen_t len = strnlen(unix_server_path, sizeof sa->sun_path) + 1;
	if (len > sizeof sa->sun_path) {
		log_err("%s: unix socket path too long", unix_server_path);
		return false;
	}

	memset(sa, 0, sizeof *sa);
	sa->sun_family = AF_UNIX;
	strcpy(sa->sun_path, unix_server_path);
	/* Starts with a '@' -> turn into abstract address */
	if (sa->sun_path[0] == '@')
		sa->sun_path[0] = 0;
	len += offsetof(struct sockaddr_un, sun_path);
	*sa_len = len;
	return true;
}

/** Start unix socket server
 *
 * This is used for controlled terminating of an already running splashscreen
 * application in situations where dbus systembus is not available yet and
 * thus can't be used for controlling mutually exclusive access to graphics
 * sw stack.
 */
static bool
unix_server_init(void)
{
	int                fd  = -1;
	GIOChannel        *chn = NULL;
	guint              wid = 0;
	GIOCondition       cnd = G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL;
	struct sockaddr_un sa  = {};
	socklen_t          len = 0;

	if (unix_server_iowatch_id != 0)
		goto cleanup;

	if (!unix_server_addr(&sa, &len))
		goto cleanup;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		log_err("%s: socket(): %m", unix_server_path);
		goto cleanup;
	}
	if (bind(fd, (struct sockaddr *)&sa, len) == -1) {
		log_err("%s: bind(): %m", unix_server_path);
		goto cleanup;
	}
	if (listen(fd, 1) == -1) {
		log_err("%s: listen(): %m", unix_server_path);
		goto cleanup;
	}
	if (!(chn = g_io_channel_unix_new(fd))) {
		log_err("Could not create signal fd io channel");
		goto cleanup;
	}

	if (!(wid = g_io_add_watch(chn, cnd, unix_server_iowatch_cb, NULL))) {
		log_err("Could not create add signal fd io watch");
		goto cleanup;
	}

	unix_server_socket_fd = fd, fd = -1;
	unix_server_iowatch_id = wid, wid = 0;

cleanup:
	if (wid)
		g_source_remove(wid);
	if (chn)
		g_io_channel_unref(chn);
	if (fd != -1)
		close(fd);

	return unix_server_iowatch_id != 0;
}

/** Stop unix socket server
 */
static void
unix_server_quit(void)
{
	if (unix_server_iowatch_id)
		g_source_remove(unix_server_iowatch_id), unix_server_iowatch_id = 0;

	if (unix_server_socket_fd != -1)
		close(unix_server_socket_fd), unix_server_socket_fd = -1;
}

/* ========================================================================= *
 * UNIX_CLIENT
 * ========================================================================= */

/** Terminate already running splashscreen application via unix socket ipc
 *
 * This is expected to work only when there is splashscreen application
 * running that was started before systembus became available.
 */
static bool
unix_client_terminate_server(void)
{
	bool               ack = false;
	int                fd  = -1;
	struct sockaddr_un sa  = {};
	socklen_t          len = 0;

	if (!unix_server_addr(&sa, &len))
		goto cleanup;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		log_err("%s: socket(): %m", unix_server_path);
		goto cleanup;
	}

	if (connect(fd, (struct sockaddr *)&sa, len) == -1) {
		if (errno == ECONNREFUSED)
			log_debug("%s: server not running", unix_server_path);
		else
			log_err("%s: connect(): %m", unix_server_path);
		goto cleanup;
	}

	char tmp[32];
	int rc = read(fd, tmp, sizeof tmp);

	if (rc == -1) {
		log_err("%s: read(): %m", unix_server_path);
		goto cleanup;
	}

	if (rc > 0) {
		log_err("%s: read(): got data?", unix_server_path);
		goto cleanup;
	}

	log_debug("%s: read(): got EOF", unix_server_path);
	ack = true;
cleanup:
	if (fd != -1)
		close(fd);
	return ack;
}

/* ========================================================================= *
 * COMPOSITOR
 * ========================================================================= */

/** Well known dbus name of compositor service */
#define COMPOSITOR_SERVICE                     "org.nemomobile.compositor"
#define COMPOSITOR_PATH                        "/"
#define COMPOSITOR_IFACE                       "org.nemomobile.compositor"

/** Enabling/disabling display updates via compositor service */
#define COMPOSITOR_SET_UPDATES_ENABLED         "setUpdatesEnabled"

/** Query owner of topmost ui window */
#define COMPOSITOR_GET_TOPMOST_WINDOW_PID      "privateTopmostWindowProcessId"

/** Change notification for owner of topmost ui window */
#define COMPOSITOR_TOPMOST_WINDOW_PID_CHANGED  "privateTopmostWindowProcessIdChanged"

/** Query requirements of this compositor process */
#define COMPOSITOR_GET_SETUP_ACTIONS           "privateGetSetupActions"

/** Setup actions supported by mce */
#define COMPOSITOR_ACTION_NONE                 0
#define COMPOSITOR_ACTION_STOP_HWC             (1<<0)
#define COMPOSITOR_ACTION_START_HWC            (1<<1)
#define COMPOSITOR_ACTION_RESTART_HWC          (1<<2)

/** Introspect XML - needed for setting up glib based dbus service */
static const char introspect_xml[] = ""
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
"<node>\n"
"  <interface name=\"" COMPOSITOR_IFACE "\">\n"
"    <method name=\"" COMPOSITOR_SET_UPDATES_ENABLED "\">\n"
"      <arg direction=\"in\" type=\"b\" name=\"enabled\"/>\n"
"    </method>\n"
"    <method name=\"" COMPOSITOR_GET_TOPMOST_WINDOW_PID "\">\n"
"      <arg direction=\"out\" type=\"i\" name=\"pid\"/>\n"
"    </method>\n"
"    <method name=\"" COMPOSITOR_GET_SETUP_ACTIONS "\">\n"
"      <arg direction=\"out\" type=\"u\" name=\"flags\"/>\n"
"    </method>\n"
"    <signal name=\"" COMPOSITOR_TOPMOST_WINDOW_PID_CHANGED "\">\n"
"      <arg type=\"i\" name=\"pid\"/>\n"
"    </signal>\n"
"    <method name=\"privateTopmostWindowPolicyApplicationId\">\n"
"      <arg direction=\"out\" type=\"s\" name=\"id\"/>\n"
"    </method>\n"
"    <signal name=\"privateTopmostWindowPolicyApplicationIdChanged\">\n"
"      <arg type=\"s\" name=\"id\"/>\n"
"    </signal>\n"
"  </interface>\n"
"</node>\n";

static const GDBusInterfaceVTable compositor_interface_vtable =
{
	compositor_method_call_cb,
	compositor_get_property_cb,
	compositor_set_property_cb,
};

static GDBusNodeInfo *compositor_introspect_data  = NULL;
static guint          compositor_name_owning_id   = 0;
static guint          compositor_connect_id       = 0;
static bool           compositor_name_acquired    = false;

/** Callback for handling incoming method call messages
 */
static void
compositor_method_call_cb(GDBusConnection       *connection,
			  const gchar           *sender,
			  const gchar           *object_path,
			  const gchar           *interface_name,
			  const gchar           *method_name,
			  GVariant              *parameters,
			  GDBusMethodInvocation *invocation,
			  gpointer               user_data)
{
	(void)connection;
	(void)sender;
	(void)object_path;
	(void)user_data;

	log_debug("obj: %s method: %s.%s", object_path, interface_name, method_name);

	if (!g_strcmp0(method_name, COMPOSITOR_SET_UPDATES_ENABLED)) {
		gboolean enabled = FALSE;
		g_variant_get(parameters, "(b)", &enabled);
		log_debug("enabled := %s", enabled ? "true" : "false");
		display_set_updates_enabled(enabled);
		if (enabled)
			app_on_enable_from_dbus();
		g_dbus_method_invocation_return_value(invocation, NULL);
	}
	else if (!g_strcmp0(method_name, COMPOSITOR_GET_TOPMOST_WINDOW_PID)) {
		gint pid = getpid();
		log_debug("pid == %d", pid);
		g_dbus_method_invocation_return_value(invocation,
						      g_variant_new("(i)", pid));
	}
	else if (!g_strcmp0(method_name, COMPOSITOR_GET_SETUP_ACTIONS)) {
		guint flags = COMPOSITOR_ACTION_STOP_HWC;
		log_debug("flags == 0x%x", flags);
		g_dbus_method_invocation_return_value(invocation,
						      g_variant_new("(u)", flags));
	}
	else {
		log_err("Unhandled method: %s.%s", interface_name, method_name);
		g_dbus_method_invocation_return_error(invocation,
						      G_DBUS_ERROR,
						      G_DBUS_ERROR_NOT_SUPPORTED,
						      "unknown method: %s",
						      method_name);
	}
}

/** Dummy callback for handling incoming dbus property Get method calls
 */
static GVariant *
compositor_get_property_cb(GDBusConnection  *connection,
			   const gchar      *sender,
			   const gchar      *object_path,
			   const gchar      *interface_name,
			   const gchar      *property_name,
			   GError          **error,
			   gpointer          user_data)
{
	(void)connection;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)property_name;
	(void)error;
	(void)user_data;

	GVariant *res = NULL;
	return res;
}

/** Dummy callback for handling incoming dbus property Set method calls
 */
static gboolean
compositor_set_property_cb(GDBusConnection  *connection,
			   const gchar      *sender,
			   const gchar      *object_path,
			   const gchar      *interface_name,
			   const gchar      *property_name,
			   GVariant         *value,
			   GError          **error,
			   gpointer          user_data)
{
	(void)connection;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)property_name;
	(void)value;
	(void)error;
	(void)user_data;

	gboolean res = FALSE;
	return res;
}

/** Callback for connected-to-dbus phase of compositor name owning
 */
static void
compositor_connected_cb(GDBusConnection *connection,
			const gchar     *name,
			gpointer         user_data)
{
	(void)name;
	(void)user_data;

	log_debug("bus_acquired: %p %s", connection, name);

	guint registration_id =
		g_dbus_connection_register_object(connection,
						  COMPOSITOR_PATH,
						  compositor_introspect_data->interfaces[0],
						  &compositor_interface_vtable,
						  NULL,  /* user_data */
						  NULL,  /* user_data_free_func */
						  NULL); /* GError** */
	if (!registration_id)
		mainloop_stop();
}

/** Callback for name-acquired phase of compositor name owning
 */
static void
compositor_name_acquired_cb(GDBusConnection *connection,
			    const gchar     *name,
			    gpointer         user_data)
{
	(void)connection;
	(void)name;
	(void)user_data;

	log_debug("name_acquired: %p %s", connection, name);
	compositor_name_acquired = true;
}

/** Callback for name-lost phase of compositor name owning
 */
static void
compositor_name_lost_cb(GDBusConnection *connection,
			const gchar     *name,
			gpointer         user_data)
{
	(void)name;
	(void)user_data;

	log_debug("name_lost: %p %s", connection, name);

	if (!connection) {
		log_err("dbus connection failure");
		mainloop_stop();
	}
	else if (compositor_name_acquired) {
		log_debug("service handover");
		mainloop_stop();
	}
	else {
		log_debug("waiting for name...");
	}
}

/** Idle callback for delayed handling of SystemBus available
 */
static gboolean
compositor_connect_cb(gpointer aptr)
{
	(void)aptr;

	compositor_connect_id = 0;

	if (!systembus_is_available())
		goto cleanup;

	if (!compositor_name_owning_id) {
		log_debug("dbus connect");
		GBusNameOwnerFlags flags =
			G_BUS_NAME_OWNER_FLAGS_REPLACE |
			G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
		log_debug("own name flags: 0x%x", flags);
		compositor_name_owning_id =
			g_bus_own_name(G_BUS_TYPE_SYSTEM,
				       COMPOSITOR_SERVICE,
				       flags,
				       compositor_connected_cb,
				       compositor_name_acquired_cb,
				       compositor_name_lost_cb,
				       NULL, NULL);
	}

cleanup:
	return G_SOURCE_REMOVE;
}

/** Schedule connect to systembus
 */
static void
compositor_schedule_connect(void)
{
	if (!compositor_connect_id) {
		compositor_connect_id =
			g_timeout_add(50, compositor_connect_cb, NULL);
	}
}

/** Cancel scheduled connect to systembus
 */
static void
compositor_cancel_connect(void)
{
	if (compositor_connect_id) {
		g_source_remove(compositor_connect_id),
			compositor_connect_id = 0;
	}
}

/** Disconnect from systembus
 */
static void
compositor_disconnect(void)
{
	compositor_cancel_connect();

	if (compositor_name_owning_id) {
		log_debug("dbus disconnect");
		g_bus_unown_name(compositor_name_owning_id),
			compositor_name_owning_id = 0;
	}
}

/** Initialize compositor service data
 */
static bool
compositor_init(void)
{
	bool ack = false;

	if (!compositor_introspect_data) {
		compositor_introspect_data =
			g_dbus_node_info_new_for_xml(introspect_xml, NULL);
	}
	if (!compositor_introspect_data) {
		log_err("Could not create dbus introspect data");
		goto cleanup;
	}

	ack = true;
cleanup:
	return ack;
}

/** Cleanup compositor service data
 */
static void
compositor_quit(void)
{
	compositor_disconnect();

	if (compositor_introspect_data) {
		g_dbus_node_info_unref(compositor_introspect_data),
			compositor_introspect_data = NULL;
	}
}

/* ========================================================================= *
 * APP
 * ========================================================================= */

static unsigned long int        app_animate_ms            = 0;
static unsigned long long int   app_stop_ms               = 0;
static unsigned long long int   app_progress_ms           = 0;
static char                    *app_text                  = NULL;
static gchar                   *app_images[IMAGES_MAX]    = {};
static const char              *app_images_dir            = "/res/images";;
static int                      app_image_count           = 0;
static bool                     app_already_enabled       = false;
static bool                     app_systemd_notify        = false;
static int                      app_step                  = -1;
static void                   (*app_draw_ui_cb)(void)     = NULL;

/** Notify systemd that application has started up
 *
 * Done once, if requrested via '--systemd' option
 */
static void
app_notify_systemd(void)
{
	if (app_systemd_notify) {
		app_systemd_notify = false;
		log_debug("execute systemd notify");
		sd_notify(0, "READY=1");
	}
}

/** React to setUpdatesEnabled(true) call from mce
 *
 * If app is started before systembus, updates are enabled without
 * waiting for permission from mce.
 *
 * This function is used for differentiating between that and real
 * permission to draw from mce.
 */
static void
app_on_enable_from_dbus(void)
{
	if (!app_already_enabled) {
		app_already_enabled = true;
		log_debug("enabled by mce");

		/* If running as systemd service, this is when
		 * the app can be considered as "started".
		 */
		app_notify_systemd();
	}
}

/** Locate and cache path to image file given on command line
 *
 * Tries:
 * 1) the given filename as-is
 * 2) filename in image directory
 * 3) filename in image directory with .png extension
 *
 * @param filename file name, path, or stem
 */
static void
app_add_image(const char *filename)
{
	gchar *filepath = NULL;

	/* have room for more images? */
	if (app_image_count >= IMAGES_MAX) {
		log_err("%s: ignored, too many images", filename);
		goto cleanup;
	}

	/* try: filename as-is */
	filepath = g_strdup(filename);
	if (access(filepath, R_OK) == 0)
		goto cleanup;
	if (errno != ENOENT)
		log_err("%s: access(): %m", filepath);
	g_free(filepath), filepath = NULL;

	/* try: filename in image dir */
	filepath = g_strdup_printf("%s/%s", app_images_dir, filename);
	if (filepath && access(filepath, R_OK) == 0)
		goto cleanup;
	if (errno != ENOENT)
		log_err("%s: access(): %m", filepath);
	g_free(filepath), filepath = NULL;

	/* try: filename in image dir with png extension */
	filepath = g_strdup_printf("%s/%s.png", app_images_dir, filename);
	if (filepath && access(filepath, R_OK) == 0)
		goto cleanup;
	log_err("%s: access(): %m", filepath);
	g_free(filepath), filepath = NULL;

cleanup:
	if (filepath) {
		log_debug("got image \"%s\" to display", filepath);
		app_images[app_image_count++] = filepath;
	}
}

/** Flush cached image paths
 */
static void
app_flush_images(void)
{
	while (app_image_count > 0) {
		gchar *filepath = app_images[--app_image_count];
		g_free(filepath);
	}
}

/** Hook for redrawing ui content after display unblank
 */
static void
app_draw_ui(void)
{
	if (app_draw_ui_cb)
		app_draw_ui_cb();
}

/** Draw text given as '--text' command line option
 */
static void
app_draw_text(void)
{
	if (app_text) {
		gr_color(255, 255, 255, 255);
		gr_text(20, 20, app_text, 1);
	}
}

/** Callback for drawing 'text_only' mode ui
 */
static void
app_draw_text_only_cb(void)
{
	/* Set draw on unblank hook */
	app_draw_ui_cb = app_draw_text_only_cb;

	if (display_can_be_drawn()) {
		app_draw_text();
		gr_flip();
	}
}

/** Prepare for 'text_only' mode ui
 */
static void
app_start_text_only(void)
{
	app_draw_text_only_cb();
}

/** Callback for drawing 'single_image' mode ui
 */
static void
app_draw_single_image_cb(void)
{
	/* Set draw on unblank hook */
	app_draw_ui_cb = app_draw_single_image_cb;

	if (display_can_be_drawn()) {
		app_draw_text();
		showLogo();
		gr_flip();
	}
}

/** Prepare for 'single_image' mode ui
 */
static void
app_start_single_image(void)
{
	if (loadLogo(app_images[0], NULL) == -1)
		mainloop_stop();
	else
		app_draw_single_image_cb();
}

/** Callback for drawing 'progress_bar' mode ui
 */
static void
app_draw_progress_bar_cb(void)
{
	/* Set draw on unblank hook */
	app_draw_ui_cb = app_draw_progress_bar_cb;

	if (display_can_be_drawn()) {
		app_draw_text();
		osUpdateScreenShowProgress(app_step);
		gr_flip();
	}
}

/** Timer callback for updating 'progress_bar' mode ui
 */
static gboolean
app_update_progress_bar_cb(gpointer aptr)
{
	(void)aptr;

	app_step += 1;

	if (app_step > 100) {
		mainloop_stop();
		return G_SOURCE_REMOVE;
	}

	app_draw_progress_bar_cb();
	return G_SOURCE_CONTINUE;
}

/** Prepare for 'progress_bar' mode ui
 */
static void
app_start_progress_bar(void)
{
	if (app_image_count > 0 && loadLogo(app_images[0], NULL) == -1) {
		mainloop_stop();
	}
	else {
		int period = (app_progress_ms + 101 - 1) / 101;
		log_debug("%s - period %d", __func__, period);
		g_timeout_add(period, app_update_progress_bar_cb, NULL);
		app_update_progress_bar_cb(NULL);
	}
}

/** Callback for drawing 'animation' mode ui
 */
static void
app_draw_animate_images_cb(void)
{
	/* Set draw on unblank hook */
	app_draw_ui_cb = app_draw_animate_images_cb;

	if (display_can_be_drawn()) {
		gr_color(0, 0, 0, 255);
		gr_clear();
		app_draw_text();
		showLogo();
		gr_flip();
	}
}

/** Timer callback for updating 'animation' mode ui
 */
static gboolean
app_update_animate_images_cb(gpointer aptr)
{
	(void)aptr;

	app_step += 1;
	app_step %= app_image_count;

	if (loadLogo(app_images[app_step], NULL) == -1) {
		mainloop_stop();
		return G_SOURCE_REMOVE;
	}

	app_draw_animate_images_cb();
	return G_SOURCE_CONTINUE;
}

/** Prepare for 'animation' mode ui
 */
static void
app_start_animate_images(void)
{
	int period = (app_animate_ms + app_image_count - 1)
		/ app_image_count;
	log_debug("%s - period %d", __func__, period);
	g_timeout_add(period, app_update_animate_images_cb, NULL);
	app_update_animate_images_cb(NULL);
}

/** Idle callback for continuing app startup from within mainloop
 */
static gboolean
app_start_cb(gpointer aptr)
{
	(void)aptr;

	bool success = false;

	/* Handle started-in-early-boot situation */

	if (!systembus_is_available()) {
		/* Setup unix socket service so that we can be
		 * terminated without need for dbus access.
		 */
		if (!unix_server_init())
			goto cleanup;

		/* Assume that when dbus becomes available, we
		 * will be granted permission to draw and grap
		 * display already now.
		 */
		display_set_updates_enabled(true);
	}

	/* Select what kind of ui mode to use */

	if (app_progress_ms) {
		if (app_image_count > 1) {
			log_err("Can only show one image with progressbar");
			goto cleanup;
		}
		app_start_progress_bar();
	}
	else if (app_animate_ms) {
		if (app_image_count < 2) {
			log_err("Animating requires at least 2 images");
			goto cleanup;
		}
		app_start_animate_images();
	}
	else if (app_image_count > 0) {
		app_start_single_image();
	}
	else if (app_text) {
		app_start_text_only();
	}
	else {
		log_err("Neither text nor image given");
		goto cleanup;
	}

	/* Schedule automatic application exit */

	if (app_stop_ms > 0)
		g_timeout_add(app_stop_ms, app_stop_cb, NULL);

	success = true;
cleanup:
	if (!success)
		mainloop_stop();
	return G_SOURCE_REMOVE;
}

/** Timer callback for automated application termination
 */
static gboolean
app_stop_cb(gpointer aptr)
{
	(void)aptr;

	mainloop_stop();
	return G_SOURCE_REMOVE;
}

/** Show short usage info
 */
static void
app_print_short_help(void)
{
	printf("  yamui [OPTIONS] [IMAGE(s)]\n");
}

/** Show long usage info
 */
static void
app_print_long_help(void)
{
	printf("  yamui - tool to display progress bar, logo, or small animation on UI\n");
	printf("  Usage:\n");
	app_print_short_help();
	printf("    IMAGE(s)   - png picture file names in DIR without .png extension\n");
	printf("                 NOTE: currently maximum of %d pictures supported\n",
	       IMAGES_MAX);
	printf("\n  OPTIONS:\n");
	printf("  --animate=PERIOD, -a PERIOD\n");
	printf("         Show IMAGEs (at least 2) in rotation over PERIOD ms\n");
	printf("  --imagesdir=DIR, -i DIR\n");
	printf("         Load IMAGE(s) from DIR, /res/images by default\n");
	printf("  --progressbar=TIME, -p TIME\n");
	printf("         Show a progess bar over TIME milliseconds\n");
	printf("  --stopafter=TIME, -s TIME\n");
	printf("         Stop showing the IMAGE(s) after TIME milliseconds\n");
	printf("  --text=STRING, -t STRING\n");
	printf("         Show STRING on the screen\n");
	printf("  --help, -h\n");
	printf("         Print this help\n");
	printf("  --terminate, -x\n");
	printf("         Terminate splashscreen (when dbus is not available)\n");
	printf("  --skip-cleanup, -c\n");
	printf("         Skip display cleanup at exit.\n");
}

/** Long form command line options */
static struct option opt_long[] = {
	{"animate",      required_argument, 0, 'a'},
	{"imagesdir",    required_argument, 0, 'i'},
	{"progressbar",  required_argument, 0, 'p'},
	{"stopafter",    required_argument, 0, 's'},
	{"text",         required_argument, 0, 't'},
	{"help",         no_argument,       0, 'h'},
	{"terminate",    no_argument,       0, 'x'},
	{"systemd",      no_argument,       0, 'n'},
	{"skip-cleanup", no_argument,       0, 'c'},
	{0, 0, 0, 0},
};

/** Short form command line options */
static const char opt_short[] = "a:i:p:s:t:hxnc";

/* ========================================================================= *
 * MAIN
 * ========================================================================= */

int
main(int argc, char *argv[])
{
	bool do_cleanup = true;

	setlinebuf(stdout);
	setlinebuf(stderr);

	log_debug("startup");

	for (;;) {
		int opt = getopt_long(argc, argv, opt_short, opt_long, NULL);
		if (opt == -1)
			break;

		switch (opt) {
		case 'a':
			log_debug("got animate %s ms", optarg);
			app_animate_ms = strtoul(optarg, NULL, 10);
			break;
		case 'i':
			log_debug("got imagesdir \"%s\"", optarg);
			app_images_dir = optarg;
			break;
		case 'p':
			log_debug("got progressbar %s ms", optarg);
			app_progress_ms = strtoull(optarg, NULL, 10);
			break;
		case 's':
			log_debug("got stop at %s ms", optarg);
			app_stop_ms = strtoull(optarg, NULL, 10);
			break;
		case 't':
			log_debug("got text \"%s\" to display", optarg);
			app_text = optarg;
			break;
		case 'x':
			if (!unix_client_terminate_server()) {
				log_err("Failed to terminate splashscreen");
				exit(EXIT_FAILURE);
			}
			log_debug("terminated splashscreen");
			exit(EXIT_SUCCESS);
		case 'n':
			log_debug("using systemd notify");
			app_systemd_notify = true;
			break;
		case 'c':
			log_debug("skip display cleanup");
			do_cleanup = false;
			break;
		case 'h':
			app_print_long_help();
			exit(EXIT_SUCCESS);
		default:
			log_err("getopt returned character code 0%o", opt);
			app_print_short_help();
			exit(EXIT_FAILURE);
		}
	}

	while (optind < argc)
		app_add_image(argv[optind++]);

	if (app_image_count < 1 && !app_text) {
		log_err("No text or images specified");
		app_print_short_help();
		exit(EXIT_FAILURE);
	}

	if (!compositor_init())
		goto cleanup;

	if (!systembus_init_socket_monitor())
		goto cleanup;

	unix_client_terminate_server();

	if (!signals_init())
		goto cleanup;

	if (!g_idle_add(app_start_cb, NULL))
		goto cleanup;

	mainloop_run();

cleanup:
	log_debug("cleanup");

	/* To keep any systemd unit dependencies etc on happy
	 * path: If we have been asked to notify systemd, do
	 * not exit without doing so.
	 */
	app_notify_systemd();

	/* Restore default signal handling to avoid potential
	 * surprises on exit path.
	 */
	signals_quit();

	/* If server socket is not explicitly closed, implicit
	 * linger time is applied for the address and it will not
	 * be immediately available for the next yamui instance.
	 */
	unix_server_quit();

	/* Apart from the above: assume that the rest of the
	 * cleanup is not necessary, and that skipping it might
	 * (depending on device type) leave the display powered
	 * on and showing the last frame drawn from here (and
	 * thus potentially eliminate / shorten black screen time
	 * during compositor handover).
	 */
	if (do_cleanup) {
		display_release();
		app_flush_images();
		systembus_quit_socket_monitor();
		compositor_quit();
	}

	log_debug("exit");
	return EXIT_SUCCESS;
}
