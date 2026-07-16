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

    /* select events */
    XSelectInput(dpy, s->root,
                 SubstructureRedirectMask | SubstructureNotifyMask
                 | ButtonPressMask | KeyPressMask | PointerMotionMask
                 | EnterWindowMask | LeaveWindowMask | StructureNotifyMask
                 | PropertyChangeMask);

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

    /* ---- signal handling ---- */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* ---- WM state (survives reload) ---- */
    WMState state;
    memset(&state, 0, sizeof(state));
    state.reload_count = 0;
    state.running = 1;

    /* ---- X11 init ---- */
    if (!x11_init(&state)) return 1;

    /* ---- path to wm.c ---- */
    /* default: look next to the binary or in CWD */
    const char *wm_source = "src/wm.c";

    /* ---- compiler context ---- */
    rewm_compiler_t *rc = rewm_compiler_create();
    if (!rc) {
        fprintf(stderr, "rewm: failed to create compiler\n");
        wm_cleanup(&state);
        return 1;
    }
    rewm_set_optimize_level(rc, 3);

    /* ---- file mtime tracking ---- */
    long wm_mtime = 0;
    int first = 1;

    /* ---- main reload loop ---- */
    while (state.running && !sig_caught) {
        if (first || file_changed(wm_source, &wm_mtime)) {
            if (!first) {
                fprintf(stderr, "rewm: %s changed, recompiling...\n", wm_source);
                /* tear down MIR context completely and re-create */
                rewm_compiler_destroy(rc);
                rc = rewm_compiler_create();
                if (!rc) break;
                rewm_set_optimize_level(rc, 3);
                state.reload_count++;
            }
            first = 0;

            /* compile wm.c and get wm_entry pointer */
            typedef int (*wm_entry_fn)(WMState *);
            wm_entry_fn wm_entry = (wm_entry_fn)
                rewm_compile_and_get(rc, wm_source, "wm_entry");

            if (!wm_entry) {
                fprintf(stderr, "rewm: failed to compile %s\n", wm_source);
                continue;
            }

            /* call into JIT-compiled WM logic */
            fprintf(stderr, "rewm: entering wm (reload #%d)\n", state.reload_count);
            int status = wm_entry(&state);
            fprintf(stderr, "rewm: wm returned %d\n", status);

            if (status == REWM_QUIT) {
                state.running = 0;
            }
            /* REWM_RELOAD: loop back and recompile */
        } else {
            /* If the WM returned RELOAD but file hasn't changed yet,
             * wait a bit before retrying */
            usleep(50000); /* 50ms */
        }
    }

    rewm_compiler_destroy(rc);
    wm_cleanup(&state);
    fprintf(stderr, "rewm: bye\n");
    return 0;
}
