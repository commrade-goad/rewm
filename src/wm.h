#ifndef WM_H
#define WM_H

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xinerama.h>
#include <X11/Xft/Xft.h>
#include <time.h>

#define LENGTH(x)       (sizeof(x) / sizeof(x[0]))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define BETWEEN(x, a, b) ((a) <= (x) && (x) <= (b))
#define CLEANMASK(m)    ((m) & ~(numlockmask|LockMask) & (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))
#define INTERSECT(x,y,w,h,m) \
    (int)(MAX(0, MIN((x)+(w),(m)->wx+(m)->ww) - MAX((x),(m)->wx)) * \
          MAX(0, MIN((y)+(h),(m)->wy+(m)->wh) - MAX((y),(m)->wy)))


enum { REWM_OK, REWM_RELOAD, REWM_QUIT };
enum { SchemeNorm, SchemeSel, SchemeLast };
enum { ColFg, ColBg, ColBorder, ColLast };

typedef struct Client  Client;
typedef struct Monitor Monitor;
typedef struct WMState WMState;

typedef struct Fnt Fnt;
struct Fnt {
    Fnt *next;
    XftFont *xfont;
    FcPattern *pattern;
};

struct Client {
    char name[256];
    Window win;
    int x, y, w, h;              /* geometry */
    int oldx, oldy, oldw, oldh;  /* saved geometry (pre-fullscreen / float toggle) */
    int oldbw;
    int basew, baseh, incw, inch, maxw, maxh, minw, minh;
    float mina, maxa;            /* aspect ratio hints */
    int bw;                      /* border width */
    unsigned int tags;
    int isfloating, isurgent, isfullscreen, issticky;
    int wasfloating;            /* floating state to restore after unfullscreen */
    int isfixed;                /* min==max size -> never resize */
    int neverfocus;             /* WM_HINTS input=False */
    int hintsvalid;             /* size hints cache flag */
    Client *next;               /* monitor client list */
    Client *snext;              /* stack (focus/raise order) */
    Monitor *mon;
};

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
    int barfocused;             /* -1 = unset, else last WM_NAME state we wrote (0/1) */
};

typedef struct {
    const char *symbol;
    void (*arrange)(WMState *, Monitor *);
} Layout;

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

    GC gc;
    Visual *visual;
    Colormap cmap;
    unsigned int depth;
    Drawable drawable;          /* shared scratch pixmap for bar rendering */
    unsigned int draww, drawh;  /* current scratch pixmap size */
    XftDraw *xftdraw;           /* bound to `drawable` */
    Fnt *fonts;                 /* fallback chain, primary font = fonts (head) */
    XftColor xftcol[SchemeLast][ColLast];
    int fonth;                  /* font pixel height */
    int barheight;
    int lrpad;                  /* left+right text padding */
    unsigned long col[SchemeLast][ColLast]; /* plain pixel values, for GC rect fills */

    Cursor cur_normal, cur_move, cur_resize;

    char statustext[256]; // NOTE: if neede make this bigger (for longer status text)
    Window wmcheckwin;

    /* runtime flags */
    int running;
    int initialized;            /* one-time setup guard, survives reloads */
    volatile int sig_caught;    /* set by signal handler, checked by event loop */

    int reload_count;

    /* urgent tag flash */
    int urgent_flash_on;
    long last_flash_ms;
};

#endif /* WM_H */
