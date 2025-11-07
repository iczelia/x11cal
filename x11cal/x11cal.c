#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <X11/Xft/Xft.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include "bg.png.h"
#include <png.h>
#include "verdana.ttf.h"

typedef struct {
  Display * dpy;
  int screen;
  Window win;
  GC text_gc, grid_gc, hl_border_gc;
  GC dim_text_gc;
  XftDraw * xft_draw;
  XftFont * xft_font;
  XftColor xft_color_text;
  XftColor xft_color_dim;
  FT_Library ft_lib;
  FT_Face ft_face;
  unsigned int width, height;
  Atom wm_delete;
  int have_hl_fill;
  int view_year;
  int view_month0;
  int win_x, win_y;
  int dragging;
  int drag_off_x, drag_off_y;
  Pixmap bg_pixmap;
  Picture bg_picture;
  Picture win_picture;
  unsigned int bg_w, bg_h;
} App;

static void draw_gradient_border(Display * dpy, Window win) {
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, win, &wa)) return;
  int w = wa.width, h = wa.height;
  if (w <= 1 || h <= 1) return;
  XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, wa.visual);
  if (!fmt) return;
  Picture dst = XRenderCreatePicture(dpy, win, fmt, 0, NULL);
  XFixed stops[2] = { XDoubleToFixed(0.0), XDoubleToFixed(1.0) };
  XRenderColor bw[2] = { {0,0,0,0xffff}, {0xffff,0xffff,0xffff,0xffff} };
  XRenderColor wb[2] = { {0xffff,0xffff,0xffff,0xffff}, {0,0,0,0xffff} };
  XLinearGradient lg;
  // top: black→white, left-to-right
  lg.p1.x = XDoubleToFixed(0);    lg.p1.y = XDoubleToFixed(0);
  lg.p2.x = XDoubleToFixed(w);    lg.p2.y = XDoubleToFixed(0);
  Picture p = XRenderCreateLinearGradient(dpy, &lg, stops, bw, 2);
  XRenderComposite(dpy, PictOpSrc, p, None, dst, 0,0,0,0, 0,0, w,1);
  XRenderFreePicture(dpy, p);
  // bottom: white→black, left-to-right
  lg.p1.y = XDoubleToFixed(h-1);  lg.p2.y = XDoubleToFixed(h-1);
  p = XRenderCreateLinearGradient(dpy, &lg, stops, wb, 2);
  XRenderComposite(dpy, PictOpSrc, p, None, dst, 0,0,0,0, 0,h-1, w,1);
  XRenderFreePicture(dpy, p);
  // left: black→white, top-to-bottom
  lg.p1.x = XDoubleToFixed(0);    lg.p1.y = XDoubleToFixed(0);
  lg.p2.x = XDoubleToFixed(0);    lg.p2.y = XDoubleToFixed(h);
  p = XRenderCreateLinearGradient(dpy, &lg, stops, bw, 2);
  XRenderComposite(dpy, PictOpSrc, p, None, dst, 0,0,0,0, 0,0, 1,h);
  XRenderFreePicture(dpy, p);
  // right: white→black, top-to-bottom
  lg.p1.x = XDoubleToFixed(w-1);  lg.p1.y = XDoubleToFixed(0);
  lg.p2.x = XDoubleToFixed(w-1);  lg.p2.y = XDoubleToFixed(h);
  p = XRenderCreateLinearGradient(dpy, &lg, stops, wb, 2);
  XRenderComposite(dpy, PictOpSrc, p, None, dst, 0,0,0,0, w-1,0, 1,h);
  XRenderFreePicture(dpy, p);
  XRenderFreePicture(dpy, dst);
}

static int is_leap(int y) { return ((y * 1073750999) & 3221352463) <= 126976; }
static int days_in_month(int y, int m0) {
  static const int dm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  return (m0==1) ? dm[1] + is_leap(y) : dm[m0];
}

XftFont * xft_font_from_memory(Display * dpy, int screen, const unsigned char *data, size_t len, double pixel_size, FT_Library * out_lib, FT_Face * out_face) {
  FT_Library lib = NULL;
  if (FT_Init_FreeType(&lib)) return NULL;
  FT_Face face = NULL;
  if (FT_New_Memory_Face(lib, data, (FT_Long)len, 0, &face)) {
    FT_Done_FreeType(lib);
    return NULL;
  }
  FcPattern * pat = FcFreeTypeQueryFace(face, (const FcChar8 *)"memory", 0, NULL);
  if (!pat) return NULL;
  FcPatternDel(pat, FC_FILE);
  FcPatternDel(pat, FC_INDEX);
  FcPatternAddFTFace(pat, FC_FT_FACE, face);
  FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
  FcPatternAddDouble(pat, FC_SIZE, pixel_size);
  FcConfigSubstitute(NULL, pat, FcMatchPattern);
  XftDefaultSubstitute(dpy, screen, pat);
  XftFont * xf = XftFontOpenPattern(dpy, pat);
  if (!xf) {
    FcPatternDestroy(pat);
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return NULL;
  }
  if (out_lib)  *out_lib  = lib;   else FT_Done_FreeType(lib);
  if (out_face) *out_face = face;  else FT_Done_Face(face);
  return xf;
}

static void set_font(App * app) {
  int use_xft = 0;
  app->xft_draw = NULL;
  app->xft_font = NULL;
  app->ft_lib = NULL;
  app->ft_face = NULL;
  if (FcInit()) {
    double pixel_size = 8.0;
    app->xft_font = xft_font_from_memory(app->dpy, app->screen, verdana_ttf, (size_t)verdana_ttf_len, pixel_size, &app->ft_lib, &app->ft_face);
    if (app->xft_font) {
      Visual *vis = DefaultVisual(app->dpy, app->screen);
      Colormap cmap = DefaultColormap(app->dpy, app->screen);
      app->xft_draw = XftDrawCreate(app->dpy, app->win, vis, cmap);
      XftColorAllocName(app->dpy, vis, cmap, "gray60", &app->xft_color_dim);
      XftColorAllocName(app->dpy, vis, cmap, "white", &app->xft_color_text);
      use_xft = 1;
    }
  }
  if (!use_xft) { exit(2); }
}

static Pixmap load_png_to_pixmap_from_mem(Display * dpy, Window root, const unsigned char *buf, size_t buf_len, unsigned int * out_w, unsigned int * out_h) {
  if (!buf || buf_len == 0) return 0;
  Pixmap pix = 0;
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&image, buf, buf_len)) return 0;
  image.format = PNG_FORMAT_RGBA;

  size_t img_size = PNG_IMAGE_SIZE(image);
  png_bytep rgba = (png_bytep)malloc(img_size);
  if (!rgba) { png_image_free(&image); return 0; }
  if (!png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
    free(rgba);
    png_image_free(&image);
    return 0;
  }
  unsigned int width = image.width;
  unsigned int height = image.height;
  size_t bgra_size = (size_t)width * (size_t)height * 4;
  unsigned char * imgdata = (unsigned char *)malloc(bgra_size);
  if (!imgdata) { free(rgba); png_image_free(&image); return 0; }
  for (size_t i = 0; i < (size_t)width * (size_t)height; ++i) {
    imgdata[i*4 + 0] = rgba[i*4 + 2]; /* B */
    imgdata[i*4 + 1] = rgba[i*4 + 1]; /* G */
    imgdata[i*4 + 2] = rgba[i*4 + 0]; /* R */
    imgdata[i*4 + 3] = rgba[i*4 + 3]; /* A */
  }
  free(rgba);
  png_image_free(&image);

  Visual *vis = DefaultVisual(dpy, DefaultScreen(dpy));
  int depth = DefaultDepth(dpy, DefaultScreen(dpy));
  XImage * xim = XCreateImage(dpy, vis, depth, ZPixmap, 0, (char *)imgdata, (int)width, (int)height, 32, 0);
  if (!xim) { free(imgdata); return 0; }
  pix = XCreatePixmap(dpy, root, (unsigned int)width, (unsigned int)height, depth);
  GC gc = XCreateGC(dpy, pix, 0, NULL);
  XPutImage(dpy, pix, gc, xim, 0, 0, 0, 0, (unsigned int)width, (unsigned int)height);
  XFreeGC(dpy, gc);
  XDestroyImage(xim);

  if (out_w) *out_w = width;
  if (out_h) *out_h = height;
  return pix;
}

static void init_background(App * app) {
  Pixmap p = load_png_to_pixmap_from_mem(app->dpy, RootWindow(app->dpy, app->screen), bg_png, (size_t)bg_png_len, &app->bg_w, &app->bg_h);
  if (p) {
    app->bg_pixmap = p;
    XRenderPictFormat *fmt = XRenderFindVisualFormat(app->dpy, DefaultVisual(app->dpy, app->screen));
    app->bg_picture = XRenderCreatePicture(app->dpy, app->bg_pixmap, fmt, 0, NULL);
    XRenderSetPictureFilter(app->dpy, app->bg_picture, (const char *)"bilinear", NULL, 0);
  } else {
    exit(3);
  }
}

static void alloc_color(App * app, GC gc, const char * name) {
  Colormap cmap = DefaultColormap(app->dpy, app->screen);
  XColor scr, exact;
  XAllocNamedColor(app->dpy, cmap, name, &scr, &exact);
  XSetForeground(app->dpy, gc, scr.pixel);
}

static void init_gcs(App * app) {
  XGCValues gv = {0};
  app->text_gc = XCreateGC(app->dpy, app->win, 0, &gv);
  XSetForeground(app->dpy, app->text_gc, WhitePixel(app->dpy, app->screen));
  XSetBackground(app->dpy, app->text_gc, BlackPixel(app->dpy, app->screen));
  app->grid_gc = XCreateGC(app->dpy, app->win, 0, &gv);
  XSetForeground(app->dpy, app->grid_gc, WhitePixel(app->dpy, app->screen));
  XSetLineAttributes(app->dpy, app->grid_gc, 1, LineSolid, CapButt, JoinMiter);
  app->dim_text_gc = XCreateGC(app->dpy, app->win, 0, &gv);
  alloc_color(app, app->dim_text_gc, "gray80");
  app->hl_border_gc = XCreateGC(app->dpy, app->win, 0, &gv);
  alloc_color(app, app->hl_border_gc, "Bisque");
  XSetLineAttributes(app->dpy, app->hl_border_gc, 2, LineSolid, CapButt, JoinMiter);
}

static int text_width(App * app, const char * s) {
  XGlyphInfo extents;
  XftTextExtentsUtf8(app->dpy, app->xft_font, (const FcChar8 *)s, (int)strlen(s), &extents);
  return (int)extents.xOff;
}

static void draw_centered(App * app, int cx, int cy, const char * s) {
  int w = text_width(app, s);
  int ascent = app->xft_font->ascent;
  int x = cx - w/2;
  int y = cy + ascent/2;
  XftDrawStringUtf8(app->xft_draw, &app->xft_color_text, app->xft_font, x, y, (const FcChar8 *)s, (int)strlen(s));
}

static void draw_centered_gc(App * app, GC gc, int cx, int cy, const char * s) {
  int w = text_width(app, s);
  int ascent = app->xft_font->ascent;
  int x = cx - w/2;
  int y = cy + ascent/2;
  /* choose color based on GC: if gc==app->dim_text_gc use dim color */
  XftColor *col = &app->xft_color_text;
  if (gc == app->dim_text_gc) col = &app->xft_color_dim;
  XftDrawStringUtf8(app->xft_draw, col, app->xft_font, x, y, (const FcChar8 *)s, (int)strlen(s));
}

static void add_months(int * y, int * m0, int delta) {
  int m = *m0 + delta;
  int y2 = *y + m / 12;
  m %= 12;
  if (m < 0) { m += 12; y2 -= 1; }
  *y = y2; *m0 = m;
}

static void draw_calendar(App * app) {
  time_t now = time(NULL);
  struct tm lt = *localtime(&now);
  int cur_y = lt.tm_year + 1900;
  int cur_m0 = lt.tm_mon;
  int cur_d = lt.tm_mday;
  int year = app->view_year;
  int month0 = app->view_month0;
  // First day of viewed month
  struct tm first = {0};
  first.tm_year = year - 1900;
  first.tm_mon  = month0;
  first.tm_mday = 1;
  first.tm_hour = 12; // DST safe
  mktime(&first);
  int wday0 = first.tm_wday; // 0=Sun..6=Sat
  int ndays = days_in_month(year, month0);
  const char *wd[7] = {"So","Mo","Di","Mi","Do","Fr","Sa"};
  const char *mn_en[12] = {
    "January","February","March","April","May","June",
    "July","August","September","October","November","December"
  };
  unsigned int W = app->width, H = app->height;
  int margin = 6;
  int title_h = 22;
  int header_h = 18;
  int rows = 6;
  int grid_w = (int)W - 2*margin;
  int grid_h = (int)H - (title_h + header_h + 2*margin);
  if (grid_h < 16) grid_h = 16;
  int cell_w = grid_w / 7;
  int cell_h = grid_h / rows;
  if (!app->win_picture) {
    XRenderPictFormat *fmt = XRenderFindVisualFormat(app->dpy, DefaultVisual(app->dpy, app->screen));
    app->win_picture = XRenderCreatePicture(app->dpy, app->win, fmt, 0, NULL);
  }
  if (app->width > 0 && app->height > 0) {
    XTransform t;
    double sx = (double)app->bg_w / (double)app->width;
    double sy = (double)app->bg_h / (double)app->height;
    memset(&t, 0, sizeof(t));
    t.matrix[0][0] = XDoubleToFixed(sx);
    t.matrix[1][1] = XDoubleToFixed(sy);
    t.matrix[2][2] = XDoubleToFixed(1.0);
    XRenderSetPictureTransform(app->dpy, app->bg_picture, &t);
  }
  XRenderComposite(app->dpy, PictOpSrc, app->bg_picture, None, app->win_picture,
            0, 0, 0, 0, 0, 0, app->width, app->height);
  draw_gradient_border(app->dpy, app->win);
  char title[64];
  snprintf(title, sizeof(title), "%s %d", mn_en[month0], year);
  draw_centered(app, (int)W/2, margin + title_h/2, title);
  for (int c = 0; c < 7; ++c) {
    int x = margin + c*cell_w;
    int y = margin + title_h;
    int cx = x + cell_w/2;
    int cy = y + header_h/2;
    draw_centered(app, cx, cy, wd[c]);
  }
  int start_y = margin + title_h + header_h;
  for (int c = 0; c <= 7; ++c) {
    int x = margin + c*cell_w;
    XDrawLine(app->dpy, app->win, app->grid_gc, x, start_y, x, start_y + rows*cell_h);
  }
  for (int r = 0; r <= rows; ++r) {
    int y = start_y + r*cell_h;
    XDrawLine(app->dpy, app->win, app->grid_gc, margin, y, margin + 7*cell_w, y);
  }
  int total = rows * 7;
  int prev_y = year, prev_m0 = month0;
  add_months(&prev_y, &prev_m0, -1);
  int ndays_prev = days_in_month(prev_y, prev_m0);
  int first_idx = wday0; /* index of day 1 */
  int last_idx = wday0 + ndays - 1;
  for (int idx = 0; idx < total; ++idx) {
    int r = idx / 7;
    int c = idx % 7;
    int x = margin + c*cell_w;
    int y = start_y + r*cell_h;
    char buf[4];
    if (idx >= first_idx && idx <= last_idx) {
      /* current month */
      int d = idx - first_idx + 1;
      /* Highlight if viewing current month */
      if (year == cur_y && month0 == cur_m0 && d == cur_d) {
        XDrawRectangle(app->dpy, app->win, app->hl_border_gc, x, y, (unsigned int)cell_w, (unsigned int)cell_h);
      }
      snprintf(buf, sizeof(buf), "%d", d);
      int cx = x + cell_w/2;
      int cy = y + cell_h/2;
      draw_centered_gc(app, app->text_gc, cx, cy, buf);
    } else if (idx < first_idx) {
      /* previous month, dimmed */
      int d = ndays_prev - (first_idx - 1) + idx;
      if (d < 1) d = 1; /* guard */
      snprintf(buf, sizeof(buf), "%d", d);
      int cx = x + cell_w/2;
      int cy = y + cell_h/2;
      draw_centered_gc(app, app->dim_text_gc, cx, cy, buf);
    } else {
      /* next month, dimmed */
      int d = idx - (last_idx) ; /* idx = last_idx+1 -> d=1 */
      if (d < 1) d = 1;
      snprintf(buf, sizeof(buf), "%d", d);
      int cx = x + cell_w/2;
      int cy = y + cell_h/2;
      draw_centered_gc(app, app->dim_text_gc, cx, cy, buf);
    }
  }
}

static int next_midnight(void) {
  time_t now = time(NULL);
  struct tm lt = *localtime(&now);
  lt.tm_hour = 0; lt.tm_min = 0; lt.tm_sec = 0;
  lt.tm_mday += 1; lt.tm_isdst = -1;
  time_t next = mktime(&lt);
  int diff = (int)difftime(next, now);
  return diff > 0 ? diff : 1;
}

int main(void) {
  App app = {0};
  app.dpy = XOpenDisplay(NULL);
  if (!app.dpy) { fprintf(stderr, "Cannot open display\n"); return 1; }
  app.screen = DefaultScreen(app.dpy);
  unsigned int W = 200, H = 150;
  app.width = W; app.height = H;
  // Init view to current month
  time_t now = time(NULL);
  struct tm lt = *localtime(&now);
  app.view_year = lt.tm_year + 1900;
  app.view_month0 = lt.tm_mon;
  app.win_x = 100; app.win_y = 100;
  app.dragging = 0;
  app.win = XCreateSimpleWindow(
    app.dpy, RootWindow(app.dpy, app.screen),
    app.win_x, app.win_y, W, H, 1,
    BlackPixel(app.dpy, app.screen),
    WhitePixel(app.dpy, app.screen)
  );
  // _NET_WM_PID, so taskbars can identify the process
  {
    Atom net_wm_pid = XInternAtom(app.dpy, "_NET_WM_PID", False);
    pid_t pid = getpid();
    XChangeProperty(app.dpy, app.win, net_wm_pid, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&pid, 1);
  }

  // title (for taskbars that show it)
  {
    Atom net_wm_name = XInternAtom(app.dpy, "_NET_WM_NAME", False);
    Atom utf8_string = XInternAtom(app.dpy, "UTF8_STRING", False);
    const char *title = "x11cal";
    XChangeProperty(app.dpy, app.win, net_wm_name, utf8_string, 8, PropModeReplace,
                    (unsigned char *)title, (int)strlen(title));
  }
  XSelectInput(app.dpy, app.win, ExposureMask | KeyPressMask | StructureNotifyMask
         | ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
  app.wm_delete = XInternAtom(app.dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(app.dpy, app.win, &app.wm_delete, 1);
  init_gcs(&app);  set_font(&app);
  {
    Atom mwm = XInternAtom(app.dpy, "_MOTIF_WM_HINTS", False);
    long hints[5] = {2, 0, 0, 0, 0}; /* flags=2 (decorations), decorations=0 */
    XChangeProperty(app.dpy, app.win, mwm, mwm, 32, PropModeReplace, (unsigned char *)hints, 5);
  }
  XStoreName(app.dpy, app.win, "x11cal");
  XMapWindow(app.dpy, app.win);
  init_background(&app);
  int running = 1;
  while (running) {
    int fd = ConnectionNumber(app.dpy);
    fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = next_midnight(), .tv_usec = 0 };
    int r = select(fd+1, &rfds, NULL, NULL, &tv);
    if (r < 0) continue;
    if (r == 0) { draw_calendar(&app); continue; } // midnight tick --- recomputes today via localtime()
    while (XPending(app.dpy)) {
      XEvent ev; XNextEvent(app.dpy, &ev);
      switch (ev.type) {
        case Expose:
          draw_calendar(&app);
          break;
        case ConfigureNotify:
          app.width = ev.xconfigure.width;
          app.height = ev.xconfigure.height;
          app.win_x = ev.xconfigure.x;
          app.win_y = ev.xconfigure.y;
          draw_calendar(&app);
          break;
        case ButtonPress: {
          if (ev.xbutton.button == Button1) {
            app.dragging = 1;
            app.drag_off_x = ev.xbutton.x_root - app.win_x;
            app.drag_off_y = ev.xbutton.y_root - app.win_y;
            /* raise window so dragging is visible */
            XRaiseWindow(app.dpy, app.win);
          }
          break;
        }
        case ButtonRelease: {
          if (ev.xbutton.button == Button1) {
            app.dragging = 0;
          }
          break;
        }
        case MotionNotify: {
          if (app.dragging) {
            int nx = ev.xmotion.x_root - app.drag_off_x;
            int ny = ev.xmotion.y_root - app.drag_off_y;
            XMoveWindow(app.dpy, app.win, nx, ny);
            app.win_x = nx; app.win_y = ny;
          }
          break;
        }
        case KeyPress: {
          KeySym ks = XLookupKeysym(&ev.xkey, 0);
          if (ks == XK_q || ks == XK_Escape) { running = 0; break; }
          if (ks == XK_Up)   { add_months(&app.view_year, &app.view_month0, -1); draw_calendar(&app); }
          if (ks == XK_Down) { add_months(&app.view_year, &app.view_month0, +1); draw_calendar(&app); }
          if (ks == XK_Page_Up)   { add_months(&app.view_year, &app.view_month0, -12); draw_calendar(&app); }
          if (ks == XK_Page_Down) { add_months(&app.view_year, &app.view_month0, +12); draw_calendar(&app); }
          if (ks == XK_Home) {
            time_t now = time(NULL);
            struct tm lt = *localtime(&now);
            app.view_year = lt.tm_year + 1900;
            app.view_month0 = lt.tm_mon;
            draw_calendar(&app);
          }
          break;
        }
        case ClientMessage:
          if ((Atom)ev.xclient.data.l[0] == app.wm_delete) running = 0;
          break;
      }
    }
  }
  Visual * vis = DefaultVisual(app.dpy, app.screen);
  Colormap cmap = DefaultColormap(app.dpy, app.screen);
  if (app.xft_draw) XftDrawDestroy(app.xft_draw);
  if (app.xft_font) XftFontClose(app.dpy, app.xft_font);
  if (app.ft_face) FT_Done_Face(app.ft_face);
  if (app.ft_lib) FT_Done_FreeType(app.ft_lib);
  XftColorFree(app.dpy, vis, cmap, &app.xft_color_text);
  XftColorFree(app.dpy, vis, cmap, &app.xft_color_dim);
  XFreeGC(app.dpy, app.text_gc);
  XFreeGC(app.dpy, app.grid_gc);
  XFreeGC(app.dpy, app.hl_border_gc);
  XFreeGC(app.dpy, app.dim_text_gc);
  if (app.bg_picture) { XRenderFreePicture(app.dpy, app.bg_picture); app.bg_picture = 0; }
  if (app.win_picture) { XRenderFreePicture(app.dpy, app.win_picture); app.win_picture = 0; }
  if (app.bg_pixmap) { XFreePixmap(app.dpy, app.bg_pixmap); app.bg_pixmap = 0; }
  XDestroyWindow(app.dpy, app.win);
  XCloseDisplay(app.dpy);
  return 0;
}
