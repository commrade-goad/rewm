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

/* ---- forward declarations ----------------------------------------------- */
typedef struct Client  Client;
typedef struct Monitor Monitor;
typedef struct WMState WMState;

/* ---- client ------------------------------------------------------------- */
struct Client {
    Window win;
    int x, y, w, h;           /* geometry */
    int basew, baseh, incw, inch, maxw, maxh, minw, minh;
    int bw;                   /* border width */
    unsigned int tags;
    int isfloating, isurgent, isfullscreen;
    Client *next;
    Client *snext;            /* stack pointer */
    Monitor *mon;
};

/* ---- monitor ------------------------------------------------------------ */
struct Monitor {
    int num;
    int mx, my, mw, mh;       /* screen size */
    int wx, wy, ww, wh;       /* window area */
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

    /* monitors & clients */
    Monitor *mons;
    Monitor *selmon;
    Client *clients;
    Client *sel;
    Client *stack;

    /* atoms */
    Atom wm_protocols, wm_delete_window, wm_state, wm_take_focus;
    Atom net_wm_name, net_wm_state, net_wm_fullscreen;
    Atom net_active_window, net_client_list;
    Atom utf8_string;

    /* geometry */
    unsigned int borderpx;
    int snap;
    int bx, by, bw, bh;       /* bar geometry */

    /* runtime flags */
    int running;

    /* last reload status — host sets before each entry call */
    int reload_count;
};

#endif /* WM_H */
