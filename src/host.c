#include "c2mir-lib.h"
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <locale.h>

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
    return 0;
}

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

    XGrabServer(dpy);
    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;
    if (XQueryTree(dpy, s->root, &root_return, &parent_return,
                   &children, &nchildren)) {
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

    XSync(dpy, False);
    XUngrabServer(dpy);

    XSync(dpy, False);
    fprintf(stderr, "rewm: X11 init done (%dx%d)\n", DisplayWidth(dpy, s->screen), DisplayHeight(dpy, s->screen));
    return dpy;
}

static void wm_cleanup(WMState *s) {
    if (s->dpy) {
        XCloseDisplay(s->dpy);
        s->dpy = NULL;
    }
}

static WMState *sig_state = NULL;
static void sig_handler(int sig) {
    (void)sig;
    if (sig_state)
        sig_state->sig_caught = 1;
}

static int check_other_wm(Display *dpy) {
    XSetErrorHandler(xerrorstart);
    XSelectInput(dpy, RootWindow(dpy, DefaultScreen(dpy)), SubstructureRedirectMask);
    XSync(dpy, False);
    XSetErrorHandler(xerror);
    XSync(dpy, False);
    return 0;
}

static void parse_cflags(rewm_compiler_t *rc) {
    const char *cflags = getenv("REWM_CFLAGS");
    if (!cflags) return;

    char *buf = strdup(cflags);
    if (!buf) return;

    char *token = strtok(buf, " \t");
    while (token) {
        if (strncmp(token, "-I", 2) == 0) {
            const char *path = token + 2;
            if (*path) {
                char *path_copy = strdup(path);
                if (path_copy) {
                    rewm_add_include_dir(rc, path_copy);
                }
            }
        } else if (strncmp(token, "-L", 2) == 0) {
            const char *path = token + 2;
            if (*path) {
                char *path_copy = strdup(path);
                if (path_copy) {
                    rewm_add_lib_dir(rc, path_copy);
                }
            }
        } else if (strncmp(token, "-l", 2) == 0) {
            const char *name = token + 2;
            if (*name) {
                rewm_add_lib(rc, name);
            }
        }
        token = strtok(NULL, " \t");
    }

    free(buf);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    mallopt(M_MMAP_THRESHOLD, 64 * 1024);

    char *srcpath = getenv("REWM_PATH");
    if (!srcpath) {
        fprintf(stderr, "rewm: REWM_PATH is not set, we dont know where are your source code at... sorry!\n");
        return 1;
    }
    size_t srcsize = snprintf(NULL, 0, "%s/wm.cmm", srcpath) + 1;
    char *srcloc = calloc(srcsize, 1);
    if (snprintf(srcloc, srcsize, "%s/wm.cmm", srcpath) == 0) {
        fprintf(stderr, "rewm: failed to allocate the path on REWM_PATH... sorry!\n");
        return 1;
    }

    WMState state;
    memset(&state, 0, sizeof(state));
    state.reload_count = 0;
    state.sig_caught = 0;
    sig_state = &state;  /* set before installing signal handler */

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    /* reap any pre-existing zombies */
    while (waitpid(-1, NULL, WNOHANG) > 0);

    setlocale(LC_CTYPE, "");
    if (!XSupportsLocale())
        fprintf(stderr, "rewm: no locale support\n");

    if (!x11_init(&state)) return 1;
    if (check_other_wm(state.dpy)) {
        fprintf(stderr, "rewm: another window manager is already running\n");
        wm_cleanup(&state);
        return 1;
    }

    typedef int (*wm_entry_fn)(WMState *); // func call for the cmm entry point
    rewm_compiler_t *rc = NULL;
    wm_entry_fn wm_entry = NULL;
    int had_success = 0;
    long wm_mtime = 0;
    {
        struct stat st;
        if (stat(srcloc, &st) == 0) wm_mtime = st.st_mtime;
    }
    int first = 1;
    int host_running = 1;
    int force_reload = 0;

    while (host_running && !state.sig_caught) {
        if (first || force_reload) {
            first = 0;
            force_reload = 0;
            fprintf(stderr, "rewm: (re)compiling %s...\n", srcloc);

            rewm_compiler_t *new_rc = rewm_compiler_create();
            wm_entry_fn new_entry = NULL;
            if (new_rc) {
                parse_cflags(new_rc);
                rewm_set_optimize_level(new_rc, 2);
                new_entry = (wm_entry_fn)
                    rewm_compile_and_get(new_rc, srcloc, "wm_entry");
            }

            if (!new_entry) {
                fprintf(stderr, "rewm: compile failed, keeping previous version\n");
                if (new_rc) {
                    rewm_compiler_destroy(new_rc);
                    malloc_trim(0);
                }
                if (!wm_entry) {
                    fprintf(stderr, "rewm:"
                            "no previous entry and the current src file have an error"
                            ", exiting...\n");
                    return 1;
                }
                // fallthrough....
            } else {
                if (rc) {
                    rewm_compiler_destroy(rc);
                    malloc_trim(0);
                }
                rc = new_rc;
                wm_entry = new_entry;
                if (had_success) state.reload_count++;
                had_success = 1;
                fprintf(stderr, "rewm: compiled OK (reload #%d)\n", state.reload_count);
            }

            fprintf(stderr, "rewm: entering wm\n");
            int status = wm_entry(&state);
            fprintf(stderr, "rewm: wm returned %d\n", status);

            if (status == REWM_QUIT) {
                host_running = 0;
            } else if (status == REWM_RELOAD) {
                force_reload = 1;
            }
        } else {
            usleep(50000); // 50ms
        }
    }

    if (rc) rewm_compiler_destroy(rc);
    wm_cleanup(&state);
    free(srcloc);
    fprintf(stderr, "rewm: bye\n");
    return 0;
}
