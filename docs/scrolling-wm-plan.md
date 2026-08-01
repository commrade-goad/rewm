# Scrolling Window Manager Mode — Technical Implementation Plan

## 1. Objective

Add a PaperWM/niri-style horizontal scrolling layout to `rewm` while preserving the existing tile, monocle, and floating layouts. Add a direct `libinput` gesture backend for three-finger horizontal touchpad swipes; do not use an X11/libinput wrapper.

The scrolling layout will:

- Arrange visible tiled clients in a single horizontal strip.
- Keep tiled clients vertically fitted to the monitor work area.
- Permit the strip to be wider than the monitor viewport.
- Maintain a per-monitor horizontal scroll offset.
- Automatically reveal the focused client.
- Support keyboard scrolling and mouse-driven strip scrolling.
- Support three-finger horizontal touchpad swipes through direct `libinput` device access.
- Preserve normal floating, fullscreen, tagging, reload, and EWMH behavior.

The implementation spans `src/wm.cmm`, `src/wm.h`, `src/host.c`, and `build.c`. `libinput` must be linked directly and run against Linux evdev devices, not through an X11 input wrapper. X11 calls remain on the WM event-loop thread; the input thread communicates gestures through a pipe/event queue.

## 2. Existing architecture and constraints

### Runtime model

- `src/host.c` loads `$REWM_PATH/wm.cmm`.
- The CMM source is compiled into a function named `wm_entry`.
- `WMState` is allocated by the host and survives source reloads.
- `wm_entry` is called again after `REWM_RELOAD`.
- X11 objects and client state are intended to survive reloads.

### Existing layout dispatch

Current layout dispatch is:

```c
Layout {
    const char *symbol;
    void (*arrange)(WMState *, Monitor *);
};
```

The current layout table is:

```c
static layouts: []Layout = {
    { "[T]", tile },
    { "[M]", monocle },
    { "[F]", floating },
};
```

`arrange()` calls `showhide()`, then `arrangemon()`, then `restack()`.

### Existing client filtering

A tiled client is currently identified by `nexttiled()`:

```c
c.isfloating == 0 && isvisible(c.mon, c)
```

Fullscreen clients are handled separately through `isfullscreen`. The scrolling layout must use the same tag visibility rules and exclude floating/fullscreen clients from strip geometry.

### Existing monitor state

`Monitor` currently contains geometry, gaps, tags, layout selection, client lists, and bar state. It does not contain a horizontal scroll offset. The offset must be added to `Monitor` so each monitor can scroll independently.

### Input/threading constraints

- `libinput` direct mode requires access to `/dev/input/event*`, normally through a seat/session manager such as logind/seatd or an appropriate udev ACL.
- The libinput context must be created with `libinput_udev_create_context()` and assigned to the active seat with `libinput_udev_assign_seat()`.
- The libinput file descriptor must be monitored with `poll()`/`ppoll()` and serviced using the required `libinput_dispatch()` / `libinput_get_event()` sequence.
- The libinput context must not be accessed concurrently from the X11 thread. All libinput calls belong to the gesture thread.
- The gesture thread must not call Xlib, `arrange()`, `focus()`, or mutate `WMState`/`Monitor` directly.
- Gesture commands must cross into the WM thread through a nonblocking self-pipe or `eventfd`; the X11 loop handles the readable notification and applies the scroll delta.
- If Xlib is ever accessed from more than the WM thread, `XInitThreads()` would have to run before `XOpenDisplay()`. The preferred design avoids that requirement entirely.

## 3. Data model changes

### 3.1 Add scrolling constants/configuration

Add configuration values near the existing layout constants in `src/wm.cmm`:

```c
#define SCROLL_STEP       300
#define SCROLL_MIN_WIDTH  200
#define SCROLL_OUTER_GAP  50
#define SCROLL_INNER_GAP  30
#define SCROLL_TITLE_GAP  0
```

Prefer existing `m.gappx` for the configurable gap where possible. If separate inner/outer gaps are required, add explicit configuration variables instead of hard-coding them inside layout functions.

### 3.2 Extend `Monitor`

Add fields to `struct Monitor` in `src/wm.h`:

```c
int scroll_x;          /* horizontal strip offset in pixels */
int scroll_align;      /* 0=left, 1=right, 2=center when strip fits */
```

If the CMM compiler requires all fields to be declared in the CMM-visible type, mirror these fields in the CMM declaration/use accordingly. Because `WMState` survives reloads, `scroll_x` must be initialized in `createmon()` and not reset on every `wm_entry()` invocation.

Use a signed integer offset:

- `0` means the strip starts at the normal left work-area edge.
- Negative values move later strip columns leftward.
- Positive values must never survive clamping.

### 3.3 Add layout-specific helper declarations

Add declarations near the existing layout/helper declarations:

```c
static scroll: (s: *WMState, m: *Monitor) void;
static scrollleft: (s: *WMState, arg: *const Arg) void;
static scrollright: (s: *WMState, arg: *const Arg) void;
static scrollcenter: (s: *WMState, arg: *const Arg) void;
static scrolltotagged: (s: *WMState, c: *Client) void;
static scrollclamp: (s: *WMState, m: *Monitor) void;
static scrollstripwidth: (s: *WMState, m: *Monitor) int;
static scrollensurevisible: (s: *WMState, m: *Monitor, c: *Client) void;
static scrollnexttiled: (m: *Monitor, c: *Client) *Client;
```

Names may be adjusted to match local conventions, but helpers should remain separated from event handlers so geometry logic is testable by inspection.

## 4. Geometry algorithm

### 4.1 Work-area dimensions

The scroll layout must use `m.wx`, `m.wy`, `m.ww`, and `m.wh`, which already account for bar placement.

Define:

```c
outer = max(m.gappx, 0);
inner = max(m.gappx, 0);
viewport_left = m.wx + outer;
viewport_right = m.wx + m.ww - outer;
viewport_width = viewport_right - viewport_left;
client_height = m.wh - 2 * outer;
```

If `client_height <= 1`, clamp it to `1` before calling `resize()`.

### 4.2 Client collection

Build an ordered array of visible strip clients by iterating `m.clients`:

- Include only `isvisible(m, c) == true`.
- Exclude `c.isfloating`.
- Exclude `c.isfullscreen`.
- Exclude fixed/unsupported special windows if the existing client lifecycle marks them separately.

Use a bounded array with `MAX_CLIENTS` or an equivalent local limit. Do not allocate per-arrange heap memory.

The order must match the monitor client list so existing `zoom()`, `movestack()`, tagging, and reload behavior remain predictable.

### 4.3 Column width

For a basic scrolling mode, each normal client receives a stable width derived from the viewport:

```c
column_width = (viewport_width - inner) / 2;
```

Then clamp:

```c
if (column_width < SCROLL_MIN_WIDTH)
    column_width = SCROLL_MIN_WIDTH;
```

A maximized client may retain or use its saved width, but the initial implementation should use one consistent column width for all normal strip clients. This avoids a changing strip width when focus changes.

### 4.4 Total strip width

For `n` strip clients:

```c
total_width = n * column_width + (n - 1) * inner + 2 * outer;
```

For `n == 0`, return `0` or the minimum work-area width consistently; callers must handle the empty case.

The helper must calculate width from the same filtering and width rules used by `scroll()`. Duplicating slightly different filtering logic will cause incorrect clamp/reveal behavior.

### 4.5 Scroll bounds

If the strip fits inside the monitor:

```c
if (total_width <= m.ww) {
    switch (m.scroll_align) {
    case 1: m.scroll_x = m.ww - total_width; break;
    case 2: m.scroll_x = (m.ww - total_width) / 2; break;
    default: m.scroll_x = 0; break;
    }
}
```

If the strip is wider than the viewport:

```c
max_scroll = -(total_width - m.ww);
if (m.scroll_x > 0) m.scroll_x = 0;
if (m.scroll_x < max_scroll) m.scroll_x = max_scroll;
```

Use monitor-relative coordinates consistently. If `total_width` includes `m.wx`, do not subtract `m.x` a second time.

### 4.6 Placement

For each strip client at index `i`:

```c
x = m.wx + outer + m.scroll_x + i * (column_width + inner);
y = m.wy + outer;
w = column_width - 2 * c.bw;
h = m.wh - 2 * outer - 2 * c.bw;
```

Call the existing `resize(s, c, x, y, w, h, 0)` function. Do not directly configure windows in the new layout.

The existing `showhide()` behavior may map clients whose geometry is outside the viewport. The layout should still clamp the strip offset and place every visible client at its logical strip position. X11 clipping will naturally prevent off-screen portions from appearing.

### 4.7 Floating and fullscreen clients

The scrolling layout must not reposition floating clients. It should retain the existing behavior:

- Floating visible clients are resized to their current geometry by the common arrange path or floating logic.
- Fullscreen clients are handled by `setfullscreen()`/existing fullscreen arrangement.
- Floating clients must not contribute to `total_width`.
- The selected floating client remains raised by `restack()`.

If the current common `arrange()` path does not preserve floating geometry correctly, make the smallest targeted change needed rather than embedding floating logic in the scroll layout.

## 5. Focus reveal algorithm

### 5.1 Trigger points

Call `scrollensurevisible()` after focusing a tiled client from:

- `focusstack()`.
- `focusmon()` if the selected client is on a scrolling monitor.
- `zoom()`.
- `view()` and `toggleview()` after selecting/restoring a client.
- `tagclient()`/`toggletag()` when the selected client remains visible.
- `maprequest()`/manage path after a new client becomes selected.
- `movestack()` after reordering.

Avoid calling it recursively from `focus()` unless the implementation can prove there is no arrange/focus recursion. Prefer explicit calls at action boundaries.

### 5.2 Locate client position

Iterate the same ordered strip-client list used by the layout. Calculate:

```c
client_left  = m.wx + outer + m.scroll_x + index * (column_width + inner);
client_right = client_left + column_width;
```

Use the actual width if the layout supports variable widths.

### 5.3 Reveal policy

If the client is left of the viewport:

```c
m.scroll_x += viewport_left - client_left;
```

If the client is right of the viewport:

```c
m.scroll_x -= client_right - viewport_right;
```

Then call `scrollclamp()`.

The first implementation should use a minimal reveal policy: reveal the smallest amount necessary. Do not center every focused window unless an explicit center mode is added later.

### 5.4 Apply geometry

After changing `scroll_x`, call `arrange(s, m)` or `scroll(s, m)` through the existing dispatch path. Avoid calling `arrange()` from inside `scrollensurevisible()` if the caller is already arranging; use a boolean/explicit caller convention to prevent duplicate arrangement.

Recommended convention:

```c
scrollensurevisible(s, m, c);  /* changes offset only */
arrange(s, m);                 /* caller applies geometry */
```

## 6. Scrolling commands and keybindings

### 6.1 Commands

Implement:

```c
static void scrollleft(WMState *s, const Arg *arg) {
    if (layouts[s->selmon->sellt].arrange != scroll) return;
    s->selmon->scroll_x += SCROLL_STEP;
    scrollclamp(s, s->selmon);
    arrange(s, s->selmon);
}

static void scrollright(WMState *s, const Arg *arg) {
    if (layouts[s->selmon->sellt].arrange != scroll) return;
    s->selmon->scroll_x -= SCROLL_STEP;
    scrollclamp(s, s->selmon);
    arrange(s, s->selmon);
}
```

Use the existing `Arg` convention. Ignore `arg` if the step is compile-time configured, or use `arg.i` for configurable increments.

Optional command:

```c
static void scrollcenter(WMState *s, const Arg *arg);
```

This should reveal the selected client centered in the viewport, but it is not required for the first working implementation.

### 6.2 Key choices

Do not reuse `Mod+h` and `Mod+l` unless the existing master-factor commands are removed or remapped. Recommended initial bindings:

```c
{ MODKEY, XK_Left,  scrollleft,  { .i = SCROLL_STEP } },
{ MODKEY, XK_Right, scrollright, { .i = SCROLL_STEP } },
```

Add layout selection:

```c
{ MODKEY, XK_s, setlayout, { .i = SCROLL_LAYOUT_INDEX } },
```

If `Mod+s` conflicts with sticky behavior, use another unused key and document it in the plan/README.

Keep existing `[T]`, `[M]`, and `[F]` layout selection behavior intact.

## 7. Mouse behavior

### 7.1 First implementation requirement

Existing `movemouse()` converts tiled clients to floating after movement. Preserve this behavior outside scrolling mode.

When the active layout is scrolling:

- A drag on a tiled client should not unexpectedly make it floating merely because the user is trying to navigate the strip.
- Horizontal drag may adjust `m.scroll_x`.
- Optional reorder behavior should be implemented only after basic scrolling is stable.

### 7.2 Safe initial mouse implementation

For the first pass, implement keyboard scrolling only and leave existing mouse move/resize behavior unchanged. This reduces risk in the synchronous pointer-grab event loop and isolates geometry bugs.

Then, as a separate follow-up phase, add horizontal drag scrolling to `movemouse()`:

```c
if (layout == scroll && !c->isfloating) {
    delta_x = ev.xmotion.x - start_x;
    m->scroll_x = original_scroll_x + delta_x;
    scrollclamp(s, m);
    arrange(s, m);
}
```

Do not mix reordering and viewport scrolling in the same first patch. Reordering requires careful list mutation and focus preservation.

## 8. Layout integration

### 8.1 Layout table

Add the scrolling layout after the existing layouts:

```c
static layouts: []Layout = {
    { "[T]", tile },
    { "[M]", monocle },
    { "[F]", floating },
    { "[S]", scroll },
};
```

Update `LAYOUT_COUNT` automatically through the existing `sizeof` expression.

### 8.2 Existing helper assumptions

Audit all code paths that compare layout function pointers:

- `restack()` checks for floating layout.
- `movemouse()` checks for floating layout.
- `resizemouse()` checks for floating layout.
- `togglefloating()` checks client state.

The new layout must not be treated as floating. Replace any overly broad condition with explicit `c.isfloating` checks where necessary.

### 8.3 `nexttiled()` and monocle

Do not change `nexttiled()` globally. The scroll layout should use a scroll-specific iterator or the same iterator with a separate collection pass.

Monocle must remain unchanged and continue to display all visible tiled clients in the same viewport.

## 8A. Direct libinput touchpad gesture backend

Implement a Linux-only direct libinput backend. Do not use X11/XInput2, libinput-gestures, libinput-tools, compositor APIs, or a desktop-environment gesture daemon.

### Backend and thread ownership

- Link directly against `libinput`, `libudev`, and `pthread`.
- Create the context with `libinput_udev_create_context()`.
- Assign the active seat with `libinput_udev_assign_seat()`, normally `seat0`.
- Run all libinput calls on one dedicated gesture thread.
- The gesture thread must never call Xlib or mutate `WMState`, `Monitor`, clients, focus, or layout state.
- The WM/X11 thread alone applies scroll deltas and calls `arrange()`.
- Direct access requires seat permissions/udev ACLs or logind/seatd integration; failure must disable gestures without disabling the WM.

### Host-owned lifecycle

Add native host functions in `src/host.c`:

```c
int gesture_backend_start(WMState *s);
void gesture_backend_request_stop(WMState *s);
void gesture_backend_join(WMState *s);
```

Add persistent fields to `WMState` in `src/wm.h`:

```c
pthread_t gesture_thread;
int gesture_thread_running;
int gesture_stop_pipe[2];
int gesture_event_pipe[2];
int gesture_backend_ready;
```

The gesture thread must be stopped and joined before its pipes, udev/libinput context, or reload-owned resources are destroyed. Do not pass JIT function pointers into the thread because the CMM image may be replaced during reload.

The preferred reload design keeps the host-owned gesture thread alive across `wm_entry()` reloads. If incompatible with the current host loop, stop/join before reload and restart afterward; never leave a thread executing against unloaded CMM code.

### Polling and dispatch

The gesture thread polls both descriptors:

```c
struct pollfd fds[2] = {
    { .fd = stop_fd,     .events = POLLIN },
    { .fd = libinput_fd, .events = POLLIN },
};
poll(fds, 2, -1);
if (fds[0].revents & POLLIN) break;
if (fds[1].revents & POLLIN) {
    libinput_dispatch(li);
    while ((event = libinput_get_event(li)) != NULL) {
        process_libinput_event(event);
        libinput_event_destroy(event);
    }
}
```

Handle `POLLERR`, `POLLHUP`, `POLLNVAL`, device removal, dispatch failure, and gesture cancellation. Cleanup must end any active gesture state.

### Three-finger horizontal recognition

Use libinput gesture swipe events where supported (`LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN`, `UPDATE`, `END`). Confirm the installed libinput API exposes finger count; if it does not, define and document a minimum supported libinput version rather than guessing from pointer events.

Recognizer rules:

- Accept exactly three fingers.
- Reject two-, four-, and five-finger gestures.
- Accumulate `dx`/`dy` from swipe updates.
- Reject the gesture when `abs(total_dy) > abs(total_dx) * vertical_ratio`.
- Ignore movement inside a configurable dead zone.
- Apply a sensitivity multiplier and clamp emitted deltas to `int32_t` range.
- Clear state on END, CANCEL, device removal, thread shutdown, or invalid finger count.

```c
typedef struct {
    int active;
    int valid;
    unsigned int fingers;
    double total_dx, total_dy;
    double pending_dx;
} SwipeState;
```

### Cross-thread command protocol

Use a fixed-size command smaller than `PIPE_BUF`:

```c
typedef struct {
    uint8_t type;       /* GESTURE_SCROLL_DELTA or GESTURE_CANCEL */
    int32_t delta_px;
} GestureCommand;
```

The gesture thread writes only commands. On `EAGAIN`, coalesce/drop intermediate motion updates; never block indefinitely. The WM thread drains the pipe nonblocking and coalesces pending deltas into one `arrange()` per event-loop pass.

Expose a host helper such as `rewm_drain_gesture_commands(WMState *)`, called from the CMM event loop. It must not block and must apply commands only when the selected monitor uses the scrolling layout:

```c
m := s.selmon;
if (m && layouts[m.sellt].arrange == scroll) {
    m.scroll_x -= delta_px; /* validate sign during testing */
    scrollclamp(s, m);
    arrange(s, m);
}
```

Do not call `XInitThreads()` as a substitute for ownership discipline; the design must keep all Xlib use on the WM thread.

## 9. Tags/workspaces and reload behavior

### Tag changes

When `view()`, `toggleview()`, `tagclient()`, or `toggletag()` changes the visible client set:

1. Focus a valid visible client.
2. Reset `scroll_x` to `0`, or preserve it only if the new strip still contains the previous selected client and the offset remains valid.
3. Clamp the offset using the new strip width.
4. Arrange the monitor.
5. Reveal the selected client if the selected layout is scrolling.

Recommended first implementation: reset to `0` on a changed tag view, then reveal the selected client.

### Reload

Do not reset `scroll_x` from `wm_entry()` or `arrange()`. Only initialize it in `createmon()` and modify it through scrolling/tag/geometry operations. This allows hot reload to retain the current viewport.

### Monitor geometry changes

In `updategeom()`/`configurenotify` handling:

- Recalculate monitor work geometry.
- Call `scrollclamp()` for each monitor.
- Arrange all affected monitors.

If monitor removal already migrates clients, ensure the destination monitor's scroll offset is clamped after migration.

## 10. EWMH and window visibility

No new EWMH protocol is required for the initial implementation.

Verify that:

- `_NET_ACTIVE_WINDOW` still tracks the focused client.
- `_NET_CLIENT_LIST` remains unchanged.
- Tagged clients remain managed even when physically outside the viewport.
- A physically off-screen client is not treated as unmapped or withdrawn.
- `showhide()` only hides clients for tag visibility, not because their logical strip position is outside the monitor viewport.

Do not call `XUnmapWindow()` solely for off-screen strip clients.

## 11. Implementation sequence

1. Add `scroll_x` and initialization in monitor creation.
2. Add scrolling constants and helper declarations.
3. Implement shared strip-client collection and strip-width calculation.
4. Implement `scrollclamp()`.
5. Implement `scroll()` layout and add `[S]` to the layout table.
6. Add keyboard scroll commands and keybindings.
7. Add focus reveal and integrate it with focus navigation.
8. Integrate offset reset/clamping with tag changes and monitor geometry changes.
9. Audit floating/fullscreen/restack/mouse assumptions.
10. Build and run syntax/JIT validation.
11. Only after the baseline works, consider mouse drag scrolling and strip reordering.

## 12. Validation plan

### Static/source validation

- Confirm every new function has a declaration before use, matching CMM syntax.
- Confirm `Monitor` fields are initialized in `createmon()`.
- Confirm the layout index used by keybindings matches the layout table.
- Confirm no helper counts floating, fullscreen, or invisible clients.
- Confirm all scroll arithmetic uses signed integers.
- Confirm offset clamping is called after every operation that changes strip width or monitor width.

### Build validation

Run:

```sh
gcc build.c -o nob
./nob clean
./nob
```

Then launch with the normal environment:

```sh
export REWM_PATH="$PWD/src"
export REWM_CFLAGS="-I/usr/include/freetype2"
./rewm
```

Expected result:

- CMM compilation succeeds.
- `wm_entry` loads successfully.
- Existing layouts still compile and select.
- Reload still works.

### Manual X11 smoke tests

With at least four test windows:

1. Select scrolling layout.
2. Verify the first clients appear side-by-side.
3. Scroll right until the last client is visible.
4. Scroll left back to the first client.
5. Focus next/previous repeatedly and verify automatic reveal.
6. Toggle floating on a client and verify it no longer contributes to strip width.
7. Toggle fullscreen and exit fullscreen.
8. Change tags and verify the offset is reset/clamped.
9. Resize the monitor/X display and verify no client geometry becomes invalid.
10. Reload the WM while scrolled and verify clients remain managed and the offset is not corrupted.
11. Kill the selected client and verify focus and strip width recover correctly.

### Regression checks

- Tile layout still respects `mfact`, `nmaster`, and gaps.
- Monocle still displays visible tiled clients.
- Floating layout still preserves floating positions.
- Existing mouse move/resize paths still work.
- Multi-monitor focus/tag movement does not dereference a missing monitor.

## 13. Non-goals for the first implementation

- Wayland support.
- New X11 protocols for viewport position.
- Vertical scrolling.
- Per-window variable-width rules.
- Automatic client reordering by drag.
- Animation or easing.
- Gesture recognition beyond direct three-finger horizontal swipe scrolling.
- Replacing the existing tag model with independent workspaces.

These can be added after the baseline layout has stable geometry, focus reveal, reload behavior, and regression coverage.

## 14. Expected files changed

Primary:

- `src/wm.cmm`
- `src/wm.h` for monitor scroll state and gesture-thread/pipe state
- `src/host.c` for the native libinput thread and command draining
- `build.c` for direct `libinput`, `libudev`, and `pthread` linkage

Documentation/tests, if appropriate:

- `README.md` for keybindings and layout selection
- `assets/roadmap.org` to mark the scrolling layout task complete

Not expected to change:

- `src/c2mir-lib.c`
