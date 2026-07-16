/* wm.c — JIT-compiled window manager logic
 *
 * This file is compiled by c2mir (JIT) every load/reload.  It receives
 * a pointer to WMState which lives on the native heap and survives
 * across reloads.
 *
 * All X11 calls resolve via the import resolver (RTLD_DEFAULT) from
 * host's libX11 linkage.
 */

#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xproto.h>

/* =========================================================================
 *  Config
 * ========================================================================= */
#define MODKEY            Mod4Mask
#define TERMINAL          "st"
#define TAGMASK           ((1 << 9) - 1)

/* forward declarations */
static void    arrange(WMState *s, Monitor *m);
static int     numlockmask;
static void    spawn(WMState *s, const char *cmd);
static Monitor *wintomon(WMState *s, Window w);
static Client  *wintoclient(WMState *s, Window w);
static int     getrootptr(WMState *s, int *x, int *y);
static void    setfocus(WMState *s, Client *c);
static void    grabbuttons(WMState *s, Client *c, int focused);
static void    maprequest(WMState *s, XEvent *e);

/* =========================================================================
 *  Layouts
 * ========================================================================= */
static void tile(WMState *s, Monitor *m);
static void monocle(WMState *s, Monitor *m);
static void floating(WMState *s, Monitor *m);

static Layout layouts[] = {
    { "[]=",  tile },
    { "[M]",  monocle },
    { "><>",  floating },
};
#define LAYOUT_COUNT ((int)(sizeof(layouts)/sizeof(layouts[0])))

/* =========================================================================
 *  Client management
 * ========================================================================= */
static void applysizehints(WMState *s, Client *c, int *x, int *y, int *w, int *h) {
    (void)s;
    /* minimum */
    if (*w < c->minw) *w = c->minw;
    if (*h < c->minh) *h = c->minh;
    /* maximum */
    if (c->maxw && *w > c->maxw) *w = c->maxw;
    if (c->maxh && *h > c->maxh) *h = c->maxh;
    /* base size + resize increments */
    if (c->incw) *w -= (*w - c->basew) % c->incw;
    if (c->inch) *h -= (*h - c->baseh) % c->inch;
}

static void resize(WMState *s, Client *c, int x, int y, int w, int h, int interact) {
    if (interact)
        applysizehints(s, c, &x, &y, &w, &h);
    XMoveResizeWindow(s->dpy, c->win, x, y, w - 2*c->bw, h - 2*c->bw);
    c->x = x; c->y = y; c->w = w; c->h = h;
}

static void focus(WMState *s, Client *c) {
    if (!c || !c->mon->clients)
        c = s->sel;
    if (!c) return;
    s->sel = c;
    XSetInputFocus(s->dpy, c->win, RevertToPointerRoot, CurrentTime);
    XChangeProperty(s->dpy, s->root, s->net_active_window,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&c->win, 1);
}

static void unfocus(WMState *s, Client *c, int setfocus) {
    if (!c) return;
    /* borders handled by callers */
    (void)setfocus;
}

static void setclientstate(WMState *s, Client *c, long state) {
    long data[] = { state, None };
    XChangeProperty(s->dpy, c->win, s->wm_state, s->wm_state, 32,
                    PropModeReplace, (unsigned char *)data, 2);
}

static Client *createclient(WMState *s, Window w) {
    Client *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->win = w;
    c->bw  = s->borderpx;
    /* get geometry */
    XWindowAttributes wa;
    XGetWindowAttributes(s->dpy, w, &wa);
    c->x = wa.x; c->y = wa.y; c->w = wa.width;  c->h = wa.height;
    c->minw = 100; c->minh = 50;
    c->maxw = 9999; c->maxh = 9999;
    c->basew = 0;  c->baseh = 0;
    c->incw  = 1;  c->inch  = 1;
    /* attach to monitor */
    Monitor *mon = wintomon(s, w);
    c->mon = mon;
    c->next = mon->clients;
    mon->clients = c;
    /* append to global list */
    c->next = s->clients;
    s->clients = c;
    setclientstate(s, c, NormalState);
    XSelectInput(s->dpy, w, EnterWindowMask|FocusChangeMask|PropertyChangeMask|StructureNotifyMask);
    grabbuttons(s, c, 1);
    XMapWindow(s->dpy, w);
    setfocus(s, c);
    return c;
}

static void destroyclient(WMState *s, Client *c) {
    if (!c) return;
    /* detach from monitor */
    Monitor *m = c->mon;
    Client **mp;
    for (mp = &m->clients; *mp && *mp != c; mp = &(*mp)->next);
    if (*mp) *mp = c->next;
    /* detach from global list */
    for (mp = &s->clients; *mp && *mp != c; mp = &(*mp)->next);
    if (*mp) *mp = c->next;
    /* detach from stack */
    for (mp = &s->stack; *mp && *mp != c; mp = &(*mp)->snext);
    if (*mp) *mp = c->snext;
    if (s->sel == c) s->sel = NULL;
    free(c);
}

static Client *wintoclient(WMState *s, Window w) {
    Client *c;
    for (c = s->clients; c && c->win != w; c = c->next);
    return c;
}

static Monitor *wintomon(WMState *s, Window w) {
    if (w == s->root) return s->selmon;
    int x, y;
    if (!getrootptr(s, &x, &y)) return s->selmon;
    for (Monitor *m = s->mons; m; m = m->next)
        if (x >= m->mx && x < m->mx + m->mw && y >= m->my && y < m->my + m->mh)
            return m;
    return s->selmon;
}

static Client *nexttiled(Client *c) {
    for (; c && (c->isfloating || c->isfullscreen); c = c->next);
    return c;
}

static void grabbuttons(WMState *s, Client *c, int focused) {
    XUngrabButton(s->dpy, AnyButton, AnyModifier, c->win);
    if (!focused) {
        XGrabButton(s->dpy, AnyButton, AnyModifier, c->win, False,
                    ButtonPressMask, GrabModeSync, GrabModeSync, None, None);
    }
}

/* =========================================================================
 *  Event handlers
 * ========================================================================= */
static void buttonpress(WMState *s, XEvent *e) {
    XButtonPressedEvent *ev = &e->xbutton;
    Client *c = wintoclient(s, ev->window);
    if (c) {
        focus(s, c);
        if (ev->button == Button1)
            XRaiseWindow(s->dpy, c->win);
    }
}

static void keypress(WMState *s, XEvent *e) {
    XKeyPressedEvent *ev = &e->xkey;
    KeySym ks = XKeycodeToKeysym(s->dpy, ev->keycode, 0);
    unsigned int mod = CLEANMASK(ev->state);

    /* ---- keybindings (static table, C11-safe) ---- */
    if (mod == MODKEY) {
        if (ks == XK_Return) {              /* terminal */
            spawn(s, TERMINAL);
        } else if (ks == XK_p) {            /* dmenu */
            spawn(s, "dmenu_run");
        } else if (ks == XK_q) {            /* quit */
            s->running = 0;
        } else if (ks == XK_Tab) {          /* cycle focus */
            if (s->sel && s->sel->next)
                focus(s, s->sel->next);
        } else if (ks == XK_j) {            /* focus next */
            Client *c = s->sel ? s->sel->next : s->clients;
            if (c) focus(s, c);
        } else if (ks == XK_k) {            /* focus prev */
            Client *p = NULL, *c;
            for (c = s->clients; c && c != s->sel; p = c, c = c->next);
            if (p) focus(s, p);
        } else if (ks == XK_h) {            /* decrease master */
            if (s->selmon) s->selmon->tagset[s->selmon->seltags] >>= 1;
            arrange(s, s->selmon);
        } else if (ks == XK_l) {            /* increase master */
            if (s->selmon) s->selmon->tagset[s->selmon->seltags] <<= 1;
            arrange(s, s->selmon);
        }
    }
    if (mod == MODKEY && ks >= XK_1 && ks <= XK_9) { /* tag views */
        unsigned int tag = 1 << (ks - XK_1);
        if (s->selmon) {
            s->selmon->tagset[s->selmon->seltags] = tag;
            arrange(s, s->selmon);
        }
    }
}

static void enternotify(WMState *s, XEvent *e) {
    XCrossingEvent *ev = &e->xcrossing;
    Client *c;
    if (ev->mode != NotifyNormal || ev->detail == NotifyInferior) return;
    if ((c = wintoclient(s, ev->window)))
        focus(s, c);
}

static void expos(WMState *s, XEvent *e) {
    XExposeEvent *ev = &e->xexpose;
    Monitor *m;
    for (m = s->mons; m; m = m->next)
        if (m->barwin == ev->window) break;
}

static void destroynotify(WMState *s, XEvent *e) {
    XDestroyWindowEvent *ev = &e->xdestroywindow;
    Client *c = wintoclient(s, ev->window);
    if (c) destroyclient(s, c);
}

static void maprequest(WMState *s, XEvent *e) {
    XMapRequestEvent *ev = &e->xmaprequest;
    Client *c = wintoclient(s, ev->window);
    if (!c) createclient(s, ev->window);
}

static void configurenotify(WMState *s, XEvent *e) {
    XConfigureEvent *ev = &e->xconfigure;
    /* TODO: handle monitor changes */
    (void)s; (void)ev;
}

static void mappingnotify(WMState *s, XEvent *e) {
    XMappingEvent *ev = &e->xmapping;
    XRefreshKeyboardMapping(ev);
    if (ev->request == MappingKeyboard) {
        XUngrabKey(s->dpy, AnyKey, AnyModifier, s->root);
        /* re-grab keys */
        unsigned int modmasks[] = { 0, LockMask };
        KeySym keys[] = { XK_Tab, XK_Return, XK_p, XK_q, XK_j, XK_k, XK_h, XK_l };
        int nkeys = (int)(sizeof(keys)/sizeof(keys[0]));
        for (int k = 0; k < nkeys; k++)
            for (int m = 0; m < (int)(sizeof(modmasks)/sizeof(modmasks[0])); m++)
                XGrabKey(s->dpy, XKeysymToKeycode(s->dpy, keys[k]),
                         MODKEY | modmasks[m], s->root, True,
                         GrabModeAsync, GrabModeAsync);
        for (KeySym ks = XK_1; ks <= XK_9; ks++)
            for (int m = 0; m < (int)(sizeof(modmasks)/sizeof(modmasks[0])); m++)
                XGrabKey(s->dpy, XKeysymToKeycode(s->dpy, ks),
                         MODKEY | modmasks[m], s->root, True,
                         GrabModeAsync, GrabModeAsync);
    }
}

/* =========================================================================
 *  Utilities
 * ========================================================================= */
static void spawn(WMState *s, const char *cmd) {
    if (fork() == 0) {
        if (s->dpy) close(ConnectionNumber(s->dpy));
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        exit(0);
    }
}

static int getrootptr(WMState *s, int *x, int *y) {
    Window w;
    int di;
    unsigned int dui;
    return XQueryPointer(s->dpy, s->root, &w, &w, x, y, &di, &di, &dui);
}

static void setfocus(WMState *s, Client *c) {
    if (!c) return;
    focus(s, c);
    for (Client *cc = s->clients; cc; cc = cc->next)
        XSetWindowBorder(s->dpy, cc->win,
                         cc == c ? 0xFF0000 : 0x222222);
}

/* static void detachstack(WMState *s, Client *c) {
    Client **p;
    for (p = &s->stack; *p && *p != c; p = &(*p)->snext);
    if (*p) *p = c->snext;
    c->snext = NULL;
} */

/* =========================================================================
 *  Layout implementations
 * ========================================================================= */
static void tile(WMState *s, Monitor *m) {
    int n = 0, i = 0, my, ty;
    Client *c;
    for (c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
    if (n == 0) return;

    /* master area */
    int mw = n > 1 ? m->ww / 2 : m->ww;
    for (c = nexttiled(m->clients), my = ty = 0;
         c; c = nexttiled(c->next), i++) {
        if (i < 1) { /* master client */
            resize(s, c, m->wx, m->wy + my,
                   mw - 2*c->bw, (m->wh - my) / (MIN(n, 1) - i) - 2*c->bw, 0);
            if (my + c->h + 2*c->bw < m->wh)
                my += c->h + 2*c->bw;
        } else { /* stack clients */
            resize(s, c, m->wx + mw, m->wy + ty,
                   m->ww - mw - 2*c->bw, (m->wh - ty) / (n - i) - 2*c->bw, 0);
            if (ty + c->h + 2*c->bw < m->wh)
                ty += c->h + 2*c->bw;
        }
    }
}

static void monocle(WMState *s, Monitor *m) {
    for (Client *c = nexttiled(m->clients); c; c = nexttiled(c->next))
        resize(s, c, m->wx, m->wy, m->ww - 2*c->bw, m->wh - 2*c->bw, 0);
}

static void floating(WMState *s, Monitor *m) {
    (void)s; (void)m;
    /* just leave clients where they are */
}

/* =========================================================================
 *  Arrange
 * ========================================================================= */
static void arrange(WMState *s, Monitor *m) {
    if (!m) m = s->selmon;
    if (!m) return;
    /* apply current layout */
    layouts[m->sellt].arrange(s, m);
}

/* =========================================================================
 *  Entry point — called from host.c after JIT compile
 * ========================================================================= */
int wm_entry(WMState *s) {
    /* ---- grab keys ---------------------------------------------------- */
    unsigned int modmasks[] = { 0, LockMask };
    int nmod = (int)(sizeof(modmasks)/sizeof(modmasks[0]));

    KeySym numlock = XKeysymToKeycode(s->dpy, XK_Num_Lock) ? XK_Num_Lock : 0;
    numlockmask = 0;
    if (numlock) {
        XModifierKeymap *modmap = XGetModifierMapping(s->dpy);
        for (int i = 0; i < 8; i++)
            if (modmap->modifiermap[i] == numlock)
                numlockmask = (1 << i);
        XFreeModifiermap(modmap);
    }

    /* grab base keys */
    KeySym basekeys[] = { XK_Tab, XK_Return, XK_p, XK_q, XK_j, XK_k, XK_h, XK_l };
    for (int k = 0; k < (int)(sizeof(basekeys)/sizeof(basekeys[0])); k++)
        for (int m = 0; m < nmod; m++)
            XGrabKey(s->dpy, XKeysymToKeycode(s->dpy, basekeys[k]),
                     MODKEY | modmasks[m], s->root, True,
                     GrabModeAsync, GrabModeAsync);
    /* grab number keys (tags) */
    for (KeySym ks = XK_1; ks <= XK_9; ks++)
        for (int m = 0; m < nmod; m++)
            XGrabKey(s->dpy, XKeysymToKeycode(s->dpy, ks),
                     MODKEY | modmasks[m], s->root, True,
                     GrabModeAsync, GrabModeAsync);

    /* ---- grab buttons ------------------------------------------------- */
    XUngrabButton(s->dpy, AnyButton, AnyModifier, s->root);
    XGrabButton(s->dpy, Button1, MODKEY, s->root, False, ButtonPressMask,
                GrabModeAsync, GrabModeSync, None, None);
    XGrabButton(s->dpy, Button3, MODKEY, s->root, False, ButtonPressMask,
                GrabModeAsync, GrabModeSync, None, None);

    /* ---- init atoms --------------------------------------------------- */
    s->wm_protocols      = XInternAtom(s->dpy, "WM_PROTOCOLS",      False);
    s->wm_delete_window  = XInternAtom(s->dpy, "WM_DELETE_WINDOW",  False);
    s->wm_state          = XInternAtom(s->dpy, "WM_STATE",          False);
    s->wm_take_focus     = XInternAtom(s->dpy, "WM_TAKE_FOCUS",     False);
    s->net_wm_name       = XInternAtom(s->dpy, "_NET_WM_NAME",      False);
    s->net_wm_state      = XInternAtom(s->dpy, "_NET_WM_STATE",     False);
    s->net_wm_fullscreen = XInternAtom(s->dpy, "_NET_WM_STATE_FULLSCREEN", False);
    s->net_active_window = XInternAtom(s->dpy, "_NET_ACTIVE_WINDOW", False);
    s->net_client_list   = XInternAtom(s->dpy, "_NET_CLIENT_LIST",  False);
    s->utf8_string       = XInternAtom(s->dpy, "UTF8_STRING",       False);

    /* ---- event loop --------------------------------------------------- */
    XEvent ev;
    s->running = 1;
    while (s->running) {
        XNextEvent(s->dpy, &ev);
        switch (ev.type) {
        case ButtonPress:      buttonpress(s, &ev);      break;
        case KeyPress:         keypress(s, &ev);          break;
        case EnterNotify:      enternotify(s, &ev);      break;
        case Expose:           expos(s, &ev);             break;
        case DestroyNotify:    destroynotify(s, &ev);    break;
        case MapRequest:       maprequest(s, &ev);        break;
        case ConfigureNotify:  configurenotify(s, &ev);  break;
        case MappingNotify:    mappingnotify(s, &ev);    break;
        }
    }

    return REWM_QUIT;
}
