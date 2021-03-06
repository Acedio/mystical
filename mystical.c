/* mystical, Copyright (c) 2020 Josh Simmons <josh.simmons@gmail.com>
 *
 * A screensaver inspired by the classic "Mystify Your Mind" from Windows.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation.  No representations are made about the suitability of this
 * software for any purpose.  It is provided "as is" without express or 
 * implied warranty.
 */

#include "screenhack.h"

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
#include "xdbe.h"
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

#define NCOLORS 1024

#define max(a,b) ((a)>(b)?(a):(b))

struct shape {
  /* Number of points in each polygon. Note that we include enough room in the
   * array for a duplicate of the first point to make draw calls easier. For
   * example, if npoints = 4, then there are actually 5 XPoints per polygon
   * array. */
  int npoints;
  /* Number of polygons in the shape (the head plus the tails). */
  int npolys;
  XPoint **polys;
  /* The leading poly. This will change as we rotate through the poly array. */
  int lead_poly;
  /* The velocity of the points in the leading poly. */
  XPoint *vels;
  GC gc;
  /* An index into state->colors. */
  int color_index;
};

struct state {
  Display *dpy;
  Window window;

  int nshapes;
  struct shape **shapes;
  int npoints;
  int npolys;

  int max_speed;
  int delay;

  int w, h;

  int ncolors;
  XColor *colors;

  Bool dbuf;
  GC erase_gc;
  XWindowAttributes xgwa;
  Pixmap b, ba, bb; /* double-buffer to reduce flicker */

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
  Bool dbeclear_p;
  XdbeBackBuffer backb;
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
};

static int random_speed(int max_speed) {
  int min_speed;
  if (max_speed <= 1) {
    return 1;
  }
  min_speed = max_speed / 6;
  min_speed = min_speed > 1 ? min_speed : 1;
  return min_speed + (random() % (max_speed - min_speed));
}

static int random_velocity(int positive, int max_speed) {
  if (positive) {
    return random_speed(max_speed);
  } else {
    return -random_speed(max_speed);
  }
}

static void draw_shape(struct state *st, Drawable d, struct shape *s) {
  int i;
  for (i = 0; i < s->npolys; ++i) {
    XSetForeground(st->dpy, s->gc, st->colors[s->color_index].pixel);
    XDrawLines(st->dpy, d, s->gc, s->polys[i], s->npoints + 1, CoordModeOrigin);
  }
}

static struct shape *make_shape(struct state *st, Drawable d, int w, int h,
                                int color_index) {
  int i, j;
  XGCValues gcv;
  struct shape *s = (struct shape *)malloc(sizeof(struct shape));

  s->npoints = st->npoints;
  s->npolys = st->npolys;
  s->lead_poly = 0;

  s->polys = (XPoint **)calloc(s->npolys, sizeof(XPoint *));

  /* Initialize the first poly (the lead poly).
   * One extra XPoint is allocated for the duplicate of the first. */
  s->polys[0] = (XPoint *)calloc(s->npoints + 1, sizeof(XPoint));
  for (j = 0; j < s->npoints; ++j) {
    s->polys[0][j].x = random() % w;
    s->polys[0][j].y = random() % h;
  }
  /* Duplicate the first point in the last position. */
  s->polys[0][j].x = s->polys[0][0].x;
  s->polys[0][j].y = s->polys[0][0].y;

  for (i = 1; i < s->npolys; ++i) {
    /* Hide the rest of the polygons behind the first. */
    s->polys[i] = (XPoint *)calloc(s->npoints + 1, sizeof(XPoint));
    for (j = 0; j < s->npoints + 1; ++j) {
      s->polys[i][j].x = s->polys[0][j].x;
      s->polys[i][j].y = s->polys[0][j].y;
    }
  }

  s->vels = (XPoint *)calloc(s->npoints, sizeof(XPoint));
  for (i = 0; i < s->npoints; ++i) {
    s->vels[i].x = random_velocity(random() & 1, st->max_speed);
    s->vels[i].y = random_velocity(random() & 1, st->max_speed);
  }

  s->color_index = color_index;
  gcv.foreground = st->colors[color_index].pixel;
  gcv.line_width =
      max(1, get_integer_resource(st->dpy, "thickness", "Thickness"));
  gcv.join_style = JoinBevel;

  s->gc = XCreateGC(st->dpy, d, GCForeground | GCLineWidth | GCJoinStyle, &gcv);
  return s;
}

static void free_shape(Display *dpy, struct shape *s) {
  if (s) {
    if (s->polys) {
      int i = 0;
      for (; i < s->npolys; ++i) {
        free(s->polys[i]);
      }
      free(s->polys);
    }
    free(s->vels);
    XFreeGC(dpy, s->gc);
    free(s);
  }
}

static void update_shape(struct shape *s, int w, int h, int max_speed,
                         int ncolors) {
  int i;
  XPoint *prev;
  XPoint *lead;

  /* The previous lead poly becomes the second in line and so forth. */
  prev = s->polys[s->lead_poly];
  ++s->lead_poly;
  if (s->lead_poly >= s->npolys) {
    s->lead_poly = 0;
  }
  lead = s->polys[s->lead_poly];

  for (i = 0; i < s->npoints; ++i) {
    lead[i].x = prev[i].x + s->vels[i].x;
    if (s->vels[i].x > 0 && lead[i].x >= w) {
      /* If x = w, we've overshot the edge by 1 px. New x should be w - 2. */
      lead[i].x = (w - 1) - (lead[i].x - (w - 1));
      s->vels[i].x = random_velocity(s->vels[i].x < 0, max_speed);
    } else if (s->vels[i].x < 0 && lead[i].x < 0) {
      lead[i].x = -lead[i].x;
      s->vels[i].x = random_velocity(s->vels[i].x < 0, max_speed);
    }

    lead[i].y = prev[i].y + s->vels[i].y;
    if (s->vels[i].y > 0 && lead[i].y >= h) {
      /* If y = h, we've overshot the edge by 1 px. New y should be h - 2. */
      lead[i].y = (h - 1) - (lead[i].y - (h - 1));
      s->vels[i].y = random_velocity(s->vels[i].y < 0, max_speed);
    } else if (s->vels[i].y < 0 && lead[i].y < 0) {
      lead[i].y = -lead[i].y;
      s->vels[i].y = random_velocity(s->vels[i].y < 0, max_speed);
    }
  }
  /* Duplicate the first point in the last position. */
  lead[i].x = lead[0].x;
  lead[i].y = lead[0].y;

  /* Rotate the color. */
  ++s->color_index;
  if (s->color_index >= ncolors) {
    s->color_index = 0;
  }
}

static void *mystical_init(Display *dpy, Window window) {
  struct state *st = (struct state *)calloc(1, sizeof(struct state));
  XGCValues gcv;
  int i;
  st->dpy = dpy;
  st->window = window;
  st->nshapes = max(1, get_integer_resource(st->dpy, "polys", "Integer"));
  st->npoints = max(2, get_integer_resource(st->dpy, "points", "Integer"));
  st->npolys = max(1, 1 + get_integer_resource(st->dpy, "trails", "Integer"));
  st->max_speed = max(1, get_integer_resource(st->dpy, "speed", "Speed"));
  st->delay = get_integer_resource(st->dpy, "delay", "Integer");
  st->dbuf = get_boolean_resource(st->dpy, "doubleBuffer", "Boolean");

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
  st->dbeclear_p = get_boolean_resource(st->dpy, "useDBEClear", "Boolean");
#endif

#ifdef HAVE_JWXYZ /* Don't second-guess Quartz's double-buffering */
  st->dbuf = False;
#endif

  XGetWindowAttributes(st->dpy, st->window, &st->xgwa);

  st->w = st->xgwa.width;
  st->h = st->xgwa.height;

  st->ncolors = NCOLORS;
  st->colors = (XColor *)calloc(st->ncolors, sizeof(XColor));
  if (get_boolean_resource(st->dpy, "boldColors", "Boolean") == True) {
    make_uniform_colormap(st->xgwa.screen, st->xgwa.visual, st->xgwa.colormap,
                          st->colors, &st->ncolors, True, False, True);
  } else {
    make_smooth_colormap(st->xgwa.screen, st->xgwa.visual, st->xgwa.colormap,
                         st->colors, &st->ncolors, True, False, True);
  }

  if (st->dbuf) {
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
    if (st->dbeclear_p)
      st->b = xdbe_get_backbuffer(st->dpy, st->window, XdbeBackground);
    else
      st->b = xdbe_get_backbuffer(st->dpy, st->window, XdbeUndefined);
    st->backb = st->b;
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */

    if (!st->b) {
      st->ba = XCreatePixmap(st->dpy, st->window, st->xgwa.width,
                             st->xgwa.height, st->xgwa.depth);
      st->bb = XCreatePixmap(st->dpy, st->window, st->xgwa.width,
                             st->xgwa.height, st->xgwa.depth);
      st->b = st->ba;
    }
  } else {
    st->b = st->window;
  }

  st->shapes = (struct shape **)calloc(st->nshapes, sizeof(struct shape *));
  for (i = 0; i < st->nshapes; ++i) {
    st->shapes[i] =
        make_shape(st, st->b, st->w, st->h, i * (st->ncolors / st->nshapes));
  }

  gcv.foreground = get_pixel_resource(st->dpy, st->xgwa.colormap, "background",
                                      "Background");
  st->erase_gc = XCreateGC(st->dpy, st->b, GCForeground, &gcv);

  if (st->ba)
    XFillRectangle(st->dpy, st->ba, st->erase_gc, 0, 0, st->xgwa.width,
                   st->xgwa.height);
  if (st->bb)
    XFillRectangle(st->dpy, st->bb, st->erase_gc, 0, 0, st->xgwa.width,
                   st->xgwa.height);

  return st;
}

static unsigned long mystical_draw(Display *dpy, Window window, void *closure) {
  struct state *st = (struct state *)closure;
  int i;
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
  if (!st->dbeclear_p || !st->backb)
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
    XFillRectangle(st->dpy, st->b, st->erase_gc, 0, 0, st->xgwa.width,
                   st->xgwa.height);

  for (i = 0; i < st->nshapes; ++i) {
    update_shape(st->shapes[i], st->w, st->h, st->max_speed, st->ncolors);
  }
  for (i = 0; i < st->nshapes; ++i) {
    draw_shape(st, st->b, st->shapes[i]);
  }

#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
  if (st->backb) {
    XdbeSwapInfo info[1];
    info[0].swap_window = st->window;
    info[0].swap_action = (st->dbeclear_p ? XdbeBackground : XdbeUndefined);
    XdbeSwapBuffers(st->dpy, info, 1);
  } else
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
  if (st->dbuf) {
    XCopyArea(st->dpy, st->b, st->window, st->erase_gc, 0, 0, st->xgwa.width,
              st->xgwa.height, 0, 0);
    st->b = (st->b == st->ba ? st->bb : st->ba);
  }

  return st->delay;
}

static void mystical_reshape(Display *dpy, Window window, void *closure,
                             unsigned int w, unsigned int h) {
  struct state *st = (struct state *)closure;
  int i;

  st->w = w;
  st->h = h;

  /* Just reinit the shapes. */
  for (i = 0; i < st->nshapes; ++i) {
    free_shape(dpy, st->shapes[i]);
    st->shapes[i] =
        make_shape(st, st->b, st->w, st->h, i * (st->ncolors / st->nshapes));
  }
}

static Bool mystical_event(Display *dpy, Window window, void *closure,
                           XEvent *event) {
  return False;
}

static void mystical_free(Display *dpy, Window window, void *closure) {
  struct state *st = (struct state *)closure;
  int i;
  XFreeGC(dpy, st->erase_gc);
  if (st->ba) XFreePixmap(dpy, st->ba);
  if (st->bb) XFreePixmap(dpy, st->bb);
  for (i = 0; i < st->nshapes; i++) {
    free_shape(dpy, st->shapes[i]);
  }
  free(st->shapes);
  free(st->colors);
  free(st);
}

static const char *mystical_defaults [] = {
  ".background:		black",
  "*delay:		30000",
  "*polys:		2",
  "*points:		4",
  "*trails:		5",
  "*speed:		20",
  "*thickness:		1",
  "*boldColors:		False",
  "*doubleBuffer:	True",
#ifdef HAVE_DOUBLE_BUFFER_EXTENSION
  "*useDBE:		True",
  "*useDBEClear:	True",
#endif /* HAVE_DOUBLE_BUFFER_EXTENSION */
#ifdef HAVE_MOBILE
  "*ignoreRotation:	True",
#endif
  0
};

static XrmOptionDescRec mystical_options[] = {
    {"-delay",		".delay",		XrmoptionSepArg, 0},
    {"-polys",		".polys",		XrmoptionSepArg, 0},
    {"-points",		".points",		XrmoptionSepArg, 0},
    {"-trails",		".trails",		XrmoptionSepArg, 0},
    {"-speed",		".speed",		XrmoptionSepArg, 0},
    {"-thickness",	".thickness",		XrmoptionSepArg, 0},
    {"-bold-colors",	".boldColors",		XrmoptionNoArg,  "True"},
    {"-no-bold-colors",	".boldColors",		XrmoptionNoArg,  "False"},
    {"-db",		".doubleBuffer",	XrmoptionNoArg,  "True"},
    {"-no-db",		".doubleBuffer",	XrmoptionNoArg,  "False"},
    {0, 0, 0, 0}};

XSCREENSAVER_MODULE ("Mystical", mystical)
