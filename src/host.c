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
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdint.h>
#include <libudev.h>
#include <libinput.h>
#include <linux/input-event-codes.h>

#define GESTURE_CMD_DELTA 1
#define GESTURE_CMD_CANCEL 2

typedef struct {
    uint8_t type;
    int32_t delta_px;
} GestureCommand;

typedef struct {
    struct libinput *li;
    struct udev *udev;
    int stop_fd;
    int output_fd;
    int li_fd;
    int active;
    int valid;
    unsigned int fingers;
    double total_dx;
    double total_dy;
    double pending_dx;
} GestureThreadState;

static int li_open_restricted(const char *path, int flags, void *user_data) {
    (void)user_data;
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}

static void li_close_restricted(int fd, void *user_data) {
    (void)user_data;
    close(fd);
}

static const struct libinput_interface li_interface = {
    .open_restricted = li_open_restricted,
    .close_restricted = li_close_restricted,
};

static void gesture_write_output(int output_fd, int32_t delta_px) {
    GestureCommand cmd;
    cmd.type = GESTURE_CMD_DELTA;
    cmd.delta_px = delta_px;
    ssize_t r;
    do {
        r = write(output_fd, &cmd, sizeof(cmd));
    } while (r < 0 && errno == EINTR);
}

static void gesture_flush_pending(GestureThreadState *g) {
    if (g->pending_dx == 0.0 || !g->valid) {
        g->pending_dx = 0.0;
        return;
    }
    double sens = 0.5;
    double v = g->pending_dx * sens;
    if (v > (double)INT32_MAX) v = (double)INT32_MAX;
    if (v < (double)INT32_MIN) v = (double)INT32_MIN;
    gesture_write_output(g->output_fd, (int32_t)v);
    g->pending_dx = 0.0;
}

static void gesture_reset(GestureThreadState *g) {
    g->active = 0;
    g->valid = 0;
    g->fingers = 0;
    g->total_dx = 0.0;
    g->total_dy = 0.0;
    g->pending_dx = 0.0;
}

static void gesture_cancel(GestureThreadState *g) {
    GestureCommand cmd;
    gesture_flush_pending(g);
    cmd.type = GESTURE_CMD_CANCEL;
    cmd.delta_px = 0;
    ssize_t r;
    do {
        r = write(g->output_fd, &cmd, sizeof(cmd));
    } while (r < 0 && errno == EINTR);
    gesture_reset(g);
}

static void gesture_process_event(GestureThreadState *g, struct libinput_event *ev) {
    enum libinput_event_type type = libinput_event_get_type(ev);
    (void)libinput_event_get_device(ev);
    if (type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN) {
        struct libinput_event_gesture *gev = libinput_event_get_gesture_event(ev);
        unsigned int fingers = libinput_event_gesture_get_finger_count(gev);
        gesture_reset(g);
        g->active = 1;
        g->fingers = fingers;
        g->valid = (fingers == 3);
    } else if (type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE) {
        struct libinput_event_gesture *gev = libinput_event_get_gesture_event(ev);
        if (!g->active) return;
        unsigned int fingers = libinput_event_gesture_get_finger_count(gev);
        if (fingers != 3) g->valid = 0;
        if (!g->valid) return;
        double dx = libinput_event_gesture_get_dx(gev);
        double dy = libinput_event_gesture_get_dy(gev);
        g->total_dx += dx;
        g->total_dy += dy;
        g->pending_dx += dx;
        double adx = g->total_dx < 0.0 ? -g->total_dx : g->total_dx;
        double ady = g->total_dy < 0.0 ? -g->total_dy : g->total_dy;
        if (ady > adx * 0.75 + 8.0) {
            g->valid = 0;
            gesture_flush_pending(g);
        }
    } else if (type == LIBINPUT_EVENT_GESTURE_SWIPE_END) {
        if (g->active) {
            gesture_flush_pending(g);
            gesture_reset(g);
        }
    }
}

static void gesture_backend_free_arg(void *arg) {
    GestureThreadState *g = (GestureThreadState *)arg;
    if (!g) return;
    if (g->li) {
        libinput_unref(g->li);
        g->li = NULL;
    }
    if (g->udev) {
        udev_unref(g->udev);
        g->udev = NULL;
    }
    free(g);
}

static void gesture_backend_request_stop(WMState *s);
static void gesture_backend_join(WMState *s);

static void *gesture_thread_fn(void *arg) {
    GestureThreadState *g = (GestureThreadState *)arg;
    struct pollfd fds[2];
    fds[0].fd = g->stop_fd;
    fds[0].events = POLLIN;
    fds[1].fd = g->li_fd;
    fds[1].events = POLLIN;

    for (;;) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & POLLIN) {
            char buf[8];
            (void)read(g->stop_fd, buf, sizeof(buf));
            break;
        }
        if (fds[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
            if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                gesture_cancel(g);
                break;
            }
            libinput_dispatch(g->li);
            struct libinput_event *ev;
            while ((ev = libinput_get_event(g->li)) != NULL) {
                gesture_process_event(g, ev);
                libinput_event_destroy(ev);
            }
        }
    }

    gesture_cancel(g);
    gesture_backend_free_arg(g);
    return NULL;
}

static void gesture_backend_request_stop(WMState *s) {
    if (s->gesture_stop_pipe[1] < 0) return;
    char c = 1;
    ssize_t r;
    do {
        r = write(s->gesture_stop_pipe[1], &c, 1);
    } while (r < 0 && errno == EINTR);
}

static void gesture_backend_join(WMState *s) {
    if (s->gesture_thread_running && s->gesture_thread) {
        pthread_join(s->gesture_thread, NULL);
        s->gesture_thread_running = 0;
        s->gesture_thread = 0;
    }
    if (s->gesture_stop_pipe[0] >= 0) {
        close(s->gesture_stop_pipe[0]);
        s->gesture_stop_pipe[0] = -1;
    }
    if (s->gesture_stop_pipe[1] >= 0) {
        close(s->gesture_stop_pipe[1]);
        s->gesture_stop_pipe[1] = -1;
    }
    if (s->gesture_event_pipe[0] >= 0) {
        close(s->gesture_event_pipe[0]);
        s->gesture_event_pipe[0] = -1;
    }
    if (s->gesture_event_pipe[1] >= 0) {
        close(s->gesture_event_pipe[1]);
        s->gesture_event_pipe[1] = -1;
    }
}

static int gesture_backend_start(WMState *s) {
    if (s->gesture_thread_running) return 0;
    if (pipe(s->gesture_stop_pipe) < 0) return -1;
    if (pipe(s->gesture_event_pipe) < 0) {
        close(s->gesture_stop_pipe[0]);
        close(s->gesture_stop_pipe[1]);
        s->gesture_stop_pipe[0] = -1;
        s->gesture_stop_pipe[1] = -1;
        return -1;
    }
    int flags = fcntl(s->gesture_event_pipe[0], F_GETFL, 0);
    if (flags >= 0) fcntl(s->gesture_event_pipe[0], F_SETFL, flags | O_NONBLOCK);

    GestureThreadState *g = calloc(1, sizeof(GestureThreadState));
    if (!g) goto fail;
    g->stop_fd = s->gesture_stop_pipe[0];
    g->output_fd = s->gesture_event_pipe[1];
    g->li_fd = -1;

    g->udev = udev_new();
    if (!g->udev) {
        fprintf(stderr, "rewm: gesture: udev_new failed\n");
        goto fail;
    }
    g->li = libinput_udev_create_context(&li_interface, NULL, g->udev);
    if (!g->li) {
        fprintf(stderr, "rewm: gesture: libinput_udev_create_context failed\n");
        goto fail;
    }
    int rc = libinput_udev_assign_seat(g->li, "seat0");
    if (rc < 0) {
        fprintf(stderr, "rewm: gesture: libinput_udev_assign_seat failed: %d\n", rc);
        goto fail;
    }
    g->li_fd = libinput_get_fd(g->li);
    if (g->li_fd < 0) {
        fprintf(stderr, "rewm: gesture: libinput_get_fd failed\n");
        goto fail;
    }
    libinput_dispatch(g->li);

    if (pthread_create(&s->gesture_thread, NULL, gesture_thread_fn, g) != 0) {
        fprintf(stderr, "rewm: gesture: pthread_create failed\n");
        goto fail;
    }
    s->gesture_thread_running = 1;
    s->gesture_backend_ready = 1;
    fprintf(stderr, "rewm: gesture backend started\n");
    return 0;

fail:
    if (g) {
        if (g->li) libinput_unref(g->li);
        if (g->udev) udev_unref(g->udev);
        free(g);
    }
    gesture_backend_join(s);
    s->gesture_backend_ready = 0;
    return -1;
}

static void gesture_backend_cleanup(WMState *s) {
    gesture_backend_request_stop(s);
    gesture_backend_join(s);
    s->gesture_backend_ready = 0;
}

void rewm_drain_gesture_commands(WMState *s);

void rewm_drain_gesture_commands(WMState *s) {
    if (s->gesture_event_pipe[0] < 0) return;
    GestureCommand cmd;
    int32_t total = 0;
    int got = 0;
    for (;;) {
        ssize_t r = read(s->gesture_event_pipe[0], &cmd, sizeof(cmd));
        if (r == sizeof(cmd)) {
            if (cmd.type == GESTURE_CMD_DELTA) {
                total += cmd.delta_px;
                got = 1;
            } else if (cmd.type == GESTURE_CMD_CANCEL) {
                total = 0;
                got = 0;
            }
        } else if (r < 0 && errno == EAGAIN) {
            break;
        } else {
            break;
        }
    }
    if (got) {
        s->gesture_accum = total;
    }
}

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
    state.gesture_stop_pipe[0] = -1;
    state.gesture_stop_pipe[1] = -1;
    state.gesture_event_pipe[0] = -1;
    state.gesture_event_pipe[1] = -1;
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

    /* start gesture backend before first wm_entry */
    gesture_backend_start(&state);

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

    gesture_backend_cleanup(&state);
    if (rc) rewm_compiler_destroy(rc);
    wm_cleanup(&state);
    free(srcloc);
    fprintf(stderr, "rewm: bye\n");
    return 0;
}
