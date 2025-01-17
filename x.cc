#include "x.h"

#include <algorithm>

#include <cerrno>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <ctime>

extern "C" {
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <libgen.h>
#include <sys/select.h>
#include <unistd.h>
}

#include "font.h"
#include "mt.h"

/* XEMBED messages */
#define XEMBED_FOCUS_IN 4
#define XEMBED_FOCUS_OUT 5

/* macros */
#define TRUERED(x) (((x)&0xff0000) >> 8)
#define TRUEGREEN(x) (((x)&0xff00))
#define TRUEBLUE(x) (((x)&0xff) << 8)

typedef XftDraw *Draw;
typedef XftColor Color;

/* Purely graphic info */
typedef struct {
  Display *dpy;
  Colormap cmap;
  Window win;
  Drawable buf;
  Atom xembed, wmdeletewin, netwmname, netwmpid;
  XIM xim;
  XIC xic;
  Draw draw;
  Visual *vis;
  XSetWindowAttributes attrs;
  int scr;
  int isfixed; /* is fixed geometry? */
  int l, t;    /* left and top offset */
  int gm;      /* geometry mask */
} XWindow;

typedef struct { Atom xtarget; } XSelection;

/* Drawing Context */
typedef struct {
  Color *col;
  size_t collen;
  std::unique_ptr<MTFont> font;
  GC gc;
} DC;

static inline ushort sixd_to_16bit(int);
static int xmakeglyphfontspecs(XftGlyphFontSpec *, const MTGlyph *, int, int,
                               int);
static void xdrawglyphfontspecs(const XftGlyphFontSpec *, MTGlyph, int, int,
                                int);
static void xdrawglyph(MTGlyph, int, int);
static void xclear(int, int, int, int);
static void xdrawcursor(void);
static int xgeommasktogravity(int);

static void expose(XEvent *);
static void visibility(XEvent *);
static void unmap(XEvent *);
static void kpress(XEvent *);
static void cmessage(XEvent *);
static void resize(XEvent *);
static void focus(XEvent *);
static void brelease(XEvent *);
static void bpress(XEvent *);
static void bmotion(XEvent *);
static void propnotify(XEvent *);
static void selnotify(XEvent *);
static void selclear_(XEvent *);
static void selrequest(XEvent *);

static void selcopy(Time);
static void getbuttoninfo(XEvent *);
static void mousereport(XEvent *);

void handle(XEvent *ev) {
  switch (ev->type) {
  case KeyPress:
    return kpress(ev);
  case ClientMessage:
    return cmessage(ev);
  case ConfigureNotify:
    return resize(ev);
  case VisibilityNotify:
    return visibility(ev);
  case UnmapNotify:
    return unmap(ev);
  case Expose:
    return expose(ev);
  case FocusIn:
    return focus(ev);
  case FocusOut:
    return focus(ev);
  case MotionNotify:
    return bmotion(ev);
  case ButtonPress:
    return bpress(ev);
  case ButtonRelease:
    return brelease(ev);
  // Uncomment if you want the selection to disappear when you select
  // something different in another window.
  //  case SelectionClear: return selclear_(ev);
  case SelectionNotify:
    return selnotify(ev);
  // PropertyNotify is only turned on when there is some INCR transfer
  // happening for the selection retrieval.
  case PropertyNotify:
    return propnotify(ev);
  case SelectionRequest:
    return selrequest(ev);
  }
}

/* Globals */
static DC dc;
static XWindow xw;
static XSelection xsel;

void getbuttoninfo(XEvent *e) {
  int type;
  uint state = e->xbutton.state & ~(Button1Mask | forceselmod);

  sel.alt = IS_SET(MODE_ALTSCREEN);

  sel.oe.x = x2col(e->xbutton.x);
  sel.oe.y = y2row(e->xbutton.y);
  selnormalize();

  sel.type = SEL_REGULAR;
  for (type = 1; type < selmaskslen; ++type) {
    if (match(selmasks[type], state)) {
      sel.type = type;
      break;
    }
  }
}

void mousereport(XEvent *e) {
  int x = x2col(e->xbutton.x), y = y2row(e->xbutton.y),
      button = e->xbutton.button, state = e->xbutton.state, len;
  char buf[40];
  static int ox, oy;

  /* from urxvt */
  if (e->xbutton.type == MotionNotify) {
    if (x == ox && y == oy)
      return;
    if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY))
      return;
    /* MOUSE_MOTION: no reporting if no button is pressed */
    if (IS_SET(MODE_MOUSEMOTION) && oldbutton == 3)
      return;

    button = oldbutton + 32;
    ox = x;
    oy = y;
  } else {
    if (!IS_SET(MODE_MOUSESGR) && e->xbutton.type == ButtonRelease) {
      button = 3;
    } else {
      button -= Button1;
      if (button >= 3)
        button += 64 - 3;
    }
    if (e->xbutton.type == ButtonPress) {
      oldbutton = button;
      ox = x;
      oy = y;
    } else if (e->xbutton.type == ButtonRelease) {
      oldbutton = 3;
      /* MODE_MOUSEX10: no button release reporting */
      if (IS_SET(MODE_MOUSEX10))
        return;
      if (button == 64 || button == 65)
        return;
    }
  }

  if (!IS_SET(MODE_MOUSEX10)) {
    button += ((state & ShiftMask) ? 4 : 0) + ((state & Mod4Mask) ? 8 : 0) +
              ((state & ControlMask) ? 16 : 0);
  }

  if (IS_SET(MODE_MOUSESGR)) {
    len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c", button, x + 1, y + 1,
                   e->xbutton.type == ButtonRelease ? 'm' : 'M');
  } else if (x < 223 && y < 223) {
    len = snprintf(buf, sizeof(buf), "\033[M%c%c%c", 32 + button, 32 + x + 1,
                   32 + y + 1);
  } else {
    return;
  }

  ttywrite(buf, len);
}

void bpress(XEvent *e) {
  struct timespec now;
  MouseShortcut *ms;

  if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forceselmod)) {
    mousereport(e);
    return;
  }

  for (ms = mshortcuts; ms < mshortcuts + mshortcutslen; ms++) {
    if (e->xbutton.button == ms->b && match(ms->mask, e->xbutton.state)) {
      ttysend(ms->s, strlen(ms->s));
      return;
    }
  }

  if (e->xbutton.button == Button1) {
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Clear previous selection, logically and visually. */
    selclear_(NULL);
    sel.mode = SEL_EMPTY;
    sel.type = SEL_REGULAR;
    sel.oe.x = sel.ob.x = x2col(e->xbutton.x);
    sel.oe.y = sel.ob.y = y2row(e->xbutton.y);

    /*
     * If the user clicks below predefined timeouts specific
     * snapping behaviour is exposed.
     */
    if (TIMEDIFF(now, sel.tclick2) <= tripleclicktimeout) {
      sel.snap = SNAP_LINE;
    } else if (TIMEDIFF(now, sel.tclick1) <= doubleclicktimeout) {
      sel.snap = SNAP_WORD;
    } else {
      sel.snap = 0;
    }
    selnormalize();

    if (sel.snap != 0)
      sel.mode = SEL_READY;
    tsetdirt(sel.nb.y, sel.ne.y);
    sel.tclick2 = sel.tclick1;
    sel.tclick1 = now;
  }
}

void selcopy(Time t) { xsetsel(getsel(), t); }

void propnotify(XEvent *e) {
  XPropertyEvent *xpev;
  Atom clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);

  xpev = &e->xproperty;
  if (xpev->state == PropertyNewValue &&
      (xpev->atom == XA_PRIMARY || xpev->atom == clipboard)) {
    selnotify(e);
  }
}

void selnotify(XEvent *e) {
  ulong nitems, ofs, rem;
  int format;
  uchar *data, *last;
  Atom type, incratom, property;

  incratom = XInternAtom(xw.dpy, "INCR", 0);

  ofs = 0;
  if (e->type == SelectionNotify) {
    property = e->xselection.property;
  } else if (e->type == PropertyNotify) {
    property = e->xproperty.atom;
  } else {
    return;
  }
  if (property == None)
    return;

  do {
    if (XGetWindowProperty(xw.dpy, xw.win, property, ofs, BUFSIZ / 4, False,
                           AnyPropertyType, &type, &format, &nitems, &rem,
                           &data)) {
      fprintf(stderr, "Clipboard allocation failed\n");
      return;
    }

    if (e->type == PropertyNotify && nitems == 0 && rem == 0) {
      /*
       * If there is some PropertyNotify with no data, then
       * this is the signal of the selection owner that all
       * data has been transferred. We won't need to receive
       * PropertyNotify events anymore.
       */
      MODBIT(xw.attrs.event_mask, 0, PropertyChangeMask);
      XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
    }

    if (type == incratom) {
      /*
       * Activate the PropertyNotify events so we receive
       * when the selection owner does send us the next
       * chunk of data.
       */
      MODBIT(xw.attrs.event_mask, 1, PropertyChangeMask);
      XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);

      /*
       * Deleting the property is the transfer start signal.
       */
      XDeleteProperty(xw.dpy, xw.win, (int)property);
      continue;
    }

    /*
     * As seen in getsel:
     * Line endings are inconsistent in the terminal and GUI world
     * copy and pasting. When receiving some selection data,
     * replace all '\n' with '\r'.
     * FIXME: Fix the computer world.
     */
    last = data + nitems * format / 8;
    std::replace(data, last, '\n', '\r');

    if (IS_SET(MODE_BRCKTPASTE) && ofs == 0)
      ttywrite("\033[200~", 6);
    ttysend((char *)data, nitems * format / 8);
    if (IS_SET(MODE_BRCKTPASTE) && rem == 0)
      ttywrite("\033[201~", 6);
    XFree(data);
    /* number of 32-bit chunks returned */
    ofs += nitems * format / 32;
  } while (rem > 0);

  /*
   * Deleting the property again tells the selection owner to send the
   * next data chunk in the property.
   */
  XDeleteProperty(xw.dpy, xw.win, (int)property);
}

void xselpaste(void) {
  XConvertSelection(xw.dpy, XA_PRIMARY, xsel.xtarget, XA_PRIMARY, xw.win,
                    CurrentTime);
}

void xclipcopy(void) {
  Atom clipboard;

  if (sel.clipboard != NULL)
    free(sel.clipboard);

  if (sel.primary != NULL) {
    sel.clipboard = xstrdup(sel.primary);
    clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
    XSetSelectionOwner(xw.dpy, clipboard, xw.win, CurrentTime);
  }
}

void xclippaste(void) {
  Atom clipboard;

  clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
  XConvertSelection(xw.dpy, clipboard, xsel.xtarget, clipboard, xw.win,
                    CurrentTime);
}

void selclear_(XEvent *e) { selclear(); }

void selrequest(XEvent *e) {
  XSelectionRequestEvent *xsre;
  XSelectionEvent xev;
  Atom xa_targets, string, clipboard;
  char *seltext;

  xsre = (XSelectionRequestEvent *)e;
  xev.type = SelectionNotify;
  xev.requestor = xsre->requestor;
  xev.selection = xsre->selection;
  xev.target = xsre->target;
  xev.time = xsre->time;
  if (xsre->property == None)
    xsre->property = xsre->target;

  /* reject */
  xev.property = None;

  xa_targets = XInternAtom(xw.dpy, "TARGETS", 0);
  if (xsre->target == xa_targets) {
    /* respond with the supported type */
    string = xsel.xtarget;
    XChangeProperty(xsre->display, xsre->requestor, xsre->property, XA_ATOM, 32,
                    PropModeReplace, (uchar *)&string, 1);
    xev.property = xsre->property;
  } else if (xsre->target == xsel.xtarget || xsre->target == XA_STRING) {
    /*
     * xith XA_STRING non ascii characters may be incorrect in the
     * requestor. It is not our problem, use utf8.
     */
    clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
    if (xsre->selection == XA_PRIMARY) {
      seltext = sel.primary;
    } else if (xsre->selection == clipboard) {
      seltext = sel.clipboard;
    } else {
      fprintf(stderr, "Unhandled clipboard selection 0x%lx\n", xsre->selection);
      return;
    }
    if (seltext != NULL) {
      XChangeProperty(xsre->display, xsre->requestor, xsre->property,
                      xsre->target, 8, PropModeReplace, (uchar *)seltext,
                      strlen(seltext));
      xev.property = xsre->property;
    }
  }

  /* all done, send a notification to the listener */
  if (!XSendEvent(xsre->display, xsre->requestor, 1, 0, (XEvent *)&xev))
    fprintf(stderr, "Error sending SelectionNotify event\n");
}

void xsetsel(char *str, Time t) {
  free(sel.primary);
  sel.primary = str;

  XSetSelectionOwner(xw.dpy, XA_PRIMARY, xw.win, t);
  if (XGetSelectionOwner(xw.dpy, XA_PRIMARY) != xw.win)
    selclear_(NULL);
}

void brelease(XEvent *e) {
  if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forceselmod)) {
    mousereport(e);
    return;
  }

  if (e->xbutton.button == Button2) {
    xselpaste();
  } else if (e->xbutton.button == Button1) {
    if (sel.mode == SEL_READY) {
      getbuttoninfo(e);
      selcopy(e->xbutton.time);
    } else
      selclear_(NULL);
    sel.mode = SEL_IDLE;
    tsetdirt(sel.nb.y, sel.ne.y);
  }
}

void bmotion(XEvent *e) {
  int oldey, oldex, oldsby, oldsey;

  if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forceselmod)) {
    mousereport(e);
    return;
  }

  if (!sel.mode)
    return;

  sel.mode = SEL_READY;
  oldey = sel.oe.y;
  oldex = sel.oe.x;
  oldsby = sel.nb.y;
  oldsey = sel.ne.y;
  getbuttoninfo(e);

  if (oldey != sel.oe.y || oldex != sel.oe.x)
    tsetdirt(MIN(sel.nb.y, oldsby), MAX(sel.ne.y, oldsey));
}

void xresize(int col, int row) {
  win.tw = MAX(1, col * win.cw);
  win.th = MAX(1, row * win.ch);

  XFreePixmap(xw.dpy, xw.buf);
  xw.buf =
      XCreatePixmap(xw.dpy, xw.win, win.w, win.h, DefaultDepth(xw.dpy, xw.scr));
  XftDrawChange(xw.draw, xw.buf);
  xclear(0, 0, win.w, win.h);
}

ushort sixd_to_16bit(int x) { return x == 0 ? 0 : 0x3737 + 0x2828 * x; }

int xloadcolor(int i, const char *name, Color *ncolor) {
  XRenderColor color = {};
  color.alpha = 0xffff;

  if (!name) {
    if (BETWEEN(i, 16, 255)) {  /* 256 color */
      if (i < 6 * 6 * 6 + 16) { /* same colors as xterm */
        color.red = sixd_to_16bit(((i - 16) / 36) % 6);
        color.green = sixd_to_16bit(((i - 16) / 6) % 6);
        color.blue = sixd_to_16bit(((i - 16) / 1) % 6);
      } else { /* greyscale */
        color.red = 0x0808 + 0x0a0a * (i - (6 * 6 * 6 + 16));
        color.green = color.blue = color.red;
      }
      return XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &color, ncolor);
    } else
      name = colorname[i];
  }

  return XftColorAllocName(xw.dpy, xw.vis, xw.cmap, name, ncolor);
}

void xloadcols(void) {
  int i;
  static int loaded;
  Color *cp;

  dc.collen = MAX(colornamelen, 256);
  dc.col = static_cast<Color *>(malloc(dc.collen * sizeof(Color)));
  if (!dc.col)
    die("Out of memory\n");

  if (loaded) {
    for (cp = dc.col; cp < &dc.col[dc.collen]; ++cp)
      XftColorFree(xw.dpy, xw.vis, xw.cmap, cp);
  }

  for (i = 0; i < dc.collen; i++)
    if (!xloadcolor(i, NULL, &dc.col[i])) {
      if (colorname[i])
        die("Could not allocate color '%s'\n", colorname[i]);
      else
        die("Could not allocate color %d\n", i);
    }
  loaded = 1;
}

int xsetcolorname(int x, const char *name) {
  Color ncolor;

  if (!BETWEEN(x, 0, dc.collen))
    return 1;

  if (!xloadcolor(x, name, &ncolor))
    return 1;

  XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.col[x]);
  dc.col[x] = ncolor;

  return 0;
}

/*
 * Absolute coordinates.
 */
void xclear(int x1, int y1, int x2, int y2) {
  XftDrawRect(xw.draw, &dc.col[IS_SET(MODE_REVERSE) ? defaultfg : defaultbg],
              x1, y1, x2 - x1, y2 - y1);
}

void xhints(void) {
  XClassHint xclass = {opt_name ? opt_name : termname,
                       opt_class ? opt_class : termname};
  XWMHints wm = {};
  wm.flags = InputHint;
  wm.input = 1;
  XSizeHints *sizeh = NULL;

  sizeh = XAllocSizeHints();

  sizeh->flags = PSize | PResizeInc | PBaseSize;
  sizeh->height = win.h;
  sizeh->width = win.w;
  sizeh->height_inc = win.ch;
  sizeh->width_inc = win.cw;
  sizeh->base_height = 2 * borderpx;
  sizeh->base_width = 2 * borderpx;
  if (xw.isfixed) {
    sizeh->flags |= PMaxSize | PMinSize;
    sizeh->min_width = sizeh->max_width = win.w;
    sizeh->min_height = sizeh->max_height = win.h;
  }
  if (xw.gm & (XValue | YValue)) {
    sizeh->flags |= USPosition | PWinGravity;
    sizeh->x = xw.l;
    sizeh->y = xw.t;
    sizeh->win_gravity = xgeommasktogravity(xw.gm);
  }

  XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm, &xclass);
  XFree(sizeh);
}

int xgeommasktogravity(int mask) {
  switch (mask & (XNegative | YNegative)) {
  case 0:
    return NorthWestGravity;
  case XNegative:
    return NorthEastGravity;
  case YNegative:
    return SouthWestGravity;
  }

  return SouthEastGravity;
}

static void reloadmetrics() {
  win.cw = ceilf(dc.font->metrics().width * cwscale);
  win.ch = ceilf(dc.font->metrics().height * chscale);
}

static int default_font_size;
double xdefaultfontsize() { return default_font_size; }
double xfontsize() { return dc.font->metrics().pixel_size; }
void xsetfontsize(double fontsize) {
  dc.font->SetPixelSize(fontsize);
  reloadmetrics();
}

void xinit(void) {
  XGCValues gcvalues;
  Cursor cursor;
  Window parent;
  pid_t thispid = getpid();
  XColor xmousefg, xmousebg;

  if (!(xw.dpy = XOpenDisplay(NULL)))
    die("Can't open display\n");
  xw.scr = XDefaultScreen(xw.dpy);
  xw.vis = XDefaultVisual(xw.dpy, xw.scr);

  /* font */
  if (!FcInit())
    die("Could not init fontconfig.\n");
  dc.font.reset(
      new MTFont(opt_font == nullptr ? font : opt_font, xw.dpy, xw.scr));
  reloadmetrics();
  default_font_size = dc.font->metrics().pixel_size;

  /* colors */
  xw.cmap = XDefaultColormap(xw.dpy, xw.scr);
  xloadcols();

  /* adjust fixed window geometry */
  win.w = 2 * borderpx + term.col * win.cw;
  win.h = 2 * borderpx + term.row * win.ch;
  if (xw.gm & XNegative)
    xw.l += DisplayWidth(xw.dpy, xw.scr) - win.w - 2;
  if (xw.gm & YNegative)
    xw.t += DisplayHeight(xw.dpy, xw.scr) - win.h - 2;

  /* Events */
  xw.attrs.background_pixel = dc.col[defaultbg].pixel;
  xw.attrs.border_pixel = dc.col[defaultbg].pixel;
  xw.attrs.bit_gravity = NorthWestGravity;
  xw.attrs.event_mask = FocusChangeMask | KeyPressMask | ExposureMask |
                        VisibilityChangeMask | StructureNotifyMask |
                        ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
  xw.attrs.colormap = xw.cmap;

  if (!(opt_embed && (parent = strtol(opt_embed, NULL, 0))))
    parent = XRootWindow(xw.dpy, xw.scr);
  xw.win = XCreateWindow(xw.dpy, parent, xw.l, xw.t, win.w, win.h, 0,
                         XDefaultDepth(xw.dpy, xw.scr), InputOutput, xw.vis,
                         CWBackPixel | CWBorderPixel | CWBitGravity |
                             CWEventMask | CWColormap,
                         &xw.attrs);

  memset(&gcvalues, 0, sizeof(gcvalues));
  gcvalues.graphics_exposures = False;
  dc.gc = XCreateGC(xw.dpy, parent, GCGraphicsExposures, &gcvalues);
  xw.buf =
      XCreatePixmap(xw.dpy, xw.win, win.w, win.h, DefaultDepth(xw.dpy, xw.scr));
  XSetForeground(xw.dpy, dc.gc, dc.col[defaultbg].pixel);
  XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, win.w, win.h);

  /* Xft rendering context */
  xw.draw = XftDrawCreate(xw.dpy, xw.buf, xw.vis, xw.cmap);

  /* input methods */
  if ((xw.xim = XOpenIM(xw.dpy, NULL, NULL, NULL)) == NULL) {
    XSetLocaleModifiers("@im=local");
    if ((xw.xim = XOpenIM(xw.dpy, NULL, NULL, NULL)) == NULL) {
      XSetLocaleModifiers("@im=");
      if ((xw.xim = XOpenIM(xw.dpy, NULL, NULL, NULL)) == NULL) {
        die("XOpenIM failed. Could not open input"
            " device.\n");
      }
    }
  }
  xw.xic = XCreateIC(xw.xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                     XNClientWindow, xw.win, XNFocusWindow, xw.win, NULL);
  if (xw.xic == NULL)
    die("XCreateIC failed. Could not obtain input method.\n");

  /* white cursor, black outline */
  cursor = XCreateFontCursor(xw.dpy, mouseshape);
  XDefineCursor(xw.dpy, xw.win, cursor);

  if (XParseColor(xw.dpy, xw.cmap, colorname[mousefg], &xmousefg) == 0) {
    xmousefg.red = 0xffff;
    xmousefg.green = 0xffff;
    xmousefg.blue = 0xffff;
  }

  if (XParseColor(xw.dpy, xw.cmap, colorname[mousebg], &xmousebg) == 0) {
    xmousebg.red = 0x0000;
    xmousebg.green = 0x0000;
    xmousebg.blue = 0x0000;
  }

  XRecolorCursor(xw.dpy, cursor, &xmousefg, &xmousebg);

  xw.xembed = XInternAtom(xw.dpy, "_XEMBED", False);
  xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
  xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
  XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

  xw.netwmpid = XInternAtom(xw.dpy, "_NET_WM_PID", False);
  XChangeProperty(xw.dpy, xw.win, xw.netwmpid, XA_CARDINAL, 32, PropModeReplace,
                  (uchar *)&thispid, 1);

  resettitle();
  XMapWindow(xw.dpy, xw.win);
  xhints();
  XSync(xw.dpy, False);

  xsel.xtarget = XInternAtom(xw.dpy, "UTF8_STRING", 0);
  if (xsel.xtarget == None)
    xsel.xtarget = XA_STRING;
}

int xmakeglyphfontspecs(XftGlyphFontSpec *specs, const MTGlyph *glyphs, int len,
                        int x, int y) {
  float winx = borderpx + x * win.cw, winy = borderpx + y * win.ch, xp, yp;
  int i, numspecs = 0;
  for (i = 0, xp = winx, yp = winy + dc.font->metrics().ascent; i < len; ++i) {
    ushort mode = glyphs[i].mode;
    if (mode == ATTR_WDUMMY)
      continue;

    MTFont::Glyph glyph = dc.font->FindGlyph(
        glyphs[i].u, static_cast<MTFont::Style>(
                         ((mode & ATTR_BOLD) ? MTFont::BOLD : 0) |
                         ((mode & ATTR_ITALIC) ? MTFont::ITALIC : 0)));
    specs[numspecs].glyph = glyph.index;
    specs[numspecs].font = glyph.font;
    specs[numspecs].x = (short)xp;
    specs[numspecs].y = (short)yp;
    xp += win.cw;
    if (mode & ATTR_WIDE) xp += win.cw;
    numspecs++;
  }

  return numspecs;
}

void xdrawglyphfontspecs(const XftGlyphFontSpec *specs, MTGlyph base, int len,
                         int x, int y) {
  int charlen = len * ((base.mode & ATTR_WIDE) ? 2 : 1);
  int winx = borderpx + x * win.cw, winy = borderpx + y * win.ch,
      width = charlen * win.cw;
  Color *fg, *bg, *temp, revfg, revbg, truefg, truebg;
  XRenderColor colfg, colbg;
  XRectangle r;

  if (IS_TRUECOL(base.fg)) {
    colfg.alpha = 0xffff;
    colfg.red = TRUERED(base.fg);
    colfg.green = TRUEGREEN(base.fg);
    colfg.blue = TRUEBLUE(base.fg);
    XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &truefg);
    fg = &truefg;
  } else {
    fg = &dc.col[base.fg];
  }

  if (IS_TRUECOL(base.bg)) {
    colbg.alpha = 0xffff;
    colbg.green = TRUEGREEN(base.bg);
    colbg.red = TRUERED(base.bg);
    colbg.blue = TRUEBLUE(base.bg);
    XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &truebg);
    bg = &truebg;
  } else {
    bg = &dc.col[base.bg];
  }

  /* Change basic system colors [0-7] to bright system colors [8-15] */
  if ((base.mode & ATTR_BOLD_FAINT) == ATTR_BOLD && BETWEEN(base.fg, 0, 7))
    fg = &dc.col[base.fg + 8];

  if (IS_SET(MODE_REVERSE)) {
    if (fg == &dc.col[defaultfg]) {
      fg = &dc.col[defaultbg];
    } else {
      colfg.red = ~fg->color.red;
      colfg.green = ~fg->color.green;
      colfg.blue = ~fg->color.blue;
      colfg.alpha = fg->color.alpha;
      XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &revfg);
      fg = &revfg;
    }

    if (bg == &dc.col[defaultbg]) {
      bg = &dc.col[defaultfg];
    } else {
      colbg.red = ~bg->color.red;
      colbg.green = ~bg->color.green;
      colbg.blue = ~bg->color.blue;
      colbg.alpha = bg->color.alpha;
      XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &revbg);
      bg = &revbg;
    }
  }

  if (base.mode & ATTR_REVERSE) {
    temp = fg;
    fg = bg;
    bg = temp;
  }

  if ((base.mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
    colfg.red = fg->color.red / 2;
    colfg.green = fg->color.green / 2;
    colfg.blue = fg->color.blue / 2;
    XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &revfg);
    fg = &revfg;
  }

  if (base.mode & ATTR_BLINK && term.mode & MODE_BLINK)
    fg = bg;

  if (base.mode & ATTR_INVISIBLE)
    fg = bg;

  /* Intelligent cleaning up of the borders. */
  if (x == 0) {
    xclear(0, (y == 0) ? 0 : winy, borderpx,
           winy + win.ch + ((y >= term.row - 1) ? win.h : 0));
  }
  if (x + charlen >= term.col) {
    xclear(winx + width, (y == 0) ? 0 : winy, win.w,
           ((y >= term.row - 1) ? win.h : (winy + win.ch)));
  }
  if (y == 0)
    xclear(winx, 0, winx + width, borderpx);
  if (y == term.row - 1)
    xclear(winx, winy + win.ch, winx + width, win.h);

  /* Clean up the region we want to draw to. */
  XftDrawRect(xw.draw, bg, winx, winy, width, win.ch);

  /* Set the clip region because Xft is sometimes dirty. */
  r.x = 0;
  r.y = 0;
  r.height = win.ch;
  r.width = width;
  XftDrawSetClipRectangles(xw.draw, winx, winy, &r, 1);

  /* Render the glyphs. */
  XftDrawGlyphFontSpec(xw.draw, fg, specs, len);

  /* Render underline and strikethrough. */
  if (base.mode & ATTR_UNDERLINE) {
    XftDrawRect(xw.draw, fg, winx, winy + dc.font->metrics().ascent + 1, width,
                1);
  }

  if (base.mode & ATTR_STRUCK) {
    XftDrawRect(xw.draw, fg, winx, winy + 2 * dc.font->metrics().ascent / 3,
                width, 1);
  }

  /* Reset clip to none. */
  XftDrawSetClip(xw.draw, 0);
}

void xdrawglyph(MTGlyph g, int x, int y) {
  int numspecs;
  XftGlyphFontSpec spec;

  numspecs = xmakeglyphfontspecs(&spec, &g, 1, x, y);
  xdrawglyphfontspecs(&spec, g, numspecs, x, y);
}

void xdrawcursor(void) {
  static int oldx = 0, oldy = 0;
  int curx;
  MTGlyph g = {' ', ATTR_NULL, defaultbg, defaultcs}, og;
  int ena_sel = sel.ob.x != -1 && sel.alt == IS_SET(MODE_ALTSCREEN);
  Color drawcol;

  LIMIT(oldx, 0, term.col - 1);
  LIMIT(oldy, 0, term.row - 1);

  curx = term.c.x;

  /* adjust position if in dummy */
  if (term.line[oldy][oldx].mode & ATTR_WDUMMY)
    oldx--;
  if (term.line[term.c.y][curx].mode & ATTR_WDUMMY)
    curx--;

  /* remove the old cursor */
  og = term.line[oldy][oldx];
  if (ena_sel && selected(oldx, oldy))
    og.mode ^= ATTR_REVERSE;
  xdrawglyph(og, oldx, oldy);

  g.u = term.line[term.c.y][term.c.x].u;
  g.mode |= term.line[term.c.y][term.c.x].mode &
            (ATTR_BOLD | ATTR_ITALIC | ATTR_UNDERLINE | ATTR_STRUCK);

  /*
   * Select the right color for the right mode.
   */
  if (IS_SET(MODE_REVERSE)) {
    g.mode |= ATTR_REVERSE;
    g.bg = defaultfg;
    if (ena_sel && selected(term.c.x, term.c.y)) {
      drawcol = dc.col[defaultcs];
      g.fg = defaultrcs;
    } else {
      drawcol = dc.col[defaultrcs];
      g.fg = defaultcs;
    }
  } else {
    if (ena_sel && selected(term.c.x, term.c.y)) {
      drawcol = dc.col[defaultrcs];
      g.fg = defaultfg;
      g.bg = defaultrcs;
    } else {
      drawcol = dc.col[defaultcs];
    }
  }

  if (IS_SET(MODE_HIDE))
    return;

  /* draw the new one */
  if (win.state & WIN_FOCUSED) {
    switch (win.cursor) {
    case 7: /* mt extension: snowman */
      utf8decode("☃", &g.u, UTF_SIZ);
    case 0: /* Blinking Block */
    case 1: /* Blinking Block (Default) */
    case 2: /* Steady Block */
      g.mode |= term.line[term.c.y][curx].mode & ATTR_WIDE;
      xdrawglyph(g, term.c.x, term.c.y);
      break;
    case 3: /* Blinking Underline */
    case 4: /* Steady Underline */
      XftDrawRect(xw.draw, &drawcol, borderpx + curx * win.cw,
                  borderpx + (term.c.y + 1) * win.ch - cursorthickness, win.cw,
                  cursorthickness);
      break;
    case 5: /* Blinking bar */
    case 6: /* Steady bar */
      XftDrawRect(xw.draw, &drawcol, borderpx + curx * win.cw,
                  borderpx + term.c.y * win.ch, cursorthickness, win.ch);
      break;
    }
  } else {
    XftDrawRect(xw.draw, &drawcol, borderpx + curx * win.cw,
                borderpx + term.c.y * win.ch, win.cw - 1, 1);
    XftDrawRect(xw.draw, &drawcol, borderpx + curx * win.cw,
                borderpx + term.c.y * win.ch, 1, win.ch - 1);
    XftDrawRect(xw.draw, &drawcol, borderpx + (curx + 1) * win.cw - 1,
                borderpx + term.c.y * win.ch, 1, win.ch - 1);
    XftDrawRect(xw.draw, &drawcol, borderpx + curx * win.cw,
                borderpx + (term.c.y + 1) * win.ch - 1, win.cw, 1);
  }
  oldx = curx, oldy = term.c.y;
}

void xsetenv(void) {
  char buf[sizeof(long) * 8 + 1];

  snprintf(buf, sizeof(buf), "%lu", xw.win);
  setenv("WINDOWID", buf, 1);
}

void xsettitle(const char *p) {
  XTextProperty prop;

  // This function only reads p, but doesn't declare it const...
  Xutf8TextListToTextProperty(xw.dpy, const_cast<char **>(&p), 1,
                              XUTF8StringStyle, &prop);
  XSetWMName(xw.dpy, xw.win, &prop);
  XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmname);
  XFree(prop.value);
}

void draw(void) {
  drawregion(0, 0, term.col, term.row);
  XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, 0, 0, win.w, win.h, 0, 0);
  XSetForeground(xw.dpy, dc.gc,
                 dc.col[IS_SET(MODE_REVERSE) ? defaultfg : defaultbg].pixel);
}

void drawregion(int x1, int y1, int x2, int y2) {
  int i, x, y, ox, numspecs;
  MTGlyph base, changed;
  XftGlyphFontSpec *specs;
  int ena_sel = sel.ob.x != -1 && sel.alt == IS_SET(MODE_ALTSCREEN);

  if (!(win.state & WIN_VISIBLE))
    return;

  for (y = y1; y < y2; y++) {
    if (!term.dirty[y])
      continue;

    term.dirty[y] = 0;

    specs = term.specbuf;
    numspecs = xmakeglyphfontspecs(specs, &term.line[y][x1], x2 - x1, x1, y);

    i = ox = 0;
    for (x = x1; x < x2 && i < numspecs; x++) {
      changed = term.line[y][x];
      if (changed.mode == ATTR_WDUMMY)
        continue;
      if (ena_sel && selected(x, y))
        changed.mode ^= ATTR_REVERSE;
      if (i > 0 && ATTRCMP(base, changed)) {
        xdrawglyphfontspecs(specs, base, i, ox, y);
        specs += i;
        numspecs -= i;
        i = 0;
      }
      if (i == 0) {
        ox = x;
        base = changed;
      }
      i++;
    }
    if (i > 0)
      xdrawglyphfontspecs(specs, base, i, ox, y);
  }
  xdrawcursor();
}

void expose(XEvent *ev) { redraw(); }

void visibility(XEvent *ev) {
  XVisibilityEvent *e = &ev->xvisibility;

  MODBIT(win.state, e->state != VisibilityFullyObscured, WIN_VISIBLE);
}

void unmap(XEvent *ev) { win.state &= ~WIN_VISIBLE; }

void xsetpointermotion(int set) {
  MODBIT(xw.attrs.event_mask, set, PointerMotionMask);
  XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
}

void xseturgency(int add) {
  XWMHints *h = XGetWMHints(xw.dpy, xw.win);

  MODBIT(h->flags, add, XUrgencyHint);
  XSetWMHints(xw.dpy, xw.win, h);
  XFree(h);
}

void xbell(void) { XkbBell(xw.dpy, xw.win, 0, (Atom)NULL); }

unsigned long xwinid(void) { return xw.win; }

void focus(XEvent *ev) {
  XFocusChangeEvent *e = &ev->xfocus;

  if (e->mode == NotifyGrab)
    return;

  if (ev->type == FocusIn) {
    XSetICFocus(xw.xic);
    win.state |= WIN_FOCUSED;
    xseturgency(0);
    if (IS_SET(MODE_FOCUS))
      ttywrite("\033[I", 3);
  } else {
    XUnsetICFocus(xw.xic);
    win.state &= ~WIN_FOCUSED;
    if (IS_SET(MODE_FOCUS))
      ttywrite("\033[O", 3);
  }
}

void kpress(XEvent *ev) {
  XKeyEvent *e = &ev->xkey;
  KeySym ksym;
  char buf[32];
  const char *customkey;
  int len;
  Rune c;
  Status status;
  Shortcut *bp;

  if (IS_SET(MODE_KBDLOCK))
    return;

  len = XmbLookupString(xw.xic, e, buf, sizeof buf, &ksym, &status);
  /* 1. shortcuts */
  for (bp = shortcuts; bp < shortcuts + shortcutslen; bp++) {
    if (ksym == bp->keysym && match(bp->mod, e->state)) {
      bp->func(&(bp->arg));
      return;
    }
  }

  /* 2. custom keys from config.h */
  if ((customkey = kmap(ksym, e->state))) {
    ttysend(customkey, strlen(customkey));
    return;
  }

  /* 3. composed string from input method */
  if (len == 0)
    return;
  if (len == 1 && e->state & Mod1Mask) {
    if (IS_SET(MODE_8BIT)) {
      if (*buf < 0177) {
        c = *buf | 0x80;
        len = utf8encode(c, buf);
      }
    } else {
      buf[1] = buf[0];
      buf[0] = '\033';
      len = 2;
    }
  }
  ttysend(buf, len);
}

void cmessage(XEvent *e) {
  /*
   * See xembed specs
   *  http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html
   */
  if (e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
    if (e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
      win.state |= WIN_FOCUSED;
      xseturgency(0);
    } else if (e->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
      win.state &= ~WIN_FOCUSED;
    }
  } else if (e->xclient.data.l[0] == xw.wmdeletewin) {
    /* Send SIGHUP to shell */
    kill(pid, SIGHUP);
    exit(0);
  }
}

void resize(XEvent *e) {
  if (e->xconfigure.width == win.w && e->xconfigure.height == win.h)
    return;

  cresize(e->xconfigure.width, e->xconfigure.height);
  ttyresize();
}

void run(void) {
  XEvent ev;
  int w = win.w, h = win.h;
  fd_set rfd;
  int xfd = XConnectionNumber(xw.dpy), xev, blinkset = 0, dodraw = 0;
  struct timespec drawtimeout, *tv = NULL, now, last, lastblink;
  long deltatime;

  /* Waiting for window mapping */
  do {
    XNextEvent(xw.dpy, &ev);
    /*
     * This XFilterEvent call is required because of XOpenIM. It
     * does filter out the key event and some client message for
     * the input method too.
     */
    if (XFilterEvent(&ev, None))
      continue;
    if (ev.type == ConfigureNotify) {
      w = ev.xconfigure.width;
      h = ev.xconfigure.height;
    }
  } while (ev.type != MapNotify);

  cresize(w, h);
  ttynew();
  ttyresize();

  clock_gettime(CLOCK_MONOTONIC, &last);
  lastblink = last;

  for (xev = actionfps;;) {
    FD_ZERO(&rfd);
    FD_SET(cmdfd, &rfd);
    FD_SET(xfd, &rfd);

    if (pselect(MAX(xfd, cmdfd) + 1, &rfd, NULL, NULL, tv, NULL) < 0) {
      if (errno == EINTR)
        continue;
      die("select failed: %s\n", strerror(errno));
    }
    if (FD_ISSET(cmdfd, &rfd)) {
      ttyread();
      if (blinktimeout) {
        blinkset = tattrset(ATTR_BLINK);
        if (!blinkset)
          MODBIT(term.mode, 0, MODE_BLINK);
      }
    }

    if (FD_ISSET(xfd, &rfd))
      xev = actionfps;

    clock_gettime(CLOCK_MONOTONIC, &now);
    drawtimeout.tv_sec = 0;
    drawtimeout.tv_nsec = (1000 * 1E6) / xfps;
    tv = &drawtimeout;

    dodraw = 0;
    if (blinktimeout && TIMEDIFF(now, lastblink) > blinktimeout) {
      tsetdirtattr(ATTR_BLINK);
      term.mode ^= MODE_BLINK;
      lastblink = now;
      dodraw = 1;
    }
    deltatime = TIMEDIFF(now, last);
    if (deltatime > 1000 / (xev ? xfps : actionfps)) {
      dodraw = 1;
      last = now;
    }

    if (dodraw) {
      while (XPending(xw.dpy)) {
        XNextEvent(xw.dpy, &ev);
        if (XFilterEvent(&ev, None))
          continue;
        handle(&ev);
      }

      draw();
      XFlush(xw.dpy);

      if (xev && !FD_ISSET(xfd, &rfd))
        xev--;
      if (!FD_ISSET(cmdfd, &rfd) && !FD_ISSET(xfd, &rfd)) {
        if (blinkset) {
          if (TIMEDIFF(now, lastblink) > blinktimeout) {
            drawtimeout.tv_nsec = 1000;
          } else {
            drawtimeout.tv_nsec =
                (1E6 * (blinktimeout - TIMEDIFF(now, lastblink)));
          }
          drawtimeout.tv_sec = drawtimeout.tv_nsec / 1E9;
          drawtimeout.tv_nsec %= (long)1E9;
        } else {
          tv = NULL;
        }
      }
    }
  }
}

int main(int argc, char *argv[]) {
  xw.l = xw.t = 0;
  xw.isfixed = False;
  win.cursor = cursorshape;

  auto usage = [&]{
    fprintf(stderr,
R"(usage: %s [-iv] [-c class] [-f font] [-g geometry] [-n name] [-o file]
            [-T title] [-t title] [-w windowid] [[-e] command [args ...]]
)", argv[0]);
  };
  int argi, argj;  // Index and character being processed.
  auto read_param = [&]{
    // -fvalue syntax: parameter is the rest of the word.
    if (argv[argi][argj + 1] != '\0') {
      auto* rest = &argv[argi][argj + 1];
      argj = strlen(argv[argi]) - 1; // advance cursor to last char
      return rest;
    }
    // -f value syntax: parameter is the next word.
    if (++argi < argc) {
      argj = strlen(argv[argi]) - 1;
      return argv[argi];
    }
    usage(); // Missing param.
    exit(1);
  };
  for (argi = 1; argi < argc; ++argi) {
    const char* arg = argv[argi];
    // Stop after non-flag args, including --.
    if (argv[argi][0] != '-' || (argv[argi][1] == '-' && !argv[argi][2])) break;
    for (argj = 1; argv[argi][argj] != '\0'; ++argj) switch(arg[argj]) {
      case 'a':
        allowaltscreen = 0;
        break;
      case 'c':
        opt_class = read_param();
        break;
      case 'e':
        ++argi;
        goto run;
      case 'f':
        opt_font = read_param();
        break;
      case 'g':
        xw.gm = XParseGeometry(read_param(), &xw.l, &xw.t, &cols, &rows);
        break;
      case 'i':
        xw.isfixed = 1;
        break;
      case 'o':
        opt_io = read_param();
        break;
      case 'n':
        opt_name = read_param();
        break;
      case 't':
      case 'T':
        opt_title = read_param();
        break;
      case 'w':
        opt_embed = read_param();
        break;
      case 'v':
        fprintf(stderr, "%s " VERSION "\n", argv[0]);
        exit(0);
      default:
        usage();
        exit(1);
    }
  }

run:
  if (argi < argc) {
    /* eat all remaining arguments */
    opt_cmd = &argv[argi];
    if (!opt_title)
      opt_title = basename(opt_cmd[0]);
  }
  setlocale(LC_CTYPE, "");
  XSetLocaleModifiers("");
  tnew(MAX(cols, 1), MAX(rows, 1));
  xinit();
  selinit();
  run();

  return 0;
}
