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

#define LENGTH(x)       (sizeof(x) / sizeof(x[0]))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define BETWEEN(x, a, b) ((a) <= (x) && (x) <= (b))
#define CLEANMASK(m)    ((m) & ~(numlockmask|LockMask) & (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))

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
    int bw;                     /* border width */
    unsigned int tags;
    int isfloating, isurgent, isfullscreen;
    int wasfloating;            /* floating state to restore after unfullscreen */
    int isfixed;                 /* min==max size -> never resize */
    int neverfocus;               /* WM_HINTS input=False */
    Client *next;                /* monitor client list */
    Client *snext;               /* stack (focus/raise order) */
    Monitor *mon;
};

/* ---- monitor ------------------------------------------------------------ */
struct Monitor {
    int num;
    float mfact;
    int nmaster;
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
    Atom net_active_window, net_client_list, net_supported;
    Atom net_wm_window_type, net_wm_window_type_dialog;
    Atom net_wm_check;
    Atom utf8_string;

    /* bar / drawing (core-font based, no external deps) */
    GC gc;
    XFontStruct *xfont;
    int fonth;                  /* font pixel height */
    int barheight;
    int lrpad;                  /* left+right text padding */
    unsigned long col[SchemeLast][ColLast];

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

    /* runtime flags */
    int running;
    int initialized;            /* one-time setup guard, survives reloads */

    /* last reload status — host sets before each entry call */
    int reload_count;
};

#endif /* WM_H */
