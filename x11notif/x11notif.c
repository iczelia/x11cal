#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>
#include <locale.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <X11/extensions/Xinerama.h>

static void nsleep(long ns) {
  struct timespec req = { ns / 1000000000L, ns % 1000000000L };
  while (nanosleep(&req, &req) == -1) {}
}

static Region make_parallelogram_region(int w, int h, int slant) {
  XPoint pts[4];
  pts[0].x = slant; pts[0].y = 0;
  pts[1].x = w;   pts[1].y = 0;
  pts[2].x = w - slant; pts[2].y = h;
  pts[3].x = 0;   pts[3].y = h;
  return XPolygonRegion(pts, 4, EvenOddRule);
}

static void draw_slanted_stripe(Display *dpy, Drawable win, GC gc, int w, int h, int slant, int stripe_w, unsigned long pixel) {
  XSetForeground(dpy, gc, pixel);
  XPoint p[4];
  p[0].x = slant;      p[0].y = 0;
  p[1].x = slant + stripe_w; p[1].y = 0;
  p[2].x = stripe_w;     p[2].y = h;
  p[3].x = 0;        p[3].y = h;
  XFillPolygon(dpy, win, gc, p, 4, Convex, CoordModeOrigin);
}

static inline XRenderColor rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
  XRenderColor c;
  c.red   = (unsigned short)(r * 257u);
  c.green = (unsigned short)(g * 257u);
  c.blue  = (unsigned short)(b * 257u);
  c.alpha = (unsigned short)(a * 257u);
  return c;
}

static Picture linear_grad(Display *dpy, double x1, double y1, double x2, double y2, const XRenderColor *cols, const double *ofs, int n) {
  XLinearGradient grad;
  grad.p1.x = XDoubleToFixed(x1); grad.p1.y = XDoubleToFixed(y1);
  grad.p2.x = XDoubleToFixed(x2); grad.p2.y = XDoubleToFixed(y2);
  XFixed *stops = (XFixed*)calloc((size_t)n, sizeof(XFixed));
  for (int i = 0; i < n; ++i) stops[i] = XDoubleToFixed(ofs[i]);
  Picture p = XRenderCreateLinearGradient(dpy, &grad, stops, (XRenderColor*)cols, n);
  free(stops);
  return p;
}

static void paint_background(Display *dpy, Window win, int screen, int w, int h) {
  // Colors from spec
  const unsigned char dl_r=0x13, dl_g=0x16, dl_b=0x34; // #131634
  const unsigned char pu_r=0x60, pu_g=0x60, pu_b=0x9a; // #60609a

  XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
  Picture dst = XRenderCreatePicture(dpy, win, fmt, 0, NULL);

  {
    XRenderColor base = rgba(pu_r, pu_g, pu_b, 255);
    Picture solid = XRenderCreateSolidFill(dpy, &base);
    XRenderComposite(dpy, PictOpSrc, solid, None, dst, 0,0,0,0, 0,0, w,h);
    XRenderFreePicture(dpy, solid);
  }

  {
    XRenderColor c[3] = {
      rgba(dl_r, dl_g, dl_b, 255), // at TL
      rgba(dl_r, dl_g, dl_b,   0), // mid transparent
      rgba(dl_r, dl_g, dl_b, 255)  // at BR
    };
    double t[3] = {0.0, 0.5, 1.0};
    Picture p = linear_grad(dpy, 0, 0, w, h, c, t, 3);
    XRenderComposite(dpy, PictOpOver, p, None, dst, 0,0,0,0, 0,0, w,h);
    XRenderFreePicture(dpy, p);
  }

  {
    double cx = w * 0.5, cy = h * 0.5;
    double nx = -(double)h, ny = (double)w; // normal to TL→BR
    double k = 1.2; // extend past bounds
    double x1 = cx - k*nx, y1 = cy - k*ny;
    double x2 = cx + k*nx, y2 = cy + k*ny;

    XRenderColor c[5] = {
      rgba(dl_r, dl_g, dl_b,   0),
      rgba(dl_r, dl_g, dl_b,   0),
      rgba(dl_r, dl_g, dl_b, 255),
      rgba(dl_r, dl_g, dl_b,   0),
      rgba(dl_r, dl_g, dl_b,   0)
    };
    double t[5] = {0.0, 0.49, 0.50, 0.51, 1.0};
    Picture p = linear_grad(dpy, x1, y1, x2, y2, c, t, 5);
    XRenderComposite(dpy, PictOpOver, p, None, dst, 0,0,0,0, 0,0, w,h);
    XRenderFreePicture(dpy, p);
  }

  {
    const unsigned char hi_r=0xC4, hi_g=0xC4, hi_b=0xE1; // #c4c4e1
    int h = 8;

    XRenderColor col[3] = {
      rgba(dl_r, dl_g, dl_b, 255),   // #131634 at start
      rgba(hi_r, hi_g, hi_b, 255),   // highlight at 1/3
      rgba(dl_r, dl_g, dl_b, 255)  // back to #131634
    };
    double s[3] = {0.0, 0.33, 1.0};
    Picture src = linear_grad(dpy, 0, 0, w, h, col, s, 3);

    double nx = -(double)h, ny = (double)w;
    double nlen = hypot(nx, ny);
    double k = 1.2;                   // extend past bounds
    double cx = 0.5*w, cy = 0.5*h;
    double x1 = cx - k*nx, y1 = cy - k*ny;
    double x2 = cx + k*nx, y2 = cy + k*ny;

    double stroke_px = (h >= 64 ? 6.0 : 4.0);
    double half = stroke_px / (4.0 * k * nlen);

    XRenderColor maskc[5] = {
      rgba(0,0,0,0),
      rgba(0,0,0,0),
      rgba(0,0,0,255),   // solid center
      rgba(0,0,0,0),
      rgba(0,0,0,0)
    };
    double mt[5] = {0.5 - 2*half, 0.5 - half, 0.5, 0.5 + half, 0.5 + 2*half};
    Picture mask = linear_grad(dpy, x1, y1, x2, y2, maskc, mt, 5);

    XRenderComposite(dpy, PictOpOver, src, mask, dst, 0,0, 0,0, 0,0, w, h);

    XRenderFreePicture(dpy, src);
    XRenderFreePicture(dpy, mask);
  }

  XRenderFreePicture(dpy, dst);
}

static void draw_notification(Display *dpy, Window win, int w, int h, int slant, const char *msg, const char *fontname) {
  int screen = DefaultScreen(dpy);
  GC gc = XCreateGC(dpy, win, 0, NULL);

  paint_background(dpy, win, screen, w, h);

  XColor col_scr, col_accent;
  Colormap cmap = DefaultColormap(dpy, screen);
  XAllocNamedColor(dpy, cmap, "#131634", &col_accent, &col_scr);

  int stripe_w = slant / 2; if (stripe_w < 10) stripe_w = 10;
  draw_slanted_stripe(dpy, win, gc, w, h, slant, stripe_w, col_accent.pixel);

  XftDraw *draw = XftDrawCreate(dpy, win, DefaultVisual(dpy, screen), cmap);
  XftColor xft_fg, xft_shadow;
  XRenderColor xr_fg = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF }; // white
  XRenderColor xr_shadow = { 0x0000, 0x0000, 0x0000, 0x7FFF }; // semi black

  XftColorAllocValue(dpy, DefaultVisual(dpy, screen), cmap, &xr_shadow, &xft_shadow);
  XftColorAllocValue(dpy, DefaultVisual(dpy, screen), cmap, &xr_fg, &xft_fg);

  XftFont *font = XftFontOpenName(dpy, screen, fontname ? fontname : "Sans:bold:pixelsize=20");
  if (!font) font = XftFontOpenName(dpy, screen, "Sans:pixelsize=20");

  XGlyphInfo ext;
  XftTextExtentsUtf8(dpy, font, (const FcChar8*)msg, (int)strlen(msg), &ext);

  int padding = 48;
  int text_x = stripe_w + padding + slant / 6;
  if (text_x + (int)ext.width > w - padding) text_x = w - padding - (int)ext.width;
  if (text_x < padding) text_x = padding;

  int baseline = (h + font->ascent - font->descent) / 2;

  XftDrawStringUtf8(draw, &xft_shadow, font, text_x + 1, baseline + 1, (const FcChar8*)msg, (int)strlen(msg));
  XftDrawStringUtf8(draw, &xft_fg, font, text_x, baseline, (const FcChar8*)msg, (int)strlen(msg));

  XftFontClose(dpy, font);
  XftColorFree(dpy, DefaultVisual(dpy, screen), cmap, &xft_fg);
  XftColorFree(dpy, DefaultVisual(dpy, screen), cmap, &xft_shadow);
  XftDrawDestroy(draw);

  XFreeGC(dpy, gc);
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "");

  const char *msg = (argc >= 2) ? argv[1] : "Hello, world!";
  const char *fontname = getenv("X11NOTIF_FONT");

  /* Named semaphore to serialize notifications across processes. The
   * name can be overridden with X11NOTIF_SEMNAME; default is
   * "/x11notif_sem". If we cannot open the semaphore we continue
   * without serialization. */
  const char *semname = getenv("X11NOTIF_SEMNAME");
  if (!semname) semname = "/x11notif_sem";
  sem_t *notif_sem = sem_open(semname, O_CREAT, 0644, 1);
  if (notif_sem == SEM_FAILED) {
    perror("sem_open");
    notif_sem = NULL;
  } else {
    /* Acquire (decrement) the semaphore; this will block until the
     * previous notifier posts. On EINTR retry. On other errors give
     * up serialization and continue. */
    while (sem_wait(notif_sem) == -1) {
      if (errno == EINTR) continue;
      perror("sem_wait");
      notif_sem = NULL;
      break;
    }
  }

  Display *dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "Cannot open display.");
    return 1;
  }

  int screen = DefaultScreen(dpy);
  int sw = DisplayWidth(dpy, screen);
  int sh = DisplayHeight(dpy, screen);

  // Multi-monitor support via Xinerama
  int nmon = 0;
  XineramaScreenInfo *mons = NULL;
  int mons_from_xinerama = 0;
  {
    int xev, xerr;
    if (XineramaQueryExtension(dpy, &xev, &xerr) && XineramaIsActive(dpy)) {
      mons = XineramaQueryScreens(dpy, &nmon);
      mons_from_xinerama = (mons != NULL);
    }
  }
  if (nmon <= 0) {
    // fallback: single monitor covering the default screen
    nmon = 1;
    mons = (XineramaScreenInfo *)calloc(1, sizeof(XineramaScreenInfo));
    mons[0].x_org = 0;
    mons[0].y_org = 0;
    mons[0].width = sw;
    mons[0].height = sh;
  }

  int margin_y = 32;  // distance from top of screen
  int w = sw;       // full screen width
  int h = 48;       // notification height
  int slant = 32;     // pixels of skew on top edge

  if (h > sh) h = sh / 4;
  if (slant < 0) slant = 0; if (slant > w/2) slant = w/2;

  XSetWindowAttributes attrs;
  attrs.override_redirect = True;
  attrs.backing_store = WhenMapped;
  attrs.save_under = True;
  attrs.event_mask = ExposureMask;

  Window *wins = (Window*)calloc((size_t)nmon, sizeof(Window));

  for (int i = 0; i < nmon; ++i) {
    int mw = mons[i].width;
    int mh = mons[i].height;
    int mx = mons[i].x_org;
    int my = mons[i].y_org;

    int ww = mw; // full monitor width
    if (h > mh) h = mh / 4; // ensure height fits monitor
    if (slant < 0) slant = 0; if (slant > ww/2) slant = ww/2;

    wins[i] = XCreateWindow(
      dpy, RootWindow(dpy, screen),
      mx - ww, my + margin_y,
      (unsigned int)ww, (unsigned int)h, 0,
      CopyFromParent, InputOutput, CopyFromParent,
      CWOverrideRedirect | CWBackingStore | CWSaveUnder | CWEventMask,
      &attrs);

    int shape_event_base, shape_error_base;
    if (XShapeQueryExtension(dpy, &shape_event_base, &shape_error_base)) {
      Region r = make_parallelogram_region(ww, h, slant);
      XShapeCombineRegion(dpy, wins[i], ShapeBounding, 0, 0, r, ShapeSet);
      Region empty = XCreateRegion();
      XShapeCombineRegion(dpy, wins[i], ShapeInput, 0, 0, empty, ShapeSet);
      XDestroyRegion(empty);
      XDestroyRegion(r);
    }

    XMapRaised(dpy, wins[i]);
    draw_notification(dpy, wins[i], ww, h, slant, msg, fontname);
  }
  XFlush(dpy);

  const int fps = 30;
  const long frame_ns = 1000000000L / fps;
  const int slide_in_ms = 300;
  const int hold_ms = 2000;
  const int slide_out_ms = 300;

  int frames_in = slide_in_ms * fps / 1000;
  int frames_out = slide_out_ms * fps / 1000;

  // Per-monitor animation targets
  int *mon_x = (int*)calloc((size_t)nmon, sizeof(int));
  int *mon_w = (int*)calloc((size_t)nmon, sizeof(int));
  int *mon_y = (int*)calloc((size_t)nmon, sizeof(int));
  for (int i = 0; i < nmon; ++i) {
    mon_x[i] = mons[i].x_org;
    mon_w[i] = mons[i].width;
    mon_y[i] = mons[i].y_org + margin_y;
  }

  // Slide in from left with ease-out cubic
  for (int i = 0; i <= frames_in; ++i) {
    double t = (double)i / (double)frames_in; // 0..1
    double te = 1.0 - (1.0 - t)*(1.0 - t)*(1.0 - t);
    for (int m = 0; m < nmon; ++m) {
      int ww = mon_w[m];
      int start_x = mon_x[m] - ww;
      int x = (int)(start_x + te * (ww)); // moves from left off-screen to monitor.x_org
      XMoveWindow(dpy, wins[m], x, mon_y[m]);
    }
    XFlush(dpy);
    nsleep(frame_ns);
  }

  // Hold
  int hold_frames = hold_ms * fps / 1000;
  for (int i = 0; i < hold_frames; ++i) {
#ifdef DO_REPAINT
    if (i % fps == 0) {
      for (int m = 0; m < nmon; ++m) {
        draw_notification(dpy, wins[m], mon_w[m], h, slant, msg, fontname);
      }
    }
    XFlush(dpy);
#else
    nsleep(frame_ns);
#endif
  }

  // Slide out to the right with ease-in cubic
  for (int i = 0; i <= frames_out; ++i) {
    double t = (double)i / (double)frames_out; // 0..1
    double te = t*t*t;
    for (int m = 0; m < nmon; ++m) {
      int start = mon_x[m];
      int x = (int)(start + te * (mon_w[m])); // move fully off-screen to the right of this monitor
      XMoveWindow(dpy, wins[m], x, mon_y[m]);
    }
    XFlush(dpy);
    nsleep(frame_ns);
  }
  for (int m = 0; m < nmon; ++m) {
    XUnmapWindow(dpy, wins[m]);
    XDestroyWindow(dpy, wins[m]);
  }

  if (mons) {
    if (mons_from_xinerama) XFree(mons);
    else free(mons);
  }
  free(wins);
  free(mon_x);
  free(mon_w);
  free(mon_y);

  if (notif_sem) {
    if (sem_post(notif_sem) == -1) perror("sem_post");
    if (sem_close(notif_sem) == -1) perror("sem_close");
  }

  XCloseDisplay(dpy);
  return 0;
}
