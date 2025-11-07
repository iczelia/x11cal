#define _GNU_SOURCE
#include "bg.png.h"
#include "verdana.ttf.h"
#include "da.png.h"
#include "bg-add.png.h"
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrender.h>
#include <X11/keysym.h>
#include <png.h>

#include <freetype2/freetype/freetype.h>
#include <ft2build.h>
#include <fontconfig/fcfreetype.h>
#include <fontconfig/fontconfig.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// hack
#define ENTRIES_FILE "/etc/xlunch/entries.dsv"
#define MAX_APPS 512
#define MAX_LABEL 256
#define FILTER_MAX 128

#define ICON_SIZE 48
#define CELL_PAD_W 80
#define CELL_PAD_H 24
#define CELL_PAD_TOP 8

#define ANIM_STEPS 20
#define ANIM_TOTAL_MS 200

typedef struct {
  char name[MAX_LABEL];
  char icon_path[512];
  char cmd[512];
  Pixmap icon_pixmap;
  Picture icon_picture;
  unsigned icon_w, icon_h;
  int valid;
} App;

typedef struct {
  Window win;
  int x, y, w, h;
  XftFont *xft_font;
  XftDraw *xft_draw;
  XftColor xft_color_text;
  XftColor xft_color_dim;
  FT_Library ft_lib;
  FT_Face ft_face;
  Picture win_picture;
  Pixmap bg_pixmap;
  Picture bg_picture;
  unsigned bg_w, bg_h;
} Overlay;

static Display *dpy;
static int screen;
static Window root;
static Visual *visual;
static Colormap colormap;
static int depth;

static Overlay *overlays = NULL;
static int overlay_count = 0;

static App apps[MAX_APPS];
static int app_count = 0;

static char filter_text[FILTER_MAX] = {0};
static int filter_len = 0;

static int selected_index = 0;
static int scroll_row = 0;
static int overlay_visible = 0;

/* shared background, loaded once */
static Pixmap shared_bg_pixmap = 0;
static Picture shared_bg_picture = 0;
static unsigned shared_bg_w = 0, shared_bg_h = 0;

/* selection bitmap (da.png.h) */
static Pixmap shared_sel_pixmap = 0;
static Picture shared_sel_picture = 0;
static unsigned shared_sel_w = 0, shared_sel_h = 0;

/* corner decoration (bg-add.png.h) */
static Pixmap shared_add_pixmap = 0;
static Picture shared_add_picture = 0;
static unsigned shared_add_w = 0, shared_add_h = 0;

static void animate_shrink(void) {
  if (!overlay_visible || !overlays || overlay_count == 0)
    return;
  const int steps = ANIM_STEPS;
  const int sleep_us = (ANIM_TOTAL_MS * 1000) / steps;
  for (int s = 0; s < steps; ++s) {
    double f = 1.0 - (double)(s + 1) / (double)steps;
    for (int o = 0; o < overlay_count; ++o) {
      Overlay *ov = &overlays[o];
      if (!ov || !ov->win)
        continue;
      /* shrink around center */
      int nw = (int)(ov->w * f);
      int nh = (int)(ov->h * f);
      if (nw < 20) nw = 20;
      if (nh < 20) nh = 20;
      int nx = ov->x + (ov->w - nw) / 2;
      int ny = ov->y + (ov->h - nh) / 2;
      XMoveResizeWindow(dpy, ov->win, nx, ny, nw, nh);
    }
    XSync(dpy, False);
    usleep(sleep_us);
  }
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

static XftFont *xft_font_from_memory(Display *dpy, int screen,
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

static Pixmap load_png_to_pixmap_from_file(Display *dpy, Drawable root,
                                           const char *path, unsigned *out_w,
                                           unsigned *out_h) {
  FILE *fp = fopen(path, "rb");
  if (!fp)
    return 0;
  png_image im;
  memset(&im, 0, sizeof im);
  im.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_stdio(&im, fp)) {
    fclose(fp);
    return 0;
  }
  im.format = PNG_FORMAT_RGBA;
  size_t sz = PNG_IMAGE_SIZE(im);
  png_bytep rgba = malloc(sz);
  if (!rgba) {
    png_image_free(&im);
    fclose(fp);
    return 0;
  }
  if (!png_image_finish_read(&im, NULL, rgba, 0, NULL)) {
    free(rgba);
    png_image_free(&im);
    fclose(fp);
    return 0;
  }
  fclose(fp);

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
  xi->byte_order = ImageByteOrder(dpy);

  GC gc = XCreateGC(dpy, pix, 0, NULL);
  XPutImage(dpy, pix, gc, xi, 0, 0, 0, 0, w, h);
  XFreeGC(dpy, gc);
  XDestroyImage(xi);

  if (out_w)
    *out_w = w;
  if (out_h)
    *out_h = h;
  return pix;
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
    r = (uint8_t)((r * (uint16_t)a + 127) / 255);
    g = (uint8_t)((g * (uint16_t)a + 127) / 255);
    b = (uint8_t)((b * (uint16_t)a + 127) / 255);
    argb[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
              (uint32_t)b;
  }
  free(rgba);
  png_image_free(&im);

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
  xi->byte_order = ImageByteOrder(dpy);

  GC gc = XCreateGC(dpy, pix, 0, NULL);
  XPutImage(dpy, pix, gc, xi, 0, 0, 0, 0, w, h);
  XFreeGC(dpy, gc);
  XDestroyImage(xi);

  if (out_w)
    *out_w = w;
  if (out_h)
    *out_h = h;
  return pix;
}

static Picture picture_from_pixmap(Pixmap pix) {
  XRenderPictFormat *fmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);
  if (!fmt)
    return 0;
  return XRenderCreatePicture(dpy, pix, fmt, 0, NULL);
}

static void trim_newline(char *s) {
  char *p = strchr(s, '\n');
  if (p)
    *p = 0;
}

static void parse_dsv_line(const char *line, char *name, char *icon,
                           char *cmd) {
  name[0] = icon[0] = cmd[0] = 0;
  char tmp[1024];
  strncpy(tmp, line, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;
  trim_newline(tmp);

  int field = 0;
  char *tok = tmp;
  for (char *c = tmp; ; ++c) {
    if (*c == ';' || *c == 0) {
      char save = *c;
      *c = 0;
      if (field == 0)
        strncpy(name, tok, MAX_LABEL - 1);
      else if (field == 1)
        strncpy(icon, tok, 511);
      else if (field == 2)
        strncpy(cmd, tok, 511);
      field++;
      if (save == 0)
        break;
      tok = c + 1;
    }
  }
  name[MAX_LABEL - 1] = 0;
  icon[511] = 0;
  cmd[511] = 0;

  if (name[0] == '"') {
    size_t l = strlen(name);
    if (l > 1 && name[l-1] == '"') {
      name[l-1] = 0;
      memmove(name, name+1, l-1);
    }
  }
  if (cmd[0] == '"') {
    size_t l = strlen(cmd);
    if (l > 1 && cmd[l-1] == '"') {
      cmd[l-1] = 0;
      memmove(cmd, cmd+1, l-1);
    }
  }
}

static void load_apps_from_file(void) {
  FILE *f = fopen(ENTRIES_FILE, "r");
  if (!f)
    return;
  char line[1024];
  while (fgets(line, sizeof(line), f) && app_count < MAX_APPS) {
    if (line[0] == '#' || line[0] == 0)
      continue;
    char name[MAX_LABEL], icon[512], cmd[512];
    parse_dsv_line(line, name, icon, cmd);
    if (!name[0] || !cmd[0])
      continue;
    App *a = &apps[app_count];
    memset(a, 0, sizeof(*a));
    strncpy(a->name, name, sizeof(a->name) - 1);
    strncpy(a->icon_path, icon, sizeof(a->icon_path) - 1);
    strncpy(a->cmd, cmd, sizeof(a->cmd) - 1);
    if (icon[0]) {
      a->icon_pixmap = load_png_to_pixmap_from_file(dpy, root, icon,
                                                    &a->icon_w, &a->icon_h);
      if (a->icon_pixmap)
        a->icon_picture = picture_from_pixmap(a->icon_pixmap);
    }
    a->valid = 1;
    app_count++;
  }
  fclose(f);
}

static void set_font(Overlay *ov) {
  if (!FcInit())
    exit(2);
  ov->xft_font =
      xft_font_from_memory(dpy, screen, verdana_ttf, (size_t)verdana_ttf_len,
                           10.0, &ov->ft_lib, &ov->ft_face);
  if (!ov->xft_font)
    exit(2);
  XWindowAttributes wa;
  XGetWindowAttributes(dpy, ov->win, &wa);
  ov->xft_draw = XftDrawCreate(dpy, ov->win, wa.visual, wa.colormap);
  if (!ov->xft_draw)
    exit(2);
  if (!XftColorAllocName(dpy, wa.visual, wa.colormap, "white",
                         &ov->xft_color_text))
    exit(2);
  if (!XftColorAllocName(dpy, wa.visual, wa.colormap, "gray80",
                         &ov->xft_color_dim))
    exit(2);
  XRenderPictFormat *dst_fmt = XRenderFindVisualFormat(dpy, visual);
  if (dst_fmt)
    ov->win_picture = XRenderCreatePicture(dpy, ov->win, dst_fmt, 0, NULL);
  ov->bg_pixmap = shared_bg_pixmap;
  ov->bg_picture = shared_bg_picture;
  ov->bg_w = shared_bg_w;
  ov->bg_h = shared_bg_h;
}

static void create_overlays(void) {
  int event_base, error_base;
  int screens = 1;
  XineramaScreenInfo *info = NULL;
  if (XineramaQueryExtension(dpy, &event_base, &error_base) &&
      XineramaIsActive(dpy)) {
    info = XineramaQueryScreens(dpy, &screens);
  }

  int min_w = DisplayWidth(dpy, screen);
  int min_h = DisplayHeight(dpy, screen);
  if (info) {
    min_w = info[0].width;
    min_h = info[0].height;
    for (int i = 1; i < screens; ++i) {
      if (info[i].width < min_w)
        min_w = info[i].width;
      if (info[i].height < min_h)
        min_h = info[i].height;
    }
  }

  overlay_count = screens;
  overlays = calloc(screens, sizeof(Overlay));
  for (int i = 0; i < screens; ++i) {
    int sw = (info ? info[i].width : DisplayWidth(dpy, screen));
    int sh = (info ? info[i].height : DisplayHeight(dpy, screen));
    int sx = (info ? info[i].x_org : 0);
    int sy = (info ? info[i].y_org : 0);
    int w = (int)(min_w * 0.5);  /* was 0.3 */
    int h = (int)(min_h * 0.5);  /* was 0.3 */
    int x = sx + (sw - w) / 2;
    int y = sy + (sh - h) / 2;

    XSetWindowAttributes attr = {0};
    attr.override_redirect = True;
    attr.colormap = colormap;
    attr.border_pixel = 0;
    attr.background_pixel = 0;
    attr.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask;
    Window win =
        XCreateWindow(dpy, root, x, y, w, h, 0, depth, InputOutput, visual,
                      CWOverrideRedirect | CWColormap | CWBorderPixel |
                          CWBackPixel | CWEventMask,
                      &attr);
    XSetWindowBackgroundPixmap(dpy, win, None);

    overlays[i].win = win;
    overlays[i].x = x;
    overlays[i].y = y;
    overlays[i].w = w;
    overlays[i].h = h;

    set_font(&overlays[i]);
  }
  if (info)
    XFree(info);
}

static void show_overlays(void) {
  create_overlays();
  for (int i = 0; i < overlay_count; ++i)
    XMapRaised(dpy, overlays[i].win);
  XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);
  overlay_visible = 1;
  selected_index = 0;
  scroll_row = 0;
}

static void destroy_overlays(void) {
  for (int i = 0; i < overlay_count; ++i) {
    Overlay *ov = &overlays[i];
    if (ov->win_picture) {
      XRenderFreePicture(dpy, ov->win_picture);
      ov->win_picture = 0;
    }
    if (ov->xft_draw) {
      XftDrawDestroy(ov->xft_draw);
      ov->xft_draw = NULL;
    }
    if (ov->xft_font) {
      XftFontClose(dpy, ov->xft_font);
      ov->xft_font = NULL;
    }
    if (ov->ft_face) {
      FT_Done_Face(ov->ft_face);
      ov->ft_face = NULL;
    }
    if (ov->ft_lib) {
      FT_Done_FreeType(ov->ft_lib);
      ov->ft_lib = NULL;
    }
    if (ov->win) {
      XDestroyWindow(dpy, ov->win);
      ov->win = 0;
    }
  }
  free(overlays);
  overlays = NULL;
  overlay_count = 0;
  overlay_visible = 0;
}

static void hide_overlays(void) {
  XUngrabKeyboard(dpy, CurrentTime);
  filter_len = 0;  filter_text[0] = 0;
  selected_index = 0;
  scroll_row = 0;
  destroy_overlays();
}

static void grab_ctrl_r(void) {
  KeyCode kc = XKeysymToKeycode(dpy, XK_r);
  XGrabKey(dpy, kc, ControlMask, root, True,
             GrabModeAsync, GrabModeAsync);
}

static void ungrab_ctrl_r(void) {
  KeyCode kc = XKeysymToKeycode(dpy, XK_r);
  XUngrabKey(dpy, kc, ControlMask, root);
}

static void reset_filter(void) {
  filter_text[0] = 0;
  filter_len = 0;
}

static int strcasestr_simple(const char *hay, const char *needle) {
  if (!needle[0])
    return 1;
  size_t hl = strlen(hay);
  size_t nl = strlen(needle);
  for (size_t i = 0; i + nl <= hl; ++i) {
    size_t j = 0;
    for (; j < nl; ++j) {
      char c1 = tolower((unsigned char)hay[i + j]);
      char c2 = tolower((unsigned char)needle[j]);
      if (c1 != c2)
        break;
    }
    if (j == nl)
      return 1;
  }
  return 0;
}

static int build_matches(int *out) {
  int c = 0;
  for (int i = 0; i < app_count; ++i) {
    if (!apps[i].valid)
      continue;
    if (strcasestr_simple(apps[i].name, filter_text) ||
        strcasestr_simple(apps[i].cmd, filter_text)) {
      out[c++] = i;
    }
  }
  return c;
}

static void launch_app(const char *cmd) {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }
  animate_shrink();
  hide_overlays();
}

static void draw_label_truncated_centered(Overlay *ov, const char *text,
                                          int cell_x, int cell_w,
                                          int baseline_y) {
  char buf[256];
  XGlyphInfo ext;
  int maxw = cell_w - 12;
  XftTextExtentsUtf8(dpy, ov->xft_font, (FcChar8 *)text, strlen(text), &ext);
  if (ext.width <= maxw) {
    int x = cell_x + (cell_w - ext.width) / 2;
    XftDrawStringUtf8(ov->xft_draw, &ov->xft_color_text, ov->xft_font, x,
                      baseline_y, (FcChar8 *)text, strlen(text));
    return;
  }
  const char *ellipsis = "...";
  int ell_len = 3;
  int len = strlen(text);
  for (int n = len; n > 0; --n) {
    if (n + ell_len >= (int)sizeof(buf))
      continue;
    memcpy(buf, text, n);
    memcpy(buf + n, ellipsis, ell_len);
    buf[n + ell_len] = 0;
    XftTextExtentsUtf8(dpy, ov->xft_font, (FcChar8 *)buf, n + ell_len, &ext);
    if (ext.width <= maxw) {
      int x = cell_x + (cell_w - ext.width) / 2;
      XftDrawStringUtf8(ov->xft_draw, &ov->xft_color_text, ov->xft_font, x,
                        baseline_y, (FcChar8 *)buf, n + ell_len);
      return;
    }
  }
}

static void ensure_selection_visible(int *match_indices, int match_count,
                                     int cols, int visible_rows) {
  int pos = 0;
  for (int i = 0; i < match_count; ++i) {
    if (match_indices[i] == selected_index) {
      pos = i;
      break;
    }
  }
  int row = pos / cols;
  if (row < scroll_row)
    scroll_row = row;
  if (row >= scroll_row + visible_rows)
    scroll_row = row - visible_rows + 1;
  if (scroll_row < 0)
    scroll_row = 0;
}

static void draw_overlay_contents(void) {
  if (!overlay_visible)
    return;

  int match_indices[MAX_APPS];
  int match_count = build_matches(match_indices);
  if (match_count == 0) {
    selected_index = -1;
  } else {
    int found = 0;
    for (int i = 0; i < match_count; ++i) {
      if (match_indices[i] == selected_index) {
        found = 1;
        break;
      }
    }
    if (!found)
      selected_index = match_indices[0];
  }

  for (int o = 0; o < overlay_count; ++o) {
    Overlay *ov = &overlays[o];

    if (ov->bg_picture && ov->win_picture && ov->bg_w && ov->bg_h) {
      XTransform tr;
      double sx = (double)ov->bg_w / (double)ov->w;
      double sy = (double)ov->bg_h / (double)ov->h;
      memset(&tr, 0, sizeof(tr));
      tr.matrix[0][0] = XDoubleToFixed(sx);
      tr.matrix[1][1] = XDoubleToFixed(sy);
      tr.matrix[2][2] = XDoubleToFixed(1.0);
      XRenderSetPictureTransform(dpy, ov->bg_picture, &tr);
      XRenderSetPictureFilter(dpy, ov->bg_picture, "bilinear", NULL, 0);
      XRenderComposite(dpy, PictOpSrc, ov->bg_picture, None, ov->win_picture,
                       0, 0, 0, 0, 0, 0, ov->w, ov->h);
    }

    /* draw bottom-right decoration above bg but below everything else */
    if (shared_add_picture && shared_add_w && shared_add_h && ov->win_picture) {
      int margin = 5;
      int dest_w = (int)shared_add_w;
      int dest_h = (int)shared_add_h;
      int dest_x = ov->w - dest_w - margin;
      int dest_y = ov->h - dest_h - margin;
      if (dest_x < 0) dest_x = 0;
      if (dest_y < 0) dest_y = 0;
      /* no scaling: draw at native size */
      XRenderComposite(dpy, PictOpOver,
                       shared_add_picture, None,
                       ov->win_picture,
                       0, 0, 0, 0,
                       dest_x, dest_y,
                       dest_w, dest_h);
    }

    int margin = 20;
    int top = margin + ov->xft_font->ascent + 10;
    char filterline[256];
    snprintf(filterline, sizeof(filterline), "filter: %s", filter_text);
    XftDrawStringUtf8(ov->xft_draw, &ov->xft_color_text, ov->xft_font, margin,
                      margin + ov->xft_font->ascent,
                      (FcChar8 *)filterline, strlen(filterline));

    int cell_w = ICON_SIZE + CELL_PAD_W;
    int cell_h = ICON_SIZE + ov->xft_font->ascent + ov->xft_font->descent + CELL_PAD_H;
    int cols = (ov->w - 2 * margin) / cell_w;
    if (cols < 1)
      cols = 1;
    int rows_visible = (ov->h - top - margin) / cell_h;
    if (rows_visible < 1)
      rows_visible = 1;

    if (match_count > 0)
      ensure_selection_visible(match_indices, match_count, cols, rows_visible);

    for (int i = 0; i < rows_visible * cols; ++i) {
      int global_pos = (scroll_row * cols) + i;
      if (global_pos >= match_count)
        break;
      int app_idx = match_indices[global_pos];
      App *a = &apps[app_idx];
      int row = i / cols;
      int col = i % cols;
      int cx = margin + col * cell_w;
      int cy = top + row * cell_h;

      if (app_idx == selected_index && shared_sel_picture &&
          shared_sel_w && shared_sel_h && ov->win_picture) {
        XTransform tr;
        double sx = (double)shared_sel_w / (double)(cell_w);
        double sy = (double)shared_sel_h / (double)(cell_h);
        memset(&tr, 0, sizeof(tr));
        tr.matrix[0][0] = XDoubleToFixed(sx);
        tr.matrix[1][1] = XDoubleToFixed(sy);
        tr.matrix[2][2] = XDoubleToFixed(1.0);
        XRenderSetPictureTransform(dpy, shared_sel_picture, &tr);
        XRenderSetPictureFilter(dpy, shared_sel_picture, "bilinear", NULL, 0);
        XRenderComposite(dpy, PictOpOver,
                         shared_sel_picture, None,
                         ov->win_picture,
                         0, 0, 0, 0,
                         cx, cy,
                         cell_w, cell_h);
      }

      /* center icon horizontally */
      int icon_x = cx + (cell_w - ICON_SIZE) / 2;
      int icon_y = cy + CELL_PAD_TOP;
      if (a->icon_picture && a->icon_w && a->icon_h && ov->win_picture) {
        XTransform tr;
        double sx = (double)a->icon_w / (double)ICON_SIZE;
        double sy = (double)a->icon_h / (double)ICON_SIZE;
        memset(&tr, 0, sizeof(tr));
        tr.matrix[0][0] = XDoubleToFixed(sx);
        tr.matrix[1][1] = XDoubleToFixed(sy);
        tr.matrix[2][2] = XDoubleToFixed(1.0);
        XRenderSetPictureTransform(dpy, a->icon_picture, &tr);
        XRenderSetPictureFilter(dpy, a->icon_picture, "best", NULL, 0);
        XRenderComposite(dpy, PictOpOver, a->icon_picture, None,
                         ov->win_picture, 0, 0, 0, 0,
                         icon_x, icon_y, ICON_SIZE, ICON_SIZE);
      } else {
        XGlyphInfo qext;
        XftTextExtentsUtf8(dpy, ov->xft_font, (FcChar8 *)"?", 1, &qext);
        int qx = cx + (cell_w - qext.width) / 2;
        int qy = icon_y + ov->xft_font->ascent;
        XftDrawStringUtf8(ov->xft_draw, &ov->xft_color_dim, ov->xft_font,
                          qx, qy, (FcChar8 *)"?", 1);
      }

      /* center label horizontally under icon */
      int label_y = icon_y + ICON_SIZE + ov->xft_font->ascent + 2 + CELL_PAD_TOP;
      draw_label_truncated_centered(ov, a->name, cx, cell_w, label_y);
    }

    int total_rows = (match_count + cols - 1) / cols;
    if (total_rows > rows_visible) {
      int bar_h = (rows_visible * (ov->h - top - margin)) / total_rows;
      if (bar_h < 20)
        bar_h = 20;
      int bar_y =
          top + (scroll_row * (ov->h - top - margin - bar_h)) /
                    (total_rows - rows_visible);
      XRenderColor sc = {0xffff, 0xffff, 0xffff, 0x9999};
      if (ov->win_picture)
        XRenderFillRectangle(dpy, PictOpOver, ov->win_picture, &sc,
                             ov->w - margin / 2, bar_y, 4, bar_h);
    }

    draw_gradient_border(ov->w, ov->h, dpy, ov->win);

    XFlush(dpy);
  }

  if (match_count == 1) {
    int idx = match_indices[0];
    launch_app(apps[idx].cmd);
  }
}

static void move_selection(int dx, int dy) {
  int match_indices[MAX_APPS];
  int match_count = build_matches(match_indices);
  if (match_count == 0)
    return;

  Overlay *ov = &overlays[0];
  int margin = 20;
  int top = margin + ov->xft_font->ascent + 10;
  int cell_w = ICON_SIZE + CELL_PAD_W;
  int cell_h = ICON_SIZE + ov->xft_font->ascent + ov->xft_font->descent + CELL_PAD_H;
  int cols = (ov->w - 2 * margin) / cell_w;
  if (cols < 1)
    cols = 1;
  int rows_visible = (ov->h - top - margin) / cell_h;
  if (rows_visible < 1)
    rows_visible = 1;

  int pos = 0;
  for (int i = 0; i < match_count; ++i) {
    if (match_indices[i] == selected_index) {
      pos = i;
      break;
    }
  }
  int total_rows = (match_count + cols - 1) / cols;
  int row = pos / cols;
  int col = pos % cols;

  int new_row = row + dy;
  int new_col = col + dx;

  if (new_col < 0)
    new_col = 0;
  if (new_col >= cols)
    new_col = cols - 1;

  if (new_row < 0) {
    if (scroll_row > 0) {
      scroll_row--;
      new_row = scroll_row;
    } else {
      new_row = 0;
    }
  } else if (new_row >= total_rows) {
    new_row = total_rows - 1;
  } else if (new_row >= scroll_row + rows_visible) {
    scroll_row = new_row - rows_visible + 1;
  } else if (new_row < scroll_row) {
    scroll_row = new_row;
  }

  int new_pos = new_row * cols + new_col;
  if (new_pos >= match_count)
    new_pos = match_count - 1;
  selected_index = match_indices[new_pos];
  draw_overlay_contents();
}

int main(int argc, char **argv) {
  dpy = XOpenDisplay(NULL);
  if (!dpy)
    _exit(1);

  screen = DefaultScreen(dpy);
  root = RootWindow(dpy, screen);

  XVisualInfo vinfo;
  if (!XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo)) {
    fprintf(stderr, "No 32-bit TrueColor visual\n");
    exit(1);
  }
  XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, vinfo.visual);
  if (!fmt || fmt->type != PictTypeDirect || fmt->direct.alphaMask == 0) {
    fprintf(stderr, "32-bit visual lacks ARGB\n");
    exit(1);
  }
  visual = vinfo.visual;
  depth = vinfo.depth;
  colormap = XCreateColormap(dpy, root, visual, AllocNone);

  shared_bg_pixmap = load_png_to_pixmap_from_mem(
      dpy, root, bg_png, sizeof(bg_png), &shared_bg_w, &shared_bg_h);
  if (shared_bg_pixmap)
    shared_bg_picture = picture_from_pixmap(shared_bg_pixmap);


  /* load selection bitmap (da.png.h) */
  shared_sel_pixmap = load_png_to_pixmap_from_mem(
      dpy, root, da_png, sizeof(da_png), &shared_sel_w, &shared_sel_h);
  if (shared_sel_pixmap)
    shared_sel_picture = picture_from_pixmap(shared_sel_pixmap);

  /* load corner decoration (bg-add.png.h) */
  shared_add_pixmap = load_png_to_pixmap_from_mem(
      dpy, root, bg_add_png, sizeof(bg_add_png),
      &shared_add_w, &shared_add_h);
  if (shared_add_pixmap)
    shared_add_picture = picture_from_pixmap(shared_add_pixmap);

  load_apps_from_file();
  
  grab_ctrl_r();

  for (;;) {
    XEvent ev;
    XNextEvent(dpy, &ev);
    if (ev.type == Expose) {
      if (overlay_visible)
        draw_overlay_contents();
    } else if (ev.type == KeyPress) {
      XKeyEvent *ke = &ev.xkey;
      KeySym ks = XLookupKeysym(ke, 0);
      if (!overlay_visible) {
        if (ks == XK_r && (ke->state & ControlMask)) {
          show_overlays();
          draw_overlay_contents();
        }
        continue;
      }

      if (ks == XK_Escape) {
        /* hide overlays and return to waiting for the hotkey */
        animate_shrink();
        hide_overlays();
        continue;
      } else if (ks == XK_Return) {
        int match_indices[MAX_APPS];
        int match_count = build_matches(match_indices);
        if (match_count > 0) {
          int idx = selected_index;
          launch_app(apps[idx].cmd);
        }
      } else if (ks == XK_Tab) {
        int match_indices[MAX_APPS];
        int match_count = build_matches(match_indices);
        if (match_count > 0) {
          int pos = 0;
          for (int i = 0; i < match_count; ++i) {
            if (match_indices[i] == selected_index) {
              pos = i;
              break;
            }
          }
          pos = (pos + 1) % match_count;
          selected_index = match_indices[pos];
          draw_overlay_contents();
        }
      } else if (ks == XK_Left) {
        move_selection(-1, 0);
      } else if (ks == XK_Right) {
        move_selection(+1, 0);
      } else if (ks == XK_Up) {
        move_selection(0, -1);
      } else if (ks == XK_Down) {
        move_selection(0, +1);
      } else if (ks == XK_BackSpace) {
        if (filter_len > 0) {
          filter_text[--filter_len] = 0;
          draw_overlay_contents();
        }
      } else {
        char buf[8];
        int n = XLookupString(ke, buf, sizeof(buf), NULL, NULL);
        if (n > 0) {
          int changed = 0;
          for (int i = 0; i < n; ++i) {
            if (filter_len < FILTER_MAX - 1 && buf[i] >= 32 && buf[i] < 127) {
              filter_text[filter_len++] = buf[i];
              changed = 1;
            }
          }
          if (changed) {
            filter_text[filter_len] = 0;
            draw_overlay_contents();
          }
        }
      }
    }
  }

  return 0;
}
