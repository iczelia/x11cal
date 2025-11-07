#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>

#define BUS_NAME   "net.iczelia.K16BrightD"
#define OBJ_PATH   "/net/iczelia/K16BrightD"
#define IFACE_NAME "net.iczelia.K16BrightD"

static volatile sig_atomic_t running = 1;

static void on_signal(int sig) {
  (void)sig;
  running = 0;
}

static int write_string_to_file(const char *path, const char *s) {
  FILE *f = fopen(path, "w");
  if (!f) return -errno;
  int rc = fputs(s, f) < 0 ? -errno : 0;
  int rc2 = fflush(f);
  if (rc == 0 && rc2 != 0) rc = -errno;
  fclose(f);
  return rc;
}

static int write_int_to_file(const char *path, int v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d\n", v);
  return write_string_to_file(path, buf);
}

static dbus_bool_t send_error(DBusConnection *conn, DBusMessage *msg, const char *name, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[256];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  DBusMessage *err = dbus_message_new_error(msg, name, buf);
  if (!err) return FALSE;
  dbus_bool_t ok = dbus_connection_send(conn, err, NULL);
  dbus_message_unref(err);
  dbus_connection_flush(conn);
  return ok;
}

static dbus_bool_t send_empty_reply(DBusConnection *conn, DBusMessage *msg) {
  DBusMessage *reply = dbus_message_new_method_return(msg);
  if (!reply) return FALSE;
  dbus_bool_t ok = dbus_connection_send(conn, reply, NULL);
  dbus_message_unref(reply);
  dbus_connection_flush(conn);
  return ok;
}

static int is_valid_governor(const char *gov) {
  if (!gov) return 0;
  size_t n = strlen(gov);
  if (n == 0 || n > 32) return 0;
  for (size_t i = 0; i < n; ++i) {
    char c = gov[i];
    if (!(c=='_' || (c>='a'&&c<='z') || (c>='A'&&c<='Z'))) return 0;
  }
  return 1;
}

static int path_exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

static dbus_bool_t handle_set_governor(DBusConnection *conn, DBusMessage *msg) {
  DBusError err; dbus_error_init(&err);
  int32_t cpu = -1;
  const char *gov = NULL;

  if (!dbus_message_get_args(msg, &err,
                 DBUS_TYPE_INT32, &cpu,
                 DBUS_TYPE_STRING, &gov,
                 DBUS_TYPE_INVALID)) {
    dbus_error_free(&err);
    return send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Invalid args");
  }

  if (cpu < 0 || cpu > 4096) {
    return send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "cpu out of range");
  }
  if (!is_valid_governor(gov)) {
    return send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "invalid governor");
  }

  char path[256];
  snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
  if (!path_exists(path)) {
    return send_error(conn, msg, DBUS_ERROR_FAILED, "path not found: %s", path);
  }

  int rc = write_string_to_file(path, gov);
  if (rc < 0) {
    return send_error(conn, msg, DBUS_ERROR_FAILED, "write failed: %s (%d)", strerror(-rc), rc);
  }

  return send_empty_reply(conn, msg);
}

static int is_valid_backlight_name(const char *name) {
  if (!name) return 0;
  size_t n = strlen(name);
  if (n == 0 || n > 64) return 0;
  for (size_t i = 0; i < n; ++i) {
    char c = name[i];
    if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return 0;
  }
  return 1;
}

static dbus_bool_t handle_set_brightness(DBusConnection *conn, DBusMessage *msg) {
  DBusError err; dbus_error_init(&err);
  const char *bl = NULL; int32_t value = -1;

  if (!dbus_message_get_args(msg, &err,
                 DBUS_TYPE_STRING, &bl,
                 DBUS_TYPE_INT32, &value,
                 DBUS_TYPE_INVALID)) {
    dbus_error_free(&err);
    return send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "Invalid args");
  }

  if (!is_valid_backlight_name(bl)) {
    return send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "invalid backlight name");
  }

  char base[256];
  snprintf(base, sizeof(base), "/sys/class/backlight/%s", bl);
  if (!path_exists(base)) {
    return send_error(conn, msg, DBUS_ERROR_FAILED, "backlight not found: %s", bl);
  }

  char maxp[300]; snprintf(maxp, sizeof(maxp), "%s/max_brightness", base);
  FILE *f = fopen(maxp, "r");
  if (!f) {
    return send_error(conn, msg, DBUS_ERROR_FAILED, "cannot read max_brightness");
  }
  int maxv = 0; if (fscanf(f, "%d", &maxv) != 1) maxv = 0; fclose(f);

  if (value < 0 || (maxv > 0 && value > maxv)) {
    return send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "brightness out of range 0..%d", maxv);
  }

  char bp[300]; snprintf(bp, sizeof(bp), "%s/brightness", base);
  int rc = write_int_to_file(bp, value);
  if (rc < 0) {
    return send_error(conn, msg, DBUS_ERROR_FAILED, "write failed: %s (%d)", strerror(-rc), rc);
  }

  return send_empty_reply(conn, msg);
}

static DBusHandlerResult msg_handler(DBusConnection *conn, DBusMessage *msg, void *user_data) {
  (void)user_data;
  if (dbus_message_is_method_call(msg, IFACE_NAME, "SetGovernor")) {
    if (!handle_set_governor(conn, msg)) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  if (dbus_message_is_method_call(msg, IFACE_NAME, "SetBrightness")) {
    if (!handle_set_brightness(conn, msg)) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusObjectPathVTable vtable = {
  .unregister_function = NULL,
  .message_function = msg_handler,
  .dbus_internal_pad1 = NULL,
  .dbus_internal_pad2 = NULL,
  .dbus_internal_pad3 = NULL,
  .dbus_internal_pad4 = NULL
};

int main(void) {
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  DBusError err; dbus_error_init(&err);

  DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
  if (!conn) {
    fprintf(stderr, "Failed to connect to system bus: %s\n", err.message);
    dbus_error_free(&err);
    return 1;
  }

  int req = dbus_bus_request_name(conn, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
  if (dbus_error_is_set(&err)) {
    fprintf(stderr, "Name error: %s\n", err.message);
    dbus_error_free(&err);
    return 1;
  }
  if (req != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    fprintf(stderr, "Bus name already taken\n");
    return 1;
  }

  if (!dbus_connection_register_object_path(conn, OBJ_PATH, &vtable, NULL)) {
    fprintf(stderr, "Failed to register object path\n");
    return 1;
  }

  while (running && dbus_connection_read_write_dispatch(conn, -1)) { }

  dbus_connection_unregister_object_path(conn, OBJ_PATH);
  return 0;
}