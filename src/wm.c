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
 * dwm-alike: tags, master/stack tiling, monocle, floating, an
 * Xft-rendered text bar, mouse move/resize (sxwm-style drag),
 * fullscreen, ICCCM/EWMH basics, Xinerama multi-monitor.
 *
 * IMPORTANT: because JIT-compiled code resolves symbols via
 * dlsym(RTLD_DEFAULT) against the *host* process, host.c's own link
 * line must include -lXft -lfontconfig -lXrender -lXinerama, not just
 * have those libs present on the system -- dlsym(RTLD_DEFAULT) only
 * sees symbols already loaded into this process.
 */

#define _DEFAULT_SOURCE
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <sys/wait.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/Xproto.h>
#include <X11/XF86keysym.h>
#include <fontconfig/fontconfig.h>

/* =========================================================================
 *  Config
 * ========================================================================= */
#define MODKEY            Mod4Mask
#define TERMINAL          "st"
#define LAUNCHER          "tmenu_runner"
#define TAGMASK           ((1 << 9) - 1)
static const char *fontnames[] = {
    "SeriousShanns Nerd Font:size=11",
};

#define BARHEIGHT_PAD     4      /* extra px added to font height for bar */
#define COL_NORM_BG       "#1D1C19"
#define COL_NORM_FG       "#E2D5C0"
#define COL_NORM_BORDER   "#463F34"
#define COL_SEL_BG        "#CCB998"
#define COL_SEL_FG        "#1D1C19"
#define COL_SEL_BORDER    "#CCB998"
#define DEFAULT_MFACT     0.55f
#define DEFAULT_NMASTER   1
#define DEFAULT_GAP       4
#define DEFAULT_RESIZEHINTS  1
#define DEFAULT_LOCKFULLSCREEN 1
#define DEFAULT_REFRESHRATE 0
#define TITLE_MAX_CHARS    128    /* max UTF-8 codepoints of client title in bar */

static const char *tagnames[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

typedef struct {
    const char *class;
    const char *instance;
    const char *title;
    unsigned int tags;
    int isfloating;
} Rule;

/* class/instance/title match -> tags/floating, like dwm's config.h rules[].
 * NULL fields match anything. tags==0 keeps whatever tag was active when
 * the window was created. Edit freely -- this is your config section. */
static const Rule rules[] = {
    /* class          instance  title     tags mask   isfloating */
    { "mpv",          NULL,     NULL,     0,           1 },
    { "dialog",       NULL,     NULL,     0,           1 },
};

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
static void    drawbars(WMState *s);
static void    updatetitle(WMState *s, Client *c);
static void    configure(WMState *s, Client *c);
static void    resizeclient(WMState *s, Client *c, int x, int y, int w, int h);
static void    setfullscreen(WMState *s, Client *c, int fullscreen);
static void    setsticky(WMState *s, Client *c, int sticky);
static Client  *nexttiled(Client *c);
static Client  *nexttagged(Client *c);
static void    attach(WMState *s, Client *c);
static void    attachaside(WMState *s, Client *c);
static void    attachstack(WMState *s, Client *c);
static void    detach(WMState *s, Client *c);
static void    detachstack(WMState *s, Client *c);
static void    syncglobalclients(WMState *s);
static void    updatesizehints(WMState *s, Client *c);
static void    updatewmhints(WMState *s, Client *c);
static void    updatewindowtype(WMState *s, Client *c);
static int     sendevent(WMState *s, Window w, Atom proto, long d0, long d1, long d2, long d3, long d4);
static void    unmanage(WMState *s, Client *c, int destroyed);
static void    expose(WMState *s, XEvent *e);
static void    focusin(WMState *s, XEvent *e);
static void    motionnotify(WMState *s, XEvent *e);
static void    resizerequest(WMState *s, XEvent *e);
static void    cleanup(WMState *s);
static Monitor *recttomon(WMState *s, int x, int y, int w, int h);
static long    getstate(WMState *s, Window w);
static void    updatenumlockmask(WMState *s);
static int     updategeom(WMState *s);
static Monitor *createmon(WMState *s);
static void    cleanupmon(WMState *s, Monitor *m);
static Monitor *dirtomon(WMState *s, int dir);
static void    focusmon(WMState *s, const Arg *arg);
static void    tagmon(WMState *s, const Arg *arg);
static void    updatebarpos(WMState *s, Monitor *m);
static void    resizebarwin(WMState *s, Monitor *m);

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
static void togglesticky(WMState *s, const Arg *arg);
static void setgaps(WMState *s, const Arg *arg);
static void setgapsabs(WMState *s, const Arg *arg);
static void setborderpx(WMState *s, const Arg *arg);
static void setborderpxabs(WMState *s, const Arg *arg);
static void movestack(WMState *s, const Arg *arg);
static void setlayout(WMState *s, const Arg *arg);
static void movemouse(WMState *s, const Arg *arg);
static void resizemouse(WMState *s, const Arg *arg);
static void quit(WMState *s, const Arg *arg);
static void reload_wm(WMState *s, const Arg *arg);
static void focusmon(WMState *s, const Arg *arg);
static void tagmon(WMState *s, const Arg *arg);

/* =========================================================================
 *  Layouts
 * ========================================================================= */
static void tile(WMState *s, Monitor *m);
static void monocle(WMState *s, Monitor *m);
static void floating(WMState *s, Monitor *m);

static Layout layouts[] = {
    { "<T>",  tile },
    { "<M>",  monocle },
    { "<F>",  floating },
};
#define LAYOUT_COUNT ((int)(sizeof(layouts)/sizeof(layouts[0])))

/* =========================================================================
 *  Keybindings (dwm/sxwm style: MODKEY + key)
 * ========================================================================= */
#define TAGKEYS(KEY,TAG)                                                \
    { MODKEY,               KEY, view,       {.ui = 1 << (TAG)} },      \
    { MODKEY|ControlMask,    KEY, toggleview, {.ui = 1 << (TAG)} },     \
    { MODKEY|ShiftMask,      KEY, tagclient,  {.ui = 1 << (TAG)} },     \
    { MODKEY|ControlMask|ShiftMask, KEY, toggletag, {.ui = 1 << (TAG)} }

static const char *termcmd[]  = { TERMINAL,                                       NULL };
static const char *dmenucmd[] = { TERMINAL, "-c", "dialog", "-e", LAUNCHER,       NULL };
static const char *zoomcmd[]  = { "boomer",                                       NULL };
static const char *editcmd[]  = { "emacs",                                        NULL };
static const char *webcmd[]   = { "firefox-bin",                                  NULL };
static const char *emojicmd[] = { TERMINAL, "-c", "dialog", "-e", "dmenumoji"   , NULL };

/* media keys */
static const char *up_vol[]       = { "pamixer-wrapper",       "raise",                         NULL };
static const char *down_vol[]     = { "pamixer-wrapper",       "lower",                         NULL };
static const char *mute_vol[]     = { "pamixer-wrapper",       "toggle",                        NULL };
static const char *mute_mic_vol[] = { "pactl", "set-source-mute", "@DEFAULT_SOURCE@", "toggle", NULL };

/* brightness keys */
static const char *brighter[]     = { "brightnessctl-wrapper", "raise",      NULL };
static const char *dimmer[]       = { "brightnessctl-wrapper", "lower",      NULL };

/* screenshot keys */
static const char *screenshot[]       = { "/bin/sh", "-c", "xsc.sh", NULL };
static const char *screenshot_sel[]   = { "/bin/sh", "-c", "xscsel.sh", NULL };

static Key keys[] = {
    { MODKEY,                XK_Return, spawn,          {.v = termcmd } },
    { MODKEY,                XK_p,      spawn,          {.v = dmenucmd} },
    { MODKEY,                XK_e,      spawn,          {.v = editcmd } },
    { MODKEY,                XK_w,      spawn,          {.v = webcmd  } },
    { MODKEY|ShiftMask,      XK_z,      spawn,          {.v = zoomcmd } },
    { MODKEY,                XK_period, spawn,          {.v = emojicmd} },
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
    { MODKEY|ShiftMask,      XK_space,  togglefloating, {0} },
    { MODKEY,                XK_t,      setlayout,      {.i = 0} },
    { MODKEY,                XK_m,      setlayout,      {.i = 1} },
    { MODKEY,                XK_f,      setlayout,      {.i = 2} },
    { MODKEY,                XK_z,      togglefloating, {0} },
    { MODKEY,                XK_u,      togglefullscreen,{0} },
    { MODKEY|ShiftMask,      XK_f,      togglefullscreen,{0} },
    { MODKEY,                XK_s,      togglesticky,   {0} },
    { MODKEY|ShiftMask,      XK_j,      movestack,      {.i = +1} },
    { MODKEY|ShiftMask,      XK_k,      movestack,      {.i = -1} },
    { MODKEY,                XK_minus,  setgaps,        {.i = -1} },
    { MODKEY,                XK_equal,  setgaps,        {.i = +1} },
    { MODKEY|ShiftMask,      XK_equal,  setgapsabs,     {.i = DEFAULT_GAP} },
    { MODKEY,                XK_bracketleft,  setborderpx,   {.i = -1} },
    { MODKEY,                XK_bracketright, setborderpx,   {.i = +1} },
    { MODKEY|ShiftMask,      XK_bracketright, setborderpxabs,{.i = 0} },
    { MODKEY,                XK_comma,  focusmon,       {.i = -1} },
    { MODKEY,                XK_period, focusmon,       {.i = +1} },
    { MODKEY|ShiftMask,      XK_comma,  tagmon,         {.i = -1} },
    { MODKEY|ShiftMask,      XK_period, tagmon,         {.i = +1} },
    { MODKEY,                XK_0,      view,           {.ui = ~0} },
    { MODKEY|ShiftMask,      XK_0,      tagclient,      {.ui = ~0} },
    /* media keys */
    { 0, XF86XK_AudioMicMute,      spawn, {.v = mute_mic_vol} },
    { 0, XF86XK_AudioMute,         spawn, {.v = mute_vol} },
    { 0, XF86XK_AudioLowerVolume,  spawn, {.v = down_vol} },
    { 0, XF86XK_AudioRaiseVolume,  spawn, {.v = up_vol} },
    { 0, XF86XK_MonBrightnessDown, spawn, {.v = dimmer} },
    { 0, XF86XK_MonBrightnessUp,   spawn, {.v = brighter} },
    /* screenshot keys */
    { 0,         XK_Print, spawn, {.v = screenshot} },
    { ShiftMask, XK_Print, spawn, {.v = screenshot_sel} },
    TAGKEYS(XK_1, 0), TAGKEYS(XK_2, 1), TAGKEYS(XK_3, 2),
    TAGKEYS(XK_4, 3), TAGKEYS(XK_5, 4), TAGKEYS(XK_6, 5),
    TAGKEYS(XK_7, 6), TAGKEYS(XK_8, 7), TAGKEYS(XK_9, 8),
};
#define KEY_COUNT ((int)(sizeof(keys)/sizeof(keys[0])))

/* =========================================================================
 *  Colors / font (Xft-backed)
 * ========================================================================= */
static unsigned long getcolor(WMState *s, const char *name) {
    XColor c;
    Colormap cmap = DefaultColormap(s->dpy, s->screen);
    if (!XAllocNamedColor(s->dpy, cmap, name, &c, &c))
	return BlackPixel(s->dpy, s->screen);
    return c.pixel;
}

static void freexftcolors(WMState *s) {
    /* only meaningful once visual/cmap are known, i.e. after the very
     * first setupfont() -- guard so the pre-init call doesn't touch
     * garbage */
    if (!s->visual) return;
    for (int i = 0; i < SchemeLast; i++)
	for (int j = 0; j < ColLast; j++)
	    if (s->xftcol[i][j].pixel || s->xftcol[i][j].color.alpha)
		XftColorFree(s->dpy, s->visual, s->cmap, &s->xftcol[i][j]);
}

static void allocxftcolor(WMState *s, int scheme, int col, const char *name) {
    if (!XftColorAllocName(s->dpy, s->visual, s->cmap, name, &s->xftcol[scheme][col]))
	fprintf(stderr, "rewm: cannot allocate Xft color '%s'\n", name);
}

static void setupcolors(WMState *s) {
    /* plain pixel values, used by drawrect() (tag dots, borders) */
    s->col[SchemeNorm][ColFg]     = getcolor(s, COL_NORM_FG);
    s->col[SchemeNorm][ColBg]     = getcolor(s, COL_NORM_BG);
    s->col[SchemeNorm][ColBorder] = getcolor(s, COL_NORM_BORDER);
    s->col[SchemeSel][ColFg]      = getcolor(s, COL_SEL_FG);
    s->col[SchemeSel][ColBg]      = getcolor(s, COL_SEL_BG);
    s->col[SchemeSel][ColBorder]  = getcolor(s, COL_SEL_BORDER);

    /* Xft colors, used by drawtext(). setupcolors() is called on
     * every reload (not just first init), so free the previous
     * allocation first -- XftColorAllocName holds Render-side
     * resources that plain XAllocNamedColor refcounting doesn't. */
    freexftcolors(s);
    s->visual = DefaultVisual(s->dpy, s->screen);
    s->cmap   = DefaultColormap(s->dpy, s->screen);
    s->depth  = (unsigned int)DefaultDepth(s->dpy, s->screen);
    allocxftcolor(s, SchemeNorm, ColFg,     COL_NORM_FG);
    allocxftcolor(s, SchemeNorm, ColBg,     COL_NORM_BG);
    allocxftcolor(s, SchemeNorm, ColBorder, COL_NORM_BORDER);
    allocxftcolor(s, SchemeSel,  ColFg,     COL_SEL_FG);
    allocxftcolor(s, SchemeSel,  ColBg,     COL_SEL_BG);
    allocxftcolor(s, SchemeSel,  ColBorder, COL_SEL_BORDER);
}

/* This function is an implementation detail -- callers should use
 * setupfont() (initial load) or getfontforchar() (dynamic fallback).
 * Ported from dwm's drw.c xfont_create(). */
static Fnt *xfont_create(WMState *s, const char *fontname, FcPattern *fontpattern) {
    XftFont *xfont = NULL;
    FcPattern *pattern = NULL;

    if (fontname) {
	/* Using the pattern found at xfont->pattern does not yield the
	 * same substitution results as using the pattern returned by
	 * FcNameParse; using the latter results in the desired fallback
	 * behaviour whereas the former just results in missing-character
	 * rectangles being drawn, at least with some fonts. */
	if (!(xfont = XftFontOpenName(s->dpy, s->screen, fontname))) {
	    fprintf(stderr, "rewm: cannot load font from name: '%s'\n", fontname);
	    return NULL;
	}
	if (!(pattern = FcNameParse((FcChar8 *)fontname))) {
	    fprintf(stderr, "rewm: cannot parse font name to pattern: '%s'\n", fontname);
	    XftFontClose(s->dpy, xfont);
	    return NULL;
	}
    } else if (fontpattern) {
	if (!(xfont = XftFontOpenPattern(s->dpy, fontpattern))) {
	    fprintf(stderr, "rewm: cannot load font from pattern\n");
	    return NULL;
	}
	/* pattern stays NULL here -- this Fnt is a dynamically-found
	 * fallback leaf, not something we'd ever seed another lookup
	 * from (see the "first font must be loaded from a string"
	 * assumption in getfontforchar()). */
    } else {
	fprintf(stderr, "rewm: xfont_create: no font specified\n");
	return NULL;
    }

    Fnt *font = calloc(1, sizeof(Fnt));
    font->xfont = xfont;
    font->pattern = pattern;
    return font;
}

static void xfont_free(WMState *s, Fnt *font) {
    if (!font) return;
    if (font->pattern) FcPatternDestroy(font->pattern);
    XftFontClose(s->dpy, font->xfont);
    free(font);
}

static void freefonts(WMState *s) {
    while (s->fonts) {
	Fnt *f = s->fonts;
	s->fonts = f->next;
	xfont_free(s, f);
    }
}

/* fonts[i] in fontnames[] are tried first, in order; anything not
 * covered by them falls through to a dynamic per-codepoint
 * XftFontMatch() lookup (see getfontforchar()) instead of just
 * drawing tofu -- so one primary font here is usually enough. */
static void setupfont(WMState *s) {
    /* Close the old chain before loading a new one (for hot-reload) */
    freefonts(s);

    for (int i = (int)LENGTH(fontnames) - 1; i >= 0; i--) {
	Fnt *f = xfont_create(s, fontnames[i], NULL);
	if (!f) continue;
	f->next = s->fonts;
	s->fonts = f;
    }

    if (!s->fonts) {
	/* everything configured failed to load -- last resort */
	Fnt *f = xfont_create(s, "fixed", NULL);
	if (f) s->fonts = f;
    }

    if (s->fonts) {
	s->fonth = s->fonts->xfont->ascent + s->fonts->xfont->descent;
    } else {
	fprintf(stderr, "rewm: no fonts loaded at all, bar will be broken\n");
	s->fonth = 12;
    }
    s->lrpad = s->fonth;
    s->barheight = s->fonth + BARHEIGHT_PAD;
}

/* find a font covering `rune`: first check the already-loaded chain,
 * then fall back to a dynamic fontconfig lookup seeded from the
 * primary font's pattern, caching whatever's found onto the tail of
 * the chain so the expensive XftFontMatch call only happens once per
 * codepoint. A small negative-result cache (`nomatches`) avoids
 * re-querying fontconfig for codepoints we already know nothing
 * covers. Ported from dwm's drw_text() fallback path.
 *
 * NOTE: `nomatches` is a plain `static` (not on WMState), so it
 * resets on every hot reload -- that's fine, it's purely a perf
 * cache, not correctness-affecting. */
static Fnt *getfontforchar(WMState *s, FcChar32 rune) {
    static unsigned int nomatches[128];
    unsigned int hash, h0, h1;

    for (Fnt *f = s->fonts; f; f = f->next)
	if (XftCharExists(s->dpy, f->xfont, rune))
	    return f;

    /* none of the loaded fonts have it -- ask fontconfig, unless we
     * already know this codepoint has no match anywhere */
    hash = rune;
    hash = ((hash >> 16) ^ hash) * 0x21F0AAAD;
    hash = ((hash >> 15) ^ hash) * 0xD35A2D97;
    h0 = ((hash >> 15) ^ hash) % LENGTH(nomatches);
    h1 = (hash >> 17) % LENGTH(nomatches);
    if (nomatches[h0] == rune || nomatches[h1] == rune)
	return s->fonts;

    if (!s->fonts || !s->fonts->pattern)
	return s->fonts; /* nothing sane to seed the lookup from */

    FcCharSet *fccharset = FcCharSetCreate();
    FcCharSetAddChar(fccharset, rune);

    FcPattern *fcpattern = FcPatternDuplicate(s->fonts->pattern);
    FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
    FcPatternAddBool(fcpattern, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(NULL, fcpattern, FcMatchPattern);
    FcDefaultSubstitute(fcpattern);

    FcResult result;
    FcPattern *match = XftFontMatch(s->dpy, s->screen, fcpattern, &result);

    FcCharSetDestroy(fccharset);
    FcPatternDestroy(fcpattern);

    if (match) {
	Fnt *nf = xfont_create(s, NULL, match);
	if (nf && XftCharExists(s->dpy, nf->xfont, rune)) {
	    Fnt *tail = s->fonts;
	    while (tail->next) tail = tail->next;
	    tail->next = nf;
	    return nf;
	}
	if (nf) xfont_free(s, nf);
	nomatches[nomatches[h0] ? h1 : h0] = rune;
    }
    return s->fonts;
}

/* Shared off-screen scratch pixmap that every drawbar() call draws
 * into before XCopyArea-ing the result onto that monitor's barwin --
 * same trick your drw.c uses (drw->drawable / drw_resize), just sized
 * once to the full display width so no per-monitor resize is needed. */
static void setupdrawable(WMState *s) {
    unsigned int w = (unsigned int)MAX(s->sw, 1);
    unsigned int h = (unsigned int)MAX(s->barheight, 1);

    /* Clean up old drawable if it exists (for hot-reload) */
    if (s->xftdraw) {
	XftDrawDestroy(s->xftdraw);
	s->xftdraw = NULL;
    }
    if (s->drawable) {
	XFreePixmap(s->dpy, s->drawable);
	s->drawable = 0;
    }

    s->drawable = XCreatePixmap(s->dpy, s->root, w, h, s->depth);
    s->draww = w;
    s->drawh = h;
    s->xftdraw = XftDrawCreate(s->dpy, s->drawable, s->visual, s->cmap);
}

static void resizedrawable(WMState *s, unsigned int w, unsigned int h) {
    if (w <= s->draww && h <= s->drawh) return;
    if (w < s->draww) w = s->draww;
    if (h < s->drawh) h = s->drawh;
    if (s->xftdraw) XftDrawDestroy(s->xftdraw);
    if (s->drawable) XFreePixmap(s->dpy, s->drawable);
    s->drawable = XCreatePixmap(s->dpy, s->root, w, h, s->depth);
    s->draww = w;
    s->drawh = h;
    s->xftdraw = XftDrawCreate(s->dpy, s->drawable, s->visual, s->cmap);
}

static void setupcursors(WMState *s) {
    s->cur_normal = XCreateFontCursor(s->dpy, XC_left_ptr);
    s->cur_move   = XCreateFontCursor(s->dpy, XC_fleur);
    s->cur_resize = XCreateFontCursor(s->dpy, XC_sizing);
    XDefineCursor(s->dpy, s->root, s->cur_normal);
}

static int textwidth(WMState *s, const char *text) {
    if (!text || !*text) return s->lrpad;
    if (!s->fonts) return (int)strlen(text) * 6 + s->lrpad;

    int w = 0;
    const char *p = text;
    while (*p) {
	FcChar32 rune;
	int clen = FcUtf8ToUcs4((FcChar8 *)p, &rune, (int)strlen(p));
	if (clen <= 0) clen = 1;
	Fnt *f = getfontforchar(s, rune);
	XGlyphInfo ext;
	XftTextExtentsUtf8(s->dpy, f->xfont, (const FcChar8 *)p, clen, &ext);
	w += ext.xOff;
	p += clen;
    }
    return w + s->lrpad;
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

static void drawrect(WMState *s, int x, int y, int w, int h, unsigned long pixel, int fill) {
    XSetForeground(s->dpy, s->gc, pixel);
    if (fill) {
	XFillRectangle(s->dpy, s->drawable, s->gc, x, y, (unsigned)w, (unsigned)h);
    } else {
	/* Draw 4 sides with XFillRectangle instead of XDrawRectangle(…, w-1, h-1).
	 * XDrawRectangle's corner rendering drops the bottom-right pixel on
	 * some drivers/servers; 4 side fills are unambiguous and cost is
	 * negligible (bar redraws a few times/sec). */
	XFillRectangle(s->dpy, s->drawable, s->gc, x, y, (unsigned)w, 1);
	XFillRectangle(s->dpy, s->drawable, s->gc, x, y + h - 1, (unsigned)w, 1);
	XFillRectangle(s->dpy, s->drawable, s->gc, x, y, 1, (unsigned)h);
	XFillRectangle(s->dpy, s->drawable, s->gc, x + w - 1, y, 1, (unsigned)h);
    }
}

/* UTF-8 helpers for title truncation */
static int utf8charlen(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (!c) return 0;
    if (c < 0x80) return 1;
    if (c < 0xC0) return 1;
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    return 4;
}

static int utf8strlen(const char *s) {
    int n = 0;
    while (*s) { s += utf8charlen(s); n++; }
    return n;
}

static void truncate_utf8(const char *src, char *dst, size_t dstsize, int max_cp) {
    int cp = 0;
    size_t pos = 0;
    while (src[pos] && cp < max_cp) {
	int clen = utf8charlen(&src[pos]);
	if (pos + (size_t)clen >= dstsize) break;
	for (int i = 0; i < clen; i++) dst[pos + i] = src[pos + i];
	pos += (size_t)clen;
	cp++;
    }
    if (src[pos]) {
	const char *e = "\xe2\x80\xa6"; /* … (U+2026, UTF-8) */
	if (pos + 3 < dstsize) { dst[pos++] = e[0]; dst[pos++] = e[1]; dst[pos++] = e[2]; }
    }
    if (pos < dstsize) dst[pos] = '\0';
    else dst[dstsize - 1] = '\0';
}

static void drawtext(WMState *s, int x, int y, unsigned long fg, unsigned long bg,
		     XftColor *xfg, int w, int h, const char *text) {
    (void)y;
    (void)fg;
    XSetForeground(s->dpy, s->gc, bg);
    XFillRectangle(s->dpy, s->drawable, s->gc, x, 0, (unsigned)w, (unsigned)h);
    if (!text || !*text || !s->fonts) return;

    /* per-glyph fallback: draw one codepoint at a time, picking
     * whichever font in the chain actually has that glyph. No run
     * batching -- simpler, and the bar redraws are infrequent/cheap
     * enough that per-glyph XftDrawStringUtf8 calls don't matter. */
    int cx = x + s->lrpad / 2;
    const char *p = text;
    while (*p) {
	FcChar32 rune;
	int clen = FcUtf8ToUcs4((FcChar8 *)p, &rune, (int)strlen(p));
	if (clen <= 0) clen = 1;
	Fnt *f = getfontforchar(s, rune);
	int ty = (h - (f->xfont->ascent + f->xfont->descent)) / 2 + f->xfont->ascent;
	XftDrawStringUtf8(s->xftdraw, xfg, f->xfont, cx, ty, (const FcChar8 *)p, clen);
	XGlyphInfo ext;
	XftTextExtentsUtf8(s->dpy, f->xfont, (const FcChar8 *)p, clen, &ext);
	cx += ext.xOff;
	p += clen;
    }
}

static void drawbar(WMState *s, Monitor *m) {
    if (!m || !m->showbar || m->barwin == 0) return;
    resizedrawable(s, (unsigned)m->ww, (unsigned)s->barheight);

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
	int scheme = sel ? SchemeSel : SchemeNorm;
	drawtext(s, x, 0, s->col[scheme][ColFg], s->col[scheme][ColBg],
                 &s->xftcol[scheme][ColFg], w, s->barheight, tagnames[i]);
	if (occ & (1 << i))
	    drawrect(s, x + 2, 2, 4, 4,
                     (urg & (1 << i)) ? s->col[SchemeSel][ColBorder] : s->col[scheme][ColFg], 1);
	x += w;
    }

    /* layout symbol */
    int lw = textwidth(s, m->ltsymbol);
    drawtext(s, x, 0, s->col[SchemeNorm][ColFg], s->col[SchemeNorm][ColBg],
             &s->xftcol[SchemeNorm][ColFg], lw, s->barheight, m->ltsymbol);
    x += lw;

    /* status text on far right */
    int sw = textwidth(s, s->statustext);
    int mid = m->ww - sw;
    if (mid < x) mid = x;
    drawtext(s, mid, 0, s->col[SchemeNorm][ColFg], s->col[SchemeNorm][ColBg],
             &s->xftcol[SchemeNorm][ColFg], m->ww - mid, s->barheight, s->statustext);

    /* selected window title fills the middle */
    int tw = mid - x;
    if (tw > 0) {
	Client *sel = s->sel;
	const char *raw = (sel && sel->mon == m) ? sel->name : "";
	char truncated[256];
	truncate_utf8(raw, truncated, sizeof(truncated), TITLE_MAX_CHARS);
	drawtext(s, x, 0, s->col[SchemeNorm][ColFg], s->col[SchemeNorm][ColBg],
                 &s->xftcol[SchemeNorm][ColFg], tw, s->barheight, truncated);
	if (sel && sel->mon == m && sel->isfloating) {
            drawrect(s, x + 1, 2, 4, 4, s->col[SchemeNorm][ColFg], 1);
            // x += 6;
            tw = mid - x;
	}
    }

    XCopyArea(s->dpy, s->drawable, m->barwin, s->gc, 0, 0,
              (unsigned)m->ww, (unsigned)s->barheight, 0, 0);
    XSync(s->dpy, False);
}

static void drawbars(WMState *s) {
    for (Monitor *m = s->mons; m; m = m->next)
	drawbar(s, m);
}

static void updatestatus(WMState *s) {
    char **list = NULL;
    int n;
    XTextProperty tp;

    s->statustext[0] = '\0';
    if (XGetTextProperty(s->dpy, s->root, &tp, XA_WM_NAME) && tp.nitems > 0) {
	if (tp.encoding == XA_STRING) {
	    strncpy(s->statustext, (char *)tp.value, sizeof(s->statustext) - 1);
	} else if (XmbTextPropertyToTextList(s->dpy, &tp, &list, &n) >= Success
                   && n > 0 && list && *list) {
	    strncpy(s->statustext, *list, sizeof(s->statustext) - 1);
	    XFreeStringList(list);
	}
	s->statustext[sizeof(s->statustext) - 1] = '\0';
	XFree(tp.value);
    }
    if (!s->statustext[0]) {
	strncpy(s->statustext, "rewm", sizeof(s->statustext) - 1);
    }
}

/* =========================================================================
 *  Client management
 * ========================================================================= */
static int applysizehints(WMState *s, Client *c, int *x, int *y, int *w, int *h, int interact) {
    int baseismin;
    Monitor *m = c->mon;

    if (!c->hintsvalid) {
	updatesizehints(s, c);
    }

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
    if (*h < s->barheight) *h = s->barheight;
    if (*w < s->barheight) *w = s->barheight;

    if (s->resizehints || c->isfloating || layouts[m->sellt].arrange == floating) {
	if (!c->basew && !c->baseh) {
	    c->basew = c->baseh = 0;
	}
	baseismin = (c->basew == c->minw && c->baseh == c->minh);
	if (!baseismin) {
	    if (c->minw && *w < c->minw) *w = c->minw;
	    if (c->minh && *h < c->minh) *h = c->minh;
	}
	if (c->maxw && *w > c->maxw) *w = c->maxw;
	if (c->maxh && *h > c->maxh) *h = c->maxh;

	if (c->basew && c->incw) {
	    *w -= (*w - c->basew) % c->incw;
	}
	if (c->baseh && c->inch) {
	    *h -= (*h - c->baseh) % c->inch;
	}

	if (c->maxa > 0.0f && c->mina > 0.0f) {
	    if ((float)*w / *h > c->maxa)
		*h = (int)((float)*w / c->maxa + 0.5f);
	    else if ((float)*w / *h < c->mina)
		*w = (int)((float)*h * c->mina + 0.5f);
	}
    }

    return (c->oldx != *x || c->oldy != *y || c->oldw != *w || c->oldh != *h);
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
    XWindowChanges wc;
    c->oldx = c->x; c->x = wc.x = x;
    c->oldy = c->y; c->y = wc.y = y;
    c->oldw = c->w; c->w = wc.width = w;
    c->oldh = c->h; c->h = wc.height = h;
    wc.border_width = c->bw;
    XConfigureWindow(s->dpy, c->win, CWX|CWY|CWWidth|CWHeight|CWBorderWidth, &wc);
    configure(s, c);
    XSync(s->dpy, False);
}

static void resize(WMState *s, Client *c, int x, int y, int w, int h, int interact) {
    applysizehints(s, c, &x, &y, &w, &h, interact);
    resizeclient(s, c, x, y, w, h);
}

static int isvisible(Monitor *m, Client *c) {
    return c->issticky || (c->tags & m->tagset[m->seltags]) != 0;
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

static int sendevent(WMState *s, Window w, Atom proto, long d0, long d1, long d2, long d3, long d4) {
    int n;
    Atom *protocols;
    Atom mt;
    int exists = 0;
    XEvent ev;

    if (proto != s->wm_protocols) {
	mt = s->wm_protocols;
	if (XGetWMProtocols(s->dpy, w, &protocols, &n)) {
	    while (n-- > 0)
		if (protocols[n] == proto) { exists = 1; break; }
	    XFree(protocols);
	}
    } else {
	mt = s->wm_protocols;
	exists = 1;
    }
    if (exists) {
	ev.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = mt;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = d0;
	ev.xclient.data.l[1] = d1;
	ev.xclient.data.l[2] = d2;
	ev.xclient.data.l[3] = d3;
	ev.xclient.data.l[4] = d4;
	XSendEvent(s->dpy, w, False, NoEventMask, &ev);
    }
    return exists;
}

static void setfocus(WMState *s, Client *c) {
    if (!c->neverfocus) {
	XSetInputFocus(s->dpy, c->win, RevertToPointerRoot, CurrentTime);
	XChangeProperty(s->dpy, s->root, s->net_active_window, XA_WINDOW, 32,
			PropModeReplace, (unsigned char *)&c->win, 1);
    }
    sendevent(s, c->win, s->wm_protocols, s->wm_take_focus, CurrentTime, 0, 0, 0);
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
    updatenumlockmask(s);
    XUngrabKey(s->dpy, AnyKey, AnyModifier, s->root);

    int mincode, maxcode;
    XDisplayKeycodes(s->dpy, &mincode, &maxcode);
    int keysyms_per_keycode;
    KeySym *keysyms = XGetKeyboardMapping(s->dpy, mincode, maxcode - mincode + 1, &keysyms_per_keycode);
    if (!keysyms) return;

    for (int kc = mincode; kc <= maxcode; kc++) {
	KeySym keysym = keysyms[(kc - mincode) * keysyms_per_keycode];
	for (int i = 0; i < KEY_COUNT; i++) {
	    if (keysym == keys[i].keysym) {
		unsigned int mods[] = { 0, LockMask, numlockmask, numlockmask | LockMask };
		for (int j = 0; j < (int)LENGTH(mods); j++)
		    XGrabKey(s->dpy, kc, keys[i].mod | mods[j], s->root, True,
			     GrabModeAsync, GrabModeAsync);
	    }
	}
    }
    XFree(keysyms);
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
    if (hints.flags & PAspect) {
	c->mina = (float)hints.min_aspect.y / hints.min_aspect.x;
	c->maxa = (float)hints.max_aspect.x / hints.max_aspect.y;
    } else {
	c->mina = c->maxa = 0.0f;
    }
    c->isfixed = (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh);
    c->hintsvalid = 1;
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
	if (t == s->net_wm_sticky)
	    setsticky(s, c, 1);
    }
}

static void attach(WMState *s, Client *c) {
    c->next = c->mon->clients;
    c->mon->clients = c;
    syncglobalclients(s);
}

static void attachaside(WMState *s, Client *c) {
    Client *at = nexttagged(c);
    if (!at) {
	attach(s, c);
	return;
    }
    c->next = at->next;
    at->next = c;
    syncglobalclients(s);
}

static Client *nexttagged(Client *c) {
    Client *walked;
    for (walked = c->mon->clients; walked && (walked->isfloating || !(walked->tags & c->tags)); walked = walked->next);
    return walked;
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
    XDeleteProperty(s->dpy, s->root, s->net_client_list);
    for (Monitor *m = s->mons; m; m = m->next)
	for (Client *c = m->clients; c; c = c->next)
	    XChangeProperty(s->dpy, s->root, s->net_client_list, XA_WINDOW, 32,
			    PropModeAppend, (unsigned char *)&c->win, 1);
}

static Client *nexttiled(Client *c) {
    for (; c && (c->isfloating || !isvisible(c->mon, c)); c = c->next);
    return c;
}

static Monitor *recttomon(WMState *s, int x, int y, int w, int h) {
    Monitor *m, *r = s->selmon;
    int a, area = 0;
    for (m = s->mons; m; m = m->next) {
	if ((a = INTERSECT(x, y, w, h, m)) > area) {
	    area = a;
	    r = m;
	}
    }
    return r;
}

static Monitor *wintomon(WMState *s, Window w) {
    int x, y;
    if (w == s->root && getrootptr(s, &x, &y))
	return recttomon(s, x, y, 1, 1);
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

static void applyrules(WMState *s, Client *c) {
    XClassHint ch = { NULL, NULL };
    XGetClassHint(s->dpy, c->win, &ch);
    const char *class    = ch.res_class ? ch.res_class : "broken";
    const char *instance = ch.res_name  ? ch.res_name  : "broken";

    for (int i = 0; i < (int)LENGTH(rules); i++) {
	const Rule *r = &rules[i];
	if ((!r->title || strstr(c->name, r->title))
	    && (!r->class || strstr(class, r->class))
	    && (!r->instance || strstr(instance, r->instance))) {
	    c->isfloating = r->isfloating;
	    c->tags |= r->tags;
	}
    }
    if (ch.res_class) XFree(ch.res_class);
    if (ch.res_name) XFree(ch.res_name);

    if (c->mon) {
	c->tags = (c->tags & TAGMASK) ? (c->tags & TAGMASK) : c->mon->tagset[c->mon->seltags];
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
    c->tags = 0;

    updatetitle(s, c);
    updatesizehints(s, c);
    updatewmhints(s, c);

    Window trans = None;
    Client *parent = NULL;
    if (XGetTransientForHint(s->dpy, w, &trans) && (parent = wintoclient(s, trans))) {
	c->isfloating = 1;
	c->mon = parent->mon;
	c->tags = parent->tags;
    } else {
	applyrules(s, c);
    }

    updatewindowtype(s, c);

    c->x = c->mon->mx + (c->mon->mw - (c->w + 2 * c->bw)) / 2;
    c->y = c->mon->my + (c->mon->mh - (c->h + 2 * c->bw)) / 2;

    XSetWindowBorderWidth(s->dpy, w, (unsigned)c->bw);
    XSetWindowBorder(s->dpy, w, s->col[SchemeNorm][ColBorder]);
    configure(s, c);
    XSelectInput(s->dpy, w, EnterWindowMask | FocusChangeMask | PropertyChangeMask | StructureNotifyMask);
    grabbuttons(s, c, 0);
    if (!c->isfloating) c->isfloating = c->isfixed;

    if (c->isfloating)
	XRaiseWindow(s->dpy, c->win);

    attachaside(s, c);
    attachstack(s, c);

    XChangeProperty(s->dpy, s->root, s->net_client_list, XA_WINDOW, 32,
		    PropModeAppend, (unsigned char *)&w, 1);
    XMoveResizeWindow(s->dpy, c->win, c->x + 2 * s->sw, c->y, (unsigned)c->w, (unsigned)c->h);
    setclientstate(s, c, NormalState);

    if (c->mon == s->selmon) {
	if (s->sel)
	    unfocus(s, s->sel, 0);
	s->sel = c;
    }
    arrange(s, c->mon);
    XMapWindow(s->dpy, w);
    focus(s, NULL);
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
	XSetWindowBorderWidth(s->dpy, c->win, (unsigned)c->oldbw);
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
    unsigned int clickregion = 0; /* 0=unset, 1=tagbar, 2=ltsymbol, 3=wintitle, 4=status */

    for (Monitor *m = s->mons; m; m = m->next) {
	if (ev->window == m->barwin) {
	    s->selmon = m;
	    int x = 0;
	    /* check tag bar region */
	    for (int i = 0; i < (int)LENGTH(tagnames); i++) {
		int w = textwidth(s, tagnames[i]);
		if (ev->x >= x && ev->x < x + w) {
		    Arg a;
		    a.ui = 1u << i;
		    unsigned int mod = CLEANMASK(ev->state);
		    if (mod == CLEANMASK(MODKEY)) {
			if (ev->button == Button1) tagclient(s, &a);
			else if (ev->button == Button3) toggletag(s, &a);
		    } else {
			if (ev->button == Button1) view(s, &a);
			else if (ev->button == Button3) toggleview(s, &a);
		    }
		    return;
		}
		x += w;
	    }
	    /* check layout symbol region */
	    int lw = textwidth(s, m->ltsymbol);
	    if (ev->x >= x && ev->x < x + lw) {
		clickregion = 2;
		if (ev->button == Button1) {
		    Arg a; a.i = -1; setlayout(s, &a); /* cycle */
		} else if (ev->button == Button3) {
		    Arg a; a.i = 1; setlayout(s, &a); /* monocle */
		}
		return;
	    }
	    x += lw;
	    /* check status text region (right side) */
	    int sw = textwidth(s, s->statustext);
	    if (ev->x >= m->ww - sw) {
		clickregion = 4;
		if (ev->button == Button2) {
		    Arg a; a.v = termcmd; spawn(s, &a);
		}
		return;
	    }
	    /* otherwise it's window title region */
	    clickregion = 3;
	    if (ev->button == Button2) {
		Arg a; a.i = 0; zoom(s, &a);
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
    } else if (cleanmod == CLEANMASK(MODKEY) && ev->button == Button2) {
	Arg a; a.i = 0;
	togglefloating(s, &a);
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

static int iswmwin(WMState *s, Window w) {
    if (w == s->wmcheckwin) return 1;
    for (Monitor *m = s->mons; m; m = m->next)
	if (w == m->barwin) return 1;
    return 0;
}

static void maprequest(WMState *s, XEvent *e) {
    XMapRequestEvent *ev = &e->xmaprequest;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(s->dpy, ev->window, &wa)) return;
    if (wa.override_redirect) return;
    if (iswmwin(s, ev->window)) return;
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
    int dirty = (s->sw != ev->width || s->sh != ev->height);
    s->sw = ev->width;
    s->sh = ev->height;
    if (updategeom(s) || dirty) {
	arrange(s, NULL);
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
	if ((Atom)cme->data.l[1] == s->net_wm_sticky || (Atom)cme->data.l[2] == s->net_wm_sticky)
	    setsticky(s, c, (cme->data.l[0] == 1 || (cme->data.l[0] == 2 && !c->issticky)));
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

static void focusin(WMState *s, XEvent *e) {
    XFocusChangeEvent *ev = &e->xfocus;
    if (s->sel && ev->window != s->sel->win)
	setfocus(s, s->sel);
}

static void motionnotify(WMState *s, XEvent *e) {
    XMotionEvent *ev = &e->xmotion;
    if (ev->window != s->root) return;

    if (ev->subwindow) {
	Client *c = wintoclient(s, ev->subwindow);
	if (c && c != s->sel)
	    focus(s, c);
	return;
    }

    static Monitor *mon = NULL;
    Monitor *m = recttomon(s, ev->x_root, ev->y_root, 1, 1);
    if (m && m != s->selmon) {
	if (mon)
	    unfocus(s, s->sel, 1);
	s->selmon = m;
	focus(s, NULL);
    }
    mon = m;
}

static void resizerequest(WMState *s, XEvent *e) {
    (void)s; (void)e;
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
    int gap = m->gappx;

    int mw = (n > nmaster) ? (nmaster ? (int)(m->ww * m->mfact) : 0) : (m->ww - gap);

    int i = 0, my = gap, ty = gap;
    for (Client *c = nexttiled(m->clients); c; c = nexttiled(c->next), i++) {
	if (i < nmaster) {
	    int h = (m->wh - my) / (MIN(n, nmaster) - i) - gap;
	    resize(s, c, m->wx + gap, m->wy + my, mw - 2 * c->bw - gap, h - 2 * c->bw, 0);
	    if (my + c->h + 2 * c->bw + gap < m->wh) my += c->h + 2 * c->bw + gap;
	} else {
	    int h = (m->wh - ty) / (n - i) - gap;
	    resize(s, c, m->wx + mw + gap, m->wy + ty, m->ww - mw - 2 * c->bw - 2 * gap, h - 2 * c->bw, 0);
	    if (ty + c->h + 2 * c->bw + gap < m->wh) ty += c->h + 2 * c->bw + gap;
	}
    }
}

static void monocle(WMState *s, Monitor *m) {
    int n = 0;
    for (Client *c = m->clients; c; c = c->next)
	if (isvisible(m, c)) n++;
    if (n > 0)
	snprintf(m->ltsymbol, sizeof(m->ltsymbol), "<%d>", n);
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
    if (s->sel->isfullscreen && s->lockfullscreen) return;
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
    if (!sendevent(s, s->sel->win, s->wm_protocols, s->wm_delete_window, CurrentTime, 0, 0, 0)) {
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

static void setsticky(WMState *s, Client *c, int sticky) {
    if (sticky && !c->issticky) {
	XChangeProperty(s->dpy, c->win, s->net_wm_state, XA_ATOM, 32,
			PropModeReplace, (unsigned char *)&s->net_wm_sticky, 1);
	c->issticky = 1;
    } else if (!sticky && c->issticky) {
	XChangeProperty(s->dpy, c->win, s->net_wm_state, XA_ATOM, 32,
			PropModeReplace, (unsigned char *)0, 0);
	c->issticky = 0;
	arrange(s, c->mon);
    }
}

static void togglesticky(WMState *s, const Arg *arg) {
    (void)arg;
    if (!s->sel) return;
    setsticky(s, s->sel, !s->sel->issticky);
    arrange(s, s->selmon);
}

static void setgaps(WMState *s, const Arg *arg) {
    if (arg->i == 0 || s->selmon->gappx + arg->i < 0)
	s->selmon->gappx = 0;
    else
	s->selmon->gappx += arg->i;
    arrange(s, s->selmon);
}

static void setgapsabs(WMState *s, const Arg *arg) {
    s->selmon->gappx = arg->i;
    arrange(s, s->selmon);
}

static void setborderpx(WMState *s, const Arg *arg) {
    if (arg->i == 0 || (int)s->borderpx + arg->i < 0)
	s->borderpx = 0;
    else
	s->borderpx += (unsigned int)arg->i;
    for (Monitor *m = s->mons; m; m = m->next)
	for (Client *c = m->clients; c; c = c->next) {
	    c->bw = (int)s->borderpx;
	    XSetWindowBorderWidth(s->dpy, c->win, (unsigned)c->bw);
	}
    arrange(s, s->selmon);
}

static void setborderpxabs(WMState *s, const Arg *arg) {
    s->borderpx = arg->i < 0 ? 0 : (unsigned int)arg->i;
    for (Monitor *m = s->mons; m; m = m->next)
	for (Client *c = m->clients; c; c = c->next) {
	    c->bw = (int)s->borderpx;
	    XSetWindowBorderWidth(s->dpy, c->win, (unsigned)c->bw);
	}
    arrange(s, s->selmon);
}

/* ported from movestack.c: swap the focused client with its neighbour
 * in the tiling stack order (MOD+Shift+j/k) */
static void movestack(WMState *s, const Arg *arg) {
    Client *c = NULL, *p = NULL, *pc = NULL, *i;
    Monitor *mon = s->selmon;
    if (!s->sel) return;

    if (arg->i > 0) {
	for (c = s->sel->next; c && (!isvisible(mon, c) || c->isfloating); c = c->next);
	if (!c)
	    for (c = mon->clients; c && (!isvisible(mon, c) || c->isfloating); c = c->next);
    } else {
	for (i = mon->clients; i != s->sel; i = i->next)
	    if (isvisible(mon, i) && !i->isfloating) c = i;
	if (!c)
	    for (; i; i = i->next)
		if (isvisible(mon, i) && !i->isfloating) c = i;
    }

    for (i = mon->clients; i && (!p || !pc); i = i->next) {
	if (i->next == s->sel) p = i;
	if (i->next == c) pc = i;
    }

    if (c && c != s->sel) {
	Client *temp = (s->sel->next == c) ? s->sel : s->sel->next;
	s->sel->next = (c->next == s->sel) ? c : c->next;
	c->next = temp;

	if (p && p != c) p->next = c;
	if (pc && pc != s->sel) pc->next = s->sel;

	if (s->sel == mon->clients) mon->clients = c;
	else if (c == mon->clients) mon->clients = s->sel;

	syncglobalclients(s);
	arrange(s, mon);
    }
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
    cleanup(s);
    exitcode = REWM_QUIT;
    s->running = 0;
}

static void reload_wm(WMState *s, const Arg *arg) {
    (void)arg;
    exitcode = REWM_RELOAD;
    s->running = 0;
}

/* =========================================================================
 *  Multi-monitor support
 * ========================================================================= */
static int isuniquegeom(XineramaScreenInfo *unique, size_t n, XineramaScreenInfo *info) {
    while (n--)
	if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org &&
	    unique[n].width == info->width && unique[n].height == info->height)
	    return 0;
    return 1;
}

static int updategeom(WMState *s) {
    int dirty = 0;

    if (XineramaIsActive(s->dpy)) {
	int n;
	XineramaScreenInfo *info = XineramaQueryScreens(s->dpy, &n);
	if (info) {
	    XineramaScreenInfo *unique = malloc(n * sizeof(*unique));
	    if (unique) {
		size_t nn = 0;
		for (int i = 0; i < n; i++)
		    if (isuniquegeom(unique, nn, &info[i]))
			unique[nn++] = info[i];
		XFree(info);

		Monitor *m = s->mons;
		for (size_t i = 0; i < nn; i++) {
		    if (!m) {
			m = createmon(s);
		    }
		    if (m->mx != unique[i].x_org || m->my != unique[i].y_org ||
			m->mw != unique[i].width || m->mh != unique[i].height) {
			dirty = 1;
			m->mx = m->wx = unique[i].x_org;
			m->my = m->wy = unique[i].y_org;
			m->mw = unique[i].width;
			m->mh = unique[i].height;
			updatebarpos(s, m);
			resizebarwin(s, m);
		    }
		    m = m->next;
		}
		free(unique);
	    } else {
		XFree(info);
	    }
	}
    } else {
	if (!s->mons) {
	    s->mons = createmon(s);
	}
	Monitor *m = s->mons;
	if (m->mw != s->sw || m->mh != s->sh) {
	    dirty = 1;
	    m->mw = m->ww = s->sw;
	    m->mh = m->wh = s->sh;
	    updatebarpos(s, m);
	    resizebarwin(s, m);
	}
    }

    if (dirty) {
	s->selmon = s->mons;
    }

    if (!s->selmon) {
	s->selmon = s->mons;
    }

    return dirty;
}

static Monitor *createmon(WMState *s) {
    Monitor *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    m->num = 0;
    m->mfact = DEFAULT_MFACT;
    m->nmaster = DEFAULT_NMASTER;
    m->gappx = DEFAULT_GAP;
    m->showbar = 1;
    m->topbar = 1;
    m->sellt = 0;
    m->tagset[0] = m->tagset[1] = 1;
    strncpy(m->ltsymbol, layouts[0].symbol, sizeof(m->ltsymbol) - 1);

    m->mx = 0;
    m->my = 0;
    m->mw = s->sw;
    m->mh = s->sh;
    m->wx = m->mx;
    m->wy = m->my;
    m->ww = m->mw;
    m->wh = m->mh;

    updatebarpos(s, m);

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixmap = ParentRelative;
    swa.event_mask = ExposureMask | ButtonPressMask | SubstructureNotifyMask;
    m->barwin = XCreateWindow(s->dpy, s->root, m->mx, m->topbar ? m->my : m->my + m->wh,
			      (unsigned)m->mw, (unsigned)s->barheight, 0, DefaultDepth(s->dpy, s->screen),
			      CopyFromParent, DefaultVisual(s->dpy, s->screen),
			      CWOverrideRedirect | CWBackPixmap | CWEventMask, &swa);
    XDefineCursor(s->dpy, m->barwin, s->cur_normal);
    XMapRaised(s->dpy, m->barwin);
    XClassHint ch = { "rewm", "rewm" };
    XSetClassHint(s->dpy, m->barwin, &ch);

    m->next = NULL;
    if (s->mons) {
	Monitor *last = s->mons;
	while (last->next) last = last->next;
	last->next = m;
	m->num = last->num + 1;
    } else {
	s->mons = m;
    }

    return m;
}

static void cleanupmon(WMState *s, Monitor *m) {
    if (!m) return;

    if (m->barwin) {
	XUnmapWindow(s->dpy, m->barwin);
	XDestroyWindow(s->dpy, m->barwin);
	m->barwin = 0;
    }

    if (m == s->mons) {
	s->mons = m->next;
    } else {
	Monitor *prev = s->mons;
	while (prev && prev->next != m) prev = prev->next;
	if (prev) prev->next = m->next;
    }

    free(m);
}

static Monitor *dirtomon(WMState *s, int dir) {
    Monitor *m;

    if (dir > 0) {
	if (!(m = s->selmon->next))
	    m = s->mons;
    } else {
	if (s->selmon == s->mons)
	    for (m = s->mons; m->next; m = m->next);
	else
	    for (m = s->mons; m->next != s->selmon; m = m->next);
    }
    return m;
}

static void focusmon(WMState *s, const Arg *arg) {
    Monitor *m = dirtomon(s, arg->i);
    if (m == s->selmon) return;
    unfocus(s, s->sel, 0);
    s->selmon = m;
    focus(s, NULL);
}

static void tagmon(WMState *s, const Arg *arg) {
    if (!s->sel) return;
    Monitor *m = dirtomon(s, arg->i);
    if (m == s->sel->mon) return;
    detach(s, s->sel);
    detachstack(s, s->sel);
    s->sel->mon = m;
    attach(s, s->sel);
    attachstack(s, s->sel);
    focus(s, NULL);
    arrange(s, NULL);
}

static void resizebarwin(WMState *s, Monitor *m) {
    if (!m || !m->barwin) return;
    XMoveResizeWindow(s->dpy, m->barwin, m->mx, m->topbar ? m->my : m->my + m->wh,
		      (unsigned)m->mw, (unsigned)s->barheight);
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
	if (iswmwin(s, wins[i])) continue;
	if (wintoclient(s, wins[i])) continue;

	if (wa.map_state == IsViewable) {
	    Window trans = None;
	    if (XGetTransientForHint(s->dpy, wins[i], &trans)) continue;
	    createclient(s, wins[i], &wa);
	} else {
	    Atom state;
	    unsigned long nitems;
	    if (getstate(s, wins[i]) == IconicState) {
		createclient(s, wins[i], &wa);
	    }
	}
    }

    if (wins) XFree(wins);
}

static long getstate(WMState *s, Window w) {
    Atom real;
    int format;
    unsigned long n, extra;
    unsigned char *data = NULL;
    long result = -1;

    if (XGetWindowProperty(s->dpy, w, s->wm_state, 0L, 2L, False, s->wm_state,
			   &real, &format, &n, &extra, &data) == Success) {
	if (n != 0)
	    result = *(long *)data;
	XFree(data);
    }
    return result;
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

static void cleanup(WMState *s) {
    if (!s->initialized) return;

    Arg a = { .ui = ~0 };
    view(s, &a);

    Layout lt = { "", NULL };
    for (Monitor *m = s->mons; m; m = m->next) {
	m->sellt = 0;
	strncpy(m->ltsymbol, lt.symbol, sizeof(m->ltsymbol) - 1);
    }

    XUngrabKey(s->dpy, AnyKey, AnyModifier, s->root);

    XSetInputFocus(s->dpy, s->root, RevertToPointerRoot, CurrentTime);

    while (s->clients) {
	Client *c = s->clients;
	s->clients = c->next;
	XUngrabButton(s->dpy, AnyButton, AnyModifier, c->win);
	setclientstate(s, c, WithdrawnState);
	XSetWindowBorderWidth(s->dpy, c->win, (unsigned)c->oldbw);
	free(c);
    }

    while (s->mons) {
	cleanupmon(s, s->mons);
    }

    if (s->wmcheckwin) {
	XDestroyWindow(s->dpy, s->wmcheckwin);
	s->wmcheckwin = 0;
    }

    freexftcolors(s);
    freefonts(s);
    if (s->xftdraw) { XftDrawDestroy(s->xftdraw); s->xftdraw = NULL; }
    if (s->drawable) { XFreePixmap(s->dpy, s->drawable); s->drawable = 0; }
    if (s->gc) { XFreeGC(s->dpy, s->gc); s->gc = NULL; }

    XDeleteProperty(s->dpy, s->root, s->net_supported);
    XDeleteProperty(s->dpy, s->root, s->net_wm_check);
    XDeleteProperty(s->dpy, s->root, s->net_client_list);
    XDeleteProperty(s->dpy, s->root, s->net_active_window);

    s->initialized = 0;
}

/* =========================================================================
 *  Entry point — called from host.c after JIT compile
 * ========================================================================= */
int wm_entry(WMState *s) {
    exitcode = REWM_QUIT;

    /* Defensive: release any grab that might still be held from a
     * previous wm_entry() invocation (e.g. one that exited mid-drag,
     * or hit an unexpected code path). A stuck Sync-mode grab freezes
     * input for the whole X session, and reload should always be able
     * to recover from that rather than compound it. */
    XUngrabPointer(s->dpy, CurrentTime);
    XUngrabKeyboard(s->dpy, CurrentTime);
    XUngrabServer(s->dpy);
    XAllowEvents(s->dpy, ReplayPointer, CurrentTime);
    XSync(s->dpy, False);

    /* ---- atoms (cheap; re-intern every reload, ids are stable) --------- */
    s->wm_protocols      = XInternAtom(s->dpy, "WM_PROTOCOLS",      False);
    s->wm_delete_window  = XInternAtom(s->dpy, "WM_DELETE_WINDOW",  False);
    s->wm_state          = XInternAtom(s->dpy, "WM_STATE",          False);
    s->wm_take_focus     = XInternAtom(s->dpy, "WM_TAKE_FOCUS",     False);
    s->net_wm_name       = XInternAtom(s->dpy, "_NET_WM_NAME",      False);
    s->net_wm_state      = XInternAtom(s->dpy, "_NET_WM_STATE",     False);
    s->net_wm_fullscreen = XInternAtom(s->dpy, "_NET_WM_STATE_FULLSCREEN", False);
    s->net_wm_sticky     = XInternAtom(s->dpy, "_NET_WM_STATE_STICKY", False);
    s->net_active_window = XInternAtom(s->dpy, "_NET_ACTIVE_WINDOW", False);
    s->net_client_list   = XInternAtom(s->dpy, "_NET_CLIENT_LIST",  False);
    s->utf8_string       = XInternAtom(s->dpy, "UTF8_STRING",       False);

    updatenumlockmask(s);

    setupcolors(s);

    s->resizehints = DEFAULT_RESIZEHINTS;
    s->lockfullscreen = DEFAULT_LOCKFULLSCREEN;
    s->refreshrate = DEFAULT_REFRESHRATE;

    /* These run on every reload so font/color changes take effect */
    setupfont(s);
    setupdrawable(s);

    for (Monitor *m = s->mons; m; m = m->next) {
	updatebarpos(s, m);
	resizebarwin(s, m);
    }

    if (!s->initialized) {
	s->sw = DisplayWidth(s->dpy, s->screen);
	s->sh = DisplayHeight(s->dpy, s->screen);

	s->gc = XCreateGC(s->dpy, s->root, 0, NULL);
	setupcursors(s);
	setupewmh(s);

	updategeom(s);

	scanwindows(s);

	s->initialized = 1;
    }

    updatestatus(s);

    grabkeys(s);
    XUngrabButton(s->dpy, AnyButton, AnyModifier, s->root);
    XSelectInput(s->dpy, s->root,
                 SubstructureRedirectMask | SubstructureNotifyMask
                 | ButtonPressMask        | PointerMotionMask
                 | EnterWindowMask        | LeaveWindowMask
                 | StructureNotifyMask    | PropertyChangeMask);

    focus(s, s->sel);
    arrange(s, NULL);
    drawbars(s);

    XEvent ev;
    while (XPending(s->dpy) > 0) {
	XNextEvent(s->dpy, &ev);
    }

    /* ---- event loop --------------------------------------------------- */
    s->running = 1;
    XSync(s->dpy, False);
    while (s->running && !s->sig_caught) {
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
	case FocusIn:            focusin(s, &ev);           break;
	case MotionNotify:       motionnotify(s, &ev);      break;
	case ResizeRequest:      resizerequest(s, &ev);     break;
	}
    }

    XSync(s->dpy, True);
    return exitcode;
}
