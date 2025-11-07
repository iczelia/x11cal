#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>

int call_setgov(int cpu, const char * gov) {
  DBusError err; dbus_error_init(&err);
  DBusConnection * conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
  if (!conn) { fprintf(stderr, "%s\n", err.message); return 1; }
  DBusMessage * msg = dbus_message_new_method_call("net.iczelia.K16BrightD",
    "/net/iczelia/K16BrightD",
    "net.iczelia.K16BrightD",
    "SetGovernor");
  if (!msg) return 1;
  dbus_message_append_args(msg, DBUS_TYPE_INT32, &cpu, DBUS_TYPE_STRING, &gov, DBUS_TYPE_INVALID);
  DBusMessage * reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
  dbus_message_unref(msg);
  if (!reply) { fprintf(stderr, "Error: %s\n", err.message); return 1; }
  dbus_message_unref(reply);
  return 0;
}

int main(int argc, char * argv[]) {
  if (argc != 3) { fprintf(stderr, "usage: %s CPU GOV\n", argv[0]); return 2; }
  return call_setgov(atoi(argv[1]), argv[2]);
}
