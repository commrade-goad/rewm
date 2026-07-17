/* wm.h — shared types between host.c (native) and wm.c (JIT-compiled)
 *
 * WMState lives on the native heap (allocated by host.c).  A pointer
 * to it is passed into every JIT entry point so that state survives
 * across module reloads.
 */
#ifndef WM_H
#define WM_H

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xinerama.h>
#include <X11/Xft/Xft.h>

#define LENGTH(x)       (sizeof(x) / sizeof(x[0]))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define BETWEEN(x, a, b) ((a) <= (x) && (x) <= (b))
#define CLEANMASK(m)    ((m) & ~(numlockmask|LockMask) & (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))
#define INTERSECT(x,y,w,h,m) \
    (int)(MAX(0, MIN((x)+(w),(m)->wx+(m)->ww) - MAX((x),(m)->wx)) * \
          MAX(0, MIN((y)+(h),(m)->wy+(m)->wh) - MAX((y),(m)->wy)))

/* ---- status codes returned by wm_entry --------------------------------- */
enum { REWM_OK, REWM_RELOAD, REWM_QUIT };

/* ---- color scheme slots -------------------------------------------------- */
enum { SchemeNorm, SchemeSel, SchemeLast };
enum { ColFg, ColBg, ColBorder, ColLast };

/* ---- forward declarations ----------------------------------------------- */
typedef struct Client  Client;
typedef struct Monitor Monitor;
typedef struct WMState WMState;

/* ---- client ------------------------------------------------------------- */
struct Client {
    char name[256];
    Window win;
    int x, y, w, h;             /* geometry */
    int oldx, oldy, oldw, oldh;  /* saved geometry (pre-fullscreen / float toggle) */
    int oldbw;
    int basew, baseh, incw, inch, maxw, maxh, minw, minh;
    float mina, maxa;             /* aspect ratio hints */
    int bw;                     /* border width */
    unsigned int tags;
    int isfloating, isurgent, isfullscreen, issticky;
    int wasfloating;            /* floating state to restore after unfullscreen */
    int isfixed;                 /* min==max size -> never resize */
    int neverfocus;               /* WM_HINTS input=False */
    int hintsvalid;              /* size hints cache flag */
    Client *next;                /* monitor client list */
    Client *snext;               /* stack (focus/raise order) */
    Monitor *mon;
};

/* ---- monitor ------------------------------------------------------------ */
struct Monitor {
    int num;
    float mfact;
    int nmaster;
    int gappx;
    char ltsymbol[16];
    int showbar;
    int topbar;
    int mx, my, mw, mh;         /* screen size */
    int wx, wy, ww, wh;         /* window area (excludes bar) */
    unsigned int tagset[2];
    int seltags;
    int sellt;
    Client *clients;
    Client *stack;
    Monitor *next;
    Window barwin;
};

/* ---- layout plug -------------------------------------------------------- */
typedef struct {
    const char *symbol;
    void (*arrange)(WMState *, Monitor *);
} Layout;

/* ---- all persistent state ----------------------------------------------- */
struct WMState {
    /* X11 */
    Display *dpy;
    Window root;
    int screen;
    int sw, sh;                 /* display width/height */

    /* monitors & clients */
    Monitor *mons;
    Monitor *selmon;
    Client *clients;            /* mirror of selmon->clients, kept for _NET_CLIENT_LIST */
    Client *sel;
    Client *stack;

    /* atoms */
    Atom wm_protocols, wm_delete_window, wm_state, wm_take_focus;
    Atom net_wm_name, net_wm_state, net_wm_fullscreen;
    Atom net_wm_sticky;
    Atom net_active_window, net_client_list, net_supported;
    Atom net_wm_window_type, net_wm_window_type_dialog;
    Atom net_wm_check;
    Atom utf8_string;

    /* bar / drawing (Xft-backed, mirrors your drw.c: one shared
     * off-screen scratch pixmap, resized as needed, XCopyArea'd onto
     * whichever monitor's barwin is being redrawn) */
    GC gc;
    Visual *visual;
    Colormap cmap;
    unsigned int depth;
    Drawable drawable;          /* shared scratch pixmap for bar rendering */
    unsigned int draww, drawh;  /* current scratch pixmap size */
    XftDraw *xftdraw;           /* bound to `drawable` */
    XftFont *xftfont;           /* single font for now, no fallback chain yet */
    XftColor xftcol[SchemeLast][ColLast];
    int fonth;                  /* font pixel height */
    int barheight;
    int lrpad;                  /* left+right text padding */
    unsigned long col[SchemeLast][ColLast]; /* plain pixel values, for GC rect fills */

    /* cursors */
    Cursor cur_normal, cur_move, cur_resize;

    /* status text shown on the right of the bar (set via WM_NAME on root) */
    char statustext[256];

    /* EWMH support window */
    Window wmcheckwin;

    /* geometry */
    unsigned int borderpx;
    int snap;
    int bx, by, bw, bh;         /* bar geometry (unused, kept for compat) */

    /* config options */
    int resizehints;             /* 1 = respect size hints for tiled windows */
    int lockfullscreen;          /* 1 = prevent focus change from fullscreen */
    int refreshrate;             /* throttle move/resize (ms) */

    /* runtime flags */
    int running;
    int initialized;            /* one-time setup guard, survives reloads */
    volatile int sig_caught;    /* set by signal handler, checked by event loop */

    /* last reload status — host sets before each entry call */
    int reload_count;
};

#endif /* WM_H */
