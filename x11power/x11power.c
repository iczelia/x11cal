
// Headers
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>

#include <ft2build.h>
#include FT_FREETYPE_H

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
#include <sys/types.h>
#include <sys/wait.h>


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

typedef struct {
  double percentage;  // %
  double energy_rate; // W
  uint32_t state;     // enum
  int64_t tte;        // seconds
  int64_t ttf;        // seconds
  bool valid;
} BatteryInfo;

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

static void ui_draw(Ui *ui, const BatteryInfo *b) {
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
  if (!b->valid) {
    const char *msg = "No battery data";
    XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, 12, 18,
                      (const FcChar8 *)msg, (int)strlen(msg));
    return;
  }
  char l2[128], l3[128], l4[128];
  fmt_eta(l2, sizeof l2, b->state, b->tte, b->ttf);
  snprintf(l3, sizeof l3, "%s; %.1f%%", state_str(b->state), b->percentage);
  snprintf(l4, sizeof l4, "%s, %.2f W", l2, b->energy_rate);
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
  XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, text_x, y,
                    (const FcChar8 *)l3, (int)strlen(l3));
  y += 16;
  XftDrawStringUtf8(ui->xft_draw, &ui->xft_color_text, ui->xft_font, text_x, y,
                    (const FcChar8 *)l4, (int)strlen(l4));
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
  ui->win_h = 45;
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
  if (!fetch_props(conn, dev_path, &b)) {
    fprintf(stderr, "Failed to fetch initial properties\n");
    // continue anyway; window will show "No battery data"
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

  BatteryInfo prev = b; // copy initial state to avoid spurious notifications

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

    if (dirty) {
      // Check and send notifications based on transitions/thresholds
      check_and_notify(&prev, &b, notify_enabled);

      ui_update_icon(&ui, &b);
      ui_draw(&ui, &b);
      XFlush(ui.dpy);
      // update previous snapshot
      prev = b;
      dirty = false;
    }

    // Periodic poll fallback every 5s
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((now.tv_sec - last_poll.tv_sec) >= 5) {
      if (fetch_props(conn, dev_path, &b))
        dirty = true;
      last_poll = now;
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
  return 0;
}
