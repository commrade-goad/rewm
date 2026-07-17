/* wm.c — JIT-compiled window manager logic
 *
 * This file is compiled by c2mir (JIT) every load/reload.  It receives
 * a pointer to WMState which lives on the native heap and survives
 * across reloads.  Heavy one-time X resources (GC, font, cursors, the
 * bar window, EWMH check window) are guarded by s->initialized so a
 * hot reload just swaps behaviour, not X state.
 *
 * All X11 calls resolve via the import resolver (RTLD_DEFAULT) from
 * host's libX11 linkage.
 *
 * dwm-alike: tags, master/stack tiling, monocle, floating, a text
 * bar, mouse move/resize (sxwm-style drag), fullscreen, ICCCM/EWMH
 * basics.  No Xft/fontconfig dependency — core X fonts only, so the
 * host link line stays -lX11 -lm -ldl -lpthread.
 */

#define _DEFAULT_SOURCE
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
#define LAUNCHER          "dmenu_run"
#define TAGMASK           ((1 << 9) - 1)
#define FONTNAME          "fixed"
#define BARHEIGHT_PAD     6      /* extra px added to font height for bar */
#define COL_NORM_BG       "#222222"
#define COL_NORM_FG       "#bbbbbb"
#define COL_NORM_BORDER   "#444444"
#define COL_SEL_BG        "#333333"
#define COL_SEL_FG        "#eeeeee"
#define COL_SEL_BORDER    "#5577cc"
#define DEFAULT_MFACT     0.55f
#define DEFAULT_NMASTER   1

static const char *tagnames[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

typedef union {
    int i;
    unsigned int ui;
    float f;
    const void *v;
} Arg;

typedef struct {
    unsigned int mod;
    KeySym keysym;
    void (*func)(WMState *, const Arg *);
    Arg arg;
} Key;

/* forward declarations */
static void arrange(WMState *s, Monitor *m);
static void arrangemon(WMState *s, Monitor *m);
static void restack(WMState *s, Monitor *m);
static int  numlockmask;
static void spawn(WMState *s, const Arg *arg);
static Monitor *wintomon(WMState *s, Window w);
static Client  *wintoclient(WMState *s, Window w);
static int     getrootptr(WMState *s, int *x, int *y);
static void    setfocus(WMState *s, Client *c);
static void    grabbuttons(WMState *s, Client *c, int focused);
static void    maprequest(WMState *s, XEvent *e);
static void    focus(WMState *s, Client *c);
static void    unfocus(WMState *s, Client *c, int setfocus);
static void    drawbar(WMState *s, Monitor *m);
static void    updatetitle(WMState *s, Client *c);
static void    configure(WMState *s, Client *c);
static void    resizeclient(WMState *s, Client *c, int x, int y, int w, int h);
static void    setfullscreen(WMState *s, Client *c, int fullscreen);
static Client  *nexttiled(Client *c);
static void    attach(WMState *s, Client *c);
static void    attachstack(WMState *s, Client *c);
static void    detach(WMState *s, Client *c);
static void    detachstack(WMState *s, Client *c);
static void    syncglobalclients(WMState *s);
static void    updatesizehints(WMState *s, Client *c);
static void    updatewmhints(WMState *s, Client *c);
static void    updatewindowtype(WMState *s, Client *c);
static int     sendevent(WMState *s, Client *c, Atom proto);
static void    unmanage(WMState *s, Client *c, int destroyed);
static void    expose(WMState *s, XEvent *e);

/* ---- key action funcs (bound in `keys[]` below) ------------------------- */
static void togglebar(WMState *s, const Arg *arg);
static void focusstack(WMState *s, const Arg *arg);
static void incnmaster(WMState *s, const Arg *arg);
static void setmfact(WMState *s, const Arg *arg);
static void zoom(WMState *s, const Arg *arg);
static void view(WMState *s, const Arg *arg);
static void toggleview(WMState *s, const Arg *arg);
static void tagclient(WMState *s, const Arg *arg);
static void toggletag(WMState *s, const Arg *arg);
static void killclient(WMState *s, const Arg *arg);
static void togglefloating(WMState *s, const Arg *arg);
static void togglefullscreen(WMState *s, const Arg *arg);
static void setlayout(WMState *s, const Arg *arg);
static void movemouse(WMState *s, const Arg *arg);
static void resizemouse(WMState *s, const Arg *arg);
static void quit(WMState *s, const Arg *arg);
static void reload_wm(WMState *s, const Arg *arg);

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
 *  Keybindings (dwm/sxwm style: MODKEY + key)
 * ========================================================================= */
#define TAGKEYS(KEY,TAG) \
    { MODKEY,               KEY, view,       {.ui = 1 << (TAG)} }, \
    { MODKEY|ControlMask,    KEY, toggleview, {.ui = 1 << (TAG)} }, \
    { MODKEY|ShiftMask,      KEY, tagclient,  {.ui = 1 << (TAG)} }, \
    { MODKEY|ControlMask|ShiftMask, KEY, toggletag, {.ui = 1 << (TAG)} }

static const char *termcmd[]  = { TERMINAL, NULL };
static const char *dmenucmd[] = { LAUNCHER, NULL };

static Key keys[] = {
    { MODKEY,                XK_Return, spawn,          {.v = termcmd} },
    { MODKEY,                XK_p,      spawn,          {.v = dmenucmd} },
    { MODKEY|ShiftMask,      XK_q,      killclient,     {0} },
    { MODKEY|ShiftMask,      XK_e,      quit,           {0} },
    { MODKEY|ShiftMask,      XK_r,      reload_wm,      {0} },
    { MODKEY,                XK_b,      togglebar,      {0} },
    { MODKEY,                XK_j,      focusstack,     {.i = +1} },
    { MODKEY,                XK_k,      focusstack,     {.i = -1} },
    { MODKEY,                XK_Tab,    focusstack,     {.i = +1} },
    { MODKEY,                XK_h,      setmfact,       {.f = -0.05f} },
    { MODKEY,                XK_l,      setmfact,       {.f = +0.05f} },
    { MODKEY,                XK_i,      incnmaster,     {.i = +1} },
    { MODKEY,                XK_d,      incnmaster,     {.i = -1} },
    { MODKEY|ShiftMask,      XK_Return, zoom,           {0} },
    { MODKEY,                XK_space,  setlayout,      {.i = -1} }, /* cycle */
    { MODKEY,                XK_t,      setlayout,      {.i = 0} },
    { MODKEY,                XK_m,      setlayout,      {.i = 1} },
    { MODKEY,                XK_f,      setlayout,      {.i = 2} },
    { MODKEY,                XK_z,      togglefloating, {0} },
    { MODKEY,                XK_u,      togglefullscreen,{0} },
    TAGKEYS(XK_1, 0), TAGKEYS(XK_2, 1), TAGKEYS(XK_3, 2),
    TAGKEYS(XK_4, 3), TAGKEYS(XK_5, 4), TAGKEYS(XK_6, 5),
    TAGKEYS(XK_7, 6), TAGKEYS(XK_8, 7), TAGKEYS(XK_9, 8),
};
#define KEY_COUNT ((int)(sizeof(keys)/sizeof(keys[0])))

/* =========================================================================
 *  Colors / font (core X fonts, no Xft dependency)
 * ========================================================================= */
static unsigned long getcolor(WMState *s, const char *name) {
    XColor c;
    Colormap cmap = DefaultColormap(s->dpy, s->screen);
    if (!XAllocNamedColor(s->dpy, cmap, name, &c, &c))
        return BlackPixel(s->dpy, s->screen);
    return c.pixel;
}

static void setupcolors(WMState *s) {
    s->col[SchemeNorm][ColFg]     = getcolor(s, COL_NORM_FG);
    s->col[SchemeNorm][ColBg]     = getcolor(s, COL_NORM_BG);
    s->col[SchemeNorm][ColBorder] = getcolor(s, COL_NORM_BORDER);
    s->col[SchemeSel][ColFg]      = getcolor(s, COL_SEL_FG);
    s->col[SchemeSel][ColBg]      = getcolor(s, COL_SEL_BG);
    s->col[SchemeSel][ColBorder]  = getcolor(s, COL_SEL_BORDER);
}

static void setupfont(WMState *s) {
    s->xfont = XLoadQueryFont(s->dpy, FONTNAME);
    if (!s->xfont)
        s->xfont = XLoadQueryFont(s->dpy, "fixed");
    if (s->xfont) {
        s->fonth = s->xfont->ascent + s->xfont->descent;
        XSetFont(s->dpy, s->gc, s->xfont->fid);
    } else {
        s->fonth = 12;
    }
    s->lrpad = s->fonth;
    s->barheight = s->fonth + BARHEIGHT_PAD;
}

static void setupcursors(WMState *s) {
    s->cur_normal = XCreateFontCursor(s->dpy, XC_left_ptr);
    s->cur_move   = XCreateFontCursor(s->dpy, XC_fleur);
    s->cur_resize = XCreateFontCursor(s->dpy, XC_sizing);
    XDefineCursor(s->dpy, s->root, s->cur_normal);
}

static int textwidth(WMState *s, const char *text) {
    if (!text || !*text) return s->lrpad;
    if (s->xfont)
        return XTextWidth(s->xfont, text, (int)strlen(text)) + s->lrpad;
    return (int)strlen(text) * 6 + s->lrpad;
}

/* =========================================================================
 *  Bar
 * ========================================================================= */
static void updatebarpos(WMState *s, Monitor *m) {
    m->wy = m->my;
    m->wh = m->mh;
    if (m->showbar) {
        m->wh -= s->barheight;
        m->wy = m->topbar ? m->wy + s->barheight : m->wy;
    }
}

static void drawrect(WMState *s, Window win, int x, int y, int w, int h, unsigned long pixel, int fill) {
    XSetForeground(s->dpy, s->gc, pixel);
    if (fill) XFillRectangle(s->dpy, win, s->gc, x, y, (unsigned)w, (unsigned)h);
    else      XDrawRectangle(s->dpy, win, s->gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
}

static void drawtext(WMState *s, Window win, int x, int y, unsigned long fg, unsigned long bg,
                     int w, int h, const char *text) {
    (void)y;
    XSetForeground(s->dpy, s->gc, bg);
    XFillRectangle(s->dpy, win, s->gc, x, 0, (unsigned)w, (unsigned)h);
    if (!text || !*text) return;
    XSetForeground(s->dpy, s->gc, fg);
    int ty = (h + s->fonth) / 2 - 2;
    XDrawString(s->dpy, win, s->gc, x + s->lrpad / 2, ty, text, (int)strlen(text));
}

static void drawbar(WMState *s, Monitor *m) {
    if (!m || !m->showbar || m->barwin == 0) return;

    int x = 0;
    int occ = 0, urg = 0;
    for (Client *c = m->clients; c; c = c->next) {
        occ |= (int)c->tags;
        if (c->isurgent) urg |= (int)c->tags;
    }

    /* tags */
    for (int i = 0; i < (int)LENGTH(tagnames); i++) {
        int sel = (m->tagset[m->seltags] & (1u << i)) != 0;
        int w = textwidth(s, tagnames[i]);
        unsigned long fg = sel ? s->col[SchemeSel][ColFg] : s->col[SchemeNorm][ColFg];
        unsigned long bg = sel ? s->col[SchemeSel][ColBg] : s->col[SchemeNorm][ColBg];
        drawtext(s, m->barwin, x, 0, fg, bg, w, s->barheight, tagnames[i]);
        if (occ & (1 << i))
            drawrect(s, m->barwin, x + 1, 1, 3, 3, (urg & (1 << i)) ? s->col[SchemeSel][ColBorder] : fg, 1);
        x += w;
    }

    /* layout symbol */
    int lw = textwidth(s, m->ltsymbol);
    drawtext(s, m->barwin, x, 0, s->col[SchemeNorm][ColFg], s->col[SchemeNorm][ColBg], lw, s->barheight, m->ltsymbol);
    x += lw;

    /* status text on far right */
    int sw = textwidth(s, s->statustext);
    int mid = m->ww - sw;
    if (mid < x) mid = x;
    drawtext(s, m->barwin, mid, 0, s->col[SchemeNorm][ColFg], s->col[SchemeNorm][ColBg], m->ww - mid, s->barheight, s->statustext);

    /* selected window title fills the middle */
    int tw = mid - x;
    if (tw > 0) {
        const char *title = (s->sel && s->sel->mon == m) ? s->sel->name : "";
        drawtext(s, m->barwin, x, 0, s->col[SchemeNorm][ColFg], s->col[SchemeNorm][ColBg], tw, s->barheight, title);
    }

    XSync(s->dpy, False);
}

static void updatestatus(WMState *s) {
    XTextProperty tp;
    if (XGetTextProperty(s->dpy, s->root, &tp, XA_WM_NAME) && tp.value) {
        strncpy(s->statustext, (char *)tp.value, sizeof(s->statustext) - 1);
        s->statustext[sizeof(s->statustext) - 1] = '\0';
        XFree(tp.value);
    } else {
        strncpy(s->statustext, "rewm", sizeof(s->statustext) - 1);
    }
}

/* =========================================================================
 *  Client management
 * ========================================================================= */
static void applysizehints(WMState *s, Client *c, int *x, int *y, int *w, int *h, int interact) {
    Monitor *m = c->mon;
    if (interact) {
        if (*x > s->sw) *x = s->sw - c->w;
        if (*y > s->sh) *y = s->sh - c->h;
        if (*x + *w + 2 * c->bw < 0) *x = 0;
        if (*y + *h + 2 * c->bw < 0) *y = 0;
    } else {
        if (*x >= m->wx + m->ww) *x = m->wx + m->ww - c->w;
        if (*y >= m->wy + m->wh) *y = m->wy + m->wh - c->h;
        if (*x + *w + 2 * c->bw <= m->wx) *x = m->wx;
        if (*y + *h + 2 * c->bw <= m->wy) *y = m->wy;
    }
    if (*h < 1) *h = 1;
    if (*w < 1) *w = 1;

    if (c->isfloating || layouts[m->sellt].arrange == floating) {
        if (c->minw && *w < c->minw) *w = c->minw;
        if (c->minh && *h < c->minh) *h = c->minh;
        if (c->maxw && *w > c->maxw) *w = c->maxw;
        if (c->maxh && *h > c->maxh) *h = c->maxh;
        if (c->incw) *w -= (*w - c->basew) % c->incw;
        if (c->inch) *h -= (*h - c->baseh) % c->inch;
    }
}

static void configure(WMState *s, Client *c) {
    XConfigureEvent ce;
    ce.type = ConfigureNotify;
    ce.display = s->dpy;
    ce.event = c->win;
    ce.window = c->win;
    ce.x = c->x; ce.y = c->y;
    ce.width = c->w; ce.height = c->h;
    ce.border_width = c->bw;
    ce.above = None;
    ce.override_redirect = False;
    XSendEvent(s->dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

static void resizeclient(WMState *s, Client *c, int x, int y, int w, int h) {
    c->oldx = c->x; c->oldy = c->y; c->oldw = c->w; c->oldh = c->h;
    c->x = x; c->y = y; c->w = w; c->h = h;
    XMoveResizeWindow(s->dpy, c->win, x, y, (unsigned)MAX(w - 2 * c->bw, 1), (unsigned)MAX(h - 2 * c->bw, 1));
    configure(s, c);
}

static void resize(WMState *s, Client *c, int x, int y, int w, int h, int interact) {
    applysizehints(s, c, &x, &y, &w, &h, interact);
    resizeclient(s, c, x, y, w, h);
}

static int isvisible(Monitor *m, Client *c) {
    return (c->tags & m->tagset[m->seltags]) != 0;
}

static void showhide(WMState *s, Client *c) {
    if (!c) return;
    if (isvisible(c->mon, c)) {
        XMoveWindow(s->dpy, c->win, c->x, c->y);
        if (c->isfloating && !c->isfullscreen)
            resize(s, c, c->x, c->y, c->w, c->h, 0);
        showhide(s, c->snext);
    } else {
        showhide(s, c->snext);
        XMoveWindow(s->dpy, c->win, -2 * c->w - 100, c->y);
    }
}

static void setclientstate(WMState *s, Client *c, long state) {
    long data[] = { state, None };
    XChangeProperty(s->dpy, c->win, s->wm_state, s->wm_state, 32,
                    PropModeReplace, (unsigned char *)data, 2);
}

static int sendevent(WMState *s, Client *c, Atom proto) {
    int exists = 0;
    Atom *protocols;
    int n;
    if (XGetWMProtocols(s->dpy, c->win, &protocols, &n)) {
        for (int i = 0; i < n && !exists; i++)
            exists = (protocols[i] == proto);
        XFree(protocols);
    }
    if (exists) {
        XEvent ev;
        ev.type = ClientMessage;
        ev.xclient.window = c->win;
        ev.xclient.message_type = s->wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)proto;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(s->dpy, c->win, False, NoEventMask, &ev);
    }
    return exists;
}

static void setfocus(WMState *s, Client *c) {
    if (!c->neverfocus) {
        XSetInputFocus(s->dpy, c->win, RevertToPointerRoot, CurrentTime);
        XChangeProperty(s->dpy, s->root, s->net_active_window, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *)&c->win, 1);
    }
    sendevent(s, c, s->wm_take_focus);
}

static void unfocus(WMState *s, Client *c, int setfocusflag) {
    if (!c) return;
    grabbuttons(s, c, 0);
    XSetWindowBorder(s->dpy, c->win, s->col[SchemeNorm][ColBorder]);
    if (setfocusflag) {
        XSetInputFocus(s->dpy, s->root, RevertToPointerRoot, CurrentTime);
        XDeleteProperty(s->dpy, s->root, s->net_active_window);
    }
}

static void focus(WMState *s, Client *c) {
    if (!c || !isvisible(c->mon, c))
        for (c = s->selmon->stack; c && !isvisible(c->mon, c); c = c->snext);
    if (s->sel && s->sel != c)
        unfocus(s, s->sel, 0);
    if (c) {
        if (c->mon != s->selmon) s->selmon = c->mon;
        if (c->isurgent) c->isurgent = 0;
        detachstack(s, c);
        attachstack(s, c);
        grabbuttons(s, c, 1);
        XSetWindowBorder(s->dpy, c->win, s->col[SchemeSel][ColBorder]);
        setfocus(s, c);
    } else {
        XSetInputFocus(s->dpy, s->root, RevertToPointerRoot, CurrentTime);
        XDeleteProperty(s->dpy, s->root, s->net_active_window);
    }
    s->sel = c;
    drawbar(s, s->selmon);
}

static void grabbuttons(WMState *s, Client *c, int focused) {
    XUngrabButton(s->dpy, AnyButton, AnyModifier, c->win);
    if (!focused)
        XGrabButton(s->dpy, AnyButton, AnyModifier, c->win, False,
                    ButtonPressMask, GrabModeSync, GrabModeSync, None, None);
    unsigned int mods[] = { 0, LockMask, numlockmask, numlockmask | LockMask };
    for (int i = 0; i < (int)LENGTH(mods); i++) {
        XGrabButton(s->dpy, Button1, MODKEY | mods[i], c->win, False,
                    ButtonPressMask | ButtonMotionMask | ButtonReleaseMask,
                    GrabModeAsync, GrabModeAsync, None, None);
        XGrabButton(s->dpy, Button3, MODKEY | mods[i], c->win, False,
                    ButtonPressMask | ButtonMotionMask | ButtonReleaseMask,
                    GrabModeAsync, GrabModeAsync, None, None);
    }
}

static void grabkeys(WMState *s) {
    XUngrabKey(s->dpy, AnyKey, AnyModifier, s->root);
    unsigned int mods[] = { 0, LockMask, numlockmask, numlockmask | LockMask };
    for (int i = 0; i < KEY_COUNT; i++) {
        KeyCode code = XKeysymToKeycode(s->dpy, keys[i].keysym);
        if (!code) continue;
        for (int j = 0; j < (int)LENGTH(mods); j++)
            XGrabKey(s->dpy, code, keys[i].mod | mods[j], s->root, True,
                     GrabModeAsync, GrabModeAsync);
    }
}

static void updatenumlockmask(WMState *s) {
    numlockmask = 0;
    XModifierKeymap *modmap = XGetModifierMapping(s->dpy);
    KeyCode nl = XKeysymToKeycode(s->dpy, XK_Num_Lock);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < modmap->max_keypermod; j++)
            if (modmap->modifiermap[i * modmap->max_keypermod + j] == nl)
                numlockmask = (1 << i);
    XFreeModifiermap(modmap);
}

static void updatetitle(WMState *s, Client *c) {
    XTextProperty tp;
    if (XGetTextProperty(s->dpy, c->win, &tp, s->net_wm_name) && tp.value) {
        strncpy(c->name, (char *)tp.value, sizeof(c->name) - 1);
        c->name[sizeof(c->name) - 1] = '\0';
        XFree(tp.value);
        return;
    }
    if (XGetTextProperty(s->dpy, c->win, &tp, XA_WM_NAME) && tp.value) {
        strncpy(c->name, (char *)tp.value, sizeof(c->name) - 1);
        c->name[sizeof(c->name) - 1] = '\0';
        XFree(tp.value);
        return;
    }
    strncpy(c->name, "broken", sizeof(c->name) - 1);
}

static void updatesizehints(WMState *s, Client *c) {
    long msize;
    XSizeHints hints;
    if (!XGetWMNormalHints(s->dpy, c->win, &hints, &msize))
        hints.flags = 0;
    if (hints.flags & PBaseSize) { c->basew = hints.base_width; c->baseh = hints.base_height; }
    else if (hints.flags & PMinSize) { c->basew = hints.min_width; c->baseh = hints.min_height; }
    else { c->basew = c->baseh = 0; }
    if (hints.flags & PResizeInc) { c->incw = hints.width_inc; c->inch = hints.height_inc; }
    else { c->incw = c->inch = 0; }
    if (hints.flags & PMaxSize) { c->maxw = hints.max_width; c->maxh = hints.max_height; }
    else { c->maxw = c->maxh = 0; }
    if (hints.flags & PMinSize) { c->minw = hints.min_width; c->minh = hints.min_height; }
    else if (hints.flags & PBaseSize) { c->minw = hints.base_width; c->minh = hints.base_height; }
    else { c->minw = c->minh = 0; }
    c->isfixed = (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh);
}

static void updatewmhints(WMState *s, Client *c) {
    XWMHints *wmh = XGetWMHints(s->dpy, c->win);
    if (!wmh) return;
    if (c == s->sel && (wmh->flags & XUrgencyHint)) {
        wmh->flags &= ~XUrgencyHint;
        XSetWMHints(s->dpy, c->win, wmh);
    } else {
        c->isurgent = (wmh->flags & XUrgencyHint) ? 1 : 0;
    }
    c->neverfocus = (wmh->flags & InputHint) ? !wmh->input : 0;
    XFree(wmh);
}

static void updatewindowtype(WMState *s, Client *c) {
    Atom actual; int format; unsigned long n, extra;
    unsigned char *data = NULL;
    if (XGetWindowProperty(s->dpy, c->win, s->net_wm_window_type, 0, 1, False, XA_ATOM,
                           &actual, &format, &n, &extra, &data) == Success && data) {
        Atom t = *(Atom *)data;
        XFree(data);
        if (t == s->net_wm_window_type_dialog)
            c->isfloating = 1;
    }
    data = NULL;
    if (XGetWindowProperty(s->dpy, c->win, s->net_wm_state, 0, 1, False, XA_ATOM,
                           &actual, &format, &n, &extra, &data) == Success && data) {
        Atom t = *(Atom *)data;
        XFree(data);
        if (t == s->net_wm_fullscreen)
            setfullscreen(s, c, 1);
    }
}

static void attach(WMState *s, Client *c) {
    c->next = c->mon->clients;
    c->mon->clients = c;
    syncglobalclients(s);
}

static void detach(WMState *s, Client *c) {
    Client **tc;
    for (tc = &c->mon->clients; *tc && *tc != c; tc = &(*tc)->next);
    if (*tc) *tc = c->next;
    syncglobalclients(s);
}

static void attachstack(WMState *s, Client *c) {
    (void)s;
    c->snext = c->mon->stack;
    c->mon->stack = c;
}

static void detachstack(WMState *s, Client *c) {
    (void)s;
    Client **tc;
    for (tc = &c->mon->stack; *tc && *tc != c; tc = &(*tc)->snext);
    if (*tc) *tc = c->snext;
}

static void syncglobalclients(WMState *s) {
    /* single-monitor mirror, kept for _NET_CLIENT_LIST + external readers */
    s->clients = s->mons ? s->mons->clients : NULL;
    s->stack = s->mons ? s->mons->stack : NULL;
    XDeleteProperty(s->dpy, s->root, s->net_client_list);
    for (Client *c = s->clients; c; c = c->next)
        XChangeProperty(s->dpy, s->root, s->net_client_list, XA_WINDOW, 32,
                        PropModeAppend, (unsigned char *)&c->win, 1);
}

static Client *nexttiled(Client *c) {
    for (; c && (c->isfloating || !isvisible(c->mon, c)); c = c->next);
    return c;
}

static Monitor *wintomon(WMState *s, Window w) {
    if (w == s->root) return s->selmon;
    Client *c = wintoclient(s, w);
    if (c) return c->mon;
    return s->selmon;
}

static Client *wintoclient(WMState *s, Window w) {
    for (Monitor *m = s->mons; m; m = m->next)
        for (Client *c = m->clients; c; c = c->next)
            if (c->win == w) return c;
    return NULL;
}

static void seturgent(WMState *s, Client *c, int urg) {
    c->isurgent = urg;
    XWMHints *wmh = XGetWMHints(s->dpy, c->win);
    if (!wmh) return;
    wmh->flags = urg ? (wmh->flags | XUrgencyHint) : (wmh->flags & ~XUrgencyHint);
    XSetWMHints(s->dpy, c->win, wmh);
    XFree(wmh);
}

static void setfullscreen(WMState *s, Client *c, int fullscreen) {
    if (fullscreen && !c->isfullscreen) {
        XChangeProperty(s->dpy, c->win, s->net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&s->net_wm_fullscreen, 1);
        c->isfullscreen = 1;
        c->wasfloating = c->isfloating;
        c->oldbw = c->bw;
        c->bw = 0;
        c->isfloating = 1;
        resizeclient(s, c, c->mon->mx, c->mon->my, c->mon->mw, c->mon->mh);
        XRaiseWindow(s->dpy, c->win);
    } else if (!fullscreen && c->isfullscreen) {
        XChangeProperty(s->dpy, c->win, s->net_wm_state, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)0, 0);
        c->isfullscreen = 0;
        c->isfloating = c->wasfloating;
        c->bw = c->oldbw;
        c->x = c->oldx; c->y = c->oldy; c->w = c->oldw; c->h = c->oldh;
        resizeclient(s, c, c->x, c->y, c->w, c->h);
        arrange(s, c->mon);
    }
}

static Client *createclient(WMState *s, Window w, XWindowAttributes *wa) {
    Client *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->win = w;
    c->x = wa->x; c->y = wa->y; c->w = wa->width; c->h = wa->height;
    c->oldx = c->x; c->oldy = c->y; c->oldw = c->w; c->oldh = c->h;
    c->bw = (int)s->borderpx;
    c->oldbw = wa->border_width;
    c->mon = s->selmon;
    c->tags = c->mon->tagset[c->mon->seltags];

    updatetitle(s, c);
    updatesizehints(s, c);
    updatewmhints(s, c);

    Window trans = None;
    if (XGetTransientForHint(s->dpy, w, &trans) && wintoclient(s, trans))
        c->isfloating = 1;
    updatewindowtype(s, c);

    if (c->isfloating || c->isfixed) {
        c->x = MAX(c->x, c->mon->wx);
        c->y = MAX(c->y, c->mon->wy);
    }

    XSetWindowBorderWidth(s->dpy, w, (unsigned)c->bw);
    XSetWindowBorder(s->dpy, w, s->col[SchemeNorm][ColBorder]);
    configure(s, c);
    XSelectInput(s->dpy, w, EnterWindowMask | FocusChangeMask | PropertyChangeMask | StructureNotifyMask);
    grabbuttons(s, c, 0);
    if (!c->isfloating) c->isfloating = c->isfixed;

    attach(s, c);
    attachstack(s, c);

    XChangeProperty(s->dpy, s->root, s->net_client_list, XA_WINDOW, 32,
                    PropModeAppend, (unsigned char *)&w, 1);
    XMoveResizeWindow(s->dpy, c->win, c->x, c->y, (unsigned)c->w, (unsigned)c->h);
    setclientstate(s, c, NormalState);
    XMapWindow(s->dpy, w);
    arrange(s, c->mon);
    focus(s, c);
    return c;
}

static void unmanage(WMState *s, Client *c, int destroyed) {
    Monitor *m = c->mon;
    detach(s, c);
    detachstack(s, c);
    if (!destroyed) {
        XGrabServer(s->dpy);
        XSelectInput(s->dpy, c->win, NoEventMask);
        XUngrabButton(s->dpy, AnyButton, AnyModifier, c->win);
        setclientstate(s, c, WithdrawnState);
        XSync(s->dpy, False);
        XUngrabServer(s->dpy);
    }
    if (s->sel == c) s->sel = NULL;
    free(c);
    focus(s, NULL);
    arrange(s, m);
}

/* =========================================================================
 *  Event handlers
 * ========================================================================= */
static void buttonpress(WMState *s, XEvent *e) {
    XButtonPressedEvent *ev = &e->xbutton;

    for (Monitor *m = s->mons; m; m = m->next) {
        if (ev->window == m->barwin) {
            s->selmon = m;
            int x = 0;
            for (int i = 0; i < (int)LENGTH(tagnames); i++) {
                int w = textwidth(s, tagnames[i]);
                if (ev->x >= x && ev->x < x + w) {
                    Arg a;
                    a.ui = 1u << i;
                    if (ev->button == Button1) view(s, &a);
                    else if (ev->button == Button3) toggleview(s, &a);
                    return;
                }
                x += w;
            }
            return;
        }
    }

    Client *c = wintoclient(s, ev->window);
    if (!c) return;

    focus(s, c);
    restack(s, s->selmon);
    XAllowEvents(s->dpy, ReplayPointer, CurrentTime);

    unsigned int cleanmod = CLEANMASK(ev->state);
    if (cleanmod == CLEANMASK(MODKEY) && ev->button == Button1) {
        Arg a; a.i = 0;
        movemouse(s, &a);
    } else if (cleanmod == CLEANMASK(MODKEY) && ev->button == Button3) {
        Arg a; a.i = 0;
        resizemouse(s, &a);
    }
}

static void keypress(WMState *s, XEvent *e) {
    XKeyPressedEvent *ev = &e->xkey;
    KeySym ks = XKeycodeToKeysym(s->dpy, ev->keycode, 0);
    unsigned int mod = CLEANMASK(ev->state);

    for (int i = 0; i < KEY_COUNT; i++) {
        if (ks == keys[i].keysym && mod == CLEANMASK(keys[i].mod) && keys[i].func) {
            keys[i].func(s, &keys[i].arg);
            return;
        }
    }
}

static void enternotify(WMState *s, XEvent *e) {
    XCrossingEvent *ev = &e->xcrossing;
    if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) && ev->window != s->root)
        return;
    Client *c = wintoclient(s, ev->window);
    Monitor *m = c ? c->mon : wintomon(s, ev->window);
    if (m != s->selmon) s->selmon = m;
    else if (!c || c == s->sel) return;
    focus(s, c);
}

static void expose(WMState *s, XEvent *e) {
    XExposeEvent *ev = &e->xexpose;
    if (ev->count == 0)
        for (Monitor *m = s->mons; m; m = m->next)
            if (m->barwin == ev->window) drawbar(s, m);
}

static void destroynotify(WMState *s, XEvent *e) {
    XDestroyWindowEvent *ev = &e->xdestroywindow;
    Client *c = wintoclient(s, ev->window);
    if (c) unmanage(s, c, 1);
}

static void unmapnotify(WMState *s, XEvent *e) {
    XUnmapEvent *ev = &e->xunmap;
    Client *c = wintoclient(s, ev->window);
    if (c && !ev->send_event) unmanage(s, c, 0);
}

static void maprequest(WMState *s, XEvent *e) {
    XMapRequestEvent *ev = &e->xmaprequest;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(s->dpy, ev->window, &wa)) return;
    if (wa.override_redirect) return;
    if (wintoclient(s, ev->window)) return;
    createclient(s, ev->window, &wa);
}

static void configurerequest(WMState *s, XEvent *e) {
    XConfigureRequestEvent *ev = &e->xconfigurerequest;
    Client *c = wintoclient(s, ev->window);
    if (c) {
        if (ev->value_mask & CWBorderWidth) c->bw = ev->border_width;
        if (c->isfloating || layouts[c->mon->sellt].arrange == floating) {
            if (ev->value_mask & CWX) c->x = c->mon->mx + ev->x;
            if (ev->value_mask & CWY) c->y = c->mon->my + ev->y;
            if (ev->value_mask & CWWidth) c->w = ev->width;
            if (ev->value_mask & CWHeight) c->h = ev->height;
            resizeclient(s, c, c->x, c->y, c->w, c->h);
        } else {
            configure(s, c);
        }
    } else {
        XWindowChanges wc;
        wc.x = ev->x; wc.y = ev->y;
        wc.width = ev->width; wc.height = ev->height;
        wc.border_width = ev->border_width;
        wc.sibling = ev->above;
        wc.stack_mode = ev->detail;
        XConfigureWindow(s->dpy, ev->window, (unsigned)ev->value_mask, &wc);
    }
    XSync(s->dpy, False);
}

static void configurenotify(WMState *s, XEvent *e) {
    XConfigureEvent *ev = &e->xconfigure;
    if (ev->window != s->root) return;
    s->sw = ev->width;
    s->sh = ev->height;
    if (s->selmon) {
        s->selmon->mw = s->selmon->ww = ev->width;
        s->selmon->mh = ev->height;
        updatebarpos(s, s->selmon);
        XMoveResizeWindow(s->dpy, s->selmon->barwin, s->selmon->mx,
                          s->selmon->topbar ? s->selmon->my : s->selmon->my + s->selmon->wh,
                          (unsigned)s->selmon->mw, (unsigned)s->barheight);
        arrange(s, s->selmon);
    }
}

static void propertynotify(WMState *s, XEvent *e) {
    XPropertyEvent *ev = &e->xproperty;
    if (ev->window == s->root && ev->atom == XA_WM_NAME) {
        updatestatus(s);
        drawbar(s, s->selmon);
        return;
    }
    if (ev->state == PropertyDelete) return;
    Client *c = wintoclient(s, ev->window);
    if (!c) return;
    if (ev->atom == XA_WM_HINTS) {
        updatewmhints(s, c);
        drawbar(s, c->mon);
    } else if (ev->atom == XA_WM_NORMAL_HINTS) {
        updatesizehints(s, c);
    } else if (ev->atom == s->net_wm_name || ev->atom == XA_WM_NAME) {
        updatetitle(s, c);
        if (c == s->sel) drawbar(s, c->mon);
    }
}

static void clientmessage(WMState *s, XEvent *e) {
    XClientMessageEvent *cme = &e->xclient;
    Client *c = wintoclient(s, cme->window);
    if (!c) return;
    if (cme->message_type == s->net_wm_state) {
        if ((Atom)cme->data.l[1] == s->net_wm_fullscreen || (Atom)cme->data.l[2] == s->net_wm_fullscreen)
            setfullscreen(s, c, (cme->data.l[0] == 1 || (cme->data.l[0] == 2 && !c->isfullscreen)));
    } else if (cme->message_type == s->net_active_window) {
        if (c != s->sel && !c->isurgent) seturgent(s, c, 1);
    }
}

static void mappingnotify(WMState *s, XEvent *e) {
    XMappingEvent *ev = &e->xmapping;
    XRefreshKeyboardMapping(ev);
    if (ev->request == MappingKeyboard)
        grabkeys(s);
}

/* =========================================================================
 *  Utilities
 * ========================================================================= */
static void spawn(WMState *s, const Arg *arg) {
    if (fork() == 0) {
        if (s->dpy) close(ConnectionNumber(s->dpy));
        setsid();
        const char **argv = (const char **)arg->v;
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "rewm: execvp '%s' failed\n", argv[0]);
        exit(1);
    }
}

static int getrootptr(WMState *s, int *x, int *y) {
    Window w;
    int di;
    unsigned int dui;
    return XQueryPointer(s->dpy, s->root, &w, &w, x, y, &di, &di, &dui);
}

static void restack(WMState *s, Monitor *m) {
    drawbar(s, m);
    if (!s->sel || s->sel->mon != m) return;
    if (s->sel->isfloating || layouts[m->sellt].arrange == floating)
        XRaiseWindow(s->dpy, s->sel->win);
    if (layouts[m->sellt].arrange != floating) {
        XWindowChanges wc;
        wc.stack_mode = Below;
        wc.sibling = m->barwin;
        for (Client *c = m->stack; c; c = c->snext)
            if (!c->isfloating && isvisible(m, c)) {
                XConfigureWindow(s->dpy, c->win, CWSibling | CWStackMode, &wc);
                wc.sibling = c->win;
            }
    }
}

/* =========================================================================
 *  Layout implementations
 * ========================================================================= */
static void tile(WMState *s, Monitor *m) {
    int n = 0;
    for (Client *c = nexttiled(m->clients); c; c = nexttiled(c->next)) n++;
    if (n == 0) return;

    int nmaster = m->nmaster;
    if (nmaster < 0) nmaster = 0;

    int mw = (nmaster == 0) ? 0 : ((n > nmaster) ? (int)(m->ww * m->mfact) : m->ww);

    int i = 0, my = 0, ty = 0;
    for (Client *c = nexttiled(m->clients); c; c = nexttiled(c->next), i++) {
        if (i < nmaster) {
            int h = (m->wh - my) / (MIN(n, nmaster) - i);
            resize(s, c, m->wx, m->wy + my, mw - 2 * c->bw, h - 2 * c->bw, 0);
            if (my + c->h + 2 * c->bw < m->wh) my += c->h + 2 * c->bw;
        } else {
            int h = (m->wh - ty) / (n - i);
            resize(s, c, m->wx + mw, m->wy + ty, m->ww - mw - 2 * c->bw, h - 2 * c->bw, 0);
            if (ty + c->h + 2 * c->bw < m->wh) ty += c->h + 2 * c->bw;
        }
    }
}

static void monocle(WMState *s, Monitor *m) {
    int n = 0;
    for (Client *c = m->clients; c; c = c->next)
        if (isvisible(m, c)) n++;
    if (n > 0)
        snprintf(m->ltsymbol, sizeof(m->ltsymbol), "[%d]", n);
    for (Client *c = nexttiled(m->clients); c; c = nexttiled(c->next))
        resize(s, c, m->wx, m->wy, m->ww - 2 * c->bw, m->wh - 2 * c->bw, 0);
}

static void floating(WMState *s, Monitor *m) {
    for (Client *c = m->clients; c; c = c->next)
        if (isvisible(m, c))
            resize(s, c, c->x, c->y, c->w, c->h, 1);
}

/* =========================================================================
 *  Arrange
 * ========================================================================= */
static void arrangemon(WMState *s, Monitor *m) {
    strncpy(m->ltsymbol, layouts[m->sellt].symbol, sizeof(m->ltsymbol) - 1);
    if (layouts[m->sellt].arrange) layouts[m->sellt].arrange(s, m);
}

static void arrange(WMState *s, Monitor *m) {
    if (m) {
        showhide(s, m->stack);
        arrangemon(s, m);
        restack(s, m);
    } else {
        for (Monitor *mm = s->mons; mm; mm = mm->next) showhide(s, mm->stack);
        for (Monitor *mm = s->mons; mm; mm = mm->next) arrangemon(s, mm);
        drawbar(s, s->selmon);
    }
}

/* =========================================================================
 *  Key action implementations
 * ========================================================================= */
static void togglebar(WMState *s, const Arg *arg) {
    (void)arg;
    Monitor *m = s->selmon;
    m->showbar = !m->showbar;
    updatebarpos(s, m);
    XMoveResizeWindow(s->dpy, m->barwin, m->mx, m->topbar ? m->my : m->my + m->wh,
                      (unsigned)m->ww, (unsigned)s->barheight);
    if (!m->showbar) XUnmapWindow(s->dpy, m->barwin);
    else XMapWindow(s->dpy, m->barwin);
    arrange(s, m);
}

static void focusstack(WMState *s, const Arg *arg) {
    Client *c = NULL;
    if (!s->sel) return;
    if (arg->i > 0) {
        for (c = s->sel->next; c && !isvisible(c->mon, c); c = c->next);
        if (!c) for (c = s->selmon->clients; c && !isvisible(c->mon, c); c = c->next);
    } else {
        Client *i;
        for (i = s->selmon->clients; i != s->sel; i = i->next)
            if (isvisible(i->mon, i)) c = i;
        if (!c) for (; i; i = i->next) if (isvisible(i->mon, i)) c = i;
    }
    if (c) { focus(s, c); restack(s, s->selmon); }
}

static void incnmaster(WMState *s, const Arg *arg) {
    s->selmon->nmaster = MAX(s->selmon->nmaster + arg->i, 0);
    arrange(s, s->selmon);
}

static void setmfact(WMState *s, const Arg *arg) {
    float f = arg->f + s->selmon->mfact;
    if (f < 0.05f || f > 0.95f) return;
    s->selmon->mfact = f;
    arrange(s, s->selmon);
}

static void zoom(WMState *s, const Arg *arg) {
    (void)arg;
    Client *c = s->sel;
    if (!c || layouts[s->selmon->sellt].arrange == floating || c->isfloating)
        return;
    if (c == nexttiled(s->selmon->clients))
        if (!c || !(c = nexttiled(c->next))) return;
    detach(s, c);
    attach(s, c);
    focus(s, c);
    arrange(s, c->mon);
}

static void view(WMState *s, const Arg *arg) {
    if ((arg->ui & TAGMASK) == s->selmon->tagset[s->selmon->seltags]) return;
    s->selmon->seltags ^= 1;
    if (arg->ui & TAGMASK) s->selmon->tagset[s->selmon->seltags] = arg->ui & TAGMASK;
    focus(s, NULL);
    arrange(s, s->selmon);
}

static void toggleview(WMState *s, const Arg *arg) {
    unsigned int newtags = s->selmon->tagset[s->selmon->seltags] ^ (arg->ui & TAGMASK);
    if (!newtags) return;
    s->selmon->tagset[s->selmon->seltags] = newtags;
    focus(s, NULL);
    arrange(s, s->selmon);
}

static void tagclient(WMState *s, const Arg *arg) {
    if (!s->sel || !(arg->ui & TAGMASK)) return;
    s->sel->tags = arg->ui & TAGMASK;
    focus(s, NULL);
    arrange(s, s->selmon);
}

static void toggletag(WMState *s, const Arg *arg) {
    if (!s->sel) return;
    unsigned int newtags = s->sel->tags ^ (arg->ui & TAGMASK);
    if (!newtags) return;
    s->sel->tags = newtags;
    focus(s, NULL);
    arrange(s, s->selmon);
}

static void killclient(WMState *s, const Arg *arg) {
    (void)arg;
    if (!s->sel) return;
    if (!sendevent(s, s->sel, s->wm_delete_window)) {
        XGrabServer(s->dpy);
        XSetCloseDownMode(s->dpy, DestroyAll);
        XKillClient(s->dpy, s->sel->win);
        XSync(s->dpy, False);
        XUngrabServer(s->dpy);
    }
}

static void togglefloating(WMState *s, const Arg *arg) {
    (void)arg;
    if (!s->sel || s->sel->isfullscreen) return;
    s->sel->isfloating = !s->sel->isfloating || s->sel->isfixed;
    if (s->sel->isfloating)
        resize(s, s->sel, s->sel->x, s->sel->y, s->sel->w, s->sel->h, 0);
    arrange(s, s->selmon);
}

static void togglefullscreen(WMState *s, const Arg *arg) {
    (void)arg;
    if (!s->sel) return;
    setfullscreen(s, s->sel, !s->sel->isfullscreen);
}

static void setlayout(WMState *s, const Arg *arg) {
    if (arg->i < 0)
        s->selmon->sellt = (s->selmon->sellt + 1) % LAYOUT_COUNT;
    else if (arg->i < LAYOUT_COUNT)
        s->selmon->sellt = arg->i;
    strncpy(s->selmon->ltsymbol, layouts[s->selmon->sellt].symbol, sizeof(s->selmon->ltsymbol) - 1);
    if (s->sel) arrange(s, s->selmon);
    else drawbar(s, s->selmon);
}

/* ---- mouse move/resize (sxwm-style click-drag with edge snapping) ------- */
static void movemouse(WMState *s, const Arg *arg) {
    (void)arg;
    Client *c = s->sel;
    if (!c || c->isfullscreen) return;
    restack(s, s->selmon);
    int ocx = c->x, ocy = c->y;
    if (XGrabPointer(s->dpy, s->root, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, s->cur_move, CurrentTime) != GrabSuccess)
        return;
    int x0, y0;
    if (!getrootptr(s, &x0, &y0)) { XUngrabPointer(s->dpy, CurrentTime); return; }

    XEvent ev;
    do {
        XMaskEvent(s->dpy, ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask | SubstructureRedirectMask, &ev);
        if (ev.type == MotionNotify) {
            int nx = ocx + (ev.xmotion.x - x0);
            int ny = ocy + (ev.xmotion.y - y0);
            if (abs((s->selmon->wx) - nx) < s->snap) nx = s->selmon->wx;
            else if (abs((s->selmon->wx + s->selmon->ww) - (nx + c->w)) < s->snap)
                nx = s->selmon->wx + s->selmon->ww - c->w;
            if (abs(s->selmon->wy - ny) < s->snap) ny = s->selmon->wy;
            else if (abs((s->selmon->wy + s->selmon->wh) - (ny + c->h)) < s->snap)
                ny = s->selmon->wy + s->selmon->wh - c->h;
            if (!c->isfloating && layouts[s->selmon->sellt].arrange != floating
                && (abs(nx - c->x) > s->snap || abs(ny - c->y) > s->snap)) {
                c->isfloating = 1;
                arrange(s, s->selmon);
            }
            if (c->isfloating || layouts[s->selmon->sellt].arrange == floating)
                resize(s, c, nx, ny, c->w, c->h, 1);
        } else if (ev.type == Expose) {
            expose(s, &ev);
        } else if (ev.type == MapRequest) {
            maprequest(s, &ev);
        }
    } while (ev.type != ButtonRelease);
    XUngrabPointer(s->dpy, CurrentTime);
}

static void resizemouse(WMState *s, const Arg *arg) {
    (void)arg;
    Client *c = s->sel;
    if (!c || c->isfullscreen) return;
    restack(s, s->selmon);
    if (XGrabPointer(s->dpy, s->root, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, s->cur_resize, CurrentTime) != GrabSuccess)
        return;
    XWarpPointer(s->dpy, None, c->win, 0, 0, 0, 0, c->w - 1, c->h - 1);

    XEvent ev;
    do {
        XMaskEvent(s->dpy, ButtonPressMask | ButtonReleaseMask | PointerMotionMask | ExposureMask | SubstructureRedirectMask, &ev);
        if (ev.type == MotionNotify) {
            int nw = MAX(ev.xmotion.x - c->x + 1, 1);
            int nh = MAX(ev.xmotion.y - c->y + 1, 1);
            if (!c->isfloating && layouts[s->selmon->sellt].arrange != floating
                && (abs(nw - c->w) > s->snap || abs(nh - c->h) > s->snap)) {
                c->isfloating = 1;
                arrange(s, s->selmon);
            }
            if (c->isfloating || layouts[s->selmon->sellt].arrange == floating)
                resize(s, c, c->x, c->y, nw, nh, 1);
        } else if (ev.type == Expose) {
            expose(s, &ev);
        } else if (ev.type == MapRequest) {
            maprequest(s, &ev);
        }
    } while (ev.type != ButtonRelease);
    XWarpPointer(s->dpy, None, c->win, 0, 0, 0, 0, c->w - 1, c->h - 1);
    XUngrabPointer(s->dpy, CurrentTime);
}

/* ---- lifecycle ------------------------------------------------------------ */
static int exitcode = REWM_QUIT;

static void quit(WMState *s, const Arg *arg) {
    (void)arg;
    exitcode = REWM_QUIT;
    s->running = 0;
}

static void reload_wm(WMState *s, const Arg *arg) {
    (void)arg;
    exitcode = REWM_RELOAD;
    s->running = 0;
}

/* =========================================================================
 *  One-time setup (survives hot reloads via s->initialized)
 * ========================================================================= */
static void scanwindows(WMState *s) {
    Window root_return, parent_return, *wins = NULL;
    unsigned int n = 0;
    if (!XQueryTree(s->dpy, s->root, &root_return, &parent_return, &wins, &n)) return;
    for (unsigned int i = 0; i < n; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(s->dpy, wins[i], &wa)) continue;
        if (wa.override_redirect) continue;
        if (wa.map_state != IsViewable) continue;
        if (wintoclient(s, wins[i])) continue;
        createclient(s, wins[i], &wa);
    }
    if (wins) XFree(wins);
}

static void setupewmh(WMState *s) {
    s->net_supported             = XInternAtom(s->dpy, "_NET_SUPPORTED", False);
    s->net_wm_window_type        = XInternAtom(s->dpy, "_NET_WM_WINDOW_TYPE", False);
    s->net_wm_window_type_dialog = XInternAtom(s->dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    s->net_wm_check              = XInternAtom(s->dpy, "_NET_SUPPORTING_WM_CHECK", False);

    s->wmcheckwin = XCreateSimpleWindow(s->dpy, s->root, 0, 0, 1, 1, 0, 0, 0);
    XChangeProperty(s->dpy, s->wmcheckwin, s->net_wm_check, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&s->wmcheckwin, 1);
    XChangeProperty(s->dpy, s->wmcheckwin, s->net_wm_name, s->utf8_string, 8,
                    PropModeReplace, (unsigned char *)"rewm", 4);
    XChangeProperty(s->dpy, s->root, s->net_wm_check, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&s->wmcheckwin, 1);

    Atom supported[] = {
        s->net_supported, s->net_wm_name, s->net_wm_state, s->net_wm_fullscreen,
        s->net_active_window, s->net_client_list, s->net_wm_window_type,
        s->net_wm_window_type_dialog, s->net_wm_check,
    };
    XChangeProperty(s->dpy, s->root, s->net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)supported, (int)LENGTH(supported));
    XDeleteProperty(s->dpy, s->root, s->net_client_list);
}

/* =========================================================================
 *  Entry point — called from host.c after JIT compile
 * ========================================================================= */
int wm_entry(WMState *s) {
    exitcode = REWM_QUIT;

    /* ---- atoms (cheap; re-intern every reload, ids are stable) --------- */
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

    updatenumlockmask(s);

    if (!s->initialized) {
        s->sw = DisplayWidth(s->dpy, s->screen);
        s->sh = DisplayHeight(s->dpy, s->screen);

        s->gc = XCreateGC(s->dpy, s->root, 0, NULL);
        setupfont(s);
        setupcolors(s);
        setupcursors(s);
        setupewmh(s);
        strncpy(s->statustext, "rewm", sizeof(s->statustext) - 1);

        for (Monitor *m = s->mons; m; m = m->next) {
            if (m->mfact <= 0.0f) m->mfact = DEFAULT_MFACT;
            if (m->nmaster <= 0) m->nmaster = DEFAULT_NMASTER;
            m->showbar = 1;
            m->topbar = 1;
            strncpy(m->ltsymbol, layouts[0].symbol, sizeof(m->ltsymbol) - 1);
            updatebarpos(s, m);
            m->barwin = XCreateSimpleWindow(s->dpy, s->root, m->mx, m->topbar ? m->my : m->my + m->wh,
                                            (unsigned)m->mw, (unsigned)s->barheight, 0,
                                            s->col[SchemeNorm][ColBorder], s->col[SchemeNorm][ColBg]);
            XSetWindowBackground(s->dpy, m->barwin, s->col[SchemeNorm][ColBg]);
            XDefineCursor(s->dpy, m->barwin, s->cur_normal);
            XSelectInput(s->dpy, m->barwin, ExposureMask | ButtonPressMask);
            XMapRaised(s->dpy, m->barwin);
        }

        scanwindows(s);
        s->initialized = 1;
    }

    grabkeys(s);
    XUngrabButton(s->dpy, AnyButton, AnyModifier, s->root);

    focus(s, s->sel);
    arrange(s, NULL);

    /* ---- event loop --------------------------------------------------- */
    XEvent ev;
    s->running = 1;
    XSync(s->dpy, False);
    while (s->running) {
        if (XPending(s->dpy) == 0) {
            usleep(5000);
            continue;
        }
        XNextEvent(s->dpy, &ev);
        switch (ev.type) {
        case ButtonPress:      buttonpress(s, &ev);       break;
        case KeyPress:          keypress(s, &ev);          break;
        case EnterNotify:       enternotify(s, &ev);       break;
        case Expose:             expose(s, &ev);            break;
        case DestroyNotify:     destroynotify(s, &ev);     break;
        case UnmapNotify:        unmapnotify(s, &ev);       break;
        case MapRequest:         maprequest(s, &ev);        break;
        case ConfigureRequest:  configurerequest(s, &ev);  break;
        case ConfigureNotify:    configurenotify(s, &ev);   break;
        case PropertyNotify:     propertynotify(s, &ev);    break;
        case ClientMessage:      clientmessage(s, &ev);     break;
        case MappingNotify:      mappingnotify(s, &ev);     break;
        }
    }

    return exitcode;
}
