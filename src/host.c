/* host.c — native host that inits X11 and runs wm.c via JIT
 *
 * Compile with:
 *   cc -o rewm host.c src/c2mir-lib.c deps/cmm/{mir, mir-gen, c2mir/c2mir}.c \
 *      -I deps/cmm -I deps/cmm/c2mir -lm -ldl -lpthread -lX11
 */

#include "c2mir-lib.h"
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 *  X error handling
 *
 *  Xlib's default error handler calls exit() on *any* protocol error.
 *  Races on window close (we touch a window that just got destroyed
 *  out from under us) are completely normal in a WM and must not be
 *  fatal -- this is what was killing rewm on window close.
 * ------------------------------------------------------------------------- */
static int xerrorstart(Display *dpy, XErrorEvent *ee) {
    (void)dpy; (void)ee;
    fprintf(stderr, "rewm: another window manager is already running\n");
    exit(1);
}

static int xerror(Display *dpy, XErrorEvent *ee) {
    (void)dpy;
    if (ee->error_code == BadWindow) return 0;
    fprintf(stderr, "rewm: X error: request=%d error=%d resourceid=%lu (ignored)\n",
            ee->request_code, ee->error_code, ee->resourceid);
    return 0; /* never let a protocol error take the process down */
}

/* -------------------------------------------------------------------------
 *  X11 initialisation
 * ------------------------------------------------------------------------- */
static Display *x11_init(WMState *s) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "rewm: can't open display\n");
        return NULL;
    }
    s->dpy    = dpy;
    s->screen = DefaultScreen(dpy);
    s->root   = RootWindow(dpy, s->screen);

    /* temporary handler: if SubstructureRedirectMask fails with
     * BadAccess, another WM already owns this display */
    XSetErrorHandler(xerrorstart);
    XSelectInput(dpy, s->root,
                 SubstructureRedirectMask | SubstructureNotifyMask
                 | ButtonPressMask | KeyPressMask | PointerMotionMask
                 | EnterWindowMask | LeaveWindowMask | StructureNotifyMask
                 | PropertyChangeMask);
    XSync(dpy, False);
    /* permanent handler: log and shrug off everything else */
    XSetErrorHandler(xerror);

    /* create a single monitor covering the whole screen */
    Monitor *m = calloc(1, sizeof(*m));
    if (!m) { XCloseDisplay(dpy); return NULL; }
    m->num  = 0;
    m->mx   = 0;  m->my   = 0;
    m->mw   = DisplayWidth(dpy, s->screen);
    m->mh   = DisplayHeight(dpy, s->screen);
    m->wx   = 0;  m->wy   = 0;
    m->ww   = m->mw;
    m->wh   = m->mh;
    m->tagset[0] = m->tagset[1] = 1;
    m->seltags = 0;
    m->sellt   = 0;

    s->mons   = m;
    s->selmon = m;

    /* geometry defaults */
    s->borderpx = 2;
    s->snap     = 32;

    /* grab server while we set up */
    XGrabServer(dpy);

    /* manage existing top-level windows */
    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;
    if (XQueryTree(dpy, s->root, &root_return, &parent_return,
                   &children, &nchildren)) {
        /* Children that exist before WM starts need to be handled
         * by the JIT code — for now just map them */
        for (unsigned int i = 0; i < nchildren; i++) {
            XWindowAttributes wa;
            if (children[i] != s->root
                && XGetWindowAttributes(dpy, children[i], &wa)
                && !wa.override_redirect && wa.map_state == IsViewable) {
                XMapWindow(dpy, children[i]);
            }
        }
        XFree(children);
    }

    /* Sync & ungrab */
    XSync(dpy, False);
    XUngrabServer(dpy);

    XSync(dpy, False);
    fprintf(stderr, "rewm: X11 init done (%dx%d)\n", m->mw, m->mh);
    return dpy;
}

/* -------------------------------------------------------------------------
 *  Watch source file for changes (simple mtime poll)
 * ------------------------------------------------------------------------- */
static int file_changed(const char *path, long *mtime_out) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (st.st_mtime != *mtime_out) {
        *mtime_out = st.st_mtime;
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 *  Clean-up
 * ------------------------------------------------------------------------- */
static void wm_cleanup(WMState *s) {
    if (s->dpy) {
        XCloseDisplay(s->dpy);
        s->dpy = NULL;
    }
}

/* -------------------------------------------------------------------------
 *  SIGINT / SIGTERM handler
 * ------------------------------------------------------------------------- */
static volatile int sig_caught = 0;
static void sig_handler(int sig) {
    (void)sig;
    sig_caught = 1;
}

/* -------------------------------------------------------------------------
 *  Main
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    char *srcpath = getenv("REWM_PATH");
    if (!srcpath) {
	fprintf(stderr, "rewm: REWM_PATH is not set, we dont know where are your source code at... sorry!\n");
	return 1;
    }
    size_t srcsize = snprintf(NULL, 0, "%s/wm.c", srcpath);
    char *srcloc = calloc(sizeof(srcsize), 1);
    if (snprintf(srcloc, srcsize, "%s/wm.c", srcpath) == 0) {
	fprintf(stderr, "rewm: failed to allocate the path on REWM_PATH... sorry!\n");
	return 1;
    }

    /* ---- signal handling ---- */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* ---- WM state (survives reload) ---- */
    WMState state;
    memset(&state, 0, sizeof(state));
    state.reload_count = 0;

    /* ---- X11 init ---- */
    if (!x11_init(&state)) return 1;

    /* ---- path to wm.c ---- */
    /* default: look next to the binary or in CWD */

    typedef int (*wm_entry_fn)(WMState *);

    /* ---- compiler context / currently-live wm_entry ---- */
    rewm_compiler_t *rc = NULL;
    wm_entry_fn wm_entry = NULL;
    int had_success = 0; /* has any build ever succeeded yet? */

    /* ---- file mtime tracking ---- */
    long wm_mtime = 0;
    {
        struct stat st;
        if (stat(srcloc, &st) == 0) wm_mtime = st.st_mtime;
    }
    int first = 1;

    /* This controls whether the *host process* keeps going. It is
     * deliberately separate from state.running: wm_entry() resets
     * state.running=1 on every call and sets it back to 0 whenever
     * its own event loop exits -- for a plain reload just as much as
     * for quit. Using state.running here would tear the whole host
     * down on every MOD+Shift+r. */
    int host_running = 1;
    int force_reload = 0;

    /* ---- main reload loop ---- */
    while (host_running && !sig_caught) {
        int changed = file_changed(srcloc, &wm_mtime);
        if (first || changed || force_reload) {
            first = 0;
            force_reload = 0;
            fprintf(stderr, "rewm: (re)compiling %s...\n", srcloc);

            /* Build the candidate in a brand-new context. The old
             * context (and its still-valid wm_entry) is left alone
             * until we know the new one actually works. */
            rewm_compiler_t *new_rc = rewm_compiler_create();
            wm_entry_fn new_entry = NULL;
            if (new_rc) {
                rewm_set_optimize_level(new_rc, 3);
                new_entry = (wm_entry_fn)
                    rewm_compile_and_get(new_rc, srcloc, "wm_entry");
            }

            if (!new_entry) {
                fprintf(stderr, "rewm: compile failed, keeping previous version\n");
                if (new_rc) rewm_compiler_destroy(new_rc);
                if (!wm_entry) {
                    /* never had a working build to fall back to yet */
                    usleep(200000); /* 200ms, avoid busy-spinning on a broken file */
                    continue;
                }
                /* fall through and re-run the still-live old wm_entry */
            } else {
                /* success: swap in the new build, *then* tear down the old */
                if (rc) rewm_compiler_destroy(rc);
                rc = new_rc;
                wm_entry = new_entry;
                if (had_success) state.reload_count++;
                had_success = 1;
                fprintf(stderr, "rewm: compiled OK (reload #%d)\n", state.reload_count);
            }

            /* call into JIT-compiled WM logic */
            fprintf(stderr, "rewm: entering wm\n");
            int status = wm_entry(&state);
            fprintf(stderr, "rewm: wm returned %d\n", status);

            if (status == REWM_QUIT) {
                host_running = 0;
            } else if (status == REWM_RELOAD) {
                force_reload = 1;
            }
        } else {
            usleep(50000); /* 50ms */
        }
    }

    if (rc) rewm_compiler_destroy(rc);
    wm_cleanup(&state);
    free(srcloc);
    fprintf(stderr, "rewm: bye\n");
    return 0;
}
