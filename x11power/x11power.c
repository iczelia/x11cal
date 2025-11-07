
// Headers
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>

#include <ft2build.h>
#include <freetype2/freetype/freetype.h>

#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>

#include <dbus/dbus.h>
#include <png.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <ctype.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Resources
#include "bg.png.h"
#include "gpm-ac-adapter.png.h"
#include "gpm-primary-000-charging.png.h"
#include "gpm-primary-000.png.h"
#include "gpm-primary-010-charging.png.h"
#include "gpm-primary-010.png.h"
#include "gpm-primary-020-charging.png.h"
#include "gpm-primary-020.png.h"
#include "gpm-primary-040-charging.png.h"
#include "gpm-primary-040.png.h"
#include "gpm-primary-060-charging.png.h"
#include "gpm-primary-060.png.h"
#include "gpm-primary-080-charging.png.h"
#include "gpm-primary-080.png.h"
#include "gpm-primary-090-charging.png.h"
#include "gpm-primary-090.png.h"
#include "gpm-primary-100-charging.png.h"
#include "gpm-primary-100.png.h"
#include "gpm-primary-charged.png.h"
#include "gpm-primary-missing-charging.png.h"
#include "gpm-primary-missing.png.h"
#include "verdana.ttf.h"

// Data structures
#define UPOWER_BUS "org.freedesktop.UPower"
#define UPOWER_PATH "/org/freedesktop/UPower"
#define UPOWER_IFACE "org.freedesktop.UPower"
#define UPOWER_DEV_IF "org.freedesktop.UPower.Device"
#define DBUS_PROP_IF "org.freedesktop.DBus.Properties"

#define BRIGHTD_BUS "net.iczelia.K16BrightD"
#define BRIGHTD_PATH "/net/iczelia/K16BrightD"
#define BRIGHTD_IFACE "net.iczelia.K16BrightD"

typedef struct {
  double percentage;  // %
  double energy_rate; // W
  uint32_t state;     // enum
  int64_t tte;        // seconds
  int64_t ttf;        // seconds
  bool valid;
} BatteryInfo;

typedef struct {
  double frequency_mhz; // MHz
  double temperature_c; // Celsius
  double fan_rpm;       // RPM
  bool have_freq;
  bool have_temp;
  bool have_fan;
} CpuInfo;

static void send_notification(const char *msg);

typedef struct {
  int level;    // raw brightness value
  int max;      // raw maximum value
  bool valid;
} BrightnessInfo;

typedef struct {
  char name[64];
  bool valid;
} GovernorInfo;

typedef struct {
  char original[64];
  bool has_original;
} GovernorState;

static GovernorState *governor_states = NULL;
static int governor_states_count = 0;
static bool powersave_active = false;
static bool powersave_threshold_enabled = false;
static double powersave_threshold = -1.0;
static GovernorInfo governor_info = {0};

#define EDIT_BUFFER_MAX 16

typedef enum {
  EDIT_FIELD_BRIGHTNESS = 0,
  EDIT_FIELD_GOVERNOR
} EditField;

typedef struct {
  bool active;
  EditField field;
  char buffer[EDIT_BUFFER_MAX + 1];
  size_t length;
  size_t cursor;
} EditState;

static EditState edit_state = {0};

typedef struct {
  Display *dpy;
  int screen;
  Window win;
  GC gc;
  int win_w, win_h, win_x, win_y;

  FT_Library ft_lib;
  FT_Face ft_face;
  XftFont *xft_font;
  XftDraw *xft_draw;
  XftColor xft_color_text;
  Pixmap icon_pix;
  unsigned int icon_w, icon_h;
  const unsigned char *icon_buf;
  size_t icon_len;

  Pixmap bg_pixmap;
  Picture bg_picture;
  Picture win_picture;
  unsigned int bg_w, bg_h;

  int dragging;
  int drag_off_x, drag_off_y;
} Ui;

static char cpu_freq_path[PATH_MAX];
static char cpu_temp_path[PATH_MAX];
static char fan_speed_path[PATH_MAX];
static char brightness_path[PATH_MAX];
static char max_brightness_path[PATH_MAX];

static double double_abs(double x) { return (x < 0.0) ? -x : x; }

static bool str_contains_ci(const char *haystack, const char *needle) {
  if (!haystack || !needle || !*needle)
    return false;
  size_t nlen = strlen(needle);
  for (const char *p = haystack; *p; ++p) {
    size_t i = 0;
    while (i < nlen && p[i] &&
           tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
      ++i;
    }
    if (i == nlen)
      return true;
  }
  return false;
}

static bool read_double_from_file(const char *path, double *out) {
  if (!path || !*path || !out)
    return false;
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  char buf[128];
  if (!fgets(buf, sizeof buf, f)) {
    fclose(f);
    return false;
  }
  fclose(f);
  errno = 0;
  char *end = NULL;
  double val = strtod(buf, &end);
  if (end == buf || errno == ERANGE)
    return false;
  *out = val;
  return true;
}

static bool read_int_from_file(const char *path, int *out) {
  if (!path || !*path || !out)
    return false;
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  char buf[128];
  if (!fgets(buf, sizeof buf, f)) {
    fclose(f);
    return false;
  }
  fclose(f);
  errno = 0;
  char *end = NULL;
  long val = strtol(buf, &end, 10);
  if (end == buf || errno == ERANGE)
    return false;
  *out = (int)val;
  return true;
}

static bool detect_cpu_freq_path(char *out, size_t n) {
  if (!out || n == 0)
    return false;
  const char *candidates[] = {
      "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
      "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq",
      "/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq",
      "/sys/devices/system/cpu/cpufreq/policy0/cpuinfo_cur_freq",
      NULL};
  for (size_t i = 0; candidates[i]; ++i) {
    FILE *f = fopen(candidates[i], "r");
    if (f) {
      fclose(f);
      snprintf(out, n, "%s", candidates[i]);
      return true;
    }
  }
  out[0] = '\0';
  return false;
}

static bool read_cpu_frequency_from_proc(double *out_mhz) {
  if (!out_mhz)
    return false;
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (!f)
    return false;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    if (strncmp(line, "cpu MHz", 7) != 0)
      continue;
    char *colon = strchr(line, ':');
    if (!colon)
      continue;
    ++colon;
    while (*colon && isspace((unsigned char)*colon))
      ++colon;
    errno = 0;
    double mhz = strtod(colon, NULL);
    if (errno == 0 && mhz > 0.0) {
      fclose(f);
      *out_mhz = mhz;
      return true;
    }
  }
  fclose(f);
  return false;
}

static bool read_cpu_frequency(double *out_mhz) {
  if (!out_mhz)
    return false;
  if (!cpu_freq_path[0])
    detect_cpu_freq_path(cpu_freq_path, sizeof cpu_freq_path);
  if (cpu_freq_path[0]) {
    double raw = 0.0;
    if (read_double_from_file(cpu_freq_path, &raw)) {
      while (raw > 10000.0)
        raw /= 1000.0;
      if (raw > 0.0) {
        *out_mhz = raw;
        return true;
      }
    } else {
      cpu_freq_path[0] = '\0';
    }
  }
  return read_cpu_frequency_from_proc(out_mhz);
}

static bool detect_cpu_temp_path(char *out, size_t n) {
  if (!out || n == 0)
    return false;
  const char *keywords[] = {"cpu", "package", "x86_pkg_temp", "soc", NULL};
  char type_path[PATH_MAX];
  char temp_path[PATH_MAX];
  for (int i = 0; i < 32; ++i) {
    snprintf(type_path, sizeof type_path,
             "/sys/class/thermal/thermal_zone%d/type", i);
    FILE *f = fopen(type_path, "r");
    if (!f)
      continue;
    char buf[128];
    if (!fgets(buf, sizeof buf, f)) {
      fclose(f);
      continue;
    }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = '\0';
    bool match = false;
    for (size_t k = 0; keywords[k]; ++k) {
      if (str_contains_ci(buf, keywords[k])) {
        match = true;
        break;
      }
    }
    if (!match)
      continue;
    snprintf(temp_path, sizeof temp_path,
             "/sys/class/thermal/thermal_zone%d/temp", i);
    FILE *tf = fopen(temp_path, "r");
    if (tf) {
      fclose(tf);
      snprintf(out, n, "%s", temp_path);
      return true;
    }
  }
  const char *fallbacks[] = {"/sys/class/thermal/thermal_zone0/temp",
                              "/sys/class/hwmon/hwmon0/temp1_input", NULL};
  for (size_t i = 0; fallbacks[i]; ++i) {
    FILE *f = fopen(fallbacks[i], "r");
    if (f) {
      fclose(f);
      snprintf(out, n, "%s", fallbacks[i]);
      return true;
    }
  }
  out[0] = '\0';
  return false;
}

static bool read_cpu_temperature(double *out_c) {
  if (!out_c)
    return false;
  if (!cpu_temp_path[0])
    detect_cpu_temp_path(cpu_temp_path, sizeof cpu_temp_path);
  if (!cpu_temp_path[0])
    return false;
  double raw = 0.0;
  if (!read_double_from_file(cpu_temp_path, &raw)) {
    cpu_temp_path[0] = '\0';
    return false;
  }
  if (raw > 1000.0)
    raw /= 1000.0;
  *out_c = raw;
  return true;
}

static bool detect_fan_speed_path(char *out, size_t n) {
  if (!out || n == 0)
    return false;
  const char *base = "/sys/class/hwmon";
  DIR *dir = opendir(base);
  if (dir) {
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
      if (strncmp(ent->d_name, "hwmon", 5) != 0)
        continue;
      char hwmon_path[PATH_MAX];
      snprintf(hwmon_path, sizeof hwmon_path, "%s/%s", base, ent->d_name);
      for (int fan = 1; fan <= 8; ++fan) {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof candidate, "%s/fan%d_input", hwmon_path,
                 fan);
        FILE *f = fopen(candidate, "r");
        if (f) {
          fclose(f);
          snprintf(out, n, "%s", candidate);
          closedir(dir);
          return true;
        }
      }
    }
    closedir(dir);
  }
  const char *fallbacks[] = {
      "/sys/devices/platform/applesmc.768/fan1_input",
      "/sys/devices/platform/thinkpad_hwmon/hwmon/hwmon0/fan1_input",
      NULL};
  for (size_t i = 0; fallbacks[i]; ++i) {
    FILE *f = fopen(fallbacks[i], "r");
    if (f) {
      fclose(f);
      snprintf(out, n, "%s", fallbacks[i]);
      return true;
    }
  }
  out[0] = '\0';
  return false;
}

static bool read_fan_speed(double *out_rpm) {
  if (!out_rpm)
    return false;
  if (!fan_speed_path[0])
    detect_fan_speed_path(fan_speed_path, sizeof fan_speed_path);
  if (!fan_speed_path[0])
    return false;
  double raw = 0.0;
  if (!read_double_from_file(fan_speed_path, &raw)) {
    fan_speed_path[0] = '\0';
    return false;
  }
  if (raw < 0.0)
    return false;
  *out_rpm = raw;
  return true;
}

static bool detect_brightness_paths(void) {
  if (brightness_path[0] && max_brightness_path[0])
    return true;
  DIR *dir = opendir("/sys/class/backlight");
  if (!dir)
    return false;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_name[0] == '.')
      continue;
    char base[PATH_MAX];
    snprintf(base, sizeof base, "/sys/class/backlight/%s", ent->d_name);
    char b_path[PATH_MAX];
    char max_path[PATH_MAX];
    snprintf(b_path, sizeof b_path, "%s/brightness", base);
    snprintf(max_path, sizeof max_path, "%s/max_brightness", base);
    FILE *b_file = fopen(b_path, "r");
    FILE *m_file = fopen(max_path, "r");
    if (b_file && m_file) {
      fclose(b_file);
      fclose(m_file);
      snprintf(brightness_path, sizeof brightness_path, "%s", b_path);
      snprintf(max_brightness_path, sizeof max_brightness_path, "%s",
               max_path);
      closedir(dir);
      return true;
    }
    if (b_file)
      fclose(b_file);
    if (m_file)
      fclose(m_file);
  }
  closedir(dir);
  return false;
}

static bool read_brightness(BrightnessInfo *info) {
  if (!info)
    return false;
  BrightnessInfo tmp = {0};
  if (!detect_brightness_paths()) {
    *info = tmp;
    return false;
  }
  int level = 0;
  int max = 0;
  if (!read_int_from_file(brightness_path, &level) ||
      !read_int_from_file(max_brightness_path, &max) || max <= 0) {
    brightness_path[0] = '\0';
    max_brightness_path[0] = '\0';
    *info = tmp;
    return false;
  }
  tmp.level = level;
  tmp.max = max;
  tmp.valid = true;
  *info = tmp;
  return true;
}

static bool write_brightness(int value) {
  if (!detect_brightness_paths())
    return false;
  FILE *f = fopen(brightness_path, "w");
  if (!f)
    return false;
  int rc = fprintf(f, "%d\n", value);
  int err = ferror(f);
  fclose(f);
  if (rc < 0 || err)
    return false;
  return true;
}

static bool get_backlight_name(char *out, size_t n) {
  if (!out || n == 0)
    return false;
  if (!detect_brightness_paths())
    return false;
  const char *end = strrchr(brightness_path, '/');
  if (!end || end == brightness_path)
    return false;
  const char *start = end;
  while (start > brightness_path && *(start - 1) != '/')
    --start;
  size_t len = (size_t)(end - start);
  if (len == 0 || len + 1 > n)
    return false;
  memcpy(out, start, len);
  out[len] = '\0';
  return true;
}

static bool set_brightness_via_service(DBusConnection *conn, int value);
static bool dbus_check(DBusError *err, const char *ctx);
static bool set_governor_all(DBusConnection *conn, const char *governor);
static void query_governor_info(GovernorInfo *info);
static bool governor_info_equal(const GovernorInfo *a,
                                const GovernorInfo *b);
static void edit_state_begin(EditField field);
static void edit_state_switch(void);
static void edit_state_cancel(void);
static bool edit_handle_key(KeySym sym, const char *text, int text_len,
                            DBusConnection *conn, BrightnessInfo *brightness,
                            bool *out_dirty);
static int draw_label(Ui *ui, int x, int y, const char *label,
                      bool underline);
static void draw_edit_buffer(Ui *ui, int x, int y);
static bool apply_brightness_input(DBusConnection *conn,
                                   BrightnessInfo *brightness);
static bool apply_governor_input(DBusConnection *conn);
static bool parse_percentage_input(const char *text, double *out_pct);
static void trim_whitespace(char *s);

static bool read_cpu_governor(int cpu, char *out, size_t n) {
  if (!out || n == 0 || cpu < 0) {
    return false;
  }
  char path[PATH_MAX];
  snprintf(path, sizeof path,
           "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
  FILE *f = fopen(path, "r");
  if (!f)
    return false;
  char buf[128];
  if (!fgets(buf, sizeof buf, f)) {
    fclose(f);
    return false;
  }
  fclose(f);
  buf[strcspn(buf, "\r\n")] = '\0';
  if (buf[0] == '\0')
    return false;
  snprintf(out, n, "%s", buf);
  return true;
}

static bool set_governor_via_service(DBusConnection *conn, int cpu,
                                     const char *governor) {
  if (!conn || !governor)
    return false;
  DBusMessage *msg = dbus_message_new_method_call(
      BRIGHTD_BUS, BRIGHTD_PATH, BRIGHTD_IFACE, "SetGovernor");
  if (!msg)
    return false;
  int32_t cpu_arg = (int32_t)cpu;
  const char *gov_arg = governor;
  dbus_message_append_args(msg, DBUS_TYPE_INT32, &cpu_arg, DBUS_TYPE_STRING,
                           &gov_arg, DBUS_TYPE_INVALID);
  DBusError err;
  dbus_error_init(&err);
  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(conn, msg, 2000, &err);
  dbus_message_unref(msg);
  if (!dbus_check(&err, "K16BrightD.SetGovernor")) {
    if (reply)
      dbus_message_unref(reply);
    return false;
  }
  if (!reply)
    return false;
  if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
    const char *err_name = dbus_message_get_error_name(reply);
    if (err_name)
      fprintf(stderr, "K16BrightD.SetGovernor: %s\n", err_name);
    dbus_message_unref(reply);
    return false;
  }
  dbus_message_unref(reply);
  return true;
}

static int detect_cpu_count(void) {
  long n = sysconf(_SC_NPROCESSORS_CONF);
  if (n > 0 && n < INT_MAX)
    return (int)n;
  int highest = -1;
  struct stat st;
  char path[PATH_MAX];
  for (int cpu = 0; cpu < 4096; ++cpu) {
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d", cpu);
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
      highest = cpu;
  }
  return highest + 1;
}

static bool ensure_governor_states(void) {
  if (governor_states)
    return governor_states_count > 0;
  int count = detect_cpu_count();
  if (count <= 0)
    return false;
  governor_states = calloc((size_t)count, sizeof(GovernorState));
  if (!governor_states)
    return false;
  governor_states_count = count;
  return true;
}

static bool apply_powersave(DBusConnection *conn) {
  if (!conn)
    return false;
  if (!ensure_governor_states())
    return false;
  bool touched = false;
  bool ok = true;
  for (int cpu = 0; cpu < governor_states_count; ++cpu) {
    char current[64];
    if (!read_cpu_governor(cpu, current, sizeof current))
      continue;
    touched = true;
    snprintf(governor_states[cpu].original,
             sizeof governor_states[cpu].original, "%s", current);
    governor_states[cpu].has_original = true;
    if (strcmp(current, "powersave") != 0) {
      if (!set_governor_via_service(conn, cpu, "powersave"))
        ok = false;
    }
  }
  if (touched) {
    powersave_active = true;
    if (!ok)
      fprintf(stderr, "Warning: failed to set powersave governor on all CPUs\n");
    return true;
  }
  return false;
}

static bool restore_governors(DBusConnection *conn) {
  if (!conn || !governor_states)
    return false;
  bool have_state = false;
  bool touched = false;
  bool ok = true;
  for (int cpu = 0; cpu < governor_states_count; ++cpu) {
    if (!governor_states[cpu].has_original)
      continue;
    have_state = true;
    const char *target = governor_states[cpu].original;
    if (!target[0])
      continue;
    char current[64];
    if (read_cpu_governor(cpu, current, sizeof current) &&
        strcmp(current, target) == 0)
      continue;
    if (!set_governor_via_service(conn, cpu, target))
      ok = false;
    else
      touched = true;
  }
  if (!have_state) {
    powersave_active = false;
    return false;
  }
  if (ok)
    powersave_active = false;
  else if (touched)
    fprintf(stderr, "Warning: failed to restore all CPU governors\n");
  return touched || ok;
}

static bool set_governor_all(DBusConnection *conn, const char *governor) {
  if (!conn || !governor)
    return false;
  if (!ensure_governor_states())
    return false;
  bool any = false;
  bool ok = true;
  for (int cpu = 0; cpu < governor_states_count; ++cpu) {
    char current[64];
    if (!read_cpu_governor(cpu, current, sizeof current))
      continue;
    any = true;
    if (strcmp(current, governor) == 0) {
      if (!powersave_active) {
        snprintf(governor_states[cpu].original,
                 sizeof governor_states[cpu].original, "%s", governor);
        governor_states[cpu].has_original = true;
      }
      continue;
    }
    if (!set_governor_via_service(conn, cpu, governor)) {
      ok = false;
    } else if (!powersave_active) {
      snprintf(governor_states[cpu].original,
               sizeof governor_states[cpu].original, "%s", governor);
      governor_states[cpu].has_original = true;
    }
  }
  if (!any)
    return false;
  if (!ok)
    fprintf(stderr, "Warning: failed to set governor on all CPUs\n");
  GovernorInfo updated = {0};
  query_governor_info(&updated);
  if (updated.valid)
    governor_info = updated;
  return ok;
}

static bool handle_powersave_threshold(DBusConnection *conn,
                                       const BatteryInfo *b) {
  if (!powersave_threshold_enabled || !b || !b->valid)
    return false;
  if (powersave_active) {
    if (b->percentage > powersave_threshold) {
      if (restore_governors(conn))
        return true;
    }
    return false;
  }
  if (b->percentage <= powersave_threshold) {
    if (apply_powersave(conn))
      return true;
  }
  return false;
}

static void query_governor_info(GovernorInfo *info) {
  if (!info)
    return;
  GovernorInfo tmp = {0};
  char name[64];
  if (read_cpu_governor(0, name, sizeof name)) {
    snprintf(tmp.name, sizeof tmp.name, "%s", name);
    tmp.valid = true;
  }
  *info = tmp;
}

static bool governor_info_equal(const GovernorInfo *a, const GovernorInfo *b) {
  if (!a || !b)
    return false;
  if (a->valid != b->valid)
    return false;
  if (!a->valid)
    return true;
  return strncmp(a->name, b->name, sizeof a->name) == 0;
}

static void trim_whitespace(char *s) {
  if (!s)
    return;
  size_t len = strlen(s);
  size_t start = 0;
  while (start < len && isspace((unsigned char)s[start]))
    ++start;
  size_t end = len;
  while (end > start && isspace((unsigned char)s[end - 1]))
    --end;
  size_t new_len = end > start ? (end - start) : 0;
  if (start > 0 && new_len > 0)
    memmove(s, s + start, new_len);
  s[new_len] = '\0';
}

static bool parse_percentage_input(const char *text, double *out_pct) {
  if (!text || !out_pct)
    return false;
  char buf[32];
  size_t len = strnlen(text, EDIT_BUFFER_MAX);
  memcpy(buf, text, len);
  buf[len] = '\0';
  trim_whitespace(buf);
  if (!buf[0])
    return false;
  size_t blen = strlen(buf);
  if (blen > 0 && buf[blen - 1] == '%') {
    buf[blen - 1] = '\0';
    trim_whitespace(buf);
    if (!buf[0])
      return false;
  }
  errno = 0;
  char *end = NULL;
  double val = strtod(buf, &end);
  if (end == buf || errno == ERANGE)
    return false;
  while (end && *end) {
    if (!isspace((unsigned char)*end))
      return false;
    ++end;
  }
  *out_pct = val;
  return true;
}

static void edit_state_begin(EditField field) {
  edit_state.active = true;
  edit_state.field = field;
  edit_state.length = 0;
  edit_state.cursor = 0;
  memset(edit_state.buffer, 0, sizeof edit_state.buffer);
}

static void edit_state_switch(void) {
  EditField next = EDIT_FIELD_BRIGHTNESS;
  if (edit_state.active && edit_state.field == EDIT_FIELD_BRIGHTNESS)
    next = EDIT_FIELD_GOVERNOR;
  else if (edit_state.active && edit_state.field == EDIT_FIELD_GOVERNOR)
    next = EDIT_FIELD_BRIGHTNESS;
  edit_state_begin(next);
}

static void edit_state_cancel(void) {
  edit_state.active = false;
  edit_state.length = 0;
  edit_state.cursor = 0;
  edit_state.buffer[0] = '\0';
}

static bool edit_insert_char(char ch) {
  if (edit_state.length >= EDIT_BUFFER_MAX)
    return false;
  if (edit_state.cursor > edit_state.length)
    edit_state.cursor = edit_state.length;
  memmove(edit_state.buffer + edit_state.cursor + 1,
          edit_state.buffer + edit_state.cursor,
          edit_state.length - edit_state.cursor + 1);
  edit_state.buffer[edit_state.cursor] = ch;
  ++edit_state.length;
  ++edit_state.cursor;
  return true;
}

static bool edit_backspace(void) {
  if (edit_state.cursor == 0 || edit_state.length == 0)
    return false;
  memmove(edit_state.buffer + edit_state.cursor - 1,
          edit_state.buffer + edit_state.cursor,
          edit_state.length - edit_state.cursor + 1);
  --edit_state.cursor;
  --edit_state.length;
  return true;
}

static bool edit_delete(void) {
  if (edit_state.cursor >= edit_state.length)
    return false;
  memmove(edit_state.buffer + edit_state.cursor,
          edit_state.buffer + edit_state.cursor + 1,
          edit_state.length - edit_state.cursor);
  --edit_state.length;
  return true;
}

static bool edit_move_left(void) {
  if (edit_state.cursor == 0)
    return false;
  --edit_state.cursor;
  return true;
}

static bool edit_move_right(void) {
  if (edit_state.cursor >= edit_state.length)
    return false;
  ++edit_state.cursor;
  return true;
}

static bool edit_move_home(void) {
  if (edit_state.cursor == 0)
    return false;
  edit_state.cursor = 0;
  return true;
}

static bool edit_move_end(void) {
  if (edit_state.cursor == edit_state.length)
    return false;
  edit_state.cursor = edit_state.length;
  return true;
}

static bool apply_brightness_input(DBusConnection *conn,
                                   BrightnessInfo *brightness) {
  if (!conn || !brightness)
    return false;
  double pct = 0.0;
  if (!parse_percentage_input(edit_state.buffer, &pct)) {
    fprintf(stderr, "Invalid brightness value\n");
    return false;
  }
  if (!read_brightness(brightness)) {
    fprintf(stderr, "No brightness device detected\n");
    return false;
  }
  if (brightness->max <= 0) {
    fprintf(stderr, "Brightness max value invalid\n");
    return false;
  }
  if (pct < 0.0)
    pct = 0.0;
  if (pct > 100.0)
    pct = 100.0;
  double raw = pct * (double)brightness->max / 100.0;
  int new_level = (int)(raw + 0.5);
  if (new_level < 0)
    new_level = 0;
  if (new_level > brightness->max)
    new_level = brightness->max;
  if (!set_brightness_via_service(conn, new_level)) {
    fprintf(stderr, "Failed to set brightness\n");
    return false;
  }
  BrightnessInfo updated = {0};
  if (read_brightness(&updated)) {
    *brightness = updated;
  }
  return true;
}

static bool is_valid_governor_name(const char *name) {
  if (!name || !*name)
    return false;
  size_t len = strlen(name);
  if (len > 32)
    return false;
  for (size_t i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)name[i];
    if (!(isalnum(c) || c == '_' || c == '-'))
      return false;
  }
  return true;
}

static bool apply_governor_input(DBusConnection *conn) {
  if (!conn)
    return false;
  char buf[EDIT_BUFFER_MAX + 1];
  memcpy(buf, edit_state.buffer, edit_state.length);
  buf[edit_state.length] = '\0';
  trim_whitespace(buf);
  if (!buf[0]) {
    fprintf(stderr, "Governor value is empty\n");
    return false;
  }
  if (!is_valid_governor_name(buf)) {
    fprintf(stderr, "Governor name contains invalid characters\n");
    return false;
  }
  if (!set_governor_all(conn, buf))
    return false;
  return true;
}

static bool edit_handle_key(KeySym sym, const char *text, int text_len,
                            DBusConnection *conn, BrightnessInfo *brightness,
                            bool *out_dirty) {
  if (out_dirty)
    *out_dirty = false;
  if (!edit_state.active)
    return false;
  switch (sym) {
  case XK_Return:
  case XK_KP_Enter: {
    bool ok = false;
    if (edit_state.field == EDIT_FIELD_BRIGHTNESS)
      ok = apply_brightness_input(conn, brightness);
    else if (edit_state.field == EDIT_FIELD_GOVERNOR)
      ok = apply_governor_input(conn);
    if (ok) {
      edit_state_cancel();
      if (out_dirty)
        *out_dirty = true;
    }
    return true;
  }
  case XK_Escape:
    edit_state_cancel();
    if (out_dirty)
      *out_dirty = true;
    return true;
  case XK_Left:
    if (edit_move_left()) {
      if (out_dirty)
        *out_dirty = true;
    }
    return true;
  case XK_Right:
    if (edit_move_right()) {
      if (out_dirty)
        *out_dirty = true;
    }
    return true;
  case XK_Home:
    if (edit_move_home()) {
      if (out_dirty)
        *out_dirty = true;
    }
    return true;
  case XK_End:
    if (edit_move_end()) {
      if (out_dirty)
        *out_dirty = true;
    }
    return true;
  case XK_BackSpace:
    if (edit_backspace()) {
      if (out_dirty)
        *out_dirty = true;
    }
    return true;
  case XK_Delete:
    if (edit_delete()) {
      if (out_dirty)
        *out_dirty = true;
    }
    return true;
  default:
    break;
  }
  bool inserted = false;
  for (int i = 0; i < text_len; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (c < 32 || c > 126)
      continue;
    if (edit_insert_char((char)c))
      inserted = true;
  }
  if (inserted && out_dirty)
    *out_dirty = true;
  return inserted;
}

static bool adjust_brightness(int direction, BrightnessInfo *info,
                              DBusConnection *conn) {
  if (!info)
    return false;
  BrightnessInfo current = {0};
  if (!info->valid) {
    if (!read_brightness(&current))
      return false;
  } else {
    current = *info;
  }
  if (!current.valid)
    return false;
  int step = current.max / 20;
  if (step < 1)
    step = 1;
  int new_level = current.level + direction * step;
  if (new_level < 0)
    new_level = 0;
  if (new_level > current.max)
    new_level = current.max;
  if (new_level == current.level)
    return false;
  bool wrote = set_brightness_via_service(conn, new_level);
  if (!wrote)
    return false;
  BrightnessInfo updated = {0};
  if (!read_brightness(&updated))
    return false;
  *info = updated;
  return true;
}

static bool brightness_equal(const BrightnessInfo *a,
                             const BrightnessInfo *b) {
  if (!a || !b)
    return false;
  if (a->valid != b->valid)
    return false;
  if (!a->valid)
    return true;
  return a->level == b->level && a->max == b->max;
}

static bool read_cpu_info(CpuInfo *info) {
  if (!info)
    return false;
  CpuInfo tmp = {0};
  double mhz = 0.0;
  if (read_cpu_frequency(&mhz)) {
    tmp.frequency_mhz = mhz;
    tmp.have_freq = true;
  }
  double temp_c = 0.0;
  if (read_cpu_temperature(&temp_c)) {
    tmp.temperature_c = temp_c;
    tmp.have_temp = true;
  }
  double rpm = 0.0;
  if (read_fan_speed(&rpm)) {
    tmp.fan_rpm = rpm;
    tmp.have_fan = true;
  }
  *info = tmp;
  return tmp.have_freq || tmp.have_temp || tmp.have_fan;
}

static bool cpu_info_equal(const CpuInfo *a, const CpuInfo *b) {
  if (!a || !b)
    return false;
  if (a->have_freq != b->have_freq)
    return false;
  if (a->have_temp != b->have_temp)
    return false;
  if (a->have_fan != b->have_fan)
    return false;
  if (a->have_freq && double_abs(a->frequency_mhz - b->frequency_mhz) > 0.5)
    return false;
  if (a->have_temp && double_abs(a->temperature_c - b->temperature_c) > 0.2)
    return false;
  if (a->have_fan && double_abs(a->fan_rpm - b->fan_rpm) > 50.0)
    return false;
  return true;
}

static void draw_gradient_border(int w, int h, Display *dpy, Window win) {
  if (w <= 1 || h <= 1)
    return;
  XRenderPictFormat *fmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);
  if (!fmt)
    return;
  Picture dst = XRenderCreatePicture(dpy, win, fmt, 0, NULL);
  XFixed stops[2] = {XDoubleToFixed(0.0), XDoubleToFixed(1.0)};
  XRenderColor bw[2] = {{0, 0, 0, 0xffff}, {0xffff, 0xffff, 0xffff, 0xffff}};
  XRenderColor wb[2] = {{0xffff, 0xffff, 0xffff, 0xffff}, {0, 0, 0, 0xffff}};
  XLinearGradient lg;
  // top: black→white, left-to-right
  lg.p1.x = XDoubleToFixed(0);
  lg.p1.y = XDoubleToFixed(0);
  lg.p2.x = XDoubleToFixed(w);
  lg.p2.y = XDoubleToFixed(0);
  Picture p = XRenderCreateLinearGradient(dpy, &lg, stops, bw, 2);
  XRenderComposite(dpy, PictOpSrc, p, None, dst, 0, 0, 0, 0, 0, 0, w, 1);
  XRenderFreePicture(dpy, p);
  // bottom: white→black, left-to-right
  lg.p1.y = XDoubleToFixed(h - 1);
  lg.p2.y = XDoubleToFixed(h - 1);
  p = XRenderCreateLinearGradient(dpy, &lg, stops, wb, 2);
  XRenderComposite(dpy, PictOpSrc, p, None, dst, 0, 0, 0, 0, 0, h - 1, w, 1);
  XRenderFreePicture(dpy, p);
  // left: black→white, top-to-bottom
  lg.p1.x = XDoubleToFixed(0);
  lg.p1.y = XDoubleToFixed(0);
  lg.p2.x = XDoubleToFixed(0);
  lg.p2.y = XDoubleToFixed(h);
  p = XRenderCreateLinearGradient(dpy, &lg, stops, bw, 2);
  XRenderComposite(dpy, PictOpSrc, p, None, dst, 0, 0, 0, 0, 0, 0, 1, h);
  XRenderFreePicture(dpy, p);
  // right: white→black, top-to-bottom
  lg.p1.x = XDoubleToFixed(w - 1);
  lg.p1.y = XDoubleToFixed(0);
  lg.p2.x = XDoubleToFixed(w - 1);
  lg.p2.y = XDoubleToFixed(h);
  p = XRenderCreateLinearGradient(dpy, &lg, stops, wb, 2);
  XRenderComposite(dpy, PictOpSrc, p, None, dst, 0, 0, 0, 0, w - 1, 0, 1, h);
  XRenderFreePicture(dpy, p);
  XRenderFreePicture(dpy, dst);
}

static int draw_label(Ui *ui, int x, int y, const char *label,
                      bool underline) {
  if (!ui || !label)
    return 0;
  int len = (int)strlen(label);
  XGlyphInfo ext;
  XftTextExtentsUtf8(ui->dpy, ui->xft_font, (const FcChar8 *)label, len,
                     &ext);
  XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, x, y,
                    (const FcChar8 *)label, len);
  if (underline) {
    XSetForeground(ui->dpy, ui->gc, ui->xft_color_text.pixel);
    int underline_y = y + 2;
    XDrawLine(ui->dpy, ui->win, ui->gc, x, underline_y, x + ext.xOff,
              underline_y);
  }
  return ext.xOff;
}

static void draw_edit_buffer(Ui *ui, int x, int y) {
  if (!ui)
    return;
  if (edit_state.length > 0) {
    XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, x, y,
                      (const FcChar8 *)edit_state.buffer,
                      (int)edit_state.length);
  }
  char prefix[EDIT_BUFFER_MAX + 1];
  size_t prefix_len = edit_state.cursor;
  if (prefix_len > EDIT_BUFFER_MAX)
    prefix_len = EDIT_BUFFER_MAX;
  memcpy(prefix, edit_state.buffer, prefix_len);
  prefix[prefix_len] = '\0';
  XGlyphInfo ext = {0};
  if (prefix_len > 0) {
    XftTextExtentsUtf8(ui->dpy, ui->xft_font, (const FcChar8 *)prefix,
                       (int)prefix_len, &ext);
  }
  int cursor_x = x + ext.xOff;
  int cursor_y = y + 2;
  XSetForeground(ui->dpy, ui->gc, ui->xft_color_text.pixel);
  XDrawLine(ui->dpy, ui->win, ui->gc, cursor_x, cursor_y, cursor_x + 8,
            cursor_y);
}

XftFont *xft_font_from_memory(Display *dpy, int screen,
                              const unsigned char *data, size_t len,
                              double pixel_size, FT_Library *out_lib,
                              FT_Face *out_face) {
  FT_Library lib = NULL;
  if (FT_Init_FreeType(&lib))
    return NULL;
  FT_Face face = NULL;
  if (FT_New_Memory_Face(lib, data, (FT_Long)len, 0, &face)) {
    FT_Done_FreeType(lib);
    return NULL;
  }
  FcPattern *pat =
      FcFreeTypeQueryFace(face, (const FcChar8 *)"memory", 0, NULL);
  if (!pat)
    return NULL;
  FcPatternDel(pat, FC_FILE);
  FcPatternDel(pat, FC_INDEX);
  FcPatternAddFTFace(pat, FC_FT_FACE, face);
  FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
  FcPatternAddDouble(pat, FC_SIZE, pixel_size);
  FcConfigSubstitute(NULL, pat, FcMatchPattern);
  XftDefaultSubstitute(dpy, screen, pat);
  XftFont *xf = XftFontOpenPattern(dpy, pat);
  if (!xf) {
    FcPatternDestroy(pat);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return NULL;
  }
  if (out_lib)
    *out_lib = lib;
  else
    FT_Done_FreeType(lib);
  if (out_face)
    *out_face = face;
  else
    FT_Done_Face(face);
  return xf;
}

static void set_font(Ui *app) {
  if (!FcInit())
    exit(2);

  double pixel_size = 9.0;
  app->xft_font = xft_font_from_memory(app->dpy, app->screen, verdana_ttf,
                                       (size_t)verdana_ttf_len, pixel_size,
                                       &app->ft_lib, &app->ft_face);
  if (!app->xft_font)
    exit(2);

  // Get the visual + colormap actually used by the window
  XWindowAttributes wa;
  XGetWindowAttributes(app->dpy, app->win, &wa);

  app->xft_draw = XftDrawCreate(app->dpy, app->win, wa.visual, wa.colormap);
  if (!app->xft_draw)
    exit(2);

  if (!XftColorAllocName(app->dpy, wa.visual, wa.colormap, "white",
                         &app->xft_color_text))
    exit(2);
}

static Pixmap load_png_to_pixmap_from_mem(Display *dpy, Drawable root,
                                          const unsigned char *buf, size_t len,
                                          unsigned *out_w, unsigned *out_h) {
  if (!buf || !len)
    return 0;

  png_image im;
  memset(&im, 0, sizeof im);
  im.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&im, buf, len))
    return 0;
  im.format = PNG_FORMAT_RGBA;

  size_t sz = PNG_IMAGE_SIZE(im);
  png_bytep rgba = malloc(sz);
  if (!rgba) {
    png_image_free(&im);
    return 0;
  }
  if (!png_image_finish_read(&im, NULL, rgba, 0, NULL)) {
    free(rgba);
    png_image_free(&im);
    return 0;
  }

  const unsigned w = im.width, h = im.height;
  uint32_t *argb = malloc((size_t)w * h * 4);
  if (!argb) {
    free(rgba);
    png_image_free(&im);
    return 0;
  }
  for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
    uint8_t r = rgba[4 * i + 0], g = rgba[4 * i + 1], b = rgba[4 * i + 2],
            a = rgba[4 * i + 3];
    // premultiply
    r = (uint8_t)((r * (uint16_t)a + 127) / 255);
    g = (uint8_t)((g * (uint16_t)a + 127) / 255);
    b = (uint8_t)((b * (uint16_t)a + 127) / 255);
    argb[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
              (uint32_t)b;
  }
  free(rgba);
  png_image_free(&im);

  int fmt_count = 0;
  XPixmapFormatValues *pf = XListPixmapFormats(dpy, &fmt_count);
  int has32 = 0;
  for (int i = 0; i < fmt_count; ++i)
    if (pf[i].depth == 32) {
      has32 = 1;
      break;
    }
  if (pf)
    XFree(pf);
  if (!has32) {
    free(argb);
    return 0;
  }

  Pixmap pix = XCreatePixmap(dpy, root, w, h, 32);
  if (!pix) {
    free(argb);
    return 0;
  }

  XImage *xi = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 32,
                            ZPixmap, 0, (char *)argb, w, h, 32, 0);
  if (!xi) {
    XFreePixmap(dpy, pix);
    free(argb);
    return 0;
  }
  xi->byte_order = ImageByteOrder(dpy); // let Xlib handle endianness

  GC gc = XCreateGC(dpy, pix, 0, NULL);
  XPutImage(dpy, pix, gc, xi, 0, 0, 0, 0, w, h);
  XFreeGC(dpy, gc);
  XDestroyImage(xi); // also frees argb

  if (out_w)
    *out_w = w;
  if (out_h)
    *out_h = h;
  return pix;
}

static const char *state_str(uint32_t s) {
  switch (s) {
  case 1:
    return "Charging";
  case 2:
    return "Discharging";
  case 3:
    return "Empty";
  case 4:
    return "Full";
  case 5:
    return "Pending+";
  case 6:
    return "Pending-";
  default:
    return "Unknown";
  }
}

static void fmt_eta(char *out, size_t n, uint32_t state, int64_t tte,
                    int64_t ttf) {
  int64_t sec = (state == 1) ? ttf : (state == 2) ? tte : -1;
  if (sec <= 0) {
    snprintf(out, n, "?");
    return;
  }
  long h = (long)(sec / 3600);
  long m = (long)((sec % 3600) / 60);
  if (h > 0)
    snprintf(out, n, "%ldh %ldm", h, m);
  else
    snprintf(out, n, "%ldm", m);
}

static void ui_draw(Ui *ui, const BatteryInfo *b, const CpuInfo *cpu,
                    const BrightnessInfo *brightness,
                    const GovernorInfo *governor) {
  if (!ui->win_picture) {
    XRenderPictFormat *fmt =
        XRenderFindStandardFormat(ui->dpy, PictStandardARGB32);
    if (!fmt) {
      printf("Failed to find ARGB32 pict format\n");
      exit(5);
    }
    ui->win_picture = XRenderCreatePicture(ui->dpy, ui->win, fmt, 0, NULL);
  }
  if (ui->win_w > 0 && ui->win_h > 0) {
    XTransform t;
    double sx = (double)ui->bg_w / (double)ui->win_w;
    double sy = (double)ui->bg_h / (double)ui->win_h;
    memset(&t, 0, sizeof(t));
    t.matrix[0][0] = XDoubleToFixed(sx);
    t.matrix[1][1] = XDoubleToFixed(sy);
    t.matrix[2][2] = XDoubleToFixed(1.0);
    XRenderSetPictureTransform(ui->dpy, ui->bg_picture, &t);
  }
  XRenderComposite(ui->dpy, PictOpSrc, ui->bg_picture, None, ui->win_picture, 0,
                   0, 0, 0, 0, 0, ui->win_w, ui->win_h);
  draw_gradient_border(ui->win_w, ui->win_h, ui->dpy, ui->win);
  char l_eta[128];
  char l_status[128];
  char l_power[128];
  int text_x = 8;
  if (ui->icon_pix) {
    XRenderPictFormat *sfmt =
        XRenderFindStandardFormat(ui->dpy, PictStandardARGB32);

    Picture src = XRenderCreatePicture(ui->dpy, ui->icon_pix, sfmt, 0, NULL);
    XRenderComposite(ui->dpy, PictOpOver, src, None, ui->win_picture, 0, 0, 0,
                     0, 5, 10, ui->icon_w, ui->icon_h);
    XRenderFreePicture(ui->dpy, src);
    text_x = (int)ui->icon_w + 12; /* leave some padding */
  }
  int y = 18;
  if (b && b->valid) {
    fmt_eta(l_eta, sizeof l_eta, b->state, b->tte, b->ttf);
    snprintf(l_status, sizeof l_status, "%s; %.1f%%", state_str(b->state),
             b->percentage);
    snprintf(l_power, sizeof l_power, "%s, %.2f W", l_eta, b->energy_rate);
    XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, text_x,
                      y, (const FcChar8 *)l_status, (int)strlen(l_status));
    y += 16;
    XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, text_x,
                      y, (const FcChar8 *)l_power, (int)strlen(l_power));
    y += 16;
  } else {
    const char *msg = "No battery data";
    XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, text_x,
                      y, (const FcChar8 *)msg, (int)strlen(msg));
    y += 16;
  }

  char cpu_line[160];
  if (cpu && (cpu->have_freq || cpu->have_temp)) {
    int len = snprintf(cpu_line, sizeof cpu_line, "CPU:");
    if (cpu->have_freq) {
      double mhz = cpu->frequency_mhz;
      const char *unit;
      if (mhz >= 1000.0) {
        mhz /= 1000.0;
        unit = "GHz";
      } else {
        unit = "MHz";
      }
      len += snprintf(cpu_line + len, sizeof cpu_line - (size_t)len,
                      " %.2f %s", mhz, unit);
    }
    if (cpu->have_temp) {
      len += snprintf(cpu_line + len, sizeof cpu_line - (size_t)len,
                      "%s %.1f °C",
                      (cpu->have_freq ? "," : ""), cpu->temperature_c);
    }
    if (len <= 4) {
      snprintf(cpu_line, sizeof cpu_line, "CPU data unavailable");
    }
  } else if (cpu && cpu->have_temp) {
    snprintf(cpu_line, sizeof cpu_line, "CPU temp: %.1f °C", cpu->temperature_c);
  } else {
    snprintf(cpu_line, sizeof cpu_line, "CPU data unavailable");
  }
  XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, 8, y,
                    (const FcChar8 *)cpu_line, (int)strlen(cpu_line));
  y += 16;
  if (cpu && cpu->have_fan) {
    char fan_line[64];
    snprintf(fan_line, sizeof fan_line, "Fan speed: %.0f RPM", cpu->fan_rpm);
    XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, 8, y,
                      (const FcChar8 *)fan_line, (int)strlen(fan_line));
    y += 16;
  }
  bool edit_b = edit_state.active && edit_state.field == EDIT_FIELD_BRIGHTNESS;
  bool edit_g = edit_state.active && edit_state.field == EDIT_FIELD_GOVERNOR;

  const char *br_label = "Brightness:";
  int label_width = draw_label(ui, 8, y, br_label, edit_b);
  int value_x = 8 + label_width + 4;
  if (edit_b) {
    draw_edit_buffer(ui, value_x, y);
  } else if (brightness && brightness->valid) {
    double pct = (double)brightness->level / (double)brightness->max * 100.0;
    char value[64];
    snprintf(value, sizeof value, " %.0f%%", pct);
    XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, value_x,
                      y, (const FcChar8 *)value, (int)strlen(value));
  } else {
    const char *value = " unavailable";
    XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, value_x,
                      y, (const FcChar8 *)value, (int)strlen(value));
  }
  y += 16;

  const char *gov_label = "Governor:";
  label_width = draw_label(ui, 8, y, gov_label, edit_g);
  value_x = 8 + label_width + 4;
  if (edit_g) {
    draw_edit_buffer(ui, value_x, y);
  } else if (governor && governor->valid) {
    char value[64];
    snprintf(value, sizeof value, " %s", governor->name);
    XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, value_x,
                      y, (const FcChar8 *)value, (int)strlen(value));
  } else {
    const char *value = " unknown";
    XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, value_x,
                      y, (const FcChar8 *)value, (int)strlen(value));
  }
  y += 16;
}

/* select appropriate icon buffer/len for the current battery info */
static void select_icon_for_battery(const BatteryInfo *b,
                                    const unsigned char **out_buf,
                                    size_t *out_len) {
  if (!b || !b->valid) {
    *out_buf = NULL;
    *out_len = 0;
    return;
  }
  const unsigned char *buf = NULL;
  size_t len = 0;
  /* map percentage to buckets */
  int p = (int)b->percentage;
  const char *charging_suffix = (b->state == 1) ? "charging" : NULL;

  if (b->state == 4) {
    buf = gpm_primary_charged_png;
    len = gpm_primary_charged_png_len;
  } else if (p >= 100) {
    buf = (b->state == 1) ? gpm_primary_100_charging_png : gpm_primary_100_png;
    len = (b->state == 1) ? gpm_primary_100_charging_png_len
                          : gpm_primary_100_png_len;
  } else if (p >= 90) {
    buf = (b->state == 1) ? gpm_primary_090_charging_png : gpm_primary_090_png;
    len = (b->state == 1) ? gpm_primary_090_charging_png_len
                          : gpm_primary_090_png_len;
  } else if (p >= 80) {
    buf = (b->state == 1) ? gpm_primary_080_charging_png : gpm_primary_080_png;
    len = (b->state == 1) ? gpm_primary_080_charging_png_len
                          : gpm_primary_080_png_len;
  } else if (p >= 60) {
    buf = (b->state == 1) ? gpm_primary_060_charging_png : gpm_primary_060_png;
    len = (b->state == 1) ? gpm_primary_060_charging_png_len
                          : gpm_primary_060_png_len;
  } else if (p >= 40) {
    buf = (b->state == 1) ? gpm_primary_040_charging_png : gpm_primary_040_png;
    len = (b->state == 1) ? gpm_primary_040_charging_png_len
                          : gpm_primary_040_png_len;
  } else if (p >= 20) {
    buf = (b->state == 1) ? gpm_primary_020_charging_png : gpm_primary_020_png;
    len = (b->state == 1) ? gpm_primary_020_charging_png_len
                          : gpm_primary_020_png_len;
  } else if (p >= 10) {
    buf = (b->state == 1) ? gpm_primary_010_charging_png : gpm_primary_010_png;
    len = (b->state == 1) ? gpm_primary_010_charging_png_len
                          : gpm_primary_010_png_len;
  } else if (p >= 0) {
    buf = (b->state == 1) ? gpm_primary_000_charging_png : gpm_primary_000_png;
    len = (b->state == 1) ? gpm_primary_000_charging_png_len
                          : gpm_primary_000_png_len;
  }
  /* fallback to missing icon if none chosen */
  if (!buf) {
    buf = (b->state == 1) ? gpm_primary_missing_charging_png
                          : gpm_primary_missing_png;
    len = (b->state == 1) ? gpm_primary_missing_charging_png_len
                          : gpm_primary_missing_png_len;
  }
  *out_buf = buf;
  *out_len = len;
}

static void init_background(Ui *app) {
  Pixmap p = load_png_to_pixmap_from_mem(
      app->dpy, RootWindow(app->dpy, app->screen), bg_png, (size_t)bg_png_len,
      &app->bg_w, &app->bg_h);
  if (p) {
    app->bg_pixmap = p;
    XRenderPictFormat *fmt =
        XRenderFindStandardFormat(app->dpy, PictStandardARGB32);
    if (!fmt) {
      printf("Failed to find ARGB32 pict format\n");
      exit(5);
    }
    app->bg_picture =
        XRenderCreatePicture(app->dpy, app->bg_pixmap, fmt, 0, NULL);
    XRenderSetPictureFilter(app->dpy, app->bg_picture, (const char *)"bilinear",
                            NULL, 0);
  } else {
    exit(3);
  }
}

/* update cached icon in UI if needed */
static void ui_update_icon(Ui *ui, const BatteryInfo *b) {
  const unsigned char *buf = NULL;
  size_t len = 0;
  select_icon_for_battery(b, &buf, &len);
  if (!buf || len == 0) {
    if (ui->icon_pix) {
      XFreePixmap(ui->dpy, ui->icon_pix);
      ui->icon_pix = 0;
    }
    ui->icon_buf = NULL;
    ui->icon_len = 0;
    ui->icon_w = ui->icon_h = 0;
    return;
  }
  /* if same buffer already loaded, skip */
  if (ui->icon_buf == buf && ui->icon_len == len)
    return;
  /* free previous */
  if (ui->icon_pix) {
    XFreePixmap(ui->dpy, ui->icon_pix);
    ui->icon_pix = 0;
  }
  unsigned int w = 0, h = 0;
  Pixmap p = load_png_to_pixmap_from_mem(
      ui->dpy, RootWindow(ui->dpy, ui->screen), buf, len, &w, &h);
  if (!p) {
    ui->icon_buf = NULL;
    ui->icon_len = 0;
    ui->icon_w = ui->icon_h = 0;
    ui->icon_pix = 0;
    return;
  }
  ui->icon_pix = p;
  ui->icon_w = w;
  ui->icon_h = h;
  ui->icon_buf = buf;
  ui->icon_len = len;
}

static bool dbus_check(DBusError *err, const char *ctx) {
  if (dbus_error_is_set(err)) {
    fprintf(stderr, "%s: %s: %s\n", ctx, err->name, err->message);
    dbus_error_free(err);
    return false;
  }
  return true;
}

static bool set_brightness_via_service(DBusConnection *conn, int value) {
  if (!conn)
    return false;
  char backlight[128];
  if (!get_backlight_name(backlight, sizeof backlight))
    return false;
  DBusMessage *msg = dbus_message_new_method_call(BRIGHTD_BUS, BRIGHTD_PATH,
                                                  BRIGHTD_IFACE,
                                                  "SetBrightness");
  if (!msg)
    return false;
  const char *name_arg = backlight;
  dbus_message_append_args(msg, DBUS_TYPE_STRING, &name_arg, DBUS_TYPE_INT32,
                           &value, DBUS_TYPE_INVALID);
  DBusError err;
  dbus_error_init(&err);
  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(conn, msg, 2000, &err);
  dbus_message_unref(msg);
  if (!dbus_check(&err, "K16BrightD.SetBrightness")) {
    if (reply)
      dbus_message_unref(reply);
    return false;
  }
  if (!reply)
    return false;
  if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
    const char *err_name = dbus_message_get_error_name(reply);
    if (err_name)
      fprintf(stderr, "K16BrightD.SetBrightness: %s\n", err_name);
    dbus_message_unref(reply);
    return false;
  }
  dbus_message_unref(reply);
  return true;
}

static bool get_display_device_path(DBusConnection *conn, char *out, size_t n) {
  DBusMessage *msg = dbus_message_new_method_call(
      UPOWER_BUS, UPOWER_PATH, UPOWER_IFACE, "GetDisplayDevice");
  if (!msg)
    return false;

  DBusError err;
  dbus_error_init(&err);
  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
  dbus_message_unref(msg);
  if (!dbus_check(&err, "GetDisplayDevice"))
    return false;
  if (!reply)
    return false;

  const char *path = NULL;
  if (!dbus_message_get_args(reply, &err, DBUS_TYPE_OBJECT_PATH, &path,
                             DBUS_TYPE_INVALID)) {
    dbus_check(&err, "GetDisplayDevice args");
    dbus_message_unref(reply);
    return false;
  }
  snprintf(out, n, "%s", path);
  dbus_message_unref(reply);
  return true;
}

static void apply_kv(const char *key, int vtype, DBusMessageIter *var,
                     BatteryInfo *b) {
  if (strcmp(key, "Percentage") == 0 && vtype == DBUS_TYPE_DOUBLE) {
    double d;
    dbus_message_iter_get_basic(var, &d);
    b->percentage = d;
    b->valid = true;
  } else if (strcmp(key, "EnergyRate") == 0 && vtype == DBUS_TYPE_DOUBLE) {
    double d;
    dbus_message_iter_get_basic(var, &d);
    b->energy_rate = d;
    b->valid = true;
  } else if (strcmp(key, "State") == 0 && vtype == DBUS_TYPE_UINT32) {
    uint32_t u;
    dbus_message_iter_get_basic(var, &u);
    b->state = u;
    b->valid = true;
  } else if (strcmp(key, "TimeToEmpty") == 0 &&
             (vtype == DBUS_TYPE_INT64 || vtype == DBUS_TYPE_INT32)) {
    int64_t x = 0;
    if (vtype == DBUS_TYPE_INT64)
      dbus_message_iter_get_basic(var, &x);
    else {
      int32_t t;
      dbus_message_iter_get_basic(var, &t);
      x = t;
    }
    b->tte = x;
  } else if (strcmp(key, "TimeToFull") == 0 &&
             (vtype == DBUS_TYPE_INT64 || vtype == DBUS_TYPE_INT32)) {
    int64_t x = 0;
    if (vtype == DBUS_TYPE_INT64)
      dbus_message_iter_get_basic(var, &x);
    else {
      int32_t t;
      dbus_message_iter_get_basic(var, &t);
      x = t;
    }
    b->ttf = x;
  }
}

static bool fetch_props(DBusConnection *conn, const char *dev_path,
                        BatteryInfo *b) {
  DBusMessage *msg = dbus_message_new_method_call(UPOWER_BUS, dev_path,
                                                  DBUS_PROP_IF, "GetAll");
  if (!msg)
    return false;
  const char *iface = UPOWER_DEV_IF;
  dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);

  DBusError err;
  dbus_error_init(&err);
  DBusMessage *reply =
      dbus_connection_send_with_reply_and_block(conn, msg, -1, &err);
  dbus_message_unref(msg);
  if (!dbus_check(&err, "GetAll"))
    return false;
  if (!reply)
    return false;

  DBusMessageIter it;
  if (!dbus_message_iter_init(reply, &it) ||
      dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
    dbus_message_unref(reply);
    return false;
  }
  DBusMessageIter arr;
  dbus_message_iter_recurse(&it, &arr);
  while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter entry;
    dbus_message_iter_recurse(&arr, &entry);
    const char *key = NULL;
    if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING) {
      dbus_message_iter_next(&arr);
      continue;
    }
    dbus_message_iter_get_basic(&entry, &key);
    dbus_message_iter_next(&entry);
    if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) {
      dbus_message_iter_next(&arr);
      continue;
    }
    DBusMessageIter var;
    dbus_message_iter_recurse(&entry, &var);
    int vtype = dbus_message_iter_get_arg_type(&var);
    apply_kv(key, vtype, &var, b);
    dbus_message_iter_next(&arr);
  }
  dbus_message_unref(reply);
  return b->valid;
}

typedef struct {
  BatteryInfo *b;
  const char *dev_path;
  bool *dirty;
} SignalCtx;

static DBusHandlerResult signal_filter(DBusConnection *c, DBusMessage *m,
                                       void *user) {
  SignalCtx *ctx = (SignalCtx *)user;
  if (!dbus_message_is_signal(m, DBUS_PROP_IF, "PropertiesChanged"))
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const char *path = dbus_message_get_path(m);
  if (!path || strcmp(path, ctx->dev_path) != 0)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  DBusMessageIter it;
  if (!dbus_message_iter_init(m, &it))
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  const char *iface = NULL;
  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  dbus_message_iter_get_basic(&it, &iface);
  if (!iface || strcmp(iface, UPOWER_DEV_IF) != 0)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  dbus_message_iter_next(&it);
  if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  DBusMessageIter changes;
  dbus_message_iter_recurse(&it, &changes);
  while (dbus_message_iter_get_arg_type(&changes) == DBUS_TYPE_DICT_ENTRY) {
    DBusMessageIter entry;
    dbus_message_iter_recurse(&changes, &entry);
    const char *key = NULL;
    dbus_message_iter_get_basic(&entry, &key);
    dbus_message_iter_next(&entry);
    if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) {
      dbus_message_iter_next(&changes);
      continue;
    }
    DBusMessageIter var;
    dbus_message_iter_recurse(&entry, &var);
    int vtype = dbus_message_iter_get_arg_type(&var);
    apply_kv(key, vtype, &var, ctx->b);
    dbus_message_iter_next(&changes);
  }
  *(ctx->dirty) = true;
  return DBUS_HANDLER_RESULT_HANDLED;
}

Window create_argb32_window(Display *dpy, int x, int y, unsigned w,
                            unsigned h) {
  int scr = DefaultScreen(dpy);

  XVisualInfo vinfo;
  if (!XMatchVisualInfo(dpy, scr, 32, TrueColor, &vinfo)) {
    fprintf(stderr, "No 32-bit TrueColor visual on this screen\n");
    return 0;
  }

  /* Verify the visual has an alpha channel for per-pixel opacity */
  XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, vinfo.visual);
  if (!fmt || fmt->type != PictTypeDirect || fmt->direct.alphaMask == 0) {
    fprintf(stderr, "32-bit visual lacks ARGB alpha support\n");
    return 0;
  }

  Colormap cmap =
      XCreateColormap(dpy, RootWindow(dpy, scr), vinfo.visual, AllocNone);

  XSetWindowAttributes swa = {0};
  swa.colormap = cmap;
  swa.border_pixel = 0;
  swa.background_pixel = 0;
  swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask;

  Window win = XCreateWindow(
      dpy, RootWindow(dpy, scr), x, y, w, h,
      0,           // border width
      vinfo.depth, // 32
      InputOutput, vinfo.visual,
      CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &swa);
  XSetWindowBackgroundPixmap(dpy, win, None);

  {
    Atom mwm = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    long hints[5] = {2, 0, 0, 0, 0}; /* flags=2 (decorations), decorations=0 */
    XChangeProperty(dpy, win, mwm, mwm, 32, PropModeReplace,
                    (unsigned char *)hints, 5);
  }

  {
    Atom net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom skip_taskbar = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom skip_pager   = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);

    XEvent e = {0};
    e.xclient.type = ClientMessage;
    e.xclient.message_type = net_wm_state;
    e.xclient.display = dpy;
    e.xclient.window = win;
    e.xclient.format = 32;
    e.xclient.data.l[0] = 1;                 // _NET_WM_STATE_ADD
    e.xclient.data.l[1] = skip_taskbar;
    e.xclient.data.l[2] = skip_pager;
    e.xclient.data.l[3] = 1;                 // source: normal

    XSendEvent(dpy, DefaultRootWindow(dpy), False,
              SubstructureRedirectMask | SubstructureNotifyMask, &e);
  }

  // _NET_WM_PID, so taskbars can identify the process
  {
    Atom net_wm_pid = XInternAtom(dpy, "_NET_WM_PID", False);
    pid_t pid = getpid();
    XChangeProperty(dpy, win, net_wm_pid, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&pid, 1);
  }

  // title (for taskbars that show it)
  {
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
    const char *title = "x11power";
    XChangeProperty(dpy, win, net_wm_name, utf8_string, 8, PropModeReplace,
                    (unsigned char *)title, (int)strlen(title));
  }

  XMapWindow(dpy, win);
  return win;
}

static bool ui_init(Ui *ui) {
  ui->dpy = XOpenDisplay(NULL);
  if (!ui->dpy) {
    fprintf(stderr, "XOpenDisplay failed\n");
    return false;
  }
  ui->screen = DefaultScreen(ui->dpy);
  ui->win_w = 200;
  ui->win_h = 110;
  ui->win = create_argb32_window(ui->dpy, 50, 50, (unsigned)ui->win_w,
                                 (unsigned)ui->win_h);
  XStoreName(ui->dpy, ui->win, "x11power");
  XSelectInput(ui->dpy, ui->win,
               ExposureMask | KeyPressMask | StructureNotifyMask |
                   ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
  ui->gc = XCreateGC(ui->dpy, ui->win, 0, NULL);
  Atom wm_delete = XInternAtom(ui->dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(ui->dpy, ui->win, &wm_delete, 1);
  set_font(ui);
  init_background(ui);
  /* init icon cache */
  ui->icon_pix = 0;
  ui->icon_w = ui->icon_h = 0;
  ui->icon_buf = NULL;
  ui->dragging = 0;
  ui->icon_len = 0;
  ui->win_picture = 0;
  XMapWindow(ui->dpy, ui->win);
  XSync(ui->dpy, False);
  return true;
}

static void send_notification(const char *msg) {
  if (!msg)
    return;
  pid_t pid = fork();
  if (pid < 0) {
    return; // fork failed
  }
  if (pid == 0) {
    // child -> fork again and exit; grandchild execs the notifier
    pid_t pid2 = fork();
    if (pid2 < 0) _exit(1);
    if (pid2 == 0) {
      // grandchild: execute x11notif
      execlp("x11notif", "x11notif", msg, (char *)NULL);
      // if exec fails
      _exit(127);
    }
    // child exits immediately so parent can reap it
    _exit(0);
  }
  // parent: wait for immediate child to avoid zombie
  int status = 0;
  waitpid(pid, &status, 0);
}

static int threshold_15_signalled = 0;
static int threshold_5_signalled = 0;

static void check_and_notify(const BatteryInfo *prev, const BatteryInfo *cur,
                             bool notify_enabled) {
  if (!notify_enabled) return;
  if (!cur || !cur->valid) { printf("Not notifying (1).\n"); return; }
  if (!prev || !prev->valid) { printf("Not notifying (2).\n"); return; }

  char eta[64];

  // If ETA changed from unknown to known, notify about the newly-known time.
  // Unknown ETA is represented by <= 0 in tte/ttf.
  if (prev->valid && cur->valid) {
    // Discharging: TTE became known
    if ((prev->tte <= 0) && (cur->tte > 0) && cur->state == 2) {
      fmt_eta(eta, sizeof eta, cur->state, cur->tte, cur->ttf);
      char msg[256];
      snprintf(msg, sizeof msg, "Battery ETA: %s left (%.1f%%)", eta, cur->percentage);
      send_notification(msg);
    }
    // Charging: TTF became known
    if ((prev->ttf <= 0) && (cur->ttf > 0) && cur->state == 1) {
      fmt_eta(eta, sizeof eta, cur->state, cur->tte, cur->ttf);
      char msg[256];
      snprintf(msg, sizeof msg, "Battery to full: %s (%.1f%%)", eta, cur->percentage);
      send_notification(msg);
    }
  }

  // 1) Low battery thresholds (15% and 5%) when discharging.
  // Shitty, but who's gonna do it better?
  if (cur->percentage <= 15 && threshold_15_signalled == 0) {
    fmt_eta(eta, sizeof eta, cur->state, cur->tte, cur->ttf);
    char msg[256];
    snprintf(msg, sizeof msg, "Battery low: %.0f%% (ETA %s)", cur->percentage, eta);
    send_notification(msg);
    threshold_15_signalled = 1;
  }

  if (cur->percentage <= 5 && threshold_5_signalled == 0) {
    fmt_eta(eta, sizeof eta, cur->state, cur->tte, cur->ttf);
    char msg[256];
    snprintf(msg, sizeof msg, "Battery low: %.0f%% (ETA %s)", cur->percentage, eta);
    send_notification(msg);
    threshold_5_signalled = 1;
  }

  if (cur->percentage > 5) threshold_5_signalled = 0;
  if (cur->percentage > 15) threshold_15_signalled = 0;

  // 2) Fully charged
  if (cur->state == 4 && prev->state != 4) {
    char msg[128];
    snprintf(msg, sizeof msg, "Battery fully charged: %.0f%%", cur->percentage);
    send_notification(msg);
  }

  // 3) Discharging started
  if (cur->state == 2 && prev->state != 2) {
    fmt_eta(eta, sizeof eta, cur->state, cur->tte, cur->ttf);
    char msg[256];
    snprintf(msg, sizeof msg, "Battery discharging: %.1f%% (ETA %s)", cur->percentage, eta);
    send_notification(msg);
  }

  // 4) Charging started
  if (cur->state == 1 && prev->state != 1) {
    fmt_eta(eta, sizeof eta, cur->state, cur->tte, cur->ttf);
    char msg[256];
    snprintf(msg, sizeof msg, "Battery charging: %.1f%% (ETA %s)", cur->percentage, eta);
    send_notification(msg);
  }
}

int main(int argc, char **argv) {
  bool notify_enabled = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--notifications") == 0) {
      notify_enabled = true;
    } else if (strncmp(argv[i], "--powersave-threshold=", 23) == 0) {
      const char *val = argv[i] + 23;
      if (!val[0]) {
        fprintf(stderr, "Empty value for --powersave-threshold\n");
        return 1;
      }
      char *end = NULL;
      double parsed = strtod(val, &end);
      if (end == val) {
        fprintf(stderr, "Invalid value for --powersave-threshold: %s\n", val);
        return 1;
      }
      while (end && *end && isspace((unsigned char)*end))
        ++end;
      if (end && *end == '%') {
        ++end;
      }
      while (end && *end && isspace((unsigned char)*end))
        ++end;
      if (end && *end != '\0') {
        fprintf(stderr, "Unexpected characters in --powersave-threshold: %s\n",
                val);
        return 1;
      }
      if (parsed < 0.0 || parsed > 100.0) {
        fprintf(stderr, "Threshold must be between 0 and 100\n");
        return 1;
      }
      powersave_threshold = parsed;
      powersave_threshold_enabled = true;
    }
  }
  // D-Bus setup
  DBusError err;
  dbus_error_init(&err);
  DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
  if (!dbus_check(&err, "dbus_bus_get") || !conn)
    return 1;

  char dev_path[256] = {0};
  if (!get_display_device_path(conn, dev_path, sizeof dev_path)) {
    fprintf(stderr, "Failed to get DisplayDevice path\n");
    return 1;
  }

  BatteryInfo b = {0};
  CpuInfo cpu = {0};
  BrightnessInfo brightness = {0};
  if (!fetch_props(conn, dev_path, &b)) {
    fprintf(stderr, "Failed to fetch initial properties\n");
    // continue anyway; window will show "No battery data"
  }
  read_cpu_info(&cpu);
  read_brightness(&brightness);
  query_governor_info(&governor_info);

  if (powersave_threshold_enabled) {
    if (handle_powersave_threshold(conn, &b))
      query_governor_info(&governor_info);
  }

  // Subscribe to PropertiesChanged
  char match[512];
  snprintf(match, sizeof match,
           "type='signal',interface='%s',member='PropertiesChanged',path='%s'",
           DBUS_PROP_IF, dev_path);
  dbus_bus_add_match(conn, match, &err);
  dbus_connection_flush(conn);
  if (!dbus_check(&err, "add_match")) { /* keep going without signals */
  }

  bool dirty = true;
  SignalCtx sctx = {.b = &b, .dev_path = dev_path, .dirty = &dirty};
  dbus_connection_add_filter(conn, signal_filter, &sctx, NULL);

  // X11 UI
  Ui ui;
  if (!ui_init(&ui))
    return 1;

  {
    Window root = RootWindow(ui.dpy, ui.screen);
    KeyCode kc_up = XKeysymToKeycode(ui.dpy, XF86XK_MonBrightnessUp);
    KeyCode kc_down = XKeysymToKeycode(ui.dpy, XF86XK_MonBrightnessDown);
    if (kc_up != 0) {
      XGrabKey(ui.dpy, kc_up, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    }
    if (kc_down != 0) {
      XGrabKey(ui.dpy, kc_down, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    }
  }

  BatteryInfo prev = b; // copy initial state to avoid spurious notifications
  struct timespec last_cpu_poll = {0, 0};
  struct timespec last_brightness_poll = {0, 0};
  struct timespec last_governor_poll = {0, 0};

  // Main loop
  struct timespec last_poll = (struct timespec){0, 0};
  const int xfd = ConnectionNumber(ui.dpy);

  for (;;) {
    dbus_connection_read_write_dispatch(conn, 0); // one pass is enough

    while (XPending(ui.dpy)) {
      XEvent e;
      XNextEvent(ui.dpy, &e);
      switch (e.type) {
      case Expose:
        if (e.xexpose.count == 0)
          dirty = true;
        break;
      case ConfigureNotify:
        ui.win_w = e.xconfigure.width;
        ui.win_h = e.xconfigure.height;
        ui.win_x = e.xconfigure.x;
        ui.win_y = e.xconfigure.y;
        if (!ui.dragging)
          dirty = true;
        break;
      case ClientMessage:
        if ((Atom)e.xclient.data.l[0] ==
            XInternAtom(ui.dpy, "WM_DELETE_WINDOW", False)) {
          goto end;
        }
        break;
      case KeyPress: {
        KeySym sym = NoSymbol;
        char text_buf[32];
        int text_len = XLookupString(&e.xkey, text_buf, sizeof text_buf, &sym,
                                     NULL);

        if (sym == XK_Tab || sym == XK_ISO_Left_Tab) {
          printf("Switching edit field\n");
          if (!edit_state.active)
            edit_state_begin(EDIT_FIELD_BRIGHTNESS);
          else
            edit_state_switch();
          dirty = true;
          break;
        }

        if (edit_state.active) {
          bool request_redraw = false;
          EditField active_field = edit_state.field;
          bool handled = edit_handle_key(sym, text_buf, text_len, conn,
                                         &brightness, &request_redraw);
          if (handled) {
            if (request_redraw)
              dirty = true;
            if (!edit_state.active &&
                (sym == XK_Return || sym == XK_KP_Enter)) {
              if (active_field == EDIT_FIELD_GOVERNOR)
                last_governor_poll = (struct timespec){0, 0};
              else if (active_field == EDIT_FIELD_BRIGHTNESS)
                last_brightness_poll = (struct timespec){0, 0};
            }
            break;
          }
          // Ignore unhandled keys while editing.
          break;
        }

        bool handled = false;
        if (sym == XF86XK_MonBrightnessUp)
          handled = adjust_brightness(+1, &brightness, conn);
        else if (sym == XF86XK_MonBrightnessDown)
          handled = adjust_brightness(-1, &brightness, conn);
        if (handled)
          dirty = true;
        break;
      }
      case ButtonPress: {
        if (e.xbutton.button == Button1) {
          ui.dragging = 1;
          ui.drag_off_x = e.xbutton.x_root - ui.win_x;
          ui.drag_off_y = e.xbutton.y_root - ui.win_y;
          /* raise window so dragging is visible */
          XRaiseWindow(ui.dpy, ui.win);
        }
        break;
      }
      case ButtonRelease: {
        if (e.xbutton.button == Button1) {
          ui.dragging = 0;
        }
        break;
      }
      case MotionNotify: {
        while (XPending(ui.dpy)) {
          XEvent next;
          XPeekEvent(ui.dpy, &next);
          if (next.type != MotionNotify)
            break;
          XNextEvent(ui.dpy, &next); // discard intermediate motions
          e = next;
        }
        if (ui.dragging) {
          int nx = e.xmotion.x_root - ui.drag_off_x;
          int ny = e.xmotion.y_root - ui.drag_off_y;
          XMoveWindow(ui.dpy, ui.win, nx, ny);
          ui.win_x = nx;
          ui.win_y = ny;
        }
        break;
      }
      }
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    bool cpu_due = false;
    if (last_cpu_poll.tv_sec == 0 && last_cpu_poll.tv_nsec == 0)
      cpu_due = true;
    else {
      time_t sec = now.tv_sec - last_cpu_poll.tv_sec;
      long nsec = now.tv_nsec - last_cpu_poll.tv_nsec;
      if (sec > 2 || (sec == 2 && nsec >= 0))
        cpu_due = true;
    }
    if (cpu_due) {
      CpuInfo updated = {0};
      read_cpu_info(&updated);
      if (!cpu_info_equal(&cpu, &updated))
        dirty = true;
      cpu = updated;
      last_cpu_poll = now;
    }

    bool brightness_due = false;
    if (last_brightness_poll.tv_sec == 0 && last_brightness_poll.tv_nsec == 0)
      brightness_due = true;
    else {
      time_t sec = now.tv_sec - last_brightness_poll.tv_sec;
      long nsec = now.tv_nsec - last_brightness_poll.tv_nsec;
      if (sec > 2 || (sec == 2 && nsec >= 0))
        brightness_due = true;
    }
    if (brightness_due) {
      BrightnessInfo updated = {0};
      if (read_brightness(&updated)) {
        if (!brightness_equal(&brightness, &updated))
          dirty = true;
        brightness = updated;
      } else {
        if (brightness.valid) {
          BrightnessInfo cleared = {0};
          brightness = cleared;
          dirty = true;
        }
      }
      last_brightness_poll = now;
    }

    bool governor_due = false;
    if (last_governor_poll.tv_sec == 0 && last_governor_poll.tv_nsec == 0)
      governor_due = true;
    else {
      time_t sec = now.tv_sec - last_governor_poll.tv_sec;
      long nsec = now.tv_nsec - last_governor_poll.tv_nsec;
      if (sec > 2 || (sec == 2 && nsec >= 0))
        governor_due = true;
    }
    if (governor_due) {
      GovernorInfo updated = {0};
      query_governor_info(&updated);
      if (!governor_info_equal(&governor_info, &updated)) {
        governor_info = updated;
        dirty = true;
      }
      last_governor_poll = now;
    }

    // Periodic poll fallback every 5s
    if ((now.tv_sec - last_poll.tv_sec) >= 5) {
      if (fetch_props(conn, dev_path, &b))
        dirty = true;
      last_poll = now;
    }

    if (dirty) {
      // Check and send notifications based on transitions/thresholds
      bool governor_changed = handle_powersave_threshold(conn, &b);
      if (governor_changed) {
        GovernorInfo updated = {0};
        query_governor_info(&updated);
        governor_info = updated;
        last_governor_poll = now;
      }
      check_and_notify(&prev, &b, notify_enabled);
      ui_update_icon(&ui, &b);
      ui_draw(&ui, &b, &cpu, &brightness, &governor_info);
      XFlush(ui.dpy);
      // update previous snapshot
      prev = b;
      dirty = false;
    }

    struct pollfd pfd = {.fd = xfd, .events = POLLIN, .revents = 0};
    int rc = poll(&pfd, 1, 1000);
    if (rc < 0) {
      if (errno == EINTR)
        continue;
      perror("poll");
      break;
    }
  }

end:
  if (powersave_threshold_enabled && powersave_active)
    restore_governors(conn);
  return 0;
}
